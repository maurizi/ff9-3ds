/* Native battle model meshes (port/game/mesh_native.c -> port/ds/gx_hybrid.c): a mesh the hybrid renderer draws
 * from its own data and projected vertices instead of the PS1 packets btl_drawMesh writes. */
#ifndef NATIVE_MESH_H
#define NATIVE_MESH_H

/* Port code only an overlay's own code calls, linked into that overlay's native area (DS: gen_link.py's slot 0, read
 * from ovls.bin with the overlay; it takes no resident memory while the slot's largest overlay sets its size) */
/* A field background layer taken as a unit (port/game/field_bg.c, gx_hybrid.c gxh_bg_unit): its tiles lifted out of
 * the OT, chained through their own links from head (count of them); a tile's OT entry is z plus the low 12 bits of
 * its position word (pos[(tile - sprites) / 24]) */
int gxh_bg_unit(const unsigned int *head, int count, const unsigned char *sprites, const unsigned int *pos, int z,
                const unsigned int *ot); /* (ot: the OT the entries are of) */
int gxh_bg_units_on(void); /* whether field_bg.c lifts layers as units */

#ifdef __arm__
#define PORT_OVERLAY_CODE(id) __attribute__((section(".psxovl_" #id ".port"), noinline))
/* the hottest code of a mode, in ITCM while its overlay is resident (gen_link.py's ITCM overlay, port/ds/ovl_pack.c):
 * ARM, called from main RAM through long calls */
#define PORT_ITCM_CODE(id) __attribute__((section(".itcmovl_" #id ".port"), long_call, target("arm"), noinline))
/* code only that overlay's mode runs, kept in the dead part of its MIPS code range in psx_ram (ovl0b_layout.h
 * OVL0B_RAMCODE_AT): read from ovls.bin where its disc sectors land (port/ds/ovl_pack.c); no resident memory */
#define PORT_RAM_CODE(id) __attribute__((section(".ramovl_" #id ".port"), noinline))
#else
#define PORT_RAM_CODE(id)
#define PORT_OVERLAY_CODE(id)
#define PORT_ITCM_CODE(id)
#endif

typedef struct {
    const unsigned char *src;      /* BtlMesh.prims: the eight source lists one after the other (mesh_packets.c) */
    unsigned short count[8];       /* nFT4, nFT3, nGT4, nGT3, nG4, nG3, nF4, nF3 */
    const unsigned short *uvs;     /* u | v << 8 */
    const unsigned int *colors;    /* RGB words */
    const unsigned int *tex;       /* the model's texture words: clut << 16 | tpage */
    int ot_bias;                   /* BtlMesh.unk13: OT entries nearer than the projected depths */
    const unsigned int *sxy;       /* g_vtxScreenXY: projected y << 16 | x */
    const unsigned int *sz;        /* g_vtxScreenZ */
    int nverts;
    int draw_x, draw_y;            /* the frame's drawing area (its scene's buffer) */
    int strips;                    /* its FT3s carry strip links (mesh_native.c strip_order) */
} NativeMesh;

/* 1: the renderer took the mesh for the scene being drawn (DS hybrid battle); 0: draw its packets */
int port_native_mesh(const NativeMesh *m);

/* A battle background record (btlbg_drawRecord, src_equiv/ovl_0b/background.c): textured quads then triangles
 * over packed 6-byte vertices the GTE's current matrices project. */
typedef struct {
    const unsigned char *rec;      /* BtlBgRec */
    const unsigned char *verts;    /* its packed vertices (x, y, z s16 each) */
    const unsigned int *textab;    /* the group's texture words, by colour bits 24-28 */
    const unsigned int *ot;        /* the OT the record links into */
    unsigned int bright, shift;    /* the low halfwords of g_btlbgBrightness and g_btlbgOtShift */
    int draw_x, draw_y;            /* the frame's drawing area */
} NativeBg;

/* 1: the renderer took the record (nothing to link); 0: draw its packets */
int port_native_bg(const NativeBg *r);

/* A world map block (wm_drawBlock, ovl_0c) after the game's vertex pass (src_equiv/ovl_0c/world_verts.c): its faces
 * (world_faces.c) drawn by the renderer from the projected vertices instead of as PS1 packets. */
enum { WORLD_FACES_FOG, WORLD_FACES_NEAR, WORLD_FACES_FAR, WORLD_BACKDROP }; /* wm_blockFaces{Fog,Near,Far}, wm_backdropFaces */
typedef struct {
    const unsigned char *block;    /* WmBlock: vertex count (low byte of word 0), faces (16 bytes each) at +8 */
    const unsigned int *sxy;       /* the vertices' screen positions (the scratchpad) */
    const unsigned int *sz;        /* their depths, SZ (g_worldVtxDepth) */
    const unsigned int *lit;       /* their lit colours (g_worldVtxLitColors) */
    const unsigned int *fog;       /* their fog colours (g_worldVtxFogColors) */
    const unsigned int *alt;       /* the block's alternate texture page entry (*g_worldCurTexArea, 3 words) */
    const unsigned int *fog_params;/* g_worldFogParams: two { colour, fog offset | lit offset << 16 }; the backdrop's:
                                    * g_worldBackdropFog */
    int variant;                   /* WORLD_FACES_* */
    int draw_x, draw_y;            /* the frame's drawing area */
} NativeWorldBlock;

/* 1: the renderer took the block's faces (no packets); 0: the game's face routine writes them */
int port_native_world_block(const NativeWorldBlock *b);

/* A field model group (fldmdl_drawMesh, ovl_0a): its polygons drawn by the renderer from the model's vertices,
 * re-projected with the frame's bone matrices, and its prebuilt packets' colours and texture words, instead of the
 * packets linked into the OT. */
typedef struct {
    const void *model;             /* ModelObj */
    const unsigned char *group;    /* WmModelGroup: counts (+2 up), OT bias (+0x13), bone vertex counts (+0x14),
                                    * vertices (+0x18), polygon sources (+0x1C) */
    const unsigned char *packets;  /* this buffer's packets of the group */
    const unsigned int *ot;        /* the OT entry its depths count from (the frame's OT less its bias) */
    const int *sxy, *sz;           /* its vertices as fldmdl_projectMesh left them (g_vtxScreenXY, g_vtxScreenZ) */
    int nverts;
    int draw_x, draw_y;            /* the frame's drawing area */
    int clip_y;                    /* fldmdl_drawMeshClipY's: a polygon with a vertex projectMeshClipY flagged (its
                                    * depth word's high half) isn't drawn */
    int mirror;                    /* fldmdl_drawMeshMirrored's: a reflection, culled the other way (NCLIP >= 0) */
} NativeFieldMesh;

/* 1: the renderer took the group (nothing to link); 2: its opaque polygons (link the translucent ones); 0: link its
 * packets */
int port_native_field_mesh(const NativeFieldMesh *m);

#endif
