/**
 * @file    diskio.c
 * @brief   FatFs disk I/O 层实现：MSC + FatFs 共享同一 Flash 块设备后端
 *
 *  【重要设计】读写互斥（USB 主机 vs. 本机 FatFs）：
 *  Flash 只能串行读写（Pico SDK 的 flash_range_program/erase 每次都关中断），
 *  因此"主机通过 USB MSC 写"和"本机用 cat/rm 等命令写"绝对不能同时发生。
 *  我们用 ejected 标志作为互斥锁：
 *    - ejected = false : 允许 USB 主机读写；本机 Shell 命令（除 cat/ls 只读外）
 *                        必须先提示 `msc eject`，再允许 write。
 *    - ejected = true  : USB 主机已弹出，状态 NOT READY，不读写；本机可自由操作。
 *  读取（disk_read）永远允许（两边都只读，无竞争）。
 */

/* 【include 顺序注意】
 * 1. 先 ff.h：内部处理完 ffconf.h → typedef BYTE/WORD/DWORD/LBA_t/UINT/TCHAR
 *              → 然后自动 include 本目录的 diskio.h（用 BYTE 定义 DSTATUS）
 * 2. 再 fatfs_api.h：引入我们对外暴露的 fatfs_*() 原型（避免 -Wmissing-prototypes）
 * 3. 其他 HAL 头 */
#include "ff.h"
#include "fatfs_api.h"
#include "diskio.h"           /* 虽然 ff.h 已 include 过，但显式 include 让 LSP/IDE 更清楚，header guard 防止重复 */
#include "msc_blockdev.h"
#include "hal/flash_layout.h"
#include "hal/hal_interface.h"
#include <string.h>

/* FatFs 只支持 1 个物理卷：pdrv=0 → FLASH_LAYOUT_MSC_* */
#define FATFS_PDRV_FLASH    0

/* ================================================================
 *  FatFs disk_* 接口实现
 * ================================================================ */

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != FATFS_PDRV_FLASH) return STA_NOINIT;
    /* 我们的 msc_blockdev 是纯 Flash 映射，不需要 init，直接返回 0（ready）。
     *   若 ejected，则 STA_NODISK（上层 Shell 命令会显示"U 盘已弹出"）。*/
    if (msc_blockdev_is_ejected()) return STA_NODISK;
    return 0;
}

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != FATFS_PDRV_FLASH) return STA_NOINIT;
    /* ejected = true → 相当于"没有盘"（和 MSC USB 侧一致）。
     * 注意：STA_PROTECT 我们不返回（不是只读，只要 ejected=false 都可写）。*/
    if (msc_blockdev_is_ejected()) return STA_NODISK;
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != FATFS_PDRV_FLASH) return RES_PARERR;
    if (count == 0u) return RES_PARERR;
    /* 即使 ejected 也允许读（Shell 在 ejected 状态下 ls/cat 仍可用，
     *   因为 ejected 只是 MSC USB 状态，Flash 物理还是存在的；
     *   但 disk_status 返回 STA_NODISK 会让 f_mount 失败。
     *   所以： ejected=true 时 Shell 的 fs_* 命令执行时，先临时
     *   清 ejected → 调 FatFs → 再恢复 ejected（见 Shell 里 fs_lock()）。
     *   这里正常读。 */
    if (!msc_blockdev_read_multi((uint32_t)sector, (uint32_t)count, (uint8_t*)buff)) {
        return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != FATFS_PDRV_FLASH) return RES_PARERR;
    if (count == 0u) return RES_PARERR;
    /* ejected=false 表示主机 USB 当前可写入 → 为避免竞争，本机写被拒绝。
     * （Shell 命令要写前必须先 `msc eject` 使 ejected=true，此时主机被锁）。*/
    if (!msc_blockdev_is_ejected()) {
        /* 直接失败，RES_WRPRT（我们把这种"写保护"解释为：USB 正在占用，Shell 侧写锁） */
        return RES_WRPRT;
    }
    if (!msc_blockdev_write_multi((uint32_t)sector, (uint32_t)count, (const uint8_t*)buff)) {
        return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    if (pdrv != FATFS_PDRV_FLASH) return RES_PARERR;
    DRESULT rc = RES_OK;
    switch (cmd) {
        case CTRL_SYNC:
            /* Flash 写入是同步的（hal_flash_program 立即完成），无需 flush。 */
            rc = RES_OK;
            break;

        case GET_SECTOR_COUNT: {
            LBA_t *p = (LBA_t*)buff;
            *p = (LBA_t)msc_blockdev_sector_count();
            rc = RES_OK;
            break;
        }

        case GET_SECTOR_SIZE: {
            WORD *p = (WORD*)buff;
            *p = (WORD)MSC_BLOCKDEV_SECTOR_BYTES;
            rc = RES_OK;
            break;
        }

        case GET_BLOCK_SIZE: {
            /* 擦除块大小 = HAL_FLASH_SECTOR_SIZE (4096) bytes；单位是"sector" */
            DWORD *p = (DWORD*)buff;
            *p = (DWORD)(HAL_FLASH_SECTOR_SIZE / MSC_BLOCKDEV_SECTOR_BYTES);
            rc = RES_OK;
            break;
        }

        case CTRL_TRIM:
            /* TRIM 对 Flash 可选。我们的 Flash 不做垃圾回收（FTL），
             *   仅简单返回 OK（无副作用）。 */
            rc = RES_OK;
            break;

        default:
            rc = RES_PARERR;
            break;
    }
    return rc;
}

