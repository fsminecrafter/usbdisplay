#!/usr/bin/env python3
"""usbdisplay-tty (Python version; the C program usbdisplay-tty.c is the default and uses less CPU) - mirror Linux virtual console (tty1) onto the Pico USB display.

Instead of trying to intercept bytes written to tty1, this reads the *finished screen*
of the kernel's own terminal emulator (/dev/vcsa1 = characters + colour attributes,
/dev/vcsu1 = real Unicode characters) and redraws the Pico whenever it changes.
Because the kernel has already interpreted every escape sequence, all of these just work:

  - clear / reset                       -> the display clears too
  - \\r progress bars (flatpak, apt, curl, dd ...) -> the line is replaced in place
  - erase line, cursor movement, scrolling, editors (nano, htop, ...)
  - colours                             -> sent as ANSI SGR, only if the display says it can do colour

The Pico firmware has no cursor addressing, so a frame is drawn as:
    HOME, then for each row: the 80 characters (if changed) or just "\\n" (if unchanged)
and the cursor is parked afterwards by re-printing the start of its row. Only changed rows
are sent, and a full refresh is sent every few seconds to repair any dropped byte.

Run as root (systemd service) - /dev/vcsa* and resizing the console need it.
"""
import argparse
import fcntl
import glob
import os
import re
import select
import signal
import struct
import sys
import termios
import time
import tty
import unicodedata

FF, HOME, ENQ = b"\x0c", b"\x01", b"\x05"
SGR_RESET = b"\x1b[0m"
VGA2ANSI = [0, 4, 2, 6, 1, 5, 3, 7]          # console colour index (VGA order) -> ANSI 0-7


def log(msg):
    print(f"usbdisplay-tty: {msg}", file=sys.stderr, flush=True)


# ---------------------------------------------------------------- character mapping

_H = {0x2500, 0x2501, 0x2504, 0x2505, 0x2508, 0x2509, 0x254C, 0x254D, 0x2550}
_V = {0x2502, 0x2503, 0x2506, 0x2507, 0x250A, 0x250B, 0x254E, 0x254F, 0x2551}
_CACHE = {}
_BUF = 1 << 16                  # read size for vcsa/vcsu (covers any console)
UNCHANGED = object()            # Console.snapshot(): nothing changed since last call


def to_ascii(cp: int) -> int:
    """Unicode code point -> one printable ASCII byte (the display font is ASCII only)."""
    r = _CACHE.get(cp)
    if r is not None:
        return r
    if 32 <= cp < 127:
        r = cp
    elif cp == 0 or cp == 0xA0:
        r = 32
    elif 0x2588 <= cp <= 0x258F or cp in (0x2590, 0x2593, 0x25A0, 0x25AE, 0x25AC):
        r = ord("#")                                   # progress-bar blocks
    elif cp in (0x2591, 0x2592):
        r = ord(".")
    elif cp in _H:
        r = ord("-")
    elif cp in _V:
        r = ord("|")
    elif 0x2500 <= cp <= 0x257F:
        r = ord("+")                                   # corners / junctions
    elif cp in (0x2022, 0x25CF):
        r = ord("*")
    elif cp == 0x2026:
        r = ord(".")
    elif cp in (0x2018, 0x2019):
        r = ord("'")
    elif cp in (0x201C, 0x201D):
        r = ord('"')
    elif cp in (0x2013, 0x2014):
        r = ord("-")
    else:
        base = "".join(c for c in unicodedata.normalize("NFD", chr(cp))
                       if unicodedata.category(c) != "Mn")
        r = ord(base) if len(base) == 1 and 32 <= ord(base) < 127 else ord("?")
    _CACHE[cp] = r
    return r


_T256 = bytes(to_ascii(i) for i in range(256))      # fast path for rows with only code points < 256


# ---------------------------------------------------------------- display

class Display:
    def __init__(self, port: str, verify: bool):
        self.port = port
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            tty.setraw(self.fd)
            attrs = termios.tcgetattr(self.fd)
            attrs[2] &= ~termios.HUPCL             # keep DTR asserted if we exit
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
            self.cols, self.rows, self.colour = 80, 30, False
            reply = self.identify()
            if verify and not reply.startswith("USBDISPLAY"):
                raise OSError(f"{port} did not answer ENQ like the display")
            m = re.search(r"(\d+)x(\d+)", reply)
            if m:
                self.cols, self.rows = int(m.group(1)), int(m.group(2))
            self.colour = bool(re.search(r"colou?r", reply, re.I))
        except BaseException:
            os.close(self.fd)
            raise

    def identify(self) -> str:
        termios.tcflush(self.fd, termios.TCIFLUSH)
        self.write(ENQ)
        buf, deadline = b"", time.monotonic() + 1.5
        while time.monotonic() < deadline and b"\n" not in buf:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if r:
                try:
                    buf += os.read(self.fd, 256)
                except BlockingIOError:
                    pass
        return buf.decode("ascii", "replace").strip()

    def write(self, data: bytes):
        view = memoryview(data)
        while view:
            _, w, _ = select.select([], [self.fd], [], 5.0)
            if not w:
                raise TimeoutError("display is not accepting data")
            try:
                n = os.write(self.fd, view)
            except BlockingIOError:
                continue
            view = view[n:]

    def drain(self):
        try:
            termios.tcflush(self.fd, termios.TCIFLUSH)
        except termios.error:
            pass

    def close(self):
        os.close(self.fd)


