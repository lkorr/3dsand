#!/usr/bin/env python3
"""HARROWBY 09 -- the village becomes a LOCALE (docs/PLAN_map_v2.md L1).

A locale is a hand-made place on the world map that moves WITH the ground
under it (map.json `locales[]`, src/sim/worldmap.h LocaleDef). Three things
make Harrowby one, and this script does all three, deterministically:

  1. map.json gets `locales: [{"id": "harrowby", ...}]`: the village's bounds
     (the soften box), and its ANCHOR at the green with the authored ground
     height there (`anchor.y`). At load the engine measures the bare ground at
     the anchor for the seed and map in force; the difference from anchor.y
     (the locale's delta) is added to every ref of the group.
  2. refs/harrowby.json's pos.y become RELATIVE to anchor.y (a ref group named
     like a locale is stored that way). Only on the run that ADDS the locale:
     a second run must not subtract twice.
  3. The village's ground cells that are not paths move out of the map's edit
     layer (assets/worldedits/<map.json editLayer>.svedit) into the locale's
     own layer (locales/harrowby/ground.svedit). Both are SVED v2,
     ground-relative per column, so a moved column means exactly what it
     meant before. A PATH column is what scripts/paint_ground.mjs writes for
     harrowby_08_paths.sh: at most two cells, dy 0 / -1, all gravel or all
     dirt. Those stay: package R1 turns them into map.json `roads[]` and
     erases them from the map layer. Everything else in the bounds (Osric's
     cellar, scripts/paint_cellar.mjs) moves. A moved column REPLACES the
     locale's column at the same (x, z).

RE-RUNNABLE. Step 3 runs every time and moves whatever non-path columns the
map layer still holds inside the bounds (none, the second time); steps 1-2
run only while map.json has no `harrowby` locale. So after merging R1:

    python assets/worldmap/default/scripts/harrowby_09_locale.py

The first run (2026-10-08) was

    python assets/worldmap/default/scripts/harrowby_09_locale.py --anchor-y 201

with anchor.y = the bare ground at the anchor (555, 3467) under the default
map at the default seed AS THE LOADER MEASURES IT -- the `groundY` that
`sandvox.exe --mapcheck default` reports for the locale -- so the village's
delta was 0 on the day it became a locale and nothing moved. (Not the
SANDVOX_HEIGHTMAP_BARE heightmap: that one drops the refs entirely, so the
waynode routes' softening goes too, and it reads 197 at the green.)

`--dry` prints what it would do and writes nothing.
"""
import argparse
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MAP_DIR = os.path.normpath(os.path.join(HERE, '..'))
ASSETS = os.path.normpath(os.path.join(MAP_DIR, '..', '..'))
LOCALE = 'harrowby'
BOUNDS = ((330, 3090), (790, 3650))      # the soften box harrowby_ground, inclusive
ANCHOR_AT = (555, 3467)                  # the green (refs/harrowby.json harrowby/green)
PATH_MATS = {'gravel', 'dirt'}           # harrowby_08_paths.sh's two surfaces

MAGIC, FLAG_RELATIVE = 0x44455653, 1


def read_sved(path):
    """-> (seed, {(x, z): [(dy, word), ...]}) in file order; v2 relative only."""
    if not os.path.isfile(path):
        return 1337, {}
    b = open(path, 'rb').read()
    magic, ver, count, _vc, seed, flags = struct.unpack_from('<6I', b, 0)
    if magic != MAGIC or ver != 2 or not (flags & FLAG_RELATIVE):
        sys.exit('%s: not an SVED v2 ground-relative layer' % path)
    cols, p = {}, 32
    for _ in range(count):
        x, z, n = struct.unpack_from('<iiI', b, p)
        p += 12
        cols[(x, z)] = [struct.unpack_from('<iI', b, p + 8 * j) for j in range(n)]
        p += 8 * n
    return seed, cols


