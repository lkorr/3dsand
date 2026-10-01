// selftest_itemstage.cpp — THE ITEM STAGE (DESIGN.md §8b "The item stage";
// docs/PLAN_weapon_coats.md package C)
//
// item-stage: the stage's picture, its pick and its brush, through the same
// functions the game calls (ui/item_stage.h Render / Pick, game/itemstage.h
// ApplyStroke -- what TickAuthority runs for PlayerSession::itemStroke), on a
// real creature's real kit:
//
//   A  picture   the shipped sword drawn on the CPU: pixels drawn, the
//                silhouette centred on the picture.
//   B  pick      the ray through the centre pixel meets a voxel.
//   C  stroke    a filled flask of the test liquid, brushed over the item in
//                the PACK for N ticks: voxels coated, the flask spent, and
//                exactly what left the flask is what the stroke reports.
//   D  reopen    the stage shut and opened again on the same item (the
//                stack moved away and back): the lattice and the picture are
//                byte-identical.
//   E  wash      a washing liquid brushed over the same spot rinses the coat
//                down (stainPrecedence: washing is water, not a tool).
//   F  held      the coated sword in the fist, brushed again through its
//                HAND slot: the live rig slot's coat equals the item's
//                lattice, voxel for voxel.
//
// It writes the last picture to build/item_stage.bmp (3x, over the panel's
// backdrop) for a person to look at.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "game/equipment.h"
#include "game/item.h"
#include "game/itemcoat.h"
#include "game/itemstage.h"
#include "game/mob.h"
#include "sim/materials.h"
#include "test/selftest.h"
#include "test/support.h"
#include "ui/item_stage.h"

using namespace sandvox;

