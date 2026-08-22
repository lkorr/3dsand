#!/usr/bin/env python3
"""Generate assets/mobs/asha.{vox,json} — an alternative PLAYER AVATAR: a lean
traveler figure with a short cap, long tunic and wrapped leggings.

A contender alongside gen_mina.py. Same 17-world-voxel height, same rig
topology, same facing and handedness conventions (model +X = character's LEFT).
Differs in proportions: taller torso, smaller head, visible legs below a shorter
tunic, wrapped leather leggings, a short rounded cap instead of a pointed hood.

SILHOUETTE. Where Mina is a triangle (flared skirt, big hood), Asha is a
column: narrow tunic, visible wrapped legs, small cap. Reading top down:

    a short rounded cap, no brim, sitting on the crown
    a visible face — still a void, but wider and shorter than Mina's
    a long tunic that ends mid-thigh, no flare
    a cloth sash at the waist (the one bright element)
    wrapped leggings visible below the tunic hem
    simple flat shoes

HEIGHT BUDGET (same contract as gen_mina.py):
    Player::kHalfY    0.85 m  -> 17.0 world voxels tall   (player.h)
    Player::kEyeOffset 0.65 m -> eyes 1.50 m up = voxel 15.0
    kVoxelMeters      0.10 m                              (sim/world.h)

    foot    z  0..3      shoes
    shin    z  3..13     wrapped leggings
    thigh   z 12..22     upper leg, under the tunic
    hips    z  8..34     tunic: hem at mid-shin, sash at the top
    torso   z 32..50     shoulders and chest — TALLER than Mina's (14 -> 18)
    head    z 50..68     cap + face void — SHORTER than Mina's (22 -> 18)

Run:  python scripts/gen_asha.py
"""
import json
import math
import os
import re
import struct

# ---- palette ---------------------------------------------------------------
ROBE = 48       # robe_cloth   deep indigo — the tunic
TRIM = 49       # robe_trim    gold, faintly emissive — the sash
SHADE = 50      # robe_shadow  near-black indigo: cap underside, face void
SKIN = 51       # skin         hands and face surround
LEATHER = 53    # leather      shoes and leg wraps

ART_SCALE = 4
SKIN_UPSCALE = 2
SCALE = ART_SCALE * SKIN_UPSCALE  # 8
WORLD_H = 17
MICRO_H = WORLD_H * SCALE  # 136

SPRINT_SPEED_MPS = 6.0
VOXEL_METERS = 0.10


# ---- .vox writing ----------------------------------------------------------
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


# ---- shape helpers ----------------------------------------------------------
def flip_y(size, voxels):
    sy = size[1]
    return [(x, sy - 1 - y, z, c) for (x, y, z, c) in voxels]


def solid(size, mat):
    sx, sy, sz = size
    return [(x, y, z, mat)
            for z in range(sz) for y in range(sy) for x in range(sx)]


def ellipse_mask(x, y, cx, cy, rx, ry):
    if rx <= 0 or ry <= 0:
        return False
    dx = (x + 0.5 - cx) / rx
    dy = (y + 0.5 - cy) / ry
    return dx * dx + dy * dy <= 1.0


def tapered_tube(size, mat, r0, r1, trim_rows=(), shade_rows=(),
                 leather_rows=()):
    sx, sy, sz = size
    cx, cy = sx * 0.5, sy * 0.5
    out = []
    for z in range(sz):
        t = z / max(sz - 1, 1)
        r = r0 + (r1 - r0) * t
        m = mat
        if z in trim_rows:
            m = TRIM
        elif z in shade_rows:
            m = SHADE
        elif z in leather_rows:
            m = LEATHER
        for y in range(sy):
            for x in range(sx):
                if ellipse_mask(x, y, cx, cy, r, r):
                    out.append((x, y, z, m))
    return out


# ---- per-limb builders ------------------------------------------------------
def head_vox(size):
    """A short rounded cap sitting on the crown, with a wider face void below.

    Shorter than Mina's hood (18 micro vs 22), so the torso gets more room.
    The cap is a half-sphere on top, the face void is a wider rectangle that
    gives the character a distinct, open look compared to Mina's deep hood."""
    sx, sy, sz = size
    out = []
    cx, cy = sx * 0.5, sy * 0.5

    # z layout in this 18-tall head:
    #   0..10   skull/face region
    #   10..18  cap (rounded dome)
    cap_z = int(sz * 0.56)  # ~10
    head_r = sx * 0.45

    for z in range(sz):
        if z < cap_z:
            # skull: a rounded cylinder
            r = head_r
            lean = 0.0
        else:
            # cap: dome that narrows to a rounded top
            t = (z - cap_z) / max(sz - 1 - cap_z, 1)
            r = head_r * (1.0 - t * t * 0.85) + 0.3
            lean = 0.0
        for y in range(sy):
            for x in range(sx):
                if not ellipse_mask(x, y, cx, cy + lean, r, r):
                    continue
                mat = ROBE
                if z < cap_z:
                    mat = ROBE  # skull wrapped in cloth (a coif under the cap)
                # back of the head in shadow
                if y <= cy - 3.5:
                    mat = SHADE
                out.append((x, y, z, mat))

    # the face void: wider and shorter than Mina's, giving a different read
    eye_z = 60 - LIMBS_ART["head"][1][2]  # world voxel 15, same contract
    face_lo, face_hi = eye_z - 3, eye_z + 2
    fz = (face_lo + face_hi) * 0.5

    def in_face(x, z):
        return (face_lo <= z <= face_hi
                and ellipse_mask(x, z, cx, fz, 3.8, (face_hi - face_lo) * 0.65))

    front_of = {}
    for (x, y, z, _m) in out:
        if in_face(x, z):
            front_of[(x, z)] = max(front_of.get((x, z), -1), y)
    carved = []
    for (x, y, z, m) in out:
        if in_face(x, z) and y >= cy - 0.5:
            if y >= front_of.get((x, z), -1):
                continue
            m = SHADE
        carved.append((x, y, z, m))
    return flip_y(size, carved)


