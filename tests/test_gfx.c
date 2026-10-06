// Round-trip tests: encoder -> (random chunking) -> decoder, compared against the source frame.
//   gcc -O1 -g -fsanitize=address,undefined -Icommon -Ilepotato/gfx -Ipico tests/test_gfx.c lepotato/gfx/gfx_enc.c pico/gfx_dec.c -o t_gfx && ./t_gfx
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gfx_enc.h"
#include "gfx_dec.h"
#include "lz4mini.h"

#define W 640
#define H 480
static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct {
	gfx_dec_t d;
	uint8_t *fb, *cache, *bm, *payload, *raw;
	size_t sent; int acks; int last_ack;
} sim_t;

static int emit(void *u, const uint8_t *data, size_t n) {
	sim_t *s = u;
	s->sent += n;
	size_t i = 0;
	while (i < n) {                                  // feed in odd-sized chunks
		size_t c = 1 + (size_t)(rand() % 97);
		if (c > n - i) c = n - i;
		if (rand() & 1) gfx_dec_feed_bytes(&s->d, data + i, c);
		else for (size_t k = 0; k < c; ++k) gfx_dec_feed(&s->d, data[i + k]);
		i += c;
		if (s->d.ack_flag) { s->d.ack_flag = 0; s->acks++; s->last_ack = s->d.ack_seq; }
	}
	return 0;
}

static void sim_init(sim_t *s, int cache_tiles, int raw_max) {
	memset(s, 0, sizeof *s);
	s->fb = malloc(W * H); s->cache = malloc((size_t)(cache_tiles ? cache_tiles : 1) * 256);
	s->bm = malloc(65536); s->payload = malloc(raw_max + 2); s->raw = malloc(raw_max);
	gfx_dec_init(&s->d, s->fb, W, H, s->cache, cache_tiles, s->bm, 65536, s->payload, s->raw, raw_max);
}
static void sim_free(sim_t *s) { free(s->fb); free(s->cache); free(s->bm); free(s->payload); free(s->raw); }

static int same(const sim_t *s, const uint8_t *cur) { return !memcmp(s->fb, cur, W * H); }

// ---- synthetic content ----
static void glyph(uint8_t *fr, int x, int y, unsigned ch, uint8_t fg, uint8_t bg) {
	unsigned h = ch * 2654435761u;
	for (int r = 0; r < 16; ++r) {
		h = h * 1103515245u + 12345u;
		unsigned bits = (h >> 16) & 0xFF;
		for (int c = 0; c < 8; ++c) fr[(y + r) * W + x + c] = (bits >> c) & 1 ? fg : bg;
	}
}
static void text_screen(uint8_t *fr, int first_line, int lines) {
	memset(fr, 0x03, W * H);
	for (int l = 0; l < lines; ++l)
		for (int c = 0; c < 78; ++c) {
			unsigned seed = (unsigned)(first_line + l) * 7919u + (unsigned)c * 104729u;
			if ((seed >> 4) % 5 == 0) continue;               // gaps
			glyph(fr, c * 8, l * 16, 33 + (seed >> 8) % 90, 0xFF, 0x03);
		}
}

static void test_lz4(void) {
	uint8_t *a = malloc(40000), *c = malloc(41000), *d = malloc(40000);
	for (int round = 0; round < 300; ++round) {
		size_t n = (size_t)(rand() % 20000);
		int kind = round % 4;
		for (size_t i = 0; i < n; ++i)
			a[i] = kind == 0 ? (uint8_t)rand() : kind == 1 ? 7 : kind == 2 ? (uint8_t)((i / 3) % 11) : (uint8_t)(rand() % 3);
		size_t cs = lz4m_compress(a, n, c, 41000);
		CHECK(cs > 0, "compress failed n=%zu", n);
		long ds = lz4m_decompress(c, cs, d, 40000);
		CHECK(ds == (long)n && !memcmp(a, d, n), "lz4 roundtrip n=%zu kind=%d", n, kind);
	}
	for (int round = 0; round < 3000; ++round) {            // hostile input must never crash
		size_t n = (size_t)(rand() % 300);
		for (size_t i = 0; i < n; ++i) c[i] = (uint8_t)rand();
		(void)lz4m_decompress(c, n, d, 1000);
	}
	free(a); free(c); free(d);
	printf("lz4 ok\n");
}

