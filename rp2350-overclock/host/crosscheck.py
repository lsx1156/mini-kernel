#!/usr/bin/env python3
"""nanoDLA（sigrok fx2lafw）与 RP2350 嗅探器交叉验证。

同一根线同时接两边的输入，板子用 PIO 自己产生激励：
    nanoDLA GND <-> 板 GND
    nanoDLA CH0 <-> GP11   (WS2812 数据 / SPI MOSI)
    nanoDLA CH1 <-> GP10   (SPI SCK)
    nanoDLA CH2 <-> GP9    (SPI CS)

用法：
    python crosscheck.py COM3 --test spi
    python crosscheck.py COM3 --test ws2812
"""
import argparse
import os
import struct
import subprocess
import sys
import time

import serial

from decode import Capture, decode_i2c, decode_spi, decode_ws2812

SG_DEFAULT = r"C:/Users/master/MOSS/lib/sigrok-cli/sigrok-cli.exe"


def sg_env(sg):
    """sigrok-cli 需要 Windows 原生 TMP 路径，以及解码器脚本目录。"""
    env = dict(os.environ)
    env['TMP'] = env['TEMP'] = env['TMPDIR'] = r'C:/Users/master/MOSS/tmp'
    base = os.path.dirname(os.path.abspath(sg))
    for cand in (os.path.join(base, 'share', 'libsigrokdecode', 'decoders'),
                 os.path.join(base, '..', 'share', 'libsigrokdecode', 'decoders')):
        if os.path.isdir(cand):
            env['SIGROKDECODE_DIR'] = os.path.normpath(cand)
            break
    return env


# ---------------- 板子侧 ----------------

def read_line(s, timeout=3.0):
    buf, end = b'', time.monotonic() + timeout
    while time.monotonic() < end and not buf.endswith(b'\n'):
        c = s.read(1)
        if c:
            buf += c
    return buf.decode(errors='replace').strip()


def read_exact(s, n, timeout=6.0):
    buf, end = b'', time.monotonic() + timeout
    while len(buf) < n and time.monotonic() < end:
        buf += s.read(n - len(buf))
    return buf


def open_board(port, chans, rate):
    s = serial.Serial(port, 115200, timeout=0.05)
    s.reset_input_buffer()
    for ch in chans:
        s.write(f'R {ch} {rate}\n'.encode())
        read_line(s)
    return s


def board_frame(s, mode, us=400, rate=10_000_000):
    """抓一帧并返回（解码结果, 各通道采样数）。"""
    s.write(f'C {us} {mode}\n'.encode())
    r = read_line(s)
    if not r.startswith('OK'):
        return None, None
    counts = [int(x) for x in r.split()[1:]]

    def dump(ch):
        s.write(f'D {ch}\n'.encode())
        hdr = read_exact(s, 4)
        if len(hdr) != 4:
            return None
        n = struct.unpack('<I', hdr)[0]
        data = read_exact(s, n * 4)
        return Capture(list(struct.unpack('<%dI' % n, data)), counts[ch], rate)

    if mode == 1:
        cap = dump(11)
        return (decode_ws2812(cap) if cap else None), counts
    if mode == 3:
        scl, sda = dump(10), dump(11)
        return (decode_i2c(scl, sda) if (scl and sda) else None), counts
    c9, c10, c11 = dump(9), dump(10), dump(11)
    if not (c9 and c10 and c11):
        return None, counts
    return decode_spi(c10, c11, c9), counts


# ---------------- nanoDLA 侧 ----------------

