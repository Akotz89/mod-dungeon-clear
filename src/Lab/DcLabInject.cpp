/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Lab/DcLabInject.h"

#include <cmath>
#include <limits>

#include "Creature.h"
#include "CreatureAI.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellAuras.h"
#include "TemporarySummon.h"

#include "Playerbots.h"
#include "PlayerbotAI.h"

#include "Ai/Dungeon/DungeonClear/DcPullContext.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Lab/DcLabJob.h"
#include "Lab/DcLabPuppet.h"
#include "Lab/DcLabRecorder.h"

namespace
{
    // Default auras for the control ops. Applied with AddAura (no cast, no
    // caster resources), so any of them lands on any unit; a scenario may name
    // its own with "spell".
    constexpr uint32 kSpellStun = 853;   // Hammer of Justice (rank 1), 3s stun
    constexpr uint32 kSpellRoot = 122;   // Frost Nova (rank 1), 8s root
    constexpr uint32 kSpellFear = 5782;  // Fear (rank 1)
    constexpr uint32 kSpellDaze = 1604;  // Dazed
    // A closed portcullis (display 411, Portcullisactive.m2) has a collision
    // model in GameObjectModels, so it blocks dynamic line of sight. It does not
    // change the navmesh: a "wall here" what-if for LOS only.
    constexpr uint32 kLosBlockerEntry = 18972;
    constexpr uint32 kLosBlockerLifeS = 600;
}

// ================================================================ injector

DcLabInjector::DcLabInjector(DcLabJob& job) : _job(job)
{
    for (DcLabScenario::Injection const& inj : job.Run()->scenario.injections)
        _triggers.emplace_back(inj.when);
}

std::vector<Unit*> DcLabInjector::Resolve(DcLabJob& job, std::string const& ref)
{
    std::vector<Unit*> out;
    Player* tank = job.Tank();
    if (!tank || !tank->IsInWorld() || ref.empty())
        return out;
    Map* map = tank->GetMap();
    DcLabScenario::Scenario const& sc = job.Run()->scenario;

    auto member = [&](std::size_t i) { if (Player* p = job.Member(i)) out.push_back(p); };
    auto nthRole = [&](char const* role, std::size_t n)
    {
        std::size_t seen = 0;
        for (std::size_t i = 0; i < job.Roles().size(); ++i)
            if (std::string(job.Roles()[i]) == role && seen++ == n)
                return member(i);
    };

    if (ref == "tank")
        member(0);
    else if (ref == "heal" || ref == "healer")
        nthRole("heal", 0);
    else if (ref == "dps")
        nthRole("dps", 0);
    else if (ref.size() > 3 && ref.rfind("dps", 0) == 0 && std::isdigit(static_cast<unsigned char>(ref[3])))
        nthRole("dps", static_cast<std::size_t>(std::max(1, std::atoi(ref.c_str() + 3)) - 1));
    else if (ref.rfind("slot:", 0) == 0)
        member(static_cast<std::size_t>(std::atoi(ref.c_str() + 5)));
    else if (ref == "human")
    {
        if (sc.human.slot > 0)
            member(static_cast<std::size_t>(sc.human.slot));
    }
    else if (ref == "party")
        for (std::size_t i = 0; i < job.Members().size(); ++i)
            member(i);
    else if (ref == "pullTarget")
    {
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(tank))
        {
            DcPullContext const& pull = ai->GetAiObjectContext()->GetValue<DcPullContext&>(DcKey::PullContext)->Get();
            ObjectGuid const g = pull.pullTarget ? pull.pullTarget : pull.decisionTarget;
            if (Creature* c = g ? map->GetCreature(g) : nullptr)
                out.push_back(c);
        }
    }
    else if (ref == "nearest" || ref == "nearestIdle")
    {
        Creature* best = nullptr;
        float bestD = std::numeric_limits<float>::max();
        if (DcLabRecorder* rec = job.Recorder())
            for (DcLab::Unit const& u : rec->View().units)
                if (Creature* c = map->GetCreature(ObjectGuid(u.guid)))
                    if (c->IsAlive() && !c->IsFriendlyTo(tank) && (ref == "nearest" || !c->IsInCombat()))
                        if (float const d = c->GetDistance(tank); d < bestD)
                        {
                            bestD = d;
                            best = c;
                        }
        if (best)
            out.push_back(best);
    }
    else if (ref.rfind("spawn:", 0) == 0)
    {
        uint64 const id = std::strtoull(ref.c_str() + 6, nullptr, 10);
        auto range = map->GetCreatureBySpawnIdStore().equal_range(id);
        for (auto it = range.first; it != range.second; ++it)
            out.push_back(it->second);
    }
    else
    {
        std::string const tag = ref.rfind("pack:", 0) == 0 ? ref.substr(5) : ref;
        auto syn = job.Synthetic().find(tag);
        if (syn != job.Synthetic().end())
        {
            if (Creature* c = map->GetCreature(syn->second))
                out.push_back(c);
            return out;
        }
        // Synthetic actors summoned into this pack count as its members too.
        for (DcLabScenario::Synthetic const& syn : sc.synthetic)
            if (syn.pack == tag)
            {
                auto it = job.Synthetic().find(syn.tag);
                if (it != job.Synthetic().end())
                    if (Creature* c = map->GetCreature(it->second))
                        if (c->IsAlive())
                            out.push_back(c);
            }
        auto pack = sc.packs.find(tag);
        if (pack != sc.packs.end())
            for (uint64 id : pack->second)
            {
                auto range = map->GetCreatureBySpawnIdStore().equal_range(id);
                for (auto it = range.first; it != range.second; ++it)
                    if (it->second->IsAlive())
                        out.push_back(it->second);
            }
    }
    return out;
}

