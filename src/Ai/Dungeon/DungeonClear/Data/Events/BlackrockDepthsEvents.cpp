/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonWingRegistry.h"

#include <cmath>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

#include "Bag.h"
#include "CharmInfo.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "GameObject.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Item.h"
#include "Log.h"
#include "LootMgr.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "WorldSession.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/BlackrockDepthsLyceum.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcStatusPublisher.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"

// --- Blackrock Depths (map 230) — the RING OF LAW, ANCHORED + PERSISTENT ---
// A sealed arena gauntlet that gates the way between Houndmaster Grebmar and
// Pyromancer Loregrain. A party member steps onto the centre (area trigger
// 1526), the entrance gate slams shut, Grimstone (10096) summons two random
// trash waves then one random boss, and on the boss's death the forward gate
// opens. Nothing the party fights is in the spawn tables — the waves and boss
// are all SUMMONED and randomised — so the event is purely position/state based.
//
// Navigation is done by the boss list: the Ring of Law is added as an OBJECTIVE
// anchor at the arena centre (BossRosterRegistry, encounterIndex 3, between
// Grebmar at 2 and Loregrain at 4). Boss-nav drives the tank into the arena
// exactly as to any boss; the event then runs in place.
//
// The arena is COME-TO-YOU: every summon AttackStart()s a random player within
// 100yd, so the party never pulls — it holds the centre and the combat engine
// fights reactively as the waves and boss arrive. We therefore do NOT use
// ClearRadius (a "done when the band is empty" gate would false-complete during
// the ~12s Grimstone intro and the ~10s wave-2->boss gap, both of which leave
// the floor momentarily empty while the arena is still sealed). It would also
// risk driving an outward engage UP to the hostile spectator gallery (z ~ -35)
// or DOWN toward Grebmar's prison (z ~ -84), which flank the floor (z ~ -54).
//
// Instead the completion gate is the instance state reaching DONE
// (TYPE_RING_OF_LAW == DONE): it can't be missed across a combat tick-gap and
// can't fire during the empty-floor windows. The tank garrisons dead-centre,
// re-centring between fights (the MoveTo re-checks distance every tick), until
// the whole gauntlet is done — at which point the objective latches and the
// clear proceeds out the now-open north gate toward Loregrain.
//
// TYPE_RING_OF_LAW IS NOT MONOTONIC, unlike the phase counters the instance-data
// gate is usually pointed at. npc_grimstone runs its own watchdog (updateReset):
// once a wave or the boss is out and 30s pass with no summon holding a victim, it
// SetData(FAIL)s — which despawns Grimstone and every summon, puts the gates back
// as they were, and drops the state to NOT_STARTED. Nothing in the core restarts
// it, and nothing on our side used to either: the Custom step that fired the
// areatrigger had latched Done, and the test-run areatrigger relay is EDGE-
// triggered on a volume the party is standing in. The party then held an empty
// arena for the whole 600s timeout. That is what tr-20260808-150405-10 did.
// .WhileHolding re-runs the start hook from inside the garrison, so a reset is
// re-fired (the core's own 2-minute post-fail cooldown decides when it takes).
//
// PERSISTENT because a multi-combat arena event sees a >1s Drive gap after each
// wave/boss fight (the bot is on the combat engine); a non-persistent event
// would rewind to step 0 each time.

namespace
{
    // Instance-data accessor + state (mirrors instance_blackrock_depths.cpp's
    // TYPE_RING_OF_LAW / EncounterState; kept local so this TU needn't pull the
    // core BRD header). GetData(TYPE_RING_OF_LAW) returns the live state.
    constexpr uint32 BRD_TYPE_RING_OF_LAW = 1;  // DataTypes::TYPE_RING_OF_LAW
    constexpr uint32 BRD_RING_DONE = 3;          // EncounterState::DONE

    // Arena centre = area trigger 1526 (x,y from AreaTrigger.dbc; z on the floor
    // where the waves/boss spawn, ~3.9yd below the 8yd trigger sphere's centre —
    // well inside it, so arriving here both crosses the trigger and lets the
    // EnsureRingStarted fallback fire it).
    constexpr float BRD_ARENA_X = 596.432f;
    constexpr float BRD_ARENA_Y = -188.498f;
    constexpr float BRD_ARENA_Z = -53.9f;

    // Garrison tolerance, used for BOTH the walk-in and the hold so the two steps
    // agree on where "on the spot" is. Tight on purpose: this is a 4yd leash, not
    // a dead band. It was 10yd on the hold, which is wide enough that the tank
    // simply kept wherever the last wave ended — tr-20260808-150405-10 finished
    // the waves at (599.0, -197.2), 9.7yd out toward the mob gate, and never
    // re-centred because 9.7 < 10. The arena is come-to-you and its script picks
    // targets and spawns the spoils chest around the centre; standing off it is
    // drift, not positioning.
    constexpr float BRD_ARENA_RADIUS = 4.0f;

    // EnsureRingStarted hook (ObjectiveHookRegistry id 1): fires the real area
    // trigger if arrival alone didn't start the encounter (all-bot party / no
    // human on the trigger).
    constexpr uint32 BRD_ENSURE_RING_STARTED_HOOK = 1;

    // --- Shadowforge Lock (event 2) ---------------------------------------
    // The lever in the East Garrison, and the Giant Doors it drives. GOState
    // values are spelled out locally so this TU needn't pull SharedDefines:
    // GO_STATE_ACTIVE = 0, GO_STATE_READY = 1.
    constexpr uint32 BRD_GO_SHADOWFORGE_LOCK = 161460;
    constexpr uint32 BRD_GO_GIANT_DOORS = 157923;
    constexpr uint32 BRD_GO_STATE_READY = 1;

    // The lever's own spawn point, dropped onto the garrison floor (the mmaps
    // surface under it is z -59.82; the GO sits at -60.06 on the wall). The
    // objective anchor delivers the tank here and the UseGO step's own 5yd
    // approach closes whatever is left.
    constexpr float BRD_LOCK_X = 615.61f;
    constexpr float BRD_LOCK_Y = -49.78f;
    constexpr float BRD_LOCK_Z = -59.82f;
    constexpr float BRD_LOCK_RADIUS = 4.0f;

    // Search radius for the Giant Doors from the lever. They are 113yd away at
    // (723.1,-105.9,-71.5) but in the SAME map grid as the lever (both fall in
    // grid 30/32), so the cell visit always has them loaded — this is a state
    // read, not a proximity gate.
    constexpr float BRD_GIANT_DOOR_SCAN = 200.0f;
    constexpr uint32 BRD_GIANT_DOOR_TIMEOUT_MS = 30000;

    using namespace DcBlackrockDepths;

    // --- the Grim Guzzler (events 3-5) -------------------------------------
    // Three of the bar's four bosses cannot be fought until a player does
    // something to them first, and a bot party does none of it. Runs -1..-5 of
    // tp-20260928-214704-2 all died in here: three flapped fighting_boss /
    // fighting_trash on an invisible Hurley for four minutes, two stood idle in
    // front of a friendly Phalanx for two and a half.

    // Search radii. The bar is one room ~80yd across; these reach its far side
    // from any of the event anchors without reaching Flamelash's hall.
    constexpr float BRD_BAR_SCAN = 80.0f;
    // The kegs stand within 4.3yd of KEG_X/Y; the event's MoveTo puts the tank
    // on that spot first.
    constexpr float BRD_KEG_SCAN = 10.0f;
    // All three kegs are needed (see WakeHurley); one per second keeps each
    // use its own SmartAI chain without the party standing at the kegs.
    constexpr uint32 BRD_KEG_FLOOR_MS = 1000;
    // Rocknot's hold: generous, the escort is ~40s of walking and kicking
    // (waypoint path 95030: a 4.7s and a 3.1s stop at the first two kegs, then
    // the 5.1s final-keg sequence).
    constexpr uint32 BRD_ROCKNOT_TIMEOUT_MS = 120000;
    // The Bar Door goes to GO_STATE_ACTIVE_ALTERNATIVE when Rocknot breaks the
    // last keg (npc_rocknot EVENT_ROCKNOT_BREAK_KEG). Phalanx is woken 7s LATER.
    constexpr uint32 BRD_GO_STATE_ACTIVE_ALTERNATIVE = 2;
    // Phalanx's wake-up: the 7s cork delay, then his run to the door, which his
    // own 15s EVENT_PHALANX_FINISH_MOVEMENT caps. A minute is plenty, and a
    // Phalanx who never turns is this step's timeout rather than a tank parked
    // on a friendly golem.
    constexpr uint32 BRD_PHALANX_WAKE_TIMEOUT_MS = 60000;
    // Interaction reach for Plugger and Rocknot. The core's vendor and quest
    // checks test INTERACTION_DISTANCE (5yd); stand a little inside it, because
    // Plugger never stops walking behind his counter (path 477030, no delays).
    constexpr float BRD_BAR_NPC_REACH = 4.0f;
    // One hand-in at a time, so each is its own drink and "Ahhh!".
    constexpr uint32 BRD_ALE_TURN_IN_FLOOR_MS = 1500;

