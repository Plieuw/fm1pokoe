#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Which special registers / instructions the code uses, and where (build/felucca.dis): what an interrupt wrapper
must save. tools/insn_scan.py [--gh] [REGEX]   (default: hardware loops, MAC accumulators, other special registers)"""
import re, sys
from pathlib import Path

B = Path(__file__).resolve().parent.parent / "build"
HEAD = re.compile(r"^(?:[0-9a-f]+ )?<?([A-Za-z_.$][\w.$]*)>?:\s*$")
INSN = re.compile(r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2} )+\s*\t(.*)$")


def main(args):
    gh = "--gh" in args
    pats = [a for a in args if a != "--gh"] or [r"\brep\b|\bloop|\blsb\b|\blse\b|\blc\d?\b|\bmac[ab]?\b|\bmac[a-z]*|\bmulti|\bq\d\b|\bicfg\b|\bssp\b|\busp\b"]
    rx = re.compile("|".join(pats))
    toks, where, cur = {}, {}, None
    for ln in (B / "felucca.dis").read_text().splitlines():
        h = HEAD.match(ln)
        if h:
            cur = h.group(1)
            continue
        m = INSN.match(ln)
        if not m:
            continue
        ins = m.group(1).split("<")[0]
        for t in re.findall(r"[a-z_][a-z0-9_.]*", ins):
            toks[t] = toks.get(t, 0) + 1
        if rx.search(ins):
            where.setdefault(cur, []).append(ins.strip())
    out = ["words: " + ", ".join(f"{t} {n}" for t, n in sorted(toks.items(), key=lambda kv: -kv[1]) if not re.match(r"r\d+$", t))]
    for f, l in sorted(where.items()):
        out.append(f"{f}: {len(l)}: " + " | ".join(sorted(set(l))[:6]))
    if gh:                                     # (GitHub keeps 10 annotations a step: joined)
        for k in range(0, min(len(out), 90), 10):
            print("::notice title=insn::" + "%0A".join(o[:600].replace("%", "%25") for o in out[k:k + 10]))
    else:
        print("\n".join(out))


if __name__ == "__main__":
    main(sys.argv[1:])
