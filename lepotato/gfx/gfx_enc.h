// gfx_enc.h - graphics-mode encoder: turns RGB332 frames into compact tile-op packets.
// See common/gfxproto.h for the wire format.
#ifndef USBDISPLAY_GFX_ENC_H
#define USBDISPLAY_GFX_ENC_H

#include <stdint.h>
#include <stddef.h>
#include "gfxproto.h"

typedef int (*gfx_emit_fn)(void *user, const uint8_t *data, size_t len);   // return <0 on failure

typedef struct {
	int w, h, tw, th;
	int raw_max, cache_tiles;
	int enable_scroll;                 // detect full-width vertical scrolls (default on)
	int use_seq;                       // number packets (needed when they are sent over two links)
	int budget_wire;                   // soft limit of bytes per frame (0 = unlimited); the rest of a big change
	                                   // is deferred to the next frames, always sending the newest content
	int deferred;                      // tiles left over by the last frame because of the budget
	float ratio;                       // recent wire bytes / op bytes (LZ4 gain), used to turn the budget into op bytes
	gfx_emit_fn emit; void *user;
	uint8_t seq;                       // seq of the last PRESENT sent
	uint16_t pseq;                     // next packet sequence number (use_seq)
	int err;                           // sticky: an emit call failed
	uint64_t st_frames, st_wire, st_ops[GFX_OP_COUNT];

	/* private */
	uint8_t *prev;                     // what the Pico is showing
	uint64_t *prev_rh, *cur_rh;        // per-row hashes
	uint8_t *kind, *tcol;              // per-tile scratch
	int16_t *rtab; uint8_t *rdup; int *votes; int rtab_mask;
	uint8_t *ctile; uint32_t *chash; uint16_t *ctab; uint32_t ctab_mask; int cnext;
	uint8_t *ops; int ops_len; int raw_total;
	uint8_t *lz, *pkt;
	uint8_t *bm[GFX_MAX_BM]; uint16_t bm_w[GFX_MAX_BM], bm_h[GFX_MAX_BM];
} gfx_enc_t;

// w and h must be multiples of 16. raw_max / cache_tiles come from the display's ENQ reply.
int  gfx_enc_init(gfx_enc_t *e, int w, int h, int raw_max, int cache_tiles, gfx_emit_fn emit, void *user);
void gfx_enc_free(gfx_enc_t *e);
void gfx_enc_reset(gfx_enc_t *e);                  // call when the Pico (re)enters graphics mode

// Encode `cur` (w*h RGB332) relative to what the Pico shows. Returns the number of bytes handed to
// emit() (0 = nothing changed, nothing sent) or -1 if emit failed.
long gfx_enc_frame(gfx_enc_t *e, const uint8_t *cur);

// Low-level API for programs that draw themselves instead of mirroring a screen.
// Both keep the encoder's model of the Pico screen up to date; call gfx_enc_present() when done.
int  gfx_enc_bitmap_define(gfx_enc_t *e, int id, int w, int h, const uint8_t *pixels);
int  gfx_enc_bitmap_blit(gfx_enc_t *e, int id, int x, int y);
long gfx_enc_present(gfx_enc_t *e);
int  gfx_enc_mode_text(gfx_enc_t *e);              // tell the Pico to go back to the text terminal

#endif
