/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCLABTRIGGER_H
#define _PLAYERBOT_DCLABTRIGGER_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Lab/DcLabScenario.h"

// Engine-free (gtested in TestLabScenario).
// One `when` armed against the run (pull-lab plan §2.3). Keyed on pipeline
// events, not wall clock: Feed() sees each pull event the recorder reports
// (phase:<Name>, commit, aggroConfirmed, campReached, safetyRelease,
// followerReleased, verdict:<Name>); Due() says whether it is time to act.
class DcLabTrigger
{
public:
    explicit DcLabTrigger(DcLabScenario::When const& when) : _w(when) {}

    void Feed(std::vector<std::pair<std::uint32_t, std::string>> const& events);
    // `predicate` is evaluated only for Predicate triggers.
    bool Due(std::uint32_t nowMs, bool predicate);
    bool Fired() const { return _fired; }
    void MarkFired() { _fired = true; }

    // Scenario event names that are aliases for a recorder event.
    static std::string Canonical(std::string const& event);

private:
    DcLabScenario::When _w;
    std::uint32_t _seen = 0;
    bool _armed = false;
    bool _fired = false;
    std::uint32_t _fireAt = 0;
    std::string _lastPhase;
};

#endif  // _PLAYERBOT_DCLABTRIGGER_H
