// 12 通道协议嗅探器 v1.1
// 采样内核：每通道一个 PIO SM 定速采样 → DMA → SRAM；主机通过 USB 命令抓取与上传。
// 命令（行结束 \n）：
//   R <ch> <hz>     设置通道采样率
//   C <us> <mode>   抓取 us 微秒；mode 0=纯抓取 1=同时发 WS2812 2=同时发 SPI
//   D <ch>          二进制上传该通道：4 字节小端字数 + 原始 32 位字（最早采样在 bit0）
//   V               版本与状态
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "tusb.h"
#include "pico/stdio_usb.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "sampler.pio.h"
#include "ws2812.pio.h"
#include "spi_tx.pio.h"
#include "oc.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "hardware/vreg.h"
#include "hardware/adc.h"
#include "pico/bootrom.h"

#define NCH        12
#define BUF_WORDS  4096
#define SAMPLES    (BUF_WORDS * 32)

static uint32_t rate_hz[NCH] = {
    1000000, 1000000, 2000000, 2000000,
    10000000, 10000000, 10000000, 10000000,
    1000000, 1000000, 1000000, 1000000,
};

// 环形 DMA 的窗口必须与环大小（16 KB）对齐，所以整块按 16 KB 对齐；每通道恰好 16 KB
static uint32_t buf[NCH][BUF_WORDS] __attribute__((aligned(16384)));
static uint32_t got[NCH];
static int      dma_ch[NCH];
static PIO      blk[3] = { pio0, pio1, pio2 };
static uint     prg_off[3], ws_off, spi_off;
static uint     boot_mv;
static uint32_t last_pc;   // 诊断：生成器停止时的 PC

static inline PIO  pio_of(int i) { return blk[i / 4]; }
static inline uint sm_of (int i) { return (uint)(i & 3); }

static void ch_config(int i, uint32_t hz) {
    PIO  pio = pio_of(i);
    uint sm  = sm_of(i);
    float div = (float)clock_get_hz(clk_sys) / (float)hz;
    if (div < 1.0f) div = 1.0f;                     // 采样率上限 = clk_sys
    pio_sm_config c = sampler_program_get_default_config(prg_off[i / 4]);
    sm_config_set_in_pins(&c, (uint)i);
    sm_config_set_in_shift(&c, true, true, 32);     // 右移：最早采样进 bit0
    sm_config_set_clkdiv(&c, div);
    pio_sm_init(pio, sm, prg_off[i / 4], &c);
    pio_sm_set_enabled(pio, sm, false);
    rate_hz[i] = (uint32_t)((float)clock_get_hz(clk_sys) / div);
}

static void dma_arm(int i) {
    PIO  pio = pio_of(i);
    uint sm  = sm_of(i);
    dma_channel_config dc = dma_channel_get_default_config((uint)dma_ch[i]);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_dreq(&dc, pio_get_dreq(pio, sm, false));
    dma_channel_configure((uint)dma_ch[i], &dc, buf[i], &pio->rxf[sm], BUF_WORDS, false);
}

