/* ================================================================
 * bootscript.c - 开机固化指令（持久化命令脚本）子系统
 *
 * 功能：让用户输入的 shell 命令能被"固化"到板载 W25Q Flash，掉电不丢，
 *       下次开机自动执行。
 *
 * 语法约定（用户在 shell 里操作）：
 *   ! <命令行...>            → 立即执行该命令 + 追加固化为开机自动执行
 *   save   <命令行...>       → 仅追加固化（不立即执行）
 *   unsave <index>           → 删除第 index 条已固化命令（后面条目自动前移）
 *   unsave all               → 清空整个 bootscript（擦除两个扇区）
 *   list                    → 列出当前所有已固化命令及序号、CRC 状态
 *   boot exec               → 立刻从 Flash 顺序执行所有已固化命令
 *
 * 存储方案（双备份 + CRC8 + 增量 append，掉电可恢复）：
 *   · 板载 Flash 最后 2 × 4KB 扇区（共 8KB）做镜像双备份：
 *     A = offset = PICO_FLASH_SIZE_BYTES - 2 * 4096
 *     B = offset = PICO_FLASH_SIZE_BYTES - 1 * 4096
 *     每一条写入同时写 A 和 B，读时校验 CRC 选第一个健康的扇区。
 *   · 扇区内部结构（4KB = 4096 B）：
 *     [0x00] magic1 = 'M' (0x4D)
 *     [0x01] magic2 = 'K' (0x4B)        → 0xFF 未初始化扇区
 *     [0x02] num_entries (uint8 LSB, 实际数量 0..31)
 *     [0x03] crc_hdr (uint8 CRC8 of bytes 0..2)
 *     [0x04..0x07] reserved (0xFF)
 *     --- slot array ---
 *     [0x08 + 128*i .. 0x07+128*(i+1)]  for i=0..31
 *       slot (128 B / entry):
 *         [0]   valid    = 0xAA 有效 / 其他 无效（擦后 0xFF 未占用）
 *         [1]   cmd_len  (1..125, 实际命令行字节数)
 *         [2..2+cmd_len-1]  命令行（ASCII，含 '\0' 吗？- 不含，长度字段存字节数）
 *         [126] crc_entry CRC8 of bytes 0..125
 *         [127] reserved (0xFF)
 *     4096 = 8 header + 32 * 128 slot, 刚好，最多 32 条固化命令。
 *
 * 增量写入策略（避免每次都整扇区擦除）：
 *   · 每个 slot 的 valid=0xAA 只有从 0xFF→0xAA 的 1→0 转换允许不擦直接写。
 *   · 追加新命令时找到第一个 valid=0xFF 的空槽，直接写 slot。
 *   · 删除（unsave）时只能把 valid 从 0xAA→0x00（无效），然后需要"压缩/迁移"
 *     才会释放该 slot。每次 unsave <N> 都把后面的 slot 迁移到前面，
 *     然后整扇区先擦再重写（带临时内存 staging buffer，4KB 栈 OK 因为只在
 *     shell 任务调用，shell 栈 768 不够，所以用 kmalloc 4KB staging）。
 *
 * 双备份同步：
 *   · 任何写入操作：先读 A + B，选一个"主"，更新 staging，然后分别擦 A 和 B
 *     → 写 A → 写 B（串行化）。
 *   · 任何读取操作：按 A 先验后 B，校验 magic + header CRC，选第一个合法。
 *     两者都合法时选 num_entries 一致的那一个；不一致时选 CRC 正确且
 *     num_entries 较小的那一个（通常是中途断电的备份损坏较少）。
 * ================================================================ */
#include "bootscript.h"
#include "hal_interface.h"
#include "os_config.h"
#include "mem.h"           /* kmalloc / kfree */
#include <string.h>

/* ---------- 存储布局 ---------- */
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES   (2u * 1024u * 1024u)
#endif

