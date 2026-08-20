#include "game/mob.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

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

// CPU mirror of common.wgsl pcg/hash3, same as the one in debris.cpp: stateless
// and counter-based, so a given (mob, limb, tick, index) always produces the
// same droplet. Spray direction is presentation, but it is authored INTO the
// tick's spawn stream, which replays must reproduce — a stateful rng here would
// desync a replay the moment a frame boundary moved.
uint32_t Pcg(uint32_t v) {
  uint32_t s = v * 747796405u + 2891336453u;
  uint32_t w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
  return (w >> 22u) ^ w;
}
uint32_t Hash3(uint32_t a, uint32_t b, uint32_t c) {
  return Pcg(a ^ Pcg(b ^ Pcg(c)));
}
// Uniform in [-1, 1) from a hash word.
float SignedUnit(uint32_t h) {
  return (float)(int32_t)(h & 0xFFFFu) / 32768.0f - 1.0f;
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

    // Micro-voxel authoring scale (PLAN §C). Anything other than 1/2/4 is a
    // typo, not a feature: fall back to 1 loudly rather than half-applying it.
    def.scale = j.value("scale", 1u);
    if (def.scale != 1 && def.scale != 2 && def.scale != 4) {
      log += jp + ": scale must be 1, 2 or 4 (got " +
             std::to_string(def.scale) + ") — using 1\n";
      def.scale = 1;
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
    // DebrisVoxel is int8: each LIMB must fit in the byte range (§2.1). At
    // scale > 1 the limb's voxels are stored in MICRO units, so the bound
    // applies to the micro box — a scale-4 limb may only be 30 world voxels
    // long. Say so, because "exceeds 120 voxels" on a limb the author drew 24
    // world-voxels tall is otherwise baffling.
    for (const PrefabModel& m : def.prefab.models)
      if (m.size.x > 120 || m.size.y > 120 || m.size.z > 120) {
        log += def.name + ": limb model \"" + m.name + "\" is " +
               std::to_string(m.size.x) + "x" + std::to_string(m.size.y) + "x" +
               std::to_string(m.size.z) +
               (def.scale > 1 ? " MICRO voxels; the DebrisVoxel int8 bound is "
                                "120 micro voxels per axis (= " +
                                    std::to_string(120 / def.scale) +
                                    " world voxels at scale " +
                                    std::to_string(def.scale) + ")\n"
                              : " voxels, exceeding the int8 bound of 120\n");
        ok = false;
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
        // MICRO -> WORLD. Anchors are authored in .vox coordinates, which at
        // scale>1 are micro units; the rig, the gait and the physics all work
        // in world voxels. Converting HERE, once, is what keeps every
        // downstream stage (AnimFlatten, IK, GroundHeightAt, the joint
        // anchors in Spawn) completely scale-unaware.
        const float inv = 1.0f / (float)def.scale;
        sk.parts[i].anchorLocal = anchor * inv;
      }
      for (size_t i = 0; i < def.limbs.size(); i++) {
        int par = sk.parts[i].parent;
        sk.parts[i].rest.pos =
            par >= 0 ? sk.parts[i].anchorLocal - sk.parts[par].anchorLocal
                     : sk.parts[i].anchorLocal;
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
    def.worldSize = Vec3{(float)def.prefab.size.x, (float)def.prefab.size.y,
                         (float)def.prefab.size.z} * (1.0f / (float)def.scale);

    // ---- micro brick upload (PLAN §C, sim/microbody.h) ----
    // Packed once per DEF, shared by every instance: a limb's voxels never
    // change after load in v1, so there is no per-instance storage at all.
    // Done last so a def that failed validation never enters the pool.
    if (ok && def.scale > 1) {
      for (MobLimbDef& ld : def.limbs) {
        int mi = FindModel(def.prefab, ld.name);
        if (mi < 0) continue;
        const PrefabModel& m = def.prefab.models[mi];
        ld.microModel = MicroBodyPack(micro, m.voxels, m.size, def.scale,
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
      if (l.body) phys_->RemoveBody(l.body);
    }
  mobs_.clear();
  instancesDirty_ = true;
}

uint64_t MobSystem::Spawn(int defIndex, IVec3 atVoxel) {
  if (defIndex < 0 || defIndex >= (int)defs_.size()) return 0;
  if (mobs_.size() >= kMaxMobs) return 0;
  const MobDef& def = defs_[defIndex];

  Mob mob;
  mob.id = nextId_++;
  mob.defIndex = defIndex;
  mob.origin = Vec3{(float)atVoxel.x, (float)atVoxel.y, (float)atVoxel.z};
  mob.limbs.resize(def.limbs.size());

  // MICRO -> WORLD. limb.voxels and limb.size stay in the def's authoring units
  // (micro voxels at scale>1) because that is what both the collider and the
  // renderer's brick march want; every POSITION derived from them is divided
  // into world voxels here.
  const float inv = 1.0f / (float)def.scale;

  for (size_t i = 0; i < def.limbs.size(); i++) {
    const MobLimbDef& ld = def.limbs[i];
    int mi = FindModel(def.prefab, ld.name);
    const PrefabModel& model = def.prefab.models[mi];
    Limb& limb = mob.limbs[i];
    limb.hp = ld.hp;
    limb.size = model.size;
    limb.microModel = ld.microModel;
    limb.restOffset = Vec3{(float)model.offset.x, (float)model.offset.y,
                           (float)model.offset.z} * inv;
    limb.voxels.reserve(model.voxels.size());
    for (const PrefabVoxel& v : model.voxels) {
      uint32_t variant = ((uint32_t)(v.x * 7 + v.y * 13 + v.z * 29)) % 3u;
      limb.voxels.push_back({(int8_t)v.x, (int8_t)v.y, (int8_t)v.z, 0,
                             (uint16_t)(v.material | (variant << 12))});
    }
    // The body origin is the limb's min corner in WORLD voxels; the collider
    // is built at pitch 1/scale so its micro-unit local coordinates land in the
    // right physical place. Not an integer cell any more at scale>1, which is
    // fine — a Jolt body has never been lattice-aligned.
    Vec3 origin = mob.origin + limb.restOffset;
    BodyTransform bxf{};
    bxf.pos = origin;
    bxf.quat[3] = 1;
    limb.body = phys_->CreateDebrisBodyXf(limb.voxels, bxf, densityOf_,
                                          true /*allowKinematic*/, inv);
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
      inst.timeMs = 0;
      return;
    }
  ClipInstance inst;
  inst.clip = ci;
  inst.weight = 1.0f;
  mob.anim.clips.push_back(inst);
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
    float targetY = sumY / (float)nFeet + g.rideHeight * mob.anim.feet[0].legLength;
    // the ride height is expressed in leg lengths but the mob's origin is its
    // prefab min corner, so clamp the correction rather than snapping
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

  // ---- stages 1-3: sample active clips, blend, apply additives ----
  AnimSampleAndBlend(sk, st, dt);

  // ---- procedural layer: legacy phase swing + pelvis bob/sway/spine ----
  // dummy.json has swingAmp/swingPhase and no chains; running it HERE rather
  // than as a separate code path means the fallback and the new rig share one
  // pipeline (flatten, IK, physics blend all behave identically).
  for (size_t i = 0; i < sk.parts.size(); i++) {
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
  if (g.present && def.rootLimb >= 0 && def.rootLimb < (int)sk.parts.size()) {
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
  for (size_t i = 0; i < sk.parts.size(); i++) {
    const AnimPart& p = sk.parts[i];
    if (!p.hasSpring) continue;
    Vec3 goal{-st.velocity.z * p.spring.gain * 0.05f, 0,
              st.velocity.x * p.spring.gain * 0.05f};
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
  if (g.present) UpdateGait(mob, def, world, dt);
  if (!sk.chains.empty()) {
    Quat yaw = AxisAngle({0, 1, 0}, mob.heading);
    Vec3 rootAnchor = sk.parts[def.rootLimb].anchorLocal;
    Vec3 pivot{def.worldSize.x * 0.5f, 0, def.worldSize.z * 0.5f};
    Vec3 bodyOrigin{mob.origin.x, mob.bodyY, mob.origin.z};
    for (size_t c = 0; c < sk.chains.size() && c < st.feet.size(); c++) {
      const FootState& f = st.feet[c];
      float weight = f.valid ? sk.chains[c].weight : 0.0f;  // limb loss -> 0
      if (weight <= 0) continue;
      // world foot target -> model space (inverse of the submit transform)
      Vec3 rel = f.planted - bodyOrigin - pivot;
      Vec3 prefabPt = RotateInv(yaw, rel) + pivot;
      AnimSolveTwoBone(sk, st, sk.chains[c], prefabPt - rootAnchor, weight);
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
      // ---- kinematic walk (PLAN §B4: keyframes, not motors) ----
      float cx = mob.origin.x + def.worldSize.x * 0.5f;
      float cz = mob.origin.z + def.worldSize.z * 0.5f;
      Vec3 fwd{std::sin(mob.heading), 0, std::cos(mob.heading)};

      int groundY;
      bool haveGround = GroundHeightAt(world, ifloor(cx), ifloor(cz),
                                       ifloor(mob.origin.y) + 3, groundY);
      int aheadY;
      bool haveAhead = GroundHeightAt(
          world, ifloor(cx + fwd.x * (def.worldSize.x * 0.5f + 2.0f)),
          ifloor(cz + fwd.z * (def.worldSize.z * 0.5f + 2.0f)),
          ifloor(mob.origin.y) + 3, aheadY);

      if (haveGround) {
        // settle feet onto the ground; walk only when footing is known
        float targetY = (float)groundY;
        mob.origin.y += std::clamp(targetY - mob.origin.y, -0.3f, 0.3f);
        bool blocked = haveAhead && aheadY > groundY + 2;  // > step-up reach
        if (blocked && tick > mob.lastTurnTick + 15) {
          mob.heading += 1.5707963f;  // turn 90° and try again
          mob.lastTurnTick = tick;
        } else if (!blocked) {
          mob.origin += fwd * (def.speed * dt);
          mob.phase += def.speed * dt * 2.2f;  // stride frequency
        }
      }

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
      Vec3 yawPivot{cx - mob.origin.x, 0, cz - mob.origin.z};
      Vec3 rootAnchor = def.skel.parts.empty()
                            ? Vec3{}
                            : def.skel.parts[def.rootLimb].anchorLocal;
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
        // model space is anchored at the root's own anchor; rebase to the
        // prefab frame, then rotate about the yaw pivot as before
        Vec3 anchorPrefab = modelPos + rootAnchor;
        Vec3 anchorW = bodyOrigin + yawPivot +
                       Rotate(bodyRot, anchorPrefab - yawPivot);
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
          int decay = std::max(1, gore.severDecayTicks);
          // Triangular weighting: sum over the window of (2*total/decay) *
          // (k/decay) for k = decay..1 is ~= total, so severSpray is the actual
          // droplet count released rather than a rate to be multiplied out.
          float frac = (float)limb.gushTicks / (float)decay;
          int want = (int)std::lround(2.0f * (float)gore.severSpray * frac /
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
            Vec3 dir{axis.x + SignedUnit(h) * gore.severSprayCone,
                     axis.y + SignedUnit(Pcg(h ^ 0x51A17u)) * gore.severSprayCone,
                     axis.z + SignedUnit(Pcg(h ^ 0xB0011u)) * gore.severSprayCone};
            // speed varies +-25% so the jet has depth instead of a hard front
            float sp = gore.severSpraySpeed *
                       (0.75f + 0.5f * (float)(Pcg(h ^ 0x1234u) & 0xFFFFu) / 65535.0f);
            if (!world.CellInWindow({ifloor(origin.x), ifloor(origin.y),
                                     ifloor(origin.z)}))
              break;
            spawns.push_back(MakeDroplet(origin, dir * sp, def.bleedMat, true,
                                         gore.microLifeTicks, gore.microScale));
          }
          limb.gushTicks--;
        }

        if (limb.bleedBudget < 1.0f || bleedOps >= kBleedOpsPerTick) continue;
        if ((tick & 3u) != 0) continue;  // drip every 4th tick
        Vec3 w = limb.body
                     ? limb.xf.pos + Rotate(lq, limb.woundLocal)
                     : bodyFrame(limb.anchorRoot);  // stump on the parent
        ops.push_back({ifloor(w.x), ifloor(w.y), ifloor(w.z), 1,
                       def.bleedMat, 0 /*paint into air*/, 0, 0});
        limb.bleedBudget -= 1.0f;
        bleedOps++;

        // ---- the spray that accompanies the drip ----
        // Rides the drip's existing budget rather than carrying its own: the
        // drip rate is already bounded and already tied to the wound, so spray
        // per drip cannot outrun the bleeding it is depicting.
        int sprayN = (int)std::lround(gore.bleedSprayPerDrip);
        for (int k = 0; k < sprayN; k++) {
          if (spawns.size() >= kMaxParticleSpawnsPerTick) break;
          uint32_t h = Hash3((uint32_t)mob.id * 40503u + (uint32_t)li,
                             tick ^ 0xB1005u, (uint32_t)k * 2246822519u);
          // biased upward and outward: a wound sprays, it does not just drool
          Vec3 dir{SignedUnit(h) * gore.bleedSprayCone,
                   0.6f + 0.4f * std::fabs(SignedUnit(Pcg(h ^ 0x77u))),
                   SignedUnit(Pcg(h ^ 0xC0FFEEu)) * gore.bleedSprayCone};
          spawns.push_back(MakeDroplet(w, dir * gore.bleedSpraySpeed,
                                       def.bleedMat, true, gore.microLifeTicks,
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
      limb.bleedBudget = std::min(limb.bleedBudget + amount * def.bleedPerDamage,
                                  120.0f);
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

      DetachLimb(mob, limbIndex, true);
      // the stump bleeds: wound at the joint on the PARENT side
      for (size_t k = 0; k < def.limbs.size(); k++)
        if (def.limbs[k].name == ld.parent) {
          Limb& parent = mob.limbs[k];
          Quat q{parent.xf.quat[0], parent.xf.quat[1], parent.xf.quat[2],
                 parent.xf.quat[3]};
          parent.woundLocal = RotateInv(q, anchorW - parent.xf.pos);
          parent.bleedBudget = std::min(parent.bleedBudget + 40.0f, 120.0f);

          // Arm the gout. PreTick drains it over severDecayTicks; arming state
          // here rather than emitting now keeps every particle this frame
          // inside the one per-tick spawn budget, and keeps spray order
          // independent of the order limbs happened to be damaged in.
          const auto& gore = CurrentTuning().gore;
          parent.gushTicks = gore.severDecayTicks;
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
          for (int k = 0; k < gore.severVoxels; k++) {
            if (pendingSpawns_.size() >= kMaxParticleSpawnsPerTick) break;
            uint32_t h = Hash3((uint32_t)mob.id * 22695477u + (uint32_t)limbIndex,
                               (uint32_t)k, 0x5EEDu);
            Vec3 dir{parent.gushDir.x + SignedUnit(h) * 0.7f,
                     std::fabs(parent.gushDir.y) + 0.3f,
                     parent.gushDir.z + SignedUnit(Pcg(h ^ 0x31u)) * 0.7f};
            dir = Rotate(q, dir);
            float sp = gore.severVoxelSpeed *
                       (0.6f + 0.8f * (float)(Pcg(h ^ 0x9Fu) & 0xFFFFu) / 65535.0f);
            pendingSpawns_.push_back(MakeDroplet(anchorW, dir * sp, def.bleedMat,
                                                 false, 0, 0));
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
    // severed microvoxel limb keep its detail as ordinary debris (PLAN §C4).
    debris_->AdoptBody(limb.body, limb.voxels, limb.xf, limb.MicroRef(def.scale));
    limb.holdBody = limb.body;
    limb.holdSeconds = kSeverHoldSeconds;
  } else {
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
                       limb.MicroRef(defs_[mob.defIndex].scale));
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
      for (const DebrisVoxel& v : *src) {
        if (out.size() >= kMaxBodyVoxInstances) break;
        out.push_back({(float)v.x, (float)v.y, (float)v.z,
                       (uint32_t)v.payload | (slot << 16)});
      }
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
      BodyXformGpu x{};
      x.pos[0] = limb.xf.pos.x;
      x.pos[1] = limb.xf.pos.y;
      x.pos[2] = limb.xf.pos.z;
      std::memcpy(x.quat, limb.xf.quat, sizeof(x.quat));
      out.push_back(x);
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

uint32_t MobSystem::LimbBodyCount() const {
  uint32_t n = 0;
  for (const Mob& mob : mobs_)
    for (const Limb& limb : mob.limbs)
      if (limb.body) n++;
  return n;
}
