"""Generate assets/items/flask.{vox,json} and pouch.{vox,json} -- the VESSELS
(ItemKind::Container, src/game/container.h).

Both are turned shapes about the box's long X axis, the axis every held item
in this library is authored along (see gen_shortblade_items.py: the art is
never mirrored or re-aimed, the grip rotation does that). Low x is the base,
high x the mouth, so the mouth leads the way a blade's tip does and the hand
closes round the belly.

What a vessel HOLDS is not here: that is items.json's `container` block. This
file is only the thing you see in the hand and on the ground.

Run: python scripts/gen_vessel_items.py
"""

import json
import math
import os
import struct

# palette index == material id (assets/prefabs convention)
GLASS = 26
WOOD = 2
LEATHER = 53
GRIP = 58

ART_VOXELS_PER_METRE = 40


def chunk(cid, content, children=b""):
    return cid + struct.pack("<ii", len(content), len(children)) + content + children


def dict_bytes(d):
    out = struct.pack("<i", len(d))
    for k, v in d.items():
        out += struct.pack("<i", len(k)) + k.encode()
        out += struct.pack("<i", len(v)) + v.encode()
    return out


def write_vox(path, name, size, voxels):
    sx, sy, sz = size
    xyzi = struct.pack("<i", len(voxels)) + b"".join(
        struct.pack("4B", x, y, z, c) for (x, y, z, c) in voxels)
    body = chunk(b"SIZE", struct.pack("<iii", sx, sy, sz)) + chunk(b"XYZI", xyzi)
    pivot = (sx // 2, sy // 2, sz // 2)

    def ntrn(node_id, nm, child, t):
        attrs = {"_name": nm} if nm else {}
        c = struct.pack("<i", node_id) + dict_bytes(attrs)
        c += struct.pack("<iiii", child, -1, 0, 1)
        c += dict_bytes({"_t": f"{t[0]} {t[1]} {t[2]}"})
        return chunk(b"nTRN", c)

    grp = struct.pack("<i", 1) + dict_bytes({}) + struct.pack("<ii", 1, 2)
    shp = struct.pack("<i", 3) + dict_bytes({}) + struct.pack("<ii", 1, 0) + dict_bytes({})
    graph = ntrn(0, "", 1, (0, 0, 0)) + chunk(b"nGRP", grp)
    graph += ntrn(2, name, 3, pivot) + chunk(b"nSHP", shp)
    payload = body + graph
    data = b"VOX " + struct.pack("<i", 150)
    data += b"MAIN" + struct.pack("<ii", 0, len(payload)) + payload
    with open(path, "wb") as f:
        f.write(data)


def lathe(profile, size, shell=None):
    """A solid of revolution about X. `profile(x)` -> (radius, material) or
    None. Voxel centres are i+0.5, as in every generator here."""
    sx, sy, sz = size
    cy, cz = sy * 0.5, sz * 0.5
    out = []
    for x in range(sx):
        p = profile(x)
        if not p:
            continue
        r, mat = p
        for z in range(sz):
            for y in range(sy):
                dy, dz = y + 0.5 - cy, z + 0.5 - cz
                if dy * dy + dz * dz <= r * r:
                    out.append((x, y, z, mat))
    return out


VESSELS = [
    {
        "name": "flask",
        # 0.25 m long, a round-bellied glass flask with a stoppered neck.
        "size": (10, 7, 7),
        "profile": lambda x: (
            (2.6, GLASS) if x == 0 else
            (3.5, GLASS) if x <= 4 else
            (2.6, GLASS) if x == 5 else
            (1.3, GLASS) if x <= 7 else
            (1.5, WOOD)),   # the stopper
        "hilt": {"min": [1, 0, 0], "size": [4, 7, 7]},
        "hp": 6,
        "severImpact": 3.5,
        "comment": "A glass flask: round belly, narrow neck, a wooden stopper.",
    },
    {
        "name": "pouch",
        # 0.2 m, a drawstring leather bag: a fat sack gathered at the mouth.
        "size": (8, 7, 7),
        "profile": lambda x: (
            (2.5, LEATHER) if x == 0 else
            (3.5, LEATHER) if x <= 4 else
            (1.6, GRIP) if x == 5 else          # the drawstring
            (2.2, LEATHER)),                    # the gathered frill
        "hilt": {"min": [1, 0, 0], "size": [4, 7, 7]},
        "hp": 8,
        "severImpact": 3.5,
        "comment": "A leather pouch, gathered at the mouth with a drawstring.",
    },
]


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "assets", "items")
    for v in VESSELS:
        name, size = v["name"], v["size"]
        vox = lathe(v["profile"], size)
        assert vox, name
        write_vox(os.path.join(out_dir, f"{name}.vox"), name, size, vox)
        sidecar = {
            "comment": v["comment"] + " Regenerate with scripts/gen_vessel_items.py. "
                       "WHAT IT HOLDS is items.json's `container` block, not this file.",
            "name": name,
            "model": name,
            "artVoxelsPerMetre": ART_VOXELS_PER_METRE,
            "hp": v["hp"],
            "severable": True,
            "severImpactSpeed": v["severImpact"],
            # The weapons' rotation: authored +x (the mouth) onto +Z.
            "grip": {"held_right": {"translation": [0, 0, 0],
                                    "rotation": [0, -90, 0], "scale": 1.0}},
            # Where the fist closes: the belly.
            "hilt": v["hilt"],
        }
        with open(os.path.join(out_dir, f"{name}.json"), "w") as f:
            json.dump(sidecar, f, indent=2)
            f.write("\n")
        print(f"wrote {name}.vox ({len(vox)} voxels, size {size}) + {name}.json")


if __name__ == "__main__":
    main()
