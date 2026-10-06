#!/usr/bin/env python3
"""Tests for lepotato/usbdisplay-tty.c (the C mirror of usbdisplay-tty.py).

  1. Unicode -> ASCII mapping: every code point against the Python reference.
  2. End to end: the real binary reads fake vcsa/vcsu files and writes to a pty that plays the Pico
     (same terminal model as test_tty_render.py). Checks the screen, cursor, colour output, crop/pad,
     idle behaviour (no traffic, ~0 CPU) and the dynamic frame-rate levels.

    python3 tests/test_tty_c.py            (CC=... CFLAGS=... to change the compiler)
"""
import importlib.util, os, pty, random, re, select, shlex, struct, subprocess, sys, tempfile, termios, time

here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, "..", "lepotato")
spec = importlib.util.spec_from_file_location("ref", os.path.join(src, "usbdisplay-tty.py"))
ref = importlib.util.module_from_spec(spec); spec.loader.exec_module(ref)

tmp = tempfile.mkdtemp()
CC = os.environ.get("CC", "gcc")
CFLAGS = shlex.split(os.environ.get("CFLAGS", "-O2 -g")) + ["-std=gnu11", "-Wall", "-Wextra"]

def build(source, out):
    r = subprocess.run([CC, *CFLAGS, source, "-o", out], capture_output=True, text=True)
    if r.returncode:
        print(r.stderr); sys.exit("build failed: " + source)
    return out

# ---------------------------------------------------------------- 1. mapping
dump = build(os.path.join(here, "test_tty_ascii.c"), os.path.join(tmp, "ascii"))
got = subprocess.run([dump], capture_output=True, check=True).stdout
assert len(got) == 0x110000
bad = [cp for cp in range(0x110000) if got[cp] != ref.to_ascii(cp)] if got != bytes(ref.to_ascii(cp) for cp in range(0x110000)) else []
assert not bad, f"to_ascii differs at U+{bad[0]:04X}: C {got[bad[0]]} python {ref.to_ascii(bad[0])}"
print("ascii mapping: all 1114112 code points match")

# ---------------------------------------------------------------- 2. end to end
exe = build(os.path.join(src, "usbdisplay-tty.c"), os.path.join(tmp, "usbdisplay-tty"))

class Pico:                                    # model of pico/term.c (as in test_tty_render.py)
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
    def rows(s): return [''.join(r) for r in s.g]

SGR = re.compile(rb"\x1b\[[0-9;]*m")

class Run:
    """The binary under test, a pty playing the Pico, and fake vcsa/vcsu files."""
    def __init__(s, *args, enq=b"USBDISPLAY 1 80x30 pins=8 clk=8 inv=0\r\n", vr=30, vc=80):
        s.vr, s.vc = vr, vc
        s.a, s.u = os.path.join(tmp, "vcsa"), os.path.join(tmp, "vcsu")
        for p in (s.a, s.u): open(p, "wb").close()
        s.set([], 0, 0)
        s.master, slave = pty.openpty()
        s.port = os.ttyname(slave)
        s.model, s.raw, s.nbytes = Pico(), b"", 0
        s.p = subprocess.Popen([exe, "--port", s.port, "--vcsa", s.a, "--vcsu", s.u, *args],
                               stderr=subprocess.PIPE, pass_fds=())
        os.close(slave) if False else None
        s.slave = slave
        deadline = time.time() + 5
        while time.time() < deadline:                      # answer the ENQ
            if b"\x05" in s.pump(0.1):
                os.write(s.master, enq); break
        else:
            raise AssertionError("program never sent ENQ")
    def pump(s, t):
        got = b""
        end = time.time() + t
        while True:
            r, _, _ = select.select([s.master], [], [], max(0, end - time.time()))
            if not r: break
            try: d = os.read(s.master, 65536)
            except OSError: break
            got += d
            if time.time() >= end: break
        s.raw += got; s.nbytes += len(got)
        s.model.feed(SGR.sub(b"", got))
        return got
    def set(s, lines, cx, cy, attrs=None, cps=None):
        """Write a vr x vc console. `lines` are str (may hold Unicode)."""
        vr, vc = s.vr, s.vc
        cells = []
        for y in range(vr):
            l = lines[y] if y < len(lines) else ""
            for x in range(vc):
                cells.append(ord(l[x]) if x < len(l) else 32)
        glyph = lambda c: c if 32 <= c < 127 else 63
        A = bytearray(bytes([vr, vc, cx, cy]))
        for i, c in enumerate(cells): A += bytes([glyph(c), (attrs or {}).get(i, 7)])
        U = bytearray(bytes([vr, vc, cx, cy])) + struct.pack(f"={vr*vc}I", *cells)
        for path, data in ((s.a, A), (s.u, U)):
            fd = os.open(path, os.O_WRONLY); os.ftruncate(fd, len(data)); os.pwrite(fd, bytes(data), 0); os.close(fd)
    def expect(s, lines, cx, cy, timeout=4.0, rows=30, cols=80):
        want = []
        for y in range(rows):
            l = lines[y] if y < len(lines) else ""
            want.append(''.join(chr(ref.to_ascii(ord(l[x]))) if x < len(l) else ' ' for x in range(cols)))
        end = time.time() + timeout
        while True:
            s.pump(0.05)
            if s.model.rows() == want and (s.model.r, s.model.c) == (cy, cx): return
            if time.time() > end: break
        for y in range(rows):
            if s.model.rows()[y] != want[y]: print(f"row {y}\n  got  {s.model.rows()[y]!r}\n  want {want[y]!r}")
        raise AssertionError(f"screen mismatch; cursor got {(s.model.r, s.model.c)} want {(cy, cx)}")
    def cpu(s):
        f = open(f"/proc/{s.p.pid}/stat").read().rsplit(")", 1)[1].split()
        return (int(f[11]) + int(f[12])) / os.sysconf("SC_CLK_TCK")
    def stop(s):
        s.p.terminate()
        try: rc = s.p.wait(5)
        except subprocess.TimeoutExpired: s.p.kill(); raise AssertionError("did not exit on SIGTERM")
        err = s.p.stderr.read().decode(); s.p.stderr.close(); os.close(s.master)
        return rc, err

