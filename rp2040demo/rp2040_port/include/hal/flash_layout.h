/**
 * @file    flash_layout.h
 * @brief   v2.4 W25Q16JV (2MiB) Flash 分区定义 — 固件 + MSC U 盘 + Config + Bootscript
 *
 *  分区规则（绝对不要重叠，重叠 = 要么代码损坏要么文件系统损坏）：
 *
 *  ┌───────────────────────────────────────────────────────────────┐  0x00200000 (2MiB)
 *  │  Bootscript SEC_B (4 KiB)                  0x001FF000 ↑      │
 *  ├───────────────────────────────────────────────────────────────┤  0x001FF000
 *  │  Bootscript SEC_A (4 KiB)                  0x001FE000 ↑      │
 *  ├───────────────────────────────────────────────────────────────┤  0x001FE000
 *  │  Config 固化区 (4 KiB) — v2.4 超频档位 / 多核标志               │
 *  │  0x001FD000 → 0x001FDFFF                                      │
 *  ├───────────────────────────────────────────────────────────────┤  0x001FD000
 *  │                                                               │
 *  │  MSC 数据盘 (FAT16) — 电脑读写的 U 盘介质                     │
 *  │  1,036,288 Bytes = 2024 × 512B/sector                         │
 *  │  0x00100000 → 0x001FCFFF                                      │
 *  │                                                               │
 *  ├───────────────────────────────────────────────────────────────┤  0x00100000 (1 MiB)
 *  │                                                               │
 *  │  Firmware 代码区 — XIP 执行 mini-kernel + demo + shell         │
 *  │  1 MiB (足够放 ~195 KB 当前固件，未来翻倍也够)                  │
 *  │  0x00000000 → 0x000FFFFF                                      │
 *  │                                                               │
 *  └───────────────────────────────────────────────────────────────┘  0x00000000
 *
 *  地址说明：
 *    · 本文件所有 offset 都是相对 W25Q16 芯片的"Flash 偏移"，不是 XIP 基址。
 *      XIP 映射在 RP2040 上是 0x10000000 + offset，所以：
 *          XIP_read(addr) = *((volatile uint8_t*)(0x10000000 + offset))
 *    · hal_flash_*() 系列函数（erase_sector / program / map_read）的参数
 *      也是这个"相对 Flash offset"，所以直接用下面的宏传入即可。
 */
#ifndef MINIK_FLASH_LAYOUT_H
#define MINIK_FLASH_LAYOUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Flash 芯片总大小（W25Q16JV = 2 MiB） ---------- */
#ifndef FLASH_LAYOUT_TOTAL_BYTES
#  define FLASH_LAYOUT_TOTAL_BYTES    (2u * 1024u * 1024u)
#endif

/* ---------- 代码固件区：前 1 MiB ----------
 *   · 不做任何运行时擦除（除非将来做 OTA A/B 分区，当前 v2.2 没有）
 *   · uf2 烧录脚本只写这个范围 */
#define FLASH_LAYOUT_FW_OFFSET         (0x00000000u)
#define FLASH_LAYOUT_FW_BYTES          (1024u * 1024u)       /* 1 MiB */

/* ---------- MSC 数据盘：2024 × 512 B = 1,036,288 B ----------
 *   · Windows/Mac/Linux 插入 USB 后直接作为"可移动磁盘"显示
 *   · 真实 FAT16 文件系统（FatFs + f_mkfs 自动生成 BPB/FAT/RootDir）
 *   · 支持创建子目录、任意拷贝文件（断电保留）
 *   · 尾部预留 4 KiB 给 Config 固化区，Config 之后恰好是 bootscript SEC_A */
#define FLASH_LAYOUT_MSC_OFFSET        (FLASH_LAYOUT_FW_OFFSET + FLASH_LAYOUT_FW_BYTES)
#define FLASH_LAYOUT_MSC_SECTOR_BYTES  (512u)                 /* 兼容 USB MSC 官方标准 */
#define FLASH_LAYOUT_MSC_SECTORS       (2024u)
#define FLASH_LAYOUT_MSC_BYTES         (FLASH_LAYOUT_MSC_SECTORS * FLASH_LAYOUT_MSC_SECTOR_BYTES)
                                                              /* = 1,036,288 B */
#define FLASH_LAYOUT_CONFIG_BYTES      (4096u)                /* 固化区 = 1 扇区 */
#define FLASH_LAYOUT_CONFIG_OFFSET     (FLASH_LAYOUT_MSC_OFFSET + FLASH_LAYOUT_MSC_BYTES)
                                                              /* = 0x001FD000 */

/* ---------- Bootscript 固化区：最后 2 × 4 KiB ----------
 *   · 与 bootscript.c 的 BOOTSCRIPT_SECTOR_A / _B 计算完全一致，
 *     只是在此集中声明方便核对不重叠。
 *   · SEC_A = 2MiB - 2*4KB = 0x1FE000
 *     SEC_B = 2MiB - 1*4KB = 0x1FF000 */
#define FLASH_LAYOUT_BOOTSCRIPT_A      (FLASH_LAYOUT_TOTAL_BYTES - 2u * 4096u)
#define FLASH_LAYOUT_BOOTSCRIPT_B      (FLASH_LAYOUT_TOTAL_BYTES - 1u * 4096u)

/* ---------- 编译期断言：检查所有边界严丝合缝 ---------- */
#if (FLASH_LAYOUT_CONFIG_OFFSET + FLASH_LAYOUT_CONFIG_BYTES != FLASH_LAYOUT_BOOTSCRIPT_A)
#error "Config region end must touch bootscript SEC_A start exactly — no gap, no overlap"
#endif
#if (FLASH_LAYOUT_BOOTSCRIPT_A + 4096u != FLASH_LAYOUT_BOOTSCRIPT_B)
#error "Bootscript SEC_A must be immediately followed by SEC_B (4096 B each)"
#endif
#if (FLASH_LAYOUT_BOOTSCRIPT_B + 4096u != FLASH_LAYOUT_TOTAL_BYTES)
#error "Bootscript SEC_B + 4096B must equal end of flash"
#endif
#if ((FLASH_LAYOUT_MSC_OFFSET & 4095u) != 0u)
#error "MSC region start must be 4 KiB aligned (W25Q sector erase boundary)"
#endif
#if ((FLASH_LAYOUT_MSC_BYTES & 4095u) != 0u)
#error "MSC region size must be multiple of 4 KiB (W25Q erase sector granularity)"
#endif
#if ((FLASH_LAYOUT_CONFIG_OFFSET & 4095u) != 0u)
#error "Config region start must be 4 KiB aligned (W25Q sector erase boundary)"
#endif

#ifdef __cplusplus
}
#endif

#endif /* MINIK_FLASH_LAYOUT_H */
