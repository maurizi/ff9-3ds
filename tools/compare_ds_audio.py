#!/usr/bin/env python3
"""Compare diagnostic ARM9 mixer PCM in a melonDS state with a host WAV.

Requires AUDIOCAPTURE=<stereo frames> and the exact matching ELF. This checks
source PCM before Maxmod/hardware output, not speaker DAC or wall-clock timing.
"""
import argparse
from array import array
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import wave


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('state', type=Path)
    parser.add_argument('elf', type=Path)
    parser.add_argument('reference', type=Path)
    parser.add_argument('--wav', type=Path, help='save DS source PCM as a new WAV')
    args = parser.parse_args()
    state = args.state.read_bytes()
    if state[:4] != b'MELN' or state[16:20] != b'NDSG' or len(state) < 36 + 16*1024*1024:
        parser.error('expected an uncompressed melonDS DSi state')
    ram = state[36:36+16*1024*1024]
    chunks = {}
    cursor = 16
    while cursor < len(state):
        if cursor + 16 > len(state):
            parser.error('truncated state section header')
        size = struct.unpack_from('<I',state,cursor+4)[0]
        if size < 16 or cursor+size > len(state):
            parser.error('invalid state section length')
        chunks[state[cursor:cursor+4]] = state[cursor:cursor+size]
        cursor += size
    symbols = {p[2]: int(p[0],16) for row in subprocess.check_output(['nm','-a',str(args.elf)],text=True).splitlines()
               if len(p := row.split()) == 3}
    elf = args.elf.read_bytes()
    if elf[:6] != b'\x7fELF\x01\x01':
        parser.error('expected little-endian ELF32')
    def read(name, count):
        start = symbols[name] - 0x02000000
        if not 0 <= start <= len(ram)-count:
            parser.error(f'{name} is outside main RAM')
        return ram[start:start+count]
    # Verify relevant loaded code against this ELF before interpreting globals.
    shoff = struct.unpack_from('<I',elf,32)[0]
    shsize, shnum = struct.unpack_from('<HH',elf,46)
    for name in ['plat_audio_samples','psx_spu_write','func_8005DFE0']:
        address = symbols[name] & ~1
        for i in range(shnum):
            section = struct.unpack_from('<10I',elf,shoff+i*shsize)
            if section[1] == 1 and section[3] <= address and address+64 <= section[3]+section[5]:
                pos = section[4]+address-section[3]
                if 0x02000000 <= address <= 0x03000000-64:
                    loaded = ram[address-0x02000000:address-0x02000000+64]
                elif 0x01000000 <= address <= 0x01008000-64 and b'CP15' in chunks:
                    offset = 28+address-0x01000000
                    loaded = chunks[b'CP15'][offset:offset+64]
                else:
                    parser.error(f'{name} code is outside supported RAM/ITCM')
                if loaded != elf[pos:pos+64]:
                    parser.error(f'loaded {name} code differs from supplied ELF')
                break
        else:
            parser.error(f'{name} code missing in supplied ELF')
    count = struct.unpack('<I',read('port_audio_captured_frames',4))[0]
    sizes = {p[3]:int(p[1],16) for row in subprocess.check_output(['nm','-S',str(args.elf)],text=True).splitlines()
             if len(p := row.split()) == 4}
    if count*4 > sizes.get('port_audio_capture',0):
        parser.error('capture count exceeds the buffer in the supplied ELF')
    stats = struct.unpack('<12I',read('port_audio_stats',48))
    if not count:
        parser.error('no PCM was captured')
    pcm = read('port_audio_capture',count*4)
    with wave.open(str(args.reference),'rb') as f:
        if (f.getnchannels(),f.getsampwidth(),f.getframerate()) != (2,2,44100):
            parser.error('reference must be stereo PCM16 at 44100 Hz')
        expected = f.readframes(count)
        if len(expected) != len(pcm):
            parser.error('host reference is shorter than the DS capture')
    actual_values, expected_values = array('h'), array('h')
    actual_values.frombytes(pcm); expected_values.frombytes(expected)
    if sys.byteorder != 'little':
        actual_values.byteswap();expected_values.byteswap()
    first, mismatches, max_error = None, 0, 0
    for i,(actual,reference) in enumerate(zip(actual_values,expected_values)):
        if actual != reference:
            mismatches += 1
            max_error = max(max_error,abs(actual-reference))
            if first is None:
                first = {'frame':i//2,'channel':i%2,'actual':actual,'reference':reference}
    if args.wav:
        with args.wav.open('xb') as output:
            with wave.open(output,'wb') as f:
                f.setparams((2,2,44100,0,'NONE','not compressed'));f.writeframes(pcm)
    report = {'scope':'ARM9 source PCM before Maxmod/hardware output',
              'game_frame':struct.unpack('<I',read('frames',4))[0],
              'stereo_frames':count,'produced':stats[2],'consumed':stats[3],
              'inserted_silence_at_snapshot':stats[4],'format_errors':stats[9],
              'ds_pcm_sha256':hashlib.sha256(pcm).hexdigest(),
              'host_pcm_sha256':hashlib.sha256(expected).hexdigest(),
              'differing_values':mismatches,'first_difference':first,'max_absolute_error':max_error}
    print(json.dumps(report,indent=2))
    return int(bool(mismatches))


if __name__ == '__main__':
    sys.exit(main())