static void test_basic(void) {
	sim_t s; gfx_enc_t e; sim_init(&s, 256, 16384);
	CHECK(gfx_enc_init(&e, W, H, 16384, 256, emit, &s) == 0, "enc init");
	uint8_t *cur = malloc(W * H);

	memset(cur, 0xFF, W * H);                                // whole screen white
	long n = gfx_enc_frame(&e, cur);
	CHECK(n > 0 && n < 32, "white screen should be ~15 bytes, got %ld", n);
	CHECK(same(&s, cur), "white screen mismatch");
	CHECK(s.acks == 1 && s.last_ack == e.seq, "ack missing");

	n = gfx_enc_frame(&e, cur);                              // identical frame
	CHECK(n == 0, "identical frame must send nothing (got %ld)", n);

	text_screen(cur, 0, 30);                                 // text-like content
	n = gfx_enc_frame(&e, cur);
	CHECK(same(&s, cur), "text screen mismatch");
	printf("text screen: %ld bytes (raw 307200)\n", n);

	// scroll by one text line: only the new bottom line should need tile ops
	uint8_t *next = malloc(W * H);
	text_screen(next, 1, 30);
	size_t before = e.st_ops[GFX_OP_COPY];
	n = gfx_enc_frame(&e, next);
	CHECK(same(&s, next), "scroll mismatch");
	CHECK(e.st_ops[GFX_OP_COPY] == before + 1, "scroll was not detected");
	CHECK(n < 3000, "scroll frame too big: %ld", n);
	printf("one-line scroll: %ld bytes\n", n);

	// scroll the other way, by 3 lines
	text_screen(cur, -2, 30);
	n = gfx_enc_frame(&e, cur);
	CHECK(same(&s, cur), "reverse scroll mismatch");
	printf("3-line reverse scroll: %ld bytes\n", n);

	// back to a flat colour, then a progress bar that grows
	memset(cur, 0x1C, W * H);
	gfx_enc_frame(&e, cur);
	CHECK(same(&s, cur), "flat mismatch");
	for (int i = 0; i < 40; ++i) {
		for (int y = 400; y < 420; ++y) memset(cur + y * W + 20, 0xE0, 5 + i * 14);
		gfx_enc_frame(&e, cur);
		CHECK(same(&s, cur), "progress bar mismatch at %d", i);
	}
	CHECK(s.d.errors == 0, "decoder errors: %u", s.d.errors);
	gfx_enc_free(&e); sim_free(&s); free(cur); free(next);
	printf("basic ok\n");
}

static void test_cache(int cache_tiles) {
	sim_t s; gfx_enc_t e; sim_init(&s, cache_tiles, 8192);
	CHECK(gfx_enc_init(&e, W, H, 8192, cache_tiles, emit, &s) == 0, "enc init");
	uint8_t *cur = malloc(W * H);
	srand(7);
	// a few multi-colour "icons" placed at many positions, plus unique noise tiles that churn the ring
	uint8_t icons[8][256];
	for (int i = 0; i < 8; ++i) for (int k = 0; k < 256; ++k) icons[i][k] = (uint8_t)(rand() % 200 + 1);
	size_t cached_before = 0;
	for (int frame = 0; frame < 120; ++frame) {
		memset(cur, 0x10, W * H);
		for (int k = 0; k < 60; ++k) {
			int tx = rand() % 40, ty = rand() % 30;
			uint8_t t[256];
			if (rand() % 3 == 0) for (int j = 0; j < 256; ++j) t[j] = (uint8_t)(rand() % 250 + 1);
			else memcpy(t, icons[rand() % 8], 256);
			for (int r = 0; r < 16; ++r) memcpy(cur + (ty * 16 + r) * W + tx * 16, t + r * 16, 16);
		}
		gfx_enc_frame(&e, cur);
		CHECK(same(&s, cur), "cache=%d frame %d mismatch", cache_tiles, frame);
		if (frame == 1) cached_before = e.st_ops[GFX_OP_CACHED];
	}
	if (cache_tiles) CHECK(e.st_ops[GFX_OP_CACHED] > cached_before + 50, "cache never hit (%llu)", (unsigned long long)e.st_ops[GFX_OP_CACHED]);
	else CHECK(e.st_ops[GFX_OP_CACHED] == 0, "cached op used with cache disabled");
	CHECK(s.d.errors == 0, "decoder errors: %u", s.d.errors);
	printf("cache=%d ok: wire %zu B, CACHED ops %llu, RAW %llu, MONO %llu\n", cache_tiles, s.sent,
	       (unsigned long long)e.st_ops[GFX_OP_CACHED], (unsigned long long)e.st_ops[GFX_OP_RAW], (unsigned long long)e.st_ops[GFX_OP_MONO]);
	gfx_enc_free(&e); sim_free(&s); free(cur);
}

