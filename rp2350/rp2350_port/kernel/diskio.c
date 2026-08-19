/**
 * @file    diskio.c
 * @brief   RP2350 FatFs 底层磁盘 I/O (diskio.c)
 * 
 * 对接 msc_blockdev.c 的 DSTATUS/DRESULT 接口
 */

#include "hal_port.h"
#include "msc_blockdev.h"
#include "diskio.h"
#include <ff.h>

/* FatFs 磁盘驱动状态 */
static DSTATUS Stat = STA_NOINIT;

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    Stat = disk_initialize(pdrv);
    return Stat;
}

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    return Stat;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;
    return disk_read(pdrv, buff, sector, count);
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;
    if (Stat & STA_PROTECT) return RES_WRPRT;
    return disk_write(pdrv, buff, sector, count);
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != 0) return RES_PARERR;
    return disk_ioctl(pdrv, cmd, buff);
}

/* 获取时间戳 (FatFs 用于文件创建/修改时间) */
DWORD get_fattime(void) {
    /* 简化：返回固定时间 2024-01-01 00:00:00 */
    return ((2024 - 1980) << 25) | (1 << 21) | (1 << 16);
}