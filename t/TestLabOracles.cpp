/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Pull Lab P0: the JSON reader, the LabTrace JSONL round trip, and each of the
// ten oracles against small canned traces — one that must fail and one that
// must pass (or be exempt) per oracle, so a threshold or exemption change shows
// up here before it shows up as a museum verdict flip.

#include "gtest/gtest.h"

#include "Lab/DcLabJson.h"
#include "Lab/DcLabOracles.h"
#include "Lab/DcLabTrace.h"

using namespace DcLab;
using DcLabOracles::OracleConfig;

namespace
{
    constexpr std::uint64_t T = 1, H = 2, D1 = 3, D2 = 4, D3 = 5, HUM = 6;
    // Creature GUIDs carry high-type bits past 2^53 — exercise the hex path.
    constexpr std::uint64_t M1 = 0xF130000000000101ull, M2 = 0xF130000000000102ull,
                            M3 = 0xF130000000000103ull, X1 = 0xF1300000000001FFull;

    struct Builder
    {
        Trace tr;

        Builder(bool human = false)
        {
            tr.header.runId = "lr-test";
            tr.header.releaseDelayMs = 1500;
            tr.header.party = {{T, "Tank", "tank", "warrior", false, true, "fast"},
                               {H, "Heal", "heal", "priest", false, false, "fast"},
                               {D1, "Dps1", "dps", "mage", false, false, "fast"},
                               {D2, "Dps2", "dps", "rogue", false, false, "fast"},
                               {D3, "Dps3", "dps", "hunter", human, false, "fast"}};
            if (human)
                tr.header.party[4].guid = HUM;
            tr.units = {{M1, 100, 11, "Mob1", "target", false},
                        {M2, 100, 12, "Mob2", "target", false},
                        {M3, 100, 13, "Mob3", "target", false},
                        {X1, 200, 99, "Outsider", "side", false}};
            tr.header.goalPacks = {"target"};
        }

        // Frames 0..endMs every 250ms, every unit idle/alive at (50,0), every
        // bot alive at (0,0) out of combat; then `fn(frame)` customises each.
        template <typename Fn>
        Builder& Frames(std::uint32_t endMs, Fn fn)
        {
            for (std::uint32_t t = 0; t <= endMs; t += kFrameMs)
            {
                Frame f;
                f.t = t;
                for (DcLab::Unit const& u : tr.units)
                {
                    UnitSample s;
                    s.guid = u.guid;
                    s.x = 50;
                    s.alive = true;
                    s.hpPct = 100;
                    f.units.push_back(s);
                }
                for (Member const& m : tr.header.party)
                {
                    BotSample b;
                    b.guid = m.guid;
                    b.alive = true;
                    b.hpPct = 100;
                    b.engine = 1;
                    f.bots.push_back(b);
                }
                f.dc.valid = true;
                f.dc.enabled = true;
                fn(f);
                tr.frames.push_back(f);
            }
            tr.endMs = endMs;
            tr.endReason = "goal";
            return *this;
        }

        Builder& Ev(std::uint32_t t, char const* ev, std::uint64_t a, std::uint64_t b = 0, std::int64_t v = 0)
        {
            Event e;
            e.t = t;
            e.ev = ev;
            e.a = a;
            e.b = b;
            e.v = v;
            tr.events.push_back(e);
            return *this;
        }

        Builder& Act(std::uint32_t from, std::uint32_t to, std::uint64_t g, char const* name)
        {
            for (std::uint32_t t = from; t <= to; t += 100)
            {
                ActionRec a;
                a.t = t;
                a.guid = g;
                a.engine = 0;
                a.action = name;
                a.relevance = 30.0f;
                a.executed = true;
                tr.actions.push_back(a);
            }
            return *this;
        }
    };

    UnitSample& U(Frame& f, std::uint64_t g)
    {
        for (UnitSample& u : f.units)
            if (u.guid == g)
                return u;
        return f.units.front();
    }
    BotSample& B(Frame& f, std::uint64_t g)
    {
        for (BotSample& b : f.bots)
            if (b.guid == g)
                return b;
        return f.bots.front();
    }
    void KillGoal(Frame& f)
    {
        U(f, M1).alive = U(f, M2).alive = U(f, M3).alive = false;
    }