    // --- WakeHurley (hook 52) ----------------------------------------------
    // Hurley Blackbreath (9537) and his three Blackbreath Cronies spawn
    // INVISIBLE, passive and immune to players (SmartAI 9537 rows 0-2). What
    // brings them in is the THIRD Thunderbrew Lager Keg (164911) used: each
    // keg's state change sends data 1 1 to the other kegs, which advances their
    // phase (164911 rows 0 and 2), and only a keg already in phase 2 — two used
    // before it — sends data 1 1 to Hurley (row 1). He then turns visible,
    // yells, and walks path 9537 to the bar floor, where he drops his immune
    // flags (rows 8-10). Engaging him then pulls all four (rows 16-18).
    //
    // A WaitForSpawn gate on him would pass on the first tick — he EXISTS from
    // spawn, he just can't be seen — so this hook is both the click and the
    // gate, keyed on what the script actually changes:
    //   * dead / gone                -> Done (nothing to wake)
    //   * visible, no longer immune  -> Done (on the floor; the roster's Hurley
    //                                   row engages him)
    //   * visible, still immune      -> Running (walking his path)
    //   * invisible                  -> use the next ready keg, then Running
    // A used keg is consumed (43200s respawn) and skipped by the GO_READY test,
    // so each tick reaches for the next one; Hurley's visibility ends it.
    ObjectiveArriveResult WakeHurley(Player* bot, AiObjectContext* context,
                                     DungeonBossInfo const& /*info*/)
    {
        Creature* hurley = bot->FindNearestCreature(NPC_HURLEY, BRD_BAR_SCAN, /*alive*/ true);
        if (!hurley)
            return ObjectiveArriveResult::Done;
        if (hurley->IsVisible())
            return hurley->IsImmuneToPC() ? ObjectiveArriveResult::Running
                                          : ObjectiveArriveResult::Done;

        if (DcRun::Of(context).Throttled(DcThrottle::BrdKegUse, BRD_KEG_FLOOR_MS))
            return ObjectiveArriveResult::Running;

        std::list<GameObject*> kegs;
        bot->GetGameObjectListWithEntryInGrid(kegs, GO_THUNDERBREW_KEG, BRD_KEG_SCAN);
        for (GameObject* keg : kegs)
        {
            if (!keg->isSpawned() || keg->getLootState() != GO_READY)
                continue;
            keg->Use(bot);
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] Blackrock Depths — used Thunderbrew Lager Keg {} to bring "
                     "Hurley Blackbreath in (he spawns invisible until a keg is used)",
                     bot->GetName(), keg->GetGUID().ToString());
            return ObjectiveArriveResult::Running;
        }
        return ObjectiveArriveResult::Running;  // none in reach yet / all spent: the timeout surfaces it
    }

    // --- the Grim Guzzler's ale (hooks 54 and 47) --------------------------
    // The Bar Door (170571) and Phalanx (9502, faction 35 — unattackable) are
    // both released by ONE script: npc_rocknot. Each turn-in of "Rocknot's Ale"
    // (quest 4295: two Dark Iron Ale Mugs, repeatable) bumps the instance's
    // BarAleCount, and on the third Rocknot walks path 95030 kicking kegs. The
    // last kick breaks the Keg Shotgun and the Bar Door goes to state 2; 7s later
    // the Keg Trap fires, TYPE_BAR goes DONE and Phalanx is told to start: he RUNS
    // from his corner to the door (path 95020, still faction 35 the whole way)
    // and only turns hostile (faction 54) when he gets there.
    //
    // The player's part is what these two hooks do, for real: buy six mugs from
    // Plugger Spazzring (hook 54), then hand the quest to Rocknot three times
    // (hook 47) through the same calls the quest opcodes make. Nothing here
    // writes the bar's state or touches Rocknot's walk.
    //
    // If the player's route is closed — Plugger dead, in combat or out of mugs,
    // no gold, a full quest log — hook 47 drops to entering npc_rocknot's
    // sQuestReward directly, which runs the identical walk, door and Phalanx. It
    // says so at WARN; the bar has to open either way, the door is the only way
    // on.

    // Walk the leader to (x,y,z) unless a hop is already under way.
    void HopToward(Player* bot, float x, float y, float z)
    {
        if (bot->isMoving())
            return;
        bot->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                          /*generatePath*/ true, false);
    }

    // How close to the counter-end stand point counts as standing on it.
    constexpr float BRD_PLUGGER_STAND_RADIUS = 1.5f;

    uint32 AleMugsNeeded() { return ALE_TURN_INS * ALE_MUGS_PER_TURN_IN; }

    // --- BuyRocknotAle (hook 54) -------------------------------------------
    // Done once the leader carries six mugs, once the run is already under way,
    // or once a purchase was attempted (success or not: hook 47 decides what a
    // shortfall means).
    ObjectiveArriveResult BuyRocknotAle(Player* bot, AiObjectContext* context,
                                        DungeonBossInfo const& /*info*/)
    {
        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Running;
        uint32 const bar = inst->GetData(TYPE_BAR);
        if (bar == STATE_DONE || bar == STATE_SPECIAL)
            return ObjectiveArriveResult::Done;

        uint32 const have = bot->GetItemCount(ITEM_DARK_IRON_ALE_MUG);
        if (have >= AleMugsNeeded())
            return ObjectiveArriveResult::Done;

        Creature* plugger = bot->FindNearestCreature(NPC_PLUGGER, BRD_BAR_SCAN, /*alive*/ true);
        if (!plugger || plugger->IsInCombat() || !plugger->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
        {
            LOG_WARN("playerbots.dungeonclear",
                     "[DC:{}] Blackrock Depths — Plugger Spazzring is {}; no Dark Iron Ale to buy",
                     bot->GetName(), !plugger ? "gone" : "not trading");
            return ObjectiveArriveResult::Done;
        }

        // Wait at the counter's end for his patrol to bring him past (see
        // PLUGGER_STAND_*): chasing him puts the leader across the counter from a
        // bartender who is out of reach for most of his loop.
        if (!bot->IsWithinDistInMap(plugger, BRD_BAR_NPC_REACH))
        {
            if (bot->GetExactDist2d(PLUGGER_STAND_X, PLUGGER_STAND_Y) > BRD_PLUGGER_STAND_RADIUS)
                HopToward(bot, PLUGGER_STAND_X, PLUGGER_STAND_Y, PLUGGER_STAND_Z);
            if (!DcRun::Of(context).Throttled(DcThrottle::BrdEventLog, 10000))
                LOG_INFO("playerbots.dungeonclear",
                         "[DC:{}] Grim Guzzler — at the bar for Dark Iron Ale; Plugger Spazzring is "
                         "{:.1f}yd away", bot->GetName(), bot->GetExactDist(plugger));
            return ObjectiveArriveResult::Running;
        }

        VendorItemData const* stock = plugger->GetVendorItems();
        uint32 slot = 0;
        bool found = false;
        for (uint32 i = 0; stock && i < stock->GetItemCount(); ++i)
            if (VendorItem const* vi = stock->GetItem(i); vi && vi->item == ITEM_DARK_IRON_ALE_MUG)
            {
                slot = i;
                found = true;
                break;
            }
        if (!found)
        {
            LOG_WARN("playerbots.dungeonclear",
                     "[DC:{}] Blackrock Depths — Plugger Spazzring does not sell item {}",
                     bot->GetName(), ITEM_DARK_IRON_ALE_MUG);
            return ObjectiveArriveResult::Done;
        }

        // BuyItemFromVendorSlot reads the session's open vendor list first; a bot
        // that last traded elsewhere would index someone else's stock.
        bot->GetSession()->SetCurrentVendor(plugger->GetEntry());
        uint32 const want = AleMugsNeeded() - have;
        bool const bought = bot->BuyItemFromVendorSlot(plugger->GetGUID(), slot, ITEM_DARK_IRON_ALE_MUG,
                                                       static_cast<uint8>(want), NULL_BAG, NULL_SLOT);
        bot->GetSession()->SetCurrentVendor(0);
        if (bought)
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] Grim Guzzler — bought {} Dark Iron Ale Mug(s) from Plugger Spazzring "
                     "(now carrying {})", bot->GetName(), want, bot->GetItemCount(ITEM_DARK_IRON_ALE_MUG));
        else
            LOG_WARN("playerbots.dungeonclear",
                     "[DC:{}] Blackrock Depths — could not buy {} Dark Iron Ale Mug(s) from Plugger "
                     "(stock, gold or bag space)", bot->GetName(), want);
        return ObjectiveArriveResult::Done;
    }

    // --- StartRocknotAleRun (hook 47) --------------------------------------
    // Hand "Rocknot's Ale" in, one turn-in per BRD_ALE_TURN_IN_FLOOR_MS, until
    // the bar reads SPECIAL (Rocknot is walking). Each turn-in is the quest
    // opcodes' own sequence: accept, complete (the two mugs are taken), then the
    // creature's sQuestReward, which is where npc_rocknot counts it.
    //
    // Idempotent: SPECIAL or DONE is Done at the top, so a rewind never starts a
    // second escort, and a count a human already began is finished, not
    // restarted (GetData reads IN_PROGRESS until the third).
    //
    // No forced end state. If the escort stalls TYPE_BAR stays SPECIAL, this is
    // a no-op, and the door wait after it times out visibly.
    ObjectiveArriveResult StartRocknotAleRun(Player* bot, AiObjectContext* context,
                                             DungeonBossInfo const& /*info*/)
    {
        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Running;

        uint32 const bar = inst->GetData(TYPE_BAR);
        if (bar == STATE_DONE || bar == STATE_SPECIAL)
            return ObjectiveArriveResult::Done;

        Creature* rocknot = bot->FindNearestCreature(NPC_ROCKNOT, BRD_BAR_SCAN, /*alive*/ true);
        if (!rocknot || !rocknot->AI())
            return ObjectiveArriveResult::Running;

        Quest const* ale = sObjectMgr->GetQuestTemplate(QUEST_ALE);
        if (!ale)
        {
            LOG_ERROR("playerbots.dungeonclear",
                      "[DC:{}] Blackrock Depths — quest {} missing from the world DB; "
                      "Rocknot's ale run cannot be started", bot->GetName(), QUEST_ALE);
            return ObjectiveArriveResult::Blocked;
        }

        if (!bot->IsWithinDistInMap(rocknot, BRD_BAR_NPC_REACH))
        {
            HopToward(bot, rocknot->GetPositionX(), rocknot->GetPositionY(), rocknot->GetPositionZ());
            return ObjectiveArriveResult::Running;
        }

        if (DcRun::Of(context).Throttled(DcThrottle::BrdAleTurnIn, BRD_ALE_TURN_IN_FLOOR_MS))
            return ObjectiveArriveResult::Running;

        bot->StopMoving();
        bot->SetFacingToObject(rocknot);

        bool handedIn = false;
        if (bot->GetItemCount(ITEM_DARK_IRON_ALE_MUG) >= ALE_MUGS_PER_TURN_IN)
        {
            if (bot->GetQuestStatus(QUEST_ALE) == QUEST_STATUS_NONE && bot->CanTakeQuest(ale, false) &&
                bot->CanAddQuest(ale, false))
                bot->AddQuestAndCheckCompletion(ale, rocknot);
            if (bot->CanRewardQuest(ale, 0, false))
            {
                bot->RewardQuest(ale, 0, rocknot);
                if (!sScriptMgr->OnQuestReward(bot, rocknot, ale, 0))
                    rocknot->AI()->sQuestReward(bot, ale, 0);
                handedIn = true;
            }
        }

        if (handedIn)
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] Grim Guzzler — handed Rocknot's Ale (quest {}) to Private Rocknot; "
                     "{} mug(s) left", bot->GetName(), QUEST_ALE, bot->GetItemCount(ITEM_DARK_IRON_ALE_MUG));
        else
        {
            // The player's route is closed (see the header): count this hand-in
            // the script's own way so the bar still opens.
            if (bot->GetQuestStatus(QUEST_ALE) == QUEST_STATUS_INCOMPLETE)
                bot->RemoveActiveQuest(QUEST_ALE);
            LOG_WARN("playerbots.dungeonclear",
                     "[DC:{}] Blackrock Depths — no Dark Iron Ale to hand in ({} mug(s), quest status {}); "
                     "counting the turn-in through npc_rocknot::sQuestReward", bot->GetName(),
                     bot->GetItemCount(ITEM_DARK_IRON_ALE_MUG), static_cast<uint32>(bot->GetQuestStatus(QUEST_ALE)));
            rocknot->AI()->sQuestReward(bot, ale, 0);
        }

        uint32 const now = inst->GetData(TYPE_BAR);
        if (now == STATE_SPECIAL)
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] Blackrock Depths — Private Rocknot has his three ales and is off to the "
                     "kegs; the Bar Door and Phalanx follow when he breaks the last one", bot->GetName());
        return (now == STATE_SPECIAL || now == STATE_DONE) ? ObjectiveArriveResult::Done
                                                           : ObjectiveArriveResult::Running;
    }

    // --- PhalanxAtDoor (hook 55) -------------------------------------------
    // The door breaking is NOT the moment Phalanx can be fought: he is woken 7s
    // later and spends up to 15s running from his corner to the door, faction 35
    // throughout (npc_phalanx StartActivation -> Activate on PathEndReached).
    // Latching the objective on the door sent the roster's Phalanx row after a
    // friendly golem still dormant in his corner, the tank pursuing him across
    // the doorway while he walked past it. Done once he can be fought — hostile
    // to the leader — or is already dead.
    ObjectiveArriveResult PhalanxAtDoor(Player* bot, AiObjectContext* context,
                                        DungeonBossInfo const& /*info*/)
    {
        Creature* phalanx = bot->FindNearestCreature(NPC_PHALANX, BRD_BAR_SCAN, /*alive*/ true);
        if (!phalanx || phalanx->IsHostileTo(bot))
            return ObjectiveArriveResult::Done;
        if (!DcRun::Of(context).Throttled(DcThrottle::BrdEventLog, 10000))
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] Grim Guzzler — Bar Door is broken; waiting for Phalanx to take the "
                     "doorway ({:.1f}yd from it)", bot->GetName(), phalanx->GetExactDist2d(PHALANX_DOOR_X, PHALANX_DOOR_Y));
        return ObjectiveArriveResult::Running;
    }

    // --- the Tomb of the Seven (event 6) -----------------------------------
    // Talking to Doom'rel (9039; menu 1947 opt 0 -> 1950 opt 0) is the start:
    // boss_doomrel::sGossipSelect sets TYPE_TOMB_OF_SEVEN to IN_PROGRESS, and
    // that CLOSES BOTH TOMB DOORS (instance SetData). Anyone outside stays
    // outside for the whole fight. The instance then wakes one ghost 1s later and
    // one every 30s after, each attacking the nearest player within 130yd, until
    // seven kills set DONE and open the doors again.
    //
    // The reset is the danger: if every woken ghost still alive spends 15s out
    // of combat, TombOfSevenReset respawns the dead ones, makes them all friendly,
    // reopens the entrance and drops the state to NOT_STARTED. A garrison on a
    // `>= DONE` gate would then hold an empty tomb until it timed out.

    // --- TombMuster (hook 51) ----------------------------------------------
    // Running until every living party member stands within TOMB_MUSTER_RADIUS of
    // the garrison, on the tomb's floors — i.e. INSIDE when the gossip seals the
    // doors. A member that cannot get in is still bounded: the step's timeout
    // stalls the run visibly, and stranded recovery pulls a stuck bot to the tank
    // well inside it. Done straight away once the encounter is running or over,
    // so a rewind into a live tomb does not wait on a party that is fighting.
    ObjectiveArriveResult TombMuster(Player* bot, AiObjectContext* context,
                                     DungeonBossInfo const& /*info*/)
    {
        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Running;
        uint32 const state = inst->GetData(TYPE_TOMB_OF_SEVEN);
        if (state == STATE_IN_PROGRESS || state == STATE_DONE)
            return ObjectiveArriveResult::Done;

        Group* group = bot->GetGroup();
        if (!group)
            return ObjectiveArriveResult::Done;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || !member->IsInWorld() || !member->IsAlive() ||
                member->GetMapId() != bot->GetMapId())
                continue;
            bool const inside =
                member->GetExactDist2d(TOMB_HOLD_X, TOMB_HOLD_Y) <= TOMB_MUSTER_RADIUS &&
                std::fabs(member->GetPositionZ() - TOMB_HOLD_Z) <= TOMB_MUSTER_ZBAND;
            if (inside)
                continue;
            if (!DcRun::Of(context).Throttled(DcThrottle::BrdEventLog, 10000))
                LOG_INFO("playerbots.dungeonclear",
                         "[DC:{}] Tomb of the Seven — mustering before Doom'rel seals the "
                         "doors: {} is {:.1f}yd out", bot->GetName(), member->GetName(),
                         member->GetExactDist2d(TOMB_HOLD_X, TOMB_HOLD_Y));
            return ObjectiveArriveResult::Running;
        }
        return ObjectiveArriveResult::Done;
    }

    // --- TombResetWatch (hook 53) ------------------------------------------
    // Runs every tick of the garrison (.WhileHolding). NOT_STARTED there means
    // the tomb reset under the party — or the gossip never took — so ask for a
    // Restart: the event rewinds, re-musters, and talks to Doom'rel again (he
    // gets his gossip back in his Reset()). The Ring of Law self-restart, keyed
    // on instance data instead of an areatrigger. Anything else is Done, and is
    // ignored: the garrison's own DONE gate ends the hold.
    ObjectiveArriveResult TombResetWatch(Player* bot, AiObjectContext* /*context*/,
                                         DungeonBossInfo const& /*info*/)
    {
        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst || inst->GetData(TYPE_TOMB_OF_SEVEN) != STATE_NOT_STARTED)
            return ObjectiveArriveResult::Done;
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] Tomb of the Seven reads NOT_STARTED under the garrison (the "
                 "15s out-of-combat reset, or the challenge never started) — restarting "
                 "the challenge", bot->GetName());
        return ObjectiveArriveResult::Restart;
    }

    // --- the Lyceum (events 7 and 8) ---------------------------------------
    // Two braziers, S (174744) and N (174745), lock 799: the Shadowforge Torch.
    // go_shadowforge_brazier sets TYPE_LYCEUM IN_PROGRESS on the first and DONE
    // when the second is lit while the first still burns, which opens both Golem
    // doors to Magmus. Torches (11885: one per Flame Keeper, 100%; 300s in the
    // bags; spent on use) come only from the Shadowforge Flame Keepers of pool
    // 1550 — two alive at a time over four points, 300s respawn — in a room of
    // 232 Anvilrage Reservists that each come back a minute after they die.
    //
    // GameObject::Use() has no lock check, so a server-side click would light a
    // brazier with no torch and erase the whole challenge. The hooks below play
    // it straight instead: kill keepers, LOOT their torches, and SPEND one per
    // brazier. Nothing is granted, and no respawn is forced.

    // TORCHES ARE LOST TO THE CORPSE CLOCK, not to the hunt. Flame Keepers are
    // rank 0, so an unlooted corpse decays in Corpse.Decay.NORMAL = 60s, and the
    // hunt (a KillCreature gate: Running while ANY keeper is alive in 160yd)
    // already kills both live keepers. What used to lose the second torch was
    // when and where it was looked for: only out of combat — which, among 232
    // Reservists on a 60s respawn, can be a minute after the kill — and only
    // within 40yd of wherever the fights had left the tank. All six logged runs
    // of 2026-10-18 banked ONE torch from the south hunt and then waited 2-5
    // minutes at the north brazier for pool 1550 to respawn a keeper (two runs
    // waited twice: keepers killed, corpses gone before anyone stood at them).
    //
    // So a corpse is now taken two ways. In combat, DcLyceum::Tick loots any
    // torch corpse a member is already standing at (the melee who killed it).
    // Out of combat, the torch hook WALKS to one anywhere in the room for as
    // long as the party holds fewer torches than there are braziers left to
    // light: a walk across the room is twenty seconds, and the wait it replaces
    // is three hundred.
    //
    // Loot reach: the core's interaction distance.
    constexpr float BRD_TORCH_LOOT_REACH = 5.0f;
    // A torch holder this close to a brazier lights it. The tank stands on the
    // brazier's stand point (within 4yd, itself ~6yd in front of the brazier),
    // and a follower trails it by up to ~6yd.
    constexpr float BRD_BRAZIER_REACH = 10.0f;
    constexpr uint32 BRD_TORCH_NUDGE_MS = 2000;
    // How long the brazier waits for the holder of the torch that expires first
    // before it settles for a fresher one already in reach. The holder trails the
    // tank by a few yards at most; past this it is not coming.
    constexpr uint32 BRD_OLDEST_TORCH_WAIT_MS = 8000;

    bool LyceumDone(Player* bot)
    {
        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        return inst && inst->GetData(TYPE_LYCEUM) == STATE_DONE;
    }

    // Living party members on the bot's map (the bot itself included).
    template <typename Fn>
    void ForEachLivingMember(Player* bot, Fn&& fn)
    {
        Group* group = bot->GetGroup();
        if (!group)
        {
            fn(bot);
            return;
        }
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member->IsInWorld() && member->IsAlive() &&
                member->GetMapId() == bot->GetMapId())
                fn(member);
        }
    }

    uint32 PartyTorchCount(Player* bot)
    {
        uint32 n = 0;
        ForEachLivingMember(bot, [&](Player* m) { n += m->GetItemCount(ITEM_SHADOWFORGE_TORCH); });
        return n;
    }

    // Every Shadowforge Torch a living member carries, with the time left on its
    // 300s bag clock.
    struct HeldTorch
    {
        Player* holder = nullptr;
        Item* item = nullptr;
        uint32 leftMs = 0;
    };

    template <typename Fn>
    void ForEachTorchIn(Player* p, Fn&& fn)
    {
        auto visit = [&](Item* item)
        {
            if (item && item->GetEntry() == ITEM_SHADOWFORGE_TORCH)
                fn(item);
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            visit(p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* b = p->GetBagByPos(bag))
                for (uint32 slot = 0; slot < b->GetBagSize(); ++slot)
                    visit(b->GetItemByPos(static_cast<uint8>(slot)));
    }

    std::vector<HeldTorch> PartyTorches(Player* bot)
    {
        std::vector<HeldTorch> out;
        ForEachLivingMember(bot, [&](Player* m)
        {
            ForEachTorchIn(m, [&](Item* item)
            {
                out.push_back({ m, item, item->GetUInt32Value(ITEM_FIELD_DURATION) * IN_MILLISECONDS });
            });
        });
        return out;
    }

    // The slot of an untaken torch in a keeper corpse's loot, or -1.
    int32 TorchSlot(Creature* corpse)
    {
        Loot const& loot = corpse->loot;
        for (uint8 slot = 0; slot < loot.items.size(); ++slot)
            if (loot.items[slot].itemid == ITEM_SHADOWFORGE_TORCH && !loot.items[slot].is_looted)
                return slot;
        return -1;
    }

    // Torches the party still has a use for: one per unlit brazier. The first
    // lit brazier moves TYPE_LYCEUM to IN_PROGRESS, the second to DONE.
    uint32 TorchesStillNeeded(Player* bot)
    {
        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return 1;
        switch (inst->GetData(TYPE_LYCEUM))
        {
            case STATE_DONE:        return 0;
            case STATE_IN_PROGRESS: return 1;
            default:                return 2;
        }
    }

    // The nearest keeper corpse in `scan` of the bot that still holds its torch.
    Creature* NearestTorchCorpse(Player* bot, float scan)
    {
        std::list<Creature*> keepers;
        bot->GetCreatureListWithEntryInGrid(keepers, NPC_FLAME_KEEPER, scan);
        Creature* corpse = nullptr;
        for (Creature* k : keepers)
            if (!k->IsAlive() && TorchSlot(k) >= 0 &&
                (!corpse || bot->GetExactDist(k) < bot->GetExactDist(corpse)))
                corpse = k;
        return corpse;
    }

    // A living member standing at `corpse` (the bot itself preferred), or null.
    Player* MemberAtCorpse(Player* bot, Creature* corpse)
    {
        Player* looter = nullptr;
        ForEachLivingMember(bot, [&](Player* m)
        {
            if (m->IsWithinDistInMap(corpse, BRD_TORCH_LOOT_REACH) && (!looter || m == bot))
                looter = m;
        });
        return looter;
    }

    // The real loot: the corpse's own generated loot, taken with StoreLootItem
    // by a member standing at it (the Hakkari Blood precedent).
    void LootTorch(Player* bot, Player* looter, Creature* corpse, char const* when)
    {
        InventoryResult msg = EQUIP_ERR_OK;
        looter->StoreLootItem(static_cast<uint8>(TorchSlot(corpse)), &corpse->loot, msg);
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] Lyceum — {} looted a Shadowforge Torch from Flame Keeper {} {} "
                 "(result {}); party now holds {} of {} needed",
                 bot->GetName(), looter->GetName(), corpse->GetGUID().ToString(), when,
                 static_cast<uint32>(msg), PartyTorchCount(bot), TorchesStillNeeded(bot));
    }

    // --- GatherShadowforgeTorch (hook 48) ----------------------------------
    // Step 2 of each brazier event, right after the keeper hunt (step 1, a
    // KillCreatureEngage over LYCEUM_KEEPER_SCAN). Decides, every tick:
    //   1. the party holds fewer torches than braziers left unlit, and a keeper
    //      corpse anywhere in the room still holds one -> walk to it and loot it.
    //      Ordinary bot looting and the in-combat sweep (DcLyceum::Tick) usually
    //      get there first, which is fine — the count sees the torch wherever it
    //      landed; this is what keeps a corpse nobody stopped at from decaying
    //      with the north brazier's torch in it.
    //   2. the party holds a torch -> Done; go and light the brazier. One is
    //      enough: braziers are lit as soon as a torch is in hand, because a
    //      torch carried around waiting for its pair is a torch expiring.
    //   3. no torch, but a keeper is alive -> Restart: rewind to the hunt (step
    //      0, the sticky-arming MoveTo, is Done on the spot anywhere in the room).
    //   4. no torch, no keeper -> the pool is respawning. Hold where the hunt
    //      left the party and keep fighting; the step's TORCH_WAIT_MS timeout
    //      stalls it visibly if no keeper ever comes back.
    ObjectiveArriveResult GatherShadowforgeTorch(Player* bot, AiObjectContext* context,
                                                 DungeonBossInfo const& /*info*/)
    {
        if (LyceumDone(bot))
            return ObjectiveArriveResult::Done;

        Creature* corpse = PartyTorchCount(bot) < TorchesStillNeeded(bot)
                               ? NearestTorchCorpse(bot, LYCEUM_KEEPER_SCAN)
                               : nullptr;
        if (corpse)
        {
            Player* looter = MemberAtCorpse(bot, corpse);
            if (!looter)
            {
                if (!bot->isMoving())
                    bot->GetMotionMaster()->MovePoint(0, corpse->GetPositionX(), corpse->GetPositionY(),
                                                      corpse->GetPositionZ(), FORCED_MOVEMENT_NONE,
                                                      0.0f, 0.0f, /*generatePath*/ true, false);
                return ObjectiveArriveResult::Running;
            }
            LootTorch(bot, looter, corpse, "between fights");
            return ObjectiveArriveResult::Running;  // re-decide with the new count
        }

        if (PartyTorchCount(bot) >= 1)
            return ObjectiveArriveResult::Done;

        if (bot->FindNearestCreature(NPC_FLAME_KEEPER, LYCEUM_KEEPER_SCAN, /*alive*/ true))
        {
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] Lyceum — no Shadowforge Torch in the party and a Flame Keeper "
                     "is up: back to the hunt", bot->GetName());
            return ObjectiveArriveResult::Restart;
        }

        // Waiting on pool 1550. Say so every 10s, and once — loudly — when the
        // wait has outlived a respawn, which is the step's timeout firing next.
        DcRunState& run = DcRun::Of(context);
        DungeonEventProgress const& prog =
            context->GetValue<DungeonEventProgress&>(DcKey::EventProgress)->Get();
        if (getMSTimeDiff(prog.stepStartMs, getMSTime()) + 1000 >= TORCH_WAIT_MS &&
            !run.Throttled(DcThrottle::BrdTorchWarn, TORCH_WAIT_MS))
            LOG_WARN("playerbots.dungeonclear",
                     "[DC:{}] LYCEUM no Flame Keeper respawned in {}s (pool 1550, 300s respawn) — "
                     "if this repeats, the pooled-respawn fix (AC PR #25826) is not in this core",
                     bot->GetName(), TORCH_WAIT_MS / 1000);
        else if (!run.Throttled(DcThrottle::BrdEventLog, 10000))
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] Lyceum — out of torches and no Flame Keeper alive; waiting for "
                     "pool 1550 to respawn one", bot->GetName());
        return ObjectiveArriveResult::Running;
    }

    // --- LightBrazier (hooks 49 S / 50 N) ----------------------------------
    // The last step of each brazier event. Spend ONE torch from a holder at the
    // brazier, then GameObject::Use() it as that holder: the same Use() a
    // player's click reaches, so go_shadowforge_brazier still runs and still
    // sees which brazier was first. The torch is destroyed by hand because
    // Use() skips lock 799 and would otherwise light it for free.
    //   * lit already (by us, a human, or a rewind) -> Done, no torch spent
    //   * a holder in reach -> spend + light
    //   * a holder elsewhere in the party -> walk them over (throttled)
    //   * no torch left anywhere (expired, or the holder died and released) ->
    //     Restart: back to the hunt
    ObjectiveArriveResult LightBrazier(Player* bot, AiObjectContext* context, uint32 brazierEntry)
    {
        if (LyceumDone(bot))
            return ObjectiveArriveResult::Done;

        GameObject* brazier = bot->FindNearestGameObject(brazierEntry, LYCEUM_ROOM_SCAN);
        if (!brazier)
            return ObjectiveArriveResult::Running;
        if (brazier->GetGoState() != GO_STATE_READY)
            return ObjectiveArriveResult::Done;
        if (brazier->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE))
            return ObjectiveArriveResult::Running;  // Use() would swallow it — and the torch

        // SPEND THE TORCH THAT DIES FIRST. Each one runs its own 300s clock, so
        // which torch lights the south brazier decides whether the other survives
        // to the north one. tr-20261018-211242-1 held a 3m30s-old torch and a
        // fresh one, the tank lit the south brazier with its own fresh one, and
        // the old torch expired on the walk north: back to the hunt, seven minutes
        // on the north brazier. With a torch still to keep, the oldest one is
        // spent, its holder walked in for up to BRD_OLDEST_TORCH_WAIT_MS; with
        // none to keep (the last brazier), the oldest one already in reach.
        std::vector<HeldTorch> const torches = PartyTorches(bot);
        HeldTorch const* oldest = nullptr;
        HeldTorch const* oldestInReach = nullptr;
        for (HeldTorch const& t : torches)
        {
            if (!oldest || t.leftMs < oldest->leftMs)
                oldest = &t;
            if (t.holder->IsWithinDistInMap(brazier, BRD_BRAZIER_REACH) &&
                (!oldestInReach || t.leftMs < oldestInReach->leftMs))
                oldestInReach = &t;
        }

        HeldTorch const* spend = nullptr;
        if (oldest && oldest->holder->IsWithinDistInMap(brazier, BRD_BRAZIER_REACH))
            spend = oldest;
        else if (oldestInReach)
        {
            DungeonEventProgress const& prog =
                context->GetValue<DungeonEventProgress&>(DcKey::EventProgress)->Get();
            if (TorchesStillNeeded(bot) <= 1 ||
                getMSTimeDiff(prog.stepStartMs, getMSTime()) >= BRD_OLDEST_TORCH_WAIT_MS)
                spend = oldestInReach;
        }

        if (!spend)
        {
            if (!oldest)
            {
                LOG_INFO("playerbots.dungeonclear",
                         "[DC:{}] Lyceum — at the {} brazier with no Shadowforge Torch left "
                         "(expired or lost): back to the hunt", bot->GetName(),
                         brazierEntry == GO_BRAZIER_S ? "south" : "north");
                return ObjectiveArriveResult::Restart;
            }
            if (!DcRun::Of(context).Throttled(DcThrottle::BrdTorchNudge, BRD_TORCH_NUDGE_MS))
                oldest->holder->GetMotionMaster()->MovePoint(0, brazier->GetPositionX(),
                                                             brazier->GetPositionY(), brazier->GetPositionZ(),
                                                             FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                                             /*generatePath*/ true, false);
            return ObjectiveArriveResult::Running;
        }

        Player* holder = spend->holder;
        uint32 const leftS = spend->leftMs / IN_MILLISECONDS;
        uint32 one = 1;
        holder->DestroyItemCount(spend->item, one, /*update*/ true);
        brazier->Use(holder);
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] Lyceum — {} spent a Shadowforge Torch ({}s left of 300) on the {} brazier: "
                 "state {} (TYPE_LYCEUM now {})", bot->GetName(), holder->GetName(), leftS,
                 brazierEntry == GO_BRAZIER_S ? "south" : "north",
                 static_cast<uint32>(brazier->GetGoState()),
                 DcTargeting::GetInstanceScript(bot)
                     ? DcTargeting::GetInstanceScript(bot)->GetData(TYPE_LYCEUM) : 0u);
        return ObjectiveArriveResult::Running;  // the lit-state check above latches it
    }
}

