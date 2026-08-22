#include "game/mob.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "game/rigrender.h"
#include "phys/lattice.h"
#include "sim/rng.h"
#include "sim/tuning.h"

using nlohmann::json;

namespace {

// Quaternion helpers now live in game/anim.h (CPU gameplay floats: legal
// outside the hashed grid domain, see debris.h). Thin aliases keep the
// existing call sites readable.
inline Quat AxisAngle(Vec3 axis, float angle) { return QuatAxisAngle(axis, angle); }
inline Quat Mul(const Quat& a, const Quat& b) { return QuatMul(a, b); }
inline Vec3 Rotate(const Quat& q, Vec3 v) { return QuatRotate(q, v); }
inline Vec3 RotateInv(const Quat& q, Vec3 v) { return QuatRotateInv(q, v); }

// sim/rng.h: a given (mob, limb, tick, index) always produces the same droplet.
// Spray direction is presentation, but it is authored INTO the tick's spawn
// stream, which replays must reproduce.
using rng::Hash3;
using rng::Pcg;
using rng::SignedUnit;

// Event-scoped variance, applied at the spray sites. Entity-scoped variances
// were already resolved into mob.gore at spawn, so these pass through — this is
// what keeps a per-mob trait from being re-rolled every droplet and flattening
// back into noise.
float EventVar(float base, const Variance& v, uint32_t seed, uint32_t tick,
               uint32_t index) {
  return v.scope == Variance::kEvent ? ApplyVariance(base, v, seed, tick, index)
                                     : base;
}
int EventVarI(int base, const Variance& v, uint32_t seed, uint32_t tick,
              uint32_t index) {
  return v.scope == Variance::kEvent ? ApplyVarianceI(base, v, seed, tick, index)
                                     : base;
}

// Builds one ballistic droplet. `micro` picks sub-voxel spray (dies on contact,
// stains, never becomes a voxel) over a real blood voxel.
//
// Velocity is in voxels/sec and converted to the particle system's fixed 24.8
// voxels/TICK here, at the same 30 Hz the debris shatter path uses — the sim is
// fixed-step, so this factor is a constant, not a frame-time.
ParticleSpawn MakeDroplet(Vec3 posVoxel, Vec3 vel, uint32_t material,
                          bool micro, int lifeTicks, int microScale) {
  ParticleSpawn s{};
  s.px = (int32_t)std::lround(posVoxel.x * 256.0f);
  s.py = (int32_t)std::lround(posVoxel.y * 256.0f);
  s.pz = (int32_t)std::lround(posVoxel.z * 256.0f);
  s.vx = (int32_t)std::lround(vel.x * 256.0f / 30.0f);
  s.vy = (int32_t)std::lround(vel.y * 256.0f / 30.0f);
  s.vz = (int32_t)std::lround(vel.z * 256.0f / 30.0f);
  s.payload = material & 0xFFFu;
  s.flags = kPFlagAlive;
  if (micro) {
    s.flags |= kPFlagMicro | ParticleMicroBits(microScale, lifeTicks);
  }
  return s;
}

int FindMaterialId(const std::vector<MaterialDef>& mats, const std::string& name) {
  for (size_t i = 0; i < mats.size(); i++)
    if (mats[i].name == name) return (int)i;
  return -1;
}

int FindModel(const Prefab& pf, const std::string& name) {
  for (size_t i = 0; i < pf.models.size(); i++)
    if (pf.models[i].name == name) return (int)i;
  return -1;
}

// Joint anchor from geometry: midpoint of the gap between the two limb AABBs
// (prefab-local voxels). Works whenever the art keeps limbs adjacent; the
// sidecar can still override with an explicit "anchor".
Vec3 AutoAnchor(const PrefabModel& limb, const PrefabModel& parent) {
  Vec3 lo, hi;
  for (int a = 0; a < 3; a++) {
    float lmin = a == 0 ? (float)limb.offset.x : a == 1 ? (float)limb.offset.y : (float)limb.offset.z;
    float lext = a == 0 ? (float)limb.size.x : a == 1 ? (float)limb.size.y : (float)limb.size.z;
    float pmin = a == 0 ? (float)parent.offset.x : a == 1 ? (float)parent.offset.y : (float)parent.offset.z;
    float pext = a == 0 ? (float)parent.size.x : a == 1 ? (float)parent.size.y : (float)parent.size.z;
    float mn = std::max(lmin, pmin);
    float mx = std::min(lmin + lext, pmin + pext);
    float v0 = mn <= mx ? (mn + mx) * 0.5f          // overlapping span: middle
                        : (lmin + lext < pmin ? (lmin + lext + pmin) * 0.5f
                                              : (pmin + pext + lmin) * 0.5f);
    (a == 0 ? lo.x : a == 1 ? lo.y : lo.z) = v0;
  }
  (void)hi;
  return lo;
}

Vec3 JsonVec3(const json& j, Vec3 fallback) {
  if (!j.is_array() || j.size() != 3) return fallback;
  return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

Quat JsonQuat(const json& j) {
  if (!j.is_array() || j.size() != 4) return {};
  return QuatNormalize({j[0].get<float>(), j[1].get<float>(),
                        j[2].get<float>(), j[3].get<float>()});
}

// Topologically order limbs parent-before-child. AnimFlatten's single linear
// pass depends on it (a child must never be visited before its parent), and it
// also makes the "detach subtree" walks below strictly forward-looking.
// Returns false on a cycle or a missing parent.
bool TopoSortLimbs(std::vector<MobLimbDef>& limbs, int& rootLimb) {
  const size_t n = limbs.size();
  std::vector<int> parentOf(n, -1);
  for (size_t i = 0; i < n; i++) {
    if ((int)i == rootLimb) continue;
    for (size_t k = 0; k < n; k++)
      if (limbs[k].name == limbs[i].parent) parentOf[i] = (int)k;
    if (parentOf[i] < 0) return false;
  }
  std::vector<int> order;
  std::vector<uint8_t> placed(n, 0);
  order.reserve(n);
  for (size_t pass = 0; pass < n + 1 && order.size() < n; pass++) {
    bool progressed = false;
    for (size_t i = 0; i < n; i++) {
      if (placed[i]) continue;
      if (parentOf[i] >= 0 && !placed[parentOf[i]]) continue;
      placed[i] = 1;
      order.push_back((int)i);
      progressed = true;
    }
    if (!progressed) break;
  }
  if (order.size() != n) return false;  // cycle
  std::vector<MobLimbDef> sorted;
  sorted.reserve(n);
  std::vector<int> newIndex(n, -1);
  for (size_t k = 0; k < n; k++) newIndex[order[k]] = (int)k;
  for (int oldIdx : order) sorted.push_back(std::move(limbs[oldIdx]));
  limbs = std::move(sorted);
  rootLimb = newIndex[rootLimb];
  return true;
}

}  // namespace

// ---- sidecar + .vox pairing -------------------------------------------------

bool LoadMobDefs(const std::string& dir, const std::vector<MaterialDef>& mats,
                 std::vector<MobDef>& out, MicroBodySet& micro,
                 std::string& log) {
  out.clear();
  // The pool is rebuilt wholesale on every load: model indices are positions in
  // it, so growing an existing pool across a hot reload would leave stale defs
  // pointing at the wrong bricks.
  micro.models.clear();
  micro.pool.clear();
  std::error_code ec;
  std::vector<std::string> voxPaths;
  for (auto& e : std::filesystem::directory_iterator(dir, ec))
    if (e.is_regular_file() && e.path().extension() == ".vox")
      voxPaths.push_back(e.path().string());
  if (ec) return true;  // no mob dir: nothing to load
  std::sort(voxPaths.begin(), voxPaths.end());

  for (const std::string& vp : voxPaths) {
    MobDef def;
    std::string err, warn;
    if (!LoadVoxFile(vp, mats.size(), def.prefab, err, warn)) {
      log += vp + ": " + err;
      continue;
    }
    log += warn;
    def.name = def.prefab.name;

    std::string jp = vp.substr(0, vp.size() - 4) + ".json";
    std::ifstream f(jp);
    if (!f) {
      log += def.name + ": missing sidecar " + jp + " — skipped\n";
      continue;
    }
    json j;
    try {
      j = json::parse(f);
    } catch (const std::exception& e) {
      log += jp + ": JSON parse error: " + e.what() + "\n";
      continue;
    }

    std::string root = j.value("root", "");
    if (j.contains("bleed")) {
      std::string bm = j["bleed"].value("material", "");
      int id = FindMaterialId(mats, bm);
      if (id < 0) log += jp + ": unknown bleed material \"" + bm + "\"\n";
      else def.bleedMat = (uint32_t)id;
      def.bleedPerDamage = j["bleed"].value("perDamage", 1.5f);
    }
    def.speed = j.value("speed", 4.0f);

    // Sound slots (assets/sound_schema.js). Presentation only, so a bad entry
    // is skipped rather than failing the def — a mob with a typo'd sound name
    // should still walk into the world, silently.
    def.sounds.clear();
    if (j.contains("sounds") && j["sounds"].is_object())
      for (auto& [slot, name] : j["sounds"].items())
        if (name.is_string() && !name.get<std::string>().empty())
          def.sounds[slot] = name.get<std::string>();

    // Micro-voxel AUTHORING scale (PLAN §C). Anything other than 1/2/4/8 is a
    // typo, not a feature: fall back to 1 loudly rather than half-applying it.
    //
    // "skinScale" is the current name. "scale" is kept as an alias meaning
    // BOTH lattices are equal, which is what it meant before the split — that
    // is what keeps every already-authored sidecar loading unchanged. An asset
    // that says "scale" gets the old single-lattice behaviour exactly; only an
    // asset that says "skinScale" opts into a derived coarser collider.
    const bool authoredSkin = j.contains("skinScale");
    def.skinScale = j.value("skinScale", j.value("scale", 1u));
    if (def.skinScale != 1 && def.skinScale != 2 && def.skinScale != 4 &&
        def.skinScale != 8) {
      log += jp + ": " + (authoredSkin ? "skinScale" : "scale") +
             " must be 1, 2, 4 or 8 (got " + std::to_string(def.skinScale) +
             ") — using 1\n";
      def.skinScale = 1;
    }

    bool ok = true;
    for (auto& l : j.value("limbs", json::array())) {
      MobLimbDef ld;
      ld.name = l.value("name", "");
      ld.parent = l.value("parent", "");
      std::string jt = l.value("joint", "ball");
      ld.joint = jt == "fixed" ? Physics::JointType::Fixed
                 : jt == "hinge" ? Physics::JointType::Hinge
                                 : Physics::JointType::Ball;
      ld.hp = l.value("hp", 20.0f);
      ld.severable = l.value("severable", true);
      ld.vital = l.value("vital", false);
      ld.swingAmp = l.value("swingAmp", 0.0f);
      ld.swingPhase = l.value("swingPhase", 0.0f);
      ld.tag = l.value("tag", "");
      // absent severImpactSpeed = "never severs on impact alone"
      ld.severImpactSpeed = l.value("severImpactSpeed", 0.0f);
      // A cutting edge: the segment this part cuts along, in its own local
      // frame. Authored in MICRO units along an axis of the part's model box;
      // converted to world voxels below, with the anchors.
      if (l.contains("edge") && l["edge"].is_object()) {
        const json& e = l["edge"];
        Vec3 ax{0, 0, 1};
        if (e.contains("axis") && e["axis"].size() == 3)
          ax = {e["axis"][0].get<float>(), e["axis"][1].get<float>(),
                e["axis"][2].get<float>()};
        // The .vox scene is Z-up and the engine is Y-up (voxload.cpp), so an
        // axis authored up the model's +Z is the engine's +Y. Mapping it here
        // means the sidecar can speak the art's coordinates, which is what the
        // generator that wrote them was thinking in.
        Vec3 axEngine{ax.x, ax.z, -ax.y};
        ld.hasEdge = true;
        ld.edgeFrom = axEngine * e.value("from", 0.0f);
        ld.edgeTo = axEngine * e.value("to", 0.0f);
        ld.edgeHalfWidth = e.value("halfWidth", 1.0f);
      }
      if (l.contains("spring") && l["spring"].is_object()) {
        ld.hasSpring = true;
        ld.spring.halflife = l["spring"].value("halflife", 0.15f);
        ld.spring.gain = l["spring"].value("gain", 1.0f);
        ld.spring.maxAngle = l["spring"].value("maxAngle", 0.7f);
      }
      if (l.contains("axis") && l["axis"].size() == 3)
        ld.axis = {l["axis"][0].get<float>(), l["axis"][1].get<float>(),
                   l["axis"][2].get<float>()};
      ld.minAngle = l.value("minAngle", -1.2f);
      ld.maxAngle = l.value("maxAngle", 1.2f);
      if (l.contains("anchor") && l["anchor"].size() == 3) {
        ld.anchor = {l["anchor"][0].get<float>(), l["anchor"][1].get<float>(),
                     l["anchor"][2].get<float>()};
        ld.anchorAuto = false;
      }
      if (FindModel(def.prefab, ld.name) < 0) {
        log += jp + ": limb \"" + ld.name + "\" has no .vox model of that name\n";
        ok = false;
      }
      if (ld.name == root) def.rootLimb = (int)def.limbs.size();
      def.limbs.push_back(std::move(ld));
    }
    if (def.rootLimb < 0) {
      log += jp + ": root \"" + root + "\" is not a limb\n";
      ok = false;
    }
    for (auto& ld : def.limbs) {
      if ((int)(&ld - def.limbs.data()) == def.rootLimb) continue;
      bool found = false;
      for (auto& p : def.limbs) found |= p.name == ld.parent;
      if (!found) {
        log += jp + ": limb \"" + ld.name + "\" parent \"" + ld.parent +
               "\" not found\n";
        ok = false;
      }
    }
    // Derive the COLLIDER resolution from the art (mob.h MobDef::physScale).
    //
    // DebrisVoxel is int8, so a limb's collider box must fit ±120 on every
    // axis. The skin has no such bound (PrefabVoxel is int16), which is the
    // whole point of the split: pick the finest collider that fits BOTH the
    // int8 bound and the kMaxPhysScale cost ceiling (mob.h), and let the skin
    // stay as fine as it was authored. Mina at skinScale 8 lands on physScale
    // 4: her 68-skin-voxel hips would fit ±120 at 8, but an 8× collider is 8×
    // the boxes for no gain a player can feel.
    //
    // Measured on the largest limb, not the whole rig: each limb is its own
    // body with its own origin, so the bound applies per limb.
    {
      int32_t maxExtent = 0;
      for (const PrefabModel& m : def.prefab.models)
        maxExtent = std::max(
            {maxExtent, m.size.x, m.size.y, m.size.z});
      def.physScale = 1;
      for (uint32_t cand : {8u, 4u, 2u, 1u}) {
        if (cand > def.skinScale) continue;  // never finer than the art
        if (cand > kMaxPhysScale) continue;  // physics cost ceiling, below
        // Extents are in skin units; a collider voxel spans skinScale/cand of
        // them, so the collider box is maxExtent * cand / skinScale.
        if ((int64_t)maxExtent * cand / def.skinScale <= 120) {
          def.physScale = cand;
          break;
        }
      }
      // Even physScale 1 can overflow if a limb is over 120 WORLD voxels —
      // that is a genuine authoring error, not something to derive around.
      if ((int64_t)maxExtent * def.physScale / def.skinScale > 120) {
        for (const PrefabModel& m : def.prefab.models)
          if (m.size.x > 120 || m.size.y > 120 || m.size.z > 120) {
            log += def.name + ": limb model \"" + m.name + "\" is " +
                   std::to_string(m.size.x) + "x" + std::to_string(m.size.y) +
                   "x" + std::to_string(m.size.z) +
                   (def.skinScale > 1
                        ? " SKIN voxels (= " +
                              std::to_string(m.size.y / def.skinScale) +
                              " world voxels tall at skinScale " +
                              std::to_string(def.skinScale) +
                              "); even a 1:1 collider exceeds the DebrisVoxel "
                              "int8 bound of 120 world voxels\n"
                        : " voxels, exceeding the int8 bound of 120\n");
            ok = false;
          }
      }
      // Log the pick ALWAYS, not only when it is surprising. Collider
      // resolution is emergent from art size, and an emergent value that
      // changes mass, contacts and ground probes must never move silently —
      // this line is the record that it did.
      if (ok && def.skinScale > 1)
        log += def.name + ": skinScale " + std::to_string(def.skinScale) +
               ", derived physScale " + std::to_string(def.physScale) +
               " (largest limb " + std::to_string(maxExtent) +
               " skin voxels -> " +
               std::to_string(maxExtent * def.physScale / def.skinScale) +
               " collider voxels, bound 120)\n";
    }

    // ---- rig for the animation runtime (all of this is optional data) ----
    if (ok && !TopoSortLimbs(def.limbs, def.rootLimb)) {
      log += jp + ": limb hierarchy has a cycle\n";
      ok = false;
    }
    if (ok) {
      AnimSkeleton& sk = def.skel;
      sk.parts.resize(def.limbs.size());
      for (size_t i = 0; i < def.limbs.size(); i++) {
        const MobLimbDef& ld = def.limbs[i];
        AnimPart& p = sk.parts[i];
        p.name = ld.name;
        p.tag = ld.tag;
        p.parent = -1;
        if ((int)i != def.rootLimb)
          for (size_t k = 0; k < def.limbs.size(); k++)
            if (def.limbs[k].name == ld.parent) p.parent = (int)k;
        p.axis = ld.axis;
        p.swingAmp = ld.swingAmp;
        p.swingPhase = ld.swingPhase;
        p.hasSpring = ld.hasSpring;
        p.spring = ld.spring;
      }
      // rest transforms come from the .vox layout: a part's local rest
      // position is its joint anchor relative to the parent's anchor.
      for (size_t i = 0; i < def.limbs.size(); i++) {
        const MobLimbDef& ld = def.limbs[i];
        int mi = FindModel(def.prefab, ld.name);
        Vec3 anchor = ld.anchor;
        if (ld.anchorAuto && (int)i != def.rootLimb) {
          int pmi = FindModel(def.prefab, ld.parent);
          if (mi >= 0 && pmi >= 0)
            anchor = AutoAnchor(def.prefab.models[mi], def.prefab.models[pmi]);
        } else if ((int)i == def.rootLimb && mi >= 0) {
          const PrefabModel& m = def.prefab.models[mi];
          anchor = Vec3{(float)m.offset.x + m.size.x * 0.5f, (float)m.offset.y,
                        (float)m.offset.z + m.size.z * 0.5f};
        }
        // SKIN -> WORLD. Anchors are authored in .vox coordinates, which at
        // skinScale>1 are SKIN units — this is the ART's lattice, so it is
        // skinScale here and NOT physScale. (The collider frame conversion is
        // the one in CarveLimb, which multiplies by physScale; confusing the
        // two shifts every joint in the rig without changing anything visible
        // about the art, which is why they are commented at both ends.)
        //
        // The rig, the gait and the physics all work in world voxels.
        // Converting HERE, once, is what keeps every downstream stage
        // (AnimFlatten, IK, GroundHeightAt, the joint anchors in Spawn)
        // completely scale-unaware.
        const float inv = 1.0f / (float)def.skinScale;
        sk.parts[i].anchorLocal = anchor * inv;
        // The cutting edge rides the same conversion, for the same reason: it
        // is rig geometry, and every consumer downstream works in world
        // voxels. Its offsets are measured from the part's own ORIGIN (the
        // model's min corner), so they need no anchor rebasing here — the
        // melee sweep composes them with the part transform, which already
        // carries the origin.
        MobLimbDef& mld = def.limbs[i];
        if (mld.hasEdge) {
          mld.edgeFrom = mld.edgeFrom * inv;
          mld.edgeTo = mld.edgeTo * inv;
          mld.edgeHalfWidth *= inv;
        }
      }
      for (size_t i = 0; i < def.limbs.size(); i++) {
        int par = sk.parts[i].parent;
        sk.parts[i].rest.pos =
            par >= 0 ? sk.parts[i].anchorLocal - sk.parts[par].anchorLocal
                     : sk.parts[i].anchorLocal;
      }

      // ---- sockets: where a held ITEM attaches (mob.h MobSocketDef) --------
      //
      // Parsed after the limbs because a socket names the part it rides and is
      // resolved to an index here — a socket on a part that does not exist is
      // a loud diagnostic, never a silent no-op, since the failure mode it
      // guards against is an item that renders at the origin instead of in the
      // hand.
      //
      // Offsets take the SAME skin -> world conversion the anchors just did,
      // and for the same reason: everything downstream works in world voxels.
      {
        const float inv = 1.0f / (float)def.skinScale;
        for (const auto& s : j.value("sockets", json::array())) {
          MobSocketDef sd;
          sd.name = s.value("name", "");
          sd.part = s.value("part", "");
          if (sd.name.empty() || sd.part.empty()) {
            log += jp + ": socket needs both \"name\" and \"part\"\n";
            continue;
          }
          sd.partIndex = sk.FindPart(sd.part);
          if (sd.partIndex < 0) {
            log += jp + ": socket \"" + sd.name + "\" names part \"" + sd.part +
                   "\", which is not a limb of this rig\n";
            continue;
          }
          if (s.contains("offset") && s["offset"].size() == 3)
            sd.offset = Vec3{s["offset"][0].get<float>(),
                             s["offset"][1].get<float>(),
                             s["offset"][2].get<float>()} * inv;
          if (s.contains("rotation") && s["rotation"].size() == 3)
            sd.rotation = QuatFromEulerDeg({s["rotation"][0].get<float>(),
                                            s["rotation"][1].get<float>(),
                                            s["rotation"][2].get<float>()});
          def.sockets.push_back(std::move(sd));
        }
      }

      if (j.contains("gait") && j["gait"].is_object()) {
        const json& g = j["gait"];
        GaitDef& gd = sk.gait;
        gd.present = true;
        gd.cadence = g.value("cadence", 2.2f);
        gd.strideBias = g.value("strideBias", 0.35f);
        gd.leadTime = g.value("leadTime", 0.2f);
        gd.stepThreshold = g.value("stepThreshold", 0.6f);
        gd.stepDuration = g.value("stepDuration", 0.22f);
        gd.stepHeight = g.value("stepHeight", 0.25f);
        gd.rideHeight = g.value("rideHeight", 0.9f);
        gd.bobAmp = g.value("bobAmp", 0.06f);
        gd.bobFreqMul = g.value("bobFreqMul", 2.0f);
        gd.swayAmp = g.value("swayAmp", 0.05f);
        gd.rollAmp = g.value("rollAmp", 0.09f);
        gd.spineCounter = g.value("spineCounter", 0.7f);
        gd.phaseLag = g.value("phaseLag", 0.05f);
        for (const auto& grp : g.value("groups", json::array())) {
          std::vector<int> members;
          for (const auto& nm : grp) {
            int pi = sk.FindPart(nm.get<std::string>());
            if (pi >= 0) members.push_back(pi);
            else log += jp + ": gait group names unknown part \"" +
                        nm.get<std::string>() + "\"\n";
          }
          if (!members.empty()) gd.groups.push_back(std::move(members));
        }
      }

      // Steering limits (anim.h LocomotionDef). Absent = the defaults, which
      // are tuned for a humanoid; every field is optional so an existing
      // sidecar keeps working untouched.
      if (j.contains("locomotion") && j["locomotion"].is_object()) {
        const json& l = j["locomotion"];
        LocomotionDef& ld = sk.loco;
        ld.turnRate = l.value("turnRate", ld.turnRate);
        ld.turnAccel = l.value("turnAccel", ld.turnAccel);
        ld.driveAlignFull = l.value("driveAlignFull", ld.driveAlignFull);
        ld.driveAlignZero = l.value("driveAlignZero", ld.driveAlignZero);
        ld.turnRateMoving = l.value("turnRateMoving", ld.turnRateMoving);
        // A zero-width align band would divide by zero in the drive scale.
        if (ld.driveAlignZero <= ld.driveAlignFull)
          ld.driveAlignZero = ld.driveAlignFull + 1e-3f;
        if (ld.turnRate < 0) ld.turnRate = 0;
      }

      for (const auto& c : j.value("chains", json::array())) {
        IkChain ch;
        ch.tag = c.value("tag", "");
        ch.pole = JsonVec3(c.contains("pole") ? c["pole"] : json(), {0, 0, 1});
        std::string solver = c.value("solver", "twobone");
        ch.solver = IkSolver::TwoBone;
        if (solver != "twobone")
          log += jp + ": chain solver \"" + solver +
                 "\" unsupported, using twobone\n";
        for (const auto& nm : c.value("parts", json::array())) {
          int pi = sk.FindPart(nm.get<std::string>());
          if (pi >= 0) ch.parts.push_back(pi);
          else log += jp + ": chain names unknown part \"" +
                      nm.get<std::string>() + "\"\n";
        }
        std::string eff = c.value("effector", "");
        ch.effector = eff.empty() ? (ch.parts.empty() ? -1 : ch.parts.back())
                                  : sk.FindPart(eff);
        if (ch.parts.size() >= 2 && ch.effector >= 0) sk.chains.push_back(std::move(ch));
        else log += jp + ": chain \"" + ch.tag + "\" needs >=2 parts + effector\n";
      }

      // Dismemberment locomotion states. Authored order IS the priority
      // order (first match wins), so the array form is deliberate — an object
      // would let the JSON library reorder the rules.
      for (const auto& s : j.value("states", json::array())) {
        AnimStateRule rule;
        rule.name = s.value("name", "");
        auto partList = [&](const char* key, std::vector<int>& out) {
          for (const auto& nm : s.value(key, json::array())) {
            int pi = sk.FindPart(nm.get<std::string>());
            if (pi >= 0) out.push_back(pi);
            else log += jp + ": state \"" + rule.name +
                        "\" names unknown part \"" + nm.get<std::string>() + "\"\n";
          }
        };
        partList("missing", rule.missingAll);
        partList("missingAny", rule.missingAnyOf);
        rule.minChainsLost = s.value("minChainsLost", 0);
        rule.clip = s.value("clip", "");
        rule.speedScale = s.value("speedScale", 1.0f);
        rule.disableGait = s.value("disableGait", false);
        rule.bodyYOffset = s.value("bodyYOffset", 0.0f);
        if (rule.missingAll.empty() && rule.missingAnyOf.empty() &&
            rule.minChainsLost <= 0)
          log += jp + ": state \"" + rule.name +
                 "\" has an empty predicate and will never match\n";
        sk.states.push_back(std::move(rule));
      }

      // NB: bind the object to a named local. `j.value(k, json::object())`
      // returns a TEMPORARY; iterating begin()/end() off two separate
      // temporaries yields iterators into different destroyed objects.
      const json clipsJson =
          j.contains("clips") && j["clips"].is_object() ? j["clips"] : json::object();
      for (auto it = clipsJson.begin(); it != clipsJson.end(); ++it) {
        const json& c = it.value();
        AnimClip clip;
        clip.name = it.key();
        clip.durationMs = c.value("durationMs", 0);
        clip.loop = c.value("loop", false);
        clip.mode = c.value("mode", std::string("override")) == "additive"
                        ? ClipMode::Additive
                        : ClipMode::Override;
        clip.blendInMs = c.value("blendInMs", 0);
        clip.blendOutMs = c.value("blendOutMs", 0);
        if (c.contains("mask") && c["mask"].is_array()) {
          clip.mask.assign(sk.parts.size(), 0);
          for (const auto& nm : c["mask"]) {
            int pi = sk.FindPart(nm.get<std::string>());
            if (pi >= 0) clip.mask[pi] = 1;
          }
        }
        if (c.contains("tracks") && c["tracks"].is_object()) {
          for (auto t = c["tracks"].begin(); t != c["tracks"].end(); ++t) {
            int pi = sk.FindPart(t.key());
            if (pi < 0) {
              log += jp + ": clip \"" + clip.name + "\" track for unknown part \"" +
                     t.key() + "\"\n";
              continue;
            }
            AnimTrack tr;
            tr.part = pi;
            // fuse the rot and pos key lists by time (the plan doc's fused
            // keyframe model: one key carries both channels)
            std::vector<AnimKey> keys;
            auto upsert = [&](int32_t tMs) -> AnimKey& {
              for (AnimKey& k : keys)
                if (k.tMs == tMs) return k;
              keys.push_back(AnimKey{});
              keys.back().tMs = tMs;
              return keys.back();
            };
            for (const auto& k : t.value().value("rot", json::array())) {
              AnimKey& key = upsert(k.value("t", 0));
              key.rot = JsonQuat(k.contains("q") ? k["q"] : json());
              key.hasRot = true;
              if (k.contains("ease")) key.ease = ParseEase(k["ease"].get<std::string>());
            }
            for (const auto& k : t.value().value("pos", json::array())) {
              AnimKey& key = upsert(k.value("t", 0));
              key.pos = JsonVec3(k.contains("v") ? k["v"] : json(), {});
              key.hasPos = true;
              if (k.contains("ease")) key.ease = ParseEase(k["ease"].get<std::string>());
            }
            std::sort(keys.begin(), keys.end(),
                      [](const AnimKey& a, const AnimKey& b) { return a.tMs < b.tMs; });
            tr.keys = std::move(keys);
            if (!tr.keys.empty()) clip.tracks.push_back(std::move(tr));
          }
        }
        sk.clips.push_back(std::move(clip));
      }

      // States name clips by string and PlayClip silently no-ops on a miss, so
      // a typo'd crawl clip would otherwise fail as "the mob just slides".
      for (const AnimStateRule& rule : sk.states)
        if (!rule.clip.empty() && sk.FindClip(rule.clip) < 0)
          log += jp + ": state \"" + rule.name + "\" names unknown clip \"" +
                 rule.clip + "\"\n";

      const json fbJson = j.contains("flipbooks") && j["flipbooks"].is_object()
                              ? j["flipbooks"]
                              : json::object();
      for (auto it = fbJson.begin(); it != fbJson.end(); ++it) {
        Flipbook fb;
        fb.name = it.key();
        fb.loop = it.value().value("loop", true);
        for (const auto& f : it.value().value("frames", json::array())) {
          FlipbookFrame ff;
          ff.part = sk.FindPart(f.value("part", ""));
          ff.model = f.value("model", 0);
          ff.durationMs = f.value("durationMs", 100);
          if (ff.part >= 0) fb.frames.push_back(ff);
        }
        if (!fb.frames.empty()) sk.flipbooks.push_back(std::move(fb));
      }

      if (!sk.ParentsFirst()) {  // belt and braces: AnimFlatten depends on it
        log += jp + ": internal error, parts are not parent-before-child\n";
        ok = false;
      }
    }

    // Prefab box in WORLD voxels — the one number the gait/terrain code reads.
    //
    // HELD PROPS ARE EXCLUDED. This box is the CREATURE's, not its luggage:
    // the gait pivot, the avatar's origin, its standing height and the terrain
    // anchor radius all derive from it (avatar.cpp, and the pivot uses below).
    // A sword lying in the hand reaches well outside the body, so counting it
    // here silently re-centres the rig on the weapon — which showed up as the
    // avatar's walk widening until its legs failed their own upright
    // assertion, a "leg bug" whose actual cause was the thing it was holding.
    //
    // Anything tagged "prop" is therefore measured out. Props still render,
    // still collide and are still severable; they simply do not define how big
    // the creature is.
    {
      IVec3 lo{INT32_MAX, INT32_MAX, INT32_MAX};
      IVec3 hi{INT32_MIN, INT32_MIN, INT32_MIN};
      bool any = false;
      for (const MobLimbDef& ld : def.limbs) {
        if (ld.tag == "prop") continue;
        int mi = FindModel(def.prefab, ld.name);
        if (mi < 0) continue;
        const PrefabModel& m = def.prefab.models[mi];
        lo.x = std::min(lo.x, m.offset.x);
        lo.y = std::min(lo.y, m.offset.y);
        lo.z = std::min(lo.z, m.offset.z);
        hi.x = std::max(hi.x, m.offset.x + m.size.x);
        hi.y = std::max(hi.y, m.offset.y + m.size.y);
        hi.z = std::max(hi.z, m.offset.z + m.size.z);
        any = true;
      }
      const Vec3 box =
          any ? Vec3{(float)(hi.x - lo.x), (float)(hi.y - lo.y),
                     (float)(hi.z - lo.z)}
              : Vec3{(float)def.prefab.size.x, (float)def.prefab.size.y,
                     (float)def.prefab.size.z};
      // The prefab box is measured in the .vox's own units, which are SKIN
      // units — so it divides by skinScale to reach world voxels.
      def.worldSize = box * (1.0f / (float)def.skinScale);
    }

    // ---- micro brick upload (PLAN §C, sim/microbody.h) ----
    // Packed once per DEF, shared by every instance: a limb's voxels never
    // change after load in v1, so there is no per-instance storage at all.
    // Done last so a def that failed validation never enters the pool.
    if (ok && def.skinScale > 1) {
      for (MobLimbDef& ld : def.limbs) {
        int mi = FindModel(def.prefab, ld.name);
        if (mi < 0) continue;
        const PrefabModel& m = def.prefab.models[mi];
        // The one PURE RENDER read in the loader: the brick is the art, so it
        // is packed at the authored skin resolution and never at physScale.
        ld.microModel = MicroBodyPack(micro, m.voxels, m.size, def.skinScale,
                                      def.name + "/" + ld.name, log);
        if (ld.microModel < 0)
          log += def.name + ": limb \"" + ld.name +
                 "\" has no micro brick and will not render (the cube path "
                 "would draw it at the wrong scale)\n";
      }
    }
    if (ok) out.push_back(std::move(def));
  }
  return true;
}

// ---- MobSystem ---------------------------------------------------------------

void MobSystem::Init(Physics* phys, World* world, DebrisSystem* debris,
                     const std::vector<MaterialDef>& mats) {
  phys_ = phys;
  world_ = world;
  debris_ = debris;
  OnMaterialsReloaded(mats);
}

void MobSystem::OnMaterialsReloaded(const std::vector<MaterialDef>& mats) {
  densityOf_.clear();
  classOf_.clear();
  for (const auto& m : mats) {
    densityOf_.push_back((float)m.gpu.density);
    classOf_.push_back(m.gpu.klass);
  }
}

void MobSystem::SetDefs(std::vector<MobDef> defs) { defs_ = std::move(defs); }

void MobSystem::Reset() {
  for (Mob& m : mobs_)
    for (Limb& l : m.limbs) {
      // held pieces are DebrisSystem's now; only drop the kinematic hold
      if (l.holdBody) {
        phys_->SetBodyKinematic(l.holdBody, false);
        phys_->ClearCollisionGroup(l.holdBody);
        l.holdBody = 0;
      }
      // A carved limb owns a copy-on-write brick; dropping the mob without
      // returning it leaks pool words nothing will ever reclaim.
      ReleaseLimbMicro(l);
      if (l.body) phys_->RemoveBody(l.body);
    }
  mobs_.clear();
  instancesDirty_ = true;
}

// Draws this mob's own bleed character. Entity-scoped variances resolve here,
// once, against the mob id; event-scoped ones are left alone (they are drawn
// per droplet at the spray sites, where the droplet index is in hand).
//
// The whole-wound gain is folded into every QUANTITY (spray counts, thrown
// voxels) but deliberately NOT into speeds, cones or lifetimes: "this one is a
// gusher" should mean more blood, not blood that also flies faster and lives
// longer, which reads as a different material rather than a worse wound.
MobSystem::GoreProfile MobSystem::MakeGoreProfile(uint64_t mobId) {
  const auto& g = CurrentTuning().gore;
  const uint32_t seed = (uint32_t)(mobId ^ (mobId >> 32));
  // Entity draws share one seed but must not share one INDEX, or every
  // parameter with the same distribution would move in lockstep.
  auto ent = [&](float base, const Variance& v, uint32_t idx) {
    return v.scope == Variance::kEntity ? ApplyVariance(base, v, seed, 0, idx)
                                        : base;
  };
  auto entI = [&](int base, const Variance& v, uint32_t idx) {
    return v.scope == Variance::kEntity ? ApplyVarianceI(base, v, seed, 0, idx)
                                        : base;
  };

  GoreProfile p;
  // Gain first: it multiplies the quantities drawn below. Floored at 0 so a
  // wide gaussian tail cannot invert a wound into negative blood.
  p.bleedGain = ent(g.bleedGain, g.bleedGainVar, 101u);
  if (!(p.bleedGain > 0.0f)) p.bleedGain = 0.0f;

  p.bleedSprayPerDrip =
      ent(g.bleedSprayPerDrip, g.bleedSprayPerDripVar, 1u) * p.bleedGain;
  p.bleedSpraySpeed = ent(g.bleedSpraySpeed, g.bleedSpraySpeedVar, 2u);
  p.bleedSprayCone = ent(g.bleedSprayCone, g.bleedSprayConeVar, 3u);
  p.severSpray =
      (int)std::lround(entI(g.severSpray, g.severSprayVar, 4u) * p.bleedGain);
  p.severSpraySpeed = ent(g.severSpraySpeed, g.severSpraySpeedVar, 5u);
  p.severSprayCone = ent(g.severSprayCone, g.severSprayConeVar, 6u);
  p.severVoxels =
      (int)std::lround(entI(g.severVoxels, g.severVoxelsVar, 7u) * p.bleedGain);
  p.severVoxelSpeed = ent(g.severVoxelSpeed, g.severVoxelSpeedVar, 8u);
  p.severDecayTicks = entI(g.severDecayTicks, g.severDecayTicksVar, 9u);
  p.microLifeTicks = entI(g.microLifeTicks, g.microLifeTicksVar, 10u);

  // Re-apply the invariants the loader enforces on the authored values: a
  // variance draw can push past them, and microLifeTicks in particular is an
  // 8-bit field (PMICRO_LIFE_MASK) that wraps to "dies instantly" past 255.
  if (p.severDecayTicks < 1) p.severDecayTicks = 1;
  if (p.microLifeTicks < 1) p.microLifeTicks = 1;
  if (p.microLifeTicks > 255) p.microLifeTicks = 255;
  if (p.bleedSprayPerDrip < 0.0f) p.bleedSprayPerDrip = 0.0f;
  if (p.bleedSpraySpeed < 0.0f) p.bleedSpraySpeed = 0.0f;
  if (p.severSpraySpeed < 0.0f) p.severSpraySpeed = 0.0f;
  if (p.severVoxelSpeed < 0.0f) p.severVoxelSpeed = 0.0f;
  if (p.bleedSprayCone < 0.0f) p.bleedSprayCone = 0.0f;
  if (p.severSprayCone < 0.0f) p.severSprayCone = 0.0f;
  if (p.severSpray < 0) p.severSpray = 0;
  if (p.severVoxels < 0) p.severVoxels = 0;
  return p;
}

void MobSystem::RefreshGoreProfiles() {
  // Same id -> same draw, so a mob keeps its identity across a reload unless
  // the variance settings themselves changed.
  for (Mob& mob : mobs_) mob.gore = MakeGoreProfile(mob.id);
}

uint64_t MobSystem::Spawn(int defIndex, IVec3 atVoxel) {
  if (defIndex < 0 || defIndex >= (int)defs_.size()) return 0;
  if (mobs_.size() >= kMaxMobs) return 0;
  const MobDef& def = defs_[defIndex];

  Mob mob;
  mob.id = nextId_++;
  mob.gore = MakeGoreProfile(mob.id);
  mob.defIndex = defIndex;
  mob.origin = Vec3{(float)atVoxel.x, (float)atVoxel.y, (float)atVoxel.z};
  mob.limbs.resize(def.limbs.size());

  // SKIN -> WORLD. The .vox model is authored on the skin lattice, so every
  // POSITION derived from it divides by skinScale to reach world voxels.
  const float inv = 1.0f / (float)def.skinScale;
  // COLLIDER pitch: limb.voxels live on the physScale lattice, so the Jolt
  // body is built at 1/physScale. These two differ whenever the collider was
  // derived coarser than the art, and using one where the other belongs is the
  // silent-joint-shift bug the split exists to make impossible.
  const float physInv = 1.0f / (float)std::max(1u, def.physScale);
  const uint32_t ratio =
      std::max(1u, def.skinScale / std::max(1u, def.physScale));

  for (size_t i = 0; i < def.limbs.size(); i++) {
    const MobLimbDef& ld = def.limbs[i];
    int mi = FindModel(def.prefab, ld.name);
    const PrefabModel& model = def.prefab.models[mi];
    Limb& limb = mob.limbs[i];
    limb.hp = ld.hp;
    limb.microModel = ld.microModel;
    limb.restOffset = Vec3{(float)model.offset.x, (float)model.offset.y,
                           (float)model.offset.z} * inv;
    if (ratio > 1) {
      // Fine skin: the authored art IS the skin lattice, and the collider is
      // DERIVED from it by the same majority-fill the debris path uses. Data
      // flows skin -> collider and never back (phys/lattice.h), so the two can
      // never drift; a carve edits the skin and re-derives.
      limb.skinVoxels.reserve(model.voxels.size());
      for (const PrefabVoxel& v : model.voxels) {
        uint32_t variant = ((uint32_t)(v.x * 7 + v.y * 13 + v.z * 29)) % 3u;
        limb.skinVoxels.push_back(
            {v.x, v.y, v.z, (uint16_t)(v.material | (variant << 12))});
      }
      bool overflow = false;
      limb.voxels = DownsampleSkin(limb.skinVoxels, ratio, &overflow);
      if (overflow)
        std::printf(
            "mob: limb \"%s\" of %s exceeded the collider's +-127 bound; part "
            "of it was dropped from the collider (physScale too fine)\n",
            ld.name.c_str(), def.name.c_str());
      // Collider units, so the body's box matches the lattice it is built on.
      limb.size = IVec3{(model.size.x + (int)ratio - 1) / (int)ratio,
                        (model.size.y + (int)ratio - 1) / (int)ratio,
                        (model.size.z + (int)ratio - 1) / (int)ratio};
    } else {
      // The two lattices coincide: `voxels` is the whole story and skinVoxels
      // stays empty, exactly the pre-split path. Every existing def is here.
      limb.size = model.size;
      limb.voxels.reserve(model.voxels.size());
      for (const PrefabVoxel& v : model.voxels) {
        uint32_t variant = ((uint32_t)(v.x * 7 + v.y * 13 + v.z * 29)) % 3u;
        limb.voxels.push_back({(int8_t)v.x, (int8_t)v.y, (int8_t)v.z, 0,
                               (uint16_t)(v.material | (variant << 12))});
      }
    }
    // Authored volume, so carve damage can be expressed as a FRACTION of the
    // limb — the same wound should read the same on a scale-1 and a scale-4 rig.
    // Counted on the lattice a carve actually removes from, so the fraction is
    // measured against the same denominator that shrinks.
    limb.voxelsAtSpawn = (uint32_t)(limb.HasFineSkin() ? limb.skinVoxels.size()
                                                       : limb.voxels.size());
    // The body origin is the limb's min corner in WORLD voxels; the collider
    // is built at pitch 1/physScale so its collider-unit local coordinates land
    // in the right physical place. Not an integer cell any more at scale>1,
    // which is fine — a Jolt body has never been lattice-aligned.
    Vec3 origin = mob.origin + limb.restOffset;
    BodyTransform bxf{};
    bxf.pos = origin;
    bxf.quat[3] = 1;
    limb.body = phys_->CreateDebrisBodyXf(limb.voxels, bxf, densityOf_,
                                          true /*allowKinematic*/, physInv);
    if (limb.body == 0) {
      for (Limb& l : mob.limbs)
        if (l.body) phys_->RemoveBody(l.body);
      return 0;
    }
    phys_->SetBodyKinematic(limb.body, true);
    limb.xf.pos = origin;
    limb.xf.quat[3] = 1;
  }

  // joints after all bodies exist (world-space anchors at the rest pose)
  for (size_t i = 0; i < def.limbs.size(); i++) {
    const MobLimbDef& ld = def.limbs[i];
    if ((int)i == def.rootLimb) continue;
    int pi = -1;
    for (size_t k = 0; k < def.limbs.size(); k++)
      if (def.limbs[k].name == ld.parent) pi = (int)k;
    // The RIG is the single source of joint geometry: skel.parts[i].anchorLocal
    // is this exact anchor, already resolved (auto vs authored) and already
    // converted micro -> world at load. Re-deriving it from def.prefab here
    // would be a second implementation of the same rule, and the two silently
    // disagreeing shows up as limbs pivoting off-joint.
    Vec3 anchor = def.skel.parts[i].anchorLocal;
    Limb& limb = mob.limbs[i];
    limb.anchorRoot = anchor;
    limb.anchorLimb = anchor - limb.restOffset;
    Vec3 anchorWorld = mob.origin + anchor;
    limb.joint = phys_->CreateJoint(mob.limbs[pi].body, limb.body, ld.joint,
                                    anchorWorld, ld.axis, ld.minAngle,
                                    ld.maxAngle);
  }
  // limbs of one mob never collide with each other (they'd fight the joints
  // and jitter forever); different mobs and debris still collide normally
  {
    std::vector<uint64_t> handles;
    for (const Limb& l : mob.limbs) handles.push_back(l.body);
    phys_->DisableCollisionsAmong(handles);
  }

  // Root's "anchor" is its own centre (the yaw pivot) — again taken from the
  // rig, which already computed exactly that at load.
  {
    Limb& root = mob.limbs[def.rootLimb];
    root.anchorRoot = def.skel.parts[def.rootLimb].anchorLocal;
    root.anchorLimb = root.anchorRoot - root.restOffset;
  }

  // Flipbook frames: pre-convert every .vox model any flipbook references, so
  // a frame change is a pointer swap in AppendInstances rather than a parse.
  // Only parts that actually appear in a flipbook pay for this.
  for (const Flipbook& fb : def.skel.flipbooks)
    for (const FlipbookFrame& ff : fb.frames) {
      if (ff.part < 0 || ff.part >= (int)mob.limbs.size()) continue;
      if (ff.model < 0 || ff.model >= (int)def.prefab.models.size()) continue;
      Limb& limb = mob.limbs[ff.part];
      if ((int)limb.frameVoxels.size() <= ff.model)
        limb.frameVoxels.resize(ff.model + 1);
      if (!limb.frameVoxels[ff.model].empty()) continue;
      const PrefabModel& fm = def.prefab.models[ff.model];
      std::vector<DebrisVoxel>& dst = limb.frameVoxels[ff.model];
      dst.reserve(fm.voxels.size());
      for (const PrefabVoxel& v : fm.voxels) {
        uint32_t variant = ((uint32_t)(v.x * 7 + v.y * 13 + v.z * 29)) % 3u;
        dst.push_back({(int8_t)v.x, (int8_t)v.y, (int8_t)v.z, 0,
                       (uint16_t)(v.material | (variant << 12))});
      }
    }

  // animation runtime state (float presentation, never hashed)
  mob.anim.partAlive.assign(def.limbs.size(), 1);
  mob.anim.springs.assign(def.limbs.size(), SpringState{});
  mob.anim.feet.assign(def.skel.chains.size(), FootState{});
  for (size_t c = 0; c < def.skel.chains.size(); c++) {
    const IkChain& ch = def.skel.chains[c];
    // leg length drives every gait threshold, so it is measured from the rig
    // rather than tuned per mob: sum of the chain's rest bone lengths.
    float len = 0;
    for (size_t k = 1; k < ch.parts.size(); k++)
      len += def.skel.parts[ch.parts[k]].rest.pos.len();
    mob.anim.feet[c].legLength = std::max(len, 1.0f);
  }
  // Rest sole height above the prefab min corner: walk each leg chain's rest
  // offsets down from the root and keep the LOWEST effector anchor. Measured
  // from the rig rather than authored, for the same reason legLength is — the
  // art moves, and a hand-tuned constant would silently rot when it does.
  //
  // This lands on the ANKLE PIVOT, not the bottom of the shoe, so a rig whose
  // foot art hangs below its pivot sits low by that overhang. Deliberately not
  // "fixed" by measuring the effector's own voxels: the foot models do not
  // share a local frame (the critter's is flipped), so that correction helps
  // one rig and pushes another several voxels into the air. `rideHeight` is the
  // authored per-rig trim for exactly this residual — see the stance term in
  // UpdateGait — and it is already within a voxel on every current def.
  {
    float lowest = 0;
    bool any = false;
    for (const IkChain& ch : def.skel.chains) {
      if (ch.tag != "leg" || ch.parts.empty()) continue;
      // rest.pos is parent-relative (anchorLocal deltas), so accumulating from
      // the chain root's absolute anchor reproduces AnimFlatten's rest result
      // without running the whole pipeline.
      float y = def.skel.parts[ch.parts[0]].anchorLocal.y;
      for (size_t k = 1; k < ch.parts.size(); k++)
        y += def.skel.parts[ch.parts[k]].rest.pos.y;
      if (!any || y < lowest) { lowest = y; any = true; }
    }
    mob.restSoleY = any ? lowest : 0.0f;
  }
  mob.anim.lastPos = mob.origin;
  mob.bodyY = mob.origin.y;

  mobs_.push_back(std::move(mob));
  instancesDirty_ = true;
  return mobs_.back().id;
}

bool MobSystem::GroundHeightAt(World& world, int wx, int wz, int yFrom,
                               int& outY) const {
  // scan down through the chunk cache; request fetches for missing chunks
  // (bounded: one column per mob per tick)
  for (int y = yFrom; y > yFrom - 24; y--) {
    IVec3 cell{wx, y, wz};
    if (!world.CellInWindow(cell)) return false;
    IVec3 wc{wx >> 4, y >> 4, wz >> 4};
    const CachedChunk* cc = world.Cached(wc);
    if (!cc || cc->voxels.size() != kChunkVol) {
      world.RequestChunkFetch(wc);
      return false;
    }
    uint32_t lx = (uint32_t)(wx & 15), ly = (uint32_t)(y & 15),
             lz = (uint32_t)(wz & 15);
    uint32_t mat = cc->voxels[(lz * kChunk + ly) * kChunk + lx] & 0xFFF;
    // solids/powders carry weight; liquids/gases don't (mobs wade, not walk
    // on blood pools)
    if (mat != 0 && mat < classOf_.size() &&
        (classOf_[mat] == CLASS_SOLID || classOf_[mat] == CLASS_POWDER)) {
      outY = y + 1;
      return true;
    }
  }
  return false;
}

void MobSystem::PlayClip(Mob& mob, const MobDef& def, const std::string& name) {
  int ci = def.skel.FindClip(name);
  if (ci < 0) return;
  for (ClipInstance& inst : mob.anim.clips)
    if (inst.clip == ci && !inst.stopping) {  // retrigger: restart, don't stack
      // ONLY a one-shot rewinds. A looping clip that is already running needs
      // no retrigger, and rewinding one is actively wrong: loco clips are
      // re-requested every tick to keep them alive, so resetting timeMs pins
      // the clip at t=0 forever and the pose freezes on the first keyframe.
      // avatar.cpp had this fixed; mob.cpp did not, and mobs re-request their
      // state clip on the same per-tick cadence.
      if (!def.skel.clips[ci].loop) {
        inst.timeMs = 0;
        inst.ageMs = 0;
      }
      return;
    }
  ClipInstance inst;
  inst.clip = ci;
  inst.weight = 1.0f;
  mob.anim.clips.push_back(inst);
}

// ---- locomotion stage 0: sense ---------------------------------------------
// One terrain probe per tick, shared by intent and drive. The fan is in the
// mob's own frame (probe 0 is dead ahead) so the behaviour layer can reason in
// "how far off my nose" without knowing the world yaw.
MobSystem::GroundSense MobSystem::SenseGround(const Mob& mob, const MobDef& def,
                                             World& world) const {
  GroundSense s;
  const float cx = mob.origin.x + def.worldSize.x * 0.5f;
  const float cz = mob.origin.z + def.worldSize.z * 0.5f;
  const int yFrom = ifloor(mob.origin.y) + 3;
  s.haveGround = GroundHeightAt(world, ifloor(cx), ifloor(cz), yFrom, s.groundY);

  // Probe at the mob's own footprint plus a margin, so a wide creature notices
  // a wall before its shoulder is already inside it. The reach is taken
  // PER AXIS (an ellipse, not a circle) to match the footprint of a non-square
  // mob — an isotropic max() makes a long creature probe well past where it
  // can actually walk, and it stops short of gaps it would fit through.
  const float reachX = def.worldSize.x * 0.5f + 2.0f;
  const float reachZ = def.worldSize.z * 0.5f + 2.0f;
  for (int i = 0; i < GroundSense::kProbeCount; i++) {
    const float yaw =
        mob.heading + (6.2831853f * (float)i) / (float)GroundSense::kProbeCount;
    const float px = cx + std::sin(yaw) * reachX;
    const float pz = cz + std::cos(yaw) * reachZ;
    int py = 0;
    if (!GroundHeightAt(world, ifloor(px), ifloor(pz), yFrom, py)) {
      // Unknown footing is WALKABLE, and deliberately so. The probe reaches
      // past the CPU mirror long before it reaches anything interesting, so
      // treating unknown as blocked makes a mob stop dead at the edge of its
      // own knowledge — it reads as an invisible wall, and it is the mob-scale
      // version of the projectile bug in CLAUDE.md (Unknown is not the same
      // test as out-of-window). The drive's own ground check still refuses to
      // walk off into space, so nothing here can strand a mob in the air.
      //
      // `stepUp` is left at 0 rather than INT_MAX so the intent layer's
      // flatness tie-break does not treat "I cannot see" as "a cliff".
      s.clear[i] = true;
      s.stepUp[i] = 0;
      continue;
    }
    const int rise = s.haveGround ? py - s.groundY : 0;
    s.stepUp[i] = rise;
    // > 2 voxels of rise is beyond step-up reach; a big DROP is survivable but
    // not desirable, so it is walkable and the intent layer merely prefers
    // flatter ground.
    s.clear[i] = rise <= 2;
  }
  return s;
}

// ---- locomotion stage 1: intent --------------------------------------------
// THE AI SEAM. The only stage permitted an opinion, and the only thing it may
// write is desiredHeading / driveScale. Replacing this function with a
// behaviour tree, a utility scorer or a nav-mesh follower is the whole
// extension story; steering, drive, gait and pose below are all agnostic.
//
// Today: walk forward, and when the way ahead is not walkable pick the clear
// probe direction closest to the current heading. Choosing by ANGULAR DISTANCE
// rather than by a fixed 90-degree turn is what makes the avoidance produce
// free angles — a mob grazing a wall deflects a few degrees along it instead
// of ricocheting orthogonally.
void MobSystem::DecideIntent(Mob& mob, const MobDef& def,
                             const GroundSense& sense, uint32_t tick,
                             float dt) {
  mob.driveScale = 1.0f;
  if (!sense.haveGround) return;

  // The forward probe is index 0 by construction of the fan.
  const bool blockedAhead = !sense.clear[0];
  if (!blockedAhead) {
    mob.blockedTicks = 0;
    return;
  }
  mob.blockedTicks++;

  // Re-picking a target every tick while a slow turn is still executing makes
  // the mob dither in place. Commit to a chosen deflection for at least as
  // long as a quarter turn takes, unless we are still blocked well after that.
  const LocomotionDef& lo = def.skel.loco;
  const uint32_t kCommitTicks =
      (uint32_t)std::max(4.0f, (1.5707963f / std::max(lo.turnRate, 0.1f)) / dt);
  if (tick < mob.lastTurnTick + kCommitTicks) return;

  // Score every clear probe by how little it deviates from where we are
  // already headed, preferring flatter ground to break ties. The forward probe
  // is excluded — it is the one that is blocked.
  int best = -1;
  float bestCost = 0;
  for (int i = 1; i < GroundSense::kProbeCount; i++) {
    if (!sense.clear[i]) continue;
    // Signed angular offset of this probe from the nose, in [-pi, pi].
    float off = (6.2831853f * (float)i) / (float)GroundSense::kProbeCount;
    if (off > 3.14159265f) off -= 6.2831853f;
    // Wedged: after several blocked ticks, stop preferring the shallow
    // deflections that clearly are not working and favour a real reversal.
    // Without this a mob in a corner alternates between two near-forward
    // probes forever, which is the classic oscillation this kind of steering
    // fails at.
    const bool wedged = mob.blockedTicks > kCommitTicks * 3;
    float cost = wedged ? -std::abs(off) : std::abs(off);
    cost += 0.15f * (float)std::abs(sense.stepUp[i]);  // prefer flat
    if (best < 0 || cost < bestCost) {
      best = i;
      bestCost = cost;
    }
  }

  if (best < 0) {
    // Boxed in on every probe — reverse. Still a desired heading, so the turn
    // rate still applies and the mob pivots rather than flipping.
    mob.desiredHeading = mob.heading + 3.14159265f;
    mob.lastTurnTick = tick;
    return;
  }
  float off = (6.2831853f * (float)best) / (float)GroundSense::kProbeCount;
  if (off > 3.14159265f) off -= 6.2831853f;
  // Aim BETWEEN the blocked nose and the clear probe rather than exactly at
  // the probe centre: the probes are a coarse 8-way fan and committing to a
  // multiple of 45 degrees would reintroduce the very quantization this change
  // exists to remove. The mob re-senses as it turns, so the heading it settles
  // on is continuous.
  mob.desiredHeading = mob.heading + off * 0.6f;
  mob.lastTurnTick = tick;
}

// ---- locomotion stage 2: steer ---------------------------------------------
// Close heading -> desiredHeading at a bounded, ramped rate. Returns the
// resulting forward-drive alignment factor in 0..1.
float MobSystem::Steer(Mob& mob, const MobDef& def, float dt) {
  const LocomotionDef& lo = def.skel.loco;
  // Shortest signed error, wrapped into [-pi, pi]. Doing this with remainder
  // rather than a while-loop matters: a desiredHeading that has accumulated
  // many turns (it is never normalized) would otherwise spin here.
  float err = std::remainder(mob.desiredHeading - mob.heading, 6.2831853f);

  // Turn tighter when slow, wider when fast — a physical body's trade.
  const float speedFrac =
      std::clamp(mob.speedNow / std::max(def.speed, 0.01f), 0.0f, 1.0f);
  const float rateCap =
      lo.turnRate * (1.0f + (lo.turnRateMoving - 1.0f) * speedFrac);

  if (rateCap <= 0.0f) {
    mob.turnVel = 0;
  } else if (lo.turnAccel <= 0.0f) {
    // No ramp: step straight to the capped rate.
    mob.turnVel = std::clamp(err / std::max(dt, 1e-4f), -rateCap, rateCap);
  } else {
    // Ramp toward the rate that would arrive on target, but never past the cap.
    // The sqrt term is a critically-damped arrival: it is the fastest rate from
    // which the remaining error can still be bled off at turnAccel without
    // overshooting, so the mob decelerates INTO its heading instead of ringing
    // around it.
    const float arrive =
        std::sqrt(2.0f * lo.turnAccel * std::abs(err)) * (err < 0 ? -1.0f : 1.0f);
    const float want = std::clamp(arrive, -rateCap, rateCap);
    const float dv = lo.turnAccel * dt;
    mob.turnVel += std::clamp(want - mob.turnVel, -dv, dv);
  }

  // Never step past the target within one tick: at low dt the ramp handles it,
  // but a large dt (a hitch) would otherwise overshoot and oscillate.
  float step = mob.turnVel * dt;
  if (std::abs(step) > std::abs(err)) {
    step = err;
    mob.turnVel = err / std::max(dt, 1e-4f);
  }
  mob.heading += step;
  // Keep the ACTUATED heading normalized. Intent is left un-normalized on
  // purpose (it is a target, and remainder() above handles the wrap), but
  // `heading` feeds sin/cos every tick for the whole life of the mob and would
  // slowly lose float precision if it drifted to large magnitudes.
  mob.heading = std::remainder(mob.heading, 6.2831853f);

  // Alignment: full drive within driveAlignFull, zero past driveAlignZero.
  const float e = std::abs(std::remainder(mob.desiredHeading - mob.heading,
                                          6.2831853f));
  if (e <= lo.driveAlignFull) return 1.0f;
  if (e >= lo.driveAlignZero) return 0.0f;
  return 1.0f - (e - lo.driveAlignFull) /
                    (lo.driveAlignZero - lo.driveAlignFull);
}

// ---- locomotion stage 3: drive ---------------------------------------------
void MobSystem::DriveLocomotion(Mob& mob, const MobDef& def,
                                const GroundSense& sense, float align,
                                float dt) {
  if (!sense.haveGround) return;  // walk only when footing is known

  // settle feet onto the ground
  mob.origin.y += std::clamp((float)sense.groundY - mob.origin.y, -0.3f, 0.3f);

  // A maimed mob keeps moving, just slower: the active dismemberment state
  // scales the drive speed (a crawl covers ground at a fraction of a walk; a
  // fully disarmed prone state is 0 and goes nowhere). Reads LAST tick's state
  // — UpdateAnimation re-evaluates it — which is at most one tick of lag.
  const float speedScale =
      mob.anim.locoState >= 0 &&
              mob.anim.locoState < (int)def.skel.states.size()
          ? def.skel.states[mob.anim.locoState].speedScale
          : 1.0f;

  // Translate along the ACTUAL facing, scaled by how well it is aligned with
  // intent. A mob mid-turn therefore traces an arc, and one that has to turn
  // around pivots roughly in place — both fall out of this one multiply rather
  // than needing a turn-in-place special case.
  const float drive = def.speed * speedScale * mob.driveScale * align;
  if (drive <= 0.0f) return;
  // Do not walk into a wall we can already feel. The mob keeps turning (Steer
  // ran before this and is unaffected), so it grinds along the obstacle and
  // turns off it rather than freezing against it.
  if (!sense.clear[0]) return;

  const Vec3 fwd{std::sin(mob.heading), 0, std::cos(mob.heading)};
  mob.origin += fwd * (drive * dt);
  mob.phase += drive * dt * 2.2f;  // stride frequency
}

// Procedural gait layer. Writes foot targets into mob.anim.feet and derives
// the body height/tilt from the resulting foot plane; the IK pass in
// UpdateAnimation then places the legs.
void MobSystem::UpdateGait(Mob& mob, const MobDef& def, World& world, float dt) {
  const AnimSkeleton& sk = def.skel;
  const GaitDef& g = sk.gait;
  if (sk.chains.empty()) return;

  Vec3 fwd{std::sin(mob.heading), 0, std::cos(mob.heading)};
  // Scale stride and lift by SPEED so a standing mob's feet are perfectly
  // still — a fixed stride makes idle mobs march in place, which reads as a
  // bug even though every individual formula is right.
  float speedFactor = std::clamp(mob.speedNow / std::max(def.speed, 0.01f),
                                 0.0f, 1.5f);

  // Which gait group (if any) currently has a swinging foot. Exactly ONE
  // group may swing at a time: that single constraint IS the gait state
  // machine. It generalizes to any leg count (two singleton groups = a biped
  // alternating, diagonal pairs = a quadruped trot) with no per-gait table,
  // and it degrades gracefully when a leg is severed — the surviving legs
  // simply take their turns sooner.
  int swingingGroup = -1;
  for (size_t c = 0; c < mob.anim.feet.size(); c++) {
    if (!mob.anim.feet[c].swinging) continue;
    for (size_t gi = 0; gi < g.groups.size(); gi++)
      for (int p : g.groups[gi])
        if (p == sk.chains[c].effector || p == sk.chains[c].parts[0])
          swingingGroup = (int)gi;
    if (swingingGroup < 0) swingingGroup = (int)c;  // ungrouped: its own group
  }

  float sumY = 0;
  int nFeet = 0;
  Vec3 planeAccum{};
  std::vector<Vec3> plantedPts;

  for (size_t c = 0; c < sk.chains.size() && c < mob.anim.feet.size(); c++) {
    const IkChain& ch = sk.chains[c];
    FootState& f = mob.anim.feet[c];
    // limb loss: stop scheduling this leg's steps entirely. The body-from-feet
    // average below then re-centers on the survivors for free.
    bool alive = true;
    for (int p : ch.parts)
      if (p >= 0 && p < (int)mob.anim.partAlive.size() && !mob.anim.partAlive[p])
        alive = false;
    if (!alive) {
      f.valid = false;
      f.swinging = false;
      continue;
    }

    // hip position in world space (rest rig + yaw), the anchor the step is
    // measured from
    Vec3 hipLocal = sk.parts[ch.parts[0]].anchorLocal;
    Vec3 hipWorld = mob.origin + Rotate(AxisAngle({0, 1, 0}, mob.heading),
                                        hipLocal - Vec3{def.worldSize.x * 0.5f,
                                                        0,
                                                        def.worldSize.z * 0.5f}) +
                    Vec3{def.worldSize.x * 0.5f, 0, def.worldSize.z * 0.5f};

    // ideal contact point: under the hip, biased forward, plus velocity
    // lookahead so the foot lands where the body WILL be
    Vec3 ideal = hipWorld + fwd * (g.strideBias * f.legLength * speedFactor) +
                 mob.anim.velocity * g.leadTime;
    int groundY = 0;
    if (GroundHeightAt(world, ifloor(ideal.x), ifloor(ideal.z),
                       ifloor(mob.origin.y) + 3, groundY))
      ideal.y = (float)groundY;
    else
      ideal.y = mob.origin.y;

    if (!f.valid) {  // first frame or leg just came back: plant immediately
      f.valid = true;
      f.planted = ideal;
      f.swinging = false;
    }

    if (f.swinging) {
      f.swingT += dt / std::max(g.stepDuration, 1e-3f);
      if (f.swingT >= 1.0f) {
        f.swingT = 0;
        f.swinging = false;
        f.planted = f.swingTo;
      } else {
        Vec3 flat = f.swingFrom + (f.swingTo - f.swingFrom) * f.swingT;
        // sin(t*pi) arc: zero lift at both ends, peak at mid-swing
        flat.y += g.stepHeight * f.legLength * speedFactor *
                  std::sin(f.swingT * 3.14159265f);
        f.planted = flat;  // `planted` doubles as the current foot target
      }
    } else {
      Vec3 drift = ideal - f.planted;
      float driftLen = std::sqrt(drift.x * drift.x + drift.z * drift.z);
      // unplant only when BOTH conditions hold: this foot has drifted far
      // enough AND no other leg in the same group is mid-swing
      int myGroup = -1;
      for (size_t gi = 0; gi < g.groups.size(); gi++)
        for (int p : g.groups[gi])
          if (p == ch.effector || p == ch.parts[0]) myGroup = (int)gi;
      bool groupFree = swingingGroup < 0 || swingingGroup == myGroup;
      if (driftLen > g.stepThreshold * f.legLength && groupFree) {
        f.swinging = true;
        f.swingT = 0;
        f.swingFrom = f.planted;
        f.swingTo = ideal;
        swingingGroup = myGroup >= 0 ? myGroup : (int)c;
      }
      sumY += f.planted.y;
      nFeet++;
      plantedPts.push_back(f.planted);
    }
  }

  // ---- body from feet ----
  // Body height and tilt are DERIVED from where the feet actually are, so a
  // mob walking up a voxel staircase leans and rises correctly without a
  // single line of slope-handling code.
  if (nFeet > 0) {
    // bodyY is the prefab MIN CORNER (origin.y's frame), so the foot average
    // has to be converted out of the sole's frame: subtracting the rig's rest
    // sole height puts the corner where the authored rest pose would stand.
    //
    // `rideHeight` is then a CROUCH/STRETCH about that rest stance, not an
    // absolute lift: 1.0 stands at the authored height, lower squats, higher
    // reaches. Written as (rideHeight - 1) so the default rig sits exactly on
    // its own art rather than a leg-length above it — the raw
    // `footAvg + rideHeight*legLength` treated a min corner as a hip and is
    // the other half of why mobs hovered.
    float stance = (g.rideHeight - 1.0f) * mob.anim.feet[0].legLength;
    float targetY = sumY / (float)nFeet - mob.restSoleY + stance;
    if (!mob.footInit) {
      mob.bodyY = targetY;
      mob.footInit = true;
    } else {
      mob.bodyY += std::clamp(targetY - mob.bodyY, -0.4f, 0.4f);
    }
  } else {
    mob.bodyY += std::clamp(mob.origin.y - mob.bodyY, -0.4f, 0.4f);
    mob.footInit = true;
  }
  // Body TILT from the foot-plane normal — same idea as the height: the mob
  // leans to match the ground it is actually standing on, derived, not coded.
  Vec3 targetUp{0, 1, 0};
  if (plantedPts.size() >= 3) {
    // Newell's method: a robust plane normal from any polygon of contact
    // points (handles non-planar and near-degenerate foot sets gracefully,
    // unlike a single cross product of three arbitrary feet).
    Vec3 n{};
    for (size_t i = 0; i < plantedPts.size(); i++) {
      const Vec3& a = plantedPts[i];
      const Vec3& b = plantedPts[(i + 1) % plantedPts.size()];
      n.x += (a.y - b.y) * (a.z + b.z);
      n.y += (a.z - b.z) * (a.x + b.x);
      n.z += (a.x - b.x) * (a.y + b.y);
    }
    if (n.y < 0) n = n * -1.0f;
    Vec3 up = n.normalized();
    // cap the lean: a foot on a 1-voxel ledge shouldn't tip the mob over
    if (up.len() > 0.5f && up.y > 0.6f) targetUp = up;
  }
  // ease toward the target normal so stepping onto a new block doesn't snap
  mob.bodyUp = (mob.bodyUp * 0.85f + targetUp * 0.15f).normalized();
  if (mob.bodyUp.len() < 0.5f) mob.bodyUp = {0, 1, 0};
}

void MobSystem::UpdateAnimation(Mob& mob, const MobDef& def, World& world,
                                float dt) {
  const AnimSkeleton& sk = def.skel;
  AnimState& st = mob.anim;
  if (sk.parts.empty()) return;
  st.partAlive.resize(sk.parts.size(), 1);
  st.springs.resize(sk.parts.size(), SpringState{});

  // measured velocity drives everything speed-scaled downstream
  Vec3 delta = mob.origin - st.lastPos;
  st.lastPos = mob.origin;
  Vec3 planar{delta.x / std::max(dt, 1e-4f), 0, delta.z / std::max(dt, 1e-4f)};
  st.velocity = st.velocity * 0.7f + planar * 0.3f;   // smooth out tick noise
  mob.speedNow = Vec3{st.velocity.x, 0, st.velocity.z}.len();
  float speedFactor = std::clamp(mob.speedNow / std::max(def.speed, 0.01f),
                                 0.0f, 1.5f);

  const GaitDef& g = sk.gait;
  st.gaitPhase += dt * (g.present ? g.cadence : 2.2f) * speedFactor;
  if (st.gaitPhase > 1.0f) st.gaitPhase -= std::floor(st.gaitPhase);
  mob.phase = st.gaitPhase * 6.2831853f;

  // ---- dismemberment locomotion states ----
  // Re-evaluated every frame rather than only on Sever: partAlive changes in
  // several places (Sever, recursive DetachLimb, Die), and this poll is a few
  // comparisons against a handful of rules. On a transition the outgoing
  // state's loco clip blends out (the runtime's stopping fade) while the new
  // one blends in over its own blendInMs — a crossfade for free.
  {
    int want = AnimSelectState(sk, st);
    if (want != st.locoState) {
      if (st.locoState >= 0 && !sk.states[st.locoState].clip.empty()) {
        int old = sk.FindClip(sk.states[st.locoState].clip);
        for (ClipInstance& inst : st.clips)
          if (inst.clip == old) inst.stopping = true;
      }
      st.locoState = want;
      if (want >= 0) {
        const AnimStateRule& rule = sk.states[want];
        if (!rule.clip.empty()) PlayClip(mob, def, rule.clip);
        // A foot frozen mid-swing would report "swinging" forever once the
        // gait stops running; land everything where it stands.
        if (rule.disableGait)
          for (FootState& f : st.feet) f.swinging = false;
      }
    }
  }
  const AnimStateRule* loco =
      st.locoState >= 0 ? &sk.states[st.locoState] : nullptr;
  const bool clipOwnsPose = loco && loco->disableGait;

  // ---- stages 1-3: sample active clips, blend, apply additives ----
  AnimSampleAndBlend(sk, st, dt);

  // ---- procedural layer: legacy phase swing + pelvis bob/sway/spine ----
  // dummy.json has swingAmp/swingPhase and no chains; running it HERE rather
  // than as a separate code path means the fallback and the new rig share one
  // pipeline (flatten, IK, physics blend all behave identically).
  // Suppressed entirely while a disableGait loco state is active: a crawl clip
  // keys the same parts the walk swing and pelvis bob drive, and "authored
  // crawl plus leftover walk bounce" reads as a glitch, not a blend.
  for (size_t i = 0; !clipOwnsPose && i < sk.parts.size(); i++) {
    const AnimPart& p = sk.parts[i];
    if (p.swingAmp == 0) continue;
    // progressive phase lag up the hierarchy: each level down the chain
    // trails its parent slightly, which is what makes a walk look like it
    // propagates through the body instead of moving as one rigid piece.
    int depth = 0;
    for (int k = p.parent; k >= 0; k = sk.parts[k].parent) depth++;
    float lag = g.present ? g.phaseLag * (float)depth * 6.2831853f : 0.0f;
    float swing = p.swingAmp * std::sin(mob.phase - lag + p.swingPhase * 3.14159265f);
    // when a gait is present the legs are IK-driven, so the swing only
    // survives on parts no chain owns (arms, head)
    bool inChain = false;
    for (const IkChain& ch : sk.chains)
      for (int cp : ch.parts) inChain |= (cp == (int)i);
    if (inChain) continue;
    st.local[i].rot = QuatNormalize(QuatMul(st.local[i].rot,
                                            QuatAxisAngle(p.axis, swing)));
  }
  if (g.present && !clipOwnsPose && def.rootLimb >= 0 &&
      def.rootLimb < (int)sk.parts.size()) {
    Transform& root = st.local[def.rootLimb];
    // Pelvis bob runs at 2x step frequency: the body rises once per FOOTFALL,
    // and there are two footfalls per full stride cycle.
    root.pos.y += g.bobAmp * std::sin(g.bobFreqMul * 6.2831853f * st.gaitPhase) *
                  speedFactor;
    // sway and roll at 1x (once per stride, left then right)
    root.pos.x += g.swayAmp * std::sin(6.2831853f * st.gaitPhase) * speedFactor;
    float roll = g.rollAmp * std::sin(6.2831853f * st.gaitPhase) * speedFactor;
    root.rot = QuatNormalize(QuatMul(root.rot, QuatAxisAngle({0, 0, 1}, roll)));
    // spine counter-rotation: the chest turns against the hips
    for (size_t i = 0; i < sk.parts.size(); i++) {
      if (sk.parts[i].tag != "spine") continue;
      float yawCounter = -g.spineCounter * roll;
      st.local[i].rot =
          QuatNormalize(QuatMul(st.local[i].rot, QuatAxisAngle({0, 1, 0}, yawCounter)));
    }
  }

  // ---- springs: parts with `spring` are jiggled, never keyed ----
  // The goal is the body's own motion expressed in the part's local frame, so
  // a tail lags behind acceleration and settles when the mob stops.
  //
  // Velocity is NORMALIZED by the def's own top speed, exactly as avatar.cpp
  // does it, so `gain` means "radians of lag at full speed" for every def
  // regardless of how fast that def moves. Against raw voxels/second the same
  // authored gain reads completely differently on a slow critter and on a
  // 60-voxel/s player avatar — the latter sat pegged at maxAngle permanently.
  // Both drivers must agree here or a def's springs change meaning depending on
  // which one is animating it.
  const float speedRef = std::max(def.speed, 0.01f);
  for (size_t i = 0; i < sk.parts.size(); i++) {
    const AnimPart& p = sk.parts[i];
    if (!p.hasSpring) continue;
    Vec3 goal{-st.velocity.z / speedRef * p.spring.gain * kSpringVelScale, 0,
              st.velocity.x / speedRef * p.spring.gain * kSpringVelScale};
    goal.x = std::clamp(goal.x, -p.spring.maxAngle, p.spring.maxAngle);
    goal.z = std::clamp(goal.z, -p.spring.maxAngle, p.spring.maxAngle);
    AnimSpringStep(p.spring, st.springs[i], goal, dt);
    const Vec3& s = st.springs[i].x;
    Quat jiggle = QuatMul(QuatAxisAngle({1, 0, 0}, s.x),
                          QuatMul(QuatAxisAngle({0, 1, 0}, s.y),
                                  QuatAxisAngle({0, 0, 1}, s.z)));
    st.local[i].rot = QuatNormalize(QuatMul(st.local[i].rot, jiggle));
  }

  // ---- stage 4: flatten to model space ----
  AnimFlatten(sk, st);

  // ---- gait + stage 5: IK, strictly a POST-PROCESS on the flattened pose ----
  // IK must never be a blended layer: blending two IK results produces a pose
  // that satisfies neither end-effector constraint, which defeats the point.
  const bool gaitActive = g.present && !clipOwnsPose;
  if (gaitActive) {
    UpdateGait(mob, def, world, dt);
  } else {
    // No foot plane is being maintained (legacy rig, or a loco clip owns the
    // pose): the animated body height follows the walk drive's ground contact
    // plus the state's authored offset, and the slope tilt eases back flat.
    // This is the same settle UpdateGait applies when every foot is lost.
    float targetY = mob.origin.y + (loco ? loco->bodyYOffset : 0.0f);
    mob.bodyY += std::clamp(targetY - mob.bodyY, -0.4f, 0.4f);
    mob.footInit = true;
    mob.bodyUp = (mob.bodyUp * 0.85f + Vec3{0, 1, 0} * 0.15f).normalized();
    if (mob.bodyUp.len() < 0.5f) mob.bodyUp = {0, 1, 0};
  }
  if (!sk.chains.empty() && gaitActive) {
    Quat yaw = AxisAngle({0, 1, 0}, mob.heading);
    Vec3 pivot{def.worldSize.x * 0.5f, 0, def.worldSize.z * 0.5f};
    Vec3 bodyOrigin{mob.origin.x, mob.bodyY, mob.origin.z};
    for (size_t c = 0; c < sk.chains.size() && c < st.feet.size(); c++) {
      const FootState& f = st.feet[c];
      float weight = f.valid ? sk.chains[c].weight : 0.0f;  // limb loss -> 0
      if (weight <= 0) continue;
      // world foot target -> model space (inverse of the submit transform)
      Vec3 rel = f.planted - bodyOrigin - pivot;
      Vec3 prefabPt = RotateInv(yaw, rel) + pivot;
      // PREFAB-ABSOLUTE ON BOTH ENDS — nothing is rebased here. AnimFlatten
      // seeds the root from its own rest.pos, which IS rootAnchor, so the hip
      // AnimSolveTwoBone reads out of st.model[] already carries the root
      // offset. Subtracting it from the target alone put the two ends in
      // different frames and skewed (target - root) by exactly rootAnchor,
      // which on a tall-hipped rig over-reaches the leg into its clamp and
      // splays both legs the same way instead of mirroring. Matches the submit
      // path below, which likewise does not re-add rootAnchor to modelPos.
      AnimSolveTwoBone(sk, st, sk.chains[c], prefabPt, weight);
    }
  }

  // ---- flipbooks: integer frame index from elapsed ms ----
  if (st.flipbook.book >= 0 && st.flipbook.book < (int)sk.flipbooks.size()) {
    const Flipbook& fb = sk.flipbooks[st.flipbook.book];
    st.flipbook.elapsedMs += (int32_t)std::lround(dt * 1000.0);
    int fr = AnimFlipbookFrame(fb, st.flipbook.elapsedMs);
    if (fr != st.flipbook.frame) {
      st.flipbook.frame = fr;
      // re-point the affected limb at the frame's model; instances rebuild
      for (const FlipbookFrame& ff : fb.frames) {
        if (ff.part < 0 || ff.part >= (int)mob.limbs.size()) continue;
        mob.limbs[ff.part].flipbookModel = -1;
      }
      if (fr >= 0 && fr < (int)fb.frames.size()) {
        const FlipbookFrame& ff = fb.frames[fr];
        if (ff.part >= 0 && ff.part < (int)mob.limbs.size()) {
          mob.limbs[ff.part].flipbookModel = ff.model;
          instancesDirty_ = true;   // bounded: only on an actual frame change
        }
      }
    }
    if (!fb.loop && st.flipbook.frame == (int)fb.frames.size() - 1)
      st.flipbook.book = -1;
  }
}

void MobSystem::PreTick(uint32_t tick, World& world, std::vector<BrushOp>& ops,
                        std::vector<ParticleSpawn>& spawns) {
  const float dt = 1.0f / 30.0f;
  IVec3 wo = world.WindowOrigin();
  Vec3 wlo{(float)(wo.x * (int)kChunk), (float)(wo.y * (int)kChunk),
           (float)(wo.z * (int)kChunk)};
  int bleedOps = 0;

  // Rebuilt from scratch each tick: a wound that stopped bleeding, or a mob
  // that despawned, simply stops appearing, and the audio layer reaps the
  // loop. Nothing has to remember to remove an entry.
  bleeds_.clear();

  // Drain particles authored since the last tick (dismemberment blood voxels).
  // Dropped rather than carried over when the tick's budget is already full:
  // stale gore arriving a tick late would spawn from a wound that has since
  // moved, and the burst it belongs to is over by then anyway.
  for (const ParticleSpawn& s : pendingSpawns_) {
    if (spawns.size() >= kMaxParticleSpawnsPerTick) break;
    IVec3 c{s.px >> 8, s.py >> 8, s.pz >> 8};
    if (!world.CellInWindow(c)) continue;
    spawns.push_back(s);
  }
  pendingSpawns_.clear();

  for (size_t mi = 0; mi < mobs_.size();) {
    Mob& mob = mobs_[mi];
    const MobDef& def = defs_[mob.defIndex];

    // Pending sever holds tick down first, wherever the mob is in its life:
    // a piece left kinematic and unowned would never sleep (rule #2).
    for (Limb& limb : mob.limbs) {
      if (!limb.holdBody) continue;
      limb.holdSeconds -= dt;
      if (limb.holdSeconds > 0) continue;
      limb.holdSeconds = 0;
      phys_->SetBodyKinematic(limb.holdBody, false);
      // A severed part must collide with the body it came off again: the
      // mob's GroupFilterTable suppressed those contacts forever otherwise.
      phys_->ClearCollisionGroup(limb.holdBody);
      limb.holdBody = 0;
    }

    // corpses hand their bodies to DebrisSystem in Die(); drop the husk once
    // no limb is still holding a pose
    if (!mob.alive) {
      bool holding = false;
      for (const Limb& l : mob.limbs) holding |= l.holdBody != 0;
      if (!holding) {
        mobs_[mi] = std::move(mobs_.back());
        mobs_.pop_back();
        continue;
      }
      mi++;
      continue;
    }

    // despawn when the window moves away (same rule as debris)
    const float kPad = 16.0f;
    bool out = mob.origin.x < wlo.x - kPad || mob.origin.y < wlo.y - kPad ||
               mob.origin.z < wlo.z - kPad ||
               mob.origin.x > wlo.x + (float)kWorldN + kPad ||
               mob.origin.y > wlo.y + (float)kWorldN + kPad ||
               mob.origin.z > wlo.z + (float)kWorldN + kPad;
    if (out) {
      for (Limb& l : mob.limbs) {
        // a held piece belongs to DebrisSystem already — let its own culling
        // take it, just release the kinematic hold
        if (l.holdBody) {
          phys_->SetBodyKinematic(l.holdBody, false);
          phys_->ClearCollisionGroup(l.holdBody);
          l.holdBody = 0;
        }
        ReleaseLimbMicro(l);  // carved limbs own their brick — return it
        if (l.body) phys_->RemoveBody(l.body);
      }
      mobs_[mi] = std::move(mobs_.back());
      mobs_.pop_back();
      instancesDirty_ = true;
      continue;
    }

    // terrain collision anchors for every live limb (ManageTerrain sweep)
    float mobRadius = 0.5f * std::sqrt(def.worldSize.x * def.worldSize.x +
                                        def.worldSize.y * def.worldSize.y +
                                        def.worldSize.z * def.worldSize.z);
    debris_->AddTerrainAnchor(mob.origin + Vec3{def.worldSize.x * 0.5f,
                                                def.worldSize.y * 0.5f,
                                                def.worldSize.z * 0.5f},
                              mobRadius);

    if (mob.alive) {
      // ---- locomotion: sense -> intent -> steer -> drive ----
      // Four stages with one direction of data flow. Only DecideIntent has an
      // opinion about where to go; only Steer may move `heading`, and it does
      // so at a bounded rate; only DriveLocomotion translates, and it does so
      // along the ACTUAL facing. That last point is what turns a heading
      // change into a curved path instead of an instant change of direction.
      GroundSense sense = SenseGround(mob, def, world);
      DecideIntent(mob, def, sense, tick, dt);
      float align = Steer(mob, def, dt);
      DriveLocomotion(mob, def, sense, align, dt);

      // ---- stages 1-5: pose the rig (float presentation state) ----
      UpdateAnimation(mob, def, world, dt);

      // ---- stages 6-7: model space -> world, submit to Jolt ----
      // Yaw about the mob's center column, then the animated model pose on
      // top. Body Y comes from the foot plane (UpdateGait), not the raw
      // ground probe, which is what makes slopes work with no slope code.
      Quat yaw = AxisAngle({0, 1, 0}, mob.heading);
      // Tilt the whole body to the foot-plane normal, again free from gait.
      Quat tilt = QuatFromTo({0, 1, 0}, mob.bodyUp);
      Quat bodyRot = Mul(tilt, yaw);
      // The yaw pivot is the footprint centre in PREFAB coordinates — it is a
      // property of the def, not of where the mob currently stands, so it is
      // written from worldSize directly rather than differencing a world-space
      // centre against the origin the drive step just moved.
      Vec3 yawPivot{def.worldSize.x * 0.5f, 0, def.worldSize.z * 0.5f};
      Vec3 bodyOrigin{mob.origin.x, mob.bodyY, mob.origin.z};
      for (size_t i = 0; i < mob.limbs.size(); i++) {
        Limb& limb = mob.limbs[i];
        if (!limb.body) continue;
        // a severed part in its hold window keeps its last pose and is not
        // re-driven; the countdown lives in the sweep below
        if (limb.holdSeconds > 0) continue;
        Quat local = i < mob.anim.model.size() ? mob.anim.model[i].rot : Quat{};
        Vec3 modelPos =
            i < mob.anim.model.size() ? mob.anim.model[i].pos : Vec3{};
        Quat rot = QuatNormalize(Mul(bodyRot, local));
        // modelPos is ALREADY in prefab coordinates: AnimFlatten seeds the root
        // from its rest.pos, which IS rootAnchor, so every part's model pos
        // carries the root offset. Adding rootAnchor again lifted the whole rig
        // by the root anchor — on a biped that is the hip height (6.5 world
        // voxels on mina), and it is half of why mobs hovered. avatar.cpp fixed
        // this on its own submit and the mob path never got the same fix.
        Vec3 anchorW = bodyOrigin + yawPivot +
                       Rotate(bodyRot, modelPos - yawPivot);
        Vec3 pos = anchorW - Rotate(rot, limb.anchorLimb);
        float q[4] = {rot.x, rot.y, rot.z, rot.w};
        phys_->MoveKinematicBody(limb.body, pos, q, dt);
      }
    }

    // ---- bleeding (PLAN §B5): decaying wound budget, bounded ops ----
    if (def.bleedMat != 0) {
      const auto& gore = CurrentTuning().gore;
      // Rest-pose prefab offset -> world, for limbs with no physics body left.
      // Recomputed here rather than reusing the walk block's bodyRot because
      // that is scoped to live mobs, and a corpse still bleeds.
      //
      // Only the yaw and the animated body height matter for placing a wound;
      // the foot-plane tilt is deliberately omitted, since a dead mob has no
      // maintained foot plane and a stale one would swing the spray sideways.
      const Quat bodyRotNow = AxisAngle({0, 1, 0}, mob.heading);
      const Vec3 bodyOriginNow{mob.origin.x, mob.bodyY, mob.origin.z};
      auto bodyFrame = [&](Vec3 prefabOffset) {
        return bodyOriginNow + Rotate(bodyRotNow, prefabOffset);
      };
      for (size_t li = 0; li < mob.limbs.size(); li++) {
        Limb& limb = mob.limbs[li];
        Quat lq{limb.xf.quat[0], limb.xf.quat[1], limb.xf.quat[2],
                limb.xf.quat[3]};

        // ---- the dismemberment gout ----
        // Front-loaded: emission is proportional to the REMAINING countdown, so
        // the first ticks after the cut throw the bulk of it and the tail
        // thins out. Runs on its own schedule (every tick, not the drip's every
        // 4th) because a burst that stutters at 7.5 Hz reads as a pump.
        if (limb.gushTicks > 0) {
          int decay = std::max(1, mob.gore.severDecayTicks);
          // Triangular weighting: sum over the window of (2*total/decay) *
          // (k/decay) for k = decay..1 is ~= total, so severSpray is the actual
          // droplet count released rather than a rate to be multiplied out.
          float frac = (float)limb.gushTicks / (float)decay;
          int want = (int)std::lround(2.0f * (float)mob.gore.severSpray * frac /
                                      (float)decay);
          // Bodyless fallback goes through bodyFrame(), not mob.origin +
          // anchorRoot: origin.y is the spawn corner, so the raw offset puts
          // the wound at the mob's feet instead of at the joint, and it also
          // drops the heading rotation. Same reasoning as the fix in Sever().
          Vec3 origin = limb.body ? limb.xf.pos + Rotate(lq, limb.gushLocal)
                                  : bodyFrame(limb.anchorRoot);
          Vec3 axis = limb.body ? Rotate(lq, limb.gushDir)
                                : Rotate(bodyRotNow, limb.gushDir);
          for (int k = 0; k < want; k++) {
            if (spawns.size() >= kMaxParticleSpawnsPerTick) break;
            uint32_t h = Hash3((uint32_t)mob.id * 2654435761u + (uint32_t)li,
                               tick, (uint32_t)k * 0x9E3779B9u);
            // Event-scoped variance re-rolls per droplet; entity-scoped values
            // already landed in mob.gore and pass through untouched.
            const uint32_t es = (uint32_t)mob.id ^ ((uint32_t)li << 16);
            float cone = EventVar(mob.gore.severSprayCone, gore.severSprayConeVar,
                                  es, tick, (uint32_t)k * 3u + 0u);
            Vec3 dir{axis.x + SignedUnit(h) * cone,
                     axis.y + SignedUnit(Pcg(h ^ 0x51A17u)) * cone,
                     axis.z + SignedUnit(Pcg(h ^ 0xB0011u)) * cone};
            // speed varies +-25% so the jet has depth instead of a hard front
            float sp = EventVar(mob.gore.severSpraySpeed,
                                gore.severSpraySpeedVar, es, tick,
                                (uint32_t)k * 3u + 1u) *
                       (0.75f + 0.5f * (float)(Pcg(h ^ 0x1234u) & 0xFFFFu) / 65535.0f);
            if (sp < 0.0f) sp = 0.0f;
            int life = EventVarI(mob.gore.microLifeTicks, gore.microLifeTicksVar,
                                 es, tick, (uint32_t)k * 3u + 2u);
            life = life < 1 ? 1 : (life > 255 ? 255 : life);
            if (!world.CellInWindow({ifloor(origin.x), ifloor(origin.y),
                                     ifloor(origin.z)}))
              break;
            spawns.push_back(MakeDroplet(origin, dir * sp, def.bleedMat, true,
                                         life, gore.microScale));
          }
          limb.gushTicks--;
        }

        // Report the wound for audio BEFORE the budget/op-rate early-outs
        // below. Those exist to bound how much MATTER enters the CA per tick;
        // a wound that is out of drip ops this tick is still bleeding, and
        // gating the sound on them would make the loop stutter with the drip
        // rate instead of tracking the wound.
        if (limb.bleedBudget >= 1.0f) {
          const float cap = std::max(1.0f, gore.bleedBudgetCap);
          Vec3 wpos = limb.body
                          ? limb.xf.pos + Rotate(lq, limb.woundLocal)
                          : bodyFrame(limb.anchorRoot);
          // Key on (mob, limb) so one loop follows one wound across ticks
          // even as limbs are severed and the vector is reshuffled.
          bleeds_.push_back(BleedSource{
              wpos, (mob.id << 8) ^ (uint64_t)(li + 1),
              std::clamp(limb.bleedBudget / cap, 0.0f, 1.0f)});
        }

        if (limb.bleedBudget < 1.0f || bleedOps >= gore.bleedOpsPerTick) continue;
        // Charge the clump BEFORE emitting it, and shrink it to what the wound
        // can still afford. The natural ordering (emit, then subtract) lets the
        // last drip overrun bleedBudgetCap by nearly a whole sphere — 123
        // voxels at radius 3 — which is exactly the rule 2 trap in CLAUDE.md.
        // Shrinking rather than refusing keeps a wound's final voxels flowing
        // instead of stranding a sub-clump remainder that never drips.
        int clumpR = gore.bleedClumpRadius;
        while (clumpR > 0 &&
               (float)BleedClumpVoxels(clumpR) > limb.bleedBudget)
          clumpR--;
        // Drip period, tunable. Modulo rather than a mask because the tuner
        // offers every period, not just powers of two; the divisor is clamped
        // >= 1 at load so this cannot divide by zero.
        if (tick % (uint32_t)std::max(1, gore.bleedDripTicks) != 0) continue;
        Vec3 w = limb.body
                     ? limb.xf.pos + Rotate(lq, limb.woundLocal)
                     : bodyFrame(limb.anchorRoot);  // stump on the parent
        // Clump size is a brush radius, and a BrushOp paints a SOLID SPHERE, so
        // the budget is debited by the volume actually painted (1/7/33/123) and
        // not by 1. Charging the real cost is what keeps bleedBudgetCap a true
        // bound on matter entering the CA once clump size leaves 0 (rule 2).
        ops.push_back({ifloor(w.x), ifloor(w.y), ifloor(w.z), clumpR,
                       def.bleedMat, 0 /*paint into air*/, 0, 0});
        limb.bleedBudget -= (float)BleedClumpVoxels(clumpR);
        bleedOps++;

        // ---- the spray that accompanies the drip ----
        // Rides the drip's existing budget rather than carrying its own: the
        // drip rate is already bounded and already tied to the wound, so spray
        // per drip cannot outrun the bleeding it is depicting.
        const uint32_t es = (uint32_t)mob.id ^ ((uint32_t)li << 16);
        // The drip's spray count. Entity scope makes this mob a heavy bleeder
        // for life; event scope varies it drip to drip.
        int sprayN = (int)std::lround(
            EventVar(mob.gore.bleedSprayPerDrip, gore.bleedSprayPerDripVar, es,
                     tick, 0u));
        if (sprayN < 0) sprayN = 0;
        for (int k = 0; k < sprayN; k++) {
          if (spawns.size() >= kMaxParticleSpawnsPerTick) break;
          uint32_t h = Hash3((uint32_t)mob.id * 40503u + (uint32_t)li,
                             tick ^ 0xB1005u, (uint32_t)k * 2246822519u);
          float cone = EventVar(mob.gore.bleedSprayCone, gore.bleedSprayConeVar,
                                es, tick, (uint32_t)k * 3u + 1u);
          // biased upward and outward: a wound sprays, it does not just drool
          Vec3 dir{SignedUnit(h) * cone,
                   0.6f + 0.4f * std::fabs(SignedUnit(Pcg(h ^ 0x77u))),
                   SignedUnit(Pcg(h ^ 0xC0FFEEu)) * cone};
          float sp = EventVar(mob.gore.bleedSpraySpeed, gore.bleedSpraySpeedVar,
                              es, tick, (uint32_t)k * 3u + 2u);
          if (sp < 0.0f) sp = 0.0f;
          int life = EventVarI(mob.gore.microLifeTicks, gore.microLifeTicksVar,
                               es, tick, (uint32_t)k * 3u + 3u);
          life = life < 1 ? 1 : (life > 255 ? 255 : life);
          spawns.push_back(MakeDroplet(w, dir * sp, def.bleedMat, true, life,
                                       gore.microScale));
        }
      }
    }
    mi++;
  }
}

void MobSystem::PostStep() {
  for (Mob& mob : mobs_)
    for (Limb& limb : mob.limbs)
      if (limb.body) phys_->GetTransform(limb.body, limb.xf);
}

bool MobSystem::FindLimb(uint64_t bodyHandle, uint64_t& mobId,
                         int& limbIndex) const {
  for (const Mob& mob : mobs_)
    for (size_t i = 0; i < mob.limbs.size(); i++)
      if (mob.limbs[i].body == bodyHandle) {
        mobId = mob.id;
        limbIndex = (int)i;
        return true;
      }
  return false;
}

bool MobSystem::Damage(uint64_t bodyHandle, float amount, Vec3 hitWorldVoxel,
                       float impactSpeed) {
  for (Mob& mob : mobs_) {
    for (size_t i = 0; i < mob.limbs.size(); i++) {
      Limb& limb = mob.limbs[i];
      if (limb.body != bodyHandle) continue;
      const MobDef& def = defs_[mob.defIndex];
      const MobLimbDef& ld = def.limbs[i];
      Quat q{limb.xf.quat[0], limb.xf.quat[1], limb.xf.quat[2], limb.xf.quat[3]};
      // beam crossing the joint anchor severs outright (PLAN §C2)
      if ((int)i != def.rootLimb && limb.joint) {
        Vec3 anchorW = limb.xf.pos + Rotate(q, limb.anchorLimb);
        if ((hitWorldVoxel - anchorW).len() < 1.75f) {
          Sever(mob.id, (int)i);
          return true;
        }
      }
      // Second sever threshold: a fast enough impact takes the limb off no
      // matter how much hp is left. severImpactSpeed == 0 means "absent" =
      // infinite, so unconfigured rigs behave exactly as before.
      bool impactSevers =
          ld.severImpactSpeed > 0 && impactSpeed >= ld.severImpactSpeed;
      limb.hp -= amount;
      limb.woundLocal = RotateInv(q, hitWorldVoxel - limb.xf.pos);
      limb.bleedBudget =
          AddBleedBudget(limb.bleedBudget, amount * def.bleedPerDamage);
      if (limb.hp <= 0 || impactSevers) {
        Sever(mob.id, (int)i);
      } else {
        // non-fatal hit: flinch. This is the one wired trigger for now — it
        // exercises the whole clip layer (sample/blend/mask/blend-out).
        PlayClip(mob, def, "attack");
      }
      return true;
    }
  }
  return false;
}

// ---- per-voxel carving of LIVE limbs ---------------------------------------
//
// The live-limb twin of the DebrisSystem::DamageBody family. The two are
// deliberately parallel rather than shared: a debris body answers to nobody, so
// carving it is "erase, re-skin, rebuild, maybe split", while a limb is held in
// a joint chain and driven by an animation rig, so the same carve additionally
// has to keep the rig's anchors, the parent/child joints and the loco states
// agreeing with the new geometry. Trying to serve both from one function would
// mean threading a rig-shaped callback through the debris path for the benefit
// of its one non-debris caller.
//
// What is genuinely shared is the part that must not diverge: the micro
// copy-on-write brick (sim/microbody.h) is the SAME pool with the same
// ownership rules, so a limb carved here and the same limb carved after it is
// severed behave identically — which is the property that makes "cut the arm
// off, then keep cutting the arm" work without a special case.

void MobSystem::LimbVoxelsToParticles(const Limb& limb, uint32_t physScale,
                                      const std::vector<DebrisVoxel>& voxels,
                                      World& world,
                                      std::vector<ParticleSpawn>& spawns) const {
  // Mirrors DebrisSystem::VoxelsToParticles: a micro limb's COLLIDER voxels are
  // 1/physScale of a world voxel, so one particle per voxel would emit
  // `physScale^3` times the matter the limb actually lost. Sub-sampling on that
  // lattice conserves the visible volume — the grid has no sub-voxel resolution
  // to receive the detail anyway.
  const float inv = 1.0f / (float)std::max(1u, physScale);
  const int step = (int)std::max(1u, physScale);
  Quat q{limb.xf.quat[0], limb.xf.quat[1], limb.xf.quat[2], limb.xf.quat[3]};
  for (const DebrisVoxel& v : voxels) {
    if (spawns.size() >= kMaxParticleSpawnsPerTick) return;  // ring full: lost
    if (step > 1 && (((int)v.x % step) || ((int)v.y % step) || ((int)v.z % step)))
      continue;
    Vec3 wp = limb.xf.pos + Rotate(q, Vec3{((float)v.x + 0.5f) * inv,
                                           ((float)v.y + 0.5f) * inv,
                                           ((float)v.z + 0.5f) * inv});
    if (!world.CellInWindow({ifloor(wp.x), ifloor(wp.y), ifloor(wp.z)})) continue;
    // A limb is kinematic while alive, so Jolt's velocity is the animation
    // drive, not something to inherit — flesh knocked off a walking mob should
    // fall away from the wound, not sail off at walk speed. The outward push
    // comes from the carve site instead (the caller's ejection direction).
    ParticleSpawn s;
    s.px = (int32_t)std::lround(wp.x * 256.0f);
    s.py = (int32_t)std::lround(wp.y * 256.0f);
    s.pz = (int32_t)std::lround(wp.z * 256.0f);
    Vec3 out = wp - limb.xf.pos;
    float len = out.len();
    out = len > 1e-3f ? out * (1.0f / len) : Vec3{0, 1, 0};
    out.y += 0.6f;  // loft, so gobbets arc instead of skidding along the floor
    const float sp = 3.0f;
    s.vx = (int32_t)std::lround(out.x * sp * 256.0f / 30.0f);
    s.vy = (int32_t)std::lround(out.y * sp * 256.0f / 30.0f);
    s.vz = (int32_t)std::lround(out.z * sp * 256.0f / 30.0f);
    s.payload = v.payload;
    s.flags = 1u;  // PFLAG_ALIVE
    spawns.push_back(s);
  }
}

void MobSystem::ReleaseLimbMicro(Limb& limb) {
  // Only a CARVED limb owns its brick; an intact one points at the def's shared
  // model, which every other instance of that mob is also using. MicroBodyFree
  // ignores shared models, but the `carved` gate makes the intent explicit at
  // the call site rather than relying on that.
  if (limb.carved && limb.microModel >= 0 && microSet_)
    MicroBodyFree(*microSet_, (uint32_t)limb.microModel);
  limb.carved = false;
}

bool MobSystem::ReskinLimbMicro(Mob& mob, Limb& limb, uint32_t skinScale,
                                uint32_t physScale) {
  if (limb.microModel < 0 || !microSet_) return false;
  int own = MicroBodyOwn(*microSet_, (uint32_t)limb.microModel);
  if (own < 0) return false;  // pool full: keep the stale skin, stay carved
  limb.microModel = own;
  limb.carved = true;
  // The brick is packed from the SKIN when there is one, and from the collider
  // voxels when the two lattices coincide — the same rule debris.cpp
  // ReskinMicro follows, because whatever this reads is what the player sees.
  std::vector<PrefabVoxel> mv;
  const bool fine = limb.HasFineSkin();
  if (fine) {
    mv = limb.skinVoxels;
  } else {
    mv.reserve(limb.voxels.size());
    for (const DebrisVoxel& v : limb.voxels)
      mv.push_back({(int16_t)v.x, (int16_t)v.y, (int16_t)v.z,
                    (uint16_t)(v.payload & 0xFFF)});
  }
  IVec3 shift{};
  if (!MicroBodyEdit(*microSet_, (uint32_t)limb.microModel, mv, shift))
    return false;
  if (shift.x || shift.y || shift.z) {
    // MicroBodyEdit rebased the brick to its own min corner. For a debris body
    // that means moving the transform; for a LIMB it means moving the transform
    // AND the rig offsets that derive from the limb origin, because the
    // animation pipeline re-poses this limb from restOffset every single frame.
    // Move only the transform and the next frame's pose puts it straight back
    // where it was, undoing the shift and sliding the art off the collider —
    // the wound would appear to crawl along the limb as it was carved.
    //
    // The shift is in SKIN units (that is the lattice MicroBodyEdit just
    // rebased), so it divides by skinScale to reach world voxels — NOT by
    // physScale, even though limb.voxels are physScale units.
    const float inv = 1.0f / (float)std::max(1u, skinScale);
    Vec3 d{(float)shift.x * inv, (float)shift.y * inv, (float)shift.z * inv};
    // The transform is read from `limb.xf` as the animation left it, never
    // re-read from Jolt here: re-reading a kinematic limb's transform mid-carve
    // picks up a pose the rest of this carve was not computed against, which
    // slides the wound along the limb (gotcha-live-limb-carve-pose).
    Quat q{limb.xf.quat[0], limb.xf.quat[1], limb.xf.quat[2], limb.xf.quat[3]};
    limb.xf.pos += Rotate(q, d);
    limb.restOffset += d;
    // The joint anchors are expressed from the limb origin, so they move the
    // opposite way to stay on the same physical point of the creature.
    limb.anchorLimb = limb.anchorLimb - d;
    if (fine) {
      // A skin-unit shift is not generally a whole number of collider voxels
      // (at skin 8 / collider 2, a shift of 3 is 0.75 of one). So the skin
      // rebases exactly and the collider is RE-DERIVED from it rather than
      // shifted to match — the derived lattice inherits the brick's origin
      // instead of negotiating for it, which is what makes the two frames
      // agree by construction rather than by vigilance.
      for (PrefabVoxel& sv : limb.skinVoxels) {
        sv.x = (int16_t)(sv.x - shift.x);
        sv.y = (int16_t)(sv.y - shift.y);
        sv.z = (int16_t)(sv.z - shift.z);
      }
      bool overflow = false;
      limb.voxels = DownsampleSkin(
          limb.skinVoxels,
          std::max(1u, skinScale / std::max(1u, physScale)), &overflow);
    } else {
      for (DebrisVoxel& v : limb.voxels) {
        v.x = (int8_t)(v.x - shift.x);
        v.y = (int8_t)(v.y - shift.y);
        v.z = (int8_t)(v.z - shift.z);
      }
    }
    limb.woundLocal = limb.woundLocal - d;
    limb.gushLocal = limb.gushLocal - d;
  }
  return true;
}

bool MobSystem::RebuildLimbBody(Mob& mob, int limbIndex) {
  Limb& limb = mob.limbs[limbIndex];
  if (!limb.body || limb.voxels.empty()) return false;
  const MobDef& def = defs_[mob.defIndex];
  // COLLIDER pitch: limb.voxels are physScale units, so the Jolt body is built
  // at 1/physScale.
  const float pitch = 1.0f / (float)std::max(1u, def.physScale);
  phys_->GetTransform(limb.body, limb.xf);
  uint64_t nh = phys_->CreateDebrisBodyXf(limb.voxels, limb.xf, densityOf_,
                                          true /*allowKinematic*/, pitch);
  if (nh == 0) return false;  // Jolt refused: keep the old collider, stay carved

  // The handle CHANGES, so every reference to the old one must be re-pointed
  // in the same breath or the limb silently detaches:
  //   - its joint to its parent,
  //   - its children's joints, which anchor to this body,
  //   - the intra-mob collision exclusion set.
  // Rebuilding them from the LIVE poses (not the rest pose) is what keeps a
  // limb carved mid-stride from snapping back to its spawn position.
  for (size_t k = 0; k < mob.limbs.size(); k++) {
    if ((int)k == limbIndex) continue;
    if (def.limbs[k].parent != def.limbs[limbIndex].name) continue;
    Limb& child = mob.limbs[k];
    if (!child.body || !child.joint) continue;
    Quat cq{child.xf.quat[0], child.xf.quat[1], child.xf.quat[2],
            child.xf.quat[3]};
    Vec3 anchorW = child.xf.pos + Rotate(cq, child.anchorLimb);
    phys_->DestroyJoint(child.joint);
    child.joint = phys_->CreateJoint(nh, child.body, def.limbs[k].joint, anchorW,
                                     def.limbs[k].axis, def.limbs[k].minAngle,
                                     def.limbs[k].maxAngle);
  }
  bool kinematic = mob.alive;
  phys_->RemoveBody(limb.body);
  limb.body = nh;
  phys_->SetBodyKinematic(limb.body, kinematic);

  if (limb.joint) {
    phys_->DestroyJoint(limb.joint);
    limb.joint = 0;
  }
  if (limbIndex != def.rootLimb) {
    for (size_t k = 0; k < def.limbs.size(); k++) {
      if (def.limbs[k].name != def.limbs[limbIndex].parent) continue;
      if (!mob.limbs[k].body) break;  // parent already severed: no joint to make
      Quat q{limb.xf.quat[0], limb.xf.quat[1], limb.xf.quat[2], limb.xf.quat[3]};
      Vec3 anchorW = limb.xf.pos + Rotate(q, limb.anchorLimb);
      limb.joint = phys_->CreateJoint(mob.limbs[k].body, limb.body,
                                      def.limbs[limbIndex].joint, anchorW,
                                      def.limbs[limbIndex].axis,
                                      def.limbs[limbIndex].minAngle,
                                      def.limbs[limbIndex].maxAngle);
      break;
    }
  }
  // Re-exclude the whole mob: the new handle is not in the old exclusion set,
  // so without this a carved limb starts colliding with its own siblings and
  // the rig fights itself into a jitter.
  {
    std::vector<uint64_t> handles;
    for (const Limb& l : mob.limbs)
      if (l.body) handles.push_back(l.body);
    phys_->DisableCollisionsAmong(handles);
  }
  return true;
}

void MobSystem::EmitCarvedFragment(Mob& mob, const Limb& src, uint32_t physScale,
                                   std::vector<DebrisVoxel> part, World& world,
                                   std::vector<ParticleSpawn>& spawns) {
  // Rebase the chunk to its own min corner and move its pose to match, the same
  // construction ShatterBody uses for a debris fragment.
  IVec3 mn{127, 127, 127};
  for (const DebrisVoxel& v : part) {
    mn.x = std::min<int>(mn.x, v.x);
    mn.y = std::min<int>(mn.y, v.y);
    mn.z = std::min<int>(mn.z, v.z);
  }
  BodyTransform xf = src.xf;
  const float inv = 1.0f / (float)std::max(1u, physScale);
  Quat q{xf.quat[0], xf.quat[1], xf.quat[2], xf.quat[3]};
  xf.pos += Rotate(q, Vec3{(float)mn.x * inv, (float)mn.y * inv,
                           (float)mn.z * inv});
  for (DebrisVoxel& v : part) {
    v.x = (int8_t)(v.x - mn.x);
    v.y = (int8_t)(v.y - mn.y);
    v.z = (int8_t)(v.z - mn.z);
  }

  // A chunk of a micro limb is itself a micro body: it needs its OWN brick,
  // because the limb's brick shows the limb. Without one it cannot be drawn at
  // the right size at all (cube instances are one WORLD voxel each), so it
  // falls through to particles — the same handoff every under-floor piece takes.
  MicroBodyRef micro{};
  if (src.microModel >= 0 && microSet_) {
    std::vector<PrefabVoxel> mv;
    mv.reserve(part.size());
    for (const DebrisVoxel& v : part)
      mv.push_back({(int16_t)v.x, (int16_t)v.y, (int16_t)v.z,
                    (uint16_t)(v.payload & 0xFFF)});
    IVec3 dims{1, 1, 1};
    for (const PrefabVoxel& v : mv) {
      dims.x = std::max<int>(dims.x, v.x + 1);
      dims.y = std::max<int>(dims.y, v.y + 1);
      dims.z = std::max<int>(dims.z, v.z + 1);
    }
    std::string log;
    int m = MicroBodyPack(*microSet_, mv, dims, physScale, "carve", log);
    if (m < 0) {
      LimbVoxelsToParticles(src, physScale, part, world, spawns);
      return;
    }
    // Packed models are SHARED by default; this one belongs to exactly one body
    // and must be freeable with it, or every gobbet leaks pool words.
    if (m < (int)microSet_->owned.size()) microSet_->owned[m] = 1;
    micro = MicroBodyRef{(uint32_t)m, physScale};
  }

  const float pitch = 1.0f / (float)std::max(1u, physScale);
  uint64_t h = phys_->CreateDebrisBodyXf(part, xf, densityOf_, false, pitch);
  if (h == 0) {
    if (micro.Valid()) MicroBodyFree(*microSet_, micro.model);
    LimbVoxelsToParticles(src, physScale, part, world, spawns);
    return;
  }
  // Push it off the wound so it visibly leaves the body rather than resting in
  // the cavity it came from.
  Vec3 away = xf.pos - src.xf.pos;
  float len = away.len();
  away = len > 1e-3f ? away * (1.0f / len) : Vec3{0, 1, 0};
  phys_->SetBodyVelocities(h, away * 2.5f + Vec3{0, 1.5f, 0}, Vec3{});
  debris_->AdoptBody(h, std::move(part), xf, micro, 0, {},
                     defs_[mob.defIndex].bleedMat);
}

bool MobSystem::CarveLimb(Mob& mob, int limbIndex, World& world,
                          std::vector<ParticleSpawn>& spawns, bool eject,
                          const LimbCarveFactory& carveAt) {
  Limb& limb = mob.limbs[limbIndex];
  if (!limb.body || limb.voxels.empty()) return true;
  const MobDef& def = defs_[mob.defIndex];
  // NOTE: limb.xf is deliberately NOT refreshed from Jolt here. the predicate
  // was built against the pose the CALLER measured, and a live limb is
  // kinematic — the animation pipeline re-poses it every tick — so re-reading
  // the transform now would test the voxels against a pose the predicate never
  // saw and carve the wrong cells (or, as the pose drifts, none at all).

  const bool fine = limb.HasFineSkin();
  // ONE world-space volume, re-expressed per lattice. The collider predicate
  // always exists; the skin one only when the skin is a separate lattice.
  const auto keep = carveAt((float)std::max(1u, def.physScale));

  std::vector<DebrisVoxel> removed;
  for (const DebrisVoxel& v : limb.voxels)
    if (!keep(v.x, v.y, v.z)) removed.push_back(v);
  // On a fine skin the COLLIDER may be too coarse to notice a small carve that
  // the skin does register. Deciding "nothing in range" on the collider would
  // silently make fine tools no-ops on exactly the detailed art the skin exists
  // to serve, so the skin gets its own say.
  bool skinRemoved = false;
  // Accumulated DURING the erase: once a voxel is gone the predicate can no
  // longer be asked where it was.
  Vec3 skinLostSum{};
  size_t skinLostN = 0;
  if (fine) {
    const auto keepSkin = carveAt((float)std::max(1u, def.skinScale));
    const size_t before = limb.skinVoxels.size();
    limb.skinVoxels.erase(
        std::remove_if(limb.skinVoxels.begin(), limb.skinVoxels.end(),
                       [&](const PrefabVoxel& v) {
                         if (keepSkin(v.x, v.y, v.z)) return false;
                         skinLostSum +=
                             Vec3{(float)v.x, (float)v.y, (float)v.z};
                         skinLostN++;
                         return true;
                       }),
        limb.skinVoxels.end());
    skinRemoved = limb.skinVoxels.size() != before;
  }
  if (removed.empty() && !skinRemoved) return true;  // nothing in range

  if (eject) LimbVoxelsToParticles(limb, def.physScale, removed, world, spawns);
  if (fine) {
    // Skin is authoritative: re-derive the collider from what the carve left
    // rather than carving the collider in parallel. Disagreement between the
    // two lattices is then unrepresentable (phys/lattice.h).
    bool overflow = false;
    limb.voxels = DownsampleSkin(
        limb.skinVoxels,
        std::max(1u, def.skinScale / std::max(1u, def.physScale)), &overflow);
  } else {
    limb.voxels.erase(
        std::remove_if(limb.voxels.begin(), limb.voxels.end(),
                       [&](const DebrisVoxel& v) {
                         return !keep(v.x, v.y, v.z);
                       }),
        limb.voxels.end());
  }
  instancesDirty_ = true;

  // A carved limb must stop flipbooking: a frame swap re-points rendering at an
  // intact authored model, which would heal every wound on screen.
  limb.flipbookModel = -1;

  // Losing matter hurts, in proportion to how much of the limb it was. The
  // wound is placed at the carve so the existing bleed machinery sprays from
  // the right spot with no new plumbing.
  const uint32_t at0 = std::max(1u, limb.voxelsAtSpawn);
  // Measured on the lattice `voxelsAtSpawn` counted, which is the skin whenever
  // there is one: a fraction only means anything against its own denominator,
  // and mixing the two would scale every wound by (skinScale/physScale)^3.
  const uint32_t nowCount =
      (uint32_t)(fine ? limb.skinVoxels.size() : limb.voxels.size());
  const uint32_t lostCount = at0 > nowCount ? at0 - nowCount : 0u;
  const float lost = (float)lostCount / (float)at0;
  limb.hp -= lost * def.limbs[limbIndex].hp * kCarveDamagePerVolume;
  if (def.bleedMat) {
    // Centroid of what was removed, in limb-local WORLD voxels — the frame
    // woundLocal is read in (PreTick rotates it by the limb's live quat).
    // Taken on whichever lattice actually registered the carve, then divided
    // by THAT lattice's scale.
    Vec3 c{};
    size_t n = 0;
    if (!removed.empty()) {
      for (const DebrisVoxel& v : removed)
        c += Vec3{(float)v.x, (float)v.y, (float)v.z};
      n = removed.size();
      c = c * (1.0f / (float)n / (float)std::max(1u, def.physScale));
    } else if (skinLostN) {
      // Collider too coarse to notice, skin was not: fall back to the skin's
      // own account of where the damage landed rather than leaving the wound
      // at wherever the last one happened to be.
      n = skinLostN;
      c = skinLostSum *
          (1.0f / (float)n / (float)std::max(1u, def.skinScale));
    }
    if (n) limb.woundLocal = c;
    limb.bleedBudget = AddBleedBudget(limb.bleedBudget,
                                      lost * (float)at0 * def.bleedPerDamage);
  }

  // Carved down past the point of being a limb at all: it comes off. This is
  // the geometric route to dismemberment — no hp threshold decided it, the
  // player simply removed too much of the arm for it to still be an arm.
  // The FRACTION is measured on the same lattice `at0` counted (skin when
  // there is one); the absolute kMinFragmentVoxels floor stays on the collider,
  // which is what "too few voxels to be a body" has always meant to Jolt.
  const bool collapsed =
      limb.voxels.size() < kMinFragmentVoxels ||
      (float)nowCount < kLimbCollapseFraction * (float)at0;
  if (collapsed || limb.hp <= 0) {
    // Sever() re-enters this mob by id and may call Die(); after it, neither
    // `mob` nor `limb` may be assumed live, so nothing below may touch them.
    Sever(mob.id, limbIndex);
    return false;
  }

  // ---- did the carve separate the limb into pieces? --------------------------
  // 6-connected components in limb-local space, the same test ShatterBody runs.
  // The component holding the JOINT ANCHOR keeps the limb's identity — it is
  // the part still attached to the creature — rather than simply the largest,
  // which would let a big carved-off haunch steal the leg's rig slot and leave
  // the animation driving a stump that is no longer connected to anything.
  const uint32_t n = (uint32_t)limb.voxels.size();
  if (n >= 2) {
    std::unordered_map<uint32_t, uint32_t> map;
    map.reserve(n * 2);
    auto key = [](int x, int y, int z) {
      return (uint32_t)((x + 128) | ((y + 128) << 8) | ((z + 128) << 16));
    };
    for (uint32_t i = 0; i < n; i++)
      map[key(limb.voxels[i].x, limb.voxels[i].y, limb.voxels[i].z)] = i;
    std::vector<int32_t> comp(n, -1);
    std::vector<uint32_t> compSize, stack;
    for (uint32_t seed = 0; seed < n; seed++) {
      if (comp[seed] != -1) continue;
      int32_t c = (int32_t)compSize.size();
      uint32_t size = 0;
      stack.assign(1, seed);
      comp[seed] = c;
      while (!stack.empty()) {
        uint32_t i = stack.back();
        stack.pop_back();
        size++;
        const DebrisVoxel& v = limb.voxels[i];
        const int d[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                             {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
        for (auto& dd : d) {
          auto it = map.find(key(v.x + dd[0], v.y + dd[1], v.z + dd[2]));
          if (it != map.end() && comp[it->second] == -1) {
            comp[it->second] = c;
            stack.push_back(it->second);
          }
        }
      }
      compSize.push_back(size);
    }
    if (compSize.size() > 1) {
      // The anchor in limb-local COLLIDER units — the point the joint holds.
      // The components were found on `limb.voxels`, so the anchor must be
      // expressed on THAT lattice: physScale, not skinScale. (This is the
      // conversion the loader's skinScale one at the top of the file is easiest
      // to confuse with; they run in opposite directions on different data.)
      Vec3 aMicro = limb.anchorLimb * (float)std::max(1u, def.physScale);
      uint32_t keepComp = 0;
      float best = 1e30f;
      for (uint32_t i = 0; i < n; i++) {
        const DebrisVoxel& v = limb.voxels[i];
        float d2 = (Vec3{(float)v.x, (float)v.y, (float)v.z} - aMicro).len();
        // Ties broken toward the bigger component, so a lone voxel sitting on
        // the anchor cannot inherit the limb.
        if (d2 < best || (d2 == best && compSize[comp[i]] > compSize[keepComp])) {
          best = d2;
          keepComp = (uint32_t)comp[i];
        }
      }
      std::vector<std::vector<DebrisVoxel>> parts(compSize.size());
      for (uint32_t c = 0; c < compSize.size(); c++) parts[c].reserve(compSize[c]);
      for (uint32_t i = 0; i < n; i++) parts[comp[i]].push_back(limb.voxels[i]);

      // The SKIN must follow the component that kept the limb's identity, or
      // the art would go on drawing flesh that physics has already thrown away
      // — the split happened on the collider, so which coarse block a skin
      // voxel belongs to is what decides where it goes.
      if (fine) {
        const uint32_t ratio =
            std::max(1u, def.skinScale / std::max(1u, def.physScale));
        std::unordered_map<uint32_t, uint32_t> blockComp;
        blockComp.reserve(n * 2);
        for (uint32_t i = 0; i < n; i++)
          blockComp[key(limb.voxels[i].x, limb.voxels[i].y, limb.voxels[i].z)] =
              (uint32_t)comp[i];
        limb.skinVoxels.erase(
            std::remove_if(limb.skinVoxels.begin(), limb.skinVoxels.end(),
                           [&](const PrefabVoxel& v) {
                             auto it = blockComp.find(key((int)v.x / (int)ratio,
                                                          (int)v.y / (int)ratio,
                                                          (int)v.z / (int)ratio));
                             // A skin voxel whose block did not survive the
                             // majority-fill has no component to belong to;
                             // it goes with the limb rather than vanishing.
                             return it != blockComp.end() &&
                                    it->second != keepComp;
                           }),
            limb.skinVoxels.end());
      }
      limb.voxels = std::move(parts[keepComp]);

      uint32_t budget = kMaxCarveFragments;
      for (uint32_t c = 0; c < (uint32_t)parts.size(); c++) {
        if (c == keepComp || parts[c].empty()) continue;
        if (parts[c].size() >= kMinFragmentVoxels && budget > 0) {
          // A carved-off fragment becomes its own debris body, packed from the
          // COLLIDER voxels it was split on — it has no skin lattice of its
          // own, so its brick is packed at physScale to match its coords.
          EmitCarvedFragment(mob, limb, def.physScale, std::move(parts[c]),
                             world, spawns);
          budget--;
        } else {
          LimbVoxelsToParticles(limb, def.physScale, parts[c], world, spawns);
        }
      }
      // Losing the disconnected mass can itself take the limb under the floor.
      // Same lattice pairing as the first collapse test above.
      if (limb.voxels.size() < kMinFragmentVoxels ||
          (float)(fine ? limb.skinVoxels.size() : limb.voxels.size()) <
              kLimbCollapseFraction * (float)at0) {
        Sever(mob.id, limbIndex);
        return false;
      }
    }
  }

  // Art, then collider. ReskinLimbMicro may shift the limb origin, and the
  // collider must be built from the voxels in their FINAL frame.
  if (limb.microModel >= 0)
    ReskinLimbMicro(mob, limb, def.skinScale, def.physScale);
  RebuildLimbBody(mob, limbIndex);
  return true;
}

bool MobSystem::CarveLimbRadial(uint64_t bodyHandle, Vec3 centerWorldVoxel,
                                float radiusVoxels, bool ragged, bool eject,
                                World& world,
                                std::vector<ParticleSpawn>& spawns) {
  if (!phys_ || radiusVoxels <= 0.0f) return false;
  for (Mob& mob : mobs_) {
    for (size_t i = 0; i < mob.limbs.size(); i++) {
      if (mob.limbs[i].body != bodyHandle) continue;
      const MobDef& def = defs_[mob.defIndex];
      Limb& limb = mob.limbs[i];
      phys_->GetTransform(limb.body, limb.xf);
      // ONE world-space sphere, re-expressed per lattice. CarveLimb calls this
      // once with physScale and, on a fine-skinned limb, again with skinScale —
      // the centre and radius are converted into whichever frame the voxels
      // being tested live in, so the same physical volume is removed from both.
      //
      // This is what makes precision scale with the def: at skinScale 8 a
      // radius of 0.125 world voxels is a single skin voxel, so a fine enough
      // tool can take out one cell of a brain without any new code path.
      Quat q{limb.xf.quat[0], limb.xf.quat[1], limb.xf.quat[2], limb.xf.quat[3]};
      const Vec3 cBody = RotateInv(q, centerWorldVoxel - limb.xf.pos);
      const uint32_t seed = (uint32_t)mob.id * 2654435761u + (uint32_t)i;
      // The ragged-rim jitter is keyed on the SKIN lattice regardless of which
      // lattice is being tested, so the crater's shape is a property of the
      // art rather than of whatever collider resolution the engine happened to
      // derive — otherwise the same blast would tear differently on two rigs
      // that only differ in limb size.
      const float jitterScale = (float)std::max(1u, def.skinScale);
      CarveLimb(
          mob, (int)i, world, spawns, eject,
          [&, cBody, seed, jitterScale](float scale) {
            const Vec3 cLocal = cBody * scale;
            const float rLocal = radiusVoxels * scale;
            const float rLocal2 = rLocal * rLocal;
            const float toSkin = jitterScale / scale;
            return [=](int x, int y, int z) {
              Vec3 c{(float)x + 0.5f, (float)y + 0.5f, (float)z + 0.5f};
              Vec3 dv = c - cLocal;
              float d2 = dv.dot(dv);
              if (d2 >= rLocal2) return true;  // outside
              if (!ragged) return false;       // clean bore (laser kerf)
              // Ragged rim: certain removal in the core, thinning outward,
              // so a blast crater in flesh is torn rather than scooped.
              // CPU gameplay state (limbs are outside the hashed domain),
              // so a float hash is fine — rule 1 governs the grid.
              float t = std::sqrt(d2 / rLocal2);
              float chance = 1.0f - t * t;
              // Quantised onto the skin lattice so both passes agree on which
              // part of the rim is torn.
              int sx = (int)std::floor((float)x * toSkin);
              int sy = (int)std::floor((float)y * toSkin);
              int sz = (int)std::floor((float)z * toSkin);
              uint32_t h = Hash3(seed, (uint32_t)sx * 73856093u,
                                 (uint32_t)sy * 19349663u ^
                                     (uint32_t)sz * 83492791u);
              return (float)(h & 0xFFFFu) / 65535.0f >= chance;
            };
          });
      return true;
    }
  }
  return false;
}

void MobSystem::CarveMobsRadial(Vec3 centerWorldVoxel, float radiusVoxels,
                                World& world,
                                std::vector<ParticleSpawn>& spawns) {
  if (!phys_ || radiusVoxels <= 0.0f) return;
  // Collect handles FIRST: carving rebuilds colliders and can sever limbs or
  // kill mobs, both of which mutate mobs_ and limb handles mid-walk. Iterating
  // the live structure while it reshapes under us is how this kind of loop
  // usually acquires a use-after-free.
  std::vector<uint64_t> handles;
  for (const Mob& mob : mobs_)
    if (mob.alive)
      for (const Limb& l : mob.limbs)
        if (l.body) {
          BodyTransform xf{};
          phys_->GetTransform(l.body, xf);
          // cheap reject against the limb's bounding sphere. `l.size` is in
          // COLLIDER units, so it divides by physScale.
          float r = 0;
          const float inv =
              1.0f / (float)std::max(1u, defs_[mob.defIndex].physScale);
          r = 0.5f * Vec3{(float)l.size.x, (float)l.size.y, (float)l.size.z}.len() *
              inv;
          if ((xf.pos - centerWorldVoxel).len() <= radiusVoxels + r + 2.0f)
            handles.push_back(l.body);
        }
  for (uint64_t h : handles)
    CarveLimbRadial(h, centerWorldVoxel, radiusVoxels, true /*ragged*/,
                    true /*eject*/, world, spawns);
}

void MobSystem::Sever(uint64_t mobId, int limbIndex) {
  for (Mob& mob : mobs_) {
    if (mob.id != mobId) continue;
    const MobDef& def = defs_[mob.defIndex];
    if (limbIndex < 0 || limbIndex >= (int)mob.limbs.size()) return;
    const MobLimbDef& ld = def.limbs[limbIndex];
    if (limbIndex == def.rootLimb || ld.vital || !ld.severable) {
      Die(mob);
    } else {
      // The cut point in WORLD space, captured BEFORE DetachLimb: the joint
      // anchor expressed through the severed limb's own live transform.
      //
      // The obvious-looking `mob.origin + anchorRoot` is wrong and was the bug
      // behind blood appearing under the mob and on its wrong side. anchorRoot
      // is a REST-POSE offset in the prefab authoring frame, so using it raw
      // ignores (a) the mob's heading — a creature facing away sprays from the
      // mirrored side — and (b) `mob.bodyY`, the animated body height, where
      // mob.origin.y is only the spawn corner, so the wound sits at the mob's
      // feet while the body stands above it.
      //
      // limb.xf is the pose Jolt is actually holding this instant, and
      // anchorLimb is the same joint in that limb's local frame, so this is
      // correct under any animation, heading or slope with no frame rebuild —
      // the identical construction Damage() uses to locate a hit.
      const Limb& cut = mob.limbs[limbIndex];
      Quat cq{cut.xf.quat[0], cut.xf.quat[1], cut.xf.quat[2], cut.xf.quat[3]};
      Vec3 anchorW = cut.body ? cut.xf.pos + Rotate(cq, cut.anchorLimb)
                              : mob.origin + cut.anchorRoot;

      // Report the cut for audio. Recorded HERE, before DetachLimb, because
      // anchorW is the one place the wound's world position is known — after
      // the detach the limb has its own frame and the joint is gone.
      severs_.push_back(SeverEvent{anchorW, mob.id, limbIndex,
                                   (int)mob.defIndex, bladeCut_,
                                   bladeCut_ ? bladeSeverity_ : 1.0f});

      DetachLimb(mob, limbIndex, true);
      // the stump bleeds: wound at the joint on the PARENT side
      for (size_t k = 0; k < def.limbs.size(); k++)
        if (def.limbs[k].name == ld.parent) {
          Limb& parent = mob.limbs[k];
          Quat q{parent.xf.quat[0], parent.xf.quat[1], parent.xf.quat[2],
                 parent.xf.quat[3]};
          const auto& gore = CurrentTuning().gore;
          parent.woundLocal = RotateInv(q, anchorW - parent.xf.pos);
          // The stump's own drip budget, on top of the thrown voxels below:
          // this is the puddle that keeps forming under a fresh amputation.
          parent.bleedBudget =
              AddBleedBudget(parent.bleedBudget, gore.severStumpBudget);

          // Arm the gout. PreTick drains it over severDecayTicks; arming state
          // here rather than emitting now keeps every particle this frame
          // inside the one per-tick spawn budget, and keeps spray order
          // independent of the order limbs happened to be damaged in.
          parent.gushTicks = mob.gore.severDecayTicks;
          parent.gushLocal = parent.woundLocal;
          // Spray along the stump: from the parent's centre out through the
          // wound, so a cut arm sprays away from the torso instead of into it.
          Vec3 out = anchorW - parent.xf.pos;
          float len = out.len();
          out = len > 1e-3f ? out * (1.0f / len) : Vec3{0, 1, 0};
          // Tilt it upward — a horizontal jet mostly misses the world and the
          // droplets expire in mid-air with nothing stained.
          out.y += 0.5f;
          len = out.len();
          parent.gushDir = RotateInv(q, len > 1e-3f ? out * (1.0f / len)
                                                    : Vec3{0, 1, 0});

          // The whole blood VOXELS the cut throws. These are conserved matter
          // (they pool, they flow, the CA owns them), so they are deliberately
          // few next to the hundreds of micro droplets — the spray does the
          // visual work, the voxels do the lasting mess.
          const uint32_t es = (uint32_t)mob.id ^ ((uint32_t)limbIndex << 16);
          // Sever is not driven by the tick loop, so the draw is indexed by the
          // voxel counter alone; the mob id keeps it distinct between mobs.
          int nVox = EventVarI(mob.gore.severVoxels, gore.severVoxelsVar, es,
                               0x5EEDu, 0u);
          if (nVox < 0) nVox = 0;
          // Gobbet size SUBDIVIDES the throw: severVoxels stays the total voxel
          // count and `gob` of them share one trajectory, so raising it makes
          // the cut throw fewer, fatter lumps without changing how much matter
          // enters the world. `k` counts gobbets; `m` counts members.
          const int gob = std::max(1, gore.severGobbetVoxels);
          const int nGob = (nVox + gob - 1) / gob;
          int thrown = 0;
          for (int k = 0; k < nGob; k++) {
            if (pendingSpawns_.size() >= kMaxParticleSpawnsPerTick) break;
            uint32_t h = Hash3((uint32_t)mob.id * 22695477u + (uint32_t)limbIndex,
                               (uint32_t)k, 0x5EEDu);
            Vec3 dir{parent.gushDir.x + SignedUnit(h) * 0.7f,
                     std::fabs(parent.gushDir.y) + 0.3f,
                     parent.gushDir.z + SignedUnit(Pcg(h ^ 0x31u)) * 0.7f};
            dir = Rotate(q, dir);
            float sp = EventVar(mob.gore.severVoxelSpeed, gore.severVoxelSpeedVar,
                                es, 0x5EEDu, (uint32_t)k + 1u) *
                       (0.6f + 0.8f * (float)(Pcg(h ^ 0x9Fu) & 0xFFFFu) / 65535.0f);
            if (sp < 0.0f) sp = 0.0f;
            // One gobbet: `gob` voxels on the SAME velocity, offset by a small
            // jitter so they occupy distinct cells. Stacking them on one cell
            // instead would hand the claim lattice a pile where exactly one
            // wins per tick, turning a lump into a slow drip.
            for (int m = 0; m < gob && thrown < nVox; m++, thrown++) {
              if (pendingSpawns_.size() >= kMaxParticleSpawnsPerTick) break;
              uint32_t hm = Pcg(h ^ (0x9E3779B9u * (uint32_t)(m + 1)));
              const float sprd = gore.severGobbetSpread;
              Vec3 at{anchorW.x + SignedUnit(hm) * sprd,
                      anchorW.y + SignedUnit(Pcg(hm ^ 0x2A5u)) * sprd,
                      anchorW.z + SignedUnit(Pcg(hm ^ 0xB77u)) * sprd};
              pendingSpawns_.push_back(
                  MakeDroplet(at, dir * sp, def.bleedMat, false, 0, 0));
            }
          }
        }
    }
    return;
  }
}

void MobSystem::DetachLimb(Mob& mob, int limbIndex, bool adopt) {
  Limb& limb = mob.limbs[limbIndex];
  if (!limb.body) return;
  if (limb.joint) {
    // DestroyJoint calls Jolt's RemoveConstraint and drops the ref — the
    // constraint is GONE, not left disabled. A disabled constraint would keep
    // the severed limb tethered to a body it no longer belongs to.
    phys_->DestroyJoint(limb.joint);
    limb.joint = 0;
  }
  // children of this limb are orphaned too: their joints attach to it and
  // die with the body chain when severed recursively
  const MobDef& def = defs_[mob.defIndex];
  for (size_t k = 0; k < def.limbs.size(); k++)
    if (def.limbs[k].parent == def.limbs[limbIndex].name && mob.limbs[k].body)
      DetachLimb(mob, (int)k, adopt);

  // The limb becomes debris NOW (so counts and rendering are immediate), but
  // stays kinematic in its last animated pose for a beat before going
  // dynamic. Cutting straight to ragdoll on the hit frame reads as a
  // teleport; the brief hold sells the cut.
  if (limbIndex >= 0 && limbIndex < (int)mob.anim.partAlive.size())
    mob.anim.partAlive[limbIndex] = 0;  // gait stops scheduling it, IK -> 0
  if (adopt) {
    // Hand the micro description over with the body: that is what makes a
    // severed microvoxel limb keep its detail as ordinary debris (PLAN §C4) —
    // and, for a CARVED limb, what makes it keep its wounds. The brick this
    // limb owns is transferred, not copied, so `carved` is cleared: from here
    // DebrisSystem's ReleaseBody is the one thing that may free it, and a limb
    // that also thought it owned the model would double-free the block.
    // The SKIN travels with the body: AdoptBody's trailing params exist so a
    // severed limb stays as detailed as it was on the creature. Without them
    // the debris body would fall back to its collider lattice and the limb
    // would visibly coarsen at the moment it came off.
    debris_->AdoptBody(limb.body, limb.voxels, limb.xf,
                       limb.MicroRef(def.skinScale), def.physScale,
                       std::move(limb.skinVoxels), def.bleedMat);
    limb.skinVoxels.clear();
    limb.carved = false;
    limb.holdBody = limb.body;
    limb.holdSeconds = kSeverHoldSeconds;
  } else {
    // Not adopted: nothing downstream will ever free this limb's brick, so it
    // must be returned here.
    ReleaseLimbMicro(limb);
    phys_->SetBodyKinematic(limb.body, false);
    phys_->ClearCollisionGroup(limb.body);
    phys_->RemoveBody(limb.body);
  }
  limb.body = 0;  // the mob no longer owns it either way
  instancesDirty_ = true;
}

void MobSystem::Die(Mob& mob) {
  if (!mob.alive) return;
  mob.alive = false;
  // whole-body ragdoll: every limb goes dynamic and becomes debris; joints
  // stay so the corpse hangs together until pieces get culled or settle
  for (size_t i = 0; i < mob.limbs.size(); i++) {
    Limb& limb = mob.limbs[i];
    if (!limb.body) continue;
    phys_->SetBodyKinematic(limb.body, false);
    debris_->AdoptBody(limb.body, limb.voxels, limb.xf,
                       limb.MicroRef(defs_[mob.defIndex].skinScale),
                       defs_[mob.defIndex].physScale,
                       std::move(limb.skinVoxels),
                       defs_[mob.defIndex].bleedMat);
    limb.skinVoxels.clear();
    limb.carved = false;  // brick ownership moved with the body (see DetachLimb)
    limb.body = 0;
    if (limb.joint) limb.joint = 0;  // ownership follows the bodies now
    if (i < mob.anim.partAlive.size()) mob.anim.partAlive[i] = 0;
  }
  instancesDirty_ = true;
  // Death goes straight to ragdoll (no hold): the whole body flips at once,
  // so there is no "still-attached" pose left to sell. The husk is removed on
  // the next PreTick sweep once nothing is holding; drop the limb list but
  // keep any in-flight hold entries alive.
  std::vector<Limb> holding;
  for (Limb& l : mob.limbs)
    if (l.holdBody) holding.push_back(std::move(l));
  mob.limbs = std::move(holding);
}

void MobSystem::AppendInstances(std::vector<BodyVoxInst>& out,
                                uint32_t slotBase) {
  uint32_t slot = slotBase;
  for (const Mob& mob : mobs_) {
    for (const Limb& limb : mob.limbs) {
      if (!limb.body) continue;
      if (slot >= kMaxBodySlots) break;
      // Slots WITH a micro model render through the OBB/brick-march pass, so
      // they must not also emit cube instances — that would draw the limb
      // twice, at the wrong size (cube instances are one WORLD voxel each).
      // The slot is still consumed: slots are shared with the micro pass.
      if (limb.microModel >= 0) {
        slot++;
        continue;
      }
      // a playing flipbook re-points this limb at another model's voxels
      const std::vector<DebrisVoxel>* src = &limb.voxels;
      if (limb.flipbookModel >= 0 &&
          limb.flipbookModel < (int)limb.frameVoxels.size() &&
          !limb.frameVoxels[limb.flipbookModel].empty())
        src = &limb.frameVoxels[limb.flipbookModel];
      rigrender::AppendVoxInsts(out, slot, *src);
      slot++;
    }
  }
  instancesDirty_ = false;
}

void MobSystem::AppendXforms(std::vector<BodyXformGpu>& out) const {
  for (const Mob& mob : mobs_) {
    for (const Limb& limb : mob.limbs) {
      if (!limb.body) continue;
      if (out.size() >= kMaxBodySlots) return;
      rigrender::AppendXform(out, limb.xf);
    }
  }
}


// ---- collision-box debug overlay (world.h DebugBox) -------------------------
//
// Draws the individual sub-shapes of each compound collider rather than one
// big AABB per body. See rigrender::AppendDebugBoxesFor for why these come
// from the Jolt shape rather than from the voxels that built it.
void MobSystem::AppendDebugBoxes(std::vector<DebugBox>& out, size_t limit,
                                 uint32_t color) const {
  if (!phys_) return;
  static std::vector<SubShapeBox> subs;
  for (const Mob& mob : mobs_) {
    for (const Limb& limb : mob.limbs) {
      if (!limb.body) continue;
      if (out.size() >= limit) return;
      rigrender::AppendDebugBoxesFor(out, *phys_, limb.body, limb.xf, limit,
                                     color, subs);
    }
  }
}

void MobSystem::AppendMicroInsts(std::vector<MicroBodyInstGpu>& out,
                                 uint32_t slotBase) const {
  // Walks slots in the SAME order as AppendXforms/AppendInstances, so the slot
  // it records is the one the transform lands in.
  uint32_t slot = slotBase;
  for (const Mob& mob : mobs_)
    for (const Limb& limb : mob.limbs) {
      if (!limb.body) continue;
      if (slot >= kMaxBodySlots) return;
      if (limb.microModel >= 0)
        out.push_back({slot, (uint32_t)limb.microModel, 0, 0});
      slot++;
    }
}

uint64_t MobSystem::LimbBody(uint64_t mobId, int limbIndex) const {
  for (const Mob& mob : mobs_)
    if (mob.id == mobId && limbIndex >= 0 && limbIndex < (int)mob.limbs.size())
      return mob.limbs[limbIndex].body;
  return 0;
}

bool MobSystem::IsAlive(uint64_t mobId) const {
  for (const Mob& mob : mobs_)
    if (mob.id == mobId) return mob.alive;
  return false;
}

Vec3 MobSystem::MobOrigin(uint64_t mobId) const {
  for (const Mob& mob : mobs_)
    if (mob.id == mobId) return mob.origin;
  return {};
}

Vec3 MobSystem::MobFacing(uint64_t mobId) const {
  // Must stay byte-identical to the `fwd` in the walk step and to
  // AxisAngle({0,1,0}, heading) applied to +Z — one convention, one formula.
  for (const Mob& mob : mobs_)
    if (mob.id == mobId)
      return Vec3{std::sin(mob.heading), 0, std::cos(mob.heading)};
  return {0, 0, 1};
}

float MobSystem::MobHeading(uint64_t mobId) const {
  for (const Mob& mob : mobs_)
    if (mob.id == mobId) return mob.heading;
  return 0;
}

float MobSystem::MobDesiredHeading(uint64_t mobId) const {
  for (const Mob& mob : mobs_)
    if (mob.id == mobId) return mob.desiredHeading;
  return 0;
}

float MobSystem::MobTurnVel(uint64_t mobId) const {
  for (const Mob& mob : mobs_)
    if (mob.id == mobId) return mob.turnVel;
  return 0;
}

void MobSystem::SetDesiredHeading(uint64_t mobId, float radians) {
  for (Mob& mob : mobs_)
    if (mob.id == mobId) {
      mob.desiredHeading = radians;
      // Deliberately does NOT touch `heading` or `turnVel`: an external
      // steering command is subject to exactly the same turn-rate clamp the
      // wander behaviour is. That is the invariant that keeps a future
      // "face the player" from becoming an instant snap.
      return;
    }
}

int MobSystem::SwingingFeet(uint64_t mobId) const {
  for (const Mob& mob : mobs_) {
    if (mob.id != mobId) continue;
    int n = 0;
    for (const FootState& f : mob.anim.feet) n += (f.valid && f.swinging) ? 1 : 0;
    return n;
  }
  return -1;
}

int MobSystem::PlantedFeet(uint64_t mobId) const {
  for (const Mob& mob : mobs_) {
    if (mob.id != mobId) continue;
    int n = 0;
    for (const FootState& f : mob.anim.feet) n += (f.valid && !f.swinging) ? 1 : 0;
    return n;
  }
  return -1;
}

int MobSystem::ActiveClips(uint64_t mobId) const {
  for (const Mob& mob : mobs_)
    if (mob.id == mobId) return (int)mob.anim.clips.size();
  return -1;
}

int MobSystem::LocoState(uint64_t mobId) const {
  for (const Mob& mob : mobs_)
    if (mob.id == mobId) return mob.anim.locoState;
  return -1;
}

std::vector<std::pair<std::string, float>> MobSystem::ClipWeights(
    uint64_t mobId) const {
  std::vector<std::pair<std::string, float>> out;
  for (const Mob& mob : mobs_) {
    if (mob.id != mobId) continue;
    const AnimSkeleton& sk = defs_[mob.defIndex].skel;
    for (const ClipInstance& ci : mob.anim.clips) {
      if (ci.clip < 0 || ci.clip >= (int)sk.clips.size()) continue;
      out.push_back({sk.clips[ci.clip].name, ci.weight * ci.fade});
    }
  }
  return out;
}

Vec3 MobSystem::LimbLocalUp(uint64_t mobId, int limbIndex) const {
  for (const Mob& mob : mobs_) {
    if (mob.id != mobId) continue;
    if (limbIndex < 0 || limbIndex >= (int)mob.anim.local.size()) break;
    return QuatRotate(mob.anim.local[limbIndex].rot, {0, 1, 0});
  }
  return {0, 1, 0};
}

Vec3 MobSystem::LimbModelUp(uint64_t mobId, int limbIndex) const {
  for (const Mob& mob : mobs_) {
    if (mob.id != mobId) continue;
    if (limbIndex < 0 || limbIndex >= (int)mob.anim.model.size()) break;
    return QuatRotate(mob.anim.model[limbIndex].rot, {0, 1, 0});
  }
  return {0, 1, 0};
}

uint32_t MobSystem::LimbVoxelCount(uint64_t mobId, int limbIndex) const {
  for (const Mob& mob : mobs_)
    if (mob.id == mobId && limbIndex >= 0 && limbIndex < (int)mob.limbs.size())
      return (uint32_t)mob.limbs[limbIndex].voxels.size();
  return 0;
}

uint32_t MobSystem::LimbVoxelsAtSpawn(uint64_t mobId, int limbIndex) const {
  for (const Mob& mob : mobs_)
    if (mob.id == mobId && limbIndex >= 0 && limbIndex < (int)mob.limbs.size())
      return mob.limbs[limbIndex].voxelsAtSpawn;
  return 0;
}

Vec3 MobSystem::LimbVoxelPos(uint64_t mobId, int limbIndex, uint32_t n) const {
  for (const Mob& mob : mobs_) {
    if (mob.id != mobId) continue;
    if (limbIndex < 0 || limbIndex >= (int)mob.limbs.size()) break;
    const Limb& limb = mob.limbs[limbIndex];
    if (limb.voxels.empty()) return limb.xf.pos;
    const DebrisVoxel& v = limb.voxels[n % (uint32_t)limb.voxels.size()];
    // Reading limb.voxels, which are COLLIDER units.
    const float inv =
        1.0f / (float)std::max(1u, defs_[mob.defIndex].physScale);
    Vec3 c{((float)v.x + 0.5f) * inv, ((float)v.y + 0.5f) * inv,
           ((float)v.z + 0.5f) * inv};
    Quat q{limb.xf.quat[0], limb.xf.quat[1], limb.xf.quat[2], limb.xf.quat[3]};
    return limb.xf.pos + Rotate(q, c);
  }
  return Vec3{};
}

uint32_t MobSystem::LimbBodyCount() const {
  uint32_t n = 0;
  for (const Mob& mob : mobs_)
    for (const Limb& limb : mob.limbs)
      if (limb.body) n++;
  return n;
}
