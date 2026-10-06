#!/usr/bin/env python3
"""Checks usbdisplay-tty.py's drawing against a model of the Pico text terminal (pico/term.c)."""
import importlib.util, os, random, sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("tty", os.path.join(here, "..", "lepotato", "usbdisplay-tty.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

class Pico:
    def __init__(s, C=80, R=30): s.C, s.R = C, R; s.reset()
    def reset(s): s.g = [[' '] * s.C for _ in range(s.R)]; s.r = s.c = 0; s.pw = False
    def nl(s):
        if s.r == s.R - 1: s.g.pop(0); s.g.append([' '] * s.C)
        else: s.r += 1
    def feed(s, data):
        for b in data:
            if b == 0x0c: s.reset()
            elif b == 1: s.r = s.c = 0; s.pw = False
            elif b == 10: s.c = 0; s.pw = False; s.nl()
            elif b == 13: s.c = 0; s.pw = False
            elif 32 <= b < 127:
                if s.pw: s.c = 0; s.pw = False; s.nl()
                s.g[s.r][s.c] = chr(b)
                if s.c == s.C - 1: s.pw = True
                else: s.c += 1

R, C = 30, 80
def mk(lines, cx=0, cy=0):
    L = [(l.encode()[:C]).ljust(C) for l in lines] + [b" " * C] * (R - len(lines))
    return (L, [b"\x07" * C] * R, cx, cy)

random.seed(1)
pico, prev = Pico(), None
for i in range(60):
    lines = [f"line {k} {'x' * random.randint(0, 90)}" for k in range(random.randint(1, 30))]
    if i % 7 == 0: lines = []
    if lines: lines[-1] = f"[{'#' * i}{' ' * (60 - i)}] {i}%"
    s = mk(lines, random.randint(0, 79), random.randint(0, 29))
    pico.feed(m.render(s, prev, False, prev is None or i % 10 == 0)); prev = s
    for y in range(R):
        assert ''.join(pico.g[y]) == s[0][y].decode(), (i, y)
    assert (pico.r, pico.c) == (s[3], s[2]), (i, pico.r, pico.c, s[2], s[3])
c = mk(["hi"]); c[1][0] = bytes([0x1c]) + b"\x07" * (C - 1)
assert b"\x1b[0;91;44m" in m.render(c, None, True, True)
assert m.to_ascii(0x2588) == ord('#') and m.to_ascii(0xe9) == ord('e')
print("tty render ok")