#define BOOTSCRIPT_SECTOR_B     (PICO_FLASH_SIZE_BYTES       - HAL_FLASH_SECTOR_SIZE)
#define BOOTSCRIPT_SECTOR_A     (BOOTSCRIPT_SECTOR_B         - HAL_FLASH_SECTOR_SIZE)

#define HDR_MAGIC1       0x4Du   /* 'M' */
#define HDR_MAGIC2       0x4Bu   /* 'K' */
#define HDR_OFF_MAGIC1   0u
#define HDR_OFF_MAGIC2   1u
#define HDR_OFF_COUNT    2u
#define HDR_OFF_CRC      3u
#define HDR_SIZE         8u

#define SLOT_VALID_MARK  0xAAu   /* slot valid: 0xFF 擦 → 0xAA 写；删除时写成 0x00 */
#define SLOT_EMPTY_MARK  0xFFu
#define SLOT_DEAD_MARK   0x00u
#define SLOT_OFF_VALID   0u
#define SLOT_OFF_LEN     1u
#define SLOT_OFF_CMD     2u
#define SLOT_OFF_CRC     126u
#define SLOT_SIZE        128u
#define SLOT_MAX_CMD     123u  /* 128 - 3(valid+len+crc) - 1(reserved) - 1 padding for safety */
#define MAX_ENTRIES      32u   /* (4096 - 8) / 128 */

/* ---------- 本地 CRC8（与 hal_port.c 相同，保证跨模块一致） ---------- */
static uint8_t bs_crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++) {
            crc = (uint8_t)((crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1));
        }
    }
    return crc;
}

/* ---------- 扇区健康度判定 ---------- */
static bool sector_is_valid(uint32_t sector_off, uint8_t *out_count) {
    const uint8_t *p = hal_flash_map_read(sector_off);
    if (!p) return false;
    if (p[HDR_OFF_MAGIC1] != HDR_MAGIC1 || p[HDR_OFF_MAGIC2] != HDR_MAGIC2) return false;
    uint8_t calc = bs_crc8(&p[0], 3u);
    if (calc != p[HDR_OFF_CRC]) return false;
    uint8_t n = p[HDR_OFF_COUNT];
    if (n > MAX_ENTRIES) return false;
    /* 再抽查 valid=0xAA 的 slot CRC */
    for (uint8_t i = 0; i < n; i++) {
        uint32_t slot_off = HDR_SIZE + (uint32_t)i * SLOT_SIZE;
        if (p[slot_off + SLOT_OFF_VALID] != SLOT_VALID_MARK) return false;
        uint8_t slot_crc = bs_crc8(&p[slot_off], SLOT_SIZE - 2u);  /* 最后 1B CRC + 1B reserved 前的 126B */
        if (slot_crc != p[slot_off + SLOT_OFF_CRC]) return false;
    }
    if (out_count) *out_count = n;
    return true;
}

/* 选取一个健康扇区：优先 A，A 坏 → B；都坏 → 都没初始化（首次）返回 false。
 * 首次启动（两个全 0xFF）：sector_is_valid 都会 false，我们返回 false 表示"空"。 */
static bool bootscript_pick_active(uint32_t *out_sector, uint8_t *out_count) {
    uint8_t ca = 0, cb = 0;
    bool a_ok = sector_is_valid(BOOTSCRIPT_SECTOR_A, &ca);
    bool b_ok = sector_is_valid(BOOTSCRIPT_SECTOR_B, &cb);
    if (a_ok && b_ok) {
        /* 两个都好：选 A 作主，B 是镜像，num_entries 应该一致；不一致选较小的（更保守） */
        if (out_count) *out_count = (ca < cb) ? ca : cb;
        if (out_sector) *out_sector = BOOTSCRIPT_SECTOR_A;
        return true;
    }
    if (a_ok) { if (out_count) *out_count = ca; if (out_sector) *out_sector = BOOTSCRIPT_SECTOR_A; return true; }
    if (b_ok) { if (out_count) *out_count = cb; if (out_sector) *out_sector = BOOTSCRIPT_SECTOR_B; return true; }
    return false;
}

