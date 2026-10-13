#!/usr/bin/env python3
"""Solve the archery and sneak clips from TARGET POINTS on the stock human rig.

Writes assets/anims/bow_draw.json, bow_draw_crouch.json and sneak.json. Run it
again after changing a target below; hand-tuning the quaternions in the JSON is
how the first bow_draw ended up with its string arm's elbow out in front of the
face, where first person looks through it.

THE RIG (assets/mobs/human.json): art cells at 80 per metre, the character
facing +Z with its left at +X, Y up; every part's local rotation is about its
own anchor in its PARENT's rest frame, rest = arms hanging along -Y (pose.cpp,
anim.cpp AnimFlatten). The pose limits the engine clamps every pose to
(anim.cpp AnimClampPoseLimits: shoulder reach half-spaces + twist, elbow hinge
0..130) are penalties here, so a solved pose is one the engine will not move.

THE BOW lies along the bow hand's local +Z (with the arm held out ahead, local
+Z is world up -- the standing draw holds it upright). The crouched draw rolls
the hand a quarter turn so the bow lies flat.

    python scripts/gen_archery_clips.py          # solve, write, print the FK
    python scripts/gen_archery_clips.py --check  # print only
"""
import json
import math
import re
import os
import sys

import numpy as np
from scipy.optimize import minimize

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ANIMS = os.path.join(ROOT, "assets", "anims")

# ---- the rig --------------------------------------------------------------
ANCHOR = {
    "torso": (18, 72, 10), "head": (18, 104, 10),
    "armU.L": (32, 106, 10), "armL.L": (32, 86, 10), "hand.L": (32, 66, 10),
    "armU.R": (4, 106, 10), "armL.R": (4, 86, 10), "hand.R": (4, 66, 10),
}
PARENT = {
    "torso": None, "head": "torso",
    "armU.L": "torso", "armL.L": "armU.L", "hand.L": "armL.L",
    "armU.R": "torso", "armL.R": "armU.R", "hand.R": "armL.R",
}
ORDER = ["torso", "head", "armU.L", "armL.L", "hand.L", "armU.R", "armL.R", "hand.R"]
EYE_LOCAL = (0, 16, 0)          # human.json eyeLocal, from the head anchor
SOCKET_R = (6, 61, 10)          # human.json sockets.held_right (hand.R)
STRING_L = (31, 57, 10)         # the draw hand's hooked fingers (hand.L)
# Shoulder limits (human.json poseLimit), in the part's rest frame.
REACH = {
    "armU.L": [((0, 0, -1), 50), ((-1, 0, 0), 30)],
    "armU.R": [((0, 0, -1), 50), ((1, 0, 0), 30)],
}
TWIST_MAX = math.radians(75)
HINGE_MAX = math.radians(130)
# The torso's share of the look pitch (the clips' pitchFollow): most of it, so
# the bow stays where the eye put it and first person sees the same draw at
# any aim; the head hands that share back (pose.cpp), so the face turns by the
# look alone.
FOLLOW = 0.85


# ---- quaternions (x, y, z, w) ----------------------------------------------
def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return np.array([aw * bx + ax * bw + ay * bz - az * by,
                     aw * by - ax * bz + ay * bw + az * bx,
                     aw * bz + ax * by - ay * bx + az * bw,
                     aw * bw - ax * bx - ay * by - az * bz])


def qconj(q):
    return np.array([-q[0], -q[1], -q[2], q[3]])


def qrot(q, v):
    p = np.array([v[0], v[1], v[2], 0.0])
    return qmul(qmul(q, p), qconj(q))[:3]


def qaxis(axis, ang):
    a = np.asarray(axis, float)
    a = a / np.linalg.norm(a)
    s = math.sin(ang / 2)
    return np.array([a[0] * s, a[1] * s, a[2] * s, math.cos(ang / 2)])


def qrotvec(rv):
    ang = float(np.linalg.norm(rv))
    if ang < 1e-9:
        return np.array([0.0, 0.0, 0.0, 1.0])
    return qaxis(rv / ang, ang)


