/**
 * @file    main.c
 * @brief   RP2040 Demo 应用入口（mini-kernel 独立应用工程示例）
 *
 *  mini-kernel/kernel/core/kernel.c 中已提供 __attribute__((weak)) main()
 *  实现（关中断 → kernel_main → 永不返回），这里的强定义 main() 覆盖
 *  内核的弱 main，使得：
 *    · rp2040demo 完全掌控自己的启动入口（可在此处做应用级前置初始化）
 *    · 调用 kernel_main() 让内核起 HAL / 内存 / 调度器
 *    · 若需要创建额外应用任务，可在 kernel_main() 之前调用
 *      "app_task_preinstall()" 之类的钩子（此处留空示例）
 */
#include "kernel.h"

extern void kernel_main(void);

/* 应用级前置初始化（示例：此处为空，留给用户扩展）。
 * 注意：
 *   · 进入 main 前，Pico SDK crt0.S 已做 MSP 设置 / .bss 清零 / data 搬迁。
 *   · 关中断必须在调用 kernel_main 之前做。kernel.c 的 weak main() 内部
 *     已经包含 cpsid i，但此处强 main 必须自己做同样保护。 */
static inline void _app_init_early(void) {
    /* 预留位：若应用有额外的 boot ROM 配置 / watchdog 设置，放这里。 */
}

int main(void) {
    /* 与 kernel.c weak main 同步：启动阶段关中断，防止 kmem/task 初始化
     * 未完成时 SDK alarm pool / TinyUSB 回调已经中断触发。 */
    __asm volatile ("cpsid i" ::: "memory");

    _app_init_early();

    /* 进入内核：初始化 HAL / 内存 / 任务 / SysTick / 调度器并永不返回 */
    kernel_main();

    return 0;
}
