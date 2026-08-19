/**
 * @file    config_store.h
 * @brief   RP2350 持久化配置存储接口
 */

#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include "hal_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

hal_err_t config_store_init(void);
hal_err_t config_store_load(void);
hal_err_t config_store_save(void);
hal_err_t config_store_get(const char *key, char *value, size_t max_len);
hal_err_t config_store_set(const char *key, const char *value);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_STORE_H */