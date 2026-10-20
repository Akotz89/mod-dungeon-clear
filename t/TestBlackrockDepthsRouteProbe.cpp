/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Navmesh probe for the Blackrock Depths wing split (map 230): does an Upper
// City run, which starts at the portal and never clears the Detention Block,
// route clear of the Ring of Law?
//
// The Ring of Law is a sealed arena run by area trigger 1526: a player crossing
// the centre starts the gauntlet and slams the gates. A full-dungeon run went
// through it on purpose (it is a Detention Block encounter). An Upper City run
// must not stumble into it on the way to the Shadowforge Lock or beyond — if the
// navmesh ever routes a leg across the arena floor, the Upper City needs a
// wing-tagged DcNavPenaltyRegistry fence there, as Blackrock Spire has.
//
// The suite also PRINTS each leg's closest approach to every Detention Block
// boss, so a route that walks the party past one of them (a boss pull the run
// does not count) is visible.
//
// Not a committed regression: reads the FULL mmaps dir from env DC_PROBE_MMAPS
// and GTEST_SKIPs when unset, same contract as the other route probes.
//
//   DC_PROBE_MMAPS=/home/jared/azerothcore/env/dist/bin \
//     ./dungeon_clear_tests --gtest_filter='BlackrockDepthsRouteProbe.*'

#include "gtest/gtest.h"
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
    constexpr uint32_t BRD = 230;

    struct Pt { float x, y, z; char const* name; };

    // Area trigger 1526, the arena centre on the floor (BlackrockDepthsEvents).
    constexpr Pt ARENA = { 596.432f, -188.498f, -53.9f, "Ring of Law" };
    // The arena floor is ~40yd across; anything within this of the centre, on
    // the floor's level, is inside the sealed ring.
    constexpr float ARENA_FLOOR_R = 25.0f;
    constexpr float ARENA_FLOOR_DZ = 8.0f;

    // Portal, then the Upper City in its clear order (Shadowforge Lock first).
    std::vector<Pt> const UPPER_CITY = {
        { 456.93f,   34.09f, -68.09f, "portal" },
        { 615.61f,  -49.78f, -59.82f, "Shadowforge Lock" },
        { 652.4f,    21.4f,  -60.0f,  "Angerforge" },
        { 846.8f,    16.3f,  -53.6f,  "Argelmach" },
        { 878.1f,  -153.1f,  -49.8f,  "Hurley" },
        { 869.0f,  -225.0f,  -43.7f,  "Phalanx" },
        { 878.5f,  -167.7f,  -49.7f,  "Ribbly" },
        { 888.5f,  -177.9f,  -43.0f,  "Plugger" },
        { 1009.8f, -239.0f,  -61.3f,  "Flamelash" },
        { 1215.1f, -220.7f,  -85.6f,  "The Seven" },
        { 1380.7f, -659.3f,  -92.0f,  "Magmus" },
        { 1380.2f, -831.6f,  -87.6f,  "Thaurissan" },
    };

    std::vector<Pt> const DETENTION_BLOCK = {
        { 310.6f, -146.3f, -70.3f, "Gerstahn" },
        { 615.5f, -267.4f, -83.6f, "Roccor" },
        { 607.7f, -174.8f, -84.5f, "Grebmar" },
        { 530.2f, -243.9f, -43.0f, "Loregrain" },
        { 893.5f, -267.1f, -71.9f, "Incendius" },
        { 823.4f, -342.3f, -50.1f, "Stilgiss" },
        { 963.3f, -343.7f, -71.7f, "Darkvire" },
        { 702.4f,  184.5f, -72.0f, "Bael'Gar" },
    };

    std::shared_ptr<dtNavMesh> LoadOrSkip()
    {
        char const* dir = std::getenv("DC_PROBE_MMAPS");
        if (!dir || !*dir)
            return nullptr;
        return DcNavHarness::LoadMap(dir, BRD);
    }

    Pt SnapOrKeep(dtNavMesh const* mesh, Pt p)
    {
        G3D::Vector3 s;
        if (DcNavHarness::NearestPoint(mesh, p.x, p.y, p.z, 10.0f, 20.0f, s))
            return { s.x, s.y, s.z, p.name };
        return p;
    }

    float Dist3(G3D::Vector3 const& a, Pt const& b)
    {
        float const dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // The polyline sampled every 2yd: its corners alone can straddle the arena.
    std::vector<G3D::Vector3> Densify(std::vector<G3D::Vector3> const& pts)
    {
        std::vector<G3D::Vector3> out;
        for (std::size_t i = 0; i + 1 < pts.size(); ++i)
        {
            G3D::Vector3 const d = pts[i + 1] - pts[i];
            int const steps = std::max(1, static_cast<int>(d.length() / 2.0f));
            for (int k = 0; k < steps; ++k)
                out.push_back(pts[i] + d * (static_cast<float>(k) / steps));
        }
        if (!pts.empty())
            out.push_back(pts.back());
        return out;
    }

    bool OnArenaFloor(G3D::Vector3 const& p)
    {
        float const dx = p.x - ARENA.x, dy = p.y - ARENA.y;
        return std::sqrt(dx * dx + dy * dy) < ARENA_FLOOR_R && std::fabs(p.z - ARENA.z) < ARENA_FLOOR_DZ;
    }
}

