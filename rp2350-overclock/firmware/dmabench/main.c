// DMA double-buffering benchmark for RP2350 (SRAM -> SRAM copy path).
//
// A) blocking: one channel; CPU re-arms and waits for every chunk
// B) double buffered: two channels; the completion IRQ immediately arms and
//    starts the other one, so the bus never waits for the CPU to re-arm
//
// C) concurrency: continuous DMA copy while the CPU streams a third buffer.

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "oc.h"

#ifndef MAX_MHZ
#define MAX_MHZ 150
#endif
#ifndef VOLT_MV
#define VOLT_MV 1100
#endif

#define NW      16384u      // 64 KB buffers
#define TOTAL   (1u << 19)  // words per case

static uint32_t src[NW] __attribute__((aligned(1024)));
static uint32_t dst[NW] __attribute__((aligned(1024)));
static uint32_t other[NW] __attribute__((aligned(1024)));

static volatile uint32_t irq_chunks;
static volatile int g_ch_a = -1, g_ch_b = -1;
static volatile uint32_t g_chunk;

static void dma_isr(void) {
    uint32_t s = dma_hw->ints0;
    dma_hw->ints0 = s;

    if (g_ch_a >= 0 && (s & (1u << g_ch_a))) {
        irq_chunks++;
        if (g_ch_b < 0) {   // continuous single-channel mode
            dma_channel_set_write_addr((uint)g_ch_a, dst, false);
            dma_channel_set_read_addr((uint)g_ch_a, src, false);
            dma_channel_set_trans_count((uint)g_ch_a, g_chunk, true);
        } else {
            dma_channel_set_write_addr((uint)g_ch_b, dst, false);
            dma_channel_set_read_addr((uint)g_ch_b, src, false);
            dma_channel_set_trans_count((uint)g_ch_b, g_chunk, true);
        }
    } else if (g_ch_b >= 0 && (s & (1u << g_ch_b))) {
        irq_chunks++;
        dma_channel_set_write_addr((uint)g_ch_a, dst, false);
        dma_channel_set_read_addr((uint)g_ch_a, src, false);
        dma_channel_set_trans_count((uint)g_ch_a, g_chunk, true);
    }
}

static uint32_t __not_in_flash_func(read32)(const uint32_t *p, uint n, uint reps) {
    uint32_t c0 = 0, c1 = 0, c2 = 0, c3 = 0;
    for (uint r = 0; r < reps; r++) {
        for (uint i = 0; i < n; i += 8) {
            c0 += p[i+0] + p[i+4];
            c1 += p[i+1] + p[i+5];
            c2 += p[i+2] + p[i+6];
            c3 += p[i+3] + p[i+7];
        }
    }
    return c0 + c1 + c2 + c3;
}

static dma_channel_config copy_cfg(int ch) {
    dma_channel_config c = dma_channel_get_default_config((uint)ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);
    return c;
}

static double run_blocking(uint chunk) {
    int ch = dma_claim_unused_channel(true);
    dma_channel_config c = copy_cfg(ch);
    dma_channel_configure((uint)ch, &c, dst, src, chunk, false);

    uint64_t t0 = time_us_64();
    for (uint done = 0; done < TOTAL; done += chunk) {
        dma_channel_set_write_addr((uint)ch, dst, false);
        dma_channel_set_read_addr((uint)ch, src, false);
        dma_channel_set_trans_count((uint)ch, chunk, true);
        dma_channel_wait_for_finish_blocking((uint)ch);
    }
    uint64_t us = time_us_64() - t0;
    dma_channel_unclaim((uint)ch);
    return (double)TOTAL * 4 / ((double)us * 1e-6);
}

