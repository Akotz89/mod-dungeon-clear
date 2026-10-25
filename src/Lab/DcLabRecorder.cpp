/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Lab/DcLabRecorder.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <shared_mutex>

#include "CellImpl.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Timer.h"

#include "Action.h"
#include "Engine.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"

#include "Ai/Dungeon/DungeonClear/DcPullContext.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLabTap.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Util/DcEngineAccess.h"

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

namespace
{
    // Zero-behaviour-change observer: allows everything, overrides nothing,
    // and reports each execution to the hub by guid.
    class DcLabActionListener : public ActionExecutionListener
    {
    public:
        DcLabActionListener(std::uint64_t guid, std::uint8_t engine) : _guid(guid), _engine(engine) {}

        bool Before(Action* /*action*/, Event /*event*/) override { return true; }
        bool AllowExecution(Action* /*action*/, Event /*event*/) override { return true; }
        bool OverrideResult(Action* /*action*/, bool executed, Event /*event*/) override { return executed; }
        void After(Action* action, bool executed, Event event) override
        {
            if (!action || !DcLabHub::Active())
                return;
            DcLabHub::Action(_guid, _engine, action->getName(), action->getRelevance(), executed,
                             event.GetSource());
        }

    private:
        std::uint64_t _guid;
        std::uint8_t _engine;
    };

    // --- hub state ---------------------------------------------------------
    std::shared_mutex g_hubMutex;
    std::atomic<int> g_armedCount{0};
    std::unordered_map<std::uint64_t, DcLabRecorder*> g_byGuid;
    std::unordered_map<void const*, std::pair<DcLabRecorder*, std::uint64_t>> g_byContext;

    DcLabRecorder* ByGuidLocked(std::uint64_t g)
    {
        auto it = g_byGuid.find(g);
        return it == g_byGuid.end() ? nullptr : it->second;
    }

    void TapPhase(void const* ctx, std::uint32_t from, std::uint32_t to, std::uint32_t nowMs)
    {
        std::shared_lock lock(g_hubMutex);
        auto it = g_byContext.find(ctx);
        if (it != g_byContext.end())
            it->second.first->OnPhase(it->second.second, from, to, nowMs);
    }

    void TapVerdict(std::uint64_t guid, char const* verdict, std::uint32_t predicted, std::uint32_t ceiling)
    {
        std::shared_lock lock(g_hubMutex);
        if (DcLabRecorder* r = ByGuidLocked(guid))
            r->OnVerdict(guid, verdict, predicted, ceiling);
    }

    void TapPassive(std::uint64_t guid, bool on)
    {
        std::shared_lock lock(g_hubMutex);
        if (DcLabRecorder* r = ByGuidLocked(guid))
            r->OnPassive(guid, on);
    }

    DcLabTap::Hooks const g_tapHooks{&TapPhase, &TapVerdict, &TapPassive};

    // A pet/guardian's damage is its owner's for every oracle question (a
    // hunter's pet opening on a pack IS the hunter opening on it).
    std::uint64_t Attribute(Unit* u)
    {
        if (!u)
            return 0;
        if (Player* owner = u->GetCharmerOrOwnerPlayerOrPlayerItself())
            return owner->GetGUID().GetRawValue();
        return u->GetGUID().GetRawValue();
    }

