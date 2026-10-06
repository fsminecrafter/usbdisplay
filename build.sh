#!/usr/bin/env bash
# build.sh - build the Pico firmware (usbdisplay.uf2) with a small selection menu.
#
#   ./build.sh                          menu: choose the device, then toggle options, then build
#   ./build.sh --device plus --gfx -y   no menu (device: pico | plus), build straight away
#
# Options for non-interactive use:
#   --device pico|plus     --gfx   --cache N   --font dejavu|terminus|legacy8x8   --invert
#   --polarity auto|on|off --clean --flash     -y|--yes (skip the options screen)
#   --psram|--no-psram (Pico Plus, default on)   --uart   --uart-baud N (1000000..4000000)
#
# Needs: cmake, arm-none-eabi-gcc, the Pico SDK (PICO_SDK_PATH) and PicoDVI (PICODVI_DIR,
# default ~/pico/PicoDVI/software). Output: dist/usbdisplay-<board>.uf2
set -u
cd "$(dirname "$0")"
ROOT=$PWD
BUILD_ROOT="${BUILD_ROOT:-$ROOT/pico/build}"
DIST_DIR="${DIST_DIR:-$ROOT/dist}"
PICODVI_DIR="${PICODVI_DIR:-$HOME/pico/PicoDVI/software}"

# ---------------------------------------------------------------- state
DEVICE=""                       # pico | plus
CACHES=(64 128 256 384);        C_IDX=2          # tile cache in SRAM (tiles)
PCACHES=(1024 4096 8192 16384); PC_IDX=2         # tile cache in PSRAM (tiles)
BAUDS=(1000000 2000000 3000000 4000000); B_IDX=1
FONTS=(dejavu terminus legacy8x8); F_IDX=0
POLS=(AUTO ON OFF);             P_IDX=0
O_GFX=0; O_INVERT=0; O_CLEAN=0; O_FLASH=0; O_PSRAM=-1; O_UART=0   # O_PSRAM -1 = default (on for Pico Plus)
YES=0

while [ $# -gt 0 ]; do
	case "$1" in
		--device) DEVICE="$2"; shift ;;
		--gfx) O_GFX=1 ;;
		--cache) for i in "${!CACHES[@]}"; do [ "${CACHES[$i]}" = "$2" ] && C_IDX=$i; done
		         for i in "${!PCACHES[@]}"; do [ "${PCACHES[$i]}" = "$2" ] && PC_IDX=$i; done; shift ;;
		--psram) O_PSRAM=1 ;;
		--no-psram) O_PSRAM=0 ;;
		--uart) O_UART=1 ;;
		--uart-baud) for i in "${!BAUDS[@]}"; do [ "${BAUDS[$i]}" = "$2" ] && B_IDX=$i; done; shift ;;
		--font) for i in "${!FONTS[@]}"; do [ "${FONTS[$i]}" = "$2" ] && F_IDX=$i; done; shift ;;
		--invert) O_INVERT=1 ;;
		--polarity) for i in "${!POLS[@]}"; do [ "${POLS[$i],,}" = "${2,,}" ] && P_IDX=$i; done; shift ;;
		--clean) O_CLEAN=1 ;;
		--flash) O_FLASH=1 ;;
		-y|--yes) YES=1 ;;
		-h|--help) sed -n '2,13p' "$0"; exit 0 ;;
		*) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
	esac
	shift
done
case "$DEVICE" in ""|pico|plus) ;; *) echo "--device must be pico or plus" >&2; exit 2 ;; esac

# ---------------------------------------------------------------- terminal helpers
if [ -t 1 ]; then B=$'\e[1m'; D=$'\e[2m'; G=$'\e[32m'; R=$'\e[31m'; Y=$'\e[33m'; Z=$'\e[0m'; INV=$'\e[7m'
else B=""; D=""; G=""; R=""; Y=""; Z=""; INV=""; fi
clear_screen() { [ -t 1 ] && printf '\e[H\e[2J\e[3J'; }

KEY=""
readkey() {
	local k rest=""
	IFS= read -rsn1 k || { KEY=EOF; return; }
	if [ "$k" = $'\e' ]; then IFS= read -rsn2 -t 0.05 rest || true; k+="$rest"; fi
	case "$k" in
		"") KEY=ENTER ;;
		$'\e[A') KEY=UP ;;
		$'\e[B') KEY=DOWN ;;
		$'\e') KEY=ESC ;;
		*) KEY="$k" ;;
	esac
}

