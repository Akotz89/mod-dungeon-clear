/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Lab/DcLabManager.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "Chat.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "StringFormat.h"
#include "WorldSession.h"

#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Lab/DcLabJob.h"
#include "Lab/DcLabPaths.h"
#include "TestRun/DcTestDriver.h"
#include "TestRun/DcTestDungeonRegistry.h"
#include "TestRun/DcTestGearTiers.h"
#include "TestRun/DcTestRunManager.h"

namespace
{
    constexpr uint32 kLaunchBackoffMs = 15000;
    constexpr uint32 kLiveWriteMs = 2000;

    uint64 NowUnixMs()
    {
        return static_cast<uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());
    }

    std::string Stamp(char const* prefix)
    {
        std::time_t const now = std::time(nullptr);
        std::tm tmBuf{};
        localtime_r(&now, &tmBuf);
        char buf[40];
        std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tmBuf);
        return std::string(prefix) + buf;
    }

    // The registry row a scenario runs in: its token, else the first row on
    // its map that is not a scenario row.
    DcTestDungeonRegistry::Row const* RowFor(DcLabScenario::Scenario const& s)
    {
        if (!s.dungeon.empty())
            return DcTestDungeonRegistry::Find(s.dungeon);
        for (DcTestDungeonRegistry::Row const& r : DcTestDungeonRegistry::All())
            if (r.mapId == s.mapId && !DcTestDungeonRegistry::IsScenario(r))
                return &r;
        return nullptr;
    }

    void AppendRecord(std::string const& json)
    {
        static std::mutex mtx;
        std::lock_guard<std::mutex> lock(mtx);
        std::ofstream out(DcLabPaths::RunsPath(), std::ios::app);
        if (out)
            out << json << '\n';
        else
            LOG_ERROR("playerbots.dungeonclear", "LAB could not append to {}", DcLabPaths::RunsPath());
    }
}

DcLabManager& DcLabManager::Instance()
{
    static DcLabManager inst;
    return inst;
}

std::vector<std::string> DcLabManager::ListIds(std::string const& glob) const
{
    std::vector<std::string> out;
    for (auto const& [id, path] : DcLabScenario::List(DcLabPaths::LabDir() + "/scenarios"))
        if (glob.empty() || DcLabScenario::GlobMatch(glob, id))
            out.push_back(id);
    return out;
}

bool DcLabManager::Load(std::string const& id, DcLabScenario::Scenario& out, std::string* err) const
{
    std::string const path = DcLabPaths::LabDir() + "/scenarios/" + id + ".json";
    return DcLabScenario::LoadFile(path, id, out, err);
}

std::string DcLabManager::NewRunId(std::string const& batchId)
{
    // lb-20261003-140501-1 -> lr-20261003-140501-1-<n>
    return "lr-" + batchId.substr(3) + "-" + std::to_string(++_runCounter);
}

