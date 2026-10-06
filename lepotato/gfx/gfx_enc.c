#include <stdlib.h>
#include <string.h>
#include "gfx_enc.h"
#include "lz4mini.h"

static inline uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static uint64_t hash64(const uint8_t *p, size_t n) {
	uint64_t h = 0x9E3779B97F4A7C15ull;
	size_t i = 0;
	for (; i + 8 <= n; i += 8) { h ^= rd64(p + i); h *= 0xFF51AFD7ED558CCDull; h ^= h >> 29; }
	for (; i < n; ++i) { h ^= p[i]; h *= 0x100000001B3ull; }
	return h ^ (h >> 32);
}

int gfx_enc_init(gfx_enc_t *e, int w, int h, int raw_max, int cache_tiles, gfx_emit_fn emit, void *user) {
	memset(e, 0, sizeof *e);
	if (w <= 0 || h <= 0 || w % GFX_TILE || h % GFX_TILE || w / GFX_TILE > 255 || h / GFX_TILE > 255) return -1;
	if (raw_max < 1024 || raw_max > 32768) return -1;
	if (cache_tiles < 0 || cache_tiles > 60000) return -1;
	e->w = w; e->h = h; e->tw = w / GFX_TILE; e->th = h / GFX_TILE;
	e->raw_max = raw_max; e->cache_tiles = cache_tiles;
	e->emit = emit; e->user = user; e->enable_scroll = 1;
	int tiles = e->tw * e->th;
	e->prev = calloc((size_t)w * h, 1);
	e->prev_rh = calloc((size_t)h, 8);
	e->cur_rh = calloc((size_t)h, 8);
	e->kind = calloc((size_t)tiles, 1);
	e->tcol = calloc((size_t)tiles, 1);
	int rt = 16; while (rt < 4 * h) rt <<= 1;
	e->rtab_mask = rt - 1;
	e->rtab = calloc((size_t)rt, sizeof(int16_t));
	e->rdup = calloc((size_t)h, 1);
	e->votes = calloc((size_t)(2 * h + 1), sizeof(int));
	e->ops = malloc((size_t)raw_max);
	e->lz = malloc((size_t)raw_max + 8);
	e->pkt = malloc((size_t)raw_max + 16);
	if (cache_tiles) {
		uint32_t ct = 16; while (ct < 4u * (uint32_t)cache_tiles) ct <<= 1;
		e->ctab_mask = ct - 1;
		e->ctile = calloc((size_t)cache_tiles, GFX_TILE_BYTES);
		e->chash = calloc((size_t)cache_tiles, 4);
		e->ctab = calloc(ct, 2);
		if (!e->ctile || !e->chash || !e->ctab) { gfx_enc_free(e); return -1; }
	}
	if (!e->prev || !e->prev_rh || !e->cur_rh || !e->kind || !e->tcol || !e->rtab || !e->rdup ||
	    !e->votes || !e->ops || !e->lz || !e->pkt) { gfx_enc_free(e); return -1; }
	gfx_enc_reset(e);
	return 0;
}

void gfx_enc_free(gfx_enc_t *e) {
	free(e->prev); free(e->prev_rh); free(e->cur_rh); free(e->kind); free(e->tcol);
	free(e->rtab); free(e->rdup); free(e->votes); free(e->ctile); free(e->chash); free(e->ctab);
	free(e->ops); free(e->lz); free(e->pkt);
	for (int i = 0; i < GFX_MAX_BM; ++i) free(e->bm[i]);
	memset(e, 0, sizeof *e);
}

void gfx_enc_reset(gfx_enc_t *e) {
	memset(e->prev, 0, (size_t)e->w * e->h);
	uint64_t z = hash64(e->prev, (size_t)e->w);          // a row of zeros
	for (int y = 0; y < e->h; ++y) e->prev_rh[y] = z;
	if (e->cache_tiles) {
		memset(e->ctile, 0, (size_t)e->cache_tiles * GFX_TILE_BYTES);
		memset(e->chash, 0, (size_t)e->cache_tiles * 4);
		memset(e->ctab, 0, ((size_t)e->ctab_mask + 1) * 2);
	}
	e->cnext = 0; e->ops_len = 0; e->err = 0; e->pseq = 0; e->seq = 0; e->deferred = 0; e->ratio = 1.0f;
	for (int i = 0; i < GFX_MAX_BM; ++i) { free(e->bm[i]); e->bm[i] = NULL; }
}