namespace selftest {
namespace {

IVec3 Site(const World& world, int inset) {
  const IVec3 org = world.WindowOrigin();
  const int x = org.x * (int)kChunk + inset;
  const int z = org.z * (int)kChunk + inset;
  return IVec3{x, World::TerrainHeight(x, z, kDefaultSeed) + 1, z};
}

uint32_t SumCoat(const std::vector<PrefabVoxel>& lat, uint32_t mat) {
  uint32_t s = 0;
  for (const PrefabVoxel& v : lat)
    if (v.stain && BodyStainMat(v.stain) == mat) s += BodyStainAmt(v.stain);
  return s;
}
uint32_t CountCoat(const std::vector<PrefabVoxel>& lat, uint32_t mat) {
  uint32_t n = 0;
  for (const PrefabVoxel& v : lat)
    if (v.stain && BodyStainMat(v.stain) == mat) n++;
  return n;
}

Status GateItemStage(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  const ItemDef* sword = c.items.Named("sword");
  const ItemDef* flask = nullptr;
  for (const ItemDef& d : c.items.items)
    if (d.IsContainer()) { flask = &d; break; }
  if (!sword || !flask || !itemstage::StageTakes(*sword)) {
    detail = "the item library has no sword or no vessel";
    return Status::Skip;
  }
  const std::string* liqName = BaselineValue("itemStageLiquid");
  const uint32_t liq = mobs.MaterialIdNamed(liqName ? *liqName : "oil");
  if (!liq || liq >= c.mats.size() || c.mats[liq].stainSlot == 0) {
    detail = "the test liquid has no body coat";
    return Status::Fail;
  }
  // The washer is whatever the materials say washes (materials.json
  // `washes`), not a name.
  uint32_t washer = 0;
  for (size_t m = 1; m < c.mats.size() && !washer; m++)
    if ((c.mats[m].gpu.stainPack & kStainPackWashesBit) && c.mats[m].stainSlot)
      washer = (uint32_t)m;
  const int ticks = (int)BaselineNumber("itemStageTicks", 10);
  const float radius = (float)BaselineNumber("itemStageRadius", 3.0);
  const int handSlot = EquipSlotOfHand(Hand::Right);

  // ---- a creature to own the kit (and later hold the sword) ---------------
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  mobs.Reset();
  c.debris.Reset();
  uint64_t wid = 0;
  for (size_t d = 0; d < mobs.Defs().size() && !wid; d++) {
    if (mobs.Defs()[d].limbs.empty()) continue;
    const uint64_t id = mobs.Spawn((int)d, Site(c.world, 200));
    if (!id) continue;
    // DIRECT PHASE CALLS ON PURPOSE (W2-O): fixture posing, as coat-transfer's
    // setup does, so the rig has a hand pose to grip in.
    for (int i = 0; i < 4; i++) {
      std::vector<BrushOp> ops;
      std::vector<ParticleSpawn> sp;
      std::vector<CellOp> cellOps;
      mobs.PreTick(2000u + (uint32_t)i, c.world, ops, cellOps, sp);
      c.phys.Step(kTickDt);
      mobs.PostStep();
    }
    Mob* m = mobs.FindMobById(id);
    if (m && m->EquipItemAsWas(sword, Hand::Right, nullptr)) {
      m->EquipItemAsWas(nullptr, Hand::Right, nullptr);
      wid = id;
    } else {
      mobs.Reset();
    }
  }
  Mob* w = wid ? mobs.FindMobById(wid) : nullptr;
  if (!w) {
    detail = "no loaded creature can hold the sword";
    return Status::Fail;
  }
  Kit& kit = w->KitMut();
  for (ItemStack& s : kit.bag.slots) s = ItemStack{};
  kit.equip.slots[handSlot] = ItemStack{};
  const KitRef itemBag{KitSpace::Bag, 0}, vesselRef{KitSpace::Bag, 1};
  const KitRef washRef{KitSpace::Bag, 2}, parkRef{KitSpace::Bag, 7};
  const KitRef hand{KitSpace::Equip, handSlot};
  kit.bag.slots[0] = ItemStack{sword->name, 1};
  kit.bag.slots[1] = ItemStack{flask->name, 1};
  kit.bag.slots[1].contents.Add((uint16_t)liq, (uint32_t)flask->container.capacity);
  if (washer) {
    kit.bag.slots[2] = ItemStack{flask->name, 1};
    kit.bag.slots[2].contents.Add((uint16_t)washer, (uint32_t)flask->container.capacity);
  }

  bool ok = true;
  std::string why;
  auto fail = [&](const std::string& s) { ok = false; why += " | " + s; };
  const int W = (int)BaselineNumber("itemStageW", 256), H = (int)BaselineNumber("itemStageH", 192);
  itemstage::Look look;
  look.mats = &c.mats;
  look.artColors = mobs.MicroSet() ? &mobs.MicroSet()->artColors : nullptr;
  look.scale = sword->scale;
  const itemstage::View view;
  auto draw = [&](const KitRef& ref, std::vector<uint8_t>& px, itemstage::LatticeGrid& g,
                  itemstage::StageCamera& cam) -> const std::vector<PrefabVoxel>* {
    static std::vector<PrefabVoxel> scratch;
    const ItemStack* st = kit.Resolve(ref);
    const std::vector<PrefabVoxel>* lat =
        st ? itemstage::ViewLattice(*st, *sword, 0, scratch) : nullptr;
    if (!lat) return nullptr;
    g.Build(*lat);
    cam = itemstage::MakeCamera(g, view, W, H);
    look.dye = st->dye;
    itemstage::Render(g, *lat, cam, look, px);
    return lat;
  };

  // ---- A: the picture ----------------------------------------------------------
  std::vector<uint8_t> pxA;
  itemstage::LatticeGrid g;
  itemstage::StageCamera cam;
  uint32_t drawn = 0;
  double cx = 0, cy = 0;
  if (draw(itemBag, pxA, g, cam)) {
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++)
        if (pxA[((size_t)y * W + x) * 4 + 3]) {
          drawn++;
          cx += x + 0.5;
          cy += y + 0.5;
        }
  }
  if (drawn) {
    cx /= drawn;
    cy /= drawn;
  }
  const double tol = BaselineNumber("itemStageCentreTol", 0.12);
  if (drawn < (uint32_t)BaselineNumber("itemStageMinPixels", 200))
    fail(Format("A: only %u pixels drawn", drawn));
  if (std::fabs(cx / W - 0.5) > tol || std::fabs(cy / H - 0.5) > tol)
    fail(Format("A: silhouette centred at (%.0f,%.0f) of %dx%d", cx, cy, W, H));

  // ---- B: the pick ----------------------------------------------------------------
  const int pickC = itemstage::Pick(g, cam, W * 0.5f, H * 0.5f);
  if (pickC < 0) fail("B: the centre pixel meets no voxel");

