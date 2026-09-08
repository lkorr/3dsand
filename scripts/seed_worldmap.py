#!/usr/bin/env python3
"""Write the shipped default world map: assets/worldmap/default/{map.json,map.svmap}.

This is a STARTING map for the World Map tab to repaint, not the noise it
replaces: Tier A of docs/PLAN_world_map.md is seed-INDEPENDENT, so the layout
below is drawn once, here, from a fixed constant, and every world seed then
fills the same regions differently (the boundary warp and everything inside
a region use the world seed in the shader).

The layout is the one the design docs already assumed -- tundra to the north
(+Z), desert to the east (+X), an alpine ridge to the north-west, a swamp in
the south-east lowland, pine between the forest and the cold, meadows as
clearings, forest everywhere else, an ocean ring outside the painted disc.
The cells around the origin are forced FOREST so the selftest fixtures keep
the ground they were written against.

Format (src/sim/worldmap.h is the authority):
  map.json   name, cellLog2, size [w,h], originCell [cx,cz] (the plane cell
             whose low corner is world (0,0)), seaLevelY, oceanFadeCells,
             warpAmpVox, biomes[] (plane byte -> biome NAME), sites[], rules[]
  map.svmap  'SVMP' u32, version u32 (1), w u32, h u32, then three u8 planes
             of w*h cells, z-major (index = cz*w + cx): biome (index into
             map.json biomes[]), landform (0 ocean floor .. 255 alpine),
             moisture (reserved, 0).
"""
import json, math, pathlib, struct, sys

ROOT = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / 'assets' / 'worldmap' / 'default'
W = H = 196            # 196 cells x 102.4 m = 20 km
CELL_LOG2 = 10
ORIGIN = (98, 98)      # the world origin at the centre; spawn is a site (below)
RADIUS = 88            # painted disc, cells; ocean outside
FADE = 6
BIOMES = ['forest', 'meadow', 'pine', 'desert', 'tundra', 'swamp', 'alpine', 'ocean']
B = {n: i for i, n in enumerate(BIOMES)}

def hash2(x, y, salt):
    h = (x * 374761393 + y * 668265263 + salt * 2246822519) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return (h ^ (h >> 16)) & 0xFFFFFFFF

def vnoise(x, y, cell, salt):
    """Value noise in 0..1 at cell size `cell`, fixed salt: the authoring pen."""
    gx, gy = math.floor(x / cell), math.floor(y / cell)
    fx, fy = x / cell - gx, y / cell - gy
    sx, sy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    def c(i, j): return hash2(gx + i, gy + j, salt) / 0xFFFFFFFF
    a = c(0, 0) + (c(1, 0) - c(0, 0)) * sx
    b = c(0, 1) + (c(1, 1) - c(0, 1)) * sx
    return a + (b - a) * sy

def fbm(x, y, cell, salt):
    return (vnoise(x, y, cell, salt) * 0.6 + vnoise(x, y, cell / 2, salt + 1) * 0.3 +
            vnoise(x, y, cell / 4, salt + 2) * 0.1)

biome = bytearray(W * H)
land = bytearray(W * H)
moist = bytearray(W * H)
for cz in range(H):
    for cx in range(W):
        dx, dz = cx - ORIGIN[0], cz - ORIGIN[1]
        d = math.hypot(dx, dz)
        # coast: a warped disc so the shoreline is not a circle
        coast = RADIUS + (fbm(cx, cz, 24, 11) - 0.5) * 14
        i = cz * W + cx
        if d > coast:
            biome[i] = B['ocean']
            # landform fades to the sea floor over FADE cells past the coast
            t = min(1.0, (d - coast) / FADE)
            land[i] = int(round(40 * (1 - t)))
            continue
        north = dz / RADIUS          # +1 at the north coast
        east = dx / RADIUS
        n1 = fbm(cx, cz, 18, 21) - 0.5   # +-0.5 regional wobble
        n2 = fbm(cx, cz, 9, 31) - 0.5
        b = B['forest']
        relief = 134
        if north + n1 * 0.6 > 0.42:
            b = B['tundra']; relief = 144
        elif east + n1 * 0.5 > 0.45:
            b = B['desert']; relief = 136
        elif dx < -8 and north > 0.10 and north + n2 * 0.8 < 0.55 and (-east) + n1 * 0.7 > 0.30:
            b = B['alpine']; relief = 225
        elif east + n2 * 0.6 > 0.12 and north + n1 * 0.6 < -0.28:
            b = B['swamp']; relief = 116
        elif north + n2 * 0.5 > 0.22:
            b = B['pine']; relief = 154
        elif n2 > 0.27:
            b = B['meadow']; relief = 134
        # LANDFORM UNITS: contAmplitude 1024 makes one unit 4 voxels (0.4 m) about
        # 128, and the finer octaves dip up to ~84 voxels, so land must sit >= 132
        # to stay above seaLevelY 112 everywhere (swamp at 116 floods in its lows
        # on purpose). Ocean <= 40 is 350+ voxels down.
        # the harness region: today's fixtures were written against this ground
        if abs(dx) <= 2 and abs(dz) <= 2:
            b = B['forest']; relief = 134
        relief += int(round((fbm(cx, cz, 6, 41) - 0.5) * 8))
        biome[i] = b
        land[i] = max(0, min(255, relief))

