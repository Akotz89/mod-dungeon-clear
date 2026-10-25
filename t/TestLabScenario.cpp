/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Pull Lab P1: the scenario file format, comp tokens, id globbing and the
// museum verdict (Judge) — plus a sweep over every committed scenario so a
// malformed file under lab/scenarios fails here, not at `.dc lab batch` time.

#include <filesystem>

#include "gtest/gtest.h"

#include "Lab/DcLabJson.h"
#include "Lab/DcLabScenario.h"

using namespace DcLabScenario;

namespace
{
    Scenario ParseOk(char const* text)
    {
        DcLabJson::Value v;
        std::string err;
        EXPECT_TRUE(DcLabJson::Parse(text, v, &err)) << err;
        Scenario s;
        EXPECT_TRUE(Parse(v, "museum/x", s, &err)) << err;
        return s;
    }

    std::string ParseErr(char const* text)
    {
        DcLabJson::Value v;
        std::string err;
        EXPECT_TRUE(DcLabJson::Parse(text, v, &err)) << err;
        Scenario s;
        EXPECT_FALSE(Parse(v, "museum/x", s, &err));
        return err;
    }

    char const* kFull = R"({
        "about": "corridor with cells",
        "geometry": {"map": 34, "region": {"center": [1, 2, 3], "radius": 50}, "objective": 1716},
        "actors": {"native": {"packs": {"side": [7, 8]}, "keep": [5, 6, 7]},
                   "synthetic": [{"tag": "add1", "entry": 1708, "pos": [1, 2, 3, 0.5], "spawn": "onInject"}]},
        "party": {"comp": "warrior-tank,priest,mage,rogue,hunter", "start": [10, 20, 30, 1.5],
                  "react": "masterless", "human": {"slot": 4, "script": "humans/x"}},
        "settings": {"PullSetting": 2, "PullDynamicMaxLeeroyMobs": 3},
        "injections": [
            {"when": {"phaseEnter": "Returning", "delayMs": 400}, "do": {"spawn": "add1", "aggro": "heal"}},
            {"when": {"event": "aggroConfirmed"}, "do": {"stun": "tank"}, "label": "stun-on-aggro"},
            {"when": {"predicate": "tankDistToCamp < 10"}, "do": {"kill": "pack:side"}}
        ],
        "goal": {"kill": "pack:target", "timeoutS": 60},
        "expect": {"oracles": {"O1": "pass", "O7": {"joinersMax": 1, "expect": "fail"}}, "knownFailure": "open:#17"}
    })";
}

TEST(DcLabScenario, ParsesTheFullSchema)
{
    Scenario const s = ParseOk(kFull);
    EXPECT_EQ(s.id, "museum/x");
    EXPECT_EQ(s.mapId, 34u);
    EXPECT_FLOAT_EQ(s.radius, 50.0f);
    EXPECT_EQ(s.objective, 1716u);
    // keep -> target, except 7 which packs already put in "side".
    ASSERT_EQ(s.packs.at("target").size(), 2u);
    EXPECT_EQ(s.PackOf(7), "side");
    EXPECT_EQ(s.PackOf(5), "target");
    EXPECT_FALSE(s.Keeps(99));
    ASSERT_EQ(s.synthetic.size(), 1u);
    EXPECT_EQ(s.synthetic[0].pack, "add1");
    EXPECT_FLOAT_EQ(s.start.o, 1.5f);
    EXPECT_EQ(s.react, "masterless");
    EXPECT_EQ(s.human.slot, 4);
    EXPECT_EQ(s.pullSetting, 2);
    ASSERT_EQ(s.settings.size(), 1u);
    EXPECT_EQ(s.settings[0].first, "PullDynamicMaxLeeroyMobs");
    ASSERT_EQ(s.injections.size(), 3u);
    EXPECT_EQ(s.injections[0].when.kind, When::Kind::PhaseEnter);
    EXPECT_EQ(s.injections[0].when.phase, 3u);
    EXPECT_EQ(s.injections[0].what.op, "spawn");
    EXPECT_EQ(s.injections[0].what.args["aggro"].AsString(), "heal");
    EXPECT_EQ(s.injections[0].label, "spawn@Returning+400");
    EXPECT_EQ(s.injections[1].label, "stun-on-aggro");
    EXPECT_EQ(s.injections[2].when.lhs, "tankDistToCamp");
    EXPECT_EQ(s.injections[2].when.op, "<");
    EXPECT_EQ(s.goalPacks, std::vector<std::string>{"target"});
    EXPECT_EQ(s.timeoutS, 60u);
    EXPECT_EQ(s.expected.at("O7"), "fail");
    EXPECT_EQ(s.expected.at("O1"), "pass");
}