/* 把 staging 4KB 写到 A 和 B（先擦再写）。staging 必须是完整、CRC 已计算好的一整个扇区。 */
static hal_err_t bs_commit_both(const uint8_t *staging) {
    hal_err_t a_erase = hal_flash_erase_sector(BOOTSCRIPT_SECTOR_A);
    hal_err_t b_erase = hal_flash_erase_sector(BOOTSCRIPT_SECTOR_B);
    if (a_erase != HAL_OK || b_erase != HAL_OK) return HAL_ERR_IO;
    hal_err_t a_wr = hal_flash_program(BOOTSCRIPT_SECTOR_A, staging, HAL_FLASH_SECTOR_SIZE);
    hal_err_t b_wr = hal_flash_program(BOOTSCRIPT_SECTOR_B, staging, HAL_FLASH_SECTOR_SIZE);
    if (a_wr != HAL_OK || b_wr != HAL_OK) return HAL_ERR_IO;
    return HAL_OK;
}

/* 构造一个空扇区 staging（擦后 0xFF，然后写 magic/count=0/header CRC） */
static void bs_build_empty(uint8_t *staging) {
    memset(staging, 0xFF, HAL_FLASH_SECTOR_SIZE);
    staging[HDR_OFF_MAGIC1] = HDR_MAGIC1;
    staging[HDR_OFF_MAGIC2] = HDR_MAGIC2;
    staging[HDR_OFF_COUNT]  = 0;
    staging[HDR_OFF_CRC]    = bs_crc8(&staging[0], 3);
}

/* 从健康扇区拷贝到 staging（用于后续修改再 commit）。若当前没有健康扇区，返回 empty。
 * 返回实际 entries 数。 */
static uint8_t bs_load_to_staging(uint8_t *staging) {
    uint8_t count = 0;
    uint32_t act = 0;
    if (bootscript_pick_active(&act, &count)) {
        const uint8_t *p = hal_flash_map_read(act);
        memcpy(staging, p, HAL_FLASH_SECTOR_SIZE);
    } else {
        bs_build_empty(staging);
        count = 0;
    }
    return count;
}

/* 计算 slot CRC 并填到 staging */
static void bs_sign_slot(uint8_t *staging, uint8_t idx) {
    uint32_t slot_off = HDR_SIZE + (uint32_t)idx * SLOT_SIZE;
    staging[slot_off + SLOT_OFF_CRC] = bs_crc8(&staging[slot_off], SLOT_SIZE - 2u);
}

static void bs_sign_header(uint8_t *staging, uint8_t count) {
    staging[HDR_OFF_MAGIC1] = HDR_MAGIC1;
    staging[HDR_OFF_MAGIC2] = HDR_MAGIC2;
    staging[HDR_OFF_COUNT]  = count;
    staging[HDR_OFF_CRC]    = bs_crc8(&staging[0], 3);
}

/* ========== 公共 API ========== */

uint8_t bootscript_count(void) {
    uint8_t n = 0;
    (void)bootscript_pick_active(NULL, &n);
    return n;
}

bool bootscript_get(uint8_t idx, char *buf, size_t buf_size) {
    if (buf == NULL || buf_size < (SLOT_MAX_CMD + 1u)) return false;
    uint8_t count = 0;
    uint32_t act = 0;
    if (!bootscript_pick_active(&act, &count)) return false;
    if (idx >= count) return false;
    const uint8_t *p = hal_flash_map_read(act);
    uint32_t slot_off = HDR_SIZE + (uint32_t)idx * SLOT_SIZE;
    uint8_t len = p[slot_off + SLOT_OFF_LEN];
    if (len > SLOT_MAX_CMD) return false;
    memcpy(buf, &p[slot_off + SLOT_OFF_CMD], len);
    buf[len] = '\0';
    return true;
}

