/* Declarative test launch through normal loaders. No CPU state, stack,
 * executable bytes or runtime pointers are imported. */
#include "psyq_port.h"
#define printf port_psx_printf_declaration
#include "ff9.h"
#undef printf
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
void psx_cd_select_disc(unsigned);
void o14_opening_initNewGame(void);
void o14_opening_findSceneSectors(void);
void port_scenario_set_enemy_hp(const int *values);
s32 chr_resetToLevel(s32,s32,u8);
static struct {
    int blackjack; /* blackjack=1 with field_exit=4: the ending's movie skipped and The End's code taken (blackjack_ui.c) */
    int chocograph; /* GameData.chocograph (1-24, 0 none): the one the chocobo digs for (not an event byte, decomp R37) */
    int enabled,consumed,kind,disc,field,battle,world,world_explicit,level,story,field_explicit,return_field_ready,enemy_hp_applied,summons_seen,gil,tetra;
    int menu,menu_arg; /* kind=field: menu=TYPE[,ARG] opens that menu once the field is ready (4,2: the save menu) */
    unsigned seed;
    int party[4],hp[4],enemy_hp[4],trance[4],preset[4],equip[4][5];
    u8 events[0x800],events_set[0x800/8]; /* (bytes and a bit each: the heap is tight) */
    s8 items[256]; /* -1: unset (counts 0-99) */
    u8 cards[100]; /* card.ID=N: N copies of card ID added (the game's card_add, its stats rolled) */
    u8 keyitems[0x100/8]; /* keyitem.ID=1: key item ID held (keyItem_add) */
    char characters[256],event_blob[256];
} scenario;
static int number(const char *s,int low,int high) {
    char *end;errno=0;long n=strtol(s,&end,0);
    if(errno==ERANGE || end==s || *end || n<low || n>high) port_fatal("scenario: invalid value %s",s);
    return (int)n;
}
void port_scenario_load(const char *path) {
    FILE *f=fopen(path,"r");
    if(!f) port_fatal("scenario: cannot open %s",path);
    __builtin_memset(&scenario,0,sizeof scenario);
    scenario.disc=1;scenario.field=70;scenario.battle=-1;scenario.world=0x2331;
    scenario.level=-1;scenario.story=-1;scenario.gil=-1;scenario.seed=1;
    for(int i=0;i<4;++i) {scenario.party[i]=i;scenario.hp[i]=scenario.enemy_hp[i]=scenario.trance[i]=scenario.preset[i]=-1;for(int j=0;j<5;++j) scenario.equip[i][j]=-1;}
    for(int i=0;i<256;++i) scenario.items[i]=-1;
    char line[512];
    while(fgets(line,sizeof line,f)) {
        if(!__builtin_strchr(line,'\n') && !feof(f)) port_fatal("scenario: line too long");
        char *s=line;while(*s==' ' || *s=='\t') ++s;
        if(!*s || *s=='#' || *s=='\n') continue;
        char *eq=__builtin_strchr(s,'=');if(!eq) port_fatal("scenario: expected key=value");*eq++=0;
        char *key_end=eq-1;while(key_end>s && (key_end[-1]==' ' || key_end[-1]=='\t')) *--key_end=0;
        while(*eq==' ' || *eq=='\t') ++eq;
        char *end=eq+__builtin_strlen(eq);while(end>eq && (end[-1]=='\n' || end[-1]=='\r' || end[-1]==' ' || end[-1]=='\t')) *--end=0;
        if(!__builtin_strcmp(s,"kind")) {
            if(!__builtin_strcmp(eq,"field")) scenario.kind=1;
            else if(!__builtin_strcmp(eq,"battle")) scenario.kind=2;
            else if(!__builtin_strcmp(eq,"world")) scenario.kind=3;
            else port_fatal("scenario: kind must be field, battle or world");
        } else if(!__builtin_strcmp(s,"disc")) scenario.disc=number(eq,1,4);
        else if(!__builtin_strcmp(s,"field")) {scenario.field=number(eq,0,32767);scenario.field_explicit=1;}
        else if(!__builtin_strcmp(s,"battle")) scenario.battle=number(eq,0,65535);
        else if(!__builtin_strcmp(s,"world")) {scenario.world=number(eq,0,65535);scenario.world_explicit=1;}
        else if(!__builtin_strcmp(s,"chocograph")) scenario.chocograph=number(eq,0,24);
        else if(!__builtin_strcmp(s,"blackjack")) scenario.blackjack=number(eq,0,1);
        else if(!__builtin_strcmp(s,"level")) scenario.level=number(eq,1,99);
        else if(!__builtin_strcmp(s,"story")) scenario.story=number(eq,0,65535);
        else if(!__builtin_strcmp(s,"gil")) scenario.gil=number(eq,0,9999999);
        else if(!__builtin_strcmp(s,"menu")) {
            char *comma=__builtin_strchr(eq,',');
            if(comma) *comma=0,scenario.menu_arg=number(comma+1,0,0xFFFF);
            scenario.menu=1+number(eq,0,8);
        }
        else if(!__builtin_strcmp(s,"tetra")) scenario.tetra=number(eq,0,1)?9:0; /* kind=field: then a card game */
        else if(!__builtin_strcmp(s,"field_exit")) { /* kind=field: then this exit: 4 the ending, 7 game over, 9 cards */
            scenario.tetra=number(eq,4,9);
            if(scenario.tetra!=4 && scenario.tetra!=7 && scenario.tetra!=9) port_fatal("scenario: field_exit must be 4, 7 or 9");
        }
        else if(!__builtin_strcmp(s,"summons_seen")) scenario.summons_seen=number(eq,0,65535);
        else if(!__builtin_strcmp(s,"seed")) scenario.seed=(unsigned)number(eq,0,0x7fffffff);
        else if(!__builtin_strcmp(s,"party")) {
            int used=0;
            if(sscanf(eq,"%d,%d,%d,%d%n",scenario.party,scenario.party+1,scenario.party+2,scenario.party+3,&used)!=4 || eq[used]) port_fatal("scenario: party needs four indices");
            for(int i=0;i<4;++i) {
                if(scenario.party[i]<-1 || scenario.party[i]>8) port_fatal("scenario: party outside -1..8");
                for(int j=0;j<i;++j) if(scenario.party[i]>=0 && scenario.party[i]==scenario.party[j]) port_fatal("scenario: duplicate party member");
            }
            if(scenario.party[0]<0) port_fatal("scenario: first party slot must be occupied");
        } else if(!__builtin_strcmp(s,"characters") || !__builtin_strcmp(s,"events")) {
            char *dst=!__builtin_strcmp(s,"characters")?scenario.characters:scenario.event_blob;
            if(__builtin_strlen(eq)>=256) port_fatal("scenario: blob path too long");
            __builtin_strcpy(dst,eq);
        } else if(!__builtin_strncmp(s,"enemy_hp.",9)) scenario.enemy_hp[number(s+9,0,3)]=number(eq,1,65535);
        else if(!__builtin_strncmp(s,"trance.",7)) scenario.trance[number(s+7,0,3)]=number(eq,0,255);
        else if(!__builtin_strncmp(s,"preset.",7)) scenario.preset[number(s+7,0,3)]=number(eq,0,19);
        else if(!__builtin_strncmp(s,"hp.",3)) scenario.hp[number(s+3,0,3)]=number(eq,0,9999);
        else if(!__builtin_strncmp(s,"event.",6)) {int e=number(s+6,0,0x7ff);scenario.events[e]=(u8)number(eq,0,255);scenario.events_set[e>>3]|=(u8)(1u<<(e&7));}
        else if(!__builtin_strncmp(s,"item.",5)) scenario.items[number(s+5,0,255)]=(s8)number(eq,0,99);
        else if(!__builtin_strncmp(s,"card.",5)) scenario.cards[number(s+5,0,99)]=(u8)number(eq,0,99);
        else if(!__builtin_strncmp(s,"keyitem.",8)) {int k=number(s+8,0,255);if(number(eq,0,1)) scenario.keyitems[k>>3]|=(u8)(1u<<(k&7));}
        else if(!__builtin_strncmp(s,"equip.",6)) {
            int slot=number(s+6,0,3),used=0,*v=scenario.equip[slot];
            if(sscanf(eq,"%d,%d,%d,%d,%d%n",v,v+1,v+2,v+3,v+4,&used)!=5 || eq[used]) port_fatal("scenario: equipment needs five IDs");
            for(int i=0;i<5;++i) if(v[i]<0 || v[i]>255) port_fatal("scenario: equipment outside 0..255");
        } else port_fatal("scenario: unknown key %s",s);
    }
    if(ferror(f)) port_fatal("scenario: error reading %s",path);
    fclose(f);
    if(!scenario.kind || (scenario.kind==2 && scenario.battle<0)) port_fatal("scenario: kind and battle ID required");
    if(scenario.kind!=2) for(int i=0;i<4;i++) if(scenario.enemy_hp[i]>=0)
        port_fatal("scenario: enemy_hp requires kind=battle");
    scenario.enabled=1;psx_cd_select_disc((unsigned)scenario.disc);
    port_log("scenario configured: disc %d, %s %d\n",scenario.disc,scenario.kind==1?"field":scenario.kind==2?"battle":"world",
             scenario.kind==1?scenario.field:scenario.kind==2?scenario.battle:scenario.world);
}
static void load_blob(const char *path,void *data,size_t size) {
    if(!*path) return;
    FILE *f=fopen(path,"rb");
    if(!f || fread(data,1,size,f)!=size || fgetc(f)!=EOF) port_fatal("scenario: blob %s must be %u bytes",path,(unsigned)size);
    fclose(f);
}
/* Field/battle initialization normally reseeds from a hardware counter. */
s32 port_scenario_seed(s32 ordinary_seed) {
    return scenario.enabled ? (s32)scenario.seed : ordinary_seed;
}
void port_scenario_battle_ready(void) {
    if(!scenario.enabled || !scenario.consumed || scenario.kind!=2 || scenario.enemy_hp_applied) return;
    scenario.enemy_hp_applied=1;
    for(int i=0;i<4;i++) if(scenario.enemy_hp[i]>=0) {
        port_scenario_set_enemy_hp(scenario.enemy_hp);
        return;
    }
}
/* A battle the world map starts (kind=battle with world=): it returns there, as the world map's own exit sets up
 * (ovl_0c: its file's sectors as the return sectors, unkA 3; ovl_0b btl_initReturnInfo keeps the world id). */
