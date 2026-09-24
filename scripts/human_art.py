#!/usr/bin/env python3
"""The stock human's LIMB ART TABLE and shape builders — a library, not a generator.

WHAT THIS IS. The part of the retired `scripts/gen_human.py` that is still true:
the per-limb art boxes (ART_LIMBS), the shape builders that fill them (SHAPES),
the palette slots they paint, and the MagicaVoxel chunk writers. It writes no
file. `gen_stock_armor.py` and `gen_peasant_clothes.py` import it, because every
worn shell's `fitBox` is MEASURED off this table — re-proportion the body here
and the wardrobe follows on its next regeneration.

WHAT IT IS NOT. It is not the human. `assets/mobs/human.json` is the truth for
the rig contract (limbs, chains, sockets, clips, gait, states, anatomy) and
`assets/mobs/human.vox` for the body; both have been hand-extended far past
anything this file could make (anatomy baked into the interior, the undercloth
garment, dye slots in the art palette, the hand-rounded shoulders). The old
`gen_human.py` main() that wrote them was DELETED on 2026-09-24 (rule-
unification W1-E) because running it silently destroyed that work — 3,712
undercloth voxels on the 2026-08-30 measurement — and because
`scripts/test_mobgen.mjs` §A pinned `assets/editor/mobgen.js` against its
output, which froze every bred character to a rig four commits stale.
Generated characters are pinned against human.json now (test_mobgen.mjs §L/§M).

Measured when it was split out: 13 of the 15 limb boxes here are identical to
the cell to the shipped human.vox; the torso and upper arms differ only by the
hand-rounded shoulder (mobgen.js reproduces that as a rule). Both wardrobe
generators still reproduce their committed .vox geometry and paint exactly
against this table.

FACING AND HANDEDNESS. The loader maps scene -> engine as (x, z, -y), which
negates scene y and therefore FLIPS handedness. So the model is authored with
its face toward scene +Y and mirrored once by flip_y at the end of every
builder, and model +X is the character's LEFT — .L limbs take POSITIVE scene x.

CENTRING. Every box is centred so that its ellipse centre (mn.x + sx*0.5) lands
on x = 0, and left/right boxes are exact mirrors about it.
"""
import struct

# ---- material ---------------------------------------------------------------
# PALETTE CONVENTION (PLAN_voxel_art_and_mobs.md A1): .vox palette index i+1 ==
# materials.json[i]. FLESH 51 is materials.json[50] == "skin", the material the
# flesh reaction chain is authored against (skin -> flesh_cooked ->
# flesh_burning -> flesh_charred, assets/materials/reactions.json). NOT
# asserted here (the retired gen_human.py main() did): a consumer that writes
# FLESH into a file must check it against materials.json itself.
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
# ---- THE SIZE CONTRACT IS METRES (DESIGN.md 3b) -----------------------------
# HEIGHT_M is the physical fact; the lattice figures below are DERIVED from it
# and from the art's own resolution. WORLD_H used to be a literal 17 "world
# voxels head to toe", which fixed a real height only while kVoxelMeters was
# 0.10 -- so when the engine moved to 0.05 the figure stayed 17 cells and
# became 0.85 m: half the collision capsule it drives, with the art file
# perfectly correct and nothing able to notice.
#
# ART_VOXELS_PER_METRE is what the sidecar declares and is now the ONLY scale
# this generator needs. kVoxelMeters does not appear in the contract at all:
# tuning.json's player.halfHeight is already metres, so the cross-check below
# is metres against metres and this file no longer has to be regenerated when
# the engine's voxel size changes.
HEIGHT_M = 1.70                      # head to toe
EYE_M = 1.50                         # first-person camera row
ART_VOXELS_PER_METRE = 80            # shipped art resolution (the sidecar value)
AUTHORED_VOXELS_PER_METRE = ART_VOXELS_PER_METRE // SKIN_UPSCALE   # 40
# The scale the sidecar's WORLD-space rows (speed, severImpactSpeed, rideHeight,
# bodyYOffset, clip pos) are written at. The loader rescales them from here to
# the live kVoxelsPerMetre -- see mob.cpp's `sidecarVoxelsPerMetre`.
SIDECAR_VOXELS_PER_METRE = 10
WORLD_H = round(HEIGHT_M * AUTHORED_VOXELS_PER_METRE / ART_SCALE)
MICRO_H = round(HEIGHT_M * AUTHORED_VOXELS_PER_METRE)

# The eye line, in AUTHORED scene z. Player::kEyeOffset (0.65 m above the AABB
# centre, i.e. 1.50 m up at kVoxelMeters 0.10) puts the first-person camera at
# world voxel 15 = art micro 60. This is the one number in the table below that
# is not free, and head_vox() derives the eye row from it rather than restating
# it — see the note there about the trap this avoids.
EYE_Z = round(EYE_M * AUTHORED_VOXELS_PER_METRE)



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
# a writer (the retired gen_human.py main()) writes the same cells twice — once with FLESH into "<name>", once with
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

