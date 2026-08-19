/**
 * @file    rp2350_clk.c
 * @brief   RP2350 时钟配置实现
 */

#include "rp2350_port.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/pll.h"
#include "hardware/structs/clocks.h"
#include "hardware/structs/pll.h"
#include "hardware/structs/vreg.h"

/* ================================================================
 * 预定义频率配置
 * ================================================================ */

static const struct {
    uint32_t sys_mhz;
    uint32_t vreg_voltage;
    uint32_t pll_sys_mhz;
    uint32_t pll_usb_mhz;
    uint32_t clk_peri_div;
} clk_presets[] = {
    { 48,  VREG_VOLTAGE_1_00, 48,  48,  1 },
    { 100, VREG_VOLTAGE_1_10, 100, 48,  1 },
    { 125, VREG_VOLTAGE_1_10, 125, 48,  1 },
    { 133, VREG_VOLTAGE_1_15, 133, 48,  1 },
    { 150, VREG_VOLTAGE_1_20, 150, 48,  1 },
};

bool rp2350_clk_configure(rp2350_clk_freq_t freq) {
    /* 查找预设配置 */
    const int num_presets = sizeof(clk_presets) / sizeof(clk_presets[0]);
    const struct {
        uint32_t sys_mhz;
        uint32_t vreg_voltage;
        uint32_t pll_sys_mhz;
        uint32_t pll_usb_mhz;
        uint32_t clk_peri_div;
    } *cfg = NULL;
    
    for (int i = 0; i < num_presets; i++) {
        if (clk_presets[i].sys_mhz == freq / 1000000) {
            cfg = &clk_presets[i];
            break;
        }
    }
    
    if (!cfg) return false;
    
    /* 设置电压调节器 */
    vreg_set_voltage(cfg->vreg_voltage);
    
    /* 配置 PLL_SYS */
    pll_init(pll_sys, 1, cfg->pll_sys_mhz * 1000000, 5, 2);
    
    /* 配置 PLL_USB (必须 48MHz) */
    pll_init(pll_usb, 1, cfg->pll_usb_mhz * 1000000, 5, 2);
    
    /* 配置时钟源 */
    clock_configure(clk_sys,
        CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
        CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
        cfg->pll_sys_mhz * 1000000,
        cfg->pll_sys_mhz * 1000000);
    
    /* 配置外设时钟 */
    clock_configure(clk_peri,
        0,
        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
        cfg->pll_sys_mhz * 1000000 / cfg->clk_peri_div,
        cfg->pll_sys_mhz * 1000000 / cfg->clk_peri_div);
    
    /* USB 时钟 */
    clock_configure(clk_usb,
        0,
        CLOCKS_CLK_USB_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
        cfg->pll_usb_mhz * 1000000,
        cfg->pll_usb_mhz * 1000000);
    
    /* ADC 时钟 */
    clock_configure(clk_adc,
        0,
        CLOCKS_CLK_ADC_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
        48000000,
        48000000);
    
    return true;
}

void rp2350_clk_get_config(rp2350_clk_config_t *config) {
    if (!config) return;
    
    config->sys_clk_hz = clock_get_hz(clk_sys);
    config->peri_clk_hz = clock_get_hz(clk_peri);
    config->usb_clk_hz = clock_get_hz(clk_usb);
    config->adc_clk_hz = clock_get_hz(clk_adc);
}

uint32_t rp2350_clk_sys_hz(void) {
    return clock_get_hz(clk_sys);
}

uint32_t rp2350_clk_peri_hz(void) {
    return clock_get_hz(clk_peri);
}