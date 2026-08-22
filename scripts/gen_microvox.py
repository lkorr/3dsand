#!/usr/bin/env python3
"""Generate assets/microvox/*.vox — the static micro-detail example set.

A micro model is ONE world cell at subdiv^3 resolution: at kVoxelMeters =
0.0625 a cell is 6.25 cm, so subdiv 8 gives ~8 mm detail — fine enough for
individual grass blades and flower petals. Each .vox MODEL in the file is one
flipbook FRAME (docs/PLAN_voxel_editor.md §A); the frame durations live in the
"micro" block in materials.json, not here, because .vox cannot carry timing.

Coordinates below are MagicaVoxel scene space (Z-UP). The engine loader
converts once, chirality-preserving: engine = (x, z, -y), then rebases the
prefab min corner to 0 (voxload.h:16-20). So scene Z is engine Y = UP, which is
what the "height" axis means throughout this file.

IMPORTANT — the loader rebases the WHOLE PREFAB to its own min corner, so a
model that (correctly) does not fill its cell comes back as a box smaller than
subdiv^3. The micro loader accepts that and places a small model centred in X/Z
and floor-aligned in Y, so nothing here needs dummy corner voxels.

Crucially the rebasing is per-PREFAB, not per-model: every frame in one file
shares one bounding box and keeps its own `offset` inside it. So a flipbook
whose frames have different extents still animates in place — frame 2 does not
snap sideways because frame 1 happened to be narrower.

Palette index == material ID. The IDs below must match the append order in
assets/materials/materials.json — see MATERIALS at the top.
"""
import os
import struct
import sys

# ---- material IDs -----------------------------------------------------------
# Index in materials.json's "materials" array + 1 (slot 0 is air). These are the
# APPENDED micro materials plus the pre-existing ones they lean on. If
# materials.json is reordered these break — which is exactly why the file says
# APPEND ONLY.
#
# Pre-existing (positions 1..38 in the array => IDs 1..38):
PLANT = 17          # existing "plant" green, used for blades and stems

# Appended by this feature, in this order, at the END of materials.json:
GRASS_TUFT = 39     # the micro material itself (its own cells are the tuft)
FOLIAGE_BUSH = 40
FLOWER_POPPY = 41
FLOWER_DAISY = 42
# Colour materials the micro models paint WITH (they are ordinary materials, so
# a petal that gets knocked loose behaves like a petal):
PETAL_RED = 43
PETAL_WHITE = 44
PETAL_YELLOW = 45
LEAF_GREEN = 46
STEM_GREEN = 47

# ---- vines / climbers / hanging moss set ------------------------------------
# The one set in this file whose models HANG rather than stand. Everything else
# here grows up from z=0 (the cell floor); a vine strand is a continuous rope
# passing THROUGH its cell, so it must reach both z=0 and z=S-1 or consecutive
# cells of the same strand show a visible gap at every cell boundary. That is
# the whole authoring rule for this set and it is easy to get wrong, because
# every existing model in this file breaks it deliberately.
#
# The numbers below are DEFAULTS ONLY and are stale by design. materials.json is
# append-only and positional (id == index + 1), five agents append to it at
# once, and this block was reserved 70..76 and has landed at both 70..73 and
# 77..80 on different runs depending on who committed first. resolve_ids()
# re-derives every one of them from the material NAME at generate time, so a
# concurrent append above this block cannot silently repaint the art.
VINE_HANG = 77
CREEPER_FLOWER = 78
MOSS_HANG = 79
IVY = 80
# Colour material the flowering creeper paints its bloom with. Pre-existing if
# the meadow set is present; resolved by name like everything else, and the
# creeper falls back to its own green if it is not there.
PETAL_PINK = 64
PETAL_BLUE = 63
# The meadow set. Same defaults-are-stale caveat: resolved by name below.
FLOWER_BLUEBELL = 65
FLOWER_FOXGLOVE = 66
FLOWER_BUTTERCUP = 67
FLOWER_CLOVER = 68
FLOWER_WILDROSE = 69
# The desert / pine / alpine set. These were referenced by the models below but
# never declared or registered for name resolution, so this file raised
# NameError and the committed .vox art for them could not be regenerated —
# the same dead-end the meadow set was in. Same stale-defaults caveat.
WOOD = 2               # the pre-existing trunk material, for woody stems
CACTUS_BLOOM = 72
DESERT_SCRUB = 73
DRY_TUSSOCK = 74
HEATH_SHRUB = 75
ALPINE_CUSHION = 76