def qfromto(a, b):
    a = np.asarray(a, float) / np.linalg.norm(a)
    b = np.asarray(b, float) / np.linalg.norm(b)
    c = np.cross(a, b)
    d = float(np.dot(a, b))
    if d < -0.999999:
        axis = np.cross(a, (1, 0, 0))
        if np.linalg.norm(axis) < 1e-6:
            axis = np.cross(a, (0, 1, 0))
        return qaxis(axis, math.pi)
    q = np.array([c[0], c[1], c[2], 1.0 + d])
    return q / np.linalg.norm(q)


def qangle(q):
    return 2 * math.acos(min(1.0, abs(q[3])))


# ---- forward kinematics ------------------------------------------------------
def fk(local):
    """local: part -> quat. Returns (model rot, joint pos) per part."""
    rot, pos = {}, {}
    for p in ORDER:
        q = local.get(p, np.array([0.0, 0.0, 0.0, 1.0]))
        par = PARENT[p]
        if par is None:
            rot[p] = q
            pos[p] = np.array(ANCHOR[p], float)
        else:
            rot[p] = qmul(rot[par], q)
            pos[p] = pos[par] + qrot(rot[par], np.subtract(ANCHOR[p], ANCHOR[par]))
    return rot, pos


def point(rot, pos, part, p):
    return pos[part] + qrot(rot[part], np.subtract(p, ANCHOR[part]))


def limit_penalty(local):
    pen = 0.0
    for p, planes in REACH.items():
        q = local[p]
        d = qrot(q, (0, -1, 0))
        for n, deg in planes:
            over = float(np.dot(d, n)) - math.sin(math.radians(deg))
            if over > 0:
                pen += over * over
        swing = qfromto((0, -1, 0), d)
        tw = qmul(qconj(swing), q)
        ang = qangle(tw)
        if ang > TWIST_MAX:
            pen += (ang - TWIST_MAX) ** 2
    return pen


# ---- the solver -------------------------------------------------------------
# Free: the torso's YAW (the head turns back by the same amount, so the face
# and the eye stay square to the look), armU.* as rotation vectors, armL.* as
# one hinge angle each (about -X, the engine's hinge), hand.* as rotation
# vectors. Everything else is given.
#
# WHY THE TORSO TURNS. With the chest square to the target the string hand
# cannot reach the jaw with its elbow behind the shoulder: the shoulder joint
# sits beside the jaw, so the hand at the jaw and the elbow behind it make the
# SHOULDER the fold, and the elbow's 130 degrees cannot close a 40-cell arm onto
# a point 12 cells from its own shoulder. A real archer stands side-on for the
# same reason. The yaw is about the waist joint, which lies on the head's axis,
# so the eye (first person rides it) does not move.
FREE3 = ["armU.L", "hand.L", "armU.R", "hand.R"]
FREE1 = ["armL.L", "armL.R"]


def hinge_of(u):
    """The elbow's bend from an unbounded variable: 0..HINGE_MAX exactly, so
    the solver cannot hand the engine a bend its hinge clamp would move."""
    return HINGE_MAX / (1.0 + math.exp(-u))


def unpack(x, fixed):
    local = {k: v.copy() for k, v in fixed.items()}
    yaw = x[0]
    local["torso"] = qmul(qaxis((0, 1, 0), yaw), fixed.get("torso", np.array([0.0, 0, 0, 1])))
    local["head"] = qaxis((0, 1, 0), -yaw)
    i = 1
    for p in FREE3:
        local[p] = qrotvec(x[i:i + 3])
        i += 3
    for p in FREE1:
        local[p] = qaxis((-1, 0, 0), hinge_of(x[i]))
        i += 1
    return local


def solve(fixed, cost, yaw0=0.8):
    n = 1 + 3 * len(FREE3) + len(FREE1)
    x0 = np.zeros(n)
    x0[0] = yaw0
    # Start the arms forward and a little bent: every target here is ahead.
    x0[1:4] = (-1.4, 0, 0)
    x0[7:10] = (-1.4, 0, 0)
    x0[13] = 1.0     # hinge_of(1.0) ~ 95 deg
    x0[14] = -2.0    # ~ 15 deg

    def f(x):
        local = unpack(x, fixed)
        return cost(local) + 2000.0 * limit_penalty(local)

    best = None
    for seed in range(10):
        start = x0 + (np.random.RandomState(seed).randn(n) * 0.5 if seed else 0)
        r = minimize(f, start, method="BFGS", options={"maxiter": 6000, "gtol": 1e-9})
        if best is None or r.fun < best.fun:
            best = r
    return unpack(best.x, fixed), best.fun


