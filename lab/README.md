# Pull Lab

A rig that puts the pull pipeline into a chosen situation — a real pack in a real
instance, a chosen party, optional faults at chosen moments — runs it for a
minute, and scores it automatically. A fix is proven against the whole failure
catalogue before it merges, instead of one live dungeon run at a time.

The Lab runs inside the worldserver against the real code: DC's actions and
triggers, stock playerbots arbitration, core aggro/threat/call-for-help, VMAP
line of sight, SmartAI. Nothing is modelled. (The offline Sim is a later phase.)

## Commands

```
.dc lab list [glob]                      scenarios under lab/scenarios (with known failures)
.dc lab show <id>                        what a scenario stages
.dc lab run <id> [seed=N] [repeat=N] [sweep=all|phases|events]
.dc lab batch <glob> [repeat=N]          e.g. `.dc lab batch museum/*`
.dc lab status | stop
.dc test start <dungeon> ... trace=1     record + score an ordinary test run too
```

Runs are queued by party key (map, difficulty, comp, level, gear, human slot).
Up to `DungeonClear.Lab.MaxParties` (default 15, max 20) warm parties drain the
queues in parallel; each is a normal test-run party that re-stages its own
instance between runs instead of re-provisioning.

## What a run does

1. **Staging**: `dc off`, the party healed, revived, cooldowns reset, and placed
   at `party.start` in a small formation with seeded jitter. Every hostile
   creature the scenario does not keep is hidden. It is phased out and set
   passive, which needs no DB write and can be reversed. Kept spawns are revived
   or reset at home. A creature whose grid loads later is hidden the moment it
   enters the world.
2. **Starting**: settings overrides, then the pull setting, then `dc on`. Every
   roster boss except `geometry.objective` is skipped, so DC routes toward the
   objective and pulls whatever stands on that route. With
   `party.seedTarget: true`, the pull latch is pinned on the target pack.
3. **Running**: the LabTrace recorder samples every 250 ms. It also takes the
   pull-phase, verdict and passive taps, damage and engage hooks, and every
   executed playerbots action. The run ends on the goal (every goal-pack mob
   dead), a wipe, the timeout, or DC switching itself off.
4. **Scoring**: the ten oracles (O1–O10) produce a verdict
   (`pass` / `fail` / `expected-fail` / `unexpected-pass`), written to:
   - one record per run in `dc_labruns.jsonl`;
   - the trace in `lab_traces/<lr-id>.jsonl`;
   - one chat line to the issuer, and a matrix when the batch ends.

## Reading results

```
tools/dc_test_run.py lr-...            record, oracle verdicts, LAB log lines
tools/dc_test_run.py lb-...            the whole batch
tools/lab_trace.py lr-...              summary, pull timeline
tools/lab_trace.py lr-... --oracle O1  the violation + the frame it happened in
tools/lab_trace.py lr-... --actions Tank   executed-action stream (tick thieves)
tools/dc_analytics.py                  labruns + labrun_oracles tables
```

The oracles are pure over the trace, so an oracle change can be checked
against a whole recorded baseline without re-running anything:

```
clang++ -std=c++20 -O2 -I src t/LabRescore.cpp src/Lab/DcLab{Json,Trace,Oracles,Scenario}.cpp \
        src/TestRun/DcTestComp.cpp -o lab_rescore
./lab_rescore <data-dir>/lab_traces/lr-*.jsonl --scenario-dir lab/scenarios
```

## Oracles

| id | fails when |
|---|---|
| O1 | a mob on the party is unattended (nobody attacking, tank not near or closing) > 3 s (6 s parked ranged / LOS pull) |
| O2 | a non-tank follower hits or starts attacking a pull mob before the tank hit it + `PullPlayerReleaseDelay` |
| O3 | a held (passive/stay) follower loses > 30 % hp while not released |
| O4 | a DPS is idle (out of combat, or no damage) > 4 s while the tank has fought > 4 s |
| O5 | one executed action repeats > 8 s with no movement/target/cast/damage change (tick thief) |
| O6 | Forming > 9 s, Advancing > 20 s, Returning > 25 s, or Idle-in-combat under an Advanced verdict with no maneuver tick |
| O7 | engaged mobs exceed the governor's prediction by > 1; any outside-pack joiner (attributed: patrol / call-for-help / proximity) |
| O8 | > 3 verdict flips or > 5 movement reversals inside 10 s |
| O9 | (human scenarios) followers drawn to the human during the threat lead, tank parked on a far human, human fighting alone |
| O10 | goal pack alive at the end, any death, wipe |

Every threshold can be overridden per scenario under `expect.oracles`.
Examples: `"O7": {"joinersMax": 2}`, `"O1": {"unattendedS": 5}`. The keys are in
`DcLabOracles::ApplyOverrides`.

## Scenario files

`lab/scenarios/<group>/<name>.json`. The id is the path without `.json`.
`//` and `#` comments and trailing commas are allowed.

```jsonc
{
  "about": "what this reproduces",
  "geometry": { "map": 36, "dungeon": "deadmines", "heroic": false,
                "region": { "center": [x, y, z], "radius": 70 },
                "objective": 639 },               // roster boss DC routes toward (required for committed scenarios)
  "actors": {
    "native": { "packs": { "target": [spawnId, ...], "side": [...] },
                "keep": [spawnId, ...] },         // bare keep list = the "target" pack
    "synthetic": [ { "tag": "add1", "entry": 1708, "pos": [x, y, z, o], "spawn": "onInject" } ]
  },
  "party": { "comp": "warrior-tank,priest,mage,rogue,hunter", "level": 20,
             "start": [x, y, z, o], "react": "fast", "seedTarget": true,
             "human": { "slot": 4, "script": "humans/hunter-pulls-ahead" } },
  "settings": { "PullSetting": 2, "PullDynamicMaxLeeroyMobs": 5 },
  "injections": [ { "when": { "phaseEnter": "Returning", "delayMs": 400 },
                    "do": { "spawn": "add1", "aggro": "heal" } } ],
  "goal": { "kill": "pack:target", "timeoutS": 90 },
  "expect": { "oracles": { "O1": "pass", "O7": { "joinersMax": 1, "expect": "fail" } },
              "knownFailure": "open:#17" }
}
```

- **Comp tokens** have the form `class[-spec|-tank|-heal|-dps]`. A bare class
  takes the positional role: slot 0 tanks, slot 1 heals, the rest are DPS.
  Slot 0 must be the tank. An empty comp draws a seeded random one.
- **Expectations**:
  - `knownFailure: "open:<ref>"` marks a museum entry that is expected to fail
    today. A run that reproduces it is `expected-fail`; a run that no longer
    does is `unexpected-pass`, which means fixed and the entry needs updating.
  - `"fixed:<ref>"` entries must pass.
- **Native vs synthetic**: formations, waypoint patrols, SmartAI range mode and
  linked respawn only behave for real DB spawns, so keep those natively. Summons
  are only for "something aggroes now" injections.

### Injections: faults at chosen moments

Each injection is a `when` plus a `do`. Injections key on pull events, not wall
clock, so "an add aggroes the healer 400 ms after the drag-back starts" lands at
the same point of the pull on every run.

- **`when`**:
  - `{"atMs": N}`
  - `{"phaseEnter": "Forming|Advancing|Returning|Engage|Idle", "delayMs": N, "occurrence": K}`
  - `{"event": E, "delayMs": N}`, where `E` is one of `commit`, `aggroConfirmed`,
    `campReached`, `safetyRelease`, `followerReleased`, `verdict:Leeroy`,
    `verdict:Advanced`, `verdict:PatrolHold`, `tagCast` (= Advancing),
    `plant` (= campReached), `endCampFight`, `nextScoutStart`
  - `{"predicate": "<lhs> <op> <n>"}`, where `<lhs>` is one of `tankDistToCamp`,
    `tankHp`, `tankDistToPack`, `partyMinHp`, `elapsedS`, `packEngaged`
- **`do`** — `<ref>` below is a unit reference: `tank`, `heal`, `dps`,
  `dps2`, `slot:N`, `human`, `party`, `pack:<tag>`, a synthetic tag,
  `spawn:<spawnId>`, `pullTarget`, `nearest`, `nearestIdle`.

| `do` | effect |
|---|---|
| `{"spawn": "<tag>", "aggro": "<ref>"}` | summon a synthetic actor (registered in the trace), optionally engaging `<ref>` |
| `{"aggro": "<ref>", "on": "<ref>"}` | make existing mobs engage a party member |
| `{"startPatrol": "<ref>", "path": [[x,y,z],...]}` | walk mobs along a path (default: to the party start) |
| `{"forceCallForHelp": "<ref>", "radius": 15}` | `Creature::CallForHelp` now |
| `{"stun"\|"root"\|"fear"\|"daze": "<ref>", "spell": id, "durationMs": n}` | aura on the unit (defaults: Hammer of Justice, Frost Nova, Fear, Dazed) |
| `{"knockback": "<ref>", "from": "<ref>", "speedXY": 10, "speedZ": 5}` | knock back away from `from` |
| `{"evade": "<ref>"}` / `{"dropCombat": "<ref>"}` / `{"kill": "<ref>"}` | |
| `{"door": spawnId, "state": "open\|close"}` | set a door's state |
| `{"spawnLosBlocker": [x,y,z,o], "entry": 18972}` | a closed portcullis that blocks dynamic LOS but not the navmesh (removed at the next staging) |
| `{"setReact": "<ref>", "profile": "fast\|masterless"}` | switch bots between the GM-master fast path and the masterless slow think |
| `{"humanAction": "attack", "target": "<ref>"}` | drive the puppet (any puppet action) |
| `{"note": "..."}` | just mark the trace |

### Sweeps and heat-maps

`.dc lab run <id> sweep=all` runs the scenario once per point of a timing grid.
The swept injection is the one marked `"sweep": true`, or the first one. Its
`when` is replaced at each point:
- every holding-phase entry × `delayMs` ∈ {0, 150, 400, 1000, 2500};
- plus every pull event (`sweep=phases` or `sweep=events` runs one half).

All 25 points share one party key, so one warm party walks the whole grid. The
batch ends with a phase × delay matrix in chat and the log, and in
`lab_heatmaps/<batchId>.json`. Each cell is a verdict letter (`P`, `F`,
`x` expected-fail, `U` unexpected-pass, `E` error) plus the oracles that failed
there. It shows which windows are holes instead of finding them by luck.

### The human puppet

`party.human: {"slot": N, "script": "humans/<name>"}` turns slot N into a human.
Slot N must not be 0, which is the DC tank.

- **How it becomes human.** The Lab detaches the character's PlayerbotAI through
  the public `PlayerbotHolder::DisablePlayerBot`. Playerbots defines a real
  player as one with no AI, so from then on every stock gate (real-player
  master, wait-for-attack, avoid-aoe, follow-master, master-threat ownership)
  and every DC human branch sees a human.
- **Its place in the party.** The puppet leads the group and is every bot's
  master. The bots keep stock follow-master, as they would under a real player.
- **Restoring it.** At teardown, `OnBotLogin` on the same holder restores the AI
  and files the bot back under the holder, so the normal logout runs. No
  playerbots code is edited.
- **Its script** (`lab/humans/<name>.json`) is a list of steps, each a `when`
  plus one action:
  - `follow <ref> [dist]`;
  - `attack <ref> [spell, everyMs, range]`;
  - `moveTo [x,y,z]`;
  - `runAhead <yd>`;
  - `standAt camp|start`;
  - `castAoe <spellId>`;
  - `idle`.

  `humanAction` injections use the same actions.

Known gap: client-only behaviour is not reproduced. That covers areatriggers a
client fires, client movement packets and addon traffic.

### Synthetic formations (spike result)

Summoned actors cannot join a formation. `CreatureGroup::AddMember` looks the
member up in `FormationMgr::CreatureGroupMap` by DB spawnId and dereferences the
result without an end check. A summon has spawnId 0, so that lookup is undefined
behaviour. Waypoint patrols are equally DB-keyed. Formation and patrol cases
therefore use native spawns; `startPatrol` approximates a patrol with MovePoint
legs, which DC's lone-patroller detection does not classify as a patrol.

### Authoring from a real pull

Every pull a test run makes lands in `dungeonclear_pull_snapshots.jsonl`.
`tools/lab_from_snapshot.py` turns one into a scenario:

```
tools/lab_from_snapshot.py --list --over --dungeon strat
tools/lab_from_snapshot.py 'tr-20260922-185150-2#3' --id calibration/outside-adds/x --write
```

The converter keeps the pack and every creature that really joined, starts the
party where the tank stood, and aims DC at the boss the run killed next. That
boss must have a DB spawn on the map; a gong- or event-summoned boss falls back
to the nearest spawned roster boss.

## Layout

- `scenarios/calibration/`: converted from real pulls (`source.snapshot` names
  the pull).
- `scenarios/museum/`: the regression museum, one scenario per catalogued
  failure (plan §3).
- `humans/`: puppet scripts for scenarios with a human in the party.
