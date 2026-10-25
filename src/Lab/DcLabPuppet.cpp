/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Lab/DcLabPuppet.h"

#include <cmath>
#include <fstream>
#include <sstream>

#include "Group.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"

#include "Ai/Dungeon/DungeonClear/DcPullContext.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Lab/DcLabJob.h"
#include "Lab/DcLabPaths.h"
#include "Lab/DcLabRecorder.h"

DcLabPuppet::DcLabPuppet(DcLabJob& job, ObjectGuid human) : _job(job), _guid(human) {}

DcLabPuppet::~DcLabPuppet()
{
    Detach();
}

Player* DcLabPuppet::Human() const
{
    return ObjectAccessor::FindPlayer(_guid);
}

bool DcLabPuppet::LoadScript(std::string const& name, std::string* err)
{
    _steps.clear();
    if (name.empty())
        return true;  // a human that only does what injections tell it
    std::string const path = DcLabPaths::LabDir() + "/" + name + ".json";
    std::ifstream in(path);
    if (!in)
    {
        *err = "cannot open puppet script " + path;
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    DcLabJson::Value root;
    std::string perr;
    if (!DcLabJson::Parse(ss.str(), root, &perr))
    {
        *err = path + ":" + perr;
        return false;
    }
    for (DcLabJson::Value const& s : root["steps"].arr)
    {
        DcLabScenario::When w;
        if (s.Has("when") && !DcLabScenario::ParseWhen(s["when"], w, &perr))
        {
            *err = path + ": step " + std::to_string(_steps.size()) + ": " + perr;
            return false;
        }
        _steps.push_back({DcLabTrigger(w), s["do"]});
        _whens.push_back(w);
    }
    return true;
}

bool DcLabPuppet::Attach(std::string* err)
{
    if (_attached)
        return true;
    Player* human = Human();
    if (!human || !human->IsInWorld())
    {
        *err = "puppet character not in world";
        return false;
    }
    // Whichever holder logged this character in: the GM's own PlayerbotMgr for
    // an in-game issuer, the random-bot holder for the headless driver.
    Player* gm = ObjectAccessor::FindConnectedPlayer(_job.Gm());
    PlayerbotHolder* holder = gm ? GET_PLAYERBOT_MGR(gm) : nullptr;
    if (!holder || !holder->GetPlayerBot(_guid))
        holder = &sRandomPlayerbotMgr;
    if (!holder->GetPlayerBot(_guid))
    {
        *err = "puppet character is not filed under any playerbot holder";
        return false;
    }
    _holder = holder;
    holder->DisablePlayerBot(_guid);
    if (GET_PLAYERBOT_AI(human))
    {
        *err = "DisablePlayerBot left an AI attached";
        return false;
    }
    _attached = true;

    // A real session's shape: the human leads, and is every bot's master.
    if (Group* group = human->GetGroup())
        if (_job.Run() && _job.Run()->scenario.human.leads && !group->IsLeader(_guid))
            group->ChangeLeader(_guid);
    for (std::size_t i = 0; i < _job.Members().size(); ++i)
        if (Player* p = _job.Member(i))
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
                ai->SetMaster(human);

    LOG_INFO("playerbots.dungeonclear", "LAB {} puppet {} attached (AI detached, leads the group)",
             _job.CurrentRunId(), human->GetName());
    return true;
}

void DcLabPuppet::Detach()
{
    if (!_attached)
        return;
    _attached = false;
    Player* human = Human();
    if (!human || !_holder)
        return;
    human->AttackStop();
    human->GetMotionMaster()->Clear();
    // Leadership back to the tank before the AI returns: a bot that comes back
    // leading a group would run the leader paths on its first tick.
    if (Group* group = human->GetGroup())
        if (group->IsLeader(_guid) && !_job.Members().empty())
            group->ChangeLeader(_job.Members().front());
    _holder->OnBotLogin(human);
    LOG_INFO("playerbots.dungeonclear", "LAB puppet {} detached (AI restored)", human->GetName());
}

void DcLabPuppet::CompleteTeleport()
{
    if (!_attached)
        return;
    Player* human = Human();
    if (!human || !human->IsInWorld() || !human->IsBeingTeleportedNear() || !human->GetSession())
        return;
    Player* mover = human->m_mover ? human->m_mover->ToPlayer() : nullptr;
    if (!mover)
        return;
    WorldPacket ack(MSG_MOVE_TELEPORT_ACK, 20);
    ack << mover->GetPackGUID();
    ack << uint32(0);  // flags
    ack << uint32(0);  // time
    human->GetSession()->HandleMoveTeleportAck(ack);
}

void DcLabPuppet::Reset()
{
    _attackTarget = ObjectGuid::Empty;
    _steps.clear();
    _whens.clear();
    if (Player* human = Human())
    {
        human->AttackStop();
        human->StopMoving();
        human->GetMotionMaster()->Clear();
    }
}

void DcLabPuppet::Tick(uint32 diff, std::vector<std::pair<uint32, std::string>> const& events)
{
    if (!_attached)
        return;
    Player* human = Human();
    if (!human || !human->IsAlive())
        return;

    // Bots drift off a human master the same way they drift off the GM one
    // (stock ResetAiAction); keep the session's shape.
    for (std::size_t i = 0; i < _job.Members().size(); ++i)
        if (Player* p = _job.Member(i))
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
                if (ai->GetMaster() != human)
                    ai->SetMaster(human);

    uint32 const now = _job.Recorder() ? _job.Recorder()->NowMs() : 0;
    for (std::size_t i = 0; i < _steps.size(); ++i)
    {
        Step& s = _steps[i];
        if (s.trigger.Fired())
            continue;
        s.trigger.Feed(events);
        bool const pred = _whens[i].kind == DcLabScenario::When::Kind::Predicate &&
                          DcLabInjector::Evaluate(_job, _whens[i]);
        if (s.trigger.Due(now, pred))
        {
            s.trigger.MarkFired();
            Apply(s.action);
        }
    }
    TickAttack(diff);
}

void DcLabPuppet::Do(DcLabJson::Value const& action)
{
    if (!_attached)
        return;
    // A humanAction injection names the action under "humanAction" and its
    // argument under "target"/"arg": {"humanAction": "attack", "target": "nearestIdle"}.
    DcLabJson::Value a = action;
    std::string const verb = action["humanAction"].AsString();
    DcLabJson::Value arg;
    arg.type = DcLabJson::Value::Type::String;
    arg.str = action["target"].AsString(action["arg"].AsString());
    if (action["arg"].IsNumber() || action["arg"].IsArray())
        arg = action["arg"];
    a.obj.erase("humanAction");
    a.obj[verb] = arg;
    Apply(a);
}

void DcLabPuppet::Apply(DcLabJson::Value const& a)
{
    Player* human = Human();
    if (!human)
        return;
    std::string what;
    if (a.Has("idle"))
    {
        human->AttackStop();
        human->StopMoving();
        human->GetMotionMaster()->Clear();
        _attackTarget = ObjectGuid::Empty;
        what = "idle";
    }
    else if (a.Has("follow"))
    {
        std::vector<Unit*> const t = DcLabInjector::Resolve(_job, a["follow"].AsString("tank"));
        if (!t.empty())
        {
            human->GetMotionMaster()->Clear();
            human->GetMotionMaster()->MoveFollow(t.front(), static_cast<float>(a["dist"].AsNumber(3.0)),
                                                 3.14159265f);
            what = "follow";
        }
    }
    else if (a.Has("attack"))
    {
        std::vector<Unit*> const t = DcLabInjector::Resolve(_job, a["attack"].AsString("nearestIdle"));
        if (!t.empty())
        {
            _attackTarget = t.front()->GetGUID();
            _attackSpell = static_cast<uint32>(a["spell"].AsNumber(0));
            _attackEveryMs = static_cast<uint32>(a["everyMs"].AsNumber(2000));
            _attackRange = static_cast<float>(a["range"].AsNumber(0));
            _attackAccum = _attackEveryMs;  // first cast on the next tick
            human->Attack(t.front(), _attackRange <= 0.0f);
            human->GetMotionMaster()->Clear();
            human->GetMotionMaster()->MoveChase(t.front(), _attackRange);
            what = "attack";
        }
    }
    else if (a.Has("moveTo"))
    {
        DcLabJson::Value const& p = a["moveTo"];
        if (p.IsArray() && p.arr.size() >= 3)
        {
            human->GetMotionMaster()->Clear();
            human->GetMotionMaster()->MovePoint(0, static_cast<float>(p.arr[0].AsNumber()),
                                                static_cast<float>(p.arr[1].AsNumber()),
                                                static_cast<float>(p.arr[2].AsNumber()));
            what = "moveTo";
        }
    }
    else if (a.Has("runAhead"))
    {
        float const yd = static_cast<float>(a["runAhead"].AsNumber(15.0));
        std::vector<Unit*> const pack = DcLabInjector::Resolve(_job, "pack:target");
        float o = human->GetOrientation();
        if (!pack.empty())
            o = human->GetAbsoluteAngle(pack.front());
        float const x = human->GetPositionX() + yd * std::cos(o);
        float const y = human->GetPositionY() + yd * std::sin(o);
        float z = human->GetMap()->GetHeight(human->GetPhaseMask(), x, y, human->GetPositionZ() + 2.0f, true, 10.0f);
        if (z <= INVALID_HEIGHT)
            z = human->GetPositionZ();
        human->GetMotionMaster()->Clear();
        human->GetMotionMaster()->MovePoint(0, x, y, z);
        what = "runAhead";
    }
    else if (a.Has("standAt"))
    {
        std::string const where = a["standAt"].AsString("camp");
        Position dest;
        bool have = false;
        if (where == "camp")
            if (Player* tank = _job.Tank())
                if (PlayerbotAI* ai = GET_PLAYERBOT_AI(tank))
                {
                    DcPullContext const& pull =
                        ai->GetAiObjectContext()->GetValue<DcPullContext&>(DcKey::PullContext)->Get();
                    if (pull.HasCamp())
                    {
                        dest = pull.camp;
                        have = true;
                    }
                }
        if (where == "start" && _job.Run())
        {
            DcLabScenario::Vec4 const& s = _job.Run()->scenario.start;
            dest.Relocate(s.x, s.y, s.z);
            have = true;
        }
        if (have)
        {
            human->GetMotionMaster()->Clear();
            human->GetMotionMaster()->MovePoint(0, dest);
            what = "standAt " + where;
        }
    }
    else if (a.Has("castAoe"))
    {
        human->CastSpell(human, static_cast<uint32>(a["castAoe"].AsNumber()), true);
        what = "castAoe";
    }
    if (!what.empty() && _job.Recorder())
        _job.Recorder()->AddEvent(DcLab::Ev::Note, _guid.GetRawValue(), _attackTarget.GetRawValue(), 0,
                                  "human:" + what, DcLabJson::Dump(a));
}

void DcLabPuppet::TickAttack(uint32 diff)
{
    if (!_attackTarget)
        return;
    Player* human = Human();
    Unit* target = human ? ObjectAccessor::GetUnit(*human, _attackTarget) : nullptr;
    if (!human || !target || !target->IsAlive())
    {
        _attackTarget = ObjectGuid::Empty;
        if (human)
            human->AttackStop();
        return;
    }
    if (human->GetVictim() != target)
        human->Attack(target, _attackRange <= 0.0f);
    _attackAccum += diff;
    if (_attackSpell && _attackAccum >= _attackEveryMs && !human->IsNonMeleeSpellCast(false))
    {
        _attackAccum = 0;
        human->CastSpell(target, _attackSpell, false);
    }
}