# Every material name this file paints WITH or declares a model FOR. Palette
# index == material id, so all of these must resolve or the .vox paints the
# wrong colour silently.
_ID_NAMES = {
    "PLANT": "plant",
    "GRASS_TUFT": "grass_tuft",
    "FOLIAGE_BUSH": "foliage_bush",
    "FLOWER_POPPY": "flower_poppy",
    "FLOWER_DAISY": "flower_daisy",
    "PETAL_RED": "petal_red",
    "PETAL_WHITE": "petal_white",
    "PETAL_YELLOW": "petal_yellow",
    "LEAF_GREEN": "leaf_green",
    "STEM_GREEN": "stem_green",
    "PETAL_PINK": "petal_pink",
    "PETAL_BLUE": "petal_blue",
    "FLOWER_BLUEBELL": "flower_bluebell",
    "FLOWER_FOXGLOVE": "flower_foxglove",
    "FLOWER_BUTTERCUP": "flower_buttercup",
    "FLOWER_CLOVER": "flower_clover",
    "FLOWER_WILDROSE": "flower_wildrose",
    "WOOD": "wood",
    "CACTUS_BLOOM": "cactus_bloom",
    "DESERT_SCRUB": "desert_scrub",
    "DRY_TUSSOCK": "dry_tussock",
    "HEATH_SHRUB": "heath_shrub",
    "ALPINE_CUSHION": "alpine_cushion",
    "VINE_HANG": "vine_hang",
    "CREEPER_FLOWER": "creeper_flower",
    "MOSS_HANG": "moss_hang",
    "IVY": "ivy",
}


def resolve_ids():
    """Re-derive every palette index from materials.json array POSITIONS.

    materials.json is append-only and positional (id == index + 1, slot 0 is
    air), and several agents append to it at once. A literal material id in this
    file is therefore a bug waiting for someone else's commit: their block lands
    above mine, every id shifts, and the generated .vox files silently paint
    with whatever material now occupies the old slot. Deriving from the NAMES at
    generate time makes that class of failure impossible — regenerate after an
    append and the art is correct again.

    Missing names are left at their module defaults and reported, so this stays
    runnable before a materials block has been appended.
    """
    import json

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(root, "assets", "materials", "materials.json")
    try:
        with open(path, "r", encoding="utf-8") as f:
            mats = json.load(f)["materials"]
    except Exception as e:  # noqa: BLE001 - diagnostic, never fatal
        print(f"warning: cannot read {path} ({e}); using built-in ids")
        return []
    by_name = {m.get("id"): i + 1 for i, m in enumerate(mats)}
    missing = []
    g = globals()
    for var, name in _ID_NAMES.items():
        if name in by_name:
            g[var] = by_name[name]
        else:
            missing.append(name)
    return missing


# ---- .vox writing (same hand-rolled chunks as scripts/gen_test_mob.py) -------
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


def write_vox(path, size, frames):
    """frames: list of voxel lists, one per flipbook frame.

    No scene graph: with every model at the origin the loader's fallback places
    them all at (0,0,0), which is exactly what a flipbook wants — every frame
    occupies the same cell.
    """
    body = b"".join(model_chunks(size, f) for f in frames)
    data = b"VOX " + struct.pack("<i", 150)
    data += b"MAIN" + struct.pack("<ii", 0, len(body)) + body
    with open(path, "wb") as f:
        f.write(data)


# ---- geometry helpers -------------------------------------------------------
S = 8  # subdiv: 8^3 micro voxels per world cell


def blade(x, y, height, mat, lean=0, phase=0):
    """One grass blade rising from (x, y, 0).

    `lean` shifts the tip sideways over the blade's height; the sway frames
    reuse the same function with a different lean, which is why two frames read
    as the SAME tuft moving rather than as two different tufts.
    """
    out = []
    for z in range(height):
        # integer lean: the blade bends progressively, tip furthest over
        dx = (lean * z) // max(height - 1, 1)
        px = x + dx + phase
        if 0 <= px < S:
            out.append((px, y, z, mat))
    return out


def disc(cx, cy, cz, r, mat, s=S):
    """A filled circle in the XY plane at height cz — a blossom face."""
    out = []
    for y in range(s):
        for x in range(s):
            dx, dy = x - cx, y - cy
            if dx * dx + dy * dy <= r * r:
                out.append((x, y, cz, mat))
    return out


