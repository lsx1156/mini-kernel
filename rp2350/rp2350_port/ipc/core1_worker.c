/**
 * @file    core1_worker.c
 * @brief   RP2350 Core 1 工作线程
 * 
 * Core 1 独立运行的内核实例，处理：
 * 1. IPC 消息处理
 * 2. Core 1 本地任务调度
 * 3. 专用外设驱动 (可选)
 */

#include "hal_port.h"
#include "shmem_ipc.h"
#include "task.h"
#include "kernel.h"
#include <stdio.h>

/* Core 1 专用内核上下文 */
static kernel_t g_core1_kernel;
static bool g_core1_kernel_inited = false;

static void core1_ipc_handler(void) {
    char buf[256];
    size_t len;
    
    while (shmem_ipc_recv_from_core0(buf, sizeof(buf), &len) == HAL_OK) {
        /* 处理来自 Core 0 的命令 */
        if (strncmp(buf, "echo:", 5) == 0) {
            shmem_ipc_send_to_core0(buf, len);
        } else if (strncmp(buf, "task:", 5) == 0) {
            /* 在 Core 1 创建任务 */
            char task_name[64];
            snprintf(task_name, sizeof(task_name), "core1_%s", buf + 5);
            task_create(task_name, core1_test_task, NULL, 2048, 1);
            shmem_ipc_send_to_core0("task created", 13);
        } else if (strncmp(buf, "stat:", 5) == 0) {
            char stat[128];
            snprintf(stat, sizeof(stat), "core1: tasks=%d, heap=%lu", 
                     g_core1_kernel.task_count, g_core1_kernel.heap_free);
            shmem_ipc_send_to_core0(stat, strlen(stat) + 1);
        }
    }
}

static void core1_test_task(void *arg) {
    (void)arg;
    uint32_t counter = 0;
    for (;;) {
        task_sleep(2000);
        counter++;
        /* 定期向 Core 0 发送心跳 */
        char msg[64];
        snprintf(msg, sizeof(msg), "heartbeat #%lu", counter);
        shmem_ipc_send_to_core0(msg, strlen(msg) + 1);
    }
}

void core1_kernel_init(void) {
    if (g_core1_kernel_inited) return;
    
    /* 初始化 Core 1 独立的内核实例 */
    kernel_config_t cfg = {
        .tick_hz = 1000,
        .time_slice = 5,
        .heap_size = 64 * 1024,
        .max_tasks = 16,
        .core_id = 1,
    };
    
    kernel_init(&g_core1_kernel, &cfg);
    g_core1_kernel_inited = true;
}

void core1_worker_entry(void *arg) {
    (void)arg;
    
    /* 1. 初始化硬件 (Core 1 专用) */
    rp2350_port_init();
    
    /* 2. 初始化 Core 1 内核 */
    core1_kernel_init();
    
    /* 3. 初始化 IPC */
    shmem_ipc_init();
    
    /* 4. 创建 Core 1 本地任务 */
    task_create("ipc_handler", core1_ipc_handler, NULL, 1024, 2);
    task_create("heartbeat", core1_test_task, NULL, 2048, 1);
    
    /* 5. 启动 Core 1 调度器 */
    kernel_start(&g_core1_kernel);
    
    /* 不应返回 */
    while (1) __wfi();
}