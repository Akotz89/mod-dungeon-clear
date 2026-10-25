/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Route probe for The Black Morass (map 269) portal-grounds sweep — the
// "Clear the portal grounds" stall from soak sk-20261003-174711: 6 of 15 runs
// froze the tank at (-1988, 7154, 18.7) on the P4 -> P2 hop and never moved
// again.
//
// The sweep's MoveTo steps travel by core MovePoint, not DC's long-range
// router, so this replays CORE's query for a headless bot
// (PathGenerator::CreateFilter: GROUND|WATER minus MAGMA|SLIME|GROUND_STEEP,
// water cost 20; MMapMgr's 1024-node query pool; MAX_PATH_LENGTH 148) plus a
// port of PathGenerator::FindSmoothPath, and drives it the way the event does
// live: StopBot(Hold) halts the tank every tick and HopTo re-plans from where
// it stands.
//
// Not a committed regression (needs client-derived mmaps): GTEST_SKIPs unless
// DC_PROBE_MMAPS names a dir containing mmaps/ for map 269.
//
//   DC_PROBE_MMAPS=/home/jared/azerothcore/env/dist/bin \
//     ./dungeon_clear_tests --gtest_filter='BlackMorassRouteProbe.*'

#include "gtest/gtest.h"
#include "NavHarness.h"

#include "DetourCommon.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "DetourStatus.h"
#include "MapDefines.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace
{
    constexpr uint32 MAP_BM = 269;

    // Mirrors BlackMorassEvents.cpp (anonymous there).
    constexpr float P1[3]     = { -2030.83f, 7024.94f, 23.07f };
    constexpr float P2[3]     = { -1961.73f, 7029.53f, 21.81f };
    constexpr float P3[3]     = { -1887.70f, 7106.56f, 22.05f };
    constexpr float P4[3]     = { -1930.91f, 7183.60f, 23.01f };
    constexpr float FORD_N[3] = { -1951.77f, 7114.77f, 19.27f };
    constexpr float FORD_S[3] = { -1954.31f, 7082.87f, 19.68f };
    constexpr float HOLD[3]   = { -2014.4f, 7114.6f, 22.8f };
}

// Port of PathGenerator::FindSmoothPath / GetSteerTarget / FixupCorridor
// (PathGenerator.cpp), slope check off as for MovePoint — the point path the
// tank is actually handed.
namespace
{
    bool InRangeYZX(float const* v1, float const* v2, float r, float h)
    {
        float const dx = v2[0] - v1[0], dy = v2[1] - v1[1], dz = v2[2] - v1[2];
        return (dx * dx + dz * dz) < r * r && std::fabs(dy) < h;
    }

    uint32 Fixup(dtPolyRef* path, uint32 npath, uint32 maxPath, dtPolyRef const* visited, uint32 nvisited)
    {
        int32 fp = -1, fv = -1;
        for (int32 i = npath - 1; i >= 0; --i)
        {
            bool found = false;
            for (int32 j = nvisited - 1; j >= 0; --j)
                if (path[i] == visited[j]) { fp = i; fv = j; found = true; }
            if (found) break;
        }
        if (fp == -1 || fv == -1)
            return npath;
        uint32 req = nvisited - fv;
        uint32 orig = uint32(fp + 1) < npath ? fp + 1 : npath;
        uint32 size = npath > orig ? npath - orig : 0;
        if (req + size > maxPath) size = maxPath - req;
        if (size) memmove(path + req, path + orig, size * sizeof(dtPolyRef));
        for (uint32 i = 0; i < req; ++i) path[i] = visited[(nvisited - 1) - i];
        return req + size;
    }

    bool Steer(dtNavMeshQuery& q, float const* sp, float const* ep, dtPolyRef const* path, uint32 n,
               float* steer, unsigned char& flag)
    {
        float sPath[9]; unsigned char sFlags[3]; dtPolyRef sPolys[3]; int ns = 0;
        if (dtStatusFailed(q.findStraightPath(sp, ep, path, n, sPath, sFlags, sPolys, &ns, 3)) || !ns)
            return false;
        int i = 0;
        while (i < ns)
        {
            if ((sFlags[i] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) || !InRangeYZX(&sPath[i * 3], sp, 0.3f, 1000.0f))
                break;
            ++i;
        }
        if (i >= ns) return false;
        dtVcopy(steer, &sPath[i * 3]);
        steer[1] = sp[1];
        flag = sFlags[i];
        return true;
    }

