#ifndef _BOOTSCRIPT_H_
#define _BOOTSCRIPT_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "hal_interface.h"   /* hal_err_t */

/* 返回当前已固化命令条数（0~31） */
uint8_t  bootscript_count(void);

/* 读取第 idx 条（从 0 开始）已固化命令，写入 buf（必须 ≥ 124 B，保证 '\0'）。
 *   返回 true = 成功。 */
bool     bootscript_get(uint8_t idx, char *buf, size_t buf_size);

/* 追加一条新命令 cmd_line 到末尾，同步写入 A/B 双备份。
 *   cmd_line 字节数 1..123，返回 HAL_OK / HAL_ERR_NOMEM / HAL_ERR_FULL / HAL_ERR_IO */
hal_err_t bootscript_append(const char *cmd_line);

/* 删除第 idx 条命令（后面条目前移），同步重写 A/B 双备份。 */
hal_err_t bootscript_remove(uint8_t idx);

/* 清空所有已固化命令（整扇区 A/B 重写为空扇区头） */
hal_err_t bootscript_clear_all(void);

/* B 线路验证辅助 */
hal_err_t bootscript_erase_test(void);
bool     bootscript_verify(void);

/* 启动诊断：打印 Sector A/B magic/valid/count/crc。
 *   bootscript_run_all 在 count==0 但用户确认"save 过"时自动调用。 */
void     bootscript_diag_dump(void);

/* ================================================================
 * 持久化回放结果（RAM 常驻，解决 PICO_STDIO_USB_STDOUT_TIMEOUT_US=0
 * 导致 bootscript_run_all 启动时输出被丢、用户看不到的问题）：
 *
 *   用户打开终端后输入 boot status 即可查看上次回放的每条命令
 *   是否执行、返回码、当时 GPIO25 电平等 —— 不依赖启动时的即时串口输出。
 * ================================================================ */
#define BOOTSCRIPT_LOG_MAX_ENTRIES   32u   /* 与 MAX_ENTRIES 对齐 */

typedef struct {
    uint8_t  slot;            /* 0..31 原固化序号 */
    int      exec_rc;         /* shell_exec_line 返回值（0=OK 其他=失败）*/
    char     cmd_line[124];   /* 实际执行的命令行（含 '\0'） */
} bootscript_log_entry_t;

typedef struct {
    bool                      ran;                 /* true = bootscript_run_all 被调用过（本次上电） */
    uint8_t                   total;               /* 原固化总条数 */
    uint8_t                   ok_count;            /* 执行成功数 */
    uint8_t                   fail_count;          /* 执行失败数 */
    uint8_t                   final_gpio25_level;  /* 回放结束后 GPIO25 电平（0/1，0xFF 未知） */
    bootscript_log_entry_t    entries[BOOTSCRIPT_LOG_MAX_ENTRIES];
} bootscript_status_t;

/* 获取 RAM 中上次回放的结果（从未运行过返回 ran=false）*/
const bootscript_status_t *bootscript_get_status(void);

/* —— 仅 shell.c bootscript_run_all 调用：写入回放结果到 RAM —— */
void bootscript_rec_begin(uint8_t total_slots);
void bootscript_rec_entry(uint8_t slot, const char *cmd_line, int exec_rc);
void bootscript_rec_end(uint8_t ok_cnt, uint8_t fail_cnt, uint8_t final_gpio25_level_01_or_FF);

#endif /* _BOOTSCRIPT_H_ */
