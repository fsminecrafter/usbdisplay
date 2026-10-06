#!/usr/bin/env bash
# test.sh - self-test for usbdisplay. Needs gcc and python3; nothing here touches the Pico unless
# you pass --hardware.
#
#   ./test.sh               software tests only
#   ./test.sh --hardware    also talk to a connected Pico (ENQ check, text, 5 s of graphics test pattern)
set -u
cd "$(dirname "$0")"
HW=0; [ "${1:-}" = "--hardware" ] && HW=1
PASS=0; FAIL=0; SKIP=0
ok()   { printf '  \033[32mPASS\033[0m  %s\n' "$1"; PASS=$((PASS+1)); }
bad()  { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; FAIL=$((FAIL+1)); }
skip() { printf '  SKIP  %s\n' "$1"; SKIP=$((SKIP+1)); }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
run() { local name="$1"; shift; if "$@" >"$TMP/out" 2>&1; then ok "$name"; else bad "$name"; sed 's/^/        /' "$TMP/out" | tail -15; fi; }

echo "tools"
for t in gcc make python3; do command -v $t >/dev/null && ok "$t found" || bad "$t missing (run ./setup.sh)"; done
command -v pkg-config >/dev/null && pkg-config --exists x11 xext 2>/dev/null && ok "X11 dev libraries" || skip "X11 dev libraries missing (screen capture from X unavailable; ./setup.sh installs them)"
command -v Xvfb >/dev/null && ok "Xvfb found" || skip "Xvfb missing (run.sh cannot start a headless X server)"

echo "python"
run "usbdisplay.py compiles"      python3 -m py_compile lepotato/usbdisplay.py
run "usbdisplay-tty.py compiles"  python3 -m py_compile lepotato/usbdisplay-tty.py
run "tty mirror drawing vs terminal model" python3 tests/test_tty_render.py
run "usbdisplay-tty.c: unicode map, end-to-end vs terminal model, idle CPU, dynamic fps" python3 tests/test_tty_c.py
CFLAGS="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined" run "usbdisplay-tty.c under ASan/UBSan" python3 tests/test_tty_c.py
find . -name __pycache__ -prune -exec rm -rf {} + 2>/dev/null

echo "shell"
for f in setup.sh run.sh test.sh build.sh; do run "$f syntax" bash -n "$f"; done

run "build.sh menu + cmake arguments (stub toolchain)" tests/test_build.sh

echo "firmware logic (host build)"
SAN="-fsanitize=address,undefined"
gcc -Wall -Ipico -DTERM_COLS=80 -DTERM_ROWS=30 $SAN tests/test_term.c pico/term.c -o "$TMP/t_term" 2>"$TMP/out" && run "terminal emulation" "$TMP/t_term" || { bad "terminal emulation (build)"; tail -5 "$TMP/out"; }
gcc -O1 -g -Wall -Wextra $SAN -Icommon -Ilepotato/gfx -Ipico tests/test_gfx.c lepotato/gfx/gfx_enc.c pico/gfx_dec.c -o "$TMP/t_gfx" 2>"$TMP/out" \
	&& run "graphics encoder/decoder round trip (LZ4, FILL, COPY scroll, MONO, RAW, tile cache, bitmaps, resync)" "$TMP/t_gfx" \
	|| { bad "graphics tests (build)"; tail -8 "$TMP/out"; }

