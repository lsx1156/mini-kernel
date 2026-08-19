/**
 * @file    hal_port.h
 * @brief   RP2350 移植层统一对外头文件
 */

#ifndef HAL_PORT_H
#define HAL_PORT_H

#include "hal_interface.h"
#include "os_config.h"
#include "rp2350_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Flash 布局 (RP2350: 4MB Flash, 520KB RAM)
 * ================================================================ */

#define HAL_FLASH_SECTOR_SIZE        4096   /* 4KB 扇区 */
#define HAL_FLASH_TOTAL_SIZE         (4 * 1024 * 1024)  /* 4MB */

/* 固件区：0x10000000 - 0x10200000 (2MB for kernel + app) */
#define HAL_FLASH_FIRMWARE_OFFSET    0x00000000
#define HAL_FLASH_FIRMWARE_SIZE      (2 * 1024 * 1024)  /* 2MB */

/* MSC 数据盘：0x10200000 - 0x103FE000 (2014 KB) */
#define HAL_FLASH_MSC_OFFSET         0x00200000
#define HAL_FLASH_MSC_SIZE           (2014 * 1024)

/* Bootscript A/B (双备份)：0x103FE000 - 0x10400000 (8 KB) */
#define HAL_FLASH_BOOTSCRIPT_A_OFFSET 0x003FE000
#define HAL_FLASH_BOOTSCRIPT_B_OFFSET 0x003FF000
#define HAL_FLASH_BOOTSCRIPT_SIZE     4096  /* 4KB */

/* ================================================================
 * RAM 布局 (520KB = 532480 bytes)
 * ================================================================ */

#define HAL_RAM_TOTAL_SIZE           (520 * 1024)
#define HAL_RAM_BASE                 0x20000000

/* Core 0: 0x20000000 - 0x20060000 (384KB) */
#define HAL_CORE0_RAM_OFFSET         0x00000000
#define HAL_CORE0_RAM_SIZE           (384 * 1024)

/* 共享内存 IPC: 0x20060000 - 0x20062000 (8KB) */
#define HAL_SHARED_RAM_OFFSET        0x00060000
#define HAL_SHARED_RAM_SIZE          (8 * 1024)

/* Core 1: 0x20062000 - 0x20082000 (128KB) */
#define HAL_CORE1_RAM_OFFSET         0x00062000
#define HAL_CORE1_RAM_SIZE           (128 * 1024)

/* ================================================================
 * Bootscript 配置
 * ================================================================ */

#define HAL_BOOTSCRIPT_MAX_CMDS      32
#define HAL_BOOTSCRIPT_MAX_CMD_LEN   123
#define HAL_BOOTSCRIPT_SLOTS         2

/* ================================================================
 * MSC 配置
 * ================================================================ */

#define HAL_MSC_BLOCK_SIZE           512
#define HAL_MSC_NUM_BLOCKS           (HAL_FLASH_MSC_SIZE / HAL_MSC_BLOCK_SIZE)

/* ================================================================
 * 外部符号声明 (由对应 .c 定义)
 * ================================================================ */

/* msc_blockdev.c */
extern const msc_blockdev_ops_t msc_blockdev_ops;

/* msc_usb.c */
extern const tusb_desc_device_t *msc_usb_get_device_descriptor(void);
extern const char **msc_usb_get_string_descriptors(void);
extern const uint8_t *msc_usb_get_config_descriptor(void);

/* config_store.c */
hal_err_t config_store_init(void);
hal_err_t config_store_load(void);
hal_err_t config_store_save(void);
hal_err_t config_store_get(const char *key, char *value, size_t max_len);
hal_err_t config_store_set(const char *key, const char *value);

/* bootscript.c */
hal_err_t bootscript_init(void);
hal_err_t bootscript_add_cmd(const char *cmd);
hal_err_t bootscript_del_cmd(uint8_t slot);
hal_err_t bootscript_save(void);
hal_err_t bootscript_load(void);
int bootscript_count(void);
hal_err_t bootscript_get_cmd(uint8_t slot, char *buf, size_t max_len);
void bootscript_run_all(void);

/* diskio.c */
DSTATUS disk_initialize(BYTE pdrv);
DSTATUS disk_status(BYTE pdrv);
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count);
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count);
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff);

/* shell_fs.c */
void shell_fs_register(void);

/* shell_mcore.c */
void shell_mcore_register(void);

/* shell_ovclk.c */
void shell_ovclk_register(void);

/* shell_ipc.c */
void shell_ipc_register(void);

/* plc_core.c */
void plc_core_init(void);

/* shmem_ipc.c */
void shmem_ipc_init(void);
hal_err_t shmem_ipc_send(uint32_t core_id, const void *data, size_t len);
hal_err_t shmem_ipc_recv(uint32_t core_id, void *data, size_t max_len, size_t *out_len);

/* core1_worker.c */
void core1_worker_entry(void *arg);

/* hal_port.c 已导出的接口 */
extern const hal_export_t hal_export;

/* ================================================================
 * 初始化顺序
 * ================================================================ */

static inline void hal_port_modules_init(void) {
    /* 1. 基础硬件 */
    config_store_init();
    bootscript_init();
    
    /* 2. 存储/文件系统 */
    shmem_ipc_init();
    
    /* 3. Shell 扩展命令 */
    shell_fs_register();
    shell_mcore_register();
    shell_ovclk_register();
    shell_ipc_register();
    
    /* 4. PLC */
    plc_core_init();
}

#ifdef __cplusplus
}
#endif

#endif /* HAL_PORT_H */