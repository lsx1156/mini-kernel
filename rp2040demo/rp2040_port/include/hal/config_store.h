/**
 * @file    config_store.h
 * @brief   v2.4 系统固化配置存储 — 超频档位 / 多核标志 持久化
 *
 *  方案（对应需求）：
 *    · 默认单核 + 125MHz（tier 0）。用户通过 shell 指令（ovclk）显式保存
 *      超频档位、多核使能后，固化到独立 Flash Config 区，掉电不丢。
 *    · 冷启动读取 Config 区：**有效** → 应用所存档位 + 多核；
 *      **无效 / 未固化 / CRC 损坏** → 一律回到默认（单核 + 125MHz），
 *      绝不冒进，保证安全兜底，不会对硬件造成不可逆损害。
 *    · 恢复出厂：ovclk reset 或 config_clear_all() 把 Config 区擦回空，
 *      下次启动即恢复单核 + 原始频率。
 *
 *  存储布局（独立于 bootscript，位于 MSC 之后、bootscript SEC_A 之前）：
 *    · 一个 4 KiB 扇区（W25Q 擦除粒度），offset = FLASH_LAYOUT_CONFIG_OFFSET
 *    · 起始处放 config_data_t（固定 20 字节，前 16 字节 CRC32 保护）
 *    · 其余扇区保留 0xFF（擦除态），将来扩展字段可加在 struct 内。
 */
#ifndef HAL_CONFIG_STORE_H
#define HAL_CONFIG_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_interface.h"   /* hal_err_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 魔数与版本 ---------- */
#define CONFIG_MAGIC          0x4D4B3243u   /* 'MK2C' */
#define CONFIG_VERSION        2u            /* v2.4.2：clock_tier(档位号) → clock_mhz(任意 MHz) */

/* ---------- 固化配置数据结构（固定 20 字节） ---------- */
typedef struct {
    uint32_t magic;        /* == CONFIG_MAGIC，判断扇区是否已初始化 */
    uint16_t version;      /* == CONFIG_VERSION */
    uint16_t clock_mhz;    /* 超频主频 MHz：0=未设置（启动回 125MHz 默认） */
    uint8_t  multi_core;   /* 多核使能：0=单核（默认） 1=双核 */
    uint8_t  reserved[7];  /* 保留，将来扩展，当前恒 0x00 */
    uint32_t crc32;        /* CRC32(前 16 字节) */
} config_data_t;

/* ---------- API ---------- */

/* 用默认值（未固化语义）填充 cfg：magic/version 置位、clock_mhz=0、multi_core=0 */
void config_defaults(config_data_t *cfg);

/* 读取 Config 区。返回 true = 有效（已固化且 CRC 正确），cfg 被填充；
 * 返回 false = 无效/未初始化/损坏，cfg 被填充为默认值。 */
bool config_read(config_data_t *cfg);

/* 写入 Config 区（整扇区擦除 + 重写 struct）。返回 hal_err_t。 */
hal_err_t config_write(const config_data_t *cfg);

/* 擦除 Config 区（恢复出厂：下次启动回到单核 + 125MHz）。返回 hal_err_t。 */
hal_err_t config_clear_all(void);

/* 冷启动一次性应用：内核在 sched_start 前调用。
 *   · Config 有效 → 应用超频频率 + 多核；
 *   · 无效 → 保持默认（单核 + 125MHz），不做任何改动。
 *   返回本次实际生效的主频 MHz（默认 125）。 */
int config_apply(void);

/* 多核启动钩子（弱符号，默认 no-op）。多核调度器实现后由对应模块提供强定义，
 *   enable=true 时启动 core1 并进入双核调度。 */
void config_mcore_apply(bool enable);

#ifdef __cplusplus
}
#endif

#endif /* HAL_CONFIG_STORE_H */