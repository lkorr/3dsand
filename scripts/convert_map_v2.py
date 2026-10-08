#!/usr/bin/env python3
"""Convert a v1 world map (map.json + map.svmap) to the v2 format.

    python scripts/convert_map_v2.py <name> [--assets DIR] [--keep-v1]

Writes assets/worldmap/<name>/map.json (version 2) and the plane tiles
assets/worldmap/<name>/planes/{height,biome}/<tx>_<tz>.svt, then deletes
map.svmap (unless --keep-v1). docs/PLAN_map_v2.md "The v2 map format" is the
contract; src/sim/worldmap.h is the engine's authority.

WHAT THE HEIGHT PLANE GETS. A v1 column's ground was

    homeY + fine + ((baseHeight + curve(landform + range) - homeY) * ws) >> 14

with `ws` the calm-area fade (spawn and pad box). v2 splits that into an
AUTHORED height and seeded noise. Each height post gets the noise-free part:

    authored = homeY + (baseHeight + landformDev - homeY) * ws

where landformDev is the v1 landform plane read with the v1 bilinear over
cell centres (and the v1 ocean fade past its edge), the declared landform
sites baked in UNCLAMPED (no u8 saturation), and ws the v1 calm fade. The v1
biome curve is applied only if a biome authors a non-identity curve -- every
shipped biome carries the identity, so the converter refuses a non-identity
set rather than guess at a shape (the owner is redesigning the world; a
converter that is "close" is the brief, not one that is exact).

The range rung (seeded) and the fine rungs stay noise: v2's landAt adds them
on top, with the calm fade on the range rung only, as v1 did.

WHAT THE BIOME PLANE GETS. The nearest v1 cell at each 25.6 m cell centre;
the v1 warp is not baked (the author repaints). The palette is unchanged.

Ocean tiles (every sample equal to the plane default) are not written.
"""
import argparse
import json
import math
import os
import struct
import sys

import numpy as np

TILE = 256
TERRAIN_ABOUT = ("The terrain, per MAP: the seeded NOISE on the authored height plane (map v2). Lengths in voxels at refVoxelsPerMetre; log2 cells are shifts; fbmAtten / sedFraction / sedSlope are Q8 counts. The ground is planes.height (absolute voxels) + range/hill/detail/grain (the seeded octave ladder; the biome curve shapes the range rung); homeArea is the calm noise around the spawn site; the sediment wedge and the treeline are what they were as knobs. baseHeight is the height used only when no plane is loaded. Per-biome relief is each biome file's `terrain` block.")
HEIGHT_LOG2 = 6
BIOME_LOG2 = 8
SVT_MAGIC = 0x31545653   # 'SVT1' little-endian
PLANE_HEIGHT, PLANE_BIOME = 1, 2


def write_tile(path, kind, tx, tz, samples, sample_bytes):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    hdr = struct.pack('<IIiiIIII', SVT_MAGIC, kind, tx, tz, TILE, TILE, sample_bytes, 0)
    dt = '<i2' if sample_bytes == 2 else 'u1'
    with open(path + '.tmp', 'wb') as f:
        f.write(hdr)
        f.write(np.ascontiguousarray(samples, dtype=dt).tobytes())
    os.replace(path + '.tmp', path)


