/**
 * @file    config_store.c
 * @brief   RP2350 持久化配置存储 (Flash Bootscript 扇区复用)
 */

#include "hal_port.h"
#include "flash_layout.h"
#include <string.h>
#include <stdio.h>

/* 配置存储在 Bootscript A 扇区末尾 256 字节 */
#define CONFIG_STORE_OFFSET           (PART_BOOTSCRIPT_A_OFFSET + PART_BOOTSCRIPT_A_SIZE - 256)
#define CONFIG_STORE_MAGIC            0x43464753  /* 'SFGC' = Stored Flash Global Config */
#define CONFIG_STORE_MAX_ENTRIES      16
#define CONFIG_STORE_KEY_MAXLEN       32
#define CONFIG_STORE_VAL_MAXLEN       64

typedef struct {
    uint32_t magic;
    uint16_t count;
    uint16_t crc16;
    struct {
        char key[CONFIG_STORE_KEY_MAXLEN];
        char value[CONFIG_STORE_VAL_MAXLEN];
    } entries[CONFIG_STORE_MAX_ENTRIES];
} config_store_t;

static config_store_t g_config_store __attribute__((section(".noinit")));
static bool g_config_dirty = false;

static uint16_t crc16_ccitt(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)p[i] << 8;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
        }
    }
    return crc;
}

hal_err_t config_store_init(void) {
    /* 从 Flash 读取配置 */
    const config_store_t *flash_cfg = (const config_store_t *)(FLASH_XIP_BASE + CONFIG_STORE_OFFSET);
    
    if (flash_cfg->magic == CONFIG_STORE_MAGIC) {
        uint16_t calc_crc = crc16_ccitt(flash_cfg, sizeof(config_store_t) - 2);
        if (calc_crc == flash_cfg->crc16) {
            memcpy(&g_config_store, flash_cfg, sizeof(config_store_t));
            return HAL_OK;
        }
    }
    
    /* 初始化为空 */
    memset(&g_config_store, 0, sizeof(config_store_t));
    g_config_store.magic = CONFIG_STORE_MAGIC;
    g_config_store.count = 0;
    g_config_dirty = true;
    return config_store_save();
}

hal_err_t config_store_load(void) {
    return config_store_init();
}

hal_err_t config_store_save(void) {
    g_config_store.crc16 = crc16_ccitt(&g_config_store, sizeof(config_store_t) - 2);
    
    /* 擦除 Bootscript A 扇区 (配置存储在其中) */
    flash_range_erase(FLASH_BOOTSCRIPT_A_START, FLASH_SECTOR_SIZE);
    
    /* 先恢复 Bootscript 数据 (读取原有 bootscript) */
    const bootscript_t *old_bs = (const bootscript_t *)FLASH_BOOTSCRIPT_A_START;
    bootscript_t bs_copy;
    memcpy(&bs_copy, old_bs, sizeof(bootscript_t));
    
    /* 写回 Bootscript + 配置 */
    uint8_t sector_buf[FLASH_SECTOR_SIZE];
    memset(sector_buf, 0xFF, FLASH_SECTOR_SIZE);
    memcpy(sector_buf, &bs_copy, sizeof(bootscript_t));
    memcpy(sector_buf + FLASH_SECTOR_SIZE - 256, &g_config_store, 256);
    
    flash_range_program(FLASH_BOOTSCRIPT_A_START, sector_buf, FLASH_SECTOR_SIZE);
    g_config_dirty = false;
    return HAL_OK;
}

hal_err_t config_store_get(const char *key, char *value, size_t max_len) {
    if (!key || !value || max_len == 0) return HAL_ERR_INVAL;
    
    for (uint16_t i = 0; i < g_config_store.count; i++) {
        if (strcmp(g_config_store.entries[i].key, key) == 0) {
            strncpy(value, g_config_store.entries[i].value, max_len - 1);
            value[max_len - 1] = '\0';
            return HAL_OK;
        }
    }
    return HAL_ERR_NOT_FOUND;
}

hal_err_t config_store_set(const char *key, const char *value) {
    if (!key || !value) return HAL_ERR_INVAL;
    if (strlen(key) >= CONFIG_STORE_KEY_MAXLEN) return HAL_ERR_INVAL;
    if (strlen(value) >= CONFIG_STORE_VAL_MAXLEN) return HAL_ERR_INVAL;
    
    /* 查找现有 */
    for (uint16_t i = 0; i < g_config_store.count; i++) {
        if (strcmp(g_config_store.entries[i].key, key) == 0) {
            strcpy(g_config_store.entries[i].value, value);
            g_config_dirty = true;
            return config_store_save();
        }
    }
    
    /* 新增 */
    if (g_config_store.count >= CONFIG_STORE_MAX_ENTRIES) return HAL_ERR_FULL;
    
    strcpy(g_config_store.entries[g_config_store.count].key, key);
    strcpy(g_config_store.entries[g_config_store.count].value, value);
    g_config_store.count++;
    g_config_dirty = true;
    return config_store_save();
}