namespace DcLyceum
{
    namespace
    {
        using namespace DcBlackrockDepths;

        constexpr uint32 LYCEUM_TICK_MS = 500;
        uint32 g_lyceumAccumMs = 0;

        // Owner -> the stance its pet had before the room took it. Only pets
        // this sweep changed are here, so a stance a player set themselves is
        // never "restored" over.
        std::map<ObjectGuid, ReactStates> g_lyceumPets;

        bool InLyceum(WorldObject const* o)
        {
            return o && o->GetMapId() == MAP_ID &&
                   o->GetPositionX() >= LYCEUM_BOX_X_MIN && o->GetPositionX() <= LYCEUM_BOX_X_MAX &&
                   o->GetPositionY() >= LYCEUM_BOX_Y_MIN && o->GetPositionY() <= LYCEUM_BOX_Y_MAX &&
                   o->GetPositionZ() >= LYCEUM_BOX_Z_MIN && o->GetPositionZ() <= LYCEUM_BOX_Z_MAX;
        }

        // Emperor Dagran Thaurissan's throne room while it is pre-cleared and he
        // sleeps. Pets are held there too: the chase ban around his sphere
        // (DungeonClearMultiplier, ChaseWakesSleepingRoomBoss) rules on bot
        // actions, and a pet is no bot. sk-20261019-105358: four of fifteen fatal
        // wakes were a hunter's or warlock's pet attacked by him at 19.8-21.4yd,
        // no player inside 27yd. Released the moment he is in combat, so the pets
        // fight the boss.
        constexpr uint32 NPC_EMPEROR_DAGRAN = 9019;

