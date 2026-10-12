"""Generate assets/items/<key>.{vox,json} -- the KEYS (ItemKind::Key, dungeons
D3, docs/PLAN_dungeons.md section 3.7).

A key does nothing in a hand and is never worn: having one in the kit is what
opens a door whose `props.lock` names its keyId (items.json `unlocks`, default
the item's own id). The art is only what the bag and the ground show.

Authored along +x like every held item in the library (gen_vessel_items.py):
the bow (the ring you hold) at low x, the shaft, the bit (the teeth) at high x.

Run: python scripts/gen_key_items.py
"""

import json
import os
import struct

ART_VOXELS_PER_METRE = 80

# palette index == material id + 1 (assets/prefabs convention, see
# gen_vessel_items.py); keep <= 127 (128..255 is the art palette).
KEYS = [
    {"name": "iron_key", "pal": 121, "comment": "A plain iron key, the sort a gaoler carries by the dozen."},
    {"name": "bone_key", "pal": 56, "comment": "A key carved from a long bone, polished by hands."},
    {"name": "old_key", "pal": 57, "comment": "A heavy steel key, black with age, its bit worn smooth."},
]

SIZE = (16, 2, 7)   # x along the key, y thickness, z height (MagicaVoxel Z-up)


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


def key_cells(pal):
    cells = set()
    # The bow: a 7x7 ring at x 0..6 (z 0..6), hollow 3x3 centre.
    for x in range(0, 7):
        for z in range(0, 7):
            edge = x in (0, 6) or z in (0, 6)
            corner = (x in (0, 6)) and (z in (0, 6))
            if edge and not corner:
                cells.add((x, z))
    # The shaft: x 7..15 along the middle row (z 3).
    for x in range(7, 16):
        cells.add((x, 3))
    # The bit: two teeth down from the shaft near the tip.
    for x, z in ((12, 2), (12, 1), (14, 2), (14, 1), (15, 2), (15, 1)):
        cells.add((x, z))
    vox = []
    for (x, z) in sorted(cells):
        for y in range(SIZE[1]):
            vox.append((x, y, z, pal))
    return vox


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "assets", "items")
    for k in KEYS:
        name = k["name"]
        vox = key_cells(k["pal"])
        write_vox(os.path.join(out_dir, f"{name}.vox"), name, SIZE, vox)
        sidecar = {
            "comment": k["comment"] + " Regenerate with scripts/gen_key_items.py. WHICH LOCK it "
                       "opens is items.json's `unlocks` (default: the item id).",
            "name": name,
            "model": name,
            "artVoxelsPerMetre": ART_VOXELS_PER_METRE,
            "hp": 20,
            "severable": False,
            "grip": {"held_right": {"translation": [0, 0, 0], "rotation": [0, -90, 0], "scale": 1.0}},
            "hilt": {"min": [0, 0, 0], "size": [7, 2, 7]},
        }
        with open(os.path.join(out_dir, f"{name}.json"), "w") as f:
            json.dump(sidecar, f, indent=2)
            f.write("\n")
        print(f"wrote {name}.vox ({len(vox)} voxels) + {name}.json")


if __name__ == "__main__":
    main()