    std::string GitDescribe(std::string const& dir)
    {
        if (dir.empty() || !std::filesystem::exists(dir + "/.git"))
            return "";
        // The worldserver runs as root under systemd with no HOME, and the
        // checkout belongs to the developer: without safe.directory git refuses
        // the repo as "dubious ownership" and every trace header went blank.
        std::string const cmd = "HOME=\"${HOME:-/root}\" git -c safe.directory='*' -C '" + dir +
                                "' describe --always --dirty --abbrev=10 2>/dev/null";
        std::string out;
        if (FILE* p = popen(cmd.c_str(), "r"))
        {
            char buf[128];
            while (fgets(buf, sizeof(buf), p))
                out += buf;
            pclose(p);
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
            out.pop_back();
        return out;
    }

    std::string ModuleDir()
    {
        std::string f = __FILE__;
        std::size_t const at = f.rfind("/src/Lab/");
        return at == std::string::npos ? std::string() : f.substr(0, at);
    }

    std::uint8_t HpPct(Unit const* u)
    {
        if (!u || !u->IsAlive() || !u->GetMaxHealth())
            return 0;
        return static_cast<std::uint8_t>(std::min<std::uint64_t>(100, u->GetHealth() * 100 / u->GetMaxHealth()));
    }
}

// ============================================================== DcLabHub

namespace DcLabHub
{
    bool Active() { return g_armedCount.load(std::memory_order_acquire) > 0; }

    void Register(DcLabRecorder* r, std::vector<std::uint64_t> const& guids,
                  std::vector<void const*> const& contexts)
    {
        std::unique_lock lock(g_hubMutex);
        for (std::uint64_t g : guids)
            g_byGuid[g] = r;
        for (std::size_t i = 0; i < contexts.size(); ++i)
            if (contexts[i])
                g_byContext[contexts[i]] = {r, guids.size() > i ? guids[i] : 0};
        if (g_armedCount.fetch_add(1) == 0)
            DcLabTap::Install(&g_tapHooks);
    }

    void AddGuid(DcLabRecorder* r, std::uint64_t guid)
    {
        std::unique_lock lock(g_hubMutex);
        g_byGuid[guid] = r;
    }

    void Unregister(DcLabRecorder* r)
    {
        std::unique_lock lock(g_hubMutex);
        bool had = false;
        for (auto it = g_byGuid.begin(); it != g_byGuid.end();)
        {
            if (it->second == r)
            {
                it = g_byGuid.erase(it);
                had = true;
            }
            else
                ++it;
        }
        for (auto it = g_byContext.begin(); it != g_byContext.end();)
        {
            if (it->second.first == r)
                it = g_byContext.erase(it);
            else
                ++it;
        }
        (void)had;
        if (g_armedCount.fetch_sub(1) == 1)
            DcLabTap::Install(nullptr);
    }

    void Damage(Unit* attacker, Unit* victim, std::uint32_t amount)
    {
        if (!Active() || !attacker || !victim || !amount)
            return;
        std::uint64_t const a = Attribute(attacker);
        std::uint64_t const v = victim->GetGUID().GetRawValue();
        std::shared_lock lock(g_hubMutex);
        DcLabRecorder* r = ByGuidLocked(a);
        if (!r)
            r = ByGuidLocked(v);
        if (r)
            r->OnDamage(a, v, amount);
    }

    void Heal(Unit* healer, Unit* /*target*/, std::uint32_t amount)
    {
        if (!Active() || !healer || !amount)
            return;
        std::uint64_t const h = Attribute(healer);
        std::shared_lock lock(g_hubMutex);
        if (DcLabRecorder* r = ByGuidLocked(h))
            r->OnHeal(h, amount);
    }

    void Engage(Unit* unit, Unit* victim)
    {
        if (!Active() || !unit || !victim || !unit->IsCreature())
            return;
        std::uint64_t const v = Attribute(victim);
        std::shared_lock lock(g_hubMutex);
        if (DcLabRecorder* r = ByGuidLocked(v))
            if (r->IsParty(v))
                r->OnEngage(unit->GetGUID().GetRawValue(), v);
    }

    void Action(std::uint64_t guid, std::uint8_t engine, std::string const& action, float relevance,
                bool executed, std::string const& source)
    {
        std::shared_lock lock(g_hubMutex);
        if (DcLabRecorder* r = ByGuidLocked(guid))
            r->OnAction(guid, engine, action, relevance, executed, source);
    }

    std::string ModuleSha()
    {
        static std::string const sha = GitDescribe(ModuleDir());
        return sha;
    }

