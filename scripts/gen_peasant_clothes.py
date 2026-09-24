#!/usr/bin/env python3
"""Generate the COMMONER WARDROBE — nine plain garments in linen, leather and
wood, three of each kind, every one of them authored to be DYED.

    shirts  tunic     smock      jerkin      (armor_chest)
    legs    trousers  breeches   hose        (armor_legs)
    feet    shoes     clogs      footwraps   (armor_boots)

WHY A SECOND GENERATOR AND NOT A ROW IN gen_stock_armor.py. That file makes the
wizard's kit and the plate set: specific, named, single-instance costumes whose
colour IS part of what they are (a black robe, a gold sash). These are the
opposite kind of content — the clothes of nobody in particular, meant to be
stamped out by the hundred with a different colour on each villager. The two
have different rules about colour, and the rule is the reason for the split:

  * gen_stock_armor.py paints in FIXED art slots. Black cloth is black.
  * THIS FILE PAINTS IN A GREYSCALE RAMP AND NOTHING ELSE, and the colour is
    supplied at RUNTIME per worn instance (game/dye.h, the `dye` word on
    MicroBodyInstGpu). A garment here is a PATTERN; a garment on a body is a
    pattern times a colour.

So the nine pieces below are nine silhouettes and nine weaves, and the variety
the player sees is nine times however many colours anybody picks. That is the
whole point: a village of forty needs forty outfits and one of them is worth
authoring.

THE RAMP IS A MULTIPLIER, NOT A COLOUR. Every art slot this file uses is a grey
whose value says "this cell is <that> times the dye". `kDyeRefGrey` (0.70) is the
one that comes out EXACTLY the picked colour; everything else is a weave tone
around it, so the fabric keeps its noise, its seams and its hems whatever colour
it is dyed. Undyed — which is what a piece dropped in the world or read out of
an old save looks like — it renders as the neutral grey linen it literally is,
so a missing dye is legible rather than invisible.

GEOMETRY IS DERIVED, exactly as gen_stock_armor.py's is, and for the same three
reasons stated at the top of that file: the shell is strictly outside the body,
it cannot go stale because the limb boxes are IMPORTED, and `offset`/`fitBox`
are measured rather than typed. The shape builders and the .vox writer are
imported from there rather than restated — a second copy of `tube()` is a
second thing to keep true when the human is re-proportioned.

THE TWO SEAMS THESE MUST KEEP, both inherited from the stock set so the two
wardrobes interoperate piece by piece (peasant trousers over stock boots, stock
pants over clogs):

  * a foot piece owns the shin up to `foot z1 + 2`,
  * a leg piece starts at `foot z1 + 3`.

Run:  python scripts/gen_peasant_clothes.py
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import human_art as H
# The shape builders, the .vox writer and the two measured numbers. IMPORTED,
# never restated — see the note above.
import gen_stock_armor as A

# ---- materials --------------------------------------------------------------
# PALETTE CONVENTION (as everywhere): .vox palette index i+1 == materials.json[i].
# Asserted in main() rather than trusted.
#
# LINEN, NOT `cloth`. `cloth` is the wizard's heavy robe-stuff and carries its
# own burn chain (cloth_burning/cloth_charred); linen has its own, and a
# peasant's shirt going up differently from a mage's robe is the sort of
# difference that is free here because it is already a material. Clogs are WOOD
# for the same reason: a wooden shoe burns and a leather one chars.
LINEN_MAT, LINEN_ID = 111, "linen"
LEATHER_MAT, LEATHER_ID = 53, "leather"
WOOD_MAT, WOOD_ID = 2, "wood"

# ---- the dye ramp -----------------------------------------------------------
# Allocated DOWNWARD from 245, below gen_stock_armor's 246..255 block, so the
# two generators can run in either order without either one's slots meaning
# something else. FILE-LOCAL like every .vox palette (memory: art palette
# indices are file-local) — what makes these mean the same thing across all nine
# pieces is that they are the same RGB, and MicroBodyMergeArt dedupes by RGB.
#
# THE NUMBERS ARE THE MULTIPLIER. Slot value v renders as `dye * (v/255) / 0.70`
# — see DYE_REF_GREY below and DYE_REF in assets/shaders/microbody.wgsl,
# which is the one place the 0.70 is stated to the GPU.
BASE = 245        # 1.00x — the dye, exactly as picked
SHADE = 244       # 0.78x — the ordinary weave shadow
DARK = 243        # 0.62x — the deep threads
LIGHT = 242       # 1.12x — where the weave catches the light
SEAM = 241        # 0.41x — hems, seams, laces, soles: the piece's own drawing
TRIM = 240        # 1.25x — eyelets, buttons, a bleached patch

DYE_REF_GREY = 179    # == BASE's grey. MUST equal round(255 * DYE_REF).

RAMP_RGB = {
    BASE:  0xB3B3B3,   # 179
    SHADE: 0x8C8C8C,   # 140
    DARK:  0x6E6E6E,   # 110
    LIGHT: 0xC8C8C8,   # 200
    SEAM:  0x4A4A4A,   # 74
    TRIM:  0xE0E0E0,   # 224
}

# A DELIBERATE MUTATION of the imported module's palette table, and the reason
# it is done rather than copying `rgba_chunk`/`write_piece` down here: those
# two functions read ART_RGB out of their own module globals, so the choice is
# between publishing the ramp into that table or maintaining a second .vox
# writer. A second writer is the worse of the two — it is the part most likely
# to drift (the entry-i-is-index-i+1 off-by-one is documented there precisely
# because it looks almost right when wrong). The slots do not collide, so
# gen_stock_armor's own output is unaffected by this even if both run.
A.ART_RGB.update(RAMP_RGB)

# One step darker, for the away-facing half. A table rather than arithmetic
# because the ramp is not evenly spaced and SEAM/TRIM are drawing, not weave:
# darkening a lace line would make it read as shadow.
DARKER = {LIGHT: BASE, BASE: SHADE, SHADE: DARK, DARK: DARK,
          SEAM: SEAM, TRIM: LIGHT}

UPSCALE = A.UPSCALE


# ---- deterministic weave ----------------------------------------------------

def h3(x, y, z, salt=0):
    """A counter-based hash over a cell, so the weave is a FUNCTION of position.

    Not `random`: two runs of this generator must produce byte-identical .vox
    files, and a seeded PRNG threaded through the builders in call order would
    make the noise depend on which piece was generated first. Same shape as the
    engine's own rng::Hash3 and for the same reason (CLAUDE.md rule 1), even
    though nothing here is hashed — a generator whose output moves when you
    reorder its table is a generator whose output you cannot review."""
    v = ((x & 0xFFFF) * 73856093) ^ ((y & 0xFFFF) * 19349663) ^ \
        ((z & 0xFFFF) * 83492791) ^ ((salt & 0xFFFF) * 2654435761)
    v &= 0xFFFFFFFF
    v ^= v >> 15
    v = (v * 2246822519) & 0xFFFFFFFF
    v ^= v >> 13
    v = (v * 3266489917) & 0xFFFFFFFF
    v ^= v >> 16
    return v


def weave(cells, salt, coarse=1, light=12, shade=20, dark=8):
    """The base paint for a panel: mostly BASE with a scatter of the other
    three weave tones, plus the away-facing half dropped one step.

    `coarse` groups cells into blocks before hashing, which is how a garment
    gets a visible slub instead of per-voxel television snow. At the shipped
    resolution (8 skin voxels per world voxel, of which this authors at 4 and
    the writer doubles) coarse=1 is already a 2x2x2 block on screen; coarse=2 is
    a run of thread.

    The front/back split is gen_stock_armor's `shade()` rule, kept because a
    garment without it reads as a decal at grazing angles — the renderer's own
    shading is doing the real work, this is what keeps the ALBEDO from being
    flat."""
    if not cells:
        return {}
    ys = [c[1] for c in cells]
    mid = (min(ys) + max(ys)) * 0.5
    out = {}
    for c in cells:
        x, y, z = c
        r = h3(x // coarse, y // coarse, z // coarse, salt) % 100
        if r < light:
            t = LIGHT
        elif r < light + shade:
            t = SHADE
        elif r < light + shade + dark:
            t = DARK
        else:
            t = BASE
        out[c] = DARKER[t] if y > mid else t
    return out


def band(paint, cells, zlo, zhi, tone=SEAM):
    """Paint every cell in a z band — a hem, a waistband, a cuff, a garter."""
    for c in cells:
        if zlo <= c[2] <= zhi:
            paint[c] = tone


def front_line(paint, cells, tone=SEAM, width=0, every=1, phase=0):
    """Paint a vertical stripe down the FRONT face (low y) of a panel.

    The front is low y because the shape builders flip front-to-back — the same
    reading of the figure gen_stock_armor's `body_cells` documents, and getting
    it backwards puts the lacing down the wearer's spine."""
    if not cells:
        return
    y0 = min(c[1] for c in cells)
    xs = [c[0] for c in cells]
    cx = (min(xs) + max(xs)) * 0.5
    for c in cells:
        x, y, z = c
        if y > y0:
            continue
        if abs(x + 0.5 - cx) > width + 0.5:
            continue
        if (z - phase) % every != 0:
            continue
        paint[c] = tone


