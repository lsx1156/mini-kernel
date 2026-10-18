// 电压 × 频率 扫描台：一次闪烁，主机远程指定每个点
//   S <mhz> <mv> <sec>   设频设压，跑 sec 秒负载，回报
//   V                    版本
//   B                    进 BOOTSEL（以后再刷机不用拔线）
// 卡死由 20 s 硬件看门狗复位，主机侧记为该点 HANG。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "hardware/adc.h"
#include "hardware/vreg.h"
#include "oc.h"

static uint32_t buf[4096] __attribute__((aligned(16)));
static uint32_t golden;

static uint32_t verify(uint32_t rounds) {
    uint32_t a = 0x12345678u, b = 0x9abcdef0u, c = 0x0f0f0f0fu, d = 0xf0f0f0f0u;
    for (uint32_t r = 0; r < rounds; r++) {
        for (int i = 0; i < 1024; i += 4) {
            a = a * 1664525u + buf[i]     + d;
            b = b * 22695477u + buf[i + 1] + a;
            c = c * 1103515245u + buf[i + 2] + b;
            d = d * 214013u + buf[i + 3] + c;
        }
    }
    return a ^ (b + c) ^ d;
}

static float temp_c(void) {
    adc_select_input(4);
    float v = adc_read() * 3.3f / 4096.0f;
    return 27.0f - (v - 0.706f) / 0.001721f;
}

int main(void) {
    stdio_init_all();
    stdio_set_translate_crlf(&stdio_usb, false);
    while (!stdio_usb_connected()) sleep_ms(10);
    sleep_ms(300);

    for (int i = 0; i < 4096; i++) buf[i] = 0x9e3779b9u * (uint32_t)(i + 1);

    watchdog_enable(20000, 0);
    adc_init();
    adc_set_temp_sensor_enabled(true);

    golden = verify(50);
    printf("READY vsweep clk=%lu mv=%u golden=%08lx\n",
           (unsigned long)clock_get_hz(clk_sys), (unsigned)vreg_get_voltage(), (unsigned long)golden);

    char line[64]; int li = 0;
    for (;;) {
        watchdog_update();
        int ch = getchar_timeout_us(2000);
        if (ch == PICO_ERROR_TIMEOUT) continue;
        if (ch == '\r') continue;
        if (ch != '\n') { if (li < 63) line[li++] = (char)ch; continue; }
        line[li] = 0; li = 0;

        unsigned long mhz, mv, sec;
        if (line[0] == 'S' && sscanf(line, "S %lu %lu %lu", &mhz, &mv, &sec) == 3) {
            oc_apply((uint32_t)mhz, (uint32_t)mv);
            uint32_t rounds = 0;
            uint64_t end = time_us_64() + (uint64_t)sec * 1000000ull;
            while (time_us_64() < end) {
                for (int i = 0; i < 4096; i += 4) {
                    buf[i]     = buf[i]     * 1664525u + 1u;
                    buf[i + 1] = buf[i + 1] * 22695477u + 2u;
                    buf[i + 2] = buf[i + 2] * 1103515245u + 3u;
                    buf[i + 3] = buf[i + 3] * 214013u + 4u;
                }
                rounds++;
                watchdog_update();
            }
            for (int i = 0; i < 4096; i++) buf[i] = 0x9e3779b9u * (uint32_t)(i + 1);
            uint32_t v = verify(50);
            printf("RES %lu %lu %lu %lu %08lx %s %.1f\n",
                   mhz, mv, sec, (unsigned long)rounds, (unsigned long)v,
                   (v == golden) ? "OK" : "MISMATCH", (double)temp_c());
        } else if (line[0] == 'C' && sscanf(line, "C %lu %lu", &mhz, &mv) == 2) {
            oc_apply((uint32_t)mhz, (uint32_t)mv);
            printf("CFG %lu %lu\n", mhz, (unsigned long)(clock_get_hz(clk_sys) / 1000000u));
        } else if (line[0] == 'V') {
            printf("vsweep clk=%lu mv=%u\n", (unsigned long)clock_get_hz(clk_sys), (unsigned)vreg_get_voltage());
        } else if (line[0] == 'B') {
            printf("BOOT\n"); sleep_ms(50);
            reset_usb_boot(0, 0);
        } else if (line[0]) {
            printf("ERR\n");
        }
    }
}
