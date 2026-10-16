#!/usr/bin/env python3
"""Generate synthetic reverb goldens from a pinned local Beetle PSX source.

Reference-derived source/executables remain private under ignored build/.
The port implements the algorithm independently; see audio-validation.md.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile
from spu_interpolation_reference import SOURCE_SHA256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    args = parser.parse_args()
    raw = args.reference.read_bytes()
    if hashlib.sha256(raw).hexdigest() != SOURCE_SHA256:
        parser.error("reference source does not match pinned SHA256")
    source = raw.decode()
    begin = source.index("typedef union\n{\n   uint16_t Regs[0x100];")
    registers = source[begin:source.index("} SPU_RegBlock;", begin) + len("} SPU_RegBlock;")]
    begin = source.index("static INLINE int16_t ReverbSat(")
    processing = source[begin:source.index("static INLINE void SPU_RunNoise(", begin)]
    harness = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "reverb_cases.h"
#define INLINE inline
#define MDFN_UNLIKELY(x) (x)
'''
    harness += registers
    harness += r'''
static SPU_RegBlock regs;
static uint16_t SPUControl;
static uint32_t ReverbWA,ReverbCur;
static int16_t RDSB[2][128],RUSB[2][64];
static int32_t RvbResPos;
static uint8_t ram[512*1024];
static int16_t SPU_ReadSPURAM(uint32_t word) {
    return (int16_t)((uint16_t)ram[word*2] | ((uint16_t)ram[word*2+1]<<8));
}
static void SPU_WriteSPURAM(uint32_t word,int16_t value) {
    ram[word*2]=(uint16_t)value;ram[word*2+1]=(uint16_t)value>>8;
}
'''
    harness += processing
    harness += r'''
int main(void) {
    puts("/* Synthetic reverb hashes from pinned Beetle PSX, not new hardware captures.");
    puts(" * Regenerate with tools/spu_reverb_reference.py; see audio-validation.md. */");
    puts("static const uint32_t reverb_reference_hashes[32] = {");
    for(unsigned scenario=0;scenario<REVERB_CASES;++scenario) {
        memset(&regs,0,sizeof(regs));memset(RDSB,0,sizeof(RDSB));memset(RUSB,0,sizeof(RUSB));RvbResPos=0;
        reverb_case_init(scenario,regs.s.reverb.ReverbRegs,ram);
        ReverbCur=ReverbWA=(uint32_t)reverb_case_base(scenario)*4;
        uint32_t hash=2166136261u;
        for(unsigned frame=0;frame<REVERB_FRAMES;++frame) {
            uint16_t base;
            if(reverb_case_base_event(scenario,frame,&base)) ReverbCur=ReverbWA=(uint32_t)base*4;
            int16_t pcm[2];int32_t in[2],out[2];reverb_case_input(scenario,frame,pcm);
            in[0]=pcm[0];in[1]=pcm[1];SPUControl=reverb_case_enable(scenario,frame)?0x80:0;
            SPU_RunReverb(in,out);
            hash=reverb_case_hash(hash,(uint16_t)out[0]);hash=reverb_case_hash(hash,(uint16_t)out[1]);
            hash=reverb_case_hash(hash,ReverbCur);
        }
        for(unsigned i=0;i<sizeof(ram);++i) hash=reverb_case_hash(hash,ram[i]);
        printf("0x%08xu,%s",hash,scenario%4==3?"\n":" ");
    }
    puts("};");
}
'''
    root = Path(__file__).resolve().parents[1]
    directory = root / "build" / "validation"
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="spu-reverb-reference-", dir=directory) as tmp:
        c = Path(tmp) / "reference.c"
        exe = Path(tmp) / "reference"
        c.write_text(harness)
        subprocess.run(["cc", "-std=gnu17", "-O2", "-I", str(root / "tests/spu"),
                        str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
