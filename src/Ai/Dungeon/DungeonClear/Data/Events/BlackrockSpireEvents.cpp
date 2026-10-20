/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonWingRegistry.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "Creature.h"
#include "GameObject.h"
#include "InstanceScript.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Ai/Dungeon/DungeonClear/Util/DcUbrsDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"

// Blackrock Spire (map 229) is two dungeons on one map: Lower (LFG 32, ends at
// Overlord Wyrmthalak) and Upper (LFG 44, Pyroguard Emberseer -> General
// Drakkisath). This file is its definition unit: the wing split, and UBRS's
// encounter gates as Anchored events on one map-229 roster patch. The TU stays
// linked because DungeonWingRegistry, DungeonEventRegistry, BossRosterRegistry and
// ObjectiveHookRegistry each call into it explicitly.
//
// MOD-INDIVIDUAL-PROGRESSION IS IN THE LOOP, AND EVERY EVENT HERE MUST WORK IN ALL
// THREE OF ITS STATES. IP registers instance_blackrock_spire and
// at_dragonspire_hall under the core's ScriptNames (the last registration wins, so
// its versions run whenever it is compiled in, whatever IndividualProgression.Enable
// says), and its world SQL for map 229 stays applied if the module is later
// removed. For UBRS that changes exactly one thing, the Dragonspine Door:
//
//   stock core       the door spawns open; nothing touches it
//   IP compiled      the door spawns shut; AT 2046 with a non-LFG player carrying
//                    the Seal of Ascension (12344) runs SetData(2046, 26), which
//                    lights the six braziers and opens it ~3s later (saved)
//   IP SQL only      the door spawns shut and nothing ever opens it
//
// and the shape of at_dragonspire_hall's call (core SetData(1, 2046), IP
// SetData(2046, 15)). The hall runes, Emberseer, the Rookery, Rend/Gyth and the
// rest are byte-identical. So nothing here includes an IP header, assumes the door
// is open or shut, or assumes which at_dragonspire_hall is registered: the door
// hook enters the SAME script entry points a player's act would.
//
// A bot cannot earn the Seal. The door hook stands in for "a party member holds
// it", the way BRD's key-locked doors are waived for a party that is already
// inside ("the party has access by being here"). That also skips IP's LFG-group
// rule; DC runs are explicit `.dc on ubrs` / test runs, and LFG 44 has no
// lfg_dungeon_template row on this core, so a queue-filled UBRS run cannot exist.

namespace
{
    using namespace DcBlackrockSpire;

    uint32 StepElapsedMs(AiObjectContext* context)
    {
        DungeonEventProgress const& prog =
            context->GetValue<DungeonEventProgress&>(DcKey::EventProgress)->Get();
        return getMSTimeDiff(prog.stepStartMs, getMSTime());
    }

    // True if `slot` was stamped within the last `withinMs` (without stamping it).
    bool StampedWithin(DcRunState const& run, DcThrottle slot, uint32 withinMs)
    {
        DcThrottleSlot const& s = run.throttles[static_cast<std::size_t>(slot)];
        return s.ms && GetMSTimeDiffToNow(s.ms) <= withinMs;
    }

    // Run AT 2046's script for `bot` exactly as the client's CMSG_AREATRIGGER
    // would reach it (ScriptMgr::OnAreaTrigger is what the opcode handler calls
    // once the radius check passes): STORE snapshots the rune packs and CHECK
    // starts, whichever at_dragonspire_hall is registered. A repeat re-snapshots,
    // which only ever drops a mob that has left its rune — never harmful.
    bool FireDragonspireHallTrigger(Player* bot)
    {
        AreaTrigger const* at = sObjectMgr->GetAreaTrigger(AT_DRAGONSPIRE_HALL);
        if (!at)
        {
            LOG_ERROR("playerbots.dungeonclear",
                      "[DC:{}] Upper Blackrock Spire — area trigger {} missing from the "
                      "world DB; the Dragonspire Hall runes cannot be armed",
                      bot->GetName(), AT_DRAGONSPIRE_HALL);
            return false;
        }
        return sScriptMgr->OnAreaTrigger(bot, at);
    }

