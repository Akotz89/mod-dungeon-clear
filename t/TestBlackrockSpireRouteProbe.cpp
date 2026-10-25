/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Navmesh probe for Upper Blackrock Spire (map 229): does the router take the
// Rookery's north lane, clear of the eggs, and are the UBRS event spots on the
// navmesh and chained?
//
// The Rookery floor carries 149 Rookery Eggs, 1.5yd proximity traps that hatch a
// Whelp on any player, and every way from Emberseer's room to the stadium crosses
// it. The tank walks an authored anchor route along the measured north lane
// (DcBlackrockSpire::ROOKERY_ROUTE); this checks that route on the real mesh,
// and that the raw corridor — which followers' recovery paths and the fallback
// resolver still use — stays egg-clear too.
//
// Not a committed regression: reads the FULL mmaps dir from env DC_PROBE_MMAPS
// and GTEST_SKIPs when unset, same contract as the other route probes.
//
//   DC_PROBE_MMAPS=/home/jared/azerothcore/env/dist/bin \
//     ./dungeon_clear_tests --gtest_filter='BlackrockSpireRouteProbe.*'

#include "gtest/gtest.h"
#include "BlackrockSpireRookeryEggs.h"
#include "NavHarness.h"

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

#include "DetourNavMesh.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <vector>

namespace
{
    namespace B = DcBlackrockSpire;

    struct Pt { float x, y, z; char const* name; };

    // A route point this close (2D, on the floor's level) to an egg would trip
    // its 1.5yd trigger for a follower trailing a little off the line.
    constexpr float ROUTE_EGG_CLEARANCE = 2.5f;
    constexpr float EGG_DZ = 4.0f;

    std::shared_ptr<dtNavMesh> LoadOrSkip()
    {
        char const* dir = std::getenv("DC_PROBE_MMAPS");
        if (!dir || !*dir)
            return nullptr;
        return DcNavHarness::LoadMap(dir, B::MAP_ID);
    }

    Pt SnapOrKeep(dtNavMesh const* mesh, Pt p)
    {
        G3D::Vector3 s;
        if (DcNavHarness::NearestPoint(mesh, p.x, p.y, p.z, 10.0f, 20.0f, s))
            return { s.x, s.y, s.z, p.name };
        return p;
    }

    std::vector<G3D::Vector3> Densify(std::vector<G3D::Vector3> const& pts)
    {
        std::vector<G3D::Vector3> out;
        for (std::size_t i = 0; i + 1 < pts.size(); ++i)
        {
            G3D::Vector3 const d = pts[i + 1] - pts[i];
            int const steps = std::max(1, static_cast<int>(d.length() / 1.0f));
            for (int k = 0; k < steps; ++k)
                out.push_back(pts[i] + d * (static_cast<float>(k) / steps));
        }
        if (!pts.empty())
            out.push_back(pts.back());
        return out;
    }

    // Closest 2D approach of `walk` to any north-floor egg on its level.
    float NearestEgg(std::vector<G3D::Vector3> const& walk, G3D::Vector3& at)
    {
        float best = std::numeric_limits<float>::max();
        for (G3D::Vector3 const& p : walk)
            for (DcRookeryEggs::Egg const& e : DcRookeryEggs::NORTH_FLOOR)
            {
                if (std::fabs(p.z - e.z) > EGG_DZ)
                    continue;
                float const d = std::hypot(p.x - e.x, p.y - e.y);
                if (d < best)
                {
                    best = d;
                    at = p;
                }
            }
        return best;
    }

    DcNavHarness::RouteResult RouteLeg(dtNavMesh const* mesh, Pt const& a, Pt const& b)
    {
        Pt const from = SnapOrKeep(mesh, a);
        Pt const to = SnapOrKeep(mesh, b);
        return DcNavHarness::Route(mesh, B::MAP_ID, from.x, from.y, from.z, to.x, to.y, to.z);
    }
}

