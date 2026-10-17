/* Port runtime services shared by port/psx and port/ds. */
#ifndef PSX_RUNTIME_H
#define PSX_RUNTIME_H

/* One chunk of a binary's initialised data (generated img_<bin>.s): copy `size` bytes to psx_ram + offset. */
typedef struct {
    unsigned int offset;
    unsigned int size;
    const void *data;
} PsxImageChunk;

extern const PsxImageChunk psximg_main[];

/* Code overlays (generated): FF9.IMG dir-0 file id, slot (0x800A7000, 0x801EDF00, 0x80198700), and the pointer
 * words to convert once the disc bytes are in (tools/gen_link.py: offset | kind << 29, code pointers followed by
 * the native address; 0xFFFFFFFF ends). */
typedef struct {
    unsigned int id;
    unsigned int slot;
    const unsigned int *relocs;
} PsxOverlay;

extern const PsxOverlay psx_overlays[];

void psx_overlay_patch(unsigned int id, unsigned int off, unsigned int len);

/* Load addresses of the code overlay slots (index = PsxOverlay.slot). */
#define PSX_OVL_SLOT_ADDRS {0x800A7000, 0x801EDF00, 0x80198700}
extern unsigned char psx_ovl_resident[3];

void psx_load_image(const PsxImageChunk *img);
/* Port buffers in PS1 RAM the port never uses (main's MIPS code range, ~335 KB); fatal when it runs out. */
void *psx_spare_alloc(unsigned int size);

/* Load main's data image and enter the game's main loop (decomp game_main). Never returns. */
void psx_run_game(void);
void port_scenario_load(const char *path);
void port_scenario_bootstrap(void);
void port_scenario_validate_return(void);
void port_scenario_battle_ready(void);
void port_scenario_field_entering(int field);
void port_scenario_field_ready(int field);
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

#endif
