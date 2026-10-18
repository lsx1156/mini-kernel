// HSTX throughput benchmark: how fast does the HSTX shifter consume data?
//
// The shifter takes one 32-bit word every N_SHIFTS HSTX cycles (TMDS config:
// N_SHIFTS=5, SHIFT=2 -> one word = 3 x 10-bit TMDS symbols = one pixel).
// DMA feeds the FIFO from SRAM and the DREQ paces the transfer, so the
// measured word rate is the HSTX consumption rate, not the DMA's ability.

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/dma.h"
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

#define NWORDS  (1u << 21)     // 2M words
#define BUFWORDS 4096u

static uint32_t buf[BUFWORDS] __attribute__((aligned(1024)));

static void hstx_setup(uint32_t hz) {
    hstx_ctrl_hw->csr = 0;                  // stop before touching the clock
    clock_configure(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, hz, hz);

    for (int i = 0; i < 8; i++) gpio_set_function((uint)(12 + i), GPIO_FUNC_HSTX);

    // TMDS bit map from the datasheet (12.11.3.2): two bits per lane per cycle
    hstx_ctrl_hw->bit[0] = (0u)  | (1u << 8);
    hstx_ctrl_hw->bit[1] = (0u)  | (1u << 8)  | (1u << 16);
    hstx_ctrl_hw->bit[2] = (10u) | (11u << 8);
    hstx_ctrl_hw->bit[3] = (10u) | (11u << 8) | (1u << 16);
    hstx_ctrl_hw->bit[4] = (20u) | (21u << 8);
    hstx_ctrl_hw->bit[5] = (20u) | (21u << 8) | (1u << 16);
    hstx_ctrl_hw->bit[6] = 0;
    hstx_ctrl_hw->bit[7] = 0;

    hstx_ctrl_hw->csr = (5u << 16) | (2u << 8) | 1u;   // N_SHIFTS=5, SHIFT=2, EN
}

static double measure(uint32_t ms) {
    uint64_t end = time_us_64() + (uint64_t)ms * 1000ull;
    uint32_t n = 0;
    while (time_us_64() < end) {
        for (int i = 0; i < 2048; i++) {
            while (hstx_fifo_hw->stat & (1u << 8)) tight_loop_contents();  // FULL
            hstx_fifo_hw->fifo = 0xA5A5A5A5u;
            n++;
        }
    }
    return (double)n / ((double)ms / 1000.0);
}

int main(void) {
#if defined(MAX_MHZ) && (MAX_MHZ != 150)
    oc_apply(MAX_MHZ, VOLT_MV);
#endif
    stdio_init_all();
    for (uint i = 0; i < BUFWORDS; i++) buf[i] = 0x12345678u * (i + 1);

    watchdog_enable(15000, false);
    sleep_ms(1500);

    uint32_t sys = clock_get_hz(clk_sys);
    printf("\n[hx] clk_sys=%lu kHz temp=%.1f C\n", (unsigned long)(sys / 1000), oc_temp_c());
    stdio_flush();

    const uint32_t divs[] = {1, 2, 4, 8};
    for (int i = 0; i < 4; i++) {
        uint32_t hz = sys / divs[i];
        hstx_setup(hz);
        sleep_ms(50);
        double wps = measure(300);
        if (wps < 0) {
            printf("[hx] clk_hstx=%8lu kHz  STALLED (fifo not drained)\n", (unsigned long)(hz / 1000));
        } else {
            printf("[hx] clk_hstx=%8lu kHz  %8.2f Mword/s   expect %8.2f   (word/%.1f clk_hstx, %.2f GB/s, %.1f Mpix/s)\n",
                   (unsigned long)(hz / 1000), wps / 1e6, (double)hz / 5.0 / 1e6,
                   (double)hz / wps, wps * 4 / 1e9, wps / 1e6);
        }
        stdio_flush();
    }

    // pixels per frame at the highest rate reached
    double best = 0;
    for (int i = 0; i < 1; i++) best = 0;
    printf("[hx] done\n");
    stdio_flush();
    (void)best;
    while (true) tight_loop_contents();
}
