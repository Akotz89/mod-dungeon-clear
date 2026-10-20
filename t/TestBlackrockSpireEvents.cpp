/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "BlackrockSpireRookeryEggs.h"

#include "Ai/Dungeon/DungeonClear/Data/BossPullbackRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcEventDoorRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcHazardRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcNavPenaltyRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcNeverTargetRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonWingRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/RoomAggroRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRunWing.h"
#include "Ai/Dungeon/DungeonClear/Util/DcUbrsDecision.h"
#include "TestRun/DcTestDungeonRegistry.h"

// Upper Blackrock Spire (map 229): the event shapes, the roster order the wing
// filter leaves a UBRS run, the event-owned doors, and the pure kernels the hooks
// decide with (BlackrockSpireEvents.cpp, Util/DcUbrsDecision.h). All pure — no
// world, no bots.

namespace
{
    namespace B = DcBlackrockSpire;

    DungeonBossInfo Boss(uint32 entry, uint32 bit, float x, float y, float z)
    {
        DungeonBossInfo b;
        b.entry = entry;
        b.encounterIndex = bit;
        b.name = std::to_string(entry);
        b.mapId = B::MAP_ID;
        b.x = x;
        b.y = y;
        b.z = z;
        return b;
    }

    // BossSpawnIndex for 229 (Urok, Gizrul and Solakar are summoned), through the
    // map-229 roster patch.
    std::vector<DungeonBossInfo> BrsRoster()
    {
        std::vector<DungeonBossInfo> const index = {
            Boss(9196,  0,  -22.8f, -300.7f,  31.8f),   // Omokk
            Boss(9236,  1, -121.2f, -482.2f,  24.7f),   // Vosh'gajin
            Boss(9237,  2,  -17.0f, -459.1f, -18.6f),   // Voone
            Boss(10596, 3, -135.5f, -565.8f,  10.2f),   // Smolderweb
            Boss(9736,  5, -190.5f, -475.6f,  87.4f),   // Zigris
            Boss(10220, 7, -193.9f, -338.1f,  64.5f),   // Halycon
            Boss(9568,  8,  -22.6f, -486.2f,  90.8f),   // Wyrmthalak
            Boss(9816,  9,  144.4f, -258.0f,  96.4f),   // Emberseer
            Boss(10429, 11, 159.3f, -443.6f, 122.1f),   // Rend
            Boss(10430, 12, 124.2f, -563.8f, 107.4f),   // The Beast
            Boss(10363, 13,  36.5f, -286.0f, 111.0f),   // Drakkisath
        };
        return BossRosterRegistry::Apply(B::MAP_ID, DcDiffKey::Dungeon(DUNGEON_DIFFICULTY_NORMAL), index);
    }

    std::vector<DungeonBossInfo> Wing(char const* token)
    {
        DungeonWingLayout const* layout = DungeonWingRegistry::Get(B::MAP_ID);
        EXPECT_NE(layout, nullptr);
        return DcRunWing::FilterToWing(BrsRoster(), *DungeonWingRegistry::FindWing(*layout, token));
    }

    uint32 Obj(uint32 seq) { return BossRosterRegistry::ObjectiveEntry(seq); }

    DungeonEvent const& Event(uint32 id)
    {
        DungeonEvent const* e = DungeonEventRegistry::Find(B::MAP_ID, id);
        EXPECT_NE(e, nullptr) << "map 229 event " << id;
        return *e;
    }
}

// ---- the roster ------------------------------------------------------------------

// A UBRS run, in clear order: each gate objective just ahead of the boss it
// unlocks. Solakar has no static spawn and is never in the run.
TEST(DcBlackrockSpireTest, UbrsRunOpensTheDoorThenClearsTheHallBeforeEmberseer)
{
    std::vector<DungeonBossInfo> const ubrs = Wing("ubrs");
    std::vector<uint32> const want = {
        Obj(B::OBJ_DRAGONSPINE_DOOR), Obj(B::OBJ_DRAGONSPIRE_HALL), Obj(B::OBJ_BLACKROCK_ALTAR),
        9816, Obj(B::OBJ_BLACKROCK_STADIUM), 10429, 10430, 10363,
    };
    ASSERT_GE(ubrs.size(), want.size());
    // Only the door, the hall and the bosses are pinned here; later objectives
    // slot between them.
    std::vector<uint32> got;
    for (DungeonBossInfo const& b : ubrs)
        for (uint32 w : want)
            if (b.entry == w)
                got.push_back(b.entry);
    EXPECT_EQ(got, want);
    EXPECT_EQ(ubrs.front().eventId, B::EVENT_DRAGONSPINE_DOOR);
    EXPECT_EQ(ubrs.back().entry, 10363u) << "Drakkisath is terminal";
    for (DungeonBossInfo const& b : ubrs)
        EXPECT_NE(b.entry, 10264u) << "Solakar has no static spawn";
}

TEST(DcBlackrockSpireTest, LbrsRunIsUntouchedByTheUbrsObjectives)
{
    std::vector<DungeonBossInfo> const lbrs = Wing("lbrs");
    ASSERT_EQ(lbrs.size(), 7u);
    for (DungeonBossInfo const& b : lbrs)
        EXPECT_NE(b.kind, DungeonAnchorKind::Objective) << b.name;
    EXPECT_EQ(lbrs.back().entry, 9568u);
}

