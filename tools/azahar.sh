#!/usr/bin/env bash
# Run a 3DS build in Azahar on a private X display and capture what it shows.
#   tools/azahar.sh [-t SECONDS] [-s "T1 T2 ..."] [-o OUTDIR] [build/ff9-test.3dsx]
# (default: the test build, which make writes when any test option is set, else build/ff9.3dsx)
# Screenshots of the emulator window at each time in -s go to OUTDIR/shot_<T>.png; the game's log
# (svcOutputDebugString lines) to OUTDIR/game.log. The display (:47, private to this project) is started if needed and left running.
# AZAHAR_BIN=/path/to/azahar runs a native (self-built) Azahar instead of the flatpak, with the portable user directory
# AZAHAR_HOME/user (config, sdmc, log; AZAHAR_HOME defaults to the binary's directory); it is stopped by its PID only,
# so a flatpak Azahar run by another session is left alone. Its environment (e.g. AZAHAR_PICA_F24) is passed through.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SECS=30; SHOTS=""; OUT="$ROOT/build/azahar"; DISP=${AZAHAR_DISPLAY:-:47}
while getopts "t:s:o:" o; do case $o in t) SECS=$OPTARG;; s) SHOTS=$OPTARG;; o) OUT=$OPTARG;; *) exit 1;; esac; done
shift $((OPTIND - 1))
DEF="$ROOT/build/ff9.3dsx"
[ "$ROOT/build/ff9-test.3dsx" -nt "$DEF" ] && DEF="$ROOT/build/ff9-test.3dsx"
APP="$(readlink -f "${1:-$DEF}")"
mkdir -p "$OUT"; rm -f "$OUT"/shot_*.png
if [ -n "${AZAHAR_BIN:-}" ]; then
    AZAHAR_HOME=${AZAHAR_HOME:-$(dirname "$(readlink -f "$AZAHAR_BIN")")}
    DATA="$AZAHAR_HOME/user"
else
    DATA=~/.var/app/org.azahar_emu.Azahar/data/azahar-emu
fi
LOG="$DATA/log/azahar_log.txt"
if ! xdpyinfo -display "$DISP" >/dev/null 2>&1; then
    (Xvfb "$DISP" -screen 0 1280x960x24 >/dev/null 2>&1 &); sleep 1
fi
if [ -n "${AZAHAR_BIN:-}" ]; then
    (cd "$AZAHAR_HOME" && DISPLAY=$DISP exec "$AZAHAR_BIN" "$APP") > "$OUT/azahar.out" 2>&1 &
else
    DISPLAY=$DISP flatpak run org.azahar_emu.Azahar "$APP" > "$OUT/azahar.out" 2>&1 &
fi
PID=$!
start=$(date +%s.%N)
for t in $SHOTS; do
    now=$(date +%s.%N); d=$(echo "$start + $t - $now" | bc); [ "${d:0:1}" = "-" ] || sleep "$d"
    import -display "$DISP" -window root "$OUT/shot_$t.png" 2>/dev/null || true
done
now=$(date +%s.%N); d=$(echo "$start + $SECS - $now" | bc); [ "${d:0:1}" = "-" ] || sleep "$d"
# SIGTERM first (exact process name: never a pattern, which could match other shells), so its log is flushed
if [ -n "${AZAHAR_BIN:-}" ]; then
    kill -TERM $PID 2>/dev/null || true
    for _ in 1 2 3 4 5 6; do kill -0 $PID 2>/dev/null || break; sleep 0.5; done
    kill -KILL $PID 2>/dev/null || true
else
pkill -TERM -x azahar 2>/dev/null || true
for _ in 1 2 3 4 5 6; do pgrep -x azahar >/dev/null || break; sleep 0.5; done
flatpak kill org.azahar_emu.Azahar 2>/dev/null || true
fi
wait $PID 2>/dev/null || true
# the game's own log (port/3ds/console.c), and the emulator's errors
cp "$DATA/sdmc/3ds/ff9/log.txt" "$OUT/game.log" 2>/dev/null || : > "$OUT/game.log"
grep -a "Fatal\|Critical" "$LOG" >> "$OUT/game.log" || true
echo "log: $OUT/game.log ($(wc -l < "$OUT/game.log") lines)"