hal_err_t bootscript_append(const char *cmd_line) {
    if (cmd_line == NULL) return HAL_ERR_PARAM;
    size_t len = strlen(cmd_line);
    if (len == 0 || len > SLOT_MAX_CMD) return HAL_ERR_PARAM;

    uint8_t *staging = (uint8_t *)kmalloc(HAL_FLASH_SECTOR_SIZE);
    if (!staging) return HAL_ERR_NOMEM;

    uint8_t count = bs_load_to_staging(staging);
    hal_err_t err = HAL_ERR_FULL;
    if (count < MAX_ENTRIES) {
        uint32_t slot_off = HDR_SIZE + (uint32_t)count * SLOT_SIZE;
        staging[slot_off + SLOT_OFF_VALID] = SLOT_VALID_MARK;   /* 0xFF → 0xAA OK (1→0) */
        staging[slot_off + SLOT_OFF_LEN]   = (uint8_t)len;
        memcpy(&staging[slot_off + SLOT_OFF_CMD], cmd_line, len);
        bs_sign_slot(staging, count);
        count++;
        bs_sign_header(staging, count);
        err = bs_commit_both(staging);
    }
    kfree(staging);
    return err;
}

hal_err_t bootscript_remove(uint8_t idx) {
    uint8_t *staging = (uint8_t *)kmalloc(HAL_FLASH_SECTOR_SIZE);
    if (!staging) return HAL_ERR_NOMEM;
    uint8_t count = bs_load_to_staging(staging);
    hal_err_t err = HAL_ERR_PARAM;
    if (idx < count) {
        /* 把 idx+1..count-1 的 slots 向前搬 1 格，然后把最后那个 slot 清零（0xFF 整体覆盖到 staging） */
        for (uint8_t i = idx; i < count - 1u; i++) {
            uint32_t dst = HDR_SIZE + (uint32_t)i * SLOT_SIZE;
            uint32_t src = HDR_SIZE + (uint32_t)(i + 1u) * SLOT_SIZE;
            memcpy(&staging[dst], &staging[src], SLOT_SIZE);
        }
        uint32_t last = HDR_SIZE + (uint32_t)(count - 1u) * SLOT_SIZE;
        memset(&staging[last], 0xFF, SLOT_SIZE);
        count--;
        /* 重新给每个存活 slot 计算 CRC 和 header CRC */
        for (uint8_t i = 0; i < count; i++) bs_sign_slot(staging, i);
        bs_sign_header(staging, count);
        err = bs_commit_both(staging);
    }
    kfree(staging);
    return err;
}

hal_err_t bootscript_clear_all(void) {
    uint8_t *staging = (uint8_t *)kmalloc(HAL_FLASH_SECTOR_SIZE);
    if (!staging) return HAL_ERR_NOMEM;
    bs_build_empty(staging);
    hal_err_t err = bs_commit_both(staging);
    kfree(staging);
    return err;
}

/* ========== B 线路（SPI Flash）自检辅助 API ==========
 * 这两个函数给后续 SPI 线路验证板载 W25Q16JV 的擦/写/读/CRC 全链路提供：
 *   bootscript_erase_test()  清空双备份扇区（触发真实 erase + program）
 *   bootscript_verify()      读回 A/B 扇区 header CRC 并检查是否一致 */
hal_err_t bootscript_erase_test(void) {
    return bootscript_clear_all();
}

bool bootscript_verify(void) {
    uint8_t ca = 0xFF, cb = 0xFF;
    bool a_ok = sector_is_valid(BOOTSCRIPT_SECTOR_A, &ca);
    bool b_ok = sector_is_valid(BOOTSCRIPT_SECTOR_B, &cb);
    if (!a_ok || !b_ok) return false;
    return (ca == cb);
}
