// Clock helper shared by the CoreMark / multicore builds.
#ifndef OC_H
#define OC_H

#include <stdint.h>

// Set DVDD then clk_sys. Raises the QSPI divider first so flash SCK never
// exceeds ~80 MHz as clk_sys rises (the SDK leaves QMI alone).
void oc_apply(uint32_t mhz, uint32_t mv);

// Die temperature from the on-chip sensor (ADC4).
float oc_temp_c(void);

// Effective clk_sys in MHz after the last oc_apply().
uint32_t oc_mhz(void);

#endif