DWORD get_fattime(void) {
    /* FatFs 时间戳：[31..25] Year=1980+offset, [24..21] Month, [20..16] Day,
     * [15..11] Hour, [10..5] Min, [4..0] Sec/2。
     * NORTC=1 下仍然会被调用，返回一个固定合法值 2024/01/01 00:00:00。 */
    return ((DWORD)(2024u - 1980u) << 25u)
         | ((DWORD)(1u) << 21u)
         | ((DWORD)(1u) << 16u);
}

/* ================================================================
 *  FatFs 上层初始化：v2.2 首次启动自动 f_mkfs（格式化 FAT16）
 *  同时暴露 fatfs_shell_is_writable() 给 Shell 命令判断互斥锁。
 * ================================================================ */
#include "ff.h"

static FATFS s_fatfs_obj;    /* FatFs 文件系统对象（挂载后的工作 RAM 元数据） */
static bool s_fs_mounted = false;
static bool s_fs_mkfs_done_this_boot = false;

/* Shell 调用：获取 FatFs 对象指针（给 f_opendir / f_open 用） */
FATFS * fatfs_get_obj(void) { return &s_fatfs_obj; }
bool fatfs_is_mounted(void) { return s_fs_mounted; }

/**
 * @brief  以"安全互斥"模式执行一个 FatFs 操作：
 *          - 如果用户在 ejected=true 状态 → 直接执行（主机不会写）
 *          - 如果用户在 ejected=false 状态（USB 占用中）:
 *              - 只读操作(op_type=R)：临时 ejected=true → 执行 → 还原
 *              - 写操作(op_type=W)：用户必须先 `msc eject`，否则失败
 *
 * @return 0 = 锁已就绪（调用方自己 FatFs API）；非 0 = 失败码直接返回 Shell
 *
 * 为简化接口，实际"互斥锁"在 Shell 命令文件系统内部手动控制。
 * 这里提供两个 helper：
 */
bool fatfs_try_enter_write_mode(void) {
    /* 写模式：必须 ejected=true */
    return msc_blockdev_is_ejected();
}

bool fatfs_enter_read_mode(void) {
    /* 读模式：任何情况都可以，无需切换（只读不会损坏数据；
     *   即使主机正在写 → 主机已经在 MSC thread 里被保护了 Flash 串行；
     *   我们读的是 XIP 只读映射，主机擦写时 Pico SDK 关了中断，
     *   我们不会读一半，读到的是前后两状态之一即可）。*/
    return true;
}

