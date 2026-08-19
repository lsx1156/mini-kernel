/**
 * @file    msc_blockdev.h
 * @brief   MSC Block Device 接口 (FatFs diskio 回调)
 */

#ifndef MSC_BLOCKDEV_H
#define MSC_BLOCKDEV_H

#include "hal_interface.h"
#include "ff.h"  /* FatFs 类型 */

#ifdef __cplusplus
extern "C" {
#endif

/* FatFs diskio 回调函数 */
DSTATUS disk_initialize(BYTE pdrv);
DSTATUS disk_status(BYTE pdrv);
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count);
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count);
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff);

/* MSC 状态控制 */
extern bool g_msc_ejected;
void msc_blockdev_set_ejected(bool ejected);

#ifdef __cplusplus
}
#endif

#endif /* MSC_BLOCKDEV_H */