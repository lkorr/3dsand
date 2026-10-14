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
# The draw hand's NOCK point: the hand.L socket (human.json held_right on
# hand.R, mirrored the way MobSystem derives held_left). The drawn string and
# the nocked arrow are placed there at runtime (game/arrows.cpp
# PresentDrawnBow, Mob::HandSocketWorld), so it is what the solve aims.
STRING_L = (30, 61, 10)
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
    return HINGE_MAX / (1.0 + math.exp(-max(-60.0, min(60.0, u))))


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


def solve(fixed, cost, yaw0=0.8, warm=None):
    """Returns (pose, residual, x). `warm` = the x of the previous key of the
    same clip: the solve starts there and only there, so consecutive keys stay
    on one branch of the arm's solutions and the engine's slerp between them
    is a short, continuous move instead of a flip through the body."""
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

    starts = [np.array(warm, float)] if warm is not None else [
        x0 + (np.random.RandomState(seed).randn(n) * 0.5 if seed else 0) for seed in range(10)]
    best = None
    for start in starts:
        r = minimize(f, start, method="BFGS", options={"maxiter": 6000, "gtol": 1e-9})
        if best is None or r.fun < best.fun:
            best = r
    return unpack(best.x, fixed), best.fun, best.x


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


def draw_cost(grip, anchor, elbow_y, bow_up, yaw_pref, behind=1.0, bow_bend=10.0):
    """The bow arm reaches the grip nearly straight with the bow along bow_up;
    the string hand's fingers sit at the anchor under the jaw and the draw
    elbow rides at elbow_y BEHIND the shoulder line (so first person, at the
    eye, never looks through the forearm). `behind` scales that last rule:
    1 at full draw, 0 at brace, where the string hand is out at the bow and
    the elbow is in front of the shoulder because the forearm is."""
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
        c += behind * max(0.0, e[2] - (ANCHOR["head"][2] - 10.0)) ** 2
        # ...and OUT to the draw side, past the head: with the anchor under
        # the middle of the chin (aimed at the reticle) the cheapest elbow is
        # straight behind the head, and the forearm then runs through the
        # neck. A real draw brings the forearm in along the jaw from the side.
        c += behind * 2.0 * max(0.0, (ANCHOR["head"][0] + ELBOW_OUT) - e[0]) ** 2
        # The string hand may cock its wrist in toward the jaw, no more than a
        # real hooked draw does; the bow hand stays straight on its forearm
        # but may ROLL (the forearm's own pronation -- how the crouch lays the
        # bow flat).
        sw, tw = swing_twist(local["hand.L"], (0, -1, 0))
        c += 2.0 * sw ** 2 + 400.0 * max(0.0, sw - math.radians(55)) ** 2 + 2.0 * tw ** 2
        swr, twr = swing_twist(local["hand.R"], (0, -1, 0))
        c += 60.0 * swr ** 2 + 2.0 * twr ** 2 + 400.0 * max(0.0, twr - math.radians(100)) ** 2
        bend = qangle(local["armL.R"])
        c += 20.0 * (bend - math.radians(bow_bend)) ** 2
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


def reticle_clearance(local, pitch=0.0, follow=FOLLOW):
    """Degrees from the view's centre to the nearest edge of each arm part in
    first person (negative = the part covers the reticle)."""
    lp = look_pose(local, pitch, follow)
    rot, pos = fk(lp)
    e = point(rot, pos, "head", np.add(ANCHOR["head"], EYE_LOCAL))
    fwd = np.array([0.0, math.sin(pitch), math.cos(pitch)])
    right = np.array([-1.0, 0.0, 0.0])
    up = np.cross(right, fwd) * -1.0
    out = {}
    for p, (a, b, r) in BONES.items():
        best = 99.0
        for t in np.linspace(0, 1, 15):
            c = point(rot, pos, p, np.add(a, np.multiply(np.subtract(b, a), t)))
            v = c - e
            z = float(np.dot(v, fwd))
            if z < 1.0:
                continue
            off = math.hypot(float(np.dot(v, right)), float(np.dot(v, up)))
            best = min(best, math.degrees(math.atan(off / z)) - math.degrees(math.atan(r / z)))
        out[p] = round(best, 1)
    return out


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
    print(f"   reticle clearance at look 0 (deg from the crosshair to each arm part's edge; "
          f"< 0 = over it): {reticle_clearance(local)}")
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


