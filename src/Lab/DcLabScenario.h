/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCLABSCENARIO_H
#define _PLAYERBOT_DCLABSCENARIO_H

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "Lab/DcLabJson.h"
#include "TestRun/DcTestComp.h"

// A Pull Lab scenario (pull-lab plan §2.1): one file under lab/scenarios/,
// committed to the module repo, shared by the live Lab and the offline Sim.
// The id is the file's path under lab/scenarios/ without ".json".
//
// Engine-free: parsed and validated here so the gtest target pins the format.
//
//   {
//     "about": "...",
//     "geometry": { "map": 36, "dungeon": "deadmines", "heroic": false,
//                   "center": [x,y,z], "radius": 70, "objective": 639 },
//     "actors": { "native": { "packs": { "target": [spawnId...], "side": [...] },
//                             "keep": [spawnId...] },
//                 "synthetic": [ { "tag": "add1", "entry": 1708, "pos": [x,y,z,o],
//                                  "spawn": "onInject", "pack": "add" } ] },
//     "party": { "comp": "warrior-tank,priest,mage,rogue,hunter", "level": 20,
//                "start": [x,y,z,o], "react": "fast", "seedTarget": true,
//                "human": { "slot": 4, "script": "humans/hunter-pulls-ahead" } },
//     "settings": { "PullSetting": 2, "PullDynamicMaxLeeroyMobs": 5 },
//     "injections": [ { "when": { "phaseEnter": "Returning", "delayMs": 400 },
//                       "do": { "spawn": "add1", "aggro": "heal" } } ],
//     "goal": { "kill": "pack:target", "timeoutS": 90 },
//     "expect": { "oracles": { "O1": "pass", "O7": { "joinersMax": 1 } },
//                 "knownFailure": "open:#17" }
//   }
namespace DcLabScenario
{
    struct Vec4
    {
        float x = 0, y = 0, z = 0, o = 0;
        bool set = false;
    };

    struct Synthetic
    {
        std::string tag;
        std::uint32_t entry = 0;
        Vec4 pos;
        std::string spawn = "start";  // start | onInject
        std::string pack;             // defaults to the tag
    };

    // When an injection fires. Keyed on pipeline events, not wall clock (§2.3),
    // so a fault lands at the same point of the pull on every run.
    struct When
    {
        enum class Kind : std::uint8_t { AtMs, PhaseEnter, Event, Predicate };
        Kind kind = Kind::AtMs;
        std::uint32_t atMs = 0;
        std::uint32_t phase = 0;      // PhaseEnter: DcPullPhase value
        std::string event;            // Event: commit, aggroConfirmed, verdict:Leeroy, ...
        std::uint32_t delayMs = 0;    // after the phase/event
        std::string lhs;              // Predicate: tankDistToCamp, tankHp, ...
        std::string op;               // < <= > >=
        double rhs = 0;
        std::uint32_t occurrence = 1; // the Nth time the phase/event happens
    };

    // What it does: an op name plus its raw arguments (the live executor reads
    // them; Validate() checks the op is one it knows).
    struct Do
    {
        std::string op;
        DcLabJson::Value args;
    };

    struct Injection
    {
        When when;
        Do what;
        std::string label;  // for traces/heat-maps; derived when absent
        bool sweep = false; // the injection a `sweep=` run multiplies (default: the first)
    };

    struct Human
    {
        int slot = -1;        // party slot index driven by the puppet; -1 = none
        std::string script;   // lab/humans/<script>.json
        bool leads = true;    // the puppet leads the group (bots get it as master)
    };

    struct Scenario
    {
        std::string id;
        std::string about;
        std::string sourcePath;
        std::uint32_t schema = 1;

        // geometry
        std::uint32_t mapId = 0;
        std::string dungeon;          // registry token; "" = first row on mapId
        bool heroic = false;
        float cx = 0, cy = 0, cz = 0;
        float radius = 70.0f;
        std::uint32_t objective = 0;  // roster boss entry DC routes toward; 0 = DC's own pick
        bool hideAll = true;          // hide every hostile creature not kept (false: only within radius)

