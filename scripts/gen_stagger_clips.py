#!/usr/bin/env python3
"""Solve the stagger clips from HAND TARGETS on the stock human rig.

Writes assets/anims/stagger_head.json and stagger_chest.json, the keyed half
of the hit stagger (mob.h Mob::NoteStaggerHp, MobSystem::StartStagger). The
torso and head of every key are GIVEN (the recoil, the bow, the writhe); the
arms are SOLVED so the palms land on the head / the belly in that key's
posed frame, inside the engine's pose limits. Edit the keys below and re-run;
never hand-edit the quaternions in the JSON.

The rig, the forward kinematics, the limit penalty and the quaternion helpers
are gen_archery_clips.py's (one copy of the rig).

    python scripts/gen_stagger_clips.py          # solve, write, print residuals
    python scripts/gen_stagger_clips.py --check  # print only
"""
import math
import os
import sys

import numpy as np
from scipy.optimize import minimize

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_archery_clips as rig  # noqa: E402

from gen_archery_clips import (ANCHOR, fk, point, qaxis, qmul, qrot,  # noqa: E402
                               qrotvec, qangle, hinge_of, limit_penalty,
                               swing_twist, write, fmt_q, ANIMS, ROOT)

ARMS = ["armU.L", "armL.L", "hand.L", "armU.R", "armL.R", "hand.R"]
MASK = ["torso", "head"] + ARMS
# The PALM of each hand (hand.R's held socket, and its mirror): what touches.
PALM = {"hand.L": (30, 61, 10), "hand.R": (6, 61, 10)}
# Its NORMAL in the hand's rest frame: a hanging hand's palm faces the thigh,
# so the left palm looks along -X and the right along +X.
PALM_N = {"hand.L": (-1, 0, 0), "hand.R": (1, 0, 0)}
I = np.array([0.0, 0.0, 0.0, 1.0])


def spine(lean=0.0, yaw=0.0, roll=0.0):
    """A torso/head local: yaw about Y, then pitch about X (+ = forward / down
    for the upright torso and the head), then roll about Z, in degrees."""
    return qmul(qaxis((0, 1, 0), math.radians(yaw)),
                qmul(qaxis((1, 0, 0), math.radians(lean)),
                     qaxis((0, 0, 1), math.radians(roll))))


def unpack(x, fixed):
    local = dict(fixed)
    i = 0
    for p in ("armU.L", "hand.L", "armU.R", "hand.R"):
        local[p] = qrotvec(x[i:i + 3])
        i += 3
    for p in ("armL.L", "armL.R"):
        local[p] = qaxis((-1, 0, 0), hinge_of(x[i]))
        i += 1
    return local


def arm_cost(targets, toward):
    """`targets`: hand -> (part whose frame the point is in, point in that
    part's REST coordinates). `toward`: hand -> (part, rest point) the palm
    should face. Wrists stay near straight; the arms prefer to hang."""
    def cost(local):
        rot, pos = fk(local)
        c = 0.0
        for h, (part, p) in targets.items():
            want = point(rot, pos, part, p)
            got = point(rot, pos, h, PALM[h])
            c += 4.0 * float(np.sum((got - want) ** 2))
        for h, (part, p) in toward.items():
            at = point(rot, pos, part, p)
            got = point(rot, pos, h, PALM[h])
            n = qrot(rot[h], PALM_N[h])
            d = at - got
            d = d / max(1e-6, float(np.linalg.norm(d)))
            c += 40.0 * (1.0 - float(np.dot(n, d)))
        for h in ("hand.L", "hand.R"):
            sw, tw = swing_twist(local[h], (0, -1, 0))
            c += 3.0 * sw ** 2 + 400.0 * max(0.0, sw - math.radians(60)) ** 2 + 2.0 * tw ** 2
        return c
    return cost


def solve_arms(fixed, cost, warm=None, prev=None, w_prev=20.0):
    """Arms only (torso/head given). `warm` = the previous key's x, so the
    clip stays on one branch of the arm's solutions."""
    n = 3 * 4 + 2
    def f(x):
        local = unpack(x, fixed)
        c = cost(local) + 2000.0 * limit_penalty(local)
        if prev is not None:
            c += w_prev * sum(qangle(qmul(rig.qconj(prev[p]), local[p])) ** 2 for p in ARMS)
        return c
    if warm is not None:
        starts = [np.array(warm, float)]
    else:
        x0 = np.zeros(n)
        # Elbows out to the sides and up, forearms folded: every target here
        # is the head or the belly.
        x0[0:3] = (-1.2, 0, 0.9)
        x0[6:9] = (-1.2, 0, -0.9)
        x0[12] = 2.0
        x0[13] = 2.0
        starts = [x0 + (np.random.RandomState(s).randn(n) * 0.4 if s else 0) for s in range(12)]
    best = None
    for s in starts:
        r = minimize(f, s, method="BFGS", options={"maxiter": 8000, "gtol": 1e-9})
        if best is None or r.fun < best.fun:
            best = r
    return unpack(best.x, fixed), best.x


