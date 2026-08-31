#include "game/mob.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "game/item.h"
#include "game/rigrender.h"
#include "phys/lattice.h"
#include "sim/bytestream.h"
#include "sim/reactcpu.h"
#include "sim/rng.h"
#include "sim/scale.h"   // SkinScaleFor / NeededArtUpsample / MetresToCells
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

// ---- correlated noise for the blast crater (Tuning::Gore carve*) -----------
//
// WHITE NOISE CANNOT MAKE A CHUNK. An independent draw per voxel has no feature
// size at all, so whatever radial falloff it is thresholded against, what comes
// off is a fine speckle — which is exactly the reported "thin scatter of voxels
// spread over the whole body". Correlating the draws over a few voxels is what
// makes neighbours leave together, and the correlation length IS the size of the
// piece that tears away.
//
// Ordinary trilinear value noise: hash the eight lattice corners, smoothstep
// between them. Keyed off the same rng::Hash3 the rim jitter uses, so it is
// reproducible for a given (mob, limb) and a replay tears the same way. Limbs
// are outside the hashed grid domain, so floats here are legal (rule 1 governs
// the sim, not gameplay state) — the same licence the jitter already takes.
float ValueNoise3(uint32_t seed, float x, float y, float z) {
  const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
  const int ix = (int)fx, iy = (int)fy, iz = (int)fz;
  const float tx = x - fx, ty = y - fy, tz = z - fz;
  // Smoothstep the interpolants, or the field is C0 and the crater rim shows
  // the lattice as flat facets.
  auto sm = [](float t) { return t * t * (3.0f - 2.0f * t); };
  const float ux = sm(tx), uy = sm(ty), uz = sm(tz);
  auto corner = [&](int cx, int cy, int cz) {
    const uint32_t h = Hash3(seed ^ 0x9E3779B9u, (uint32_t)(cx * 73856093),
                             (uint32_t)(cy * 19349663) ^
                                 (uint32_t)(cz * 83492791));
    return (float)(h & 0xFFFFu) / 65535.0f;
  };
  auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
  const float c000 = corner(ix, iy, iz), c100 = corner(ix + 1, iy, iz);
  const float c010 = corner(ix, iy + 1, iz), c110 = corner(ix + 1, iy + 1, iz);
  const float c001 = corner(ix, iy, iz + 1), c101 = corner(ix + 1, iy, iz + 1);
  const float c011 = corner(ix, iy + 1, iz + 1);
  const float c111 = corner(ix + 1, iy + 1, iz + 1);
  const float x00 = lerp(c000, c100, ux), x10 = lerp(c010, c110, ux);
  const float x01 = lerp(c001, c101, ux), x11 = lerp(c011, c111, ux);
  return lerp(lerp(x00, x10, uy), lerp(x01, x11, uy), uz);
}

// Event-scoped variance, applied at the spray sites. Entity-scoped variances
// were already resolved into mob.gore_ at spawn, so these pass through — this is
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

// ---- ball-joint limits, by tag ---------------------------------------------
//
// A ball joint used to be a bare point constraint: position welded, rotation
// completely free. Alive that never showed, because live limbs are kinematic
// and a constraint cannot move infinite mass. Dead, it is the whole ragdoll —
// and since one mob's limbs are excluded from colliding with each other
// (DisableCollisionsAmong, or they fight their own joints and never sleep),
// the cone is the ONLY thing that keeps a corpse's parts out of each other. A
// waist that can fold 180 degrees puts the torso inside the pelvis.
//
// Angles are half-angles in radians, measured from the limb's rest direction.
// `fwd` is swing in the limb's fore/aft plane and `side` is out of it, which
// is the distinction that lets a hip take a full stride without doing the
// splits. Nothing here is anatomy for its own sake — these are the loosest
// values at which the parts still read as attached.
struct JointLimits {
  float fwd, side, twist, friction;
};

