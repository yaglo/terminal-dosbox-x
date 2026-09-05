# Running DOSBox-X in a terminal

DOSBox-X can render and run entirely inside a terminal emulator that speaks the
[Kitty graphics protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/) —
no window, no X11. It does this through a general-purpose SDL2 **`terminal`
video driver** (`vs/sdl2/src/video/dummy/SDL_terminalvideo.c`), so the same
mechanism works for any pure-SDL2 program, not just DOSBox-X.

Tested against Ubiquitty; also works in kitty and Ghostty. See
[TERMINAL_PORT_PLAN.md](TERMINAL_PORT_PLAN.md) for the architecture and
milestones.

## Quick start

```sh
# Mount a program's folder as C: and run it (basename becomes an 8.3 short name):
dos ~/Downloads/harm0697/harm.exe

# Mount a folder as C: and drop to the C:\> prompt:
dos ~/games/

# Ctrl-C to quit.
```

`dos` is a thin launcher on your `PATH` (e.g. `~/.local/bin/dos`). Under the hood
it just runs the DOSBox-X binary with the terminal driver selected:

```sh
SDL_VIDEODRIVER=terminal src/dosbox-x -set output=surface -nopromptfolder \
    -c 'mount c /path/to/dir' -c c: -c PROG.EXE
```

DOSBox-X's own logging is redirected to `/tmp/dosbox.log` so it doesn't corrupt
the image stream.

## Automatic capability detection

At startup the driver **probes the terminal** and turns on each optional feature
only where it's supported — no flags needed. It sends:

- **`DECRQM`** (`CSI ?7402$p`, `CSI ?7403$p`) to detect Ubiquitty's fullscreen
  (7402) and mouselook (7403) private modes;
- a tiny **1×1 kitty transmit** over `t=t` (file transport), `t=s` (shared
  memory), and one with `o=z` (zlib), reading the `;OK`/`;E…` ACK to detect each;
- a trailing **Primary DA** (`CSI c`) as a guaranteed terminator so the probe
  never hangs on a terminal that stays silent.

Replies are drained in ~a few hundred ms (with a grace window for terminals that
decode graphics off-thread and ACK after the DA). On Ubiquitty you get
shared-memory transport + zlib + fullscreen + mouselook automatically (shared
memory wins over file transport when both probe true — see Performance below);
on a plain kitty-graphics terminal you get whatever it supports; on anything
else it falls back cleanly to base64. Each feature can still be forced on/off
with the env vars below (an explicit value always wins over the probe).

## Display aspect ratio (4:3)

DOS software was drawn for **4:3 CRTs** with non-square pixels, so a raw
pixel-for-pixel render (e.g. 320×200 = 8:5) looks horizontally stretched. The
driver can letterbox the frame to a chosen display aspect.

**`dos` defaults to 4:3.** Override it with the `SDL_TERMINAL_ASPECT` variable:

```sh
dos game.exe                         # 4:3 (default)
SDL_TERMINAL_ASPECT=16:9 dos game.exe  # widescreen
SDL_TERMINAL_ASPECT=native dos game.exe # image's own pixel ratio / fill the grid
```

The image box is snapped to whole terminal cells, so the aspect is accurate to
within about one cell (≈1% on a normal-size grid). The residual bars are filled
by the terminal's background. For a pixel-perfect, edge-to-edge, solid-black
letterbox, use fullscreen mode:

## Fullscreen (Ubiquitty DECSET 7402)

**Auto-enabled** when the terminal supports DECSET 7402 (detected via DECRQM at
startup). Force it with `SDL_TERMINAL_FULLSCREEN=1`, or off with `=0`.

In fullscreen the terminal zeroes its padding, fits the frame to the whole window
at retina precision, and paints a solid-black letterbox — cleaner than the
cell-quantized in-grid placement. The driver sends the frame at its **native
resolution** (no resampling) and drops its own cell box; all scaling is the
terminal's.

To correct DOS's non-square pixels (320×200 is drawn for 4:3, not its raw 8:5),
the driver declares the intended display aspect to the terminal via **OSC 7402**
(`fit=stretch;ar=W:H`), taken from DOSBox's per-mode aspect — so text and graphics
modes each display at the right shape and flip automatically as the guest changes
modes. One feature, one number: the OSC that configures the mode answers on the
same number as the DECSET that turns it on. (It was OSC 2501 while the private
modes lived at 25xx; terminals accept that older spelling for one release.) On a
terminal that implements 7402 but lacks the display-aspect knob, the `ar` is
ignored and the frame shows at its native pixel ratio instead. The driver emits `?7402h` on start (when
enabled) and `?7402l` on exit.

For **square-pixel guests like Windows**, stretch-to-fill isn't what you want —
set `SDL_TERMINAL_SCALE=N` for a fixed integer magnification (`fit=scale;scale=N`),
crisp and centered, at whatever size you choose (`scale=3` ≈ 1.5× logical on a 2×
Retina display) rather than filling the whole window.

## Mouse & mouselook

Absolute mouse works out of the box. For relative mouselook (and for how Windows/
games move the guest cursor), DOSBox has to **capture** the mouse first — and it
only does that when its mouse is *locked*. `dos` runs with `autolock=true`, so
once a game or Windows grabs the mouse, **click once in the window** (or press
**Ctrl-F10**) to capture. At that point DOSBox switches SDL into relative mode and
the driver enters the terminal's **pointer-lock** (Ubiquitty DECSET 7403): the
terminal hides and confines the OS cursor and reports relative deltas, which the
driver feeds to the game. On Ubiquitty, **hold ⌃⌘** to break out and get your
cursor back.

