/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Lab/DcLabTrace.h"

#include <map>
#include <sstream>

#include "Lab/DcLabJson.h"

namespace DcLab
{
    char const* PhaseName(std::uint32_t p)
    {
        switch (p)
        {
            case 0: return "Idle";
            case 1: return "Forming";
            case 2: return "Advancing";
            case 3: return "Returning";
            case 4: return "Engage";
            default: return "?";
        }
    }

    bool PhaseFromName(std::string const& s, std::uint32_t& out)
    {
        for (std::uint32_t p = 0; p <= 4; ++p)
            if (s == PhaseName(p))
            {
                out = p;
                return true;
            }
        return false;
    }

    char const* DecisionName(std::uint32_t d)
    {
        switch (d)
        {
            case 0: return "None";
            case 1: return "Leeroy";
            case 2: return "Advanced";
            case 3: return "PatrolHold";
            default: return "?";
        }
    }

    char const* EngineName(std::uint8_t e)
    {
        switch (e)
        {
            case 0: return "C";
            case 1: return "NC";
            case 2: return "D";
            default: return "-";
        }
    }

    char const* VerdictName(Verdict v)
    {
        switch (v)
        {
            case Verdict::Pass: return "pass";
            case Verdict::Fail: return "fail";
            default: return "na";
        }
    }

    Member const* Trace::FindMember(std::uint64_t guid) const
    {
        for (Member const& m : header.party)
            if (m.guid == guid)
                return &m;
        return nullptr;
    }

    Unit const* Trace::FindUnit(std::uint64_t guid) const
    {
        for (Unit const& u : units)
            if (u.guid == guid)
                return &u;
        return nullptr;
    }

    Member const* Trace::Leader() const
    {
        for (Member const& m : header.party)
            if (m.leader)
                return &m;
        return header.party.empty() ? nullptr : &header.party.front();
    }

    namespace
    {
        std::uint8_t EngineFromName(std::string const& s)
        {
            if (s == "C")
                return 0;
            if (s == "NC")
                return 1;
            if (s == "D")
                return 2;
            return kEngineNone;
        }
    }

