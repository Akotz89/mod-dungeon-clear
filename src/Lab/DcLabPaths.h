/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCLABPATHS_H
#define _PLAYERBOT_DCLABPATHS_H

#include <cstdlib>
#include <string>

// Where the Pull Lab reads and writes. Relative paths resolve against the
// worldserver's working directory, next to dc_testruns.jsonl — the same
// convention (and the same env-var override style) as the test-run record.
namespace DcLabPaths
{
    inline std::string Env(char const* name, char const* def)
    {
        if (char const* v = std::getenv(name))
            if (v[0])
                return v;
        return def;
    }

    // One LabTrace JSONL per run: <dir>/<runId>.jsonl.
    inline std::string TraceDir() { return Env("DC_LAB_TRACE_DIR", "lab_traces"); }

    // One record per Lab run (scenario, sweep point, oracle verdicts, SHAs).
    inline std::string RunsPath() { return Env("DC_LABRUNS_FILE", "dc_labruns.jsonl"); }

    // Sweep heat-maps: <dir>/<batchId>.json, one grid per swept scenario.
    inline std::string HeatmapDir() { return Env("DC_LAB_HEATMAP_DIR", "lab_heatmaps"); }

    // Live Lab state for the Deck (truncate-written, like dc_testrun_live.json).
    inline std::string LivePath() { return Env("DC_LAB_LIVE_FILE", "dc_lab_live.json"); }

    // The committed scenario tree (lab/ in the module repo). Defaults to the
    // source checkout this binary was built from; DC_LAB_DIR overrides it for a
    // deploy that ships the scenarios elsewhere.
    inline std::string LabDir()
    {
        if (char const* v = std::getenv("DC_LAB_DIR"))
            if (v[0])
                return v;
        std::string f = __FILE__;
        std::size_t const at = f.rfind("/src/Lab/");
        return at == std::string::npos ? std::string("lab") : f.substr(0, at) + "/lab";
    }
}

#endif  // _PLAYERBOT_DCLABPATHS_H