  // ---- C: a stroke on the item in the pack ----------------------------------------
  itemstage::Stroke sk;
  sk.active = true;
  sk.item = itemBag;
  sk.shell = 0;
  sk.radius = radius;
  sk.vessel = vesselRef;
  cam.Ray(W * 0.5f, H * 0.5f, sk.ro, sk.rd);
  const uint32_t vBefore = kit.bag.slots[1].contents.Total();
  int64_t milli = 0;
  uint32_t spent = 0, marked = 0, hits = 0;
  for (int t = 0; t < ticks; t++) {
    const itemstage::StrokeResult r =
        itemstage::ApplyStroke(kit, &mobs, wid, c.items, c.mats, sk, milli);
    spent += r.spent;
    marked += r.marked;
    hits += r.hit ? 1 : 0;
  }
  const uint32_t vAfter = kit.bag.slots[1].contents.Total();
  const std::vector<PrefabVoxel>* recC = ItemLatticeIfAny(kit.bag.slots[0]);
  const uint32_t coatedC = recC ? CountCoat(*recC, liq) : 0;
  const uint32_t sumC = recC ? SumCoat(*recC, liq) : 0;
  if (coatedC == 0) fail("C: the stroke coated nothing");
  if (vAfter >= vBefore) fail("C: the flask was not spent");
  if (vBefore - vAfter != spent) fail(Format("C: flask lost %u, stroke says %u", vBefore - vAfter, spent));

  // ---- D: shut, moved away and back, reopened -------------------------------------
  std::vector<uint8_t> pxD0, pxD1;
  itemstage::LatticeGrid gD;
  itemstage::StageCamera camD;
  draw(itemBag, pxD0, gD, camD);
  const std::vector<PrefabVoxel> beforeMove = recC ? *recC : std::vector<PrefabVoxel>{};
  uint32_t dDiff = 0;
  if (w->KitMove(itemBag, parkRef, c.items) != MoveResult::Ok ||
      w->KitMove(parkRef, itemBag, c.items) != MoveResult::Ok) {
    fail("D: the pack refused the move");
  } else {
    const std::vector<PrefabVoxel>* back = ItemLatticeIfAny(kit.bag.slots[0]);
    if (!back || back->size() != beforeMove.size()) dDiff = 1u << 30;
    else
      for (size_t i = 0; i < back->size(); i++)
        if ((*back)[i].stain != beforeMove[i].stain || (*back)[i].x != beforeMove[i].x)
          dDiff++;
    draw(itemBag, pxD1, gD, camD);
    if (pxD1 != pxD0) fail("D: the reopened picture differs");
  }
  if (dDiff) fail(Format("D: %u voxels differ after reopen", dDiff));

  // ---- E: water over it -------------------------------------------------------------
  uint32_t sumE = sumC;
  if (washer) {
    itemstage::Stroke sw = sk;
    sw.vessel = washRef;
    int64_t wm = 0;
    for (int t = 0; t < ticks; t++) itemstage::ApplyStroke(kit, &mobs, wid, c.items, c.mats, sw, wm);
    const std::vector<PrefabVoxel>* recE = ItemLatticeIfAny(kit.bag.slots[0]);
    sumE = recE ? SumCoat(*recE, liq) : 0;
    if (sumE >= sumC) fail(Format("E: washing left the coat at %u (was %u)", sumE, sumC));
  }

