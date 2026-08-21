#!/usr/bin/env python3
"""Generate assets/mobs/mina.{vox,json} — the PLAYER AVATAR: a small hooded,
robed figure in the style of Noita's Mina.

WHY THIS IS A MOB DEF. Same story as gen_wizard.py: the avatar is authored in
exactly the same format as critter/dummy and loaded by the same LoadMobDefs
path, so it inherits the whole animation runtime for free — two-bone IK, the
gait state machine, springs, clips, and `states`, the dismemberment locomotion
rules (src/game/anim.h AnimStateRule). It does NOT inherit the mob AI:
src/game/avatar.cpp drives this rig from the Player's own position and input.
One schema, two drivers. gen_wizard.py is kept as a second character.

HEIGHT IS THE WHOLE POINT OF THIS FILE. The old wizard was authored 28 world
voxels tall, which at kVoxelMeters = 0.10 is 2.8 m — it towered over its own
1.7 m collision box and the first-person camera sat inside its chest. This rig
is built to the numbers the ENGINE already declares, so the art, the AABB and
the eye camera agree by construction rather than by luck:

    Player::kHalfY    0.85 m  -> 17.0 world voxels tall   (player.h)
    Player::kEyeOffset 0.65 m -> eyes 1.50 m up = voxel 15.0
    kVoxelMeters      0.10 m                              (sim/world.h)

So: 17 world voxels head to toe, eye slit at world voxel 15. If kVoxelMeters
or kHalfY ever change, the assert in main() below fails rather than silently
producing another floating giant.

SCALE 4. Every coordinate in LIMBS is in MICRO units, 4 per world voxel, so
17 world voxels = 68 micro. A micro voxel is 2.5 cm — fine enough for a hood
brim, a sash and a hand on a figure this small. The engine divides every
POSITION by `scale` at load and builds colliders at pitch 1/4.

SILHOUETTE (from the reference). Mina is defined by what she does NOT have:
no hat brim, no beard, no staff, no visible legs, no face. Reading top down:

    a pointed hood that merges into the shoulders as ONE unbroken cone
    a black void where the face is, set back under the hood brow
    a plain robe body that flares steadily to the floor
    a gold sash at the waist, the only bright element
    one small hand emerging from a sleeve
    feet hidden under the hem (they exist for the gait, barely seen)

The parts still exist as a full severable rig even where the art hides them —
the legs are under the skirt, but they drive the IK gait and they can be cut
off, at which point the states table below takes over.

FACING CONVENTION (load-bearing — see gen_critter_mob.py for the derivation).
The engine's heading is fwd = (sin h, 0, cos h), so heading 0 travels along
engine +Z and a model must have its FRONT on engine +Z. The loader maps scene
-> engine as (x, z, -y), which negates scene y, so "front on engine +Z" means
"front on scene -Y". Everything below is authored with the face toward scene
-Y, in the natural "front = +y" reading, and mirrored once through flip_y.

HANDEDNESS CONVENTION (separate from facing, and NOT derivable from it).
Model +X is the character's LEFT. Camera::Right() = Forward() x (0,1,0)
(game/camera.cpp:23) composed with the rig heading h = pi/2 - cam.yaw gives
dot(model +X in world, camera Right) = -1.000 at every heading. The scene ->
engine map (x, z, -y) negates y, which flips handedness, so the intuitive
"it faces -Y, therefore +X is its right" is wrong — that reasoning had every
.L/.R limb on the wrong side of both characters until 2026-08-21. So .L limbs
sit at POSITIVE scene x and .R limbs at negative.

This rig carries NO WEAPON. A held item is a standalone asset with its own
.vox and its own origin (assets/items/, scripts/gen_sword_item.py); this file
publishes a SOCKET on hand.R and the runtime composes socket x grip to place
it. The sword lived here as a `"tag": "prop"` limb until the handedness fix
above moved hand.R to model -X and took the blade to engine x -30 — geometry
at negative prefab-local coordinates, which that space cannot hold, drawing
the body offset from where its own arm solved.

Run:  python scripts/gen_mina.py
"""
import json
import math
import os
import re
import struct

# ---- palette ---------------------------------------------------------------
# PALETTE CONVENTION (PLAN_voxel_art_and_mobs.md A1, gen_palette.py): .vox
# palette index i+1 == materials.json[i]. These are the +1 indices, so ROBE 48
# is materials.json[47] == robe_cloth. Off-by-one here paints the whole figure
# in the neighbouring material and is invisible until you look at it.
ROBE = 48       # robe_cloth   deep indigo
TRIM = 49       # robe_trim    gold, faintly emissive — the sash
SHADE = 50      # robe_shadow  near-black indigo: hood interior and the face
SKIN = 51       # skin         the one hand
LEATHER = 53    # leather      shoes under the hem
# STEEL/GRIP are the sword's palette entries and live with the sword now
# (scripts/gen_sword_item.py); the figure itself is cloth, skin and leather.

SCALE = 4       # micro voxels per world voxel
WORLD_H = 17    # world voxels, head to toe — matches Player::kHalfY * 2
MICRO_H = WORLD_H * SCALE  # 68

