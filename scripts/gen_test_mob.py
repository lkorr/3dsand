#!/usr/bin/env python3
"""Generate assets/mobs/dummy.{vox,json} — a small articulated training dummy.

Serves two purposes: a working example of the mob authoring format (one .vox
model per limb, named via the scene graph, plus a JSON sidecar for joints/HP/
bleeding), and an end-to-end fixture for the mob pipeline without needing
MagicaVoxel installed. The .vox opens fine in MagicaVoxel for editing.

Coordinates here are MagicaVoxel scene space (Z-up); the engine loader
converts to Y-up. Palette index == material ID (wood=2, plant=17).
"""
import json
import os
import struct
import sys

WOOD, PLANT = 2, 17


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


def box(size, mat):
    sx, sy, sz = size
    return [(x, y, z, mat)
            for z in range(sz) for y in range(sy) for x in range(sx)]


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "assets", "mobs")
    os.makedirs(out_dir, exist_ok=True)

    # limb -> (size, min-corner in scene space, material). Feet on z=0.
    limbs = {
        "torso": ((4, 2, 5), (-2, -1, 6), WOOD),
        "head":  ((3, 3, 3), (-1, -1, 11), PLANT),
        "arm.L": ((1, 1, 5), (-3, -1, 6), WOOD),
        "arm.R": ((1, 1, 5), (2, -1, 6), WOOD),
        "leg.L": ((1, 1, 6), (-2, -1, 0), WOOD),
        "leg.R": ((1, 1, 6), (1, -1, 0), WOOD),
    }

    order = ["torso", "head", "arm.L", "arm.R", "leg.L", "leg.R"]
    body = b""
    graph = b""
    grp_children = []
    for i, name in enumerate(order):
        size, mn, mat = limbs[name]
        body += model_chunks(size, box(size, mat))
        pivot = (size[0] // 2, size[1] // 2, size[2] // 2)
        t = (mn[0] + pivot[0], mn[1] + pivot[1], mn[2] + pivot[2])
        trn_id, shp_id = 2 + 2 * i, 3 + 2 * i
        graph += ntrn(trn_id, name, shp_id, t) + nshp(shp_id, i)
        grp_children.append(trn_id)
    graph = ntrn(0, "", 1, (0, 0, 0)) + ngrp(1, grp_children) + graph

    payload = body + graph
    data = b"VOX " + struct.pack("<i", 150)
    data += b"MAIN" + struct.pack("<ii", 0, len(payload)) + payload
    vox_path = os.path.join(out_dir, "dummy.vox")
    with open(vox_path, "wb") as f:
        f.write(data)

    # Sidecar. Anchors are PREFAB-LOCAL ENGINE coords (Y-up, min corner 0):
    # engine = (scene.x + 3, scene.z, -scene.y) for this model's extents.
    sidecar = {
        "root": "torso",
        "bleed": {"material": "blood", "perDamage": 2.0},
        "speed": 5.0,
        "limbs": [
            {"name": "torso", "hp": 40, "severable": False},
            {"name": "head", "parent": "torso", "joint": "ball", "hp": 15,
             "severable": True, "vital": True, "anchor": [2.5, 11.0, 0.5]},
            {"name": "arm.L", "parent": "torso", "joint": "ball", "hp": 12,
             "severable": True, "anchor": [1.0, 10.0, 0.5],
             "swingAmp": 0.35, "swingPhase": 1.0},
            {"name": "arm.R", "parent": "torso", "joint": "ball", "hp": 12,
             "severable": True, "anchor": [5.0, 10.0, 0.5],
             "swingAmp": 0.35, "swingPhase": 0.0},
            {"name": "leg.L", "parent": "torso", "joint": "ball", "hp": 15,
             "severable": True, "anchor": [1.5, 6.0, 0.5],
             "swingAmp": 0.5, "swingPhase": 0.0},
            {"name": "leg.R", "parent": "torso", "joint": "ball", "hp": 15,
             "severable": True, "anchor": [4.5, 6.0, 0.5],
             "swingAmp": 0.5, "swingPhase": 1.0},
        ],
    }
    json_path = os.path.join(out_dir, "dummy.json")
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(sidecar, f, indent=2)
        f.write("\n")
    print(f"wrote {vox_path} and {json_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