static void prepare_return_world(GameData *d) {
    u8 mode=g_gameState->mode;
    d->worldId=(u16)scenario.world;
    g_gameState->mode=3;o14_opening_findSceneSectors();g_gameState->mode=mode;
    if(d->sceneSectors[1]<=0) port_fatal("scenario: world map %d absent on disc %d",scenario.world,scenario.disc);
    for(int i=0;i<2;i++) {
        d->returnSector[i]=d->sceneSector[i];d->returnSectors[i]=d->sceneSectors[i];
    }
    g_gameState->prevMode=3;
    scenario.return_field_ready=1;
}
static void prepare_return_field(GameData *d) {
    typedef struct {u16 field,area;u32 sector;} FieldEntry;
    _Static_assert(sizeof(FieldEntry)==8,"field directory entry layout");
    DirInfo *dirs=g_gameState->dirs;
    const FieldEntry *entries=(const FieldEntry *)(g_loadAreaEnd+0x14c700+(dirs[4].tableSector<<11));
    int found=0;
    for(int i=0;i<dirs[4].count;i++) if(entries[i].field==scenario.field) {found=1;break;}
    if(!found) {
        if(scenario.field_explicit) port_fatal("scenario: return field %d absent on disc %d",scenario.field,scenario.disc);
        /* A battle-only fixture still needs a field to return to: without one the game preloads stale sectors as the
         * return field while the rewards run, over ovl_65's own data. The disc's first field stands in. */
        if(dirs[4].count<=0) port_fatal("scenario: no field on disc %d to return to",scenario.disc);
        scenario.field=entries[0].field;d->fieldId=(u16)scenario.field;
        port_log("scenario: battle returns to field %d (the disc's first)\n",scenario.field);
    }
    u8 mode=g_gameState->mode;
    g_gameState->mode=1;o14_opening_findSceneSectors();g_gameState->mode=mode;
    for(int i=0;i<2;i++) {
        d->returnSector[i]=d->sceneSector[i];d->returnSectors[i]=d->sceneSectors[i];
    }
    scenario.return_field_ready=1;
}
/* Scenario diagnostics only. Validate the current archive's directory before
 * a synthetic story state can send the loader to an absent resource. The IDs are
 * copied at bootstrap, while the in-RAM directory is intact: its area is later
 * shared with movie buffers (MBG_init) and battle data, and the game reloads it
 * before its own lookups. */