# The SPEED contract, the companion to the height contract above. `speed` in
# the sidecar is the reference top speed in world voxels/sec that the runtime
# divides the measured speed by, so it has to be the PLAYER's speed, not a
# mob's. Both of these mirror values the engine already declares:
#   tuning.json player.sprintSpeed  6.0 m/s
#   sim/world.h kVoxelMeters        0.10 m
# giving 60 world voxels/sec. Asserted in main() against the same files.
SPRINT_SPEED_MPS = 6.0
VOXEL_METERS = 0.10


# ---- .vox writing (same helpers as gen_wizard.py / gen_critter_mob.py) ------
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
    """Mirror front-to-back once, at the end. Authoring every builder in the
    natural "face at high y" reading and flipping once is far easier to check
    by eye than writing every index backwards, and it keeps shape placement and
    limb placement using ONE idea of "front"."""
    sy = size[1]
    return [(x, sy - 1 - y, z, c) for (x, y, z, c) in voxels]


def solid(size, mat):
    sx, sy, sz = size
    return [(x, y, z, mat)
            for z in range(sz) for y in range(sy) for x in range(sx)]


def ellipse_mask(x, y, cx, cy, rx, ry):
    """True when voxel (x,y)'s CENTRE is inside the axis-aligned ellipse. Voxel
    i covers [i, i+1) so its centre is i+0.5 — that half is added here, once,
    so no builder has to invent its own convention."""
    if rx <= 0 or ry <= 0:
        return False
    dx = (x + 0.5 - cx) / rx
    dy = (y + 0.5 - cy) / ry
    return dx * dx + dy * dy <= 1.0


def tapered_tube(size, mat, r0, r1, trim_rows=(), shade_rows=()):
    """A vertical (z-axis) tube whose XY radius lerps from r0 at the bottom to
    r1 at the top. Arms, legs and the robe skirt are all tapered tubes with
    different end radii. trim_rows / shade_rows are z indices repainted, which
    is how the sash and hem get expressed without a second model."""
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
    """The hood: a cone that comes to a point and TIPS FORWARD, with a black
    void where the face should be.

    This one model carries the entire read of the character, and it is built
    from exactly two ideas:

    1. The cone leans FORWARD as it rises (`lean` grows with height, toward
       high y = front before the flip). The reference's hood peak hangs out
       over the face rather than standing straight up; a vertical cone reads as
       a party hat instead.
    2. The face is a VOID, not features. A shallow dish of SHADE is carved into
       the front of the lower cone. No eyes, no nose — the reference has none,
       and at this size any feature you add reads as noise. The darkness IS the
       face.

    The model's own origin (z=0) is the base of the hood at the shoulders, so
    the neck anchor is at z=0, unlike the wizard's head whose origin sat under
    a hanging beard."""
    sx, sy, sz = size
    out = []
    cx, cy = sx * 0.5, sy * 0.5
    # The brow: below this the hood is a head-sized cylinder, above it the cone
    # tapers to the peak. Keeping a straight-sided section at the bottom is
    # what gives the hood a face to shadow rather than closing to a point
    # immediately. Set high (2/3) so the face void has room to sit at the eye
    # line rather than down at the neck.
    brow_z = int(sz * 0.66)
    # Derived from the declared box rather than hardcoded, so resizing the head
    # in LIMBS actually resizes the hood instead of leaving a small cone
    # rattling around inside a bigger model.
    hood_r = sx * 0.5
    for z in range(sz):
        if z <= brow_z:
            r = hood_r
            lean = 0.0
        else:
            t = (z - brow_z) / max(sz - 1 - brow_z, 1)
            # ease the taper so the peak is a soft point, not a sharp spike
            r = hood_r * (1.0 - t * t * 0.92) + 0.4
            lean = 2.6 * t * t   # peak drifts FORWARD, over the face
        for y in range(sy):
            for x in range(sx):
                if not ellipse_mask(x, y, cx, cy + lean, r, r):
                    continue
                mat = ROBE
                # a single rim row at the very back falls into shadow, which
                # separates the silhouette from the sky in third person. Kept
                # to one voxel deep: a wide dark band here reads as a second
                # face void from the side and muddies the whole head.
                if y <= cy - 4.0:
                    mat = SHADE
                out.append((x, y, z, mat))
    # ---- the face void ----
    # Carved as a repaint pass over voxels that exist, so it can never float
    # beside the head. It is an ellipse on the front face, set BELOW the brow
    # and inset one voxel so the hood's own edge overhangs it — that overhang
    # is what makes it read as a recess instead of a black sticker.
    # The void is anchored to the EYE LINE, not to the middle of the model:
    # eye_z is the micro row that lands on world voxel 15 once the head's own
    # min corner is added back, so the darkness sits where the camera is.
    eye_z = 60 - LIMBS["head"][1][2]
    face_lo, face_hi = eye_z - 4, eye_z + 2
    fz = (face_lo + face_hi) * 0.5
    # Two separate things happen here, and conflating them is what produced a
    # hole straight through the head on the first attempt:
    #   - voxels INSIDE the face ellipse and in front of the head's mid-plane
    #     become SHADE (the void itself);
    #   - the frontmost row of that ellipse is DELETED, so the brow above and
    #     the cheeks either side physically overhang the darkness. That real
    #     geometric recess is what sells it under moving light; a flat black
    #     patch painted on a convex surface just looks like a decal.
    def in_face(x, z):
        return (face_lo <= z <= face_hi
                and ellipse_mask(x, z, cx, fz, 3.2, (face_hi - face_lo) * 0.62))

    front_of = {}   # (x, z) -> max y present, i.e. the frontmost surface voxel
    for (x, y, z, _m) in out:
        if in_face(x, z):
            front_of[(x, z)] = max(front_of.get((x, z), -1), y)
    carved = []
    for (x, y, z, m) in out:
        if in_face(x, z) and y >= cy - 0.5:
            if y >= front_of.get((x, z), -1):
                continue            # delete the surface row: the recess
            m = SHADE               # everything behind it is the void
        carved.append((x, y, z, m))
    return flip_y(size, carved)


