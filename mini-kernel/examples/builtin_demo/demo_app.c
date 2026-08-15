/**
 * @file    demo_app.c
 * @brief   Mini Kernel 演示应用：多任务调度 + LED + 内存 + 任务控制
 *
 * 演示清单：
 *   1. task_led        — LED 周期性闪烁（GPIO25，RP2040 板载）
 *   2. task_heartbeat  — 串口心跳日志，打印 tick / 堆空闲 / 最大空闲块
 *   3. task_mem_stress — 定时做 malloc/free 压力测试，验证零碎片
 *   4. task_ctrl       — 定时挂起/恢复 LED 任务，演示任务控制 API
 *   5. 启动时打印任务列表、配置宏等系统信息
 */
#include "task.h"
#include "sched.h"
#include "mem.h"
#include "hal_interface.h"
#include "os_config.h"
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* Pico SDK / TinyUSB 前向声明（demo_app 模块无 SDK 头文件路径） */
#define PICO_ERROR_TIMEOUT ((int)((unsigned int)-1))
extern int  getchar_timeout_us(uint32_t timeout_us);
extern void tud_task_ext(uint32_t timeout_ms, int in_isr);
extern bool tud_cdc_n_available(uint8_t itf);
extern uint32_t tud_cdc_n_read(uint8_t itf, void *buffer, uint32_t bufsize);

/* k_version 原型在 kernel.h（用户态头），内核内部仅前向声明 */
extern const char *k_version(void);

/* demo_app_init 由 kernel.c 调用，此处提供原型以满足 -Wmissing-prototypes */
void demo_app_init(void);
/* shell_start 由 shell.c 提供（OS_CFG_SHELL=1 时创建交互 shell 任务）*/
extern void shell_start(void);
/* fatfs_init_and_mount 由 fatfs_api.h 提供（v2.2，首次启动自动格式化 FAT16）*/
#if OS_CFG_FATFS
#include "fatfs_api.h"
#endif

#if OS_CFG_DEMO_APP

/* RP2040 板载 LED 引脚 */
#define DEMO_LED_PIN        25

/* 演示任务句柄（供任务控制用） */
static tcb_t *g_demo_led_task      = NULL;
static tcb_t *g_demo_heartbeat_task = NULL;

/* ================================================================
 * 辅助：串口字符串打印（避免依赖未实现的 k_printf 完整版本）
 * ================================================================ */
static void demo_puts(const char *s) {
    while (*s) hal_console_putc(*s++);
}

static void demo_put_uint32(uint32_t v) {
    char buf[16];
    int i = 0;
    if (v == 0) {
        hal_console_putc('0');
        return;
    }
    while (v > 0) {
        buf[i++] = '0' + (v % 10);
        v /= 10;
    }
    while (i > 0) hal_console_putc(buf[--i]);
}

static void demo_put_hex32(uint32_t v) {
    const char hex[] = "0123456789ABCDEF";
    hal_console_putc('0'); hal_console_putc('x');
    for (int i = 7; i >= 0; i--) {
        hal_console_putc(hex[(v >> (i * 4)) & 0xF]);
    }
}

/* ================================================================
 * 辅助：打印任务列表快照
 * ================================================================ */
static void demo_dump_task_list(void) {
    demo_puts("\r\n--- Task List ---\r\n");
    demo_puts("ID  Name        State     TicksLeft  StackBase  StackSize\r\n");
    for (int i = 0; i < OS_CFG_MAX_TASKS; i++) {
        tcb_t *t = g_task_pool[i];
        if (!t) continue;
        demo_put_uint32(t->id);
        demo_puts("   ");
        int pad = 12 - (int)strlen(t->name);
        if (pad < 0) pad = 0;
        demo_puts(t->name);
        for (int k = 0; k < pad; k++) hal_console_putc(' ');
        switch (t->state) {
            case TASK_STATE_READY:   demo_puts("READY    "); break;
            case TASK_STATE_RUNNING: demo_puts("RUNNING  "); break;
            case TASK_STATE_SLEEP:   demo_puts("SLEEP    "); break;
            case TASK_STATE_SUSPEND: demo_puts("SUSPEND  "); break;
            case TASK_STATE_DEAD:    demo_puts("DEAD     "); break;
            default:                 demo_puts("UNKNOWN  "); break;
        }
        demo_put_uint32(t->ticks_to_sleep);
        hal_console_putc(' ');
        demo_put_hex32((uint32_t)(uintptr_t)t->stack_base);
        hal_console_putc(' ');
        demo_put_uint32(t->stack_size);
        demo_puts("\r\n");
    }
    demo_puts("-----------------\r\n\r\n");
}

