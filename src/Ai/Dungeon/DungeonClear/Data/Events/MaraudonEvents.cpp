/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonWingRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"

#include <unordered_map>

// Maraudon (map 349) has no scripted events — its clear-data is the wing
// layout (three dungeon-finder wings plus the whole dungeon, chosen per run)
// and one roster removal (Rotgrip, who sits in open water). This file is
// Maraudon's definition unit so that data lives with its dungeon like every
// other. The TU stays linked because DungeonWingRegistry's aggregator calls
// RegisterMaraudonWings explicitly (and BossRosterRegistry's calls
// RegisterMaraudonRoster).

// --- wing layout (relocated from DungeonWingRegistry) --------------------
void RegisterMaraudonWings(std::unordered_map<uint32, DungeonWingLayout>& store)
{
    // --- Maraudon (map 349) --------------------------------------
    // Three regions — Orange (Foulspore Cavern), Purple (Wicked Grotto) and
    // the inner Pristine Waters (Earth Song Falls) — that, UNLIKE Dire Maul,
    // share one connected interior: orange and purple converge at the
    // Celebras seal, which opens into the inner waters. Every boss is
    // reachable from any entrance.
    //
    // The 3.3 dungeon finder still queues each region as its own dungeon,
    // with its own entrance: LFG 26 Orange Crystals (ends on Razorlash), 272
    // Purple Crystals (Vyletongue) and 273 Pristine Waters (Princess
    // Theradras), whose entrance drops the party in the inner waters. A
    // whole-map list there starts at Noxxion (bit 0) and walks the party back
    // out through the entire dungeon, so the wing is chosen per RUN
    // (WingSelect::Explicit, see DcRunWing): the queue-fill latches the LFG
    // dungeon's wing, `dc on <wing>` / a test row picks one by hand.
    //
    // With no choice made the run clears the WHOLE dungeon — the last wing,
    // "maraudon", lists all eight bosses and is the default — so a bare `dc
    // on` behaves as it always has. It sits last so WingOf/WingName still
    // label each boss with its region. There are no wing regions to fall back
    // on (the interior is connected), so the fallback is always "maraudon".
    //
    // Entries are the kill-creature credit-entries from instance_encounters
    // (what BossSpawnIndex emits). Celebras the Cursed sits at the purple-side
    // seal he unlocks, so he is grouped with Purple. Encounter bits from
    // DungeonEncounter.dbc (map 349, bits 0-7, entries 422-429): Noxxion,
    // Razorlash, Vyletongue, Celebras, Landslide, Gizlock, Rotgrip, Theradras.
    store[349] = {true, {
        {"Maraudon (Orange)", {
            13282,  // Noxxion
            12258,  // Razorlash
        }, "mara-orange", /*lfgDungeonId*/ 26, /*terminalBossEntry*/ 0, /*encounterMask*/ 0x03u},
        {"Maraudon (Purple)", {
            12236,  // Lord Vyletongue
            12225,  // Celebras the Cursed
        }, "mara-purple", /*lfgDungeonId*/ 272, /*terminalBossEntry*/ 0, /*encounterMask*/ 0x0Cu},
        {"Maraudon (Pristine Waters)", {
            13601,  // Tinkerer Gizlock
            12203,  // Landslide
            13596,  // Rotgrip — dropped from the clear (see the roster patch
                    // below); the label is kept so anything that resolves his
                    // entry to a region still names the right one.
            12201,  // Princess Theradras
        }, "mara-pw", /*lfgDungeonId*/ 273, /*terminalBossEntry*/ 12201, /*encounterMask*/ 0xF0u},
        {"Maraudon", {
            13282,  // Noxxion
            12258,  // Razorlash
            12236,  // Lord Vyletongue
            12225,  // Celebras the Cursed
            13601,  // Tinkerer Gizlock
            12203,  // Landslide
            13596,  // Rotgrip
            12201,  // Princess Theradras
        }, "maraudon"},
    }, WingSelect::Explicit, /*defaultWing*/ "maraudon"};
}

// --- roster patch: drop Rotgrip ------------------------------------------
void RegisterMaraudonRoster(std::vector<BossRosterPatch>& t)
{
    // --- Maraudon (map 349) --------------------------------------
    // ROTGRIP (13596, DBC bit 6) is SKIPPED — removed from the clear list, not
    // relocated.
    //
    // He is a crocolisk who spawns and lives in the Pristine Waters lake at
    // (42.08, -65.95, -199.55) — open water, not a shoreline platform. That is
    // the same navmesh shape as the Underbog basin the Ghaz'an anchor had to be
    // moved off (see UnderbogEvents.cpp): every mesh column over that lake is a
    // water SURFACE sheet, so boss-nav either routes the party into a swim or
    // fails outright. Unlike Ghaz'an there is no scripted event that walks him
    // out onto dry ground and no dry anchor within his leash, so there is
    // nothing to re-anchor to — remove + re-add can't fix a boss whose whole
    // encounter is in the water.
    //
    // Consequence: the party clears Gizlock -> Landslide -> Princess Theradras
    // and reports the dungeon done with Rotgrip's kill-bit unset. That is
    // intentional; he is an optional side boss off the main path to the
    // Princess, so skipping him costs no progression. Removing him (rather than
    // leaving him in for the run to stall on) is what keeps the clear moving:
    // an unreachable boss otherwise parks the tank at the water's edge until the
    // stalled-boss fallback fires, once per attempt.
    BossRosterPatch p;
    p.mapId = 349;
    p.remove = {13596};  // Rotgrip
    t.push_back(std::move(p));
}
