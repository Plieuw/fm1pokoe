#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The data symbols next to a symbol (build/felucca.sym, sorted by address): what lies around a stack or a buffer.
  tools/sym_near.py [--gh] NAME [N]"""
import re, sys
from pathlib import Path

B = Path(__file__).resolve().parent.parent / "build"
args = [a for a in sys.argv[1:] if a != "--gh"]
gh = "--gh" in sys.argv
name, n = args[0], int(args[1]) if len(args) > 1 else 8
rows = []
for ln in (B / "felucca.sym").read_text().splitlines():
    m = re.match(r"^([0-9a-f]{8}) .{7} (\S+)\s+([0-9a-f]+) (\S+)$", ln)
    if m and m.group(2) in (".bss", ".data", ".pool", ".noinit", "*ABS*"):
        rows.append((int(m.group(1), 16), int(m.group(3), 16), m.group(2), m.group(4)))
rows.sort()
idx = [i for i, r in enumerate(rows) if r[3] == name]
out = []
for i in idx:
    for a, sz, sec, nm in rows[max(0, i - n):i + n + 1]:
        out.append(f"{a:08x} {sz:6d} {sec:7s} {nm}{'   <--' if nm == name else ''}")
text = "%0A".join(out) if gh else "\n".join(out)
print(f"::notice title=near {name}::{text}" if gh else text)
