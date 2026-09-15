"""Generate assets/items/shortsword.{vox,json} and dagger.{vox,json} — the
SMALL end of the melee range.

ONE SCRIPT, TWO ITEMS, unlike gen_sword_item.py / gen_cleaver_item.py which are
one file each. Those two are different weapons: a straight double-edged blade
and a single-edged chopper that widens toward the tip, and each builder is its
own argument about where the mass goes. These two are THE SAME BLADE AT TWO
SIZES — a shortsword is an arming sword's section on a shorter bar, and a
dagger is that again — so splitting them would mean two copies of one builder
drifting apart, which is exactly the failure mode the `edge`-emitted-from-mesh
rule exists to prevent. The parameters that actually differ are the BLADES
table below; everything else is shared by construction.

WHY SMALL WEAPONS EXIST. The wound model (src/game/mob.h BladeCut, sim/tuning.h
§E) scales a cut by the weapon's HEFT, derived from the item's own voxel volume
(item.h ItemDef::heftVolume), and HeftFactor floors at 0.2 with the comment
"a dagger SHOULD cut shallower than an arming sword". Until now there was no
dagger: the library ran sword (1.0x) and cleaver (1.85x), so the whole lower
half of the curve — including the floor — was unreachable content. These two
land at roughly 0.5x and 0.2x, which puts a real weapon on either side of the
reference instead of only above it.

THE FOUR CONSTRAINTS INHERITED FROM gen_sword_item.py all still hold and are
not restated here: the blade lies along the box's long axis and is aimed by the
grip rotation (never by mirroring the art), the `edge` and `hilt` blocks are
emitted from the same constants that build the mesh so the hitbox cannot drift
from it, the `flat` axis is the thin one and only this generator knows which
that is, and translation is a residual nudge that a well-authored item leaves
at zero because the runtime aligns the hilt box's centre with the hand socket.

Run: python scripts/gen_shortblade_items.py
"""

import json
import os
import struct

# ---- palette (assets/prefabs — palette index == material ID) ----------------
STEEL = 57      # the blade, guard and pommel
GRIP = 58       # grip_leather, the wrapped hilt

# Art voxels per METRE. The same 40 the sword and the cleaver are authored at,
# so all four weapons land on one micro lattice at any world scale and their
# derived hefts are comparable as pure geometry (DESIGN.md §3b — never author a
# bare `scale`).
ART_VOXELS_PER_METRE = 40

# The arming sword's derived volume in world voxels (gore.woundHeftRef), for
# the printed heft ratio only. Nothing reads it at runtime — the loader
# measures each item off its own .vox — so it is a reporting aid, not truth.
HEFT_REF = 5.3


# ---- what makes each blade its own weapon -----------------------------------
#
# All lengths in MICRO units at 40 per metre, so `length` is metres * 40.
#
# `halfW` is the blade's half-width at the ricasso and `taper` how much of that
# is gone by the tip (hw = halfW * (1 - taper*t^2), the sword's own curve). A
# dagger tapers harder over a shorter run, which is what makes it read as a
# point with a blade behind it rather than as a small sword.
BLADES = [
    {
        "name": "shortsword",
        # 0.70 m overall against the arming sword's 1.10 m: a gladius/xiphos,
        # the blade you can still swing properly in a press.
        "length": 28,
        "grip": 7,
        "guard": 2,
        "halfW": 3,        # the sword's section, unchanged — it is not narrower
        "taper": 0.60,
        "pommel": 2,       # micro of steel knob at the butt
        "pommelR": 1.5,
        "gripR": 1.1,
        "hp": 20,
        # Knocked out of the hand more easily than the arming sword (7.0): less
        # blade means less of it in the fist.
        "severImpact": 6.0,
        # Quicker to settle than the sword's 0.09/0.35/0.15. Lighter things
        # stop ringing sooner and move further while they do.
        "spring": {"halflife": 0.07, "gain": 0.40, "maxAngle": 0.17},
    },
    {
        "name": "dagger",
        # 0.35 m overall, of which 0.22 m is blade. Below this the item stops
        # being a weapon the swing arc can express: the stroke driver sizes an
        # arc from `reach`, and a reach shorter than the arm is just a fist.
        "length": 14,
        "grip": 5,
        "guard": 1,
        "halfW": 2,
        "taper": 0.70,
        "pommel": 1,
        "pommelR": 1.2,
        "gripR": 0.95,
        "hp": 12,
        "severImpact": 4.5,
        "spring": {"halflife": 0.05, "gain": 0.45, "maxAngle": 0.20},
    },
]

