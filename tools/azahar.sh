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
flatpak kill org.azahar_emu.Azahar 2>/dev/null || true
wait $PID 2>/dev/null || true
grep -a "Debug.Emulated\|Fatal\|Critical" "$LOG" | sed 's/^.*Debug.Emulated[^>]*> *//' > "$OUT/game.log" || true
echo "log: $OUT/game.log ($(wc -l < "$OUT/game.log") lines)"
