#!/usr/bin/env python3
"""Solve the shield raise clips from TARGETS on the stock human rig.

Writes assets/anims/shield_raise.json and buckler_raise.json. Run it again
after changing a target below; never hand-tune the quaternions.

Both clips are authored for a shield in the RIGHT fist (the library rule: a
right-authored clip, the left hand plays its derived `.mirror`, Mob::HandClip).
They mask only the shield arm, so the other arm stays the weapon's.

THE GRIP (scripts/gen_shield_items.py, `held_right` = Rz(180) Rx(90)): the
shield's face normal is the hand's -X (the back of the right hand) and its UP
is the hand's +Z (the fist's grip axis). Its centre is the hand's socket (the
hilt box is the fist), pushed SHIELD_FACE cells along the normal to the
boards. So "the shield is up" is three targets on the hand: the socket at a
point in front of the chest, -X forward, +Z up.

HOW THE ENGINE PLAYS THEM (Mob::StepShield): the clip's WEIGHT is the raise
fraction, every tick, and `restFill` makes a part keyed at weight w a blend of
its rest pose and the key -- so the arm is exactly as far up as the mechanics
say, at whatever speed a given shield comes up (items.json raiseMs). Hence no
blend-in here: the weight IS the blend. `pitchFollow` on the upper arm lifts
and tilts the shield with the look (the player's camera, an NPC's line to the
blow it is reading), so looking up covers the head.

    python scripts/gen_shield_clips.py          # solve, write, print the FK
    python scripts/gen_shield_clips.py --check  # print only
"""
import math
import os
import sys

import numpy as np
from scipy.optimize import minimize

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_archery_clips import (  # noqa: E402  (the rig and its solver kit)
    ANCHOR, ANIMS, ROOT, fk, point, limit_penalty, qrot, qaxis, qrotvec,
    hinge_of, swing_twist, qangle, clip_json, write)

SOCKET_R = (6, 61, 10)      # human.json sockets.held_right (hand.R)
SHIELD_FACE = 4.0           # hilt centre -> the boards, cells along the normal
FWD = np.array([0.0, 0.0, 1.0])
UP = np.array([0.0, 1.0, 0.0])
ARM = ["armU.R", "armL.R", "hand.R"]


def unpack(x):
    return {"armU.R": qrotvec(x[0:3]),
            "armL.R": qaxis((-1, 0, 0), hinge_of(x[3])),
            "hand.R": qrotvec(x[4:7]),
            # The other arm hangs at rest (limit_penalty reads both).
            "armU.L": np.array([0.0, 0.0, 0.0, 1.0])}


def shield_frame(local):
    rot, pos = fk(local)
    s = point(rot, pos, "hand.R", SOCKET_R)
    n = qrot(rot["hand.R"], (-1, 0, 0))
    u = qrot(rot["hand.R"], (0, 0, 1))
    return s + n * SHIELD_FACE, n, u, rot, pos


def raise_cost(centre, bend_pref, wrist_w):
    """The shield's centre at `centre`, facing forward, upright; the elbow near
    `bend_pref`; the wrist as close to straight on the forearm as that allows
    (a forearm under a shield does not crank its wrist)."""
    def cost(local):
        c_, n, u, rot, pos = shield_frame(local)
        c = 4.0 * float(np.sum((c_ - centre) ** 2))
        c += 400.0 * (1.0 - float(np.dot(n, FWD)))
        c += 80.0 * (1.0 - float(np.dot(u, UP)))
        sw, tw = swing_twist(local["hand.R"], (0, -1, 0))
        c += wrist_w * sw ** 2 + 4.0 * tw ** 2
        c += 400.0 * max(0.0, sw - math.radians(40)) ** 2
        bend = qangle(local["armL.R"])
        c += 20.0 * (bend - bend_pref) ** 2
        return c
    return cost


