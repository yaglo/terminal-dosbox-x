/*
  SDL2 "terminal" video driver.

  Renders an SDL window's software framebuffer into a text terminal using the
  Kitty graphics protocol, and (later) translates terminal input back into SDL
  events. Modeled on the "dummy" video driver. This lets ANY pure-SDL2 app run
  inside a Kitty-graphics-capable terminal (e.g. Ubiquitty) via
  SDL_VIDEODRIVER=terminal.

  Part of the terminal-dosbox-x project. It lives in the dummy/ dir so it is
  picked up by SDL2's existing per-file configure glob (src/video/dummy/*.c) and
  compiled whenever the dummy driver is enabled. Moving it to its own
  src/video/terminal/ dir is deferred deliberately: SDL2's `configure` is a
  tracked, generated file, so relocating would require regenerating it (a large,
  autoconf-version-dependent diff to vendored third-party code) for no functional
  gain. Do that only as part of an upstreaming effort.

  This file is compiled whenever the dummy driver is enabled.

  Environment variables (all optional):

    SDL_VIDEODRIVER=terminal
        Select this driver. Required to activate it.

    SDL_TERMINAL_ASPECT=<spec>
        Force the display aspect the frame is letterboxed to. Accepts a "W:H"
        ratio ("4:3", "16:9", "5:4") or a decimal ("1.6"). Unset or unparseable
        => native: the image's own pixel ratio when the terminal reports pixel
        geometry, otherwise fill the character grid. DOS content was drawn for
        4:3 CRTs, so the `dos` launcher defaults this to "4:3". Implemented in
        term_fit_cells(): it picks the c x r cell box whose pixel aspect matches
        <spec>, using the cell size from TIOCGWINSZ when the terminal reports it
        else the CSI 16t reply (default 8x16) — so it works even in terminals
        (Ubiquitty) that report ws_xpixel/ypixel = 0.

    SDL_TERMINAL_FULLSCREEN=1
        Opt in to Ubiquitty's fullscreen-content mode (private DECSET 2501): the
        terminal zeroes its padding, aspect-CONTAIN-fits the frame edge-to-edge,
        and paints a solid-black letterbox. The driver transmits the frame at its
        NATIVE resolution and drops its own c/r cell box (the terminal ignores it
        in 2501) — all scaling is the terminal's. Note: the terminal fits to the
        frame's PIXEL ratio, so DOS's non-square pixels show at that ratio (e.g.
        320x200 -> 8:5) unless the aspect is corrected upstream (the terminal's
        display-aspect knob, or DOSBox's own aspect correction). OFF by default;
        the terminal must implement 2501 or the frame won't fill the window.

    SDL_TERMINAL_ZLIB=0/1
        Force zlib payload compression (kitty o=z) off/on; auto-detected via a
        startup capability probe by default. Deflates the frame before base64,
        cutting wire bytes for flat DOS content. libz is dlopen'd at runtime (no
        link dependency). Only applies to the base64 transport — under file or
        shm transport the pty payload is just a short path/name either way, so
        compression buys no wire savings, just the deflate CPU cost; it's
        skipped there regardless of this setting.
        Independent of the always-on skip-unchanged-frame optimization, which
        drops byte-identical re-renders entirely.

    SDL_TERMINAL_XFER=file|shm
        Force kitty file transport (t=t) or shared-memory transport (t=s).
        Both skip per-frame pixel base64 over the pty — file writes the frame to
        a temp file and sends only its (base64) path (the ~megabyte frame
        travels through the filesystem page cache instead); shm writes it into a
        POSIX shm object and sends only its name (no filesystem at all). Both are
        AUTO-DETECTED by default via a startup capability probe (a real 1x1
        frame sent over t=t / t=s; only an ACK from the terminal enables it), so
        you normally don't need to set this — it exists to force a choice (e.g.
        override a probe false-negative) or force base64 (any other value, e.g.
        "0"). shm wins over file when both probe true (see term_emit_frame).
        File transport composes with SDL_TERMINAL_ZLIB (the temp file is then
        zlib-compressed); shm does not (memory isn't a wire, nothing to shrink).
        Temp files go under $TMPDIR (or /tmp), named ubterm-*; shm objects are
        named /ubPID-N; the terminal deletes both after reading, and the driver
        bounds/cleans its own trail as a safety net either way.

    SDL_TERMINAL_OUT=<path>
        Write the Kitty/escape stream to <path> instead of /dev/tty. For debug
        capture; opened O_WRONLY|O_CREAT|O_TRUNC (a regular file), so terminal
        window-size queries do not work through it.

    SDL_TERMINAL_IN=<path>
        Read terminal input from <path> instead of STDIN.
*/
#include "../../SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_DUMMY

#include "SDL_video.h"
#include "SDL_mouse.h"
#include "SDL_hints.h"
#include "SDL_timer.h"
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
#include <sys/mman.h>
#include <poll.h>
#include <signal.h>
#include <dlfcn.h>

#define TERMINALVID_DRIVER_NAME "terminal"
#define TERMINAL_SURFACE_DATA   "_SDL_TerminalSurface"
#define TERM_TMP_RING 8          /* in-flight temp files tracked for cleanup (pow2) */
/* Kitty image ids are unsigned 32-bit per spec, but Ubiquitty's parser reads
   `i=` into a SIGNED int32 and clamps on overflow (confirmed: an id above
   INT32_MAX comes back in the ACK as 2147483647/0x7FFFFFFF regardless of what
   was sent — silently breaking capability detection since neither probe's
   real id ever appears in its own reply). Keep these below INT32_MAX so they
   round-trip on any client, spec-compliant or not; still high enough to be
   very unlikely to collide with a real app's own image ids. */
#define TERM_PROBE_FILE_ID 2147418113u /* 0x7FFF0001: file-transport (t=t) probe image */
#define TERM_PROBE_ZLIB_ID 2147418114u /* 0x7FFF0002: zlib (o=z) probe image */
#define TERM_PROBE_SHM_ID  2147418115u /* 0x7FFF0003: shm-transport (t=s) probe image */

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
    int win_w, win_h;        /* text-area pixel size (from CSI 14t); 0 if unknown.
                                Gives a PRECISE fractional cell advance (win/cols)
                                for the mouse map — CSI 16t rounds cell size to an
                                integer, which drifts under a non-integer grid. */
    int mod_shift, mod_ctrl, mod_alt; /* injected modifier state (kitty input) */
    double forced_ar;        /* forced display aspect (SDL_TERMINAL_ASPECT); 0 = native */

    /* A whole frame is built into `out` then written in one blocking flush, so
       the kitty escape reaches the terminal as few large writes instead of many
       small ones. (Output goes to /dev/tty, which cannot be polled for
       writability on macOS — POLLNVAL — and stdout is the logfile, so true
       non-blocking output would need a writer thread; blocking gives natural
       back-pressure and is the proven path.) */
    Uint8 *out;
    size_t out_cap, out_len, out_off;

    /* Skip-unchanged-frame: the last frame's packed RGB, to detect and drop
       identical re-renders (static DOS screens) without touching the wire. A
       bounded streak forces a periodic resend so the image can't desync. */
    Uint8 *last;
    size_t last_len, last_cap;
    int unchanged_streak;

    /* Optional zlib payload compression (kitty o=z), loaded via dlopen so no
       link dependency is forced on SDL apps. NULL/0 unless SDL_TERMINAL_ZLIB
       opted in and libz resolved. */
    SDL_bool use_zlib;
    void *zlib_handle;
    unsigned long (*z_compressBound)(unsigned long);
    int (*z_compress2)(Uint8 *, unsigned long *, const Uint8 *, unsigned long, int);
    Uint8 *zbuf;
    size_t zbuf_cap;

    /* Opt-in kitty file transport (t=t): SDL_TERMINAL_XFER=file. Writes the raw
       frame to a temp file and sends only its path — no per-frame pixel base64,
       and the frame bypasses the pty. Needs terminal t=t support (Ubiquitty:
       requires a FileReadClient). tmp_ring tracks recent paths for exit cleanup
       (the terminal deletes them under t=t; the ring covers the unread tail). */
    SDL_bool use_file_xfer;
    char tmp_ring[TERM_TMP_RING][256];
    int tmp_ring_i;

    /* Kitty t=s shared-memory transport: the frame goes into a POSIX shm object,
       only the short name crosses the pty, and the terminal maps + deletes the
       object -- no temp file, no zlib (nothing to shrink: shared memory is not
       a wire). Auto-detected via a startup probe like file/zlib (cap_shm);
       SDL_TERMINAL_XFER=shm/file still force it on/off. The name ring mirrors
       tmp_ring for exit cleanup of any unread tail. macOS rules honored in
       term_write_shm: names <= 31 bytes incl '/'; ftruncate exactly once; write
       via mmap (write(2) fails on shm descriptors). */
    SDL_bool use_shm_xfer;
    Uint32 shm_seq;
    char shm_ring[TERM_TMP_RING][32];
    int shm_ring_i;

    /* Fullscreen content mode (Ubiquitty DECSET 2501): SDL_TERMINAL_FULLSCREEN=1.
       The terminal zeroes padding, aspect-CONTAIN-fits the frame to the drawable,
       and paints a solid-black letterbox — so the driver drops its own c/r cell
       box and transmits the frame at native resolution (all scaling is the
       terminal's). tx_w/tx_h = the transmitted image size, needed for the
       contain-fit mouse inverse. */
    SDL_bool fullscreen;
    int tx_w, tx_h;

    /* Relative pointer-lock / mouselook (Ubiquitty DECSET 2502): enabled while
       the app (DOSBox) is in SDL relative-mouse mode. The terminal then hides +
       confines the OS cursor and reports motion as center-biased deltas in the
       ?1016 frame (px = 0x8000 + dx); the driver injects them as relative SDL
       motion so DOS mouselook works. */
    SDL_bool relative;

    /* Runtime terminal-capability detection (term_probe_drain): at startup the
       driver probes the terminal and each cap_* is set from the reply, so the
       features below auto-enable only where supported (env vars still override).
       cap_file/cap_zlib/cap_shm come from a 1x1 transmit probe ACK (t=t / o=z /
       t=s); cap_2501/cap_2502 from DECRQM (CSI ?25xx$p -> DECRPM ;Pv$y with Pv
       in {1,2,3}). */
    SDL_bool cap_probed;
    SDL_bool cap_file, cap_zlib, cap_shm, cap_2501, cap_2502;

    /* Fullscreen display geometry declared to the terminal via OSC 2501, and
       mirrored here so the mouse inverse knows the on-screen image layout:
       osc_ar  = stretch-to-aspect ratio (fit=stretch), 0 = contain/native;
       want_scale = SDL_TERMINAL_SCALE, a fixed integer device-px scale (fit=scale)
       for square-pixel guests (e.g. Windows) — crisp integer magnification, not
       fill; osc_scale = the scale currently declared. scale and ar are mutually
       exclusive (scale wins). */
    double osc_ar;
    /* Pointer-lock delta equalization: fractional remainders carried across
       relative motion events, so the per-axis display-inverse scaling below
       never drops sub-pixel motion. */
    double rel_carry_x, rel_carry_y;
    int want_scale, osc_scale;
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

