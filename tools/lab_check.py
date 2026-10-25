#!/usr/bin/env python3
# Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3.
"""Check Pull Lab scenarios against the world DB before committing them.

  lab_check.py                      every scenario under lab/scenarios
  lab_check.py 'museum/*'           a glob over scenario ids
  lab_check.py path/to/file.json

The C++ loader (t/TestLabScenario CommittedScenariosLoad) checks the format.
This checks the WORLD: that the dungeon token exists and matches the map, every
kept spawnId is a creature on that map, the objective boss has a spawn there
(an event-summoned boss cannot anchor DC's route), synthetic entries exist, the
party start is near the action, and referenced puppet scripts exist.

Read-only: a `mysql` SELECT session against acore_world, nothing written.
Exit status 1 when any scenario has an ERROR.
"""

import fnmatch
import json
import math
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
LAB = HERE.parent / "lab"
REFS = {"tank", "heal", "healer", "dps", "human", "party", "pullTarget", "nearest", "nearestIdle"}


def strip_comments(text):
    out, i, in_str = [], 0, False
    while i < len(text):
        c = text[i]
        if in_str:
            out.append(c)
            if c == "\\":
                out.append(text[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
        elif c == '"':
            in_str = True
            out.append(c)
        elif c == "#" or text.startswith("//", i):
            while i < len(text) and text[i] != "\n":
                i += 1
            continue
        else:
            out.append(c)
        i += 1
    return re.sub(r",(\s*[}\]])", r"\1", "".join(out))


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
    if r.returncode:
        raise RuntimeError(r.stderr.strip())
    return [l.split("\t") for l in r.stdout.splitlines() if l.strip()]


def id_col():
    rows = mysql("SELECT column_name FROM information_schema.columns WHERE table_schema = DATABASE() "
                 "AND table_name = 'creature' AND column_name IN ('id', 'id1')")
    return "id1" if any(r[0] == "id1" for r in rows) else "id"


def catalogue():
    for cand in [HERE.parent.parent.parent.parent / "env" / "dist" / "bin" / "dc_test_dungeons.json",
                 Path.home() / "azerothcore" / "env" / "dist" / "bin" / "dc_test_dungeons.json"]:
        if cand.exists():
            d = json.loads(cand.read_text())
            rows = {r["token"]: r for r in d.get("dungeons", [])}
            for alias, target in (d.get("aliases") or {}).items():
                if target in rows:
                    rows[alias] = rows[target]
            return rows
    return {}


def check(path, sid, col, cat):
    errs, warns = [], []
    try:
        s = json.loads(strip_comments(path.read_text()))
    except Exception as e:
        return [f"not JSON: {e}"], []
    g = s.get("geometry", {})
    m = g.get("map")
    tok = g.get("dungeon")
    if tok:
        row = cat.get(tok)
        if not row:
            errs.append(f"unknown dungeon token '{tok}'")
        elif m and row["mapId"] != m:
            errs.append(f"token '{tok}' is map {row['mapId']}, scenario says {m}")
        m = m or (row or {}).get("mapId")
    if not m:
        return errs + ["no map"], warns

    packs = s.get("actors", {}).get("native", {}).get("packs", {}) or {}
    keep = s.get("actors", {}).get("native", {}).get("keep", []) or []
    ids = sorted({int(i) for v in packs.values() for i in v} | {int(i) for i in keep})
    pos = {}
    if ids:
        rows = mysql(f"SELECT guid, map, {col}, position_x, position_y, position_z FROM creature "
                     f"WHERE guid IN ({','.join(map(str, ids))})")
        found = {int(r[0]): r for r in rows}
        for i in ids:
            r = found.get(i)
            if not r:
                errs.append(f"spawnId {i} does not exist")
            elif int(r[1]) != m:
                errs.append(f"spawnId {i} is on map {r[1]}, not {m}")
            else:
                pos[i] = tuple(float(x) for x in r[3:6])
    obj = g.get("objective")
    if not obj:
        errs.append("no geometry.objective")
    else:
        n = int(mysql(f"SELECT COUNT(*) FROM creature WHERE map = {m} AND {col} = {int(obj)}")[0][0])
        if not n:
            errs.append(f"objective {obj} has no spawn on map {m} (event-summoned bosses cannot anchor DC's route)")
    for syn in s.get("actors", {}).get("synthetic", []) or []:
        if not mysql(f"SELECT entry FROM creature_template WHERE entry = {int(syn.get('entry', 0))}"):
            errs.append(f"synthetic '{syn.get('tag')}' entry {syn.get('entry')} not in creature_template")

    start = (s.get("party") or {}).get("start")
    tgt = [pos[int(i)] for i in packs.get("target", keep) if int(i) in pos]
    if start and tgt:
        d = min(math.dist(start[:2], p[:2]) for p in tgt)
        dz = min(abs(start[2] - p[2]) for p in tgt)
        if d > 70:
            warns.append(f"party start is {d:.0f}yd from the nearest target-pack mob")
        if d < 8:
            warns.append(f"party start is only {d:.0f}yd from the target pack (inside aggro?)")
        if dz > 15:
            warns.append(f"party start z differs from the target pack by {dz:.0f}yd")
    human = (s.get("party") or {}).get("human")
    if human and human.get("script"):
        if not (LAB / f"{human['script']}.json").exists():
            errs.append(f"puppet script lab/{human['script']}.json missing")
    tags = set(packs) | {x.get("tag") for x in s.get("actors", {}).get("synthetic", []) or []}
    for inj in s.get("injections", []) or []:
        for k, v in (inj.get("do") or {}).items():
            if isinstance(v, str) and k not in ("state", "profile", "note", "humanAction"):
                ref = v[5:] if v.startswith("pack:") else v
                if not (ref in REFS or ref in tags or re.match(r"^(dps\d+|slot:\d+|spawn:\d+)$", ref)):
                    warns.append(f"injection ref '{v}' resolves to nothing known")
    for oid in ((s.get("expect") or {}).get("oracles") or {}):
        if not re.match(r"^O([1-9]|10)$", oid):
            errs.append(f"unknown oracle id {oid}")
    return errs, warns


def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else "*"
    files = []
    p = Path(arg)
    if p.suffix == ".json" and p.exists():
        files.append((p, p.stem))
    else:
        for f in sorted((LAB / "scenarios").rglob("*.json")):
            sid = str(f.relative_to(LAB / "scenarios"))[:-5]
            if fnmatch.fnmatch(sid, arg):
                files.append((f, sid))
    if not files:
        print(f"no scenarios match {arg}")
        return 1
    col = id_col()
    cat = catalogue()
    bad = 0
    for f, sid in files:
        errs, warns = check(f, sid, col, cat)
        status = "ERROR" if errs else ("warn" if warns else "ok")
        print(f"{status:<5} {sid}")
        for e in errs:
            print(f"      ERROR {e}")
        for w in warns:
            print(f"      warn  {w}")
        bad += bool(errs)
    print(f"\n{len(files)} scenario(s), {bad} with errors")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
