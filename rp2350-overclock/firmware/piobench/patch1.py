import re

p = 'main.c'
s = open(p, encoding='utf-8').read()

# 1) drop SDK SM claiming (it panics when a bit is already set and adds no value here)
s = s.replace('        pio_claim_sm_mask(pio, 1u << sm);\n', '')
s = s.replace('        pio_sm_unclaim(pio, sm);\n', '')

# 2) replace blocking DMA waits with deadline-polled waits so a misconfiguration
#    reports TIMEOUT instead of freezing the whole run
s = s.replace(
    '        for (uint i = 0; i < nsm; i++) dma_channel_wait_for_finish_blocking((uint)ch[i]);',
    '        for (uint i = 0; i < nsm; i++) dma_wait_ms((uint)ch[i], 500);')

helper = '''
static bool dma_wait_ms(uint ch, uint ms) {
    absolute_time_t deadline = make_timeout_time_ms(ms);
    while (dma_channel_is_busy(ch)) {
        if (time_reached(deadline)) return false;
        tight_loop_contents();
    }
    return true;
}
'''
s = s.replace('static void pins_init(', helper + '\nstatic void pins_init(')

open(p, 'w', encoding='utf-8').write(s)
print('patched:', s.count('dma_wait_ms('), 'waits;', s.count('pio_claim_sm_mask'), 'claims left')