std::string DcLabManager::Enqueue(Player* issuer, std::vector<std::string> const& ids, uint32 seed, uint32 repeat,
                                  std::string const& sweep, std::string* msg)
{
    if (ids.empty())
    {
        *msg = "no scenarios matched (see .dc lab list)";
        return "";
    }
    repeat = std::clamp<uint32>(repeat, 1u, 100u);

    std::vector<LabRun> runs;
    std::string const batchId = Stamp("lb-") + "-" + std::to_string(++_batchCounter);
    for (std::string const& id : ids)
    {
        DcLabScenario::Scenario sc;
        std::string err;
        if (!Load(id, sc, &err))
        {
            *msg = "scenario " + id + ": " + err;
            return "";
        }
        if (!RowFor(sc))
        {
            *msg = "scenario " + id + ": no dungeon row for map " + std::to_string(sc.mapId) +
                   (sc.dungeon.empty() ? "" : " / token '" + sc.dungeon + "'");
            return "";
        }
        // A sweep multiplies the scenario across its timing grid; every point
        // keeps the same party key, so one warm party walks the whole grid.
        std::vector<std::pair<std::string, DcLabScenario::Scenario>> points;
        if (sweep.empty())
            points.emplace_back("", sc);
        else
        {
            points = DcLabScenario::SweepPoints(sc, sweep, &err);
            if (points.empty())
            {
                *msg = "scenario " + id + ": " + err;
                return "";
            }
        }
        for (auto const& [label, point] : points)
            for (uint32 r = 0; r < repeat; ++r)
            {
                LabRun run;
                run.batchId = batchId;
                run.runId = NewRunId(batchId);
                run.scenario = point;
                run.sweepPoint = label;
                run.seed = seed ? seed + r : (static_cast<uint32>(std::hash<std::string>{}(run.runId)) | 1u);
                run.repeat = r;
                run.partyKey = DcLabScenario::PartyKey(point);
                runs.push_back(std::move(run));
            }
    }

    Batch b;
    b.id = batchId;
    b.issuer = issuer ? issuer->GetGUID() : ObjectGuid::Empty;
    b.total = static_cast<uint32>(runs.size());
    b.startedMs = NowUnixMs();
    _batches[batchId] = b;
    if (issuer)
        _issuer = issuer->GetGUID();
    // Map-major: runs land in their party key's queue, so a warm party never
    // crosses maps between runs.
    for (LabRun& run : runs)
        _queues[run.partyKey].push_back(std::move(run));
    *msg = Acore::StringFormat("Lab batch {}: {} run(s) of {} scenario(s) queued; up to {} parallel parties", batchId,
                               b.total, ids.size(), DcSettings::GetUInt(ObjectGuid::Empty, "Lab.MaxParties"));
    LOG_INFO("playerbots.dungeonclear", "LAB BATCH {} queued {} runs ({} scenarios)", batchId, b.total, ids.size());
    return batchId;
}

Player* DcLabManager::Issuer() const
{
    return _issuer ? ObjectAccessor::FindConnectedPlayer(_issuer) : nullptr;
}

std::optional<DcLabManager::LabRun> DcLabManager::Next(std::string const& key)
{
    auto it = _queues.find(key);
    if (it == _queues.end() || it->second.empty())
        return std::nullopt;
    LabRun run = std::move(it->second.front());
    it->second.pop_front();
    if (it->second.empty())
        _queues.erase(it);
    return run;
}

void DcLabManager::Requeue(LabRun run)
{
    LOG_INFO("playerbots.dungeonclear", "LAB {} re-queued (its party went away)", run.runId);
    _queues[run.partyKey].push_front(std::move(run));
}

