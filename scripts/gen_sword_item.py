"""Generate assets/items/sword.{vox,json} — the arming sword, as a STANDALONE ITEM.

An item is not a part of the creature that holds it: it has its own .vox and
its own origin, and the rig BORROWS A SLOT to show it (src/game/item.h has the
history — the sword used to be a rig prop and hung outside its wearer's box).
It is authored pommel-at-low-x, tip-at-high-x, and the GRIP ROTATION is what
points it outboard when held; the art never encodes handedness.

THE ART (2026-09-27, scripts/bladesmith.py for the shared parts). 80 art voxels
per metre — one cell is 12.5 mm — which is fine enough for a blade to have a
section instead of being a slab:

  * a CHAMFERED WHEEL POMMEL, blued, with a raised boss;
  * a leather grip with a spiral cord wrap, swelling slightly at the middle,
    between two iron risers;
  * a CROSSGUARD whose quillons taper and sweep toward the blade, ending in
    knobs, with a central block and a langet tongue lying on the ricasso;
  * a BLADE with a short unground ricasso, then a lenticular section — three
    cells through the flats, one at the edge, the step between them being the
    bevel — a fuller down the first 60% of the flat, a maker's mark inlaid in
    it, and a diamond section with a midrib from the end of the fuller into an
    acute point.

What the rig reads is unchanged: overall length 1.10 m, the grip's centre
15 cm from the butt, the edge from 30 cm to the tip, halfWidth 7.5 cm. Each of
those is the old 40/m number doubled.

The `edge` and `hilt` blocks are emitted from the same constants that build the
mesh, so the hitbox cannot drift from the art.

Run: python scripts/gen_sword_item.py
"""

import math
import os

import bladesmith as B

LEN = 88            # pommel butt to tip: 1.10 m
GRIP = (6, 20)      # the wrapped span; risers at both ends
HILT = (4, 20)      # the hilt box (unchanged from 40/m: 2..10 micro)
GUARD = (20, 24)    # the crossguard
BLADE0 = 24         # ricasso starts here; the edge block starts here too
HALF_W = 6          # edge.halfWidth: 3 micro at 40/m


