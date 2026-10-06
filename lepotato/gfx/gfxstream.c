// gfxstream - capture a screen (X11 / fbdev / synthetic test pattern), encode it with gfx_enc and
// stream it to the Pico display over USB serial. See ../../README.md.
//
//   gfxstream --source x11 --display :0
//   gfxstream --source test --null --frames 300      # benchmark the encoder, no hardware needed
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/fb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifdef HAVE_X11
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#endif
#include "gfx_enc.h"

#define MAX_INFLIGHT 2

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void sleep_ms(double ms) { if (ms <= 0) return; struct timespec t = { (time_t)(ms / 1000), (long)((ms - (long)(ms / 1000) * 1000) * 1e6) }; nanosleep(&t, NULL); }

/* ================================================================= frame sources */

typedef struct {                       // a captured frame in the source's native pixel format
	int sw, sh, stride, bpp;           // bpp 8 = already RGB332 at the output size
	const uint8_t *data;
	int roff, rlen, goff, glen, boff, blen;
} frame_t;

typedef struct {
	int (*grab)(void *ctx, frame_t *f);
	void (*close)(void *ctx);
	void *ctx;
} source_t;

static inline unsigned chan(uint32_t p, int off, int len, int bits) {
	unsigned v = (p >> off) & ((1u << len) - 1);
	return len >= bits ? v >> (len - bits) : v << (bits - len);
}

static const uint8_t bayer4[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

// 8 bit r,g,b -> RGB332 with an ordered 4x4 dither (the pattern depends only on x,y, so unchanged pixels stay unchanged).
static inline uint8_t dither332(unsigned r, unsigned g, unsigned b, int x, int y) {
	int t = bayer4[y & 3][x & 3];                       // 0..15
	int r2 = (int)r + ((t * 32) / 16 - 16), g2 = (int)g + ((t * 32) / 16 - 16), b2 = (int)b + ((t * 64) / 16 - 32);
	r2 = r2 < 0 ? 0 : r2 > 255 ? 255 : r2;
	g2 = g2 < 0 ? 0 : g2 > 255 ? 255 : g2;
	b2 = b2 < 0 ? 0 : b2 > 255 ? 255 : b2;
	return (uint8_t)(((r2 >> 5) << 5) | ((g2 >> 5) << 2) | (b2 >> 6));
}

// Nearest-neighbour scale + conversion to RGB332 (optionally dithered).
static void to332(const frame_t *f, uint8_t *out, int W, int H, int *xmap, int dither) {
	if (f->bpp == 8 && f->sw == W && f->sh == H) {
		for (int y = 0; y < H; ++y) memcpy(out + (size_t)y * W, f->data + (size_t)y * f->stride, (size_t)W);
		return;
	}
	for (int x = 0; x < W; ++x) xmap[x] = (int)((long)x * f->sw / W);
	for (int y = 0; y < H; ++y) {
		const uint8_t *row = f->data + (size_t)((long)y * f->sh / H) * f->stride;
		uint8_t *o = out + (size_t)y * W;
		if (f->bpp == 32) {
			const uint32_t *r = (const uint32_t *)row;
			if (dither) for (int x = 0; x < W; ++x) {
				uint32_t p = r[xmap[x]];
				o[x] = dither332(chan(p, f->roff, f->rlen, 8), chan(p, f->goff, f->glen, 8), chan(p, f->boff, f->blen, 8), x, y);
			} else for (int x = 0; x < W; ++x) {
				uint32_t p = r[xmap[x]];
				o[x] = (uint8_t)((chan(p, f->roff, f->rlen, 3) << 5) | (chan(p, f->goff, f->glen, 3) << 2) | chan(p, f->boff, f->blen, 2));
			}
		} else if (f->bpp == 16) {
			const uint16_t *r = (const uint16_t *)row;
			if (dither) for (int x = 0; x < W; ++x) {
				uint32_t p = r[xmap[x]];
				o[x] = dither332(chan(p, f->roff, f->rlen, 8), chan(p, f->goff, f->glen, 8), chan(p, f->boff, f->blen, 8), x, y);
			} else for (int x = 0; x < W; ++x) {
				uint32_t p = r[xmap[x]];
				o[x] = (uint8_t)((chan(p, f->roff, f->rlen, 3) << 5) | (chan(p, f->goff, f->glen, 3) << 2) | chan(p, f->boff, f->blen, 2));
			}
		} else if (f->bpp == 24) {
			for (int x = 0; x < W; ++x) {
				const uint8_t *q = row + xmap[x] * 3;
				uint32_t p = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16);
				o[x] = dither ? dither332(chan(p, f->roff, f->rlen, 8), chan(p, f->goff, f->glen, 8), chan(p, f->boff, f->blen, 8), x, y)
				              : (uint8_t)((chan(p, f->roff, f->rlen, 3) << 5) | (chan(p, f->goff, f->glen, 3) << 2) | chan(p, f->boff, f->blen, 2));
			}
		}
	}
}

/* ---- synthetic test pattern: window, scrolling "text", progress bar, blinking cursor, periodic clear ---- */