/* --- frame output buffer (batches a whole frame into one blocking write) --- */

static void term_out_reset(void)
{
    term.out_len = 0;
    term.out_off = 0;
}

static void term_out_append(const void *buf, size_t len)
{
    if (term.out_len + len > term.out_cap) {
        size_t ncap = term.out_cap ? term.out_cap : 65536;
        Uint8 *nb;
        while (ncap < term.out_len + len)
            ncap *= 2;
        nb = (Uint8 *)SDL_realloc(term.out, ncap);
        if (!nb)
            return;                     /* OOM: frame truncated (skipped this time) */
        term.out = nb;
        term.out_cap = ncap;
    }
    SDL_memcpy(term.out + term.out_len, buf, len);
    term.out_len += len;
}

static void term_out_flush(void)
{
    if (term.out_len > term.out_off)
        term_write(term.out + term.out_off, term.out_len - term.out_off);
    term_out_reset();
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
    /* Flush any buffered frame so we don't leave a dangling APC. */
    term_out_flush();
    /* Leading ST closes any string escape still open. Then leave fullscreen
       content mode (2501 — soft reset does NOT clear it, so exit it explicitly),
       pop kitty keyboard flags (stops further CSI-u), disable mouse, delete all
       kitty images (q=2: no ACK), show cursor, leave alt screen. */
    if (term.relative)
        term_write_str("\x1b[?2502l");   /* release pointer lock -> un-hide OS cursor */
    if (term.fullscreen)
        term_write_str("\x1b[?2501l");
    term_write_str("\x1b\\\x1b[?1003;1006;1016l"
                   "\x1b[<u\x1b_Ga=d,q=2\x1b\\\x1b[?25h\x1b[?1049l");
    /* Flush pending/in-flight input so it doesn't spill onto the shell prompt as
       garbage like `9;5:3u` — the key/modifier RELEASE events the terminal
       queued in the kitty CSI-u format (e.g. releases of keys still held when we
       quit). A short fixed poll is not enough: those events can land a little
       after we tear down. Instead, AFTER popping kitty keyboard above, emit a
       DSR (cursor-position report) and read+discard input up to its reply. The
       terminal answers the DSR only once it has processed the mode-pop, so every
       CSI-u event queued before then is drained; anything the user does after is
       in the shell's legacy mode (no release reports) and is harmless. */
    if (term.infd >= 0) {
        struct pollfd pfd;
        char discard[512];
        int rounds;
        SDL_bool saw_reply = SDL_FALSE;
        term_write_str("\x1b[6n"); /* DSR — reply is ESC[<row>;<col>R */
        pfd.fd = term.infd;
        pfd.events = POLLIN;
        for (rounds = 0; rounds < 16 && !saw_reply; rounds++) {
            ssize_t n;
            int i, pr;
            pr = poll(&pfd, 1, 30);
            if (pr < 0)
                break;
            if (pr == 0 || !(pfd.revents & POLLIN))
                continue;             /* reply may be slightly delayed; keep waiting */
            n = read(term.infd, discard, sizeof(discard));
            if (n <= 0)
                break;
            for (i = 0; i < (int)n; i++) {
                if (discard[i] == 'R') { saw_reply = SDL_TRUE; break; }
            }
        }
    }
    if (term.raw_active) {
        tcsetattr(term.infd, TCSANOW, &term.saved_termios);
        term.raw_active = SDL_FALSE;
    }
    /* Remove any file-transport temp files the terminal didn't consume. Runs on
       every exit path incl. signal handlers (unlink is async-signal-safe). */
    {
        int k;
        for (k = 0; k < TERM_TMP_RING; k++)
            if (term.tmp_ring[k][0]) {
                unlink(term.tmp_ring[k]);
                term.tmp_ring[k][0] = 0;
            }
        for (k = 0; k < TERM_TMP_RING; k++)
            if (term.shm_ring[k][0]) {
                shm_unlink(term.shm_ring[k]);
                term.shm_ring[k][0] = 0;
            }
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
                   "\x1b[14t"              /* query text-area pixel size (precise advance) */
                   "\x1b[16t");            /* query cell pixel size (fallback) */
    /* DECSET 2501h (fullscreen) is emitted after term_probe_drain resolves whether
       the terminal supports it and whether fullscreen is enabled. */
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
   without c/r may not be drawn.

   The DISPLAY aspect is either the image's own pixel aspect (W/H) or, when
   SDL_TERMINAL_ASPECT forced one (e.g. 4:3 for DOS content whose non-square
   pixels look wrong at their raw 8:5 ratio), that forced ratio. The image is
   scaled by the terminal into the chosen cell box, so picking a 4:3 box is what
   distorts a 320x200 frame back to the 4:3 its CRT showed.

   Cell pixel size comes from the kernel winsize when present, else from the
   CSI 16t reply (term.cell_w/h, default 8x16) — so aspect-fitting works even in
   terminals (Ubiquitty) that report ws_xpixel/ypixel = 0. With no pixel geometry
   AND no forced aspect we fall back to filling the grid (stretched, but visible).
   Returns SDL_FALSE only if even the grid size is unknown. */
/* Effective display aspect for the current mode: an explicit SDL_TERMINAL_ASPECT
   wins; else the per-mode ratio DOSBox publishes (DOSBOX_DISPLAY_ASPECT hint) so
   text/DOS modes auto-correct to 4:3 and flip as the guest changes modes; else 0
   (the image's own pixel aspect). Shared by term_fit_cells (legacy cell box) and
   the fullscreen OSC 2501 stretch target (term_emit_frame). */
static double term_display_ar(void)
{
    double ar = term.forced_ar;
    if (ar <= 0.0) {
        const char *h = SDL_GetHint("DOSBOX_DISPLAY_ASPECT");
        if (h && *h) {
            double a = SDL_atof(h);
            if (a > 0.0) ar = a;
        }
    }
    return ar;
}

static SDL_bool term_fit_cells(int W, int H, int *out_cols, int *out_rows)
{
    struct winsize ws;
    double cell_w, cell_h, target_ar, term_pw, term_ph, term_ar, tw, th, eff_ar;
    int cols, rows;

    if (ioctl(term.outfd, TIOCGWINSZ, &ws) != 0)
        return SDL_FALSE;
    if (ws.ws_col == 0 || ws.ws_row == 0)
        return SDL_FALSE;

    /* Effective display aspect: an explicit SDL_TERMINAL_ASPECT wins; else the
       per-mode aspect DOSBox publishes (DOSBOX_DISPLAY_ASPECT hint) so text/DOS
       modes auto-correct to 4:3 while hi-res graphical modes stay native, flipping
       automatically as the guest changes modes; else 0 => the image's own pixel
       aspect. */
    eff_ar = term.forced_ar;
    if (eff_ar <= 0.0) {
        const char *h = SDL_GetHint("DOSBOX_DISPLAY_ASPECT");
        if (h && *h) {
            double a = SDL_atof(h);
            if (a > 0.0) eff_ar = a;
        }
    }
    if (SDL_getenv("SDL_TERMINAL_DEBUG")) {
        static double s_last = -2.0;
        if (eff_ar != s_last) {
            fprintf(stderr, "[aspect] surface %dx%d -> display AR %.4f (%s)\n",
                    W, H, eff_ar > 0.0 ? eff_ar : (double)W / (double)H,
                    term.forced_ar > 0.0 ? "SDL_TERMINAL_ASPECT" :
                    (eff_ar > 0.0 ? "DOSBOX hint" : "native pixel"));
            s_last = eff_ar;
        }
    }

    if (ws.ws_xpixel > 0 && ws.ws_ypixel > 0) {
        cell_w = (double)ws.ws_xpixel / ws.ws_col;
        cell_h = (double)ws.ws_ypixel / ws.ws_row;
    } else if (eff_ar > 0.0) {
        /* No kernel pixel geometry, but we have a target aspect: use the cell
           size from the CSI 16t reply (default 8x16). */
        cell_w = (double)term.cell_w;
        cell_h = (double)term.cell_h;
    } else {
        /* No pixel geometry and no target aspect: fill the grid so the image is
           at least visible. */
        *out_cols = ws.ws_col;
        *out_rows = ws.ws_row;
        return SDL_TRUE;
    }
    if (cell_w < 1.0) cell_w = 1.0;
    if (cell_h < 1.0) cell_h = 1.0;

    target_ar = (eff_ar > 0.0) ? eff_ar : ((double)W / (double)H);
    term_pw = (double)ws.ws_col * cell_w;
    term_ph = (double)ws.ws_row * cell_h;
    term_ar = term_pw / term_ph;

    if (term_ar > target_ar) { /* terminal wider than target: fit height */
        th = term_ph;
        tw = th * target_ar;
    } else { /* fit width */
        tw = term_pw;
        th = tw / target_ar;
    }
    cols = (int)(tw / cell_w);
    rows = (int)(th / cell_h);
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols > (int)ws.ws_col) cols = (int)ws.ws_col;
    if (rows > (int)ws.ws_row) rows = (int)ws.ws_row;
    *out_cols = cols;
    *out_rows = rows;
    return SDL_TRUE;
}

