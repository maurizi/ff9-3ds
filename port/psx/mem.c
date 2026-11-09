/* The PS1 address space: psx_ram (see psx_mem.h), the scratchpad, the SPU register file, and the data images. */
#include <stdlib.h>
#include <string.h>

#include "psx_mem.h"
#include "platform.h"
#include "psx_runtime.h"

/* psx_ram itself is defined by the platform: a bss array on the DS, a fixed mapping at 0x02000000 on the PC. */
unsigned char psx_scratchpad[0x400] __attribute__((aligned(32)));
unsigned short psx_spu_regs[0x200];

/* File id of the overlay loaded in each slot (0x800A7000, 0x801EDF00, 0x80198700); 0 = none. */
unsigned char psx_ovl_resident[3];

unsigned int psx_hw_bad(unsigned int a) {
    port_fatal("access to unmapped PS1 hardware address 0x%08X", a);
}

void psx_bad_overlay_call(const char *tramp) {
    port_fatal("%s called with overlays %02X/%02X/%02X resident", tramp, psx_ovl_resident[0], psx_ovl_resident[1],
               psx_ovl_resident[2]);
}

/* PS1 RAM the port never touches: main's MIPS code range (0x800127A0-0x800665E0). The PS1 copies that code there;
 * the port runs native code and loads only main's data, and the range stayed zero in field, battle and New Game
 * runs (docs/ds-mode.md). Port buffers that would otherwise take DS heap live here, allocated once. (The PS1 stack
 * above the highest overlay is not free: battles load disc sectors to 0x801FCF00 and up.) */
#define SPARE_LO 0x12800
#define SPARE_HI 0x66500
void *psx_spare_alloc(unsigned int size) {
    static unsigned int next = SPARE_LO;
    size = (size + 31) & ~31u;
    if (next + size > SPARE_HI) port_fatal("psx_spare_alloc: %u bytes over the spare range", size);
    void *p = psx_ram + next;
    next += size;
    return p;
}

/* main's data, from the disc's own executable (the same on all four discs): psxrel.bin says where its bytes go,
 * their hash (a disc whose executable isn't the one it was made from stops here, not later on wrong data) and which
 * words are pointers */
void psx_load_image(void) {
    const unsigned int *m = psx_relocs(PSXREL_MAIN);
    if (!m) port_fatal("psxrel.bin has no section for the executable");
    const unsigned int want_size = m[0], want_hash = m[1], nchunks = m[2];
    const unsigned int *chunks = m + 3, *relocs = chunks + 3 * nchunks;
    unsigned int lba, size;
    if (!psx_cd_boot_exe(&lba, &size)) port_fatal("the disc has no boot executable (SYSTEM.CNF)");
    if (size != want_size) port_fatal("the disc's executable is %u bytes, not %u: not FF9 (NTSC-U)", size, want_size);
    const unsigned int sectors = (size + 2047) / 2048;
    unsigned char *exe = malloc(sectors * 2048);
    if (!exe) port_fatal("no room to read the executable (%u bytes)", size);
    psx_cd_read_data(lba, exe, sectors);
    unsigned int h = 0x811C9DC5u;
    for (unsigned int c = 0; c < nchunks; c++)
        for (unsigned int i = 0; i < chunks[3 * c + 1]; i++) h = (h ^ exe[chunks[3 * c + 2] + i]) * 0x01000193u;
    if (h != want_hash) port_fatal("the disc's executable isn't the one psxrel.bin was made from (FF9 NTSC-U)");
    for (unsigned int c = 0; c < nchunks; c++) {
        memcpy(psx_ram + chunks[3 * c], exe + chunks[3 * c + 2], chunks[3 * c + 1]);
        psx_relocate(relocs, chunks[3 * c], chunks[3 * c + 1]);
    }
    free(exe);
}

void game_main(void); /* the game's main loop (decomp src/main/main.c) */
void port_ovl_native_load(unsigned int id);
void plat_guard_unused_ram(int on);