    std::string PlayerbotsSha()
    {
        static std::string const sha = []
        {
            std::string const dir = ModuleDir();
            std::size_t const at = dir.rfind('/');
            return at == std::string::npos ? std::string() : GitDescribe(dir.substr(0, at) + "/mod-playerbots");
        }();
        return sha;
    }
}

// ============================================================== recorder

DcLabRecorder::DcLabRecorder(DcLab::Header header)
{
    _trace.header = std::move(header);
}

DcLabRecorder::~DcLabRecorder()
{
    Disarm();
}

std::uint32_t DcLabRecorder::NowMs() const
{
    return _armMs ? getMSTimeDiff(_armMs, getMSTime()) : 0;
}

bool DcLabRecorder::IsParty(std::uint64_t g) const
{
    // _party is written only before Arm (world thread) — immutable while hooks run.
    return _party.count(g) != 0;
}

void DcLabRecorder::AddMember(Player* p, std::string const& role, bool human, bool leader,
                              std::string const& react)
{
    if (!p)
        return;
    DcLab::Member m;
    m.guid = p->GetGUID().GetRawValue();
    m.name = p->GetName();
    m.role = role;
    m.human = human;
    m.leader = leader;
    m.react = react;
    switch (p->getClass())
    {
        case CLASS_WARRIOR: m.cls = "warrior"; break;
        case CLASS_PALADIN: m.cls = "paladin"; break;
        case CLASS_HUNTER: m.cls = "hunter"; break;
        case CLASS_ROGUE: m.cls = "rogue"; break;
        case CLASS_PRIEST: m.cls = "priest"; break;
        case CLASS_DEATH_KNIGHT: m.cls = "dk"; break;
        case CLASS_SHAMAN: m.cls = "shaman"; break;
        case CLASS_MAGE: m.cls = "mage"; break;
        case CLASS_WARLOCK: m.cls = "warlock"; break;
        case CLASS_DRUID: m.cls = "druid"; break;
        default: break;
    }
    _trace.header.party.push_back(m);
    _party.insert(m.guid);
    if (leader)
        _leader = m.guid;
}

void DcLabRecorder::AddUnit(Creature* c, std::string const& pack)
{
    if (!c)
        return;
    std::uint64_t const g = c->GetGUID().GetRawValue();
    {
        std::lock_guard<std::mutex> lock(_mu);
        if (!_units.insert(g).second)
        {
            // Re-registration only upgrades the pack tag.
            if (!pack.empty())
                for (DcLab::Unit& u : _trace.units)
                    if (u.guid == g)
                        u.pack = pack;
            return;
        }
        DcLab::Unit u;
        u.guid = g;
        u.entry = c->GetEntry();
        u.spawnId = c->GetSpawnId();
        u.name = c->GetName();
        u.pack = pack;
        u.boss = c->IsDungeonBoss() || c->isWorldBoss();
        _trace.units.push_back(u);
    }
    if (_armed)
        DcLabHub::AddGuid(this, g);
}

Player* DcLabRecorder::FindLeader() const
{
    return _leader ? ObjectAccessor::FindPlayer(ObjectGuid(_leader)) : nullptr;
}

void DcLabRecorder::Arm()
{
    if (_armed)
        return;
    _armMs = getMSTime();
    if (!_armMs)
        _armMs = 1;
    _trace.header.moduleSha = DcLabHub::ModuleSha();
    _trace.header.playerbotsSha = DcLabHub::PlayerbotsSha();

    std::vector<std::uint64_t> guids;
    std::vector<void const*> contexts;
    for (std::uint64_t g : _party)
    {
        guids.push_back(g);
        Player* p = ObjectAccessor::FindPlayer(ObjectGuid(g));
        PlayerbotAI* ai = p ? GET_PLAYERBOT_AI(p) : nullptr;
        if (!ai)
        {
            contexts.push_back(nullptr);
            continue;
        }
        contexts.push_back(&ai->GetAiObjectContext()->GetValue<DcPullContext&>(DcKey::PullContext)->Get());
        for (std::uint8_t s = 0; s < BOT_STATE_MAX; ++s)
            if (Engine* e = DcEngineAccess::Get(ai, static_cast<BotState>(s)))
            {
                auto* l = new DcLabActionListener(g, s);
                e->AddActionExecutionListener(l);
                _listeners.push_back({g, ai, e, l});
            }
    }
    {
        std::lock_guard<std::mutex> lock(_mu);
        for (std::uint64_t g : _units)
            guids.push_back(g);
    }
    _contexts = contexts;
    DcLabHub::Register(this, guids, contexts);
    _armed = true;
    SampleFrame();
}

void DcLabRecorder::Disarm()
{
    if (!_armed)
        return;
    _armed = false;
    DcLabHub::Unregister(this);  // after this no hook can reach us

    for (ListenerRec const& rec : _listeners)
    {
        auto* l = static_cast<DcLabActionListener*>(rec.listener);
        Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid(rec.guid));
        PlayerbotAI* ai = p ? GET_PLAYERBOT_AI(p) : nullptr;
        // Same AI, same engine: the listener is still in that engine's list and
        // is ours to remove and free. Anything else means the AI (and with it
        // the engine's list, which deletes its listeners) is gone already.
        if (ai && ai == rec.ai)
        {
            bool found = false;
            for (std::uint8_t s = 0; s < BOT_STATE_MAX; ++s)
                if (DcEngineAccess::Get(ai, static_cast<BotState>(s)) == rec.engine)
                    found = true;
            if (found)
            {
                rec.engine->removeActionExecutionListener(l);
                delete l;
            }
        }
    }
    _listeners.clear();
}