static double run_double(uint chunk) {
    int a = dma_claim_unused_channel(true);
    int b = dma_claim_unused_channel(true);
    g_chunk = chunk;

    dma_channel_config ca = copy_cfg(a);
    dma_channel_config cb = copy_cfg(b);
    dma_channel_configure((uint)a, &ca, dst, src, chunk, false);
    dma_channel_configure((uint)b, &cb, dst, src, chunk, false);
    dma_channel_set_irq0_enabled((uint)a, true);
    dma_channel_set_irq0_enabled((uint)b, true);

    g_ch_a = a; g_ch_b = b;
    irq_chunks = 0;
    uint64_t t0 = time_us_64();
    dma_channel_start((uint)a);

    while (irq_chunks * chunk < TOTAL) { watchdog_update(); tight_loop_contents(); }
    uint64_t us = time_us_64() - t0;

    dma_channel_abort((uint)a);
    dma_channel_abort((uint)b);
    dma_channel_set_irq0_enabled((uint)a, false);
    dma_channel_set_irq0_enabled((uint)b, false);
    dma_channel_unclaim((uint)a);
    dma_channel_unclaim((uint)b);
    g_ch_a = g_ch_b = -1;

    return (double)irq_chunks * chunk * 4 / ((double)us * 1e-6);
}

int main(void) {
#if defined(MAX_MHZ) && (MAX_MHZ != 150)
    oc_apply(MAX_MHZ, VOLT_MV);
#endif
    stdio_init_all();
    for (uint i = 0; i < NW; i++) { src[i] = i * 2654435761u; other[i] = ~src[i]; dst[i] = 0; }

    irq_set_exclusive_handler(DMA_IRQ_0, dma_isr);
    irq_set_enabled(DMA_IRQ_0, true);

    watchdog_enable(12000, false);   // a hang reboots instead of wedging USB

    sleep_ms(1500);
    uint32_t clk = clock_get_hz(clk_sys);
    printf("\n[db] clk_sys=%lu kHz temp=%.1f C\n", (unsigned long)(clk / 1000), oc_temp_c());
    stdio_flush();

    const uint chunks[] = {1024, 4096, 16384};
    for (int i = 0; i < 3; i++) {
        double r1 = run_blocking(chunks[i]);
        double r2 = run_double(chunks[i]);
        printf("[db] copy chunk=%5u  blocking %6.2f GB/s   double %6.2f GB/s   gain %+5.1f%%   (%.2f -> %.2f B/clk)\n",
               chunks[i], r1 / 1e9, r2 / 1e9, (r2 / r1 - 1.0) * 100.0, r1 / clk, r2 / clk);
        stdio_flush();
    }

    int cc = dma_claim_unused_channel(true);
    dma_channel_config cccc = copy_cfg(cc);
    g_ch_a = cc; g_ch_b = -1; g_chunk = NW;
    dma_channel_configure((uint)cc, &cccc, dst, src, NW, false);
    dma_channel_set_irq0_enabled((uint)cc, true);

    irq_chunks = 0;
    uint64_t t0 = time_us_64();
    dma_channel_start((uint)cc);
    while (irq_chunks < 128) { watchdog_update(); tight_loop_contents(); }
    uint64_t us = time_us_64() - t0;
    double dma_alone = (double)NW * 4 * 128 / ((double)us * 1e-6);
    printf("[db] dma copy alone      %6.2f GB/s (%.2f B/clk)\n", dma_alone / 1e9, dma_alone / clk);
    stdio_flush();

    irq_chunks = 0;
    t0 = time_us_64();
    dma_channel_start((uint)cc);
    uint32_t acc = read32(other, NW, 128);
    while (irq_chunks < 128) { watchdog_update(); tight_loop_contents(); }
    us = time_us_64() - t0;
    double both = (double)NW * 4 * 128 / ((double)us * 1e-6);
    printf("[db] dma copy + cpu read %6.2f GB/s each | sum %6.2f GB/s (%.2f B/clk) acc=%08lx\n",
           both / 1e9, both * 2 / 1e9, both * 2 / clk, (unsigned long)acc);
    stdio_flush();

    dma_channel_abort((uint)cc);
    dma_channel_set_irq0_enabled((uint)cc, false);
    dma_channel_unclaim((uint)cc);
    g_ch_a = -1;

    printf("[db] done\n");
    stdio_flush();
    while (true) tight_loop_contents();
}
