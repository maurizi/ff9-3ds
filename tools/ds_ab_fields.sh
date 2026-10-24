#!/bin/sh
# A/B of HEAD and the working tree on fields (fixtures in build/validation/fieldsweep/cfg), DS timers
# (PROFILE=1, or PROF=2 for busy time only): tools/ds_ab_fields.sh "d1_f0306 d2_f0931" port/psx/softgpu.c ...
# Builds HEAD's versions of the files and the working tree's; prints busy time and categories, and whether the
# screenshots differ. One emulator and one build at a time.
cd "$(dirname "$0")/.."
W=build/validation/sample_field; S=$W/ab_saved; mkdir -p $S
fields=$1; shift
build() { tools/docker.sh make -j16 ds HYBRID=1 HWSOUND=1 AUTORUN=1 PROFILE=${PROF:-1} CAPTURE_FRAME=1500 SCENARIO=sd:/ff9ds/scenarios/current.cfg > $W/ab_build.log 2>&1; }
for f in "$@"; do mkdir -p $S/$(dirname $f); cp $f $S/$f; git show HEAD:$f > $f; done
if build; then cp build/ff9ds.nds $W/ab_old.nds; cp build/ds/ff9ds.elf $W/ab_old.elf; for k in fxmods ovls; do [ -f build/$k.bin ] && cp build/$k.bin $W/ab_old_$k.bin; done; else echo "OLD BUILD FAILED"; fi
for f in "$@"; do cp $S/$f $f; done
if build; then cp build/ff9ds.nds $W/ab_new.nds; cp build/ds/ff9ds.elf $W/ab_new.elf; for k in fxmods ovls; do [ -f build/$k.bin ] && cp build/$k.bin $W/ab_new_$k.bin; done; else echo "NEW BUILD FAILED"; exit 1; fi
for n in $fields; do
    disc=$(echo $n | cut -c2)
    if [ $disc = 1 ]; then sd=build/sdroot; img=build/melon_dsisd.img; else sd=build/sdroot_disc$disc; img=build/melon_sd_disc$disc.img; fi
    cp build/validation/fieldsweep/cfg/$n.cfg $sd/ff9ds/scenarios/current.cfg
    for v in old new; do
        rm -f "$W/ab_${v}_$n.ml8" "$W/ab_${v}_$n.png"
        for k in fxmods ovls; do rm -f "$sd/ff9ds/$k.bin"; [ -f $W/ab_${v}_$k.bin ] && cp $W/ab_${v}_$k.bin $sd/ff9ds/$k.bin; done
        timeout 1700 python3 tools/ds_profile.py --rom $W/ab_$v.nds --elf $W/ab_$v.elf --frame 1500 --out $W/ab_${v}_$n.ml8 \
            --sd-folder $sd --sd-image $img --screenshot $W/ab_${v}_$n.png --timeout 1600 > $W/ab_${v}_$n.log 2>&1
        grep -v '^{' $W/ab_${v}_$n.log | awk -v n=$n -v v=$v 'NR==1{t=$5} /vblank_wait/{w=$2} /polygons/{p=$2} /rectangles/{r=$2} /gpu_commands/{g=$2} END{printf "%s %s: total %s busy %.2f gpu %s poly %s rect %s\n", n, v, t, t-w, g, p, r}'
    done
    python3 -c "
from PIL import Image, ImageChops
a=Image.open('$W/ab_old_$n.png').convert('RGB').crop((118,38,598,364)); b=Image.open('$W/ab_new_$n.png').convert('RGB').crop((118,38,598,364))
print('$n screen diff', ImageChops.difference(a,b).getbbox())"
done