/* Write `len` bytes to a fresh temp file for kitty t=t transport (the terminal
   deletes it after reading). Returns the path in `out` (NUL-terminated) and
   SDL_TRUE on success. */
static SDL_bool term_write_tmpfile(const Uint8 *buf, size_t len, char *out, size_t cap)
{
    const char *dir = SDL_getenv("TMPDIR");
    char tmpl[256];
    const char *p;
    size_t n, plen;
    int fd;

    if (!dir || !*dir)
        dir = "/tmp";
    /* "tty-graphics-protocol" in the name: kitty/Ghostty only read+delete a t=t
       file whose path contains that literal (a safety rule); Ubiquitty accepts
       any temp-dir path. Keeping it satisfies all three. */
    if (SDL_snprintf(tmpl, sizeof(tmpl), "%s/ubterm-tty-graphics-protocol-XXXXXX", dir) >= (int)sizeof(tmpl))
        return SDL_FALSE;
    fd = mkstemp(tmpl);
    if (fd < 0)
        return SDL_FALSE;
    p = (const char *)buf;
    n = len;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            unlink(tmpl);
            return SDL_FALSE;
        }
        p += w;
        n -= (size_t)w;
    }
    close(fd);
    plen = SDL_strlen(tmpl);
    if (plen + 1 > cap) {
        unlink(tmpl);
        return SDL_FALSE;
    }
    SDL_memcpy(out, tmpl, plen + 1);
    return SDL_TRUE;
}

/* Write one frame into a fresh POSIX shared-memory object and return its name
   (<= 31 bytes incl the leading '/', the macOS PSHMNAMLEN cap). The terminal
   shm_unlinks the object after reading (t=s consume semantics); the unlink here
   reclaims a slot against a terminal that never read it, bounding live objects
   to the ring. macOS: ftruncate on shm is ONE-SHOT and write(2) is unsupported,
   so size exactly once and fill through a mapping. */
static SDL_bool term_write_shm(const Uint8 *buf, size_t len, char *out, size_t cap)
{
    void *map;
    int fd;

    if (SDL_snprintf(out, cap, "/ub%u-%u", (unsigned)(getpid() % 100000u), (unsigned)(term.shm_seq & (TERM_TMP_RING - 1))) >= (int)cap)
        return SDL_FALSE;
    term.shm_seq++;
    shm_unlink(out); /* reclaim: harmless ENOENT when the terminal consumed it */
    fd = shm_open(out, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0)
        return SDL_FALSE;
    if (ftruncate(fd, (off_t)len) != 0) {
        close(fd);
        shm_unlink(out);
        return SDL_FALSE;
    }
    map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        shm_unlink(out);
        return SDL_FALSE;
    }
    SDL_memcpy(map, buf, len);
    munmap(map, len);
    close(fd);
    /* Record the name for EXIT cleanup only. Do NOT unlink the ring slot here:
       the ring and the name sequence cycle in lockstep, so the slot being
       recorded holds the SAME name as the object just created -- an unlink here
       deletes the fresh frame before the terminal can open it (broke every
       frame after the ring warmed up). Reclaiming a slot's previous object is
       the pre-create shm_unlink(out) above; quit-time residue is the ring sweep. */
    SDL_memcpy(term.shm_ring[term.shm_ring_i], out, SDL_strlen(out) + 1);
    term.shm_ring_i = (term.shm_ring_i + 1) & (TERM_TMP_RING - 1);
    return SDL_TRUE;
}