void DcLabManager::Launch(std::string const& key, Player* gm)
{
    auto it = _queues.find(key);
    if (it == _queues.end() || it->second.empty())
        return;
    LabRun const& head = it->second.front();
    DcLabScenario::Scenario const& sc = head.scenario;
    DcTestDungeonRegistry::Row const* row = RowFor(sc);
    std::string const token = row ? row->token : "";
    uint32 const level = sc.level ? sc.level : (sc.heroic && row ? row->heroicLevel : row ? row->recommendedLevel : 0);

    std::vector<DcTestComp::Slot> comp;
    std::string err;
    if (!row || !DcLabScenario::ParseComp(sc.comp, head.seed, DcTestRunJob::RosterFor(*row, level), comp, &err))
    {
        // Permanent for this key: fail every queued run with the reason.
        std::string const why = row ? "comp: " + err : "no dungeon row";
        while (auto run = Next(key))
        {
            Result res;
            res.runId = run->runId;
            res.batchId = run->batchId;
            res.scenario = run->scenario.id;
            res.verdict = "error";
            res.end = "error:" + why;
            Report(*run, res, "{\"schema\":1,\"runId\":\"" + run->runId + "\",\"batchId\":\"" + run->batchId +
                                  "\",\"scenario\":\"" + run->scenario.id + "\",\"verdict\":\"error\",\"end\":\"error:" +
                                  DcLabJson::Escape(why) + "\"}");
        }
        return;
    }

    DcTestGearTiers::Spec gear;
    gear.ilvl = sc.gearIlvl;
    auto driver = std::make_shared<DcLabJob>(key);
    driver->Prime(*Next(key));

    std::string msg;
    std::string hostRunId;
    DcTestRunManager::StartErr kind = DcTestRunManager::StartErr::None;
    if (!DcTestRunManager::Instance().StartLab(gm, token, sc.level, head.seed, sc.heroic, gear, comp, driver, &msg,
                                               &kind, &hostRunId))
    {
        // Give the primed run back untouched — a refused launch never ran it,
        // so it costs no attempt; transient refusals (pool, cap) back off.
        if (std::optional<LabRun> back = driver->TakePending())
            _queues[key].push_front(std::move(*back));
        bool const transient = kind == DcTestRunManager::StartErr::CapHit ||
                               kind == DcTestRunManager::StartErr::PoolExhausted;
        _backoffMs[key] = kLaunchBackoffMs;
        LOG_INFO("playerbots.dungeonclear", "LAB party for {} not started ({}): {}", key,
                 transient ? "will retry" : "permanent", msg);
        if (!transient)
            while (auto run = Next(key))
            {
                Result res;
                res.runId = run->runId;
                res.batchId = run->batchId;
                res.scenario = run->scenario.id;
                res.verdict = "error";
                res.end = "error:" + msg;
                Report(*run, res, "{\"schema\":1,\"runId\":\"" + run->runId + "\",\"verdict\":\"error\",\"end\":\"" +
                                      DcLabJson::Escape(msg) + "\"}");
            }
        return;
    }
    LOG_INFO("playerbots.dungeonclear", "LAB party {} launched for {}", hostRunId, key);
    _parties.push_back({key, driver, hostRunId});
}

void DcLabManager::Tick(uint32 diff)
{
    for (auto& [key, ms] : _backoffMs)
        ms = ms > diff ? ms - diff : 0;

    _parties.erase(std::remove_if(_parties.begin(), _parties.end(),
                                  [](Party const& p) { return p.driver->Finished(); }),
                   _parties.end());

    if (!_queues.empty())
    {
        Player* gm = Issuer();
        if (!gm)
        {
            std::string why;
            if (DcTestDriver::EnsureOnline(&why))
                gm = DcTestDriver::Get();
        }
        if (gm)
        {
            uint32 const cap = std::max(1u, DcSettings::GetUInt(ObjectGuid::Empty, "Lab.MaxParties"));
            // Fill free capacity round-robin across keys, one party per key per
            // pass, never more parties on a key than it has runs waiting.
            bool launched = true;
            while (launched && _parties.size() < cap)
            {
                launched = false;
                std::vector<std::string> keys;
                for (auto const& kv : _queues)
                    keys.push_back(kv.first);
                for (std::string const& key : keys)
                {
                    if (_parties.size() >= cap)
                        break;
                    auto q = _queues.find(key);
                    if (q == _queues.end() || q->second.empty() || _backoffMs[key] > 0)
                        continue;
                    std::size_t onKey = 0;
                    for (Party const& p : _parties)
                        onKey += p.key == key;
                    // Each party already on this key takes the next run when it
                    // finishes its current one; only runs beyond that need a new party.
                    if (q->second.size() <= onKey)
                        continue;
                    std::size_t const before = _parties.size();
                    Launch(key, gm);
                    launched |= _parties.size() > before;
                }
            }
        }
    }

    _liveAccumMs += diff;
    if (_liveAccumMs >= kLiveWriteMs)
    {
        _liveAccumMs = 0;
        WriteLive();
    }
}

