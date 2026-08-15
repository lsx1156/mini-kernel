/**
 * @file    msc_blockdev.c
 * @brief   v2.2 共享块设备后端：MSC USB + FatFs 统一读写 Flash 上的 MSC 分区
 */
#include "msc_blockdev.h"
#include "hal/flash_layout.h"
#include "hal/hal_interface.h"
#include <string.h>

/* 4KB RMW 缓冲：放在 .bss，不占 kmalloc 堆。擦除/写入时保护。 */
static uint8_t s_rmw_buf[HAL_FLASH_SECTOR_SIZE] __attribute__((aligned(4)));

/* ejected 标志（true = 主机认为 U 盘已被安全删除）
 *
 * 【v2.2.4 修复 · 启动期间 HardFault 根因】
 *   旧版默认 false → USB 枚举后 Windows 立刻看到 MSC "就绪" →
 *   在 demo_app_init / fatfs_init_and_mount 尚未运行时就尝试
 *   挂载+写 Flash（创建 System Volume Information 等）→
 *   tud_msc_write10_cb 从 USBCTRL_IRQ 上下文调用 flash_range_erase →
 *   在内核数据结构未完全初始化时擦写 Flash → HardFault 爆闪。
 *
 *   修复：默认 true（ejected），Windows 看到"无介质"，不读不写。
 *   fatfs_init_and_mount 完成后保持 true（Shell 独占模式）。
 *   用户 `msc mount` 命令才设 false 让主机访问。 */
static bool s_ejected = true;

/* --------------------------------------------------------------------------
 *  LBA ↔ Flash Offset 翻译
 * -------------------------------------------------------------------------- */
static inline bool lba_valid(uint32_t lba) {
    return (lba < FLASH_LAYOUT_MSC_SECTORS);
}
static inline uint32_t lba_to_flash(uint32_t lba) {
    return FLASH_LAYOUT_MSC_OFFSET + (lba * MSC_BLOCKDEV_SECTOR_BYTES);
}

/* --------------------------------------------------------------------------
 *  对外 API
 * -------------------------------------------------------------------------- */
uint32_t msc_blockdev_sector_count(void) {
    return FLASH_LAYOUT_MSC_SECTORS;
}

bool msc_blockdev_read(uint32_t lba, uint8_t *buf) {
    return msc_blockdev_read_multi(lba, 1u, buf);
}

bool msc_blockdev_read_multi(uint32_t lba, uint32_t count, uint8_t *buf) {
    if (!buf || count == 0u) return false;
    if ((lba + count) > FLASH_LAYOUT_MSC_SECTORS) return false;
    const uint8_t *src = hal_flash_map_read(lba_to_flash(lba));
    if (!src) return false;
    memcpy(buf, src, (size_t)count * MSC_BLOCKDEV_SECTOR_BYTES);
    return true;
}

/* 单 4KB 擦-改-写回原子操作（已保证 s_rmw_buf 被正确填充 + offset 对齐） */
static bool do_rmw_flash(uint32_t aligned_offset, uint32_t byte_off_in_sector,
                         const uint8_t *data, uint32_t len) {
    /* 1. 先读回 4KB 原内容（如果是同 4KB 相邻 write_multi 连续写，
     *    本函数会被多次调用；s_rmw_buf 读一次即可，但为了简单和容错
     *    每次都重新读） */
    const uint8_t *src = hal_flash_map_read(aligned_offset);
    if (!src) return false;
    memcpy(s_rmw_buf, src, sizeof(s_rmw_buf));

    /* 2. 改我们要写的 512B（或更小段） */
    if ((byte_off_in_sector + len) > sizeof(s_rmw_buf)) return false;
    memcpy(s_rmw_buf + byte_off_in_sector, data, len);

    /* 3. 擦 4KB（Pico SDK 会自己在内部关中断/切换 RAM 执行） */
    if (hal_flash_erase_sector(aligned_offset) != HAL_OK) return false;

    /* 4. 写回 4KB（内部按 256B page 分块 program） */
    if (hal_flash_program(aligned_offset, s_rmw_buf, sizeof(s_rmw_buf)) != HAL_OK) {
        return false;
    }
    return true;
}

