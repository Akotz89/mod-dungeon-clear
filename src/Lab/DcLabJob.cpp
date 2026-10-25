/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Lab/DcLabJob.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <iterator>
#include <mutex>
#include <random>

#include "Creature.h"
#include "GridDefines.h"
#include "TypeContainerVisitor.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "StringFormat.h"
#include "TemporarySummon.h"
#include "Timer.h"

#include "Playerbots.h"
#include "PlayerbotAI.h"

#include "DcStrategyGate.h"
#include "Ai/Dungeon/DungeonClear/Action/DcActionShared.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/DcPullContext.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Util/DcFollowerLifecycle.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Lab/DcLabInject.h"
#include "Lab/DcLabJson.h"
#include "Lab/DcLabOracles.h"
#include "Lab/DcLabPaths.h"
#include "Lab/DcLabPuppet.h"
#include "Lab/DcLabRecorder.h"

namespace
{
    // A phase no player is ever in: a creature moved here is invisible to the
    // party and to every grid search that checks phase (aggro, call-for-help,
    // DC's own scans), and comes back exactly by restoring its mask.
    constexpr uint32 kLabHiddenPhase = 0x40000000;

    constexpr uint32 kSettleMinMs = 3000;  // also the gap between back-to-back runs
    constexpr uint32 kSettleWaitExtrasMs = 12000;  // kept spawns / pets
    constexpr uint32 kSettleRecombatMs = 8000;
    constexpr uint32 kSettleGiveUpMs = 20000;
    constexpr uint32 kDcOnTimeoutMs = 10000;
    constexpr uint32 kDcOffGraceMs = 3000;   // after start, before "DC switched itself off" ends a run

    // Hide rule for creatures that enter the world after staging (grids that
    // load as the party moves). Read on map threads, written on the world thread.
    struct HideRule
    {
        std::set<uint64> keep;
        uint32 objective = 0;
        ObjectGuid tank;
        std::vector<std::pair<uint64, std::pair<uint32, uint8>>> hiddenOnAdd;  // spawnId -> (phase, react)
    };
    std::mutex g_ruleMutex;
    std::map<std::pair<uint32, uint32>, HideRule> g_rules;  // (map, instance)
    std::atomic<std::size_t> g_ruleCount{0};                // lock-free "any Lab instance?" for the hook

    uint64 NowUnixMs()
    {
        return static_cast<uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());
    }

    DcLabJson::Value Str(std::string const& s)
    {
        DcLabJson::Value v;
        v.type = DcLabJson::Value::Type::String;
        v.str = s;
        return v;
    }
    DcLabJson::Value Num(double d)
    {
        DcLabJson::Value v;
        v.type = DcLabJson::Value::Type::Number;
        v.num = d;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.10g", d);
        v.str = buf;
        return v;
    }
    DcLabJson::Value Bool(bool b)
    {
        DcLabJson::Value v;
        v.type = DcLabJson::Value::Type::Bool;
        v.b = b;
        return v;
    }
    DcLabJson::Value Obj()
    {
        DcLabJson::Value v;
        v.type = DcLabJson::Value::Type::Object;
        return v;
    }
    DcLabJson::Value Arr()
    {
        DcLabJson::Value v;
        v.type = DcLabJson::Value::Type::Array;
        return v;
    }
}

DcLabJob::DcLabJob(std::string partyKey) : _key(std::move(partyKey)) {}

DcLabJob::~DcLabJob()
{
    std::lock_guard<std::mutex> lock(g_ruleMutex);
    g_rules.erase({_mapId, _instanceId});
    g_ruleCount = g_rules.size();
}

char const* DcLabJob::StageName(Stage s)
{
    switch (s)
    {
        case Stage::Idle: return "idle";
        case Stage::Staging: return "staging";
        case Stage::Settling: return "settling";
        case Stage::Starting: return "starting";
        case Stage::Running: return "running";
        case Stage::Scoring: return "scoring";
    }
    return "?";
}

std::string const& DcLabJob::CurrentRunId() const
{
    static std::string const none;
    return _run ? _run->runId : none;
}

std::string DcLabJob::Status() const
{
    std::string s = StageName(_stage);
    if (_run)
        s += " " + _run->runId + " " + _run->scenario.id + (_run->sweepPoint.empty() ? "" : " @" + _run->sweepPoint) +
             " " + std::to_string(_stageMs / 1000) + "s";
    s += ", " + std::to_string(_runsDone) + " done";
    if (!_lastVerdict.empty())
        s += ", last " + _lastVerdict;
    return s;
}

bool DcLabJob::KeepGmMaster() const
{
    // The host re-installs the GM on every bot each tick; off whenever a bot
    // must have a different master (the puppet) or none (masterless).
    return _reactProfile == "fast" && _masterless.empty() && !(_puppet && _puppet->Attached());
}

void DcLabJob::SetReactFor(std::vector<ObjectGuid> const& bots, std::string const& profile)
{
    Player* gm = ObjectAccessor::FindConnectedPlayer(_gm);
    Player* human = _puppet && _puppet->Attached() ? _puppet->Human() : nullptr;
    for (ObjectGuid const& g : bots)
    {
        Player* p = ObjectAccessor::FindPlayer(g);
        PlayerbotAI* ai = p ? GET_PLAYERBOT_AI(p) : nullptr;
        if (!ai)
            continue;
        if (profile == "masterless")
        {
            _masterless.insert(g);
            ai->SetMaster(nullptr);
        }
        else
        {
            _masterless.erase(g);
            ai->SetMaster(human ? human : gm);
        }
    }
}