    // Returns smooth point count; *ok = core's DT_SUCCESS verdict.
    int Smooth(dtNavMeshQuery& q, dtQueryFilter const& f, float const* sp, float const* ep,
               dtPolyRef const* in, uint32 n, float* out, bool* ok)
    {
        dtPolyRef polys[148];
        memcpy(polys, in, sizeof(dtPolyRef) * n);
        uint32 np = n;
        float iter[3], tgt[3];
        if (n > 1)
        {
            q.closestPointOnPolyBoundary(polys[0], sp, iter);
            q.closestPointOnPolyBoundary(polys[np - 1], ep, tgt);
        }
        else { dtVcopy(iter, sp); dtVcopy(tgt, ep); }
        int ns = 0;
        dtVcopy(&out[ns++ * 3], iter);
        while (np && ns < 148)
        {
            float steer[3]; unsigned char flag = 0;
            if (!Steer(q, iter, tgt, polys, np, steer, flag)) break;
            bool const end = (flag & DT_STRAIGHTPATH_END) != 0;
            float d[3]; dtVsub(d, steer, iter);
            float len = std::sqrt(dtVdot(d, d));
            len = (end && len < 4.0f) ? 1.0f : 4.0f / len;
            float mt[3]; dtVmad(mt, iter, d, len);
            float res[3]; dtPolyRef vis[16]; int nv = 0;
            if (dtStatusFailed(q.moveAlongSurface(polys[0], iter, mt, &f, res, vis, &nv, 16)))
            { *ok = false; return ns; }
            np = Fixup(polys, np, 148, vis, nv);
            q.getPolyHeight(polys[0], res, &res[1]);
            res[1] += 0.5f;
            dtVcopy(iter, res);
            if (end && InRangeYZX(iter, steer, 0.3f, 1.0f))
            {
                dtVcopy(iter, tgt);
                if (ns < 148) dtVcopy(&out[ns++ * 3], iter);
                break;
            }
            if (ns < 148) dtVcopy(&out[ns++ * 3], iter);
        }
        *ok = ns < 148;
        return ns;
    }
}

namespace
{
    // Simulates the live MoveTo loop: every tick DcObjectiveArrive's
    // StopBot(Hold) halts the tank and HopTo re-issues a fresh core MovePoint
    // from where it stands, so it only ever travels the first smoothed step of
    // each plan. Returns ticks to arrive within `radius`, or -1 on a livelock.
    int SimulateHop(dtNavMesh const* mesh, float const* a, float const* b, float radius, float* endPos)
    {
        dtNavMeshQuery q;
        q.init(mesh, 1024);
        dtQueryFilter f;
        f.setIncludeFlags(NAV_GROUND | NAV_WATER);
        f.setExcludeFlags(NAV_MAGMA | NAV_SLIME | NAV_GROUND_STEEP);
        f.setAreaCost(NAV_WATER, 20.0f);
        float const ext[3] = { 3.0f, 5.0f, 3.0f };
        float const ep[3] = { b[1], b[2], b[0] };
        float pos[3] = { a[1], a[2], a[0] };
        for (int tick = 0; tick < 400; ++tick)
        {
            float const dx = pos[2] - b[0], dy = pos[0] - b[1], dz = pos[1] - b[2];
            endPos[0] = pos[2]; endPos[1] = pos[0]; endPos[2] = pos[1];
            if (std::sqrt(dx * dx + dy * dy + dz * dz) <= radius)
                return tick;
            dtPolyRef sRef = 0, eRef = 0; float sN[3], eN[3];
            q.findNearestPoly(pos, ext, &f, &sRef, sN);
            q.findNearestPoly(ep, ext, &f, &eRef, eN);
            dtPolyRef path[148]; int n = 0;
            q.findPath(sRef, eRef, sN, eN, &f, path, &n, 148);
            float pts[148 * 3]; bool ok = false;
            int const np = n ? Smooth(q, f, sN, eN, path, n, pts, &ok) : 0;
            if (!ok || np < 2)
                return -1;
            dtVcopy(pos, &pts[3]);
            pos[1] -= 0.5f;
        }
        return -1;
    }

    void Hop(dtNavMesh const* mesh, char const* label, float const* a, float const* b, float radius, bool expectArrive)
    {
        float end[3];
        int const ticks = SimulateHop(mesh, a, b, radius, end);
        std::printf("  [%s] %s after %d ticks, ended (%.2f, %.2f, %.2f)\n", label,
                    ticks >= 0 ? "ARRIVED" : "LIVELOCK", ticks >= 0 ? ticks : 400, end[0], end[1], end[2]);
        EXPECT_EQ(ticks >= 0, expectArrive) << label;
    }
}

// The soak failure, reproduced: the old single P4 -> P2 hop livelocks next to
// the observed freeze point, and every hop of the authored sweep — plus the
// walk on to the Defend Medivh hold point — arrives.
TEST(BlackMorassRouteProbe, PortalSweepHopsArriveUnderCoreMovePoint)
{
    char const* dir = std::getenv("DC_PROBE_MMAPS");
    if (!dir || !*dir)
        GTEST_SKIP() << "set DC_PROBE_MMAPS to a dir containing mmaps/ for map 269";
    std::shared_ptr<dtNavMesh> const mesh = DcNavHarness::LoadMap(dir, MAP_BM);
    ASSERT_TRUE(mesh) << "no mmaps for map 269 under " << dir;

    Hop(mesh.get(), "old P4 -> P2", P4, P2, 8.0f, false);
    Hop(mesh.get(), "P3 -> P4", P3, P4, 8.0f, true);
    Hop(mesh.get(), "P4 -> ford N", P4, FORD_N, 5.0f, true);
    Hop(mesh.get(), "ford N -> ford S", FORD_N, FORD_S, 5.0f, true);
    Hop(mesh.get(), "ford S -> P2", FORD_S, P2, 8.0f, true);
    Hop(mesh.get(), "P2 -> P1", P2, P1, 8.0f, true);
    Hop(mesh.get(), "P1 -> hold", P1, HOLD, 8.0f, true);
}