    OracleResult Oracle(Trace const& tr, char const* id, OracleConfig const& c = OracleConfig{})
    {
        for (OracleResult const& r : DcLabOracles::Evaluate(tr, c))
            if (r.id == id)
                return r;
        return {};
    }
}

// ------------------------------------------------------------------- JSON

TEST(DcLabJson, ParsesNestedWithCommentsAndTrailingCommas)
{
    DcLabJson::Value v;
    std::string err;
    ASSERT_TRUE(DcLabJson::Parse(R"({
        // a scenario note
        "id": "museum/x", # hash comment too
        "geometry": {"map": 34, "center": [1.5, -2, 3e1],},
        "flag": true, "none": null, "s": "a,\"b\"A",
    })", v, &err)) << err;
    EXPECT_EQ(v["id"].AsString(), "museum/x");
    EXPECT_EQ(v["geometry"]["map"].AsU64(), 34u);
    ASSERT_EQ(v["geometry"]["center"].arr.size(), 3u);
    EXPECT_DOUBLE_EQ(v["geometry"]["center"].arr[2].AsNumber(), 30.0);
    EXPECT_TRUE(v["flag"].AsBool());
    EXPECT_TRUE(v["none"].IsNull());
    EXPECT_TRUE(v["missing"]["deeper"].IsNull());
    EXPECT_EQ(v["s"].AsString(), "a,\"b\"A");
}

TEST(DcLabJson, ReportsErrorPosition)
{
    DcLabJson::Value v;
    std::string err;
    EXPECT_FALSE(DcLabJson::Parse("{\n  \"a\": [1, 2\n}", v, &err));
    EXPECT_EQ(err.rfind("3:", 0), 0u) << err;
}

TEST(DcLabJson, LargeGuidsSurviveAsHex)
{
    std::string const line = DcLabJson::Line().Guid("g", M1).Add("n", "x,y").Str();
    DcLabJson::Value v;
    ASSERT_TRUE(DcLabJson::Parse(line, v));
    EXPECT_EQ(v["g"].AsU64(), M1);
    EXPECT_EQ(v["n"].AsString(), "x,y");
}

// ------------------------------------------------------------------- trace

TEST(DcLabTrace, JsonlRoundTrip)
{
    Builder b;
    b.Frames(1000, [](Frame& f) {
        f.dc.phase = 3;
        f.dc.decision = 2;
        f.dc.pullTarget = M1;
        U(f, M1).victim = T;
        B(f, D1).dmgDone = f.t;
    });
    b.Ev(500, Ev::FirstDmg, T, M1, 120).Act(0, 300, T, "dungeon clear pull maneuver");
    b.tr.oracles = DcLabOracles::Evaluate(b.tr, OracleConfig{});

    Trace back;
    std::size_t bad = 99;
    ASSERT_TRUE(FromJsonl(ToJsonl(b.tr), back, &bad));
    EXPECT_EQ(bad, 0u);
    EXPECT_EQ(back.header.runId, "lr-test");
    EXPECT_EQ(back.header.releaseDelayMs, 1500u);
    ASSERT_EQ(back.header.party.size(), 5u);
    EXPECT_TRUE(back.header.party[0].leader);
    ASSERT_EQ(back.frames.size(), b.tr.frames.size());
    EXPECT_EQ(back.frames[2].dc.phase, 3u);
    EXPECT_EQ(back.frames[2].dc.pullTarget, M1);
    EXPECT_EQ(U(back.frames[2], M1).victim, T);
    EXPECT_EQ(B(back.frames[4], D1).dmgDone, 1000u);
    ASSERT_EQ(back.events.size(), 1u);
    EXPECT_EQ(back.events[0].b, M1);
    EXPECT_EQ(back.actions.size(), 4u);
    EXPECT_EQ(back.endReason, "goal");
    ASSERT_EQ(back.oracles.size(), 10u);
    EXPECT_EQ(back.oracles[9].id, "O10");
}