void DcLabJob::ReassertMasters()
{
    // Only needed when the host is not doing it (KeepGmMaster false) and no
    // puppet owns mastership: keep the fast bots on the GM, the masterless
    // ones on nothing.
    if (KeepGmMaster() || (_puppet && _puppet->Attached()))
        return;
    Player* gm = ObjectAccessor::FindConnectedPlayer(_gm);
    for (std::size_t i = 0; i < _members.size(); ++i)
        if (Player* p = Member(i))
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
            {
                Player* want = _masterless.count(p->GetGUID()) || _reactProfile == "masterless" ? nullptr : gm;
                if (ai->GetMaster() != want)
                    ai->SetMaster(want);
            }
}

bool DcLabJob::AttachPuppet()
{
    DcLabScenario::Scenario const& sc = _run->scenario;
    if (sc.human.slot <= 0)
        return true;
    if (static_cast<std::size_t>(sc.human.slot) >= _members.size())
    {
        Abandon("party.human.slot " + std::to_string(sc.human.slot) + " is past the party");
        return false;
    }
    std::string err;
    if (!_puppet)
        _puppet = std::make_unique<DcLabPuppet>(*this, _members[sc.human.slot]);
    if (!_puppet->LoadScript(sc.human.script, &err) || (!_puppet->Attached() && !_puppet->Attach(&err)))
    {
        Abandon("puppet: " + err);
        return false;
    }
    return true;
}

Player* DcLabJob::Member(std::size_t slot) const
{
    return slot < _members.size() ? ObjectAccessor::FindPlayer(_members[slot]) : nullptr;
}

void DcLabJob::Enter(Stage s)
{
    _stage = s;
    _stageMs = 0;
}

bool DcLabJob::Tick(DcTestRunJob::LabParty const& party, uint32 diff)
{
    _gm = party.gm;
    _members = party.members;
    _roles = party.roles;
    _hostRunId = party.runId;
    if (party.instanceId)
    {
        _mapId = party.mapId;
        _instanceId = party.instanceId;
    }
    _stageMs += diff;
    if (_puppet)
        _puppet->CompleteTeleport();

    // Fold in what the late-grid hook hid on map threads since last tick.
    {
        std::lock_guard<std::mutex> lock(g_ruleMutex);
        auto it = g_rules.find({_mapId, _instanceId});
        if (it != g_rules.end())
        {
            for (auto const& h : it->second.hiddenOnAdd)
                if (!_hidden.count(h.first))
                    _hidden[h.first] = Hidden{h.second.first, h.second.second};
            it->second.hiddenOnAdd.clear();
        }
    }

    // The party must still be standing in its own Lab instance. A global
    // instance reset (or anything else that moves it out) ends this party; the
    // teardown hands the run in flight back to the queue for a fresh party.
    if (_instanceId)
        if (Player* tank = Tank())
            if (tank->IsInWorld() && !tank->IsBeingTeleported() &&
                (tank->GetMapId() != _mapId || tank->GetInstanceId() != _instanceId))
            {
                LOG_WARN("playerbots.dungeonclear", "LAB party {} left its instance ({} #{} -> {} #{}); ending it",
                         _hostRunId, _mapId, _instanceId, tank->GetMapId(), tank->GetInstanceId());
                return false;
            }

    switch (_stage)
    {
        case Stage::Idle:
            if (_retire || !BeginRun())
            {
                _finished = true;
                return false;
            }
            break;
        case Stage::Staging: TickStaging(); break;
        case Stage::Settling: TickSettling(); break;
        case Stage::Starting: TickStarting(); break;
        case Stage::Running: TickRunning(diff); break;
        case Stage::Scoring: Enter(Stage::Idle); break;
    }
    return true;
}

bool DcLabJob::BeginRun()
{
    if (_pending)
    {
        _run = std::move(_pending);
        _pending.reset();
    }
    else
        _run = DcLabManager::Instance().Next(_key);
    if (!_run)
        return false;
    _prepared = false;
    _masterless.clear();  // a setReact injection belongs to the run that made it
    LOG_INFO("playerbots.dungeonclear", "LAB {} start {}{} seed={} (party {})", _run->runId, _run->scenario.id,
             _run->sweepPoint.empty() ? "" : " @" + _run->sweepPoint, _run->seed, _hostRunId);
    Enter(Stage::Staging);
    return true;
}

// ------------------------------------------------------------------ staging

void DcLabJob::TickStaging()
{
    Player* tank = Tank();
    if (!tank || !tank->IsInWorld() || tank->IsBeingTeleported())
    {
        if (_stageMs > kSettleGiveUpMs)
            Abandon("tank not in world at staging");
        return;
    }
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(tank))
    {
        if (DcRun::Of(ai).enabled)
            DcActionShared::DisableDungeonClear(ai, "lab restage");
        DcSettings::ClearRun(tank->GetGUID());
    }
    if (_puppet)
        _puppet->Reset();
    ResetParty();
    StageCreatures(tank->GetMap(), tank);
    SpawnSynthetic("start");
    Enter(Stage::Settling);
}

