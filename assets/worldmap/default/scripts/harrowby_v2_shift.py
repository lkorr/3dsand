#!/usr/bin/env python3
"""Keep Harrowby on its ground across the map v2 conversion (docs/PLAN_map_v2.md F1 item 6).

The village's refs carry ABSOLUTE pos.y (refs/harrowby.json). Converting the
`default` map to v2 (scripts/convert_map_v2.py) re-expresses the ground as an
authored height plane plus noise, so the bare ground under a structure can
move by a few voxels. This measures the bare ground (no structure pads:
SANDVOX_HEIGHTMAP_BARE=1) at every structure ref's column before and after
and moves the WHOLE village -- every ref of the group: structures, waynodes,
NPCs, markers, readables -- by the MEDIAN delta. Rigidly, not per structure:
the houses share one floor height (their pads may then overlap freely,
worldmap.cpp's overlap rule) and the routes and villagers between them were
authored against it; a per-structure shift would tilt the village apart.
That is also what L1's locale anchor will do (one delta for the locale).

    # 1. BEFORE converting, with an exe that reads v1 maps:
    python harrowby_v2_shift.py --record-v1 <sandvox.exe> [--assets <v1 assets dir>]
    # 2. AFTER converting, with an exe that reads v2 maps:
    python harrowby_v2_shift.py --apply <sandvox.exe> [--assets <assets dir>]

Step 1 writes harrowby_v1_ground.json beside this script (committed: the v1
map files are gone after the conversion, so the "before" is a record, not
something to recompute). L1 (locales) replaces this shim with locale anchors.
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REFS = os.path.join(HERE, '..', 'refs', 'harrowby.json')
RECORD = os.path.join(HERE, 'harrowby_v1_ground.json')
# The grid: one voxel per sample over the whole village.
CX, CZ, SPAN = 560, 3320, 512


def run_sh():
    # assets/worldmap/default/scripts -> repo root -> scripts/run.sh
    return os.path.normpath(os.path.join(HERE, '..', '..', '..', '..', 'scripts', 'run.sh')).replace(os.sep, '/')


def ground(exe, assets):
    out = os.path.join(tempfile.mkdtemp(), 'hm.bin')
    env = dict(os.environ, SANDVOX_MAP='default', SANDVOX_HEIGHTMAP_BARE='1', SANDVOX_NO_CRASH_DIALOG='1')
    if assets:
        env['SANDVOX_ASSET_DIR'] = assets
    # shutil.which: a bare 'bash' resolves to System32's WSL bash on Windows.
    subprocess.run([shutil.which('bash') or 'bash', run_sh(), exe, '--heightmap', '%d,%d,%d,%d' % (CX, CZ, SPAN, SPAN),
                    '--heightmap-out', out], env=env, check=True)
    with open(out, 'rb') as f:
        b = f.read()
    res = struct.unpack_from('<I', b, 8)[0]

    def at(x, z):
        i, k = x - (CX - SPAN // 2), z - (CZ - SPAN // 2)
        if not (0 <= i < res and 0 <= k < res):
            sys.exit('column (%d,%d) is outside the measured grid' % (x, z))
        return struct.unpack_from('<i', b, 48 + (k * res + i) * 8)[0]
    return at


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--record-v1', metavar='EXE')
    ap.add_argument('--apply', metavar='EXE')
    ap.add_argument('--assets')
    a = ap.parse_args()
    with open(REFS, encoding='utf-8') as f:
        doc = json.load(f)
    structs = [r for r in doc['refs'] if r.get('kind') == 'structure']
    if a.record_v1:
        at = ground(a.record_v1, a.assets)
        rec = {r['id']: at(r['pos'][0], r['pos'][2]) for r in structs}
        with open(RECORD, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(rec, f, indent=2)
            f.write('\n')
        print('recorded v1 bare ground at %d structures -> %s' % (len(rec), RECORD))
        return
    if not a.apply:
        sys.exit('--record-v1 EXE or --apply EXE')
    with open(RECORD, encoding='utf-8') as f:
        rec = json.load(f)
    at = ground(a.apply, a.assets)
    delta = {r['id']: at(r['pos'][0], r['pos'][2]) - rec[r['id']] for r in structs}
    for r in structs:
        print('%-24s v1 %4d  v2 %4d  delta %+d' % (r['id'], rec[r['id']], rec[r['id']] + delta[r['id']], delta[r['id']]))
    ds = sorted(delta.values())
    d = ds[len(ds) // 2]
    print('the village moves by the median delta %+d' % d)
    if d == 0:
        print('refs unchanged')
        return
    # Rewrite pos.y in place, ref by ref, so the file's one-ref-per-line
    # layout (and every other byte) survives.
    with open(REFS, encoding='utf-8') as f:
        text = f.read()
    for r in doc['refs']:
        old = '"id": "%s", ' % r['id']
        i = text.find(old)
        p = text.find('"pos": [%d, %d, %d]' % tuple(r['pos']), i)
        if i < 0 or p < 0:
            sys.exit('could not find %s pos in %s' % (r['id'], REFS))
        newpos = '"pos": [%d, %d, %d]' % (r['pos'][0], r['pos'][1] + d, r['pos'][2])
        text = text[:p] + newpos + text[p + len('"pos": [%d, %d, %d]' % tuple(r['pos'])):]
    with open(REFS, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print('shifted refs in %s' % REFS)


if __name__ == '__main__':
    main()
