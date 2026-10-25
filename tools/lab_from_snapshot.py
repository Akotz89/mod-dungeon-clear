#!/usr/bin/env python3
# Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3.
"""Turn a real pull into a Pull Lab scenario (pull-lab plan §3 calibration set).

Every pull a test run took is recorded in dungeonclear_pull_snapshots.jsonl
(tank position, the target and every creature nearby by spawnId, what the
governor predicted, what actually joined). A snapshot converts mechanically
into a native-mode scenario: keep the target and its neighbours, start the
party where the tank stood, aim DC at the boss the run killed next.

  lab_from_snapshot.py --list [--dungeon strat] [--over] [--outside] [--los] [--wipe]
  lab_from_snapshot.py tr-20260922-185149-1#3 --id calibration/strat-x [--write]

Pack tags: "target" = the pulled mob plus everything within --pack-radius of it
(or in its formation); "near" = the rest of the snapshot's neighbourhood, kept
so the same outside adds can join as they did for real. The scenario's O7
expectation records what the real run saw.

Reads the data dir (worldserver cwd, found like dc_test_run.py) and, for the
objective-boss fallback, a read-only `mysql` session against acore_world.
"""

import argparse
import json
import math
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
MODULE = HERE.parent


def die(msg):
    print(f"lab_from_snapshot: {msg}", file=sys.stderr)
    sys.exit(2)


def find_data_dir(explicit):
    if explicit:
        return Path(explicit).expanduser()
    if os.environ.get("DC_DATA_DIR"):
        return Path(os.environ["DC_DATA_DIR"]).expanduser()
    for start in [HERE, Path.cwd().resolve()]:
        for cand in [start, *start.parents]:
            hit = cand / "env" / "dist" / "bin"
            if (hit / "dc_testruns.jsonl").exists():
                return hit
    die("could not find the data dir; pass --data-dir")


def load_jsonl(path):
    out = []
    with open(path) as fh:
        for line in fh:
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return out


def mysql(sql):
    conf = Path.home() / "azerothcore" / "env" / "dist" / "etc" / "worldserver.conf"
    info = "127.0.0.1;3306;acore;acore;acore_world"
    if conf.exists():
        for line in conf.read_text(errors="ignore").splitlines():
            if line.startswith("WorldDatabaseInfo"):
                info = line.split('"')[1]
    host, port, user, pw, db = info.split(";")
    r = subprocess.run(["mysql", f"-h{host}", f"-P{port}", f"-u{user}", f"-p{pw}", db, "-N", "-B", "-e", sql],
                       capture_output=True, text=True)
    return [l.split("\t") for l in r.stdout.splitlines() if l.strip()]


def creature_id_col():
    # The spawn table's entry column was renamed upstream (id1 -> id); probe.
    rows = mysql("SELECT column_name FROM information_schema.columns WHERE table_schema = DATABASE() "
                 "AND table_name = 'creature' AND column_name IN ('id', 'id1')")
    names = {r[0] for r in rows}
    return "id1" if "id1" in names else "id"


def pack_of(snap, radius):
    tgt = snap["target"]
    pack = {tgt["spawnId"]}
    for n in snap["nearby"]:
        if n["dTarget"] <= radius or (n.get("formation") and tgt.get("formation") and n["dTarget"] <= radius * 2):
            pack.add(n["spawnId"])
    return pack


def classify(snap, radius):
    pack = pack_of(snap, radius)
    joined = {j["spawnId"] for j in snap["joined"]}
    return {
        "over": snap["observed"] > snap["predicted"] + 1,
        "outside": bool(joined - pack),
        "los": snap.get("losPull", False),
        "wipe": snap.get("wipedHere", False),
        "outsideIds": sorted(joined - pack),
    }


def objective_for(snap, run):
    # Real bosses only: DC event anchors carry synthetic entries (> 1e6) that
    # name an objective, not a creature, and cannot anchor a route on their own.
    for b in run.get("bossTimeline", []):
        if b.get("t", 0) >= snap["t"] and 0 < b.get("entry", 0) < 1000000:
            return b["entry"], b.get("name", "")
    killed = {b.get("name") for b in run.get("bossTimeline", [])}
    for name in run.get("bossRoster", []):
        if name in killed:
            continue
        rows = mysql("SELECT ct.entry FROM creature_template ct JOIN creature c ON c." + creature_id_col() + " = ct.entry "
                     f"WHERE c.map = {int(snap['mapId'])} AND ct.name = '{name.replace(chr(39), chr(39) * 2)}' LIMIT 1")
        if rows:
            return int(rows[0][0]), name
    return 0, ""


def comp_tokens(run):
    toks = []
    for c in run.get("comp", []):
        cls = c.get("class") or c.get("className", "")
        spec = (c.get("spec") or "").split(" ")[0]
        toks.append(f"{cls}-{spec}" if spec and spec != "(random)" else f"{cls}-{c.get('role', 'dps')}")
    return ",".join(toks)


