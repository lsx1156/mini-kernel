/**
 * @file    sysclk.h
 * @brief   v2.4 系统时钟档位（超频）抽象接口 — 硬件无关声明
 *
 *  RP2040 双核共享单一 SYS PLL，因此"超频"对两个核心一视同仁：
 *  无论跑在哪个核心，CPU 频率都同升同降。本接口只负责把 CPU 主频
 *  切到某个预置档位，并保证**外设总线（CLK_PERI）速度不变**（钳回
 *  125MHz），从而外设（UART/I2C/SPI/Timer）的波特率与节拍不受影响。
 *
 *  档位设计（多个预置档，运行时只改 PLL 参数即可切换，
 *   全部取 125MHz 的整数倍，保证 clk_peri 整数分频精确、UART 波特率不偏）：
 *    tier 0 = 125MHz（默认 / 安全兜底）
 *    tier 1 = 250MHz
 *    tier 2 = 375MHz
 *    tier 3 = 500MHz（隐藏档）
 *
 *  安全策略：
 *    · 未固化（config 无效）→ 启动一律回到 tier 0 + 单核，绝不冒进；
 *    · 只有用户显式 save 固化后，下次冷启动才应用该档位；
 *    · 改变档位时同步缩放 XIP flash 的 SSI 分频，保持 flash 读取速度
 *      不超安全值，避免高主频下 XIP 读崩（看起来像代码损坏/HardFault）。
 */
#ifndef HAL_SYSCLK_H
#define HAL_SYSCLK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 档位定义 ---------- */
#define SYSCLK_TIER_DEFAULT   0u      /* 125MHz，默认/安全兜底 */
#define SYSCLK_TIER_COUNT     4u      /* 0..3，共 4 档 */
#define SYSCLK_MHZ_DEFAULT    125u
#define SYSCLK_PERI_MHZ       125u    /* 外设总线钳回目标（保持不变，整数分频） */
#define SYSCLK_FLASH_MAX_MHZ  66u     /* XIP flash 串行时钟安全上限（MHz）。
                                       * v2.4.3：实测 300MHz 档把 flash 推到 100MHz 会
                                       * XIP 取指损坏（HardFault PC 跳到 .data，且故障
                                       * 处理代码自身也被乱码）→ 降档到 ≤62.5MHz 稳定区间。 */
#define SYSCLK_MAX_SAFE_MHZ   250u    /* 冷启动安全应用的主频上限：>250MHz 的极限档固化后不自动应用 */

/* ---------- 任意频率输入（v2.4.2） ----------
 * 除预设档外，shell 也允许直接输入 MHz（ovclk try/set <MHz>），由移植层
 * 自动匹配：SYS PLL 整数分频（就近锁定可达频率）、XIP flash 分频（钳 ≤
 * SYSCLK_FLASH_MAX_MHZ）、clk_peri 整数分频（钳 ≤ SYSCLK_PERI_MAX_MHZ）、
 * 核心电压（按频率分档）。 */
#define SYSCLK_MHZ_MIN        100u    /* 任意频率输入下限（MHz） */
#define SYSCLK_MHZ_MAX        500u    /* 任意频率输入上限（MHz） */
#define SYSCLK_PERI_MAX_MHZ   133u    /* clk_peri 自动整数分频上限（MHz，RP2040 外设规格） */

/* 注意事项（v2.4.2 · 超频各子系统的正确做法）：
 *   · SysTick/节拍：用 RP2040 硬件 TIMER（独立于 CPU 主频），不随超频漂移；
 *     若改用 ARM Core SysTick 则必须按真实 sys_clk 重算重载值。
 *   · Flash 时钟：分频加大、钳在 SYSCLK_FLASH_MAX_MHZ 以内给足余量，
 *     杜绝高主频下 XIP 读崩（见 hal_port.c sysclk_apply_tier）。
 *   · UART/PWM/SPI/I2C：clk_peri 恒 125MHz（整数分频），超频后所有外设
 *     保持原波特率/参数；外设初始化用 SDK 按真实 clk_peri 算分频，
 *     绝不复制 133MHz 档的硬编码配置。
 *   · pll_usb：保持独立 48MHz（set_sys_clock_khz 不触碰 pll_usb）。 */

typedef struct {
    uint32_t mhz;        /* 该档 CPU 主频（MHz） */
    const char *name;    /* 档位名 */
    bool     hidden;     /* true = 隐藏档（不显示在 list，但 set 仍可用） */
} sysclk_tier_t;

/* 档位表（定义在 hal_port.c，RP2040 移植层） */
extern const sysclk_tier_t g_sysclk_tiers[SYSCLK_TIER_COUNT];

/* 应用某个档位（0..SYSCLK_TIER_COUNT-1）。
 *   · 切换 SYS PLL 到档位频率
 *   · 把 clk_peri 钳回 SYSCLK_PERI_MHZ，保证外设总线速度不变
 *   · 按需升压 + 缩放 XIP flash SSI 分频，保证 flash 读取安全
 *   返回 true = 成功；false = 档位非法或切换失败。
 *   ⚠ 必须在调度器启动前（冷启动阶段）调用，避免 USB/Timer 节拍被改乱。 */
bool sysclk_apply_tier(int tier);

/* 应用任意频率（MHz，SYSCLK_MHZ_MIN..SYSCLK_MHZ_MAX）。
 *   自动匹配：SYS PLL 就近锁定可达频率、XIP flash 分频钳 ≤
 *   SYSCLK_FLASH_MAX_MHZ、clk_peri 整数分频钳 ≤ SYSCLK_PERI_MAX_MHZ、
 *   核心电压按频率分档。返回 true = 成功；false = 频率非法或无法锁定。
 *   预设档（125 的整数倍）由 sysclk_apply_tier 走本函数实现，clk_peri
 *   仍精确钳回 125MHz。 */
bool sysclk_apply_mhz(uint32_t mhz);

/* 根据当前实际 clk_sys 反查所在档位（找不到返回 -1） */
int  sysclk_current_tier(void);

/* 返回当前 clk_sys 频率（MHz，向上取整） */
uint32_t sysclk_current_mhz(void);

#ifdef __cplusplus
}
#endif

#endif /* HAL_SYSCLK_H */