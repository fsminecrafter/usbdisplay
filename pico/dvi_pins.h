// dvi_pins.h - which Pico GPIOs carry the DVI signals to the Waveshare
// PICO-DVI-7inch board.
//
// Pinout (Waveshare wiki / Pico-DVI-LCD-Code.zip):
//   GP8  DVI_CLKN   GP9  DVI_CLKP
//   GP10 DVI_D0N    GP11 DVI_D0P
//   GP12 DVI_D1N    GP13 DVI_D1P
//   GP14 DVI_D2N    GP15 DVI_D2P
// The negative signal is on the lower pin of each pair, so the pairs are
// inverted. (Same as PicoDVI's "picodvi_dvi_cfg".)
//
// NOTE: Waveshare's demo *redefines* the name pico_sock_cfg to this layout.
// Upstream PicoDVI's pico_sock_cfg is a DIFFERENT layout (12/18/16, clk 14) and
// will give "No signal" on this panel. If you build your own PicoDVI projects for
// this display from the upstream repo, pass -DDVI_DEFAULT_SERIAL_CONFIG=picodvi_dvi_cfg.
//
// Other layouts can still be chosen at build time:
//   cmake .. -DDVI_PIN_CONFIG=<preset from common_dvi_pin_configs.h>
//   cmake .. -DDVI_INVERT=ON|OFF     (force differential-pair polarity)
#ifndef USBDISPLAY_DVI_PINS_H
#define USBDISPLAY_DVI_PINS_H

#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"   // PicoDVI presets (from PICODVI_DIR/include)

static const struct dvi_serialiser_cfg waveshare_pico_dvi_cfg = {
	.pio = pio0,
	.sm_tmds = {0, 1, 2},
	.pins_tmds = {10, 12, 14},   // D0, D1, D2 (lower pin of each pair)
	.pins_clk = 8,
	.invert_diffpairs = true
};

#ifndef DVI_PIN_CONFIG
#define DVI_PIN_CONFIG waveshare_pico_dvi_cfg
#endif

#endif
