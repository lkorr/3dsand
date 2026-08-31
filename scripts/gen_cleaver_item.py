"""Generate assets/items/cleaver.{vox,json} — the HEAVY end of the melee range.

WHY A SECOND WEAPON EXISTS AT ALL. The wound model (src/game/mob.h BladeCut,
sim/tuning.h §E) scales a cut's depth and length by the weapon's HEFT, and heft
is derived from the item's own voxel volume rather than authored — a greatsword
cuts deeper than a knife because it IS bigger (item.h ItemDef::heftVolume). A
mechanism with exactly one weapon in the library cannot be shown to work: the
`wound-heft` gate needs two items whose derived hefts differ, and the game
wants a big two-handed blade regardless.

SIZED AGAINST THE SWORD, DELIBERATELY. gore.woundHeftRef is the stock arming
sword's volume in world voxels (340 art voxels at scale 4 = 5.3), so heft 1.0
IS the sword and everything else is measured against it. This blade comes out
at 9.78 world voxels = 1.85x, measured by the loader off this file rather than
asserted here — and that is the number `wound-heft` records under
woundHeftCleaver, so a re-author shows up as a moved observation rather than as
a comment that quietly went stale.

The ceiling is gore.woundHeftMax (4.0). Deliberately not approached: a first
attempt at these proportions derived 3.75x, which is a weapon that parts a
human thigh in one blow at full commitment and leaves the wound model with
nothing to say.

SHAPE: a single-edged falchion/cleaver. No crossguard — the blade's own
shoulder is the stop — a long two-handed grip, and a profile that WIDENS
toward the tip, which is where the mass wants to be on a chopping weapon and
is also what makes the silhouette read as something other than a bigger sword.

The three constraints inherited from gen_sword_item.py all still hold and are
not restated here: the blade lies along the box's long axis and is aimed by the
grip rotation, the `edge` block is emitted from the same constants that build
the mesh so the hitbox cannot drift from the art, and the hilt box is what the
fist closes on so no placement constant is ever hand-tuned.

Run: python scripts/gen_cleaver_item.py
"""

import json
import os
import struct

# ---- palette (assets/prefabs — palette index == material ID) ----------------
STEEL = 57      # the blade and the shoulder
GRIP = 58       # grip_leather, the wrapped hilt

# Art voxels per METRE. The same 40 the sword is authored at, so the two land
# on the same micro lattice at any world scale and their derived hefts are
# comparable as pure geometry (DESIGN.md §3b — never author a bare `scale`).
ART_VOXELS_PER_METRE = 40

# ---- dimensions, micro units (40 per metre) ---------------------------------
CLEAVER_LEN = 54        # butt to tip: 1.35 m
CLEAVER_GRIP = 18       # micro of two-handed hilt behind the shoulder
CLEAVER_SHOULDER = 2    # the blade's own stop, in place of a crossguard
CLEAVER_HALF_W = 4      # blade half-depth at the widest (0.20 m of edge)
CLEAVER_HALF_T = 1.15   # half-thickness of the flat


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