typedef struct { uint8_t *buf; int W, H; long n; } testsrc_t;

static void t_glyph(uint8_t *fr, int W, int x, int y, unsigned ch, uint8_t fg, uint8_t bg) {
	unsigned h = ch * 2654435761u;
	for (int r = 0; r < 16; ++r) {
		h = h * 1103515245u + 12345u;
		unsigned bits = (h >> 16) & 0xFF;
		for (int c = 0; c < 8; ++c) fr[(size_t)(y + r) * W + x + c] = (bits >> c) & 1 ? fg : bg;
	}
}

static int test_grab(void *ctx, frame_t *f) {
	testsrc_t *t = ctx;
	int W = t->W, H = t->H;
	long i = t->n++;
	long phase = i % 300;
	uint8_t *b = t->buf;
	if (phase >= 295) { memset(b, 0xFF, (size_t)W * H); goto out; }          // flash of white
	memset(b, 0x03, (size_t)W * H);
	for (int y = 40; y < H - 40; ++y) memset(b + (size_t)y * W + 40, 0xDB, (size_t)W - 80);   // window
	for (int y = 40; y < 56; ++y) memset(b + (size_t)y * W + 40, 0x1F, (size_t)W - 80);       // title bar
	long scroll = i / 6;
	int lines = (H - 80 - 16 - 40) / 16;
	for (int l = 0; l < lines; ++l)
		for (int c = 0; c < (W - 100) / 8; ++c) {
			unsigned seed = (unsigned)(scroll + l) * 7919u + (unsigned)c * 104729u;
			if ((seed >> 4) % 4 == 0) continue;
			t_glyph(b, W, 50 + c * 8, 60 + l * 16, 33 + (seed >> 8) % 90, 0x00, 0xDB);
		}
	int bar = (int)((phase * (W - 120)) / 295);
	for (int y = H - 70; y < H - 54; ++y) { memset(b + (size_t)y * W + 60, 0x49, (size_t)W - 120); memset(b + (size_t)y * W + 60, 0xE0, (size_t)bar); }
	if ((i / 15) & 1) for (int y = 60 + (lines - 1) * 16; y < 60 + lines * 16; ++y) memset(b + (size_t)y * W + 50, 0x00, 8);
out:
	f->sw = W; f->sh = H; f->stride = W; f->bpp = 8; f->data = b;
	return 0;
}
static void test_close(void *ctx) { testsrc_t *t = ctx; free(t->buf); free(t); }
static int open_test(source_t *s, int W, int H) {
	testsrc_t *t = calloc(1, sizeof *t);
	t->W = W; t->H = H; t->buf = malloc((size_t)W * H);
	s->grab = test_grab; s->close = test_close; s->ctx = t;
	return 0;
}

/* ---- noise: every frame ~25% of the tiles change to random pixels (worst case; for link / budget tests) ---- */

