import sys, time, serial
port = sys.argv[1]; dur = float(sys.argv[2]) if len(sys.argv)>2 else 8
s = serial.Serial(port, 115200, timeout=0.3)
s.reset_input_buffer()
end = time.time()+dur
buf = b""
while time.time() < end:
    buf += s.read(4096)
s.close()
sys.stdout.write(buf.decode("utf-8", "replace"))