# Behaviour numbers, for the items.json rows this script PRINTS but does not
# write. items.json is hand-authored content (it holds descriptions), so the
# generator states the geometry-derived half and leaves the prose to the file.
BEHAVIOUR = {
    # damage is per hit at full swing speed; reach is world voxels FROM THE
    # SHOULDER, so it is arm + weapon and never drops to the blade's own length
    # (the sword's 9.0 is 0.9 m of both). carveBonus widens the wound past the
    # blade's authored halfWidth — kept small, since the geometry is supposed
    # to decide the cut.
    "shortsword": {"damage": 10.0, "carveBonus": 0.10, "reach": 7.5},
    "dagger": {"damage": 6.5, "carveBonus": 0.04, "reach": 6.5},
}


# ---- .vox writing (same helpers as gen_sword_item.py) -----------------------
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


def ellipse_mask(x, y, cx, cy, rx, ry):
    """True when voxel (x,y)'s CENTRE is inside the axis-aligned ellipse. Voxel
    i covers [i, i+1) so its centre is i+0.5 — that half is added here, once,
    exactly as gen_sword_item.py does it."""
    if rx <= 0 or ry <= 0:
        return False
    dx = (x + 0.5 - cx) / rx
    dy = (y + 0.5 - cy) / ry
    return dx * dx + dy * dy <= 1.0


# ---- the blade --------------------------------------------------------------
def blade_vox(b, size):
    """A straight, tapering, double-edged blade on a wrapped hilt, lying along
    the box's X axis with the pommel at LOW x and the tip at HIGH x. Thin in Z
    (the flat), so the section reads as a blade rather than as a bar.

    Structurally gen_sword_item.py's sword_vox with its four constants pulled
    out into `b`. NO MIRROR, for the reason that file gives at length: an
    item's own box has no handedness, so the natural pommel-first ordering
    stands and the grip rotation is what aims it outboard."""
    sx, sy, sz = size
    out = []
    cy, cz = sy * 0.5, sz * 0.5
    guard_x = b["grip"] + b["guard"]

    for x in range(sx):
        if x < b["pommel"]:
            # pommel: a squat steel knob, wider than the grip, and most of why
            # a short blade balances in the hand at all
            for z in range(sz):
                for y in range(sy):
                    if ellipse_mask(y, z, cy, cz, b["pommelR"], b["pommelR"]):
                        out.append((x, y, z, STEEL))
        elif x < b["grip"]:
            # wrapped grip
            for z in range(sz):
                for y in range(sy):
                    if ellipse_mask(y, z, cy, cz, b["gripR"], b["gripR"]):
                        out.append((x, y, z, GRIP))
        elif x < guard_x:
            # crossguard: a bar across Y, thin in Z
            for z in range(sz):
                for y in range(sy):
                    if ellipse_mask(y, z, cy, cz, sy * 0.5, 1.0):
                        out.append((x, y, z, STEEL))
        else:
            # blade: full width at the ricasso, tapering to a point. Half
            # thickness 1 in z, so the flat reads flat.
            t = (x - guard_x) / max(sx - 1 - guard_x, 1)
            hw = b["halfW"] * (1.0 - b["taper"] * t * t)
            for z in range(sz):
                for y in range(sy):
                    if ellipse_mask(y, z, cy, cz, hw, 1.0):
                        out.append((x, y, z, STEEL))
    return out


