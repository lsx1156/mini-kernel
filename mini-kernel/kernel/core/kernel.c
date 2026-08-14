/**
 * @file    kernel.c
 * @brief   内核主入口 — 修复版：启动任务化（boot_setup task）
 *
 *  【关键架构修复】
 *    诊断固件证明：纯调度器 + 单任务完全正常工作。
 *    崩溃根因：旧版 kernel_main 在 kmem_init / task_module_init 等核心初始化
 *    之前就 `cpsie i` 并 busy_wait 500ms 做 USB 枚举。USBCTRL_IRQ / SDK
 *    alarm1 IRQ 在核心初始化数据结构（kmem 链表/TCB 池/就绪队列）尚未
 *    静态一致时触发，TinyUSB state machine + SDK alarm 回调可能与初始化
 *    代码发生非同步的内存访问，导致静默数据损坏 → 稍后才触发 HardFault。
 *
 *    修复：全部"冷初始化"期间（kmem/task/sched/systick）保持 PRIMASK=1
 *    （关中断），创建一个 boot_setup 任务挂入就绪队列。sched_start 启动
 *    调度器后 boot_setup 被第一个调度运行，此时核心状态已稳定，再按顺序：
 *      hal_console_init → cpsie i → USB 枚举 → 打印 banner → demo_app_init → 自销毁。
 */
#include "task.h"
#include "sched.h"
#include "mem.h"
#include "hal_interface.h"
#include "os_config.h"

#include "pico/stdlib.h"
#include <stdio.h>
#ifndef PICO_DEFAULT_LED_PIN
#  define PICO_DEFAULT_LED_PIN 25
#endif

extern uint8_t __end__;
/* 内核堆大小统一使用 os_config.h 的 OS_CFG_HEAP_SIZE_BYTES。
 * 默认 4KB，覆盖 boot_setup(768) + idle(256) + led(384) + heartbeat(768)
 * + mem(768) + ctrl(384) + shell(768) 七个 TCB(~56B 每个) + 少量
 * kmalloc 碎片缓冲。RP2040 256KB SRAM 充足。
 * 若用户裁剪更多模块/任务，可在 os_config.h 中调小此值节省 RAM。 */
#define KERNEL_HEAP_SIZE  OS_CFG_HEAP_SIZE_BYTES

/* demo_app_init() 由 examples/builtin_demo/demo_app.c 提供，
 * 独立应用（如 rp2040demo）不链接 demo_app.c。
 * 使用 weak 声明：若链接器找不到符号，调用时跳到 0/直接跳过。
 * 运行时判断 &demo_app_init != NULL 即可安全地决定是否调用。 */
extern __attribute__((weak)) void demo_app_init(void);
void kernel_main(void);
void SysTick_Handler(void);
void kernel_tick_hook(void);

/* ================================================================
 * 板载 LED 初始化（main 开头直接写寄存器，避免 SDK 行为）
 * ================================================================ */
static inline void _led_init(void) {
    /* PADS_BANK0: GPIO25 — ISO=0 OD=0 IE=0 DRIVE=1(4mA) PUE=0 PDE=0 SCHMITT=1 SLEWFAST=0 */
    *(volatile uint32_t *)(0x4001C000 + 0x6C) = 0x00000056;
    /* IO_BANK0: GPIO25 = FUNC5 (SIO) */
    *(volatile uint32_t *)(0x40014000 + 0x0CC) = 0x00000005;
    /* SIO GPIO_OE set */
    *(volatile uint32_t *)(0xD0000000 + 0x024) = 0x02000000u;
    /* SIO OUT_CLR */
    *(volatile uint32_t *)(0xD0000000 + 0x018) = 0x02000000u;
}
static inline void _led_set(int on) {
    if (on) *(volatile uint32_t *)(0xD0000000 + 0x014) = 0x02000000u;
    else     *(volatile uint32_t *)(0xD0000000 + 0x018) = 0x02000000u;
}

/* boot_setup 任务：调度器首次运行后执行的热初始化阶段
 *   0. [可选] 板载 USB Serial 需要主机 → SET_CONFIGURATION。
 *      等待 stdio_usb_connected() 超时 5s（LED 闪烁标识等待中）
 *   1. stdio_init_all (UART0 + USB CDC) ← 已在 hal_console_init 完成
 *   2. 开中断 + 等 USB 枚举
 *   3. 打印 Banner
 *   4. demo_app_init (4 个 demo 任务 + shell)
 *   5. 自挂起（启动任务一次性） */

/* LED 设置：GPIO25 SIO 直接写 */
static inline void _led_on(void) {
    *(volatile uint32_t *)(0xD0000000u + 0x014) = 0x02000000u;
}
static inline void _led_off(void) {
    *(volatile uint32_t *)(0xD0000000u + 0x018) = 0x02000000u;
}

