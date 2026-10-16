#!/usr/bin/env python3
"""Generate synthetic decoder goldens with a pinned, locally supplied reference.

The reference implementation stays in ignored build/, never in this repository.
See docs/audio-validation.md for the source URL, limitations, and commands.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

SOURCE_SHA256 = "7527e40ea418aee56c95615fd4b2ece34be28ceb365ac3490adf320dbfe57003"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    args = parser.parse_args()
    raw = args.reference.read_bytes()
    if hashlib.sha256(raw).hexdigest() != SOURCE_SHA256:
        parser.error("reference source does not match the pinned SHA256")
    source = raw.decode()
    begin = source.index("void SPU::Voice::DecodeBlock(const ADPCMBlock& block)")
    end = source.index("\nstatic constexpr", begin)
    decoder = source[begin:end]
    # Supply only the types used by the unmodified reference method. The three
    # interpolation-history slots are retained exactly as in the reference.
    harness = r'''
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include "adpcm_cases.h"
using u8=uint8_t; using s8=int8_t; using u32=uint32_t;
using s16=int16_t; using s32=int32_t;
static constexpr unsigned NUM_SAMPLES_FROM_LAST_ADPCM_BLOCK=3;
static constexpr unsigned NUM_SAMPLES_PER_ADPCM_BLOCK=28;
static s32 Clamp16(s32 n) { return std::clamp(n,-32768,32767); }
static uint16_t ZeroExtend16(u8 n) { return n; }
static s16 Truncate16(s32 n) { return static_cast<s16>(n); }
struct ADPCMFlags { u8 bits; };
struct ADPCMBlock {
    u8 header; ADPCMFlags flags; std::array<u8,14> data;
    u8 GetShift() const { u8 n=header&15; return n>12?9:n; }
    u8 GetFilter() const { return header>>4; }
};
struct SPU { struct Voice {
    std::array<s16,31> current_block_samples{};
    s16 adpcm_last_samples[2]{}; ADPCMFlags current_block_flags{};
    void DecodeBlock(const ADPCMBlock&);
}; };
'''
    harness += decoder
    harness += r'''
int main() {
    puts("/* Synthetic output hashes from the pinned DuckStation decoder.");
    puts(" * Regenerate using tools/spu_adpcm_reference.py; see audio-validation.md. */");
    puts("static const uint32_t adpcm_reference_hashes[256] = {");
    for (unsigned header=0; header<256; ++header) {
        uint32_t hash=2166136261u;
        for (unsigned scenario=0; scenario<5; ++scenario) {
            SPU::Voice voice;
            voice.adpcm_last_samples[0]=adpcm_initial_history[scenario][0];
            voice.adpcm_last_samples[1]=adpcm_initial_history[scenario][1];
            uint32_t random=adpcm_seed(header,scenario);
            for (unsigned b=0; b<8; ++b) {
                uint8_t bytes[16]; adpcm_case_block(bytes,header,&random);
                ADPCMBlock block;
                block.header=bytes[0]; block.flags.bits=bytes[1];
                std::copy(bytes+2,bytes+16,block.data.begin());
                voice.DecodeBlock(block);
                for (unsigned i=3; i<31; ++i)
                    hash=adpcm_hash(hash,static_cast<uint16_t>(voice.current_block_samples[i]));
                hash=adpcm_hash(hash,static_cast<uint16_t>(voice.adpcm_last_samples[0]));
                hash=adpcm_hash(hash,static_cast<uint16_t>(voice.adpcm_last_samples[1]));
                hash=adpcm_hash(hash,voice.current_block_flags.bits);
            }
        }
        printf("0x%08xu,%s",hash,(header%8==7)?"\n":" ");
    }
    puts("};");
}
'''
    root = Path(__file__).resolve().parents[1]
    # All reference-derived source and binaries remain ignored and local.
    directory = root / "build" / "validation"
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="spu-reference-", dir=directory) as tmp:
        cpp = Path(tmp) / "reference.cpp"
        exe = Path(tmp) / "reference"
        cpp.write_text(harness)
        subprocess.run(["g++", "-std=c++17", "-O2", "-I", str(root / "tests/spu"),
                        str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