def build(b, out_dir):
    name = b["name"]
    size = (b["length"], 2 * b["halfW"], 4)
    voxels = blade_vox(b, size)
    assert voxels, f"{name} generated no voxels"
    # DebrisVoxel is int8 and these are MICRO units, so a part may be at most
    # 120 micro on an axis. Same ceiling the rig and the sword assert — never
    # in danger here, and checked anyway so the assert does not rot away.
    assert max(size) <= 120, f"{name} exceeds DebrisVoxel int8 range"

    seen = set()
    uniq = []
    for v in voxels:
        x, y, z, _c = v
        assert 0 <= x < size[0] and 0 <= y < size[1] and 0 <= z < size[2], \
            f"{name} voxel {(x, y, z)} outside declared size {size}"
        if (x, y, z) in seen:
            continue          # first write wins, as in the sword generator
        seen.add((x, y, z))
        uniq.append(v)

    # ONE model, named for the item: the loader reads a single-model .vox the
    # same way it reads a rig's per-limb models, and the name is what the
    # sidecar's "model" key looks up.
    body = model_chunks(size, uniq)
    pivot = (size[0] // 2, size[1] // 2, size[2] // 2)
    graph = ntrn(0, "", 1, (0, 0, 0)) + ngrp(1, [2])
    graph += ntrn(2, name, 3, pivot) + nshp(3, 0)

    payload = body + graph
    data = b"VOX " + struct.pack("<i", 150)
    data += b"MAIN" + struct.pack("<ii", 0, len(payload)) + payload
    with open(os.path.join(out_dir, f"{name}.vox"), "wb") as f:
        f.write(data)

    scale = ART_VOXELS_PER_METRE // 10   # micro per world voxel at 10 vox/m
    metres = b["length"] / ART_VOXELS_PER_METRE
    sidecar = {
        "comment": (
            f"A {name}: the small end of the melee range, where the wound "
            "model's heft factor falls BELOW the arming sword instead of "
            "above it. Regenerate with scripts/gen_shortblade_items.py — the "
            "edge and hilt blocks are emitted from the same constants that "
            "build the mesh, so the hitbox cannot drift from the art. There "
            "is deliberately no \"heft\" key: heft is DERIVED from this "
            "file's own voxel volume (item.h ItemDef::heftVolume), so "
            "re-authoring the blade re-weighs it in the same edit."),
        "name": name,
        "model": name,
        "artVoxelsPerMetre": ART_VOXELS_PER_METRE,
        "hp": b["hp"],
        "severable": True,
        # See sim/tuning.h gore.woundImpactSeverScale — the item's durability
        # as a BORROWED RIG SLOT (how fast a hit knocks it out of the hand),
        # not the wound model's.
        "severImpactSpeed": b["severImpact"],
        # -90 about Y carries the authored +x onto +Z, which is heading 0, so
        # the blade points out front rather than across the body. Same as the
        # sword's, and verified the same way:
        #   python scripts/geometry.py rotate_point 0 1 0 -90 -- 1 0 0
        "grip": {
            "held_right": {
                "translation": [0, 0, 0],
                "rotation": [0, -90, 0],
                "scale": 1.0,
            }
        },
        # WHERE THE FIST CLOSES, in the item's own micro units: exactly the
        # span blade_vox fills with GRIP leather, between the pommel knob and
        # the guard. The runtime puts this box's CENTRE on the hand socket, so
        # nothing here is a tuned placement constant. y/z are the full
        # cross-section so the centre lands on the blade's axis rather than on
        # its corner.
        "hilt": {
            "min": [b["pommel"], 0, 0],
            "size": [b["grip"] - b["pommel"], 2 * b["halfW"], 4],
        },
        # The sharpened part, from the ricasso just past the guard to the tip,
        # in the item's own frame and micro units.
        "edge": {
            "from": b["grip"] + b["guard"],
            "to": b["length"],
            "axis": [1, 0, 0],
            # WHICH WAY THE FLAT FACES. blade_vox builds the section wide in y
            # and THIN IN Z, so +z is the face. Nothing can derive this at load
            # time — "which of the two axes across the blade is the thin one"
            # is a fact only this generator holds — and an item without it
            # falls back to an arbitrary perpendicular, which makes rolling the
            # blade into the cut worth nothing. That was a real bug on the
            # cleaver until 2026-09-14.
            "flat": [0, 0, 1],
            "halfWidth": b["halfW"],
        },
        "spring": b["spring"],
    }
    with open(os.path.join(out_dir, f"{name}.json"), "w") as f:
        json.dump(sidecar, f, indent=2)
        f.write("\n")

    vol = len(uniq) / float(scale ** 3)
    print(f"wrote {out_dir}/{name}.vox  ({len(uniq)} voxels, size {size})")
    print(f"wrote {out_dir}/{name}.json")
    print(f"  {b['length']} micro = {b['length'] / scale} world voxels "
          f"({metres:.2f} m), edge {b['grip'] + b['guard']}..{b['length']}")
    print(f"  DERIVED HEFT VOLUME {vol:.2f} world voxels "
          f"= {vol / HEFT_REF:.2f}x the arming sword (gore.woundHeftRef)")
    bh = BEHAVIOUR[name]
    print(f"  items.json row: damage {bh['damage']}, "
          f"carveBonus {bh['carveBonus']}, reach {bh['reach']}")


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "assets", "items")
    os.makedirs(out_dir, exist_ok=True)
    for b in BLADES:
        build(b, out_dir)


if __name__ == "__main__":
    main()