// ------------------------------------------------------------------- O1

TEST(DcLabOracles, O1FailsOnMobLeftOnHealer)
{
    Builder b;
    b.Frames(5000, [](Frame& f) {
        UnitSample& m = U(f, M1);
        m.inCombat = true;
        m.victim = H;
        m.moving = true;  // chasing the healer, so not a parked caster
        m.x = 40;         // tank at 0 — 40yd away, not closing
    });
    OracleResult const r = Oracle(b.tr, "O1");
    EXPECT_EQ(r.verdict, Verdict::Fail) << r.detail;
    EXPECT_EQ(r.unit, M1);
    EXPECT_EQ(r.firstMs, 3000u);
}

TEST(DcLabOracles, O1PassesWhenTankClosesOrDrags)
{
    Builder closing;
    closing.Frames(5000, [](Frame& f) {
        UnitSample& m = U(f, M1);
        m.inCombat = true;
        m.victim = H;
        B(f, T).x = f.t / 250.0f;  // walks toward the mob at x=50
    });
    EXPECT_EQ(Oracle(closing.tr, "O1").verdict, Verdict::Pass);

    Builder drag;
    drag.Frames(5000, [](Frame& f) {
        f.dc.phase = 3;  // Returning
        UnitSample& m = U(f, M1);
        m.inCombat = true;
        m.victim = T;
    });
    EXPECT_EQ(Oracle(drag.tr, "O1").verdict, Verdict::Pass);
}

// ------------------------------------------------------------------- O2

TEST(DcLabOracles, O2FailsWhenDpsHitsFirst)
{
    Builder b;
    b.Frames(4000, [](Frame& f) { B(f, T).inCombat = f.t >= 1000; });
    b.Ev(800, Ev::FirstDmg, D1, M1, 300).Ev(1200, Ev::FirstDmg, T, M1, 100);
    OracleResult const r = Oracle(b.tr, "O2");
    EXPECT_EQ(r.verdict, Verdict::Fail);
    EXPECT_EQ(r.unit, D1);
}

TEST(DcLabOracles, O2FailsInsideReleaseDelayPassesAfter)
{
    Builder early;
    early.Frames(4000, [](Frame& f) { B(f, T).inCombat = f.t >= 1000; });
    early.Ev(1100, Ev::FirstDmg, T, M1, 100).Ev(1600, Ev::FirstDmg, D1, M1, 300);
    EXPECT_EQ(Oracle(early.tr, "O2").verdict, Verdict::Fail);

    Builder ok;
    ok.Frames(4000, [](Frame& f) { B(f, T).inCombat = f.t >= 1000; });
    ok.Ev(1100, Ev::FirstDmg, T, M1, 100).Ev(2600, Ev::FirstDmg, D1, M1, 300);
    EXPECT_EQ(Oracle(ok.tr, "O2").verdict, Verdict::Pass);
}

TEST(DcLabOracles, O2ExemptsSafetyReleaseAndSelfDefense)
{
    Builder rel;
    rel.Frames(4000, [](Frame& f) {
        B(f, T).inCombat = f.t >= 1000;
        f.dc.partyReleased = true;
    });
    rel.Ev(800, Ev::FirstDmg, T, M1, 10).Ev(900, Ev::FirstDmg, D1, M1, 300);
    EXPECT_EQ(Oracle(rel.tr, "O2").verdict, Verdict::Pass);

    Builder self;
    self.Frames(4000, [](Frame& f) {
        B(f, T).inCombat = f.t >= 1000;
        U(f, M2).victim = D2;
        U(f, M2).inCombat = true;
    });
    self.Ev(500, Ev::FirstDmg, D2, M2, 300);
    EXPECT_NE(Oracle(self.tr, "O2").verdict, Verdict::Fail);
}

// ------------------------------------------------------------------- O3