// The raw A* corridor Emberseer Out -> upper floor, with the map's penalty rows in
// force. Untaxed it is 113yd and 3.29yd off every egg (2026-10-19). With the five
// egg clusters flanking the lane boxed, as first planned, it went round the
// SOUTH half instead — 193yd, 0.22yd from an egg — so nothing may tax this floor.
TEST(BlackrockSpireRouteProbe, RawCorridorAcrossTheRookeryKeepsOffTheEggs)
{
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkip();
    if (!mesh)
        GTEST_SKIP() << "DC_PROBE_MMAPS unset or map 229 mmaps missing";

    B::LanePoint const& out = B::ROOKERY_LANE[0];
    B::LanePoint const& up = B::ROOKERY_LANE[sizeof(B::ROOKERY_LANE) / sizeof(B::ROOKERY_LANE[0]) - 1];
    DcNavHarness::RouteResult const r =
        RouteLeg(mesh.get(), { out.x, out.y, out.z, "Emberseer Out" }, { up.x, up.y, up.z, "upper floor" });
    std::printf("Upper Blackrock Spire — Rookery crossing: reachable=%d complete=%d len2d=%.1f pts=%zu %s\n",
                r.reachable, r.corridorComplete, r.routeLength2d, r.points.size(), r.failureReason.c_str());
    for (G3D::Vector3 const& p : r.points)
        std::printf("    (%.1f, %.1f, %.1f)\n", p.x, p.y, p.z);
    ASSERT_TRUE(r.reachable);

    G3D::Vector3 at;
    float const nearest = NearestEgg(Densify(r.points), at);
    std::printf("  nearest north-floor egg %.2fyd, at (%.1f, %.1f, %.1f)\n", nearest, at.x, at.y, at.z);
    EXPECT_GE(nearest, ROUTE_EGG_CLEARANCE) << "the crossing clips an egg at (" << at.x << ", " << at.y << ")";
    EXPECT_LT(r.routeLength2d, 150.0f) << "the corridor left the north side of the floor";
}

// The authored row (DcBlackrockSpire::ROOKERY_ROUTE): every anchor on the mesh at
// its z, every straight leg on the mesh sample by sample (anchors are walked as
// straight legs), and the Rookery legs egg-clear.
TEST(BlackrockSpireRouteProbe, RookeryAnchorRouteIsWalkableAndEggClear)
{
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkip();
    if (!mesh)
        GTEST_SKIP() << "DC_PROBE_MMAPS unset or map 229 mmaps missing";

    constexpr std::size_t kN = sizeof(B::ROOKERY_ROUTE) / sizeof(B::ROOKERY_ROUTE[0]);
    uint32 offMesh = 0;
    for (std::size_t i = 0; i < kN; ++i)
    {
        B::LanePoint const& a = B::ROOKERY_ROUTE[i];
        G3D::Vector3 s;
        ASSERT_TRUE(DcNavHarness::NearestPoint(mesh.get(), a.x, a.y, a.z, 1.0f, 2.5f, s))
            << "anchor " << i << " is off the navmesh";
        EXPECT_LT(std::fabs(s.z - a.z), 2.0f) << "anchor " << i << " snapped to another floor";
        if (i + 1 == kN)
            continue;
        B::LanePoint const& b = B::ROOKERY_ROUTE[i + 1];
        float const len = std::hypot(b.x - a.x, b.y - a.y);
        int const steps = std::max(1, static_cast<int>(len / 1.0f));
        for (int k = 1; k < steps; ++k)
        {
            float const t = static_cast<float>(k) / steps;
            float const x = a.x + (b.x - a.x) * t, y = a.y + (b.y - a.y) * t, z = a.z + (b.z - a.z) * t;
            G3D::Vector3 q;
            if (!DcNavHarness::NearestPoint(mesh.get(), x, y, z, 0.5f, 3.0f, q))
            {
                ++offMesh;
                std::printf("  leg %zu -> %zu off the mesh at (%.1f, %.1f, %.1f)\n", i, i + 1, x, y, z);
            }
        }
    }
    EXPECT_EQ(offMesh, 0u);

    std::vector<G3D::Vector3> lane;
    for (uint32 i = B::ROOKERY_ROUTE_LANE_FIRST; i <= B::ROOKERY_ROUTE_LANE_LAST; ++i)
        lane.emplace_back(B::ROOKERY_ROUTE[i].x, B::ROOKERY_ROUTE[i].y, B::ROOKERY_ROUTE[i].z);
    G3D::Vector3 at;
    float const nearest = NearestEgg(Densify(lane), at);
    std::printf("  anchor route: %zu anchors, nearest north-floor egg %.2fyd\n", kN, nearest);
    EXPECT_GE(nearest, 3.5f) << "at (" << at.x << ", " << at.y << ")";
}

// The hand lane itself, point to point: every point on the navmesh at its z, and
// each segment walkable and egg-clear.
TEST(BlackrockSpireRouteProbe, RookeryLaneIsOnTheNavmeshAndEggClear)
{
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkip();
    if (!mesh)
        GTEST_SKIP() << "DC_PROBE_MMAPS unset or map 229 mmaps missing";

    std::vector<G3D::Vector3> lane;
    for (B::LanePoint const& p : B::ROOKERY_LANE)
    {
        G3D::Vector3 s;
        bool const snapped = DcNavHarness::NearestPoint(mesh.get(), p.x, p.y, p.z, 2.0f, 3.0f, s);
        std::printf("  lane (%.1f, %.1f, %.1f) snapped=%d dz=%.2f\n", p.x, p.y, p.z, snapped,
                    snapped ? s.z - p.z : 0.0f);
        ASSERT_TRUE(snapped) << "lane point (" << p.x << ", " << p.y << ") is off the navmesh";
        EXPECT_LT(std::fabs(s.z - p.z), 2.0f);
        lane.emplace_back(p.x, p.y, p.z);
    }
    G3D::Vector3 at;
    EXPECT_GE(NearestEgg(Densify(lane), at), 3.5f);
}

