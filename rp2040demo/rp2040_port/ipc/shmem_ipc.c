/**
 * @file    shmem_ipc.c
 * @brief   v2.6 双核共享内存 IPC —— Core0 侧实现
 *
 * 职责：
 *   · ctrl 控制块生命周期（init/start/stop/status）
 *   · Core1 镜像搬运（Flash LMA → 条带 RAM CORE1 区 VMA）+ 拉起/复位
 *   · FIFO 发送/等待 + 乒乓帧 API + bench/selftest
 *
 * 本文件运行在 Core0（Flash XIP），可用 libc/除法；
 * Core1 侧代码见 core1_worker.c（禁止 libc/除法/中断）。
 */
#include "shmem_ipc.h"
#include "pico/time.h"        /* time_us_32 */
#include <string.h>
#include <stdio.h>            /* snprintf（诊断行格式化） */

/* ---- linker 符号（memmap_ipc.ld） ---- */
extern const uint8_t __core1_load_start__[];
extern const uint8_t __core1_load_end__[];
extern uint8_t       __core1_start__[];
extern uint8_t       __core1_bss_start__[];
extern uint8_t       __core1_bss_end__[];

/* ---- 跨文件符号 ---- */
extern void core1_worker_entry(void);                       /* core1_worker.c（.core1_text） */
extern void multicore_launch_core1_with_stack(void (*entry)(void),
                                              uint32_t *stack_bottom,
                                              unsigned int stack_size_bytes);
extern void multicore_launch_core1(void (*entry)(void));
extern void multicore_reset_core1(void);
extern void hal_diag_puts(const char *s);
extern void hal_diag_put_u32(uint32_t v);
extern void hal_diag_putc(char c);
extern void busy_wait_us_32(uint32_t us);

/* ---- SIO FIFO 直接寄存器（不引 SDK 头，沿用 hal_port.c 的寄存器直写风格） ----
 * 【v2.6.6 根因修复】FIFO_WR/FIFO_RD 偏移笔误：0x40/0x44 实为
 * GPIO_HI_OE / GPIO_HI_OE_SET（QSPI SIO 引脚寄存器，读恒 0）——
 * 真实 FIFO 寄存器是 0x54/0x58（手册 §SIO）。该笔误导致：
 *   · pop 永远读 0（读到 OE_SET 别名）
 *   · 真 FIFO_RD 从未被读 → bootrom park-0 永留队列 → VLD 永不消
 *   · 旧无界 drain while(VLD)pop = 死循环（flash67 无声卡死根因）
 *   · push 写进 GPIO_HI_OE（未爆仅因 QSPI 引脚复用给 SSI） */
#define SIO_BASE     0xD0000000u
#define SIO_FIFO_WR  (*(volatile uint32_t *)(SIO_BASE + 0x54u)) /* 写=推给对端 */
#define SIO_FIFO_RD  (*(volatile uint32_t *)(SIO_BASE + 0x58u)) /* 读=弹出对端消息 */
#define SIO_FIFO_ST  (*(volatile uint32_t *)(SIO_BASE + 0x50u))
#define FIFO_ST_VLD  1u     /* bit0: RX 有数据 */
#define FIFO_ST_RDY  2u     /* bit1: TX 可写（深度 8，很少满） */

#define IPC_DMB()    __asm volatile ("dmb" ::: "memory")
#define IPC_SEV()    __asm volatile ("sev")

/* ---- 探针用寄存器 ----
 * 【v2.6.5 修正】PSM 基址 = 0x40010000（0x40000000 是 SYSINFO！之前
 * 误读 SYSINFO_PLATFORM（恒 0x2 = ASIC 标志），导致"XOSC 断电"假象
 * 与"PSM 写入失效"误判。换板复现后实锤此为探针 bug，非芯片问题。） */
#define SIO_CPUID       (*(volatile uint32_t *)(SIO_BASE + 0x00u))
#define PSM_FRCE_OFF    (*(volatile uint32_t *)0x40010004u)         /* RO/RC */
#define PSM_FRCE_OFF_ST (*(volatile uint32_t *)0x40012004u)         /* set 别名 */
#define PSM_FRCE_OFF_CL (*(volatile uint32_t *)0x40013004u)         /* clr 别名 */
#define PSM_PROC1_BIT   0x00010000u                                  /* FRCE_OFF_PROC1 */