typedef struct { uint8_t *buf; int W, H; uint32_t rng; } noisesrc_t;
static uint32_t xr(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static int noise_grab(void *ctx, frame_t *f) {
	noisesrc_t *t = ctx;
	int tw = t->W / 16, th = t->H / 16;
	for (int k = 0; k < tw * th / 4; ++k) {
		int tx = (int)(xr(&t->rng) % (uint32_t)tw), ty = (int)(xr(&t->rng) % (uint32_t)th);
		for (int r = 0; r < 16; ++r) for (int c = 0; c < 16; ++c) t->buf[(size_t)(ty * 16 + r) * t->W + tx * 16 + c] = (uint8_t)xr(&t->rng);
	}
	f->sw = t->W; f->sh = t->H; f->stride = t->W; f->bpp = 8; f->data = t->buf;
	return 0;
}
static void noise_close(void *ctx) { noisesrc_t *t = ctx; free(t->buf); free(t); }
static int open_noise(source_t *s, int W, int H) {
	noisesrc_t *t = calloc(1, sizeof *t);
	t->W = W; t->H = H; t->buf = calloc((size_t)W * H, 1); t->rng = 2463534242u;
	s->grab = noise_grab; s->close = noise_close; s->ctx = t;
	return 0;
}

/* ---- fbdev ---- */

typedef struct { int fd; uint8_t *map; size_t len; struct fb_var_screeninfo v; struct fb_fix_screeninfo fx; } fbsrc_t;

static int fb_grab(void *ctx, frame_t *f) {
	fbsrc_t *s = ctx;
	if (ioctl(s->fd, FBIOGET_VSCREENINFO, &s->v) < 0) return -1;
	f->sw = (int)s->v.xres; f->sh = (int)s->v.yres; f->stride = (int)s->fx.line_length; f->bpp = (int)s->v.bits_per_pixel;
	f->data = s->map + (size_t)s->v.yoffset * s->fx.line_length;
	f->roff = (int)s->v.red.offset; f->rlen = (int)s->v.red.length;
	f->goff = (int)s->v.green.offset; f->glen = (int)s->v.green.length;
	f->boff = (int)s->v.blue.offset; f->blen = (int)s->v.blue.length;
	return 0;
}
static void fb_close(void *ctx) { fbsrc_t *s = ctx; munmap(s->map, s->len); close(s->fd); free(s); }
static int open_fb(source_t *src, const char *dev) {
	fbsrc_t *s = calloc(1, sizeof *s);
	s->fd = open(dev, O_RDONLY);
	if (s->fd < 0) { perror(dev); free(s); return -1; }
	if (ioctl(s->fd, FBIOGET_FSCREENINFO, &s->fx) < 0 || ioctl(s->fd, FBIOGET_VSCREENINFO, &s->v) < 0) { perror("fb ioctl"); free(s); return -1; }
	if (s->v.bits_per_pixel != 32 && s->v.bits_per_pixel != 16 && s->v.bits_per_pixel != 24) { fprintf(stderr, "fb: unsupported %u bpp\n", s->v.bits_per_pixel); free(s); return -1; }
	s->len = s->fx.smem_len;
	s->map = mmap(NULL, s->len, PROT_READ, MAP_SHARED, s->fd, 0);
	if (s->map == MAP_FAILED) { perror("mmap fb"); free(s); return -1; }
	src->grab = fb_grab; src->close = fb_close; src->ctx = s;
	return 0;
}

/* ---- X11 ---- */

#ifdef HAVE_X11
typedef struct { Display *d; Window root; XImage *img; XShmSegmentInfo shm; int w, h, use_shm; } x11src_t;

static int x11_fill(frame_t *f, XImage *img) {
	f->sw = img->width; f->sh = img->height; f->stride = img->bytes_per_line; f->bpp = img->bits_per_pixel; f->data = (const uint8_t *)img->data;
	unsigned long m[3] = { img->red_mask, img->green_mask, img->blue_mask };
	int off[3], len[3];
	for (int i = 0; i < 3; ++i) {
		if (!m[i]) return -1;
		off[i] = __builtin_ctzl(m[i]); len[i] = __builtin_popcountl(m[i]);
	}
	f->roff = off[0]; f->rlen = len[0]; f->goff = off[1]; f->glen = len[1]; f->boff = off[2]; f->blen = len[2];
	return 0;
}

static int x11_grab(void *ctx, frame_t *f) {
	x11src_t *s = ctx;
	if (s->use_shm) {
		if (!XShmGetImage(s->d, s->root, s->img, 0, 0, AllPlanes)) { fprintf(stderr, "XShmGetImage failed\n"); return -1; }
		return x11_fill(f, s->img);
	}
	if (s->img) XDestroyImage(s->img);
	s->img = XGetImage(s->d, s->root, 0, 0, (unsigned)s->w, (unsigned)s->h, AllPlanes, ZPixmap);
	if (!s->img) return -1;
	return x11_fill(f, s->img);
}
static void x11_close(void *ctx) {
	x11src_t *s = ctx;
	if (s->use_shm) { XShmDetach(s->d, &s->shm); shmdt(s->shm.shmaddr); }
	if (s->img) XDestroyImage(s->img);
	XCloseDisplay(s->d); free(s);
}
static int open_x11(source_t *src, const char *name) {
	Display *d = XOpenDisplay(name);
	if (!d) { fprintf(stderr, "gfxstream: cannot open X display '%s'\n", name ? name : "(default)"); return -1; }
	x11src_t *s = calloc(1, sizeof *s);
	s->d = d; s->root = DefaultRootWindow(d);
	XWindowAttributes a; XGetWindowAttributes(d, s->root, &a);
	s->w = a.width; s->h = a.height;
	if (XShmQueryExtension(d)) {
		s->img = XShmCreateImage(d, a.visual, (unsigned)a.depth, ZPixmap, NULL, &s->shm, (unsigned)s->w, (unsigned)s->h);
		if (s->img) {
			s->shm.shmid = shmget(IPC_PRIVATE, (size_t)s->img->bytes_per_line * s->img->height, IPC_CREAT | 0600);
			if (s->shm.shmid >= 0) {
				s->shm.shmaddr = s->img->data = shmat(s->shm.shmid, NULL, 0);
				s->shm.readOnly = False;
				if (s->shm.shmaddr != (void *)-1 && XShmAttach(d, &s->shm)) { XSync(d, False); shmctl(s->shm.shmid, IPC_RMID, NULL); s->use_shm = 1; }
			}
			if (!s->use_shm) { s->img->data = NULL; XDestroyImage(s->img); s->img = NULL; }
		}
	}
	fprintf(stderr, "gfxstream: X display %s %dx%d depth %d (%s)\n", name ? name : "(default)", s->w, s->h, a.depth, s->use_shm ? "XShm" : "XGetImage, slower");
	src->grab = x11_grab; src->close = x11_close; src->ctx = s;
	return 0;
}
static int probe_x11(const char *name) {
	Display *d = XOpenDisplay(name);
	if (!d) return 1;
	printf("%dx%d\n", DisplayWidth(d, DefaultScreen(d)), DisplayHeight(d, DefaultScreen(d)));
	XCloseDisplay(d);
	return 0;
}
#else
static int open_x11(source_t *src, const char *name) { (void)src; (void)name; fprintf(stderr, "gfxstream: built without X11 support (install libx11-dev libxext-dev, then run setup.sh)\n"); return -1; }
static int probe_x11(const char *name) { (void)name; fprintf(stderr, "built without X11 support\n"); return 2; }
#endif

/* ================================================================= links (USB serial + optional UART) */

typedef struct {
	int fd, is_uart;
	double rate;                         // bytes/s used for scheduling
	double finish_ms;                    // when everything queued on this link would be on the wire
	uint8_t *buf; size_t cap, len, off;  // output waiting for the kernel (multi-link mode)
	uint32_t inflight, window;           // UART: bytes sent but not yet executed by the Pico / allowed
	uint64_t sent;
} lane_t;

typedef struct {
	lane_t lane[2]; int n;               // lane[0] is always the USB serial port
	uint8_t acked;                       // last frame (PRESENT) seq acknowledged
	int ack_state; uint8_t ack_buf[3];
	int ready;                           // 0x08 seen
	uint8_t pk_errs; int pk_err_warned;
	struct { uint16_t seq; uint32_t bytes; int lane; } pk[256];
} link_t;

typedef struct { int w, h, cache, raw, bm, psram, uart, ring, l1raw; } caps_t;

static speed_t baud_const(long b) {
	switch (b) {
	case 115200: return B115200; case 230400: return B230400; case 460800: return B460800; case 500000: return B500000;
	case 576000: return B576000; case 921600: return B921600; case 1000000: return B1000000; case 1152000: return B1152000;
	case 1500000: return B1500000; case 2000000: return B2000000; case 2500000: return B2500000; case 3000000: return B3000000;
	case 3500000: return B3500000; case 4000000: return B4000000; default: return 0;
	}
}

static int open_serial(const char *path, long baud) {
	int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) return -1;
	struct termios t;
	if (tcgetattr(fd, &t) == 0) {
		cfmakeraw(&t); t.c_cflag &= ~(HUPCL | CRTSCTS); t.c_cflag |= CLOCAL | CREAD;
		if (baud) { speed_t sp = baud_const(baud); if (!sp) { close(fd); errno = EINVAL; return -1; } cfsetspeed(&t, sp); }
		if (tcsetattr(fd, TCSANOW, &t) != 0 && baud) { int e = errno; close(fd); errno = e; return -1; }
	}
	return fd;
}

