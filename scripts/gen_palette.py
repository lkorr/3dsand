#!/usr/bin/env python3
"""Generate assets/prefabs/palette.png for MagicaVoxel from materials.json.

Palette convention (PLAN_voxel_art_and_mobs.md A1): .vox palette index ==
engine material ID. PNG pixel i maps to MagicaVoxel palette index i+1, which
is material ID i+1 (ID 0 = air, never painted). Load this in MagicaVoxel via
the palette panel so models are painted with true material colours.

Pure stdlib (zlib + struct): no Pillow dependency. Re-run after adding
materials (append-only, so existing art never shifts).
"""
import json
import os
import struct
import sys
import zlib


def png_write(path, pixels, w, h):
    """pixels: list of (r,g,b,a) rows-major."""
    raw = b"".join(
        b"\x00" + b"".join(struct.pack("4B", *px) for px in pixels[y * w:(y + 1) * w])
        for y in range(h)
    )

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)  # 8-bit RGBA
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    mats_path = os.path.join(root, "assets", "materials", "materials.json")
    out_dir = os.path.join(root, "assets", "prefabs")
    out_path = os.path.join(out_dir, "palette.png")

    with open(mats_path, "r", encoding="utf-8") as f:
        mats = json.load(f)["materials"]

    # pixel i -> palette index i+1 -> material ID i+1 -> materials.json[i]
    # (LoadAssets prepends implicit air at ID 0)
    pixels = []
    for i in range(256):
        if i < len(mats):
            c = mats[i]["colors"][0].lstrip("#")
            pixels.append((int(c[0:2], 16), int(c[2:4], 16), int(c[4:6], 16), 255))
        else:
            # unmapped slots: mid-gray so accidental use is visible in-editor
            pixels.append((110, 110, 110, 255))

    os.makedirs(out_dir, exist_ok=True)
    png_write(out_path, pixels, 256, 1)
    print(f"wrote {out_path} ({len(mats)} materials mapped, "
          f"palette indices 1..{len(mats)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
