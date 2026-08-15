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
#include "pico/time.h"   /* sleep_ms / busy_wait — 不依赖 stdio 初始化，关中断也能工作 */
#include "hardware/timer.h"
#include "hardware/irq.h"
#include <stdio.h>

/* 从 hal_port.c 导出的符号 */
extern volatile uint32_t g_tick_interval_us;
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
#  define MK_BOOT_DIAG_LED   0   /* 0 = 关闭所有 stage 闪烁 */
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
 * 板载 LED 初始化（v2.2.4 修复：用 SDK gpio_* API，与 minimal_led_test.c
 * 完全一致；sleep_ms / busy_wait_until 直接读 TIMER 硬件寄存器，
 * 关中断下也能正常工作，不依赖 systick 或 alarm IRQ）。
 * ================================================================ */
static inline void _led_init(void) {
    const uint LED = (uint)PICO_DEFAULT_LED_PIN;
    gpio_init(LED);
    gpio_set_dir(LED, GPIO_OUT);
    gpio_set_slew_rate(LED, GPIO_SLEW_RATE_SLOW);
    gpio_set_drive_strength(LED, GPIO_DRIVE_STRENGTH_4MA);
    gpio_put(LED, 0);
}
static inline void _led_set(int on) {
    gpio_put((uint)PICO_DEFAULT_LED_PIN, on ? 1 : 0);
}

/* boot_setup 任务：调度器首次运行后执行的热初始化阶段
 *   0. [可选] 板载 USB Serial 需要主机 → SET_CONFIGURATION。
 *      等待 stdio_usb_connected() 超时 5s（LED 闪烁标识等待中）
 *   1. stdio_init_all (UART0 + USB CDC) ← 已在 hal_console_init 完成
 *   2. 开中断 + 等 USB 枚举
 *   3. 打印 Banner
 *   4. demo_app_init (4 个 demo 任务 + shell)
 *   5. 自挂起（启动任务一次性） */

/* LED 设置：GPIO25 通过 SDK API 操作 */
static inline void _led_on(void) {
    gpio_put((uint)PICO_DEFAULT_LED_PIN, 1);
}
static inline void _led_off(void) {
    gpio_put((uint)PICO_DEFAULT_LED_PIN, 0);
}

/* LED 诊断辅助函数：通过 LED 闪烁次数指示当前阶段
 * 用法：_led_stage(3) 会让 LED 闪 3 次（每次亮灭各 200ms）
 * 这样用户可以告诉我们 LED 闪了几下，就能定位崩溃位置。
 * 【v0.2.1 性能优化】MK_BOOT_DIAG_LED=0 时此函数体折叠为空。
 * 【v2.2.4 修复】节奏改为 250ms on / 250ms off，与 main.c 开头的 5 闪
 *               节奏一致，便于用户对比；sleep_ms 基于硬件 TIMER
 *               （非忙等 nop 计数），关中断也准确。 */