TEST(DcBlackrockSpireTest, ObjectivesCarryTheirEventsAndAnchorAtTheirFirstStep)
{
    for (DungeonBossInfo const& b : BrsRoster())
    {
        if (b.kind != DungeonAnchorKind::Objective)
            continue;
        DungeonEvent const& e = Event(b.eventId);
        EXPECT_EQ(e.activation, EventActivation::Anchored) << b.name;
        EXPECT_EQ(e.orderIndex, b.encounterIndex) << b.name;
        ASSERT_FALSE(e.steps.empty());
        EXPECT_EQ(e.steps[0].kind, EventStepKind::MoveTo) << b.name;
        // The objective delivers the tank onto its event's first MoveTo.
        EXPECT_LT(std::hypot(e.steps[0].x - b.x, e.steps[0].y - b.y), 0.5f) << b.name;
    }
}

TEST(DcBlackrockSpireTest, UbrsTestRowHasTheLongNoProgressBudget)
{
    DcTestDungeonRegistry::Row const* row = DcTestDungeonRegistry::Find("ubrs");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->noProgressS, 900u);
}

// ---- event 1: the Dragonspine Door -------------------------------------------------

TEST(DcBlackrockSpireTest, DragonspineDoorEventShape)
{
    DungeonEvent const& e = Event(B::EVENT_DRAGONSPINE_DOOR);
    EXPECT_EQ(e.orderIndex, 9u);
    EXPECT_TRUE(e.persistent);
    // Completing on the door would skip the hall arming under the stock core,
    // where the door spawns open.
    EXPECT_EQ(e.completeGoEntry, 0u);

    ASSERT_EQ(e.steps.size(), 3u);
    EXPECT_EQ(e.steps[0].kind, EventStepKind::MoveTo);
    EXPECT_FLOAT_EQ(e.steps[0].x, B::DOOR_STAND_X);
    EXPECT_EQ(e.steps[1].kind, EventStepKind::Custom);
    EXPECT_EQ(e.steps[1].hookId, B::HOOK_OPEN_DRAGONSPINE_DOOR);
    EXPECT_GT(e.steps[1].timeoutMs, B::DOOR_SCRIPT_WAIT_MS + B::DOOR_CLICK_WAIT_MS)
        << "the hook must reach Blocked before the step times out";
    EXPECT_EQ(e.steps[2].kind, EventStepKind::WaitForGameObjectState);
    EXPECT_EQ(e.steps[2].goEntry, B::GO_DRAGONSPINE_DOOR);
    EXPECT_EQ(e.steps[2].wantState, B::GO_STATE_OPEN);
    // The door is 22yd from the stand point: past the 20yd default search.
    float const door = std::hypot(126.879f - B::DOOR_STAND_X, -319.109f - B::DOOR_STAND_Y);
    EXPECT_GT(door, 20.0f);
    EXPECT_GT(e.steps[2].radius, door);
    EXPECT_GT(B::DOOR_SCAN, door);
}

// The stand point is inside AT 2046 (r10 around (102.43,-319.17)), so arriving
// there trips it for a client — and the test relay — before the hook fires it.
TEST(DcBlackrockSpireTest, DoorStandIsInsideTheDragonspireHallTrigger)
{
    EXPECT_LT(std::hypot(B::DOOR_STAND_X - 102.43f, B::DOOR_STAND_Y - -319.17f), 10.0f);
}

// The three states mod-individual-progression can leave the door in.
TEST(DcBlackrockSpireTest, DoorDecisionCoversAllThreeIndividualProgressionStates)
{
    using DcUbrs::DoorStep;
    using DcUbrs::DecideDragonspineDoor;

    // Stock core: the door spawns open — Done on the first tick.
    EXPECT_EQ(DecideDragonspineDoor(true, 0), DoorStep::Done);
    EXPECT_STREQ(DcUbrs::DoorOpenedVia(0, false), "already-open");

    // IP compiled: shut, SetData opens it ~3s later — Wait, then Done.
    EXPECT_EQ(DecideDragonspineDoor(false, 0), DoorStep::Wait);
    EXPECT_EQ(DecideDragonspineDoor(false, 3000), DoorStep::Wait);
    EXPECT_EQ(DecideDragonspineDoor(true, 3200), DoorStep::Done);
    EXPECT_STREQ(DcUbrs::DoorOpenedVia(3200, false), "ip-script");

    // IP SQL without its code: shut forever — Click once the script had its
    // chance, Done if the click took, Blocked if nothing did.
    EXPECT_EQ(DecideDragonspineDoor(false, B::DOOR_SCRIPT_WAIT_MS), DoorStep::Click);
    EXPECT_EQ(DecideDragonspineDoor(true, B::DOOR_SCRIPT_WAIT_MS + 500), DoorStep::Done);
    EXPECT_STREQ(DcUbrs::DoorOpenedVia(B::DOOR_SCRIPT_WAIT_MS + 500, true), "click");
    EXPECT_EQ(DecideDragonspineDoor(false, B::DOOR_SCRIPT_WAIT_MS + B::DOOR_CLICK_WAIT_MS - 1), DoorStep::Click);
    EXPECT_EQ(DecideDragonspineDoor(false, B::DOOR_SCRIPT_WAIT_MS + B::DOOR_CLICK_WAIT_MS), DoorStep::Blocked);

    // An open door always wins, however late.
    EXPECT_EQ(DecideDragonspineDoor(true, 600000), DoorStep::Done);
}

