#!/usr/bin/env python3
"""Flatten a GNU ld script's INCLUDE tree into one self-contained script.

Why: binutils 2.47 (msys2 riscv32-unknown-elf) rejects *any* INCLUDE with
"linker script file 'x (y)' appears multiple times", even for a two-file chain.
Expansion is textual and order-preserving, exactly what INCLUDE does; only a
true include cycle is broken out of.

Usage: flatten_ld.py ROOT.ld OUT.ld DIR1 [DIR2 ...]
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


def advance_comment_state(line, in_comment):
    """Walk a line and return the comment state after it."""
    i = 0
    while True:
        if not in_comment:
            j = line.find("/*", i)
            if j < 0:
                return False
            in_comment = True
            i = j + 2
        else:
            j = line.find("*/", i)
            if j < 0:
                return True
            in_comment = False
            i = j + 2


def emit(path, dirs, out, stack, depth=0):
    if depth > 32:
        out.append(f"/* flatten: depth limit at {path} */\n")
        return
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            lines = fh.read().splitlines()
    except OSError as e:
        out.append(f"/* flatten: cannot read {path}: {e} */\n")
        return
    in_comment = False
    for line in lines:
        # INCLUDE inside a /* ... */ block must stay a comment (the SDK keeps
        # commented-out examples there); expanding it would eat the closing */.
        m = INCLUDE_RE.match(line) if not in_comment else None
        if not m:
            out.append(line + "\n")
            in_comment = advance_comment_state(line, in_comment)
            continue
        name = m.group(1)
        sub = resolve(name, [os.path.dirname(path)] + dirs)
        out.append(f"/* ---- begin {name} ---- */\n")
        if sub is None:
            out.append(f"/* flatten: {name} NOT FOUND */\n")
        elif os.path.normcase(os.path.abspath(sub)) in stack:
            out.append(f"/* flatten: cycle {name} skipped */\n")
        else:
            stack.append(os.path.normcase(os.path.abspath(sub)))
            emit(sub, dirs, out, stack, depth + 1)
            stack.pop()
        out.append(f"/* ---- end {name} ---- */\n")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    root, outpath = sys.argv[1], sys.argv[2]
    dirs = [os.path.abspath(d) for d in sys.argv[3:]]
    out = [f"/* flattened from {root} */\n"]
    stack = [os.path.normcase(os.path.abspath(root))]
    emit(root, dirs, out, stack)
    with open(outpath, "w", encoding="utf-8", newline="\n") as fh:
        fh.writelines(out)
    print(outpath)
    return 0


if __name__ == "__main__":
    sys.exit(main())
