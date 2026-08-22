#!/usr/bin/env python3
"""Generate the SHORELINE micro-detail set into assets/microvox/.

The wet fringe around a pond (worldgen.wgsl, shoreAt + the shore-cover block in
genCell). The pond INTERIOR already had lilypads, reeds and kelp; this is the
band OUTSIDE the disc, which until now went straight from water to the same
plain grass as a hillside a kilometre inland.

Two of the seven shore materials are ONE world cell tall and therefore get a
micro model here — marsh grass and the water iris. The other five are either
multi-cell stalks that worldgen stacks itself (cattail, its seed head,
horsetail) or ground skins with no geometry of their own (shore mud, wet moss),
and none of those wants a micro brick: a model is one CELL, so a 20-voxel
cattail cannot be one.

A SEPARATE FILE from scripts/gen_microvox.py on purpose. That script is being
edited by other flora agents in the same window and it regenerates every model
it knows about; splitting the shore set off means neither of us can clobber the
other's art by running our own generator.

Everything about the .vox encoding, the Z-up scene convention and the
"palette index == material id" rule is documented at the top of
scripts/gen_microvox.py — read that first. The one rule worth restating here
because it is the easiest to get wrong:

  A MICRO MATERIAL'S OWN `colors` IN materials.json ARE ITS LOD COLOURS.
  Past TUNE_MICRO_LOD_DIST the cell shades as a plain voxel with them, so they
  must be the MODEL'S AVERAGE. A water iris cell is mostly green leaf with a
  small violet flag, so its LOD colour is a blue-green, not violet — otherwise
  a distant shore reads as a field of purple cubes.

Usage:  python scripts/gen_microvox_shore.py
"""
import json
import os
import struct
import sys

# ---- material ids -----------------------------------------------------------
# Index in materials.json's "materials" array + 1 (slot 0 is air).
#
# These are DEFAULTS ONLY. resolve_ids() re-derives every one of them from the
# material NAMES at generate time, exactly as gen_microvox.py does and for the
# same reason: materials.json is append-only and positional, several agents
# append to it at once, and a literal id written here goes stale the moment
# someone else's block lands above mine — silently, with the art repainted in
# whatever material now occupies the slot. The names are the contract.
#
# Painted WITH (all pre-existing):
STEM_GREEN = 47
LEAF_GREEN = 46
PETAL_YELLOW = 45
PETAL_BLUE = 63
# Declared FOR (this feature's own micro materials):
MARSH_GRASS = 71
WATER_IRIS = 74

_ID_NAMES = {
    "STEM_GREEN": "stem_green",
    "LEAF_GREEN": "leaf_green",
    "PETAL_YELLOW": "petal_yellow",
    "PETAL_BLUE": "petal_blue",
    "MARSH_GRASS": "marsh_grass",
    "WATER_IRIS": "water_iris",
}


def resolve_ids():
    """Re-derive every palette index from materials.json array POSITIONS."""
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


# ---- .vox writing (same hand-rolled chunks as scripts/gen_microvox.py) -------
def chunk(cid, content, children=b""):
    return cid + struct.pack("<ii", len(content), len(children)) + content + children


def model_chunks(size, voxels):
    sx, sy, sz = size
    xyzi = struct.pack("<i", len(voxels)) + b"".join(
        struct.pack("4B", x, y, z, c) for (x, y, z, c) in voxels
    )
    return chunk(b"SIZE", struct.pack("<iii", sx, sy, sz)) + chunk(b"XYZI", xyzi)


def write_vox(path, size, frames):
    """frames: list of voxel lists, one per flipbook frame."""
    body = b"".join(model_chunks(size, f) for f in frames)
    data = b"VOX " + struct.pack("<i", 150)
    data += b"MAIN" + struct.pack("<ii", 0, len(body)) + body
    with open(path, "wb") as f:
        f.write(data)


# ---- geometry ---------------------------------------------------------------
S = 8  # subdiv: 8^3 micro voxels per world cell


def blade(x, y, height, mat, lean=0, taper_from=None):
    """One blade rising from (x, y, 0), bending `lean` units over its height.

    `taper_from` doubles the blade's width below that height, which is what
    turns a grass blade into a SEDGE: marsh grass has broad strap leaves with a
    keel, not the hair-thin blades of lawn grass, and at 8 units across the only
    way to say "broad" is to spend a second voxel on the lower half.
    """
    out = []
    for z in range(height):
        dx = (lean * z) // max(height - 1, 1)
        px = x + dx
        if 0 <= px < S:
            out.append((px, y, z, mat))
            if taper_from is not None and z < taper_from and 0 <= px + 1 < S:
                out.append((px + 1, y, z, mat))
    return out


