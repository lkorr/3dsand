"""Compare two mob-cap64 'fights x3' tables. Usage: python scripts/perf/fightcmp.py old.log new.log"""
import re, sys

def load(p):
    d = {}
    for ln in open(p, encoding='utf-8', errors='replace'):
        m = re.match(r'mob-cap64: fights x3 (\S+)\s+([-\d.]+) \[([-\d.]+) \.\. ([-\d.]+)\]', ln)
        if m:
            d[m.group(1)] = (float(m.group(2)), float(m.group(3)), float(m.group(4)))
    return d

a, b = load(sys.argv[1]), load(sys.argv[2])
keys = list(dict.fromkeys(list(a) + list(b)))
rows = []
for k in keys:
    va = a.get(k, (None,))[0]
    vb = b.get(k, (None,))[0]
    d = (vb - va) if (va is not None and vb is not None) else None
    rows.append((k, va, vb, d))
print(f"{'metric':32s} {'r4':>10s} {'head':>10s} {'delta':>9s}")
for k, va, vb, d in rows:
    fa = f"{va:10.3f}" if va is not None else "         -"
    fb = f"{vb:10.3f}" if vb is not None else "         -"
    fd = f"{d:+9.3f}" if d is not None else "        "
    print(f"{k:32s} {fa} {fb} {fd}")