void DcLabRecorder::Tick(std::uint32_t diff)
{
    if (!_armed)
        return;
    _frameAccum += diff;
    if (_frameAccum < DcLab::kFrameMs)
        return;
    _frameAccum = 0;
    SampleFrame();
}

void DcLabRecorder::End(std::string const& reason)
{
    if (_ended)
        return;
    _ended = true;
    if (_armed)
        SampleFrame();
    {
        std::lock_guard<std::mutex> lock(_mu);
        _trace.endMs = NowMs();
        _trace.endReason = reason;
    }
    Disarm();
}

void DcLabRecorder::AddEvent(char const* ev, std::uint64_t a, std::uint64_t b, std::int64_t v,
                             std::string const& s, std::string const& s2)
{
    std::lock_guard<std::mutex> lock(_mu);
    DcLab::Event e;
    e.t = NowMs();
    e.ev = ev;
    e.a = a;
    e.b = b;
    e.v = v;
    e.s = s;
    e.s2 = s2;
    _trace.events.push_back(std::move(e));
}

void DcLabRecorder::PushPullEvent(std::uint32_t t, std::string const& ev)
{
    _pullEvents.emplace_back(t, ev);
}

std::vector<std::pair<std::uint32_t, std::string>> DcLabRecorder::DrainPullEvents()
{
    std::lock_guard<std::mutex> lock(_mu);
    std::vector<std::pair<std::uint32_t, std::string>> out;
    out.swap(_pullEvents);
    return out;
}

void DcLabRecorder::Discover(Player* leader)
{
    if (_discoverRadius <= 0.0f || !leader || !leader->IsInWorld())
        return;
    std::list<Creature*> nearby;
    Acore::AnyUnitInObjectRangeCheck check(leader, _discoverRadius);
    Acore::CreatureListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(leader, nearby, check);
    Cell::VisitObjects(leader, searcher, _discoverRadius);
    for (Creature* c : nearby)
    {
        if (!c || !c->IsAlive() || c->IsPet() || c->IsTotem() || !c->IsHostileTo(leader))
            continue;
        bool known;
        {
            std::lock_guard<std::mutex> lock(_mu);
            known = _units.count(c->GetGUID().GetRawValue()) != 0;
        }
        if (!known)
            AddUnit(c, _packOf ? _packOf(c) : std::string());
    }
}

