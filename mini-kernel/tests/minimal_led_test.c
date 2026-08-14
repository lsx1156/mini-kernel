/* ================================================================
 * minimal_led_test.c — 100% Pico SDK，零依赖 Mini Kernel
 *
 * 用途：硬件/烧录/板型诊断分流。
 *   - 若本程序烧录后 LED 仍不亮：
 *       → 硬件问题 / 板子 LED 引脚不是 GPIO25 (如 Pico W)
 *       → 或 UF2 没写进 Flash (拖放操作未真正执行)
 *   - 若本程序闪灯正常：
 *       → 硬件通路 100% 好，回到 mini_kernel 的代码/初始化逻辑排错
 *
 * 现象：GPIO25 以 2Hz 持续闪烁 (ON 250ms / OFF 250ms)
 * ================================================================ */
#include "pico/stdlib.h"

#ifndef PICO_DEFAULT_LED_PIN
#  warning "PICO_DEFAULT_LED_PIN not defined — fallback to 25"
#  define PICO_DEFAULT_LED_PIN 25
#endif

int main(void) {
    const uint LED = (uint)PICO_DEFAULT_LED_PIN;

    gpio_init(LED);
    gpio_set_dir(LED, GPIO_OUT);
    gpio_set_slew_rate(LED, GPIO_SLEW_RATE_SLOW);
    gpio_set_drive_strength(LED, GPIO_DRIVE_STRENGTH_4MA);

    /* 永远闪下去，不进入任何内核 */
    while (1) {
        gpio_put(LED, 1);
        sleep_ms(250);
        gpio_put(LED, 0);
        sleep_ms(250);
    }
    return 0;
}