def keyed_clip_json(name, comment, keys, parts, extra):
    """A ONE-SHOT that HOLDS its last key (AnimClip::holdEnd): `keys` is
    [(t ms, pose)], the last one the held pose."""
    tracks = {}
    for p in parts:
        tracks[p] = {"rot": [{"t": int(round(t)), "q": fmt_q(local.get(p, (0, 0, 0, 1)))}
                             for t, local in keys]}
    d = {"//": comment, "name": name, "sidecarVoxelsPerMetre": 10,
         "durationMs": int(round(keys[-1][0])), "loop": False, "holdEnd": True,
         "mode": "override"}
    for k in ("blendInMs", "blendOutMs"):
        if k in extra:
            d[k] = extra[k]
    d["mask"] = list(parts)
    for k in ("quietAdditive", "pitchFollow"):
        if k in extra:
            d[k] = extra[k]
    d["tracks"] = tracks
    return d


def bow_timing():
    """(windupMs, drawTimeMs) of items.json's `bow`: the clips are authored on
    the bow's own timeline (game/arrows.h BowDrawFraction), so the string is
    where the loosed speed says it is."""
    with open(os.path.join(ROOT, "assets", "items", "items.json"), encoding="utf-8") as fh:
        items = json.load(fh)
    rows = items["items"] if isinstance(items, dict) else items
    for r in rows:
        if r.get("id") == "bow":
            b = r["bow"]
            return float(b.get("windupMs", 0)), float(b["drawTimeMs"])
    raise SystemExit("items.json has no `bow` row")


# The pull is sampled at these fractions of its TIME; the string sits at the
# eased draw fraction of each (1 - (1 - t)^2, BowDrawFraction), so the hand
# moves fast off the brace and creeps the last inches into the anchor.
PULL_SAMPLES = (0.25, 0.5, 0.75, 1.0)
BRACE = 16.0        # cells from the grip to the string at brace (~20 cm)
# Where the WINDUP leaves the string: this share of the way from brace to the
# anchor. Not brace itself -- the rig cannot put the string hand there: with
# the torso turned side-on for the anchor, the far arm reaches no more than
# ~17 cells ahead of the eye with the bow arm out (brace is 40), and a SET
# key solved at brace drags the BOW hand back to meet it. So the windup raises
# the bow and draws most of the way in one motion, and the PULL is the last
# ~19 cm -- the slight, slow pull that builds the tension (owner's words) and
# that the loosed speed is priced on.
SET_DRAW = 0.62
# Cost per rad^2 of a key's joints turning away from the next key's (see
# solve_draw's `near`): enough to pin the free roll, small next to the targets.
CONTINUITY = 30.0


def solve_draw(label, grip, anchor, lift_grip, lift_up, bow_up, elbow_y, yaw):
    """The whole draw as keys: LIFT (t=0: the bow coming up low and canted,
    the string hand at the nock), SET (end of the windup: the bow on the aim,
    fingers on the string at brace), then the PULL from brace to the anchor.
    Returns ([(fraction of the timeline, pose)], residuals)."""
    windup, draw = bow_timing()
    total = windup + draw
    line = anchor - grip
    brace = grip + line * (BRACE / float(np.linalg.norm(line)))
    brace = brace + (anchor - brace) * SET_DRAW      # the SET string point
    keys, errs = [], []

    def near(cost, prev, w=CONTINUITY):
        """The arm has a redundant ROLL about its own axis (the hand takes the
        roll back, so the grip and string do not care): left free, every key
        picks its own and the slerp between them rolls the arm back and
        forth. Charged here against the neighbouring key, so the clip only
        moves what the targets move."""
        if prev is None:
            return cost

        def c(local):
            return cost(local) + w * sum(
                qangle(qmul(qconj(prev[p]), local[p])) ** 2
                for p in FREE3 + FREE1 + ["torso"])
        return c

    prev = None
    # Solved BACKWARDS from the full draw (from scratch, the canonical pose)
    # with each earlier key warm-started from the one after it, so the whole
    # clip is one continuous branch that ends on the pose the archery was
    # tuned on.
    x = None
    for k in reversed(PULL_SAMPLES):
        f = 1.0 - (1.0 - k) ** 2
        pose, e, x = solve({}, near(draw_cost(grip, brace + (anchor - brace) * f, elbow_y,
                                              bow_up, yaw, behind=f), prev), warm=x)
        keys.append((windup + draw * k, pose)); errs.append(e)
        prev = pose
    # SET (end of the windup): the bow up on the aim, the string most of the way back.
    pose, e, x = solve({}, near(draw_cost(grip, brace, elbow_y, bow_up, yaw,
                                          behind=0.0), prev), warm=x)
    keys.append((windup, pose)); errs.append(e)
    prev = pose
    # LIFT (t=0, under the blend-in): the bow coming up low and canted, the
    # arrow laid on the rest, the string hand at the nock a little behind the
    # grip on the arrow's line, the elbow low.
    nock = lift_grip + unit(line) * 9.0
    pose, e, x = solve({}, near(draw_cost(lift_grip, nock, elbow_y - 14, lift_up,
                                          yaw * 0.6, behind=0.0, bow_bend=25.0), prev),
                       warm=x)
    keys.append((0.0, pose)); errs.append(e)
    keys.reverse(); errs.reverse()
    print(f"== {label}: windup {windup:.0f} ms + pull {draw:.0f} ms, residuals "
          + " ".join(f"{v:.2f}" for v in errs))
    return keys