void DcLabRecorder::SampleFrame()
{
    Player* const leader = FindLeader();
    Discover(leader);

    DcLab::Frame f;
    std::vector<std::uint64_t> units;
    {
        std::lock_guard<std::mutex> lock(_mu);
        f.t = NowMs();
        units.assign(_units.begin(), _units.end());
    }

    for (std::uint64_t g : _party)
    {
        DcLab::BotSample b;
        b.guid = g;
        if (Player* p = ObjectAccessor::FindPlayer(ObjectGuid(g)))
        {
            b.x = p->GetPositionX();
            b.y = p->GetPositionY();
            b.z = p->GetPositionZ();
            b.alive = p->IsAlive();
            b.hpPct = HpPct(p);
            b.inCombat = p->IsInCombat();
            b.victim = p->GetVictim() ? p->GetVictim()->GetGUID().GetRawValue() : 0;
            b.casting = p->IsNonMeleeSpellCast(false);
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
            {
                b.engine = static_cast<std::uint8_t>(ai->GetState());
                if (Unit* t = ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Get())
                    b.target = t->GetGUID().GetRawValue();
                b.passive = ai->HasStrategy("passive", BOT_STATE_COMBAT);
                b.stay = ai->HasStrategy("stay", BOT_STATE_COMBAT) || ai->HasStrategy("stay", BOT_STATE_NON_COMBAT);
            }
            else
            {
                b.engine = DcLab::kEngineNone;
                b.target = p->GetTarget().GetRawValue();
            }
        }
        f.bots.push_back(b);
    }

    if (leader && leader->IsInWorld())
    {
        for (std::uint64_t g : units)
        {
            DcLab::UnitSample s;
            s.guid = g;
            Creature* c = ObjectAccessor::GetCreature(*leader, ObjectGuid(g));
            if (!c)
            {
                // Despawned / corpse removed: carry the last position, dead.
                for (auto it = _trace.frames.rbegin(); it != _trace.frames.rend(); ++it)
                {
                    auto u = std::find_if(it->units.begin(), it->units.end(),
                                          [g](DcLab::UnitSample const& x) { return x.guid == g; });
                    if (u != it->units.end())
                    {
                        s.x = u->x;
                        s.y = u->y;
                        s.z = u->z;
                        break;
                    }
                }
                f.units.push_back(s);
                continue;
            }
            s.x = c->GetPositionX();
            s.y = c->GetPositionY();
            s.z = c->GetPositionZ();
            s.alive = c->IsAlive();
            s.hpPct = HpPct(c);
            s.inCombat = c->IsInCombat();
            s.evading = c->IsInEvadeMode();
            s.moving = c->isMoving();
            s.motion = static_cast<std::uint8_t>(c->GetMotionMaster()->GetCurrentMovementGeneratorType());
            s.victim = c->GetVictim() ? Attribute(c->GetVictim()) : 0;
            if (Unit* top = c->GetThreatMgr().GetLastVictim())
                s.threat = Attribute(top);
            f.units.push_back(s);
        }

        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(leader))
        {
            AiObjectContext* ctx = ai->GetAiObjectContext();
            DcPullContext const& pull = ctx->GetValue<DcPullContext&>(DcKey::PullContext)->Get();
            DcLab::DcSample& d = f.dc;
            d.valid = true;
            d.phase = static_cast<std::uint32_t>(pull.phase);
            d.decision = static_cast<std::uint32_t>(pull.decision);
            d.decisionSeq = pull.decisionSeq;
            d.predicted = pull.predictedCount;
            d.ceiling = pull.predictedCeiling;
            d.campX = pull.camp.GetPositionX();
            d.campY = pull.camp.GetPositionY();
            d.campZ = pull.camp.GetPositionZ();
            d.pullTarget = pull.pullTarget.GetRawValue();
            d.tagTarget = pull.tagTarget.GetRawValue();
            d.abortTarget = pull.abortTarget.GetRawValue();
            d.partyReleased = pull.partyReleased;
            d.losPull = pull.losPull;
            d.scoutAggro = pull.scoutAggroMs != 0 && getMSTimeDiff(pull.scoutAggroMs, getMSTime()) < 3000;
            d.enabled = DcRun::Of(ctx).enabled;
            d.paused = DcRun::Of(ctx).paused;
        }
    }

    std::lock_guard<std::mutex> lock(_mu);
    for (DcLab::BotSample& b : f.bots)
    {
        b.dmgDone = _dmgDone[b.guid];
        b.healDone = _healDone[b.guid];
    }

    // Edges: combat on/off and deaths, for party and creatures alike.
    auto edge = [&](std::uint64_t g, bool inCombat, bool alive, std::uint64_t victim)
    {
        auto c = _lastCombat.find(g);
        if (c != _lastCombat.end() && c->second != inCombat)
        {
            DcLab::Event e;
            e.t = f.t;
            e.ev = inCombat ? DcLab::Ev::CombatOn : DcLab::Ev::CombatOff;
            e.a = g;
            e.b = victim;
            _trace.events.push_back(e);
        }
        _lastCombat[g] = inCombat;
        auto a = _lastAlive.find(g);
        if (a != _lastAlive.end() && a->second && !alive)
        {
            DcLab::Event e;
            e.t = f.t;
            e.ev = DcLab::Ev::Death;
            e.a = g;
            _trace.events.push_back(e);
        }
        _lastAlive[g] = alive;
    };
    for (DcLab::BotSample const& b : f.bots)
        edge(b.guid, b.inCombat, b.alive, b.victim);
    for (DcLab::UnitSample const& u : f.units)
        edge(u.guid, u.inCombat, u.alive, u.victim);

    // Pull-state fallbacks for writes the tap cannot see (DcPullContext::Reset
    // assigns a fresh struct), and the safety-release edge.
    if (f.dc.valid)
    {
        if (_havePhase && f.dc.phase != _lastPhase)
        {
            DcLab::Event e;
            e.t = f.t;
            e.ev = DcLab::Ev::Phase;
            e.a = _leader;
            e.s = std::string(DcLab::PhaseName(_lastPhase)) + ">" + DcLab::PhaseName(f.dc.phase);
            e.s2 = "sample";
            _trace.events.push_back(e);
            PushPullEvent(f.t, std::string("phase:") + DcLab::PhaseName(f.dc.phase));
        }
        _lastPhase = f.dc.phase;
        _havePhase = true;
        if (f.dc.partyReleased && !_lastReleased)
        {
            DcLab::Event e;
            e.t = f.t;
            e.ev = DcLab::Ev::SafetyRelease;
            e.a = _leader;
            _trace.events.push_back(e);
            PushPullEvent(f.t, DcLab::Ev::SafetyRelease);
        }
        _lastReleased = f.dc.partyReleased;
    }
    _trace.frames.push_back(std::move(f));
}

