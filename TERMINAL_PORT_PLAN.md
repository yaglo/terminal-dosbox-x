# DOSBox-X → Ubiquitty Terminal Port — Execution Plan

> **Using it?** See [TERMINAL.md](TERMINAL.md) for how to run it (the `dos`
> launcher) and the driver's options (incl. `SDL_TERMINAL_ASPECT`). This file is
> the design/execution plan.

> Goal: make DOSBox-X render and play **inside a terminal** (Ubiquitty) via the
> **Kitty graphics protocol**, translating terminal keyboard/mouse input back
> into the emulator. Model: `~/Projects/terminal-doom`. Both ends are ours
> (Ubiquitty at `~/Projects/ubiquitty` can be extended).
>
> Derived from a 10-agent architecture-mapping workflow (map → synthesize →
> adversarial critique → revise). File:line references were spot-verified in-tree.

> **⚠️ ARCHITECTURE PIVOT (2026-07-10).** Instead of a DOSBox-specific
> `SCREEN_TERMINAL` output backend, we build a **general SDL2 `terminal` video
> driver** in the vendored SDL2 (`vs/sdl2`, v2.32.10), modeled on the `dummy`
> driver. Any pure-SDL2 app runs in the terminal via `SDL_VIDEODRIVER=terminal`;
> DOSBox-X runs with `output=surface` and needs only a couple of macOS-menu
> guards. This dissolves the hardest milestones (keyboard/mouse translation into
> DOSBox internals) because we inject **real SDL events** and DOSBox's mature SDL
> input path handles the rest; aspect correction becomes DOSBox's job (it
> letterboxes into the surface we size to the terminal). The sections (A)–(D)
> below describe the *original* backend plan and are kept as reference — the
> Kitty emit, kitty-keyboard parsing, terminal lifecycle, and Ubiquitty Specs
> A/B are identical either way; only the integration seam moved. New milestone
> track: **D0** scaffold+register driver, build SDL2, selectable → **D1** first
> pixels via a tiny SDL test app → **D2** DOSBox-X renders (surface path +
> macOS-menu guard + stdout diversion) → **D3** keyboard (SDL_SendKeyboardKey)
> → **D4** mouse → **D5** aspect/polish + Ubiquitty specs → **D6** perf.
> Verification note: the tool harness captures bytes but can't *see* Ubiquitty's
> render — I validate Kitty protocol bytes programmatically; the user does the
> visual confirmation in real Ubiquitty.

## (A) Recommended Architecture

Add a new `SCREEN_TERMINAL` output backend that **clones GAMELINK exactly** — a
real, hidden SDL window under the normal (Cocoa) video driver, **not** the SDL
`dummy` driver — living inside the existing `main()`/`Normal_Loop`, driven
one-emit-per-VGA-frame by `RENDER_EndUpdate → GFX_EndUpdate`. The backend hands
the per-scanline scaler a persistent 32bpp framebuffer
(`OUTPUT_TERMINAL_StartUpdate`), then on `EndUpdate` snapshots 32→24bpp and ships
the frame to Ubiquitty via the **Kitty shared-memory/file transport
(`t=s`/`t=f`, optionally `o=z` zlib) as the primary path**, with base64-over-tty
(`a=T,f=24`) kept only as a remote/SSH fallback. A dedicated writer thread does
the encode + `write()` so terminal back-pressure never stalls emulation. Input
is pumped from `GFX_Events` next to the gamelink hook: kitty keyboard CSI-u →
own `KBD_KEYS` table → `KEYBOARD_AddKey` ("design A") for ordinary keys, with a
few essential actions (quit, save/load-state) routed through the mapper. Mouse
uses SGR-pixel absolute coords transformed terminal→image→guest and pushed to
the VMware absolute path plus `Mouse_CursorMoved`/`Mouse_WheelMoved`. The
`dummy`-driver + windowless variant is **deferred** to a future headless/SSH
mode (it removes `sdl.surface`, forcing a `GFX_GetBShift` special-case and a full
audit of every `sdl.window`/`sdl.surface` deref).