volatile ipc_ctrl_t *ipc_ctrl(void) { return (volatile ipc_ctrl_t *)IPC_CTRL_BASE; }

/* ================================================================
 * 低层 FIFO 原语
 * ================================================================ */
static void fifo_push(uint32_t msg) {
    while ((SIO_FIFO_ST & FIFO_ST_RDY) == 0u) { /* 深度 8，Core1 消费快，几乎不等 */ }
    SIO_FIFO_WR = msg;
    IPC_SEV();   /* 唤醒对端 WFE */
}

static int fifo_pop(uint32_t *out) {
    if ((SIO_FIFO_ST & FIFO_ST_VLD) == 0u) return 0;
    *out = SIO_FIFO_RD;
    IPC_DMB();   /* 先取通知再读数据：保证能看到对端 push 前的 SRAM 写 */
    return 1;
}

/* 【v2.6.2】有界排空：FIFO 深度只有 8，pop 超过 16 次仍有数据 =
 * RX FIFO 有活跃推手或 VLD 标志卡死（SIO FIFO 无清除寄存器）。
 * 旧的无界 while(VLD) pop 在这种情况下永久自旋且零输出——
 * 板上两次实测均卡死在 mirror verified 之后、s3 打印之前
 * （反汇编 1001847e-1001848e 即该循环）。绝不再无界等。 */
static uint32_t fifo_drain_bounded(uint32_t max_pops) {
    uint32_t n = 0;
    while ((SIO_FIFO_ST & FIFO_ST_VLD) != 0u && n < max_pops) {
        (void)SIO_FIFO_RD;
        n++;
    }
    return n;
}

/* ================================================================
 * 生命周期
 * ================================================================ */
int ipc_init(void) {
    volatile ipc_ctrl_t *c = ipc_ctrl();
    if (c->magic != IPC_MAGIC) {
        volatile uint32_t *p = (volatile uint32_t *)IPC_CTRL_BASE;
        for (uint32_t i = 0; i < 256u / 4u; i++) p[i] = 0u;  /* 只清 256B 头部 */
        c->magic = IPC_MAGIC;
    }
    if (c->state == IPC_STATE_UNINIT) c->state = IPC_STATE_READY;
    return 0;
}

/* Flash 侧跳板（v2.6.1-fix）：
 *   launch 形态与 mcore 成功路径完全一致 —— multicore_launch_core1(Flash 函数)
 *   + SDK 默认 .stack1 栈（SCRATCH_X 4KB）。Core1 经此立即进入 bank1 SRAM 镜像，
 *   跳板只执行 2 条指令，XIP 争用可忽略。
 *   （此前直接把 SRAM 镜像地址作为 entry + 自定义栈，launch 握手死等。） */
static void __attribute__((noinline)) core1_flash_trampoline(void) {
    core1_worker_entry();   /* 不返回（noreturn 循环） */
}

/* 分阶段诊断：卡死时能从串口最后一行直接定位阶段 */
static void ipc_diag(uint32_t stage, const char *msg) {
    hal_diag_puts("[IPC s"); hal_diag_put_u32(stage); hal_diag_puts("] ");
    hal_diag_puts(msg); hal_diag_puts("\r\n");
}

/* 【v2.6.6】verbose 版：IPC_VERBOSE_DIAG=0 时编译期整体消除
 * （含字符串常量，不占 Flash），只留故障路径的 ipc_diag/ipc_diag_hex */
#if IPC_VERBOSE_DIAG
  #define ipc_diagv     ipc_diag
  #define ipc_diagv_hex ipc_diag_hex
#else
  #define ipc_diagv(stage, msg)     ((void)(stage), (void)(msg))
  #define ipc_diagv_hex(tag, v)     ((void)(tag), (void)(v))
#endif

/* 纯 UART 直写十六进制（不经 snprintf——其内部可能依赖 memcpy 跳表） */
static void ipc_diag_hex(const char *tag, uint32_t v) {
    const char hex[] = "0123456789ABCDEF";
    hal_diag_puts(tag);
    hal_diag_puts("0x");
    for (int i = 28; i >= 0; i -= 4) hal_diag_putc(hex[(v >> i) & 0xFu]);
    hal_diag_puts("\r\n");
}