def aim_draw(label, eye, anchor, rest_off, lift_grip, lift_up, bow_up, elbow_y, yaw):
    """solve_draw with the grip AIMED (aimed_grip) from where the nock really
    lands: the rig cannot always put the fingers exactly on the anchor, and a
    grip aimed from the anchor then points the arrow off the reticle by the
    miss. Two passes: solve, re-aim the grip from the solved nock, solve."""
    nock = anchor
    for _ in range(2):
        keys = solve_draw(label, aimed_grip(eye, nock, rest_off), anchor, lift_grip,
                          lift_up, bow_up, elbow_y, yaw)
        rot, pos = fk(keys[-1][1])
        nock = point(rot, pos, "hand.L", STRING_L)
    rot, pos = fk(keys[-1][1])
    g = point(rot, pos, "hand.R", SOCKET_R) + rest_off
    d = unit(g - nock)
    miss = nock + d * ((ZERO - (nock[2] - eye[2])) / d[2]) - np.array([eye[0], eye[1], eye[2] + ZERO])
    print(f"   {label}: the nocked arrow crosses the look {ZERO:.0f} cells out "
          f"{np.round(miss[:2], 1)} cells off the reticle (x, y)")
    return keys


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

ZERO = 2400.0      # session.cpp kZero, 30 m, in art cells: where the shot crosses the look
DRAW = 46.0        # nock -> arrow rest at full draw, cells (the arrow is 52: 6 stand past)
REST_OUT = 2.0     # arrows.cpp PresentDrawnBow: the rest, cells off the grip along the flat
ELBOW_OUT = 20.0   # the draw elbow's least distance out from the head's middle, cells


