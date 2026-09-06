#!/usr/bin/env python3
"""The pond lattice, for scripts/check_shaders.sh.

worldgen's rolled ponds sit on ONE lattice of POND_TILE voxels: the finest
`water.features[].tile` among the biomes that author a row that can roll (a
preset file that exists, rarity > 0, tile > 0), with a floor of 64 voxels,
or 0 when no biome does. The engine derives it at load (src/sim/worldmap.cpp
PondLatticeVox) and emits it as a prelude constant (gpu/resources.cpp
ShaderConstantPrelude); this mirrors that arithmetic from the assets so the
offline validator compiles worldgen.wgsl against the same number.

    python scripts/pond_lattice.py --vpm 10        # `const POND_TILE : i32 = N;`
"""
import argparse
import glob
import json
import math
import os

TILE_FLOOR_VOX = 64   # worldmap.cpp PondLatticeVox


def pond_lattice_vox(asset_dir, vpm):
    presets = {os.path.splitext(os.path.basename(p))[0]
               for p in glob.glob(os.path.join(asset_dir, "water", "*.json"))
               if not os.path.basename(p).startswith("_")}
    finest = 0
    for p in sorted(glob.glob(os.path.join(asset_dir, "biomes", "*.json"))):
        if os.path.basename(p).startswith("_"):
            continue
        with open(p, encoding="utf-8") as f:
            j = json.load(f)
        rows = (j.get("water", {}) or {}).get("features", []) if isinstance(j, dict) else []
        for r in rows or []:
            if not isinstance(r, dict):
                continue
            tile_m = float(r.get("tile", 0) or 0)
            if int(r.get("rarity", 0) or 0) <= 0 or tile_m <= 0 or r.get("preset") not in presets:
                continue
            # lround: half away from zero, as worldmap.cpp rounds.
            t = int(math.floor(tile_m * vpm + 0.5))
            if t <= 0:
                continue
            finest = t if finest == 0 else min(finest, t)
    return 0 if finest == 0 else max(finest, TILE_FLOOR_VOX)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vpm", type=int, required=True, help="voxels per metre (world.h kVoxelsPerMetre)")
    ap.add_argument("--assets", default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "assets"))
    a = ap.parse_args()
    print("const POND_TILE : i32 = %d;" % pond_lattice_vox(a.assets, a.vpm))


if __name__ == "__main__":
    main()
