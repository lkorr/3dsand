import json,sys
rows=[]
for path in sys.argv[1:]:
    d=json.load(open(path))
    tag=path.split('/')[-1]
    print("== %s ==" % tag)
    print("%-12s %8s %8s %8s %8s %10s %9s %10s %s" % ("scene","p50","fluid","seam+st","march","clamps","settle@","capture%","ledger/live"))
    for r in d:
        s=r['scene']; f=r['frameMs']; p=r['passesMs']
        fl=p.get('fluid(substep)',{}).get('avg',0)
        sm=p.get('fluidSeam',{}).get('avg',0)+p.get('fluidSettle',{}).get('avg',0)
        march=r['renderSplitMs'].get('fluidMarch',0)
        cl=r['clampEngagements']['total'] if isinstance(r['clampEngagements'],dict) else r['clampEngagements']
        cap=r['basinCapture']
        ml=r['massLedger']
        print("%-12s %8.2f %8.2f %8.2f %8.2f %10d %9d %10s %s" % (
            s, f['p50'], fl, sm, march, cl, r['tickOfSettle'],
            ("%.1f"%(cap*100)) if cap>=0 else "-",
            "%s in=%s stand=%s carry=%s live=%s" % (ml.get('exact'),ml.get('poured'),ml.get('standing'),ml.get('carried'),r['liveEnd'])))
    print()
