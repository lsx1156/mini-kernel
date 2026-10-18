// SRAM bandwidth benchmark: separates pure read, pure write, copy, access width,
// DMA copy and dual-core read so the earlier "SRAM 587 MB/s" figure can be
// reconciled with the PIO-DMA result (1.83 GB/s at 588 MHz).
//
// Everything hot is __not_in_flash_func so code runs from SRAM.

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "oc.h"

#ifndef MAX_MHZ
#define MAX_MHZ 150
#endif
#ifndef VOLT_MV
#define VOLT_MV 1100
#endif

#define NW      32768u            // 128 KB per buffer
#define REPS    400u

static uint32_t bufA[NW] __attribute__((aligned(1024)));
static uint32_t bufB[NW] __attribute__((aligned(1024)));

static volatile uint32_t sink;
static volatile uint32_t core1_acc;
static volatile uint32_t core1_go;
static volatile uint32_t core1_done;

static uint32_t __not_in_flash_func(read32)(const uint32_t *p, uint n, uint reps) {
    uint32_t c0 = 0, c1 = 0, c2 = 0, c3 = 0;
    for (uint r = 0; r < reps; r++) {
        const uint32_t *q = p;
        for (uint i = 0; i < n; i += 8) {
            c0 += q[i+0] + q[i+4];
            c1 += q[i+1] + q[i+5];
            c2 += q[i+2] + q[i+6];
            c3 += q[i+3] + q[i+7];
        }
    }
    return c0 + c1 + c2 + c3;
}

static uint32_t __not_in_flash_func(write32)(uint32_t *p, uint n, uint reps) {
    for (uint r = 0; r < reps; r++) {
        uint32_t *q = p;
        for (uint i = 0; i < n; i += 8) {
            q[i+0] = 0x11111111u; q[i+1] = 0x22222222u;
            q[i+2] = 0x33333333u; q[i+3] = 0x44444444u;
            q[i+4] = 0x55555555u; q[i+5] = 0x66666666u;
            q[i+6] = 0x77777777u; q[i+7] = 0x88888888u;
        }
    }
    return 0;
}

static uint32_t __not_in_flash_func(copy32)(uint32_t *d, const uint32_t *s, uint n, uint reps) {
    for (uint r = 0; r < reps; r++) {
        for (uint i = 0; i < n; i += 8) {
            d[i+0] = s[i+0]; d[i+1] = s[i+1]; d[i+2] = s[i+2]; d[i+3] = s[i+3];
            d[i+4] = s[i+4]; d[i+5] = s[i+5]; d[i+6] = s[i+6]; d[i+7] = s[i+7];
        }
    }
    return 0;
}

#if !defined(__riscv)
// Same work with LDM/STM (one instruction moves four words). Tests whether the
// core's load/store pipeline - not SRAM - is what limits CPU-side bandwidth.
static uint32_t __not_in_flash_func(read32_ldm)(const uint32_t *p, uint n, uint reps) {
    uint32_t acc = 0;
    for (uint r = 0; r < reps; r++) {
        const uint32_t *q = p;
        for (uint i = 0; i < n; i += 4) {
            uint32_t a, b, c, d;
            __asm volatile ("ldmia %0!, {%1, %2, %3, %4}"
                            : "+r"(q), "=r"(a), "=r"(b), "=r"(c), "=r"(d));
            acc += a + b + c + d;
        }
    }
    return acc;
}

static uint32_t __not_in_flash_func(copy32_ldm)(uint32_t *d, const uint32_t *s, uint n, uint reps) {
    for (uint r = 0; r < reps; r++) {
        const uint32_t *sp = s;
        uint32_t *dp = d;
        for (uint i = 0; i < n; i += 4) {
            uint32_t a, b, c, e;
            __asm volatile ("ldmia %0!, {%2, %3, %4, %5}\n\tstmia %1!, {%2, %3, %4, %5}"
                            : "+r"(sp), "+r"(dp), "=r"(a), "=r"(b), "=r"(c), "=r"(e)
                            :
                            : "memory");
        }
    }
    return 0;
}
#endif

static uint32_t __not_in_flash_func(read8)(const uint8_t *p, uint n, uint reps) {
    uint32_t c0 = 0, c1 = 0;
    for (uint r = 0; r < reps; r++) {
        for (uint i = 0; i < n; i += 8) {
            c0 += p[i+0] + p[i+2] + p[i+4] + p[i+6];
            c1 += p[i+1] + p[i+3] + p[i+5] + p[i+7];
        }
    }
    return c0 + c1;
}