static void term_emit_frame(SDL_Surface *surface)
{
    int W = surface->w;
    int H = surface->h;
    /* f=24 (tight RGB) for file transport and base64: with o=z zlib compression
       in play there, the pre-compression size matters (25% less than RGBA to
       deflate) more than the terminal-side widen-to-RGBA cost. f=32 (RGBA) for
       shm instead: zlib is skipped there (memory isn't a wire), so that
       trade-off doesn't apply, and RGBA lets the terminal map/upload the
       segment directly with no widen step at all. */
    int bpp = term.use_shm_xfer ? 4 : 3;
    int fmt_num = term.use_shm_xfer ? 32 : 24;
    size_t rgb_len = (size_t)W * (size_t)H * (size_t)bpp;
    const size_t CHUNK = 4096;
    const Uint8 *src_rgb;       /* what we transmit: term.rgb, or the pre-scaled buffer */
    size_t src_len;
    int src_w, src_h;
    const Uint8 *payload;
    size_t payload_len, b64_max, b64_len, off;
    const char *ozp = "";       /* ",o=z" when the payload is zlib-compressed */
    Uint8 *dst;
    int x, y, cols = 0, rows = 0, len;
    SDL_bool have_fit, first;
    char hdr[128];
    Uint32 id;

    term_out_reset();

    /* In fullscreen, declare the display aspect to the terminal via OSC 2501 so it
       stretches DOS's non-square pixels to the intended shape (e.g. 320x200 -> 4:3)
       rather than showing the raw pixel ratio. Emitted only when the aspect changes
       (per mode); term.osc_ar records it for the fullscreen mouse inverse. Written
       directly (before the buffered frame) so "set aspect" precedes "draw". */
    if (term.fullscreen) {
        int wscale = term.want_scale;                          /* fixed integer scale, or 0 */
        double war = (wscale > 0) ? 0.0 : term_display_ar();   /* else stretch aspect, or 0 */
        double d = war - term.osc_ar;
        if (wscale != term.osc_scale || d < -1e-6 || d > 1e-6) {
            char osc[64];
            term.osc_scale = wscale;
            term.osc_ar = war;
            if (wscale > 0)
                SDL_snprintf(osc, sizeof(osc), "\x1b]2501;fit=scale;scale=%d\x1b\\", wscale);
            else if (war > 0.0)
                SDL_snprintf(osc, sizeof(osc),
                             "\x1b]2501;fit=stretch;ar=%d:1000\x1b\\", (int)(war * 1000.0 + 0.5));
            else
                SDL_strlcpy(osc, "\x1b]2501;fit=contain\x1b\\", sizeof(osc));
            term_write_str(osc);
        }
    }

    if (rgb_len > term.rgb_cap) {
        Uint8 *nb = (Uint8 *)SDL_realloc(term.rgb, rgb_len);
        if (!nb)
            return;
        term.rgb = nb;
        term.rgb_cap = rgb_len;
    }

    /* pack XRGB8888 (SDL_PIXELFORMAT_RGB888) -> tight RGB (f=24) or RGBA (f=32) */
    dst = term.rgb;
    if (bpp == 4) {
        for (y = 0; y < H; ++y) {
            const Uint32 *row = (const Uint32 *)((const Uint8 *)surface->pixels + (size_t)y * surface->pitch);
            for (x = 0; x < W; ++x) {
                Uint32 p = row[x];
                *dst++ = (Uint8)((p >> 16) & 0xff); /* R */
                *dst++ = (Uint8)((p >> 8) & 0xff);  /* G */
                *dst++ = (Uint8)(p & 0xff);         /* B */
                *dst++ = 0xff;                      /* A (opaque) */
            }
        }
    } else {
        for (y = 0; y < H; ++y) {
            const Uint32 *row = (const Uint32 *)((const Uint8 *)surface->pixels + (size_t)y * surface->pitch);
            for (x = 0; x < W; ++x) {
                Uint32 p = row[x];
                *dst++ = (Uint8)((p >> 16) & 0xff); /* R */
                *dst++ = (Uint8)((p >> 8) & 0xff);  /* G */
                *dst++ = (Uint8)(p & 0xff);         /* B */
            }
        }
    }

    /* Skip-unchanged: if this frame is byte-identical to the last one we sent,
       the terminal is already showing it — skip all the compress/base64/write
       work (a big win for static DOS screens, menus, the C:\> prompt). A bounded
       streak forces a periodic resend so a scrolled/dropped image can't stay
       stale forever. */
    if (term.last && term.last_len == rgb_len && term.unchanged_streak < 60 &&
        SDL_memcmp(term.last, term.rgb, rgb_len) == 0) {
        term.unchanged_streak++;
        return;
    }
    term.unchanged_streak = 0;
    if (rgb_len > term.last_cap) {
        Uint8 *nb = (Uint8 *)SDL_realloc(term.last, rgb_len);
        if (nb) { term.last = nb; term.last_cap = rgb_len; }
    }
    if (term.last_cap >= rgb_len) {
        SDL_memcpy(term.last, term.rgb, rgb_len);
        term.last_len = rgb_len;
    } else {
        term.last_len = 0;  /* couldn't cache -> always send until it fits */
    }

    /* Transmit the frame at its NATIVE resolution and let the terminal do all
       scaling — in 2501 it aspect-contain-fits and letterboxes it. The driver
       does not resample. (Making DOS's non-square pixels display at 4:3 rather
       than their raw pixel ratio is the terminal's job — a display-aspect knob —
       or DOSBox's own aspect correction; not the driver's.) */
    src_rgb = term.rgb;
    src_len = rgb_len;
    src_w = W;
    src_h = H;
    term.tx_w = W;   /* transmitted size, for the mouse contain-fit inverse */
    term.tx_h = H;

    /* Optional zlib payload compression (kitty o=z). Compresses the raw RGB
       before base64, cutting wire bytes for typical flat DOS frames. Falls back
       to the raw payload if libz is unavailable or compression fails. Skipped
       entirely under file transport: the pty payload there is just the temp
       file's path (~150-250 bytes either way), so compression buys no wire
       savings — only the deflate CPU cost (~14ms/frame for a full raw frame)
       for a fraction-of-a-ms reduction in what the OS page cache moves. */
    payload = src_rgb;
    payload_len = src_len;
    if (term.use_zlib && !term.use_file_xfer && !term.use_shm_xfer && term.z_compress2 && term.z_compressBound) {
        unsigned long zcap = term.z_compressBound((unsigned long)src_len);
        if ((size_t)zcap > term.zbuf_cap) {
            Uint8 *nb = (Uint8 *)SDL_realloc(term.zbuf, (size_t)zcap);
            if (nb) {
                term.zbuf = nb;
                term.zbuf_cap = (size_t)zcap;
            }
        }
        if (term.zbuf && term.zbuf_cap >= (size_t)zcap) {
            unsigned long zlen = (unsigned long)term.zbuf_cap;
            if (term.z_compress2(term.zbuf, &zlen, src_rgb, (unsigned long)src_len, 1) == 0) {
                payload = term.zbuf;
                payload_len = (size_t)zlen;
                ozp = ",o=z";
            }
        }
    }

    /* In 2501 the terminal contain-fits and ignores c/r, so don't compute or send
       a cell box; the mouse uses the contain-fit inverse instead. */
    have_fit = term.fullscreen ? SDL_FALSE : term_fit_cells(W, H, &cols, &rows);

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

    /* Fastest path: kitty shared-memory transport (t=s). The frame goes into a
       POSIX shm object; only the short name + header cross the pty; the payload
       is raw (S carries its exact length -- the kernel page-rounds object
       sizes) and uncompressed (the compress branch above skips shm: memory is
       not a wire, and inflate costs ~100x what the page-cache bytes save).
       Auto-detected via a startup probe (cap_shm) like file/zlib -- a terminal
       without t=s just never sets it, so this only fires where it verifiably
       worked; SDL_TERMINAL_XFER=shm/file still force it on/off. Falls through
       to t=t / base64 when the object can't be created. */
    if (term.use_shm_xfer) {
        char shm_name[32], b64name[64];
        if (term_write_shm(payload, payload_len, shm_name, sizeof(shm_name))) {
            size_t pn = term_base64((const Uint8 *)shm_name, SDL_strlen(shm_name), b64name);
            term_out_append("\x1b[H", 3);
            if (have_fit)
                len = SDL_snprintf(hdr, sizeof(hdr),
                                   "\x1b_Gq=2,a=T,f=%d,t=s,S=%u,s=%d,v=%d,i=%u,C=1,c=%d,r=%d,m=0;",
                                   fmt_num, (unsigned)payload_len, src_w, src_h, (unsigned)id, cols, rows);
            else
                len = SDL_snprintf(hdr, sizeof(hdr),
                                   "\x1b_Gq=2,a=T,f=%d,t=s,S=%u,s=%d,v=%d,i=%u,C=1,m=0;",
                                   fmt_num, (unsigned)payload_len, src_w, src_h, (unsigned)id);
            term_out_append(hdr, (size_t)len);
            term_out_append(b64name, pn);
            term_out_append("\x1b\\", 2);
            if (term.prev_image_id != 0) {
                len = SDL_snprintf(hdr, sizeof(hdr), "\x1b_Ga=d,d=I,i=%u,q=2\x1b\\", (unsigned)term.prev_image_id);
                term_out_append(hdr, (size_t)len);
            }
            term.prev_image_id = id;
            term_out_flush();
            return;
        }
    }

    /* Fast path: kitty file transport (t=t). Write the raw payload to a temp
       file and send only its (base64) path — skips the per-frame base64 of the
       pixels entirely, and the ~megabyte frame travels through the filesystem
       (page cache) instead of the pty. Opt-in via SDL_TERMINAL_XFER=file; the
       terminal must support t=t or the image silently won't render (hence not
       the default). Falls through to base64 if the temp file can't be written. */
    if (term.use_file_xfer) {
        char path[256], b64path[512];
        if (term_write_tmpfile(payload, payload_len, path, sizeof(path))) {
            size_t pn = term_base64((const Uint8 *)path, SDL_strlen(path), b64path);
            /* Reclaim the slot we're about to reuse: under t=t the terminal has
               long since read+deleted it (unlink is a harmless ENOENT); against
               a terminal that never read it, this bounds our temp files to the
               ring instead of leaking one per frame. */
            if (term.tmp_ring[term.tmp_ring_i][0])
                unlink(term.tmp_ring[term.tmp_ring_i]);
            SDL_memcpy(term.tmp_ring[term.tmp_ring_i], path, SDL_strlen(path) + 1);
            term.tmp_ring_i = (term.tmp_ring_i + 1) & (TERM_TMP_RING - 1);
            term_out_append("\x1b[H", 3);
            if (have_fit)
                len = SDL_snprintf(hdr, sizeof(hdr),
                                   "\x1b_Gq=2,a=T,f=%d,t=t,s=%d,v=%d,i=%u,C=1%s,c=%d,r=%d,m=0;",
                                   fmt_num, src_w, src_h, (unsigned)id, ozp, cols, rows);
            else
                len = SDL_snprintf(hdr, sizeof(hdr),
                                   "\x1b_Gq=2,a=T,f=%d,t=t,s=%d,v=%d,i=%u,C=1%s,m=0;",
                                   fmt_num, src_w, src_h, (unsigned)id, ozp);
            term_out_append(hdr, (size_t)len);
            term_out_append(b64path, pn);
            term_out_append("\x1b\\", 2);
            if (term.prev_image_id != 0) {
                len = SDL_snprintf(hdr, sizeof(hdr), "\x1b_Ga=d,d=I,i=%u,q=2\x1b\\", (unsigned)term.prev_image_id);
                term_out_append(hdr, (size_t)len);
            }
            term.prev_image_id = id;
            term_out_flush();
            return;
        }
    }

    b64_max = 4 * ((payload_len + 2) / 3);
    if (b64_max > term.b64_cap) {
        char *nb = (char *)SDL_realloc(term.b64, b64_max);
        if (!nb)
            return;
        term.b64 = nb;
        term.b64_cap = b64_max;
    }
    b64_len = term_base64(payload, payload_len, term.b64);

    /* Build the whole frame into the output buffer, then flush it non-blocking. */
    term_out_append("\x1b[H", 3);

    off = 0;
    first = SDL_TRUE;
    do {
        size_t n = (b64_len - off > CHUNK) ? CHUNK : (b64_len - off);
        int last = (off + n >= b64_len);
        int m = last ? 0 : 1;
        if (first) {
            if (have_fit) {
                len = SDL_snprintf(hdr, sizeof(hdr),
                                   "\x1b_Gq=2,a=T,f=%d,s=%d,v=%d,i=%u,C=1%s,c=%d,r=%d,m=%d;",
                                   fmt_num, src_w, src_h, (unsigned)id, ozp, cols, rows, m);
            } else {
                len = SDL_snprintf(hdr, sizeof(hdr),
                                   "\x1b_Gq=2,a=T,f=%d,s=%d,v=%d,i=%u,C=1%s,m=%d;",
                                   fmt_num, src_w, src_h, (unsigned)id, ozp, m);
            }
            first = SDL_FALSE;
        } else {
            len = SDL_snprintf(hdr, sizeof(hdr), "\x1b_Gm=%d;", m);
        }
        term_out_append(hdr, (size_t)len);
        if (n > 0)
            term_out_append(term.b64 + off, n);
        term_out_append("\x1b\\", 2);
        off += n;
    } while (off < b64_len);

    /* Delete the previous frame's image so slots/textures don't accumulate.
       q=2 suppresses the terminal's per-delete "OK" acknowledgement; without it
       the terminal streams an APC reply for every frame, which piles up in our
       input and spews to the shell on exit. */
    if (term.prev_image_id != 0) {
        len = SDL_snprintf(hdr, sizeof(hdr), "\x1b_Ga=d,d=I,i=%u,q=2\x1b\\", (unsigned)term.prev_image_id);
        term_out_append(hdr, (size_t)len);
    }
    term.prev_image_id = id;

    term_out_flush();
}

