#include "game/mob.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace {

// ---- minimal quaternion helpers (CPU gameplay floats: legal outside the
// hashed grid domain, see debris.h) -----------------------------------------
struct Quat {
  float x = 0, y = 0, z = 0, w = 1;
};

Quat AxisAngle(Vec3 axis, float angle) {
  float s = std::sin(angle * 0.5f);
  Vec3 a = axis.normalized();
  return {a.x * s, a.y * s, a.z * s, std::cos(angle * 0.5f)};
}

Quat Mul(const Quat& a, const Quat& b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
          a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Vec3 Rotate(const Quat& q, Vec3 v) {
  Vec3 u{q.x, q.y, q.z};
  Vec3 t = u.cross(v) * 2.0f;
  return v + t * q.w + u.cross(t);
}

Vec3 RotateInv(const Quat& q, Vec3 v) {
  return Rotate({-q.x, -q.y, -q.z, q.w}, v);
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

}  // namespace

// ---- sidecar + .vox pairing -------------------------------------------------

bool LoadMobDefs(const std::string& dir, const std::vector<MaterialDef>& mats,
                 std::vector<MobDef>& out, std::string& log) {
  out.clear();
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
    // DebrisVoxel is int8: each LIMB must fit in the byte range (§2.1)
    for (const PrefabModel& m : def.prefab.models)
      if (m.size.x > 120 || m.size.y > 120 || m.size.z > 120) {
        log += def.name + ": limb model \"" + m.name + "\" exceeds 120 voxels\n";
        ok = false;
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
    for (Limb& l : m.limbs)
      if (l.body) phys_->RemoveBody(l.body);
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

  for (size_t i = 0; i < def.limbs.size(); i++) {
    const MobLimbDef& ld = def.limbs[i];
    int mi = FindModel(def.prefab, ld.name);
    const PrefabModel& model = def.prefab.models[mi];
    Limb& limb = mob.limbs[i];
    limb.hp = ld.hp;
    limb.size = model.size;
    limb.restOffset = Vec3{(float)model.offset.x, (float)model.offset.y,
                           (float)model.offset.z};
    limb.voxels.reserve(model.voxels.size());
    for (const PrefabVoxel& v : model.voxels) {
      uint32_t variant = ((uint32_t)(v.x * 7 + v.y * 13 + v.z * 29)) % 3u;
      limb.voxels.push_back({(int8_t)v.x, (int8_t)v.y, (int8_t)v.z, 0,
                             (uint16_t)(v.material | (variant << 12))});
    }
    IVec3 origin{atVoxel.x + model.offset.x, atVoxel.y + model.offset.y,
                 atVoxel.z + model.offset.z};
    limb.body = phys_->CreateDebrisBody(limb.voxels, origin, densityOf_,
                                        true /*allowKinematic*/);
    if (limb.body == 0) {
      for (Limb& l : mob.limbs)
        if (l.body) phys_->RemoveBody(l.body);
      return 0;
    }
    phys_->SetBodyKinematic(limb.body, true);
    limb.xf.pos = Vec3{(float)origin.x, (float)origin.y, (float)origin.z};
    limb.xf.quat[3] = 1;
  }

  // joints after all bodies exist (world-space anchors at the rest pose)
  for (size_t i = 0; i < def.limbs.size(); i++) {
    const MobLimbDef& ld = def.limbs[i];
    if ((int)i == def.rootLimb) continue;
    int pi = -1;
    for (size_t k = 0; k < def.limbs.size(); k++)
      if (def.limbs[k].name == ld.parent) pi = (int)k;
    int mi = FindModel(def.prefab, ld.name);
    int pmi = FindModel(def.prefab, ld.parent);
    Vec3 anchor = ld.anchorAuto
                      ? AutoAnchor(def.prefab.models[mi], def.prefab.models[pmi])
                      : ld.anchor;
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

  // root's "anchor" is its own center (yaw pivot)
  {
    Limb& root = mob.limbs[def.rootLimb];
    root.anchorRoot = root.restOffset +
                      Vec3{root.size.x * 0.5f, 0, root.size.z * 0.5f};
    root.anchorLimb = Vec3{root.size.x * 0.5f, 0, root.size.z * 0.5f};
  }

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

void MobSystem::PreTick(uint32_t tick, World& world, std::vector<BrushOp>& ops) {
  const float dt = 1.0f / 30.0f;
  IVec3 wo = world.WindowOrigin();
  Vec3 wlo{(float)(wo.x * (int)kChunk), (float)(wo.y * (int)kChunk),
           (float)(wo.z * (int)kChunk)};
  int bleedOps = 0;

  for (size_t mi = 0; mi < mobs_.size();) {
    Mob& mob = mobs_[mi];
    const MobDef& def = defs_[mob.defIndex];

    // corpses hand their bodies to DebrisSystem in Die(); drop the husk
    if (!mob.alive && mob.limbs.empty()) {
      mobs_[mi] = std::move(mobs_.back());
      mobs_.pop_back();
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
      for (Limb& l : mob.limbs)
        if (l.body) phys_->RemoveBody(l.body);
      mobs_[mi] = std::move(mobs_.back());
      mobs_.pop_back();
      instancesDirty_ = true;
      continue;
    }

    // terrain collision anchors for every live limb (ManageTerrain sweep)
    float mobRadius = 0.5f * std::sqrt((float)(def.prefab.size.x * def.prefab.size.x +
                                               def.prefab.size.y * def.prefab.size.y +
                                               def.prefab.size.z * def.prefab.size.z));
    debris_->AddTerrainAnchor(mob.origin + Vec3{def.prefab.size.x * 0.5f,
                                                def.prefab.size.y * 0.5f,
                                                def.prefab.size.z * 0.5f},
                              mobRadius);

    if (mob.alive) {
      // ---- kinematic walk (PLAN §B4: keyframes, not motors) ----
      float cx = mob.origin.x + def.prefab.size.x * 0.5f;
      float cz = mob.origin.z + def.prefab.size.z * 0.5f;
      Vec3 fwd{std::sin(mob.heading), 0, std::cos(mob.heading)};

      int groundY;
      bool haveGround = GroundHeightAt(world, ifloor(cx), ifloor(cz),
                                       ifloor(mob.origin.y) + 3, groundY);
      int aheadY;
      bool haveAhead = GroundHeightAt(
          world, ifloor(cx + fwd.x * ((float)def.prefab.size.x * 0.5f + 2.0f)),
          ifloor(cz + fwd.z * ((float)def.prefab.size.z * 0.5f + 2.0f)),
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

      // drive each limb transform: yaw about the mob center column, swing
      // about the joint anchor
      Quat yaw = AxisAngle({0, 1, 0}, mob.heading);
      Vec3 yawPivot{cx - mob.origin.x, 0, cz - mob.origin.z};
      for (size_t i = 0; i < mob.limbs.size(); i++) {
        Limb& limb = mob.limbs[i];
        if (!limb.body) continue;
        const MobLimbDef& ld = def.limbs[i];
        float swing = ld.swingAmp *
                      std::sin(mob.phase + ld.swingPhase * 3.14159265f);
        Quat rot = Mul(yaw, AxisAngle(ld.axis, swing));
        // world = origin + yaw*(anchorRoot - pivot) + pivot + rot*(local - anchorLimb)
        Vec3 anchorW = mob.origin + yawPivot +
                       Rotate(yaw, limb.anchorRoot - yawPivot);
        Vec3 pos = anchorW - Rotate(rot, limb.anchorLimb);
        float q[4] = {rot.x, rot.y, rot.z, rot.w};
        phys_->MoveKinematicBody(limb.body, pos, q, dt);
      }
    }

    // ---- bleeding (PLAN §B5): decaying wound budget, bounded ops ----
    if (def.bleedMat != 0) {
      for (Limb& limb : mob.limbs) {
        if (limb.bleedBudget < 1.0f || bleedOps >= kBleedOpsPerTick) continue;
        if ((tick & 3u) != 0) continue;  // drip every 4th tick
        Vec3 w = limb.body
                     ? limb.xf.pos + Rotate({limb.xf.quat[0], limb.xf.quat[1],
                                             limb.xf.quat[2], limb.xf.quat[3]},
                                            limb.woundLocal)
                     : mob.origin + limb.anchorRoot;  // stump on the parent
        ops.push_back({ifloor(w.x), ifloor(w.y), ifloor(w.z), 1,
                       def.bleedMat, 0 /*paint into air*/, 0, 0});
        limb.bleedBudget -= 1.0f;
        bleedOps++;
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

bool MobSystem::Damage(uint64_t bodyHandle, float amount, Vec3 hitWorldVoxel) {
  for (Mob& mob : mobs_) {
    for (size_t i = 0; i < mob.limbs.size(); i++) {
      Limb& limb = mob.limbs[i];
      if (limb.body != bodyHandle) continue;
      const MobDef& def = defs_[mob.defIndex];
      Quat q{limb.xf.quat[0], limb.xf.quat[1], limb.xf.quat[2], limb.xf.quat[3]};
      // beam crossing the joint anchor severs outright (PLAN §C2)
      if ((int)i != def.rootLimb && limb.joint) {
        Vec3 anchorW = limb.xf.pos + Rotate(q, limb.anchorLimb);
        if ((hitWorldVoxel - anchorW).len() < 1.75f) {
          Sever(mob.id, (int)i);
          return true;
        }
      }
      limb.hp -= amount;
      limb.woundLocal = RotateInv(q, hitWorldVoxel - limb.xf.pos);
      limb.bleedBudget = std::min(limb.bleedBudget + amount * def.bleedPerDamage,
                                  120.0f);
      if (limb.hp <= 0) Sever(mob.id, (int)i);
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
      DetachLimb(mob, limbIndex, true);
      // the stump bleeds: wound at the joint on the PARENT side
      for (size_t k = 0; k < def.limbs.size(); k++)
        if (def.limbs[k].name == ld.parent) {
          Limb& parent = mob.limbs[k];
          Quat q{parent.xf.quat[0], parent.xf.quat[1], parent.xf.quat[2],
                 parent.xf.quat[3]};
          Vec3 anchorW = mob.origin + mob.limbs[limbIndex].anchorRoot;
          parent.woundLocal = RotateInv(q, anchorW - parent.xf.pos);
          parent.bleedBudget = std::min(parent.bleedBudget + 40.0f, 120.0f);
        }
    }
    return;
  }
}

void MobSystem::DetachLimb(Mob& mob, int limbIndex, bool adopt) {
  Limb& limb = mob.limbs[limbIndex];
  if (!limb.body) return;
  if (limb.joint) {
    phys_->DestroyJoint(limb.joint);
    limb.joint = 0;
  }
  // children of this limb are orphaned too: their joints attach to it and
  // die with the body chain when severed recursively
  const MobDef& def = defs_[mob.defIndex];
  for (size_t k = 0; k < def.limbs.size(); k++)
    if (def.limbs[k].parent == def.limbs[limbIndex].name && mob.limbs[k].body)
      DetachLimb(mob, (int)k, adopt);

  phys_->SetBodyKinematic(limb.body, false);
  if (adopt) debris_->AdoptBody(limb.body, limb.voxels, limb.xf);
  else phys_->RemoveBody(limb.body);
  limb.body = 0;
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
    debris_->AdoptBody(limb.body, limb.voxels, limb.xf);
    limb.body = 0;
    if (limb.joint) limb.joint = 0;  // ownership follows the bodies now
  }
  instancesDirty_ = true;
  // corpse mobs are removed next PreTick sweep (no live limbs)
  mob.limbs.clear();
}

void MobSystem::AppendInstances(std::vector<BodyVoxInst>& out,
                                uint32_t slotBase) {
  uint32_t slot = slotBase;
  for (const Mob& mob : mobs_) {
    for (const Limb& limb : mob.limbs) {
      if (!limb.body) continue;
      if (slot >= kMaxBodySlots) break;
      for (const DebrisVoxel& v : limb.voxels) {
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

uint32_t MobSystem::LimbBodyCount() const {
  uint32_t n = 0;
  for (const Mob& mob : mobs_)
    for (const Limb& limb : mob.limbs)
      if (limb.body) n++;
  return n;
}
