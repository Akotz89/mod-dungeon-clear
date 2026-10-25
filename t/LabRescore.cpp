/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Re-score saved Pull Lab traces with the CURRENT oracles, without re-running
// anything in game: the oracles are pure over the trace, so an oracle change
// is checked against a whole recorded baseline in seconds.
//
//   lab_rescore <trace.jsonl>... [--scenario-dir <lab/scenarios>]
//
// Prints, per trace, the stored oracle line (from the run) and the rescored
// one. With --scenario-dir, the scenario's expect.oracles thresholds apply and
// the museum verdict is re-judged. Engine-free; build by hand, e.g.:
//   clang++ -std=c++20 -O2 -I src t/LabRescore.cpp src/Lab/DcLab{Json,Trace,Oracles,Scenario}.cpp
//           src/TestRun/DcTestComp.cpp -o lab_rescore

#include <fstream>
#include <iostream>
#include <sstream>

#include "Lab/DcLabOracles.h"
#include "Lab/DcLabScenario.h"
#include "Lab/DcLabTrace.h"

int main(int argc, char** argv)
{
    std::string scenarioDir;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--scenario-dir" && i + 1 < argc)
            scenarioDir = argv[++i];
        else
            files.push_back(a);
    }
    for (std::string const& path : files)
    {
        std::ifstream in(path);
        std::stringstream ss;
        ss << in.rdbuf();
        DcLab::Trace tr;
        if (!DcLab::FromJsonl(ss.str(), tr))
        {
            std::cout << path << ": not a LabTrace\n";
            continue;
        }
        DcLabOracles::OracleConfig cfg;
        DcLabScenario::Scenario sc;
        bool haveScenario = false;
        if (!scenarioDir.empty() && !tr.header.scenario.empty())
        {
            std::string err;
            haveScenario = DcLabScenario::LoadFile(scenarioDir + "/" + tr.header.scenario + ".json",
                                                   tr.header.scenario, sc, &err);
            if (haveScenario)
                DcLabOracles::ApplyOverrides(cfg, sc.oracleExpect);
        }
        std::vector<DcLab::OracleResult> const old = tr.oracles;
        std::vector<DcLab::OracleResult> const now = DcLabOracles::Evaluate(tr, cfg);
        std::string verdict;
        if (haveScenario)
        {
            std::vector<std::pair<std::string, std::string>> res;
            for (DcLab::OracleResult const& o : now)
                res.emplace_back(o.id, DcLab::VerdictName(o.verdict));
            verdict = DcLabScenario::Judge(sc, res);
        }
        std::cout << tr.header.runId << " " << tr.header.scenario << (verdict.empty() ? "" : " -> " + verdict)
                  << "\n  was " << DcLabOracles::Summary(old) << "\n  now " << DcLabOracles::Summary(now) << "\n";
        for (DcLab::OracleResult const& o : now)
            if (o.verdict == DcLab::Verdict::Fail)
                std::cout << "    " << o.id << " " << o.detail << "\n";
    }
    return 0;
}