/* ================================================================
 * 【v2.6.3】FIFO 深度探针 —— 定位"排空后 VLD 仍置位"的活跃推手
 *
 * 证据链（flash69 实测）：drain 16 pop 后 FIFO_ST=0x3 —— WOF/ROE 均 0，
 * 说明每次 pop 都读到了真实数据（若读空 FIFO，bit3 ROE 必置位）。
 * 即：有东西在持续 push core0 的 RX。core0 侧无 FIFO_WR 写入者
 * （全库 grep 证实），推手只能在 Core1（PSM 复位后 = bootrom）。
 *
 * 探针四连问：
 *   Q1 CPUID/PSM —— 确认本核身份 + core1 电源状态
 *   Q2 pop 字面值 —— 全 0 = bootrom ready-0 循环；0x30 = worker 残留；
 *                    随机值 = core1 乱飞
 *   Q3 静置 1ms 再 drain —— refill>0 = 推手仍然活跃（非残留）
 *   Q4 保持 core1 断电 2ms 再 drain —— VLD 终于清零 = 推手确在 core1；
 *                    断电仍推 = SIO 硬件级异常（硅缺陷，FIFO 不可用）
 * ================================================================ */
void ipc_fifo_probe(void) {
    uint32_t m;
    ipc_diag_hex("CPUID        =", SIO_CPUID);
    ipc_diag_hex("PSM frce_off =", PSM_FRCE_OFF);
    ipc_diag_hex("FIFO_ST      =", SIO_FIFO_ST);
    for (int i = 0; i < 4; i++) {
        if (fifo_pop(&m)) {
            hal_diag_puts("[PROBE]   pop[");
            hal_diag_putc((char)('0' + i));
            hal_diag_puts("] =");
            ipc_diag_hex("", m);
        } else {
            hal_diag_puts("[PROBE]   (fifo empty)\r\n");
            break;
        }
    }
    ipc_diag_hex("FIFO_ST post =", SIO_FIFO_ST);

    /* W1C 错误位实验（WOF/ROE 文档语义为写 1 清除；写只读位无副作用） */
    SIO_FIFO_ST = 0xCu;
    ipc_diag_hex("after W1C    =", SIO_FIFO_ST);

    /* Q3：静置 1ms 再探 —— 判别"活跃推手"与"静态残留" */
    busy_wait_us_32(1000);
    ipc_diag_hex("refill@1ms   =", fifo_drain_bounded(32u));

    /* Q3b.【v2.6.5】roundtrip：push 魔数，看 bootrom park 是否 echo。
     *     bootrom mailbox 协议本身就是"收一字 echo 一字"（SDK launch
     *     握手 cmd==response 依赖此）。echo 回 0xDEADBEEF = 数据通路
     *     活着、bootrom 在正常应答 → 无限零另有来源；
     *     无 echo = core1/bootrom 侧不响应。 */
    {
        uint32_t magic = 0xDEADBEEFu;
        SIO_FIFO_WR = magic;
        IPC_SEV();
        uint32_t t0 = time_us_32();
        uint32_t got = 0, n = 0;
        while ((uint32_t)(time_us_32() - t0) < 5000u && n < 16u) {
            uint32_t m2;
            if (fifo_pop(&m2)) {
                if (m2 == magic) { got = 1u; break; }
                n++;
            }
        }
        hal_diag_puts("[PROBE] roundtrip ");
        hal_diag_puts(got ? "ECHO OK" : "NO ECHO");
        hal_diag_puts(" (skipped ");
        hal_diag_put_u32(n);
        hal_diag_puts(" words)\r\n");
    }

    /* Q4：真·core1 断电对照（v2.6.5 修正 PSM 基址后首次有效） */
    PSM_FRCE_OFF_ST = PSM_PROC1_BIT;
    {
        uint32_t t0 = time_us_32();
        while ((PSM_FRCE_OFF & PSM_PROC1_BIT) == 0u) {
            if ((uint32_t)(time_us_32() - t0) > 10000u) break;  /* 10ms 放弃 */
        }
    }
    ipc_diag_hex("PSM@off      =", PSM_FRCE_OFF);  /* 期望 0x00010000：PROC1 位回读=断电生效 */
    busy_wait_us_32(3000);                          /* 断电保持 3ms */
    ipc_diag_hex("pops@voff    =", fifo_drain_bounded(64u));
    ipc_diag_hex("FIFO_ST@voff =", SIO_FIFO_ST);   /* core1 已断电：VLD 应=0 */
    if ((SIO_FIFO_ST & FIFO_ST_VLD) == 0u) {
        /* 故意读空一次：ROE(bit3) 必须置位 —— 验证状态寄存器硬件语义 */
        (void)SIO_FIFO_RD;
        ipc_diag_hex("ROE@overread =", SIO_FIFO_ST);
    }
    PSM_FRCE_OFF_CL = PSM_PROC1_BIT;   /* 释放：bootrom 重新 park（会推 ready-0） */
    busy_wait_us_32(1000);
    (void)fifo_drain_bounded(32u);     /* 清掉 park 后的静态残留 */
    ipc_diag_hex("FIFO_ST final =", SIO_FIFO_ST);
}