// ---- event 2: the Dragonspire Hall -------------------------------------------------

TEST(DcBlackrockSpireTest, DragonspireHallEventShape)
{
    DungeonEvent const& e = Event(B::EVENT_DRAGONSPIRE_HALL);
    EXPECT_EQ(e.orderIndex, 9u);
    EXPECT_TRUE(e.persistent);
    EXPECT_EQ(e.completeGoEntry, B::GO_EMBERSEER_IN);
    EXPECT_EQ(e.completeGoState, B::GO_STATE_OPEN);
    EXPECT_GE(e.completeGoRadius, B::HALL_SCAN);

    constexpr std::size_t kRunes = sizeof(B::HALL_RUNES) / sizeof(B::HALL_RUNES[0]);
    ASSERT_EQ(kRunes, 7u);
    ASSERT_EQ(e.steps.size(), 1 + kRunes + 1);

    // Step 0 is Done anywhere in the hall, so a sweep Restart never walks the
    // tank back to the door.
    EXPECT_EQ(e.steps[0].kind, EventStepKind::MoveTo);
    EXPECT_GE(e.steps[0].radius, B::HALL_SCAN);

    for (std::size_t i = 0; i < kRunes; ++i)
    {
        EventStep const& s = e.steps[1 + i];
        EXPECT_EQ(s.kind, EventStepKind::ClearRadius) << "room " << B::HALL_RUNES[i].room;
        EXPECT_FLOAT_EQ(s.x, B::HALL_RUNES[i].x);
        EXPECT_FLOAT_EQ(s.radius, B::HALL_RUNE_CLEAR_RADIUS);
        EXPECT_GE(s.radius, 15.0f) << "the instance lists every pack mob within 15yd";
        EXPECT_EQ(s.entryFilter, (std::vector<uint32>{ B::NPC_BLACKHAND_DREADWEAVER,
                                                       B::NPC_BLACKHAND_SUMMONER,
                                                       B::NPC_BLACKHAND_VETERAN }));
    }
    EXPECT_EQ(e.steps.back().kind, EventStepKind::Custom);
    EXPECT_EQ(e.steps.back().hookId, B::HOOK_HALL_RUNES_SWEEP);
}

// The Veteran/Summoner pack at (217-226, -258..-266, z 82) is not rune-tied (30yd
// from rune 7); no room volume may reach it.
TEST(DcBlackrockSpireTest, RuneRoomVolumesStayOffTheUnlistedPack)
{
    for (B::HallRune const& r : B::HALL_RUNES)
        EXPECT_GT(std::hypot(217.0f - r.x, -258.0f - r.y), B::HALL_RUNE_CLEAR_RADIUS) << "room " << r.room;
}

TEST(DcBlackrockSpireTest, HallSweepDecision)
{
    using DcUbrs::HallStep;
    using DcUbrs::DecideHallSweep;

    // Emberseer In open: done, whatever else reads.
    EXPECT_EQ(DecideHallSweep(true, 3, 0), HallStep::Done);
    // A lit rune with its pack still at it: back over the rooms.
    EXPECT_EQ(DecideHallSweep(false, 1, 0), HallStep::Restart);
    // Nobody left at a lit rune: give CHECK (every 3s) time, then re-arm.
    EXPECT_EQ(DecideHallSweep(false, 0, 0), HallStep::Wait);
    EXPECT_EQ(DecideHallSweep(false, 0, B::HALL_REARM_AFTER_MS - 1), HallStep::Wait);
    EXPECT_EQ(DecideHallSweep(false, 0, B::HALL_REARM_AFTER_MS), HallStep::Rearm);
    EXPECT_GT(B::HALL_REARM_AFTER_MS, 3000u * 2) << "two CHECK periods before a re-arm";
}

// ---- event 3: the Blackrock Altar ---------------------------------------------------