/* ================================================================
 * 演示任务 1：LED 闪烁（RP2040 板载 GPIO25）
 *
 * 【实现说明】：
 *   为了避免 OS_CFG_PERIPH_SERVICE 开关影响 LED 心跳可见性，
 *   用 SIO 直接写寄存器（main() 开头 gpio_init 已把 GPIO25 配成 SIO 推挽），
 *   这样无论 PERIPH_SERVICE 开/关，task_led 都会稳定打出 500ms 心跳闪。
 * ================================================================ */
static void task_led(void *arg) {
    (void)arg;

    /* GPIO25 操作基址与掩码（与 main() 初始化的引脚一致） */
    register const uint32_t SIO_BASE = 0xD0000000u;
    register const uint32_t MASK25   = 0x02000000u;
    uint8_t on = 0;

    demo_puts("[LED   ] Task started, blink every 500ms\r\n");

    while (1) {
        on = !on;
        if (on) {
            *(volatile uint32_t *)(SIO_BASE + 0x014) = MASK25;   /* OUT_SET */
        } else {
            *(volatile uint32_t *)(SIO_BASE + 0x018) = MASK25;   /* OUT_CLR */
        }
        task_sleep(500);  /* 500ms at 1kHz tick */
    }
}

/* ================================================================
 * 演示任务 2：心跳日志（每 5s 打印一次系统状态）
 *
 * 【日志频率调整说明】
 *   原版每 1s 打印一次 HEART + 前 10 beat 打印 [U1..U10] USB 诊断，
 *   与 mem/ctrl 任务一起导致串口每秒 2~3 条日志刷屏，用户根本来不及
 *   输入 shell 命令（help）。现调整为：
 *     · heartbeat: 每 5s 打 1 次（频率降为原来的 1/5）
 *     · mem 压力测试: 每 10s 打 1 次（频率降为原来的 1/5）
 *     · ctrl 任务: 每 15s 做 1 次 suspend/resume（LED 状态切换间隔拉长）
 *     · [U...] USB 诊断：**完全移除**（USB 接收链路已经稳定工作）。
 *                     若以后要诊断 USB 问题，在 shell 里加专门命令。
 * ================================================================ */
static void task_heartbeat(void *arg) {
    (void)arg;
    uint32_t cnt = 0;
    demo_puts("[HEART ] Task started, tick=1/5Hz status log\r\n");

    while (1) {
        cnt++;
        demo_puts("[HEART ] beat #");
        demo_put_uint32(cnt);
        demo_puts(" | tick=");
        demo_put_uint32(hal_systick_get_tick());
        demo_puts(" | heap_free=");
        demo_put_uint32(kmem_free_size());
        demo_puts("B | cur_task=");
        demo_puts(g_current_task ? g_current_task->name : "?");
        demo_puts("\r\n");

        /* 每次 beat 都保持一次 hal_usb_poll（含条件式 INTE 恢复 + EP1 OUT 搬）
         * 以维持 USB CDC 双向通畅，**但不再打印 USB 诊断**。 */
        extern void hal_usb_poll(void);
        hal_usb_poll();

        task_sleep(5000);   /* 5s at 1kHz tick */
    }
}

/* ================================================================
 * 演示任务 3：内存压力测试（每 10s 一轮 alloc/free）
 * ================================================================ */