def torso_vox(size):
    """Shoulders and chest under the robe. Deliberately featureless: the
    reference has no collar, no lapel, no belt buckle — the shoulders are just
    where the hood's cone stops widening. Sloped at the top so the hood sits
    into it as one continuous line."""
    sx, sy, sz = size
    cx, cy = sx * 0.5, sy * 0.5
    out = []
    for z in range(sz):
        t = z / max(sz - 1, 1)
        # widest at the shoulders (top), narrowing very slightly to the waist,
        # then tucked in over the last rows so the hood meets it cleanly
        rx = 3.4 + 1.0 * t - (1.4 if z >= sz - 2 else 0.0)
        ry = 2.6 + 0.5 * t - (1.0 if z >= sz - 2 else 0.0)
        for y in range(sy):
            for x in range(sx):
                if not ellipse_mask(x, y, cx, cy, rx, ry):
                    continue
                out.append((x, y, z, SHADE if y <= cy - 1.6 else ROBE))
    return flip_y(size, out)


def hips_vox(size):
    """The robe skirt — the bottom two thirds of the silhouette.

    Authored as a cone that widens DOWNWARD (z=0 is the hem, at the floor) so
    it flares past the legs and hides them. The gold sash is a band at the top,
    and it is the only saturated colour on the figure, which is exactly why the
    reference reads at a glance."""
    sx, sy, sz = size
    cx, cy = sx * 0.5, sy * 0.5
    out = []
    # The flare is the silhouette. waist_r must be clearly NARROWER than the
    # torso above it and hem_r clearly wider, or the figure reads as a barrel:
    # it is the difference between the two, not the absolute width, that says
    # "robe". The curve is eased (t*t) so the skirt hangs and then kicks out
    # near the floor rather than being a straight-sided cone.
    # waist_r must be a shade WIDER than the torso's half-width above it (4.5),
    # or the chest overhangs the sash and buries the one bright element on the
    # figure. Narrower than the shoulders, wider than the ribs.
    hem_r, waist_r = sx * 0.5, 4.7
    sash_lo = sz - 3          # gold band at the very TOP rows of the skirt
    for z in range(sz):
        t = z / max(sz - 1, 1)
        r = waist_r + (hem_r - waist_r) * (1.0 - t) * (1.0 - t)
        for y in range(sy):
            for x in range(sx):
                # slightly oval in y so the figure has a front-to-back depth
                # rather than being a perfect cylinder of cloth
                if not ellipse_mask(x, y, cx, cy, r, r * 0.82):
                    continue
                mat = ROBE
                if z >= sash_lo:
                    mat = TRIM                    # the sash
                elif z == 0:
                    mat = SHADE                   # hem in contact shadow
                elif y <= cy - 2.2:
                    mat = SHADE                   # shaded back
                # ONE fold line down each side of the front, and only in the
                # lower half where the cloth actually gathers. An every-nth-
                # column rule speckles the whole skirt into what reads as rot
                # rather than drapery — at this size two lines is the budget.
                elif (z < sz * 0.45 and y > cy + 1.0
                      and abs(abs(x - cx) - 3.0) < 0.55):
                    mat = SHADE
                out.append((x, y, z, mat))
    return flip_y(size, out)


def upper_arm_vox(size):
    """A plain sleeve. No cuff, no flare — the reference's arm is a simple
    stub, and the ONE piece of detail on the whole figure is the hand."""
    return flip_y(size, tapered_tube(size, ROBE, 2.0, 2.2, shade_rows=(0,)))


def fore_arm_vox(size):
    """Forearm sleeve, tapering slightly toward the wrist so the hand reads as
    emerging from cloth."""
    return flip_y(size, tapered_tube(size, ROBE, 1.7, 2.0))


def hand_vox(size):
    """A small mitten hand. At 4x on a figure this small there is not room for
    separate fingers the way the wizard has them — a rounded blob with a thumb
    nub is what actually reads, and inventing five fingers here just produces
    speckle."""
    sx, sy, sz = size
    out = []
    cx, cy = sx * 0.5, sy * 0.5
    for z in range(sz):
        t = z / max(sz - 1, 1)
        r = 1.5 + 0.7 * t          # narrower at the fingertips (low z)
        for y in range(sy):
            for x in range(sx):
                if ellipse_mask(x, y, cx, cy, r, r * 0.8):
                    out.append((x, y, z, SKIN))
    # thumb nub, proud of the front-inside edge
    out.append((int(cx) + 1, sy - 1, sz - 2, SKIN))
    return flip_y(size, out)


