/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCPACKLEASHDECISION_H
#define _PLAYERBOT_DCPACKLEASHDECISION_H

#include <cstdint>

#include "Define.h"

// PURE kernel for an event's PACK LEASH (DungeonEvent::packLeash): may the leader
// take its next step, or does it stand and let the party close up first?
//
// The shape it serves is a room that refills behind the party — Blackrock Depths'
// Lyceum, 232 Anvilrage Reservists on a 60s respawn. There the run is "walk to a
// destination as one body, then kill everything that came with you". A leader
// that walks on between fights while the healer is still 40yd back drinking, or a
// hunter is still looting, drags a column across fresh spawns: every member
// sweeps their own aggro cylinder, and the adds land on whoever is furthest from
// the tank. Live (tr-20261018-190617-15), almost every add that hit someone more
// than 30yd from the tank hit the healer, the tank ran after them, and the run
// stalled 100yd from its brazier.
//
// So, between fights, the leader HOLDS while any living member is outside the
// leash. It never walks back — the followers' own follow does the closing — and
// it never holds in combat (the caller is the non-combat objective action).
//
// EVERY HOLD IS BOUNDED, and the release is a DUTY CYCLE, not a latch: after
// `holdBudgetMs` the leader gets `walkWindowMs` of progress, then the leash
// re-arms with a fresh budget. Same asymmetry as DcSuppressionTransit's
// PackTrailing hold: walking is what makes a straggler hold worse, so a latched
// release would march the tank to the brazier alone. One wedged member costs the
// room seconds per cycle, never the run; stranded recovery owns that member.
namespace DcPackLeash
{
    struct Inputs
    {
        uint32 outside = 0;        // living followers further than the leash from the leader
        uint32 nowMs = 0;
        uint32 holdSinceMs = 0;    // stored: getMSTime() the current hold began (0 = not holding)
        uint32 releasedAtMs = 0;   // stored: getMSTime() the watchdog last released (0 = none)
        uint32 holdBudgetMs = 0;   // 0 = unbounded, which no caller should pass
        uint32 walkWindowMs = 0;
    };

    struct Verdict
    {
        bool   hold = false;       // stand still this tick
        bool   timedOut = false;   // the watchdog released THIS tick
        uint32 holdSinceMs = 0;    // to store back
        uint32 releasedAtMs = 0;   // to store back
    };

    // Wrap-safe: unsigned subtraction of an interval survives getMSTime()'s wrap.
    inline bool Elapsed(uint32 nowMs, uint32 sinceMs, uint32 budgetMs)
    {
        return sinceMs != 0 && static_cast<uint32>(nowMs - sinceMs) >= budgetMs;
    }

    inline Verdict Decide(Inputs const& in)
    {
        Verdict v;

        // Formed up: walk, and forget every clock — the next straggle gets a
        // full budget.
        if (in.outside == 0)
            return v;

        // Inside a watchdog's walk window: keep walking, keep the window.
        if (in.releasedAtMs && !Elapsed(in.nowMs, in.releasedAtMs, in.walkWindowMs))
        {
            v.releasedAtMs = in.releasedAtMs;
            return v;
        }

        uint32 const since = in.holdSinceMs ? in.holdSinceMs : in.nowMs;
        if (in.holdBudgetMs && Elapsed(in.nowMs, since, in.holdBudgetMs))
        {
            v.timedOut = true;
            v.releasedAtMs = in.nowMs ? in.nowMs : 1u;  // 0 means "none"
            return v;
        }

        v.hold = true;
        v.holdSinceMs = since ? since : 1u;
        return v;
    }
}

#endif  // _PLAYERBOT_DCPACKLEASHDECISION_H
