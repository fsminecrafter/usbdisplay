#!/usr/bin/env bash
# setup.sh - build and install the tty1 mirror (usbdisplay-tty) on the Le Potato (Debian / Ubuntu / Armbian).
#
#   ./setup.sh                 install a compiler if needed, build, install, enable the boot service
#   ./setup.sh --no-service    install the program but do not enable/start the service
#   ./setup.sh --no-apt        never call apt (gcc is already installed)
#   ./setup.sh --python        install the old Python version instead of the C program
#   ./setup.sh --uninstall     remove the program and the service
#
# This only handles usbdisplay-tty. The full installer (also usbdisplay, usbdisplay-gfx and their
# dependencies) is ../setup.sh, which uses the same C build.
set -euo pipefail
cd "$(dirname "$0")"

APT=1; SERVICE=1; UNINSTALL=0; PYTHON=0
for a in "$@"; do
	case "$a" in
		--no-apt) APT=0 ;;
		--no-service) SERVICE=0 ;;
		--python) PYTHON=1 ;;
		--uninstall) UNINSTALL=1 ;;
		-h|--help) sed -n '2,11p' "$0"; exit 0 ;;
		*) echo "unknown option: $a" >&2; exit 2 ;;
	esac
done

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
	command -v sudo >/dev/null || { echo "Run as root or install sudo." >&2; exit 1; }
	SUDO="sudo"
fi
say() { printf '\n==> %s\n' "$*"; }
BIN=/usr/local/bin/usbdisplay-tty
UNIT=/etc/systemd/system/usbdisplay-tty.service

if [ "$UNINSTALL" = 1 ]; then
	say "Removing usbdisplay-tty"
	$SUDO systemctl disable --now usbdisplay-tty.service 2>/dev/null || true
	$SUDO rm -f "$UNIT" "$BIN"
	$SUDO systemctl daemon-reload
	echo "done"; exit 0
fi

if [ "$PYTHON" = 1 ]; then
	command -v python3 >/dev/null || { echo "python3 not found" >&2; exit 1; }
	SRC=usbdisplay-tty.py
else
	if ! command -v gcc >/dev/null && ! command -v cc >/dev/null; then
		[ "$APT" = 1 ] && command -v apt-get >/dev/null || { echo "No C compiler. Install gcc, or use --python." >&2; exit 1; }
		say "Installing a C compiler"
		$SUDO apt-get update
		$SUDO apt-get install -y build-essential
	fi
	say "Building usbdisplay-tty"
	./build.sh --clean >/dev/null
	./build.sh
	SRC=build/usbdisplay-tty
fi

say "Installing $BIN"
$SUDO install -m 755 "$SRC" "$BIN"
"$BIN" --version 2>/dev/null || true

say "Installing the systemd unit"
$SUDO install -m 644 usbdisplay-tty.service "$UNIT"
$SUDO systemctl daemon-reload

if [ "$SERVICE" = 1 ]; then
	say "Enabling the tty1 mirror at boot"
	# Only one program may own the serial port at a time.
	$SUDO systemctl disable --now usbdisplay-sysinfo.service 2>/dev/null || true
	$SUDO systemctl enable usbdisplay-tty.service
	$SUDO systemctl restart usbdisplay-tty.service        # also picks up a new binary on upgrade
	sleep 1
	systemctl --no-pager --lines=6 status usbdisplay-tty.service || true
else
	echo "Skipped. Enable later with: sudo systemctl enable --now usbdisplay-tty"
fi

cat <<MSG

Done.
  status / logs   : systemctl status usbdisplay-tty      journalctl -u usbdisplay-tty -f
  frame rate      : sudo systemctl edit usbdisplay-tty   (override ExecStart, e.g.
                    ExecStart=
                    ExecStart=$BIN --tty 1 --fps-idle 2 --fps-mid 24 --fps-high 30 -v)
  CPU check       : top -p \$(pgrep -x usbdisplay-tty)
MSG
