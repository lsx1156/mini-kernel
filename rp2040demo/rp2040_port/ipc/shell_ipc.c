/**
 * @file    shell_ipc.c
 * @brief   v2.6 Shell 命令 `ipc` —— 双核共享内存 IPC 控制/诊断
 *
 *  用法：
 *    ipc start                — 拷镜像 + 拉起 Core1 worker（与 mcore demo 互斥）
 *    ipc stop                 — 复位 Core1，回到 READY
 *    ipc status               — ctrl 状态 / 心跳 / 帧统计 / 内存映射
 *    ipc bench [n]            — n 帧（默认 16）A/B 乒乓实测往返时延与吞吐
 *    ipc selftest             — 心跳 + 单帧 + 4 帧乒乓 三阶段自检
 *
 *  注意：
 *    · `ipc bench/selftest` 计时段为忙等（不 task_sleep），保证 µs 级计时
 *      精度；期间其它任务短暂让不出 CPU，属预期行为。
 *    · Core1 worker 见 core1_worker.c；内存映射见 memmap_ipc.ld。
 */
#include "shell_core.h"
#include "shmem_ipc.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void emit(const char *s) { shell_puts(NULL, s); }

static const char *state_name(uint32_t st) {
    switch (st) {
        case IPC_STATE_RUNNING: return "RUNNING";
        case IPC_STATE_READY:   return "READY";
        case IPC_STATE_FAULT:   return "FAULT";
        default:                return "UNINIT";
    }
}

static int cmd_ipc(int argc, char **argv, shell_ctx_t *ctx) {
    (void)ctx;
    const char *sub = (argc > 1) ? argv[1] : "status";

    if (!strcmp(sub, "start")) {
        int r = ipc_start();
        if (r == 0) {
            volatile ipc_ctrl_t *c = ipc_ctrl();
            char line[96];
            snprintf(line, sizeof(line),
                     "IPC: core1 RUNNING (SRAM mirror @0x%08X, stack 8KB top)\r\n",
                     (unsigned)IPC_CORE1_BASE);
            emit(line);
            snprintf(line, sizeof(line),
                     "IPC: ctrl@0x%08X  IN-A/B@0x%08X/0x%08X  OUT-A/B@0x%08X/0x%08X\r\n",
                     (unsigned)IPC_CTRL_BASE,
                     (unsigned)IPC_IN_ADDR(0), (unsigned)IPC_IN_ADDR(1),
                     (unsigned)IPC_OUT_ADDR(0), (unsigned)IPC_OUT_ADDR(1));
            emit(line);
            (void)c;
        } else if (ipc_ctrl()->state == IPC_STATE_RUNNING) {
            emit("IPC: already running\r\n");
        } else {
            emit("IPC: start FAILED (core1 no HELLO in 200ms; check mirror copy)\r\n");
        }
        return 0;
    }

    if (!strcmp(sub, "stop")) {
        ipc_stop();
        emit("IPC: core1 reset, state=READY\r\n");
        return 0;
    }

    if (!strcmp(sub, "bench")) {
        if (!ipc_is_running()) {
            emit("IPC: not running (do 'ipc start' first)\r\n");
            return 1;
        }
        uint32_t n = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 10) : 16u;
        if (n == 0u || n > 10000u) { emit("IPC: n out of range (1..10000)\r\n"); return 1; }
        ipc_bench_t b;
        int r = ipc_bench(n, &b, emit);
        char line[112];
        snprintf(line, sizeof(line),
                 "IPC bench: %u/%u ok  avg=%uus min=%uus max=%uus  thru=%u KB/s (32KB/frame)\r\n",
                 (unsigned)b.ok, (unsigned)b.iters,
                 (unsigned)b.avg_us, (unsigned)b.min_us, (unsigned)b.max_us,
                 (unsigned)b.kbps);
        emit(line);
        return (r == 0) ? 0 : 1;
    }

    if (!strcmp(sub, "selftest")) {
        if (!ipc_is_running()) {
            emit("IPC: not running (do 'ipc start' first)\r\n");
            return 1;
        }
        return (ipc_selftest(emit) == 0) ? 0 : 1;
    }

    if (!strcmp(sub, "status")) {
        volatile ipc_ctrl_t *c = ipc_ctrl();
        char line[96];
        uint32_t hb0 = c->c1_hb;
        /* 存活性：20ms 内心跳应自增（busy 短等，不影响调度） */
        uint32_t spins = 0;
        while (c->c1_hb == hb0 && spins < 2000000u) spins++;
        uint32_t hb1 = c->c1_hb;
        snprintf(line, sizeof(line),
                 "IPC: state=%s  alive=%s  hb=%u->%u\r\n",
                 state_name(c->state),
                 (c->state == IPC_STATE_RUNNING) ? ((hb1 > hb0) ? "Y" : "N!") : "-",
                 (unsigned)hb0, (unsigned)hb1);
        emit(line);
        snprintf(line, sizeof(line),
                 "IPC: frames=%u done_fid=%u drops=%u c1_err=%u\r\n",
                 (unsigned)c->c1_frames, (unsigned)c->last_frame_done,
                 (unsigned)c->c0_drops, (unsigned)c->c1_err);
        emit(line);
        snprintf(line, sizeof(line),
                 "IPC: map ctrl=0x%08X core1=0x%08X IN=0x%08X/0x%08X OUT=0x%08X/0x%08X (2x16KB ping-pong)\r\n",
                 (unsigned)IPC_CTRL_BASE, (unsigned)IPC_CORE1_BASE,
                 (unsigned)IPC_IN_ADDR(0), (unsigned)IPC_IN_ADDR(1),
                 (unsigned)IPC_OUT_ADDR(0), (unsigned)IPC_OUT_ADDR(1));
        emit(line);
        return 0;
    }

    shell_puts(ctx, "usage: ipc start|stop|status|bench [n]|selftest\r\n");
    return 1;
}

void shell_ipc_register(void) {
    shell_register("ipc", cmd_ipc,
                   "ipc start|stop|status|bench|selftest",
                   "dual-core shared-mem IPC (FIFO + 2x16KB ping-pong, core1 SRAM worker)");
}
