/**
 * @file    syscall_contract.c
 * @brief   系统调用契约表 —— 静态表生成 + 查询接口实现
 *
 *  g_syscall_table 由 X-Macro 在编译期展开为 const 数组，链接后存放于
 *  Flash .rodata 段，零 RAM 开销。查询函数按线性扫描（表项 ≤ 20，
 *  O(n) 完全可接受，避免哈希表的开销）。
 */
#include "syscall_contract.h"
#include <string.h>

/* ================================================================
 * 静态契约表（编译期生成，const → Flash）
 *
 *  SYSCALL 宏已在 syscall_contract.h 中定义为初始化项格式，
 *  SYSCALL_CONTRACT_TABLE 宏展开为所有条目。
 * ================================================================ */
const syscall_entry_t g_syscall_table[] = {
    SYSCALL_CONTRACT_TABLE
};

const size_t g_syscall_table_size =
    sizeof(g_syscall_table) / sizeof(g_syscall_table[0]);

/* ================================================================
 * 查询接口实现
 * ================================================================ */

const syscall_entry_t *syscall_lookup_by_id(syscall_id_t id) {
    for (size_t i = 0; i < g_syscall_table_size; i++) {
        if (g_syscall_table[i].id == id) {
            return &g_syscall_table[i];
        }
    }
    return NULL;
}

const syscall_entry_t *syscall_lookup_by_name(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < g_syscall_table_size; i++) {
        if (strcmp(g_syscall_table[i].name, name) == 0) {
            return &g_syscall_table[i];
        }
    }
    return NULL;
}

const syscall_entry_t *syscall_get_entry(size_t idx) {
    if (idx >= g_syscall_table_size) return NULL;
    return &g_syscall_table[idx];
}

size_t syscall_table_size(void) {
    return g_syscall_table_size;
}