# ---- targets -----------------------------------------------------------------
def unit(v):
    v = np.asarray(v, float)
    return v / np.linalg.norm(v)


def swing_twist(q, axis):
    """(swing angle, twist angle) of q about `axis` (unit, local)."""
    d = qrot(q, axis)
    swing = qfromto(axis, d)
    tw = qmul(qconj(swing), q)
    return qangle(swing), qangle(tw)


def draw_cost(grip, anchor, elbow_y, bow_up, yaw_pref):
    """The bow arm reaches the grip nearly straight with the bow along bow_up;
    the string hand's fingers sit at the anchor under the jaw and the draw
    elbow rides at elbow_y BEHIND the shoulder line (so first person, at the
    eye, never looks through the forearm)."""
    def cost(local):
        rot, pos = fk(local)
        c = 0.0
        g = point(rot, pos, "hand.R", SOCKET_R)
        c += 4.0 * np.sum((g - grip) ** 2)
        s = point(rot, pos, "hand.L", STRING_L)
        c += 4.0 * np.sum((s - anchor) ** 2)
        # Bow orientation: the hand's local +Z onto bow_up.
        z = qrot(rot["hand.R"], (0, 0, 1))
        c += 200.0 * (1.0 - float(np.dot(z, bow_up)))
        # The draw elbow: at height, and BEHIND the eye plane by a forearm's
        # worth of clearance, out past the shoulder.
        e = pos["armL.L"]
        c += 0.5 * (e[1] - elbow_y) ** 2
        c += 1.0 * max(0.0, e[2] - (ANCHOR["head"][2] - 10.0)) ** 2
        # The string hand may cock its wrist in toward the jaw, no more than a
        # real hooked draw does; the bow hand stays straight on its forearm
        # but may ROLL (the forearm's own pronation -- how the crouch lays the
        # bow flat).
        sw, tw = swing_twist(local["hand.L"], (0, -1, 0))
        c += 2.0 * sw ** 2 + 400.0 * max(0.0, sw - math.radians(55)) ** 2 + 2.0 * tw ** 2
        swr, twr = swing_twist(local["hand.R"], (0, -1, 0))
        c += 60.0 * swr ** 2 + 2.0 * twr ** 2 + 400.0 * max(0.0, twr - math.radians(100)) ** 2
        bend = qangle(local["armL.R"])
        c += 20.0 * (bend - math.radians(10)) ** 2
        # As little twist at the waist as the draw needs.
        yaw = 2 * math.atan2(local["torso"][1], local["torso"][3])
        c += 10.0 * (yaw - yaw_pref) ** 2
        return c
    return cost


def fmt_q(q):
    q = np.asarray(q, float)
    if q[3] < 0:
        q = -q
    return [round(float(v), 4) for v in q]


FOV_Y = 1.35            # tuning.json camera.fovY (radians)
ASPECT = 16.0 / 9.0
# Each arm part as a bone (joint -> the next joint, or the hand's length) and
# a radius, in rest coordinates: what first person can see of it.
BONES = {
    "armU.L": ((32, 106, 10), (32, 86, 10), 5.5), "armL.L": ((32, 86, 10), (32, 66, 10), 5.0),
    "hand.L": ((32, 66, 10), (32, 54, 10), 4.0),
    "armU.R": ((4, 106, 10), (4, 86, 10), 5.5), "armL.R": ((4, 86, 10), (4, 66, 10), 5.0),
    "hand.R": ((4, 66, 10), (4, 54, 10), 4.0),
}