TEST(DcLabOracles, O3HeldMemberHurt)
{
    Builder b;
    b.Frames(3000, [](Frame& f) {
        BotSample& d = B(f, D2);
        d.passive = true;
        d.hpPct = static_cast<std::uint8_t>(100 - f.t / 50);  // 100 -> 40
    });
    EXPECT_EQ(Oracle(b.tr, "O3").verdict, Verdict::Fail);

    Builder released;
    released.Frames(3000, [](Frame& f) {
        BotSample& d = B(f, D2);
        d.passive = true;
        d.hpPct = static_cast<std::uint8_t>(100 - f.t / 50);
        f.dc.partyReleased = true;
    });
    EXPECT_NE(Oracle(released.tr, "O3").verdict, Verdict::Fail);
}

// ------------------------------------------------------------------- O4

TEST(DcLabOracles, O4DpsIdleWhileTankFights)
{
    Builder b;
    b.Frames(12000, [](Frame& f) {
        B(f, T).inCombat = true;
        U(f, M1).inCombat = true;
        U(f, M1).victim = T;
        for (std::uint64_t g : {D1, D2})
        {
            B(f, g).inCombat = true;
            B(f, g).dmgDone = f.t;  // keeps hitting
        }
    });
    OracleResult const r = Oracle(b.tr, "O4");
    EXPECT_EQ(r.verdict, Verdict::Fail);
    EXPECT_EQ(r.unit, D3);

    Builder held;
    held.Frames(12000, [](Frame& f) {
        B(f, T).inCombat = true;
        U(f, M1).inCombat = true;
        U(f, M1).victim = T;
        f.dc.phase = 3;
    });
    EXPECT_NE(Oracle(held.tr, "O4").verdict, Verdict::Fail);
}

// ------------------------------------------------------------------- O5

TEST(DcLabOracles, O5StarvedAction)
{
    Builder b;
    b.Frames(12000, [](Frame& f) { B(f, D1).inCombat = true; });
    b.Act(1000, 11000, D1, "dungeon clear regroup combat");
    OracleResult const r = Oracle(b.tr, "O5");
    EXPECT_EQ(r.verdict, Verdict::Fail);
    EXPECT_EQ(r.unit, D1);

    Builder moving;
    moving.Frames(12000, [](Frame& f) {
        B(f, D1).inCombat = true;
        B(f, D1).x = f.t / 100.0f;
    });
    moving.Act(1000, 11000, D1, "dungeon clear regroup combat");
    EXPECT_EQ(Oracle(moving.tr, "O5").verdict, Verdict::Pass);

    Builder exempt;
    exempt.Frames(12000, [](Frame& f) { B(f, D1).inCombat = true; });
    exempt.Act(1000, 11000, D1, "dungeon clear hold at camp");
    EXPECT_EQ(Oracle(exempt.tr, "O5").verdict, Verdict::Pass);
}

// ------------------------------------------------------------------- O6

TEST(DcLabOracles, O6PhaseDwell)
{
    Builder b;
    b.Frames(12000, [](Frame& f) { f.dc.phase = 1; });
    OracleResult const r = Oracle(b.tr, "O6");
    EXPECT_EQ(r.verdict, Verdict::Fail);
    EXPECT_EQ(r.firstMs, 9000u);

    Builder idle;
    idle.Frames(5000, [](Frame& f) {
        f.dc.decision = 2;
        B(f, T).inCombat = true;
    });
    EXPECT_EQ(Oracle(idle.tr, "O6").verdict, Verdict::Fail);
    idle.Act(500, 5000, T, "dungeon clear pull maneuver");
    EXPECT_EQ(Oracle(idle.tr, "O6").verdict, Verdict::Pass);
}

// ------------------------------------------------------------------- O7

