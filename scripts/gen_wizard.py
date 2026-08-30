#!/usr/bin/env python3
"""Generate assets/mobs/wizard.{vox,json} — a robed wizard/witch rigged for
dismemberment.

NOT THE CURRENT PLAYER AVATAR. That is gen_mina.py; this file is kept as a
second character. It is also MIS-SCALED for the player slot: the sizes below
were authored assuming kVoxelMeters = 0.0625, but sim/world.h has long said
0.10, which makes this figure 28 world voxels = 2.8 m tall against a 1.7 m
player box — it towered over its own collider and the first-person camera sat
inside its chest. Fix the height budget (or scale every z by 17/28) before
pointing kAvatarDefName at it again.

WHY THIS IS A MOB DEF. The avatar is authored in exactly the same format as
critter/dummy and loaded by the same LoadMobDefs path, so it inherits the whole
animation runtime for free: two-bone IK, the gait state machine, springs,
clips, flipbooks and — the reason this file exists — `states`, the
dismemberment locomotion rules (src/game/anim.h AnimStateRule). What it does
NOT inherit is the mob AI: src/game/avatar.cpp drives this rig from the
Player's own position and input instead of MobSystem's wander drive. One
schema, two drivers.

SCALE 4. Every coordinate below is in MICRO units, 4 per world voxel, so the
numbers here are 4x their world-voxel values. A micro voxel is a quarter of a
world voxel — fine enough for individual fingers, a hat brim, a beard and robe
folds. The engine divides every POSITION by `scale` at load and builds the
colliders at pitch 1/4. (The original comment here claimed this made a ~1.75 m
human; that arithmetic used kVoxelMeters = 0.0625. See the header note.)

BODY PLAN — 17 severable parts, which is the whole point of the exercise:

    head                    (hat + face + beard, one piece)
    torso                   (chest, root of the rig)
    hips                    (pelvis + the robe skirt)
    armU.L/R  armL.L/R      upper + fore arm, per side
    hand.L/R                independent hands (severable at the wrist)
    legU.L/R  legL.L/R      thigh + shin, per side
    foot.L/R                independent feet
    staff                   held in the right hand; falls when that hand goes

FACING CONVENTION (load-bearing — see gen_critter_mob.py for the full
derivation). The engine's heading is fwd = (sin h, 0, cos h), so heading 0
travels along engine +Z, and a model must have its FRONT on engine +Z. The
loader maps scene -> engine as (x, z, -y), which negates scene y, so
"front on engine +Z" means "front on scene -Y". Everything below is authored
with the face/toes toward scene -Y.

HANDEDNESS CONVENTION (separate from facing, and NOT derivable from it).
Model +X is the character's LEFT. Camera::Right() = Forward() x (0,1,0)
(game/camera.cpp:23) composed with the rig heading h = pi/2 - cam.yaw gives
dot(model +X in world, camera Right) = -1.000 at every heading. The scene ->
engine map (x, z, -y) negates y, which flips handedness, so the intuitive
"it faces -Y, therefore +X is its right" is wrong — that reasoning had every
.L/.R limb on the wrong side of both characters until 2026-08-21. So .L limbs
sit at POSITIVE scene x and .R limbs at negative.

Run:  python scripts/gen_wizard.py
"""
import json
import math
import os
import struct

# ---- palette (materials.json ids; palette index == material ID) -------------
ROBE = 48       # robe_cloth   deep indigo
TRIM = 49       # robe_trim    gold, faintly emissive
SHADE = 50      # robe_shadow  darker indigo, for folds and the hat underside
SKIN = 51       # skin
HAIR = 52       # hair_white   beard, brows
LEATHER = 53    # leather      belt, boots, straps
STAFF = 54      # staff_wood
GEM = 55        # gem_arcane   staff crystal + eyes, strongly emissive
BONE = 56       # bone         (unused in the skin; kept for gore/props)

SCALE = 4  # micro voxels per world voxel


# ---- .vox writing (same helpers as gen_critter_mob.py) ----------------------
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
# All builders are written in the natural "front = +y" reading (face and toes at
# the HIGH-y end) and mirrored once through flip_y at the end. Authoring forward
# and mirroring once is far easier to check by eye than writing every index
# backwards, and it keeps shape and placement using ONE idea of "front".
def flip_y(size, voxels):
    sy = size[1]
    return [(x, sy - 1 - y, z, c) for (x, y, z, c) in voxels]


def solid(size, mat):
    sx, sy, sz = size
    return [(x, y, z, mat)
            for z in range(sz) for y in range(sy) for x in range(sx)]


def centre(size):
    """Centre of a voxel box in the SAME space ellipse_mask compares against.

    Voxel i covers [i, i+1) and its centre is at i+0.5, so the centre of an
    n-wide box is at n*0.5 in continuous space. Passing n*0.5 to ellipse_mask
    (which adds the 0.5 itself) is therefore correct — but writing it out here
    once, and calling this everywhere, is what stops each builder from
    inventing its own half-voxel convention. Getting it wrong shifts a
    cross-section half a voxel and clips the far side of the widest ring,
    which on a cone (the robe skirt) is the most visible ring there is."""
    return size * 0.5


def ellipse_mask(x, y, cx, cy, rx, ry):
    """True when voxel (x,y)'s CENTRE is inside the axis-aligned ellipse. Used
    to round every cross-section: a robed figure is all cylinders and cones,
    and at 4x there are finally enough voxels for a circle to read as one."""
    if rx <= 0 or ry <= 0:
        return False
    dx = (x + 0.5 - cx) / rx
    dy = (y + 0.5 - cy) / ry
    return dx * dx + dy * dy <= 1.0


