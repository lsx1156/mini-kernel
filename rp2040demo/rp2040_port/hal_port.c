/* ================================================================
 * hal_port.c - RP2040 HAL 移植层实现
 *
 * 1) 系统滴答定时器（TIMER_IRQ_0，1ms 间隔 @ OS_CFG_TICK_HZ=1000）
 * 2) 调试串口（UART0 + USB CDC，统一通过 SDK stdio 接口输出）
 * 3) USB EP1 OUT 直接硬件读取（绕过 TinyUSB 接收软件层）
 * 4) GPIO / SPI / I2C / UART / SD 外设服务（按 OS_CFG 裁剪）
 * ================================================================ */
#include "hal_port.h"
#include "hal_interface.h"
#include "hal/sysclk.h"
#include "hal/config_store.h"  /* config_mcore_apply 强符号（覆盖 weak no-op） */
#include "os_config.h"
#include "mem.h"
#include "task.h"   /* tcb_t / g_current_task，供 HardFault dump 读取任务名 */

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/timer.h"
#include "hardware/irq.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "hardware/spi.h"
#include "hardware/i2c.h"
#include "hardware/sync.h"
#include "hardware/resets.h"
/* 注意：不 include hardware/sio.h / hardware/multicore.h（hal_port 的 include
 * 路径未暴露这些模块）。核心号用 SIO_CPUID 寄存器直读，core1 启动用
 * multicore_launch_core1 的前向声明（链接期由 pico_multicore 提供）。 */
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/pll.h"
#include "hardware/structs/ssi.h"
/* 注意：不直接 include "hardware/flash.h"，因为 Pico SDK hardware 子模块的
 *       头文件搜索路径并未在 shell_module 的 include 列表中暴露。
 *       flash_range_erase / flash_range_program 是 Pico SDK 提供的全局符号（
 *       boot2/link 时都会链接到 ROM 函数调用 trampoline），这里用 extern
 *       前向声明即可，void* 形参兼容 Pico SDK 的 const uint8_t* 形参。 */
extern void flash_range_erase(uint32_t flash_off, size_t count);
extern void flash_range_program(uint32_t flash_off, const void *data, size_t count);

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* TinyUSB 内部接口（运行期轮询调用，仅前向声明，不直接 include tusb.h
 * 以避免 TinyUSB 头文件路径依赖。Pico SDK 的 pico_stdio_usb 已链接TinyUSB
 * 库，这些符号在链接期由 libtinyusb.a 提供）。 */
extern void dcd_int_handler(uint8_t rhport);
extern void dcd_int_enable (uint8_t rhport);
extern void tud_task_ext(uint32_t timeout_ms, int in_isr);

/* CDC 行状态检测（v2.4.3）：tud_cdc_get_line_state 是 static inline 不可链接，
 * 改调其内部实现 tud_cdc_n_get_line_state(0)，返回 line_state 字节（bit0=DTR）。
 * 比抓 SETUP 包快照可靠：正常运行时 USB IRQ 会在轮询前先把
 * SET_CONTROL_LINE_STATE 处理掉。 */
extern uint8_t tud_cdc_n_get_line_state(uint8_t itf);
/* v2.7.1：中断正常（INTE!=0）时 shell 输入走 TinyUSB 标准接收，不再 bypass EP1
 * （避免与 USBCTRL_IRQ 中断竞争同一单缓冲端点描述符 → USB 失效）。 */
extern uint32_t tud_cdc_n_available(uint8_t itf);
extern uint32_t tud_cdc_n_read(uint8_t itf, void *buffer, uint32_t bufsize);

/* 【v2.6.4】启动期 FIFO/PSM 快照（实现见 rp2040_port/ipc/shmem_ipc.c，
 * 输出走 UART0/TTL——诊断专用，不依赖 USB/TinyUSB 状态） */
extern void ipc_boot_snap(const char *tag);

/* 内核回调：滴答中断尾部 */
extern void kernel_tick_hook(void);

/* ================================================================
 * 1. 系统滴答定时器（TIMER_IRQ_0 ← 使用 ALARM0，不与 SDK alarm_pool 冲突）
 *
 * ⚠️ 【v2.4.1 修复：爆闪根因 = ALARM3 冲突】
 *   反汇编 + SDK 源码证实：Pico SDK 默认 alarm_pool 使用
 *   PICO_TIME_DEFAULT_ALARM_POOL_HARDWARE_ALARM_NUM = **3**（ALARM3），
 *   即在 stdio_init_all() 时 claim ALARM3 并注册 TIMER_IRQ_3。
 *   旧注释"SDK 用 ALARM0、ALARM1/2/3 留给用户"是错误认知。
 *
 *   内核若也用 ALARM3 → hal_systick_init 里
 *   irq_set_exclusive_handler(TIMER_IRQ_3, systick_irq_handler) 时
 *   vtable 已被 SDK 占用 → hard_assert 失败 → panic →
 *   "*** PANIC *** Hard assert" → LED 爆闪。
 *
 * ✅ 修复：内核 tick 改用 ALARM0 + TIMER_IRQ_0。SDK 默认 pool 用 ALARM3，
 *   我们与它零冲突（不覆盖任何已注册 handler）。
 * ================================================================ */
volatile uint32_t g_tick_count[OS_CFG_CORE_COUNT] = {0,0}; /* 每核独立 tick 计数 */
volatile uint32_t g_tick_interval_us = 1000u; /* 默认 1ms @ 1000Hz */
volatile uint32_t g_core1_tick_count = 0;      /* 诊断：core1 实际产生多少次 tick */

/* 【多核】每个核心独立占一个 ALARM + 对应 IRQ，互不争抢硬件寄存器：
 *   core0 → ALARM0 + TIMER_IRQ_0
 *   core1 → ALARM1 + TIMER_IRQ_1
 * 由 hal_core_id() 在当前核心上取对应索引。alarm[0]/alarm[1] 是独立
 * 的硬件寄存器，两核各自写自己的 alarm、各自清自己的 intr 位，无竞争。 */
#define KTICK_ALARM_BASE        0u    /* core0 用 alarm[0]，core1 用 alarm[1] */
#define KTICK_IRQ_BASE          TIMER_IRQ_0
static inline uint32_t ktick_alarm_idx(void) { return KTICK_ALARM_BASE + hal_core_id(); }
static inline uint32_t ktick_bit(void)        { return (1u << ktick_alarm_idx()); }
static inline uint32_t ktick_irq(void)        { return KTICK_IRQ_BASE + hal_core_id(); }

/* TIMER_IRQ_n 中断处理：清中断 + 累计 tick + 重设本核 alarm + 调内核钩子。
 *
 *   RP2040 Timer 每个 ALARM 都是一次性触发：触发后 armed 位自动清 0，
 *   必须在中断里重写 alarm[N] 才能产生下一个周期。
 *   intr 寄存器是 write-1-to-clear，写 ktick_bit() 清本核 ALARM 的 flag。
 *   间隔基于 timerawl（当前计数器低 32 位）+ interval_us，避免漂移累积。 */
void systick_irq_handler(void) {
    /* 【致命 Bug 修复】INTR 是 write-1-to-clear 寄存器。
     * hw_clear_bits 做的是 *addr &= ~mask，对 W1C 寄存器等于写 0 → 不清中断！
     * 正确做法：直接写 ktick_bit() 到 INTR（写 1 清除对应位）。 */
    timer_hw->intr = ktick_bit();                     /* 清本核 ALARM 中断 (W1C) */
    timer_hw->alarm[ktick_alarm_idx()] = timer_hw->timerawl + g_tick_interval_us;
    g_tick_count[hal_core_id()]++;                    /* 只计本核 tick */
    if (hal_core_id() == 1) g_core1_tick_count++;      /* 诊断：core1 tick 计数 */
    kernel_tick_hook();                                /* 推进本核调度器 */
}

static void hal_systick_init_impl(uint32_t tick_hz) {
    if (tick_hz == 0u) tick_hz = 1000u;
    g_tick_interval_us = 1000000u / tick_hz;

    uint32_t bit  = ktick_bit();
    uint32_t irq  = ktick_irq();
    uint32_t idx  = ktick_alarm_idx();

    /* 关闭本核 alarm 中断使能 + 清 armed 标志，配置过程中不触发 IRQ。
     * ARMED 也是 write-1-to-clear，必须直接写 bit（不能用 hw_clear_bits）。 */
    hw_clear_bits(&timer_hw->inte, bit);
    timer_hw->armed = bit;

    /* 设置首次 alarm */
    timer_hw->alarm[idx] = timer_hw->timerawl + g_tick_interval_us;

    /* 注册 IRQ 处理函数 + 设优先级最低（不抢占 TinyUSB / SDK 关键中断） */
    irq_set_exclusive_handler(irq, systick_irq_handler);
    irq_set_priority(irq, 0xFFu);

    /* PendSV 优先级最低（0xE000ED22 = SHPR3[2] = EXC#14 PendSV），
     * 避免 PendSV 抢占 TIMER_IRQ_n tick 中断导致调度器重入、队列损坏。
     * Cortex-M0+ SHPR3 字节布局：[20]=#12 [21]=#13 [22]=#14 PendSV [23]=#15 SysTick */
    *(volatile uint8_t *)0xE000ED22u = 0xFFu;

    /* 开启本核 alarm INTE + NVIC */
    hw_set_bits(&timer_hw->inte, bit);
    irq_set_enabled(irq, true);
}

static uint32_t hal_systick_get_tick_impl(void) {
    return g_tick_count[hal_core_id()];   /* 返回本核 tick（core1 任务用 core1 计数 → FPS 正确） */
}

static void hal_systick_delay_us_impl(uint32_t us) {
    busy_wait_us_32(us);
}

const hal_systick_ops_t hal_systick_ops = {
    .init     = hal_systick_init_impl,
    .get_tick = hal_systick_get_tick_impl,
    .delay_us = hal_systick_delay_us_impl,
};

/* ================================================================
 * 2. 调试串口（UART0 + USB CDC via stdio_usb）
 * ================================================================ */

/* _usb_force_poll 前置声明：hal_usb_poll 在文件后面用到。*/
static inline void _usb_force_poll(void);