// ---- 生成器：借用 pio0 SM0（CH0），生成期间 CH0 不采样 ----
static void ws2812_send(void) {
    // 先把引脚拉低（帧间空闲电平），否则上电时上拉造成的高电平会并进第一个 T0H
    gpio_set_function(11, GPIO_FUNC_SIO);
    gpio_put(11, 0);
    gpio_set_dir(11, GPIO_OUT);
    sleep_us(60);

    pio_sm_set_enabled(pio0, 0, false);
    pio_sm_config c = ws2812_program_get_default_config(ws_off);
    sm_config_set_out_pins(&c, 11, 1);
    sm_config_set_set_pins(&c, 11, 1);
    sm_config_set_out_shift(&c, false, false, 32);   // 左移 → MSB 先出
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / 40000000.0f);  // 40 MHz
    pio_sm_init(pio0, 0, ws_off, &c);
    pio_sm_set_pins_with_mask(pio0, 0, 0, 1u << 11);   // 输出锁存先归零，切换功能时不会跳高
    pio_gpio_init(pio0, 11);
    pio_sm_set_consecutive_pindirs(pio0, 0, 11, 1, true);
    pio_sm_set_enabled(pio0, 0, true);

    // GRB：灯0 = R 0x12 G 0x34 B 0x56，灯1 = R 0xAB G 0xCD B 0xEF
    uint32_t cols[2] = { (0x34u << 16) | (0x12u << 8) | 0x56u,
                         (0xCDu << 16) | (0xABu << 8) | 0xEFu };
    for (int i = 0; i < 2; i++) {
        while (pio_sm_is_tx_fifo_full(pio0, 0)) tight_loop_contents();
        pio_sm_put(pio0, 0, cols[i] << 8);
    }
    // 2 灯 = 48 比特 × 51 拍 / 40 MHz = 61.2 µs；75 µs 后关 SM，避免它继续空转出多余的零比特
    sleep_us(75);
    last_pc = pio_sm_get_pc(pio0, 0);
    pio_sm_set_enabled(pio0, 0, false);
}

static void spi_send(void) {
    gpio_init(9); gpio_set_dir(9, GPIO_OUT); gpio_put(9, 1);      // CS 先保持高

    // 先把 SCK/MOSI 拉低，避免切换功能时的跳变被当成时钟沿
    gpio_set_function(10, GPIO_FUNC_SIO); gpio_put(10, 0); gpio_set_dir(10, GPIO_OUT);
    gpio_set_function(11, GPIO_FUNC_SIO); gpio_put(11, 0); gpio_set_dir(11, GPIO_OUT);
    sleep_us(20);

    pio_sm_set_enabled(pio0, 0, false);
    pio_sm_config c = spi_tx_program_get_default_config(spi_off);
    sm_config_set_out_pins(&c, 11, 1);                            // MOSI
    sm_config_set_set_pins(&c, 10, 1);                            // SCK
    sm_config_set_out_shift(&c, false, false, 32);                // MSB 先出
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / 10000000.0f);  // 10 MHz
    pio_sm_init(pio0, 0, spi_off, &c);
    pio_sm_set_pins_with_mask(pio0, 0, 0, (1u << 10) | (1u << 11)); // 锁存归零后再切功能
    pio_gpio_init(pio0, 10);
    pio_gpio_init(pio0, 11);
    pio_sm_set_consecutive_pindirs(pio0, 0, 10, 2, true);
    pio_sm_set_enabled(pio0, 0, true);

    gpio_put(9, 0);                                               // CS 下降沿，帧开始
    uint8_t bytes[2] = { 0xA5, 0x3C };
    for (int i = 0; i < 2; i++) {
        while (pio_sm_is_tx_fifo_full(pio0, 0)) tight_loop_contents();
        pio_sm_put(pio0, 0, (uint32_t)bytes[i] << 24);
    }
    sleep_us(150);
    gpio_put(9, 1);
    pio_sm_set_enabled(pio0, 0, false);
}

static void gen_release(void) {
    gpio_set_function(9, GPIO_FUNC_SIO);  gpio_init(9);  gpio_set_dir(9, GPIO_IN);  gpio_pull_down(9);
    gpio_set_function(10, GPIO_FUNC_SIO); gpio_init(10); gpio_set_dir(10, GPIO_IN); gpio_pull_down(10);
    gpio_set_function(11, GPIO_FUNC_SIO); gpio_init(11); gpio_set_dir(11, GPIO_IN); gpio_pull_down(11);
    ch_config(0, rate_hz[0]);
}