def ring(cx, cy, cz, rin, rout, mat, s=S):
    """An annulus — daisy petals around a centre."""
    out = []
    for y in range(s):
        for x in range(s):
            dx, dy = x - cx, y - cy
            d2 = dx * dx + dy * dy
            if rin * rin < d2 <= rout * rout:
                out.append((x, y, cz, mat))
    return out


# ---- the four models --------------------------------------------------------
def grass_tuft(lean):
    """A few thin blades. Two frames differing only in lean = a sway."""
    v = []
    # Deliberately sparse and asymmetric: a tuft that fills its cell reads as a
    # green block, and the whole point of micro-detail is the silhouette.
    v += blade(2, 2, 7, PLANT, lean=lean)
    v += blade(4, 3, 6, PLANT, lean=-lean)
    v += blade(3, 5, 8, PLANT, lean=lean)
    v += blade(5, 5, 5, PLANT, lean=0)
    v += blade(6, 2, 4, PLANT, lean=-lean)
    return v


def foliage():
    """A leafy clump — denser than grass but still porous, so light gets in."""
    v = []
    cx = cy = 3.5
    for z in range(1, 8):
        # radius bulges in the middle: a clump, not a cylinder
        r = [0, 2.2, 3.0, 3.4, 3.4, 3.0, 2.2, 1.4][z]
        for y in range(S):
            for x in range(S):
                dx, dy = x - cx, y - cy
                if dx * dx + dy * dy > r * r:
                    continue
                # Punch holes on a deterministic lattice so the clump is porous.
                # A solid ellipsoid would render exactly like the cube it
                # replaced, which is the failure mode worth guarding against.
                if (x * 3 + y * 5 + z * 7) % 4 == 0:
                    continue
                v.append((x, y, z, LEAF_GREEN))
    # a short woody stub at the base so the clump is attached to something
    for z in range(0, 2):
        v.append((3, 3, z, STEM_GREEN))
        v.append((4, 3, z, STEM_GREEN))
    return v


def flower_poppy(lean):
    """Stem + a red blossom cup. Two frames = a nod in the breeze."""
    v = []
    # stem, leaning with the frame
    h = 6
    for z in range(h):
        dx = (lean * z) // max(h - 1, 1)
        v.append((3 + dx, 3, z, STEM_GREEN))
    tipx = 3 + (lean * (h - 1)) // max(h - 1, 1)
    # a couple of leaves low on the stem
    v.append((2, 3, 2, LEAF_GREEN))
    v.append((4, 3, 3, LEAF_GREEN))
    # blossom: a small cup, wider at the top
    v += disc(tipx, 3, h, 1, PETAL_RED)
    v += ring(tipx, 3, h + 1, 0, 2, PETAL_RED)
    # a dark eye at the very centre, which is what makes a poppy a poppy
    v.append((tipx, 3, h + 1, PETAL_YELLOW))
    return v


def flower_daisy():
    """White ray petals around a yellow disc floret, on a short stem."""
    v = []
    h = 5
    for z in range(h):
        v.append((3, 3, z, STEM_GREEN))
    v.append((2, 3, 2, LEAF_GREEN))
    v.append((4, 4, 3, LEAF_GREEN))
    # petals: an annulus of white with a yellow centre one voxel proud, so the
    # flower reads from above AND in silhouette
    v += ring(3, 3, h, 1, 2.6, PETAL_WHITE)
    v += disc(3, 3, h, 1, PETAL_YELLOW)
    v.append((3, 3, h + 1, PETAL_YELLOW))
    return v


# ---- meadow flowers ---------------------------------------------------------
# These five were authored once, lost when a concurrent session ran
# `git reset --hard` on the shared tree, and are re-authored here. The .vox
# binaries survived in the commit; the source did not, which made the art a
# dead end nobody could edit. That is the whole reason this block exists.
#
# THE AUTHORING RULE FOR THIS SET is a third one, different from both the
# stand-up models above and the hanging strands below: worldgen stacks a meadow
# flower 1-5 cells tall (flowerAt/flowerHeight in worldgen.wgsl), repeating THE
# SAME model in every cell of the stack. So each model has to work in two roles
# at once:
#
#   * as a WHOLE PLANT, when the species is one cell tall (clover), and
#   * as a SLICE of a taller plant, where it sits above a copy of itself.
#
# Which means the stem must reach z=0 AND z=S-1 (like a vine strand) or a tall
# foxglove shows a gap at every cell boundary — but the blossom must NOT sit at
# the very top, or a 4-cell plant grows four flower heads stacked like beads.
# The resolution used here: put the flowering mass on the SIDES of the stalk
# (bells, florets, hips) rather than as a cap on top. A side-flowering model
# tiles into a continuous flowering spire, which is what a real foxglove or
# bluebell actually looks like, and it is why none of these five are built like
# the poppy/daisy above.
#
# The one exception is clover, which is always exactly one cell (height 1 in
# flowerHeight) and therefore free to be a flat trefoil mat with no through-stem.


