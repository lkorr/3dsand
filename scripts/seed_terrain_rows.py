#!/usr/bin/env python3
"""P-G one-off (docs/PLAN_environment_truth.md), kept so it is reproducible:
the environment data that replaced the worldgen.* knobs and the shader's
hard-coded flora chain, with the values those had, so the world reads the
same by default.

  assets/worldmap/default/map.json `terrain`   the per-map terrain block
      (was worldgen.baseHeight / contAmplitude / range..grain / fbmAtten /
       spawnPlain* / sed* / treeline / refVoxelsPerMetre)
  assets/biomes/<name>.json `terrain`          {curve[9], hill, detail, grain}
      (replaces the unread `terrain.overrides`; the identity curve and 256)
  assets/biomes/<name>.json cover.plants[]     the ground-flora chain as ROWS
      with canopyMin / canopyMax conditions (forest, meadow, pine, swamp,
      tundra: the biomes with cover.groundFlora), every row's maxY at the
      treeline where it authored none (the cover stack has no treeline gate
      any more), and the alpine cushion above the treeline as a sparse row
      with minY at the treeline (was worldgen.alpineChance)
  assets/trees/<species>.json placement.minY/maxY   -1: the caps were authored
      for a world whose ground spanned y32..y86 and sat on the 20 m home
      area (env-truth finding 2); the treeline is the cap now. RE-BAKE the
      atlas afterwards (node scripts/bake_trees.mjs): the band is a header
      word of the .svtree.

Only ADDS keys that are absent (and rows whose material + conditions are not
already there); an authored value is never overwritten, so running it twice
is a no-op. Writes with the tuner's formatting (2-space indent, trailing
newline). Run from the repo root:

    python scripts/seed_terrain_rows.py
"""
import glob
import io
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TREELINE = 228          # map.json terrain.treeline, voxels
IDENTITY = [-16384, -12288, -8192, -4096, 0, 4096, 8192, 12288, 16384]

TERRAIN = {
    "about": ("The terrain, per MAP (P-G): what used to be worldgen.* in tuning.json. Lengths in "
              "voxels at refVoxelsPerMetre; log2 cells are shifts; fbmAtten / sedFraction / sedSlope "
              "are Q8 counts. baseHeight is the mean ground; a painted landform 0..255 spans "
              "landformRangeVox centred on 128; range/hill/detail/grain are the seeded octave "
              "ladder under the plane; homeArea is the calm ground around the spawn site; the "
              "sediment wedge and the treeline are what they were as knobs. Per-biome relief is "
              "each biome file's `terrain` block."),
    "refVoxelsPerMetre": 10,
    "baseHeight": 200,
    "landformRangeVox": 1024,
    "rangeAmplitude": 256, "rangeLog2": 9,
    "hillAmplitude": 64, "hillLog2": 7,
    "detailAmplitude": 16, "detailLog2": 5,
    "grainAmplitude": 4, "grainLog2": 3,
    "fbmAtten": 256,
    "homeArea": {"y": 200, "radius": 320, "fade": 2048},
    "sedCeil": 264, "sedFraction": 64, "sedStrip": 6, "sedSlope": 96, "sedMax": 32, "sedTopsoil": 4,
    "treeline": TREELINE,
}


def cond(**kw):
    c = {"minY": -1, "maxY": TREELINE - 1, "maxSlope": 1024, "nearWaterMax": -1, "nearWaterMin": 0,
         "patchThreshold": 0, "canopyMin": 0, "canopyMax": 255}
    c.update(kw)
    return c


def row(material, chance, height, head="", **kw):
    return {"material": material, "head": head, "chance": chance, "height": height, "conditions": cond(**kw)}


# The chain, in the order it rolled (first hit wins), as rows. Under the crowns
# (canopy >= 96): the shade set; at the edge (40..95): thin litter and the
# woodland-margin rose; in the gaps: the flowers. Rates are the chain's
# (worldgen.wgsl UG_* before P-G, halved on 2026-09-04).
CANOPY_ROWS = [
    row("mushroom_cluster", 24, 0.2, canopyMin=96),
    row("bramble", 46, 0.3, canopyMin=96, canopyMax=189),
    row("moss_patch", 6, 0.1, canopyMin=96, patchThreshold=150),
    row("sapling", 900, 0.5, canopyMin=96),
    row("leaf_litter", 8, 0.1, canopyMin=96),
    row("leaf_litter", 22, 0.1, canopyMin=40, canopyMax=95),
    row("flower_wildrose", 900, 0.4, canopyMin=40, canopyMax=95),
]
GAP_FLOWERS = [
    row("flower_foxglove", 900, 0.5, canopyMax=95, patchThreshold=180),
    row("flower_buttercup", 700, 0.3, canopyMax=95, patchThreshold=180),
]
ALPINE_ABOVE = row("alpine_cushion", 80, 0.1, minY=TREELINE, maxY=-1, patchThreshold=96)


