/*
  SDL2 "terminal" video driver.

  Renders an SDL window's software framebuffer into a text terminal using the
  Kitty graphics protocol, and (later) translates terminal input back into SDL
  events. Modeled on the "dummy" video driver. This lets ANY pure-SDL2 app run
  inside a Kitty-graphics-capable terminal (e.g. Ubiquitty) via
  SDL_VIDEODRIVER=terminal.

  Part of the terminal-dosbox-x project. Currently lives in the dummy/ dir so it
  is picked up by SDL2's existing per-file configure glob for that directory
  without build-system surgery; it will move to its own src/video/terminal/ dir
  once the pipeline is proven.

  This file is compiled whenever the dummy driver is enabled.
*/
#include "../../SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_DUMMY

#include "SDL_video.h"
#include "SDL_mouse.h"
#include "SDL_hints.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"
#include "../../events/SDL_keyboard_c.h"
#include "../../events/SDL_mouse_c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <signal.h>

#define TERMINALVID_DRIVER_NAME "terminal"
#define TERMINAL_SURFACE_DATA   "_SDL_TerminalSurface"

/* --- driver-global terminal state (single terminal per process) --- */
typedef struct
{
    int outfd;               /* where Kitty escapes are written */
    int infd;                /* where terminal input is read */
    SDL_bool own_outfd;      /* close outfd on quit */
    SDL_bool own_infd;       /* close infd on quit */
    SDL_bool is_tty;         /* outfd is an interactive tty */
    SDL_bool raw_active;     /* termios raw mode installed */
    struct termios saved_termios;

    Uint8 *rgb;              /* scratch: tightly packed RGB frame */
    size_t rgb_cap;
    char *b64;               /* scratch: base64 of rgb */
    size_t b64_cap;

    SDL_bool active;         /* terminal is in raw/alt-screen mode (needs restore) */
    Uint32 image_seq;        /* monotonic kitty image id (fresh per frame) */
    Uint32 prev_image_id;    /* previous frame's image id, to delete */

    SDL_Window *window;      /* window to target with injected mouse events */
    int img_cols, img_rows;  /* last frame's placement cell span (c,r) */
    int img_w, img_h;        /* last frame's image pixel size (surface w,h) */
    int cell_w, cell_h;      /* terminal cell pixel size (from CSI 16t; default 8x16) */
    int mod_shift, mod_ctrl, mod_alt; /* injected modifier state (kitty input) */
} TERMINAL_State;

static TERMINAL_State term;

/* ------------------------------------------------------------------ */
/* low-level I/O helpers                                              */
/* ------------------------------------------------------------------ */

static void term_write(const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = write(term.outfd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        p += n;
        len -= (size_t)n;
    }
}

static void term_write_str(const char *s)
{
    term_write(s, strlen(s));
}

/* base64 encode src[0..len) into dst; returns encoded length. dst must hold
   4*ceil(len/3) bytes. */
