#!/usr/bin/env python3
"""Render a SANDVOX_FLOW_DUMP file (src/sim/flowfield.cpp) as a PNG: water
nodes coloured by speed, a short line per node in the flow direction.

    python scripts/flow_view.py build/flow.bin build/flow.png [scale]
"""
import struct
import sys

from PIL import Image, ImageDraw

src, dst = sys.argv[1], sys.argv[2]
px = int(sys.argv[3]) if len(sys.argv) > 3 else 10
w = open(src, "rb").read()
words = struct.unpack("<%dI" % (len(w) // 4), w)
ox, oz, n = struct.unpack("<iiI", struct.pack("<III", *words[:3]))


def i16(v):
    return v - 65536 if v >= 32768 else v


img = Image.new("RGB", (n * px, n * px), (40, 32, 24))
d = ImageDraw.Draw(img)
vmax = 1e-6
nodes = []
for j in range(n):
    for i in range(n):
        k = 4 + 2 * (j * n + i)
        w0, w1 = words[k], words[k + 1]
        s = i16(w1 & 0xFFFF)
        if s == -32768:
            continue
        vx, vz = i16(w0 & 0xFFFF) / 128.0, i16(w0 >> 16) / 128.0
        sp = (vx * vx + vz * vz) ** 0.5
        vmax = max(vmax, sp)
        nodes.append((i, j, vx, vz, sp))
for i, j, vx, vz, sp in nodes:
    t = sp / vmax
    col = (int(255 * t), int(80 + 120 * (1 - abs(t - 0.5) * 2)), int(255 * (1 - t)))
    d.rectangle([i * px, j * px, i * px + px - 1, j * px + px - 1], fill=col)
    if sp > 1e-3:
        cx, cz = i * px + px / 2, j * px + px / 2
        L = px * 0.45 / sp
        d.line([cx, cz, cx + vx * L, cz + vz * L], fill=(0, 0, 0))
img.save(dst)
print("origin (%d,%d) nodes %d, water %d, max %.2f cells/s (%.2f m/s)" % (ox, oz, n, len(nodes), vmax, vmax * 0.1))
