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
    int enabled,consumed,kind,disc,field,battle,world,world_explicit,level,story,field_explicit,return_field_ready,enemy_hp_applied,summons_seen,gil,tetra;
    unsigned seed;
    int party[4],hp[4],enemy_hp[4],trance[4],preset[4],equip[4][5];
    s16 events[0x800],items[256]; /* -1: unset */
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
    for(int i=0;i<0x800;++i) scenario.events[i]=-1;
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
        else if(!__builtin_strcmp(s,"level")) scenario.level=number(eq,1,99);
        else if(!__builtin_strcmp(s,"story")) scenario.story=number(eq,0,65535);
        else if(!__builtin_strcmp(s,"gil")) scenario.gil=number(eq,0,9999999);
        else if(!__builtin_strcmp(s,"tetra")) scenario.tetra=number(eq,0,1); /* kind=field: then a card game */
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
        else if(!__builtin_strncmp(s,"event.",6)) scenario.events[number(s+6,0,0x7ff)]=number(eq,0,255);
        else if(!__builtin_strncmp(s,"item.",5)) scenario.items[number(s+5,0,255)]=number(eq,0,99);
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
    d->unk7DC=(u16)scenario.world;
    g_gameState->mode=3;o14_opening_findSceneSectors();g_gameState->mode=mode;
    if(d->sceneSectors[1]<=0) port_fatal("scenario: world map %d absent on disc %d",scenario.world,scenario.disc);
    for(int i=0;i<2;i++) {
        d->returnSector[i]=d->sceneSector[i];d->returnSectors[i]=d->sceneSectors[i];
    }
    g_gameState->unkA=3;
    scenario.return_field_ready=1;
}
static void prepare_return_field(GameData *d) {
    typedef struct {u16 field,area;u32 sector;} FieldEntry;
    _Static_assert(sizeof(FieldEntry)==8,"field directory entry layout");
    FileDirEntry *dirs=(FileDirEntry *)g_gameState->dirs;
    const FieldEntry *entries=(const FieldEntry *)(g_loadAreaEnd+0x14c700+(dirs[4].unk8<<11));
    int found=0;
    for(int i=0;i<dirs[4].unk4;i++) if(entries[i].field==scenario.field) {found=1;break;}
    if(!found) {
        if(scenario.field_explicit) port_fatal("scenario: return field %d absent on disc %d",scenario.field,scenario.disc);
        /* A battle-only fixture still needs a field to return to: without one the game preloads stale sectors as the
         * return field while the rewards run, over ovl_65's own data. The disc's first field stands in. */
        if(dirs[4].unk4<=0) port_fatal("scenario: no field on disc %d to return to",scenario.disc);
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
static u16 field_ids[512];
static int n_field_ids=-1;
static void cache_field_ids(void) {
    typedef struct {u16 field,area;u32 sector;} FieldEntry;
    _Static_assert(sizeof(FieldEntry)==8,"field directory entry layout");
    FileDirEntry *dirs=(FileDirEntry *)g_gameState->dirs;
    const FieldEntry *entries=(const FieldEntry *)(g_loadAreaEnd+0x14c700+(dirs[4].unk8<<11));
    if(dirs[4].unk4>(s32)(sizeof field_ids/sizeof field_ids[0])) port_fatal("scenario: %d field entries",(int)dirs[4].unk4);
    for(n_field_ids=0;n_field_ids<dirs[4].unk4;n_field_ids++) field_ids[n_field_ids]=entries[n_field_ids].field;
}
void port_scenario_field_entering(int field) {
    if(!scenario.enabled || !scenario.consumed) return;
    if(n_field_ids<0) cache_field_ids();
    port_log("scenario field entering: %d\n",field);
    for(int i=0;i<n_field_ids;i++) if(field_ids[i]==field) return;
    port_fatal("scenario: unavailable field %d in current archive (initial fixture disc %d, field %d); supply appropriate story/event state",
               field,scenario.disc,scenario.field);
}
void port_scenario_tetra_exit(void); /* (scenario_field.c, ovl_0a's) */
void port_scenario_field_ready(int field) {
    if(scenario.enabled && scenario.consumed) {
        port_log("scenario field ready: %d\n",field);
        if(scenario.tetra==1 && scenario.kind==1) { /* (once: the card game returns to the field) */
            scenario.tetra=2;
            port_scenario_tetra_exit();
            port_log("scenario: Tetra Master\n");
        }
    }
}
/* Battle-only fixtures may omit a field unavailable on their disc, but an
 * actual return requires valid saved ranges instead of an unbounded CD read. */
void port_scenario_validate_return(void) {
    if(scenario.enabled && scenario.consumed && scenario.kind==2 && !scenario.return_field_ready)
        port_fatal("scenario: battle returned to field %d without valid return-field context; set field= to an on-disc field",scenario.field);
}
static void prepare_battle_resources(GameData *d) {
    /* The ordinary field/world transition supplies these two CD ranges.
     * Resolve the encounter's bundle and its background dependency directly
     * from the selected disc's own directories instead of fabricating them. */
    FileDirEntry *dirs=(FileDirEntry *)g_gameState->dirs;
    FileIdEntry *entries=(FileIdEntry *)(g_loadAreaEnd+0x14c700+(dirs[5].unk8<<11));
    int found=-1;
    for(int i=0;i<dirs[5].unk4;++i) if(entries[i].unk0==scenario.battle) {found=i;break;}
    if(found<0) port_fatal("scenario: battle %d is absent on disc %d",scenario.battle,scenario.disc);
    d->sceneSector[0]=entries[found].unk4;
    d->sceneSectors[0]=entries[found+1].unk4-entries[found].unk4;
    int size=d->sceneSectors[0]<<11;if(size>0x2800) size=0x2800;
    if(size<=0) port_fatal("scenario: invalid encounter bundle extent");
    u8 *buffer=malloc((unsigned)size);
    if(!buffer) port_fatal("scenario: no memory for encounter metadata");
    cd_readAsync(g_gameState->imgSector+d->sceneSector[0],size,buffer,NULL);
    while(cd_getState()) {}
    FileTable *table=(FileTable *)db_lookup((s32)buffer,4,0x140000,0);
    if((u8 *)table<buffer || (u8 *)table+4>buffer+size ||
       (u8 *)(table->unk4+table->unk2)>buffer+size)
        port_fatal("scenario: encounter dependency table outside metadata");
    found=0;
    for(int i=0;i<table->unk2;++i) if((table->unk4[i].unk0&0xff0000)==0xc0000) {
        if(found++) port_fatal("scenario: multiple backgrounds require explicit selection");
        d->sceneSector[1]=table->unk4[i].unk4;
        d->sceneSectors[1]=table->unk4[i].unk8;
    }
    free(buffer);
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
    for(int i=0;i<0x800;++i) if(scenario.events[i]>=0) g_evtGlobalVars.b[i]=(u8)scenario.events[i];
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
            if(chr_getClass(preset)!=D_8006CAA4[ch][(flags>>4)&1])
                port_fatal("scenario: preset %d incompatible with character %d",preset,ch);
            c->unk34.b.row=preset;
            c->unkA=flags;
            int defaults=chr_getDefaultSet(c);
            chr_setDefaultName(c->unk0,defaults);
            chr_setDefaultEquip(c->equip,defaults);
        }
        for(int j=0;j<5;++j) if(scenario.equip[i][j]>=0) c->equip[j]=(u8)scenario.equip[i][j];
        c->unk34.b.unk4=chr_calcAppearance(c->unk34.b.id,(c->unkA>>4)&1,c->equip);
        if(scenario.level>0 || scenario.preset[i]>=0) chr_resetToLevel(ch,scenario.level>0?scenario.level:c->level,1);
        if(scenario.trance[i]>=0) c->trance=(u8)scenario.trance[i];
        if(scenario.hp[i]>=0) {if(scenario.hp[i]>c->maxHp) port_fatal("scenario: HP exceeds maximum");c->hp=(u16)scenario.hp[i];}
    }
    rand_init((s32)scenario.seed);g_gameState->mode=(u8)scenario.kind;
    g_gameState->unkA=1; /* ordinary battle entry from a field */
    d->fieldId=(u16)scenario.field;
    if(scenario.kind==1) o14_opening_findSceneSectors();
    else if(scenario.kind==3) {d->unk7DC=(u16)scenario.world;o14_opening_findSceneSectors();} /* as a save made on the world map */
    else {
        if(scenario.world_explicit) prepare_return_world(d); else prepare_return_field(d);
        prepare_battle_resources(d);d->battleId=(u16)scenario.battle;
    }
    port_log("scenario entered without title/intro: mode %d, disc %d, id %d\n",scenario.kind,scenario.disc,
             scenario.kind==1?scenario.field:scenario.kind==2?scenario.battle:scenario.world);
}