static void task_mem_stress(void *arg) {
    (void)arg;
    demo_puts("[MEM   ] Task started, stress test every 10s\r\n");
    uint32_t round = 0;

    while (1) {
        round++;
        size_t free_before = kmem_free_size();
        size_t max_before  = kmem_max_free_block();

        /* 随机大小分配一组 */
        #define NMEM 8
        void *p[NMEM];
        size_t sizes[NMEM] = { 32, 64, 128, 96, 256, 48, 160, 80 };
        int ok = 1;
        for (int i = 0; i < NMEM; i++) {
            p[i] = kmalloc(sizes[i]);
            if (!p[i]) {
                ok = 0;
                demo_puts("[MEM   ] Round #");
                demo_put_uint32(round);
                demo_puts(" alloc FAILED at i=");
                demo_put_uint32(i);
                demo_puts(" size=");
                demo_put_uint32(sizes[i]);
                demo_puts("B\r\n");
                break;
            }
            /* 写点数据防止完全未使用 */
            memset(p[i], (int)(i + 0xAA), sizes[i]);
        }

        /* 打乱顺序释放（模拟真实使用场景） */
        int order[NMEM] = { 3,0,7,2,5,1,6,4 };
        for (int i = 0; i < NMEM; i++) {
            int idx = order[i];
            if (p[idx]) kfree(p[idx]);
        }

        size_t free_after = kmem_free_size();
        size_t max_after  = kmem_max_free_block();

        if (ok) {
            demo_puts("[MEM   ] Round #");
            demo_put_uint32(round);
            demo_puts(" free_before=");
            demo_put_uint32(free_before);
            demo_puts("B -> free_after=");
            demo_put_uint32(free_after);
            demo_puts("B");
            if (free_before == free_after && max_before == max_after) {
                demo_puts(" [ZERO-FRAGMENT OK]");
            } else {
                demo_puts(" [free diff=");
                demo_put_uint32((free_after > free_before) ?
                                (free_after - free_before) :
                                (free_before - free_after));
                demo_puts("B]");
            }
            demo_puts("\r\n");
        }

        task_sleep(10000);   /* 10s at 1kHz tick */
    }
}

/* ================================================================
 * 演示任务 4：任务控制 — 定时挂起/恢复 LED 任务
 * ================================================================ */
static void task_ctrl(void *arg) {
    (void)arg;
    demo_puts("[CTRL  ] Task started, suspend/resume LED every 15s\r\n");
    int suspended = 0;

    while (1) {
        /* 先等 15 秒让它跑一会儿 */
        task_sleep(15000);

        if (g_demo_led_task) {
            if (!suspended) {
                task_suspend(g_demo_led_task);
                demo_puts("[CTRL  ] -> Suspend LED task (LED should stop)\r\n");
                suspended = 1;
            } else {
                task_resume(g_demo_led_task);
                demo_puts("[CTRL  ] -> Resume LED task (LED should blink again)\r\n");
                suspended = 0;
            }
        }
    }
}

/* ================================================================
 * Demo 总入口（由 kernel_main 在启动调度器前调用）
 *
 * 【架构调整 — 仅命令触发输出】
 *   用户需求："每次输入完指令后才会回复日志及操作的返回结果"。
 *   这意味着系统需要是"纯同步交互式"的，任何异步后台任务（heartbeat
 *   每秒 1 条 / mem 每 2s 1 轮 / ctrl 每 5s 1 次 / LED 任务 500ms 闪烁）
 *   都不应该再向串口周期性打印日志 —— 否则会打断用户输入，并且与
 *   "仅命令触发" 的交互模型冲突。
 *
 *  策略：
 *    · 不再创建 led / heartbeat / mem_stress / ctrl 4 个演示任务
 *      （这些任务的功能都可以通过 shell 命令手动触发）
 *    · 只保留 shell 任务：用户输入命令 → 解析 → 操作硬件 → 打印结果
 *    · hal_usb_poll() 由 shell 主循环的 hal_console_getc /
 *      getchar_timeout_us 路径内部维持，无需 heartbeat 任务。
 * ================================================================ */
