"""Shared builder for the bladed items: the sword, the shortsword and the dagger.

WHY A MODULE. gen_sword_item.py and gen_shortblade_items.py each carried their
own copy of the .vox writer, the ellipse test and a blade loop, and the three
weapons they built were the same slab — a 2-micro-thick bar with a taper, on a
round stick — at three lengths. That is what "lego planks" meant: at 40 art
voxels per metre a blade is 5 cm thick with square shoulders, and there is no
room in the lattice for anything between "steel" and "air".

WHAT CHANGED (2026-09-27): THE ART IS AUTHORED AT 80 VOXELS PER METRE, and at
that pitch a blade has a SECTION. The flats are three cells thick, the honed
edge one, and the step between them is the bevel — a one-cell ramp that the
micro renderer's occupancy-gradient normal (microbody.wgsl BODY_SMOOTH_N)
shades as a slope rather than as a stair. Fullers, midribs, a ricasso, langets
on the guard and a chamfered wheel pommel all fall out of the same idea: shape
the SECTION, not only the silhouette.

WHAT DID NOT CHANGE, and must not:
  * every length a character holds a weapon by. The lattice doubled, so every
    sidecar number doubled with it — `hilt`, `edge.from/to/halfWidth` and
    `grip.translation` land on exactly the world positions they had at 40/m.
    The hilt box's centre (what the runtime puts on the hand socket) and the
    blade's reach are the same to the millimetre.
  * the grip ROTATION, which the tuner's held-item preview owns (commit
    355a024 rolled the sword 90 degrees there). The generators write it; the
    value is the one the tuner saved.
  * the weapon's HEFT. That used to be derived from voxel volume, and a blade
    with a real section has far less volume than a slab did — the sword would
    have lost ~60% of its cut. items.json pins `heft` to the old derived
    factors instead (the authored override item.h keeps for "the case the
    geometry cannot state", which is now this one: steel is denser than the
    lattice can say).

PAINT. Each .vox carries a "<name>.col" colour layer over the same cells
(voxload.cpp's art-layer fold) and an RGBA chunk. The material stays steel /
grip_leather / staff_wood — that is what a dropped sword becomes in the grid —
and the colour is art: honed edge, bevel, flat, fuller, blued or brass
fittings, a spiral leather wrap. The three weapons share one colour table, so
the merged art palette (255 entries across every creature and item) pays for
each colour once. Unused RGBA entries are written as zeros, and the merge
(MicroBodyMergeArt) only takes the slots a voxel actually paints.

Coordinates are .vox SCENE axes: x along the weapon (pommel at low x), y across
the blade (the width), z through it (the thickness — the thin axis, which is
why every sidecar's `flat` is +z). Every box is ODD in y and z and the weapon's
axis runs through the middle cell, so `dy`/`dz` below are integers centred on
zero and the art is mirror-symmetric by construction.
"""

import json
import math
import os
import struct

# ---- materials (palette index == material ID) -------------------------------
STEEL = 57
GRIP_LEATHER = 58
STAFF_WOOD = 54
BONE = 56

ART_VOXELS_PER_METRE = 80
# Micro per world voxel at the engine's 10 voxels/metre (sim/world.h).
SCALE = ART_VOXELS_PER_METRE // 10

# ---- the colour table ---------------------------------------------------------
# Names -> RGB. ONE table for every blade, so the same steel is the same merged
# palette entry on all three weapons. Slots are assigned per file, from 255
# downward, only for the names that file uses.
#
# DARKER THAN THEY LOOK HERE, on purpose. Measured in --shot-mob at --time 0.4,
# the lit scene lifts a body's albedo ~1.6x and lifts darks most: #2C323A iron
# renders ~#6E757C, and the first pass (flat #A7B0BB) clipped the blade to one
# near-white with no bevel left in it. Judge a change in the engine
# (`--shot-mob human:*sword` writes screenshot_mob_held*.bmp), not in a
# swatch.
COLOURS = {
    # the blade
    "edge":       0xCDD4DC,   # the honed edge: the brightest steel there is
    "bevel":      0xA9B2BD,   # the shoulder of the bevel, catching light
    "flat":       0x8A949F,   # the flat
    "forged":     0x707A85,   # ricasso: unground, as it came off the anvil
    "fuller":     0x4C5561,   # the groove
    # blued / blackened fittings (the arming sword's guard and pommel)
    "iron":       0x2C323A,
    "iron_hi":    0x535D68,
    # brass fittings (the dagger's, and the gladius's guard plate)
    "brass":      0x9A7630,
    "brass_hi":   0xC9A650,
    "brass_dk":   0x654B1A,
    # a leather-wrapped grip
    "leather":    0x3A2619,
    "leather_dk": 0x1E130C,
    "cord":       0x573A26,
    # the gladius's hardwood pommel and guard, bone grip
    "wood":       0x4A2E1C,
    "wood_hi":    0x684329,
    "bone":       0xBDB091,
    "bone_dk":    0x8E7F60,
}


def clamp01(v):
    return 0.0 if v < 0.0 else (1.0 if v > 1.0 else v)


