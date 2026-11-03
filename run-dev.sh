#!/bin/sh
# Runs Butterscotch on this machine with the software renderer, keeping every
# save file inside ./saves/ in this repository.
#
# By default it starts the FULL game (the launcher data.win at the top of the
# DELTARUNE folder), so the chapter select, the chapter hand-off and every
# chapter's rendering are testable here. Pass a path to run one chapter direct:
#
#   ./run-dev.sh                                            # full game
#   ./run-dev.sh .../chapter2_windows/data.win             # just chapter 2
#   ./run-dev.sh .../DELTARUNE                             # folder, uses its data.win
#   ./run-dev.sh --headless --exit-at-frame 60
#   ./run-dev.sh --perf-log perf.log
#
# Anything after the data.win is passed straight to the runner, so the usual
# debugging flags (--perf-log, --log-file, --trace-frame, --disassemble) work.
#
# Environment:
#   BUTTERSCOTCH_BIN      runner binary to use instead of the built one
#   DELTARUNE_DIR         DELTARUNE install folder (the one holding data.win)
#   BUTTERSCOTCH_SAVES    save folder (default: ./saves next to this script)
#   BUTTERSCOTCH_LOG      set to 0 to stop writing session.log (default: on)
#   STEAM_LIBRARY         Steam library root to search for DELTARUNE
#   BUTTERSCOTCH_EXTRA_ARGS   extra runner arguments, word-split
#
# Two things make this convenient for bug hunting: --save-folder below keeps
# progress out of the Steam install (the runner otherwise saves next to
# data.win, mixing this port's saves with the real game's), and a log file is
# written by default so a crash still leaves its last lines (log.c flushes per
# line, so the file survives a segfault).

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
        # Steam install. The data.win at the top of the DELTARUNE folder is the
        # full-game launcher: it shows the chapter select and then hands control
        # to <chapter>_windows/data.win through game_change. Falling back to a
        # chapter keeps the script usable from a partial copy of the game.
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
            if [ -f "$lib/steamapps/common/DELTARUNE/data.win" ]; then
                DATA=$lib/steamapps/common/DELTARUNE/data.win
                break
            fi
            for chapter in chapter2_windows chapter1_windows chapter3_windows \
                           chapter4_windows chapter5_windows
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
# --- save states -----------------------------------------------------------


# F5/F6 (Start+L1 / Start+R1 on the handheld) store and reload a snapshot of the
# VM next to the other saves. The runner names that file after the folder the
# running data.win lives in (butterscotch-state-chapter5_windows.sav), so every
# chapter and the launcher keep their own snapshot. Do NOT pin one shared path
# here: this script starts the full-game launcher by default, and a single file
# meant a chapter's state was refused as "another game" on the next run.
# Set BUTTERSCOTCH_STATE_PATH yourself only to force one exact file.
# --- log file --------------------------------------------------------------

# Log by default: a crash then leaves its last lines on disk instead of only in
# a scrolling terminal. Skipped if the caller already asked for a log, and
# turned off with BUTTERSCOTCH_LOG=0.
LOG_ARGS=
LOG_PATH=$ROOT/session.log
if [ "${BUTTERSCOTCH_LOG:-1}" != "0" ]; then
    case " $* ${BUTTERSCOTCH_EXTRA_ARGS:-} " in
        *" --log-file "*) ;;
        *) LOG_ARGS="--log-file $LOG_PATH" ;;
    esac
fi

echo "runner:   $BIN"
echo "game:     $DATA"
echo "saves:    $SAVES"echo "renderer: software"
[ -n "$LOG_ARGS" ] && echo "log:      $LOG_PATH"

# --- run -------------------------------------------------------------------

# Word-split on purpose: BUTTERSCOTCH_EXTRA_ARGS and LOG_ARGS hold several
# arguments. set -f stops a '*' in them from being expanded against the cwd.
set -f
# shellcheck disable=SC2086
exec "$BIN" "$DATA" \
    --renderer software \
    --save-folder "$SAVES" \
    $LOG_ARGS \
    ${BUTTERSCOTCH_EXTRA_ARGS:-} \
    "$@"