static uint32_t __not_in_flash_func(copy8)(uint8_t *d, const uint8_t *s, uint n, uint reps) {
    for (uint r = 0; r < reps; r++)
        for (uint i = 0; i < n; i++) d[i] = s[i];
    return 0;
}

static void __not_in_flash_func(core1_main)(void) {
    while (!core1_go) tight_loop_contents();
    core1_acc = read32(bufB, NW, REPS);
    core1_done = 1;
}

static void report(const char *name, double bytes, uint64_t us, uint32_t clk_hz) {
    double bps = bytes / ((double)us * 1e-6);
    printf("[sb] %-18s %8.2f GB/s   %5.2f B/clk\n",
           name, bps / 1e9, bps / (double)clk_hz);
}

int main(void) {
#if defined(MAX_MHZ) && (MAX_MHZ != 150)
    oc_apply(MAX_MHZ, VOLT_MV);
#endif
    stdio_init_all();
    for (uint i = 0; i < NW; i++) { bufA[i] = i * 2654435761u; bufB[i] = ~bufA[i]; }

    sleep_ms(2000);
    uint32_t clk = clock_get_hz(clk_sys);
    printf("\n[sb] clk_sys=%lu kHz  temp=%.1f C\n", (unsigned long)(clk / 1000), oc_temp_c());
    printf("[sb] buffers: A=%p B=%p  %u KB each, reps=%u\n",
           (void*)bufA, (void*)bufB, NW * 4 / 1024, REPS);

    uint64_t t;
    uint32_t acc;

    t = time_us_64(); acc = read32(bufA, NW, REPS); sink = acc;
    report("read32 (pure)", (double)NW * 4 * REPS, time_us_64() - t, clk);

    t = time_us_64(); write32(bufA, NW, REPS);
    report("write32 (pure)", (double)NW * 4 * REPS, time_us_64() - t, clk);

    t = time_us_64(); copy32(bufB, bufA, NW, REPS);
    report("copy32 (r+w)", (double)NW * 4 * REPS, time_us_64() - t, clk);

#if !defined(__riscv)
    t = time_us_64(); acc = read32_ldm(bufA, NW, REPS); sink = acc;
    report("read32 LDM", (double)NW * 4 * REPS, time_us_64() - t, clk);

    t = time_us_64(); copy32_ldm(bufB, bufA, NW, REPS);
    report("copy32 LDM/STM", (double)NW * 4 * REPS, time_us_64() - t, clk);
#endif

    t = time_us_64(); acc = read8((const uint8_t*)bufA, NW * 4, REPS / 4); sink = acc;
    report("read8 (pure)", (double)NW * 4 * (REPS / 4), time_us_64() - t, clk);

    t = time_us_64(); copy8((uint8_t*)bufB, (const uint8_t*)bufA, NW * 4, REPS / 4);
    report("copy8 (r+w)", (double)NW * 4 * (REPS / 4), time_us_64() - t, clk);

    // DMA SRAM -> SRAM
    int ch = dma_claim_unused_channel(true);
    dma_channel_config dc = dma_channel_get_default_config((uint)ch);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, true);
    dma_channel_configure((uint)ch, &dc, bufB, bufA, NW, true);
    dma_channel_wait_for_finish_blocking((uint)ch);
    t = time_us_64();
    for (uint r = 0; r < REPS; r++) {
        dma_channel_set_read_addr((uint)ch, bufA, false);
        dma_channel_set_write_addr((uint)ch, bufB, false);
        dma_channel_set_trans_count((uint)ch, NW, true);
        dma_channel_wait_for_finish_blocking((uint)ch);
    }
    report("dma copy32 (r+w)", (double)NW * 4 * REPS, time_us_64() - t, clk);
    dma_channel_unclaim((uint)ch);

    // dual core: both cores read a different buffer
    multicore_launch_core1(core1_main);
    core1_go = 1;
    t = time_us_64();
    uint32_t a0 = read32(bufA, NW, REPS);
    while (!core1_done) tight_loop_contents();
    uint64_t us = time_us_64() - t;
    report("read32 x2 cores", 2.0 * NW * 4 * REPS, us, clk);

    printf("[sb] done acc=%lx/%lx\n", (unsigned long)a0, (unsigned long)core1_acc);
    while (true) tight_loop_contents();
}
