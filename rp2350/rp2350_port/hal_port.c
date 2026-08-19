/**
 * @file    hal_port.c
 * @brief   RP2350 (Cortex-M33) HAL 移植层实现
 */

#include "rp2350_port.h"
#include "hal_interface.h"
#include "os_config.h"
#include <string.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/timer.h"
#include "hardware/irq.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/vreg.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/systick.h"
#include "hardware/structs/nvic.h"
#include "hardware/structs/iobank0.h"
#include "hardware/structs/padsbank0.h"
#include "hardware/structs/clocks.h"
#include "hardware/structs/resets.h"
#include "hardware/structs/ppb.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/systick.h"
#include "pico/multicore.h"

/* ================================================================
 * 系统滴答定时器 (每核独立)
 * ================================================================ */

static volatile uint32_t g_tick_count[RP2350_CORE_COUNT] = {0, 0};
static volatile uint32_t g_tick_interval_ms = 1;  /* 默认 1ms @ 1000Hz */

void systick_irq_handler(void) {
    uint32_t core = rp2350_core_id();
    
    /* 清除中断标志 */
    systick_hw->csr |= SYST_CSR_COUNTFLAG_BITS;
    
    g_tick_count[core]++;
    kernel_tick_hook();  /* 调用内核 tick 钩子 */
}

static void hal_systick_init_impl(uint32_t tick_hz) {
    if (tick_hz == 0) tick_hz = 1000;
    g_tick_interval_ms = 1000 / tick_hz;
    
    uint32_t core = rp2350_core_id();
    uint32_t reload = clock_get_hz(clk_sys) / tick_hz - 1;
    
    /* 配置 SysTick */
    systick_hw->csr = 0;
    systick_hw->rvr = reload;
    systick_hw->cvr = 0;
    systick_hw->csr = SYST_CSR_CLKSOURCE_BITS | SYST_CSR_TICKINT_BITS | SYST_CSR_ENABLE_BITS;
    
    /* 设置中断优先级最低 */
    irq_set_priority(SYST_IRQ, 0xFF);
    irq_set_enabled(SYST_IRQ, true);
}

static uint32_t hal_systick_get_tick_impl(void) {
    return g_tick_count[rp2350_core_id()];
}

static void hal_systick_delay_us_impl(uint32_t us) {
    busy_wait_us(us);
}

const hal_systick_ops_t hal_systick_ops = {
    .init     = hal_systick_init_impl,
    .get_tick = hal_systick_get_tick_impl,
    .delay_us = hal_systick_delay_us_impl,
};

/* ================================================================
 * 控制台 (UART0 + USB CDC)
 * ================================================================ */

static void hal_console_init_impl(uint32_t baudrate) {
    stdio_init_all();
}

static int hal_console_putc_impl(char c) {
    return putchar_raw(c) == c ? 1 : 0;
}

static int hal_console_getc_impl(char *c) {
    int ch = getchar_timeout_us(0);
    if (ch >= 0) {
        *c = (char)ch;
        return 1;
    }
    return 0;
}

const hal_console_ops_t hal_console_ops = {
    .init = hal_console_init_impl,
    .putc = hal_console_putc_impl,
    .getc = hal_console_getc_impl,
};

/* ================================================================
 * GPIO
 * ================================================================ */

hal_err_t hal_gpio_init(uint32_t pin, hal_gpio_mode_t mode, uint32_t af) {
    if (pin >= 48) return HAL_ERR_INVAL;
    
    gpio_init(pin);
    
    switch (mode) {
        case HAL_GPIO_IN:
            gpio_set_dir(pin, GPIO_IN);
            break;
        case HAL_GPIO_OUT_PP:
            gpio_set_dir(pin, GPIO_OUT);
            break;
        case HAL_GPIO_OUT_OD:
            gpio_set_dir(pin, GPIO_OUT);
            gpio_set_pulls(pin, true, false);  /* 上拉模拟开漏 */
            break;
        case HAL_GPIO_AF:
            gpio_set_function(pin, af);
            break;
        default:
            return HAL_ERR_INVAL;
    }
    return HAL_OK;
}

void hal_gpio_write(uint32_t pin, hal_gpio_level_t level) {
    if (pin < 48) gpio_put(pin, level);
}

hal_gpio_level_t hal_gpio_read(uint32_t pin) {
    return pin < 48 ? gpio_get(pin) : 0;
}

void hal_gpio_toggle(uint32_t pin) {
    if (pin < 48) gpio_xor_mask(1u << pin);
}