void demo_app_init(void) {
    demo_puts("\r\n");
    demo_puts("========================================\r\n");
    demo_puts("  Mini Kernel Demo Platform Ready\r\n");
    demo_puts("========================================\r\n");
    demo_puts("Kernel version: ");
    demo_puts(k_version());
    demo_puts("\r\n");
    demo_puts("Config: MAX_TASKS=");
    demo_put_uint32(OS_CFG_MAX_TASKS);
    demo_puts("  HEAP=");
    demo_put_uint32(OS_CFG_HEAP_SIZE_BYTES);
    demo_puts("B  TICK_HZ=");
    demo_put_uint32(OS_CFG_TICK_HZ);
    demo_puts("  TIME_SLICE=");
    demo_put_uint32(OS_CFG_TIME_SLICE_TICKS);
    demo_puts("\r\n");
    demo_puts("PeriphService=");
    demo_puts(OS_CFG_PERIPH_SERVICE ? "ON" : "OFF");
    demo_puts("  Shell=");
    demo_puts(OS_CFG_SHELL ? "ON" : "OFF");
    demo_puts("  VFS=");
    demo_puts(OS_CFG_VFS ? "ON" : "OFF");
#if OS_CFG_FATFS
    demo_puts("  FatFs=ON  USB-MSC=ON");
#endif
    demo_puts("\r\n");
    demo_puts("========================================\r\n\r\n");

    demo_puts("[BOOT  ] Mode: Sync-Interactive (commands only, no async logs)\r\n");
    demo_puts("[BOOT  ] Supported buses: GPIO ");
#if OS_CFG_PERIPH_SERVICE
    demo_puts("SPI I2C UART ");
#endif
    demo_puts("\r\n");
    demo_puts("[BOOT  ] Creating interactive shell task...\r\n");

    /* ── HAL 外围"预热"（解决用户报告：重启后固化命令 led on 不生效）──────
     * 用户固化的命令（如 led on / i2c init 0 4 5 ...）理论上每条命令内部都会自行
     *   hal_gpio_init 或 hal_i2c_init，但是为了避免极端情况（例如用户 save 的是
     *   一条 "gpio write 25 1" 而非 "gpio init 25 out 1 + write"），这里先把
     *   最常见、最核心的板载外设做一次基础 init：
     *
     *   1) GPIO25（板载 LED）：init 成推挽输出、初始 LOW（灭）
     *        → bootscript_run_all 执行 "led on" 后会拉成 HIGH
     *
     *   注意：这里只做"安全默认 init"（不亮灯、不产生总线行为），真正的用户级
     *        配置仍然必须由 bootscript 中的显式命令执行。
     *
     *   实现：**直接写 RP2040 寄存器**（避免在这里 extern hal_gpio_init 触发
     *         hal_interface.h 的同名宏展开导致编译错误）。代码与
     *         hal_port.c hal_gpio_init_impl(HAL_GPIO_OUT_PP) 完全等价。
     * ──────────────────────────────────────────────────────────────── */
#if OS_CFG_PERIPH_SERVICE
    {
        register const uint32_t SIO_BASE        = 0xD0000000u;
        register const uint32_t PADS_BANK0_BASE = 0x4001C000u;
        register const uint32_t IO_BANK0_BASE   = 0x40014000u;
        register const uint32_t PIN25           = 25u;
        register const uint32_t MASK25          = 1u << PIN25;
        register const uint32_t RESETS_BASE     = 0x4000C000u;

        /* 1. 确保 RESETS.PADS_BANK0 / IO_BANK0 / PWM / USBCTRL（等）都已经脱离复位
         *    Pico SDK bootrom 已完成；这里只对 PADS/IO 做写操作。 */
        *(volatile uint32_t *)(PADS_BANK0_BASE + 0x04 + PIN25 * 4u) =
            (0u << 7) | (1u << 6) | (0u << 4) | (1u << 3) | (0u << 2) | (0u << 1) | (0u << 0);
        /*   含义: ISO=0, OD=0, IE=1(enable input), DRIVE=8mA, PUE=0, PDE=0, SCHMITT=1, SLEWFAST=0 */

        /* 2. IO_BANK0 引脚 mux: GPIO25 → F5 (SIO)，与 hal_port AF=0 对应 */
        *(volatile uint32_t *)(IO_BANK0_BASE + 0x04 + PIN25 * 8u) = 5u; /* func 5 = SIO */

        /* 3. SIO OE: 置位 → 输出模式 */
        *(volatile uint32_t *)(SIO_BASE + 0x024) = MASK25;  /* OE_SET */

        /* 4. SIO OUT: 先清零（灭灯）*/
        *(volatile uint32_t *)(SIO_BASE + 0x018) = MASK25;  /* OUT_CLR */

        /* 防编译器 unused：RESETS_BASE 没写，读取一下确认在地址空间 */
        (void)(*(volatile uint32_t *)RESETS_BASE);

        demo_puts("[BOOT  ] Pre-init: GPIO25 (LED) OUT, LOW (ready for bootscript)\r\n");

        /* 给 bootscript_run_all 的入口加一行 banner，让启动回放的 6 行 big banner
         *   和 demo banner 之间有明确的视觉分界点（否则用户不知道从哪里开始）。*/
        demo_puts("[BOOT  ] Entering persistent boot command playback (shell_start → bootscript_run_all)...\r\n");
        demo_puts("\r\n");
    }
#endif

    /* 创建交互式 Shell 任务（USB CDC + UART0 双通道）
     *   这是本平台唯一持续运行的用户任务。
     *   Shell 模式：输入命令 → 同步执行 → 打印结果 → 等待下一条命令。
     *   注意 (v0.2)：shell_start() 内部在创建交互式 shell 任务之前，会先
     *   同步调用 bootscript_run_all()，跑完所有 save/! 固化的命令，再进
     *   交互模式。LED / I2C 的回放结果都在那个大 banner 里体现。 */
    shell_start();
    demo_puts("[BOOT  ] Shell ready. Connect USB CDC COMx and type 'help' or 'gpio help'.\r\n\r\n");

    /* ── v2.2: FatFs + USB MSC 初始化（在 bootscript_run_all 跑完 + shell 就绪后）
     *   初始化顺序很关键：
     *     1. shell_start() → 已经向命令表注册了 msc/ls/cd/... 扩展命令
     *     2. fatfs_init_and_mount() → 检测空片自动 f_mkfs FAT16 + 挂载
     *     3. 首启动完成后 ejected=true（Shell 独占写模式，防止主机 USB 竞争写）
     *   用户若想电脑看见 U 盘盘符，显式执行 `msc mount` 即可。
     * ─────────────────────────────────────────────────────────────────── */
#if OS_CFG_FATFS
    {
        FRESULT fr = fatfs_init_and_mount();
        if (fr == FR_OK) {
            if (fatfs_mkfs_done_this_boot()) {
                demo_puts("[BOOT  ] MSC: Blank flash → auto-f_mkfs FAT16 (1012KiB data partition).\r\n");
            }
            demo_puts("[BOOT  ] MSC: FatFs mounted OK. Mode = SHELL-EXCLUSIVE (ejected=true).\r\n");
            demo_puts("[BOOT  ] MSC: To expose drive to host PC, run:  msc mount\r\n");
            demo_puts("[BOOT  ] MSC: Shell commands (mkdir/rm/cat) work now; no `msc eject` needed.\r\n\r\n");
        } else {
            demo_puts("[BOOT  ] MSC: FatFs mount SKIPPED (code=");
            demo_put_uint32((uint32_t)fr);
            demo_puts("). Run `msc status` then `msc mount` or `msc format`.\r\n\r\n");
        }
    }
#endif
}

