#!/usr/bin/env python3
"""Replay a host SPU journal through pinned Beetle PSX and compare mix stages.

The complete original DSP/register code is built privately under build/.
Observation assignments are inserted; the default replay leaves arithmetic and
key timing unchanged. --reverb-only instead supplies the recorded dry/send stages
to isolate reverb and final mixing from differences in voice generation.
This compares identical SPU inputs, not the PSX game's CPU/timer schedule.
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path
import subprocess
import tempfile
from spu_interpolation_reference import SOURCE_SHA256

HEADER_SHA256 = 'feee6c16a8b885f2d85aa30181de9a17c3189767d06594c70c72c084d38862d7'


def checked(path, sha):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != sha:
        raise ValueError(f'pinned source SHA256 mismatch: {path}')
    return data.decode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('header', type=Path)
    parser.add_argument('journal', type=Path)
    parser.add_argument('--reference-stages', type=Path, help='new LE32 stage file for alignment analysis')
    parser.add_argument('--reverb-only', action='store_true',
                        help='feed recorded dry/send stages to the reference mixer, isolating voice differences')
    parser.add_argument("--require-match", action="store_true", help="exit nonzero on any stage mismatch")
    args = parser.parse_args()
    source = checked(args.source, SOURCE_SHA256)
    header = checked(args.header, HEADER_SHA256)
    types = header[header.index('enum\n{'):header.index('/*\n * Audio output buffer.')]
    source = source[source.index('uint32_t IntermediateBufferPos;'):source.index('  int SPU_StateAction(')]
    # Observe local accumulators at fixed points without replacing computations.
    before = '      SPU_RunReverb(accum_fv, reverb);'
    after = '''      observed[0] = accum[0]; observed[1] = accum[1];
      observed[2] = accum_fv[0]; observed[3] = accum_fv[1];
''' + before + '''
      observed[4] = reverb[0]; observed[5] = reverb[1];'''
    assert source.count(before) == 1
    if args.reverb_only:
        after = '      accum[0]=recorded[0]; accum[1]=recorded[1]; accum_fv[0]=recorded[2]; accum_fv[1]=recorded[3];\n' + after
    source = source.replace(before, after)
    before = '            IntermediateBuffer[IntermediateBufferPos][lr] = (out * 3 + 2) >> 2;'
    assert source.count(before) == 1
    source = source.replace(before, before + '\n            observed[6 + lr] = out;')
    harness = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#define INLINE inline
#define MDFN_UNLIKELY(x) (x)
#define IRQ_SPU 0
static void IRQ_Assert(int irq, bool asserted) {(void)irq;(void)asserted;}
static void CDC_GetCDAudioSample(int32_t sample[2]) {sample[0]=sample[1]=0;}
uint8_t spu_samples=1;
bool psx_spu_silent_voice_opt=false;
static int32_t observed[8], recorded[8];
'''
    harness += types + source
    harness += r'''
static void fail(const char *message) {fprintf(stderr,"SPU replay: %s\n",message);exit(2);}
static uint32_t read32(FILE *f) {
    uint8_t b[4];if(fread(b,1,4,f)!=4) fail("truncated journal");
    return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);
}
static void write32(FILE *f,uint32_t value) {
    uint8_t b[4]={value,value>>8,value>>16,value>>24};
    if(fwrite(b,1,4,f)!=4) fail("stage file write failed");
}
int main(int argc,char **argv) {
    if(argc<2) fail("journal required");
    FILE *f=fopen(argv[1],"rb"),*out=argc>2?fopen(argv[2],"wbx"):NULL;
    if(!f || (argc>2&&!out)) fail("cannot open journal/new stage file");
    char magic[8];if(fread(magic,1,8,f)!=8||memcmp(magic,"FF9SPU1\0",8))fail("bad journal signature");
    uint64_t samples=0,writes=0,transfers=0,resets=0,mismatches[8]={0};
    uint64_t first[8];int32_t first_actual[8]={0},first_reference[8]={0};
    int64_t max_error[8]={0};for(unsigned i=0;i<8;++i)first[i]=UINT64_MAX;
    bool initialized=false;
    for(;;) {
        uint32_t type=read32(f),size=read32(f);
        if(type==UINT32_MAX) {
            if(size||fgetc(f)!=EOF) fail("bad completed-journal footer");
            break;
        }
        if(type==0&&size==0) {SPU_Init();SPU_Power();initialized=true;++resets;continue;}
        if(!initialized) fail("journal must start with reset");
        if(type==1&&size==8) {
            uint32_t offset=read32(f),value=read32(f);
            if(offset>=0x400||(offset&1)||value>65535)fail("invalid register event");
            SPU_Write(0,offset,value);++writes;
        } else if(type==2&&size>=8) {
            uint32_t address=read32(f),count=read32(f);
            if(count!=size-8||address>0x80000||count>0x80000-address)fail("invalid RAM event");
            if(fread((uint8_t*)SPURAM+address,1,count,f)!=count)fail("truncated RAM event");
            ++transfers;
        } else if(type==3&&size==32) {
            int32_t actual[8];for(unsigned i=0;i<8;++i)actual[i]=(int32_t)read32(f);
            memcpy(recorded,actual,sizeof recorded);
            IntermediateBufferPos=0;SPU_UpdateFromCDC(768);
            if(IntermediateBufferPos!=1) fail("reference did not emit exactly one sample");
            for(unsigned i=0;i<8;++i) {
                int64_t error=(int64_t)actual[i]-observed[i];if(error<0)error=-error;
                if(error) {
                    ++mismatches[i];if(error>max_error[i])max_error[i]=error;
                    if(first[i]==UINT64_MAX){first[i]=samples;first_actual[i]=actual[i];first_reference[i]=observed[i];}
                }
                if(out)write32(out,(uint32_t)observed[i]);
            }
            ++samples;
        } else fail("unknown event or invalid size");
    }
    fclose(f);if(out&&fclose(out))fail("stage file close failed");
    if(!samples)fail("journal contains no samples");
    printf("{\"samples\":%"PRIu64",\"writes\":%"PRIu64",\"ram_events\":%"PRIu64",\"resets\":%"PRIu64",\"stages\":[",samples,writes,transfers,resets);
    for(unsigned i=0;i<8;++i) {
        if(i)putchar(',');
        printf("{\"mismatched_values\":%"PRIu64",\"max_absolute_error\":%"PRId64",\"first_sample\":",mismatches[i],max_error[i]);
        if(first[i]==UINT64_MAX)printf("null");else printf("%"PRIu64,first[i]);
        printf(",\"first_actual\":%d,\"first_reference\":%d}",first_actual[i],first_reference[i]);
    }
    puts("]}");return 0;
}
'''
    root = Path(__file__).resolve().parents[1]
    directory = root / 'build/validation'
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='spu-full-reference-', dir=directory) as tmp:
        c, exe = Path(tmp)/'reference.c', Path(tmp)/'reference'
        c.write_text(harness)
        subprocess.run(['cc', '-std=gnu17', '-O2', '-fwrapv', str(c), '-o', str(exe)], check=True)
        command = [str(exe), str(args.journal)]
        if args.reference_stages:
            command += [str(args.reference_stages)]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            print(result.stderr, file=sys.stderr, end='')
            sys.exit(result.returncode)
        report = json.loads(result.stdout)
        report['mode'] = 'recorded-dry-send' if args.reverb_only else 'full-reference'
        report['stage_order'] = ['dry_left', 'dry_right', 'send_left', 'send_right',
                                 'wet_left', 'wet_right', 'pcm_left', 'pcm_right']
        if args.reverb_only:
            # Recorded dry/send are inputs, not independently verified outputs.
            report['stage_order'] = report['stage_order'][4:]
            report['stages'] = report['stages'][4:]
        print(json.dumps(report, indent=2))
        if args.require_match and any(stage['mismatched_values'] for stage in report['stages']):
            sys.exit(1)


if __name__ == '__main__':
    main()
