// RP2350 frequency/voltage map + soak.
//
// Sweeps (clk_sys, DVDD) points in ONE session that survives watchdog resets:
//   scratch[0] = step being run, scratch[1..3] = done / pass / hang bitmaps.
// A hang resets the chip; the next boot records that step as HANG and continues
// with the following point instead of starting over (which would reheat the die
// and corrupt the measurement).
//
// Per point: widen the QSPI divider (SDK's set_sys_clock_khz() leaves it alone,
// so flash SCK would otherwise track clk_sys and kill XIP), set DVDD, set clock,
// run checksummed workloads against the 150 MHz golden run, print throughput and
// die temperature.
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "hardware/adc.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/watchdog.h"
#include "hardware/regs/qmi.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h"
#include "hardware/structs/powman.h"
#include "hardware/regs/powman.h"
#include "hardware/platform_defs.h"

#define S_STEP (watchdog_hw->scratch[0])
#define S_DONE (watchdog_hw->scratch[1])
#define S_PASS (watchdog_hw->scratch[2])
#define S_FAIL (watchdog_hw->scratch[3])
#define MARK_STEP 0xA5000000u
#define MARK_END  0xC0000000u

#ifndef SOAK_S
#define SOAK_S 30
#endif

typedef struct {
    uint32_t mhz;           // multiple of 12: this board's XOSC is 12 MHz
    enum vreg_voltage mv;
    const char *label;
    bool unlock;            // clear POWMAN_VREG_CTRL.DISABLE_VOLTAGE_LIMIT first
} step_t;

// VREG voltage codes step in 50 mV increments up to 1.30 V (0b01111); there is
// no 1.26/1.28 setting. Above 1.30 the next code is 1.35 V and needs
// POWMAN_VREG_CTRL_DISABLE_VOLTAGE_LIMIT.
static float mv_of(enum vreg_voltage v) {
    switch (v) {
        case VREG_VOLTAGE_1_10: return 1.10f;
        case VREG_VOLTAGE_1_15: return 1.15f;
        case VREG_VOLTAGE_1_20: return 1.20f;
        case VREG_VOLTAGE_1_25: return 1.25f;
        case VREG_VOLTAGE_1_30: return 1.30f;
        case VREG_VOLTAGE_1_35: return 1.35f;
        default: return -1.0f;
    }
}

static const step_t LADDER[] = {
    { 150, VREG_VOLTAGE_1_10, "stock/golden", false },
    { 420, VREG_VOLTAGE_1_30, "ctrl 420 @1.30", false },
    { 444, VREG_VOLTAGE_1_35, "target 444 @1.35", true },
    { 444, VREG_VOLTAGE_1_35, "target 444 @1.35 (retry)", true },
};

static const uint32_t N_ALU  = 2000000;
static const uint32_t N_MULD = 500000;
static const uint32_t N_F32  = 500000;
static const uint32_t N_F64  = 200000;

static volatile uint32_t sink32;
static volatile float sinkf;
static volatile double sinkd;

static void __no_inline_not_in_flash_func(set_flash_div)(uint32_t div) {
    uint32_t save = save_and_disable_interrupts();
    uint32_t t = qmi_hw->m[0].timing;
    t &= ~QMI_M0_TIMING_CLKDIV_BITS;
    t |= (div << QMI_M0_TIMING_CLKDIV_LSB) & QMI_M0_TIMING_CLKDIV_BITS;
    qmi_hw->m[0].timing = t;
    (void)qmi_hw->m[0].timing;
    restore_interrupts(save);
}

static uint32_t even_div_for(uint32_t khz) {
    uint32_t d = 2;
    while (khz / d > 80000) d += 2;     // keep flash SCK <= 80 MHz
    return d;
}

static float die_temp_c(void) {
    adc_select_input(4);
    uint16_t raw = adc_read();
    float v = (float)raw * 3.3f / 4096.0f;
    return 27.0f - (v - 0.706f) / 0.001721f;
}

static uint32_t bench_alu(uint32_t n, double *ops_per_s) {
    uint32_t x = 0x12345678u;
    uint64_t t0 = time_us_64();
    for (uint32_t i = 0; i < n; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; x += i * 2654435761u; }
    *ops_per_s = (double)n / ((time_us_64() - t0) * 1e-6);
    return x;
}

static uint32_t bench_muldiv(uint32_t n, double *ops_per_s) {
    uint32_t x = 0x9E3779B9u;
    uint64_t t0 = time_us_64();
    for (uint32_t i = 0; i < n; i++) x = (x * 1664525u + 1013904223u) ^ (x / 7u) ^ (x % 255u);
    *ops_per_s = (double)n / ((time_us_64() - t0) * 1e-6);
    return x;
}

