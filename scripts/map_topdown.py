#!/usr/bin/env python3
"""A top-down picture of a map's REAL worldgen: rivers, lakes, roads as the
engine makes them (docs/PLAN_worldbuilder.md W1 -- "LOOK at the river").

Boots ONE `sandvox --voxserve` under scripts/run.sh (the GPU lock), asks it for
the box in tiles of up to 128 samples a side, and paints every column's top
non-air sample: its material colour, relief-shaded, contour lines every
--contour voxels, water blue by depth. Writes a PNG.

  python scripts/map_topdown.py --map default --box -2048,2048,4096,4096 \
      --y 0,1024 --lod 4 --out build/topdown.png
  python scripts/map_topdown.py --map harness --box 0,0,512,512 --y 0,512 --lod 1

--box is x0,z0,width,depth in world voxels; --y the vertical band (y0,height)
the columns are searched in (a column whose ground is outside it stays black).
--asset-dir runs the exe against another tree's assets (SANDVOX_ASSET_DIR).
The `rivers` selftest gate writes its own synthetic window to
build/rivers_top.bmp; this is the same picture for any map on disk.
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def decode_svvx(blob):
    (magic, ver, ox, oy, oz, nx, ny, nz, lod, seed, vpm, flags, runs, solid, ymin, ymax) = struct.unpack_from(
        "<IIiiiIIIIIiIIIii", blob, 0)
    if magic != 0x58565653:
        raise ValueError("not an SVVX region")
    grid = bytearray()
    words = []
    off = 64
    for _ in range(runs):
        w, c = struct.unpack_from("<II", blob, off)
        off += 8
        words.extend([w & 0xFFF] * c)
    return (ox, oy, oz, nx, ny, nz, lod), words


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--map", required=True)
    ap.add_argument("--box", required=True, help="x0,z0,width,depth (world voxels)")
    ap.add_argument("--y", default="0,512", help="y0,height of the searched band (world voxels)")
    ap.add_argument("--lod", type=int, default=4, help="world voxels per sample (1, 2, 4, 8, 16)")
    ap.add_argument("--seed", type=int, default=1337)
    ap.add_argument("--contour", type=int, default=8, help="contour interval, voxels (0 = none)")
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "Release", "sandvox.exe"))
    ap.add_argument("--asset-dir", default=None)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "topdown.png"))
    a = ap.parse_args()
    from PIL import Image

    x0, z0, W, D = (int(v) for v in a.box.split(","))
    y0, H = (int(v) for v in a.y.split(","))
    lod = a.lod
    x0 -= x0 % 16; z0 -= z0 % 16; y0 -= y0 % 16
    nxs, nzs, nys = W // lod, D // lod, H // lod
    env = dict(os.environ, SANDVOX_MAP=a.map, SANDVOX_NO_CRASH_DIALOG="1")
    if a.asset_dir:
        env["SANDVOX_ASSET_DIR"] = os.path.abspath(a.asset_dir)
    tmp = tempfile.mkdtemp(prefix="sv_topdown_")
    # Git's bash, by install location (a bare "bash" may be WSL's launcher,
    # which cannot read a Windows path); forward slashes for its arguments.
    bash = os.environ.get("SANDVOX_BASH") or next(
        (c for c in (os.path.join(os.environ.get("ProgramFiles", "C:/Program Files"), "Git", "bin", "bash.exe"),
                     "C:/msys64/usr/bin/bash.exe") if os.path.isfile(c)), "bash")
    fwd = lambda q: os.path.abspath(q).replace("\\", "/")
    p = subprocess.Popen([bash, fwd(os.path.join(ROOT, "scripts", "run.sh")), fwd(a.exe), "--voxserve"], cwd=ROOT, env=env,
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)

    def cmd(line):
        p.stdin.write(line + "\n")
        p.stdin.flush()
        while True:
            ack = p.stdout.readline()
            if not ack:
                raise RuntimeError("voxserve died (is the exe built? is SANDVOX_MAP=%s a map?)" % a.map)
            ack = ack.strip()
            if ack.startswith("OK ") or ack.startswith("ERR "):
                return ack

    while True:
        line = p.stdout.readline()
        if not line:
            sys.exit("voxserve died during boot")
        if line.startswith("VOXSERVE READY"):
            break
    pal_path = os.path.join(tmp, "palette.json")
    cmd("PALETTE " + pal_path)
    pal = json.load(open(pal_path))["materials"]
    rgb = {m["id"]: tuple(int(m["colors"][0][i:i + 2], 16) for i in (1, 3, 5)) for m in pal}
    water = {m["id"] for m in pal if m["name"] in ("water", "ice")}
    top = [[(-1, 0)] * nxs for _ in range(nzs)]   # (sample y, material)
    step = min(128, 512 // lod)   # samples a tile: <= 128, and <= the 512-voxel residency window
    for tz in range(0, nzs, step):
        for tx in range(0, nxs, step):
            for ty in range(nys - (nys % step or step), -1, -step):
                ny = min(step, nys - ty)
                if ny <= 0:
                    continue
                nx, nz = min(step, nxs - tx), min(step, nzs - tz)
                out = os.path.join(tmp, "r.bin")
                ack = cmd("REGION %d %d %d %d %d %d %d %d %s" % (x0 + tx * lod, y0 + ty * lod, z0 + tz * lod, nx, ny, nz,
                                                               lod, a.seed, out))
                if not ack.startswith("OK"):
                    sys.exit("voxserve: " + ack)
                hdr, words = decode_svvx(open(out, "rb").read())
                for k in range(nz):
                    for i in range(nx):
                        if top[tz + k][tx + i][0] >= 0:
                            continue
                        for j in range(ny - 1, -1, -1):
                            m = words[(k * ny + j) * nx + i]
                            if m != 0:
                                top[tz + k][tx + i] = (ty + j, m)
                                break
    p.stdin.write("QUIT\n")   # exits without an ack
    p.stdin.flush()
    p.wait(timeout=30)
    img = Image.new("RGB", (nxs, nzs))
    px = img.load()
    for k in range(nzs):
        for i in range(nxs):
            y, m = top[k][i]
            if y < 0:
                continue
            r, g, b = rgb.get(m, (255, 0, 255))
            wy = y0 + y * lod
            if m in water:
                r, g, b = 30, 150, 230
            shade = 0.7 + 0.6 * (y / max(nys - 1, 1))
            if i > 0 and top[k][i - 1][0] >= 0:
                shade += 0.04 * (y - top[k][i - 1][0])   # light from the west
            if a.contour and m not in water and i > 0 and top[k][i - 1][0] >= 0 and \
                    (y0 + top[k][i - 1][0] * lod) // a.contour != wy // a.contour:
                shade *= 0.7
            px[i, k] = tuple(max(0, min(255, int(c * shade))) for c in (r, g, b))
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    img.save(a.out)
    print("wrote %s (%dx%d samples at lod %d, map %s)" % (a.out, nxs, nzs, lod, a.map))


if __name__ == "__main__":
    main()
