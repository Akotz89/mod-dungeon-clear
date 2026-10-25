/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Lab/DcLabTrigger.h"

std::string DcLabTrigger::Canonical(std::string const& e)
{
    if (e == "tagCast")
        return "phase:Advancing";
    if (e == "plant")
        return "campReached";
    if (e == "endCampFight" || e == "nextScoutStart")
        return "phase:Idle";
    return e;
}

void DcLabTrigger::Feed(std::vector<std::pair<std::uint32_t, std::string>> const& events)
{
    using Kind = DcLabScenario::When::Kind;
    if (_armed || (_w.kind != Kind::PhaseEnter && _w.kind != Kind::Event))
        return;
    static char const* const kPhase[] = {"Idle", "Forming", "Advancing", "Returning", "Engage"};
    std::string const want = _w.kind == Kind::PhaseEnter ? std::string("phase:") + kPhase[_w.phase] : Canonical(_w.event);
    for (auto const& [t, name] : events)
    {
        // endCampFight/nextScoutStart: an Idle that follows a camp fight.
        bool const afterFight = _w.kind == Kind::Event && (_w.event == "endCampFight" || _w.event == "nextScoutStart");
        bool const match = name == want && (!afterFight || _lastPhase == "phase:Engage");
        if (name.rfind("phase:", 0) == 0)
            _lastPhase = name;
        if (!match)
            continue;
        if (++_seen == _w.occurrence)
        {
            _armed = true;
            _fireAt = t + _w.delayMs;
            return;
        }
    }
}

bool DcLabTrigger::Due(std::uint32_t nowMs, bool predicate)
{
    using Kind = DcLabScenario::When::Kind;
    if (_fired)
        return false;
    if (!_armed)
    {
        if (_w.kind == Kind::AtMs)
        {
            _armed = true;
            _fireAt = _w.atMs + _w.delayMs;
        }
        else if (_w.kind == Kind::Predicate && predicate)
        {
            _armed = true;
            _fireAt = nowMs + _w.delayMs;
        }
    }
    return _armed && nowMs >= _fireAt;
}

