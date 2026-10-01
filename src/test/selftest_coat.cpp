// selftest_coat.cpp — A COAT MOVES ON CONTACT (DESIGN.md §7; PLAN_weapon_coats A)
//
// coat-transfer: one gate, five claims, every blow through the REAL
// MeleeSweepDamage (the path the player's and every NPC's swing take) against
// a fresh spawn on pristine ground:
//
//   A  base strike   a sword coated on the TIP HALF ONLY, struck with its
//                    base: the wound takes no coat at all. "Which voxels
//                    touched" is real, not "the weapon is coated".
//   B  tip strike    the same blade struck with its tip: coat lands on the
//                    WOUND WALL (exposed survivors of the carve), the tip's
//                    coat FELL, and the blade's contact voxels now carry the
//                    target's own wound fluid.
//   C  blunt         a mace coated all over, on intact skin: coat lands on
//                    surface voxels only -- no coated voxel below depth 1.
//   D  fist          a coated bare hand punches: coat lands on the target.
//   E  the bag       the coated blade unequipped into the bag, the world
//                    ticked, and re-equipped: the bag's record never moved
//                    and the blade comes back voxel-for-voxel as it went in.
//
// The liquid is DATA (baseline coatTransferLiquid, default "oil": a distinct,
// inert body coat that dries slowly). Nothing here depends on venom.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "game/equipment.h"
#include "game/item.h"
#include "game/itemcoat.h"
#include "game/melee.h"
#include "game/mob.h"
#include "phys/coatcontact.h"
#include "sim/tuning.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

using namespace sandvox;

