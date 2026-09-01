#include "game/anim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

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

Quat QuatFromEulerDeg(Vec3 deg) {
  const float k = 3.14159265358979f / 180.0f;
  // X then Y then Z, composed left-to-right so the LAST rotation is applied in
  // the frame the first two produced. See the note in anim.h: the order is a
  // shared convention, not a local choice.
  Quat q = QuatAxisAngle({1, 0, 0}, deg.x * k);
  q = QuatMul(QuatAxisAngle({0, 1, 0}, deg.y * k), q);
  q = QuatMul(QuatAxisAngle({0, 0, 1}, deg.z * k), q);
  return QuatNormalize(q);
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
//
// Rot and pos are sampled INDEPENDENTLY over the fused key list: a key that
// carries only a position must not act as a rotation key, because its rot
// field is the default identity and every mid-segment sample would get dragged
// toward it. First shipped symptom: a crawl clip whose torso track had rot
// keys at 0/450/900 ms and pos keys at 0/225/450/675/900 rendered an authored
// 75-degree pitch as ~25 degrees — the pos-only keys at 225/675 pulled each
// rot sample most of the way back upright. Each channel interpolates between
// its own neighbouring keys, with the ease of that channel's outgoing key.
bool SampleTrack(const AnimTrack& tr, float tMs, Transform& out,
                 bool& gotRot, bool& gotPos) {
  if (tr.keys.empty()) return false;
  gotRot = gotPos = false;
  auto channel = [&](bool AnimKey::*has, auto apply) {
    int prev = -1, next = -1;
    for (size_t k = 0; k < tr.keys.size(); k++) {
      if (!(tr.keys[k].*has)) continue;
      if ((float)tr.keys[k].tMs <= tMs) prev = (int)k;
      else { next = (int)k; break; }
    }
    if (prev < 0 && next < 0) return false;      // channel absent entirely
    if (prev < 0) { apply(tr.keys[next], tr.keys[next], 0.0f); return true; }
    if (next < 0) { apply(tr.keys[prev], tr.keys[prev], 0.0f); return true; }
    const AnimKey& a = tr.keys[prev];
    const AnimKey& b = tr.keys[next];
    float span = (float)(b.tMs - a.tMs);
    float u = span > 0 ? (tMs - (float)a.tMs) / span : 0.0f;
    // easing is a property of the OUTGOING key (Aseprite/ozz convention)
    apply(a, b, ApplyEase(a.ease, u));
    return true;
  };
  gotRot = channel(&AnimKey::hasRot,
                   [&](const AnimKey& a, const AnimKey& b, float u) {
                     out.rot = QuatNlerp(a.rot, b.rot, u);
                   });
  gotPos = channel(&AnimKey::hasPos,
                   [&](const AnimKey& a, const AnimKey& b, float u) {
                     out.pos = a.pos + (b.pos - a.pos) * u;
                   });
  return gotRot || gotPos;
}

// Per-clip blend-in/out envelope: ramps up over blendInMs, and for
// non-looping clips ramps back down over the final blendOutMs.
//
// `ageMs` is time since the instance STARTED; `tMs` is the playhead. They are
// the same thing for a one-shot and very different for a loop, and the blend-in
// wants the former. Driving it from the playhead re-ran the fade on EVERY loop,
// because tMs wraps to 0 at the end of each cycle: a 308 ms walk with a 180 ms
// blend-in spent 58% of every cycle below full weight, forever, and the clip
// never once reached the amplitude it was authored at. A blend-in is a
// transition into a clip, so it must happen once.
float ClipFade(const AnimClip& c, float tMs, float ageMs, bool stopping,
               float stopFade) {
  float f = 1.0f;
  if (c.blendInMs > 0) f = std::min(f, ageMs / (float)c.blendInMs);
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

  // Per-part accumulators for the OVERRIDE pass.
  //
  // acc starts at ZERO, not at Quat{} — the default-constructed Quat is the
  // IDENTITY (0,0,0,1), and this is a weighted SUM, not a pose. Seeding it
  // with identity silently adds an unweighted "upright" sample to every part:
  // one clip at weight 1 then normalized to halfway between its key and rest,
  // so an authored 90-degree crawl pitch rendered at 45 and every clip in the
  // game was quietly at half strength. accW stays the true weight total, which
  // is what made the bug invisible in the weights.
  static thread_local std::vector<Quat> acc;
  static thread_local std::vector<Vec3> accPos;
  static thread_local std::vector<float> accW;
  acc.assign(n, Quat{0, 0, 0, 0});
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

    // The playhead carries the instance's rate; the AGE does not — see the
    // note on ClipInstance::rate. Clamped positive so a bad rate can stall a
    // clip but never run it backwards through its own blend.
    inst.timeMs += dt * 1000.0f * std::max(inst.rate, 0.0f);
    inst.ageMs += dt * 1000.0f;
    if (c.loop && c.durationMs > 0) {
      while (inst.timeMs >= (float)c.durationMs) inst.timeMs -= (float)c.durationMs;
    } else if (c.durationMs > 0 && inst.timeMs >= (float)c.durationMs) {
      inst.timeMs = (float)c.durationMs;
      inst.stopping = true;
    }
    if (inst.stopping) inst.fade = std::max(0.0f, inst.fade - dt * 6.0f);
    else inst.fade = 1.0f;
    float w = inst.weight *
              ClipFade(c, inst.timeMs, inst.ageMs, inst.stopping, inst.fade);
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
        // The additive DELTA is measured against the part's REST pose, not
        // against the clip's own frame 0.
        //
        // Frame 0 is the wrong reference for any CYCLIC clip, which is most of
        // them. A walk's arm track is authored as +14 -> -14 -> +14 degrees: a
        // swing that ALTERNATES about rest, which is what an arm swing IS.
        // Referencing frame 0 subtracts that +14 from every sample, so the
        // delta becomes 0 -> -28 -> 0 — a one-sided motion that leaves rest,
        // goes 28 degrees to ONE side, and comes back. Both arms then sit
        // permanently off to the same side of their rest pose and merely
        // twitch, which is the "sticks their arms out like a zombie" look:
        // the pose never crosses rest, so the arms never alternate.
        //
        // Rest is also the only reference that composes. An additive layer
        // means "the change this clip makes to the rest pose", so two additive
        // clips on one part sum the way an animator expects, and a clip whose
        // frame 0 is already displaced (jump, land, headless) contributes that
        // displacement instead of silently cancelling it.
        //
        // The cost of getting this right is that an additive clip must be
        // AUTHORED about rest. That is the natural way to key one anyway, and
        // every clip in mina.json already is.
        Quat ref = sk.parts[tr.part].rest.rot;
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

// ---- stage 3.5: the torso serves the swing ----------------------------------

void AnimApplySpineTwist(const AnimSkeleton& sk, AnimState& st, float yawRight,
                         float pitchUp, int rootLimb) {
  if (std::fabs(yawRight) < 1e-4f && std::fabs(pitchUp) < 1e-4f) return;
  int nSpine = 0;
  for (size_t i = 0; i < sk.parts.size(); i++)
    if (sk.parts[i].tag == "spine" && (int)i != rootLimb) nSpine++;
  if (nSpine == 0) return;
  // Model-space signs. Pitch is the head-look's verified one: a positive
  // rotation about +X pitches the nose DOWN (avatar.cpp quotes the
  // geometry.py check), so "chest up" is the negative angle. Yaw is derived
  // the same way: a positive rotation about +Y takes +Z (the rig's forward)
  // to +X, and the basis right is fwd x up = -X, so "toward the right" is
  // also the negative angle. If a rig ever reads mirrored, the A/B is
  // melee.torsoShare = 0, not a sign hunt across callers — both live here.
  const float yawPer = -yawRight / (float)nSpine;
  const float pitchPer = -pitchUp / (float)nSpine;
  const Quat d = QuatMul(QuatAxisAngle({0, 1, 0}, yawPer),
                         QuatAxisAngle({1, 0, 0}, pitchPer));
  for (size_t i = 0; i < sk.parts.size(); i++) {
    if (sk.parts[i].tag != "spine" || (int)i == rootLimb) continue;
    if (i < st.partAlive.size() && !st.partAlive[i]) continue;
    st.local[i].rot = QuatNormalize(QuatMul(st.local[i].rot, d));
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
  // ORTHOGONALIZE THE POLE AGAINST THE TARGET (Gram-Schmidt) rather than
  // crossing with it raw.
  //
  // The pole says which way the joint should BULGE. An arm's pole is straight
  // back ([0,0,-1] on these rigs, because a human elbow points behind), so
  // reaching straight FORWARD makes the pole exactly antiparallel to the
  // target and `dirTarget.cross(pole)` collapses to zero — the degenerate case.
  // The old fallback then bent the joint about world-up, i.e. sideways, which
  // is how "point the hand forward" produced an elbow winging out to the side
  // while pointing BACKWARD (equally degenerate, opposite sign) still looked
  // plausible. Worse, it SNAPPED: the fallback switched in discontinuously as
  // the arm swung through the parallel direction.
  //
  // Removing the target-parallel component keeps only the part of the pole
  // that can actually define a plane, so the bend direction stays continuous
  // as the limb sweeps through the pole axis instead of flipping at it.
  Vec3 polePerp = pole - dirTarget * dirTarget.dot(pole);
  if (polePerp.len() < 1e-4f) {
    // Genuinely no information left (pole exactly along the target). Any plane
    // is as good as another here, but it must be CHOSEN CONSISTENTLY or the
    // joint spins as the target crosses this axis: prefer the rig's up, then
    // its side, whichever is less parallel to the target.
    Vec3 alt = std::fabs(dirTarget.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    polePerp = alt - dirTarget * dirTarget.dot(alt);
  }
  if (polePerp.len() < 1e-6f) return;
  polePerp = polePerp.normalized();
  // NOTE THE ORDER: polePerp x dirTarget, not dirTarget x polePerp.
  //
  // `angle1` above comes out NEGATIVE (ty is 0 and tx is positive, so the
  // atan2 numerator is -tx*opp), which means rotating the upper bone by it
  // about `dirTarget x polePerp` swings the middle joint AWAY from the pole.
  // That inverted the whole convention: with the arm pole authored straight
  // back the elbow bulged forward, and — measured the same way — the leg pole
  // authored forward put the KNEE BACKWARD at every target, which is
  // anatomically backwards for both. Crossing the other way makes the joint
  // bulge TOWARD the pole, which is what "pole vector" means everywhere else
  // and what the authored [0,0,-1] / [0,0,1] values plainly intend.
  Vec3 bendAxis = polePerp.cross(dirTarget);
  if (bendAxis.len() < 1e-4f) return;
  bendAxis = bendAxis.normalized();

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

// ---- stage 6: pose-space joint limits ---------------------------------------

namespace {

// Swing-twist decomposition, clamp the twist, recompose. `axis` must be unit.
//
// The twist is the part of `q` about `axis`; the swing is everything else, and
// it is left ALONE. That split is what makes a one-axis limit useful on a ball
// joint: a hip may be told "you pitch between -10 and +80 degrees" without also
// being told how far it may abduct, which is a separate anatomical fact and a
// separate number.
Quat ClampTwist(const Quat& q, Vec3 axis, float lo, float hi) {
  const Vec3 v{q.x, q.y, q.z};
  const float d = v.dot(axis);
  Quat twist{axis.x * d, axis.y * d, axis.z * d, q.w};
  const float mag =
      std::sqrt(twist.x * twist.x + twist.y * twist.y + twist.z * twist.z +
                twist.w * twist.w);
  // Degenerate: `q` is a half turn about an axis perpendicular to ours, so the
  // projection carries no information about the twist. There is no meaningful
  // angle to clamp, and inventing one would snap the joint.
  if (mag < 1e-5f) return q;
  twist.x /= mag; twist.y /= mag; twist.z /= mag; twist.w /= mag;
  // Signed angle about `axis`. The normalized twist's vector part lies along
  // +-axis by construction, so its dot with the axis IS the signed sine term.
  // atan2 rather than 2*acos(w) because acos loses the sign and is ill-
  // conditioned near zero, which is where a joint sits most of the time.
  const float s = Vec3{twist.x, twist.y, twist.z}.dot(axis);
  float ang = 2.0f * std::atan2(s, twist.w);
  // Wrap into (-pi, pi] before clamping: an angle that has wrapped past a half
  // turn is nearer the opposite stop, and clamping the unwrapped value would
  // drive the joint the long way round.
  while (ang > 3.14159265f) ang -= 6.2831853f;
  while (ang <= -3.14159265f) ang += 6.2831853f;
  const float clamped = std::clamp(ang, lo, hi);
  if (std::fabs(clamped - ang) < 1e-6f) return q;  // already legal
  // swing = q * conj(twist); rebuild with the clamped twist.
  const Quat swing = QuatMul(q, QuatConj(twist));
  return QuatNormalize(QuatMul(swing, QuatAxisAngle(axis, clamped)));
}

// The signed rotation of `q` about `axis`, ignoring everything else. Same
// projection ClampTwist uses; split out because the hinge below wants the ANGLE
// and then throws the rest of `q` away rather than putting it back.
//
// THE BODY OF THE PUBLIC AnimHingeAngleAbout (below the namespace), because
// `Mob::ApplyWeaponArm` asks the same question about the same joint one stage
// earlier and the two used to be independent copies that disagreed about the
// WRAP — see anim.h for what that cost.
bool TwistAngleAboutImpl(const Quat& q, Vec3 axis, float* out) {
  const Vec3 v{q.x, q.y, q.z};
  const float d = v.dot(axis);
  Quat t{axis.x * d, axis.y * d, axis.z * d, q.w};
  const float mag =
      std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z + t.w * t.w);
  if (mag < 1e-5f) return false;
  t.x /= mag; t.y /= mag; t.z /= mag; t.w /= mag;
  float ang = 2.0f * std::atan2(Vec3{t.x, t.y, t.z}.dot(axis), t.w);
  while (ang > 3.14159265f) ang -= 6.2831853f;
  while (ang <= -3.14159265f) ang += 6.2831853f;
  *out = ang;
  return true;
}

// ONE degree of freedom: the result is a pure rotation about `axis` by the
// clamped angle, with the off-axis swing discarded (see AnimPart::poseHinge).
Quat ClampHinge(const Quat& q, Vec3 axis, float lo, float hi) {
  float ang = 0;
  // No recoverable angle (q is a half turn about something perpendicular).
  // Snapping such a pose to the nearest stop would be inventing a number, so
  // the nearest legal HINGE is the one the joint is already closest to, and
  // with no angle to read that is the rest end of the range.
  if (!AnimHingeAngleAbout(q, axis, &ang)) ang = 0.0f;
  return QuatAxisAngle(axis, std::clamp(ang, lo, hi));
}

// Clamp the direction `d` (unit) into the intersection of up to two half-spaces
// `d . n[k] <= s[k]`, returning the NEAREST legal direction. The normals are
// mutually perpendicular (the loader enforces it), so completing them to an
// orthonormal frame turns the projection into three independent components and
// the unit-length identity supplies the third.
Vec3 ClampDirHalfSpaces(Vec3 d, const Vec3* n, const float* s, int count) {
  if (count <= 0) return d;
  const Vec3 n0 = n[0];
  // A second axis is needed to complete the frame even when only one plane is
  // authored; any perpendicular one will do because its component is preserved.
  Vec3 n1 = count >= 2 ? n[1]
                       : (std::fabs(n0.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0});
  n1 = (n1 - n0 * n0.dot(n1));
  if (n1.len() < 1e-5f) return d;
  n1 = n1.normalized();
  const Vec3 n2 = n0.cross(n1);
  const float c0 = d.dot(n0), c1 = d.dot(n1), c2 = d.dot(n2);
  const float w0 = std::min(c0, s[0]);
  const float w1 = count >= 2 ? std::min(c1, s[1]) : c1;
  if (w0 == c0 && w1 == c1) return d;   // already legal, bit-for-bit unchanged
  float rem = 1.0f - w0 * w0 - w1 * w1;
  // Both stops pinned so hard that no unit vector satisfies them: the authored
  // limits leave an empty set in this corner. Give back the closest thing that
  // exists rather than a NaN.
  if (rem <= 0.0f) {
    const Vec3 v = n0 * w0 + n1 * w1;
    return v.len() > 1e-5f ? v.normalized() : d;
  }
  // The third component keeps its sign. At c2 == 0 the two answers are exactly
  // equidistant, so the nearest-point projection is genuinely two-valued there;
  // taking the sign from the INPUT makes the choice vary continuously with the
  // pose everywhere except that measure-zero corner (bone aimed exactly into
  // the intersection of both forbidden regions).
  const float s2 = std::sqrt(rem) * (c2 < 0.0f ? -1.0f : 1.0f);
  return (n0 * w0 + n1 * w1 + n2 * s2).normalized();
}

// Swing-twist clamp for a ball joint: bound where the bone POINTS, then bound
// how far it rolls about itself.
//
// The decomposition is exact rather than approximate. `swing` is the minimal
// rotation carrying the rest bone direction onto the current one, so
// conj(swing) * q leaves the bone fixed — and a rotation that fixes an axis IS
// a rotation about it. Re-composing with the clamped swing therefore changes
// only the reach, and clamping the twist changes only the roll.
Quat ClampBall(const Quat& q, const PoseBallLimit& lim) {
  const Vec3 bone = lim.bone;
  const Vec3 dOld = QuatRotate(q, bone);
  const Vec3 dNew = ClampDirHalfSpaces(dOld, lim.reachNormal, lim.reachSin,
                                       lim.reachCount);
  const Quat swing = QuatFromTo(bone, dOld);
  Quat twist = QuatNormalize(QuatMul(QuatConj(swing), q));
  if (lim.hasTwist) twist = ClampTwist(twist, bone, lim.twistMin, lim.twistMax);
  const Quat swingNew =
      (dNew.x == dOld.x && dNew.y == dOld.y && dNew.z == dOld.z)
          ? swing
          : QuatFromTo(bone, dNew);
  return QuatNormalize(QuatMul(swingNew, twist));
}

}  // namespace

// See anim.h: the ONE place a hinge's bend angle is measured.
bool AnimHingeAngleAbout(const Quat& q, const Vec3& axis, float* out) {
  return TwistAngleAboutImpl(q, axis, out);
}

void AnimClampPoseLimits(const AnimSkeleton& sk, AnimState& st,
                         const PoseAxisOverride* overrides, int overrideCount) {
  const size_t n = sk.parts.size();
  if (st.model.size() < n) return;
  bool any = false;
  for (size_t i = 0; i < n; i++)
    any |= sk.parts[i].hasPoseLimit || sk.parts[i].poseBall.has;
  if (!any) return;  // the common case: nothing authored, nothing to pay for

  // SNAPSHOT THE POSE AS THE IK LEFT IT, expressed parent-relative.
  //
  // st.local[] cannot be used here: the IK writes st.model[] directly and never
  // updates local, so local still holds the pre-IK pose. Re-deriving each
  // part's local transform from the model pair is what lets a clamp on the hip
  // carry the knee and the foot with it while KEEPING the knee's own solved
  // bend — recomposing from local would throw the IK away.
  static thread_local std::vector<Transform> eff;
  eff.resize(n);
  for (size_t i = 0; i < n; i++) {
    const int par = sk.parts[i].parent;
    if (par < 0 || (size_t)par >= n) {
      eff[i] = st.model[i];
      continue;
    }
    const Quat inv = QuatConj(st.model[par].rot);
    eff[i].rot = QuatNormalize(QuatMul(inv, st.model[i].rot));
    eff[i].pos = QuatRotate(inv, st.model[i].pos - st.model[par].pos);
  }

  // Parents-first, which sk.ParentsFirst() guarantees: a child recomposes
  // against a parent that has already been clamped, so the correction
  // propagates down the limb in one pass with no second flatten.
  for (size_t i = 0; i < n; i++) {
    const AnimPart& p = sk.parts[i];
    const int par = p.parent;
    if (p.hasPoseLimit || p.poseBall.has) {
      // Measured against REST, not against the parent: "0 degrees" has to
      // mean the pose the art was drawn in, or every rig would need its
      // limits re-derived from wherever its bind pose happens to sit.
      const Quat rest = p.rest.rot;
      Quat delta = QuatMul(QuatConj(rest), eff[i].rot);
      bool moved = false;
      // BALL FIRST, THEN THE AXIS FORM. The ball limit bounds where the bone
      // points and how it rolls; a one-axis clamp on the same part would be a
      // third constraint on quantities the ball has already resolved, and
      // applying it afterwards would silently reopen the reach. No rig authors
      // both, and running the ball last would make "which won" depend on the
      // order rather than on the data.
      if (p.poseBall.has) {
        delta = ClampBall(delta, p.poseBall);
        moved = true;
      } else if (p.hasPoseLimit) {
        Vec3 raw = p.poseAxis;
        // THE PLANE MAY BE STEERED; THE RANGE MAY NOT (anim.h
        // PoseAxisOverride). Blended toward the authored axis rather than
        // switched, so a weapon take-over and a hand-back are ramps.
        for (int k = 0; k < overrideCount; k++) {
          const PoseAxisOverride& ov = overrides[k];
          if (ov.part != (int)i || ov.blend <= 0.0f) continue;
          const float b = ov.blend > 1.0f ? 1.0f : ov.blend;
          const Vec3 a = raw.normalized();
          const Vec3 o = ov.axis.normalized();
          // A DEFENSIVE SIGN FIX, and as of 2026-09-01 a no-op for the one
          // caller that exists. Mob::ApplyWeaponArm now bounds its override to
          // a CONE about this very axis (the shoulder's authored twist range),
          // so `a.dot(o)` is positive by construction there and `as == a`.
          //
          // It stays because this is a public interface and the invariant it
          // protects is not one a future caller can be assumed to know: the
          // two senses name the same PLANE but only one makes the joint's
          // authored [min, max] describe the bend it is actually in, and
          // blending an axis against its own negation passes through zero,
          // where `len > 1e-5f` below silently drops the clamp entirely.
          //
          // It was previously the OTHER half of a scheme in which the caller
          // re-signed its override to force a positive bend; that turned the
          // elbow's one-way `[0, 130]` hinge into a rubber stamp and is the
          // reported "the elbow bends both ways". See ApplyWeaponArm.
          //
          // The flip is only ever applied while blending; at blend 1 the
          // authored axis drops out entirely.
          const Vec3 as = a.dot(o) < 0.0f ? a * -1.0f : a;
          raw = as + (o - as) * b;
          break;
        }
        const float len = raw.len();
        if (len > 1e-5f) {
          const Vec3 axis = raw * (1.0f / len);
          delta = p.poseHinge ? ClampHinge(delta, axis, p.poseMin, p.poseMax)
                              : ClampTwist(delta, axis, p.poseMin, p.poseMax);
          moved = true;
        }
      }
      if (moved) eff[i].rot = QuatNormalize(QuatMul(rest, delta));
    }
    if (par < 0 || (size_t)par >= n) {
      st.model[i] = eff[i];
    } else {
      st.model[i].rot = QuatNormalize(QuatMul(st.model[par].rot, eff[i].rot));
      st.model[i].pos =
          st.model[par].pos + QuatRotate(st.model[par].rot, eff[i].pos);
    }
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
