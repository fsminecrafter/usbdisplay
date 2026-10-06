#include <string.h>
#include "gfx_dec.h"
#include "lz4mini.h"

enum { S_SYNC, S_HDR, S_PAYLOAD };

void gfx_dec_init(gfx_dec_t *g, uint8_t *fb, int w, int h, uint8_t *cache, int cache_tiles,
                  uint8_t *bm_arena, uint32_t bm_size, uint8_t *payload, uint8_t *raw, int raw_max) {
	memset(g, 0, sizeof *g);
	g->fb = fb; g->w = w; g->h = h;
	g->cache = cache; g->cache_tiles = cache_tiles;
	g->bm_arena = bm_arena; g->bm_size = bm_size;
	g->raw = raw; g->raw_max = raw_max;
	g->lane[0].payload = payload; g->lane[0].cap = raw_max + 2;
	g->nlanes = 1;
	gfx_dec_reset(g);
}

int gfx_dec_add_lane(gfx_dec_t *g, uint8_t *payload, int cap) {
	if (g->nlanes >= GFX_MAX_LANES) return -1;
	g->lane[g->nlanes].payload = payload; g->lane[g->nlanes].cap = cap;
	return g->nlanes++;
}

void gfx_dec_reset(gfx_dec_t *g) {
	memset(g->fb, 0, (size_t)g->w * g->h);
	if (g->cache_tiles) memset(g->cache, 0, (size_t)g->cache_tiles * GFX_TILE_BYTES);
	g->cache_next = 0;
	g->bm_used = 0;
	memset(g->bm, 0, sizeof g->bm);
	for (int i = 0; i < g->nlanes; ++i) {
		gfx_lane_t *l = &g->lane[i];
		l->st = S_SYNC; l->hdr_have = 0; l->plen = l->phave = 0; l->pending = 0;
	}
	g->next_seq = 0; g->ack_head = g->ack_tail = 0;
	g->ack_flag = 0; g->leave = 0;
}