extern unsigned short *psx_vram;
extern unsigned char *psx_spu_ram;
extern unsigned psx_spu_ram_size, psx_spu_ram_split[2];
extern unsigned char *psx_spu_ram_mid, *psx_spu_ram_hi;

/* The VRAM and SPU RAM shadows (1.5 MB): allocated, so a 4 MB DS can still run the game logic without them. */
#ifdef PORT_CANARY
/* CANARY=1: a patterned tail after each shadow (32 KiB, DS mode 8), checked by port_canary */
unsigned short *psx_vram_tail;
unsigned char *psx_spu_ram_tail;
#ifdef PORT_DS_MODE
#define SHADOW_TAIL 0x2000 /* (DS mode's heap has 26 KB past the shadows) */
#else
#define SHADOW_TAIL 0x8000
#endif
#else
#define SHADOW_TAIL 0
#endif
#ifdef PORT_NDS
#include <malloc.h>
#include <unistd.h>
extern char *fake_heap_end; /* where libnds's sbrk stops (set at start-up) */
/* what the heap can still give: the arena's free blocks and the room before its end */
unsigned int psx_heap_free(void);
#define heap_free psx_heap_free
unsigned int psx_heap_free(void) { return mallinfo().fordblks + (unsigned int)(fake_heap_end - (char *)sbrk(0)); }
#endif
/* The DS calls it first thing (port/ds/platform.c), before anything else takes heap: the shadows are most of it */
void psx_mem_init(void) {
    static int done;
    if (done) return;
    done = 1;
#ifdef PORT_NDS
    unsigned int before = heap_free();
#endif
#ifdef PORT_HW_SOUND
    /* The DS mixes in hardware, reading SPU RAM a block at a time, and never runs the PS1 reverb (spu.c):
     * - FF9's work area (0x79020 up, "studio large", in every scene measured) isn't kept;
     * - the first 56 KB live in PS1 RAM that only the BIOS used (0x2000-0xFFFF: no scene measured reads or writes
     *   it once the game runs, docs/ds-mode.md; the BIOS's own data the game reads through NULL is below 0x2000);
     * - on an original DS the next 32 KB in the shared WRAM, which its ARM7 leaves to the ARM9
     *   (port/arm7/main.c, port/ds/platform.c). */
    psx_spu_ram_size = 0x79020;
    psx_spu_ram_split[0] = psx_spu_ram_split[1] = 0xE000;
    psx_spu_ram = psx_ram + 0x2000;
#ifdef PORT_DS_MODE
    psx_spu_ram_split[1] = 0x16000;
    psx_spu_ram_mid = (unsigned char *)0x03000000 - psx_spu_ram_split[0];
#endif
    /* one block for the VRAM shadow and SPU RAM's last piece: the allocator doesn't grow a free block at the heap's
     * end into a larger one, so two would need room for the first twice */
    unsigned char *block = malloc(1024 * 512 * 2 + SHADOW_TAIL + psx_spu_ram_size - psx_spu_ram_split[1] + SHADOW_TAIL);
    psx_vram = block ? (unsigned short *)block : NULL;
    psx_spu_ram_hi = block ? block + 1024 * 512 * 2 + SHADOW_TAIL - psx_spu_ram_split[1] : NULL;
    if (!block) psx_spu_ram = NULL;
#else
    psx_vram = malloc(1024 * 512 * 2 + SHADOW_TAIL);
    psx_spu_ram = malloc(psx_spu_ram_size + SHADOW_TAIL);
#endif
#ifdef PORT_CANARY
    if (psx_vram) psx_vram_tail = psx_vram + 1024 * 512, memset(psx_vram_tail, 0xA5, SHADOW_TAIL);
#ifdef PORT_HW_SOUND
    if (psx_spu_ram) psx_spu_ram_tail = psx_spu_ram_hi + psx_spu_ram_size, memset(psx_spu_ram_tail, 0xA5, SHADOW_TAIL);
#else
    if (psx_spu_ram) psx_spu_ram_tail = psx_spu_ram + psx_spu_ram_size, memset(psx_spu_ram_tail, 0xA5, SHADOW_TAIL);
#endif
    port_log("vram %p..%p spu %p\n", (void *)psx_vram, (void *)(psx_vram + 1024 * 512), (void *)psx_spu_ram);
#endif
    if (!psx_vram || !psx_spu_ram) /* (nothing can draw or play without them: a clear stop, not a crash later) */
#ifdef PORT_NDS
        port_fatal("no room for the VRAM/SPU shadows (%u KB free)", heap_free() >> 10);
#else
        port_fatal("no room for the VRAM/SPU shadows");
#endif
    if (psx_vram) {
        memset(psx_vram, 0, 1024 * 512 * 2);
    }
#ifdef PORT_NDS
    port_log("heap: %u KB free, %u KB after the shadows (end %p)\n", before >> 10, heap_free() >> 10,
             (void *)fake_heap_end);
#endif
}