def tapered_tube(size, mat, r0, r1, trim_rows=(), shade_rows=()):
    """A vertical (z-axis) tube whose XY radius lerps from r0 at the bottom to
    r1 at the top. This is the workhorse: arms, legs and the robe skirt are all
    tapered tubes with different end radii.

    trim_rows / shade_rows are z indices repainted in TRIM / SHADE, which is how
    cuffs, hems and folds get expressed without a second model."""
    sx, sy, sz = size
    cx, cy = sx * 0.5, sy * 0.5
    out = []
    for z in range(sz):
        t = z / max(sz - 1, 1)
        r = r0 + (r1 - r0) * t
        m = TRIM if z in trim_rows else (SHADE if z in shade_rows else mat)
        for y in range(sy):
            for x in range(sx):
                if ellipse_mask(x, y, cx, cy, r, r):
                    out.append((x, y, z, m))
    return out


# ---- per-limb builders ------------------------------------------------------
def head_vox(size):
    """Hat cone + head + beard + gem eyes, all in one model.

    The head is the single most detailed part and the clearest argument for
    microvoxels: a pointed brimmed hat, a nose, browridge, two glowing eye
    pixels and a beard that tapers to a point all live inside a box that is
    only 5x5x8 WORLD voxels. At scale 1 this whole thing would be five voxels
    tall and none of it would exist."""
    sx, sy, sz = size
    out = []
    cx, cy = sx * 0.5, sy * 0.5
    # z layout, bottom (z=0) to top. The head model's own origin sits at the
    # BASE OF THE BEARD, which hangs below the chin — so the neck joint anchor
    # is up at `face_lo`, not at z=0. Getting this wrong floats the head.
    #   0..beard_hi   beard, tapering DOWN to a point (front half only)
    #   face_lo..face_hi  skull + face
    #   brim_z        hat brim (one wide disc)
    #   brim_z+1..    hat cone
    beard_hi = 7
    face_lo, face_hi = 6, 17
    brim_z = 18
    for z in range(sz):
        for y in range(sy):
            for x in range(sx):
                mat = None
                if z <= beard_hi and z < face_lo:
                    # beard below the chin: widens as it rises toward the jaw,
                    # and only on the FRONT half (high y before the flip)
                    t = z / max(beard_hi, 1)
                    r = 1.2 + 2.2 * t
                    if y >= cy - 1.4 and ellipse_mask(x, y, cx, cy + 0.8, r, r):
                        mat = HAIR
                elif z <= face_hi:
                    # head proper: a rounded box, slightly narrower at the crown
                    t = (z - face_lo) / max(face_hi - face_lo, 1)
                    r = 3.5 - 0.7 * max(0.0, t - 0.6)
                    if ellipse_mask(x, y, cx, cy, r, r):
                        mat = SKIN
                        # beard continues UP the jawline over the lower face,
                        # and hair covers the BACK and CROWN of the skull
                        if z <= beard_hi + 2 and y >= cy - 0.5:
                            mat = HAIR
                        elif y <= cy - 2.2:
                            mat = HAIR
                        elif z >= face_hi - 1:
                            mat = HAIR
                elif z == brim_z:
                    # hat brim: wide flat disc, slightly drooped at the front
                    if ellipse_mask(x, y, cx, cy, sx * 0.5, sy * 0.5):
                        mat = SHADE
                else:
                    # hat cone, leaning back (low y) as it rises
                    t = (z - brim_z) / max(sz - 1 - brim_z, 1)
                    r = 3.6 * (1.0 - t) + 0.4
                    lean = -1.8 * t  # tip drifts toward the BACK
                    if ellipse_mask(x, y, cx, cy + lean, r, r):
                        mat = ROBE
                        # gold band just above the brim
                        if z <= brim_z + 2:
                            mat = TRIM
                if mat is not None:
                    out.append((x, y, z, mat))
    # Features punched in afterwards as an O(n) rewrite pass, so they always
    # land on voxels that actually exist rather than floating beside the head.
    # Features punched in afterwards as an O(n) rewrite pass, so they always
    # land on voxels that actually exist rather than floating beside the head.
    # eye_z sits in the upper half of the face band, clear of the beard.
    eye_z = face_hi - 3
    feat = {}
    ex = (int(cx - 2), int(cx + 1))
    for x in ex:
        for y in (sy - 2, sy - 3):
            feat[(x, y, eye_z)] = GEM        # glowing eyes, under the brow
        feat[(x, sy - 2, eye_z + 1)] = HAIR  # white brows above them
    out = [(x, y, z, feat.get((x, y, z), m)) for (x, y, z, m) in out]
    # the nose is proud of the face, so it must be ADDED, not just repainted
    have = {(x, y, z) for (x, y, z, _m) in out}
    for x in (int(cx - 1), int(cx)):
        for z in (eye_z - 1, eye_z - 2):
            if (x, sy - 1, z) not in have:
                out.append((x, sy - 1, z, SKIN))
    return flip_y(size, out)


def torso_vox(size):
    """Chest under a robe, with a gold-trimmed collar, a lapel V down the
    front and shoulder taper."""
    sx, sy, sz = size
    cx, cy = sx * 0.5, sy * 0.5
    out = []
    for z in range(sz):
        t = z / max(sz - 1, 1)
        # narrow at the waist, broad at the shoulders, tucked at the very top
        rx = 3.2 + 1.6 * t - (1.2 if z >= sz - 2 else 0.0)
        ry = 2.2 + 0.9 * t
        for y in range(sy):
            for x in range(sx):
                if not ellipse_mask(x, y, cx, cy, rx, ry):
                    continue
                mat = ROBE
                if z >= sz - 2:
                    mat = TRIM                      # collar
                elif y > cy and abs(x - cx) < 1.2 + 2.0 * (1.0 - t):
                    mat = TRIM if z > sz - 6 else SHADE   # lapel V
                elif y <= cy - 1.5:
                    mat = SHADE                     # shaded back
                out.append((x, y, z, mat))
    return flip_y(size, out)