JointLimits DefaultJointLimits(const std::string& tag) {
  constexpr float kDeg = 3.14159265f / 180.0f;
  // The two the corpses actually broke on. The waist carries the torso's whole
  // mass on one joint, so it is the one that reads worst when it is wrong.
  if (tag == "spine") return {40 * kDeg, 25 * kDeg, 25 * kDeg, 0.35f};
  if (tag == "leg") return {80 * kDeg, 30 * kDeg, 20 * kDeg, 0.25f};
  if (tag == "arm") return {90 * kDeg, 70 * kDeg, 50 * kDeg, 0.15f};
  if (tag == "head") return {50 * kDeg, 35 * kDeg, 60 * kDeg, 0.30f};
  if (tag == "hand" || tag == "foot") return {50 * kDeg, 50 * kDeg, 60 * kDeg, 0.15f};
  if (tag == "tail") return {60 * kDeg, 60 * kDeg, 35 * kDeg, 0.10f};
  // Untagged (dummy.json) and anything new: the 90 degrees that is the whole
  // point — a limb may reach square to its parent and no further, so it can
  // never end up inside it.
  return {90 * kDeg, 90 * kDeg, 60 * kDeg, 0.15f};
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
  // Same reasoning for the art palette: slots are positions in this merged
  // list, so carrying one across a reload would repaint every def with the
  // previous load's colours.
  micro.artColors.clear();
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

    // Fold this file's art colours into the one palette every def resolves
    // against, and rewrite its voxels' slots to the merged numbering NOW —
    // before anything copies them into a limb, a flipbook frame or a micro
    // brick. Doing it here means exactly one place understands the remap.
    if (!def.prefab.artColors.empty()) {
      const std::vector<uint8_t> remap =
          MicroBodyMergeArt(micro, def.prefab.artColors, def.name, log);
      for (PrefabModel& pm : def.prefab.models)
        for (PrefabVoxel& v : pm.voxels)
          if (v.color) v.color = remap[v.color];
    }

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
      // NOT rescaled: this is a COUNT of voxels a wound may still owe, i.e.
      // a volume budget, and volume goes as the cube of the voxel scale. It
      // is gore rate rather than size or shape, so it is left as authored
      // and flagged here instead of being silently cubed.
      def.bleedPerDamage = j["bleed"].value("perDamage", 1.5f);
    }
    // ---- WORLD-SPACE SIDECAR LENGTHS (DESIGN.md §3b) --------------------
    // `speed`, `severImpactSpeed`, `gait.rideHeight`, `states[].bodyYOffset`
    // and clip position keys are WORLD VOXELS (or voxels/sec), not art
    // units -- so `artVoxelsPerMetre` does not describe them and the art
    // upsample must not touch them. They still have a scale, though: every
    // one was authored when the world was 10 voxels/metre, which is what
    // `sidecarVoxelsPerMetre` writes down.
    //
    // Without this, a correctly-sized 1.7 m human walked at half its
    // authored speed in m/s the moment kVoxelMeters halved -- the body the
    // right size, moving through the world at the wrong one, against a
    // stride budget that avatar.cpp:54 already documents as barely
    // positive.
    const int sidecarVpm =
        j.value("sidecarVoxelsPerMetre", kLegacyAuthoringVoxelsPerMetre);
    const float worldLen =
        (float)kVoxelsPerMetre / (float)(sidecarVpm > 0 ? sidecarVpm
                                                       : kVoxelsPerMetre);
    def.speed = j.value("speed", 4.0f) * worldLen;

    // Sound slots (assets/sound_schema.js). Presentation only, so a bad entry
    // is skipped rather than failing the def — a mob with a typo'd sound name
    // should still walk into the world, silently.
    def.sounds.clear();
    if (j.contains("sounds") && j["sounds"].is_object())
      for (auto& [slot, name] : j["sounds"].items())
        if (name.is_string() && !name.get<std::string>().empty())
          def.sounds[slot] = name.get<std::string>();

    // Declared here rather than below the limb loop: the art-scale block that
    // follows can fail a def outright (a scale that admits no integer
    // skinScale), and that has to reach the same `ok` the limb checks use.
    bool ok = true;

    // ---- ART SCALE (DESIGN.md §3b) ------------------------------------------
    //
    // `artVoxelsPerMetre` is the authored fact; `skinScale` is DERIVED from it
    // and the engine's kVoxelsPerMetre. See MobDef::artVoxelsPerMetre for why
    // the direction matters — authoring skinScale directly encoded "and the
    // world is 10 cm" into every sidecar, so the human halved in metres the
    // moment kVoxelMeters did.
    //
    // "skinScale"/"scale" survive as LEGACY: they said art voxels per WORLD
    // voxel, and every asset that used them was authored at 10 voxels/metre, so
    // that is the reading they get. Loud, because a modded asset silently
    // assumed to be 10 vox/m is exactly the failure this replaces.
    if (j.contains("artVoxelsPerMetre")) {
      def.artVoxelsPerMetre = j.value("artVoxelsPerMetre", 0);
    } else {
      const bool authoredSkin = j.contains("skinScale");
      const uint32_t legacy = j.value("skinScale", j.value("scale", 1u));
      def.artVoxelsPerMetre = (int)legacy * kLegacyAuthoringVoxelsPerMetre;
      log += jp + ": no \"artVoxelsPerMetre\"; reading legacy " +
             std::string(authoredSkin ? "skinScale" : "scale") + " " +
             std::to_string(legacy) + " as " +
             std::to_string(def.artVoxelsPerMetre) +
             " art voxels/metre (authored at " +
             std::to_string(kLegacyAuthoringVoxelsPerMetre) +
             " voxels/metre). Declare artVoxelsPerMetre to be explicit.\n";
    }

    if (def.artVoxelsPerMetre <= 0) {
      log += jp + ": artVoxelsPerMetre must be positive (got " +
             std::to_string(def.artVoxelsPerMetre) + ") — using " +
             std::to_string(kVoxelsPerMetre) + " (art is 1:1 with world)\n";
      def.artVoxelsPerMetre = kVoxelsPerMetre;
      ok = false;
    }

    // Art coarser than the world has no integer skinScale, so replicate the
    // grid until it does. Lossless, and it costs u^3 voxels — but it is the
    // only way a 10 vox/m asset can hold its metre size in a 20 vox/m world
    // without being redrawn.
    def.artUpsample = NeededArtUpsample(def.artVoxelsPerMetre);
    def.skinScale =
        SkinScaleFor(def.artVoxelsPerMetre * (int)def.artUpsample);
    if (def.skinScale != 1 && def.skinScale != 2 && def.skinScale != 4 &&
        def.skinScale != 8) {
      log += jp + ": artVoxelsPerMetre " +
             std::to_string(def.artVoxelsPerMetre) + " gives skinScale " +
             std::to_string(def.skinScale) + " at " +
             std::to_string(kVoxelsPerMetre) +
             " world voxels/metre, which must be 1, 2, 4 or 8 — the art scale "
             "has to be a power-of-two multiple of the world scale. Using 1; "
             "this mob will be the wrong physical size.\n";
      def.skinScale = 1;
      def.artUpsample = 1;
      ok = false;
    }
    if (def.artUpsample > 1) {
      UpsamplePrefab(def.prefab, def.artUpsample);
      log += def.name + ": art is " + std::to_string(def.artVoxelsPerMetre) +
             " vox/m in a " + std::to_string(kVoxelsPerMetre) +
             " vox/m world — block-replicated " +
             std::to_string(def.artUpsample) + "x to skinScale " +
             std::to_string(def.skinScale) + " (same size, no new detail)\n";
    }

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
      ld.severImpactSpeed = l.value("severImpactSpeed", 0.0f) * worldLen;
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
      // Ball-joint cone, by TAG. Every rig in the tree already tags its parts
      // ("spine", "leg", "arm", ...) for the gait and the IK chains, so the
      // tag is the authoring surface that already exists — a per-limb angle in
      // every sidecar would be five files restating the same anatomy. A limb
      // that wants something else says so and overrides.
      {
        const JointLimits jl = DefaultJointLimits(ld.tag);
        const float cone = l.value("cone", jl.fwd);
        ld.coneFwd = cone;
        // "coneSide" defaults to "cone" when only the one number is authored,
        // so `"cone": 0.5` means a plain symmetric cone and nothing surprising
        // leaks in from the tag table.
        ld.coneSide = l.value("coneSide", l.contains("cone") ? cone : jl.side);
        ld.twistLimit = l.value("twist", jl.twist);
        ld.jointFriction = l.value("jointFriction", jl.friction);
      }
      // Pose-space range of motion. Deliberately NOT folded into the "cone"
      // block above: that one is a Jolt constraint on a dynamic body and this
      // one bounds the animated pose, and conflating them is exactly the
      // confusion that let an IK-driven thigh swing to any angle it liked while
      // a perfectly good-looking ragdoll limit sat in the same sidecar. Degrees
      // in, radians out, like `edge` converts its units at the same point.
      if (l.contains("poseLimit") && l["poseLimit"].is_object()) {
        const json& pl = l["poseLimit"];
        const float kDeg = 3.14159265f / 180.0f;
        auto vec3 = [&](const char* key, Vec3 dflt) {
          if (!pl.contains(key) || pl[key].size() != 3) return dflt;
          return Vec3{pl[key][0].get<float>(), pl[key][1].get<float>(),
                      pl[key][2].get<float>()};
        };
        // BALL FORM (a shoulder): bounded by where the bone may POINT, not by
        // one rotation component. Selected by the presence of `bone`, because
        // the bone direction is the thing the axis form has no use for and the
        // ball form cannot work without.
        if (pl.contains("bone")) {
          PoseBallLimit& b = ld.poseBall;
          b.has = true;
          b.bone = vec3("bone", {0, -1, 0});
          if (b.bone.len() < 1e-5f) {
            log += jp + ": limb \"" + ld.name + "\" poseLimit.bone is zero\n";
            ok = false;
          } else {
            b.bone = b.bone.normalized();
          }
          if (pl.contains("reach") && pl["reach"].is_array()) {
            for (const json& r : pl["reach"]) {
              if (b.reachCount >= 2) {
                log += jp + ": limb \"" + ld.name +
                       "\" poseLimit.reach takes at most 2 planes (see "
                       "PoseBallLimit in anim.h)\n";
                ok = false;
                break;
              }
              Vec3 nrm{0, 0, 1};
              if (r.contains("normal") && r["normal"].size() == 3)
                nrm = {r["normal"][0].get<float>(), r["normal"][1].get<float>(),
                       r["normal"][2].get<float>()};
              if (nrm.len() < 1e-5f) {
                log += jp + ": limb \"" + ld.name +
                       "\" poseLimit.reach normal is zero\n";
                ok = false;
                continue;
              }
              nrm = nrm.normalized();
              // The closed-form projection is only the NEAREST legal direction
              // when the planes are perpendicular; a tilted pair would be
              // solved wrong and look almost right, so it is a load error.
              if (b.reachCount == 1 &&
                  std::fabs(nrm.dot(b.reachNormal[0])) > 1e-3f) {
                log += jp + ": limb \"" + ld.name +
                       "\" poseLimit.reach normals must be perpendicular\n";
                ok = false;
              }
              b.reachNormal[b.reachCount] = nrm;
              // "at most N degrees past this plane" — the plane itself is 0.
              b.reachSin[b.reachCount] =
                  std::sin(std::clamp(r.value("max", 0.0f), -90.0f, 90.0f) *
                           kDeg);
              b.reachCount++;
            }
          }
          if (pl.contains("twist") && pl["twist"].is_object()) {
            b.hasTwist = true;
            b.twistMin = pl["twist"].value("min", -180.0f) * kDeg;
            b.twistMax = pl["twist"].value("max", 180.0f) * kDeg;
            if (b.twistMin > b.twistMax) std::swap(b.twistMin, b.twistMax);
          }
        } else {
          ld.hasPoseLimit = true;
          ld.poseAxis = vec3("axis", ld.poseAxis);
          ld.poseMin = pl.value("min", -180.0f) * kDeg;
          ld.poseMax = pl.value("max", 180.0f) * kDeg;
          if (ld.poseMin > ld.poseMax) std::swap(ld.poseMin, ld.poseMax);
          // A hinge is one DOF, not one bounded DOF: it also discards the
          // off-axis swing (anim.h, AnimPart::poseHinge).
          ld.poseHinge = pl.value("hinge", false);
        }
      }
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
        p.hasPoseLimit = ld.hasPoseLimit;
        p.poseAxis = ld.poseAxis;
        p.poseMin = ld.poseMin;
        p.poseMax = ld.poseMax;
        p.poseHinge = ld.poseHinge;
        p.poseBall = ld.poseBall;
      }
      // rest transforms come from the .vox layout: a part's local rest
      // position is its joint anchor relative to the parent's anchor.
      for (size_t i = 0; i < def.limbs.size(); i++) {
        const MobLimbDef& ld = def.limbs[i];
        int mi = FindModel(def.prefab, ld.name);
        // TWO LATTICES MEET HERE, and they are not the same one after an art
        // upsample (DESIGN.md §3b). `ld.anchor` is AUTHORED, so it is in the
        // sidecar's original art grid; AutoAnchor and the root fallback are
        // DERIVED FROM def.prefab, which UpsamplePrefab may have multiplied by
        // artUpsample. Dividing both by skinScale would put every authored
        // joint of an upsampled rig at 1/artUpsample of its intended offset —
        // a rig that collapses toward its own origin, with the art still
        // looking perfectly correct.
        //
        // ArtToWorld() divides the upsample back out; the derived branches take
        // the plain skin->world divisor. They agree exactly when artUpsample
        // is 1, which is every asset that has not been redrawn coarser than
        // the world.
        Vec3 anchor = ld.anchor;
        float aInv = def.ArtToWorld();
        const float prefabInv = 1.0f / (float)def.skinScale;
        if (ld.anchorAuto && (int)i != def.rootLimb) {
          int pmi = FindModel(def.prefab, ld.parent);
          if (mi >= 0 && pmi >= 0) {
            anchor = AutoAnchor(def.prefab.models[mi], def.prefab.models[pmi]);
            aInv = prefabInv;
          }
        } else if ((int)i == def.rootLimb && mi >= 0) {
          const PrefabModel& m = def.prefab.models[mi];
          anchor = Vec3{(float)m.offset.x + m.size.x * 0.5f, (float)m.offset.y,
                        (float)m.offset.z + m.size.z * 0.5f};
          aInv = prefabInv;
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
        sk.parts[i].anchorLocal = anchor * aInv;
        // The cutting edge rides the same conversion, for the same reason: it
        // is rig geometry, and every consumer downstream works in world
        // voxels. Its offsets are measured from the part's own ORIGIN (the
        // model's min corner), so they need no anchor rebasing here — the
        // melee sweep composes them with the part transform, which already
        // carries the origin.
        MobLimbDef& mld = def.limbs[i];
        if (mld.hasEdge) {
          // Always authored, so always the art-frame divisor.
          const float eInv = def.ArtToWorld();
          mld.edgeFrom = mld.edgeFrom * eInv;
          mld.edgeTo = mld.edgeTo * eInv;
          mld.edgeHalfWidth *= eInv;
        }
        // THE BONE: from the joint anchor to the limb's own centre, in the
        // rest pose. This is the centre line of the ball joint's swing cone
        // (mob.h MobLimbDef::boneAxis), and it is derived from the same two
        // pieces of rig geometry the pose pipeline uses rather than authored,
        // for the same reason the anchor is not restated per consumer: a cone
        // centred anywhere but the rest direction parks the limb against its
        // own limit while it is still standing.
        //
        // It is normalised, so a common scale would cancel — but the two terms
        // do NOT share one after an art upsample: `centre` is measured off the
        // (possibly replicated) prefab and `anchor` may be authored in the
        // original grid. Convert each with its own divisor first; when
        // artUpsample is 1 they are the same number and this is the old
        // expression exactly.
        if ((int)i != def.rootLimb && mi >= 0) {
          const PrefabModel& m = def.prefab.models[mi];
          const Vec3 centre{(float)m.offset.x + m.size.x * 0.5f,
                            (float)m.offset.y + m.size.y * 0.5f,
                            (float)m.offset.z + m.size.z * 0.5f};
          const Vec3 bone = centre * prefabInv - anchor * aInv;
          // A limb whose centre IS its anchor (a ball-shaped head sat exactly
          // on the neck) has no direction to speak of; straight down is the
          // rig-neutral guess and the cone stays symmetric about it.
          mld.boneAxis = bone.len() > 1e-4f ? bone.normalized() : Vec3{0, -1, 0};
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
      // Authored, so it is the art-frame divisor (ArtToWorld), not the raw
      // 1/skinScale — see the two-lattices note at the anchors above.
      {
        const float inv = def.ArtToWorld();
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
        // World voxels: a per-rig trim of about one cell at the authored
        // scale, so it has to follow the scale or the rig sinks.
        gd.rideHeight = g.value("rideHeight", 0.9f) * worldLen;
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
        rule.bodyYOffset = s.value("bodyYOffset", 0.0f) * worldLen;
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
            // NOT art units, and so NOT subject to the art upsample: a clip's
            // position track is a DELTA added straight onto `rest.pos`
            // (anim.cpp:385), which is world voxels. Multiplying these by
            // artUpsample the way the anchors are would displace every keyed
            // translation of an upsampled rig.
            //
            // They are world voxels rather than metres, though, which makes
            // them a voxel-size dependency of their own — scaled below with the
            // rest of the sidecar's world-space lengths.
            for (const auto& k : t.value().value("pos", json::array())) {
              AnimKey& key = upsert(k.value("t", 0));
              key.pos = JsonVec3(k.contains("v") ? k["v"] : json(), {}) *
                        worldLen;
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
                     const std::vector<MaterialDef>& mats,
                     const std::vector<ReactionGpu>& reactions) {
  phys_ = phys;
  world_ = world;
  debris_ = debris;
  OnMaterialsReloaded(mats, reactions);
}

void MobSystem::OnMaterialsReloaded(const std::vector<MaterialDef>& mats,
                                    const std::vector<ReactionGpu>& reactions) {
  densityOf_.clear();
  classOf_.clear();
  matGpu_.clear();
  matSelfActive_.clear();
  matHasPair_.clear();
  matHot_.clear();
  matRewritesNbr_.clear();
  matAttacksBody_.clear();
  ignitedForm_.clear();
  reactions_ = reactions;
  // The tag bit for "hot" is looked up ONCE, by name, from whatever material
  // declares it — there is no hardcoded id and no hardcoded bit. A material
  // becomes a heat source by carrying the tag, which is the same contract the
  // grid's combustion rules already work by.
  // The BIT one tag name owns, recovered from the compiled masks: the bits
  // common to every material carrying the tag, minus every bit any material
  // without it carries. Exact, and it needs no access to the TagRegistry —
  // which is a load-time local and deliberately not published.
  //
  // Doing it as a plain OR would be wrong in a way that only shows up later:
  // every material carrying `hot` also carries `organic` now that flesh can
  // burn, so an OR-derived "hot" mask would quietly mean "hot or organic".
  auto tagBit = [&](const char* name) {
    uint32_t all = 0xFFFFFFFFu, none = 0;
    bool any = false;
    for (const auto& m : mats) {
      bool has = false;
      for (const auto& t : m.tags)
        if (t == name) has = true;
      if (has) { all &= m.gpu.tagMask; any = true; } else { none |= m.gpu.tagMask; }
    }
    return any ? (all & ~none) : 0u;
  };
  const uint32_t hotMask = tagBit("hot");
  const uint32_t dissolvableMask = tagBit("dissolvable");

  for (const auto& m : mats) {
    densityOf_.push_back((float)m.gpu.density);
    // Collision class, not raw klass: passable vegetation reads as gas so a
    // mob's ground probe walks through reeds instead of standing on them or
    // treating a reed bed as a wall (sim/materials.h kMatFlagPassable).
    classOf_.push_back((m.gpu.flags & kMatFlagPassable) ? (uint32_t)CLASS_GAS
                                                        : m.gpu.klass);
    matGpu_.push_back(m.gpu);
    uint8_t selfActive = 0, hasPair = 0, rewritesNbr = 0;
    for (uint32_t ri = 0; ri < m.gpu.reactCount; ri++) {
      const ReactionGpu& r = reactions_[m.gpu.reactOffset + ri];
      const uint32_t kind = r.packed & 3u;
      if (kind == kReactDecay || kind == kReactEmit) selfActive = 1;
      if (kind == kReactPair) {
        hasPair = 1;
        if (r.prodNbr != kProdKeep) rewritesNbr = 1;
      }
    }
    matSelfActive_.push_back(selfActive);
    matHasPair_.push_back(hasPair);
    matRewritesNbr_.push_back(rewritesNbr);
    matHot_.push_back((m.gpu.tagMask & hotMask) != 0 ? 1 : 0);
    // Could any of those rewrites land on a creature? See matAttacksBody_.
    uint8_t attacks = 0;
    for (uint32_t ri = 0; ri < m.gpu.reactCount; ri++) {
      const ReactionGpu& r = reactions_[m.gpu.reactOffset + ri];
      if ((r.packed & 3u) != kReactPair || r.prodNbr == kProdKeep) continue;
      // A wildcard predicate matches everything, so it matches a body too.
      if (r.nbrMat == kNbrAny && r.nbrTags == 0) { attacks = 1; break; }
      if (r.nbrTags & dissolvableMask) { attacks = 1; break; }
      if (r.nbrMat != kNbrAny && r.nbrMat < mats.size() &&
          (mats[r.nbrMat].gpu.tagMask & dissolvableMask)) { attacks = 1; break; }
    }
    matAttacksBody_.push_back(attacks);
  }
  // What each material becomes when it CATCHES: the product of the first rule
  // in its bucket whose product is itself hot. Resolved from the table so the
  // ignition entry point never names a material — bone and steel refuse
  // because nothing in their buckets produces heat, not because of a list.
  for (const auto& m : mats) {
    uint32_t hot = 0;
    for (uint32_t ri = 0; ri < m.gpu.reactCount && !hot; ri++) {
      const ReactionGpu& r = reactions_[m.gpu.reactOffset + ri];
      const uint32_t kind = r.packed & 3u;
      const uint32_t prod = kind == kReactDecay ? r.prodSelf
                            : kind == kReactPair ? r.prodSelf
                                                 : (uint32_t)kProdKeep;
      if (prod == kProdKeep || prod == 0) continue;
      const uint32_t pm = prod & 0xFFFu;
      if (pm < matHot_.size() && matHot_[pm]) hot = pm;
    }
    ignitedForm_.push_back(hot);
  }
}

void MobSystem::SetDefs(std::vector<MobDef> defs) { defs_ = std::move(defs); }

void MobSystem::Reset(bool rewindIds) {
  for (Mob& m : mobs_) m.ReleaseRig();
  mobs_.clear();
  // THE ID COUNTER IS DELIBERATELY NOT REWOUND BY DEFAULT.
  //
  // A mob id is not just a handle: it seeds the entity-scoped gore variance
  // (MakeGoreProfile) and the blast crater's noise (Mob::CarveLimbRadial keys
  // on `id_ * 2654435761u`). So rewinding it is a real behaviour change —
  // the next creature spawned after a reset inherits the previous first
  // creature's gore personality and tears the same way.
  //
  // It is tempting to call that "correct", since Reset() otherwise restores
  // the initial state. It was tried, and it MOVED A GATE: `mob-burn`'s
  // cloth-vs-flesh subtest burned a mina to 100% of both instead of 95%/51%,
  // in-suite only, because her burn draw is id-keyed. Shipping randomness is
  // not worth re-rolling to make a test tidy.
  //
  // `rewindIds` is that test's seam and nothing else uses it: an A/B that runs
  // the same tuning twice around a changed one needs the id-seeded draws to
  // repeat, or its two control arms disagree and the middle arm proves nothing
  // (measured: 164 voxels vs 155 with the counter running).
  if (rewindIds) nextId_ = 1;
  instancesDirty_ = true;
}

// Tear down every body, joint and brick this rig still owns. The shared half
// of despawn/reset — the avatar's Despawn and the mob despawn sweep both end
// here, so neither can forget the brick return or the hold release.
void Mob::ReleaseRig() {
  for (MobLimb& l : limbs_) {
    // held pieces are DebrisSystem's now; only drop the kinematic hold
    if (l.holdBody) {
      phys_->SetBodyKinematic(l.holdBody, false);
      phys_->ClearCollisionGroup(l.holdBody);
      OnBodyReleasedToWorld(l.holdBody);
      l.holdBody = 0;
    }
    // A carved limb owns a copy-on-write brick; dropping the rig without
    // returning it leaks pool words nothing will ever reclaim.
    ReleaseLimbMicro(l);
    Mob::DropBurnIndex(l.burn);
    if (l.joint) {
      phys_->DestroyJoint(l.joint);
      l.joint = 0;
    }
    if (l.body) phys_->RemoveBody(l.body);
    l.body = 0;
  }
}

// ---- shared-service accessors ----------------------------------------------
// The burn tables, micro pool and material tables live on MobSystem (one per
// game); every creature — NPC or avatar — borrows them through these.
MicroBodySet* Mob::MicroSet() const { return sys_ ? sys_->microSet_ : nullptr; }
const std::vector<float>& Mob::DensityOf() const { return sys_->densityOf_; }
const std::vector<uint32_t>& Mob::ClassOf() const { return sys_->classOf_; }

void Mob::MarkInstancesDirty() {
  // NPCs share MobSystem's instance list; the avatar overrides this to mark
  // its own (game/avatar.h).
  if (sys_) sys_->instancesDirty_ = true;
}

// Draws this mob's own bleed character. Entity-scoped variances resolve here,
// once, against the mob id; event-scoped ones are left alone (they are drawn
// per droplet at the spray sites, where the droplet index is in hand).
//
// The whole-wound gain is folded into every QUANTITY (spray counts, thrown
// voxels) but deliberately NOT into speeds, cones or lifetimes: "this one is a
// gusher" should mean more blood, not blood that also flies faster and lives
// longer, which reads as a different material rather than a worse wound.
GoreProfile Mob::MakeGoreProfile(uint64_t mobId) {
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
  for (Mob& mob : mobs_) mob.gore_ = Mob::MakeGoreProfile(mob.id_);
}

Mob* MobSystem::FindMobById(uint64_t id) {
  for (Mob& mob : mobs_)
    if (mob.id_ == id) return &mob;
  return nullptr;
}

bool MobSystem::EquipItem(uint64_t mobId, const ItemDef* item,
                          const char* context) {
  Mob* mob = FindMobById(mobId);
  return mob ? mob->EquipItem(item, context) : false;
}

bool MobSystem::WearItem(uint64_t mobId, const ItemDef* item, int equipSlot) {
  Mob* mob = FindMobById(mobId);
  return mob ? mob->WearItem(item, equipSlot) : false;
}

bool MobSystem::UnwearItem(uint64_t mobId, int equipSlot) {
  Mob* mob = FindMobById(mobId);
  return mob ? mob->UnwearItem(equipSlot) : false;
}

uint64_t MobSystem::Spawn(int defIndex, IVec3 atVoxel) {
  if (defIndex < 0 || defIndex >= (int)defs_.size()) return 0;
  if (mobs_.size() >= kMaxMobs) return 0;
  const MobDef& def = defs_[defIndex];

  Mob mob;
  mob.sys_ = this;
  mob.phys_ = phys_;
  mob.world_ = world_;
  mob.debris_ = debris_;
  mob.id_ = nextId_++;
  mob.gore_ = Mob::MakeGoreProfile(mob.id_);
  mob.defIndex_ = defIndex;
  mob.def_ = &def;
  if (!mob.BuildRig(def, Vec3{(float)atVoxel.x, (float)atVoxel.y,
                              (float)atVoxel.z}))
    return 0;
  mobs_.push_back(std::move(mob));
  instancesDirty_ = true;
  return mobs_.back().id_;
}

// Build limbs, bodies, joints and animation state from `def`, prefab min
// corner at `origin` (world voxels). THE one rig-construction path: NPCs and
// the player avatar both assemble here, so a change to how a creature is
// built cannot reach one and miss the other. False = physics refused a body;
// everything created so far is torn down.
bool Mob::BuildRig(const MobDef& def, Vec3 origin) {
  if (!phys_ || !sys_) return false;
  origin_ = origin;
  bodyUp_ = Vec3{0, 1, 0};
  footInit_ = false;
  speedNow_ = 0;
  anim_ = AnimState{};
  // The rig this instance animates is a COPY of the def's (mob.h): a held
  // item borrows a slot by APPENDING a part, and the shared def must not grow
  // a sword every time somebody picks one up. Reset here so a respawn never
  // inherits the last life's weapon.
  skel_ = def.skel;
  limbDefs_ = def.limbs;
  hidden_.assign(def.limbs.size(), 0);
  heldSlot_ = -1;
  heldItem_.clear();
  heldPartIndex_ = -1;
  heldPart_.clear();
  // Same rule as the weapon, and for the same reason: a respawn must not
  // inherit the last life's coat. The armour itself is not lost — it lives in
  // the wearer's Equipment, which is a different owner — but the SHELLS are
  // rig state and belong to this rig only.
  worn_.clear();
  baseLimbs_ = (int)def.limbs.size();
  limbs_.assign(def.limbs.size(), MobLimb{});

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
    MobLimb& limb = limbs_[i];
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
        // `color` rides along untouched: it is ART, independent of the
        // material, and only the material reaches the collider below.
        limb.skinVoxels.push_back(
            {v.x, v.y, v.z, (uint16_t)(v.material | (variant << 12)), v.color});
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
        limb.voxels.push_back({(int8_t)v.x, (int8_t)v.y, (int8_t)v.z, v.color,
                               (uint16_t)(v.material | (variant << 12))});
      }
    }
    // Authored volume, so carve damage can be expressed as a FRACTION of the
    // limb — the same wound should read the same on a scale-1 and a scale-4 rig.
    // Counted on the lattice a carve actually removes from, so the fraction is
    // measured against the same denominator that shrinks.
    limb.voxelsAtSpawn = (uint32_t)(limb.HasFineSkin() ? limb.skinVoxels.size()
                                                       : limb.voxels.size());
    limb.voxelsCharged = limb.voxelsAtSpawn;  // nothing lost, nothing charged
    // The body origin is the limb's min corner in WORLD voxels; the collider
    // is built at pitch 1/physScale so its collider-unit local coordinates land
    // in the right physical place. Not an integer cell any more at scale>1,
    // which is fine — a Jolt body has never been lattice-aligned.
    Vec3 o = origin_ + limb.restOffset;
    BodyTransform bxf{};
    bxf.pos = o;
    bxf.quat[3] = 1;
    limb.body = phys_->CreateDebrisBodyXf(limb.voxels, bxf, DensityOf(),
                                          true /*allowKinematic*/, physInv);
    if (limb.body == 0) {
      for (MobLimb& l : limbs_)
        if (l.body) phys_->RemoveBody(l.body);
      limbs_.clear();
      return false;
    }
    phys_->SetBodyKinematic(limb.body, true);
    // The avatar's limbs live inside the player's capsule proxy and must not
    // push it (avatar.cpp Spawn); an NPC's stay on the normal dynamic layer.
    if (AvatarLayer()) phys_->SetBodyAvatarLayer(limb.body, true);
    limb.xf.pos = o;
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
    MobLimb& limb = limbs_[i];
    limb.anchorRoot = anchor;
    limb.anchorLimb = anchor - limb.restOffset;
    Vec3 anchorWorld = origin_ + anchor;
    limb.joint = phys_->CreateJoint(limbs_[pi].body, limb.body,
                                    JointDescFor(ld, anchorWorld));
  }
  // limbs of one creature never collide with each other (they'd fight the
  // joints and jitter forever); different creatures and debris still collide
  {
    std::vector<uint64_t> handles;
    for (const MobLimb& l : limbs_) handles.push_back(l.body);
    phys_->DisableCollisionsAmong(handles);
  }

  // Root's "anchor" is its own centre (the yaw pivot) — again taken from the
  // rig, which already computed exactly that at load.
  {
    MobLimb& root = limbs_[def.rootLimb];
    root.anchorRoot = def.skel.parts[def.rootLimb].anchorLocal;
    root.anchorLimb = root.anchorRoot - root.restOffset;
  }

  // Flipbook frames: pre-convert every .vox model any flipbook references, so
  // a frame change is a pointer swap in AppendInstances rather than a parse.
  // Only parts that actually appear in a flipbook pay for this.
  for (const Flipbook& fb : def.skel.flipbooks)
    for (const FlipbookFrame& ff : fb.frames) {
      if (ff.part < 0 || ff.part >= (int)limbs_.size()) continue;
      if (ff.model < 0 || ff.model >= (int)def.prefab.models.size()) continue;
      MobLimb& limb = limbs_[ff.part];
      if ((int)limb.frameVoxels.size() <= ff.model)
        limb.frameVoxels.resize(ff.model + 1);
      if (!limb.frameVoxels[ff.model].empty()) continue;
      const PrefabModel& fm = def.prefab.models[ff.model];
      std::vector<DebrisVoxel>& dst = limb.frameVoxels[ff.model];
      dst.reserve(fm.voxels.size());
      for (const PrefabVoxel& v : fm.voxels) {
        uint32_t variant = ((uint32_t)(v.x * 7 + v.y * 13 + v.z * 29)) % 3u;
        dst.push_back({(int8_t)v.x, (int8_t)v.y, (int8_t)v.z, v.color,
                       (uint16_t)(v.material | (variant << 12))});
      }
    }

  // animation runtime state (float presentation, never hashed)
  anim_.partAlive.assign(def.limbs.size(), 1);
  anim_.springs.assign(def.limbs.size(), SpringState{});
  anim_.feet.assign(def.skel.chains.size(), FootState{});
  for (size_t c = 0; c < def.skel.chains.size(); c++) {
    const IkChain& ch = def.skel.chains[c];
    // leg length drives every gait threshold, so it is measured from the rig
    // rather than tuned per mob: sum of the chain's rest bone lengths.
    float len = 0;
    for (size_t k = 1; k < ch.parts.size(); k++)
      len += def.skel.parts[ch.parts[k]].rest.pos.len();
    anim_.feet[c].legLength = std::max(len, 1.0f);
    // feet[].valid stays FALSE here: the NPC gait's first-frame path plants a
    // not-yet-valid foot immediately, and pre-marking it valid skips that
    // plant and leaves the foot state garbage. The avatar driver (whose gait
    // reads valid differently) sets its own flags right after BuildRig.
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
    float lowest = 0, hipOfLowest = 0, aheadOfLowest = 0;
    bool any = false;
    for (const IkChain& ch : def.skel.chains) {
      if (ch.tag != "leg" || ch.parts.empty()) continue;
      // Horizontal hip -> ankle offset in the rest pose, accumulated the same
      // way the heights are.
      float ax = 0, az = 0;
      for (size_t k = 1; k < ch.parts.size(); k++) {
        ax += def.skel.parts[ch.parts[k]].rest.pos.x;
        az += def.skel.parts[ch.parts[k]].rest.pos.z;
      }
      const float ahead = std::sqrt(ax * ax + az * az);
      // rest.pos is parent-relative (anchorLocal deltas), so accumulating from
      // the chain root's absolute anchor reproduces AnimFlatten's rest result
      // without running the whole pipeline.
      const float hip = def.skel.parts[ch.parts[0]].anchorLocal.y;
      float y = hip;
      for (size_t k = 1; k < ch.parts.size(); k++)
        y += def.skel.parts[ch.parts[k]].rest.pos.y;
      // The HIP of whichever leg reaches lowest, not the highest hip on the
      // rig: the span these two describe has to belong to ONE chain, or a rig
      // with legs at different heights reports a span no leg actually has.
      if (!any || y < lowest) {
        lowest = y;
        hipOfLowest = hip;
        aheadOfLowest = ahead;
        any = true;
      }
    }
    restSoleY_ = any ? lowest : 0.0f;
    restHipY_ = any ? hipOfLowest : 0.0f;
    restFootAhead_ = any ? aheadOfLowest : 0.0f;
  }
  anim_.lastPos = origin_;
  bodyY_ = origin_.y;
  return true;
}

bool Mob::GroundHeightAt(World& world, int wx, int wz, int yFrom,
                         int& outY, uint32_t* outMat) const {
  // scan down through the chunk cache; request fetches for missing chunks
  // (bounded: one column per creature per tick)
  // 2.4 m of downward scan. Authored in metres: a fixed 24 cells is 2.4 m at
  // 10 cm and 1.2 m at 5 cm, which silently shortens how far a mob can find
  // the ground below it and turns walkable terrain into an invisible drop.
  const int kScanDepth = MetresToCellsI(2.4f);
  for (int y = yFrom; y > yFrom - kScanDepth; y--) {
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
    // solids/powders carry weight; liquids/gases don't (creatures wade, not
    // walk on blood pools)
    if (mat != 0 && mat < ClassOf().size() &&
        (ClassOf()[mat] == CLASS_SOLID || ClassOf()[mat] == CLASS_POWDER)) {
      outY = y + 1;
      if (outMat != nullptr) *outMat = mat;
      return true;
    }
  }
  return false;
}

void Mob::PlayClip(const std::string& name) {
  PlayClipIndex(skel_.FindClip(name));
}

void Mob::PlayClipIndex(int ci) {
  if (ci < 0 || ci >= (int)skel_.clips.size()) return;
  for (ClipInstance& inst : anim_.clips)
    if (inst.clip == ci && !inst.stopping) {  // retrigger: restart, don't stack
      // ONLY a one-shot rewinds. A looping clip that is already running needs
      // no retrigger, and rewinding one is actively wrong: loco clips are
      // re-requested every tick to keep them alive, so resetting timeMs pins
      // the clip at t=0 forever and the pose freezes on the first keyframe.
      if (!skel_.clips[ci].loop) {
        inst.timeMs = 0;
        inst.ageMs = 0;   // a re-triggered one-shot replays its blend-in too
      }
      return;
    }
  ClipInstance inst;
  inst.clip = ci;
  inst.weight = 1.0f;
  anim_.clips.push_back(inst);
}

// ---- locomotion stage 0: sense ---------------------------------------------
// One terrain probe per tick, shared by intent and drive. The fan is in the
// mob's own frame (probe 0 is dead ahead) so the behaviour layer can reason in
// "how far off my nose" without knowing the world yaw.
MobSystem::GroundSense MobSystem::SenseGround(const Mob& mob, const MobDef& def,
                                             World& world) const {
  GroundSense s;
  const float cx = mob.origin_.x + def.worldSize.x * 0.5f;
  const float cz = mob.origin_.z + def.worldSize.z * 0.5f;
  const int yFrom = ifloor(mob.origin_.y) + kMobProbeLiftCells;
  s.haveGround = mob.GroundHeightAt(world, ifloor(cx), ifloor(cz), yFrom, s.groundY);

  // Probe at the mob's own footprint plus a margin, so a wide creature notices
  // a wall before its shoulder is already inside it. The reach is taken
  // PER AXIS (an ellipse, not a circle) to match the footprint of a non-square
  // mob — an isotropic max() makes a long creature probe well past where it
  // can actually walk, and it stops short of gaps it would fit through.
  // 20 cm of margin beyond the body box, in metres so the fan reaches the
  // same real distance past the mob at any voxel size.
  const float senseMargin = MetresToCells(0.2f);
  const float reachX = def.worldSize.x * 0.5f + senseMargin;
  const float reachZ = def.worldSize.z * 0.5f + senseMargin;
  for (int i = 0; i < GroundSense::kProbeCount; i++) {
    const float yaw =
        mob.heading_ + (6.2831853f * (float)i) / (float)GroundSense::kProbeCount;
    const float px = cx + std::sin(yaw) * reachX;
    const float pz = cz + std::cos(yaw) * reachZ;
    int py = 0;
    if (!mob.GroundHeightAt(world, ifloor(px), ifloor(pz), yFrom, py)) {
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
    // Beyond 20 cm of rise is past step-up reach; a big DROP is survivable but
    // not desirable, so it is walkable and the intent layer merely prefers
    // flatter ground.
    //
    // METRES, like the player's own kMaxStepUpVoxels (player.h:190) which has
    // derived its budget from kStepUpM since v0.2. This one was a bare 2, so a
    // mob's step-up silently halved in real terms at 5 cm while the player's
    // held: the same kerb the player walks over becomes a wall to a mob.
    s.clear[i] = rise <= kMobStepUpCells;
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
  mob.driveScale_ = 1.0f;
  if (!sense.haveGround) return;

  // The forward probe is index 0 by construction of the fan.
  const bool blockedAhead = !sense.clear[0];
  if (!blockedAhead) {
    mob.blockedTicks_ = 0;
    return;
  }
  mob.blockedTicks_++;

  // Re-picking a target every tick while a slow turn is still executing makes
  // the mob dither in place. Commit to a chosen deflection for at least as
  // long as a quarter turn takes, unless we are still blocked well after that.
  const LocomotionDef& lo = mob.skel_.loco;
  const uint32_t kCommitTicks =
      (uint32_t)std::max(4.0f, (1.5707963f / std::max(lo.turnRate, 0.1f)) / dt);
  if (tick < mob.lastTurnTick_ + kCommitTicks) return;

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
    const bool wedged = mob.blockedTicks_ > kCommitTicks * 3;
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
    mob.desiredHeading_ = mob.heading_ + 3.14159265f;
    mob.lastTurnTick_ = tick;
    return;
  }
  float off = (6.2831853f * (float)best) / (float)GroundSense::kProbeCount;
  if (off > 3.14159265f) off -= 6.2831853f;
  // Aim BETWEEN the blocked nose and the clear probe rather than exactly at
  // the probe centre: the probes are a coarse 8-way fan and committing to a
  // multiple of 45 degrees would reintroduce the very quantization this change
  // exists to remove. The mob re-senses as it turns, so the heading it settles
  // on is continuous.
  mob.desiredHeading_ = mob.heading_ + off * 0.6f;
  mob.lastTurnTick_ = tick;
}

// ---- locomotion stage 2: steer ---------------------------------------------
// Close heading -> desiredHeading at a bounded, ramped rate. Returns the
// resulting forward-drive alignment factor in 0..1.
float MobSystem::Steer(Mob& mob, const MobDef& def, float dt) {
  const LocomotionDef& lo = mob.skel_.loco;
  // Shortest signed error, wrapped into [-pi, pi]. Doing this with remainder
  // rather than a while-loop matters: a desiredHeading that has accumulated
  // many turns (it is never normalized) would otherwise spin here.
  float err = std::remainder(mob.desiredHeading_ - mob.heading_, 6.2831853f);

  // Turn tighter when slow, wider when fast — a physical body's trade.
  const float speedFrac =
      std::clamp(mob.speedNow_ / std::max(def.speed, 0.01f), 0.0f, 1.0f);
  const float rateCap =
      lo.turnRate * (1.0f + (lo.turnRateMoving - 1.0f) * speedFrac);

  if (rateCap <= 0.0f) {
    mob.turnVel_ = 0;
  } else if (lo.turnAccel <= 0.0f) {
    // No ramp: step straight to the capped rate.
    mob.turnVel_ = std::clamp(err / std::max(dt, 1e-4f), -rateCap, rateCap);
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
    mob.turnVel_ += std::clamp(want - mob.turnVel_, -dv, dv);
  }

  // Never step past the target within one tick: at low dt the ramp handles it,
  // but a large dt (a hitch) would otherwise overshoot and oscillate.
  float step = mob.turnVel_ * dt;
  if (std::abs(step) > std::abs(err)) {
    step = err;
    mob.turnVel_ = err / std::max(dt, 1e-4f);
  }
  mob.heading_ += step;
  // Keep the ACTUATED heading normalized. Intent is left un-normalized on
  // purpose (it is a target, and remainder() above handles the wrap), but
  // `heading` feeds sin/cos every tick for the whole life of the mob and would
  // slowly lose float precision if it drifted to large magnitudes.
  mob.heading_ = std::remainder(mob.heading_, 6.2831853f);

  // Alignment: full drive within driveAlignFull, zero past driveAlignZero.
  const float e = std::abs(std::remainder(mob.desiredHeading_ - mob.heading_,
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
  // 3 cm per tick of ground snap. A rate in cells would double in real terms
  // every time the voxel halved, turning a smooth settle into a pop.
  const float snap = MetresToCells(0.03f);
  mob.origin_.y += std::clamp((float)sense.groundY - mob.origin_.y, -snap, snap);

  // A maimed mob keeps moving, just slower: the active dismemberment state
  // scales the drive speed (a crawl covers ground at a fraction of a walk; a
  // fully disarmed prone state is 0 and goes nowhere). Reads LAST tick's state
  // — UpdateAnimation re-evaluates it — which is at most one tick of lag.
  const float speedScale =
      mob.anim_.locoState >= 0 &&
              mob.anim_.locoState < (int)mob.skel_.states.size()
          ? mob.skel_.states[mob.anim_.locoState].speedScale
          : 1.0f;

  // Translate along the ACTUAL facing, scaled by how well it is aligned with
  // intent. A mob mid-turn therefore traces an arc, and one that has to turn
  // around pivots roughly in place — both fall out of this one multiply rather
  // than needing a turn-in-place special case.
  const float drive = def.speed * speedScale * mob.driveScale_ * align;
  if (drive <= 0.0f) return;
  // Do not walk into a wall we can already feel. The mob keeps turning (Steer
  // ran before this and is unaffected), so it grinds along the obstacle and
  // turns off it rather than freezing against it.
  if (!sense.clear[0]) return;

  const Vec3 fwd{std::sin(mob.heading_), 0, std::cos(mob.heading_)};
  mob.origin_ += fwd * (drive * dt);
  mob.phase_ += drive * dt * 2.2f;  // stride frequency
}

// Procedural gait layer. Writes foot targets into mob.anim_.feet and derives
// the body height/tilt from the resulting foot plane; the IK pass in
// UpdateAnimation then places the legs.
void MobSystem::UpdateGait(Mob& mob, const MobDef& def, World& world, float dt) {
  const AnimSkeleton& sk = mob.skel_;
  const GaitDef& g = sk.gait;
  if (sk.chains.empty()) return;

  Vec3 fwd{std::sin(mob.heading_), 0, std::cos(mob.heading_)};
  // Scale stride and lift by SPEED so a standing mob's feet are perfectly
  // still — a fixed stride makes idle mobs march in place, which reads as a
  // bug even though every individual formula is right.
  float speedFactor = std::clamp(mob.speedNow_ / std::max(def.speed, 0.01f),
                                 0.0f, 1.5f);

  // Which gait group (if any) currently has a swinging foot. Exactly ONE
  // group may swing at a time: that single constraint IS the gait state
  // machine. It generalizes to any leg count (two singleton groups = a biped
  // alternating, diagonal pairs = a quadruped trot) with no per-gait table,
  // and it degrades gracefully when a leg is severed — the surviving legs
  // simply take their turns sooner.
  int swingingGroup = -1;
  for (size_t c = 0; c < mob.anim_.feet.size(); c++) {
    if (!mob.anim_.feet[c].swinging) continue;
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

  for (size_t c = 0; c < sk.chains.size() && c < mob.anim_.feet.size(); c++) {
    const IkChain& ch = sk.chains[c];
    FootState& f = mob.anim_.feet[c];
    // limb loss: stop scheduling this leg's steps entirely. The body-from-feet
    // average below then re-centers on the survivors for free.
    bool alive = true;
    for (int p : ch.parts)
      if (p >= 0 && p < (int)mob.anim_.partAlive.size() && !mob.anim_.partAlive[p])
        alive = false;
    if (!alive) {
      f.valid = false;
      f.swinging = false;
      continue;
    }

    // hip position in world space (rest rig + yaw), the anchor the step is
    // measured from
    Vec3 hipLocal = sk.parts[ch.parts[0]].anchorLocal;
    Vec3 hipWorld = mob.origin_ + Rotate(AxisAngle({0, 1, 0}, mob.heading_),
                                        hipLocal - Vec3{def.worldSize.x * 0.5f,
                                                        0,
                                                        def.worldSize.z * 0.5f}) +
                    Vec3{def.worldSize.x * 0.5f, 0, def.worldSize.z * 0.5f};

    // ideal contact point: under the hip, biased forward, plus velocity
    // lookahead so the foot lands where the body WILL be
    Vec3 ideal = hipWorld + fwd * (g.strideBias * f.legLength * speedFactor) +
                 mob.anim_.velocity * g.leadTime;
    int groundY = 0;
    if (mob.GroundHeightAt(world, ifloor(ideal.x), ifloor(ideal.z),
                       ifloor(mob.origin_.y) + kMobProbeLiftCells, groundY))
      ideal.y = (float)groundY;
    else
      ideal.y = mob.origin_.y;

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
    float stance = (g.rideHeight - 1.0f) * mob.anim_.feet[0].legLength;
    float targetY = sumY / (float)nFeet - mob.restSoleY_ + stance;
    if (!mob.footInit_) {
      mob.bodyY_ = targetY;
      mob.footInit_ = true;
    } else {
      mob.bodyY_ += std::clamp(targetY - mob.bodyY_, -0.4f, 0.4f);
    }
  } else {
    mob.bodyY_ += std::clamp(mob.origin_.y - mob.bodyY_, -0.4f, 0.4f);
    mob.footInit_ = true;
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
  mob.bodyUp_ = (mob.bodyUp_ * 0.85f + targetUp * 0.15f).normalized();
  if (mob.bodyUp_.len() < 0.5f) mob.bodyUp_ = {0, 1, 0};
}

void MobSystem::UpdateAnimation(Mob& mob, const MobDef& def, World& world,
                                float dt) {
  const AnimSkeleton& sk = mob.skel_;
  AnimState& st = mob.anim_;
  if (sk.parts.empty()) return;
  st.partAlive.resize(sk.parts.size(), 1);
  st.springs.resize(sk.parts.size(), SpringState{});

  // measured velocity drives everything speed-scaled downstream
  Vec3 delta = mob.origin_ - st.lastPos;
  st.lastPos = mob.origin_;
  Vec3 planar{delta.x / std::max(dt, 1e-4f), 0, delta.z / std::max(dt, 1e-4f)};
  st.velocity = st.velocity * 0.7f + planar * 0.3f;   // smooth out tick noise
  mob.speedNow_ = Vec3{st.velocity.x, 0, st.velocity.z}.len();
  float speedFactor = std::clamp(mob.speedNow_ / std::max(def.speed, 0.01f),
                                 0.0f, 1.5f);

  const GaitDef& g = sk.gait;
  // THE NPC GAIT STILL RUNS A FREE OSCILLATOR, and that is a deliberate scope
  // line rather than an oversight. PlayerAvatar drives its phase from the feet
  // (PlayerAvatar::SyncStrideClock) because on the avatar the two clocks
  // disagree by ~2.6x and the bob reads as a jitter — but the avatar's swing
  // duration is budget-capped against the player's much higher speed, which is
  // what makes the mismatch that large. The NPC path swings for a flat
  // `stepDuration` at mob speeds, where cadence is close enough that nobody has
  // reported it. Anyone unifying these should port the sync, not the
  // oscillator: `mob.phase_` below is also the legacy swingAmp drive for rigs
  // with no chains, and THAT genuinely has no foot to lock to.
  st.gaitPhase += dt * (g.present ? g.cadence : 2.2f) * speedFactor;
  if (st.gaitPhase > 1.0f) st.gaitPhase -= std::floor(st.gaitPhase);
  mob.phase_ = st.gaitPhase * 6.2831853f;

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
        if (!rule.clip.empty()) mob.PlayClip(rule.clip);
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
    float swing = p.swingAmp * std::sin(mob.phase_ - lag + p.swingPhase * 3.14159265f);
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
    float targetY = mob.origin_.y + (loco ? loco->bodyYOffset : 0.0f);
    mob.bodyY_ += std::clamp(targetY - mob.bodyY_, -0.4f, 0.4f);
    mob.footInit_ = true;
    mob.bodyUp_ = (mob.bodyUp_ * 0.85f + Vec3{0, 1, 0} * 0.15f).normalized();
    if (mob.bodyUp_.len() < 0.5f) mob.bodyUp_ = {0, 1, 0};
  }
  if (!sk.chains.empty() && gaitActive) {
    Quat yaw = AxisAngle({0, 1, 0}, mob.heading_);
    Vec3 pivot{def.worldSize.x * 0.5f, 0, def.worldSize.z * 0.5f};
    Vec3 bodyOrigin{mob.origin_.x, mob.bodyY_, mob.origin_.z};
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

  // ---- stage 6: the pose has to be anatomically possible ----
  // After every solve, never between them: the IK is what puts a joint out of
  // range, so clamping earlier would only clamp a pose about to be replaced.
  AnimClampPoseLimits(sk, st);

  // ---- flipbooks: integer frame index from elapsed ms ----
  if (st.flipbook.book >= 0 && st.flipbook.book < (int)sk.flipbooks.size()) {
    const Flipbook& fb = sk.flipbooks[st.flipbook.book];
    st.flipbook.elapsedMs += (int32_t)std::lround(dt * 1000.0);
    int fr = AnimFlipbookFrame(fb, st.flipbook.elapsedMs);
    if (fr != st.flipbook.frame) {
      st.flipbook.frame = fr;
      // re-point the affected limb at the frame's model; instances rebuild
      for (const FlipbookFrame& ff : fb.frames) {
        if (ff.part < 0 || ff.part >= (int)mob.limbs_.size()) continue;
        mob.limbs_[ff.part].flipbookModel = -1;
      }
      if (fr >= 0 && fr < (int)fb.frames.size()) {
        const FlipbookFrame& ff = fb.frames[fr];
        if (ff.part >= 0 && ff.part < (int)mob.limbs_.size()) {
          mob.limbs_[ff.part].flipbookModel = ff.model;
          instancesDirty_ = true;   // bounded: only on an actual frame change
        }
      }
    }
    if (!fb.loop && st.flipbook.frame == (int)fb.frames.size() - 1)
      st.flipbook.book = -1;
  }
}

void MobSystem::PreTick(uint32_t tick, World& world, std::vector<BrushOp>& ops,
                        std::vector<CellOp>& cellOps,
                        std::vector<ParticleSpawn>& spawns) {
  const float dt = 1.0f / 30.0f;
  // Per-voxel burning and dissolution, once per TICK — never per frame. The
  // pass writes fire into the hashed grid, so running it off the render clock
  // would make the world a function of frame rate.
  BurnLimbs(tick, world, cellOps, spawns);
  IVec3 wo = world.WindowOrigin();
  Vec3 wlo{(float)(wo.x * (int)kChunk), (float)(wo.y * (int)kChunk),
           (float)(wo.z * (int)kChunk)};
  int bleedOps = 0;

  // Rebuilt from scratch each tick: a wound that stopped bleeding, or a mob
  // that despawned, simply stops appearing, and the audio layer reaps the
  // loop. Nothing has to remember to remove an entry.
  bleeds_.clear();

  for (size_t mi = 0; mi < mobs_.size();) {
    Mob& mob = mobs_[mi];
    const MobDef& def = defs_[mob.defIndex_];

    // Drain particles authored since the last tick (dismemberment gore), and
    // tick pending sever holds down first, wherever the mob is in its life —
    // a piece left kinematic and unowned would never sleep (rule #2).
    mob.DrainPendingSpawns(world, spawns);
    mob.TickSeveredHolds(dt);

    // corpses hand their bodies to DebrisSystem in Die(); drop the husk once
    // no limb is still holding a pose
    if (!mob.alive_) {
      bool holding = false;
      for (const MobLimb& l : mob.limbs_) holding |= l.holdBody != 0;
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
    bool out = mob.origin_.x < wlo.x - kPad || mob.origin_.y < wlo.y - kPad ||
               mob.origin_.z < wlo.z - kPad ||
               mob.origin_.x > wlo.x + (float)kWorldN + kPad ||
               mob.origin_.y > wlo.y + (float)kWorldN + kPad ||
               mob.origin_.z > wlo.z + (float)kWorldN + kPad;
    if (out) {
      mob.ReleaseRig();
      mobs_[mi] = std::move(mobs_.back());
      mobs_.pop_back();
      instancesDirty_ = true;
      continue;
    }

    // terrain collision anchors for every live limb (ManageTerrain sweep)
    mob.RegisterTerrainAnchor();

    if (mob.alive_) {
      // ---- locomotion: sense -> intent -> steer -> drive ----
      // Four stages with one direction of data flow. Only DecideIntent has an
      // opinion about where to go; only Steer may move `heading`, and it does
      // so at a bounded rate; only DriveLocomotion translates, and it does so
      // along the ACTUAL facing. That last point is what turns a heading
      // change into a curved path instead of an instant change of direction.
      // THIS is the NPC driver — the block the avatar replaces with player
      // input; everything else in this loop is shared Mob mechanics.
      GroundSense sense = SenseGround(mob, def, world);
      DecideIntent(mob, def, sense, tick, dt);
      float align = Steer(mob, def, dt);
      DriveLocomotion(mob, def, sense, align, dt);

      // ---- stages 1-5: pose the rig (float presentation state) ----
      UpdateAnimation(mob, def, world, dt);

      // ---- stages 6-7: model space -> world, submit to Jolt ----
      // writeXf=false: the NPC path keeps limb.xf as PostStep left it, so its
      // bleed positions are unchanged by the refactor (see Mob::SubmitPose).
      mob.SubmitPose(dt, /*writeXf=*/false);
    }

    // ---- bleeding (PLAN §B5): decaying wound budget, bounded ops ----
    mob.BleedTick(tick, world, ops, spawns, bleedOps);
    mi++;
  }
}

void Mob::DrainPendingSpawns(World& world, std::vector<ParticleSpawn>& spawns) {
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
}

void Mob::TickSeveredHolds(float dt) {
  for (MobLimb& limb : limbs_) {
    if (!limb.holdBody) continue;
    limb.holdSeconds -= dt;
    if (limb.holdSeconds > 0) continue;
    limb.holdSeconds = 0;
    phys_->SetBodyKinematic(limb.holdBody, false);
    // A severed part must collide with the body it came off again: the
    // rig's GroupFilterTable suppressed those contacts forever otherwise.
    phys_->ClearCollisionGroup(limb.holdBody);
    // It also stops being part of this creature — the avatar strips its
    // player-layer exemption here (no-op for an NPC).
    OnBodyReleasedToWorld(limb.holdBody);
    limb.holdBody = 0;
  }
}

void Mob::RegisterTerrainAnchor() {
  // Not only about terrain collision: the anchor is what keeps the chunks
  // around the body FETCHED AND REFRESHED in the CPU mirror
  // (DebrisSystem::ManageTerrain), and the burn pass reads that mirror to
  // find out whether it is standing in a fire. Without it the avatar did not
  // burn while mobs did — the ground probe fetches the chunk under the feet
  // once and World::Cached returns that first copy forever.
  if (!debris_ || !def_) return;
  const Vec3 ws = def_->worldSize;
  const float r =
      0.5f * std::sqrt(ws.x * ws.x + ws.y * ws.y + ws.z * ws.z);
  debris_->AddTerrainAnchor(
      origin_ + Vec3{ws.x * 0.5f, ws.y * 0.5f, ws.z * 0.5f}, r);
}

void Mob::SubmitPose(float dt, bool writeXf) {
  const MobDef& def = *def_;
  // Yaw about the creature's centre column, then the animated model pose on
  // top. Body Y comes from the foot plane (the gait), not the raw ground
  // probe, which is what makes slopes work with no slope code.
  Quat yaw = AxisAngle({0, 1, 0}, heading_);
  // Tilt the whole body to the foot-plane normal, again free from gait.
  Quat tilt = QuatFromTo({0, 1, 0}, bodyUp_);
  Quat bodyRot = Mul(tilt, yaw);
  // The yaw pivot is the footprint centre in PREFAB coordinates — a property
  // of the def, not of where the creature currently stands.
  Vec3 yawPivot{def.worldSize.x * 0.5f, 0, def.worldSize.z * 0.5f};
  Vec3 bodyOrigin{origin_.x, bodyY_, origin_.z};
  for (size_t i = 0; i < limbs_.size(); i++) {
    MobLimb& limb = limbs_[i];
    if (!limb.body) continue;
    // a severed part in its hold window keeps its last pose and is not
    // re-driven; the countdown lives in TickSeveredHolds
    if (limb.holdSeconds > 0) continue;
    Quat local = i < anim_.model.size() ? anim_.model[i].rot : Quat{};
    Vec3 modelPos = i < anim_.model.size() ? anim_.model[i].pos : Vec3{};
    Quat rot = QuatNormalize(Mul(bodyRot, local));
    // modelPos is ALREADY in prefab coordinates: AnimFlatten seeds the root
    // from its rest.pos, which IS rootAnchor, so every part's model pos
    // carries the root offset. Adding rootAnchor again lifts the whole rig
    // by the root anchor — on a biped that is the hip height.
    Vec3 anchorW = bodyOrigin + yawPivot +
                   Rotate(bodyRot, modelPos - yawPivot);
    Vec3 pos = anchorW - Rotate(rot, limb.anchorLimb);
    float q[4] = {rot.x, rot.y, rot.z, rot.w};
    phys_->MoveKinematicBody(limb.body, pos, q, dt);
    // `writeXf` stores the submitted pose immediately: the avatar's held-item
    // placement below and its camera read the hand's FRESH pose this tick.
    // The NPC path passes false so limb.xf stays as PostStep left it and its
    // bleed positions are byte-identical to the pre-refactor behaviour.
    if (writeXf) {
      limb.xf.pos = pos;
      limb.xf.quat[0] = rot.x; limb.xf.quat[1] = rot.y;
      limb.xf.quat[2] = rot.z; limb.xf.quat[3] = rot.w;
    }
  }

  // ---- THE HELD ITEM: HILT ONTO THE HAND ---------------------------------
  // Placed directly from the hand's transform rather than through the
  // anchorLimb/restOffset pair every other part uses: an item is a foreign
  // object whose entire relationship to the rig is "this point of me sits at
  // that point of the hand" (see the long note in game/avatar.h history and
  // EquipItem below). Shared here so a mob wielding a sword places it exactly
  // as the player does.
  if (heldSlot_ >= 0 && heldSlot_ < (int)limbs_.size()) {
    MobLimb& item = limbs_[heldSlot_];
    const int handIdx = skel_.parts[heldSlot_].parent;
    if (item.body && item.holdSeconds <= 0 && handIdx >= 0 &&
        handIdx < (int)limbs_.size() && limbs_[handIdx].body) {
      const MobLimb& hand = limbs_[handIdx];
      const Quat handQ{hand.xf.quat[0], hand.xf.quat[1], hand.xf.quat[2],
                       hand.xf.quat[3]};
      // The socket, in world space: a point in the hand's own frame, carried
      // by whatever pose the hand is in this tick.
      const Vec3 socketW =
          hand.xf.pos + Rotate(handQ, skel_.parts[heldSlot_].rest.pos);
      // The item's orientation is the hand's, composed with the authored
      // grip rotation — the blade keeps its angle in the fist through a
      // swing instead of being re-aimed (melee.h's rule).
      const Quat itemQ =
          QuatNormalize(Mul(handQ, skel_.parts[heldSlot_].rest.rot));
      // Put the grip point on the socket. gripBody_ is already in the item's
      // BODY frame, so this needs no corner or recentring correction.
      const Vec3 pos = socketW - Rotate(itemQ, gripBody_);
      float q[4] = {itemQ.x, itemQ.y, itemQ.z, itemQ.w};
      phys_->MoveKinematicBody(item.body, pos, q, dt);
      if (writeXf) {
        item.xf.pos = pos;
        item.xf.quat[0] = q[0]; item.xf.quat[1] = q[1];
        item.xf.quat[2] = q[2]; item.xf.quat[3] = q[3];
      }
    }
  }
}

void Mob::BleedTick(uint32_t tick, World& world, std::vector<BrushOp>& ops,
                    std::vector<ParticleSpawn>& spawns, int& bleedOps) {
  const MobDef& def = *def_;
  if (def.bleedMat == 0) return;
  const auto& gore = CurrentTuning().gore;
  // Rest-pose prefab offset -> world, for limbs with no physics body left.
  // Only the yaw and the animated body height matter for placing a wound;
  // the foot-plane tilt is deliberately omitted, since a dead creature has no
  // maintained foot plane and a stale one would swing the spray sideways.
  const Quat bodyRotNow = AxisAngle({0, 1, 0}, heading_);
  const Vec3 bodyOriginNow{origin_.x, bodyY_, origin_.z};
  auto bodyFrame = [&](Vec3 prefabOffset) {
    return bodyOriginNow + Rotate(bodyRotNow, prefabOffset);
  };
  for (size_t li = 0; li < limbs_.size(); li++) {
    MobLimb& limb = limbs_[li];
    Quat lq{limb.xf.quat[0], limb.xf.quat[1], limb.xf.quat[2],
            limb.xf.quat[3]};

    // ---- the dismemberment gout ----
    // Front-loaded: emission is proportional to the REMAINING countdown, so
    // the first ticks after the cut throw the bulk of it and the tail
    // thins out. Runs on its own schedule (every tick, not the drip's every
    // 4th) because a burst that stutters at 7.5 Hz reads as a pump.
    if (limb.gushTicks > 0) {
      int decay = std::max(1, gore_.severDecayTicks);
      // Triangular weighting: sum over the window of (2*total/decay) *
      // (k/decay) for k = decay..1 is ~= total, so severSpray is the actual
      // droplet count released rather than a rate to be multiplied out.
      float frac = (float)limb.gushTicks / (float)decay;
      int want = (int)std::lround(2.0f * (float)gore_.severSpray * frac /
                                  (float)decay);
      // Bodyless fallback goes through bodyFrame(), not origin_ +
      // anchorRoot: origin_.y is the spawn corner, so the raw offset puts
      // the wound at the creature's feet instead of at the joint, and it
      // also drops the heading rotation. Same reasoning as Sever().
      Vec3 gOrigin = limb.body ? limb.xf.pos + Rotate(lq, limb.gushLocal)
                               : bodyFrame(limb.anchorRoot);
      Vec3 axis = limb.body ? Rotate(lq, limb.gushDir)
                            : Rotate(bodyRotNow, limb.gushDir);
      for (int k = 0; k < want; k++) {
        if (spawns.size() >= kMaxParticleSpawnsPerTick) break;
        uint32_t h = Hash3((uint32_t)id_ * 2654435761u + (uint32_t)li,
                           tick, (uint32_t)k * 0x9E3779B9u);
        // Event-scoped variance re-rolls per droplet; entity-scoped values
        // already landed in gore_ and pass through untouched.
        const uint32_t es = (uint32_t)id_ ^ ((uint32_t)li << 16);
        float cone = EventVar(gore_.severSprayCone, gore.severSprayConeVar,
                              es, tick, (uint32_t)k * 3u + 0u);
        Vec3 dir{axis.x + SignedUnit(h) * cone,
                 axis.y + SignedUnit(Pcg(h ^ 0x51A17u)) * cone,
                 axis.z + SignedUnit(Pcg(h ^ 0xB0011u)) * cone};
        // speed varies +-25% so the jet has depth instead of a hard front
        float sp = EventVar(gore_.severSpraySpeed,
                            gore.severSpraySpeedVar, es, tick,
                            (uint32_t)k * 3u + 1u) *
                   (0.75f + 0.5f * (float)(Pcg(h ^ 0x1234u) & 0xFFFFu) / 65535.0f);
        if (sp < 0.0f) sp = 0.0f;
        int life = EventVarI(gore_.microLifeTicks, gore.microLifeTicksVar,
                             es, tick, (uint32_t)k * 3u + 2u);
        life = life < 1 ? 1 : (life > 255 ? 255 : life);
        if (!world.CellInWindow({ifloor(gOrigin.x), ifloor(gOrigin.y),
                                 ifloor(gOrigin.z)}))
          break;
        spawns.push_back(MakeDroplet(gOrigin, dir * sp, def.bleedMat, true,
                                     life, gore.microScale));
      }
      limb.gushTicks--;
    }

    // Report the wound for audio BEFORE the budget/op-rate early-outs
    // below. Those exist to bound how much MATTER enters the CA per tick;
    // a wound that is out of drip ops this tick is still bleeding, and
    // gating the sound on them would make the loop stutter with the drip
    // rate instead of tracking the wound.
    if (limb.bleedBudget >= 1.0f && sys_) {
      const float cap = std::max(1.0f, gore.bleedBudgetCap);
      Vec3 wpos = limb.body
                      ? limb.xf.pos + Rotate(lq, limb.woundLocal)
                      : bodyFrame(limb.anchorRoot);
      // Key on (creature, limb) so one loop follows one wound across ticks
      // even as limbs are severed and the vector is reshuffled.
      sys_->bleeds_.push_back(MobSystem::BleedSource{
          wpos, (id_ << 8) ^ (uint64_t)(li + 1),
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
    const uint32_t es = (uint32_t)id_ ^ ((uint32_t)li << 16);
    // The drip's spray count. Entity scope makes this creature a heavy
    // bleeder for life; event scope varies it drip to drip.
    int sprayN = (int)std::lround(
        EventVar(gore_.bleedSprayPerDrip, gore.bleedSprayPerDripVar, es,
                 tick, 0u));
    if (sprayN < 0) sprayN = 0;
    for (int k = 0; k < sprayN; k++) {
      if (spawns.size() >= kMaxParticleSpawnsPerTick) break;
      uint32_t h = Hash3((uint32_t)id_ * 40503u + (uint32_t)li,
                         tick ^ 0xB1005u, (uint32_t)k * 2246822519u);
      float cone = EventVar(gore_.bleedSprayCone, gore.bleedSprayConeVar,
                            es, tick, (uint32_t)k * 3u + 1u);
      // biased upward and outward: a wound sprays, it does not just drool
      Vec3 dir{SignedUnit(h) * cone,
               0.6f + 0.4f * std::fabs(SignedUnit(Pcg(h ^ 0x77u))),
               SignedUnit(Pcg(h ^ 0xC0FFEEu)) * cone};
      float sp = EventVar(gore_.bleedSpraySpeed, gore.bleedSpraySpeedVar,
                          es, tick, (uint32_t)k * 3u + 2u);
      if (sp < 0.0f) sp = 0.0f;
      int life = EventVarI(gore_.microLifeTicks, gore.microLifeTicksVar,
                           es, tick, (uint32_t)k * 3u + 3u);
      life = life < 1 ? 1 : (life > 255 ? 255 : life);
      spawns.push_back(MakeDroplet(w, dir * sp, def.bleedMat, true, life,
                                   gore.microScale));
    }
  }
}

void Mob::PostStep() {
  for (MobLimb& limb : limbs_)
    if (limb.body) phys_->GetTransform(limb.body, limb.xf);
}

void MobSystem::PostStep() {
  for (Mob& mob : mobs_) mob.PostStep();
}

bool MobSystem::FindLimb(uint64_t bodyHandle, uint64_t& mobId,
                         int& limbIndex) const {
  for (const Mob& mob : mobs_)
    for (size_t i = 0; i < mob.limbs_.size(); i++)
      if (mob.limbs_[i].body == bodyHandle) {
        mobId = mob.id_;
        limbIndex = (int)i;
        return true;
      }
  return false;
}

bool MobSystem::Damage(uint64_t bodyHandle, float amount, Vec3 hitWorldVoxel,
                       float impactSpeed) {
  for (Mob& mob : mobs_)
    if (mob.Damage(bodyHandle, amount, hitWorldVoxel, impactSpeed)) return true;
  return false;
}

bool Mob::Damage(uint64_t bodyHandle, float amount, Vec3 hitWorldVoxel,
                 float impactSpeed) {
  if (!def_) return false;
  for (size_t i = 0; i < limbs_.size(); i++) {
    MobLimb& limb = limbs_[i];
    if (limb.body != bodyHandle) continue;
    // limbDefs_, not def_->limbs: a borrowed item slot is damageable (and
    // severable — you can cut the sword out of a hand) like any limb.
    const MobLimbDef& ld = limbDefs_[i];
    Quat q{limb.xf.quat[0], limb.xf.quat[1], limb.xf.quat[2], limb.xf.quat[3]};
    // beam crossing the joint anchor severs outright (PLAN §C2)
    if ((int)i != def_->rootLimb && limb.joint) {
      Vec3 anchorW = limb.xf.pos + Rotate(q, limb.anchorLimb);
      if ((hitWorldVoxel - anchorW).len() < 1.75f) {
        Sever((int)i);
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
        AddBleedBudget(limb.bleedBudget, amount * def_->bleedPerDamage);
    if (limb.hp <= 0 || impactSevers) {
      Sever((int)i);
    } else {
      // non-fatal hit: flinch. This is the one wired trigger for now — it
      // exercises the whole clip layer (sample/blend/mask/blend-out).
      PlayClip("attack");
      // ...and the creature says so. Intensity is the fraction of THIS
      // limb's max hp removed, which is what the hurt slot documents: a
      // scratch on a torso and a scratch on a finger are not the same event.
      // A hit that severs deliberately says nothing here — Sever() already
      // reports, and the sever cue falls back to the hurt set when a mob
      // binds no sever take, so voicing both would double it. (The avatar's
      // hurt slot is simply unbound today, so this stays silent for the
      // player with no exception needed.)
      if (sys_)
        sys_->PushVoice(*this, MobSystem::VoiceKind::Hurt, hitWorldVoxel,
                        ld.hp > 0 ? amount / ld.hp : 1.0f);
    }
    return true;
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

void Mob::LimbVoxelsToParticles(const MobLimb& limb, uint32_t physScale,
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

// ============================================================================
// Per-voxel body reactivity — docs/PLAN_body_reactivity.md
//
// A limb is not "on fire": individual voxels of it are. Cloth catches from one
// hot face, flesh needs three, flesh chars through cooked -> burning -> charred
// as a chain of MATERIALS, acid eats a leg off without necessarily killing the
// creature, and a burning mob walking into a bush lights the bush through
// ordinary fire voxels it emits into the grid.
//
// None of that is authored here. Every one of those behaviours is a row in
// assets/materials/reactions.json evaluated by the same table the GPU runs over
// the grid; this file only decides WHICH voxels get asked, and that decision is
// the whole performance story:
//
//   Fire lives on a SURFACE. The burning set of a limb is a 2D front over a 3D
//   volume, so its cost is bounded by area, not by voxel count. A mina torso is
//   31,456 skin voxels and a scan of it is fifteen times the world's entire
//   per-tick body-burn budget; the front of a fully engulfed mob is a few
//   hundred to ~2000 voxels. So the pass is driven by the front and pushes
//   OUTWARD to its neighbours, and a limb that is not reacting costs one
//   bounded AABB walk over cached chunks and exits (CLAUDE.md rule 2, stated
//   for a new population).
// ============================================================================

namespace {

// World cell -> its chunk. Same one-liner debris.cpp keeps for its own burn
// pass; a shared header for `>> 4` would cost more to find than to restate.
inline IVec3 ChunkOfCell(int x, int y, int z) { return {x >> 4, y >> 4, z >> 4}; }

// burnIdx sentinel: no cell (outside the index box).
constexpr uint32_t kNoBurnCell = 0xFFFFFFFFu;
// Top bit of a burnIdx entry: "already queued as a candidate this tick". The
// entry is a lattice index + 1 and lattices are nowhere near 2^31, so the bit
// is free and costs no second dims-sized array to dedupe the candidate list.
constexpr uint32_t kBurnQueued = 0x80000000u;
// Face samples per axis when seeding from a world contact face. At skinScale 8
// one world cell faces 64 skin voxels, and probing only the face CENTRE would
// seed 1 of 64 and make a limb in acid dissolve an order of magnitude too
// slowly. Sampling at the lattice pitch seeds the whole exposed face, which is
// also what makes the dissolution rate come out volume-exact: 64 independent
// rolls on 1/64-volume voxels remove the same matter per tick that one roll on
// one grid voxel would.
constexpr int kBurnFaceSamplesMax = 8;

// ---- how far the worn-occlusion ray looks -----------------------------------
//
// Both reaches are in WORLD VOXELS and both are "one grid cell, plus enough
// slack for the gap between a rounded limb and a garment cut to its box".
// That gap is what makes a point test useless (BurnLimbView::occlude): a
// diagonal on a tube can be two or three lattice cells clear of the cloth
// around it.
//
// They are deliberately SHORT. A long ray would start finding the far side of
// the coat from inside it, which would make a limb protect itself.
// Authored in METRES: these reach from a sample point to a neighbouring cell,
// so as bare cell counts they would have halved in real terms at 5 cm and
// stopped finding the very cells they exist to find.
const float kWornSeedReach = MetresToCells(0.125f);  // sample point -> hot cell
const float kWornNbrReach = MetresToCells(0.10f);    // surface voxel -> its cell
// Hard cap on lattice steps, whatever the reach asks for. Every loop in this
// system is bounded; this one is a cost that scales with skinScale.
constexpr int kWornMarchMax = 12;

const IVec3 kBurnDirs[6] = {{0, 1, 0}, {0, -1, 0}, {1, 0, 0},
                            {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};

}  // namespace

void Mob::DropBurnIndex(BodyBurnState& st) {
  std::vector<uint32_t>().swap(st.idx);
  std::vector<uint32_t>().swap(st.front);
  st.dims = IVec3{0, 0, 0};
  st.quiet = 0;
  // `st.alight` deliberately SURVIVES. Everything above is an index INTO a
  // lattice that just changed shape; the flag is a fact ABOUT the lattice, and
  // it is the only thing that will make BurnOneLimb rebuild this index once the
  // world fire that started the burn has gone out (see BodyBurnState).
}

BurnLimbView Mob::ViewOf(MobLimb& limb) {
  const MobDef& def = *def_;
  const bool fine = limb.HasFineSkin();
  BurnLimbView v;
  v.skin = fine ? &limb.skinVoxels : nullptr;
  v.coll = fine ? nullptr : &limb.voxels;
  v.scale = fine ? SkinScaleOf(limb) : PhysScaleOf(limb);
  v.xf = &limb.xf;
  v.size = limb.size;
  v.physScale = PhysScaleOf(limb);
  v.microModel = &limb.microModel;
  v.carved = &limb.carved;
  v.flipbook = &limb.flipbookModel;
  v.burn = &limb.burn;
  return v;
}

void MobSystem::BuildBurnIndex(BurnLimbView& v) {
  BodyBurnState& st = *v.burn;
  const size_t n = v.Size();
  st.idx.clear();
  st.front.clear();
  st.dims = IVec3{0, 0, 0};
  if (n == 0) return;

  IVec3 mn{1 << 30, 1 << 30, 1 << 30}, mx{-(1 << 30), -(1 << 30), -(1 << 30)};
  for (size_t i = 0; i < n; i++) {
    IVec3 p = v.At(i);
    mn.x = std::min(mn.x, p.x); mn.y = std::min(mn.y, p.y); mn.z = std::min(mn.z, p.z);
    mx.x = std::max(mx.x, p.x); mx.y = std::max(mx.y, p.y); mx.z = std::max(mx.z, p.z);
  }
  const IVec3 dims{mx.x - mn.x + 1, mx.y - mn.y + 1, mx.z - mn.z + 1};
  const uint64_t cells = (uint64_t)dims.x * dims.y * dims.z;
  // A limb whose bounding box is absurd next to its voxel count (a long
  // diagonal sliver) would allocate a lot to index very little. Refusing is
  // right: it makes the limb un-burnable this tick rather than spending the
  // memory, and no authored rig comes near the ceiling.
  if (cells > (1u << 20)) return;

  st.min = mn;
  st.dims = dims;
  st.idx.assign((size_t)cells, 0u);
  for (size_t i = 0; i < n; i++) {
    if (v.Mat(i) == 0) continue;  // tombstone from an unflushed burn
    IVec3 p = v.At(i);
    const size_t c = ((size_t)(p.z - mn.z) * dims.y + (p.y - mn.y)) * dims.x +
                     (p.x - mn.x);
    st.idx[c] = (uint32_t)i + 1u;
  }
  // Seed the front from whatever is ALREADY alight — the index can be rebuilt
  // mid-fire (a carve drops it), and losing the front would put the fire out.
  for (size_t i = 0; i < n; i++) {
    const uint32_t m = v.Mat(i);
    if (m == 0 || m >= matSelfActive_.size() || !matSelfActive_[m]) continue;
    IVec3 p = v.At(i);
    st.front.push_back(
        (uint32_t)(((size_t)(p.z - mn.z) * dims.y + (p.y - mn.y)) * dims.x +
                   (p.x - mn.x)));
  }
  // This sweep is the ONLY authority that may CLEAR `alight`: it is the one
  // place that looks at every voxel of the limb rather than at a candidate set.
  // The flag exists so that the cheap gate in BurnOneLimb keeps rebuilding this
  // index until the sweep says the fire really is out (see BodyBurnState).
  st.alight = !st.front.empty();
}

void Mob::ReleaseLimbMicro(MobLimb& limb) {
  // Only a CARVED limb owns its brick; an intact one points at the def's shared
  // model, which every other instance of that mob is also using. MicroBodyFree
  // ignores shared models, but the `carved` gate makes the intent explicit at
  // the call site rather than relying on that.
  if (limb.carved && limb.microModel >= 0 && MicroSet())
    MicroBodyFree(*MicroSet(), (uint32_t)limb.microModel);
  limb.carved = false;
}

bool Mob::ReskinLimbMicro(MobLimb& limb, uint32_t skinScale,
                                uint32_t physScale) {
  if (limb.microModel < 0 || !MicroSet()) return false;
  int own = MicroBodyOwn(*MicroSet(), (uint32_t)limb.microModel);
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
  if (!MicroBodyEdit(*MicroSet(), (uint32_t)limb.microModel, mv, shift))
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

bool Mob::RebuildLimbBody(int limbIndex) {
  MobLimb& limb = limbs_[limbIndex];
  if (!limb.body || limb.voxels.empty()) return false;
  const MobDef& def = *def_;
  // COLLIDER pitch: limb.voxels are physScale units, so the Jolt body is built
  // at 1/physScale.
  const float pitch = 1.0f / (float)std::max(1u, PhysScaleOf(limb));
  phys_->GetTransform(limb.body, limb.xf);
  uint64_t nh = phys_->CreateDebrisBodyXf(limb.voxels, limb.xf, DensityOf(),
                                          true /*allowKinematic*/, pitch);
  if (nh == 0) return false;  // Jolt refused: keep the old collider, stay carved

  // The handle CHANGES, so every reference to the old one must be re-pointed
  // in the same breath or the limb silently detaches:
  //   - its joint to its parent,
  //   - its children's joints, which anchor to this body,
  //   - the intra-mob collision exclusion set.
  // Rebuilding them from the LIVE poses (not the rest pose) is what keeps a
  // limb carved mid-stride from snapping back to its spawn position.
  for (size_t k = 0; k < limbs_.size(); k++) {
    if ((int)k == limbIndex) continue;
    if (limbDefs_[k].parent != limbDefs_[limbIndex].name) continue;
    MobLimb& child = limbs_[k];
    if (!child.body || !child.joint) continue;
    Quat cq{child.xf.quat[0], child.xf.quat[1], child.xf.quat[2],
            child.xf.quat[3]};
    Vec3 anchorW = child.xf.pos + Rotate(cq, child.anchorLimb);
    phys_->DestroyJoint(child.joint);
    child.joint = phys_->CreateJoint(nh, child.body,
                                     JointDescFor(limbDefs_[k], anchorW));
  }
  bool kinematic = alive_;
  phys_->RemoveBody(limb.body);
  limb.body = nh;
  phys_->SetBodyKinematic(limb.body, kinematic);
  // ...and the AVATAR-LAYER EXEMPTION, which is part of "every reference to the
  // old handle" exactly as much as the joints above are. A new handle starts on
  // the plain MOVING layer, and a still-attached avatar limb on MOVING is back
  // inside the player's own capsule proxy, where the solver sees a contact it
  // can never resolve and PlayerPushOut sums a depenetration vector whose
  // direction swings with the gait (Layers::AVATAR, phys/physics.cpp).
  //
  // That is the whole "acid eats a bite out of me and I start flying off
  // upwards at an angle" bug, and it was reachable from every damage source
  // there is: acid, fire, laser and blast all end in a carve, and every carve
  // rebuilds the collider. Your own body must not push you — including after it
  // has been rebuilt.
  if (AvatarLayer()) phys_->SetBodyAvatarLayer(limb.body, true);

  if (limb.joint) {
    phys_->DestroyJoint(limb.joint);
    limb.joint = 0;
  }
  if (limbIndex != def.rootLimb) {
    for (size_t k = 0; k < limbDefs_.size(); k++) {
      if (limbDefs_[k].name != limbDefs_[limbIndex].parent) continue;
      if (!limbs_[k].body) break;  // parent already severed: no joint to make
      Quat q{limb.xf.quat[0], limb.xf.quat[1], limb.xf.quat[2], limb.xf.quat[3]};
      Vec3 anchorW = limb.xf.pos + Rotate(q, limb.anchorLimb);
      limb.joint = phys_->CreateJoint(
          limbs_[k].body, limb.body,
          JointDescFor(limbDefs_[limbIndex], anchorW));
      break;
    }
  }
  // Re-exclude the whole mob: the new handle is not in the old exclusion set,
  // so without this a carved limb starts colliding with its own siblings and
  // the rig fights itself into a jitter.
  {
    std::vector<uint64_t> handles;
    for (const MobLimb& l : limbs_)
      if (l.body) handles.push_back(l.body);
    phys_->DisableCollisionsAmong(handles);
  }
  return true;
}

void Mob::EmitCarvedFragment(const MobLimb& src, uint32_t physScale,
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
  if (src.microModel >= 0 && MicroSet()) {
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
    int m = MicroBodyPack(*MicroSet(), mv, dims, physScale, "carve", log);
    if (m < 0) {
      LimbVoxelsToParticles(src, physScale, part, world, spawns);
      return;
    }
    // Packed models are SHARED by default; this one belongs to exactly one body
    // and must be freeable with it, or every gobbet leaks pool words.
    if (m < (int)MicroSet()->owned.size()) MicroSet()->owned[m] = 1;
    micro = MicroBodyRef{(uint32_t)m, physScale};
  }

  const float pitch = 1.0f / (float)std::max(1u, physScale);
  uint64_t h = phys_->CreateDebrisBodyXf(part, xf, DensityOf(), false, pitch);
  if (h == 0) {
    if (micro.Valid()) MicroBodyFree(*MicroSet(), micro.model);
    LimbVoxelsToParticles(src, physScale, part, world, spawns);
    return;
  }
  // A gobbet of the AVATAR is born INSIDE the player's capsule proxy — the
  // carve that made it happened on a limb that lives there. On the plain MOVING
  // layer that is an instant deep penetration against the proxy, and
  // PlayerPushOut turns it into a shove; a spray of them is the player skating
  // off sideways every time acid takes a bite. Same rule as the limb it came
  // off (Layers::AVATAR): your own body may not push you, and neither may the
  // pieces of it. Unlike a severed limb this never converts back — a fist-sized
  // lump of you that has stopped colliding with you is invisible, and it is
  // still fully collidable with the world.
  if (AvatarLayer()) phys_->SetBodyAvatarLayer(h, true);
  // Push it off the wound so it visibly leaves the body rather than resting in
  // the cavity it came from.
  Vec3 away = xf.pos - src.xf.pos;
  float len = away.len();
  away = len > 1e-3f ? away * (1.0f / len) : Vec3{0, 1, 0};
  phys_->SetBodyVelocities(h, away * 2.5f + Vec3{0, 1.5f, 0}, Vec3{});
  debris_->AdoptBody(h, std::move(part), xf, micro, 0, {},
                     def_->bleedMat);
}

bool Mob::CarveLimb(int limbIndex, World& world,
                          std::vector<ParticleSpawn>& spawns, bool eject,
                          const LimbCarveFactory& carveAt,
                          const CarveSpall* spall) {
  // Burning leaves material-0 TOMBSTONES in the lattice between its batched
  // flushes, and every reader of the lattice has to see past them: counting
  // them as present would over-report the limb's volume (so a limb burnt to a
  // thread would not collapse) and majority-fill them into the collider (so
  // physics would keep matter the fire has already taken). The flush expresses
  // itself as a carve, hence the guard.
  if (!inBurnFlush_ && limbs_[limbIndex].burn.removed) {
    if (!FlushBurn(limbIndex, world, spawns, /*force=*/true)) return false;
  }
  MobLimb& limb = limbs_[limbIndex];
  if (!limb.body || limb.voxels.empty()) return true;
  const MobDef& def = *def_;
  // NOTE: limb.xf is deliberately NOT refreshed from Jolt here. the predicate
  // was built against the pose the CALLER measured, and a live limb is
  // kinematic — the animation pipeline re-poses it every tick — so re-reading
  // the transform now would test the voxels against a pose the predicate never
  // saw and carve the wrong cells (or, as the pose drifts, none at all).

  const bool fine = limb.HasFineSkin();
  // ONE world-space volume, re-expressed per lattice. The collider predicate
  // always exists; the skin one only when the skin is a separate lattice.
  const auto keep = carveAt((float)std::max(1u, PhysScaleOf(limb)));

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
    const auto keepSkin = carveAt((float)std::max(1u, SkinScaleOf(limb)));
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

  // ---- SPALL: grow the hole into its own rim -------------------------------
  //
  // The crater predicate is a per-voxel question and cannot express "take more
  // where matter is already missing" — it has no idea what is left. This does,
  // because `limb` is the authoritative voxel list, and that is the whole
  // reason the growth lives in CarveLimb rather than in the predicate.
  //
  // Each round removes surviving voxels that are INSIDE the blast and already
  // have enough missing face-neighbours, weighted toward the centre. That turns
  // a rim of isolated survivors into a torn edge, makes a second hit on an
  // existing wound widen it instead of stippling fresh flesh next to it, and is
  // what lets a blast beside an arm take the arm.
  //
  // Runs on whichever lattice is AUTHORITATIVE (the skin when there is one), so
  // the collider re-derive below picks the result up for free.
  if (spall != nullptr && spall->rounds > 0 && spall->strength > 0.0f &&
      spall->radius > 0.0f) {
    const float scale =
        (float)std::max(1u, fine ? SkinScaleOf(limb) : PhysScaleOf(limb));
    const Vec3 cLocal = spall->centerLocal * scale;
    const float rLocal = spall->radius * scale;
    const float r2 = rLocal * rLocal;
    // Packed key over the lattice. int16 skin coords, so 21 bits per axis with
    // a bias is ample and collision-free (unlike hashing the position, which
    // would make occupancy probabilistic — the one thing this pass must not be).
    auto key = [](int x, int y, int z) -> uint64_t {
      return ((uint64_t)(uint32_t)(x + 32768) << 42) |
             ((uint64_t)(uint32_t)(y + 32768) << 21) |
             (uint64_t)(uint32_t)(z + 32768);
    };
    std::unordered_set<uint64_t> live;
    auto rebuild = [&] {
      live.clear();
      if (fine) {
        live.reserve(limb.skinVoxels.size() * 2);
        for (const PrefabVoxel& v : limb.skinVoxels) live.insert(key(v.x, v.y, v.z));
      } else {
        live.reserve(limb.voxels.size() * 2);
        for (const DebrisVoxel& v : limb.voxels) live.insert(key(v.x, v.y, v.z));
      }
    };
    rebuild();
    // A voxel on an intact surface already has one open face, so "eroded"
    // starts at three: fewer and this eats the whole skin from the outside in
    // rather than widening the crater.
    constexpr int kMinOpenFaces = 3;
    for (int round = 0; round < spall->rounds; round++) {
      size_t took = 0;
      auto doomed = [&](int x, int y, int z) {
        const Vec3 d{(float)x + 0.5f - cLocal.x, (float)y + 0.5f - cLocal.y,
                     (float)z + 0.5f - cLocal.z};
        const float d2 = d.dot(d);
        if (d2 >= r2) return false;               // outside the blast
        int open = 0;
        static const int kN[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                     {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (const auto& n : kN)
          if (!live.count(key(x + n[0], y + n[1], z + n[2]))) open++;
        if (open < kMinOpenFaces) return false;
        // Proximity-weighted, so the tearing is concentrated at the blast and
        // fades out rather than eroding the rim uniformly. Keyed on the same
        // (mob, limb) seed plus the round, so a replay spalls identically.
        const float t = std::sqrt(d2 / r2);
        const float chance = spall->strength * (1.0f - t) *
                             ((float)open / 6.0f);
        const uint32_t h =
            Hash3(spall->seed + 0x5BF03635u * (uint32_t)(round + 1),
                  (uint32_t)(x * 73856093) ^ (uint32_t)(y * 19349663),
                  (uint32_t)(z * 83492791));
        return (float)(h & 0xFFFFu) / 65535.0f < chance;
      };
      if (fine) {
        const size_t before = limb.skinVoxels.size();
        limb.skinVoxels.erase(
            std::remove_if(limb.skinVoxels.begin(), limb.skinVoxels.end(),
                           [&](const PrefabVoxel& v) {
                             if (!doomed(v.x, v.y, v.z)) return false;
                             skinLostSum +=
                                 Vec3{(float)v.x, (float)v.y, (float)v.z};
                             skinLostN++;
                             return true;
                           }),
            limb.skinVoxels.end());
        took = before - limb.skinVoxels.size();
        if (took) skinRemoved = true;
      } else {
        const size_t before = limb.voxels.size();
        limb.voxels.erase(
            std::remove_if(limb.voxels.begin(), limb.voxels.end(),
                           [&](const DebrisVoxel& v) {
                             if (!doomed(v.x, v.y, v.z)) return false;
                             removed.push_back(v);
                             return true;
                           }),
            limb.voxels.end());
        took = before - limb.voxels.size();
      }
      // Nothing left to grow into: stop rather than paying for empty passes.
      if (took == 0) break;
      // The NEXT round must see the hole this one opened, or every round tests
      // the same rim and the growth is one round wide however many are asked
      // for. This is the whole mechanism.
      rebuild();
    }
  }

  // THE PARTICLES ARE BILLED FROM THE COLLIDER DELTA, not from the predicate.
  //
  // On a fine skin the spall above ran on the SKIN, so the `removed` list built
  // from the collider predicate no longer describes what actually left the body
  // — it would under-report a torn chunk as a sprinkle of gore. Re-deriving the
  // collider first and differencing gives the honest set, and it is the same
  // set the physics is about to be rebuilt from.
  std::vector<DebrisVoxel> colliderBefore;
  if (fine && eject) colliderBefore = limb.voxels;
  if (fine) {
    // Skin is authoritative: re-derive the collider from what the carve left
    // rather than carving the collider in parallel. Disagreement between the
    // two lattices is then unrepresentable (phys/lattice.h).
    bool overflow = false;
    limb.voxels = DownsampleSkin(
        limb.skinVoxels,
        std::max(1u, SkinScaleOf(limb) / std::max(1u, PhysScaleOf(limb))),
        &overflow);
  } else {
    limb.voxels.erase(
        std::remove_if(limb.voxels.begin(), limb.voxels.end(),
                       [&](const DebrisVoxel& v) {
                         return !keep(v.x, v.y, v.z);
                       }),
        limb.voxels.end());
  }
  if (eject) {
    if (fine) {
      // What the collider lost across the whole carve, spall included. The
      // predicate-built `removed` is deliberately discarded here rather than
      // merged: on a fine skin it was only ever an approximation of this.
      std::unordered_set<uint64_t> after;
      after.reserve(limb.voxels.size() * 2);
      auto ck = [](const DebrisVoxel& v) -> uint64_t {
        return ((uint64_t)(uint32_t)(v.x + 32768) << 42) |
               ((uint64_t)(uint32_t)(v.y + 32768) << 21) |
               (uint64_t)(uint32_t)(v.z + 32768);
      };
      for (const DebrisVoxel& v : limb.voxels) after.insert(ck(v));
      removed.clear();
      for (const DebrisVoxel& v : colliderBefore)
        if (!after.count(ck(v))) removed.push_back(v);
    }
    LimbVoxelsToParticles(limb, PhysScaleOf(limb), removed, world, spawns);
  }
  MarkInstancesDirty();

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
  // THE LOSS IS INCREMENTAL; THE DENOMINATOR IS NOT.
  //
  // `voxelsAtSpawn` never moves, so `at0 - nowCount` is everything this limb
  // has EVER lost. Charging that to hp on every carve makes the damage from N
  // equal carves grow as N(N+1)/2 instead of N — the second bite costs twice
  // what it should, the tenth costs ten times.
  //
  // Nothing noticed while carves were rare (one blade, one blast). BURNING is
  // what exposed it: FlushBurn expresses itself as a carve and fires every
  // max(12, n>>6) voxels removed, so a limb on fire carves itself dozens of
  // times, and at kCarveDamagePerVolume 1.5 it reached hp 0 having lost about
  // 14% of its volume. Every burning limb dismembered, and the torso reaching 0
  // is a vital limb, so Sever() called Die() — a creature that caught fire came
  // apart instead of charring. The fraction is still measured against `at0`
  // (that is what makes a wound read the same on any rig); only the numerator
  // becomes the delta since the last charge.
  const uint32_t prevCount = std::max(1u, limb.voxelsCharged);
  const uint32_t lostCount = prevCount > nowCount ? prevCount - nowCount : 0u;
  limb.voxelsCharged = nowCount;
  const float lost = (float)lostCount / (float)at0;
  limb.hp -= lost * limbDefs_[limbIndex].hp * kCarveDamagePerVolume;
  // ---- WHAT MAY BLEED, AND WHAT MAY NOT ------------------------------------
  //
  // Two exclusions, both of them reported as bugs and both of them the same
  // mistake: this function is reached by causes that are not a cut, and it was
  // written as though every caller were one.
  //
  //   * A GARMENT HAS NO BLOOD IN IT. A shell is a borrowed rig slot, so
  //     `def.bleedMat` — the WEARER's blood — was being sprayed out of a
  //     burning robe. See Mob::IsWornSlot.
  //   * FIRE CAUTERISES. FlushBurn expresses a tick of burning as a carve and
  //     fires every max(12, n>>6) voxels removed, so a limb on fire carves
  //     itself dozens of times a second; each one topped the drip budget back
  //     up, which is why being on fire read as haemorrhaging. Charring is not
  //     a wound that bleeds, and the material chain (skin -> cooked -> burning
  //     -> charred) is already the whole visual account of it.
  //
  // The HP CHARGE above is deliberately outside both: fire still kills you, and
  // a burnt shell still loses its own durability. Only the blood is refused.
  const bool bleeds = !inBurnFlush_ && !IsWornSlot(limbIndex);
  if (def.bleedMat && bleeds) {
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
      c = c * (1.0f / (float)n / (float)std::max(1u, PhysScaleOf(limb)));
    } else if (skinLostN) {
      // Collider too coarse to notice, skin was not: fall back to the skin's
      // own account of where the damage landed rather than leaving the wound
      // at wherever the last one happened to be.
      n = skinLostN;
      c = skinLostSum *
          (1.0f / (float)n / (float)std::max(1u, SkinScaleOf(limb)));
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
    Sever(limbIndex);
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
      Vec3 aMicro = limb.anchorLimb * (float)std::max(1u, PhysScaleOf(limb));
      // Which components are big enough to still be this limb (see the
      // straggler note in the loop below). Both floors are the ones the limb is
      // about to be judged by anyway: kMinFragmentVoxels is "too few voxels to
      // be a body" and kLimbCollapseFraction is "too little left to be a limb",
      // measured here against what was present BEFORE the split.
      const uint32_t keepFloor = std::max(
          kMinFragmentVoxels, (uint32_t)(kLimbCollapseFraction * (float)n));
      std::vector<uint8_t> eligible(compSize.size(), 0);
      bool anyEligible = false;
      for (size_t c = 0; c < compSize.size(); c++)
        if (compSize[c] >= keepFloor) { eligible[c] = 1; anyEligible = true; }
      // Nothing survives the split intact: fall back to plain nearest-wins so
      // the limb still picks up SOMETHING, and let the collapse test below do
      // what it was always going to do to it.
      if (!anyEligible) eligible.assign(compSize.size(), 1);
      uint32_t keepComp = 0;
      float best = 1e30f;
      for (uint32_t i = 0; i < n; i++) {
        const DebrisVoxel& v = limb.voxels[i];
        float d2 = (Vec3{(float)v.x, (float)v.y, (float)v.z} - aMicro).len();
        // A STRAGGLER MAY NOT INHERIT THE LIMB.
        //
        // Nearest-to-the-anchor is the right rule between two real pieces of a
        // limb — the one still joined to the creature keeps its identity, and
        // the big carved-off haunch does not steal the leg's rig slot. But it
        // was applied to every component including single loose voxels, and
        // the tie-break below only ever fires on an EXACT distance tie, which
        // never happens. One voxel merely NEARER than the mass won outright.
        //
        // Burning is a machine for producing that voxel: it eats holes, and
        // holes strand specks. Measured on a wizard's head lit for 23 ticks —
        // 940 collider voxels, 82% of its skin still there, hp 16.5/22 — the
        // split handed the limb to a ONE-voxel component, the other 771 left
        // as fragments, and `voxels.size() < kMinFragmentVoxels` then severed
        // it. The head is vital, so the creature died. That is the "a burning
        // body dismembers instead of charring" bug, and no amount of tuning the
        // burn rates could have reached it.
        //
        // The rule now: only a component that could still BE this limb may
        // inherit it — the same two floors that decide whether the limb
        // survives at all, applied one step earlier. If nothing qualifies the
        // limb genuinely is dust, and the collapse test below severs it as
        // before.
        if (!eligible[comp[i]]) continue;
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
            std::max(1u, SkinScaleOf(limb) / std::max(1u, PhysScaleOf(limb)));
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
          EmitCarvedFragment(limb, PhysScaleOf(limb), std::move(parts[c]),
                             world, spawns);
          budget--;
        } else {
          LimbVoxelsToParticles(limb, PhysScaleOf(limb), parts[c], world, spawns);
        }
      }
      // Losing the disconnected mass can itself take the limb under the floor.
      // Same lattice pairing as the first collapse test above.
      if (limb.voxels.size() < kMinFragmentVoxels ||
          (float)(fine ? limb.skinVoxels.size() : limb.voxels.size()) <
              kLimbCollapseFraction * (float)at0) {
        Sever(limbIndex);
        return false;
      }
    }
  }

  // Re-sync the charged count: a connectivity split above threw mass away
  // AFTER the hp charge was computed, and that mass left as its own body or as
  // particles rather than as a wound. Leaving it uncharged here would bill it
  // to the NEXT carve, which is the same off-by-a-history error in slower form.
  limb.voxelsCharged =
      (uint32_t)(fine ? limb.skinVoxels.size() : limb.voxels.size());

  // Art, then collider. ReskinLimbMicro may shift the limb origin, and the
  // collider must be built from the voxels in their FINAL frame.
  if (limb.microModel >= 0)
    ReskinLimbMicro(limb, SkinScaleOf(limb), PhysScaleOf(limb));
  RebuildLimbBody(limbIndex);
  // The carve compacted the lattice (and may have rebased it), so every entry
  // in the burn index now points at the wrong voxel. Drop it: the next burn
  // tick rebuilds it and re-seeds the front from whatever is still alight, so
  // a fire survives being carved through without the index having to.
  DropBurnIndex(limb.burn);
  // The creature cries out — but only HERE, on the surviving path. Every
  // route out of this function above went through Sever(), which reports its
  // own event, and the sever cue falls back to the hurt set when nothing is
  // bound; voicing both would double a single blow. `lost` is already the
  // fraction of the limb's volume removed, so `lost * kCarveDamagePerVolume`
  // is the fraction of its max hp — exactly what the hurt slot documents.
  //
  // A GARMENT DOES NOT CRY OUT. Same exclusion as the blood above: a hole
  // opening in a hood is the hood's damage, not the wearer's, and voicing it
  // made a burning robe sound like a mauling.
  if (lost > 0.0f && !IsWornSlot(limbIndex))
    if (sys_)
    sys_->PushVoice(*this, MobSystem::VoiceKind::Hurt, limb.xf.pos,
                    lost * kCarveDamagePerVolume);
  return true;
}

void Mob::StripBurnTombstones(MobLimb& limb) {
  if (limb.burn.removed == 0) return;
  limb.skinVoxels.erase(
      std::remove_if(limb.skinVoxels.begin(), limb.skinVoxels.end(),
                     [](const PrefabVoxel& v) { return (v.material & 0xFFFu) == 0; }),
      limb.skinVoxels.end());
  limb.voxels.erase(
      std::remove_if(limb.voxels.begin(), limb.voxels.end(),
                     [](const DebrisVoxel& v) { return (v.payload & 0xFFFu) == 0; }),
      limb.voxels.end());
  limb.burn.removed = 0;
  DropBurnIndex(limb.burn);
  MarkInstancesDirty();
}

bool Mob::FlushBurn(int limbIndex, World& world,
                          std::vector<ParticleSpawn>& spawns, bool force) {
  MobLimb& limb = limbs_[limbIndex];
  if (limb.burn.removed == 0) return true;
  const MobDef& def = *def_;
  const bool fine = limb.HasFineSkin();
  const uint32_t latScale = fine ? SkinScaleOf(limb) : PhysScaleOf(limb);
  const size_t n = fine ? limb.skinVoxels.size() : limb.voxels.size();
  // Scale the threshold with the limb. 12 voxels out of a 62-voxel dummy arm
  // is a real shape change and must rebuild now; 12 out of a 31k mina torso is
  // invisible, and rebuilding Jolt for it every tick — which replaces the body
  // handle and re-makes this limb's joint, its children's joints and the
  // intra-mob exclusion set — is what would make burning mobs the most
  // expensive thing in the game (PLAN §4.6.2).
  const uint32_t threshold =
      std::max(kBurnRebuildFloor, (uint32_t)(n >> kBurnRebuildShift));
  if (!force && limb.burn.removed < threshold) return true;
  limb.burn.removed = 0;

  // Express the accumulated removals as an ORDINARY CARVE rather than
  // re-implementing its tail. Everything a carve already does is what burning
  // through a limb should do: hp falls in proportion to the fraction of the
  // limb that is gone, the collider is re-derived from the authoritative
  // lattice, the brick is re-packed, a limb burnt below kLimbCollapseFraction
  // severs, and a limb burnt THROUGH splits into pieces. "Acid takes the leg
  // off but does not kill" is not a feature here — it is CarveLimb's existing
  // behaviour reached by a different cause.
  //
  // The removed set is read off the burn index, where a burnt voxel's entry was
  // cleared the moment it went: the tombstone in the sparse lattice is what the
  // predicate rejects, and the index is what remembers which those are.
  const IVec3 bmin = limb.burn.min, bdim = limb.burn.dims;
  const std::vector<uint32_t>* idx = &limb.burn.idx;
  auto keepAt = [bmin, bdim, idx](int x, int y, int z) -> bool {
    const int lx = x - bmin.x, ly = y - bmin.y, lz = z - bmin.z;
    if (lx < 0 || ly < 0 || lz < 0 || lx >= bdim.x || ly >= bdim.y ||
        lz >= bdim.z)
      return true;
    return (*idx)[((size_t)lz * bdim.y + ly) * bdim.x + lx] != 0;
  };
  if (idx->empty()) {  // index already gone: fall back to a plain compaction
    StripBurnTombstones(limb);
    return true;
  }

  inBurnFlush_ = true;
  const bool alive = CarveLimb(
      limbIndex, world, spawns, /*eject=*/false,
      [&](float scale) -> LimbCarveKeep {
        // Only the AUTHORITATIVE lattice is carved. The other one is either the
        // same array or derived from this one by majority-fill, and carving it
        // independently is what makes the two drift.
        if ((uint32_t)(scale + 0.5f) == latScale) return keepAt;
        return [](int, int, int) { return true; };
      });
  inBurnFlush_ = false;
  if (!alive) return false;  // severed or the mob died: caller must not touch it

  // The carve compacted the lattice, so every index in the burn index moved.
  // Drop it; the next tick rebuilds it and re-seeds the front from whatever is
  // still alight, so the fire survives the rebuild without the index having to.
  DropBurnIndex(limbs_[limbIndex].burn);
  return true;
}

uint32_t MobSystem::IgniteOneLimb(BurnLimbView& v, uint32_t count,
                                 uint32_t onlyMat) {
  if (!BurnTablesReady() || v.Size() == 0) return 0;
  BodyBurnState& st = *v.burn;
  if (st.idx.empty()) BuildBurnIndex(v);
  if (st.idx.empty()) return 0;

  const IVec3 d = st.dims, mn = st.min;
  auto cellOf = [&](IVec3 p) -> uint32_t {
    const int lx = p.x - mn.x, ly = p.y - mn.y, lz = p.z - mn.z;
    if (lx < 0 || ly < 0 || lz < 0 || lx >= d.x || ly >= d.y || lz >= d.z)
      return kNoBurnCell;
    return (uint32_t)(((size_t)lz * d.y + ly) * d.x + lx);
  };
  uint32_t lit = 0;
  // SURFACE voxels only, and in lattice order so a given (body, count) always
  // lights the same voxels — this is a test and spell entry point, and a
  // nondeterministic one would be useless for both.
  for (size_t i = 0; i < v.Size() && lit < count; i++) {
    const uint32_t m = v.Mat(i);
    if (onlyMat && m != (onlyMat & 0xFFFu)) continue;
    const uint32_t hot = IgnitedForm(m);
    if (hot == 0) continue;  // bone, steel: no path to burning, no exception list
    const IVec3 p = v.At(i);
    bool surface = false;
    for (const IVec3& dir : kBurnDirs) {
      const uint32_t nc = cellOf({p.x + dir.x, p.y + dir.y, p.z + dir.z});
      if (nc == kNoBurnCell || st.idx[nc] == 0) { surface = true; break; }
    }
    if (!surface) continue;
    v.Set(i, hot, 0);
    const uint32_t c = cellOf(p);
    if (c != kNoBurnCell) st.front.push_back(c);
    lit++;
  }
  if (!lit) return 0;
  // Micro bodies must own their brick before a poke can land on it, exactly as
  // a carve must; a shared model backs every instance of the def.
  if (v.microModel && *v.microModel >= 0 && microSet_) {
    const int own = MicroBodyOwn(*microSet_, (uint32_t)*v.microModel);
    if (own >= 0) {
      *v.microModel = own;
      if (v.carved) *v.carved = true;
      if (v.flipbook) *v.flipbook = -1;
      for (uint32_t c : st.front) {
        const uint32_t vi = st.idx[c] & ~kBurnQueued;
        if (!vi) continue;
        const IVec3 p = v.At(vi - 1);
        MicroBodyPoke(*microSet_, (uint32_t)*v.microModel, p.x, p.y, p.z,
                      (uint8_t)v.Mat(vi - 1), 0);
      }
    }
  }
  return lit;
}

uint32_t MobSystem::IgniteLimb(uint64_t mobId, int limbIndex, uint32_t count,
                               uint32_t onlyMat) {
  Mob* mob = FindMobById(mobId);
  return mob ? mob->Ignite(limbIndex, count, onlyMat) : 0;
}

uint32_t Mob::Ignite(int limbIndex, uint32_t count, uint32_t onlyMat) {
  if (!sys_ || limbIndex < 0 || limbIndex >= (int)limbs_.size()) return 0;
  if (!limbs_[limbIndex].body) return 0;
  BurnLimbView v = ViewOf(limbs_[limbIndex]);
  const uint32_t lit = sys_->IgniteOneLimb(v, count, onlyMat);
  if (lit) MarkInstancesDirty();
  return lit;
}

// One tick of per-voxel burning over ONE limb, whoever owns it.
//
// This is the whole pass, and it is deliberately not a member of the thing it
// burns. MobSystem::MobLimb and PlayerAvatar::Part are two runtime spellings of
// the same idea — the avatar IS a MobDef with a different driver — and a
// creature and the player character have to burn identically. The only way to
// be sure of that is for there to be one implementation; two that agree today
// is two that disagree after the next tuning change.
//
// What the CALLER keeps is what genuinely differs: how a part burnt through is
// severed, how hp falls, how a collider is rebuilt. This returns whether
// anything changed, and leaves the removed voxels as material-0 tombstones for
// the caller to flush on its own schedule.
bool MobSystem::BurnOneLimb(BurnLimbView& v, uint32_t tick, uint32_t rngKey,
                            World& world, std::vector<CellOp>& cellOps,
                            uint32_t& frontBudget, uint32_t& opsBudget) {
  bool changed = false;
  if (!BurnTablesReady() || v.Size() == 0 || frontBudget == 0) return false;
  BodyBurnState& st = *v.burn;
  const uint32_t limbKey = rngKey;

  // One-entry chunk memo. A limb spans one or two chunks and the ignition scan
  // asks the same one over and over; without this the walk is a hash lookup per
  // cell instead of per chunk.
  IVec3 memoChunk{INT_MIN, INT_MIN, INT_MIN};
  const CachedChunk* memoCC = nullptr;
  auto worldMatAt = [&](IVec3 c) -> uint32_t {
    if (!world.CellInWindow(c)) return 0u;
    const IVec3 wc = ChunkOfCell(c.x, c.y, c.z);
    if (wc.x != memoChunk.x || wc.y != memoChunk.y || wc.z != memoChunk.z) {
      memoChunk = wc;
      memoCC = world.Cached(wc);
    }
    // Unknown or unfetched reads as AIR — ignition is best-effort and must
    // never fail the wrong way — but ASK for the chunk, because the CPU mirror
    // is fetch-on-demand and nothing else is going to want this particular one.
    //
    // Reading without requesting is why the player did not burn while mobs did:
    // a mob's ground probe happens to fetch the chunk under its feet, which is
    // usually where the fire is, so the burn pass rode on somebody else's
    // request and looked like it worked. The player's probe fetches the same
    // chunk, but a fire reaching its head is a chunk UP, which nothing had ever
    // asked for — so a burning bonfire read as empty air forever. Bounded
    // (kFetchPerTick), coalesced, and one tick latent.
    if (!memoCC || memoCC->voxels.size() != kChunkVol) {
      world.RequestChunkFetch(wc);
      return 0u;
    }
    const uint32_t lx = (uint32_t)(c.x & 15), ly = (uint32_t)(c.y & 15),
                   lz = (uint32_t)(c.z & 15);
    return memoCC->voxels[(lz * kChunk + ly) * kChunk + lx] & 0xFFFu;
  };

  // Grid writes are DEDUPED per world cell. At skinScale 8 five hundred skin
  // voxels share one world cell, and without this a burning torso would spend
  // the whole op budget writing fire into the same few cells. The dedupe is
  // also what makes the emission RATE right: what a burning surface puts into
  // the world is one fire voxel per adjacent air CELL, not one per sub-voxel.
  std::vector<uint32_t> emitted;
  auto emitCell = [&](IVec3 c, uint32_t mat, uint32_t state) {
    if (opsBudget == 0 || cellOps.size() >= kMaxCellOpsPerTick) return;
    if (!world.CellInWindow(c)) return;
    const uint32_t ci = World::SlotCellIndex(c);
    for (uint32_t e : emitted)
      if (e == ci) return;
    emitted.push_back(ci);
    // fill-air-only: grid content wins deterministically on the GPU, exactly as
    // burning debris and settle-back already write.
    cellOps.push_back({ci, PackVoxNew(mat, state) | kCellOpIfAir});
    opsBudget--;
  };

  std::vector<IVec3> scanHot;
  std::vector<uint32_t> cand;

  // The pose is read from `limb.xf` as the animation left it, never
  // re-read from Jolt: a live limb is kinematic and re-posed every tick, so
  // a mid-pass re-read tests voxels against a pose the rest of the pass was
  // not computed against (gotcha-live-limb-carve-pose).
  const Quat q{v.xf->quat[0], v.xf->quat[1], v.xf->quat[2],
               v.xf->quat[3]};
  const float inv = 1.0f / (float)std::max(1u, v.scale);
  auto worldOf = [&](IVec3 vp) {
    return v.xf->pos + Rotate(q, Vec3{((float)vp.x + 0.5f) * inv,
                                      ((float)vp.y + 0.5f) * inv,
                                      ((float)vp.z + 0.5f) * inv});
  };

  // ---- the cheap gate: walk the WORLD side, not the body side -----------
  // A mina limb's world AABB is order 4x4x8 = 128 cells; its surface is
  // order 5000 skin voxels. Asking "what is in the cells around me" is two
  // orders of magnitude cheaper than asking "what is in the cell each of my
  // surface voxels occupies", and it degrades gracefully: a limb touching
  // nothing interesting costs one walk over ~100 cached cells and exits
  // (PLAN §4.2).
  IVec3 lo{}, hi{};
  {
    const float sinv = 1.0f / (float)std::max(1u, v.physScale);
    Vec3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
    for (int k = 0; k < 8; k++) {
      const Vec3 c{(k & 1) ? (float)v.size.x * sinv : 0.0f,
                   (k & 2) ? (float)v.size.y * sinv : 0.0f,
                   (k & 4) ? (float)v.size.z * sinv : 0.0f};
      const Vec3 w = v.xf->pos + Rotate(q, c);
      mn.x = std::min(mn.x, w.x); mn.y = std::min(mn.y, w.y);
      mn.z = std::min(mn.z, w.z);
      mx.x = std::max(mx.x, w.x); mx.y = std::max(mx.y, w.y);
      mx.z = std::max(mx.z, w.z);
    }
    lo = {ifloor(mn.x) - 1, ifloor(mn.y) - 1, ifloor(mn.z) - 1};
    hi = {ifloor(mx.x) + 1, ifloor(mx.y) + 1, ifloor(mx.z) + 1};
  }
  scanHot.clear();
  {
    uint32_t seen = 0;
    for (int y = lo.y; y <= hi.y && seen < kBurnScanCells; y++)
      for (int z = lo.z; z <= hi.z && seen < kBurnScanCells; z++)
        for (int x = lo.x; x <= hi.x && seen < kBurnScanCells; x++) {
          seen++;
          const uint32_t m = worldMatAt({x, y, z});
          if (m == 0 || m >= matHot_.size()) continue;
          // "Reactive" is two things, and both come out of the table: the
          // cell is HOT (it can ignite me through my own rules) or it
          // REWRITES ITS NEIGHBOUR (acid, which acts on me through its
          // rules — see the inbound pass below).
          if (matHot_[m] || matAttacksBody_[m]) scanHot.push_back({x, y, z});
        }
  }

  // `st.alight`, not just `st.front`, is what "this limb is still on fire"
  // means. The front is a list of cells in the index box, so DropBurnIndex has
  // to throw it away with the box — and burning drops the index constantly,
  // because every FlushBurn expresses itself as a carve and every carve
  // compacts the lattice the index points into.
  //
  // Testing the front alone therefore said "not alight" on the tick after each
  // flush, and if the world fire had gone out by then this early-out ran, the
  // index was never rebuilt, and the limb's flesh_burning / cloth_burning
  // voxels never rolled their decay again. The character was left permanently
  // sheathed in flame that had nothing left to burn instead of settling to the
  // charred materials the table already authors for it.
  if (scanHot.empty() && st.front.empty() && !st.alight) {
    // Nothing alight, nothing nearby: this limb costs exactly the walk
    // above and nothing else. The index is kept for a short grace period so
    // a limb stepping in and out of a campfire does not rebuild it every
    // other tick, then released.
    if (!st.idx.empty() && ++st.quiet > kBurnIndexGrace)
      Mob::DropBurnIndex(st);
    return changed;
  }
  st.quiet = 0;
  if (st.idx.empty()) BuildBurnIndex(v);
  if (st.idx.empty()) return changed;  // refused: absurd bounding box

  const IVec3 bd = st.dims, bm = st.min;
  auto cellOf = [&](IVec3 p) -> uint32_t {
    const int lx = p.x - bm.x, ly = p.y - bm.y, lz = p.z - bm.z;
    if (lx < 0 || ly < 0 || lz < 0 || lx >= bd.x || ly >= bd.y || lz >= bd.z)
      return kNoBurnCell;
    return (uint32_t)(((size_t)lz * bd.y + ly) * bd.x + lx);
  };
  auto posOf = [&](uint32_t c) -> IVec3 {
    return {(int)(c % (uint32_t)bd.x) + bm.x,
            (int)((c / (uint32_t)bd.x) % (uint32_t)bd.y) + bm.y,
            (int)(c / ((uint32_t)bd.x * (uint32_t)bd.y)) + bm.z};
  };
  cand.clear();
  auto queue = [&](uint32_t c) {
    if (c == kNoBurnCell) return;
    uint32_t& e = st.idx[c];
    if (e == 0 || (e & kBurnQueued)) return;  // empty, or already queued
    e |= kBurnQueued;
    cand.push_back(c);
  };
  // A NEIGHBOUR of the front only earns a rule evaluation if it can respond
  // to anything at all. In steady state most of a front's neighbours are
  // already burning or already spent, and rolling their empty buckets is
  // the difference between the front costing 1x and 7x.
  auto queueNbr = [&](uint32_t c) {
    if (c == kNoBurnCell) return;
    const uint32_t e = st.idx[c];
    if (e == 0 || (e & kBurnQueued)) return;
    const uint32_t m = v.Mat((e & ~kBurnQueued) - 1);
    if (m == 0 || m >= matGpu_.size() || matGpu_[m].reactCount == 0) return;
    queue(c);
  };

  // Candidates: the front, its lattice neighbours (spread is pushed
  // OUTWARD, never pulled by scanning for candidates), and whatever the
  // world is touching.
  for (uint32_t c : st.front) queue(c);
  for (size_t k = 0, n0 = cand.size(); k < n0; k++) {
    const IVec3 p = posOf(cand[k]);
    for (const IVec3& d : kBurnDirs)
      queueNbr(cellOf({p.x + d.x, p.y + d.y, p.z + d.z}));
  }

  // ---- world contact: seed the exposed FACE ----------------------------
  // Probing the face CENTRE would seed 1 of the 64 skin voxels a world cell
  // faces at skinScale 8, and a limb standing in acid would dissolve an
  // order of magnitude too slowly. Sampling at the lattice pitch seeds the
  // whole exposed face, which is also what makes the rate come out
  // volume-exact: 64 independent rolls on 1/64-volume voxels remove the
  // same matter per tick as one roll on one grid voxel.
  if (!scanHot.empty()) {
    const int S = std::min<int>((int)v.scale, kBurnFaceSamplesMax);
    // Its OWN budget, not the world-scan's. These count two different things —
    // kBurnScanCells bounds how many world CELLS are looked at, this bounds how
    // many face SAMPLES are transformed — and sharing one number silently tied
    // "how big a neighbourhood may I look at" to "how much of my surface may
    // catch this tick". With the face cull below in place a limb in acid spends
    // these on faces that really do touch it, so the ceiling is now reached
    // only by a body genuinely submerged.
    uint32_t probes = kBurnSeedProbes;
    for (const IVec3& c : scanHot) {
      if (!probes) break;
      for (const IVec3& d : kBurnDirs) {
        const IVec3 nb{c.x + d.x, c.y + d.y, c.z + d.z};
        if (nb.x < lo.x || nb.x > hi.x || nb.y < lo.y || nb.y > hi.y ||
            nb.z < lo.z || nb.z > hi.z)
          continue;
        const Vec3 dv{(float)d.x, (float)d.y, (float)d.z};
        const Vec3 u = d.x ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
        const Vec3 w = dv.cross(u);
        // the shared face, stepped half a LATTICE voxel into the neighbour
        const Vec3 fc = Vec3{(float)c.x + 0.5f, (float)c.y + 0.5f,
                             (float)c.z + 0.5f} +
                        dv * (0.5f + 0.5f * inv);
        // ---- CULL THE WHOLE FACE BEFORE SAMPLING IT -------------------------
        //
        // Five of these six directions fire AWAY from the limb, and each one
        // was costing S*S transforms to discover that. At skinScale 8 that is
        // 384 samples per reactive world cell, so a body submerged in acid ran
        // its per-limb probe budget out on ~10 of the several hundred cells
        // actually touching it — which is most of "acid does not do nearly
        // enough damage", the rest being the rate (see the acid+tag:organic
        // rule in reactions.json).
        //
        // One transform answers it. The S*S samples span half a world voxel
        // either side of `fc` in the face plane, so in lattice units they lie
        // within `scale/2 + 1` cells of its image; if that ball misses the
        // lattice box entirely, none of them can seed anything. Conservative in
        // the safe direction — it can only ever admit a face that has no
        // voxels, never reject one that has.
        {
          const Vec3 lc = RotateInv(q, fc - v.xf->pos) * (float)v.scale;
          const float r = (float)v.scale * 0.5f + 1.0f;
          if (lc.x < (float)bm.x - r || lc.x > (float)(bm.x + bd.x) + r ||
              lc.y < (float)bm.y - r || lc.y > (float)(bm.y + bd.y) + r ||
              lc.z < (float)bm.z - r || lc.z > (float)(bm.z + bd.z) + r)
            continue;
        }
        for (int a = 0; a < S && probes; a++)
          for (int b = 0; b < S && probes; b++) {
            probes--;
            const Vec3 p = fc + u * (((float)a + 0.5f) / (float)S - 0.5f) +
                           w * (((float)b + 0.5f) / (float)S - 0.5f);
            const Vec3 l = RotateInv(q, p - v.xf->pos) * (float)v.scale;
            const uint32_t seed = cellOf({ifloor(l.x), ifloor(l.y),
                                          ifloor(l.z)});
            // RESOLVE THE CELL FIRST. Most face samples land on nothing — the
            // six-direction walk fires away from the limb as often as toward
            // it — and asking about armour before knowing there is anything to
            // protect both wastes the march and, worse, makes the counters
            // below meaningless: 15,640 "passed" samples turned out to be
            // overwhelmingly samples that seeded nothing at all.
            if (seed == kNoBurnCell || st.idx[seed] == 0) continue;
            // ARMOUR, half one: THE FIRE IS TOUCHING THE ROBE, NOT THE ARM.
            //
            // Asked BACKWARDS, from the sample point toward the hot cell,
            // because that is the direction the heat has to travel. A point
            // test at `p` alone is not enough: `p` is a sixteenth of a world
            // voxel past the shared face, so on a cell whose box overlaps the
            // limb it lands INSIDE the flesh, past a shell that is exactly
            // where it should be.
            if (v.occlude) {
              if (v.WornAlong(p, dv * -1.0f, kWornSeedReach)) {
                wornStats_.seedsBlocked++;
                continue;
              }
              wornStats_.seedsPassed++;
            }
            queue(seed);
          }
      }
    }
  }

  // Micro limbs must OWN their brick before a poke can land: a shared model
  // backs every instance of the def, so charring one would char them all.
  // Same clone-on-first-damage the carve path does, for the same reason.
  // Every one of these is OPTIONAL on the view: a caller with no brick passes
  // no `microModel`, and the avatar has no flipbooks to invalidate so it passes
  // no `flipbook`. Writing through them unconditionally is a null dereference,
  // and it is one that only fires on the FIRST voxel the body ever loses —
  // which is why it survived the mob tests and crashed the moment the player
  // caught fire.
  auto ensureOwnedBrick = [&]() {
    if (!v.microModel || *v.microModel < 0 || !microSet_) return;
    if (v.carved && *v.carved) return;  // already a private copy
    const int own = MicroBodyOwn(*microSet_, (uint32_t)(*v.microModel));
    if (own < 0) return;  // pool full: the body burns, the skin stops keeping up
    *v.microModel = own;
    if (v.carved) *v.carved = true;
    // A carved/burnt limb must stop flipbooking: a frame swap re-points
    // rendering at an intact authored model and would heal the burns.
    if (v.flipbook) *v.flipbook = -1;
  };

  auto applyTo = [&](uint32_t cell, uint32_t prod, uint32_t rr) {
    if (prod == kProdKeep) return;
    const uint32_t vi = st.idx[cell] & ~kBurnQueued;
    if (vi == 0) return;
    const size_t i = vi - 1;
    const IVec3 p = v.At(i);
    const uint32_t pm = prod & 0xFFFu;
    ensureOwnedBrick();
    const bool solid = pm != 0 && pm < matGpu_.size() &&
                       matGpu_[pm].klass == CLASS_SOLID;
    if (solid) {
      // A state change, not a shape change: the voxel stays, its material
      // moves along the chain. This is the whole reason burn state is
      // encoded as material identity — one 16-bit poke, no re-pack, no
      // realloc, no origin shift, no rig fix-up (PLAN §3.4/§3.5).
      v.Set(i, pm, (rr >> 6) % 3u);
      if (v.carved && *v.carved && v.microModel && *v.microModel >= 0 && microSet_)
        MicroBodyPoke(*microSet_, (uint32_t)(*v.microModel), p.x, p.y, p.z,
                      (uint8_t)pm, 0);
    } else {
      // Anything not solid LEAVES the body — ash falls off, smoke and fire
      // rise, air is just a hole — and non-air products land in the grid at
      // the voxel's own world cell, so burning matter visibly wastes away
      // instead of silently vanishing.
      if (pm != 0 && pm < matGpu_.size()) {
        const uint32_t state =
            matGpu_[pm].klass == CLASS_LIQUID ? 7u : (rr >> 6) % 3u;
        const Vec3 wv = worldOf(p);
        emitCell({ifloor(wv.x), ifloor(wv.y), ifloor(wv.z)}, pm, state);
      }
      v.Set(i, 0, 0);      // tombstone; FlushBurn compacts it away
      st.idx[cell] = 0;  // gone NOW, so neighbours see through it
      st.removed++;
      if (v.carved && *v.carved && v.microModel && *v.microModel >= 0 && microSet_)
        MicroBodyPoke(*microSet_, (uint32_t)(*v.microModel), p.x, p.y, p.z,
                      0, 0);
    }
    changed = true;
  };

  // ---- run the table over each candidate --------------------------------
  for (uint32_t cell : cand) {
    if (frontBudget == 0) break;
    frontBudget--;
    const uint32_t vi0 = st.idx[cell] & ~kBurnQueued;
    if (vi0 == 0) continue;
    const size_t i = vi0 - 1;
    const uint32_t m = v.Mat(i);
    if (m == 0 || m >= matGpu_.size()) continue;
    const IVec3 vp = v.At(i);

    // The six face neighbours, gathered ONCE: the limb's own lattice first,
    // and the world cell one LATTICE step away when the lattice has nothing
    // there. A surface voxel's neighbours genuinely are grid cells, and the
    // step is 1/scale of a world voxel — stepping a whole cell would skip
    // `scale` of the limb's own voxels and read the wrong thing entirely.
    uint32_t nmat[6];
    uint32_t ncell[6];
    for (int k = 0; k < 6; k++) {
      const IVec3& d = kBurnDirs[k];
      const uint32_t nc = cellOf({vp.x + d.x, vp.y + d.y, vp.z + d.z});
      const uint32_t ni = nc == kNoBurnCell ? 0u
                                            : (st.idx[nc] & ~kBurnQueued);
      if (ni) {
        nmat[k] = v.Mat(ni - 1);
        ncell[k] = nc;
      } else {
        const Vec3 wv =
            worldOf(vp) + Rotate(q, Vec3{(float)d.x * inv, (float)d.y * inv,
                                        (float)d.z * inv});
        // ARMOUR, half two: WHAT IS OUTSIDE ME MAY BE MY OWN COAT.
        //
        // Substituting the shell's material for the world's is the whole
        // mechanic, and it does two jobs at once because both directions read
        // this array:
        //
        //   * The voxel's OWN rules count hot neighbours (flesh needs three
        //     to catch). Under a robe the neighbour is cloth, so the skin
        //     never reaches its own ignition threshold — and the robe, whose
        //     own pass sees the real fire, burns.
        //   * The INBOUND pass below looks for a world neighbour whose rule
        //     rewrites this voxel. Acid becomes cloth, so it does not match,
        //     and the acid instead eats the shell through the shell's own
        //     pass. Steel simply never dissolves, because `steel` carries no
        //     tag:dissolvable — no armour value said so.
        //
        // `ncell` stays kNoBurnCell, so the read-only rule still holds: the
        // flesh cannot rewrite the cloth. The cloth's own pass owns the cloth.
        //
        // ONLY WHEN THERE IS SOMETHING TO BE PROTECTED FROM. The world material
        // is read first and the march only runs against a cell that can
        // actually act on this body — a surface voxel facing open air is the
        // overwhelmingly common case and costs nothing. Substituting cloth for
        // AIR would change no rule anyway: neither is hot, neither attacks.
        const uint32_t wm =
            worldMatAt({ifloor(wv.x), ifloor(wv.y), ifloor(wv.z)});
        const bool threat = wm != 0 &&
                            ((wm < matHot_.size() && matHot_[wm]) ||
                             (wm < matAttacksBody_.size() &&
                              matAttacksBody_[wm]));
        const Vec3 outward{(float)d.x, (float)d.y, (float)d.z};
        const uint32_t worn =
            threat ? v.WornAlong(wv, Rotate(q, outward), kWornNbrReach) : 0u;
        if (threat && v.occlude) {
          wornStats_.nbrThreats++;
          if (worn) wornStats_.nbrSubstituted++;
          else if (v.WornAlong(wv, Rotate(q, outward), kWornNbrReach * 4.0f))
            wornStats_.nbrMissInReach++;
        }
        nmat[k] = worn ? worn : wm;
        ncell[k] = kNoBurnCell;
      }
    }

    // ---- 1. this voxel's own rules ----
    const MaterialGpu& mg = matGpu_[m];
    bool fired = false;
    for (uint32_t ri = 0; ri < mg.reactCount && !fired; ri++) {
      const ReactionGpu& r = reactions_[mg.reactOffset + ri];
      if (!ReactLightMatches(r, dayPhase_, /*seesSky=*/true)) continue;
      uint32_t chance = r.chance;
      if (ReactScaleArmed(r)) {
        // The neighbour-count ramp. THIS is the mechanic the whole feature
        // turns on: flesh authored with minCount 3 cannot ignite from a
        // lone hot face, so a single ember on a shoulder gutters out, while
        // a wide front spreads and accelerates. It reaches a body at all
        // only because sim/reactcpu.h exists.
        const bool invert = ReactScaleInverted(r);
        uint32_t cnt = 0;
        for (int k = 0; k < 6; k++)
          if (ReactNbrMatches(r, nmat[k], matGpu_) != invert) cnt++;
        chance = ReactScaledChance(r, cnt);
        if (chance == 0) continue;
      }
      const uint32_t rr = Hash3(limbKey ^ (cell * 2246822519u), tick, ri);
      if (rr % kReactChanceDen >= chance) continue;
      const uint32_t kind = r.packed & 3u;
      if (kind == kReactDecay) {
        applyTo(cell, r.prodSelf, rr);
        fired = true;
      } else if (kind == kReactEmit) {
        // Emit in an allowed WORLD direction: fire rises in world space no
        // matter how the limb is posed. First direction the limb does not
        // itself occlude.
        const uint32_t dmask = (r.packed >> 2u) & 7u;
        IVec3 cnd[6];
        int nc2 = 0;
        if (dmask & kDirUp) cnd[nc2++] = {0, 1, 0};
        if (dmask & kDirDown) cnd[nc2++] = {0, -1, 0};
        if (dmask & kDirSide) {
          const IVec3 side[4] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
          const uint32_t rot = rr >> 12u;
          for (int k = 0; k < 4; k++) cnd[nc2++] = side[(rot + k) & 3u];
        }
        const Vec3 wv = worldOf(vp);
        for (int k = 0; k < nc2; k++) {
          const IVec3 t{ifloor(wv.x) + cnd[k].x, ifloor(wv.y) + cnd[k].y,
                        ifloor(wv.z) + cnd[k].z};
          emitCell(t, r.prodNbr, (rr >> 8u) % 3u);
          fired = true;  // the rule is consumed even if the budget refused
          break;
        }
      } else {  // kReactPair
        int match = -1;
        for (int k = 0; k < 6; k++)
          if (nmat[k] && ReactNbrMatches(r, nmat[k], matGpu_)) { match = k; break; }
        if (match < 0) continue;
        // World neighbours are READ-ONLY from this side: a limb cannot
        // rewrite grid content, so only rules that keep the neighbour may
        // match against one.
        const bool isWorld = ncell[match] == kNoBurnCell;
        if (isWorld && r.prodNbr != kProdKeep) continue;
        applyTo(cell, r.prodSelf, rr);
        if (!isWorld && r.prodNbr != kProdKeep)
          applyTo(ncell[match], r.prodNbr, Pcg(rr));
        fired = true;
      }
    }
    if (fired) continue;  // at most one rule per voxel per tick, file order

    // ---- 2. INBOUND: a world neighbour's rule that rewrites ME ----------
    // Acid is authored as `acid + tag:dissolvable -> neighborBecomes air`,
    // i.e. from the ACID's side, which is also the direction the GPU
    // evaluates it. Running that direction here is what makes a limb
    // dissolve in acid with no acid-specific code and, more importantly,
    // with no mirrored rule to keep in step: mirroring it per body material
    // would be an N x M table whose two halves would drift.
    for (int k = 0; k < 6 && !fired; k++) {
      if (ncell[k] != kNoBurnCell) continue;  // lattice neighbour, handled above
      const uint32_t wm = nmat[k];
      if (wm == 0 || wm >= matRewritesNbr_.size() || !matRewritesNbr_[wm])
        continue;
      const MaterialGpu& wg = matGpu_[wm];
      for (uint32_t rj = 0; rj < wg.reactCount; rj++) {
        const ReactionGpu& r = reactions_[wg.reactOffset + rj];
        if ((r.packed & 3u) != kReactPair || r.prodNbr == kProdKeep) continue;
        if (!ReactNbrMatches(r, m, matGpu_)) continue;
        if (!ReactLightMatches(r, dayPhase_, /*seesSky=*/true)) continue;
        // A distinct rule-index space (+64) from the self pass, so a voxel
        // that is both burning and dissolving does not roll one stream
        // twice and correlate the two.
        const uint32_t rr =
            Hash3(limbKey ^ (cell * 668265263u), tick, 64u + rj);
        if (rr % kReactChanceDen >= r.chance) continue;
        applyTo(cell, r.prodNbr, rr);
        fired = true;
        break;
      }
    }
  }


  // Clear the per-tick queued bits and rebuild the front from what is actually
  // alight now. Candidates skipped for budget keep their material, so an
  // exhausted budget slows the fire down rather than putting it out.
  st.front.clear();
  for (uint32_t c : cand) {
    uint32_t& e = st.idx[c];
    e &= ~kBurnQueued;
    if (e == 0) continue;
    const uint32_t m = v.Mat(e - 1);
    if (m && m < matSelfActive_.size() && matSelfActive_[m]) st.front.push_back(c);
  }
  // Only the CANDIDATES were re-tested, so an empty front here does not by
  // itself mean the limb is out — a budget-starved tick or a fire on the far
  // side of the same limb leaves alight cells that were never candidates. The
  // authority on "is anything on this limb still burning" is the full material
  // sweep in BuildBurnIndex, which is exactly what runs once the index is next
  // rebuilt; until then a non-empty front is the only positive evidence there
  // is, and it may only ADD to the flag, never clear it.
  if (!st.front.empty()) st.alight = true;
  return changed;
}

// The MOB driver. One view per live limb, then this system's own flush — which
// is a CarveLimb, so a limb burnt through severs and a limb burnt below the
// collapse fraction comes off, both by the existing geometry rules rather than
// by anything burning had to invent.
void MobSystem::BurnLimbs(uint32_t tick, World& world,
                          std::vector<CellOp>& cellOps,
                          std::vector<ParticleSpawn>& spawns) {
  if (!BurnTablesReady() || mobs_.empty()) return;
  uint32_t frontBudget = kBurnFrontPerTick;
  uint32_t opsBudget = kBurnOpsPerTick;
  for (size_t mi = 0; mi < mobs_.size() && frontBudget; mi++)
    mobs_[mi].BurnTick(tick, world, cellOps, spawns, frontBudget, opsBudget);
}

void Mob::BurnTick(uint32_t tick, World& world, std::vector<CellOp>& cellOps,
                   std::vector<ParticleSpawn>& spawns, uint32_t& frontBudget,
                   uint32_t& opsBudget) {
  // A corpse's limbs belong to DebrisSystem the moment Die() adopts them;
  // limb.body is cleared there, so this is belt and braces.
  if (!sys_ || !sys_->BurnTablesReady() || !alive_) return;
  // Counter-based RNG stream. NO FLOAT TERM, deliberately: a limb's world
  // position and velocity are Jolt floats, and keying a roll on one would
  // inject physics float state into a HASHED grid write and make the world
  // hash frame-rate dependent. That is the one way to break rule 1 from here
  // and it looks entirely innocent at the call site (PLAN §5).
  const uint32_t mobKey = (uint32_t)id_ * 0x9E3779B9u;

  // The occlusion context, hoisted out of the loop so its address is stable
  // for the whole call. `limb` is rewritten per iteration; BurnOneLimb runs
  // synchronously and never keeps the pointer, so one instance is enough.
  WornProbe probe{this, -1};
  for (int li = 0; li < (int)limbs_.size(); li++) {
    if (frontBudget == 0) break;
    if (!limbs_[li].body) continue;
    BurnLimbView v = ViewOf(limbs_[li]);
    // Only a limb something is actually WORN OVER pays for the probe, and an
    // undressed creature does not even transform a point. `worn_` is empty for
    // every mob in the game today, so this is a vector-empty test per limb.
    if (LimbHasShells(li)) {
      probe.limb = li;
      v.occlude = &WornProbe::Call;
      v.occludeCtx = &probe;
    }
    const uint32_t key = mobKey + (uint32_t)li * 2654435761u;
    if (sys_->BurnOneLimb(v, tick, key, world, cellOps, frontBudget, opsBudget))
      MarkInstancesDirty();
    // Batched maintenance. May sever the limb or kill the creature, in which
    // case the limb list has been reshaped and nothing below may touch it --
    // including the rest of this loop.
    if (limbs_[li].burn.removed && !FlushBurn(li, world, spawns, false))
      break;
  }
}

bool MobSystem::CarveLimbRadial(uint64_t bodyHandle, Vec3 centerWorldVoxel,
                                float radiusVoxels, bool ragged, bool eject,
                                World& world,
                                std::vector<ParticleSpawn>& spawns) {
  for (Mob& mob : mobs_)
    if (mob.CarveLimbRadial(bodyHandle, centerWorldVoxel, radiusVoxels, ragged,
                            eject, world, spawns))
      return true;
  return false;
}

bool Mob::CarveLimbRadial(uint64_t bodyHandle, Vec3 centerWorldVoxel,
                          float radiusVoxels, bool ragged, bool eject,
                          World& world,
                          std::vector<ParticleSpawn>& spawns) {
  if (!phys_ || radiusVoxels <= 0.0f) return false;
  for (size_t i = 0; i < limbs_.size(); i++) {
    if (limbs_[i].body != bodyHandle) continue;
    const MobDef& def = *def_;
    MobLimb& limb = limbs_[i];
    phys_->GetTransform(limb.body, limb.xf);
    // ONE world-space sphere, re-expressed per lattice. CarveLimb calls this
    // once with physScale and, on a fine-skinned limb, again with skinScale --
    // the centre and radius are converted into whichever frame the voxels
    // being tested live in, so the same physical volume is removed from both.
    //
    // This is what makes precision scale with the def: at skinScale 8 a
    // radius of 0.125 world voxels is a single skin voxel, so a fine enough
    // tool can take out one cell of a brain without any new code path.
    Quat q{limb.xf.quat[0], limb.xf.quat[1], limb.xf.quat[2], limb.xf.quat[3]};
    const Vec3 cBody = RotateInv(q, centerWorldVoxel - limb.xf.pos);
    const uint32_t seed = (uint32_t)id_ * 2654435761u + (uint32_t)i;
    // The ragged-rim jitter is keyed on the SKIN lattice regardless of which
    // lattice is being tested, so the crater's shape is a property of the
    // art rather than of whatever collider resolution the engine happened to
    // derive -- otherwise the same blast would tear differently on two rigs
    // that only differ in limb size.
    const float jitterScale = (float)std::max(1u, SkinScaleOf(limb));

    // ---- crater shape (tuning.json gore.carve*) ----------------------------
    const auto& gt = CurrentTuning().gore;
    // SEVERITY COUPLES TO THE BLAST. A grazing hit from a distant explosion
    // and a charge going off against the elbow should not tear the same way,
    // and the honest measure of "how big was this, for THIS limb" is the blast
    // radius against the limb's own extent — a radius that swallows the arm
    // chunks it, one that clips the edge still stipples. `l.size` is in
    // collider units, hence the physScale division (the same conversion
    // CarveRadialAll does for its reject sphere).
    float limbExtent = 1.0f;
    {
      const float inv = 1.0f / (float)std::max(1u, PhysScaleOf(limb));
      limbExtent = std::max(
          0.5f, 0.5f * Vec3{(float)limb.size.x, (float)limb.size.y,
                            (float)limb.size.z}.len() * inv);
    }
    // 0 at a graze, 1 once the blast is as big as the limb. Bounded above so a
    // huge explosion does not keep escalating past "took the whole thing".
    const float severity =
        std::clamp(radiusVoxels / limbExtent, 0.0f, 1.0f);
    // The master slider, scaled by severity so distance still reads. Everything
    // below lerps from the OLD behaviour at chunk == 0 — that identity is what
    // makes the slider an A/B rather than an approximation, and the mob gate
    // asserts it.
    const float chunk =
        std::clamp(gt.carveChunkiness, 0.0f, 1.0f) * (0.35f + 0.65f * severity);
    const float falloffExp = 1.0f + (gt.carveFalloff - 1.0f) * chunk;
    const float blob = std::max(gt.carveBlobSize, 0.5f);

    // The spall pass, filled ONLY on this path — a null pointer is what keeps
    // the burn flush (mob.cpp's FlushBurn carve) and the laser's clean bore out
    // of it by construction, rather than by each of them remembering to say no.
    // `ragged` is part of the gate for the same reason: a kerf that spalled
    // would stop being a kerf.
    Mob::CarveSpall spallData{};
    const bool wantSpall = ragged && chunk > 0.0f && gt.carveSpallRounds > 0;
    if (wantSpall) {
      spallData.centerLocal = cBody;
      spallData.radius = radiusVoxels;
      // Severity again: a close blast tears its rim apart, a distant one just
      // stipples. Same coupling the falloff gets, so the two move together.
      spallData.strength = std::clamp(0.55f * chunk, 0.0f, 1.0f);
      spallData.rounds = gt.carveSpallRounds;
      spallData.seed = seed;
    }
    const Mob::CarveSpall* spallPtr = wantSpall ? &spallData : nullptr;

    CarveLimb(
        (int)i, world, spawns, eject,
        [&, cBody, seed, jitterScale, chunk, falloffExp,
         blob](float scale) -> LimbCarveKeep {
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
            // so a float hash is fine -- rule 1 governs the grid.
            float t = std::sqrt(d2 / rLocal2);
            float chance = 1.0f - t * t;
            // TIGHTEN THE CRATER. `1 - t^2` is 0.75 at half the radius and
            // 0.36 at 80% of it, which is why a large blast really did remove
            // a bit of everything it touched instead of a hole where it hit.
            // Raising it to a power concentrates the removal without moving
            // the endpoints (it is still 1 at the centre and 0 at the rim), so
            // the crater gets tighter rather than smaller.
            if (falloffExp > 1.0f) chance = std::pow(chance, falloffExp);
            // Quantised onto the skin lattice so both passes agree on which
            // part of the rim is torn.
            int sx = (int)std::floor((float)x * toSkin);
            int sy = (int)std::floor((float)y * toSkin);
            int sz = (int)std::floor((float)z * toSkin);
            const uint32_t h = Hash3(seed, (uint32_t)sx * 73856093u,
                                     (uint32_t)sy * 19349663u ^
                                         (uint32_t)sz * 83492791u);
            const float white = (float)(h & 0xFFFFu) / 65535.0f;
            // CORRELATED NOISE INSTEAD OF WHITE NOISE.
            //
            // An independent draw per voxel has no feature size, so what it
            // removes is a fine speckle by construction — no falloff shape can
            // make speckle into a chunk. Sampling a SMOOTH field at
            // carveBlobSize instead gives neighbouring voxels correlated draws,
            // so they leave together, and the blob size IS the size of the
            // piece that comes off.
            //
            // Sampled on the SKIN lattice for the same reason the jitter above
            // is: the crater's shape is a property of the art, so the same
            // blast tears identically whatever collider resolution the engine
            // derived for this def.
            float draw = white;
            if (chunk > 0.0f) {
              const float smooth =
                  ValueNoise3(seed, (float)sx / blob, (float)sy / blob,
                              (float)sz / blob);
              draw = white + (smooth - white) * chunk;
            }
            return draw >= chance;
          };
        },
        spallPtr);
    return true;
  }
  return false;
}

void MobSystem::CarveMobsRadial(Vec3 centerWorldVoxel, float radiusVoxels,
                                World& world,
                                std::vector<ParticleSpawn>& spawns) {
  for (Mob& mob : mobs_)
    mob.CarveRadialAll(centerWorldVoxel, radiusVoxels, world, spawns);
}

void Mob::CarveRadialAll(Vec3 centerWorldVoxel, float radiusVoxels,
                         World& world, std::vector<ParticleSpawn>& spawns) {
  if (!phys_ || !alive_ || radiusVoxels <= 0.0f) return;
  // Collect handles FIRST: carving rebuilds colliders and can sever limbs or
  // kill the creature, both of which reshape the limb list and its handles
  // mid-walk. Iterating the live structure while it mutates under us is how
  // this kind of loop usually acquires a use-after-free.
  std::vector<uint64_t> handles;
  for (const MobLimb& l : limbs_)
    if (l.body) {
      BodyTransform xf{};
      phys_->GetTransform(l.body, xf);
      // cheap reject against the limb's bounding sphere. l.size is in
      // COLLIDER units, so it divides by physScale.
      const float inv = 1.0f / (float)std::max(1u, PhysScaleOf(l));
      float r =
          0.5f * Vec3{(float)l.size.x, (float)l.size.y, (float)l.size.z}.len() *
          inv;
      if ((xf.pos - centerWorldVoxel).len() <= radiusVoxels + r + 2.0f)
        handles.push_back(l.body);
    }
  for (uint64_t h : handles)
    CarveLimbRadial(h, centerWorldVoxel, radiusVoxels, true /*ragged*/,
                    true /*eject*/, world, spawns);
}

void MobSystem::Sever(uint64_t mobId, int limbIndex) {
  Mob* mob = FindMobById(mobId);
  if (mob) mob->Sever(limbIndex);
}

void Mob::Sever(int limbIndex) {
  if (!def_) return;
  const MobDef& def = *def_;
  if (limbIndex < 0 || limbIndex >= (int)limbs_.size()) return;
  // limbDefs_, not def.limbs: a borrowed item slot severs like any limb (the
  // sword is cut out of the hand), and its def row only exists on the copy.
  const MobLimbDef& ld = limbDefs_[limbIndex];
  // HOW MUCH OF THE LIMB WAS STILL THERE WHEN IT CAME OFF.
  //
  // The one number that separates the two ways a limb can leave: it ran out of
  // matter (small fraction — geometry, kLimbCollapseFraction), or something
  // took it while it was still a limb (large fraction — a damage rule, or the
  // connectivity split handing its identity to a straggler). Both of those
  // bugs shipped, both looked identical from outside as "a limb came off", and
  // the fraction names them apart on sight. Recorded here because this is the
  // only instant it is knowable: a heartbeat later the limb is debris and, if
  // it was vital, the whole limb list is gone.
  if (sys_) {
    const MobLimb& L = limbs_[limbIndex];
    const uint32_t at0 = L.voxelsAtSpawn;
    const uint32_t now =
        (uint32_t)(L.HasFineSkin() ? L.skinVoxels.size() : L.voxels.size());
    const float frac = at0 ? (float)now / (float)at0 : 0.0f;
    if (frac > sys_->worstSeverFrac_) {
      sys_->worstSeverFrac_ = frac;
      sys_->worstSeverLimb_ = ld.name;
    }
  }
  if (limbIndex == def.rootLimb || ld.vital || !ld.severable) {
    Die();
  } else {
      // The cut point in WORLD space, captured BEFORE DetachLimb: the joint
      // anchor expressed through the severed limb's own live transform.
      //
      // The obvious-looking `mob.origin_ + anchorRoot` is wrong and was the bug
      // behind blood appearing under the mob and on its wrong side. anchorRoot
      // is a REST-POSE offset in the prefab authoring frame, so using it raw
      // ignores (a) the mob's heading — a creature facing away sprays from the
      // mirrored side — and (b) `mob.bodyY_`, the animated body height, where
      // mob.origin_.y is only the spawn corner, so the wound sits at the mob's
      // feet while the body stands above it.
      //
      // limb.xf is the pose Jolt is actually holding this instant, and
      // anchorLimb is the same joint in that limb's local frame, so this is
      // correct under any animation, heading or slope with no frame rebuild —
      // the identical construction Damage() uses to locate a hit.
      const MobLimb& cut = limbs_[limbIndex];
      Quat cq{cut.xf.quat[0], cut.xf.quat[1], cut.xf.quat[2], cut.xf.quat[3]};
      Vec3 anchorW = cut.body ? cut.xf.pos + Rotate(cq, cut.anchorLimb)
                              : origin_ + cut.anchorRoot;

      // Report the cut for audio. Recorded HERE, before DetachLimb, because
      // anchorW is the one place the wound's world position is known — after
      // the detach the limb has its own frame and the joint is gone.
      if (sys_)
      sys_->severs_.push_back(MobSystem::SeverEvent{
          anchorW, id_, limbIndex, defIndex_, sys_->bladeCut_,
          sys_->bladeCut_ ? sys_->bladeSeverity_ : 1.0f});

      // ---- A GARMENT THAT COMES OFF IS NOT AN AMPUTATION -------------------
      //
      // Everything below this point is GORE — a stump wound on the parent, an
      // arterial gout armed for severDecayTicks, and a throw of conserved blood
      // voxels. A worn shell reaching here means a robe burnt through or a
      // strap was cut, and running the gore path on it sprayed the WEARER's
      // blood out of their own coat. Suppressed for the same reason CarveLimb
      // suppresses the drip: see Mob::IsWornSlot.
      //
      // FIRE IS THE OTHER SUPPRESSION, and it applies to real limbs too: an arm
      // that burns THROUGH parts company already cauterised, and arming a gout
      // there is the second half of "being on fire causes spurts like crazy".
      const bool worn = IsWornSlot(limbIndex);
      const bool gore = !worn && !inBurnFlush_;
      // ...AND IT MUST NOT BECOME A RIGIDBODY INSIDE THE WEARER.
      //
      // DetachLimb(adopt=true) hands the slot to DebrisSystem, which makes it
      // an ordinary dynamic body — spawned exactly overlapping the player
      // capsule it was strapped to, and outside Layers::AVATAR, so Jolt's
      // resolution of that overlap fires the player across the room. That is
      // the reported "clothes burning off launches the player", and it is not
      // a physics tuning problem: a garment consumed BY FIRE has nothing left
      // to fall off, so there should be no body at all. Rags cut loose by a
      // blade still drop, because that is a piece of gear hitting the floor.
      const bool adopt = !(worn && inBurnFlush_);
      DetachLimb(limbIndex, adopt);
      if (!adopt) {
        // Burnt to nothing: make the lattice say so, or CaptureWorn (which
        // reads the shells, not the body) would record the last un-flushed
        // state and the piece would come out of the wardrobe intact.
        MobLimb& gone = limbs_[limbIndex];
        gone.skinVoxels.clear();
        gone.voxels.clear();
        gone.hp = 0.0f;
      }
      if (!gore) return;
      // the stump bleeds: wound at the joint on the PARENT side
      for (size_t k = 0; k < limbDefs_.size(); k++)
        if (limbDefs_[k].name == ld.parent) {
          MobLimb& parent = limbs_[k];
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
          parent.gushTicks = gore_.severDecayTicks;
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
          const uint32_t es = (uint32_t)id_ ^ ((uint32_t)limbIndex << 16);
          // Sever is not driven by the tick loop, so the draw is indexed by the
          // voxel counter alone; the mob id keeps it distinct between mobs.
          int nVox = EventVarI(gore_.severVoxels, gore.severVoxelsVar, es,
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
            uint32_t h = Hash3((uint32_t)id_ * 22695477u + (uint32_t)limbIndex,
                               (uint32_t)k, 0x5EEDu);
            Vec3 dir{parent.gushDir.x + SignedUnit(h) * 0.7f,
                     std::fabs(parent.gushDir.y) + 0.3f,
                     parent.gushDir.z + SignedUnit(Pcg(h ^ 0x31u)) * 0.7f};
            dir = Rotate(q, dir);
            float sp = EventVar(gore_.severVoxelSpeed, gore.severVoxelSpeedVar,
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
}

void Mob::DetachLimb(int limbIndex, bool adopt) {
  MobLimb& limb = limbs_[limbIndex];
  if (!limb.body) return;
  // The lattice is about to leave this system for DebrisSystem::AdoptBody, and
  // a material-0 tombstone from an unflushed burn must not go with it.
  StripBurnTombstones(limb);
  DropBurnIndex(limb.burn);
  if (limb.joint) {
    // DestroyJoint calls Jolt's RemoveConstraint and drops the ref — the
    // constraint is GONE, not left disabled. A disabled constraint would keep
    // the severed limb tethered to a body it no longer belongs to.
    phys_->DestroyJoint(limb.joint);
    limb.joint = 0;
  }
  // children of this limb are orphaned too: their joints attach to it and
  // die with the body chain when severed recursively
  const MobDef& def = *def_;
  for (size_t k = 0; k < limbDefs_.size(); k++)
    if (limbDefs_[k].parent == limbDefs_[limbIndex].name && limbs_[k].body)
      DetachLimb((int)k, adopt);

  // The limb becomes debris NOW (so counts and rendering are immediate), but
  // stays kinematic in its last animated pose for a beat before going
  // dynamic. Cutting straight to ragdoll on the hit frame reads as a
  // teleport; the brief hold sells the cut.
  if (limbIndex >= 0 && limbIndex < (int)anim_.partAlive.size())
    anim_.partAlive[limbIndex] = 0;  // gait stops scheduling it, IK -> 0
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
                       limb.MicroRef(SkinScaleOf(limb)), PhysScaleOf(limb),
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
  MarkInstancesDirty();
}

// De-duplicated per mob per drain window for Hurt: an explosion that carves
// six limbs of one creature is one cry, not six overlapping copies of the same
// sample. Death is not de-duplicated because Die() can only run once per mob.
//
// The AUDIO layer also rate-limits Hurt by wall clock (kMobVoiceMinGap), and
// that is not the same guard: it cannot stop the queue itself from growing,
// and it is bypassed entirely in headless runs where there is no audio at all.
// Bounding the event is this layer's job (CLAUDE.md rule 2); choosing not to
// play it is the audio layer's.
void MobSystem::PushVoice(const Mob& mob, VoiceKind kind, Vec3 posVoxel,
                          float intensity) {
  if (kind == VoiceKind::Hurt)
    for (const VoiceEvent& v : voices_)
      if (v.mobId == mob.id_ && v.kind == VoiceKind::Hurt) return;
  voices_.push_back(VoiceEvent{posVoxel, mob.id_, (int)mob.defIndex_, kind,
                               std::clamp(intensity, 0.0f, 1.0f)});
}

void Mob::Die() {
  if (!alive_) return;
  alive_ = false;
  // The death cry, BEFORE the limb list is dismantled below — the root limb's
  // live transform is where the creature actually is, and `mob.origin_` is only
  // the spawn corner (the trap called out in Sever()). Reported even if the
  // mob binds no death take: the audio layer is what decides silence, and a
  // test asserting "the engine noticed this creature died" needs the event to
  // exist whether or not anyone recorded a sound for it.
  {
    Vec3 at = origin_;
    const int rl = def_ ? def_->rootLimb : -1;
    if (rl >= 0 && rl < (int)limbs_.size() && limbs_[rl].body)
      at = limbs_[rl].xf.pos;
    if (sys_) sys_->PushVoice(*this, MobSystem::VoiceKind::Death, at, 1.0f);
  }
  // whole-body ragdoll: every limb goes dynamic and becomes debris; joints
  // stay so the corpse hangs together until pieces get culled or settle
  for (size_t i = 0; i < limbs_.size(); i++) {
    MobLimb& limb = limbs_[i];
    if (!limb.body) continue;
    // Same reason as DetachLimb: the lattice is handed to DebrisSystem here,
    // and an unflushed burn tombstone must not travel with it.
    StripBurnTombstones(limb);
    DropBurnIndex(limb.burn);
    phys_->SetBodyKinematic(limb.body, false);
    debris_->AdoptBody(limb.body, limb.voxels, limb.xf,
                       limb.MicroRef(SkinScaleOf(limb)), PhysScaleOf(limb),
                       std::move(limb.skinVoxels), def_->bleedMat);
    // NOTE THE ABSENT OnBodyReleasedToWorld. A limb that is SEVERED gets its
    // avatar-layer exemption stripped, but only after kSeverHoldSeconds
    // (TickSeveredHolds) — by then it has swung clear of the capsule it grew
    // out of. A corpse has no such beat: on death EVERY limb goes dynamic at
    // once, in the pose it was standing in, i.e. entirely inside the player's
    // capsule proxy. Handing those to the plain MOVING layer in the same frame
    // hands Jolt a dozen deep, unresolvable penetrations against the proxy and
    // the solver does the only thing it can — it fires the whole ragdoll out of
    // the capsule in whatever direction the overlap resolved. That is the "I
    // died and my body went FLYING" bug; it should keel over.
    //
    // So the corpse keeps the exemption for good. The cost is that you can walk
    // through your own remains after respawning, which nobody can see; the
    // alternative is a launch nobody can miss.
    limb.skinVoxels.clear();
    limb.carved = false;  // brick ownership moved with the body (see DetachLimb)
    limb.body = 0;
    if (limb.joint) limb.joint = 0;  // ownership follows the bodies now
    if (i < anim_.partAlive.size()) anim_.partAlive[i] = 0;
  }
  MarkInstancesDirty();
  // Death goes straight to ragdoll (no hold): the whole body flips at once,
  // so there is no "still-attached" pose left to sell. The husk is removed on
  // the next PreTick sweep once nothing is holding; drop the limb list but
  // keep any in-flight hold entries alive.
  // ...except for the avatar, which keeps its limb list so the HUD's
  // per-part readout survives the death screen (DropLimbListOnDeath).
  if (DropLimbListOnDeath()) {
    std::vector<MobLimb> holding;
    for (MobLimb& l : limbs_)
      if (l.holdBody) holding.push_back(std::move(l));
    limbs_ = std::move(holding);
  }
}

void MobSystem::AppendInstances(std::vector<BodyVoxInst>& out,
                                uint32_t slotBase) {
  uint32_t slot = slotBase;
  for (Mob& mob : mobs_) slot = mob.AppendInstances(out, slot);
  instancesDirty_ = false;
}

uint32_t Mob::AppendInstances(std::vector<BodyVoxInst>& out, uint32_t slotBase) {
  uint32_t slot = slotBase;
  for (size_t i = 0; i < limbs_.size(); i++) {
    const MobLimb& limb = limbs_[i];
    if (!limb.body) continue;
    if (slot >= kMaxBodySlots) break;
    // Slots WITH a micro model render through the OBB/brick-march pass, so
    // they must not also emit cube instances — that would draw the limb
    // twice, at the wrong size (cube instances are one WORLD voxel each).
    // The slot is still consumed: slots are shared with the micro pass.
    // Hidden limbs (first-person body suppression) also consume theirs.
    if (limb.microModel >= 0 || LimbHidden((int)i)) {
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
  return slot;
}

void MobSystem::AppendXforms(std::vector<BodyXformGpu>& out) const {
  for (const Mob& mob : mobs_) mob.AppendXforms(out);
}

void Mob::AppendXforms(std::vector<BodyXformGpu>& out) const {
  for (const MobLimb& limb : limbs_) {
    if (!limb.body) continue;
    if (out.size() >= kMaxBodySlots) return;
    rigrender::AppendXform(out, limb.xf);
  }
}


// ---- collision-box debug overlay (world.h DebugBox) -------------------------
//
// Draws the individual sub-shapes of each compound collider rather than one
// big AABB per body. See rigrender::AppendDebugBoxesFor for why these come
// from the Jolt shape rather than from the voxels that built it.
void MobSystem::AppendDebugBoxes(std::vector<DebugBox>& out, size_t limit,
                                 uint32_t color) const {
  for (const Mob& mob : mobs_) mob.AppendDebugBoxes(out, limit, color);
}

void Mob::AppendDebugBoxes(std::vector<DebugBox>& out, size_t limit,
                           uint32_t color) const {
  if (!phys_) return;
  static std::vector<SubShapeBox> subs;
  for (const MobLimb& limb : limbs_) {
    if (!limb.body) continue;
    if (out.size() >= limit) return;
    rigrender::AppendDebugBoxesFor(out, *phys_, limb.body, limb.xf, limit,
                                   color, subs);
  }
}

void MobSystem::AppendMicroInsts(std::vector<MicroBodyInstGpu>& out,
                                 uint32_t slotBase) const {
  uint32_t slot = slotBase;
  for (const Mob& mob : mobs_) slot = mob.AppendMicroInsts(out, slot);
}

uint32_t Mob::AppendMicroInsts(std::vector<MicroBodyInstGpu>& out,
                               uint32_t slotBase) const {
  // Walks slots in the SAME order as AppendXforms/AppendInstances, so the slot
  // it records is the one the transform lands in.
  uint32_t slot = slotBase;
  for (size_t i = 0; i < limbs_.size(); i++) {
    const MobLimb& limb = limbs_[i];
    if (!limb.body) continue;
    if (slot >= kMaxBodySlots) return slot;
    if (limb.microModel >= 0 && !LimbHidden((int)i))
      out.push_back({slot, (uint32_t)limb.microModel, 0, 0});
    slot++;
  }
  return slot;
}

uint64_t MobSystem::LimbBody(uint64_t mobId, int limbIndex) const {
  for (const Mob& mob : mobs_)
    if (mob.id_ == mobId && limbIndex >= 0 && limbIndex < (int)mob.limbs_.size())
      return mob.limbs_[limbIndex].body;
  return 0;
}

bool MobSystem::IsAlive(uint64_t mobId) const {
  for (const Mob& mob : mobs_)
    if (mob.id_ == mobId) return mob.alive_;
  return false;
}


Vec3 MobSystem::MobOrigin(uint64_t mobId) const {
  for (const Mob& mob : mobs_)
    if (mob.id_ == mobId) return mob.origin_;
  return {};
}

Vec3 MobSystem::MobFacing(uint64_t mobId) const {
  // Must stay byte-identical to the `fwd` in the walk step and to
  // AxisAngle({0,1,0}, heading) applied to +Z — one convention, one formula.
  for (const Mob& mob : mobs_)
    if (mob.id_ == mobId)
      return Vec3{std::sin(mob.heading_), 0, std::cos(mob.heading_)};
  return {0, 0, 1};
}

float MobSystem::MobHeading(uint64_t mobId) const {
  for (const Mob& mob : mobs_)
    if (mob.id_ == mobId) return mob.heading_;
  return 0;
}

float MobSystem::MobDesiredHeading(uint64_t mobId) const {
  for (const Mob& mob : mobs_)
    if (mob.id_ == mobId) return mob.desiredHeading_;
  return 0;
}

float MobSystem::MobTurnVel(uint64_t mobId) const {
  for (const Mob& mob : mobs_)
    if (mob.id_ == mobId) return mob.turnVel_;
  return 0;
}

void MobSystem::SetDesiredHeading(uint64_t mobId, float radians) {
  for (Mob& mob : mobs_)
    if (mob.id_ == mobId) {
      mob.desiredHeading_ = radians;
      // Deliberately does NOT touch `heading` or `turnVel`: an external
      // steering command is subject to exactly the same turn-rate clamp the
      // wander behaviour is. That is the invariant that keeps a future
      // "face the player" from becoming an instant snap.
      return;
    }
}

int MobSystem::SwingingFeet(uint64_t mobId) const {
  for (const Mob& mob : mobs_) {
    if (mob.id_ != mobId) continue;
    int n = 0;
    for (const FootState& f : mob.anim_.feet) n += (f.valid && f.swinging) ? 1 : 0;
    return n;
  }
  return -1;
}

int MobSystem::PlantedFeet(uint64_t mobId) const {
  for (const Mob& mob : mobs_) {
    if (mob.id_ != mobId) continue;
    int n = 0;
    for (const FootState& f : mob.anim_.feet) n += (f.valid && !f.swinging) ? 1 : 0;
    return n;
  }
  return -1;
}

int MobSystem::ActiveClips(uint64_t mobId) const {
  for (const Mob& mob : mobs_)
    if (mob.id_ == mobId) return (int)mob.anim_.clips.size();
  return -1;
}

int MobSystem::LocoState(uint64_t mobId) const {
  for (const Mob& mob : mobs_)
    if (mob.id_ == mobId) return mob.anim_.locoState;
  return -1;
}

std::vector<std::pair<std::string, float>> MobSystem::ClipWeights(
    uint64_t mobId) const {
  std::vector<std::pair<std::string, float>> out;
  for (const Mob& mob : mobs_) {
    if (mob.id_ != mobId) continue;
    const AnimSkeleton& sk = defs_[mob.defIndex_].skel;
    for (const ClipInstance& ci : mob.anim_.clips) {
      if (ci.clip < 0 || ci.clip >= (int)sk.clips.size()) continue;
      out.push_back({sk.clips[ci.clip].name, ci.weight * ci.fade});
    }
  }
  return out;
}

Vec3 MobSystem::LimbLocalUp(uint64_t mobId, int limbIndex) const {
  for (const Mob& mob : mobs_) {
    if (mob.id_ != mobId) continue;
    if (limbIndex < 0 || limbIndex >= (int)mob.anim_.local.size()) break;
    return QuatRotate(mob.anim_.local[limbIndex].rot, {0, 1, 0});
  }
  return {0, 1, 0};
}

Vec3 MobSystem::LimbModelUp(uint64_t mobId, int limbIndex) const {
  for (const Mob& mob : mobs_) {
    if (mob.id_ != mobId) continue;
    if (limbIndex < 0 || limbIndex >= (int)mob.anim_.model.size()) break;
    return QuatRotate(mob.anim_.model[limbIndex].rot, {0, 1, 0});
  }
  return {0, 1, 0};
}

uint32_t MobSystem::LimbVoxelCount(uint64_t mobId, int limbIndex) const {
  for (const Mob& mob : mobs_)
    if (mob.id_ == mobId && limbIndex >= 0 && limbIndex < (int)mob.limbs_.size())
      return (uint32_t)mob.limbs_[limbIndex].voxels.size();
  return 0;
}

uint32_t MobSystem::LimbOpenFaceCount(uint64_t mobId, int limbIndex,
                                      int minOpen) const {
  for (const Mob& mob : mobs_) {
    if (mob.id_ != mobId || limbIndex < 0 ||
        limbIndex >= (int)mob.limbs_.size())
      continue;
    const MobLimb& limb = mob.limbs_[limbIndex];
    // Measured on the SKIN when there is one, for the same reason the crater
    // noise is keyed there: the shape belongs to the art, not to whichever
    // collider resolution the engine derived.
    const bool fine = limb.HasFineSkin();
    auto key = [](int x, int y, int z) -> uint64_t {
      return ((uint64_t)(uint32_t)(x + 32768) << 42) |
             ((uint64_t)(uint32_t)(y + 32768) << 21) |
             (uint64_t)(uint32_t)(z + 32768);
    };
    std::unordered_set<uint64_t> live;
    if (fine)
      for (const PrefabVoxel& v : limb.skinVoxels) live.insert(key(v.x, v.y, v.z));
    else
      for (const DebrisVoxel& v : limb.voxels) live.insert(key(v.x, v.y, v.z));
    static const int kN[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                 {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    uint32_t n = 0;
    auto tally = [&](int x, int y, int z) {
      int open = 0;
      for (const auto& d : kN)
        if (!live.count(key(x + d[0], y + d[1], z + d[2]))) open++;
      if (open >= minOpen) n++;
    };
    if (fine)
      for (const PrefabVoxel& v : limb.skinVoxels) tally(v.x, v.y, v.z);
    else
      for (const DebrisVoxel& v : limb.voxels) tally(v.x, v.y, v.z);
    return n;
  }
  return 0;
}

uint32_t MobSystem::LimbBurningCount(uint64_t mobId, int limbIndex) const {
  for (const Mob& mob : mobs_)
    if (mob.id_ == mobId && limbIndex >= 0 && limbIndex < (int)mob.limbs_.size())
      return (uint32_t)mob.limbs_[limbIndex].burn.front.size();
  return 0;
}

uint32_t MobSystem::LimbMaterialCount(uint64_t mobId, int limbIndex,
                                      uint32_t mat) const {
  for (const Mob& mob : mobs_) {
    if (mob.id_ != mobId || limbIndex < 0 || limbIndex >= (int)mob.limbs_.size())
      continue;
    const MobLimb& l = mob.limbs_[limbIndex];
    uint32_t n = 0;
    if (l.HasFineSkin()) {
      for (const PrefabVoxel& v : l.skinVoxels)
        if ((v.material & 0xFFFu) == (mat & 0xFFFu)) n++;
    } else {
      for (const DebrisVoxel& v : l.voxels)
        if ((v.payload & 0xFFFu) == (mat & 0xFFFu)) n++;
    }
    return n;
  }
  return 0;
}

uint32_t MobSystem::LimbVoxelsAtSpawn(uint64_t mobId, int limbIndex) const {
  for (const Mob& mob : mobs_)
    if (mob.id_ == mobId && limbIndex >= 0 && limbIndex < (int)mob.limbs_.size())
      return mob.limbs_[limbIndex].voxelsAtSpawn;
  return 0;
}

Vec3 MobSystem::LimbVoxelPos(uint64_t mobId, int limbIndex, uint32_t n) const {
  for (const Mob& mob : mobs_) {
    if (mob.id_ != mobId) continue;
    if (limbIndex < 0 || limbIndex >= (int)mob.limbs_.size()) break;
    const MobLimb& limb = mob.limbs_[limbIndex];
    if (limb.voxels.empty()) return limb.xf.pos;
    const DebrisVoxel& v = limb.voxels[n % (uint32_t)limb.voxels.size()];
    // Reading limb.voxels, which are COLLIDER units.
    const float inv =
        1.0f / (float)std::max(1u, defs_[mob.defIndex_].physScale);
    Vec3 c{((float)v.x + 0.5f) * inv, ((float)v.y + 0.5f) * inv,
           ((float)v.z + 0.5f) * inv};
    Quat q{limb.xf.quat[0], limb.xf.quat[1], limb.xf.quat[2], limb.xf.quat[3]};
    return limb.xf.pos + Rotate(q, c);
  }
  return Vec3{};
}

uint32_t MobSystem::LimbBodyCount() const {
  uint32_t n = 0;
  for (const Mob& mob : mobs_) n += mob.LimbBodyCount();
  return n;
}

uint32_t Mob::LimbBodyCount() const {
  uint32_t n = 0;
  for (const MobLimb& limb : limbs_)
    if (limb.body) n++;
  return n;
}

// ---- persistence (entities.sve section 'MOBS') ------------------------------

void MobSystem::SaveState(std::vector<uint8_t>& out) const {
  ByteWriter w{out};
  uint32_t count = 0;
  for (const Mob& m : mobs_)
    if (m.alive_ && m.defIndex_ >= 0 && m.defIndex_ < (int)defs_.size()) count++;
  w.U32(count);
  for (const Mob& m : mobs_) {
    if (!(m.alive_ && m.defIndex_ >= 0 && m.defIndex_ < (int)defs_.size()))
      continue;  // dead mobs are debris already (see mob.h)
    w.Str(defs_[m.defIndex_].name);
    w.Pod(m.origin_);
    w.F32(m.heading_);
    w.F32(m.bodyY_);
    w.U32((uint32_t)m.limbs_.size());
    for (const MobLimb& L : m.limbs_) {
      w.U32(L.body ? 1u : 0u);  // 0 = severed (sever state IS this flag)
      w.F32(L.hp);
      // The rig offsets travel because a carve SHIFTS them (ReskinLimbMicro):
      // restoring def values under carved art would slide the wound.
      w.Pod(L.restOffset);
      w.Pod(L.anchorRoot);
      w.Pod(L.anchorLimb);
      w.Pod(L.xf);
      w.Pod(L.size);
      w.PodVec(L.voxels);
      w.PodVec(L.skinVoxels);
    }
  }
}

bool MobSystem::LoadState(const uint8_t* data, size_t len, uint32_t version) {
  if (version != kSaveVersion) {
    std::printf("mob: unknown MOBS section version %u\n", version);
    return false;
  }
  ByteReader r{data, len};
  uint32_t count = 0;
  r.U32(count);
  for (uint32_t mi = 0; mi < count && r.ok; mi++) {
    std::string defName;
    Vec3 origin{};
    float heading = 0, bodyY = 0;
    uint32_t nLimbs = 0;
    r.Str(defName);
    r.Pod(origin);
    r.F32(heading);
    r.F32(bodyY);
    r.U32(nLimbs);
    struct LimbState {
      uint32_t alive = 1;
      float hp = 0;
      Vec3 restOffset{}, anchorRoot{}, anchorLimb{};
      BodyTransform xf{};
      IVec3 size{};
      std::vector<DebrisVoxel> voxels;
      std::vector<PrefabVoxel> skinVoxels;
    };
    std::vector<LimbState> ls(nLimbs);
    for (LimbState& s : ls) {
      r.U32(s.alive);
      r.F32(s.hp);
      r.Pod(s.restOffset);
      r.Pod(s.anchorRoot);
      r.Pod(s.anchorLimb);
      r.Pod(s.xf);
      r.Pod(s.size);
      r.PodVec(s.voxels);
      r.PodVec(s.skinVoxels);
    }
    if (!r.ok) break;

    // Resolve the def BY NAME: index order is whatever the directory listing
    // was the day the save was written. A missing def skips the mob (the save
    // stays loadable when a creature is retired) — loudly, so retired art
    // silently eating saved mobs cannot masquerade as a load bug.
    int defIndex = -1;
    for (size_t d = 0; d < defs_.size(); d++)
      if (defs_[d].name == defName) defIndex = (int)d;
    if (defIndex < 0) {
      std::printf("mob: saved def '%s' no longer exists; skipping\n",
                  defName.c_str());
      continue;
    }
    // Spawn() first: it derives everything the save deliberately does not
    // carry (anim state, joints, rest sole, flipbooks, gore profile) from the
    // def, exactly as a fresh mob would. The saved damage overlays that.
    uint64_t id = Spawn(defIndex, {ifloor(origin.x), ifloor(origin.y),
                                   ifloor(origin.z)});
    if (id == 0) {
      std::printf("mob: could not respawn saved '%s' (limit or physics)\n",
                  defName.c_str());
      continue;
    }
    Mob& m = mobs_.back();
    m.origin_ = origin;
    m.heading_ = m.desiredHeading_ = heading;
    m.bodyY_ = bodyY;
    m.anim_.lastPos = origin;
    const MobDef& def = defs_[defIndex];
    const uint32_t nApply = std::min<uint32_t>(nLimbs, (uint32_t)m.limbs_.size());

    // Carve state first, on limbs that are still attached: a lattice that
    // differs from the def's means the limb was carved, so the saved lattice
    // (and the rig offsets the carve shifted) replace the authored ones, the
    // brick is re-derived, and the Jolt body is rebuilt to the carved shape.
    for (uint32_t i = 0; i < nApply; i++) {
      if (!ls[i].alive) continue;
      MobLimb& L = m.limbs_[i];
      L.hp = ls[i].hp;
      const bool differs = ls[i].voxels.size() != L.voxels.size() ||
                           ls[i].skinVoxels.size() != L.skinVoxels.size();
      if (!differs || ls[i].voxels.empty()) continue;
      L.voxels = std::move(ls[i].voxels);
      L.skinVoxels = std::move(ls[i].skinVoxels);
      L.size = ls[i].size;
      L.restOffset = ls[i].restOffset;
      L.anchorRoot = ls[i].anchorRoot;
      L.anchorLimb = ls[i].anchorLimb;
      if (L.microModel >= 0)
        m.ReskinLimbMicro(L, m.SkinScaleOf(L), m.PhysScaleOf(L));
      m.RebuildLimbBody((int)i);
    }
    // Severs second: DetachLimb recurses into children, and doing it after
    // the carve pass means it never operates on a limb the loop still needs.
    // adopt=false — the severed piece is not re-created here, it already
    // travels in the 'DBRS' section as the debris it became.
    for (uint32_t i = 0; i < nApply; i++)
      if (!ls[i].alive && m.limbs_[i].body) m.DetachLimb((int)i, false);
  }
  instancesDirty_ = true;
  return r.ok;
}

// ============================================================================
// Holding an item — THE ENTITY<->SLOT SYNC SEAM, and deliberately the only
// one. Equipping BORROWS A RIG SLOT: the item's own geometry fills a real
// MobLimb parented to the socket's limb, so while worn it is a rig part in
// every respect — animated, rendered, severable with the arm that holds it,
// droppable as debris, carvable per voxel. Nothing downstream needs an "is
// this an item" branch, which is precisely why the item is not welded on as a
// special case.
//
// On the BASE class so any creature can hold a weapon: the player equips
// through exactly this path, and a mob with a sword is one EquipItem call —
// the mob-combat scaffolding the class exists for.
//
// Placement composes SOCKET x GRIP, forward: the rig says where the fist
// closes (MobDef::sockets), the item says how it sits in that fist
// (ItemDef::grip). Pass nullptr to unequip.
// ============================================================================

bool Mob::EquipItem(const ItemDef* item, const char* context) {
  if (!def_ || !phys_) return false;

  // Unequip first, always — including on a re-equip, so swapping weapons goes
  // through exactly one code path instead of a "replace in place" variant that
  // would have to duplicate the joint and body teardown.
  //
  // The weapon is NO LONGER GUARANTEED TO BE LAST: worn shells share the
  // appended tail with it, so an armoured character who draws a sword and then
  // puts on a hood has the blade in the middle. RemoveAppendedSlots owns that
  // (mob.h); this used to be a `pop_back` and the assumption behind it is the
  // one thing armour genuinely broke.
  if (heldSlot_ >= 0) {
    RemoveAppendedSlots(heldSlot_, 1);
    heldSlot_ = -1;
    heldItem_.clear();
    heldPartIndex_ = -1;
    heldPart_.clear();
    MarkInstancesDirty();
  }
  if (!item) return true;
  if (limbs_.empty()) return false;  // no rig yet (unspawned): nothing to hold

  const int si = def_->FindSocket(context);
  if (si < 0) return false;              // no such socket: loud, per avatar.h
  const MobSocketDef& sock = def_->sockets[si];
  const ItemGrip* grip = item->Grip(context);
  if (!grip) return false;               // item cannot be held this way
  if (sock.partIndex < 0 || sock.partIndex >= (int)limbs_.size()) return false;
  if (!LimbAlive(sock.partIndex)) return false;   // no hand, no grip

  // ---- the borrowed slot --------------------------------------------------
  const int slot = (int)limbs_.size();
  limbs_.push_back(MobLimb{});
  limbDefs_.push_back(MobLimbDef{});
  skel_.parts.push_back(AnimPart{});
  hidden_.push_back(0);

  MobLimbDef& ld = limbDefs_[slot];
  ld.name = "item:" + item->name;
  ld.parent = sock.part;
  ld.joint = Physics::JointType::Fixed;   // a grip does not hinge
  ld.hp = item->hp;
  ld.severable = item->severable;
  ld.vital = false;                       // losing your sword is not fatal
  ld.tag = "item";
  ld.severImpactSpeed = item->severImpactSpeed;
  ld.hasSpring = item->hasSpring;
  ld.spring = item->spring;
  ld.hasEdge = item->hasEdge;
  ld.edgeFrom = item->edgeFrom;
  ld.edgeTo = item->edgeTo;
  ld.edgeHalfWidth = item->edgeHalfWidth;
  ld.microModel = item->microModel;

  AnimPart& ap = skel_.parts[slot];
  ap.name = ld.name;
  ap.parent = sock.partIndex;
  ap.tag = ld.tag;
  ap.hasSpring = ld.hasSpring;
  ap.spring = ld.spring;

  // ---- SOCKET x GRIP, composed forward ------------------------------------
  //
  // The socket is a point in the HAND's own frame; the grip is how the item
  // sits once placed there. Translation applies BEFORE rotation (item.h), so
  // the item's local offset is rotated into the socket frame rather than added
  // after it — getting that order backwards puts the pommel where the tip
  // should be and looks like a bad grip constant.
  //
  // NOT the inverse form (Inverse(grip) x socket) VR rigs use: that solves for
  // a hand pose given a fixed grip, which is not the question here.
  const MobLimb& hand = limbs_[sock.partIndex];
  const Quat q = QuatMul(sock.rotation, grip->rotation);
  ap.rest.rot = q;
  // The slot's rest position, relative to the hand part, is the socket offset
  // measured from the hand's own model corner.
  const Vec3 socketLocal = sock.offset - hand.restOffset;

  // ---- put the HILT BOX on the socket -------------------------------------
  //
  // The socket is the centre of the hand's authored limb box; the hilt is the
  // centre of the item's authored hilt box (item.h ItemHilt). Aligning the two
  // is the whole placement, and both sides come from the LIMB/ART definitions
  // rather than from a collider or a hand-tuned constant — so re-authoring
  // either one keeps the sword in the fist instead of silently sliding it out.
  //
  // gripLocal is the vector, IN THE ITEM'S OWN FRAME, from the item's origin
  // to the point the fist closes on. With a hilt box that is the hilt centre
  // (plus any residual nudge); without one it degrades to the bare translation.
  const Vec3 gripLocal =
      item->hilt.has ? item->hilt.center - grip->translation
                     : (grip->translation * -1.0f);
  // REST.POS IS THE ANCHOR, exactly as it is for every other part.
  //
  // This is the convention the whole rig runs on and the item must not be the
  // exception: AnimFlatten chains rest.pos parent-to-child, the drive loop
  // recovers the body corner with anchorW - Rotate(rot, anchorLimb), and
  // WeaponEdge/DetachLimb go the other way with xf.pos + Rotate(q, anchorLimb).
  // Placing the item's ORIGIN here instead silently redefines anchorLimb for
  // this one part and every one of those call sites then disagrees with it by
  // the grip vector, rotated by the animated hand.
  ap.rest.pos = socketLocal;
  ap.anchorLocal = sock.offset;

  MobLimb& p = limbs_[slot];
  p.hp = item->hp;
  p.size = item->size;
  p.microModel = item->microModel;
  const float inv = 1.0f / (float)(item->scale ? item->scale : 1);
  // restOffset MEANS THE MODEL'S MIN CORNER, in the part's own frame — the
  // contract every other part keeps, and what the kinematic drive loop
  // assumes when it does pos = anchorW - Rotate(rot, anchorLimb).
  p.restOffset = Vec3{(float)item->offset.x, (float)item->offset.y,
                      (float)item->offset.z} * inv;
  // anchorLimb: the anchor measured from this part's own min corner, in the
  // part's own frame — the identical relationship anchor - restOffset states
  // for a limb. Here the anchor sits gripLocal from the item's ORIGIN, and
  // restOffset is that origin's offset to the corner, so the two compose.
  // Set BEFORE the body is created, because the initial placement below runs
  // the same two steps the drive loop does and needs this term.
  p.anchorLimb = gripLocal - p.restOffset;
  p.voxels.reserve(item->voxels.size());
  for (const PrefabVoxel& v : item->voxels) {
    uint32_t variant = ((uint32_t)(v.x * 7 + v.y * 13 + v.z * 29)) % 3u;
    // `color` rides along, as it does for a body limb and for a worn shell.
    // It used to be dropped here, which was invisible only because the one
    // item in the game is unpainted — a painted weapon would have rendered in
    // its bare material colours with nothing to say why.
    p.voxels.push_back({(int8_t)v.x, (int8_t)v.y, (int8_t)v.z, v.color,
                        (uint16_t)(v.material | (variant << 12))});
  }

  // Body at the composed pose. The body sits at the item's MIN CORNER, not at
  // its anchor — the same place BuildRig puts a limb's body.
  BodyTransform bxf{};
  const Quat handQ{hand.xf.quat[0], hand.xf.quat[1], hand.xf.quat[2],
                   hand.xf.quat[3]};
  const Quat worldQ = QuatMul(handQ, q);
  // Same two steps the drive loop takes, in the same order, so the pose on the
  // equip frame matches the pose on every frame after it: reach the anchor
  // through the hand, then back off to the corner the collider is built
  // around. Doing only the first step leaves the body one anchor-offset out
  // for one tick, which is a visible pop as the sword snaps into the fist.
  bxf.pos = hand.xf.pos + QuatRotate(handQ, ap.rest.pos) -
            QuatRotate(worldQ, p.anchorLimb);
  bxf.quat[0] = q.x; bxf.quat[1] = q.y; bxf.quat[2] = q.z; bxf.quat[3] = q.w;
  p.body = phys_->CreateDebrisBodyXf(p.voxels, bxf, DensityOf(), true, inv);
  if (p.body == 0) {
    limbs_.pop_back();
    skel_.parts.pop_back();
    limbDefs_.pop_back();
    hidden_.pop_back();
    return false;
  }
  phys_->SetBodyKinematic(p.body, true);
  // On the wearer's layer: your own sword must not shove you any more than
  // your own elbow may (meaningful for the avatar; an NPC's stays dynamic).
  if (AvatarLayer()) phys_->SetBodyAvatarLayer(p.body, true);

  // THE GRIP POINT IN THE BODY'S OWN FRAME.
  //
  // Everything above is in the item's AUTHORED frame, where the origin is the
  // model's corner. Jolt does not keep that frame: a compound shape is
  // RE-CENTRED on its centre of mass, so the body position GetTransform hands
  // back is the middle of the blade, not the corner. Ask the shape where its
  // own corner ended up and rebase the grip point on it, rather than modelling
  // Jolt's recentring here (which would be a second source of truth for it).
  {
    Vec3 clo, chi;
    if (phys_->GetLocalBounds(p.body, clo, chi)) gripBody_ = clo + gripLocal;
    else gripBody_ = gripLocal;
    // Place it once, right now, by the SAME expression the drive loop uses —
    // so the equip frame and every frame after it agree and the sword does not
    // pop into position on the first tick.
    const Vec3 socketW = hand.xf.pos + QuatRotate(handQ, ap.rest.pos);
    bxf.pos = socketW - QuatRotate(worldQ, gripBody_);
    float bq[4] = {q.x, q.y, q.z, q.w};
    phys_->MoveKinematicBody(p.body, bxf.pos, bq, 0.0f);
  }

  p.xf = bxf;
  p.anchorRoot = sock.offset;
  p.joint = phys_->CreateJoint(hand.body, p.body, JointDescFor(ld, p.xf.pos));

  // The new body must not collide with the rest of this creature, exactly as
  // BuildRig arranges for the limbs — a sword resting against the thigh would
  // otherwise fight the solver every tick.
  {
    std::vector<uint64_t> handles;
    for (const MobLimb& q2 : limbs_)
      if (q2.body) handles.push_back(q2.body);
    phys_->DisableCollisionsAmong(handles);
  }

  anim_.partAlive.resize(skel_.parts.size(), 1);
  anim_.springs.resize(skel_.parts.size(), SpringState{});
  heldSlot_ = slot;
  heldItem_ = item->name;
  // The weapon-arm IK derives the arm from the held part's parent, so these
  // stay in step with the slot rather than being a second source of truth.
  heldPartIndex_ = slot;
  heldPart_ = ld.name;
  // A weapon lattice is the ITEM's, at the ITEM's scale — not the wearer's
  // skin. Recorded so every scale-taking operation on this slot (burn, carve,
  // re-skin, drop) converts by the right divisor; before shells existed this
  // happened to agree with the def on every rig in the repo, which is exactly
  // the kind of agreement that stops being true without anybody noticing.
  p.ownSkinScale = item->scale ? item->scale : 1u;
  p.ownPhysScale = p.ownSkinScale;
  MarkInstancesDirty();
  return true;
}

// ============================================================================
// THE APPENDED TAIL — worn shells and the held item
//
// Slots at or past `baseLimbs_` are not part of the authored rig. They are
// appended by EquipItem (one) and WearItem (one per ItemCover), and they are
// the only slots that may be removed from a live rig. See the long note on
// RemoveAppendedSlots in mob.h for why removal ERASES rather than rebuilds.
// ============================================================================

bool Mob::AppendedInvariantHolds() const {
  const size_t n = limbs_.size();
  return skel_.parts.size() == n && limbDefs_.size() == n &&
         hidden_.size() == n && anim_.partAlive.size() == n &&
         anim_.springs.size() == n && (int)n >= baseLimbs_;
}

void Mob::RemoveAppendedSlots(int first, int count) {
  if (count <= 0 || !phys_) return;
  const int n = (int)limbs_.size();
  // Refuse anything that is not wholly inside the appended tail. A caller that
  // gets this wrong would be erasing an authored limb out from under every
  // part index in the rig, so it fails loudly rather than corrupting quietly.
  if (first < baseLimbs_ || first + count > n) {
    std::printf(
        "mob: RemoveAppendedSlots(%d,%d) is outside the appended tail "
        "[%d,%d) — refused\n",
        first, count, baseLimbs_, n);
    return;
  }

  // Physics first, while the handles are still reachable.
  for (int i = first; i < first + count; i++) {
    MobLimb& L = limbs_[i];
    if (L.joint) {
      phys_->DestroyJoint(L.joint);
      L.joint = 0;
    }
    if (L.body) {
      phys_->RemoveBody(L.body);
      L.body = 0;
    }
    // The brick is this slot's OWN copy-on-write clone the moment anything
    // damaged it, and once the slot is gone nothing downstream will ever free
    // it. ReleaseLimbMicro no-ops on an undamaged slot (shared model) and on a
    // severed one (the brick went with the debris).
    ReleaseLimbMicro(L);
    DropBurnIndex(L.burn);
  }

  auto cut = [&](auto& v) {
    if ((int)v.size() >= first + count)
      v.erase(v.begin() + first, v.begin() + first + count);
  };
  cut(limbs_);
  cut(skel_.parts);
  cut(limbDefs_);
  cut(hidden_);
  cut(anim_.partAlive);
  cut(anim_.springs);
  // model/local are rebuilt from the skeleton every tick, but truncate them
  // too: between here and the next PreTick something could read a stale entry
  // past the end of the rig, and a size mismatch is the cheapest thing to
  // avoid and the most annoying to diagnose.
  cut(anim_.model);
  cut(anim_.local);

  // ---- fix up every index that referred past the hole ---------------------
  // -1 means "that slot IS gone". No parent index needs fixing: an appended
  // slot is never a parent (mob.h), so every surviving parent points below
  // `first`. Checked rather than assumed, because the day somebody straps a
  // pauldron to a pauldron this is the assertion that says so.
  auto shift = [&](int s) {
    if (s < first) return s;
    if (s < first + count) return -1;
    return s - count;
  };
  for (size_t i = 0; i < skel_.parts.size(); i++) {
    const int par = skel_.parts[i].parent;
    if (par >= first)
      std::printf(
          "mob: part \"%s\" was parented to an appended slot (%d) that has "
          "just been removed — the rig is now inconsistent\n",
          skel_.parts[i].name.c_str(), par);
  }

  if (heldSlot_ >= 0) {
    heldSlot_ = shift(heldSlot_);
    heldPartIndex_ = heldSlot_;
    if (heldSlot_ < 0) {
      heldItem_.clear();
      heldPart_.clear();
    }
  }
  for (size_t w = 0; w < worn_.size();) {
    WornPiece& p = worn_[w];
    for (size_t k = 0; k < p.slots.size();) {
      const int s = shift(p.slots[k]);
      if (s < 0) {
        // ALL THREE, or the piece's own parallel arrays drift: `index` is the
        // occlusion cache and `cover` says which ItemCover entry this shell
        // came from, which is what CaptureWorn indexes the damage blob by.
        // Dropping only `slots` would put a sleeve's holes on the hood.
        p.slots.erase(p.slots.begin() + k);
        if (k < p.index.size()) p.index.erase(p.index.begin() + k);
        if (k < p.cover.size()) p.cover.erase(p.cover.begin() + k);
      } else {
        p.slots[k] = s;
        k++;
      }
    }
    if (p.slots.empty())
      worn_.erase(worn_.begin() + w);
    else
      w++;
  }

  if (!AppendedInvariantHolds())
    std::printf(
        "mob: appended vectors fell out of step after a removal "
        "(limbs %zu / parts %zu / defs %zu / hidden %zu / alive %zu)\n",
        limbs_.size(), skel_.parts.size(), limbDefs_.size(), hidden_.size(),
        anim_.partAlive.size());
  MarkInstancesDirty();
}

// ---- one shell of a worn piece ---------------------------------------------
//
// A SHELL RIDES ITS LIMB. Unlike the held item — which is a foreign object
// aligned hilt-to-socket and therefore needs its own placement block in
// SubmitPose — a shell shares the covered limb's anchor and rotation exactly.
// So its AnimPart rest transform is the IDENTITY, AnimFlatten puts it where
// the covered part goes, and the ordinary kinematic drive loop places it with
// no special case anywhere. The only authored number is where its own corner
// sits relative to the limb's.
int Mob::AppendWornShell(const ItemDef& item, const ItemCover& cover,
                         int bodyLimb, const std::string& partName) {
  if (!phys_ || bodyLimb < 0 || bodyLimb >= (int)limbs_.size()) return -1;
  if (cover.voxels.empty()) return -1;
  // A limb that has already come off cannot be covered. Not an error: an
  // armless mob putting on a robe simply wears the parts of it that have
  // somewhere to go.
  if (!limbs_[bodyLimb].body) return -1;

  const uint32_t itemScale = item.scale ? item.scale : 1u;

  // ---- FIT: RESAMPLE TO THE WEARER THIS PIECE IS ACTUALLY ON --------------
  //
  // A shell is authored against ONE mannequin — `cover.fitBox` records whose
  // limb, in world voxels. Anybody else's limb is a different size, and the
  // difference is per-axis: a goblin is not a small human, it is a wide short
  // one. So the lattice is resampled by the ratio of the two boxes and the
  // authored offset is carried along by the same rationals, which is what
  // makes "goblin helmets look right on anyone" CONTENT rather than a piece
  // of art per species.
  //
  // Computed as INTEGER RATIONALS in the item's own micro units, not as a
  // float scale: the resample indexes cells, and a float that lands a
  // hair either side of an integer would resample differently on two machines
  // for no reason anybody could see.
  //
  // Skipped entirely at ratio 1, which is the stock set on the stock human and
  // therefore the overwhelmingly common case. It costs a comparison, and it
  // matters for more than speed: at ratio 1 the shell keeps sharing the def's
  // brick until something damages it, exactly as a body limb does.
  IVec3 fitSrc{cover.size.x, cover.size.y, cover.size.z};
  IVec3 fitDst = fitSrc;
  Vec3 fitOffset = cover.offset;
  if (cover.fitBox.x > 0.5f / (float)itemScale &&
      cover.fitBox.y > 0.5f / (float)itemScale &&
      cover.fitBox.z > 0.5f / (float)itemScale) {
    // The wearer's own limb box, in the item's micro units. `limb.size` is in
    // COLLIDER units, so it divides by that limb's phys scale to reach world
    // voxels before it multiplies back up — using the wrong divisor here is
    // the silent-joint-shift class of bug the skin/collider split exists to
    // make impossible.
    const MobLimb& hostLimb = limbs_[bodyLimb];
    const float hostInv = 1.0f / (float)std::max(1u, PhysScaleOf(hostLimb));
    const int wear[3] = {
        (int)std::lround((float)hostLimb.size.x * hostInv * (float)itemScale),
        (int)std::lround((float)hostLimb.size.y * hostInv * (float)itemScale),
        (int)std::lround((float)hostLimb.size.z * hostInv * (float)itemScale)};
    const int fit[3] = {
        (int)std::lround(cover.fitBox.x * (float)itemScale),
        (int)std::lround(cover.fitBox.y * (float)itemScale),
        (int)std::lround(cover.fitBox.z * (float)itemScale)};
    int dst[3] = {fitSrc.x, fitSrc.y, fitSrc.z};
    const int srcA[3] = {fitSrc.x, fitSrc.y, fitSrc.z};
    float off[3] = {cover.offset.x, cover.offset.y, cover.offset.z};
    bool any = false;
    for (int a = 0; a < 3; a++) {
      if (fit[a] <= 0 || wear[a] <= 0) continue;
      // BOUNDED, like every other emergent quantity here. A fitBox that
      // disagrees wildly with the wearer is a content error, and the honest
      // failure is a garment that fits badly rather than one that allocates a
      // lattice the size of a chunk.
      int64_t d = ((int64_t)srcA[a] * wear[a]) / fit[a];
      d = std::max<int64_t>(srcA[a] / 4, std::min<int64_t>(srcA[a] * 4, d));
      dst[a] = (int)std::max<int64_t>(1, d);
      off[a] = off[a] * (float)wear[a] / (float)fit[a];
      if (dst[a] != srcA[a]) any = true;
    }
    if (any) {
      fitDst = IVec3{dst[0], dst[1], dst[2]};
      fitOffset = Vec3{off[0], off[1], off[2]};
    }
  }
  const bool resampled = fitDst.x != fitSrc.x || fitDst.y != fitSrc.y ||
                         fitDst.z != fitSrc.z;
  std::vector<PrefabVoxel> fitted;
  if (resampled) fitted = ResampleLattice(cover.voxels, fitSrc, fitDst);
  const std::vector<PrefabVoxel>& shellVox =
      resampled ? fitted : cover.voxels;
  const IVec3 shellSize = resampled ? fitDst : cover.size;
  // The shell's COLLIDER pitch is the wearer's, not the item's: a Jolt body
  // built at the art's own resolution would be an order of magnitude more
  // boxes than the limb it wraps, for a shape the physics never needs that
  // finely. Data still flows skin -> collider and never back, exactly as
  // BuildRig arranges for a body limb, so the two lattices cannot drift.
  const uint32_t physScale =
      std::min(itemScale, std::max(1u, def_ ? def_->physScale : 1u));
  const uint32_t ratio = std::max(1u, itemScale / std::max(1u, physScale));

  const int slot = (int)limbs_.size();
  limbs_.push_back(MobLimb{});
  limbDefs_.push_back(MobLimbDef{});
  skel_.parts.push_back(AnimPart{});
  hidden_.push_back(0);

  MobLimbDef& ld = limbDefs_[slot];
  // Named for what it is and what it covers, so a printf about a rig part is
  // legible and so DetachLimb's parent-by-name recursion finds it.
  ld.name = "worn:" + item.name + ":" + partName;
  ld.parent = limbDefs_[bodyLimb].name;
  ld.joint = Physics::JointType::Fixed;   // a strap does not hinge
  ld.hp = cover.hp;
  ld.severable = true;                    // a cut strap drops the pauldron
  ld.vital = false;                       // losing your hood is not fatal
  // "worn" is deliberately NOT one of the figure tags (head/spine/arm/...), so
  // main.cpp's BodySlotFor returns -1 for it and the HUD body diagram keeps
  // counting the CREATURE's injuries rather than its wardrobe's.
  ld.tag = "worn";
  ld.microModel = cover.microModel;

  AnimPart& ap = skel_.parts[slot];
  ap.name = ld.name;
  ap.parent = bodyLimb;
  ap.tag = ld.tag;
  ap.rest = Transform{};                          // identity: ride the limb
  ap.anchorLocal = skel_.parts[bodyLimb].anchorLocal;

  const MobLimb& host = limbs_[bodyLimb];
  MobLimb& p = limbs_[slot];
  p.hp = cover.hp;
  p.microModel = cover.microModel;
  p.ownSkinScale = itemScale;
  p.ownPhysScale = physScale;
  // restOffset MEANS THE MODEL'S MIN CORNER in the creature's rest frame —
  // the contract every other part keeps and what the drive loop assumes when
  // it recovers the corner with anchorW - Rotate(rot, anchorLimb).
  p.restOffset = host.restOffset + fitOffset;
  // Same anchor as the limb it wraps, which is what `ap.rest` being the
  // identity means geometrically. anchorLimb is that anchor measured from this
  // part's own corner — the identical `anchor - restOffset` relationship a
  // body limb states.
  p.anchorRoot = host.anchorRoot;
  p.anchorLimb = host.anchorRoot - p.restOffset;

  // Skin, then the collider derived from it by the same majority-fill the
  // body path uses. `color` rides along untouched: it is ART (the .col layers
  // are where a black robe gets its black), independent of the material, and
  // only the material reaches the collider.
  if (ratio > 1) {
    p.skinVoxels.reserve(shellVox.size());
    for (const PrefabVoxel& v : shellVox) {
      const uint32_t variant =
          ((uint32_t)(v.x * 7 + v.y * 13 + v.z * 29)) % 3u;
      p.skinVoxels.push_back(
          {v.x, v.y, v.z, (uint16_t)(v.material | (variant << 12)), v.color});
    }
    bool overflow = false;
    p.voxels = DownsampleSkin(p.skinVoxels, ratio, &overflow);
    if (overflow)
      std::printf(
          "mob: shell \"%s\" exceeded the collider's +-127 bound; part of it "
          "was dropped from the collider (item scale too fine)\n",
          ld.name.c_str());
    p.size = IVec3{(shellSize.x + (int)ratio - 1) / (int)ratio,
                   (shellSize.y + (int)ratio - 1) / (int)ratio,
                   (shellSize.z + (int)ratio - 1) / (int)ratio};
  } else {
    p.size = shellSize;
    p.voxels.reserve(shellVox.size());
    for (const PrefabVoxel& v : shellVox) {
      const uint32_t variant =
          ((uint32_t)(v.x * 7 + v.y * 13 + v.z * 29)) % 3u;
      p.voxels.push_back({(int8_t)v.x, (int8_t)v.y, (int8_t)v.z, v.color,
                          (uint16_t)(v.material | (variant << 12))});
    }
  }
  // A RESAMPLED SHELL NEEDS ITS OWN BRICK. The def's brick is the authored
  // lattice at the authored size, shared by every instance of the item;
  // drawing a resized shell through it would render the mannequin's helmet on
  // the goblin's head. Packed into the SAME copy-on-write pool a damaged limb
  // clones into, and marked owned (`carved`) so ReleaseLimbMicro returns it on
  // unwear -- otherwise every equip would leak a brick.
  //
  // Pool exhaustion DEGRADES rather than crashes: the shell keeps its physics,
  // its hp and its occlusion and simply draws at the authored size. That is
  // the null-check lesson at ensureOwnedBrick, in the one other place a brick
  // is allocated outside the loader.
  if (resampled && MicroSet() && itemScale > 1) {
    std::string log;
    const int own = MicroBodyPack(*MicroSet(), shellVox, shellSize, itemScale,
                                  "fit/" + ld.name, log);
    if (own >= 0) {
      p.microModel = own;
      ld.microModel = own;
      p.carved = true;   // "this slot owns its brick" -- the flag's real sense
    } else if (!log.empty()) {
      std::printf("%s", log.c_str());
    }
  }
  p.voxelsAtSpawn =
      (uint32_t)(p.HasFineSkin() ? p.skinVoxels.size() : p.voxels.size());
  p.voxelsCharged = p.voxelsAtSpawn;

  // Placed by the SAME two steps the drive loop takes, in the same order, so
  // the wear frame and every frame after it agree and the shell does not pop
  // into position: reach the anchor through the host, then back off to the
  // corner the collider is built around.
  const Quat hostQ{host.xf.quat[0], host.xf.quat[1], host.xf.quat[2],
                   host.xf.quat[3]};
  BodyTransform bxf{};
  bxf.pos = host.xf.pos + Rotate(hostQ, host.anchorLimb) -
            Rotate(hostQ, p.anchorLimb);
  bxf.quat[0] = hostQ.x; bxf.quat[1] = hostQ.y;
  bxf.quat[2] = hostQ.z; bxf.quat[3] = hostQ.w;
  p.body = phys_->CreateDebrisBodyXf(p.voxels, bxf, DensityOf(), true,
                                     1.0f / (float)std::max(1u, physScale));
  if (p.body == 0) {
    // The slot is still LAST at this point, so unwinding it is a pop.
    limbs_.pop_back();
    skel_.parts.pop_back();
    limbDefs_.pop_back();
    hidden_.pop_back();
    return -1;
  }
  phys_->SetBodyKinematic(p.body, true);
  if (AvatarLayer()) phys_->SetBodyAvatarLayer(p.body, true);
  p.xf = bxf;
  p.joint = phys_->CreateJoint(host.body, p.body, JointDescFor(ld, p.xf.pos));

  anim_.partAlive.resize(skel_.parts.size(), 1);
  anim_.springs.resize(skel_.parts.size(), SpringState{});
  return slot;
}

bool Mob::WearItem(const ItemDef* item, int equipSlot,
                   const WornDamage* damage) {
  if (!def_ || !phys_ || !item) return false;
  if (limbs_.empty()) return false;   // unspawned: nothing to put it on
  if (item->cover.empty()) return false;

  // Take off whatever was in this slot first, always — including on a
  // re-wear, so swapping a hood for a helm goes through one path rather than a
  // "replace in place" variant that would duplicate the teardown.
  UnwearItem(equipSlot);

  WornPiece piece;
  piece.equipSlot = equipSlot;
  piece.item = item->name;
  for (const ItemCover& cv : item->cover) {
    // BY LIMB NAME. A wearer with no such part simply skips that shell —
    // that is what makes one authored helmet fit any humanoid rig, and it is
    // deliberately not an error.
    int host = -1;
    for (int i = 0; i < baseLimbs_ && i < (int)limbDefs_.size(); i++)
      if (limbDefs_[i].name == cv.part) { host = i; break; }
    if (host < 0) continue;
    const int slot = AppendWornShell(*item, cv, host, cv.part);
    if (slot < 0) continue;
    // PUT THE DAMAGE BACK. Indexed by the piece's own COVER ORDER rather than
    // by the appended slot, because a cover entry that found no limb on this
    // wearer is skipped and the two lists would then be out of step — a
    // one-armed goblin would get its sleeve's holes on its hood.
    const size_t coverIndex = (size_t)(&cv - item->cover.data());
    if (damage && coverIndex < damage->shells.size() &&
        !damage->shells[coverIndex].Empty())
      RestoreShellLattice(slot, damage->shells[coverIndex]);
    piece.slots.push_back(slot);
    piece.index.push_back(WornPiece::ShellIndex{});
    piece.cover.push_back((int)coverIndex);
  }
  if (piece.slots.empty()) return false;   // nothing of it found a home

  // Nothing on this creature collides with anything else on it: a pauldron
  // resting against a helmet would fight the solver every tick, exactly as a
  // sword resting against a thigh would.
  {
    std::vector<uint64_t> handles;
    for (const MobLimb& l : limbs_)
      if (l.body) handles.push_back(l.body);
    phys_->DisableCollisionsAmong(handles);
  }

  worn_.push_back(std::move(piece));
  if (!AppendedInvariantHolds())
    std::printf("mob: appended vectors fell out of step after WearItem\n");
  MarkInstancesDirty();
  return true;
}

bool Mob::UnwearItem(int equipSlot) {
  for (size_t w = 0; w < worn_.size(); w++) {
    if (worn_[w].equipSlot != equipSlot) continue;
    // The slots of one piece are contiguous ONLY when nothing was appended
    // between them, which is true today (WearItem appends them in one loop)
    // but is not something to rely on. Removed one at a time, highest first,
    // so each removal cannot renumber a slot this loop has not reached yet.
    std::vector<int> slots = worn_[w].slots;
    std::sort(slots.begin(), slots.end(), std::greater<int>());
    for (int s : slots) RemoveAppendedSlots(s, 1);
    // RemoveAppendedSlots drops a WornPiece once its last slot is gone, so the
    // entry is already off `worn_` by here — but only if every slot really was
    // one of its own. Sweep defensively rather than indexing back into a
    // vector another call may have reshaped.
    for (size_t k = 0; k < worn_.size(); k++)
      if (worn_[k].equipSlot == equipSlot) {
        worn_.erase(worn_.begin() + k);
        break;
      }
    MarkInstancesDirty();
    return true;
  }
  return false;
}

void Mob::RestoreShellLattice(int slot, const WornShellDamage& d) {
  if (slot < 0 || slot >= (int)limbs_.size()) return;
  MobLimb& L = limbs_[slot];
  if (d.hp >= 0.0f) L.hp = d.hp;
  if (d.lattice.empty()) return;

  // The AUTHORITATIVE lattice, whichever it is: a shell with a separate skin
  // carries its damage there and re-derives the collider, one with a single
  // lattice carries it in the collider. Writing the derived side instead would
  // be undone by the next re-derive (phys/lattice.h's one-way rule), which is
  // a quiet way to lose exactly what this call exists to keep.
  const uint32_t skinS = SkinScaleOf(L), physS = PhysScaleOf(L);
  const uint32_t ratio = std::max(1u, skinS / std::max(1u, physS));
  if (ratio > 1) {
    L.skinVoxels.clear();
    L.skinVoxels.reserve(d.lattice.size());
    for (const PrefabVoxel& v : d.lattice) L.skinVoxels.push_back(v);
    bool overflow = false;
    L.voxels = DownsampleSkin(L.skinVoxels, ratio, &overflow);
  } else {
    L.skinVoxels.clear();
    L.voxels.clear();
    L.voxels.reserve(d.lattice.size());
    for (const PrefabVoxel& v : d.lattice)
      L.voxels.push_back({(int8_t)v.x, (int8_t)v.y, (int8_t)v.z, v.color,
                          v.material});
  }
  // `voxelsAtSpawn` is the DENOMINATOR damage fractions are measured against
  // and must stay the AUTHORED volume — resetting it to what is left would
  // make a half-destroyed piece read as pristine and take another full
  // piece's worth of damage to come apart.
  L.voxelsCharged =
      (uint32_t)(L.HasFineSkin() ? L.skinVoxels.size() : L.voxels.size());

  // Art, then collider, in that order: ReskinLimbMicro may shift the limb
  // origin and the body must be built from the voxels in their FINAL frame.
  if (L.microModel >= 0) ReskinLimbMicro(L, skinS, physS);
  RebuildLimbBody(slot);
  DropBurnIndex(L.burn);
  MarkInstancesDirty();
}

bool Mob::CaptureWorn(int equipSlot, WornDamage& out) const {
  for (const WornPiece& p : worn_) {
    if (p.equipSlot != equipSlot) continue;
    out.Clear();
    // Sized by the piece's COVER count, not by how many shells this wearer
    // ended up with, so the blob means the same thing on the next body.
    size_t n = 0;
    for (int ci : p.cover) n = std::max(n, (size_t)ci + 1);
    out.shells.resize(n);
    for (size_t k = 0; k < p.slots.size() && k < p.cover.size(); k++) {
      const int slot = p.slots[k];
      const int ci = p.cover[k];
      if (slot < 0 || slot >= (int)limbs_.size() || ci < 0) continue;
      const MobLimb& L = limbs_[slot];
      WornShellDamage& d = out.shells[(size_t)ci];
      // ONLY WHAT DIFFERS FROM THE AUTHORED PIECE. An undamaged shell must
      // produce an EMPTY entry, or every equip writes a full lattice into the
      // save and the file grows by the size of the wardrobe rather than by the
      // wounds. The authored values are right there in the rig's own limb def,
      // which is the copy this instance was built from.
      const float authoredHp =
          slot < (int)limbDefs_.size() ? limbDefs_[slot].hp : L.hp;
      if (L.hp != authoredHp) d.hp = L.hp;
      const size_t now =
          L.HasFineSkin() ? L.skinVoxels.size() : L.voxels.size();
      // CONDITION TRAVELS WITH THE BLOB, as two counts rather than as a
      // percentage. The lattice below is the exact record and is the thing the
      // armour mechanic actually reads; these are so the CHARACTER SCREEN can
      // say "62%" about a piece sitting in the pack, where there is no shell to
      // measure and re-deriving the denominator would mean re-running the fit
      // resample against a wearer who is not currently wearing it.
      //
      // Written for EVERY shell, including undamaged ones, so the ratio is
      // whole-piece rather than "the ratio over the shells that happened to be
      // hurt". WornShellDamage::Empty() ignores them when they agree, so an
      // untouched piece still costs nothing in the save.
      d.atSpawn = L.voxelsAtSpawn;
      d.live = (uint32_t)now;
      if (now == (size_t)L.voxelsAtSpawn) continue;   // nothing lost
      d.lattice.reserve(now);
      if (L.HasFineSkin()) {
        for (const PrefabVoxel& v : L.skinVoxels) d.lattice.push_back(v);
      } else {
        for (const DebrisVoxel& v : L.voxels)
          d.lattice.push_back(PrefabVoxel{(int16_t)v.x, (int16_t)v.y,
                                          (int16_t)v.z, v.payload, v.color});
      }
    }
    return true;
  }
  return false;
}

float Mob::WornCondition(int equipSlot) const {
  for (const WornPiece& p : worn_) {
    if (p.equipSlot != equipSlot) continue;
    // Volume-weighted, not a mean of per-shell fractions: a robe is a torso
    // shell and two sleeves, and averaging the fractions would let a sleeve
    // burnt away count as much as the body of the garment.
    uint64_t at0 = 0, now = 0;
    for (int slot : p.slots) {
      if (slot < 0 || slot >= (int)limbs_.size()) continue;
      const MobLimb& L = limbs_[slot];
      at0 += L.voxelsAtSpawn;
      now += L.HasFineSkin() ? L.skinVoxels.size() : L.voxels.size();
    }
    return at0 ? std::min(1.0f, (float)now / (float)at0) : 1.0f;
  }
  return 1.0f;
}

const std::string& Mob::WornItem(int equipSlot) const {
  static const std::string kNone;
  for (const WornPiece& p : worn_)
    if (p.equipSlot == equipSlot) return p.item;
  return kNone;
}

const std::vector<int>& Mob::WornSlotsAt(int pieceIndex) const {
  static const std::vector<int> kNone;
  return (pieceIndex >= 0 && pieceIndex < (int)worn_.size())
             ? worn_[pieceIndex].slots
             : kNone;
}

bool Mob::LimbHasShells(int bodyLimb) const {
  if (bodyLimb < 0) return false;
  for (const WornPiece& p : worn_)
    for (int s : p.slots)
      if (s >= 0 && s < (int)skel_.parts.size() &&
          skel_.parts[s].parent == bodyLimb)
        return true;
  return false;
}

// ---- the worn-occlusion probe ----------------------------------------------
//
// "Is the world actually touching this limb, or is it touching the robe over
// it?" The burn pass cannot answer that on its own: a shell's voxels are
// neither in the grid nor in the body limb's lattice, so fire lapping at a
// sleeve reads to the arm underneath as fire lapping at the arm.
//
// Answered geometrically, and it reports the shell's MATERIAL rather than a
// bool — which is what makes the mechanic emergent instead of a rule. The
// flesh's neighbour simply becomes "cloth" instead of "fire", cloth-over-flesh
// semantics fall out of the ordinary reaction table, and the moment the shell
// burns through, the probe returns 0 there and the skin is exposed. No
// integrity threshold, no armour value, nothing to tune.
uint32_t Mob::WornAlong(int bodyLimb, const Vec3& from, const Vec3& dir,
                        float dist) {
  if (worn_.empty() || bodyLimb < 0) return 0;
  for (WornPiece& piece : worn_) {
    for (size_t k = 0; k < piece.slots.size(); k++) {
      const int s = piece.slots[k];
      if (s < 0 || s >= (int)limbs_.size()) continue;
      if (skel_.parts[s].parent != bodyLimb) continue;
      MobLimb& shell = limbs_[s];
      if (!shell.body) continue;          // strap cut: it is not there any more
      const bool fine = shell.HasFineSkin();
      const size_t n = fine ? shell.skinVoxels.size() : shell.voxels.size();
      if (n == 0) continue;
      if (k >= piece.index.size()) continue;
      WornPiece::ShellIndex& ix = piece.index[k];

      // (Re)build the dense index whenever the lattice changed size. Carving
      // and burning both COMPACT the lattice, so the count is a sufficient
      // witness — and it is the same witness the save/restore path uses to
      // decide a limb was carved.
      if (ix.builtFor != n) {
        IVec3 mn{1 << 30, 1 << 30, 1 << 30}, mx{-(1 << 30), -(1 << 30),
                                                -(1 << 30)};
        for (size_t i = 0; i < n; i++) {
          const IVec3 q = fine ? IVec3{shell.skinVoxels[i].x,
                                       shell.skinVoxels[i].y,
                                       shell.skinVoxels[i].z}
                               : IVec3{shell.voxels[i].x, shell.voxels[i].y,
                                       shell.voxels[i].z};
          mn.x = std::min(mn.x, q.x); mn.y = std::min(mn.y, q.y);
          mn.z = std::min(mn.z, q.z);
          mx.x = std::max(mx.x, q.x); mx.y = std::max(mx.y, q.y);
          mx.z = std::max(mx.z, q.z);
        }
        const IVec3 dims{mx.x - mn.x + 1, mx.y - mn.y + 1, mx.z - mn.z + 1};
        const uint64_t cells = (uint64_t)dims.x * dims.y * dims.z;
        // Same refusal BuildBurnIndex makes, and for the same reason: a long
        // diagonal sliver would allocate a lot to index very little. A refused
        // shell simply does not occlude, which fails toward "the fire reaches
        // the skin" — the honest direction to fail in.
        if (cells > (1u << 20)) {
          ix.builtFor = n;
          ix.mat.clear();
          ix.dims = IVec3{0, 0, 0};
          continue;
        }
        ix.min = mn;
        ix.dims = dims;
        ix.mat.assign((size_t)cells, 0);
        for (size_t i = 0; i < n; i++) {
          const IVec3 q = fine ? IVec3{shell.skinVoxels[i].x,
                                       shell.skinVoxels[i].y,
                                       shell.skinVoxels[i].z}
                               : IVec3{shell.voxels[i].x, shell.voxels[i].y,
                                       shell.voxels[i].z};
          const uint32_t m = fine
                                 ? (uint32_t)(shell.skinVoxels[i].material &
                                              0xFFFu)
                                 : (uint32_t)(shell.voxels[i].payload & 0xFFFu);
          // A material-0 tombstone is an unflushed burn: the voxel is already
          // gone and must not go on occluding.
          if (m == 0) continue;
          const size_t ci = (size_t)(((q.z - mn.z) * (size_t)dims.y +
                                      (q.y - mn.y)) *
                                         (size_t)dims.x +
                                     (q.x - mn.x));
          ix.mat[ci] = (uint16_t)m;
        }
        ix.builtFor = n;
      }
      if (ix.mat.empty()) continue;

      // World -> the shell's own lattice. The pose is read from `shell.xf` as
      // the animation left it, NEVER re-read from Jolt: a live shell is
      // kinematic and re-posed every tick, and a mid-pass re-read would test
      // against a pose the rest of the burn pass was not computed against
      // (gotcha-live-limb-carve-pose).
      const Quat q{shell.xf.quat[0], shell.xf.quat[1], shell.xf.quat[2],
                   shell.xf.quat[3]};
      const float scale = (float)std::max(1u, SkinScaleOf(shell));
      Vec3 l = RotateInv(q, from - shell.xf.pos) * scale;
      // ONE transform for the whole march. A shell rides its limb, so the
      // rotation does not change along the segment: the lattice-space step is
      // the rotated direction, and moving 1/scale world voxels is one cell.
      const float dl = dir.len();
      const Vec3 stepL =
          dl > 1e-6f ? RotateInv(q, dir * (1.0f / dl)) : Vec3{0, 0, 0};
      // Bounded, like every other loop in this system: a caller asking for a
      // long ray gets a truncated one rather than an unbounded cost.
      const int steps =
          std::min(kWornMarchMax, std::max(1, (int)(dist * scale) + 1));
      for (int st = 0; st < steps; st++) {
        const int lx = ifloor(l.x) - ix.min.x, ly = ifloor(l.y) - ix.min.y,
                  lz = ifloor(l.z) - ix.min.z;
        if (lx >= 0 && ly >= 0 && lz >= 0 && lx < ix.dims.x &&
            ly < ix.dims.y && lz < ix.dims.z) {
          const uint32_t m =
              ix.mat[(size_t)(((size_t)lz * ix.dims.y + ly) * ix.dims.x + lx)];
          if (m) return m;
        }
        l += stepL;
      }
    }
  }
  return 0;
}

void Mob::SetWeaponPose(Vec3 handOffset, Vec3 bladeDir, Vec3 bladeUp,
                        float weight) {
  weaponHand_ = handOffset;
  // bladeDir/bladeUp are accepted but NOT applied to the held part: the blade
  // keeps its grip angle and only the ARM is driven (the weapon-arm block in
  // the driver's animation pass). Kept in the signature because the caller
  // computes them anyway, and a weapon that genuinely does re-aim in the hand
  // — a levelled spear, a raised shield — would want them.
  if (bladeDir.len() > 1e-4f) weaponDir_ = bladeDir.normalized();
  if (bladeUp.len() > 1e-4f) weaponUp_ = bladeUp.normalized();
  weaponWeight_ = weight < 0 ? 0 : (weight > 1 ? 1 : weight);
}

bool Mob::WeaponEdge(Vec3& outBase, Vec3& outTip, float& outHalfWidth) const {
  if (!def_ || heldPartIndex_ < 0) return false;
  if (heldPartIndex_ >= (int)limbDefs_.size()) return false;
  const MobLimbDef& ld = limbDefs_[heldPartIndex_];
  if (!ld.hasEdge) return false;
  if (!LimbAlive(heldPartIndex_)) return false;   // severed: nothing to cut with
  const MobLimb& p = limbs_[heldPartIndex_];
  if (!p.body) return false;
  // The part's body transform sits at the model's MIN CORNER, and the authored
  // edge is measured from that same corner — so the composition is direct,
  // with no anchor rebasing. Reading the LIVE transform is the point: the
  // hitbox is wherever the renderer just drew the blade.
  Quat q{p.xf.quat[0], p.xf.quat[1], p.xf.quat[2], p.xf.quat[3]};
  outBase = p.xf.pos + QuatRotate(q, ld.edgeFrom);
  outTip = p.xf.pos + QuatRotate(q, ld.edgeTo);
  outHalfWidth = ld.edgeHalfWidth;
  return true;
}

bool Mob::OwnsBody(uint64_t bodyHandle) const {
  if (!bodyHandle) return false;
  for (const MobLimb& p : limbs_)
    if (p.body == bodyHandle) return true;
  return false;
}
