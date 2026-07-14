#!/usr/bin/env bash
# build-terminal.sh — fast rebuild after editing the SDL2 "terminal" video driver
# (vs/sdl2/src/video/dummy/SDL_terminalvideo.c).
#
# Does NOT touch configure/config.h or rebuild the other vendored libraries
# (zlib/libpng/freetype) — it only recompiles the driver object, refreshes the
# vendored SDL2 static lib (vs/sdl2/linux-host), and relinks dosbox-x against
# whichever configuration (release/debug) is currently active. For a full
# from-scratch build use ./build-macos-sdl2 instead.
#
# Ends with a headless functional smoke test (boots the driver via the terminal
# video driver and checks it actually emits a kitty graphics frame), so a
# build that compiles but crashes/hangs is caught here, not on next launch.

set -u
cd "$(dirname "${BASH_SOURCE[0]}")"

fail() { echo "build-terminal.sh: $*" >&2; exit 1; }

DRIVER="vs/sdl2/src/video/dummy/SDL_terminalvideo.c"
[ -f "$DRIVER" ] || fail "can't find $DRIVER — run from the dosbox-x repo root"

echo "==> compiling terminal driver + vendored SDL2 static lib"
rm -f vs/sdl2/linux-build/build/SDL_terminalvideo.lo
(
    cd vs/sdl2/linux-build || exit 1
    make -j3 && make install
) > /tmp/build-terminal-sdl2.log 2>&1
if [ $? -ne 0 ]; then
    echo "SDL2/driver build FAILED — errors:" >&2
    grep -iE "error:" /tmp/build-terminal-sdl2.log >&2
    echo "(full log: /tmp/build-terminal-sdl2.log)" >&2
    exit 1
fi
echo "    OK"

echo "==> relinking dosbox-x"
make -j4 > /tmp/build-terminal-dosbox.log 2>&1
if [ $? -ne 0 ]; then
    echo "dosbox-x link FAILED — errors:" >&2
    grep -iE "error:|Undefined symbols" /tmp/build-terminal-dosbox.log >&2
    echo "(full log: /tmp/build-terminal-dosbox.log)" >&2
    exit 1
fi
[ -x src/dosbox-x ] || fail "link reported success but src/dosbox-x is missing"
echo "    OK  ($(ls -lh src/dosbox-x | awk '{print $5}'))"

echo "==> smoke test: booting through the terminal driver"
SMOKE_OUT="$(mktemp)"
timeout 8 env SDL_VIDEODRIVER=terminal SDL_TERMINAL_OUT="$SMOKE_OUT" SDL_TERMINAL_IN=/dev/null \
    src/dosbox-x -set output=surface -nopromptfolder -c exit > /tmp/build-terminal-smoke.log 2>&1
bytes=$(wc -c < "$SMOKE_OUT" 2>/dev/null || echo 0)
frame=$(grep -aoE '\x1b_Gq=2,a=T[^;]*' "$SMOKE_OUT" 2>/dev/null | head -1)
rm -f "$SMOKE_OUT"
if [ -z "$frame" ]; then
    echo "SMOKE TEST FAILED — driver did not emit a kitty graphics frame (got $bytes bytes)." >&2
    echo "(full log: /tmp/build-terminal-smoke.log)" >&2
    exit 1
fi
echo "    OK  ($bytes bytes, frame: $frame)"

echo "==> done — src/dosbox-x is ready"