def parse_vcd(path):
    """返回 {通道名: [(t_ns, '0'/'1'), ...]}。

    注意：sigrok 写的 VCD 把变化值和时间戳放在同一行（`#12345 1" 0#`），
    按行首字符过滤会漏掉全部跳变。
    """
    ids, edges = {}, {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('$var'):
                p = line.split()
                ids[p[-3]] = p[-2]
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('#'):
                p = line.split()
                t = int(p[0][1:])
                for tok in p[1:]:
                    if len(tok) >= 2 and tok[0] in '01':
                        ch = ids.get(tok[1:])
                        if ch:
                            edges.setdefault(ch, []).append((t, tok[0]))
    return edges


def ws2812_timing(edges, ch='D0'):
    """由高电平脉宽统计 T0H / T1H。"""
    pulses, rise = [], None
    for t, v in edges.get(ch, []):
        if v == '1':
            rise = t
        elif rise is not None:
            pulses.append(t - rise)
            rise = None
    if not pulses:
        return None
    t0 = [p for p in pulses if p < 600]
    t1 = [p for p in pulses if p >= 600]
    return {'脉冲数': len(pulses),
            'T0H_ns': round(sum(t0) / len(t0), 1) if t0 else None,
            'T1H_ns': round(sum(t1) / len(t1), 1) if t1 else None,
            'T0H数': len(t0), 'T1H数': len(t1)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('port')
    ap.add_argument('--sigrok', default=SG_DEFAULT)
    ap.add_argument('--test', choices=['ws2812', 'spi', 'i2c'], default='spi')
    ap.add_argument('--out', default=r'C:/Users/master/MOSS/tmp/x.sr')
    ap.add_argument('--seconds', type=float, default=1.0)
    ap.add_argument('--chans', default='D0,D1,D2,D3,D4,D5,D6,D7')
    ap.add_argument('--rate', type=int, default=8)
    a = ap.parse_args()

    env = sg_env(a.sigrok)
    mode = {'ws2812': 1, 'spi': 2, 'i2c': 3}[a.test]
    chans = {1: [11], 2: [9, 10, 11], 3: [10, 11]}[mode]

    print(f'[1/3] nanoDLA 采集 {a.seconds}s @ {a.rate} MHz …')
    p = subprocess.Popen([a.sigrok, '-d', 'fx2lafw',
                          '--config', f'samplerate={a.rate}m',
                          '--samples', str(int(a.rate * 1e6 * a.seconds)),
                          '--channels', 'D0,D1,D2,D3,D4,D5,D6,D7',
                          '-o', a.out],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, env=env)

    print('[2/3] 板子持续产生激励 …')
    s = open_board(a.port, chans, 10_000_000)
    t0, mine, frames = time.monotonic(), None, 0
    while time.monotonic() - t0 < a.seconds + 2.0:
        out, _ = board_frame(s, mode)
        frames += 1
        if out and mine is None:
            mine = out
    s.close()
    p.wait(timeout=60)
    print(f'   触发 {frames} 帧，板子解码: {mine}')

    print('[3/3] nanoDLA 侧分析 …')
    vcd = a.out.replace('.sr', '.vcd')
    subprocess.run([a.sigrok, '-i', a.out, '-O', 'vcd', '-o', vcd],
                   capture_output=True, text=True, env=env)
    edges = parse_vcd(vcd)

    if mode == 1:
        print('   D0 脉宽统计:', ws2812_timing(edges, 'D0'))
        print('   D0 跳变数:', len(edges.get('D0', [])))
    elif mode == 3:
        r = subprocess.run([a.sigrok, '-i', a.out, '-P', 'i2c:scl=D1:sda=D0'],
                           capture_output=True, text=True, env=env)
        print('   sigrok i2c 输出:')
        for ln in r.stdout.splitlines()[:40]:
            print('     ', ln)
        print('   D1(SCL) 跳变:', len(edges.get('D1', [])), ' D0(SDA) 跳变:', len(edges.get('D0', [])))
    else:
        r = subprocess.run([a.sigrok, '-i', a.out, '-P', 'spi:clk=D1:mosi=D0:cs=D2'],
                           capture_output=True, text=True, env=env)
        toks = [ln.split()[-1] for ln in r.stdout.splitlines() if ln.strip()]
        hexes = [t for t in toks if len(t) == 2 and all(c in '0123456789ABCDEF' for c in t)]
        print('   sigrok 解出的字节:', hexes[:16], f'（共 {len(hexes)} 个标注）')
        print('   D1(SCK) 跳变:', len(edges.get('D1', [])), ' D2(CS) 跳变:', len(edges.get('D2', [])))


if __name__ == '__main__':
    sys.exit(main())