def residual(local, targets):
    rot, pos = fk(local)
    return {h: round(float(np.linalg.norm(point(rot, pos, h, PALM[h]) - point(rot, pos, part, p))), 1)
            for h, (part, p) in targets.items()}


def fk_pose(fixed, arms):
    """A key with the arms POSED, not solved: armU as (forward deg, out deg),
    the elbow bend in degrees."""
    local = dict(fixed)
    for side, sgn in (("L", 1.0), ("R", -1.0)):
        fwd, out, bend = arms[side]
        local["armU." + side] = qmul(qaxis((0, 0, 1), math.radians(out) * sgn),
                                     qaxis((1, 0, 0), -math.radians(fwd)))
        local["armL." + side] = qaxis((-1, 0, 0), math.radians(bend))
        local["hand." + side] = I.copy()
    return local


def clip(name, comment, keys, blend_in, blend_out, ease=None):
    """keys: [(t ms, local)]. A one-shot override over MASK."""
    tracks = {}
    for p in MASK:
        rows = []
        for i, (t, local) in enumerate(keys):
            row = {"t": int(round(t)), "q": fmt_q(local.get(p, I))}
            rows.append(row)
        tracks[p] = {"rot": rows}
    return {"//": comment, "name": name, "sidecarVoxelsPerMetre": 10,
            "durationMs": int(round(keys[-1][0])), "loop": False, "mode": "override",
            "blendInMs": blend_in, "blendOutMs": blend_out, "mask": list(MASK),
            # The walk's arm swing and the gait's sway would land on top of
            # hands that are holding something: silence them on the arms, and
            # mostly on the trunk.
            "quietAdditive": dict({p: 1.0 for p in ARMS}, torso=0.7, head=0.7),
            "tracks": tracks}


# ---- the head: rest-frame points on the skull ----------------------------
# The head part's anchor is the neck (18, 104, 10); the eye is 16 above it.
# A palm on each side of the skull a little behind the ears, and the palm
# facing the skull's middle.
HEAD_MID = (18, 117, 11)
HEAD_SIDE_L = (28, 117, 9)
HEAD_SIDE_R = (8, 117, 9)
BROW = (15, 121, 21)        # the right palm over the brow, the forehead's front
HEAD_BACK_L = (25, 121, 1)  # the left palm round the back of the skull
JAW_L = (25, 111, 17)
JAW_R = (11, 111, 17)

# ---- the trunk: rest-frame points on the torso (anchor = waist, 18, 72, 10)
BELLY = (21, 82, 19)
RIBS_R = (10, 90, 18)
TRUNK_MID = (18, 86, 10)


def head_keys():
    keys = []
    # t=0: neutral under the blend-in (an override's first key is where the
    # blend leaves from).
    k0 = fk_pose({"torso": I, "head": I}, {"L": (6, 8, 12), "R": (6, 8, 12)})
    keys.append((0, k0))
    # THE RECOIL: snapped back at the neck, the chest thrown back, the arms
    # flung up and out by the jolt.
    k1 = fk_pose({"torso": spine(lean=-9, roll=3), "head": spine(lean=-24, roll=-6)},
                 {"L": (35, 28, 45), "R": (30, 32, 40)})
    keys.append((130, k1))
    # THE CLUTCH and THE WRITHE: bent over the hurt, both hands on the skull
    # (the right over the brow, the left round the side and back), the trunk
    # twisting one way and the other under it.
    held = [
        (340, spine(lean=16, yaw=4), spine(lean=20, roll=4),
         {"hand.L": ("head", HEAD_SIDE_L), "hand.R": ("head", BROW)}),
        (540, spine(lean=21, yaw=16, roll=7), spine(lean=24, yaw=6, roll=-9),
         {"hand.L": ("head", HEAD_BACK_L), "hand.R": ("head", BROW)}),
        (760, spine(lean=14, yaw=-13, roll=-6), spine(lean=17, yaw=-5, roll=8),
         {"hand.L": ("head", HEAD_SIDE_L), "hand.R": ("head", BROW)}),
        (940, spine(lean=13, yaw=7, roll=3), spine(lean=15, roll=-4),
         {"hand.L": ("head", HEAD_BACK_L), "hand.R": ("head", HEAD_SIDE_R)}),
    ]
    warm, prev = None, None
    for t, torso, head, targets in held:
        fixed = {"torso": torso, "head": head}
        toward = {h: ("head", HEAD_MID) for h in targets}
        local, warm = solve_arms(fixed, arm_cost(targets, toward), warm=warm, prev=prev)
        print(f"   stagger_head t={t}: palm miss (cells) {residual(local, targets)}"
              f"  limit {limit_penalty(local):.4f}")
        keys.append((t, local))
        prev = local
    # Coming out of it: the hands sliding down to the jaw, the body
    # straightening; the blend-out carries it the rest of the way.
    targets = {"hand.L": ("head", JAW_L), "hand.R": ("head", JAW_R)}
    fixed = {"torso": spine(lean=7), "head": spine(lean=9)}
    local, warm = solve_arms(fixed, arm_cost(targets, {h: ("head", HEAD_MID) for h in targets}),
                             warm=warm, prev=prev)
    print(f"   stagger_head t=1100: palm miss (cells) {residual(local, targets)}"
          f"  limit {limit_penalty(local):.4f}")
    keys.append((1100, local))
    return keys