```
VGA emulation (vga_draw)
  → RENDER_* scalers (render.cpp)  [writes into framebuf, 32bpp, pitch=draw.width*4]
      ↑ buffer from OUTPUT_TERMINAL_StartUpdate  (mirror output_gamelink.cpp:239)
  → GFX_EndUpdate(changedLines)  (sdlmain.cpp:3301)
      → case SCREEN_TERMINAL: OUTPUT_TERMINAL_EndUpdate  (insert at :3369, beside gamelink)
          → snapshot 32→24  →  [ring buffer / shm handle]
  → OUTPUT_TERMINAL_Transfer  (insert at :3393)
─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ (thread boundary) ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─
  WRITER THREAD: encode (t=s/t=f, o=z; base64 fallback) → write(/dev/tty)
                 → Ubiquitty renders Kitty image i=1 (flicker-free replace), q=2 (no ACK)

Ubiquitty stdin (kitty CSI-u kbd / SGR-pixel mouse / resize / focus)
  → read(/dev/tty non-blocking) in OUTPUT_TERMINAL_InputEvent
      ↑ pumped from GFX_Events  (sdlmain.cpp:5922, beside gamelink hook)
  → keys:  codepoint → KBD_KEYS → KEYBOARD_AddKey  (design A)
           quit / save-load → mapper path
  → mouse: terminal-px → letterbox/image-rect → guest → VMWARE_MousePosition
           (+ Mouse_CursorMoved / Mouse_WheelMoved); VMWARE_ScreenParams synced to letterbox geometry
```

## (B) Staged Milestones

### M0 — Build skeleton + two blocking spikes
Wire `C_TERMINAL`, empty backend links, `output=terminal` selectable; resolve the
two architecture-deciding questions.
- `configure.ac` (~line 605): `AH_TEMPLATE(C_TERMINAL,…)` + `AC_ARG_ENABLE(terminal,…)` + `AC_DEFINE` + `AM_CONDITIONAL([C_TERMINAL],…)`. Do **not** gate on SDL_STRING.
- `src/output/Makefile.am` (~line 18): `if C_TERMINAL / liboutput_a_SOURCES += output_terminal.cpp / endif`.
- New `src/output/output_terminal.h` (mirror `output_gamelink.h:14-24`) + `src/output/output_terminal.cpp` (`#include "config.h"` then `#if C_TERMINAL … #endif`, stub all 9 functions).
- `include/sdlmain.h`: add `SCREEN_TERMINAL` to `enum SCREEN_TYPES` **after `SCREEN_GAMELINK` at `:26`** (enum opens `:19`); add `struct { Bitu pitch; void* framebuf; /* +kitty/tty state */ } terminal;` near the `gamelink` member (~`:134-149`); `#include <output/output_terminal.h>`.
- Re-run `./autogen.sh` (mandatory — else `#if C_TERMINAL` silently reads 0).
- **Spike 1 (P0-2):** confirm the GAMELINK model (real hidden SDL window under the Cocoa driver) works when the controlling terminal is Ubiquitty, and that Kitty output to a separate `/dev/tty` fd is undisturbed by Cocoa `NSApplication`. Decides M1 architecture.
- **Spike 2 (P0-1):** confirm Ubiquitty accepts Kitty `t=s` (shm) and/or `t=f` (file) transmit + `o=z` — reported implemented at `KittyGraphics.cpp:378-381`. Decides M1 transport; largely deletes the throughput risk.

**Acceptance:** `--enable-terminal` compiles/links; `output=terminal` selects the backend without crashing; spike notes recorded. **Difficulty:** Low (skeleton) + Medium (spikes).