static void cache_insert(gfx_dec_t *g, int tx, int ty) {
	if (!g->cache_tiles) return;
	uint8_t *dst = g->cache + (size_t)g->cache_next * GFX_TILE_BYTES;
	const uint8_t *src = g->fb + (size_t)ty * GFX_TILE * g->w + tx * GFX_TILE;
	for (int r = 0; r < GFX_TILE; ++r) memcpy(dst + r * GFX_TILE, src + (size_t)r * g->w, GFX_TILE);
	if (++g->cache_next >= g->cache_tiles) g->cache_next = 0;
}

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// Execute an op stream. Returns on the first malformed op (the rest of the packet is dropped).
static void run_ops(gfx_dec_t *g, const uint8_t *p, size_t n) {
	const int W = g->w, H = g->h, TW = W / GFX_TILE, TH = H / GFX_TILE;
	size_t i = 0;
	while (i < n) {
		uint8_t op = p[i];
		size_t left = n - i;
		switch (op) {
		case GFX_OP_FILL: {
			if (left < 6) goto bad;
			int tx = p[i + 1], ty = p[i + 2], tw = p[i + 3], th = p[i + 4];
			uint8_t c = p[i + 5];
			if (tx + tw > TW || ty + th > TH) goto bad;
			for (int r = 0; r < th * GFX_TILE; ++r)
				memset(g->fb + (size_t)(ty * GFX_TILE + r) * W + tx * GFX_TILE, c, (size_t)tw * GFX_TILE);
			i += 6; break;
		}
		case GFX_OP_COPY: {
			if (left < 13) goto bad;
			int sx = rd16(p + i + 1), sy = rd16(p + i + 3), dx = rd16(p + i + 5), dy = rd16(p + i + 7);
			int w = rd16(p + i + 9), h = rd16(p + i + 11);
			if (sx + w > W || dx + w > W || sy + h > H || dy + h > H) goto bad;
			if (dy > sy) { for (int r = h - 1; r >= 0; --r) memmove(g->fb + (size_t)(dy + r) * W + dx, g->fb + (size_t)(sy + r) * W + sx, (size_t)w); }
			else         { for (int r = 0; r < h; ++r)      memmove(g->fb + (size_t)(dy + r) * W + dx, g->fb + (size_t)(sy + r) * W + sx, (size_t)w); }
			i += 13; break;
		}
		case GFX_OP_MONO: {
			if (left < 37) goto bad;
			int tx = p[i + 1], ty = p[i + 2];
			if (tx >= TW || ty >= TH) goto bad;
			uint8_t fg = p[i + 3], bg = p[i + 4];
			const uint8_t *m = p + i + 5;
			for (int r = 0; r < GFX_TILE; ++r) {
				uint16_t bits = rd16(m + r * 2);
				uint8_t *d = g->fb + (size_t)(ty * GFX_TILE + r) * W + tx * GFX_TILE;
				for (int x = 0; x < GFX_TILE; ++x) d[x] = (bits >> x) & 1 ? fg : bg;
			}
			cache_insert(g, tx, ty);
			i += 37; break;
		}
		case GFX_OP_RAW: {
			if (left < 3 + GFX_TILE_BYTES) goto bad;
			int tx = p[i + 1], ty = p[i + 2];
			if (tx >= TW || ty >= TH) goto bad;
			for (int r = 0; r < GFX_TILE; ++r)
				memcpy(g->fb + (size_t)(ty * GFX_TILE + r) * W + tx * GFX_TILE, p + i + 3 + r * GFX_TILE, GFX_TILE);
			cache_insert(g, tx, ty);
			i += 3 + GFX_TILE_BYTES; break;
		}
		case GFX_OP_CACHED: {
			if (left < 5) goto bad;
			int tx = p[i + 1], ty = p[i + 2], slot = rd16(p + i + 3);
			if (tx >= TW || ty >= TH || slot >= g->cache_tiles) goto bad;
			const uint8_t *s = g->cache + (size_t)slot * GFX_TILE_BYTES;
			for (int r = 0; r < GFX_TILE; ++r)
				memcpy(g->fb + (size_t)(ty * GFX_TILE + r) * W + tx * GFX_TILE, s + r * GFX_TILE, GFX_TILE);
			i += 5; break;
		}
		case GFX_OP_BM_DEF: {
			if (left < 7) goto bad;
			unsigned id = rd16(p + i + 1), bw = rd16(p + i + 3), bh = rd16(p + i + 5);
			size_t bytes = (size_t)bw * bh;
			if (left < 7 + bytes) goto bad;
			if (id < GFX_MAX_BM && bw && bh) {
				uint32_t off;
				if (g->bm[id].valid && (size_t)g->bm[id].w * g->bm[id].h == bytes) off = g->bm[id].off;
				else if (g->bm_arena && g->bm_used + bytes <= g->bm_size) { off = g->bm_used; g->bm_used += (uint32_t)bytes; }
				else { g->errors++; i += 7 + bytes; break; }
				memcpy(g->bm_arena + off, p + i + 7, bytes);
				g->bm[id].off = off; g->bm[id].w = (uint16_t)bw; g->bm[id].h = (uint16_t)bh; g->bm[id].valid = 1;
			} else g->errors++;
			i += 7 + bytes; break;
		}
		case GFX_OP_BM_BLIT: {
			if (left < 7) goto bad;
			unsigned id = rd16(p + i + 1);
			int x = (int16_t)rd16(p + i + 3), y = (int16_t)rd16(p + i + 5);
			if (id < GFX_MAX_BM && g->bm[id].valid) {
				int bw = g->bm[id].w, bh = g->bm[id].h;
				const uint8_t *src = g->bm_arena + g->bm[id].off;
				for (int r = 0; r < bh; ++r) {
					int yy = y + r;
					if (yy < 0 || yy >= H) continue;
					int x0 = x < 0 ? -x : 0, x1 = x + bw > W ? W - x : bw;
					if (x1 > x0) memcpy(g->fb + (size_t)yy * W + x + x0, src + (size_t)r * bw + x0, (size_t)(x1 - x0));
				}
			} else g->errors++;
			i += 7; break;
		}
		case GFX_OP_PRESENT:
			if (left < 2) goto bad;
			g->ack_seq = p[i + 1];
			g->ack_flag = 1;
			g->frames++;
			i += 2; break;
		default:
			goto bad;
		}
	}
	return;
bad:
	g->errors++;
}

static void execute(gfx_dec_t *g, const gfx_lane_t *l) {
	switch (l->ptype) {
	case GFX_PKT_OPS:
		run_ops(g, l->payload, (size_t)l->plen);
		break;
	case GFX_PKT_OPS_LZ4: {
		if (l->plen < 2) { g->errors++; break; }
		size_t rawlen = rd16(l->payload);
		if (rawlen > (size_t)g->raw_max) { g->errors++; break; }
		long got = lz4m_decompress(l->payload + 2, (size_t)l->plen - 2, g->raw, (size_t)g->raw_max);
		if (got < 0 || (size_t)got != rawlen) { g->errors++; break; }
		run_ops(g, g->raw, rawlen);
		break;
	}
	case GFX_PKT_MODE_TEXT:
		g->leave = 1;
		break;
	}
}

static void applied(gfx_dec_t *g, uint16_t seq) {
	uint8_t next = (uint8_t)((g->ack_head + 1) % 32);
	if (next == g->ack_tail) return;                  // queue full: drop (host times out and recovers)
	g->ackq[g->ack_head] = seq;
	g->ack_head = next;
}

