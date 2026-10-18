"""Flash a .uf2 onto an RP2350 Pico in BOOTSEL-when-needed fashion, then capture USB-CDC output.

Usage:  python tools_flash_capture.py <uf2> [capture_seconds]
Steps: (1) 1200-baud touch on every 2E8A CDC port to reboot into BOOTSEL,
       (2) wait for the RPI-RP2 volume (F: default), copy the uf2,
       (3) wait for the CDC port to come back, read its output.
"""
import os
import shutil
import sys
import threading
import time

import serial
from serial.tools import list_ports

VOLUME = os.environ.get("RP_VOLUME", "F:\\")


def cdc_ports():
    out = []
    for p in list_ports.comports():
        vid = p.vid or 0
        if vid == 0x2E8A:
            out.append(p)
    return out


def open_guarded(dev, baud, guard=5.0):
    """Open a CDC port with a hard timeout: a wedged device makes pyserial block forever."""
    box = {}

    def _run():
        try:
            box["s"] = serial.Serial(dev, baud, timeout=0.2)
        except Exception as exc:  # noqa: BLE001
            box["e"] = exc

    t = threading.Thread(target=_run, daemon=True)
    t.start()
    t.join(guard)
    if t.is_alive():
        print(f"open {dev}: WEDGED (>{guard:.0f}s), skipping")
        return None
    if "e" in box:
        print(f"open {dev}: {box['e']}")
        return None
    return box["s"]


def touch_reset(dev):
    try:
        s = open_guarded(dev, 1200)
        if s is None:
            return False
        s.dtr = False
        s.rts = False
        time.sleep(0.1)
        s.close()
        return True
    except Exception as exc:  # noqa: BLE001
        # pyserial raises OSError(22) when the device re-enumerates under the open;
        # the 1200-bps line coding has already been applied, so the reboot is in flight.
        print(f"touch {dev}: {exc} (expected if the reboot already started)")
        return False


def wait_for(pred, timeout, label):
    t0 = time.time()
    while time.time() - t0 < timeout:
        r = pred()
        if r:
            return r
        time.sleep(0.25)
    raise SystemExit(f"timeout waiting for {label}")


def main():
    uf2 = sys.argv[1]
    cap = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0

    ports = cdc_ports()
    if ports:
        print("cdc before :", [(p.device, f"{p.vid:04X}:{p.pid:04X}") for p in ports])
        for p in ports:
            if p.pid in (0x0009, 0x000A, 0x000F):
                print("touch reset:", p.device)
                touch_reset(p.device)
    else:
        print("no RP2350 CDC present; assuming BOOTSEL already")

    wait_for(lambda: os.path.exists(os.path.join(VOLUME, "INFO_UF2.TXT")), 20, "BOOTSEL volume")
    print("bootsel    :", open(os.path.join(VOLUME, "INFO_UF2.TXT")).read().replace("\r", "").strip().replace("\n", " | "))

    dst = os.path.join(VOLUME, os.path.basename(uf2))
    shutil.copyfile(uf2, dst)
    print(f"flashed    : {uf2} -> {dst} ({os.path.getsize(uf2)} B)")

    # board reboots; the volume disappears and a CDC port appears
    time.sleep(2.5)
    port = wait_for(lambda: (cdc_ports() or [None])[0], 20, "CDC port after flash")
    print("cdc after  :", port.device if port else "?")

    s = open_guarded(port.device, 115200)
    if s is None:
        raise SystemExit("CDC port wedged; replug the board")
    s.reset_input_buffer()
    end = time.time() + cap
    buf = b""
    while time.time() < end:
        buf += s.read(4096)
    s.close()
    sys.stdout.write(buf.decode("utf-8", "replace"))


if __name__ == "__main__":
    main()