double DcLabInjector::Measure(DcLabJob& job, std::string const& lhs)
{
    Player* tank = job.Tank();
    double const nan = std::numeric_limits<double>::quiet_NaN();
    if (!tank)
        return nan;
    if (lhs == "elapsedS")
        return job.RunElapsedMs() / 1000.0;
    if (lhs == "tankHp")
        return tank->GetHealthPct();
    if (lhs == "partyMinHp")
    {
        double m = 100.0;
        for (std::size_t i = 0; i < job.Members().size(); ++i)
            if (Player* p = job.Member(i))
                m = std::min<double>(m, p->IsAlive() ? p->GetHealthPct() : 0.0);
        return m;
    }
    if (lhs == "tankDistToCamp")
    {
        PlayerbotAI* ai = GET_PLAYERBOT_AI(tank);
        if (!ai)
            return nan;
        DcPullContext const& pull = ai->GetAiObjectContext()->GetValue<DcPullContext&>(DcKey::PullContext)->Get();
        return pull.HasCamp() ? tank->GetExactDist2d(pull.camp.GetPositionX(), pull.camp.GetPositionY()) : nan;
    }
    if (lhs == "tankDistToPack" || lhs == "packEngaged")
    {
        double best = std::numeric_limits<double>::max();
        uint32 engaged = 0;
        for (Unit* u : Resolve(job, "pack:target"))
        {
            best = std::min<double>(best, tank->GetExactDist2d(u));
            engaged += u->IsInCombat();
        }
        if (lhs == "packEngaged")
            return engaged;
        return best == std::numeric_limits<double>::max() ? nan : best;
    }
    return nan;
}

bool DcLabInjector::Evaluate(DcLabJob& job, DcLabScenario::When const& w)
{
    double const v = Measure(job, w.lhs);
    if (std::isnan(v))
        return false;
    if (w.op == "<")
        return v < w.rhs;
    if (w.op == "<=")
        return v <= w.rhs;
    if (w.op == ">")
        return v > w.rhs;
    return v >= w.rhs;
}

void DcLabInjector::Tick(std::vector<std::pair<uint32, std::string>> const& events)
{
    DcLabRecorder* rec = _job.Recorder();
    if (!rec)
        return;
    uint32 const now = rec->NowMs();
    std::vector<DcLabScenario::Injection> const& injs = _job.Run()->scenario.injections;
    for (std::size_t i = 0; i < _triggers.size(); ++i)
    {
        DcLabTrigger& t = _triggers[i];
        if (t.Fired())
            continue;
        t.Feed(events);
        bool const pred =
            injs[i].when.kind == DcLabScenario::When::Kind::Predicate && Evaluate(_job, injs[i].when);
        if (t.Due(now, pred))
        {
            t.MarkFired();
            Fire(i);
        }
    }
    TickPatrols();
}