TEST(BlackrockDepthsRouteProbe, UpperCityRouteStaysOutOfTheRingOfLaw)
{
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkip();
    if (!mesh)
        GTEST_SKIP() << "DC_PROBE_MMAPS unset or map 230 mmaps missing";

    std::printf("Blackrock Depths — Upper City legs (closest approach to the arena / Detention Block bosses)\n");
    for (std::size_t i = 0; i + 1 < UPPER_CITY.size(); ++i)
    {
        Pt const from = SnapOrKeep(mesh.get(), UPPER_CITY[i]);
        Pt const to = SnapOrKeep(mesh.get(), UPPER_CITY[i + 1]);
        DcNavHarness::RouteResult const r =
            DcNavHarness::Route(mesh.get(), BRD, from.x, from.y, from.z, to.x, to.y, to.z);
        std::printf("  %-16s -> %-16s reachable=%d complete=%d len2d=%.1f pts=%zu %s\n", from.name, to.name,
                    r.reachable, r.corridorComplete, r.routeLength2d, r.points.size(), r.failureReason.c_str());
        EXPECT_TRUE(r.reachable) << from.name << " -> " << to.name;

        std::vector<G3D::Vector3> const walk = Densify(r.points);
        float arenaMin = std::numeric_limits<float>::max();
        for (G3D::Vector3 const& p : walk)
        {
            arenaMin = std::min(arenaMin, Dist3(p, ARENA));
            EXPECT_FALSE(OnArenaFloor(p)) << from.name << " -> " << to.name << " crosses the Ring of Law at ("
                                          << p.x << ", " << p.y << ", " << p.z << ")";
        }
        std::printf("      arena %.1fyd", arenaMin);
        for (Pt const& boss : DETENTION_BLOCK)
        {
            float d = std::numeric_limits<float>::max();
            for (G3D::Vector3 const& p : walk)
                d = std::min(d, Dist3(p, boss));
            if (d < 60.0f)
                std::printf(" | %s %.1fyd", boss.name, d);
        }
        std::printf("\n");
    }
}

