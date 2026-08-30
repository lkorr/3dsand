#!/usr/bin/env python3
"""Generate assets/mobs/human.{vox,json} — the STOCK BASE PLAYER MODEL.

WHY THIS EXISTS. `mina` has been standing in as the player avatar, and she is
not a base model: she is a finished character. The hood, the robe and the gold
sash are welded into her geometry, so there is nowhere for a helmet, a cuirass
or a pair of boots to go — equipping armour onto her would mean drawing armour
that fits one specific robe. This file is the opposite: a plain body with the
minimum clothing that keeps it decent, so the armour system can layer anything
on top of it and so the character-customisation pass has something neutral to
vary.

WHAT IS THE SAME AS MINA, DELIBERATELY. The rig (15 limbs, the same names, the
same parents, joints, tags, chains and sockets), skinScale 8, the 17-world-voxel
height contract, the sprint-speed contract, the gait block, and the whole
dismemberment clip/state ladder. Anything that already knows how to drive `mina`
drives this without a line of code — including avatar.cpp, the ledge-grab `hang`
clip and the states table.

WHAT IS DIFFERENT.
  1. TORSO +4 art micro (14 -> 18). Mina's chest is short because a robe hides
     where a chest ends; a bare one has to read.
  2. HEAD 22 -> 16 art micro (0.73x). Mina's "head" is a hood with a peak, which
     is why it could be a third of her total height. A bare skull at that size
     is a bobblehead. 0.73x is the nearest whole art micro to the 0.75x asked
     for, and the height budget below is what makes it whole.
  3. The 6 micro the head gave up went to the torso (+4) and the legs (+2), so
     the total is still exactly 68 art micro = 17 world voxels.
  4. EVERY VOXEL IS ONE MATERIAL — see the next section.

ONE MATERIAL, MANY COLOURS. Every voxel of this figure is `skin` (materials.json
index 51), which is the FLESH material: it cooks to flesh_cooked, burns, chars,
and bleeds like a creature everywhere, including under the shorts. What varies
is `color`, an ART PALETTE slot carried in a parallel "<name>.col" model
(sim/voxload.h, assets/editor/vox.js). That split is the whole point of the art
layer: art colour never reaches the world grid, so it can never touch the sim
hash, while material is what the CA reacts to. Mina does the opposite — she
paints with materials (robe_cloth, robe_trim, leather), which means her sash
burns on a different schedule from her sleeve and her shoes do not burn at all.
For a base body that armour will cover, uniform flesh is the correct answer and
the paint is free.

This is the FIRST shipped asset to use the .col path; nothing else in
assets/mobs paints. If colours come out as material IDs, the suspect is the
RGBA chunk or the ART_BASE agreement, not the geometry.

FACING AND HANDEDNESS. Identical to gen_mina.py and NOT re-derived here, since
both were got wrong once already: the loader maps scene -> engine as (x, z, -y),
which negates scene y and therefore FLIPS handedness. So the model is authored
with its face toward scene +Y and mirrored once by flip_y at the end of every
builder, and model +X is the character's LEFT — .L limbs take POSITIVE scene x.

CENTRING. Unlike mina, every box here is centred so that its ellipse centre
(mn.x + sx*0.5) lands on x = 0, and left/right boxes are exact mirrors about it.
Mina's are not (her torso centre is x = 0.5 and her arms are a micro apart in
span), which is invisible on a robe and would not be on a bare shoulder line.

Run:  python scripts/gen_human.py
"""
import json
import math
import os
import re
import struct

# ---- material ---------------------------------------------------------------
# PALETTE CONVENTION (PLAN_voxel_art_and_mobs.md A1): .vox palette index i+1 ==
# materials.json[i]. FLESH 51 is materials.json[50] == "skin", the material the
# flesh reaction chain is authored against (skin -> flesh_cooked ->
# flesh_burning -> flesh_charred, assets/materials/reactions.json). Asserted
# against materials.json in main() rather than trusted.
FLESH = 51
FLESH_ID = "skin"

# ---- art palette ------------------------------------------------------------
# Slots are allocated DOWNWARD from ART_TOP, mirroring assets/editor/vox.js's
# ArtPalette so a document round-tripped through the editor keeps its numbering.
# Only `color` varies across this figure; `material` never does.
SKIN_BASE = 255   # mid neutral — the customisation pass varies this row
SKIN_SHADE = 254  # the away-facing side of everything
SKIN_LIGHT = 253  # unused by geometry today; reserved so the row exists
HAIR = 252
HAIR_SHADE = 251
EYE = 250
CLOTH = 249       # the shorts: undyed linen, the only clothing on the figure
CLOTH_SHADE = 248
NAIL = 247        # reserved (fingernails/toenails are below this resolution)

ART_RGB = {
    SKIN_BASE:  0xC98D63,
    SKIN_SHADE: 0xA06E4B,
    SKIN_LIGHT: 0xE2B189,
    HAIR:       0x4A3728,
    HAIR_SHADE: 0x33251A,
    EYE:        0x241D18,
    CLOTH:      0x7A6A4E,
    CLOTH_SHADE: 0x5C4F39,
    NAIL:       0xD9AE90,
}

# ---- scale contract ---------------------------------------------------------
# Identical to gen_mina.py and for the same reason: every literal in ART_LIMBS
# and in the builders is an absolute count of micro voxels at the resolution it
# was DRAWN for. The geometry is generated at ART_SCALE and then upscaled 2x
# (every voxel becomes a 2x2x2 block, exactly the editor's upscale2x), which
# preserves world size bit for bit. The engine path runs at skinScale 8 end to
# end so the next revision can carve half-voxel detail with no format change.
ART_SCALE = 4
SKIN_UPSCALE = 2
SCALE = ART_SCALE * SKIN_UPSCALE   # 8 skin voxels per world voxel, as shipped
WORLD_H = 17                        # world voxels head to toe = Player::kHalfY*2
MICRO_H = WORLD_H * ART_SCALE       # 68, in the AUTHORED lattice

# The eye line, in AUTHORED scene z. Player::kEyeOffset (0.65 m above the AABB
# centre, i.e. 1.50 m up at kVoxelMeters 0.10) puts the first-person camera at
# world voxel 15 = art micro 60. This is the one number in the table below that
# is not free, and head_vox() derives the eye row from it rather than restating
# it — see the note there about the trap this avoids.
EYE_Z = 15 * ART_SCALE