namespace selftest {
namespace {

// The fixture helpers selftest_wound.cpp uses, restated: they are file-local
// there (and in selftest_impact.cpp) by the same choice.
struct Target {
  int defIndex = -1;
  int limb = -1;
  uint32_t atSpawn = 0;
  std::string defName, limbName;
  bool valid() const { return defIndex >= 0 && limb >= 0; }
};

struct LimbAxis {
  Vec3 anchor{};
  Vec3 along{0, 1, 0};
  Vec3 edge{1, 0, 0};
  Vec3 travel{0, 0, 1};
  float reach = 1.0f;
  bool valid = false;
};

LimbAxis MeasureLimb(MobSystem& mobs, uint64_t id, int limb) {
  LimbAxis a;
  if (!mobs.LimbBody(id, limb)) return a;
  a.anchor = mobs.LimbAnchorPos(id, limb);
  Vec3 sum{};
  std::vector<Vec3> pts;
  for (uint32_t k = 0; k < 24; k++) {
    const Vec3 p = mobs.LimbVoxelPos(id, limb, k * 7919u);
    pts.push_back(p);
    sum += p;
  }
  const Vec3 centroid = sum * (1.0f / (float)pts.size());
  Vec3 along = centroid - a.anchor;
  if (along.len() < 1e-3f) along = Vec3{0, -1, 0};
  a.along = along.normalized();
  float far = 0;
  for (const Vec3& p : pts) far = std::max(far, (p - a.anchor).dot(a.along));
  a.reach = std::max(far, 0.5f);
  const Vec3 cand[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  int best = 0;
  float bestDot = 2.0f;
  for (int i = 0; i < 3; i++) {
    const float d = std::fabs(a.along.dot(cand[i]));
    if (d < bestDot) { bestDot = d; best = i; }
  }
  a.travel = a.along.cross(cand[best]).normalized();
  a.edge = a.travel.cross(a.along).normalized();
  a.valid = true;
  return a;
}

Target ChooseTarget(MobSystem& mobs, IVec3 at) {
  Target best;
  for (size_t d = 0; d < mobs.Defs().size(); d++) {
    const MobDef& def = mobs.Defs()[d];
    if (def.limbs.empty() || def.bleedMat == 0) continue;
    // The fist arm needs a creature that can punch.
    if (def.FindNatural("fist.R") < 0) continue;
    mobs.Reset();
    const uint64_t id = mobs.Spawn((int)d, at);
    if (!id) continue;
    for (size_t li = 0; li < def.limbs.size(); li++) {
      if ((int)li == def.rootLimb) continue;
      if (!def.limbs[li].severable || def.limbs[li].vital) continue;
      if (def.limbs[li].bloodless) continue;
      if (!mobs.LimbBody(id, (int)li)) continue;
      const uint32_t n = mobs.LimbVoxelsAtSpawn(id, (int)li);
      if (n <= best.atSpawn) continue;
      best.defIndex = (int)d;
      best.limb = (int)li;
      best.atSpawn = n;
      best.defName = def.name;
      best.limbName = def.limbs[li].name;
    }
  }
  mobs.Reset();
  return best;
}

IVec3 FixtureSite(const World& world, int inset) {
  const IVec3 org = world.WindowOrigin();
  const int x = org.x * (int)kChunk + inset;
  const int z = org.z * (int)kChunk + inset;
  return IVec3{x, World::TerrainHeight(x, z, kDefaultSeed) + 1, z};
}

// Is lattice voxel `v` exposed (an empty face neighbour) in `lat`?
struct Occ {
  LatticeOcc o;
  explicit Occ(std::vector<PrefabVoxel>& lat) {
    StainLattice L;
    L.skin = &lat;
    o = BuildLatticeOcc(L);
  }
  bool Exposed(const PrefabVoxel& v) const {
    return o.Exposed(IVec3{v.x, v.y, v.z});
  }
};

uint32_t SumCoat(const std::vector<PrefabVoxel>& lat, uint32_t mat) {
  uint32_t s = 0;
  for (const PrefabVoxel& v : lat)
    if (BodyStainMat(v.stain) == mat) s += BodyStainAmt(v.stain);
  return s;
}
uint32_t CountCoat(const std::vector<PrefabVoxel>& lat, uint32_t mat) {
  uint32_t n = 0;
  for (const PrefabVoxel& v : lat)
    if (v.stain && BodyStainMat(v.stain) == mat) n++;
  return n;
}

Status GateCoatTransfer(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 170));
  if (!t.valid()) {
    detail = "no loaded mob def with fists has a severable limb that bleeds";
    return Status::Fail;
  }
  const ItemDef* sword = c.items.At(c.items.Find("sword"));
  const ItemDef* mace = c.items.At(c.items.Find("mace"));
  if (!sword || !mace || !sword->hasEdge || !mace->hasEdge) {
    detail = "the item library has no edged sword or mace";
    return Status::Skip;
  }
  const std::string* liqName = BaselineValue("coatTransferLiquid");
  const uint32_t liq = mobs.MaterialIdNamed(liqName ? *liqName : "oil");
  if (!liq || mobs.StainTypeOf(liq) == 0) {
    detail = "the test liquid has no body coat";
    return Status::Fail;
  }
  const uint32_t coatAmt = (uint32_t)BaselineNumber("coatTransferAmt", 12);
  const auto& g = CurrentTuning().gore;
  MeleeTuning mt;
  ApplyMeleeTuning(mt);
  const float speed = (float)BaselineNumber("bladeWoundSpeed", 0.8);
  const float step = mt.fullSpeed * kTickDt * speed;
  const int handSlot = EquipSlotOfHand(Hand::Right);