static void test_random_frames(void) {
	sim_t s; gfx_enc_t e; sim_init(&s, 64, 4096);
	CHECK(gfx_enc_init(&e, W, H, 4096, 64, emit, &s) == 0, "enc init");
	uint8_t *cur = malloc(W * H);
	srand(99);
	memset(cur, 0, W * H);
	for (int frame = 0; frame < 150; ++frame) {
		int kind = rand() % 5;
		if (kind == 0) memset(cur, (uint8_t)rand(), W * H);
		else if (kind == 1) for (int i = 0; i < W * H; ++i) cur[i] = (uint8_t)rand();
		else if (kind == 2) { int sh = (rand() % 100) - 50; if (sh) memmove(cur + (sh > 0 ? sh * W : 0), cur + (sh < 0 ? -sh * W : 0), (size_t)(H - abs(sh)) * W); }
		else for (int k = 0; k < 200; ++k) { int x = rand() % (W - 40), y = rand() % (H - 40), w = rand() % 40 + 1, h = rand() % 40 + 1; uint8_t c = (uint8_t)rand();
			for (int r = 0; r < h; ++r) memset(cur + (y + r) * W + x, c, w); }
		gfx_enc_frame(&e, cur);
		CHECK(same(&s, cur), "random frame %d (kind %d) mismatch", frame, kind);
	}
	CHECK(s.d.errors == 0, "decoder errors: %u", s.d.errors);
	gfx_enc_free(&e); sim_free(&s); free(cur);
	printf("random frames ok (small packets, wire %zu B)\n", s.sent);
}

static void test_bitmaps(void) {
	sim_t s; gfx_enc_t e; sim_init(&s, 0, 16384);
	CHECK(gfx_enc_init(&e, W, H, 16384, 0, emit, &s) == 0, "enc init");
	uint8_t bmp[40 * 30];
	for (int i = 0; i < 40 * 30; ++i) bmp[i] = (uint8_t)(i * 7 + 1);
	CHECK(gfx_enc_bitmap_define(&e, 3, 40, 30, bmp) == 0, "define");
	gfx_enc_bitmap_blit(&e, 3, 100, 100);
	gfx_enc_bitmap_blit(&e, 3, 400, 200);
	gfx_enc_bitmap_blit(&e, 3, -10, -5);                     // clipped top-left
	gfx_enc_bitmap_blit(&e, 3, 630, 470);                    // clipped bottom-right
	long n = gfx_enc_present(&e);
	CHECK(n > 0, "present");
	CHECK(same(&s, e.prev), "bitmap model mismatch");
	CHECK(s.d.errors == 0, "decoder errors: %u", s.d.errors);
	gfx_enc_free(&e); sim_free(&s);
	printf("bitmaps ok\n");
}

