// Mixed-architecture test: Arm Cortex-M33 on core 0, Hazard3 (RISC-V) on core 1.
//
// The chip samples OTP_ARCHSEL when a processor reset is released, so the order
// matters: flip ARCHSEL.CORE1, power-cycle core 1, then hand it a RISC-V entry
// through the same FIFO launch protocol the bootrom uses for Arm.
#include <stdio.h>
#include <string.h>
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
#ifndef WINDOW_MS
#define WINDOW_MS 3000
#endif

#define OTP_ARCHSEL (*(volatile uint32_t *)0x40120158)
#define OTP_ARCHSEL_STATUS (*(volatile uint32_t *)0x4012015c)

#define PAYLOAD_BASE 0x20060000u
#define RES ((volatile uint32_t *)0x20070000)
#define STACK_BASE 0x2006F000u
#define STACK_SIZE 0x1000u

#define RV_STAGE_REACHED 0xC0DE0001u
#define RV_STAGE_DONE    0xC0DE0002u

extern const uint8_t payload_bin[];
extern const uint32_t payload_bin_len;

static uint32_t buf0[256] __attribute__((aligned(64)));

static uint32_t kernel_fixed(uint32_t *b, uint32_t iters) {
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

static uint32_t kernel_timed(uint32_t *b, uint32_t window_ms) {
    uint32_t x = 0x12345678u, y = 0x9e3779b9u, iters = 0;
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

int main(void) {
    stdio_init_all();
    for (int i = 0; i < 120 && !stdio_usb_connected(); i++) sleep_ms(100);
    sleep_ms(1200);

    oc_apply(MAX_MHZ, VOLT_MV);

    printf("\n[mx] core0=cortex-m33 core1=hazard3-rv32 clock=%luMHz vreg=%dmV gcc=%s\n",
           (unsigned long)oc_mhz(), VOLT_MV, __VERSION__);
    printf("[mx] ARCHSEL before=0x%08lx status=0x%08lx\n",
           (unsigned long)OTP_ARCHSEL, (unsigned long)OTP_ARCHSEL_STATUS);

    memcpy((void *)PAYLOAD_BASE, payload_bin, payload_bin_len);
    printf("[mx] payload %lu bytes -> 0x%08lx\n",
           (unsigned long)payload_bin_len, (unsigned long)PAYLOAD_BASE);
    uint32_t ref_acc = kernel_fixed(buf0, 1000);
    printf("[mx] reference csum=0x%08lx\n", (unsigned long)ref_acc);

    RES[0] = 0;
    RES[1] = 0;

    // 1) select RISC-V for core 1 (bit 1)
    OTP_ARCHSEL = OTP_ARCHSEL | 0x2u;
    printf("[mx] ARCHSEL after write=0x%08lx status=0x%08lx\n",
           (unsigned long)OTP_ARCHSEL, (unsigned long)OTP_ARCHSEL_STATUS);

    // 2) power-cycle core 1 so it samples the new value
    multicore_reset_core1();
    sleep_ms(10);
    printf("[mx] core1 reset done, status=0x%08lx\n", (unsigned long)OTP_ARCHSEL_STATUS);

    // 4) start both cores together and run the same timed kernel
    RES[2] = WINDOW_MS;
    RES[4] = 0;
    multicore_launch_core1_raw((void (*)(void))PAYLOAD_BASE, (uint32_t *)STACK_BASE, PAYLOAD_BASE);
    printf("[mx] core1 launched at 0x%08lx sp=0x%08lx\n",
           (unsigned long)PAYLOAD_BASE, (unsigned long)STACK_BASE);
    sleep_ms(30);                 // let the RISC-V side reach the barrier
    RES[4] = 1;

    uint32_t i0 = kernel_timed(buf0, WINDOW_MS);

    uint32_t stage = 0;
    for (int i = 0; i < 500; i++) {
        stage = RES[0];
        if (stage == RV_STAGE_DONE) break;
        sleep_ms(10);
    }
    printf("[mx] core1 stage=0x%08lx csum=0x%08lx iters=%lu\n",
           (unsigned long)stage, (unsigned long)RES[1], (unsigned long)RES[3]);
    printf("[mx] core0 iters=%lu csum=0x%08lx (expect 0x1f1c333c)\n",
           (unsigned long)i0, (unsigned long)ref_acc);
    if (stage == RV_STAGE_DONE && RES[1] == ref_acc) {
        printf("[mx] MIXED OK: aggregate %lu iters over %dms (%.0f iters/ms)\n",
               (unsigned long)(i0 + RES[3]), WINDOW_MS, (double)(i0 + RES[3]) / (double)WINDOW_MS);
    } else {
        printf("[mx] MIXED FAIL: stage=0x%08lx\n", (unsigned long)stage);
    }
    printf("[mx] DONE T=%.1fC\n", oc_temp_c());

    while (true) {
        sleep_ms(3000);
        printf("[mx] idle %lu MHz T=%.1fC stage=0x%08lx\n",
               (unsigned long)oc_mhz(), oc_temp_c(), (unsigned long)RES[0]);
    }
}
