/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _DC_BLACKROCK_DEPTHS_LYCEUM_H
#define _DC_BLACKROCK_DEPTHS_LYCEUM_H

#include "Define.h"

// Blackrock Depths' Lyceum (map 230): the room-scoped half of the brazier
// events, the work that has to happen IN COMBAT and on every member, where the
// leader's non-combat objective action never reaches. Implemented beside the
// torch and brazier hooks in BlackrockDepthsEvents.cpp; ticked from the world
// update (DungeonClearReaperScript).
namespace DcLyceum
{
    // Throttled internally (500ms). For every clearing party on map 230:
    //   * PETS PASSIVE inside the room. A bot's pet in the Lyceum box is held
    //     REACT_PASSIVE and on follow; mod-playerbots' PetAttackAction refuses a
    //     passive pet, so it neither defends nor is sent in. Live, pets took
    //     15-30% of all Reservist hits and most of the ones far from the tank.
    //     Its prior stance is put back the moment the owner leaves the box, the
    //     run stops or pauses, or the module goes off — which is why this ticks
    //     ahead of the module gate and is told whether the module is on.
    //     The same hold covers every member while the Emperor's throne room is
    //     pre-cleared and he still sleeps (a pet waking him brings the room).
    //   * TORCHES LOOTED IN COMBAT. A Flame Keeper corpse holding its torch is
    //     looted by any member standing at it, fight or no fight: the corpse
    //     decays in 60s and the Reservists can keep the party fighting longer.
    void Tick(uint32 diff, bool moduleEnabled);
}

#endif  // _DC_BLACKROCK_DEPTHS_LYCEUM_H
