/**
 * @file    test_sched.c
 * @brief   调度器单元测试
 * 验收标准对应：
 * - 核心调度策略：时间片轮转+尽力而为执行，无优先级抢占
 * - 可正常创建多个任务并稳定轮转运行
 */
#include "unity.h"
#include "unity_fixture.h"
#include "sched.h"
#include "task.h"
#include "mem.h"

TEST_GROUP(sched);

static uint8_t heap_buf[4096];
static tcb_t *tasks[4];
static int exec_order[16];
static int exec_idx = 0;

void task_a(void *arg) { exec_order[exec_idx++] = 1; task_yield(); }
void task_b(void *arg) { exec_order[exec_idx++] = 2; task_yield(); }
void task_c(void *arg) { exec_order[exec_idx++] = 3; task_yield(); }
void task_d(void *arg) { exec_order[exec_idx++] = 4; task_yield(); }

TEST_SETUP(sched) {
    kmem_init(heap_buf, sizeof(heap_buf));
    task_module_init();
    exec_idx = 0;
    memset(exec_order, 0, sizeof(exec_order));
    for (int i = 0; i < 4; i++) tasks[i] = NULL;
}

TEST_TEAR_DOWN(sched) {
    for (int i = 0; i < 4; i++) {
        if (tasks[i]) { task_destroy(tasks[i]); tasks[i] = NULL; }
    }
}

TEST(sched, ready_queue_fifo_order) {
    tasks[0] = task_create("A", task_a, NULL, 256, 1);
    tasks[1] = task_create("B", task_b, NULL, 256, 1);
    tasks[2] = task_create("C", task_c, NULL, 256, 1);

    /* 模拟 9 次调度（每个任务跑 3 次） */
    for (int i = 0; i < 9; i++) {
        tcb_t *next = sched_ready_pick_next();
        TEST_ASSERT_NOT_NULL(next);
        if (next == tasks[0]) task_a(NULL);
        else if (next == tasks[1]) task_b(NULL);
        else if (next == tasks[2]) task_c(NULL);
    }

    /* 验证轮转顺序：A B C A B C A B C */
    for (int i = 0; i < 9; i++) {
        TEST_ASSERT_EQUAL(i % 3 + 1, exec_order[i]);
    }
}

TEST(sched, idle_task_when_no_ready) {
    /* 所有任务睡眠，应调度到 idle */
    tasks[0] = task_create("A", task_a, NULL, 256, 1);
    task_sleep(100);

    tcb_t *next = sched_ready_pick_next();
    TEST_ASSERT_EQUAL(&g_idle_task, next);
}

TEST(sched, sleep_queue_wakeup_order) {
    tasks[0] = task_create("A", task_a, NULL, 256, 1);
    tasks[1] = task_create("B", task_b, NULL, 256, 1);
    tasks[2] = task_create("C", task_c, NULL, 256, 1);

    /* 让任务按不同 tick 睡眠 */
    task_sleep(5);  /* A 睡 5 */
    task_sleep(2);  /* B 睡 2 */
    task_sleep(8);  /* C 睡 8 */

    /* 模拟 tick 到 2：B 唤醒 */
    for (int i = 0; i < 2; i++) sched_sleep_tick();
    TEST_ASSERT_EQUAL(TASK_STATE_READY, tasks[1]->state);
    TEST_ASSERT_EQUAL(TASK_STATE_SLEEP, tasks[0]->state);
    TEST_ASSERT_EQUAL(TASK_STATE_SLEEP, tasks[2]->state);

    /* tick 到 5：A 唤醒 */
    for (int i = 0; i < 3; i++) sched_sleep_tick();
    TEST_ASSERT_EQUAL(TASK_STATE_READY, tasks[0]->state);

    /* tick 到 8：C 唤醒 */
    for (int i = 0; i < 3; i++) sched_sleep_tick();
    TEST_ASSERT_EQUAL(TASK_STATE_READY, tasks[2]->state);
}

TEST(sched, no_preemption_weight_only_affects_timeslice) {
    /* 权重不同，仅影响时间片长度，不触发抢占 */
    tasks[0] = task_create("high", task_a, NULL, 256, 3);  /* weight=3 */
    tasks[1] = task_create("low",  task_b, NULL, 256, 1);  /* weight=1 */

    /* 验证时间片分配 */
    TEST_ASSERT_EQUAL(OS_CFG_TIME_SLICE_TICKS * 3, tasks[0]->time_slice);
    TEST_ASSERT_EQUAL(OS_CFG_TIME_SLICE_TICKS * 1, tasks[1]->time_slice);

    /* 调度器仍按轮转顺序选取，不因优先级抢占 */
    tcb_t *next = sched_ready_pick_next();
    TEST_ASSERT_EQUAL(tasks[0], next);
    next = sched_ready_pick_next();
    TEST_ASSERT_EQUAL(tasks[1], next);
}

TEST_GROUP_RUNNER(sched) {
    RUN_TEST_CASE(sched, ready_queue_fifo_order);
    RUN_TEST_CASE(sched, idle_task_when_no_ready);
    RUN_TEST_CASE(sched, sleep_queue_wakeup_order);
    RUN_TEST_CASE(sched, no_preemption_weight_only_affects_timeslice);
}