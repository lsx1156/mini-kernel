#include "coremark.h"
#include "core_portme.h"
#include "pico/stdlib.h"
#include "oc.h"

#if VALIDATION_RUN
volatile ee_s32 seed1_volatile = 0x3415;
volatile ee_s32 seed2_volatile = 0x3415;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PERFORMANCE_RUN
volatile ee_s32 seed1_volatile = 0x0;
volatile ee_s32 seed2_volatile = 0x0;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PROFILE_RUN
volatile ee_s32 seed1_volatile = 0x8;
volatile ee_s32 seed2_volatile = 0x8;
volatile ee_s32 seed3_volatile = 0x8;
#endif
volatile ee_s32 seed4_volatile = ITERATIONS;
volatile ee_s32 seed5_volatile = 0;

/* 1 MHz tick from the 64-bit system timer; differences stay valid across the
 * 32-bit wrap because the arithmetic is modular. */
#define EE_TICKS_PER_SEC 1000000

static CORETIMETYPE start_time_val, stop_time_val;

CORETIMETYPE barebones_clock(void) {
    return (CORETIMETYPE)time_us_64();
}

void start_time(void) { start_time_val = barebones_clock(); }
void stop_time(void)  { stop_time_val = barebones_clock(); }

CORE_TICKS get_time(void) {
    return (CORE_TICKS)(stop_time_val - start_time_val);
}

secs_ret time_in_secs(CORE_TICKS ticks) {
    return ((secs_ret)ticks) / (secs_ret)EE_TICKS_PER_SEC;
}

ee_u32 default_num_contexts = 1;

void portable_init(core_portable *p, int *argc, char *argv[]) {
    (void)argc; (void)argv;
    if (p->portable_id != 1) {
        p->portable_id = 1;
    }
}

void portable_fini(core_portable *p) {
    p->portable_id = 0;
}