def chest_keys():
    keys = []
    k0 = fk_pose({"torso": I, "head": I}, {"L": (6, 8, 12), "R": (6, 8, 12)})
    keys.append((0, k0))
    # THE JOLT: the chest knocked back an instant, the head thrown forward.
    k1 = fk_pose({"torso": spine(lean=-6), "head": spine(lean=8)},
                 {"L": (18, 14, 30), "R": (16, 14, 28)})
    keys.append((90, k1))
    # THE HUNCH: folded over the wound, the left palm pressed to the belly,
    # the right forearm across the ribs.
    held = [
        (240, spine(lean=24, yaw=-5), spine(lean=10),
         {"hand.L": ("torso", BELLY), "hand.R": ("torso", RIBS_R)}),
        (460, spine(lean=19, yaw=-3, roll=-3), spine(lean=6),
         {"hand.L": ("torso", BELLY), "hand.R": ("torso", RIBS_R)}),
    ]
    warm, prev = None, None
    for t, torso, head, targets in held:
        fixed = {"torso": torso, "head": head}
        toward = {h: ("torso", TRUNK_MID) for h in targets}
        local, warm = solve_arms(fixed, arm_cost(targets, toward), warm=warm, prev=prev)
        print(f"   stagger_chest t={t}: palm miss (cells) {residual(local, targets)}"
              f"  limit {limit_penalty(local):.4f}")
        keys.append((t, local))
        prev = local
    # Straightening, the left hand still on the belly.
    targets = {"hand.L": ("torso", BELLY)}
    fixed = {"torso": spine(lean=9), "head": spine(lean=2)}
    cost_l = arm_cost(targets, {"hand.L": ("torso", TRUNK_MID)})
    def cost(local):
        # The right arm drops back toward hanging.
        return cost_l(local) + 20.0 * (qangle(local["armU.R"]) ** 2 +
                                       (qangle(local["armL.R"]) - math.radians(20)) ** 2)
    local, warm = solve_arms(fixed, cost, warm=warm, prev=prev, w_prev=5.0)
    print(f"   stagger_chest t=650: palm miss (cells) {residual(local, targets)}"
          f"  limit {limit_penalty(local):.4f}")
    keys.append((650, local))
    return keys


def main():
    check = "--check" in sys.argv
    print("== stagger_head")
    hk = head_keys()
    print("== stagger_chest")
    ck = chest_keys()
    if check:
        return
    base = ("Solved by scripts/gen_stagger_clips.py from palm targets -- edit the keys "
            "there and re-run, never these quaternions. ")
    write(os.path.join(ANIMS, "stagger_head.json"), clip(
        "stagger_head",
        "STAGGERED BY A BLOW TO THE HEAD (mob.h Mob::NoteStaggerHp, MobSystem::StartStagger): "
        "a one-shot override played when a blow takes combatfx.staggerHeadFrac of the head's "
        "hp. The head snaps back and the arms fly up with the jolt; then the body bends over "
        "it with both hands on the skull -- the right over the brow, the left round the side "
        "and back -- and writhes, the trunk twisting one way and then the other; then the "
        "hands slide down to the jaw as it straightens and the blend-out hands it back. The "
        "legs are not in the mask: the backstep is the walk's own back-pedal "
        "(MobSystem::StaggerDrive), so the feet plant. " + base,
        hk, blend_in=60, blend_out=320))
    write(os.path.join(ANIMS, "stagger_chest.json"), clip(
        "stagger_chest",
        "STAGGERED BY A BLOW TO THE TRUNK (mob.h Mob::NoteStaggerHp): a one-shot override "
        "played when a stab, an arrow or a blow takes combatfx.staggerChestFrac of the torso "
        "or hips' hp. A jolt back, then a hunch over the wound with the left palm pressed to "
        "the belly and the right forearm across the ribs, then straightening with the hand "
        "still on the belly. Short on purpose: the body keeps walking under it, slowed "
        "(combatfx.staggerChestMoveScale), and catches up. " + base,
        ck, blend_in=50, blend_out=300))


if __name__ == "__main__":
    main()