bool msc_blockdev_write(uint32_t lba, const uint8_t *buf) {
    return msc_blockdev_write_multi(lba, 1u, buf);
}

bool msc_blockdev_write_multi(uint32_t lba, uint32_t count, const uint8_t *buf) {
    if (!buf || count == 0u) return false;
    if ((lba + count) > FLASH_LAYOUT_MSC_SECTORS) return false;

    /* 合并 RMW：遍历每个 LBA，计算它在 4KB 中的位置；
     *   · 若与前一个 LBA 落在不同 4KB → 前一个 4KB flush 后新读
     *   · 若落在同一 4KB → 直接 memcpy 进 s_rmw_buf，暂不擦写（节省擦除）
     * 简化实现：对每个 4KB 对齐块（包含的所有 512B sectors 全部
     *   memcpy 进 s_rmw_buf 后一次性 erase+program）。
     *   算法：按 4KB 擦除块分组处理。 */
    uint32_t remain = count;
    uint32_t cur_lba = lba;
    const uint8_t *src = buf;

    while (remain > 0u) {
        uint32_t flash_off = lba_to_flash(cur_lba);
        uint32_t aligned_off = flash_off & ~((uint32_t)(HAL_FLASH_SECTOR_SIZE - 1u));

        /* 本组 LBA 从 cur_lba 开始，先读整块 4KB 到 s_rmw_buf */
        const uint8_t *orig = hal_flash_map_read(aligned_off);
        if (!orig) return false;
        memcpy(s_rmw_buf, orig, sizeof(s_rmw_buf));

        /* 把落入该 4KB 范围内的所有 512B sectors 都 copy 进来 */
        uint32_t first_boff_in_sector = flash_off - aligned_off;   /* 0..4095 */
        /* 第一 LBA 在本 4KB 的位置；计算本 4KB 共可容纳多少 sectors */
        uint32_t sectors_in_this_4k =
            (HAL_FLASH_SECTOR_SIZE - first_boff_in_sector) / MSC_BLOCKDEV_SECTOR_BYTES;
        if (sectors_in_this_4k > remain) sectors_in_this_4k = remain;

        for (uint32_t s = 0u; s < sectors_in_this_4k; s++) {
            uint32_t boff = first_boff_in_sector + s * MSC_BLOCKDEV_SECTOR_BYTES;
            memcpy(s_rmw_buf + boff, src + s * MSC_BLOCKDEV_SECTOR_BYTES,
                   MSC_BLOCKDEV_SECTOR_BYTES);
        }

        /* 擦+写 4KB */
        if (hal_flash_erase_sector(aligned_off) != HAL_OK) return false;
        if (hal_flash_program(aligned_off, s_rmw_buf, sizeof(s_rmw_buf)) != HAL_OK) {
            return false;
        }

        src    += sectors_in_this_4k * MSC_BLOCKDEV_SECTOR_BYTES;
        cur_lba += sectors_in_this_4k;
        remain -= sectors_in_this_4k;
    }
    return true;
}

void msc_blockdev_set_ejected(bool ejected) {
    s_ejected = ejected;
}
bool msc_blockdev_is_ejected(void) {
    return s_ejected;
}

bool msc_blockdev_host_write_allowed(void) {
    return !s_ejected;
}

bool msc_blockdev_is_blank(void) {
    const uint8_t *p = hal_flash_map_read(FLASH_LAYOUT_MSC_OFFSET);
    if (!p) return false;
    /* 典型非空状态：LBA 0 = Boot Sector（FAT16 的 BPB），第 0 字节是 0xEB
     * 或 0xE9（跳转指令），或第 510-511 字节是 0x55 0xAA 标志。
     * 简化检测：若前 16 字节不全是 0xFF，就认为"已有数据，不是空片"。 */
    for (uint32_t i = 0; i < 16u; i++) {
        if (p[i] != 0xFFu) return false;
    }
    /* 再检查 FAT16 最后 2 字节签名（如果有） */
    if (p[510u] != 0xFFu || p[511u] != 0xFFu) return false;
    return true;
}