def smooth(a, b, v):
    t = clamp01((v - a) / (b - a))
    return t * t * (3.0 - 2.0 * t)


# ---- .vox writing -------------------------------------------------------------
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


def write_vox(path, name, size, cells):
    """`cells` maps (x, y, z) -> (material, colour name). Writes two models over
    the SAME cells at the SAME translation — "<name>" carrying materials and
    "<name>.col" carrying art slots — because the loader joins them by absolute
    cell (voxload.cpp), and an RGBA chunk whose unused entries are zero."""
    used = sorted({c for (_m, c) in cells.values()})
    slot_of = {}
    for i, cname in enumerate(used):
        slot_of[cname] = 255 - i
    assert 255 - len(used) >= 128, "more colours than the art palette range"

    keys = sorted(cells)
    body = model_chunks(size, [(x, y, z, cells[(x, y, z)][0]) for (x, y, z) in keys])
    body += model_chunks(size, [(x, y, z, slot_of[cells[(x, y, z)][1]])
                                for (x, y, z) in keys])

    # ENTRY i IS INDEX i+1 (gen_stock_armor.py rgba_chunk says why that matters).
    pal = bytearray(1024)
    for cname, slot in slot_of.items():
        rgb = COLOURS[cname]
        o = (slot - 1) * 4
        pal[o:o + 4] = bytes(((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255))

    pivot = (size[0] // 2, size[1] // 2, size[2] // 2)
    graph = ntrn(0, "", 1, (0, 0, 0)) + ngrp(1, [2, 4])
    graph += ntrn(2, name, 3, pivot) + nshp(3, 0)
    graph += ntrn(4, name + ".col", 5, pivot) + nshp(5, 1)

    payload = body + chunk(b"RGBA", bytes(pal)) + graph
    data = b"VOX " + struct.pack("<i", 150)
    data += b"MAIN" + struct.pack("<ii", 0, len(payload)) + payload
    with open(path, "wb") as f:
        f.write(data)
    return used


# ---- building ---------------------------------------------------------------
class Forge:
    """A sparse lattice with the box centred on the weapon's axis. Parts are
    stamped in PRIORITY order: the first part to claim a cell keeps it, so a
    guard stamped before the blade wins the cells where the two meet."""

    def __init__(self, sx, sy, sz):
        assert sy % 2 == 1 and sz % 2 == 1, "cross-section must be odd"
        self.size = (sx, sy, sz)
        self.cy = (sy - 1) // 2
        self.cz = (sz - 1) // 2
        self.cells = {}

    def put(self, x, dy, dz, mat, col):
        sx, sy, sz = self.size
        y, z = dy + self.cy, dz + self.cz
        if not (0 <= x < sx and 0 <= y < sy and 0 <= z < sz):
            return
        self.cells.setdefault((x, y, z), (mat, col))

    def has(self, x, dy, dz):
        return (x, dy + self.cy, dz + self.cz) in self.cells

    def stamp(self, fn, x0=0, x1=None):
        """fn(x, dy, dz) -> (mat, colour) or None, for every cell in x0..x1."""
        sx, sy, sz = self.size
        x1 = sx if x1 is None else x1
        for x in range(max(0, x0), min(sx, x1)):
            for dy in range(-self.cy, self.cy + 1):
                for dz in range(-self.cz, self.cz + 1):
                    r = fn(x, dy, dz)
                    if r is not None:
                        self.put(x, dy, dz, r[0], r[1])

    def exposed_edges(self, x, dy, dz):
        """How many of the four faces ACROSS the weapon's length (+-y, +-x)
        open onto air, plus whether the +-z faces do. Used to put a highlight
        on the arrises of fittings — the corners a real guard wears bright."""
        n = 0
        for (ax, ay) in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            if not self.has(x + ax, dy + ay, dz):
                n += 1
        zface = (not self.has(x, dy, dz + 1)) or (not self.has(x, dy, dz - 1))
        return n, zface

    def recolour_arrises(self, x0, x1, base, hi, mats=None):
        """Every cell of colour `base` in x0..x1 that sits on an arris (two
        open faces, or one open side face plus an open z face) becomes `hi`.
        Done AFTER stamping, because it is a property of the finished shape."""
        changes = []
        for (x, y, z), (m, c) in self.cells.items():
            if c != base or not (x0 <= x < x1):
                continue
            if mats is not None and m not in mats:
                continue
            dy, dz = y - self.cy, z - self.cz
            n, zf = self.exposed_edges(x, dy, dz)
            if n >= 2 or (n >= 1 and zf):
                changes.append(((x, y, z), (m, hi)))
        for k, v in changes:
            self.cells[k] = v


def wrap_colour(x, dy, dz, period=3.0):
    """A spiral leather wrap, laid at 45 degrees: the phase advances one cell
    per cell of ARC LENGTH around the grip (not per unit angle, which on a
    5-cell section quantizes into a checkerboard), so every face shows clean
    diagonal bands. Dark in the gap between turns, lighter on the cord's
    crown."""
    a = math.atan2(dz, dy)                        # -pi..pi around
    arc = a * max(1.0, math.hypot(dy, dz))
    ph = ((x + arc) / period) % 1.0
    if ph < 0.30:
        return "leather_dk"
    if ph < 0.62:
        return "cord"
    return "leather"


def blade_section(dy, dz, hw, *, thick_to, fuller=0.0, ridge=False):
    """The cross-section of a ground blade at one x.

    `hw`        half-width of the blade here (continuous, in cells)
    `thick_to`  |dy| below which the blade is three cells thick (|dz| <= 1);
                outside it the blade is one cell thick (the bevel ground down
                to the edge). THIS STEP IS THE BEVEL.
    `fuller`    half-width of the fuller groove; inside it the blade is one
                cell thick again, so the groove reads as a channel down the
                middle of the flat
    `ridge`     paint the midrib (dy == 0) as a highlight — a diamond section

    Returns a colour name, or None where there is no steel."""
    ady = abs(dy)
    if ady >= hw:
        return None
    thick = ady < thick_to and not (ady < fuller)
    if abs(dz) > (1 if thick else 0):
        return None
    if ady < fuller:
        return "fuller"
    if not thick:
        # the ground bevel down to the edge; its outermost cell is the edge
        return "edge" if ady + 1.0 >= hw else "bevel"
    if ridge and ady == 0:
        return "bevel"
    # the flat's last cell before the bevel breaks is the shoulder
    if ady + 1.0 >= thick_to:
        return "bevel"
    return "flat"


def emit(out_dir, name, forge, sidecar):
    sx, sy, sz = forge.size
    # DebrisVoxel is int8 and a held item's collider is built on the item's own
    # lattice (mob.cpp EquipItem: ownPhysScale = ownSkinScale), so no axis may
    # pass 127 micro.
    assert max(forge.size) <= 120, f"{name} exceeds DebrisVoxel int8 range"
    used = write_vox(os.path.join(out_dir, f"{name}.vox"), name, forge.size,
                     forge.cells)
    with open(os.path.join(out_dir, f"{name}.json"), "w") as f:
        json.dump(sidecar, f, indent=2)
        f.write("\n")
    vol = len(forge.cells) / float(SCALE ** 3)
    print(f"wrote {out_dir}/{name}.vox  ({len(forge.cells)} voxels, size "
          f"{forge.size}, {len(used)} colours)")
    print(f"  {sx} micro = {sx / SCALE:.2f} world voxels = "
          f"{sx / ART_VOXELS_PER_METRE:.3f} m; voxel volume {vol:.2f} world "
          f"voxels (heft is PINNED in items.json, not derived from this)")


def sidecar(name, forge, *, comment, hp, sever, grip_t, hilt_x, edge_x,
            half_w, old_line, spring):
    """The sidecar, with every length in the 80/m lattice. The hilt box runs
    through the MIDDLE of the cross-section, which is where the weapon's axis
    is by construction (Forge centres it).

    `old_line` is where the 40/m sidecar's cutting segment ran, as a (y, z)
    offset from the blade's axis in WORLD voxels. That sidecar had no way to
    say where across the blade the edge is, so the segment rode the model's
    min-corner line — (+0.75, -0.5) off the axis for the sword — and every
    swing, parry and wound gate was tuned against a hitbox there. A 23-cell-
    wide crossguard box would have walked that corner line to (+1.44, -0.31);
    `edge.line` pins it where it was instead, so this re-art moves NO hit.
    Putting it on the axis (0, 0) is the right geometry and a separate,
    gameplay-visible change: measured 2026-09-27 it flips which swing-plane
    check fails (the follow residual spikes to 6.18 vox at the reversal)."""
    sx, sy, sz = forge.size
    line_y = sy / 2.0 + old_line[0] * SCALE
    line_z = sz / 2.0 + old_line[1] * SCALE
    return {
        "comment": comment,
        "name": name,
        "model": name,
        "artVoxelsPerMetre": ART_VOXELS_PER_METRE,
        "hp": hp,
        "severable": True,
        "severImpactSpeed": sever,
        "grip": {
            "held_right": {
                "translation": list(grip_t),
                # Owned by the tuner's held-item preview; see the module note.
                "rotation": [90, -90, 0],
                "scale": 1,
            }
        },
        # Where the fist closes: the wrapped grip, full cross-section, so the
        # centre lands on the axis.
        "hilt": {"min": [hilt_x[0], 0, 0],
                 "size": [hilt_x[1] - hilt_x[0], sy, sz]},
        "edge": {
            "from": edge_x[0],
            "to": edge_x[1],
            "axis": [1, 0, 0],
            # The thin axis is z; see the module note.
            "flat": [0, 0, 1],
            "halfWidth": half_w,
            # WHERE ACROSS THE BLADE the edge runs, in the same scene units as
            # the art (melee.cpp LoadItemAsset): where the 40/m art's corner
            # line put it, so no hit moved. See `old_line` above.
            "line": [0, line_y, line_z],
        },
        "spring": spring,
    }