psram_on() {   # 1 when this build puts the tile cache in PSRAM
	[ "$DEVICE" = plus ] || return 1
	[ "$O_PSRAM" = 0 ] && return 1
	return 0
}
cache_tiles() { if psram_on; then echo "${PCACHES[$PC_IDX]}"; else echo "${CACHES[$C_IDX]}"; fi; }

board_for() {
	case "$1" in
		pico) echo pico2_w ;;
		plus) echo pimoroni_pico_plus2_rp2350 ;;
	esac
}
device_name() { case "$1" in pico) echo "Pico" ;; plus) echo "Pico Plus" ;; esac; }

# ---------------------------------------------------------------- screen 1: device
select_device() {
	while :; do
		clear_screen
		printf '%sSelect Device%s\n\n' "$B" "$Z"
		printf '  1. Pico        %s(Pico 2 / Pico 2 W, board pico2_w)%s\n' "$D" "$Z"
		printf '  2. Pico Plus   %s(Pimoroni Pico Plus 2, RP2350B + PSRAM)%s\n\n' "$D" "$Z"
		printf '  q to quit\n\nSelection: '
		readkey
		case "$KEY" in
			1) DEVICE=pico; return 0 ;;
			2) DEVICE=plus; return 0 ;;
			q|Q|EOF) echo; exit 0 ;;
		esac
	done
}

# ---------------------------------------------------------------- screen 2: options
ITEMS=()
NITEMS=0
build_items() {
	ITEMS=(gfx)
	[ "$DEVICE" = plus ] && ITEMS+=(psram)
	ITEMS+=(cache uart baud font invert pol clean flash)
	NITEMS=${#ITEMS[@]}
}

box() { [ "$1" = 1 ] && printf '%s[x]%s' "$G" "$Z" || printf '[ ]'; }
item_text() {   # prints "label|value"
	case "$1" in
		gfx)    printf 'Graphics mode (-DGFX)|%s' "$(box $O_GFX) $([ $O_GFX = 1 ] && echo 'full-colour 640x480 stream, untested on hardware' || echo 'text terminal only')" ;;
		psram)  printf 'Use the 8 MB PSRAM (cache, bitmaps)|%s' "$(box $([ "$O_PSRAM" = 0 ] && echo 0 || echo 1)) $([ "$O_PSRAM" = 0 ] && echo 'SRAM only' || echo 'falls back to SRAM if the PSRAM test fails')" ;;
		cache)  if psram_on; then printf 'Tile cache size|< %s tiles = %s KB of PSRAM >%s' "${PCACHES[$PC_IDX]}" "$((PCACHES[PC_IDX] / 4))" "$([ $O_GFX = 0 ] && echo "  (graphics mode only)")"
		        else printf 'Tile cache size|< %s tiles = %s KB of SRAM >%s' "${CACHES[$C_IDX]}" "$((CACHES[C_IDX] / 4))" "$([ $O_GFX = 0 ] && echo "  (graphics mode only)")"; fi ;;
		uart)   printf 'Second link: UART (RX on GP1 + GND)|%s%s' "$(box $O_UART)" "$([ $O_GFX = 0 ] && echo "  (graphics mode only)")" ;;
		baud)   printf 'UART baud rate|< %s = ~%s KB/s >%s' "${BAUDS[$B_IDX]}" "$((BAUDS[B_IDX] / 10240))" "$([ $O_UART = 0 ] && echo "  (needs the UART link)")" ;;
		font)   printf 'Font|< %s >' "${FONTS[$F_IDX]}" ;;
		invert) printf 'Invert video (black on white)|%s' "$(box $O_INVERT)" ;;
		pol)    printf 'DVI pair polarity|< %s >%s' "${POLS[$P_IDX]}" "$([ $P_IDX = 0 ] && echo '  (AUTO = use the pin preset)')" ;;
		clean)  printf 'Clean build directory first|%s' "$(box $O_CLEAN)" ;;
		flash)  printf 'Copy to Pico afterwards (BOOTSEL)|%s' "$(box $O_FLASH)" ;;
	esac
}
item_change() {
	case "$1" in
		gfx)    O_GFX=$((1 - O_GFX)) ;;
		psram)  if [ "$O_PSRAM" = 0 ]; then O_PSRAM=1; else O_PSRAM=0; fi ;;
		cache)  if psram_on; then PC_IDX=$(((PC_IDX + 1) % ${#PCACHES[@]})); else C_IDX=$(((C_IDX + 1) % ${#CACHES[@]})); fi ;;
		uart)   O_UART=$((1 - O_UART)) ;;
		baud)   B_IDX=$(((B_IDX + 1) % ${#BAUDS[@]})) ;;
		font)   F_IDX=$(((F_IDX + 1) % ${#FONTS[@]})) ;;
		invert) O_INVERT=$((1 - O_INVERT)) ;;
		pol)    P_IDX=$(((P_IDX + 1) % ${#POLS[@]})) ;;
		clean)  O_CLEAN=$((1 - O_CLEAN)) ;;
		flash)  O_FLASH=$((1 - O_FLASH)) ;;
	esac
}

select_options() {   # returns 0 = build, 1 = change device
	local cur=0 i t label value
	build_items
	while :; do
		clear_screen
		printf '%sSelect Options%s   %sdevice: %s, board %s%s\n' "$B" "$Z" "$D" "$(device_name "$DEVICE")" "$(board_for "$DEVICE")" "$Z"
		printf '%senter = enable / change   up/down or j/k = move   b = build   d = change device   q = quit%s\n\n' "$D" "$Z"
		for ((i = 0; i < NITEMS; i++)); do
			t=$(item_text "${ITEMS[$i]}"); label=${t%%|*}; value=${t#*|}
			if [ $i = $cur ]; then printf ' %s>%s %d. %s%-34s%s %s\n' "$B" "$Z" $((i + 1)) "$INV" "$label" "$Z" "$value"
			else printf '   %d. %-34s %s\n' $((i + 1)) "$label" "$value"; fi
		done
		if [ $O_GFX = 1 ] && [ $O_UART = 1 ]; then
			printf '\n%sUART wiring: host TX -> Pico GP1 (pin 2), GND -> GND. Pass --uart /dev/ttyXXX to gfxstream/run.sh.%s\n' "$Y" "$Z"
		fi
		if [ $O_GFX = 1 ] && psram_on; then
			printf '%sPSRAM is initialised and memory-tested at boot (untested on hardware); a failed test falls back to the SRAM cache.%s\n' "$Y" "$Z"
		fi
		printf '\n'
		readkey
		case "$KEY" in
			UP|k) cur=$(((cur + NITEMS - 1) % NITEMS)) ;;
			DOWN|j) cur=$(((cur + 1) % NITEMS)) ;;
			ENTER|" ") item_change "${ITEMS[$cur]}" ;;
			[1-9]) [ "$KEY" -le "$NITEMS" ] && { cur=$((KEY - 1)); item_change "${ITEMS[$cur]}"; } ;;
			b|B) return 0 ;;
			d|D|ESC) return 1 ;;
			q|Q|EOF) echo; exit 0 ;;
		esac
	done
}

