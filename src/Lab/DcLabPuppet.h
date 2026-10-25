/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCLABPUPPET_H
#define _PLAYERBOT_DCLABPUPPET_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ObjectGuid.h"
#include "Lab/DcLabInject.h"
#include "Lab/DcLabJson.h"

class DcLabJob;
class Player;
class PlayerbotHolder;

// The "human" in a Lab party (pull-lab plan §4.5). Playerbots defines a real
// player as "no PlayerbotAI attached", so the puppet is one of the party's own
// pool characters with its AI detached through the public
// PlayerbotHolder::DisablePlayerBot: the AI is deleted (its destructor
// unregisters it from PlayerbotsMgr), the holder drops the bot from its map,
// and the Player stays in the world on its session. From then on every stock
// gate (real-player master, wait-for-attack, avoid-aoe, follow-master,
// master-threat ownership) and every DC human branch treats it as human.
//
// It leads the group and is every bot's master — a real session's shape.
// Detach() reverses it with the same holder's public OnBotLogin, which
// re-creates the AI and re-files the bot so the run's teardown logs it out
// normally. No playerbots file is touched.
//
// Known gap (documented in the plan): client-only behaviour — areatriggers a
// client fires, client movement packets, addon traffic — is not reproduced.
//
// Script (lab/humans/<name>.json):
//   { "about": "...", "steps": [ { "when": {...}, "do": { "follow": "tank", "dist": 4 } }, ... ] }
// Actions: follow <ref> [dist] | attack <ref> [spell, everyMs, range] |
//          moveTo [x,y,z] | runAhead <yd> | standAt camp|start | castAoe <spellId> | idle
class DcLabPuppet
{
public:
    DcLabPuppet(DcLabJob& job, ObjectGuid human);
    ~DcLabPuppet();

    bool LoadScript(std::string const& name, std::string* err);
    bool Attach(std::string* err);
    void Detach();
    bool Attached() const { return _attached; }
    // Between runs: stop whatever the last script left it doing.
    void Reset();
    // A near teleport only completes on the client's MSG_MOVE_TELEPORT_ACK. A
    // bot's AI sends that itself (PlayerbotAI::UpdateAI); the puppet has
    // neither AI nor client, so the Lab sends it — or the re-staging teleport
    // between runs never lands and the party never settles. Every tick.
    void CompleteTeleport();

    void Tick(std::uint32_t diff, std::vector<std::pair<std::uint32_t, std::string>> const& events);
    // A `humanAction` injection: the same action vocabulary as script steps.
    void Do(DcLabJson::Value const& action);

    Player* Human() const;
    ObjectGuid Guid() const { return _guid; }

private:
    struct Step
    {
        DcLabTrigger trigger;
        DcLabJson::Value action;
    };

    void Apply(DcLabJson::Value const& action);
    void TickAttack(std::uint32_t diff);

    DcLabJob& _job;
    ObjectGuid _guid;
    PlayerbotHolder* _holder = nullptr;
    bool _attached = false;
    std::vector<Step> _steps;
    std::vector<DcLabScenario::When> _whens;

    // Current standing action.
    ObjectGuid _attackTarget;
    std::uint32_t _attackSpell = 0;
    std::uint32_t _attackEveryMs = 2000;
    std::uint32_t _attackAccum = 0;
    float _attackRange = 0.0f;
};

#endif  // _PLAYERBOT_DCLABPUPPET_H
