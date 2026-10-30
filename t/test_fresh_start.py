#!/usr/bin/env python3
"""Compile the real start paths against small engine doubles."""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile

if not __debug__:
    raise SystemExit('Run without Python optimization; negative-control assertions are required')

repo = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--build-guard', type=Path, required=True)
p.add_argument('--work-dir', type=Path, required=True)
args = p.parse_args()
subprocess.run([str(args.build_guard)], check=True)
source = (repo / 'src/Ai/Dungeon/DungeonClear/Action/DungeonClearChatActions.cpp').read_text()

def extract(signature, source=source):
    found = re.search(re.escape(signature) + r'[^;{}]*\{', source)
    if not found:
        raise RuntimeError('Missing source function: ' + signature)
    end, depth = found.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[found.start():end]

wing_source = (repo / 'src/Ai/Dungeon/DungeonClear/Util/DcRunWing.cpp').read_text()
template = (repo / 't/fresh_start_fixture.cpp.in').read_text()
for marker, signature in [('// CAPTURE_BODY', 'DcRunWing::Snapshot DcRunWing::Capture'), ('// RESTORE_BODY', 'void DcRunWing::Restore')]:
    template = template.replace(marker, extract(signature, wing_source))
for marker, signature in [('// ENABLE_BODY', 'bool EnableClearOnLeader'), ('// ON_BODY', 'bool DcOnAction::Execute')]:
    template = template.replace(marker, extract(signature))
args.work_dir.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(dir=args.work_dir) as tmp:
    cpp, binary = Path(tmp) / 'fixture.cpp', Path(tmp) / 'fixture'
    cpp.write_text(template)
    subprocess.run(['c++', '-std=c++20', '-I', str(repo / 't/fresh_start_stubs'), '-I', str(repo / 'src'), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    assert template.count('DcRun::Of(context).Reset();') == 2
    cpp.write_text(template.replace('DcRun::Of(context).Reset();', 'DcRun::Of(context).OnResume();'))
    subprocess.run(['c++', '-std=c++20', '-I', str(repo / 't/fresh_start_stubs'), '-I', str(repo / 'src'), str(cpp), '-o', str(binary)], check=True)
    assert subprocess.run([str(binary)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0, 'negative control did not detect the missing reset'
    assert template.count('RefreshPartyBossLists(snapshot.holder);') == 1
    cpp.write_text(template.replace('RefreshPartyBossLists(snapshot.holder);', '', 1))
    subprocess.run(['c++', '-std=c++20', '-I', str(repo / 't/fresh_start_stubs'), '-I', str(repo / 'src'), str(cpp), '-o', str(binary)], check=True)
    assert subprocess.run([str(binary)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0, 'negative control did not detect missing cache refresh'
print('Fresh-start paths passed')
