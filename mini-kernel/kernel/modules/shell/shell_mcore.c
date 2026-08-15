/**
 * @file    shell_mcore.c
 * @brief   v2.4 多核调度测试命令 `mcore`
 *
 *  用法：
 *    mcore status   — 查看 core0/core1 当前任务 + core1 工作负载计数
 *    mcore demo     — 在 core1 上创建一个心跳任务（每 100ms 自增计数）
 *    mcore stop     — 销毁 core1 上的心跳任务
 *
 *  说明：
 *    · core1 工作任务**不打印**到共享控制台（避免双核并发 putchar 竞争
 *      SDK stdio），只用内存计数证明 core1 在真实调度该任务；
 *    · 通过 `mcore status` 在主核读取计数即可确认 core1 已运行。
 */
#include "shell_core.h"
#include "task.h"
#include "sched.h"
#include "hal_interface.h"
#include <string.h>
#include <stdlib.h>

/* core1 心跳任务：每 100ms 自增计数，证明 core1 在真实调度它 */
static volatile uint32_t g_core1_counter = 0;
static tcb_t *g_core1_worker = NULL;
static int g_core1_started = 0;

static void task_core1_worker(void *arg) {
    (void)arg;
    for (;;) {
        g_core1_counter++;
        task_sleep(100);   /* 走 core1 的睡眠队列 + tick，验证多核调度 */
    }
}

static int cmd_mcore(int argc, char **argv, shell_ctx_t *ctx) {
    if (argc < 2) {
        shell_puts(ctx,
            "mcore: multi-core scheduling test\n"
            "  mcore status   - show core0/core1 current task + core1 counter\n"
            "  mcore demo     - start core1 + spawn a heartbeat task on it (every 100ms)\n"
            "  mcore stop     - destroy the core1 heartbeat task\n");
        return 0;
    }

    if (!strcmp(argv[1], "status")) {
        char line[64];
        shell_puts(ctx, "core0 curr : ");
        if (g_current_task_table[0]) shell_puts(ctx, g_current_task_table[0]->name);
        shell_puts(ctx, "\n");
        shell_puts(ctx, "core1 curr : ");
        if (g_current_task_table[1]) shell_puts(ctx, g_current_task_table[1]->name);
        shell_puts(ctx, "\n");
        shell_snprintf(line, sizeof(line), "core1 counter: %lu\n", (unsigned long)g_core1_counter);
        shell_puts(ctx, line);
        shell_snprintf(line, sizeof(line), "core1 ticks  : %lu\n", (unsigned long)hal_mcore_core1_ticks());
        shell_puts(ctx, line);
        shell_puts(ctx, g_core1_started ? "core1 started : YES\n" : "core1 started : NO\n");
        shell_puts(ctx, g_core1_worker ? "core1 worker : RUNNING\n" : "core1 worker : none\n");
        return 0;
    }

    if (!strcmp(argv[1], "demo")) {
        if (g_core1_worker) { shell_puts(ctx, "core1 worker already running (use mcore stop)\n"); return 1; }
        if (!g_core1_started) {
            hal_mcore_start();           /* 显式启动 core1 调度器 */
            g_core1_started = 1;
            shell_puts(ctx, "core1 launched\n");
        }
        g_core1_counter = 0;
        g_core1_worker = task_create_on("c1worker", task_core1_worker, NULL, 512, 1, 1);
        shell_puts(ctx, g_core1_worker ? "core1 worker spawned OK\n" : "FAILED: task_create_on core1\n");
        return 0;
    }

    if (!strcmp(argv[1], "stop")) {
        if (!g_core1_worker) { shell_puts(ctx, "no core1 worker\n"); return 0; }
        task_destroy(g_core1_worker);
        g_core1_worker = NULL;
        shell_puts(ctx, "core1 worker destroyed\n");
        return 0;
    }

    shell_puts(ctx, "unknown mcore sub-command. Try `mcore` for help.\n");
    return 1;
}

void shell_mcore_register(void) {
    shell_register("mcore", cmd_mcore, "mcore status|demo|stop",
                   "Multi-core scheduling test (spawn/inspect task on core1)");
}