static void hal_console_init_impl(uint32_t baudrate) {
    (void)baudrate;
    stdio_init_all();
    ipc_boot_snap("B-after-usb");   /* 【v2.6.4】USB 枚举后的 FIFO/PSM 状态 */
}

/* 控制台输出：PRIMASK 保护 putchar_raw + 每 32 字符 yield 让 idle 刷新 USB FIFO。
 *
 * 【为什么需要 PRIMASK】putchar_raw 内部 Pico SDK stdio 状态不可重入，
 *   如果执行中被 TIMER_IRQ 打断、中断里又调 putc → 状态混乱 → HardFault。
 *
 * 【为什么需要定期 yield】CDC TX FIFO = 64B。shell 连续打印超过 64 字符时，
 *   如果不切到 idle 任务，hal_usb_poll 不会被调用 → FIFO 满 → 丢字符 → 截断。
 *   每 32 字符触发一次 PendSV yield，idle 任务获得 CPU 后调 hal_usb_poll
 *   刷新 TX FIFO，然后 shell 被调度回来继续打印。
 *
 * 【为什么不在 putc 里直接调 _usb_force_poll】_usb_force_poll 内部调
 *   dcd_int_handler / tud_task_ext，这些函数与 USB IRQ handler 重入时
 *   会导致 TinyUSB 内部状态混乱 → HardFault。idle 任务里的 hal_usb_poll
 *   是安全的设计上下文，不应该从其他任务直接调用。*/
/* ================================================================
 * 开机日志缓存回放（v2.4.3）
 *
 *  用户需求：无论何时打开终端，都能看到完整开机日志（之前超时=0 时
 *  开机打印一次性瞬间完成，终端没打开就全丢了）。
 *
 *  方案：
 *    · 启动阶段（控制台初始化 → sched_start 前）把每次 hal_console_putc
 *      的字符同步捕获进 RAM 缓冲（纯内存写，不阻塞、不丢）；
 *    · sched_start 前由内核调用 hal_bootlog_end() 停止捕获；
 *    · hal_usb_poll 里解析 CDC SET_CONTROL_LINE_STATE(0x21 0x22) 的
 *      DTR 上升沿（= 用户刚打开终端），把缓存的完整开机日志回放一遍。
 *      已在启动时开着的终端本来就能实时看到，不会触发回放（无上升沿）。
 * ================================================================ */
#define BOOTLOG_SIZE 2048u
static char             s_bootlog[BOOTLOG_SIZE];
static volatile uint16_t s_bootlog_len     = 0u;
static volatile bool    s_bootlog_capture  = true;   /* 启动阶段捕获中 */
static volatile bool    s_bootlog_replayed = false;  /* 已回放过（避免每次连接都重放） */
static volatile bool    s_bootlog_dtr_prev = false;  /* 最近一次检测到的 CDC DTR 状态 */
static volatile uint32_t s_bootlog_open_us  = 0u;    /* DTR 上升沿（设备就绪）时刻，us */

/* 控制台输出时同步捕获（由 hal_console_putc_impl 调用） */
static void hal_bootlog_putc(char c) {
    if (!s_bootlog_capture) return;
    if (s_bootlog_len < BOOTLOG_SIZE) {
        s_bootlog[s_bootlog_len++] = c;
    }
}

/* 停止捕获（内核在 sched_start 前调用 hal_bootlog_end，见 hal_interface.h） */
void hal_bootlog_end(void) {
    s_bootlog_capture = false;
    ipc_boot_snap("C-pre-sched");   /* 【v2.6.4】sched_start 前最后一刻 */
}

static void _bootlog_dtr_check(void);   /* 实现见文件后部（依赖 SETUP 快照） */

static int hal_console_putc_impl(char c) {
    static uint8_t s_yield_counter = 0;

    /* 开机日志捕获（内存写，放最前，与中断保护无关） */
    hal_bootlog_putc(c);

    /* 每 32 字符让出 CPU，让 idle 任务调 hal_usb_poll 刷新 FIFO */
    if (++s_yield_counter >= 32) {
        s_yield_counter = 0;
        hal_yield_trigger();
    }

    /* 关中断写入 1 字符（防止与 IRQ 里的 putc 重入）*/
    uint32_t pm;
    __asm volatile ("mrs %0, primask" : "=r" (pm) :: "memory");
    __asm volatile ("cpsid i" ::: "memory");
    int ret = (putchar_raw((unsigned char)c) == (unsigned char)c) ? 1 : 0;
    __asm volatile ("msr primask, %0" :: "r" (pm) : "memory");
    return ret;
}

/* —— USB 驱动的公共入口（运行期轮询）——
 *
 * 【暴力 INTE=0xFFFFFFFF 是锁死根因 · 已移除】
 * 上一版在这里写 *(0x50110014) = 0xFFFFFFFF 强制把 RP2040 USB 外设
 * 所有中断源都打开了，包括 SOF sent / VBUS detect / Resume signaling /
 * 未使用端点的 STALL 等 TinyUSB 根本没注册处理逻辑的中断位。
 * 这些位被硬件置 1 后产生 USBCTRL_IRQ，共享 handler 链里无人去清
 * INTF 对应位 → 中断被持续触发 → CPU 100% 跑 USB 中断死循环 →
 * 其他任务完全抢不到时间片 → 启动画面在 beat #1 截断卡死。
 *
 * 【正确认知 · INTE 只控制 NVIC 发不发 IRQ，不影响手动读数据】
 * RP2040 USB 寄存器有三对独立概念：
 *   BUFSTAT ：硬件端点缓冲区状态（EP1_OUT_AVAIL=bit2）——**纯只读**，
 *             主机发数据来它就置位，永远不受 INTE/ISER 影响
 *   INTF    ：中断状态 Flag（硬件置位）——**只读**，只要某个条件
 *             满足（EP 发送完成、SETUP 到达等）就会置位，与 INTE 无关
 *   INTE    ：Interrupt Enable（是否向 CPU 发 IRQ 信号）
 *   NVIC ISER：CPU 端是否响应某 IRQ 号
 * 所以当 TinyUSB 内部 dcd_int_disable() 把 INTE 清 0 / NVIC 关时：
 *     硬件 BUFSTAT / INTF 仍然会正确反映数据到达，
 *     只是不会触发 NVIC 中断而已。
 * 此时我们手动调 dcd_int_handler(0) → 它读 INTF / BUFSTAT 来处理 →
 * 数据照样能从端点 FIFO 搬到 TinyUSB 队列，完全不受 INTE 影响。
 * 且 dcd_int_handler 在 INTS=0（无待处理 Flag）时直接返回（幂等），
 * 即使 USB 硬件中断链路在正常跑，我们在 idle/shell/getc 里再手动
 * 调一次也不冲突。
 *
 * 结论：**永远不要手动改 INTE 和 NVIC ISER**，只在轮询里同步调用
 *   dcd_int_handler(0) + tud_task_ext(0,0)
 * 就能做到"硬件中断工作时不冲突、中断哑了手动也能捞数据"。 */
/* ================================================================
 * 2.5 【终极绕过】直接读 EP1 OUT 硬件端点，彻底绕过 SDK/TinyUSB 接收链路
 *
 * 背景：
 *   内核抢占调度（PendSV 任务切换）会让 TinyUSB 内部的
 *   dcd_int_disable() 执行后、配对的 dcd_int_enable() 没被走回来
 *   → INTE=0 + ISERb21=0 → INTS=INTF&INTE 永远=0 →
 *   dcd_int_handler() 直接 return → SDK 的 getchar_timeout_us()、
 *   tud_cdc_n_read() 全读不到数据。之前 6+ 版修复都在"让 TinyUSB
 *   重新进入正确状态"上打补丁，反复失败。
 *
 * 方案：
 *   完全绕过所有 SDK/TinyUSB 的接收软件层，直接操作 RP2040 USB
 *   硬件寄存器 + USB DPRAM 的 EP1 OUT 描述符：
 *     1. BUFSTAT.EP1_OUT_AVAIL(bit2)==1 → 有主机发来的包
 *     2. ADDR_ENDP1 = DPRAM 偏移量的 EP1 描述符地址
 *     3. 从 DPRAM[desc_off] 读 buf_ctrl (length + available flag)
 *     4. 从 DPRAM[desc_off+4] 读 buf_addr (数据区 DPRAM 偏移)
 *     5. 从 DPRAM[buf_addr] 读 length 个字节 → 进私有 ring buffer
 *     6. 清 DPRAM[desc_off] bit31 (=0, "软件已拿走，硬件可接收下一包")
 *     7. hal_console_getc_impl 直接从私有 ring buffer 取字节
 * 发送仍然走 SDK 的 putchar()（之前的所有版本输出一直正常）。
 * 枚举阶段仍然保留 USBCTRL_IRQ 共享链路（开着就行，不动它）。
 * ================================================================ */
#define USB_DPRAM_BASE    0x50100000u   /* RP2040 USB DPRAM 基址 */
#define USB_REGS_BASE     0x50110000u
#define USB_ADDR_ENDP1    0x50110098u   /* EP1 描述符偏移寄存器 */
#define USB_BUFSTAT       0x50110058u   /* Buffer Status */
#define EP1_OUT_AVAIL_BIT (1u << 2)     /* BUFSTAT bit2 = EP1 OUT 有包 */
#define EP1_BUF_CTRL_AVAIL (1u << 31)   /* 描述符 ctrl bit31: OUT=1时"硬件已收到,软件未读" */

/* 私有 ring buffer：128 字节足够，比 EPx 包大小（64B）的 2 倍还多 */
#define RX_RING_SIZE 128u
static uint8_t  s_rx_ring[RX_RING_SIZE];
static volatile uint8_t s_rx_head; /* 写位置（硬件读到的新字节写这里） */
static volatile uint8_t s_rx_tail; /* 读位置（hal_console_getc 从这取） */

/* 一次从 EP1 OUT 硬件端点搬一个包到私有 ring buffer。
 * 返回这次搬了多少字节（包长），0 表示 BUFSTAT 上没有可用包。
 *
 * 【并发保护】s_rx_ring 由本函数（生产者，可能在 hal_usb_poll 中被
 *   idle/heartbeat/shell 调用）和 hal_console_getc_impl（消费者，被
 *   shell 调用）共同访问。Cortex-M0+ 单核 + 抢占式调度下，ring 读写
 *   不原子，必须用 PRIMASK 临界区保护，否则 head/tail 错位导致数据
 *   丢失或重复。 */