def side_seam(paint, cells, tone=SEAM):
    """Paint the two outermost x columns of a panel — the seam a trouser leg
    or a sleeve is sewn up. Cheap, and it is most of what makes a plain tube
    read as a made thing rather than as a extruded box."""
    if not cells:
        return
    xs = [c[0] for c in cells]
    x0, x1 = min(xs), max(xs)
    for c in cells:
        if c[0] == x0 or c[0] == x1:
            paint[c] = tone


def patch(paint, cells, salt, tone=TRIM, frac=6):
    """A mended square: one contiguous run of cells around a hashed centre.

    `frac` is roughly one in how many cells of the panel end up in it. A patch
    is the single clearest signal that a garment is SOMEBODY'S rather than
    stock, and it costs one loop."""
    if not cells:
        return
    cl = sorted(cells)
    c0 = cl[h3(salt, len(cl), 7, salt) % len(cl)]
    r = max(1, int(round((len(cl) / float(frac)) ** (1.0 / 3.0))))
    for c in cells:
        if abs(c[0] - c0[0]) <= r and abs(c[2] - c0[2]) <= r and \
                abs(c[1] - c0[1]) <= r + 1:
            # HASHED, not `(x + z) % 2`. A parity rule is a perfect
            # checkerboard, which at this resolution reads as a texture the
            # engine applied rather than as somebody's stitching — the one
            # place in this file where a regular pattern is worse than noise.
            paint[c] = tone if h3(c[0], c[1], c[2], salt + 3) % 3 else SEAM


