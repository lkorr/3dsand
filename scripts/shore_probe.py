#!/usr/bin/env python3
"""Offline mirror of the shore band in worldgen.wgsl, for tuning it by numbers.

The shore fringe is a pure function of (column, seed) like every other worldgen
feature, so it can be evaluated on the CPU with no GPU, no build and no world —
which is the only practical way to answer "is the band actually continuous",
"how much of it is being cut as bluff" and "what does each species cover",
since ponds are keep-out'd from the spawn area and so never appear in a --shot.

It mirrors pcg/hash3, baseHeight, pondInfo/pondSurface/pondAt and shoreAt
EXACTLY (integer division and all) and reads its constants from
assets/materials/tuning.json, so it re-derives itself when the knobs move. It is
a MIRROR, not the source: if worldgen.wgsl changes, this has to follow — the
same standing obligation World::TerrainHeight already carries for baseHeight.

  python scripts/shore_probe.py             # coverage + species mix at one pond
  python scripts/shore_probe.py --profile   # height above the waterline per ring
  python scripts/shore_probe.py --rings     # band continuity all the way round
"""
import json
import math
import os
import sys
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SEED = 1337  # kDefaultSeed (src/test/support.h)
M = 0xFFFFFFFF


# ---- hash (common.wgsl) -----------------------------------------------------
def pcg(v):
    s = (v * 747796405 + 2891336453) & M
    w = (((s >> ((s >> 28) + 4)) ^ s) * 277803737) & M
    return ((w >> 22) ^ w) & M


def hash3(a, b, c):
    return pcg((a & M) ^ pcg((b & M) ^ pcg(c & M)))


def u32(i):
    return i & M


# ---- floor div / positive mod (worldgen.wgsl fdiv/fmodp) --------------------
def fdiv(a, b):
    q = int(a / b) if b else 0
    if b and a % b != 0 and (a < 0) != (b < 0):
        q -= 1
    return q


def fmodp(a, b):
    return a - fdiv(a, b) * b


T = json.load(open(os.path.join(ROOT, "assets", "materials", "tuning.json"),
                   encoding="utf-8"))["worldgen"]
BASE_H = T["baseHeight"]
HILL_WL, HILL_AMP = T["hillWavelength"], T["hillAmplitude"]
DET_WL, DET_AMP = T["detailWavelength"], T["detailAmplitude"]
TREELINE = T["treeline"]
POND_TILE, PC = T["pondTile"], T["pondChance"]
RMIN, RSPAN = T["pondRadiusMin"], T["pondRadiusSpan"]
PD, PDR = T["pondDepth"], T["pondDepthRim"]
BAND, MUDW = T["shoreBand"], T["shoreMudWidth"]
LIFT = T["shoreLift"]
CATC = T["shoreCattailChance"]
CATR, CATH = T["shoreCattailReach"], T["shoreCattailHeight"]
SEDC = T["shoreSedgeChance"]
HORC, HORH = T["shoreHorsetailChance"], T["shoreHorsetailHeight"]
IRIC, MOSSC = T["shoreIrisChance"], T["shoreMossChance"]


def vnoise(x, z, cs, seed):
    gx, gz = fdiv(x, cs), fdiv(z, cs)
    fx, fz = fmodp(x, cs), fmodp(z, cs)
    h00 = hash3(seed, u32(gx), u32(gz)) & 0xFF
    h10 = hash3(seed, u32(gx + 1), u32(gz)) & 0xFF
    h01 = hash3(seed, u32(gx), u32(gz + 1)) & 0xFF
    h11 = hash3(seed, u32(gx + 1), u32(gz + 1)) & 0xFF
    v0 = h00 * (cs - fx) + h10 * fx
    v1 = h01 * (cs - fx) + h11 * fx
    return (v0 * (cs - fz) + v1 * fz) // (cs * cs)