def cleaver_vox(size):
    """A single-edged chopping blade on a long two-handed grip, lying along the
    box's X axis with the butt at LOW x and the tip at HIGH x.

    SINGLE-EDGED means the section is not centred: the spine sits at high y and
    the edge at low y, and the blade grows DOWNWARD from the spine as it
    approaches the tip. That is what puts the mass out at the end (a cleaver's
    whole argument) and it is also why the `edge` block below is not simply the
    box's centreline — it runs along the sharpened side."""
    sx, sy, sz = size
    out = []
    cz = sz * 0.5
    shoulder_x = CLEAVER_GRIP + CLEAVER_SHOULDER
    spine_y = sy - 1.0          # the back of the blade, in micro units

    for x in range(sx):
        if x < 3:
            # butt cap: a squat steel counterweight, which is most of why a
            # two-handed weapon balances at all
            for z in range(sz):
                for y in range(sy):
                    if ellipse_mask(y, z, sy * 0.5, cz, 1.9, 1.9):
                        out.append((x, y, z, STEEL))
        elif x < CLEAVER_GRIP:
            for z in range(sz):
                for y in range(sy):
                    if ellipse_mask(y, z, sy * 0.5, cz, 1.3, 1.3):
                        out.append((x, y, z, GRIP))
        elif x < shoulder_x:
            # the shoulder: a short steel collar, the blade's own stop
            for z in range(sz):
                for y in range(sy):
                    if ellipse_mask(y, z, sy * 0.5, cz, sy * 0.35, 1.7):
                        out.append((x, y, z, STEEL))
        else:
            # THE BLADE. Depth grows from ~55% at the shoulder to full at the
            # tip; the spine stays put and the edge drops away from it.
            t = (x - shoulder_x) / max(sx - 1 - shoulder_x, 1)
            depth = 2.0 * CLEAVER_HALF_W * (0.55 + 0.45 * t)
            edge_y = spine_y - depth
            for z in range(sz):
                for y in range(sy):
                    if y + 0.5 < edge_y or y > spine_y:
                        continue
                    # Thinner toward the edge: the flat tapers to the bevel, so
                    # the section reads as a blade rather than as a bar.
                    frac = (y + 0.5 - edge_y) / max(depth, 1e-3)
                    half_t = CLEAVER_HALF_T * (0.45 + 0.55 * min(frac * 2.0, 1.0))
                    if abs(z + 0.5 - cz) <= half_t:
                        out.append((x, y, z, STEEL))
    return out


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "assets", "items")
    os.makedirs(out_dir, exist_ok=True)

    size = (CLEAVER_LEN, 2 * CLEAVER_HALF_W + 2, 4)
    voxels = cleaver_vox(size)
    assert voxels, "cleaver generated no voxels"
    # DebrisVoxel is int8 and these are MICRO units, so a part may be at most
    # 120 micro on an axis. Same ceiling the rig and the sword assert.
    assert max(size) <= 120, "cleaver exceeds DebrisVoxel int8 range"

    seen = set()
    uniq = []
    for v in voxels:
        x, y, z, _c = v
        assert 0 <= x < size[0] and 0 <= y < size[1] and 0 <= z < size[2], \
            f"cleaver voxel {(x, y, z)} outside declared size {size}"
        if (x, y, z) in seen:
            continue          # first write wins, as in the sword generator
        seen.add((x, y, z))
        uniq.append(v)

    body = model_chunks(size, uniq)
    pivot = (size[0] // 2, size[1] // 2, size[2] // 2)
    graph = ntrn(0, "", 1, (0, 0, 0)) + ngrp(1, [2])
    graph += ntrn(2, "cleaver", 3, pivot) + nshp(3, 0)

    payload = body + graph
    data = b"VOX " + struct.pack("<i", 150)
    data += b"MAIN" + struct.pack("<ii", 0, len(payload)) + payload
    with open(os.path.join(out_dir, "cleaver.vox"), "wb") as f:
        f.write(data)

    scale = ART_VOXELS_PER_METRE // 10   # micro per world voxel at 10 vox/m
    sidecar = {
        "comment": (
            "A two-handed cleaver: the heavy end of the melee range, and the "
            "second arm of the wound model's heft differential. Regenerate "
            "with scripts/gen_cleaver_item.py — the edge and hilt blocks are "
            "emitted from the same constants that build the mesh, so the "
            "hitbox cannot drift from the art. NOTE there is no \"heft\" key "
            "here on purpose: heft is DERIVED from this file's own voxel "
            "volume (item.h ItemDef::heftVolume), so re-authoring the blade "
            "re-weighs it in the same edit."),
        "name": "cleaver",
        "model": "cleaver",
        "artVoxelsPerMetre": ART_VOXELS_PER_METRE,
        # Tougher than the sword as a rig slot: it is more metal.
        "hp": 45,
        "severable": True,
        # See sim/tuning.h gore.woundImpactSeverScale — this is the item's own
        # durability as a borrowed rig slot (how fast a hit knocks it out of
        # the hand), not the wound model's.
        "severImpactSpeed": 9.0,
        "grip": {
            "held_right": {
                "translation": [0, 0, 0],
                "rotation": [0, -90, 0],
                "scale": 1.0,
            }
        },
        # The wrapped hilt between the butt cap and the shoulder, in the item's
        # own micro units. The runtime puts this box's CENTRE on the hand
        # socket, so nothing here is a tuned placement constant.
        "hilt": {
            "min": [3, 0, 0],
            "size": [CLEAVER_GRIP - 3, 2 * CLEAVER_HALF_W + 2, 4],
        },
        # The sharpened side, from the shoulder to the tip. Authored along +x
        # because that is how the art is built.
        "edge": {
            "from": CLEAVER_GRIP + CLEAVER_SHOULDER,
            "to": CLEAVER_LEN,
            "axis": [1, 0, 0],
            "halfWidth": CLEAVER_HALF_W,
        },
        # Heavier and slower to settle than the sword's flick.
        "spring": {"halflife": 0.14, "gain": 0.30, "maxAngle": 0.12},
    }
    with open(os.path.join(out_dir, "cleaver.json"), "w") as f:
        json.dump(sidecar, f, indent=2)
        f.write("\n")

    vol = len(uniq) / float(scale ** 3)
    print(f"wrote {out_dir}/cleaver.vox  ({len(uniq)} voxels, size {size})")
    print(f"wrote {out_dir}/cleaver.json")
    print(f"  blade {CLEAVER_LEN} micro = {CLEAVER_LEN / scale} world voxels "
          f"({CLEAVER_LEN / ART_VOXELS_PER_METRE:.2f} m)")
    print(f"  DERIVED HEFT VOLUME {vol:.2f} world voxels "
          f"= {vol / 5.3:.2f}x the arming sword (gore.woundHeftRef)")


if __name__ == "__main__":
    main()
