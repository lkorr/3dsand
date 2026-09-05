#!/usr/bin/env python3
"""P-E one-off, kept so it is reproducible: add the per-biome flora keys that
replaced the worldgen.* knobs, with the values those knobs had, so the world
looks the same by default.

  caves.features[near_surface].mushroomChance = 26   (was worldgen.caveMushroomChance)
  caves.features[deep].crystalChance         = 9    (was worldgen.caveCrystalChance)
  cover.cactusChance                         = 26   (was worldgen.cactusChance, percent of tiles)
  cover.saguaroFraction                      = 22   (was worldgen.saguaroFraction, percent of cacti)

Only ADDS keys that are absent; an authored value is never overwritten, so
running it twice is a no-op and it is safe beside someone else's edits to the
same files. Writes with the tuner's formatting (2-space indent, trailing
newline). Run from the repo root:

    python scripts/seed_flora_rows.py
"""
import glob
import io
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULTS = {"mushroom": 26, "crystal": 9, "cactus": 26, "saguaro": 22}


def seed(path):
    with io.open(path, encoding="utf-8") as f:
        j = json.load(f)
    changed = []
    cover = j.setdefault("cover", {})
    if "cactusChance" not in cover:
        cover["cactusChance"] = DEFAULTS["cactus"]; changed.append("cover.cactusChance")
    if "saguaroFraction" not in cover:
        cover["saguaroFraction"] = DEFAULTS["saguaro"]; changed.append("cover.saguaroFraction")
    for row in j.get("caves", {}).get("features", []):
        if row.get("preset") == "near_surface" and "mushroomChance" not in row:
            row["mushroomChance"] = DEFAULTS["mushroom"]; changed.append("caves.near_surface.mushroomChance")
        if row.get("preset") == "deep" and "crystalChance" not in row:
            row["crystalChance"] = DEFAULTS["crystal"]; changed.append("caves.deep.crystalChance")
    if changed:
        with io.open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(j, f, indent=2, ensure_ascii=False)
            f.write("\n")
    return changed


if __name__ == "__main__":
    for p in sorted(glob.glob(os.path.join(ROOT, "assets", "biomes", "*.json"))):
        if os.path.basename(p).startswith("_"):
            continue
        c = seed(p)
        print("%s: %s" % (os.path.relpath(p, ROOT), ", ".join(c) if c else "already seeded"))