def aimed_grip(eye, nock, rest_off):
    """The grip that puts the arrow from `nock` through its rest onto the
    crosshair's point ZERO cells straight ahead of the eye."""
    aim = np.array([eye[0], eye[1], eye[2] + ZERO])
    rest = nock + unit(aim - nock) * DRAW
    return rest - rest_off

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
    #
    # THE WINDUP before it (owner, 2026-10-14: "windup before being able to
    # fire, then pulling the string back slightly to build tension"): the bow
    # comes up from low and canted at the chest (LIFT) to the aim with the
    # fingers on the string at brace (SET) over items.json windupMs, and the
    # string is PULLED from brace to the anchor over drawTimeMs.
    #
    # THE ARROW AT THE RETICLE (owner, 2026-10-14: "the arm should be ever so
    # slightly pointed inwards so that it looks like the arrow is actually
    # pointing towards the reticle"). The nocked arrow is drawn from the nock
    # through the ARROW REST -- REST_OUT cells off the grip along the bow's
    # flat, toward the head (arrows.cpp) -- so the grip is SOLVED FROM the
    # anchor: on the line from the nock to the crosshair's point ZERO ahead,
    # DRAW cells out, less the rest offset. The anchor is under the middle of
    # the chin, the bow arm angled in to a grip a few cells right of the eye
    # line, and the shaft runs parallel to the look, a cell right of it.
    #
    # 2026-10-14 (owner): the anchor under the chin put the fingers INSIDE
    # the head (the eye is the middle of the head box) and the last of the
    # string through the lens. It is BESIDE THE LEFT CHEEK now, outside the
    # head and a little forward of the eye, where first person sees the whole
    # string come back to the hand at the edge of the view. Aimed at the
    # reticle from there, the shaft runs parallel to the look ~15 cm left, so
    # the bow is left of centre too.
    anchor = np.array([eye[0] + 12.0, eye[1] - 10.0, eye[2] + 9.0])
    grip = aimed_grip(eye, anchor, np.array([-REST_OUT, 0.0, 0.0]))
    liftGrip = np.array([eye[0] - 6.0, eye[1] - 32.0, eye[2] + 36.0])
    # The rest is on the HEAD's side of the bow (arrows.cpp), which with the
    # bow left of the eye is toward -x.
    standKeys = aim_draw("bow_draw", eye, anchor, np.array([-REST_OUT, 0.0, 0.0]),
                         liftGrip, unit((0.3, 1, 0.35)), unit((0, 1, 0)),
                         ANCHOR["armU.L"][1] + 2, math.radians(55))
    report("bow_draw at FULL DRAW", standKeys[-1][1])
    for t, k in standKeys[:-1]:
        print(f"   key t={t:.0f}: first person at look 0: {fp_visible(k, 0.0, FOLLOW)}")

    # ---- CROUCHED DRAW: the bow laid flat --------------------------------------
    # The same draw from a crouch, the bow canted a quarter turn so it lies
    # across the body (top limb toward the draw side), held a little lower and
    # closer: a low, short draw under cover.
    # The flat bow's rest is ON TOP of the grip (its flat normal is up).
    # Under the RIGHT of the jaw, not the middle: with the flat bow's grip
    # straight under the eye the bow arm is seen end-on from behind, across
    # the reticle (measured: upper arm and forearm 4 degrees INTO the
    # crosshair; at 9 cells right the bow hand is the nearest, 2 degrees off). The
    # shaft still runs parallel to the look, ~11 cm right of it.
    #
    # 2026-10-14 (owner): "the other arm is also inside the character's head,
    # so extend the bow holding arm out a bit more so that the drawstring
    # holding hand is in front of the head". Forward of the face (the head box
    # reaches ~8 cells past the eye), the bow arm out by the same amount.
    anchorC = np.array([eye[0] - 9.0, eye[1] - 14.0, eye[2] + 12.0])
    gripC = aimed_grip(eye, anchorC, np.array([0.0, REST_OUT, 0.0]))
    liftGripC = np.array([eye[0] - 2.0, eye[1] - 30.0, eye[2] + 34.0])
    crouchKeys = aim_draw("bow_draw_crouch", eye, anchorC, np.array([0.0, REST_OUT, 0.0]),
                          liftGripC, unit((1, 0.5, 0.3)), unit((1, 0.12, 0)),
                          ANCHOR["armU.L"][1], math.radians(55))
    report("bow_draw_crouch at FULL DRAW", crouchKeys[-1][1])

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
    timeline = ("A ONE-SHOT THAT HOLDS (holdEnd) on the bow's own timeline (items.json bow "
                "windupMs + drawTimeMs, game/arrows.h BowDrawFraction): t=0 LIFT, the bow low "
                "and canted, the string hand at the nock; end of the windup SET, the bow on "
                "the aim, the string already most of the way back -- nothing can be loosed before here; "
                "then the slow PULL of the last inches to the anchor, keyed at the eased draw fraction so the string "
                "is where the loosed speed says it is; then held. ")
    write(os.path.join(ANIMS, "bow_draw.json"), keyed_clip_json(
        "bow_draw",
        "DRAWING A BOW (session.cpp PhaseG's bow block, game/arrows.h): played while the bow "
        "hand's button is held, stopped on the loose. Authored for the bow in the RIGHT fist; a "
        "bow in the left plays the library's derived bow_draw.mirror. " + timeline +
        "At full draw the bow arm is straight out "
        "ahead of the face, a hand right of the eye line, the bow upright; the string hand "
        "hooked under the left of the jaw with its ELBOW OUT BEHIND THE SHOULDER at shoulder "
        "height -- the line a real draw makes, and one first person (the eye rides the head) "
        "never looks through. pitchFollow tips the torso, and both arms with it, with the look. "
        + base, standKeys, ARMS + ["torso", "head"],
        extra={"blendInMs": 220, "blendOutMs": 140,
               "quietAdditive": {p: 1.0 for p in ARMS},
               "pitchFollow": {"torso": FOLLOW}}))
    write(os.path.join(ANIMS, "bow_draw_crouch.json"), keyed_clip_json(
        "bow_draw_crouch",
        "DRAWING A BOW FROM A CROUCH (session.cpp's bow block plays this instead of bow_draw "
        "while the player is crouched). " + timeline + "At full draw the bow laid flat, canted a quarter turn by the bow "
        "hand so its top limb points across to the draw side, held a little lower and closer "
        "than the standing draw; the same string-hand line (elbow behind the shoulder). The "
        "torso and head are this clip's: the sneak lean (sneak.json, an additive) is quieted "
        "there so the arrow is not aimed into the ground. "
        + base, crouchKeys, ARMS + ["torso", "head"],
        extra={"blendInMs": 220, "blendOutMs": 140,
               "quietAdditive": dict({p: 1.0 for p in ARMS}, torso=1.0, head=1.0),
               "pitchFollow": {"torso": FOLLOW}}))


if __name__ == "__main__":
    main()