// The rest of a UBRS run, leg by leg, from Emberseer's room on: each leg
// reachable, each spot on its floor. Doors are not in the navmesh — this proves
// geometry only; the events own the doors.
TEST(BlackrockSpireRouteProbe, UbrsSpotsAreOnTheNavmeshAndChained)
{
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkip();
    if (!mesh)
        GTEST_SKIP() << "DC_PROBE_MMAPS unset or map 229 mmaps missing";

    std::vector<Pt> const spots = {
        { B::DOOR_STAND_X, B::DOOR_STAND_Y, B::DOOR_STAND_Z, "door stand" },
        { B::HALL_ANCHOR_X, B::HALL_ANCHOR_Y, B::HALL_ANCHOR_Z, "hall anchor" },
        { B::ALTAR_STAND_X, B::ALTAR_STAND_Y, B::ALTAR_STAND_Z, "altar stand" },
        { 113.9f, -258.9f, 91.8f, "Emberseer Out" },
        { 108.1f, -266.0f, 106.7f, "upper floor" },
        { 118.0f, -420.0f, 111.0f, "stadium muster" },
        { 150.0f, -420.0f, 110.9f, "arena stand" },
        { 93.0f, -435.6f, 111.0f, "TOBOSSROOMS" },
        { 60.0f, -540.0f, 111.0f, "Beast camp" },
        { 34.0f, -334.0f, 111.0f, "Drakkisath archway" },
    };

    std::printf("Upper Blackrock Spire — event spots\n");
    for (std::size_t i = 0; i < spots.size(); ++i)
    {
        Pt const& want = spots[i];
        G3D::Vector3 s;
        bool const snapped = DcNavHarness::NearestPoint(mesh.get(), want.x, want.y, want.z, 4.0f, 6.0f, s);
        float const off2d = snapped ? std::hypot(s.x - want.x, s.y - want.y) : -1.0f;
        std::printf("  %-18s (%.1f, %.1f, %.2f) -> snapped=%d (%.2f, %.2f, %.2f) off2d=%.2f\n", want.name,
                    want.x, want.y, want.z, snapped, s.x, s.y, s.z, off2d);
        ASSERT_TRUE(snapped) << want.name << " is off the navmesh";
        EXPECT_LT(off2d, 2.0f) << want.name;
        EXPECT_LT(std::fabs(s.z - want.z), 2.5f) << want.name << " snapped to another floor";
        if (i == 0)
            continue;
        DcNavHarness::RouteResult const r = RouteLeg(mesh.get(), spots[i - 1], want);
        std::printf("      from %-18s reachable=%d complete=%d len2d=%.1f %s\n", spots[i - 1].name, r.reachable,
                    r.corridorComplete, r.routeLength2d, r.failureReason.c_str());
        EXPECT_TRUE(r.reachable) << spots[i - 1].name << " -> " << want.name;
    }
}

// Authoring aid: the corridor from Emberseer's room to Emberseer Out, and from the
// upper floor to the stadium muster point, printed vertex by vertex.
TEST(BlackrockSpireRouteProbe, PrintCrossingCorridors)
{
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkip();
    if (!mesh)
        GTEST_SKIP() << "DC_PROBE_MMAPS unset or map 229 mmaps missing";
    std::vector<std::pair<Pt, Pt>> const legs = {
        { { 144.4f, -262.0f, 91.6f, "Emberseer's room" }, { 113.9f, -258.9f, 91.8f, "Emberseer Out" } },
        { { 108.1f, -266.0f, 106.7f, "upper floor" }, { 118.0f, -420.0f, 111.0f, "stadium muster" } },
    };
    for (auto const& [a, b] : legs)
    {
        DcNavHarness::RouteResult const r = RouteLeg(mesh.get(), a, b);
        std::printf("CORRIDOR %s -> %s len2d=%.1f\n", a.name, b.name, r.routeLength2d);
        for (G3D::Vector3 const& p : r.points)
            std::printf("    { %.2ff, %.2ff, %.2ff },\n", p.x, p.y, p.z);
    }
}