TEST(DcLabOracles, O7OverPullAndOutsideJoiner)
{
    Builder b;
    b.Frames(6000, [](Frame& f) {
        f.dc.decisionSeq = 1;
        f.dc.predicted = 1;
        f.dc.pullTarget = M1;
        for (std::uint64_t g : {M1, M2, M3})
            if (f.t >= 1000)
            {
                U(f, g).inCombat = true;
                U(f, g).victim = T;
            }
    });
    OracleResult const r = Oracle(b.tr, "O7");
    EXPECT_EQ(r.verdict, Verdict::Fail);
    EXPECT_NE(r.detail.find("engaged 3 vs predicted 1"), std::string::npos) << r.detail;

    Builder side;
    side.Frames(6000, [](Frame& f) {
        f.dc.decisionSeq = 1;
        f.dc.predicted = 3;
        f.dc.pullTarget = M1;
        if (f.t >= 1000)
            for (std::uint64_t g : {M1, M2, M3})
            {
                U(f, g).inCombat = true;
                U(f, g).victim = T;
            }
        if (f.t >= 3000)
        {
            U(f, X1).inCombat = true;
            U(f, X1).victim = H;
            U(f, X1).x = 55;  // next to the engaged pack at 50
        }
    });
    OracleResult const s = Oracle(side.tr, "O7");
    EXPECT_EQ(s.verdict, Verdict::Fail);
    EXPECT_EQ(s.unit, X1);
    EXPECT_NE(s.detail.find("call-for-help"), std::string::npos) << s.detail;

    OracleConfig loose;
    loose.o7OutsideMax = -1;
    EXPECT_EQ(Oracle(side.tr, "O7", loose).verdict, Verdict::Pass);
}

// ------------------------------------------------------------------- O8

TEST(DcLabOracles, O8VerdictFlipsAndPingPong)
{
    Builder flips;
    flips.Frames(6000, [](Frame& f) { f.dc.decision = (f.t / 1000) % 2 ? 1 : 2; });
    EXPECT_EQ(Oracle(flips.tr, "O8").verdict, Verdict::Fail);

    Builder pong;
    pong.Frames(8000, [](Frame& f) { B(f, D1).x = (f.t / 250) % 2 ? 3.0f : 0.0f; });
    OracleResult const r = Oracle(pong.tr, "O8");
    EXPECT_EQ(r.verdict, Verdict::Fail);
    EXPECT_EQ(r.unit, D1);

    Builder calm;
    calm.Frames(8000, [](Frame& f) { B(f, D1).x = f.t / 100.0f; });
    EXPECT_EQ(Oracle(calm.tr, "O8").verdict, Verdict::Pass);
}

// ------------------------------------------------------------------- O9

TEST(DcLabOracles, O9HumanFightsAlone)
{
    Builder none;
    none.Frames(1000, [](Frame&) {});
    EXPECT_EQ(Oracle(none.tr, "O9").verdict, Verdict::NotApplicable);

    Builder b(true);
    b.Frames(8000, [](Frame& f) { B(f, HUM).inCombat = f.t >= 1000; });
    OracleResult const r = Oracle(b.tr, "O9");
    EXPECT_EQ(r.verdict, Verdict::Fail);
    EXPECT_EQ(r.unit, HUM);
}

// ------------------------------------------------------------------- O10

TEST(DcLabOracles, O10Outcome)
{
    Builder win;
    win.Frames(2000, [](Frame& f) {
        if (f.t >= 1500)
            KillGoal(f);
    });
    EXPECT_EQ(Oracle(win.tr, "O10").verdict, Verdict::Pass);

    Builder alive;
    alive.Frames(2000, [](Frame&) {});
    alive.tr.endReason = "timeout";
    EXPECT_EQ(Oracle(alive.tr, "O10").verdict, Verdict::Fail);

    Builder death;
    death.Frames(2000, [](Frame& f) {
        KillGoal(f);
        B(f, H).alive = f.t < 1000;
    });
    OracleResult const r = Oracle(death.tr, "O10");
    EXPECT_EQ(r.verdict, Verdict::Fail);
    EXPECT_EQ(r.unit, H);
}

// ------------------------------------------------------------------- config