void DcLabManager::Report(LabRun const& run, Result const& result, std::string const& recordJson)
{
    AppendRecord(recordJson);
    auto it = _batches.find(run.batchId);
    if (it == _batches.end())
        return;
    Batch& b = it->second;
    ++b.done;
    b.results.push_back(result);
    if (Player* p = b.issuer ? ObjectAccessor::FindConnectedPlayer(b.issuer) : nullptr)
        ChatHandler(p->GetSession()).SendSysMessage(Acore::StringFormat(
            "[lab {}/{}] {} {}{}: {} (end {}, {:.1f}s) {}", b.done, b.total, result.runId, result.scenario,
            result.sweepPoint.empty() ? "" : " @" + result.sweepPoint, result.verdict, result.end,
            result.durationMs / 1000.0, result.oracles));
    if (b.done >= b.total)
    {
        FinishBatch(b);
        _batches.erase(it);
    }
}

void DcLabManager::FinishBatch(Batch& b)
{
    // Per scenario (and sweep point): verdict tally + the oracles that failed.
    struct Row
    {
        std::map<std::string, uint32> verdicts;
        std::map<std::string, uint32> fails;
        uint32 n = 0;
    };
    std::map<std::string, Row> rows;
    for (Result const& r : b.results)
    {
        Row& row = rows[r.scenario + (r.sweepPoint.empty() ? "" : " @" + r.sweepPoint)];
        ++row.n;
        ++row.verdicts[r.verdict];
        for (auto const& [id, res] : r.oracleRes)
            if (res == "fail")
                ++row.fails[id];
    }
    std::vector<std::string> lines;
    lines.push_back(Acore::StringFormat("Lab batch {} done: {} runs in {}s", b.id, b.total,
                                        (NowUnixMs() - b.startedMs) / 1000));
    for (auto const& [name, row] : rows)
    {
        std::string v, f;
        for (auto const& [k, n] : row.verdicts)
            v += (v.empty() ? "" : " ") + k + "=" + std::to_string(n);
        for (auto const& [k, n] : row.fails)
            f += (f.empty() ? "" : " ") + k + "x" + std::to_string(n);
        lines.push_back("  " + name + ": " + v + (f.empty() ? "" : "  [" + f + "]"));
    }
    // Sweep heat-maps (§2.3): phase x delay, plus the event row, per scenario.
    // Cell: verdict letter (P pass, F fail, x expected-fail, U unexpected-pass,
    // E error) and the oracles that failed there.
    std::map<std::string, std::map<std::string, Result const*>> sweeps;
    for (Result const& r : b.results)
        if (!r.sweepPoint.empty())
            sweeps[r.scenario][r.sweepPoint] = &r;
    auto cell = [](Result const* r) -> std::string
    {
        if (!r)
            return "-";
        std::string c = r->verdict == "pass" ? "P" : r->verdict == "fail" ? "F" : r->verdict == "expected-fail" ? "x"
                       : r->verdict == "unexpected-pass" ? "U" : "E";
        std::string f;
        for (auto const& [id, res] : r->oracleRes)
            if (res == "fail")
                f += (f.empty() ? ":" : ",") + id;
        return c + f;
    };
    std::string heat = "{\"batch\":\"" + b.id + "\",\"scenarios\":{";
    bool firstScenario = true;
    for (auto const& [scenario, pts] : sweeps)
    {
        lines.push_back("  heat-map " + scenario + " (rows: phase entered, cols: +delay ms)");
        std::string head = Acore::StringFormat("    {:<10}", "");
        for (uint32 d : DcLabScenario::SweepDelays())
            head += Acore::StringFormat("{:<14}", "+" + std::to_string(d));
        lines.push_back(head);
        for (std::string const& ph : DcLabScenario::SweepPhases())
        {
            std::string row = Acore::StringFormat("    {:<10}", ph);
            for (uint32 d : DcLabScenario::SweepDelays())
            {
                auto it = pts.find(ph + "+" + std::to_string(d));
                row += Acore::StringFormat("{:<14}", cell(it == pts.end() ? nullptr : it->second));
            }
            lines.push_back(row);
        }
        std::string ev = "    events    ";
        for (std::string const& e : DcLabScenario::SweepEvents())
        {
            auto it = pts.find("event:" + e);
            if (it != pts.end())
                ev += e + "=" + cell(it->second) + " ";
        }
        lines.push_back(ev);
        heat += std::string(firstScenario ? "" : ",") + "\"" + DcLabJson::Escape(scenario) + "\":{";
        firstScenario = false;
        bool firstPoint = true;
        for (auto const& [pt, r] : pts)
        {
            std::string fails;
            for (auto const& [id, res] : r->oracleRes)
                if (res == "fail")
                    fails += std::string(fails.empty() ? "" : ",") + "\"" + id + "\"";
            heat += std::string(firstPoint ? "" : ",") + "\"" + pt + "\":{\"verdict\":\"" + r->verdict +
                    "\",\"run\":\"" + r->runId + "\",\"fails\":[" + fails + "]}";
            firstPoint = false;
        }
        heat += "}";
    }
    heat += "}}";
    if (!sweeps.empty())
    {
        std::string const dir = DcLabPaths::HeatmapDir();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream out(dir + "/" + b.id + ".json", std::ios::trunc);
        if (out)
        {
            out << heat << '\n';
            lines.push_back("  heat-map written to " + dir + "/" + b.id + ".json");
        }
    }

    Player* p = b.issuer ? ObjectAccessor::FindConnectedPlayer(b.issuer) : nullptr;
    for (std::string const& l : lines)
    {
        LOG_INFO("playerbots.dungeonclear", "LAB {}", l);
        if (p)
            ChatHandler(p->GetSession()).SendSysMessage(l);
    }
}