static void _led_stage(uint8_t count) {
#if MK_BOOT_DIAG_LED
    const uint LED = (uint)PICO_DEFAULT_LED_PIN;
    for (uint8_t i = 0; i < count; i++) {
        gpio_put(LED, 1);  /* ON  250ms */
        sleep_ms(250);
        gpio_put(LED, 0);  /* OFF 250ms */
        sleep_ms(250);
    }
    /* 结束后留 1 秒停顿，便于区分数组 */
    sleep_ms(1000);
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

/* ────────── 精简诊断：只闪 1 次 ────────── */
static inline void _blink1(void) {
    _led_on();  busy_wait_us_32(200000u);
    _led_off(); busy_wait_us_32(200000u);
}

static void _boot_setup_task(void *arg) {
    (void)arg;

    /* — Step 1: 初始化内核 systick (ALARM3 + TIMER_IRQ_3) —
     * SDK alarm_pool 已在 hal_console_init 中初始化，使用 ALARM0
     * (PICO_TIME_DEFAULT_ALARM_POOL_HARDWARE_ALARM_NUM=0)，不与内核 ALARM3 冲突。
     * irq_set_exclusive_handler(3, ...) 的 hard_assert 能通过
     * (vtable 中 TIMER_IRQ_3 仍是 __unhandled_user_irq)。 */
    hal_systick_init(OS_CFG_TICK_HZ);

    /* — Step 2: 打印启动横幅（USB 已就绪，直接输出） — */
    {
        for (int i = 0; i < 60; i++) hal_console_putc('=');
        hal_console_putc('\r'); hal_console_putc('\n');
        const char *banner = "=== Mini Kernel Boot ===\r\n";
        for (const char *p = banner; *p; p++) hal_console_putc(*p);
        for (int i = 0; i < 60; i++) hal_console_putc('=');
        hal_console_putc('\r'); hal_console_putc('\n');
    }

    /* — Step 3: Demo 应用初始化（创建 led / heartbeat / mem / ctrl + shell） — */
    if (&demo_app_init != NULL) {
        demo_app_init();
    }

    /* — Step 4: FatFs / MSC 分区初始化 — */
#if OS_CFG_FATFS
    {
        extern int fatfs_init_and_mount(void);
        (void)fatfs_init_and_mount();
    }
#endif

    /* — Step 5: 启动流程结束 — boot_setup 自挂起，永久不再调度 —
     *
     *   【v2.2.5 修复 · TCB 损坏 / ps 乱码 / LED 爆闪 根因】
     *
     *   旧版：task_suspend + while(1) task_sleep(1000)
     *     · task_suspend 设 state=SUSPEND，hal_yield_trigger 设 PendSV pending
     *     · 但 PendSV 在 cpsie i 后才真正进入，此期间代码继续执行
     *     · task_sleep 不检查 state，直接覆盖 SUSPEND→SLEEP，
     *       并 _sched_sleep_enqueue 把 SUSPEND 任务塞进睡眠队列
     *     · 1000 tick 后 sched_tick_hook 唤醒 → READY → 重新入就绪队列
     *     · boot_setup "复活"后再次执行 demo_app_init / fatfs_init，
     *       重复创建已存在的任务 → g_task_pool 指针被覆盖为新 TCB，
     *       旧 TCB 悬空 → ps 输出 ID=0x20000464 等垃圾值 → LED 爆闪。
     *
     *   修复：task_suspend 后用空循环等待 PendSV 切换。SUSPEND 状态
     *   永远不会被 sched_ready_pick_next 选中，while(1) 只是防止
     *   noreturn 任务返回触发 HardFault（LR=0）。 */
    task_suspend(g_current_task);
    while (1) { /* SUSPEND 后不会再被调度；空循环仅作 noreturn 守卫 */ }
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
 * kernel_tick_hook：HAL TIMER_IRQ_3 尾部回调
 * ================================================================ */
void kernel_tick_hook(void) {
    sched_sleep_tick();

    if (g_current_task) {
        /* 【v2.2.9 · 栈金丝雀检查（TIMER_IRQ_3 中断内：只做「检测 + 置标志 + 超短提示」，绝不做调度/长打印！）
         *
         *   一按回车就崩（PuTTY "Error writing to serial device"）的根因：
         *   上一版在此处直接调用 task_suspend(g_current_task) 做 self-suspend
         *   + 打印 200+ 字节 PSP/MAGIC 诊断到 hal_console_putc：
         *     ①  TIMER_IRQ_3 里改 RUNNING→SUSPEND 后，本函数还要继续跑
         *        time_slice--/hal_yield_trigger，调度状态不一致 → HardFault；
         *     ②  中断里写 200+ 字节 CDC FIFO → TinyUSB 状态机不推进（
         *        需要非中断上下文的 tud_task_ext）→ USB 端点卡死 → Windows
         *        认为 COM 口断开 → PuTTY 报错。
         *
         *   修复：中断里只做三件事（全部 O(1)，10 条指令内搞定）：
         *     · 检查 4×MAGIC 是否损坏；
         *     · 损坏 → 置 TCB.stack_overflow 标志位（给 sched_do_switch
         *       在 PendSV 上下文里安全处理）；
         *     · 打印一行极短提示（~12 字节："\n[K] OVF\n"）——最多三字节
         *       落 FIFO，不塞爆 TinyUSB 状态机。
         *
         *   完整诊断打印 + 挂起状态切换全部挪到 sched_do_switch，
         *   PendSV 优先级最低/零中断嵌套 + 已保存 from->sp 的绝对安全点。
         *   （PS: hal_console_putc 在 STDOUT_TIMEOUT_US=0 下 FIFO 满直接丢，
         *         不会阻塞中断等待 USB）
         */
        /* 【v2.2.11 · OVF 轻量化 · 只设标志不触发切换】
         *   致命旧 bug：当 VT2 栈在 Thread mode 下已经到 MAGIC 边缘时，
         *   TIMER_IRQ_3 硬件自动 push 8 个寄存器（32B）到 PSP 上，
         *   把 MAGIC 击穿 → 本 if 检测到并置 stack_overflow=1 →
         *   irq handler return 时硬件 pop 从损坏的 MAGIC 区读取垃圾 pc/xpsr
         *   → Thread mode 跳非法地址 → HardFault = LED 5Hz 爆闪。
         *   （PendSV 里的 OVF 安全处理代码永远没机会执行！）
         *
         *   修复：OVF 检测只做一件事 — 置 stack_overflow 标志位。
         *   不再在 TIMER_IRQ_3 里打印任何字符（避免触发 CDC FIFO 状态机
         *   的额外嵌套调用）。PendSV 里的 OVF 处理保留，但从 sched_do_switch
         *   入口的第一层检查改成"如果 from 已标记 OVF 且 MSP 此时安全，
         *   才打印诊断并挂起 from"——但我们现在把 OVF 的实际强制挂起
         *   也去掉了，只在 `ps` 命令里显示标志位。这样 MAGIC 击穿后最坏情况是
         *   任务行为异常（串口打印乱码），但绝不会触发退出异常时的 HardFault
         *   爆闪。后续用户通过 `ps` 看到 OVF=Y 手动 kill 或重启即可。*/
        if (g_current_task->state == TASK_STATE_RUNNING &&
            !task_stack_check(g_current_task) &&
            !g_current_task->stack_overflow) {
            g_current_task->stack_overflow = 1;
            /* 不打印、不切任务、不挂起；只留标志给 `ps` 展示即可 */
        }

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

/* ================================================================
 * 【v2.2.8】HardFault 现场转储（UART0 直写寄存器）
 *
 *   之前 HardFault 只有 5Hz 闪灯，无法定位崩溃现场。现在 handler
 *   (context_switch.S) 先把 PSP/MSP 存到下面的全局，再 bl 本函数，
 *   用最底层方式（直写 UART0 寄存器 + 忙等）输出故障寄存器：
 *     · 出错任务的 PC/LR/xPSR（从 PSP 硬件栈帧还原 → 直接看到飞哪了）
 *     · SCB CFSR/HFSR/BFAR（总线错地址 / UsageFault 类型）
 *     · g_current_task 名字（谁在跑时崩的）
 *   不走 putchar/SDK/TinyUSB —— fault 时 USB 状态未知，调用 SDK 有
 *   二次 fault → Lockup 风险；UART0 寄存器直写在任何总线状态下安全。
 *
 *   查看方式：USB-TTL 接 GPIO0(UART0 TX) → GND，115200-8N1。
 *   没接 UART 线时仍保持 5Hz 闪灯（dump 之后的死循环）。
 * ================================================================ */
volatile uint32_t g_fault_psp = 0;
volatile uint32_t g_fault_msp = 0;

#define FAULT_UART_DR    (*(volatile uint32_t *)0x40034000u) /* UART0 数据寄存器 */
#define FAULT_UART_FR    (*(volatile uint32_t *)0x40034018u) /* UART0 flag 寄存器 */
#define FAULT_UART_TXFF  (1u << 5)                           /* bit5: TX FIFO 满 */
#define FAULT_SRAM_LO    0x20000000u
#define FAULT_SRAM_HI    0x20042000u   /* RP2040 264KB SRAM 上界 */

static void _fault_putc(char c) {
    while (FAULT_UART_FR & FAULT_UART_TXFF) { }   /* 忙等 TX FIFO 非满 */
    FAULT_UART_DR = (uint32_t)(uint8_t)c;
}
static void _fault_puts(const char *s) { while (*s) _fault_putc(*s++); }
static void _fault_puthex32(uint32_t v) {
    const char hex[] = "0123456789ABCDEF";
    _fault_putc('0'); _fault_putc('x');
    for (int i = 28; i >= 0; i -= 4) _fault_putc(hex[(v >> i) & 0xF]);
}

/* 由 isr_hardfault 调用（Handler 模式，跑在 MSP 上，C 函数栈安全） */
void hardfault_dump_c(void) {
    uint32_t hfsr = *(volatile uint32_t *)0xE000ED2Cu;  /* HardFault Status */
    uint32_t cfsr = *(volatile uint32_t *)0xE000ED28u;  /* Configurable Fault Status */
    uint32_t bfar = *(volatile uint32_t *)0xE000ED38u;  /* BusFault Address */

    _fault_puts("\r\n\n!!! HardFault !!!\r\n");
    _fault_puts("MSP=");   _fault_puthex32(g_fault_msp);
    _fault_puts("  PSP="); _fault_puthex32(g_fault_psp);
    _fault_puts("  CFSR="); _fault_puthex32(cfsr);
    _fault_puts("  HFSR="); _fault_puthex32(hfsr);
    _fault_puts("  BFAR="); _fault_puthex32(bfar);

    /* PSP 指向被打断任务的硬件栈帧底：
     * [0]r0 [1]r1 [2]r2 [3]r3 [4]r12 [5]lr [6]pc [7]xpsr */
    if (g_fault_psp >= FAULT_SRAM_LO && g_fault_psp < FAULT_SRAM_HI) {
        volatile uint32_t *f = (volatile uint32_t *)g_fault_psp;
        _fault_puts("\r\nFault PC=");  _fault_puthex32(f[6]);
        _fault_puts("  LR=");          _fault_puthex32(f[5]);
        _fault_puts("  xPSR=");        _fault_puthex32(f[7]);
        _fault_puts("  R0=");          _fault_puthex32(f[0]);
        _fault_puts("  R12=");         _fault_puthex32(f[4]);
    } else {
        _fault_puts("\r\nPSP invalid (fault in Handler mode?)");
    }

    /* 出错任务名（指针先做 SRAM 范围校验，防二次 fault） */
    {
        uint32_t t = (uint32_t)(uintptr_t)g_current_task;
        if (t >= FAULT_SRAM_LO && t < FAULT_SRAM_HI) {
            const char *n = ((tcb_t *)(uintptr_t)t)->name;
            _fault_puts("\r\ntask='");
            for (int i = 0; i < 12 && n[i]; i++) _fault_putc(n[i]);
            _fault_putc('\'');
        }
    }
    _fault_puts("\r\n(v2.2.8 fault dump, UART0 115200-8N1)\r\n");
}
