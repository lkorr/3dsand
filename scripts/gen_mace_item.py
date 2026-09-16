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
    shaft as dangerous as the ball — and a mace's whole character is that only
    the last quarter of it hurts.

 2. NO `flat`. `MeleeEdgeAlign` returns 1 when a weapon declares no flat, which
    is exactly right here: a mace has no edge to lay along the line of travel,
    so it does the same damage whichever way it is rolled. On a sword that
    field is what makes a badly-rolled cut a slap; a club cannot be slapped
    with. Do NOT add one "for completeness" — it would silently halve the
    weapon.

WHY THE ART IS TWO SHAPES AND NOT SIX (2026-09-16). The first version of this
weapon was a leather-wrapped haft, a steel pommel knob, a swelling drum, four
cross flanges and a rounded crown — five silhouettes stacked inside a head that
is NINE MICRO VOXELS ACROSS. At that budget a flange is one voxel wide, which is
indistinguishable from the drum it stands on, and the whole head read as a grey
smear with a bobble at each end. It is now what the owner asked for in one line:
a plain wood stick socketed into a steel ball. Two materials, two primitives,
and a silhouette that survives being four pixels tall on screen.

The corollary, for anyone tempted to put the spikes back: a morning star's studs
would need to stand at least two micro clear of the ball to read, which is a
ball of radius ~7 in a 16-wide box — a head nearly as wide as the weapon is
long. Detail below the voxel budget is not detail, it is noise.

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
# Asserted against materials.json in main() rather than trusted: a renumbered
# material turns the ball into whatever took its slot, and the symptom is a
# weapon that behaves correctly and looks wrong.
#
# The stick is `staff_wood` (54) and NOT `wood` (2), for the reason that
# material's own note in materials.json gives at length: staff_wood is the
# TINTED one, placed only by prefabs and items, so its state nibble is a finish
# picker nobody else writes. `wood` is what every tree in the world is made of
# and worldgen scribbles `rnd % 3` into its nibble. The wizard's staff is
# authored against the same slot (scripts/gen_wizard.py).
STEEL = 57      # the ball
WOOD = 54       # staff_wood, the stick
STEEL_ID = "steel"
WOOD_ID = "staff_wood"

# ---- resolution -------------------------------------------------------------
# THE WEAPON WAS HALVED BY CHANGING THIS, NOT BY SHRINKING THE ART (2026-09-16).
# The first ball was 0.23 m across — wider than a human head — and read as
# comical in the hand. The obvious fix, halving the art dimensions at the
# weapons' usual 40/m, does not work: a stick has to be 4 art voxels across to
# render as a stick rather than a dotted line, so a ball half the size would be
# 5 across and the two shapes would be indistinguishable. Detail below the
# voxel budget is not detail; see the docstring.
#
# So the mace is authored at 80 art voxels per metre instead — 1.25 cm units,
# the SAME resolution as the human body and every garment and plate in
# assets/items. The art grid below is unchanged in shape and the physical size
# it maps to halves. `SkinScaleFor` accepts it exactly (80 / kVoxelsPerMetre 10
# = 8 micro per world voxel, no upsample), and the hilt/edge spans are art
# units that `ItemDef::ArtToWorld` converts through the same factor, so nothing
# drifts.
#
# IF THE SIZE IS EVER TUNED AGAIN, TUNE IT HERE OR IN MACE_LEN — NOT IN THE
# TUNER'S MODEL EDITOR. Rescaling the .vox box there moves the geometry out
# from under the `hilt` block, whose CENTRE is what the runtime puts on the
# rig's socket; the symptom is a weapon floating beside the fist, and it is
# what happened on 2026-09-16.
SCALE = 8       # micro voxels per world voxel; artVoxelsPerMetre = SCALE * 10

# ---- dimensions, micro units ------------------------------------------------
# One micro is 1.25 cm. 44 micro = 0.55 m overall, against the sword's 1.2 m:
# a one-handed morning star, short because nearly all of its mass is at one end.
MACE_LEN = 44           # butt to the crown of the ball
MACE_GRIP = 14          # micro of stick the fist can close on (the hilt box)
MACE_HALF_W = 5         # half the box in y/z — the ball at its equator
BALL_R = 4.6            # steel ball radius, micro (0.115 m across — half a head)
BALL_CX = 39.0          # the ball's centre along x
HAFT_R = 1.7            # stick radius: the smallest that fills a 4-micro cross
                        # under ellipse_mask. 4 micro is 5 cm of haft, and the
                        # ball is 2.3x that — the ratio the silhouette needs.