### M1 — FIRST PIXELS: one static frame via Kitty
Blit the DOS BIOS/welcome screen once into Ubiquitty, letterboxed with correct 4:3.
- **Early mode detection (P0-3):** `output=terminal` is applied only at the Select dispatch (`sdlmain.cpp:4150-4235`, output string read `:4077`, gamelink Select `:4170`), which runs **after** the main `SDL_Init` at `:9466`. Detect terminal mode from a dedicated CLI flag / env var / early config peek **before** `:9466`.
- **stdout/log diversion (P1-4):** redirect `stdout`/`stderr` + DOSBox log sink to a file/`/dev/null`; open a **separate** `/dev/tty` fd for Kitty output (DOSBox `printf`/`LOG_MSG` would otherwise corrupt the image stream, e.g. `sdlmain.cpp:7135+`).
- **Suppress macOS folder prompt (P1-8):** force `control->opt_promptfolder = 0` for terminal mode before the modal `NSOpenPanel` at `sdlmain.cpp:8692` (`macosx_prompt_folder` `:8715`).
- `OUTPUT_TERMINAL_Select()` (mirror `output_gamelink.cpp:32-56`, dispatch beside `:4170`): `want_type=SCREEN_TERMINAL`, `render.aspectOffload=true`, `fullscreen=false`, `mouse.autoenable=false`.
- `OUTPUT_TERMINAL_SetSize()` (mirror `:162-186`; switch case beside gamelink `sdlmain.cpp:2271`): **allocate `framebuf` to actual `draw.width*draw.height*4` and realloc every SetSize (P2-10)** — a fixed `SCALER_MAXWIDTH*SCALER_MAXHEIGHT*4` (800×600, `render_scalers.h:26-27`) overruns on ≥1024×768 VESA modes. Keep the hidden window. Return `GFX_CAN_32 | GFX_SCALING`.
- `OUTPUT_TERMINAL_StartUpdate`/`EndUpdate` (mirror `:239`/`:271`; `GFX_StartUpdate` case `:3254`, `GFX_EndUpdate` `:3301`, insert case at **`:3369`**, `Transfer` at **`:3393`**). Full-frame; ignore `changedLines` for now.
- Kitty emitter: adapt the terminal-quake3 recipe (`sdl_glimp.c:1194-1308`) **but do NOT lift verbatim (P2-11)** — fold `render.src.ratio` into the display-box `c`/`r` math (aspectOffload means the render layer no longer corrects pixel aspect; raw 320×200 = wrong 8:5). Prefer M0-chosen `t=s`/`t=f` over base64. Alt-screen + hide-cursor once on init.
- `GFX_GetRGB` (`sdlmain.cpp:3456`): `case SCREEN_TERMINAL: return (r)|(g<<8)|(b<<16)|(255u<<24);` — keep byte order consistent with `GFX_GetBShift` (Risk 2/3).
- `GUI_ShutDown` (`sdlmain.cpp:3496`, **not :3533**): restore termios/cursor/alt-screen/pop kitty keyboard.
- `GetBestMode` case (dead on SDL2, add for SDL1/correctness).

**Acceptance:** inside Ubiquitty, the DOS blue welcome/prompt appears as a Kitty image, centered, 4:3-correct, no log garbage. Static/stuttery OK. Confirm audio init didn't abort. **Difficulty:** High.

### M2 — Live video (continuous, dirty-aware)
- Persistent buffer: reallocate only on `SetSize`; never clear mid-run.
- Honor `changedLines` (`output_surface.cpp:749-768`): skip emit when `changedLines[0]==draw.height`.
- Force-redraw on resize/clear via `RENDER_SetForceUpdate(true)` (`render.cpp:342`, Risk 4).
- Policy: `render.scale.hardware=true` for 1:1 (avoid 320×200→640×400 doubling).

**Acceptance:** boot DOS, run a demo/game intro; continuous motion, no flicker (`i=1`), no ACK spam (`q=2`). **Difficulty:** Medium.

### M3 — Keyboard input (playable)
- Raw termios + kitty progressive enhancement (`\x1b[>31u`, report-all-keys flag 8) for release + real modifier up/down (Risk 6).
- `OUTPUT_TERMINAL_InputEvent()` parser (CSI-u event 1/2/3, legacy CSI, ASCII); codepoint → `KBD_KEYS` → `KEYBOARD_AddKey(k,pressed)`. Pump from `GFX_Events` `:5922`.
- **Quit + essential mapper actions (P1-6):** design A bypasses the mapper; define a terminal quit key → clean `GUI_ShutDown` (`:3496`); route quit + save/load-state through `MAPPER_CheckEvent`.
- Stuck-key hygiene: on focus-loss/mode-switch `MAPPER_ReleaseAllKeys()` (`sdl_mapper.cpp:367`).
- **Budget all 11 window-event guards now (P2-9):** hidden window still emits real focus/mouse/resize events; guards at `sdlmain.cpp:6034,6051,6061,6144,6157,6172,6180,6242,6246,6283,6332` + suppressions `:2788,3149,3900,3985,4026,9616`. Behavioral, not deferrable.

