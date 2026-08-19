/**
 * @file    main.c
 * @brief   RP2350 Demo 应用入口
 */

#include "hal_interface.h"
#include "os_config.h"
#include "rp2350_port.h"

#include <stdio.h>

/* 内核主入口 */
extern void kernel_main(void);

/* 弱引用 demo_app_init */
extern __attribute__((weak)) void demo_app_init(void);

/* ================================================================
 * main - 系统入口
 * ================================================================ */
int main(void) {
    /* 1. 初始化移植层 (硬件初始化) */
    rp2350_port_init();
    
    /* 2. 启动内核 */
    kernel_main();
    
    /* 不应返回 */
    while (1) {
        __wfi();
    }
    
    return 0;
}