/* ------------------------------------------------------------------ */
/* SDL video device hooks                                            */
/* ------------------------------------------------------------------ */

static int TERMINAL_CreateWindowFramebuffer(_THIS, SDL_Window *window, Uint32 *format, void **pixels, int *pitch);
static int TERMINAL_UpdateWindowFramebuffer(_THIS, SDL_Window *window, const SDL_Rect *rects, int numrects);
static void TERMINAL_DestroyWindowFramebuffer(_THIS, SDL_Window *window);
static void TERMINAL_PumpEvents(_THIS);

/* Opt-in zlib payload compression (kitty o=z). dlopen'd at runtime so the driver
   never forces a libz link dependency on host SDL apps. Off unless
   SDL_TERMINAL_ZLIB is set to a non-"0" value AND libz resolves. Note: the
   terminal must support o=z (kitty/Ghostty do; verify others). */
static void term_zlib_init(void)
{
    const char *want = SDL_getenv("SDL_TERMINAL_ZLIB");
    const char *names[3];
    int i;

    term.use_zlib = SDL_FALSE;   /* resolved in VideoInit once the probe result is known */
    term.zlib_handle = NULL;
    term.z_compress2 = NULL;
    term.z_compressBound = NULL;
    /* Load libz unless explicitly disabled (SDL_TERMINAL_ZLIB=0): auto-detection
       needs it both to send the o=z capability probe and to compress frames if
       the terminal turns out to support o=z. */
    if (want && want[0] == '0' && want[1] == '\0')
        return;

    names[0] = "libz.dylib";  /* macOS */
    names[1] = "libz.so.1";   /* Linux */
    names[2] = "libz.so";
    for (i = 0; i < 3 && !term.zlib_handle; i++)
        term.zlib_handle = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
    if (!term.zlib_handle)
        return;

    *(void **)(&term.z_compress2) = dlsym(term.zlib_handle, "compress2");
    *(void **)(&term.z_compressBound) = dlsym(term.zlib_handle, "compressBound");
    /* term.z_compress2 && term.z_compressBound now indicates libz is usable. */
}

/* Scan a batch of terminal replies collected during startup probing and record
   capabilities. Idempotent (re-scanning the growing buffer each round is safe);
   returns SDL_TRUE once the terminating Primary-DA reply (CSI ... c) is seen.
   Handles: DECRPM (CSI ?<mode>;<Pv>$y), kitty APC ACKs (ESC _G i=<id>;OK/E.. ST),
   the DA terminator, and 14t/16t geometry replies (captured as a bonus). */
static SDL_bool term_probe_parse(const unsigned char *b, int n)
{
    int i = 0;
    SDL_bool saw_da = SDL_FALSE;
    while (i < n) {
        if (b[i] == 0x1b && i + 1 < n && b[i + 1] == '[') {
            int j = i + 2, priv = 0, np = 0, seen = 0, dollar = 0;
            int params[4];
            unsigned int cur = 0;
            unsigned char final = 0;
            if (j < n && (b[j] == '?' || b[j] == '>')) { priv = b[j]; j++; }
            while (j < n) {
                unsigned char ch = b[j];
                if (ch >= '0' && ch <= '9') { cur = cur * 10 + (ch - '0'); seen = 1; j++; continue; }
                if (ch == ';') { if (np < 4) params[np++] = seen ? (int)cur : 0; cur = 0; seen = 0; j++; continue; }
                if (ch == '$') { dollar = 1; j++; continue; }
                if (seen && np < 4) params[np++] = (int)cur;
                final = ch; j++; break;
            }
            if (final == 0) break; /* incomplete CSI: wait for more bytes */
            if (final == 'y' && dollar && priv == '?' && np >= 2) {
                int mode = params[0], pv = params[1];
                SDL_bool ok = (pv == 1 || pv == 2 || pv == 3); /* recognized+settable */
                if (mode == 2501) term.cap_2501 = ok;
                else if (mode == 2502) term.cap_2502 = ok;
            } else if (final == 'c') {
                saw_da = SDL_TRUE;
            } else if (final == 't' && np >= 3) {
                if (params[0] == 6 && params[1] > 0 && params[2] > 0) { term.cell_h = params[1]; term.cell_w = params[2]; }
                else if (params[0] == 4 && params[1] > 0 && params[2] > 0) { term.win_h = params[1]; term.win_w = params[2]; }
            }
            i = j;
            continue;
        }
        if (b[i] == 0x1b && i + 1 < n && b[i + 1] == '_') {
            int j = i + 2, st = -1, k;
            while (j + 1 < n) { if (b[j] == 0x1b && b[j + 1] == '\\') { st = j; break; } j++; }
            if (st < 0) break; /* incomplete APC: wait for ST */
            if (i + 2 < st && b[i + 2] == 'G') {
                int semi = -1;
                long id = -1;
                for (k = i + 3; k < st; k++) if (b[k] == ';') { semi = k; break; }
                for (k = i + 3; k + 1 < (semi < 0 ? st : semi); k++) {
                    if (b[k] == 'i' && b[k + 1] == '=') {
                        long v = 0; int m = k + 2, got = 0;
                        while (m < st && b[m] >= '0' && b[m] <= '9') { v = v * 10 + (b[m] - '0'); m++; got = 1; }
                        if (got) id = v;
                        break;
                    }
                }
                if (semi >= 0 && semi + 2 < st && b[semi + 1] == 'O' && b[semi + 2] == 'K') {
                    if (id == (long)TERM_PROBE_FILE_ID) term.cap_file = SDL_TRUE;
                    else if (id == (long)TERM_PROBE_ZLIB_ID) term.cap_zlib = SDL_TRUE;
                    else if (id == (long)TERM_PROBE_SHM_ID) term.cap_shm = SDL_TRUE;
                }
            }
            i = st + 2;
            continue;
        }
        i++;
    }
    return saw_da;
}

