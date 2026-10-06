#!/usr/bin/env bash
# setup.sh - install usbdisplay on the Le Potato (Debian / Ubuntu / Armbian).
#
#   ./setup.sh                 install packages, build, install tools, enable the tty1 mirror at boot
#   ./setup.sh --no-service    same, but do not enable/start the boot service
#   ./setup.sh --no-apt        skip apt (packages already installed)
#   ./setup.sh --uninstall     remove what this script installed
set -euo pipefail
cd "$(dirname "$0")"

APT=1; SERVICE=1; UNINSTALL=0
for a in "$@"; do
	case "$a" in
		--no-apt) APT=0 ;;
		--no-service) SERVICE=0 ;;
		--uninstall) UNINSTALL=1 ;;
		-h|--help) sed -n '2,8p' "$0"; exit 0 ;;
		*) echo "unknown option: $a" >&2; exit 2 ;;
	esac
done

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
	command -v sudo >/dev/null || { echo "Run as root or install sudo." >&2; exit 1; }
	SUDO="sudo"
fi
say() { printf '\n==> %s\n' "$*"; }

if [ "$UNINSTALL" = 1 ]; then
	say "Removing usbdisplay"
	$SUDO systemctl disable --now usbdisplay-tty.service 2>/dev/null || true
	$SUDO rm -f /etc/systemd/system/usbdisplay-tty.service /etc/systemd/system/usbdisplay-sysinfo.service \
	            /usr/local/bin/usbdisplay /usr/local/bin/usbdisplay-tty /usr/local/bin/usbdisplay-gfx
	$SUDO systemctl daemon-reload
	echo "done"; exit 0
fi

if [ "$APT" = 1 ]; then
	command -v apt-get >/dev/null || { echo "apt-get not found; install gcc make pkg-config libx11-dev libxext-dev xvfb python3 yourself and use --no-apt." >&2; exit 1; }
	say "Installing packages"
	$SUDO apt-get update
	# build-essential: gcc/make   libx11/libxext: screen capture   xvfb: headless X server for run.sh
	$SUDO apt-get install -y build-essential pkg-config libx11-dev libxext-dev xvfb x11-utils python3
fi

say "Building usbdisplay-tty (C)"
lepotato/build.sh --clean >/dev/null
lepotato/build.sh

say "Building gfxstream"
make -C lepotato/gfx clean >/dev/null
make -C lepotato/gfx

say "Installing programs to /usr/local/bin"
$SUDO install -m 755 lepotato/usbdisplay.py      /usr/local/bin/usbdisplay
$SUDO install -m 755 lepotato/build/usbdisplay-tty /usr/local/bin/usbdisplay-tty
$SUDO install -m 755 lepotato/gfx/gfxstream      /usr/local/bin/usbdisplay-gfx

say "Serial port access"
TARGET_USER="${SUDO_USER:-${USER:-$(id -un)}}"
if [ "$TARGET_USER" != root ] && ! id -nG "$TARGET_USER" | tr ' ' '\n' | grep -qx dialout; then
	$SUDO usermod -aG dialout "$TARGET_USER"
	echo "Added $TARGET_USER to 'dialout' - log out and back in for it to take effect."
else
	echo "ok"
fi

say "Installing systemd units"
$SUDO install -m 644 lepotato/usbdisplay-tty.service     /etc/systemd/system/usbdisplay-tty.service
$SUDO install -m 644 lepotato/usbdisplay-sysinfo.service /etc/systemd/system/usbdisplay-sysinfo.service
$SUDO systemctl daemon-reload

if [ "$SERVICE" = 1 ]; then
	say "Enabling the tty1 mirror at boot"
	# Only one program may own the serial port at a time.
	$SUDO systemctl disable --now usbdisplay-sysinfo.service 2>/dev/null || true
	$SUDO systemctl enable usbdisplay-tty.service
	$SUDO systemctl restart usbdisplay-tty.service       # also picks up a new binary on upgrade
	sleep 1
	systemctl --no-pager --lines=5 status usbdisplay-tty.service || true
else
	echo "Skipped. Enable later with: sudo systemctl enable --now usbdisplay-tty"
fi

cat <<MSG

Done.
  tty1 on the display  : sudo systemctl status usbdisplay-tty     (logs: journalctl -u usbdisplay-tty -f)
  stream the desktop   : ./run.sh            (needs firmware built with -DGFX=ON; stops the tty1 service while running)
  self-test            : ./test.sh
MSG