**Acceptance:** `DIR`+Enter works; a keyboard game plays with arrows/space; no stuck keys; quit cleanly restores the terminal. **Difficulty:** High.

### M4 — Mouse input (absolute + wheel)
- Enable SGR-pixel/any-motion mouse (`\x1b[?1003;1016h`); parse `\x1b[<b;x;y(M|m)`.
- **Explicit transform (P2-12):** terminal-px → letterbox/image-rect → guest from `TIOCGWINSZ` geometry (available since M1). Keep `VMWARE_ScreenParams` (`mouse.cpp:2780`) synced every resize; feed `VMWARE_MousePosition` (`:2731`) + `Mouse_CursorMoved` + `Mouse_WheelMoved`.
- `Mouse_AutoLock` short-circuit for `SCREEN_TERMINAL` at `sdlmain.cpp:4401`.

**Acceptance:** move/click in a mouse-aware DOS program; cursor tracks across the letterbox; wheel scrolls. **Difficulty:** Medium-High.

### M5 — Aspect-perfect & polished (needs Ubiquitty Specs A + B)
- Query `CSI 16t`/`18t` (or in-band resize DECSET `2048`) at startup + resize for exact cell pixel size; recompute letterbox.
- Emit new Ubiquitty private DECSETs to zero padding + hide/lock pointer when a DOS game requests relative mouse; emit relative deltas when locked.

**Acceptance:** content fills the terminal edge-to-edge, correct 4:3; a mouselook game works with host pointer hidden + continuous deltas. **Difficulty:** Medium (blocked on Ubiquitty specs).

### M6 — Performance & throughput
- **Encode on the writer thread (P2-15):** emulation thread does only the 32→24 snapshot / publishes the shm handle; writer thread does base64/zlib + `write()`.
- Frame coalescing/throttle in `EndUpdate` (cap to refresh; skip when tty write would block). Keep `InputEvent` outside the `DB_POLLSKIP` early-return (`:5900-5905`).
- Optional: Kitty sub-region transmit from `changedLines`.

**Acceptance:** 640×480 game sustains playable framerate; cycle auto-adjust unaffected. **Difficulty:** Medium (largely pre-solved by shm transport).

### Cross-cutting — Terminal restore & audio (do in M1, harden through M3)
- **Robust restore (P1-7):** DOSBox exits via `E_Exit` (throw) and can die on SIGINT/SIGTERM/SIGSEGV. Install signal handlers + hardened `atexit`: pop kitty keyboard (`\x1b[<u`), leave alt-screen, show cursor, `tcsetattr` saved termios — chaining DOSBox's existing handlers. `GUI_ShutDown` alone only covers clean exit.
- **Audio (P1-5):** `SDL_Init(SDL_INIT_AUDIO|VIDEO|TIMER)` at `:9466` is fatal on failure. Add audio to M1 acceptance; for headless support `SDL_AUDIODRIVER=dummy` / soft-fail. Audio plays on **host speakers**, not "in the terminal."

## (C) Risks & Open Questions (ranked)