static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t term_base64(const Uint8 *src, size_t len, char *dst)
{
    size_t i, o = 0;
    for (i = 0; i + 2 < len; i += 3) {
        Uint32 v = ((Uint32)src[i] << 16) | ((Uint32)src[i + 1] << 8) | src[i + 2];
        dst[o++] = b64tab[(v >> 18) & 0x3f];
        dst[o++] = b64tab[(v >> 12) & 0x3f];
        dst[o++] = b64tab[(v >> 6) & 0x3f];
        dst[o++] = b64tab[v & 0x3f];
    }
    if (i < len) {
        Uint32 v = (Uint32)src[i] << 16;
        int rem = (int)(len - i); /* 1 or 2 */
        if (rem == 2)
            v |= (Uint32)src[i + 1] << 8;
        dst[o++] = b64tab[(v >> 18) & 0x3f];
        dst[o++] = b64tab[(v >> 12) & 0x3f];
        dst[o++] = (rem == 2) ? b64tab[(v >> 6) & 0x3f] : '=';
        dst[o++] = '=';
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* terminal lifecycle                                                */
/* ------------------------------------------------------------------ */

static void term_open_io(void)
{
    const char *outpath = SDL_getenv("SDL_TERMINAL_OUT");
    const char *inpath = SDL_getenv("SDL_TERMINAL_IN");

    term.outfd = -1;
    term.infd = -1;
    term.own_outfd = SDL_FALSE;
    term.own_infd = SDL_FALSE;

    if (outpath && *outpath) {
        term.outfd = open(outpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        term.own_outfd = (term.outfd >= 0) ? SDL_TRUE : SDL_FALSE;
    }
    if (term.outfd < 0) {
        int fd = open("/dev/tty", O_RDWR | O_NOCTTY);
        if (fd >= 0) {
            term.outfd = fd;
            term.own_outfd = SDL_TRUE;
            /* Deliberately NOT using this /dev/tty fd for input: on macOS/BSD
               poll()/select() on a fd opened from /dev/tty returns POLLNVAL
               (it is a redirector device, not a real tty vnode) — reads/writes
               work but poll never reports POLLIN. Input is read from STDIN (a
               real tty fd) below; termios is per-tty so raw mode still applies. */
        }
    }
    if (term.outfd < 0)
        term.outfd = STDOUT_FILENO;

    if (inpath && *inpath) {
        term.infd = open(inpath, O_RDONLY);
        term.own_infd = (term.infd >= 0) ? SDL_TRUE : SDL_FALSE;
    }
    if (term.infd < 0)
        term.infd = STDIN_FILENO;

    term.is_tty = isatty(term.outfd) ? SDL_TRUE : SDL_FALSE;
}

/* Idempotent restore of terminal STATE (escapes + termios). Safe to call from
   a signal handler (write/tcsetattr) and from atexit. Does not touch fds. */
static void term_restore(void)
{
    if (!term.active)
        return;
    term.active = SDL_FALSE;
    /* pop kitty keyboard flags, delete all kitty images (q=2: no ACK), show
       cursor, leave alt screen */
    term_write_str("\x1b[?1003;1006;1016l" /* disable mouse reporting */
                   "\x1b[<u\x1b_Ga=d,q=2\x1b\\\x1b[?25h\x1b[?1049l");
    if (term.raw_active) {
        tcsetattr(term.infd, TCSANOW, &term.saved_termios);
        term.raw_active = SDL_FALSE;
    }
}

static void term_atexit(void)
{
    term_restore();
}

static void term_signal_handler(int sig)
{
    term_restore();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void term_enter(void)
{
    if (term.is_tty) {
        struct termios raw;
        if (tcgetattr(term.infd, &term.saved_termios) == 0) {
            raw = term.saved_termios;
            raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
            raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
            raw.c_oflag &= ~(OPOST);
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            if (tcsetattr(term.infd, TCSANOW, &raw) == 0)
                term.raw_active = SDL_TRUE;
        }
    }
    /* alt screen + hide cursor, then push kitty keyboard flags: 1 disambiguate
       + 2 report-event-types (press/release -> real key-hold) + 8 report-all-
       keys (every key, incl. modifiers, as CSI-u). Terminals without kitty
       keyboard ignore it and we fall back to legacy byte input. */
    term_write_str("\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H\x1b[>11u"
                   "\x1b[?1003;1006;1016h" /* mouse: any-motion + SGR + SGR-pixel */
                   "\x1b[16t");            /* query cell pixel size for mouse mapping */
    term.active = SDL_TRUE;

    /* Guarantee terminal restore on any exit path: clean quit (VideoQuit),
       process exit (atexit), and fatal/async signals (window close = SIGHUP,
       kill = SIGTERM, crash = SIGSEGV). Without this a killed process leaves
       the terminal in raw mode with the cursor hidden and on the alt screen. */
    atexit(term_atexit);
    signal(SIGINT, term_signal_handler);
    signal(SIGTERM, term_signal_handler);
    signal(SIGHUP, term_signal_handler);
    signal(SIGQUIT, term_signal_handler);
    signal(SIGSEGV, term_signal_handler);
    signal(SIGBUS, term_signal_handler);
    signal(SIGILL, term_signal_handler);
    signal(SIGFPE, term_signal_handler);
    signal(SIGABRT, term_signal_handler);
}

static void term_leave(void)
{
    term_restore();
    if (term.own_outfd && term.outfd >= 0)
        close(term.outfd);
    if (term.own_infd && term.infd >= 0)
        close(term.infd);
    term.outfd = term.infd = -1;
}

/* ------------------------------------------------------------------ */
/* framebuffer -> Kitty                                              */
/* ------------------------------------------------------------------ */

/* Compute the cell box (cols x rows) to place a WxH image into the terminal.
   ALWAYS returns an explicit cell span (like libvaxis) — a kitty placement
   without c/r may not be drawn. When the terminal reports pixel dimensions
   (ws_xpixel/ws_ypixel) we aspect-fit; otherwise we fall back to filling the
   character grid (stretched, but visible). Returns SDL_FALSE only if even the
   grid size is unknown. */
static SDL_bool term_fit_cells(int W, int H, int *out_cols, int *out_rows)
{
    struct winsize ws;
    double cell_w, cell_h, img_ar, term_ar, tw, th;
    int cols, rows;

    if (ioctl(term.outfd, TIOCGWINSZ, &ws) != 0)
        return SDL_FALSE;
    if (ws.ws_col == 0 || ws.ws_row == 0)
        return SDL_FALSE;

    if (ws.ws_xpixel == 0 || ws.ws_ypixel == 0) {
        /* No pixel geometry (Ubiquitty reports 0 here). Fill the grid so the
           image is at least visible; aspect correction comes later via a
           CSI 16t cell-size query. */
        *out_cols = ws.ws_col;
        *out_rows = ws.ws_row;
        return SDL_TRUE;
    }

    cell_w = (double)ws.ws_xpixel / ws.ws_col;
    cell_h = (double)ws.ws_ypixel / ws.ws_row;
    img_ar = (double)W / (double)H;
    term_ar = (double)ws.ws_xpixel / (double)ws.ws_ypixel;

    if (term_ar > img_ar) { /* terminal wider than image: fit height */
        th = ws.ws_ypixel;
        tw = th * img_ar;
    } else { /* fit width */
        tw = ws.ws_xpixel;
        th = tw / img_ar;
    }
    cols = (int)(tw / cell_w);
    rows = (int)(th / cell_h);
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    *out_cols = cols;
    *out_rows = rows;
    return SDL_TRUE;
}

static void term_emit_frame(SDL_Surface *surface)
{
    int W = surface->w;
    int H = surface->h;
    size_t rgb_len = (size_t)W * (size_t)H * 3u;
    const size_t CHUNK = 4096;
    size_t b64_max, b64_len, off;
    Uint8 *dst;
    int x, y, cols = 0, rows = 0, len;
    SDL_bool have_fit, first;
    char hdr[128];
    Uint32 id;

    if (rgb_len > term.rgb_cap) {
        Uint8 *nb = (Uint8 *)SDL_realloc(term.rgb, rgb_len);
        if (!nb)
            return;
        term.rgb = nb;
        term.rgb_cap = rgb_len;
    }

    /* pack XRGB8888 (SDL_PIXELFORMAT_RGB888) -> tight RGB */
    dst = term.rgb;
    for (y = 0; y < H; ++y) {
        const Uint32 *row = (const Uint32 *)((const Uint8 *)surface->pixels + (size_t)y * surface->pitch);
        for (x = 0; x < W; ++x) {
            Uint32 p = row[x];
            *dst++ = (Uint8)((p >> 16) & 0xff); /* R */
            *dst++ = (Uint8)((p >> 8) & 0xff);  /* G */
            *dst++ = (Uint8)(p & 0xff);         /* B */
        }
    }

    b64_max = 4 * ((rgb_len + 2) / 3);
    if (b64_max > term.b64_cap) {
        char *nb = (char *)SDL_realloc(term.b64, b64_max);
        if (!nb)
            return;
        term.b64 = nb;
        term.b64_cap = b64_max;
    }
    b64_len = term_base64(term.rgb, rgb_len, term.b64);

    have_fit = term_fit_cells(W, H, &cols, &rows);

    /* Remember the placement geometry for the mouse coordinate transform. */
    term.img_w = W;
    term.img_h = H;
    if (have_fit) {
        term.img_cols = cols;
        term.img_rows = rows;
    }

    /* Fresh image id every frame, deleting the previous one. Re-using one id
       for video is valid kitty and works on spec-compliant terminals (kitty,
       Ghostty), but some terminals cache the GPU texture by (id,dimensions,
       format) and never re-upload changed pixels for the same id — freezing on
       frame 1. Cycling the id (as libvaxis/notcurses do) forces a fresh upload
       on every terminal. C=1 keeps the cursor from advancing (else a
       full-height image scrolls itself off the screen). */
    term.image_seq++;
    if (term.image_seq == 0)
        term.image_seq = 1;
    id = term.image_seq;

    term_write_str("\x1b[H");

    off = 0;
    first = SDL_TRUE;
    do {
        size_t n = (b64_len - off > CHUNK) ? CHUNK : (b64_len - off);
        int last = (off + n >= b64_len);
        int m = last ? 0 : 1;
        if (first) {
            if (have_fit) {
                len = SDL_snprintf(hdr, sizeof(hdr),
                                   "\x1b_Gq=2,a=T,f=24,s=%d,v=%d,i=%u,C=1,c=%d,r=%d,m=%d;",
                                   W, H, (unsigned)id, cols, rows, m);
            } else {
                len = SDL_snprintf(hdr, sizeof(hdr),
                                   "\x1b_Gq=2,a=T,f=24,s=%d,v=%d,i=%u,C=1,m=%d;",
                                   W, H, (unsigned)id, m);
            }
            first = SDL_FALSE;
        } else {
            len = SDL_snprintf(hdr, sizeof(hdr), "\x1b_Gm=%d;", m);
        }
        term_write(hdr, (size_t)len);
        if (n > 0)
            term_write(term.b64 + off, n);
        term_write("\x1b\\", 2);
        off += n;
    } while (off < b64_len);

    /* Delete the previous frame's image so slots/textures don't accumulate.
       q=2 suppresses the terminal's per-delete "OK" acknowledgement; without it
       the terminal streams an APC reply for every frame, which piles up in our
       input and spews to the shell on exit. */
    if (term.prev_image_id != 0) {
        len = SDL_snprintf(hdr, sizeof(hdr), "\x1b_Ga=d,d=I,i=%u,q=2\x1b\\", (unsigned)term.prev_image_id);
        term_write(hdr, (size_t)len);
    }
    term.prev_image_id = id;
}

/* ------------------------------------------------------------------ */
/* SDL video device hooks                                            */
/* ------------------------------------------------------------------ */

static int TERMINAL_CreateWindowFramebuffer(_THIS, SDL_Window *window, Uint32 *format, void **pixels, int *pitch);
static int TERMINAL_UpdateWindowFramebuffer(_THIS, SDL_Window *window, const SDL_Rect *rects, int numrects);
static void TERMINAL_DestroyWindowFramebuffer(_THIS, SDL_Window *window);
static void TERMINAL_PumpEvents(_THIS);

static int TERMINAL_VideoInit(_THIS)
{
    SDL_DisplayMode mode;
    struct winsize ws;
    int w = 640, h = 480;

    term.cell_w = 8; /* sane defaults until the CSI 16t reply arrives */
    term.cell_h = 16;
    term.mod_shift = term.mod_ctrl = term.mod_alt = 0;
    term_open_io();
    term_enter();

    /* Report a display sized to the terminal's pixel box when known, so apps
       that query the desktop size get something sensible. */
    if (ioctl(term.outfd, TIOCGWINSZ, &ws) == 0 && ws.ws_xpixel && ws.ws_ypixel) {
        w = ws.ws_xpixel;
        h = ws.ws_ypixel;
    }

    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_RGB888;
    mode.w = w;
    mode.h = h;
    mode.refresh_rate = 60;
    mode.driverdata = NULL;
    if (SDL_AddBasicVideoDisplay(&mode) < 0)
        return -1;
    SDL_AddDisplayMode(&_this->displays[0], &mode);
    return 0;
}

static void TERMINAL_VideoQuit(_THIS)
{
    (void)_this;
    term_leave();
    SDL_free(term.rgb);
    SDL_free(term.b64);
    term.rgb = NULL;
    term.b64 = NULL;
    term.rgb_cap = term.b64_cap = 0;
}

static int TERMINAL_CreateWindowFramebuffer(_THIS, SDL_Window *window, Uint32 *format, void **pixels, int *pitch)
{
    SDL_Surface *surface;
    const Uint32 surface_format = SDL_PIXELFORMAT_RGB888;
    int w, h;

    TERMINAL_DestroyWindowFramebuffer(_this, window);

    SDL_GetWindowSizeInPixels(window, &w, &h);
    surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 0, surface_format);
    if (!surface)
        return -1;

    SDL_SetWindowData(window, TERMINAL_SURFACE_DATA, surface);
    term.window = window; /* target for injected mouse events */
    *format = surface_format;
    *pixels = surface->pixels;
    *pitch = surface->pitch;
    return 0;
}

static int TERMINAL_UpdateWindowFramebuffer(_THIS, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    SDL_Surface *surface;
    (void)_this;
    (void)rects;
    (void)numrects;

    surface = (SDL_Surface *)SDL_GetWindowData(window, TERMINAL_SURFACE_DATA);
    if (!surface)
        return SDL_SetError("Couldn't find terminal surface for window");

    term_emit_frame(surface);
    return 0;
}

static void TERMINAL_DestroyWindowFramebuffer(_THIS, SDL_Window *window)
{
    SDL_Surface *surface;
    (void)_this;
    surface = (SDL_Surface *)SDL_SetWindowData(window, TERMINAL_SURFACE_DATA, NULL);
    SDL_FreeSurface(surface);
}

/* ------------------------------------------------------------------ */
/* terminal input -> SDL key events                                  */
/* ------------------------------------------------------------------ */

/* Map a printable/control ASCII byte to an SDL scancode; sets *shift when the
   base key must be shifted to produce that character. SDL_SCANCODE_UNKNOWN for
   bytes we don't handle. */
static SDL_Scancode ascii_to_scancode(unsigned char c, int *shift)
{
    *shift = 0;
    if (c >= 'a' && c <= 'z')
        return (SDL_Scancode)(SDL_SCANCODE_A + (c - 'a'));
    if (c >= 'A' && c <= 'Z') {
        *shift = 1;
        return (SDL_Scancode)(SDL_SCANCODE_A + (c - 'A'));
    }
    if (c >= '1' && c <= '9')
        return (SDL_Scancode)(SDL_SCANCODE_1 + (c - '1'));
    switch (c) {
    case '0': return SDL_SCANCODE_0;
    case ' ': return SDL_SCANCODE_SPACE;
    case '\r': case '\n': return SDL_SCANCODE_RETURN;
    case '\t': return SDL_SCANCODE_TAB;
    case 0x7f: case 0x08: return SDL_SCANCODE_BACKSPACE;
    case '!': *shift = 1; return SDL_SCANCODE_1;
    case '@': *shift = 1; return SDL_SCANCODE_2;
    case '#': *shift = 1; return SDL_SCANCODE_3;
    case '$': *shift = 1; return SDL_SCANCODE_4;
    case '%': *shift = 1; return SDL_SCANCODE_5;
    case '^': *shift = 1; return SDL_SCANCODE_6;
    case '&': *shift = 1; return SDL_SCANCODE_7;
    case '*': *shift = 1; return SDL_SCANCODE_8;
    case '(': *shift = 1; return SDL_SCANCODE_9;
    case ')': *shift = 1; return SDL_SCANCODE_0;
    case '-': return SDL_SCANCODE_MINUS;
    case '_': *shift = 1; return SDL_SCANCODE_MINUS;
    case '=': return SDL_SCANCODE_EQUALS;
    case '+': *shift = 1; return SDL_SCANCODE_EQUALS;
    case '[': return SDL_SCANCODE_LEFTBRACKET;
    case '{': *shift = 1; return SDL_SCANCODE_LEFTBRACKET;
    case ']': return SDL_SCANCODE_RIGHTBRACKET;
    case '}': *shift = 1; return SDL_SCANCODE_RIGHTBRACKET;
    case '\\': return SDL_SCANCODE_BACKSLASH;
    case '|': *shift = 1; return SDL_SCANCODE_BACKSLASH;
    case ';': return SDL_SCANCODE_SEMICOLON;
    case ':': *shift = 1; return SDL_SCANCODE_SEMICOLON;
    case '\'': return SDL_SCANCODE_APOSTROPHE;
    case '"': *shift = 1; return SDL_SCANCODE_APOSTROPHE;
    case '`': return SDL_SCANCODE_GRAVE;
    case '~': *shift = 1; return SDL_SCANCODE_GRAVE;
    case ',': return SDL_SCANCODE_COMMA;
    case '<': *shift = 1; return SDL_SCANCODE_COMMA;
    case '.': return SDL_SCANCODE_PERIOD;
    case '>': *shift = 1; return SDL_SCANCODE_PERIOD;
    case '/': return SDL_SCANCODE_SLASH;
    case '?': *shift = 1; return SDL_SCANCODE_SLASH;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}

/* Inject a key as press+release (a tap). Held keys arrive as terminal
   auto-repeat (repeated bytes) until the kitty keyboard protocol lands, which
   will give real press/release for game key-holds. */
static void term_send_key(SDL_Scancode sc, int shift)
{
    if (sc == SDL_SCANCODE_UNKNOWN)
        return;
    if (shift)
        SDL_SendKeyboardKey(SDL_PRESSED, SDL_SCANCODE_LSHIFT);
    SDL_SendKeyboardKey(SDL_PRESSED, sc);
    SDL_SendKeyboardKey(SDL_RELEASED, sc);
    if (shift)
        SDL_SendKeyboardKey(SDL_RELEASED, SDL_SCANCODE_LSHIFT);
}

/* ESC [ <num> ~  -> nav / function keys */
static SDL_Scancode term_csi_tilde(int num)
{
    switch (num) {
    case 1: case 7: return SDL_SCANCODE_HOME;
    case 2: return SDL_SCANCODE_INSERT;
    case 3: return SDL_SCANCODE_DELETE;
    case 4: case 8: return SDL_SCANCODE_END;
    case 5: return SDL_SCANCODE_PAGEUP;
    case 6: return SDL_SCANCODE_PAGEDOWN;
    case 11: return SDL_SCANCODE_F1;
    case 12: return SDL_SCANCODE_F2;
    case 13: return SDL_SCANCODE_F3;
    case 14: return SDL_SCANCODE_F4;
    case 15: return SDL_SCANCODE_F5;
    case 17: return SDL_SCANCODE_F6;
    case 18: return SDL_SCANCODE_F7;
    case 19: return SDL_SCANCODE_F8;
    case 20: return SDL_SCANCODE_F9;
    case 21: return SDL_SCANCODE_F10;
    case 23: return SDL_SCANCODE_F11;
    case 24: return SDL_SCANCODE_F12;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}

/* ESC [ <letter>  -> arrows / home / end */
static SDL_Scancode term_csi_final(unsigned char f)
{
    switch (f) {
    case 'A': return SDL_SCANCODE_UP;
    case 'B': return SDL_SCANCODE_DOWN;
    case 'C': return SDL_SCANCODE_RIGHT;
    case 'D': return SDL_SCANCODE_LEFT;
    case 'H': return SDL_SCANCODE_HOME;
    case 'F': return SDL_SCANCODE_END;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}

/* ESC O <letter>  -> F1..F4 / home / end */
static SDL_Scancode term_ss3_final(unsigned char f)
{
    switch (f) {
    case 'P': return SDL_SCANCODE_F1;
    case 'Q': return SDL_SCANCODE_F2;
    case 'R': return SDL_SCANCODE_F3;
    case 'S': return SDL_SCANCODE_F4;
    case 'H': return SDL_SCANCODE_HOME;
    case 'F': return SDL_SCANCODE_END;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}

/* Map a kitty keyboard codepoint (the unshifted key, or a functional-key value
   from the Unicode private-use area) to an SDL scancode. Modifiers arrive as
   their own key events (report-all-keys), so shift is NOT applied here. */
static SDL_Scancode kitty_cp_to_scancode(unsigned int cp)
{
    if (cp >= 'a' && cp <= 'z')
        return (SDL_Scancode)(SDL_SCANCODE_A + (cp - 'a'));
    if (cp >= 'A' && cp <= 'Z')
        return (SDL_Scancode)(SDL_SCANCODE_A + (cp - 'A'));
    if (cp >= '1' && cp <= '9')
        return (SDL_Scancode)(SDL_SCANCODE_1 + (cp - '1'));
    if (cp >= 57364 && cp <= 57375)
        return (SDL_Scancode)(SDL_SCANCODE_F1 + (cp - 57364)); /* F1..F12 */
    switch (cp) {
    case '0': return SDL_SCANCODE_0;
    case ' ': return SDL_SCANCODE_SPACE;
    case 13: return SDL_SCANCODE_RETURN;
    case 9: return SDL_SCANCODE_TAB;
    case 27: return SDL_SCANCODE_ESCAPE;
    case 127: case 8: return SDL_SCANCODE_BACKSPACE;
    case '-': return SDL_SCANCODE_MINUS;
    case '=': return SDL_SCANCODE_EQUALS;
    case '[': return SDL_SCANCODE_LEFTBRACKET;
    case ']': return SDL_SCANCODE_RIGHTBRACKET;
    case '\\': return SDL_SCANCODE_BACKSLASH;
    case ';': return SDL_SCANCODE_SEMICOLON;
    case '\'': return SDL_SCANCODE_APOSTROPHE;
    case '`': return SDL_SCANCODE_GRAVE;
    case ',': return SDL_SCANCODE_COMMA;
    case '.': return SDL_SCANCODE_PERIOD;
    case '/': return SDL_SCANCODE_SLASH;
    case 57348: return SDL_SCANCODE_INSERT;
    case 57349: return SDL_SCANCODE_DELETE;
    case 57350: return SDL_SCANCODE_LEFT;
    case 57351: return SDL_SCANCODE_RIGHT;
    case 57352: return SDL_SCANCODE_UP;
    case 57353: return SDL_SCANCODE_DOWN;
    case 57354: return SDL_SCANCODE_PAGEUP;
    case 57355: return SDL_SCANCODE_PAGEDOWN;
    case 57356: return SDL_SCANCODE_HOME;
    case 57357: return SDL_SCANCODE_END;
    case 57358: return SDL_SCANCODE_CAPSLOCK;
    case 57441: return SDL_SCANCODE_LSHIFT;
    case 57442: return SDL_SCANCODE_LCTRL;
    case 57443: return SDL_SCANCODE_LALT;
    case 57444: return SDL_SCANCODE_LGUI;
    case 57447: return SDL_SCANCODE_RSHIFT;
    case 57448: return SDL_SCANCODE_RCTRL;
    case 57449: return SDL_SCANCODE_RALT;
    case 57451: return SDL_SCANCODE_RGUI;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}

/* Scan a CSI sequence (buf[start] == '['). Extracts num0 (field-0 value: the
   key codepoint for a 'u' seq, or the number for a '~' seq), mods (field-1
   value, kitty = 1 + modifier bitmask), event (field-1 sub-1: 1 press, 2
   repeat, 3 release; default 1), and the final byte. Sets *is_mouse for an SGR
   mouse seq (ESC [ < ...). Returns bytes consumed from '[' (0 if incomplete). */
static int csi_scan(const unsigned char *buf, int start, int n,
                    int *num0, int *num1, int *num2, int *event,
                    unsigned char *final, int *is_mouse)
{
    int j = start + 1;
    int field = 0, sub = 0, seen = 0;
    unsigned int cur = 0;
    *num0 = 0; *num1 = 1; *num2 = 0; *event = 1; *final = 0; *is_mouse = 0;
    if (j < n && buf[j] == '<') { *is_mouse = 1; j++; }
    while (j < n) {
        unsigned char ch = buf[j];
        if (ch >= '0' && ch <= '9') {
            cur = cur * 10 + (unsigned int)(ch - '0');
            seen = 1;
            j++;
            continue;
        }
        if (seen) {
            if (field == 0 && sub == 0) *num0 = (int)cur;
            else if (field == 1 && sub == 0) *num1 = (int)cur;
            else if (field == 1 && sub == 1) *event = (int)cur;
            else if (field == 2 && sub == 0) *num2 = (int)cur;
        }
        cur = 0; seen = 0;
        if (ch == ':') { if (sub < 1) sub++; j++; }
        else if (ch == ';') { field++; sub = 0; j++; }
        else { *final = ch; j++; break; }
    }
    if (*final == 0)
        return 0;
    return j - start;
}

/* Map an SGR-pixel mouse report to the image rectangle and inject an SDL mouse
   event. b = SGR button byte, (px,py) = 1-based terminal pixel coords, final
   'M' = press/motion, 'm' = release. */
static void term_mouse(int b, int px, int py, unsigned char final)
{
    int rect_w, rect_h, mx, my, ix, iy;
    if (!term.window || term.img_w <= 0 || term.img_h <= 0)
        return;
    rect_w = term.img_cols * term.cell_w;
    rect_h = term.img_rows * term.cell_h;
    if (rect_w < 1) rect_w = 1;
    if (rect_h < 1) rect_h = 1;
    mx = px - 1; if (mx < 0) mx = 0;
    my = py - 1; if (my < 0) my = 0;
    ix = (int)((long)mx * term.img_w / rect_w);
    iy = (int)((long)my * term.img_h / rect_h);
    if (ix < 0) ix = 0; else if (ix >= term.img_w) ix = term.img_w - 1;
    if (iy < 0) iy = 0; else if (iy >= term.img_h) iy = term.img_h - 1;

    if (b & 64) { /* wheel */
        SDL_SendMouseWheel(term.window, 0, 0.0f, (b == 64) ? 1.0f : -1.0f, SDL_MOUSEWHEEL_NORMAL);
        return;
    }
    SDL_SendMouseMotion(term.window, 0, 0, ix, iy);
    if (!(b & 32)) { /* button press/release (bit 32 = pure motion) */
        Uint8 btn = (Uint8)(((b & 3) == 0) ? SDL_BUTTON_LEFT :
                            ((b & 3) == 1) ? SDL_BUTTON_MIDDLE : SDL_BUTTON_RIGHT);
        SDL_SendMouseButton(term.window, 0, (Uint8)((final == 'M') ? SDL_PRESSED : SDL_RELEASED), btn);
    }
}

/* Send a single press or release for a scancode (kitty event-based path). */
static void term_key_state(SDL_Scancode sc, int pressed)
{
    if (sc != SDL_SCANCODE_UNKNOWN)
        SDL_SendKeyboardKey((Uint8)(pressed ? SDL_PRESSED : SDL_RELEASED), sc);
}

/* Drive the injected shift/ctrl/alt state to the requested values, emitting the
   minimal press/release. Called from each key's modifier field AND from
   standalone modifier events, so shifted chars work whether or not the terminal
   reports modifiers as separate key events. */
static void term_apply_mods(int want_shift, int want_ctrl, int want_alt)
{
    if (want_shift != term.mod_shift) {
        term_key_state(SDL_SCANCODE_LSHIFT, want_shift);
        term.mod_shift = want_shift;
    }
    if (want_ctrl != term.mod_ctrl) {
        term_key_state(SDL_SCANCODE_LCTRL, want_ctrl);
        term.mod_ctrl = want_ctrl;
    }
    if (want_alt != term.mod_alt) {
        term_key_state(SDL_SCANCODE_LALT, want_alt);
        term.mod_alt = want_alt;
    }
}

/* Parse a batch of terminal input into SDL key events. Prefers the kitty
   keyboard protocol (CSI-u with press/release event types -> real key-hold);
   falls back to legacy escape sequences + printable bytes (tap) for terminals
   that don't support it. */
static void term_process_input(const unsigned char *buf, int n)
{
    int i = 0;
    while (i < n) {
        unsigned char c = buf[i];
        if (c == 0x03 || c == 0x04) { /* legacy Ctrl-C / Ctrl-D -> quit */
            SDL_SendQuit();
            return;
        }
        if (c == 0x1b) {
            if (i + 1 < n && buf[i + 1] == '[') {
                int num0 = 0, num1 = 1, num2 = 0, event = 1, consumed = 0, is_mouse = 0;
                unsigned char final = 0;
                SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
                consumed = csi_scan(buf, i + 1, n, &num0, &num1, &num2, &event, &final, &is_mouse);
                if (consumed == 0) /* split across reads: drop the partial */
                    return;
                if (is_mouse) { /* SGR mouse: b=num0, px=num1, py=num2 */
                    term_mouse(num0, num1, num2, final);
                    i += 1 + consumed;
                    continue;
                }
                if (final == 't') { /* CSI 16t reply [6;<h>;<w>t = cell pixel size */
                    if (num0 == 6 && num1 > 0 && num2 > 0) {
                        term.cell_h = num1;
                        term.cell_w = num2;
                    }
                    i += 1 + consumed;
                    continue;
                }
                if (final == 'u') {
                    int w = (event != 3);
                    /* Ctrl-C (kitty encoding) still quits. num1 = 1 + modifier
                       bitmask; ctrl bit = 4. */
                    if (num0 == 'c' && ((num1 - 1) & 4) && event != 3) {
                        SDL_SendQuit();
                        return;
                    }
                    /* Standalone modifier key events (report-all-keys). */
                    if (num0 == 57441 || num0 == 57447) { /* shift */
                        term_apply_mods(w, term.mod_ctrl, term.mod_alt);
                        i += 1 + consumed;
                        continue;
                    }
                    if (num0 == 57442 || num0 == 57448) { /* control */
                        term_apply_mods(term.mod_shift, w, term.mod_alt);
                        i += 1 + consumed;
                        continue;
                    }
                    if (num0 == 57443 || num0 == 57449) { /* alt */
                        term_apply_mods(term.mod_shift, term.mod_ctrl, w);
                        i += 1 + consumed;
                        continue;
                    }
                    sc = kitty_cp_to_scancode((unsigned int)num0);
                } else if (final == '~') {
                    sc = term_csi_tilde(num0);
                } else {
                    sc = term_csi_final(final); /* A/B/C/D/H/F arrows/home/end */
                }
                /* Sync modifiers from this key's mods field (num1 = 1 + bitmask:
                   bit0 shift, bit1 alt, bit2 ctrl), then inject the key — so
                   shifted chars work even without standalone modifier events. */
                {
                    int bm = num1 - 1;
                    term_apply_mods((bm & 1) != 0, (bm & 4) != 0, (bm & 2) != 0);
                }
                term_key_state(sc, event != 3); /* event 3 = release */
                i += 1 + consumed;
                continue;
            }
            if (i + 2 < n && buf[i + 1] == 'O') {
                term_send_key(term_ss3_final(buf[i + 2]), 0); /* legacy F1-F4 tap */
                i += 3;
                continue;
            }
            term_send_key(SDL_SCANCODE_ESCAPE, 0); /* lone ESC */
            i += 1;
            continue;
        }
        {
            int shift = 0;
            SDL_Scancode sc = ascii_to_scancode(c, &shift);
            if (sc != SDL_SCANCODE_UNKNOWN)
                term_send_key(sc, shift); /* legacy plain byte -> tap */
        }
        i++;
    }
}

static void TERMINAL_PumpEvents(_THIS)
{
    unsigned char buf[512];
    ssize_t n;
    struct pollfd pfd;
    (void)_this;
    if (term.infd < 0)
        return;
    pfd.fd = term.infd;
    pfd.events = POLLIN;
    /* Fully drain input each pump; poll() with a zero timeout gates each read
       so we never block (also works for an inherited blocking STDIN). */
    while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        n = read(term.infd, buf, sizeof(buf));
        if (n <= 0)
            break;
        term_process_input(buf, (int)n);
    }
}

/* ------------------------------------------------------------------ */
/* bootstrap                                                         */
/* ------------------------------------------------------------------ */

static int TERMINAL_Available(void)
{
    const char *envr = SDL_GetHint(SDL_HINT_VIDEODRIVER);
    return (envr && SDL_strcmp(envr, TERMINALVID_DRIVER_NAME) == 0) ? 1 : 0;
}

static void TERMINAL_DeleteDevice(SDL_VideoDevice *device)
{
    SDL_free(device);
}

static SDL_VideoDevice *TERMINAL_CreateDevice(void)
{
    SDL_VideoDevice *device;

    if (!TERMINAL_Available())
        return NULL;

    device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        SDL_OutOfMemory();
        return NULL;
    }

    device->VideoInit = TERMINAL_VideoInit;
    device->VideoQuit = TERMINAL_VideoQuit;
    device->PumpEvents = TERMINAL_PumpEvents;
    device->CreateWindowFramebuffer = TERMINAL_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = TERMINAL_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = TERMINAL_DestroyWindowFramebuffer;
    device->free = TERMINAL_DeleteDevice;

    return device;
}

VideoBootStrap TERMINAL_bootstrap = {
    TERMINALVID_DRIVER_NAME, "SDL terminal (Kitty graphics) video driver",
    TERMINAL_CreateDevice,
    NULL /* no ShowMessageBox */
};

#endif /* SDL_VIDEO_DRIVER_DUMMY */

/* vi: set ts=4 sw=4 expandtab: */