def thigh_vox(size):
    """Upper leg. Entirely hidden by the skirt when standing — it exists to
    drive the IK gait and to be severable. It becomes visible the moment the
    skirt is cut away or a leg comes off."""
    return flip_y(size, tapered_tube(size, SHADE, 1.9, 2.1))


def shin_vox(size):
    """Lower leg, dark so that the gap under the hem reads as shadow rather
    than as bare limbs."""
    return flip_y(size, tapered_tube(size, SHADE, 1.6, 1.8))


def foot_vox(size):
    """A simple shoe: a rounded sole reaching forward. No buckle, no curl —
    it lives under the hem and is seen only in silhouette when running."""
    sx, sy, sz = size
    out = []
    cx = sx * 0.5
    for z in range(sz):
        for y in range(sy):
            for x in range(sx):
                # the sole spans the length; the ankle is a column at the BACK
                in_sole = z < 2
                in_ankle = y < 3 and z >= 2
                if not (in_sole or in_ankle):
                    continue
                if not ellipse_mask(x, y if in_ankle else 2, cx, 2.0, 1.9, 2.4):
                    continue
                out.append((x, y, z, LEATHER))
    return flip_y(size, out)


# ---- limb table -------------------------------------------------------------
# name -> (size, min-corner in SCENE space, default material)
#
# MICRO units (4 per world voxel, 2.5 cm each). Scene is Z-up; engine Y = scene
# Z, so the z column below IS the height.
#
# HEIGHT BUDGET (micro; /4 for world voxels). The figure stands with the soles
# at z=0 and the hood peak at z=68 — 17 world voxels, matching the 1.7 m player
# AABB exactly. The eye slit in the hood lands at world voxel 15, which is
# where Player::kEyeOffset puts the first-person camera.
#
#   foot    z  0..4      shoes, under the hem
#   shin    z  4..15     hidden by the skirt
#   thigh   z 14..26     hidden by the skirt
#   hips    z  0..34     the SKIRT: hem at the FLOOR, sash at the top
#   torso   z 32..46     shoulders and chest
#   head    z 46..68     hood; face void spans ~z 48..57, eye line z 60
#
# The eye line lands at micro z 60 = world voxel 15, which is exactly where
# Player::kEyeOffset (0.65 m above the AABB centre) puts the first-person
# camera. That is the one number in this table that is not free.
#
# The skirt deliberately starts at z=0 and overlaps the legs for its whole
# length — that is what "the robe hides the legs" means geometrically. Its
# pivot is still at the waist (see the hips anchor below), so it swings from
# the hips like cloth rather than from the floor.
#
# Front is scene -Y throughout (see the module docstring).
LIMBS = {
    #                size          min corner (x, y, z)
    # The skirt is the widest thing on the figure — wider than the shoulders
    # and wider than the hanging arms (which reach x -9..9), so the silhouette
    # is a triangle standing on its base rather than pinching at the floor.
    "hips":     ((20, 16, 34), (-10, -8,  0), ROBE),
    "torso":    ((11,  9, 14), (-5, -4, 32), ROBE),
    "head":     ((13, 12, 22), (-6, -6, 46), ROBE),

    # Arms hang just clear of the torso (which spans x -4..4): a 1-micro gap
    # either side. Overlapping them into the chest is what turned the first
    # draft into an unbroken slab from shoulder to sash with no waist.
    # HANDEDNESS: model +X is the character's LEFT, so .L limbs take the
    # POSITIVE x offsets and .R the negative. This is NOT the "obvious" reading
    # and it has been got wrong once already: the .vox -> engine map is
    # (x, z, -y), which negates y and therefore FLIPS handedness, so the
    # natural "front = +y, right = +x" intuition does not survive the load.
    # It was verified from the camera basis rather than argued from the
    # facing: dot(model +X in world, camera Right) = -1.000 across yaws.
    # Derive the side from the camera basis, never from the facing alone.
    "armU.L":   (( 4,  4,  8), (  4, -2, 37), ROBE),
    "armL.L":   (( 4,  4,  7), (  4, -2, 31), ROBE),
    "hand.L":   (( 4,  4,  4), (  4, -2, 28), SKIN),
    "armU.R":   (( 4,  4,  8), (-8, -2, 37), ROBE),
    "armL.R":   (( 4,  4,  7), (-8, -2, 31), ROBE),
    "hand.R":   (( 4,  4,  4), (-8, -2, 28), SKIN),

    "legU.L":   (( 4,  4, 12), (  0, -2, 14), SHADE),
    "legL.L":   (( 4,  4, 11), (  0, -2,  4), SHADE),
    "foot.L":   (( 4,  7,  4), (  0, -4,  0), LEATHER),
    "legU.R":   (( 4,  4, 12), (-4, -2, 14), SHADE),
    "legL.R":   (( 4,  4, 11), (-4, -2,  4), SHADE),
    "foot.R":   (( 4,  7,  4), (-4, -4,  0), LEATHER),

    # NO SWORD HERE. A held weapon is a standalone item with its own .vox and
    # its own origin (assets/items/, scripts/gen_sword_item.py); the rig
    # publishes a SOCKET on hand.R instead. See the socket block in main().
}

