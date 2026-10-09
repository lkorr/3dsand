#!/usr/bin/env python3
"""Write tests/river_vectors.json: the ENGINE's river / lake bake, pinned.

    python scripts/gen_river_vectors.py [--exe build/Release/sandvox.exe]

The World map page previews a river's reaches, waterfalls and profile with a
JS port of the engine's bake (assets/editor/riverbake.js, a port of
src/sim/worldwater.cpp BakeRiversLakes). This script pins the port to the
C++: it copies the `peaks` map (real relief: a 300 m cliff band, a coast) to
a temporary map, adds the rivers and lakes below (every branch of the bake:
smooth and polyline, knots without w / d, a waterfall run, a mouth in the
sea, a tributary joining it, a river sourced in a lake, one ending in a lake,
a smooth lake and a polygon lake with an edge over 256 voxels, a V profile, a
custom stepVox and bank), runs `sandvox --mapcheck` on it (GPU-free: the
loader + the bake, the same numbers the tuner's check panel gets) and writes
its input and the engine's output to tests/river_vectors.json. The temporary
map is deleted afterwards.

Every river here is short enough (< 255 samples) that --mapcheck's profile is
NOT decimated, so the table holds the bare ground at EVERY sample: the JS test
(scripts/test_riverbake.mjs) feeds that ground back in and requires each
sample's arc and water surface, the reach and piece counts, the waterfalls,
the drain level and the source / mouth lakes to be identical.

Re-run it (and commit the table) whenever worldwater.cpp's bake changes; the
JS port must change in the same commit.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = '_rivervec'

RIVERS = [
    {'id': 'cliff_run', 'name': 'over the cliff band', 'smooth': True,
     'points': [[0, -7550, 24, 6], [60, -7900], [-20, -8450, 40, 10]]},
    {'id': 'coast_run', 'name': 'into the sea', 'smooth': True, 'stepVox': 3,
     'points': [[2000, -29250, 30, 6], [2100, -29600, 36, 7], [2000, -30050, 48, 9]]},
    {'id': 'coast_trib', 'name': 'a tributary of coast_run', 'smooth': True, 'joins': 'coast_run',
     'points': [[2650, -29400, 12, 3], [2400, -29650], [2090, -29700, 16, 4]]},
    {'id': 'from_lake', 'name': 'out of the smooth lake', 'smooth': True,
     'points': [[-19700, -19700, 20, 5], [-19650, -19300], [-19700, -18900, 20, 5]]},
    {'id': 'into_lake', 'name': 'into the smooth lake', 'smooth': True,
     'points': [[-20700, -19650, 14, 4], [-20300, -19720], [-19800, -19700, 18, 5]]},
    {'id': 'polyline_v', 'name': 'a polyline, V profile', 'smooth': False, 'profile': 'V', 'stepVox': 8, 'bank': 30,
     'points': [[-5000, -12000, 20, 4], [-4900, -12300], [-5050, -12600], [-5000, -12900, 40, 8]]},
]
LAKES = [
    {'id': 'round_lake', 'name': 'a smooth lake', 'smooth': True, 'level': 1400,
     'points': [[-20000, -20000], [-19400, -20000], [-19400, -19400], [-20000, -19400]]},
    {'id': 'tri_lake', 'name': 'a polygon lake (an edge over 256)', 'smooth': False, 'level': 1500, 'band': 40,
     'points': [[5000, -15000], [5900, -15000], [5400, -14500]]},
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=os.path.join(ROOT, 'build', 'Release', 'sandvox.exe'))
    a = ap.parse_args()
    src = os.path.join(ROOT, 'assets', 'worldmap', 'peaks')
    dst = os.path.join(ROOT, 'assets', 'worldmap', TMP)
    if os.path.exists(dst):
        shutil.rmtree(dst)
    shutil.copytree(src, dst)
    try:
        with open(os.path.join(dst, 'map.json'), encoding='utf-8') as f:
            mj = json.load(f)
        mj['name'] = TMP
        mj['rivers'] = RIVERS
        mj['lakes'] = LAKES
        with open(os.path.join(dst, 'map.json'), 'w', encoding='utf-8', newline='\n') as f:
            json.dump(mj, f, indent=1)
        r = subprocess.run([a.exe, '--mapcheck', TMP], cwd=ROOT, capture_output=True, text=True, timeout=300)
    finally:
        shutil.rmtree(dst, ignore_errors=True)
    line = next((ln for ln in r.stdout.splitlines() if ln.startswith('MAPCHECK ')), None)
    if line is None:
        sys.exit('no MAPCHECK line from %s (exit %d):\n%s\n%s' % (a.exe, r.returncode, r.stdout[-2000:], r.stderr[-2000:]))
    ck = json.loads(line[len('MAPCHECK '):])
    if not ck.get('ok'):
        sys.exit('the engine refused the vector map: %s' % ck.get('error'))
    for rv in ck['rivers']:
        # consecutive samples are at most 4 voxels apart; a wider gap = decimated
        s = rv['profile']['s']
        if any(b - a > 4 for a, b in zip(s, s[1:])):
            sys.exit('river %s: --mapcheck decimated its profile (%d points), shorten it' % (rv['id'], len(s)))
    doc = {
        'about': ('THE RIVER BAKE, pinned: the engine\'s (src/sim/worldwater.cpp BakeRiversLakes) output for the '
                  'rivers and lakes below on a copy of the peaks map, as `sandvox --mapcheck` reports it. Every '
                  'profile is undecimated (one entry per 4-voxel sample), so `ground` is the bare ground the engine '
                  'sampled at every sample. scripts/test_riverbake.mjs feeds it to assets/editor/riverbake.js and '
                  'requires s, y (the water surface), reaches, pieces, waterfalls, endLevel, lakeIn / lakeOut and '
                  'each lake\'s outline size + bbox to match exactly. Written by scripts/gen_river_vectors.py.'),
        'map': 'peaks',
        'seaLevelY': mj['seaLevelY'],
        'rivers': RIVERS,
        'lakes': LAKES,
        'engine': {'rivers': ck['rivers'], 'lakes': ck['lakes'],
                   'warnings': [w for w in ck.get('warnings', []) if 'river' in w or 'lake' in w]},
    }
    out = os.path.join(ROOT, 'tests', 'river_vectors.json')
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(doc, f, indent=1)
        f.write('\n')
    print('wrote %s: %d rivers (%s), %d lakes' % (out, len(ck['rivers']),
          ', '.join('%s %d samples %d reaches %d falls' % (rv['id'], len(rv['profile']['s']), rv['reaches'], len(rv['waterfalls']))
                    for rv in ck['rivers']), len(ck['lakes'])))


if __name__ == '__main__':
    main()