    // --- OpenDragonspineDoor (hook 56) -------------------------------------
    // Layered so each of the header's three states passes:
    //   1. arm the hall (once per attempt): AT 2046's script, then IP's opener,
    //      SetData(2046, 26) — under IP exactly what its AT script does after the
    //      seal check; under the core a type it has no case for, a no-op
    //   2. Done once the door reads open (stock: at once; IP: ~3s)
    //   3. after DOOR_SCRIPT_WAIT_MS, click it: the player-click path for a
    //      lock-free door (template lock 0, no auto-close) — the IP-SQL-only state
    //   4. Blocked if even that has not opened it
    // The event owns the door: 164725 is IsScriptOnly, so the walker never pops
    // it before the hall is armed (it would still pause on it, but the run
    // stands at this objective before any path through the door is asked for).
    ObjectiveArriveResult OpenDragonspineDoor(Player* bot, AiObjectContext* context,
                                              DungeonBossInfo const& /*info*/)
    {
        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Running;

        DcRunState& run = DcRun::Of(context);
        if (!run.Throttled(DcThrottle::UbrsDoorArm, DOOR_SCRIPT_WAIT_MS + DOOR_CLICK_WAIT_MS))
        {
            bool const fired = FireDragonspireHallTrigger(bot);
            inst->SetData(AT_DRAGONSPIRE_HALL, IP_DATA_UBRS_DOOR_OPEN);
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] Upper Blackrock Spire — armed the Dragonspire Hall (AT {} script {}) "
                     "and asked the instance to open the Dragonspine Door (SetData({}, {}))",
                     bot->GetName(), AT_DRAGONSPIRE_HALL, fired ? "ran" : "did not run",
                     AT_DRAGONSPIRE_HALL, IP_DATA_UBRS_DOOR_OPEN);
        }

        GameObject* door = bot->FindNearestGameObject(GO_DRAGONSPINE_DOOR, DOOR_SCAN);
        if (!door)
            return ObjectiveArriveResult::Running;  // grid not loaded yet

        uint32 const elapsed = StepElapsedMs(context);
        bool const open = static_cast<uint32>(door->GetGoState()) == GO_STATE_OPEN;
        switch (DcUbrs::DecideDragonspineDoor(open, elapsed))
        {
            case DcUbrs::DoorStep::Done:
                LOG_INFO("playerbots.dungeonclear", "[DC:{}] UBRS door opened via {}", bot->GetName(),
                         DcUbrs::DoorOpenedVia(elapsed, StampedWithin(run, DcThrottle::UbrsDoorClick, elapsed)));
                return ObjectiveArriveResult::Done;
            case DcUbrs::DoorStep::Wait:
                return ObjectiveArriveResult::Running;
            case DcUbrs::DoorStep::Click:
                if (!run.Throttled(DcThrottle::UbrsDoorClick, DOOR_CLICK_WAIT_MS))
                {
                    door->UseDoorOrButton(0, false, bot);
                    LOG_WARN("playerbots.dungeonclear",
                             "[DC:{}] Upper Blackrock Spire — the Dragonspine Door is still shut {}s "
                             "after the instance was asked to open it (no script opener: "
                             "mod-individual-progression's SQL without its code?) — clicking it open",
                             bot->GetName(), elapsed / IN_MILLISECONDS);
                }
                return ObjectiveArriveResult::Running;
            case DcUbrs::DoorStep::Blocked:
                LOG_ERROR("playerbots.dungeonclear",
                          "[DC:{}] Upper Blackrock Spire — the Dragonspine Door (GO {}) is still shut "
                          "after the area trigger, the instance opener and a click; stalling here",
                          bot->GetName(), GO_DRAGONSPINE_DOOR);
                return ObjectiveArriveResult::Blocked;
        }
        return ObjectiveArriveResult::Running;
    }

    // --- HallRunesSweep (hook 57) ------------------------------------------
    // The last step of the hall event, after the seven rune-room ClearRadius
    // steps. The instance's CHECK unlights a rune once every mob STORE listed for
    // it is dead, and opens Emberseer In when all seven are out; this hook is
    // the safety net for what the room steps can let slip:
    //   * a lit rune with a living pack mob near it (a 15%-HP fleer that came
    //     home after its room completed, a Summoner add) -> Restart: the event
    //     rewinds onto the room steps, which clear it (step 0 is a MoveTo on the
    //     whole hall, Done wherever the tank is)
    //   * lit runes with nobody at them, or every rune out and the door still
    //     shut, for HALL_REARM_AFTER_MS -> CHECK is not running (the trigger was
    //     never tripped) or its snapshot holds a mob that is not coming back:
    //     re-fire AT 2046's script, which re-snapshots and restarts CHECK
    //   * Emberseer In open -> Done
    ObjectiveArriveResult HallRunesSweep(Player* bot, AiObjectContext* context,
                                         DungeonBossInfo const& /*info*/)
    {
        GameObject* in = bot->FindNearestGameObject(GO_EMBERSEER_IN, HALL_SCAN);
        bool const inOpen = in && static_cast<uint32>(in->GetGoState()) == GO_STATE_OPEN;

        uint32 lit = 0;
        uint32 litWithPack = 0;
        std::string stillLit;
        for (HallRune const& r : HALL_RUNES)
        {
            GameObject* rune = bot->FindNearestGameObject(r.goEntry, HALL_SCAN);
            if (!rune || static_cast<uint32>(rune->GetGoState()) != GO_STATE_OPEN)
                continue;
            ++lit;
            stillLit += (stillLit.empty() ? "" : ",") + std::to_string(r.room);
            for (uint32 entry : { NPC_BLACKHAND_DREADWEAVER, NPC_BLACKHAND_SUMMONER, NPC_BLACKHAND_VETERAN })
                if (rune->FindNearestCreature(entry, HALL_RUNE_CLEAR_RADIUS, /*alive*/ true))
                {
                    ++litWithPack;
                    break;
                }
        }

        DcRunState& run = DcRun::Of(context);
        uint32 const elapsed = StepElapsedMs(context);
        switch (DcUbrs::DecideHallSweep(inOpen, litWithPack, elapsed))
        {
            case DcUbrs::HallStep::Done:
                return ObjectiveArriveResult::Done;
            case DcUbrs::HallStep::Restart:
                if (run.Throttled(DcThrottle::UbrsHallRestart, HALL_RESTART_FLOOR_MS))
                    break;
                LOG_INFO("playerbots.dungeonclear",
                         "[DC:{}] Dragonspire Hall — {} lit rune(s) still have a pack mob at them "
                         "(rooms {}): back over the rune rooms", bot->GetName(), litWithPack, stillLit);
                return ObjectiveArriveResult::Restart;
            case DcUbrs::HallStep::Rearm:
                if (run.Throttled(DcThrottle::UbrsHallRearm, HALL_REARM_FLOOR_MS))
                    break;
                LOG_INFO("playerbots.dungeonclear",
                         "[DC:{}] Dragonspire Hall — {} rune(s) lit (rooms {}) with no pack left at "
                         "them and Emberseer In still shut: re-firing area trigger {} (script {})",
                         bot->GetName(), lit, stillLit.empty() ? "none" : stillLit, AT_DRAGONSPIRE_HALL,
                         FireDragonspireHallTrigger(bot) ? "ran" : "did not run");
                return ObjectiveArriveResult::Running;
            case DcUbrs::HallStep::Wait:
                break;
        }

        if (!run.Throttled(DcThrottle::UbrsEventLog, 30000))
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] Dragonspire Hall — waiting on the runes: still lit {}",
                     bot->GetName(), stillLit.empty() ? "none" : stillLit);
        return ObjectiveArriveResult::Running;
    }
}