void DcLabJob::ResetParty()
{
    DcLabScenario::Scenario const& sc = _run->scenario;
    std::mt19937 rng(_run->seed * 2654435761u + _run->repeat);
    std::uniform_real_distribution<float> jitter(-0.75f, 0.75f);
    static float const kOff[][2] = {{0, 0},       {-3, 1.5f},  {-3, -1.5f}, {-5.5f, 2.5f}, {-5.5f, -2.5f},
                                    {-8, 0},      {-8, 3},     {-8, -3},    {-10.5f, 1.5f}, {-10.5f, -1.5f}};
    float const o = sc.start.o;
    float const co = std::cos(o), so = std::sin(o);

    for (std::size_t i = 0; i < _members.size(); ++i)
    {
        Player* p = Member(i);
        if (!p || !p->IsInWorld())
            continue;
        if (!p->IsAlive() || p->HasPlayerFlag(PLAYER_FLAGS_GHOST))
        {
            p->RemoveAurasDueToSpell(27827);
            p->ResurrectPlayer(1.0f, false);
            p->SpawnCorpseBones();
        }
        p->CombatStop(true);
        p->SetFullHealth();
        if (p->GetMaxPower(POWER_MANA) > 0)
            p->SetPower(POWER_MANA, p->GetMaxPower(POWER_MANA));
        p->SetPower(POWER_RAGE, 0);
        p->RemoveAllSpellCooldown();
        p->UnsummonAllTotems();  // a Searing Totem outlived its run into the next
        p->RemoveAllDynObjects();  // a Flamestrike still burning where the pack respawns
        if (Pet* pet = p->GetPet())
        {
            pet->CombatStop(true);
            pet->SetFullHealth();
        }
        if (i > 0)
            DcFollowerLifecycle::RemoveFollowerPassive(p);

        std::size_t const k = std::min<std::size_t>(i, std::size(kOff) - 1);
        float const dx = kOff[k][0] + (i ? jitter(rng) : 0.0f);
        float const dy = kOff[k][1] + (i ? jitter(rng) : 0.0f);
        float const x = sc.start.x + dx * co - dy * so;
        float const y = sc.start.y + dx * so + dy * co;
        // A height far from the authored start is the wrong floor (open
        // terrain under an instance, a lower level): stage on the start's own z.
        float z = p->GetMap()->GetHeight(p->GetPhaseMask(), x, y, sc.start.z + 2.0f, true, 10.0f);
        if (z <= INVALID_HEIGHT || std::fabs(z - sc.start.z) > 4.0f)
            z = sc.start.z;
        p->NearTeleportTo(x, y, z + 0.1f, o);

        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
        {
            ai->Reset();
            ai->GetAiObjectContext()->GetValue<DcPullContext&>(DcKey::PullContext)->Get().Reset();
        }
    }
}

bool DcLabJob::ResetKept(Creature* c)
{
    if (!c->IsInCombat() && !c->IsInEvadeMode() && c->GetHealth() >= c->GetMaxHealth() &&
        c->GetExactDist(&c->GetHomePosition()) <= 1.0f)
        return false;
    c->CombatStop(true);
    c->GetThreatMgr().ClearAllThreat();
    c->RemoveAllAuras();  // the last run's DoTs would keep it in combat
    c->SetFullHealth();
    Position const& home = c->GetHomePosition();
    c->NearTeleportTo(home.GetPositionX(), home.GetPositionY(), home.GetPositionZ(), home.GetOrientation());
    c->GetMotionMaster()->Initialize();
    return true;
}

void DcLabJob::Hide(Creature* c)
{
    uint64 const id = c->GetSpawnId();
    if (!_hidden.count(id))
        _hidden[id] = Hidden{c->GetPhaseMask(), static_cast<uint8>(c->GetReactState())};
    else if (c->GetPhaseMask() == kLabHiddenPhase)
        return;
    c->CombatStop(true);
    c->SetReactState(REACT_PASSIVE);
    c->SetPhaseMask(kLabHiddenPhase, true);
}

void DcLabJob::Unhide(Creature* c)
{
    auto it = _hidden.find(c->GetSpawnId());
    if (it == _hidden.end())
    {
        // Hidden by the add-world hook this very tick (not folded in yet):
        // its saved state is still in the rule, but the defaults are right
        // for a spawn that just entered the world.
        if (c->GetPhaseMask() == kLabHiddenPhase)
        {
            c->SetReactState(REACT_AGGRESSIVE);
            c->SetPhaseMask(PHASEMASK_NORMAL, true);
        }
        return;
    }
    c->SetReactState(static_cast<ReactStates>(it->second.react));
    c->SetPhaseMask(it->second.phaseMask, true);
    _hidden.erase(it);
}