static u32 field_ids[4096/32]; /* (a bit per field ID: the heap is short) */
static int n_field_ids=-1;
static void cache_field_ids(void) {
    typedef struct {u16 field,area;u32 sector;} FieldEntry;
    _Static_assert(sizeof(FieldEntry)==8,"field directory entry layout");
    DirInfo *dirs=g_gameState->dirs;
    const FieldEntry *entries=(const FieldEntry *)(g_loadAreaEnd+0x14c700+(dirs[4].tableSector<<11));
    __builtin_memset(field_ids,0,sizeof field_ids);
    for(n_field_ids=0;n_field_ids<dirs[4].count;n_field_ids++) {
        if(entries[n_field_ids].field>=4096) port_fatal("scenario: field %d past the bitmap",(int)entries[n_field_ids].field);
        field_ids[entries[n_field_ids].field>>5]|=1u<<(entries[n_field_ids].field&31);
    }
}
static int disc_changed;
void port_scenario_disc_changed(void) { disc_changed=1; } /* (port/game/disc_change.c) */
void port_scenario_field_entering(int field) {
    if(!scenario.enabled || !scenario.consumed) return;
    if(disc_changed) return; /* (a disc changed during the run: its fields aren't checked against the fixture's disc) */
    if(n_field_ids<0) cache_field_ids();
    port_log("scenario field entering: %d\n",field);
    if(field>=0 && field<4096 && (field_ids[field>>5]>>(field&31)&1)) return;
    port_fatal("scenario: unavailable field %d in current archive (initial fixture disc %d, field %d); supply appropriate story/event state",
               field,scenario.disc,scenario.field);
}
void port_scenario_field_exit(int reason); /* (scenario_field.c, ovl_0a's) */
void port_scenario_field_ready(int field) {
    if(scenario.enabled && scenario.consumed) {
        port_log("scenario field ready: %d\n",field);
        if(scenario.tetra>0 && scenario.kind==1) { /* (once: the card game returns to the field) */
            port_scenario_field_exit(scenario.tetra);
            port_log("scenario: field exit %d\n",scenario.tetra);
            scenario.tetra=-1;
        }
        if(scenario.menu>0 && scenario.kind==1) {
            if(scenario.menu-1==3) { /* (party select: its request, as event op B2 builds it: the party, the rest in reserve) */
                static u8 req[24] __attribute__((aligned(4)));
                __builtin_memset(req,0xFF,sizeof req);
                req[0]=(u8)(scenario.menu_arg ? scenario.menu_arg : 4);
                req[1]=req[2]=req[3]=0;
                int n=0,r=0;
                for(int i=0;i<4;i++) {
                    Character *c=g_gameState->gameData->party[i];
                    if(c) req[4+n++]=(u8)(c-g_gameState->gameData->characters);
                }
                for(int id=0;id<9 && r<8;id++) {
                    int inp=0;
                    for(int k=0;k<n;k++) inp|=req[4+k]==id;
                    if(!inp) req[8+r++]=(u8)id;
                }
                __builtin_memset(req+16,0,8);
                menu_open(3,(s32)req);
            } else
            menu_open(scenario.menu-1,scenario.menu_arg);
            port_log("scenario: menu %d,%d\n",scenario.menu-1,scenario.menu_arg);
            scenario.menu=-1;
        }
    }
}
/* Battle-only fixtures may omit a field unavailable on their disc, but an
 * actual return requires valid saved ranges instead of an unbounded CD read. */
