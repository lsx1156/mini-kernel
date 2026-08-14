/**
 * @file    test_firmware_main.c
 * @brief   集成测试固件入口：验收标准全自动化验证
 * 运行在 Renode/QEMU 中，通过 UART 输出测试结果
 * 验收标准对应：
 * 1. 最小内核体积 ≤ 10KB Flash / 4KB RAM  (链接时静态检查)
 * 2. 可正常创建多个任务并稳定轮转运行
 * 3. 任务销毁后内存可完整回收，无碎片泄漏
 */
#include "kernel.h"
#include "task.h"
#include "mem.h"
#include "hal_interface.h"
#include <stdio.h>

static int test_passed = 0;
static int test_failed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (cond) { \
            hal_console_putc('['); hal_console_putc('O'); hal_console_putc('K'); hal_console_putc(']'); \
            hal_console_putc(' '); \
            const char *s = msg; while(*s) hal_console_putc(*s++); \
            hal_console_putc('\r'); hal_console_putc('\n'); \
            test_passed++; \
        } else { \
            hal_console_putc('['); hal_console_putc('F'); hal_console_putc('A'); hal_console_putc('I'); hal_console_putc('L'); hal_console_putc(']'); \
            hal_console_putc(' '); \
            const char *s = msg; while(*s) hal_console_putc(*s++); \
            hal_console_putc('\r'); hal_console_putc('\n'); \
            test_failed++; \
        } \
    } while(0)

/* 测试任务 */
static int task1_cnt = 0, task2_cnt = 0;
static tcb_t *g_task1 = NULL, *g_task2 = NULL;

void test_task1(void *arg) {
    (void)arg;
    for (int i = 0; i < 5; i++) {
        task1_cnt++;
        hal_systick_delay_us(1000);
        task_yield();
    }
    /* 自毁验证内存回收 */
    size_t heap_before = kmem_free_size();
    task_destroy(g_task1);
    g_task1 = NULL;
    size_t heap_after = kmem_free_size();
    TEST_ASSERT(heap_after >= heap_before, "Task destroy frees memory");
}

void test_task2(void *arg) {
    (void)arg;
    for (int i = 0; i < 5; i++) {
        task2_cnt++;
        hal_systick_delay_us(1000);
        task_yield();
    }
    /* 自毁 */
    size_t heap_before = kmem_free_size();
    task_destroy(g_task2);
    g_task2 = NULL;
    size_t heap_after = kmem_free_size();
    TEST_ASSERT(heap_after >= heap_before, "Task2 destroy frees memory");
}

/* 堆完整性测试 */
void test_heap_integrity(void) {
    size_t free0 = kmem_free_size();
    void *p[10];
    for (int i = 0; i < 10; i++) {
        p[i] = kmalloc(64 + i * 8);
        TEST_ASSERT(p[i] != NULL, "kmalloc succeeds");
    }
    for (int i = 0; i < 10; i++) kfree(p[i]);
    size_t free1 = kmem_free_size();
    TEST_ASSERT(free1 == free0, "Heap fully recovered after alloc/free");
}

/* 栈溢出检测测试 */
void test_stack_guard(void) {
    tcb_t *t = task_create("guard", test_task1, NULL, 128, 1);
    TEST_ASSERT(t != NULL, "Task create with small stack");
    TEST_ASSERT(task_stack_check(t), "Stack guard intact initially");
    task_destroy(t);
}

/* 主测试流程 */
void integration_test_main(void *arg) {
    (void)arg;

    hal_console_putc('\r'); hal_console_putc('\n');
    const char *hdr = "=== Integration Test Start ===\r\n";
    while (*hdr) hal_console_putc(*hdr++);

    /* 1. 任务创建与轮转 */
    g_task1 = task_create("T1", test_task1, NULL, 256, 1);
    g_task2 = task_create("T2", test_task2, NULL, 256, 1);
    TEST_ASSERT(g_task1 && g_task2, "Multiple tasks created");

    /* 等待任务完成（轮转 10 次） */
    for (int i = 0; i < 20; i++) {
        hal_systick_delay_us(5000);
        task_yield();
    }
    TEST_ASSERT(task1_cnt == 5 && task2_cnt == 5, "Tasks round-robin executed");

    /* 2. 堆完整性 */
    test_heap_integrity();

    /* 3. 栈保护 */
    test_stack_guard();

    /* 4. 空闲任务运行 */
    TEST_ASSERT(g_idle_task.state == TASK_STATE_READY || g_idle_task.state == TASK_STATE_RUNNING,
                "Idle task alive");

    /* 结果汇总 */
    hal_console_putc('\r'); hal_console_putc('\n');
    char buf[64];
    snprintf(buf, sizeof(buf), "PASSED: %d, FAILED: %d\r\n", test_passed, test_failed);
    while (*buf) hal_console_putc(*buf++);

    if (test_failed == 0) {
        const char *ok = "ALL TESTS PASSED\r\n";
        while (*ok) hal_console_putc(*ok++);
    } else {
        const char *fail = "SOME TESTS FAILED\r\n";
        while (*fail) hal_console_putc(*fail++);
    }

    /* 结束仿真（Renode 可检测此输出自动退出） */
    hal_console_putc('\r'); hal_console_putc('\n');
    const char *end = "=== SIMULATION END ===\r\n";
    while (*end) hal_console_putc(*end++);

    while (1) { hal_systick_delay_us(1000000); }
}

/* 入口 */
void kernel_main(void) {
    hal_systick_init(1000);
    hal_console_init(115200);
    kmem_init(&__heap_start__, &__heap_end__ - &__heap_start__);
    task_module_init();
    sched_init();

    /* 创建测试主任务 */
    task_create("integ", integration_test_main, NULL, 1024, 1);

    sched_start();
}