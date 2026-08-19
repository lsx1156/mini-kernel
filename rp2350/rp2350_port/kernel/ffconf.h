/**
 * @file    ffconf.h
 * @brief   RP2350 FatFs 配置
 */

#ifndef FFCONF_H
#define FFCONF_H

/* 基础设置 */
#define FF_CODE_PAGE            437
#define FF_USE_LFN              0          /* 禁用长文件名，节省 RAM */
#define FF_MAX_LFN              255
#define FF_LFN_UNICODE          0
#define FF_STRF_ENCODE          0

/* 文件系统支持 */
#define FF_FS_FAT12             1
#define FF_FS_FAT16             1
#define FF_FS_FAT32             1
#define FF_FS_EXFAT             0

/* 功能启用 */
#define FF_FS_READONLY          0
#define FF_FS_MINIMIZE          0
#define FF_FS_RPATH             2          /* 相对路径 + 工作目录 */
#define FF_FS_REENTRANT         1          /* 线程安全 */
#define FF_FS_TIMEOUT           1000
#define FF_FS_LOCK              4

/* 扇区大小 */
#define FF_MAX_SS               512
#define FF_MIN_SS               512

/* 缓冲区 */
#define FF_FS_TINY              0          /* 使用独立缓冲区 */
#define FF_VOLUMES              1
#define FF_STR_VOLUME_ID        0
#define FF_USE_MKFS             1
#define FF_FS_NORTC             1          /* 无 RTC */
#define FF_FS_NORTC_MON         1

/* 优化 */
#define FF_FS_FASTSEEK          0
#define FF_USE_FIND             1
#define FF_USE_CHMOD            1
#define FF_USE_LABEL            1
#define FF_USE_FORWARD          0
#define FF_USE_EXPAND           0
#define FF_USE_UTF8             0

/* 内存分配 */
#define FF_MEMALLOC(ptr, size)  kmalloc(size)
#define FF_MEMFREE(ptr)         kfree(ptr)

#endif /* FFCONF_H */