"""Summarise build/perf.json (or a given file): GPU passes, CPU scopes, counters.
usage: python scripts/perf_summary.py [perf.json] [--top N]"""
import json, sys, statistics as st
path = next((a for a in sys.argv[1:] if not a.startswith('--')), 'build/perf.json')
top = 25
if '--top' in sys.argv: top = int(sys.argv[sys.argv.index('--top') + 1])
d = json.load(open(path))
for sc in d['scenarios']:
    print(f"== {sc['id']}  hash {sc['worldHash']}  frames {sc['frames']}  note: {sc['note']}")
    se = sc['series']
    w = sorted(se['wallMs'])
    if w: print(f"  wall p50 {w[len(w)//2]:.2f}  p95 {w[int(.95*(len(w)-1))]:.2f}  max {w[-1]:.2f}")
    for key in ('gpu', 'cpu'):
        g = se.get(key)
        if isinstance(g, dict):
            rows = []
            for k, v in g.items():
                if isinstance(v, list) and v and isinstance(v[0], (int, float)):
                    rows.append((st.mean(v), st.median(v), max(v), k))
            rows.sort(reverse=True)
            print(f"  {key} nodes (mean/p50/max):")
            for m, p, x, k in rows[:12]:
                if m > 0.02: print(f"    {k:18s} {m:7.2f} {p:7.2f} {x:7.2f}")
    print("  GPU passes (us/frame):")
    ps = sorted(sc['passes'], key=lambda p: -p['usPerFrame'])
    tot = sum(p['usPerFrame'] for p in ps)
    print(f"    TOTAL {tot/1000:.2f} ms")
    for p in ps[:top]:
        print(f"    {p['name']:24s} {p['node']:14s} {p['usPerFrame']/1000:7.3f} ms")
    c = se.get('counters')
    if isinstance(c, dict):
        for k, v in c.items():
            if isinstance(v, list) and v and isinstance(v[0], (int, float)):
                print(f"  counter {k:20s} mean {st.mean(v):10.1f} max {max(v)}")
