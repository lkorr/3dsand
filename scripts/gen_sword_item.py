"""Generate assets/items/sword.{vox,json} — the sword as a STANDALONE ITEM.

WHY THIS FILE EXISTS AT ALL. The sword used to be a limb of the wearer's rig
(a `"tag": "prop"` entry in gen_mina.py's LIMBS, parented to hand.R). That is
where it broke: prefab-local space has its origin at the BODY's min corner —
props are deliberately excluded from that measurement, because a creature's
size must not change with what it happens to be carrying — so when the
handedness fix moved the right hand to model -X, the blade followed it to
engine x -30..14. Thirty micro of blade sat at NEGATIVE prefab-local
coordinates, which that space cannot represent. On screen the body rendered
offset from where the arm actually solved.

An item is not a part of the creature, so it does not belong in the creature's
box. It gets its own .vox and its own origin, and the rig BORROWS A SLOT to
show it. Everything the old arrangement bought — severing with the parent
limb, dropping to debris, per-voxel carving, micro detail — is preserved by
the item's runtime entity filling a real rig Part, which is what src/game/
item.cpp does with the `grip` block below.

THE ART IS UNCHANGED. The geometry here is the same builder that lived in
gen_mina.py (sword_vox), moved verbatim except for the mirror: a rig prop had
to be built pointing along the wearer's -X, but an item's own box has no
handedness, so it is authored pommel-at-low-x, tip-at-high-x — the natural
reading — and the GRIP ROTATION is what points it outboard when held. That is
the whole reason the offset lives on the item rather than on the skeleton.

TWO CONSTRAINTS INHERITED FROM THE RIG VERSION, both still load-bearing:

 1. The blade lies along the box's LONG axis (x here), and when held it must
    end up ORTHOGONAL TO THE FOREARM. The arm extends along -y in the rig,
    so the blade's +x is already perpendicular — identity grip rotation is
    correct. The angle must HOLD through a swing: the arm moves, the wrist
    does not swivel.
 2. The `edge` block is the ONE source of truth for where the weapon cuts.
    melee.cpp sweeps that segment; re-measuring it by eye in C++ would rot the
    moment the art changes. It is emitted from the same constants that build
    the mesh, so art and hitbox can only move together.

Run: python scripts/gen_sword_item.py
"""

import json
import os
import struct

# ---- palette (assets/prefabs — palette index == material ID) ----------------
STEEL = 57      # the blade, guard and pommel
GRIP = 58       # grip_leather, the wrapped hilt

SCALE = 4       # micro voxels per world voxel, matching the mina rig

# ---- blade dimensions, micro units ------------------------------------------
SWORD_LEN = 44          # pommel butt to tip: 11 world voxels
SWORD_GRIP = 10         # micro of hilt behind the guard
SWORD_GUARD = 2         # micro of crossguard
SWORD_HALF_W = 3        # blade half-width at the widest


