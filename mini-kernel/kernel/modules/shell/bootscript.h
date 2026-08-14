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

#endif /* _BOOTSCRIPT_H_ */
