#!/usr/bin/env bash
# Tests build.sh's menu and the cmake command it produces, using stub cmake/arm-none-eabi-gcc (no SDK needed).
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bin" "$T/sdk/src/boards/include/boards" "$T/dvi/libdvi"
touch "$T/sdk/src/boards/include/boards/pimoroni_pico_plus2_rp2350.h"
cat > "$T/bin/cmake" <<'STUB'
#!/usr/bin/env bash
echo "$*" >> "$STUB_LOG"
if [ "$1" = "--build" ]; then touch "$2/usbdisplay.uf2"; fi
STUB
printf '#!/bin/sh\nexit 0\n' > "$T/bin/arm-none-eabi-gcc"
chmod +x "$T/bin/"*
export PATH="$T/bin:$PATH" PICO_SDK_PATH="$T/sdk" PICODVI_DIR="$T/dvi" BUILD_ROOT="$T/build" DIST_DIR="$T/dist" STUB_LOG="$T/log"
fail=0
check() { if grep -q -- "$2" "$T/log"; then echo "  ok   $1"; else echo "  FAIL $1 (wanted: $2)"; sed 's/^/       /' "$T/log"; fail=1; fi; }
checkno() { if grep -q -- "$2" "$T/log"; then echo "  FAIL $1 (unexpected: $2)"; fail=1; else echo "  ok   $1"; fi; }

# menu: device 2 (Pico Plus); options: Enter on item1 (graphics on), j j (-> font), Enter (terminus), then b
: > "$T/log"
# keys: '2' = Pico Plus; Enter = graphics on; 5 x j = down to Font (gfx, psram, cache, uart, baud, font); Enter = terminus; b = build
printf '2\njjjjj\nb' > "$T/keys1"
./build.sh < "$T/keys1" > "$T/out1" 2>&1
check "plus board selected"     "PICO_BOARD=pimoroni_pico_plus2_rp2350"
check "graphics enabled"        "DGFX=ON"
check "psram on by default for plus" "USE_PSRAM=ON"
check "psram cache size passed"  "GFX_CACHE_TILES=8192"
check "uart off by default"      "GFX_UART=OFF"
check "font cycled to terminus" "DFONT=terminus"
check "uf2 copied to dist"      "--build"
[ -f "$T/dist/usbdisplay-pimoroni_pico_plus2_rp2350-gfx-psram.uf2" ] && echo "  ok   dist file" || { echo "  FAIL dist file"; fail=1; }

# menu: device 1 (Pico), straight to build
: > "$T/log"; printf '1b' | ./build.sh > "$T/out2" 2>&1
check "pico board"   "PICO_BOARD=pico2_w"
check "gfx off"      "DGFX=OFF"
checkno "no psram on pico" "USE_PSRAM=ON"
checkno "no cache define when gfx off" "GFX_CACHE_TILES"

# device screen: q quits without building
: > "$T/log"; printf 'q' | ./build.sh > /dev/null 2>&1; rc=$?
[ $rc = 0 ] && [ ! -s "$T/log" ] && echo "  ok   q quits" || { echo "  FAIL q quits"; fail=1; }

# no-menu mode
: > "$T/log"; ./build.sh --device plus --gfx --cache 4096 --font legacy8x8 --invert --polarity on -y > "$T/out3" 2>&1
check "cli: cache"    "GFX_CACHE_TILES=4096"
check "cli: font"     "DFONT=legacy8x8"
check "cli: invert"   "DINVERT_VIDEO=ON"
check "cli: polarity" "DVI_INVERT=ON"

# options: uart + psram off + sram cache size
: > "$T/log"; ./build.sh --device plus --gfx --no-psram --cache 384 --uart --uart-baud 3000000 -y > "$T/out6" 2>&1
check "cli: psram off"  "USE_PSRAM=OFF"
check "cli: sram cache" "GFX_CACHE_TILES=384"
check "cli: uart"       "GFX_UART=ON"
check "cli: uart baud"  "GFX_UART_BAUD=3000000"
: > "$T/log"; ./build.sh --device pico --gfx --uart -y > "$T/out7" 2>&1
check "pico: uart"      "GFX_UART=ON"
checkno "pico: never psram" "USE_PSRAM=ON"

# menu: Pico Plus, toggle graphics (Enter), move down to the UART item (psram, cache, uart = 3 downs) and enable it
: > "$T/log"; printf '2\n' > "$T/keys2"; printf 'jjj\nb' >> "$T/keys2"; ./build.sh < "$T/keys2" > "$T/out8" 2>&1
check "menu: uart enabled via keys" "GFX_UART=ON"

# missing pieces are reported, nothing is built
: > "$T/log"; PICO_SDK_PATH="" ./build.sh --device pico -y > "$T/out4" 2>&1; rc=$?
[ $rc != 0 ] && grep -q "PICO_SDK_PATH" "$T/out4" && [ ! -s "$T/log" ] && echo "  ok   missing SDK reported" || { echo "  FAIL missing SDK"; cat "$T/out4"; fail=1; }
rm "$T/sdk/src/boards/include/boards/pimoroni_pico_plus2_rp2350.h"
: > "$T/log"; ./build.sh --device plus -y > "$T/out5" 2>&1; rc=$?
[ $rc != 0 ] && grep -q "needs SDK 2.1" "$T/out5" && echo "  ok   old SDK without Plus board reported" || { echo "  FAIL old SDK check"; cat "$T/out5"; fail=1; }
exit $fail
