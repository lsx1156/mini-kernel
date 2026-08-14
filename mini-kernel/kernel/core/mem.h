/**
 * @file    mem.h
 * @brief   内存管理模块：固定内存池 + 简易内核堆
 */
#ifndef MEM_H
#define MEM_H

#include <stddef.h>
#include <stdint.h>
#include "os_config.h"

/* ================================================================
 * 固定内存池（用于 TCB、信号量、队列等内核对象）
 * ================================================================ */
typedef struct {
    void *pool;
    size_t block_size;
    size_t num_blocks;
    uint8_t *bitmap;  /* 位图：1=已分配 */
} mem_pool_t;

/* 创建/销毁内存池 */
int mem_pool_create(mem_pool_t *pool, void *buf, size_t buf_size, size_t block_size);
void *mem_pool_alloc(mem_pool_t *pool);
void mem_pool_free(mem_pool_t *pool, void *ptr);

/* ================================================================
 * 简易内核堆（最先适配，首部隐式链表）
 * ================================================================ */
void kmem_init(void *heap_start, size_t heap_size);
void *kmalloc(size_t size);
void kfree(void *ptr);
size_t kmem_free_size(void);
size_t kmem_max_free_block(void);

/* ================================================================
 * 栈溢出检查辅助
 * ================================================================ */
#define STACK_MAGIC  0xDEADBEEF
static inline void stack_guard_init(void *stack_bottom) {
    *(uint32_t *)stack_bottom = STACK_MAGIC;
}
static inline int stack_guard_check(void *stack_bottom) {
    return *(uint32_t *)stack_bottom == STACK_MAGIC;
}

#endif /* MEM_H */