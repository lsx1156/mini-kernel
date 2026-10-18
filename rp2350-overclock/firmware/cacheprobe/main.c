// Step 0: is the XIP cache shared between the two cores, and is that sharing
// observable as a timing effect in software?
//
// core1 sits in SRAM and measures the latency of reading one fixed flash word.
// core0 either idles, thrashes a small far region (bus contention only), or
// thrashes a 64 KB span (forces cache eviction). If only the large span hurts
// core1, the effect is cache, not bus arbitration.

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "oc.h"

#ifndef MAX_MHZ
#define MAX_MHZ 150
#endif
#ifndef VOLT_MV
#define VOLT_MV 1100
#endif

#define NSAMP   2000

// 64 KB of .rodata -> lives in flash (XIP), never copied to RAM by default
static const uint32_t flash_tbl[16384] __attribute__((used)) = { 0xDEADBEEF };

static volatile uint32_t go, done, mode;
static uint32_t samples[NSAMP];
static volatile uint32_t core1_view;

#define DEMCR     (*(volatile uint32_t *)0xE000EDFCu)
#define DWT_CTRL  (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYC   (*(volatile uint32_t *)0xE0001004u)
#define TRCENA          (1u << 24)
#define CYCCNTENA       (1u << 0)

static inline void dwt_on(void) {
    DEMCR |= TRCENA;
    DWT_CTRL |= CYCCNTENA;
    DWT_CYC = 0;
}

static void __not_in_flash_func(core1_main)(void) {
    dwt_on();
    const volatile uint32_t *p = &flash_tbl[0];
    for (;;) {
        while (!go) tight_loop_contents();
        go = 0;
        for (int i = 0; i < NSAMP; i++) {
            uint32_t t0 = DWT_CYC;
            core1_view = *p;
            samples[i] = DWT_CYC - t0;
        }
        done = 1;
    }
}

static uint32_t ram_tbl[1024] __attribute__((aligned(1024)));

static void __not_in_flash_func(alu_spin)(void) {
    uint32_t a = 1, b = 2, c = 3;
    while (!done) { a = a * 3 + b; b = b ^ c; c += a; }
    core1_view = a + b + c;
}

static void __not_in_flash_func(thrash_ram)(void) {
    uint32_t i = 0;
    while (!done) { ram_tbl[i++ & 0x3FF] = i; }
}

static void __not_in_flash_func(thrash_small)(void) {
    const volatile uint32_t *p = &flash_tbl[8192];   // 32 KB away
    uint32_t i = 0;
    while (!done) { core1_view += p[(i++ & 0x3FF)]; }
}

// 64 KB span 8 MB into flash: never overlaps the probe, so the probe line can
// actually be evicted (and is never refreshed by core0's own traffic).
static void __not_in_flash_func(thrash_far64k)(void) {
    const volatile uint32_t *p = (const volatile uint32_t *)0x10080000u;
    uint32_t i = 0;
    while (!done) { core1_view += p[(i++ & 0x3FFF)]; }
}

static void __not_in_flash_func(thrash_big)(void) {
    const volatile uint32_t *p = &flash_tbl[0];      // whole 64 KB span
    uint32_t i = 0;
    while (!done) { core1_view += p[(i++ & 0x3FFF)]; }
}

static void stats(const char *name, uint32_t clk) {
    uint32_t mn = 0xFFFFFFFF, mx = 0;
    uint64_t sum = 0;
    for (int i = 0; i < NSAMP; i++) {
        uint32_t s = samples[i];
        if (s < mn) mn = s;
        if (s > mx) mx = s;
        sum += s;
    }
    // median via 64-bucket histogram (cycles are small)
    uint32_t hist[64] = {0};
    for (int i = 0; i < NSAMP; i++) hist[samples[i] < 63 ? samples[i] : 63]++;
    uint32_t med = 0, acc = 0;
    for (int i = 0; i < 64; i++) { acc += hist[i]; if (acc * 2 >= NSAMP) { med = (uint32_t)i; break; } }
    printf("[cp] %-14s mean %5.1f  med %2lu  min %2lu  max %3lu  cyc  (%.1f ns mean)\n",
           name, (double)sum / NSAMP, (unsigned long)med, (unsigned long)mn, (unsigned long)mx,
           (double)sum / NSAMP * 1e9 / (double)clk);
}

int main(void) {
#if defined(MAX_MHZ) && (MAX_MHZ != 150)
    oc_apply(MAX_MHZ, VOLT_MV);
#endif
    stdio_init_all();
    watchdog_enable(20000, false);
    sleep_ms(1500);
    uint32_t clk = clock_get_hz(clk_sys);
    printf("\n[cp] clk_sys=%lu kHz temp=%.1f C\n", (unsigned long)(clk / 1000), oc_temp_c());
    printf("[cp] probe=%p  far=%p  samples=%d\n", (const void*)&flash_tbl[0],
           (const void*)&flash_tbl[8192], NSAMP);
    stdio_flush();

    multicore_launch_core1(core1_main);
    sleep_ms(50);

    // 0: core0 spins in registers only (no memory traffic at all)
    done = 0; go = 1;
    alu_spin();
    stats("core0 ALU only", clk);

    // 1: core0 thrashes 4 KB of SRAM (memory traffic, no XIP involvement)
    done = 0; go = 1;
    thrash_ram();
    stats("core0 SRAM 4KB", clk);

    // 2: core0 thrashes 4 KB of flash, all cache hits after warm-up
    done = 0; go = 1;
    thrash_small();
    stats("core0 flash 4KB", clk);

    // 3: core0 thrashes the whole 64 KB span (forced flash misses)
    done = 0; go = 1;
    thrash_big();
    stats("core0 flash 64KB", clk);

    // 4: 64 KB of flash 8 MB away - the only case that can evict the probe line
    done = 0; go = 1;
    thrash_far64k();
    stats("core0 far 64KB", clk);

    printf("[cp] done\n");
    stdio_flush();
    while (true) tight_loop_contents();
}
