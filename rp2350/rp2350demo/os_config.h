/**
 * @file    os_config.h
 * @brief   RP2350 Demo 工程裁剪配置
 * 
 * 适配 RP2350 (Cortex-M33 + 可选 RISC-V Hazard3)
 */

#ifndef OS_CONFIG_H
#define OS_CONFIG_H

/* ================================================================
 * 1. 体积红线
 * ================================================================ */
#define OS_CFG_MIN_KERNEL_FLASH_KB   10
#define OS_CFG_MIN_KERNEL_RAM_KB      4
#define OS_CFG_FULL_BASE_FLASH_KB    512   /* RP2350 有 4MB Flash */
#define OS_CFG_FULL_BASE_RAM_KB      128   /* RP2350 有 520KB RAM */

/* ================================================================
 * 2. 核心功能裁剪
 * ================================================================ */
#define OS_CFG_TASK_MODULE            1
#define OS_CFG_SCHED_RR               1
#define OS_CFG_SCHED_WEIGHT           1    /* RP2350 性能足够支持权重调度 */
#define OS_CFG_MEM_POOL               1
#define OS_CFG_KERNEL_HEAP            1
#define OS_CFG_STACK_OVF_CHECK        1
#define OS_CFG_SYSCALL                1
#define OS_CFG_PARAM_CHECK            1

/* ================================================================
 * 3. 扩展模块裁剪
 * ================================================================ */
#define OS_CFG_PERIPH_SERVICE         1
#define OS_CFG_SHELL                  1
#define OS_CFG_VFS                    0
#define OS_CFG_FATFS                  1
#define OS_CFG_LOADER                 0
#define OS_CFG_DEMO_APP               1
#define OS_CFG_SHOW_DEMO              0    /* 无 OLED 展示 */

/* ================================================================
 * 4. 平台专属功能 (RP2350)
 * ================================================================ */
#define OS_CFG_OVCLK                  1    /* 超频/时钟配置 */
#define OS_CFG_MULTICORE              1    /* 双核 SMP */

/* ================================================================
 * 5. 平台移植开关
 * ================================================================ */
#define OS_CFG_PORT_RP2350            1    /* RP2350 目标工程 */

/* ================================================================
 * 6. 双核共享内存 IPC
 * ================================================================ */
#define OS_CFG_IPC                    1

/* ================================================================
 * 7. RISC-V Hazard3 核心支持 (可选)
 * ================================================================ */
#define OS_CFG_RISCV                  0    /* 默认只用 Cortex-M33，如需 RISC-V 置 1 */

/* ================================================================
 * 8. TrustZone 安全扩展
 * ================================================================ */
#define OS_CFG_TRUSTZONE              1    /* 启用 TrustZone 支持 */

/* ================================================================
 * 9. 资源上限
 * ================================================================ */
#define OS_CFG_MAX_TASKS              32   /* 更多任务 */
#define OS_CFG_CORE_COUNT             2    /* 双核 SMP */
#define OS_CFG_MAX_SEM                16
#define OS_CFG_MAX_MUTEX              8
#define OS_CFG_MAX_QUEUE              16
#define OS_CFG_MAX_TIMERS             8
#define OS_CFG_HEAP_SIZE_BYTES        (128*1024)  /* 128KB 内核堆 */
#define OS_CFG_IDLE_STACK_SIZE        4096
#define OS_CFG_DEFAULT_TASK_STACK     2048

/* 用户栈限额 */
#define OS_CFG_USER_STACK_POOL_BYTES  (128*1024)
#define OS_CFG_USER_STACK_WARN_PCT    80

/* ================================================================
 * 10. 调度与时间基准
 * ================================================================ */
#define OS_CFG_TICK_HZ                1000
#define OS_CFG_TIME_SLICE_TICKS       5
#define OS_CFG_TICKLESS               0

/* ================================================================
 * 11. 调试与诊断
 * ================================================================ */
#define OS_CFG_DEBUG                  1
#define OS_CFG_ASSERT                 1
#define OS_CFG_SHELL_HISTORY          16
#define OS_CFG_PROFILING              1    /* 启用性能分析 */

/* ================================================================
 * 12. 架构相关
 * ================================================================ */
#define OS_CFG_CPU_ARM_CORTEX_M       1
#define OS_CFG_CPU_ARM_CORTEX_M33     1    /* Cortex-M33 特有 */
#define OS_CFG_CPU_ENDIAN_LITTLE      1
#define OS_CFG_ALIGN_SIZE             8

/* ================================================================
 * 13. 编译期断言
 * ================================================================ */
#if (OS_CFG_MIN_KERNEL_FLASH_KB > 10) || (OS_CFG_MIN_KERNEL_RAM_KB > 4)
#error "最小内核体积超标，请精简配置或调整红线"
#endif

/* TrustZone 要求 */
#if OS_CFG_TRUSTZONE && !OS_CFG_PORT_RP2350
#error "TrustZone requires OS_CFG_PORT_RP2350=1"
#endif

#endif /* OS_CONFIG_H */