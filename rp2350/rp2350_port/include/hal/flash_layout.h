/**
 * @file    flash_layout.h
 * @brief   RP2350 Flash 分区布局定义
 * 
 * W25Q32JV (4 MiB = 0x400000 = 4,194,304 B)
 * 
 * 分区设计原则：
 * 1. 固件区与数据区物理隔离，USB MSC 写入绝不触碰固件
 * 2. 双备份 Bootscript 扇区，CRC8 校验，断电自恢复
 * 3. Core 0/1 镜像区分离，支持双核 OTA
 * 4. 共享 IPC 区域固定偏移，便于双核通信
 */

#ifndef FLASH_LAYOUT_H
#define FLASH_LAYOUT_H

#include "os_config.h"

/* ================================================================
 * Flash 几何参数
 * ================================================================ */

#define FLASH_TOTAL_SIZE              (4 * 1024 * 1024)    /* 4 MB */
#define FLASH_SECTOR_SIZE             4096                  /* 4 KB 扇区 */
#define FLASH_PAGE_SIZE               256                   /* 256 B 页 */
#define FLASH_NUM_SECTORS             (FLASH_TOTAL_SIZE / FLASH_SECTOR_SIZE)  /* 1024 扇区 */

/* XIP 基址 (RP2350) */
#define FLASH_XIP_BASE                0x10000000

/* ================================================================
 * 分区定义 (偏移量均相对于 Flash 起始 0x10000000)
 * ================================================================ */

/* ------------------------------------------------------------------
 * [0] 固件区 Firmware: 0x00000000 - 0x001FFFFF (2 MiB = 512 扇区)
 *   存放：boot2 + kernel_core + rp2350_port + demo_app + shell
 *   受保护：USB MSC 绝不写入此区
 * ------------------------------------------------------------------ */
#define PART_FIRMWARE_OFFSET          0x00000000
#define PART_FIRMWARE_SIZE            (2 * 1024 * 1024)     /* 2 MB */
#define PART_FIRMWARE_SECTORS         (PART_FIRMWARE_SIZE / FLASH_SECTOR_SIZE)  /* 512 */

/* ------------------------------------------------------------------
 * [1] MSC 数据盘 Mass Storage: 0x00200000 - 0x003FDFFF (≈2014 KiB)
 *   FatFs FAT16 分区，作为 U 盘暴露给 PC
 *   Shell 写入时需先 msc eject (ejected=true)
 * ------------------------------------------------------------------ */
#define PART_MSC_OFFSET               0x00200000
#define PART_MSC_SIZE                 (2014 * 1024)         /* 2014 KiB = 4028 扇区 */
#define PART_MSC_SECTORS              (PART_MSC_SIZE / FLASH_SECTOR_SIZE)

/* ------------------------------------------------------------------
 * [2] Bootscript A (主): 0x003FE000 - 0x003FEFFF (4 KB = 1 扇区)
 *   固化指令槽位 0..31 (双备份主)
 * ------------------------------------------------------------------ */
#define PART_BOOTSCRIPT_A_OFFSET      0x003FE000
#define PART_BOOTSCRIPT_A_SIZE        FLASH_SECTOR_SIZE     /* 4 KB */

/* ------------------------------------------------------------------
 * [3] Bootscript B (副): 0x003FF000 - 0x003FFFFF (4 KB = 1 扇区)
 *   固化指令槽位 0..31 (双备份副 / 对比恢复)
 * ------------------------------------------------------------------ */
#define PART_BOOTSCRIPT_B_OFFSET      0x003FF000
#define PART_BOOTSCRIPT_B_SIZE        FLASH_SECTOR_SIZE     /* 4 KB */

/* ------------------------------------------------------------------
 * [4] Core 1 镜像备份区: 0x003F0000 - 0x003FDFFF (60 KB = 15 扇区)
 *   可选：Core 1 固件镜像备份，支持双核独立 OTA
 * ------------------------------------------------------------------ */
#define PART_CORE1_BACKUP_OFFSET      0x003F0000
#define PART_CORE1_BACKUP_SIZE        (60 * 1024)

/* ================================================================
 * 便利宏：分区起始绝对地址 (XIP 空间)
 * ================================================================ */

#define FLASH_FIRMWARE_START          (FLASH_XIP_BASE + PART_FIRMWARE_OFFSET)
#define FLASH_MSC_START               (FLASH_XIP_BASE + PART_MSC_OFFSET)
#define FLASH_BOOTSCRIPT_A_START      (FLASH_XIP_BASE + PART_BOOTSCRIPT_A_OFFSET)
#define FLASH_BOOTSCRIPT_B_START      (FLASH_XIP_BASE + PART_BOOTSCRIPT_B_OFFSET)
#define FLASH_CORE1_BACKUP_START      (FLASH_XIP_BASE + PART_CORE1_BACKUP_OFFSET)

/* ================================================================
 * 校验与对齐断言 (编译期检查)
 * ================================================================ */

#if (PART_FIRMWARE_OFFSET % FLASH_SECTOR_SIZE) != 0
#error "Firmware partition not sector-aligned"
#endif

#if (PART_MSC_OFFSET % FLASH_SECTOR_SIZE) != 0
#error "MSC partition not sector-aligned"
#endif

#if (PART_BOOTSCRIPT_A_OFFSET % FLASH_SECTOR_SIZE) != 0
#error "Bootscript A not sector-aligned"
#endif

#if (PART_BOOTSCRIPT_B_OFFSET % FLASH_SECTOR_SIZE) != 0
#error "Bootscript B not sector-aligned"
#endif

/* 总大小检查 */
#if (PART_FIRMWARE_OFFSET + PART_FIRMWARE_SIZE \
   + PART_MSC_SIZE \
   + PART_BOOTSCRIPT_A_SIZE \
   + PART_BOOTSCRIPT_B_SIZE \
   + PART_CORE1_BACKUP_SIZE) > FLASH_TOTAL_SIZE
#error "Partitions exceed Flash size"
#endif

/* ================================================================
 * RAM 布局对应 (供链接脚本/启动代码参考)
 * ================================================================ */

#define RAM_TOTAL_SIZE                (520 * 1024)          /* 520 KB */
#define RAM_BASE                      0x20000000

/* Core 0: 0x20000000 - 0x2005FFFF (384 KB) */
#define RAM_CORE0_OFFSET              0x00000000
#define RAM_CORE0_SIZE                (384 * 1024)

/* Shared IPC: 0x20060000 - 0x20061FFF (8 KB) */
#define RAM_SHARED_IPC_OFFSET         0x00060000
#define RAM_SHARED_IPC_SIZE           (8 * 1024)

/* Core 1: 0x20062000 - 0x20081FFF (128 KB) */
#define RAM_CORE1_OFFSET              0x00062000
#define RAM_CORE1_SIZE                (128 * 1024)

/* ================================================================
 * 外部符号 (链接器脚本定义)
 * ================================================================ */

extern char _estack;                    /* Core 0 栈顶 */
extern char _core1_stack_top;           /* Core 1 栈顶 */
extern char _shared_ram_start;          /* 共享 IPC 起始 */
extern char _shared_ram_end;            /* 共享 IPC 结束 */
extern char _ipc_ctrl;                  /* IPC 控制结构 */
extern char _ipc_in_buf[];              /* Core0→Core1 缓冲区 */
extern char _ipc_out_buf[];             /* Core1→Core0 缓冲区 */

#endif /* FLASH_LAYOUT_H */