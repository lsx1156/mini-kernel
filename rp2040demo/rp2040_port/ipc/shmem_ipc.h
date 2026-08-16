/**
 * @file    shmem_ipc.h
 * @brief   v2.6 双核共享内存 IPC —— 内存映射 / 消息编码 / Core0 侧 API
 *
 * 架构（见 memmap_ipc.ld 头注释）：
 *   Core0（跑 mini-kernel，Flash XIP + 条带 RAM 128KB）
 *     ├─ 传感器采集 → 写 IN[n]（16KB 乒乓）
 *     ├─ FIFO push「帧就绪」→ SEV 唤醒 Core1
 *     ├─ 等 FIFO「帧完成」→ 读 OUT[n] 做安全判定
 *   Core1（裸循环，SRAM 镜像 @0x20019000，不开中断）
 *     └─ WFE 等通知 → 读 IN[n] → 运算 → 写 OUT[n] → FIFO 应答
 *
 * 防撕裂：缓冲「所有权」随 FIFO 消息在两核间转移 ——
 *   READY(n)：Core0 放弃 IN[n]/OUT[n] 访问权，交给 Core1
 *   DONE(n) ：Core1 交还 IN[n]/OUT[n] 访问权
 * 同一 16KB 块任一时刻只有单侧访问 → 无锁、无撕裂。
 * ctrl 统计字段全部单字 32bit（M0+ 对齐访问原子），同样无需锁。
 *
 * 【并发安全依据】Cortex-M0+ 无 cache：SRAM 写后立即对另一核可见；
 * 每侧在 FIFO push/pop 前后加 DMB，保证「先写数据、后发通知」的
 * 程序序跨核成立（RP2040 总线矩阵对跨端点写序不做架构保证）。
 */
#ifndef SHMEM_IPC_H
#define SHMEM_IPC_H

#include <stdint.h>

/* ================================================================
 * 1. 内存映射（数值与 memmap_ipc.ld 严格对应，改一处必须同步另一处）
 *    【v2.6.1】IPC 区全部使用常规条带地址（RAM 96k 之外的保留区），
 *    不再使用 bank 别名（0x2101xxxx）——实测别名写会打爆内核 .data。
 * ================================================================ */
#define IPC_CTRL_BASE    0x20018000u  /* ctrl 控制块 4KB（RAM region 之外，裸地址） */
#define IPC_CORE1_BASE   0x20019000u  /* Core1 镜像 28KB（linker CORE1 region） */

#define IPC_IN_ADDR(n)   (0x20020000u + (uint32_t)((n) & 1u) * 0x4000u)  /* IN-A/IN-B 各 16KB */
#define IPC_OUT_ADDR(n)  (0x20028000u + (uint32_t)((n) & 1u) * 0x4000u)  /* OUT-A/OUT-B 各 16KB */

#define IPC_BUF_BYTES    16384u       /* 单缓冲 16KB */
#define IPC_BUF_WORDS    (IPC_BUF_BYTES / 4u)

#define IPC_IN(n)        ((volatile uint32_t *)IPC_IN_ADDR(n))
#define IPC_OUT(n)       ((volatile uint32_t *)IPC_OUT_ADDR(n))

/* 【v2.6.6】诊断输出总开关（TTL/UART0）：
 *   1 = 启动 SNAP 快照 + ipc_start 全程分阶段日志（排障用）
 *   0 = 正常运行只保留故障路径输出（STUCK/TIMEOUT/VERIFY FAIL）
 * 主动诊断命令 `ipc fifo` 的探针输出不受此开关影响 */
#define IPC_VERBOSE_DIAG 0

/* ================================================================
 * 2. ctrl 控制块（0x20018000，所有字段单字 —— 原子，无需自旋锁）
 * ================================================================ */
#define IPC_MAGIC        0x31435049u  /* 'IPC1' */
#define IPC_STATE_UNINIT 0u
#define IPC_STATE_READY  1u           /* ctrl 已初始化，Core1 未拉起 */
#define IPC_STATE_RUNNING 2u          /* Core1 worker 运行中 */
#define IPC_STATE_FAULT  3u