void RegisterBlackrockSpireHooks(ObjectiveHookRegistry::HookTable& out)
{
    ObjectiveHookRegistry::AddHook(out, HOOK_OPEN_DRAGONSPINE_DOOR, &OpenDragonspineDoor);
    ObjectiveHookRegistry::AddHook(out, HOOK_HALL_RUNES_SWEEP, &HallRunesSweep);
}

void RegisterBlackrockSpireEvents(std::vector<DungeonEvent>& out)
{
    using namespace DcBlackrockSpire;

    // --- THE DRAGONSPINE DOOR, ANCHORED ahead of Emberseer (index 9) ------
    // Stand inside AT 2046, arm the hall and open the door (hook 56, see
    // OpenDragonspineDoor), then confirm on the door itself. No event-level
    // CompleteWhenGOState on the door: under the stock core it spawns open, and
    // completing on it would skip the arming the hall event depends on.
    //
    // PERSISTENT: the Scarshield packs that flank the door approach can start a
    // fight mid-event, and a rewind would re-arm a hall that is already armed.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_DRAGONSPINE_DOOR, "Dragonspine Door")
            .Anchored(/*orderIndex*/ 9)
            .Persistent()
            .MoveTo(DOOR_STAND_X, DOOR_STAND_Y, DOOR_STAND_Z, /*radius*/ 4.0f)
            .Custom(HOOK_OPEN_DRAGONSPINE_DOOR)
            .Timeout(DOOR_SCRIPT_WAIT_MS + DOOR_CLICK_WAIT_MS + 10000)
            .WaitForGOState(GO_DRAGONSPINE_DOOR, GO_STATE_OPEN, /*timeoutMs*/ 15000, DOOR_SCAN)
            .Build());

    // --- THE DRAGONSPIRE HALL, ANCHORED second on index 9 -----------------
    // Emberseer In (175244) opens only when all seven hall runes are out, and a
    // rune goes out only when every Blackhand pack mob the instance listed within
    // 15yd of it is dead. So: one ClearRadius per rune room, measured from the
    // live spawns (the DM West "large room" lesson: several small volumes, never
    // one giant one), in navmesh order — the lower floor 3, 1, 4, 2, up the ramp,
    // then 7, 6, 5, ending 49yd from Emberseer In. OnlyEntries keeps each room
    // from waiting on the Rage Talon Dragonspawn in the aisle, which the ordinary
    // engage layer fights as the party passes.
    //
    // Step 0 is a MoveTo on the anchor with the whole hall as its radius (the
    // Lyceum shape): Done on arrival, so the persistent event stamps its
    // progress before any room engages, and Done wherever a sweep Restart
    // rewinds from — it never walks the tank back to the door.
    //
    // The event completes whatever step it is on once Emberseer In reads open: a
    // room finished by an evade and re-snapshot, or an instance reloaded with the
    // room DONE, short-circuits. No DrivesInCombat: the ClearRadius steps are the
    // combat driver (the Stratholme Slaughterhouse shape).
    EventBuilder hall(MAP_ID, EVENT_DRAGONSPIRE_HALL, "Dragonspire Hall");
    hall.Anchored(/*orderIndex*/ 9)
        .Persistent()
        .CompleteWhenGOState(GO_EMBERSEER_IN, GO_STATE_OPEN, HALL_SCAN)
        .MoveTo(HALL_ANCHOR_X, HALL_ANCHOR_Y, HALL_ANCHOR_Z, /*radius*/ HALL_SCAN);
    for (HallRune const& r : HALL_RUNES)
        hall.ClearRadius(r.x, r.y, r.z, HALL_RUNE_CLEAR_RADIUS, HALL_RUNE_CLEAR_ZBAND)
            .OnlyEntries({ NPC_BLACKHAND_DREADWEAVER, NPC_BLACKHAND_SUMMONER, NPC_BLACKHAND_VETERAN })
            .Timeout(180000);
    // CHECK runs every 3s, the sweep's re-arm waits 8s and re-fires at most
    // every 20s; five minutes here is a hall that will not finish.
    hall.Custom(HOOK_HALL_RUNES_SWEEP).Timeout(300000);
    out.push_back(hall.Build());
}

