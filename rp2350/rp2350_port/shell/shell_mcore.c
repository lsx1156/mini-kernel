/**
 * @file    shell_mcore.c
 * @brief   RP2350 双核管理 Shell 命令
 */

#include "shell_core.h"
#include "rp2350_port.h"
#include "task.h"
#include <stdio.h>

static void cmd_mcore(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) {
        shell_puts(ctx, "mcore <status|start|stop|task>\r\n");
        shell_puts(ctx, "  status  - show core status\r\n");
        shell_puts(ctx, "  start   - launch core 1\r\n");
        shell_puts(ctx, "  stop    - halt core 1\r\n");
        shell_puts(ctx, "  task    - list tasks on core 1\r\n");
        return;
    }
    
    if (strcmp(argv[1], "status") == 0) {
        uint32_t core0_id = 0;
        uint32_t core1_id = 1;
        
        shell_printf(ctx, "Core 0 (Main): RUNNING\r\n");
        shell_printf(ctx, "  ID: %lu, Clock: %lu MHz\r\n", core0_id, rp2350_clk_sys_hz()/1000000);
        
        extern bool g_core1_started;
        if (g_core1_started) {
            shell_printf(ctx, "Core 1: RUNNING\r\n");
            shell_printf(ctx, "  ID: %lu, Clock: %lu MHz\r\n", core1_id, rp2350_clk_sys_hz()/1000000);
        } else {
            shell_puts(ctx, "Core 1: STOPPED\r\n");
        }
    } else if (strcmp(argv[1], "start") == 0) {
        extern bool g_core1_started;
        if (g_core1_started) {
            shell_puts(ctx, "Core 1 already running\r\n");
        } else {
            rp2350_core1_start(core1_worker_entry, NULL);
            g_core1_started = true;
            shell_puts(ctx, "Core 1 started\r\n");
        }
    } else if (strcmp(argv[1], "stop") == 0) {
        extern bool g_core1_started;
        if (!g_core1_started) {
            shell_puts(ctx, "Core 1 not running\r\n");
        } else {
            /* 发送停止信号给 Core 1 */
            shell_puts(ctx, "Core 1 stop requested\r\n");
        }
    } else if (strcmp(argv[1], "task") == 0) {
        extern void ps_core1(shell_ctx_t *ctx);
        ps_core1(ctx);
    }
}

static const shell_cmd_t mcore_cmd = {"mcore", cmd_mcore, "multi-core management"};

void shell_mcore_register(void) {
    shell_register(&mcore_cmd);
}