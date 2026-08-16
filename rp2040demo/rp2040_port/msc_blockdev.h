/**
 * @file    msc_blockdev.h
 * @brief   v2.2 共享"块设备"后端：TinyUSB MSC 和 FatFs 都从这里读/写扇区
 *
 *  【关键架构】MSC（主机 USB 读写 FAT16 扇区）和 FatFs（本机 f_open/f_read 读写文件）
 *  必须操作同一套"Flash ↔ LBA 翻译"。如果两边各自写自己的代码，非常容易出现：
 *    · 一边用 0x100000 做基址，另一边 0 做基址 → 读到代码区
 *    · 一边不做 4KB erase→modify→program RMW，另一边做 → 文件系统撕裂
 *  因此本模块作为唯一的扇区访问入口，MSC 回调和 FatFs diskio 都调用它。
 *
 *  【块大小约束】
 *    · 对外扇区 = 512 B（USB MSC 官方标准 / FAT16 BPB 也是 512）
 *    · 对内 Flash erase/program 粒度 = 4096 B（W25Q16 最小擦除扇区）
 *    · blockdev_write() 会自动执行"读 4KB → 擦 4KB → 改 512B → 写回 4KB"
 *      的 RMW 循环，并使用 4KB 静态缓冲区（分配在 .bss，不占 kernel 堆）。
 *
 *  【ejected 状态】USB MSC 规范里 START STOP UNIT (start=0, loej=1) 会把
 *  介质标记为"已弹出"。之后主机查询 TEST UNIT READY 会得到 NOT READY，
 *  这样用户"安全删除硬件"后，我们本地 f_mount/f_mkdir 仍可以操作。
 *  用户运行命令 `msc mount` 会把 ejected 重新置 false，U 盘重新出现。
 */
#ifndef MINIK_MSC_BLOCKDEV_H
#define MINIK_MSC_BLOCKDEV_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 扇区大小（对外 USB MSC / FAT16 BPB 一致使用 512B，不要改） */
#define MSC_BLOCKDEV_SECTOR_BYTES      512u

/** 总扇区数（= FLASH_LAYOUT_MSC_SECTORS，2032 个 512B 扇区） */
uint32_t msc_blockdev_sector_count(void);

/**
 *  单扇区读取。
 *  @param  lba   逻辑扇区号（0 .. sector_count-1）
 *  @param  buf   接收缓冲区（至少 512 字节，必须 4 字节对齐即可）
 *  @return true  成功
 *          false lba 越界或 buf==NULL
 */
bool msc_blockdev_read(uint32_t lba, uint8_t *buf);

/**
 *  多扇区连续读取（MSC 128KB 大传输用，减少调用开销）。
 *  @param  lba    起始 LBA
 *  @param  count  连续扇区数（通常 1，但 USB MSC Read(10) 可能一次传多个）
 *  @param  buf    目标缓冲区（count * 512 字节）
 */
bool msc_blockdev_read_multi(uint32_t lba, uint32_t count, uint8_t *buf);

/**
 *  单扇区写入（自动处理 4KB 擦除 → 修改 → 回写）。
 *  @param  lba   逻辑扇区号
 *  @param  buf   待写入的 512 字节数据
 *  @return true  写入成功（erase + program 均 HAL_OK）
 *          false 越界、Flash I/O 错误、或 ejected 状态为 ejected（禁止写）
 *
 *  【注意】写入前必须先关 XIP 中断等，由 hal_flash_*() 在内部自己完成。
 *  调用者不需要额外操作。
 */
bool msc_blockdev_write(uint32_t lba, const uint8_t *buf);

/**
 *  多扇区连续写入（MSC Write(10) 一次 64 扇区）。
 *  内部优化：同一 4KB Flash erase sector 内的多个 512B 写入
 *  会合并做一次 erase + 读 → 改多个 512B → 一次 program，
 *  避免对同一 4KB 重复擦 7 次（明显提速 + 延长 Flash 寿命）。
 */
bool msc_blockdev_write_multi(uint32_t lba, uint32_t count, const uint8_t *buf);

/** 弹出/挂载介质标志（只影响 USB MSC 侧 SCSI 命令返回，不影响 FatFs 本地读写）。 */
void msc_blockdev_set_ejected(bool ejected);
bool msc_blockdev_is_ejected(void);

/**
 *  是否允许写（本模块内部统一检查）。
 *  ejected = false 时主机/本地都可写；ejected = true 时
 *  主机 USB MSC WRITE(10) 被拒绝（返回 CHECK CONDITION），
 *  但本机 FatFs 命令行仍然允许写（这样 `msc eject` 之后仍能用
 *  shell 命令改文件，不会锁死本地操作）。
 */
bool msc_blockdev_host_write_allowed(void);

/** 上电自检：检查 Flash 读回 0 字节全 0xFF（空白）还是已有 FAT 签名。
 *  仅用于首次上电自动 f_mkfs 判断，不改变任何介质状态。 */
bool msc_blockdev_is_blank(void);

#ifdef __cplusplus
}
#endif

#endif /* MINIK_MSC_BLOCKDEV_H */
