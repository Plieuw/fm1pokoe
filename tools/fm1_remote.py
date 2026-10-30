#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""fm1_remote: look at and play the FM-1 (CHUA firmware) from a computer over USB-MIDI (firmware/src/remote.c).

    python3 tools/fm1_remote.py ping                 the firmware's identity
    python3 tools/fm1_remote.py state                the page, the screen's text, preset, BPM, load, tape, FX ...
    python3 tools/fm1_remote.py screen out.png       the picture (the plot area), saved as a PNG (x2)
    python3 tools/fm1_remote.py button FX            press a button (FX SCL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY
                                                     REC OCT- OCT+)
    python3 tools/fm1_remote.py turn "KNOB 1" 5      turn an encoder (SELECT ALGORITHM PRESETS "KNOB 1".."KNOB 4")
    python3 tools/fm1_remote.py note 60 100 0.5      play a note (velocity, seconds)
    python3 tools/fm1_remote.py cc 1 64              a control change (CC 1: the mod wheel)
    python3 tools/fm1_remote.py program 12           a program change (preset 13)
    python3 tools/fm1_remote.py watch 10 shots/      a screen every half second for 10 s

Needs: pip3 install mido python-rtmidi Pillow. The port is the one whose name has "FM" in it (--port to choose).
Recording the FM-1's USB audio is a job for the system's tools (e.g. ffmpeg / sox from its USB audio device)."""
import argparse
import sys
import time

BUTTONS = ["FX", "SCL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC", "OCT-", "OCT+"]
ENCODERS = ["SELECT", "ALGORITHM", "PRESETS", "KNOB 1", "KNOB 2", "KNOB 3", "KNOB 4"]
HDR = [0x7D, 0x43, 0x48]
PLOT_W = 240


def unpack7(b):
    out = bytearray()
    i = 0
    while i < len(b):
        hi = b[i]
        i += 1
        for j in range(7):
            if i >= len(b):
                break
            out.append(b[i] | ((hi >> j) & 1) << 7)
            i += 1
    return bytes(out)


class FM1:
    def __init__(self, port=None):
        import mido
        self.mido = mido
        names_out = mido.get_output_names()
        names_in = mido.get_input_names()
        pick = (lambda names: next((n for n in names if (port or "FM") in n), None))
        po, pi = pick(names_out), pick(names_in)
        if not po or not pi:
            sys.exit("no FM-1 MIDI port found; ports: %s" % names_out)
        self.out = mido.open_output(po)
        self.inp = mido.open_input(pi)

    def send(self, cmd, *data):
        self.out.send(self.mido.Message("sysex", data=HDR + [cmd] + list(data)))

    def replies(self, until, timeout=3.0):
        """our messages (cmd, payload) until one whose cmd is in until"""
        t0 = time.time()
        got = []
        while time.time() - t0 < timeout:
            for m in self.inp.iter_pending():
                if m.type == "sysex" and list(m.data[:3]) == HDR:
                    got.append((m.data[3], bytes(m.data[4:])))
                    if m.data[3] in until:
                        return got
            time.sleep(0.002)
        return got

    def ping(self):
        self.send(0x05)
        r = self.replies({0x51})
        return r[-1][1].decode() if r else None

    def state(self):
        self.send(0x02)
        r = self.replies({0x50})
        return r[-1][1].decode() if r else None

    def screen(self):
        """the plot as (width, height, RGB bytes)"""
        self.send(0x01)
        r = self.replies({0x42}, timeout=10.0)
        rows = {}
        h = 0
        for cmd, p in r:
            if cmd == 0x41:
                u = unpack7(p)
                y = u[0]
                px = u[1:]
                for k in range(len(px) // (2 * PLOT_W)):
                    rows[y + k] = px[k * 2 * PLOT_W:(k + 1) * 2 * PLOT_W]
            elif cmd == 0x42 and len(p) >= 2:
                h = p[0] | p[1] << 7
        if not h:
            return None
        rgb = bytearray()
        for y in range(h):
            row = rows.get(y, bytes(2 * PLOT_W))
            for x in range(PLOT_W):
                v = row[2 * x] << 8 | row[2 * x + 1]               # RGB565, its high byte first
                rgb += bytes(((v >> 11 & 31) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31))
        return PLOT_W, h, bytes(rgb)

    def button(self, name):
        self.send(0x03, BUTTONS.index(name.upper()))

    def turn(self, name, steps):
        self.send(0x04, ENCODERS.index(name.upper()), max(0, min(127, 64 + steps)))

    def note(self, n, vel=100, secs=0.5, ch=0):
        self.out.send(self.mido.Message("note_on", note=n, velocity=vel, channel=ch))
        time.sleep(secs)
        self.out.send(self.mido.Message("note_off", note=n, channel=ch))

    def cc(self, c, v, ch=0):
        self.out.send(self.mido.Message("control_change", control=c, value=v, channel=ch))

    def program(self, p, ch=0):
        self.out.send(self.mido.Message("program_change", program=p, channel=ch))


def save_png(shot, path, scale=2):
    from PIL import Image
    w, h, rgb = shot
    Image.frombytes("RGB", (w, h), rgb).resize((w * scale, h * scale), Image.NEAREST).save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="a part of the MIDI port's name (default: FM)")
    ap.add_argument("cmd")
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()
    fm = FM1(a.port)
    if a.cmd == "ping":
        print(fm.ping())
    elif a.cmd == "state":
        print(fm.state())
    elif a.cmd == "screen":
        s = fm.screen()
        if not s:
            sys.exit("no picture (a page without one, or no answer)")
        save_png(s, a.args[0] if a.args else "fm1_screen.png")
    elif a.cmd == "button":
        fm.button(a.args[0])
    elif a.cmd == "turn":
        fm.turn(a.args[0], int(a.args[1]))
    elif a.cmd == "note":
        fm.note(int(a.args[0]), int(a.args[1]) if len(a.args) > 1 else 100, float(a.args[2]) if len(a.args) > 2 else 0.5)
    elif a.cmd == "cc":
        fm.cc(int(a.args[0]), int(a.args[1]))
    elif a.cmd == "program":
        fm.program(int(a.args[0]))
    elif a.cmd == "watch":
        import os
        secs = float(a.args[0]) if a.args else 5
        d = a.args[1] if len(a.args) > 1 else "fm1_watch"
        os.makedirs(d, exist_ok=True)
        t0, i = time.time(), 0
        while time.time() - t0 < secs:
            s = fm.screen()
            if s:
                save_png(s, os.path.join(d, "%04d.png" % i))
                i += 1
            time.sleep(0.5)
        print("%d pictures in %s" % (i, d))
    else:
        sys.exit("unknown command: " + a.cmd)


if __name__ == "__main__":
    main()
