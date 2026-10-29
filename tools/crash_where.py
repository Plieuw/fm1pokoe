#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""A crash report's addresses (the red CRASH screen: PC, RETS) to the function and the code around them.
  tools/crash_where.py ADDR [ADDR ...]   (hex; reads build/felucca.sym and build/felucca.dis)
  --gh: as GitHub annotations (one per address), for a CI run started with crash_addrs"""
import re, sys
from pathlib import Path

B = Path(__file__).resolve().parent.parent / "build"


def main(args):
    gh = "--gh" in args
    addrs = [int(a, 16) for a in args if a != "--gh"]
    syms = (B / "felucca.sym").read_text()
    funcs = sorted({(int(m.group(1), 16), m.group(2)) for m in
                    re.finditer(r"^([0-9a-f]{8}) .{7} (?:\.text|\.ram_text|\.c1_text)\s+[0-9a-f]+ (\S+)$", syms, re.M)})
    dis = (B / "felucca.dis").read_text().splitlines()
    insn = [(int(m.group(1), 16), i) for i, ln in enumerate(dis) if (m := re.match(r"^\s*([0-9a-f]+):", ln))]
    for a in addrs:
        f = max((x for x in funcs if x[0] <= a), default=(0, "?"))
        out = [f"{a:#010x} = {f[1]} + {a - f[0]:#x}"]
        near = [i for x, i in insn if abs(x - a) <= 0x40]
        if near:
            out += dis[max(near[0] - 4, 0):near[-1] + 2]
        if gh:
            print(f"::notice title=crash {a:#010x}::" + "%0A".join(ln.replace("%", "%25") for ln in out))
        else:
            print("\n".join(out) + "\n")


if __name__ == "__main__":
    main(sys.argv[1:])
