/**
 * @file    fatfs_api.h
 * @brief   FatFs 对外统一入口：初始化 + Shell 文件系统命令调用
 *
 *  Shell 文件在 shell_fs.c 中使用。我们不把 fatfs_init_and_mount 原型散在各处。
 */
#ifndef FATFS_API_H
#define FATFS_API_H

#include <stdbool.h>
#include "ff.h"      /* 引入 FATFS / FRESULT 等 FatFs 类型 */

#ifdef __cplusplus
extern "C" {
#endif

/** 挂载文件系统（如空片则自动 f_mkfs FAT16）；FatFs 标准返回码 */
FRESULT fatfs_init_and_mount(void);
/** 是否已挂载（Shell banner / msc status 打印用） */
bool    fatfs_is_mounted(void);
/** 本次启动是否做过 f_mkfs（首启动诊断） */
bool    fatfs_mkfs_done_this_boot(void);

/** 获取 FatFs 内部 FATFS 对象指针（用于 f_mount / f_opendir 等） */
FATFS * fatfs_get_obj(void);

/** 尝试进入写模式：要求当前 ejected=true（防止 Shell 写与 USB 主机竞争） */
bool    fatfs_try_enter_write_mode(void);
/** 进入读模式：任何时候都 OK（返回 true，占位函数可读性更好） */
bool    fatfs_enter_read_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* FATFS_API_H */
