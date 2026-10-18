p = 'main.c'
s = open(p, encoding='utf-8').read()

s = s.replace('#include "hardware/gpio.h"', '#include "hardware/gpio.h"\n#include "oc.h"')

s = s.replace(
    'int main(void) {\n    stdio_init_all();',
    'int main(void) {\n'
    '#if defined(MAX_MHZ) && (MAX_MHZ != 150)\n'
    '    oc_apply(MAX_MHZ, VOLT_MV);\n'
    '#endif\n'
    '    stdio_init_all();')

s = s.replace('    sleep_ms(2000);',
              '    sleep_ms(2000);\n    printf("[pb] temp=%.1f C\\n", oc_temp_c());')

open(p, 'w', encoding='utf-8').write(s)
print('ok', 'oc_apply' in s, 'oc_temp_c' in s)
