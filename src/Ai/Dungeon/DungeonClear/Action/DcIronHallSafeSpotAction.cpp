/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Log.h"

#include <cstdint>

#include "Creature.h"
#include "MoveSpline.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "Timer.h"

#include "Ai/Dungeon/DungeonClear/Action/DungeonClearActions.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/DcRunState.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Trigger/DungeonClearTriggers.h"
#include "Ai/Dungeon/DungeonClear/Util/DcIronHallDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcThrottle.h"

// Blackrock Depths' Iron Hall — the PER-BOT half of "walk through the fire, never
// stop in it". The geometry and the rule are DcIronHall (Util/DcIronHallDecision.h);
// this is the glue that feeds it a live bot and walks the bot to the answer.
//
// WHY A RUNG AND NOT A WORLD TICK. Every mover that parks a bot in a circle — the
// follow, the regroup, the heal reposition, stock MoveChase / reach — is an
// action in one of the two engines, so the only way to keep it from undoing the
// hop is to outrank it there. And it is registered in BOTH engines with no
// MayDrive gate: Magmus's fight is the combat engine, the walk is the
// non-combat one, and a bot combat-flagged with no attackers (the DC blind
// window) idles on the non-combat engine with every driving rung off. Under
// mod-individual-progression's statue data the fire itself used to produce
// exactly that bot — DungeonClearIronHallGuardianScript now restores the stock
// CANNOT_ENTER_COMBAT flag — but a blind window from any other cause must still
// not leave a bot standing in a circle.
namespace
{
    namespace B = DcBlackrockDepths;

    // A hop is done inside this of its slot. Under the 1yd margin every slot keeps
    // over the fire test, so arriving can never read "in the fire" again.
    constexpr float HOP_ARRIVE = 1.0f;
    // A hop that has not arrived by now is abandoned and re-decided. The longest
    // hop is one band, ~45yd on the centre line: six seconds at run speed with
    // slack.
    constexpr uint32 HOP_MAX_MS = 8000;
    // A bot has to have stood in the fire this long before it hops: a leader's
    // glide pauses for a beat between route segments, and hopping on that beat
    // would turn the walk into a stutter. Half a second of fire is one tick.
    constexpr uint32 STILL_GRACE_MS = 500;

    struct HopView
    {
        bool travel = false;        // there is a hop to walk (new or in flight)
        bool fresh = false;         // ...decided this evaluation
        DcIronHall::Verdict verdict{};
        float x = 0.0f, y = 0.0f;
    };

    void ClearHop(DcRunState& st)
    {
        st.ironHallHopMs = 0;
        st.ironHallStillMs = 0;
    }

    // Resolves the rung and STORES THE LATCH, so the trigger and the action agree
    // about where the bot is going (the HoR stay-ahead's rule).
    HopView Probe(Player* bot, PlayerbotAI* botAI)
    {
        HopView v;
        if (!bot || !botAI || bot->isDead() || bot->GetMapId() != B::MAP_ID)
            return v;

        DcRunState& st = DcRun::Of(botAI);
        if (!DcIronHall::InHall(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()))
        {
            ClearHop(st);
            return v;
        }

        // Members of a running clear only; a paused or finished run hands the
        // bot back to its player.
        Player* tank = botAI->GetAiObjectContext()->GetValue<Player*>(DcKey::PartyTank)->Get();
        if (!tank)
        {
            ClearHop(st);
            return v;
        }

        // A hop in flight keeps the tick until it lands.
        if (st.ironHallHopMs)
        {
            if (bot->GetExactDist2d(st.ironHallHopX, st.ironHallHopY) > HOP_ARRIVE &&
                GetMSTimeDiffToNow(st.ironHallHopMs) < HOP_MAX_MS)
            {
                v.travel = true;
                v.x = st.ironHallHopX;
                v.y = st.ironHallHopY;
                return v;
            }
            ClearHop(st);
        }

        DcIronHall::Inputs in;
        in.x = bot->GetPositionX();
        in.y = bot->GetPositionY();
        in.moving = bot->isMoving();
        if (in.moving && bot->movespline && !bot->movespline->Finalized())
        {
            G3D::Vector3 const end = bot->movespline->FinalDestination();
            in.hasDest = true;
            in.destX = end.x;
            in.destY = end.y;
        }

        in.isLeader = tank == bot || DcLeaderSignal::IsDungeonClearLeader(bot);
        if (in.isLeader)
        {
            // The way on: Magmus while he stands, then the throne door.
            if (Creature* magmus = bot->FindNearestCreature(B::NPC_MAGMUS, 250.0f, true))
            {
                in.goalX = magmus->GetPositionX();
                in.goalY = magmus->GetPositionY();
            }
            else
            {
                in.goalX = B::IRON_HALL_ZONES[B::IRON_HALL_ZONE_COUNT - 1].x;
                in.goalY = B::IRON_HALL_HOP_MIN_Y;
            }
        }
        else
        {
            in.goalX = tank->GetPositionX();
            in.goalY = tank->GetPositionY();
        }

        Unit* victim = bot->GetVictim();
        if (bot->IsInCombat() && victim && victim->IsAlive())
        {
            in.hasTarget = true;
            in.targetX = victim->GetPositionX();
            in.targetY = victim->GetPositionY();
            in.reach = PlayerbotAI::IsRanged(bot)
                           ? sPlayerbotAIConfig.SpellDistance - 2.0f
                           : bot->GetMeleeRange(victim) - 0.5f;
        }
        in.slotSeed = static_cast<uint32>(bot->GetGUID().GetCounter());

        DcIronHall::Verdict const verdict = DcIronHall::Decide(in);
        if (!verdict.hop)
        {
            st.ironHallStillMs = 0;
            return v;
        }

        // Stopped in the fire: hop once it has been still for the grace beat. A
        // redirect (a spline ending in the fire) needs no grace — it is moving.
        if (!verdict.redirect)
        {
            if (!st.ironHallStillMs)
                st.ironHallStillMs = getMSTime();
            if (GetMSTimeDiffToNow(st.ironHallStillMs) < STILL_GRACE_MS)
                return v;
        }

        st.ironHallStillMs = 0;
        st.ironHallHopX = verdict.spot.x;
        st.ironHallHopY = verdict.spot.y;
        st.ironHallHopMs = getMSTime();
        v.travel = true;
        v.fresh = true;
        v.verdict = verdict;
        v.x = verdict.spot.x;
        v.y = verdict.spot.y;
        return v;
    }
}

