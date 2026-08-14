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

/* ================================================================
 *  v0.2.1 启动速度开关（Startup Performance Knob）
 *
 *  MK_BOOT_DIAG_LED = 0   【默认，发布版】跳过所有 _led_stage 诊断闪烁
 *                            和 5s USB 枚举忙等，启动耗时 < 500ms（USB 枚举
 *                            + banner 打印 + 任务创建）。诊断保留在代码里，
 *                            将来如果遇到启动崩溃，把此宏改成 1，
 *                            数 LED 闪烁次数即可定位崩溃在哪一步。
 *
 *  MK_BOOT_DIAG_LED = 1   【调试版】启用阶段 1-12 的 LED 诊断指示
 *                            （每阶段 count 次闪烁 + 末尾约 1s 停顿），
 *                            同时保留 50M nop ~5s 的 USB 枚举等待，
 *                            给用户"打开 PuTTY 再开机"捕捉 banner 的时间。
 * ================================================================ */
#ifndef MK_BOOT_DIAG_LED
#  define MK_BOOT_DIAG_LED   0
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

/* LED 诊断辅助函数：通过 LED 闪烁次数指示当前阶段
 * 用法：_led_stage(3) 会让 LED 闪 3 次（每次亮灭各 ~200ms）
 * 这样用户可以告诉我们 LED 闪了几下，就能定位崩溃位置。
 * 【v0.2.1 性能优化】MK_BOOT_DIAG_LED=0 时此函数体折叠为空。 */
static void _led_stage(uint8_t count) {
#if MK_BOOT_DIAG_LED
    register const uint32_t sio_base = 0xD0000000u;
    register const uint32_t gpio25_mask = 0x02000000u;
    
    for (uint8_t i = 0; i < count; i++) {
        *(volatile uint32_t *)(sio_base + 0x014) = gpio25_mask;  /* ON */
        for (volatile uint32_t j = 0; j < 2500000; j++) __asm("nop");
        *(volatile uint32_t *)(sio_base + 0x018) = gpio25_mask;  /* OFF */
        for (volatile uint32_t j = 0; j < 2500000; j++) __asm("nop");
    }
    /* 结束后留 1 秒停顿 */
    for (volatile uint32_t j = 0; j < 6250000; j++) __asm("nop");
#else
    (void)count;
#endif
}

/* boot_setup 任务：调度器首次运行后执行的热初始化阶段
 *   0. [可选] 板载 USB Serial 需要主机 → SET_CONFIGURATION。
 *      等待 stdio_usb_connected() 超时 5s（LED 闪烁标识等待中）
 *   1. stdio_init_all (UART0 + USB CDC) ← 已在 hal_console_init 完成
 *   2. 开中断 + 等 USB 枚举
 *   3. 打印 Banner
 *   4. demo_app_init (4 个 demo 任务 + shell)
 *   5. 自挂起（启动任务一次性） */