// pioasm 生成的 jmp 是绝对地址，SDK 的 pio_add_program() 不重定位。
// 这里自己搬：把 jmp 的地址字段加上起始偏移，再放到指定位置。
static uint add_reloc(PIO pio, const struct pio_program *prg, int *cursor) {
    static uint16_t tmp[32];
    uint off = (uint)*cursor, len = prg->length;
    for (uint i = 0; i < len; i++) {
        uint16_t ins = prg->instructions[i];
        if ((ins & 0xE000) == 0x0000) {                 // JMP
            uint addr = (ins >> 8) & 0x1Fu;
            ins = (uint16_t)((ins & ~0x1F00u) | ((addr + off) & 0x1Fu) << 8);
        }
        tmp[i] = ins;
    }
    pio_program_t p = { .instructions = tmp, .length = (uint8_t)len, .origin = -1 };
    uint got = pio_add_program_at_offset(pio, &p, off);
    *cursor += (int)len;
    return got;
}

// ---- I2C 自测：CPU 位翻转发 START + 0xA0/0x12/0x34 + STOP（约 100 kHz）----
// 只用于验证嗅探器与解码器：推挽驱动，不模拟开漏/时钟拉伸。
static void i2c_send(void) {
    gpio_set_function(10, GPIO_FUNC_SIO); gpio_init(10); gpio_set_dir(10, GPIO_OUT);
    gpio_set_function(11, GPIO_FUNC_SIO); gpio_init(11); gpio_set_dir(11, GPIO_OUT);
    gpio_put(10, 1); gpio_put(11, 1);
    sleep_us(20);

    gpio_put(11, 0); sleep_us(5);        // START：SCL 高时 SDA 下降
    gpio_put(10, 0); sleep_us(5);

    uint8_t bytes[3] = { 0xA0, 0x12, 0x34 };
    for (int k = 0; k < 3; k++) {
        for (int b = 7; b >= 0; b--) {
            gpio_put(11, (bytes[k] >> b) & 1);
            sleep_us(2);
            gpio_put(10, 1); sleep_us(5);   // SCL 高：从机采样
            gpio_put(10, 0); sleep_us(2);
        }
        gpio_put(11, 0);  sleep_us(2);      // 第 9 拍：拉低 SDA 模拟从机 ACK
        gpio_put(10, 1);  sleep_us(5);
        gpio_put(10, 0);  sleep_us(2);
    }

    gpio_put(11, 0); sleep_us(2);           // STOP：SCL 高时 SDA 上升
    gpio_put(10, 1); sleep_us(5);
    gpio_put(11, 1); sleep_us(5);
}

static void capture(uint32_t us, int mode) {
    for (int i = 0; i < NCH; i++) {
        pio_sm_clear_fifos(pio_of(i), sm_of(i));
        pio_sm_restart(pio_of(i), sm_of(i));
        dma_channel_abort((uint)dma_ch[i]);
        dma_arm(i);
        pio_sm_set_enabled(pio_of(i), sm_of(i), true);
        dma_channel_start((uint)dma_ch[i]);
        got[i] = 0;
    }
    if (mode) {
        sleep_us(100);
        if (mode == 1) ws2812_send();
        else if (mode == 2) spi_send();
        else if (mode == 3) i2c_send();
    }
    // 长窗口要持续喂 USB：CPU 忙等时 tud_task() 不跑，主机的控制传输（开端口、SET_LINE_CODING）
    // 就得不到应答，Windows 会报 semaphore timeout 并把端口卡死。1 ms 一次，短窗口仍精确睡。
    uint64_t t_stop = time_us_64() + (uint64_t)us;
    for (;;) {
        uint64_t now = time_us_64();
        if (now >= t_stop) break;
        uint64_t rem = t_stop - now;
        tud_task();
        if (rem > 1000ull) sleep_us(1000); else { sleep_us((uint32_t)rem); break; }
    }
    for (int i = 0; i < NCH; i++) {
        pio_sm_set_enabled(pio_of(i), sm_of(i), false);
    }
    sleep_us(2);    // 等 DREQ 落下再 abort，否则可能卡在 SDK 的 BUSY 等待里
    for (int i = 0; i < NCH; i++) {
        uint32_t rem = dma_channel_hw_addr((uint)dma_ch[i])->transfer_count;
        dma_channel_abort((uint)dma_ch[i]);
        got[i] = (BUF_WORDS - rem) * 32u;
    }
    if (mode) gen_release();
}


