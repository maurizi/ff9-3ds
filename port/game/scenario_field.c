/* The field-side hooks of the scenario layer (port/game/scenario.c) and the field HUD, which need ovl_0a's types: in
 * its code range. */
#include "psyq_port.h"
#include "ovl_0a.h"
#include "native_mesh.h"

void port_scenario_field_exit(int reason);

/* the field left at once with this exit, as the event results that set it leave it (fld_main.c: 4 the ending,
 * FLD_EXIT_GAME_OVER, FLD_EXIT_TETRA_MASTER) */
PORT_OVERLAY_CODE(0a) void port_scenario_field_exit(int reason) {
    g_gameState->gameData->ctx.field->area->scene->exitReason = (u8)reason;
    g_gameState->flags |= GS_FLD_LEAVE;
}

static int in_ram(const void *p) {
    extern unsigned char psx_ram[];
    return (const unsigned char *)p >= psx_ram + 0x10000 && (const unsigned char *)p < psx_ram + 0x200000 - 0x400;
}
/* field_hud.c: an encounter begun (fld_startEncounter: the swirl into the battle runs, still in field mode), 0 if not
 * or no field is up */
int port_field_encountering(void);
PORT_OVERLAY_CODE(0a) int port_field_encountering(void) {
    GameData *g = g_gameState->gameData;
    if (g_gameState->mode != GS_MODE_FIELD || !g || !in_ram(g->ctx.field)) return 0;
    FieldMapObj *a = g->ctx.field->area;
    if (!in_ram(a) || !in_ram(a->scene)) return 0;
    return a->scene->exitReason == FLD_EXIT_BATTLE;
}
