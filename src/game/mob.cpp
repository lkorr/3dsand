#include "game/mob.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>

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
    // Authored volume, so carve damage can be expressed as a FRACTION of the
    // limb — the same wound should read the same on a scale-1 and a scale-4 rig.
    limb.voxelsAtSpawn = (uint32_t)limb.voxels.size();
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
        // A maimed mob keeps moving, just slower: the active dismemberment
        // state scales the drive speed (a crawl covers ground at a fraction
        // of a walk; a fully disarmed prone state is 0 and goes nowhere).
        // Reads LAST tick's state — UpdateAnimation below re-evaluates it —
        // which is at most one tick of lag on a sever.
        float speedScale =
            mob.anim.locoState >= 0 &&
                    mob.anim.locoState < (int)def.skel.states.size()
                ? def.skel.states[mob.anim.locoState].speedScale
                : 1.0f;
        bool blocked = haveAhead && aheadY > groundY + 2;  // > step-up reach
        // speedScale 0 also suppresses the blocked-turn: an immobile mob
        // pirouetting in place at a wall reads as a bug, not as AI.
        if (blocked && speedScale > 0 && tick > mob.lastTurnTick + 15) {
          mob.heading += 1.5707963f;  // turn 90° and try again
          mob.lastTurnTick = tick;
        } else if (!blocked) {
          mob.origin += fwd * (def.speed * speedScale * dt);
          mob.phase += def.speed * speedScale * dt * 2.2f;  // stride frequency
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

void MobSystem::LimbVoxelsToParticles(const Limb& limb, uint32_t defScale,
                                      const std::vector<DebrisVoxel>& voxels,
                                      World& world,
                                      std::vector<ParticleSpawn>& spawns) const {
  // Mirrors DebrisSystem::VoxelsToParticles: a micro limb's voxels are 1/scale
  // of a world voxel, so one particle per voxel would emit `scale^3` times the
  // matter the limb actually lost. Sub-sampling on the micro lattice conserves
  // the visible volume — the grid has no sub-voxel resolution to receive the
  // detail anyway.
  const float inv = 1.0f / (float)std::max(1u, defScale);
  const int step = (int)std::max(1u, defScale);
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

bool MobSystem::ReskinLimbMicro(Mob& mob, Limb& limb, uint32_t defScale) {
  if (limb.microModel < 0 || !microSet_) return false;
  int own = MicroBodyOwn(*microSet_, (uint32_t)limb.microModel);
  if (own < 0) return false;  // pool full: keep the stale skin, stay carved
  limb.microModel = own;
  limb.carved = true;
  std::vector<PrefabVoxel> mv;
  mv.reserve(limb.voxels.size());
  for (const DebrisVoxel& v : limb.voxels)
    mv.push_back({(int16_t)v.x, (int16_t)v.y, (int16_t)v.z,
                  (uint16_t)(v.payload & 0xFFF)});
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
    const float inv = 1.0f / (float)std::max(1u, defScale);
    Vec3 d{(float)shift.x * inv, (float)shift.y * inv, (float)shift.z * inv};
    Quat q{limb.xf.quat[0], limb.xf.quat[1], limb.xf.quat[2], limb.xf.quat[3]};
    limb.xf.pos += Rotate(q, d);
    limb.restOffset += d;
    // The joint anchors are expressed from the limb origin, so they move the
    // opposite way to stay on the same physical point of the creature.
    limb.anchorLimb = limb.anchorLimb - d;
    for (DebrisVoxel& v : limb.voxels) {
      v.x = (int8_t)(v.x - shift.x);
      v.y = (int8_t)(v.y - shift.y);
      v.z = (int8_t)(v.z - shift.z);
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
  const float pitch = 1.0f / (float)std::max(1u, def.scale);
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

void MobSystem::EmitCarvedFragment(Mob& mob, const Limb& src, uint32_t defScale,
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
  const float inv = 1.0f / (float)std::max(1u, defScale);
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
    int m = MicroBodyPack(*microSet_, mv, dims, defScale, "carve", log);
    if (m < 0) {
      LimbVoxelsToParticles(src, defScale, part, world, spawns);
      return;
    }
    // Packed models are SHARED by default; this one belongs to exactly one body
    // and must be freeable with it, or every gobbet leaks pool words.
    if (m < (int)microSet_->owned.size()) microSet_->owned[m] = 1;
    micro = MicroBodyRef{(uint32_t)m, defScale};
  }

  const float pitch = 1.0f / (float)std::max(1u, defScale);
  uint64_t h = phys_->CreateDebrisBodyXf(part, xf, densityOf_, false, pitch);
  if (h == 0) {
    if (micro.Valid()) MicroBodyFree(*microSet_, micro.model);
    LimbVoxelsToParticles(src, defScale, part, world, spawns);
    return;
  }
  // Push it off the wound so it visibly leaves the body rather than resting in
  // the cavity it came from.
  Vec3 away = xf.pos - src.xf.pos;
  float len = away.len();
  away = len > 1e-3f ? away * (1.0f / len) : Vec3{0, 1, 0};
  phys_->SetBodyVelocities(h, away * 2.5f + Vec3{0, 1.5f, 0}, Vec3{});
  debris_->AdoptBody(h, std::move(part), xf, micro);
}

bool MobSystem::CarveLimb(Mob& mob, int limbIndex, World& world,
                          std::vector<ParticleSpawn>& spawns, bool eject,
                          const std::function<bool(const DebrisVoxel&)>& keep) {
  Limb& limb = mob.limbs[limbIndex];
  if (!limb.body || limb.voxels.empty()) return true;
  const MobDef& def = defs_[mob.defIndex];
  // NOTE: limb.xf is deliberately NOT refreshed from Jolt here. `keep` was
  // built against the pose the CALLER measured, and a live limb is kinematic —
  // the animation pipeline re-poses it every tick — so re-reading the transform
  // now would test the voxels against a pose the predicate never saw and carve
  // the wrong cells (or, as the pose drifts, none at all).

  std::vector<DebrisVoxel> removed;
  for (const DebrisVoxel& v : limb.voxels)
    if (!keep(v)) removed.push_back(v);
  if (removed.empty()) return true;  // nothing in range

  if (eject) LimbVoxelsToParticles(limb, def.scale, removed, world, spawns);
  limb.voxels.erase(
      std::remove_if(limb.voxels.begin(), limb.voxels.end(),
                     [&](const DebrisVoxel& v) { return !keep(v); }),
      limb.voxels.end());
  instancesDirty_ = true;

  // A carved limb must stop flipbooking: a frame swap re-points rendering at an
  // intact authored model, which would heal every wound on screen.
  limb.flipbookModel = -1;

  // Losing matter hurts, in proportion to how much of the limb it was. The
  // wound is placed at the carve so the existing bleed machinery sprays from
  // the right spot with no new plumbing.
  const uint32_t at0 = std::max(1u, limb.voxelsAtSpawn);
  const float lost = (float)removed.size() / (float)at0;
  limb.hp -= lost * def.limbs[limbIndex].hp * kCarveDamagePerVolume;
  if (def.bleedMat) {
    Vec3 c{};
    for (const DebrisVoxel& v : removed)
      c += Vec3{(float)v.x, (float)v.y, (float)v.z};
    // Centroid of what was removed, in limb-local WORLD voxels — the frame
    // woundLocal is read in (PreTick rotates it by the limb's live quat).
    limb.woundLocal = c * (1.0f / (float)removed.size() /
                           (float)std::max(1u, def.scale));
    limb.bleedBudget =
        std::min(limb.bleedBudget + lost * (float)at0 * def.bleedPerDamage,
                 120.0f);
  }

  // Carved down past the point of being a limb at all: it comes off. This is
  // the geometric route to dismemberment — no hp threshold decided it, the
  // player simply removed too much of the arm for it to still be an arm.
  const bool collapsed =
      limb.voxels.size() < kMinFragmentVoxels ||
      (float)limb.voxels.size() < kLimbCollapseFraction * (float)at0;
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
      // The anchor in limb-local MICRO units — the point the joint holds.
      Vec3 aMicro = limb.anchorLimb * (float)std::max(1u, def.scale);
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
      limb.voxels = std::move(parts[keepComp]);

      uint32_t budget = kMaxCarveFragments;
      for (uint32_t c = 0; c < (uint32_t)parts.size(); c++) {
        if (c == keepComp || parts[c].empty()) continue;
        if (parts[c].size() >= kMinFragmentVoxels && budget > 0) {
          EmitCarvedFragment(mob, limb, def.scale, std::move(parts[c]), world,
                             spawns);
          budget--;
        } else {
          LimbVoxelsToParticles(limb, def.scale, parts[c], world, spawns);
        }
      }
      // Losing the disconnected mass can itself take the limb under the floor.
      if (limb.voxels.size() < kMinFragmentVoxels ||
          (float)limb.voxels.size() < kLimbCollapseFraction * (float)at0) {
        Sever(mob.id, limbIndex);
        return false;
      }
    }
  }

  // Art, then collider. ReskinLimbMicro may shift the limb origin, and the
  // collider must be built from the voxels in their FINAL frame.
  if (limb.microModel >= 0) ReskinLimbMicro(mob, limb, def.scale);
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
      // Carve centre into limb-local MICRO units, so the per-voxel test is a
      // plain distance compare in the frame the voxels live in. This is what
      // makes precision scale with the def: at scale 4 a radius of 0.25 world
      // voxels is a single micro voxel, so a fine enough tool can take out one
      // cell of a brain without any new code path.
      const float scale = (float)std::max(1u, def.scale);
      Quat q{limb.xf.quat[0], limb.xf.quat[1], limb.xf.quat[2], limb.xf.quat[3]};
      Vec3 cLocal = RotateInv(q, centerWorldVoxel - limb.xf.pos) * scale;
      const float rLocal = radiusVoxels * scale;
      const float rLocal2 = rLocal * rLocal;
      const uint32_t seed = (uint32_t)mob.id * 2654435761u + (uint32_t)i;
      CarveLimb(mob, (int)i, world, spawns, eject,
                [&](const DebrisVoxel& v) {
                  Vec3 c{(float)v.x + 0.5f, (float)v.y + 0.5f, (float)v.z + 0.5f};
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
                  uint32_t h = Hash3(seed, (uint32_t)(int32_t)v.x * 73856093u,
                                     (uint32_t)(int32_t)v.y * 19349663u ^
                                         (uint32_t)(int32_t)v.z * 83492791u);
                  return (float)(h & 0xFFFFu) / 65535.0f >= chance;
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
          // cheap reject against the limb's bounding sphere
          float r = 0;
          const float inv = 1.0f / (float)std::max(1u, defs_[mob.defIndex].scale);
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
          for (int k = 0; k < nVox; k++) {
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
    // severed microvoxel limb keep its detail as ordinary debris (PLAN §C4) —
    // and, for a CARVED limb, what makes it keep its wounds. The brick this
    // limb owns is transferred, not copied, so `carved` is cleared: from here
    // DebrisSystem's ReleaseBody is the one thing that may free it, and a limb
    // that also thought it owned the model would double-free the block.
    debris_->AdoptBody(limb.body, limb.voxels, limb.xf, limb.MicroRef(def.scale));
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
                       limb.MicroRef(defs_[mob.defIndex].scale));
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
    const float inv = 1.0f / (float)std::max(1u, defs_[mob.defIndex].scale);
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