/* ---------------------------------------------------------------- packets */

static int send_packet(gfx_enc_t *e, uint8_t type, const uint8_t *payload, size_t n) {
	uint8_t *p = e->pkt;
	size_t h;
	p[0] = GFX_SYNC; p[2] = (uint8_t)n; p[3] = (uint8_t)(n >> 8);
	if (e->use_seq) {
		p[1] = type | GFX_SEQ_FLAG; p[4] = (uint8_t)e->pseq; p[5] = (uint8_t)(e->pseq >> 8);
		p[6] = gfx_hdr_chk_seq(p[1], p[2], p[3], p[4], p[5]);
		e->pseq++; h = GFX_HDR_LEN_SEQ;
	} else {
		p[1] = type; p[4] = gfx_hdr_chk(type, p[2], p[3]); h = GFX_HDR_LEN;
	}
	if (n) memcpy(p + h, payload, n);
	e->st_wire += n + h;
	if (e->emit(e->user, p, n + h) < 0) { e->err = 1; return -1; }
	return 0;
}

static int flush_ops(gfx_enc_t *e) {
	if (!e->ops_len || e->err) { e->ops_len = 0; return e->err ? -1 : 0; }
	size_t n = (size_t)e->ops_len;
	e->ops_len = 0;
	if (n >= 48) {
		size_t c = lz4m_compress(e->ops, n, e->lz + 2, n - 3);   // only worth it if it saves bytes
		if (c) {
			e->ratio = 0.8f * e->ratio + 0.2f * ((float)(c + 2 + GFX_HDR_LEN_SEQ) / (float)n);
			put16(e->lz, (unsigned)n);
			return send_packet(e, GFX_PKT_OPS_LZ4, e->lz, c + 2);
		}
	}
	e->ratio = 0.8f * e->ratio + 0.2f * 1.0f;
	return send_packet(e, GFX_PKT_OPS, e->ops, n);
}

static uint8_t *reserve(gfx_enc_t *e, int n, int op) {
	if (e->ops_len + n > e->raw_max) flush_ops(e);
	uint8_t *o = e->ops + e->ops_len;
	e->ops_len += n; e->raw_total += n;
	o[0] = (uint8_t)op;
	e->st_ops[op]++;
	return o;
}

int gfx_enc_mode_text(gfx_enc_t *e) {
	if (flush_ops(e) < 0) return -1;
	return send_packet(e, GFX_PKT_MODE_TEXT, NULL, 0);
}

long gfx_enc_present(gfx_enc_t *e) {
	uint64_t before = e->st_wire;
	uint8_t *o = reserve(e, 2, GFX_OP_PRESENT);
	o[1] = ++e->seq;
	e->st_frames++;
	if (flush_ops(e) < 0) return -1;
	return (long)(e->st_wire - before);
}

/* ---------------------------------------------------------------- tile cache */

static int cache_find(const gfx_enc_t *e, const uint8_t *t, uint64_t hh) {
	uint16_t s = e->ctab[(uint32_t)(hh >> 20) & e->ctab_mask];
	if (!s) return -1;
	--s;
	if (e->chash[s] == (uint32_t)hh && !memcmp(e->ctile + (size_t)s * GFX_TILE_BYTES, t, GFX_TILE_BYTES)) return s;
	return -1;
}

static void cache_insert(gfx_enc_t *e, const uint8_t *t, uint64_t hh) {
	int s = e->cnext;
	memcpy(e->ctile + (size_t)s * GFX_TILE_BYTES, t, GFX_TILE_BYTES);
	e->chash[s] = (uint32_t)hh;
	e->ctab[(uint32_t)(hh >> 20) & e->ctab_mask] = (uint16_t)(s + 1);
	if (++e->cnext >= e->cache_tiles) e->cnext = 0;
}

