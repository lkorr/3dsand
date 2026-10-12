# gen_dungeon_fixture.py -- writes assets/dungeons/_fixture/shell.json, the
# engine's HAND-WRITTEN dungeon fixture (docs/PLAN_dungeons.md D1; the
# dungeon-format / dungeon-gen gates), stamping `sourceHash` from the
# dungeon.json beside it (FNV-1a 32, '\r' dropped). Then
# `sandvox --dungeon-bake _fixture` writes shell.bin. Run from the repo root.
import json
src = open('assets/dungeons/_fixture/dungeon.json', 'rb').read().replace(b'\r', b'')
h = 2166136261
for c in src:
    h = ((h ^ c) * 16777619) & 0xFFFFFFFF

def box(op, shape, mn, mx, **kw):
    d = {"op": op, "shape": shape, "min": mn, "max": mx}
    d.update(kw)
    return d

prims = [
    # p0: the stair down from the surface, rising toward -x to the anchor's floor.
    box("carve", "ramp", [0, -60, -9], [120, 27, 9], dir="-x", stepH=1, stepD=2, height=28, below=0,
        noise={"amp": 1, "scale": 3, "octaves": 1, "seed": 101}, blend=4, piece="p0"),
    # p1: the vaulted corridor; ore in its walls.
    box("carve", "vault", [118, -60, -10], [520, -28, 10], axis="x", rise=8,
        noise={"amp": 2, "scale": 4, "octaves": 2, "seed": 111}, blend=6, piece="p1",
        scatter=[{"kind": "ore_vein", "density": 0.15, "size": [4, 4], "seed": 10}]),
    # p2: the chamber; rubble piles on the floor, stalactites from the roof.
    box("carve", "rbox", [520, -62, -60], [680, -20, 60], r=10,
        noise={"amp": 3, "scale": 5, "octaves": 2, "seed": 121}, blend=10, piece="p2",
        scatter=[{"kind": "rubble", "density": 0.35, "size": [2, 4], "seed": 5},
                 {"kind": "stalactite", "density": 0.3, "size": [3, 8], "surface": "ceil", "seed": 6}]),
    # p3: the stair down along +z (rising toward -z to the chamber floor).
    box("carve", "ramp", [585, -114, 60], [615, -34, 140], dir="-z", stepH=2, stepD=3, height=26, below=0,
        blend=4, piece="p3"),
    # p4: the lower vault along +z: moss, puddles, bones.
    box("carve", "vault", [575, -114, 140], [625, -84, 260], axis="z", rise=10,
        noise={"amp": 2, "scale": 4, "octaves": 1, "seed": 141}, blend=6, piece="p4",
        scatter=[{"kind": "moss", "density": 0.5, "surface": "any", "seed": 7},
                 {"kind": "puddle", "density": 0.4, "size": [2, 3], "seed": 8},
                 {"kind": "bones", "density": 0.2, "size": [1, 2], "seed": 9}]),
    # A natural-rock pillar the chamber's carve leaves standing.
    box("keep", "cylinder", [592, -62, -8], [608, -20, 8], axis="y", piece="p2"),
    # A flagstone floor under the chamber.
    box("fill", "box", [520, -63, -60], [680, -63, 60], mat="floor", piece="p2"),
    # The corridor's cobble wall layer (the carve's own surface, 2 deep).
    box("skin", "box", [114, -66, -16], [524, -22, 16], mat="trim", skin=2, piece="p1"),
]
# Two timber sets in the corridor: posts against both walls and a lintel.
for x in (200, 300):
    prims.append(box("fill", "box", [x, -60, -10], [x + 2, -36, -8], mat="support", piece="p1"))
    prims.append(box("fill", "box", [x, -60, 8], [x + 2, -36, 10], mat="support", piece="p1"))
    prims.append(box("fill", "box", [x, -37, -10], [x + 2, -35, 10], mat="support", piece="p1"))

# THE ALCOVE PROP: a niche carved into a wall (the CARVE code), a plank shelf
# across it, a glowcap hanging from its roof. 9 wide, 14 tall, 7 deep (z).
vox = []
for x in range(1, 8):
    for y in range(0, 12):
        for z in range(0, 6):
            if y == 5:
                vox.append([x, y, z, 1])
            else:
                vox.append([x, y, z, 0])
vox.append([4, 11, 5, 2])
vox = [v for v in vox if not (v[0] == 4 and v[1] == 11 and v[2] == 5 and v[3] == 0)]
shell = {
    "format": "sandvox-dungeon-shell",
    "version": 1,
    "dungeon": "_fixture",
    "sourceHash": h,
    "anchor": [0, 0, 0],
    "keepOut": 24,
    "naturalCaves": False,
    "materials": {"wall": "limestone", "floor": "flagstone", "trim": "cobble", "support": "timber", "rubble": "gravel",
                  "moss": "moss_patch", "water": "water", "bone": "bone", "ore": "iron", "crystal": "crystal"},
    "prims": prims,
    "props": {
        "templates": [{"id": "fixture/alcove", "size": [9, 14, 7], "palette": ["@carve", "plank", "glowcap"], "voxels": vox}],
        "instances": [{"template": 0, "pos": [677, -60, -4], "yaw": 90, "id": "x0"}],
    },
}
open('assets/dungeons/_fixture/shell.json', 'w', encoding='utf-8', newline='\n').write(json.dumps(shell, indent=1) + "\n")
print("sourceHash", h, "prims", len(prims), "template voxels", len(vox))