        bool ThroneRoomPreClear(Player* tank, PlayerbotAI* tankAI)
        {
            AiObjectContext* ctx = tankAI->GetAiObjectContext();
            std::optional<DungeonBossInfo> const next =
                ctx->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss)->Get();
            if (!next.has_value() || next->entry != NPC_EMPEROR_DAGRAN ||
                !DcTargeting::IsRoomClearActive(tank, ctx))
                return false;
            Creature* const boss = DcTargeting::GetLiveBoss(tank, ctx, next->entry);
            return boss && !boss->IsInCombat();
        }

        // Hold one bot's pet: passive, and back on follow if it is fighting or
        // has been told to do anything but follow. `room` names the hold in the log.
        void HoldPet(Player* owner, PlayerbotAI* ownerAI, char const* room)
        {
            Pet* pet = owner->GetPet();
            if (!pet || !pet->IsAlive())
                return;

            if (pet->GetReactState() != REACT_PASSIVE)
            {
                g_lyceumPets.emplace(owner->GetGUID(), pet->GetReactState());
                pet->SetReactState(REACT_PASSIVE);
                LOG_INFO("playerbots.dungeonclear",
                         "[DC:{}] {} — {}'s pet {} held passive on follow for the room",
                         owner->GetName(), room, owner->GetName(), pet->GetName());
            }
            if (!g_lyceumPets.count(owner->GetGUID()))
                return;  // passive by its owner's choice; not ours to steer

            CharmInfo* ci = pet->GetCharmInfo();
            if (pet->GetVictim() || (ci && !ci->HasCommandState(COMMAND_FOLLOW)))
                ownerAI->PetFollow();
        }

