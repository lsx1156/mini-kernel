/**
 * @file    os_config.h
 * @brief   系统裁剪配置模板 —— 所有可选功能、体积红线、调试开关在此集中定义
 * @note    用户工程仅需复制一份重命名为 os_config.h 并修改宏即可，内核源码零修改
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
#define OS_CFG_VFS                    0   /* 虚拟文件系统接口 —— 模块暂未实现（v2.2 直接用 FatFs，不经过 VFS 抽象）*/
#define OS_CFG_FATFS                  1   /* Elm FatFs（FAT16，MSC U 盘 + 本机文件目录命令）*/
#define OS_CFG_LOADER                 0   /* 用户程序加载器（需 VFS=1）    */
#define OS_CFG_DEMO_APP               1   /* 启动演示应用（4 个演示任务）  */

/* ================================================================
 * 4. 资源上限（决定静态数组大小，超限编译期报错）
 * ================================================================ */
#define OS_CFG_MAX_TASKS              16  /* 最大任务数（含空闲任务）     */
#define OS_CFG_MAX_SEM                8   /* 最大信号量数                 */
#define OS_CFG_MAX_MUTEX              4   /* 最大互斥锁数                 */
#define OS_CFG_MAX_QUEUE              8   /* 最大消息队列数               */
#define OS_CFG_MAX_TIMERS             4   /* 最大软件定时器数             */
/* 内核堆大小（任务栈 + TCB + kmalloc 都从此分配，统一管理）：
 *   - 最小内核（关 Shell/VFS/Periph）：4KB 足够（仅 boot_setup 任务）
 *   - 完整基础版（含 Shell + VFS + Periph + demo）：需 8KB
 *     7 个任务栈合计 ~4KB + 7×TCB(~56B) + mem_pool bitmap + 碎片缓冲。
 * 当前配置开启了完整基础版，使用 8KB。裁剪时按比例调小。 */
#define OS_CFG_HEAP_SIZE_BYTES        (8*1024)  /* 内核堆大小，0=关闭堆     */
#define OS_CFG_IDLE_STACK_SIZE        256 /* 空闲任务栈 (字节)            */
#define OS_CFG_DEFAULT_TASK_STACK     512 /* 默认任务栈 (字节)            */

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
#define OS_CFG_PROFILING              0       /* 运行时剖析（任务切换计数） */

/* ================================================================
 * 7. 架构相关（由 HAL 移植层提供，应用层勿改）
 * ================================================================ */
#define OS_CFG_CPU_ARM_CORTEX_M       1   /* 1=Cortex-M, 0=RISC-V 等    */
#define OS_CFG_CPU_ENDIAN_LITTLE      1   /* 1=小端, 0=大端             */
#define OS_CFG_ALIGN_SIZE             8   /* 内存对齐粒度 (字节)        */

/* ================================================================
 * 8. 编译期断言：体积红线自检（链接后由构建脚本二次校验）
 * ================================================================ */
#if (OS_CFG_MIN_KERNEL_FLASH_KB > 10) || (OS_CFG_MIN_KERNEL_RAM_KB > 4)
#error "最小内核体积超标，请精简配置或调整红线"
#endif

#endif /* OS_CONFIG_H */