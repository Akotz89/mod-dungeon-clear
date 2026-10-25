/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCLABMANAGER_H
#define _PLAYERBOT_DCLABMANAGER_H

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ObjectGuid.h"
#include "Lab/DcLabScenario.h"

class DcLabJob;
class Player;

// The Pull Lab scheduler (pull-lab plan §4.1). `.dc lab run|batch` turn into
// LabRuns queued by party key (map, difficulty, comp, level, gear, human slot);
// up to Lab.MaxParties warm parties drain those queues in parallel, each party
// re-staging its own instance between runs instead of re-provisioning. A party
// is an ordinary DcTestRunJob (it shows in `.dc test status`) whose Lab driver
// is a DcLabJob.
//
// World thread only, like the test-run manager it sits beside.
class DcLabManager
{
public:
    struct LabRun
    {
        std::string runId;     // lr-<batch>-<n>
        std::string batchId;   // lb-YYYYmmdd-HHMMSS-N
        DcLabScenario::Scenario scenario;  // possibly sweep-modified
        std::uint32_t seed = 0;
        std::uint32_t repeat = 0;          // 0-based repeat index
        std::string sweepPoint;            // "" or the injection's when, e.g. "Returning+400"
        std::string partyKey;
        std::uint32_t attempts = 0;        // re-queued after a party died under it
    };

    struct Result
    {
        std::string runId;
        std::string batchId;
        std::string scenario;
        std::string sweepPoint;
        std::string verdict;    // pass | fail | expected-fail | unexpected-pass | error
        std::string end;        // goal | wipe | timeout | dc-disabled | error:...
        std::string oracles;    // DcLabOracles::Summary
        std::vector<std::pair<std::string, std::string>> oracleRes;  // id -> pass/fail/na
        std::uint32_t durationMs = 0;
        std::string traceFile;
    };

    static DcLabManager& Instance();

    // `.dc lab run <id> [seed=] [repeat=] [sweep=phases|events|all]` and
    // `.dc lab batch <glob> [repeat=]`. Returns the batch id, or "" with *msg.
    std::string Enqueue(Player* issuer, std::vector<std::string> const& scenarioIds, std::uint32_t seed,
                        std::uint32_t repeat, std::string const& sweep, std::string* msg);

    // Scenario ids under lab/scenarios matching `glob` ("*" = all).
    std::vector<std::string> ListIds(std::string const& glob) const;
    bool Load(std::string const& id, DcLabScenario::Scenario& out, std::string* err) const;

    void Stop(std::string* msg);
    std::string StatusText() const;
    bool Active() const { return !_queues.empty() || !_parties.empty(); }

    void Tick(std::uint32_t diff);

    // --- called by DcLabJob (world thread) ----------------------------------
    std::optional<LabRun> Next(std::string const& partyKey);
    void Requeue(LabRun run);
    void Report(LabRun const& run, Result const& result, std::string const& recordJson);

    std::string NewRunId(std::string const& batchId);

private:
    DcLabManager() = default;

    struct Party
    {
        std::string key;
        std::shared_ptr<DcLabJob> driver;
        std::string hostRunId;  // tr- id of the hosting job
    };

    struct Batch
    {
        std::string id;
        ObjectGuid issuer;
        std::uint32_t total = 0;
        std::uint32_t done = 0;
        std::uint64_t startedMs = 0;
        std::vector<Result> results;
    };

    void Launch(std::string const& key, Player* gm);
    void FinishBatch(Batch& b);
    void WriteLive();
    Player* Issuer() const;

    std::map<std::string, std::deque<LabRun>> _queues;
    std::map<std::string, std::uint32_t> _backoffMs;   // key -> ms until the next launch try
    std::vector<Party> _parties;
    std::map<std::string, Batch> _batches;
    ObjectGuid _issuer;
    std::uint32_t _liveAccumMs = 0;
    std::uint32_t _batchCounter = 0;
    std::uint32_t _runCounter = 0;
};

#endif  // _PLAYERBOT_DCLABMANAGER_H