/* Probe the terminal for optional-feature support, then block briefly for the
   replies. Sends: DECRQM for 2501/2502, a 1x1 kitty transmit over t=t (file),
   t=s (shm) and o=z (zlib) whose ACKs prove those mediums work, and a trailing
   Primary DA as a guaranteed terminator so the drain never hangs on a terminal
   that stays silent. Results land in term.cap_*. Modeled on the term_restore
   DSR-drain (bounded poll rounds). Must run before the feature flags resolve. */
static void term_probe_drain(void)
{
    char msg[128], fpath[256], fb64[512], shm_name[32], shm_b64[64];
    SDL_bool have_file = SDL_FALSE, have_zlib = SDL_FALSE, have_shm = SDL_FALSE;
    int L;

    term.cap_probed = SDL_TRUE;

    term_write_str("\x1b[?2501$p\x1b[?2502$p");

    /* File-transport probe: a real 1x1 RGB frame written to a temp file and sent
       via t=t. A ";OK" ACK for TERM_PROBE_FILE_ID means the terminal read it. */
    {
        Uint8 px[3] = { 0, 0, 0 };
        if (term_write_tmpfile(px, sizeof(px), fpath, sizeof(fpath))) {
            size_t pn;
            /* Register in the ring so term_restore's async-signal-safe cleanup
               removes it if a signal lands during the (blocking) drain below,
               before the explicit unlink(fpath) runs. */
            SDL_memcpy(term.tmp_ring[term.tmp_ring_i], fpath, SDL_strlen(fpath) + 1);
            term.tmp_ring_i = (term.tmp_ring_i + 1) & (TERM_TMP_RING - 1);
            pn = term_base64((const Uint8 *)fpath, SDL_strlen(fpath), fb64);
            L = SDL_snprintf(msg, sizeof(msg),
                             "\x1b_Ga=t,i=%u,f=24,s=1,v=1,t=t,q=0;", (unsigned)TERM_PROBE_FILE_ID);
            term_write(msg, (size_t)L);
            term_write(fb64, pn);
            term_write_str("\x1b\\");
            have_file = SDL_TRUE;
        }
    }

    /* Shm probe: a 1x1 RGB frame in a POSIX shm object, sent via t=s. A ";OK"
       ACK for TERM_PROBE_SHM_ID means the terminal mapped + read it. Reuses
       term_write_shm, which already registers the name in term.shm_ring for
       exit-time cleanup, so unlike the file probe above there's no manual ring
       bookkeeping needed here. */
    {
        Uint8 px[3] = { 0, 0, 0 };
        if (term_write_shm(px, sizeof(px), shm_name, sizeof(shm_name))) {
            size_t pn = term_base64((const Uint8 *)shm_name, SDL_strlen(shm_name), shm_b64);
            L = SDL_snprintf(msg, sizeof(msg),
                             "\x1b_Ga=t,i=%u,f=24,s=1,v=1,t=s,q=0;", (unsigned)TERM_PROBE_SHM_ID);
            term_write(msg, (size_t)L);
            term_write(shm_b64, pn);
            term_write_str("\x1b\\");
            have_shm = SDL_TRUE;
        }
    }

    /* Zlib probe: a 1x1 RGB frame compressed with o=z, sent direct (t=d). */
    if (term.z_compress2 && term.z_compressBound) {
        Uint8 px[3] = { 0, 0, 0 };
        Uint8 zb[64];
        unsigned long zl = sizeof(zb);
        if (term.z_compress2(zb, &zl, px, sizeof(px), 1) == 0) {
            char zb64[128];
            size_t pn = term_base64(zb, (size_t)zl, zb64);
            L = SDL_snprintf(msg, sizeof(msg),
                             "\x1b_Ga=t,i=%u,f=24,s=1,v=1,o=z,q=0;", (unsigned)TERM_PROBE_ZLIB_ID);
            term_write(msg, (size_t)L);
            term_write(zb64, pn);
            term_write_str("\x1b\\");
            have_zlib = SDL_TRUE;
        }
    }

    term_write_str("\x1b[c"); /* Primary DA: guaranteed reply, marks end of replies */

    if (term.infd >= 0) {
        unsigned char buf[1024];
        int len = 0, rounds, grace = 0;
        struct pollfd pfd;
        SDL_bool saw_da = SDL_FALSE;
        pfd.fd = term.infd;
        pfd.events = POLLIN;
        /* Drain until every probe we sent is answered, or a grace window past the
           DA terminator elapses. The DA reply can PRECEDE the async graphics ACKs
           (Ubiquitty decodes off-thread), so seeing DA is not a stop signal by
           itself — we keep reading for a trailing ;OK. The file-transport ACK in
           particular involves real disk I/O plus (on a cold first probe of the
           session) one-time temp-dir/capability-check overhead, which measurably
           exceeds a few hundred ms in practice — a short grace window here reads
           as a false "unsupported". Grace ~1s past DA, hard cap ~2s total: a
           one-time startup cost, and a terminal that answers promptly (the
           common case) short-circuits via the file_done/zlib_done check below
           long before either bound is reached. */
        for (rounds = 0; rounds < 80 && len < (int)sizeof(buf); rounds++) {
            ssize_t r;
            int pr = poll(&pfd, 1, 25);
            if (pr < 0) break;
            if (pr > 0 && (pfd.revents & POLLIN)) {
                r = read(term.infd, buf + len, sizeof(buf) - (size_t)len);
                if (r <= 0) break;
                len += (int)r;
                if (term_probe_parse(buf, len)) saw_da = SDL_TRUE;
            }
            if (saw_da) {
                SDL_bool file_done = !have_file || term.cap_file;
                SDL_bool zlib_done = !have_zlib || term.cap_zlib;
                SDL_bool shm_done  = !have_shm  || term.cap_shm;
                if ((file_done && zlib_done && shm_done) || ++grace >= 40) break;
            }
        }
        if (SDL_getenv("SDL_TERMINAL_DEBUG")) {
            int i;
            fprintf(stderr, "[caps] raw probe reply (%d bytes, %d rounds): \"", len, rounds);
            for (i = 0; i < len; i++) {
                unsigned char c = buf[i];
                if (c == 0x1b) fprintf(stderr, "\\e");
                else if (c >= 0x20 && c < 0x7f) fputc((int)c, stderr);
                else fprintf(stderr, "\\x%02x", c);
            }
            fprintf(stderr, "\"\n");
        }
    }

    /* Clean up: the terminal deletes a consumed t=t file / t=s shm object itself
       (our unlink/shm_unlink then ENOENTs harmlessly); if it never read it, we
       remove it here. Then delete the stored 1x1 probe images (q=2: no reply). */
    if (have_file)
        unlink(fpath);
    if (have_shm)
        shm_unlink(shm_name);
    if (have_file) {
        L = SDL_snprintf(msg, sizeof(msg), "\x1b_Ga=d,d=I,i=%u,q=2\x1b\\", (unsigned)TERM_PROBE_FILE_ID);
        term_write(msg, (size_t)L);
    }
    if (have_shm) {
        L = SDL_snprintf(msg, sizeof(msg), "\x1b_Ga=d,d=I,i=%u,q=2\x1b\\", (unsigned)TERM_PROBE_SHM_ID);
        term_write(msg, (size_t)L);
    }
    if (have_zlib) {
        L = SDL_snprintf(msg, sizeof(msg), "\x1b_Ga=d,d=I,i=%u,q=2\x1b\\", (unsigned)TERM_PROBE_ZLIB_ID);
        term_write(msg, (size_t)L);
    }
}

