/**
 * @file    rp2350_gpio.c
 * @brief   RP2350 GPIO 驱动实现
 */

#include "rp2350_port.h"
#include "hardware/gpio.h"
#include "hardware/structs/iobank0.h"
#include "hardware/structs/padsbank0.h"

#define RP2350_GPIO_MAX 48

hal_err_t rp2350_gpio_init(uint32_t pin, uint32_t func) {
    if (pin >= RP2350_GPIO_MAX) return HAL_ERR_INVAL;
    
    gpio_init(pin);
    if (func != 0xFF) {
        gpio_set_function(pin, func);
    }
    return HAL_OK;
}

void rp2350_gpio_set_dir(uint32_t pin, bool out) {
    if (pin < RP2350_GPIO_MAX) {
        gpio_set_dir(pin, out ? GPIO_OUT : GPIO_IN);
    }
}

void rp2350_gpio_put(uint32_t pin, bool value) {
    if (pin < RP2350_GPIO_MAX) {
        gpio_put(pin, value);
    }
}

bool rp2350_gpio_get(uint32_t pin) {
    return (pin < RP2350_GPIO_MAX) ? gpio_get(pin) : false;
}

void rp2350_gpio_toggle(uint32_t pin) {
    if (pin < RP2350_GPIO_MAX) {
        gpio_xor_mask(1u << pin);
    }
}