TEST(DcBlackrockSpireTest, BlackrockAltarEventShape)
{
    DungeonEvent const& e = Event(B::EVENT_BLACKROCK_ALTAR);
    EXPECT_EQ(e.orderIndex, 9u);
    EXPECT_TRUE(e.persistent);
    EXPECT_EQ(e.completeGoEntry, B::GO_EMBERSEER_OUT);
    EXPECT_EQ(e.completeGoState, B::GO_STATE_OPEN);
    // Emberseer Out is 35yd from the stand point: past the 20yd default.
    EXPECT_GT(e.completeGoRadius, std::hypot(113.751f - B::ALTAR_STAND_X, -258.945f - B::ALTAR_STAND_Y));

    ASSERT_EQ(e.steps.size(), 5u);
    EXPECT_EQ(e.steps[0].kind, EventStepKind::MoveTo);
    // Muster BEFORE the altar: the incarcerators seal the room 2s after it.
    EXPECT_EQ(e.steps[1].kind, EventStepKind::Custom);
    EXPECT_EQ(e.steps[1].hookId, B::HOOK_MUSTER_HALL_OF_BINDING);
    EXPECT_GT(e.steps[1].timeoutMs, B::MUSTER_CAP_MS) << "the cap, not the timeout, ends the muster";
    EXPECT_EQ(e.steps[2].kind, EventStepKind::Custom);
    EXPECT_EQ(e.steps[2].hookId, B::HOOK_USE_BLACKROCK_ALTAR);
    EXPECT_GT(e.steps[2].timeoutMs, B::ALTAR_FALLBACK_MS) << "the fallback must fire before the timeout";
    EXPECT_EQ(e.steps[3].kind, EventStepKind::ClearRadius);
    EXPECT_EQ(e.steps[3].entryFilter, (std::vector<uint32>{ B::NPC_BLACKHAND_INCARCERATOR }));
    EXPECT_EQ(e.steps[4].kind, EventStepKind::Custom);
    EXPECT_EQ(e.steps[4].hookId, B::HOOK_EMBERSEER_FREED);
    EXPECT_GE(e.steps[4].timeoutMs, 69000u) << "his release takes ~69s from the altar";
}

// Every incarcerator corner of x 126-162, y -241..-277 is inside the clear, and the
// clear is judged from where it is centred: the stand point, not his platform.
TEST(DcBlackrockSpireTest, IncarceratorClearReachesAllSevenFromTheStandPoint)
{
    DungeonEvent const& e = Event(B::EVENT_BLACKROCK_ALTAR);
    EventStep const& c = e.steps[3];
    EXPECT_FLOAT_EQ(c.x, B::ALTAR_STAND_X);
    EXPECT_FLOAT_EQ(c.y, B::ALTAR_STAND_Y);
    for (float x : { 126.0f, 162.0f })
        for (float y : { -241.0f, -277.0f })
            EXPECT_LT(std::hypot(x - c.x, y - c.y), c.radius) << x << "," << y;
}

TEST(DcBlackrockSpireTest, HallOfBindingMusterBox)
{
    auto const in = [](float x, float y, float z) { return B::InBox(B::HALL_OF_BINDING, x, y, z); };
    EXPECT_TRUE(in(B::ALTAR_STAND_X, B::ALTAR_STAND_Y, B::ALTAR_STAND_Z));
    EXPECT_TRUE(in(144.4f, -258.0f, 96.4f)) << "Emberseer's platform";
    EXPECT_TRUE(in(126.0f, -241.0f, 91.6f)) << "an incarcerator";
    // The ramp foot inside the Doors (175.1,-258.9) is the way in, not the room.
    EXPECT_FALSE(in(176.0f, -258.9f, 91.6f));
    // The hall floor under the room is z 71: outside, however close in XY.
    EXPECT_FALSE(in(155.3f, -270.0f, 70.9f));
    // The Rookery floor west of Emberseer Out.
    EXPECT_FALSE(in(100.0f, -258.5f, 91.6f));
}

TEST(DcBlackrockSpireTest, MusterDecision)
{
    using DcUbrs::MusterStep;
    EXPECT_EQ(DcUbrs::DecideMuster(0, 0, B::MUSTER_CAP_MS), MusterStep::Done);
    EXPECT_EQ(DcUbrs::DecideMuster(2, 0, B::MUSTER_CAP_MS), MusterStep::Wait);
    EXPECT_EQ(DcUbrs::DecideMuster(2, B::MUSTER_CAP_MS - 1, B::MUSTER_CAP_MS), MusterStep::Wait);
    EXPECT_EQ(DcUbrs::DecideMuster(1, B::MUSTER_CAP_MS, B::MUSTER_CAP_MS), MusterStep::CapReached);
}

TEST(DcBlackrockSpireTest, AltarDecisionHoldsTheRitualChannel)
{
    using DcUbrs::AltarStep;
    using DcUbrs::DecideAltar;
    // Released (or a human beat us to it): done.
    EXPECT_EQ(DecideAltar(B::STATE_IN_PROGRESS, false, false, 0), AltarStep::Done);
    EXPECT_EQ(DecideAltar(B::STATE_DONE, false, true, 0), AltarStep::Done);
    // Ready: use it; channelling: stand still — even past the fallback time, a
    // channel up is a ritual about to land.
    EXPECT_EQ(DecideAltar(B::STATE_NOT_STARTED, false, true, 0), AltarStep::Use);
    EXPECT_EQ(DecideAltar(B::STATE_NOT_STARTED, true, false, 2000), AltarStep::Hold);
    EXPECT_EQ(DecideAltar(B::STATE_NOT_STARTED, true, false, B::ALTAR_FALLBACK_MS + 1), AltarStep::Hold);
    // Busy (mid-ritual / reset cooldown): wait.
    EXPECT_EQ(DecideAltar(B::STATE_NOT_STARTED, false, false, 2000), AltarStep::Wait);
    // The ritual never took: the script's own entry point.
    EXPECT_EQ(DecideAltar(B::STATE_NOT_STARTED, false, true, B::ALTAR_FALLBACK_MS), AltarStep::Fallback);
}