  struct Arm {
    bool ran = false, landed = false;
    uint64_t wid = 0, id = 0;
    int heldSlot = -1;
    std::vector<PrefabVoxel> bladeBefore, bladeAfter, targetAfter;
    MobSystem::CoatContactResult res;
  };
  // Fresh pair, the wielder holding `item` (null = bare-handed), coated by
  // `coat` through the item API (or, bare-handed, the hand by SoakLimb).
  auto setup = [&](Arm& a, const ItemDef* item,
                   const std::function<bool(const Vec3&)>& coatWhere) -> Mob* {
    mobs.Reset();
    c.debris.Reset();
    a.wid = mobs.Spawn(t.defIndex, FixtureSite(c.world, 225));
    a.id = mobs.Spawn(t.defIndex, FixtureSite(c.world, 170));
    if (!a.wid || !a.id) return nullptr;
    // DIRECT PHASE CALLS ON PURPOSE (W2-O): fixture posing (SpawnTarget's).
    for (int i = 0; i < 8; i++) {
      std::vector<BrushOp> ops;
      std::vector<ParticleSpawn> st;
      std::vector<CellOp> cellOps;
      mobs.PreTick(1000u + (uint32_t)i, c.world, ops, cellOps, st);
      c.phys.Step(kTickDt);
      mobs.PostStep();
    }
    Mob* w = mobs.FindMobById(a.wid);
    if (!w) return nullptr;
    if (item) {
      // The item through the KIT, as the player's is: the hand slot holds the
      // stack and the fist is dressed from it.
      w->KitMut().equip.slots[handSlot] = ItemStack{item->name, 1};
      if (!w->EquipItemAsWas(item, Hand::Right, nullptr)) return nullptr;
      a.heldSlot = w->HeldSlot(Hand::Right);
      // Coat the item's own lattice where asked (item frame, world voxels
      // from its corner), then push it onto the blade in the fist.
      ItemStack& st = w->KitMut().equip.slots[handSlot];
      std::vector<PrefabVoxel>& lat = ItemLatticeMut(st, *item);
      const float inv = 1.0f / (float)std::max(1u, item->scale);
      for (PrefabVoxel& v : lat) {
        const Vec3 p{((float)v.x + 0.5f) * inv, ((float)v.y + 0.5f) * inv,
                     ((float)v.z + 0.5f) * inv};
        if (coatWhere(p)) v.stain = PackBodyStain(liq, coatAmt);
      }
      if (PushItemLatticeToLimb(*w, KitRef{KitSpace::Equip, handSlot}) <= 0)
        return nullptr;
      a.bladeBefore = mobs.LimbLattice(a.wid, a.heldSlot);
    }
    a.ran = true;
    return w;
  };
  // One fabricated sweep: an edge of `len` world voxels laid along the limb's
  // cross-section axis so that fraction `u` of it sits at the limb's middle,
  // travelling across the limb by one tick's swing.
  auto strike = [&](Arm& a, Mob& w, const StrikeProfile& prof, float halfW,
                    float carve, float heft, float u, float len, bool selfMounted,
                    uint32_t tick) {
    const LimbAxis ax = MeasureLimb(mobs, a.id, t.limb);
    const Vec3 mid = ax.anchor + ax.along * (ax.reach * 0.5f);
    const Vec3 a1 = mid - ax.edge * (len * u), b1 = mid + ax.edge * (len * (1.0f - u));
    EdgeSweep sw;
    sw.aPrev = a1 - ax.travel * step;
    sw.bPrev = b1 - ax.travel * step;
    sw.aNow = a1;
    sw.bNow = b1;
    sw.flatNow = ax.along;
    sw.dt = kTickDt;
    sw.halfWidth = halfW;
    sw.carveBonus = carve;
    sw.strike = prof;
    sw.heft = heft;
    sw.tick = tick;
    sw.selfMounted = selfMounted;
    sw.valid = true;
    std::vector<ParticleSpawn> spawns;
    const EdgeSweepResult r =
        MeleeSweepDamage(sw, mt, w, c.phys, mobs, c.debris, c.world, spawns);
    a.landed = r.bodiesHit > 0;
    a.res = mobs.LastCoatContact();
    a.targetAfter = mobs.LimbLattice(a.id, t.limb);
    if (a.heldSlot >= 0) a.bladeAfter = mobs.LimbLattice(a.wid, a.heldSlot);
  };
  // Along the sword's own edge, base (0) -> tip (1).
  const Vec3 eFrom = sword->edgeFrom, eDir = sword->edgeTo - sword->edgeFrom;
  const float eLen2 = std::max(eDir.dot(eDir), 1e-6f);
  auto tipHalf = [&](const Vec3& p) { return (p - eFrom).dot(eDir) / eLen2 >= 0.5f; };
  const float swordHeft = sword->HeftFactor(g.woundHeftRef, g.woundHeftMax);
  const float kLen = 12.0f;

  bool ok = true;
  std::string why;
  auto fail = [&](const std::string& s) { ok = false; why += " | " + s; };

