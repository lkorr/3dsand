#!/usr/bin/env python3
"""Generate assets/mobs/critter.{vox,json} — a small quadruped exercising the
Wave 2a animation runtime (docs/PLAN_voxel_editor.md §B).

Where dummy.vox is the *legacy* fixture (flat limbs, swingAmp phase-swing, no
IK), critter is the *new* one and demonstrates every added feature:

  - TWO-SEGMENT legs (upper + lower per leg) so the two-bone IK solver has a
    real elbow to place, not a degenerate single bone.
  - `chains`, one per leg, with an explicit pole vector (the bend plane is
    never inferred from the pose).
  - `gait` with DIAGONAL-PAIR groups (FL+BR / FR+BL) — the one-group-swinging
    rule then produces a trot with no gait-specific code.
  - a `spring` tail: jiggled by body motion, never keyed.
  - a two-key `attack` clip (override, masked to the head) wired to the
    non-fatal-damage flinch trigger.
  - `severImpactSpeed` on the legs and tail.

Wave 3 upgrades it to `"scale": 2` — MICROVOXELS. Every limb is authored at 2
voxels per world voxel, so the silhouette is unchanged (a 3x6x3 world-voxel
torso is now a 6x12x6 micro box) but there is room for features a world-voxel
grid cannot hold: toed feet, eye pixels, a tapering tail. Those limbs render
through the OBB/brick-march pass (assets/shaders/microbody.wgsl) instead of one
cube instance per voxel, and their Jolt colliders are built at voxel pitch 1/2.

dummy.vox deliberately stays at scale 1: it is the regression fixture that
proves the legacy instanced-cube path still works.

Coordinates are MagicaVoxel scene space (Z-up), in MICRO units; the loader
converts to Y-up and the engine divides positions by `scale`. Palette index ==
material ID. Every limb must stay under 120 MICRO voxels per axis (DebrisVoxel
is int8) — the largest here is 12.

The creature faces scene -Y, which the loader maps to engine +Z, which is the
direction the engine's heading of 0 travels along. See the FACING CONVENTION
comment on LIMBS: authoring the nose on the wrong axis makes the mob walk
backwards, and the `mob gait` selftest asserts against exactly that.
"""
import json
import math
import os
import struct
import sys

WOOD, PLANT = 2, 17
# Micro voxels per world voxel. Everything below is authored in micro units;
# SCALE is the single number that relates them to the world grid.
SCALE = 2


def chunk(cid, content, children=b""):
    return cid + struct.pack("<ii", len(content), len(children)) + content + children


def dict_bytes(d):
    out = struct.pack("<i", len(d))
    for k, v in d.items():
        out += struct.pack("<i", len(k)) + k.encode()
        out += struct.pack("<i", len(v)) + v.encode()
    return out


def model_chunks(size, voxels):
    sx, sy, sz = size
    xyzi = struct.pack("<i", len(voxels)) + b"".join(
        struct.pack("4B", x, y, z, c) for (x, y, z, c) in voxels
    )
    return chunk(b"SIZE", struct.pack("<iii", sx, sy, sz)) + chunk(b"XYZI", xyzi)


def ntrn(node_id, name, child, t):
    attrs = {"_name": name} if name else {}
    frame = {"_t": f"{t[0]} {t[1]} {t[2]}"}
    c = struct.pack("<i", node_id) + dict_bytes(attrs)
    c += struct.pack("<iiii", child, -1, 0, 1) + dict_bytes(frame)
    return chunk(b"nTRN", c)


def ngrp(node_id, children):
    c = struct.pack("<i", node_id) + dict_bytes({})
    c += struct.pack("<i", len(children))
    for ch in children:
        c += struct.pack("<i", ch)
    return chunk(b"nGRP", c)


def nshp(node_id, model_id):
    c = struct.pack("<i", node_id) + dict_bytes({})
    c += struct.pack("<ii", 1, model_id) + dict_bytes({})
    return chunk(b"nSHP", c)