/* 【v2.6.4】启动期 FIFO/PSM 快照（TTL/UART0 输出）。
 * 布点：A=hal_diag_init 尾（最早，USB/内核未初始化）
 *       B=stdio_init_all 后（USB 刚枚举）
 *       C=hal_bootlog_end（sched_start 前最后一刻）
 * 用途：判定 SIO FIFO 楔死是"上电即坏"（硬件）还是"启动过程弄坏"（软件）。
 * flash71 实测：热复位后 FIFO_ST=0x3 楔死 + PSM set+回读失效（0x2 悬挂），
 * 但全部观测均发生在软复位之后——本快照补齐"真冷上电第一时间"数据。 */
void ipc_boot_snap(const char *tag) {
#if IPC_VERBOSE_DIAG
    hal_diag_puts("[SNAP ");
    hal_diag_puts(tag);
    hal_diag_puts("] FIFO_ST=");
    ipc_diag_hex("", SIO_FIFO_ST);
    hal_diag_puts("[SNAP ");
    hal_diag_puts(tag);
    hal_diag_puts("] PSM_OFF=");
    ipc_diag_hex("", PSM_FRCE_OFF);
#else
    (void)tag;   /* 静默：正常启动不刷 TTL（v2.6.6 排障期使命已完成） */
#endif
}

/* 【v2.6.1-fix】手工 volatile 字拷贝，绕过 libc memcpy。
 *   __aeabi_memcpy 走 aeabi_mem_funcs 跳表（.data 里由 preinit 构造器
 *   __aeabi_mem_init → rom_funcs_lookup 把 ROM 查找码 'MC' 换成真函数
 *   地址）。静态链完好但板子实测死在 memcpy——直接字拷贝消除该变量。 */
static void ipc_word_copy(volatile uint32_t *dst, const uint32_t *src, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) dst[i] = src[i];
}

