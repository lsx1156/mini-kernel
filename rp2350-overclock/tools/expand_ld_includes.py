#!/usr/bin/env python3
"""Expand a GNU ld script's INCLUDE tree into an ordered list of file paths.

Why: binutils 2.47 (msys2 riscv32-unknown-elf) rejects any INCLUDE with
"linker script file 'x (y)' appears multiple times", even for a two-file chain.
Passing the same files as separate -T arguments is equivalent (ld concatenates
scripts in order) and sidesteps the broken INCLUDE handling.

Usage: expand_ld_includes.py ROOT.ld DIR1 [DIR2 ...]
Output: one absolute-ish path per line, depth-first, ROOT first.
"""
import os
import re
import sys

INCLUDE_RE = re.compile(r'^\s*INCLUDE\s+"([^"]+)"', re.IGNORECASE)


def resolve(name, dirs):
    for d in dirs:
        p = os.path.join(d, name)
        if os.path.isfile(p):
            return p
    return None


def expand(path, dirs, out, seen):
    out.append(path)
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            lines = fh.readlines()
    except OSError as e:
        print(f"warn: cannot read {path}: {e}", file=sys.stderr)
        return
    for line in lines:
        m = INCLUDE_RE.match(line)
        if not m:
            continue
        name = m.group(1)
        sub = resolve(name, [os.path.dirname(path)] + dirs)
        if sub is None:
            print(f"warn: INCLUDE {name} not found from {path}", file=sys.stderr)
            continue
        key = os.path.normcase(os.path.abspath(sub))
        if key in seen:
            print(f"warn: INCLUDE cycle/duplicate {sub}", file=sys.stderr)
            continue
        seen.add(key)
        expand(sub, dirs, out, seen)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    root = sys.argv[1]
    dirs = [os.path.abspath(d) for d in sys.argv[2:]]
    out, seen = [], set()
    seen.add(os.path.normcase(os.path.abspath(root)))
    expand(root, dirs, out, seen)
    for p in out:
        print(os.path.abspath(p).replace("\\", "/"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