        // actors
        std::map<std::string, std::vector<std::uint64_t>> packs;  // tag -> spawnIds
        std::vector<Synthetic> synthetic;

        // party
        std::string comp;             // comp tokens; "" = seeded random comp
        std::uint32_t level = 0;      // 0 = the row's level
        std::uint32_t gearIlvl = 0;   // 0 = conf default
        Vec4 start;
        std::string react = "fast";   // fast | masterless
        bool seedTarget = false;      // pin the pull latch on the first target-pack member
        Human human;

        // settings
        std::vector<std::pair<std::string, double>> settings;  // DcSettings overrides
        int pullSetting = -1;         // -1 = leave; 0 off, 1 on, 2 dynamic

        std::vector<Injection> injections;

        // goal
        std::vector<std::string> goalPacks{"target"};
        std::uint32_t timeoutS = 90;

        // expectations
        DcLabJson::Value oracleExpect;                 // raw expect.oracles (thresholds + verdicts)
        std::map<std::string, std::string> expected;   // O7 -> "fail" (verdict expectations only)
        std::string knownFailure;                      // "" | "open:#17" | "fixed:<sha>"

        // A Sim-only key the Lab cannot honour, or vice versa, is reported here
        // rather than silently dropped.
        std::vector<std::string> warnings;

        // Which pack tag a spawnId belongs to ("" = not kept).
        std::string PackOf(std::uint64_t spawnId) const;
        bool Keeps(std::uint64_t spawnId) const { return !PackOf(spawnId).empty(); }
    };

    bool Parse(DcLabJson::Value const& root, std::string const& id, Scenario& out, std::string* err);
    bool LoadFile(std::string const& path, std::string const& id, Scenario& out, std::string* err);

    // Every *.json under `dir` (recursive), as (id, path), sorted by id.
    std::vector<std::pair<std::string, std::string>> List(std::string const& dir);

    // Glob over ids: '*' matches any run of characters (including '/').
    bool GlobMatch(std::string const& pattern, std::string const& id);

    // Party comp from tokens "class[-spec|-role]", comma separated. A bare class
    // takes the positional role (slot 0 tank, slot 1 heal, the rest dps). An
    // empty spec draws a seeded random comp (DcTestComp::BuildComp).
    bool ParseComp(std::string const& spec, std::uint32_t seed, DcTestComp::Roster roster,
                   std::vector<DcTestComp::Slot>& out, std::string* err);

    // Two scenarios can share a warm party when this key matches.
    std::string PartyKey(Scenario const& s);

    // Lab verdict for one run given its oracle results and the scenario's
    // expectations: "pass", "fail", "expected-fail" (known failure reproduced),
    // or "unexpected-pass" (a known failure no longer reproduces).
    std::string Judge(Scenario const& s, std::vector<std::pair<std::string, std::string>> const& oracleRes);

    bool PhaseFromName(std::string const& s, std::uint32_t& out);

    // One `when` object (also used by puppet scripts).
    bool ParseWhen(DcLabJson::Value const& w, When& out, std::string* err);

    // Sweep mode (§2.3): one copy of the scenario per point in the timing grid,
    // with the sweep injection's `when` replaced. mode "phases" = every holding
    // phase entry x delayMs {0,150,400,1000,2500}; "events" = every pull event
    // at +0; "all" = both (25 points). Returns (pointLabel, scenario) pairs, or
    // empty + *err when the scenario has no injection to sweep.
    std::vector<std::pair<std::string, Scenario>> SweepPoints(Scenario const& s, std::string const& mode,
                                                              std::string* err);

    // The grid axes, for the heat-map.
    std::vector<std::string> const& SweepPhases();
    std::vector<std::uint32_t> const& SweepDelays();
    std::vector<std::string> const& SweepEvents();
}

#endif  // _PLAYERBOT_DCLABSCENARIO_H
