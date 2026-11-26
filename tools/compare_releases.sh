#!/bin/bash
# tools/compare_releases.sh NAME SCENARIO FRAME [PRESSES]: the PC build of each release (us es it de fr: make
# VERSION=... pc first; RELEASES="us fr" for some) runs the scenario to FRAME and dumps it, two to a row (us es / it de
# / fr) into
# build/touchcmp/NAME.png. How the touch layouts were checked against the PAL screens (2026-11-26).
name=$1; cfg=$2; f=$3; press=$4
for v in ${RELEASES:-us es it de fr}; do
  b=build/pc/ff9ds; [ $v != us ] && b=build/$v/pc/ff9ds
  mkdir -p build/touchcmp/$v
  timeout 300 tools/docker.sh $b --scenario $cfg --frames $f --dump-dir build/touchcmp/$v ${press:+--press "$press"} --dump $f > build/touchcmp/$v/$name.log 2>&1 &
done; wait
python3 - "$name" "$f" "${RELEASES:-us es it de fr}" <<'PY'
import sys
from PIL import Image
name, f = sys.argv[1], int(sys.argv[2])
ims = [Image.open(f"build/touchcmp/{v}/frame_{f:06d}.ppm") for v in sys.argv[3].split()]
w, h = ims[0].size
o = Image.new("RGB", (w * 2 + 4, h * ((len(ims) + 1) // 2) + 4 * ((len(ims) - 1) // 2)))
for i, im in enumerate(ims): o.paste(im, ((i % 2) * (w + 4), (i // 2) * (h + 4)))
o.resize((o.width * 2, o.height * 2), Image.NEAREST).save(f'build/touchcmp/{name}.png')
PY
