#!/usr/bin/env python3
# Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3.
"""View a Pull Lab trace (LabTrace JSONL, src/Lab/DcLabTrace.h).

A trace is written for every Lab run (lr-...) and every `.dc test start ...
trace=1` run (tr-...) into <data-dir>/lab_traces/<runId>.jsonl.

  lab_trace.py <runId|path>                 summary: party, units, oracles, pull timeline
  lab_trace.py <id> --timeline              every event in order, names resolved
  lab_trace.py <id> --actions [BOT]         executed-action stream (repeats collapsed)
  lab_trace.py <id> --frame SECONDS         the frame nearest a time, all units/bots
  lab_trace.py <id> --oracle O5             one oracle, plus the frame at its violation
  lab_trace.py <id> --grep RE               raw lines matching RE (names substituted)
  lab_trace.py --list                       recent traces

The data dir (worldserver's cwd) is found the same way dc_test_run.py finds it;
override with --data-dir or $DC_DATA_DIR. $DC_LAB_TRACE_DIR overrides the trace dir.
"""

import argparse
import json
import math
import os
import re
import sys
from pathlib import Path

PHASES = ["Idle", "Forming", "Advancing", "Returning", "Engage"]


def die(msg):
    print(f"lab_trace: {msg}", file=sys.stderr)
    sys.exit(2)


def find_data_dir(explicit):
    if explicit:
        return Path(explicit).expanduser()
    if os.environ.get("DC_DATA_DIR"):
        return Path(os.environ["DC_DATA_DIR"]).expanduser()
    for start in [Path(__file__).resolve().parent, Path.cwd().resolve()]:
        for cand in [start, *start.parents]:
            hit = cand / "env" / "dist" / "bin"
            if (hit / "dc_testruns.jsonl").exists():
                return hit
            if (cand / "dc_testruns.jsonl").exists():
                return cand
    return Path.cwd()


def trace_dir(data_dir):
    env = os.environ.get("DC_LAB_TRACE_DIR")
    if env:
        p = Path(env)
        return p if p.is_absolute() else data_dir / p
    return data_dir / "lab_traces"


def resolve(arg, data_dir):
    p = Path(arg)
    if p.exists():
        return p
    d = trace_dir(data_dir)
    for cand in [d / arg, d / f"{arg}.jsonl"]:
        if cand.exists():
            return cand
    hits = sorted(d.glob(f"{arg}*.jsonl")) if d.exists() else []
    if len(hits) == 1:
        return hits[0]
    if hits:
        die(f"'{arg}' is ambiguous: " + ", ".join(h.stem for h in hits[:10]))
    die(f"no trace '{arg}' in {d}")


class Trace:
    def __init__(self, path):
        self.path = path
        self.hdr = {}
        self.settings = []
        self.members = []
        self.units = []
        self.frames = {}  # t -> {"u": [...], "b": [...], "d": {...}}
        self.events = []
        self.actions = []
        self.end = {}
        self.oracles = []
        self.raw = []
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                self.raw.append(line)
                try:
                    r = json.loads(line)
                except json.JSONDecodeError:
                    continue
                k = r.get("k")
                t = r.get("t", 0)
                if k == "hdr":
                    self.hdr = r
                elif k == "setting":
                    self.settings.append(r)
                elif k == "member":
                    self.members.append(r)
                elif k == "unit":
                    self.units.append(r)
                elif k in ("fu", "fb", "fd"):
                    fr = self.frames.setdefault(t, {"u": [], "b": [], "d": None})
                    if k == "fu":
                        fr["u"].append(r)
                    elif k == "fb":
                        fr["b"].append(r)
                    else:
                        fr["d"] = r
                elif k == "ev":
                    self.events.append(r)
                elif k == "act":
                    self.actions.append(r)
                elif k == "end":
                    self.end = r
                elif k == "oracle":
                    self.oracles.append(r)
        self.names = {}
        for m in self.members:
            self.names[m["g"]] = m["name"]
        for u in self.units:
            tag = f"[{u['pack']}]" if u.get("pack") else ""
            self.names[u["g"]] = f"{u.get('name') or u['g']}{tag}"
        self.names["0x0"] = "-"

    def name(self, g):
        return self.names.get(g, g)

    def member_by(self, who):
        who_l = who.lower()
        for m in self.members:
            if m["name"].lower() == who_l or m["role"] == who_l or m["g"].lower() == who_l:
                return m
        die(f"no party member '{who}' (have: {', '.join(m['name'] for m in self.members)})")

    def frame_near(self, t):
        if not self.frames:
            return None, None
        best = min(self.frames, key=lambda ft: abs(ft - t))
        return best, self.frames[best]