def look_pose(local, pitch, follow=0.6):
    """The pose as pose.cpp leaves it for a look `pitch` (radians, + up): the
    head's look, then pitchFollow's share on the torso about the hips' X (the
    head handing that share back, so the face turns by the look alone)."""
    out = dict(local)
    out["head"] = qmul(local.get("head", np.array([0.0, 0, 0, 1])), qaxis((1, 0, 0), -pitch * (1 - follow)))
    out["torso"] = qmul(qaxis((1, 0, 0), -pitch * follow), local.get("torso", np.array([0.0, 0, 0, 1])))
    return out


def fp_visible(local, pitch, follow=0.6, posePitch=None):
    """Which arm parts first person sees at a look `pitch`, as the share of the
    view's height each one covers at its nearest (0 = out of view)."""
    lp = look_pose(local, pitch if posePitch is None else posePitch, follow)
    rot, pos = fk(lp)
    eye = point(rot, pos, "head", np.add(ANCHOR["head"], EYE_LOCAL))
    fwd = np.array([0.0, math.sin(pitch), math.cos(pitch)])
    right = np.array([-1.0, 0.0, 0.0])
    up = np.cross(right, fwd) * -1.0
    ty = math.tan(FOV_Y / 2)
    tx = ty * ASPECT
    seen = {}
    for p, (a, b, r) in BONES.items():
        best = 0.0
        for t in np.linspace(0, 1, 9):
            c = point(rot, pos, p, np.add(a, np.multiply(np.subtract(b, a), t)))
            v = c - eye
            z = float(np.dot(v, fwd))
            if z < 1.0:
                continue
            x = float(np.dot(v, right)) / z
            y = float(np.dot(v, up)) / z
            # The bone's disc, grown by its radius, against the frustum.
            rr = r / z
            if abs(x) - rr < tx and abs(y) - rr < ty:
                best = max(best, min(1.0, 2 * rr / (2 * ty)))
        seen[p] = round(best, 2)
    return seen


def report(name, local):
    rot, pos = fk(local)
    eye = point(rot, pos, "head", np.add(ANCHOR["head"], EYE_LOCAL))
    g = point(rot, pos, "hand.R", SOCKET_R)
    s = point(rot, pos, "hand.L", STRING_L)
    print(f"== {name}")
    print(f"   eye      {np.round(eye, 1)}")
    for p in ("armL.R", "hand.R", "armL.L", "hand.L"):
        print(f"   {p:7s}  {np.round(pos[p], 1)}  (rel eye {np.round(pos[p] - eye, 1)})")
    print(f"   grip     {np.round(g, 1)}   string {np.round(s, 1)}")
    z = qrot(rot["hand.R"], (0, 0, 1))
    print(f"   bow axis {np.round(z, 2)}")
    for p in ("armL.L", "armL.R"):
        print(f"   {p} bend {math.degrees(qangle(local[p])):.0f} deg")
    for p in ("hand.L", "hand.R"):
        sw, tw = swing_twist(local[p], (0, -1, 0))
        print(f"   {p} wrist bend {math.degrees(sw):.0f} roll {math.degrees(tw):.0f} deg")
    print(f"   string-arm elbow z {pos['armL.L'][2]:.1f} (eye z {eye[2]:.1f})")
    for deg in (-35, -20, 0, 20, 35):
        print(f"   first person at look {deg:+d}: {fp_visible(local, math.radians(deg), FOLLOW)}")
    # A fast flick: the camera there, the pose (eased on headLookHalflife)
    # still half way.
    for deg in (-40, 40):
        print(f"   first person flick to {deg:+d}, pose at half: "
              f"{fp_visible(local, math.radians(deg), FOLLOW, math.radians(deg / 2))}")
    t = local.get("torso", np.array([0.0, 0, 0, 1]))
    print(f"   torso yaw {math.degrees(2 * math.atan2(t[1], t[3])):.0f} deg")
    print(f"   limit penalty {limit_penalty(local):.5f}")