static int write_all(int fd, const uint8_t *p, size_t n) {
	while (n) {
		struct pollfd pf = { fd, POLLOUT, 0 };
		int r = poll(&pf, 1, 5000);
		if (r <= 0) { fprintf(stderr, "gfxstream: display not accepting data\n"); return -1; }
		ssize_t w = write(fd, p, n);
		if (w < 0) { if (errno == EAGAIN || errno == EINTR) continue; perror("write"); return -1; }
		p += w; n -= (size_t)w;
	}
	return 0;
}

// Parse what the Pico sends back: 06 seq (frame shown) / 07 lo hi errs (packet executed) / 08 (graphics ready).
static void parse_replies(link_t *l, const uint8_t *buf, ssize_t n) {
	for (ssize_t i = 0; i < n; ++i) {
		uint8_t b = buf[i];
		switch (l->ack_state) {
		case 0: l->ack_state = b == GFX_ACK_BYTE ? 1 : b == GFX_PKT_ACK_BYTE ? 2 : 0; if (b == GFX_READY_BYTE) l->ready = 1; break;
		case 1: l->acked = b; l->ack_state = 0; break;
		case 2: l->ack_buf[0] = b; l->ack_state = 3; break;
		case 3: l->ack_buf[1] = b; l->ack_state = 4; break;
		case 4: {
			uint16_t seq = (uint16_t)(l->ack_buf[0] | (l->ack_buf[1] << 8));
			l->pk_errs = b; l->ack_state = 0;
			if (l->pk[seq & 255].seq == seq && l->pk[seq & 255].bytes) {
				lane_t *ln = &l->lane[l->pk[seq & 255].lane];
				ln->inflight = ln->inflight > l->pk[seq & 255].bytes ? ln->inflight - l->pk[seq & 255].bytes : 0;
				l->pk[seq & 255].bytes = 0;
			}
			break;
		}
		}
	}
}

static int read_replies(link_t *l, int timeout_ms) {
	struct pollfd pf = { l->lane[0].fd, POLLIN, 0 };
	if (poll(&pf, 1, timeout_ms) <= 0) return 0;
	uint8_t buf[256];
	ssize_t n = read(l->lane[0].fd, buf, sizeof buf);
	if (n > 0) parse_replies(l, buf, n);
	return n > 0;
}