TEST(DcLabScenario, RejectsBadFiles)
{
    EXPECT_NE(ParseErr(R"({"party": {"start": [1,2,3]}})").find("map"), std::string::npos);
    EXPECT_NE(ParseErr(R"({"geometry": {"map": 1}, "party": {}})").find("start"), std::string::npos);
    EXPECT_NE(ParseErr(R"({"geometry": {"map": 1}, "party": {"start": [1,2,3]}, "actors": {"native": {"keep": [1]}},
                           "injections": [{"when": {"event": "nope"}, "do": {"kill": "tank"}}]})")
                  .find("unknown event"),
              std::string::npos);
    EXPECT_NE(ParseErr(R"({"geometry": {"map": 1}, "party": {"start": [1,2,3]}, "actors": {"native": {"keep": [1]}},
                           "injections": [{"when": {"atMs": 5}, "do": {"spawn": "ghost"}}]})")
                  .find("unknown synthetic"),
              std::string::npos);
    EXPECT_NE(ParseErr(R"({"geometry": {"map": 1}, "party": {"start": [1,2,3]}, "actors": {"native": {"keep": [1]}},
                           "goal": {"kill": "pack:nobody"}})")
                  .find("no members"),
              std::string::npos);
    EXPECT_NE(ParseErr(R"({"geometry": {"map": 1}, "party": {"start": [1,2,3], "human": {"slot": 0}},
                           "actors": {"native": {"keep": [1]}}})")
                  .find("slot"),
              std::string::npos);
}

TEST(DcLabScenario, CompTokens)
{
    std::vector<DcTestComp::Slot> c;
    std::string err;
    ASSERT_TRUE(ParseComp("warrior-tank,priest,mage,rogue,hunter", 0, DcTestComp::Roster::NoDeathKnights, c, &err))
        << err;
    ASSERT_EQ(c.size(), 5u);
    EXPECT_EQ(c[0].classId, 1);
    EXPECT_STREQ(c[0].role, "tank");
    EXPECT_STREQ(c[1].role, "heal");  // positional: a bare priest in slot 1 heals
    EXPECT_STREQ(c[2].role, "dps");

    ASSERT_TRUE(ParseComp("paladin, priest-shadow, druid-heal, mage-frost", 0, DcTestComp::Roster::NoDeathKnights,
                          c, &err))
        << err;
    EXPECT_STREQ(c[1].specName, "shadow pve");
    EXPECT_STREQ(c[2].role, "heal");
    EXPECT_STREQ(c[3].specName, "frost pve");
    EXPECT_EQ(c[3].classId, 8);

    EXPECT_FALSE(ParseComp("mage,priest,rogue", 0, DcTestComp::Roster::NoDeathKnights, c, &err));  // mage can't tank
    EXPECT_FALSE(ParseComp("warrior,bard", 0, DcTestComp::Roster::NoDeathKnights, c, &err));
    EXPECT_FALSE(ParseComp("dk-tank,priest", 0, DcTestComp::Roster::NoDeathKnights, c, &err));

    ASSERT_TRUE(ParseComp("", 7, DcTestComp::Roster::NoDeathKnights, c, &err));
    EXPECT_EQ(c.size(), 5u);
}

TEST(DcLabScenario, GlobAndPartyKey)
{
    EXPECT_TRUE(GlobMatch("museum/*", "museum/outside/stockade"));
    EXPECT_TRUE(GlobMatch("*stockade*", "museum/outside/stockade-cells"));
    EXPECT_FALSE(GlobMatch("museum/*", "smoke/x"));
    EXPECT_TRUE(GlobMatch("exact", "exact"));

    Scenario a = ParseOk(kFull), b = ParseOk(kFull);
    EXPECT_EQ(PartyKey(a), PartyKey(b));
    b.comp = "warrior,priest,mage,rogue,warlock";
    EXPECT_NE(PartyKey(a), PartyKey(b));
}

TEST(DcLabScenario, JudgeMuseumVerdicts)
{
    Scenario s = ParseOk(kFull);  // open:#17, O7 expected to fail
    std::vector<std::pair<std::string, std::string>> r = {{"O1", "pass"}, {"O7", "fail"}};
    EXPECT_EQ(Judge(s, r), "expected-fail");
    r = {{"O1", "pass"}, {"O7", "pass"}};
    EXPECT_EQ(Judge(s, r), "unexpected-pass");
    r = {{"O1", "fail"}, {"O7", "fail"}};
    EXPECT_EQ(Judge(s, r), "fail");

    s.knownFailure.clear();
    s.expected.clear();
    r = {{"O1", "pass"}, {"O9", "na"}};
    EXPECT_EQ(Judge(s, r), "pass");
    r = {{"O1", "fail"}};
    EXPECT_EQ(Judge(s, r), "fail");
}