        void ReleasePet(ObjectGuid ownerGuid, ReactStates prior)
        {
            Player* owner = ObjectAccessor::FindPlayer(ownerGuid);
            if (!owner || !owner->IsInWorld())
                return;
            if (Pet* pet = owner->GetPet())
            {
                pet->SetReactState(prior);
                LOG_INFO("playerbots.dungeonclear",
                         "[DC:{}] BRD — {}'s pet {} released from the room hold",
                         owner->GetName(), owner->GetName(), pet->GetName());
            }
        }

        // Any keeper corpse in the room that still holds its torch, looted by a
        // member already standing at it. Nobody is walked anywhere: in combat the
        // fight owns movement, and the corpse is where the melee killed it.
        void LootTorchesInReach(Player* tank)
        {
            if (PartyTorchCount(tank) >= TorchesStillNeeded(tank))
                return;
            std::list<Creature*> keepers;
            tank->GetCreatureListWithEntryInGrid(keepers, NPC_FLAME_KEEPER, LYCEUM_KEEPER_SCAN);
            for (Creature* k : keepers)
            {
                if (k->IsAlive() || TorchSlot(k) < 0)
                    continue;
                if (Player* looter = MemberAtCorpse(tank, k))
                {
                    LootTorch(tank, looter, k, tank->IsInCombat() ? "mid-fight" : "on the spot");
                    if (PartyTorchCount(tank) >= TorchesStillNeeded(tank))
                        return;
                }
            }
        }
    }

    void Tick(uint32 diff, bool moduleEnabled)
    {
        g_lyceumAccumMs += diff;
        if (g_lyceumAccumMs < LYCEUM_TICK_MS)
            return;
        g_lyceumAccumMs = 0;

        std::set<ObjectGuid> held;
        if (moduleEnabled)
        {
            for (ObjectGuid guid : DcStatusPublisher::ActiveTanks())
            {
                Player* tank = ObjectAccessor::FindPlayer(guid);
                if (!tank || !tank->IsInWorld() || tank->GetMapId() != MAP_ID)
                    continue;
                PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank);
                if (!tankAI)
                    continue;
                DcRunState const& run = DcRun::Of(tankAI);
                if (!run.enabled || run.paused)
                    continue;

                if (tank->IsAlive() && InLyceum(tank))
                    LootTorchesInReach(tank);

                bool const throne = tank->IsAlive() && ThroneRoomPreClear(tank, tankAI);
                ForEachLivingMember(tank, [&](Player* m)
                {
                    if (m->GetInstanceId() != tank->GetInstanceId())
                        return;
                    bool const lyceum = InLyceum(m);
                    if (!lyceum && !throne)
                        return;
                    PlayerbotAI* mAI = GET_PLAYERBOT_AI(m);
                    if (!mAI)
                        return;  // a human's pet is the human's
                    held.insert(m->GetGUID());
                    HoldPet(m, mAI, lyceum ? "Lyceum" : "Throne room");
                });
            }
        }

        for (auto it = g_lyceumPets.begin(); it != g_lyceumPets.end();)
        {
            if (held.count(it->first))
            {
                ++it;
                continue;
            }
            ReleasePet(it->first, it->second);
            it = g_lyceumPets.erase(it);
        }
    }
}

void RegisterBlackrockDepthsHooks(ObjectiveHookRegistry::HookTable& out)
{
    ObjectiveHookRegistry::AddHook(out, HOOK_GATHER_TORCH, &GatherShadowforgeTorch);
    ObjectiveHookRegistry::AddHook(out, HOOK_LIGHT_BRAZIER_S,
        [](Player* bot, AiObjectContext* context, DungeonBossInfo const&)
        { return LightBrazier(bot, context, GO_BRAZIER_S); });
    ObjectiveHookRegistry::AddHook(out, HOOK_LIGHT_BRAZIER_N,
        [](Player* bot, AiObjectContext* context, DungeonBossInfo const&)
        { return LightBrazier(bot, context, GO_BRAZIER_N); });
    ObjectiveHookRegistry::AddHook(out, HOOK_TOMB_MUSTER, &TombMuster);
    ObjectiveHookRegistry::AddHook(out, HOOK_TOMB_RESET_WATCH, &TombResetWatch);
    ObjectiveHookRegistry::AddHook(out, HOOK_START_ROCKNOT_ALE_RUN, &StartRocknotAleRun);
    ObjectiveHookRegistry::AddHook(out, HOOK_WAKE_HURLEY, &WakeHurley);
    ObjectiveHookRegistry::AddHook(out, HOOK_BUY_ROCKNOT_ALE, &BuyRocknotAle);
    ObjectiveHookRegistry::AddHook(out, HOOK_PHALANX_AT_DOOR, &PhalanxAtDoor);
}

