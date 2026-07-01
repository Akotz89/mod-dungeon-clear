/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcPartyState.h"

#include "DungeonClearMath.h"
#include "DungeonClearTuning.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "AttackersValue.h"
#include "CellImpl.h"
#include "Config.h"
#include "Creature.h"
#include "CreatureGroups.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "ItemTemplate.h"
#include "LootMgr.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "InstanceScript.h"
#include "LootObjectStack.h"
#include "Map.h"
#include "ModelIgnoreFlags.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "Chat.h"
#include "ServerFacade.h"
#include "Timer.h"
#include "World.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/DcPullContext.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"
#include "Ai/Dungeon/DungeonClear/Util/NavmeshSnap.h"
#include "Ai/Dungeon/DungeonClear/Value/DungeonClearLiveBossValue.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"

float DcPartyState::RestMinHpPct(Player* bot)
{
    // Per-run override wins: the group set a health rest target for this run, and
    // DungeonClearNeedsEatTrigger makes bots actually eat up to it, so we use it
    // verbatim (no playerbots clamp — even a target above AlmostFullHealth is
    // reachable now). 0 means "inherit" and falls through.
    if (uint32 const target = DcSettings::GetUInt(bot, "RestHealthPct"))
        return static_cast<float>(target);

    // 90% is our "topped up enough to pull" ceiling. Clamp it to the level bots
    // actually eat back up to (AiPlayerbot.AlmostFullHealth, default 85) so the
    // gate never waits on HP a bot won't restore on its own.
    return std::min(90.0f, static_cast<float>(sPlayerbotAIConfig.almostFullHealth));
}
float DcPartyState::RestMinMpPct(Player* bot)
{
    // Per-run override wins; see RestMinHpPct. DungeonClearNeedsDrinkTrigger makes
    // bots drink up to this target, so it stays reachable above HighMana too.
    if (uint32 const target = DcSettings::GetUInt(bot, "RestManaPct"))
        return static_cast<float>(target);

    // 75% ceiling, clamped to the level bots actually drink back up to
    // (AiPlayerbot.HighMana, default 65). Bots stop drinking at HighMana, so a
    // higher gate would strand the tank waiting on slow natural mana regen.
    return std::min(75.0f, static_cast<float>(sPlayerbotAIConfig.highMana));
}
bool DcPartyState::IsPartyReady(Player* bot, float minHpPct, float minMpPct, float maxSpread,
    Position const* spreadAnchor, float maxTankGap)
{
    if (!bot)
        return false;
    Group* group = bot->GetGroup();
    if (!group)
        return true;  // Solo tank — always "ready."

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member)
            continue;
        if (member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId())
            continue;
        if (member->isDead())
            continue;  // Dead members handled by the party-died trigger.

        if (member != bot)
        {
            float const spread = spreadAnchor ? member->GetDistance(*spreadAnchor)
                : bot->GetDistance(member);
            if (spread > maxSpread)
                return false;
            // Absolute tank<->member backstop (camp-anchored mode only): a stale
            // camp sitting where the party stands passes the anchored spread
            // forever while the tank glides away — cap the real gap too.
            if (maxTankGap > 0.0f && bot->GetDistance(member) > maxTankGap)
                return false;
        }
        if (member->GetHealthPct() < minHpPct)
            return false;
        if (member->getPowerType() == POWER_MANA)
        {
            uint32 const maxMp = member->GetMaxPower(POWER_MANA);
            if (maxMp > 0)
            {
                float const mpPct = 100.0f * float(member->GetPower(POWER_MANA)) / float(maxMp);
                if (mpPct < minMpPct)
                    return false;
            }
        }
    }
    return true;
}
DcPartyState::SpreadGate DcPartyState::GetSpreadGate(Player* bot, AiObjectContext* context)
{
    // Through DcSettings (NOT raw sConfigMgr) so a per-run addon override of
    // PartyMaxSpread actually takes effect — the registry marks it
    // player-facing, and reading conf directly here silently ignored it.
    SpreadGate gate{ DcSettings::GetFloat(bot, "PartyMaxSpread"), nullptr };
    if (!context)
        return gate;

    DcPullContext const& pull =
        context->GetValue<DcPullContext&>(DcKey::PullContext)->Get();
    // Waive the spread requirement ONLY while a pull maneuver is actually
    // holding the party at camp — see the header comment for the full why.
    if (DcLeaderSignal::IsPullPhaseHolding(static_cast<uint32>(pull.phase)))
    {
        gate.maxSpread = 100000.0f;
        return gate;
    }
    // Pull mode between maneuvers (Idle): hold-at-camp still pins the party at
    // the camp, so "caught up" must be measured against the camp, not the tank —
    // otherwise a camp standoff at/over PartyMaxSpread deadlocks the run (see
    // the header comment on SpreadGate). (0,0,0) camp = unset, fall back.

    if (context->GetValue<bool>(DcKey::PullMode)->Get() && pull.HasCamp())


    {
        gate.anchor = &pull.camp;
        // Camp-anchored backstop: members set at a live camp are by construction
        // within PartyMaxSpread + the camp's own standoff behind the tank
        // (PullSetback normally, drag-extended up to PullMaxDrag for clearance /
        // LOS-break camps), so this cap never trips in healthy states. It only
        // bites when the camp has gone stale at the party's feet — the case
        // where the anchored spread alone passes forever while the tank glides
        // away (the scout-runaway gap).
        float const setback = DcSettings::GetFloat(bot, "PullSetback");
        float const maxDrag = DcSettings::GetFloat(bot, "PullMaxDrag");
        gate.maxTankGap = gate.maxSpread + std::max(setback, maxDrag);
    }
    return gate;
}
bool DcPartyState::IsBetweenPullsReady(Player* bot, AiObjectContext* context, bool requireNoLoot)
{
    if (!bot || !context)
        return false;
    if (requireNoLoot && context->GetValue<bool>(DcKey::Stock::HasAvailableLoot)->Get())
        return false;
    SpreadGate const gate = GetSpreadGate(bot, context);
    return IsPartyReady(bot, RestMinHpPct(bot), RestMinMpPct(bot), gate.maxSpread, gate.anchor,
        gate.maxTankGap);
}
bool DcPartyState::IsAnyPartyMemberLooting(Player* bot)
{
    if (!bot)
        return false;
    Group* group = bot->GetGroup();
    if (!group)
        return false;  // Solo tank — no followers to wait on.

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot)
            continue;
        if (!member->IsAlive())
            continue;
        if (member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId())
            continue;

        // Only bot members loot under our coordination; a real player has no
        // PlayerbotAI, so we can't read their loot intent and don't wait on it.
        PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
        if (!memberAI)
            continue;

        AiObjectContext* memberCtx = memberAI->GetAiObjectContext();
        if (memberCtx->GetValue<bool>(DcKey::Stock::CanLoot)->Get() ||
            memberCtx->GetValue<bool>(DcKey::Stock::HasAvailableLoot)->Get())
            return true;
    }
    return false;
}
std::string DcPartyState::DescribePartyNotReady(Player* bot,
    float minHpPct, float minMpPct,
    float maxSpread,
    Position const* spreadAnchor,
    float maxTankGap)
{
    if (!bot)
        return "";
    Group* group = bot->GetGroup();
    if (!group)
        return "";  // Solo tank — nobody to wait on.

    // Keep the addon line short: name a few members, then collapse the rest.
    constexpr size_t MAX_NAMED = 3;
    std::vector<std::string> parts;
    size_t extra = 0;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member)
            continue;
        if (member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId())
            continue;
        if (member->isDead())
            continue;  // Dead members handled by the party-died trigger.

        // Mirror IsPartyReady's checks, but record the limiting reason. Order
        // matters only for which single reason we surface first; distance reads
        // most intuitively, then health, then mana.
        std::string reason;
        if (member != bot &&
            ((spreadAnchor ? member->GetDistance(*spreadAnchor)
                : bot->GetDistance(member)) > maxSpread ||
                (maxTankGap > 0.0f && bot->GetDistance(member) > maxTankGap)))
            reason = "out of range";
        else if (member->GetHealthPct() < minHpPct)
            reason = "low HP";
        else if (member->getPowerType() == POWER_MANA)
        {
            uint32 const maxMp = member->GetMaxPower(POWER_MANA);
            if (maxMp > 0)
            {
                float const mpPct = 100.0f * float(member->GetPower(POWER_MANA)) / float(maxMp);
                if (mpPct < minMpPct)
                    reason = "low mana";
            }
        }

        if (reason.empty())
            continue;  // This member is ready — not blocking.

        if (parts.size() < MAX_NAMED)
            parts.push_back(member->GetName() + " (" + reason + ")");
        else
            ++extra;
    }

    if (parts.empty())
        return "";

    std::string out = "Waiting on ";
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i)
            out += ", ";
        out += parts[i];
    }
    if (extra)
        out += " +" + std::to_string(extra) + " more";
    return out;
}
std::string DcPartyState::DescribePartyLooting(Player* bot)
{
    if (!bot)
        return "";
    Group* group = bot->GetGroup();
    if (!group)
        return "";

    constexpr size_t MAX_NAMED = 1;
    std::vector<std::string> names;
    size_t extra = 0;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot)
            continue;
        if (!member->IsAlive())
            continue;
        if (member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId())
            continue;

        PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
        if (!memberAI)
            continue;  // Real player — we don't drive or wait on their loot.

        AiObjectContext* memberCtx = memberAI->GetAiObjectContext();
        if (!memberCtx->GetValue<bool>(DcKey::Stock::CanLoot)->Get() &&
            !memberCtx->GetValue<bool>(DcKey::Stock::HasAvailableLoot)->Get())
            continue;

        if (names.size() < MAX_NAMED)
            names.push_back(member->GetName());
        else
            ++extra;
    }

    if (names.empty())
        return "";

    std::string out;
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (i)
            out += ", ";
        out += names[i];
    }
    if (extra)
        out += " +" + std::to_string(extra) + " more";
    out += " looting";
    return out;
}
