/**
 * @file    config_store.c
 * @brief   v2.4 系统固化配置存储实现（超频档位 / 多核标志 持久化）
 *
 *  存放在独立 Flash Config 区（见 flash_layout.h），与 bootscript 解耦：
 *  超频必须在调度器启动前（冷启动阶段）应用，而 bootscript 在调度器
 *  启动后才回放，故不能复用 bootscript 机制。
 *
 *  安全策略（用户需求）：
 *    · 未固化 / 损坏 / CRC 错 → 启动一律回到 单核 + 125MHz（tier 0）；
 *    · 只有显式 ovclk save 固化后，下次冷启动才应用档位 + 多核；
 *    · config_clear_all（ovclk reset）擦空 Config 区 → 恢复出厂默认。
 */
#include "hal/config_store.h"
#include "hal/sysclk.h"
#include "hal/flash_layout.h"
#include <string.h>

/* ---------- CRC32（IEEE 802.3 多项式，无表位循环实现） ---------- */
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

/* 计算 config_data_t 前 16 字节（不含 crc 字段）的 CRC */
static uint32_t config_calc_crc(const config_data_t *cfg) {
    return config_crc32((const uint8_t *)cfg, 16u);
}

void config_defaults(config_data_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->magic   = CONFIG_MAGIC;
    cfg->version = CONFIG_VERSION;
    cfg->clock_mhz = 0;                      /* 0 = 未设置 → 启动回 125MHz 默认 */
    cfg->multi_core = 0;                     /* 单核 */
    cfg->reserved[0] = 1;                    /* v2.7 demo_show：默认开（展示任务自动运行） */
    /* 其余 reserved 已 memset 为 0 */
    cfg->crc32 = config_calc_crc(cfg);
}

bool config_read(config_data_t *cfg) {
    const uint8_t *p = hal_flash_map_read(FLASH_LAYOUT_CONFIG_OFFSET);
    if (!p) { if (cfg) config_defaults(cfg); return false; }

    /* 未初始化（擦除态全 0xFF）→ 无效 */
    if (p[0] == 0xFFu) { if (cfg) config_defaults(cfg); return false; }

    config_data_t tmp;
    memcpy(&tmp, p, sizeof(tmp));
    if (tmp.magic != CONFIG_MAGIC || tmp.version != CONFIG_VERSION) {
        if (cfg) config_defaults(cfg);
        return false;
    }
    if (tmp.crc32 != config_calc_crc(&tmp)) {
        if (cfg) config_defaults(cfg);
        return false;
    }
    /* 频率合法性兜底：0=默认；非法值归档为 0（保持 125MHz） */
    if (tmp.clock_mhz != 0u &&
        (tmp.clock_mhz < SYSCLK_MHZ_MIN || tmp.clock_mhz > SYSCLK_MHZ_MAX)) {
        tmp.clock_mhz = 0u;
    }
    if (cfg) *cfg = tmp;
    return true;
}

hal_err_t config_write(const config_data_t *cfg) {
    if (!cfg) return HAL_ERR_PARAM;
    config_data_t tmp = *cfg;
    tmp.magic   = CONFIG_MAGIC;
    tmp.version = CONFIG_VERSION;
    tmp.crc32   = config_calc_crc(&tmp);

    /* Config 区 = 1 个 4 KiB 扇区：先整扇区擦除，再写 struct @@ */
    hal_err_t e = hal_flash_erase_sector(FLASH_LAYOUT_CONFIG_OFFSET);
    if (e != HAL_OK) return e;
    return hal_flash_program(FLASH_LAYOUT_CONFIG_OFFSET, (const uint8_t *)&tmp, sizeof(tmp));
}

hal_err_t config_clear_all(void) {
    return hal_flash_erase_sector(FLASH_LAYOUT_CONFIG_OFFSET);
}

/* 多核启动钩子：弱符号，默认 no-op。
 * 多核调度器实装后，由对应模块提供强定义以真正启动 core1。 */
__attribute__((weak)) void config_mcore_apply(bool enable) {
    (void)enable;
}

/* 冷启动一次性应用：sched_start 前调用。返回实际生效的主频 MHz。 */
int config_apply(void) {
    config_data_t cfg;
    bool valid = config_read(&cfg);
    if (!valid) {
        return SYSCLK_MHZ_DEFAULT;   /* 未固化 → 单核 + 125MHz，不动任何硬件 */
    }

    /* 1) 应用超频频率（失败则维持当前频率，不中断启动） */
    uint32_t mhz = (uint32_t)cfg.clock_mhz;
    if (mhz == 0u) mhz = SYSCLK_MHZ_DEFAULT;

    /* 【v2.4.2 · 极限档安全保护】
     * >250MHz 超出 RP2040 稳定范围，固化后冷启动若直接应用，可能导致
     * USB/系统异常甚至无法进入 shell 恢复。
     * 保护：固化的是极限频率时**冷启动不应用**（保持 125MHz），让系统
     * 始终可进 shell；config 区保留，用户可在 shell 里 `ovclk reset`
     * 清除，或 `ovclk set <安全频率>` 改回再 save。 */
    if (mhz > SYSCLK_MAX_SAFE_MHZ) {
        return SYSCLK_MHZ_DEFAULT;   /* 保持 125MHz，不应用超频 */
    }
    (void)sysclk_apply_mhz(mhz);

    /* 2) 应用多核标志 */
    config_mcore_apply(cfg.multi_core != 0);

    return (int)sysclk_current_mhz();   /* 实际生效主频 */
}