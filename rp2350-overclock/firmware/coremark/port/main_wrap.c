// CoreMark launcher: runs 3 measured passes per power-on, using a watchdog
// reboot between passes so each pass starts from a clean boot (same procedure
// the CoreMark run rules expect), then parks and reports idle.
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "oc.h"

int coremark_main(int argc, char *argv[]);

#ifndef MAX_MHZ
#define MAX_MHZ 150
#endif
#ifndef VOLT_MV
#define VOLT_MV 1100
#endif
#ifndef ITERATIONS
#define ITERATIONS 5000
#endif
#define RUNS 3

int main(void) {
    stdio_init_all();
    for (int i = 0; i < 120 && !stdio_usb_connected(); i++) sleep_ms(100);
    sleep_ms(1500);   // let the host reattach after a watchdog reboot

    uint32_t run = 1;
    if (watchdog_caused_reboot()) {
        uint32_t s = watchdog_hw->scratch[0];
        if (s >= 1 && s <= RUNS) run = s;
    }

    oc_apply(MAX_MHZ, VOLT_MV);

    printf("\n[cm] ===== run %lu/%d =====", (unsigned long)run, RUNS);
    printf(" core=%s clock=%luMHz vreg=%dmV iters=%d gcc=%s T=%.1fC\n",
#ifdef __riscv
           "hazard3-rv32",
#else
           "cortex-m33",
#endif
           (unsigned long)oc_mhz(), VOLT_MV, ITERATIONS, __VERSION__, oc_temp_c());

    int rc = coremark_main(0, NULL);
    printf("[cm] pass %lu return=%d T=%.1fC\n", (unsigned long)run, rc, oc_temp_c());

    if (run < RUNS) {
        watchdog_hw->scratch[0] = run + 1;
        watchdog_reboot(0, 0, 100);
        while (true) tight_loop_contents();
    }

    watchdog_hw->scratch[0] = 0;
    printf("[cm] ALL RUNS DONE (%d passes at %lu MHz)\n", RUNS, (unsigned long)oc_mhz());
    while (true) {
        printf("[cm] idle %lu MHz T=%.1fC\n", (unsigned long)oc_mhz(), oc_temp_c());
        sleep_ms(3000);
    }
}