  // ---- A: the base of a tip-coated blade -----------------------------------
  Arm A;
  uint32_t aWound = 0;
  if (Mob* w = setup(A, sword, tipHalf)) {
    strike(A, *w, sword->strike, sword->edgeHalfWidth, sword->carveBonus,
           swordHeft, 0.04f, kLen, false, 7000u);
    aWound = CountCoat(A.targetAfter, liq);
    if (!A.landed) fail("A: the base strike never landed");
    if (aWound != 0) fail(Format("A: base strike left %u coated voxels", aWound));
  } else {
    fail("A: fixture refused");
  }

  // ---- B: the tip of the same blade ------------------------------------------
  Arm B;
  uint32_t bWound = 0, bWall = 0, tipBefore = 0, tipAfter = 0, bladeBled = 0;
  if (Mob* w = setup(B, sword, tipHalf)) {
    strike(B, *w, sword->strike, sword->edgeHalfWidth, sword->carveBonus,
           swordHeft, 0.85f, kLen, false, 7100u);
    Occ occ(B.targetAfter);
    for (const PrefabVoxel& v : B.targetAfter)
      if (v.stain && BodyStainMat(v.stain) == liq) {
        bWound++;
        if (occ.Exposed(v)) bWall++;
      }
    tipBefore = SumCoat(B.bladeBefore, liq);
    tipAfter = SumCoat(B.bladeAfter, liq);
    if (B.res.bleedMat) bladeBled = CountCoat(B.bladeAfter, B.res.bleedMat);
    if (!B.landed) fail("B: the tip strike never landed");
    if (bWall == 0) fail("B: no coat on the wound wall");
    if (bWall != bWound) fail(Format("B: %u coated voxels buried", bWound - bWall));
    if (tipAfter >= tipBefore) fail("B: the tip's coat did not fall");
    if (bladeBled == 0) fail("B: the blade carries none of the wound's fluid");
  } else {
    fail("B: fixture refused");
  }

  // ---- C: a coated mace on intact skin ---------------------------------------
  Arm C;
  uint32_t cSurf = 0, cDeep = 0;
  if (Mob* w = setup(C, mace, [](const Vec3&) { return true; })) {
    strike(C, *w, mace->strike, mace->edgeHalfWidth, mace->carveBonus,
           mace->HeftFactor(g.woundHeftRef, g.woundHeftMax), 0.5f, kLen, false,
           7200u);
    Occ occ(C.targetAfter);
    for (const PrefabVoxel& v : C.targetAfter)
      if (v.stain && BodyStainMat(v.stain) == liq) (occ.Exposed(v) ? cSurf : cDeep)++;
    if (!C.landed) fail("C: the mace never landed");
    if (cSurf == 0) fail("C: the mace left no coat");
    if (cDeep != 0) fail(Format("C: %u coated voxels below depth 1", cDeep));
  } else {
    fail("C: fixture refused");
  }

  // ---- D: a coated fist -------------------------------------------------------
  Arm D;
  uint32_t dCoat = 0;
  if (Mob* w = setup(D, nullptr, {})) {
    const MobDef& def = mobs.Defs()[t.defIndex];
    const int ni = def.FindNatural("fist.R");
    const MobNaturalWeaponDef* nw = w->NaturalWeapon(ni);
    if (nw && nw->partIndex >= 0) {
      mobs.SoakLimb(D.wid, nw->partIndex, liq, coatAmt, 7300u);
      w->SetStrikeEffector(nw->partIndex, StrikeEffectorMode::Chain, ni);
      const float hw = std::max(nw->edgeHalfWidth, MetresToCells(0.10f));
      strike(D, *w, w->StrikeProfileFor(*nw), hw, 0.0f, 1.0f, 0.5f, 2.0f, true,
             7300u);
      dCoat = CountCoat(D.targetAfter, liq);
    }
    if (!D.landed) fail("D: the punch never landed");
    if (dCoat == 0) fail("D: the fist left no coat");
  } else {
    fail("D: fixture refused");
  }

