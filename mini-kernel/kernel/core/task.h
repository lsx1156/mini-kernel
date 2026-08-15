/**
 * @file    task.h
 * @brief   任务管理模块内部定义（内核核心私有，用户态不可见）
 */
#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "os_config.h"
#include "hal_interface.h"

/* ================================================================
 * 任务状态机
 * ================================================================ */
typedef enum {
    TASK_STATE_READY   = 0,  /* 就绪 */
    TASK_STATE_RUNNING = 1,  /* 运行中 */
    TASK_STATE_SLEEP   = 2,  /* 延时睡眠 */
    TASK_STATE_SUSPEND = 3,  /* 挂起 */
    TASK_STATE_DEAD    = 4,  /* 已销毁，等待回收 */
} task_state_t;

/* ================================================================
 * 任务控制块 (TCB)
 * 固定大小，存放在静态内存池中
 * ================================================================ */
typedef struct tcb {
    /* 内核调度相关 */
    uint32_t            *sp;            /* 栈指针（指向 hal_context_t） */
    task_state_t        state;          /* 任务状态 */
    uint8_t             priority;       /* 权重/优先级（仅用于时间片比例） */
    /* 【v2.2.9 · 栈溢出检测解耦标志位】
     *   · kernel_tick_hook（TIMER_IRQ_3 中断上下文）里只置这个标志，
     *     不做任何调度/长打印；
     *   · sched_do_switch（PendSV 安全上下文）在保存完 from->sp 后
     *     检查此标志 → 打印诊断 + 把 state 改为 SUSPEND，安全挂起；
     *   · 彻底避免"在 TIMER_IRQ_3 中断里调 task_suspend(self) +
     *     超长 hal_console_putc 打印"导致 USB/HardFault 的问题。*/
    uint8_t             stack_overflow; /* 1=tick_hook 检测到 MAGIC 破坏 */
    uint32_t            ticks_to_sleep; /* 剩余睡眠 tick */
    uint32_t            time_slice;     /* 剩余时间片 */
    uint32_t            weight;         /* 权重（默认 1） */

    /* 栈管理 */
    void                *stack_base;    /* 栈底地址（高地址） */
    size_t              stack_size;     /* 栈大小 (字节) */
    uint32_t            stack_magic;    /* 栈尾魔值 */

    /* 链表节点（就绪队列、睡眠队列） */
    struct tcb          *next;
    struct tcb          *prev;

    /* 任务标识 */
    char                name[12];       /* 任务名，以 \0 结尾 */
    uint32_t            id;             /* 任务 ID */
    uint8_t             core;           /* 绑定的核心号（0=core0, 1=core1） */
} tcb_t;

/* 空闲任务 TCB（每核一个，静态分配） */
extern tcb_t g_idle_task_table[OS_CFG_CORE_COUNT];
extern tcb_t *g_current_task_table[OS_CFG_CORE_COUNT];
extern tcb_t *g_task_pool[OS_CFG_MAX_TASKS];
extern uint8_t g_task_bitmap;           /* 位图：bit=1 表示槽位已用 */

/* 【多核】g_idle_task / g_current_task 按当前运行核心自动展开。
 * 这样内核/调度器/中断里所有既有引用（g_current_task->xxx 等）无需改动，
 * 自动作用到"本核心"的实例。 */
#define g_idle_task      (g_idle_task_table[hal_core_id()])
#define g_current_task   (g_current_task_table[hal_core_id()])

/* 【v2.2.9 · 栈限额统计（内核/用户分离）】
 *   g_user_stack_used_bytes    — 用户任务(id>2)动态栈总和
 *   g_kernel_stack_used_bytes  — 内核任务(id 1~2)动态栈总和
 *   idle(id=0)静态栈不计入任何统计。*/
extern size_t g_user_stack_used_bytes;
extern size_t g_kernel_stack_used_bytes;

/* ================================================================
 * 内部接口
 * ================================================================ */
void    task_module_init(void);
tcb_t  *task_create(const char *name, void (*entry)(void *), void *arg,
                    size_t stack_size, uint8_t priority);
tcb_t  *task_create_on(const char *name, void (*entry)(void *), void *arg,
                       size_t stack_size, uint8_t priority, uint8_t core); /* 多核：显式指定核心（0/1） */