void DcLabManager::Stop(std::string* msg)
{
    std::size_t queued = 0;
    for (auto const& kv : _queues)
        queued += kv.second.size();
    _queues.clear();
    std::size_t stopped = 0;
    for (Party const& p : _parties)
    {
        std::string m;
        if (DcTestRunManager::Instance().Stop(p.hostRunId, &m))
            ++stopped;
    }
    _batches.clear();
    *msg = Acore::StringFormat("Lab stopped: {} queued run(s) dropped, {} party(ies) stopping", queued, stopped);
}

std::string DcLabManager::StatusText() const
{
    std::size_t queued = 0;
    for (auto const& kv : _queues)
        queued += kv.second.size();
    std::string out = Acore::StringFormat("Lab: {} queued, {} parties (cap {})", queued, _parties.size(),
                                          DcSettings::GetUInt(ObjectGuid::Empty, "Lab.MaxParties"));
    for (auto const& [id, b] : _batches)
        out += Acore::StringFormat("\n  batch {}: {}/{} done", id, b.done, b.total);
    for (Party const& p : _parties)
        out += "\n  party " + p.hostRunId + ": " + p.driver->Status();
    return out;
}

void DcLabManager::WriteLive()
{
    static bool wasActive = false;
    bool const active = Active() || !_batches.empty();
    if (!active && !wasActive)
        return;
    wasActive = active;
    std::string parties = "[";
    for (std::size_t i = 0; i < _parties.size(); ++i)
    {
        Party const& p = _parties[i];
        if (i)
            parties += ",";
        parties += DcLabJson::Line().Add("host", p.hostRunId).Add("key", p.key).Add("run", p.driver->CurrentRunId())
                       .Add("status", p.driver->Status()).Str();
    }
    parties += "]";
    std::string batches = "[";
    bool first = true;
    for (auto const& [id, b] : _batches)
    {
        if (!first)
            batches += ",";
        first = false;
        batches += DcLabJson::Line().Add("id", id).Add("done", b.done).Add("total", b.total).Str();
    }
    batches += "]";
    std::size_t queued = 0;
    for (auto const& kv : _queues)
        queued += kv.second.size();
    std::string const body = "{\"active\":" + std::string(active ? "true" : "false") +
                             ",\"updatedAt\":" + std::to_string(NowUnixMs()) +
                             ",\"queued\":" + std::to_string(queued) + ",\"parties\":" + parties +
                             ",\"batches\":" + batches + "}";
    std::ofstream out(DcLabPaths::LivePath(), std::ios::trunc);
    if (out)
        out << body << '\n';
}