SHAPES = {
    "hips": hips_vox, "torso": torso_vox, "head": head_vox,
    "armU.L": upper_arm_vox, "armU.R": upper_arm_vox,
    "armL.L": fore_arm_vox, "armL.R": fore_arm_vox,
    "hand.L": hand_vox, "hand.R": hand_vox,
    "legU.L": thigh_vox, "legU.R": thigh_vox,
    "legL.L": shin_vox, "legL.R": shin_vox,
    "foot.L": foot_vox, "foot.R": foot_vox,
}

# Parent-before-child; the loader also topologically sorts, but authoring in
# order keeps the .vox scene graph readable in MagicaVoxel.
ORDER = ["hips", "torso", "head",
         "armU.L", "armL.L", "hand.L",
         "armU.R", "armL.R", "hand.R",
         "legU.L", "legL.L", "foot.L",
         "legU.R", "legL.R", "foot.R"]

# Held props: in the rig, but not part of the figure's own silhouette. The
# height contract and the prefab-origin measurement in main() both exclude
# these, mirroring the loader (mob.cpp skips tag == "prop" when it measures
# worldSize).
#
# EMPTY NOW, AND THAT IS THE POINT. The sword was the only entry; it is a
# standalone item today (assets/items/, scripts/gen_sword_item.py) held
# through the hand.R socket, so nothing the figure carries can reach into
# these measurements at all. The mechanism stays because the exclusion rule
# is still correct for any prop that IS genuinely part of a rig — but a
# WEAPON should be an item, not a prop.
PROPS = set()


