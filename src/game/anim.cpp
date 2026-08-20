#include "game/anim.h"

#include <algorithm>
#include <cmath>

// Layered animation runtime — see anim.h for the determinism boundary note.
// Stage numbering matches docs/PLAN_voxel_editor.md §B:
//   1 SAMPLE  2 BLEND  3 ADDITIVE  4 FLATTEN  5 IK  (6 PHYSICS / 7 SUBMIT live
// in mob.cpp, which owns Jolt).

// ---- math -------------------------------------------------------------------

Quat QuatMul(const Quat& a, const Quat& b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
          a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Quat QuatConj(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }

Quat QuatAxisAngle(Vec3 axis, float angle) {
  Vec3 a = axis.normalized();
  if (a.len() < 1e-6f) return {};
  float s = std::sin(angle * 0.5f);
  return {a.x * s, a.y * s, a.z * s, std::cos(angle * 0.5f)};
}

float QuatDot(const Quat& a, const Quat& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

Quat QuatNormalize(const Quat& q) {
  float l2 = QuatDot(q, q);
  if (l2 < 1e-12f) return {};
  float inv = 1.0f / std::sqrt(l2);
  return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

Vec3 QuatRotate(const Quat& q, Vec3 v) {
  Vec3 u{q.x, q.y, q.z};
  Vec3 t = u.cross(v) * 2.0f;
  return v + t * q.w + u.cross(t);
}

Vec3 QuatRotateInv(const Quat& q, Vec3 v) { return QuatRotate(QuatConj(q), v); }

Quat QuatNlerp(const Quat& a, const Quat& b, float t) {
  // sign-fix onto the shortest arc, then straight lerp + renormalize. nlerp is
  // the right primitive for N-way blends (see AnimSampleAndBlend); chained
  // slerps are order-dependent and would make the blend non-commutative.
  Quat q = b;
  if (QuatDot(a, b) < 0) q = {-b.x, -b.y, -b.z, -b.w};
  float s = 1.0f - t;
  return QuatNormalize({a.x * s + q.x * t, a.y * s + q.y * t,
                        a.z * s + q.z * t, a.w * s + q.w * t});
}

Quat QuatSlerp(const Quat& a, const Quat& b, float t) {
  float d = QuatDot(a, b);
  Quat q = b;
  if (d < 0) {
    q = {-b.x, -b.y, -b.z, -b.w};
    d = -d;
  }
  if (d > 0.9995f) return QuatNlerp(a, q, t);
  float theta = std::acos(std::clamp(d, -1.0f, 1.0f));
  float sinT = std::sin(theta);
  float wa = std::sin((1.0f - t) * theta) / sinT;
  float wb = std::sin(t * theta) / sinT;
  return QuatNormalize({a.x * wa + q.x * wb, a.y * wa + q.y * wb,
                        a.z * wa + q.z * wb, a.w * wa + q.w * wb});
}

Quat QuatFromTo(Vec3 from, Vec3 to) {
  Vec3 f = from.normalized(), t = to.normalized();
  if (f.len() < 1e-6f || t.len() < 1e-6f) return {};
  float d = std::clamp(f.dot(t), -1.0f, 1.0f);
  if (d > 0.999999f) return {};
  if (d < -0.999999f) {
    // antiparallel: any perpendicular axis works; pick the most stable one
    Vec3 ax = std::fabs(f.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    return QuatAxisAngle(f.cross(ax).normalized(), 3.14159265f);
  }
  Vec3 c = f.cross(t);
  return QuatNormalize({c.x, c.y, c.z, 1.0f + d});
}

// ---- easing -----------------------------------------------------------------

Ease ParseEase(const std::string& s) {
  if (s == "instant") return Ease::Instant;
  if (s == "quadIn") return Ease::QuadIn;
  if (s == "quadOut") return Ease::QuadOut;
  if (s == "quadInOut") return Ease::QuadInOut;
  if (s == "cubicIn") return Ease::CubicIn;
  if (s == "cubicOut") return Ease::CubicOut;
  if (s == "cubicInOut") return Ease::CubicInOut;
  return Ease::Linear;
}

float ApplyEase(Ease e, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  switch (e) {
    case Ease::Instant: return 0.0f;
    case Ease::QuadIn: return t * t;
    case Ease::QuadOut: return 1.0f - (1.0f - t) * (1.0f - t);
    case Ease::QuadInOut:
      return t < 0.5f ? 2 * t * t : 1.0f - 2 * (1.0f - t) * (1.0f - t);
    case Ease::CubicIn: return t * t * t;
    case Ease::CubicOut: {
      float u = 1.0f - t;
      return 1.0f - u * u * u;
    }
    case Ease::CubicInOut: {
      if (t < 0.5f) return 4 * t * t * t;
      float u = -2 * t + 2;
      return 1.0f - u * u * u * 0.5f;
    }
    default: return t;
  }
}

// ---- skeleton lookups -------------------------------------------------------

int AnimSkeleton::FindPart(const std::string& name) const {
  for (size_t i = 0; i < parts.size(); i++)
    if (parts[i].name == name) return (int)i;
  return -1;
}

int AnimSkeleton::FindClip(const std::string& name) const {
  for (size_t i = 0; i < clips.size(); i++)
    if (clips[i].name == name) return (int)i;
  return -1;
}

bool AnimSkeleton::ParentsFirst() const {
  for (size_t i = 0; i < parts.size(); i++)
    if (parts[i].parent >= (int)i) return false;
  return true;
}

// ---- stage 1: sample --------------------------------------------------------

namespace {

// Sample one track at `tMs`. Keys are sorted; holds the first/last key outside
// the range. Returns false when the track has no keys at all.
bool SampleTrack(const AnimTrack& tr, float tMs, Transform& out,
                 bool& gotRot, bool& gotPos) {
  if (tr.keys.empty()) return false;
  gotRot = gotPos = false;
  if (tMs <= (float)tr.keys.front().tMs) {
    out.rot = tr.keys.front().rot;
    out.pos = tr.keys.front().pos;
    gotRot = tr.keys.front().hasRot;
    gotPos = tr.keys.front().hasPos;
    return true;
  }
  if (tMs >= (float)tr.keys.back().tMs) {
    out.rot = tr.keys.back().rot;
    out.pos = tr.keys.back().pos;
    gotRot = tr.keys.back().hasRot;
    gotPos = tr.keys.back().hasPos;
    return true;
  }
  size_t k = 0;
  while (k + 1 < tr.keys.size() && (float)tr.keys[k + 1].tMs <= tMs) k++;
  const AnimKey& a = tr.keys[k];
  const AnimKey& b = tr.keys[std::min(k + 1, tr.keys.size() - 1)];
  float span = (float)(b.tMs - a.tMs);
  float u = span > 0 ? (tMs - (float)a.tMs) / span : 0.0f;
  // easing is a property of the OUTGOING key (Aseprite/ozz convention)
  u = ApplyEase(a.ease, u);
  out.rot = QuatNlerp(a.rot, b.rot, u);
  out.pos = a.pos + (b.pos - a.pos) * u;
  gotRot = a.hasRot || b.hasRot;
  gotPos = a.hasPos || b.hasPos;
  return true;
}

// Per-clip blend-in/out envelope: ramps up over blendInMs, and for
// non-looping clips ramps back down over the final blendOutMs.
float ClipFade(const AnimClip& c, float tMs, bool stopping, float stopFade) {
  float f = 1.0f;
  if (c.blendInMs > 0) f = std::min(f, tMs / (float)c.blendInMs);
  if (!c.loop && c.blendOutMs > 0 && c.durationMs > 0)
    f = std::min(f, ((float)c.durationMs - tMs) / (float)c.blendOutMs);
  if (stopping) f = std::min(f, stopFade);
  return std::clamp(f, 0.0f, 1.0f);
}

}  // namespace

// ---- dismemberment state selection ------------------------------------------

int AnimSelectState(const AnimSkeleton& sk, const AnimState& st) {
  auto dead = [&](int p) {
    return p >= 0 && p < (int)st.partAlive.size() && !st.partAlive[p];
  };
  // A chain is "lost" the moment ANY of its parts is severed — the same test
  // UpdateGait uses to stop scheduling a leg's steps, so a rule keyed on
  // minChainsLost flips exactly when the gait stops using that leg.
  int chainsLost = 0;
  for (const IkChain& ch : sk.chains) {
    bool lost = false;
    for (int p : ch.parts) lost |= dead(p);
    chainsLost += lost ? 1 : 0;
  }
  for (size_t r = 0; r < sk.states.size(); r++) {
    const AnimStateRule& rule = sk.states[r];
    // an empty predicate would shadow every rule after it — never match it
    if (rule.missingAll.empty() && rule.missingAnyOf.empty() &&
        rule.minChainsLost <= 0)
      continue;
    bool match = true;
    for (int p : rule.missingAll) match &= dead(p);
    if (!rule.missingAnyOf.empty()) {
      bool any = false;
      for (int p : rule.missingAnyOf) any |= dead(p);
      match &= any;
    }
    match &= chainsLost >= rule.minChainsLost;
    if (match) return (int)r;
  }
  return -1;
}

void AnimSampleAndBlend(const AnimSkeleton& sk, AnimState& st, float dt) {
  const size_t n = sk.parts.size();
  st.local.resize(n);
  // rest pose is the baseline for everything below
  for (size_t i = 0; i < n; i++) st.local[i] = sk.parts[i].rest;
  if (st.clips.empty()) return;

  // per-part accumulators for the OVERRIDE pass
  static thread_local std::vector<Quat> acc;
  static thread_local std::vector<Vec3> accPos;
  static thread_local std::vector<float> accW;
  acc.assign(n, Quat{});
  accPos.assign(n, Vec3{});
  accW.assign(n, 0.0f);
  static thread_local std::vector<uint8_t> touched;
  touched.assign(n, 0);

  struct Additive { int part; Quat dq; float w; };
  static thread_local std::vector<Additive> additives;
  additives.clear();

  for (size_t ci = 0; ci < st.clips.size();) {
    ClipInstance& inst = st.clips[ci];
    if (inst.clip < 0 || inst.clip >= (int)sk.clips.size()) {
      st.clips.erase(st.clips.begin() + ci);
      continue;
    }
    const AnimClip& c = sk.clips[inst.clip];

    inst.timeMs += dt * 1000.0f;
    if (c.loop && c.durationMs > 0) {
      while (inst.timeMs >= (float)c.durationMs) inst.timeMs -= (float)c.durationMs;
    } else if (c.durationMs > 0 && inst.timeMs >= (float)c.durationMs) {
      inst.timeMs = (float)c.durationMs;
      inst.stopping = true;
    }
    if (inst.stopping) inst.fade = std::max(0.0f, inst.fade - dt * 6.0f);
    else inst.fade = 1.0f;
    float w = inst.weight * ClipFade(c, inst.timeMs, inst.stopping, inst.fade);
    if (inst.stopping && w <= 0.0f) {
      st.clips.erase(st.clips.begin() + ci);
      continue;
    }
    ci++;
    if (w <= 0.0f) continue;

    for (const AnimTrack& tr : c.tracks) {
      if (tr.part < 0 || tr.part >= (int)n) continue;
      // A non-empty mask is an allowlist: parts beyond its length are masked
      // OFF, not let through. The loader always sizes the mask to the part
      // count, so this only matters for future producers of short masks —
      // but "short mask unmasks high-index parts" is too surprising to keep.
      if (!c.mask.empty() &&
          (tr.part >= (int)c.mask.size() || !c.mask[tr.part])) continue;
      Transform sampled{};
      bool gotRot = false, gotPos = false;
      if (!SampleTrack(tr, inst.timeMs, sampled, gotRot, gotPos)) continue;

      if (c.mode == ClipMode::Additive) {
        // The additive DELTA is measured against the clip's OWN frame 0, so an
        // additive clip authored on top of any base pose means "the change this
        // clip makes", not "this absolute pose".
        Quat ref = tr.keys.front().rot;
        Quat dq = QuatMul(QuatConj(ref), sampled.rot);
        additives.push_back({tr.part, dq, w});
        continue;
      }

      // OVERRIDE: accumulator-aligned nlerp. Align each incoming quaternion
      // against the RUNNING SUM (not against q0) — aligning against a fixed
      // reference can still let two later quats cancel each other out.
      int p = tr.part;
      if (gotRot) {
        Quat q = sampled.rot;
        if (accW[p] > 0 && QuatDot(acc[p], q) < 0) q = {-q.x, -q.y, -q.z, -q.w};
        acc[p].x += q.x * w;
        acc[p].y += q.y * w;
        acc[p].z += q.z * w;
        acc[p].w += q.w * w;
      }
      if (gotPos) accPos[p] += sampled.pos * w;
      accW[p] += w;
      touched[p] = 1;
    }
  }

  // ---- stage 2: normalize ----
  for (size_t i = 0; i < n; i++) {
    if (!touched[i] || accW[i] < kBlendEpsilon) continue;  // rest-pose fallback
    float mag = std::sqrt(QuatDot(acc[i], acc[i]));
    if (mag < 1e-6f) continue;                             // degenerate: rest
    st.local[i].rot = QuatNormalize(acc[i]);
    st.local[i].pos = sk.parts[i].rest.pos + accPos[i] * (1.0f / accW[i]);
  }

  // ---- stage 3: additive, AFTER normalization ----
  // q_out = q_base * nlerp(identity, dq, w). Applying additives before the
  // normalize would let the weight-division scale the delta away.
  for (const Additive& a : additives) {
    Quat scaled = QuatNlerp(Quat{}, a.dq, std::clamp(a.w, 0.0f, 1.0f));
    st.local[a.part].rot = QuatNormalize(QuatMul(st.local[a.part].rot, scaled));
  }
}

// ---- stage 4: flatten -------------------------------------------------------

void AnimFlatten(const AnimSkeleton& sk, AnimState& st) {
  const size_t n = sk.parts.size();
  st.model.resize(n);
  // ONE linear pass: parts are stored parent-before-child, so a parent's model
  // transform is always final by the time a child reads it. No recursion, no
  // sort, no visited set.
  for (size_t i = 0; i < n; i++) {
    const AnimPart& p = sk.parts[i];
    if (p.parent < 0) {
      st.model[i] = st.local[i];
    } else {
      const Transform& par = st.model[p.parent];
      st.model[i].rot = QuatNormalize(QuatMul(par.rot, st.local[i].rot));
      st.model[i].pos = par.pos + QuatRotate(par.rot, st.local[i].pos);
    }
  }
}

// ---- stage 5: two-bone IK ---------------------------------------------------

void AnimSolveTwoBone(const AnimSkeleton& sk, AnimState& st,
                      const IkChain& chain, Vec3 targetModel, float weight) {
  if (weight <= 0.0f) return;
  if (chain.parts.size() < 2 || chain.effector < 0) return;
  int i0 = chain.parts[0];                       // upper bone
  int i1 = chain.parts[1];                       // lower bone
  int ie = chain.effector;
  const size_t n = sk.parts.size();
  if (i0 < 0 || i1 < 0 || ie < 0 || (size_t)i0 >= n || (size_t)i1 >= n ||
      (size_t)ie >= n)
    return;
  if (!st.partAlive.empty() &&
      (!st.partAlive[i0] || !st.partAlive[i1]))
    return;                                      // limb lost: chain goes silent

  Vec3 root = st.model[i0].pos;
  Vec3 joint = st.model[i1].pos;
  Vec3 tip = st.model[ie].pos;
  // Effector may be the lower bone itself; then the tip is its own end, found
  // by extending along the bone direction (rest length).
  if (ie == i1) tip = joint + QuatRotate(st.model[i1].rot, sk.parts[i1].rest.pos);

  float L1 = (joint - root).len();
  float L2 = (tip - joint).len();
  if (L1 < 1e-4f || L2 < 1e-4f) return;

  Vec3 toTarget = targetModel - root;
  float d = toTarget.len();
  if (d < 1e-4f) return;
  // Clamp reach to the annulus the chain can physically hit. Without the
  // epsilons the acos arguments land exactly on +-1 and the cross products
  // below degenerate.
  const float kEps = 1e-3f;
  float dMin = std::fabs(L1 - L2) + kEps;
  float dMax = L1 + L2 - kEps;
  float dc = std::clamp(d, dMin, dMax);

  // Interior angles by the law of cosines. acos inputs MUST be clamped: even
  // after the reach clamp, float error can push them a few ulps outside [-1,1]
  // and acos returns NaN, which then poisons the whole pose.
  float cosInterior =
      std::clamp((L1 * L1 + L2 * L2 - dc * dc) / (2 * L1 * L2), -1.0f, 1.0f);
  float interior = std::acos(cosInterior);
  float angle2 = 3.14159265f - interior;         // bend at the middle joint

  // Root angle via the atan2 form (numerically stable across the whole range,
  // unlike acos((L1^2 + d^2 - L2^2)/(2*L1*d)) which loses precision near full
  // extension).
  float adj = L1 + L2 * std::cos(angle2);
  float opp = L2 * std::sin(angle2);
  float tx = dc, ty = 0.0f;                      // target in the 2D bend plane
  float angle1 = std::atan2(ty * adj - tx * opp, tx * adj + ty * opp);

  Vec3 dirTarget = toTarget.normalized();
  // Bend plane from the EXPLICIT pole vector — never from the current pose,
  // which flips unpredictably when the chain straightens.
  Vec3 pole = chain.pole.normalized();
  if (pole.len() < 1e-6f) pole = Vec3{0, 0, 1};
  Vec3 bendAxis = dirTarget.cross(pole);
  if (bendAxis.len() < 1e-4f) {
    // pole parallel to the target direction: substitute a fixed forward vector
    Vec3 alt = std::fabs(dirTarget.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    bendAxis = dirTarget.cross(alt);
  }
  bendAxis = bendAxis.normalized();
  if (bendAxis.len() < 1e-4f) return;

  // Current upper-bone direction -> aimed at the target, then rotated by
  // angle1 about the bend axis to open the elbow/knee.
  Vec3 curDir = (joint - root).normalized();
  Quat aim = QuatFromTo(curDir, dirTarget);
  Quat bend = QuatAxisAngle(bendAxis, angle1);
  Quat upperDelta = QuatMul(bend, aim);

  Quat newUpper = QuatNormalize(QuatMul(upperDelta, st.model[i0].rot));
  Vec3 newJoint = root + QuatRotate(upperDelta, joint - root);

  // Lower bone: aim from the new joint position at the target.
  Vec3 curLower = (tip - joint).normalized();
  Vec3 wantLower = (targetModel - newJoint).normalized();
  Quat lowerDelta = QuatFromTo(QuatRotate(upperDelta, curLower), wantLower);
  Quat newLower =
      QuatNormalize(QuatMul(lowerDelta, QuatMul(upperDelta, st.model[i1].rot)));
  Vec3 newLowerPos = newJoint;

  // Blend weight lets the solver fade in/out (limb loss, ragdoll).
  float w = std::clamp(weight, 0.0f, 1.0f);
  st.model[i0].rot = QuatSlerp(st.model[i0].rot, newUpper, w);
  st.model[i1].rot = QuatSlerp(st.model[i1].rot, newLower, w);
  st.model[i1].pos = st.model[i1].pos + (newLowerPos - joint) * w;

  // Descendants of the lower bone (e.g. a foot) follow rigidly: re-flatten the
  // tail of the chain rather than leaving them at the pre-IK pose.
  for (size_t k = 0; k < n; k++) {
    const AnimPart& p = sk.parts[k];
    if (p.parent != i1) continue;
    st.model[k].rot = QuatNormalize(QuatMul(st.model[i1].rot, st.local[k].rot));
    st.model[k].pos =
        st.model[i1].pos + QuatRotate(st.model[i1].rot, st.local[k].pos);
  }
}

// ---- springs ----------------------------------------------------------------

void AnimSpringStep(const SpringDef& def, SpringState& s, Vec3 goal, float dt) {
  // Holden's closed-form critically-damped spring: exact for any dt, so it
  // never explodes at a frame spike the way an explicit-Euler spring does.
  float halflife = std::max(def.halflife, 1e-3f);
  float y = (2.77258872f / halflife) * 0.5f;
  float e = std::exp(-y * dt);
  float* xs[3] = {&s.x.x, &s.x.y, &s.x.z};
  float* vs[3] = {&s.v.x, &s.v.y, &s.v.z};
  const float gs[3] = {goal.x, goal.y, goal.z};
  for (int a = 0; a < 3; a++) {
    float j0 = *xs[a] - gs[a];
    float j1 = *vs[a] + j0 * y;
    *xs[a] = e * (j0 + j1 * dt) + gs[a];
    *vs[a] = e * (*vs[a] - j1 * y * dt);
    *xs[a] = std::clamp(*xs[a], -def.maxAngle, def.maxAngle);
  }
}

// ---- flipbooks --------------------------------------------------------------

int AnimFlipbookFrame(const Flipbook& fb, int32_t elapsedMs) {
  if (fb.frames.empty()) return -1;
  int32_t total = 0;
  for (const FlipbookFrame& f : fb.frames) total += std::max(1, f.durationMs);
  if (total <= 0) return -1;
  int32_t t = elapsedMs;
  if (fb.loop) {
    t %= total;
    if (t < 0) t += total;
  } else if (t >= total) {
    return (int)fb.frames.size() - 1;
  }
  int32_t acc = 0;
  for (size_t i = 0; i < fb.frames.size(); i++) {
    acc += std::max(1, fb.frames[i].durationMs);
    if (t < acc) return (int)i;
  }
  return (int)fb.frames.size() - 1;
}