# ---- shared shell builders --------------------------------------------------
#
# Each returns shells BY PART, the same shape build_robe does, because each part
# becomes its own rig slot on the wearer.

def chest_core(per_limb, occ, collar=True, yoke_arms=True, yoke_grow=1):
    """The torso panel every shirt shares: a tube up the trunk, a yoke across
    the shoulders and (optionally) a collar around the neck rows.

    Lifted straight from build_robe's torso, including the reason the yoke is
    the torso's own top row AND both upper arms' top rows grown by one: capping
    only the torso leaves the top of each arm bare, which reads as a shirt with
    the shoulders cut out. `yoke_arms` is False only for the sleeveless jerkin,
    which WANTS a bare shoulder cap — but takes the armhole edge instead (see
    build_jerkin)."""
    torso = per_limb["torso"]
    tz1 = max(c[2] for c in torso)
    body = A.tube(torso, occ, 1)
    body |= A.cap(torso, occ, tz1, range(tz1 + 1, tz1 + 2), grow=1)
    if yoke_arms:
        # `yoke_grow` IS THE SHOULDER LINE, and it is the one number here worth
        # a paragraph. The robe grows the arm caps by one so the yoke hangs a
        # micro past the outside of each sleeve, which on a FULL sleeve reads as
        # a shoulder seam. On a SHORT sleeve it reads as a pad: the lip is a
        # flat horizontal shelf catching full light, with nothing below it long
        # enough to make the overhang look like cloth falling. Measured on the
        # first tunic render, which grew a set of square epaulettes. A
        # short-sleeved piece passes 0 and takes a one-micro ring open at the
        # very top of the sleeve, visible only from directly overhead.
        for arm in ("armU.L", "armU.R"):
            cells = per_limb[arm]
            az1 = max(c[2] for c in cells)
            body |= A.cap(cells, occ, az1, range(az1 + 1, az1 + 2),
                          grow=yoke_grow)
    if collar:
        head = per_limb["head"]
        hz0 = min(c[2] for c in head)
        neck = {c for c in head if c[2] < hz0 + A.neck_rows(head)}
        body |= A.tube(neck, occ, 1, zlo=tz1 + 1)
    return body


