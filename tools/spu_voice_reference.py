#!/usr/bin/env python3
"""Run pinned reference voice methods on synthetic events; emit fixed hashes.

All reference-derived code remains private under ignored build/. This oracle
exercises DuckStation voice methods with Beetle's matching scalar interpolation.
It does not emulate the CPU bus, key-event latch timing, reverb, or audio output.
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path
import subprocess
import tempfile
from spu_adpcm_reference import SOURCE_SHA256 as DUCK_SHA256
from spu_interpolation_reference import SOURCE_SHA256 as BEETLE_SHA256

HELPERS = {
    "types": "20b9d3a6c71e094a6e2c30fd4bb101c32ec14612bf259052d612a68babbc52d4",
    "bitfield": "5b8d6ff8e5c34af2253bbbfc5e9b45e1d59c696b3bffa3bfba45a957ed4f2f04",
    "bitutils": "4e1de8318888cb8485e15634971589b87ef26bc4a6ac8fe1e686424944afcd22",
}


def checked(path, sha):
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != sha:
        raise ValueError(f"pinned SHA256 mismatch: {path}")
    return raw.decode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("duck", type=Path)
    parser.add_argument("beetle", type=Path)
    parser.add_argument("--helpers", required=True, type=Path,
                        help="directory containing pinned duck_types/bitfield/bitutils.h")
    parser.add_argument("--journal", type=Path, help="compare real host journal dry/send stages instead of synthetic fixtures")
    parser.add_argument("--require-match", action="store_true", help="journal replay exits nonzero on any dry/send mismatch")
    args = parser.parse_args()
    if args.require_match and not args.journal:
        parser.error("--require-match requires --journal")
    duck = checked(args.duck, DUCK_SHA256)
    beetle = checked(args.beetle, BEETLE_SHA256)
    helpers = {name: checked(args.helpers / f"duck_{name}.h", sha) for name, sha in HELPERS.items()}

    def section(start, end):
        begin = duck.index(start)
        return duck[begin:duck.index(end, begin)]

    start = beetle.index("static const int16_t FIR_Table[256][4] =")
    table = beetle[start:beetle.index("};", start) + 2]
    start = beetle.index("voice_pvs = ((voice->DecodeBuffer[")
    interpolate = beetle[start:beetle.index(";", start) + 1]
    harness = r'''
#include "types.h"
#include "bitfield.h"
#include "bitutils.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <tuple>
#include "voice_cases.h"
#define DebugAssert assert
#define Assert assert
#define DEBUG_LOG(...) ((void)0)
#define DEV_LOG(...) ((void)0)
#define TRACE_LOG(...) ((void)0)
#define ERROR_LOG(...) ((void)0)
static s32 Clamp16(s32 n) { return std::clamp(n,-32768,32767); }
static s32 ApplyVolume(s32 n,s16 v) { return (n*v)>>15; }
namespace SPU {
enum : u32 {NUM_VOICES=24, NUM_VOICE_REGISTERS=8, NUM_SAMPLES_FROM_LAST_ADPCM_BLOCK=3, NUM_SAMPLES_PER_ADPCM_BLOCK=28};
'''
    harness += section("union ADSRRegister", "struct ReverbRegisters")
    harness += r'''
static struct { std::array<Voice,24> voices; u32 endx_register, key_on_register;
    u32 noise_count, noise_level; struct {u8 noise_clock;} SPUCNT; } s_state;
static u8 ram[512*1024]; static s16 noise;
static void GeneratePendingSamples() {}
static s16 GetVoiceNoiseLevel() { return noise; }
static void ReadADPCMBlock(u16 address, ADPCMBlock* block) {
    std::memcpy(block,ram+address*8,sizeof(*block));
}
static void WriteVoiceRegister(u32,u16);
static void UpdateNoise();
static ADSRPhase GetNextADSRPhase(ADSRPhase);
static std::tuple<s32,s32> SampleVoice(Voice&,u32,bool,bool,bool,s32);
}
'''
    harness += section("void SPU::Voice::KeyOn()", "static constexpr std::array<std::array<s16, 4>")
    harness += section("void SPU::WriteVoiceRegister(", "bool SPU::IsVoiceReverbEnabled(")
    harness += table
    harness += r'''
s32 SPU::Voice::Interpolate() const {
    struct { s16 DecodeBuffer[32]; } storage{};
    std::copy(current_block_samples.begin(),current_block_samples.end(),storage.DecodeBuffer);
    const auto* voice=&storage;
    const int si=counter.sample_index.GetValue(), pi=counter.interpolation_index.GetValue();
    s32 voice_pvs;
'''
    harness += interpolate + '\nreturn voice_pvs;\n}\n'
    harness += section("ALWAYS_INLINE_RELEASE std::tuple<s32, s32> SPU::SampleVoice(", "void SPU::UpdateNoise()")
    harness += r'''
void voice_case_write(unsigned voice,unsigned reg,uint16_t value) {
    SPU::WriteVoiceRegister(voice*16+reg*2,value);
}
void voice_case_key(unsigned voice,int on) {
    if(on) { SPU::s_state.voices[voice].KeyOn(); SPU::s_state.endx_register&=~(1u<<voice); }
    else SPU::s_state.voices[voice].KeyOff();
}
int main() {
    puts("/* Synthetic 24-voice hashes: pinned DuckStation methods + Beetle interpolation.");
    puts(" * See tools/spu_voice_reference.py and audio-validation.md for scope. */");
    puts("static const uint32_t voice_reference_hashes[8] = {");
    for(unsigned scenario=0;scenario<VOICE_CASES;++scenario) {
        SPU::s_state={}; std::memset(SPU::ram,0,sizeof(SPU::ram)); voice_case_ram(SPU::ram,scenario);
        uint32_t hash=2166136261u;
        for(unsigned frame=0;frame<VOICE_CASE_FRAMES;++frame) {
            voice_case_events(scenario,frame); SPU::noise=voice_case_noise(frame);
            int32_t sums[2]{},sends[2]{},previous=0;
            for(unsigned i=0;i<24;++i) {
                auto [l,r]=SPU::SampleVoice(SPU::s_state.voices[i],i,(voice_case_noise_mask(scenario)>>i)&1,
                    i && ((voice_case_modulation(scenario)>>i)&1),scenario>=4,previous);
                previous=SPU::s_state.voices[i].last_volume;
                sums[0]+=l;sums[1]+=r;
                if((voice_case_reverb(scenario)>>i)&1) {sends[0]+=l;sends[1]+=r;}
            }
            for(int channel=0;channel<2;++channel) hash=voice_case_hash(hash,sums[channel]);
            for(int channel=0;channel<2;++channel) hash=voice_case_hash(hash,sends[channel]);
            hash=voice_case_hash(hash,(u16)Clamp16(SPU::s_state.voices[1].last_volume));
            hash=voice_case_hash(hash,(u16)Clamp16(SPU::s_state.voices[3].last_volume));
            hash=voice_case_hash(hash,SPU::s_state.endx_register);
            for(const auto& v:SPU::s_state.voices) {
                hash=voice_case_hash(hash,(u16)v.regs.adsr_volume);
                hash=voice_case_hash(hash,v.regs.adpcm_repeat_address);
                hash=voice_case_hash(hash,v.current_address);
                hash=voice_case_hash(hash,v.counter.bits);
                hash=voice_case_hash(hash,(unsigned)v.adsr_phase);
                hash=voice_case_hash(hash,(u16)v.left_volume.current_level);
                hash=voice_case_hash(hash,(u16)v.right_volume.current_level);
                hash=voice_case_hash(hash,(u32)v.last_volume);
            }
        }
        printf("0x%08xu,\n",hash);
    }
    puts("};");
}
'''
    if args.journal:
        harness = harness[:harness.index("void voice_case_write(")]
        start = duck.index("void SPU::UpdateNoise()")
        end = duck.index("\n}", start) + 2
        harness += duck[start:end]
        harness += '\n#include "spu_voice_journal.h"\n'
    root = Path(__file__).resolve().parents[1]
    directory = root / "build" / "validation"
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="spu-voice-reference-", dir=directory) as tmp:
        cpp = Path(tmp) / "reference.cpp"
        exe = Path(tmp) / "reference"
        cpp.write_text(harness)
        for name, body in helpers.items():
            (Path(tmp) / f"{name}.h").write_text(body)
        subprocess.run(["g++", "-std=c++23", "-O2", "-I", str(root / "tests/spu"), "-I", str(root / "tools"),
                        str(cpp), "-o", str(exe)], check=True)
        if args.journal:
            result = subprocess.run([str(exe), str(args.journal)], capture_output=True, text=True)
            if result.returncode:
                print(result.stderr, file=sys.stderr, end='')
                sys.exit(result.returncode)
            report = json.loads(result.stdout)
            report['stage_order'] = ['dry_left', 'dry_right', 'send_left', 'send_right']
            report['reference'] = 'DuckStation voice methods with Beetle scalar interpolation; replay driver supplies scheduling'
            print(json.dumps(report, indent=2))
            if args.require_match and any(stage['mismatched_values'] for stage in report['stages']):
                sys.exit(1)
        else:
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