def hips_vox(size):
    """Pelvis plus the flared robe SKIRT — the silhouette that says 'wizard'.

    Authored as a cone that widens downward (z=0 is the hem). The belt is a
    leather band with a gold buckle at the top."""
    sx, sy, sz = size
    cx, cy = centre(sx), centre(sy)
    out = []
    # The skirt must FLARE PAST THE LEGS or it reads as trousers: the hem is
    # the full width of the declared box, the waist is hip-width.
    hem_r, waist_r = sx * 0.5, 3.4
    for z in range(sz):
        t = z / max(sz - 1, 1)
        r = hem_r + (waist_r - hem_r) * t   # widest at the hem (z=0)
        for y in range(sy):
            for x in range(sx):
                if not ellipse_mask(x, y, cx, cy, r, r * 0.8):
                    continue
                mat = ROBE
                if z == 0:
                    mat = TRIM                      # gold hem
                elif z >= sz - 2:
                    mat = LEATHER                   # belt
                elif y <= cy - 1.5:
                    mat = SHADE
                # vertical fold lines: a few columns dropped into shadow read
                # as gathered cloth once the skirt is in motion
                elif (x + z // 3) % 5 == 0 and y > cy - 1:
                    mat = SHADE
                out.append((x, y, z, mat))
    # buckle, on the front face of the belt
    for x in (int(cx) - 1, int(cx)):
        out.append((x, sy - 1, sz - 2, TRIM))
    return flip_y(size, out)


def upper_arm_vox(size):
    """Robed upper arm: a tapered sleeve, wider at the shoulder."""
    return flip_y(size, tapered_tube(size, ROBE, 2.4, 1.9,
                                     shade_rows=(0,)))


def fore_arm_vox(size):
    """Forearm: the sleeve FLARES toward the wrist (the classic wizard cuff),
    with a gold band at the opening."""
    sx, sy, sz = size
    out = tapered_tube(size, ROBE, 1.6, 2.8, trim_rows=(0, 1))
    return flip_y(size, out)


def hand_vox(size):
    """A hand with four fingers and a thumb — SEPARATE voxels with gaps
    between them. This is the detail that only exists at 4x: at world
    resolution a hand is one voxel."""
    sx, sy, sz = size
    out = []
    cx, cy = sx * 0.5, sy * 0.5
    # palm: the top rows (z high = wrist end)
    for z in range(sz - 3, sz):
        for y in range(sy):
            for x in range(sx):
                if ellipse_mask(x, y, cx, cy, 2.2, 1.6):
                    out.append((x, y, z, SKIN))
    # four fingers hanging down (low z), one column each, separated by gaps
    for i, fx in enumerate((0, 2, 4, 6)):
        length = 3 if i in (1, 2) else 2      # middle fingers longest
        for z in range(sz - 3 - length, sz - 3):
            out.append((fx, int(cy), z, SKIN))
            out.append((fx, int(cy) + 1, z, SKIN))
    # thumb: off the front-inside edge, one voxel proud
    for z in range(sz - 4, sz - 2):
        out.append((int(cx) + 2, sy - 1, z, SKIN))
    return flip_y(size, out)


def thigh_vox(size):
    """Upper leg, robed. Hidden by the skirt when standing, visible the moment
    the wizard runs or the skirt is cut away."""
    return flip_y(size, tapered_tube(size, ROBE, 2.3, 2.0, shade_rows=(0,)))


def shin_vox(size):
    """Lower leg: cloth over a leather legging at the bottom."""
    sx, sy, sz = size
    out = tapered_tube(size, ROBE, 1.7, 2.0)
    out = [(x, y, z, LEATHER if z < 3 else m) for (x, y, z, m) in out]
    return flip_y(size, out)


def foot_vox(size):
    """A pointed boot: sole spreading forward into a curled toe, with a gold
    buckle on the instep. The curl is what makes it read as a wizard's boot
    rather than a block."""
    sx, sy, sz = size
    out = []
    cx = sx * 0.5
    for z in range(sz):
        # the boot leans forward as it rises toward the ankle
        for y in range(sy):
            for x in range(sx):
                # ankle column at the BACK (low y), sole across the whole length
                in_sole = z < 2
                in_ankle = y < 3 and z >= 2
                if not (in_sole or in_ankle):
                    continue
                if not ellipse_mask(x, y if in_ankle else 2, cx,
                                    2.0 if in_ankle else 2.0, 2.2, 2.6):
                    if in_ankle:
                        continue
                    if abs(x - cx) > 2.2:
                        continue
                out.append((x, y, z, LEATHER))
    # curled toe tip: one voxel rising at the very front
    for x in (int(cx - 1), int(cx)):
        out.append((x, sy - 1, 2, LEATHER))
    # buckle on the instep
    for x in (int(cx - 1), int(cx)):
        out.append((x, 2, 3, TRIM))
    return flip_y(size, out)


def staff_vox(size):
    """A gnarled staff topped with a glowing crystal. Held in hand.R; it is a
    normal severable part, so losing that hand drops the staff as debris."""
    sx, sy, sz = size
    out = []
    cx, cy = sx * 0.5, sy * 0.5
    for z in range(sz - 4):
        # a slight kink so it does not read as a dowel
        off = 0.6 * math.sin(z * 0.25)
        for y in range(sy):
            for x in range(sx):
                if ellipse_mask(x, y, cx + off, cy, 1.15, 1.15):
                    out.append((x, y, z, STAFF))
    # crystal: a small octahedron at the top
    top = sz - 4
    for z in range(4):
        r = 2.2 - abs(z - 1.5) * 0.7
        for y in range(sy):
            for x in range(sx):
                if ellipse_mask(x, y, cx, cy, r, r):
                    out.append((x, y, top + z, GEM))
    return out  # radially symmetric: no flip needed


# ---- limb table -------------------------------------------------------------
# name -> (size, min-corner in SCENE space, default material)
#
# MICRO units (4 per world voxel). Scene is Z-up; engine Y = scene Z, so the
# z column below IS the height. The figure stands with feet at z=0 and the hat
# tip near z=112, i.e. 28 world voxels to the crown.
#
# Front is scene -Y throughout (see the module docstring).
# HEIGHT BUDGET (micro units, 4 per world voxel). The player AABB is 1.7 m
# tall (Player::kHalfY = 0.85 m), so an avatar must stand very close to that or
# it visibly floats above / sinks into its own collision box. THIS RIG DOES
# NOT: 28 world voxels is 1.75 m only at kVoxelMeters 0.0625, and the engine
# runs at 0.10, making it 2.8 m. See the header note before using it as the
# player. gen_mina.py derives its budget from the constants instead and
# asserts it.
#
#   feet    z  0..6     boots
#   shin    z  5..25    (overlaps the boot cuff)
#   thigh   z 24..44
#   hips    z 42..60    pelvis + skirt (skirt hem hangs to z 42)
#   torso   z 58..84
#   head    z 78..118   beard base 78, chin ~84, crown ~112, hat tip 118
LIMBS = {
    #                size          min corner (x, y, z)
    "hips":     ((16, 13, 18), (-8, -6, 42), ROBE),
    "torso":    ((13, 11, 26), (-6, -5, 58), ROBE),
    "head":     ((12, 12, 40), (-6, -6, 78), SKIN),

    # HANDEDNESS: model +X is the character's LEFT, so .L limbs take the
    # POSITIVE x offsets and .R the negative. Camera::Right() (camera.cpp:23)
    # composed with the rig heading h = pi/2 - cam.yaw gives
    # dot(model +X in world, camera Right) = -1 at every heading; the scene ->
    # engine map (x, z, -y) negates y and so flips handedness, which is why
    # "the figure faces -Y therefore +X is its right" does NOT hold.
    "armU.L":   (( 6,  6, 16), (  5, -3, 68), ROBE),
    "armL.L":   (( 7,  7, 15), (  4, -3, 53), ROBE),
    "hand.L":   (( 7,  6,  9), (  4, -3, 44), SKIN),
    "armU.R":   (( 6,  6, 16), (-11, -3, 68), ROBE),
    "armL.R":   (( 7,  7, 15), (-11, -3, 53), ROBE),
    "hand.R":   (( 7,  6,  9), (-11, -3, 44), SKIN),

    "legU.L":   (( 7,  7, 20), ( 0, -3, 24), ROBE),
    "legL.L":   (( 6,  6, 20), ( 0, -3,  5), ROBE),
    "foot.L":   (( 6, 10,  7), ( 0, -7,  0), LEATHER),
    "legU.R":   (( 7,  7, 20), (-7, -3, 24), ROBE),
    "legL.R":   (( 6,  6, 20), (-6, -3,  5), ROBE),
    "foot.R":   (( 6, 10,  7), (-6, -7,  0), LEATHER),

    # Held in hand.R, which is now at model -X (the figure's right).
    #
    # ITS X IS PINNED TO THE BODY'S MINIMUM (-11, hand.R's own low corner) and
    # that is a correctness constraint, not styling. Anchors are rebased on the
    # BODY's min corner — props are excluded from that measurement, since a
    # creature's size must not change with what it carries — so a prop reaching
    # below it lands at NEGATIVE prefab-local coordinates, which that space
    # cannot represent. At -14 this staff did exactly that (engine x -3), the
    # same defect that put mina's sword 30 micro outside her box and drew her
    # body offset from where her arm solved.
    #
    # The real fix for a WEAPON is to make it a standalone item with its own
    # origin (assets/items/, scripts/gen_sword_item.py) held through a socket,
    # which is what the sword did. This staff stays a rig prop for now — the
    # wizard is not the player avatar (main.cpp kAvatarDefName is "mina") and
    # it is keyframed by name in the cast clips — so it is simply kept inboard:
    # centred in the fist rather than hanging off its outer edge.
    "staff":    (( 5,  5, 56), (-11, -2, 26), STAFF),
}

# Held props. Excluded from the prefab extents below because the LOADER
# excludes them (mob.cpp skips `tag == "prop"` when measuring worldSize) — see
# the note by min_x. A prop must never redefine the body's frame.
PROPS = {"staff"}

SHAPES = {
    "hips": hips_vox, "torso": torso_vox, "head": head_vox,
    "armU.L": upper_arm_vox, "armU.R": upper_arm_vox,
    "armL.L": fore_arm_vox, "armL.R": fore_arm_vox,
    "hand.L": hand_vox, "hand.R": hand_vox,
    "legU.L": thigh_vox, "legU.R": thigh_vox,
    "legL.L": shin_vox, "legL.R": shin_vox,
    "foot.L": foot_vox, "foot.R": foot_vox,
    "staff": staff_vox,
}

# Parent-before-child; the loader also topologically sorts, but authoring in
# order keeps the .vox scene graph readable in MagicaVoxel.
ORDER = ["hips", "torso", "head",
         "armU.L", "armL.L", "hand.L",
         "armU.R", "armL.R", "hand.R",
         "legU.L", "legL.L", "foot.L",
         "legU.R", "legL.R", "foot.R",
         "staff"]


def to_engine(scene_xyz, min_x, max_y):
    """Scene (Z-up) -> prefab-local engine coords (Y-up, min corner 0), matching
    voxload.cpp: engine = (x, z, -y), rebased so the prefab min corner is 0."""
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
        # DebrisVoxel is int8, and these are MICRO units: 120 micro = 30 world
        # voxels per axis. The tallest part here is the 40-deep staff.
        assert max(size) <= 120, f"{name} exceeds DebrisVoxel int8 range"
        voxels = SHAPES[name](size) if name in SHAPES else solid(size, mat)
        assert voxels, f"{name} generated no voxels"
        seen = set()
        uniq = []
        for v in voxels:
            x, y, z, _c = v
            assert 0 <= x < size[0] and 0 <= y < size[1] and 0 <= z < size[2], \
                f"{name} voxel {(x, y, z)} outside declared size {size}"
            if (x, y, z) in seen:
                continue          # later writes win; keep the first
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
    with open(os.path.join(out_dir, "wizard.vox"), "wb") as f:
        f.write(data)

    # prefab extents in scene space, for the anchor conversion.
    #
    # PROPS ARE EXCLUDED to MIRROR THE LOADER: mob.cpp (~L603) skips
    # `tag == "prop"` when measuring worldSize, so the engine's box is the
    # BODY's box. Measuring a different box here shifts every anchor by the
    # difference and puts the gait pivot (worldSize * 0.5) off the body.
    # The staff is the prop; keep this in step with the `"tag": "prop"` limb.
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

    def qxz(dx, dz):
        """X then Z, matching QuatFromEulerDeg's X->Y->Z order (anim.h)."""
        a, b = math.radians(dx) * 0.5, math.radians(dz) * 0.5
        sa, ca, sb, cb = math.sin(a), math.cos(a), math.sin(b), math.cos(b)
        # q = qz * qx  (Z applied last, i.e. outermost)
        return [round(sa * cb, 4), round(sa * sb, 4),
                round(ca * sb, 4), round(ca * cb, 4)]

    ident = [0.0, 0.0, 0.0, 1.0]

    # ---- joint anchors, DERIVED from the limb table ------------------------
    # A joint sits at the seam between a part and its parent, so restating its
    # height as a literal is a second source of truth that silently rots the
    # moment a limb is resized. `joint_top` puts the pivot at the TOP face of a
    # part (shoulder, hip, knee, elbow, wrist all work this way once the parts
    # are stacked), centred on the part in x/y unless told otherwise.
    def joint_top(part, dx=0.0, dy=0.0, inset=0.0):
        size, mn, _m = LIMBS[part]
        return anchor((mn[0] + size[0] * 0.5 + dx,
                       mn[1] + size[1] * 0.5 + dy,
                       mn[2] + size[2] - inset))

    def joint_bottom(part, dx=0.0, dy=0.0, rise=0.0):
        size, mn, _m = LIMBS[part]
        return anchor((mn[0] + size[0] * 0.5 + dx,
                       mn[1] + size[1] * 0.5 + dy,
                       mn[2] + rise))

    # ---- limbs ----
    # hp/severImpactSpeed are authored so extremities come off easily and the
    # torso does not: a graze takes a hand, only a serious hit takes a thigh.
    limbs = [
        {"name": "hips", "hp": 60, "severable": False, "tag": "spine"},
        # waist: the top of the hips block
        {"name": "torso", "parent": "hips", "joint": "ball", "hp": 60,
         "severable": False, "vital": True, "tag": "spine",
         "anchor": joint_top("hips", inset=2)},
        # neck: the top of the torso, NOT the base of the head model (the head
        # model's own origin is the bottom of the beard, which hangs lower)
        {"name": "head", "parent": "torso", "joint": "ball", "hp": 22,
         "severable": True, "vital": True, "tag": "head",
         "anchor": joint_top("torso", inset=1), "severImpactSpeed": 20.0,
         # the hat/beard mass jiggles when the head turns
         "spring": {"halflife": 0.12, "gain": 0.9, "maxAngle": 0.35}},
    ]
    # arms: shoulder -> elbow -> wrist, per side. Mirrored hinge limits so both
    # elbows bend the same way relative to the body.
    for side in ("L", "R"):
        limbs.append({
            "name": f"armU.{side}", "parent": "torso", "joint": "ball",
            "hp": 18, "severable": True, "tag": "arm",
            "anchor": joint_top(f"armU.{side}"),      # shoulder
            "severImpactSpeed": 15.0})
        limbs.append({
            "name": f"armL.{side}", "parent": f"armU.{side}", "joint": "hinge",
            "hp": 15, "severable": True, "tag": "arm", "axis": [1, 0, 0],
            "minAngle": -2.4, "maxAngle": 0.05,
            "anchor": joint_top(f"armL.{side}"),      # elbow
            "severImpactSpeed": 13.0})
        limbs.append({
            "name": f"hand.{side}", "parent": f"armL.{side}", "joint": "ball",
            "hp": 10, "severable": True, "tag": "hand",
            "anchor": joint_top(f"hand.{side}"),      # wrist
            "severImpactSpeed": 9.0})
    # legs: hip -> knee -> ankle, per side
    for side in ("L", "R"):
        limbs.append({
            "name": f"legU.{side}", "parent": "hips", "joint": "ball",
            "hp": 24, "severable": True, "tag": "leg",
            "anchor": joint_top(f"legU.{side}"),      # hip
            "severImpactSpeed": 18.0})
        limbs.append({
            "name": f"legL.{side}", "parent": f"legU.{side}", "joint": "hinge",
            "hp": 20, "severable": True, "tag": "leg", "axis": [1, 0, 0],
            "minAngle": -2.4, "maxAngle": 0.05,
            "anchor": joint_top(f"legL.{side}"),      # knee
            "severImpactSpeed": 16.0})
        limbs.append({
            "name": f"foot.{side}", "parent": f"legL.{side}", "joint": "hinge",
            "hp": 14, "severable": True, "tag": "foot", "axis": [1, 0, 0],
            "minAngle": -0.6, "maxAngle": 0.6,
            # ankle: above the sole, at the BACK of the boot (the toe reaches
            # forward from here), so the foot pivots the way a foot does
            "anchor": joint_bottom(f"foot.{side}", dy=-2.0, rise=4.0),
            "severImpactSpeed": 12.0})
    # the staff is a held prop: low hp, comes off easily, and is not vital.
    # Its grip is at the hand, partway UP the shaft, not at either end.
    limbs.append({
        "name": "staff", "parent": "hand.R", "joint": "ball", "hp": 12,
        "severable": True, "tag": "prop",
        "anchor": joint_bottom("staff", rise=20.0),
        "severImpactSpeed": 6.0,
        "spring": {"halflife": 0.1, "gain": 0.5, "maxAngle": 0.2}})

    # ---- IK chains ----
    # Legs are gait-driven. Arms get chains too so the avatar code can plant a
    # hand on a wall or aim the staff without a bespoke solver; weight is
    # driven to 0 automatically when a chain loses a part.
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
                       # elbows point BACK, so the pole is behind the figure
                       "pole": [0, 0, -1], "solver": "twobone"})

    # ---- clips ------------------------------------------------------------
    # Quaternions are (x, y, z, w). Every clip is masked to the parts it owns,
    # so e.g. `cast` can play over a walk without touching the legs.
    def track(rot_keys):
        return {"rot": rot_keys}

    def swing(part, deg, period=900, phase=0, ease="quadInOut"):
        """Simple two-extreme loop about X, `phase` in half-periods."""
        a, b = (deg, -deg) if phase == 0 else (-deg, deg)
        half = period // 2
        return {"rot": [{"t": 0, "q": qx(a), "ease": ease},
                        {"t": half, "q": qx(b), "ease": ease},
                        {"t": period, "q": qx(a)}]}

    clips = {}

    # idle: a slow breath in the spine plus a drifting staff hand
    clips["idle"] = {
        "durationMs": 3200, "loop": True, "mode": "additive",
        "blendInMs": 400, "blendOutMs": 400,
        "mask": ["torso", "head", "armU.L", "armU.R"],
        "tracks": {
            "torso": {"rot": [{"t": 0, "q": qx(0), "ease": "quadInOut"},
                              {"t": 1600, "q": qx(-1.6), "ease": "quadInOut"},
                              {"t": 3200, "q": qx(0)}],
                      "pos": [{"t": 0, "v": [0, 0, 0], "ease": "quadInOut"},
                              {"t": 1600, "v": [0, 0.12, 0], "ease": "quadInOut"},
                              {"t": 3200, "v": [0, 0, 0]}]},
            "head": {"rot": [{"t": 0, "q": qy(0), "ease": "quadInOut"},
                             {"t": 1100, "q": qy(4), "ease": "quadInOut"},
                             {"t": 2300, "q": qy(-3), "ease": "quadInOut"},
                             {"t": 3200, "q": qy(0)}]},
            "armU.L": {"rot": [{"t": 0, "q": qz(0), "ease": "quadInOut"},
                               {"t": 1600, "q": qz(2.5), "ease": "quadInOut"},
                               {"t": 3200, "q": qz(0)}]},
            "armU.R": {"rot": [{"t": 0, "q": qz(0), "ease": "quadInOut"},
                               {"t": 1600, "q": qz(-2.5), "ease": "quadInOut"},
                               {"t": 3200, "q": qz(0)}]},
        },
    }

    # walk/run arm swing: additive over the gait, masked to the arms only, so
    # the IK-driven legs are untouched. The staff arm swings less (it is
    # carrying something).
    for nm, deg, period in (("walk", 26, 900), ("run", 42, 620)):
        clips[nm] = {
            "durationMs": period, "loop": True, "mode": "additive",
            "blendInMs": 180, "blendOutMs": 180,
            "mask": ["armU.L", "armL.L", "armU.R", "armL.R", "torso"],
            "tracks": {
                "armU.L": swing("armU.L", deg, period, 0),
                "armU.R": swing("armU.R", deg * 0.45, period, 1),
                "armL.L": {"rot": [{"t": 0, "q": qx(-12), "ease": "quadInOut"},
                                   {"t": period // 2, "q": qx(-26),
                                    "ease": "quadInOut"},
                                   {"t": period, "q": qx(-12)}]},
                "armL.R": {"rot": [{"t": 0, "q": qx(-20)}]},
                "torso": {"rot": [{"t": 0, "q": qy(-deg * 0.18),
                                   "ease": "quadInOut"},
                                  {"t": period // 2, "q": qy(deg * 0.18),
                                   "ease": "quadInOut"},
                                  {"t": period, "q": qy(-deg * 0.18)}]},
            },
        }

    # jump / fall / land — additive so they read over whatever the legs do
    # JUMPING. The legs TUCK, which on this rig is a NEGATIVE rotation about X:
    # +X swings a hanging limb backward (verified against the selftest's own
    # swingOf convention, where negative reads "behind the body"). The old +35
    # / +25 keys therefore raked both legs out BEHIND the character, which is
    # the reported "both back legs move behind him" -- and because the clip
    # used to fire on any loss of ground contact, an ordinary step-down played
    # it. It now fires only on Player::jumped, a real launch.
    clips["jump"] = {
        "durationMs": 500, "loop": False, "mode": "additive",
        "blendInMs": 60, "blendOutMs": 200,
        "mask": ["torso", "armU.L", "armU.R", "legU.L", "legU.R"],
        "tracks": {
            "torso": {"rot": [{"t": 0, "q": qx(0), "ease": "cubicOut"},
                              {"t": 160, "q": qx(-10), "ease": "cubicInOut"},
                              {"t": 500, "q": qx(0)}]},
            "armU.L": {"rot": [{"t": 0, "q": qx(0), "ease": "cubicOut"},
                               {"t": 200, "q": qx(-42), "ease": "cubicInOut"},
                               {"t": 500, "q": qx(0)}]},
            "armU.R": {"rot": [{"t": 0, "q": qx(0), "ease": "cubicOut"},
                               {"t": 200, "q": qx(-30), "ease": "cubicInOut"},
                               {"t": 500, "q": qx(0)}]},
            "legU.L": {"rot": [{"t": 0, "q": qx(0), "ease": "cubicOut"},
                               {"t": 220, "q": qx(-32), "ease": "cubicInOut"},
                               {"t": 500, "q": qx(0)}]},
            "legU.R": {"rot": [{"t": 0, "q": qx(0), "ease": "cubicOut"},
                               {"t": 220, "q": qx(-22), "ease": "cubicInOut"},
                               {"t": 500, "q": qx(0)}]},
        },
    }
    # FALLING. Authored NEAR-NATURAL and opened out by WEIGHT, not by being a
    # single wide pose that switches on: avatar.cpp ramps this clip's weight
    # over avatar.fallFlailDelay/fallFlailRamp seconds of air, so a step off a
    # kerb plays a hint of it and only a genuine drop reaches the full shape.
    #
    # The old pose keyed both arms at -88 deg about X, which on this rig is 88
    # degrees FORWARD (a positive X rotation swings a hanging limb backward, so
    # negative swings it forward) -- both arms shot straight out in front, which
    # is the reported look and is not what a falling body does anyway. Arms go
    # OUT TO THE SIDES and trail slightly back; model +X is the character's
    # LEFT on these rigs, so .L abducts with +Z and .R with -Z.
    clips["fall"] = {
        "durationMs": 900, "loop": True, "mode": "additive",
        "blendInMs": 250, "blendOutMs": 250,
        "mask": ["torso", "armU.L", "armU.R", "armL.L", "armL.R"],
        "tracks": {
            "torso": {"rot": [{"t": 0, "q": qx(7), "ease": "quadInOut"},
                              {"t": 450, "q": qx(11), "ease": "quadInOut"},
                              {"t": 900, "q": qx(7)}]},
            "armU.L": {"rot": [{"t": 0, "q": qxz(14, 52), "ease": "quadInOut"},
                               {"t": 450, "q": qxz(4, 62), "ease": "quadInOut"},
                               {"t": 900, "q": qxz(14, 52)}]},
            "armU.R": {"rot": [{"t": 0, "q": qxz(4, -62), "ease": "quadInOut"},
                               {"t": 450, "q": qxz(14, -52), "ease": "quadInOut"},
                               {"t": 900, "q": qxz(4, -62)}]},
            "armL.L": {"rot": [{"t": 0, "q": qx(-18), "ease": "quadInOut"},
                               {"t": 450, "q": qx(-30), "ease": "quadInOut"},
                               {"t": 900, "q": qx(-18)}]},
            "armL.R": {"rot": [{"t": 0, "q": qx(-30), "ease": "quadInOut"},
                               {"t": 450, "q": qx(-18), "ease": "quadInOut"},
                               {"t": 900, "q": qx(-30)}]},
        },
    }
    clips["land"] = {
        "durationMs": 420, "loop": False, "mode": "additive",
        "blendInMs": 40, "blendOutMs": 180,
        "mask": ["torso", "hips", "armU.L", "armU.R"],
        "tracks": {
            "hips": {"pos": [{"t": 0, "v": [0, 0, 0], "ease": "cubicOut"},
                             {"t": 110, "v": [0, -0.55, 0], "ease": "cubicInOut"},
                             {"t": 420, "v": [0, 0, 0]}]},
            "torso": {"rot": [{"t": 0, "q": qx(0), "ease": "cubicOut"},
                              {"t": 110, "q": qx(14), "ease": "cubicInOut"},
                              {"t": 420, "q": qx(0)}]},
            "armU.L": {"rot": [{"t": 0, "q": qx(0), "ease": "cubicOut"},
                               {"t": 110, "q": qx(-40), "ease": "cubicInOut"},
                               {"t": 420, "q": qx(0)}]},
            "armU.R": {"rot": [{"t": 0, "q": qx(0), "ease": "cubicOut"},
                               {"t": 110, "q": qx(-30), "ease": "cubicInOut"},
                               {"t": 420, "q": qx(0)}]},
        },
    }

    # cast: the staff arm sweeps up and forward. Override + masked to the RIGHT
    # arm so it fully owns that limb while it plays; everything else keeps
    # walking. This is the clip the avatar plays on the attack/use input.
    clips["cast"] = {
        "durationMs": 620, "loop": False, "mode": "override",
        "blendInMs": 80, "blendOutMs": 220,
        "mask": ["armU.R", "armL.R", "hand.R", "staff", "torso"],
        "tracks": {
            "armU.R": {"rot": [{"t": 0, "q": qx(0), "ease": "cubicOut"},
                               {"t": 180, "q": qx(-105), "ease": "cubicInOut"},
                               {"t": 340, "q": qx(-72), "ease": "cubicInOut"},
                               {"t": 620, "q": qx(0)}]},
            "armL.R": {"rot": [{"t": 0, "q": qx(-20), "ease": "cubicOut"},
                               {"t": 180, "q": qx(-58), "ease": "cubicInOut"},
                               {"t": 340, "q": qx(-14), "ease": "cubicInOut"},
                               {"t": 620, "q": qx(-20)}]},
            "hand.R": {"rot": [{"t": 0, "q": ident}]},
            "staff": {"rot": [{"t": 0, "q": qx(0), "ease": "cubicOut"},
                              {"t": 180, "q": qx(18), "ease": "cubicInOut"},
                              {"t": 620, "q": qx(0)}]},
            "torso": {"rot": [{"t": 0, "q": qy(0), "ease": "cubicOut"},
                              {"t": 180, "q": qy(-14), "ease": "cubicInOut"},
                              {"t": 620, "q": qy(0)}]},
        },
    }

    # flinch on non-fatal damage (MobSystem plays "attack"; the avatar plays
    # this by the same name so both drivers share one convention)
    clips["attack"] = clips["cast"]

    # ---- dismemberment locomotion clips ----------------------------------
    # These are the poses the AnimStateRule table below selects between. Each
    # is an OVERRIDE loop that owns the parts it masks.

    # limp: one leg is gone. The gait keeps running (disableGait False) so the
    # surviving leg still steps via IK; this clip only adds the hunched torso
    # and the compensating arm.
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
                     "pos": [{"t": 0, "v": [0, -0.35, 0], "ease": "quadInOut"},
                             {"t": 500, "v": [0, -0.1, 0], "ease": "quadInOut"},
                             {"t": 1000, "v": [0, -0.35, 0]}]},
            "armU.L": {"rot": [{"t": 0, "q": qx(-26), "ease": "quadInOut"},
                               {"t": 500, "q": qx(-8), "ease": "quadInOut"},
                               {"t": 1000, "q": qx(-26)}]},
            "armU.R": {"rot": [{"t": 0, "q": qx(-14)}]},
        },
    }

    # hop: both feet gone but the thighs remain — the wizard bunny-hops.
    clips["hop"] = {
        "durationMs": 760, "loop": True, "mode": "override",
        "blendInMs": 220,
        "mask": ["torso", "hips", "legU.L", "legL.L", "legU.R", "legL.R",
                 "armU.L", "armU.R"],
        "tracks": {
            "hips": {"pos": [{"t": 0, "v": [0, 0, 0], "ease": "cubicOut"},
                             {"t": 260, "v": [0, 1.1, 0], "ease": "cubicInOut"},
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
            "armU.R": {"rot": [{"t": 0, "q": qx(-24), "ease": "cubicInOut"},
                               {"t": 260, "q": qx(-52), "ease": "cubicInOut"},
                               {"t": 760, "q": qx(-24)}]},
        },
    }

    # crawl: both legs unusable. Drags itself on both forearms. disableGait, so
    # the body settles to the ground and the IK/bob/sway are all suppressed.
    clips["crawl"] = {
        "durationMs": 1200, "loop": True, "mode": "override",
        "blendInMs": 260,
        "mask": ["torso", "hips", "head",
                 "armU.L", "armL.L", "armU.R", "armL.R",
                 "legU.L", "legL.L", "legU.R", "legL.R"],
        "tracks": {
            # face-down, pulling forward
            "hips": {"rot": [{"t": 0, "q": qx(74), "ease": "quadInOut"},
                             {"t": 600, "q": qx(80), "ease": "quadInOut"},
                             {"t": 1200, "q": qx(74)}]},
            "torso": {"rot": [{"t": 0, "q": qy(-9), "ease": "quadInOut"},
                              {"t": 600, "q": qy(9), "ease": "quadInOut"},
                              {"t": 1200, "q": qy(-9)}]},
            "head": {"rot": [{"t": 0, "q": qx(-48), "ease": "quadInOut"},
                             {"t": 600, "q": qx(-38), "ease": "quadInOut"},
                             {"t": 1200, "q": qx(-48)}]},
            # forearms alternate: one reaches while the other pulls
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
            # whatever leg stumps remain trail limply
            "legU.L": {"rot": [{"t": 0, "q": qx(-8)}]},
            "legU.R": {"rot": [{"t": 0, "q": qx(-8)}]},
            "legL.L": {"rot": [{"t": 0, "q": qx(-14)}]},
            "legL.R": {"rot": [{"t": 0, "q": qx(-14)}]},
        },
    }

    # squirm: no legs AND no arms. Nothing left to push with — the torso
    # undulates and barely moves. This is the floor of the state ladder.
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

    # one-armed: the cast arm is gone, so the survivor casts with the other
    # hand and the empty shoulder hangs.
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

    # headless: the classic. The body keeps walking for a few strides, hands
    # groping at the missing head. Additive over the normal gait.
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

    # ---- dismemberment locomotion states ---------------------------------
    # Predicates name PARTS BY NAME; the loader resolves them through
    # sk.FindPart and logs any typo rather than silently never matching. The
    # JSON keys are "missing" (all of these are severed) and "missingAny" (at
    # least one is) — see the partList lambda in mob.cpp's states loader.
    # FIRST MATCH WINS, so this list runs most-maimed to least. Predicate
    # fields are AND-ed; an empty predicate never matches.
    #
    # The ladder:
    #   squirm  no legs and an arm gone too   -> barely moves
    #   crawl   both legs unusable            -> drags on forearms
    #   hop     both feet gone, thighs remain -> bunny-hop
    #   limp    one leg unusable              -> gait continues, hunched
    #   onearm  a whole arm gone              -> compensating posture
    #   headless head gone                    -> keeps walking, gropes
    #
    # WHY THESE NAME LEG PARTS INSTEAD OF USING minChainsLost. AnimSelectState
    # counts EVERY IK chain, and this rig has arm chains as well as leg chains
    # (unlike the critter, which is all legs). `minChainsLost: 2` would
    # therefore fire "crawl" when both ARMS came off and the legs were fine.
    # Naming the leg parts directly keeps each rule about the thing it is
    # actually describing; the two are only equivalent on an all-legs rig.
    #
    # A leg is "unusable" if any part of its chain is gone, so each side lists
    # its thigh, shin and foot — matching the chain-lost test the gait uses.
    states = [
        {"name": "squirm",
         "missing": ["legU.L", "legU.R"],
         "missingAny": ["armU.L", "armU.R"],
         "clip": "squirm", "speedScale": 0.12,
         "disableGait": True, "bodyYOffset": -0.55},
        # both legs unusable: any part of the left chain AND any of the right.
        # Expressed as "both thighs gone" plus the shin/foot cases handled by
        # the hop rule below, so the common amputations all land somewhere.
        {"name": "crawl",
         "missing": ["legU.L", "legU.R"],
         "clip": "crawl", "speedScale": 0.3,
         "disableGait": True, "bodyYOffset": -0.5},
        {"name": "crawl.shins",
         "missing": ["legL.L", "legL.R"],
         "clip": "crawl", "speedScale": 0.3,
         "disableGait": True, "bodyYOffset": -0.5},
        {"name": "hop",
         "missing": ["foot.L", "foot.R"],
         "clip": "hop", "speedScale": 0.45,
         "disableGait": True, "bodyYOffset": -0.15},
        # one leg gone, in any of its three parts
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
        # MICROVOXELS: every limb coordinate above is in 1/4-world-voxel units.
        "scale": SCALE,
        "bleed": {"material": "blood", "perDamage": 2.5},
        "speed": 5.0,
        "gait": {
            # Biped: two SINGLETON groups, so only one foot may swing at a
            # time. That single constraint is the whole gait state machine.
            "groups": [["legU.L"], ["legU.R"]],
            "cadence": 2.1, "strideBias": 0.42, "leadTime": 0.2,
            "stepThreshold": 0.62, "stepDuration": 0.24, "stepHeight": 0.22,
            "rideHeight": 0.94,
            "bobAmp": 0.07, "bobFreqMul": 2.0, "swayAmp": 0.045,
            "rollAmp": 0.06, "spineCounter": 0.75, "phaseLag": 0.05,
        },
        "limbs": limbs,
        "chains": chains,
        "states": states,
        "clips": clips,
        "flipbooks": {},
    }

    with open(os.path.join(out_dir, "wizard.json"), "w") as f:
        json.dump(sidecar, f, indent=2)
        f.write("\n")

    total = sum(len(SHAPES[n](LIMBS[n][0]) if n in SHAPES else
                    solid(LIMBS[n][0], LIMBS[n][2])) for n in ORDER)
    print(f"wrote wizard.vox ({len(ORDER)} parts, ~{total} micro voxels) "
          f"and wizard.json ({len(clips)} clips, {len(states)} states)")


if __name__ == "__main__":
    main()