def box(size, mat):
    sx, sy, sz = size
    return [(x, y, z, mat)
            for z in range(sz) for y in range(sy) for x in range(sx)]


def torso_vox(size):
    """Rounded slab: knock the four long edges off so the body reads as a
    creature rather than a brick. Only possible at 2x — at world resolution a
    3-voxel-wide torso has no edge to spare."""
    sx, sy, sz = size
    out = []
    for z in range(sz):
        for y in range(sy):
            for x in range(sx):
                edge_x = x == 0 or x == sx - 1
                edge_z = z == 0 or z == sz - 1
                edge_y = y == 0 or y == sy - 1
                # drop the 8 corners and the 4 long x/z edges at the ends
                if edge_x and edge_z:
                    continue
                if edge_y and (edge_x or edge_z):
                    continue
                out.append((x, y, z, WOOD))
    return out


# The shape builders below are all written in the natural "front = +y" reading
# (snout at the high-y end, toes reaching +y). The creature actually faces scene
# -y — see the FACING CONVENTION note on LIMBS — so each one is mirrored through
# `flip_y` at the end. Writing them forward and mirroring once is much easier to
# check by eye than authoring every index backwards, and it keeps the shapes and
# the placement using ONE definition of "front".
def flip_y(size, voxels):
    sy = size[1]
    return [(x, sy - 1 - y, z, c) for (x, y, z, c) in voxels]


def head_vox(size):
    """Blunt snout plus two dark eye pixels — the feature the whole scale-2
    upgrade exists to demonstrate. Eyes are WOOD against a PLANT head, so they
    read as a different material rather than a different shade."""
    sx, sy, sz = size
    out = []
    for z in range(sz):
        for y in range(sy):
            for x in range(sx):
                # taper the front two rows into a snout
                if y >= sy - 2:
                    if x == 0 or x == sx - 1 or z == sz - 1:
                        continue
                if (x == 0 or x == sx - 1) and (z == 0 or z == sz - 1):
                    continue
                out.append((x, y, z, PLANT))
    # Eyes are WOOD pixels punched into the finished head: just behind the
    # snout (y), upper half (z). A set keeps the rewrite a single O(n) pass.
    eyes = {(1, sy - 3, sz - 2), (sx - 2, sy - 3, sz - 2)}
    out = [(x, y, z, WOOD if (x, y, z) in eyes else m) for (x, y, z, m) in out]
    return flip_y(size, out)


def tail_vox(size):
    """Tapers to a single voxel at the tip. A world-resolution 1x4x1 tail has
    no cross-section to taper; at 2x it starts 2x2 and narrows.

    Authored with the BASE at high y (nearest the torso, which sits toward the
    creature's front); flip_y then puts the base at the torso end for real."""
    _, sy, _ = size
    out = []
    for y in range(sy):
        # thick at the base (y = sy-1, nearest the torso), one voxel at the tip
        thick = 2 if y >= sy // 2 else 1
        for z in range(thick):
            for x in range(thick):
                out.append((x, y, z, PLANT))
    return flip_y(size, out)


def foot_vox(size):
    """Lower leg with a two-toed foot. The declared box is 2 micro voxels deep
    in +y; the shin fills only the back one and the bottom slice spreads
    forward into two toes. A world-resolution leg is a 1x1 column with nowhere
    to put a toe at all."""
    sx, _, sz = size
    out = []
    for z in range(sz):
        for x in range(sx):
            out.append((x, 0, z, WOOD))     # shin: back row only
    for x in (0, sx - 1):
        out.append((x, 1, 0, WOOD))         # two toes on the ground slice
    return flip_y(size, out)