Note: DOSBox-X defaults `autolock` to *off*, so without it (a bare
`SDL_VIDEODRIVER=terminal src/dosbox-x …`) a click won't capture — only Ctrl-F10
will. Requires a terminal that implements DECSET 7403 for the relative path;
absolute mouse works everywhere regardless.

## Environment variables

These are read by the `terminal` SDL video driver — set them on any SDL2 program
run with `SDL_VIDEODRIVER=terminal`, or pass them through `dos`.

| Variable | Values | Effect |
|---|---|---|
| `SDL_VIDEODRIVER` | `terminal` | Selects this driver. Required. |
| `SDL_TERMINAL_ASPECT` | `W:H` (`4:3`, `16:9`, `5:4`) or a decimal (`1.6`) | Forces the display aspect (legacy cell box, and the fullscreen OSC 7402 `ar`). Unset ⇒ DOSBox's per-mode aspect (`DOSBOX_DISPLAY_ASPECT` hint, ~4:3 for DOS); unparseable ⇒ native pixel ratio. |
| `SDL_TERMINAL_SCALE` | `N` (integer ≥ 1) | Fullscreen only: present the guest at a **fixed integer device-pixel scale** (OSC 7402 `fit=scale;scale=N`) instead of stretch/contain — crisp integer magnification for **square-pixel guests like Windows**, not fill-to-screen. On a 2× Retina display `scale=3` renders at 1.5× logical size. Overrides `SDL_TERMINAL_ASPECT`; needs terminal `fit=scale` support. |
| `SDL_TERMINAL_FULLSCREEN` | `1` / `0` | Force Ubiquitty fullscreen-content mode (DECSET 7402) on / off. **Default: auto** — enabled when the terminal answers DECRQM for 7402. See below. |
| `SDL_TERMINAL_ZLIB` | `1` / `0` | Force zlib payload compression (kitty `o=z`) on / off. **Default: auto** — enabled when the startup probe confirms `o=z` (and libz loads). See Performance below. |
| `SDL_TERMINAL_XFER` | `shm` / `file` (or `t`) / anything else | Force kitty **shared-memory transport** (`t=s`), **file transport** (`t=t`), or base64. **Default: auto** — each is enabled when the startup probe confirms it; shared memory wins over file when both probe true. See Performance below. |
| `SDL_TERMINAL_OUT` | path | Write the Kitty/escape stream to a file instead of `/dev/tty` (debug capture). |
| `SDL_TERMINAL_IN` | path | Read terminal input from a file instead of `STDIN`. |

## Performance

- **Skip-unchanged frames (always on).** A frame byte-identical to the previous
  one is dropped entirely — nothing hits the wire — so a static screen, menu, or
  `C:\>` prompt costs ~0 bandwidth. (A keyframe is resent about once a second so
  the image can't desync.) DOS games spend a lot of time on static screens, so
  this is the biggest single win.
- **Shared-memory transport (auto; `SDL_TERMINAL_XFER=shm` to force).** The
  fastest path: each frame goes into a POSIX shm object and only its short name
  crosses the pty as a kitty `t=s` transmit — no filesystem, no per-frame pixel
  base64. Auto-enabled when the startup probe confirms `t=s`, and wins over file
  transport when both probe true. Sent as `f=32` (RGBA) so the terminal can map
  and upload it with no widen step. Doesn't compose with `o=z` — memory isn't a
  wire, so zlib is skipped under shm regardless of that setting.
- **File transport (auto; `SDL_TERMINAL_XFER=file`/`base64` to force).** The
  next-best win for a local terminal without `t=s` support: each frame is
  written to a temp file (`$TMPDIR/ubterm-tty-graphics-protocol-*`) and only its
  path is sent over the pty as a kitty `t=t` transmit. This skips the per-frame
  base64 of the pixels *and* takes the ~megabyte frame off the pty entirely — it
  moves through the filesystem page cache, and the terminal reads it off its
  async decode queue. The terminal deletes the temp file after reading.
  Auto-enabled when the startup probe confirms `t=t`. Sent as `f=24` (tight RGB,
  no alpha channel) since — unlike shm — zlib compression still applies here and
  a smaller pre-compression frame deflates faster. Composes with `o=z` (the temp
  file is then compressed).
- **zlib compression (auto; `SDL_TERMINAL_ZLIB=1`/`0` to force).** Deflates each
  frame before transmit. Flat DOS content (text modes, 16/256-color screens)
  compresses several-fold, so a low-color frame isn't sent at full raw size.
  Auto-enabled when the startup probe confirms `o=z` (kitty/Ghostty/Ubiquitty
  support it). Note: deflate is CPU-heavy per frame on high-entropy (detailed
  SVGA) content — force it off there with `SDL_TERMINAL_ZLIB=0` if it costs more
  than it saves.
- **Output is blocking.** Frames are written with blocking I/O, which gives
  natural back-pressure (the emulator paces to the terminal). Non-blocking
  frame-drop isn't used: output goes to `/dev/tty` (because `stdout` is the log
  file), and `/dev/tty` can't be polled for write-readiness on macOS — so a true
  drop-under-back-pressure path would need a dedicated writer thread.

## Requirements

- A terminal emulator that supports the **Kitty graphics protocol** and,
  ideally, the **Kitty keyboard protocol** (for key-hold / modifier handling)
  and **SGR-pixel mouse** reporting.
- The DOSBox-X binary built with the vendored SDL2 in `vs/sdl2` (its build
  includes the `terminal` driver). See [BUILD.md](BUILD.md).
