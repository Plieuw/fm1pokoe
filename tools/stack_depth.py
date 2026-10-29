#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The deepest stack each entry point can reach, from the disassembly (build/felucca.dis): per function its push and
its sp adjustment, the deepest of its direct calls on top (recursion and calls through a register: not followed,
counted). tools/stack_depth.py [--gh] [FUNC ...]   (default: main, the interrupt handlers)"""
import re, sys
from pathlib import Path

B = Path(__file__).resolve().parent.parent / "build"
HEAD = re.compile(r"^(?:[0-9a-f]+ )?<?([A-Za-z_.$][\w.$]*)>?:\s*$")
PUSH = re.compile(r"\[--sp\]\s*=\s*\{([^}]*)\}")
ADJ = re.compile(r"\bsp\s*\+=\s*(-?\d+)")
CALL = re.compile(r"\bcall\s+-?\d+\s+<([\w.$]+)")


def nregs(lst):
    n = 0
    for part in lst.split(","):
        part = part.strip()
        m = re.match(r"r(\d+)-r(\d+)$", part)
        n += abs(int(m.group(1)) - int(m.group(2))) + 1 if m else 1
    return n


def main(args):
    gh = "--gh" in args
    want = [a for a in args if a != "--gh"]
    fn, cur = {}, None
    for ln in (B / "felucca.dis").read_text().splitlines():
        h = HEAD.match(ln)
        if h:
            cur = h.group(1)
            fn.setdefault(cur, {"frame": 0, "calls": set(), "indirect": 0})
            continue
        if cur is None:
            continue
        f = fn[cur]
        for m in PUSH.finditer(ln):
            f["frame"] += 4 * nregs(m.group(1))
        for m in ADJ.finditer(ln):
            if int(m.group(1)) < 0:
                f["frame"] += -int(m.group(1))
        for m in CALL.finditer(ln):
            f["calls"].add(m.group(1).split("+")[0])
        if re.search(r"\bcall\s+r\d+", ln):
            f["indirect"] += 1
    memo = {}

    def depth(name, stack=()):
        if name in memo:
            return memo[name]
        if name not in fn or name in stack:
            return 0, [name + ("(recursion)" if name in stack else "")]
        best, path = 0, []
        for c in fn[name]["calls"]:
            d, p = depth(c, stack + (name,))
            if d > best:
                best, path = d, p
        r = (fn[name]["frame"] + best, [f"{name}({fn[name]['frame']})"] + path)
        memo[name] = r
        return r

    if not want:
        want = ["main", "fm1_alnk0_irq", "fm1_timer5_irq", "keys_block", "karma_block", "synth_render", "ui_draw"]
    for w in want:
        d, p = depth(w)
        line = f"{w}: {d} bytes deep: " + " > ".join(p)
        print(f"::notice title=stack {w}::{line}" if gh else line)


if __name__ == "__main__":
    main(sys.argv[1:])