static inline uint32_t _ep1_out_drain_one(void) {
    if ((*(volatile uint32_t *)USB_BUFSTAT & EP1_OUT_AVAIL_BIT) == 0u) {
        return 0;  /* 硬件端点没包 */
    }

    /* 1. 取 EP1 的描述符偏移（ADDR_ENDP1 存的是 DPRAM 偏移量，不是绝对地址） */
    uint32_t desc_off = *(volatile uint32_t *)USB_ADDR_ENDP1;
    /* RP2040 手册：ADDR_ENDPx 的值是"从 USB_DPRAM_BASE 开始的字节偏移"，
     * 每个端点 OUT 描述符 8 字节。也可能 TinyUSB 把描述符放在不同地方，
     * 所以先判断偏移有效性（DPRAM 是 4KB，偏移 < 0x1000）。 */
    if (desc_off >= 0x1000u) return 0;

    volatile uint32_t *ctrl = (volatile uint32_t *)(USB_DPRAM_BASE + desc_off);
    volatile uint32_t *aptr = (volatile uint32_t *)(USB_DPRAM_BASE + desc_off + 4u);

    uint32_t buf_ctrl = *ctrl;
    if ((buf_ctrl & EP1_BUF_CTRL_AVAIL) == 0u) {
        /* 硬件可能已被其他路径处理过（共享 handler 链），安全退出 */
        return 0;
    }

    uint32_t length = buf_ctrl & 0xFFFFu;     /* 实际字节数（通常<=64） */
    uint32_t data_off = *aptr & 0xFFFFu;      /* 数据区 DPRAM 偏移 */
    if (data_off >= 0x1000u || length > 64u) {
        /* 异常值：强制释放包，避免端点死锁 */
        *ctrl = buf_ctrl & ~EP1_BUF_CTRL_AVAIL;
        return 0;
    }
    volatile uint8_t *src = (volatile uint8_t *)(USB_DPRAM_BASE + data_off);

    /* 2. 逐字节塞进私有 ring buffer（临界区保护 head/ring 写入） */
    uint32_t primask = save_and_disable_interrupts();
    for (uint32_t i = 0; i < length; i++) {
        uint8_t b = src[i];
        uint8_t next_head = (uint8_t)((s_rx_head + 1u) & (RX_RING_SIZE - 1u));
        if (next_head == s_rx_tail) {
            /* ring 满：丢弃剩余字节（让主机重传） */
            break;
        }
        s_rx_ring[s_rx_head] = b;
        s_rx_head = next_head;
    }
    restore_interrupts(primask);

    /* 3. 关键：清 bit31，告诉硬件"软件已拿走，端点 OUT 可以接收下一包" */
    *ctrl = buf_ctrl & ~EP1_BUF_CTRL_AVAIL;
    return length;
}

/* 尝试把 EP1 OUT 当前所有挂起的包都搬进来。
 * RP2040 双缓冲，最多连续 2 包。 */
static inline void _ep1_out_drain_all(void) {
    for (uint32_t i = 0; i < 2u; i++) {
        if (_ep1_out_drain_one() == 0u) break;
    }
}

/* 驱动 USB 状态机 + 端点硬件搬运（供 idle/shell/getc/heartbeat 调用）。
 *
 * 【SETUP/Control Request 必须被处理才能让 Host 发 EP1 OUT】
 *   PuTTY 打开串口时 Windows 会先对 CDC 接口发 2 个 Class Control Request：
 *     SET_LINE_CODING (0x20) + SET_CONTROL_LINE_STATE (0x22, DTR=1)
 *   这些走 EP0（Control）端点 SETUP/OUT/IN 序列。设备如果不应答，
 *   Windows 认为 CDC 接口"未就绪"，**绝对不会向 EP1 OUT 发任何数据包**
 *   → BUFSTAT.EP1_OUT_AVAIL 永远=0 → H=0 R=0。
 *
 * 【条件式 INTE 恢复（v19 修复）】
 *   v18 无条件每次 poll 都写 INTE 掩码，但会和 TinyUSB 自己的 dcd_int_enable/
 *   dcd_int_disable 调用产生竞争（覆盖 TinyUSB 刚刚设置的掩码值）。
 *   正确做法：只在 INTE==0（即 dcd_int_disable 被抢占未配对恢复）时才干预，
 *   否则尊重 TinyUSB 自己的管理。这样：
 *     - 正常情况 INTE != 0：完全不动 INTE，避免覆盖 SDK 设置；
 *     - 异常情况 INTE == 0：先调 dcd_int_enable 让 SDK 重建，失败再写安全掩码。
 *
 * 安全掩码：只用 TinyUSB 的 dcd_rp2040_irq 有 ack 代码的位，避免 SOF 占满 CPU。
 *   【v2.4.3-fix · 掩码位号全面纠错，修复 "Unhandled IRQ 0x%x" PANIC】
 *   旧掩码 0x0009004D 的位号解读全错（把 HOST 模式位当成了设备模式位）：
 *     - 误开了 TinyUSB 未处理的位：EP_STALL_NAK(bit19 0x80000)、
 *       ERROR_RX_TIMEOUT(bit6 0x40)、TRANS_COMPLETE(bit3 0x8)、
 *       HOST_SOF(bit2 0x4)、HOST_CONN_DIS(bit0 0x1)。
 *       dcd_rp2040_irq 读到 `status ^ handled != 0` → panic("Unhandled IRQ 0x%x")
 *       （主机在设备忙/未 re-arm 时轮询 EP1 OUT → 硬件 NAK → EP_STALL_NAK 置位
 *       → 直接 PANIC；重负载 + 抢占调度下极易触发）。
 *     - 同时漏了 BUFF_STATUS(bit4)/BUS_RESET(bit12)/DEV_SUSPEND(bit14)/
 *       DEV_RESUME(bit15)，CDC IN 发送完成中断收不到。
 *   正确掩码 = TinyUSB dcd_init 使能且 dcd_rp2040_irq 全部处理的位：
 *     BUFF_STATUS(bit4 0x10) | BUS_RESET(bit12 0x1000) |
 *     DEV_CONN_DIS(bit13 0x2000) | DEV_SUSPEND(bit14 0x4000) |
 *     DEV_RESUME_FROM_HOST(bit15 0x8000) | SETUP_REQ(bit16 0x10000)
 *   = 0x0001_F010  （不含 DEV_SOF：TinyUSB 运行期按需开/关） */
#define USB_INTE_SAFE_MASK 0x0001F010u
#define USB_INTE_REG       0x50110014u   /* USB_INTE (Interrupt Enable) */
#define USB_INTS_REG       0x50110010u   /* USB_INTS = raw_flags & INTE (只读) */
#define USB_EP0_SETUP_BIT  (1u << 16)    /* INTS bit16 = EP0_SETUP_REQ pending（旧 1u<<6 是 ERROR_RX_TIMEOUT） */
#define USB_EP1_BUFF_BIT   (1u << 4)     /* INTS bit4  = BUFF_STATUS pending（旧 1u<<2 是 HOST_SOF） */

/* ===== 诊断统计计数器 ===== */
static volatile uint32_t s_poll_dcd_enable_count = 0;
static volatile uint32_t s_poll_mask_write_count  = 0;
static volatile uint32_t s_dcd_handler_called     = 0;

/* SETUP 包检测：必须在 dcd_int_handler 之前读 INTS，因为 handler 会清位。
 * INTS = raw_interrupt_flags & INTE，所以必须先写 INTE 再读 INTS。
 * RP2040: SETUP 包 8 字节固定存在 DPRAM 偏移 0x000~0x007。 */
static volatile uint32_t s_last_setup_w0 = 0, s_last_setup_w1 = 0;
static volatile uint32_t s_setup_count = 0;

uint32_t hal_usb_diag_setup_count(void)       { return s_setup_count; }
uint32_t hal_usb_diag_setup_w0(void)          { return s_last_setup_w0; }
uint32_t hal_usb_diag_setup_w1(void)          { return s_last_setup_w1; }
uint32_t hal_usb_diag_mask_write_count(void)  { return s_poll_mask_write_count; }
uint32_t hal_usb_diag_dcd_handler_count(void) { return s_dcd_handler_called; }

/* 在 INTE 已写好之后、dcd_int_handler 之前调用：
 * 读 INTS 检查 EP0_SETUP_REQ 是否挂起，若挂起则抓 DPRAM SETUP 内容。 */
static inline void _snapshot_setup_if_pending(void) {
    uint32_t ints = *(volatile uint32_t *)USB_INTS_REG;
    if (ints & USB_EP0_SETUP_BIT) {
        /* INTS bit6=1 → 有 SETUP 包待处理，DPRAM 0~7 存了 8 字节 */
        uint32_t w0 = *(volatile uint32_t *)0x50100000u;
        uint32_t w1 = *(volatile uint32_t *)0x50100004u;
        if (w0 != s_last_setup_w0 || w1 != s_last_setup_w1) {
            s_last_setup_w0 = w0;
            s_last_setup_w1 = w1;
            s_setup_count++;
        }
    }
}