# ---- .vox writing (same helpers as gen_mina.py / gen_wizard.py) -------------
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
    so no builder has to invent its own convention."""
    if rx <= 0 or ry <= 0:
        return False
    dx = (x + 0.5 - cx) / rx
    dy = (y + 0.5 - cy) / ry
    return dx * dx + dy * dy <= 1.0


# ---- the blade --------------------------------------------------------------
def sword_vox(size):
    """A straight, tapering, double-edged blade on a wrapped hilt, lying along
    the box's X axis with the pommel at LOW x and the tip at HIGH x. Thin in Z
    (the flat of the blade), so it reads as a blade rather than a bar, and the
    taper is what makes the tip the part that bites.

    NO MIRROR, unlike the rig-prop version this came from. That one was flipped
    so the blade pointed along the wearer's -X; an item's own box has no
    handedness and no wearer, so the natural pommel-first ordering stands and
    the grip rotation is what aims it. Keeping the flip here would mean the
    art and the grip both encoded "which way is outboard", and they would
    disagree the next time either changed."""
    sx, sy, sz = size
    out = []
    cy, cz = sy * 0.5, sz * 0.5
    guard_x = SWORD_GRIP + SWORD_GUARD

    for x in range(sx):
        if x < 2:
            # pommel: a squat steel knob, wider than the grip
            for z in range(sz):
                for y in range(sy):
                    if ellipse_mask(y, z, cy, cz, 1.6, 1.6):
                        out.append((x, y, z, STEEL))
        elif x < SWORD_GRIP:
            # wrapped grip
            for z in range(sz):
                for y in range(sy):
                    if ellipse_mask(y, z, cy, cz, 1.1, 1.1):
                        out.append((x, y, z, GRIP))
        elif x < guard_x:
            # crossguard: a bar across Y, thin in Z
            for z in range(sz):
                for y in range(sy):
                    if ellipse_mask(y, z, cy, cz, sy * 0.5, 1.0):
                        out.append((x, y, z, STEEL))
        else:
            # blade: full width at the ricasso, tapering to a point. Thin in Z
            # (half-thickness 1) so the flat reads flat.
            t = (x - guard_x) / max(sx - 1 - guard_x, 1)
            hw = SWORD_HALF_W * (1.0 - 0.65 * t * t)
            for z in range(sz):
                for y in range(sy):
                    if ellipse_mask(y, z, cy, cz, hw, 1.0):
                        out.append((x, y, z, STEEL))
    return out


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "assets", "items")
    os.makedirs(out_dir, exist_ok=True)

    size = (SWORD_LEN, 2 * SWORD_HALF_W, 4)
    voxels = sword_vox(size)
    assert voxels, "sword generated no voxels"
    # DebrisVoxel is int8 and these are MICRO units, so a part may be at most
    # 120 micro = 30 world voxels on an axis. Same ceiling the rig asserts.
    assert max(size) <= 120, "sword exceeds DebrisVoxel int8 range"

    seen = set()
    uniq = []
    for v in voxels:
        x, y, z, _c = v
        assert 0 <= x < size[0] and 0 <= y < size[1] and 0 <= z < size[2], \
            f"sword voxel {(x, y, z)} outside declared size {size}"
        if (x, y, z) in seen:
            continue          # first write wins, as in the rig generator
        seen.add((x, y, z))
        uniq.append(v)

    # ONE model, named for the item. The loader reads a single-model .vox the
    # same way it reads a rig's per-limb models; the name is what item.cpp
    # looks up, so it must match the sidecar's "model".
    body = model_chunks(size, uniq)
    pivot = (size[0] // 2, size[1] // 2, size[2] // 2)
    graph = ntrn(0, "", 1, (0, 0, 0)) + ngrp(1, [2])
    graph += ntrn(2, "sword", 3, pivot) + nshp(3, 0)

    payload = body + graph
    data = b"VOX " + struct.pack("<i", 150)
    data += b"MAIN" + struct.pack("<ii", 0, len(payload)) + payload
    with open(os.path.join(out_dir, "sword.vox"), "wb") as f:
        f.write(data)

    # ---- the sidecar --------------------------------------------------------
    #
    # GRIP IS A MAP OF CONTEXTS, not a single offset, and only held_right is
    # populated today. Minecraft carries nine display slots because one offset
    # never covers held/ground/GUI/head; making this a map now means adding
    # held_left or ground is DATA rather than a schema migration. Two of its
    # rules are stolen verbatim because both are the kind of thing you only
    # learn by getting them wrong:
    #
    #   * TRANSLATION APPLIES BEFORE ROTATION.
    #   * A CONTEXT THAT OMITS A SUB-KEY DOES NOT INHERIT IT. Be explicit.
    #
    # The composition is handSocket x grip: the rig says where the hand's grip
    # point is, the item says how it sits in that grip. Explicitly NOT the
    # inverse form (Inverse(grip) x socket) that VR rigs use — that inverts
    # because in VR the HAND POSE is the constraint being solved for, which is
    # not the case here, and it drags in a scale hazard.
    #
    # ROTATION, degrees, applied X then Y then Z. The blade is authored along
    # +x; the arm extends along -y in the rig.
    #
    # -90 ABOUT Y POINTS THE BLADE AWAY FROM THE CHARACTER. Heading 0 faces +Z,
    # and -90 about Y carries the authored +x onto +Z, so the sword points out
    # front rather than across the body. Verified rather than reasoned:
    #
    #   python scripts/geometry.py rotate_point 0 1 0 -90 -- 1 0 0
    #   -> [0, 0, 1]                     (blade +x lands on +Z, i.e. forward)
    #
    # Constraint 1 in this file's docstring still holds: a rotation about Y
    # keeps the blade horizontal, so it stays perpendicular to a forearm that
    # runs along -y. The angle is carried by the grip, not by the art, so the
    # blade is still authored pommel-at-low-x and nothing about the mesh moves.
    #
    # TRANSLATION is a RESIDUAL NUDGE in MICRO units, and is zero here. Where
    # the fist closes is stated by the `hilt` box below, and the runtime puts
    # that box's centre on the rig's socket — so the placement needs no tuned
    # constant at all.
    #
    # IT USED TO BE [-SWORD_GRIP, 0, 0], AND THAT WAS THE BUG. It was chosen as
    # if the socket sat at the pommel butt, but the socket is the CENTRE of the
    # hand's limb box, and mina's hand is one world voxel across. -10 micro is
    # -2.5 world voxels, so the item's origin was parked two and a half voxels
    # outboard of a one-voxel fist and the blade — which grows from that origin
    # along +x — hung down and away from the hand. It also never centred the
    # blade's cross-section in y or z, so even the intended axis was off by
    # half the guard. Aligning boxes removes all three errors at once.
    sidecar = {
        "comment": (
            "The sword as a standalone item: its own art, its own origin. "
            "Held by BORROWING a rig slot (see src/game/item.cpp), so it is a "
            "real rig Part while worn and severs, drops, burns and carves "
            "exactly like a limb. Regenerate with scripts/gen_sword_item.py — "
            "the edge block is emitted from the same constants that build the "
            "mesh, so the hitbox cannot drift from the art."),
        "name": "sword",
        "model": "sword",
        "scale": SCALE,
        "hp": 30,
        "severable": True,
        "severImpactSpeed": 7.0,
        "grip": {
            "held_right": {
                "translation": [0, 0, 0],
                "rotation": [0, -90, 0],
                "scale": 1.0,
            }
        },
        # WHERE THE FIST CLOSES, in the item's own frame and micro units — the
        # wrapped grip between the pommel knob and the crossguard, which is
        # exactly the span sword_vox() fills with GRIP leather. Emitted from
        # the same constants that build the mesh, for the same reason the edge
        # block is: re-measuring it by eye would rot the moment the art moved.
        #
        # This is the item's answer to the question the LIMB SYSTEM answers for
        # a hand. The rig states what a hand is by declaring a box; the item
        # states where its hilt is by declaring a box; the runtime puts the two
        # centres together (src/game/avatar.cpp EquipItem). Neither side is a
        # tuned number, so neither can drift.
        #
        # x: the leather runs from the pommel (x < 2 is the knob) to the guard.
        # y/z: the full cross-section, so the centre lands on the blade's axis
        #      rather than on its corner — the omission that left the old
        #      placement off by half the guard even along the axis it did set.
        "hilt": {
            "min": [2, 0, 0],
            "size": [SWORD_GRIP - 2, 2 * SWORD_HALF_W, 4],
        },
        # The cutting edge, in the ITEM's own local frame and micro units,
        # from the ricasso (where the sharpened part starts, just past the
        # guard) to the tip. Authored along +x because that is how the art is
        # built — the grip rotation carries it wherever the hand goes.
        "edge": {
            "from": SWORD_GRIP + SWORD_GUARD,
            "to": SWORD_LEN,
            "axis": [1, 0, 0],
            # WHICH WAY THE FLAT FACES. sword_vox builds the blade wide in y
            # and THIN IN Z, so +z is the face — the same constant the mesh is
            # cut from, for the same reason `from`/`to` are emitted here rather
            # than measured by eye in C++. The stroke driver rolls the blade so
            # the edge leads the cut and the damage sweep scales a hit by how
            # edge-on it was; both read this, and neither can derive it.
            "flat": [0, 0, 1],
            "halfWidth": SWORD_HALF_W,
        },
        # Jiggle, carried over from the rig prop: a held weapon that is welded
        # rigid to the fist reads as part of the arm.
        "spring": {"halflife": 0.09, "gain": 0.35, "maxAngle": 0.15},
    }
    with open(os.path.join(out_dir, "sword.json"), "w") as f:
        json.dump(sidecar, f, indent=2)
        f.write("\n")

    print(f"wrote {out_dir}/sword.vox  ({len(uniq)} voxels, size {size})")
    print(f"wrote {out_dir}/sword.json")
    print(f"  blade {SWORD_LEN} micro = {SWORD_LEN / SCALE} world voxels, "
          f"edge {SWORD_GRIP + SWORD_GUARD}..{SWORD_LEN}")


if __name__ == "__main__":
    main()