def clip_json(name, comment, local, parts, mode="override", extra=None, dur=1200):
    tracks = {}
    for p in parts:
        q = fmt_q(local.get(p, (0, 0, 0, 1)))
        tracks[p] = {"rot": [{"t": 0, "q": q}, {"t": dur, "q": q}]}
    extra = extra or {}
    d = {"//": comment, "name": name, "sidecarVoxelsPerMetre": 10,
         "durationMs": dur, "loop": True, "mode": mode}
    for k in ("blendInMs", "blendOutMs"):
        if k in extra:
            d[k] = extra[k]
    d["mask"] = list(parts)
    for k in ("quietAdditive", "pitchFollow"):
        if k in extra:
            d[k] = extra[k]
    d["tracks"] = tracks
    return d


def write(path, d):
    text = json.dumps(d, indent=2)
    # One key per line, as the library's hand-written files have them.
    text = re.sub(r'\{\s*"t": (\d+),\s*"q": \[\s*([-\d.e]+),\s*([-\d.e]+),\s*'
                  r'([-\d.e]+),\s*([-\d.e]+)\s*\]\s*\}',
                  r'{ "t": \1, "q": [\2, \3, \4, \5] }', text)
    text = re.sub(r'"mask": \[([^\]]*)\]',
                  lambda m: '"mask": [' + ", ".join(x.strip() for x in m.group(1).split(",")) + "]",
                  text)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text + "\n")
    print("wrote", os.path.relpath(path, ROOT))


ARMS = ["armU.R", "armL.R", "hand.R", "armU.L", "armL.L", "hand.L"]

# The sneak, in degrees (see main()).
SNEAK_LEAN = 25.0        # chest forward over the hips
SNEAK_HEAD_BACK = 18.0   # the head's share turned back up
SNEAK_ARM_FWD = -4.0     # arms forward past hanging plumb (- = a little back)
SNEAK_ARM_OUT = 22.0     # ...and out from the sides
SNEAK_ELBOW = 28.0       # soft elbows