void DcLabJob::StageCreatures(Map* map, Player* tank)
{
    if (!map || !tank)
        return;
    DcLabScenario::Scenario const& sc = _run->scenario;

    // Last run's synthetic actors, summons and LOS blockers go first.
    for (ObjectGuid const& g : _summons)
        if (Creature* c = map->GetCreature(g))
            c->DespawnOrUnsummon();
    _summons.clear();
    _synthetic.clear();
    for (ObjectGuid const& g : _goSummons)
        if (GameObject* go = map->GetGameObject(g))
            go->DespawnOrUnsummon();
    _goSummons.clear();

    // Load what the scenario needs before walking the spawn store: the region,
    // the start, and every kept spawn's home cell.
    map->LoadGridsInRange(Position(sc.cx, sc.cy, sc.cz), sc.radius);
    map->LoadGrid(sc.start.x, sc.start.y);
    std::set<uint64> keep;
    for (auto const& kv : sc.packs)
        for (uint64 id : kv.second)
        {
            keep.insert(id);
            if (CreatureData const* d = sObjectMgr->GetCreatureData(id))
                map->LoadGrid(d->posX, d->posY);
        }

    // The late-grid hide rule must already carry THIS scenario's keep set: the
    // respawns just below enter the world through the same hook, and under the
    // previous scenario's rule a newly kept spawn would be hidden on arrival.
    {
        std::lock_guard<std::mutex> lock(g_ruleMutex);
        HideRule& r = g_rules[{_mapId, _instanceId}];
        r.keep = keep;
        r.objective = sc.objective;
        r.tank = tank->GetGUID();
        g_ruleCount = g_rules.size();
    }

    // A kept spawn whose object is gone (corpse removed under the dynamic
    // respawn system) is brought back now rather than on its timer.
    auto& store = map->GetCreatureBySpawnIdStore();
    for (uint64 id : keep)
        if (store.find(id) == store.end())
        {
            map->RemoveRespawnTime(SPAWN_TYPE_CREATURE, id);
            map->ProcessCreatureRespawn(id);
        }

    // Snapshot first: Respawn can touch the store while we iterate it.
    std::vector<std::pair<uint64, Creature*>> all;
    all.reserve(store.size());
    for (auto const& kv : store)
        all.emplace_back(kv.first, kv.second);

    uint32 kept = 0, hidden = 0;
    for (auto const& [spawnId, c] : all)
    {
        if (!c || !c->IsInWorld())
            continue;
        bool const isKept = keep.count(spawnId) || (sc.objective && c->GetEntry() == sc.objective);
        if (isKept)
        {
            Unhide(c);
            if (!c->IsAlive())
                c->Respawn(true);
            else
                ResetKept(c);
            ++kept;
            continue;
        }
        if (c->IsFriendlyTo(tank) || c->IsPet() || c->IsTotem())
            continue;
        if (!sc.hideAll && c->GetExactDist2d(sc.cx, sc.cy) > sc.radius + 40.0f)
            continue;
        Hide(c);
        ++hidden;
    }

    HideStraySummons(map, tank);
    LOG_INFO("playerbots.dungeonclear", "LAB {} staged {}: {} kept, {} hidden, {} total spawns", _run->runId,
             sc.id, kept, hidden, all.size());
}

namespace
{
    // Set around the Lab's own SummonCreature calls so the add-world hook
    // never hides a synthetic actor it is in the middle of creating.
    thread_local bool t_labSummoning = false;

    // Every creature on a map (summons included — the spawn store has only DB spawns).
    struct CreatureCollector
    {
        std::vector<Creature*> out;
        void Visit(std::unordered_map<ObjectGuid, Creature*>& m)
        {
            for (auto& kv : m)
                out.push_back(kv.second);
        }
        template <class T>
        void Visit(std::unordered_map<ObjectGuid, T*>&) {}
    };

    // A summon belongs with the scenario when whatever summoned it is kept
    // (a kept necromancer's skeleton, a kept vehicle's passengers) or a player.
    bool SummonBelongs(Creature* c, std::set<uint64> const& keep, uint32 objective)
    {
        TempSummon* ts = c->ToTempSummon();
        Unit* summoner = ts ? ts->GetSummonerUnit() : nullptr;
        if (!summoner)
            summoner = c->GetVehicleBase();
        if (!summoner)
            return false;
        if (summoner->IsPlayer() || summoner->GetCharmerOrOwnerPlayerOrPlayerItself())
            return true;
        if (Creature* sc = summoner->ToCreature())
            return keep.count(sc->GetSpawnId()) || (objective && sc->GetEntry() == objective);
        return false;
    }
}

DcLabJob::SummonScope::SummonScope() { t_labSummoning = true; }
DcLabJob::SummonScope::~SummonScope() { t_labSummoning = false; }

void DcLabJob::HideStraySummons(Map* map, Player* tank)
{
    CreatureCollector worker;
    TypeContainerVisitor<CreatureCollector, MapStoredObjectTypesContainer> visitor(worker);
    visitor.Visit(map->GetObjectsStore());
    std::set<uint64> keep;
    for (auto const& kv : _run->scenario.packs)
        keep.insert(kv.second.begin(), kv.second.end());
    for (Creature* c : worker.out)
    {
        if (!c || !c->IsInWorld() || c->GetSpawnId() || !c->IsAlive() || _summons.count(c->GetGUID()))
            continue;
        if (c->IsPet() || c->IsTotem() || c->IsFriendlyTo(tank) || SummonBelongs(c, keep, _run->scenario.objective))
            continue;
        c->CombatStop(true);
        c->SetReactState(REACT_PASSIVE);
        c->SetPhaseMask(kLabHiddenPhase, true);
    }
}

bool DcLabJob::HideOnAdd(Creature* c)
{
    if (!g_ruleCount.load(std::memory_order_relaxed) || t_labSummoning)
        return false;
    if (!c || !c->GetMap() || !c->GetMap()->IsDungeon())
        return false;
    std::lock_guard<std::mutex> lock(g_ruleMutex);
    auto it = g_rules.find({c->GetMapId(), c->GetInstanceId()});
    if (it == g_rules.end())
        return false;
    HideRule& r = it->second;
    if (r.keep.count(c->GetSpawnId()) || (r.objective && c->GetEntry() == r.objective))
        return false;
    Player* tank = ObjectAccessor::GetPlayer(*c, r.tank);
    if (!tank || c->IsFriendlyTo(tank) || c->IsPet() || c->IsTotem())
        return false;
    if (!c->GetSpawnId())
    {
        // A script summon or passenger: hide unless it came with something kept.
        // Never recorded for restoring — the next run's instance script makes
        // its own.
        if (SummonBelongs(c, r.keep, r.objective))
            return false;
        c->SetReactState(REACT_PASSIVE);
        c->SetPhaseMask(kLabHiddenPhase, false);
        return true;
    }
    r.hiddenOnAdd.push_back({c->GetSpawnId(), {c->GetPhaseMask(), static_cast<uint8>(c->GetReactState())}});
    c->SetReactState(REACT_PASSIVE);
    c->SetPhaseMask(kLabHiddenPhase, false);
    return true;
}