void DcLabInjector::TickPatrols()
{
    Player* tank = _job.Tank();
    if (!tank)
        return;
    for (Patrol& p : _patrols)
    {
        Creature* c = tank->GetMap()->GetCreature(p.mob);
        if (!c || !c->IsAlive() || c->IsInCombat() || p.next >= p.path.size())
            continue;
        Position const& dest = p.path[p.next];
        if (c->GetExactDist(&dest) < 1.5f)
        {
            if (++p.next < p.path.size())
                c->GetMotionMaster()->MovePoint(0, p.path[p.next]);
        }
        else if (!c->isMoving())
            c->GetMotionMaster()->MovePoint(0, dest);
    }
}

void DcLabInjector::Fire(std::size_t idx)
{
    DcLabScenario::Injection const& inj = _job.Run()->scenario.injections[idx];
    DcLabJson::Value const& a = inj.what.args;
    std::string const& op = inj.what.op;
    Player* tank = _job.Tank();
    DcLabRecorder* rec = _job.Recorder();
    if (!tank || !rec)
        return;
    Map* map = tank->GetMap();
    uint64 first = 0;
    std::string note;

    auto auraOp = [&](uint32 defSpell)
    {
        uint32 const spell = static_cast<uint32>(a["spell"].AsNumber(defSpell));
        for (Unit* u : Resolve(_job, a[op].AsString()))
        {
            Aura* aura = u->AddAura(spell, u);
            if (aura && a.Has("durationMs"))
                aura->SetDuration(static_cast<int32>(a["durationMs"].AsNumber()));
            first = first ? first : u->GetGUID().GetRawValue();
        }
    };

    if (op == "spawn")
    {
        std::string const tag = a["spawn"].AsString();
        for (DcLabScenario::Synthetic const& s : _job.Run()->scenario.synthetic)
        {
            if (s.tag != tag || _job.Synthetic().count(tag))
                continue;
            TempSummon* c = nullptr;
            {
                DcLabJob::SummonScope scope;
                c = map->SummonCreature(s.entry, Position(s.pos.x, s.pos.y, s.pos.z, s.pos.o));
            }
            if (!c)
            {
                note = "summon failed";
                break;
            }
            _job.Synthetic()[tag] = c->GetGUID();
            _job.TrackSummon(c->GetGUID());
            rec->AddUnit(c, s.pack);
            first = c->GetGUID().GetRawValue();
            if (a.Has("aggro"))
            {
                std::vector<Unit*> const victims = Resolve(_job, a["aggro"].AsString());
                if (!victims.empty())
                    c->EngageWithTarget(victims.front());
            }
        }
    }
    else if (op == "aggro")
    {
        std::vector<Unit*> const victims = Resolve(_job, a["on"].AsString("heal"));
        if (!victims.empty())
            for (Unit* u : Resolve(_job, a["aggro"].AsString()))
                if (Creature* c = u->ToCreature())
                {
                    c->EngageWithTarget(victims.front());
                    first = first ? first : c->GetGUID().GetRawValue();
                }
    }
    else if (op == "startPatrol")
    {
        std::vector<Position> path;
        for (DcLabJson::Value const& p : a["path"].arr)
            if (p.IsArray() && p.arr.size() >= 3)
                path.emplace_back(p.arr[0].AsNumber(), p.arr[1].AsNumber(), p.arr[2].AsNumber());
        if (path.empty())
        {
            DcLabScenario::Vec4 const& st = _job.Run()->scenario.start;
            path.emplace_back(st.x, st.y, st.z);
        }
        std::string const who = a["startPatrol"].IsNumber() ? "spawn:" + a["startPatrol"].AsString() : a["startPatrol"].AsString();
        for (Unit* u : Resolve(_job, who))
            if (Creature* c = u->ToCreature())
            {
                c->GetMotionMaster()->Clear();
                c->GetMotionMaster()->MovePoint(0, path.front());
                _patrols.push_back({c->GetGUID(), path, 0});
                first = first ? first : c->GetGUID().GetRawValue();
            }
    }
    else if (op == "forceCallForHelp")
    {
        float const radius = static_cast<float>(a["radius"].AsNumber(15.0));
        for (Unit* u : Resolve(_job, a["forceCallForHelp"].AsString()))
            if (Creature* c = u->ToCreature())
            {
                c->CallForHelp(radius);
                first = first ? first : c->GetGUID().GetRawValue();
            }
    }
    else if (op == "stun")
        auraOp(kSpellStun);
    else if (op == "root")
        auraOp(kSpellRoot);
    else if (op == "fear")
        auraOp(kSpellFear);
    else if (op == "daze")
        auraOp(kSpellDaze);
    else if (op == "knockback")
    {
        float const xy = static_cast<float>(a["speedXY"].AsNumber(10.0));
        float const z = static_cast<float>(a["speedZ"].AsNumber(5.0));
        std::vector<Unit*> const src = Resolve(_job, a["from"].AsString("pack:target"));
        for (Unit* u : Resolve(_job, a["knockback"].AsString()))
        {
            float sx = u->GetPositionX() + std::cos(u->GetOrientation());
            float sy = u->GetPositionY() + std::sin(u->GetOrientation());
            if (!src.empty())
            {
                sx = src.front()->GetPositionX();
                sy = src.front()->GetPositionY();
            }
            u->GetMotionMaster()->MoveKnockbackFrom(sx, sy, xy, z, true);
            first = first ? first : u->GetGUID().GetRawValue();
        }
    }
    else if (op == "evade")
    {
        for (Unit* u : Resolve(_job, a["evade"].AsString()))
            if (Creature* c = u->ToCreature())
                if (c->AI())
                {
                    c->AI()->EnterEvadeMode();
                    first = first ? first : c->GetGUID().GetRawValue();
                }
    }
    else if (op == "dropCombat")
    {
        for (Unit* u : Resolve(_job, a["dropCombat"].AsString()))
        {
            u->CombatStop(true);
            if (Creature* c = u->ToCreature())
                c->GetThreatMgr().ClearAllThreat();
            first = first ? first : u->GetGUID().GetRawValue();
        }
    }
    else if (op == "kill")
    {
        for (Unit* u : Resolve(_job, a["kill"].AsString()))
            if (u->IsAlive())
            {
                first = first ? first : u->GetGUID().GetRawValue();
                u->KillSelf();
            }
    }
    else if (op == "door")
    {
        uint64 const id = a["door"].AsU64();
        bool const open = a["state"].AsString("open") == "open";
        auto range = map->GetGameObjectBySpawnIdStore().equal_range(id);
        for (auto it = range.first; it != range.second; ++it)
        {
            it->second->SetGoState(open ? GO_STATE_ACTIVE : GO_STATE_READY);
            first = it->second->GetGUID().GetRawValue();
        }
        if (!first)
            note = "no door with spawnId " + std::to_string(id);
    }
    else if (op == "spawnLosBlocker")
    {
        DcLabJson::Value const& p = a["spawnLosBlocker"];
        if (p.IsArray() && p.arr.size() >= 3)
        {
            float const o = p.arr.size() > 3 ? static_cast<float>(p.arr[3].AsNumber()) : 0.0f;
            uint32 const entry = static_cast<uint32>(a["entry"].AsNumber(kLosBlockerEntry));
            if (GameObject* go = tank->SummonGameObject(entry, static_cast<float>(p.arr[0].AsNumber()),
                                                        static_cast<float>(p.arr[1].AsNumber()),
                                                        static_cast<float>(p.arr[2].AsNumber()), o, 0.0f, 0.0f,
                                                        std::sin(o / 2.0f), std::cos(o / 2.0f), kLosBlockerLifeS,
                                                        false, GO_SUMMON_TIMED_DESPAWN))
            {
                go->SetGoState(GO_STATE_READY);
                _job.TrackGo(go->GetGUID());
                first = go->GetGUID().GetRawValue();
            }
            else
                note = "blocker summon failed";
        }
    }
    else if (op == "setReact")
    {
        std::vector<ObjectGuid> who;
        for (Unit* u : Resolve(_job, a["setReact"].AsString("party")))
            if (u->IsPlayer())
                who.push_back(u->GetGUID());
        _job.SetReactFor(who, a["profile"].AsString("masterless"));
        note = a["profile"].AsString("masterless");
    }
    else if (op == "humanAction")
    {
        if (DcLabPuppet* puppet = _job.Puppet())
            puppet->Do(a);
        else
            note = "no human in this scenario";
    }
    else if (op == "note")
        note = a["note"].AsString();

    rec->AddEvent(DcLab::Ev::Inject, first, 0, static_cast<int64>(idx), inj.label, note.empty() ? op : op + ": " + note);
    LOG_INFO("playerbots.dungeonclear", "LAB {} inject {} at {}ms{}", _job.CurrentRunId(), inj.label, rec->NowMs(),
             note.empty() ? "" : " (" + note + ")");
}