def open_display(explicit, wait=True) -> Display:
    """Find and open the display, waiting for it to be plugged in."""
    announced = False
    while True:
        if explicit:
            cands = [explicit] if os.path.exists(explicit) else []
        else:
            cands = []
            for pat in ("/dev/serial/by-id/*Pico*", "/dev/serial/by-id/*2E8A*", "/dev/ttyACM*"):
                for p in sorted(glob.glob(pat)):
                    if p not in cands:
                        cands.append(p)
        for p in cands:
            try:
                return Display(p, verify=not explicit)
            except OSError as e:
                log(f"{p}: {e}")
        if not wait:
            sys.exit("usbdisplay-tty: no display found")
        if not announced:
            log("waiting for the Pico display ...")
            announced = True
        time.sleep(1.0)


# ---------------------------------------------------------------- console access

class Console:
    def __init__(self, n: int):
        self.n = n
        self.vcsa = os.open(f"/dev/vcsa{n}", os.O_RDONLY)
        try:
            self.vcsu = os.open(f"/dev/vcsu{n}", os.O_RDONLY)
        except OSError:
            self.vcsu = None
        self._key = None
        self._rowcache = {}
        self._poll = select.poll()
        self._poll.register(self.vcsa, select.POLLPRI)    # POLLIN is always set on vcs; POLLPRI = screen changed

    def close(self):
        os.close(self.vcsa)
        if self.vcsu is not None:
            os.close(self.vcsu)

    def resize(self, rows: int, cols: int):
        fd = os.open(f"/dev/tty{self.n}", os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        finally:
            os.close(fd)

    def snapshot(self, rows: int, cols: int):
        """Return (lines, attrs, cx, cy) cropped/padded to rows x cols, None on a torn read,
        or UNCHANGED if the kernel screen is byte-identical to the previous call.

        lines[y] is `cols` bytes of printable ASCII, attrs[y] is `cols` console attribute bytes.

        The expensive part (Unicode -> ASCII) is skipped entirely when the raw bytes did not
        change, and otherwise only rows whose raw bytes changed are converted again."""
        raw = os.pread(self.vcsa, _BUF, 0)                  # one syscall: header + screen
        if len(raw) < 4:
            return None
        vr, vc, cx, cy = raw[:4]
        if not vr or not vc or len(raw) != 4 + 2 * vr * vc:
            return None

        rawu = None
        if self.vcsu is not None:
            u = os.pread(self.vcsu, _BUF * 2, 0)
            if len(u) == 4 + 4 * vr * vc:
                rawu = u[4:]
            elif len(u) == 4 * vr * vc:
                rawu = u
        key = (raw, rawu, rows, cols)
        if key == self._key:
            return UNCHANGED
        self._key = key

        glyphs, attrs = raw[4::2], raw[5::2]
        w = min(vc, cols)
        lines, atts = [], []
        for y in range(rows):
            if y < vr:
                lo = y * vc
                at = bytes(attrs[lo:lo + w])
                if rawu is not None:
                    chunk = rawu[4 * lo:4 * (lo + w)]
                    line = self._rowcache.get(chunk)
                    if line is None:
                        if not any(chunk[1::4]) and not any(chunk[2::4]) and not any(chunk[3::4]):
                            line = chunk[0::4].translate(_T256)      # all code points < 256
                        else:
                            line = bytes(to_ascii(c) for c in struct.unpack(f"={w}I", chunk))
                        if len(self._rowcache) > 2048:
                            self._rowcache.clear()
                        self._rowcache[chunk] = line
                else:                                   # no Unicode device: glyph index
                    line = bytes(g if 32 <= g < 127 else (32 if g == 0 else 63)
                                 for g in glyphs[lo:lo + w])
            else:
                line, at = b"", b""
            pad = cols - len(line)
            lines.append(line + b" " * pad)
            atts.append(at + b"\x07" * pad)
        return lines, atts, min(cx, cols - 1), min(cy, rows - 1)

    def wait(self, timeout: float):
        """Sleep until the kernel reports a screen update or `timeout` expires.

        /dev/vcsa* supports poll(): POLLPRI is raised when the console changes. If a kernel
        does not wake us, this simply acts as a sleep of `timeout` seconds."""
        try:
            self._poll.poll(timeout * 1000)
        except OSError:
            time.sleep(timeout)


# ---------------------------------------------------------------- drawing

def sgr(attr: int) -> bytes:
    if attr == 0x07:
        return SGR_RESET
    fg, bg = attr & 0x0F, (attr >> 4) & 0x07
    f = (90 if fg & 8 else 30) + VGA2ANSI[fg & 7]
    b = 40 + VGA2ANSI[bg]
    return f"\x1b[0;{f};{b}m".encode()


def emit_row(line: bytes, at: bytes, upto: int, colour: bool) -> bytes:
    if not colour:
        return line[:upto]
    out, cur = bytearray(), None
    for ch, a in zip(line[:upto], at[:upto]):
        if a != cur:
            out += sgr(a)
            cur = a
        out.append(ch)
    return bytes(out)


def render(snap, prev, colour: bool, full: bool) -> bytes:
    lines, atts, cx, cy = snap
    rows = len(lines)
    cols = len(lines[0])
    out = bytearray(HOME)
    for y in range(rows):
        changed = full or prev is None or lines[y] != prev[0][y] or (colour and atts[y] != prev[1][y])
        if changed:
            out += emit_row(lines[y], atts[y], cols, colour)
        if y != rows - 1:
            out += b"\n"
    # Park the cursor: HOME, down cy rows, re-print the start of that row up to cx.
    out += HOME + b"\n" * cy + emit_row(lines[cy], atts[cy], cx, colour)
    if colour:
        out += SGR_RESET
    return bytes(out)


def same(a, b, colour: bool) -> bool:
    """Would drawing `a` over `b` change anything on the display?"""
    if b is None:
        return False
    return a[0] == b[0] and a[2:] == b[2:] and (not colour or a[1] == b[1])


# ---------------------------------------------------------------- main loop

def session(args):
    disp = open_display(args.port)
    log(f"display {disp.port}: {disp.cols}x{disp.rows}, colour {'on' if disp.colour else 'off'}")
    colour = disp.colour if args.colour == "auto" else args.colour == "on"
    con = Console(args.tty)
    try:
        if args.resize:
            try:
                con.resize(disp.rows, disp.cols)
            except OSError as e:
                log(f"could not resize tty{args.tty} to {disp.cols}x{disp.rows}: {e}")
        disp.write(FF + (SGR_RESET if colour else b""))
        prev, last_full, period = None, 0.0, 1.0 / args.fps
        while True:
            snap = con.snapshot(disp.rows, disp.cols)
            now = time.monotonic()
            full = prev is None or now - last_full >= args.refresh
            if snap is UNCHANGED:
                snap = prev
            if snap is not None and (full or not same(snap, prev, colour)):
                disp.write(render(snap, prev, colour, full))
                disp.drain()
                prev = snap
                if full:
                    last_full = now
            elif snap is not None:
                prev = snap
            # Idle: sleep in poll() until the kernel signals a change (or the next full refresh
            # is due / a safety timeout). Busy: the sleep below caps updates at --fps.
            con.wait(min(args.idle, max(0.0, last_full + args.refresh - time.monotonic())))
            time.sleep(period)
    finally:
        con.close()
        disp.close()


def main():
    ap = argparse.ArgumentParser(description="Mirror a Linux virtual console to the Pico USB display.")
    ap.add_argument("-t", "--tty", type=int, default=1, help="virtual console number (default 1)")
    ap.add_argument("-p", "--port", help="serial port (default: auto-detect)")
    ap.add_argument("--colour", "--color", choices=["auto", "on", "off"], default="auto",
                    help="send ANSI colours (auto = only if the display announces colour support)")
    ap.add_argument("--no-resize", dest="resize", action="store_false",
                    help="do not resize the console to the display size (it is then cropped top-left)")
    ap.add_argument("--fps", type=float, default=20.0, help="max updates per second (default 20)")
    ap.add_argument("--idle", type=float, default=0.5,
                    help="max seconds to sleep when the screen is static; the kernel normally wakes us "
                         "sooner (default 0.5)")
    ap.add_argument("--refresh", type=float, default=5.0, help="full redraw interval in seconds (default 5)")
    args = ap.parse_args()

    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    while True:
        try:
            session(args)
        except KeyboardInterrupt:
            break
        except (OSError, TimeoutError) as e:
            log(f"{e}; retrying in 2 s")
            time.sleep(2)


if __name__ == "__main__":
    main()
