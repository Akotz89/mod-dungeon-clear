#!/usr/bin/env python3
# Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3.
"""
gen_navmap.py - render a map's walkable navmesh (Detour mmtiles) to a top-down
PNG that is aligned exactly to world coordinates.

The image is drawn from the same geometry the server paths on, so walls,
pillars and drops show up as gaps, and the render transform IS the world->pixel
calibration: no fitting, no landmarks. The affine is printed on stdout.

Multi-level maps (caverns, lakes, towers) flatten into one blob top-down; use
--z to keep only the navmesh in a height band, and --style height to tell
levels apart.

OFFLINE DEV TOOL - not part of the running module. Reads the server's extracted
mmaps/ directory only and writes the PNG you name. Needs Pillow (pip install
pillow). The rendered geometry is client-derived; don't commit the images.

Examples:
  # Whole map, flat silhouette.
  python3 tools/gen_navmap.py 389 rfc.png --mmaps ~/server/data/mmaps

  # Lady Vashj's platform in SSC: 90yd crop, only z 15..55, shaded by height,
  # with Vashj and two shield generators marked.
  python3 tools/gen_navmap.py 548 vashj.png --center=30,-922.4 --radius 90 \\
      --z=15,55 --style height --scale 3 \\
      --mark=30,-922.4,Vashj --mark=52,-901.2 --mark=7.8,-945.2

World->pixel (S = --scale, minX/maxY = the image's world bounds):
  px = pad + (maxY - y) * S        py = pad + (maxX - x) * S
North (+X) is up, west (+Y) is left - the same orientation as the in-game map.
"""
import argparse
import json
import os
import re
import sys

try:
    from PIL import Image, ImageDraw, ImageFilter
except ImportError:
    sys.exit("gen_navmap.py needs Pillow: pip install pillow")

import struct

# Detour stores (x, y, z) with y up; WoW is (x, y, z) with z up.
# Recast->WoW: wowX = recast.z, wowY = recast.x, wowZ = recast.y.
DT_MAGIC = b"VAND"            # dtMeshHeader magic 'DNAV', little-endian
DT_MESH_HEADER_SIZE = 100
DT_POLY_SIZE = 32
DT_POLYTYPE_OFFMESH = 1

# AzerothCore NavTerrainFlag (src/common/Collision/Maps/MapDefines.h).
NAV_GROUND, NAV_MAGMA, NAV_SLIME, NAV_WATER = 0x01, 0x02, 0x04, 0x08

GROUND = (58, 74, 96)
WATER = (40, 90, 200)
LIQUID = (190, 80, 40)        # magma / slime
BACKGROUND = (10, 12, 18)
MARK = (255, 60, 60)


def parse_tile(path):
    """Yield (corners, area) per walkable poly; corners are WoW (x, y, z)."""
    with open(path, "rb") as f:
        data = f.read()
    # The MmapTileHeader in front of the Detour data has changed size across
    # core versions, so locate the dtMeshHeader by its magic.
    h = data.find(DT_MAGIC)
    if h < 0:
        return
    poly_count, vert_count = struct.unpack_from("<2i", data, h + 24)
    off = h + DT_MESH_HEADER_SIZE
    v = struct.unpack_from("<%df" % (vert_count * 3), data, off)
    off += vert_count * 12
    for i in range(poly_count):
        base = off + i * DT_POLY_SIZE
        idx = struct.unpack_from("<6H", data, base + 4)
        nverts, area_and_type = data[base + 30], data[base + 31]
        if area_and_type >> 6 == DT_POLYTYPE_OFFMESH:
            continue
        corners = [(v[k * 3 + 2], v[k * 3], v[k * 3 + 1]) for k in idx[:nverts]]
        yield corners, area_and_type & 0x3F


def tile_files(mmaps, map_id):
    # Tiles are <map:%03d><tileY:%02d><tileX:%02d>.mmtile. Match the full
    # name: a bare "43*" prefix glob would also pick up map 430's tiles.
    pat = re.compile(r"^%03d\d{4}\.mmtile$" % map_id)
    return sorted(os.path.join(mmaps, n) for n in os.listdir(mmaps) if pat.match(n))


def parse_pair(s, what):
    try:
        a, b = (float(t) for t in s.split(","))
    except ValueError:
        sys.exit("bad %s %r: expected two comma-separated numbers" % (what, s))
    return a, b


def parse_mark(s):
    parts = s.split(",", 2)
    try:
        x, y = float(parts[0]), float(parts[1])
    except (ValueError, IndexError):
        sys.exit("bad --mark %r: expected X,Y[,label]" % s)
    return x, y, parts[2] if len(parts) > 2 else ""


def height_shade(z, zlo, zhi):
    t = 0.0 if zhi <= zlo else max(0.0, min(1.0, (z - zlo) / (zhi - zlo)))
    return (int(70 + 170 * t), int(80 + 140 * t), int(70 + 30 * t))