    std::string ToJsonl(Trace const& tr)
    {
        using DcLabJson::Line;
        std::ostringstream out;
        Header const& h = tr.header;
        {
            Line l;
            l.Add("k", "hdr").Add("schema", h.schema).Add("scenario", h.scenario)
                .Add("runId", h.runId).Add("sweep", h.sweepPoint).Add("seed", h.seed)
                .Add("sha", h.moduleSha).Add("pbSha", h.playerbotsSha).Add("map", h.mapId)
                .Add("instance", h.instanceId).Add("pullSetting", h.pullSetting)
                .Add("releaseDelayMs", h.releaseDelayMs).Add("timeoutMs", h.timeoutMs);
            std::string goals;
            for (std::string const& g : h.goalPacks)
                goals += (goals.empty() ? "" : ",") + g;
            l.Add("goal", goals);
            out << l.Str() << '\n';
        }
        for (Setting const& s : h.settings)
            out << Line().Add("k", "setting").Add("key", s.key).Add("v", s.value).Str() << '\n';
        for (Member const& m : h.party)
            out << Line().Add("k", "member").Guid("g", m.guid).Add("name", m.name)
                       .Add("role", m.role).Add("cls", m.cls).Add("human", m.human)
                       .Add("leader", m.leader).Add("react", m.react).Str()
                << '\n';
        for (Unit const& u : tr.units)
            out << Line().Add("k", "unit").Guid("g", u.guid).Add("entry", u.entry)
                       .Add("spawnId", u.spawnId).Add("name", u.name).Add("pack", u.pack)
                       .Add("boss", u.boss).Str()
                << '\n';

        // Frames, events and actions interleaved in time order so the file reads
        // (and greps) as a timeline.
        std::size_t fi = 0, ei = 0, ai = 0;
        auto const nextT = [&](std::size_t i, auto const& vec) -> std::uint64_t
        {
            return i < vec.size() ? vec[i].t : UINT64_MAX;
        };
        while (fi < tr.frames.size() || ei < tr.events.size() || ai < tr.actions.size())
        {
            std::uint64_t const tf = nextT(fi, tr.frames);
            std::uint64_t const te = nextT(ei, tr.events);
            std::uint64_t const ta = nextT(ai, tr.actions);
            if (tf <= te && tf <= ta)
            {
                Frame const& f = tr.frames[fi++];
                for (UnitSample const& s : f.units)
                    out << Line().Add("k", "fu").Add("t", f.t).Guid("g", s.guid).Add("x", s.x)
                               .Add("y", s.y).Add("z", s.z).Add("hp", static_cast<std::uint32_t>(s.hpPct))
                               .Add("alive", s.alive).Add("cbt", s.inCombat).Add("evade", s.evading)
                               .Add("mov", s.moving).Add("mot", static_cast<std::uint32_t>(s.motion))
                               .Guid("vic", s.victim).Guid("thr", s.threat).Str()
                        << '\n';
                for (BotSample const& s : f.bots)
                    out << Line().Add("k", "fb").Add("t", f.t).Guid("g", s.guid).Add("x", s.x)
                               .Add("y", s.y).Add("z", s.z).Add("hp", static_cast<std::uint32_t>(s.hpPct))
                               .Add("alive", s.alive).Add("cbt", s.inCombat)
                               .Add("eng", EngineName(s.engine)).Guid("vic", s.victim)
                               .Guid("tgt", s.target).Add("passive", s.passive).Add("stay", s.stay)
                               .Add("cast", s.casting).Add("dmg", s.dmgDone).Add("heal", s.healDone)
                               .Str()
                        << '\n';
                DcSample const& d = f.dc;
                if (d.valid)
                    out << Line().Add("k", "fd").Add("t", f.t).Add("phase", PhaseName(d.phase))
                               .Add("decision", DecisionName(d.decision)).Add("seq", d.decisionSeq)
                               .Add("pred", d.predicted).Add("ceil", d.ceiling).Add("cx", d.campX)
                               .Add("cy", d.campY).Add("cz", d.campZ).Guid("pullT", d.pullTarget)
                               .Guid("tagT", d.tagTarget).Guid("abortT", d.abortTarget)
                               .Add("released", d.partyReleased).Add("los", d.losPull)
                               .Add("scout", d.scoutAggro).Add("on", d.enabled).Add("paused", d.paused)
                               .Str()
                        << '\n';
                else
                    out << Line().Add("k", "fd").Add("t", f.t).Add("valid", false).Str() << '\n';
            }
            else if (te <= ta)
            {
                Event const& e = tr.events[ei++];
                out << Line().Add("k", "ev").Add("t", e.t).Add("ev", e.ev).Guid("a", e.a)
                           .Guid("b", e.b).Add("v", e.v).Add("s", e.s).Add("s2", e.s2).Str()
                    << '\n';
            }
            else
            {
                ActionRec const& a = tr.actions[ai++];
                out << Line().Add("k", "act").Add("t", a.t).Guid("g", a.guid)
                           .Add("eng", EngineName(a.engine)).Add("a", a.action)
                           .Add("rel", a.relevance).Add("x", a.executed).Add("src", a.source).Str()
                    << '\n';
            }
        }
        out << Line().Add("k", "end").Add("t", tr.endMs).Add("reason", tr.endReason).Str() << '\n';
        for (OracleResult const& o : tr.oracles)
            out << Line().Add("k", "oracle").Add("id", o.id).Add("name", o.name)
                       .Add("res", VerdictName(o.verdict)).Add("t", o.firstMs).Guid("g", o.unit)
                       .Add("n", o.count).Add("detail", o.detail).Str()
                << '\n';
        return out.str();
    }