def _stalk(x, y, mat, lean=0, s=S):
    """A stem running the FULL height of the cell, z=0..s-1.

    Full height is what makes a stacked flower one continuous plant instead of
    a dashed line — the same constraint the vine cords below carry.
    """
    out = []
    for z in range(s):
        dx = (lean * z) // max(s - 1, 1)
        px = x + dx
        if 0 <= px < s:
            out.append((px, y, z, mat))
    return out


def flower_bluebell(lean):
    """A bowed stalk hung with one-sided nodding bells.

    Bells hang off ONE side only, which is the bluebell's whole silhouette and
    also what keeps a stacked spire from reading as radially symmetric mush.
    """
    v = []
    v += _stalk(3, 3, STEM_GREEN, lean=lean)
    # Bells down the length of the stalk, alternating slightly in reach so a
    # stacked pair does not line its bells up into a straight column.
    for z, reach in ((1, 1), (3, 2), (5, 1), (6, 2)):
        dx = (lean * z) // max(S - 1, 1)
        bx = 3 + dx + reach
        if bx >= S:
            continue
        # each bell: a two-voxel drooping cup
        v.append((bx, 3, z, PETAL_BLUE))
        if z + 1 < S:
            v.append((bx, 3, z + 1, PETAL_BLUE))
        if bx + 1 < S and z > 0:
            v.append((bx, 2, z, PETAL_BLUE))
    # a strap leaf low down, the way a bluebell sits in the sward
    v.append((2, 4, 0, LEAF_GREEN))
    v.append((2, 4, 1, LEAF_GREEN))
    return v


def flower_foxglove(lean):
    """The spire: a thick stalk sleeved in tubular florets on two faces.

    This is the species that stacks tallest (up to 5 cells / 50 cm), so it is
    authored strictly as a repeating SLICE — florets on the sides, nothing that
    reads as a terminal tip, and a stalk touching both cell faces.
    """
    v = []
    v += _stalk(3, 3, STEM_GREEN, lean=lean)
    v += _stalk(4, 3, STEM_GREEN, lean=lean)  # a fat bole: it is a big plant
    for z in range(0, S, 2):
        dx = (lean * z) // max(S - 1, 1)
        # florets left and right, offset in z so the two sides interleave
        lx = 3 + dx - 1
        rx = 4 + dx + 1
        if 0 <= lx < S:
            v.append((lx, 3, z, PETAL_PINK))
            if z + 1 < S:
                v.append((lx, 2, z + 1, PETAL_PINK))
        if 0 <= rx < S and z + 1 < S:
            v.append((rx, 3, z + 1, PETAL_PINK))
            v.append((rx, 4, z, PETAL_PINK))
    return v


def flower_buttercup(lean):
    """A branching cluster of small yellow cups on wiry stems."""
    v = []
    v += _stalk(3, 3, STEM_GREEN, lean=lean)
    # two side stems carrying the cups out from the main axis
    for (bx, by, bz) in ((2, 3, 3), (5, 4, 5)):
        v.append((bx, by, bz, STEM_GREEN))
        v.append((bx, by, bz + 1, STEM_GREEN)) if bz + 1 < S else None
        # the cup: a tiny 5-voxel rosette, yellow
        cz = min(bz + 2, S - 1)
        v.append((bx, by, cz, PETAL_YELLOW))
        for (ox, oy) in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            px, py = bx + ox, by + oy
            if 0 <= px < S and 0 <= py < S:
                v.append((px, py, cz, PETAL_YELLOW))
    v.append((4, 2, 1, LEAF_GREEN))
    v.append((2, 4, 2, LEAF_GREEN))
    return v


