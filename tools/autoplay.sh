#!/bin/bash
# Unattended test runs: start the game with an autoplay script (DXH_AUTOPLAY, see
# src/host/api/autoplay.m) again and again, sort each run into ok / crash / hang /
# timeout and keep its log and screenshots. The Steam save folder is backed up
# before the first run and restored after every run (as are the launcher
# preferences), so bot autosaves never replace the player's saves.
#
#   tools/autoplay.sh SCRIPT [RUNS] [MINUTES_PER_RUN] [OUT_DIR]
#
# Extra environment (DXH_RENDER_SCALE, DXH_GL_NOSHADOW, DXCPU_TSO, ...) is passed
# through to the game; DXH_HOST selects another dxhost build.
set -u
cd "$(dirname "$0")/.."
SCRIPT=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
RUNS=${2:-10}
MINUTES=${3:-30}
OUT=${4:-autoplay-out/$(date +%Y%m%d-%H%M%S)}
APP=${HR_APP:-games/hr/Deus Ex Human Revolution - Directors Cut.app}
HOST=${DXH_HOST:-build/arm64-dxcpu/dxhost}
SAVES=$(ls -d "$HOME/Library/Application Support/Steam/userdata/"*/238010/remote 2>/dev/null | head -1)
PREFS="$HOME/Library/Application Support/Feral Interactive/Deus Ex Human Revolution - Directors Cut/Preferences Data"

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
if [ -n "$SAVES" ] && [ ! -d "$OUT/save-backup" ]; then
    cp -Rp "$SAVES" "$OUT/save-backup"
    echo "saves backed up to $OUT/save-backup"
fi
[ -f "$PREFS" ] && [ ! -f "$OUT/prefs-backup" ] && cp -p "$PREFS" "$OUT/prefs-backup"
restore_saves() {
    # the launcher rewrites its preferences (launch count, detected scale): keep the player's
    [ -f "$OUT/prefs-backup" ] && cp -p "$OUT/prefs-backup" "$PREFS"
    [ -n "$SAVES" ] && [ -d "$OUT/save-backup" ] || return
    if ! diff -rq "$OUT/save-backup" "$SAVES" >/dev/null 2>&1; then
        rm -rf "$SAVES" && cp -Rp "$OUT/save-backup" "$SAVES"
        echo "  saves restored"
    fi
}
PID=
trap '[ -n "$PID" ] && kill $PID 2>/dev/null; sleep 2; restore_saves; exit 130' INT TERM

for ((i = 1; i <= RUNS; i++)); do
    RUN=$(printf "%s/run-%03d" "$OUT" "$i")
    mkdir -p "$RUN"
    SEED=${DXH_AUTOPLAY_SEED:-$((RANDOM * 32768 + RANDOM))}
    # skip Feral's launcher (it sometimes ignored the bot's Return); the
    # player's preferences come back from the backup after the run
    [ -f "$PREFS" ] && sed -i.bak 's/"GameOptionsDialogShouldShow" type="integer">1/"GameOptionsDialogShouldShow" type="integer">0/' "$PREFS" && rm -f "$PREFS.bak"
    START=$(date +%s)
    echo "run $i: seed $SEED, $(date +%H:%M:%S)"
    DXH_AUTOPLAY="$SCRIPT" DXH_AUTOPLAY_OUT="$RUN" DXH_AUTOPLAY_SEED=$SEED \
        DXH_AUTOPLAY_SHOTS=${DXH_AUTOPLAY_SHOTS:-60} \
        DXH_RENDER_SCALE=${DXH_RENDER_SCALE:-2} DXH_FPS=${DXH_FPS:-10} DXH_SLOW_MS=${DXH_SLOW_MS:-50} \
        DXH_LOG=${DXH_LOG:-1} DXH_LOG_FILE="$RUN/game.log" \
        "$HOST" "$APP" >"$RUN/stdout.log" 2>&1 &
    PID=$!
    while kill -0 $PID 2>/dev/null; do
        if (($(date +%s) - START > MINUTES * 60)); then
            kill $PID 2>/dev/null
            sleep 5
            kill -9 $PID 2>/dev/null
            echo timeout >"$RUN/killed"
            break
        fi
        sleep 2
    done
    wait $PID 2>/dev/null
    CODE=$?
    SECS=$(($(date +%s) - START))
    if grep -q "=== fatal signal" "$RUN/game.log" 2>/dev/null; then
        RESULT=crash
    elif grep -q "bot: HANG" "$RUN/game.log" 2>/dev/null; then
        RESULT=hang
    elif [ -f "$RUN/killed" ]; then
        RESULT=timeout
    elif [ $CODE -eq 0 ]; then
        RESULT=ok
    else
        RESULT="exit-$CODE"
    fi
    LAST=$(grep "bot: " "$RUN/game.log" 2>/dev/null | tail -1 | sed 's/^.*bot: //')
    FPS=$(grep "fps:" "$RUN/game.log" 2>/dev/null | tail -1 | sed 's/^.*fps: //')
    LINE="run $i seed $SEED: $RESULT after ${SECS}s (exit $CODE); last bot action: $LAST; $FPS"
    echo "  $LINE"
    echo "$LINE" >>"$OUT/summary.txt"
    restore_saves
done
echo "summary: $OUT/summary.txt"
