"""Generate assets/items/mace.{vox,json} — the answer to plate.

WHY A MACE EXISTS AT ALL. Until 2026-09-15 every weapon in this engine arrived
as a KERF: an item carried one `damage` float and `MeleeSweepDamage` could do
exactly one thing with a body it met. Iron is authored at hardness 160 against
skin's 8, so `gear.cutHardnessRef` scales a blade's slot to a twentieth of
itself on a cuirass and armour was, correctly, almost proof against a sword —
and, incorrectly, proof against everything. Armour that cannot be answered is
not a mechanic, it is a wall.

A strike is now three parts (src/game/impact.h), and this is the weapon that is
made of the second one: `blunt` 16 against `damage` (the cut part) 2. It breaks
plate in as real geometry (`armorBreak` 0.8) and transmits `gear.bluntThrough`
of the blow to the limb underneath whether or not it did — the plate deforming
IS how the energy arrives — and on bare flesh it dents rather than cuts
(`bluntCarve` 0.6, inside a scope that refuses the collapse sever, so a mace can
cave a skull in and can never take an arm off).

TWO ART DECISIONS, BOTH MECHANICAL:

 1. `edge` IS THE HEAD'S SPAN, not the whole weapon. `MeleeSweepDamage` sweeps
    that segment and nothing else, so authoring the haft into it would make the
    shaft as dangerous as the flanges — and a mace's whole character is that
    only the last quarter of it hurts.

 2. NO `flat`. `MeleeEdgeAlign` returns 1 when a weapon declares no flat, which
    is exactly right here: a mace has no edge to lay along the line of travel,
    so it does the same damage whichever way it is rolled. On a sword that
    field is what makes a badly-rolled cut a slap; a club cannot be slapped
    with. Do NOT add one "for completeness" — it would silently halve the
    weapon.

Everything else is gen_sword_item.py's shape, deliberately: same .vox writer,
same hilt-box convention (the runtime puts the box's centre on the rig's
socket, so the placement needs no tuned constant), same -90 about Y in the grip
to point the head away from the body. Re-read that file's notes before changing
any of it; the traps it documents are all still here.

Run: python scripts/gen_mace_item.py
"""

import json
import os
import struct

# ---- palette (assets/prefabs — palette index == material ID) ----------------
# Same two as the sword, and asserted against materials.json in main() rather
# than trusted: a renumbered material turns the head into whatever took its
# slot, and the symptom is a weapon that behaves correctly and looks wrong.
STEEL = 57      # the head and the flanges
GRIP = 58       # grip_leather, the wrapped haft
STEEL_ID = "steel"
GRIP_ID = "grip_leather"

SCALE = 4       # micro voxels per world voxel, matching the mina rig

# ---- dimensions, micro units ------------------------------------------------
# A mace is SHORTER than an arming sword and most of its mass is at one end.
# 32 micro = 8 world voxels overall, against the sword's 11.
MACE_LEN = 32           # butt to crown
MACE_GRIP = 14          # micro of wrapped haft behind the head
MACE_HEAD = 10          # micro of head (flanged section)
MACE_HALF_W = 4         # head half-width at the widest flange
HAFT_R = 1.2            # haft radius, micro
FLANGES = 4             # how many vanes stand out of the head