std::string DcLabRecorder::WriteFile(std::string const& dir) const
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string const path = dir + "/" + (_trace.header.runId.empty() ? "trace" : _trace.header.runId) + ".jsonl";
    std::ofstream out(path, std::ios::trunc);
    if (!out)
    {
        LOG_ERROR("playerbots.dungeonclear", "LAB could not write trace {}", path);
        return "";
    }
    out << DcLab::ToJsonl(_trace);
    return path;
}

// ------------------------------------------------------------- hub callbacks

void DcLabRecorder::OnDamage(std::uint64_t attacker, std::uint64_t victim, std::uint32_t amount)
{
    std::lock_guard<std::mutex> lock(_mu);
    if (_party.count(attacker) && !_party.count(victim))
        _dmgDone[attacker] += amount;
    if (_firstDmg.insert({attacker, victim}).second)
    {
        DcLab::Event e;
        e.t = NowMs();
        e.ev = DcLab::Ev::FirstDmg;
        e.a = attacker;
        e.b = victim;
        e.v = amount;
        _trace.events.push_back(e);
    }
}

void DcLabRecorder::OnHeal(std::uint64_t healer, std::uint32_t amount)
{
    std::lock_guard<std::mutex> lock(_mu);
    if (_party.count(healer))
        _healDone[healer] += amount;
}