static void _boot_setup_task(void *arg) {
    (void)arg;

    /* 阶段 9：boot_setup 任务开始运行 */
    _led_stage(9);

    /* — Step 1: 系统滴答定时器 — */
    hal_systick_init(OS_CFG_TICK_HZ);

    /* 阶段 10：systick 初始化完成 */
    _led_stage(10);

    /* 【关键修复】确保全局中断开启
     * sched_start() 中的 svc #0 进入 SVC_Handler，SVC_Handler 内部可能
     * 执行了 cpsid i 但未恢复 cpsie i → 从 SVC 返回后 PRIMASK=1 →
     * TIMER_IRQ_0 无法触发 → tick 永远是 0 → task_sleep 永远不唤醒。
     * 在 hal_systick_init 之后显式 cpsie i，确保 systick 中断能触发。 */
    __asm volatile ("cpsie i" ::: "memory");

    /* 阶段 11：中断已开启 */
    _led_stage(11);

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

    /* 阶段 12：横幅打印完成，准备创建 demo 任务 */
    _led_stage(12);

    /* ── Step 3: Demo 应用初始化（创建 led / heartbeat / mem / ctrl + shell） ──
     * 【v0.2.0-beta 修复：**绝对不要在 demo_app_init 返回后再写 GPIO25！**】
     *   之前的 bug：demo_app_init 内部调用 shell_start() → bootscript_run_all()
     *   → 用户固化的 "led on" 把 GPIO25 HIGH，然后返回 boot_setup_task，
     *   接着 _led_stage(13) 做了 13 次 "ON → long wait → OFF → long wait"，
     *   最后一步 SIO_OUT_CLR 直接把用户设置的 HIGH 拉回 LOW，LED 表现为
     *   "启动亮了一下然后又灭了"。用户输入 list 能看到 #0: led on，
     *   但灯实际是灭的，这就是 2026-08-15 用户报告的 "操作系统这样是不对的"。
     *
     *   修复原则：
     *     · 所有 _led_stage 诊断指示必须放在 demo_app_init 之前跑完；
     *     · demo_app_init（及其内部 bootscript 回放）之后，**永不**
     *       无条件操纵 GPIO25（除了 HardFault / NMI 处理的故障指示）。
     *
     *   阶段 13 合并入阶段 12，取消独立的"demo 任务创建完成"指示，避免冲突。
     * ──────────────────────────────────────────────────────────────── */
    if (&demo_app_init != NULL) {
        demo_app_init();
    }

    /* ── v2.2.3: FatFs / MSC 分区初始化（可移植内核独立完成，不依赖 demo 应用）
     *
     *  ⚠️ 【为什么放在 demo_app_init 之后（= 原 v2.2.0 位置），不能在前面？】
     *  1. 【修复死机 Bug v2.2.1】boot_setup_task 栈很小（虽然已经从 1024 扩到
     *     2048），首启动空片时 fatfs_init_and_mount → f_mkfs() 内部调用链极深
     *     （FatFs 格式化计算 FAT 表、分簇、大量局部变量），demo_app_init() 内部
     *     的 shell_start() 会调用 bootscript_run_all()，boot_setup_task 的
     *     调用栈深度如果在 banner 打印帧 + 前置函数帧基础上，再立刻跑
     *     f_mkfs，容易在 1024 栈顶时硬爆栈 → 写坏 kmem 链表头 / 就绪队列
     *     → 调度器 HardFault 死机，用户看到"根本开不了机"。
     *  2. 【时序一致】OS_CFG_DEMO_APP=1（RP2040 demo 默认开）时，demo_app_init()
     *     里本来就已经调用过 fatfs_init_and_mount()（见 demo_app.c L398），
     *     我们放在它之后，s_fs_mounted=true 直接返回 FR_OK（幂等安全），
     *     不会重复 mkfs；而且运行顺序严格恢复到 v2.2.0 用户验证通过的：
     *        shell_start() → fatfs_init_and_mount()
     *  3. 【OS_CFG_DEMO_APP=0 不依赖 example】demo_app_init 是空桩直接返回，
     *     我们仍在这之后执行 fatfs_init_and_mount()，确保关闭示例应用时
     *     用户自有应用也能自动用到 FatFs + Shell 目录命令。
     *
     *  ⚠️ 【boot_setup_task 栈大小说明 v2.2.3】
     *     task_create("boot_setup", 栈 1024 → 2048)：f_mkfs 在 FF_USE_LFN=0
     *     下的最坏栈也需要 ~800B（DIR + FILINFO + MKFS_PARM + FatFs 内部
     *     多层调用帧），1024 字节在 banner 打印帧之后的连续调用下，留的
     *     安全边界只有 ~150B，首启动触发格式化时 100% 越界写。
     *     扩到 2048 后给格式化 / Shell 启动命令回放留 1KB 安全边界。
     *
     *   头文件隔离：fatfs_api.h 内部 #include "ff.h"，当 OS_CFG_FATFS=0 时
     *     fatfs shim 目录不在 include path，直接 #include 会编译错。因此用
     *     extern 前向声明，整个块也放在 #if OS_CFG_FATFS 中，关闭时被
     *     预处理器彻底移除。
     * ──────────────────────────────────────────────────────────────── */
#if OS_CFG_FATFS
    {
        extern int fatfs_init_and_mount(void);   /* FRESULT 与 int 兼容（返回值 0-20，枚举 1 字节，ARM AAPCS r0 返回） */
        (void)fatfs_init_and_mount();            /* 结果忽略：demo app / Shell banner 会做更详细诊断打印 */
    }
#endif

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
     *   开中断做 USB 枚举 → SDK alarm pool / TinyUSB 回调会在 kmem_init / task_module_init 数据结构未初始化完成时就跑起来 → 静默损坏内存 → 稍后 HardFault。 */
    __asm volatile ("cpsid i" ::: "memory");

    _led_init();
    _led_set(0);  /* 灭灯 */

    /* 阶段 1：进入 main */
    _led_stage(1);

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
    /* 确保 LED GPIO 已初始化（rp2040demo 的 main 覆盖了弱 main，
     * 不会调用 _led_init，诊断 LED 需要 GPIO 配置才能工作） */
    _led_init();
    _led_set(0);

    /* 阶段 2：进入 kernel_main */
    _led_stage(2);

    /* —— 冷初始化 Step 1: 内存管理（关中断安全） —— */
    kmem_init(&__end__, KERNEL_HEAP_SIZE);

    /* 阶段 3：内存初始化完成 */
    _led_stage(3);

    /* —— 冷初始化 Step 2: 任务模块 + idle 任务（关中断安全） —— */
    task_module_init();

    /* 阶段 4：任务模块初始化完成 */
    _led_stage(4);

    /* —— 冷初始化 Step 3: 调度器（空队列安全） —— */
    sched_init();

    /* —— 冷初始化 Step 4: 创建 boot_setup 启动任务（栈增大到 2048 以容纳 demo_app_init + f_mkfs 最坏栈） —— */
    task_create("boot_setup", _boot_setup_task, NULL, 2048, 3);

    /* 阶段 5：boot_setup 任务创建完成 */
    _led_stage(5);

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

    /* 阶段 6：开中断，准备初始化控制台 */
    _led_stage(6);

    hal_console_init(115200);

    /* 阶段 7：控制台初始化完成，开始等待 USB 枚举 */
    _led_stage(7);

    /* 【v0.2.1 性能优化】原代码在这里忙等 50,000,000 nop (~5s)，说是
     * "等 USB 枚举完成"。但实际上 USB 枚举完全由 USBCTRL_IRQ + tud_task()
     * 驱动，与 CPU 空等无关。并且我们已经设置：
     *   · PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1（不等待 DTR 握手）
     *   · PICO_STDIO_USB_STDOUT_TIMEOUT_US=0    （发送不阻塞）
     *   · boot status 命令（RAM 常驻回放结果）——即使启动时没接串口，
     *     用户打开终端后仍能看到所有 bootscript 回放结果。
     * 因此这个 5s 忙等在发布版里 100% 是浪费时间，直接删除。
     * 调试模式（MK_BOOT_DIAG_LED=1）保留，供崩溃定位用。 */
#if MK_BOOT_DIAG_LED
    for (volatile uint32_t i = 0; i < 50000000; i++) __asm("nop"); /* ~5s wait for USB enum */
#endif

    /* 阶段 8：USB 枚举完成，准备启动调度器 */
    _led_stage(8);

    /* —— 冷初始化 Step 5: 启动调度器（内部 cpsie i + msr psp + svc #0） —— */
    sched_start();

    /* 不应到达此处 */
    while (1) {
        register uint32_t sio_base = 0xD0000000u;
        register uint32_t mask25   = 0x02000000u;
        *(volatile uint32_t *)(sio_base + 0x014) = mask25;
        for (volatile uint32_t j = 0; j < 6250000; j++) __asm("nop");
        *(volatile uint32_t *)(sio_base + 0x018) = mask25;
        for (volatile uint32_t j = 0; j < 6250000; j++) __asm("nop");
    }
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
