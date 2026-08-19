/**
 * @file    bootscript.c
 * @brief   RP2350 开机脚本持久化 (双备份扇区 + CRC8)
 */

#include "hal_port.h"
#include "flash_layout.h"
#include <string.h>

#define BOOTSCRIPT_MAGIC              0x42534352  /* 'RSCB' */
#define BOOTSCRIPT_VERSION            1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint8_t  crc8;
    char     cmds[HAL_BOOTSCRIPT_MAX_CMDS][HAL_BOOTSCRIPT_MAX_CMD_LEN];
} bootscript_t;

static bootscript_t g_bootscript;
static bool g_bootscript_dirty = false;

static uint8_t crc8(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1);
        }
    }
    return crc;
}

static bool bootscript_validate(const bootscript_t *bs) {
    if (bs->magic != BOOTSCRIPT_MAGIC) return false;
    if (bs->version != BOOTSCRIPT_VERSION) return false;
    if (bs->count > HAL_BOOTSCRIPT_MAX_CMDS) return false;
    
    uint8_t calc_crc = crc8(bs, sizeof(bootscript_t) - 1);
    return calc_crc == bs->crc8;
}

static void bootscript_compute_crc(void) {
    g_bootscript.crc8 = crc8(&g_bootscript, sizeof(bootscript_t) - 1);
}

hal_err_t bootscript_init(void) {
    /* 尝试从 Bootscript A 加载 */
    const bootscript_t *bs_a = (const bootscript_t *)FLASH_BOOTSCRIPT_A_START;
    if (bootscript_validate(bs_a)) {
        memcpy(&g_bootscript, bs_a, sizeof(bootscript_t));
        return HAL_OK;
    }
    
    /* 尝试从 Bootscript B 加载 */
    const bootscript_t *bs_b = (const bootscript_t *)FLASH_BOOTSCRIPT_B_START;
    if (bootscript_validate(bs_b)) {
        memcpy(&g_bootscript, bs_b, sizeof(bootscript_t));
        return HAL_OK;
    }
    
    /* 都无效，初始化为空 */
    memset(&g_bootscript, 0, sizeof(bootscript_t));
    g_bootscript.magic = BOOTSCRIPT_MAGIC;
    g_bootscript.version = BOOTSCRIPT_VERSION;
    g_bootscript.count = 0;
    bootscript_compute_crc();
    g_bootscript_dirty = true;
    return bootscript_save();
}

hal_err_t bootscript_save(void) {
    bootscript_compute_crc();
    
    /* 写入 Bootscript A */
    flash_range_erase(FLASH_BOOTSCRIPT_A_START, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_BOOTSCRIPT_A_START, (const uint8_t *)&g_bootscript, sizeof(bootscript_t));
    
    /* 写入 Bootscript B (副本) */
    flash_range_erase(FLASH_BOOTSCRIPT_B_START, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_BOOTSCRIPT_B_START, (const uint8_t *)&g_bootscript, sizeof(bootscript_t));
    
    g_bootscript_dirty = false;
    return HAL_OK;
}

hal_err_t bootscript_load(void) {
    return bootscript_init();
}

int bootscript_count(void) {
    return g_bootscript.count;
}

hal_err_t bootscript_add_cmd(const char *cmd) {
    if (!cmd || strlen(cmd) >= HAL_BOOTSCRIPT_MAX_CMD_LEN) return HAL_ERR_INVAL;
    if (g_bootscript.count >= HAL_BOOTSCRIPT_MAX_CMDS) return HAL_ERR_FULL;
    
    strncpy(g_bootscript.cmds[g_bootscript.count], cmd, HAL_BOOTSCRIPT_MAX_CMD_LEN - 1);
    g_bootscript.cmds[g_bootscript.count][HAL_BOOTSCRIPT_MAX_CMD_LEN - 1] = '\0';
    g_bootscript.count++;
    g_bootscript_dirty = true;
    return HAL_OK;
}

hal_err_t bootscript_del_cmd(uint8_t slot) {
    if (slot >= g_bootscript.count) return HAL_ERR_INVAL;
    
    for (uint16_t i = slot; i < g_bootscript.count - 1; i++) {
        strcpy(g_bootscript.cmds[i], g_bootscript.cmds[i + 1]);
    }
    g_bootscript.count--;
    g_bootscript_dirty = true;
    return HAL_OK;
}

hal_err_t bootscript_get_cmd(uint8_t slot, char *buf, size_t max_len) {
    if (slot >= g_bootscript.count || !buf || max_len == 0) return HAL_ERR_INVAL;
    
    strncpy(buf, g_bootscript.cmds[slot], max_len - 1);
    buf[max_len - 1] = '\0';
    return HAL_OK;
}

void bootscript_run_all(void) {
    for (uint16_t i = 0; i < g_bootscript.count; i++) {
        shell_exec(g_bootscript.cmds[i]);
    }
}

hal_err_t bootscript_set_dirty(void) {
    g_bootscript_dirty = true;
    return HAL_OK;
}