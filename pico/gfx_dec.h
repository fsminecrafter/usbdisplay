// gfx_dec.h - graphics-mode packet decoder (no hardware dependencies, host-testable).
// See common/gfxproto.h for the wire format. The caller owns all buffers, so on a Pico with PSRAM
// the tile cache and bitmap arena can simply point into PSRAM.
#ifndef USBDISPLAY_GFX_DEC_H
#define USBDISPLAY_GFX_DEC_H

#include <stdint.h>
#include <stddef.h>
#include "gfxproto.h"

// Receive state of one link (USB or UART).
typedef struct {
	uint8_t *payload; int cap;
	uint8_t st, hdr[6];
	int hdr_have, hdr_need;
	int plen, phave;
	uint8_t ptype, has_seq, pending;       // pending: complete packet waiting for its turn
	uint16_t pseq;
} gfx_lane_t;

typedef struct {
	// --- configured by gfx_dec_init ---
	uint8_t *fb;               // w*h bytes RGB332, row-major
	int w, h;                  // multiples of 16
	uint8_t *cache;            // cache_tiles * 256 bytes (may be NULL if cache_tiles == 0)
	int cache_tiles;
	uint8_t *bm_arena;         // bitmap storage (may be NULL / 0)
	uint32_t bm_size;
	uint8_t *raw;              // raw_max bytes (LZ4 output)
	int raw_max;
	// --- state ---
	int cache_next;
	uint32_t bm_used;
	struct { uint32_t off; uint16_t w, h; uint8_t valid; } bm[GFX_MAX_BM];
	gfx_lane_t lane[GFX_MAX_LANES];
	int nlanes;
	uint16_t next_seq;
	uint16_t ackq[32]; uint8_t ack_head, ack_tail;   // executed packet sequence numbers, for the host
	// --- outputs for the caller ---
	volatile uint8_t ack_flag;  // set on PRESENT: send GFX_ACK_BYTE then ack_seq
	uint8_t ack_seq;
	volatile uint8_t leave;     // set when MODE_TEXT arrived
	uint32_t errors, frames;
} gfx_dec_t;

// `payload` is the receive buffer of lane 0 (raw_max + 2 bytes). Add a second link with gfx_dec_add_lane().
void gfx_dec_init(gfx_dec_t *g, uint8_t *fb, int w, int h, uint8_t *cache, int cache_tiles,
                  uint8_t *bm_arena, uint32_t bm_size, uint8_t *payload, uint8_t *raw, int raw_max);
int  gfx_dec_add_lane(gfx_dec_t *g, uint8_t *payload, int cap);   // returns the lane number, or -1
void gfx_dec_reset(gfx_dec_t *g);                 // clear screen, cache, bitmaps, parsers, sequence

// Feed bytes of lane `lane`. Return the number of bytes consumed: less than n when the lane completed a packet
// that must wait for an earlier sequence number (keep the rest and retry after the other lane delivered).
size_t gfx_dec_feed_bytes_lane(gfx_dec_t *g, int lane, const uint8_t *buf, size_t n);
int  gfx_dec_lane_blocked(const gfx_dec_t *g, int lane);
int  gfx_dec_pop_pkt_ack(gfx_dec_t *g, uint16_t *seq);   // 1 if an executed-packet ack was queued

// Lane 0 conveniences (single-link use).
void gfx_dec_feed(gfx_dec_t *g, uint8_t byte);
void gfx_dec_feed_bytes(gfx_dec_t *g, const uint8_t *buf, size_t n);

#endif