void DcLabJob::SpawnSynthetic(std::string const& when)
{
    Player* tank = Tank();
    if (!tank || !_run)
        return;
    for (DcLabScenario::Synthetic const& s : _run->scenario.synthetic)
    {
        if (s.spawn != when || _synthetic.count(s.tag))
            continue;
        TempSummon* c = nullptr;
        {
            SummonScope scope;
            c = tank->GetMap()->SummonCreature(s.entry, Position(s.pos.x, s.pos.y, s.pos.z, s.pos.o));
        }
        if (!c)
        {
            LOG_WARN("playerbots.dungeonclear", "LAB {} could not summon synthetic {} (entry {})", _run->runId, s.tag,
                     s.entry);
            continue;
        }
        _synthetic[s.tag] = c->GetGUID();
        _summons.insert(c->GetGUID());
        if (_recorder)
            _recorder->AddUnit(c, s.pack);
    }
}

// ------------------------------------------------------------------ settle + start

void DcLabJob::TickSettling()
{
    bool settled = true;
    std::string why;
    for (std::size_t i = 0; i < _members.size(); ++i)
    {
        Player* p = Member(i);
        char const* bad = !p                        ? "missing"
                          : !p->IsInWorld()          ? "not in world"
                          : p->IsBeingTeleported()   ? "teleporting"
                          : !p->IsAlive()            ? "dead"
                          : p->IsInCombat()          ? "in combat"
                                                     : nullptr;
        if (bad)
        {
            settled = false;
            why += std::string(why.empty() ? "" : ", ") + (p ? p->GetName() : "slot " + std::to_string(i)) +
                   " " + bad;
        }
    }
    // Kept spawns brought back at staging may land a beat later (respawn
    // processing, linked respawns); start only once they are all standing, so
    // the recorder and the pull latch see them. Pet classes get their pet up
    // first — a fresh warlock otherwise spends the pull casting Summon Imp.
    // Both waits are bounded: a spawn or pet that never comes is logged, and
    // the run goes ahead.
    bool const patient = _stageMs < kSettleWaitExtrasMs;
    if (settled && patient)
    {
        if (Player* tank = Tank())
        {
            auto& store = tank->GetMap()->GetCreatureBySpawnIdStore();
            for (auto const& kv : _run->scenario.packs)
                for (uint64 id : kv.second)
                {
                    auto range = store.equal_range(id);
                    bool alive = false;
                    for (auto it = range.first; it != range.second; ++it)
                        if (it->second && it->second->IsInWorld() && it->second->IsAlive())
                        {
                            alive = true;
                            // Still fighting/evading from the last run: reset again.
                            if (ResetKept(it->second))
                            {
                                settled = false;
                                why += std::string(why.empty() ? "" : ", ") + "kept " + std::to_string(id) +
                                       " not reset";
                            }
                        }
                    if (!alive)
                        why += std::string(why.empty() ? "" : ", ") + "kept " + std::to_string(id) + " not up";
                    settled &= alive;
                }
        }
        for (std::size_t i = 0; i < _members.size(); ++i)
            if (Player* p = Member(i))
                if ((p->getClass() == CLASS_HUNTER || p->getClass() == CLASS_WARLOCK) && GET_PLAYERBOT_AI(p) &&
                    !p->GetPet())
                    settled = false;
    }
    if (settled && _stageMs >= kSettleMinMs)
    {
        if (!patient)
            LOG_WARN("playerbots.dungeonclear", "LAB {} starting without every kept spawn/pet up after {}ms",
                     _run->runId, _stageMs);
        Enter(Stage::Starting);
        return;
    }
    if (_stageMs >= kSettleRecombatMs && _stageMs < kSettleRecombatMs + 300)
        for (std::size_t i = 0; i < _members.size(); ++i)
            if (Player* p = Member(i))
                p->CombatStop(true);
    if (_stageMs >= kSettleGiveUpMs + kSettleWaitExtrasMs)
        Abandon("party would not settle after staging: " + why);
}

void DcLabJob::ApplyReactProfile(std::string const& profile)
{
    _reactProfile = profile;
    Player* gm = ObjectAccessor::FindConnectedPlayer(_gm);
    for (std::size_t i = 0; i < _members.size(); ++i)
        if (Player* p = Member(i))
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
            {
                // fast: the GM master keeps GetReactDelay() on its 100ms path
                // (the host re-asserts it every tick). masterless: no master at
                // all — the 1-3s out-of-combat think the leader used to fall into.
                ai->SetMaster(profile == "fast" ? gm : nullptr);
                ai->ChangeStrategy("-follow", BOT_STATE_NON_COMBAT);
                ai->ChangeStrategy("-follow", BOT_STATE_COMBAT);
            }
}

void DcLabJob::ApplySkips(Player* tank)
{
    uint32 const objective = _run->scenario.objective;
    if (!objective || !tank)
        return;
    PlayerbotAI* ai = GET_PLAYERBOT_AI(tank);
    if (!ai)
        return;
    AiObjectContext* ctx = ai->GetAiObjectContext();
    auto& skipped = ctx->GetValue<std::unordered_set<uint32>&>(DcKey::Skipped)->Get();
    for (DungeonBossInfo const& b : ctx->GetValue<std::vector<DungeonBossInfo>>(DcKey::DungeonBosses)->Get())
        if (b.entry != objective)
            skipped.insert(b.entry);
    skipped.erase(objective);
}

