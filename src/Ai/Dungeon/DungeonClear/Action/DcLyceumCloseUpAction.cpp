/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Log.h"

#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Timer.h"

#include "Ai/Dungeon/DungeonClear/Action/DungeonClearActions.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/DcRunState.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Trigger/DungeonClearTriggers.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLyceumCloseUpDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcThrottle.h"

// Blackrock Depths' Lyceum — the PER-BOT half of "fight inside 15yd of the tank".
// The rule is DcLyceumCloseUp (Util/DcLyceumCloseUpDecision.h); this feeds it a
// live bot and walks the bot to the answer.
//
// A RUNG, NOT A WORLD TICK, for the Iron Hall's reason: what holds a bot out at
// 25-30yd — stock reach / MoveChase, the reach-heal, the heal reposition — is an
// action in the combat engine, so the walk back has to outrank it there.
namespace
{
    struct CloseUpView
    {
        DcLyceumCloseUp::Verdict verdict{};
        bool fresh = false;   // the walk began this evaluation
        float dist = 0.0f;
        float z = 0.0f;
    };

    bool InRoom(WorldObject const* o)
    {
        return DcLyceumCloseUp::InRoom(o->GetPositionX(), o->GetPositionY(), o->GetPositionZ());
    }

    // Resolves the rung and STORES THE LATCH, so the trigger and the action agree.
    CloseUpView Probe(Player* bot, PlayerbotAI* botAI)
    {
        CloseUpView v;
        if (!bot || !botAI || bot->isDead() || bot->GetMapId() != DcBlackrockDepths::MAP_ID)
            return v;

        DcRunState& st = DcRun::Of(botAI);
        // A running clear only (a paused or finished one hands the bot back), a
        // member that is not the tank, both in the room, both in this instance.
        Player* tank = botAI->GetAiObjectContext()->GetValue<Player*>(DcKey::PartyTank)->Get();
        if (!bot->IsInCombat() || !tank || tank == bot || !tank->IsAlive() ||
            tank->GetInstanceId() != bot->GetInstanceId() || DcLeaderSignal::IsDungeonClearLeader(bot) ||
            !InRoom(bot) || !InRoom(tank))
        {
            st.lyceumCloseUpMs = 0;
            return v;
        }

        v.verdict = DcLyceumCloseUp::Decide(bot->GetPositionX(), bot->GetPositionY(), tank->GetPositionX(),
                                            tank->GetPositionY(), st.lyceumCloseUpMs != 0);
        if (!v.verdict.close)
        {
            st.lyceumCloseUpMs = 0;
            return v;
        }
        v.fresh = st.lyceumCloseUpMs == 0;
        if (v.fresh)
            st.lyceumCloseUpMs = getMSTime() ? getMSTime() : 1u;
        v.dist = bot->GetExactDist2d(tank);
        v.z = tank->GetPositionZ();
        return v;
    }
}

bool DungeonClearLyceumCloseUpTrigger::IsActive()
{
    // Map first: registered on every bot, and everywhere outside Blackrock Depths
    // it must cost one integer compare.
    if (!bot || bot->isDead() || bot->GetMapId() != DcBlackrockDepths::MAP_ID)
        return false;

    if (bot->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_FLEEING |
                          UNIT_STATE_CONFUSED | UNIT_STATE_ROOT))
        return false;

    return Probe(bot, botAI).verdict.close;
}

bool DungeonClearLyceumCloseUpAction::Execute(Event /*event*/)
{
    if (!bot || !botAI)
        return false;

    CloseUpView const v = Probe(bot, botAI);
    if (!v.verdict.close)
        return false;  // raced clear between the trigger and here

    // The tank moves, so the aim point moves: re-issue past 2yd of drift, and
    // CLAIM the tick while the walk is in flight — handing it to the engine is
    // how stock MoveChase turns the bot round halfway.
    DcRunState& st = DcRun::Of(botAI);
    if (st.ThrottledIssue(DcThrottle::LyceumCloseUpIssue, v.verdict.x, v.verdict.y, v.z,
                          /*epsilon*/ 2.0f, /*windowMs*/ 500) &&
        bot->isMoving())
        return true;

    if (!DcMoveTo(bot->GetMapId(), v.verdict.x, v.verdict.y, v.z, /*idle*/ false,
                  /*react*/ false, /*normal_only*/ false, /*exact_waypoint*/ false,
                  MovementPriority::MOVEMENT_COMBAT))
    {
        st.lyceumCloseUpMs = 0;
        return false;
    }

    if (v.fresh)
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] Lyceum — {:.1f}yd from the tank in a fight (leash {:.0f}yd), closing up to "
                  "({:.1f}, {:.1f})",
                  bot->GetName(), v.dist, DcLyceumCloseUp::LEASH, v.verdict.x, v.verdict.y);
    return true;
}
