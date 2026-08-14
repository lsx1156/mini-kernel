/**
 * @file    syscall_contract.h
 * @brief   系统调用契约表 —— 静态编译期生成，记录每个 syscall 的元信息
 *
 * 设计要点：
 *   · X-Macro 集中定义：所有 syscall 在 SYSCALL_CONTRACT.def 一处维护，
 *     "注入"= 在 .def 中加一行，"更新"= 修改已有行，"记录"= 编译后
 *     生成的只读表自动反映变更。
 *   · 静态 const 数组放 Flash，零 RAM 开销（符合内核体积红线）。
 *   · 查询接口：按 ID / 按名称 / 遍历，供 shell 诊断 + SVC 分发器使用。
 *
 * 使用方式（编译时注入/更新条目）：
 *   1. 在本文件 SYSCALL_CONTRACT 列表中添加/修改一行
 *   2. 重新编译，g_syscall_table 自动更新
 *   3. 运行时通过 syscall_lookup_by_id() 等查询
 */
#ifndef SYSCALL_CONTRACT_H
#define SYSCALL_CONTRACT_H

#include <stdint.h>
#include <stddef.h>
#include "os_config.h"
#include "kernel.h"

/* ================================================================
 * 契约条目结构
 * ================================================================ */
typedef struct {
    syscall_id_t  id;            /* 系统调用号            */
    const char   *name;          /* 名称（字符串字面量）   */
    uint8_t       param_count;   /* 参数个数              */
    const char   *return_type;   /* 返回类型（诊断用）     */
    const char   *signature;     /* 签名摘要（诊断用）     */
} syscall_entry_t;

/* ================================================================
 * X-Macro 契约定义表
 *
 * 格式：SYSCALL(id_enum, "name", param_count, "return_type", "signature")
 *
 * 【注入新条目】在此列表中追加一行
 * 【更新条目】修改对应行的字段
 * 【删除条目】注释掉对应行
 * ================================================================ */

/* 对外导出的契约表 + 大小（静态生成，const 放 Flash） */
extern const syscall_entry_t g_syscall_table[];
extern const size_t          g_syscall_table_size;

/* ================================================================
 * 查询接口
 * ================================================================ */

/* 按 ID 查找，未找到返回 NULL */
const syscall_entry_t *syscall_lookup_by_id(syscall_id_t id);

/* 按名称查找，未找到返回 NULL */
const syscall_entry_t *syscall_lookup_by_name(const char *name);

/* 遍历：返回第 idx 个条目，越界返回 NULL（idx 从 0 开始） */
const syscall_entry_t *syscall_get_entry(size_t idx);

/* 返回表大小 */
size_t syscall_table_size(void);

/* ================================================================
 * 契约表展开宏
 *
 *   每个 SYSCALL(...) 行在此展开为一条 syscall_entry_t 初始化项。
 *   下方 #include "syscall_contract.def" 之前必须先定义 SYSCALL 宏。
 * ================================================================ */
#define SYSCALL(id, name, pcount, ret, sig) \
    { (id), (name), (pcount), (ret), (sig) },

/* ----------------------------------------------------------------
 *  契约表数据（静态注入区）
 *
 *  每行格式：
 *    SYSCALL(枚举ID, "名称", 参数数, "返回类型", "签名摘要")
 *
 *  签名摘要约定（紧凑表示）：
 *    u32=uint32_t  ptr=指针  i=int  v=void  str=const char*
 *    例：k_task_create(name:str,entry:ptr,arg:ptr,stack:u32,prio:u32)->ptr
 * ---------------------------------------------------------------- */
#define SYSCALL_CONTRACT_TABLE \
    /* ---- 任务管理 ---- */ \
    SYSCALL(SYS_TASK_CREATE,   "task_create",   5, "k_task_t", "name:str,entry:ptr,arg:ptr,stack:u32,prio:u32 -> ptr") \
    SYSCALL(SYS_TASK_DESTROY,  "task_destroy",  1, "k_err_t",  "task:ptr -> int") \
    SYSCALL(SYS_TASK_YIELD,    "task_yield",    0, "void",     "-> void") \
    SYSCALL(SYS_TASK_SLEEP,    "task_sleep",    1, "void",     "ticks:u32 -> void") \
    SYSCALL(SYS_TASK_SUSPEND,  "task_suspend",  1, "k_err_t",  "task:ptr -> int") \
    SYSCALL(SYS_TASK_RESUME,   "task_resume",   1, "k_err_t",  "task:ptr -> int") \
    /* ---- 内存管理 ---- */ \
    SYSCALL(SYS_KMALLOC,       "kmalloc",       1, "void*",    "size:u32 -> ptr") \
    SYSCALL(SYS_KFREE,         "kfree",         1, "void",     "ptr:ptr -> void") \
    /* ---- 控制台 ---- */ \
    SYSCALL(SYS_CONSOLE_PUTC,  "console_putc",  1, "int",      "c:i -> int") \
    SYSCALL(SYS_CONSOLE_GETC,  "console_getc",  1, "int",      "c:ptr -> int") \
    /* ---- 外设服务（条件编译） ---- */ \
    OS_CFG_PERIPH_SYSCALLS

/* PERIPH_SERVICE=1 时注入外设 syscall 契约 */
#if OS_CFG_PERIPH_SERVICE
#define OS_CFG_PERIPH_SYSCALLS \
    SYSCALL(SYS_GPIO_INIT,     "gpio_init",     3, "k_err_t",  "pin:u32,mode:u8,af:u32 -> int") \
    SYSCALL(SYS_GPIO_WRITE,    "gpio_write",    2, "void",     "pin:u32,level:u8 -> void") \
    SYSCALL(SYS_GPIO_READ,     "gpio_read",     1, "uint8_t",  "pin:u32 -> u8") \
    SYSCALL(SYS_GPIO_TOGGLE,   "gpio_toggle",   1, "void",     "pin:u32 -> void") \
    SYSCALL(SYS_SPI_XFER,      "spi_xfer",      4, "k_err_t",  "bus:u32,tx:ptr,rx:ptr,len:u32 -> int") \
    SYSCALL(SYS_I2C_TX,        "i2c_write",     4, "k_err_t",  "bus:u32,addr:u8,buf:ptr,len:u32 -> int") \
    SYSCALL(SYS_I2C_RX,        "i2c_read",      4, "k_err_t",  "bus:u32,addr:u8,buf:ptr,len:u32 -> int") \
    SYSCALL(SYS_UART_WRITE,    "uart_write",    4, "k_err_t",  "id:u32,buf:ptr,len:u32,timeout:u32 -> int") \
    SYSCALL(SYS_UART_READ,     "uart_read",     4, "k_err_t",  "id:u32,buf:ptr,len:u32,timeout:u32 -> int")
#else
#define OS_CFG_PERIPH_SYSCALLS
#endif

#endif /* SYSCALL_CONTRACT_H */