static uint32_t bench_f32(uint32_t n, double *ops_per_s) {
    float f = 1.0f;
    uint64_t t0 = time_us_64();
    for (uint32_t i = 0; i < n; i++) f = f * 1.0000001f + (float)(i & 0xff) * 0.5f;
    *ops_per_s = (double)n / ((time_us_64() - t0) * 1e-6);
    sinkf = f;
    return (uint32_t)(f * 1e6f);
}

static uint32_t bench_f64(uint32_t n, double *ops_per_s) {
    double d = 1.0;
    uint64_t t0 = time_us_64();
    for (uint32_t i = 0; i < n; i++) d = d * 1.0000001 + (double)(i & 0xff) * 0.5;
    *ops_per_s = (double)n / ((time_us_64() - t0) * 1e-6);
    sinkd = d;
    return (uint32_t)(d * 1e6);
}

static uint32_t bench_mem(double *mbps) {
    static uint32_t buf[8192];
    uint32_t s = 0;
    uint64_t t0 = time_us_64();
    for (int pass = 0; pass < 64; pass++) {
        for (uint32_t i = 0; i < 8192; i++) buf[i] = i * 2654435761u + pass;
        for (uint32_t i = 0; i < 8192; i++) s ^= buf[i];
    }
    *mbps = (double)(2 * 64 * 8192 * 4) / ((time_us_64() - t0) * 1e-6) / 1e6;
    return s;
}

static uint32_t bench_xip(double *mbps) {
    const volatile uint32_t *p = (const volatile uint32_t *)(XIP_BASE + 0x1000);
    const uint32_t words = 8192;
    uint32_t s = 0;
    uint64_t t0 = time_us_64();
    for (int pass = 0; pass < 32; pass++)
        for (uint32_t i = 0; i < words; i++) s ^= p[i];
    *mbps = (double)(32 * words * 4) / ((time_us_64() - t0) * 1e-6) / 1e6;
    return s;
}

typedef struct {
    uint32_t alu, muld, f32, f64, mem, xip;
    double alu_s, muld_s, f32_s, f64_s, mem_mbps, xip_mbps;
} result_t;

static result_t run_all(void) {
    result_t r;
    r.alu  = bench_alu(N_ALU, &r.alu_s);
    r.muld = bench_muldiv(N_MULD, &r.muld_s);
    r.f32  = bench_f32(N_F32, &r.f32_s);
    r.f64  = bench_f64(N_F64, &r.f64_s);
    sink32 = r.alu ^ r.muld;
    r.mem  = bench_mem(&r.mem_mbps);
    r.xip  = bench_xip(&r.xip_mbps);
    return r;
}

static bool same(const result_t *a, const result_t *b) {
    return a->alu == b->alu && a->muld == b->muld && a->f32 == b->f32 &&
           a->f64 == b->f64 && a->mem == b->mem && a->xip == b->xip;
}

static bool apply_point(const step_t *s) {
    if (s->unlock) {
        // >1.30 V needs the clamp off: password + UNLOCK, then the limit bit.
        hw_set_bits(&powman_hw->vreg_ctrl, POWMAN_PASSWORD_BITS | POWMAN_VREG_CTRL_UNLOCK_BITS);
        hw_set_bits(&powman_hw->vreg_ctrl, POWMAN_PASSWORD_BITS | POWMAN_VREG_CTRL_DISABLE_VOLTAGE_LIMIT_BITS);
        while (powman_hw->vreg & POWMAN_VREG_UPDATE_IN_PROGRESS_BITS) tight_loop_contents();
    }
    vreg_set_voltage(s->mv);
    sleep_ms(10);
    set_flash_div(even_div_for(s->mhz * 1000u));
    bool ok = set_sys_clock_khz(s->mhz * 1000u, false);
    sleep_ms(5);
    return ok;
}

