#include <string.h>
#include "term.h"

static char *row_ptr(term_t *t, unsigned r) {
	return &t->cells[((t->top + r) % TERM_ROWS) * TERM_COLS];
}

static void newline(term_t *t) {
	if (t->row == TERM_ROWS - 1) {
		// Scroll: the old top row becomes the new (blank) bottom row.
		unsigned old_top = t->top;
		memset(&t->cells[old_top * TERM_COLS], ' ', TERM_COLS);
		t->top = (old_top + 1) % TERM_ROWS;
	} else {
		t->row++;
	}
}

static void put_printable(term_t *t, char c) {
	if (t->pending_wrap) {
		t->col = 0;
		t->pending_wrap = false;
		newline(t);
	}
	row_ptr(t, t->row)[t->col] = c;
	if (t->col == TERM_COLS - 1)
		t->pending_wrap = true;
	else
		t->col++;
}

void term_init(term_t *t) {
	memset(t->cells, ' ', sizeof t->cells);
	t->top = 0;
	t->row = 0;
	t->col = 0;
	t->pending_wrap = false;
}

int term_feed(term_t *t, uint8_t b) {
	switch (b) {
	case 0x0C: // clear + home
		memset(t->cells, ' ', sizeof t->cells);
		t->top = t->row = t->col = 0;
		t->pending_wrap = false;
		return 0;
	case 0x01: // home
		t->row = t->col = 0;
		t->pending_wrap = false;
		return 0;
	case 0x0B: { // erase to end of screen
		unsigned c = t->col;
		memset(&row_ptr(t, t->row)[c], ' ', TERM_COLS - c);
		for (unsigned r = t->row + 1; r < TERM_ROWS; ++r)
			memset(row_ptr(t, r), ' ', TERM_COLS);
		return 0;
	}
	case '\n':
		t->col = 0;
		t->pending_wrap = false;
		newline(t);
		return 0;
	case '\r':
		t->col = 0;
		t->pending_wrap = false;
		return 0;
	case '\b':
		if (t->col) t->col--;
		t->pending_wrap = false;
		return 0;
	case '\t': {
		unsigned next = (t->col + 8) & ~7u;
		t->pending_wrap = false;
		t->col = next >= TERM_COLS ? TERM_COLS - 1 : next;
		return 0;
	}
	case 0x05:
		return 1;
	default:
		break;
	}
	if (b >= 32 && b < 127)
		put_printable(t, (char)b);
	else if (b >= 0xC0)
		put_printable(t, '?');
	// everything else (other control bytes, 0x7F, UTF-8 continuation) is ignored
	return 0;
}