void    task_destroy(tcb_t *task);
void    task_sleep(uint32_t ticks);
void    task_wakeup(tcb_t *task);
void    task_yield(void);
void    task_suspend(tcb_t *task);
void    task_resume(tcb_t *task);

/* 空闲任务入口 */
void    idle_task_entry(void *arg);

/* 栈溢出检查
 *
 * 【v2.2.9 · 关键修复 · 金丝雀位置前移 + 多写 4 个】
 *
 *   旧版 Bug：STATIC_MAGIC 仅写在 stack_base - stack_size（数据区第 1 字节），
 *   而堆块 header [heap_block_t] 恰好在数据区"前面"（低地址 -8B 处）。
 *   栈向下溢出时先踩坏 header（size/next 元数据），再踩几字节才轮到
 *   MAGIC → task_stack_check 仍返回 true（未检测），但 kmalloc/kfree
 *   遍历空闲链表时读到垃圾指针 → 随机 HardFault。
 *
 *   修复：在栈底最低 16B 范围内写 4 个 MAGIC，任何 1 个被破坏都算溢出；
 *   额外提供 task_stack_used()/task_stack_margin() 估算栈已用量，
 *   kernel_tick_hook 可以提前打印"栈余量 <25%"警告，不用等到写穿才报。
 */
#define STACK_MAGIC_VALUE   0xDEADBEEF
/* 【v2.2.11 · MAGIC 缓冲从 4 字 → 16 字 = 64B】
 *   实测：Pico SDK i2c_write_blocking_until 调用栈极深，叠加上每次
 *   TIMER_IRQ_3 进入异常时硬件自动 PUSH 的 8×4=32B 栈帧（Thread→Handler
 *   过渡用 PSP，不是 MSP），刚好把 16B 小 MAGIC 区击穿。MAGIC 区从 16B
 *   扩到 64B 后：底部 64B 只写金丝雀，任务实际可用区少 48B，但保证
 *   5 轮硬件 PUSH 帧 + SDK 深调用都碰不到"任务栈最低可写区"的边界。
 *   代价：每个任务额外占用 48B。RP2040 256KB SRAM 完全能接受。*/
#define STACK_MAGIC_COUNT   16u
#define STACK_WARN_PCT      25u         /* 剩余 < 25% 时警告（可选） */

static inline void task_stack_init(tcb_t *task) {
    uint8_t *p = (uint8_t *)task->stack_base;
    p -= task->stack_size;              /* p = 栈底最低地址（数据区起始）*/
    uint32_t *magic_ptr = (uint32_t *)p;
    for (uint32_t i = 0; i < STACK_MAGIC_COUNT; i++) {
        magic_ptr[i] = STACK_MAGIC_VALUE;
    }
    task->stack_magic = STACK_MAGIC_VALUE;
}

static inline int task_stack_check(tcb_t *task) {
    uint8_t *p = (uint8_t *)task->stack_base;
    p -= task->stack_size;
    uint32_t *magic_ptr = (uint32_t *)p;
    for (uint32_t i = 0; i < STACK_MAGIC_COUNT; i++) {
        if (magic_ptr[i] != STACK_MAGIC_VALUE) return 0;
    }
    return 1;
}

/* 估算当前栈已用字节（从 MAGIC 后第一个字节向上扫第一个非 0 字节）
 *
 *   【v2.2.11 修复 · used=stack_size 恒满误报】
 *   v2.2.9 版本从 bot（最低地址）直接扫：bot 位置放了 STACK_MAGIC_COUNT
 *   个 0xDEADBEEF（非 0），所以 *p != 0 立刻命中 bot → hit=bot →
 *   used = stack_size → 诊断永远显示 100% 满，完全失去参考价值。
 *   修复：先跳过底部 STACK_MAGIC_COUNT × 4 字节的 MAGIC 区，
 *   从 MAGIC 上方的"真实可写栈区域起点"开始扫第一个非 0。
 *
 *   精度说明：kmalloc 返回的块不一定被清零（若上一次 free 后没有被
 *   擦写则残留旧数据），所以仅作**粗估**；精确判断靠 task_stack_check()
 *   的 MAGIC 金丝雀（只要任一 MAGIC 被改写 = 真溢出）。
 */