def marsh_grass(lean):
    """A dense clump of broad sedge leaves — the ground cover of the whole band.

    Denser and TALLER than grass_tuft on purpose. This is the difference between
    a mown lawn running up to the water and a marsh: shore grass grows in fat
    hummocks that you push through, so this clump fills most of its cell's
    height while still leaving the silhouette ragged. It is deliberately not
    symmetric — the leaves fan to one side, which is what a tussock does.

    Two frames differing only in lean read as the SAME clump swaying, which is
    the entire reason the lean is a parameter rather than baked in.
    """
    v = []
    # tall central strap leaves, fanning
    v += blade(2, 2, 8, LEAF_GREEN, lean=lean, taper_from=4)
    v += blade(4, 2, 7, LEAF_GREEN, lean=lean + 1, taper_from=3)
    v += blade(3, 4, 8, LEAF_GREEN, lean=-lean, taper_from=4)
    v += blade(5, 4, 6, LEAF_GREEN, lean=lean, taper_from=3)
    # shorter outer leaves, filling the base so the hummock reads as solid
    # at the ground and open at the top
    v += blade(1, 5, 5, STEM_GREEN, lean=-lean, taper_from=2)
    v += blade(6, 3, 4, STEM_GREEN, lean=lean + 1)
    v += blade(4, 6, 5, LEAF_GREEN, lean=0, taper_from=2)
    v += blade(2, 6, 3, STEM_GREEN, lean=-lean)
    return v


def water_iris(lean):
    """Sword leaves with a violet flag flower — the accent of the shore band.

    An iris is a FAN of flat vertical sword leaves with one flower stem rising
    through the middle, and the fan is the recognisable part; the flower is
    small. So most of the model is leaf, which is also why the material's LOD
    colour is green rather than violet.

    The flag itself is three falls (the drooping outer petals) around a raised
    standard, with a yellow signal on the falls — that yellow streak is what
    reads as "iris" at 8 units rather than "purple blob".
    """
    v = []
    # the leaf fan: flat vertical straps, all in one plane, different heights
    for i, (x, h) in enumerate(((1, 6), (2, 8), (5, 7), (6, 5))):
        ln = lean if i % 2 == 0 else -lean
        for z in range(h):
            dx = (ln * z) // max(h - 1, 1)
            px = x + dx
            if 0 <= px < S:
                v.append((px, 3, z, LEAF_GREEN))
                if z < h // 2 and 0 <= px < S:
                    v.append((px, 4, z, LEAF_GREEN))
    # flower stem through the middle of the fan, standing proud of the leaves
    fh = 7
    for z in range(fh):
        v.append((3, 3, z, STEM_GREEN))
    tip = 3 + (lean * (fh - 1)) // max(fh - 1, 1)
    # falls: three drooping petals around the tip, one unit below the standard
    for dx, dy in ((-1, 0), (1, 0), (0, 1)):
        px, py = tip + dx, 3 + dy
        if 0 <= px < S and 0 <= py < S:
            v.append((px, py, fh, PETAL_BLUE))
            if fh - 1 >= 0:
                v.append((px, py, fh - 1, PETAL_BLUE))
    # the yellow signal streak on the near fall
    if 0 <= tip < S:
        v.append((tip, 4, fh - 1, PETAL_YELLOW))
    # standard: the upright inner petals, one unit above the falls
    if fh + 1 < S and 0 <= tip < S:
        v.append((tip, 3, fh, PETAL_BLUE))
        v.append((tip, 3, fh + 1, PETAL_BLUE))
    return v


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "assets", "microvox")
    os.makedirs(out, exist_ok=True)

    missing = resolve_ids()
    if missing:
        print("warning: not in materials.json yet, using default ids: " +
              ", ".join(missing))
    print(f"ids: marsh_grass={MARSH_GRASS} water_iris={WATER_IRIS} "
          f"leaf_green={LEAF_GREEN} stem_green={STEM_GREEN} "
          f"petal_blue={PETAL_BLUE} petal_yellow={PETAL_YELLOW}")

    size = (S, S, S)
    # Two frames each, differing only by the lean, so the shore breathes. The
    # frame DURATIONS live in the "micro" block in materials.json, not here —
    # .vox cannot carry timing.
    write_vox(os.path.join(out, "marsh_grass.vox"), size,
              [marsh_grass(1), marsh_grass(-1)])
    write_vox(os.path.join(out, "water_iris.vox"), size,
              [water_iris(0), water_iris(1)])
    print("wrote assets/microvox/{marsh_grass,water_iris}.vox")
    return 0


if __name__ == "__main__":
    sys.exit(main())
