/* The GTE's depth shadow (gte.c, docs/design.md 4.3): its tables and psx_depth_lookup's body, inline for the
 * renderer's use of it on every vertex it draws (an out-of-line call from render.c each). Include after gte.h and
 * psx_mem.h. */
#ifndef PORT_GTE_DEPTH_H
#define PORT_GTE_DEPTH_H
#ifdef PORT_GTE_DEPTH
#include <stdint.h>
/* (a word and its info side by side: a lookup reads one cache line, not one in each of two 2 MB tables, which on an
 * Old 3DS (no L2 cache) were two misses for every vertex drawn and every one stored) */
typedef struct { gte_u32 word, inf; } PsxDepthSlot;
typedef struct { gte_u32 word, inf, frame; } PsxDepthValue;
#define PSX_DEPTH_VALUE_SLOTS 4096
extern PsxDepthSlot psx_depth_tab[PSX_RAM_SIZE / 4], psx_depth_sp_tab[256];
extern PsxDepthValue psx_depth_val_tab[PSX_DEPTH_VALUE_SLOTS];
/* the renderer's frame count (presents): a value entry older than 8 is stale (battles project every 4 VBlanks) */
extern unsigned int psx_depth_frame;
static inline unsigned psx_depth_value_slot(gte_u32 w) { return (w * 0x9E3779B1u) >> (32 - 12); }

/* psx_depth_lookup (gte.h): the info recorded for the vertex word at p, 0 if none or stale; *by_value set when it was
 * found by value */
static inline gte_u32 psx_depth_find(const gte_u32 *p, int *by_value) {
    const gte_u32 w = *p;
    *by_value = 0;
    uintptr_t off = (const unsigned char *)p - psx_ram;
    if (off < PSX_RAM_SIZE) {
        const PsxDepthSlot *d = &psx_depth_tab[off >> 2];
        if (d->word == w && d->inf) return d->inf;
    } else if ((off = (const unsigned char *)p - psx_scratchpad) < 0x400) {
        if (psx_depth_sp_tab[off >> 2].word == w) return psx_depth_sp_tab[off >> 2].inf;
    }
    const PsxDepthValue *e = &psx_depth_val_tab[psx_depth_value_slot(w)];
    if (e->word != w || psx_depth_frame - e->frame > 8) return 0;
    *by_value = 1;
    return e->inf;
}
#endif
#endif
