#!/bin/sh
# Runs Butterscotch on this machine with the software renderer, keeping every
# save file inside ./saves/ in this repository.
#
#   ./run-dev.sh                       # auto-detect the game data
#   ./run-dev.sh /path/to/data.win     # use a specific data.win (or its folder)
#   ./run-dev.sh --headless --exit-at-frame 60
#
# Anything after the data.win is passed straight to the runner, so the usual
# debugging flags (--perf-log, --log-file, --trace-frame, --disassemble) work:
#
#   ./run-dev.sh --renderer software --perf-log perf.log --log-file session.log
#
# Environment:
#   BUTTERSCOTCH_BIN      runner binary to use instead of the built one
#   DELTARUNE_DIR         folder holding the chapter's data.win
#   BUTTERSCOTCH_SAVES    save folder (default: ./saves next to this script)
#   STEAM_LIBRARY         Steam library root to search for DELTARUNE
#   BUTTERSCOTCH_EXTRA_ARGS   extra runner arguments, word-split
#
# The save folder is the point of this script: without --save-folder the runner
# saves next to data.win, which for a Steam install is a read-only place to keep
# progress and mixes this port's saves with the real game's.

set -eu

ROOT=$(cd "$(dirname "$0")" && pwd)
SAVES=${BUTTERSCOTCH_SAVES:-$ROOT/saves}

die() {
    echo "run-dev.sh: $*" >&2
    exit 1
}

# --- runner binary ---------------------------------------------------------

if [ -n "${BUTTERSCOTCH_BIN:-}" ]; then
    BIN=$BUTTERSCOTCH_BIN
else
    BIN=
    for candidate in \
        "$ROOT/build-miyoo-sdl2/butterscotch" \
        "$ROOT/build/butterscotch" \
        "$ROOT/build-release/butterscotch"
    do
        if [ -x "$candidate" ]; then
            BIN=$candidate
            break
        fi
    done
    [ -n "$BIN" ] || die "no build found. Build one first, or set BUTTERSCOTCH_BIN.
  cmake -S '$ROOT' -B '$ROOT/build-miyoo-sdl2' -DBACKEND=sdl2 -DRENDERER=sw
  cmake --build '$ROOT/build-miyoo-sdl2' -j\$(nproc)"
fi

# --- game data -------------------------------------------------------------

# A directory argument means "the data.win in here"; a file argument is used as
# is. Everything that does not look like a path is left for the runner.
DATA=
if [ $# -gt 0 ]; then
    case "$1" in
        /*|./*|../*|*.win|*.unx)
            if [ -d "$1" ]; then
                DATA=$1/data.win
            else
                DATA=$1
            fi
            shift
            ;;
    esac
fi

if [ -z "$DATA" ]; then
    if [ -n "${DELTARUNE_DIR:-}" ]; then
        DATA=$DELTARUNE_DIR/data.win
    elif [ -f "$ROOT/data.win" ]; then
        DATA=$ROOT/data.win
    else
        # Steam install, chapter 2 first: it is what this port is tested
        # against, and the later chapters' fullscreen effects are the ones that
        # still need work.
        for lib in \
            "${STEAM_LIBRARY:-}" \
            "$HOME/.steam/steam" \
            "$HOME/.local/share/Steam" \
            "$HOME/Ігри/SteamLibrary" \
            "$HOME/Games/SteamLibrary" \
            "$HOME/.var/app/com.valvesoftware.Steam/data/Steam"
        do
            [ -n "$lib" ] || continue
            [ -d "$lib" ] || continue
            for chapter in chapter2_windows chapter1_windows chapter3_windows \
                           chapter4_windows chapter5_windows DELTARUNE
            do
                if [ -f "$lib/steamapps/common/DELTARUNE/$chapter/data.win" ]; then
                    DATA=$lib/steamapps/common/DELTARUNE/$chapter/data.win
                    break
                fi
            done
            [ -n "$DATA" ] && break
        done
    fi
fi

[ -n "$DATA" ] || die "could not find a data.win.
Pass one explicitly:  $0 /path/to/data.win
or point DELTARUNE_DIR at the folder that holds it."
[ -f "$DATA" ] || die "no such file: $DATA"

# --- save folder -----------------------------------------------------------

mkdir -p "$SAVES"
# Absolute, because the runner may change directory and would otherwise write
# ./saves relative to wherever it ends up.
SAVES=$(cd "$SAVES" && pwd)

echo "runner:   $BIN"
echo "game:     $DATA"
echo "saves:    $SAVES"
echo "renderer: software"

# --- run -------------------------------------------------------------------

# Word-split on purpose: BUTTERSCOTCH_EXTRA_ARGS holds several arguments. set -f
# stops a '*' in them from being expanded against the current directory.
set -f
# shellcheck disable=SC2086
exec "$BIN" "$DATA" \
    --renderer software \
    --save-folder "$SAVES" \
    ${BUTTERSCOTCH_EXTRA_ARGS:-} \
    "$@"