/* Mouse-subsystem hook: SDL calls this when the app enters/leaves relative-mouse
   mode (DOSBox does so when a DOS game grabs the mouse). Returning 0 makes
   SDL_SetRelativeMouseMode succeed (without it SDL has no relative implementation
   for this driver and fails); we translate the transition into Ubiquitty's
   pointer-lock (DECSET 2502), and term_mouse then decodes the biased deltas. */
static int TERMINAL_SetRelativeMouseMode(SDL_bool enabled)
{
    /* Only drive Ubiquitty pointer-lock (2502) if the terminal supports it (probed);
       otherwise leave the mouse absolute. Return 0 regardless so SDL relative mode
       still "succeeds" and DOSBox proceeds (it just gets absolute reports). */
    SDL_bool want = (enabled && term.cap_2502) ? SDL_TRUE : SDL_FALSE;
    if (SDL_getenv("SDL_TERMINAL_DEBUG"))
        fprintf(stderr, "[relmode] app requested relative mouse = %d (2502 cap=%d -> %s)\n",
                (int)enabled, (int)term.cap_2502, want ? "h" : "l");
    if (want != term.relative) {
        /* Keep term.relative TRUE across the entire locked window so an async
           signal -> term_restore always emits ?2502l when the terminal might be
           in pointer-lock: set the flag BEFORE enabling, clear it AFTER releasing.
           (A spurious ?2502l from the pre-enable gap is a harmless no-op; a MISSED
           one would leave the OS cursor hidden/confined on the shell.) */
        if (want) {
            term.relative = SDL_TRUE;
            term_write_str("\x1b[?2502h");
        } else {
            term_write_str("\x1b[?2502l");
            term.relative = SDL_FALSE;
        }
    }
    return 0;
}

static int TERMINAL_VideoInit(_THIS)
{
    SDL_DisplayMode mode;
    struct winsize ws;
    const char *aspect;
    int w = 640, h = 480;

    term.cell_w = 8; /* sane defaults until the CSI 16t reply arrives */
    term.cell_h = 16;
    term.mod_shift = term.mod_ctrl = term.mod_alt = 0;

    /* SDL_TERMINAL_ASPECT forces a display aspect: "W:H" (e.g. "4:3", the right
       ratio for DOS content) or a decimal ("1.333"). Unset/invalid => native. */
    term.forced_ar = 0.0;
    aspect = SDL_getenv("SDL_TERMINAL_ASPECT");
    if (aspect && *aspect) {
        const char *colon = SDL_strchr(aspect, ':');
        if (colon) {
            int num = SDL_atoi(aspect);
            int den = SDL_atoi(colon + 1);
            if (num > 0 && den > 0)
                term.forced_ar = (double)num / (double)den;
        } else {
            double v = SDL_atof(aspect);
            if (v > 0.0)
                term.forced_ar = v;
        }
    }

    /* SDL_TERMINAL_SCALE=N: in fullscreen, present the guest at a fixed integer
       device-pixel scale (OSC 2501 fit=scale;scale=N) instead of stretch/contain —
       for square-pixel guests like Windows where crisp integer magnification beats
       filling the screen. Needs terminal 2501 + fit=scale support; ignored unless
       fullscreen is active. */
    term.want_scale = 0;
    {
        const char *sc = SDL_getenv("SDL_TERMINAL_SCALE");
        if (sc && *sc) {
            int n = SDL_atoi(sc);
            if (n > 0) term.want_scale = n;
        }
    }

    /* Feature flags resolve from a runtime capability probe (auto-enable where the
       terminal supports it) unless an env var forces them. Capture the env intent
       now as a tri-state (-1 auto / 0 force-off / 1 force-on); term_probe_drain
       fills term.cap_*; then select below. Env values: SDL_TERMINAL_XFER=shm|file|t
       forces that transport (unset => auto from cap_shm/cap_file; any other set
       value forces both off, i.e. base64); SDL_TERMINAL_ZLIB=0 forces off (any
       other set value = force on); SDL_TERMINAL_FULLSCREEN=1/0. */
    {
        const char *xfer = SDL_getenv("SDL_TERMINAL_XFER");
        const char *zl   = SDL_getenv("SDL_TERMINAL_ZLIB");
        const char *fs   = SDL_getenv("SDL_TERMINAL_FULLSCREEN");
        int env_file = xfer ? ((SDL_strcmp(xfer, "file") == 0 || SDL_strcmp(xfer, "t") == 0) ? 1 : 0) : -1;
        int env_shm  = xfer ? ((SDL_strcmp(xfer, "shm") == 0) ? 1 : 0) : -1;
        int env_zlib = zl   ? ((zl[0] == '0' && zl[1] == '\0') ? 0 : 1) : -1;
        int env_fs   = fs   ? ((fs[0] == '1' && fs[1] == '\0') ? 1 : 0) : -1;

        term.tmp_ring_i = 0;
        SDL_memset(term.tmp_ring, 0, sizeof(term.tmp_ring));
        term.shm_ring_i = 0;
        term.shm_seq = 0;
        SDL_memset(term.shm_ring, 0, sizeof(term.shm_ring));

        term_zlib_init();   /* load libz (for the o=z probe + compression) unless =0 */
        term_open_io();
        term_enter();       /* raw mode, alt screen, mouse, 14t/16t queries */
        term_probe_drain(); /* probe: cap_file/cap_zlib/cap_2501/cap_2502 (+ geometry) */

        /* env override wins; unset => auto from the probe. */
        term.use_file_xfer = (env_file >= 0) ? (SDL_bool)env_file : term.cap_file;
        term.use_shm_xfer = (env_shm >= 0) ? (SDL_bool)env_shm : term.cap_shm;
        {
            SDL_bool have_lib = (term.z_compress2 && term.z_compressBound) ? SDL_TRUE : SDL_FALSE;
            SDL_bool want = (env_zlib >= 0) ? (SDL_bool)env_zlib : term.cap_zlib;
            term.use_zlib = (want && have_lib) ? SDL_TRUE : SDL_FALSE;
        }
        term.fullscreen = (env_fs >= 0) ? (SDL_bool)env_fs : term.cap_2501;
        if (term.fullscreen)
            term_write_str("\x1b[?2501h"); /* Ubiquitty fullscreen-content mode */

        if (SDL_getenv("SDL_TERMINAL_DEBUG"))
            fprintf(stderr, "[caps] probed file=%d shm=%d zlib=%d 2501=%d 2502=%d -> "
                    "xfer=%s zlib=%s fullscreen=%s mouselook=%s\n",
                    (int)term.cap_file, (int)term.cap_shm, (int)term.cap_zlib, (int)term.cap_2501, (int)term.cap_2502,
                    term.use_shm_xfer ? "shm" : (term.use_file_xfer ? "file" : "base64"),
                    term.use_zlib ? "on" : "off",
                    term.fullscreen ? "on" : "off", term.cap_2502 ? "on" : "off");
    }

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
    /* Claim relative-mouse support so SDL_SetRelativeMouseMode succeeds and routes
       through our hook (-> DECSET 2502). */
    SDL_GetMouse()->SetRelativeMouseMode = TERMINAL_SetRelativeMouseMode;
    return 0;
}

