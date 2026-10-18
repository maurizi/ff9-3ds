#!/bin/sh
# Capture-frame comparison of HEAD and the working tree on one battle (PROFILE=2, HYBRID=1 HWSOUND=1):
#   tools/ds_ab_battle.sh disc3_ark 3 1761 port/ds/gx_hybrid.c ...
# (FILE: the changed sources; HEAD's versions are swapped in for the "old" ROM). PROFILE=2 timings.
cd "$(dirname "$0")/.."
W=build/validation/sample_ark; S=$W/capcmp_saved; mkdir -p $S
scen=$1; disc=$2; frame=$3; shift 3
sd=build/sdroot_disc$disc; img=build/melon_sd_disc$disc.img
[ $disc = 1 ] && { sd=build/sdroot; img=build/melon_dsisd.img; }
build() { tools/docker.sh make -j16 ds HYBRID=1 HWSOUND=1 AUTORUN=1 PROFILE=2 CAPTURE_FRAME=$frame SCENARIO=sd:/ff9ds/scenarios/$scen.cfg > $W/capcmp_build.log 2>&1; }
for f in "$@"; do mkdir -p $S/$(dirname $f); cp $f $S/$f; git show HEAD:$f > $f; done
if build; then cp build/ff9ds.nds $W/c_old.nds; cp build/ds/ff9ds.elf $W/c_old.elf; else echo "OLD BUILD FAILED"; fi
for f in "$@"; do cp $S/$f $f; done
if build; then cp build/ff9ds.nds $W/c_new.nds; cp build/ds/ff9ds.elf $W/c_new.elf; else echo "NEW BUILD FAILED"; exit 1; fi
for v in old new; do
    rm -f "$W/c_$v.ml8" "$W/c_$v.png"
    timeout 1700 python3 tools/ds_profile.py --rom $W/c_$v.nds --elf $W/c_$v.elf --frame $frame --out $W/c_$v.ml8 \
        --sd-folder $sd --sd-image $img --screenshot $W/c_$v.png --timeout 1600 > $W/c_$v.log 2>&1
    echo "$scen $v: $(grep -v '^{' $W/c_$v.log | head -1)"
done
python3 -c "
from PIL import Image, ImageChops
a=Image.open('$W/c_old.png').convert('RGB').crop((118,38,598,364)); b=Image.open('$W/c_new.png').convert('RGB').crop((118,38,598,364))
print('$scen screen diff', ImageChops.difference(a,b).getbbox())"
