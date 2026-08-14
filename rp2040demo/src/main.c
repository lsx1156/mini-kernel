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
    /* ================================================================
     * 最小启动诊断：先做最基础 LED 闪烁测试，不依赖任何内核模块。
     * 如果能看到 LED 快闪 5 次 → 启动/链接/GPIO 配置没问题，崩溃在后面。
     * 如果完全不亮 → 向量表/链接/启动汇编有问题。
     * ================================================================ */

    /* Step 1: 直接写寄存器初始化 GPIO25 (板载 LED) */
    /* PADS_BANK0: GPIO25 — DRIVE=1(4mA) SCHMITT=1 SLEWFAST=0 */
    *(volatile uint32_t *)(0x4001C000u + 0x6C) = 0x00000056u;
    /* IO_BANK0: GPIO25 = FUNC5 (SIO) */
    *(volatile uint32_t *)(0x40014000u + 0x0CC) = 0x00000005u;
    /* SIO GPIO_OE set (output enable) */
    *(volatile uint32_t *)(0xD0000000u + 0x024) = 0x02000000u;

    /* Step 2: 快速闪 5 次（每次 100ms on / 100ms off，共 ~1s） */
    for (volatile int i = 0; i < 5; i++) {
        *(volatile uint32_t *)(0xD0000000u + 0x014) = 0x02000000u;  /* ON */
        for (volatile uint32_t j = 0; j < 1200000; j++) __asm("nop");
        *(volatile uint32_t *)(0xD0000000u + 0x018) = 0x02000000u;  /* OFF */
        for (volatile uint32_t j = 0; j < 1200000; j++) __asm("nop");
    }

    /* Step 3: LED 长亮 2 秒，提示正在进入 kernel_main */
    *(volatile uint32_t *)(0xD0000000u + 0x014) = 0x02000000u;  /* ON */
    for (volatile uint32_t j = 0; j < 12000000; j++) __asm("nop");
    *(volatile uint32_t *)(0xD0000000u + 0x018) = 0x02000000u;  /* OFF */

    /* 现在才进入内核：关中断，正常内核冷启动流程 */
    __asm volatile ("cpsid i" ::: "memory");

    _app_init_early();

    /* 进入内核：初始化 HAL / 内存 / 任务 / SysTick / 调度器并永不返回 */
    kernel_main();

    return 0;
}