    bool FromJsonl(std::string const& text, Trace& out, std::size_t* badLines)
    {
        out = Trace{};
        std::size_t bad = 0;
        std::map<std::uint32_t, std::size_t> frameAt;  // t -> index in out.frames
        auto frameFor = [&](std::uint32_t t) -> Frame&
        {
            auto it = frameAt.find(t);
            if (it != frameAt.end())
                return out.frames[it->second];
            frameAt[t] = out.frames.size();
            out.frames.push_back(Frame{});
            out.frames.back().t = t;
            return out.frames.back();
        };

        std::istringstream in(text);
        std::string line;
        bool sawHeader = false;
        while (std::getline(in, line))
        {
            if (line.empty() || line[0] == '#')
                continue;
            DcLabJson::Value v;
            if (!DcLabJson::Parse(line, v) || !v.IsObject())
            {
                ++bad;
                continue;
            }
            std::string const k = v["k"].AsString();
            std::uint32_t const t = static_cast<std::uint32_t>(v["t"].AsU64());
            if (k == "hdr")
            {
                sawHeader = true;
                Header& h = out.header;
                h.schema = static_cast<std::uint32_t>(v["schema"].AsU64(kTraceSchema));
                h.scenario = v["scenario"].AsString();
                h.runId = v["runId"].AsString();
                h.sweepPoint = v["sweep"].AsString();
                h.seed = static_cast<std::uint32_t>(v["seed"].AsU64());
                h.moduleSha = v["sha"].AsString();
                h.playerbotsSha = v["pbSha"].AsString();
                h.mapId = static_cast<std::uint32_t>(v["map"].AsU64());
                h.instanceId = static_cast<std::uint32_t>(v["instance"].AsU64());
                h.pullSetting = static_cast<std::uint32_t>(v["pullSetting"].AsU64());
                h.releaseDelayMs = static_cast<std::uint32_t>(v["releaseDelayMs"].AsU64());
                h.timeoutMs = static_cast<std::uint32_t>(v["timeoutMs"].AsU64());
                std::string const goals = v["goal"].AsString();
                std::size_t from = 0;
                while (from < goals.size())
                {
                    std::size_t const comma = goals.find(',', from);
                    std::string const g = goals.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
                    if (!g.empty())
                        h.goalPacks.push_back(g);
                    if (comma == std::string::npos)
                        break;
                    from = comma + 1;
                }
            }
            else if (k == "setting")
                out.header.settings.push_back({v["key"].AsString(), v["v"].AsNumber()});
            else if (k == "member")
            {
                Member m;
                m.guid = v["g"].AsU64();
                m.name = v["name"].AsString();
                m.role = v["role"].AsString();
                m.cls = v["cls"].AsString();
                m.human = v["human"].AsBool();
                m.leader = v["leader"].AsBool();
                m.react = v["react"].AsString();
                out.header.party.push_back(m);
            }
            else if (k == "unit")
            {
                Unit u;
                u.guid = v["g"].AsU64();
                u.entry = static_cast<std::uint32_t>(v["entry"].AsU64());
                u.spawnId = v["spawnId"].AsU64();
                u.name = v["name"].AsString();
                u.pack = v["pack"].AsString();
                u.boss = v["boss"].AsBool();
                out.units.push_back(u);
            }
            else if (k == "fu")
            {
                UnitSample s;
                s.guid = v["g"].AsU64();
                s.x = static_cast<float>(v["x"].AsNumber());
                s.y = static_cast<float>(v["y"].AsNumber());
                s.z = static_cast<float>(v["z"].AsNumber());
                s.hpPct = static_cast<std::uint8_t>(v["hp"].AsU64());
                s.alive = v["alive"].AsBool();
                s.inCombat = v["cbt"].AsBool();
                s.evading = v["evade"].AsBool();
                s.moving = v["mov"].AsBool();
                s.motion = static_cast<std::uint8_t>(v["mot"].AsU64());
                s.victim = v["vic"].AsU64();
                s.threat = v["thr"].AsU64();
                frameFor(t).units.push_back(s);
            }
            else if (k == "fb")
            {
                BotSample s;
                s.guid = v["g"].AsU64();
                s.x = static_cast<float>(v["x"].AsNumber());
                s.y = static_cast<float>(v["y"].AsNumber());
                s.z = static_cast<float>(v["z"].AsNumber());
                s.hpPct = static_cast<std::uint8_t>(v["hp"].AsU64());
                s.alive = v["alive"].AsBool();
                s.inCombat = v["cbt"].AsBool();
                s.engine = EngineFromName(v["eng"].AsString());
                s.victim = v["vic"].AsU64();
                s.target = v["tgt"].AsU64();
                s.passive = v["passive"].AsBool();
                s.stay = v["stay"].AsBool();
                s.casting = v["cast"].AsBool();
                s.dmgDone = v["dmg"].AsU64();
                s.healDone = v["heal"].AsU64();
                frameFor(t).bots.push_back(s);
            }
            else if (k == "fd")
            {
                DcSample& d = frameFor(t).dc;
                if (v.Has("valid") && !v["valid"].AsBool())
                    continue;
                d.valid = true;
                PhaseFromName(v["phase"].AsString(), d.phase);
                std::string const dec = v["decision"].AsString();
                for (std::uint32_t c = 0; c <= 3; ++c)
                    if (dec == DecisionName(c))
                        d.decision = c;
                d.decisionSeq = static_cast<std::uint32_t>(v["seq"].AsU64());
                d.predicted = static_cast<std::uint32_t>(v["pred"].AsU64());
                d.ceiling = static_cast<std::uint32_t>(v["ceil"].AsU64());
                d.campX = static_cast<float>(v["cx"].AsNumber());
                d.campY = static_cast<float>(v["cy"].AsNumber());
                d.campZ = static_cast<float>(v["cz"].AsNumber());
                d.pullTarget = v["pullT"].AsU64();
                d.tagTarget = v["tagT"].AsU64();
                d.abortTarget = v["abortT"].AsU64();
                d.partyReleased = v["released"].AsBool();
                d.losPull = v["los"].AsBool();
                d.scoutAggro = v["scout"].AsBool();
                d.enabled = v["on"].AsBool();
                d.paused = v["paused"].AsBool();
            }
            else if (k == "ev")
            {
                Event e;
                e.t = t;
                e.ev = v["ev"].AsString();
                e.a = v["a"].AsU64();
                e.b = v["b"].AsU64();
                e.v = v["v"].AsI64();
                e.s = v["s"].AsString();
                e.s2 = v["s2"].AsString();
                out.events.push_back(e);
            }
            else if (k == "act")
            {
                ActionRec a;
                a.t = t;
                a.guid = v["g"].AsU64();
                a.engine = EngineFromName(v["eng"].AsString());
                a.action = v["a"].AsString();
                a.relevance = static_cast<float>(v["rel"].AsNumber());
                a.executed = v["x"].AsBool();
                a.source = v["src"].AsString();
                out.actions.push_back(a);
            }
            else if (k == "end")
            {
                out.endMs = t;
                out.endReason = v["reason"].AsString();
            }
            else if (k == "oracle")
            {
                OracleResult o;
                o.id = v["id"].AsString();
                o.name = v["name"].AsString();
                std::string const res = v["res"].AsString();
                o.verdict = res == "pass" ? Verdict::Pass : res == "fail" ? Verdict::Fail : Verdict::NotApplicable;
                o.firstMs = t;
                o.unit = v["g"].AsU64();
                o.count = static_cast<std::uint32_t>(v["n"].AsU64());
                o.detail = v["detail"].AsString();
                out.oracles.push_back(o);
            }
        }
        if (badLines)
            *badLines = bad;
        return sawHeader;
    }
}