int main(void) {
    stdio_init_all();
    adc_init();
    adc_set_temp_sensor_enabled(true);
    for (int i = 0; i < 100 && !stdio_usb_connected(); i++) sleep_ms(100);
    sleep_ms(200);

    uint32_t prev = S_STEP;
    bool wd = watchdog_caused_reboot();
    uint32_t done = S_DONE, pass = S_PASS, fail = S_FAIL;
    int hung = -1;
    if (!wd) { done = pass = fail = 0; }
    if ((prev & 0xFF000000u) == MARK_STEP) {
        hung = prev & 0x3F;
        if (hung < 32) fail |= 1u << hung;
        done |= 1u << hung;
    }

    const uint32_t nsteps = sizeof(LADDER) / sizeof(LADDER[0]);
    printf("\n[sweep] %s done=0x%03lx pass=0x%03lx fail=0x%03lx T=%.1fC\n",
           wd ? "resume after watchdog reset" : "cold boot",
           (unsigned long)done, (unsigned long)pass, (unsigned long)fail, die_temp_c());
    if (hung >= 0)
        printf("[sweep] step %d (%lu MHz @ %.2f V): HANG\n", hung,
               (unsigned long)LADDER[hung].mhz, (double)mv_of(LADDER[hung].mv));

    result_t golden = {0};

    for (uint32_t i = 0; i < nsteps; i++) {
        // step 0 is always re-run: it produces the golden checksums
        if (i != 0 && (done & (1u << i))) continue;

        const step_t *s = &LADDER[i];
        S_STEP = MARK_STEP | i;
        printf("[apply] step %lu: target %lu MHz @ %.2f V unlock=%d (limit_bit=%lu)\n",
               (unsigned long)i, (unsigned long)s->mhz, (double)mv_of(s->mv), (int)s->unlock,
               (unsigned long)((powman_hw->vreg_ctrl & POWMAN_VREG_CTRL_DISABLE_VOLTAGE_LIMIT_BITS) ? 1 : 0));
        watchdog_enable(8000, false);
        bool ok_clk = apply_point(s);

        uint32_t real_mhz = clock_get_hz(clk_sys) / 1000000u;
        uint32_t div = even_div_for(s->mhz * 1000u);
        float t0c = die_temp_c();
        result_t r = run_all();
        float t1c = die_temp_c();
        bool ok_chk = (i == 0) ? true : same(&r, &golden);
        if (i == 0) golden = r;

        printf("F=%-4lu V=%.2f div=%lu sck=%.1f T=%.1f->%.1fC | alu %.0fM muldiv %.0fM fp32 %.0fM fp64 %.2fM | mem %.0f xip %.0f | %s %s %s\n",
               (unsigned long)real_mhz,
               (double)mv_of(s->mv),
               (unsigned long)div, (double)real_mhz / div, t0c, t1c,
               r.alu_s / 1e6, r.muld_s / 1e6, r.f32_s / 1e6, r.f64_s / 1e6,
               r.mem_mbps, r.xip_mbps, s->label,
               (ok_clk && real_mhz == s->mhz) ? "CLK-OK" : "CLK-BAD",
               ok_chk ? "PASS" : "CHECKSUM-FAIL");

        done |= 1u << i;
        if (ok_chk && ok_clk && real_mhz == s->mhz) pass |= 1u << i;
        else fail |= 1u << i;
        S_DONE = done; S_PASS = pass; S_FAIL = fail;
        S_STEP = MARK_END;
        watchdog_disable();
    }

    // ---- summary + soak at the highest passing point ----
    int best = -1;
    for (uint32_t i = 0; i < nsteps; i++) if (pass & (1u << i)) best = (int)i;
    printf("[sweep] DONE pass=0x%03lx fail=0x%03lx -> best=%d (%lu MHz)\n",
           (unsigned long)pass, (unsigned long)fail, best,
           best >= 0 ? (unsigned long)LADDER[best].mhz : 0UL);

    if (best >= 0) {
        apply_point(&LADDER[best]);
        watchdog_enable(8000, false);
        uint32_t errors = 0, iters = 0;
        uint64_t t0 = time_us_64();
        uint64_t next = 0;
        while ((time_us_64() - t0) < (uint64_t)SOAK_S * 1000000ull) {
            result_t r = run_all();
            iters++;
            if (!same(&r, &golden)) errors++;
            watchdog_update();
            uint64_t el = (time_us_64() - t0) / 1000000ull;
            if (el >= next) {
                printf("[soak] %2llus %lu MHz T=%.1fC iters=%lu errors=%lu\n",
                       (unsigned long long)el, (unsigned long)clock_get_hz(clk_sys) / 1000000u,
                       die_temp_c(), (unsigned long)iters, (unsigned long)errors);
                next = el + 5;
            }
        }
        printf("[soak] DONE %lus %lu MHz T=%.1fC iters=%lu errors=%lu -> %s\n",
               (unsigned long)SOAK_S, (unsigned long)clock_get_hz(clk_sys) / 1000000u,
               die_temp_c(), (unsigned long)iters, (unsigned long)errors,
               errors ? "UNSTABLE" : "STABLE");
        watchdog_disable();
    }

    while (true) {
        printf("[alive] %lu MHz T=%.1fC\n", (unsigned long)clock_get_hz(clk_sys) / 1000000u, die_temp_c());
        sleep_ms(3000);
    }
}
