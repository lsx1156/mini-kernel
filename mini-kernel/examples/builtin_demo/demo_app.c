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

/* USB 诊断用 HAL 层 API（绕过 TinyUSB 接收软件层）*/
extern uint32_t hal_usb_force_poll_and_snapshot(uint32_t *inte_after, uint32_t *iser_after,
                                                uint32_t *safe_mask_written,
                                                uint32_t *dcd_enable_calls);
extern uint32_t hal_usb_diag_ep1_ring_count(void);
extern uint32_t hal_usb_diag_ep1_hw_avail(void);
extern uint32_t hal_usb_diag_setup_count(void);
extern uint32_t hal_usb_diag_setup_w0(void);
extern uint32_t hal_usb_diag_setup_w1(void);
extern uint32_t hal_usb_diag_mask_write_count(void);
extern uint32_t hal_usb_diag_dcd_handler_count(void);
extern bool tud_cdc_n_available(uint8_t itf);

/* ================================================================
 * 演示任务 2：心跳日志（每 1s 打印一次系统状态）
 * 前 5 次心跳附带完整 USB 诊断：force_poll → 快照 → 打印
 * ================================================================ */
static void task_heartbeat(void *arg) {
    (void)arg;
    uint32_t cnt = 0;
    demo_puts("[HEART ] Task started, tick=1Hz status log\r\n");

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

        /* 前 10 beat 做完整 USB 诊断（前 5 次含强制修复，后 5 次验证链路）。
         *   【INTE/NVIC 状态】
         *   I0 = poll 前 INTE（TinyUSB 正常时≠0；被 dcd_int_disable 抢占未配对
         *        恢复时=0，此时触发条件式恢复）
         *   I  = poll 后 INTE（正常时由 TinyUSB 维护，异常时=0x0009004D 安全掩码）
         *   V  = poll 后 ISER bit21（NVIC USBCTRL_IRQ 使能位，期望=1）
         *   MW = 累计手动写 INTE 安全掩码次数（仅在 INTE==0 且 dcd_int_enable
         *        失败时 +1，正常时应缓慢增长或停滞）
         *   DH = 累计 dcd_int_handler 调用次数（每次 poll 都 +1，是路径
         *        畅通的最直接证明）
         *
         *   【EP0 SETUP / Control Request（最关键！必须>0 Windows才会发数据）】
         *   S  = 累计收到的不同 SETUP 包数量
         *        - S=0: Windows 还没发任何 Control Request（没打开串口 / 没断开重连）
         *        - S=1: 只收到 1 个（通常是 SET_LINE_CODING 0x20，还差 SET_CONTROL_LINE_STATE）
         *        - S≥2: 收到 2 个以上（CDC 初始化完成，Windows 应该开始发用户数据了）
         *   W0 = 最近 SETUP 包字节 0-3（bRequest 在 byte1：0x20=SET_LINE_CODING，0x22=SET_CONTROL_LINE_STATE）
         *   W1 = 最近 SETUP 包字节 4-7
         *
         *   【EP1 OUT 数据接收】
         *   H  = BUFSTAT.EP1_OUT_AVAIL（主机发了包=1）
         *   R  = 私有 ring buffer 可读字节数（>0 表示 HAL 读到了）
         *   A  = TinyUSB tud_cdc_n_available（仅参考） */
        if (cnt >= 1 && cnt <= 10) {
            uint32_t inte_before = *(volatile uint32_t *)0x50110014u; /* poll 前 */
            uint32_t inte_after, iser_after, m_written, c_calls;
            hal_usb_force_poll_and_snapshot(&inte_after, &iser_after,
                                            &m_written, &c_calls);
            uint32_t hw_avail = hal_usb_diag_ep1_hw_avail();
            uint32_t ring_cnt = hal_usb_diag_ep1_ring_count();
            uint32_t sw_avail = (uint32_t)tud_cdc_n_available(0);
            uint32_t sc = hal_usb_diag_setup_count();
            uint32_t s0 = hal_usb_diag_setup_w0();
            uint32_t s1 = hal_usb_diag_setup_w1();
            uint32_t mw = hal_usb_diag_mask_write_count();
            uint32_t dh = hal_usb_diag_dcd_handler_count();
            (void)m_written;  /* 单次返回值未使用，改用累计计数器 mw */
            (void)c_calls;    /* 同上 */

            demo_puts("[U"); demo_put_uint32(cnt);
            demo_puts("]I0="); demo_put_hex32(inte_before);
            demo_puts(" I=");  demo_put_hex32(inte_after);
            demo_puts(" V=");  demo_put_uint32(iser_after);
            demo_puts(" MW="); demo_put_uint32(mw);
            demo_puts(" DH="); demo_put_uint32(dh);
            demo_puts(" S=");  demo_put_uint32(sc);
            demo_puts(" W0="); demo_put_hex32(s0);
            demo_puts(" W1="); demo_put_hex32(s1);
            demo_puts(" H=");  demo_put_uint32(hw_avail);
            demo_puts(" R=");  demo_put_uint32(ring_cnt);
            demo_puts(" A=");  demo_put_uint32(sw_avail);
            demo_puts("\r\n");
            if (cnt == 1) {
                demo_puts("[U1提示] 操作步骤：1)关闭当前串口 2)重新打开(或断开重连USB线) 3)在输入框输入 help 回车。观察：S应从0变≥2（收到Control Request），然后H=1且R>0（收到help字符）。W0 byte1=0x20(SET_LINE_CODING)/0x22(SET_CONTROL_LINE_STATE)。MW正常时应停滞或缓慢增长（仅在INTE=0异常时+1）。\r\n");
            }
        } else {
            /* 非诊断 beat 也保持 poll（含条件式 INTE 恢复 + EP1 OUT 搬） */
            extern void hal_usb_poll(void);
            hal_usb_poll();
        }

        task_sleep(1000);
    }
}

