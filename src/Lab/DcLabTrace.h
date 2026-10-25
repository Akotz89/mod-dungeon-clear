/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCLABTRACE_H
#define _PLAYERBOT_DCLABTRACE_H

#include <cstdint>
#include <string>
#include <vector>

// The Pull Lab trace (LabTrace JSONL): one record of what a pull actually did,
// emitted by the live Lab (and later the offline Sim), read by the oracles
// (DcLabOracles), replayed by tools/lab_trace.py and the Deck.
//
// Engine-free plain data. Times are ms since the trace was armed. GUIDs are raw
// 64-bit values (written as "0x..." strings in the JSONL).
//
// File layout — one flat object per line, `k` names the record kind:
//   hdr     run header (scenario, run id, seed, SHAs, settings snapshot)
//   member  one party member
//   unit    one scenario creature (registry: entry, spawnId, pack tag)
//   fu      unit sample          } one of each per unit/bot every frame
//   fb      bot sample           } (DcLab::kFrameMs), all sharing `t`
//   fd      DC pull-state sample }
//   ev      event (phase, verdict, passive, combat, dmg, death, inject, ...)
//   act     executed action (from the playerbots ActionExecutionListener)
//   end     end of run + reason
//   oracle  one oracle verdict (appended by scoring)
namespace DcLab
{
    inline constexpr std::uint32_t kTraceSchema = 1;
    inline constexpr std::uint32_t kFrameMs = 250;

    // DcPullPhase values (mirrored, so this header stays engine-free).
    enum class Phase : std::uint8_t { Idle = 0, Forming = 1, Advancing = 2, Returning = 3, Engage = 4 };
    char const* PhaseName(std::uint32_t p);
    bool PhaseFromName(std::string const& s, std::uint32_t& out);
    // DcPullDecisionCode values.
    char const* DecisionName(std::uint32_t d);

    // Bot engine (playerbots BotState): 0 combat, 1 non-combat, 2 dead.
    inline constexpr std::uint8_t kEngineNone = 255;  // no AI (a human)
    char const* EngineName(std::uint8_t e);

    struct Member
    {
        std::uint64_t guid = 0;
        std::string name;
        std::string role;   // tank | heal | dps
        std::string cls;    // class token
        bool human = false;
        bool leader = false;
        std::string react;  // fast | masterless
    };

    struct Setting
    {
        std::string key;
        double value = 0.0;
    };

    struct Header
    {
        std::uint32_t schema = kTraceSchema;
        std::string scenario;     // "" for a traced ordinary test run
        std::string runId;        // lr-... / tr-...
        std::string sweepPoint;   // "" or e.g. "phaseEnter=Returning+400"
        std::uint32_t seed = 0;
        std::string moduleSha;
        std::string playerbotsSha;
        std::uint32_t mapId = 0;
        std::uint32_t instanceId = 0;
        std::uint32_t pullSetting = 0;      // 0 off, 1 on, 2 dynamic
        std::uint32_t releaseDelayMs = 0;   // PullPlayerReleaseDelay
        std::vector<Setting> settings;      // the scenario's overrides
        std::vector<std::string> goalPacks; // pack tags whose death is the goal
        std::uint32_t timeoutMs = 0;
        std::vector<Member> party;
    };

    struct Unit
    {
        std::uint64_t guid = 0;
        std::uint32_t entry = 0;
        std::uint64_t spawnId = 0;   // 0 = summoned / synthetic
        std::string name;
        std::string pack;            // scenario pack tag; "" = outside any pack
        bool boss = false;
    };

    struct UnitSample
    {
        std::uint64_t guid = 0;
        float x = 0, y = 0, z = 0;
        std::uint8_t hpPct = 0;
        bool alive = false;
        bool inCombat = false;
        bool evading = false;
        bool moving = false;
        std::uint8_t motion = 0;     // MovementGeneratorType of the active slot
        std::uint64_t victim = 0;    // GetVictim()
        std::uint64_t threat = 0;    // threat-table top (current victim)
    };

    struct BotSample
    {
        std::uint64_t guid = 0;
        float x = 0, y = 0, z = 0;
        std::uint8_t hpPct = 0;
        bool alive = false;
        bool inCombat = false;
        std::uint8_t engine = kEngineNone;
        std::uint64_t victim = 0;    // GetVictim() — what it is auto-attacking
        std::uint64_t target = 0;    // AI "current target" (or selection for a human)
        bool passive = false;        // "passive" strategy in the combat engine
        bool stay = false;           // "stay" strategy (held healer)
        bool casting = false;
        std::uint64_t dmgDone = 0;   // cumulative damage dealt to non-party units
        std::uint64_t healDone = 0;  // cumulative healing done
    };

