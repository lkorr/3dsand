"""Per-arm hz_sun stats from SANDVOX_FRAMES_PASSES CSVs (live,pass,ms) and the
per-frame GPU total. Usage: python scripts/perf/hzsum.py <SANDVOX_FRAMES_PASSES csv>..."""
import csv, sys, collections

def pct(v, p):
    if not v: return 0.0
    v = sorted(v); return v[min(len(v)-1, int(p*(len(v)-1)))]

for f in sys.argv[1:]:
    frames = collections.defaultdict(float)
    hz = {}
    for row in csv.DictReader(open(f)):
        fr = int(row['live']); ms = float(row['ms'])
        frames[fr] += ms
        if row['pass'] == 'hz_sun':
            hz[fr] = hz.get(fr, 0.0) + ms
    tot = list(frames.values())
    hzv = list(hz.values())
    n = len(frames)
    print(f"{f}: {n} frames | hz_sun on {len(hzv)} frames, max {max(hzv) if hzv else 0:.2f} "
          f"p50 {pct(hzv,.5):.2f} sum {sum(hzv):.1f} ms ({sum(hzv)/max(n,1):.3f}/frame) | "
          f"frame GPU p50 {pct(tot,.5):.2f} p99 {pct(tot,.99):.2f} max {max(tot):.2f} | "
          f"GPU of hz frames: max {max((frames[k] for k in hz), default=0):.2f}")