static void emit_tile(gfx_enc_t *e, const uint8_t *cur, int tx, int ty) {
	uint8_t t[GFX_TILE_BYTES];
	const uint8_t *src = cur + (size_t)ty * GFX_TILE * e->w + tx * GFX_TILE;
	for (int r = 0; r < GFX_TILE; ++r) memcpy(t + r * GFX_TILE, src + (size_t)r * e->w, GFX_TILE);
	uint64_t hh = e->cache_tiles ? hash64(t, GFX_TILE_BYTES) : 0;
	if (e->cache_tiles) {
		int slot = cache_find(e, t, hh);
		if (slot >= 0) {
			uint8_t *o = reserve(e, 5, GFX_OP_CACHED);
			o[1] = (uint8_t)tx; o[2] = (uint8_t)ty; put16(o + 3, (unsigned)slot);
			return;
		}
	}
	uint8_t c0 = t[0], c1 = 0;
	int n = 1;
	for (int i = 1; i < GFX_TILE_BYTES && n < 3; ++i) {
		if (t[i] == c0) continue;
		if (n == 1) { c1 = t[i]; n = 2; continue; }
		if (t[i] != c1) n = 3;
	}
	if (n == 2) {
		uint8_t *o = reserve(e, 37, GFX_OP_MONO);
		o[1] = (uint8_t)tx; o[2] = (uint8_t)ty; o[3] = c1; o[4] = c0;
		for (int r = 0; r < GFX_TILE; ++r) {
			unsigned m = 0;
			for (int x = 0; x < GFX_TILE; ++x) if (t[r * GFX_TILE + x] == c1) m |= 1u << x;
			put16(o + 5 + r * 2, m);
		}
	} else {
		uint8_t *o = reserve(e, 3 + GFX_TILE_BYTES, GFX_OP_RAW);
		o[1] = (uint8_t)tx; o[2] = (uint8_t)ty;
		memcpy(o + 3, t, GFX_TILE_BYTES);
	}
	if (e->cache_tiles) cache_insert(e, t, hh);
}

/* ---------------------------------------------------------------- scroll detection */

// Looks for a full-width vertical scroll. If found, emits a COPY and applies it to the model of the
// Pico screen (prev), so the normal tile diff only has to fix up the rows that scrolled in.
static void try_scroll(gfx_enc_t *e, const uint8_t *cur) {
	const int w = e->w, h = e->h;
	int changed = 0;
	for (int y = 0; y < h; ++y) changed += e->cur_rh[y] != e->prev_rh[y];
	if (changed < 3 * GFX_TILE) return;

	memset(e->rtab, 0, ((size_t)e->rtab_mask + 1) * sizeof(int16_t));
	memset(e->rdup, 0, (size_t)h);
	for (int y = 0; y < h; ++y) {                     // hash table of previous rows
		uint32_t i = (uint32_t)e->prev_rh[y] & (uint32_t)e->rtab_mask;
		for (;;) {
			int16_t v = e->rtab[i];
			if (!v) { e->rtab[i] = (int16_t)(y + 1); break; }
			if (e->prev_rh[v - 1] == e->prev_rh[y]) { e->rdup[v - 1] = 1; break; }
			i = (i + 1) & (uint32_t)e->rtab_mask;
		}
	}
	memset(e->votes, 0, (size_t)(2 * h + 1) * sizeof(int));
	for (int y = 0; y < h; ++y) {
		if (e->cur_rh[y] == e->prev_rh[y]) continue;
		uint32_t i = (uint32_t)e->cur_rh[y] & (uint32_t)e->rtab_mask;
		for (;;) {
			int16_t v = e->rtab[i];
			if (!v) break;
			if (e->prev_rh[v - 1] == e->cur_rh[y]) { if (!e->rdup[v - 1]) e->votes[(v - 1) - y + h]++; break; }
			i = (i + 1) & (uint32_t)e->rtab_mask;
		}
	}
	int best = 0, bestv = 0;
	for (int k = 0; k <= 2 * h; ++k) if (k != h && e->votes[k] > bestv) { bestv = e->votes[k]; best = k - h; }
	if (bestv < 24 || bestv * 4 < changed) return;

	int run0 = 0, runlen = 0, cs = -1;                 // longest run of rows with cur[y] == prev[y + best]
	for (int y = 0; y <= h; ++y) {
		int ok = 0, ys = y + best;
		if (y < h && ys >= 0 && ys < h && e->cur_rh[y] == e->prev_rh[ys] &&
		    !memcmp(cur + (size_t)y * w, e->prev + (size_t)ys * w, (size_t)w)) ok = 1;
		if (ok) { if (cs < 0) cs = y; }
		else if (cs >= 0) { if (y - cs > runlen) { runlen = y - cs; run0 = cs; } cs = -1; }
	}
	if (runlen < 2 * GFX_TILE) return;

	uint8_t *o = reserve(e, 13, GFX_OP_COPY);
	put16(o + 1, 0); put16(o + 3, (unsigned)(run0 + best));
	put16(o + 5, 0); put16(o + 7, (unsigned)run0);
	put16(o + 9, (unsigned)w); put16(o + 11, (unsigned)runlen);
	memmove(e->prev + (size_t)run0 * w, e->prev + (size_t)(run0 + best) * w, (size_t)runlen * w);
	memmove(e->prev_rh + run0, e->prev_rh + run0 + best, (size_t)runlen * sizeof(uint64_t));
}

