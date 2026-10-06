#!/usr/bin/env bash
# build.sh - build usbdisplay-tty (the tty1 mirror) from usbdisplay-tty.c.
#
#   ./build.sh               optimised build -> build/usbdisplay-tty
#   ./build.sh --debug       -O0 -g with AddressSanitizer + UBSan (for testing)
#   ./build.sh --static      statically linked binary (copy it to another board without libc worries)
#   ./build.sh --native      -march=native -mtune=native (only runs on a CPU like the one it was built on)
#   ./build.sh --clean       remove build/
#   ./build.sh --test        build, then run the C tests (tests/test_tty_c.py)
#   ./build.sh -o PATH       write the binary to PATH
#
# Needs only gcc (or cc/clang) - no libraries. CC=... overrides the compiler.
set -euo pipefail
cd "$(dirname "$0")"

OUT="build/usbdisplay-tty"; MODE=release; STATIC=0; NATIVE=0; TEST=0
while [ $# -gt 0 ]; do
	case "$1" in
		--debug)  MODE=debug ;;
		--static) STATIC=1 ;;
		--native) NATIVE=1 ;;
		--test)   TEST=1 ;;
		--clean)  rm -rf build; echo "removed build/"; exit 0 ;;
		-o)       OUT="${2:?-o needs a path}"; shift ;;
		-h|--help) sed -n '2,12p' "$0"; exit 0 ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
	shift
done

CC="${CC:-}"
if [ -z "$CC" ]; then
	for c in gcc cc clang; do command -v "$c" >/dev/null 2>&1 && { CC="$c"; break; }; done
fi
[ -n "$CC" ] || { echo "No C compiler found. Install one with: sudo apt-get install build-essential" >&2; exit 1; }

CFLAGS=(-std=gnu11 -Wall -Wextra)
if [ "$MODE" = debug ]; then
	CFLAGS+=(-O0 -g -fsanitize=address,undefined)
else
	CFLAGS+=(-O2 -DNDEBUG)
fi
[ "$NATIVE" = 1 ] && CFLAGS+=(-march=native -mtune=native)
LDFLAGS=()
[ "$STATIC" = 1 ] && LDFLAGS+=(-static)

mkdir -p "$(dirname "$OUT")"
"$CC" "${CFLAGS[@]}" usbdisplay-tty.c -o "$OUT" "${LDFLAGS[@]}"
[ "$MODE" = release ] && command -v strip >/dev/null 2>&1 && strip "$OUT" 2>/dev/null || true
echo "built $OUT ($(wc -c <"$OUT") bytes, $MODE, $CC)"

if [ "$TEST" = 1 ]; then
	CC="$CC" python3 ../tests/test_tty_c.py
fi
