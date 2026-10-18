// Render-shaped microbenchmark: the same rasterizer written twice, once in
// fixed point and once in float, plus a per-pixel lighting kernel.
//
// Purpose: RP2350 can run either a Cortex-M33 (hardware single-precision FPU +
// DCP for double) or a Hazard3 (RV32IMAC+Zb*, no F/D, so every float op is a
// soft-float call). Renderers care about exactly this difference, so measure it
// on the real silicon instead of arguing from ISA names.
//
// Every kernel is deterministic: the checksum must be identical across ISAs,
// otherwise the number is worthless.
#include <stdio.h>
#include <math.h>
#include "pico/stdlib.h"
#include "oc.h"

#ifndef MAX_MHZ
#define MAX_MHZ 150
#endif
#ifndef VOLT_MV
#define VOLT_MV 1100
#endif

#define W 256
#define H 256
#define NPIX (W * H)

static uint16_t fb[NPIX];
static volatile uint32_t sink;

// ---- fixed point: 16.16 texture coords, 12.4 z ------------------------------
static uint32_t __not_in_flash_func(raster_fixed)(int32_t u0, int32_t v0, int32_t du, int32_t dv)
{
    uint32_t acc = 0;
    for (int y = 0; y < H; y++) {
        int32_t v = v0 + (y * dv);
        int32_t u = u0;
        for (int x = 0; x < W; x++) {
            int32_t z = 4096 + ((u >> 16) & 0xFF) + ((v >> 16) & 0xFF);
            uint16_t c = (uint16_t)(((u >> 8) & 0xF800) | ((v >> 13) & 0x07E0) | ((z >> 3) & 0x001F));
            fb[y * W + x] = c;
            acc += c;
            u += du;
        }
    }
    return acc;
}

// ---- straight float version of the same loop --------------------------------
static uint32_t __not_in_flash_func(raster_float)(float u0, float v0, float du, float dv)
{
    uint32_t acc = 0;
    for (int y = 0; y < H; y++) {
        float v = v0 + (float)y * dv;
        float u = u0;
        for (int x = 0; x < W; x++) {
            float z = 4096.0f + (u * 0.001f) + (v * 0.001f);
            uint16_t c = (uint16_t)(((int)(u * 8.0f) & 0xF800) | ((int)(v * 8.0f) & 0x07E0) | ((int)z & 0x001F));
            fb[y * W + x] = c;
            acc += c;
            u += du;
        }
    }
    return acc;
}

// ---- lighting: normalize a 3-vector per pixel (sqrt + 3 divides) ------------
static uint32_t __not_in_flash_func(lighting)(int n)
{
    uint32_t acc = 0;
    for (int i = 0; i < n; i++) {
        float x = (float)(i & 0xFF) + 1.0f;
        float y = (float)((i >> 8) & 0xFF) + 1.0f;
        float z = (float)((i >> 16) & 0xFF) + 1.0f;
        float inv = 1.0f / sqrtf(x * x + y * y + z * z);
        acc += (uint32_t)(x * inv * 65535.0f) ^ (uint32_t)(y * inv * 65535.0f);
    }
    return acc;
}

static double now_s(void) { return (double)time_us_64() * 1e-6; }

int main(void)
{
    stdio_init_all();
    for (int i = 0; i < 100 && !stdio_usb_connected(); i++) sleep_ms(100);
    sleep_ms(200);

    oc_apply(MAX_MHZ, VOLT_MV);

    printf("\n[rd] core="
#ifdef __riscv
           "hazard3-rv32"
#else
           "cortex-m33"
#endif
           " clock=%luMHz vreg=%dmV gcc=%s T=%.1fC\n",
           (unsigned long)oc_mhz(), VOLT_MV, __VERSION__, oc_temp_c());

    // warm the caches once so the first measured pass is not an outlier
    sink = raster_fixed(0, 0, 1 << 12, 1 << 12);

    const int PASSES = 8;

    double t0 = now_s();
    uint32_t cf = 0;
    for (int p = 0; p < PASSES; p++)
        cf += raster_fixed(0x00100000 + p * 0x00007000, 0x00200000 - p * 0x00003000,
                           0x00001300 + p * 0x40, 0x00009100 - p * 0x80);
    double tf = now_s() - t0;
    sink ^= cf;
    printf("[rd] fixed passes=%d csum=0x%08lx\n", PASSES, (unsigned long)cf);

    t0 = now_s();
    uint32_t cg = 0;
    for (int p = 0; p < PASSES; p++)
        cg += raster_float(16.0f + (float)p * 0.37f, 32.0f - (float)p * 0.11f,
                           0.19f + (float)p * 0.001f, 1.41f - (float)p * 0.002f);
    double tg = now_s() - t0;
    sink ^= cg;
    printf("[rd] float passes=%d csum=0x%08lx\n", PASSES, (unsigned long)cg);

    const int NL = 300000;
    t0 = now_s();
    uint32_t cl = lighting(NL);
    double tl = now_s() - t0;
    sink ^= cl;

    printf("[rd] fixed  csum=0x%08lx  %.1f Mpix/s\n", (unsigned long)cf, (double)NPIX * PASSES / tf / 1e6);
    printf("[rd] float  csum=0x%08lx  %.1f Mpix/s\n", (unsigned long)cg, (double)NPIX * PASSES / tg / 1e6);
    printf("[rd] light  csum=0x%08lx  %.1f Mnorm/s\n", (unsigned long)cl, (double)NL / tl / 1e6);
    printf("[rd] ratio fixed/float = %.2fx   light/fixed = %.3f\n",
           (double)NPIX * PASSES / tf / ((double)NPIX * PASSES / tg),
           (double)NL / tl / ((double)NPIX * PASSES / tf));
    printf("[rd] DONE T=%.1fC\n", oc_temp_c());

    while (true) {
        sleep_ms(3000);
        printf("[rd] idle %lu MHz T=%.1fC\n", (unsigned long)oc_mhz(), oc_temp_c());
    }
}