def flower_clover():
    """A flat trefoil mat with a couple of round white heads.

    ALWAYS one cell tall (flowerHeight returns 1), so this is the one model in
    the set that does NOT need a through-stem — it is ground cover, and it
    should read as lawn you could lie down in rather than as a stem.
    """
    v = []
    # trefoil leaves: three small lobes on short petioles, lying low
    for (cx, cy) in ((2, 2), (5, 3), (3, 5)):
        v.append((cx, cy, 0, STEM_GREEN))
        for (ox, oy) in ((0, 0), (1, 0), (0, 1)):
            v.append((cx + ox, cy + oy, 1, LEAF_GREEN))
    # two white flower heads just above the leaf mat
    v.append((4, 5, 2, PETAL_WHITE))
    v.append((2, 3, 2, PETAL_WHITE))
    return v


def flower_wildrose(lean):
    """An arching woody briar: thorny cane, sparse leaves, a few pink blooms.

    Placed at the canopy EDGE by worldgen rather than in the open meadow, and
    stacks to 3-4 cells, so like the foxglove it is authored as a repeating
    slice of cane rather than as a single shrub with a top.
    """
    v = []
    # the cane, woody and slightly off-centre so a stack reads as arching
    v += _stalk(4, 4, STEM_GREEN, lean=lean)
    # thorns/short side shoots
    for z in (1, 4, 6):
        dx = (lean * z) // max(S - 1, 1)
        sx = 4 + dx - 1
        if 0 <= sx < S:
            v.append((sx, 4, z, STEM_GREEN))
    # compound leaves off the cane
    for (lx, ly, lz) in ((2, 4, 2), (6, 3, 5), (3, 5, 6)):
        if 0 <= lx < S and 0 <= ly < S and lz < S:
            v.append((lx, ly, lz, LEAF_GREEN))
            if lx + 1 < S:
                v.append((lx + 1, ly, lz, LEAF_GREEN))
    # blooms: five-petal flat faces on the side of the cane, never on top
    for (bz, bx) in ((2, 6), (5, 2)):
        dx = (lean * bz) // max(S - 1, 1)
        px = bx + dx
        if not (0 <= px < S):
            continue
        v.append((px, 4, bz, PETAL_PINK))
        for (ox, oy) in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            qx, qy = px + ox, 4 + oy
            if 0 <= qx < S and 0 <= qy < S:
                v.append((qx, qy, bz, PETAL_PINK))
    return v


# ---- vines, climbers and hanging moss ---------------------------------------
# THE RULE FOR THIS SET, and it is the opposite of every model above: a strand
# must touch BOTH z=0 and z=S-1. Worldgen stacks these cells vertically into a
# rope tens of cells long, so a model that stops short of either face leaves a
# dashed line — the plant equivalent of the "vines came out dotted" failure the
# per-cell (rather than per-column) hash would cause in the shader.
#
# The strands are also deliberately OFF-CENTRE and asymmetric. A cord down the
# exact middle of every cell reads as a machined wire; letting it wander a
# micro-voxel or two per cell, with the wander driven by the yaw variant, is
# what makes a hanging curtain look grown.


def _cord(x0, y0, drift, mat, thick=1, s=S):
    """A near-vertical cord from the cell floor to its ceiling.

    `drift` shifts the cord sideways over the full height, so a column of these
    reads as a rope with some slack rather than as a plumb line. Both end faces
    are always occupied — that is the continuity rule for this set.
    """
    out = []
    for z in range(s):
        dx = (drift * z) // max(s - 1, 1)
        for t in range(thick):
            px, py = x0 + dx + t, y0
            if 0 <= px < s and 0 <= py < s:
                out.append((px, py, z, mat))
    return out


def vine_hang(phase):
    """A hanging vine: one main cord, a thinner companion, a few leaves.

    Two frames with opposite drift give a slow sway. The leaves sit on
    alternating sides at different heights so a long strand does not repeat
    visibly every cell — the tell that gives away a tiled model.
    """
    v = []
    v += _cord(3, 3, 1 if phase == 0 else -1, VINE_HANG)
    # a second, thinner strand alongside: real vines come in tangles
    v += _cord(5, 4, -1 if phase == 0 else 1, VINE_HANG)
    # leaves, offset from the cords so they read as sprouting off them
    for (lx, ly, lz) in ((2, 3, 1), (6, 4, 3), (2, 4, 5), (6, 3, 6)):
        px = lx + (1 if phase else 0)
        for (ox, oy) in ((0, 0), (0, 1)):
            x, y = px + ox, ly + oy
            if 0 <= x < S and 0 <= y < S:
                v.append((x, y, lz, LEAF_GREEN))
    return v