def torso_vox(size):
    """A longer, leaner torso than Mina's. Slightly narrower shoulders, more
    vertical — reads as a tunic rather than a robe."""
    sx, sy, sz = size
    cx, cy = sx * 0.5, sy * 0.5
    out = []
    for z in range(sz):
        t = z / max(sz - 1, 1)
        # narrower than Mina — lean figure, not broad-shouldered
        rx = 3.0 + 1.2 * t - (1.2 if z >= sz - 2 else 0.0)
        ry = 2.4 + 0.6 * t - (0.8 if z >= sz - 2 else 0.0)
        for y in range(sy):
            for x in range(sx):
                if not ellipse_mask(x, y, cx, cy, rx, ry):
                    continue
                mat = ROBE
                if y <= cy - 1.6:
                    mat = SHADE
                # vertical seam down the centre front — a tunic's closing line
                elif y > cy + 1.5 and abs(x - cx) < 0.6:
                    mat = SHADE
                out.append((x, y, z, mat))
    return flip_y(size, out)


def hips_vox(size):
    """The tunic skirt — shorter than Mina's, ending mid-thigh rather than at
    the floor. No flare: it hangs straight, showing the wrapped legs below.
    Gold sash at the top."""
    sx, sy, sz = size
    cx, cy = sx * 0.5, sy * 0.5
    out = []
    # minimal flare — a straight-hanging tunic, not a wizard's robe
    hem_r, waist_r = 4.5, 4.2
    sash_lo = sz - 3
    for z in range(sz):
        t = z / max(sz - 1, 1)
        r = waist_r + (hem_r - waist_r) * (1.0 - t) * (1.0 - t)
        for y in range(sy):
            for x in range(sx):
                if not ellipse_mask(x, y, cx, cy, r, r * 0.82):
                    continue
                mat = ROBE
                if z >= sash_lo:
                    mat = TRIM
                elif z <= 1:
                    mat = SHADE
                elif y <= cy - 2.0:
                    mat = SHADE
                out.append((x, y, z, mat))
    return flip_y(size, out)


def upper_arm_vox(size):
    return flip_y(size, tapered_tube(size, ROBE, 1.9, 2.1, shade_rows=(0,)))


def fore_arm_vox(size):
    return flip_y(size, tapered_tube(size, ROBE, 1.6, 1.9))


def hand_vox(size):
    sx, sy, sz = size
    out = []
    cx, cy = sx * 0.5, sy * 0.5
    for z in range(sz):
        t = z / max(sz - 1, 1)
        r = 1.5 + 0.6 * t
        for y in range(sy):
            for x in range(sx):
                if ellipse_mask(x, y, cx, cy, r, r * 0.8):
                    out.append((x, y, z, SKIN))
    out.append((int(cx) + 1, sy - 1, sz - 2, SKIN))
    return flip_y(size, out)


def thigh_vox(size):
    """Upper leg with wrapped leather bindings over cloth."""
    sx, sy, sz = size
    cx, cy = sx * 0.5, sy * 0.5
    out = []
    for z in range(sz):
        t = z / max(sz - 1, 1)
        r = 1.8 + 0.3 * t
        for y in range(sy):
            for x in range(sx):
                if not ellipse_mask(x, y, cx, cy, r, r):
                    continue
                # diagonal wrap pattern: leather bands over dark cloth
                mat = SHADE
                if (x + z) % 4 < 2:
                    mat = LEATHER
                out.append((x, y, z, mat))
    return flip_y(size, out)


def shin_vox(size):
    """Lower leg: leather wraps more prominent here."""
    sx, sy, sz = size
    cx, cy = sx * 0.5, sy * 0.5
    out = []
    for z in range(sz):
        t = z / max(sz - 1, 1)
        r = 1.5 + 0.3 * t
        for y in range(sy):
            for x in range(sx):
                if not ellipse_mask(x, y, cx, cy, r, r):
                    continue
                mat = LEATHER
                # alternating wrap bands
                if (x + z) % 3 == 0:
                    mat = SHADE
                out.append((x, y, z, mat))
    return flip_y(size, out)


def foot_vox(size):
    """Simple flat shoes — no curl, no buckle. Functional."""
    sx, sy, sz = size
    out = []
    cx = sx * 0.5
    for z in range(sz):
        for y in range(sy):
            for x in range(sx):
                in_sole = z < 2
                in_ankle = y < 3 and z >= 2
                if not (in_sole or in_ankle):
                    continue
                if not ellipse_mask(x, y if in_ankle else 2, cx, 2.0, 1.8, 2.3):
                    continue
                out.append((x, y, z, LEATHER))
    return flip_y(size, out)


