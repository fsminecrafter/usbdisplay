#!/usr/bin/env bash
# run.sh - stream an X screen to the Pico display (graphics mode).
#
# Looks for a running X server (Xorg, Xwayland, Xvfb, Xvnc, ...). If there is none it starts a
# headless Xvfb on :99 (640x480) - start your programs with  DISPLAY=:99  to see them on the display.
#
#   ./run.sh                    auto-detect everything
#   ./run.sh --list             only show the X servers that were found
#   ./run.sh -d :1              use this X display
#   ./run.sh --no-xvfb          never start Xvfb; fall back to /dev/fb0 if there is no X server
#   ./run.sh --fb [/dev/fb0]    capture the framebuffer instead of X
#   ./run.sh --test             show the synthetic test pattern instead of a screen
#   ./run.sh --uart /dev/ttyXXX second link through a UART wired to the Pico (firmware built with the UART option)
#   ./run.sh [other options]    everything else goes to gfxstream (see: gfxstream --help), e.g. --fps 24 --port /dev/ttyACM1 --dither
#
# While it runs, usbdisplay-tty.service (the tty1 mirror) is stopped, because only one program can own the
# serial port; it is started again when you quit with Ctrl+C.
set -u
cd "$(dirname "$0")"

BIN=""
for c in ./lepotato/gfx/gfxstream /usr/local/bin/usbdisplay-gfx; do [ -x "$c" ] && { BIN="$c"; break; }; done
if [ -z "$BIN" ]; then
	echo "gfxstream is not built yet - building ..."
	make -C lepotato/gfx >/dev/null || { echo "build failed; run ./setup.sh first" >&2; exit 1; }
	BIN=./lepotato/gfx/gfxstream
fi

DISPLAY_ARG=""; LIST=0; ALLOW_XVFB=1; MODE=x11; FBDEV=/dev/fb0; PASS=()
while [ $# -gt 0 ]; do
	case "$1" in
		--list) LIST=1 ;;
		-d|--display) DISPLAY_ARG="$2"; shift ;;
		--no-xvfb) ALLOW_XVFB=0 ;;
		--fb) MODE=fb; if [ $# -gt 1 ] && [[ "$2" == /dev/* ]]; then FBDEV="$2"; shift; fi ;;
		--test) MODE=test ;;
		-h|--help) sed -n '2,17p' "$0"; exit 0 ;;
		*) PASS+=("$1") ;;
	esac
	shift
done

# ---- find X servers -------------------------------------------------------------------------
# Candidates: $DISPLAY, every socket in /tmp/.X11-unix, plus anything the process list reveals.
find_x_candidates() {
	[ -n "${DISPLAY:-}" ] && echo "$DISPLAY"
	for s in /tmp/.X11-unix/X*; do [ -S "$s" ] && echo ":${s##*/X}"; done
	ps -eo args= 2>/dev/null | grep -E '(^|/)(Xorg|X|Xwayland|Xvfb|Xvnc|Xephyr|Xnest|Xdummy|Xtigervnc)( |$)' | grep -v grep \
		| grep -oE ' :[0-9]+' | tr -d ' '
}
guess_xauth() {   # print a plausible Xauthority file for the invoking user / display manager
	[ -n "${XAUTHORITY:-}" ] && [ -r "$XAUTHORITY" ] && { echo "$XAUTHORITY"; return; }
	local u="${SUDO_USER:-${USER:-$(id -un)}}" h
	h=$(getent passwd "$u" 2>/dev/null | cut -d: -f6); h="${h:-$HOME}"
	for f in "$h/.Xauthority" /run/user/*/gdm/Xauthority /var/run/lightdm/root/:* /run/lightdm/root/:* /var/lib/lightdm/.Xauthority /root/.Xauthority; do
		[ -r "$f" ] && { echo "$f"; return; }
	done
}
probe() {   # probe DISPLAY -> prints "WxH" when connectable
	local xa; xa=$(guess_xauth)
	if [ -n "$xa" ]; then XAUTHORITY="$xa" "$BIN" --probe "$1" 2>/dev/null
	else "$BIN" --probe "$1" 2>/dev/null; fi
}

FOUND=(); declare -A SIZE
for d in $(find_x_candidates | awk '!seen[$0]++'); do
	if sz=$(probe "$d"); then FOUND+=("$d"); SIZE[$d]="$sz"; fi
done

echo "X servers: ${FOUND[*]:-none}"
for d in "${FOUND[@]:-}"; do [ -n "$d" ] && echo "  $d  ${SIZE[$d]}"; done
if [ -z "${DISPLAY_ARG}" ] && [ "${#FOUND[@]}" -eq 0 ] && [ -n "${WAYLAND_DISPLAY:-}" ]; then
	echo "note: a Wayland session is running but exposes no X server; Wayland screens cannot be captured."
fi
[ "$LIST" = 1 ] && exit 0

# ---- choose the source ----------------------------------------------------------------------
XVFB_PID=""
cleanup() {
	[ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null
	if [ "${RESTART_TTY:-0}" = 1 ]; then $SUDO systemctl start usbdisplay-tty.service 2>/dev/null || true; fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

ARGS=()
case "$MODE" in
	test) ARGS=(--source test) ;;
	fb)   ARGS=(--source fb --fb "$FBDEV") ;;
	x11)
		if [ -n "$DISPLAY_ARG" ]; then X="$DISPLAY_ARG"
		elif [ "${#FOUND[@]}" -gt 0 ]; then X="${FOUND[0]}"
		else X=""; fi
		if [ -z "$X" ]; then
			if [ "$ALLOW_XVFB" = 1 ] && command -v Xvfb >/dev/null; then
				X=:99
				echo "No X server found - starting Xvfb $X (640x480). Run programs with:  DISPLAY=$X <program> &"
				Xvfb "$X" -screen 0 640x480x24 -nolisten tcp >/dev/null 2>&1 &
				XVFB_PID=$!
				for _ in $(seq 1 50); do [ -S "/tmp/.X11-unix/X${X#:}" ] && break; sleep 0.1; done
				[ -S "/tmp/.X11-unix/X${X#:}" ] || { echo "Xvfb did not start" >&2; exit 1; }
			elif [ -r /dev/fb0 ]; then
				echo "No X server found and Xvfb is not allowed/installed - capturing /dev/fb0 instead."
				MODE=fb; ARGS=(--source fb --fb /dev/fb0)
			else
				echo "No X server found. Install Xvfb (./setup.sh), start a desktop, or use --test / --fb." >&2
				exit 1
			fi
		fi
		if [ "$MODE" = x11 ]; then
			XA=$(guess_xauth); [ -n "$XA" ] && export XAUTHORITY="$XA"
			ARGS=(--source x11 --display "$X")
			echo "Streaming X display $X"
		fi ;;
esac

# ---- free the serial port, run, give it back -------------------------------------------------
SUDO=""; [ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null && SUDO="sudo -n"
RESTART_TTY=0
if systemctl is-active --quiet usbdisplay-tty.service 2>/dev/null; then
	if $SUDO systemctl stop usbdisplay-tty.service 2>/dev/null; then RESTART_TTY=1
	else echo "warning: could not stop usbdisplay-tty.service (needs root) - the port may be busy. Try: sudo ./run.sh" >&2; fi
fi

"$BIN" --wait "${ARGS[@]}" "${PASS[@]}"
