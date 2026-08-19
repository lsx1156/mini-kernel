/**
 * @file    shell_ovclk.c
 * @brief   RP2350 超频/时钟配置 Shell 命令
 */

#include "shell_core.h"
#include "rp2350_port.h"
#include <stdio.h>

static void cmd_ovclk(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) {
        rp2350_clk_config_t cfg;
        rp2350_clk_get_config(&cfg);
        shell_printf(ctx, "Current clocks:\r\n");
        shell_printf(ctx, "  sys_clk:  %lu Hz (%lu MHz)\r\n", cfg.sys_clk_hz, cfg.sys_clk_hz/1000000);
        shell_printf(ctx, "  peri_clk: %lu Hz (%lu MHz)\r\n", cfg.peri_clk_hz, cfg.peri_clk_hz/1000000);
        shell_printf(ctx, "  usb_clk:  %lu Hz\r\n", cfg.usb_clk_hz);
        shell_printf(ctx, "  adc_clk:  %lu Hz\r\n", cfg.adc_clk_hz);
        shell_puts(ctx, "\r\nUsage: ovclk <list|set|try|get|save|unsave>\r\n");
        shell_puts(ctx, "  list  - show available presets\r\n");
        shell_puts(ctx, "  set   - set frequency (48|100|125|133|150)\r\n");
        shell_puts(ctx, "  try   - try frequency temporarily\r\n");
        shell_puts(ctx, "  get   - show current\r\n");
        shell_puts(ctx, "  save  - persist to flash\r\n");
        shell_puts(ctx, "  unsave - clear persisted\r\n");
        return;
    }
    
    if (strcmp(argv[1], "list") == 0) {
        shell_puts(ctx, "Available presets:\r\n");
        shell_puts(ctx, "  48   - USB boot default\r\n");
        shell_puts(ctx, "  100  - Stable\r\n");
        shell_puts(ctx, "  125  - RP2040 compatible\r\n");
        shell_puts(ctx, "  133  - RP2040 max\r\n");
        shell_puts(ctx, "  150  - RP2350 max\r\n");
    } else if (strcmp(argv[1], "set") == 0) {
        if (argc < 3) { shell_puts(ctx, "usage: ovclk set <mhz>\r\n"); return; }
        uint32_t mhz = atoi(argv[2]);
        rp2350_clk_freq_t freq = mhz * 1000000;
        if (rp2350_clk_configure(freq)) {
            shell_printf(ctx, "Set to %lu MHz\r\n", mhz);
            /* 保存到配置存储 */
            char buf[16];
            snprintf(buf, sizeof(buf), "%lu", mhz);
            config_store_set("ovclk", buf);
        } else {
            shell_puts(ctx, "Invalid frequency\r\n");
        }
    } else if (strcmp(argv[1], "try") == 0) {
        if (argc < 3) { shell_puts(ctx, "usage: ovclk try <mhz>\r\n"); return; }
        uint32_t mhz = atoi(argv[2]);
        rp2350_clk_freq_t freq = mhz * 1000000;
        if (rp2350_clk_configure(freq)) {
            shell_printf(ctx, "Tried %lu MHz (not persisted)\r\n", mhz);
        } else {
            shell_puts(ctx, "Invalid frequency\r\n");
        }
    } else if (strcmp(argv[1], "get") == 0) {
        cmd_ovclk(1, argv, ctx);
    } else if (strcmp(argv[1], "save") == 0) {
        rp2350_clk_config_t cfg;
        rp2350_clk_get_config(&cfg);
        char buf[16];
        snprintf(buf, sizeof(buf), "%lu", cfg.sys_clk_hz/1000000);
        config_store_set("ovclk", buf);
        shell_puts(ctx, "Saved\r\n");
    } else if (strcmp(argv[1], "unsave") == 0) {
        config_store_set("ovclk", "0");
        shell_puts(ctx, "Cleared\r\n");
    }
}

static const shell_cmd_t ovclk_cmd = {"ovclk", cmd_ovclk, "clock/overclock config"};

void shell_ovclk_register(void) {
    shell_register(&ovclk_cmd);
}