#else /* !OS_CFG_DEMO_APP —— 关闭 demo 时的空桩，保证链接无未定义符号 */

void demo_app_init(void) {
    /* 演示应用已关闭：用户在此注册自己的任务即可 */
}

#endif /* OS_CFG_DEMO_APP */

/* ================================================================
 * 版本字符串（k_version 必须始终提供，kernel.h 已声明）
 *
 *   2.2: 【大版本】Composite USB：CDC（串口命令行）+ MSC（U 盘）同时枚举。
 *          · Flash 三分区：
 *              0x000000..0x0FFFFF (1 MiB)          → 固件区
 *              0x100000..0x1FEFFF (2032×512=1016KB) → MSC U 盘 FAT16 数据盘
 *              0x1FF000..0x1FFFFF (2×4KiB)         → bootscript 双备份固化区
 *          · Shell 目录命令（真正的文件系统，不是"地址索引"）：
 *              ls / cd / pwd / mkdir / rmdir / rm / cat
 *          · msc 子命令切换主机/本机写互斥：
 *              msc mount   → 主机 USB 可写 U 盘（Shell 只读）
 *              msc eject   → 主机 USB 显示"无介质"（Shell 可 mkdir/rm/cat）
 *              msc status  → 查看分区容量 / eject / 是否格式化 / mounted
 *              msc format  → 两步确认后重建 FAT16
 *          · 首次上电自动 f_mkfs 创建 FAT16，Windows 拷贝的文件、
 *            子目录、Shell 端 cat 看到完全一致（共用同一 Flash 后端）。
 *
 *   0.2.1: 启动速度大幅优化（10s+ → <500ms）
 *     · 新增 MK_BOOT_DIAG_LED 宏（默认 0 = 发布版，不跑 LED 诊断）
 *     · 删除 50M nop ~5s "USB 枚举忙等"（已有 boot status + WITHOUT_DTR=1
 *       兜底，不需要死等 5 秒让用户"打开终端再开机"）
 *     · _led_stage 1-12 默认折叠为空函数，将来崩溃时改 MK_BOOT_DIAG_LED=1
 *       即可重新启用数闪定位。
 * ================================================================ */
#ifndef KERNEL_VERSION_STR
#define KERNEL_VERSION_STR  "2.2.6 ✅ STABLE"
#endif

const char *k_version(void) {
    return KERNEL_VERSION_STR;
}
/* (文件结束。多余 '}' 已删去) */
