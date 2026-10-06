// Host-side tests for term.c.   gcc -I../pico -DTERM_COLS=80 -DTERM_ROWS=30 test_term.c ../pico/term.c && ./a.out
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "term.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

static void feed(term_t *t, const char *s) { while (*s) term_feed(t, (uint8_t)*s++); }
static int row_is(term_t *t, unsigned r, const char *s) {
	char buf[TERM_COLS + 1];
	memset(buf, ' ', TERM_COLS);
	memcpy(buf, s, strlen(s));
	return memcmp(term_row(t, r), buf, TERM_COLS) == 0;
}

int main(void) {
	static term_t t;
	term_init(&t);

	feed(&t, "Hello\nWorld");
	CHECK(row_is(&t, 0, "Hello"));
	CHECK(row_is(&t, 1, "World"));
	CHECK(t.row == 1 && t.col == 5);

	// CRLF behaves like LF
	feed(&t, "\r\nX\r\nY");
	CHECK(row_is(&t, 2, "X") && row_is(&t, 3, "Y"));

	// CR overwrites
	feed(&t, "\rZ");
	CHECK(row_is(&t, 3, "Z"));

	// clear
	feed(&t, "\f");
	CHECK(row_is(&t, 0, "") && t.row == 0 && t.col == 0);

	// deferred wrap: exactly 80 chars then LF must NOT leave a blank line
	char line[TERM_COLS + 2];
	memset(line, 'a', TERM_COLS); line[TERM_COLS] = '\n'; line[TERM_COLS + 1] = 0;
	feed(&t, line);
	feed(&t, "b");
	CHECK(t.row == 1 && row_is(&t, 1, "b"));

	// 81st char wraps to next line
	term_init(&t);
	memset(line, 'c', TERM_COLS); line[TERM_COLS] = 'd'; line[TERM_COLS + 1] = 0;
	feed(&t, line);
	CHECK(t.row == 1 && row_is(&t, 1, "d"));

	// scrolling: write ROWS+5 numbered lines, check the last ROWS survive
	term_init(&t);
	for (int i = 0; i < TERM_ROWS + 5; ++i) {
		char b[16]; snprintf(b, sizeof b, "line%d\n", i);
		feed(&t, b);
	}
	// cursor sits on the (blank) bottom row after the final newline
	char want[16];
	snprintf(want, sizeof want, "line%d", TERM_ROWS + 4);
	CHECK(row_is(&t, TERM_ROWS - 2, want));
	snprintf(want, sizeof want, "line%d", 6);
	CHECK(row_is(&t, 0, want));
	CHECK(row_is(&t, TERM_ROWS - 1, ""));

	// home + erase-to-end (flicker-free redraw)
	term_init(&t);
	feed(&t, "one\ntwo\nthree");
	feed(&t, "\x01" "ONE\n" "\x0b");
	CHECK(row_is(&t, 0, "ONE"));
	CHECK(row_is(&t, 1, "") && row_is(&t, 2, ""));

	// tab, backspace
	term_init(&t);
	feed(&t, "a\tb");
	CHECK(t.col == 9);
	feed(&t, "\b\bX");
	CHECK(term_row(&t, 0)[7] == 'X');

	// UTF-8: 'a' + 0xC3 0xA5 (a-ring) -> "a?"
	term_init(&t);
	feed(&t, "a\xC3\xA5z");
	CHECK(row_is(&t, 0, "a?z"));

	// ENQ
	CHECK(term_feed(&t, 0x05) == 1);
	CHECK(term_feed(&t, 'q') == 0);

	// random fuzz must never go out of bounds (run under -fsanitize=address)
	srand(1);
	for (int i = 0; i < 2000000; ++i) term_feed(&t, (uint8_t)rand());
	CHECK(t.row < TERM_ROWS && t.col < TERM_COLS && t.top < TERM_ROWS);

	printf(fails ? "%d FAILED\n" : "all term tests passed\n", fails);
	return fails != 0;
}
