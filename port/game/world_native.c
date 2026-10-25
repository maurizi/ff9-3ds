/* World map blocks drawn natively (config/overrides.txt): wm_drawBlock (ovl_0c w_memoryman.c) as the game wrote it,
 * except that after its vertex pass the renderer may take the block's faces (gx_hybrid.c port_native_world_block)
 * instead of the face routine writing them as PS1 packets. */
#include "psyq_port.h"
#include "ovl_0c.h"
#include "native_mesh.h"
#include "port_hw.h"

void o0c_wm_blockVertsFog(void *o);
void o0c_wm_blockVertsNear(void *o);
void o0c_wm_blockVertsFar(void *o);
s32 o0c_wm_blockFacesFog(void *o, s32 pkt, s32 ot);
s32 o0c_wm_blockFacesNear(void *o, s32 pkt, s32 ot);
s32 o0c_wm_blockFacesFar(void *o, s32 pkt, s32 ot);
void o0c_wm_drawBlock(u8 *arg0, s32 arg1, s32 arg2);

extern u32 D_800D1FF0[]; /* per-vertex fog colours */
extern u32 D_800D23F0[]; /* per-vertex lit colours */
extern u16 D_800D29F8[]; /* per-vertex depths (SZ, u32, for the world blocks) */

/* Without the hybrid renderer (PC, the DS's software paths) every block writes its packets. */
__attribute__((weak)) int port_native_world_block(const NativeWorldBlock *b) { (void)b; return 0; }

void o0c_wm_drawBlock(u8 *arg0, s32 arg1, s32 arg2) {
    s32 t = g_worldOt[*g_worldBufIndex];
    (void)arg1;
    g_worldCurTexArea = &g_worldTexAreas[arg0[1]];
    int variant = g_worldFog != 0 ? WORLD_FACES_FOG : arg2 == 0 ? WORLD_FACES_NEAR : WORLD_FACES_FAR;
    if (variant == WORLD_FACES_FOG) o0c_wm_blockVertsFog(arg0);
    else if (variant == WORLD_FACES_NEAR) o0c_wm_blockVertsNear(arg0);
    else o0c_wm_blockVertsFar(arg0);
    NativeWorldBlock n;
    n.block = arg0;
    n.sxy = (const unsigned int *)SCRATCHPAD;
    n.sz = (const unsigned int *)D_800D29F8;
    n.lit = (const unsigned int *)D_800D23F0;
    n.fog = (const unsigned int *)D_800D1FF0;
    n.alt = (const unsigned int *)g_worldCurTexArea;
    n.fog_params = (const unsigned int *)g_worldFogParams;
    n.variant = variant;
    n.draw_x = g_gameState->drawEnv[g_gameState->gameData->unk8].clip.x;
    n.draw_y = g_gameState->drawEnv[g_gameState->gameData->unk8].clip.y;
    if (port_native_world_block(&n)) return;
    if (variant == WORLD_FACES_FOG) g_worldPrimPtr = o0c_wm_blockFacesFog(arg0, g_worldPrimPtr, t);
    else if (variant == WORLD_FACES_NEAR) g_worldPrimPtr = o0c_wm_blockFacesNear(arg0, g_worldPrimPtr, t);
    else g_worldPrimPtr = o0c_wm_blockFacesFar(arg0, g_worldPrimPtr, t);
}