1. **Dummy-driver vs real-hidden-window architecture fork (P0-2).** Resolve in M0 Spike 1. Recommendation: clone GAMELINK (real driver + hidden window); defer dummy+windowless headless mode.
2. **`GFX_GetBShift()` derefs `sdl.surface` unconditionally** (`render.cpp:162`) — windowless build crashes on palette writes. Mitigated by the real-hidden-window model.
3. **Byte-order consistency `GFX_GetRGB` ↔ `GFX_palette32bpp[]`** (`render.cpp:162-173`). Open: does `vga_draw.cpp` read `GFX_palette32bpp` directly? Experiment: boot 8bpp VGA 320×200, verify palette colors.
4. **Frames with no changes never emit** (`render.cpp:342`) — resize/clear leaves a stale image. Mitigate with `RENDER_SetForceUpdate(true)` / `GFX_CallBackRedraw`.
5. **Throughput** — base64 @ 640×480×60 ≈ 74 MB/s. Largely deleted by shm/file transport (P0-1) + `o=z`; base64 fallback for remote. Confirm M0 Spike 2.
6. **macOS audio/video init on headless hosts (P1-5).** `SDL_AUDIODRIVER=dummy`, soft-fail audio, validate in M0 Spike 1.
7. **Modifiers bitmask vs discrete keys (Risk 6).** kitty report-all-keys (flag 8) via `\x1b[>31u`, else synthesize press/release.
8. **Non-ASCII / composed Unicode** has no clean `KBD_KEYS`. Known limitation; optionally route printable chars via `BIOS_AddKeyToBuffer`.
9. **Clipboard/paste & save states lost under design A (P2-14).** Explicit deferrals; optionally bracketed-paste → `BIOS_AddKeyToBuffer`.
10. **`GetBestMode` dead on SDL2** (`render.cpp:829-833`). Treat `OUTPUT_TERMINAL_SetSize()` return as authoritative; add case for SDL1.

## (D) Ubiquitty Specs to File

Ubiquitty already implements Kitty graphics (`f=24`/`f=32`, **plus `t=s`/`t=f`/`o=z`** — `KittyGraphics.cpp:378-381`), kitty keyboard progressive flags, SGR-pixel/any-motion mouse, focus events, `CSI 16t`/`18t` device-pixel reporting, in-band resize (DECSET `2048`) — **use as-is**. Convention for new toggles: private DECSET + `modes` struct bool + DECRQM report + frontend honors it (precedent: modes `2500`, `7400` in `Modes.cpp:499-501`).

- **Spec A — Runtime no-padding / edge-to-edge content mode** (blocks M5). New private DECSET in `Source/Core/VT100/Modes.cpp setPrivateMode()`, bool in `Source/Core/Terminal.h`, DECRQM report; frontend honors it in `Source/Platform/macOS/UBWindowController.mm` (`gridInsetsForRenderer` ~`:1113`, `windowPadding*FromDefaults` ~`:1140` → 0), through `RenderProfile.mm:133-138`.
- **Spec B — Pointer hide + relative-mouse/pointer-lock** (blocks M5). Add `MousePointerShape::Hidden` to `Source/Platform/TerminalClients.h:28` + name in `Source/Core/OSCColor.cpp:731`, and a dedicated private DECSET in `Modes.cpp`/`Terminal.h`; frontend in `Source/Platform/macOS/UBTerminalView.mm` (~`:3840`): `[NSCursor hide]` + `CGAssociateMouseAndMouseCursorPosition(false)` + warp-to-center + relative deltas when locked.
- **Spec C — OSC 133 suppression** (defer until proven necessary). Config key in `Source/Config/Config.h` + early-return in `handleOSC133` (`Source/Core/OSCShellIntegration.cpp`).
- **Guarantee (not a feature):** Ubiquitty must always report `ws_xpixel/ws_ypixel` or answer `CSI 16t`/`18t`, so aspect-fit never hits the whole-grid fallback.

## Bottom line for the executor
Run **M0's two spikes first** — cheap de-risk of the whole architecture (driver/window model; shm transport that deletes throughput risk). Then drive **M1 (first pixels, 4:3-correct, logs diverted, early-init sequencing)** as the vertical slice. Create `src/output/output_terminal.cpp` (mirror `src/output/output_gamelink.cpp`); adapt the Kitty emitter from `~/Projects/terminal-quake3/ioq3/code/sdl/sdl_glimp.c:1194-1308` (fold in `render.src.ratio` — don't lift verbatim) and the CSI-u parser from terminal-doom's `Parser.zig:557-620`. Specs A + B block only M5 — file/build them in parallel while M1–M4 proceed against Ubiquitty's existing capabilities.
