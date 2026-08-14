/**
 * @file    sched.h
 * @brief   调度器内部接口
 */
#ifndef SCHED_H
#define SCHED_H

#include "task.h"

/* 就绪队列操作 */
void sched_ready_enqueue(tcb_t *task);
void sched_ready_remove(tcb_t *task);
tcb_t *sched_ready_pick_next(void);

/* 睡眠队列操作 */
void sched_sleep_enqueue(tcb_t *task);
void sched_sleep_remove(tcb_t *task);
void sched_sleep_tick(void);  /* 每个 SysTick 调用 */

/* 内部版本：在调用方已持有临界区时使用（如 task.c 的关中断区间、中断上下文） */
void _sched_ready_enqueue(tcb_t *task);
void _sched_ready_remove(tcb_t *task);
void _sched_sleep_enqueue(tcb_t *task);
void _sched_sleep_remove(tcb_t *task);

/* 调度器核心 */
void sched_init(void);
void sched_start(void);       /* 启动首个任务，不返回 */

#endif /* SCHED_H */