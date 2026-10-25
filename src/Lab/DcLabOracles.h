/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCLABORACLES_H
#define _PLAYERBOT_DCLABORACLES_H

#include <cstdint>
#include <string>
#include <vector>

#include "Lab/DcLabTrace.h"

namespace DcLabJson { struct Value; }

// The ten Pull Lab oracles (plan §2.4): automatic pass/fail over a LabTrace.
// Pure — reads only the trace, so the same evaluator scores a live Lab run, an
// offline Sim run and a canned gtest trace.
//
//   O1  aggro ownership       a mob on the party nobody is fighting or closing on
//   O2  premature DPS         a follower hits a pull mob before the tank + release delay
//   O3  held member hurt      a passive/held follower loses too much health
//   O4  idle while tank fights
//   O5  tick starvation       one action "runs" for seconds and nothing changes
//   O6  phase dwell           a pull phase outlives its budget
//   O7  joiners               engaged mobs beyond the governor's prediction; outside-pack adds
//   O8  oscillation           verdict flips / movement direction reversals
//   O9  human interference
//   O10 outcome               goal pack dead, nobody died, no timeout
//
// Every threshold lives in OracleConfig with the plan's defaults; a scenario's
// `expect.oracles` block overrides them by name (ApplyOverrides).
namespace DcLabOracles
{
    struct OracleConfig
    {
        // O1
        std::uint32_t o1UnattendedMs = 3000;
        std::uint32_t o1ParkedRangedMs = 6000;
        float o1TankNearYd = 8.0f;
        float o1ParkedRangeYd = 8.0f;
        // O2
        std::uint32_t o2ToleranceMs = 250;
        bool o2SelfDefenseExempt = true;
        // Off by default: a follower setting its victim is usually it starting
        // to run in (lb-20261003-154632-1: victim at +0.2s, 23yd out, first hit
        // 2.6s later). Damage is what takes threat; this flag adds target-picks.
        bool o2CountAttackStart = false;
        // O3
        float o3HpDropPct = 30.0f;
        // O4
        std::uint32_t o4TankFightMs = 4000;
        std::uint32_t o4IdleMs = 6000;
        // O5
        std::uint32_t o5StreakMs = 8000;
        float o5MoveYd = 1.0f;
        std::vector<std::string> o5Exempt = {"hold at camp", "stay at camp", "drink", "eat",
                                             "food", "rest", "mount", "release", "loot"};
        // O6
        std::uint32_t o6FormingMs = 9000;
        std::uint32_t o6AdvancingMs = 20000;
        std::uint32_t o6ReturningMs = 25000;
        std::uint32_t o6IdleCombatMs = 1500;
        std::string maneuverAction = "dungeon clear pull maneuver";
        // O7
        std::int32_t o7JoinersMax = 1;
        std::int32_t o7OutsideMax = 0;   // <0 = report only
        float o7CallForHelpYd = 12.0f;
        float o7ProximityYd = 30.0f;
        // O8
        std::uint32_t o8WindowMs = 10000;
        std::uint32_t o8MaxFlips = 3;
        std::uint32_t o8MaxReversals = 8;  // in-combat caster repositioning reverses often
        float o8StepYd = 1.0f;
        // O9
        std::uint32_t o9WaitMs = 15000;
        std::uint32_t o9HumanFightMs = 4000;
        float o9TowardYd = 5.0f;
        // O10
        bool o10RequireGoal = true;
        bool o10NoDeaths = true;
    };

    // Apply a scenario's `expect.oracles` object: {"O7": {"joinersMax": 2}, ...}.
    // Unknown keys are reported in *unknown (comma list) and otherwise ignored.
    void ApplyOverrides(OracleConfig& cfg, DcLabJson::Value const& oracles, std::string* unknown = nullptr);

    // Evaluate all ten. Always returns O1..O10 in order.
    std::vector<DcLab::OracleResult> Evaluate(DcLab::Trace const& trace, OracleConfig const& cfg);

    // "O1:pass O2:fail@12.5s ..." one-liner for logs and chat.
    std::string Summary(std::vector<DcLab::OracleResult> const& results);
}

#endif  // _PLAYERBOT_DCLABORACLES_H
