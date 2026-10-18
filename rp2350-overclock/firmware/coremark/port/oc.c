#include "oc.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/adc.h"
#include "hardware/structs/qmi.h"
#include "hardware/regs/qmi.h"
#include "hardware/sync.h"
#include "hardware/structs/powman.h"
#include "hardware/regs/powman.h"
#include "hardware/platform_defs.h"

static void __no_inline_not_in_flash_func(set_flash_div)(uint32_t div) {
    uint32_t save = save_and_disable_interrupts();
    uint32_t t = qmi_hw->m[0].timing;
    t &= ~QMI_M0_TIMING_CLKDIV_BITS;
    t |= (div << QMI_M0_TIMING_CLKDIV_LSB) & QMI_M0_TIMING_CLKDIV_BITS;
    qmi_hw->m[0].timing = t;
    (void)qmi_hw->m[0].timing;
    restore_interrupts(save);
}

static uint32_t even_div_for(uint32_t khz) {
    uint32_t d = 2;
    while (khz / d > 80000) d += 2;     // keep flash SCK <= 80 MHz
    return d;
}

// Above 1.30 V the regulator clamps unless POWMAN_VREG_CTRL.DISABLE_VOLTAGE_LIMIT
// is set; both that bit and the UNLOCK bit require the 0x5afe password. The
// UNLOCK bit cannot be re-locked until the next reset, so a power cycle always
// returns the part to 1.10 V.
static void __no_inline_not_in_flash_func(vreg_unlock_limit)(void) {
    hw_set_bits(&powman_hw->vreg_ctrl, POWMAN_PASSWORD_BITS | POWMAN_VREG_CTRL_UNLOCK_BITS);
    hw_set_bits(&powman_hw->vreg_ctrl, POWMAN_PASSWORD_BITS | POWMAN_VREG_CTRL_DISABLE_VOLTAGE_LIMIT_BITS);
    while (powman_hw->vreg & POWMAN_VREG_UPDATE_IN_PROGRESS_BITS) tight_loop_contents();
}

void oc_apply(uint32_t mhz, uint32_t mv) {
    // RP2350 has no 1.26/1.28/1.45/1.55/1.75 V codes: above 1.30 the steps are
    // 1.35, 1.40, 1.50, 1.60, 1.65, 1.70, 1.80, ...
    enum vreg_voltage v;
    if      (mv >= 2350) v = VREG_VOLTAGE_2_35;
    else if (mv >= 2000) v = VREG_VOLTAGE_2_00;
    else if (mv >= 1900) v = VREG_VOLTAGE_1_90;
    else if (mv >= 1800) v = VREG_VOLTAGE_1_80;
    else if (mv >= 1700) v = VREG_VOLTAGE_1_70;
    else if (mv >= 1650) v = VREG_VOLTAGE_1_65;
    else if (mv >= 1600) v = VREG_VOLTAGE_1_60;
    else if (mv >= 1500) v = VREG_VOLTAGE_1_50;
    else if (mv >= 1400) v = VREG_VOLTAGE_1_40;
    else if (mv >= 1350) v = VREG_VOLTAGE_1_35;
    else if (mv >= 1300) v = VREG_VOLTAGE_1_30;
    else if (mv >= 1250) v = VREG_VOLTAGE_1_25;
    else if (mv >= 1200) v = VREG_VOLTAGE_1_20;
    else if (mv >= 1150) v = VREG_VOLTAGE_1_15;
    else                 v = VREG_VOLTAGE_1_10;

    if (mv > 1300) vreg_unlock_limit();
    vreg_set_voltage(v);
    sleep_ms(10);
    set_flash_div(even_div_for(mhz * 1000u));
    set_sys_clock_khz(mhz * 1000u, false);
    sleep_ms(5);
}

float oc_temp_c(void) {
    static bool inited;
    if (!inited) {
        adc_init();
        adc_set_temp_sensor_enabled(true);
        inited = true;
    }
    adc_select_input(4);
    uint16_t raw = adc_read();
    float v = (float)raw * 3.3f / 4096.0f;
    return 27.0f - (v - 0.706f) / 0.001721f;
}

uint32_t oc_mhz(void) {
    return clock_get_hz(clk_sys) / 1000000u;
}
