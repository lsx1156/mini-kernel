/* Hazard3 payload for core 1, driven by an Arm core 0.
 * Position is fixed by link.ld; results are published to a shared SRAM word. */
#include <stdint.h>

#define RES ((volatile uint32_t *)0x20070000)
#define BUF ((volatile uint32_t *)0x20061000)
#define TIMERAWL (*(volatile uint32_t *)0x400b0028u)

__attribute__((noreturn)) void _start_c(void) {
    RES[0] = 0xC0DE0001u; /* reached */
    uint32_t x = 0x12345678u, y = 0x9e3779b9u, acc = 0;
    for (uint32_t it = 0; it < 1000; it++) {
        for (uint32_t i = 0; i < 64; i++) {
            x = x * 1664525u + 1013904223u;
            y ^= (x >> 7);
            BUF[i] = x + y;
            acc += BUF[i] ^ (y >> 3);
        }
    }
    RES[1] = acc;

    while (RES[4] == 0) { }

    uint32_t window_us = RES[2] * 1000u;
    uint32_t t0 = TIMERAWL;
    uint32_t iters = 0;
    do {
        for (uint32_t i = 0; i < 64; i++) {
            x = x * 1664525u + 1013904223u;
            y ^= (x >> 7);
            BUF[i] = x + y;
        }
        iters++;
    } while ((uint32_t)(TIMERAWL - t0) < window_us);

    RES[3] = iters;
    RES[0] = 0xC0DE0002u; /* done */
    for (;;) { }
}