def creeper_flower(phase):
    """A flowering creeper: the same cord, carrying a trumpet bloom.

    Painted with the existing petal materials rather than new ones, on the
    undergrowth-set precedent — a knocked-loose bloom should behave like a
    petal, not like a bespoke material nothing else knows about.
    """
    v = []
    v += _cord(3, 3, 1 if phase == 0 else -1, VINE_HANG)
    v += _cord(5, 4, -1 if phase == 0 else 1, VINE_HANG)
    for (lx, ly, lz) in ((2, 3, 0), (6, 4, 7)):
        for (ox, oy) in ((0, 0), (0, 1)):
            x, y = lx + ox, ly + oy
            if 0 <= x < S and 0 <= y < S:
                v.append((x, y, lz, LEAF_GREEN))
    # the bloom: a small cup hanging off the main cord, mid-cell
    cx, cy, cz = 4 + (1 if phase else 0), 3, 3
    for (dx, dy) in ((-1, 0), (1, 0), (0, -1), (0, 1), (0, 0)):
        x, y = cx + dx, cy + dy
        if 0 <= x < S and 0 <= y < S:
            v.append((x, y, cz, PETAL_PINK))
    for (dx, dy) in ((-1, 0), (1, 0), (0, 0)):
        x, y = cx + dx, cy + dy
        if 0 <= x < S and 0 <= y < S:
            v.append((x, y, cz - 1, PETAL_PINK))
    v.append((cx, cy, cz + 1, PETAL_YELLOW))       # the boss at the throat
    return v


def moss_hang():
    """A Spanish-moss beard: many thin filaments, no single trunk cord.

    Static on purpose (the flipbook note in main()): moss is placed as densely
    as anything in this set, so it is the worst place to spend brick pool on
    animation, and a beard reads as fuzz whether it moves or not.

    Deliberately NOT a full curtain — the filaments are sparse and unequal, so
    light comes through a beard. A solid one reads as a hanging blanket, which
    is the single most common way procedural moss goes wrong.
    """
    v = []
    # filaments at irregular (x, y) with varying drift so none of them line up
    for (fx, fy, drift) in ((1, 2, 0), (3, 1, 1), (2, 5, -1),
                            (5, 3, 0), (6, 6, 1), (4, 6, -1)):
        v += _cord(fx, fy, drift, MOSS_HANG)
    # a few short stubs that do NOT span the cell: the ragged ends of filaments
    # that stopped partway, which is what keeps the beard from reading as combed
    for (fx, fy, z0, z1) in ((0, 4, 4, S), (7, 1, 0, 3), (5, 0, 2, 6)):
        for z in range(z0, z1):
            v.append((fx, fy, z, MOSS_HANG))
    return v


def ivy():
    """Trunk/wall ivy: a flat mat of leaves on a climbing runner.

    This one is a CLIMBER, not a hanger, so it spans the cell vertically like
    the strands but is pressed flat against one face — worldgen places it on the
    shell just outside a trunk or wall, and the leaves want to face outward off
    that surface rather than fill the cell.
    """
    v = []
    # the runner: two stems climbing the y=1 face, spanning the full height
    v += _cord(2, 1, 1, VINE_HANG)
    v += _cord(5, 1, -1, VINE_HANG)
    # leaves: flat plates a micro-voxel proud of the runner, alternating sides
    for (lx, lz) in ((1, 1), (4, 2), (2, 4), (6, 5), (3, 6), (5, 0), (0, 7)):
        for (ox, oz) in ((0, 0), (1, 0), (0, 1), (1, 1)):
            x, z = lx + ox, lz + oz
            if 0 <= x < S and 0 <= z < S:
                v.append((x, 2, z, IVY))
                if (x + z) % 3 == 0:
                    v.append((x, 3, z, IVY))
    return v


# ---- desert / pine-highland / alpine models ---------------------------------
# The authoring problem these four share, and which none of the forest models
# have: they are placed on GROUND THAT IS NOT DARK. A forest model can be as
# dense as it likes because it sits on shadowed soil; these sit on pale sand or
# on white snow, where a dense model reads as a black blob and the cell looks
# like a hole punched in the ground. So every one of them is sparser than its
# forest equivalent, and the alpine one is sparser still.


