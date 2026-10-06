# usbdisplay

Le Potato / Windows  --USB serial-->  Pico 2 W  --DVI-->  Waveshare PICO-DVI-7inch (1024x600)

The Pico shows whatever text it receives as a scrolling terminal. Internally it is a
1bpp 640x480p60 DVI signal (the mode Waveshare's own demo uses; the panel scales it
to 1024x600). No framebuffer is stored: core 1 builds each scanline from the character grid
+ the font into a 1-bit line and TMDS-encodes it, so the whole screen costs 2400 bytes.
The screen is 80x30 characters (8x16 cells) in DejaVu Sans Mono, the usual Debian terminal font.

## 1. Build the Pico firmware (WSL)

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
| 0x05 | ENQ: Pico replies `USBDISPLAY 1 80x30` |

The font is ASCII only; both scripts convert (a-ring, a-umlaut, o-umlaut become a, a, o) and
send `?` for anything else. The Windows script also maps curly quotes, dashes, `...` and box-drawing
characters to plain ASCII, and strips ANSI colour codes.

## Tests

    gcc -fsanitize=address,undefined -Ipico -DTERM_COLS=80 -DTERM_ROWS=30 tests/test_term.c pico/term.c -o t && ./t
    gcc -Wall -Ipico -DTERM_COLS=80 -DTERM_ROWS=30 tests/test_font.c pico/term.c -o tf && ./tf [frame.pbm]
# USBdisplay-for-Pico-DVI
# USBdisplay-for-Pico-DVI
