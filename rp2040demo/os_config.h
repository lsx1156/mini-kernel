/**
 * @file    os_config.h
 * @brief   RP2040 Demo 目标工程裁剪配置 —— 完整基础版（含 Shell + FatFs + MSC +
 *          Periph + 超频/多核分解 + demo）
 *
 *  与纯内核库（mini-kernel/include/os_config.h）分离：本文件启用全部
 *  RP2040 专属功能，并置 OS_CFG_PORT_RP2040=1 编译 rp2040_port 移植层。
 */
#ifndef OS_CONFIG_H
#define OS_CONFIG_H

/* ================================================================
 * 1. 体积红线（刚性约束，超限编译报错）
 * ================================================================ */
#define OS_CFG_MIN_KERNEL_FLASH_KB   10   /* 最小内核 Flash 上限 (KB) */
#define OS_CFG_MIN_KERNEL_RAM_KB      4   /* 最小内核 RAM  上限 (KB) */
#define OS_CFG_FULL_BASE_FLASH_KB    20   /* 完整基础版 Flash 上限 (KB) */
#define OS_CFG_FULL_BASE_RAM_KB       8   /* 完整基础版 RAM  上限 (KB) */

/* ================================================================
 * 2. 核心功能裁剪（关闭=零代码、零开销）
 * ================================================================ */
#define OS_CFG_TASK_MODULE            1   /* 任务管理（必选，不可关） */
#define OS_CFG_SCHED_RR               1   /* 时间片轮转调度（必选）     */
#define OS_CFG_SCHED_WEIGHT           0   /* 权重调度（可选，仅影响时间片比例） */
#define OS_CFG_MEM_POOL               1   /* 固定内存池（必选）         */
#define OS_CFG_KERNEL_HEAP            1   /* 内核堆 kmalloc/kfree（可选）  */
#define OS_CFG_STACK_OVF_CHECK        1   /* 栈溢出魔值检测（建议开）    */
#define OS_CFG_SYSCALL                1   /* 系统调用层（必选）          */
#define OS_CFG_PARAM_CHECK            1   /* 系统调用参数校验（建议开）   */

/* ================================================================
 * 3. 扩展模块裁剪（按需开启，关闭后代码完全不编入）
 * ================================================================ */
#define OS_CFG_PERIPH_SERVICE         1   /* 共享总线服务（GPIO/SPI/I2C/UART）—— shell 命令直接操纵 */
#define OS_CFG_SHELL                  1   /* 命令行终端                   */
#define OS_CFG_VFS                    0   /* 虚拟文件系统接口 —— 模块暂未实现（直接用 FatFs，不经过 VFS 抽象）*/
#define OS_CFG_FATFS                  1   /* Elm FatFs（FAT16，MSC U 盘 + 本机文件目录命令）*/
#define OS_CFG_LOADER                 0   /* 用户程序加载器（需 VFS=1）    */
#define OS_CFG_DEMO_APP               1   /* 启动演示应用（4 个演示任务）  */

/* ================================================================
 * 3.5 平台专属功能裁剪（RP2040 目标工程启用）
 * ================================================================ */
#define OS_CFG_OVCLK                  1   /* 超频/多核固化（config_store + sysclk + ovclk 命令） */
#define OS_CFG_MULTICORE              1   /* 多核调度（mcore 命令 + hal_mcore_*） */

/* ================================================================
 * 3.6 平台移植开关（RP2040 目标工程置 1）
 * ================================================================ */
#define OS_CFG_PORT_RP2040            1   /* 0=纯内核库；1=RP2040 目标工程 */

/* ================================================================
 * 3.7 双核共享内存 IPC（v2.6 新增）
 *    Core0（内核）↔ Core1（SRAM 镜像 worker）：SIO FIFO 事件通知 +
 *    2x16KB 乒乓缓冲（所有权随消息转移，无锁无撕裂）。
 *    内存映射见 rp2040_port/ipc/shmem_ipc.h 与 memmap_ipc.ld
 *    （RAM 条带区缩至 128KB，bank1/2/3 别名区给 Core1 + 共享缓冲）。
 * ================================================================ */
#define OS_CFG_IPC                    1   /* 1=编译 ipc 命令组与 IPC 库 */

/* ================================================================
 * 4. 资源上限（决定静态数组大小，超限编译期报错）
 * ================================================================ */
#define OS_CFG_MAX_TASKS              16  /* 最大任务数（含空闲任务）     */
#define OS_CFG_CORE_COUNT             2   /* CPU 核心数（RP2040 双核）。*/
#define OS_CFG_MAX_SEM                8   /* 最大信号量数                 */
#define OS_CFG_MAX_MUTEX              4   /* 最大互斥锁数                 */
#define OS_CFG_MAX_QUEUE              8   /* 最大消息队列数               */
#define OS_CFG_MAX_TIMERS             4   /* 最大软件定时器数             */
#define OS_CFG_HEAP_SIZE_BYTES        (16*1024) /* 内核堆大小（任务栈+TCB+kmalloc） */
#define OS_CFG_IDLE_STACK_SIZE        1024 /* 空闲任务栈 (字节)           */
#define OS_CFG_DEFAULT_TASK_STACK     512  /* 默认任务栈 (字节)            */

/* ================================================================
 * 4.1 用户栈限额（统一 8KB，>80% 时由 VT3/vtest status 告警）
 * ================================================================ */
#define OS_CFG_USER_STACK_POOL_BYTES  (8*1024) /* 用户栈总限额 8KB             */
#define OS_CFG_USER_STACK_WARN_PCT    80      /* 告警阈值 (%)                */

/* ================================================================
 * 5. 调度与时间基准
 * ================================================================ */
#define OS_CFG_TICK_HZ                1000    /* 系统滴答频率 (Hz)        */
#define OS_CFG_TIME_SLICE_TICKS       5       /* 默认时间片 (tick)        */
#define OS_CFG_TICKLESS               0       /* Tickless 低功耗（暂不支持） */

/* ================================================================
 * 6. 调试与诊断
 * ================================================================ */
#define OS_CFG_DEBUG                  1       /* 开启调试日志              */
#define OS_CFG_ASSERT                 1       /* 断言检查                  */
#define OS_CFG_SHELL_HISTORY          8       /* Shell 历史命令条数        */
#define OS_CFG_PROFILING              0       /* 运行时剖析               */

/* ================================================================
 * 7. 架构相关（由 HAL 移植层提供，应用层勿改）
 * ================================================================ */
#define OS_CFG_CPU_ARM_CORTEX_M       1   /* 1=Cortex-M, 0=RISC-V 等    */
#define OS_CFG_CPU_ENDIAN_LITTLE      1   /* 1=小端, 0=大端             */
#define OS_CFG_ALIGN_SIZE             8   /* 内存对齐粒度 (字节)        */

/* ================================================================
 * 8. 编译期断言：体积红线自检
 * ================================================================ */
#if (OS_CFG_MIN_KERNEL_FLASH_KB > 10) || (OS_CFG_MIN_KERNEL_RAM_KB > 4)
#error "最小内核体积超标，请精简配置或调整红线"
#endif

#endif /* OS_CONFIG_H */