assert MACE_GRIP + MACE_HEAD <= MACE_LEN, "head runs off the end of the haft"


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
    so no builder has to invent its own convention. Verbatim from
    gen_sword_item.py; the two files must agree about what a round thing is."""
    if rx <= 0 or ry <= 0:
        return False
    dx = (x + 0.5 - cx) / rx
    dy = (y + 0.5 - cy) / ry
    return dx * dx + dy * dy <= 1.0


# ---- the weapon -------------------------------------------------------------
def mace_vox(size):
    """A wrapped haft with a flanged head at HIGH x, pommel at LOW x.

    Same handedness rule as the sword: an item's own box has no wearer, so it
    is authored butt-first and the GRIP ROTATION is what aims it. Keeping a
    mirror here would mean the art and the grip both encoded "which way is
    outboard", and they would disagree the next time either changed.

    THE HEAD IS ROUND IN CROSS-SECTION, not thin like a blade. That is the
    readable difference between the two weapons at a glance, and it is also
    what makes "no flat" honest: there is no thin axis to lay along a cut.
    """
    sx, sy, sz = size
    out = []
    cy, cz = sy * 0.5, sz * 0.5
    head_x0 = MACE_GRIP
    head_x1 = MACE_GRIP + MACE_HEAD

    for x in range(sx):
        if x < 2:
            # Pommel: a steel counterweight, so the weapon balances at the hand
            # rather than dragging the point down. Wider than the haft.
            for z in range(sz):
                for y in range(sy):
                    if ellipse_mask(y, z, cy, cz, 1.9, 1.9):
                        out.append((x, y, z, STEEL))
        elif x < head_x0:
            # The wrapped haft. Round and thin: this is the part that must NOT
            # be in the `edge` segment below.
            for z in range(sz):
                for y in range(sy):
                    if ellipse_mask(y, z, cy, cz, HAFT_R, HAFT_R):
                        out.append((x, y, z, GRIP))
        elif x < head_x1:
            # ---- the head ----------------------------------------------------
            # A drum, plus FLANGES standing out of it. The drum swells toward
            # the middle of the head (`t` is 0 at both ends), which is what
            # makes it read as a mace rather than as a pipe, and the vanes are
            # a cross laid on it -- four half-width spokes in the y/z plane,
            # tapering to the crown so the silhouette comes to a blunt point.
            t = (x - head_x0) / max(head_x1 - 1 - head_x0, 1)
            swell = 1.0 - 0.55 * (2.0 * t - 1.0) ** 2   # 0.45 .. 1 .. 0.45
            drum = 2.1 + 0.6 * swell
            vane = MACE_HALF_W * (0.55 + 0.45 * swell)
            for z in range(sz):
                for y in range(sy):
                    dy, dz = y + 0.5 - cy, z + 0.5 - cz
                    if dy * dy + dz * dz <= drum * drum:
                        out.append((x, y, z, STEEL))
                        continue
                    # The vanes: a cross in y/z. `FLANGES` is 4 and the two
                    # axes are what four of them means; a different count would
                    # want a real angular test, and this weapon does not.
                    assert FLANGES == 4, "the cross below is four vanes by hand"
                    if abs(dz) <= 1.0 and abs(dy) <= vane:
                        out.append((x, y, z, STEEL))
                    elif abs(dy) <= 1.0 and abs(dz) <= vane:
                        out.append((x, y, z, STEEL))
        else:
            # The crown: a short steel cap past the flanges, rounded off.
            t = (x - head_x1) / max(sx - 1 - head_x1, 1)
            r = 2.2 * (1.0 - 0.7 * t * t)
            for z in range(sz):
                for y in range(sy):
                    if ellipse_mask(y, z, cy, cz, r, r):
                        out.append((x, y, z, STEEL))
    return out


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "assets", "items")
    os.makedirs(out_dir, exist_ok=True)

    # PALETTE INDEX == MATERIAL ID, asserted rather than trusted. The same
    # check gen_stock_armor.py makes, for the same reason: a renumbered
    # material silently turns this into a weapon made of something else.
    with open(os.path.join(root, "assets", "materials", "materials.json")) as mf:
        mats = json.load(mf)["materials"]
    for idx, want in ((STEEL, STEEL_ID), (GRIP, GRIP_ID)):
        assert idx <= len(mats) and mats[idx - 1]["id"] == want, (
            f"materials.json[{idx - 1}] is "
            f"{mats[idx - 1]['id'] if idx <= len(mats) else '(missing)'!r}, not "
            f"{want!r} — the palette index this item is authored against has "
            f"moved, and the mace would load as another material")

    size = (MACE_LEN, 2 * MACE_HALF_W, 2 * MACE_HALF_W)
    voxels = mace_vox(size)
    assert voxels, "mace generated no voxels"
    # DebrisVoxel is int8 and these are MICRO units, so a part may be at most
    # 120 micro = 30 world voxels on an axis. Same ceiling the rig asserts.
    assert max(size) <= 120, "mace exceeds DebrisVoxel int8 range"

    seen = set()
    uniq = []
    for v in voxels:
        x, y, z, _c = v
        assert 0 <= x < size[0] and 0 <= y < size[1] and 0 <= z < size[2], \
            f"mace voxel {(x, y, z)} outside declared size {size}"
        if (x, y, z) in seen:
            continue          # first write wins, as in the rig generator
        seen.add((x, y, z))
        uniq.append(v)

    body = model_chunks(size, uniq)
    pivot = (size[0] // 2, size[1] // 2, size[2] // 2)
    graph = ntrn(0, "", 1, (0, 0, 0)) + ngrp(1, [2])
    graph += ntrn(2, "mace", 3, pivot) + nshp(3, 0)

    payload = body + graph
    data = b"VOX " + struct.pack("<i", 150)
    data += b"MAIN" + struct.pack("<ii", 0, len(payload)) + payload
    with open(os.path.join(out_dir, "mace.vox"), "wb") as f:
        f.write(data)

    sidecar = {
        "comment": (
            "The mace: a standalone item, held by BORROWING a rig slot exactly "
            "as the sword is. Its whole reason to exist is the BLUNT half of "
            "the impact model (src/game/impact.h) — it goes through plate a "
            "sword skates off, and it dents flesh instead of opening it. Two "
            "art facts are mechanical and are emitted from the same constants "
            "that build the mesh: `edge` is the HEAD's span only (the haft "
            "must not cut), and there is deliberately NO `flat` — a mace has "
            "no edge to align, so MeleeEdgeAlign returns 1 and a badly-rolled "
            "swing does full damage. Regenerate with scripts/gen_mace_item.py."),
        "name": "mace",
        "model": "mace",
        # THE AUTHORED RESOLUTION, in the units the loader actually reads, and
        # NOT a bare `"scale"`. A bare scale still loads — melee.cpp's legacy
        # branch multiplies it by kLegacyAuthoringVoxelsPerMetre and WARNS —
        # but emitting the derived number is how a generator's output stops
        # drifting from what the loader wants, and every shipped sibling
        # (sword, cleaver, dagger, shortsword) already reads 40.
        "artVoxelsPerMetre": SCALE * 10,
        # Heavier than the sword and harder to knock out of a hand: there is no
        # edge to catch on, and a parry with a mace is a shove.
        "hp": 40,
        "severable": True,
        "severImpactSpeed": 9.0,
        "grip": {
            # -90 about Y carries the authored +x onto +Z (forward), which is
            # the same verified rotation the sword uses — see gen_sword_item.py
            # for the geometry.py check that proves it.
            "held_right": {
                "translation": [0, 0, 0],
                "rotation": [0, -90, 0],
                "scale": 1.0,
            }
        },
        # WHERE THE FIST CLOSES: the wrapped haft between the pommel knob and
        # the head, which is exactly the span mace_vox() fills with GRIP
        # leather. The runtime puts this box's centre on the rig's socket, so
        # the placement needs no tuned constant at all.
        "hilt": {
            "min": [2, 0, 0],
            "size": [MACE_GRIP - 2, 2 * MACE_HALF_W, 2 * MACE_HALF_W],
        },
        # THE HEAD, AND ONLY THE HEAD. MeleeSweepDamage tiles this segment with
        # probes and nothing outside it can hit anything — which is what makes
        # "only the last quarter of a mace hurts" geometry rather than a rule.
        #
        # NO "flat" KEY. See the docstring: MeleeEdgeAlign returns 1 for a
        # weapon that declares none, and that is the correct answer for a club.
        "edge": {
            "from": MACE_GRIP,
            "to": MACE_LEN,
            "axis": [1, 0, 0],
            "halfWidth": MACE_HALF_W,
        },
        # Less jiggle than the sword: a mace is a bar of metal, and a floppy
        # one reads as rubber.
        "spring": {"halflife": 0.07, "gain": 0.25, "maxAngle": 0.10},
    }
    with open(os.path.join(out_dir, "mace.json"), "w") as f:
        json.dump(sidecar, f, indent=2)
        f.write("\n")

    print(f"wrote {out_dir}/mace.vox  ({len(uniq)} voxels, size {size})")
    print(f"wrote {out_dir}/mace.json")
    print(f"  haft {MACE_GRIP} micro, head {MACE_GRIP}..{MACE_LEN} micro "
          f"= {MACE_LEN / SCALE} world voxels overall, edge is the head only")


if __name__ == "__main__":
    main()
