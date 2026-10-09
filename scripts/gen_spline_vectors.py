#!/usr/bin/env python3
"""Write tests/spline_vectors.json: the shared spline test table.

THE SPLINE (docs/PLAN_worldbuilder.md 2.1 / 2.3). One tessellation, three
implementations that must agree: this reference, the engine's C++
(src/sim/worldwater.cpp worldmap::SplineTessellate, held to this table by the
`rivers` selftest gate) and the editor's assets/editor/spline.js (its Node
test reads the same table).

  * CENTRIPETAL Catmull-Rom (alpha 0.5), Barry-Goldman evaluation, every knot
    ON the curve.
  * Open curves get phantom ends P[-1] = 2 P0 - P1, P[n] = 2 P[n-1] - P[n-2];
    closed curves wrap their controls.
  * Knot parameters t1 = sqrt(|p1 - p0|), t2 = t1 + sqrt(|p2 - p1|),
    t3 = t2 + sqrt(|p3 - p2|) (t0 = 0), each distance floored at 1e-4.
  * Segment Pi -> Pi+1 is evaluated at nd = clamp(ceil(|Pi+1 - Pi| / 2), 8,
    4096) uniform steps of t over [t1, t2] (both ends exactly the knots) into a
    dense polyline; its arc length L (sum of the chords) is resampled at
    m = max(1, ceil(L / step)) EQUAL arc steps by linear interpolation along
    the dense chords. Output: Pi, then the m - 1 interior points, segment by
    segment; an open curve ends with P[n-1] exactly, a closed one does not
    repeat P0.
  * smooth: false is the same resampling over straight segments (nd = 1).

Outputs are rounded to 3 decimals; a consumer compares within 2e-3.
Run: python scripts/gen_spline_vectors.py
"""
import json
import math
import os

STEP = 16.0


def dist(a, b):
    dx = b[0] - a[0]
    dz = b[1] - a[1]
    return math.sqrt(dx * dx + dz * dz)


def bg_eval(p0, p1, p2, p3, t0, t1, t2, t3, t):
    out = []
    for c in range(2):
        a1 = (t1 - t) / (t1 - t0) * p0[c] + (t - t0) / (t1 - t0) * p1[c]
        a2 = (t2 - t) / (t2 - t1) * p1[c] + (t - t1) / (t2 - t1) * p2[c]
        a3 = (t3 - t) / (t3 - t2) * p2[c] + (t - t2) / (t3 - t2) * p3[c]
        b1 = (t2 - t) / (t2 - t0) * a1 + (t - t0) / (t2 - t0) * a2
        b2 = (t3 - t) / (t3 - t1) * a2 + (t - t1) / (t3 - t1) * a3
        out.append((t2 - t) / (t2 - t1) * b1 + (t - t1) / (t2 - t1) * b2)
    return out


def dense_segment(p0, p1, p2, p3, smooth):
    if not smooth:
        return [list(p1), list(p2)]
    d01 = max(dist(p0, p1), 1e-4)
    d12 = max(dist(p1, p2), 1e-4)
    d23 = max(dist(p2, p3), 1e-4)
    t0 = 0.0
    t1 = math.sqrt(d01)
    t2 = t1 + math.sqrt(d12)
    t3 = t2 + math.sqrt(d23)
    nd = min(max(int(math.ceil(d12 / 2.0)), 8), 4096)
    pts = [list(p1)]
    for k in range(1, nd):
        t = t1 + (t2 - t1) * k / nd
        pts.append(bg_eval(p0, p1, p2, p3, t0, t1, t2, t3, t))
    pts.append(list(p2))
    return pts


def resample(dense, step):
    cum = [0.0]
    for i in range(1, len(dense)):
        cum.append(cum[-1] + dist(dense[i - 1], dense[i]))
    L = cum[-1]
    m = max(1, int(math.ceil(L / step)))
    out = [(list(dense[0]), 0.0)]
    j = 0
    for k in range(1, m):
        s = L * k / m
        while j + 2 < len(dense) and cum[j + 1] < s:
            j += 1
        span = cum[j + 1] - cum[j]
        f = (s - cum[j]) / span if span > 0 else 0.0
        a, b = dense[j], dense[j + 1]
        out.append(([a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f], k / m))
    return out


def tessellate(points, closed, smooth, step=STEP):
    n = len(points)
    P = [list(map(float, p)) for p in points]
    if closed:
        ctrl = lambda i: P[i % n]
        nseg = n
    else:
        def ctrl(i):
            if i < 0:
                return [2 * P[0][0] - P[1][0], 2 * P[0][1] - P[1][1]]
            if i >= n:
                return [2 * P[n - 1][0] - P[n - 2][0], 2 * P[n - 1][1] - P[n - 2][1]]
            return P[i]
        nseg = n - 1
    out = []
    for i in range(nseg):
        dense = dense_segment(ctrl(i - 1), ctrl(i), ctrl(i + 1), ctrl(i + 2), smooth)
        for p, f in resample(dense, step):
            out.append((p, i, f))
    if not closed:
        out.append((list(P[n - 1]), n - 2, 1.0))
    return out


CASES = [
    ("two points: a straight line", False, True, [[0, 0], [100, 0]]),
    ("three-point bend", False, True, [[0, 0], [120, 40], [240, 0]]),
    ("S curve, uneven spacing", False, True, [[-300, 10], [-120, 90], [-100, 100], [60, -80], [400, 20]]),
    ("near-duplicate knots", False, True, [[0, 0], [1, 0], [64, 64], [65, 64], [200, 0]]),
    ("far from the origin", False, True, [[-90112, 51200], [-89600, 51900], [-88900, 51850], [-88200, 52600]]),
    ("closed loop (a lake outline)", True, True, [[0, 0], [400, 0], [480, 300], [100, 380], [-60, 200]]),
    ("polyline, smooth false", False, False, [[0, 0], [100, 0], [100, 37]]),
    ("closed polyline, smooth false", True, False, [[0, 0], [50, 0], [50, 50], [0, 50]]),
]


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    cases = []
    for name, closed, smooth, pts in CASES:
        out = tessellate(pts, closed, smooth)
        cases.append({
            "name": name, "closed": closed, "smooth": smooth, "points": pts,
            "out": [[round(p[0], 3), round(p[1], 3)] for p, _, _ in out],
            "seg": [s for _, s, _ in out],
            "f": [round(f, 6) for _, _, f in out],
        })
    doc = {
        "about": "THE SPLINE's shared test table (docs/PLAN_worldbuilder.md 2.1/2.3): centripetal Catmull-Rom "
                 "(alpha 0.5, Barry-Goldman), phantom ends P[-1] = 2 P0 - P1, P[n] = 2 P[n-1] - P[n-2] (closed: "
                 "wrapped controls), each segment densified at clamp(ceil(chord / 2), 8, 4096) uniform knot-parameter "
                 "steps, then resampled at max(1, ceil(arcLength / step)) equal arc steps (knots exact). out = [x, z] "
                 "rounded to 3 decimals, seg = the segment each point lies on, f = its arc fraction in it. Written by "
                 "scripts/gen_spline_vectors.py; read by the `rivers` selftest gate (C++ SplineTessellate) and by "
                 "assets/editor/spline.js's Node test. Compare within 2e-3.",
        "step": STEP,
        "decimals": 3,
        "cases": cases,
    }
    path = os.path.join(root, "tests", "spline_vectors.json")
    with open(path, "w", newline="\n") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    print("wrote %s (%d cases, %d points)" % (path, len(cases), sum(len(c["out"]) for c in cases)))


if __name__ == "__main__":
    main()