// Run every held packet whose turn has come.
static void drain(gfx_dec_t *g) {
	for (int again = 1; again;) {
		again = 0;
		for (int i = 0; i < g->nlanes; ++i) {
			gfx_lane_t *l = &g->lane[i];
			if (l->pending && l->pseq == g->next_seq) {
				execute(g, l);
				applied(g, l->pseq);
				g->next_seq++;
				l->pending = 0;
				again = 1;
			}
		}
	}
}

static void packet_done(gfx_dec_t *g, gfx_lane_t *l) {
	if (!l->has_seq) { execute(g, l); return; }       // single-link mode: execute at once
	if (l->pseq == g->next_seq) {
		execute(g, l);
		applied(g, l->pseq);
		g->next_seq++;
		drain(g);
	} else if ((int16_t)(uint16_t)(l->pseq - g->next_seq) < 0) {
		g->errors++;                                   // stale / duplicate
	} else {
		l->pending = 1;                                // early: wait for the missing predecessor
	}
}

static void lane_byte(gfx_dec_t *g, gfx_lane_t *l, uint8_t b) {
	switch (l->st) {
	case S_SYNC:
		if (b == GFX_SYNC) { l->st = S_HDR; l->hdr_have = 0; l->hdr_need = 0; }
		break;
	case S_HDR:
		l->hdr[l->hdr_have++] = b;
		if (l->hdr_have == 1) {
			uint8_t base = b & (uint8_t)~GFX_SEQ_FLAG;
			if (base != GFX_PKT_OPS && base != GFX_PKT_OPS_LZ4 && base != GFX_PKT_MODE_TEXT) { g->errors++; l->st = S_SYNC; break; }
			l->hdr_need = (b & GFX_SEQ_FLAG) ? 6 : 4;
			break;
		}
		if (l->hdr_have < l->hdr_need) break;
		{
			uint8_t type = l->hdr[0], lo = l->hdr[1], hi = l->hdr[2];
			int has_seq = (type & GFX_SEQ_FLAG) != 0;
			uint8_t slo = has_seq ? l->hdr[3] : 0, shi = has_seq ? l->hdr[4] : 0;
			uint8_t chk = l->hdr[l->hdr_need - 1];
			int len = lo | (hi << 8);
			uint8_t base = type & (uint8_t)~GFX_SEQ_FLAG;
			int ok = chk == gfx_hdr_chk_seq(type, lo, hi, slo, shi) && len <= l->cap &&
			         (base != GFX_PKT_MODE_TEXT || len == 0);
			if (!ok) { g->errors++; l->st = S_SYNC; break; }
			l->ptype = base; l->has_seq = (uint8_t)has_seq; l->pseq = (uint16_t)(slo | (shi << 8));
			l->plen = len; l->phave = 0;
			if (len == 0) { l->st = S_SYNC; packet_done(g, l); }
			else l->st = S_PAYLOAD;
		}
		break;
	case S_PAYLOAD:
		l->payload[l->phave++] = b;
		if (l->phave == l->plen) { l->st = S_SYNC; packet_done(g, l); }
		break;
	}
}

size_t gfx_dec_feed_bytes_lane(gfx_dec_t *g, int lane, const uint8_t *buf, size_t n) {
	if (lane < 0 || lane >= g->nlanes) return 0;
	gfx_lane_t *l = &g->lane[lane];
	size_t i = 0;
	while (i < n && !l->pending) {
		if (l->st == S_PAYLOAD) {                      // fast path: bulk copy
			size_t take = (size_t)(l->plen - l->phave);
			if (take > n - i) take = n - i;
			memcpy(l->payload + l->phave, buf + i, take);
			l->phave += (int)take; i += take;
			if (l->phave == l->plen) { l->st = S_SYNC; packet_done(g, l); }
		} else lane_byte(g, l, buf[i++]);
	}
	return i;
}

int gfx_dec_lane_blocked(const gfx_dec_t *g, int lane) {
	return lane >= 0 && lane < g->nlanes && g->lane[lane].pending;
}

int gfx_dec_pop_pkt_ack(gfx_dec_t *g, uint16_t *seq) {
	if (g->ack_tail == g->ack_head) return 0;
	*seq = g->ackq[g->ack_tail];
	g->ack_tail = (uint8_t)((g->ack_tail + 1) % 32);
	return 1;
}

void gfx_dec_feed(gfx_dec_t *g, uint8_t b) { (void)gfx_dec_feed_bytes_lane(g, 0, &b, 1); }

void gfx_dec_feed_bytes(gfx_dec_t *g, const uint8_t *buf, size_t n) {
	while (n) { size_t c = gfx_dec_feed_bytes_lane(g, 0, buf, n); if (!c) break; buf += c; n -= c; }
}