// Every committed scenario must load. DC_LAB_DIR overrides the location (the
// test binary runs from the build tree).
TEST(DcLabScenario, CommittedScenariosLoad)
{
    std::string dir = std::string(__FILE__);
    dir = dir.substr(0, dir.rfind("/t/")) + "/lab/scenarios";
    if (char const* env = std::getenv("DC_LAB_DIR"))
        dir = std::string(env) + "/scenarios";
    auto const files = List(dir);
    if (files.empty())
        GTEST_SKIP() << "no scenarios under " << dir;
    for (auto const& [id, path] : files)
    {
        Scenario s;
        std::string err;
        EXPECT_TRUE(LoadFile(path, id, s, &err)) << id << ": " << err;
        std::vector<DcTestComp::Slot> comp;
        EXPECT_TRUE(ParseComp(s.comp, 1, DcTestComp::Roster::WithDeathKnights, comp, &err)) << id << ": " << err;
        EXPECT_NE(s.objective, 0u) << id << ": a committed scenario must name its objective boss";
    }
}

TEST(DcLabScenario, SweepExpandsTheFlaggedInjection)
{
    DcLabJson::Value v;
    ASSERT_TRUE(DcLabJson::Parse(R"({
        "geometry": {"map": 1, "objective": 2}, "party": {"start": [0, 0, 0, 0]},
        "actors": {"native": {"keep": [1]}, "synthetic": [{"tag": "a", "entry": 5, "pos": [1, 1, 1]}]},
        "injections": [
            {"when": {"atMs": 100}, "do": {"note": "first"}},
            {"when": {"atMs": 200}, "do": {"spawn": "a", "aggro": "heal"}, "sweep": true}
        ]})", v));
    Scenario s;
    std::string err;
    ASSERT_TRUE(Parse(v, "x", s, &err)) << err;
    auto const all = SweepPoints(s, "all", &err);
    ASSERT_EQ(all.size(), 25u);
    EXPECT_EQ(all[0].first, "Forming+0");
    EXPECT_EQ(all[0].second.injections[1].when.kind, When::Kind::PhaseEnter);
    EXPECT_EQ(all[0].second.injections[0].when.atMs, 100u);  // the other injection is untouched
    EXPECT_EQ(all[12].first, "Returning+400");
    EXPECT_EQ(all[12].second.injections[1].when.delayMs, 400u);
    EXPECT_EQ(all[12].second.injections[1].label, "spawn@Returning+400");
    EXPECT_EQ(all.back().first, "event:safetyRelease");
    EXPECT_EQ(SweepPoints(s, "phases", &err).size(), 20u);
    EXPECT_EQ(SweepPoints(s, "events", &err).size(), 5u);
    EXPECT_TRUE(SweepPoints(s, "bogus", &err).empty());

    Scenario none = s;
    none.injections.clear();
    EXPECT_TRUE(SweepPoints(none, "all", &err).empty());
}

#include "Lab/DcLabTrigger.h"

namespace
{
    When PhaseWhen(char const* phase, std::uint32_t delay, std::uint32_t occurrence = 1)
    {
        When w;
        w.kind = When::Kind::PhaseEnter;
        PhaseFromName(phase, w.phase);
        w.delayMs = delay;
        w.occurrence = occurrence;
        return w;
    }
}

TEST(DcLabTrigger, PhaseEnterFiresAfterItsDelay)
{
    DcLabTrigger t(PhaseWhen("Returning", 400));
    t.Feed({{1000, "phase:Advancing"}});
    EXPECT_FALSE(t.Due(1500, false));
    t.Feed({{2000, "phase:Returning"}});
    EXPECT_FALSE(t.Due(2399, false));
    EXPECT_TRUE(t.Due(2400, false));
    t.MarkFired();
    EXPECT_FALSE(t.Due(9000, false));
}

TEST(DcLabTrigger, NthOccurrenceAndAliases)
{
    DcLabTrigger second(PhaseWhen("Returning", 0, 2));
    second.Feed({{1000, "phase:Returning"}, {5000, "phase:Engage"}});
    EXPECT_FALSE(second.Due(6000, false));
    second.Feed({{8000, "phase:Returning"}});
    EXPECT_TRUE(second.Due(8000, false));

    When ev;
    ev.kind = When::Kind::Event;
    ev.event = "plant";  // alias of campReached
    DcLabTrigger plant(ev);
    plant.Feed({{3000, "campReached"}});
    EXPECT_TRUE(plant.Due(3000, false));

    // endCampFight: an Idle only counts after an Engage.
    ev.event = "endCampFight";
    DcLabTrigger end(ev);
    end.Feed({{100, "phase:Idle"}});
    EXPECT_FALSE(end.Due(200, false));
    end.Feed({{500, "phase:Engage"}, {900, "phase:Idle"}});
    EXPECT_TRUE(end.Due(900, false));
}

TEST(DcLabTrigger, AtMsAndPredicate)
{
    When at;
    at.kind = When::Kind::AtMs;
    at.atMs = 2500;
    DcLabTrigger t(at);
    EXPECT_FALSE(t.Due(2000, false));
    EXPECT_TRUE(t.Due(2500, false));

    When p;
    p.kind = When::Kind::Predicate;
    p.delayMs = 300;
    DcLabTrigger q(p);
    EXPECT_FALSE(q.Due(1000, false));
    EXPECT_FALSE(q.Due(1100, true));  // armed now, fires at 1400
    EXPECT_TRUE(q.Due(1400, false));  // stays armed once true
}
