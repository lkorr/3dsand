#!/usr/bin/env python3
"""Generate the `peaks` test map: assets/worldmap/peaks/ (map v2).

    python scripts/gen_map_peaks.py [--assets DIR]

THE FIXTURE EVERY LATER MAP-V2 WAVE USES (docs/PLAN_map_v2.md F1 item 7;
`SANDVOX_MAP=peaks`). An 8 km x 8 km authored world with Skyrim-tall relief,
so altitude, horizon, roads and perf work have real mountains to stand on:

  * a COAST along the south edge (sea level y 112), the land rising north;
  * FOOTHILLS between the coast and the range, 30..200 m;
  * a 1,400 m MASSIF (summit y 14112, 1,400 m above the sea) north-east of
    centre, with a KNIFE RIDGE running west from the summit (a sharp V crest
    falling from ~1,250 m to ~900 m);
  * a 300 m CLIFF BAND: an escarpment across the foothills, 3,000 voxels
    over ~400 horizontal (the steepest ground the map holds);
  * a 600 m-deep GORGE cut through the massif's south flank, V-sided with a
    narrow floor, draining toward the coast;
  * a HIGH VALLEY at ~900 m west of the massif, ringed by higher ground, with
    an authored tarn (a `water` site) on its floor and the SPAWN beside it.

Biomes follow altitude and slope: ocean below the sea, meadow on the coast
plain and the high valley's floor, pine on the foothills and the lower
mountain, tundra on the high shoulders, alpine on the summits and the ridge.

Deterministic: pure arithmetic on the post lattice, no randomness. Re-running
rewrites the tiles and map.json identically (the map is generated, not
painted -- edit THIS script, not the tiles, to change it).
"""
import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from convert_map_v2 import write_plane, PLANE_HEIGHT, PLANE_BIOME   # noqa: E402

SEA = 112
VPM = 10                      # voxels per metre
X0 = Z0 = -40960              # the extent: 80 site cells of 1024 voxels
SIZE = 81920
HLOG2, BLOG2 = 6, 8
PALETTE = ['ocean', 'meadow', 'pine', 'tundra', 'alpine']
OCEAN, MEADOW, PINE, TUNDRA, ALPINE = range(5)

# Feature placement, world voxels.
SUMMIT = (9000.0, 16000.0)
VALLEY = (-13000.0, 14000.0)          # the high valley's centre
SPAWN = (-12400, 13400)
TARN = (-13400, 14600)


def m(v):
    return v * VPM


