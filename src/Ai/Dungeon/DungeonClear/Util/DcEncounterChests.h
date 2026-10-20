/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCENCOUNTERCHESTS_H
#define _PLAYERBOT_DCENCOUNTERCHESTS_H

#include <algorithm>
#include <array>
#include <cstdint>

// Chests that ARE an encounter's loot — the boss drops nothing, the script
// spawns (or respawns) a chest on completion and the reward sits in that. These
// are looted even with DungeonClear.IgnoreChests on (the default): that setting
// exists to stop the tank detouring to trash chests off the route, and applied to
// these it silently throws the encounter's whole reward away.
//
// Only chests the STOCK loot pipeline can open belong here: every lock below is a
// skill-less LOCK_KEY_SKILL (Lock.dbc 57 = OPEN/TREASURE, 1634 = QUICK_OPEN), so
// OpenLootAction's generic opening spell works. A key-item chest (Mechanar's Cache
// of the Legion) needs its own hook, and the Karazhan chess conductor opens the
// Dust Covered Chest itself, so neither is listed.
namespace DcEncounterChests
{
    inline constexpr std::array<uint32_t, 7> ENTRIES = {
        169243,  // Chest of The Seven — BRD Tomb of the Seven, respawned on TYPE_TOMB_OF_SEVEN DONE (lock 57)
        190663,  // Dark Runed Chest — CoS Mal'ganis, normal (lock 1634)
        193597,  // Dark Runed Chest — CoS Mal'ganis, heroic
        191349,  // Cache of Eregos — Oculus, normal (lock 1634)
        193603,  // Cache of Eregos — Oculus, heroic
        190586,  // Tribunal Chest — Halls of Stone, normal (lock 1634)
        193996,  // Tribunal Chest — Halls of Stone, heroic
    };

    inline constexpr bool IsEncounterChest(uint32_t goEntry)
    {
        return std::find(ENTRIES.begin(), ENTRIES.end(), goEntry) != ENTRIES.end();
    }
}

#endif  // _PLAYERBOT_DCENCOUNTERCHESTS_H
