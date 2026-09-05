#!/usr/bin/env python3
"""Emit the TREE_TILE / TREE_SCAN / TREE_CAND_MAX prelude constants.

Mirrors the engine's derivation (src/sim/treeatlas.h TreeLatticeFor, fed by
src/sim/biomes.cpp FinestTreeTileVox) so scripts/check_shaders.sh assembles
each shader with the same three numbers ShaderConstantPrelude() emits at
runtime. Nothing here is a second authority: the biome files and the baked
atlases are the inputs on both sides, and the arithmetic is copied line for
line from the header it names.

    python scripts/tree_lattice.py --vpm 10        # the three `const` lines
    python scripts/tree_lattice.py --vpm 10 --json # the numbers, for tools

The .svtree header word 8 is the species' measured reach (treeatlas.cpp
ParseFile); the finest tile is the smallest `trees.tile` among biomes with
`trees.density > 0` and at least one species row, floored at 16 voxels and
defaulting to 144 when no biome grows trees at all.
"""
import argparse
import glob
import json
import math
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SVTREE_MAGIC = 0x52545653  # 'SVTR'
TILE_FLOOR_VOX = 16        # biomes.h kTreeTileFloorVox
TILE_DEFAULT_VOX = 144     # biomes.h kTreeTileDefaultVox
CAND_PER_AXIS_CAP = 5      # treeatlas.h kTreeCandPerAxisCap


def finest_tile_vox(asset_dir, vpm):
    best = 0
    for p in sorted(glob.glob(os.path.join(asset_dir, "biomes", "*.json"))):
        if os.path.basename(p).startswith("_"):
            continue
        with open(p, encoding="utf-8") as f:
            j = json.load(f)
        tr = j.get("trees", {}) if isinstance(j, dict) else {}
        if int(tr.get("density", 0)) <= 0 or not tr.get("species"):
            continue
        # lround: half away from zero, as worldmap.cpp's Vox() rounds.
        t = max(TILE_FLOOR_VOX, int(math.floor(float(tr.get("tile", 14.4)) * vpm + 0.5)))
        if best == 0 or t < best:
            best = t
    return best or TILE_DEFAULT_VOX


def max_reach(asset_dir):
    reach = 0
    for p in sorted(glob.glob(os.path.join(asset_dir, "trees", "*.svtree"))):
        with open(p, "rb") as f:
            hdr = f.read(9 * 4)
        if len(hdr) < 9 * 4:
            continue
        words = struct.unpack("<9I", hdr)
        if words[0] != SVTREE_MAGIC:
            continue
        reach = max(reach, words[8])
    return reach


def lattice_for(tile, reach):
    """treeatlas.h TreeLatticeFor, verbatim in integer arithmetic."""
    inset, span = tile // 4, tile // 2
    scan = (reach + inset + span - 1) // tile
    per_axis = (2 * reach + span - 1) // tile + 1
    side = 2 * scan + 1
    return {"tile": tile, "scan": scan, "perAxis": per_axis,
            "candMax": min(per_axis * per_axis, side * side), "maxReach": reach}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vpm", type=int, default=10, help="voxels per metre (world.h)")
    ap.add_argument("--assets", default=os.path.join(ROOT, "assets"))
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    lat = lattice_for(finest_tile_vox(a.assets, a.vpm), max_reach(a.assets))
    if lat["perAxis"] > CAND_PER_AXIS_CAP:
        print(f"tree_lattice: reach {lat['maxReach']} overflows the {CAND_PER_AXIS_CAP}x"
              f"{CAND_PER_AXIS_CAP} candidate cap at tile {lat['tile']} -- the engine "
              f"refuses this atlas (treeatlas.cpp)", file=sys.stderr)
    if a.json:
        print(json.dumps(lat))
        return 0
    print(f"const TREE_TILE : i32 = {lat['tile']};")
    print(f"const TREE_SCAN : i32 = {lat['scan']};")
    print(f"const TREE_CAND_MAX : i32 = {lat['candMax']};")
    return 0


if __name__ == "__main__":
    sys.exit(main())