def smooth(t):
    t = np.clip(t, 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def height_field(x, z):
    """Absolute ground height in voxels at world (x, z) (arrays)."""
    # ---- the base: a coastal ramp rising north, rolling foothills ----
    coast = -30000.0                                    # the shoreline's z
    land = smooth((z - coast + 6000.0) / 14000.0)       # 0 at sea, 1 inland
    sea_floor = SEA - m(60)
    foot = (m(40) + m(70) * land
            + m(35) * np.sin(x / 2300.0) * np.cos(z / 3100.0)
            + m(20) * np.sin((x + z) / 1700.0))
    h = sea_floor + (SEA + foot - sea_floor) * smooth((z - coast + 1500.0) / 3000.0)

    # ---- the 300 m cliff band: an escarpment across the foothills; the land
    # north of it stands 300 m higher (a smoothstep over 420 voxels: ~10
    # voxels per voxel at its steepest) ----
    cz = -8000.0 + 900.0 * np.sin(x / 5000.0)
    band = smooth((z - cz) / 420.0) * smooth((x + 30000.0) / 4000.0) * smooth((22000.0 - x) / 4000.0)
    h = h + m(300) * band

    # ---- the massif: a broad dome with a summit cone, 1,400 m above the sea ----
    dx, dz = x - SUMMIT[0], z - SUMMIT[1]
    r = np.sqrt(dx * dx + dz * dz)
    massif = m(900) * smooth(1.0 - r / 16000.0) + m(500) * smooth(1.0 - r / 5000.0) ** 1.5
    # ---- the knife ridge: west from the summit, a sharp V crest ----
    along = np.clip(-dx / 15000.0, 0.0, 1.0)            # 0 at the summit, 1 at its west end
    across = np.abs(dz - 600.0 * np.sin(-dx / 2500.0))
    crest = m(1250) - m(350) * along                    # crest height above the sea
    ridge = np.where((dx <= 0) & (dx >= -15000), crest * np.clip(1.0 - across / 2200.0, 0.0, 1.0)
                     * smooth((dx + 15000.0) / 2000.0), 0.0)
    mountain = np.maximum(massif, ridge) * smooth((z - (coast + 9000.0)) / 6000.0)
    h = np.where(mountain > 1.0, np.maximum(h, SEA + mountain), h)

    # ---- the high valley: a flat floor at ~900 m, ringed by higher ground ----
    vx, vz = x - VALLEY[0], z - VALLEY[1]
    vr = np.sqrt((vx / 1.4) ** 2 + vz ** 2)
    rim = SEA + m(1080) + m(60) * np.sin(np.arctan2(vz, vx) * 3.0)
    floor = SEA + m(900) + m(6) * np.sin(vx / 700.0) * np.cos(vz / 900.0)
    ring = np.maximum(rim, h)
    inside = floor + (ring - floor) * smooth((vr - 2600.0) / 1800.0)
    outer = smooth(1.0 - (vr - 4400.0) / 3500.0)        # the ring easing out into the land around
    h = np.where(vr < 4400.0, inside, np.where(h < rim, h + (rim - h) * outer, h))

    # ---- the 600 m gorge: V-sided, narrow floor, south through the massif's flank ----
    gx = 14000.0 + 1800.0 * np.sin(z / 3000.0)          # the gorge's centreline x(z)
    gd = np.abs(x - gx)
    gorge_on = smooth((z - 3000.0) / 3000.0) * smooth((28000.0 - z) / 3000.0)
    depth = m(600) * np.clip(1.0 - np.maximum(gd - 100.0, 0.0) / 1400.0, 0.0, 1.0) ** 1.2
    # never below 40 m above the sea: a gorge, not a fjord the sea fill floods
    h = np.maximum(h - depth * gorge_on, np.minimum(h, SEA + m(40)))
    return np.clip(np.rint(h), -32000, 32000).astype(np.int32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--assets', default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'assets'))
    a = ap.parse_args()
    d = os.path.join(a.assets, 'worldmap', 'peaks')
    os.makedirs(d, exist_ok=True)

    px = (SIZE >> HLOG2) + 1
    xs = X0 + np.arange(px, dtype=np.float64) * (1 << HLOG2)
    zs = Z0 + np.arange(px, dtype=np.float64) * (1 << HLOG2)
    hz, hx = np.meshgrid(zs, xs, indexing='ij')
    hplane = height_field(hx, hz)
    hdefault = SEA - m(60)

    # biomes at cell centres, from the authored height and its slope there
    bp = (SIZE >> BLOG2) + 1
    bx = X0 + np.arange(bp, dtype=np.float64) * (1 << BLOG2) + (1 << (BLOG2 - 1))
    bz = Z0 + np.arange(bp, dtype=np.float64) * (1 << BLOG2) + (1 << (BLOG2 - 1))
    gz_, gx_ = np.meshgrid(bz, bx, indexing='ij')
    bh = height_field(gx_, gz_).astype(np.float64)
    e = 64.0
    sl = np.hypot(height_field(gx_ + e, gz_) - height_field(gx_ - e, gz_),
                  height_field(gx_, gz_ + e) - height_field(gx_, gz_ - e)) / (2 * e)
    alt = (bh - SEA) / VPM                                # metres above the sea
    vr = np.sqrt(((gx_ - VALLEY[0]) / 1.4) ** 2 + (gz_ - VALLEY[1]) ** 2)
    biome = np.full(bh.shape, PINE, dtype=np.int32)
    biome[alt < 120] = MEADOW
    biome[(alt >= 120) & (alt < 950)] = PINE
    biome[(alt >= 950) & (alt < 1150)] = TUNDRA
    biome[alt >= 1150] = ALPINE
    biome[(sl > 1.2) & (alt >= 600)] = ALPINE            # cliff faces read as bare mountain
    biome[vr < 2800] = MEADOW                             # the high valley's floor
    biome[alt < 0.5] = OCEAN

    nh = write_plane(os.path.join(d, 'planes', 'height'), PLANE_HEIGHT, hplane, hdefault, 2)
    nb = write_plane(os.path.join(d, 'planes', 'biome'), PLANE_BIOME, biome, OCEAN, 1)
    with open(os.path.join(a.assets, 'worldmap', 'default', 'map.json'), encoding='utf-8') as f:
        terrain = json.load(f)['terrain']
    mj = {
        'version': 2,
        'name': 'peaks',
        'about': ('THE MAP V2 TEST FIXTURE (docs/PLAN_map_v2.md F1): 8 km of Skyrim-tall relief -- a 1,400 m '
                  'massif with a knife ridge, a 300 m cliff band, a 600 m gorge, a high valley at ~900 m with a '
                  'tarn, foothills and a coast. GENERATED by scripts/gen_map_peaks.py: edit the script, not the '
                  'tiles. Load it with SANDVOX_MAP=peaks.'),
        'extent': {'min': [X0, Z0], 'size': [SIZE, SIZE]},
        'siteCellLog2': 10,
        'planes': {'height': {'log2': HLOG2, 'tile': 256, 'default': int(hdefault)},
                   'biome': {'log2': BLOG2, 'tile': 256, 'default': 'ocean'}},
        'seaLevelY': SEA,
        'edgeFadeVox': 6144,
        'warpAmpVox': 48,
        'terrain': terrain,
        'biomes': PALETTE,
        'sites': [
            {'id': 'spawn', 'kind': 'spawn', 'at': list(SPAWN),
             'about': 'On the high valley floor (~900 m), beside the tarn.'},
            {'id': 'valley_tarn', 'kind': 'water', 'preset': 'tarn', 'at': list(TARN),
             'about': 'The high valley lake.'},
        ],
        'rules': [],
    }
    with open(os.path.join(d, 'map.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(mj, f, indent=2)
        f.write('\n')
    print('peaks: %dx%d height posts (%d tiles, y%d..y%d), %dx%d biome cells (%d tiles); summit %.0f m, valley %.0f m' % (
        px, px, nh, int(hplane.min()), int(hplane.max()), bp, bp, nb,
        (hplane.max() - SEA) / VPM, (height_field(np.array([VALLEY[0]]), np.array([VALLEY[1]]))[0] - SEA) / VPM))


if __name__ == '__main__':
    main()