def sec(ms):
    return f"{ms / 1000:7.2f}s"


def print_summary(tr):
    h = tr.hdr
    print(f"{h.get('runId')}  scenario={h.get('scenario') or '-'}  sweep={h.get('sweep') or '-'}  "
          f"map={h.get('map')} instance={h.get('instance')}  seed={h.get('seed')}")
    print(f"  module {h.get('sha') or '?'}  playerbots {h.get('pbSha') or '?'}  "
          f"pullSetting={h.get('pullSetting')} releaseDelay={h.get('releaseDelayMs')}ms  goal={h.get('goal') or '-'}")
    if tr.settings:
        print("  settings: " + ", ".join(f"{s['key']}={s['v']}" for s in tr.settings))
    print(f"  end {sec(tr.end.get('t', 0)).strip()} reason={tr.end.get('reason', '?')}  "
          f"frames={len(tr.frames)} events={len(tr.events)} actions={len(tr.actions)}")
    print("\nPARTY")
    for m in tr.members:
        flags = ("leader " if m.get("leader") else "") + ("HUMAN " if m.get("human") else "")
        print(f"  {m['name']:<14} {m['role']:<5} {m.get('cls', ''):<8} {flags}{m.get('react', '')}")
    packs = {}
    for u in tr.units:
        packs.setdefault(u.get("pack") or "(none)", []).append(u)
    print("\nUNITS")
    for pack, us in packs.items():
        print(f"  {pack}: " + ", ".join(f"{u.get('name')}#{u.get('spawnId')}" for u in us[:12]) +
              (f" (+{len(us) - 12})" if len(us) > 12 else ""))
    if tr.oracles:
        print("\nORACLES")
        for o in tr.oracles:
            mark = {"fail": "FAIL", "pass": "pass", "na": " n/a"}.get(o["res"], o["res"])
            at = f"@{o['t'] / 1000:.1f}s {tr.name(o.get('g', '0x0'))}" if o["res"] == "fail" else ""
            print(f"  {o['id']:<4} {mark}  {o.get('name', ''):<24} {at}")
            if o.get("detail"):
                print(f"         {o['detail']}")
    print("\nPULL TIMELINE")
    for e in tr.events:
        if e["ev"] in ("phase", "verdict", "commit", "aggroConfirmed", "campReached", "safetyRelease",
                       "inject", "passive", "death", "engage"):
            print("  " + fmt_event(tr, e))


def fmt_event(tr, e):
    ev = e["ev"]
    a, b = tr.name(e.get("a", "0x0")), tr.name(e.get("b", "0x0"))
    s, s2, v = e.get("s", ""), e.get("s2", ""), e.get("v", 0)
    if ev == "phase":
        body = f"{s} ({s2})"
    elif ev == "verdict":
        body = f"{s} predicted={v // 1000} ceiling={v % 1000}"
    elif ev == "passive":
        body = f"{a} {'ON' if v else 'off'}"
    elif ev == "firstDmg":
        body = f"{a} -> {b} {v}"
    elif ev in ("combatOn", "engage"):
        body = f"{a} -> {b}"
    elif ev == "death":
        body = a
    else:
        body = " ".join(x for x in [a if a != "-" else "", b if b != "-" else "", s, s2] if x)
    return f"{sec(e['t'])}  {ev:<14} {body}"


def print_timeline(tr):
    for e in tr.events:
        print(fmt_event(tr, e))


def print_actions(tr, who, show_all):
    g = tr.member_by(who)["g"] if who else None
    last = None
    count = 0
    first_t = 0
    rows = []
    for a in tr.actions:
        if g and a["g"] != g:
            continue
        if not show_all and not a.get("x"):
            continue
        key = (a["g"], a["a"], a.get("eng"), a.get("x"))
        if key == last:
            count += 1
            continue
        if last:
            rows.append((first_t, last, count))
        last, count, first_t = key, 1, a["t"]
    if last:
        rows.append((first_t, last, count))
    for t, (gg, act, eng, x), n in rows:
        who_s = "" if g else f"{tr.name(gg):<12} "
        print(f"{sec(t)}  {who_s}{eng:<2} {'x' if x else '-'} {act}" + (f"  x{n}" if n > 1 else ""))