// 环形装载：地址在 16 KB 窗口内回绕，传输计数给到最大，一直跑
static void dma_arm_ring(int i) {
    PIO  pio = pio_of(i);
    uint sm  = sm_of(i);
    dma_channel_config dc = dma_channel_get_default_config((uint)dma_ch[i]);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_ring(&dc, true, 14);                 // 2^14 = 16 KB
    channel_config_set_dreq(&dc, pio_get_dreq(pio, sm, false));
    dma_channel_configure((uint)dma_ch[i], &dc, buf[i], &pio->rxf[sm], 0xFFFFFFFFu, false);
}

static uint32_t wr_off(int i) {
    uint32_t w = dma_channel_hw_addr((uint)dma_ch[i])->write_addr;
    return (uint32_t)((w - (uint32_t)(uintptr_t)buf[i]) / 4u) % BUF_WORDS;
}

// T <pre_us> <post_us> <ch> <edge>：环形预录 → 等边沿 → 再录 post_us → 停
// 预录容量 = 缓冲长度（10 MS/s 时约 13 ms），由采样率决定
static void core1_ws_loop(void) {
    for (;;) { ws2812_send(); sleep_us(300); }
}

static void trigger_capture(uint32_t post_us, int tch, int edge, int mode) {
    for (int i = 0; i < NCH; i++) {
        pio_sm_clear_fifos(pio_of(i), sm_of(i));
        pio_sm_restart(pio_of(i), sm_of(i));
        dma_channel_abort((uint)dma_ch[i]);
        dma_arm_ring(i);
        pio_sm_set_enabled(pio_of(i), sm_of(i), true);
        dma_channel_start((uint)dma_ch[i]);
    }
    uint32_t t0 = time_us_32();
    if (mode == 4) multicore_launch_core1(core1_ws_loop);   // 另一核持续发，CPU 才能轮询
    int prev = gpio_get((uint)tch);
    bool fired = false;
    while (time_us_32() - t0 < 2000000u) {                  // 2 s 超时
        tud_task();
        int v = gpio_get((uint)tch);
        if (v != prev) {
            if ((edge && v) || (!edge && !v)) { fired = true; break; }
            prev = v;
        }
    }
    uint32_t waited = time_us_32() - t0;
    sleep_us(post_us);
    for (int i = 0; i < NCH; i++) {
        pio_sm_set_enabled(pio_of(i), sm_of(i), false);
        got[i] = BUF_WORDS * 32u;
    }
    printf("OK %d %lu", fired ? 1 : 0, (unsigned long)waited);
    for (int i = 0; i < NCH; i++) {
        uint32_t o = wr_off(i);
        dma_channel_abort((uint)dma_ch[i]);
        printf(" %lu", (unsigned long)o);
    }
    if (mode == 4) multicore_reset_core1();
    printf("\n");
}

static void dump_ch(int ch) {
    if (!stdio_usb_connected()) return;      // 主机没开端口就别往里灌
    uint32_t words = (got[ch] + 31u) / 32u;
    fwrite(&words, 4, 1, stdout);
    fwrite(buf[ch], 4, words, stdout);
    fflush(stdout);
}

