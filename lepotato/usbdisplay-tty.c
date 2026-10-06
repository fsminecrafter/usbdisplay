/*
 * usbdisplay-tty - mirror a Linux virtual console (tty1) onto the Pico USB display.
 *
 * C port of usbdisplay-tty.py. It does not intercept bytes written to the console; it reads the
 * finished screen of the kernel's terminal emulator (/dev/vcsaN = glyphs + colour attributes,
 * /dev/vcsuN = real Unicode) and redraws the Pico whenever it changes, so clear screen, \r progress
 * bars, erase-line, scrolling, nano/htop all look exactly as on a monitor.
 *
 * The Pico firmware has no cursor addressing, so a frame is drawn as
 *     HOME, then per row: the characters (if the row changed) or just "\n" (if unchanged),
 * and the cursor is parked by re-printing the start of its row. A full refresh is sent every few
 * seconds to repair any dropped byte.
 *
 * Dynamic frame rate (all adjustable, see --help):
 *     nothing changing       ->  2 fps   (and the process sleeps in poll() on /dev/vcsa, so it
 *                                         reacts to a change immediately and costs ~0 CPU)
 *     something changing     -> 24 fps   (typing, one progress bar, a few rows)
 *     a lot changing         -> 30 fps   (>= 8 rows changed in one frame: scrolling, htop, editors)
 * It steps up at once and steps down after a hold time (1 s / 0.5 s).
 *
 * Needs root (reads /dev/vcsa*, resizes the console to the display size).
 * Build: ./build.sh     Install: ./setup.sh
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <glob.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "usbdisplay-tty-ascii.h"

#define VERSION "2.0-c"

#define FF_BYTE   0x0c
#define HOME_BYTE 0x01
#define ENQ_BYTE  0x05
#define SGR_RESET "\x1b[0m"
#define DEF_ATTR  0x07

#define MAX_DIM   255
#define VCSA_MAX  (4 + 2 * MAX_DIM * MAX_DIM + 16)
#define VCSU_MAX  (4 + 4 * MAX_DIM * MAX_DIM + 16)

static volatile sig_atomic_t g_stop;
static bool g_verbose;

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "usbdisplay-tty: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void sleep_s(double s)
{
    if (s <= 0)
        return;
    struct timespec ts = { (time_t)s, (long)((s - (double)(time_t)s) * 1e9) };
    nanosleep(&ts, NULL);                       /* a signal ends the sleep early; callers check g_stop */
}

/* ------------------------------------------------------------------ character mapping */

/* Unicode code point -> one printable ASCII byte (the display font is ASCII only).
 * Same mapping as to_ascii() in usbdisplay-tty.py; the table is generated from it. */