int ipc_start(void) {
    volatile ipc_ctrl_t *c = ipc_ctrl();
    /* 【v2.6.1】先打印再动手：任何一步打爆内存时，串口最后一行直接暴露卡点 */
    if (c->state == IPC_STATE_RUNNING) return 0;      /* 已在运行，幂等 */

    ipc_diagv(0, "ipc_init (write ctrl @0x20018000)");
    ipc_init();

    /* 1. 清 Core1 bss（NOLOAD 段 crt0 不搬不清） */
    ipc_diagv(1, "clear core1 bss");
    for (uint32_t *p = (uint32_t *)__core1_bss_start__;
         p < (uint32_t *)__core1_bss_end__; p++) *p = 0u;

    /* 2. 拷镜像：Flash LMA → 条带 RAM VMA（代码已按 VMA 链接，无需重定位） */
    {
        volatile uint32_t *tbl = (volatile uint32_t *)0x20001350u;
        ipc_diagv_hex("[IPC s2a] aeabi.memcpy fn=", tbl[1]);
    }
    ipc_diagv(2, "copy mirror flash->0x20019000 (word loop)");
    uint32_t len = (uint32_t)(__core1_load_end__ - __core1_load_start__);
    ipc_word_copy((volatile uint32_t *)(void *)__core1_start__,
                  (const uint32_t *)(const void *)__core1_load_start__,
                  len / 4u);
    /* 拷贝校验：首/末字对比（尾部长度非 4 倍数时末字回退到最后一个全字） */
    {
        volatile uint32_t *d = (volatile uint32_t *)(void *)__core1_start__;
        const uint32_t *s = (const uint32_t *)(const void *)__core1_load_start__;
        uint32_t last = (len / 4u) - 1u;
        if (d[0] != s[0] || d[last] != s[last]) {
            ipc_diag(2, "MIRROR VERIFY FAIL");
            c->state = IPC_STATE_FAULT;
            return -3;
        }
        ipc_diagv(2, "mirror verified (first/last word match)");
    }

    /* 2b.【v2.6.3】reset 前快照（verbose：探针排障用，不影响 3b 的统一排空） */
#if IPC_VERBOSE_DIAG
    {
        uint32_t m;
        ipc_diag_hex("[IPC s2] FIFO_ST pre-reset =", SIO_FIFO_ST);
        if (fifo_pop(&m)) ipc_diag_hex("[IPC s2] first word        =", m);
    }
#endif

    /* 3.【v2.6.2 根因修复】先复位 Core1，再碰任何 FIFO。
     *    旧顺序是 flush→reset：若 RX FIFO 有活跃推手（残留 worker）或
     *    VLD 标志卡死，无界 flush 永久自旋（板上实测卡点）。
     *    PSM 断电复位 Core1：任何在跑的推手立即消失；bootrom 重新 park
     *    （异步 drain 自己 mailbox + push 0 完成信号），等 1ms 后 FIFO 里
     *    只剩静态残留，有界排空即可清干净，再进 launch 握手不会错位。 */
    ipc_diagv(3, "reset core1 FIRST (kill any pusher)");
    multicore_reset_core1();
    busy_wait_us_32(1000);

    /* 3b. 有界排空（正常：pops<=9，深度 8 + bootrom 的 reset-done 0）。
     *     排空后 VLD 仍置位 = FIFO 异常（活跃推手/标志卡死），SDK launch
     *     内部的 multicore_fifo_drain 同样无界，硬闯必死等 —— 探针定位
     *     后报故障返回，绝不死等。 */
    {
        uint32_t pops = fifo_drain_bounded(16u);
        ipc_diagv_hex("[IPC s3] drain pops  =", pops);
        ipc_diagv_hex("[IPC s3] FIFO_ST post=", SIO_FIFO_ST);
        if ((SIO_FIFO_ST & FIFO_ST_VLD) != 0u) {
            ipc_diag(9, "FIFO STUCK (VLD never clears) -> probing...");
            ipc_fifo_probe();
            ipc_diag(9, "FIFO STUCK -> FAULT (probe above identifies pusher)");
            c->state = IPC_STATE_FAULT;
            return -5;
        }
    }

    /* 3c. ctrl 运行统计复位 */
    c->frame_ctr = 0u; c->c1_hb = 0u; c->c1_frames = 0u;
    c->c1_err = 0u; c->c0_drops = 0u; c->last_frame_done = 0u;

    /* 4. 拉起 Core1：Flash 跳板 + SDK 默认 .stack1（= mcore demo 同形态） */
    ipc_diagv(4, "launch core1 (flash trampoline + default stack1)");
    multicore_launch_core1(core1_flash_trampoline);

    /* 5. 等 HELLO（bootrom 拉起 + 镜像执行，µs~ms 级；200ms 兜底） */
    ipc_diagv(5, "wait HELLO (<=200ms)");
    uint32_t t0 = time_us_32();
    while ((uint32_t)(time_us_32() - t0) < 200000u) {
        uint32_t m;
        if (fifo_pop(&m) && IPC_MSG_TYPE(m) == IPC_MSG_HELLO) {
            ipc_diagv(6, "core1 RUNNING (HELLO received)");
            c->state = IPC_STATE_RUNNING;
            return 0;
        }
    }
    ipc_diag(9, "TIMEOUT: no HELLO in 200ms");
    c->state = IPC_STATE_FAULT;
    return -1;   /* Core1 没起来：镜像搬运/启动失败 */
}

int ipc_stop(void) {
    volatile ipc_ctrl_t *c = ipc_ctrl();
    if (c->state == IPC_STATE_RUNNING) multicore_reset_core1();
    (void)fifo_drain_bounded(16u);
    c->state = IPC_STATE_READY;
    return 0;
}