def solve(cost):
    def f(x):
        local = unpack(x)
        return cost(local) + 2000.0 * limit_penalty(local)
    best = None
    x0 = np.zeros(7)
    x0[0:3] = (-1.2, 0.0, 0.0)   # arm forward
    x0[3] = 0.5                  # ~80 deg elbow
    for seed in range(16):
        start = x0 + (np.random.RandomState(seed).randn(7) * 0.6 if seed else 0)
        r = minimize(f, start, method="BFGS", options={"maxiter": 8000, "gtol": 1e-9})
        if best is None or r.fun < best.fun:
            best = r
    return unpack(best.x), best.fun


def report(name, local, centre):
    c, n, u, rot, pos = shield_frame(local)
    print(f"== {name}")
    print(f"   shield centre {np.round(c, 1)}  (want {np.round(centre, 1)})")
    print(f"   face normal   {np.round(n, 2)}   up {np.round(u, 2)}")
    print(f"   elbow {np.round(pos['armL.R'], 1)}  wrist {np.round(pos['hand.R'], 1)}")
    print(f"   elbow bend {math.degrees(qangle(local['armL.R'])):.0f} deg")
    sw, tw = swing_twist(local["hand.R"], (0, -1, 0))
    print(f"   wrist bend {math.degrees(sw):.0f} roll {math.degrees(tw):.0f} deg")
    print(f"   limit penalty {limit_penalty(local):.5f}")


def main():
    check = "--check" in sys.argv
    # Rig landmarks (art cells, 80/m): the body's mid-plane is x = 18, the
    # shoulders y = 106, the eye ~y 120; +Z is forward, the chest's front ~z 16.
    # A RIGHT-hand shield sits toward the body's right (x < 18).
    #
    # SHIELD_RAISE: a round/heater shield held close, its centre in front of
    # the sternum a hand toward the shield side, ~1.12 m up and ~0.2 m out:
    # raised level it covers hips to eyes (a 0.6 m shield) and leaves the
    # crown and the legs to a blow that goes round it.
    centre = np.array([11.0, 90.0, 33.0])
    raise_pose, err = solve(raise_cost(centre, math.radians(85), 30.0))
    report(f"shield_raise (residual {err:.3f})", raise_pose, centre)
    # BUCKLER_RAISE: a buckler is put where the blade is going, not stood
    # behind -- the arm well out, the steel at the height of the face's
    # line and a forearm out from the chest.
    centreB = np.array([12.0, 97.0, 42.0])
    buck_pose, errB = solve(raise_cost(centreB, math.radians(45), 30.0))
    report(f"buckler_raise (residual {errB:.3f})", buck_pose, centreB)
    if check:
        return
    base = ("Solved by scripts/gen_shield_clips.py from targets -- edit them there "
            "and re-run, never these quaternions. ")
    for name, pose, what in (
        ("shield_raise", raise_pose,
         "A SHIELD RAISED (Mob::StepShield, item.h ShieldSpec::clip): the shield arm "
         "brought up with the forearm across the chest, the shield upright and facing "
         "forward in front of the sternum. "),
        ("buckler_raise", buck_pose,
         "A BUCKLER PUT OUT (Mob::StepShield): the arm well forward, the steel a forearm "
         "out from the chest at the face's height -- a buckler is put where the blade "
         "is going, not stood behind. ")):
        d = clip_json(
            name,
            what + "Authored for a shield in the RIGHT fist; the left plays the derived "
            ".mirror. The engine sets this clip's WEIGHT to the raise fraction every tick "
            "(restFill: weight w is w of the way from rest to the key), so the arm is "
            "exactly as far up as the mechanics say at whatever speed the shield comes up; "
            "no blend-in for that reason. pitchFollow lifts and tilts the shield with the "
            "look. " + base,
            pose, ARM,
            extra={"blendInMs": 0, "blendOutMs": 0,
                   "quietAdditive": {p: 1.0 for p in ARM},
                   "pitchFollow": {"armU.R": 0.85}})
        d["restFill"] = True
        write(os.path.join(ANIMS, name + ".json"), d)


if __name__ == "__main__":
    main()
