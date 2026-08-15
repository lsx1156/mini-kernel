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
#include "os_config.h"
#include "mem.h"

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
#include "hardware/clocks.h"
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

/* 内核回调：滴答中断尾部 */
extern void kernel_tick_hook(void);

/* ================================================================
 * 1. 系统滴答定时器（TIMER_IRQ_3 ← 使用 ALARM3，不与 SDK alarm_pool 冲突）
 *
 * ⚠️ 【v2.2.4 修复：电脑烧录后看不到串口（USB 完全不枚举）的根因】
 *   Pico SDK 默认 alarm_pool（驱动 stdio_usb / TinyUSB 的超时、端点握手定时、
 *   alarm_pool 回调等）**独占使用 ALARM0 + TIMER_IRQ_0**。
 *   旧代码调用 `irq_set_exclusive_handler(TIMER_IRQ_0, systick_irq_handler)`
 *   直接覆盖 SDK 已注册好的 ALARM0 handler → SDK alarm_pool 回调从此永远
 *   不触发 → TinyUSB 的 CDC/MSC 状态机（SET_ADDRESS、端点 0 DATA 阶段、
 *   帧间隔 SOF 等）超时机制全失效 → USB 枚举走到一半卡死 → Windows 完全
 *   识别不出 COM 口 / U 盘盘符 → 用户症状："烧录后没看到串口"。
 *
 * ✅ 修复：改用 ALARM3 + TIMER_IRQ_3。Pico SDK 约定：ALARM0 给默认
 *   alarm_pool 用，ALARM1/2/3 留给用户代码自由使用。我们的内核 tick 只
 *   需要一个周期性 alarm，用 ALARM3 零冲突。
 * ================================================================ */
volatile uint32_t g_tick_count = 0;
volatile uint32_t g_tick_interval_us = 1000u; /* 默认 1ms @ 1000Hz */

/* 寄存器位掩码常量（ALARM3 = bit3）——集中定义一处避免写错 */
#define KTICK_ALARM_IDX       3u                         /* alarm[3] */
#define KTICK_TIMER_IRQ       TIMER_IRQ_3                /* 对应中断号 = 3 */
#define KTICK_TIMER_BIT       (1u << KTICK_ALARM_IDX)    /* intr / inte / armed 寄存器 bit3 */

/* TIMER_IRQ_3 中断处理：清中断 + 累计 tick + 重设 alarm3 + 调内核钩子。
 *
 *   RP2040 Timer 每个 ALARM 都是一次性触发：触发后 armed 位自动清 0，
 *   必须在中断里重写 alarm[N] 才能产生下一个周期。
 *   intr 寄存器是 write-1-to-clear，写 KTICK_TIMER_BIT 清 ALARM3 flag。
 *   间隔基于 timerawl（当前计数器低 32 位）+ interval_us，避免
 *   漂移累积。 */
void systick_irq_handler(void) {
    /* 【致命 Bug 修复】INTR 是 write-1-to-clear 寄存器。
     * hw_clear_bits 做的是 *addr &= ~mask，对 W1C 寄存器等于写 0 → 不清中断！
     * 导致 tick 中断无限重入 → CPU 卡死在 TIMER_IRQ_3 → 调度器状态损坏 → HardFault。
     * 正确做法：直接写 KTICK_TIMER_BIT 到 INTR（写 1 清除对应位）。 */
    timer_hw->intr = KTICK_TIMER_BIT;                     /* 清 ALARM3 中断 (W1C) */
    timer_hw->alarm[KTICK_ALARM_IDX] = timer_hw->timerawl + g_tick_interval_us;
    g_tick_count++;
    kernel_tick_hook();
}

static void hal_systick_init_impl(uint32_t tick_hz) {
    if (tick_hz == 0u) tick_hz = 1000u;
    g_tick_interval_us = 1000000u / tick_hz;

    /* 关闭 alarm3 中断使能 + 清 armed 标志，配置过程中不触发 IRQ。
     * ARMED 也是 write-1-to-clear，必须直接写 KTICK_TIMER_BIT（不能用 hw_clear_bits）。 */
    hw_clear_bits(&timer_hw->inte, KTICK_TIMER_BIT);
    timer_hw->armed = KTICK_TIMER_BIT;

    /* 设置首次 alarm */
    timer_hw->alarm[KTICK_ALARM_IDX] = timer_hw->timerawl + g_tick_interval_us;

    /* 注册 IRQ 处理函数 + 设优先级最低（不抢占 TinyUSB / SDK 关键中断） */
    irq_set_exclusive_handler(KTICK_TIMER_IRQ, systick_irq_handler);
    irq_set_priority(KTICK_TIMER_IRQ, 0xFFu);

    /* PendSV 优先级最低（0xE000ED22 = SHPR3[2] = EXC#14 PendSV），
     * 避免 PendSV 抢占 TIMER_IRQ_3 tick 中断导致调度器重入、队列损坏。
     * Cortex-M0+ SHPR3 字节布局：[20]=#12 [21]=#13 [22]=#14 PendSV [23]=#15 SysTick */
    *(volatile uint8_t *)0xE000ED22u = 0xFFu;

    /* 开启 alarm3 INTE + NVIC */
    hw_set_bits(&timer_hw->inte, KTICK_TIMER_BIT);
    irq_set_enabled(KTICK_TIMER_IRQ, true);
}

