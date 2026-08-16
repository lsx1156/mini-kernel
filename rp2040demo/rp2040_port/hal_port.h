/**
 * @file    hal_port.h
 * @brief   RP2040 HAL 移植层内部定义（不暴露给内核核心）
 */
#ifndef HAL_PORT_H
#define HAL_PORT_H

#include "hal_interface.h"
#include <pico/stdlib.h>
#include <hardware/uart.h>
#include <hardware/timer.h>
#include <hardware/irq.h>
#include <hardware/gpio.h>
#include <hardware/spi.h>
#include <hardware/i2c.h>

/* RP2040 特定配置 */
#define RP2040_SYSTICK_IRQ_NUM        TIMER_IRQ_0
#define RP2040_CONSOLE_UART           uart0
#define RP2040_CONSOLE_BAUDRATE       115200
#define RP2040_CONSOLE_TX_PIN         0
#define RP2040_CONSOLE_RX_PIN         1

/* PendSV 异常号 (Cortex-M0+) */
#define PENDSV_IRQ_NUM                (-2)  /* PendSV 在 NVIC 中为负数 */

/* 栈对齐 */
#define STACK_ALIGN_SIZE              8

/* TCB 中栈指针偏移（hal_context_init 使用） */
#define CTX_OFFSET_SP                 0

#endif /* HAL_PORT_H */