static int identify(int fd, caps_t *c, char *reply, size_t rn) {
	tcflush(fd, TCIFLUSH);
	uint8_t enq = 0x05;
	if (write_all(fd, &enq, 1) < 0) return -1;
	size_t len = 0; double end = now_ms() + 1500;
	while (now_ms() < end && !memchr(reply, '\n', len)) {
		struct pollfd pf = { fd, POLLIN, 0 };
		if (poll(&pf, 1, 100) > 0) { ssize_t n = read(fd, reply + len, rn - 1 - len); if (n > 0) len += (size_t)n; }
		if (len >= rn - 1) break;
	}
	reply[len] = 0;
	if (strncmp(reply, "USBDISPLAY", 10) != 0) return -1;
	memset(c, 0, sizeof *c);
	const char *g = strstr(reply, "gfx=");
	if (g) sscanf(g, "gfx=%dx%dx", &c->w, &c->h);
	const char *p;
	if ((p = strstr(reply, "cache="))) sscanf(p, "cache=%d", &c->cache);
	if ((p = strstr(reply, " raw="))) sscanf(p, " raw=%d", &c->raw);
	if ((p = strstr(reply, "bm="))) sscanf(p, "bm=%d", &c->bm);
	if ((p = strstr(reply, "psram="))) sscanf(p, "psram=%d", &c->psram);
	if ((p = strstr(reply, "uart="))) sscanf(p, "uart=%d", &c->uart);
	if ((p = strstr(reply, "ring="))) sscanf(p, "ring=%d", &c->ring);
	if ((p = strstr(reply, "l1raw="))) sscanf(p, "l1raw=%d", &c->l1raw);
	return 0;
}

static int find_display(const char *explicit, int *fd_out, caps_t *c, char *reply, size_t rn, char *path_out, size_t pn) {
	const char *pats[] = { "/dev/serial/by-id/*Pico*", "/dev/serial/by-id/*2E8A*", "/dev/ttyACM*" };
	char *cands[64]; int nc = 0;
	if (explicit) cands[nc++] = strdup(explicit);
	else for (unsigned i = 0; i < sizeof pats / sizeof *pats; ++i) {
		glob_t g;
		if (glob(pats[i], 0, NULL, &g) == 0) { for (size_t k = 0; k < g.gl_pathc && nc < 64; ++k) cands[nc++] = strdup(g.gl_pathv[k]); }
		globfree(&g);
	}
	int found = -1;
	for (int i = 0; i < nc && found < 0; ++i) {
		int fd = open_serial(cands[i], 0);
		if (fd < 0) { fprintf(stderr, "gfxstream: %s: %s\n", cands[i], strerror(errno)); continue; }
		if (identify(fd, c, reply, rn) == 0) { *fd_out = fd; snprintf(path_out, pn, "%s", cands[i]); found = i; }
		else { fprintf(stderr, "gfxstream: %s does not answer like the display\n", cands[i]); close(fd); }
	}
	for (int i = 0; i < nc; ++i) free(cands[i]);
	return found < 0 ? -1 : 0;
}

/* ---- sending: single link = plain blocking writes; two links = per-link queues, scheduled by finish time ---- */

typedef struct { link_t *l; int multi; uint64_t bytes; } sink_t;

static void pump_out(link_t *l) {
	for (int i = 0; i < l->n; ++i) {
		lane_t *ln = &l->lane[i];
		if (ln->off >= ln->len) continue;
		ssize_t w = write(ln->fd, ln->buf + ln->off, ln->len - ln->off);
		if (w > 0) { ln->off += (size_t)w; ln->sent += (uint64_t)w; if (ln->off >= ln->len) ln->len = ln->off = 0; }
	}
}

static int pump(link_t *l, int timeout_ms) {
	struct pollfd pf[3]; int np = 0;
	pf[np++] = (struct pollfd){ l->lane[0].fd, POLLIN, 0 };
	for (int i = 0; i < l->n; ++i) if (l->lane[i].off < l->lane[i].len) pf[np++] = (struct pollfd){ l->lane[i].fd, POLLOUT, 0 };
	if (poll(pf, (nfds_t)np, timeout_ms) <= 0) return 0;
	if (pf[0].revents & POLLIN) { uint8_t b[256]; ssize_t n = read(l->lane[0].fd, b, sizeof b); if (n > 0) parse_replies(l, b, n); }
	pump_out(l);
	return 1;
}