typedef struct {
    volatile uint32_t magic;           /* IPC_MAGIC */
    volatile uint32_t state;           /* IPC_STATE_xxx */
    volatile uint32_t frame_ctr;       /* Core0 分配的帧序号（24bit 回绕） */
    volatile uint32_t c1_hb;           /* Core1 心跳：每轮循环 +1（存活检测） */
    volatile uint32_t c1_frames;       /* Core1 已处理帧数 */
    volatile uint32_t c1_err;          /* Core1 侧异常计数 */
    volatile uint32_t c0_drops;        /* Core0 侧超时/丢帧计数 */
    volatile uint32_t last_frame_done; /* 最近完成帧的 frame_id */
} ipc_ctrl_t;

/* ================================================================
 * 3. FIFO 消息编码（32bit 一个字）
 *      [31:8] frame_id（24bit 回绕序号）
 *      [7:4]  消息类型
 *      [3:0]  缓冲号（0=A，1=B）
 * ================================================================ */
#define IPC_MSG_READY   1u            /* C0→C1: IN[n] 已填好，请处理 */
#define IPC_MSG_DONE    2u            /* C1→C0: OUT[n] 已写完，请取 */
#define IPC_MSG_HELLO   3u            /* C1→C0: worker 已启动 */
#define IPC_MSG_FAULT   4u            /* C1→C0: 数据异常 */

#define IPC_MSG_MAKE(type, fid, n) \
    ((((fid) & 0xFFFFFFu) << 8) | (((type) & 0xFu) << 4) | ((n) & 0xFu))
#define IPC_MSG_TYPE(m)   (((m) >> 4) & 0xFu)
#define IPC_MSG_FID(m)    (((m) >> 8) & 0xFFFFFFu)
#define IPC_MSG_BUF(m)    ((m) & 0xFu)

/* Core1 对 IN 的固定变换（selftest/bench 双侧同算校验）：
 *   out[i] = in[i] + i        （i 为 32bit 字下标）
 *   out[last] = in[] 全体 XOR 折叠 */
#define IPC_XFORM_ADD    0u

/* ================================================================
 * 4. Core0 侧 API（实现见 shmem_ipc.c；Core1 侧见 core1_worker.c）
 * ================================================================ */
/* 输出回调（bench/selftest 分阶段打印，避免库直接依赖 shell） */
typedef void (*ipc_emit_t)(const char *s);

typedef struct {
    uint32_t iters;      /* 请求迭代数 */
    uint32_t ok;         /* 成功帧数 */
    uint32_t min_us;     /* 最快往返 µs */
    uint32_t max_us;     /* 最慢往返 µs */
    uint32_t avg_us;     /* 平均往返 µs（四舍五入） */
    uint32_t kbps;       /* 双向合计吞吐 KB/s（每帧 16KB 入 + 16KB 出） */
} ipc_bench_t;

int      ipc_init(void);          /* ctrl 校验/初始化 → READY（不拉起 Core1） */
int      ipc_start(void);         /* 镜像拷贝 + 拉起 Core1 → RUNNING（与 mcore 互斥） */
int      ipc_stop(void);          /* multicore_reset_core1 → READY */
int      ipc_is_running(void);
volatile ipc_ctrl_t *ipc_ctrl(void);
void     ipc_fifo_probe(void);    /* 【v2.6.3】SIO FIFO 深度探针（诊断推手） */
void     ipc_boot_snap(const char *tag); /* 【v2.6.4】启动期 FIFO/PSM 快照（TTL 输出） */

/* 分步帧 API（传感器路径推荐：begin → 直接写 IPC_IN(n) → commit → wait） */
uint32_t ipc_frame_new(void);                        /* ++frame_ctr，返回 fid */
int      ipc_frame_post(uint32_t n, uint32_t fid);   /* DMB + READY + SEV */
int      ipc_wait_done(uint32_t fid, uint32_t timeout_ms, uint32_t *out_msg);

/* 一步式：拷贝 src → IN[n] 并发送（bytes ≤ 16KB，尾部非对齐字节补 0） */
int32_t  ipc_send_frame(uint32_t n, const void *src, uint32_t bytes);

/* 诊断：n 轮乒乓（A/B 交替）实测往返时延；emit 可为 NULL（静默） */
int ipc_bench(uint32_t n_iters, ipc_bench_t *out, ipc_emit_t emit);
/* 诊断：心跳存活 + 单帧 + 4 帧交替乒乓，分阶段打印 PASS/FAIL */
int ipc_selftest(ipc_emit_t emit);

#endif /* SHMEM_IPC_H */