# The SPEED contract: `speed` in the sidecar is the reference top speed in world
# voxels/sec that the runtime divides measured speed by to get `speedFactor`,
# which scales cadence, bob, sway, roll, the spring goals and the swing
# duration. So it must be the PLAYER's top speed, not a mob's.
#
# READ, NOT ASSERTED. gen_mina.py pins this at 6.0 m/s and asserts tuning.json
# agrees; tuning.json now says 3.15, so that assert fires and mina.json's
# `speed: 60` has been stale by ~2x ever since — her speedFactor never exceeds
# 0.53, which quietly halves every gait amplitude the factor scales. Reading the
# live value instead means this file cannot go stale the same way, and the walk
# and run clip periods below are derived from it rather than transcribed.
VOXEL_METERS = 0.10


# ---- .vox writing -----------------------------------------------------------
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


def rgba_chunk(mat_rgb):
    """The 256-entry palette. ENTRY i IS PALETTE INDEX i+1 — index 0 is "empty"
    and has no entry, and getting that off by one shifts every colour by one
    slot, which looks almost right. voxload.cpp reads it with the same bias.

    The low end is filled with the material colours so the file opens sensibly
    in MagicaVoxel and the editor; the ENGINE ignores it there (material colours
    come from materials.json). Only kArtPaletteBase..kArtPaletteTop is read
    back, and only for models that have a ".col" layer.

    Unused art slots are left at 0. That matters: MicroBodyMergeArt walks all
    128 slots and dedupes by RGB, so leaving them black costs exactly ONE
    merged slot for the whole run rather than one each."""
    pal = bytearray(1024)
    for idx, rgb in mat_rgb.items():
        o = (idx - 1) * 4
        pal[o:o + 4] = bytes(((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255))
    for slot, rgb in ART_RGB.items():
        o = (slot - 1) * 4
        pal[o:o + 4] = bytes(((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255))
    return chunk(b"RGBA", bytes(pal))


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
# BUILDERS RETURN COLOUR, NOT MATERIAL. Every builder yields (x, y, z, art_slot);
# main() writes the same cells twice — once with FLESH into "<name>", once with
# the slot into "<name>.col". A builder therefore cannot accidentally introduce
# a second material, which is the invariant this whole file is built on.
def flip_y(size, voxels):
    """Mirror front-to-back once, at the end. Every builder is authored in the
    natural "face at high y" reading and flipped here, which keeps shape
    placement and limb placement using ONE idea of "front"."""
    sy = size[1]
    return [(x, sy - 1 - y, z, c) for (x, y, z, c) in voxels]


def ellipse_mask(x, y, cx, cy, rx, ry):
    """True when voxel (x,y)'s CENTRE is inside the axis-aligned ellipse. Voxel
    i covers [i, i+1) so its centre is i+0.5 — that half is added here, once."""
    if rx <= 0 or ry <= 0:
        return False
    dx = (x + 0.5 - cx) / rx
    dy = (y + 0.5 - cy) / ry
    return dx * dx + dy * dy <= 1.0


def profile(u, pts):
    """Linear interpolation over an ascending (u, value) table.

    The torso, pelvis and skull are all "a stack of ellipses whose radii follow
    a curve", and a table is far easier to read and to adjust than the nested
    lerps that produced mina's silhouette — you can see the waist, the chest and
    the shoulder line as three numbers."""
    if u <= pts[0][0]:
        return pts[0][1]
    for (u0, v0), (u1, v1) in zip(pts, pts[1:]):
        if u <= u1:
            t = (u - u0) / (u1 - u0) if u1 > u0 else 0.0
            return v0 + (v1 - v0) * t
    return pts[-1][1]


def limb_tube(size, r0, r1, col_for):
    """A vertical (z-axis) limb: a tube whose XY radius lerps r0 -> r1.

    A NOTE ON WHY THE TAPER IS INVISIBLE, so nobody spends an hour tuning it.
    Every arm and leg box here is 4 micro across, and on a 4-wide lattice the
    ellipse test has exactly two outcomes: r >= ~1.57 gives the 12-cell rounded
    square (4x4 minus the corners) and anything below gives a bare 2x2. There is
    no cross-section in between. So r0/r1 select "limb" or "twig", and real
    taper has to come from the BOX SIZES in ART_LIMBS. Kept as parameters
    anyway, because the same helper at a wider box does taper.

    `col_for(x, y, z, sx, sy, sz)` returns the art slot per cell."""
    sx, sy, sz = size
    cx, cy = sx * 0.5, sy * 0.5
    out = []
    for z in range(sz):
        t = z / max(sz - 1, 1)
        r = r0 + (r1 - r0) * t
        for y in range(sy):
            for x in range(sx):
                if ellipse_mask(x, y, cx, cy, r, r):
                    out.append((x, y, z, col_for(x, y, z, sx, sy, sz)))
    return out


def shade_back(y, cy, base, shade, depth=1.8):
    """The one shading rule the whole figure uses: cells on the away-facing side
    take the darker slot. In the AUTHORED frame front is +y, so "back" is low y.

    Deliberately a REGION rule and not a surface rule. A surface rule repaints
    whatever happens to be outermost, so it moves when the silhouette changes
    and it speckles wherever the surface is one cell thick."""
    return shade if y + 0.5 < cy - depth else base


# ---- per-limb builders ------------------------------------------------------
def head_vox(size):
    """A bare head: neck stub, skull, a swept hair cap, two eyes and a mouth.

    THE EYE ROW IS DERIVED, NOT WRITTEN DOWN. `eye_z` is EYE_Z minus this limb's
    own scene z, read out of ART_LIMBS — the AUTHORED table, never the upscaled
    one. gen_mina.py does the same computation against the module-global LIMBS,
    which is rebound to the SHIPPING table before main() runs, so mina's works
    out to -32, every face test fails, and her face void has silently not been
    carved since the 2x upscale landed. Reading ART_LIMBS by name is what makes
    that class of mistake impossible here; the assert below is the backstop."""
    sx, sy, sz = size
    cx, cy = sx * 0.5, sy * 0.5
    # The skull sits BEHIND the face plane — a human has far more cranium behind
    # the eyes than in front of them, and centring the ellipsoid gives a snout.
    hcy = cy - 0.6
    eye_z = EYE_Z - ART_LIMBS["head"][1][2]
    assert 0 <= eye_z < sz, (
        f"eye row {eye_z} is outside the head model (0..{sz - 1}); the head's "
        f"scene z or its height moved without EYE_Z following")

    neck_top = 2           # z 0..2 is neck; the skull starts above it
    skull_lo = neck_top + 1
    # Radii as a fraction u of the skull's own height. Read this as a face:
    # chin, jaw, cheekbones, temples, the round of the crown.
    prof_rx = [(0.00, 2.3), (0.18, 3.4), (0.40, 4.2), (0.70, 4.3),
               (0.88, 3.8), (1.00, 2.4)]
    prof_ry = [(0.00, 2.7), (0.18, 3.7), (0.45, 4.6), (0.75, 4.6),
               (1.00, 2.9)]

    def hair_line(y):
        """The z at or above which this depth row is hair. Lower at the back
        (the nape) than at the front (the forehead), which is what makes the cap
        read as swept hair rather than as a painted-on helmet."""
        t = (y + 0.5 - (hcy - 4.0)) / 8.0
        t = min(max(t, 0.0), 1.0)
        return 5.5 + 4.5 * t

    out = []
    for z in range(sz):
        if z <= neck_top:
            rx = ry = 2.0
        else:
            u = (z - skull_lo) / max(sz - 1 - skull_lo, 1)
            rx, ry = profile(u, prof_rx), profile(u, prof_ry)
        for y in range(sy):
            for x in range(sx):
                if not ellipse_mask(x, y, cx, hcy, rx, ry):
                    continue
                back = y + 0.5 < hcy - 1.5
                if z >= skull_lo and z >= hair_line(y):
                    col = HAIR_SHADE if back else HAIR
                else:
                    col = SKIN_SHADE if back else SKIN_BASE
                out.append((x, y, z, col))

    # Ears: appended, not carved. They sit one cell proud of the widest part of
    # the skull, which the ellipse leaves empty, so there is nothing to conflict
    # with and the dedup in main() never has to arbitrate.
    ear_y = int(round(hcy)) - 1
    for z in (eye_z - 1, eye_z):
        out.append((0, ear_y, z, SKIN_SHADE))
        out.append((sx - 1, ear_y, z, SKIN_SHADE))

    # Eyes and mouth are repaints of the FRONTMOST existing cell in their
    # column, so they can never float in front of the face or sink into it.
    def paint_front(cols, z, col):
        front = {}
        for i, (x, y, zz, _c) in enumerate(out):
            if zz != z or x not in cols:
                continue
            if x not in front or y > out[front[x]][1]:
                front[x] = i
        for i in front.values():
            x, y, zz, _c = out[i]
            out[i] = (x, y, zz, col)

    eye_dx = 2.5   # half the pupil separation, in micro, about the centre line
    paint_front({int(cx - eye_dx), int(cx + eye_dx)}, eye_z, EYE)
    paint_front({int(cx) - 1, int(cx)}, eye_z - 3, SKIN_SHADE)   # mouth
    return flip_y(size, out)


def torso_vox(size):
    """Shoulders, chest and waist, bare.

    The silhouette is three numbers in prof_rx: a narrow waist, a chest that
    widens, and a shoulder line that is the widest thing on the upper body. It
    tucks in on the last row so the head's neck stub meets it instead of sitting
    on a flat slab."""
    sx, sy, sz = size
    cx, cy = sx * 0.5, sy * 0.5
    # RADII ARE KEPT CLEAR OF THE BOX EDGE ON PURPOSE. The test is an ellipse in
    # x AND y, so a radius that only just reaches the outermost column admits
    # one or two cells at mid-depth and none either side of them — a single
    # 2x2x2 pimple on the shoulder line at ship scale, on exactly the row where
    # the curve happens to round up. Every radius here tops out where the widest
    # column still fills four depth rows, and the ARM BOXES are placed against
    # the column the torso actually fills (scene x -5..4), not against the box.
    prof_rx = [(0.00, 3.5), (0.30, 3.9), (0.62, 4.8), (0.88, 5.2), (1.00, 5.0)]
    prof_ry = [(0.00, 2.7), (0.35, 3.2), (0.70, 3.4), (1.00, 2.8)]
    out = []
    for z in range(sz):
        u = z / max(sz - 1, 1)
        rx, ry = profile(u, prof_rx), profile(u, prof_ry)
        for y in range(sy):
            for x in range(sx):
                if ellipse_mask(x, y, cx, cy, rx, ry):
                    out.append((x, y, z, shade_back(y, cy, SKIN_BASE, SKIN_SHADE)))
    return flip_y(size, out)


def hips_vox(size):
    """The pelvis, wearing the figure's only clothing.

    THE SHORTS ARE PAINT, NOT GEOMETRY, and not a second material either. They
    are three rows of CLOTH slots on a body that is `skin` all the way through,
    which is what leaves the armour system a clean surface to sit on: an
    equipped greave has to fit a leg, not a leg-plus-a-hem. The bare rows above
    the waistband are what tell you at a glance that the torso is unarmoured."""
    sx, sy, sz = size
    cx, cy = sx * 0.5, sy * 0.5
    prof_rx = [(0.00, 4.4), (0.40, 4.9), (1.00, 4.6)]
    prof_ry = [(0.00, 2.8), (0.50, 3.3), (1.00, 3.0)]
    waist = sz - 3     # the waistband row; everything above it is bare skin
    out = []
    for z in range(sz):
        u = z / max(sz - 1, 1)
        rx, ry = profile(u, prof_rx), profile(u, prof_ry)
        for y in range(sy):
            for x in range(sx):
                if not ellipse_mask(x, y, cx, cy, rx, ry):
                    continue
                if z > waist:
                    col = shade_back(y, cy, SKIN_BASE, SKIN_SHADE)
                elif z == waist:
                    col = CLOTH_SHADE          # the waistband
                else:
                    col = shade_back(y, cy, CLOTH, CLOTH_SHADE)
                out.append((x, y, z, col))
    return flip_y(size, out)


def upper_arm_vox(size):
    return flip_y(size, limb_tube(
        size, 2.0, 2.2,
        lambda x, y, z, sx, sy, sz: shade_back(y, sy * 0.5, SKIN_BASE, SKIN_SHADE)))


def fore_arm_vox(size):
    return flip_y(size, limb_tube(
        size, 1.8, 2.0,
        lambda x, y, z, sx, sy, sz: shade_back(y, sy * 0.5, SKIN_BASE, SKIN_SHADE)))


def hand_vox(size):
    """A mitten hand with a thumb nub. At 4 micro across there is no room for
    separate fingers — inventing five here produces speckle, not a hand."""
    sx, sy, sz = size
    cx, cy = sx * 0.5, sy * 0.5
    out = []
    for z in range(sz):
        t = z / max(sz - 1, 1)
        r = 1.6 + 0.6 * t          # narrower at the fingertips (low z)
        for y in range(sy):
            for x in range(sx):
                if ellipse_mask(x, y, cx, cy, r, r * 0.85):
                    out.append((x, y, z, shade_back(y, cy, SKIN_BASE, SKIN_SHADE)))
    out.append((int(cx) + 1, sy - 1, sz - 2, SKIN_BASE))   # thumb, proud
    return flip_y(size, out)


def thigh_vox(size):
    """Upper leg. The top rows carry the short's leg, so the hem lands on the
    THIGH rather than at the pelvis seam — a hem exactly on a joint pops when
    the leg swings."""
    sz = size[2]
    hem = sz - 4

    def col(x, y, z, sx, sy, szz):
        if z == hem:
            return CLOTH_SHADE
        if z > hem:
            return shade_back(y, sy * 0.5, CLOTH, CLOTH_SHADE)
        return shade_back(y, sy * 0.5, SKIN_BASE, SKIN_SHADE)

    return flip_y(size, limb_tube(size, 1.9, 2.1, col))


def shin_vox(size):
    return flip_y(size, limb_tube(
        size, 1.7, 2.0,
        lambda x, y, z, sx, sy, sz: shade_back(y, sy * 0.5, SKIN_BASE, SKIN_SHADE)))


def foot_vox(size):
    """A bare foot: a sole reaching forward from an ankle column at the back.
    Same construction as mina's shoe — the ankle anchor's relationship to the
    sole is what `rideHeight` is trimmed against, so changing it here would
    invalidate the stance number derived in main()."""
    sx, sy, sz = size
    cx = sx * 0.5
    out = []
    for z in range(sz):
        for y in range(sy):
            for x in range(sx):
                in_sole = z < 2
                in_ankle = y < 3 and z >= 2
                if not (in_sole or in_ankle):
                    continue
                if not ellipse_mask(x, y if in_ankle else 2, cx, 2.0, 1.9, 2.4):
                    continue
                out.append((x, y, z, SKIN_SHADE if z == 0 else SKIN_BASE))
    return flip_y(size, out)


# ---- limb table -------------------------------------------------------------
# name -> (size, min-corner in SCENE space)
#
# AUTHORED units: 4 per world voxel, 2.5 cm each. Scene is Z-up and engine Y =
# scene Z, so the z column IS the height. `LIMBS` below is this table times
# SKIN_UPSCALE; NOTHING should read `LIMBS` to recover an authored number.
#
# HEIGHT BUDGET (art micro; /4 for world voxels). Soles at z 0, crown at z 68.
#
#   foot   z  0..3     bare
#   shin   z  4..16
#   thigh  z 16..29    shorts hem over the top 4 rows
#   hips   z 28..37    the shorts; bare skin above the waistband
#   torso  z 35..52    bare chest, shoulders at the top
#   head   z 52..67    neck z 52..54, skull above, EYES AT z 60
#
# The eye row is the only entry that is not free: EYE_Z = 60 is world voxel 15,
# which is where Player::kEyeOffset puts the first-person camera. head_vox
# derives its face from it.
#
# CENTRING: mn.x = -sx/2 for every centred box, so the ellipse centre lands on
# x = 0; .L and .R boxes are exact mirrors (mn.x_R = -mn.x_L - sx). HANDEDNESS:
# model +X is the character's LEFT, so .L takes the POSITIVE x. See the module
# docstring — this is not the intuitive reading and it survives no re-derivation
# from the facing.
ART_LIMBS = {
    #             size          min corner (x, y, z)
    "hips":     ((10, 8, 10), (-5, -4, 28)),
    "torso":    ((12, 8, 18), (-6, -4, 35)),
    "head":     ((10, 10, 16), (-5, -5, 52)),

    # Arms hang FLUSH against the shoulder line. The torso's BOX is x -6..5 but
    # what it FILLS at its widest is x -5..4 (see the radius note in torso_vox),
    # so an arm at x 5..8 touches the shoulder and no lower. Mina leaves a
    # 1-micro gap to break up an unbroken robe; a bare shoulder with a gap is a
    # detached arm. The waist gap further down falls out of the taper, which is
    # where a gap belongs.
    "armU.L":   ((4, 4, 11), (5, -2, 42)),
    "armL.L":   ((4, 4, 11), (5, -2, 32)),
    "hand.L":   ((4, 4, 5), (5, -2, 28)),
    "armU.R":   ((4, 4, 11), (-9, -2, 42)),
    "armL.R":   ((4, 4, 11), (-9, -2, 32)),
    "hand.R":   ((4, 4, 5), (-9, -2, 28)),

    # The legs are pushed 1 micro OUT of the mina positions so cells -1 and 0
    # stay empty. A 4-wide limb box is either the 12-cell rounded square or
    # nothing (see limb_tube), so two adjacent boxes render as one 8-wide column
    # with no inner edge at all — a skirt hides that and a bare body does not.
    "legU.L":   ((4, 4, 14), (1, -2, 16)),
    "legL.L":   ((4, 4, 13), (1, -2, 4)),
    "foot.L":   ((4, 7, 4), (1, -4, 0)),
    "legU.R":   ((4, 4, 14), (-5, -2, 16)),
    "legL.R":   ((4, 4, 13), (-5, -2, 4)),
    "foot.R":   ((4, 7, 4), (-5, -4, 0)),
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

# Parent-before-child; the loader topologically sorts anyway, but authoring in
# order keeps the .vox scene graph readable in MagicaVoxel.
ORDER = ["hips", "torso", "head",
         "armU.L", "armL.L", "hand.L",
         "armU.R", "armL.R", "hand.R",
         "legU.L", "legL.L", "foot.L",
         "legU.R", "legL.R", "foot.R"]

# Held props are excluded from the height contract and the prefab-origin
# measurement, mirroring the loader (mob.cpp skips tag == "prop" when it
# measures worldSize). This rig has none, and a WEAPON should be an item with
# its own .vox held through the socket below, never a prop limb — see the long
# note in gen_mina.py for what putting a sword in a rig actually broke.
PROPS = set()

# ---- author at ART_SCALE, ship at SCALE -------------------------------------
LIMBS = {n: (tuple(v * SKIN_UPSCALE for v in size),
             tuple(v * SKIN_UPSCALE for v in mn))
         for n, (size, mn) in ART_LIMBS.items()}


def upscale_voxels(voxels, factor):
    """Every voxel becomes a factor^3 block — the generator's twin of the
    editor's upscale2x. Shape is preserved exactly; only the sampling changes,
    which is what keeps an upscale from being a redraw."""
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


def scale_clip_positions(clips, factor):
    """`pos` keys are micro units and the clip literals are authored at
    ART_SCALE, so they need the same multiplier the limb table got. `rot` keys
    are quaternions and must NOT be touched."""
    if factor == 1:
        return clips
    for c in clips.values():
        for tr in (c.get("tracks") or {}).values():
            for k in (tr.get("pos") or []):
                k["v"] = [round(v * factor, 4) for v in k["v"]]
    return clips


def to_engine(scene_xyz, min_x, max_y):
    """Scene (Z-up) -> prefab-local engine coords (Y-up, min corner 0), matching
    voxload.cpp: engine = (x, z, -y), rebased so the prefab min corner is 0."""
    x, y, z = scene_xyz
    return [round(x - min_x, 1), round(float(z), 1), round(max_y - y, 1)]


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "assets", "mobs")
    os.makedirs(out_dir, exist_ok=True)

    # ---- the contracts, asserted against the files that declare them --------
    with open(os.path.join(root, "assets", "materials", "materials.json")) as mf:
        mats = json.load(mf)["materials"]
    assert mats[FLESH - 1]["id"] == FLESH_ID, (
        f"materials.json[{FLESH - 1}] is {mats[FLESH - 1]['id']!r}, not "
        f"{FLESH_ID!r} — the palette index every voxel of this figure carries "
        f"has moved, and the whole body would load as another material")
    mat_rgb = {i + 1: int(m["colors"][0].lstrip("#"), 16)
               for i, m in enumerate(mats) if m.get("colors")}

    body_parts = {n: v for n, v in ART_LIMBS.items() if n not in PROPS}
    top = max(mn[2] + sz[2] for (sz, mn) in body_parts.values())
    bottom = min(mn[2] for (_sz, mn) in body_parts.values())
    assert bottom == 0, f"figure does not stand on z=0 (lowest part at {bottom})"
    assert top == MICRO_H, (
        f"figure is {top} art micro ({top / ART_SCALE} world voxels) tall, "
        f"expected {MICRO_H} ({WORLD_H}) to match Player::kHalfY * 2")

    with open(os.path.join(root, "assets", "materials", "tuning.json")) as tf:
        player = json.load(tf)["player"]
    sprint_mps, walk_mps = player["sprintSpeed"], player["walkSpeed"]
    with open(os.path.join(root, "src", "sim", "world.h")) as wf:
        m = re.search(r"kVoxelMeters\s*=\s*([0-9.]+)f", wf.read())
    assert m, "could not find kVoxelMeters in src/sim/world.h"
    assert abs(float(m.group(1)) - VOXEL_METERS) < 1e-9, (
        f"world.h kVoxelMeters is {m.group(1)}, this file assumes "
        f"{VOXEL_METERS} — update VOXEL_METERS and regenerate")
    # The height contract's other half: the AABB and the eye offset this rig is
    # drawn to. Restating them here as literals is what let mina's `speed` rot;
    # reading them means a tuning change complains at generation time.
    assert abs(player["halfHeight"] * 2 / VOXEL_METERS - WORLD_H) < 1e-6, (
        f"tuning.json player.halfHeight {player['halfHeight']} is "
        f"{player['halfHeight'] * 2 / VOXEL_METERS} world voxels tall, this "
        f"file draws {WORLD_H}")
    assert abs((player["halfHeight"] + player["eyeOffset"]) / VOXEL_METERS
               - EYE_Z / ART_SCALE) < 1e-6, (
        f"tuning.json puts the eye at "
        f"{(player['halfHeight'] + player['eyeOffset']) / VOXEL_METERS} world "
        f"voxels, this file draws the face at {EYE_Z / ART_SCALE}")

    # The four gait constants avatar.cpp owns, read out of it rather than
    # copied. The step-period model below is only worth deriving if it is the
    # SAME model the runtime runs, and a silently-drifted constant would give a
    # confidently wrong number instead of an error.
    with open(os.path.join(root, "src", "game", "avatar.cpp")) as af:
        asrc = af.read()

    def cpp_const(name):
        mm = re.search(r"constexpr float " + name + r"\s*=\s*([0-9.]+)f", asrc)
        assert mm, f"could not find {name} in src/game/avatar.cpp"
        return float(mm.group(1))

    swing_frac = cpp_const("kSwingTravelFrac")
    max_lead = cpp_const("kMaxLeadLegLengths")
    min_swing_scale = cpp_const("kMinSwingScale")
    min_swing_s = cpp_const("kMinSwingSeconds")

    # Mirror check. Cheap, and it catches the one class of typo that a
    # screenshot from the front cannot: a limb whose left and right boxes are
    # not reflections of each other about x = 0.
    for n, (sz, mn) in ART_LIMBS.items():
        if not n.endswith(".L"):
            continue
        rsz, rmn = ART_LIMBS[n[:-2] + ".R"]
        assert rsz == sz and rmn[1:] == mn[1:] and rmn[0] == -mn[0] - sz[0], (
            f"{n} and its .R twin are not mirrors about x=0: "
            f"{mn}/{sz} vs {rmn}/{rsz}")

    # ---- geometry: each limb emitted TWICE ----------------------------------
    # "<name>" carries FLESH in every cell; "<name>.col" carries the art slot
    # over the SAME cells, at the SAME size and the SAME translation. The loader
    # matches the two by ABSOLUTE cell, so identical placement is not a
    # convenience here, it is the join condition.
    body = b""
    graph = b""
    grp_children = []
    painted = 0
    for i, name in enumerate(ORDER):
        size, mn = LIMBS[name]
        art_size, _art_mn = ART_LIMBS[name]
        # The int8 bound is about the DERIVED COLLIDER, not this lattice: the
        # engine picks physScale as the finest of {8,4,2,1} whose limb extents
        # fit +-120 (mob.h MobDef::physScale), so what must hold is that SOME
        # collider resolution fits — the part is under 120 WORLD voxels.
        world_extent = max(size) / SCALE
        assert world_extent <= 120, (
            f"{name} is {world_extent} world voxels; even a 1:1 collider "
            f"exceeds the DebrisVoxel int8 bound")
        cells = SHAPES[name](art_size)
        assert cells, f"{name} generated no voxels"
        cells = upscale_voxels(cells, SKIN_UPSCALE)
        seen = set()
        uniq = []
        for (x, y, z, c) in cells:
            assert 0 <= x < size[0] and 0 <= y < size[1] and 0 <= z < size[2], \
                f"{name} voxel {(x, y, z)} outside declared size {size}"
            if (x, y, z) in seen:
                continue          # first write wins
            seen.add((x, y, z))
            assert 128 <= c <= 255, (
                f"{name} cell {(x, y, z)} has colour {c}, which is not an art "
                f"palette slot — a builder returned a material by mistake")
            uniq.append((x, y, z, c))
        painted += len(uniq)

        body += model_chunks(size, [(x, y, z, FLESH) for (x, y, z, _c) in uniq])
        body += model_chunks(size, uniq)
        pivot = (size[0] // 2, size[1] // 2, size[2] // 2)
        t = (mn[0] + pivot[0], mn[1] + pivot[1], mn[2] + pivot[2])
        trn, shp, ctrn, cshp = 2 + 4 * i, 3 + 4 * i, 4 + 4 * i, 5 + 4 * i
        graph += ntrn(trn, name, shp, t) + nshp(shp, 2 * i)
        graph += ntrn(ctrn, name + ".col", cshp, t) + nshp(cshp, 2 * i + 1)
        grp_children += [trn, ctrn]
    graph = ntrn(0, "", 1, (0, 0, 0)) + ngrp(1, grp_children) + graph

    payload = body + rgba_chunk(mat_rgb) + graph
    data = b"VOX " + struct.pack("<i", 150)
    data += b"MAIN" + struct.pack("<ii", 0, len(payload)) + payload
    with open(os.path.join(out_dir, "human.vox"), "wb") as f:
        f.write(data)

    # ---- anchors ------------------------------------------------------------
    # Prefab extents in scene space. PROPS ARE EXCLUDED and that MIRRORS THE
    # LOADER (mob.cpp skips tag == "prop" when it measures worldSize): anchors
    # are rebased against this origin, so measuring a box the engine does not
    # shifts every anchor and moves the gait pivot off the body.
    body_x = {n: v for n, v in LIMBS.items() if n not in PROPS}
    min_x = min(mn[0] for (_sz, mn) in body_x.values())
    max_y = max(mn[1] + sz[1] for (sz, mn) in body_x.values())

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

    # A joint sits at the seam between a part and its parent, so restating its
    # height as a literal is a second source of truth that rots the moment a
    # limb is resized. These take offsets in AUTHORED micro and scale them here.
    U = float(SKIN_UPSCALE)

    def joint_top(part, dx=0.0, dy=0.0, inset=0.0):
        size, mn = LIMBS[part]
        return anchor((mn[0] + size[0] * 0.5 + dx * U,
                       mn[1] + size[1] * 0.5 + dy * U,
                       mn[2] + size[2] - inset * U))

    def joint_bottom(part, dx=0.0, dy=0.0, rise=0.0):
        size, mn = LIMBS[part]
        return anchor((mn[0] + size[0] * 0.5 + dx * U,
                       mn[1] + size[1] * 0.5 + dy * U,
                       mn[2] + rise * U))

    def joint_at(part, z, dx=0.0, dy=0.0):
        size, mn = LIMBS[part]
        return anchor((mn[0] + size[0] * 0.5 + dx * U,
                       mn[1] + size[1] * 0.5 + dy * U, z))

    # ---- limbs --------------------------------------------------------------
    # hp / severImpactSpeed carried over from mina unchanged: extremities come
    # off easily and the spine does not.
    hips_sz, hips_mn = LIMBS["hips"]
    waist_z = hips_mn[2] + hips_sz[2]
    limbs = [
        {"name": "hips", "hp": 60, "severable": False, "tag": "spine"},
        {"name": "torso", "parent": "hips", "joint": "ball", "hp": 60,
         "severable": False, "vital": True, "tag": "spine",
         "anchor": joint_at("hips", waist_z - 2 * U)},
        # neck: the top of the torso. This head model's origin IS its base (the
        # neck stub is the bottom three rows), so this is the seam.
        {"name": "head", "parent": "torso", "joint": "ball", "hp": 22,
         "severable": True, "vital": True, "tag": "head",
         "anchor": joint_top("torso", inset=1), "severImpactSpeed": 20.0,
         # `gain` is read against velocity NORMALIZED by `speed` (kSpringVelScale
         # in game/anim.h), so it means "radians of lag at full sprint" — a small
         # number, or the head sits pegged at maxAngle every step. maxAngle is a
         # safety rail for impacts, not the working range.
         "spring": {"halflife": 0.12, "gain": 0.18, "maxAngle": 0.3}},
    ]
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
    for side in ("L", "R"):
        limbs.append({
            "name": f"legU.{side}", "parent": "hips", "joint": "ball",
            "hp": 22, "severable": True, "tag": "leg",
            "anchor": joint_top(f"legU.{side}"),      # hip
            # RANGE OF MOTION FOR THE ANIMATION, which the minAngle/cone limits
            # elsewhere in this sidecar do NOT provide: those are Jolt
            # constraints and Jolt only enforces them on a dynamic body, while
            # a live limb is kinematic and re-posed every tick. So nothing
            # bounded the IK at all, and a thigh could be raked out behind the
            # body or folded up through the pelvis with no complaint from
            # anything but a selftest.
            #
            # The axis is NEGATED X so that positive reads FORWARD, the way a
            # human would describe a hip: on these rigs a positive rotation
            # about model +X swings a hanging limb BACKWARD. 0 is the authored
            # rest pose (straight down).
            # MEASURED, not guessed. With the limits opened right out, a
            # healthy walk on flat ground uses -44..+16 degrees here and the
            # ramp fixture reaches -61..+16 (selftest --gate mob prints both).
            # So 80 forward is ample, but 10 BACK clips an ordinary stride: the
            # trailing thigh genuinely needs ~16 degrees at 1.6 m/s on a 0.68 m
            # leg, and pinning it at 10 held the leg on its stop for a third of
            # every cycle. 20 back / 85 forward leaves honest margin and still
            # makes "raked out behind" and "folded through the pelvis"
            # unrepresentable.
            "poseLimit": {"axis": [-1, 0, 0], "min": -20, "max": 85},
            "severImpactSpeed": 18.0})
        limbs.append({
            "name": f"legL.{side}", "parent": f"legU.{side}", "joint": "hinge",
            "hp": 18, "severable": True, "tag": "leg", "axis": [1, 0, 0],
            "minAngle": -2.4, "maxAngle": 0.05,
            "anchor": joint_top(f"legL.{side}"),      # knee
            # A knee flexes one way and does not hyperextend. Flexion swings the
            # shin BACKWARD relative to the thigh, which is +X here (see the hip
            # note above), and 0 is the rest pose — so `min: 0` is exactly the
            # statement that the leg is never straighter than it was drawn.
            # Measured the same way: 82 on the flat, 88 climbing. 90 sat right
            # on top of that and the knee spent whole strides pinned at it, so
            # the ceiling is 100 — still far short of a knee folding the wrong
            # way, which is what `min: 0` is really guarding.
            "poseLimit": {"axis": [1, 0, 0], "min": 0, "max": 100},
            "severImpactSpeed": 16.0})
        limbs.append({
            "name": f"foot.{side}", "parent": f"legL.{side}", "joint": "hinge",
            "hp": 12, "severable": True, "tag": "foot", "axis": [1, 0, 0],
            "minAngle": -0.6, "maxAngle": 0.6,
            # ankle: above the sole, at the BACK of the foot (the toes reach
            # forward from here) so it pivots the way an ankle does
            "anchor": joint_bottom(f"foot.{side}", dy=-1.5, rise=3.0),
            "severImpactSpeed": 12.0})

    # ---- sockets ------------------------------------------------------------
    # A socket is named for the CONTEXT an item asks to be held in, not for the
    # limb it rides. `part` is the separate matter of which limb carries that
    # context on THIS rig, so a left-handed creature moves one line here and
    # every item still hangs correctly. No rotation: the socket frame IS the
    # hand's frame, and an item's own orientation is its business (its `grip`
    # block) — a rotation here too would be a second place encoding "which way
    # does a held thing point".
    hand_sz, hand_mn = LIMBS["hand.R"]
    sockets = [{
        "name": "held_right",
        "part": "hand.R",
        "offset": anchor((hand_mn[0] + hand_sz[0] * 0.5,
                          hand_mn[1] + hand_sz[1] * 0.5,
                          hand_mn[2] + hand_sz[2] * 0.5)),
        "rotation": [0, 0, 0],
    }]

    # ---- IK chains ----------------------------------------------------------
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

    # ---- gait numbers, derived ----------------------------------------------
    # THE ARM CLIPS PLAY AT A FIXED RATE WHILE THE LEGS STEP AT A
    # SPEED-DEPENDENT ONE, so the only way they read as one motion is to author
    # each period at the step cycle for the speed that clip is selected at
    # (avatar.cpp switches to `run` past 0.80 * speed). One arm cycle spans TWO
    # steps — left plants, then right — and a cycle that does not divide the
    # step cycle is exactly what makes arms visibly drift in and out of phase
    # with the feet. So these are derived, not rounded to something tidy.
    #
    # The model is the runtime's, with its own constants read above:
    #   stance = stepThreshold * legLength / v          (mob.cpp UpdateGait:
    #            the planted foot ends its stance once it has drifted that far
    #            behind the body)
    #   swing  = clamp(stepDuration / speedFactor) capped at the TRAVEL BUDGET,
    #            kSwingTravelFrac * kMaxLeadLegLengths * legLength / v
    #            (avatar.cpp: the swing may never last longer than it takes the
    #            body to consume the ground the foot is able to gain — a
    #            negative budget is the "feet ratchet backwards forever" bug)
    #
    # legLength is measured from the RIG (hip anchor to ankle anchor), for the
    # same reason the runtime measures it: the art moves and a hand-tuned
    # constant rots silently when it does.
    hip_z = ART_LIMBS["legU.L"][1][2] + ART_LIMBS["legU.L"][0][2]
    ankle_z = ART_LIMBS["foot.L"][1][2] + 3.0
    leg_len = (hip_z - ankle_z) / ART_SCALE          # world voxels
    ref_speed = sprint_mps / VOXEL_METERS            # world voxels/sec
    STEP_DURATION, STEP_THRESHOLD = 0.10, 0.3

    def arm_cycle_ms(v_mps):
        """Milliseconds for two steps at this speed — one full arm cycle."""
        v = v_mps / VOXEL_METERS
        speed_factor = min(max(v / ref_speed, 0.0), 1.5)
        dur = max(STEP_DURATION / max(speed_factor, min_swing_scale), min_swing_s)
        dur = max(min(dur, swing_frac * max_lead * leg_len / v), min_swing_s)
        stance = STEP_THRESHOLD * leg_len / v
        return int(round((dur + stance) * 2000.0))

    walk_ms, run_ms = arm_cycle_ms(walk_mps), arm_cycle_ms(sprint_mps)

    # rideHeight is a stance trim ABOUT the authored rest pose (1.0 = stand
    # exactly as modelled), expressed in LEG LENGTHS. It exists because
    # restSoleY_ lands on the ANKLE PIVOT, not the bottom of the foot, so every
    # rig sits low by its own ankle-to-sole overhang plus whatever the foot IK's
    # plant target contributes. This foot geometry and this ankle rise are
    # mina's unchanged, so the residual in WORLD VOXELS is hers unchanged too —
    # but it is a fraction of a leg, and this leg is longer. Converting through
    # the absolute is the only transfer that is actually valid.
    MINA_LEG, MINA_RIDE = 5.75, 1.22
    ride_height = round(1.0 + (MINA_RIDE - 1.0) * MINA_LEG / leg_len, 3)

    # ---- clips --------------------------------------------------------------
    # Quaternions are (x, y, z, w). Every clip is masked to the parts it owns,
    # so `cast` can play over a walk without touching the legs.
    def swing(deg, period=900, phase=0, ease="quadInOut"):
        a, b = (deg, -deg) if phase == 0 else (-deg, deg)
        half = period // 2
        return {"rot": [{"t": 0, "q": qx(a), "ease": ease},
                        {"t": half, "q": qx(b), "ease": ease},
                        {"t": period, "q": qx(a)}]}

    clips = {}

    # idle: a slow breath in the spine and a drifting head. Small amplitudes.
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

    # walk/run arm swing: additive over the gait, masked to the arms and spine
    # so the IK-driven legs are untouched. Periods derived above.
    # Arm swing, raised with the legs: the stride is a real 57-degree sweep
    # now that the foot IK is no longer clamped straight, and the previous
    # 16/22 was sized against a leg that barely moved.
    for nm, deg, period in (("walk", 22, walk_ms), ("run", 30, run_ms)):
        clips[nm] = {
            "durationMs": period, "loop": True, "mode": "additive",
            "blendInMs": 180, "blendOutMs": 180,
            "mask": ["armU.L", "armL.L", "armU.R", "armL.R", "torso"],
            "tracks": {
                "armU.L": swing(deg, period, 0),
                "armU.R": swing(deg, period, 1),
                "armL.L": {"rot": [{"t": 0, "q": qx(-10), "ease": "quadInOut"},
                                   {"t": period // 2, "q": qx(-26),
                                    "ease": "quadInOut"},
                                   {"t": period, "q": qx(-10)}]},
                "armL.R": {"rot": [{"t": 0, "q": qx(-26), "ease": "quadInOut"},
                                   {"t": period // 2, "q": qx(-10),
                                    "ease": "quadInOut"},
                                   {"t": period, "q": qx(-26)}]},
                "torso": {"rot": [{"t": 0, "q": qy(-deg * 0.16),
                                   "ease": "quadInOut"},
                                  {"t": period // 2, "q": qy(deg * 0.16),
                                   "ease": "quadInOut"},
                                  {"t": period, "q": qy(-deg * 0.16)}]},
            },
        }

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
                               {"t": 200, "q": qx(-42), "ease": "cubicInOut"},
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

    # HANG: the ledge-grab pose (game/avatar.cpp plays it by name). Both arms
    # overhead, forearms slightly bent, with a slow sag so a hanging player is
    # not a statue. Mina's clip lives only in her SIDECAR — gen_mina.py never
    # emitted it, so regenerating her would delete it. Authored here.
    clips["hang"] = {
        "durationMs": 1600, "loop": True, "mode": "override",
        "blendInMs": 120, "blendOutMs": 180,
        "mask": ["armU.L", "armL.L", "armU.R", "armL.R"],
        "tracks": {
            "armU.L": {"rot": [{"t": 0, "q": qx(-145), "ease": "quadInOut"},
                               {"t": 800, "q": qx(-148), "ease": "quadInOut"},
                               {"t": 1600, "q": qx(-145)}]},
            "armU.R": {"rot": [{"t": 0, "q": qx(-145), "ease": "quadInOut"},
                               {"t": 800, "q": qx(-148), "ease": "quadInOut"},
                               {"t": 1600, "q": qx(-145)}]},
            "armL.L": {"rot": [{"t": 0, "q": qx(-20)}]},
            "armL.R": {"rot": [{"t": 0, "q": qx(-20)}]},
        },
    }

    # cast: the right arm thrusts forward. Override + masked to that arm so it
    # fully owns the limb while it plays; everything else keeps walking.
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

    # ---- dismemberment locomotion clips ------------------------------------
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

    # ---- dismemberment locomotion states ------------------------------------
    # FIRST MATCH WINS, so this runs most-maimed to least; predicate fields are
    # AND-ed and an empty predicate never matches. These name LEG PARTS rather
    # than using minChainsLost because AnimSelectState counts EVERY IK chain and
    # this rig has arm chains too — `minChainsLost: 2` would fire "crawl" when
    # both ARMS came off and the legs were fine. bodyYOffset is in WORLD VOXELS
    # and this figure is 17 of them, same as mina, so hers carry over.
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
        # Every limb coordinate above is in 1/8-world-voxel units. "skinScale"
        # is the ART's resolution; the engine DERIVES the collider resolution
        # from it (the finest of {8,4,2,1} whose limbs fit the DebrisVoxel int8
        # bound) and logs what it picked at load.
        "skinScale": SCALE,
        "bleed": {"material": "blood", "perDamage": 2.5},
        # `speed` is the reference top speed in world voxels/sec; the runtime
        # divides measured speed by it to get speedFactor, which scales cadence,
        # bob, sway, roll and the spring goals. Read live from tuning.json (see
        # the SPEED contract note at the top) rather than transcribed.
        "speed": round(ref_speed, 4),
        "gait": {
            # Biped: two SINGLETON groups, so only one foot may swing at a time.
            # That single constraint is the whole gait state machine.
            "groups": [["legU.L"], ["legU.R"]],
            # stepDuration and stepThreshold are the two the arm-cycle
            # derivation above READS, so they are stated once here and referred
            # to by STEP_DURATION / STEP_THRESHOLD there — changing one without
            # the other is what puts the arms out of phase with the feet.
            # The body advances v*swing during a swing and only one leg may
            # swing at a time, so the cycle has to fit the reach budget; the
            # runtime caps it, and these are the values it caps.
            "cadence": 8.0, "strideBias": 0.42, "leadTime": 0.10,
            "stepThreshold": STEP_THRESHOLD, "stepDuration": STEP_DURATION,
            # A LOW ARC, because this rig stands with near-straight legs. Hip
            # to ankle is 6.75 against a 6.79-voxel chain, so the knee has to
            # fold hard for even a small lift: at the old 0.18 (1.22 voxels of
            # foot rise) the mid-swing knee wanted 113 degrees and sat on its
            # limit through the whole swing. 0.08 is 0.54 voxels -- 5 cm on a
            # 1.7 m body, which is what a walking foot actually clears -- and
            # lands the mid-swing knee near 80.
            "stepHeight": 0.08,
            "rideHeight": ride_height,
            # bob/sway are in world voxels, on a figure the same height as mina.
            "bobAmp": 0.045, "bobFreqMul": 2.0, "swayAmp": 0.03,
            "rollAmp": 0.06, "spineCounter": 0.75, "phaseLag": 0.05,
        },
        "limbs": limbs,
        "sockets": sockets,
        "chains": chains,
        "states": states,
        # Clip `pos` keys are MICRO units (the engine divides them by the scale
        # with everything else) and the literals above were measured against the
        # AUTHORED lattice, so they take the same multiplier the limb table got.
        # Rotations are angles and carry over untouched.
        "clips": scale_clip_positions(clips, SKIN_UPSCALE),
        "flipbooks": {},
    }

    with open(os.path.join(out_dir, "human.json"), "w") as f:
        json.dump(sidecar, f, indent=2)
        f.write("\n")

    print(f"wrote human.vox ({len(ORDER)} parts x2 models, {painted} painted "
          f"micro voxels, {WORLD_H} world voxels tall, one material "
          f"({FLESH_ID}) and {len(ART_RGB)} art colours) and human.json "
          f"({len(clips)} clips, {len(states)} states)")
    print(f"  leg {leg_len:.2f} world voxels -> walk {walk_ms} ms / run "
          f"{run_ms} ms arm cycle, rideHeight {ride_height}")


if __name__ == "__main__":
    main()