def write_plane(dirpath, kind, plane, default, sample_bytes):
    """Split a (posts_z, posts_x) plane into 256x256 tiles; skip all-default ones."""
    pz, px = plane.shape
    n = 0
    if os.path.isdir(dirpath):
        for f in os.listdir(dirpath):
            if f.endswith('.svt'):
                os.remove(os.path.join(dirpath, f))
    for tz in range((pz + TILE - 1) // TILE):
        for tx in range((px + TILE - 1) // TILE):
            t = np.full((TILE, TILE), default, dtype=np.int32)
            blk = plane[tz * TILE:(tz + 1) * TILE, tx * TILE:(tx + 1) * TILE]
            t[:blk.shape[0], :blk.shape[1]] = blk
            if np.all(t == default):
                continue
            write_tile(os.path.join(dirpath, '%d_%d.svt' % (tx, tz)), kind, tx, tz, t, sample_bytes)
            n += 1
    return n


def landform_sites_overlay(acc, sites, rng, log2, w, h, ocx, ocz):
    """worldmap.cpp OverlayLandformSites in floats, WITHOUT the byte clamp."""
    half = 1 << (log2 - 1)
    cz, cx = np.mgrid[0:h, 0:w]
    wx = ((cx - ocx) << log2) + half
    wz = ((cz - ocz) << log2) + half
    for s in sites:
        r = max(1, int(s.get('radius', 1024)))
        hv = abs(int(s.get('heightVox', 0)))
        amp = hv * 256.0 / max(1, rng)
        shape = s.get('shape', 'peak')
        if shape == 'basin':
            amp = -amp
        if amp == 0:
            continue
        dx = (wx - s['at'][0]).astype(np.float64)
        dz = (wz - s['at'][1]).astype(np.float64)
        if shape == 'ridge':
            a = math.radians(int(s.get('rotation', 0)))
            u = dx * math.cos(a) + dz * math.sin(a)
            v = -dx * math.sin(a) + dz * math.cos(a)
            b = max(1.0, r / 3.0)
            t = np.sqrt((u / r) ** 2 + (v / b) ** 2)
        else:
            t = np.sqrt(dx * dx + dz * dz) / r
        if shape == 'plateau':
            f = np.where(t <= 0.6, 1.0, (5.0 - 5.0 * t) / 2.0)
        else:
            f = 1.0 - t
        acc += np.where(t < 1.0, amp * f, 0.0)


def smooth01(t):
    t = np.clip(t, 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('--assets', default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'assets'))
    ap.add_argument('--keep-v1', action='store_true')
    a = ap.parse_args()
    d = os.path.join(a.assets, 'worldmap', a.name)
    with open(os.path.join(d, 'map.json'), encoding='utf-8') as f:
        j = json.load(f)
    if j.get('version') == 2:
        sys.exit('%s is already a v2 map' % a.name)
    log2 = int(j.get('cellLog2', 10))
    w, h = j['size']
    ocx, ocz = j.get('originCell', [0, 0])
    with open(os.path.join(d, 'map.svmap'), 'rb') as f:
        raw = f.read()
    magic, ver, fw, fh = struct.unpack_from('<IIII', raw, 0)
    if magic != 0x504D5653 or ver != 1 or (fw, fh) != (w, h):
        sys.exit('map.svmap does not match map.json')
    cells = w * h
    biome = np.frombuffer(raw, np.uint8, cells, 16).reshape(h, w)
    land = np.frombuffer(raw, np.uint8, cells, 16 + cells).reshape(h, w)

    te = j.get('terrain', {})
    base = int(te.get('baseHeight', 200))
    rng = int(te.get('landformRangeVox', 1024))
    home = te.get('homeArea', {})
    homeY, homeR, homeFade = int(home.get('y', 200)), int(home.get('radius', 320)), max(1, int(home.get('fade', 2048)))
    # The biome curves: the converter only knows the identity (all shipped
    # biomes); anything else needs the engine's Hermite and is refused.
    ident = [-16384 + i * 4096 for i in range(9)]
    for bname in j['biomes']:
        with open(os.path.join(a.assets, 'biomes', bname + '.json'), encoding='utf-8') as f:
            bj = json.load(f)
        cv = (bj.get('terrain') or {}).get('curve')
        if cv is not None and list(cv) != ident:
            sys.exit('biome %s authors a non-identity terrain.curve; convert_map_v2.py bakes the identity only' % bname)

    # ---- the landform plane, Q8, with the sites baked unclamped ----
    acc = land.astype(np.float64)
    lsites = [s for s in j.get('sites', []) if s.get('kind') == 'landform']
    landform_sites_overlay(acc, lsites, rng, log2, w, h, ocx, ocz)

    X0, Z0 = -ocx << log2, -ocz << log2
    W, H = w << log2, h << log2
    px, pz = (W >> HEIGHT_LOG2) + 1, (H >> HEIGHT_LOG2) + 1
    xs = X0 + np.arange(px, dtype=np.int64) * (1 << HEIGHT_LOG2)
    zs = Z0 + np.arange(pz, dtype=np.int64) * (1 << HEIGHT_LOG2)
    half = 1 << (log2 - 1)

    def lerp_axis(coords, origin):
        m = coords - half + (origin << log2)
        c = m >> log2
        f = (m & ((1 << log2) - 1)) / float(1 << log2)
        return c, f

    cxs, fxs = lerp_axis(xs, ocx)
    czs, fzs = lerp_axis(zs, ocz)
    cx0, cx1 = np.clip(cxs, 0, w - 1), np.clip(cxs + 1, 0, w - 1)
    cz0, cz1 = np.clip(czs, 0, h - 1), np.clip(czs + 1, 0, h - 1)
    a_ = acc[np.ix_(cz0, cx0)] * (1 - fxs)[None, :] + acc[np.ix_(cz0, cx1)] * fxs[None, :]
    b_ = acc[np.ix_(cz1, cx0)] * (1 - fxs)[None, :] + acc[np.ix_(cz1, cx1)] * fxs[None, :]
    v = a_ * (1 - fzs)[:, None] + b_ * fzs[:, None]
    # the v1 ocean fade, in cells past the plane's edge (Chebyshev)
    fade = max(int(j.get('oceanFadeCells', 0)), 1)
    dOut = np.maximum(np.maximum(-cxs, cxs + 1 - w)[None, :], np.maximum(-czs, czs + 1 - h)[:, None])
    v = np.where(dOut <= 0, v, v * np.maximum(fade - dOut, 0) / fade)
    dev = (v * 256.0 - 32768.0) * rng / 65536.0
    authored = base + dev

    # ---- the calm-area fade, baked (v1 faded the coarse relief toward homeY) ----
    sp = next((s for s in j.get('sites', []) if s.get('kind') == 'spawn'), None)
    sx, sz = (sp['at'] if sp else (140, 140))
    dsp = np.maximum(np.abs(xs - sx)[None, :], np.abs(zs - sz)[:, None]) - homeR
    t = np.clip(dsp / homeFade, 0.0, 1.0)
    pad = next((s for s in j.get('sites', []) if s.get('kind') == 'pad'), None)
    if pad:
        (x0, z0), (x1, z1) = pad['min'], pad['max']
        dpad = np.maximum(np.maximum(np.maximum(x0 - xs, xs - x1)[None, :], np.maximum(z0 - zs, zs - z1)[:, None]), 0)
        t = np.minimum(t, np.clip(dpad / homeFade, 0.0, 1.0))
    ws = smooth01(t)
    authored = homeY + (authored - homeY) * ws
    hplane = np.clip(np.floor(authored + 0.5), -32000, 32000).astype(np.int32)
    hdefault = int(base - rng // 2)

    # ---- the biome plane: nearest v1 cell at each cell centre ----
    bpx, bpz = (W >> BIOME_LOG2) + 1, (H >> BIOME_LOG2) + 1
    bxs = X0 + np.arange(bpx, dtype=np.int64) * (1 << BIOME_LOG2) + (1 << (BIOME_LOG2 - 1))
    bzs = Z0 + np.arange(bpz, dtype=np.int64) * (1 << BIOME_LOG2) + (1 << (BIOME_LOG2 - 1))
    bcx = (bxs >> log2) + ocx
    bcz = (bzs >> log2) + ocz
    pal = j['biomes']
    bdefault = pal.index('ocean') if 'ocean' in pal else 0
    inside = ((bcx >= 0) & (bcx < w))[None, :] & ((bcz >= 0) & (bcz < h))[:, None]
    bplane = np.where(inside, biome[np.ix_(np.clip(bcz, 0, h - 1), np.clip(bcx, 0, w - 1))], bdefault).astype(np.int32)

    # ---- map.json v2 ----
    out = {}
    for k, val in j.items():
        if k in ('cellLog2', 'size', 'originCell', 'oceanFadeCells'):
            continue
        out[k] = val
    out = {'version': 2, **{k: v2 for k, v2 in out.items() if k != 'version'}}
    out['extent'] = {'min': [X0, Z0], 'size': [W, H]}
    out['siteCellLog2'] = log2
    out['planes'] = {'height': {'log2': HEIGHT_LOG2, 'tile': TILE, 'default': hdefault},
                     'biome': {'log2': BIOME_LOG2, 'tile': TILE, 'default': pal[bdefault]}}
    out['edgeFadeVox'] = int(j.get('oceanFadeCells', 0)) << log2
    out['warpAmpVox'] = min(int(j.get('warpAmpVox', 0)), (1 << BIOME_LOG2) // 4 - 16)
    if 'terrain' in out:
        t2 = dict(out['terrain'])
        t2.pop('landformRangeVox', None)
        t2['about'] = TERRAIN_ABOUT
        out['terrain'] = t2
    out['sites'] = [s for s in j.get('sites', []) if s.get('kind') != 'landform']
    # keep a sensible key order: the header fields first
    order = ['version', 'name', 'about', 'extent', 'siteCellLog2', 'planes', 'seaLevelY', 'edgeFadeVox',
             'warpAmpVox', 'terrain', 'biomes', 'sites', 'rules', 'editLayer']
    out = {k: out[k] for k in order if k in out} | {k: v2 for k, v2 in out.items() if k not in order}

    nh = write_plane(os.path.join(d, 'planes', 'height'), PLANE_HEIGHT, hplane, hdefault, 2)
    nb = write_plane(os.path.join(d, 'planes', 'biome'), PLANE_BIOME, bplane, bdefault, 1)
    with open(os.path.join(d, 'map.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(out, f, indent=2)
        f.write('\n')
    if not a.keep_v1:
        os.remove(os.path.join(d, 'map.svmap'))
    print('%s: extent %s + %s, height %dx%d posts (%d tiles, y%d..y%d, default %d), biome %dx%d (%d tiles), '
          '%d landform site(s) baked' % (a.name, (X0, Z0), (W, H), px, pz, nh, int(hplane.min()), int(hplane.max()),
                                         hdefault, bpx, bpz, nb, len(lsites)))


if __name__ == '__main__':
    main()