OUT.mkdir(parents=True, exist_ok=True)
meta = {
    "name": "default",
    "about": "The shipped starting map (scripts/seed_worldmap.py). Repaint it in the tuner's World Map tab. Tier A: seed-independent.",
    "cellLog2": CELL_LOG2,
    "size": [W, H],
    "originCell": list(ORIGIN),
    "seaLevelY": 112,
    "oceanFadeCells": FADE,
    "warpAmpVox": 160,
    # The TERRAIN, per map (PLAN_environment_truth P-G): every number that used
    # to be a worldgen.* knob in tuning.json. src/sim/worldmap.h kHTerrain* is
    # the packed form; scripts/seed_terrain_rows.py carries the same defaults.
    "terrain": {
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
        "treeline": 228,
    },
    "biomes": BIOMES,
    "sites": [
        {"id": "harness", "kind": "pad", "min": [-128, -128], "max": [640, 640],
         "about": "The selftest harness region: no tree trunks or crowns, no tarns, no cover. Keeps every fixture column (60..150 on the x==z diagonal) and the test tarn at (420,420) on the ground the gates were written against."},
        # The player does NOT start on the pad (PLAN_environment_truth P-C):
        # the pad refuses trees and cover, so spawn sits past its edge plus
        # the widest crown reach, still inside the forced-forest cells.
        {"id": "spawn", "kind": "spawn", "at": [900, 900],
         "about": "Where the game starts, and the centre of the calm home area (worldgen.spawnPlain*). Outside the harness pad by more than the widest crown reach (115 vox past x/z 640), on forced forest, on land: the spawn-site gate checks all of that."},
        # An AUTHORED lake (PLAN_environment_truth P-F): Tier A, the same place
        # on every seed, wearing the spawn_lake preset's geometry. East of
        # spawn by more than its radius + shore band.
        {"id": "home_lake", "kind": "water", "preset": "spawn_lake", "at": [1240, 900],
         "about": "An AUTHORED lake (P-F): same place on every seed, the spawn_lake preset's geometry. East of spawn by more than its radius + shore band, so the spawn-site gate sees it as near-but-dry ground."},
        # A DECLARED landform (PLAN_environment_truth P-G): "there is always a
        # mountain to the east". Overlaid onto the landform plane at load;
        # Tier A, the same on every seed. 4.5 km east of the origin, a
        # north-south crest 1.6 km long and 60 m high, well clear of the
        # harness pad and the spawn.
        {"id": "east_range", "kind": "landform", "shape": "ridge", "at": [45000, 0],
         "radius": 8000, "heightVox": 600, "rotation": 90,
         "about": "The mountain to the east (P-G): a ridge overlaid onto the landform plane at load, the same on every seed. radius is the crest's half-length in voxels (its width is a third of that), heightVox how far the plane is lifted at the crest, rotation the crest's heading in degrees."}
    ],
    "rules": []
}
(OUT / 'map.json').write_text(json.dumps(meta, indent=2) + '\n', encoding='utf-8')
with open(OUT / 'map.svmap', 'wb') as f:
    f.write(struct.pack('<4sIII', b'SVMP', 1, W, H))
    f.write(bytes(biome)); f.write(bytes(land)); f.write(bytes(moist))
counts = {n: sum(1 for v in biome if v == B[n]) for n in BIOMES}
print('wrote', OUT, W, 'x', H, 'cells;', {k: v for k, v in counts.items()})