int ipc_is_running(void) { return ipc_ctrl()->state == IPC_STATE_RUNNING; }

/* ================================================================
 * 帧协议
 * ================================================================ */
uint32_t ipc_frame_new(void) {
    volatile ipc_ctrl_t *c = ipc_ctrl();
    c->frame_ctr++;                       /* 单字自增，24bit 自然回绕 */
    return c->frame_ctr & 0xFFFFFFu;
}

int ipc_frame_post(uint32_t n, uint32_t fid) {
    if (!ipc_is_running() || n > 1u) return -1;
    IPC_DMB();                            /* 先写数据后发通知（跨核程序序） */
    fifo_push(IPC_MSG_MAKE(IPC_MSG_READY, fid, n));
    return 0;
}

int ipc_wait_done(uint32_t fid, uint32_t timeout_ms, uint32_t *out_msg) {
    volatile ipc_ctrl_t *c = ipc_ctrl();
    uint32_t t0 = time_us_32();
    uint32_t limit = timeout_ms * 1000u;
    for (;;) {
        uint32_t m;
        if (fifo_pop(&m)) {
            uint32_t t = IPC_MSG_TYPE(m);
            if (t == IPC_MSG_DONE && IPC_MSG_FID(m) == (fid & 0xFFFFFFu)) {
                if (out_msg) *out_msg = m;
                return 0;
            }
            if (t == IPC_MSG_FAULT) { if (out_msg) *out_msg = m; return -2; }
            /* 陈旧 DONE / 重复 HELLO：吞掉继续等 */
        }
        if ((uint32_t)(time_us_32() - t0) > limit) {
            c->c0_drops++;
            return -1;                    /* 超时：Core1 卡死或消息丢失 */
        }
    }
}

int32_t ipc_send_frame(uint32_t n, const void *src, uint32_t bytes) {
    if (!src || bytes == 0u || bytes > IPC_BUF_BYTES || n > 1u) return -3;
    uint32_t fid = ipc_frame_new();
    volatile uint32_t *in = IPC_IN(n);
    uint32_t words = bytes / 4u;
    for (uint32_t i = 0; i < words; i++) in[i] = ((const uint32_t *)src)[i];
    for (uint32_t i = words * 4u; i < bytes; i++) {       /* 尾部非对齐字节 */
        volatile uint8_t *b = (volatile uint8_t *)in;
        b[i] = ((const uint8_t *)src)[i];
    }
    return (ipc_frame_post(n, fid) == 0) ? (int32_t)fid : -4;
}

/* ================================================================
 * bench / selftest（Core0 侧同算校验，Core1 无需感知）
 * ================================================================ */
/* LCG 填充（无除法）：seed → 16KB 伪随机 */
static void fill_pattern(volatile uint32_t *buf, uint32_t seed) {
    uint32_t x = seed | 1u;
    for (uint32_t i = 0; i < IPC_BUF_WORDS; i++) {
        x = x * 1664525u + 1013904223u;
        buf[i] = x;
    }
}

/* 期望输出：out[i]=in[i]+i，末字=XOR 全体折叠（与 core1_worker.c 严格一致） */
static int verify_out(volatile uint32_t *in, volatile uint32_t *out, uint32_t seed) {
    (void)in;
    uint32_t x = seed | 1u, fold = 0u;
    for (uint32_t i = 0; i < IPC_BUF_WORDS; i++) {
        x = x * 1664525u + 1013904223u;
        uint32_t expect = (i == IPC_BUF_WORDS - 1u) ? (fold ^ x) : (x + i);
        if (out[i] != expect) return 0;
        fold ^= x;
    }
    return 1;
}

