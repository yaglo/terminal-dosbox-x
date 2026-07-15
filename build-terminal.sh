#!/usr/bin/env bash
# build-terminal.sh — fast rebuild after editing the SDL2 "terminal" video driver.
#
# The driver source is single-sourced from the sdl-terminal-video repo, pulled in
# as a git submodule at vs/sdl-terminal-video; vs/sdl2/src/video/dummy/
# SDL_terminalvideo.c is a SYMLINK into it. Edit the driver THERE (or in the
# submodule checkout) — not through a second copy. This script makes sure the
# submodule is checked out before building.
#
# Does NOT touch configure/config.h or rebuild the other vendored libraries
# (zlib/libpng/freetype) — it only recompiles the driver object, refreshes the
# vendored SDL2 static lib (vs/sdl2/linux-host), and relinks dosbox-x against
# whichever configuration (release/debug) is currently active.
#
# On a fresh checkout (no prior build) it bootstraps automatically: on macOS,
# with Homebrew available, it installs any missing build tools and runs the
# full ./build-macos-sdl2 build once. On any other platform, or without
# Homebrew, it fails with instructions instead of guessing.
#
# Ends with a headless functional smoke test (boots the driver via the terminal
# video driver and checks it actually emits a kitty graphics frame), so a
# build that compiles but crashes/hangs is caught here, not on next launch.

set -u
cd "$(dirname "${BASH_SOURCE[0]}")"

fail() { echo "build-terminal.sh: $*" >&2; exit 1; }
note() { echo "==> $*"; }

have_cmd() { command -v "$1" >/dev/null 2>&1; }

# The driver lives in the sdl-terminal-video submodule; the vs/sdl2 path is a
# symlink into it. Make sure it's checked out so the symlink resolves before the
# SDL build globs it. (No-op outside a git checkout, e.g. a source tarball where
# the driver was already materialized.)
if [ -f .gitmodules ] && have_cmd git && git rev-parse --git-dir >/dev/null 2>&1; then
    if [ ! -e vs/sdl-terminal-video/src/SDL_terminalvideo.c ]; then
        note "checking out sdl-terminal-video submodule (driver source)"
        git submodule update --init vs/sdl-terminal-video \
            || fail "git submodule update failed — run: git submodule update --init vs/sdl-terminal-video"
    fi
fi

# Portable replacement for GNU coreutils `timeout` (absent on stock macOS).
run_with_timeout() {
    local secs="$1"; shift
    "$@" &
    local pid=$!
    ( sleep "$secs" 2>/dev/null; kill -TERM "$pid" 2>/dev/null ) &
    local watcher=$!
    wait "$pid" 2>/dev/null
    local status=$?
    kill "$watcher" 2>/dev/null
    wait "$watcher" 2>/dev/null
    return $status
}

DRIVER="vs/sdl2/src/video/dummy/SDL_terminalvideo.c"
[ -f "$DRIVER" ] || fail "can't find $DRIVER — run from the dosbox-x repo root"

have_cmd make || fail "'make' not found. Install build tools first (macOS: xcode-select --install; Debian/Ubuntu: apt install build-essential)."

# --- bootstrap: make sure a full build exists before we try to do an incremental one ---
ensure_macos_bootstrap_deps() {
    have_cmd brew || fail "Homebrew not found (needed to auto-install missing build tools). Install it from https://brew.sh, or install these manually: automake autoconf libtool pkg-config."

    local missing=()
    have_cmd aclocal    || missing+=(automake)
    have_cmd autoconf   || missing+=(autoconf)
    have_cmd glibtoolize || missing+=(libtool)
    have_cmd pkg-config || missing+=(pkg-config)

    if [ "${#missing[@]}" -gt 0 ]; then
        note "installing missing build tools via brew: ${missing[*]}"
        brew install "${missing[@]}" || fail "brew install failed for: ${missing[*]} — install manually and re-run."
    fi
}

bootstrap_full_build() {
    note "no existing build found (missing Makefile / vs/sdl2/linux-build) — bootstrapping a full build first"
    case "$(uname -s)" in
        Darwin)
            ensure_macos_bootstrap_deps
            note "running ./build-macos-sdl2 --enable-debug=no (compiles zlib/libpng/freetype/SDL2/SDL2_net/dosbox-x from scratch — this can take several minutes)"
            [ -x ./build-macos-sdl2 ] || chmod +x ./build-macos-sdl2
            # build-macos-sdl2 hardcodes --enable-debug=heavy; override it here so a
            # fresh bootstrap gets a release build instead of C_HEAVY_DEBUG (which adds
            # a breakpoint check on every dynrec block and logs via real write() calls
            # even in normal code paths — measurable overhead, not just debugger cost).
            if ! ./build-macos-sdl2 --enable-debug=no > /tmp/build-terminal-bootstrap.log 2>&1; then
                echo "full bootstrap build FAILED — errors:" >&2
                grep -iE "error:|Error [0-9]|configure: error|failed to complete" /tmp/build-terminal-bootstrap.log >&2
                echo "(full log: /tmp/build-terminal-bootstrap.log)" >&2
                exit 1
            fi
            ;;
        *)
            fail "no existing build found, and auto-bootstrap is only implemented for macOS. Run the appropriate full build script for your platform first (e.g. ./build-sdl2 on Linux), then re-run build-terminal.sh."
            ;;
    esac
    [ -f Makefile ] || fail "bootstrap build reported success but top-level Makefile is still missing — something's wrong, check /tmp/build-terminal-bootstrap.log"
    [ -f vs/sdl2/linux-build/Makefile ] || fail "bootstrap build reported success but vs/sdl2/linux-build/Makefile is still missing — something's wrong, check /tmp/build-terminal-bootstrap.log"
    note "bootstrap OK — continuing with incremental build"
}

if [ ! -f Makefile ] || [ ! -f vs/sdl2/linux-build/Makefile ]; then
    bootstrap_full_build
fi

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
run_with_timeout 8 env SDL_VIDEODRIVER=terminal SDL_TERMINAL_OUT="$SMOKE_OUT" SDL_TERMINAL_IN=/dev/null \
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

if [ -x src/dosbox-x-terminal ]; then
    echo "==> done — src/dosbox-x is ready (run src/dosbox-x-terminal to skip setting SDL_VIDEODRIVER by hand)"
else
    echo "==> done — src/dosbox-x is ready (note: src/dosbox-x-terminal wrapper is missing)"
fi