// The Upper City's event stand points (Data/Events/BlackrockDepthsEvents.cpp):
// each must sit ON the navmesh (snapped within a couple of yards, on the right
// floor) and be reachable from the one before it, in clear order. Doors are not
// in the navmesh, so this proves geometry only — the events own the doors.
TEST(BlackrockDepthsRouteProbe, UpperCityEventSpotsAreOnTheNavmesh)
{
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkip();
    if (!mesh)
        GTEST_SKIP() << "DC_PROBE_MMAPS unset or map 230 mmaps missing";

    namespace B = DcBlackrockDepths;
    std::vector<Pt> const spots = {
        { 846.8f, 16.3f, -53.6f, "Argelmach" },
        { B::KEG_X, B::KEG_Y, B::KEG_Z, "Hurley's kegs" },
        { 878.47f, -167.73f, -49.68f, "Ribbly" },
        { B::ROCKNOT_HOLD_X, B::ROCKNOT_HOLD_Y, B::ROCKNOT_HOLD_Z, "Rocknot hold" },
        { 868.97f, -224.98f, -43.72f, "Phalanx" },
        { B::PLUGGER_CAMP_X, B::PLUGGER_CAMP_Y, B::PLUGGER_CAMP_Z, "Plugger camp" },
        { 1009.75f, -239.02f, -61.3f, "Flamelash" },
        { B::TOMB_HOLD_X, B::TOMB_HOLD_Y, B::TOMB_HOLD_Z, "Tomb garrison" },
        { 1281.14f, -282.15f, -78.14f, "Doom'rel" },
        { 1311.48f, -343.07f, -92.01f, "Lyceum door" },
        { B::LYCEUM_ENTRY_X, B::LYCEUM_ENTRY_Y, B::LYCEUM_ENTRY_Z, "Lyceum entry" },
        { B::BRAZIER_S_X, B::BRAZIER_S_Y, B::BRAZIER_S_Z, "S brazier stand" },
        { B::FLAME_KEEPER_POINTS[0].x, B::FLAME_KEEPER_POINTS[0].y, B::FLAME_KEEPER_POINTS[0].z, "keeper pt 0" },
        { B::FLAME_KEEPER_POINTS[1].x, B::FLAME_KEEPER_POINTS[1].y, B::FLAME_KEEPER_POINTS[1].z, "keeper pt 1" },
        { B::FLAME_KEEPER_POINTS[2].x, B::FLAME_KEEPER_POINTS[2].y, B::FLAME_KEEPER_POINTS[2].z, "keeper pt 2" },
        { B::FLAME_KEEPER_POINTS[3].x, B::FLAME_KEEPER_POINTS[3].y, B::FLAME_KEEPER_POINTS[3].z, "keeper pt 3" },
        { B::BRAZIER_N_X, B::BRAZIER_N_Y, B::BRAZIER_N_Z, "N brazier stand" },
        { 1389.55f, -555.85f, -92.01f, "Golem door N" },
        { B::MAGMUS_X, B::MAGMUS_Y, B::MAGMUS_Z, "Magmus" },
        { 1380.12f, -710.48f, -92.01f, "Throne door" },
        { 1380.18f, -831.65f, -87.59f, "Thaurissan" },
    };

    std::printf("Blackrock Depths — Upper City event spots\n");
    for (std::size_t i = 0; i < spots.size(); ++i)
    {
        Pt const& want = spots[i];
        G3D::Vector3 s;
        bool const snapped = DcNavHarness::NearestPoint(mesh.get(), want.x, want.y, want.z, 4.0f, 6.0f, s);
        float const off2d = snapped ? std::hypot(s.x - want.x, s.y - want.y) : -1.0f;
        std::printf("  %-18s (%.1f, %.1f, %.2f) -> snapped=%d (%.2f, %.2f, %.2f) off2d=%.2f dz=%.2f\n",
                    want.name, want.x, want.y, want.z, snapped, s.x, s.y, s.z, off2d,
                    snapped ? s.z - want.z : 0.0f);
        ASSERT_TRUE(snapped) << want.name << " is off the navmesh";
        EXPECT_LT(off2d, 2.0f) << want.name;
        EXPECT_LT(std::fabs(s.z - want.z), 2.5f) << want.name << " snapped to another floor";

        if (i == 0)
            continue;
        Pt const from = SnapOrKeep(mesh.get(), spots[i - 1]);
        DcNavHarness::RouteResult const r =
            DcNavHarness::Route(mesh.get(), BRD, from.x, from.y, from.z, s.x, s.y, s.z);
        std::printf("      from %-16s reachable=%d complete=%d len2d=%.1f %s\n", from.name, r.reachable,
                    r.corridorComplete, r.routeLength2d, r.failureReason.c_str());
        EXPECT_TRUE(r.reachable) << from.name << " -> " << want.name;
    }
}

