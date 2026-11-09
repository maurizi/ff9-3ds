/* Port runtime services shared by port/psx and port/ds. */
#ifndef PSX_RUNTIME_H
#define PSX_RUNTIME_H

/* psxrel.bin (port/psx/psxrel.c, tools/gen_relocs.py): the relocations of the game's data, made from the user's
 * discs; the program carries none of the game's bytes. Sections: 0x100 main, an overlay's FF9.IMG file id, 0x1000 +
 * an effect module's index in psx_fxmods. */
void psx_relocs_load(void);
const unsigned int *psx_relocs(unsigned int section); /* NULL: no such section */
/* convert the pointer words of a relocation list that fall in psx_ram [off, off + len) (a word that no longer holds
 * a PS1 address, converted already, is left alone) */
void psx_relocate(const unsigned int *relocs, unsigned int off, unsigned int len);
#define PSXREL_MAIN 0x100u
#define PSXREL_FXMOD 0x1000u

/* Code overlays (generated): FF9.IMG dir-0 file id and slot (0x800A7000, 0x801EDF00, 0x80198700); id 0 ends. */
typedef struct {
    unsigned int id;
    unsigned int slot;
} PsxOverlay;

extern const PsxOverlay psx_overlays[];

void psx_overlay_patch(unsigned int id, unsigned int off, unsigned int len);

/* Load addresses of the code overlay slots (index = PsxOverlay.slot). */
#define PSX_OVL_SLOT_ADDRS {0x800A7000, 0x801EDF00, 0x80198700}
extern unsigned char psx_ovl_resident[3];

/* main's data from the disc's executable (psxrel.bin's main section: its byte ranges, their hash, relocations) */
void psx_load_image(void);
/* The disc's boot executable (SYSTEM.CNF's BOOT line): its first sector and size; 0 if the disc has none
 * (port/psx/cd.c) */
int psx_cd_boot_exe(unsigned int *lba, unsigned int *size);
void psx_cd_read_data(unsigned int lba, void *buf, unsigned int n);
/* Port buffers in PS1 RAM the port never uses (main's MIPS code range, ~335 KB); fatal when it runs out. */
void *psx_spare_alloc(unsigned int size);
void psx_mem_init(void); /* the VRAM and SPU RAM shadows (port/psx/mem.c) */

/* Load main's data image and enter the game's main loop (decomp game_main). Never returns. */
void psx_run_game(void);
void port_scenario_load(const char *path);
void port_scenario_bootstrap(void);
void port_scenario_validate_return(void);
void port_scenario_battle_ready(void);
void port_scenario_field_entering(int field);
void port_scenario_field_ready(int field);
void port_field_entering(int field); /* (around fld_load: port/game/field_hud.c) */
void port_field_ready(int field);
int port_scenario_seed(int ordinary_seed);

/* Print to the debug console. */
void port_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Diagnostic memory-corruption canary between major phases; `where` names the phase just finished. A no-op
 * unless the platform implements it (DS CANARY=1 builds check the IRQ vector at the top of DTCM). */
void port_canary(const char *where);

/* Print to the debug console and stop. */
void port_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* Called by generated stubs for routines the port doesn't provide yet. */
void port_stub_hit(const char *name) __attribute__((noreturn));

/* LoadImage's recorder (gpu.c): the rectangle, VRAM there before, and what was loaded */
#define PSX_LOAD_RECS 24
typedef struct { short x, y, w, h; unsigned short *was, *given; } PsxLoadRec;
extern int psx_load_record, psx_n_load_recs;
extern PsxLoadRec psx_load_recs[PSX_LOAD_RECS];
void psx_load_records_free(void);

#endif