echo "gfxstream"
if make -C lepotato/gfx >"$TMP/out" 2>&1; then
	ok "gfxstream builds"
	lepotato/gfx/gfxstream --source test --null --frames 300 2>"$TMP/bench" && ok "benchmark ran" || bad "benchmark failed"
	sed 's/^/        /' "$TMP/bench" | tail -3
	lepotato/gfx/gfxstream --probe "${DISPLAY:-:0}" >/dev/null 2>&1 && ok "X display ${DISPLAY:-:0} reachable" || skip "no reachable X display (run.sh will start Xvfb)"
	gcc -O1 -Wall -Icommon -Ipico tests/fake_pico.c pico/gfx_dec.c -o "$TMP/fake_pico" 2>"$TMP/out" || { bad "fake Pico build"; tail -5 "$TMP/out"; }
	if [ -x "$TMP/fake_pico" ]; then
		"$TMP/fake_pico" >"$TMP/fake.out" 2>&1 &
		FP=$!
		for _ in $(seq 1 50); do [ -s "$TMP/fake.out" ] && break; sleep 0.1; done
		PTY=$(head -1 "$TMP/fake.out")
		lepotato/gfx/gfxstream --port "$PTY" --source test --frames 200 --fps 200 >"$TMP/gs.out" 2>&1; GS=$?
		wait $FP; FPRC=$?
		RES=$(tail -1 "$TMP/fake.out")
		if [ $GS = 0 ] && [ $FPRC = 0 ] && echo "$RES" | grep -q "errors=0"; then ok "end-to-end over a pty with a fake Pico ($RES)"
		else bad "end-to-end over a pty ($RES)"; tail -5 "$TMP/gs.out" | sed 's/^/        /'; fi
	fi
	if [ -x "$TMP/fake_pico" ]; then          # two links (USB + UART) and PSRAM-sized cache against a fake Pico
		"$TMP/fake_pico" --uart --psram >"$TMP/fake2.out" 2>&1 &
		FP=$!
		for _ in $(seq 1 50); do [ "$(wc -l <"$TMP/fake2.out")" -ge 2 ] && break; sleep 0.1; done
		PTY1=$(sed -n 1p "$TMP/fake2.out"); PTY2=$(sed -n 2p "$TMP/fake2.out")
		INFO=$(lepotato/gfx/gfxstream --port "$PTY1" --info 2>&1 | tail -1)
		echo "$INFO" | grep -q "PSRAM 8388608" && ok "ENQ reports PSRAM and UART ($INFO)" || { bad "ENQ info ($INFO)"; }
		lepotato/gfx/gfxstream --port "$PTY1" --uart "$PTY2" --source noise --frames 120 --fps 60 --no-budget >"$TMP/gs2.out" 2>&1; GS=$?
		wait $FP; FPRC=$?
		RES=$(tail -1 "$TMP/fake2.out"); UARTB=$(echo "$RES" | sed -n 's/.*uart=\([0-9]*\).*/\1/p')
		if [ $GS = 0 ] && [ $FPRC = 0 ] && echo "$RES" | grep -q "errors=0" && [ "${UARTB:-0}" -gt 100000 ]; then ok "two links: packets executed in order, no errors ($RES)"
		else bad "two-link run ($RES)"; tail -5 "$TMP/gs2.out" | sed 's/^/        /'; fi
	fi
	if command -v Xvfb >/dev/null; then
		Xvfb :97 -screen 0 640x480x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
		for _ in $(seq 1 50); do [ -S /tmp/.X11-unix/X97 ] && break; sleep 0.1; done
		if lepotato/gfx/gfxstream --source x11 --display :97 --null --frames 20 2>"$TMP/x" ; then ok "X11 capture + encode on a private Xvfb"; else bad "X11 capture on Xvfb"; tail -3 "$TMP/x" | sed 's/^/        /'; fi
		kill $XP 2>/dev/null; wait $XP 2>/dev/null
	fi
else
	bad "gfxstream build"; tail -10 "$TMP/out" | sed 's/^/        /'
fi

if [ "$HW" = 1 ]; then
	echo "hardware"
	PORT=$(ls /dev/serial/by-id/*Pico* /dev/serial/by-id/*2E8A* /dev/ttyACM* 2>/dev/null | head -1)
	if [ -z "$PORT" ]; then bad "no Pico found"; else
		systemctl is-active --quiet usbdisplay-tty.service && { sudo -n systemctl stop usbdisplay-tty.service 2>/dev/null && RESTART=1 || echo "        (could not stop usbdisplay-tty.service; port may be busy)"; }
		python3 lepotato/usbdisplay.py --clear "usbdisplay test.sh" >"$TMP/out" 2>&1 && ok "text sent to $PORT" || { bad "text to $PORT"; cat "$TMP/out"; }
		lepotato/gfx/gfxstream --port "$PORT" --source test --frames 150 2>"$TMP/gfx" && ok "graphics test pattern streamed (5 s)" || { bad "graphics mode (firmware built without -DGFX=ON?)"; tail -4 "$TMP/gfx" | sed 's/^/        /'; }
		sed 's/^/        /' "$TMP/gfx" | tail -3
		[ "${RESTART:-0}" = 1 ] && sudo -n systemctl start usbdisplay-tty.service
	fi
fi

echo
echo "passed $PASS, failed $FAIL, skipped $SKIP"
[ "$FAIL" = 0 ]
