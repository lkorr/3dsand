"""Generate assets/items/shortsword.{vox,json} and dagger.{vox,json} — the
SMALL end of the melee range.

WHY SMALL WEAPONS EXIST. The wound model (src/game/mob.h BladeCut, sim/tuning.h
§E) scales a cut by the weapon's HEFT, and HeftFactor floors at 0.2 with the
comment "a dagger SHOULD cut shallower than an arming sword". These two put a
real weapon on the light side of the reference: ~0.6x and ~0.2x. Heft is PINNED
in items.json (see scripts/bladesmith.py for why it stopped being derived from
the voxel volume when the art gained a real section).

TWO DIFFERENT WEAPONS, NOT ONE BLADE AT TWO SIZES (2026-09-27). At 40 voxels
per metre they were the arming sword's slab on a shorter bar, because that is
all the lattice could say. At 80 they can be what they are:

  SHORTSWORD — a Mainz-pattern GLADIUS. A hardwood ball pommel with a brass
  peen, a bone grip carved with four finger ridges, an oval hardwood guard
  faced with a brass plate on the blade side, and a waisted leaf blade with a
  diamond section and a long tapering point.

  DAGGER — a quillon dagger. A flattened brass pommel, a leather-wrapped grip,
  short brass quillons curving toward the blade with ball ends, and a stiff
  triangular blade with a strong midrib running to a needle point.

What the rig reads is unchanged: overall length, where the fist closes (the
hilt box's centre), the edge span and its halfWidth are all the old 40/m
numbers doubled, and `grip.translation` doubles with them — shortsword [0,3,0]
became [0,6,0], dagger [1,1,0] became [2,2,0] — so both sit in the hand exactly
where the tuner's held-item preview left them.

Run: python scripts/gen_shortblade_items.py
"""

import math
import os

import bladesmith as B


# ---- SHORTSWORD: a Mainz gladius, 0.70 m ------------------------------------
SS_LEN = 56
SS_HILT = (4, 14)        # 2..7 micro at 40/m
SS_GUARD = (14, 18)
SS_BLADE0 = 18           # 9 micro at 40/m


def build_shortsword():
    f = B.Forge(SS_LEN, 15, 7)

    # the guard: an oval hardwood block, chamfered on both faces, with a brass
    # plate on the blade side
    def guard(x, dy, dz):
        if not (SS_GUARD[0] <= x < SS_GUARD[1]):
            return None
        chamfer = 0.9 if x in (SS_GUARD[0], SS_GUARD[1] - 1) else 0.0
        ry, rz = 6.9 - chamfer, 3.4 - chamfer * 0.6
        if (dy / ry) ** 2 + (dz / rz) ** 2 > 1.0:
            return None
        if x == SS_GUARD[1] - 1:
            return (B.STEEL, "brass")
        return (B.STAFF_WOOD, "wood")
    f.stamp(guard, SS_GUARD[0], SS_GUARD[1])

    # the pommel: a hardwood ball, slightly flattened, with a brass peen
    def pommel(x, dy, dz):
        X = x + 0.5
        if x == 0 and abs(dy) <= 1 and abs(dz) <= 1:
            return (B.STEEL, "brass_hi" if dy == 0 and dz == 0 else "brass")
        e = ((X - 3.4) / 3.0) ** 2 + (dy / 3.7) ** 2 + (dz / 3.25) ** 2
        if e > 1.0:
            return None
        return (B.STAFF_WOOD, "wood")
    f.stamp(pommel, 0, 7)

    # the grip: bone, four finger ridges between the pommel and the guard
    g0, g1 = 5, SS_GUARD[0]
    def grip(x, dy, dz):
        X = x + 0.5
        t = (X - g0) / (g1 - g0)
        ridge = math.cos(math.pi * 4.0 * t) ** 2          # 4 crests
        r = 1.85 + 0.55 * ridge
        if dy * dy + dz * dz > r * r:
            return None
        return (B.BONE, "bone" if ridge > 0.45 else "bone_dk")
    f.stamp(grip, g0, g1)

    # the blade: a waisted leaf, then a long point
    blen = SS_LEN - SS_BLADE0
    def blade(x, dy, dz):
        X = x + 0.5
        u = (X - SS_BLADE0) / blen
        if u <= 0.62:
            hw = 5.7 - 0.75 * math.sin(math.pi * u / 0.62)
        else:
            p = (u - 0.62) / 0.38
            hw = 5.7 * (1.0 - p) ** 1.05
        # the shoulders meet the guard plate square
        if x < SS_BLADE0 + 2:
            hw = 5.2
        c = B.blade_section(dy, dz, hw, thick_to=max(1.0, hw * 0.52),
                            ridge=True)
        return None if c is None else (B.STEEL, c)
    f.stamp(blade, SS_BLADE0, SS_LEN)

    f.recolour_arrises(SS_GUARD[0], SS_GUARD[1], "wood", "wood_hi")
    f.recolour_arrises(SS_GUARD[0], SS_GUARD[1], "brass", "brass_hi")
    return f