void RegisterBlackrockDepthsEvents(std::vector<DungeonEvent>& out)
{
    out.push_back(
        EventBuilder(230, 1, "Ring of Law")
            .Anchored(/*encounterIndex*/ 3)
            .Persistent()
            // 1. Settle on the trigger spot. Arrival crosses area trigger 1526
            //    (you, or a self-bot relayed from the master) -> IN_PROGRESS.
            .MoveTo(BRD_ARENA_X, BRD_ARENA_Y, BRD_ARENA_Z, BRD_ARENA_RADIUS)
            // 2. Make sure the encounter actually started; if not (autonomous /
            //    no human on the trigger) the hook fires the real trigger. Done
            //    once IN_PROGRESS.
            .Custom(BRD_ENSURE_RING_STARTED_HOOK)
            // 3. Hold dead-centre, re-centring between fights, until the whole
            //    gauntlet is DONE. Combat AI fights the waves + random boss as
            //    they arrive. Generous timeout: the boss fight can be long. The
            //    same hook runs from inside the hold so a Grimstone-side reset
            //    (see the header) restarts the arena instead of stalling it.
            .MoveToHoldUntilInstanceData(BRD_ARENA_X, BRD_ARENA_Y, BRD_ARENA_Z,
                                         BRD_ARENA_RADIUS, BRD_TYPE_RING_OF_LAW,
                                         /*minValue*/ BRD_RING_DONE)
            .WhileHolding(BRD_ENSURE_RING_STARTED_HOOK)
            .Timeout(600000)
            .Build());

    // --- the SHADOWFORGE LOCK, ANCHORED between Bael'Gar and Angerforge ---
    // The Giant Doors at (723.1,-105.9,-71.5) spawn OPEN, and the lower passage
    // they stand in is the way north to Bael'Gar. Closing them is a separate,
    // deliberate act: you walk back to the East Garrison — through the East
    // Garrison Door (170570, lock 680, waived in DcEventDoorRegistry) — and pull
    // the lever at the far end of the room. That is the whole event.
    //
    // OFF-PATH OPENER, so it is ANCHORED rather than Conditional: the lever is
    // 113yd from the doors it moves and a floor above them, so nothing about
    // standing at the doors can bring the party to it (see the door-event-type
    // rule). Boss-nav has to deliver the tank to the lever, which is what an
    // objective anchor is for.
    //
    // The lever needs no key logic here: EventStepKind::UseGameObject calls
    // GameObject::Use() directly, and the DOOR branch of Use() has no lock check
    // at all (locks are adjudicated client-side and, for bots, by
    // BotCanOpenDoorLikePlayer — which the exemption covers for the walk-in
    // case). Use() -> UseDoorOrButton -> SetLootState(GO_ACTIVATED) fires the
    // lever's SmartAI chain SYNCHRONOUSLY, which activates the mechanism, the
    // Giant Doors, and the two collision hulls in one go.
    //
    // NOT persistent, and it does not need to be: every step is idempotent under
    // a rewind. MoveTo re-arrives, UseGO early-returns Done on a lever that is no
    // longer GO_STATE_READY (so a second click can never re-open the doors), and
    // the state check is a plain read.
    out.push_back(
        EventBuilder(230, 2, "Shadowforge Lock")
            .Anchored(/*orderIndex*/ 9)
            // 1. Settle at the lever inside the East Garrison.
            .MoveTo(BRD_LOCK_X, BRD_LOCK_Y, BRD_LOCK_Z, BRD_LOCK_RADIUS)
            // 2. Pull it.
            .UseGO(BRD_GO_SHADOWFORGE_LOCK, /*searchRadius*/ 15.0f)
            // 3. Confirm the machine actually moved. Gating on the DOORS rather
            //    than on the lever is the point: the lever flipping only proves
            //    the click landed, while GO_STATE_READY on 157923 proves the
            //    SmartAI chain ran and the path is open. If the world DB is
            //    missing that chain this step burns its timeout and stalls
            //    visibly instead of latching the objective on a no-op.
            .WaitForGOState(BRD_GO_GIANT_DOORS, BRD_GO_STATE_READY,
                            BRD_GIANT_DOOR_TIMEOUT_MS, BRD_GIANT_DOOR_SCAN)
            .Build());

    // --- HURLEY'S KEGS, ANCHORED ahead of Hurley (shares his index 11) ---
    // Walk to the three Thunderbrew Lager Kegs and use all three; hook 52 is
    // the clicks AND the gate (Hurley visible and no longer immune), see
    // WakeHurley. The roster's Hurley row then engages him where his path left
    // him.
    //
    // Not persistent: nothing here spans a fight. Hurley is passive until the
    // party engages him, which happens after this objective latches, and a
    // rewind re-runs a MoveTo and a hook that reads Hurley before touching a keg.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_HURLEY_KEGS, "Hurley's Kegs")
            .Anchored(/*orderIndex*/ 11)
            .MoveTo(KEG_X, KEG_Y, KEG_Z, /*radius*/ 4.0f)
            .Custom(HOOK_WAKE_HURLEY)
            // His walk from the bar's back corner to the floor is ~25yd at walk
            // speed after the yell; a minute is plenty, and a keg chain that
            // never landed shows up as this step's timeout.
            .Timeout(60000)
            .Build());

    // --- RIBBLY'S CHALLENGE, ANCHORED ahead of Ribbly (index 12) ---------
    // Ribbly Screwspigot (9543) is faction 735 — friendly. Gossip menu 1970
    // option 0 ("time to pay for your wickedness") is the whole trigger: his
    // SmartAI (event 62) sets faction 14 on him and on his three cronies (10043)
    // within 30yd and AttackStarts the player who chose it. menu 1970 has no
    // conditions row, and the leader running this event is the tank, so the
    // fight opens on the tank. The roster's Ribbly row finishes it.
    //
    // SkipIfTargetMissing: a Ribbly who is already dead (a resumed run, or a
    // human beat us to him) completes the objective instead of stalling it.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_RIBBLY, "Ribbly's Challenge")
            .Anchored(/*orderIndex*/ 12)
            .Gossip(NPC_RIBBLY, /*option*/ 0, /*searchRadius*/ 30.0f)
            .SkipIfTargetMissing()
            .Build());

    // --- ROCKNOT'S ALE RUN, ANCHORED ahead of Phalanx (index 13) ---------
    // The way a player opens the Bar Door: buy six Dark Iron Ale Mugs from
    // Plugger (hook 54), hand "Rocknot's Ale" to Private Rocknot three times
    // (hook 47), then stand clear while he walks off and kicks the kegs. His last
    // kick breaks the door; 7s later Phalanx is woken and runs from his corner to
    // the doorway, turning hostile only when he gets there. The objective latches
    // on THAT (hook 55), not on the door: the roster's Phalanx row is next, and a
    // row picked while he is still friendly has the tank chase a golem it cannot
    // attack across the doorway he is running to.
    //
    // The door wait stays as its own step. The door is what the party has to
    // walk through, and a reloaded instance can read TYPE_BAR DONE with the door
    // still shut (the upstream reload bug; see the plan's §8), which must stall
    // visibly here rather than at the door later.
    //
    // NOT an instance-data garrison: TYPE_BAR reads SPECIAL (4) for the whole
    // escort — GetData reports SPECIAL while BarAleCount is 3 and the state is
    // IN_PROGRESS — and the garrison gate is `>=`, so "hold until >= DONE (3)"
    // would pass the instant the run started.
    //
    // PERSISTENT so a fight in the bar during the escort (a patron, a crony)
    // does not rewind the event, and so the walks to Plugger and Rocknot (both
    // 15-30yd off the hold) keep the event live away from its anchor. Every step
    // survives a rewind anyway: the buy is Done once six mugs are carried, the
    // hand-in once the bar reads SPECIAL.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_ROCKNOT, "Rocknot's Ale Run")
            .Anchored(/*orderIndex*/ 13)
            .Persistent()
            .MoveTo(ROCKNOT_HOLD_X, ROCKNOT_HOLD_Y, ROCKNOT_HOLD_Z, ROCKNOT_HOLD_RADIUS)
            .Custom(HOOK_BUY_ROCKNOT_ALE)
            .Timeout(60000)
            .Custom(HOOK_START_ROCKNOT_ALE_RUN)
            .Timeout(60000)
            // Back off his walk and out of the bar's middle; Phalanx's doorway is
            // 16yd from here, close enough to meet him there.
            .MoveTo(ROCKNOT_HOLD_X, ROCKNOT_HOLD_Y, ROCKNOT_HOLD_Z, ROCKNOT_HOLD_RADIUS)
            .WaitForGOState(GO_BAR_DOOR, BRD_GO_STATE_ACTIVE_ALTERNATIVE,
                            BRD_ROCKNOT_TIMEOUT_MS, /*searchRadius*/ 40.0f)
            .Custom(HOOK_PHALANX_AT_DOOR)
            .Timeout(BRD_PHALANX_WAKE_TIMEOUT_MS)
            .Build());

    // --- THE TOMB OF THE SEVEN, ANCHORED ahead of Anger'rel (index 16) ---
    // Garrison Doom'rel's tier, muster the whole party inside (hook 51), take
    // his challenge (the Gossip step drills 1947 -> 1950 itself), and hold until
    // TYPE_TOMB_OF_SEVEN is DONE while the ghosts come to the party one by one.
    // The hold re-checks for the 15s reset every tick (hook 53) and restarts the
    // whole challenge if it happened.
    //
    // THE CREDIT TRAP this objective exists to defuse: the encounter's credit is
    // Anger'rel (bit 16), the FIRST ghost to wake. A boss-only roster would see
    // bit 16 the moment he died and walk the party out of a sealed tomb with five
    // ghosts still to come — straight into the reset. The objective shares his
    // index, sorts ahead of him, and latches only on DONE; his own row is then
    // already dead and the picker moves on.
    //
    // PERSISTENT: seven fights, each a combat gap. Ghosts that have not woken yet
    // are faction 35 and IMMUNE_TO_PC, which IsPossibleTarget already rejects,
    // so the clear never pulls one early.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_TOMB_OF_SEVEN, "Tomb of the Seven")
            .Anchored(/*orderIndex*/ 16)
            .Persistent()
            .MoveTo(TOMB_HOLD_X, TOMB_HOLD_Y, TOMB_HOLD_Z, TOMB_HOLD_RADIUS)
            .Custom(HOOK_TOMB_MUSTER)
            .Timeout(90000)
            .Gossip(NPC_DOOMREL, /*option*/ 0, /*searchRadius*/ 40.0f)
            .MoveToHoldUntilInstanceData(TOMB_HOLD_X, TOMB_HOLD_Y, TOMB_HOLD_Z, TOMB_HOLD_RADIUS,
                                         TYPE_TOMB_OF_SEVEN, /*minValue*/ STATE_DONE)
            .WhileHolding(HOOK_TOMB_RESET_WATCH)
            // Seven ghosts on a 30s cadence plus the fights. Only out-of-combat
            // time counts (Drive re-bases the step clock after every fight).
            .Timeout(420000)
            .Build());

    // --- THE LYCEUM, two ANCHORED brazier events ahead of Magmus (index 17) ---
    // The room is a PUSH, not a clear: its Reservists come back a minute after
    // they die (never-target, boxed to the room — DcNeverTargetRegistry), so
    // nothing here waits for an empty room. Each event hunts the live Flame
    // Keepers, takes their torches, and spends one on its brazier:
    //   0. a MoveTo on the anchor itself, radius LYCEUM_KEEPER_SCAN (see below)
    //   1. KillCreatureEngage the keepers (the engage pipeline walks the tank to
    //      each, across the room, through whatever it meets)
    //   2. hook 48: loot the torches; Done once the party holds one; Restart
    //      (back to 0) if it holds none and a keeper is up; otherwise wait out
    //      the pool's 300s respawn, stalling visibly at TORCH_WAIT_MS
    //   3. walk to the brazier
    //   4. hook 49/50: spend a torch and light it; Restart if the torch expired
    //
    // STEP 0 ONLY ARMS THE PERSISTENCE STICKY. With the hunt leading, the tank
    // stepped inside arriveRadius, DcObjectiveArriveAction's engage branch
    // walked it one tick toward a keeper 55-90yd off and returned BEFORE Drive,
    // so the event never stamped its progress and IsPersistentAnchoredEventActive
    // (which counts only a leading MoveTo as started) never latched. Out of the
    // ring, Advance hauled it back, forever: tr-20261018-093304-1 parked at
    // dist=8.0..9.5 > arriveRadius=8.0 on the brazier dais for four minutes with
    // not one "event 'Lyceum: South Brazier' step" line. The radius is the hunt's
    // own scan so the step is Done wherever a Restart rewinds from — anywhere the
    // hunt could have taken the tank — and never walks it back up to the dais.
    //
    // TWO EVENTS, NOT ONE: each brazier gets its own objective (the same-index
    // siblings run S, then N, then Magmus) so each completes on its own end
    // state. NEITHER IS ANCHORED ON ITS BRAZIER (LYCEUM_ENTRY in
    // DungeonEventTables.h): the south event starts just inside the door and the
    // north one on the south stand, where the south one leaves the tank, so the
    // whole room — the hunt and both walks to a brazier — runs inside an event,
    // with its pull stand-down, tight follow and pack leash, and none of it as
    // ordinary Advance. Step 3's MoveTo walks a brazier from wherever the hunt
    // left the tank, across the room if need be. The first hunt usually kills
    // both keepers, so the north event normally finds its torch already in the
    // bags and skips straight to the walk. Each event also COMPLETES on its own end state, whatever step it
    // is on: the south one on its brazier burning, the north one on the Golem
    // door opening — so a brazier a human (or a rewind) already lit is never
    // re-hunted for.
    //
    // NOT DrivesInCombat, which the plan asked for: that flag only exists on the
    // conditional rung (TestEventRegistry pins it), and an anchored event is
    // driven by the objective action between fights. The Reservists are static
    // spawns that aggro by proximity, so fights in here end; the hunt, the loot
    // and the walk happen in the gaps, and the pull system stands down for a
    // persistent anchored event so the tank face-pulls what it meets.
    //
    // MOVE AS ONE, FIGHT WHAT CAME. Between fights the tank does not take its
    // next step while any living member is more than LYCEUM_PACK_LEASH away
    // (PackLeash — DcPackLeashDecision.h bounds it). The room refills, so a
    // party strung across it is a party pulling fresh Reservists onto its
    // furthest member — in the logged runs, the healer: the tank chased those
    // adds off him across the room and once left the lit brazier 102yd behind.
    // Pets are held passive inside the room for the same reason (DcLyceum::Tick,
    // BlackrockDepthsLyceum.h): they took 15-30% of all Reservist hits.
    //
    // PERSISTENT: every step spans fights.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_LYCEUM_SOUTH, "Lyceum: South Brazier")
            .Anchored(/*orderIndex*/ 17)
            .Persistent()
            .CompleteWhenGOState(GO_BRAZIER_S, GO_STATE_LIT_OR_OPEN, LYCEUM_ROOM_SCAN)
            .PackLeash(LYCEUM_PACK_LEASH)
            .MoveTo(LYCEUM_ENTRY_X, LYCEUM_ENTRY_Y, LYCEUM_ENTRY_Z, /*radius*/ LYCEUM_KEEPER_SCAN)
            .KillCreatureEngage(NPC_FLAME_KEEPER, /*count*/ 1, LYCEUM_KEEPER_SCAN)
            .Timeout(240000)
            .Custom(HOOK_GATHER_TORCH)
            .Timeout(TORCH_WAIT_MS)
            .MoveTo(BRAZIER_S_X, BRAZIER_S_Y, BRAZIER_S_Z, /*radius*/ 4.0f)
            .Timeout(120000)
            .Custom(HOOK_LIGHT_BRAZIER_S)
            .Build());

    out.push_back(
        EventBuilder(MAP_ID, EVENT_LYCEUM_NORTH, "Lyceum: North Brazier")
            .Anchored(/*orderIndex*/ 17)
            .Persistent()
            // The doors are ~70yd from the north brazier; the scan reaches both.
            .CompleteWhenGOState(GO_GOLEM_DOOR_N, GO_STATE_LIT_OR_OPEN, LYCEUM_ROOM_SCAN)
            .PackLeash(LYCEUM_PACK_LEASH)
            .MoveTo(BRAZIER_S_X, BRAZIER_S_Y, BRAZIER_S_Z, /*radius*/ LYCEUM_KEEPER_SCAN)
            .KillCreatureEngage(NPC_FLAME_KEEPER, /*count*/ 1, LYCEUM_KEEPER_SCAN)
            .Timeout(240000)
            .Custom(HOOK_GATHER_TORCH)
            .Timeout(TORCH_WAIT_MS)
            .MoveTo(BRAZIER_N_X, BRAZIER_N_Y, BRAZIER_N_Z, /*radius*/ 4.0f)
            .Timeout(120000)
            .Custom(HOOK_LIGHT_BRAZIER_N)
            // Lighting the second brazier sets TYPE_LYCEUM DONE, which opens the
            // Golem doors in the same call. Confirm on the door — it is what the
            // party walks through, and an instance reload can read DONE with it
            // still shut (upstream; see the plan's §8).
            .WaitForGOState(GO_GOLEM_DOOR_N, GO_STATE_LIT_OR_OPEN, /*timeoutMs*/ 10000, LYCEUM_ROOM_SCAN)
            .Build());
}

