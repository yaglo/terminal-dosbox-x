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

`dos` is a thin launcher in the repo root. Under the hood it just runs the
DOSBox-X binary with the terminal driver selected:

```sh
SDL_VIDEODRIVER=terminal src/dosbox-x -set output=surface -nopromptfolder \
    -c 'mount c /path/to/dir' -c c: -c PROG.EXE
```

DOSBox-X's own logging is redirected to `/tmp/dosbox.log` so it doesn't corrupt
the image stream.

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
by the terminal's background; a solid-black, padding-free letterbox is a
terminal-side feature (see the `fullscreen-content` / private DECSET 2501 spec
filed for Ubiquitty).

## Environment variables

These are read by the `terminal` SDL video driver — set them on any SDL2 program
run with `SDL_VIDEODRIVER=terminal`, or pass them through `dos`.

| Variable | Values | Effect |
|---|---|---|
| `SDL_VIDEODRIVER` | `terminal` | Selects this driver. Required. |
| `SDL_TERMINAL_ASPECT` | `W:H` (`4:3`, `16:9`, `5:4`) or a decimal (`1.6`) | Forces the display aspect the frame is letterboxed to. Unset or unparseable ⇒ native aspect (image's pixel ratio when the terminal reports pixel geometry, else fill the grid). `dos` defaults it to `4:3`. |
| `SDL_TERMINAL_ZLIB` | `1` | Opt in to zlib payload compression (kitty `o=z`) — smaller frames on the wire. Off by default; the terminal must support `o=z` (kitty/Ghostty do — verify others). See Performance below. |
| `SDL_TERMINAL_OUT` | path | Write the Kitty/escape stream to a file instead of `/dev/tty` (debug capture). |
| `SDL_TERMINAL_IN` | path | Read terminal input from a file instead of `STDIN`. |

## Performance

- **Skip-unchanged frames (always on).** A frame byte-identical to the previous
  one is dropped entirely — nothing hits the wire — so a static screen, menu, or
  `C:\>` prompt costs ~0 bandwidth. (A keyframe is resent about once a second so
  the image can't desync.) DOS games spend a lot of time on static screens, so
  this is the biggest single win.
- **zlib compression (opt-in, `SDL_TERMINAL_ZLIB=1`).** Deflates each frame
  before base64. Flat DOS content compresses several-fold. Requires terminal
  support for `o=z`; kitty and Ghostty decode it, so verify your terminal first.
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
