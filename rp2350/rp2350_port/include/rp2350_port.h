/**
 * @file    rp2350_port.h
 * @brief   RP2350 (Pico 2) 移植层统一头文件
 */

#ifndef RP2350_PORT_H
#define RP2350_PORT_H

#include "hal_interface.h"
#include "os_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * RP2350 特定配置
 * ================================================================ */

#define RP2350_CORE_COUNT           2           /* Cortex-M33 双核 */
#define RP2350_MAX_FREQ_HZ          150000000   /* 150MHz 最大主频 */
#define RP2350_RAM_SIZE             (520 * 1024) /* 520KB RAM */
#define RP2350_FLASH_SIZE           (4 * 1024 * 1024) /* 4MB Flash */

/* TrustZone 支持 */
#define RP2350_HAS_TRUSTZONE        1

/* RISC-V Hazard3 核心支持 (可选) */
#define RP2350_HAS_RISCV            1

/* ================================================================
 * 内存映射 (RP2350)
 * ================================================================ */

#define RP2350_XIP_BASE             0x10000000
#define RP2350_RAM_BASE             0x20000000
#define RP2350_RAM_SIZE_BYTES       (520 * 1024)
#define RP2350_PPB_BASE             0xE0000000  /* Private Peripheral Bus */
#define RP2350_SCS_BASE             0xE000E000  /* System Control Space */

/* ================================================================
 * 外设基址 (RP2350 新增/变更)
 * ================================================================ */

/* GPIO - 更多引脚 (最多 48 个) */
#define RP2350_GPIO_COUNT           48
#define IO_BANK0_BASE               0x40014000
#define PADS_BANK0_BASE             0x4001C000

/* 新增 PIO 实例 (2 个 PIO，每个 4 状态机) */
#define PIO0_BASE                   0x50200000
#define PIO1_BASE                   0x50210000
#define PIO2_BASE                   0x50220000  /* RP2350 新增 */

/* 更多 UART/SPI/I2C */
#define UART0_BASE                  0x40034000
#define UART1_BASE                  0x40038000
#define SPI0_BASE                   0x40040000
#define SPI1_BASE                   0x40044000
#define I2C0_BASE                   0x40048000
#define I2C1_BASE                   0x4004C000

/* ADC - 更多通道 */
#define ADC_BASE                    0x4005C000
#define ADC_CHANNELS                6  /* GPIO 26-29 + 内部温度传感器 + 内部参考电压 */

/* PWM - 更多切片 */
#define PWM_BASE                    0x40050000
#define PWM_SLICES                  12  /* 12 个切片，每个 2 通道 */

/* 定时器 */
#define TIMER0_BASE                 0x40054000
#define TIMER1_BASE                 0x40058000  /* 新增 */

/* USB - 增强 */
#define USB_BASE                    0x50110000
#define USB_DPRAM_BASE              0x50100000

/* ================================================================
 * 中断号 (RP2350)
 * ================================================================ */

#define RP2350_IRQ_TIMER0           0
#define RP2350_IRQ_TIMER1           1
#define RP23550_IRQ_USBCTRL         24
#define RP2350_IRQ_PIO0_0           17
#define RP2350_IRQ_PIO0_1           18
#define RP2350_IRQ_PIO1_0           19
#define RP2350_IRQ_PIO1_1           20
#define RP2350_IRQ_PIO2_0           21  /* 新增 */
#define RP2350_IRQ_PIO2_1           22  /* 新增 */

/* ================================================================
 * 核心 ID 获取 (Cortex-M33 支持)
 * ================================================================ */

static inline uint32_t rp2350_core_id(void) {
    /* Cortex-M33 使用 MPIDR_EL1 寄存器 */
    uint32_t mpidr;
    __asm volatile ("mrs %0, mpidr_el1" : "=r"(mpidr));
    return mpidr & 0xF;  /* 低 4 位为核心 ID */
}

/* ================================================================
 * TrustZone 安全/非安全世界切换
 * ================================================================ */

#if RP2350_HAS_TRUSTZONE
/* 安全世界调用非安全函数 */
#define TZ_NS_CALL(addr)  __asm volatile ("bxns %0" : : "r"(addr) : "memory")

/* 非安全世界调用安全函数 (需安全侧导出) */
#define TZ_S_CALL(addr)   __asm volatile ("blxns %0" : : "r"(addr) : "memory")

/* 检查当前是否在安全世界 */
static inline bool rp2350_is_secure(void) {
    uint32_t control;
    __asm volatile ("mrs %0, control_ns" : "=r"(control));
    return (control & 1) == 0;
}
#endif

/* ================================================================
 * 系统时钟配置 (RP2350 支持更高频率)
 * ================================================================ */

typedef enum {
    RP2350_CLK_48MHZ   = 48000000,   /* USB 引导默认 */
    RP2350_CLK_100MHZ  = 100000000,  /* 稳定运行 */
    RP2350_CLK_125MHZ  = 125000000,  /* RP2040 兼容 */
    RP2350_CLK_133MHZ  = 133000000,  /* RP2040 最大 */
    RP2350_CLK_150MHZ  = 150000000,  /* RP2350 最大 */
} rp2350_clk_freq_t;

/* 时钟配置结构 */
typedef struct {
    uint32_t sys_clk_hz;     /* 系统主频 */
    uint32_t peri_clk_hz;    /* 外设时钟 */
    uint32_t usb_clk_hz;     /* USB 时钟 (必须 48MHz) */
    uint32_t adc_clk_hz;     /* ADC 时钟 */
} rp2350_clk_config_t;

/* ================================================================
 * 函数声明
 * ================================================================ */

/* 系统初始化 */
void rp2350_port_init(void);

/* 核心启动 */
void rp2350_core1_start(void (*entry)(void), void *arg);

/* 时钟配置 */
bool rp2350_clk_configure(rp2350_clk_freq_t freq);
void rp2350_clk_get_config(rp2350_clk_config_t *config);

/* GPIO */
void rp2350_gpio_init(uint32_t pin, uint32_t func);
void rp2350_gpio_set_dir(uint32_t pin, bool out);
void rp2350_gpio_put(uint32_t pin, bool value);
bool rp2350_gpio_get(uint32_t pin);

/* 系统滴答 (双核各自独立) */
void rp2350_systick_init(uint32_t core, uint32_t freq_hz);
uint32_t rp2350_systick_get_tick(uint32_t core);

/* USB */
void rp2350_usb_init(void);
void rp2350_usb_poll(void);

/* Flash */
bool rp2350_flash_erase(uint32_t offset, size_t len);
bool rp2350_flash_program(uint32_t offset, const void *data, size_t len);
const void *rp2350_flash_map_read(uint32_t offset);

/* 复位 */
void rp2350_system_reset(void);

/* ================================================================
 * 导出表 (给内核使用)
 * ================================================================ */

extern const hal_export_t hal_export;

#ifdef __cplusplus
}
#endif

#endif /* RP2350_PORT_H */