R, C = 30, 80
random.seed(7)

# 2a. random screens, text only: every diff must reproduce the screen and park the cursor
r = Run("--fps-idle", "40", "--refresh", "1000")
r.expect([], 0, 0)
for i in range(40):
    lines = [f"line {k} {'x' * random.randint(0, 90)}" for k in range(random.randint(1, 30))]
    if i % 7 == 0: lines = []
    if lines: lines[-1] = f"[{'#' * i}{' ' * (60 - i)}] {i}%"
    if i % 5 == 3: lines[0:1] = ["caf\u00e9 \u2588\u2588\u2502 \u4e2d\u6587 \u2022"] if lines else []
    cx, cy = random.randint(0, 79), random.randint(0, 29)
    r.set(lines, cx, cy)
    r.expect([l[:80] for l in lines], cx, cy)
rc, err = r.stop(); assert rc == 0, (rc, err)
print("random screens (incl. Unicode, wrap, cursor): ok")

# 2b. the console is bigger than the display (cropped) and smaller (padded)
for vr, vc in ((40, 100), (24, 60)):
    r = Run("--fps-idle", "40", vr=vr, vc=vc)
    lines = [f"{y:02d} " + "abcdefghij" * 12 for y in range(vr)]
    r.set(lines, 5, 3); r.expect([l[:min(vc, 80)] for l in lines[:30]], 5, 3)
    assert r.stop()[0] == 0
print("crop / pad: ok")

# 2c. colour only when the display says so
attrs = {y * C + x: 0x1c for y in range(2) for x in range(10)}
r = Run("--fps-idle", "40", enq=b"USBDISPLAY 1 80x30 color\r\n")
r.set(["red on blue", "second"], 0, 0, attrs=attrs); r.expect(["red on blue", "second"], 0, 0)
assert b"\x1b[0;91;44m" in r.raw and r.raw.rstrip().endswith(b"\x1b[0m"), r.raw[:200]
r.stop()
r = Run("--fps-idle", "40")
r.set(["red on blue"], 0, 0, attrs=attrs); r.expect(["red on blue"], 0, 0)
assert b"\x1b" not in r.raw
r.stop()
print("colour: ok")

# 2d. idle: no traffic between refreshes, ~0 CPU
r = Run("--refresh", "100")
r.set(["hello"], 5, 0); r.expect(["hello"], 5, 0)
r.pump(1.5); n0, c0, t0 = r.nbytes, r.cpu(), time.time()
r.pump(3.0)
idle_bytes, idle_cpu = r.nbytes - n0, (r.cpu() - c0) / (time.time() - t0) * 100
assert idle_bytes == 0, f"{idle_bytes} bytes sent while nothing changed"
assert idle_cpu < 1.0, f"idle CPU {idle_cpu:.2f}%"
print(f"idle: 0 bytes in 3 s, CPU {idle_cpu:.2f}%")
rc, err = r.stop(); assert rc == 0

# 2e. periodic full refresh repairs a corrupted display
r = Run("--refresh", "1")
r.set(["hello"], 5, 0); r.expect(["hello"], 5, 0)
r.model.g[3][10] = "X"                                      # simulate a dropped/garbled byte
r.expect(["hello"], 5, 0, timeout=3.0)
assert r.stop()[0] == 0
print("full refresh repairs the display: ok")

# 2f. dynamic frame rate levels
r = Run("-v", "--hold-mid", "0.6", "--hold-high", "0.3", "--fps-idle", "10", "--fps-mid", "24", "--fps-high", "30")
r.set(["a"], 1, 0); r.expect(["a"], 1, 0)
time.sleep(1.2); r.pump(0.1)                                # settle to idle
r.set(["abc"], 3, 0); r.expect(["abc"], 3, 0); time.sleep(0.3)             # one row  -> mid
many = [f"busy row {k}" for k in range(20)]
r.set(many, 0, 19); r.expect(many, 0, 19); time.sleep(0.2)                 # 20 rows  -> high
time.sleep(1.6); r.pump(0.1)                                               # back down to idle
rc, err = r.stop(); assert rc == 0
levels = re.findall(r"-> (idle|mid|high) \(", err)
print("levels:", " ".join(levels))
assert "mid" in levels and "high" in levels and levels[-1] == "idle", err
assert levels.index("mid") < levels.index("high"), levels
print("dynamic fps: idle -> mid -> high -> idle: ok")
print("tty C ok")