static int emit_link(void *u, const uint8_t *d, size_t n) {
	sink_t *s = u;
	s->bytes += n;
	if (!s->l) return 0;                                   // --null
	link_t *l = s->l;
	if (!s->multi) { l->lane[0].sent += n; return write_all(l->lane[0].fd, d, n); }

	uint16_t seq = (d[1] & GFX_SEQ_FLAG) ? (uint16_t)(d[4] | (d[5] << 8)) : 0;
	double deadline = now_ms() + 5000;
	for (;;) {
		double t = now_ms();
		int pick = -1; double best = 1e30;
		for (int i = 0; i < l->n; ++i) {
			lane_t *ln = &l->lane[i];
			if (ln->len > ln->off) continue;                   // previous packet still being handed to the kernel
			if (ln->is_uart && ln->inflight + n > ln->window) continue;   // would overrun the Pico's receive ring
			double start = ln->finish_ms > t ? ln->finish_ms : t;
			double fin = start + (double)n * 1000.0 / ln->rate;
			if (fin < best) { best = fin; pick = i; }
		}
		if (pick >= 0) {
			lane_t *ln = &l->lane[pick];
			memcpy(ln->buf, d, n); ln->len = n; ln->off = 0;
			ln->finish_ms = best;
			l->pk[seq & 255].seq = seq; l->pk[seq & 255].bytes = ln->is_uart ? (uint32_t)n : 0; l->pk[seq & 255].lane = pick;
			if (ln->is_uart) ln->inflight += (uint32_t)n;
			pump_out(l);
			return 0;
		}
		pump(l, 20);
		if (now_ms() > deadline) {
			for (int i = 0; i < l->n; ++i) if (l->lane[i].is_uart && l->lane[i].inflight) { fprintf(stderr, "gfxstream: no acks for the UART link - assuming the data was lost\n"); l->lane[i].inflight = 0; deadline = now_ms() + 5000; }
			if (now_ms() > deadline) { fprintf(stderr, "gfxstream: display not accepting data\n"); return -1; }
		}
	}
}

static void drain_links(link_t *l, int ms) {
	double end = now_ms() + ms;
	while (now_ms() < end) {
		int busy = 0;
		for (int i = 0; i < l->n; ++i) busy |= l->lane[i].len > l->lane[i].off;
		if (!busy) return;
		pump(l, 20);
	}
}

static void usage(void) {
	fprintf(stderr,
	"usage: gfxstream [options]\n"
	"  -s, --source x11|fb|test|noise   what to show (default x11; noise = worst-case link test)\n"
	"  -d, --display :0           X display (default $DISPLAY)\n"
	"      --fb /dev/fb0          framebuffer device for --source fb\n"
	"  -p, --port /dev/ttyACM0    serial port (default: auto-detect)\n"
	"  -u, --uart /dev/ttyXXX     second link: a UART wired to the Pico (firmware built with GFX_UART; baud is\n"
	"                             taken from the Pico). Frames are spread over USB and UART.\n"
	"  -f, --fps N                max frames per second (default 30)\n"
	"      --usb-rate KB          USB throughput used for scheduling and the frame budget (default 800)\n"
	"      --rate KB              total throughput for the frame budget (default: sum of the links x 0.9)\n"
	"      --no-budget            never defer tiles; send every change at once\n"
	"      --dither               ordered dithering when reducing to 8 bit colour (photos / video)\n"
	"      --idle-fps N           capture rate while nothing changes (default 8, 0 = same as --fps)\n"
	"      --wait                 wait for the Pico to appear instead of exiting\n"
	"      --info                 print what the display reports (size, cache, PSRAM, UART) and exit\n"
	"      --null                 no hardware: just encode and report (benchmark; runs unpaced)\n"
	"      --realtime             with --null, pace at --fps\n"
	"  -n, --frames N             stop after N captured frames\n"
	"      --no-scroll            disable scroll detection\n"
	"      --probe [DISPLAY]      exit 0 if the X display can be opened, print its size\n");
}