def same_row(a, b):
    return a.get("material") == b.get("material") and a.get("conditions", {}) == b.get("conditions", {})


def seed_biome(path):
    with io.open(path, encoding="utf-8") as f:
        j = json.load(f)
    changed = []
    te = j.get("terrain")
    if not isinstance(te, dict) or "curve" not in te:
        j["terrain"] = {"curve": list(IDENTITY), "hill": 256, "detail": 256, "grain": 256}
        changed.append("terrain")
    cover = j.setdefault("cover", {})
    plants = cover.setdefault("plants", [])
    name = j.get("name", os.path.splitext(os.path.basename(path))[0])
    # every row's conditions carry the canopy pair; a row with no ceiling
    # stops at the treeline, as the stack used to
    for r in plants:
        c = r.setdefault("conditions", {})
        if "canopyMin" not in c:
            c["canopyMin"] = 0; c["canopyMax"] = 255; changed.append(r["material"] + ".canopy")
        if c.get("maxY", -1) < 0 and not (r["material"] == "alpine_cushion" and c.get("minY", -1) >= TREELINE):
            c["maxY"] = TREELINE - 1; changed.append(r["material"] + ".maxY")
    ground_flora = cover.get("groundFlora", True) is not False
    if ground_flora:
        add = list(CANOPY_ROWS)
        if name == "meadow":
            # the meadow's own flower rows exist; its tall grass grows a head
            for r in plants:
                if r["material"] == "tall_grass" and not r.get("head"):
                    r["head"] = "tall_grass_head"; r["height"] = max(float(r.get("height", 0.3)), 0.6)
                    changed.append("tall_grass.head")
        else:
            add = add + list(GAP_FLOWERS)
        new = [r for r in add if not any(same_row(r, p) for p in plants)]
        if new:
            plants[0:0] = new          # BEFORE the biome's own rows: the chain rolled first
            changed.append("%d chain rows" % len(new))
    if name in ("alpine", "tundra") and not any(same_row(ALPINE_ABOVE, p) for p in plants):
        plants.append(dict(ALPINE_ABOVE))
        changed.append("alpine_cushion above the treeline")
    if changed:
        with io.open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(j, f, indent=2, ensure_ascii=False)
            f.write("\n")
    return changed


def seed_species(path):
    with io.open(path, encoding="utf-8") as f:
        j = json.load(f)
    pl = j.setdefault("placement", {})
    changed = []
    for k in ("minY", "maxY"):
        if pl.get(k, -1) != -1:
            pl[k] = -1; changed.append(k)
    if changed:
        with io.open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(j, f, indent=2, ensure_ascii=False)
            f.write("\n")
    return changed


def seed_map(path):
    with io.open(path, encoding="utf-8") as f:
        j = json.load(f)
    if "terrain" in j:
        return []
    out = {}
    for k, v in j.items():
        out[k] = v
        if k == "warpAmpVox":
            out["terrain"] = dict(TERRAIN)
    with io.open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(out, f, indent=2, ensure_ascii=False)
        f.write("\n")
    return ["terrain"]


if __name__ == "__main__":
    for p in sorted(glob.glob(os.path.join(ROOT, "assets", "biomes", "*.json"))):
        if os.path.basename(p).startswith("_"):
            continue
        ch = seed_biome(p)
        print(os.path.relpath(p, ROOT), ", ".join(ch) if ch else "(unchanged)")
    for p in sorted(glob.glob(os.path.join(ROOT, "assets", "trees", "*.json"))):
        if os.path.basename(p).startswith("_"):
            continue
        ch = seed_species(p)
        print(os.path.relpath(p, ROOT), ", ".join(ch) if ch else "(unchanged)")
    for p in sorted(glob.glob(os.path.join(ROOT, "assets", "worldmap", "*", "map.json"))):
        ch = seed_map(p)
        print(os.path.relpath(p, ROOT), ", ".join(ch) if ch else "(unchanged)")