def desert_scrub():
    """Creosote/sage: a leafless woody skeleton, all twig and no mass.

    The look is DRY. Where foliage() bulges into a leafy clump, this forks into
    bare stems with a few leaf specks caught in them — a bush that has given up
    on photosynthesis is the single most desert-reading plant shape there is.
    Static, because a stiff woody shrub is exactly the thing that does NOT move
    in wind; the tussock beside it carries all the motion in this biome.
    """
    v = []
    # Three stems leaving one root, splaying as they rise. Integer lean per
    # step, same technique as blade(), so the fork widens with height.
    for (bx, by, lean, top) in ((3, 3, 1, 7), (4, 4, -1, 6), (3, 4, 0, 5)):
        for z in range(top):
            dx = (lean * z) // max(top - 1, 1)
            x = bx + dx
            if 0 <= x < S:
                v.append((x, by, z, DESERT_SCRUB))
                # secondary twigs off the upper half only, so the base stays a
                # clean stem and the crown is where the tangle is
                if z >= top // 2 and (x + by + z) % 3 == 0:
                    tx = x + (1 if lean >= 0 else -1)
                    if 0 <= tx < S:
                        v.append((tx, by, z, DESERT_SCRUB))
    # A handful of leaf specks, NOT a canopy: creosote carries tiny resinous
    # leaves and the gaps between them are most of the plant.
    for (lx, ly, lz) in ((2, 3, 5), (5, 4, 4), (4, 3, 6), (2, 4, 3), (5, 3, 6)):
        v.append((lx, ly, lz, LEAF_GREEN))
    return v


def dry_tussock(lean):
    """Bleached bunchgrass: a fountain of blades springing from one crown.

    The difference from grass_tuft is the HABIT, not the colour. Meadow grass
    grows as separate blades out of turf; a desert bunchgrass grows as a tight
    clump that sprays outward from a single point, which is why every blade here
    starts within one micro-voxel of the centre and leans a different way.
    Two frames so the whole clump sways as one thing.
    """
    v = []
    # (start x, start y, height, lean multiplier) — the multipliers fan the
    # spray out around the compass rather than all bending one way, which is
    # what stops the two frames reading as the clump being blown flat.
    for (bx, by, top, mul) in ((3, 3, 8, 1), (4, 3, 7, -1), (3, 4, 6, 2),
                               (4, 4, 8, -2), (3, 3, 5, 0), (4, 4, 4, 1)):
        for z in range(top):
            dx = (lean * mul * z) // max(top - 1, 1)
            x = bx + dx
            if 0 <= x < S:
                v.append((x, by, z, DRY_TUSSOCK))
    # A few dead blades collapsed around the base: a bunchgrass keeps last
    # year's growth as a straw skirt, and it is what roots the clump visually
    # instead of leaving the blades apparently hovering.
    for (lx, ly) in ((2, 3), (5, 4), (2, 4), (5, 3), (3, 2), (4, 5)):
        v.append((lx, ly, 0, DRY_TUSSOCK))
    return v


def heath_shrub():
    """Huckleberry/juniper: a low dense mound with berries in it.

    This is the one model in this set that is allowed to be dense, because it is
    the one that sits on dark conifer duff rather than on sand or snow. Even so
    it is punched through on a lattice like foliage(), for the same reason: a
    solid mound renders identically to the cube it replaced.
    """
    v = []
    cx = cy = 3.5
    # Squat: wider than tall, topping out at z=5 of 8. A shrub under a pine
    # canopy grows sideways toward the light gaps, not upward into the shade.
    for z in range(0, 6):
        r = [3.2, 3.4, 3.3, 2.9, 2.2, 1.3][z]
        for y in range(S):
            for x in range(S):
                dx, dy = x - cx, y - cy
                if dx * dx + dy * dy > r * r:
                    continue
                if (x * 5 + y * 3 + z * 7) % 4 == 0:
                    continue
                v.append((x, y, z, HEATH_SHRUB))
    # Berries, sitting proud on the upper surface so they catch light. Painted
    # with the shrub's own material: at 10 cm a cell the berry cluster is a
    # colour variant of the plant, not a separate one, and a knocked-loose
    # berry should behave like the shrub it came off.
    for (bx, by) in ((2, 3), (5, 2), (3, 5), (4, 4)):
        v.append((bx, by, 5, HEATH_SHRUB))
    # a woody stub at the base, same anchoring trick foliage() uses
    for z in range(0, 2):
        v.append((3, 3, z, WOOD))
    return v


