#!/usr/bin/env python3
"""Turn a binary into a C array (payload embedded in the Arm image)."""
import sys

src, dst, name = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(src, "rb").read()
with open(dst, "w", encoding="utf-8") as fh:
    fh.write("#include <stdint.h>\n\n")
    fh.write(f"const uint32_t {name}_len = {len(data)}u;\n")
    fh.write(f"const uint8_t {name}[] __attribute__((aligned(4))) = {{\n")
    for i in range(0, len(data), 16):
        fh.write("    " + ",".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",\n")
    fh.write("};\n")
print(f"{dst}: {len(data)} bytes")
