/**
 * @file    mem.c
 * @brief   内存管理实现
 */
#include "mem.h"
#include <string.h>

/* ================================================================
 * 固定内存池实现
 * ================================================================ */
int mem_pool_create(mem_pool_t *pool, void *buf, size_t buf_size, size_t block_size) {
    if (!pool || !buf || block_size == 0) return -1;
    size_t num = buf_size / block_size;
    if (num == 0) return -1;

    pool->pool = buf;
    pool->block_size = block_size;
    pool->num_blocks = num;
    pool->bitmap = (uint8_t *)kmalloc((num + 7) / 8);
    if (!pool->bitmap) return -1;
    memset(pool->bitmap, 0, (num + 7) / 8);
    return 0;
}

void *mem_pool_alloc(mem_pool_t *pool) {
    if (!pool || !pool->bitmap) return NULL;
    for (size_t i = 0; i < pool->num_blocks; i++) {
        size_t byte_idx = i / 8;
        uint8_t bit = 1 << (i % 8);
        if ((pool->bitmap[byte_idx] & bit) == 0) {
            pool->bitmap[byte_idx] |= bit;
            return (uint8_t *)pool->pool + i * pool->block_size;
        }
    }
    return NULL;
}

void mem_pool_free(mem_pool_t *pool, void *ptr) {
    if (!pool || !ptr) return;
    uint8_t *base = (uint8_t *)pool->pool;
    if ((const uint8_t *)ptr < base) return;
    size_t offset = (uint8_t *)ptr - base;
    if (offset % pool->block_size != 0) return;
    size_t idx = offset / pool->block_size;
    if (idx >= pool->num_blocks) return;
    pool->bitmap[idx / 8] &= ~(1 << (idx % 8));
}

/* ================================================================
 * 简易内核堆（隐式链表，首部含大小+标志）
 * ================================================================ */
typedef struct heap_block {
    size_t size;        /* 包含头部的总大小，低位 bit0=1 表示已分配 */
    struct heap_block *next;  /* 仅空闲块使用 */
} heap_block_t;

#define BLOCK_HEAD_SIZE  sizeof(heap_block_t)
#define BLOCK_ALIGN      8
#define GET_SIZE(b)      ((b)->size & ~1)
#define IS_ALLOC(b)      ((b)->size & 1)
#define SET_ALLOC(b)     ((b)->size |= 1)
#define SET_FREE(b)      ((b)->size &= ~1)

static heap_block_t *g_heap_start = NULL;
static heap_block_t *g_heap_end = NULL;
static heap_block_t *g_free_list = NULL;  /* 空闲链表头 */

void kmem_init(void *heap_start, size_t heap_size) {
    if (!heap_start || heap_size < BLOCK_HEAD_SIZE * 2) return;
    g_heap_start = (heap_block_t *)heap_start;
    g_heap_end = (heap_block_t *)((uint8_t *)heap_start + heap_size);

    /* 初始化为一个大空闲块 */
    g_heap_start->size = heap_size;
    g_heap_start->next = NULL;
    g_free_list = g_heap_start;
}

/* 内部版本：在调用方已持有临界区时使用（如中断上下文） */
static void *_kmalloc_internal(size_t size) {
    if (size == 0) return NULL;
    size = (size + BLOCK_ALIGN - 1) & ~(BLOCK_ALIGN - 1);
    size += BLOCK_HEAD_SIZE;

    heap_block_t *prev = NULL;
    heap_block_t *curr = g_free_list;

    while (curr) {
        if (GET_SIZE(curr) >= size) {
            /* 找到合适块 */
            size_t remain = GET_SIZE(curr) - size;
            if (remain >= BLOCK_HEAD_SIZE + BLOCK_ALIGN) {
                /* 分割 */
                heap_block_t *new_block = (heap_block_t *)((uint8_t *)curr + size);
                new_block->size = remain;
                new_block->next = curr->next;
                curr->size = size;
                curr->next = new_block;
            }
            SET_ALLOC(curr);
            /* 从空闲链表移除 */
            if (prev) prev->next = curr->next;
            else g_free_list = curr->next;
            return (uint8_t *)curr + BLOCK_HEAD_SIZE;
        }
        prev = curr;
        curr = curr->next;
    }
    return NULL;  /* 内存不足 */
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    
    /* 保存/恢复 PRIMASK：若调用方已关中断（如 kernel_main 冷初始化），
     * 不能无条件 cpsie i，否则会在数据结构未初始化时开中断 → HardFault */
    uint32_t __pmask;
    __asm volatile ("mrs %0, primask" : "=r" (__pmask) :: "memory");
    __asm volatile ("cpsid i" ::: "memory");
    void *ptr = _kmalloc_internal(size);
    __asm volatile ("msr primask, %0" :: "r" (__pmask) : "memory");
    return ptr;
}

/* 内部版本：在调用方已持有临界区时使用（如中断上下文） */
static void _kfree_internal(void *ptr) {
    if (!ptr) return;
    heap_block_t *block = (heap_block_t *)((uint8_t *)ptr - BLOCK_HEAD_SIZE);
    if (!IS_ALLOC(block)) return;  /* 重复释放 */

    SET_FREE(block);

    /* 尝试与后向块合并。
     *
     *   【关键修复 · 双重计数根因】
     *   旧版合并时只更新 block->size，但未把 next 从空闲链表移除，
     *   随后 block->next = g_free_list 又把 block 插入链表头 →
     *   next 仍残留在链表中，且其内存已并入 block → kmem_free_size
     *   重复累加 next 大小 → 后续 kmalloc 可能从已合并区域重复分配
     *   → 内存踩坏。 */
    heap_block_t *next = (heap_block_t *)((uint8_t *)block + GET_SIZE(block));
    if (next < g_heap_end && !IS_ALLOC(next)) {
        /* 从空闲链表移除 next */
        if (g_free_list == next) {
            g_free_list = next->next;
        } else {
            heap_block_t *prev = g_free_list;
            while (prev && prev->next != next) prev = prev->next;
            if (prev) prev->next = next->next;
        }
        block->size = GET_SIZE(block) + GET_SIZE(next);
    }

    /* 尝试与前向块合并（需遍历空闲链表找前驱，简化：不做前向合并） */

    /* 插入空闲链表头 */
    block->next = g_free_list;
    g_free_list = block;
}

void kfree(void *ptr) {
    if (!ptr) return;
    
    /* 保存/恢复 PRIMASK */
    uint32_t __pmask;
    __asm volatile ("mrs %0, primask" : "=r" (__pmask) :: "memory");
    __asm volatile ("cpsid i" ::: "memory");
    _kfree_internal(ptr);
    __asm volatile ("msr primask, %0" :: "r" (__pmask) : "memory");
}

size_t kmem_free_size(void) {
    size_t total = 0;
    for (heap_block_t *b = g_free_list; b; b = b->next) {
        total += GET_SIZE(b);
    }
    return total;
}

size_t kmem_max_free_block(void) {
    size_t max = 0;
    for (heap_block_t *b = g_free_list; b; b = b->next) {
        if (GET_SIZE(b) > max) max = GET_SIZE(b);
    }
    return max;
}