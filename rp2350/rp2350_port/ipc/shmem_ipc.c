/**
 * @file    shmem_ipc.c
 * @brief   RP2350 共享内存核间通信 (IPC)
 * 
 * 使用 SHARED_RAM 区域 (8KB) 实现双核环形缓冲区通信
 */

#include "hal_port.h"
#include "flash_layout.h"
#include <string.h>

/* IPC 缓冲区大小 */
#define IPC_BUF_SIZE                  2048  /* 2KB per buffer */
#define IPC_BUF_MASK                  (IPC_BUF_SIZE - 1)

/* IPC 控制结构 (放在共享内存开头) */
typedef struct {
    volatile uint32_t magic;
    volatile uint32_t version;
    
    /* Core 0 -> Core 1 */
    volatile uint32_t in_head;
    volatile uint32_t in_tail;
    volatile uint8_t  in_buf[IPC_BUF_SIZE];
    
    /* Core 1 -> Core 0 */
    volatile uint32_t out_head;
    volatile uint32_t out_tail;
    volatile uint8_t  out_buf[IPC_BUF_SIZE];
    
    /* 统计 */
    volatile uint32_t sent;
    volatile uint32_t recv;
    volatile uint32_t errors;
    volatile uint32_t overflow;
} ipc_ctrl_t;

static ipc_ctrl_t *g_ipc = (ipc_ctrl_t *)&_ipc_ctrl;
ipc_stats_t g_ipc_stats = {0};

hal_err_t shmem_ipc_init(void) {
    /* 验证魔数 */
    if (g_ipc->magic != 0x4950434D) {  /* 'MIPC' */
        /* 首次初始化 */
        memset((void *)g_ipc, 0, sizeof(ipc_ctrl_t));
        g_ipc->magic = 0x4950434D;
        g_ipc->version = 1;
    }
    return HAL_OK;
}

static inline uint32_t ipc_space(uint32_t head, uint32_t tail) {
    return (head + IPC_BUF_SIZE - tail) & IPC_BUF_MASK;
}

hal_err_t shmem_ipc_send(uint32_t core_id, const void *data, size_t len) {
    (void)core_id;  /* 当前只支持发送到 Core 1 */
    
    if (!data || len == 0 || len > IPC_BUF_SIZE - 1) return HAL_ERR_INVAL;
    
    /* 简单协议：[len:2bytes][data...] */
    if (len + 2 > ipc_space(g_ipc->in_head, g_ipc->in_tail)) {
        g_ipc_stats.overflow++;
        g_ipc->overflow++;
        return HAL_ERR_FULL;
    }
    
    uint16_t nlen = (uint16_t)len;
    uint32_t head = g_ipc->in_head;
    
    /* 写长度 (小端) */
    g_ipc->in_buf[head] = nlen & 0xFF;
    head = (head + 1) & IPC_BUF_MASK;
    g_ipc->in_buf[head] = (nlen >> 8) & 0xFF;
    head = (head + 1) & IPC_BUF_MASK;
    
    /* 写数据 */
    const uint8_t *src = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        g_ipc->in_buf[head] = src[i];
        head = (head + 1) & IPC_BUF_MASK;
    }
    
    __DSB();  /* 确保写入可见 */
    g_ipc->in_head = head;
    g_ipc->sent++;
    g_ipc_stats.sent++;
    
    /* 触发 Core 1 中断 (SEV 或邮箱) */
    __SEV();
    
    return HAL_OK;
}

hal_err_t shmem_ipc_recv(uint32_t core_id, void *data, size_t max_len, size_t *out_len) {
    (void)core_id;
    
    if (!data || !out_len) return HAL_ERR_INVAL;
    
    if (g_ipc->out_head == g_ipc->out_tail) {
        return HAL_ERR_EMPTY;
    }
    
    uint32_t tail = g_ipc->out_tail;
    
    /* 读长度 */
    uint16_t nlen = g_ipc->out_buf[tail] | (g_ipc->out_buf[(tail + 1) & IPC_BUF_MASK] << 8);
    tail = (tail + 2) & IPC_BUF_MASK;
    
    if (nlen > max_len) {
        g_ipc->errors++;
        g_ipc_stats.errors++;
        return HAL_ERR_INVAL;
    }
    
    /* 读数据 */
    uint8_t *dst = (uint8_t *)data;
    for (uint16_t i = 0; i < nlen; i++) {
        dst[i] = g_ipc->out_buf[tail];
        tail = (tail + 1) & IPC_BUF_MASK;
    }
    
    __DSB();
    g_ipc->out_tail = tail;
    g_ipc->recv++;
    g_ipc_stats.recv++;
    *out_len = nlen;
    
    return HAL_OK;
}

/* Core 1 侧调用的发送/接收 (反向) */
hal_err_t shmem_ipc_send_to_core0(const void *data, size_t len) {
    if (!data || len == 0 || len > IPC_BUF_SIZE - 1) return HAL_ERR_INVAL;
    
    if (len + 2 > ipc_space(g_ipc->out_head, g_ipc->out_tail)) {
        g_ipc->overflow++;
        return HAL_ERR_FULL;
    }
    
    uint16_t nlen = (uint16_t)len;
    uint32_t head = g_ipc->out_head;
    
    g_ipc->out_buf[head] = nlen & 0xFF;
    head = (head + 1) & IPC_BUF_MASK;
    g_ipc->out_buf[head] = (nlen >> 8) & 0xFF;
    head = (head + 1) & IPC_BUF_MASK;
    
    const uint8_t *src = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        g_ipc->out_buf[head] = src[i];
        head = (head + 1) & IPC_BUF_MASK;
    }
    
    __DSB();
    g_ipc->out_head = head;
    g_ipc->sent++;
    
    __SEV();
    return HAL_OK;
}

hal_err_t shmem_ipc_recv_from_core0(void *data, size_t max_len, size_t *out_len) {
    if (!data || !out_len) return HAL_ERR_INVAL;
    
    if (g_ipc->in_head == g_ipc->in_tail) {
        return HAL_ERR_EMPTY;
    }
    
    uint32_t tail = g_ipc->in_tail;
    uint16_t nlen = g_ipc->in_buf[tail] | (g_ipc->in_buf[(tail + 1) & IPC_BUF_MASK] << 8);
    tail = (tail + 2) & IPC_BUF_MASK;
    
    if (nlen > max_len) {
        g_ipc->errors++;
        return HAL_ERR_INVAL;
    }
    
    uint8_t *dst = (uint8_t *)data;
    for (uint16_t i = 0; i < nlen; i++) {
        dst[i] = g_ipc->in_buf[tail];
        tail = (tail + 1) & IPC_BUF_MASK;
    }
    
    __DSB();
    g_ipc->in_tail = tail;
    g_ipc->recv++;
    *out_len = nlen;
    return HAL_OK;
}