// --- roster patch ----------------------------------------------------------
void RegisterBlackrockSpireRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;
    using namespace DcBlackrockSpire;

    // --- Upper Blackrock Spire (map 229) ---------------------------------
    // ONE patch for the whole map (FindPatch returns the first per map, the Dire
    // Maul precedent); the wing filter scopes each run, so LBRS sees none of it.
    // Each UBRS gate is an objective sharing the index of the boss it unlocks,
    // so the objective-before-boss tie-break walks the party to it first; two
    // objectives on one key keep insertion order (BRD's Lyceum South/North).
    //
    // Solakar Flamewreath (10264, bit 10) has no static spawn, so BossSpawnIndex
    // never lists him and there is no row to mark skipByDesign: he stays out of
    // the run by not being in it. The ubrs wing still names him, for credit.
    BossRosterPatch p;
    p.mapId = MAP_ID;
    p.add = {
        MakeObjective(OBJ(OBJ_DRAGONSPINE_DOOR), /*encounterIndex*/ 9, MAP_ID, "Dragonspine Door",
                      DOOR_STAND_X, DOOR_STAND_Y, DOOR_STAND_Z, /*arriveRadius*/ 6.0f,
                      /*gateEntry*/ 0, /*hook*/ 0, EVENT_DRAGONSPINE_DOOR),
        MakeObjective(OBJ(OBJ_DRAGONSPIRE_HALL), /*encounterIndex*/ 9, MAP_ID, "Dragonspire Hall",
                      HALL_ANCHOR_X, HALL_ANCHOR_Y, HALL_ANCHOR_Z, /*arriveRadius*/ 6.0f,
                      /*gateEntry*/ 0, /*hook*/ 0, EVENT_DRAGONSPIRE_HALL),
    };
    t.push_back(std::move(p));
}