TEST(DcBlackrockSpireTest, EmberseerFreedDecision)
{
    using DcUbrs::FreedStep;
    using DcUbrs::DecideEmberseerFreed;
    // Still bound (IN_PROGRESS, unselectable/immune): hold the roster's row back.
    EXPECT_EQ(DecideEmberseerFreed(B::STATE_IN_PROGRESS, true, false), FreedStep::Wait);
    EXPECT_EQ(DecideEmberseerFreed(B::STATE_IN_PROGRESS, true, true), FreedStep::Done);
    // An incarcerator's Reset put the encounter back: run it again.
    EXPECT_EQ(DecideEmberseerFreed(B::STATE_NOT_STARTED, true, false), FreedStep::Restart);
    // Dead or DONE: nothing to wait for.
    EXPECT_EQ(DecideEmberseerFreed(B::STATE_DONE, false, false), FreedStep::Done);
    EXPECT_EQ(DecideEmberseerFreed(B::STATE_IN_PROGRESS, false, false), FreedStep::Done);
}

// ---- the Rookery crossing ---------------------------------------------------------

// The measured lane is untaxed, and keeps 3.5yd+ off every north-floor egg.
TEST(DcBlackrockSpireTest, RookeryLaneIsUntaxedAndEggClear)
{
    for (B::LanePoint const& p : B::ROOKERY_LANE)
        for (char const* wing : { "", "ubrs" })
            EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(B::MAP_ID, p.x, p.y, p.z, wing), 1.0f)
                << "(" << p.x << ", " << p.y << ") run wing '" << wing << "'";

    for (std::size_t i = 0; i + 1 < sizeof(B::ROOKERY_LANE) / sizeof(B::ROOKERY_LANE[0]); ++i)
    {
        B::LanePoint const& a = B::ROOKERY_LANE[i];
        B::LanePoint const& b = B::ROOKERY_LANE[i + 1];
        for (int k = 0; k <= 20; ++k)
        {
            float const t = k / 20.0f;
            float const x = a.x + (b.x - a.x) * t, y = a.y + (b.y - a.y) * t, z = a.z + (b.z - a.z) * t;
            for (DcRookeryEggs::Egg const& e : DcRookeryEggs::NORTH_FLOOR)
                if (std::fabs(z - e.z) < 4.0f)
                    EXPECT_GE(std::hypot(x - e.x, y - e.y), 3.5f)
                        << "lane segment " << i << " passes egg (" << e.x << ", " << e.y << ")";
        }
    }
}

// Nothing taxes the Rookery floor for a UBRS run: boxing its egg clusters pushed
// the corridor round the south half into the eggs (DcNavPenaltyRegistry's 229
// note). The fallback still reads the Rookery as UBRS.
TEST(DcBlackrockSpireTest, NoPenaltyRowTaxesTheRookeryFloor)
{
    for (DcRookeryEggs::Egg const& e : DcRookeryEggs::NORTH_FLOOR)
        EXPECT_FLOAT_EQ(DcNavPenaltyRegistry::PenaltyAt(B::MAP_ID, e.x, e.y, e.z, "ubrs"), 1.0f)
            << "egg (" << e.x << ", " << e.y << ")";
    char const* w = DcNavPenaltyRegistry::WingRegionAt(B::MAP_ID, 80.0f, -265.0f, 91.5f);
    ASSERT_NE(w, nullptr);
    EXPECT_STREQ(w, "ubrs");
}

// The authored crossing: registered for the leg out of Emberseer's room, every
// leg short, the measured lane walked point for point with a tight arrive radius.
TEST(DcBlackrockSpireTest, RookeryAnchorRouteWalksTheLane)
{
    std::vector<WaypointHint> const* hints =
        DungeonClearRouteRegistry::Get(B::MAP_ID, DUNGEON_DIFFICULTY_NORMAL, Obj(B::OBJ_BLACKROCK_STADIUM));
    ASSERT_NE(hints, nullptr);
    constexpr std::size_t kN = sizeof(B::ROOKERY_ROUTE) / sizeof(B::ROOKERY_ROUTE[0]);
    ASSERT_EQ(hints->size(), kN);

    // Starts in Emberseer's room (where he dies), ends on the stadium objective's
    // anchor, the muster point.
    EXPECT_LT(std::hypot((*hints)[0].x - 144.4f, (*hints)[0].y - -258.0f), 10.0f);
    EXPECT_LT(std::hypot(hints->back().x - B::STADIUM_MUSTER_X, hints->back().y - B::STADIUM_MUSTER_Y), 0.5f);
    // The leg out of Emberseer's room leads to the stadium objective.
    std::vector<DungeonBossInfo> const ubrs = Wing("ubrs");
    for (std::size_t i = 0; i + 1 < ubrs.size(); ++i)
        if (ubrs[i].entry == 9816)
            EXPECT_EQ(ubrs[i + 1].entry, Obj(B::OBJ_BLACKROCK_STADIUM));

    for (std::size_t i = 0; i + 1 < kN; ++i)
        EXPECT_LT(std::hypot((*hints)[i + 1].x - (*hints)[i].x, (*hints)[i + 1].y - (*hints)[i].y), 10.0f)
            << "leg " << i;

    // Every measured lane point is an anchor inside the tight-radius span.
    for (B::LanePoint const& l : B::ROOKERY_LANE)
    {
        bool found = false;
        for (uint32 i = B::ROOKERY_ROUTE_LANE_FIRST; i <= B::ROOKERY_ROUTE_LANE_LAST; ++i)
            found = found || (std::fabs((*hints)[i].x - l.x) < 0.01f && std::fabs((*hints)[i].y - l.y) < 0.01f);
        EXPECT_TRUE(found) << "lane point (" << l.x << ", " << l.y << ")";
    }
    for (std::size_t i = 0; i < kN; ++i)
    {
        bool const lane = i >= B::ROOKERY_ROUTE_LANE_FIRST && i <= B::ROOKERY_ROUTE_LANE_LAST;
        EXPECT_FLOAT_EQ((*hints)[i].arriveRadius, lane ? B::ROOKERY_ANCHOR_ARRIVE : WaypointHint{}.arriveRadius)
            << "anchor " << i;
    }
    // The tight radius keeps a corner cut inside the lane's clearance.
    EXPECT_LT(B::ROOKERY_ANCHOR_ARRIVE, 3.8f - 1.5f);
}