void DcLabJob::TickStarting()
{
    DcLabScenario::Scenario const& sc = _run->scenario;
    Player* tank = Tank();
    PlayerbotAI* tankAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
    if (!tankAI)
    {
        if (_stageMs > kDcOnTimeoutMs)
            Abandon("tank has no AI at start");
        return;
    }
    AiObjectContext* ctx = tankAI->GetAiObjectContext();

    // Once per run: the puppet (attached once per party, script reloaded every
    // run — Reset() cleared the last run's steps at staging).
    if (!_prepared)
    {
        if (!AttachPuppet())
            return;
        _prepared = true;
    }
    Player* const human = _puppet && _puppet->Attached() ? _puppet->Human() : nullptr;

    if (_stageMs == 0 || !DcRun::Of(ctx).enabled)
    {
        for (std::size_t i = 0; i < _members.size(); ++i)
            if (Player* p = Member(i))
                if (GET_PLAYERBOT_AI(p))
                    DcStrategyGate::Reconcile(p);
        // A human-led party keeps stock follow-master on the bots: that is part
        // of what a human session turns on, and what the scenario is testing.
        if (!human)
            ApplyReactProfile(sc.react);
        else
            _reactProfile = sc.react;
        DcSettings::ClearRun(tank->GetGUID());
        for (auto const& [key, value] : sc.settings)
        {
            std::string err;
            if (!DcSettings::SetOverride(tank->GetGUID(), key, value, &err))
                LOG_WARN("playerbots.dungeonclear", "LAB {} setting {}={} refused: {}", _run->runId, key, value, err);
        }
        DcSettings::SetOverride(tank->GetGUID(), "WaitAtBoss", 0.0);
        if (sc.pullSetting >= 0)
            ctx->GetValue<uint32>(DcKey::PullSetting)->Set(static_cast<uint32>(sc.pullSetting));
        tankAI->DoSpecificAction("dc on", Event("dc", "", human ? human : ObjectAccessor::FindConnectedPlayer(_gm)), true);
    }
    if (!DcRun::Of(ctx).enabled)
    {
        if (_stageMs > kDcOnTimeoutMs)
            Abandon("dc on did not take (look for 'DC command refused' in the DC log)");
        return;
    }

    DcTargeting::ResetCompletionLatchesForNewInstance(tank, ctx);
    // The roster must exist before the skips mean anything: an empty roster
    // here let the first route go to the map's real next boss (lb-20261003-
    // 155828-1 -49/-50/-53). Then pin the objective the way `dc go` does.
    if (sc.objective && ctx->GetValue<std::vector<DungeonBossInfo>>(DcKey::DungeonBosses)->Get().empty())
    {
        if (_stageMs > kDcOnTimeoutMs)
            Abandon("no boss roster for this map");
        return;
    }
    ApplySkips(tank);
    if (sc.objective)
        DcRun::Of(ctx).selectedBossEntry = sc.objective;

    // Recorder: the scenario's kept creatures with their pack tags, then
    // anything hostile within the region as it shows up.
    DcLab::Header h;
    h.scenario = sc.id;
    h.runId = _run->runId;
    h.sweepPoint = _run->sweepPoint;
    h.seed = _run->seed;
    h.mapId = tank->GetMapId();
    h.instanceId = tank->GetInstanceId();
    h.pullSetting = ctx->GetValue<uint32>(DcKey::PullSetting)->Get();
    h.releaseDelayMs = static_cast<uint32>(DcSettings::GetFloat(tank->GetGUID(), "PullPlayerReleaseDelay") * 1000.0f);
    for (auto const& [key, value] : sc.settings)
        h.settings.push_back({key, value});
    if (sc.pullSetting >= 0)
        h.settings.push_back({"PullSetting", static_cast<double>(sc.pullSetting)});
    h.goalPacks = sc.goalPacks;
    h.timeoutMs = sc.timeoutS * 1000;
    _recorder = std::make_unique<DcLabRecorder>(std::move(h));
    for (std::size_t i = 0; i < _members.size(); ++i)
        if (Player* p = Member(i))
            _recorder->AddMember(p, i < _roles.size() ? _roles[i] : "dps", static_cast<int>(i) == sc.human.slot,
                                 i == 0, sc.react);
    _recorder->SetDiscoveryRadius(sc.radius);
    _recorder->SetPackResolver([packs = sc.packs](Creature* c) -> std::string
    {
        for (auto const& kv : packs)
            for (uint64 id : kv.second)
                if (id == c->GetSpawnId())
                    return kv.first;
        return "";
    });
    Map* map = tank->GetMap();
    auto& store = map->GetCreatureBySpawnIdStore();
    for (auto const& kv : sc.packs)
        for (uint64 id : kv.second)
        {
            auto range = store.equal_range(id);
            for (auto it = range.first; it != range.second; ++it)
                _recorder->AddUnit(it->second, kv.first);
        }
    for (auto const& [tag, guid] : _synthetic)
        if (Creature* c = map->GetCreature(guid))
            for (DcLabScenario::Synthetic const& s : sc.synthetic)
                if (s.tag == tag)
                    _recorder->AddUnit(c, s.pack);

    // Optional: pin the pull latch on the target pack, so the scenario tests
    // the pull of THIS pack rather than whatever FindPullTarget prefers first.
    auto firstAliveTarget = [&]() -> Creature*
    {
        auto it = sc.packs.find("target");
        if (it == sc.packs.end())
            return nullptr;
        for (uint64 id : it->second)
        {
            auto range = store.equal_range(id);
            for (auto c = range.first; c != range.second; ++c)
                if (c->second && c->second->IsAlive())
                    return c->second;
        }
        return nullptr;
    };
    _objectiveGuid = ObjectGuid::Empty;
    if (sc.objective)
        for (auto const& kv : store)
            if (kv.second && kv.second->GetEntry() == sc.objective && kv.second->IsAlive())
            {
                _objectiveGuid = kv.second->GetGUID();
                break;
            }

    if (sc.seedTarget)
        if (Creature* target = firstAliveTarget())
            ctx->GetValue<DcPullContext&>(DcKey::PullContext)->Get().decisionTarget = target->GetGUID();

    _recorder->Arm();
    _injector = std::make_unique<DcLabInjector>(*this);
    _runStartMs = getMSTime();
    Enter(Stage::Running);
}

