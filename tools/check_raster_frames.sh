#!/bin/sh
# Compare frame dumps of the production software renderer with the reference rasterizer (softgpu.c built with
# PORT_SOFTGPU_REFERENCE, which compiles out every fast path) on real game scenes. Run on the host:
#   tools/check_raster_frames.sh [SCENARIO.cfg FRAMES EVERY]...
# Defaults: the field 259 and Ark fixtures. Needs build/validation/kernel_data (see scenario-testing.md).
# Leaves build/pc built for production. Exit status is nonzero if any frame differs.
set -e
cd "$(dirname "$0")/.."
[ $# -gt 0 ] || set -- tests/scenarios/disc1_field259_anim_length.cfg 1200 100 tests/scenarios/disc3_ark.cfg 3000 250
out=build/raster_frames
rm -rf "$out"
dump() { # mode scenario frames every
    tools/docker.sh make -j16 pc RASTER_REFERENCE=$1 > /dev/null
    name=$(basename "$2" .cfg)
    rm -rf build/frames
    tools/docker.sh build/pc/ff9ds --scenario "$2" --data build/validation/kernel_data --frames "$3" --dump "$4" \
        > /dev/null 2>&1 || true
    mkdir -p "$out/$1"
    mv build/frames "$out/$1/$name"
}
status=0
args="$*"
for mode in 1 0; do
    set -- $args
    while [ $# -ge 3 ]; do dump $mode "$1" "$2" "$3"; shift 3; done
done
for dir in "$out"/1/*; do
    name=$(basename "$dir")
    total=0; same=0
    for f in "$dir"/*.ppm; do
        total=$((total + 1))
        if cmp -s "$f" "$out/0/$name/$(basename "$f")"; then same=$((same + 1)); else echo "DIFF $name $(basename "$f")"; status=1; fi
    done
    echo "$name: $same/$total frames identical to the reference rasterizer"
done
exit $status
