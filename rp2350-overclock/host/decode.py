#!/usr/bin/env python3
"""12 通道嗅探器主机侧解码器：I2C / SPI / WS2812

采样数据来自固件：每个 32 位字装 32 个连续采样点，**最早采样在 bit0**
（PIO in_shift 为右移）。

用法：
    python decode.py selftest          # 合成波形自检，不需要板子
"""
import sys


class Capture:
    """一个通道的采样位流。words 为 32 位整数列表，最早采样在 bit0。"""

    def __init__(self, words, n_samples, rate_hz):
        self.words = words
        self.n = n_samples
        self.rate = float(rate_hz)

    @classmethod
    def from_bits(cls, bits, rate_hz):
        words = [0] * ((len(bits) + 31) // 32)
        for i, b in enumerate(bits):
            if b:
                words[i >> 5] |= 1 << (i & 31)
        return cls(words, len(bits), rate_hz)

    def bit(self, i):
        if i < 0 or i >= self.n:
            return 1  # 越界按空闲高电平
        w = i >> 5
        if w >= len(self.words):
            return 0
        return (self.words[w] >> (i & 31)) & 1

    def ns(self, samples):
        return samples * 1e9 / self.rate

    def edges(self):
        out = []
        for i in range(1, self.n):
            a, b = self.bit(i - 1), self.bit(i)
            if a != b:
                out.append((i, b))
        return out


# ---------------------------------------------------------------- I2C

def decode_i2c(scl, sda):
    """返回交易列表：[{addr, rw, acked, data:[{val, ack}], repeated_start}]"""
    trans, cur, bits, byte_idx = [], None, [], 0

    def flush():
        nonlocal cur
        if cur is not None and (cur['addr'] is not None or cur['data']):
            trans.append(cur)
        cur = None

    for i in range(1, min(scl.n, sda.n)):
        ps, s = scl.bit(i - 1), scl.bit(i)
        pd, d = sda.bit(i - 1), sda.bit(i)

        if s and pd == 1 and d == 0:            # START / repeated START
            if cur is not None:
                cur['repeated_start'] = True
                flush()
            cur = {'addr': None, 'rw': None, 'acked': None, 'data': [], 'repeated_start': False}
            bits, byte_idx = [], 0
        elif s and pd == 0 and d == 1:          # STOP
            flush()
        elif (not ps) and s:                    # SCL 上升沿：采样 SDA
            if cur is not None:
                bits.append(d)
                if len(bits) == 9:
                    val = 0
                    for b in bits[:8]:
                        val = (val << 1) | b
                    ack = bits[8] == 0
                    if byte_idx == 0:
                        cur['addr'] = val >> 1
                        cur['rw'] = val & 1
                        cur['acked'] = ack
                    else:
                        cur['data'].append({'val': val, 'ack': ack})
                    bits, byte_idx = [], byte_idx + 1
    flush()
    return trans


# ---------------------------------------------------------------- SPI

def decode_spi(sck, mosi, cs=None, cpha=0, nbits=8, msb=True):
    """CPOL=0 假定（空闲低）。cpha=0 在上升沿采样，cpha=1 在下降沿。"""
    out, shift, cnt = [], 0, 0
    for i in range(1, sck.n):
        if cs is not None and cs.bit(i) == 1:   # CS 高 = 空闲，丢弃半字节
            shift, cnt = 0, 0
            continue
        ps, s = sck.bit(i - 1), sck.bit(i)
        edge = 'rise' if (not ps and s) else ('fall' if (ps and not s) else None)
        if edge is None:
            continue
        if (cpha == 0 and edge == 'rise') or (cpha == 1 and edge == 'fall'):
            b = mosi.bit(i)
            shift = ((shift << 1) | b) if msb else (shift | (b << cnt))
            cnt += 1
            if cnt == nbits:
                out.append(shift & ((1 << nbits) - 1))
                shift, cnt = 0, 0
    return out


# ---------------------------------------------------------------- WS2812

def decode_ws2812(cap, t_thresh_ns=600, reset_ns=50000):
    """返回 [(g, r, b), ...]。阈值默认 600 ns，适配 800 kHz 的 T0H/T1H。"""
    pulses = []
    i = 0
    while i < cap.n:
        while i < cap.n and cap.bit(i) == 0:
            i += 1
        if i >= cap.n:
            break
        j = i
        while j < cap.n and cap.bit(j) == 1:
            j += 1
        high = cap.ns(j - i)
        k = j
        while k < cap.n and cap.bit(k) == 0:
            k += 1
        low = cap.ns(k - j) if k < cap.n else 0
        pulses.append((high, low))
        i = k

    leds, bits = [], []

    def flush():
        nonlocal bits
        while len(bits) >= 24:
            leds.append((_bits_to_int(bits[0:8]),
                         _bits_to_int(bits[8:16]),
                         _bits_to_int(bits[16:24])))
            bits = bits[24:]

    for high, low in pulses:
        if high > 2000:                          # 不是比特：抓取窗口开头/结尾的长高电平
            flush()
            bits = []
            continue
        bits.append(1 if high > t_thresh_ns else 0)
        if low > reset_ns:                       # 复位间隔：一帧结束
            flush()
            bits = []
    flush()
    return leds


def _bits_to_int(bs):
    v = 0
    for b in bs:
        v = (v << 1) | b
    return v


# ---------------------------------------------------------------- 合成波形（自检用）

def _synth_i2c(rate, addr, data, repeated=False):
    """产生 SCL/SDA 两个位流：START + addr+W + data + STOP"""
    import numpy as np
    bit_ns = 2500                                   # 400 kHz
    idle_ns = 2000
    seq = []                                        # (scl, sda) 每段
    def hold(ns_n, scl, sda):
        seq.append((ns_n, scl, sda))
    hold(idle_ns, 1, 1)
    # START: SCL 高时 SDA 下跳
    hold(bit_ns // 4, 1, 1)
    hold(bit_ns // 4, 1, 0)
    def send_byte(v, ack_bit):
        for k in range(7, -1, -1):
            b = (v >> k) & 1
            hold(bit_ns // 2, 0, b)
            hold(bit_ns // 2, 1, b)
        hold(bit_ns // 2, 0, ack_bit)
        hold(bit_ns // 2, 1, ack_bit)
    send_byte((addr << 1) | 0, 0)
    for d in data:
        send_byte(d, 0)
    hold(bit_ns // 2, 0, 0)
    hold(bit_ns // 4, 1, 0)
    hold(bit_ns // 4, 1, 1)                         # STOP: SCL 高时 SDA 上跳
    hold(idle_ns, 1, 1)

    scl_bits, sda_bits = [], []
    for ns_n, scl, sda in seq:
        n = max(1, int(round(ns_n * rate / 1e9)))
        scl_bits += [scl] * n
        sda_bits += [sda] * n
    return Capture.from_bits(scl_bits, rate), Capture.from_bits(sda_bits, rate)


def _synth_spi(rate, values, sck_hz=1_000_000):
    """CPOL=0 CPHA=0，MSB 先出，无 CS。"""
    period = rate / sck_hz
    sck_bits, mosi_bits, cs_bits = [0], [0], [1]
    cs_bits = [0]
    for v in values:
        for k in range(7, -1, -1):
            b = (v >> k) & 1
            n = max(1, int(round(period / 2)))
            sck_bits += [0] * n
            mosi_bits += [b] * n
            sck_bits += [1] * n
            mosi_bits += [b] * n
    sck_bits += [0] * 16
    mosi_bits += [0] * 16
    cs_bits = [0] * len(sck_bits)
    for i in range(len(cs_bits) - 16, len(cs_bits)):
        cs_bits[i] = 1
    return (Capture.from_bits(sck_bits, rate),
            Capture.from_bits(mosi_bits, rate),
            Capture.from_bits(cs_bits, rate))


def _synth_ws2812(rate, leds, bit_ns=1250, reset_ns=80000):
    bits = []
    for (g, r, b) in leds:
        for byte in (g, r, b):
            for k in range(7, -1, -1):
                bit = (byte >> k) & 1
                high = int(round((800 if bit else 400) * rate / 1e9))
                low = int(round(bit_ns * rate / 1e9)) - high
                bits += [1] * max(1, high) + [0] * max(1, low)
    bits += [0] * int(round(reset_ns * rate / 1e9))
    return Capture.from_bits(bits, rate)


# ---------------------------------------------------------------- 自检

def selftest():
    ok = True

    # I2C：写 0x50，数据 0x12 0x34
    scl, sda = _synth_i2c(10_000_000, 0x50, [0x12, 0x34])
    t = decode_i2c(scl, sda)
    got = [(x['addr'], x['rw'], [d['val'] for d in x['data']]) for x in t]
    want = [(0x50, 0, [0x12, 0x34])]
    print(f"[i2c ] {got}  {'PASS' if got == want else 'FAIL'}")
    ok &= got == want

    # SPI：0xA5 0x3C，1 MHz
    sck, mosi, cs = _synth_spi(10_000_000, [0xA5, 0x3C])
    got = decode_spi(sck, mosi, cs)
    want = [0xA5, 0x3C]
    print(f"[spi ] {[hex(x) for x in got]}  {'PASS' if got == want else 'FAIL'}")
    ok &= got == want

    # WS2812：两颗灯 (G,R,B) = (255,0,0) (0,255,0)
    cap = _synth_ws2812(10_000_000, [(255, 0, 0), (0, 255, 0)])
    got = decode_ws2812(cap)
    want = [(255, 0, 0), (0, 255, 0)]
    print(f"[ws28] {got}  {'PASS' if got == want else 'FAIL'}")
    ok &= got == want

    print("ALL PASS" if ok else "SOME FAILED")
    return 0 if ok else 1


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == 'selftest':
        sys.exit(selftest())
    print(__doc__)