// ------------------------------------------------------------------ running

bool DcLabJob::GoalReached() const
{
    DcLab::Frame const* f = _recorder ? _recorder->LastFrame() : nullptr;
    if (!f)
        return false;
    DcLab::Trace const& tr = _recorder->View();
    std::uint32_t goal = 0, dead = 0;
    for (DcLab::Unit const& u : tr.units)
    {
        bool isGoal = false;
        for (std::string const& g : _run->scenario.goalPacks)
            isGoal |= (g == "*" ? !u.pack.empty() : u.pack == g);
        if (!isGoal)
            continue;
        ++goal;
        for (DcLab::UnitSample const& s : f->units)
            if (s.guid == u.guid && !s.alive)
                ++dead;
    }
    return goal > 0 && dead == goal;
}

bool DcLabJob::PartyWiped() const
{
    bool any = false;
    for (std::size_t i = 0; i < _members.size(); ++i)
        if (Player* p = Member(i))
        {
            any = true;
            if (p->IsAlive())
                return false;
        }
    return any;
}

void DcLabJob::TickRunning(uint32 diff)
{
    _recorder->Tick(diff);
    std::vector<std::pair<uint32, std::string>> const events = _recorder->DrainPullEvents();
    if (_injector)
        _injector->Tick(events);
    if (_puppet)
        _puppet->Tick(diff, events);
    ReassertMasters();
    Player* tank = Tank();
    _skipAccum += diff;
    if (_skipAccum >= 1000)
    {
        _skipAccum = 0;
        ApplySkips(tank);
    }

    if (GoalReached())
        return Score("goal");
    if (PartyWiped())
        return Score("wipe");
    if (_stageMs >= _run->scenario.timeoutS * 1000)
        return Score("timeout");
    if (_objectiveGuid && tank)
    {
        Creature* obj = tank->GetMap()->GetCreature(_objectiveGuid);
        if (!obj || !obj->IsAlive())
        {
            _retire = true;
            LOG_INFO("playerbots.dungeonclear", "LAB {} objective boss died; party {} retires after this run",
                     _run->runId, _hostRunId);
            return Score("objective-killed");
        }
    }
    if (tank && _stageMs > kDcOffGraceMs)
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(tank))
            if (!DcRun::Of(ai).enabled)
                return Score("dc-disabled");
}

// ------------------------------------------------------------------ scoring

void DcLabJob::Score(std::string const& end)
{
    Enter(Stage::Scoring);
    DcLabScenario::Scenario const& sc = _run->scenario;
    _recorder->End(end);
    DcLab::Trace& tr = _recorder->TraceRef();
    DcLabOracles::OracleConfig cfg;
    std::string unknown;
    DcLabOracles::ApplyOverrides(cfg, sc.oracleExpect, &unknown);
    if (!unknown.empty())
        LOG_WARN("playerbots.dungeonclear", "LAB {} unknown oracle settings in {}: {}", _run->runId, sc.id, unknown);
    tr.oracles = DcLabOracles::Evaluate(tr, cfg);

    DcLabManager::Result res;
    res.runId = _run->runId;
    res.batchId = _run->batchId;
    res.scenario = sc.id;
    res.sweepPoint = _run->sweepPoint;
    res.end = end;
    res.oracles = DcLabOracles::Summary(tr.oracles);
    for (DcLab::OracleResult const& o : tr.oracles)
        res.oracleRes.emplace_back(o.id, DcLab::VerdictName(o.verdict));
    res.verdict = DcLabScenario::Judge(sc, res.oracleRes);
    res.durationMs = tr.endMs;
    res.traceFile = _recorder->WriteFile(DcLabPaths::TraceDir());

    DcLabJson::Value rec = Obj();
    rec.obj["schema"] = Num(1);
    rec.obj["runId"] = Str(res.runId);
    rec.obj["batchId"] = Str(res.batchId);
    rec.obj["scenario"] = Str(sc.id);
    rec.obj["sweep"] = Str(_run->sweepPoint);
    rec.obj["seed"] = Num(_run->seed);
    rec.obj["repeat"] = Num(_run->repeat);
    rec.obj["host"] = Str(_hostRunId);
    rec.obj["map"] = Num(tr.header.mapId);
    rec.obj["instance"] = Num(tr.header.instanceId);
    rec.obj["endedAt"] = Num(static_cast<double>(NowUnixMs()));
    rec.obj["durationMs"] = Num(res.durationMs);
    rec.obj["end"] = Str(end);
    rec.obj["verdict"] = Str(res.verdict);
    rec.obj["knownFailure"] = Str(sc.knownFailure);
    rec.obj["sha"] = Str(tr.header.moduleSha);
    rec.obj["pbSha"] = Str(tr.header.playerbotsSha);
    rec.obj["trace"] = Str(res.traceFile);
    rec.obj["summary"] = Str(res.oracles);
    DcLabJson::Value comp = Arr();
    for (DcLab::Member const& m : tr.header.party)
    {
        DcLabJson::Value e = Obj();
        e.obj["name"] = Str(m.name);
        e.obj["class"] = Str(m.cls);
        e.obj["role"] = Str(m.role);
        e.obj["human"] = Bool(m.human);
        comp.arr.push_back(e);
    }
    rec.obj["comp"] = comp;
    DcLabJson::Value oracles = Obj();
    for (DcLab::OracleResult const& o : tr.oracles)
    {
        DcLabJson::Value e = Obj();
        e.obj["res"] = Str(DcLab::VerdictName(o.verdict));
        e.obj["t"] = Num(o.firstMs);
        e.obj["unit"] = Str(DcLabJson::HexGuid(o.unit));
        e.obj["n"] = Num(o.count);
        e.obj["detail"] = Str(o.detail);
        oracles.obj[o.id] = e;
    }
    rec.obj["oracles"] = oracles;
    DcLabJson::Value expected = Obj();
    for (auto const& kv : sc.expected)
        expected.obj[kv.first] = Str(kv.second);
    rec.obj["expected"] = expected;

    LOG_INFO("playerbots.dungeonclear", "LAB {} {} {} end={} {} ({}ms) -> {}", res.runId, sc.id, res.verdict, end,
             res.oracles, res.durationMs, res.traceFile);
    for (DcLab::OracleResult const& o : tr.oracles)
        if (o.verdict == DcLab::Verdict::Fail)
            LOG_INFO("playerbots.dungeonclear", "LAB {} {} {} @{}ms: {}", res.runId, o.id, o.name, o.firstMs,
                     o.detail);

    DcLabManager::Instance().Report(*_run, res, DcLabJson::Dump(rec));
    _lastVerdict = res.verdict;
    ++_runsDone;
    _injector.reset();
    _recorder.reset();
    if (Player* tank = Tank())
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(tank))
            if (DcRun::Of(ai).enabled)
                DcActionShared::DisableDungeonClear(ai, "lab run scored");
    _run.reset();
}

