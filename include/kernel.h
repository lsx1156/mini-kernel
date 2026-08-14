/**
 * @file    kernel.h
 * @brief   内核对外统一头文件（用户态应用仅包含此文件）
 * @note    通过系统调用层访问内核服务，不直接调用内核内部函数
 */
#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include "os_config.h"

/* ================================================================
 * 系统调用号（与 syscall 层对应）
 * ================================================================ */
typedef enum {
    SYS_TASK_CREATE   = 0,
    SYS_TASK_DESTROY  = 1,
    SYS_TASK_YIELD    = 2,
    SYS_TASK_SLEEP    = 3,
    SYS_TASK_SUSPEND  = 4,
    SYS_TASK_RESUME   = 5,
    SYS_KMALLOC       = 10,
    SYS_KFREE         = 11,
    SYS_CONSOLE_PUTC  = 20,
    SYS_CONSOLE_GETC  = 21,
#if OS_CFG_PERIPH_SERVICE
    SYS_GPIO_INIT     = 30,
    SYS_GPIO_WRITE    = 31,
    SYS_GPIO_READ     = 32,
    SYS_GPIO_TOGGLE   = 33,
    SYS_SPI_XFER      = 40,
    SYS_I2C_TX        = 50,
    SYS_I2C_RX        = 51,
    SYS_UART_WRITE    = 60,
    SYS_UART_READ     = 61,
#endif
} syscall_id_t;

/* ================================================================
 * 用户态错误码（与内核 hal_err_t 保持一致）
 * ================================================================ */
typedef enum {
    K_OK            =  0,
    K_ERR           = -1,
    K_ERR_INVAL     = -2,
    K_ERR_NOMEM     = -3,
    K_ERR_BUSY      = -4,
    K_ERR_TIMEOUT   = -5,
    K_ERR_NOTSUP    = -6,
    K_ERR_IO        = -7,
} k_err_t;

/* ================================================================
 * 任务句柄（不透明指针）
 * ================================================================ */
typedef void *k_task_t;

/* ================================================================
 * 系统调用接口（用户态调用入口）
 * 实际实现通过 SVC 指令陷入内核，此处仅为声明
 * ================================================================ */
#ifdef __cplusplus
extern "C" {
#endif

/* 任务管理 */
k_task_t k_task_create(const char *name, void (*entry)(void *), void *arg,
                       size_t stack_size, uint8_t priority);
k_err_t  k_task_destroy(k_task_t task);
void     k_task_yield(void);
void     k_task_sleep(uint32_t ticks);
k_err_t  k_task_suspend(k_task_t task);
k_err_t  k_task_resume(k_task_t task);

/* 内存管理 */
void    *k_malloc(size_t size);
void     k_free(void *ptr);

/* 控制台 */
int      k_putc(char c);
int      k_getc(char *c);
int      k_puts(const char *s);
int      k_printf(const char *fmt, ...);

#if OS_CFG_PERIPH_SERVICE
/* GPIO */
k_err_t  k_gpio_init(uint32_t pin, uint8_t mode, uint32_t af);
void     k_gpio_write(uint32_t pin, uint8_t level);
uint8_t  k_gpio_read(uint32_t pin);
void     k_gpio_toggle(uint32_t pin);

/* SPI */
k_err_t  k_spi_init(uint32_t bus_id, uint32_t hz, uint8_t mode, uint8_t bits);
k_err_t  k_spi_xfer(uint32_t bus_id, const uint8_t *tx, uint8_t *rx, size_t len);
k_err_t  k_spi_cs_ctrl(uint32_t bus_id, uint8_t cs_pin, uint8_t active);

/* I2C */
k_err_t  k_i2c_init(uint32_t bus_id, uint32_t hz);
k_err_t  k_i2c_write(uint32_t bus_id, uint8_t addr, const uint8_t *buf, size_t len);
k_err_t  k_i2c_read(uint32_t bus_id, uint8_t addr, uint8_t *buf, size_t len);
k_err_t  k_i2c_mem_read(uint32_t bus_id, uint8_t addr, uint16_t mem_addr, uint8_t *buf, size_t len);
k_err_t  k_i2c_mem_write(uint32_t bus_id, uint8_t addr, uint16_t mem_addr, const uint8_t *buf, size_t len);

/* UART */
k_err_t  k_uart_init(uint32_t uart_id, uint32_t baud, uint8_t parity, uint8_t stop_bits);
k_err_t  k_uart_write(uint32_t uart_id, const uint8_t *buf, size_t len, uint32_t timeout_ms);
k_err_t  k_uart_read(uint32_t uart_id, uint8_t *buf, size_t len, uint32_t timeout_ms);
k_err_t  k_uart_flush(uint32_t uart_id);
k_err_t  k_uart_set_rx_cb(uint32_t uart_id, void (*cb)(uint8_t, void *), void *arg);
#endif

/* 系统信息 */
uint32_t k_get_tick(void);
const char *k_version(void);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_H */