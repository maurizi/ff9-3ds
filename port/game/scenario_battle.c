/* Optional data overrides after the ordinary battle loader has built units. */
#include "psyq_port.h"
#include "ovl_0b.h"

void port_scenario_set_enemy_hp(const int *values) {
    Battle *battle=g_gameState->gameData->ctx.battle;
    unsigned found=0;
    for(BtlUnit *unit=battle->unitList;unit;unit=unit->next) {
        unsigned id=(unit->flags>>5)&7;
        if((unit->flags&1) || id<4 || values[id-4]<0) continue;
        unsigned slot=id-4;
        if(!unit->max || !unit->max->maxHp || values[slot]>unit->max->maxHp)
            port_fatal("scenario: enemy %u HP %d exceeds initialized maximum",slot,values[slot]);
        unit->hp=(u16)values[slot];found|=1u<<slot;
        port_log("scenario: enemy %u HP %u/%u\n",slot,unit->hp,unit->max->maxHp);
    }
    for(unsigned slot=0;slot<4;slot++) if(values[slot]>=0 && !(found&(1u<<slot)))
        port_fatal("scenario: enemy %u is absent from the initialized formation",slot);
}
