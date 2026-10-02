#!/usr/bin/env python3
"""Compare two sandvox determinism fingerprints and name the first divergence.

A fingerprint is what `sandvox.exe --fingerprint <file>` writes (DESIGN.md §14
risk 3, "cross-vendor determinism"): the selftest determinism gate's 200-tick
world-hash sequence, the gas digest, and per-slot voxel digests at a few
checkpoint ticks, with the device / driver / build commit / asset stamps of the
machine that produced it.

    python scripts/det_fingerprint_compare.py A.json B.json

Exit 0 = the two sequences are identical; 1 = they diverge (the report says
where); 2 = the files cannot be compared (unreadable, different format, or a
different scenario length).

What it prints, in order of how much each line narrows the search:
  * both devices, so nobody mistakes two runs of one GPU for a cross-vendor pass;
  * build commit / asset stamp / residency mismatches, which explain a
    divergence without any GPU being at fault (fix those first);
  * the FIRST tick whose world hash differs (tick 1 = worldgen or the first
    tick's ops; a later tick = the sim);
  * at the first checkpoint at or after it, how many slots' voxel digests
    differ and the first few by world chunk coordinate. Those chunks are where
    to point SANDVOX_DET_PROBE=<tick> SANDVOX_DET_SLOTS=<slot> on a rerun for
    the field-level diff (material / state / stain / stamp).
"""
import json
import sys


def load(path):
    with open(path, "r", encoding="utf-8") as f:
        d = json.load(f)
    if not str(d.get("format", "")).startswith("sandvox-determinism-fingerprint/"):
        raise ValueError("%s: not a sandvox determinism fingerprint" % path)
    return d


def dev_line(d):
    v = d.get("device") or {}
    return "%s | %s %s | vk %s | type %s | build %s" % (
        v.get("name", "?"), v.get("driverName", "?"), v.get("driverInfo", ""),
        v.get("apiVersion", "?"), v.get("type", "?"), d.get("buildCommit", "?"))


