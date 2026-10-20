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

    // --- musters (hooks 58, 60) -------------------------------------------
    // Done once nobody living is outside the room, or once the cap has run out
    // (CapReached: proceed, and say who is still outside).
    enum class MusterStep : uint8
    {
        Done,
        Wait,
        CapReached,
    };

    inline MusterStep DecideMuster(uint32 outside, uint32 elapsedMs, uint32 capMs)
    {
        if (outside == 0)
            return MusterStep::Done;
        return elapsedMs >= capMs ? MusterStep::CapReached : MusterStep::Wait;
    }

    // --- the Blackrock Altar (hook 62) ------------------------------------
    // The altar is a summoning ritual (type 18, one participant): Use() makes the
    // user channel 16532, and 5s later GameObject::Update casts 16533 -> event
    // 4884 -> Emberseer's release — but only if the user is STILL channelling.
    // A bot that moves or casts in those five seconds drops out and the altar
    // resets to READY.
    enum class AltarStep : uint8
    {
        Done,       // his release has started (IN_PROGRESS) or he is dead
        Hold,       // channelling: stand still
        Use,        // altar ready: use it
        Fallback,   // the ritual never took: ProcessEvent(4884) directly
        Wait,       // altar busy (mid-ritual or cooling down)
    };

    inline AltarStep DecideAltar(uint32 bossState, bool channelling, bool altarReady, uint32 elapsedMs)
    {
        namespace B = DcBlackrockSpire;
        if (bossState != B::STATE_NOT_STARTED)
            return AltarStep::Done;
        if (channelling)
            return AltarStep::Hold;
        if (elapsedMs >= B::ALTAR_FALLBACK_MS)
            return AltarStep::Fallback;
        return altarReady ? AltarStep::Use : AltarStep::Wait;
    }

    // --- Emberseer freed (hook 59) ----------------------------------------
    // He is unselectable and immune to players for ~69s after the altar; the
    // roster's Emberseer row must not be handed over before then. NOT_STARTED
    // here (after the altar has set IN_PROGRESS) is an incarcerator's Reset
    // putting the whole encounter back: rewind and run it again.
    enum class FreedStep : uint8
    {
        Done,
        Restart,
        Wait,
    };

    inline FreedStep DecideEmberseerFreed(uint32 bossState, bool alive, bool attackable)
    {
        namespace B = DcBlackrockSpire;
        if (bossState == B::STATE_DONE || !alive)
            return FreedStep::Done;
        if (bossState == B::STATE_NOT_STARTED)
            return FreedStep::Restart;
        return attackable ? FreedStep::Done : FreedStep::Wait;
    }

    // --- the stadium start (hook 61) --------------------------------------
    // Rend's state: IN_PROGRESS ~1s after AT 2026 fires, DONE on his death. FAIL
    // is transient — the instance respawns him and his Reset puts NOT_STARTED
    // back — so FAIL is waited out, and NOT_STARTED (a fresh stadium, or one that
    // has just reset) fires the area trigger.
    enum class StadiumStep : uint8
    {
        Done,
        Fire,
        Wait,
    };

    inline StadiumStep DecideStadiumStart(uint32 rendState)
    {
        namespace B = DcBlackrockSpire;
        if (rendState == B::STATE_IN_PROGRESS || rendState == B::STATE_DONE)
            return StadiumStep::Done;
        return rendState == B::STATE_NOT_STARTED ? StadiumStep::Fire : StadiumStep::Wait;
    }
}

#endif  // _PLAYERBOT_DCUBRSDECISION_H