#include "soak.inc"
static void handle(char *s) {
    if (s[0] == 'R') {
        int ch; unsigned long hz;
        if (sscanf(s, "R %d %lu", &ch, &hz) == 2 && ch >= 0 && ch < NCH) {
            ch_config(ch, (uint32_t)hz);
            printf("OK %d %lu\n", ch, (unsigned long)hz);
        } else printf("ERR\n");
    } else if (s[0] == 'C') {
        unsigned long us; int mode = 0;
        if (sscanf(s, "C %lu %d", &us, &mode) >= 1) {
            capture((uint32_t)us, mode);
            printf("OK");
            for (int i = 0; i < NCH; i++) printf(" %lu", (unsigned long)got[i]);
            printf("\n");
        } else printf("ERR\n");
    } else if (s[0] == 'T') {
        unsigned long post; int tch, edge, tmode = 0;
        int nf = sscanf(s, "T %lu %d %d %d", &post, &tch, &edge, &tmode);
        if (nf >= 3 && tch >= 0 && tch < NCH) {
            trigger_capture((uint32_t)post, tch, edge, tmode);
        } else printf("ERR\n");
    } else if (s[0] == 'D') {
        int ch;
        if (sscanf(s, "D %d", &ch) == 1 && ch >= 0 && ch < NCH) dump_ch(ch);
        else printf("ERR\n");
    } else if (s[0] == 'V') {
        printf("sniffer v1.1 clk=%lu mv=%u pc=%lu\n", (unsigned long)clock_get_hz(clk_sys), boot_mv, (unsigned long)last_pc);
    } else if (s[0] == 'X') {
        unsigned long sec, de = 1000;
        if (sscanf(s, "X %lu %lu", &sec, &de) >= 1) soak_run((uint32_t)sec, (uint32_t)de);
        else printf("ERR\n");
    } else if (s[0] == 'B') {
        printf("BOOT\n");
    } else if (s[0]) {
        printf("ERR\n");
    }
}

int main(void) {
    stdio_init_all();
    stdio_set_translate_crlf(&stdio_usb, false);   // 二进制上传：禁止 \n → \r\n
    while (!stdio_usb_connected()) sleep_ms(10);
    sleep_ms(800);
#ifdef MAX_MHZ
    oc_apply(MAX_MHZ, VOLT_MV);
#endif
    adc_init();
    adc_set_temp_sensor_enabled(true);
    boot_mv = (uint)vreg_get_voltage();     // VSEL 原始编码：21=1.70 V, 22=1.80 V, 23=1.90 V

    // pio0：生成器必须放在地址 0 —— pioasm 生成的 jmp 目标是绝对地址，SDK 搬移程序时不会重定位
    static int p0 = 0;
    ws_off  = add_reloc(pio0, &ws2812_program, &p0);
    spi_off = add_reloc(pio0, &spi_tx_program, &p0);
    prg_off[0] = add_reloc(pio0, &sampler_program, &p0);    // 采样器无 jmp，放哪都行
    prg_off[1] = pio_add_program(pio1, &sampler_program);
    prg_off[2] = pio_add_program(pio2, &sampler_program);

    for (int i = 0; i < NCH; i++) {
        gpio_init((uint)i); gpio_set_dir((uint)i, GPIO_IN); gpio_pull_up((uint)i);
        dma_ch[i] = (int)dma_claim_unused_channel(true);
        ch_config(i, rate_hz[i]);
    }
    if ((watchdog_hw->scratch[0] & 0xFFFF0000u) == 0x50AC0000u) {
        printf("LASTSOAK rounds=%lu short=%lu tmax=%.1f tend=%.1f\n",
               (unsigned long)watchdog_hw->scratch[1], (unsigned long)(watchdog_hw->scratch[0] & 0xFFFFu),
               watchdog_hw->scratch[2] / 10.0, watchdog_hw->scratch[3] / 10.0);
        watchdog_hw->scratch[0] = 0;
    }
    printf("READY v1.1\n");

    char line[64]; int li = 0;
    for (;;) {
        watchdog_update();
        int c = getchar_timeout_us(2000);
        if (c == PICO_ERROR_TIMEOUT) continue;
        if (c == '\r') continue;
        if (c == '\n') { line[li] = 0; handle(line); li = 0; }
        else if (li < (int)sizeof(line) - 1) line[li++] = (char)c;
    }
}
