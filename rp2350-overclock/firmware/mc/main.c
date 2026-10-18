// Multicore scaling probe: the same integer kernel on 1 or 2 cores.
// Reports iterations per core over a fixed wall-clock window plus a checksum
// check (each core must produce the reference value), so speed cannot be bought
// with wrong answers. CoreMark itself is single-threaded, so this kernel is the
// scaling instrument; CoreMark numbers are reported separately.
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/watchdog.h"
#include "oc.h"

#ifndef MAX_MHZ
#define MAX_MHZ 150
#endif
#ifndef VOLT_MV
#define VOLT_MV 1100
#endif
#ifndef CORES
#define CORES 2
#endif
#ifndef WINDOW_MS
#define WINDOW_MS 3000
#endif

#define BUF_WORDS 256

// KERNEL_IN_RAM=1 puts the hot loop in SRAM, removing the shared XIP cache from
// the picture. Comparing the two isolates flash-cache contention from SRAM
// contention when both cores run.
#ifdef KERNEL_IN_RAM
#define KFN(n) __not_in_flash_func(n)
#else
#define KFN(n) n
#endif

static uint32_t buf0[BUF_WORDS] __attribute__((aligned(64)));
static uint32_t buf1[BUF_WORDS] __attribute__((aligned(64)));

static volatile uint32_t res0_iters, res1_iters;
static volatile uint32_t res0_csum, res1_csum;
static volatile bool start_flag;

// Fixed amount of work, deterministic -> checksum must match across cores.
static uint32_t KFN(kernel_fixed)(uint32_t *b, uint32_t iters) {
    uint32_t x = 0x12345678u, y = 0x9e3779b9u, acc = 0;
    for (uint32_t it = 0; it < iters; it++) {
        for (uint32_t i = 0; i < 64; i++) {
            x = x * 1664525u + 1013904223u;
            y ^= (x >> 7);
            b[i] = x + y;
            acc += b[i] ^ (y >> 3);
        }
    }
    return acc;
}

static uint32_t KFN(kernel_timed)(uint32_t *b, uint32_t window_ms) {
    uint32_t x = 0x12345678u, y = 0x9e3779b9u;
    uint32_t iters = 0;
    absolute_time_t t0 = get_absolute_time();
    while (absolute_time_diff_us(t0, get_absolute_time()) < (int64_t)window_ms * 1000) {
        for (uint32_t i = 0; i < 64; i++) {
            x = x * 1664525u + 1013904223u;
            y ^= (x >> 7);
            b[i] = x + y;
        }
        iters++;
    }
    return iters;
}

static void core1_entry(void) {
    res1_csum = kernel_fixed(buf1, 1000);   // before the barrier: keeps start times aligned
    while (!start_flag) tight_loop_contents();
    res1_iters = kernel_timed(buf1, WINDOW_MS);
    __sev();
}

int main(void) {
    stdio_init_all();
    for (int i = 0; i < 120 && !stdio_usb_connected(); i++) sleep_ms(100);
    sleep_ms(1200);

    oc_apply(MAX_MHZ, VOLT_MV);

    printf("\n[mc] core0=%s cores=%d clock=%luMHz vreg=%dmV window=%dms gcc=%s T=%.1fC\n",
#ifdef __riscv
           "hazard3-rv32",
#else
           "cortex-m33",
#endif
           CORES, (unsigned long)oc_mhz(), VOLT_MV, WINDOW_MS, __VERSION__, oc_temp_c());

    uint32_t ref = kernel_fixed(buf0, 1000);
    printf("[mc] reference csum=0x%08lx\n", (unsigned long)ref);

    res0_csum = kernel_fixed(buf0, 1000);

#if CORES == 2
    start_flag = false;
    multicore_launch_core1(core1_entry);
    sleep_ms(50);                 // let core1 reach the barrier
    start_flag = true;
    __sev();
#endif

    uint32_t i0 = kernel_timed(buf0, WINDOW_MS);

#if CORES == 2
    while (res1_iters == 0) tight_loop_contents();
    uint32_t i1 = res1_iters;
    const char *ok0 = (res0_csum == ref) ? "OK" : "BAD";
    const char *ok1 = (res1_csum == ref) ? "OK" : "BAD";
    printf("[mc] core0 %lu iters (%s)  core1 %lu iters (%s)\n",
           (unsigned long)i0, ok0, (unsigned long)i1, ok1);
    printf("[mc] aggregate %lu iters, per-core factor %.3fx\n",
           (unsigned long)(i0 + i1), (double)(i0 + i1) / (2.0 * (double)i0));
#else
    printf("[mc] core0 %lu iters (%s)\n", (unsigned long)i0, res0_csum == ref ? "OK" : "BAD");
    printf("[mc] aggregate %lu iters, per-core factor 1.000x\n", (unsigned long)i0);
#endif
    printf("[mc] DONE T=%.1fC\n", oc_temp_c());
    while (true) {
        sleep_ms(3000);
        printf("[mc] idle %lu MHz T=%.1fC\n", (unsigned long)oc_mhz(), oc_temp_c());
    }
}
