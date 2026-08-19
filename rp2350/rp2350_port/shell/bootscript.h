/**
 * @file    bootscript.h
 * @brief   RP2350 开机脚本持久化接口
 */

#ifndef BOOTSCRIPT_H
#define BOOTSCRIPT_H

#include "hal_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_BOOTSCRIPT_MAX_CMDS     32
#define HAL_BOOTSCRIPT_MAX_CMD_LEN  123

hal_err_t bootscript_init(void);
hal_err_t bootscript_load(void);
hal_err_t bootscript_save(void);
int bootscript_count(void);
hal_err_t bootscript_add_cmd(const char *cmd);
hal_err_t bootscript_del_cmd(uint8_t slot);
hal_err_t bootscript_get_cmd(uint8_t slot, char *buf, size_t max_len);
void bootscript_run_all(void);
hal_err_t bootscript_set_dirty(void);

#ifdef __cplusplus
}
#endif

#endif /* BOOTSCRIPT_H */