def main():
    ap = argparse.ArgumentParser(
        description="Render a map's navmesh to a world-aligned top-down PNG.",
        epilog="Negative values must use '=': --center=-239.8,-366.5  --z=-20,10",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("map", type=int, help="map id, e.g. 389 (Ragefire Chasm)")
    ap.add_argument("out", help="output PNG path")
    ap.add_argument("--mmaps", default=os.environ.get("AC_MMAPS_DIR"),
                    help="server mmaps/ directory (default: $AC_MMAPS_DIR)")
    ap.add_argument("--scale", type=float, default=1.6,
                    help="pixels per yard (default 1.6)")
    ap.add_argument("--pad", type=int, default=16,
                    help="border in pixels (default 16)")
    ap.add_argument("--center", help="crop to a square around world X,Y")
    ap.add_argument("--radius", type=float, default=100.0,
                    help="half-width of the --center crop, yards (default 100)")
    ap.add_argument("--z", help="only polys whose mean height is in LO,HI")
    ap.add_argument("--style", choices=("flat", "height"), default="flat",
                    help="flat silhouette, or shade ground by height "
                         "(light = high) with poly outlines")
    ap.add_argument("--mark", action="append", default=[], metavar="X,Y[,LABEL]",
                    help="draw a marker at world X,Y (repeatable)")
    ap.add_argument("--json", action="store_true",
                    help="print only the calibration JSON")
    a = ap.parse_args()

    if not a.mmaps:
        sys.exit("pass --mmaps <dir> or set AC_MMAPS_DIR")
    if not os.path.isdir(a.mmaps):
        sys.exit("not a directory: %s" % a.mmaps)
    files = tile_files(a.mmaps, a.map)
    if not files:
        sys.exit("no mmtiles for map %d in %s" % (a.map, a.mmaps))

    zband = parse_pair(a.z, "--z") if a.z else None
    center = parse_pair(a.center, "--center") if a.center else None
    marks = [parse_mark(m) for m in a.mark]

    polys = []
    for path in files:
        for corners, area in parse_tile(path):
            n = len(corners)
            cx = sum(c[0] for c in corners) / n
            cy = sum(c[1] for c in corners) / n
            cz = sum(c[2] for c in corners) / n
            if zband and not zband[0] <= cz <= zband[1]:
                continue
            if center and (abs(cx - center[0]) > a.radius + 50
                           or abs(cy - center[1]) > a.radius + 50):
                continue
            polys.append((corners, area, cz))
    if not polys:
        sys.exit("no navmesh left after filtering")

    if center:
        minx, maxx = center[0] - a.radius, center[0] + a.radius
        miny, maxy = center[1] - a.radius, center[1] + a.radius
    else:
        xs = [c[0] for p in polys for c in p[0]]
        ys = [c[1] for p in polys for c in p[0]]
        minx, maxx, miny, maxy = min(xs), max(xs), min(ys), max(ys)

    S, pad = a.scale, a.pad
    W = int(pad * 2 + (maxy - miny) * S)
    H = int(pad * 2 + (maxx - minx) * S)

    def w2p(x, y):
        return (pad + (maxy - y) * S, pad + (maxx - x) * S)

    zs = [p[2] for p in polys]
    zlo, zhi = zband if zband else (min(zs), max(zs))

    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    dr = ImageDraw.Draw(layer)
    # Low to high, so upper levels paint over what lies beneath them.
    for corners, area, cz in sorted(polys, key=lambda p: p[2]):
        if area & NAV_WATER:
            fill = WATER
        elif area & (NAV_MAGMA | NAV_SLIME):
            fill = LIQUID
        elif a.style == "height":
            fill = height_shade(cz, zlo, zhi)
        else:
            fill = GROUND
        outline = (0, 0, 0, 255) if a.style == "height" else None
        dr.polygon([w2p(c[0], c[1]) for c in corners], fill=fill + (255,),
                   outline=outline)

    img = Image.new("RGBA", (W, H), BACKGROUND + (255,))
    if a.style == "flat":
        glow = layer.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.GaussianBlur(2))
        img.alpha_composite(glow)
    img.alpha_composite(layer)

    dr = ImageDraw.Draw(img)
    for x, y, label in marks:
        px, py = w2p(x, y)
        dr.ellipse([px - 5, py - 5, px + 5, py + 5], fill=MARK, outline=(0, 0, 0))
        if label:
            dr.text((px + 8, py - 6), label, fill=(255, 255, 255))

    img.convert("RGB").save(a.out)

    cal = {"map": a.map, "w": W, "h": H, "scale": S, "pad": pad,
           "minX": minx, "maxX": maxx, "minY": miny, "maxY": maxy,
           # px = ax*x + bx*y + cx ; py = ay*x + by*y + cy
           "ax": 0.0, "bx": -S, "cx": pad + maxy * S,
           "ay": -S, "by": 0.0, "cy": pad + maxx * S}
    if a.json:
        print(json.dumps(cal))
        return
    print("saved %s (%dx%d) from %d tiles, %d polys"
          % (a.out, W, H, len(files), len(polys)))
    print("world X %.1f..%.1f (bottom..top), Y %.1f..%.1f (right..left)"
          % (minx, maxx, miny, maxy))
    print("calibration: " + json.dumps(cal))


if __name__ == "__main__":
    main()