def skirt(per_limb, occ, rows, flare_every):
    """A hem hanging off the hips — build_robe's skirt, parametrised. `rows` 0
    gives a bare tube, which is what a jerkin that stops at the waist wants."""
    hips = per_limb["hips"]
    hz0 = min(c[2] for c in hips)
    out = A.tube(hips, occ, 1)
    if rows <= 0:
        return out
    hem = {(x, y) for (x, y, z) in hips if z == hz0}
    for i, z in enumerate(range(hz0 - 1, hz0 - 1 - rows, -1)):
        r = 1 + (i // flare_every if flare_every else 0)
        row = set()
        for (x, y) in hem:
            for dx in range(-r, r + 1):
                for dy in range(-r, r + 1):
                    if max(abs(dx), abs(dy)) != r:
                        continue
                    row.add((x + dx, y + dy, z))
        out |= {c for c in row if c not in occ}
    return out


def foot_z1(per_limb, side):
    return max(c[2] for c in per_limb["foot." + side])


def shin_from(per_limb, side):
    """The lowest z a LEG piece may occupy on this side.

    THE SEAM, stated once. A foot piece owns the shin through `foot z1 + 2`
    (build_boots rings exactly that), so a leg piece starts at +3. Both numbers
    come off the same foot box, so re-proportioning the leg keeps the seam —
    which is the whole reason this is a function and not a 3 in two files."""
    return foot_z1(per_limb, side) + 3


def leg_tubes(per_limb, occ, thigh_only=False, shin_frac=1.0):
    """Trouser legs: a tube around each thigh, and as much of each shin as the
    cut calls for.

    `shin_frac` 1.0 is full length. Anything less takes the TOP of the shin and
    leaves the calf and ankle bare, which is what a short leg-piece IS — the
    first version took the BOTTOM fraction and produced breeches that were a
    cuff around each ankle with the whole calf showing above them, i.e. exactly
    the inverse garment. The distinction is invisible in a voxel count and
    obvious in one screenshot."""
    out = {}
    for side in ("L", "R"):
        out["legU." + side] = A.tube(per_limb["legU." + side], occ, 1)
    if thigh_only:
        return out
    for side in ("L", "R"):
        shin = per_limb["legL." + side]
        z0 = min(c[2] for c in shin)
        z1 = max(c[2] for c in shin)
        lo = max(z0, shin_from(per_limb, side))
        if shin_frac < 1.0:
            lo = z1 - int(round((z1 - lo) * shin_frac))
        if z1 < lo:
            continue
        out["legL." + side] = A.tube(shin, occ, 1, zlo=lo, zhi=z1)
    return out


def foot_shell(per_limb, occ, side, thick_rows=0, cuff=True):
    """A closed skin over the foot plus the ankle cuff that seals the seam.

    `thick_rows` grows the bottom N rows outward in xy — a wooden sole. The
    dilation is what closes the instep (build_boots' note: a flat cap alone
    leaves the hole the shin passes through), and the cuff is what closes the
    ring where the leg leaves the shoe."""
    foot = per_limb["foot." + side]
    z1 = max(c[2] for c in foot)
    z0 = min(c[2] for c in foot)
    shell = A.dilate26(foot, occ)
    if thick_rows > 0:
        # RINGED OFF THE SOLE, NOT OFF THE WHOLE SHELL. Dilating `shell` grows
        # the ankle cuff with it, which is a flared boot top rather than a
        # block sole — and it quadrupled the piece's voxel count for a shape
        # nobody asked for. The band is clamped back to the sole's own z rows
        # for the same reason.
        sole = {c for c in foot if c[2] < z0 + thick_rows}
        block = set()
        for k in (1, 2):
            block |= A.ring_xy(sole, occ, k)
        shell |= {c for c in block if z0 <= c[2] < z0 + thick_rows}
    if cuff:
        shell |= A.tube(per_limb["legL." + side], occ, 1, zlo=z1 + 1, zhi=z1 + 2)
    return {c for c in shell if c[2] >= 0}


# ---- the nine pieces --------------------------------------------------------
#
# Each builder returns (shells_by_part, paint_by_cell). The paint is built
# alongside the geometry rather than derived from it afterwards, because the
# structural tones — a hem, a lacing line, the sole of a shoe — are facts about
# HOW the panel was built and are not recoverable from the finished cell set.

def build_tunic(per_limb, occ):
    """Short sleeves, a body to mid-thigh, a slit at the throat.

    The commonest thing anybody wears: one piece pulled over the head. The
    sleeve stops at the elbow, which is what distinguishes it at a glance from
    the smock, and the skirt falls straight rather than flaring."""
    shells = {}
    paint = {}
    body = chest_core(per_limb, occ, yoke_grow=0)
    shells["torso"] = body
    paint.update(weave(body, salt=11, coarse=1, light=10, shade=17, dark=5))
    tz0 = min(c[2] for c in per_limb["torso"])
    band(paint, body, tz0, tz0 + 1)                    # the waist seam
    front_line(paint, body, SEAM, width=0, every=1,
               phase=0)                                # the throat slit
    # ... but only the top third of it: a tunic laces at the neck, not to the
    # navel. Repaint the rest of the front column back into the weave.
    tz1 = max(c[2] for c in body)
    for c in list(paint):
        if paint[c] == SEAM and c[2] < tz1 - 7 and c[2] > tz0 + 1:
            paint[c] = BASE

    hem = skirt(per_limb, occ, rows=3, flare_every=0)
    shells["hips"] = hem
    paint.update(weave(hem, salt=12, coarse=1, light=10, shade=17, dark=5))
    hz = min(c[2] for c in hem)
    band(paint, hem, hz, hz + 1)                       # the hem band

    for side in ("L", "R"):
        arm = per_limb["armU." + side]
        az0, az1 = min(c[2] for c in arm), max(c[2] for c in arm)
        cut = az0 + (az1 - az0) // 3                   # stops above the elbow
        sleeve = A.tube(arm, occ, 1, zlo=cut)
        shells["armU." + side] = sleeve
        paint.update(weave(sleeve, salt=13, coarse=1, light=10, shade=17,
                           dark=5))
        band(paint, sleeve, cut, cut + 1)              # the sleeve hem
    return shells, paint


def build_smock(per_limb, occ):
    """Long sleeves to the wrist, a long loose body, cuffs and a chest seam.

    A working garment: more cloth than the tunic everywhere, which is why its
    skirt flares (`flare_every=3`) and its sleeves run the full arm. It is the
    one piece here that covers as much as the wizard's robe, and it is cut from
    the same builders — the difference is the paint and the hem."""
    shells = {}
    paint = {}
    # yoke_grow 0 for the reason chest_core's note gives: the one-micro lip is
    # a flat horizontal shelf, and at this resolution it photographs as an
    # epaulette rather than as a shoulder seam.
    body = chest_core(per_limb, occ, yoke_grow=0)
    shells["torso"] = body
    paint.update(weave(body, salt=21, coarse=1, light=16, shade=22, dark=6))
    tz0 = min(c[2] for c in per_limb["torso"])
    tz1 = max(c[2] for c in body)
    # ONE ROW, in the weave's own dark rather than in SEAM. Two rows of SEAM
    # across the chest is not a yoke seam, it is a bib: SEAM is 0.41 of the
    # dye and the panel around it is 1.0, so a two-row band at that contrast
    # reads as a separate garment.
    band(paint, body, tz1 - 4, tz1 - 4, tone=DARK)     # the yoke seam
    band(paint, body, tz0, tz0)

    # A STRAIGHT FALL, not a flare. The robe flares every two rows and it reads
    # as a robe; the same step on a five-row hem reads as a shelf sticking out
    # at hip height with a hard dark edge under it, because the step is a whole
    # authored micro on a panel only a few micro tall. A working smock hangs.
    hem = skirt(per_limb, occ, rows=6, flare_every=0)
    shells["hips"] = hem
    paint.update(weave(hem, salt=22, coarse=1, light=16, shade=22, dark=6))
    hz = min(c[2] for c in hem)
    band(paint, hem, hz, hz)
    patch(paint, hem, salt=29, frac=14)                # a mend at the hem

    for side in ("L", "R"):
        for seg in ("armU.", "armL."):
            arm = per_limb[seg + side]
            sleeve = A.tube(arm, occ, 1)
            shells[seg + side] = sleeve
            paint.update(weave(sleeve, salt=23, coarse=1,
                               light=16, shade=22, dark=6))
        low = shells["armL." + side]
        lz0 = min(c[2] for c in low)
        band(paint, low, lz0, lz0 + 1)                 # the cuff
    return shells, paint


def build_jerkin(per_limb, occ):
    """Sleeveless, laced up the front, stopping just below the waist.

    THE ARMHOLE IS WHY THE YOKE IS OFF. chest_core's yoke normally caps both
    upper arms so a sleeve has a shoulder; a jerkin has no sleeve, so capping
    the arm would put a disc of leather floating on top of a bare shoulder.
    Without it the panel stops at the trunk and the shoulder is bare, which is
    what sleeveless means — and the topmost ring of the panel is painted SEAM
    so the armhole reads as a bound edge rather than as a cut."""
    shells = {}
    paint = {}
    body = chest_core(per_limb, occ, collar=False, yoke_arms=False)
    shells["torso"] = body
    paint.update(weave(body, salt=31, coarse=2, light=8, shade=26, dark=14))
    # AND IT COVERS THE WAIST. The torso limb stops at the hips, so a piece
    # with no hips shell leaves a bare midriff between itself and whatever is
    # on the legs — which is what the first jerkin render showed, and is not
    # what "cut off at the waist" means. A hips tube with no skirt (rows=0) is
    # the shortest thing that closes it.
    waist = skirt(per_limb, occ, rows=0, flare_every=0)
    shells["hips"] = waist
    paint.update(weave(waist, salt=32, coarse=2, light=8, shade=26, dark=14))
    wz = min(c[2] for c in waist)
    band(paint, waist, wz, wz)                         # the bound bottom edge
    tz1 = max(c[2] for c in body)
    tz0 = min(c[2] for c in per_limb["torso"])
    band(paint, body, tz1 - 1, tz1)                    # the bound armhole edge
    band(paint, body, tz0, tz0)                        # the waist edge
    # THE LACING: a SEAM column down the centre front with TRIM eyelets every
    # third row. Two calls, the second overwriting the first on its own rows,
    # which is cheaper and clearer than one pass with two conditions.
    front_line(paint, body, SEAM, width=0, every=1)
    front_line(paint, body, TRIM, width=1, every=3, phase=1)
    patch(paint, body, salt=39, frac=20)
    return shells, paint


def build_trousers(per_limb, occ):
    """Full-length, straight, a waistband and a seam down the outside of each
    leg. The default thing to put on a pair of legs."""
    shells = leg_tubes(per_limb, occ)
    paint = {}
    for part, cells in shells.items():
        paint.update(weave(cells, salt=41, coarse=2))
        side_seam(paint, cells)
    for side in ("L", "R"):
        up = shells["legU." + side]
        z1 = max(c[2] for c in up)
        band(paint, up, z1 - 1, z1)                    # the waistband
        lo = shells.get("legL." + side)
        if lo:
            z0 = min(c[2] for c in lo)
            band(paint, lo, z0, z0)                    # the ankle hem
    return shells, paint


def build_breeches(per_limb, occ):
    """Knee breeches: the thigh and the top third of the shin, banded at the
    knee. The calf below is bare, which is the point — they are worn with
    footwraps or with tall boots and they look like neither trousers."""
    shells = leg_tubes(per_limb, occ, shin_frac=0.35)
    paint = {}
    for part, cells in shells.items():
        paint.update(weave(cells, salt=51, coarse=1, light=14, shade=18, dark=10))
    for side in ("L", "R"):
        up = shells["legU." + side]
        z1 = max(c[2] for c in up)
        band(paint, up, z1 - 1, z1)                    # waistband
        lo = shells.get("legL." + side)
        if lo:
            z1l = max(c[2] for c in lo)
            z0l = min(c[2] for c in lo)
            band(paint, lo, z1l - 1, z1l)              # the seam at the knee
            band(paint, lo, z0l, z0l + 1, tone=TRIM)   # the tie that holds
                                                       # them below the calf
    return shells, paint


def build_hose(per_limb, occ):
    """Close-fitting full-length hose, gartered at the top, darned at one knee.

    Geometrically it is the trousers — at one authored micro there is no
    thinner a garment can be and still exist — so what separates them is the
    DRAWING: no outside seam, a garter band high on each thigh, and a darn.
    That is a real limit of a one-micro shell and it is the honest way to spend
    it, since the alternative (a shell at radius 0, i.e. replacing skin) would
    make the wearer's leg vanish when the hose burned."""
    shells = leg_tubes(per_limb, occ)
    paint = {}
    for part, cells in shells.items():
        paint.update(weave(cells, salt=61, coarse=1, light=18, shade=24, dark=4))
    for side in ("L", "R"):
        up = shells["legU." + side]
        z1 = max(c[2] for c in up)
        z0 = min(c[2] for c in up)
        band(paint, up, z1 - 2, z1 - 1, tone=TRIM)     # the garter
        band(paint, up, z0, z0)                        # the knee seam
    patch(paint, shells["legL.L"], salt=69, frac=8)    # a darn, one calf
    return shells, paint


def build_shoes(per_limb, occ):
    """Low leather shoes: a sole, a vamp, a lace across the instep."""
    shells = {}
    paint = {}
    for side in ("L", "R"):
        shell = foot_shell(per_limb, occ, side)
        shells["foot." + side] = shell
        paint.update(weave(shell, salt=71, coarse=2, light=8, shade=24, dark=12))
        z0 = min(c[2] for c in shell)
        band(paint, shell, z0, z0 + 1)                 # the sole
        fz1 = foot_z1(per_limb, side)
        band(paint, shell, fz1 + 1, fz1 + 2, tone=SEAM)  # the topline binding
        # THE LACE. Across the instep — the top face of the foot box, in the
        # front half — every other row, so it reads as crossed cord.
        foot = per_limb["foot." + side]
        ys = [c[1] for c in foot]
        ymid = (min(ys) + max(ys)) * 0.5
        for c in shell:
            if c[2] == fz1 + 1 or c[2] == fz1:
                continue
            if c[2] >= fz1 - 1 and c[1] < ymid and (c[0] + c[1]) % 3 == 0:
                paint[c] = TRIM
    return shells, paint


def build_clogs(per_limb, occ):
    """Wooden clogs: a thick block sole, a plain upper, a rounded toe.

    `thick_rows=2` is the whole silhouette difference — the bottom two rows of
    the foot grow outward to radius 2 instead of 1, which is a sole you can see
    from across a room. Being WOOD rather than leather is not decoration: a
    material is a mechanic here (gen_stock_armor's note), and a clog burns."""
    shells = {}
    paint = {}
    for side in ("L", "R"):
        shell = foot_shell(per_limb, occ, side, thick_rows=2)
        shells["foot." + side] = shell
        paint.update(weave(shell, salt=81, coarse=3, light=10, shade=20, dark=10))
        z0 = min(c[2] for c in shell)
        band(paint, shell, z0, z0 + 2)                 # the block sole
        fz1 = foot_z1(per_limb, side)
        band(paint, shell, fz1 + 1, fz1 + 2, tone=DARK)
        # The grain: one in three columns a shade down the length of the shoe.
        for c in shell:
            if c[2] > z0 + 2 and c[0] % 3 == 0:
                paint[c] = DARKER.get(paint.get(c, BASE), SHADE)
    return shells, paint


def build_footwraps(per_limb, occ):
    """Cloth wound round the foot and ankle, crossed and tucked.

    Same shell as the shoes — there is one micro of clearance around a foot and
    every foot piece has to live in it — with a FOLD: a second band at radius 2
    around the midsection, which is the one place a wrap can legibly sit on top
    of itself. The crossing is paint, on a diagonal so it reads as winding
    rather than as stripes."""
    shells = {}
    paint = {}
    for side in ("L", "R"):
        shell = foot_shell(per_limb, occ, side)
        foot = per_limb["foot." + side]
        z0 = min(c[2] for c in foot)
        z1 = max(c[2] for c in foot)
        mid = {c for c in foot if z0 + 1 <= c[2] <= z1 - 1}
        fold = A.ring_xy(mid | shell, occ | shell, 1)
        fold = {c for c in fold if z0 + 1 <= c[2] <= z1 - 1}
        shell |= fold
        shells["foot." + side] = shell
        paint.update(weave(shell, salt=91, coarse=1, light=16, shade=22, dark=8))
        for c in fold:
            paint[c] = LIGHT
        # The winding: a diagonal every third cell, all the way up.
        for c in shell:
            if (c[0] + c[1] + c[2]) % 3 == 0:
                paint[c] = SEAM
        band(paint, shell, z0, z0, tone=DARK)          # the dirty sole
        fz1 = foot_z1(per_limb, side)
        band(paint, shell, fz1 + 2, fz1 + 2, tone=TRIM)  # the tucked end
    return shells, paint


# ---- the table --------------------------------------------------------------

PIECES = [
    ("tunic",     "armor_chest", LINEN_MAT,   14.0, build_tunic),
    ("smock",     "armor_chest", LINEN_MAT,   16.0, build_smock),
    ("jerkin",    "armor_chest", LEATHER_MAT, 22.0, build_jerkin),
    ("trousers",  "armor_legs",  LINEN_MAT,   12.0, build_trousers),
    ("breeches",  "armor_legs",  LINEN_MAT,   11.0, build_breeches),
    ("hose",      "armor_legs",  LINEN_MAT,    9.0, build_hose),
    ("shoes",     "armor_boots", LEATHER_MAT, 12.0, build_shoes),
    ("clogs",     "armor_boots", WOOD_MAT,    16.0, build_clogs),
    ("footwraps", "armor_boots", LINEN_MAT,    7.0, build_footwraps),
]

ITEM_DESC = {
    "tunic": "A pull-over linen tunic, sleeves to the elbow and a body to "
             "mid-thigh, laced at the throat. What most people are wearing "
             "most of the time. It takes a dye evenly and it stops nothing.",
    "smock": "A long working smock, full sleeves with cuffs and cloth enough "
             "in the skirt to move in. Mended once at the hem already; it "
             "will be mended again.",
    "jerkin": "A sleeveless leather jerkin laced up the front, cut off at the "
              "waist. Bare shoulders, a bound armhole, and rather more "
              "between a blade and your ribs than linen manages.",
    "trousers": "Straight linen trousers, seamed down the outside of each leg "
                "and hemmed where a shoe begins.",
    "breeches": "Knee breeches, tied below the knee and leaving the calf to "
                "the weather. Worn with wraps, or with boots, or with "
                "nothing.",
    "hose": "Close linen hose, gartered at the thigh and darned at one knee. "
            "Nothing about them is in the way, which is the only thing they "
            "have to recommend them.",
    "shoes": "Low leather shoes with a stitched sole and a lace across the "
             "instep. Better than bare feet on a road and worse than boots "
             "anywhere else.",
    "clogs": "Wooden clogs on a block sole, an inch of hardwood between you "
             "and the mud. They do not soak, they do not rot, and they burn "
             "like the log they were.",
    "footwraps": "Strips of linen wound round the foot and crossed at the "
                 "ankle. The cheapest thing that is not going barefoot, and "
                 "the first thing to catch.",
}


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "assets", "items")
    os.makedirs(out_dir, exist_ok=True)

    with open(os.path.join(root, "assets", "materials", "materials.json")) as mf:
        mats = json.load(mf)["materials"]
    for idx, want in ((LINEN_MAT, LINEN_ID), (LEATHER_MAT, LEATHER_ID),
                      (WOOD_MAT, WOOD_ID)):
        assert idx <= len(mats), (
            f"materials.json has {len(mats)} rows, none at index {idx} for "
            f"{want!r} -- append the material before generating the set")
        assert mats[idx - 1]["id"] == want, (
            f"materials.json[{idx - 1}] is {mats[idx - 1]['id']!r}, not "
            f"{want!r} — the palette index this set is authored against has "
            f"moved, and every piece would load as another material")
    mat_rgb = {i + 1: int(m["colors"][0].lstrip("#"), 16)
               for i, m in enumerate(mats) if m.get("colors")}

    # The one number the GPU also has to know, checked here rather than
    # commented: DYE_REF in microbody.wgsl must be BASE's grey over 255, or
    # every dyed garment comes out uniformly too bright or too dark and the
    # cause is two files apart.
    assert RAMP_RGB[BASE] & 0xFF == DYE_REF_GREY
    assert (RAMP_RGB[BASE] >> 8) & 0xFF == DYE_REF_GREY
    assert (RAMP_RGB[BASE] >> 16) & 0xFF == DYE_REF_GREY

    per_limb, occ = A.body_cells()

    rows = []
    for name, kind, mat, hp, build in PIECES:
        shells, paint = build(per_limb, occ)
        models = {}
        cover = []
        for part, cells in sorted(shells.items()):
            assert cells, f"{name}: shell over {part} came out empty"
            # A SHELL MAY NOT TOUCH THE BODY IT COVERS — gen_stock_armor's
            # assertion, kept because every builder here subtracts `occ` and a
            # violation means one of them reached for the wrong cell set. The
            # symptom is cloth rendering inside flesh, which reads as
            # z-fighting rather than as the geometry error it is.
            assert not (cells & occ), f"{name}/{part} intersects the body"

            up = set()
            cols = {}
            for (x, y, z) in cells:
                c = paint.get((x, y, z), BASE)
                for dz in range(UPSCALE):
                    for dy in range(UPSCALE):
                        for dx in range(UPSCALE):
                            q = (x * UPSCALE + dx, y * UPSCALE + dy,
                                 z * UPSCALE + dz)
                            up.add(q)
                            cols[q] = c
            mn = (min(c[0] for c in up), min(c[1] for c in up),
                  min(c[2] for c in up))
            size = (max(c[0] for c in up) - mn[0] + 1,
                    max(c[1] for c in up) - mn[1] + 1,
                    max(c[2] for c in up) - mn[2] + 1)
            models[part] = (sorted(up), cols, size, mn)

            # The two MEASURED numbers — offset is the shell's engine min
            # corner minus the covered limb's, fitBox is the box it was cut
            # for. Identical to gen_stock_armor's, deliberately: these are the
            # two values a hand-typed guess in puts the garment off the body.
            limb_up = A.upscale_set(per_limb[part])
            sm, lm = A.engine_min(up), A.engine_min(limb_up)
            cover.append({
                "part": part,
                "model": part,
                "offset": [sm[0] - lm[0], sm[1] - lm[1], sm[2] - lm[2]],
                "fitBox": list(A.engine_size(limb_up)),
                "hp": hp,
            })

        A.write_piece(os.path.join(out_dir, name + ".vox"), models, mat, mat_rgb)
        sidecar = {
            "comment": (
                f"The {name}, a DYEABLE commoner garment: one shell per covered "
                f"limb, each of which becomes a real rig slot on the wearer and "
                f"therefore burns, dissolves, carves, severs and drops exactly "
                f"as a limb does. The art is a GREYSCALE WEAVE — every cell is "
                f"a multiplier on the wearer's dye (game/dye.h), not a colour — "
                f"so one pattern covers every colour anybody picks. Geometry is "
                f"the stock human's own silhouette dilated outward by one "
                f"authored micro. Regenerate with "
                f"scripts/gen_peasant_clothes.py; `offset` and `fitBox` are "
                f"MEASURED off gen_human.py's limb table, so editing them by "
                f"hand puts the garment off the body."),
            "name": name,
            "artVoxelsPerMetre": H.ART_VOXELS_PER_METRE,
            "hp": hp * len(cover),
            "severable": True,
            # THE FLAG THE ENGINE READS. Everything else about a dyeable piece
            # is a convention about its art; this is the one bit the runtime
            # needs, and it is what lets the wardrobe panel offer a colour for
            # a tunic and not for an iron helm.
            "dyeable": True,
            "cover": sorted(cover, key=lambda c: c["part"]),
        }
        with open(os.path.join(out_dir, name + ".json"), "w") as f:
            json.dump(sidecar, f, indent=2)
            f.write("\n")

        vox = sum(len(m[0]) for m in models.values())
        rows.append({"id": name, "kind": kind, "desc": ITEM_DESC[name]})
        print(f"  {name:10s} {len(cover)} shells, {vox:6d} voxels, "
              f"{', '.join(c['part'] for c in sidecar['cover'])}")

    # items.json is MERGED, not overwritten — the swords and the wizard's kit
    # are other generators' rows and hand-authored content besides. Rows this
    # file owns are replaced in place, so a re-run is idempotent.
    items_path = os.path.join(out_dir, "items.json")
    with open(items_path) as f:
        doc = json.load(f)
    mine = {r["id"] for r in rows}
    doc["items"] = [r for r in doc["items"] if r["id"] not in mine] + rows
    with open(items_path, "w") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")
    print(f"wrote {len(rows)} dyeable pieces to assets/items/ and merged their "
          f"rows into items.json ({len(doc['items'])} items total)")


if __name__ == "__main__":
    main()