# limb -> (size, min-corner in SCENE space (x, y, z), material) — MICRO units,
# i.e. every number below is 2x its world-voxel value.
#
# FACING CONVENTION (this bit is load-bearing; getting it wrong makes the mob
# walk backwards). The engine's heading is
#     fwd = (sin(heading), 0, cos(heading))
# so at heading 0 a mob travels along ENGINE +Z, and the yaw quaternion
# AxisAngle({0,1,0}, heading) is identity there. A model must therefore have its
# NOSE ON ENGINE +Z. The loader converts scene -> engine with
#     engine = (scene.x, scene.z, -scene.y)        [voxload.cpp]
# which NEGATES scene y. So "nose on engine +Z" means "nose on scene -Y".
#
# This file used to author the nose at scene +y, which the loader mapped to
# engine -Z: the critter's head pointed directly away from the direction it
# walked, i.e. it locomoted tail-first. The `-y` values below put the head, the
# front legs and the tail on the correct sides. Verify with:
#     head engineZ (= -scene y) must be GREATER than tail engineZ.
# Feet rest on z=0; upper legs span z 6..10, lower legs z 0..6.
LIMBS = {
    "torso":   ((6, 12, 6), (-2, -6, 10), WOOD),
    "head":    ((6,  6, 6), (-2, -12, 12), PLANT),
    "tail":    ((2,  8, 2), ( 0,   6, 12), PLANT),
    # front-left / front-right / back-left / back-right — FRONT is scene -y
    "legU.FL": ((2, 2, 6), (-4, -4,  6), WOOD),
    "legL.FL": ((2, 2, 6), (-4, -4,  0), WOOD),  # y-depth 2 leaves room for toes
    "legU.FR": ((2, 2, 6), ( 4, -4,  6), WOOD),
    "legL.FR": ((2, 2, 6), ( 4, -4,  0), WOOD),
    "legU.BL": ((2, 2, 6), (-4,  2,  6), WOOD),
    "legL.BL": ((2, 2, 6), (-4,  2,  0), WOOD),
    "legU.BR": ((2, 2, 6), ( 4,  2,  6), WOOD),
    "legL.BR": ((2, 2, 6), ( 4,  2,  0), WOOD),
}

# Per-limb voxel builders; anything absent is a solid box of its material.
SHAPES = {
    "torso": torso_vox,
    "head": head_vox,
    "tail": tail_vox,
}
for _leg in ("FL", "FR", "BL", "BR"):
    SHAPES[f"legL.{_leg}"] = foot_vox

ORDER = ["torso", "head", "tail",
         "legU.FL", "legL.FL", "legU.FR", "legL.FR",
         "legU.BL", "legL.BL", "legU.BR", "legL.BR"]


