// HSTX throughput probe.
// The CPU can only push ~25 Mword/s, so to measure the shifter itself the HSTX
// clock is set BELOW that from PLL_USB (48 MHz) with different dividers: the
// FIFO then throttles the CPU and the measured push rate is the HSTX rate.
// Rule under test: one 32-bit word per N_SHIFTS (=5) HSTX cycles.

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/watchdog.h"
#include "oc.h"

#ifndef MAX_MHZ
#define MAX_MHZ 150
#endif
#ifndef VOLT_MV
#define VOLT_MV 1100
#endif

#define FULL_BIT (1u << 8)

static uint32_t push_ms(uint32_t ms) {
    uint64_t end = time_us_64() + (uint64_t)ms * 1000ull;
    uint32_t n = 0;
    while (time_us_64() < end) {
        for (int i = 0; i < 512; i++) {
            while (hstx_fifo_hw->stat & FULL_BIT) tight_loop_contents();
            hstx_fifo_hw->fifo = 0x5A5A5A5Au;
            n++;
        }
    }
    return n;
}

static void hstx_pins(void) {
    for (int i = 0; i < 8; i++) gpio_set_function((uint)(12 + i), GPIO_FUNC_HSTX);

    hstx_ctrl_hw->bit[0] = (0u)  | (1u << 8);
    hstx_ctrl_hw->bit[1] = (0u)  | (1u << 8)  | (1u << 16);
    hstx_ctrl_hw->bit[2] = (10u) | (11u << 8);
    hstx_ctrl_hw->bit[3] = (10u) | (11u << 8) | (1u << 16);
    hstx_ctrl_hw->bit[4] = (20u) | (21u << 8);
    hstx_ctrl_hw->bit[5] = (20u) | (21u << 8) | (1u << 16);
    hstx_ctrl_hw->bit[6] = 0;
    hstx_ctrl_hw->bit[7] = 0;
    hstx_ctrl_hw->csr = (5u << 16) | (2u << 8) | 1u;
}

int main(void) {
#if defined(MAX_MHZ) && (MAX_MHZ != 150)
    oc_apply(MAX_MHZ, VOLT_MV);
#endif
    stdio_init_all();
    watchdog_enable(25000, false);
    sleep_ms(1500);

    uint32_t sys = clock_get_hz(clk_sys);
    printf("\n[hx] clk_sys=%lu kHz  pll_usb=%lu kHz  temp=%.1f C\n",
           (unsigned long)(sys / 1000), (unsigned long)(clock_get_hz(clk_usb) / 1000), oc_temp_c());
    stdio_flush();

    hstx_pins();

    for (int i = 0; i < 3; i++) {
        uint32_t div = (i == 0) ? 1u : (i == 1 ? 2u : 4u);
        hstx_ctrl_hw->csr = 0;
        clock_configure(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                        48000000u / div, 48000000u / div);
        hstx_ctrl_hw->csr = (5u << 16) | (2u << 8) | 1u;
        sleep_ms(20);

        uint32_t hz = clock_get_hz(clk_hstx);
        uint32_t n = push_ms(200);
        double wps = (double)n / 0.2;
        printf("[hx] clk_hstx=%7lu kHz  %6.2f Mword/s  expect %6.2f  (%.2f clk/word, %.1f Mpix/s, %.0f MB/s)\n",
               (unsigned long)(hz / 1000), wps / 1e6, (double)hz / 5.0 / 1e6,
               (double)hz / wps, wps / 1e6, wps * 4 / 1e6);
        stdio_flush();
    }

    // reference: HSTX on clk_sys (CPU-limited, shows the core cannot saturate it)
    hstx_ctrl_hw->csr = 0;
    clock_configure(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, sys, sys);
    hstx_ctrl_hw->csr = (5u << 16) | (2u << 8) | 1u;
    sleep_ms(20);
    uint32_t n = push_ms(200);
    printf("[hx] clk_hstx=%7lu kHz  %6.2f Mword/s  (CPU-bound reference)\n",
           (unsigned long)(clock_get_hz(clk_hstx) / 1000), (double)n / 0.2 / 1e6);
    stdio_flush();

    printf("[hx] done\n");
    stdio_flush();
    while (true) tight_loop_contents();
}