/* ================================================================
 * 演示任务 3：内存压力测试（每 2s 一轮 alloc/free）
 * ================================================================ */
static void task_mem_stress(void *arg) {
    (void)arg;
    demo_puts("[MEM   ] Task started, stress test every 2s\r\n");
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

        task_sleep(2000);
    }
}

/* ================================================================
 * 演示任务 4：任务控制 — 定时挂起/恢复 LED 任务
 * ================================================================ */
static void task_ctrl(void *arg) {
    (void)arg;
    demo_puts("[CTRL  ] Task started, suspend/resume LED every 5s\r\n");
    int suspended = 0;

    while (1) {
        /* 先等 5 秒让它跑一会儿 */
        task_sleep(5000);

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
 * ================================================================ */
void demo_app_init(void) {
    demo_puts("\r\n");
    demo_puts("========================================\r\n");
    demo_puts("  Mini Kernel Demo App Starting...\r\n");
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
    demo_puts("\r\n");
    demo_puts("========================================\r\n\r\n");

    /* 创建各演示任务 —— 栈大小、权重不同，体现权重调度效果
     * 检查返回值，防止内存不足时继续执行导致崩溃 */
    g_demo_led_task       = task_create("led",       task_led,       NULL, 384, 1);
    if (!g_demo_led_task) {
        demo_puts("[BOOT  ] WARN: LED task creation FAILED (heap may be low)\r\n");
    }
    
    g_demo_heartbeat_task = task_create("heartbeat", task_heartbeat, NULL, 768, 2);  /* 权重高一点 */
    if (!g_demo_heartbeat_task) {
        demo_puts("[BOOT  ] WARN: Heartbeat task creation FAILED\r\n");
    }
    
    tcb_t *mem_task = task_create("mem",    task_mem_stress, NULL, 768, 1);
    if (!mem_task) {
        demo_puts("[BOOT  ] WARN: Mem task creation FAILED\r\n");
    }
    
    tcb_t *ctrl_task = task_create("ctrl",   task_ctrl,       NULL, 384, 1);
    if (!ctrl_task) {
        demo_puts("[BOOT  ] WARN: Ctrl task creation FAILED\r\n");
    }

    demo_puts("[BOOT  ] 4 demo tasks created. Starting scheduler...\r\n");
    demo_dump_task_list();

    /* 创建交互式 Shell 任务（OS_CFG_SHELL=1 → USB CDC + UART0 双通道 SSH 风格交互）
       会自动再向 task pool 申请 1 个 TCB（=共 5 个用户任务 + 1 idle） */
    shell_start();
    demo_puts("[BOOT  ] Interactive shell task started.\r\n");
    demo_puts("[BOOT  ] Connect USB CDC COMx / UART0 115200 and type 'help'.\r\n\r\n");
}

#else /* !OS_CFG_DEMO_APP —— 关闭 demo 时的空桩，保证链接无未定义符号 */

void demo_app_init(void) {
    /* 演示应用已关闭：用户在此注册自己的任务即可 */
}

#endif /* OS_CFG_DEMO_APP */

/* ================================================================
 * 版本字符串（k_version 必须始终提供，kernel.h 已声明）
 * ================================================================ */
#ifndef KERNEL_VERSION_STR
#define KERNEL_VERSION_STR  "0.1.0"
#endif

const char *k_version(void) {
    return KERNEL_VERSION_STR;
}