static inline size_t task_stack_used(tcb_t *task) {
    uint8_t *base = (uint8_t *)task->stack_base;
    uint8_t *bot  = base - task->stack_size;
    /* 跳过底部 MAGIC 区（STACK_MAGIC_COUNT × 4B，v2.2.11 默认 64B）*/
    uint8_t *scan_start = bot + (STACK_MAGIC_COUNT * sizeof(uint32_t));
    uint8_t *hit  = base;   /* 默认"没用到"（hit=base → used=0） */
    if (scan_start < base) {
        for (uint8_t *p = scan_start; p < base; p++) {
            if (*p != 0x00) { hit = p; break; }
        }
    }
    return (size_t)(base - hit);
}

/* ================================================================
 * 【v2.2.9 · 栈限额查询接口（内核/用户分离 + 总量告警）】
 *   · 用户需求：「内核占 8KB」= 内核任务栈配额；
 *     「用户可使用栈自行更替」= 用户任务栈可自行调整；
 *     「总占用量超 80%」= 内核栈 + 用户栈 总和超 8KB 配额的 80% 时告警。
 *   · 全部 static inline 只读，无临界区（自然字长单次读写）。
 * ================================================================ */

/* --- 用户栈统计（id > 2 的任务） --- */

/* 已分配的用户栈总字节数（动态，task_create/destroy 实时变化） */
static inline size_t task_user_stack_used(void) {
    return g_user_stack_used_bytes;
}

/* 用户栈池总大小（os_config.h 中配置，默认 8KB） */
static inline size_t task_user_stack_pool(void) {
    return (size_t)OS_CFG_USER_STACK_POOL_BYTES;
}

/* 用户栈剩余字节数（饱和到 0） */
static inline size_t task_user_stack_free(void) {
    size_t pool = (size_t)OS_CFG_USER_STACK_POOL_BYTES;
    size_t used = g_user_stack_used_bytes;
    return (used >= pool) ? 0 : (pool - used);
}

/* 用户栈已使用百分比（0~100，整型，向上取整 1%） */
static inline uint8_t task_user_stack_used_pct(void) {
    size_t pool = (size_t)OS_CFG_USER_STACK_POOL_BYTES;
    if (pool == 0) return 0;
    size_t used = g_user_stack_used_bytes;
    if (used >= pool) return 100;
    return (uint8_t)((used * 100u + (pool - 1)) / pool);
}

/* --- 内核栈统计（id 1~2 的任务：boot_setup + shell） --- */

static inline size_t task_kernel_stack_used(void) {
    return g_kernel_stack_used_bytes;
}

/* 内核栈配额 = 同 8KB（用户需求：「内核占 8KB」） */
static inline size_t task_kernel_stack_pool(void) {
    return (size_t)OS_CFG_USER_STACK_POOL_BYTES;
}

static inline uint8_t task_kernel_stack_used_pct(void) {
    size_t pool = (size_t)OS_CFG_USER_STACK_POOL_BYTES;
    if (pool == 0) return 0;
    size_t used = g_kernel_stack_used_bytes;
    if (used >= pool) return 100;
    return (uint8_t)((used * 100u + (pool - 1)) / pool);
}

/* --- 总栈用量（内核 + 用户）告警 --- */

/* 内核栈 + 用户栈 总和 */
static inline size_t task_total_stack_used(void) {
    return g_kernel_stack_used_bytes + g_user_stack_used_bytes;
}

/* 总栈配额 = 8KB（内核占 8KB，用户栈也在其中，合计不超过 8KB） */
static inline size_t task_total_stack_pool(void) {
    return (size_t)OS_CFG_USER_STACK_POOL_BYTES;
}

/* 总栈使用百分比（0~100） */
static inline uint8_t task_total_stack_used_pct(void) {
    size_t pool = (size_t)OS_CFG_USER_STACK_POOL_BYTES;
    if (pool == 0) return 0;
    size_t used = g_kernel_stack_used_bytes + g_user_stack_used_bytes;
    if (used >= pool) return 100;
    return (uint8_t)((used * 100u + (pool - 1)) / pool);
}

/* 是否触发"80% 低告警"（总栈 = 内核 + 用户 超过 8KB 的 80%） */
static inline int task_total_stack_is_low(void) {
    size_t pool = (size_t)OS_CFG_USER_STACK_POOL_BYTES;
    if (pool == 0) return 0;
    size_t total = g_kernel_stack_used_bytes + g_user_stack_used_bytes;
    return (total * 100u) >= (pool * (size_t)OS_CFG_USER_STACK_WARN_PCT);
}

#endif /* TASK_H */