# ---- DAGGER: a quillon dagger, 0.35 m ----------------------------------------
DG_LEN = 28
DG_HILT = (2, 10)        # 1..5 micro at 40/m
DG_GUARD = (10, 12)
DG_BLADE0 = 12           # 6 micro at 40/m


def build_dagger():
    f = B.Forge(DG_LEN, 13, 5)

    # quillons: short, brass, curving toward the blade, ball ends
    def guard(x, dy, dz):
        X = x + 0.5
        ady = abs(dy)
        if ady <= 1.5 and DG_GUARD[0] <= x < DG_GUARD[1] and abs(dz) <= 2:
            return (B.STEEL, "brass")
        def centre(a):
            return 11.0 + 0.05 * max(0.0, a - 1.5) ** 2
        if ady <= 4.5 and abs(dz) <= 1 and abs(X - centre(ady)) <= 1.05:
            return (B.STEEL, "brass")
        if (X - centre(5.0)) ** 2 + (ady - 5.0) ** 2 + (dz * 1.2) ** 2 <= 1.45 ** 2:
            return (B.STEEL, "brass")
        return None
    f.stamp(guard, DG_GUARD[0] - 1, DG_GUARD[1] + 3)

    # pommel: a flattened brass disc-ball
    def pommel(x, dy, dz):
        X = x + 0.5
        e = ((X - 1.6) / 1.65) ** 2 + (dy / 2.7) ** 2 + (dz / 2.25) ** 2
        return (B.STEEL, "brass") if e <= 1.0 else None
    f.stamp(pommel, 0, 4)

    # grip: leather over a round core
    def grip(x, dy, dz):
        if dy * dy + dz * dz > 2.05 ** 2:
            return None
        return (B.GRIP_LEATHER, B.wrap_colour(x, dy, dz))
    f.stamp(grip, 3, DG_GUARD[0])

    # blade: a stiff triangle with a midrib
    blen = DG_LEN - DG_BLADE0
    def blade(x, dy, dz):
        X = x + 0.5
        u = (X - DG_BLADE0) / blen
        hw = 4.1 * (1.0 - u) ** 0.9
        if x == DG_BLADE0:
            hw = 3.4
        c = B.blade_section(dy, dz, hw, thick_to=max(1.0, hw * 0.55),
                            ridge=True)
        return None if c is None else (B.STEEL, c)
    f.stamp(blade, DG_BLADE0, DG_LEN)

    f.recolour_arrises(DG_GUARD[0] - 1, DG_BLADE0 + 3, "brass", "brass_hi")
    return f


def build():
    """Both, for the preview tool."""
    return [build_shortsword(), build_dagger()]


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "assets", "items")
    comment = (
        "A {n}: the small end of the melee range, where the wound model's heft "
        "factor falls BELOW the arming sword instead of above it. Regenerate "
        "with scripts/gen_shortblade_items.py — the edge and hilt blocks are "
        "emitted from the same constants that build the mesh, so the hitbox "
        "cannot drift from the art. Heft is PINNED in items.json (the art has "
        "a real blade section now, so its voxel volume no longer says what "
        "the steel weighs).")

    ss = build_shortsword()
    B.emit(out_dir, "shortsword", ss, B.sidecar(
        "shortsword", ss, comment=comment.format(n="shortsword"),
        hp=20, sever=6.0, grip_t=(0, 6, 0), hilt_x=SS_HILT,
        edge_x=(SS_BLADE0, SS_LEN), half_w=6, old_line=(0.75, -0.5),
        spring={"halflife": 0.07, "gain": 0.40, "maxAngle": 0.17}))

    dg = build_dagger()
    B.emit(out_dir, "dagger", dg, B.sidecar(
        "dagger", dg, comment=comment.format(n="dagger"),
        hp=12, sever=4.5, grip_t=(2, 2, 0), hilt_x=DG_HILT,
        edge_x=(DG_BLADE0, DG_LEN), half_w=4, old_line=(0.5, -0.5),
        spring={"halflife": 0.05, "gain": 0.45, "maxAngle": 0.20}))


if __name__ == "__main__":
    main()
