/**
 * @file    sched.c
 * @brief   调度器实现：时间片轮转 + 权重比例
 */
#include "sched.h"
#include "task.h"
#include <string.h>

/* ================================================================
 * 就绪队列：双向循环链表，按权重分桶（简化版：单队列 + 计数器）
 * ================================================================ */
static tcb_t g_ready_head = { .next = &g_ready_head, .prev = &g_ready_head };
static tcb_t g_sleep_head = { .next = &g_sleep_head, .prev = &g_sleep_head };

/* ================================================================
 * 就绪队列操作
 * ================================================================ */
void sched_ready_enqueue(tcb_t *task) {
    if (!task) return;
    /* 尾插 */
    task->next = &g_ready_head;
    task->prev = g_ready_head.prev;
    g_ready_head.prev->next = task;
    g_ready_head.prev = task;
}

void sched_ready_remove(tcb_t *task) {
    if (!task || task->next == NULL) return;
    task->prev->next = task->next;
    task->next->prev = task->prev;
    task->next = task->prev = NULL;
}

tcb_t *sched_ready_pick_next(void) {
    if (g_ready_head.next == &g_ready_head) {
        return &g_idle_task;  /* 无就绪任务，跑空闲任务 */
    }

    /* 取队头并从队列移除（变为 RUNNING，不在就绪队列中）。
     *
     *    【关键修复 · 爆闪 + TCB 踩坏 根因】：
     *    旧版在此处 remove 后又 enqueue（轮转风格），使被选中的任务
     *    仍在就绪队列里。随后 sched_do_switch 又把 from 入队 → 同一任务
     *    在队列中出现两次 → sched_ready_enqueue 写 prev/next 指针时
     *    生成自引用环 task->next=task → pick_next 永远只选它 →
     *    LED 任务疯狂 tight loop = "爆闪"；链表损坏波及 idle TCB
     *    → ps 输出 ID=12533 / name="=." 等乱码。
     *
     *    正确契约：RUNNING 任务不在就绪队列；切出时由 sched_do_switch
     *    重新入队（仅当仍为 READY）。idle 永不入队，是空队列兜底。 */
    tcb_t *next = g_ready_head.next;
    sched_ready_remove(next);
    return next;
}

/* ================================================================
 * 睡眠队列操作
 * ================================================================ */
void sched_sleep_enqueue(tcb_t *task) {
    if (!task) return;
    /* 按剩余 tick 排序（小在前），O(n) 够用，任务数 ≤ 16 */
    tcb_t *pos = g_sleep_head.next;
    while (pos != &g_sleep_head && pos->ticks_to_sleep <= task->ticks_to_sleep) {
        pos = pos->next;
    }
    task->next = pos;
    task->prev = pos->prev;
    pos->prev->next = task;
    pos->prev = task;
}

void sched_sleep_remove(tcb_t *task) {
    if (!task || task->next == NULL) return;
    task->prev->next = task->next;
    task->next->prev = task->prev;
    task->next = task->prev = NULL;
}

void sched_sleep_tick(void) {
    tcb_t *curr = g_sleep_head.next;
    while (curr != &g_sleep_head) {
        if (curr->ticks_to_sleep > 0) {
            curr->ticks_to_sleep--;
        }
        if (curr->ticks_to_sleep == 0) {
            tcb_t *next = curr->next;
            sched_sleep_remove(curr);
            curr->state = TASK_STATE_READY;
            sched_ready_enqueue(curr);
            curr = next;
        } else {
            curr = curr->next;
        }
    }
}

/* ================================================================
 * 调度器初始化与启动
 * ================================================================ */
void sched_init(void) {
    /* 队列已静态初始化 */
}

void sched_start(void) {
    /* 取第一个就绪任务并标记 RUNNING */
    g_current_task = sched_ready_pick_next();
    g_current_task->state = TASK_STATE_RUNNING;

    /* 【关键修复 · SVC 启动首任务】
     *
     *   之前 PSP = task->sp + 32（硬件栈帧起始），svc #0 触发时硬件把
     *   异常帧压到 PSP 上 → 直接覆盖了预初始化的硬件栈帧 → SVC_Handler
     *   用 EXC_RETURN 弹出的是 SVC 异常数据而非任务初始上下文 → CPU 乱飞。
     *
     *   修复：PSP = task->sp（r4-r11 区域），svc #0 的异常帧压在 r4-r11
     *   区域（该区域首任务启动时不需要），task->sp + 32 的硬件栈帧保持
     *   完整。SVC_Handler 里把 PSP 偏移 32 字节即可回到硬件栈帧。 */
    uint32_t first_sp = (uint32_t)g_current_task->sp;

    __asm volatile (
        "cpsie i\n"
        "msr psp, %[sp]\n"
        "svc #0\n"
        : /* 无输出 */
        : [sp] "r"(first_sp)
        : "memory"
    );
    /* 不应到达此处 */
    while (1) {
        register uint32_t sio_base = 0xD0000000u;
        register uint32_t mask25   = 0x02000000u;
        *(volatile uint32_t *)(sio_base + 0x014) = mask25;
        for (volatile uint32_t j = 0; j < 6250000; j++) __asm("nop");
        *(volatile uint32_t *)(sio_base + 0x018) = mask25;
        for (volatile uint32_t j = 0; j < 6250000; j++) __asm("nop");
    }
}

/* ================================================================
 * 上下文切换入口（由 PendSV_Handler 调用，old_sp 是 push r4-r11 后的 PSP）
 *   · 保存当前任务 SP，选下一个就绪任务，返回新任务 SP
 *   · 汇编层再 pop r4-r11 + bx lr（硬件自动弹 r0-r3/r12/lr/pc/xpsr）
 * ================================================================ */
uint32_t *sched_do_switch(uint32_t *old_sp) {
    tcb_t *from = g_current_task;

    /* 1. 保存 from 的 SP（push r4-r11 后已经更新过的 PSP）
     *
     *    【关键修复 · 爆闪 Bug 根因】：
     *    之前无条件把 from->state 改成 READY 并入就绪队列，但 task_sleep()
     *    已经把任务状态设为 SLEEP 并移入睡眠队列了——这里再覆盖成 READY
     *    并塞回就绪队列 → 任务同时在两个队列里 → sched_ready_pick_next
     *    立刻又把它捡回来跑 → task_sleep 形同虚设 → task_led tight loop
     *    疯狂翻转 LED = "爆闪"。
     *
     *    修复：仅当 from 仍处于 RUNNING（时间片到期的正常抢占）时才设
     *    READY 并入队；SLEEP / SUSPEND 等状态由 task_sleep / task_suspend
     *    自行管理队列归属，这里不能覆盖。 */
    if (from) {
        from->sp = old_sp;
        if (from->state == TASK_STATE_RUNNING) {
            from->state = TASK_STATE_READY;
            if (from != &g_idle_task) {
                sched_ready_enqueue(from);
            }
        }
    }

    /* 2. 选下一个就绪任务（空队列则 idle） */
    tcb_t *to = sched_ready_pick_next();
    to->state = TASK_STATE_RUNNING;
    g_current_task = to;

    /* 3. 返回新任务的 SP（pop r4-r11 从这里开始） */
    return to->sp;
}