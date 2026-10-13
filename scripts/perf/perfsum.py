"""Summarise --perf json files: frame wall percentiles, CPU scopes, GPU nodes,
counters. Usage: python scripts/perf/perfsum.py <json>... [--top N] [--cpu stream,...]"""
import json, sys

def pct(v, p):
    if not v: return 0.0
    v = sorted(v)
    return v[min(len(v) - 1, int(p * (len(v) - 1)))]

args = [a for a in sys.argv[1:] if not a.startswith('--')]
want_nodes = ['worldgen', 'farField', 'pageTable', 'caLoop', 'raymarch', 'openness', 'shadowCache', 'fluidSys']
want_cnt = ['windowShifts', 'shiftWakeWaitMs', 'shiftCpuMs', 'pageFills', 'pageFillsJitter',
            'cpuDirtyChunks', 'pagesResident', 'pagesHeldDirty', 'pagesHeldMatter', 'ticksThisFrame']
for f in args:
    d = json.load(open(f))
    sc = d['scenarios'][0]
    s = sc['series']
    wall = s['wallMs']
    print(f"== {f}: {sc['id']} frames {len(wall)} hash {sc['worldHash']}")
    print(f"   wall p50 {pct(wall,.5):.2f} p95 {pct(wall,.95):.2f} p99 {pct(wall,.99):.2f} max {max(wall):.2f} >33 {sum(1 for w in wall if w>33)}")
    cpu = s['cpu']
    keys = d['scopes'] if isinstance(d['scopes'], list) else list(cpu.keys())
    if isinstance(cpu, dict):
        for k, v in cpu.items():
            if max(v) > 0.5:
                print(f"   cpu {k:14s} mean {sum(v)/len(v):6.3f} p95 {pct(v,.95):6.3f} p99 {pct(v,.99):6.3f} max {max(v):7.3f}")
    gpu = s['gpu']
    if isinstance(gpu, dict):
        for k, v in gpu.items():
            if k in want_nodes or max(v) > 2:
                print(f"   gpu {k:14s} mean {sum(v)/len(v):6.3f} p95 {pct(v,.95):6.3f} p99 {pct(v,.99):6.3f} max {max(v):7.3f}")
    cn = s['counters']
    if isinstance(cn, dict):
        for k in want_cnt:
            if k in cn:
                v = cn[k]
                print(f"   cnt {k:16s} mean {sum(v)/len(v):9.2f} p95 {pct(v,.95):9.2f} max {max(v):9.2f}")