def to_engine(scene_xyz, min_x, max_y):
    """Scene (Z-up) -> prefab-local engine coords (Y-up, min corner 0).

    Matches the loader's conversion: engine.x = vox.x, engine.y = vox.z,
    engine.z = -vox.y, then rebased so the prefab min corner sits at 0.
    """
    x, y, z = scene_xyz
    return [round(x - min_x, 1), round(float(z), 1), round(max_y - y, 1)]


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "assets", "mobs")
    os.makedirs(out_dir, exist_ok=True)

    body = b""
    graph = b""
    grp_children = []
    for i, name in enumerate(ORDER):
        size, mn, mat = LIMBS[name]
        # DebrisVoxel is int8 and the coordinates here are MICRO units, so the
        # bound is 120 micro voxels = 60 world voxels at scale 2.
        assert max(size) <= 120, f"{name} exceeds DebrisVoxel int8 range"
        voxels = SHAPES[name](size) if name in SHAPES else box(size, mat)
        assert voxels, f"{name} generated no voxels"
        for (x, y, z, _c) in voxels:
            assert 0 <= x < size[0] and 0 <= y < size[1] and 0 <= z < size[2], \
                f"{name} voxel {(x, y, z)} outside declared size {size}"
        body += model_chunks(size, voxels)
        pivot = (size[0] // 2, size[1] // 2, size[2] // 2)
        t = (mn[0] + pivot[0], mn[1] + pivot[1], mn[2] + pivot[2])
        trn_id, shp_id = 2 + 2 * i, 3 + 2 * i
        graph += ntrn(trn_id, name, shp_id, t) + nshp(shp_id, i)
        grp_children.append(trn_id)
    graph = ntrn(0, "", 1, (0, 0, 0)) + ngrp(1, grp_children) + graph

    payload = body + graph
    data = b"VOX " + struct.pack("<i", 150)
    data += b"MAIN" + struct.pack("<ii", 0, len(payload)) + payload
    vox_path = os.path.join(out_dir, "critter.vox")
    with open(vox_path, "wb") as f:
        f.write(data)

    # prefab extents in scene space, for the anchor conversion
    min_x = min(mn[0] for (_, mn, _) in LIMBS.values())
    max_y = max(mn[1] + sz[1] for (sz, mn, _) in LIMBS.values())

    def anchor(scene_xyz):
        return to_engine(scene_xyz, min_x, max_y)

    # Quaternion helpers for clip keys: (x, y, z, w), degrees, single axis.
    def qx(deg):
        h = math.radians(deg) * 0.5
        return [round(math.sin(h), 4), 0.0, 0.0, round(math.cos(h), 4)]

    def qy(deg):
        h = math.radians(deg) * 0.5
        return [0.0, round(math.sin(h), 4), 0.0, round(math.cos(h), 4)]

    # Crawl leg tracks: diagonal pairs paddle in anti-phase about X (the same
    # pairing the trot's gait groups use, so the scrabble reads as the broken
    # remains of its old stride) while every lower leg stays folded flat. The
    # tracks cover ALL legs; a severed one has no body and simply isn't drawn.
    def paddle(phase_ms, period=1000):
        half = period // 2
        k0 = qx(40) if phase_ms == 0 else qx(-40)
        k1 = qx(-40) if phase_ms == 0 else qx(40)
        return {"rot": [{"t": 0, "q": k0, "ease": "quadInOut"},
                        {"t": half, "q": k1, "ease": "quadInOut"},
                        {"t": period, "q": k0}]}

    fold = {"rot": [{"t": 0, "q": qx(-70)}]}

    # Joint anchors are scene-space points and must sit on the same side of the
    # body as the limb they attach: the head joint at the FRONT (scene -y), the
    # tail joint at the BACK (scene +y). These flipped with the LIMBS table.
    limbs = [
        {"name": "torso", "hp": 45, "severable": False, "tag": "spine"},
        {"name": "head", "parent": "torso", "joint": "ball", "hp": 18,
         "severable": True, "vital": True, "tag": "head",
         "anchor": anchor((0, -6, 14)), "severImpactSpeed": 22.0},
        # a part is KEYED or JIGGLED, never both: the tail has `spring` and
        # deliberately no swingAmp and no clip track
        {"name": "tail", "parent": "torso", "joint": "ball", "hp": 8,
         "severable": True, "tag": "tail", "anchor": anchor((0, 6, 12)),
         "severImpactSpeed": 10.0,
         "spring": {"halflife": 0.18, "gain": 1.4, "maxAngle": 0.6}},
    ]
    # four legs, two segments each. FRONT legs are at scene -y (see LIMBS).
    for leg, (lx, ly) in (("FL", (-4, -4)), ("FR", (4, -4)),
                          ("BL", (-4, 2)), ("BR", (4, 2))):
        limbs.append({"name": f"legU.{leg}", "parent": "torso", "joint": "ball",
                      "hp": 14, "severable": True, "tag": "leg",
                      "anchor": anchor((lx, ly, 12)), "severImpactSpeed": 18.0})
        limbs.append({"name": f"legL.{leg}", "parent": f"legU.{leg}",
                      "joint": "hinge", "hp": 12, "severable": True,
                      "tag": "leg", "axis": [1, 0, 0],
                      "minAngle": -2.0, "maxAngle": 0.05,
                      "anchor": anchor((lx, ly, 6)), "severImpactSpeed": 18.0})

    sidecar = {
        "root": "torso",
        # MICROVOXELS (Wave 3). Limb .vox coordinates above are in micro units,
        # 2 per world voxel; the engine divides every POSITION by this and
        # builds the colliders at pitch 1/2, so the creature's physical size,
        # mass and gait are identical to the scale-1 version it replaced.
        "scale": SCALE,
        "bleed": {"material": "blood", "perDamage": 2.0},
        "speed": 6.0,
        "gait": {
            "cadence": 2.6, "strideBias": 0.45, "leadTime": 0.18,
            "stepThreshold": 0.55, "stepDuration": 0.18, "stepHeight": 0.3,
            "rideHeight": 1.05,
            # DIAGONAL PAIRS: only one group may swing at a time, which makes
            # this a trot. Swapping to four singleton groups would make it a
            # walk; nothing else in the engine changes.
            "groups": [["legU.FL", "legU.BR"], ["legU.FR", "legU.BL"]],
            "bobAmp": 0.09, "bobFreqMul": 2.0, "swayAmp": 0.05,
            "rollAmp": 0.07, "spineCounter": 0.6, "phaseLag": 0.04,
        },
        "limbs": limbs,
        "chains": [
            {"tag": "leg", "parts": [f"legU.{l}", f"legL.{l}"],
             "effector": f"legL.{l}",
             # pole points forward (+Z engine) so knees bend the right way
             "pole": [0, 0, 1], "solver": "twobone"}
            for l in ("FL", "FR", "BL", "BR")
        ],
        # Dismemberment locomotion. A single lost leg needs no rule at all —
        # the gait state machine already drops that chain and the survivors
        # take their turns sooner. Two unusable legs is where a trot stops
        # making sense: drop to the belly and scrabble.
        "states": [
            {"name": "crawl", "minChainsLost": 2, "clip": "crawl",
             "speedScale": 0.35, "disableGait": True},
        ],
        # Two-key override clip, masked to the head, wired to the non-fatal
        # damage flinch. Quaternions are (x, y, z, w).
        "clips": {
            "attack": {
                "durationMs": 420, "loop": False, "mode": "override",
                "mask": ["head"], "blendInMs": 60, "blendOutMs": 140,
                "tracks": {
                    "head": {
                        "rot": [
                            {"t": 0, "q": [0.0, 0.0, 0.0, 1.0], "ease": "cubicOut"},
                            # ~40 deg head snap about X (a lunge/bite)
                            {"t": 200, "q": [0.342, 0.0, 0.0, 0.940],
                             "ease": "cubicInOut"},
                            {"t": 420, "q": [0.0, 0.0, 0.0, 1.0], "ease": "linear"},
                        ]
                    }
                },
            },
            # Belly scrabble for the >=2-legs-lost state: with disableGait the
            # foot-derived body height is gone and mob.bodyY settles onto the
            # walk drive's ground contact, which alone drops the body from
            # standing height to the ground — no torso pos key needed. The
            # serpentine yaw wriggle and the head-up are what keep it reading
            # as "dragging itself" rather than "sunken idle". Tail is
            # deliberately unmasked: it is spring-jiggled, never keyed.
            "crawl": {
                "durationMs": 1000, "loop": True, "mode": "override",
                "mask": ["torso", "head",
                         "legU.FL", "legL.FL", "legU.FR", "legL.FR",
                         "legU.BL", "legL.BL", "legU.BR", "legL.BR"],
                "blendInMs": 200,
                "tracks": {
                    "torso": {
                        "rot": [
                            {"t": 0, "q": qy(8), "ease": "quadInOut"},
                            {"t": 500, "q": qy(-8), "ease": "quadInOut"},
                            {"t": 1000, "q": qy(8)},
                        ]
                    },
                    "head": {"rot": [{"t": 0, "q": qx(-30)}]},
                    "legU.FL": paddle(0), "legU.BR": paddle(0),
                    "legU.FR": paddle(500), "legU.BL": paddle(500),
                    "legL.FL": fold, "legL.FR": fold,
                    "legL.BL": fold, "legL.BR": fold,
                },
            },
        },
    }

    json_path = os.path.join(out_dir, "critter.json")
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(sidecar, f, indent=2)
        f.write("\n")
    print(f"wrote {vox_path} and {json_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