int main(int argc, char **argv) {
	const char *source = "x11", *display = NULL, *fbdev = "/dev/fb0", *port = NULL, *uart = NULL;
	double fps = 30, usb_rate_kb = 800, rate_kb = 0, idle_fps = 8; long max_frames = -1;
	int null = 0, realtime = 0, scroll = 1, no_budget = 0, dither = 0, wait = 0, info = 0;
	for (int i = 1; i < argc; ++i) {
		const char *a = argv[i];
		#define NEXT() (i + 1 < argc ? argv[++i] : (usage(), exit(2), (char *)0))
		if (!strcmp(a, "-s") || !strcmp(a, "--source")) source = NEXT();
		else if (!strcmp(a, "-d") || !strcmp(a, "--display")) display = NEXT();
		else if (!strcmp(a, "--fb")) fbdev = NEXT();
		else if (!strcmp(a, "-p") || !strcmp(a, "--port")) port = NEXT();
		else if (!strcmp(a, "-u") || !strcmp(a, "--uart")) uart = NEXT();
		else if (!strcmp(a, "-f") || !strcmp(a, "--fps")) fps = atof(NEXT());
		else if (!strcmp(a, "--usb-rate")) usb_rate_kb = atof(NEXT());
		else if (!strcmp(a, "--rate")) rate_kb = atof(NEXT());
		else if (!strcmp(a, "--idle-fps")) idle_fps = atof(NEXT());
		else if (!strcmp(a, "-n") || !strcmp(a, "--frames")) max_frames = atol(NEXT());
		else if (!strcmp(a, "--no-budget")) no_budget = 1;
		else if (!strcmp(a, "--dither")) dither = 1;
		else if (!strcmp(a, "--wait")) wait = 1;
		else if (!strcmp(a, "--info")) info = 1;
		else if (!strcmp(a, "--null")) null = 1;
		else if (!strcmp(a, "--realtime")) realtime = 1;
		else if (!strcmp(a, "--no-scroll")) scroll = 0;
		else if (!strcmp(a, "--probe")) return probe_x11(i + 1 < argc ? argv[i + 1] : getenv("DISPLAY"));
		else { usage(); return 2; }
	}
	if (fps <= 0) fps = 30;

	signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);

	caps_t caps = { 640, 480, 256, GFX_RAW_MAX_DEFAULT, 0, 0, 0, 0, 0 };
	link_t link; memset(&link, 0, sizeof link);
	link.lane[0].fd = -1; link.lane[1].fd = -1; link.n = 1;
	char reply[320] = "", portname[256] = "";
	if (!null) {
		int fd = -1, announced = 0;
		while (find_display(port, &fd, &caps, reply, sizeof reply, portname, sizeof portname) < 0) {
			if (!wait || g_stop) { fprintf(stderr, "gfxstream: no display found (is the Pico plugged in? try --port, or --wait)\n"); return 1; }
			if (!announced) { fprintf(stderr, "gfxstream: waiting for the Pico ...\n"); announced = 1; }
			sleep_ms(1000);
		}
		link.lane[0].fd = fd; link.lane[0].rate = usb_rate_kb * 1024.0;
		if (info) {
			printf("%s\n%s: gfx %dx%d, tile cache %d, PSRAM %d B, bitmap memory %d B, UART %d baud, ring %d B\n", reply, portname,
			       caps.w, caps.h, caps.cache, caps.psram, caps.bm, caps.uart, caps.ring);
			return 0;
		}
		if (caps.w <= 0 || caps.h <= 0) {
			fprintf(stderr, "gfxstream: the firmware on %s has no graphics mode (reply: '%s').\n"
			        "Rebuild the Pico firmware with  ./build.sh  (enable Graphics mode) and flash it.\n", portname, reply);
			return 1;
		}
		if (caps.raw <= 0) caps.raw = GFX_RAW_MAX_DEFAULT;
		fprintf(stderr, "gfxstream: %s: %dx%d, tile cache %d tiles%s, packet %d B\n", portname, caps.w, caps.h, caps.cache,
		        caps.psram ? " (PSRAM)" : "", caps.raw);
		if (uart) {
			if (caps.uart <= 0 || caps.ring <= 0) { fprintf(stderr, "gfxstream: the firmware has no UART link (build it with the UART option); ignoring --uart\n"); uart = NULL; }
			else {
				int ufd = open_serial(uart, caps.uart);
				if (ufd < 0) { fprintf(stderr, "gfxstream: cannot open %s at %d baud: %s\n", uart, caps.uart, strerror(errno)); return 1; }
				lane_t *u = &link.lane[1];
				u->fd = ufd; u->is_uart = 1; u->rate = caps.uart / 10.0 * 0.95;
				u->window = (uint32_t)caps.ring / 2;
				link.n = 2;
				int l1 = caps.l1raw > 0 ? caps.l1raw : 8192;
				if (caps.raw > l1 - 2) caps.raw = l1 - 2;
				fprintf(stderr, "gfxstream: second link %s at %d baud (~%.0f KB/s), window %u B, packets <= %d B\n", uart, caps.uart, u->rate / 1024, u->window, caps.raw);
			}
		}
		for (int i = 0; i < link.n; ++i) { link.lane[i].cap = (size_t)caps.raw + 32; link.lane[i].buf = malloc(link.lane[i].cap); }
	}
	const int W = caps.w, H = caps.h;

	source_t src = { 0 };
	int rc;
	if (!strcmp(source, "x11")) rc = open_x11(&src, display ? display : getenv("DISPLAY"));
	else if (!strcmp(source, "fb")) rc = open_fb(&src, fbdev);
	else if (!strcmp(source, "test")) rc = open_test(&src, W, H);
	else if (!strcmp(source, "noise")) rc = open_noise(&src, W, H);
	else { usage(); return 2; }
	if (rc < 0) return 1;

	sink_t sink = { null ? NULL : &link, link.n > 1, 0 };
	gfx_enc_t enc;
	if (gfx_enc_init(&enc, W, H, caps.raw, caps.cache, emit_link, &sink) < 0) { fprintf(stderr, "gfxstream: encoder init failed\n"); return 1; }
	enc.enable_scroll = scroll;
	enc.use_seq = sink.multi;
	double total_rate = (null ? usb_rate_kb * 1024.0 : link.lane[0].rate) + (link.n > 1 ? link.lane[1].rate : 0);
	if (rate_kb > 0) total_rate = rate_kb * 1024.0;
	if (!no_budget) { enc.budget_wire = (int)(total_rate * 0.9 / fps); fprintf(stderr, "gfxstream: frame budget %d B (%.0f KB/s at %.0f fps)\n", enc.budget_wire, total_rate / 1024, fps); }

	if (!null) {                                           // enter graphics mode (SO); wait until the Pico is ready
		uint8_t so = GFX_ENTER_BYTE;
		if (write_all(link.lane[0].fd, &so, 1) < 0) return 1;
		for (int i = 0; i < 20 && !link.ready; ++i) read_replies(&link, 50);
		if (!link.ready && sink.multi) { fprintf(stderr, "gfxstream: the Pico did not confirm graphics mode\n"); return 1; }
	}

	uint8_t *cur = malloc((size_t)W * H);
	int *xmap = malloc((size_t)W * sizeof(int));
	double period = 1000.0 / fps, next = now_ms(), stat_t = now_ms();
	double enc_ms = 0, cap_ms = 0; long frames = 0, sent_frames = 0, stat_frames = 0, idle_run = 0; uint64_t stat_bytes = 0, tot_bytes = 0;
	uint64_t lane_prev[2] = { 0, 0 };
	int status = 0;

	while (!g_stop && (max_frames < 0 || frames < max_frames)) {
		if (!null) {                                       // flow control: at most MAX_INFLIGHT unacked frames
			double deadline = now_ms() + 1000;
			while (!g_stop && (uint8_t)(enc.seq - link.acked) >= MAX_INFLIGHT) {
				if (sink.multi) pump(&link, 20); else read_replies(&link, 50);
				if (now_ms() > deadline) { link.acked = enc.seq; break; }
			}
			if (sink.multi) pump(&link, 0); else read_replies(&link, 0);
			if (link.pk_errs && !link.pk_err_warned && link.n > 1) {
				fprintf(stderr, "gfxstream: the Pico reports decode errors (%u) - the UART link is probably too fast or noisy\n", link.pk_errs);
				link.pk_err_warned = 1;
			}
		}
		double t0 = now_ms();
		frame_t f;
		if (src.grab(src.ctx, &f) < 0) { fprintf(stderr, "gfxstream: capture failed\n"); status = 1; break; }
		to332(&f, cur, W, H, xmap, dither);
		double t1 = now_ms();
		uint64_t b0 = sink.bytes;
		long n = gfx_enc_frame(&enc, cur);
		double t2 = now_ms();
		if (n < 0) { fprintf(stderr, "gfxstream: link error\n"); status = 1; break; }
		cap_ms += t1 - t0; enc_ms += t2 - t1; frames++; stat_frames++;
		if (n > 0) sent_frames++;
		stat_bytes += sink.bytes - b0; tot_bytes += sink.bytes - b0;
		idle_run = (n == 0 && enc.deferred == 0) ? idle_run + 1 : 0;

		double now = now_ms();
		if (now - stat_t >= 1000 || (max_frames >= 0 && frames == max_frames)) {
			double dt = (now - stat_t) / 1000.0;
			char lanes[96] = "";
			if (link.n > 1) snprintf(lanes, sizeof lanes, "  [usb %.0f + uart %.0f KB/s]", (double)(link.lane[0].sent - lane_prev[0]) / 1024 / dt, (double)(link.lane[1].sent - lane_prev[1]) / 1024 / dt);
			lane_prev[0] = link.lane[0].sent; lane_prev[1] = link.n > 1 ? link.lane[1].sent : 0;
			fprintf(stderr, "gfxstream: %5.1f fps  %7.1f KB/s  capture %.2f ms  encode %.2f ms  (sent %ld/%ld%s)%s\n",
			        stat_frames / dt, stat_bytes / 1024.0 / dt, cap_ms / stat_frames, enc_ms / stat_frames, sent_frames, frames,
			        enc.deferred ? ", tiles deferred" : "", lanes);
			stat_t = now; stat_frames = 0; stat_bytes = 0; cap_ms = enc_ms = 0;
		}
		if (!null || realtime) {
			double p = (idle_fps > 0 && idle_run > 15 && idle_fps < fps) ? 1000.0 / idle_fps : period;   // slow down while the screen is static
			next += p; double wait_ms = next - now_ms(); if (wait_ms > 0) sleep_ms(wait_ms); else next = now_ms();
		}
	}

	if (!null && link.lane[0].fd >= 0) {                   // back to the text terminal
		if (sink.multi) drain_links(&link, 1000);
		gfx_enc_mode_text(&enc);
		if (sink.multi) drain_links(&link, 1000);
		tcdrain(link.lane[0].fd);
		for (int i = 0; i < link.n; ++i) close(link.lane[i].fd);
	}
	fprintf(stderr, "gfxstream: %ld frames, %.1f KB total (%.2f KB/frame); ops FILL %llu COPY %llu MONO %llu RAW %llu CACHED %llu\n",
	        frames, tot_bytes / 1024.0, frames ? tot_bytes / 1024.0 / frames : 0.0,
	        (unsigned long long)enc.st_ops[GFX_OP_FILL], (unsigned long long)enc.st_ops[GFX_OP_COPY], (unsigned long long)enc.st_ops[GFX_OP_MONO],
	        (unsigned long long)enc.st_ops[GFX_OP_RAW], (unsigned long long)enc.st_ops[GFX_OP_CACHED]);
	src.close(src.ctx);
	gfx_enc_free(&enc);
	free(cur); free(xmap);
	return status;
}