# ---------------------------------------------------------------- build
preflight() {
	local ok=0 board; board=$(board_for "$DEVICE")
	command -v cmake >/dev/null || { echo "${R}missing:${Z} cmake            (sudo apt install cmake)"; ok=1; }
	command -v arm-none-eabi-gcc >/dev/null || { echo "${R}missing:${Z} arm-none-eabi-gcc (sudo apt install gcc-arm-none-eabi)"; ok=1; }
	command -v make >/dev/null || command -v ninja >/dev/null || { echo "${R}missing:${Z} make or ninja (sudo apt install build-essential)"; ok=1; }
	if [ -z "${PICO_SDK_PATH:-}" ] || [ ! -d "$PICO_SDK_PATH" ]; then
		echo "${R}missing:${Z} PICO_SDK_PATH is not set to a Pico SDK checkout (export PICO_SDK_PATH=~/pico/pico-sdk)"; ok=1
	elif [ "$DEVICE" = plus ] && [ ! -f "$PICO_SDK_PATH/src/boards/include/boards/$board.h" ]; then
		echo "${R}missing:${Z} board '$board' is not in your Pico SDK (needs SDK 2.1 or newer). Update the SDK, or build the 'Pico' device."; ok=1
	fi
	[ -d "$PICODVI_DIR/libdvi" ] || { echo "${R}missing:${Z} PicoDVI at PICODVI_DIR=$PICODVI_DIR (expected a libdvi/ folder)"; ok=1; }
	return $ok
}