TEST(DcBlackrockSpireTest, RookeryEggIsAKeepOutTrapWithNoVacate)
{
    DcTrapHazard const* egg = DcHazardRegistry::FindTrap(B::MAP_ID, B::GO_ROOKERY_EGG);
    ASSERT_NE(egg, nullptr);
    EXPECT_GT(egg->radius, 1.5f) << "wider than the egg's own trigger";
    EXPECT_FLOAT_EQ(egg->vacateRadius, 0.0f) << "a fired egg is gone; retreating hatches more";
    EXPECT_GT(egg->retreatSlack, egg->holdBand);
    EXPECT_TRUE(DcHazardRegistry::HasTrapHazards(B::MAP_ID));
}

TEST(DcBlackrockSpireTest, RookeryWhelpsAreNeverHunted)
{
    EXPECT_TRUE(DcNeverTargetRegistry::IsNeverTarget(B::MAP_ID, B::NPC_ROOKERY_WHELP));
    EXPECT_FALSE(DcNeverTargetRegistry::IsNeverTarget(230, B::NPC_ROOKERY_WHELP));
}

// ---- event 4: the Blackrock Stadium -------------------------------------------------

TEST(DcBlackrockSpireTest, BlackrockStadiumEventShape)
{
    DungeonEvent const& e = Event(B::EVENT_BLACKROCK_STADIUM);
    EXPECT_EQ(e.orderIndex, 11u) << "Rend's index";
    EXPECT_TRUE(e.persistent);

    ASSERT_EQ(e.steps.size(), 5u);
    EXPECT_EQ(e.steps[0].kind, EventStepKind::MoveTo);
    EXPECT_FLOAT_EQ(e.steps[0].x, B::STADIUM_MUSTER_X);
    EXPECT_EQ(e.steps[1].hookId, B::HOOK_MUSTER_IN_STADIUM);
    EXPECT_EQ(e.steps[2].kind, EventStepKind::MoveTo);
    EXPECT_FLOAT_EQ(e.steps[2].x, B::ARENA_STAND_X);
    EXPECT_EQ(e.steps[3].hookId, B::HOOK_ENSURE_STADIUM_STARTED);

    EventStep const& hold = e.steps[4];
    EXPECT_EQ(hold.kind, EventStepKind::MoveTo);
    EXPECT_EQ(hold.bossStateId, static_cast<int32>(B::DATA_WARCHIEF_REND_BLACKHAND));
    EXPECT_EQ(hold.bossStateClearMask, DcBossStateBit(B::STATE_DONE));
    // FAIL is overwritten by Rend's Reset (NOT_STARTED) within the tick: restart
    // on either, or the hold can miss the reset.
    EXPECT_EQ(hold.restartOnBossStateMask, DcBossStateBit(B::STATE_FAIL) | DcBossStateBit(B::STATE_NOT_STARTED));
    EXPECT_EQ(DecideBossStateGate(B::STATE_IN_PROGRESS, hold.bossStateClearMask, hold.restartOnBossStateMask),
              BossStateGateVerdict::Hold);
    EXPECT_EQ(DecideBossStateGate(B::STATE_DONE, hold.bossStateClearMask, hold.restartOnBossStateMask),
              BossStateGateVerdict::Clear);
    EXPECT_EQ(DecideBossStateGate(B::STATE_NOT_STARTED, hold.bossStateClearMask, hold.restartOnBossStateMask),
              BossStateGateVerdict::Restart);
    EXPECT_FLOAT_EQ(hold.x, B::ARENA_STAND_X);
    EXPECT_GE(hold.timeoutMs, 8u * 60u * 1000u);
}

