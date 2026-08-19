/**
 * @file    shmem_ipc.h
 * @brief   RP2350 共享内存 IPC 接口
 */

#ifndef SHMEM_IPC_H
#define SHMEM_IPC_H

#include "hal_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sent;
    uint32_t recv;
    uint32_t errors;
    uint32_t overflow;
} ipc_stats_t;

extern ipc_stats_t g_ipc_stats;

hal_err_t shmem_ipc_init(void);
hal_err_t shmem_ipc_send(uint32_t core_id, const void *data, size_t len);
hal_err_t shmem_ipc_recv(uint32_t core_id, void *data, size_t max_len, size_t *out_len);

/* Core 1 侧使用的反向接口 */
hal_err_t shmem_ipc_send_to_core0(const void *data, size_t len);
hal_err_t shmem_ipc_recv_from_core0(void *data, size_t max_len, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* SHMEM_IPC_H */