static void test_garbage_and_mode(void) {
	sim_t s; gfx_enc_t e; sim_init(&s, 128, 16384);
	CHECK(gfx_enc_init(&e, W, H, 16384, 128, emit, &s) == 0, "enc init");
	srand(5);
	uint8_t junk[4096];
	for (int round = 0; round < 200; ++round) {                // random bytes, including lots of 0xA5
		for (size_t i = 0; i < sizeof junk; ++i) junk[i] = (rand() % 8 == 0) ? GFX_SYNC : (uint8_t)rand();
		gfx_dec_feed_bytes(&s.d, junk, sizeof junk);
	}
	gfx_dec_reset(&s.d);                                     // host re-enters graphics mode
	gfx_enc_reset(&e);
	uint8_t *cur = malloc(W * H);
	text_screen(cur, 0, 30);
	gfx_enc_frame(&e, cur);
	CHECK(same(&s, cur), "no recovery after garbage");
	CHECK(!s.d.leave, "leave set too early");
	gfx_enc_mode_text(&e);
	CHECK(s.d.leave, "MODE_TEXT not seen");
	// a packet that fails its header checksum is rejected, the next good one still works
	uint8_t bad[8] = { GFX_SYNC, 1, 2, 0, 0x00, GFX_OP_PRESENT, 9, 0 };
	unsigned err0 = s.d.errors;
	gfx_dec_feed_bytes(&s.d, bad, sizeof bad);
	CHECK(s.d.errors > err0, "bad checksum accepted");
	free(cur); gfx_enc_free(&e); sim_free(&s);
	printf("garbage/mode ok\n");
}


/* ---------------------------------------------------------------- two links, out-of-order arrival */

typedef struct {
	gfx_dec_t d;
	uint8_t *fb, *cache, *payload0, *payload1, *raw;
	uint8_t *q[2]; size_t qlen[2], qcap[2];
	int packets, acked, deadlock;
	unsigned char pattern;                    // 0 = random lane, 1 = lane 1 only gets every 3rd packet
} lanesim_t;

static void lane_pump(lanesim_t *s, int drain_all) {
	int idle_rounds = 0;
	for (int iter = 0; iter < 100000; ++iter) {
		int progress = 0, queued = s->qlen[0] || s->qlen[1], first = rand() & 1;
		for (int k = 0; k < 2; ++k) {
			int l = first ^ k;
			if (!s->qlen[l]) continue;
			size_t chunk = 1 + (size_t)(rand() % 300);
			if (chunk > s->qlen[l]) chunk = s->qlen[l];
			size_t used = gfx_dec_feed_bytes_lane(&s->d, l, s->q[l], chunk);
			if (used) { memmove(s->q[l], s->q[l] + used, s->qlen[l] - used); s->qlen[l] -= used; progress = 1; }
			uint16_t a; while (gfx_dec_pop_pkt_ack(&s->d, &a)) s->acked++;
		}
		if (!queued) return;
		if (!progress) { if (++idle_rounds > 5) { s->deadlock = 1; return; } } else idle_rounds = 0;
		if (!drain_all && (rand() % 8) == 0) return;       // stop early: leave bytes in flight
	}
}

static int emit_lanes(void *u, const uint8_t *data, size_t n) {
	lanesim_t *s = u;
	int l = s->pattern == 1 ? ((s->packets % 3) == 2) : (rand() & 1);
	s->packets++;
	if (s->qlen[l] + n > s->qcap[l]) { s->qcap[l] = (s->qlen[l] + n) * 2; s->q[l] = realloc(s->q[l], s->qcap[l]); }
	memcpy(s->q[l] + s->qlen[l], data, n); s->qlen[l] += n;
	lane_pump(s, 0);
	return 0;
}

