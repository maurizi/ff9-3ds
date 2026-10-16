#!/usr/bin/env python3
"""Generate synthetic envelope goldens using the pinned local DuckStation source.

No reference source is stored outside ignored build/. See audio-validation.md.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

from spu_adpcm_reference import SOURCE_SHA256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    args = parser.parse_args()
    raw = args.reference.read_bytes()
    if hashlib.sha256(raw).hexdigest() != SOURCE_SHA256:
        parser.error("reference source does not match pinned SHA256")
    source = raw.decode()
    begin = source.index("void SPU::VolumeEnvelope::Reset(")
    end = source.index("void SPU::VolumeSweep::Reset(", begin)
    harness = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include "envelope_cases.h"
#define ALWAYS_INLINE_RELEASE
using u8=uint8_t; using u16=uint16_t; using u32=uint32_t;
using s16=int16_t; using s32=int32_t;
static s16 Truncate16(s32 n) { return static_cast<s16>(n); }
struct SPU { struct VolumeEnvelope {
    static constexpr s32 MIN_VOLUME=-32768, MAX_VOLUME=32767;
    u32 counter; u16 counter_increment; s16 step;
    u8 rate; bool decreasing, exponential, phase_invert;
    void Reset(u8,u8,bool,bool,bool);
    bool Tick(s16&);
}; };
'''
    harness += source[begin:end]
    harness += r'''
int main() {
    puts("/* Synthetic envelope hashes from pinned DuckStation, not hardware captures.");
    puts(" * Regenerate with tools/spu_envelope_reference.py; see audio-validation.md. */");
    puts("static const uint32_t envelope_reference_hashes[128] = {");
    for (unsigned rate=0; rate<128; ++rate) {
        uint32_t hash=2166136261u;
        for (unsigned mask : {0x7fu,0x7cu}) {
            for (unsigned mode=0; mode<8; ++mode) {
                for (int16_t initial : envelope_initial_levels) {
                    SPU::VolumeEnvelope env{};
                    env.Reset(rate,mask,mode&1,mode&2,mode&4);
                    int16_t level=initial;
                    for (unsigned i=0; i<envelope_case_ticks(rate); ++i) {
                        bool active=env.Tick(level);
                        hash=envelope_hash(hash,static_cast<uint16_t>(level));
                        hash=envelope_hash(hash,env.counter);
                        hash=envelope_hash(hash,active);
                    }
                }
            }
        }
        printf("0x%08xu,%s",hash,(rate%8==7)?"\n":" ");
    }
    puts("};");
}
'''
    root = Path(__file__).resolve().parents[1]
    directory = root / "build" / "validation"
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="spu-envelope-reference-", dir=directory) as tmp:
        cpp = Path(tmp) / "reference.cpp"
        exe = Path(tmp) / "reference"
        cpp.write_text(harness)
        subprocess.run(["g++", "-std=c++17", "-O2", "-I", str(root / "tests/spu"),
                        str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
