/**
 * @file    task.c
 * @brief   任务管理模块实现
 */
#include "task.h"
#include "sched.h"
#include "mem.h"
#include <string.h>
#include <stdio.h>

/* ================================================================
 * 静态资源
 * ================================================================ */
tcb_t g_idle_task;
tcb_t *g_current_task = NULL;
tcb_t *g_task_pool[OS_CFG_MAX_TASKS] = {0};
uint8_t g_task_bitmap = 0;
static uint32_t g_next_task_id = 1;

/* 空闲任务栈 */
static uint8_t g_idle_stack[OS_CFG_IDLE_STACK_SIZE] __attribute__((aligned(8)));

/* ================================================================
 * 内部辅助函数
 * ================================================================ */
static int task_alloc_slot(void) {
    for (int i = 0; i < OS_CFG_MAX_TASKS; i++) {
        if ((g_task_bitmap & (1 << i)) == 0) {
            g_task_bitmap |= (1 << i);
            return i;
        }
    }
    return -1;
}

static void task_free_slot(int slot) {
    g_task_bitmap &= ~(1 << slot);
    g_task_pool[slot] = NULL;
}

/* ================================================================
 * 模块初始化
 * ================================================================ */
void task_module_init(void) {
    /* 创建空闲任务 */
    memset(&g_idle_task, 0, sizeof(tcb_t));
    g_idle_task.stack_base = g_idle_stack + OS_CFG_IDLE_STACK_SIZE;
    g_idle_task.stack_size = OS_CFG_IDLE_STACK_SIZE;
    g_idle_task.priority = 0;
    g_idle_task.weight = 1;
    g_idle_task.state = TASK_STATE_READY;
    g_idle_task.id = 0;
    strncpy(g_idle_task.name, "idle", sizeof(g_idle_task.name) - 1);
    task_stack_init(&g_idle_task);

    /* 初始化上下文 */
    hal_context_t ctx;
    hal_context_init(&ctx, g_idle_task.stack_base, idle_task_entry, NULL);
    g_idle_task.sp = (uint32_t *)ctx.sp;

    /* 加入任务池（供 ps 命令列出），但**不入就绪队列**。
     * idle 是 sched_ready_pick_next 空队列时的兜底，永远不应出现在
     * 就绪队列中——否则会被 pick_next 选为 RUNNING 并从队列移除，
     * 与"兜底"语义冲突，且在旧版 pick_next（remove+enqueue）下
     * 会引发链表重复入队损坏。 */
    g_task_pool[0] = &g_idle_task;
    g_task_bitmap |= 1;
}

/* ================================================================
 * 任务创建
 * ================================================================ */
tcb_t *task_create(const char *name, void (*entry)(void *), void *arg,
                   size_t stack_size, uint8_t priority) {
    if (!entry || stack_size < 128) return NULL;

    /* 对齐栈大小 */
    stack_size = (stack_size + 7) & ~7;

    /* 分配 TCB 槽位 */
    int slot = task_alloc_slot();
    if (slot < 0) return NULL;

    /* 分配栈内存 */
    void *stack = kmalloc(stack_size);
    if (!stack) {
        task_free_slot(slot);
        return NULL;
    }

    /* 分配 TCB 结构（从内核堆） */
    tcb_t *task = (tcb_t *)kmalloc(sizeof(tcb_t));
    if (!task) {
        kfree(stack);
        task_free_slot(slot);
        return NULL;
    }

    memset(task, 0, sizeof(tcb_t));
    task->stack_base = (uint8_t *)stack + stack_size;
    task->stack_size = stack_size;
    task->priority = priority;
    task->weight = (priority > 0) ? priority : 1;
    task->state = TASK_STATE_READY;
    task->id = g_next_task_id++;
    if (name) {
        strncpy(task->name, name, sizeof(task->name) - 1);
    } else {
        snprintf(task->name, sizeof(task->name), "task%lu", (unsigned long)task->id);
    }

    task_stack_init(task);

    /* 初始化上下文 */
    hal_context_t ctx;
    hal_context_init(&ctx, task->stack_base, entry, arg);
    task->sp = (uint32_t *)ctx.sp;

    /* 加入任务池与就绪队列 */
    g_task_pool[slot] = task;
    sched_ready_enqueue(task);

    return task;
}

/* ================================================================
 * 任务销毁
 * ================================================================ */
void task_destroy(tcb_t *task) {
    if (!task || task == &g_idle_task) return;

    /* 禁止销毁当前运行任务：TCB/栈释放后调度器仍会访问 → use-after-free。
     * 调用者应先 task_suspend(task) 再等下一次调度后销毁，或仅销毁非自身任务。 */
    if (task == g_current_task) return;

    /* 从所在队列移除（就绪队列或睡眠队列）。
     *
     *   【关键修复 · use-after-free 根因】
     *   旧版只调 sched_ready_remove，若任务处于 SLEEP 状态则仍在睡眠队列中。
     *   释放 TCB 后，sched_sleep_tick 会递减已释放内存的 ticks_to_sleep，
     *   归零时把悬空 TCB 重新入就绪队列 → 链表损坏 / 随机崩溃。 */
    switch (task->state) {
        case TASK_STATE_READY:
            sched_ready_remove(task);
            break;
        case TASK_STATE_SLEEP:
            sched_sleep_remove(task);
            break;
        case TASK_STATE_RUNNING:
            /* RUNNING 任务不在任何队列（sched_ready_pick_next 已移除），
             * 但销毁运行任务本就非法（上方已拦截），此处防御性 break。 */
            break;
        default:
            break;
    }
    task->state = TASK_STATE_DEAD;

    /* 释放栈与 TCB */
    if (task->stack_base) {
        uint8_t *stack_base = (uint8_t *)task->stack_base - task->stack_size;
        kfree(stack_base);
    }
    kfree(task);

    /* 释放槽位 */
    for (int i = 0; i < OS_CFG_MAX_TASKS; i++) {
        if (g_task_pool[i] == task) {
            task_free_slot(i);
            break;
        }
    }
}