def baseHeight(x, z, seed=SEED):
    return (BASE_H
            + (vnoise(x, z, HILL_WL, seed ^ 1) * HILL_AMP) // 255
            + (vnoise(x, z, DET_WL, seed ^ 2) * DET_AMP) // 255)


_maxR = RMIN + RSPAN - 1
_inset = _maxR + 4
_span = max(POND_TILE - 2 * _inset, 1)


def pondInfo(pt, pz, seed=SEED):
    rh = hash3(seed ^ 0xB0A7, u32(pt), u32(pz))
    if rh % PC != 0:
        return None
    r = RMIN + ((rh >> 4) % RSPAN)
    if POND_TILE - 2 * _inset < 1:
        return None
    cx = pt * POND_TILE + _inset + ((rh >> 9) % _span)
    cz = pz * POND_TILE + _inset + ((rh >> 17) % _span)
    if -44 <= cx <= 264 and -44 <= cz <= 264:
        return None
    if abs(cx - 408) < r + 24 and abs(cz - 128) < r + 24:
        return None
    for qx, qz in ((420, 420), (260, 300), (220, 520)):
        if (cx - qx) ** 2 + (cz - qz) ** 2 < 128 * 128:
            return None
    return (cx, cz, r)


RIM = [(256, 0), (247, 66), (222, 128), (181, 181), (128, 222), (66, 247),
       (0, 256), (-66, 247), (-128, 222), (-181, 181), (-222, 128), (-247, 66),
       (-256, 0), (-247, -66), (-222, -128), (-181, -181), (-128, -222),
       (-66, -247), (0, -256), (66, -247), (128, -222), (181, -181),
       (222, -128), (247, -66)]


def pondSurface(p, seed=SEED):
    cx, cz, r = p
    return min(baseHeight(cx + (sx * r) // 256, cz + (sy * r) // 256, seed)
               for sx, sy in RIM) - 2


def pondAt(x, z, seed=SEED):
    p = pondInfo(fdiv(x, POND_TILE), fdiv(z, POND_TILE), seed)
    if not p:
        return None
    cx, cz, r = p
    d2 = (x - cx) ** 2 + (z - cz) ** 2
    if d2 > r * r:
        return None
    surf = pondSurface(p, seed)
    depth = PDR + ((r * r - d2) * (PD - PDR)) // (r * r)
    return (surf - depth, surf)


CALLS = Counter()


def shoreAt(x, z, seed=SEED):
    """(past, surf) or None. `past` is whole voxels beyond the rim."""
    if BAND <= 0:
        return None
    pt, pz = fdiv(x, POND_TILE), fdiv(z, POND_TILE)
    lx, lz = fmodp(x, POND_TILE), fmodp(z, POND_TILE)
    sx = -1 if lx < BAND else (1 if lx >= POND_TILE - BAND else 0)
    sz = -1 if lz < BAND else (1 if lz >= POND_TILE - BAND else 0)
    best, bd, n = None, 1 << 62, 0
    for iz in range(2):
        oz = sz if iz == 1 else 0
        if iz == 1 and sz == 0:
            continue
        for ix in range(2):
            ox = sx if ix == 1 else 0
            if ix == 1 and sx == 0:
                continue
            n += 1
            p = pondInfo(pt + ox, pz + oz, seed)
            if not p:
                continue
            cx, cz, r = p
            d2 = (x - cx) ** 2 + (z - cz) ** 2
            if d2 <= r * r:
                CALLS[n] += 1
                return None                     # inside the disc: that is pond
            if d2 > (r + BAND) ** 2:
                continue
            if d2 < bd:
                bd, best = d2, p
    CALLS[n] += 1
    if not best:
        return None
    r = best[2]
    lo, hi = 0, BAND
    for _ in range(8):
        if lo >= hi:
            break
        mid = (lo + hi) // 2
        if bd <= (r + mid) ** 2:
            hi = mid
        else:
            lo = mid + 1
    return (max(lo - 1, 0), pondSurface(best, seed))


def find_pond(limit=40):
    for pt in range(limit):
        for pz in range(limit):
            p = pondInfo(pt, pz)
            if p:
                return p
    return None


def cover(p):
    cx, cz, r = p
    cnt = Counter()
    cols = bluff = 0
    lo, hi = -r - BAND - 2, r + BAND + 3
    for z in range(cz + lo, cz + hi):
        for x in range(cx + lo, cx + hi):
            if pondAt(x, z):
                continue
            s = shoreAt(x, z)
            if not s:
                continue
            past, surf = s
            h = baseHeight(x, z)
            if h >= TREELINE:
                continue
            if h > surf + LIFT:
                bluff += 1
                continue
            cols += 1
            if past < MUDW:
                cnt["shore_mud"] += 1
            elif hash3(SEED ^ 0x4D05, u32(x), u32(z)) % MOSSC == 0:
                cnt["wet_moss"] += 1
            hCat = hash3(SEED ^ 0x9C41, u32(x), u32(z))
            hHor = hash3(SEED ^ 0x3E77, u32(x), u32(z))
            hSed = hash3(SEED ^ 0x58BD, u32(x), u32(z))
            hIri = hash3(SEED ^ 0xA219, u32(x), u32(z))
            catH = CATH + ((hCat >> 5) % 7) - 3
            horH = HORH + ((hHor >> 5) % 5) - 2
            if past <= CATR and catH > 1 and hCat % CATC == 0:
                cnt["cattail"] += 1
                cnt["~cattail voxels"] += catH
            elif horH > 1 and hHor % HORC == 0:
                cnt["horsetail"] += 1
                cnt["~horsetail voxels"] += horH
            elif hIri % IRIC == 0:
                cnt["water_iris"] += 1
            elif hSed % SEDC == 0:
                cnt["marsh_grass"] += 1
    return cols, bluff, cnt


def profile(p):
    cx, cz, r = p
    lo, hi = -r - BAND - 2, r + BAND + 3
    rings_ = {}
    for z in range(cz + lo, cz + hi):
        for x in range(cx + lo, cx + hi):
            if pondAt(x, z):
                continue
            s = shoreAt(x, z)
            if not s:
                continue
            rings_.setdefault(s[0], []).append(baseHeight(x, z) - s[1])
    print(f"  a column is dropped as bluff when h - waterline > {LIFT}")
    for past in sorted(rings_):
        v = sorted(rings_[past])
        n = len(v)
        kept = sum(1 for d in v if d <= LIFT)
        print(f"  past={past:3d}  n={n:5d}  h-waterline  min {v[0]:4d}  "
              f"med {v[n // 2]:4d}  p90 {v[int(n * .9)]:4d}  max {v[-1]:4d}"
              f"   kept {100.0 * kept / n:5.1f}%")


def rings(p, step=1):
    cx, cz, r = p
    widths = Counter()
    thin = 0
    for a in range(0, 360, step):
        ux, uz = math.cos(math.radians(a)), math.sin(math.radians(a))
        w = sum(1 for d in range(r, r + BAND + 6)
                if shoreAt(cx + round(ux * d), cz + round(uz * d)))
        widths[w] += 1
        if w < BAND - 2:
            thin += 1
    print("  band width by direction (%d rays): " % (360 // step)
          + ", ".join(f"{w}:{n}" for w, n in sorted(widths.items())))
    print(f"  rays with a thin or absent band: {thin} / {360 // step}")
    clear = min(cx - fdiv(cx, POND_TILE) * POND_TILE - r,
                (fdiv(cx, POND_TILE) + 1) * POND_TILE - cx - r,
                cz - fdiv(cz, POND_TILE) * POND_TILE - r,
                (fdiv(cz, POND_TILE) + 1) * POND_TILE - cz - r)
    print(f"  this disc's clearance inside its own tile: {clear} vs band "
          f"{BAND} -> band crosses the tile edge: {clear < BAND}")


def main():
    args = sys.argv[1:]
    p = find_pond()
    if not p:
        print("no pond found in the searched tile range")
        return 1
    cx, cz, r = p
    print(f"pond at ({cx}, {cz}) r={r} waterline y={pondSurface(p)}  "
          f"seed={SEED}  band={BAND}  lift={LIFT}")
    if "--profile" in args:
        profile(p)
    elif "--rings" in args:
        rings(p)
    else:
        cols, bluff, cnt = cover(p)
        print(f"  shore columns {cols}, dropped as bluff {bluff} "
              f"({100.0 * bluff / max(cols + bluff, 1):.0f}% of the raw band)")
        for k in ("shore_mud", "wet_moss", "cattail", "horsetail",
                  "water_iris", "marsh_grass"):
            print(f"    {k:12s} {cnt[k]:6d}  "
                  f"({100.0 * cnt[k] / max(cols, 1):5.1f}% of shore columns)")
        print(f"    stalk voxels: cattail {cnt['~cattail voxels']}, "
              f"horsetail {cnt['~horsetail voxels']}")
    print("  pondInfo calls per shore query: "
          + ", ".join(f"{k} call(s) x{v}" for k, v in sorted(CALLS.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
