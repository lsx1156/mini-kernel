/**
 * @file    config_store.c
 * @brief   循环缓冲区配置存储 — 超频档位 / 多核标志 持久化
 * 
 * 核心设计：4KB 扇区作为循环缓冲区，存放多条 config_data_t 记录
 * - 每次保存仅写入 256B 页 (~2ms)，无需擦除
 * - 仅当 4KB 扇区写满时才擦除一次 (~50-100ms)
 * - 读取时扫描扇区，取最新的有效记录
 */

#include "hal/config_store.h"
#include "hal/sysclk.h"
#include "hal/flash_layout.h"
#include <string.h>

/* ---------- 常量定义 ---------- */

#define CONFIG_SECTOR_OFFSET        FLASH_LAYOUT_CONFIG_OFFSET
#define CONFIG_SECTOR_SIZE          4096
#define CONFIG_RECORD_SIZE          256          /* 每条记录 256B (1 Flash 页) */
#define CONFIG_MAX_RECORDS          (CONFIG_SECTOR_SIZE / CONFIG_RECORD_SIZE)  /* 16 条 */

/* ---------- CRC32 ---------- */

static uint32_t config_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t config_calc_crc(const config_data_t *cfg) {
    return config_crc32((const uint8_t *)cfg, 16u);
}

/* ---------- 默认值 ---------- */

void config_defaults(config_data_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->magic   = CONFIG_MAGIC;
    cfg->version = CONFIG_VERSION;
    cfg->clock_mhz = 0;
    cfg->multi_core = 0;
    cfg->reserved[0] = 1;
    cfg->crc32 = config_calc_crc(cfg);
}

/* ---------- 扫描扇区，取最新有效记录 ---------- */

bool config_read(config_data_t *cfg) {
    const uint8_t *sector = (const uint8_t *)hal_flash_map_read(CONFIG_SECTOR_OFFSET);
    if (!sector) { if (cfg) config_defaults(cfg); return false; }

    /* 扫描所有记录，取最新的有效记录 (按 CRC32 校验) */
    const config_data_t *latest = NULL;
    for (int i = 0; i < CONFIG_MAX_RECORDS; i++) {
        const config_data_t *rec = (const config_data_t *)(sector + i * CONFIG_RECORD_SIZE);
        
        /* 跳过擦除态 (全 0xFF) */
        if (rec->magic == 0xFFFFFFFFu) break;
        
        /* 校验 magic/version/crc32 */
        if (rec->magic != CONFIG_MAGIC) continue;
        if (rec->version != CONFIG_VERSION) continue;
        if (rec->crc32 != config_calc_crc(rec)) continue;
        
        /* 频率合法性 */
        if (rec->clock_mhz != 0u && 
            (rec->clock_mhz < SYSCLK_MHZ_MIN || rec->clock_mhz > SYSCLK_MHZ_MAX)) {
            continue;
        }
        
        latest = rec;  /* 更新为更新的记录 */
    }
    
    if (latest) {
        if (cfg) *cfg = *latest;
        return true;
    }
    
    if (cfg) config_defaults(cfg);
    return false;
}

/* ---------- 写入新记录到循环缓冲区 ---------- */

hal_err_t config_write(const config_data_t *cfg) {
    if (!cfg) return HAL_ERR_PARAM;
    
    config_data_t tmp = *cfg;
    tmp.magic   = CONFIG_MAGIC;
    tmp.version = CONFIG_VERSION;
    tmp.crc32   = config_calc_crc(&tmp);

    /* 1. 找到下一个可写位置 */
    const uint8_t *sector = (const uint8_t *)hal_flash_map_read(CONFIG_SECTOR_OFFSET);
    if (!sector) return HAL_ERR_IO;
    
    int write_idx = -1;
    for (int i = 0; i < CONFIG_MAX_RECORDS; i++) {
        const config_data_t *rec = (const config_data_t *)(sector + i * CONFIG_RECORD_SIZE);
        if (rec->magic == 0xFFFFFFFFu) {  /* 找到空位 */
            write_idx = i;
            break;
        }
    }
    
    /* 2. 扇区满了，需要擦除 */
    if (write_idx < 0) {
        hal_err_t e = hal_flash_erase_sector(CONFIG_SECTOR_OFFSET);
        if (e != HAL_OK) return e;
        write_idx = 0;
    }
    
    /* 3. 准备 256B 完整页 */
    uint8_t page_buf[256];
    memset(page_buf, 0xFF, sizeof(page_buf));
    memcpy(page_buf, &tmp, sizeof(tmp));
    
    /* 3. 写入单页 (无擦除，~2ms) */
    uint32_t write_offset = CONFIG_SECTOR_OFFSET + write_idx * CONFIG_RECORD_SIZE;
    hal_err_t e = hal_flash_program(write_offset, page_buf, sizeof(page_buf));
    if (e != HAL_OK) return e;
    
    /* XIP 缓存失效 + 读回验证 */
    __asm volatile ("dsb sy; isb" ::: "memory");
    
    const config_data_t *written = (const config_data_t *)hal_flash_map_read(write_offset);
    if (written && written->magic == CONFIG_MAGIC && 
        written->version == CONFIG_VERSION &&
        written->clock_mhz == tmp.clock_mhz && 
        written->multi_core == tmp.multi_core &&
        written->crc32 == tmp.crc32) {
        return HAL_OK;
    }
    return HAL_ERR_IO;
}

hal_err_t config_clear_all(void) {
    return hal_flash_erase_sector(CONFIG_SECTOR_OFFSET);
}

/* ---------- 弱符号钩子 ---------- */

__attribute__((weak)) void config_mcore_apply(bool enable) {
    (void)enable;
}

/* ---------- 冷启动应用 ---------- */

int config_apply(void) {
    config_data_t cfg;
    bool valid = config_read(&cfg);
    if (!valid) {
        return SYSCLK_MHZ_DEFAULT;
    }

    uint32_t mhz = (uint32_t)cfg.clock_mhz;
    if (mhz == 0u) mhz = SYSCLK_MHZ_DEFAULT;

    if (mhz > SYSCLK_MAX_SAFE_MHZ) {
        return SYSCLK_MHZ_DEFAULT;
    }
    (void)sysclk_apply_mhz(mhz);

    config_mcore_apply(cfg.multi_core != 0);

    return (int)sysclk_current_mhz();
}