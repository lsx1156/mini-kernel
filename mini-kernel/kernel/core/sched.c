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
/* 内部版本：在调用方已持有临界区时使用（如 task.c、中断上下文） */
void _sched_ready_enqueue(tcb_t *task) {
    /* 【v2.2.9 · 防御性检查 · 防重复入队（快速判定版）】
     *
     *   v2.2.9 first patch 的 bug：用 _sched_list_contains 做链表遍历
     *   判定"是否已在队列中"。当 TCB 被 memset 清零后 next/prev=NULL 时
     *   没问题；但如果 RAM 初值被 bootloader/复位前残留污染（例如 soft reset
     *   不写零 bss），task->next/prev 恰好 == &g_ready_head（非 NULL）→
     *   遍历 → 逻辑误判"已在队列中" → return 不做入队 → 就绪队列永远空
     *   → sched_pick_next 永远返回 idle → LED 疯狂 tight loop，表现为
     *   "启动即爆闪"。
     *
     *   修复思路：只做快速判定 —— next==NULL && prev==NULL 视为"不在任何
     *   队列"，允许入队；否则认为"已在队列中"，直接跳过。
     *     · 正常契约下严格成立（remove 时强制置 NULL，memset 初始化也是 0）
     *     · RAM 残留污染场景下 next/prev 非 NULL 时也不会错误地"再串到
     *       另一个链表"，最多是**偶发一次调度**被延后一个时间片，
     *       后果远小于链表环导致的全局崩溃。
     */
    if (task->next != NULL || task->prev != NULL) {
        return;
    }
    /* 尾插 */
    task->next = &g_ready_head;
    task->prev = g_ready_head.prev;
    g_ready_head.prev->next = task;
    g_ready_head.prev = task;
}

void _sched_ready_remove(tcb_t *task) {
    task->prev->next = task->next;
    task->next->prev = task->prev;
    task->next = task->prev = NULL;
}

void sched_ready_enqueue(tcb_t *task) {
    if (!task) return;
    /* 保存/恢复 PRIMASK：避免冷初始化阶段意外开中断 */
    uint32_t __pmask;
    __asm volatile ("mrs %0, primask" : "=r" (__pmask) :: "memory");
    __asm volatile ("cpsid i" ::: "memory");
    _sched_ready_enqueue(task);
    __asm volatile ("msr primask, %0" :: "r" (__pmask) : "memory");
}

void sched_ready_remove(tcb_t *task) {
    if (!task || task->next == NULL) return;
    /* 保存/恢复 PRIMASK */
    uint32_t __pmask;
    __asm volatile ("mrs %0, primask" : "=r" (__pmask) :: "memory");
    __asm volatile ("cpsid i" ::: "memory");
    _sched_ready_remove(task);
    __asm volatile ("msr primask, %0" :: "r" (__pmask) : "memory");
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
     *    重新入队（仅当仍为 READY）。idle 永不入队，是空队列兜底。
     *
     *    注意：此函数可能在中断上下文中被调用，使用内部版本 */
    tcb_t *next = g_ready_head.next;
    _sched_ready_remove(next);  /* 可能在中断上下文，使用内部版本 */
    return next;
}

/* ================================================================
 * 睡眠队列操作
 * ================================================================ */