static void test_two_lanes(unsigned char pattern) {
	lanesim_t s; memset(&s, 0, sizeof s);
	s.pattern = pattern;
	s.fb = malloc(W * H); s.cache = malloc(256 * 256); s.payload0 = malloc(8194); s.payload1 = malloc(8194); s.raw = malloc(8192);
	gfx_dec_init(&s.d, s.fb, W, H, s.cache, 256, NULL, 0, s.payload0, s.raw, 8192);
	CHECK(gfx_dec_add_lane(&s.d, s.payload1, 8194) == 1, "add lane");
	gfx_enc_t e; CHECK(gfx_enc_init(&e, W, H, 8192, 256, emit_lanes, &s) == 0, "enc init");
	e.use_seq = 1;
	uint8_t *cur = malloc(W * H);
	srand(1234 + pattern);
	for (int frame = 0; frame < 60; ++frame) {
		if (frame % 5 == 0) text_screen(cur, frame, 30);
		else if (frame % 5 == 1) memset(cur, (uint8_t)rand(), W * H);
		else if (frame % 5 == 2) for (int i = 0; i < W * H; ++i) cur[i] = (uint8_t)(rand() % 5 ? 0x24 : rand());
		else text_screen(cur, frame * 3, 29);
		gfx_enc_frame(&e, cur);
		lane_pump(&s, 1);                               // everything that is in flight arrives
		CHECK(!s.deadlock, "deadlock at frame %d", frame);
		if (s.deadlock) break;
		CHECK(!memcmp(s.fb, cur, W * H), "two-lane frame %d mismatch (pattern %d)", frame, pattern);
	}
	CHECK(s.d.errors == 0, "decoder errors: %u", s.d.errors);
	CHECK(s.acked == s.packets, "acked %d of %d packets", s.acked, s.packets);
	printf("two lanes ok (pattern %d): %d packets, all executed in order\n", pattern, s.packets);
	gfx_enc_free(&e); free(s.fb); free(s.cache); free(s.payload0); free(s.payload1); free(s.raw); free(s.q[0]); free(s.q[1]); free(cur);
}

/* ---------------------------------------------------------------- byte budget (progressive refresh) */

static void test_budget(void) {
	sim_t s; gfx_enc_t e; sim_init(&s, 256, 16384);
	CHECK(gfx_enc_init(&e, W, H, 16384, 256, emit, &s) == 0, "enc init");
	e.budget_wire = 20000;
	uint8_t *a = malloc(W * H), *b = malloc(W * H);
	srand(3);
	for (int i = 0; i < W * H; ++i) a[i] = (uint8_t)rand();      // incompressible full-screen change
	size_t before = s.sent;
	long n = gfx_enc_frame(&e, a);
	CHECK(n > 0 && n < 20000 + 6000, "first frame must respect the budget, got %ld", n);
	CHECK(e.deferred > 0, "nothing was deferred");
	CHECK(!same(&s, a), "screen cannot be complete after one budgeted frame");
	for (int i = 0; i < W * H; ++i) b[i] = (uint8_t)rand();      // content changes again before the first one finished
	int frames = 1;
	while (frames < 100) {
		gfx_enc_frame(&e, b);
		frames++;
		if (!e.deferred) break;
	}
	CHECK(same(&s, b), "newest content must win after the deferred tiles are flushed");
	CHECK(frames > 5 && frames < 40, "unexpected number of frames: %d", frames);
	CHECK(s.sent - before < 3 * (size_t)W * H, "sent far more than needed: %zu", s.sent - before);
	CHECK(s.d.errors == 0, "decoder errors: %u", s.d.errors);
	printf("budget ok: noise screen needed %d frames, %zu bytes\n", frames, s.sent - before);
	free(a); free(b); gfx_enc_free(&e); sim_free(&s);
}

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static void bench(void) {
	sim_t s; gfx_enc_t e; sim_init(&s, 256, 16384);
	gfx_enc_init(&e, W, H, 16384, 256, emit, &s);
	uint8_t *cur = malloc(W * H);
	double t0 = now_ms(); long total = 0; int frames = 120;
	for (int i = 0; i < frames; ++i) {
		text_screen(cur, i / 2, 30);
		for (int y = 440; y < 460; ++y) memset(cur + y * W + 20, 0xE0, (i * 5) % 600);
		long n = gfx_enc_frame(&e, cur);
		total += n;
	}
	double ms = now_ms() - t0;
	printf("bench (incl. synthetic frame generation, decode and sanitizers): %.2f ms/frame, %.1f KB/frame avg\n", ms / frames, total / 1024.0 / frames);
	free(cur); gfx_enc_free(&e); sim_free(&s);
}

int main(void) {
	srand(1);
	test_lz4();
	test_basic();
	test_cache(256);
	test_cache(16);
	test_cache(0);
	test_random_frames();
	test_bitmaps();
	test_garbage_and_mode();
	test_two_lanes(0);
	test_two_lanes(1);
	test_budget();
	bench();
	if (failures) { printf("%d FAILURES\n", failures); return 1; }
	printf("ALL OK\n");
	return 0;
}