void DcLabJob::Abandon(std::string const& why)
{
    LOG_WARN("playerbots.dungeonclear", "LAB {} abandoned ({}): {}", _run ? _run->runId : "-",
             _run ? _run->scenario.id : "-", why);
    _injector.reset();
    if (_recorder)
    {
        _recorder->End("error:" + why);
        _recorder.reset();
    }
    if (_run)
    {
        DcLabManager::Result res;
        res.runId = _run->runId;
        res.batchId = _run->batchId;
        res.scenario = _run->scenario.id;
        res.sweepPoint = _run->sweepPoint;
        res.verdict = "error";
        res.end = "error:" + why;
        DcLabJson::Value rec = Obj();
        rec.obj["schema"] = Num(1);
        rec.obj["runId"] = Str(res.runId);
        rec.obj["batchId"] = Str(res.batchId);
        rec.obj["scenario"] = Str(res.scenario);
        rec.obj["sweep"] = Str(res.sweepPoint);
        rec.obj["seed"] = Num(_run->seed);
        rec.obj["host"] = Str(_hostRunId);
        rec.obj["endedAt"] = Num(static_cast<double>(NowUnixMs()));
        rec.obj["end"] = Str(res.end);
        rec.obj["verdict"] = Str("error");
        DcLabManager::Instance().Report(*_run, res, DcLabJson::Dump(rec));
        ++_runsDone;
        _lastVerdict = "error";
        _run.reset();
    }
    if (Player* tank = Tank())
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(tank))
            if (DcRun::Of(ai).enabled)
                DcActionShared::DisableDungeonClear(ai, "lab run abandoned");
    Enter(Stage::Idle);
}

void DcLabJob::OnTeardown(DcTestRunJob::LabParty const& /*party*/, std::string const& reason)
{
    _finished = true;
    _injector.reset();
    // The AI goes back before the host logs the party out, so the logout
    // finds the character under its holder again.
    if (_puppet)
    {
        _puppet->Detach();
        _puppet.reset();
    }
    if (Player* tank = Tank())
        for (ObjectGuid const& g : _goSummons)
            if (GameObject* go = tank->GetMap()->GetGameObject(g))
                go->DespawnOrUnsummon();
    _goSummons.clear();
    if (_recorder)
    {
        _recorder->End("aborted:" + reason);
        _recorder.reset();
    }
    // A run the party died under goes back once; a party that never reached
    // its first run (setup failure) hands its primed run back the same way.
    for (std::optional<DcLabManager::LabRun>* r : {&_run, &_pending})
        if (*r)
        {
            DcLabManager::LabRun run = std::move(**r);
            r->reset();
            if (run.attempts < 1 && reason != "aborted")
            {
                ++run.attempts;
                DcLabManager::Instance().Requeue(std::move(run));
            }
            else
            {
                DcLabManager::Result res;
                res.runId = run.runId;
                res.batchId = run.batchId;
                res.scenario = run.scenario.id;
                res.sweepPoint = run.sweepPoint;
                res.verdict = "error";
                res.end = "error:party " + reason;
                DcLabJson::Value rec = Obj();
                rec.obj["schema"] = Num(1);
                rec.obj["runId"] = Str(res.runId);
                rec.obj["batchId"] = Str(res.batchId);
                rec.obj["scenario"] = Str(res.scenario);
                rec.obj["end"] = Str(res.end);
                rec.obj["verdict"] = Str("error");
                rec.obj["host"] = Str(_hostRunId);
                DcLabManager::Instance().Report(run, res, DcLabJson::Dump(rec));
            }
        }
    std::lock_guard<std::mutex> lock(g_ruleMutex);
    g_rules.erase({_mapId, _instanceId});
    g_ruleCount = g_rules.size();
}
