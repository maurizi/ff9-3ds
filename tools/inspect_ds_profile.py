#!/usr/bin/env python3
"""Decode an optional PROFILE=1 DS timing block from a matching melonDS state.

Phase timings overlap (notably GTE inside effect callbacks); do not add their
percentages. The instrumented run has measurement overhead and is diagnostic.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('state',type=Path)
p.add_argument('--elf',required=True,type=Path)
a=p.parse_args()
data=a.state.read_bytes()
if data[:4]!=b'MELN' or struct.unpack_from('<I',data,8)[0]!=len(data):
    p.error('expected a complete melonDS state')
if data[16:20]!=b'NDSG':p.error('expected melonDS 1.1 NDSG layout')
symbols=subprocess.check_output(['nm','-a',str(a.elf)],text=True)
matches=[int(line.split()[0],16) for line in symbols.splitlines()
         if len(line.split())==3 and line.split()[2]=='port_profile_stats']
if len(matches)!=1:p.error('matching ELF must be built with PROFILE=1')
address=matches[0]
if not 0x02000000<=address<0x03000000:p.error('profile block outside main RAM')
words=struct.unpack_from('<16I',data,36+address-0x02000000)
if words[0]!=128 or words[15]!=0x50524631:p.error('no complete 128-frame timing block yet')
frames,hz,frame,mode,total=words[:5]
if not hz or not total:p.error('invalid timing clock or elapsed interval')
phases={name:{'ms_per_frame':round(words[5+i]*1000/(frames*hz),4),
              'percent_of_interval':round(words[5+i]*100/total,3),
              'calls_per_frame':round(words[10+i]/frames,3)}
        for i,name in enumerate(('gpu_commands','gte','effect_callbacks','display_scale','vblank_wait'))}
report={'state':str(a.state),'elf':str(a.elf),'ending_frame':frame,'mode':mode,
                  'sample_frames':frames,'clock_hz':hz,'interval_ms_per_frame':round(total*1000/(frames*hz),4),
                  'phases_overlap':True,'measurement_overhead':True,'phases':phases}
draws=[int(line.split()[0],16) for line in symbols.splitlines()
       if len(line.split())==3 and line.split()[2]=='port_profile_draw_stats']
if len(draws)==1:
    draw=struct.unpack_from('<8I',data,36+draws[0]-0x02000000)
    if draw[0]==frame and draw[7]==0x44525731:
        report['raster_phases']={name:{'ms_per_frame':round(draw[1+i]*1000/(frames*hz),4),
            'percent_of_interval':round(draw[1+i]*100/total,3),
            'calls_per_frame':round(draw[4+i]/frames,3)} for i,name in enumerate(('polygons','rectangles','lines'))}
    else:report['raster_phases']='No matching complete raster timing window'
print(json.dumps(report,indent=2))