static uint32_t hal_systick_get_tick_impl(void) {
    return g_tick_count;
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
static int hal_console_putc_impl(char c) {
    static uint8_t s_yield_counter = 0;

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
 * 安全掩码：只用 TinyUSB 有 ack 代码的位，避免 SOF 占满 CPU：
 *   STALL_STATUS(bit0) | BUFF_STATUS(bit2) | ERROR(bit3) |
 *   EP0_SETUP_REQ(bit6) | BUS_RESET(bit16) | RESUME(bit19)
 *   = 0x0009_004D */
#define USB_INTE_SAFE_MASK 0x0009004Du
#define USB_INTE_REG       0x50110014u   /* USB_INTE (Interrupt Enable) */
#define USB_INTS_REG       0x50110010u   /* USB_INTS = raw_flags & INTE (只读) */
#define USB_EP0_SETUP_BIT  (1u << 6)     /* INTS bit6 = EP0_SETUP_REQ pending */
#define USB_EP1_BUFF_BIT   (1u << 2)     /* INTS bit2 = BUFF_STATUS pending */

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

    /* 2. 在 dcd_int_handler 之前抓 SETUP 快照（handler 会清 INTS 位） */
    _snapshot_setup_if_pending();

    /* 3. 调 dcd_int_handler 处理所有挂起的中断（含 SETUP/Control Request） */
    dcd_int_handler(0);
    s_dcd_handler_called++;
    tud_task_ext(0, 0);

    /* 4. 手动搬 EP1 OUT → ring */
    _ep1_out_drain_all();

    if (inte_after)  *inte_after  = *(volatile uint32_t *)USB_INTE_REG;
    if (iser_after)  *iser_after  = (*(volatile uint32_t *)0xE000E100u >> 21) & 1u;
    if (safe_mask_written) *safe_mask_written = mask_written;
    if (dcd_enable_calls)   *dcd_enable_calls   = s_poll_dcd_enable_count;
    return 0;
}

static inline void _usb_force_poll(void) {
    /* 仅在 INTE==0 时恢复掩码，避免与 TinyUSB 自身的 INTE 管理竞争。
     * 详见 hal_usb_force_poll_and_snapshot 的注释。 */
    if (*(volatile uint32_t *)USB_INTE_REG == 0u) {
        dcd_int_enable(0);
        s_poll_dcd_enable_count++;
        if (*(volatile uint32_t *)USB_INTE_REG == 0u) {
            *(volatile uint32_t *)USB_INTE_REG = USB_INTE_SAFE_MASK;
            s_poll_mask_write_count++;
        }
        if (((*(volatile uint32_t *)0xE000E100u >> 21) & 1u) == 0u) {
            *(volatile uint32_t *)0xE000E100u = (1u << 21);
        }
    }
    _snapshot_setup_if_pending();
    dcd_int_handler(0);
    s_dcd_handler_called++;
    tud_task_ext(0, 0);
    _ep1_out_drain_all();
}

static int hal_console_getc_impl(char *c) {
    _usb_force_poll();
    /* 直接从私有 ring buffer 取字节，不碰 getchar_timeout_us / tud_cdc_read ——
     * 这两个 API 都依赖 TinyUSB 内部接收软件层，已经被 INTE=0 关死。
     *
     * 【并发保护】tail 读位置和 ring 内容必须在同一临界区读取，
     *   防止 _ep1_out_drain_one 并发写入导致读到不一致状态。 */
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
    /* 边界：offset + 4096 > FLASH_SIZE 才越界；最后一个扇区 (offset = FLASH - 4096) 是合法的
     *   （之前用 >= FLASH-4096 会把最后一个扇区误判，导致 bootscript 双备份 SEC_B 直接失败） */
    if (offset > (PICO_FLASH_SIZE_BYTES - HAL_FLASH_SECTOR_SIZE)) return HAL_ERR_PARAM;
    uint32_t primask = save_and_disable_interrupts();
    flash_range_erase(offset, HAL_FLASH_SECTOR_SIZE); /* RAM-code safe per SDK */
    restore_interrupts(primask);
    return HAL_OK;
}

static hal_err_t hal_flash_program_impl(uint32_t offset, const uint8_t *data, size_t len) {
    if (len == 0) return HAL_OK;
    if (offset + len > PICO_FLASH_SIZE_BYTES) return HAL_ERR_PARAM;
    /* SDK flash_range_program 要求 len 是 256 的整数倍，offset 256 对齐；
     *   因此把用户非对齐/非整数倍请求拆成 3 段：头零头 + 中整数页 + 尾零头，
     *   每段都先把要写的 256 字节页与当前 XIP 内容合并（写前不擦，只允许 1→0）。
     *   注意：调用方本应保证先擦再写，这里合并是为了容错，避免破坏同页未写区域。 */
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
        uint32_t primask = save_and_disable_interrupts();
        flash_range_program(page_start, page_buf, HAL_FLASH_PAGE_SIZE);
        restore_interrupts(primask);
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
 * 8. HAL 导出表（内核唯一访问入口）
 * ================================================================ */
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