def build(snap, run, args):
    radius = args.pack_radius
    tgt = snap["target"]
    pack = pack_of(snap, radius)
    near = [n["spawnId"] for n in snap["nearby"] if n["spawnId"] not in pack]
    # Mobs that really joined but sat outside the snapshot's nearby list are
    # kept too, or the over-pull the snapshot recorded could never recur.
    near += [j["spawnId"] for j in snap["joined"] if j["spawnId"] and j["spawnId"] not in pack and j["spawnId"] not in near]
    cls = classify(snap, radius)
    tank = snap["tank"]
    o = math.atan2(tgt["y"] - tank["y"], tgt["x"] - tank["x"])
    obj, objName = objective_for(snap, run)
    col = creature_id_col()
    if obj and not int(mysql(f"SELECT COUNT(*) FROM creature WHERE map = {int(snap['mapId'])} AND {col} = {int(obj)}")[0][0]):
        # Event-summoned (no DB spawn) — it cannot anchor DC's route. The
        # nearest spawned boss from the run's own DC roster instead.
        names = ",".join("'" + n.replace("'", "''") + "'" for n in run.get("bossRoster", [])) or "''"
        rows = mysql(f"SELECT ct.entry, ct.name FROM creature c JOIN creature_template ct ON ct.entry = c.{col} "
                     f"WHERE c.map = {int(snap['mapId'])} AND ct.name IN ({names}) ORDER BY "
                     f"POW(c.position_x - {tank['x']}, 2) + POW(c.position_y - {tank['y']}, 2) LIMIT 1")
        if rows:
            print(f"objective {objName} ({obj}) has no spawn; using nearest roster boss {rows[0][1]}", file=sys.stderr)
            obj, objName = int(rows[0][0]), rows[0][1]
    flags = [k for k in ("over", "outside", "los", "wipe") if cls[k]]
    about = (f"Calibration: {run.get('dungeon')} pull #{snap['pullIdx']} of {snap['runId']} — "
             f"predicted {snap['predicted']}, observed {snap['observed']}"
             + (f" ({', '.join(flags)})" if flags else "") + f"; target {tgt['entry']}#{tgt['spawnId']}.")
    scenario = {
        "about": about,
        "schema": 1,
        "source": {"snapshot": f"{snap['runId']}#{snap['pullIdx']}", "joined": [j["spawnId"] for j in snap["joined"]],
                   "outsideJoined": cls["outsideIds"], "predicted": snap["predicted"], "observed": snap["observed"]},
        "geometry": {"map": snap["mapId"], "dungeon": run.get("dungeon", ""), "heroic": bool(snap.get("heroic")),
                     "region": {"center": [tgt["x"], tgt["y"], tgt["z"]], "radius": args.radius},
                     "objective": obj},
        "actors": {"native": {"packs": {"target": sorted(pack), **({"near": sorted(near)} if near else {})}}},
        "party": {"comp": comp_tokens(run), "level": run.get("level", 0),
                  "start": [round(tank["x"], 2), round(tank["y"], 2), round(tank["z"], 2), round(o, 3)],
                  "react": "fast", "seedTarget": args.seed_target},
        "settings": {"PullSetting": 2},
        "goal": {"kill": "pack:target", "timeoutS": args.timeout},
        "expect": {"oracles": {"O7": {"joinersMax": 1}}},
    }
    if objName:
        scenario["geometry"]["objectiveName"] = objName
    if cls["outside"] or cls["over"] or cls["wipe"]:
        scenario["expect"]["knownFailure"] = f"open:snapshot {snap['runId']}#{snap['pullIdx']}"
        scenario["expect"]["oracles"]["O7"]["expect"] = "fail"
    return scenario


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("snapshot", nargs="?", help="<runId>#<pullIdx>")
    ap.add_argument("--data-dir")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--dungeon")
    for f in ("over", "outside", "los", "wipe"):
        ap.add_argument(f"--{f}", action="store_true")
    ap.add_argument("--id", help="scenario id, e.g. calibration/strat-gauntlet-1")
    ap.add_argument("--write", action="store_true", help="write lab/scenarios/<id>.json")
    ap.add_argument("--pack-radius", type=float, default=8.0)
    ap.add_argument("--radius", type=float, default=60.0)
    ap.add_argument("--timeout", type=int, default=90)
    ap.add_argument("--seed-target", action="store_true")
    args = ap.parse_args()

    data = find_data_dir(args.data_dir)
    snaps = load_jsonl(data / "dungeonclear_pull_snapshots.jsonl")
    runs = {r["runId"]: r for r in load_jsonl(data / "dc_testruns.jsonl") if "runId" in r}

    if args.list:
        for s in snaps:
            run = runs.get(s["runId"], {})
            if args.dungeon and run.get("dungeon") != args.dungeon:
                continue
            c = classify(s, args.pack_radius)
            if any(getattr(args, f) and not c[f] for f in ("over", "outside", "los", "wipe")):
                continue
            flags = ",".join(f for f in ("over", "outside", "los", "wipe") if c[f])
            print(f"{s['runId']}#{s['pullIdx']:<3} {run.get('dungeon', '?'):<14} t={s['t']:>5}s "
                  f"pred={s['predicted']} obs={s['observed']} target={s['target']['entry']}#{s['target']['spawnId']} "
                  f"{flags}")
        return

    if not args.snapshot or "#" not in args.snapshot:
        ap.error("give <runId>#<pullIdx> (see --list)")
    run_id, idx = args.snapshot.split("#")
    snap = next((s for s in snaps if s["runId"] == run_id and s["pullIdx"] == int(idx)), None)
    if not snap:
        die(f"no snapshot {args.snapshot}")
    run = runs.get(run_id)
    if not run:
        die(f"no run record {run_id} in dc_testruns.jsonl")
    sc = build(snap, run, args)
    text = json.dumps(sc, indent=2) + "\n"
    if args.write:
        if not args.id:
            die("--write needs --id")
        out = MODULE / "lab" / "scenarios" / f"{args.id}.json"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text)
        print(f"wrote {out}")
    else:
        print(text)


if __name__ == "__main__":
    main()