void psx_run_game(void) {
    psx_load_low_ram();
    psx_mem_init();
    plat_video_init();
    port_ovl_native_load(0xFF); /* the DS reads main's image into an overlay slot first (gen_link.py) */
    psx_relocs_load();
    psx_load_image();
    plat_guard_unused_ram(1);
    game_main();
    port_fatal("the game's main loop returned");
}

/* Raw overlay bytes from the disc just landed in psx_ram at arena offset `off` (length `len`), and they are
 * overlay `id`'s file bytes at its slot. Overwrite them with the relocated image where the two overlap
 * (rodata/data: pointers fixed up for the port; code bytes stay raw and are never executed), and mark the
 * overlay resident. Called from libcd for every sector of a dir-0 code file. */
/* gx_hybrid.c keeps buffers in an overlay's code range: told when overlay bytes land (no-op without it) */
__attribute__((weak)) void gxh_ram_overwritten(unsigned int off, unsigned int len) { (void)off; (void)len; }
/* effect_exports.c adjusts words of a just-relocated overlay sector (no-op without it) */
__attribute__((weak)) void port_overlay_relocated(unsigned int id, unsigned int off, unsigned int len) {
    (void)id; (void)off; (void)len;
}

/* port/pc/platform.c: PS1 RAM the game never touches, which the DS reuses, faults on the PC (no-op elsewhere) */
__attribute__((weak)) void plat_guard_unused_ram(int on) { (void)on; }
/* port/ds/ovl_pack.c: the DS reads an overlay's native code when the game loads it; elsewhere it is linked in */
__attribute__((weak)) void port_ovl_native_load(unsigned int id) { (void)id; }
/* ... and again where disc sectors land on one kept in its own MIPS code range */
__attribute__((weak)) void port_ovl_native_sector(unsigned int id, unsigned int off, unsigned int len) {
    (void)id; (void)off; (void)len;
}
/* port/ds/fx_pack.c: the DS's loaded effect module lives in ovl_0b's code range */
__attribute__((weak)) void port_fx_area_overwritten(unsigned int off, unsigned int len) { (void)off; (void)len; }

void psx_overlay_patch(unsigned int id, unsigned int off, unsigned int len) {
    for (const PsxOverlay *o = psx_overlays; o->id != 0; o++) {
        if (o->id != id) {
            continue;
        }
        psx_ovl_resident[o->slot] = (unsigned char)id;
        port_ovl_native_load(id);
        port_ovl_native_sector(id, off, len);
        gxh_ram_overwritten(off, len);
        port_fx_area_overwritten(off, len);
        /* The disc bytes are in place; convert the pointer words in [off, off + len) (a sector read again after its
         * conversion is left alone) */
        const unsigned int *relocs = psx_relocs(id);
        if (!relocs) port_fatal("psxrel.bin has no section for overlay %02X", id);
        psx_relocate(relocs, off, len);
        port_overlay_relocated(id, off, len);
        return;
    }
}
