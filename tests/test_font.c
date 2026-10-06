// Host test for the 8x16 font tables (pico/font_8x16_*.h).
//
//   gcc -Wall -Ipico -DTERM_COLS=80 -DTERM_ROWS=30 tests/test_font.c pico/term.c -o tf && ./tf [frame.pbm]
//
// Checks table layout and bit order (bit 0 = leftmost pixel, like PicoDVI's font_8x8.h),
// then composes a full 640x480 frame from the terminal model the same way
// prepare_scanline() in main.c does and optionally writes it as a PBM image.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "font_8x16_dejavu.h"
#include "font_8x16_terminus.h"
#include "term.h"

#define N 95
#define H 16

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static unsigned glyph_row(const uint8_t *font, int ch, int row) { return font[row * N + (ch - 32)]; }

static int ink(const uint8_t *font, int ch) {
	int n = 0;
	for (int r = 0; r < H; ++r) n += __builtin_popcount(glyph_row(font, ch, r));
	return n;
}

// Horizontal centre of mass * 100 (column 0 = leftmost pixel).
static int com_x(const uint8_t *font, int ch) {
	int sum = 0, n = 0;
	for (int r = 0; r < H; ++r)
		for (int x = 0; x < 8; ++x)
			if (glyph_row(font, ch, r) & (1u << x)) { sum += x; n++; }
	return n ? sum * 100 / n : -1;
}

static void check_font(const char *name, const uint8_t *font, size_t size) {
	CHECK(size == (size_t)N * H, "%s: table is %zu bytes, expected %d", name, size, N * H);
	CHECK(ink(font, ' ') == 0, "%s: space is not blank", name);
	for (int ch = 33; ch < 127; ++ch)
		CHECK(ink(font, ch) > 0, "%s: glyph '%c' is empty", name, ch);
	// Bit order: letters whose stem is on one side. 'b' 'p' 'L' have it on the left,
	// 'd' 'q' on the right. (Bracket pairs are not used: hinting positions them freely.)
	CHECK(com_x(font, 'b') < com_x(font, 'd'), "%s: 'b' / 'd' mirrored (bit order)", name);
	CHECK(com_x(font, 'p') < com_x(font, 'q'), "%s: 'p' / 'q' mirrored (bit order)", name);
	CHECK(com_x(font, '[') < com_x(font, ']'), "%s: '[' / ']' mirrored (bit order)", name);
	CHECK(com_x(font, 'L') < 350, "%s: 'L' stem not on the left (bit order)", name);
	// The first and the last cell row are inside the cell: descenders must exist for g/p/y.
	for (const char *p = "gpyj"; *p; ++p) {
		int low = 0;
		for (int r = 13; r < H; ++r) low += __builtin_popcount(glyph_row(font, *p, r));
		CHECK(low > 0, "%s: '%c' has no descender", name, *p);
	}
	// Different letters must look different (catches a table that is all one glyph).
	CHECK(memcmp(font + ('i' - 32), font + ('l' - 32), 1) || ink(font, 'i') != ink(font, 'l'), "%s: i == l", name);
}

static void compose(const uint8_t *font, const term_t *t, uint8_t *frame /* 480 rows x 80 bytes */) {
	for (unsigned y = 0; y < 480; ++y) {
		unsigned r = y / H, fy = y % H;
		const char *row = term_row(t, r);
		for (unsigned i = 0; i < TERM_COLS; ++i)
			frame[y * 80 + i] = font[fy * N + ((uint8_t)row[i] - 32)];
		if (r == t->row && (y % H) >= H - 2 && t->col < TERM_COLS)
			frame[y * 80 + t->col] ^= 0xFF;   // cursor, as in main.c
	}
}

static void write_pbm(const char *path, const uint8_t *frame) {
	FILE *f = fopen(path, "wb");
	if (!f) { perror(path); failures++; return; }
	fprintf(f, "P4\n640 480\n");
	for (int y = 0; y < 480; ++y)
		for (int b = 0; b < 80; ++b) {
			uint8_t in = frame[y * 80 + b], out = 0;
			for (int x = 0; x < 8; ++x)          // PBM wants the leftmost pixel in the MSB
				if (in & (1u << x)) out |= 0x80 >> x;
			fputc(out, f);
		}
	fclose(f);
}

int main(int argc, char **argv) {
	check_font("dejavu", font_8x16_dejavu, sizeof font_8x16_dejavu);
	check_font("terminus", font_8x16_terminus, sizeof font_8x16_terminus);

	static term_t t;
	term_init(&t);
	const char *demo =
		"usbdisplay ready\n80x30 chars, 640x480 1bpp DVI\n\n"
		"The quick brown fox jumps over the lazy dog\n"
		"0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz\n"
		"PS C:\\Users\\wm> dir | select -First 3 {}[]()<>/|~`'\"@#$%^&*_-+=\n"
		"Il1| O0o B8 rn m g9q ;:,. wm@pico:~$ ls -la /dev/ttyACM0\n"
		">>> hello from the new font";
	for (const char *p = demo; *p; ++p) term_feed(&t, (uint8_t)*p);

	static uint8_t frame[480 * 80];
	compose(font_8x16_dejavu, &t, frame);
	if (argc > 1) write_pbm(argv[1], frame);
	if (argc > 2) { compose(font_8x16_terminus, &t, frame); write_pbm(argv[2], frame); }

	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("font tests passed\n");
	return 0;
}
