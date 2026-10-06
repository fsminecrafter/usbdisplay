#!/usr/bin/env python3
"""usbdisplay.py - send text to the Pico 2 W USB display (runs on Le Potato / any Linux).

Dependency-free (stdlib only). Examples:

  usbdisplay.py "Hello world"            # append a line of text
  usbdisplay.py --clear "Fresh screen"   # clear first
  usbdisplay.py --clear                  # just clear
  dmesg | tail -n 20 | usbdisplay.py     # pipe anything in
  usbdisplay.py -f /etc/os-release       # send a file
  usbdisplay.py --follow /var/log/syslog # live tail of a file
  usbdisplay.py --sysinfo                # live status page, refreshed every 5 s

The display speaks plain text over USB serial (see pico/term.h for the few
control bytes). Text is converted to plain ASCII first (a, o instead of a-ring,
a/o-umlaut ...), because the 8x8 font only has ASCII.
"""
import argparse
import datetime
import glob
import os
import socket
import sys
import termios
import time
import tty
import unicodedata

COLS = 80
FF, HOME, ERASE_EOS = b"\x0c", b"\x01", b"\x0b"
ALLOWED_CTRL = set("\n\r\t\b\x01\x0b\x0c")


def sanitize(text: str) -> bytes:
    """Transliterate to ASCII the display can show; unknown characters -> '?'."""
    out = []
    for ch in unicodedata.normalize("NFD", text):
        if unicodedata.category(ch) == "Mn":      # drop combining accents
            continue
        o = ord(ch)
        if 32 <= o < 127 or ch in ALLOWED_CTRL:
            out.append(ch)
        elif o < 32 or o == 127:
            continue                               # other control chars
        else:
            out.append("?")
    return "".join(out).encode("ascii")


def find_port(explicit=None) -> str:
    if explicit:
        return explicit
    for pattern in ("/dev/serial/by-id/*Pico*", "/dev/serial/by-id/*2E8A*", "/dev/ttyACM*"):
        found = sorted(glob.glob(pattern))
        if found:
            return found[0]
    sys.exit("usbdisplay: no Pico serial port found (is it plugged in? try --port /dev/ttyACM0)")


class Display:
    def __init__(self, port: str):
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.fd)                        # no echo / newline translation
        attrs = termios.tcgetattr(self.fd)
        attrs[2] &= ~termios.HUPCL                 # keep DTR asserted after we exit
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)

    def send_bytes(self, data: bytes):
        view = memoryview(data)
        while view:
            try:
                n = os.write(self.fd, view)
            except BlockingIOError:
                time.sleep(0.005)
                continue
            view = view[n:]

    def send(self, text: str):
        self.send_bytes(sanitize(text))

    def close(self):
        os.close(self.fd)


def sysinfo_page() -> str:
    def line(s=""):
        return s[:COLS - 1].ljust(COLS - 1) + "\n"

    host = socket.gethostname()
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))           # no packets are sent
        ip = s.getsockname()[0]
        s.close()
    except OSError:
        ip = "n/a"
    try:
        up = float(open("/proc/uptime").read().split()[0])
        d, rem = divmod(int(up), 86400)
        h, rem = divmod(rem, 3600)
        uptime = f"{d}d {h:02d}:{rem // 60:02d}"
    except OSError:
        uptime = "n/a"
    try:
        load = " ".join(open("/proc/loadavg").read().split()[:3])
    except OSError:
        load = "n/a"
    try:
        temp = f"{int(open('/sys/class/thermal/thermal_zone0/temp').read()) / 1000:.1f} C"
    except (OSError, ValueError):
        temp = "n/a"
    try:
        mem = {}
        for l in open("/proc/meminfo"):
            k, v = l.split(":")
            mem[k] = int(v.split()[0])
        used = (mem["MemTotal"] - mem["MemAvailable"]) // 1024
        mem_s = f"{used} / {mem['MemTotal'] // 1024} MB"
    except (OSError, KeyError, ValueError):
        mem_s = "n/a"

    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    return (line(f"LE POTATO  {host}") + line("-" * 40) +
            line(f"IP      {ip}") + line(f"Uptime  {uptime}") +
            line(f"Load    {load}") + line(f"CPU     {temp}") +
            line(f"Memory  {mem_s}") + line() + line(now))


def follow(disp: Display, path: str):
    with open(path, "r", errors="replace") as f:
        f.seek(0, os.SEEK_END)
        size = f.tell()
        f.seek(max(0, size - 2048))
        if f.tell() > 0:
            f.readline()                            # drop partial first line
        while True:
            chunk = f.read()
            if chunk:
                disp.send(chunk)
            else:
                time.sleep(0.2)


def main():
    ap = argparse.ArgumentParser(description="Send text to the Pico USB display.")
    ap.add_argument("text", nargs="*", help="text to show (joined with spaces)")
    ap.add_argument("-p", "--port", help="serial port (default: auto-detect)")
    ap.add_argument("-c", "--clear", action="store_true", help="clear the screen first")
    ap.add_argument("-n", "--no-newline", action="store_true", help="don't add a newline after TEXT")
    ap.add_argument("-f", "--file", help="send the contents of FILE")
    ap.add_argument("--follow", metavar="FILE", help="show the tail of FILE and keep following it")
    ap.add_argument("--sysinfo", action="store_true", help="show a live status page")
    ap.add_argument("-i", "--interval", type=float, default=5.0, help="--sysinfo refresh seconds")
    args = ap.parse_args()

    disp = Display(find_port(args.port))
    try:
        if args.clear:
            disp.send_bytes(FF)

        if args.sysinfo:
            disp.send_bytes(FF)
            while True:
                disp.send_bytes(HOME)
                disp.send(sysinfo_page())
                disp.send_bytes(ERASE_EOS)
                time.sleep(args.interval)
        elif args.follow:
            follow(disp, args.follow)
        elif args.file:
            with open(args.file, "r", errors="replace") as f:
                disp.send(f.read())
        elif args.text:
            disp.send(" ".join(args.text) + ("" if args.no_newline else "\n"))
        elif not sys.stdin.isatty():
            while True:
                chunk = sys.stdin.buffer.read1(4096)
                if not chunk:
                    break
                disp.send(chunk.decode("utf-8", errors="replace"))
        elif not args.clear:
            ap.print_help()
    except KeyboardInterrupt:
        pass
    finally:
        disp.close()


if __name__ == "__main__":
    main()