def build():
    f = B.Forge(LEN, 23, 5)

    # ---- crossguard (stamped first: it owns the cells where it meets the
    # blade and the grip) ----------------------------------------------------
    def guard(x, dy, dz):
        X = x + 0.5
        ady = abs(dy)
        # central block: wraps the blade root, 5 cells through
        if ady <= 3 and GUARD[0] <= x < GUARD[1]:
            return (B.STEEL, "iron")
        # langet: a tongue of the guard lying on each flat of the ricasso,
        # narrowing onto the blade
        if abs(dz) == 2 and GUARD[1] <= x < GUARD[1] + 3 and ady <= 2 - (x - GUARD[1]) * 0.7:
            return (B.STEEL, "iron")
        if ady > 10:
            return None
        # quillons: a bar centred on a line that sweeps gently toward the
        # blade, tapering in x toward the tips
        def centre(a):
            k = max(0.0, a - 3.0)
            return 21.6 + 0.022 * k * k, 2.0 - 0.8 * B.clamp01(k / 6.0)
        if ady <= 9 and abs(dz) <= 1:
            xc, hx = centre(ady)
            if abs(X - xc) <= hx:
                return (B.STEEL, "iron")
        # terminal knobs: a little ball at each tip
        kx, _ = centre(9.3)
        if (X - kx) ** 2 + (ady - 9.3) ** 2 + (dz * 1.2) ** 2 <= 1.75 ** 2:
            return (B.STEEL, "iron")
        return None
    f.stamp(guard, GUARD[0] - 1, GUARD[1] + 4)

    # ---- pommel: a chamfered wheel with a boss -------------------------------
    def pommel(x, dy, dz):
        X = x + 0.5
        r = math.hypot(X - 3.4, dy)
        R = 3.45
        if r > R:
            return None
        adz = abs(dz)
        if r <= 1.6:
            t = 2                        # the boss, standing proud of the face
        elif r <= R - 1.0:
            t = 1                        # the face
        else:
            t = 0                        # the chamfered rim
        if adz > t:
            return None
        return (B.STEEL, "iron_hi" if adz == 2 else "iron")
    f.stamp(pommel, 0, 8)

    # ---- grip ------------------------------------------------------------------
    def grip(x, dy, dz):
        X = x + 0.5
        g0, g1 = GRIP
        if not (g0 <= x < g1):
            return None
        t = (X - g0) / (g1 - g0)
        s = 1.0 + 0.10 * math.sin(math.pi * t)
        # iron risers at both ends of the wrap
        if x == g0 or x == g1 - 1:
            if (dy / 2.35) ** 2 + (dz / 2.35) ** 2 <= 1.0:
                return (B.STEEL, "iron")
            return None
        ry, rz = 2.15 * s, 2.05 * s
        if (dy / ry) ** 2 + (dz / rz) ** 2 > 1.0:
            return None
        return (B.GRIP_LEATHER, B.wrap_colour(x, dy, dz))
    f.stamp(grip, GRIP[0], GRIP[1])

    # ---- blade -----------------------------------------------------------------
    blen = LEN - BLADE0
    def blade(x, dy, dz):
        X = x + 0.5
        u = (X - BLADE0) / blen
        # the ricasso: square-shouldered, full thickness, unground
        if x < BLADE0 + 4:
            if abs(dy) <= 4 and abs(dz) <= 1:
                return (B.STEEL, "forged")
            return None
        # profile: a gentle straight taper, then an ogival point
        base = 5.8 - 1.8 * u
        if u > 0.68:
            p = (u - 0.68) / 0.32
            hw = base * (1.0 - p ** 1.7) ** 0.85
        else:
            hw = base
        # shoulders out of the ricasso
        hw = min(hw, 5.0 + (x - (BLADE0 + 4)) * 0.6)
        # fuller: from just past the ricasso, running out at 60%, fading
        fl = 0.0
        if u < 0.60:
            fl = 1.0 - B.smooth(0.52, 0.60, u)
            fl = 0.9 if fl > 0.5 else 0.0
        # where the blade is thick: a wide lenticular flat near the hilt, a
        # narrowing diamond toward the point (the distal taper)
        thick_to = max(1.0, hw - 1.9) if u < 0.62 else max(1.0, hw * 0.48)
        c = B.blade_section(dy, dz, hw, thick_to=thick_to, fuller=fl,
                            ridge=(u >= 0.62))
        if c is None:
            return None
        # the maker's mark: a small brass cross inlaid at the head of the fuller
        if x in (BLADE0 + 7, BLADE0 + 8, BLADE0 + 9) and dy == 0:
            c = "brass_hi"
        if x == BLADE0 + 8 and abs(dy) == 1 and abs(dz) == 1:
            c = "brass_hi"
        return (B.STEEL, c)
    f.stamp(blade, BLADE0, LEN)

    # bright arrises on the fittings: the corners of the guard and pommel wear
    f.recolour_arrises(0, 8, "iron", "iron_hi")
    f.recolour_arrises(GUARD[0], GUARD[1] + 3, "iron", "iron_hi")
    return f


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "assets", "items")
    f = build()
    sc = B.sidecar(
        "sword", f,
        comment=(
            "The sword as a standalone item: its own art, its own origin. Held "
            "by BORROWING a rig slot (see src/game/item.cpp), so it is a real "
            "rig Part while worn and severs, drops, burns and carves exactly "
            "like a limb. Regenerate with scripts/gen_sword_item.py — the edge "
            "block is emitted from the same constants that build the mesh, so "
            "the hitbox cannot drift from the art."),
        hp=30, sever=7.0, grip_t=(0, 0, 0), hilt_x=HILT, edge_x=(BLADE0, LEN),
        half_w=HALF_W, old_line=(0.75, -0.5),
        spring={"halflife": 0.09, "gain": 0.35, "maxAngle": 0.15})
    B.emit(out_dir, "sword", f, sc)


if __name__ == "__main__":
    main()