find_bootsel_drive() {
	local u="${USER:-$(id -un)}" d
	for d in "/media/$u/RP2350" "/media/$u/RPI-RP2" "/run/media/$u/RP2350" "/run/media/$u/RPI-RP2" /Volumes/RP2350 /Volumes/RPI-RP2; do
		[ -d "$d" ] && { echo "$d"; return 0; }
	done
	return 1
}

do_build() {
	local board dir args=() uf2 out
	board=$(board_for "$DEVICE")
	dir="$BUILD_ROOT/$board"
	args=(-DPICO_BOARD="$board" -DPICODVI_DIR="$PICODVI_DIR" -DFONT="${FONTS[$F_IDX]}")
	local sfx=""
	if [ $O_GFX = 1 ]; then
		args+=(-DGFX=ON "-DGFX_CACHE_TILES=$(cache_tiles)")
		if psram_on; then args+=(-DUSE_PSRAM=ON); sfx="$sfx-psram"; else args+=(-DUSE_PSRAM=OFF); fi
		if [ $O_UART = 1 ]; then args+=(-DGFX_UART=ON "-DGFX_UART_BAUD=${BAUDS[$B_IDX]}"); sfx="$sfx-uart"; else args+=(-DGFX_UART=OFF); fi
	else
		args+=(-DGFX=OFF)
	fi
	[ $O_INVERT = 1 ] && args+=(-DINVERT_VIDEO=ON) || args+=(-DINVERT_VIDEO=OFF)
	args+=("-DDVI_INVERT=${POLS[$P_IDX]}")

	echo
	echo "${B}Build${Z}: $(device_name "$DEVICE") ($board)  font=${FONTS[$F_IDX]}  graphics=$([ $O_GFX = 1 ] && echo "on, cache $(cache_tiles)$(psram_on && echo ' (PSRAM)')$([ $O_UART = 1 ] && echo ", UART ${BAUDS[$B_IDX]} baud")" || echo off)  invert=$([ $O_INVERT = 1 ] && echo yes || echo no)  polarity=${POLS[$P_IDX]}"
	preflight || { echo; echo "${R}Fix the above and run ./build.sh again.${Z}"; return 1; }

	[ $O_CLEAN = 1 ] && rm -rf "$dir"
	mkdir -p "$dir" "$DIST_DIR"
	echo "+ cmake -S pico -B ${dir#$ROOT/} ${args[*]}"
	cmake -S "$ROOT/pico" -B "$dir" "${args[@]}" || { echo "${R}cmake failed${Z}"; return 1; }
	echo "+ cmake --build ${dir#$ROOT/}"
	cmake --build "$dir" -j"$(nproc 2>/dev/null || echo 2)" || { echo "${R}build failed${Z}"; return 1; }

	uf2="$dir/usbdisplay.uf2"
	[ -f "$uf2" ] || { echo "${R}build finished but $uf2 was not produced${Z}"; return 1; }
	out="$DIST_DIR/usbdisplay-$board$([ $O_GFX = 1 ] && echo -gfx)$sfx.uf2"
	cp "$uf2" "$out"
	echo
	echo "${G}Done:${Z} ${out#$ROOT/}"

	if [ $O_FLASH = 1 ]; then
		local drv; drv=$(find_bootsel_drive) && { cp "$out" "$drv/" && echo "Copied to $drv - the Pico reboots by itself."; } || {
			if command -v picotool >/dev/null && picotool load -fx "$out" 2>/dev/null; then echo "Flashed with picotool."
			else echo "${Y}No Pico in BOOTSEL mode found.${Z} Hold BOOTSEL, plug it in, then copy the file to the RP2350 drive."; fi; }
	else
		echo "Flash: hold BOOTSEL, plug in the Pico, copy it to the RP2350 / RPI-RP2 drive."
	fi
	if [ $O_GFX = 1 ]; then
		echo "Then:  ./lepotato/gfx/gfxstream --info   (shows size, cache, PSRAM, UART as the Pico reports them)"
		echo "       ./run.sh$([ $O_UART = 1 ] && echo ' --uart /dev/ttyXXX')   (stream a screen)    ./test.sh --hardware"
	fi
	return 0
}

# ---------------------------------------------------------------- main
if [ -z "$DEVICE" ]; then select_device; fi
if [ $YES = 0 ]; then
	while ! select_options; do select_device; done
	clear_screen
fi
do_build
rc=$?
exit $rc