  // ---- F: in the hand -----------------------------------------------------------------
  // Coat a second patch first (off-centre), so the blade carries coat AND a
  // rinsed patch when it goes into the fist.
  {
    itemstage::Stroke s2 = sk;
    cam.Ray(W * 0.30f, H * 0.5f, s2.ro, s2.rd);
    if (itemstage::Pick(g, cam, W * 0.30f, H * 0.5f) < 0) cam.Ray(W * 0.62f, H * 0.5f, s2.ro, s2.rd);
    for (int t = 0; t < ticks; t++) itemstage::ApplyStroke(kit, &mobs, wid, c.items, c.mats, s2, milli);
  }
  uint32_t fMismatch = 0, fCompared = 0, fCoated = 0;
  int fPushed = -1;
  bool fRan = false;
  if (w->KitMove(itemBag, hand, c.items) == MoveResult::Ok &&
      w->EquipItemAsWas(sword, Hand::Right, &kit.equip.slots[handSlot].damage)) {
    const int slot = w->HeldSlot(Hand::Right);
    itemstage::Stroke sh = sk;
    sh.item = hand;
    cam.Ray(W * 0.70f, H * 0.5f, sh.ro, sh.rd);
    if (itemstage::Pick(g, cam, W * 0.70f, H * 0.5f) < 0) cam.Ray(W * 0.5f, H * 0.5f, sh.ro, sh.rd);
    for (int t = 0; t < ticks; t++) {
      const itemstage::StrokeResult r =
          itemstage::ApplyStroke(kit, &mobs, wid, c.items, c.mats, sh, milli);
      if (r.hit) fPushed = std::max(fPushed, r.pushed);
    }
    const std::vector<PrefabVoxel> live = mobs.LimbLattice(wid, slot);
    const std::vector<PrefabVoxel>* rec = ItemLatticeIfAny(kit.equip.slots[handSlot]);
    std::map<std::tuple<int, int, int>, uint16_t> at;
    if (rec)
      for (const PrefabVoxel& v : *rec) at[{v.x, v.y, v.z}] = v.stain;
    for (const PrefabVoxel& v : live) {
      auto it = at.find({v.x, v.y, v.z});
      if (it == at.end()) continue;
      fCompared++;
      if (it->second != v.stain) fMismatch++;
      if (v.stain) fCoated++;
    }
    fRan = rec != nullptr;
  }
  if (!fRan) fail("F: could not put the coated sword in the hand");
  if (fCompared == 0) fail("F: no voxel of the live blade matches the item lattice");
  if (fMismatch) fail(Format("F: %u live voxels disagree with the item", fMismatch));
  if (fCoated == 0) fail("F: the blade in the fist wears no coat");

  // ---- the picture for a person -------------------------------------------------------
  {
    std::vector<uint8_t> px;
    itemstage::LatticeGrid gF;
    itemstage::StageCamera camF;
    draw(hand, px, gF, camF);
    const int S = 3;
    std::vector<uint8_t> big((size_t)W * S * H * S * 4);
    for (int y = 0; y < H * S; y++)
      for (int x = 0; x < W * S; x++) {
        const uint8_t* p = &px[((size_t)(y / S) * W + x / S) * 4];
        // The panel's backdrop: dark stepped bands, lighter toward the middle.
        const int band = (y / S) * 8 / H;
        const float k = 1.0f - std::fabs(band - 3.5f) / 3.5f;
        const uint8_t bg[3] = {(uint8_t)(16 + 24 * k), (uint8_t)(14 + 19 * k), (uint8_t)(22 + 26 * k)};
        uint8_t* o = &big[((size_t)y * W * S + x) * 4];
        for (int ch = 0; ch < 3; ch++) o[ch] = p[3] ? p[ch] : bg[ch];
        o[3] = 255;
      }
    WriteBmpFile("build/item_stage.bmp", big, (uint32_t)(W * S), (uint32_t)(H * S));
  }

  RecordObserved("itemStagePixels", (double)drawn);
  RecordObserved("itemStageCoated", (double)coatedC);
  RecordObserved("itemStageSpent", (double)spent);
  mobs.Reset();
  c.debris.Reset();

  detail = Format(
      "liquid %s, washer %s | A %u px, centre (%.0f,%.0f) of %dx%d | B centre pick voxel %d "
      "| C %d/%d ticks hit, %u voxels coated (sum %u), flask %u -> %u (stroke spent %u) "
      "| D %u differ | E coat sum %u -> %u | F pushed %d, %u compared, %u coated, %u "
      "disagree%s",
      liqName ? liqName->c_str() : "oil",
      washer ? c.mats[washer].name.c_str() : "none", drawn, cx, cy, W, H, pickC, (int)hits,
      ticks, coatedC, sumC, vBefore, vAfter, spent, dDiff, sumC, sumE, fPushed, fCompared,
      fCoated, fMismatch, why.c_str());
  std::printf("item-stage: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& ItemStageGates() {
  static const std::vector<Gate> g = {
      {"item-stage", "mob", {}, false, GateItemStage, false},
  };
  return g;
}

}  // namespace selftest