# ---- limb table (ART_SCALE units) ------------------------------------------
# Height budget (micro, at ART_SCALE=4):
#   foot    z  0..3    (3 tall)
#   shin    z  3..13   (10 tall)
#   thigh   z 12..22   (10 tall)
#   hips    z  8..34   tunic: hem at z=8 (mid-shin), sash at top
#   torso   z 32..50   (18 tall, vs Mina's 14)
#   head    z 50..68   (18 tall, vs Mina's 22)
#
# Total: soles at z=0, cap peak at z=68 = 17 world voxels.
LIMBS_ART = {
    "hips":     ((14, 14, 26), (-7, -7,  8), ROBE),
    "torso":    ((11,  9, 18), (-5, -4, 32), ROBE),
    "head":     ((11, 10, 18), (-5, -5, 50), ROBE),

    "armU.L":   (( 4,  4,  8), (  4, -2, 41), ROBE),
    "armL.L":   (( 4,  4,  7), (  4, -2, 35), ROBE),
    "hand.L":   (( 4,  4,  4), (  4, -2, 32), SKIN),
    "armU.R":   (( 4,  4,  8), (-8, -2, 41), ROBE),
    "armL.R":   (( 4,  4,  7), (-8, -2, 35), ROBE),
    "hand.R":   (( 4,  4,  4), (-8, -2, 32), SKIN),

    "legU.L":   (( 4,  4, 10), (  0, -2, 12), SHADE),
    "legL.L":   (( 4,  4, 10), (  0, -2,  3), LEATHER),
    "foot.L":   (( 4,  7,  3), (  0, -4,  0), LEATHER),
    "legU.R":   (( 4,  4, 10), (-4, -2, 12), SHADE),
    "legL.R":   (( 4,  4, 10), (-4, -2,  3), LEATHER),
    "foot.R":   (( 4,  7,  3), (-4, -4,  0), LEATHER),
}

LIMBS = LIMBS_ART

SHAPES = {
    "hips": hips_vox, "torso": torso_vox, "head": head_vox,
    "armU.L": upper_arm_vox, "armU.R": upper_arm_vox,
    "armL.L": fore_arm_vox, "armL.R": fore_arm_vox,
    "hand.L": hand_vox, "hand.R": hand_vox,
    "legU.L": thigh_vox, "legU.R": thigh_vox,
    "legL.L": shin_vox, "legL.R": shin_vox,
    "foot.L": foot_vox, "foot.R": foot_vox,
}

ORDER = ["hips", "torso", "head",
         "armU.L", "armL.L", "hand.L",
         "armU.R", "armL.R", "hand.R",
         "legU.L", "legL.L", "foot.L",
         "legU.R", "legL.R", "foot.R"]

PROPS = set()

# ---- scale to shipping resolution ------------------------------------------
_ART_LIMBS = LIMBS_ART
if SKIN_UPSCALE != 1:
    LIMBS = {n: (tuple(v * SKIN_UPSCALE for v in size),
                 tuple(v * SKIN_UPSCALE for v in mn), mat)
             for n, (size, mn, mat) in _ART_LIMBS.items()}


def scale_clip_positions(clips, factor):
    if factor == 1:
        return clips
    for c in clips.values():
        for tr in (c.get("tracks") or {}).values():
            for k in (tr.get("pos") or []):
                k["v"] = [round(v * factor, 4) for v in k["v"]]
    return clips


def upscale_voxels(voxels, factor):
    if factor == 1:
        return voxels
    out = []
    for x, y, z, c in voxels:
        bx, by, bz = x * factor, y * factor, z * factor
        for dz in range(factor):
            for dy in range(factor):
                for dx in range(factor):
                    out.append((bx + dx, by + dy, bz + dz, c))
    return out


