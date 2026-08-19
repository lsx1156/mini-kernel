/**
 * @file    msc_blockdev.c
 * @brief   RP2350 MSC Block Device (USB Mass Storage 后端)
 * 
 * 读写 Flash MSC 分区 (PART_MSC_OFFSET ~ PART_MSC_OFFSET+PART_MSC_SIZE)
 * 4KB 扇区 RMW (读-改-写) 适配 512B 逻辑块
 */

#include "hal_port.h"
#include "flash_layout.h"
#include <string.h>

#define MSC_BDEV_BLOCK_SIZE          512
#define MSC_BDEV_NUM_BLOCKS          (PART_MSC_SIZE / MSC_BDEV_BLOCK_SIZE)   /* 4028 */
#define MSC_BDEV_FLASH_SECTOR_SIZE   FLASH_SECTOR_SIZE                        /* 4096 */
#define MSC_BDEV_BLOCKS_PER_SECTOR   (MSC_BDEV_FLASH_SECTOR_SIZE / MSC_BDEV_BLOCK_SIZE)  /* 8 */

static uint8_t g_sector_buf[MSC_BDEV_FLASH_SECTOR_SIZE];
static uint32_t g_cached_sector = 0xFFFFFFFF;
static bool g_sector_dirty = false;

static inline uint32_t block_to_flash_addr(uint32_t block) {
    return FLASH_MSC_START + block * MSC_BDEV_BLOCK_SIZE;
}

static inline uint32_t block_to_sector(uint32_t block) {
    return block / MSC_BDEV_BLOCKS_PER_SECTOR;
}

static inline uint32_t sector_to_flash_addr(uint32_t sector) {
    return FLASH_MSC_START + sector * MSC_BDEV_FLASH_SECTOR_SIZE;
}

static hal_err_t flush_sector(void) {
    if (!g_sector_dirty) return HAL_OK;
    
    uint32_t flash_addr = sector_to_flash_addr(g_cached_sector);
    flash_range_erase(flash_addr, MSC_BDEV_FLASH_SECTOR_SIZE);
    flash_range_program(flash_addr, g_sector_buf, MSC_BDEV_FLASH_SECTOR_SIZE);
    g_sector_dirty = false;
    return HAL_OK;
}

static hal_err_t load_sector(uint32_t sector) {
    if (g_cached_sector == sector) return HAL_OK;
    
    hal_err_t err = flush_sector();
    if (err != HAL_OK) return err;
    
    uint32_t flash_addr = sector_to_flash_addr(sector);
    const uint8_t *src = (const uint8_t *)flash_addr;
    memcpy(g_sector_buf, src, MSC_BDEV_FLASH_SECTOR_SIZE);
    g_cached_sector = sector;
    return HAL_OK;
}

DSTATUS disk_initialize(BYTE pdrv) {
    (void)pdrv;
    g_cached_sector = 0xFFFFFFFF;
    g_sector_dirty = false;
    return RES_OK;
}

DSTATUS disk_status(BYTE pdrv) {
    (void)pdrv;
    return RES_OK;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    if (!buff || count == 0) return RES_PARERR;
    if (sector + count > MSC_BDEV_NUM_BLOCKS) return RES_PARERR;
    
    for (UINT i = 0; i < count; i++) {
        uint32_t block = sector + i;
        uint32_t sec = block_to_sector(block);
        uint32_t offset = (block % MSC_BDEV_BLOCKS_PER_SECTOR) * MSC_BDEV_BLOCK_SIZE;
        
        hal_err_t err = load_sector(sec);
        if (err != HAL_OK) return RES_ERROR;
        
        memcpy(buff + i * MSC_BDEV_BLOCK_SIZE, g_sector_buf + offset, MSC_BDEV_BLOCK_SIZE);
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    if (!buff || count == 0) return RES_PARERR;
    if (sector + count > MSC_BDEV_NUM_BLOCKS) return RES_PARERR;
    
    /* USB MSC 写入保护：需 ejected 状态 */
    extern bool g_msc_ejected;
    if (!g_msc_ejected) return RES_WRPRT;
    
    for (UINT i = 0; i < count; i++) {
        uint32_t block = sector + i;
        uint32_t sec = block_to_sector(block);
        uint32_t offset = (block % MSC_BDEV_BLOCKS_PER_SECTOR) * MSC_BDEV_BLOCK_SIZE;
        
        hal_err_t err = load_sector(sec);
        if (err != HAL_OK) return RES_ERROR;
        
        memcpy(g_sector_buf + offset, buff + i * MSC_BDEV_BLOCK_SIZE, MSC_BDEV_BLOCK_SIZE);
        g_sector_dirty = true;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    (void)pdrv;
    switch (cmd) {
        case CTRL_SYNC:
            return flush_sector() == HAL_OK ? RES_OK : RES_ERROR;
        case GET_SECTOR_COUNT:
            *(DWORD *)buff = MSC_BDEV_NUM_BLOCKS;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD *)buff = MSC_BDEV_BLOCK_SIZE;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *(DWORD *)buff = MSC_BDEV_BLOCKS_PER_SECTOR;
            return RES_OK;
        default:
            return RES_PARERR;
    }
}