/* ---------------------------------------------------------------- frame */

long gfx_enc_frame(gfx_enc_t *e, const uint8_t *cur) {
	if (e->err) return -1;
	const int w = e->w, tw = e->tw, th = e->th;
	uint64_t before = e->st_wire;
	for (int y = 0; y < e->h; ++y) e->cur_rh[y] = hash64(cur + (size_t)y * w, (size_t)w);

	uint64_t ops0 = 0;
	for (int i = 0; i < GFX_OP_COUNT; ++i) ops0 += e->st_ops[i];
	e->raw_total = 0;
	if (e->enable_scroll) try_scroll(e, cur);

	// classify tiles: 0 unchanged, 1 solid, 2 other
	for (int ty = 0; ty < th; ++ty)
		for (int tx = 0; tx < tw; ++tx) {
			const uint8_t *c = cur + (size_t)ty * GFX_TILE * w + tx * GFX_TILE;
			const uint8_t *p = e->prev + (size_t)ty * GFX_TILE * w + tx * GFX_TILE;
			int idx = ty * tw + tx, same = 1;
			for (int r = 0; r < GFX_TILE; ++r)
				if (memcmp(c + (size_t)r * w, p + (size_t)r * w, GFX_TILE)) { same = 0; break; }
			if (same) { e->kind[idx] = 0; continue; }
			uint8_t col = c[0];
			uint64_t pat = 0x0101010101010101ull * col;
			int solid = 1;
			for (int r = 0; r < GFX_TILE && solid; ++r) {
				const uint8_t *row = c + (size_t)r * w;
				if (rd64(row) != pat || rd64(row + 8) != pat) solid = 0;
			}
			e->kind[idx] = solid ? 1 : 2;
			e->tcol[idx] = col;
		}

	// merge solid tiles of one colour into rectangles
	for (int ty = 0; ty < th; ++ty)
		for (int tx = 0; tx < tw; ++tx) {
			int idx = ty * tw + tx;
			if (e->kind[idx] != 1) continue;
			uint8_t col = e->tcol[idx];
			int rw = 1, rh = 1;
			while (tx + rw < tw && e->kind[idx + rw] == 1 && e->tcol[idx + rw] == col) rw++;
			for (; ty + rh < th; ++rh) {
				int ok = 1;
				for (int k = 0; k < rw && ok; ++k) {
					int j = (ty + rh) * tw + tx + k;
					ok = e->kind[j] == 1 && e->tcol[j] == col;
				}
				if (!ok) break;
			}
			for (int r = 0; r < rh; ++r) for (int k = 0; k < rw; ++k) e->kind[(ty + r) * tw + tx + k] = 3;
			uint8_t *o = reserve(e, 6, GFX_OP_FILL);
			o[1] = (uint8_t)tx; o[2] = (uint8_t)ty; o[3] = (uint8_t)rw; o[4] = (uint8_t)rh; o[5] = col;
		}

	// Remaining tiles, in raster order, until the byte budget is used up. Tiles that do not fit stay
	// different from `prev`, so the next frame compares them against the then-current screen again.
	int budget_raw = 0;
	if (e->budget_wire > 0) {
		float r = e->ratio < 0.15f ? 0.15f : e->ratio;
		budget_raw = (int)((float)e->budget_wire / r);
		if (budget_raw < 2 * (3 + GFX_TILE_BYTES)) budget_raw = 2 * (3 + GFX_TILE_BYTES);
	}
	int deferred = 0;
	for (int ty = 0; ty < th; ++ty)
		for (int tx = 0; tx < tw; ++tx) {
			int idx = ty * tw + tx;
			if (e->kind[idx] != 2) continue;
			if (budget_raw && e->raw_total >= budget_raw) { e->kind[idx] = 4; deferred++; continue; }
			emit_tile(e, cur, tx, ty);
			e->kind[idx] = 5;
		}
	e->deferred = deferred;

	uint64_t ops1 = 0;
	for (int i = 0; i < GFX_OP_COUNT; ++i) ops1 += e->st_ops[i];
	if (ops1 == ops0 && !e->ops_len) return e->err ? -1 : 0;   // identical frame: send nothing
	if (!deferred) {
		memcpy(e->prev, cur, (size_t)w * e->h);
		uint64_t *t = e->prev_rh; e->prev_rh = e->cur_rh; e->cur_rh = t;
	} else {                                         // only the tiles that were sent become "shown"
		for (int ty = 0; ty < th; ++ty)
			for (int tx = 0; tx < tw; ++tx) {
				int k = e->kind[ty * tw + tx];
				if (k != 3 && k != 5) continue;
				for (int r = 0; r < GFX_TILE; ++r)
					memcpy(e->prev + (size_t)(ty * GFX_TILE + r) * w + tx * GFX_TILE,
					       cur + (size_t)(ty * GFX_TILE + r) * w + tx * GFX_TILE, GFX_TILE);
			}
		for (int y = 0; y < e->h; ++y) e->prev_rh[y] = hash64(e->prev + (size_t)y * w, (size_t)w);
	}
	if (gfx_enc_present(e) < 0) return -1;
	return (long)(e->st_wire - before);
}