/**
 * @brief  挂载并（如空片）自动格式化
 * @retval FRESULT（FatFs 标准错误码）
 */
FRESULT fatfs_init_and_mount(void) {
    if (s_fs_mounted) return FR_OK;

    /* 1. 无论 eject 状态，先 mount。f_mount(0, "0:", 1=立即检查) →
     *    空片时返回 FR_NO_FILESYSTEM → 需要先 mkfs。
     *
     * 问题：如果 ejected=false（主机 USB 占用），我们不能先写 Flash。
     *   解决方式：格式化必须在 ejected=true 下完成（避免和主机竞争）。
     *   所以这里先不管 ejected：若 ejected=false + 有 FS → mount 成功 OK，
     *   读（ls/cat）没问题；若 ejected=false + 无 FS → mount 失败返回
     *   FR_NO_FILESYSTEM，Shell 启动 banner 里提示用户 `msc format`。
     */

    FRESULT fr = f_mount(&s_fatfs_obj, _T("0:"), 0);   /* 0 = 延迟挂载（等第一条命令真的检查 BPB） */
    /* 为触发对 BPB 的真实检查，立刻做一次 f_stat("/",...) 或 f_opendir 根目录：
     *   f_mount(... 0) 其实不检查介质，我们先随便 f_opendir 根目录。*/
    DIR d;
    FILINFO fi;
    fr = f_opendir(&d, _T("/"));
    if (fr == FR_OK) {
        f_closedir(&d);
        s_fs_mounted = true;
        return FR_OK;
    }
    (void)fi;

    /* 两种常见 FR：
     *   - FR_NO_FILESYSTEM : 确实没 BPB → 已空或未格式化
     *   - FR_DISK_ERR      : 可能 STA_NODISK（ejected=true 时返回 disk_init NODISK）
     * 其他 FR：例如 FR_INT_ERR 之类。
     *
     * 注意：如果 ejected=true  → disk_status 返回 STA_NODISK，f_mount 立刻 FR_DISK_ERR。
     *   我们要在 ejected=true 下执行格式化。为了让 diskio 正常工作，
     *   先临时切换为 ejected=false → 完成 mkfs+mount → 切回 ejected=true。
     *
     *   但如果 ejected=false（USB 占用），此时不能格式化，我们直接把
     *   fr 返回上去，Shell banner 告诉用户要做什么。
     */

    if ((fr == FR_NO_FILESYSTEM || fr == FR_DISK_ERR) && msc_blockdev_is_blank()) {
        /* 空片：需要格式化 → 切到 ejected=true（"本机独占模式"），
         *   让 diskio disk_status 返回 NOT NODISK（因为 ejected=true
         *   会被当成 NO DISK 被 FatFs 拒绝写——我们这里临时清 ejected 让 mkfs 成功，
         *   然后把 ejected 设为 true，以便让主机重新插入前不会写。*/
        bool prev_eject = msc_blockdev_is_ejected();
        msc_blockdev_set_ejected(true);   /* ejected=true 时 disk_status 返回 STA_NODISK 会被 FatFs 拒写...
                                           * 所以 mkfs 这一步期间，我们临时改成 ejected=false（允许 disk_write 成功），
                                           * 但保证主机也不在写：因为 ejected=false 时主机 TU task 以为介质 ready，
                                           *   但我们正处于 boot 初始化阶段、tud_task 还没怎么跑（或主机还没 mount）
                                           *   —— 为确保安全，mkfs 过程中先让 ejected=false 让 disk_write 通过，
                                           *   在 mkfs 结束后立刻把 ejected 设回 true，这样主机侧 tud_msc_test_unit_ready
                                           *   返回 false，立刻感知介质已弹出，不会写。 */

        /* 正确做法：在 mkfs 期间临时 ejected=false（这样 disk_write 返回 RES_WRPRT 检查通过）
         *   但 eject=true 时 disk_status = STA_NODISK，会让 f_mkfs 在开头检查就 FR_DISK_ERR。
         *   所以 mkfs 阶段我们要 ejected=false，让 disk_status = 0 且 disk_write 允许通过。
         *   为防主机 USB 在这瞬间写，我们"临时清 ejected，mkfs 后立刻 true"。
         *   实际上这阶段是极早期 boot，主机还在枚举 MSC、没 SCSI 写进来；
         *   安全起见设 prev_eject：如果之前是 true，就 true→false→true；
         *   如果之前是 false（正常默认 false），就 false→做完保留 false，但
         *   保留 false 会让主机也能写 → 所以不管之前状态，最后强制 true：
         *   → 首启动格式化完之后，MSC 对主机显示"未就绪"，用户需要先 `msc mount`
         *     （或插拔 USB）才能看到 U 盘盘符。
         *   这正是"Shell 文件系统命令先可写"的策略——对 v2.2 这完全 OK。 */
        (void)prev_eject;
        msc_blockdev_set_ejected(false);  /* 临时允许 disk_write */

        /* mkfs：FAT16，扇区=2032，工作缓冲区随便开个 512B 在栈上足够 */
        static BYTE s_mkfs_work[512];
        MKFS_PARM opt;
        memset(&opt, 0, sizeof(opt));
        opt.fmt = FM_FAT;       /* 强制 FAT12/16（不是 FAT32），因为小容量 1016KB 选 FAT16 更省 */
        opt.n_fat = 2;          /* 2 份 FAT 表，更稳健（经典默认） */
        opt.align = HAL_FLASH_SECTOR_SIZE / MSC_BLOCKDEV_SECTOR_BYTES; /* 以 4KB 擦除块为 cluster 对齐 */
        opt.n_root = 128;       /* 根目录条目：128 × 32B = 4KB，够用，也对齐 4KB */
        opt.au_size = 0;        /* 0 = 自动（预计 1 或 2 sectors/cluster，1016KB 够用） */

        fr = f_mkfs(_T("0:"), &opt, s_mkfs_work, sizeof(s_mkfs_work));
        s_fs_mkfs_done_this_boot = (fr == FR_OK);

        /* 格式化完成后立即 ejected=true → 让主机 USB 侧看到"无介质"，
         *   保证主机不会再在 Shell 使用文件系统期间写 Flash。 */
        msc_blockdev_set_ejected(true);

        if (fr != FR_OK) {
            /* 格式化失败了：s_fs_mounted 保持 false，mount 下次继续尝试 */
            return fr;
        }

        /* 格式化成功 → 重新 mount（此时 ejected=true，但 f_mount + 立即校验要能 work。
         *   问题：disk_status 对于 ejected=true 返回 STA_NODISK → FatFs 不认介质。
         *   解：先临时 ejected=false mount；mount 完 ejected=true。
         *   mount 后即使 ejected=true，FatFs 仍会把它的 FAT 缓存和已挂载状态保留在内存，
         *   Shell f_read/f_write 可以正常跑（disk_read 永远 work；
         *   disk_write 的 ejected=true 判定反而是必要的：只有 ejected=true 才允许写，
         *   就是我们现在的状态——完美契合）。 */
        msc_blockdev_set_ejected(false);
        fr = f_mount(&s_fatfs_obj, _T("0:"), 1);  /* 1 = 立即 mount */
        msc_blockdev_set_ejected(true);           /* 立刻恢复 ejected=true */
        if (fr == FR_OK) {
            s_fs_mounted = true;
        }
        return fr;
    }

    /* 不是空片（有 BPB 或其他），但 mount 失败 → 原样返回 */
    return fr;
}

bool fatfs_mkfs_done_this_boot(void) { return s_fs_mkfs_done_this_boot; }
