// fake_pico - pretends to be the Pico on pseudo-terminals so gfxstream's serial path, handshake, flow control
// and two-link scheduling can be tested without hardware.
//   ./fake_pico [--uart] [--psram]    prints the USB pty path (and the UART pty path) on the first line(s);
//                                     runs until gfxstream leaves graphics mode; the last line is a summary
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include "gfx_dec.h"

static int open_pty(char *name, size_t n) {
	int m = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (m < 0 || grantpt(m) || unlockpt(m)) { perror("pty"); exit(1); }
	struct termios t; tcgetattr(m, &t); cfmakeraw(&t); tcsetattr(m, TCSANOW, &t);
	snprintf(name, n, "%s", ptsname(m));
	return m;
}

int main(int argc, char **argv) {
	int use_uart = 0, use_psram = 0;
	for (int i = 1; i < argc; ++i) { if (!strcmp(argv[i], "--uart")) use_uart = 1; if (!strcmp(argv[i], "--psram")) use_psram = 1; }
	char usb_name[128], uart_name[128] = "";
	int usb = open_pty(usb_name, sizeof usb_name), uart = use_uart ? open_pty(uart_name, sizeof uart_name) : -1;
	printf("%s\n", usb_name);
	if (use_uart) printf("%s\n", uart_name);
	fflush(stdout);

	enum { W = 640, H = 480, RAW = 16384, L1RAW = 8194 };
	const int cache_tiles = use_psram ? 16384 : 256;
	static uint8_t fb[W * H], cache[16384 * 256], payload[RAW + 2], payload1[L1RAW], raw[RAW];
	gfx_dec_t g; gfx_dec_init(&g, fb, W, H, cache, cache_tiles, NULL, 0, payload, raw, RAW);
	if (use_uart) gfx_dec_add_lane(&g, payload1, L1RAW);

	int gfx = 0; unsigned long bytes[2] = { 0, 0 };
	uint8_t stage[2][512]; size_t slen[2] = { 0, 0 }, soff[2] = { 0, 0 };
	char enq[256];
	snprintf(enq, sizeof enq, "USBDISPLAY 1 80x30 pins=10,12,14 clk=8 inv=1 gfx=640x480x8 cache=%d raw=%d bm=0%s%s\r\n", cache_tiles, RAW,
	         use_psram ? " psram=8388608" : "", use_uart ? " uart=2000000 ring=16384 l1raw=8194" : "");
	for (int idle = 0; idle < 100;) {            // ~10 s without any traffic -> give up
		struct pollfd pf[2] = { { usb, POLLIN, 0 }, { uart, POLLIN, 0 } };
		int pending = 0;
		for (int l = 0; l < 2; ++l) if (soff[l] < slen[l]) pending = 1;
		if (!pending && poll(pf, use_uart ? 2 : 1, 100) <= 0) { idle++; continue; }
		idle = 0;
		for (int l = 0; l < (use_uart ? 2 : 1); ++l) {
			int fd = l ? uart : usb;
			if (soff[l] >= slen[l]) {            // stage empty: read more (UART bytes before graphics mode are dropped)
				ssize_t n = read(fd, stage[l], sizeof stage[l]);
				if (n <= 0) continue;
				if (l == 1 && !gfx) continue;
				slen[l] = (size_t)n; soff[l] = 0; bytes[l] += (unsigned long)n;
			}
			while (soff[l] < slen[l]) {
				if (!gfx) {                      // text mode: only ENQ and the graphics-enter byte matter
					uint8_t b = stage[l][soff[l]++];
					if (b == 0x05) { if (write(usb, enq, strlen(enq)) < 0) perror("write"); }
					else if (b == GFX_ENTER_BYTE) { gfx_dec_reset(&g); gfx = 1; uint8_t r = GFX_READY_BYTE; if (write(usb, &r, 1) < 0) perror("write"); }
					continue;
				}
				size_t used = gfx_dec_feed_bytes_lane(&g, l, stage[l] + soff[l], slen[l] - soff[l]);
				soff[l] += used;
				if (!used) break;                // lane is holding an early packet: retry later
				if (g.ack_flag) { g.ack_flag = 0; uint8_t a[2] = { GFX_ACK_BYTE, g.ack_seq }; if (write(usb, a, 2) < 0) perror("write"); }
				uint16_t ps;
				while (gfx_dec_pop_pkt_ack(&g, &ps)) { uint8_t a[4] = { GFX_PKT_ACK_BYTE, (uint8_t)ps, (uint8_t)(ps >> 8), (uint8_t)(g.errors > 255 ? 255 : g.errors) }; if (write(usb, a, 4) < 0) perror("write"); }
				if (g.leave) { printf("frames=%u errors=%u usb=%lu uart=%lu\n", g.frames, g.errors, bytes[0], bytes[1]); return 0; }
			}
		}
	}
	printf("timeout frames=%u errors=%u\n", g.frames, g.errors);
	return 1;
}
