/* ================================================================
 * usb_print_test.c — 100% Pico SDK，零依赖 Mini Kernel
 *
 * 用途：验证 USB CDC 双向通信（输出 + 输入回显）。
 *   - 每 500ms 打印 CDC 状态诊断 + 测试行
 *   - 直接用 TinyUSB API 检查 CDC RX FIFO
 *   - LED 亮 100ms 标识收到数据
 * ================================================================ */
#include "pico/stdlib.h"
#include "tusb.h"
#include <stdio.h>

#ifndef PICO_DEFAULT_LED_PIN
#  define PICO_DEFAULT_LED_PIN 25
#endif

int main(void) {
    const uint LED = (uint)PICO_DEFAULT_LED_PIN;

    gpio_init(LED);
    gpio_set_dir(LED, GPIO_OUT);

    /* stdio_init_all() 同时启用 UART0 和 USB CDC */
    stdio_init_all();

    uint32_t n = 0;
    while (1) {
        /* 驱动 TinyUSB 状态机（双保险，SDK IRQ 也应该调） */
        tud_task();

        /* 打印 CDC 状态 + 测试行 */
        printf("[%lu] ready=%d cdc_conn=%d avail=%d\n",
               (unsigned long)n++,
               (int)tud_ready(),
               (int)tud_cdc_connected(),
               (int)tud_cdc_available());
        fflush(stdout);

        /* 方法 1：直接 TinyUSB CDC 读 */
        if (tud_cdc_available()) {
            char buf[64];
            uint32_t count = tud_cdc_read(buf, sizeof(buf) - 1);
            buf[count] = '\0';
            printf("CDC_RX(%u): %s\n", (unsigned)count, buf);
            gpio_put(LED, 1);
            sleep_ms(100);
            gpio_put(LED, 0);
        }

        /* 方法 2：SDK stdio 读（fallback） */
        int ch = getchar_timeout_us(0);
        if (ch != PICO_ERROR_TIMEOUT) {
            printf("STDIO_RX: 0x%02X\n", ch);
            gpio_put(LED, 1);
            sleep_ms(100);
            gpio_put(LED, 0);
        }

        sleep_ms(500);
    }
    return 0;
}
