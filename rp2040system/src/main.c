/**
 * @file    main.c
 * @brief   RP2040 系统入口：LED 诊断闪烁 + 启动 mini-kernel
 *
 *  与 rp2040demo 的 main.c 完全一致（无展示功能，仅启动内核 + Shell）。
 *
 *  启动诊断序列（上电即可看到）：
 *    1. 5 次快闪 (250ms ON / 250ms OFF) — 表示 boot2 → Reset_Handler →
 *       runtime_init (.data/.bss) → main() 整条链路完全正常；
 *    2. 常亮 2 秒 — 表示即将进入内核，观察是否卡死在 kernel_main 早期；
 *    3. 调用 kernel_main() → 内核接管，进入正常心跳 (500ms 闪)。
 *
 *  如果连第 1 步都看不到 = crt0 / runtime_init 阶段直接 HardFault
 *    → 查 multilib / crt0.o 是否为 thumb/v6-m/nofp 版。
 */
#include "pico/stdlib.h"

#ifndef PICO_DEFAULT_LED_PIN
#  define PICO_DEFAULT_LED_PIN 25
#endif

/* mini-kernel 内核入口（由 mini-kernel 静态库导出） */
extern void kernel_main(void);
/* 【v2.6.4】UART0 诊断口初始化 + FIFO/PSM 上电快照。
 * kernel.c 的 weak main 被本文件强 main 覆盖后，hal_diag_init 从未被
 * 调用（链接器 GC）——在此显式补回，并成为启动期最早的快照点 A。 */
extern void hal_diag_init(void);

int main(void) {
    const uint LED = (uint)PICO_DEFAULT_LED_PIN;

    /* A 点快照：crt0 → main 第一句（LED 诊断闪烁之前），TTL 输出 */
    hal_diag_init();

    /* --- 诊断阶段 1：5 次 250ms 快闪 --- */
    gpio_init(LED);
    gpio_set_dir(LED, GPIO_OUT);
    gpio_set_slew_rate(LED, GPIO_SLEW_RATE_SLOW);
    gpio_set_drive_strength(LED, GPIO_DRIVE_STRENGTH_4MA);

    for (int i = 0; i < 5; i++) {
        gpio_put(LED, 1);  /* ON */
        sleep_ms(250);
        gpio_put(LED, 0);  /* OFF */
        sleep_ms(250);
    }

    /* --- 诊断阶段 2：常亮 2 秒（表示即将进入 kernel_main） --- */
    gpio_put(LED, 1);
    sleep_ms(2000);
    gpio_put(LED, 0);

    /* --- 阶段 3：进入内核（不再返回） --- */
    kernel_main();

    /* 不应到达此处；内核异常退出时 5Hz 爆闪 */
    while (1) {
        gpio_put(LED, 1);
        sleep_ms(100);
        gpio_put(LED, 0);
        sleep_ms(100);
    }
    return 0;
}