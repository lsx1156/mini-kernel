/**
 * @file    test_task_mgmt.c
 * @brief   任务管理模块单元测试
 * 验收标准对应：
 * - 可正常创建多个任务并稳定轮转运行
 * - 任务销毁后内存可完整回收，无碎片泄漏
 */
#include "unity.h"
#include "unity_fixture.h"
#include "task.h"
#include "sched.h"
#include "mem.h"
#include "hal_interface.h"

/* 模拟 HAL 接口（测试环境） */
static uint32_t mock_tick = 0;
void mock_systick_init(uint32_t hz) { (void)hz; mock_tick = 0; }
uint32_t mock_systick_get_tick(void) { return mock_tick; }
void mock_systick_delay_us(uint32_t us) { (void)us; }

static int mock_putc(char c) { (void)c; return 1; }
static int mock_getc(char *c) { (void)c; return 0; }
void mock_console_init(uint32_t b) { (void)b; }

const hal_systick_ops_t hal_systick_ops = {
    .init = mock_systick_init, .get_tick = mock_systick_get_tick, .delay_us = mock_systick_delay_us
};
const hal_console_ops_t hal_console_ops = {
    .init = mock_console_init, .putc = mock_putc, .getc = mock_getc
};
const hal_export_t hal_export = { .systick = &hal_systick_ops, .console = &hal_console_ops };

/* 测试用任务栈 */
static uint8_t stack1[512], stack2[512], stack3[512];
static tcb_t *task1 = NULL, *task2 = NULL, *task3 = NULL;
static int task1_run = 0, task2_run = 0, task3_run = 0;

void task1_entry(void *arg) { (void)arg; task1_run++; task_yield(); }
void task2_entry(void *arg) { (void)arg; task2_run++; task_yield(); }
void task3_entry(void *arg) { (void)arg; task3_run++; task_yield(); }

TEST_GROUP(task_mgmt);

TEST_SETUP(task_mgmt) {
    /* 每个测试前重置状态 */
    kmem_init(NULL, 0);  /* 重置堆 */
    task_module_init();
    mock_tick = 0;
    task1_run = task2_run = task3_run = 0;
}

TEST_TEAR_DOWN(task_mgmt) {
    if (task1) { task_destroy(task1); task1 = NULL; }
    if (task2) { task_destroy(task2); task2 = NULL; }
    if (task3) { task_destroy(task3); task3 = NULL; }
}

TEST(task_mgmt, create_multiple_tasks) {
    task1 = task_create("t1", task1_entry, NULL, 256, 1);
    task2 = task_create("t2", task2_entry, NULL, 256, 1);
    task3 = task_create("t3", task3_entry, NULL, 256, 1);

    TEST_ASSERT_NOT_NULL(task1);
    TEST_ASSERT_NOT_NULL(task2);
    TEST_ASSERT_NOT_NULL(task3);
    TEST_ASSERT_EQUAL_UINT(1, task1->id);
    TEST_ASSERT_EQUAL_UINT(2, task2->id);
    TEST_ASSERT_EQUAL_UINT(3, task3->id);
    TEST_ASSERT_EQUAL(TASK_STATE_READY, task1->state);
}

TEST(task_mgmt, round_robin_scheduling) {
    task1 = task_create("t1", task1_entry, NULL, 256, 1);
    task2 = task_create("t2", task2_entry, NULL, 256, 1);

    /* 模拟调度器轮转 4 个 tick */
    for (int i = 0; i < 4; i++) {
        tcb_t *next = sched_ready_pick_next();
        TEST_ASSERT_NOT_NULL(next);
        /* 手动执行任务入口 */
        if (next == task1) task1_entry(NULL);
        else if (next == task2) task2_entry(NULL);
    }

    TEST_ASSERT_EQUAL(2, task1_run);
    TEST_ASSERT_EQUAL(2, task2_run);
}

TEST(task_mgmt, task_destroy_frees_memory) {
    size_t heap_before = kmem_free_size();

    task1 = task_create("t1", task1_entry, NULL, 256, 1);
    size_t heap_after_create = kmem_free_size();
    TEST_ASSERT_TRUE(heap_after_create < heap_before);

    task_destroy(task1);
    task1 = NULL;
    size_t heap_after_destroy = kmem_free_size();
    TEST_ASSERT_EQUAL(heap_before, heap_after_destroy);
}

TEST(task_mgmt, task_sleep_wakeup) {
    task1 = task_create("t1", task1_entry, NULL, 256, 1);

    /* 任务主动睡眠 */
    task_sleep(10);
    TEST_ASSERT_EQUAL(TASK_STATE_SLEEP, task1->state);
    TEST_ASSERT_EQUAL(10, task1->ticks_to_sleep);

    /* SysTick 处理 5 次 */
    for (int i = 0; i < 5; i++) {
        mock_tick++;
        sched_sleep_tick();
    }
    TEST_ASSERT_EQUAL(TASK_STATE_SLEEP, task1->state);
    TEST_ASSERT_EQUAL(5, task1->ticks_to_sleep);

    /* 再处理 5 次，任务应唤醒 */
    for (int i = 0; i < 5; i++) {
        mock_tick++;
        sched_sleep_tick();
    }
    TEST_ASSERT_EQUAL(TASK_STATE_READY, task1->state);
}

TEST(task_mgmt, stack_overflow_detection) {
    /* 创建极小栈任务 */
    tcb_t *t = task_create("small", task1_entry, NULL, 64, 1);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_TRUE(task_stack_check(t));

    /* 模拟栈溢出：破坏魔值 */
    uint32_t *stack_end = (uint32_t *)t->stack_base - t->stack_size / 4;
    *stack_end = 0xBADFOOD;
    TEST_ASSERT_FALSE(task_stack_check(t));

    task_destroy(t);
}

TEST_GROUP_RUNNER(task_mgmt) {
    RUN_TEST_CASE(task_mgmt, create_multiple_tasks);
    RUN_TEST_CASE(task_mgmt, round_robin_scheduling);
    RUN_TEST_CASE(task_mgmt, task_destroy_frees_memory);
    RUN_TEST_CASE(task_mgmt, task_sleep_wakeup);
    RUN_TEST_CASE(task_mgmt, stack_overflow_detection);
}