int ipc_bench(uint32_t n_iters, ipc_bench_t *out, ipc_emit_t emit) {
    volatile ipc_ctrl_t *c = ipc_ctrl();
    if (!ipc_is_running()) return -1;
    if (n_iters == 0u) n_iters = 16u;

    uint32_t ok = 0u, sum = 0u, mn = 0xFFFFFFFFu, mx = 0u;
    (void)c;

    for (uint32_t i = 0; i < n_iters; i++) {
        uint32_t n = i & 1u;              /* A/B 交替 = 真乒乓路径 */
        uint32_t seed = 0x12340000u + i;
        fill_pattern(IPC_IN(n), seed);

        uint32_t fid = ipc_frame_new();
        uint32_t t0 = time_us_32();
        if (ipc_frame_post(n, fid) != 0) break;
        uint32_t msg;
        if (ipc_wait_done(fid, 1000u, &msg) != 0) break;
        uint32_t dt = (uint32_t)(time_us_32() - t0);

        if (!verify_out(IPC_IN(n), IPC_OUT(n), seed)) { c->c0_drops++; continue; }
        ok++; sum += dt;
        if (dt < mn) mn = dt;
        if (dt > mx) mx = dt;
        if (emit && (i == 0u || i == n_iters - 1u)) {
            char line[64];
            snprintf(line, sizeof(line), "  frame %u/%u  n=%c  %u us\r\n",
                     (unsigned)(i + 1u), (unsigned)n_iters,
                     n ? 'B' : 'A', (unsigned)dt);
            emit(line);
        }
    }
    if (out) {
        out->iters = n_iters; out->ok = ok;
        out->min_us = (ok ? mn : 0u); out->max_us = (ok ? mx : 0u);
        out->avg_us = ok ? (sum + ok / 2u) / ok : 0u;
        /* 每帧双向搬运 16K+16K=32KB */
        out->kbps = (ok && out->avg_us) ? (32768u * 1000u) / out->avg_us : 0u;
    }
    return (ok == n_iters) ? 0 : -2;
}

int ipc_selftest(ipc_emit_t emit) {
    volatile ipc_ctrl_t *c = ipc_ctrl();
    if (!ipc_is_running()) return -1;
    int fails = 0;
    char line[80];

    /* 阶段 1：心跳存活（c1_hb 持续自增） */
    uint32_t hb0 = c->c1_hb;
    uint32_t t0 = time_us_32();
    while ((uint32_t)(time_us_32() - t0) < 5000u) { }   /* 忙等 5ms */
    uint32_t hb1 = c->c1_hb;
    if (emit) {
        snprintf(line, sizeof(line), "  [1/3] heartbeat %u -> %u ... %s\r\n",
                 (unsigned)hb0, (unsigned)hb1, (hb1 > hb0) ? "PASS" : "FAIL");
        emit(line);
    }
    if (hb1 <= hb0) fails++;

    /* 阶段 2：单帧（IN-A → OUT-A 校验） */
    uint32_t seed = 0xC0FFEE01u;
    fill_pattern(IPC_IN(0), seed);
    uint32_t fid = ipc_frame_new();
    int r = ipc_frame_post(0, fid) == 0 ? ipc_wait_done(fid, 500u, NULL) : -9;
    int pass2 = (r == 0) && verify_out(IPC_IN(0), IPC_OUT(0), seed);
    if (emit) {
        snprintf(line, sizeof(line), "  [2/3] single frame A ... %s\r\n",
                 pass2 ? "PASS" : "FAIL");
        emit(line);
    }
    if (!pass2) fails++;

    /* 阶段 3：4 帧 A/B 交替乒乓 */
    int pass3 = 1;
    for (uint32_t i = 0; i < 4u; i++) {
        uint32_t n = i & 1u;
        uint32_t sd = 0x5A5A0000u + i;
        fill_pattern(IPC_IN(n), sd);
        uint32_t f = ipc_frame_new();
        if (ipc_frame_post(n, f) != 0 || ipc_wait_done(f, 500u, NULL) != 0 ||
            !verify_out(IPC_IN(n), IPC_OUT(n), sd)) { pass3 = 0; break; }
    }
    if (emit) {
        snprintf(line, sizeof(line), "  [3/3] ping-pong A/B x4 ... %s\r\n",
                 pass3 ? "PASS" : "FAIL");
        emit(line);
    }
    if (!pass3) fails++;

    if (emit) {
        snprintf(line, sizeof(line),
                 "  stats: frames=%u drops=%u c1_err=%u -> %s\r\n",
                 (unsigned)c->c1_frames, (unsigned)c->c0_drops,
                 (unsigned)c->c1_err, fails ? "SELFTEST FAIL" : "SELFTEST PASS");
        emit(line);
    }
    return fails;
}