    struct DcSample
    {
        bool valid = false;          // the leader had a pull context this frame
        std::uint32_t phase = 0;
        std::uint32_t decision = 0;
        std::uint32_t decisionSeq = 0;
        std::uint32_t predicted = 0; // DcPullContext::predictedCount
        std::uint32_t ceiling = 0;   // DcPullContext::predictedCeiling
        float campX = 0, campY = 0, campZ = 0;
        std::uint64_t pullTarget = 0;
        std::uint64_t tagTarget = 0;
        std::uint64_t abortTarget = 0;
        bool partyReleased = false;
        bool losPull = false;
        bool scoutAggro = false;     // scoutAggroMs within its hold window
        bool enabled = false;        // DC run enabled on the leader
        bool paused = false;
    };

    struct Frame
    {
        std::uint32_t t = 0;
        std::vector<UnitSample> units;
        std::vector<BotSample> bots;
        DcSample dc;
    };

    // Event kinds (the `ev` field). Free-form strings so the Sim and future
    // hooks can add kinds without a schema bump; oracles only read these.
    namespace Ev
    {
        inline constexpr char const* Phase = "phase";          // a=leader, s=from>to, src in s2
        inline constexpr char const* Verdict = "verdict";      // a=leader, s=verdict
        inline constexpr char const* Passive = "passive";      // a=bot, v=1 on / 0 off
        inline constexpr char const* CombatOn = "combatOn";    // a=unit, b=victim
        inline constexpr char const* CombatOff = "combatOff";  // a=unit
        inline constexpr char const* FirstDmg = "firstDmg";    // a=attacker, b=victim, v=amount
        inline constexpr char const* Death = "death";          // a=unit, b=killer
        inline constexpr char const* Inject = "inject";        // s=description
        inline constexpr char const* Note = "note";            // s=free text
        // Derived from phase transitions (recorder), the names injections key on.
        inline constexpr char const* Commit = "commit";
        inline constexpr char const* AggroConfirmed = "aggroConfirmed";
        inline constexpr char const* CampReached = "campReached";
        inline constexpr char const* SafetyRelease = "safetyRelease";
    }

    struct Event
    {
        std::uint32_t t = 0;
        std::string ev;
        std::uint64_t a = 0;
        std::uint64_t b = 0;
        std::int64_t v = 0;
        std::string s;
        std::string s2;
    };

    struct ActionRec
    {
        std::uint32_t t = 0;
        std::uint64_t guid = 0;
        std::uint8_t engine = 0;
        std::string action;
        float relevance = 0.0f;
        bool executed = false;
        std::string source;  // event source ("" for a trigger-less default action)
    };

    // Oracle outcome (also serialised into the trace by scoring).
    enum class Verdict : std::uint8_t { Pass, Fail, NotApplicable };
    char const* VerdictName(Verdict v);

    struct OracleResult
    {
        std::string id;           // O1..O10
        std::string name;
        Verdict verdict = Verdict::NotApplicable;
        std::uint32_t firstMs = 0;     // first violation (Fail only)
        std::uint64_t unit = 0;        // offending unit
        std::uint32_t count = 0;       // violations / joiners / flips, per oracle
        std::string detail;
    };

    struct Trace
    {
        Header header;
        std::vector<Unit> units;
        std::vector<Frame> frames;
        std::vector<Event> events;
        std::vector<ActionRec> actions;
        std::uint32_t endMs = 0;
        std::string endReason;
        std::vector<OracleResult> oracles;

        Member const* FindMember(std::uint64_t guid) const;
        Unit const* FindUnit(std::uint64_t guid) const;
        Member const* Leader() const;
    };

    // Serialise the whole trace as JSONL (one line per record, '\n'-joined).
    std::string ToJsonl(Trace const& t);

    // Parse a JSONL trace. Unknown kinds/fields are ignored (forward
    // compatible); malformed lines are counted into *badLines.
    bool FromJsonl(std::string const& text, Trace& out, std::size_t* badLines = nullptr);
}

#endif  // _PLAYERBOT_DCLABTRACE_H