static void TERMINAL_VideoQuit(_THIS)
{
    (void)_this;
    term_leave();   /* term_restore (via term_leave) cleans temp-transport files */
    SDL_free(term.rgb);
    SDL_free(term.b64);
    SDL_free(term.out);
    SDL_free(term.zbuf);
    SDL_free(term.last);
    term.rgb = NULL;
    term.b64 = NULL;
    term.out = NULL;
    term.zbuf = NULL;
    term.last = NULL;
    term.rgb_cap = term.b64_cap = 0;
    term.out_cap = term.out_len = term.out_off = 0;
    term.zbuf_cap = 0;
    term.last_len = term.last_cap = 0;
    term.unchanged_streak = 0;
    if (term.zlib_handle) {
        dlclose(term.zlib_handle);
        term.zlib_handle = NULL;
    }
    term.use_zlib = SDL_FALSE;
    term.z_compress2 = NULL;
    term.z_compressBound = NULL;
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
    struct winsize ws;
    int mx, my, ix, iy, cols = 80, rows = 24, have_ws;
    if (!term.window || term.img_w <= 0 || term.img_h <= 0)
        return;

    if (b & 64) { /* wheel — direction only, position irrelevant in either mode */
        SDL_SendMouseWheel(term.window, 0, 0.0f, (b == 64) ? 1.0f : -1.0f, SDL_MOUSEWHEEL_NORMAL);
        return;
    }

    if (term.relative) {
        /* 2502 pointer-lock: px;py carry center-biased signed deltas (px - 0x8000);
           a button event carries delta 0. Inject as RELATIVE SDL motion so DOS
           mouselook reads xrel/yrel.

           Delta equalization: the deltas are physical device pixels, but the game
           consumes them in SOURCE pixels, and under the 2501 stretch the source is
           displayed with DIFFERENT per-axis gains (640x400 stretched to 4:3 shows
           each vertical source pixel across 1.2x more glass than a horizontal
           one), so raw injection makes the cursor visibly slower horizontally.
           Scale each axis by img/dest — source pixels per device pixel — so one
           physical pixel of motion moves the on-screen cursor one physical pixel,
           both axes alike. Fractional remainders carry across events. */
        int dx = px - 0x8000, dy = py - 0x8000;
        if (term.fullscreen && term.tx_w > 0 && term.tx_h > 0 && term.img_w > 0 && term.img_h > 0) {
            double vW = (term.win_w > 0) ? (double)term.win_w : 0.0;
            double vH = (term.win_h > 0) ? (double)term.win_h : 0.0;
            double destW = 0.0, destH = 0.0;
            if (vW > 0.0 && vH > 0.0) {
                if (term.osc_scale > 0) {
                    destW = (double)term.tx_w * (double)term.osc_scale;
                    destH = (double)term.tx_h * (double)term.osc_scale;
                } else if (term.osc_ar > 0.0) {
                    double A = term.osc_ar;
                    if (vW / vH > A) { destH = vH; destW = destH * A; }
                    else             { destW = vW; destH = destW / A; }
                } else {
                    double sx = vW / (double)term.tx_w;
                    double sy = vH / (double)term.tx_h;
                    double fit = (sx < sy) ? sx : sy;
                    destW = (double)term.tx_w * fit;
                    destH = (double)term.tx_h * fit;
                }
            }
            if (destW > 1e-6 && destH > 1e-6) {
                double fx = term.rel_carry_x + (double)dx * (double)term.img_w / destW;
                double fy = term.rel_carry_y + (double)dy * (double)term.img_h / destH;
                dx = (int)fx;
                dy = (int)fy;
                term.rel_carry_x = fx - (double)dx;
                term.rel_carry_y = fy - (double)dy;
            }
        }
        SDL_SendMouseMotion(term.window, 0, 1, dx, dy);
        if (SDL_getenv("SDL_TERMINAL_DEBUG"))
            fprintf(stderr, "[mouse] rel b=%d px=%d py=%d '%c' -> dx=%d dy=%d\n",
                    b, px, py, final ? final : '?', dx, dy);
        if (!(b & 32)) {
            Uint8 btn = (Uint8)(((b & 3) == 0) ? SDL_BUTTON_LEFT :
                                ((b & 3) == 1) ? SDL_BUTTON_MIDDLE : SDL_BUTTON_RIGHT);
            SDL_SendMouseButton(term.window, 0, (Uint8)((final == 'M') ? SDL_PRESSED : SDL_RELEASED), btn);
        }
        return;
    }

    have_ws = (ioctl(term.outfd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0);
    if (have_ws) { cols = ws.ws_col; rows = ws.ws_row; }
    mx = px - 1; if (mx < 0) mx = 0;
    my = py - 1; if (my < 0) my = 0;

    if (term.fullscreen && term.tx_w > 0 && term.tx_h > 0) {
        /* 2501: invert the terminal's fit of the transmitted image (tx_w x tx_h)
           into the drawable, centered (padding zeroed). px and the viewport share
           the same backing space, so the backing scale cancels. */
        double vW = (term.win_w > 0) ? (double)term.win_w : (double)cols * term.cell_w;
        double vH = (term.win_h > 0) ? (double)term.win_h : (double)rows * term.cell_h;
        double ox, oy;
        if (term.osc_scale > 0) {
            /* OSC 2501 fit=scale: the source is drawn at scale device-pixels per
               source pixel, centered (clipped if larger than the viewport). The
               SGR-pixel mouse coords are in that same device-pixel space, so the
               inverse is backing-independent: ix = (mx - ox) / scale.
               ASSUMES SGR-pixel/CSI-14t are device pixels and scale is the device
               factor — confirm visually; if off by the backing factor, that's the
               one knob to turn. */
            double s = (double)term.osc_scale;
            ox = (vW - (double)term.tx_w * s) / 2.0;
            oy = (vH - (double)term.tx_h * s) / 2.0;
            ix = (int)(((double)mx - ox) / s);
            iy = (int)(((double)my - oy) / s);
        } else if (term.osc_ar > 0.0) {
            /* OSC 2501 fit=stretch: the terminal contain-fits a box of aspect
               osc_ar into the viewport and stretches the whole source to fill it,
               so x and y scale independently. */
            double A = term.osc_ar, destW, destH;
            if (vW / vH > A) { destH = vH; destW = destH * A; }
            else             { destW = vW; destH = destW / A; }
            if (destW < 1e-6) destW = 1e-6;
            if (destH < 1e-6) destH = 1e-6;
            ox = (vW - destW) / 2.0;
            oy = (vH - destH) / 2.0;
            ix = (int)((((double)mx - ox) / destW) * term.img_w);
            iy = (int)((((double)my - oy) / destH) * term.img_h);
        } else {
            /* No display-aspect OSC: the terminal aspect-CONTAIN-fits at the native
               pixel ratio (uniform scale). */
            double sx = vW / (double)term.tx_w;
            double sy = vH / (double)term.tx_h;
            double fit = (sx < sy) ? sx : sy;
            if (fit < 1e-6) fit = 1e-6;
            ox = (vW - (double)term.tx_w * fit) / 2.0;
            oy = (vH - (double)term.tx_h * fit) / 2.0;
            ix = (int)((((double)mx - ox) / fit) * term.img_w / term.tx_w);
            iy = (int)((((double)my - oy) / fit) * term.img_h / term.tx_h);
        }
    } else {
        /* Legacy: image occupies an img_cols x img_rows cell box at top-left.
           Map within it using the PRECISE per-cell advance (CSI 14t width / cols)
           — CSI 16t rounds the cell to an integer, which drifts toward the edges
           under a non-integer grid. */
        double adv_x = (double)term.cell_w, adv_y = (double)term.cell_h, rect_w, rect_h;
        if (have_ws) {
            if (term.win_w > 0) adv_x = (double)term.win_w / cols;
            if (term.win_h > 0) adv_y = (double)term.win_h / rows;
        }
        rect_w = term.img_cols * adv_x;
        rect_h = term.img_rows * adv_y;
        if (rect_w < 1.0) rect_w = 1.0;
        if (rect_h < 1.0) rect_h = 1.0;
        ix = (int)((double)mx * term.img_w / rect_w);
        iy = (int)((double)my * term.img_h / rect_h);
    }
    if (ix < 0) ix = 0; else if (ix >= term.img_w) ix = term.img_w - 1;
    if (iy < 0) iy = 0; else if (iy >= term.img_h) iy = term.img_h - 1;

    if (SDL_getenv("SDL_TERMINAL_DEBUG"))
        fprintf(stderr, "[mouse] b=%d px=%d py=%d '%c' | fs=%d cell=%dx%d win=%dx%d "
                "tx=%dx%d img=%dx%d -> ix=%d iy=%d\n",
                b, px, py, final ? final : '?', (int)term.fullscreen, term.cell_w, term.cell_h,
                term.win_w, term.win_h, term.tx_w, term.tx_h, term.img_w, term.img_h, ix, iy);

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
                if (final == 't') { /* window-op report: [6;h;w t = 16t cell size,
                                       [4;h;w t = 14t text-area pixel size */
                    if (num0 == 6 && num1 > 0 && num2 > 0) {
                        term.cell_h = num1;
                        term.cell_w = num2;
                        if (SDL_getenv("SDL_TERMINAL_DEBUG"))
                            fprintf(stderr, "[16t] cell size reply: cell_w=%d cell_h=%d\n",
                                    term.cell_w, term.cell_h);
                    } else if (num0 == 4 && num1 > 0 && num2 > 0) {
                        term.win_h = num1;
                        term.win_w = num2;
                        if (SDL_getenv("SDL_TERMINAL_DEBUG"))
                            fprintf(stderr, "[14t] text-area px reply: win_w=%d win_h=%d\n",
                                    term.win_w, term.win_h);
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