def slot_to_chunk(slot, origin, n):
    if slot >= n * n * n:
        return None  # a chunk-ticket slot: no fixed window position
    s = (slot % n, (slot // n) % n, slot // (n * n))
    m = n - 1
    return tuple(origin[i] + ((s[i] - origin[i]) & m) for i in range(3))


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    try:
        a, b = load(argv[1]), load(argv[2])
    except (OSError, ValueError) as e:
        print("cannot compare: %s" % e)
        return 2
    print("A: %s\n   %s" % (argv[1], dev_line(a)))
    print("B: %s\n   %s" % (argv[2], dev_line(b)))
    da, db = a.get("device") or {}, b.get("device") or {}
    if (da.get("name"), da.get("driverName"), da.get("driverInfo")) == \
       (db.get("name"), db.get("driverName"), db.get("driverInfo")):
        print("NOTE: same device AND driver on both sides - this is a repeat, "
              "not a cross-vendor comparison.")

    # Things that explain a divergence with no GPU at fault. Reported, never
    # fatal: a stamp mismatch with identical hashes is still a useful result.
    caveats = []
    if a.get("buildCommit") != b.get("buildCommit"):
        caveats.append("build commit differs: %s vs %s" % (a.get("buildCommit"), b.get("buildCommit")))
    if a.get("tuningStamp") != b.get("tuningStamp"):
        caveats.append("asset stamps differ (tuning/materials/reactions): %s vs %s"
                       % (json.dumps(a.get("tuningStamp")), json.dumps(b.get("tuningStamp"))))
    for k in ("residency", "seed", "windowOrigin", "nChunk", "scenario"):
        if a.get(k) != b.get(k):
            caveats.append("%s differs: %s vs %s" % (k, a.get(k), b.get(k)))
    for d, tag in ((a, "A"), (b, "B")):
        dv = d.get("device") or {}
        if dv.get("outOfSpecBindings"):
            caveats.append("%s ran OUT OF SPEC (SANDVOX_ALLOW_OVERSIZE_BINDINGS): a storage binding "
                           "of %s bytes exceeds its maxStorageBufferRange %s. A MATCH is still "
                           "evidence; a divergence proves nothing until the binding is in spec"
                           % (tag, dv.get("outOfSpecMaxRange"), dv.get("maxStorageBufferRange")))
        sc = d.get("selfCheck") or {}
        if sc.get("reproduced") is False:
            caveats.append("%s did NOT reproduce itself (twice-run divergence at tick %s): "
                           "that device is nondeterministic on its own - a rule-1 bug "
                           "before it is a cross-vendor one" % (tag, sc.get("firstDivergentTick")))
        elif sc.get("reproduced") is None:
            caveats.append("%s ran once (SANDVOX_DET_RUNS=1): its own twice-run check was skipped" % tag)
    for c in caveats:
        print("CAVEAT: " + c)

    ha, hb = a.get("worldHashes", []), b.get("worldHashes", [])
    if len(ha) != len(hb) or not ha:
        print("cannot compare: hash sequences have different lengths (%d vs %d)" % (len(ha), len(hb)))
        return 2
    first = next((i + 1 for i in range(len(ha)) if ha[i] != hb[i]), 0)
    gas_a, gas_b = a.get("gas", {}), b.get("gas", {})
    gas_same = gas_a == gas_b
    if not first and gas_same:
        print("MATCH: all %d world hashes identical (final %s), gas digest %s identical."
              % (len(ha), ha[-1], gas_a.get("digest")))
        return 0
    if not first:
        print("DIVERGED in the gas population only: world hashes identical over %d ticks, "
              "gas %s vs %s (gas parcels outside the window are not in the world hash; "
              "see sim_gas.wgsl)" % (len(ha), gas_a, gas_b))
        return 1
    n_diff = sum(1 for i in range(len(ha)) if ha[i] != hb[i])
    print("DIVERGED: first divergent tick %d (%s vs %s); %d of %d ticks differ; final %s vs %s"
          % (first, ha[first - 1], hb[first - 1], n_diff, len(ha), ha[-1], hb[-1]))
    if first > 1:
        print("  ticks 1..%d identical, so worldgen and the first %d ticks agree."
              % (first - 1, first - 1))
    else:
        print("  tick 1 already differs: worldgen or the first tick's op application.")
    if not gas_same:
        print("  gas digest differs too: %s vs %s" % (gas_a, gas_b))

    ca, cb = a.get("chunkHashes", {}), b.get("chunkHashes", {})
    cps = sorted(int(t) for t in ca.keys() if t in cb)
    origin = a.get("windowOrigin", [0, 0, 0])
    n = int(a.get("nChunk", 32))
    shown_any = False
    for t in cps:
        if t < first:
            continue
        ma = {s: h for s, h in ca[str(t)]}
        mb = {s: h for s, h in cb[str(t)]}
        diff = sorted(s for s in set(ma) | set(mb) if ma.get(s) != mb.get(s))
        print("  checkpoint tick %d: %d slot digest(s) differ%s" %
              (t, len(diff), " (the first checkpoint at/after the divergence)" if not shown_any else ""))
        for s in diff[:12 if not shown_any else 4]:
            wc = slot_to_chunk(s, origin, n)
            where = "chunk (%d,%d,%d)" % wc if wc else "ticket slot"
            print("    slot %5d  %-22s %s vs %s" % (s, where, ma.get(s, "--------"), mb.get(s, "--------")))
        if not shown_any and diff:
            print("    -> rerun the gate on either machine with SANDVOX_DET_PROBE=%d "
                  "SANDVOX_DET_SLOTS=%d for the field-level diff of that slot" % (t, diff[0]))
        shown_any = True
    if not shown_any:
        print("  no checkpoint at or after tick %d; rerun with SANDVOX_FINGERPRINT_TICKS=%d"
              % (first, first))
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