def write_sved(path, seed, cols, dry):
    keys = sorted(cols)
    cells = sum(len(cols[k]) for k in keys)
    out = bytearray(struct.pack('<8I', MAGIC, 2, len(keys), cells, seed, FLAG_RELATIVE, 0, 0))
    for (x, z) in keys:
        out += struct.pack('<iiI', x, z, len(cols[(x, z)]))
        for dy, w in cols[(x, z)]:
            out += struct.pack('<iI', dy, w)
    print('  %s: %d columns, %d cells%s' % (os.path.relpath(path, ASSETS), len(keys), cells, ' (dry)' if dry else ''))
    if not dry:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path + '.tmp', 'wb') as f:
            f.write(out)
        os.replace(path + '.tmp', path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--anchor-y', type=int, help='the authored ground at the anchor (first run only)')
    ap.add_argument('--dry', action='store_true')
    a = ap.parse_args()

    map_path = os.path.join(MAP_DIR, 'map.json')
    with open(map_path, encoding='utf-8') as f:
        mj = json.load(f)
    locales = mj.get('locales') or []
    loc = next((l for l in locales if l.get('id') == LOCALE), None)

    # ---- 1 + 2: the locale and its relative refs (first run only) -------------
    if loc is None:
        if a.anchor_y is None:
            sys.exit('map.json has no "%s" locale yet: pass --anchor-y <ground at %s>' % (LOCALE, ANCHOR_AT))
        loc = {
            'id': LOCALE, 'name': 'Harrowby', 'icon': 'village',
            'bounds': {'min': list(BOUNDS[0]), 'max': list(BOUNDS[1])},
            'anchor': {'at': list(ANCHOR_AT), 'y': a.anchor_y},
            'about': "The hamlet north of the spawn (docs/EDITOR_GUIDE.md section 14). Its refs "
                     "(refs/harrowby.json) are stored with y RELATIVE to anchor.y; at load the engine adds "
                     "the bare ground at anchor.at for the seed in force, so the whole village rides the "
                     "ground if the world under it is reshaped. Its own ground cells are "
                     "locales/harrowby/ground.svedit.",
        }
        # Before editLayer, after rules: key order is the file's layout.
        out = {}
        for k, v in mj.items():
            if k == 'editLayer' and 'locales' not in out:
                out['locales'] = [loc]
            out[k] = v
        if 'locales' not in out:
            out['locales'] = [loc]
        print('map.json: + locale "%s" anchored at %s y %d' % (LOCALE, ANCHOR_AT, a.anchor_y))
        refs_path = os.path.join(MAP_DIR, 'refs', LOCALE + '.json')
        with open(refs_path, encoding='utf-8') as f:
            text = f.read()
        pos = re.compile(r'"pos": \[(-?\d+), (-?\d+), (-?\d+)\]')
        lines, n = [], 0
        for line in text.split('\n'):
            if '"id": "%s/' % LOCALE in line:
                m = pos.search(line)
                if m:
                    x, y, z = (int(v) for v in m.groups())
                    line = line[:m.start()] + '"pos": [%d, %d, %d]' % (x, y - a.anchor_y, z) + line[m.end():]
                    n += 1
            lines.append(line)
        print('refs/%s.json: %d ref(s) to y relative to %d' % (LOCALE, n, a.anchor_y))
        if not a.dry:
            with open(map_path, 'w', encoding='utf-8', newline='\n') as f:
                f.write(json.dumps(out, indent=2) + '\n')
            with open(refs_path, 'w', encoding='utf-8', newline='\n') as f:
                f.write('\n'.join(lines))
    elif a.anchor_y is not None and a.anchor_y != loc['anchor']['y']:
        sys.exit('the locale exists with anchor.y %d; --anchor-y is for the first run only' % loc['anchor']['y'])

    # ---- 3: the ground cells ---------------------------------------------------------
    with open(os.path.join(ASSETS, 'materials', 'materials.json'), encoding='utf-8') as f:
        mats = json.load(f)['materials']
    path_ids = {i + 1 for i, m in enumerate(mats) if m.get('id') in PATH_MATS}   # id = index + 1
    layer = mj.get('editLayer') or ''
    if not layer:
        print('map.json names no edit layer: no ground to move')
        return
    src_path = os.path.join(ASSETS, 'worldedits', layer + '.svedit')
    dst_path = os.path.join(MAP_DIR, 'locales', LOCALE, loc.get('editLayer') or 'ground') + '.svedit'
    seed, src = read_sved(src_path)
    _, dst = read_sved(dst_path)
    (x0, z0), (x1, z1) = BOUNDS

    def is_path(cells):
        return (len(cells) <= 2 and all(dy in (0, -1) for dy, _ in cells) and
                len({w & 0xFFF for _, w in cells}) == 1 and (cells[0][1] & 0xFFF) in path_ids)

    moved = [k for k in sorted(src) if x0 <= k[0] <= x1 and z0 <= k[1] <= z1 and not is_path(src[k])]
    kept = sum(1 for k in src if x0 <= k[0] <= x1 and z0 <= k[1] <= z1) - len(moved)
    print('ground: %d column(s) move to the locale, %d path column(s) stay in %s' % (len(moved), kept, layer))
    if not moved:
        return
    for k in moved:
        dst[k] = src.pop(k)
    write_sved(src_path, seed, src, a.dry)
    write_sved(dst_path, seed, dst, a.dry)


if __name__ == '__main__':
    main()
