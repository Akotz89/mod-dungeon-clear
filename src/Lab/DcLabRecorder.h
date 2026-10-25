/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCLABRECORDER_H
#define _PLAYERBOT_DCLABRECORDER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ObjectGuid.h"
#include "Lab/DcLabTrace.h"

class Creature;
class Engine;
class Player;
class PlayerbotAI;
class Unit;

// Live LabTrace recorder for one party (pull-lab plan §2.2, §4.2). Armed only
// for a Lab scenario or a `.dc test start ... trace=1` run, so a normal run
// pays nothing: every hook below first asks DcLabHub::Active().
//
// Three inputs:
//   * Frames, sampled by Tick() on the WORLD thread every kFrameMs: every
//     party member and every registered creature, plus the leader's pull state.
//     Combat/death edges and the derived pull events (commit, aggroConfirmed,
//     campReached, safetyRelease) are computed from consecutive frames.
//   * Hook events from MAP threads (damage, heal, creature engage, phase/
//     verdict/passive taps), routed here by DcLabHub and serialised by _mu.
//   * Executed actions, from one playerbots ActionExecutionListener per engine
//     per party bot (DcEngineAccess). Ownership: the engine's listener list
//     deletes every listener it holds when the engine dies, so on Disarm a
//     listener is removed and deleted by us ONLY when its PlayerbotAI and
//     Engine are provably the ones it was attached to; otherwise the engine
//     already deleted it. Listeners carry nothing but a guid and an engine
//     index, so one outliving the recorder (it cannot — see Disarm) would only
//     miss the hub lookup.
class DcLabRecorder
{
public:
    explicit DcLabRecorder(DcLab::Header header);
    ~DcLabRecorder();

    DcLabRecorder(DcLabRecorder const&) = delete;
    DcLabRecorder& operator=(DcLabRecorder const&) = delete;

    // --- setup (world thread, before Arm) -----------------------------------
    void AddMember(Player* p, std::string const& role, bool human, bool leader, std::string const& react);
    // Register a creature, with its scenario pack tag ("" = outside any pack).
    // Callable after Arm too (summons, discovered joiners).
    void AddUnit(Creature* c, std::string const& pack);
    // Creatures within `radius` of the leader also get auto-registered every
    // frame (pack ""), so an add from outside the scenario is still sampled.
    // 0 disables discovery.
    void SetDiscoveryRadius(float r) { _discoverRadius = r; }
    // Pack tag for a creature discovery finds. A kept spawn that respawned
    // after registration (a new GUID) must keep its scenario pack, or the goal
    // and O7 lose it (lb-20261003-155828-1: 8 of 24 runs).
    void SetPackResolver(std::function<std::string(Creature*)> f) { _packOf = std::move(f); }

    // --- lifetime ------------------------------------------------------------
    void Arm();     // register with the hub, attach listeners, map pull contexts
    void Disarm();  // the reverse; idempotent. End() calls it.
    bool Armed() const { return _armed; }

    // World thread, every world tick while armed.
    void Tick(std::uint32_t diff);

    // Close the trace: final frame, `end` record, Disarm.
    void End(std::string const& reason);

    // Thread-safe event append (t = now since arm).
    void AddEvent(char const* ev, std::uint64_t a = 0, std::uint64_t b = 0, std::int64_t v = 0,
                  std::string const& s = "", std::string const& s2 = "");

    std::uint32_t NowMs() const;
    DcLab::Header& HeaderRef() { return _trace.header; }
    // Only once End() has run (the map threads are detached by then).
    DcLab::Trace& TraceRef() { return _trace; }
    // World thread: units and frames are written only there, so reading them
    // while armed is safe (hooks append events/actions under _mu, not these).
    DcLab::Trace const& View() const { return _trace; }
    DcLab::Frame const* LastFrame() const { return _trace.frames.empty() ? nullptr : &_trace.frames.back(); }

