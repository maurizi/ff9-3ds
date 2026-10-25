/* Native battle model meshes (port/game/mesh_native.c -> port/ds/gx_hybrid.c): a mesh the hybrid renderer draws
 * from its own data and projected vertices instead of the PS1 packets btl_drawMesh writes. */
#ifndef NATIVE_MESH_H
#define NATIVE_MESH_H

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

#endif