TEST(DcLabOracles, ApplyOverridesByName)
{
    DcLabJson::Value v;
    ASSERT_TRUE(DcLabJson::Parse(R"({"O7": {"joinersMax": 3, "expect": "fail"},
                                     "O1": {"unattendedS": 5.5}, "O2": "pass",
                                     "O9": {"bogus": 1}})", v));
    OracleConfig c;
    std::string unknown;
    DcLabOracles::ApplyOverrides(c, v, &unknown);
    EXPECT_EQ(c.o7JoinersMax, 3);
    EXPECT_EQ(c.o1UnattendedMs, 5500u);
    EXPECT_EQ(unknown, "O9.bogus");
}

TEST(DcLabOracles, SummaryLine)
{
    Builder b;
    b.Frames(12000, [](Frame& f) { f.dc.phase = 1; });
    std::string const s = DcLabOracles::Summary(DcLabOracles::Evaluate(b.tr, OracleConfig{}));
    EXPECT_NE(s.find("O6:fail@9.0s"), std::string::npos) << s;
    EXPECT_NE(s.find("O9:na"), std::string::npos) << s;
}

// ------------------------------------------------------------- calibration
// From the first live baseline (lb-20261003-154150-2): three exemptions every
// run tripped falsely.

TEST(DcLabOracles, O2MobAlreadyOnTheTankIsHisAfterTheReleaseDelay)
{
    Builder b;
    b.Frames(6000, [](Frame& f) {
        B(f, T).inCombat = f.t >= 1000;
        U(f, M1).inCombat = f.t >= 1000;
        U(f, M1).victim = T;  // dragged home on the tank, not yet hit by him
    });
    b.Ev(3000, Ev::FirstDmg, D1, M1, 300).Ev(3600, Ev::FirstDmg, T, M1, 100);
    EXPECT_EQ(Oracle(b.tr, "O2").verdict, Verdict::Pass);

    Builder early;  // same, but inside the release delay
    early.Frames(6000, [](Frame& f) {
        B(f, T).inCombat = f.t >= 1000;
        U(f, M1).victim = T;
    });
    early.Ev(1500, Ev::FirstDmg, D1, M1, 300);
    EXPECT_EQ(Oracle(early.tr, "O2").verdict, Verdict::Fail);

    Builder peel;  // a mob on the healer, peeled off after the release delay
    peel.Frames(6000, [](Frame& f) {
        B(f, T).inCombat = f.t >= 1000;
        U(f, M2).inCombat = f.t >= 1000;
        U(f, M2).victim = H;
    });
    peel.Ev(3000, Ev::FirstDmg, D1, M2, 300);
    EXPECT_NE(Oracle(peel.tr, "O2").verdict, Verdict::Fail);
}

TEST(DcLabOracles, O7IgnoresThePacksOwnSummons)
{
    Builder b;
    b.tr.units.push_back({0xF130000000000201ull, 300, 0, "Skeletal Servant", "", false});
    b.Frames(4000, [](Frame& f) {
        f.dc.decisionSeq = 1;
        f.dc.predicted = 1;
        f.dc.pullTarget = M1;
        U(f, M1).inCombat = true;
        U(f, M1).victim = T;
        UnitSample s;
        s.guid = 0xF130000000000201ull;
        s.alive = s.inCombat = true;
        s.victim = T;
        f.units.push_back(s);
    });
    OracleResult const r = Oracle(b.tr, "O7");
    EXPECT_EQ(r.verdict, Verdict::Pass) << r.detail;
    EXPECT_NE(r.detail.find("1 engaged"), std::string::npos) << r.detail;
}

TEST(DcLabOracles, O4CasterMidCastIsNotIdle)
{
    Builder b;
    b.Frames(14000, [](Frame& f) {
        B(f, T).inCombat = true;
        U(f, M1).inCombat = true;
        U(f, M1).victim = T;
        for (std::uint64_t g : {D1, D2, D3})
        {
            B(f, g).inCombat = true;
            B(f, g).casting = true;  // long casts, no damage landed yet
        }
    });
    EXPECT_NE(Oracle(b.tr, "O4").verdict, Verdict::Fail);
}