static void _boot_setup_task(void *arg) {
    (void)arg;

    /* 控制台已在 kernel_main() (MSP 上下文) 中初始化完毕。
     * USB 枚举也已在启动前完成。SDK 原生 printf/putchar 可正常输出。 */

    /* — Step 1: 系统滴答定时器 — */
    hal_systick_init(OS_CFG_TICK_HZ);

    /* 【关键修复】确保全局中断开启
     * sched_start() 中的 svc #0 进入 SVC_Handler，SVC_Handler 内部可能
     * 执行了 cpsid i 但未恢复 cpsie i → 从 SVC 返回后 PRIMASK=1 →
     * TIMER_IRQ_0 无法触发 → tick 永远是 0 → task_sleep 永远不唤醒。
     * 在 hal_systick_init 之后显式 cpsie i，确保 systick 中断能触发。 */
    __asm volatile ("cpsie i" ::: "memory");

    /* — Step 2: 打印启动横幅（USB 已就绪，直接输出） — */
    {
        for (int i = 0; i < 60; i++) hal_console_putc('=');
        hal_console_putc('\r'); hal_console_putc('\n');
        hal_console_putc('\r'); hal_console_putc('\n');
        const char *banner = "=== Mini Kernel Boot ===\r\n";
        for (const char *p = banner; *p; p++) hal_console_putc(*p);
        for (int i = 0; i < 60; i++) hal_console_putc('=');
        hal_console_putc('\r'); hal_console_putc('\n');
        hal_console_putc('\r'); hal_console_putc('\n');
    }

    /* — Step 3: Demo 应用初始化（创建 led / heartbeat / mem / ctrl + shell） — */
    if (&demo_app_init != NULL) {
        demo_app_init();
    }

    /* — Step 4: 启动流程结束 — boot_setup 自挂起，不再调度到 — */
    task_suspend(g_current_task);
    while (1) task_sleep(1000);
}

/* ================================================================
 * main：冷启动（关中断、最小硬件初始化）→ kernel_main → 永远不返回
 *
 *  【关键：弱 main() 符号】
 *    mini-kernel 作为库被独立应用（rp2040demo）add_subdirectory 时，
 *    应用自己的 src/main.c 会提供 main()。用 weak 标记：
 *      · 若无人定义 main → 用内核自带 main（mini-kernel 顶层固件）
 *      · 若 rp2040demo 提供 main → 用应用的强定义（rp2040demo 的应用层）
 *    两种方式最终都应调用 kernel_main()。
 * ================================================================ */
__attribute__((weak)) int main(void) {
    /* 【核心修复】冷启动阶段**始终关中断**。诊断固件已证实：
     *   开中断做 USB 枚举 → SDK alarm pool / TinyUSB state machine
     *   的 IRQ 回调会在 kmem_init / task_module_init 数据结构未
     *   初始化完成时就跑起来 → 静默损坏内存 → 稍后 HardFault。 */
    __asm volatile ("cpsid i" ::: "memory");

    _led_init();
    _led_set(0);  /* 灭灯，避免 bootrom 继承亮态干扰判断 */

    kernel_main();

    /* scheduler 意外返回：1Hz 慢闪，与 HardFault 5Hz 100ms 爆闪区分 */
    while (1) {
        _led_set(1); busy_wait_us_32(500000);
        _led_set(0); busy_wait_us_32(500000);
    }
    return 0;
}

/* ================================================================
 * kernel_main：冷初始化部分（全部关中断，除了 USB 枚举阶段）
 * ================================================================ */
void kernel_main(void) {
    /* —— 冷初始化 Step 1: 内存管理（关中断安全） —— */
    kmem_init(&__end__, KERNEL_HEAP_SIZE);

    /* —— 冷初始化 Step 2: 任务模块 + idle 任务（关中断安全） —— */
    task_module_init();

    /* —— 冷初始化 Step 3: 调度器（空队列安全） —— */
    sched_init();

    /* —— 冷初始化 Step 4: 创建 boot_setup 启动任务（栈增大到 1024 以容纳 demo_app_init） —— */
    task_create("boot_setup", _boot_setup_task, NULL, 1024, 3);

    /* —— 【关键修复】把控制台初始化移到这里（MSP 上下文，全局中断开启后）
     *
     *   usb_print_test 证明：stdio_init_all() 必须在 MSP/main 上下文
     *   中调用才能正确初始化 USB CDC。在 PSP 任务上下文中调用时，
     *   SDK 的 USBCTRL_IRQ 注册和 USB 枚举机制失效，导致 printf
     *   无法通过 USB 发送数据。
     *
     *   同时必须开启全局中断，让 USBCTRL_IRQ 能驱动 USB 枚举（Windows
     *   枚举 USB 约 100ms），并等待 500ms 确保 SET_CONFIGURATION 完成。 */
    __asm volatile ("cpsie i" ::: "memory");
    hal_console_init(115200);
    for (volatile uint32_t i = 0; i < 50000000; i++) __asm("nop"); /* ~5s wait for USB enum */

    /* —— 冷初始化 Step 5: 启动调度器（内部 cpsie i + msr psp + svc #0） —— */
    sched_start();
}

/* ================================================================
 * kernel_tick_hook：HAL TIMER_IRQ_0 尾部回调
 * ================================================================ */
void kernel_tick_hook(void) {
    sched_sleep_tick();

    if (g_current_task) {
        /* 首次 tick 时初始化 time_slice（首次进入 tick 才赋值） */
        if (g_current_task->time_slice == 0) {
            g_current_task->time_slice = OS_CFG_TIME_SLICE_TICKS *
                ((g_current_task->weight > 0) ? g_current_task->weight : 1);
        }
        if (g_current_task->time_slice > 0) {
            g_current_task->time_slice--;
        }
        if (g_current_task->time_slice == 0) {
            hal_yield_trigger();
        }
    }
}

static void _systick_compat_stub(void) {}
void SysTick_Handler(void) __attribute__((weak, alias("_systick_compat_stub")));