// The Iron Hall's safe-spot stops (DcIronHall): every slot on the hall floor,
// and each stop walkable from the one before — the hop only ever walks between
// neighbours.
TEST(BlackrockDepthsRouteProbe, IronHallStopsAreOnTheFloorAndChained)
{
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkip();
    if (!mesh)
        GTEST_SKIP() << "DC_PROBE_MMAPS unset or map 230 mmaps missing";

    namespace B = DcBlackrockDepths;
    for (uint32_t z = 0; z < B::IRON_HALL_ZONE_COUNT; ++z)
        for (uint32_t s = 0; s < B::IRON_HALL_SLOT_COUNT; ++s)
        {
            float const x = B::IRON_HALL_ZONES[z].x + B::IRON_HALL_SLOT_OFFSETS[s].x;
            float const y = B::IRON_HALL_ZONES[z].y + B::IRON_HALL_SLOT_OFFSETS[s].y;
            G3D::Vector3 p;
            bool const snapped = DcNavHarness::NearestPoint(mesh.get(), x, y, B::IRON_HALL_FLOOR_Z, 4.0f, 6.0f, p);
            std::printf("  Iron Hall stop %u slot %u (%.1f, %.1f) -> snapped=%d off2d=%.2f z=%.2f\n", z, s, x, y,
                        snapped, snapped ? std::hypot(p.x - x, p.y - y) : -1.0f, p.z);
            ASSERT_TRUE(snapped) << "stop " << z << " slot " << s;
            EXPECT_LT(std::hypot(p.x - x, p.y - y), 0.5f) << "stop " << z << " slot " << s;
            EXPECT_LT(std::fabs(p.z - B::IRON_HALL_FLOOR_Z), 1.5f) << "stop " << z << " slot " << s;
        }
    for (uint32_t z = 1; z < B::IRON_HALL_ZONE_COUNT; ++z)
    {
        B::HallPoint const& a = B::IRON_HALL_ZONES[z - 1];
        B::HallPoint const& b = B::IRON_HALL_ZONES[z];
        DcNavHarness::RouteResult const r = DcNavHarness::Route(mesh.get(), BRD, a.x, a.y, B::IRON_HALL_FLOOR_Z,
                                                                b.x, b.y, B::IRON_HALL_FLOOR_Z);
        std::printf("  stop %u -> %u reachable=%d len2d=%.1f\n", z - 1, z, r.reachable, r.routeLength2d);
        EXPECT_TRUE(r.reachable) << z - 1 << " -> " << z;
        EXPECT_LT(r.routeLength2d, 1.2f * std::hypot(b.x - a.x, b.y - a.y)) << "a straight walk down the hall";
    }
}

// Rocknot's hold must be OFF his escort (waypoint path 95030, from his spawn) —
// a body-blocked npc_escortAI stalls the whole ale run — and off Phalanx, who
// takes the Bar Door at the end of his own run (path 95020) and is fought there.
TEST(DcBlackrockDepthsGeometry, RocknotHoldIsClearOfHisWalk)
{
    namespace B = DcBlackrockDepths;
    struct P2 { float x, y; };
    std::vector<P2> const walk = {
        { 891.198f, -197.924f }, { 884.95f, -193.154f }, { 879.862f, -189.961f },
        { 871.673f, -185.768f }, { 867.274f, -190.419f }, { 860.899f, -191.715f },
        { 858.047f, -193.255f }, { 855.187f, -194.8f }, { 851.379f, -196.694f },
    };
    float best = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i + 1 < walk.size(); ++i)
    {
        // Distance from the hold to each leg of the walk.
        float const ax = walk[i].x, ay = walk[i].y, bx = walk[i + 1].x, by = walk[i + 1].y;
        float const dx = bx - ax, dy = by - ay;
        float t = ((B::ROCKNOT_HOLD_X - ax) * dx + (B::ROCKNOT_HOLD_Y - ay) * dy) / (dx * dx + dy * dy);
        t = std::clamp(t, 0.0f, 1.0f);
        best = std::min(best, std::hypot(B::ROCKNOT_HOLD_X - (ax + t * dx), B::ROCKNOT_HOLD_Y - (ay + t * dy)));
    }
    // The hold radius plus a follower's trail behind the tank.
    EXPECT_GT(best, B::ROCKNOT_HOLD_RADIUS + 6.0f) << "hold is " << best << "yd from Rocknot's walk";
    EXPECT_GT(std::hypot(B::ROCKNOT_HOLD_X - B::PHALANX_DOOR_X, B::ROCKNOT_HOLD_Y - B::PHALANX_DOOR_Y), 12.0f)
        << "hold crowds Phalanx in the doorway";
}

