#!/usr/bin/env python3
"""Exercise the actual nearest-visible selector with small geometry doubles."""
from pathlib import Path
import subprocess
import tempfile


def definition(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


module = Path(__file__).resolve().parents[1]
guard = next((p / 'scripts/wow-build-host-guard' for p in module.parents
              if (p / 'scripts/wow-build-host-guard').is_file()), None)
if guard is None:
    raise SystemExit('WoW build-host guard not found in an ancestor')
subprocess.run([str(guard)], check=True)
source = (module / 'src/Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.cpp').read_text()
names = ('std::optional<G3D::Vector3> PointAt(', 'struct FlatIndex',
         'bool StepFlatForward(', 'float Dist3DSq(', 'bool BotCanSee(',
         'bool PickNearestVisibleForward(')
parts = [definition(source, name) for name in names]
parts[1] += ';'
preamble = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>
using uint32 = std::uint32_t; using uint64 = std::uint64_t;
namespace G3D { struct Vector3 { float x, y, z; }; }
struct Segment { std::vector<G3D::Vector3> polyline; };
struct ChunkedPathfinder { struct Result { std::vector<Segment> segments; }; };
struct DungeonFollowerState { uint32 segmentIdx = 0, pointIdx = 0, offPathTicks = 0; };
struct DungeonPathFollower { static constexpr float RESNAP_RADIUS = 45;
    static constexpr size_t RESNAP_WINDOW = 24; };
namespace VMAP { enum class ModelIgnoreFlags { Nothing }; }
constexpr int LINEOFSIGHT_CHECK_VMAP = 1; constexpr float RESNAP_LOS_Z_BUMP = 1.5f;
struct Map { bool isInLineOfSight(float,float,float,float,float,float,uint32,int,VMAP::ModelIgnoreFlags) const { return true; } };
struct Player { float x = 0; Map map; float GetPositionX() const { return x; }
    float GetPositionY() const { return 0; } float GetPositionZ() const { return 0; }
    Map* GetMap() { return &map; } uint32 GetPhaseMask() const { return 1; } };
'''
tests = r'''
int main() {
    Player bot; DungeonFollowerState state;
    float step = std::nextafter(100.0f, 101.0f) - 100.0f;
    ChunkedPathfinder::Result path{{{{{10,0,0},{0,10,std::sqrt(step)},
                                      {-10,0,std::sqrt(2*step)}}}}};
    assert(PickNearestVisibleForward(&bot, path, {0,0}, 24, state));
    assert(state.pointIdx == 0);
    path.segments[0].polyline = {{10,0,0},{0,10,0}}; state = {};
    assert(PickNearestVisibleForward(&bot, path, {0,0}, 24, state));
    assert(state.pointIdx == 1);
    float nan = std::numeric_limits<float>::quiet_NaN();
    float inf = std::numeric_limits<float>::infinity();
    path.segments[0].polyline = {{nan,0,0},{inf,0,0}}; state = {};
    assert(!PickNearestVisibleForward(&bot, path, {0,0}, 24, state));
    path.segments[0].polyline = {{nan,0,0},{10,0,0}}; state = {};
    assert(PickNearestVisibleForward(&bot, path, {0,0}, 24, state));
    assert(state.pointIdx == 1);
}
'''
with tempfile.TemporaryDirectory(dir=module) as directory:
    cpp = Path(directory) / 'resnap.cpp'
    executable = Path(directory) / 'resnap'
    cpp.write_text(preamble + '\n'.join(parts) + tests)
    subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra',
                    str(cpp), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print('Resnap candidate ordering checks passed')