    // Pull events seen since the last call (world thread) — what the Lab's
    // injection scheduler keys on: names from DcLab::Ev, plus "phase:<Name>".
    std::vector<std::pair<std::uint32_t, std::string>> DrainPullEvents();

    // Write the trace (JSONL) to `dir/<runId>.jsonl`; returns the path or "".
    std::string WriteFile(std::string const& dir) const;

    // --- hub callbacks (any thread) -------------------------------------------
    void OnDamage(std::uint64_t attacker, std::uint64_t victim, std::uint32_t amount);
    void OnHeal(std::uint64_t healer, std::uint32_t amount);
    void OnEngage(std::uint64_t creature, std::uint64_t victim);
    void OnPhase(std::uint64_t leader, std::uint32_t from, std::uint32_t to, std::uint32_t nowMs);
    void OnVerdict(std::uint64_t leader, char const* verdict, std::uint32_t predicted, std::uint32_t ceiling);
    void OnPassive(std::uint64_t guid, bool on);
    void OnAction(std::uint64_t guid, std::uint8_t engine, std::string action, float relevance, bool executed,
                  std::string source);

    bool IsParty(std::uint64_t g) const;

private:
    struct ListenerRec
    {
        std::uint64_t guid = 0;
        PlayerbotAI* ai = nullptr;
        Engine* engine = nullptr;
        void* listener = nullptr;  // DcLabActionListener*
    };

    void SampleFrame();
    void Discover(Player* leader);
    Player* FindLeader() const;
    void PushPullEvent(std::uint32_t t, std::string const& ev);

    mutable std::mutex _mu;  // everything below that hooks touch
    DcLab::Trace _trace;
    std::uint32_t _armMs = 0;
    bool _armed = false;
    bool _ended = false;
    std::uint32_t _frameAccum = 0;
    float _discoverRadius = 60.0f;
    std::function<std::string(Creature*)> _packOf;

    std::uint64_t _leader = 0;
    std::set<std::uint64_t> _party;
    std::set<std::uint64_t> _units;
    std::unordered_map<std::uint64_t, std::uint64_t> _dmgDone;
    std::unordered_map<std::uint64_t, std::uint64_t> _healDone;
    std::set<std::pair<std::uint64_t, std::uint64_t>> _firstDmg;
    std::set<std::uint64_t> _engaged;
    std::unordered_map<std::uint64_t, std::string> _lastVerdict;

    // Edge state from the previous frame.
    std::unordered_map<std::uint64_t, bool> _lastCombat;
    std::unordered_map<std::uint64_t, bool> _lastAlive;
    std::uint32_t _lastPhase = 0;
    bool _havePhase = false;
    bool _lastReleased = false;

    std::vector<std::pair<std::uint32_t, std::string>> _pullEvents;
    std::vector<ListenerRec> _listeners;
    std::vector<void const*> _contexts;
};

// Process-wide routing from hooks (map threads) to the recorder that owns a
// guid / pull context. Shared-locked for the whole callback so a recorder
// cannot be unregistered (and freed) underneath a hook in flight.
namespace DcLabHub
{
    bool Active();
    void Register(DcLabRecorder* r, std::vector<std::uint64_t> const& guids,
                  std::vector<void const*> const& contexts);
    void AddGuid(DcLabRecorder* r, std::uint64_t guid);
    void Unregister(DcLabRecorder* r);

    // Hook entry points (each is a no-op unless a recorder is armed).
    void Damage(Unit* attacker, Unit* victim, std::uint32_t amount);
    void Heal(Unit* healer, Unit* target, std::uint32_t amount);
    void Engage(Unit* unit, Unit* victim);
    void Action(std::uint64_t guid, std::uint8_t engine, std::string const& action, float relevance,
                bool executed, std::string const& source);

    // Short git description of the module / playerbots checkouts ("" if the
    // source tree is not on this machine). Read once, cached.
    std::string ModuleSha();
    std::string PlayerbotsSha();
}

#endif  // _PLAYERBOT_DCLABRECORDER_H
