#!/usr/bin/env bash
# Every scenario fixture on the 3DS build in Azahar: builds one executable per fixture (build/smoke/<name>.3dsx), runs
# each for SECS seconds, and reports the ones whose log has a fatal error; a screenshot of each at the end goes to
# build/smoke/<name>.png.   tools/smoke3ds.sh [-t SECS] [tests/scenarios/X.cfg ...]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SECS=75
while getopts "t:" o; do case $o in t) SECS=$OPTARG;; *) exit 1;; esac; done
shift $((OPTIND - 1))
CFGS=("$@")
[ ${#CFGS[@]} -eq 0 ] && CFGS=("$ROOT"/tests/scenarios/*.cfg)
SD=~/.var/app/org.azahar_emu.Azahar/data/azahar-emu/sdmc/3ds/ff9
OUT="$ROOT/build/smoke"
mkdir -p "$OUT" "$SD/scenarios"
for c in "${CFGS[@]}"; do
    n=$(basename "$c" .cfg)
    cp "$c" "$SD/scenarios/"
    "$ROOT/tools/docker.sh" make -j12 3ds "SCENARIO=sdmc:/3ds/ff9/scenarios/$n.cfg" >"$OUT/$n.build.txt" 2>&1 \
        && cp "$ROOT/build/ff9-test.3dsx" "$OUT/$n.3dsx" || echo "$n: BUILD FAILED"
done
"$ROOT/tools/docker.sh" make -j12 3ds >/dev/null 2>&1 # (the player build back)
fail=0
for c in "${CFGS[@]}"; do
    n=$(basename "$c" .cfg)
    [ -f "$OUT/$n.3dsx" ] || continue
    "$ROOT/tools/azahar.sh" -t "$SECS" -s "$((SECS - 3))" -o "$OUT/$n" "$OUT/$n.3dsx" >/dev/null 2>&1
    cp "$OUT/$n/shot_$((SECS - 3)).png" "$OUT/$n.png" 2>/dev/null
    if grep -aqiE "fatal|critical|panic" "$OUT/$n/game.log"; then
        echo "$n: FAIL  $(grep -aiE 'fatal|critical|panic' "$OUT/$n/game.log" | head -1)"
        fail=1
    else
        echo "$n: ok    $(grep -a 'scenario' "$OUT/$n/game.log" | tail -1)"
    fi
done
exit $fail
