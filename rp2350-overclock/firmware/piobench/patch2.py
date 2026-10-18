p = 'main.c'
s = open(p, encoding='utf-8').read()

# PIO pointers are separate symbols; pio0 + n does NOT yield pio1/pio2.
helper = ('static PIO pio_of(uint b) { return b == 0 ? pio0 : (b == 1 ? pio1 : pio2); }\n\n')
s = s.replace('static bool dma_wait_ms(', helper + 'static bool dma_wait_ms(')

s = s.replace('PIO pio = pio0 + (i / 4);', 'PIO pio = pio_of(i / 4);')
s = s.replace('off[b] = pio_add_program(pio0 + b, &prog);', 'off[b] = pio_add_program(pio_of(b), &prog);')
s = s.replace('pio_remove_program(pio0 + b, &prog, off[b]);', 'pio_remove_program(pio_of(b), &prog, off[b]);')

# one captured bit == one sample: report samples/s directly, words/s = bits/32
s = s.replace('khz * 1e3 / (b / 32.0)', 'khz * 1e3 / b')
s = s.replace('khz * 1e3 * 4 / (b / 32.0)', 'khz * 1e3 * 4 / b')
s = s.replace('khz * 1e3 * 12 / (b / 32.0)', 'khz * 1e3 * 12 / b')
s = s.replace('%7.2f Msample/s', '%8.2f Msample/s')

open(p, 'w', encoding='utf-8').write(s)
print('pio0+ left:', s.count('pio0 + '), '| pio_of:', s.count('pio_of('))
