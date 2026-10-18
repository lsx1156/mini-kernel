// PIO throughput benchmark for RP2350.
//
// TX: N state machines running "out pins, 32" in a 1-instruction loop, fed by DMA.
//     src=rep  : DMA reads one word repeatedly   -> pure PIO + DMA path
//     src=sram : DMA streams a 64 KB SRAM buffer -> realistic source bandwidth
// RX: N state machines running "in pins, 1" (one sample per cycle), drained by DMA.
//
// Hand-encoded instructions (no pioasm dependency):
//   out pins, 32 -> 0x6000    (count 32 is encoded as 0)
//   in  pins, 1  -> 0x4001

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "oc.h"

#ifndef MAX_MHZ
#define MAX_MHZ 150
#endif

#define MAX_SM          12
#define TX_PER_XFER     (1u << 18)
#define TX_SRAM_XFER    (16u * 1024)
#define RX_PER_XFER     4096u
#define RX_REPEATS      64u
#define REP_REPEATS     4u

static const uint16_t tx_insn = 0x6000;   // out pins, 32
static const uint16_t rx_insn = 0x4001;   // in pins, 1

static uint32_t sram_src[16u * 1024];
static uint32_t rx_dst[MAX_SM][4096];


static PIO pio_of(uint b) { return b == 0 ? pio0 : (b == 1 ? pio1 : pio2); }

static bool dma_wait_ms(uint ch, uint ms) {
    absolute_time_t deadline = make_timeout_time_ms(ms);
    while (dma_channel_is_busy(ch)) {
        if (time_reached(deadline)) return false;
        tight_loop_contents();
    }
    return true;
}

static void pins_init(uint block, bool out) {
    for (int j = 0; j < 10; j++) {
        uint pin = block * 10 + j;
        gpio_init(pin);
        gpio_set_function(pin, (gpio_function_t)(GPIO_FUNC_PIO0 + block));
        gpio_set_dir(pin, out ? GPIO_OUT : GPIO_IN);
    }
}

static double bench_tx(uint nsm, bool sram) {
    uint words = sram ? TX_SRAM_XFER : TX_PER_XFER;
    uint reps  = sram ? 256u : REP_REPEATS;
    uint nblk  = (nsm + 3) / 4;

    pio_program_t prog;
    prog.instructions = &tx_insn;
    prog.length = 1;
    prog.origin = -1;
    prog.pio_version = 1;
    prog.used_gpio_ranges = 0x0;

    uint off[3] = {0, 0, 0};
    for (uint b = 0; b < nblk; b++) {
        pins_init(b, true);
        off[b] = pio_add_program(pio_of(b), &prog);
    }

    int ch[MAX_SM];
    for (uint i = 0; i < nsm; i++) {
        PIO pio = pio_of(i / 4);
        uint sm = i % 4;

        pio_sm_config c = pio_get_default_sm_config();
        sm_config_set_wrap(&c, off[i / 4], off[i / 4]);
        sm_config_set_out_pins(&c, (i / 4) * 10, 32);
        sm_config_set_out_shift(&c, true, true, 32);
        sm_config_set_clkdiv(&c, 1.0f);
        pio_sm_init(pio, sm, off[i / 4], &c);

        ch[i] = dma_claim_unused_channel(true);
        dma_channel_config dc = dma_channel_get_default_config((uint)ch[i]);
        channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
        channel_config_set_dreq(&dc, pio_get_dreq(pio, sm, true));
        channel_config_set_read_increment(&dc, sram);
        channel_config_set_write_increment(&dc, false);
        dma_channel_configure((uint)ch[i], &dc, &pio->txf[sm], sram_src, words, false);

        pio_sm_set_enabled(pio, sm, true);
    }

    uint64_t t0 = time_us_64();
    for (uint r = 0; r < reps; r++) {
        for (uint i = 0; i < nsm; i++) {
            dma_channel_set_read_addr((uint)ch[i], sram_src, false);
            dma_channel_set_trans_count((uint)ch[i], words, true);
        }
        for (uint i = 0; i < nsm; i++) dma_wait_ms((uint)ch[i], 500);
    }
    uint64_t us = time_us_64() - t0;

    for (uint i = 0; i < nsm; i++) {
        PIO pio = pio_of(i / 4);
        uint sm = i % 4;
        pio_sm_set_enabled(pio, sm, false);
        dma_channel_unclaim((uint)ch[i]);
    }
    for (uint b = 0; b < nblk; b++) pio_remove_program(pio_of(b), &prog, off[b]);

    return (double)words * reps * nsm / ((double)us * 1e-6);
}