// Rocknot's ale is bought from Plugger, who patrols BEHIND his counter
// (waypoint path 477030, delay 0 throughout), so the buy hook chases him and
// trades the moment it is within interaction range. That only works if the
// customer side of the counter has floor within reach of his walk: list the
// reachable stand points within 4yd of it (from Rocknot's hold, on the bar's
// upper floor).
TEST(BlackrockDepthsRouteProbe, PluggerIsReachableAcrossHisCounter)
{
    std::shared_ptr<dtNavMesh> mesh = LoadOrSkip();
    if (!mesh)
        GTEST_SKIP() << "DC_PROBE_MMAPS unset or map 230 mmaps missing";

    namespace B = DcBlackrockDepths;
    struct P2 { float x, y; };
    std::vector<P2> const patrol = {
        { 888.236f, -177.809f }, { 886.791f, -175.185f }, { 886.586f, -174.894f },
        { 878.133f, -179.536f }, { 886.578f, -174.768f }, { 888.454f, -177.985f },
    };
    constexpr float PLUGGER_Z = -42.96f;
    auto distToPatrol = [&](float x, float y)
    {
        float best = std::numeric_limits<float>::max();
        for (std::size_t i = 0; i + 1 < patrol.size(); ++i)
        {
            float const ax = patrol[i].x, ay = patrol[i].y;
            float const dx = patrol[i + 1].x - ax, dy = patrol[i + 1].y - ay;
            float const len2 = dx * dx + dy * dy;
            float t = len2 > 0.0f ? ((x - ax) * dx + (y - ay) * dy) / len2 : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            best = std::min(best, std::hypot(x - (ax + t * dx), y - (ay + t * dy)));
        }
        return best;
    };

    Pt const hold = SnapOrKeep(mesh.get(), { B::ROCKNOT_HOLD_X, B::ROCKNOT_HOLD_Y, B::ROCKNOT_HOLD_Z, "hold" });
    int found = 0;
    float bestDist = std::numeric_limits<float>::max();
    std::printf("Plugger's counter — reachable stand points within 4yd of his patrol\n");
    for (float x = 872.0f; x <= 896.0f; x += 1.0f)
        for (float y = -190.0f; y <= -168.0f; y += 1.0f)
        {
            float const d = distToPatrol(x, y);
            if (d > 4.0f)
                continue;
            G3D::Vector3 s;
            if (!DcNavHarness::NearestPoint(mesh.get(), x, y, PLUGGER_Z, 0.4f, 2.0f, s))
                continue;
            if (std::hypot(s.x - x, s.y - y) > 0.5f || std::fabs(s.z - PLUGGER_Z) > 1.5f)
                continue;
            DcNavHarness::RouteResult const r =
                DcNavHarness::Route(mesh.get(), BRD, hold.x, hold.y, hold.z, s.x, s.y, s.z);
            if (!r.reachable || !r.corridorComplete)
                continue;
            ++found;
            bestDist = std::min(bestDist, d);
            std::printf("  (%.1f, %.1f, %.2f) %.1fyd from his walk, route %.1fyd\n", s.x, s.y, s.z, d,
                        r.routeLength2d);
        }
    EXPECT_GT(found, 0) << "no reachable floor within 4yd of Plugger's walk";
    EXPECT_LT(bestDist, 3.0f);

    // The stand point the buy hook waits on must be one of those.
    G3D::Vector3 s;
    ASSERT_TRUE(DcNavHarness::NearestPoint(mesh.get(), B::PLUGGER_STAND_X, B::PLUGGER_STAND_Y,
                                           B::PLUGGER_STAND_Z, 0.4f, 2.0f, s));
    EXPECT_LT(std::hypot(s.x - B::PLUGGER_STAND_X, s.y - B::PLUGGER_STAND_Y), 0.5f);
    EXPECT_LT(std::fabs(s.z - B::PLUGGER_STAND_Z), 1.0f);
    EXPECT_LT(distToPatrol(B::PLUGGER_STAND_X, B::PLUGGER_STAND_Y), 2.0f);
    DcNavHarness::RouteResult const r =
        DcNavHarness::Route(mesh.get(), BRD, hold.x, hold.y, hold.z, s.x, s.y, s.z);
    EXPECT_TRUE(r.reachable && r.corridorComplete) << "Rocknot hold -> Plugger stand";
}
