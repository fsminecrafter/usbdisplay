# usbdisplay

Le Potato / Windows  --USB serial-->  Pico 2 W (or Pico Plus)  --DVI-->  Waveshare PICO-DVI-7inch (1024x600)

The Pico shows whatever text it receives as a scrolling terminal. Internally it is a
1bpp 640x480p60 DVI signal (the mode Waveshare's own demo uses; the panel scales it
to 1024x600). No framebuffer is stored: core 1 builds each scanline from the character grid
+ the font into a 1-bit line and TMDS-encodes it, so the whole screen costs 2400 bytes.
The screen is 80x30 characters (8x16 cells) in DejaVu Sans Mono, the usual Debian terminal font.

## 1. Build the Pico firmware (WSL)

Quickest: `./build.sh`. It asks for the device (1. Pico, 2. Pico Plus), then shows an options screen
(Enter changes the highlighted option, `b` builds, `d` goes back to the device, `q` quits): graphics mode,
tile cache size, font, invert video, DVI polarity, clean build, and copying to the Pico if it is in BOOTSEL
mode. The result is `dist/usbdisplay-<board>[-gfx].uf2`. Pico = board `pico2_w`; Pico Plus = Pimoroni Pico Plus 2
(`pimoroni_pico_plus2_rp2350`, needs Pico SDK 2.1+). `./build.sh --help` lists flags for a menu-free build,
e.g. `./build.sh --device plus --gfx -y`. The manual steps are below.

Needs pico-sdk (`PICO_SDK_PATH`) and PicoDVI, as in your existing setup.

    cd pico
    mkdir build && cd build
    cmake .. -DPICODVI_DIR=$HOME/pico/PicoDVI/software      # add -DPICO_BOARD=pico2 if you like
    make -j4

Hold BOOTSEL, plug in the Pico, copy `usbdisplay.uf2` to the RPI-RP2 drive.

**DVI pins.** The default (`waveshare_pico_dvi_cfg` in `pico/dvi_pins.h`) is this board's real pinout:
clock GP8/GP9, D0 GP10/11, D1 GP12/13, D2 GP14/15, pairs inverted. Note that Waveshare's demo
redefines the name `pico_sock_cfg` to this layout; upstream PicoDVI's `pico_sock_cfg` is different
and gives "No signal" on this panel. For your own projects built from upstream PicoDVI use
`cmake .. -DDVI_DEFAULT_SERIAL_CONFIG=picodvi_dvi_cfg` (identical pins).

Other layouts at build time (fresh build dir, then `make`):

    cmake .. -DDVI_PIN_CONFIG=<preset name from PicoDVI's common_dvi_pin_configs.h>
    cmake .. -DDVI_INVERT=ON      # or OFF: force the differential-pair polarity

To see what a flashed build uses, open the serial port, send byte 0x05 and read the reply:
`USBDISPLAY 1 80x30 pins=.. clk=.. inv=..`.

Options: `-DINVERT_VIDEO=ON` (black on white).

**Font.** `-DFONT=dejavu` (default, DejaVu Sans Mono hinted to 8x16), `-DFONT=terminus` (Terminus
8x16, a crisper pixel font) or `-DFONT=legacy8x8` (the old PicoDVI 8x8 font doubled vertically;
add `-DFONT_SCALE_Y=1` for 80x60 characters). The 8x16 tables in `pico/font_8x16_*.h` are generated
by `tools/make_font.py` (needs Pillow), which can turn any TTF/BDF/PCF into a new one:

    python3 tools/make_font.py /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf \
        --size 14 --baseline 13 --name dejavu --out pico/font_8x16_dejavu.h --preview dejavu.png