/* 内部版本：在调用方已持有临界区或处于中断上下文时使用 */
void _sched_sleep_enqueue(tcb_t *task) {
    /* 【v2.2.9 · 防御性检查 · 防双队列同驻（快速判定版）】
     *   同 _sched_ready_enqueue：next/prev 非 NULL 视为已在队列 → return。
     *   移除链表遍历，避免 RAM 初值残留导致"永远不入队"的启动死锁。*/
    if (task->next != NULL || task->prev != NULL) {
        return;
    }
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

void _sched_sleep_remove(tcb_t *task) {
    task->prev->next = task->next;
    task->next->prev = task->prev;
    task->next = task->prev = NULL;
}

void sched_sleep_enqueue(tcb_t *task) {
    if (!task) return;
    /* 保存/恢复 PRIMASK */
    uint32_t __pmask;
    __asm volatile ("mrs %0, primask" : "=r" (__pmask) :: "memory");
    __asm volatile ("cpsid i" ::: "memory");
    _sched_sleep_enqueue(task);
    __asm volatile ("msr primask, %0" :: "r" (__pmask) : "memory");
}

void sched_sleep_remove(tcb_t *task) {
    if (!task || task->next == NULL) return;
    /* 保存/恢复 PRIMASK */
    uint32_t __pmask;
    __asm volatile ("mrs %0, primask" : "=r" (__pmask) :: "memory");
    __asm volatile ("cpsid i" ::: "memory");
    _sched_sleep_remove(task);
    __asm volatile ("msr primask, %0" :: "r" (__pmask) : "memory");
}

void sched_sleep_tick(void) {
    /* 注意：此函数在 SysTick 中断上下文中被调用
     * 不需要额外关中断（硬件已自动禁止中断嵌套）
     * 使用内部版本直接操作链表 */
    tcb_t *curr = g_sleep_head.next;
    while (curr != &g_sleep_head) {
        if (curr->ticks_to_sleep > 0) {
            curr->ticks_to_sleep--;
        }
        if (curr->ticks_to_sleep == 0) {
            tcb_t *next = curr->next;
            _sched_sleep_remove(curr);
            curr->state = TASK_STATE_READY;
            _sched_ready_enqueue(curr);
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

/* PendSV 安全上下文用的 hex 打印（单字节 hal_console_putc） */
static inline void _pendsv_puthex32(uint32_t v) {
    const char hex[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) {
        hal_console_putc(hex[(v >> i) & 0xF]);
    }
}
static inline void _pendsv_puts(const char *s) {
    while (*s) hal_console_putc(*s++);
}
static inline void _pendsv_putu32(uint32_t v) {
    /* 最多 10 位十进制，用小栈上缓冲反向打印，避免用 sprintf */
    char buf[11];
    int n = 0;
    if (v == 0) buf[n++] = '0';
    else while (v > 0) { buf[n++] = '0' + (char)(v % 10); v /= 10; }
    while (n > 0) hal_console_putc(buf[--n]);
}

/* ================================================================
 * 上下文切换入口（由 PendSV_Handler 调用，old_sp 是 push r4-r11 后的 PSP）
 *   · 保存当前任务 SP，选下一个就绪任务，返回新任务 SP
 *   · 汇编层再 pop r4-r11 + bx lr（硬件自动弹 r0-r3/r12/lr/pc/xpsr）
 * ================================================================ */
uint32_t *sched_do_switch(uint32_t *old_sp) {
    /* 注意：此函数在 PendSV 中断上下文中被调用
     * 不需要额外关中断（硬件已自动禁止中断嵌套）
     * 使用内部版本直接操作链表 */
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

        /* 【v2.2.11 · OVF 最小化处理】
         *   之前在这里打印 400+ 字节诊断（逐个 putc），可能在 PendSV 上下文
         *   里触发 TinyUSB 的状态机推进，间接造成调用链复杂化。
         *   新策略：不在 PendSV 里打印，只把 from->state 设 SUSPEND 避免
         *   它继续运行（SUSPEND 后下面的 RUNNING 判断不会重新入队）。
         *   标志位 stack_overflow 保留为 1，用户通过 `ps` 命令查询时能看到
         *   "OVF=Y"。这样任务静默挂起，不打印 → 不触发额外嵌套 → 不会爆闪。*/
        if (from->stack_overflow) {
            from->state = TASK_STATE_SUSPEND;   /* 下面的 RUNNING 判断就不进了 */
        }

        if (from->state == TASK_STATE_RUNNING) {
            from->state = TASK_STATE_READY;
            if (from != &g_idle_task) {
                _sched_ready_enqueue(from);  /* 中断上下文使用内部版本 */
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