// term.h - tiny text terminal model (no hardware dependencies, host-testable).
//
// A grid of character cells stored as a ring of rows, so scrolling is just
// moving `top` and blanking one row (no memmove while the display is reading).
//
// Control bytes understood by term_feed():
//   0x0C  FF   clear screen, cursor home
//   0x01  SOH  cursor home (no clear)  -- for flicker-free redraws
//   0x0B  VT   erase from cursor to end of screen
//   0x0A  LF   column 0 + next line (scrolls at the bottom)
//   0x0D  CR   column 0
//   0x08  BS   cursor left
//   0x09  TAB  next multiple of 8
//   0x05  ENQ  ask for identification (term_feed returns 1)
//   32..126    printable, drawn with deferred auto-wrap (like a VT100)
//   >=0xC0     UTF-8 lead byte -> '?', 0x80..0xBF continuation bytes dropped
#ifndef USBDISPLAY_TERM_H
#define USBDISPLAY_TERM_H

#include <stdint.h>
#include <stdbool.h>

#ifndef TERM_COLS
#define TERM_COLS 80
#endif
#ifndef TERM_ROWS
#define TERM_ROWS 30
#endif

typedef struct {
	char cells[TERM_ROWS * TERM_COLS];
	volatile unsigned top;   // physical row shown at screen row 0
	volatile unsigned row;   // cursor row (screen coordinates)
	volatile unsigned col;   // cursor column
	bool pending_wrap;       // last column was written; wrap on next printable
} term_t;

void term_init(term_t *t);

// Feed one received byte. Returns 1 if the host asked for identification.
int term_feed(term_t *t, uint8_t byte);

// Pointer to the TERM_COLS characters of screen row r (0 = top).
static inline const char *term_row(const term_t *t, unsigned r) {
	return &t->cells[((t->top + r) % TERM_ROWS) * TERM_COLS];
}

#endif