/* ---------------------------------------------------------------- explicit bitmaps */

int gfx_enc_bitmap_define(gfx_enc_t *e, int id, int w, int h, const uint8_t *px) {
	if (id < 0 || id >= GFX_MAX_BM || w <= 0 || h <= 0 || w > 65535 || h > 65535) return -1;
	size_t bytes = (size_t)w * h;
	if (7 + bytes > (size_t)e->raw_max) return -1;
	uint8_t *copy = malloc(bytes);
	if (!copy) return -1;
	memcpy(copy, px, bytes);
	free(e->bm[id]);
	e->bm[id] = copy; e->bm_w[id] = (uint16_t)w; e->bm_h[id] = (uint16_t)h;
	uint8_t *o = reserve(e, (int)(7 + bytes), GFX_OP_BM_DEF);
	put16(o + 1, (unsigned)id); put16(o + 3, (unsigned)w); put16(o + 5, (unsigned)h);
	memcpy(o + 7, px, bytes);
	return 0;
}

int gfx_enc_bitmap_blit(gfx_enc_t *e, int id, int x, int y) {
	if (id < 0 || id >= GFX_MAX_BM || !e->bm[id] || x < -32768 || x > 32767 || y < -32768 || y > 32767) return -1;
	uint8_t *o = reserve(e, 7, GFX_OP_BM_BLIT);
	put16(o + 1, (unsigned)id); put16(o + 3, (unsigned)(x & 0xFFFF)); put16(o + 5, (unsigned)(y & 0xFFFF));
	int bw = e->bm_w[id], bh = e->bm_h[id];                 // keep the model of the Pico screen in step
	for (int r = 0; r < bh; ++r) {
		int yy = y + r;
		if (yy < 0 || yy >= e->h) continue;
		int x0 = x < 0 ? -x : 0, x1 = x + bw > e->w ? e->w - x : bw;
		if (x1 > x0) memcpy(e->prev + (size_t)yy * e->w + x + x0, e->bm[id] + (size_t)r * bw + x0, (size_t)(x1 - x0));
	}
	for (int yy = y < 0 ? 0 : y; yy < y + bh && yy < e->h; ++yy)
		e->prev_rh[yy] = hash64(e->prev + (size_t)yy * e->w, (size_t)e->w);
	return 0;
}
