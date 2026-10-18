#!/usr/bin/env python3
"""12 通道嗅探器主机工具：抓取 + 解码。

用法：
    python sniff.py COM3 --test ws2812
    python sniff.py COM3 --test spi
    python sniff.py COM3 --dump 11 --us 5000
"""
import argparse
import struct
import sys
import time

import serial

from decode import Capture, decode_spi, decode_ws2812


def read_line(s, timeout=3.0):
    buf = b''
    end = time.monotonic() + timeout
    while time.monotonic() < end and not buf.endswith(b'\n'):
        c = s.read(1)
        if c:
            buf += c
    return buf.decode(errors='replace').strip()


def read_exact(s, n, timeout=5.0):
    buf = b''
    end = time.monotonic() + timeout
    while len(buf) < n and time.monotonic() < end:
        chunk = s.read(n - len(buf))
        if chunk:
            buf += chunk
    return buf


def cmd(s, line):
    s.write((line + '\n').encode())
    return read_line(s)


def capture(s, us, mode=0):
    r = cmd(s, f'C {us} {mode}')
    if not r.startswith('OK'):
        raise RuntimeError(f'capture failed: {r!r}')
    return [int(x) for x in r.split()[1:]]


def dump(s, ch):
    s.write(f'D {ch}\n'.encode())
    hdr = read_exact(s, 4)
    if len(hdr) != 4:
        raise RuntimeError('short header')
    n = struct.unpack('<I', hdr)[0]
    data = read_exact(s, n * 4, timeout=5.0)
    words = list(struct.unpack('<%dI' % n, data))
    return words


def cap_of(s, ch, n_samples, rate):
    return Capture(dump(s, ch), n_samples, rate)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('port')
    ap.add_argument('--test', choices=['ws2812', 'spi', 'i2c'])
    ap.add_argument('--trigger', metavar='CH:EDGE:POST_US',
                    help='环形预录后等边沿：如 11:1:1000（GP11 上升沿，之后录 1000 µs）')
    ap.add_argument('--stim', type=int, default=0,
                    help='触发模式下的自测激励：4=core1 循环发 WS2812')
    ap.add_argument('--dump', type=int)
    ap.add_argument('--us', type=int, default=5000)
    ap.add_argument('--rate', type=int, default=10_000_000)
    a = ap.parse_args()

    s = serial.Serial(a.port, 115200, timeout=0.05)
    s.reset_input_buffer()
    s.write(b'V\n')
    print('fw:', read_line(s, 2.0))

    if a.test == 'ws2812':
        print(cmd(s, f'R 11 {a.rate}'))
        cnt = capture(s, a.us, 1)
        print('samples:', cnt[9:12])
        cap = cap_of(s, 11, cnt[11], a.rate)
        leds = decode_ws2812(cap)
        want = [(0x34, 0x12, 0x56), (0xCD, 0xAB, 0xEF)]
        print('decoded:', [tuple(hex(v) for v in l) for l in leds])
        print('PASS' if leds == want else f'FAIL (want {want})')

    elif a.test == 'spi':
        for ch in (9, 10, 11):
            cmd(s, f'R {ch} {a.rate}')
        cnt = capture(s, a.us, 2)
        print('samples:', cnt[9:12])
        cs = cap_of(s, 9, cnt[9], a.rate)
        sck = cap_of(s, 10, cnt[10], a.rate)
        mosi = cap_of(s, 11, cnt[11], a.rate)
        data = decode_spi(sck, mosi, cs)
        print('decoded:', [hex(x) for x in data])
        print('PASS' if data == [0xA5, 0x3C] else 'FAIL')

    elif a.trigger:
        ch, edge, post = (int(x) for x in a.trigger.split(':'))
        for c in range(12):
            cmd(s, f'R {c} {a.rate}')
        info, raw = trigger(s, ch, edge, post, a.stim)
        print('trigger:', raw)
        if not info or not info['fired']:
            print('FAIL: 没等到边沿')
            s.close(); return
        print(f"fired after {info['waited_us']} µs, 预录容量 {4096*32/a.rate*1000:.1f} ms")
        words = dump_rotated(s, ch, info['offsets'][ch])
        cap = Capture(words, len(words) * 32, a.rate)
        print(f'ch{ch}: {cap.n} samples, {len(cap.edges())} edges')
        if ch == 11:
            leds = decode_ws2812(cap)
            print('decoded:', [tuple(hex(v) for v in l) for l in leds[:4]])

    elif a.dump is not None:
        cnt = capture(s, a.us, 0)
        cap = cap_of(s, a.dump, cnt[a.dump], a.rate)
        edges = cap.edges()
        print(f'ch{a.dump}: {cap.n} samples, {len(edges)} edges')
        for i, lvl in edges[:20]:
            print(f'  {cap.ns(i):.0f} ns -> {lvl}')

    s.close()


def trigger(s, ch, edge, post_us, stim=0):
    line = f'T {post_us} {ch} {edge} {stim}'
    r = cmd(s, line)
    if not r.startswith('OK'):
        return None, r
    p = r.split()
    fired, waited = int(p[1]), int(p[2])
    offs = [int(x) for x in p[3:15]]
    return {'fired': fired, 'waited_us': waited, 'offsets': offs}, r


def dump_rotated(s, ch, off_words):
    words = dump(s, ch)
    if off_words:
        k = off_words % len(words)
        words = words[k:] + words[:k]
    return words


if __name__ == '__main__':
    sys.exit(main())
