/**
 * @file    test_mem_mgmt.c
 * @brief   内存管理模块单元测试
 * 验收标准对应：
 * - 任务销毁后内存可完整回收，无碎片泄漏
 * - 最小内核体积 ≤ 10KB Flash / 4KB RAM
 */
#include "unity.h"
#include "unity_fixture.h"
#include "mem.h"
#include "task.h"

TEST_GROUP(mem_mgmt);

static uint8_t heap_buf[4096];

TEST_SETUP(mem_mgmt) {
    kmem_init(heap_buf, sizeof(heap_buf));
}

TEST_TEAR_DOWN(mem_mgmt) {
    /* 无需清理 */
}

TEST(mem_mgmt, kmalloc_kfree_basic) {
    void *p1 = kmalloc(64);
    TEST_ASSERT_NOT_NULL(p1);

    void *p2 = kmalloc(128);
    TEST_ASSERT_NOT_NULL(p2);

    kfree(p1);
    kfree(p2);

    /* 再次分配应能复用内存 */
    void *p3 = kmalloc(192);
    TEST_ASSERT_NOT_NULL(p3);
    kfree(p3);
}

TEST(mem_mgmt, kmalloc_fails_when_oom) {
    /* 分配大块直到耗尽 */
    void *ptrs[20];
    int count = 0;
    for (int i = 0; i < 20; i++) {
        ptrs[i] = kmalloc(256);
        if (ptrs[i]) count++;
        else break;
    }
    TEST_ASSERT_TRUE(count > 0);

    /* 下一次分配应失败 */
    void *p = kmalloc(256);
    TEST_ASSERT_NULL(p);

    /* 释放后应成功 */
    kfree(ptrs[0]);
    p = kmalloc(64);
    TEST_ASSERT_NOT_NULL(p);
}

TEST(mem_mgmt, mem_pool_alloc_free) {
    mem_pool_t pool;
    uint8_t pool_buf[512];
    TEST_ASSERT_EQUAL(0, mem_pool_create(&pool, pool_buf, sizeof(pool_buf), 64));

    void *p1 = mem_pool_alloc(&pool);
    void *p2 = mem_pool_alloc(&pool);
    TEST_ASSERT_NOT_NULL(p1);
    TEST_ASSERT_NOT_NULL(p2);
    TEST_ASSERT_NOT_EQUAL(p1, p2);

    mem_pool_free(&pool, p1);
    void *p3 = mem_pool_alloc(&pool);
    TEST_ASSERT_EQUAL(p1, p3);  /* 应复用刚释放的块 */

    mem_pool_free(&pool, p2);
    mem_pool_free(&pool, p3);
}

TEST(mem_mgmt, kmem_free_size_tracks_correctly) {
    size_t free0 = kmem_free_size();
    void *p = kmalloc(256);
    size_t free1 = kmem_free_size();
    TEST_ASSERT_TRUE(free1 < free0);

    kfree(p);
    size_t free2 = kmem_free_size();
    TEST_ASSERT_EQUAL(free0, free2);
}

TEST_GROUP_RUNNER(mem_mgmt) {
    RUN_TEST_CASE(mem_mgmt, kmalloc_kfree_basic);
    RUN_TEST_CASE(mem_mgmt, kmalloc_fails_when_oom);
    RUN_TEST_CASE(mem_mgmt, mem_pool_alloc_free);
    RUN_TEST_CASE(mem_mgmt, kmem_free_size_tracks_correctly);
}