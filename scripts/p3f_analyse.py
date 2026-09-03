"""P3-F: read a --perf run's `series` and split it median vs worst-5% frames.

Usage: python scripts/p3f_analyse.py build/perf_sprint.json [scenario-id]

Answers the one question the scenario exists for: what is a tail frame made of
that a median frame is not? Prints both columns side by side, per GPU node, per
CPU scope and per counter, so the comparison is a subtraction rather than a
recollection.
"""
import json
import sys

path = sys.argv[1] if len(sys.argv) > 1 else 'build/perf_sprint.json'
want = sys.argv[2] if len(sys.argv) > 2 else None
doc = json.load(open(path))

for sc in doc['scenarios']:
    if want and sc['id'] != want:
        continue
    if sc.get('skipped'):
        print('%s SKIPPED: %s' % (sc['id'], sc.get('skipWhy')))
        continue
    se = sc['series']
    wall = se['wallMs']
    n = len(wall)
    order = sorted(range(n), key=lambda i: wall[i])
    tail = set(order[int(n * 0.95):])           # worst 5%
    mid = set(order[int(n * 0.45):int(n * 0.55)])  # the middle decile

    def mean(key, group, block):
        v = se[block].get(key)
        if v is None:
            return 0.0
        return sum(v[i] for i in group) / max(1, len(group))

    print('=== %s: %d frames  (note: %s)' % (sc['id'], n, sc.get('note', '')))
    print('    wall  median-decile %.2f ms   worst-5%% %.2f ms   max %.2f' % (
        sum(wall[i] for i in mid) / max(1, len(mid)),
        sum(wall[i] for i in tail) / max(1, len(tail)), max(wall)))
    for block, label in (('gpu', 'GPU node'), ('cpu', 'CPU scope'),
                         ('counters', 'counter')):
        print('  ---- %s ----%s' % (label, ' ' * 10))
        rows = []
        for k in se[block]:
            a, b = mean(k, mid, block), mean(k, tail, block)
            rows.append((b - a, k, a, b))
        rows.sort(key=lambda r: -abs(r[0]))
        for d, k, a, b in rows:
            if abs(a) < 1e-9 and abs(b) < 1e-9:
                continue
            print('    %-16s median %12.3f   worst5%% %12.3f   delta %+12.3f'
                  % (k, a, b, d))
    gpu_mid = sum(mean(k, mid, 'gpu') for k in se['gpu'])
    gpu_tail = sum(mean(k, tail, 'gpu') for k in se['gpu'])
    cpu_mid = sum(mean(k, mid, 'cpu') for k in se['cpu'])
    cpu_tail = sum(mean(k, tail, 'cpu') for k in se['cpu'])
    print('  TOTALS  gpu %.2f -> %.2f    cpu %.2f -> %.2f' % (
        gpu_mid, gpu_tail, cpu_mid, cpu_tail))
    print('  unattributed ns %s names %s' % (sc['unattributed']['ns'],
                                             sc['unattributed']['names']))
    print('  top passes (us/frame):')
    for p in sorted(sc['passes'], key=lambda p: -p['usPerFrame'])[:14]:
        print('    %-22s %8.1f us  node %s' % (p['name'], p['usPerFrame'],
                                              p['node']))