assert BALL_CX + BALL_R >= MACE_LEN - 1, "the ball does not reach the crown"
assert BALL_CX - BALL_R > MACE_GRIP, "the ball swallows the grip"
assert BALL_R <= MACE_HALF_W, "the ball is wider than the box that holds it"


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
    """A plain wood stick with a steel ball on the HIGH-x end.

    Same handedness rule as the sword: an item's own box has no wearer, so it
    is authored butt-first and the GRIP ROTATION is what aims it. Keeping a
    mirror here would mean the art and the grip both encoded "which way is
    outboard", and they would disagree the next time either changed.

    THE HEAD IS A SPHERE, not a thin blade. That is the readable difference
    between the two weapons at a glance — one silhouette each, at any distance —
    and it is also what makes "no flat" honest: there is no thin axis to lay
    along a cut.

    The stick runs all the way to the ball's CENTRE rather than stopping at its
    surface, so the two never separate however the ball's radius is retuned.
    The buried part is simply overwritten by steel below.
    """
    sx, sy, sz = size
    out = []
    cy, cz = sy * 0.5, sz * 0.5
    r2 = BALL_R * BALL_R

    for x in range(sx):
        dx = x + 0.5 - BALL_CX
        for z in range(sz):
            dz = z + 0.5 - cz
            for y in range(sy):
                dy = y + 0.5 - cy
                # The ball wins wherever the two overlap: same first-write-wins
                # convention as the rig generator, stated as an order here
                # rather than left to the dedupe pass in main().
                if dx * dx + dy * dy + dz * dz <= r2:
                    out.append((x, y, z, STEEL))
                elif dx <= 0.0 and ellipse_mask(y, z, cy, cz, HAFT_R, HAFT_R):
                    out.append((x, y, z, WOOD))
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
    for idx, want in ((STEEL, STEEL_ID), (WOOD, WOOD_ID)):
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
            "The mace: a morning star, a plain wood stick socketed into a "
            "steel ball. A standalone item, held by BORROWING a rig slot "
            "exactly as the sword is. Its whole reason to exist is the BLUNT "
            "half of the impact model (src/game/impact.h) — it goes through "
            "plate a sword skates off, and it dents flesh instead of opening "
            "it. Two art facts are mechanical and are emitted from the same "
            "constants that build the mesh: `edge` is the BALL's span only "
            "(the stick must not cut), and there is deliberately NO `flat` — a "
            "mace has no edge to align, so MeleeEdgeAlign returns 1 and a "
            "badly-rolled swing does full damage. Regenerate with "
            "scripts/gen_mace_item.py."),
        "name": "mace",
        "model": "mace",
        # THE AUTHORED RESOLUTION, in the units the loader actually reads, and
        # NOT a bare `"scale"`. A bare scale still loads — melee.cpp's legacy
        # branch multiplies it by kLegacyAuthoringVoxelsPerMetre and WARNS —
        # but emitting the derived number is how a generator's output stops
        # drifting from what the loader wants.
        #
        # 80, where the other weapons read 40. That is DELIBERATE and it is how
        # this weapon got smaller — see the SCALE note above. It is not an odd
        # number: the human body, and every garment and plate that has to land
        # in the body's frame, is already authored at 80.
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
        # WHERE THE FIST CLOSES: the low end of the stick, well clear of the
        # ball. Its centre is x = 8 — the same place the flanged version put it,
        # so the held pose did not move when the art was redrawn. The runtime
        # puts this box's centre on the rig's socket, so the placement needs no
        # tuned constant at all.
        "hilt": {
            "min": [2, 0, 0],
            "size": [MACE_GRIP - 2, 2 * MACE_HALF_W, 2 * MACE_HALF_W],
        },
        # THE BALL, AND ONLY THE BALL. MeleeSweepDamage tiles this segment with
        # probes and nothing outside it can hit anything — which is what makes
        # "only the last quarter of a mace hurts" geometry rather than a rule.
        # Emitted from BALL_CX/BALL_R so the hitbox cannot drift from the art.
        #
        # NO "flat" KEY. See the docstring: MeleeEdgeAlign returns 1 for a
        # weapon that declares none, and that is the correct answer for a club.
        "edge": {
            "from": int(BALL_CX - BALL_R),
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
    print(f"  stick 0..{BALL_CX - BALL_R:.0f} micro, ball r{BALL_R} at "
          f"x={BALL_CX} = {MACE_LEN / SCALE} world voxels overall, "
          f"edge is the ball only")


if __name__ == "__main__":
    main()