  // ---- E: into the bag, ticked, and back ---------------------------------
  // Arm B's blade, as the tip strike left it (coated AND bloodied).
  uint32_t eVox = 0, eMoved = 0, eDiff = 0, eTicks = 0;
  bool eRan = false;
  if (Mob* w = B.ran ? mobs.FindMobById(B.wid) : nullptr) {
    const std::vector<PrefabVoxel> held = mobs.LimbLattice(B.wid, B.heldSlot);
    const KitRef hand{KitSpace::Equip, handSlot}, bag{KitSpace::Bag, 0};
    if (w->KitMove(hand, bag, c.items) == MoveResult::Ok) {
      w->EquipItemAsWas(nullptr, Hand::Right, nullptr);   // the fist lets go
      const ItemStack& inBag = w->GetKit().bag.slots[0];
      const std::vector<PrefabVoxel>* rec = ItemLatticeIfAny(inBag);
      const std::vector<PrefabVoxel> stored = rec ? *rec : std::vector<PrefabVoxel>{};
      // THE REAL TICK, while it sits in the bag.
      const int n = (int)BaselineNumber("coatTransferBagTicks", 20);
      {
        const IVec3 fc = FixtureSite(c.world, 225);
        support::TickRig rig(c, 80000u, IVec3{fc.x >> 4, fc.y >> 4, fc.z >> 4});
        support::RunTicks(rig, n);
        eTicks = (uint32_t)n;
      }
      Mob* w2 = mobs.FindMobById(B.wid);
      if (w2) {
        const ItemStack& still = w2->GetKit().bag.slots[0];
        const std::vector<PrefabVoxel>* rec2 = ItemLatticeIfAny(still);
        if (!rec2 || rec2->size() != stored.size()) eMoved = 1;
        else
          for (size_t i = 0; i < stored.size(); i++)
            if ((*rec2)[i].stain != stored[i].stain) eMoved++;
        if (w2->KitMove(bag, hand, c.items) == MoveResult::Ok &&
            w2->EquipItemAsWas(sword, Hand::Right,
                               &w2->GetKit().equip.slots[handSlot].damage)) {
          const std::vector<PrefabVoxel> back =
              mobs.LimbLattice(B.wid, w2->HeldSlot(Hand::Right));
          eVox = (uint32_t)back.size();
          if (back.size() != held.size()) eDiff = 1u << 30;
          else
            for (size_t i = 0; i < held.size(); i++)
              if (back[i].x != held[i].x || back[i].y != held[i].y ||
                  back[i].z != held[i].z || back[i].stain != held[i].stain)
                eDiff++;
          eRan = true;
        }
      }
      if (!rec) fail("E: the bag stack recorded no lattice");
    }
  }
  if (!eRan) fail("E: unequip/re-equip refused");
  if (eMoved) fail(Format("E: %u coat words changed in the bag", eMoved));
  if (eDiff) fail(Format("E: %u voxels differ after re-equip", eDiff));

  RecordObserved("coatTransferTipWall", (double)bWall);
  RecordObserved("coatTransferBluntSurface", (double)cSurf);
  RecordObserved("coatTransferFist", (double)dCoat);
  mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);

  detail = Format(
      "%s/%s, liquid %s at %u | A base: %u coated in the wound (landed %d, "
      "striker patch %u) | B tip: %u on the wall of %u coated, tip coat %u -> "
      "%u, %u blade voxels wear fluid %u (sent %u, got back %u) | C mace: %u "
      "surface, %u deeper | D fist: %u | E bag: %u ticks, %u words moved, "
      "re-equipped %u voxels, %u differ%s",
      t.defName.c_str(), t.limbName.c_str(), liqName ? liqName->c_str() : "oil",
      coatAmt, aWound, A.landed ? 1 : 0, A.res.strikerTouched, bWall, bWound,
      tipBefore, tipAfter, bladeBled, B.res.bleedMat, B.res.toTarget,
      B.res.toStriker, cSurf, cDeep, dCoat, eTicks, eMoved, eVox, eDiff,
      why.c_str());
  std::printf("coat-transfer: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& CoatGates() {
  static const std::vector<Gate> g = {
      {"coat-transfer", "mob", {}, false, GateCoatTransfer, false},
  };
  return g;
}

}  // namespace selftest