void DcLabRecorder::OnEngage(std::uint64_t creature, std::uint64_t victim)
{
    std::lock_guard<std::mutex> lock(_mu);
    if (!_engaged.insert(creature).second)
        return;
    DcLab::Event e;
    e.t = NowMs();
    e.ev = "engage";
    e.a = creature;
    e.b = victim;
    _trace.events.push_back(e);
}

void DcLabRecorder::OnPhase(std::uint64_t leader, std::uint32_t from, std::uint32_t to, std::uint32_t /*nowMs*/)
{
    // Only the leader's FSM drives the pull; a follower's own copy never moves.
    if (leader != _leader)
        return;
    std::lock_guard<std::mutex> lock(_mu);
    std::uint32_t const t = NowMs();
    DcLab::Event e;
    e.t = t;
    e.ev = DcLab::Ev::Phase;
    e.a = leader;
    e.s = std::string(DcLab::PhaseName(from)) + ">" + DcLab::PhaseName(to);
    e.s2 = "tap";
    _trace.events.push_back(e);
    PushPullEvent(t, std::string("phase:") + DcLab::PhaseName(to));
    // The derived names injections key on.
    char const* derived = nullptr;
    if (from == 0 && (to == 1 || to == 2))
        derived = DcLab::Ev::Commit;
    else if (from == 2 && to == 3)
        derived = DcLab::Ev::AggroConfirmed;
    else if (from == 3 && to == 4)
        derived = DcLab::Ev::CampReached;
    if (derived)
    {
        DcLab::Event d;
        d.t = t;
        d.ev = derived;
        d.a = leader;
        _trace.events.push_back(d);
        PushPullEvent(t, derived);
    }
    // Keep the sampled fallback from double-reporting this transition.
    _lastPhase = to;
    _havePhase = true;
}

void DcLabRecorder::OnVerdict(std::uint64_t leader, char const* verdict, std::uint32_t predicted,
                              std::uint32_t ceiling)
{
    if (!verdict)
        return;
    std::lock_guard<std::mutex> lock(_mu);
    std::string& last = _lastVerdict[leader];
    if (last == verdict)
        return;
    last = verdict;
    DcLab::Event e;
    e.t = NowMs();
    e.ev = DcLab::Ev::Verdict;
    e.a = leader;
    e.v = static_cast<std::int64_t>(predicted) * 1000 + ceiling;
    e.s = verdict;
    _trace.events.push_back(e);
    PushPullEvent(e.t, std::string("verdict:") + verdict);
}

void DcLabRecorder::OnPassive(std::uint64_t guid, bool on)
{
    std::lock_guard<std::mutex> lock(_mu);
    DcLab::Event e;
    e.t = NowMs();
    e.ev = DcLab::Ev::Passive;
    e.a = guid;
    e.v = on ? 1 : 0;
    _trace.events.push_back(e);
    if (!on)
        PushPullEvent(e.t, "followerReleased");
}

void DcLabRecorder::OnAction(std::uint64_t guid, std::uint8_t engine, std::string action, float relevance,
                             bool executed, std::string source)
{
    std::lock_guard<std::mutex> lock(_mu);
    DcLab::ActionRec a;
    a.t = NowMs();
    a.guid = guid;
    a.engine = engine;
    a.action = std::move(action);
    a.relevance = relevance;
    a.executed = executed;
    a.source = std::move(source);
    _trace.actions.push_back(std::move(a));
}
