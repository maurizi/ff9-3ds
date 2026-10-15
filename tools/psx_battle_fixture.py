#!/usr/bin/env python3
"""Enter a boss on the original PSX from an initialized, paused field.

Uses the normal field cleanup and battle loader, preserving executable bytes.
Import character/event blobs exported after the host scenario bootstrap to
match its synthetic party/story setup. The original PSX disc must match.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import struct
from scenario_catalog import block, SECTOR

ROOT=Path(__file__).resolve().parents[1]


def apply_enemy_hp(emu, values):
    """Match the native fixture hook after entrance, before its entry script."""
    address=0x800af3a0  # AF2E0: immediately after storing combat state 4.
    bp=emu.bridge('addBp',address,'Exec',4,'break',1,'fixture_enemy_hp')
    try:
        emu.run_frames(12000)
        if emu.bridge('status')['pc']!=address:
            raise ValueError('battle did not reach initialized enemy HP hook')
    finally:
        emu.bridge('removeBp',bp)
    read=emu.bridge
    gs=read('r32',0x8006794c);gd=read('r32',gs+0x1c)
    battle=read('r32',gd+0x8e4);unit=read('r32',battle+0x6f8)
    pending={};seen=set()
    while unit:
        if unit in seen or not 0x80010000<=unit<0x801fff00:
            raise ValueError('invalid initialized battle unit list')
        seen.add(unit)
        flags=read('r32',unit+0x68);slot=((flags>>5)&7)-4
        if not flags&1 and slot in values:
            maximum=read('r32',unit+0x24)
            if not 0x80010000<=maximum<0x801ffffe:
                raise ValueError('invalid enemy maximum HP pointer')
            hp=struct.unpack('<H',bytes.fromhex(read('read',maximum,2)))[0]
            if values[slot]>hp:raise ValueError(f'enemy {slot} HP exceeds initialized maximum {hp}')
            pending[slot]=(unit,hp)
        unit=read('r32',unit)
    if values.keys()-pending.keys():raise ValueError('requested enemy is absent from initialized formation')
    for slot,(unit,hp) in sorted(pending.items()):
        emu.bridge('write',unit+0x28,struct.pack('<H',values[slot]).hex())
        print(f'scenario: enemy {slot} HP {values[slot]}/{hp}',flush=True)


def ranges(path, encounter):
    with path.open('rb') as image:
        header=image.read(SECTOR)
        if header[:4]!=b'FF9 ':raise ValueError('not FF9.IMG')
        kind,count,table,_=struct.unpack_from('<4I',header,16+5*16)
        if kind!=2:raise ValueError('unsupported encounter directory')
        image.seek(table*SECTOR);entries=image.read((count+1)*8)
        for i in range(count):
            key,sector=struct.unpack_from('<2I',entries,i*8)
            if key!=encounter:continue
            end=struct.unpack_from('<I',entries,(i+1)*8+4)[0]
            if end<=sector:raise ValueError('invalid encounter extent')
            image.seek(sector*SECTOR);data=image.read((end-sector)*SECTOR)
            deps=block(data,20);n=struct.unpack_from('<H',data,deps+2)[0]
            backgrounds=[]
            for j in range(n):
                key,start,length,_=struct.unpack_from('<4I',data,deps+4+j*16)
                if key&0xff0000==0xc0000:backgrounds.append((start,length))
            if len(backgrounds)!=1:raise ValueError('fixture needs exactly one background dependency')
            return [sector,backgrounds[0][0]],[end-sector,backgrounds[0][1]]
    raise ValueError('encounter absent on selected disc')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('scenario',type=Path)
    p.add_argument('--characters',type=Path,required=True)
    p.add_argument('--events',type=Path,required=True)
    p.add_argument('--base-state',help='named original PSX field state on this disc')
    p.add_argument('--decomp',type=Path,default=ROOT.parent/'ff9-decomp')
    p.add_argument('--dry-run',action='store_true')
    args=p.parse_args()
    config={}
    for line in args.scenario.read_text().splitlines():
        line=line.strip()
        if not line or line.startswith('#'):continue
        key,value=line.split('=',1);config[key.strip()]=value.strip()
    if config.get('kind')!='battle':p.error('requires battle scenario')
    enemy_hp={}
    try:
        for key,value in config.items():
            if key.startswith('enemy_hp.'):
                slot=int(key[9:],0);hp=int(value,0)
                if not 0<=slot<4 or not 1<=hp<=65535:raise ValueError('enemy_hp requires slot 0..3 and HP 1..65535')
                enemy_hp[slot]=hp
    except ValueError as error:p.error(str(error))
    disc=int(config.get('disc','1'),0);encounter=int(config['battle'],0)
    party=[int(v) for v in config.get('party','0,1,2,3').split(',')]
    if len(party)!=4 or party[0]<0 or any(v<-1 or v>8 for v in party):p.error('invalid party')
    if len({v for v in party if v>=0})!=sum(v>=0 for v in party):p.error('duplicate party')
    characters=args.characters.read_bytes();events=args.events.read_bytes()
    if len(characters)!=9*0x90 or len(events)!=0x800:p.error('invalid canonical blob length')
    sectors,lengths=ranges(ROOT/f'disks/us/disc{disc}/FF9.IMG',encounter)
    print(json.dumps({'disc':disc,'battle':encounter,'sectors':sectors,'lengths':lengths,'party':party,'enemy_hp':enemy_hp}))
    if args.dry_run:return
    spec=importlib.util.spec_from_file_location('ff9_emu',args.decomp/'tools/emu.py')
    emu=importlib.util.module_from_spec(spec);spec.loader.exec_module(emu)
    if emu.is_running():p.error('pause the reference emulator first')
    if args.base_state:
        if not re.fullmatch('[A-Za-z0-9_-]+',args.base_state):p.error('invalid state name')
        # Redux's cold-boot BIOS runtime must initialize before state restore;
        # otherwise RAM/VRAM restore but execution can remain in a BIOS loop.
        if emu.bridge('status')['pc']>=0xbfc00000:
            print('Booting original disc before restoring field baseline.',flush=True)
            emu.run_frames(1800)
        emu.http('/state/load?name='+args.base_state)
        emu.http('/execution-flow?function=pause','POST')
    gs=emu.bridge('r32',0x8006794c);gd=emu.bridge('r32',gs+0x1c)
    if not 0x80010000<=gs<0x80200000 or not 0x80010000<=gd<0x80200000:p.error('engine is not initialized')
    if emu.bridge('read',gs+8,1)!='01':p.error('reference must be in a loaded field')
    # Exit using the original field's flags and data, then stop at main's
    # return from the field overlay. Applying a synthetic story during field
    # cleanup can make its still-active event scripts process unrelated flags.
    def word(address,value):emu.bridge('write',address,struct.pack('<I',value&0xffffffff).hex())
    bp=emu.bridge('addBp',0x80012834,'Exec',4,'break',1,'fixture_after_field_cleanup')
    try:
        emu.bridge('write',gs+8,'02');emu.bridge('write',gs+10,'01')
        word(gs,emu.bridge('r32',gs)|2)
        emu.run_frames(600)
        if emu.bridge('status')['pc']!=0x80012834:p.error('field cleanup did not reach main dispatcher; boot the disc before loading a baseline')
    finally:
        emu.bridge('removeBp',bp)
    emu.bridge('write',gd+0x3c,characters.hex());emu.bridge('write',0x8007aef0,events.hex())
    for i,ch in enumerate(party):word(gd+0x54c+i*4,0 if ch<0 else gd+0x3c+ch*0x90)
    for i in range(2):
        word(gd+0x7ec+i*4,emu.bridge('r32',gd+0x7fc+i*4))
        word(gd+0x7f4+i*4,emu.bridge('r32',gd+0x804+i*4))
        word(gd+0x7fc+i*4,sectors[i]);word(gd+0x804+i*4,lengths[i])
    field=int(config.get('field','70'),0)
    emu.bridge('write',gd+12,struct.pack('<2H',field,encounter).hex())
    emu.bridge('write',gs+8,'02');emu.bridge('write',gs+10,'01')
    if enemy_hp:
        apply_enemy_hp(emu,enemy_hp)
        print('Enemy HP applied; original battle paused before its entry event script.')
    else:
        print('Field cleanup completed; resume/step the original battle loader.')


if __name__=='__main__':main()
