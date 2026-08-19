/**
 * @file    demo_app.c
 * @brief   RP2350 Demo 应用初始化
 */

#include "task.h"
#include "hal_interface.h"
#include "os_config.h"
#include "rp2350_port.h"
#include <stdio.h>

/* ================================================================
 * Demo 应用初始化
 * ================================================================ */

void demo_app_init(void) {
    printf("\r\n=== RP2350 Demo App Init ===\r\n");
    printf("CPU: Cortex-M33 @ %lu MHz\r\n", rp2350_clk_sys_hz() / 1000000);
    printf("Peripheral: %lu MHz\r\n", rp2350_clk_peri_hz() / 1000000);
    printf("RAM: 520KB, Flash: 4MB\r\n");
    printf("Dual Core SMP: Enabled\r\n");
    printf("TrustZone: Enabled\r\n");
    printf("===========================================\r\n\r\n");
    
    /* 初始化板载 LED */
    hal_led_init();
    hal_led_set(0);
    
    /* 创建 Shell 任务 */
    extern void shell_start(void);
    task_create("shell", shell_start, NULL, 4096, 2);
    
    /* 创建心跳任务 */
    task_create("heartbeat", heartbeat_task, NULL, 1024, 1);
    
    /* 创建 Core 1 测试任务 */
    task_create("core1_test", core1_test_task, NULL, 2048, 1);
}

/* ================================================================
 * 心跳任务
 * ================================================================ */

static void heartbeat_task(void *arg) {
    (void)arg;
    for (;;) {
        hal_led_set(1);
        task_sleep(500);
        hal_led_set(0);
        task_sleep(500);
    }
}

/* ================================================================
 * Core 1 测试任务
 * ================================================================ */

static void core1_test_task(void *arg) {
    (void)arg;
    uint32_t core = hal_core_id();
    printf("[Core %lu] Test task started\r\n", core);
    
    uint32_t counter = 0;
    for (;;) {
        task_sleep(2000);
        printf("[Core %lu] Heartbeat #%lu\r\n", core, counter++);
    }
}