def to_engine(scene_xyz, min_x, max_y):
    x, y, z = scene_xyz
    return [round(x - min_x, 1), round(float(z), 1), round(max_y - y, 1)]


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "assets", "mobs")
    os.makedirs(out_dir, exist_ok=True)

    # Height contract
    body_parts = {n: v for n, v in LIMBS.items() if n not in PROPS}
    top = max(mn[2] + sz[2] for (sz, mn, _m) in body_parts.values())
    bottom = min(mn[2] for (_sz, mn, _m) in body_parts.values())
    assert bottom == 0, f"figure does not stand on z=0 (lowest part at {bottom})"
    assert top == MICRO_H, (
        f"figure is {top} micro ({top / SCALE} world voxels) tall, expected "
        f"{MICRO_H} ({WORLD_H}) to match Player::kHalfY * 2")

    # Speed contract
    with open(os.path.join(root, "assets", "materials", "tuning.json")) as tf:
        sprint = json.load(tf)["player"]["sprintSpeed"]
    assert abs(sprint - SPRINT_SPEED_MPS) < 1e-6, (
        f"tuning.json player.sprintSpeed is {sprint}, this file assumes "
        f"{SPRINT_SPEED_MPS} — update SPRINT_SPEED_MPS and regenerate")
    with open(os.path.join(root, "src", "sim", "world.h")) as wf:
        m = re.search(r"kVoxelMeters\s*=\s*([0-9.]+)f", wf.read())
    assert m, "could not find kVoxelMeters in src/sim/world.h"
    assert abs(float(m.group(1)) - VOXEL_METERS) < 1e-9, (
        f"world.h kVoxelMeters is {m.group(1)}, this file assumes "
        f"{VOXEL_METERS} — update VOXEL_METERS and regenerate")

    body = b""
    graph = b""
    grp_children = []
    for i, name in enumerate(ORDER):
        size, mn, mat = LIMBS[name]
        art_size, _art_mn, _m = _ART_LIMBS[name]
        world_extent = max(size) / SCALE
        assert world_extent <= 120, (
            f"{name} is {world_extent} world voxels; even a 1:1 collider "
            f"exceeds the DebrisVoxel int8 bound")
        voxels = (SHAPES[name](art_size) if name in SHAPES
                  else solid(art_size, mat))
        assert voxels, f"{name} generated no voxels"
        voxels = upscale_voxels(voxels, SKIN_UPSCALE)
        seen = set()
        uniq = []
        for v in voxels:
            x, y, z, _c = v
            assert 0 <= x < size[0] and 0 <= y < size[1] and 0 <= z < size[2], \
                f"{name} voxel {(x, y, z)} outside declared size {size}"
            if (x, y, z) in seen:
                continue
            seen.add((x, y, z))
            uniq.append(v)
        body += model_chunks(size, uniq)
        pivot = (size[0] // 2, size[1] // 2, size[2] // 2)
        t = (mn[0] + pivot[0], mn[1] + pivot[1], mn[2] + pivot[2])
        trn_id, shp_id = 2 + 2 * i, 3 + 2 * i
        graph += ntrn(trn_id, name, shp_id, t) + nshp(shp_id, i)
        grp_children.append(trn_id)
    graph = ntrn(0, "", 1, (0, 0, 0)) + ngrp(1, grp_children) + graph

    payload = body + graph
    data = b"VOX " + struct.pack("<i", 150)
    data += b"MAIN" + struct.pack("<ii", 0, len(payload)) + payload
    with open(os.path.join(out_dir, "asha.vox"), "wb") as f:
        f.write(data)

    # Prefab extents
    body_x = {n: v for n, v in LIMBS.items() if n not in PROPS}
    min_x = min(mn[0] for (_, mn, _) in body_x.values())
    max_y = max(mn[1] + sz[1] for (sz, mn, _) in body_x.values())

    def anchor(scene_xyz):
        return to_engine(scene_xyz, min_x, max_y)

    def qx(deg):
        h = math.radians(deg) * 0.5
        return [round(math.sin(h), 4), 0.0, 0.0, round(math.cos(h), 4)]

    def qy(deg):
        h = math.radians(deg) * 0.5
        return [0.0, round(math.sin(h), 4), 0.0, round(math.cos(h), 4)]

    def qz(deg):
        h = math.radians(deg) * 0.5
        return [0.0, 0.0, round(math.sin(h), 4), round(math.cos(h), 4)]

    ident = [0.0, 0.0, 0.0, 1.0]

    U = float(SKIN_UPSCALE)

    def joint_top(part, dx=0.0, dy=0.0, inset=0.0):
        size, mn, _m = LIMBS[part]
        return anchor((mn[0] + size[0] * 0.5 + dx * U,
                       mn[1] + size[1] * 0.5 + dy * U,
                       mn[2] + size[2] - inset * U))

    def joint_bottom(part, dx=0.0, dy=0.0, rise=0.0):
        size, mn, _m = LIMBS[part]
        return anchor((mn[0] + size[0] * 0.5 + dx * U,
                       mn[1] + size[1] * 0.5 + dy * U,
                       mn[2] + rise * U))

    def joint_at(part, z, dx=0.0, dy=0.0):
        size, mn, _m = LIMBS[part]
        return anchor((mn[0] + size[0] * 0.5 + dx * U,
                       mn[1] + size[1] * 0.5 + dy * U, z))

    # ---- limbs ----
    hips_sz, hips_mn, _ = LIMBS["hips"]
    waist_z = hips_mn[2] + hips_sz[2]
    limbs = [
        {"name": "hips", "hp": 60, "severable": False, "tag": "spine"},
        {"name": "torso", "parent": "hips", "joint": "ball", "hp": 60,
         "severable": False, "vital": True, "tag": "spine",
         "anchor": joint_at("hips", waist_z - 2 * U)},
        {"name": "head", "parent": "torso", "joint": "ball", "hp": 22,
         "severable": True, "vital": True, "tag": "head",
         "anchor": joint_top("torso", inset=1), "severImpactSpeed": 20.0,
         "spring": {"halflife": 0.12, "gain": 0.16, "maxAngle": 0.3}},
    ]
    for side in ("L", "R"):
        limbs.append({
            "name": f"armU.{side}", "parent": "torso", "joint": "ball",
            "hp": 16, "severable": True, "tag": "arm",
            "anchor": joint_top(f"armU.{side}"),
            "severImpactSpeed": 15.0})
        limbs.append({
            "name": f"armL.{side}", "parent": f"armU.{side}", "joint": "hinge",
            "hp": 13, "severable": True, "tag": "arm", "axis": [1, 0, 0],
            "minAngle": -2.4, "maxAngle": 0.05,
            "anchor": joint_top(f"armL.{side}"),
            "severImpactSpeed": 13.0})
        limbs.append({
            "name": f"hand.{side}", "parent": f"armL.{side}", "joint": "ball",
            "hp": 9, "severable": True, "tag": "hand",
            "anchor": joint_top(f"hand.{side}"),
            "severImpactSpeed": 9.0})
    for side in ("L", "R"):
        limbs.append({
            "name": f"legU.{side}", "parent": "hips", "joint": "ball",
            "hp": 22, "severable": True, "tag": "leg",
            "anchor": joint_top(f"legU.{side}"),
            "severImpactSpeed": 18.0})
        limbs.append({
            "name": f"legL.{side}", "parent": f"legU.{side}", "joint": "hinge",
            "hp": 18, "severable": True, "tag": "leg", "axis": [1, 0, 0],
            "minAngle": -2.4, "maxAngle": 0.05,
            "anchor": joint_top(f"legL.{side}"),
            "severImpactSpeed": 16.0})
        limbs.append({
            "name": f"foot.{side}", "parent": f"legL.{side}", "joint": "hinge",
            "hp": 12, "severable": True, "tag": "foot", "axis": [1, 0, 0],
            "minAngle": -0.6, "maxAngle": 0.6,
            "anchor": joint_bottom(f"foot.{side}", dy=-1.5, rise=2.0),
            "severImpactSpeed": 12.0})

    # ---- sockets ----
    hand_sz, hand_mn, _ = LIMBS["hand.R"]
    sockets = [{
        "name": "held_right",
        "part": "hand.R",
        "offset": anchor((hand_mn[0] + hand_sz[0] * 0.5,
                          hand_mn[1] + hand_sz[1] * 0.5,
                          hand_mn[2] + hand_sz[2] * 0.5)),
        "rotation": [0, 0, 0],
    }]

    # ---- IK chains ----
    chains = []
    for side in ("L", "R"):
        chains.append({"tag": "leg",
                       "parts": [f"legU.{side}", f"legL.{side}", f"foot.{side}"],
                       "effector": f"foot.{side}",
                       "pole": [0, 0, 1], "solver": "twobone"})
    for side in ("L", "R"):
        chains.append({"tag": "arm",
                       "parts": [f"armU.{side}", f"armL.{side}", f"hand.{side}"],
                       "effector": f"hand.{side}",
                       "pole": [0, 0, -1], "solver": "twobone"})

    # ---- clips ----
    def swing(deg, period=900, phase=0, ease="quadInOut"):
        a, b = (deg, -deg) if phase == 0 else (-deg, deg)
        half = period // 2
        return {"rot": [{"t": 0, "q": qx(a), "ease": ease},
                        {"t": half, "q": qx(b), "ease": ease},
                        {"t": period, "q": qx(a)}]}

    clips = {}

    clips["idle"] = {
        "durationMs": 3200, "loop": True, "mode": "additive",
        "blendInMs": 400, "blendOutMs": 400,
        "mask": ["torso", "head", "armU.L", "armU.R"],
        "tracks": {
            "torso": {"rot": [{"t": 0, "q": qx(0), "ease": "quadInOut"},
                              {"t": 1600, "q": qx(-1.4), "ease": "quadInOut"},
                              {"t": 3200, "q": qx(0)}],
                      "pos": [{"t": 0, "v": [0, 0, 0], "ease": "quadInOut"},
                              {"t": 1600, "v": [0, 0.08, 0], "ease": "quadInOut"},
                              {"t": 3200, "v": [0, 0, 0]}]},
            "head": {"rot": [{"t": 0, "q": qy(0), "ease": "quadInOut"},
                             {"t": 1100, "q": qy(5), "ease": "quadInOut"},
                             {"t": 2300, "q": qy(-4), "ease": "quadInOut"},
                             {"t": 3200, "q": qy(0)}]},
            "armU.L": {"rot": [{"t": 0, "q": qz(0), "ease": "quadInOut"},
                               {"t": 1600, "q": qz(2.0), "ease": "quadInOut"},
                               {"t": 3200, "q": qz(0)}]},
            "armU.R": {"rot": [{"t": 0, "q": qz(0), "ease": "quadInOut"},
                               {"t": 1600, "q": qz(-2.0), "ease": "quadInOut"},
                               {"t": 3200, "q": qz(0)}]},
        },
    }

    for nm, deg, period in (("walk", 14, 308), ("run", 20, 238)):
        clips[nm] = {
            "durationMs": period, "loop": True, "mode": "additive",
            "blendInMs": 180, "blendOutMs": 180,
            "mask": ["armU.L", "armL.L", "armU.R", "armL.R", "torso"],
            "tracks": {
                "armU.L": swing(deg, period, 0),
                "armU.R": swing(deg, period, 1),
                "armL.L": {"rot": [{"t": 0, "q": qx(-10), "ease": "quadInOut"},
                                   {"t": period // 2, "q": qx(-24),
                                    "ease": "quadInOut"},
                                   {"t": period, "q": qx(-10)}]},
                "armL.R": {"rot": [{"t": 0, "q": qx(-24), "ease": "quadInOut"},
                                   {"t": period // 2, "q": qx(-10),
                                    "ease": "quadInOut"},
                                   {"t": period, "q": qx(-24)}]},
                "torso": {"rot": [{"t": 0, "q": qy(-deg * 0.16),
                                   "ease": "quadInOut"},
                                  {"t": period // 2, "q": qy(deg * 0.16),
                                   "ease": "quadInOut"},
                                  {"t": period, "q": qy(-deg * 0.16)}]},
            },
        }

    clips["jump"] = {
        "durationMs": 500, "loop": False, "mode": "additive",
        "blendInMs": 60, "blendOutMs": 200,
        "mask": ["torso", "armU.L", "armU.R", "legU.L", "legU.R"],
        "tracks": {
            "torso": {"rot": [{"t": 0, "q": qx(0), "ease": "cubicOut"},
                              {"t": 160, "q": qx(-10), "ease": "cubicInOut"},
                              {"t": 500, "q": qx(0)}]},
            "armU.L": {"rot": [{"t": 0, "q": qx(0), "ease": "cubicOut"},
                               {"t": 200, "q": qx(-70), "ease": "cubicInOut"},
                               {"t": 500, "q": qx(0)}]},
            "armU.R": {"rot": [{"t": 0, "q": qx(0), "ease": "cubicOut"},
                               {"t": 200, "q": qx(-70), "ease": "cubicInOut"},
                               {"t": 500, "q": qx(0)}]},
            "legU.L": {"rot": [{"t": 0, "q": qx(0), "ease": "cubicOut"},
                               {"t": 220, "q": qx(35), "ease": "cubicInOut"},
                               {"t": 500, "q": qx(0)}]},
            "legU.R": {"rot": [{"t": 0, "q": qx(0), "ease": "cubicOut"},
                               {"t": 220, "q": qx(25), "ease": "cubicInOut"},
                               {"t": 500, "q": qx(0)}]},
        },
    }
    clips["fall"] = {
        "durationMs": 700, "loop": True, "mode": "additive",
        "blendInMs": 250, "blendOutMs": 250,
        "mask": ["torso", "armU.L", "armU.R"],
        "tracks": {
            "torso": {"rot": [{"t": 0, "q": qx(6)}]},
            "armU.L": {"rot": [{"t": 0, "q": qx(-88), "ease": "quadInOut"},
                               {"t": 350, "q": qx(-100), "ease": "quadInOut"},
                               {"t": 700, "q": qx(-88)}]},
            "armU.R": {"rot": [{"t": 0, "q": qx(-88), "ease": "quadInOut"},
                               {"t": 350, "q": qx(-100), "ease": "quadInOut"},
                               {"t": 700, "q": qx(-88)}]},
        },
    }
    clips["land"] = {
        "durationMs": 420, "loop": False, "mode": "additive",
        "blendInMs": 40, "blendOutMs": 180,
        "mask": ["torso", "hips", "armU.L", "armU.R"],
        "tracks": {
            "hips": {"pos": [{"t": 0, "v": [0, 0, 0], "ease": "cubicOut"},
                             {"t": 110, "v": [0, -0.4, 0], "ease": "cubicInOut"},
                             {"t": 420, "v": [0, 0, 0]}]},
            "torso": {"rot": [{"t": 0, "q": qx(0), "ease": "cubicOut"},
                              {"t": 110, "q": qx(14), "ease": "cubicInOut"},
                              {"t": 420, "q": qx(0)}]},
            "armU.L": {"rot": [{"t": 0, "q": qx(0), "ease": "cubicOut"},
                               {"t": 110, "q": qx(-40), "ease": "cubicInOut"},
                               {"t": 420, "q": qx(0)}]},
            "armU.R": {"rot": [{"t": 0, "q": qx(0), "ease": "cubicOut"},
                               {"t": 110, "q": qx(-40), "ease": "cubicInOut"},
                               {"t": 420, "q": qx(0)}]},
        },
    }

    clips["cast"] = {
        "durationMs": 560, "loop": False, "mode": "override",
        "blendInMs": 80, "blendOutMs": 220,
        "mask": ["armU.R", "armL.R", "hand.R", "torso"],
        "tracks": {
            "armU.R": {"rot": [{"t": 0, "q": qx(0), "ease": "cubicOut"},
                               {"t": 150, "q": qx(-95), "ease": "cubicInOut"},
                               {"t": 300, "q": qx(-74), "ease": "cubicInOut"},
                               {"t": 560, "q": qx(0)}]},
            "armL.R": {"rot": [{"t": 0, "q": qx(-18), "ease": "cubicOut"},
                               {"t": 150, "q": qx(-52), "ease": "cubicInOut"},
                               {"t": 300, "q": qx(-10), "ease": "cubicInOut"},
                               {"t": 560, "q": qx(-18)}]},
            "hand.R": {"rot": [{"t": 0, "q": ident}]},
            "torso": {"rot": [{"t": 0, "q": qy(0), "ease": "cubicOut"},
                              {"t": 150, "q": qy(-14), "ease": "cubicInOut"},
                              {"t": 560, "q": qy(0)}]},
        },
    }
    clips["attack"] = clips["cast"]

    # ---- dismemberment clips ----
    clips["limp"] = {
        "durationMs": 1000, "loop": True, "mode": "additive",
        "blendInMs": 300, "blendOutMs": 300,
        "mask": ["torso", "hips", "armU.L", "armU.R"],
        "tracks": {
            "torso": {"rot": [{"t": 0, "q": qx(16), "ease": "quadInOut"},
                              {"t": 500, "q": qx(9), "ease": "quadInOut"},
                              {"t": 1000, "q": qx(16)}]},
            "hips": {"rot": [{"t": 0, "q": qz(7), "ease": "quadInOut"},
                             {"t": 500, "q": qz(-3), "ease": "quadInOut"},
                             {"t": 1000, "q": qz(7)}],
                     "pos": [{"t": 0, "v": [0, -0.3, 0], "ease": "quadInOut"},
                             {"t": 500, "v": [0, -0.08, 0], "ease": "quadInOut"},
                             {"t": 1000, "v": [0, -0.3, 0]}]},
            "armU.L": {"rot": [{"t": 0, "q": qx(-26), "ease": "quadInOut"},
                               {"t": 500, "q": qx(-8), "ease": "quadInOut"},
                               {"t": 1000, "q": qx(-26)}]},
            "armU.R": {"rot": [{"t": 0, "q": qx(-14)}]},
        },
    }

    clips["hop"] = {
        "durationMs": 760, "loop": True, "mode": "override",
        "blendInMs": 220,
        "mask": ["torso", "hips", "legU.L", "legL.L", "legU.R", "legL.R",
                 "armU.L", "armU.R"],
        "tracks": {
            "hips": {"pos": [{"t": 0, "v": [0, 0, 0], "ease": "cubicOut"},
                             {"t": 260, "v": [0, 0.9, 0], "ease": "cubicInOut"},
                             {"t": 520, "v": [0, 0, 0], "ease": "cubicIn"},
                             {"t": 760, "v": [0, 0, 0]}],
                     "rot": [{"t": 0, "q": qx(10), "ease": "cubicInOut"},
                             {"t": 260, "q": qx(-6), "ease": "cubicInOut"},
                             {"t": 760, "q": qx(10)}]},
            "torso": {"rot": [{"t": 0, "q": qx(12), "ease": "cubicInOut"},
                              {"t": 260, "q": qx(-4), "ease": "cubicInOut"},
                              {"t": 760, "q": qx(12)}]},
            "legU.L": {"rot": [{"t": 0, "q": qx(28), "ease": "cubicInOut"},
                               {"t": 260, "q": qx(-14), "ease": "cubicInOut"},
                               {"t": 760, "q": qx(28)}]},
            "legU.R": {"rot": [{"t": 0, "q": qx(28), "ease": "cubicInOut"},
                               {"t": 260, "q": qx(-14), "ease": "cubicInOut"},
                               {"t": 760, "q": qx(28)}]},
            "legL.L": {"rot": [{"t": 0, "q": qx(-46), "ease": "cubicInOut"},
                               {"t": 260, "q": qx(-16), "ease": "cubicInOut"},
                               {"t": 760, "q": qx(-46)}]},
            "legL.R": {"rot": [{"t": 0, "q": qx(-46), "ease": "cubicInOut"},
                               {"t": 260, "q": qx(-16), "ease": "cubicInOut"},
                               {"t": 760, "q": qx(-46)}]},
            "armU.L": {"rot": [{"t": 0, "q": qx(-30), "ease": "cubicInOut"},
                               {"t": 260, "q": qx(-62), "ease": "cubicInOut"},
                               {"t": 760, "q": qx(-30)}]},
            "armU.R": {"rot": [{"t": 0, "q": qx(-30), "ease": "cubicInOut"},
                               {"t": 260, "q": qx(-62), "ease": "cubicInOut"},
                               {"t": 760, "q": qx(-30)}]},
        },
    }

    clips["crawl"] = {
        "durationMs": 1200, "loop": True, "mode": "override",
        "blendInMs": 260,
        "mask": ["torso", "hips", "head",
                 "armU.L", "armL.L", "armU.R", "armL.R",
                 "legU.L", "legL.L", "legU.R", "legL.R"],
        "tracks": {
            "hips": {"rot": [{"t": 0, "q": qx(74), "ease": "quadInOut"},
                             {"t": 600, "q": qx(80), "ease": "quadInOut"},
                             {"t": 1200, "q": qx(74)}]},
            "torso": {"rot": [{"t": 0, "q": qy(-9), "ease": "quadInOut"},
                              {"t": 600, "q": qy(9), "ease": "quadInOut"},
                              {"t": 1200, "q": qy(-9)}]},
            "head": {"rot": [{"t": 0, "q": qx(-48), "ease": "quadInOut"},
                             {"t": 600, "q": qx(-38), "ease": "quadInOut"},
                             {"t": 1200, "q": qx(-48)}]},
            "armU.L": {"rot": [{"t": 0, "q": qx(-96), "ease": "quadInOut"},
                               {"t": 600, "q": qx(-30), "ease": "quadInOut"},
                               {"t": 1200, "q": qx(-96)}]},
            "armU.R": {"rot": [{"t": 0, "q": qx(-30), "ease": "quadInOut"},
                               {"t": 600, "q": qx(-96), "ease": "quadInOut"},
                               {"t": 1200, "q": qx(-30)}]},
            "armL.L": {"rot": [{"t": 0, "q": qx(-40), "ease": "quadInOut"},
                               {"t": 600, "q": qx(-70), "ease": "quadInOut"},
                               {"t": 1200, "q": qx(-40)}]},
            "armL.R": {"rot": [{"t": 0, "q": qx(-70), "ease": "quadInOut"},
                               {"t": 600, "q": qx(-40), "ease": "quadInOut"},
                               {"t": 1200, "q": qx(-70)}]},
            "legU.L": {"rot": [{"t": 0, "q": qx(-8)}]},
            "legU.R": {"rot": [{"t": 0, "q": qx(-8)}]},
            "legL.L": {"rot": [{"t": 0, "q": qx(-14)}]},
            "legL.R": {"rot": [{"t": 0, "q": qx(-14)}]},
        },
    }

    clips["squirm"] = {
        "durationMs": 1600, "loop": True, "mode": "override",
        "blendInMs": 300,
        "mask": ["torso", "hips", "head"],
        "tracks": {
            "hips": {"rot": [{"t": 0, "q": qx(84), "ease": "quadInOut"},
                             {"t": 800, "q": qx(88), "ease": "quadInOut"},
                             {"t": 1600, "q": qx(84)}]},
            "torso": {"rot": [{"t": 0, "q": qy(-16), "ease": "quadInOut"},
                              {"t": 800, "q": qy(16), "ease": "quadInOut"},
                              {"t": 1600, "q": qy(-16)}]},
            "head": {"rot": [{"t": 0, "q": qx(-30), "ease": "quadInOut"},
                             {"t": 800, "q": qx(-20), "ease": "quadInOut"},
                             {"t": 1600, "q": qx(-30)}]},
        },
    }

    clips["onearm"] = {
        "durationMs": 1400, "loop": True, "mode": "additive",
        "blendInMs": 260, "blendOutMs": 260,
        "mask": ["torso", "armU.L", "armU.R"],
        "tracks": {
            "torso": {"rot": [{"t": 0, "q": qz(5), "ease": "quadInOut"},
                              {"t": 700, "q": qz(2), "ease": "quadInOut"},
                              {"t": 1400, "q": qz(5)}]},
            "armU.L": {"rot": [{"t": 0, "q": qx(-6), "ease": "quadInOut"},
                               {"t": 700, "q": qx(4), "ease": "quadInOut"},
                               {"t": 1400, "q": qx(-6)}]},
            "armU.R": {"rot": [{"t": 0, "q": qx(-6), "ease": "quadInOut"},
                               {"t": 700, "q": qx(4), "ease": "quadInOut"},
                               {"t": 1400, "q": qx(-6)}]},
        },
    }

    clips["headless"] = {
        "durationMs": 900, "loop": True, "mode": "additive",
        "blendInMs": 120, "blendOutMs": 200,
        "mask": ["torso", "armU.L", "armU.R", "armL.L", "armL.R"],
        "tracks": {
            "torso": {"rot": [{"t": 0, "q": qz(-4), "ease": "quadInOut"},
                              {"t": 450, "q": qz(4), "ease": "quadInOut"},
                              {"t": 900, "q": qz(-4)}]},
            "armU.L": {"rot": [{"t": 0, "q": qx(-70), "ease": "quadInOut"},
                               {"t": 450, "q": qx(-95), "ease": "quadInOut"},
                               {"t": 900, "q": qx(-70)}]},
            "armU.R": {"rot": [{"t": 0, "q": qx(-95), "ease": "quadInOut"},
                               {"t": 450, "q": qx(-70), "ease": "quadInOut"},
                               {"t": 900, "q": qx(-95)}]},
            "armL.L": {"rot": [{"t": 0, "q": qx(-60)}]},
            "armL.R": {"rot": [{"t": 0, "q": qx(-60)}]},
        },
    }

    # ---- dismemberment states ----
    states = [
        {"name": "squirm",
         "missing": ["legU.L", "legU.R"],
         "missingAny": ["armU.L", "armU.R"],
         "clip": "squirm", "speedScale": 0.12,
         "disableGait": True, "bodyYOffset": -0.35},
        {"name": "crawl",
         "missing": ["legU.L", "legU.R"],
         "clip": "crawl", "speedScale": 0.3,
         "disableGait": True, "bodyYOffset": -0.32},
        {"name": "crawl.shins",
         "missing": ["legL.L", "legL.R"],
         "clip": "crawl", "speedScale": 0.3,
         "disableGait": True, "bodyYOffset": -0.32},
        {"name": "hop",
         "missing": ["foot.L", "foot.R"],
         "clip": "hop", "speedScale": 0.45,
         "disableGait": True, "bodyYOffset": -0.1},
        {"name": "limp",
         "missingAny": ["legU.L", "legL.L", "foot.L",
                        "legU.R", "legL.R", "foot.R"],
         "clip": "limp", "speedScale": 0.55},
        {"name": "onearm",
         "missingAny": ["armU.L", "armL.L", "hand.L",
                        "armU.R", "armL.R", "hand.R"],
         "clip": "onearm", "speedScale": 0.92},
        {"name": "headless",
         "missing": ["head"],
         "clip": "headless", "speedScale": 0.7},
    ]

    sidecar = {
        "root": "hips",
        "skinScale": SCALE,
        "bleed": {"material": "blood", "perDamage": 2.5},
        "speed": SPRINT_SPEED_MPS / VOXEL_METERS,
        "gait": {
            "groups": [["legU.L"], ["legU.R"]],
            "cadence": 8.0, "strideBias": 0.42, "leadTime": 0.10,
            "stepThreshold": 0.3, "stepDuration": 0.10, "stepHeight": 0.18,
            "rideHeight": 1.18,
            "bobAmp": 0.045, "bobFreqMul": 2.0, "swayAmp": 0.03,
            "rollAmp": 0.06, "spineCounter": 0.75, "phaseLag": 0.05,
        },
        "limbs": limbs,
        "sockets": sockets,
        "chains": chains,
        "states": states,
        "clips": scale_clip_positions(clips, SKIN_UPSCALE),
        "flipbooks": {},
    }

    with open(os.path.join(out_dir, "asha.json"), "w") as f:
        json.dump(sidecar, f, indent=2)
        f.write("\n")

    total = sum(len(SHAPES[n](art_size) if n in SHAPES else
                    solid(art_size, mat))
                for n in ORDER
                for art_size, _, mat in [_ART_LIMBS[n]])
    print(f"wrote asha.vox ({len(ORDER)} parts, ~{total} micro voxels, "
          f"{WORLD_H} world voxels tall) and asha.json "
          f"({len(clips)} clips, {len(states)} states)")


if __name__ == "__main__":
    main()