void port_scenario_validate_return(void) {
    if(scenario.enabled && scenario.consumed && scenario.kind==2 && !scenario.return_field_ready)
        port_fatal("scenario: battle returned to field %d without valid return-field context; set field= to an on-disc field",scenario.field);
}
/* Bytes of a CD extent read a sector at a time into one sector of stack (port_scenario's bundle lookups) */
typedef struct { int sector,size,cached; u8 buf[2048]; } SectorCursor;
static u8 cur_u8(SectorCursor *c,int off) {
    if(off<0 || off>=c->size) port_fatal("scenario: encounter bundle read past its extent (%d)",off);
    int s=off>>11;
    if(s!=c->cached) {cd_readAsync(c->sector+s,2048,c->buf,NULL);while(cd_getState()) {}c->cached=s;}
    return c->buf[off&2047];
}
static u32 cur_u16(SectorCursor *c,int off) {return cur_u8(c,off)|(u32)cur_u8(c,off+1)<<8;}
static u32 cur_u32(SectorCursor *c,int off) {return cur_u16(c,off)|cur_u16(c,off+2)<<16;}
static void prepare_battle_resources(GameData *d) {
    /* The ordinary field/world transition supplies these two CD ranges.
     * Resolve the encounter's bundle and its background dependency directly
     * from the selected disc's own directories instead of fabricating them. */
    DirInfo *dirs=g_gameState->dirs;
    FileIdEntry *entries=(FileIdEntry *)(g_loadAreaEnd+0x14c700+(dirs[5].tableSector<<11));
    int found=-1;
    for(int i=0;i<dirs[5].count;++i) if(entries[i].id==scenario.battle) {found=i;break;}
    if(found<0) port_fatal("scenario: battle %d is absent on disc %d",scenario.battle,scenario.disc);
    d->sceneSector[0]=entries[found].sector;
    d->sceneSectors[0]=entries[found+1].sector-entries[found].sector;
    int size=d->sceneSectors[0]<<11;if(size>0x2800) size=0x2800;
    if(size<=0) port_fatal("scenario: invalid encounter bundle extent");
    /* the bundle's DB container read a sector at a time (the heap has no room for its 10 KB head at bootstrap):
     * block 0x14's item 0, the dependency table {u16 ?, u16 n, n x {key, sector, sectors, split}} */
    SectorCursor c={.sector=g_gameState->imgSector+d->sceneSector[0],.size=size,.cached=-1};
    if(cur_u8(&c,0)!=0xdb) port_fatal("scenario: encounter bundle is not a DB container");
    int table=-1;
    for(int i=0,n=cur_u8(&c,1);i<n && table<0;++i) {
        int ent=4+i*4;u32 v=cur_u32(&c,ent);
        if(((v>>24)&0x7f)!=0x14) continue;
        int blk=ent+(int)(v&0xffffff),cnt=cur_u8(&c,blk+1),ofs=(blk+4+cnt*2+3)&~3;
        for(int j=0;j<cnt;++j) if(cur_u16(&c,blk+4+2*j)==0) {int o=ofs+4*j;table=o+(int)(cur_u32(&c,o)&0xffffff);break;}
    }
    if(table<0) port_fatal("scenario: encounter dependency table missing");
    found=0;
    for(int i=0,n=cur_u16(&c,table+2);i<n;++i) {
        int e=table+4+16*i;
        if((cur_u32(&c,e)&0xff0000)!=0xc0000) continue;
        if(found++) port_fatal("scenario: multiple backgrounds require explicit selection");
        d->sceneSector[1]=cur_u32(&c,e+4);
        d->sceneSectors[1]=cur_u32(&c,e+8);
    }
    if(!found || d->sceneSectors[1]<=0) port_fatal("scenario: encounter background dependency missing");
}
void port_scenario_bootstrap(void) {
    if(!scenario.enabled || scenario.consumed) return;
    scenario.consumed=1;
    /* Main has initialized allocator/file tables/GPU/GameData. Use the
     * title overlay's New Game initializer without entering its loop. */
    game_loadModeOverlay(0);o14_opening_initNewGame();
    cache_field_ids();
    GameData *d=g_gameState->gameData;
    d->firstUseFxSeen=(u16)scenario.summons_seen;
    if(scenario.gil>=0) d->gil=(u32)scenario.gil;
    _Static_assert(sizeof(Character)==0x90,"scenario character layout");
    load_blob(scenario.characters,d->characters,sizeof d->characters);
    load_blob(scenario.event_blob,&g_evtGlobalVars,sizeof g_evtGlobalVars);
    if(scenario.story>=0) g_evtGlobalVars.h[0]=(s16)scenario.story;
    for(int i=0;i<0x800;++i) if(scenario.events_set[i>>3]&(1u<<(i&7))) g_evtGlobalVars.b[i]=scenario.events[i];
    for(int id=0;id<100;++id) for(int k=0;k<scenario.cards[id];++k) card_add(id);
    for(int id=0;id<256;++id) if(scenario.keyitems[id>>3]>>(id&7)&1) keyItem_add(id);
    for(int id=0;id<256;++id) if(scenario.items[id]>=0) {
        ItemSlot *slot=item_findSlot_inline(id);
        if(!slot && scenario.items[id]) {
            for(int i=0;i<256;++i) if(!d->items.slots[i].count) {slot=&d->items.slots[i];break;}
            if(!slot) port_fatal("scenario: item inventory full");
            slot->id=(u8)id;
        }
        if(slot) slot->count=(u8)scenario.items[id];
    }
    for(int i=0;i<4;++i) {
        int ch=scenario.party[i];party_setMember(i,ch);if(ch<0) continue;
        Character *c=&d->characters[ch];
        if(scenario.preset[i]>=0) {
            int preset=scenario.preset[i];
            u8 flags=chr_getClassFlags(preset);
            if(chr_getClass(preset)!=g_chrAppearanceTypes[ch][(flags>>4)&1])
                port_fatal("scenario: preset %d incompatible with character %d",preset,ch);
            c->info.b.row=preset;
            c->classFlags=flags;
            int defaults=chr_getDefaultSet(c);
            chr_setDefaultName(c->name,defaults);
            chr_setDefaultEquip(c->equip,defaults);
        }
        for(int j=0;j<5;++j) if(scenario.equip[i][j]>=0) c->equip[j]=(u8)scenario.equip[i][j];
        c->info.b.kind=chr_calcAppearance(c->info.b.id,(c->classFlags>>4)&1,c->equip);
        if(scenario.level>0 || scenario.preset[i]>=0) chr_resetToLevel(ch,scenario.level>0?scenario.level:c->level,1);
        if(scenario.trance[i]>=0) c->trance=(u8)scenario.trance[i];
        if(scenario.hp[i]>=0) {if(scenario.hp[i]>c->maxHp) port_fatal("scenario: HP exceeds maximum");c->hp=(u16)scenario.hp[i];}
    }
    if(scenario.chocograph) d->chocograph=scenario.chocograph;
    rand_init((s32)scenario.seed);g_gameState->mode=(u8)scenario.kind;
    g_gameState->prevMode=1; /* ordinary battle entry from a field */
    d->fieldId=(u16)scenario.field;
    if(scenario.kind==1) o14_opening_findSceneSectors();
    else if(scenario.kind==3) { /* as a save made on the world map: wm_load picks the world script by worldId and area (68 for
                                  * every world file: decomp R37) */
        d->worldId=(u16)scenario.world;d->worldAreaId=68;o14_opening_findSceneSectors();
    }
    else {
        if(scenario.world_explicit) prepare_return_world(d); else prepare_return_field(d);
        prepare_battle_resources(d);d->battleId=(u16)scenario.battle;
    }
#ifndef PORT_NDS
    { /* FF9_SCENARIO_EXPORT=DIR (host): the party and story as bootstrapped, for the original PS1 to take the same
       * state (tools/psx_field_fixture.py, tools/psx_battle_fixture.py) */
        const char *dir=getenv("FF9_SCENARIO_EXPORT");
        if(dir&&*dir) {
            char path[512];FILE *f;
            snprintf(path,sizeof path,"%s/characters.bin",dir);
            if((f=fopen(path,"wb"))) {fwrite(d->characters,1,sizeof d->characters,f);fclose(f);}
            snprintf(path,sizeof path,"%s/events.bin",dir);
            if((f=fopen(path,"wb"))) {fwrite(&g_evtGlobalVars,1,sizeof g_evtGlobalVars,f);fclose(f);}
        }
    }
#endif
    port_log("scenario entered without title/intro: mode %d, disc %d, id %d\n",scenario.kind,scenario.disc,
             scenario.kind==1?scenario.field:scenario.kind==2?scenario.battle:scenario.world);
}

int port_scenario_blackjack(void) { return scenario.enabled && scenario.blackjack; }
