// gfxproto.h - wire format of the usbdisplay graphics mode (shared by Le Potato encoder and Pico decoder).
//
// Enter graphics mode : host sends byte 0x0E (SO) while the display is in text mode.
// Leave graphics mode : host sends a MODE_TEXT packet.
// Pico -> host        : after every PRESENT op the Pico sends 0x06 followed by the PRESENT's seq byte.
//
// Packet (everything little-endian):
//   0xA5 | type | len_lo | len_hi | chk | payload[len]                      chk = type ^ len_lo ^ len_hi ^ 0x5A
//   with bit 7 of type set (multi-link mode) a u16 packet sequence number follows len:
//   0xA5 | type|0x80 | len_lo | len_hi | seq_lo | seq_hi | chk | payload     chk also xors seq_lo ^ seq_hi
//   type 1  OPS      payload = a stream of ops (below)
//   type 2  OPS_LZ4  payload = u16 raw_len + LZ4 block that decompresses to raw_len bytes of ops
//   type 0x11 MODE_TEXT  (len 0) back to the text terminal
// Ops never straddle a packet, so every packet can be decoded on its own. The Pico drops a
// corrupt packet and hunts for the next 0xA5 that is followed by a valid header.
//
// Two links (USB + UART): the host numbers packets 0,1,2,... and spreads them over both links. The Pico
// executes packets strictly in sequence order, holding a packet that arrives early until its predecessor
// has arrived on the other link, so the result is identical to one ordered stream. A lane that is holding
// a packet accepts no more bytes (USB stalls by itself; UART relies on the host's window, see below).
//
// Pico -> host bytes (always over USB):
//   0x06 seq          frame presented (PRESENT op with that seq was executed)
//   0x07 lo hi errs   packet `seq` was executed; errs = decoder error count (saturating)
//   0x08              graphics mode is ready (reply to the enter byte; send UART data only after this)
//
// Pixels are 8 bit RGB332 (RRRGGGBB). The screen is split into 16x16 tiles; tile coordinates
// are in tiles. Unchanged tiles cost nothing because every op carries its own coordinates.
//
//   0x01 FILL    tx ty tw th color                     solid tile rectangle                   6 B
//   0x02 COPY    sx sy dx dy w h (u16 pixels)          overlap-safe rectangle copy (scrolls) 13 B
//   0x03 MONO    tx ty fg bg mask[32]                  2-colour tile, 16 x u16 rows, bit x=1 -> fg  37 B
//   0x04 RAW     tx ty pixels[256]                     verbatim tile                         259 B
//   0x05 CACHED  tx ty slot(u16)                       tile from the tile cache                5 B
//   0x06 BM_DEF  id(u16) w(u16) h(u16) pixels[w*h]     store a bitmap
//   0x07 BM_BLIT id(u16) x(i16) y(i16)                 draw a stored bitmap (clipped)          7 B
//   0x08 PRESENT seq                                   frame complete -> Pico acks             2 B
//
// Tile cache: a ring of N tiles. After every MONO or RAW tile is drawn, the Pico copies that tile
// into slot `next` and advances next = (next+1) % N. The encoder mirrors the ring, so slots never
// need to be announced. FILL and CACHED tiles are not inserted.
#ifndef USBDISPLAY_GFXPROTO_H
#define USBDISPLAY_GFXPROTO_H

#include <stdint.h>

#define GFX_SYNC          0xA5
#define GFX_ENTER_BYTE    0x0E
#define GFX_ACK_BYTE      0x06
#define GFX_PKT_ACK_BYTE  0x07
#define GFX_READY_BYTE    0x08
#define GFX_HDR_LEN       5
#define GFX_HDR_LEN_SEQ   7
#define GFX_SEQ_FLAG      0x80
#define GFX_MAX_LANES     2
#define GFX_TILE          16
#define GFX_TILE_BYTES    256
#define GFX_RAW_MAX_DEFAULT 16384
#define GFX_MAX_BM        32

enum { GFX_PKT_OPS = 1, GFX_PKT_OPS_LZ4 = 2, GFX_PKT_MODE_TEXT = 0x11 };
enum { GFX_OP_FILL = 1, GFX_OP_COPY, GFX_OP_MONO, GFX_OP_RAW, GFX_OP_CACHED,
       GFX_OP_BM_DEF, GFX_OP_BM_BLIT, GFX_OP_PRESENT, GFX_OP_COUNT };

static inline uint8_t gfx_hdr_chk(uint8_t type, uint8_t lo, uint8_t hi) {
	return (uint8_t)(type ^ lo ^ hi ^ 0x5A);
}
static inline uint8_t gfx_hdr_chk_seq(uint8_t type, uint8_t lo, uint8_t hi, uint8_t slo, uint8_t shi) {
	return (uint8_t)(type ^ lo ^ hi ^ slo ^ shi ^ 0x5A);
}

#endif