DejaVu Sans Mono glyphs: (c) Bitstream Vera / DejaVu fonts, free license
(https://dejavu-fonts.github.io/License.html). Terminus: SIL OFL 1.1, Dimitar Zhekov.

## Status LED (use this when the screen stays blank)

| LED | Meaning |
|-----|---------|
| solid on, stays on | firmware started but hung before DVI came up |
| 3 quick blinks | DVI output started |
| fast blink (5 Hz) | ERROR: core 1 never started DVI |
| slow blink (1 Hz) | running, no USB host connected (or host not holding DTR) |
| short flash every 2 s | running, USB host connected, idle |
| solid on while text arrives | receiving data |

Blank screen but the LED reaches the 1 Hz / heartbeat pattern = the firmware is fine and the DVI
pin map in `pico/dvi_pins.h` (or the panel input / power) is the problem. The LED on a Pico 2 W is on the Wi-Fi
chip, so the build links the CYW43 driver for it (Wi-Fi is never started).

## 2. Le Potato

Plug the Pico into a USB port. It appears as `/dev/ttyACM0`. No packages needed.

    ./usbdisplay.py "Hello"                 # append a line
    ./usbdisplay.py --clear "Fresh screen"
    some_command | ./usbdisplay.py          # pipe anything
    ./usbdisplay.py -f file.txt
    ./usbdisplay.py --follow /var/log/syslog
    ./usbdisplay.py --sysinfo               # live status page (hostname, IP, load, temp ...)

If you get "permission denied": `sudo usermod -aG dialout $USER`, then log in again.
`usbdisplay-sysinfo.service` runs the status page at boot.

## 3. Windows

Run `.\send.ps1` with no arguments for an interactive prompt:

    >>> send text (shift enter for newline, enter to send)

| Key / command | What it does |
|---------------|--------------|
| Enter | send the text to the display |
| Shift+Enter | new line (Ctrl+Enter, Alt+Enter and Ctrl+J also work) |
| Up / Down | earlier messages (or move between lines of a multi-line message) |
| Esc | clear the input |
| `clear` | clear the Windows console **and** the display |
| `exit` | quit (`quit`, or Ctrl+C on an empty prompt, also work) |
| `--copy` | copy mode on/off (see below); `--copy off`, or `--copy <command>` for just one command |
| `help` | list these |

To send the word "clear" itself, type `\clear`.

**Copy mode.** After `--copy` the prompt becomes `PS C:\your\folder>` and whatever you type is run as a
PowerShell command. The command, and everything it prints, shows up on the terminal *and* on the display
(like `tee`; the terminal keeps its output). `cd` and variables persist between commands. Output is
formatted to the display width. Use it for commands that finish on their own (`dir`, `ipconfig`, `ping`,
`git status`); programs that wait for keyboard input (`python`, `ssh`) do not work in it, and Ctrl+C while a
command runs stops the whole script. Start in copy mode with `.\send.ps1 -Copy`.

One-shot use still works:

    .\send.ps1 "Hello from Windows"
    .\send.ps1 -Clear "Fresh screen"
    Get-Date | .\send.ps1
    .\send.ps1 -File .\notes.txt -Port COM7
    some-command | powershell -File .\send.ps1

The COM port is found automatically (Pico USB VID 2E8A); the prompt asks the display for its size on start
and says so if nothing answers. If scripts are blocked:
`powershell -ExecutionPolicy Bypass -File .\send.ps1`. Run it in a normal console or Windows Terminal, not
the PowerShell ISE.

## 4. tty1 on the display (boot service)

`lepotato/usbdisplay-tty.c` mirrors virtual console 1 onto the display, like a monitor would. It does not
intercept bytes; it reads the finished screen from the kernel's terminal emulator (`/dev/vcsa1` and `/dev/vcsu1`)
and redraws whenever it changes. So clear screen, `\r` progress bars (flatpak, apt, curl), erase-line, scrolling,
nano/htop all behave exactly as on a monitor. Only changed rows are sent. Non-ASCII glyphs are mapped to ASCII
(block characters become `#`). The console is resized to the display (80x30); `--no-resize` crops instead.

Colour is converted to ANSI `ESC[..m` but only sent if the display's ENQ reply contains the word `color`
(the current firmware is monochrome and would print the escape codes as text).

**Dynamic frame rate.** nothing changing = 2 fps, something changing (typing, a progress bar) = 24 fps, a lot changing
(8+ rows in one frame: scrolling, htop, editors) = 30 fps. It steps up immediately and steps down after 1 s / 0.5 s.
While idle the program sleeps in `poll()` on `/dev/vcsa1` (the kernel raises POLLPRI on a screen update), so it reacts to a
change at once and uses ~0 CPU; if `poll()` misbehaves it falls back to the 2 fps timer. Tune with
`--fps-idle`, `--fps-mid`, `--fps-high`, `--busy-rows`, `--hold-mid`, `--hold-high`; `-v` logs the level changes.

    cd lepotato
    ./setup.sh                    # installs gcc if needed, builds, installs and enables usbdisplay-tty.service at boot
    ./build.sh                    # just build -> lepotato/build/usbdisplay-tty   (--debug, --static, --native, --test)
    ./setup.sh --python           # install the old Python version instead
    journalctl -u usbdisplay-tty -f

The top-level `./setup.sh` does the same as part of the full install. `usbdisplay-tty.py` is kept as a fallback.

## 5. Streaming a screen (graphics mode)

`./run.sh` captures an X screen, encodes it and sends it over USB. It looks for a running X server
(`$DISPLAY`, `/tmp/.X11-unix`, `Xorg`/`Xwayland`/`Xvfb`/`Xvnc` processes); with none it starts a headless
`Xvfb :99` at 640x480. Wayland-only sessions cannot be captured. `--fb` captures `/dev/fb0` instead, `--test`
shows a synthetic pattern. While it runs, the tty1 service is stopped (one owner per serial port) and restarted on exit.

**Firmware.** Graphics mode is opt-in: `cmake .. -DGFX=ON` (optional `-DGFX_CACHE_TILES=N`). It switches the
DVI output to full-colour 640x480 RGB332 (8 bit) from a 307 KB framebuffer; the text terminal keeps working
(white on black). Without `-DGFX=ON` the firmware is unchanged and `run.sh` reports that the firmware has no
graphics mode. **The GFX scan-out path was written without the Pico SDK/PicoDVI at hand and has not been
compiled or run on hardware.** The protocol, decoder, encoder and host tools are tested (`./test.sh`).

**How it stays small.** The screen is split into 16x16 tiles; only changed tiles are sent, as ops (see
`common/gfxproto.h`): `FILL` (solid rectangles, a whole white screen is ~15 bytes), `COPY` (full-width vertical
scrolls), `MONO` (2-colour tiles = most text, 37 B instead of 259 B), `CACHED` (a tile seen before, 5 B, from a
ring cache that both sides keep in step without messages), `RAW`, plus explicit `BM_DEF`/`BM_BLIT` bitmaps. Packets
are LZ4-compressed when that is smaller (`common/lz4mini.h`, standard LZ4 block format, no dependencies).
The Pico acknowledges each frame and the host keeps at most 2 in flight.

**Two links (USB + UART).** USB full speed carries about 0.8 MB/s in practice. A UART wired to the Pico adds
`baud / 10` bytes per second (1 Mbaud = 100 KB/s ... 4 Mbaud = 400 KB/s). Build the firmware with the UART option
(`./build.sh`, "Second link: UART"), wire **host TX -> Pico GP1 (pin 2)** and **GND -> GND**, then
`./run.sh --uart /dev/ttyXXX` (the device your header UART shows up as; enable it in the board's overlay first). The host
takes the baud rate from the Pico, numbers every packet and sends each one over whichever link would finish it first;
the Pico executes packets strictly in number order and holds an early one until its predecessor arrives on the other
link, so the result is identical to a single stream (tested with random interleaving). The UART is received by DMA
into a 16 KB ring, so it costs no CPU; the host never has more than half of the ring unacknowledged (the Pico
acknowledges every executed packet over USB). USB stays required: it powers the Pico and carries the acks.
Whether the Le Potato's UART really runs at 3-4 Mbaud depends on its clock; try 1, 2, 3, 4 Mbaud and watch the
"decode errors" warning. Core 1 is busy producing the DVI signal, so there is no spare core: the second link helps
only because DMA does the UART work. Core 0 still copies USB bytes one by one, which may become the limit at the top end.

**Frame budget.** `gfxstream` knows the link speed and gives each frame a byte budget (sum of the links x 0.9 / fps).
A change bigger than that is sent in raster order across several frames; tiles that did not fit are compared against
the *newest* screen next time, so a busy screen never queues stale pictures. `--no-budget` turns this off,
`--rate KB` overrides the speed, `--dither` adds ordered dithering when reducing to 8 bit colour (good for photos
and video, worse for flat UI colours), and the capture rate drops to `--idle-fps` while the screen is static.

**PSRAM (Pico Plus).** With "Use the 8 MB PSRAM" the firmware initialises the PSRAM on boot (QMI chip select 1,
GPIO 47, conservative clock), memory-tests it, and only then places the tile cache (up to 16384 tiles = 4 MB) and
1 MB of bitmap memory there. If the chip is missing or the test fails it falls back to a small SRAM cache, so the
display still works. `gfxstream --info` shows what the Pico actually got (`PSRAM 8388608 B` and the cache size).
The framebuffer stays in SRAM: scanout reads 640 B per line (~20 MB/s) and must never stall. A 16 bit framebuffer
in PSRAM would give better colour and needs that bandwidth plus more link speed; it is not implemented.
**The PSRAM and UART code (`pico/psram.c`, `pico/uart_lane.c`) was written from the datasheets without hardware and has
not been compiled against the SDK**; everything host-side (encoder, decoder, ordering, budget, scheduler) is tested.

Other limits: 8 bpp RGB332, 640x480. Frames are applied to the live framebuffer (no vsync flip), so large changes can
tear briefly. A Pico 2 W has about 100 KB of SRAM to spare after the framebuffer; lower the cache if the link fails.

## Protocol

Plain bytes over USB CDC, so `echo hi > /dev/ttyACM0` also works. The Pico only reads while the
host holds DTR high (both scripts do this).

| Byte | Meaning |
|------|---------|
| 0x20-0x7E | printable ASCII (auto-wraps at column 80) |
| 0x0A / 0x0D | newline / carriage return |
| 0x0C | clear screen |
| 0x01 | cursor home (no clear) |
| 0x0B | erase from cursor to end of screen |
| 0x08 / 0x09 | backspace / tab |
| 0x05 | ENQ: Pico replies `USBDISPLAY 1 80x30 ...` (GFX firmware adds `gfx=640x480x8 cache=N raw=N bm=0`) |
| 0x0E | enter graphics mode (GFX firmware only; the Pico answers 0x08, then binary packets follow, see `common/gfxproto.h`) |

The font is ASCII only; both scripts convert (a-ring, a-umlaut, o-umlaut become a, a, o) and
send `?` for anything else. The Windows script also maps curly quotes, dashes, `...` and box-drawing
characters to plain ASCII, and strips ANSI colour codes.

## Tests

    ./test.sh               # everything below plus gfxstream, a fake-Pico pty test and an Xvfb capture test
    ./test.sh --hardware    # also talks to a connected Pico

Individually:

    gcc -fsanitize=address,undefined -Ipico -DTERM_COLS=80 -DTERM_ROWS=30 tests/test_term.c pico/term.c -o t && ./t
    gcc -Wall -Ipico -DTERM_COLS=80 -DTERM_ROWS=30 tests/test_font.c pico/term.c -o tf && ./tf [frame.pbm]
    python3 tests/test_tty_c.py     # usbdisplay-tty.c against a model of the Pico (pty + fake vcsa files)
    python3 tools/gen_tty_ascii.py  # regenerate lepotato/usbdisplay-tty-ascii.h after changing to_ascii() in the .py