// --- roster patch (relocated from BossRosterRegistry) --------------------
void RegisterBlackrockDepthsRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;

    // --- Blackrock Depths (map 230) — Ring of Law arena gauntlet --
    // The Ring of Law is its OWN DungeonEncounter (bit 3, between
    // Houndmaster Grebmar at 2 and Pyromancer Loregrain at 4), credited
    // to Grimstone (10096) — but Grimstone has NO static spawn (he is
    // summoned only when the centre area trigger fires), so BossSpawnIndex
    // can't emit him, exactly like Razorfen Downs' Tuten'kash. Add the
    // Ring of Law as an OBJECTIVE anchor at the arena centre (area trigger
    // 1526, x/y from AreaTrigger.dbc; floor z) so boss-nav drives the tank
    // into the sealed arena and the event (eventId 1) runs the gauntlet:
    // walk in -> the trigger fires -> hold dead-centre until DONE while
    // the random waves + boss are fought reactively. encounterIndex 3
    // slots it after Grebmar and before Loregrain; the objective-before-
    // boss tie-break + the picker's strictly-greater advance order it
    // correctly. No gateEntry (the boss is random; the event owns
    // completion via TYPE_RING_OF_LAW == DONE, see BlackrockDepthsEvents).
    {
        BossRosterPatch p;
        p.mapId = 230;
        p.remove = { DcBlackrockDepths::NPC_PLUGGER };
        p.add = {
            MakeObjective(OBJ(1), /*encounterIndex*/ 3, 230, "Ring of Law",
                          596.432f, -188.498f, -53.9f, /*arriveRadius*/ 12.0f,
                          /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 1),
            // --- Blackrock Depths — the Shadowforge Lock ------------------
            // The lever that closes the Giant Doors, carrying map-230 event 2.
            // It has no DungeonEncounter of its own; encounterIndex 9 is an
            // ordering hint only (NextDungeonBossValue consults the completion
            // mask for Boss anchors ONLY, precisely so an objective sharing a
            // boss's bit can't vanish when that bit flips), and 9 is General
            // Angerforge's bit. Sharing it is what puts the lever between
            // Bael'Gar (bit 8) and Angerforge: on Bael'Gar's death the picker
            // advances to the lowest key strictly greater than 8, and the
            // roster sort's objective-before-boss tie-break hands over the
            // lever first. arriveRadius 8 is the garrison room, not the wall —
            // the event's own MoveTo (radius 4) plus the UseGO 5yd approach do
            // the fine positioning.
            MakeObjective(OBJ(2), /*encounterIndex*/ 9, 230, "Shadowforge Lock",
                          615.61f, -49.78f, -59.82f, /*arriveRadius*/ 8.0f,
                          /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 2),
            // --- Blackrock Depths — the Grim Guzzler -------------------------
            // One objective per scripted gate, each sharing the index of the
            // boss it unlocks so the objective-before-boss tie-break walks the
            // party to the gate first (the Shadowforge Lock shape). The order
            // the bar is cleared in is keg -> Hurley -> Ribbly's challenge ->
            // Ribbly -> Rocknot's ale run -> Phalanx -> Plugger: Plugger's death
            // turns every patron in the bar hostile (his ON_DEATH SmartAI), so
            // he goes LAST, with the Bar Door already open behind Phalanx for
            // the party to leave through.
            //
            // And he is not fought at his counter. Killed there he died among
            // the patrons, and every run of tp-20261018-211235-1 ended the bar
            // in a brawl with up to seventeen of them. His row is re-anchored
            // on PLUGGER_CAMP past the Bar Door (keeping kill-bit 14), and
            // BossPullbackRegistry has the tank drag him out to it, so he dies
            // with the party out of every patron's aggro range.
            MakeBoss(DcBlackrockDepths::NPC_PLUGGER, 230, "Plugger Spazzring",
                     DcBlackrockDepths::PLUGGER_CAMP_X, DcBlackrockDepths::PLUGGER_CAMP_Y,
                     DcBlackrockDepths::PLUGGER_CAMP_Z,
                     /*completionFrom*/ DcBlackrockDepths::NPC_PLUGGER),
            MakeObjective(OBJ(DcBlackrockDepths::OBJ_HURLEY_KEGS), /*encounterIndex*/ 11, 230,
                          "Hurley's Kegs", DcBlackrockDepths::KEG_X, DcBlackrockDepths::KEG_Y,
                          DcBlackrockDepths::KEG_Z, /*arriveRadius*/ 8.0f, /*gateEntry*/ 0,
                          /*hook*/ 0, DcBlackrockDepths::EVENT_HURLEY_KEGS),
            MakeObjective(OBJ(DcBlackrockDepths::OBJ_RIBBLY), /*encounterIndex*/ 12, 230,
                          "Ribbly's Challenge", 878.47f, -167.73f, -49.68f, /*arriveRadius*/ 10.0f,
                          /*gateEntry*/ 0, /*hook*/ 0, DcBlackrockDepths::EVENT_RIBBLY),
            MakeObjective(OBJ(DcBlackrockDepths::OBJ_ROCKNOT), /*encounterIndex*/ 13, 230,
                          "Rocknot's Ale Run", DcBlackrockDepths::ROCKNOT_HOLD_X,
                          DcBlackrockDepths::ROCKNOT_HOLD_Y, DcBlackrockDepths::ROCKNOT_HOLD_Z,
                          /*arriveRadius*/ 8.0f, /*gateEntry*/ 0, /*hook*/ 0,
                          DcBlackrockDepths::EVENT_ROCKNOT),
            // --- Blackrock Depths — the Tomb of the Seven --------------------
            // Shares Anger'rel's index 16 — he is the encounter's credit and the
            // FIRST of seven to die, so the objective (latching only on the
            // instance's DONE) is what keeps the party in the tomb for the
            // other six. See BlackrockDepthsEvents' event 6.
            MakeObjective(OBJ(DcBlackrockDepths::OBJ_TOMB_OF_SEVEN), /*encounterIndex*/ 16, 230,
                          "Tomb of the Seven", DcBlackrockDepths::TOMB_HOLD_X,
                          DcBlackrockDepths::TOMB_HOLD_Y, DcBlackrockDepths::TOMB_HOLD_Z,
                          /*arriveRadius*/ 8.0f, /*gateEntry*/ 0, /*hook*/ 0,
                          DcBlackrockDepths::EVENT_TOMB_OF_SEVEN),
            // --- Blackrock Depths — the Lyceum braziers ----------------------
            // Two objectives on Magmus's index 17, one per brazier (events 7/8;
            // same-index objectives run in this order, then Magmus). Anchored at
            // the Lyceum door (S) and on the south stand (N), NOT on their
            // braziers, so the events own the whole room — see LYCEUM_ENTRY.
            MakeObjective(OBJ(DcBlackrockDepths::OBJ_LYCEUM_SOUTH), /*encounterIndex*/ 17, 230,
                          "Lyceum: South Brazier", DcBlackrockDepths::LYCEUM_ENTRY_X,
                          DcBlackrockDepths::LYCEUM_ENTRY_Y, DcBlackrockDepths::LYCEUM_ENTRY_Z,
                          /*arriveRadius*/ 8.0f, /*gateEntry*/ 0, /*hook*/ 0,
                          DcBlackrockDepths::EVENT_LYCEUM_SOUTH),
            MakeObjective(OBJ(DcBlackrockDepths::OBJ_LYCEUM_NORTH), /*encounterIndex*/ 17, 230,
                          "Lyceum: North Brazier", DcBlackrockDepths::BRAZIER_S_X,
                          DcBlackrockDepths::BRAZIER_S_Y, DcBlackrockDepths::BRAZIER_S_Z,
                          /*arriveRadius*/ 8.0f, /*gateEntry*/ 0, /*hook*/ 0,
                          DcBlackrockDepths::EVENT_LYCEUM_NORTH),
        };
        // --- Blackrock Depths — Stilgiss and Darkvire before Incendius ---
        // DBC order runs Loregrain -> Incendius (bit 5) -> Stilgiss (6) ->
        // Darkvire (7); the clear path takes Stilgiss and Darkvire first, then
        // Incendius, then Bael'Gar. Rotate the three onto the same slots (5-7)
        // so Loregrain (4) and Bael'Gar (8) keep theirs; the kill bits are
        // untouched.
        p.reorder = {
            { 9041, 5 },   // Warder Stilgiss
            { 9056, 6 },   // Fineous Darkvire
            { 9017, 7 },   // Lord Incendius
            // --- the Grim Guzzler: Ribbly before Phalanx --------------------
            // DBC bits run Hurley (11) -> Phalanx (12) -> Ribbly (13) ->
            // Plugger (14). Phalanx is gated on Rocknot's ale run and Ribbly is
            // not, and the ale run opens the door the party leaves by, so swap
            // the two onto each other's slot; Plugger keeps 14 and goes last.
            { 9543, 12 },  // Ribbly Screwspigot
            { 9502, 13 },  // Phalanx
        };
        t.push_back(std::move(p));
    }
}

