#!/usr/bin/env python3
"""The map's reference scale, for scripts/check_shaders.sh.

worldgen.wgsl's vlen() divides its hardcoded lengths by REF_VOXELS_PER_METRE
in module-scope consts, so the engine emits it as a prelude constant
(gpu/resources.cpp ShaderConstantPrelude) from the loaded map's
`terrain.refVoxelsPerMetre` (src/sim/worldmap.cpp LoadWorldMap; P-G of
docs/PLAN_environment_truth.md). This mirrors that read from the assets --
the map tuning.json's `world.mapLayer` names -- so the offline validator
compiles worldgen.wgsl against the same number.

    python scripts/map_terrain.py        # `const REF_VOXELS_PER_METRE : i32 = N;`
"""
import argparse
import json
import os


def ref_voxels_per_metre(asset_dir):
    name = "default"
    try:
        with open(os.path.join(asset_dir, "materials", "tuning.json"), encoding="utf-8") as f:
            name = ((json.load(f).get("world") or {}).get("mapLayer") or "default")
    except (OSError, ValueError):
        pass
    ref = 10
    try:
        with open(os.path.join(asset_dir, "worldmap", name, "map.json"), encoding="utf-8") as f:
            ref = int(((json.load(f).get("terrain") or {}).get("refVoxelsPerMetre") or 10))
    except (OSError, ValueError):
        pass
    return max(1, ref)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--assets", default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "assets"))
    a = ap.parse_args()
    print("const REF_VOXELS_PER_METRE : i32 = %d;" % ref_voxels_per_metre(a.assets))


if __name__ == "__main__":
    main()
