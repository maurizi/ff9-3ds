#!/usr/bin/env python3
"""Generate synthetic noise goldens from pinned local DuckStation source."""
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
    begin = source.index("void SPU::UpdateNoise()")
    end = source.index("u32 SPU::ReverbMemoryAddress(", begin)
    harness = r'''
#include <array>
#include <cstdint>
#include <cstdio>
using u8=uint8_t; using u32=uint32_t;
namespace SPU {
static struct { u32 noise_count,noise_level; struct {u8 noise_clock;} SPUCNT; } s_state;
void UpdateNoise();
}
'''
    harness += source[begin:end]
    harness += r'''
int main() {
    puts("/* Synthetic noise hashes from pinned DuckStation, not hardware captures.");
    puts(" * Regenerate with tools/spu_noise_reference.py; see audio-validation.md. */");
    puts("static const uint32_t noise_reference_hashes[64] = {");
    for(unsigned clock=0;clock<64;++clock) {
        SPU::s_state={0,1,{static_cast<u8>(clock)}};
        uint32_t hash=2166136261u;
        for(unsigned i=0;i<20000;++i) {
            SPU::UpdateNoise();
            hash=(hash^SPU::s_state.noise_count)*16777619u;
            hash=(hash^SPU::s_state.noise_level)*16777619u;
        }
        printf("0x%08xu,%s",hash,clock%8==7?"\n":" ");
    }
    puts("};");
}
'''
    root = Path(__file__).resolve().parents[1]
    directory = root / "build" / "validation"
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="spu-noise-reference-", dir=directory) as tmp:
        cpp = Path(tmp) / "reference.cpp"
        exe = Path(tmp) / "reference"
        cpp.write_text(harness)
        subprocess.run(["g++", "-std=c++17", "-O2", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
