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

    /* ── ① 尝试与后向块合并（高地址方向，紧邻 block 之后） ──
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

    /* ── ② 【v2.2.9 新增】尝试与前向块合并（低地址方向，紧邻 block 之前）
     *
     *   旧版缺陷：只做后向合并，两个物理连续的空闲块 A（低）+ B（高）
     *   若先 free A 再 free B，后向合并能成功（B 吸收 A 后面的？不——
     *   free A 时 A 的 next 是已分配块，不合并；free B 时 B 的 prev 是 A
     *   但代码只看"next 物理块"，prev 是空闲的完全忽略 → A、B 永远
     *   在空闲链表中以两个独立节点存在，总空闲字节够但 max_block
     *   始终上不去 → VT3 多轮循环 + shell/fatfs 交叉 alloc/free 后
     *   sizes[4]=96 这种中等请求失败。
     *
     *   修复：遍历空闲链表，找"物理上刚好在 block 正前方"的那块
     *   （phys_prev + phys_prev->size == block），找到就把 phys_prev
     *   从链表移除 + size 并入 block（注意：合并后"新 block"其实是
     *   phys_prev，因为它的地址更低，后面再统一插入链表头时用
     *   phys_prev 代替 block）。 */
    {
        heap_block_t *phys_prev = NULL;  /* 物理上紧邻 block 的前一个空闲块 */
        heap_block_t *pp_prev   = NULL;  /* phys_prev 在空闲链表中的前驱 */
        heap_block_t *curr      = g_free_list;
        heap_block_t *prev      = NULL;
        while (curr) {
            heap_block_t *phys_curr_end = (heap_block_t *)((uint8_t *)curr + GET_SIZE(curr));
            if (phys_curr_end == block) {
                phys_prev = curr;
                pp_prev   = prev;
                break;   /* 找到了，物理相邻且 prev 的结尾就是 block 的开头 */
            }
            prev = curr;
            curr = curr->next;
        }
        if (phys_prev) {
            /* 从空闲链表移除 phys_prev（它的 size 会被并入 block 前向） */
            if (pp_prev) {
                pp_prev->next = phys_prev->next;
            } else {
                g_free_list = phys_prev->next;
            }
            /* 合并：phys_prev（低地址）吸收 block（高地址）*/
            phys_prev->size = GET_SIZE(phys_prev) + GET_SIZE(block);
            block = phys_prev;   /* 后续"插入链表头"的主角换成更低地址的合并头 */
        }
    }

    /* ③ 合并完成后，新 block 插入空闲链表头 */
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
    /* 【v2.2.9 修复 · 线程安全】
     *   旧版无 PRIMASK 保护遍历 g_free_list。若被 TIMER_IRQ → PendSV 抢占
     *   且其他任务在此期间调用 kmalloc/kfree 修改了链表，恢复后 b->next
     *   可能指向已合并/已分配的块 → 读取垃圾 size/next → HardFault。
     *
     *   VT3 的 kmem_free_size / kmem_max_free_block 与 VT2 的 hal_i2c_mem_write
     *   (内部 kmalloc/kfree) 并发时正是这个场景。 */
    uint32_t __pmask;
    __asm volatile ("mrs %0, primask" : "=r" (__pmask) :: "memory");
    __asm volatile ("cpsid i" ::: "memory");
    size_t total = 0;
    for (heap_block_t *b = g_free_list; b; b = b->next) {
        total += GET_SIZE(b);
    }
    __asm volatile ("msr primask, %0" :: "r" (__pmask) : "memory");
    return total;
}

size_t kmem_max_free_block(void) {
    uint32_t __pmask;
    __asm volatile ("mrs %0, primask" : "=r" (__pmask) :: "memory");
    __asm volatile ("cpsid i" ::: "memory");
    size_t max = 0;
    for (heap_block_t *b = g_free_list; b; b = b->next) {
        if (GET_SIZE(b) > max) max = GET_SIZE(b);
    }
    __asm volatile ("msr primask, %0" :: "r" (__pmask) : "memory");
    return max;
}