// --- wing layout ---------------------------------------------------------
void RegisterBlackrockDepthsWings(std::unordered_map<uint32, DungeonWingLayout>& store)
{
    // --- Blackrock Depths (map 230) ------------------------------
    // Two dungeons on one map, as the 3.3 dungeon finder lists them: the
    // Detention Block (LFG 30 "Prison", High Interrogator Gerstahn -> Bael'Gar)
    // and the Upper City (LFG 276, General Angerforge -> Emperor Dagran
    // Thaurissan) — everything the Shadowforge Key opens. Both queue into the
    // SAME entrance, and from it the nearest bosses are the Upper City's (the
    // Shadowforge Lock 179yd, Angerforge 195yd; Gerstahn, the Detention Block's
    // first, is 232yd). Nearest-boss detection would put a Detention Block party
    // on the Upper City list at the door, so the wing is chosen per RUN
    // (WingSelect::Explicit, see DcRunWing), defaulting to the Detention Block.
    // There are no wing regions to fall back on: the Upper City's Phalanx and
    // Grim Guzzler sit 50yd from the Detention Block's Incendius, so no box split
    // holds — `dc on <wing>`, `dc wing` or a panel Go on the other wing's boss
    // picks it.
    //
    // Encounter bits from DungeonEncounter.dbc (map 230, bits 0-18, in path
    // order). The dungeon finder credits Gerstahn (bit 0) as the Prison's last
    // encounter; the wing here ends at Bael'Gar instead, the last boss before
    // the Shadowforge Lock.
    //   Detention Block bits 0-8: Gerstahn, Roccor, Grebmar, Ring of Law
    //     (credit Grimstone 10096, summoned — the OBJ(1) anchor stands in),
    //     Loregrain, Incendius, Stilgiss, Darkvire, Bael'Gar (cleared in the
    //     roster patch's order: Stilgiss and Darkvire before Incendius)
    //   Upper City bits 9-18: Angerforge, Argelmach, Hurley, Phalanx, Ribbly,
    //     Plugger, Flamelash, The Seven (credit Anger'rel), Magmus, Thaurissan
    // The Shadowforge Lock objective (OBJ(2), event 2) opens the Upper City: it
    // shares Angerforge's ordering index 9 and the objective-before-boss
    // tie-break takes it first, so an Upper City run walks to the lever, then
    // to Angerforge. The Upper City's other scripted gates (events 3+) are
    // objectives in the same way, each listed just before the boss it unlocks.
    using DcRoster::OBJ;
    store[230] = {true, {
        {"Blackrock Depths (Detention Block)", {
            9018,    // High Interrogator Gerstahn
            9025,    // Lord Roccor
            9319,    // Houndmaster Grebmar
            10096,   // High Justice Grimstone (Ring of Law credit, summoned)
            OBJ(1),  // Ring of Law arena objective (event 1)
            9024,    // Pyromancer Loregrain
            9017,    // Lord Incendius
            9041,    // Warder Stilgiss
            9056,    // Fineous Darkvire
            9016,    // Bael'Gar
        }, "brd-db", /*lfgDungeonId*/ 30, /*terminalBossEntry*/ 9016, /*encounterMask*/ 0x001FFu},
        {"Blackrock Depths (Upper City)", {
            OBJ(2),  // The Shadowforge Lock (event 2)
            9033,    // General Angerforge
            8983,    // Golem Lord Argelmach
            OBJ(DcBlackrockDepths::OBJ_HURLEY_KEGS),  // Hurley's Kegs (event 3)
            9537,    // Hurley Blackbreath
            OBJ(DcBlackrockDepths::OBJ_RIBBLY),       // Ribbly's Challenge (event 4)
            9543,    // Ribbly Screwspigot
            OBJ(DcBlackrockDepths::OBJ_ROCKNOT),      // Rocknot's Ale Run (event 5)
            9502,    // Phalanx
            9499,    // Plugger Spazzring
            9156,    // Ambassador Flamelash
            OBJ(DcBlackrockDepths::OBJ_TOMB_OF_SEVEN),  // Tomb of the Seven (event 6)
            9035,    // Anger'rel (The Seven)
            OBJ(DcBlackrockDepths::OBJ_LYCEUM_SOUTH),   // Lyceum: South Brazier (event 7)
            OBJ(DcBlackrockDepths::OBJ_LYCEUM_NORTH),   // Lyceum: North Brazier (event 8)
            9938,    // Magmus
            9019,    // Emperor Dagran Thaurissan
        }, "brd-uc", /*lfgDungeonId*/ 276, /*terminalBossEntry*/ 9019, /*encounterMask*/ 0x7FE00u},
    }, WingSelect::Explicit, /*defaultWing*/ "brd-db"};
}