static uint8_t to_ascii(uint32_t cp)
{
    if (cp >= 32 && cp < 127)
        return (uint8_t)cp;
    if (cp == 0)
        return ' ';
    if (cp < 0x80)
        return '?';
    size_t lo = 0, hi = sizeof ASCII_RANGES / sizeof ASCII_RANGES[0];
    while (lo < hi) {                           /* first range starting after cp */
        size_t mid = (lo + hi) / 2;
        if (ASCII_RANGES[mid].lo <= cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo && cp <= ASCII_RANGES[lo - 1].hi)
        return ASCII_RANGES[lo - 1].ch;
    return '?';
}

static uint8_t t256[256];                       /* fast path for code points < 256 */

/* ------------------------------------------------------------------ options */

typedef struct {
    int tty;
    const char *port;
    int colour;                                 /* -1 auto, 0 off, 1 on */
    bool resize;
    double fps_idle, fps_mid, fps_high;
    int busy_rows;                              /* changed rows in one frame that count as "a lot" */
    double hold_mid, hold_high;                 /* seconds to stay at a level after the last trigger */
    double refresh;
    const char *vcsa, *vcsu;                    /* test hooks: use plain files instead of /dev/vcsa* */
    bool no_poll;
} Args;

/* ------------------------------------------------------------------ display */

typedef struct {
    int fd;
    char port[256];
    int cols, rows;
    bool colour;
} Display;

static int disp_write(Display *d, const uint8_t *p, size_t n)
{
    while (n) {
        struct pollfd pf = { d->fd, POLLOUT, 0 };
        int r = poll(&pf, 1, 5000);
        if (r < 0) {
            if (errno == EINTR) {
                if (g_stop)
                    return -1;
                continue;
            }
            return -1;
        }
        if (r == 0) {
            errno = ETIMEDOUT;                  /* display is not accepting data */
            return -1;
        }
        if (pf.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = EIO;
            return -1;
        }
        ssize_t w = write(d->fd, p, n);
        if (w < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void disp_drain(Display *d)
{
    tcflush(d->fd, TCIFLUSH);                   /* ignore anything the Pico sends back */
}

/* Send ENQ, return the reply line in buf (empty if none within 1.5 s). */
static void disp_identify(Display *d, char *buf, size_t cap)
{
    size_t n = 0;
    uint8_t enq = ENQ_BYTE;
    tcflush(d->fd, TCIFLUSH);
    buf[0] = 0;
    if (disp_write(d, &enq, 1) < 0)
        return;
    double deadline = now_s() + 1.5;
    while (now_s() < deadline && !g_stop && !memchr(buf, '\n', n)) {
        struct pollfd pf = { d->fd, POLLIN, 0 };
        if (poll(&pf, 1, 100) > 0) {
            ssize_t r = read(d->fd, buf + n, cap - 1 - n);
            if (r > 0)
                n += (size_t)r;
        }
        buf[n] = 0;
        if (n >= cap - 1)
            break;
    }
    buf[n] = 0;
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
        buf[--n] = 0;
}

static int disp_open(Display *d, const char *port, bool verify)
{
    char reply[256];
    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return -1;
    struct termios t;
    if (tcgetattr(fd, &t) == 0) {
        cfmakeraw(&t);
        t.c_cflag &= ~HUPCL;                    /* keep DTR asserted if we exit */
        tcsetattr(fd, TCSANOW, &t);
    }
    memset(d, 0, sizeof *d);
    d->fd = fd;
    snprintf(d->port, sizeof d->port, "%s", port);
    d->cols = 80;
    d->rows = 30;
    disp_identify(d, reply, sizeof reply);
    if (verify && strncmp(reply, "USBDISPLAY", 10) != 0) {
        close(fd);
        errno = ENOTSUP;
        return -1;
    }
    for (const char *p = reply; *p; p++) {      /* first "<cols>x<rows>" */
        if (*p >= '0' && *p <= '9' && (p == reply || p[-1] < '0' || p[-1] > '9')) {
            int c, r, used = 0;
            if (sscanf(p, "%dx%d%n", &c, &r, &used) == 2 && used) {
                if (c >= 1 && c <= MAX_DIM && r >= 1 && r <= MAX_DIM) {
                    d->cols = c;
                    d->rows = r;
                }
                break;
            }
        }
    }
    d->colour = strcasestr(reply, "color") || strcasestr(reply, "colour");
    return 0;
}

/* Find and open the display, waiting for it to be plugged in. Returns 0, or -1 if asked to stop. */
static int display_wait(const Args *a, Display *d)
{
    bool announced = false;
    char lastfail[300] = "";
    while (!g_stop) {
        char *cands[64];
        int nc = 0;
        glob_t g[3];
        int ng = 0;
        if (a->port) {
            if (access(a->port, F_OK) == 0)
                cands[nc++] = (char *)a->port;
        } else {
            static const char *pats[] = { "/dev/serial/by-id/*Pico*", "/dev/serial/by-id/*2E8A*", "/dev/ttyACM*" };
            for (int i = 0; i < 3; i++) {
                if (glob(pats[i], 0, NULL, &g[ng]) == 0) {
                    for (size_t k = 0; k < g[ng].gl_pathc && nc < 64; k++) {
                        bool dup = false;
                        for (int j = 0; j < nc; j++)
                            dup |= strcmp(cands[j], g[ng].gl_pathv[k]) == 0;
                        if (!dup)
                            cands[nc++] = g[ng].gl_pathv[k];
                    }
                    ng++;
                }
            }
        }
        for (int i = 0; i < nc; i++) {
            if (disp_open(d, cands[i], a->port == NULL) == 0) {
                for (int k = 0; k < ng; k++)
                    globfree(&g[k]);
                return 0;
            }
            char msg[300];
            snprintf(msg, sizeof msg, "%s: %s", cands[i], strerror(errno));
            if (strcmp(msg, lastfail) != 0) {   /* log each distinct failure once */
                logmsg("%s", msg);
                snprintf(lastfail, sizeof lastfail, "%s", msg);
            }
        }
        for (int k = 0; k < ng; k++)
            globfree(&g[k]);
        if (!announced) {
            logmsg("waiting for the Pico display ...");
            announced = true;
        }
        sleep_s(1.0);
    }
    return -1;
}

/* ------------------------------------------------------------------ console access */

typedef struct {
    int n;
    int fa, fu;                                 /* vcsa / vcsu (fu = -1 if unavailable) */
    uint8_t *bufa, *bufu;                       /* current raw reads */
    uint8_t *keya, *keyu;                       /* raw data of the previous snapshot */
    bool have_key, kuni;
    int kvr, kvc, krows, kcols;
    int rows, cols;                             /* converted output size (= display size) */
    uint8_t *lines, *atts;                      /* rows*cols converted characters / attributes */
    int cx, cy;
    bool valid;                                 /* lines/atts hold a snapshot */
} Console;

enum { SNAP_TORN = -1, SNAP_SAME = 0, SNAP_NEW = 1 };

static int con_open(Console *c, const Args *a, int rows, int cols)
{
    char path[128];
    memset(c, 0, sizeof *c);
    c->n = a->tty;
    c->fu = -1;
    if (a->vcsa)
        snprintf(path, sizeof path, "%s", a->vcsa);
    else
        snprintf(path, sizeof path, "/dev/vcsa%d", a->tty);
    c->fa = open(path, O_RDONLY | O_CLOEXEC);
    if (c->fa < 0) {
        logmsg("%s: %s", path, strerror(errno));
        return -1;
    }
    if (a->vcsu)
        snprintf(path, sizeof path, "%s", a->vcsu);
    else
        snprintf(path, sizeof path, "/dev/vcsu%d", a->tty);
    c->fu = open(path, O_RDONLY | O_CLOEXEC);   /* optional */
    c->rows = rows;
    c->cols = cols;
    c->bufa = malloc(VCSA_MAX);
    c->keya = malloc(VCSA_MAX);
    c->bufu = malloc(VCSU_MAX);
    c->keyu = malloc(VCSU_MAX);
    c->lines = malloc((size_t)rows * cols);
    c->atts = malloc((size_t)rows * cols);
    if (!c->bufa || !c->keya || !c->bufu || !c->keyu || !c->lines || !c->atts) {
        logmsg("out of memory");
        return -1;
    }
    memset(c->lines, ' ', (size_t)rows * cols);
    memset(c->atts, DEF_ATTR, (size_t)rows * cols);
    return 0;
}

static void con_close(Console *c)
{
    if (c->fa >= 0)
        close(c->fa);
    if (c->fu >= 0)
        close(c->fu);
    free(c->bufa);
    free(c->keya);
    free(c->bufu);
    free(c->keyu);
    free(c->lines);
    free(c->atts);
    memset(c, 0, sizeof *c);
    c->fa = c->fu = -1;
}

static int con_resize(const Console *c, int rows, int cols)
{
    char path[32];
    snprintf(path, sizeof path, "/dev/tty%d", c->n);
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return -1;
    struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
    int r = ioctl(fd, TIOCSWINSZ, &ws);
    int e = errno;
    close(fd);
    errno = e;
    return r;
}

/*
 * Read the console. Returns SNAP_SAME if the kernel screen is byte-identical to the previous call
 * (costs two reads and a memcmp), SNAP_TORN for an inconsistent read, otherwise SNAP_NEW with
 * c->lines/atts/cx/cy updated. Only rows whose raw bytes changed are converted again.
 * lines[y] is `cols` bytes of printable ASCII, atts[y] is `cols` console attribute bytes.
 */
static int con_snapshot(Console *c)
{
    const int rows = c->rows, cols = c->cols;
    ssize_t la = pread(c->fa, c->bufa, VCSA_MAX, 0);
    if (la < 4)
        return SNAP_TORN;
    int vr = c->bufa[0], vc = c->bufa[1], cx = c->bufa[2], cy = c->bufa[3];
    if (!vr || !vc || la != 4 + 2 * vr * vc)
        return SNAP_TORN;

    const uint8_t *ru = NULL;
    if (c->fu >= 0) {
        ssize_t lu = pread(c->fu, c->bufu, VCSU_MAX, 0);
        if (lu == 4 + 4 * vr * vc)
            ru = c->bufu + 4;
        else if (lu == 4 * vr * vc)
            ru = c->bufu;
    }
    const bool uni = ru != NULL;
    const size_t nu = (size_t)4 * vr * vc;

    const bool same_geom = c->have_key && c->kvr == vr && c->kvc == vc && c->kuni == uni;
    if (same_geom && memcmp(c->bufa, c->keya, (size_t)la) == 0 && (!uni || memcmp(ru, c->keyu, nu) == 0))
        return SNAP_SAME;

    const int w = vc < cols ? vc : cols;
    const uint8_t *ka = c->keya + 4;
    for (int y = 0; y < rows; y++) {
        uint8_t *line = c->lines + (size_t)y * cols;
        uint8_t *at = c->atts + (size_t)y * cols;
        if (y >= vr) {
            if (!same_geom || !c->valid) {
                memset(line, ' ', (size_t)cols);
                memset(at, DEF_ATTR, (size_t)cols);
            }
            continue;
        }
        const size_t lo = (size_t)y * vc;
        const uint8_t *ra = c->bufa + 4 + 2 * lo;
        if (same_geom && c->valid && memcmp(ra, ka + 2 * lo, (size_t)2 * vc) == 0 &&
            (!uni || memcmp(ru + 4 * lo, c->keyu + 4 * lo, (size_t)4 * vc) == 0))
            continue;                           /* row unchanged: keep the converted line */
        if (uni) {
            for (int x = 0; x < w; x++) {
                uint32_t v;
                memcpy(&v, ru + 4 * (lo + (size_t)x), 4);
                line[x] = v < 256 ? t256[v] : to_ascii(v);
            }
        } else {                                /* no Unicode device: glyph index */
            for (int x = 0; x < w; x++) {
                uint8_t g = ra[2 * x];
                line[x] = (g >= 32 && g < 127) ? g : (g == 0 ? ' ' : '?');
            }
        }
        for (int x = 0; x < w; x++)
            at[x] = ra[2 * x + 1];
        for (int x = w; x < cols; x++) {
            line[x] = ' ';
            at[x] = DEF_ATTR;
        }
    }
    c->cx = cx < cols ? cx : cols - 1;
    c->cy = cy < rows ? cy : rows - 1;
    c->valid = true;

    memcpy(c->keya, c->bufa, (size_t)la);
    if (uni)
        memcpy(c->keyu, ru, nu);
    c->have_key = true;
    c->kuni = uni;
    c->kvr = vr;
    c->kvc = vc;
    return SNAP_NEW;
}

/* Sleep until the kernel reports a screen update or `timeout` seconds pass.
 * /dev/vcsa* is always "readable"; the update signal is POLLPRI, cleared by the next read of this fd.
 * Returns 1 = update signalled, 0 = timeout, -1 = poll() is not usable on this device. */
static int con_wait(const Console *c, double timeout)
{
    struct pollfd pf = { c->fa, POLLPRI, 0 };
    int ms = (int)(timeout * 1000.0 + 0.999);
    if (ms < 0)
        ms = 0;
    int r = poll(&pf, 1, ms);
    if (r < 0)
        return errno == EINTR ? 0 : -1;
    if (r == 0)
        return 0;
    if (pf.revents & POLLPRI)
        return 1;
    return -1;                                  /* POLLHUP / POLLNVAL / POLLERR only */
}

/* ------------------------------------------------------------------ drawing */

static const uint8_t VGA2ANSI[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };   /* console colour index -> ANSI 0-7 */

static uint8_t *put_sgr(uint8_t *o, uint8_t attr)
{
    if (attr == DEF_ATTR) {
        memcpy(o, SGR_RESET, 4);
        return o + 4;
    }
    int fg = attr & 0x0F, bg = (attr >> 4) & 0x07;
    int f = ((fg & 8) ? 90 : 30) + VGA2ANSI[fg & 7];
    int b = 40 + VGA2ANSI[bg];
    *o++ = 0x1b; *o++ = '['; *o++ = '0'; *o++ = ';';
    *o++ = (uint8_t)('0' + f / 10); *o++ = (uint8_t)('0' + f % 10); *o++ = ';';
    *o++ = (uint8_t)('0' + b / 10); *o++ = (uint8_t)('0' + b % 10); *o++ = 'm';
    return o;
}

static uint8_t *emit_row(uint8_t *o, const uint8_t *line, const uint8_t *at, int upto, bool colour)
{
    if (!colour) {
        memcpy(o, line, (size_t)upto);
        return o + upto;
    }
    int cur = -1;
    for (int i = 0; i < upto; i++) {
        if (at[i] != cur) {
            o = put_sgr(o, at[i]);
            cur = at[i];
        }
        *o++ = line[i];
    }
    return o;
}

/* Worst-case size of one frame. */
static size_t frame_cap(int rows, int cols)
{
    return (size_t)(rows + 2) * ((size_t)cols * 12 + 2) + 64;
}

/* Build the byte stream that brings the display from `prev` to `cur` (prev may be NULL). */
static size_t render(uint8_t *out, const uint8_t *lines, const uint8_t *atts, int rows, int cols, int cx, int cy,
                     const uint8_t *plines, const uint8_t *patts, bool colour, bool full)
{
    uint8_t *o = out;
    *o++ = HOME_BYTE;
    for (int y = 0; y < rows; y++) {
        const uint8_t *l = lines + (size_t)y * cols, *a = atts + (size_t)y * cols;
        bool changed = full || !plines || memcmp(l, plines + (size_t)y * cols, (size_t)cols) != 0 ||
                       (colour && memcmp(a, patts + (size_t)y * cols, (size_t)cols) != 0);
        if (changed)
            o = emit_row(o, l, a, cols, colour);
        if (y != rows - 1)
            *o++ = '\n';
    }
    /* Park the cursor: HOME, down cy rows, re-print the start of that row up to cx. */
    *o++ = HOME_BYTE;
    for (int i = 0; i < cy; i++)
        *o++ = '\n';
    o = emit_row(o, lines + (size_t)cy * cols, atts + (size_t)cy * cols, cx, colour);
    if (colour) {
        memcpy(o, SGR_RESET, 4);
        o += 4;
    }
    return (size_t)(o - out);
}

/* ------------------------------------------------------------------ main loop */

enum { T_IDLE, T_MID, T_HIGH };
static const char *const TIER_NAME[] = { "idle", "mid", "high" };

static int session(const Args *a)
{
    Display d;
    Console con;
    uint8_t *out = NULL, *plines = NULL, *patts = NULL;
    int rc = 0;

    if (display_wait(a, &d) < 0)
        return 0;
    const int rows = d.rows, cols = d.cols;
    const bool colour = a->colour < 0 ? d.colour : a->colour == 1;
    logmsg("display %s: %dx%d, colour %s", d.port, cols, rows, colour ? "on" : "off");

    con.fa = con.fu = -1;
    con.bufa = con.keya = con.bufu = con.keyu = NULL;
    con.lines = con.atts = NULL;
    if (con_open(&con, a, rows, cols) < 0) {
        con_close(&con);
        close(d.fd);
        return -1;
    }
    out = malloc(frame_cap(rows, cols));
    plines = malloc((size_t)rows * cols);
    patts = malloc((size_t)rows * cols);
    if (!out || !plines || !patts) {
        logmsg("out of memory");
        rc = -1;
        goto done;
    }
    if (a->resize && !a->vcsa && con_resize(&con, rows, cols) < 0)
        logmsg("could not resize tty%d to %dx%d: %s", a->tty, cols, rows, strerror(errno));

    {
        uint8_t init[8] = { FF_BYTE };
        size_t n = 1;
        if (colour) {
            memcpy(init + 1, SGR_RESET, 4);
            n += 4;
        }
        if (disp_write(&d, init, n) < 0) {
            rc = -1;
            goto done;
        }
    }

    const double fps[3] = { a->fps_idle, a->fps_mid, a->fps_high };
    const double floor_s = 1.0 / a->fps_high;   /* never snapshot faster than the top rate */
    bool have_prev = false;
    int pcx = 0, pcy = 0, tier = T_IDLE, spurious = 0, woke = 0;
    double last_full = 0, last_change = -1e9, last_high = -1e9, poll_pause_until = 0;

    while (!g_stop) {
        const double now = now_s();
        const int s = con_snapshot(&con);
        const bool full = !have_prev || now - last_full >= a->refresh;

        /* An update signal that brought no change: if it keeps happening, stop trusting poll() for a while. */
        if (woke == 1) {
            if (s == SNAP_NEW) {
                spurious = 0;
            } else if (++spurious >= 8) {
                spurious = 0;
                poll_pause_until = now + 1.0;
                if (g_verbose)
                    logmsg("poll() keeps waking without changes; using the timer for 1 s");
            }
        }

        if (con.valid && (s == SNAP_NEW || full)) {
            int changed_rows = 0;
            bool cursor = false;
            if (have_prev) {
                for (int y = 0; y < rows; y++) {
                    size_t o = (size_t)y * cols;
                    if (memcmp(con.lines + o, plines + o, (size_t)cols) != 0 ||
                        (colour && memcmp(con.atts + o, patts + o, (size_t)cols) != 0))
                        changed_rows++;
                }
                cursor = con.cx != pcx || con.cy != pcy;
            }
            if (!have_prev || full || changed_rows || cursor) {
                size_t n = render(out, con.lines, con.atts, rows, cols, con.cx, con.cy,
                                  have_prev ? plines : NULL, patts, colour, full);
                if (disp_write(&d, out, n) < 0) {
                    if (!g_stop)
                        logmsg("%s", strerror(errno));
                    rc = g_stop ? 0 : -1;
                    break;
                }
                disp_drain(&d);
                memcpy(plines, con.lines, (size_t)rows * cols);
                memcpy(patts, con.atts, (size_t)rows * cols);
                pcx = con.cx;
                pcy = con.cy;
                if (full)
                    last_full = now;
                if (!have_prev || changed_rows || cursor)
                    last_change = now;
                if (changed_rows >= a->busy_rows)
                    last_high = now;
                have_prev = true;
            }
        }

        /* Pick the frame rate for the next interval. */
        int nt = (now - last_high < a->hold_high) ? T_HIGH : (now - last_change < a->hold_mid) ? T_MID : T_IDLE;
        if (nt != tier) {
            if (g_verbose)
                logmsg("fps %s (%g) -> %s (%g)", TIER_NAME[tier], fps[tier], TIER_NAME[nt], fps[nt]);
            tier = nt;
        }
        const double period = 1.0 / fps[tier];

        woke = 0;
        if (tier == T_IDLE) {
            double to = period, until_full = last_full + a->refresh - now_s();
            if (until_full < to)
                to = until_full;
            if (to < 0)
                to = 0;
            if (!a->no_poll && now_s() >= poll_pause_until) {
                int w = con_wait(&con, to);
                if (w < 0) {
                    poll_pause_until = now_s() + 1.0;
                    sleep_s(to);
                } else if (w == 1) {
                    woke = 1;
                    double since = now_s() - now;           /* rate floor between snapshots */
                    if (since < floor_s)
                        sleep_s(floor_s - since);
                }
            } else {
                sleep_s(to);
            }
        } else {
            sleep_s(now + period - now_s());
        }
    }

done:
    free(out);
    free(plines);
    free(patts);
    con_close(&con);
    close(d.fd);
    return rc;
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(FILE *f)
{
    fputs("usage: usbdisplay-tty [options]\n"
          "Mirror a Linux virtual console to the Pico USB display (needs root).\n\n"
          "  -t, --tty N          virtual console number (default 1)\n"
          "  -p, --port DEV       serial port (default: auto-detect)\n"
          "      --colour MODE    auto | on | off: send ANSI colours (auto = only if the display announces them)\n"
          "      --no-resize      do not resize the console to the display size (it is cropped instead)\n"
          "\nframe rate (dynamic):\n"
          "      --fps-idle F     nothing changing        (default 2)\n"
          "      --fps-mid F      something changing      (default 24)\n"
          "      --fps-high F     a lot changing          (default 30; --fps is an alias)\n"
          "      --busy-rows N    changed rows per frame that count as 'a lot' (default 8)\n"
          "      --hold-mid S     seconds to stay at --fps-mid after the last change (default 1.0)\n"
          "      --hold-high S    seconds to stay at --fps-high after the last busy frame (default 0.5)\n"
          "      --no-poll        do not wait on /dev/vcsa with poll(); use the timer only\n"
          "      --refresh S      full redraw interval (default 5)\n"
          "  -v, --verbose        log frame-rate changes\n"
          "  -V, --version        print the version\n"
          "  -h, --help           this text\n",
          f);
}

static int parse_double(const char *s, double lo, double hi, double *out)
{
    char *end;
    double v = strtod(s, &end);
    if (end == s || *end || v < lo || v > hi)
        return -1;
    *out = v;
    return 0;
}

int main(int argc, char **argv)
{
    Args a = { .tty = 1, .port = NULL, .colour = -1, .resize = true, .fps_idle = 2, .fps_mid = 24, .fps_high = 30,
               .busy_rows = 8, .hold_mid = 1.0, .hold_high = 0.5, .refresh = 5.0 };
    static const struct option longopts[] = {
        { "tty", 1, 0, 't' }, { "port", 1, 0, 'p' }, { "colour", 1, 0, 'c' }, { "color", 1, 0, 'c' },
        { "no-resize", 0, 0, 'R' }, { "fps-idle", 1, 0, 1001 }, { "fps-mid", 1, 0, 1002 },
        { "fps-high", 1, 0, 1003 }, { "fps", 1, 0, 1003 }, { "busy-rows", 1, 0, 1004 },
        { "hold-mid", 1, 0, 1005 }, { "hold-high", 1, 0, 1006 }, { "vcsa", 1, 0, 1007 }, { "vcsu", 1, 0, 1008 },
        { "no-poll", 0, 0, 1009 }, { "refresh", 1, 0, 'r' }, { "verbose", 0, 0, 'v' }, { "version", 0, 0, 'V' },
        { "help", 0, 0, 'h' }, { 0, 0, 0, 0 }
    };
    int opt;
    double v;
    while ((opt = getopt_long(argc, argv, "t:p:r:vVh", longopts, NULL)) != -1) {
        switch (opt) {
        case 't': if (parse_double(optarg, 1, 63, &v)) goto bad; a.tty = (int)v; break;
        case 'p': a.port = optarg; break;
        case 'c':
            if (!strcmp(optarg, "auto")) a.colour = -1;
            else if (!strcmp(optarg, "on")) a.colour = 1;
            else if (!strcmp(optarg, "off")) a.colour = 0;
            else goto bad;
            break;
        case 'R': a.resize = false; break;
        case 1001: if (parse_double(optarg, 0.1, 240, &a.fps_idle)) goto bad; break;
        case 1002: if (parse_double(optarg, 0.1, 240, &a.fps_mid)) goto bad; break;
        case 1003: if (parse_double(optarg, 0.1, 240, &a.fps_high)) goto bad; break;
        case 1004: if (parse_double(optarg, 1, 255, &v)) goto bad; a.busy_rows = (int)v; break;
        case 1005: if (parse_double(optarg, 0, 3600, &a.hold_mid)) goto bad; break;
        case 1006: if (parse_double(optarg, 0, 3600, &a.hold_high)) goto bad; break;
        case 1007: a.vcsa = optarg; break;
        case 1008: a.vcsu = optarg; break;
        case 1009: a.no_poll = true; break;
        case 'r': if (parse_double(optarg, 0.2, 86400, &a.refresh)) goto bad; break;
        case 'v': g_verbose = true; break;
        case 'V': puts("usbdisplay-tty " VERSION); return 0;
        case 'h': usage(stdout); return 0;
        default: goto bad;
        }
    }
    if (optind < argc)
        goto bad;

    for (int i = 0; i < 256; i++)
        t256[i] = to_ascii((uint32_t)i);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;                  /* no SA_RESTART: poll()/nanosleep() must return */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    while (!g_stop) {
        if (session(&a) != 0 && !g_stop) {
            logmsg("retrying in 2 s");
            for (int i = 0; i < 20 && !g_stop; i++)
                sleep_s(0.1);
        }
    }
    return 0;

bad:
    usage(stderr);
    return 2;
}