def main():
    check = "--check" in sys.argv
    I = np.array([0.0, 0.0, 0.0, 1.0])
    eye = np.add(ANCHOR["head"], EYE_LOCAL)

    # What the library holds now, for comparison.
    cur = os.path.join(ANIMS, "bow_draw.json")
    try:
        d = json.load(open(cur, encoding="utf-8"))
        local = {p: np.array(t["rot"][0]["q"], float) for p, t in d["tracks"].items()}
        report("bow_draw AS IT IS ON DISK", local)
    except (OSError, ValueError, KeyError):
        pass

    # ---- STANDING DRAW --------------------------------------------------------
    # Bow hand straight out ahead of the face, a hand's width right of the eye
    # line and a little under it, the bow upright; the string hand's fingers
    # under the left of the jaw, its elbow out behind the shoulder at shoulder
    # height -- the line a real draw makes, and one first person cannot see.
    grip = np.array([eye[0] - 10.0, eye[1] - 11.0, eye[2] + 56.0])
    anchor = np.array([eye[0] + 4.5, eye[1] - 12.0, eye[2] + 2.0])
    stand, err = solve({}, draw_cost(grip, anchor, ANCHOR["armU.L"][1] + 2,
                                     unit((0, 1, 0)), math.radians(55)))
    report(f"bow_draw (residual {err:.3f})", stand)

    # ---- CROUCHED DRAW: the bow laid flat --------------------------------------
    # The same draw from a crouch, the bow canted a quarter turn so it lies
    # across the body (top limb toward the draw side), held a little lower and
    # closer: a low, short draw under cover.
    gripC = np.array([eye[0] - 3.0, eye[1] - 15.0, eye[2] + 55.0])
    anchorC = np.array([eye[0] + 4.5, eye[1] - 14.0, eye[2] + 2.0])
    crouch, errC = solve({}, draw_cost(gripC, anchorC, ANCHOR["armU.L"][1],
                                       unit((1, 0.12, 0)), math.radians(55)))
    report(f"bow_draw_crouch (residual {errC:.3f})", crouch)

    # ---- THE SNEAK: an additive on the held crouch ----------------------------
    # Deltas about REST (anim.cpp stage 3), weighted by how far the crouch has
    # sunk (pose.cpp). The chest pitched over the hips; the head turned most of
    # the way back up, so the gaze stays forward and a little down (the look
    # rides on top); the arms brought forward by the lean they hang from (else
    # they trail behind the hips) and a little more, out from the sides, the
    # elbows soft -- hands low and ready, the way a body creeps.
    lean = math.radians(SNEAK_LEAN)
    sneak = {
        "torso": qaxis((1, 0, 0), lean),
        "head": qaxis((1, 0, 0), -math.radians(SNEAK_HEAD_BACK)),
        "armU.L": qmul(qaxis((0, 0, 1), math.radians(SNEAK_ARM_OUT)),
                       qaxis((1, 0, 0), -lean - math.radians(SNEAK_ARM_FWD))),
        "armU.R": qmul(qaxis((0, 0, 1), -math.radians(SNEAK_ARM_OUT)),
                       qaxis((1, 0, 0), -lean - math.radians(SNEAK_ARM_FWD))),
        "armL.L": qaxis((-1, 0, 0), math.radians(SNEAK_ELBOW)),
        "armL.R": qaxis((-1, 0, 0), math.radians(SNEAK_ELBOW)),
    }
    rot, pos = fk(sneak)
    print("== sneak (additive, at full crouch)")
    for p in ("head", "hand.L", "hand.R"):
        print(f"   {p:7s}  {np.round(pos[p], 1)}")

    if check:
        return
    write(os.path.join(ANIMS, "sneak.json"), clip_json(
        "sneak",
        "SNEAKING (pose.cpp, the held crouch): an ADDITIVE layer weighted by how far the "
        "Ctrl crouch has sunk, so a crouch reads as creeping and not as a squat. The chest "
        "pitched over the hips, the head turned most of the way back up (the look rides on "
        "top), the arms brought forward by the lean and a little out from the sides with soft "
        "elbows. Additive about rest: an override that owns the arms (bow_draw*, spell_ready) "
        "quiets it there, and the walk's arm swing still lands on top. "
        + "Written by scripts/gen_archery_clips.py (SNEAK_* constants).",
        sneak, ["torso", "head", "armU.L", "armL.L", "armU.R", "armL.R"],
        mode="additive", extra={"blendInMs": 0, "blendOutMs": 0}))
    base = ("Solved by scripts/gen_archery_clips.py from target points -- edit the "
            "targets there and re-run, never these quaternions. ")
    write(os.path.join(ANIMS, "bow_draw.json"), clip_json(
        "bow_draw",
        "DRAWING A BOW (session.cpp PhaseG's bow block, game/arrows.h): played while the bow "
        "hand's button is held, stopped on the loose. Authored for the bow in the RIGHT fist; a "
        "bow in the left plays the library's derived bow_draw.mirror. The bow arm straight out "
        "ahead of the face, a hand right of the eye line, the bow upright; the string hand "
        "hooked under the left of the jaw with its ELBOW OUT BEHIND THE SHOULDER at shoulder "
        "height -- the line a real draw makes, and one first person (the eye rides the head) "
        "never looks through. pitchFollow tips the torso, and both arms with it, with the look. "
        + base, stand, ARMS + ["torso", "head"],
        extra={"blendInMs": 220, "blendOutMs": 140,
               "quietAdditive": {p: 1.0 for p in ARMS},
               "pitchFollow": {"torso": FOLLOW}}))
    write(os.path.join(ANIMS, "bow_draw_crouch.json"), clip_json(
        "bow_draw_crouch",
        "DRAWING A BOW FROM A CROUCH (session.cpp's bow block plays this instead of bow_draw "
        "while the player is crouched). The bow laid flat, canted a quarter turn by the bow "
        "hand so its top limb points across to the draw side, held a little lower and closer "
        "than the standing draw; the same string-hand line (elbow behind the shoulder). The "
        "torso and head are this clip's: the sneak lean (sneak.json, an additive) is quieted "
        "there so the arrow is not aimed into the ground. "
        + base, crouch, ARMS + ["torso", "head"],
        extra={"blendInMs": 220, "blendOutMs": 140,
               "quietAdditive": dict({p: 1.0 for p in ARMS}, torso=1.0, head=1.0),
               "pitchFollow": {"torso": FOLLOW}}))


if __name__ == "__main__":
    main()
