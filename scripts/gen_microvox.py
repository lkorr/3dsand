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


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "assets", "microvox")
    os.makedirs(out_dir, exist_ok=True)
    size = (S, S, S)

    written = []
    for name, frames in [
        # two frames each for the swaying ones; the lean flips sign so the sway
        # is symmetric about upright rather than a one-sided lurch
        ("grass_tuft", [grass_tuft(1), grass_tuft(-1)]),
        ("foliage", [foliage()]),
        ("flower_poppy", [flower_poppy(0), flower_poppy(1)]),
        ("flower_daisy", [flower_daisy()]),
    ]:
        path = os.path.join(out_dir, name + ".vox")
        write_vox(path, size, frames)
        written.append((path, len(frames), sum(len(f) for f in frames)))

    for path, nf, nv in written:
        print(f"wrote {path} ({nf} frame(s), {nv} voxels)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