TEST(DcBlackrockSpireTest, StadiumGeometry)
{
    auto const atDist = [](float x, float y) { return std::hypot(x - B::AT_STADIUM_X, y - B::AT_STADIUM_Y); };
    // The muster point is past the portcullis (x 108) and well outside AT 2026,
    // with room for a member a few yards off it.
    EXPECT_GT(B::STADIUM_MUSTER_X, 108.04f);
    EXPECT_GT(atDist(B::STADIUM_MUSTER_X, B::STADIUM_MUSTER_Y), B::AT_STADIUM_RADIUS + 10.0f);
    EXPECT_TRUE(B::InBox(B::STADIUM_ARENA, B::STADIUM_MUSTER_X, B::STADIUM_MUSTER_Y, B::STADIUM_MUSTER_Z));
    // The stand is inside the trigger, and the box holds everyone past the gate.
    EXPECT_LT(atDist(B::ARENA_STAND_X, B::ARENA_STAND_Y), B::AT_STADIUM_RADIUS - B::ARENA_HOLD_RADIUS);
    EXPECT_TRUE(B::InBox(B::STADIUM_ARENA, B::ARENA_STAND_X, B::ARENA_STAND_Y, B::ARENA_STAND_Z));
    EXPECT_GT(B::STADIUM_ARENA.minX, 108.04f) << "nobody counts as inside while still in the gateway";
    // A member on the upper floor or the z118 walkway outside is not inside.
    EXPECT_FALSE(B::InBox(B::STADIUM_ARENA, 133.7f, -362.8f, 118.0f));
    EXPECT_FALSE(B::InBox(B::STADIUM_ARENA, 95.5f, -406.0f, 112.7f));
    // The pen (175185 at 194.2,-420.2): summons despawn with no player in 100yd.
    EXPECT_LT(std::hypot(B::ARENA_STAND_X - 194.2f, B::ARENA_STAND_Y - -420.2f) + B::ARENA_HOLD_RADIUS, 100.0f);
    // The AT script looks for Rend within 100yd of the player (balcony 159.3,-443.6,122.1).
    EXPECT_LT(std::hypot(B::ARENA_STAND_X - 159.3f, B::ARENA_STAND_Y - -443.6f), 100.0f);
}

TEST(DcBlackrockSpireTest, StadiumStartDecision)
{
    using DcUbrs::StadiumStep;
    EXPECT_EQ(DcUbrs::DecideStadiumStart(B::STATE_NOT_STARTED), StadiumStep::Fire);
    EXPECT_EQ(DcUbrs::DecideStadiumStart(B::STATE_IN_PROGRESS), StadiumStep::Done);
    EXPECT_EQ(DcUbrs::DecideStadiumStart(B::STATE_DONE), StadiumStep::Done);
    EXPECT_EQ(DcUbrs::DecideStadiumStart(B::STATE_FAIL), StadiumStep::Wait) << "the respawn is a tick away";
}

TEST(DcBlackrockSpireTest, NefariusIsNeverTargetedButRendIs)
{
    EXPECT_TRUE(DcNeverTargetRegistry::IsNeverTarget(B::MAP_ID, B::NPC_LORD_VICTOR_NEFARIUS));
    EXPECT_FALSE(DcNeverTargetRegistry::IsNeverTarget(B::MAP_ID, B::NPC_WARCHIEF_REND_BLACKHAND));
}

// ---- The Beast and General Drakkisath --------------------------------------------

// The Beast is fought on his camp, and his roster row is anchored on it (or boss
// navigation walks the party to his live, patrolling position and the pull never
// arms).
TEST(DcBlackrockSpireTest, TheBeastIsFoughtOnHisCampSouthOfHisEvadeLine)
{
    BossPullback const* row = BossPullbackRegistry::Find(B::MAP_ID, B::NPC_THE_BEAST);
    ASSERT_NE(row, nullptr);
    EXPECT_FLOAT_EQ(row->campX, B::BEAST_CAMP_X);
    EXPECT_FLOAT_EQ(row->campY, B::BEAST_CAMP_Y);
    EXPECT_FLOAT_EQ(row->campZ, B::BEAST_CAMP_Z);
    EXPECT_FLOAT_EQ(row->forceAggroRange, 0.0f) << "he is hostile; tag him the normal way";
    EXPECT_FALSE(row->summonWhenStuckBelow);
    EXPECT_LE(row->campY, B::BEAST_EVADE_Y - 30.0f) << "he evades above y -505";
    // Inside AT 2066 (75.26,-547.68 r30): arriving starts his patrol.
    EXPECT_LT(std::hypot(row->campX - 75.26f, row->campY - -547.68f), 30.0f);

    bool found = false;
    for (DungeonBossInfo const& b : BrsRoster())
    {
        if (b.entry != B::NPC_THE_BEAST)
            continue;
        found = true;
        EXPECT_EQ(b.kind, DungeonAnchorKind::Boss);
        EXPECT_EQ(b.encounterIndex, 12u) << "keeps his kill-bit";
        EXPECT_FLOAT_EQ(b.x, row->campX);
        EXPECT_FLOAT_EQ(b.y, row->campY);
    }
    EXPECT_TRUE(found);
}