def alpine_cushion():
    """The one thing that grows above the treeline: a tight cushion of moss.

    SPARSE AND FLAT ON PURPOSE. A cushion plant is an adaptation to wind — it
    hugs the rock in a dome barely a hand deep, and anything that stood up would
    be shredded. So this occupies the bottom two micro-voxels of its cell and
    nothing above them, which is also what lets the same material double as the
    lichen crust on exposed rock when worldgen places it on stone rather than
    snow: at this height the two are the same object.

    The frame count is one. Nothing up here moves; that is the point.
    """
    v = []
    cx = cy = 3.5
    # z=0 is the full cushion footprint, z=1 is a smaller dome on top of it —
    # two layers total, so the whole plant is 2/8 of a cell (~2.5 cm) deep.
    for (z, r) in ((0, 3.4), (1, 2.4)):
        for y in range(S):
            for x in range(S):
                dx, dy = x - cx, y - cy
                if dx * dx + dy * dy > r * r:
                    continue
                # Punched harder than the forest models (every 3rd, not every
                # 4th): against snow a solid mat reads as a dark hole, and the
                # gaps let the white ground through so it reads as a plant
                # clinging to the surface rather than as a patch of dirt.
                if (x * 3 + y * 5 + z) % 3 == 0:
                    continue
                v.append((x, y, z, ALPINE_CUSHION))
    # The flowers: a few single micro-voxels one layer proud of the cushion.
    # Tiny and few — an alpine cushion in bloom carries a scatter of pinhead
    # flowers, and painting the whole top of the dome would turn the material's
    # LOD colour into the flower colour rather than the plant's.
    for (fx, fy) in ((2, 3), (4, 2), (3, 5), (5, 4)):
        v.append((fx, fy, 2, CACTUS_BLOOM))
    return v


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "assets", "microvox")
    os.makedirs(out_dir, exist_ok=True)
    size = (S, S, S)

    # Palette indices come from materials.json POSITIONS, never from the
    # literals at the top of this file — see resolve_ids().
    missing = resolve_ids()
    if missing:
        print("warning: not in materials.json yet (using built-in ids): " +
              ", ".join(missing))

    written = []
    for name, frames in [
        # two frames each for the swaying ones; the lean flips sign so the sway
        # is symmetric about upright rather than a one-sided lurch
        ("grass_tuft", [grass_tuft(1), grass_tuft(-1)]),
        ("foliage", [foliage()]),
        ("flower_poppy", [flower_poppy(0), flower_poppy(1)]),
        ("flower_daisy", [flower_daisy()]),
        # ---- meadow flowers ----
        # The four stalked species sway; clover is a flat mat with nothing to
        # move. Every one of these is stacked 1-5 cells deep by worldgen, so the
        # flipbook is paid once per MODEL and not once per cell of the stalk —
        # the same economics the vine strands below rely on.
        ("flower_bluebell", [flower_bluebell(0), flower_bluebell(1)]),
        ("flower_foxglove", [flower_foxglove(0), flower_foxglove(1)]),
        ("flower_buttercup", [flower_buttercup(0), flower_buttercup(1)]),
        ("flower_clover", [flower_clover()]),
        ("flower_wildrose", [flower_wildrose(0), flower_wildrose(1)]),
        # ---- vines / climbers / hanging moss ----
        # The two HANGING strands sway; the moss beard and the wall ivy do not.
        # Same brick-pool reasoning as the undergrowth above, with one addition:
        # a vine strand is many cells of the SAME model stacked vertically, so
        # the flipbook cost is paid once for the model, not once per cell, which
        # is why the strands can afford motion where the dense ground cover
        # cannot. Ivy is pressed flat against stone and has nothing to sway in.
        ("vine_hang", [vine_hang(0), vine_hang(1)]),
        ("creeper_flower", [creeper_flower(0), creeper_flower(1)]),
        ("moss_hang", [moss_hang()]),
        ("ivy", [ivy()]),
        # ---- desert / pine highland / alpine ----
        # Only the bunchgrass moves. The scrub is a stiff woody skeleton, the
        # heath shrub is a dense mound, and the alpine cushion is an adaptation
        # to being unable to move — so a flipbook on any of the three would be
        # brick-pool cost spent on motion that would be wrong if it appeared.
        ("desert_scrub", [desert_scrub()]),
        ("dry_tussock", [dry_tussock(1), dry_tussock(-1)]),
        ("heath_shrub", [heath_shrub()]),
        ("alpine_cushion", [alpine_cushion()]),
    ]:
        path = os.path.join(out_dir, name + ".vox")
        write_vox(path, size, frames)
        written.append((path, len(frames), sum(len(f) for f in frames)))

    for path, nf, nv in written:
        print(f"wrote {path} ({nf} frame(s), {nv} voxels)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
