/* The scenario layer's field-side hooks (port/game/scenario.c), which need ovl_0a's types: in its code range. */
#include "psyq_port.h"
#include "ovl_0a.h"
#include "native_mesh.h"

void port_scenario_field_exit(int reason);

/* the field left at once with this exit, as the event results that set it leave it (fld_main.c: 4 the ending,
 * FLD_EXIT_GAME_OVER, FLD_EXIT_TETRA_MASTER) */
PORT_OVERLAY_CODE(0a) void port_scenario_field_exit(int reason) {
    g_gameState->gameData->unk8E4.field->area->scene->exitReason = (u8)reason;
    g_gameState->unk0 |= GS_FLD_LEAVE;
}
