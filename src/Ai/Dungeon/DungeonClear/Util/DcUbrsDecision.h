/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCUBRSDECISION_H
#define _PLAYERBOT_DCUBRSDECISION_H

#include <cstdint>

#include "Define.h"

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

// PURE kernels for Upper Blackrock Spire's event hooks (BlackrockSpireEvents.cpp).
// The hooks read the world and act; what they decide lives here so the gtests can
// walk every branch — in particular the three states mod-individual-progression
// can leave the Dragonspine Door in (see the events file's header).
namespace DcUbrs
{
    // --- the Dragonspine Door (hook 56) -----------------------------------
    // The hook has already armed the hall (fired AT 2046's script and IP's
    // SetData(2046, 26)) on the step's first tick when this is asked.
    //
    //   stock core           the door spawns open           -> Done at once
    //   IP compiled          SetData opens it in ~3s        -> Wait, then Done
    //   IP SQL, no IP code   closed, and nothing opens it   -> Wait, Click, Done
    //
    // Blocked once even the click has had its time: a visible stall at the door
    // instead of a silent one.
    enum class DoorStep : uint8
    {
        Done,
        Wait,
        Click,
        Blocked,
    };

    inline DoorStep DecideDragonspineDoor(bool doorOpen, uint32 elapsedMs)
    {
        namespace B = DcBlackrockSpire;
        if (doorOpen)
            return DoorStep::Done;
        if (elapsedMs < B::DOOR_SCRIPT_WAIT_MS)
            return DoorStep::Wait;
        if (elapsedMs < B::DOOR_SCRIPT_WAIT_MS + B::DOOR_CLICK_WAIT_MS)
            return DoorStep::Click;
        return DoorStep::Blocked;
    }

    // Which path opened the door, for the "UBRS door opened via ..." line. A door
    // that reads open on the step's first tick was open before the hook did
    // anything; one that opened after the click was the click's.
    inline char const* DoorOpenedVia(uint32 elapsedMs, bool clicked)
    {
        if (clicked)
            return "click";
        return elapsedMs < 1000 ? "already-open" : "ip-script";
    }

    // --- the hall runes (hook 57) -----------------------------------------
    // One tick of the sweep, from what it counted:
    //   litWithPack   lit runes with a living 9817/9818/9819 within
    //                 HALL_RUNE_CLEAR_RADIUS — something the ClearRadius steps
    //                 let slip (a fleer back home, a Summoner add)
    enum class HallStep : uint8
    {
        Done,     // Emberseer In is open: the room is DONE
        Restart,  // rewind onto the ClearRadius steps to kill what is left
        Rearm,    // re-fire AT 2046's script (re-snapshot + restart CHECK)
        Wait,     // CHECK runs every 3s; let it catch up
    };

    //
    // Restart and Rearm are INTENTS: the hook floors each one (HALL_RESTART_FLOOR_MS,
    // HALL_REARM_FLOOR_MS) and waits instead while the floor holds.
    inline HallStep DecideHallSweep(bool emberseerInOpen, uint32 litWithPack, uint32 elapsedMs)
    {
        namespace B = DcBlackrockSpire;
        if (emberseerInOpen)
            return HallStep::Done;
        if (litWithPack > 0)
            return HallStep::Restart;
        // Every rune unlit and the door still shut, or runes lit with nobody
        // left at them: either way CHECK is not going to finish it alone.
        if (elapsedMs >= B::HALL_REARM_AFTER_MS)
            return HallStep::Rearm;
        return HallStep::Wait;
    }
}

#endif  // _PLAYERBOT_DCUBRSDECISION_H
