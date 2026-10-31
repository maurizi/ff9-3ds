#!/usr/bin/env bash
# Run a 3DS build in Azahar on a private X display and capture what it shows.
#   tools/azahar.sh [-t SECONDS] [-s "T1 T2 ..."] [-o OUTDIR] [build/ff9.3dsx]
# Screenshots of the emulator window at each time in -s go to OUTDIR/shot_<T>.png; the game's log
# (svcOutputDebugString lines) to OUTDIR/game.log. The display (:47, private to this project) is started if needed and left running.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SECS=30; SHOTS=""; OUT="$ROOT/build/azahar"; DISP=${AZAHAR_DISPLAY:-:47}
while getopts "t:s:o:" o; do case $o in t) SECS=$OPTARG;; s) SHOTS=$OPTARG;; o) OUT=$OPTARG;; *) exit 1;; esac; done
shift $((OPTIND - 1))
APP="$(readlink -f "${1:-$ROOT/build/ff9.3dsx}")"
mkdir -p "$OUT"; rm -f "$OUT"/shot_*.png
LOG=~/.var/app/org.azahar_emu.Azahar/data/azahar-emu/log/azahar_log.txt
if ! xdpyinfo -display "$DISP" >/dev/null 2>&1; then
    (Xvfb "$DISP" -screen 0 1280x960x24 >/dev/null 2>&1 &); sleep 1
fi
DISPLAY=$DISP flatpak run org.azahar_emu.Azahar "$APP" > "$OUT/azahar.out" 2>&1 &
PID=$!
start=$(date +%s.%N)
for t in $SHOTS; do
    now=$(date +%s.%N); d=$(echo "$start + $t - $now" | bc); [ "${d:0:1}" = "-" ] || sleep "$d"
    import -display "$DISP" -window root "$OUT/shot_$t.png" 2>/dev/null || true
done
now=$(date +%s.%N); d=$(echo "$start + $SECS - $now" | bc); [ "${d:0:1}" = "-" ] || sleep "$d"
# SIGTERM first (exact process name: never a pattern, which could match other shells), so its log is flushed
pkill -TERM -x azahar 2>/dev/null || true
for _ in 1 2 3 4 5 6; do pgrep -x azahar >/dev/null || break; sleep 0.5; done
flatpak kill org.azahar_emu.Azahar 2>/dev/null || true
wait $PID 2>/dev/null || true
# the game's own log (port/3ds/console.c), and the emulator's errors
cp ~/.var/app/org.azahar_emu.Azahar/data/azahar-emu/sdmc/3ds/ff9/log.txt "$OUT/game.log" 2>/dev/null || : > "$OUT/game.log"
grep -a "Fatal\|Critical" "$LOG" >> "$OUT/game.log" || true
echo "log: $OUT/game.log ($(wc -l < "$OUT/game.log") lines)"