/* 十进制打印（走 hal_console_putc，仅回放标记/诊断用） */
static void _bootlog_put_u32(uint32_t v) {
    char b[11]; int n = 0;
    if (v == 0) { hal_console_putc('0'); return; }
    while (v > 0) { b[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) hal_console_putc(b[--n]);
}

/* 回放完整开机日志（仅一次；经控制台输出，半阻塞保证送达）。
 * 开头打印 [BOOTLOG] 标记 + 缓存字节数，用于区分"实时输出"与"回放"、
 * 并诊断缓存是否抓全（若 N 远小于预期 = 捕获阶段输出就被截断）。
 *
 * 【关键守卫】s_bootlog_capture 仍为真（启动流程未结束）时**禁止回放**：
 * 若终端在启动时就开着，hal_usb_poll 会立刻检测到 DTR 上升沿 → 启动中途
 * 触发回放 → 回放输出又走 hal_console_putc → hal_bootlog_putc 重新捕获 →
 * 缓存被污染 + 回放与实时输出交织 → 满屏乱码。 */
static void _bootlog_replay(void) {
    if (s_bootlog_capture) return;   /* 启动中不回放（避免重捕获污染缓存） */
    if (s_bootlog_replayed) return;
    if (s_bootlog_len == 0u) return;
    s_bootlog_replayed = true;
    hal_console_putc('\r'); hal_console_putc('\n');
    const char *h1 = "[BOOTLOG] replay ";
    while (*h1) hal_console_putc(*h1++);
    _bootlog_put_u32((uint32_t)s_bootlog_len);
    const char *h2 = " bytes:\r\n";
    while (*h2) hal_console_putc(*h2++);
    for (uint16_t i = 0; i < s_bootlog_len; i++) {
        hal_console_putc(s_bootlog[i]);
    }
}

/* ---- 开机日志回放诊断（shell `bootlog` 命令用） ---- */
uint32_t hal_bootlog_captured(void)  { return (uint32_t)s_bootlog_len; }
uint32_t hal_bootlog_replayed(void)  { return s_bootlog_replayed ? 1u : 0u; }
uint32_t hal_bootlog_capturing(void) { return s_bootlog_capture ? 1u : 0u; }
uint32_t hal_bootlog_dtr_prev(void)  { return s_bootlog_dtr_prev ? 1u : 0u; }

/* 由 hal_usb_poll 调用：直接读 TinyUSB CDC 行状态的 DTR 位，检测上升沿。
 * DTR 0→1 = 用户刚打开终端（设备就绪）→ 记录就绪时刻。
 * 【v2.4.3-fix】回放不再在这里（idle 任务上下文）执行：
 *   idle 栈仅 OS_CFG_IDLE_STACK_SIZE(1024B)，而回放输出链
 *   hal_console_putc → putchar_raw → stdio_usb_out_chars → tud_cdc_n_write
 *   需要 ~2048B 栈，在 idle 里跑会击穿栈底 MAGIC → idle 行为异常 →
 *   串口 dump 内存乱码（症状：FAT 扇区 / task 名 / 内存内容被 dump）。
 *   改为：idle 只记录"就绪时刻"，由 shell 任务（栈 2048B）在
 *   hal_bootlog_try_replay() 里安全执行回放。
 * 之前用 SETUP 包快照解析（s_last_setup_w0）不可靠：正常运行时 USB IRQ
 * 会在本函数轮询前先把 SET_CONTROL_LINE_STATE 处理掉，快照永远抓不到。 */
#define BOOTLOG_REPLAY_DELAY_US  3000000u   /* 设备就绪后第 3 秒才开始回放 */

static void _bootlog_dtr_check(void) {
    bool dtr = (tud_cdc_n_get_line_state(0) & 0x01u) != 0u;   /* bit0 = DTR */
    if (dtr && !s_bootlog_dtr_prev) {
        s_bootlog_open_us = time_us_32();   /* 设备就绪时刻（DTR 0→1） */
    } else if (!dtr) {
        s_bootlog_open_us = 0u;             /* 终端关闭 → 取消未到期的回放 */
    }
    s_bootlog_dtr_prev = dtr;
}

/* 由 shell 任务调用（栈 2048B，安全）：设备就绪后满 3 秒才回放开机日志。
 * 返回 0 = 未到期/未就绪/已回放；1 = 本次已执行回放。 */
uint32_t hal_bootlog_try_replay(void) {
    bool dtr = (tud_cdc_n_get_line_state(0) & 0x01u) != 0u;
    if (!dtr) return 0u;                    /* 终端没开 */
    if (s_bootlog_open_us == 0u) return 0u; /* 未记录就绪时刻 */
    if ((uint32_t)(time_us_32() - s_bootlog_open_us) < BOOTLOG_REPLAY_DELAY_US) {
        return 0u;                          /* 未满 3 秒 */
    }
    _bootlog_replay();                      /* 满 3 秒 → 安全回放（幂等，内部查 replayed） */
    return 1u;
}

uint32_t hal_usb_force_poll_and_snapshot(uint32_t *inte_after, uint32_t *iser_after,
                                         uint32_t *safe_mask_written,
                                         uint32_t *dcd_enable_calls) {
    /* 1. 仅在 INTE==0 时恢复掩码（避免覆盖 TinyUSB 正常管理的 INTE 值） */
    uint32_t mask_written = 0u;
    uint32_t inte_before = *(volatile uint32_t *)USB_INTE_REG;
    if (inte_before == 0u) {
        /* INTE 被任务抢占期间 dcd_int_disable() 清零，TinyUSB 自身无法
         * 走回 dcd_int_enable() → 手动恢复。先调 dcd_int_enable 让 SDK
         * 重建掩码（包含 EP0/EP1 等它管理的位），失败再写安全掩码兜底。 */
        dcd_int_enable(0);
        s_poll_dcd_enable_count++;
        if (*(volatile uint32_t *)USB_INTE_REG == 0u) {
            *(volatile uint32_t *)USB_INTE_REG = USB_INTE_SAFE_MASK;
            s_poll_mask_write_count++;
            mask_written = 1u;
        }
        /* 同时确保 NVIC ISER bit21=1（dcd_int_enable 也可能因抢占未生效） */
        if (((*(volatile uint32_t *)0xE000E100u >> 21) & 1u) == 0u) {
            *(volatile uint32_t *)0xE000E100u = (1u << 21);
        }
    }

    /* 2. 在 dcd_int_handler 之前抓 SETUP 快照（handler 会清 INTS 位）。
     * 【v2.4.3-fix】仅当 INTE==0（中断失效）才手动调 dcd_int_handler 兜底；
     * 中断正常驱动时不手动处理，避免与 USBCTRL_IRQ 中断双路径竞态 → HardFault。 */
    if (*(volatile uint32_t *)USB_INTE_REG == 0u) {
        _snapshot_setup_if_pending();
        dcd_int_handler(0);
        s_dcd_handler_called++;
    }

    /* 3. 推进 TinyUSB 软件状态机 + 手动搬 EP1 OUT → ring */
    tud_task_ext(0, 0);
    _ep1_out_drain_all();

    if (inte_after)  *inte_after  = *(volatile uint32_t *)USB_INTE_REG;
    if (iser_after)  *iser_after  = (*(volatile uint32_t *)0xE000E100u >> 21) & 1u;
    if (safe_mask_written) *safe_mask_written = mask_written;
    if (dcd_enable_calls)   *dcd_enable_calls   = s_poll_dcd_enable_count;
    return 0;
}

static inline void _usb_force_poll(void) {
    /* 【v2.4.3-fix · 消除 USB 中断双路径竞态 → HardFault 爆闪】
     *   USBCTRL_IRQ 硬件中断（dcd_rp2040_irq）与轮询 dcd_int_handler 是同一函数。
     *   若 INTE!=0（中断正常驱动）时轮询仍无条件调 dcd_int_handler，则重开 PuTTY
     *   触发 bus reset / 重新枚举时两条路径同时处理同一批事件 → TinyUSB 状态损坏
     *   → HardFault → LED 5Hz 爆闪。
     *   修复：仅当 INTE==0（中断被抢占清零、失效）时才手动调 dcd_int_handler 兜底；
     *   中断正常时只推进软件状态机 + EP1 旁路搬运，单一路径。 */
    if (*(volatile uint32_t *)USB_INTE_REG == 0u) {
        /* INTE==0 → TinyUSB 中断路径失效，手动恢复掩码 + 手动处理硬件事件（兜底） */
        dcd_int_enable(0);
        s_poll_dcd_enable_count++;
        if (*(volatile uint32_t *)USB_INTE_REG == 0u) {
            *(volatile uint32_t *)USB_INTE_REG = USB_INTE_SAFE_MASK;
            s_poll_mask_write_count++;
        }
        if (((*(volatile uint32_t *)0xE000E100u >> 21) & 1u) == 0u) {
            *(volatile uint32_t *)0xE000E100u = (1u << 21);
        }
        _snapshot_setup_if_pending();
        dcd_int_handler(0);
        s_dcd_handler_called++;
        /* 【v2.7.1 关键修复】EP1 绕过只在 INTE==0（中断失效）时兜底搬运。
         * 之前无条件调用 _ep1_out_drain_all()，即使 INTE!=0 也直接操作 EP1 OUT
         * 端点描述符（读数据 + 清 AVAIL），与 USBCTRL_IRQ 中断（dcd_rp2040_irq）
         * 竞争同一个单缓冲端点 → 描述符状态损坏 → USB 控制器异常 → 电脑无法识别。
         * 中断正常时 EP1 由 TinyUSB 中断独占处理，轮询不再触碰。 */
        _ep1_out_drain_all();
    }
    /* INTE!=0：硬件中断正常驱动硬件事件；轮询只推进 TinyUSB 软件状态机 */
    tud_task_ext(0, 0);
}

static int hal_console_getc_impl(char *c) {
    _usb_force_poll();
    /* 【v2.7.1 关键修复】中断正常（INTE!=0）时用 TinyUSB 标准接收（tud_cdc_n_*
     * 内部由 dcd_rp2040_irq 中断把 EP1 数据搬进 TU FIFO），不再从私有 ring 读。
     * 之前 shell 输入依赖 _ep1_out_drain_all() 绕过 EP1，与中断竞争破坏端点状态。
     * 仅当 INTE==0（中断失效）才退回私有 ring（bypass 兜底）。 */
    if (*(volatile uint32_t *)USB_INTE_REG != 0u) {
        if (tud_cdc_n_available(0)) {
            uint8_t b;
            if (tud_cdc_n_read(0, &b, 1) == 1u) { *c = (char)b; return 1; }
        }
        return 0;
    }
    /* INTE==0（中断失效）：直接用私有 ring buffer，不碰 getchar/tud_cdc_read
     * （这两个 API 依赖 TinyUSB 内部接收软件层，已被 INTE=0 关死）。 */
    uint32_t primask = save_and_disable_interrupts();
    if (s_rx_head == s_rx_tail) {
        restore_interrupts(primask);
        return 0;   /* 空 */
    }
    *c = s_rx_ring[s_rx_tail];
    s_rx_tail = (uint8_t)((s_rx_tail + 1u) & (RX_RING_SIZE - 1u));
    restore_interrupts(primask);
    return 1;
}

/* 给心跳诊断用：暴露当前 ring buffer 可读字节数 + BUFSTAT.EP1_OUT_AVAIL 原值 */
uint32_t hal_usb_diag_ep1_ring_count(void) {
    uint32_t head = s_rx_head, tail = s_rx_tail;
    return (head - tail) & (RX_RING_SIZE - 1u);
}
uint32_t hal_usb_diag_ep1_hw_avail(void) {
    return (*(volatile uint32_t *)USB_BUFSTAT & EP1_OUT_AVAIL_BIT) ? 1u : 0u;
}

/* idle/shell/heartbeat 等任务调用：驱动 USB 状态机，保持 USB CDC 双向通畅。 */
void hal_usb_poll(void) {
    _usb_force_poll();
    /* 检测终端是否刚打开（DTR 上升沿）→ 回放开机日志（v2.4.3） */
    _bootlog_dtr_check();
}

const hal_console_ops_t hal_console_ops = {
    .init  = hal_console_init_impl,
    .putc  = hal_console_putc_impl,
    .getc  = hal_console_getc_impl,
};

/* ================================================================
 * 3. GPIO 实现
 * ================================================================ */
#if OS_CFG_PERIPH_SERVICE

static hal_err_t hal_gpio_init_impl(uint32_t pin, hal_gpio_mode_t mode, uint32_t af) {
    if (pin >= NUM_BANK0_GPIOS) return HAL_ERR_INVAL;

    switch (mode) {
        case HAL_GPIO_IN:
            gpio_init(pin);
            gpio_set_dir(pin, GPIO_IN);
            break;
        case HAL_GPIO_OUT_PP:
            gpio_init(pin);
            gpio_set_dir(pin, GPIO_OUT);
            break;
        case HAL_GPIO_OUT_OD:
            gpio_init(pin);
            gpio_set_dir(pin, GPIO_OUT);
            gpio_set_outover(pin, GPIO_OVERRIDE_LOW);  /* 开漏模拟 */
            break;
        case HAL_GPIO_AF:
            gpio_set_function(pin, (enum gpio_function)af);
            break;
        default:
            return HAL_ERR_INVAL;
    }
    return HAL_OK;
}

static void hal_gpio_write_impl(uint32_t pin, hal_gpio_level_t level) {
    if (pin < NUM_BANK0_GPIOS) {
        gpio_put(pin, level);
    }
}

static hal_gpio_level_t hal_gpio_read_impl(uint32_t pin) {
    if (pin < NUM_BANK0_GPIOS) {
        return gpio_get(pin) ? HAL_GPIO_HIGH : HAL_GPIO_LOW;
    }
    return HAL_GPIO_LOW;
}

static void hal_gpio_toggle_impl(uint32_t pin) {
    if (pin < NUM_BANK0_GPIOS) {
        gpio_xor_mask(1u << pin);
    }
}

static hal_err_t hal_gpio_lock_impl(uint32_t pin) {
    /* RP2040 无硬件锁定，软件层面可扩展 */
    (void)pin;
    return HAL_OK;
}

const hal_gpio_ops_t hal_gpio_ops = {
    .init   = hal_gpio_init_impl,
    .write  = hal_gpio_write_impl,
    .read   = hal_gpio_read_impl,
    .toggle = hal_gpio_toggle_impl,
    .lock   = hal_gpio_lock_impl,
};

/* ================================================================
 * 4. SPI 实现
 * ================================================================ */
static hal_err_t hal_spi_init_impl(uint32_t bus_id, uint32_t hz, uint8_t mode, uint8_t bits) {
    spi_inst_t *spi = (bus_id == 0) ? spi0 : spi1;
    if (spi == NULL) return HAL_ERR_INVAL;

    spi_init(spi, hz);
    spi_set_format(spi, bits, (spi_cpol_t)(mode & 1), (spi_cpha_t)((mode >> 1) & 1), SPI_MSB_FIRST);
    return HAL_OK;
}

static hal_err_t hal_spi_xfer_impl(uint32_t bus_id, const uint8_t *tx, uint8_t *rx, size_t len) {
    spi_inst_t *spi = (bus_id == 0) ? spi0 : spi1;
    if (spi == NULL) return HAL_ERR_INVAL;

    if (tx && rx) {
        spi_write_read_blocking(spi, tx, rx, len);
    } else if (tx) {
        spi_write_blocking(spi, tx, len);
    } else if (rx) {
        spi_read_blocking(spi, 0xFF, rx, len);
    }
    return HAL_OK;
}

static hal_err_t hal_spi_cs_ctrl_impl(uint32_t bus_id, uint8_t cs_pin, uint8_t active) {
    if (cs_pin < NUM_BANK0_GPIOS) {
        gpio_put(cs_pin, active ? 0 : 1);
    }
    return HAL_OK;
}

const hal_spi_ops_t hal_spi_ops = {
    .init    = hal_spi_init_impl,
    .xfer    = hal_spi_xfer_impl,
    .cs_ctrl = hal_spi_cs_ctrl_impl,
};

/* ================================================================
 * 5. I2C 实现
 * ================================================================ */
static hal_err_t hal_i2c_init_impl(uint32_t bus_id, uint32_t hz) {
    i2c_inst_t *i2c = (bus_id == 0) ? i2c0 : i2c1;
    if (i2c == NULL) return HAL_ERR_INVAL;
    i2c_init(i2c, hz);

    /* Pico SDK 的 i2c_init 不会自动配置 GPIO 引脚功能。
     * 必须手动把 SDA/SCL 引脚切换到 I2C 功能 (GPIO_FUNC_I2C)。*/
    uint8_t sda_pin = (bus_id == 0) ? 4 : 6;   /* I2C0: GP4/GP5, I2C1: GP6/GP7 */
    uint8_t scl_pin = (bus_id == 0) ? 5 : 7;
    gpio_set_function(sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(scl_pin, GPIO_FUNC_I2C);

    return HAL_OK;
}

static hal_err_t hal_i2c_master_tx_impl(uint32_t bus_id, uint8_t addr, const uint8_t *buf, size_t len) {
    i2c_inst_t *i2c = (bus_id == 0) ? i2c0 : i2c1;
    if (i2c == NULL) return HAL_ERR_INVAL;
    /* 【超时保护】i2c_write_blocking 在总线挂死（SCL 被拉低）时会永久阻塞，
     *   导致 VT2 卡死、无法打印诊断。改用 timeout 版本：100kHz 下
     *   单字节 ~90us，1024B GDRAM ≈ 92ms，命令帧 <5ms。给 200ms 总超时
     *   足够覆盖最大数据帧，同时防止永久卡死。*/
    int ret = i2c_write_timeout_us(i2c, addr, buf, len, false, 200000u);
    return (ret == (int)len) ? HAL_OK : HAL_ERR_IO;
}

static hal_err_t hal_i2c_master_rx_impl(uint32_t bus_id, uint8_t addr, uint8_t *buf, size_t len) {
    i2c_inst_t *i2c = (bus_id == 0) ? i2c0 : i2c1;
    if (i2c == NULL) return HAL_ERR_INVAL;
    int ret = i2c_read_timeout_us(i2c, addr, buf, len, false, 200000u);
    return (ret == (int)len) ? HAL_OK : HAL_ERR_IO;
}

static hal_err_t hal_i2c_mem_read_impl(uint32_t bus_id, uint8_t addr, uint16_t mem_addr, uint8_t *buf, size_t len) {
    i2c_inst_t *i2c = (bus_id == 0) ? i2c0 : i2c1;
    if (i2c == NULL) return HAL_ERR_INVAL;
    uint8_t reg[2] = { (uint8_t)(mem_addr >> 8), (uint8_t)mem_addr };
    int ret = i2c_write_timeout_us(i2c, addr, reg, 2, true, 50000u);
    if (ret != 2) return HAL_ERR_IO;
    ret = i2c_read_timeout_us(i2c, addr, buf, len, false, 200000u);
    return (ret == (int)len) ? HAL_OK : HAL_ERR_IO;
}

static hal_err_t hal_i2c_mem_write_impl(uint32_t bus_id, uint8_t addr, uint16_t mem_addr, const uint8_t *buf, size_t len) {
    i2c_inst_t *i2c = (bus_id == 0) ? i2c0 : i2c1;
    if (i2c == NULL) return HAL_ERR_INVAL;
    /* 使用内核堆 kmalloc 而非 libc malloc，保证所有动态分配都在
     * kmem 统计范围内，便于泄漏检测与体积约束。 */
    uint8_t *tx = kmalloc(len + 2);
    if (!tx) return HAL_ERR_NOMEM;
    tx[0] = (uint8_t)(mem_addr >> 8);
    tx[1] = (uint8_t)mem_addr;
    memcpy(&tx[2], buf, len);
    int ret = i2c_write_timeout_us(i2c, addr, tx, len + 2, false, 200000u);
    kfree(tx);
    return (ret == (int)(len + 2)) ? HAL_OK : HAL_ERR_IO;
}

const hal_i2c_ops_t hal_i2c_ops = {
    .init        = hal_i2c_init_impl,
    .master_tx   = hal_i2c_master_tx_impl,
    .master_rx   = hal_i2c_master_rx_impl,
    .mem_read    = hal_i2c_mem_read_impl,
    .mem_write   = hal_i2c_mem_write_impl,
};

/* ================================================================
 * 6. UART 实现（异步缓冲）
 * ================================================================ */
#define UART_RX_BUF_SIZE  128

typedef struct {
    uint8_t buf[UART_RX_BUF_SIZE];
    volatile uint16_t head, tail;
    void (*cb)(uint8_t, void *);
    void *cb_arg;
} uart_rx_ctx_t;

static uart_rx_ctx_t uart_ctx[2] = {0};

static void uart_irq_handler(void) {
    for (int i = 0; i < 2; i++) {
        uart_inst_t *uart = (i == 0) ? uart0 : uart1;
        if (uart_is_readable(uart)) {
            uint8_t byte = uart_getc(uart);
            uart_rx_ctx_t *ctx = &uart_ctx[i];
            uint16_t next = (ctx->head + 1) % UART_RX_BUF_SIZE;
            if (next != ctx->tail) {
                ctx->buf[ctx->head] = byte;
                ctx->head = next;
            }
            if (ctx->cb) ctx->cb(byte, ctx->cb_arg);
        }
    }
}

static hal_err_t hal_uart_init_impl(uint32_t uart_id, uint32_t baud, uint8_t parity, uint8_t stop_bits) {
    uart_inst_t *uart = (uart_id == 0) ? uart0 : uart1;
    if (uart == NULL) return HAL_ERR_INVAL;

    uart_init(uart, baud);
    uart_set_format(uart, 8, stop_bits, (uart_parity_t)parity);
    uart_set_fifo_enabled(uart, true);
    uart_set_irq_enables(uart, true, false);
    irq_set_exclusive_handler((uart_id == 0) ? UART0_IRQ : UART1_IRQ, uart_irq_handler);
    irq_set_enabled((uart_id == 0) ? UART0_IRQ : UART1_IRQ, true);
    return HAL_OK;
}

static hal_err_t hal_uart_write_impl(uint32_t uart_id, const uint8_t *buf, size_t len, uint32_t timeout_ms) {
    uart_inst_t *uart = (uart_id == 0) ? uart0 : uart1;
    if (uart == NULL) return HAL_ERR_INVAL;
    uint32_t start = timer_hw->timerawl;
    size_t written = 0;
    while (written < len) {
        if (uart_is_writable(uart)) {
            uart_putc_raw(uart, buf[written++]);
        }
        if ((timer_hw->timerawl - start) >= timeout_ms * 1000) break;
    }
    return (written == len) ? HAL_OK : HAL_ERR_TIMEOUT;
}

static hal_err_t hal_uart_read_impl(uint32_t uart_id, uint8_t *buf, size_t len, uint32_t timeout_ms) {
    uart_inst_t *uart = (uart_id == 0) ? uart0 : uart1;
    if (uart == NULL) return HAL_ERR_INVAL;
    uart_rx_ctx_t *ctx = &uart_ctx[uart_id];
    uint32_t start = timer_hw->timerawl;
    size_t read = 0;
    while (read < len) {
        if (ctx->tail != ctx->head) {
            buf[read++] = ctx->buf[ctx->tail];
            ctx->tail = (ctx->tail + 1) % UART_RX_BUF_SIZE;
        } else if ((timer_hw->timerawl - start) >= timeout_ms * 1000) {
            break;
        }
    }
    return (read == len) ? HAL_OK : HAL_ERR_TIMEOUT;
}

static hal_err_t hal_uart_flush_impl(uint32_t uart_id) {
    uart_inst_t *uart = (uart_id == 0) ? uart0 : uart1;
    if (uart == NULL) return HAL_ERR_INVAL;
    uart_tx_wait_blocking(uart);
    return HAL_OK;
}

static hal_err_t hal_uart_set_rx_cb_impl(uint32_t uart_id, void (*cb)(uint8_t, void *), void *arg) {
    if (uart_id >= 2) return HAL_ERR_INVAL;
    uart_ctx[uart_id].cb = cb;
    uart_ctx[uart_id].cb_arg = arg;
    return HAL_OK;
}

const hal_uart_ops_t hal_uart_ops = {
    .init       = hal_uart_init_impl,
    .write      = hal_uart_write_impl,
    .read       = hal_uart_read_impl,
    .flush      = hal_uart_flush_impl,
    .set_rx_cb  = hal_uart_set_rx_cb_impl,
};

#endif /* OS_CFG_PERIPH_SERVICE */

/* ================================================================
 * 7. SD 卡块设备（占位，需外挂 SPI SD 卡驱动）
 * ================================================================ */
#if OS_CFG_VFS && OS_CFG_FATFS
static hal_err_t hal_sd_init_impl(void) { return HAL_ERR_NOTSUP; }
static hal_err_t hal_sd_read_sectors_impl(uint8_t *buf, uint32_t sector, uint32_t count) { (void)buf; (void)sector; (void)count; return HAL_ERR_NOTSUP; }
static hal_err_t hal_sd_write_sectors_impl(const uint8_t *buf, uint32_t sector, uint32_t count) { (void)buf; (void)sector; (void)count; return HAL_ERR_NOTSUP; }
static hal_err_t hal_sd_get_capacity_impl(uint32_t *sector_count) { (void)sector_count; return HAL_ERR_NOTSUP; }

const hal_sdcard_ops_t hal_sdcard_ops = {
    .init            = hal_sd_init_impl,
    .read_sectors    = hal_sd_read_sectors_impl,
    .write_sectors   = hal_sd_write_sectors_impl,
    .get_capacity    = hal_sd_get_capacity_impl,
};
#endif

/* ================================================================
 * 7. 板载 Flash（W25Q16JV，2MB QSPI）—— 供 bootscript / ENV 固化
 *
 * 注意：flash_range_erase / flash_range_program 通过 ROM 函数运行，
 *       执行期间会暂停 XIP 并关闭中断，期间不会响应任何调度。
 *       offset = 相对 PICO_FLASH(0x10000000) 的字节偏移，不包含 XIP 基址。
 * ================================================================ */
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES  (2u * 1024u * 1024u)   /* W25Q16 = 2 MiB 默认值 */
#endif
#define XIP_BASE_ADDR           0x10000000u   /* Cortex-M0+ QSPI XIP 窗口基址 */

/* CRC8-SMBUS：poly=x^8+x^5+x^4+1 (0x07), init=0x00, xor_out=0x00, reflect=no */
static uint8_t _crc8_smbus(const uint8_t *data, size_t len) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x80) crc = (uint8_t)((crc << 1) ^ 0x07);
            else            crc = (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static hal_err_t hal_flash_erase_sector_impl(uint32_t offset) {
    if ((offset % HAL_FLASH_SECTOR_SIZE) != 0) return HAL_ERR_PARAM;
    if (offset > (PICO_FLASH_SIZE_BYTES - HAL_FLASH_SECTOR_SIZE)) return HAL_ERR_PARAM;
    
    /* 手动分片擦除：每次擦除 1/4 扇区 (1KB)，每次操作后重新启用中断保护 USB */
    const uint32_t chunk_size = HAL_FLASH_SECTOR_SIZE / 4;  /* 1KB per chunk */
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t chunk_offset = offset + i * chunk_size;
        flash_range_erase(chunk_offset, chunk_size);
        /* 显式重新启用中断，让 USB 有机会处理 SOF */
        __asm volatile ("cpsie i" ::: "memory");
        busy_wait_us(10);  /* 短暂延时让 USB 处理中断 */
    }
    return HAL_OK;
}

static hal_err_t hal_flash_program_impl(uint32_t offset, const uint8_t *data, size_t len) {
    if (len == 0) return HAL_OK;
    if (offset + len > PICO_FLASH_SIZE_BYTES) return HAL_ERR_PARAM;
    
    const uint8_t *xip = (const uint8_t *)XIP_BASE_ADDR;
    uint8_t page_buf[HAL_FLASH_PAGE_SIZE];
    size_t remaining = len;
    const uint8_t *src = data;
    
    while (remaining > 0) {
        uint32_t page_off = offset & (HAL_FLASH_PAGE_SIZE - 1);
        uint32_t page_start = offset - page_off;
        size_t chunk = HAL_FLASH_PAGE_SIZE - page_off;
        if (chunk > remaining) chunk = remaining;

        /* 合并整页：当前 Flash 旧值 AND 新值（因为 program 只有 1→0） */
        memcpy(page_buf, &xip[page_start], HAL_FLASH_PAGE_SIZE);
        for (size_t i = 0; i < chunk; i++) {
            page_buf[page_off + i] &= src[i];
        }
        
        /* 每页单独调用 flash_range_program，写入后显式重新启用中断 */
        flash_range_program(page_start, page_buf, HAL_FLASH_PAGE_SIZE);
        __asm volatile ("cpsie i" ::: "memory");
        busy_wait_us(5);  /* 短暂延时让 USB 处理中断 */
        
        offset += chunk;
        src += chunk;
        remaining -= chunk;
    }
    return HAL_OK;
}

static const uint8_t *hal_flash_map_read_impl(uint32_t offset) {
    if (offset >= PICO_FLASH_SIZE_BYTES) return NULL;
    return (const uint8_t *)(XIP_BASE_ADDR + offset);
}

static hal_err_t hal_flash_crc8_impl(uint32_t offset, size_t len, uint8_t *out_crc) {
    if (len == 0) { *out_crc = 0; return HAL_OK; }
    if (offset + len > PICO_FLASH_SIZE_BYTES) return HAL_ERR_PARAM;
    *out_crc = _crc8_smbus((const uint8_t *)(XIP_BASE_ADDR + offset), len);
    return HAL_OK;
}

const hal_flash_ops_t hal_flash_ops = {
    .erase_sector = hal_flash_erase_sector_impl,
    .program_page = hal_flash_program_impl,
    .map_read     = hal_flash_map_read_impl,
    .crc8_range   = hal_flash_crc8_impl,
};

/* ================================================================
 * 8. 系统时钟档位（超频）— v2.4
 *
 *  RP2040 双核共享 SYS PLL，CPU 主频对两核一视同仁。切换档位时：
 *   1) 高主频先升压（vreg），保证 PLL/核心时序余量；
 *   2) set_sys_clock_khz() 重配 SYS PLL（内部会先把 clk_peri 切到
 *      48MHz USB PLL，保证切换瞬间外设不超速）；
 *   3) 把 clk_peri 钳回 125MHz（源 = SYS PLL 分频）→ 外设总线速度不变，
 *      从而 UART/I2C/SPI/Timer 波特率与节拍完全不受超频影响；
 *   4) 缩放 XIP flash 的 SSI 分频，使 flash 读取速度与旧主频相当，
 *      避免高主频下 XIP 读崩（症状看起来像代码损坏 / HardFault）。
 * ================================================================ */
const sysclk_tier_t g_sysclk_tiers[SYSCLK_TIER_COUNT] = {
    { 125u, "125MHz", false },
    { 250u, "250MHz", false },
    { 375u, "375MHz", false },
    { 500u, "500MHz", true  },   /* 隐藏档：500=125×4（整数），clk_peri 精确 125MHz，UART 波特率不偏 */
};

uint32_t sysclk_current_mhz(void) {
    return (clock_get_hz(clk_sys) + 999999u) / 1000000u;
}

int sysclk_current_tier(void) {
    uint32_t mhz = sysclk_current_mhz();
    for (int i = 0; i < SYSCLK_TIER_COUNT; i++) {
        if (g_sysclk_tiers[i].mhz == mhz) return i;
    }
    return -1;
}

/* 在请求频率附近（±16MHz）寻找可被 SYS PLL 精确锁定的频率。
 * 纯计算（check_sys_clock_khz 不改任何寄存器），返回可达频率（MHz）；
 * 找不到返回 0。RP2040 的 PLL 只能整数分频，任意输入值未必能精确达到，
 * 这里就近取一个可达值，保证"自行输入频率"尽量贴近目标。 */
static uint32_t sysclk_nearest_achievable_mhz(uint32_t want) {
    for (int32_t d = 0; d <= 16; d++) {
        uint vco, p1, p2;
        if (check_sys_clock_khz((want + (uint32_t)d) * 1000u, &vco, &p1, &p2))
            return want + (uint32_t)d;
        if (d > 0 && want > (uint32_t)d &&
            check_sys_clock_khz((want - (uint32_t)d) * 1000u, &vco, &p1, &p2))
            return want - (uint32_t)d;
    }
    return 0u;
}

/* 【v2.7.1】超频切换进行中标志（跨核可见）。
 * sysclk_apply_mhz 切换时钟时，core1 的 systick/show 任务不受 core0 的
 * PRIMASK 关中断保护；通知 core1 的 show 任务在切换期间自我暂停，避免
 * core1 在过渡频率下运行破坏共享 RAM → core0 idle 崩 / USB 失效。 */
volatile uint32_t g_oc_switching = 0;

bool sysclk_apply_mhz(uint32_t mhz) {
    if (mhz < SYSCLK_MHZ_MIN || mhz > SYSCLK_MHZ_MAX) return false;

    /* 就近锁定可达频率（升压/分频都用它，保证 PLL 一定能切过去） */
    uint32_t target = sysclk_nearest_achievable_mhz(mhz);
    if (target == 0u) {
        set_sys_clock_khz(SYSCLK_MHZ_DEFAULT * 1000u, true);
        return false;
    }

    /* 【v2.4.3 · 切换全程关中断】
     * 内核 tick(TIMER_IRQ_0)/USB IRQ 在 clk_sys 重新配置的瞬间若抢占，
     * ISR 会在过渡频率 + 过渡 flash 分频下从 XIP 取指 → 二次崩溃风险。
     * 这里把"升压 + flash 分频 + PLL 切换 + clk_peri"整个临界区关中断，
     * 保证切换原子性（耗时几百 µs，丢一两个 tick 无碍，USB 短暂停顿可恢复）。 */
    uint32_t irq_save = save_and_disable_interrupts();

    /* 【v2.7.1-fix】通知 core1 show 任务在切换期间暂停（核心切换只保护 core0 中断）。
     * 不能在这里 busy_wait 等 show —— save_and_disable_interrupts() 已把 core0
     * 全部中断屏蔽，若 busy_wait 50ms，USB 就 50ms 不被服务 → 主机判定设备超时
     * 发起 reset/重新枚举 → 中断恢复后 USB IRQ 处理一个被破坏的端点状态 → 二次崩。
     * 正确做法：只置标志（show 在帧边界自检暂停），随后立即执行切换。切换窗口仅
     * 几百 µs，flash 分频已提前放大 + clk_peri 已钳制，show 即使恰在 I2C 中也会
     * 由 I2C 超时/重初恢复；show 栈已是 4096，不再溢出破坏共享 RAM。 */
    g_oc_switching = 1;

    /* 高主频先升压，保证 PLL/核心时序余量。
         * 250MHz: 1.20V
         * 375MHz: 1.25V
         * 500MHz: 1.30V (VREG_VOLTAGE_MAX) */
        if (target >= 500u) {
            vreg_set_voltage(VREG_VOLTAGE_MAX);       /* 1.30V - 500MHz 极限档 */
        } else if (target >= 375u) {
            vreg_set_voltage(VREG_VOLTAGE_1_25);      /* 1.25V - 375MHz */
        } else if (target >= 250u) {
            vreg_set_voltage(VREG_VOLTAGE_1_20);      /* 1.20V - 250MHz */
        } else {
            vreg_set_voltage(VREG_VOLTAGE_1_10);      /* 1.10V - 125MHz */
        }

    /* 【v2.4.2 · XIP flash 分频必须在时钟切换的"正确时机"写入】
     * set_sys_clock_khz 内部序列：切 clk_sys 到 pll_usb(48MHz) → 重配
     * pll_sys → **把 clk_sys 直接切到目标频率**。若分频没提前准备好，
     * 切到 375MHz 的瞬间 flash 时钟 = 375 / 旧分频(≈2) ≈ 187MHz 远超
     * flash 上限 → XIP 取指失败 → CPU 挂死（现象：`ovclk try 2` 打出
     * "applying now..." 后串口立刻断，连 apply 结果都来不及打印）。
     *
     * 正确顺序（保证整个切换过程 flash 时钟都不超 SYSCLK_FLASH_MAX_MHZ）：
     *   · 升频：先把分频加大（此刻 clk_sys 还低，flash 只会更慢，安全），
     *     再切 PLL —— 切到目标频率瞬间 flash 时钟 = 目标/新分频 ≤ 上限；
     *   · 降频：先降 clk_sys（旧分频下 flash 时钟随 clk_sys 一起下降，
     *     不会超速），切完再把分频减小。
     * （set_sys_clock_khz 不触碰 pll_usb，USB 时钟独立保持 48MHz。） */
    uint32_t new_baud = (target + SYSCLK_FLASH_MAX_MHZ - 1u) / SYSCLK_FLASH_MAX_MHZ;
    if (new_baud < 1u) new_baud = 1u;
    const bool up = (target > sysclk_current_mhz());
    if (up) ssi_hw->baudr = new_baud;

    /* 切换 SYS PLL 到目标频率（target 已由 check_sys_clock_khz 确认可达）。 */
    if (!set_sys_clock_khz(target * 1000u, true)) {
        set_sys_clock_khz(SYSCLK_MHZ_DEFAULT * 1000u, true);
        ssi_hw->baudr = (SYSCLK_MHZ_DEFAULT + SYSCLK_FLASH_MAX_MHZ - 1u) / SYSCLK_FLASH_MAX_MHZ;
        g_oc_switching = 0;                /* 恢复 core1 show 任务 */
        restore_interrupts(irq_save);
        return false;
    }
    if (!up) ssi_hw->baudr = new_baud;

    /* 把 clk_peri 钳到 SYSCLK_PERI_MAX_MHZ 以内（自动整数分频）。
     *   · 预设档都是 125 的整数倍（125/250/375/500）→ ceil(f/133) 分频后
     *     clk_peri 恰好 = 125MHz（整数），UART/SPI/I2C/Timer 波特率与节拍
     *     完全不变；
     *   · 任意频率按真实 clk_sys 就近取整数分频，clk_peri ≤ 133MHz 保证
     *     外设不超规格；USB 在 pll_usb(48MHz) 上，始终不受影响。 */
    uint32_t actual_mhz = sysclk_current_mhz();
    uint32_t pdiv = (actual_mhz + SYSCLK_PERI_MAX_MHZ - 1u) / SYSCLK_PERI_MAX_MHZ;
    if (pdiv < 1u) pdiv = 1u;
    clock_configure(clk_peri,
                    0u,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    actual_mhz * 1000u * 1000u,
                    (actual_mhz / pdiv) * 1000u * 1000u);

    /* 【v2.4.3 · 按新 clk_peri 重设 UART0 波特率】
     * clk_peri 被整数分频后（任意频率 ≠125 的倍数时 ≠125MHz），UART0 分频
     * 仍按旧 clk_peri 算 → TTL 调试串口波特率会偏。RP2040 UART 分频器带
     * 6 位小数部分，这里按新 clk_peri 重算（uart_set_baudrate 内部读
     * clock_get_hz(clk_peri)），把 TTL 调试口精确恢复到 115200。 */
    uart_set_baudrate(uart0, 115200);

    g_oc_switching = 0;                /* 恢复 core1 show 任务 */
    restore_interrupts(irq_save);
    return true;
}

bool sysclk_apply_tier(int tier) {
    if (tier < 0 || tier >= SYSCLK_TIER_COUNT) return false;
    return sysclk_apply_mhz(g_sysclk_tiers[tier].mhz);
}

/* ================================================================
 * 8. HAL 导出表（内核唯一访问入口）
 * ================================================================ */

/* ================================================================
 * 8.5 板级基础功能（hal_interface.h 第 1.5 节）
 *
 *  这些强符号把内核启动/诊断阶段对板子的依赖全部收到移植层，
 *  内核核心（kernel/core）只调这些接口，不直接碰 Pico SDK / RP2040
 *  寄存器，从而保证内核库纯净、可移植到其它 MCU。
 * ================================================================ */
#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 25
#endif

/* 早期诊断 UART0 直写：与 HardFault dump 同一条通道（115200-8N1）。
 * 直接写寄存器，绝不触发调度/中断（PendSV 安全）。
 * UART0 基址 0x40034000：DR=+0, FR=+0x18, FR bit5=TX FIFO 满。 */
#define DIAG_UART_DR   (*(volatile uint32_t *)0x40034000u)
#define DIAG_UART_FR   (*(volatile uint32_t *)0x40034018u)
#define DIAG_UART_TXFF (1u << 5)

void hal_led_init(void) {
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_set_slew_rate(PICO_DEFAULT_LED_PIN, GPIO_SLEW_RATE_SLOW);
    gpio_set_drive_strength(PICO_DEFAULT_LED_PIN, GPIO_DRIVE_STRENGTH_4MA);
    gpio_put(PICO_DEFAULT_LED_PIN, 0);
}
void hal_led_set(int on)                { gpio_put(PICO_DEFAULT_LED_PIN, on ? 1 : 0); }
void hal_led_on(void)                   { gpio_put(PICO_DEFAULT_LED_PIN, 1); }
void hal_led_off(void)                  { gpio_put(PICO_DEFAULT_LED_PIN, 0); }
void hal_delay_ms(uint32_t ms)          { busy_wait_us_32(ms * 1000u); }

/* 控制台就绪：WITHOUT_DTR=1 时 stdio_usb_connected() = tud_ready()（USB 已枚举） */
extern bool stdio_usb_connected(void);
int  hal_console_ready(void)            { return stdio_usb_connected() ? 1 : 0; }

void hal_irq_enable(void)               { __asm volatile ("cpsie i" ::: "memory"); }
void hal_irq_disable(void)              { __asm volatile ("cpsid i" ::: "memory"); }

/* 当前核心号：RP2040 SIO_CPUID 寄存器（0xD0000000，bit0=core, 1=core1）。 */
uint32_t hal_core_id(void)              { return (*(volatile uint32_t *)0xD0000000u) & 0xFFu; }

/* ================================================================
 * 多核启动（config_mcore_apply 强符号，覆盖 config_store.c 的 weak no-op）
 *
 *   core1 调度器入口：在 core1 上运行（MSP 上下文）。先配置 core1 的
 *   systick（ALARM1 + TIMER_IRQ_1，见 ktick_*），再启动 core1 调度器。
 *   core1 的首任务由 sched_start 从 core1 就绪队列选取（空则为其 idle）。
 *   注意：core1 的 idle / 各核心 idle 上下文已在 task_module_init 预初始化。
 * ================================================================ */
extern void sched_start(void);
extern void multicore_launch_core1(void (*entry)(void));  /* pico_multicore */
static void __attribute__((noreturn)) core1_scheduler_entry(void) {
    hal_systick_init(OS_CFG_TICK_HZ);   /* 在 core1 上配置其 tick */
    sched_start();                       /* core1 首任务，不返回 */
    while (1) { }
}

/* 多核启动请求标志：config_mcore_apply 只记录（固化语义）。 */
static volatile int g_mcore_requested = 0;
void config_mcore_apply(bool enable) {
    g_mcore_requested = enable ? 1 : 0;
}
void hal_mcore_start(void) {
    /* v2.7.1：boot_setup 开机自动启动 core1（配合 show 任务绑定核1）。
     * core1_scheduler_entry 为 core1 配置独立 systick 并跑 sched_start()，
     * 从 core1 就绪队列选任务。仅 core0 调用（内部二次校验）。 */
    if (hal_core_id() == 0) {
        multicore_launch_core1(core1_scheduler_entry);
    }
}
uint32_t hal_mcore_core1_ticks(void) { return g_core1_tick_count; }

void hal_diag_init(void) {
    /* boot 最早期初始化 UART0(115200, GP0=TX/GP1=RX)，让 HardFault dump /
     * [CONFIG] 等诊断能经 UART0 直写输出。 */
    uart_init(uart0, 115200);
    gpio_set_function(0, GPIO_FUNC_UART);
    gpio_set_function(1, GPIO_FUNC_UART);
    ipc_boot_snap("A-poweron");     /* 【v2.6.4】最早快照：上电即楔死 → 硬件 */
}
void hal_diag_putc(char c) {
    while (DIAG_UART_FR & DIAG_UART_TXFF) { }   /* 忙等 TX FIFO 非满 */
    DIAG_UART_DR = (uint32_t)(uint8_t)c;
}
void hal_diag_puts(const char *s) { while (*s) hal_diag_putc(*s++); }
void hal_diag_put_u32(uint32_t v) {
    char buf[8]; int bi = 0;
    if (v == 0) { hal_diag_putc('0'); return; }
    while (v > 0) { buf[bi++] = (char)('0' + (v % 10)); v /= 10; }
    while (bi > 0) hal_diag_putc(buf[--bi]);
}

/* ================================================================
 * 8.6 HardFault 现场转储（UART0 直写寄存器）
 *  由 context_switch.S 的 isr_hardfault 调用（Handler 模式，MSP 上）。
 *  用最底层 UART0 直写输出故障寄存器，不走 putchar/SDK/TinyUSB
 *  （fault 时 USB 状态未知，调用 SDK 易二次 fault → Lockup）。
 * ================================================================ */
volatile uint32_t g_fault_psp = 0;
volatile uint32_t g_fault_msp = 0;
volatile uint32_t g_fault_lr  = 0;   /* 【v2.7.1-fix】EXC_RETURN，区分 fault 来源（Handler/Thread） */

#define FAULT_SRAM_LO    0x20000000u
#define FAULT_SRAM_HI    0x20042000u   /* RP2040 264KB SRAM 上界 */

static void _fault_putc(char c) {
    while (DIAG_UART_FR & DIAG_UART_TXFF) { }   /* 忙等 TX FIFO 非满 */
    DIAG_UART_DR = (uint32_t)(uint8_t)c;
}
static void _fault_puts(const char *s) { while (*s) _fault_putc(*s++); }
static void _fault_puthex32(uint32_t v) {
    const char hex[] = "0123456789ABCDEF";
    _fault_putc('0'); _fault_putc('x');
    for (int i = 28; i >= 0; i -= 4) _fault_putc(hex[(v >> i) & 0xF]);
}

/* 由 isr_hardfault 调用（Handler 模式，跑在 MSP 上，C 函数栈安全） */
void hardfault_dump_c(void) {
    /* 【v2.4.3 · 按当前 clk_peri 重设 UART0 波特率再转储】
     * 超频切换中途崩溃时（clk_peri 已被 SDK 临时切到 48MHz、UART0 分频还是
     * 按旧 clk_peri 算的），直接 115200 输出会波特率错位 → 乱码。
     * 这里先按 clock_get_hz(clk_peri) 重算 UART0 分频（仅寄存器写 + RAM 读，
     * 故障现场安全），保证 dump 在任何频率下都可读。 */
    uart_set_baudrate(uart0, 115200);

    uint32_t hfsr = *(volatile uint32_t *)0xE000ED2Cu;  /* HardFault Status */
    uint32_t cfsr = *(volatile uint32_t *)0xE000ED28u;  /* Configurable Fault Status */
    uint32_t bfar = *(volatile uint32_t *)0xE000ED38u;  /* BusFault Address */

    _fault_puts("\r\n\n!!! HardFault !!!\r\n");
    _fault_puts("MSP=");   _fault_puthex32(g_fault_msp);
    _fault_puts("  PSP="); _fault_puthex32(g_fault_psp);
    _fault_puts("  CFSR="); _fault_puthex32(cfsr);
    _fault_puts("  HFSR="); _fault_puthex32(hfsr);
    _fault_puts("  BFAR="); _fault_puthex32(bfar);
    _fault_puts("  EXC_RET="); _fault_puthex32(g_fault_lr);

    /* 【v2.7.1-fix】用 EXC_RETURN 选真实故障帧：
     *   bit2=0 → Handler 模式内 fault，真实帧在 MSP（PSP 是被打断任务的旧帧，
     *            PC/xPSR 是垃圾 → 之前误判为"idle PC=0"）；
     *   bit2=1 → Thread 模式 fault，真实帧在 PSP。 */
    uint32_t fp = ((g_fault_lr & 0x4u) == 0u) ? g_fault_msp : g_fault_psp;
    /* 硬件栈帧：[0]r0 [1]r1 [2]r2 [3]r3 [4]r12 [5]lr [6]pc [7]xpsr */
    if (fp >= FAULT_SRAM_LO && fp < FAULT_SRAM_HI) {
        volatile uint32_t *f = (volatile uint32_t *)fp;
        _fault_puts("\r\nFault PC=");  _fault_puthex32(f[6]);
        _fault_puts("  LR=");          _fault_puthex32(f[5]);
        _fault_puts("  xPSR=");        _fault_puthex32(f[7]);
        _fault_puts("  R0=");          _fault_puthex32(f[0]);
        _fault_puts("  R12=");         _fault_puthex32(f[4]);
        _fault_puts(((g_fault_lr & 0x4u) == 0u) ? "  [in ISR]" : "  [in task]");
    } else {
        _fault_puts("\r\nframe invalid (fault in Handler mode, PSP only)");
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

/* 软复位系统：写 AIRCR 的 SYSRESETREQ 位触发系统复位。
 * 复位后重新执行完整启动流程（kernel_main → [CONFIG] → boot setup → banner），
 * 从而重现首次开机画面。shell `reboot` 命令调用。 */
void hal_system_reset(void) {
    /* 先刷出 pending 输出，再关中断复位 */
    busy_wait_us_32(20000u);   /* 短暂等待 USB/TTL 输出排空 */

    /* 【Bugfix】软复位前先断开 USB D+ 上拉（USB_SIE_CTRL.PULLUP_EN=0）。
     *   RP2040 软件复位（AIRCR）时若 D+ 上拉仍保持，Windows 不会把设备
     *   当作"断开→重新枚举"，导致复位后 USB CDC 完全无输出（TTL 仍正常）。
     *   先清 PULLUP_EN 让主机识别到干净的断开事件，复位后 stdio_init_all()
     *   重新拉上 D+ → Windows 重新枚举 → PuTTY 恢复输出。 */
    {
        volatile uint32_t *sie_ctrl = (volatile uint32_t *)0x5011004Cu; /* USB_SIE_CTRL */
        *sie_ctrl = *sie_ctrl & ~(1u << 16);   /* PULLUP_EN = 0 → 断开 D+ */
        busy_wait_us_32(60000u);                /* 等主机识别断开（USB spec >2.5ms，取 50ms 稳妥） */
    }

    __asm volatile ("cpsid i" ::: "memory");
    /* AIRCR = 0xE000ED0C: 写 0x05FA0000 | (1<<2) = SYSRESETREQ */
    *(volatile uint32_t *)0xE000ED0Cu = 0x05FA0004u;
    /* 同步请求，循环等待复位生效 */
    for (volatile uint32_t i = 0; i < 1000000u; i++) __asm("nop");
    /* 正常情况下不可达 */
    while (1) { }
}
const hal_export_t hal_export = {
    .systick  = &hal_systick_ops,
    .console  = &hal_console_ops,
#if OS_CFG_PERIPH_SERVICE
    .gpio     = &hal_gpio_ops,
    .spi      = &hal_spi_ops,
    .i2c      = &hal_i2c_ops,
    .uart     = &hal_uart_ops,
    .flash    = &hal_flash_ops,
#endif
#if OS_CFG_VFS && OS_CFG_FATFS
    .sdcard   = &hal_sdcard_ops,
#endif
};