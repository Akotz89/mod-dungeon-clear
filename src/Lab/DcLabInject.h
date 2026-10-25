/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCLABINJECT_H
#define _PLAYERBOT_DCLABINJECT_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ObjectGuid.h"
#include "Position.h"
#include "Lab/DcLabScenario.h"
#include "Lab/DcLabTrigger.h"

class DcLabJob;
class Unit;

// Executes a scenario's injections for one run (world thread).
class DcLabInjector
{
public:
    explicit DcLabInjector(DcLabJob& job);

    // Pull events drained from the recorder this tick (shared with the puppet).
    void Tick(std::vector<std::pair<std::uint32_t, std::string>> const& events);

    // Unit references used by injections and puppet scripts:
    //   tank | heal | healer | dps | dps1..dpsN | slot:N | human | party
    //   pack:<tag> | <synthetic or pack tag> | spawn:<spawnId>
    //   pullTarget | nearest | nearestIdle
    static std::vector<Unit*> Resolve(DcLabJob& job, std::string const& ref);

    // Live value of a predicate's left-hand side (NaN when unknown).
    static double Measure(DcLabJob& job, std::string const& lhs);
    static bool Evaluate(DcLabJob& job, DcLabScenario::When const& w);

private:
    struct Patrol
    {
        ObjectGuid mob;
        std::vector<Position> path;
        std::size_t next = 0;
    };

    void Fire(std::size_t idx);
    void TickPatrols();

    DcLabJob& _job;
    std::vector<DcLabTrigger> _triggers;
    std::vector<Patrol> _patrols;
};

#endif  // _PLAYERBOT_DCLABINJECT_H