/* ================================================================
 * 任务睡眠/唤醒
 * ================================================================ */
void task_sleep(uint32_t ticks) {
    if (ticks == 0) return;

    tcb_t *task = g_current_task;
    if (!task) return;

    /* 关中断保护队列操作：防止 TIMER_IRQ_0 在 ready_remove 与 sleep_enqueue
     * 之间触发 PendSV → sched_do_switch 此时任务不在任何队列 → 任务永久丢失。
     * 顺序：关中断 → 改状态+移队列 → 设 PendSV → 开中断（PendSV 立即生效）。 */
    __asm volatile ("cpsid i" ::: "memory");
    task->state = TASK_STATE_SLEEP;
    task->ticks_to_sleep = ticks;
    sched_ready_remove(task);
    sched_sleep_enqueue(task);
    hal_yield_trigger();        /* 设 PendSV pending（此时中断关着，不会立即触发） */
    __asm volatile ("cpsie i" ::: "memory");  /* 开中断 → PendSV 立即进入 */
}

void task_wakeup(tcb_t *task) {
    if (!task || task->state != TASK_STATE_SLEEP) return;

    sched_sleep_remove(task);
    task->state = TASK_STATE_READY;
    task->ticks_to_sleep = 0;
    sched_ready_enqueue(task);
}

/* ================================================================
 * 任务让出/挂起/恢复
 * ================================================================ */
void task_yield(void) {
    hal_yield_trigger();
}

void task_suspend(tcb_t *task) {
    if (!task || task == &g_idle_task) return;
    int self_suspend = 0;
    switch (task->state) {
        case TASK_STATE_READY:
            sched_ready_remove(task);
            break;
        case TASK_STATE_SLEEP:
            sched_sleep_remove(task);
            break;
        case TASK_STATE_RUNNING:
            /* 当前任务自我挂起：标记后需触发切换，否则会继续跑 */
            self_suspend = 1;
            break;
        default:
            return;  /* DEAD / SUSPEND 等不处理 */
    }
    task->state = TASK_STATE_SUSPEND;
    /* 自我挂起时主动触发 PendSV，让调度器切到下一个任务。
     * sched_do_switch 看到 from->state != RUNNING 不会把当前任务入就绪队列。 */
    if (self_suspend) {
        hal_yield_trigger();
    }
}

void task_resume(tcb_t *task) {
    if (!task || task->state != TASK_STATE_SUSPEND) return;
    /* 恢复到 READY 状态（原 SLEEP 任务直接就绪，忽略剩余睡眠） */
    task->state = TASK_STATE_READY;
    sched_ready_enqueue(task);
}

/* ================================================================
 * 空闲任务：低功耗或空转
 *
 * 【关键约束】idle 任务 **绝对不能调用 task_sleep()**！
 *   idle 是 sched_ready_pick_next 在就绪队列空时的兜底选择。
 *   若 idle 进入 SLEEP，则就绪队列 + 睡眠队列都无法产出 RUNNING
 *   任务 → 调度器永久锁死（表现：LED 停在某状态不跳）。
 *
 * 【USB 驱动】非 TICKLESS 模式下，idle 在所有用户任务睡眠时占用
 *   100% CPU，此时必须高频驱动 TinyUSB 状态机（tud_task_ext），
 *   否则 USB CDC OUT 数据到达后不被处理 → 输入死锁。
 *   节奏：每 ~1ms 调用一次 hal_usb_poll()，与 usb_print_test
 *   的轮询密度持平。
 *
 * 【LED 指示】非 TICKLESS 时 idle 不控 LED（心跳由 task_led 承担），
 *   这样"LED 停闪"直观指示调度器卡死，而不是 idle 在正常跑。
 * ================================================================ */
void idle_task_entry(void *arg) {
    (void)arg;

    register const uint32_t SIO_BASE = 0xD0000000u;
    register const uint32_t MASK25   = 0x02000000u;

    for (;;) {
#if OS_CFG_TICKLESS
        /* TICKLESS 模式：进入 WFI 等中断唤醒，最低功耗 */
        __asm volatile ("wfi");
#else
        /* 驱动 TinyUSB 状态机：与 usb_print_test 的主循环
         *   while(1) { tud_task(); ... sleep_ms(500); }
         * 等价，但这里更频繁（1ms 级），确保 CDC OUT 数据不堆积。 */
        extern void hal_usb_poll(void);
        hal_usb_poll();

        /* 短 busy-wait ≈ 1ms，不调 task_sleep（idle 不能睡）。
         * 用 hal_systick_delay_us 而非 nop 循环，可自动适配实际时钟频率
         * （底层调 busy_wait_us_32，基于硬件 timer 计数，精度高且不依赖
         * CPU 频率硬编码）。 */
        hal_systick_delay_us(1000);
        (void)SIO_BASE; (void)MASK25;  /* 避免未使用警告 */
#endif
    }
}