static double bench_rx(uint nsm) {
    uint nblk = (nsm + 3) / 4;

    pio_program_t prog;
    prog.instructions = &rx_insn;
    prog.length = 1;
    prog.origin = -1;
    prog.pio_version = 1;
    prog.used_gpio_ranges = 0x0;

    uint off[3] = {0, 0, 0};
    for (uint b = 0; b < nblk; b++) {
        pins_init(b, false);
        off[b] = pio_add_program(pio_of(b), &prog);
    }

    int ch[MAX_SM];
    for (uint i = 0; i < nsm; i++) {
        PIO pio = pio_of(i / 4);
        uint sm = i % 4;

        pio_sm_config c = pio_get_default_sm_config();
        sm_config_set_wrap(&c, off[i / 4], off[i / 4]);
        sm_config_set_in_pins(&c, (i / 4) * 10);
        sm_config_set_in_shift(&c, true, true, 32);
        sm_config_set_clkdiv(&c, 1.0f);
        pio_sm_init(pio, sm, off[i / 4], &c);

        ch[i] = dma_claim_unused_channel(true);
        dma_channel_config dc = dma_channel_get_default_config((uint)ch[i]);
        channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
        channel_config_set_dreq(&dc, pio_get_dreq(pio, sm, false));
        channel_config_set_read_increment(&dc, false);
        channel_config_set_write_increment(&dc, true);
        dma_channel_configure((uint)ch[i], &dc, rx_dst[i], &pio->rxf[sm],
                              RX_PER_XFER, false);

        pio_sm_set_enabled(pio, sm, true);
    }

    uint64_t t0 = time_us_64();
    for (uint r = 0; r < RX_REPEATS; r++) {
        for (uint i = 0; i < nsm; i++) {
            dma_channel_set_write_addr((uint)ch[i], rx_dst[i], false);
            dma_channel_set_trans_count((uint)ch[i], RX_PER_XFER, true);
        }
        for (uint i = 0; i < nsm; i++) dma_wait_ms((uint)ch[i], 500);
    }
    uint64_t us = time_us_64() - t0;

    for (uint i = 0; i < nsm; i++) {
        PIO pio = pio_of(i / 4);
        uint sm = i % 4;
        pio_sm_set_enabled(pio, sm, false);
        dma_channel_unclaim((uint)ch[i]);
    }
    for (uint b = 0; b < nblk; b++) pio_remove_program(pio_of(b), &prog, off[b]);

    return (double)RX_PER_XFER * 32.0 * RX_REPEATS * nsm / ((double)us * 1e-6);
}

int main(void) {
#if defined(MAX_MHZ) && (MAX_MHZ != 150)
    oc_apply(MAX_MHZ, VOLT_MV);
#endif
    stdio_init_all();

    for (uint32_t i = 0; i < (sizeof sram_src / sizeof sram_src[0]); i++)
        sram_src[i] = 0xA5A5A5A5u ^ (i * 2654435761u);

    sleep_ms(2000);
    printf("[pb] temp=%.1f C\n", oc_temp_c());
    uint32_t khz = clock_get_hz(clk_sys) / 1000;
    printf("\n[pb] clk_sys=%lu kHz\n", (unsigned long)khz);

    printf("[pb] start tx1rep\n"); stdio_flush();
    double r = bench_tx(1, false);
    printf("[pb] TX sm=1  src=rep    %8.2f Mword/s  clk/word=%.3f\n", r / 1e6, khz * 1e3 / r);
    printf("[pb] start tx1sram\n"); stdio_flush();
    r = bench_tx(1, true);
    printf("[pb] TX sm=1  src=sram   %8.2f Mword/s  clk/word=%.3f\n", r / 1e6, khz * 1e3 / r);
    printf("[pb] start tx4\n"); stdio_flush();
    r = bench_tx(4, false);
    printf("[pb] TX sm=4  src=rep    %8.2f Mword/s  per-sm=%.2f  clk/word=%.3f\n",
           r / 1e6, r / 4e6, khz * 1e3 * 4 / r);
    printf("[pb] start tx8\n"); stdio_flush();
    r = bench_tx(8, false);
    printf("[pb] TX sm=8  src=rep    %8.2f Mword/s  per-sm=%.2f  clk/word=%.3f\n",
           r / 1e6, r / 8e6, khz * 1e3 * 8 / r);
    printf("[pb] start tx12\n"); stdio_flush();
    r = bench_tx(12, false);
    printf("[pb] TX sm=12 src=rep    %8.2f Mword/s  per-sm=%.2f  clk/word=%.3f\n",
           r / 1e6, r / 12e6, khz * 1e3 * 12 / r);

    printf("[pb] start rx1\n"); stdio_flush();
    double b = bench_rx(1);
    printf("[pb] RX sm=1             %8.2f Mbit/s   %8.2f Msample/s  clk/sample=%.3f\n",
           b / 1e6, b / 1e6, khz * 1e3 / b);
    printf("[pb] start rx4\n"); stdio_flush();
    b = bench_rx(4);
    printf("[pb] RX sm=4             %8.2f Mbit/s   %8.2f Msample/s  clk/sample=%.3f\n",
           b / 1e6, b / 1e6, khz * 1e3 * 4 / b);
    printf("[pb] start rx12\n"); stdio_flush();
    b = bench_rx(12);
    printf("[pb] RX sm=12            %8.2f Mbit/s   %8.2f Msample/s  clk/sample=%.3f\n",
           b / 1e6, b / 1e6, khz * 1e3 * 12 / b);

    printf("[pb] done\n");
    while (true) tight_loop_contents();
}
