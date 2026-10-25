/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Lab/DcLabScenario.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace DcLabScenario
{
    namespace
    {
        using DcLabJson::Value;

        // Ops the Lab executor implements (DcLabInject). A typo in a scenario is
        // a load error, not a silently skipped fault.
        std::set<std::string> const kOps = {
            "spawn", "aggro", "startPatrol", "forceCallForHelp", "fear", "knockback", "stun", "root",
            "daze", "evade", "dropCombat", "kill", "door", "spawnLosBlocker", "setReact", "humanAction",
            "note"};

        // Pipeline events an injection may key on (DcLabRecorder pull events).
        std::set<std::string> const kEvents = {
            "commit", "aggroConfirmed", "campReached", "safetyRelease", "followerReleased",
            "endCampFight", "nextScoutStart", "verdict:Leeroy", "verdict:Advanced", "verdict:PatrolHold",
            "verdict:NoOp", "tagCast", "plant"};

        std::set<std::string> const kPredLhs = {"tankDistToCamp", "tankHp", "tankDistToPack", "partyMinHp",
                                                "elapsedS", "packEngaged"};

        bool ReadVec(Value const& v, Vec4& out, bool needO = false)
        {
            if (!v.IsArray() || v.arr.size() < 3)
                return false;
            out.x = static_cast<float>(v.arr[0].AsNumber());
            out.y = static_cast<float>(v.arr[1].AsNumber());
            out.z = static_cast<float>(v.arr[2].AsNumber());
            out.o = v.arr.size() > 3 ? static_cast<float>(v.arr[3].AsNumber()) : 0.0f;
            out.set = true;
            return !needO || v.arr.size() > 3;
        }

        bool ReadIds(Value const& v, std::vector<std::uint64_t>& out)
        {
            if (!v.IsArray())
                return false;
            for (Value const& e : v.arr)
                out.push_back(e.AsU64());
            return true;
        }

        bool ParseDo(Value const& d, Do& out, std::string* err)
        {
            if (!d.IsObject() || d.obj.empty())
            {
                *err = "'do' must be a non-empty object";
                return false;
            }
            // The op is the first key that names a known op ("spawn" in
            // {"spawn": "add1", "aggro": "heal"} — "aggro" is then an argument).
            for (char const* first : {"spawn", "humanAction", "startPatrol", "forceCallForHelp", "door",
                                      "spawnLosBlocker", "setReact"})
                if (d.Has(first))
                {
                    out.op = first;
                    break;
                }
            if (out.op.empty())
                for (auto const& kv : d.obj)
                    if (kOps.count(kv.first))
                    {
                        out.op = kv.first;
                        break;
                    }
            if (out.op.empty())
            {
                *err = "'do' names no known op";
                return false;
            }
            out.args = d;
            return true;
        }
    }

    bool ParseWhen(DcLabJson::Value const& w, When& out, std::string* err)
    {
        if (!w.IsObject())
        {
            *err = "'when' must be an object";
            return false;
        }
        out.delayMs = static_cast<std::uint32_t>(w["delayMs"].AsNumber(0));
        out.occurrence = static_cast<std::uint32_t>(std::max(1.0, w["occurrence"].AsNumber(1)));
        if (w.Has("atMs"))
        {
            out.kind = When::Kind::AtMs;
            out.atMs = static_cast<std::uint32_t>(w["atMs"].AsNumber());
            return true;
        }
        if (w.Has("phaseEnter"))
        {
            out.kind = When::Kind::PhaseEnter;
            if (!PhaseFromName(w["phaseEnter"].AsString(), out.phase))
            {
                *err = "unknown phase '" + w["phaseEnter"].AsString() + "'";
                return false;
            }
            return true;
        }
        if (w.Has("event"))
        {
            out.kind = When::Kind::Event;
            out.event = w["event"].AsString();
            if (!kEvents.count(out.event))
            {
                *err = "unknown event '" + out.event + "'";
                return false;
            }
            return true;
        }
        if (w.Has("predicate"))
        {
            out.kind = When::Kind::Predicate;
            std::istringstream in(w["predicate"].AsString());
            if (!(in >> out.lhs >> out.op >> out.rhs) || !kPredLhs.count(out.lhs) ||
                (out.op != "<" && out.op != "<=" && out.op != ">" && out.op != ">="))
            {
                *err = "bad predicate '" + w["predicate"].AsString() + "' (want '<lhs> <op> <number>', lhs one of "
                       "tankDistToCamp tankHp tankDistToPack partyMinHp elapsedS packEngaged)";
                return false;
            }
            return true;
        }
        *err = "'when' needs one of atMs / phaseEnter / event / predicate";
        return false;
    }

    bool PhaseFromName(std::string const& s, std::uint32_t& out)
    {
        static char const* const kNames[] = {"Idle", "Forming", "Advancing", "Returning", "Engage"};
        for (std::uint32_t i = 0; i < 5; ++i)
            if (s == kNames[i])
            {
                out = i;
                return true;
            }
        return false;
    }

    std::vector<std::string> const& SweepPhases()
    {
        static std::vector<std::string> const v = {"Forming", "Advancing", "Returning", "Engage"};
        return v;
    }

    std::vector<std::uint32_t> const& SweepDelays()
    {
        static std::vector<std::uint32_t> const v = {0, 150, 400, 1000, 2500};
        return v;
    }

    std::vector<std::string> const& SweepEvents()
    {
        static std::vector<std::string> const v = {"commit", "aggroConfirmed", "campReached", "followerReleased",
                                                   "safetyRelease"};
        return v;
    }

    std::vector<std::pair<std::string, Scenario>> SweepPoints(Scenario const& s, std::string const& mode,
                                                              std::string* err)
    {
        std::vector<std::pair<std::string, Scenario>> out;
        if (mode != "all" && mode != "phases" && mode != "events")
        {
            if (err)
                *err = "sweep must be all|phases|events";
            return out;
        }
        if (s.injections.empty())
        {
            if (err)
                *err = "scenario " + s.id + " has no injection to sweep";
            return out;
        }
        std::size_t idx = 0;
        for (std::size_t i = 0; i < s.injections.size(); ++i)
            if (s.injections[i].sweep)
            {
                idx = i;
                break;
            }
        auto add = [&](std::string const& label, When const& w)
        {
            Scenario c = s;
            c.injections[idx].when = w;
            c.injections[idx].label = c.injections[idx].what.op + "@" + label;
            out.emplace_back(label, std::move(c));
        };
        if (mode != "events")
            for (std::string const& ph : SweepPhases())
                for (std::uint32_t d : SweepDelays())
                {
                    When w;
                    w.kind = When::Kind::PhaseEnter;
                    PhaseFromName(ph, w.phase);
                    w.delayMs = d;
                    add(ph + "+" + std::to_string(d), w);
                }
        if (mode != "phases")
            for (std::string const& ev : SweepEvents())
            {
                When w;
                w.kind = When::Kind::Event;
                w.event = ev;
                add("event:" + ev, w);
            }
        return out;
    }

    std::string Scenario::PackOf(std::uint64_t spawnId) const
    {
        for (auto const& kv : packs)
            if (std::find(kv.second.begin(), kv.second.end(), spawnId) != kv.second.end())
                return kv.first;
        return "";
    }

    bool Parse(Value const& root, std::string const& id, Scenario& out, std::string* errOut)
    {
        std::string scratch;
        std::string* err = errOut ? errOut : &scratch;
        out = Scenario{};
        out.id = root.Has("id") ? root["id"].AsString() : id;
        if (!id.empty() && out.id != id)
            out.warnings.push_back("file id '" + out.id + "' differs from its path id '" + id + "' — path wins");
        if (!id.empty())
            out.id = id;
        out.about = root["about"].AsString();
        out.schema = static_cast<std::uint32_t>(root["schema"].AsNumber(1));
        if (out.schema != 1)
        {
            *err = "unsupported schema " + std::to_string(out.schema);
            return false;
        }

        Value const& g = root["geometry"];
        out.mapId = static_cast<std::uint32_t>(g["map"].AsNumber(0));
        out.dungeon = g["dungeon"].AsString();
        out.heroic = g["heroic"].AsBool(false) || g["difficulty"].AsNumber(0) >= 1;
        if (out.mapId == 0 && out.dungeon.empty())
        {
            *err = "geometry needs 'map' or 'dungeon'";
            return false;
        }
        Value const& region = g.Has("region") ? g["region"] : g;
        Vec4 c;
        if (ReadVec(region["center"], c))
        {
            out.cx = c.x;
            out.cy = c.y;
            out.cz = c.z;
        }
        out.radius = static_cast<float>(region["radius"].AsNumber(70));
        out.objective = static_cast<std::uint32_t>(g["objective"].AsNumber(0));
        out.hideAll = g["hide"].AsString("all") != "region";

        Value const& native = root["actors"]["native"];
        if (native["packs"].IsObject())
            for (auto const& kv : native["packs"].obj)
                if (!ReadIds(kv.second, out.packs[kv.first]))
                {
                    *err = "actors.native.packs." + kv.first + " must be an array of spawnIds";
                    return false;
                }
        if (native.Has("keep"))
        {
            std::vector<std::uint64_t> keep;
            if (!ReadIds(native["keep"], keep))
            {
                *err = "actors.native.keep must be an array of spawnIds";
                return false;
            }
            // A bare keep list IS the target pack unless packs say otherwise.
            for (std::uint64_t s : keep)
                if (!out.Keeps(s))
                    out.packs["target"].push_back(s);
        }
        for (Value const& sv : root["actors"]["synthetic"].arr)
        {
            Synthetic s;
            s.tag = sv["tag"].AsString();
            s.entry = static_cast<std::uint32_t>(sv["entry"].AsNumber(0));
            s.spawn = sv["spawn"].AsString("start");
            s.pack = sv["pack"].AsString(s.tag);
            if (s.tag.empty() || !s.entry || !ReadVec(sv["pos"], s.pos))
            {
                *err = "synthetic actor needs tag, entry and pos [x,y,z(,o)]";
                return false;
            }
            if (s.spawn != "start" && s.spawn != "onInject")
            {
                *err = "synthetic '" + s.tag + "': spawn must be start|onInject";
                return false;
            }
            out.synthetic.push_back(s);
        }

        Value const& p = root["party"];
        out.comp = p["comp"].AsString();
        out.level = static_cast<std::uint32_t>(p["level"].AsNumber(0));
        out.gearIlvl = static_cast<std::uint32_t>(p["ilvl"].AsNumber(0));
        if (!ReadVec(p["start"], out.start))
        {
            *err = "party.start [x,y,z,o] is required";
            return false;
        }
        out.react = p["react"].AsString("fast");
        if (out.react != "fast" && out.react != "masterless")
        {
            *err = "party.react must be fast|masterless";
            return false;
        }
        out.seedTarget = p["seedTarget"].AsBool(false);
        if (p["human"].IsObject())
        {
            out.human.slot = static_cast<int>(p["human"]["slot"].AsNumber(-1));
            out.human.script = p["human"]["script"].AsString();
            out.human.leads = p["human"]["leads"].AsBool(true);
            if (out.human.slot <= 0)
            {
                *err = "party.human.slot must be a follower slot (1+): slot 0 is the DC tank";
                return false;
            }
        }

        for (auto const& kv : root["settings"].obj)
        {
            if (kv.first == "PullSetting")
                out.pullSetting = static_cast<int>(kv.second.AsNumber(-1));
            else
                out.settings.emplace_back(kv.first, kv.second.AsNumber());
        }

        for (Value const& iv : root["injections"].arr)
        {
            Injection inj;
            std::string e;
            if (!ParseWhen(iv["when"], inj.when, &e) || !ParseDo(iv["do"], inj.what, &e))
            {
                *err = "injection " + std::to_string(out.injections.size()) + ": " + e;
                return false;
            }
            inj.label = iv["label"].AsString();
            inj.sweep = iv["sweep"].AsBool(false);
            if (inj.label.empty())
            {
                std::string w;
                switch (inj.when.kind)
                {
                    case When::Kind::AtMs: w = "t" + std::to_string(inj.when.atMs); break;
                    case When::Kind::PhaseEnter:
                    {
                        static char const* const kN[] = {"Idle", "Forming", "Advancing", "Returning", "Engage"};
                        w = kN[inj.when.phase];
                        break;
                    }
                    case When::Kind::Event: w = inj.when.event; break;
                    case When::Kind::Predicate: w = inj.when.lhs + inj.when.op + std::to_string(inj.when.rhs); break;
                }
                if (inj.when.delayMs)
                    w += "+" + std::to_string(inj.when.delayMs);
                inj.label = inj.what.op + "@" + w;
            }
            if (inj.what.op == "spawn")
            {
                std::string const tag = inj.what.args["spawn"].AsString();
                bool found = false;
                for (Synthetic const& s : out.synthetic)
                    found |= s.tag == tag;
                if (!found)
                {
                    *err = "injection spawns unknown synthetic actor '" + tag + "'";
                    return false;
                }
            }
            out.injections.push_back(std::move(inj));
        }

        Value const& goal = root["goal"];
        if (goal.Has("kill"))
        {
            out.goalPacks.clear();
            auto addGoal = [&](std::string g2)
            {
                if (g2.rfind("pack:", 0) == 0)
                    g2 = g2.substr(5);
                out.goalPacks.push_back(g2);
            };
            if (goal["kill"].IsArray())
                for (Value const& k : goal["kill"].arr)
                    addGoal(k.AsString());
            else
                addGoal(goal["kill"].AsString());
        }
        out.timeoutS = static_cast<std::uint32_t>(goal["timeoutS"].AsNumber(90));
        for (std::string const& gp : out.goalPacks)
            if (gp != "*" && !out.packs.count(gp))
            {
                bool synth = false;
                for (Synthetic const& s : out.synthetic)
                    synth |= s.pack == gp;
                if (!synth)
                {
                    *err = "goal pack '" + gp + "' has no members";
                    return false;
                }
            }

        Value const& ex = root["expect"];
        out.oracleExpect = ex["oracles"];
        out.knownFailure = ex["knownFailure"].AsString();
        for (auto const& kv : out.oracleExpect.obj)
        {
            std::string v = kv.second.IsString() ? kv.second.AsString() : kv.second["expect"].AsString();
            if (v == "pass" || v == "fail")
                out.expected[kv.first] = v;
        }
        if (!out.knownFailure.empty() && out.knownFailure.rfind("open:", 0) != 0 &&
            out.knownFailure.rfind("fixed:", 0) != 0)
        {
            *err = "expect.knownFailure must be 'open:<ref>' or 'fixed:<ref>'";
            return false;
        }
        if (out.packs.empty() && out.synthetic.empty())
            out.warnings.push_back("no kept or synthetic actors — the party will see an empty instance");
        if (!out.objective)
            out.warnings.push_back("no geometry.objective — DC routes to its own next boss, which may lead away");
        return true;
    }

    bool LoadFile(std::string const& path, std::string const& id, Scenario& out, std::string* err)
    {
        std::ifstream in(path);
        if (!in)
        {
            if (err)
                *err = "cannot open " + path;
            return false;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        Value root;
        std::string perr;
        if (!DcLabJson::Parse(ss.str(), root, &perr))
        {
            if (err)
                *err = path + ":" + perr;
            return false;
        }
        if (!Parse(root, id, out, err))
            return false;
        out.sourcePath = path;
        return true;
    }

    std::vector<std::pair<std::string, std::string>> List(std::string const& dir)
    {
        std::vector<std::pair<std::string, std::string>> out;
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec))
            return out;
        for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (!it->is_regular_file() || it->path().extension() != ".json")
                continue;
            std::string id = std::filesystem::relative(it->path(), dir, ec).generic_string();
            id = id.substr(0, id.size() - 5);
            out.emplace_back(id, it->path().string());
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    bool GlobMatch(std::string const& pat, std::string const& s)
    {
        std::size_t p = 0, i = 0, star = std::string::npos, mark = 0;
        while (i < s.size())
        {
            if (p < pat.size() && (pat[p] == s[i] || pat[p] == '?'))
            {
                ++p;
                ++i;
            }
            else if (p < pat.size() && pat[p] == '*')
            {
                star = p++;
                mark = i;
            }
            else if (star != std::string::npos)
            {
                p = star + 1;
                i = ++mark;
            }
            else
                return false;
        }
        while (p < pat.size() && pat[p] == '*')
            ++p;
        return p == pat.size();
    }

    bool ParseComp(std::string const& spec, std::uint32_t seed, DcTestComp::Roster roster,
                   std::vector<DcTestComp::Slot>& out, std::string* err)
    {
        out.clear();
        if (spec.empty() || spec == "random")
        {
            auto const c = DcTestComp::BuildComp(seed ? seed : 1u, roster);
            out.assign(c.begin(), c.end());
            return true;
        }
        static std::pair<char const*, std::uint8_t> const kClasses[] = {
            {"warrior", 1}, {"paladin", 2}, {"hunter", 3},  {"rogue", 4},   {"priest", 5},
            {"deathknight", 6}, {"dk", 6},  {"shaman", 7},  {"mage", 8},    {"warlock", 9}, {"druid", 11}};
        std::vector<std::string> tokens;
        std::string cur;
        for (char ch : spec + ",")
        {
            if (ch == ',')
            {
                if (!cur.empty())
                    tokens.push_back(cur);
                cur.clear();
            }
            else if (!std::isspace(static_cast<unsigned char>(ch)))
                cur += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (tokens.size() < DcTestComp::kMinPartySize || tokens.size() > DcTestComp::kMaxPartySize)
        {
            if (err)
                *err = "comp needs 2-40 members";
            return false;
        }
        for (std::size_t i = 0; i < tokens.size(); ++i)
        {
            std::string const& tok = tokens[i];
            std::size_t const dash = tok.find('-');
            std::string const cls = tok.substr(0, dash);
            std::string const qual = dash == std::string::npos ? "" : tok.substr(dash + 1);
            std::uint8_t classId = 0;
            for (auto const& c : kClasses)
                if (cls == c.first)
                    classId = c.second;
            if (!classId)
            {
                if (err)
                    *err = "unknown class '" + cls + "'";
                return false;
            }
            std::string role = i == 0 ? "tank" : i == 1 ? "heal" : "dps";
            std::string specWanted;
            if (qual == "tank" || qual == "heal" || qual == "dps")
                role = qual;
            else if (!qual.empty())
                specWanted = qual;
            bool found = false;
            // A named spec picks its own role; otherwise the role picks the spec.
            for (char const* r : {"tank", "heal", "dps"})
            {
                if (found)
                    break;
                if (specWanted.empty() && role != r)
                    continue;
                for (DcTestComp::Slot const& s : DcTestComp::RolePool(r, DcTestComp::Roster::WithDeathKnights))
                {
                    if (s.classId != classId)
                        continue;
                    if (!specWanted.empty() && std::string(s.fallbackSpec) != specWanted &&
                        std::string(s.specName) != specWanted)
                        continue;
                    out.push_back(s);
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                if (err)
                    *err = "no " + (specWanted.empty() ? role : specWanted) + " spec for " + cls;
                return false;
            }
            if (classId == 6 && roster == DcTestComp::Roster::NoDeathKnights)
            {
                if (err)
                    *err = "death knights need a WotLK row at level 55+";
                return false;
            }
        }
        if (std::string(out[0].role) != "tank")
        {
            if (err)
                *err = "slot 0 must be the tank (DC's leader)";
            return false;
        }
        return true;
    }

    std::string PartyKey(Scenario const& s)
    {
        std::ostringstream k;
        k << (s.dungeon.empty() ? std::to_string(s.mapId) : s.dungeon) << '|' << (s.heroic ? 'H' : 'N') << '|'
          << (s.comp.empty() ? "random" : s.comp) << '|' << s.level << '|' << s.gearIlvl << '|' << s.human.slot;
        return k.str();
    }

    std::string Judge(Scenario const& s, std::vector<std::pair<std::string, std::string>> const& res)
    {
        bool const open = s.knownFailure.rfind("open:", 0) == 0;
        std::vector<std::string> unexpectedFails;
        std::size_t expectedFails = 0, reproduced = 0;
        for (auto const& kv : s.expected)
            if (kv.second == "fail")
                ++expectedFails;
        for (auto const& r : res)
        {
            auto it = s.expected.find(r.first);
            bool const wantFail = it != s.expected.end() && it->second == "fail";
            if (r.second == "fail")
            {
                if (wantFail)
                    ++reproduced;
                else
                    unexpectedFails.push_back(r.first);
            }
        }
        if (open)
        {
            if (expectedFails == 0)
                return unexpectedFails.empty() ? "unexpected-pass" : "expected-fail";
            if (!unexpectedFails.empty())
                return "fail";
            return reproduced == 0 ? "unexpected-pass" : "expected-fail";
        }
        if (!unexpectedFails.empty())
            return "fail";
        return "pass";
    }
}