// --- wing layout ---------------------------------------------------------
void RegisterBlackrockSpireWings(std::unordered_map<uint32, DungeonWingLayout>& store)
{
    // --- Blackrock Spire (map 229) -------------------------------
    // Both wings are entered through the SAME portal and are stacked: from the
    // portal (78.5, -225.0, 49.8) the nearest bosses are UBRS's (Emberseer 87yd,
    // Drakkisath 96yd; Omokk, LBRS's first, is 128yd). Nearest-boss detection
    // would put an LBRS party on the UBRS list at the door, and the last brs test
    // runs show the other half of the problem: clear LBRS, then walk back up to
    // the Dragonspine Door for Emberseer. So the wing is chosen per RUN
    // (WingSelect::Explicit, see DcRunWing), defaulting to LBRS.
    //
    // Encounter bits from DungeonEncounter.dbc, verified against acore_world
    // (blackrock_spire.h's DATA_* indices diverge from bit 10 on — use these):
    //   LBRS bits 0-8: Omokk, Vosh'gajin, Voone, Smolderweb, Urok (GO summon),
    //                  Zigris, Gizrul (spawns on Halycon's death), Halycon,
    //                  Wyrmthalak (lastEncounterDungeon = 32)
    //   UBRS bits 9-13: Emberseer, Solakar (Father Flame summon), Rend, The
    //                  Beast, Drakkisath (lastEncounterDungeon = 44)
    // Urok, Gizrul and Solakar have no static spawn, so BossSpawnIndex never
    // lists them; they are here so credit and counting still resolve.
    //
    // UBRS's scripted gates (the roster patch's objectives) are listed with it,
    // each just before the boss it unlocks.
    using DcRoster::OBJ;
    store[229] = {true, {
        {"Blackrock Spire (Lower)", {
            9196,   // Highlord Omokk
            9236,   // Shadow Hunter Vosh'gajin
            9237,   // War Master Voone
            10596,  // Mother Smolderweb
            10584,  // Urok Doomhowl
            9736,   // Quartermaster Zigris
            10268,  // Gizrul the Slavener
            10220,  // Halycon
            9568,   // Overlord Wyrmthalak
        }, "lbrs", /*lfgDungeonId*/ 32, /*terminalBossEntry*/ 9568, /*encounterMask*/ 0x01FFu},
        {"Blackrock Spire (Upper)", {
            OBJ(DcBlackrockSpire::OBJ_DRAGONSPINE_DOOR),  // Dragonspine Door (event 1)
            OBJ(DcBlackrockSpire::OBJ_DRAGONSPIRE_HALL),  // Dragonspire Hall runes (event 2)
            9816,   // Pyroguard Emberseer
            10264,  // Solakar Flamewreath
            10429,  // Warchief Rend Blackhand
            10430,  // The Beast
            10363,  // General Drakkisath
        }, "ubrs", /*lfgDungeonId*/ 44, /*terminalBossEntry*/ 10363, /*encounterMask*/ 0x3E00u},
    }, WingSelect::Explicit, /*defaultWing*/ "lbrs"};
}
