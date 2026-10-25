/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCLABJOB_H
#define _PLAYERBOT_DCLABJOB_H

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ObjectGuid.h"
#include "Lab/DcLabManager.h"
#include "TestRun/DcTestRunJob.h"

class Creature;
class DcLabInjector;
class DcLabPuppet;
class DcLabRecorder;
class Map;
class Player;

// One warm Lab party (pull-lab plan §4.1). The hosting DcTestRunJob spawns,
// provisions, groups and teleports the party into a fresh instance; from then
// on this driver runs scenario after scenario in that same instance:
//
//   Staging  — `dc off`, the party healed and placed at party.start, every
//              hostile creature that is not kept hidden (phased out + passive:
//              no DB write, reversible), kept spawns revived/reset at home,
//              last run's synthetic actors removed.
//   Settling — a short beat for teleports and resets to land, combat to drop.
//   Starting — settings overrides, pull setting, `dc on`, other bosses skipped
//              so DC routes to the scenario's objective, trace armed.
//   Running  — trace ticks, injections fire, goal/wipe/timeout watched.
//   Scoring  — oracles, verdict, dc_labruns.jsonl, trace file.
//
// then asks the manager for the next run with the same party key. A run is
// never retried in place: a party that dies under a run hands it back once.
class DcLabJob : public DcTestRunJob::LabDriver, public std::enable_shared_from_this<DcLabJob>
{
public:
    explicit DcLabJob(std::string partyKey);
    ~DcLabJob() override;

    bool Tick(DcTestRunJob::LabParty const& party, std::uint32_t diff) override;
    void OnTeardown(DcTestRunJob::LabParty const& party, std::string const& reason) override;
    bool KeepGmMaster() const override;
    std::string Status() const override;

    bool Finished() const { return _finished; }
    std::string const& PartyKey() const { return _key; }
    std::string const& CurrentRunId() const;

    // Pre-assign the first run (the manager launches a party for a run).
    void Prime(DcLabManager::LabRun run) { _pending = std::move(run); }
    // Hand an unstarted primed run back (a refused launch).
    std::optional<DcLabManager::LabRun> TakePending()
    {
        std::optional<DcLabManager::LabRun> r = std::move(_pending);
        _pending.reset();
        return r;
    }

    // Lab hide rule for creatures that enter the world after staging (grids
    // loading as the party moves): called from the AllCreatureScript hook on a
    // map thread. True when `c` belongs to an active Lab instance and is not
    // kept — it has then been hidden.
    static bool HideOnAdd(Creature* c);
    // RAII: the Lab is summoning — the add-world hide rule stands aside.
    struct SummonScope
    {
        SummonScope();
        ~SummonScope();
    };

    // Used by the injector/puppet.
    DcLabRecorder* Recorder() { return _recorder.get(); }
    DcLabManager::LabRun const* Run() const { return _run ? &*_run : nullptr; }
    Player* Member(std::size_t slot) const;
    Player* Tank() const { return Member(0); }
    std::vector<ObjectGuid> const& Members() const { return _members; }
    std::vector<char const*> const& Roles() const { return _roles; }
    ObjectGuid Gm() const { return _gm; }
    std::uint32_t RunElapsedMs() const { return _stageMs; }
    // Synthetic actor spawned this run (tag -> guid).
    std::map<std::string, ObjectGuid>& Synthetic() { return _synthetic; }
    void TrackSummon(ObjectGuid g) { _summons.insert(g); }
    void TrackGo(ObjectGuid g) { _goSummons.insert(g); }
    DcLabPuppet* Puppet() { return _puppet.get(); }
    // `setReact`: fast (GM/human master, 100ms think) or masterless (no
    // master: the slow 1-3s out-of-combat think) for the named bots.
    void SetReactFor(std::vector<ObjectGuid> const& bots, std::string const& profile);

private:
    enum class Stage : std::uint8_t { Idle, Staging, Settling, Starting, Running, Scoring };
    static char const* StageName(Stage s);

    void Enter(Stage s);
    bool BeginRun();
    void TickStaging();
    void TickSettling();
    void TickStarting();
    void TickRunning(std::uint32_t diff);
    void Score(std::string const& end);
    void Abandon(std::string const& why);

    void ResetParty();
    void StageCreatures(Map* map, Player* tank);
    void Hide(Creature* c);
    // A kept spawn left fighting, evading, hurt or off home by the last run
    // goes back to a clean start. True when it needed it.
    bool ResetKept(Creature* c);
    // Hide hostile script summons / vehicle passengers that came with the
    // instance rather than the scenario (no spawnId, so the spawn store and
    // the add-world rule both miss them).
    void HideStraySummons(Map* map, Player* tank);
    void Unhide(Creature* c);
    void ApplySkips(Player* tank);
    void SpawnSynthetic(std::string const& when);
    bool GoalReached() const;
    bool PartyWiped() const;
    void ApplyReactProfile(std::string const& profile);
    void ReassertMasters();
    bool AttachPuppet();

    std::string _key;
    bool _finished = false;
    Stage _stage = Stage::Idle;
    std::uint32_t _stageMs = 0;
    std::uint32_t _skipAccum = 0;

    // Party (refreshed from the host every tick).
    ObjectGuid _gm;
    std::vector<ObjectGuid> _members;
    std::vector<char const*> _roles;
    std::uint32_t _mapId = 0;
    std::uint32_t _instanceId = 0;
    std::string _hostRunId;

    std::optional<DcLabManager::LabRun> _pending;
    std::optional<DcLabManager::LabRun> _run;
    std::unique_ptr<DcLabRecorder> _recorder;
    std::unique_ptr<DcLabInjector> _injector;
    std::unique_ptr<DcLabPuppet> _puppet;
    std::set<ObjectGuid> _masterless;
    bool _prepared = false;  // per-run Starting preparation done
    // The objective boss of the run in flight, and whether this party must
    // retire: once a run kills its objective, the instance's encounter state
    // says done — DC would read every later run here as "all cleared" — and a
    // creature respawn cannot undo that. A fresh party gets a fresh instance.
    ObjectGuid _objectiveGuid;
    bool _retire = false;
    std::uint32_t _runStartMs = 0;
    std::uint32_t _runsDone = 0;
    std::string _lastVerdict;
    std::string _reactProfile = "fast";

    // Hidden creature bookkeeping: spawnId -> original phase mask + react state,
    // so a creature hidden for one scenario comes back exactly for the next.
    struct Hidden
    {
        std::uint32_t phaseMask = 1;
        std::uint8_t react = 2;
    };
    std::map<std::uint64_t, Hidden> _hidden;
    std::map<std::string, ObjectGuid> _synthetic;
    std::set<ObjectGuid> _summons;
    std::set<ObjectGuid> _goSummons;
};

#endif  // _PLAYERBOT_DCLABJOB_H