// Drakkisath's room pre-clear takes every pack around his room and stops at the
// corridors. Spawns from the base world SQL.
TEST(DcBlackrockSpireTest, DrakkisathRoomClearStopsAtTheCorridors)
{
    RoomAggroBoss const* row = RoomAggroRegistry::Find(B::MAP_ID, B::NPC_GENERAL_DRAKKISATH);
    ASSERT_NE(row, nullptr);
    EXPECT_FLOAT_EQ(row->radius, B::DRAKKISATH_ROOM_RADIUS);
    EXPECT_TRUE(row->memberEntries.empty());

    struct Mob { float x, y; char const* what; };
    Mob const in[] = {
        { 24.4f, -268.2f, "north pair Elite" }, { 23.7f, -258.2f, "north pair Fire Tongue" },
        { 34.0f, -327.5f, "south archway Captain" }, { 30.4f, -332.9f, "south archway Elite" },
        { 37.3f, -333.8f, "south archway Elite" }, { 40.7f, -330.5f, "south archway Iron Guard" },
        { -3.3f, -259.9f, "western Elite" }, { -16.0f, -263.3f, "western Captain" },
    };
    for (Mob const& m : in)
        EXPECT_LT(std::hypot(m.x - B::DRAKKISATH_X, m.y - B::DRAKKISATH_Y), row->radius) << m.what;
    Mob const out[] = {
        { -8.1f, -341.9f, "south-west corridor Iron Guard" }, { 30.5f, -381.2f, "south corridor pack" },
        { 102.1f, -255.0f, "Hall of Blackhand Dragonspawn" },
    };
    for (Mob const& m : out)
        EXPECT_GT(std::hypot(m.x - B::DRAKKISATH_X, m.y - B::DRAKKISATH_Y), row->radius) << m.what;
}

TEST(DcBlackrockSpireTest, UbrsRunEndsOnDrakkisath)
{
    std::vector<DungeonBossInfo> const ubrs = Wing("ubrs");
    ASSERT_FALSE(ubrs.empty());
    EXPECT_EQ(ubrs.back().entry, B::NPC_GENERAL_DRAKKISATH);
    std::size_t bosses = 0;
    for (DungeonBossInfo const& b : ubrs)
        bosses += b.kind == DungeonAnchorKind::Boss ? 1 : 0;
    EXPECT_EQ(bosses, 4u) << "Emberseer, Rend, The Beast, Drakkisath";
    DungeonWing const* wing = DungeonWingRegistry::FindWing(*DungeonWingRegistry::Get(B::MAP_ID), "ubrs");
    ASSERT_NE(wing, nullptr);
    EXPECT_TRUE(DcRunWing::TerminalDone(*wing, ubrs, 1u << 13, false));
    EXPECT_FALSE(DcRunWing::TerminalDone(*wing, ubrs, (1u << 9) | (1u << 11) | (1u << 12), false));
}

// ---- hooks and doors -------------------------------------------------------------

TEST(DcBlackrockSpireTest, HooksAreRegistered)
{
    EXPECT_TRUE(ObjectiveHookRegistry::Has(B::HOOK_OPEN_DRAGONSPINE_DOOR));
    EXPECT_TRUE(ObjectiveHookRegistry::Has(B::HOOK_HALL_RUNES_SWEEP));
    EXPECT_TRUE(ObjectiveHookRegistry::Has(B::HOOK_MUSTER_HALL_OF_BINDING));
    EXPECT_TRUE(ObjectiveHookRegistry::Has(B::HOOK_USE_BLACKROCK_ALTAR));
    EXPECT_TRUE(ObjectiveHookRegistry::Has(B::HOOK_EMBERSEER_FREED));
    EXPECT_TRUE(ObjectiveHookRegistry::Has(B::HOOK_MUSTER_IN_STADIUM));
    EXPECT_TRUE(ObjectiveHookRegistry::Has(B::HOOK_ENSURE_STADIUM_STARTED));
}

TEST(DcBlackrockSpireTest, EventOwnedDoorsAreScriptOnly)
{
    EXPECT_TRUE(DcEventDoorRegistry::IsScriptOnly(B::GO_DRAGONSPINE_DOOR));
    EXPECT_TRUE(DcEventDoorRegistry::IsScriptOnly(B::GO_EMBERSEER_IN));
    EXPECT_TRUE(DcEventDoorRegistry::IsScriptOnly(B::GO_DOORS));
    // Under IP a bare click would open the door without the braziers or its
    // saved flag; the event, not the walker, owns it.
    EXPECT_FALSE(DcEventDoorRegistry::IsLockFreeClickable(B::GO_DRAGONSPINE_DOOR));
    // Real gates: navigation must still see them shut.
    EXPECT_TRUE(DcEventDoorRegistry::IsScriptOnly(B::GO_EMBERSEER_OUT));
    for (uint32 go : { B::GO_GYTH_ENTRY_DOOR, B::GO_GYTH_COMBAT_DOOR, B::GO_GYTH_EXIT_DOOR,
                       B::GO_DRAKKISATH_DOOR_1, B::GO_DRAKKISATH_DOOR_2 })
        EXPECT_TRUE(DcEventDoorRegistry::IsScriptOnly(go)) << go;
    for (uint32 go : { B::GO_DRAGONSPINE_DOOR, B::GO_EMBERSEER_IN, B::GO_DOORS, B::GO_EMBERSEER_OUT,
                       B::GO_GYTH_ENTRY_DOOR, B::GO_GYTH_EXIT_DOOR })
        EXPECT_FALSE(DcEventDoorRegistry::IsNavigationIgnored(go)) << go;
}
