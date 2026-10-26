/* The scenario layer's field-side hooks (port/game/scenario.c), which need ovl_0a's types: in its code range. */
#include "psyq_port.h"
#include "ovl_0a.h"
#include "native_mesh.h"

void port_scenario_tetra_exit(void);

/* the field left for a card game at once, as event result 7 leaves it (fld_main.c) */
PORT_OVERLAY_CODE(0a) void port_scenario_tetra_exit(void) {
    g_gameState->gameData->unk8E4.field->area->scene->exitReason = FLD_EXIT_TETRA_MASTER;
    g_gameState->unk0 |= GS_FLD_LEAVE;
}
