/**
 * @file    shell_ipc.c
 * @brief   RP2350 核间通信 Shell 命令
 */

#include "shell_core.h"
#include "rp2350_port.h"
#include "shmem_ipc.h"
#include <stdio.h>

static void cmd_ipc(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) {
        shell_puts(ctx, "ipc <send|recv|stats>\r\n");
        shell_puts(ctx, "  send <msg>  - send message to core 1\r\n");
        shell_puts(ctx, "  recv        - receive message from core 1\r\n");
        shell_puts(ctx, "  stats       - show IPC statistics\r\n");
        return;
    }
    
    if (strcmp(argv[1], "send") == 0) {
        if (argc < 3) { shell_puts(ctx, "usage: ipc send <msg>\r\n"); return; }
        
        char msg[256] = {0};
        for (int i = 2; i < argc; i++) {
            strcat(msg, argv[i]);
            if (i < argc - 1) strcat(msg, " ");
        }
        
        size_t len = strlen(msg);
        hal_err_t err = shmem_ipc_send(1, msg, len + 1);
        if (err == HAL_OK) {
            shell_puts(ctx, "sent\r\n");
        } else {
            shell_printf(ctx, "send failed: %d\r\n", err);
        }
    } else if (strcmp(argv[1], "recv") == 0) {
        char buf[256];
        size_t len = 0;
        hal_err_t err = shmem_ipc_recv(1, buf, sizeof(buf), &len);
        if (err == HAL_OK && len > 0) {
            shell_printf(ctx, "recv (%zu): %s\r\n", len, buf);
        } else {
            shell_puts(ctx, "no message\r\n");
        }
    } else if (strcmp(argv[1], "stats") == 0) {
        extern ipc_stats_t g_ipc_stats;
        shell_printf(ctx, "IPC Statistics:\r\n");
        shell_printf(ctx, "  sent:     %lu\r\n", g_ipc_stats.sent);
        shell_printf(ctx, "  recv:     %lu\r\n", g_ipc_stats.recv);
        shell_printf(ctx, "  errors:   %lu\r\n", g_ipc_stats.errors);
        shell_printf(ctx, "  overflow: %lu\r\n", g_ipc_stats.overflow);
    }
}

static const shell_cmd_t ipc_cmd = {"ipc", cmd_ipc, "inter-core communication"};

void shell_ipc_register(void) {
    shell_register(&ipc_cmd);
}