bool DungeonClearIronHallSafeSpotTrigger::IsActive()
{
    // Map first: registered in both engines on every bot, and everywhere outside
    // Blackrock Depths it must cost one integer compare.
    if (!bot || bot->isDead() || bot->GetMapId() != B::MAP_ID)
        return false;

    if (bot->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_FLEEING |
                          UNIT_STATE_CONFUSED | UNIT_STATE_ROOT))
        return false;

    return Probe(bot, botAI).travel;
}

bool DungeonClearIronHallSafeSpotAction::Execute(Event /*event*/)
{
    if (!bot || !botAI)
        return false;

    HopView const v = Probe(bot, botAI);
    if (!v.travel)
        return false;  // raced clear between the trigger and here

    // Re-issue floor on the slot, and the tick is CLAIMED while it suppresses:
    // handing a suppressed tick to the engine is how MoveChase gets to cancel the
    // hop halfway and walk the bot back to a target standing at the fire's edge
    // (the HoR stay-ahead's tp-20260907-232556-1 lesson).
    DcRunState& st = DcRun::Of(botAI);
    if (st.ThrottledIssue(DcThrottle::IronHallHopIssue, v.x, v.y, B::IRON_HALL_FLOOR_Z,
                          /*epsilon*/ 1.0f, /*windowMs*/ 1000) &&
        bot->isMoving())
        return true;

    if (!DcMoveTo(bot->GetMapId(), v.x, v.y, B::IRON_HALL_FLOOR_Z, /*idle*/ false,
                  /*react*/ false, /*normal_only*/ false, /*exact_waypoint*/ false,
                  MovementPriority::MOVEMENT_COMBAT))
    {
        st.ironHallHopMs = 0;
        return false;
    }

    if (v.fresh)
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] Iron Hall — {} the fire ({:.1f}yd from the nearest statue), hopping to "
                  "stop {} at ({:.1f}, {:.1f}){}",
                  bot->GetName(), v.verdict.redirect ? "heading to a stop in" : "standing in",
                  DcIronHall::FireDistance(bot->GetPositionX(), bot->GetPositionY()), v.verdict.zone,
                  v.x, v.y, v.verdict.inReach ? " — in reach of its target" : "");
    return true;
}

// The follow-tank clamp's live half (see DcIronHall::HoldFollow): true while this
// follower should stay where the hop put it rather than follow its tank.
bool DcIronHallHoldsFollow(Player* bot, PlayerbotAI* botAI, Player* tank)
{
    if (!bot || !botAI || !tank || bot == tank || bot->GetMapId() != B::MAP_ID)
        return false;
    if (!DcIronHall::InHall(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()))
        return false;
    // A hop in flight: the follow must not turn it round.
    if (DcRun::Of(botAI).ironHallHopMs)
        return true;
    return DcIronHall::HoldFollow(bot->GetPositionX(), bot->GetPositionY(),
                                  tank->GetPositionX(), tank->GetPositionY());
}
