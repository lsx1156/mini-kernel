/**
 * @file    shmem_ipc.c
 * @brief   v2.6 双核共享内存 IPC —— Core0 侧实现
 *
 * 职责：
 *   · ctrl 控制块生命周期（init/start/stop/status）
 *   · Core1 镜像搬运（Flash LMA → bank1 别名 VMA）+ 拉起/复位
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
extern uint8_t       __core1_stack_top[];

/* ---- 跨文件符号 ---- */
extern void core1_worker_entry(void);                       /* core1_worker.c（.core1_text） */
extern void multicore_launch_core1_with_stack(void (*entry)(void),
                                              uint32_t *stack_bottom,
                                              unsigned int stack_size_bytes);
extern void multicore_reset_core1(void);

/* ---- SIO FIFO 直接寄存器（不引 SDK 头，沿用 hal_port.c 的寄存器直写风格） ---- */
#define SIO_BASE     0xD0000000u
#define SIO_FIFO_WR  (*(volatile uint32_t *)(SIO_BASE + 0x40u)) /* 写=推给对端 */
#define SIO_FIFO_RD  (*(volatile uint32_t *)(SIO_BASE + 0x44u)) /* 读=弹出对端消息 */
#define SIO_FIFO_ST  (*(volatile uint32_t *)(SIO_BASE + 0x50u))
#define FIFO_ST_VLD  1u     /* bit0: RX 有数据 */
#define FIFO_ST_RDY  2u     /* bit1: TX 可写（深度 8，很少满） */

#define IPC_DMB()    __asm volatile ("dmb" ::: "memory")
#define IPC_SEV()    __asm volatile ("sev")

/* Core1 栈：区域顶 8KB（留 256B 顶部余量给 SDK launch 桩） */
#define CORE1_STACK_SIZE 0x1F00u

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

static void fifo_flush_rx(void) {
    while ((SIO_FIFO_ST & FIFO_ST_VLD) != 0u) (void)SIO_FIFO_RD;
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

int ipc_start(void) {
    volatile ipc_ctrl_t *c = ipc_ctrl();
    ipc_init();
    if (c->state == IPC_STATE_RUNNING) return 0;      /* 已在运行，幂等 */

    /* 1. 清 Core1 bss（NOLOAD 段 crt0 不搬不清） */
    for (uint32_t *p = (uint32_t *)__core1_bss_start__;
         p < (uint32_t *)__core1_bss_end__; p++) *p = 0u;

    /* 2. 拷镜像：Flash LMA → bank1 别名 VMA（代码已按 VMA 链接，无需重定位） */
    uint32_t len = (uint32_t)(__core1_load_end__ - __core1_load_start__);
    memcpy(__core1_start__, __core1_load_start__, len);

    /* 3. ctrl 运行统计复位 */
    c->frame_ctr = 0u; c->c1_hb = 0u; c->c1_frames = 0u;
    c->c1_err = 0u; c->c0_drops = 0u; c->last_frame_done = 0u;

    /* 4. 清残留 FIFO（上次 mcore demo / 崩溃残留） */
    fifo_flush_rx();

    /* 5. 拉起 Core1（与 mcore demo 互斥：先复位再启动） */
    multicore_reset_core1();
    multicore_launch_core1_with_stack(core1_worker_entry,
        (uint32_t *)(void *)(__core1_stack_top - CORE1_STACK_SIZE),
        CORE1_STACK_SIZE);

    /* 6. 等 HELLO（bootrom 拉起 + 镜像执行，µs~ms 级；200ms 兜底） */
    uint32_t t0 = time_us_32();
    while ((uint32_t)(time_us_32() - t0) < 200000u) {
        uint32_t m;
        if (fifo_pop(&m) && IPC_MSG_TYPE(m) == IPC_MSG_HELLO) {
            c->state = IPC_STATE_RUNNING;
            return 0;
        }
    }
    c->state = IPC_STATE_FAULT;
    return -1;   /* Core1 没起来：镜像搬运/启动失败 */
}

int ipc_stop(void) {
    volatile ipc_ctrl_t *c = ipc_ctrl();
    if (c->state == IPC_STATE_RUNNING) multicore_reset_core1();
    fifo_flush_rx();
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
