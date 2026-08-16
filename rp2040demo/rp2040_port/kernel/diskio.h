/**
 * @file    diskio.h
 * @brief   FatFs disk I/O 层头文件（与 Elm FatFs 官方结构一致，避免循环依赖）
 *
 *  【依赖顺序注意】
 *  ff.h 的内部 include 顺序是：include "ffconf.h" → typedef BYTE/WORD/DWORD/LBA_t/UINT/TCHAR
 *                         → 然后才 include "diskio.h"。
 *  所以本文件**绝对不要反向 #include "ff.h"**，否则会造成：
 *    diskio.h → ff.h → ffconf.h → diskio.h（header guard 生效但 BYTE 还没定义）
 *    → 一堆 "unknown type name DSTATUS" 编译错误。
 */
#ifndef _DISKIO_DEFINED
#define _DISKIO_DEFINED

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 磁盘状态（FatFs 标准：typedef BYTE DSTATUS）---- */
typedef BYTE	DSTATUS;

/* ---- Results of Disk Functions（枚举）---- */
typedef enum {
	RES_OK = 0,		/* 0: Successful */
	RES_ERROR,		/* 1: R/W Error */
	RES_WRPRT,		/* 2: Write Protected */
	RES_NOTRDY,		/* 3: Not Ready */
	RES_PARERR		/* 4: Invalid Parameter */
} DRESULT;

/* ---- Disk Status Bits（DSTATUS 位域）---- */
#define STA_NOINIT		0x01	/* Drive not initialized */
#define STA_NODISK		0x02	/* No medium in the drive */
#define STA_PROTECT		0x04	/* Write protected */

/* ---- Command code for disk_ioctrl function ---- */
#define CTRL_SYNC			0	/* Complete pending write process (needed at FF_FS_READONLY == 0) */
#define GET_SECTOR_COUNT	1	/* Get media size (needed at FF_USE_MKFS == 1) */
#define GET_SECTOR_SIZE		2	/* Get sector size (needed at FF_MAX_SS != FF_MIN_SS) */
#define GET_BLOCK_SIZE		3	/* Get erase block size (needed at FF_USE_MKFS == 1) */
#define CTRL_TRIM			4	/* Inform device that the data on the block of sectors is no longer needed */

/* ---- FatFs disk_* 接口原型 ---- */
DSTATUS disk_initialize (BYTE pdrv);
DSTATUS disk_status     (BYTE pdrv);
DRESULT disk_read       (BYTE pdrv, BYTE* buff, LBA_t sector, UINT count);
DRESULT disk_write      (BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count);
DRESULT disk_ioctl      (BYTE pdrv, BYTE cmd, void* buff);

/* FatFs 时间戳接口（FF_FS_NORTC=1，get_fattime 提供占位函数） */
DWORD get_fattime(void);

#ifdef __cplusplus
}
#endif

#endif /* _DISKIO_DEFINED */