const hal_gpio_ops_t hal_gpio_ops = {
    .init  = hal_gpio_init,
    .write = hal_gpio_write,
    .read  = hal_gpio_read,
    .toggle = hal_gpio_toggle,
};

/* ================================================================
 * Flash (RP2350 有 4MB Flash)
 * ================================================================ */

#define RP2350_FLASH_SECTOR_SIZE  4096

hal_err_t hal_flash_erase_sector(uint32_t offset) {
    if (offset % RP2350_FLASH_SECTOR_SIZE != 0) return HAL_ERR_INVAL;
    flash_range_erase(offset, RP2350_FLASH_SECTOR_SIZE);
    return HAL_OK;
}

hal_err_t hal_flash_program(uint32_t offset, const void *data, size_t len) {
    flash_range_program(offset, data, len);
    return HAL_OK;
}

const uint8_t *hal_flash_map_read(uint32_t offset) {
    return (const uint8_t *)(XIP_BASE + offset);
}

hal_err_t hal_flash_crc8(uint32_t offset, size_t len, uint8_t *out_crc) {
    const uint8_t *data = (const uint8_t *)(XIP_BASE + offset);
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1);
        }
    }
    *out_crc = crc;
    return HAL_OK;
}

const hal_flash_ops_t hal_flash_ops = {
    .erase_sector = hal_flash_erase_sector,
    .program_page = hal_flash_program,
    .map_read = hal_flash_map_read,
    .crc8_range = hal_flash_crc8,
};

/* ================================================================
 * 板载 LED (Pico 2 默认 GPIO 25)
 * ================================================================ */

static const uint8_t LED_PIN = 25;

void hal_led_init(void) {
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);
}

void hal_led_set(int on) {
    gpio_put(LED_PIN, on);
}

void hal_led_on(void)  { gpio_put(LED_PIN, 1); }
void hal_led_off(void) { gpio_put(LED_PIN, 0); }

/* ================================================================
 * 延时
 * ================================================================ */

void hal_delay_ms(uint32_t ms) {
    sleep_ms(ms);
}

/* ================================================================
 * 控制台就绪检查
 * ================================================================ */

int hal_console_ready(void) {
    return tud_cdc_connected();
}

/* ================================================================
 * 中断控制
 * ================================================================ */

void hal_irq_enable(void)  { __enable_irq(); }
void hal_irq_disable(void) { __disable_irq(); }

uint32_t hal_core_id(void) {
    return rp2350_core_id();
}

/* ================================================================
 * 多核启动
 * ================================================================ */

void hal_mcore_start(void) {
    multicore_launch_core1(kernel_main);
}

/* ================================================================
 * 系统复位
 * ================================================================ */

void hal_system_reset(void) {
    watchdog_reboot(0, 0, 10);
    while (1) __wfi();
}

/* ================================================================
 * 导出表
 * ================================================================ */

const hal_export_t hal_export = {
    .systick = &hal_systick_ops,
    .console = &hal_console_ops,
    .gpio    = &hal_gpio_ops,
    .flash   = &hal_flash_ops,
};

/* ================================================================
 * 早期诊断输出 (UART0 直写)
 * ================================================================ */

void hal_diag_init(void) {
    /* UART0 初始化 115200 */
    uart_init(uart0, 115200);
    gpio_set_function(0, GPIO_FUNC_UART);  /* TX */
    gpio_set_function(1, GPIO_FUNC_UART);  /* RX */
}

void hal_diag_putc(char c) {
    uart_putc_raw(uart0, c);
}

void hal_diag_puts(const char *s) {
    while (*s) hal_diag_putc(*s++);
}

void hal_diag_put_u32(uint32_t v) {
    char buf[11];
    int n = 0;
    if (v == 0) { hal_diag_putc('0'); return; }
    while (v > 0) { buf[n++] = '0' + (v % 10); v /= 10; }
    while (n--) hal_diag_putc(buf[n]);
}

/* ================================================================
 * 启动入口
 * ================================================================ */

extern void kernel_main(void);

void core1_main(void) {
    kernel_main();  /* Core 1 也运行内核主循环 */
}

void rp2350_core1_start(void (*entry)(void), void *arg) {
    (void)entry; (void)arg;
    multicore_launch_core1(core1_main);
}

/* ================================================================
 * 导出符号
 * ================================================================ */

void rp2350_port_init(void) {
    /* 初始化标准库 */
    stdio_init_all();
}