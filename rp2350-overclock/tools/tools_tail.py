"""Stream a Pico CDC port to stdout, tolerating disconnect/re-enumerate.

Usage: python tools_tail.py [seconds] [port]
Prints each chunk as it arrives (line-flushed), so a crash mid-run still leaves
the evidence on screen; reconnects automatically when the device re-enumerates.
"""
import sys
import time

import serial
from serial.tools import list_ports


def find_port():
    for p in list_ports.comports():
        if p.vid == 0x2E8A and p.pid in (0x0009, 0x000A):
            return p.device
    return None


def main():
    dur = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
    port = sys.argv[2] if len(sys.argv) > 2 else None
    end = time.time() + dur
    s = None
    while time.time() < end:
        if s is None:
            port = port or find_port()
            if not port:
                time.sleep(0.2)
                continue
            try:
                s = serial.Serial(port, 115200, timeout=0.2)
                s.reset_input_buffer()
                print(f"--- attached {port} ---", flush=True)
            except Exception as exc:          # noqa: BLE001
                print(f"--- open failed: {exc}", flush=True)
                time.sleep(0.3)
                continue
        try:
            data = s.read(4096)
            if data:
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
        except Exception as exc:              # noqa: BLE001
            print(f"\n--- port dropped: {exc} (device likely reset) ---", flush=True)
            try:
                s.close()
            except Exception:
                pass
            s = None
            port = None
            time.sleep(0.3)
    if s:
        s.close()


if __name__ == "__main__":
    main()