def to_engine(scene_xyz, min_x, max_y):
    """Scene (Z-up) -> prefab-local engine coords (Y-up, min corner 0), matching
    voxload.cpp: engine = (x, z, -y), rebased so the prefab min corner is 0."""
    x, y, z = scene_xyz
    return [round(x - min_x, 1), round(float(z), 1), round(max_y - y, 1)]


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "assets", "mobs")
    os.makedirs(out_dir, exist_ok=True)

    # The height contract, asserted rather than commented. If someone changes
    # kVoxelMeters or kHalfY, this is the thing that should complain — a
    # mismatch here is exactly how the old wizard ended up 2.8 m tall.
    #
    # PROPS ARE EXCLUDED. A held sword reaches well above the hood and below
    # the fist, and letting it into this measurement would mean the figure's
    # "height" changed with whatever it happens to be carrying — which would
    # both break the assert and, worse, quietly redefine the contract it
    # exists to protect. The BODY is what has to match the player AABB.
    body_parts = {n: v for n, v in LIMBS.items() if n not in PROPS}
    top = max(mn[2] + sz[2] for (sz, mn, _m) in body_parts.values())
    bottom = min(mn[2] for (_sz, mn, _m) in body_parts.values())
    assert bottom == 0, f"figure does not stand on z=0 (lowest part at {bottom})"
    assert top == MICRO_H, (
        f"figure is {top} micro ({top / SCALE} world voxels) tall, expected "
        f"{MICRO_H} ({WORLD_H}) to match Player::kHalfY * 2")

    # The speed contract, checked against the files that actually declare it —
    # a stale `speed` here does not look broken, it just pins speedFactor at its
    # clamp forever, so nothing complains without this.
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
        # DebrisVoxel is int8 and these are MICRO units, so a part may be at
        # most 120 micro = 30 world voxels on an axis. Nothing here is close.
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
                continue          # first write wins
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
    with open(os.path.join(out_dir, "mina.vox"), "wb") as f:
        f.write(data)

    # prefab extents in scene space, for the anchor conversion.
    #
    # PROPS ARE EXCLUDED, and that is not a nicety — it MIRRORS THE LOADER.
    # mob.cpp (~L603) skips `tag == "prop"` when it measures worldSize, so the
    # engine's box is the BODY's box. Anchors are rebased against this origin,
    # so if the generator measured a box the loader does not, every anchor
    # would be shifted by the difference and the gait pivot
    # (worldSize * 0.5, avatar.cpp UpdateGait) would sit off the body.
    #
    # That is exactly what happened when the sword moved to the figure's real
    # right (model -X): the blade became the new minimum x, every anchor slid
    # +30 micro, the stance was rotated about a pivot ~7 world voxels off the
    # legs, and the IK froze the legs bolt upright mid-walk (`upright=0`).
    # Keep this set in step with the `"tag": "prop"` limbs below.
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

    # ---- joint anchors, DERIVED from the limb table ------------------------
    # A joint sits at the seam between a part and its parent, so restating its
    # height as a literal is a second source of truth that rots the moment a
    # limb is resized. `joint_top` puts the pivot at the TOP face of a part
    # (shoulder, elbow, wrist, hip, knee all work this way once the parts are
    # stacked), centred in x/y unless told otherwise.
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

    def joint_at(part, z, dx=0.0, dy=0.0):
        """Pivot at an ARBITRARY height on a part, still centred in x/y. The
        skirt needs this: its model spans floor-to-waist, but it must hinge at
        the WAIST, not at either end of its own box."""
        size, mn, _m = LIMBS[part]
        return anchor((mn[0] + size[0] * 0.5 + dx,
                       mn[1] + size[1] * 0.5 + dy, z))

    # ---- limbs ----
    # hp / severImpactSpeed are authored so extremities come off easily and the
    # spine does not: a graze takes a hand, only a serious hit takes a thigh.
    hips_sz, hips_mn, _ = LIMBS["hips"]
    waist_z = hips_mn[2] + hips_sz[2]          # top of the skirt = the sash
    limbs = [
        {"name": "hips", "hp": 60, "severable": False, "tag": "spine"},
        # waist: the top of the skirt, i.e. the sash line
        {"name": "torso", "parent": "hips", "joint": "ball", "hp": 60,
         "severable": False, "vital": True, "tag": "spine",
         "anchor": joint_at("hips", waist_z - 2)},
        # neck: the top of the torso. This head model's origin IS its base (no
        # beard hanging below it, unlike the wizard), so this is the seam.
        {"name": "head", "parent": "torso", "joint": "ball", "hp": 22,
         "severable": True, "vital": True, "tag": "head",
         "anchor": joint_top("torso", inset=1), "severImpactSpeed": 20.0,
         # The hood's mass lags when the head turns. `gain` is read against
         # velocity NORMALIZED by `speed` (kSpringVelScale in game/anim.h, used
         # by both drivers), so it means "radians of lag at full sprint" — a
         # small number, or the head sits pegged at
         # maxAngle for every step and reads as rearing back off the neck.
         # maxAngle stays a safety rail for impacts, not the working range.
         "spring": {"halflife": 0.12, "gain": 0.18, "maxAngle": 0.3}},
    ]
    # arms: shoulder -> elbow -> wrist, per side. Mirrored hinge limits so both
    # elbows bend the same way relative to the body.
    for side in ("L", "R"):
        limbs.append({
            "name": f"armU.{side}", "parent": "torso", "joint": "ball",
            "hp": 16, "severable": True, "tag": "arm",
            "anchor": joint_top(f"armU.{side}"),      # shoulder
            "severImpactSpeed": 15.0})
        limbs.append({
            "name": f"armL.{side}", "parent": f"armU.{side}", "joint": "hinge",
            "hp": 13, "severable": True, "tag": "arm", "axis": [1, 0, 0],
            "minAngle": -2.4, "maxAngle": 0.05,
            "anchor": joint_top(f"armL.{side}"),      # elbow
            "severImpactSpeed": 13.0})
        limbs.append({
            "name": f"hand.{side}", "parent": f"armL.{side}", "joint": "ball",
            "hp": 9, "severable": True, "tag": "hand",
            "anchor": joint_top(f"hand.{side}"),      # wrist
            "severImpactSpeed": 9.0})
    # legs: hip -> knee -> ankle, per side
    for side in ("L", "R"):
        limbs.append({
            "name": f"legU.{side}", "parent": "hips", "joint": "ball",
            "hp": 22, "severable": True, "tag": "leg",
            "anchor": joint_top(f"legU.{side}"),      # hip
            "severImpactSpeed": 18.0})
        limbs.append({
            "name": f"legL.{side}", "parent": f"legU.{side}", "joint": "hinge",
            "hp": 18, "severable": True, "tag": "leg", "axis": [1, 0, 0],
            "minAngle": -2.4, "maxAngle": 0.05,
            "anchor": joint_top(f"legL.{side}"),      # knee
            "severImpactSpeed": 16.0})
        limbs.append({
            "name": f"foot.{side}", "parent": f"legL.{side}", "joint": "hinge",
            "hp": 12, "severable": True, "tag": "foot", "axis": [1, 0, 0],
            "minAngle": -0.6, "maxAngle": 0.6,
            # ankle: above the sole, at the BACK of the shoe (the toe reaches
            # forward from here) so the foot pivots the way a foot does
            "anchor": joint_bottom(f"foot.{side}", dy=-1.5, rise=3.0),
            "severImpactSpeed": 12.0})

    # ---- sockets: where an ITEM attaches ------------------------------------
    #
    # THE SWORD IS NOT IN THIS RIG. It used to be, as a `"tag": "prop"` limb
    # parented to hand.R, and that is precisely what broke: prefab-local space
    # is rebased on the BODY's min corner (props are excluded from that
    # measurement, because a creature's size must not change with its
    # luggage), so once the handedness fix put the right hand at model -X the
    # blade reached to engine x -30 — thirty micro of geometry at NEGATIVE
    # prefab-local coordinates, which that space cannot represent. The body
    # drew offset from where the arm actually solved.
    #
    # An item now owns its own .vox and its own origin
    # (assets/items/sword.*, scripts/gen_sword_item.py) and BORROWS this
    # socket at runtime. The rig states only where the fist closes; the item
    # states how it sits in that fist (its `grip` block), and the runtime
    # composes socket x grip. Splitting it on that seam is what lets one
    # sword be held by any rig with a hand, and what keeps a weapon from ever
    # again inflating the body it is held by.
    #
    # A socket is a POINT AND A FRAME, in the same prefab-local world voxels
    # as every anchor, derived from the hand box so resizing the hand keeps
    # the grip in it.
    hand_sz, hand_mn, _ = LIMBS["hand.R"]
    sockets = [{
        # The socket is named for the CONTEXT an item asks to be held in, not
        # for the limb it rides — those are two different questions and
        # conflating them is a bug. An item declares grips per context
        # ("held_right"), so that is the key it looks up here; `part` is the
        # separate matter of which limb carries this context on THIS rig.
        # A left-handed creature would put "held_right" on its own hand.L and
        # every item would still hang correctly with no per-item edits.
        "name": "held_right",
        "part": "hand.R",
        # Centre of the fist. The item's own translation is what slides it so
        # the right point along the blade lands here.
        "offset": anchor((hand_mn[0] + hand_sz[0] * 0.5,
                          hand_mn[1] + hand_sz[1] * 0.5,
                          hand_mn[2] + hand_sz[2] * 0.5)),
        # No rotation: the socket frame IS the hand's frame. Every weapon's
        # own orientation is its business, stated in its grip block — putting
        # a rotation here too would mean two places encode "which way does a
        # held thing point" and they would drift apart.
        "rotation": [0, 0, 0],
    }]

    # ---- IK chains ----
    # Legs are gait-driven. Arms get chains too so the avatar code can plant a
    # hand on a wall or aim without a bespoke solver; a chain's weight is
    # driven to 0 automatically when it loses a part.
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
    def swing(deg, period=900, phase=0, ease="quadInOut"):
        """Simple two-extreme loop about X; `phase` in half-periods."""
        a, b = (deg, -deg) if phase == 0 else (-deg, deg)
        half = period // 2
        return {"rot": [{"t": 0, "q": qx(a), "ease": ease},
                        {"t": half, "q": qx(b), "ease": ease},
                        {"t": period, "q": qx(a)}]}

    clips = {}

    # idle: a slow breath in the spine and a drifting hood. Small amplitudes —
    # this figure is meant to be still and a little eerie.
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
                             {"t": 1100, "q": qy(4), "ease": "quadInOut"},
                             {"t": 2300, "q": qy(-3), "ease": "quadInOut"},
                             {"t": 3200, "q": qy(0)}]},
            "armU.L": {"rot": [{"t": 0, "q": qz(0), "ease": "quadInOut"},
                               {"t": 1600, "q": qz(2.0), "ease": "quadInOut"},
                               {"t": 3200, "q": qz(0)}]},
            "armU.R": {"rot": [{"t": 0, "q": qz(0), "ease": "quadInOut"},
                               {"t": 1600, "q": qz(-2.0), "ease": "quadInOut"},
                               {"t": 3200, "q": qz(0)}]},
        },
    }

    # walk/run arm swing: additive over the gait, masked to the arms and spine
    # so the IK-driven legs are untouched. Both arms swing evenly here — unlike
    # the wizard, Mina is not carrying anything.
    #
    # PERIODS MATCH THE GAIT'S ACTUAL STEP CYCLE. These clips play at a fixed
    # rate while the legs step at a speed-dependent one, so the only way they
    # read as one motion is to author each period at the step cycle for the
    # speed that clip is selected at (avatar.cpp switches to `run` past
    # 0.80 * speed). One arm cycle spans TWO steps — left plants, then right.
    #
    # The step time is swing + stance, and BOTH are now set by the runtime
    # rather than by the authored stepDuration: avatar.cpp caps the swing at the
    # stride budget (kSwingTravelFrac), and the stance ends when the foot has
    # drifted stepThreshold * legLength behind. At walk (35 world vox/s on a
    # 5.79-voxel leg) that is 0.104 + 0.050 = 0.154 s per step, so 308 ms per
    # arm cycle; at the 60 vox/s sprint it is 0.090 + 0.029 = 0.119, so 238 ms.
    #
    # Re-derive these if the stride budget, stepThreshold or the speeds move —
    # an arm cycle that does not divide the step cycle is what makes the arms
    # visibly drift in and out of phase with the feet.
    #
    # Amplitudes: a 24-degree swing at this cadence is a windmill.
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

    # jump / fall / land — additive so they read over whatever the legs do
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

    # cast: the right arm thrusts forward. Override + masked to that arm so it
    # fully owns the limb while it plays; everything else keeps walking. This
    # is the clip the avatar plays on the attack/use input.
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

    # flinch on non-fatal damage (MobSystem plays "attack"; the avatar plays
    # this by the same name, so both drivers share one convention)
    clips["attack"] = clips["cast"]

    # ---- dismemberment locomotion clips ----------------------------------
    # These are the poses the AnimStateRule table below selects between. Each
    # owns the parts it masks.

    # limp: one leg is gone. The gait keeps running (no disableGait) so the
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
                     "pos": [{"t": 0, "v": [0, -0.3, 0], "ease": "quadInOut"},
                             {"t": 500, "v": [0, -0.08, 0], "ease": "quadInOut"},
                             {"t": 1000, "v": [0, -0.3, 0]}]},
            "armU.L": {"rot": [{"t": 0, "q": qx(-26), "ease": "quadInOut"},
                               {"t": 500, "q": qx(-8), "ease": "quadInOut"},
                               {"t": 1000, "q": qx(-26)}]},
            "armU.R": {"rot": [{"t": 0, "q": qx(-14)}]},
        },
    }

    # hop: both feet gone but the thighs remain — the figure bunny-hops.
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

    # one-armed: an arm is gone, so the empty shoulder hangs and the body
    # tilts to compensate.
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

    # headless: the hood is gone. The body keeps walking for a few strides,
    # hands groping at the missing head. Additive over the normal gait.
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
    # WHY THESE NAME LEG PARTS INSTEAD OF USING minChainsLost. AnimSelectState
    # counts EVERY IK chain, and this rig has arm chains as well as leg chains
    # (unlike the critter, which is all legs). `minChainsLost: 2` would
    # therefore fire "crawl" when both ARMS came off and the legs were fine.
    # Naming the leg parts directly keeps each rule about the thing it is
    # actually describing.
    #
    # bodyYOffset is in WORLD VOXELS, and this figure is 17 of them rather than
    # the wizard's 28 — the drops below are scaled down to match, or a crawling
    # Mina would sink through the floor.
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
        # SPEED IS NOT FREE EITHER — see the height contract at the top of this
        # file. `speed` is the reference top speed in world voxels/sec, and the
        # runtime divides the measured speed by it to get `speedFactor`, which
        # scales cadence, bob, sway, roll and the spring goals. The player is
        # driven by tuning.json's sprintSpeed (m/s), so a mob-sized literal here
        # leaves speedFactor pinned at its 1.5 clamp for every step the player
        # ever takes: max bob, max sway, max roll, max head deflection, always.
        # Sprint is the reference because that is the fastest normal locomotion.
        "speed": SPRINT_SPEED_MPS / VOXEL_METERS,
        "gait": {
            # Biped: two SINGLETON groups, so only one foot may swing at a
            # time. That single constraint is the whole gait state machine.
            "groups": [["legU.L"], ["legU.R"]],
            # TUNED FOR THE PLAYER'S SPEED, not a mob's. This figure is 1.7 m
            # tall on a ~0.58 m leg but moves at 3.5 m/s walking and 6.0
            # sprinting — Froude numbers of 2.2 and 6.4, where a human walks at
            # 0.25. It physically cannot take human-proportioned strides at
            # that speed, so the cadence has to be high and the swing short or
            # the body outruns its own planted foot and the leg trails straight
            # out behind (which is exactly what it did). avatar.cpp additionally
            # scales stepDuration DOWN with speed; these are the values at the
            # sprint reference.
            # The body advances v*stepDuration during a swing and only one leg
            # may swing at a time, so the whole cycle has to fit inside the
            # reach budget (~1.4 leg lengths of travel per step). At the sprint
            # reference that is ~134 ms, hence a 0.10 s swing and a low
            # threshold. It reads as a quick scurry, which is the honest look of
            # this speed on this body — the alternative is a leg that trails.
            "cadence": 8.0, "strideBias": 0.42, "leadTime": 0.10,
            "stepThreshold": 0.3, "stepDuration": 0.10, "stepHeight": 0.18,
            # rideHeight is a FRACTION of leg length, so it carries over from
            # the wizard unchanged even though the legs are much shorter.
            #
            # It is now a stance trim ABOUT the rig's authored rest pose (1.0 =
            # stand exactly as modelled), not an absolute lift off the foot
            # plane. The extra 0.28 is this rig's ankle-to-sole overhang: the
            # gait measures the rest sole at the ankle pivot, and mina's shoe
            # hangs ~1.6 voxels below it, so without the trim she stands that
            # far into the ground.
            "rideHeight": 1.22,
            # bob/sway are in world voxels: scaled down with the figure so the
            # walk does not look like it is wading.
            "bobAmp": 0.045, "bobFreqMul": 2.0, "swayAmp": 0.03,
            "rollAmp": 0.06, "spineCounter": 0.75, "phaseLag": 0.05,
        },
        "limbs": limbs,
        "sockets": sockets,
        "chains": chains,
        "states": states,
        "clips": clips,
        "flipbooks": {},
    }

    with open(os.path.join(out_dir, "mina.json"), "w") as f:
        json.dump(sidecar, f, indent=2)
        f.write("\n")

    total = sum(len(SHAPES[n](LIMBS[n][0]) if n in SHAPES else
                    solid(LIMBS[n][0], LIMBS[n][2])) for n in ORDER)
    print(f"wrote mina.vox ({len(ORDER)} parts, ~{total} micro voxels, "
          f"{WORLD_H} world voxels tall) and mina.json "
          f"({len(clips)} clips, {len(states)} states)")


if __name__ == "__main__":
    main()
