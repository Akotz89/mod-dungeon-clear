/*
 * mod-dungeon-clear — DungeonClearAutoStart.h
 *
 * Ownerless entry point for STARTING a dungeon-clear run on a bot-only party,
 * for use by external drivers (e.g. mod-playerbot-dungeon-sim) that assemble and
 * teleport a bot group with no real-player owner.
 *
 * The normal `.dc on` path (DungeonClearCommand.cpp -> DungeonClearDispatch ->
 * DcOnAction) self-authorizes: the Event owner must be a REAL player in the
 * bot's group. A fully autonomous bot-only group has no such owner, so it can
 * never start a clear that way. These functions provide a sanctioned, separately
 * gated path that bypasses only the human-owner check — every other guard (in a
 * dungeon, bosses known, nobody dead, a tank present) still applies.
 *
 * Gating: StartAutonomousClear is a no-op unless the global config
 *   DungeonClear.AllowAutonomousBotRuns = 1
 * so the human-facing `.dc` security model is unchanged by default.
 */

#ifndef _DUNGEON_CLEAR_AUTOSTART_H
#define _DUNGEON_CLEAR_AUTOSTART_H

class Player;

namespace DungeonClearControl
{
    // Turn the dungeon-clear run ON for an ALREADY-ELECTED leader tank. This is
    // the exact enable sequence `dc on` runs on the leader (set "dungeon clear
    // enabled", register the status pusher, reset all transient run state, apply
    // the pull setting), factored out so both the player command and the
    // autonomous driver share one implementation. It does NOT do the human-owner
    // authorization. Pre-gates: leader must be a playerbot, on a dungeon/raid
    // map, with a known boss list, and no party member dead. Returns true when
    // the run was enabled, false when a pre-gate failed. Followers need no call:
    // they self-follow off the leader's enabled flag.
    bool EnableClearOnLeader(Player* leaderTank);

    // Autonomous start: elect the leader tank from ANY group member
    // (DcLeaderSignal::FindLeaderTank) and enable the run on it. Returns false
    // when DungeonClear.AllowAutonomousBotRuns is off, no tank bot is present in
    // the group/on the map, or EnableClearOnLeader's pre-gates fail. Safe to call
    // every tick — enabling an already-running leader is effectively a re-arm, so
    // callers should gate their own calls (start once per run) to avoid resetting
    // progress mid-clear.
    bool StartAutonomousClear(Player* anyGroupMember);
}

#endif  // _DUNGEON_CLEAR_AUTOSTART_H