def print_frame(tr, t_ms):
    ft, fr = tr.frame_near(t_ms)
    if fr is None:
        die("trace has no frames")
    print(f"frame {sec(ft).strip()}")
    d = fr["d"]
    if d and d.get("valid", True):
        print(f"  DC {d.get('phase')} decision={d.get('decision')} seq={d.get('seq')} pred={d.get('pred')} "
              f"camp=({d.get('cx'):.1f},{d.get('cy'):.1f}) pullT={tr.name(d.get('pullT'))} "
              f"released={d.get('released')} los={d.get('los')} on={d.get('on')} paused={d.get('paused')}")
    lead = None
    for b in fr["b"]:
        if any(m["g"] == b["g"] and m.get("leader") for m in tr.members):
            lead = b
    print("  BOTS")
    for b in fr["b"]:
        dist = math.hypot(b["x"] - lead["x"], b["y"] - lead["y"]) if lead else 0
        print(f"    {tr.name(b['g']):<12} hp={b['hp']:>3} {'cbt' if b['cbt'] else '   '} eng={b['eng']:<2} "
              f"vic={tr.name(b['vic']):<16} tgt={tr.name(b['tgt']):<16} "
              f"{'PASSIVE ' if b['passive'] else ''}{'STAY ' if b['stay'] else ''}"
              f"dmg={b['dmg']} @({b['x']:.1f},{b['y']:.1f},{b['z']:.1f}) {dist:.1f}yd")
    print("  UNITS")
    for u in fr["u"]:
        if not u["alive"] and not u["cbt"]:
            continue
        dist = math.hypot(u["x"] - lead["x"], u["y"] - lead["y"]) if lead else 0
        print(f"    {tr.name(u['g']):<22} hp={u['hp']:>3} {'cbt' if u['cbt'] else '   '} "
              f"{'EVADE ' if u['evade'] else ''}{'mov ' if u['mov'] else ''}mot={u['mot']} "
              f"vic={tr.name(u['vic']):<12} thr={tr.name(u['thr']):<12} {dist:.1f}yd")


def print_oracle(tr, oid):
    for o in tr.oracles:
        if o["id"].lower() == oid.lower():
            print(f"{o['id']} {o.get('name')}: {o['res']}  n={o.get('n')}  unit={tr.name(o.get('g'))}")
            print(f"  {o.get('detail', '')}")
            if o["res"] == "fail":
                print()
                print_frame(tr, o["t"])
            return
    die(f"no oracle {oid} in this trace")


def grep(tr, pattern):
    rx = re.compile(pattern)
    hexrx = re.compile(r'"(0x[0-9A-F]+)"')
    for line in tr.raw:
        if rx.search(line):
            print(hexrx.sub(lambda m: '"' + tr.name(m.group(1)) + '"', line))


def list_traces(data_dir):
    d = trace_dir(data_dir)
    if not d.exists():
        die(f"no trace dir {d}")
    files = sorted(d.glob("*.jsonl"), key=lambda p: p.stat().st_mtime, reverse=True)[:30]
    for f in files:
        summary = ""
        with open(f) as fh:
            for line in fh:
                if '"k":"oracle"' in line and '"res":"fail"' in line:
                    summary += json.loads(line)["id"] + " "
        print(f"{f.stem:<36} fails: {summary or '-'}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace", nargs="?", help="run id (lr-/tr-...) or path to a .jsonl trace")
    ap.add_argument("--data-dir")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--timeline", action="store_true")
    ap.add_argument("--actions", nargs="?", const="", metavar="BOT")
    ap.add_argument("--all-actions", action="store_true", help="include non-executed actions")
    ap.add_argument("--frame", type=float, metavar="SECONDS")
    ap.add_argument("--oracle", metavar="ID")
    ap.add_argument("--grep", metavar="RE")
    args = ap.parse_args()

    data_dir = find_data_dir(args.data_dir)
    if args.list:
        list_traces(data_dir)
        return
    if not args.trace:
        ap.error("a trace id or path is required (or --list)")
    tr = Trace(resolve(args.trace, data_dir))
    if args.timeline:
        print_timeline(tr)
    elif args.actions is not None:
        print_actions(tr, args.actions, args.all_actions)
    elif args.frame is not None:
        print_frame(tr, int(args.frame * 1000))
    elif args.oracle:
        print_oracle(tr, args.oracle)
    elif args.grep:
        grep(tr, args.grep)
    else:
        print_summary(tr)


if __name__ == "__main__":
    main()
