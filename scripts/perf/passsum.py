"""Per-pass GPU ms/frame from --perf json files. Usage: python scripts/perf/passsum.py <json>..."""
import json, sys
KEYS = ('farMapFill', 'farFill', 'farCoverFill', 'farPatchFill', 'worldgenCols',
        'worldgenList', 'rm_world', 'shadow_resolve', 'hz_sun', 'opennessRefresh')
for f in sys.argv[1:]:
    d = json.load(open(f))
    sc = d['scenarios'][0]
    p = {r['name']: r['usPerFrame'] for r in sc['passes']}
    print(f.split('/')[-1].ljust(36), ' '.join(f"{k} {p.get(k, 0) / 1000:.3f}" for k in KEYS))
