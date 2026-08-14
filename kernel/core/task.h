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
} tcb_t;

/* 空闲任务 TCB（静态分配） */
extern tcb_t g_idle_task;
extern tcb_t *g_current_task;
extern tcb_t *g_task_pool[OS_CFG_MAX_TASKS];
extern uint8_t g_task_bitmap;           /* 位图：bit=1 表示槽位已用 */

/* ================================================================
 * 内部接口
 * ================================================================ */
void    task_module_init(void);
tcb_t  *task_create(const char *name, void (*entry)(void *), void *arg,
                    size_t stack_size, uint8_t priority);
void    task_destroy(tcb_t *task);
void    task_sleep(uint32_t ticks);
void    task_wakeup(tcb_t *task);
void    task_yield(void);
void    task_suspend(tcb_t *task);
void    task_resume(tcb_t *task);

/* 空闲任务入口 */
void    idle_task_entry(void *arg);

/* 栈溢出检查 */
#define STACK_MAGIC_VALUE  0xDEADBEEF
static inline void task_stack_init(tcb_t *task) {
    uint32_t *stack_end = (uint32_t *)task->stack_base;
    stack_end = (uint32_t *)((uintptr_t)stack_end - task->stack_size);
    *stack_end = STACK_MAGIC_VALUE;
    task->stack_magic = STACK_MAGIC_VALUE;
}

static inline int task_stack_check(tcb_t *task) {
    uint32_t *stack_end = (uint32_t *)task->stack_base;
    stack_end = (uint32_t *)((uintptr_t)stack_end - task->stack_size);
    return (*stack_end == STACK_MAGIC_VALUE);
}

#endif /* TASK_H */