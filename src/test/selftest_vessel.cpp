// ---------------------------------------------------------------------------
// vessel: a flask scoops liquid, a pouch scoops powder, both pour it back
// ---------------------------------------------------------------------------
//
// game/container.h, against the REAL items.json rows and a hand-built grid --
// no GPU dispatch, so everything here is a pure function of its inputs and the
// gate is one `--gate vessel` away from an answer. What it pins:
//
//   * THE CONTENT PARSES: flask holds liquid, pouch holds powder, 128 cells.
//   * WHAT GOES IN: a flask takes water and refuses sand; a pouch the reverse;
//     a flask of water refuses blood. A partial liquid cell is credited its
//     fullness in eighths, not a whole cell.
//   * MATTER IS NOT MINTED: every cell credited is one CONDITIONAL clear
//     naming the material it expects (world.h CellOpClearIfMat), and a second
//     scoop against the same (stale) snapshot takes NOTHING it already took --
//     the kSnapshotLatency duplication the memo exists for. Capacity stops it.
//   * THE POUR LANDS WHERE IT IS AIMED: the particles, flown the way
//     sim_particle.wgsl flies them (v.y -= g, then p += v), come down within a
//     cell and a half of the target; the SplatterEvent names the substance.
//   * STACKS: a filled flask never merges with anything, and filling one of
//     a stack sets the rest down first.
//   * PERSISTENCE: the fill round-trips through PLYR v6.
//   * THE HEALTH PANEL'S POUR: MobSystem::DouseLimb coats exactly the limb it
//     is given with the substance poured.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "game/caster.h"
#include "game/container.h"
#include "game/equipment.h"
#include "game/mob.h"
#include "game/player.h"
#include "game/persist.h"
#include "game/worlditems.h"
#include "game/spell.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// A vessel stack of library entry `def`, holding `amt` eighths of `mat`
// (W2-M: a slot is an ItemInstance, BY NAME).
ItemStack Vs(const ItemLibrary& lib, int def, int count, uint16_t mat = 0,
             uint16_t amt = 0) {
  ItemStack s = StackOf(lib, def, count);
  s.fillMat = mat;
  s.fillAmt = amt;
  return s;
}

Status GateVessel(Ctx& c, std::string& detail) {
  int checks = 0, failed = 0;
  std::string first;
  auto check = [&](bool ok, const char* what) {
    checks++;
    if (!ok) {
      if (failed++ == 0) first = what;
      std::printf("  vessel FAIL: %s\n", what);
    }
  };
  auto matId = [&](const char* n) -> uint32_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == n) return (uint32_t)i;
    return 0;
  };
  const uint32_t mWater = matId("water"), mSand = matId("sand"),
                 mBlood = matId("blood"), mStone = matId("stone");
  // A liquid on the GRID-particle road (not the MPM seam): lava, when it is
  // one; 0 skips the checks that need it.
  uint32_t mLava = matId("lava");
  if (mLava && (c.mats[mLava].gpu.klass != CLASS_LIQUID ||
                ContainerPoursAsFluid(c.mats[mLava])))
    mLava = 0;
  const int flaskI = c.items.Find("flask"), pouchI = c.items.Find("pouch");
  const ItemDef* flask = c.items.At(flaskI);
  const ItemDef* pouch = c.items.At(pouchI);
  if (!flask || !pouch || !mWater || !mSand || !mBlood || !mStone) {
    detail = "flask/pouch missing from items.json, or water/sand/blood/stone "
             "missing from materials.json";
    return Status::Fail;
  }

  // ---- the content ---------------------------------------------------------
  check(flask->IsContainer() && flask->container.holds == (1u << CLASS_LIQUID),
        "flask is a container that holds liquid");
  check(pouch->IsContainer() && pouch->container.holds == (1u << CLASS_POWDER),
        "pouch is a container that holds powder");
  check(flask->container.capacity == 128 * kContainerUnitsPerCell,
        "flask holds 128 cells");

  // ---- a hand-built grid ---------------------------------------------------
  // A 3x3 pool of water two deep in a stone basin, one partial cell, and a
  // heap of sand beside it. Mid-window so CellInWindow is true everywhere.
  const IVec3 o = c.world.WindowOrigin();
  const IVec3 base{o.x * (int)kChunk + (int)kWorldN / 2,
                   o.y * (int)kChunk + (int)kWorldN / 2,
                   o.z * (int)kChunk + (int)kWorldN / 2};
  std::map<std::tuple<int, int, int>, uint32_t> grid;
  auto put = [&](IVec3 p, uint32_t w) { grid[{p.x, p.y, p.z}] = w; };
  for (int dz = -3; dz <= 3; dz++)
    for (int dx = -3; dx <= 3; dx++)
      for (int dy = -3; dy <= 3; dy++) {
        const IVec3 p{base.x + dx, base.y + dy, base.z + dz};
        const bool pool = std::abs(dx) <= 1 && std::abs(dz) <= 1 && dy >= -1 && dy <= 0;
        const bool heap = dx == 3 && dy >= -1 && dy <= 0;
        if (pool)
          put(p, PackVoxNew(mWater, 7u));
        else if (heap)
          put(p, PackVoxNew(mSand, 0u));
        else if (dy < -1)
          put(p, PackVoxNew(mStone, 0u));
        else
          put(p, 0u);
      }
  // One half-full cell at the surface: fullness code 3 = 4 eighths.
  put({base.x + 1, base.y, base.z + 1}, PackVoxNew(mWater, 3u));
  const ContainerWordFn wordAt = [&](IVec3 p, uint32_t& w) {
    auto it = grid.find({p.x, p.y, p.z});
    if (it == grid.end()) return false;
    w = it->second;
    return true;
  };

  // ---- what goes in --------------------------------------------------------
  {
    ItemStack st = Vs(c.items, pouchI, 1);
    std::vector<CellOp> ops;
    const char* why = nullptr;
    const int n = ContainerScoop(*pouch, st, base, wordAt, c.world, c.mats, ops, &why);
    check(n == 0 && ops.empty() && !st.Filled(), "a pouch will not take water");
    check(why && std::string(why).find("liquid") != std::string::npos,
          "and says it is a liquid");
    const int m = ContainerScoop(*pouch, st, {base.x + 3, base.y, base.z}, wordAt,
                                 c.world, c.mats, ops, &why);
    check(m > 0 && st.fillMat == mSand, "but takes sand");
  }
  {
    ItemStack st = Vs(c.items, flaskI, 1);
    std::vector<CellOp> ops;
    const char* why = nullptr;
    ContainerScoop(*flask, st, {base.x + 3, base.y, base.z}, wordAt, c.world,
                   c.mats, ops, &why);
    check(!st.Filled() && ops.empty(), "a flask will not take sand");
    ItemStack blood = Vs(c.items, flaskI, 1, (uint16_t)mBlood, 8);
    const char* why2 = nullptr;
    check(!ContainerAccepts(*flask, blood, mWater, c.mats, &why2),
          "a flask of blood will not take water");
  }

  // ---- matter is not minted ------------------------------------------------
  {
    ItemStack st = Vs(c.items, flaskI, 1);
    ContainerScoopMemo memo;
    std::vector<CellOp> ops;
    const char* why = nullptr;
    const IVec3 hit{base.x, base.y, base.z};
    const int n1 = ContainerScoop(*flask, st, hit, wordAt, c.world, c.mats, ops,
                                  &why, &memo, 100);
    check(n1 == flask->container.scoopPerTick, "a scoop takes scoopPerTick cells");
    check(!st.Filled() && memo.Pending() > 0,
          "a scoop with a ledger files a claim instead of paying itself");
    bool allCond = !ops.empty();
    for (const CellOp& op : ops)
      allCond = allCond && op.word == CellOpClearIfMat(mWater);
    check(allCond, "every cell taken is a conditional clear naming water");
    // The half cell is at distance sqrt(2) from `hit`, level with it -- within
    // the first four taken (distance 0, then the four at 1 ordered top-down,
    // then...). Rather than lean on the order, sum what the ops name.
    int expect = 0;
    for (const CellOp& op : ops) {
      for (auto& [k, w] : grid) {
        const IVec3 p{std::get<0>(k), std::get<1>(k), std::get<2>(k)};
        if (World::SlotCellIndex(p) == op.cellIdx)
          expect += ContainerCellUnits(w, c.mats[mWater]);
      }
    }
    check(memo.Pending() == expect && memo.PendingMat() == mWater,
          "the claim is exactly the eighths the cells held");
    // The SAME stale grid next tick: nothing already taken is taken again.
    std::vector<CellOp> ops2;
    ContainerScoop(*flask, st, hit, wordAt, c.world, c.mats, ops2, &why, &memo, 101);
    bool disjoint = true;
    for (const CellOp& a : ops2)
      for (const CellOp& b : ops) disjoint = disjoint && a.cellIdx != b.cellIdx;
    check(disjoint, "a second scoop off the same snapshot takes no cell twice");
    // Hold the button until the pool is gone: the credit equals the pool.
    int poolUnits = 0;
    for (auto& [k, w] : grid)
      if ((w & 0xFFFu) == mWater) poolUnits += ContainerCellUnits(w, c.mats[mWater]);
    // The grid catches up the way the real snapshot does: a clear issued on
    // tick t is visible kSnapshotLatency ticks later, and only if the cell
    // still holds what the op names (the conditional).
    std::map<uint32_t, std::tuple<int, int, int>> keyOf;
    for (auto& [k, w] : grid)
      keyOf[World::SlotCellIndex({std::get<0>(k), std::get<1>(k), std::get<2>(k)})] = k;
    std::vector<std::pair<uint32_t, CellOp>> pending;
    auto issue = [&](uint32_t t, const std::vector<CellOp>& v) {
      for (const CellOp& op : v) pending.push_back({t, op});
    };
    issue(100, ops);
    issue(101, ops2);
    // THE GPU'S LEDGER, modelled: a clear issued on tick `at` removes what the
    // cell holds when it lands, and the snapshot that shows tick `at` -- the
    // one the CPU sees kSnapshotLatency ticks later -- carries the total.
    uint32_t ledger = 0;
    for (uint32_t t = 102; t < 160; t++) {
      if (t >= 100 + World::kSnapshotLatency) {
        const uint32_t shown = t - World::kSnapshotLatency;
        for (auto& [at, op] : pending)
          if (at == shown) {
            uint32_t& w = grid[keyOf[op.cellIdx]];
            if ((w & 0xFFFu) == ((op.word >> 12) & 0xFFFu)) {
              ledger += (uint32_t)ContainerCellUnits(w, c.mats[mWater]);
              w = 0u;
            }
          }
        ContainerSettle(memo, shown, ledger, flask, &st);
      }
      // Re-aim the way the pick ray would: at the highest water left in the
      // pool (the struck cell goes to air once its clear lands).
      IVec3 aim = hit;
      int bestY = -1 << 30;
      for (auto& [k, w] : grid)
        if ((w & 0xFFFu) == mWater && std::get<1>(k) > bestY) {
          bestY = std::get<1>(k);
          aim = {std::get<0>(k), std::get<1>(k), std::get<2>(k)};
        }
      std::vector<CellOp> more;
      ContainerScoop(*flask, st, aim, wordAt, c.world, c.mats, more, &why, &memo, t);
      issue(t, more);
    }
    ContainerSettle(memo, 200, ledger, flask, &st);   // pay the tail
    check(st.fillAmt == poolUnits && memo.claims.empty(),
          "a held scoop drains the pool and is credited exactly its eighths");
    // A claim the GPU REFUSED (the cell went to something else) pays nothing.
    {
      ContainerScoopMemo m2;
      m2.ledger.have = true;
      m2.ledger.tick = 299;
      m2.ledger.eighths = 1000;
      m2.claims.push_back({300, (uint16_t)mWater, 32});
      ItemStack f2 = Vs(c.items, flaskI, 1);
      ContainerSettle(m2, 300, 1000 + 8, flask, &f2);
      check(f2.fillAmt == 8, "a claim is paid what the GPU took, not what it asked");
    }
    // ...and capacity stops it.
    ItemStack nearlyFull = Vs(c.items, flaskI, 1, (uint16_t)mWater, (uint16_t)(flask->container.capacity - 4));
    std::vector<CellOp> ops3;
    ContainerScoopMemo memo3;
    ContainerScoop(*flask, nearlyFull, hit, wordAt, c.world, c.mats, ops3, &why,
                   &memo3, 200);
    check(nearlyFull.fillAmt <= flask->container.capacity,
          "a scoop never overfills the flask");
  }

  // ---- the pour lands where it is aimed ------------------------------------
  {
    ItemStack st = Vs(c.items, flaskI, 1, (uint16_t)mBlood, 1024);
    // Within reach (flask pourRangeM), so this is the AIMED path.
    const Vec3 mouth{(float)base.x, (float)base.y + 12.0f, (float)base.z};
    const Vec3 target{(float)base.x + 12.0f, (float)base.y, (float)base.z + 4.0f};
    check(ContainerInReach(*flask, mouth, target), "the aim fixture is within reach");
    const int g = CurrentTuning().sim.partGravity;
    std::vector<ParticleSpawn> spawns;
    SplatterEvent ev;
    const int n = ContainerPour(*flask, st, mouth, Vec3{1, 0, 0}, &target, g, 500,
                                0x1234u, spawns, &ev);
    check(n == flask->container.pourPerTick && (int)spawns.size() == n,
          "a pour throws pourPerTick particles");
    check(st.fillAmt == 1024 - n * kContainerUnitsPerCell, "and charges a cell each");
    // Ten more ticks of stream, so the landing is a distribution and not two
    // samples of it.
    for (uint32_t t = 501; t < 511; t++)
      ContainerPour(*flask, st, mouth, Vec3{1, 0, 0}, &target, g, t, 0x1234u,
                    spawns, nullptr);
    check(ev.mat == mBlood && ev.count == n && ev.reach > 0.0f,
          "the splatter names the substance poured");
    bool payloadOk = true;
    float worst = 0.0f, sumX = 0.0f, sumZ = 0.0f;
    for (const ParticleSpawn& s : spawns) {
      payloadOk = payloadOk && (s.payload & 0xFFFu) == mBlood &&
                  (s.flags & kPFlagMicro) == 0 && (s.flags & kPFlagCalm) != 0;
      // Fly it the kernel's way, in 24.8 fixed, until it drops through the
      // target's height; measure the miss there.
      int64_t px = s.px, py = s.py, pz = s.pz, vx = s.vx, vy = s.vy, vz = s.vz;
      for (int t = 0; t < 400; t++) {
        vy -= g;
        px += vx;
        py += vy;
        pz += vz;
        if (vy < 0 && py <= (int64_t)std::lround(target.y * 256.0f)) break;
      }
      const float ex = px / 256.0f - target.x, ez = pz / 256.0f - target.z;
      worst = std::max(worst, std::sqrt(ex * ex + ez * ez));
      sumX += ex;
      sumZ += ez;
    }
    check(payloadOk, "poured particles are grid matter, not spray, and calm");
    // TWO claims: the stream is AIMED at the target (its centre lands within
    // half a cell -- the arc solve), and it is a stream rather than a spray
    // (no particle further out than the authored jitter can put it). The
    // bound is TWICE the +-4% velocity jitter, not once: the vertical share of
    // it also moves the landing tick, and a drop that flies a tick longer
    // travels one more tick sideways. Plus the +-0.3 cell lip, both axes.
    const float ns = (float)std::max<size_t>(1, spawns.size());
    const float centre = std::sqrt(sumX * sumX + sumZ * sumZ) / ns;
    const float spread = 2.0f * 0.04f * (target - mouth).len() + 0.6f;
    char b[128];
    std::snprintf(b, sizeof b, "the stream is centred on its target (centre miss %.2f)",
                  centre);
    check(centre < 0.5f, b);
    std::snprintf(b, sizeof b, "and no drop strays past its spread (worst %.2f, bound %.2f)",
                  worst, spread);
    check(worst < spread, b);
    RecordObserved("vesselPourCentreMiss", (double)centre);
    // OUT OF REACH -> TIPPED, not lobbed: looking at the horizon, the stream
    // leaves along the look at the gentle speed and lands a short way in
    // front -- the owner's report was a stream solved toward a point in
    // mid-air, which came down far off at an angle.
    {
      ItemStack tip = Vs(c.items, flaskI, 1, (uint16_t)mWater, 64);
      std::vector<ParticleSpawn> ts;
      const Vec3 far = mouth + Vec3{1000, 0, 0};
      ContainerPour(*flask, tip, mouth, Vec3{1, 0, 0}, &far, g, 600, 0x99u, ts,
                    nullptr);
      float worstLand = 0.0f;
      bool forward = !ts.empty();
      for (const ParticleSpawn& sp : ts) {
        int64_t px = sp.px, py = sp.py, pz = sp.pz, vx = sp.vx, vy = sp.vy, vz = sp.vz;
        for (int t = 0; t < 400 && py > (int64_t)base.y * 256; t++) {
          vy -= g;
          px += vx;
          py += vy;
          pz += vz;
        }
        const float dx = px / 256.0f - mouth.x;
        forward = forward && dx > 0.0f;
        worstLand = std::max(worstLand, dx);
      }
      char tb[128];
      std::snprintf(tb, sizeof tb,
                    "a pour past reach is tipped: lands %.1f cells ahead from %.0f up "
                    "(bound %.1f)", worstLand, 12.0f, flask->container.pourRange);
      check(forward && worstLand < flask->container.pourRange, tb);
    }
  }

  // ---- stacks --------------------------------------------------------------
  {
    Inventory hb;
    hb.slots[0] = Vs(c.items, flaskI, 1, (uint16_t)mWater, 40);
    const int where = hb.Add(Vs(c.items, flaskI, 1));
    check(where != 0 && hb.slots[0].count == 1,
          "an empty flask does not fold into a filled one");
    hb.slots[3] = Vs(c.items, flaskI, 3);
    Bag bag;
    check(ContainerIsolateOne(hb.slots, kItemSlots, 3, bag.slots, Bag::kSlots) &&
              hb.slots[3].count == 1,
          "filling one of a stack sets the others down");
    int rest = 0;
    for (const ItemStack& s : hb.slots)
      if (!s.Empty() && s.name == "flask" && !s.Filled()) rest += s.count;
    check(rest == 4, "and none of them is lost");
    // TWO IDENTICAL FILLS DO NOT STACK: a count-2 stack has ONE fillAmt, so a
    // merge would destroy a flask's worth on the first pour.
    Inventory h2;
    h2.slots[0] = Vs(c.items, flaskI, 1, (uint16_t)mWater, 40);
    const int w2 = h2.Add(Vs(c.items, flaskI, 1, (uint16_t)mWater, 40));
    check(w2 > 0 && h2.slots[0].count == 1 && h2.slots[w2].count == 1 &&
              h2.slots[w2].fillAmt == 40,
          "two flasks of the same fill stay two stacks");
    Bag b2;
    b2.slots[0] = Vs(c.items, flaskI, 1, (uint16_t)mWater, 40);
    check(b2.Add(Vs(c.items, flaskI, 1, (uint16_t)mWater, 40)) != 0,
          "and the pack keeps them apart too");
    check(h2.Add(Vs(c.items, flaskI, 2)) == h2.Add(Vs(c.items, flaskI, 1)),
          "empty flasks still stack");
    // No path charges or pays a whole stack as one vessel.
    ItemStack three = Vs(c.items, flaskI, 3);
    check(ContainerDeposit(*flask, three, (uint16_t)mWater, 64) == 0 &&
              !three.Filled(),
          "a scoop is never paid into a stack of three (it would triple)");
    ItemStack pair = Vs(c.items, flaskI, 2, (uint16_t)mWater, 64);
    std::vector<ParticleSpawn> none;
    std::vector<FluidSpawnOp> noneF;
    const Vec3 m0{(float)base.x, (float)base.y + 12.0f, (float)base.z};
    check(ContainerPour(*flask, pair, m0, Vec3{1, 0, 0}, nullptr,
                        CurrentTuning().sim.partGravity, 1, 1u, none, nullptr) == 0 &&
              ContainerPourFluid(*flask, pair, m0, Vec3{1, 0, 0}, nullptr, 1, 1u,
                                 4096, noneF, nullptr) == 0 &&
              ContainerSpend(pair, 1) == 0 && pair.fillAmt == 64,
          "a filled stack does not pour until one is set apart");
  }

  // ---- a pour lands exactly what it was charged ---------------------------
  // The kernel used to land EVERY liquid particle full: scoop an eighth of
  // lava, pour it, and a whole cell came down (scoop 8/8, repeat: unbounded).
  {
    const uint32_t liq = mLava ? mLava : mBlood;
    ItemStack st = Vs(c.items, flaskI, 1, (uint16_t)liq, 3);
    std::vector<ParticleSpawn> sp;
    const Vec3 m0{(float)base.x, (float)base.y + 12.0f, (float)base.z};
    const int n = ContainerPour(*flask, st, m0, Vec3{1, 0, 0}, nullptr,
                                CurrentTuning().sim.partGravity, 700, 3u, sp,
                                nullptr, 0xFFFFFFFFu, &c.mats);
    check(n == 1 && sp.size() == 1 && !st.Filled() &&
              (sp[0].flags & kPFlagMeasured) != 0 &&
              ((sp[0].payload >> 12) & 7u) == 2u,
          "3 eighths pour as ONE particle measured at fullness 3/8, not a cell");
    // A pouch with a partial cell (the portrait brush can leave one): the
    // whole cells fall as grains, the rest is dust -- never a whole grain.
    ItemStack sand = Vs(c.items, pouchI, 1, (uint16_t)mSand, 12);
    sp.clear();
    ContainerPour(*pouch, sand, m0, Vec3{1, 0, 0}, nullptr,
                  CurrentTuning().sim.partGravity, 701, 3u, sp, nullptr,
                  0xFFFFFFFFu, &c.mats);
    check(sp.size() == 1 && !sand.Filled() && (sp[0].flags & kPFlagMeasured) == 0,
          "a pouch's partial last cell is dust, not a grain");
    // THE RING'S ROOM: charged only for the particles that fit.
    ItemStack big = Vs(c.items, flaskI, 1, (uint16_t)liq, 64);
    sp.clear();
    const int got = ContainerPour(*flask, big, m0, Vec3{1, 0, 0}, nullptr,
                                  CurrentTuning().sim.partGravity, 702, 3u, sp,
                                  nullptr, 1u, &c.mats);
    check(got == 1 && sp.size() == 1 && big.fillAmt == 56,
          "a pour with room for one particle is charged for one");
    check(ContainerParticleRoom(false, 0, 0) == 0 &&
              ContainerParticleRoom(true, kParticleCap, 0) == 0 &&
              ContainerParticleRoom(true, 0, 0) ==
                  kParticleCap - kMaxParticleSpawnsPerTick * World::kSnapshotLatency &&
              ContainerParticleRoom(true, 1000, 10) + 1010 ==
                  ContainerParticleRoom(true, 0, 0),
          "the ring bound: snapshot count + the streams since + this tick's queue");
  }

  // ---- one ledger for every scooper ----------------------------------------
  // Two sessions' claims landing on one tick share the GPU's one counter: the
  // sum paid is what the tick removed, not each paid the whole delta.
  {
    ContainerScoopLedger L;
    ContainerLedgerObserve(L, 299, 1000, 0);
    ContainerScoopMemo a, b;
    a.claims.push_back({300, (uint16_t)mWater, 32});
    b.claims.push_back({300, (uint16_t)mWater, 32});
    ContainerLedgerObserve(L, 300, 1000 + 40, 2);   // the tick removed 40
    ContainerLedgerObserve(L, 300, 1000 + 40, 2);   // seen again: no refill
    ItemStack fa = Vs(c.items, flaskI, 1), fb = Vs(c.items, flaskI, 1);
    const ItemDef& fd = *flask;
    ContainerSettle(L, a, 300,
                    [&](uint16_t m, int u) { return ContainerDeposit(fd, fa, m, u); },
                    nullptr);
    ContainerSettle(L, b, 300,
                    [&](uint16_t m, int u) { return ContainerDeposit(fd, fb, m, u); },
                    nullptr);
    check(fa.fillAmt + fb.fillAmt == 40 && fa.fillAmt == 32,
          Format("two scoops on one tick are paid what the GPU removed, together "
                 "(%d + %d of 40)", fa.fillAmt, fb.fillAmt).c_str());
  }

  // ---- a scoop in flight when the flask leaves the hand --------------------
  // No vessel to pay: the eighths come back as UNPAID (the session spills
  // them), not into nothing. A vessel with less room: the excess likewise.
  {
    ContainerScoopMemo m;
    m.ledger.have = true;
    m.ledger.tick = 399;
    m.ledger.eighths = 0;
    m.claims.push_back({400, (uint16_t)mWater, 24});
    std::vector<ContainerUnpaid> unpaid;
    const int paid = ContainerSettle(m, 400, 24, flask, nullptr, &unpaid);
    check(paid == 0 && unpaid.size() == 1 && unpaid[0].units == 24 &&
              unpaid[0].mat == mWater && m.claims.empty(),
          "a scoop landing with no vessel is returned as unpaid, not dropped");
    ContainerScoopMemo m2;
    m2.ledger.have = true;
    m2.ledger.tick = 400;
    m2.ledger.eighths = 24;
    m2.claims.push_back({401, (uint16_t)mWater, 24});
    ItemStack nearly = Vs(c.items, flaskI, 1, (uint16_t)mWater, (uint16_t)(flask->container.capacity - 10));
    unpaid.clear();
    const int p2 = ContainerSettle(m2, 401, 48, flask, &nearly, &unpaid);
    check(p2 == 10 && nearly.fillAmt == flask->container.capacity &&
              unpaid.size() == 1 && unpaid[0].units == 14,
          "what a vessel has no room for is returned as unpaid");
  }

  // ---- throwing and breaking -----------------------------------------------
  {
    check(ContainerThrowable(*flask) && flask->container.breakSpeed > 0.0f,
          "the flask is thrown and breaks (items.json throwSpeedMps / breakSpeedMps)");
    const int full = (int)std::ceil(flask->container.throwChargeSec * 30.0f) + 1;
    check(ContainerThrowCharge(*flask, 1) == 0.0f &&
              ContainerThrowCharge(*flask, full) == 1.0f &&
              ContainerThrowCharge(*flask, full * 4) == 1.0f,
          "the wind-up runs 0..1 over throwChargeSec and holds at full");
    const float tap = ContainerThrowSpeed(*flask, 1),
                mid = ContainerThrowSpeed(*flask, full / 2),
                top = ContainerThrowSpeed(*flask, full);
    check(tap < mid && mid < top &&
              std::fabs(top - flask->container.throwSpeed) < 1e-3f,
          "a longer draw throws harder, up to throwSpeed");
    const float b = flask->container.breakSpeed;
    check(!ContainerShouldBreak(*flask, b * 0.9f, Vec3{0, -b * 0.5f, 0}) &&
              ContainerShouldBreak(*flask, b, Vec3{}) &&
              ContainerShouldBreak(*flask, 0.0f, Vec3{b, 0, 0}),
          "it breaks on a hard contact OR a hard velocity jump, not below");
    // A tick of free fall is a velocity jump of g/30: never a break.
    check(!ContainerShouldBreak(*flask, 0.0f,
                                Vec3{0, -MetresToCells(9.81f) / 30.0f, 0}),
          "free fall does not break it");

    // The spill: every eighth of water comes out as MPM fluid, at once.
    ContainerSpill sp;
    sp.at = Vec3{10, 20, 30};
    sp.vel = Vec3{MetresToCells(10.0f), 0, 0};
    sp.away = Vec3{-1, 0, 0};
    sp.mat = (uint16_t)mWater;
    sp.units = 1024;
    sp.seed = 7;
    std::vector<FluidSpawnOp> fl;
    std::vector<ParticleSpawn> pa;
    SplatterEvent ev;
    int got = ContainerSpillStep(sp, c.mats, 100, kMaxFluidSpawnsPerTick, fl, pa, &ev);
    check(got == 1024 && sp.units == 0 && (int)fl.size() == 1024 && pa.empty(),
          "a broken flask of water lets out all 1024 eighths as fluid in one tick");
    check(sp.splatted && ev.mat == mWater && ev.count == 128,
          "and splatters whoever it broke over");
    // Under a short budget the rest waits, it is not lost.
    sp = ContainerSpill{};
    sp.at = Vec3{10, 20, 30};
    sp.mat = (uint16_t)mWater;
    sp.units = 1024;
    fl.clear();
    got = ContainerSpillStep(sp, c.mats, 100, 300, fl, pa, &ev);
    const int got2 = ContainerSpillStep(sp, c.mats, 101, 4096, fl, pa, &ev);
    check(got == 300 && got2 == 724 && sp.units == 0,
          "a spill the budget cannot hold drains next tick, conserved");
    // Sand (not fluid) leaves as grid particles, a whole cell each; the part
    // cell left over cannot be a grain and is dust -- never rounded UP.
    sp = ContainerSpill{};
    sp.at = Vec3{10, 20, 30};
    sp.mat = (uint16_t)mSand;
    sp.units = 20;
    fl.clear();
    got = ContainerSpillStep(sp, c.mats, 100, 4096, fl, pa, &ev);
    bool pOk = pa.size() == 2;
    for (const ParticleSpawn& p : pa)
      pOk = pOk && (p.payload & 0xFFFu) == mSand && !(p.flags & kPFlagMicro) &&
            !(p.flags & kPFlagMeasured);
    check(got == 20 && sp.units == 0 && fl.empty() && pOk,
          "a broken pouch of sand is whole-cell grid particles, never rounded up");
    // A liquid the seam cannot hold (lava/blood-like: grid particles) lands
    // at EXACTLY what it held: 20 eighths = 8 + 8 + 4, the last MEASURED at
    // fullness code 3 -- not three full cells.
    if (mLava) {
      sp = ContainerSpill{};
      sp.at = Vec3{10, 20, 30};
      sp.mat = (uint16_t)mLava;
      sp.units = 20;
      pa.clear();
      fl.clear();
      got = ContainerSpillStep(sp, c.mats, 100, 4096, fl, pa, &ev);
      int landed = 0;
      bool meas = pa.size() == 3;
      for (const ParticleSpawn& p : pa) {
        meas = meas && (p.flags & kPFlagMeasured) != 0;
        landed += (int)((p.payload >> 12) & 7u) + 1;
      }
      check(got == 20 && meas && landed == 20,
            Format("a broken flask of lava lands exactly its 20 eighths (%d)",
                   landed).c_str());
      // ...and the ring-room bound holds it back rather than charging it.
      sp.units = 20;
      pa.clear();
      got = ContainerSpillStep(sp, c.mats, 101, 4096, fl, pa, &ev, 1);
      check(got == 8 && sp.units == 12 && pa.size() == 1,
            "a spill charges only the particles the ring has room for");
    }
  }

  // ---- persistence ---------------------------------------------------------
  {
    GlyphLibrary glyphs;
    PlayerCaster caster;
    Kit kit;
    Inventory& hb = kit.hotbar;
    hb.slots[4] = Vs(c.items, flaskI, 1, (uint16_t)mBlood, 77);
    kit.bag.slots[2] = Vs(c.items, pouchI, 1, (uint16_t)mSand, 16);
    PlayerKitRefs refs{&caster, &glyphs, &kit, &c.items};
    EntityIO io = MakeEntityIO(c.debris, c.mobs, nullptr, &refs);
    const EntitySection* plyr = nullptr;
    for (const EntitySection& s : io.sections)
      if (s.id == (uint32_t)('P' | ('L' << 8) | ('Y' << 16) | ((uint32_t)'R' << 24)))
        plyr = &s;
    check(plyr != nullptr, "PLYR is registered");
    if (plyr) {
      std::vector<uint8_t> bytes;
      plyr->save(bytes);
      plyr->reset();
      check(plyr->load(bytes.data(), bytes.size(), kPlayerKitSaveVersion),
            "the kit loads back");
      check(hb.slots[4].fillMat == mBlood && hb.slots[4].fillAmt == 77,
            "a hotbar flask keeps its blood across a save");
      check(kit.bag.At(2).fillMat == mSand && kit.bag.At(2).fillAmt == 16,
            "a pouch in the pack keeps its sand");
      std::vector<uint8_t> v5;
      SavePlayerKit(refs, v5, 5);
      plyr->reset();
      check(plyr->load(v5.data(), v5.size(), 5) && !hb.slots[4].Filled() &&
                hb.slots[4].name == "flask",
            "a v5 payload loads with the flask empty");
    }
  }

  // ---- the health panel's pour ---------------------------------------------
  {
    IdCounterScope idScope(c.mobs);
    const int def = c.mobs.FindDef("human");
    const uint64_t id = def >= 0 ? c.mobs.Spawn(def, {base.x, base.y + 40, base.z}) : 0;
    check(id != 0, "a human spawns");
    const Mob* m = id ? c.mobs.FindMobById(id) : nullptr;
    int limb = -1;
    if (m)
      for (int i = 0; i < m->LimbCount() && limb < 0; i++)
        if (c.mobs.LimbHp(id, i) > 0.0f) limb = i;
    if (m && limb >= 0) {
      uint32_t marked = 0;
      const uint32_t did = c.mobs.DouseLimb(id, limb, mBlood, 8, 1000, &marked);
      const LimbCoat* lc = c.mobs.LimbCoatOf(id, limb);
      check(marked > 0 && lc && lc->top[0].mat == mBlood,
            "pouring blood on a limb coats it in blood");
      check(did == 0, "blood is no remedy (no coat.effects)");
      bool others = false;
      for (int i = 0; i < m->LimbCount(); i++)
        if (i != limb)
          if (const LimbCoat* o = c.mobs.LimbCoatOf(id, i))
            others = others || o->stained > 0;
      check(!others, "and no other limb");
      // WATER WASHES, THEN WETS (bodystain.h WashBodyStain). A pour of 8
      // rinses 16 levels off the blood -- all of it -- and leaves the limb
      // wet, not bloody: Raise would have left 8-deep blood where it was.
      c.mobs.DouseLimb(id, limb, mWater, 8, 1001, &marked);
      lc = c.mobs.LimbCoatOf(id, limb);
      bool bloodLeft = false;
      if (lc)
        for (const CoatEntry& en : lc->top)
          bloodLeft = bloodLeft || (en.mat == mBlood && en.sumAmt > 0);
      check(lc && lc->top[0].mat == mWater && !bloodLeft,
            "pouring water over the blood washes it off and leaves the limb wet");
    } else {
      check(false, "the human has a live limb to pour on");
    }
    c.mobs.Reset();
    c.debris.Reset();
  }

  // ---- the portrait pour BRUSH (MobSystem::PickBody / PourOnBody) ----------
  // A ray at the body from in front, as the portrait camera casts one: it has
  // to meet the skin, and the coat has to land as a PATCH on the face it met
  // -- a disc of cells, not a core through the limb -- and reach the brick
  // the renderer draws.
  {
    IdCounterScope idScope(c.mobs);
    const int def = c.mobs.FindDef("human");
    const uint64_t id = def >= 0 ? c.mobs.Spawn(def, {base.x, base.y + 40, base.z}) : 0;
    const Mob* m = id ? c.mobs.FindMobById(id) : nullptr;
    check(m != nullptr, "a human spawns for the brush");
    if (m) {
      const Vec3 at = m->RootWorldPos();
      Vec3 face = m->Facing();
      face.y = 0.0f;
      face = face.len() > 0.1f ? face.normalized() : Vec3{0, 0, 1};
      const Vec3 ro = at + face * 60.0f;
      const Vec3 rd = face * -1.0f;
      const auto pickT0 = std::chrono::steady_clock::now();
      const MobSystem::BodyRayHit hit = c.mobs.PickBody(id, ro, rd, 200.0f);
      const double pickMs = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - pickT0).count();
      check(hit.hit && hit.limb >= 0 && hit.t > 40.0f && hit.t < 60.0f,
            "a ray from in front meets the skin");
      if (hit.hit) {
        const float r = 1.5f;
        uint32_t marked = 0, drawn = 0;
        c.mobs.PourOnBody(id, hit, rd, r, mBlood, 3, 1000, &marked, &drawn);
        uint32_t stainedAll = 0, scale = 1;
        for (int i = 0; i < m->LimbCount(); i++)
          if (const LimbCoat* o = c.mobs.LimbCoatOf(id, i)) stainedAll += o->stained;
        if (m->Def()) scale = std::max(1u, m->Def()->skinScale);
        // The disc's cells at the finest pitch; the surface under it is at
        // most a couple of cells deep, a core through a limb is dozens.
        const float disc = 3.14159f * (r * scale) * (r * scale);
        std::printf("  vessel brush: limb %d at t=%.1f, %u cells coated (disc %.0f), "
                    "%u drawn, micro set %s, pick %.3f ms\n", hit.limb, hit.t,
                    marked, disc, drawn, c.mobs.MicroSet() ? "yes" : "no", pickMs);
        check(marked > 0 && stainedAll == marked, "the brush coats cells");
        check((float)marked <= disc * 3.0f,
              Format("only the surface under the disc (%u cells, disc %.0f)",
                     marked, disc).c_str());
        check(drawn == marked || !c.mobs.MicroSet(),
              Format("every coated cell reaches the drawn brick (%u of %u)",
                     drawn, marked).c_str());
        // The drain follows the brush: applyCells a second at the default
        // size, four times that at twice the radius.
        const float d1 = PourBrushCellsPerSec(*flask, kPourBrushRefRadius);
        const float d2 = PourBrushCellsPerSec(*flask, kPourBrushRefRadius * 2.0f);
        check(std::fabs(d1 - (float)flask->container.applyCells) < 1e-3f &&
                  std::fabs(d2 - 4.0f * d1) < 1e-3f,
              "a brush twice as wide drains four times as fast");
        // Held: the same spot deepens rather than spreading.
        uint32_t again = 0;
        c.mobs.PourOnBody(id, hit, rd, r, mBlood, 3, 1001, &again);
        uint32_t stained2 = 0;
        for (int i = 0; i < m->LimbCount(); i++)
          if (const LimbCoat* o = c.mobs.LimbCoatOf(id, i)) stained2 += o->stained;
        check(again > 0 && stained2 == stainedAll,
              "holding the brush deepens the same patch");
      }
    }
    c.mobs.Reset();
    c.debris.Reset();
  }

  const bool ok = failed == 0;
  detail = ok ? Format("%d checks", checks)
              : Format("%d/%d checks failed; first: %s", failed, checks, first.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// vessel-grid: the same scoop and pour through the REAL grid
// ---------------------------------------------------------------------------
//
// `vessel` proves the arithmetic against a map. This proves the two halves
// that only the GPU can: sim_mutate.wgsl's conditional clear really empties
// the cells it names (and only those), and a poured stream really reinserts
// as water. One stone basin in the sky at the top of the window, a pool in
// it, a flask held over it for 40 ticks, then poured back.
//
//   * WHAT LEFT THE GRID IS WHAT THE FLASK HOLDS. Water eighths in the whole
//     3x3x3 mirror before minus after equals the flask's credit, give or take
//     one cell (a settled pool's levelling can move an eighth under a clear
//     between snapshot and apply; the conditional refuses rather than
//     deleting, so the drift is bounded by what is in motion).
//   * WHAT IS POURED COMES BACK AS WATER. After emptying the flask and
//     letting the stream land, the mirror holds the flask's worth again, less
//     nothing and plus nothing (the last partial cell lands MEASURED).
//
// vessel-mpm: the same round trip the way the game now does it for water --
// the scoop's cells fly into the flask as GHOST MPM particles
// (ContainerScoopStream) and the pour leaves as MPM fluid (ContainerPour-
// Fluid). Two claims on top of vessel-grid's: the ghosts are a picture and
// nothing else (they existed, they are gone, and the scoop audit is still
// exact -- a ghost that settled or reacted would show up as water the flask
// never paid for), and the fluid pour is EXACT, one particle per eighth, with
// no last-drop rounding.
Status VesselRoundTrip(Ctx& c, std::string& detail, bool mpm) {
  auto matId = [&](const char* n) -> uint32_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == n) return (uint32_t)i;
    return 0;
  };
  const uint32_t mWater = matId("water"), mStone = matId("stone");
  const int flaskI = c.items.Find("flask");
  const ItemDef* flask = c.items.At(flaskI);
  if (!flask || !mWater || !mStone) {
    detail = "flask, water or stone missing";
    return Status::Fail;
  }
  // PIN A DIM DAWN, as every liquid mass audit here does (selftest_ca.cpp):
  // evaporation and freezing are authored mass sinks (reactions.json, `when:
  // day` / night) and an audit that runs through either one is inexact. Not
  // what this fixture's first "loss" turned out to be -- that was the MPM
  // excursion counted below -- but a sunlit film on a sky tray is exactly
  // what evaporation eats, and the audit should not depend on the clock.
  Tuning dawn = CurrentTuning();
  dawn.dayNight.freeze = 1;
  dawn.dayNight.freezePhase = (int)(kDaySunrise + 1024u);
  const Tuning savedTuning = CurrentTuning();
  SetCurrentTuning(dawn);
  struct Restore {
    const Tuning& t;
    ~Restore() { SetCurrentTuning(t); }
  } restore{savedTuning};

  // A CLEAN WINDOW FIRST. Gates share one World (selftest.cpp kOrder), and
  // measured in one process after `determinism` this fixture read a starting
  // pool of 8,142 eighths and 3,200 live particles that were not its own --
  // the count below is the whole 3x3x3 box plus the WORLD's MPM population.
  // Standalone it read 144. So: regenerate, then empty both particle
  // populations the way the dev panel's "clear fluid" does (the MPM live word)
  // and the harness does at a tick's head (the ballistic count pages).
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  {
    const uint32_t zeros[2] = {0u, 0u};
    c.ctx.queue.WriteBuffer(c.world.particleCounts, 0, zeros, sizeof zeros);
    const uint32_t zero = 0u;
    c.ctx.queue.WriteBuffer(c.world.fluidArgsStage, 7 * 4, &zero, 4);
    c.ctx.WaitIdle();
  }

  // High in the window: sky, so nothing the terrain does reaches the basin.
  const IVec3 o = c.world.WindowOrigin();
  const IVec3 base{o.x * (int)kChunk + (int)kWorldN / 2 + 8,
                   o.y * (int)kChunk + (int)kWorldN - 40,
                   o.z * (int)kChunk + (int)kWorldN / 2 + 8};
  const IVec3 chunk{base.x >> 4, base.y >> 4, base.z >> 4};
  uint32_t tick = 71000;
  uint32_t fluidSpawned = 0;   // conservative live bound for the MPM dispatch
  auto step = [&](const std::vector<CellOp>& cells,
                  const std::vector<ParticleSpawn>& spawns = {},
                  const std::vector<FluidSpawnOp>& fluid = {}) {
    std::vector<BrushOp> ops;
    fluidSpawned += (uint32_t)fluid.size();
    SubmitTick(c.ctx, c.world, c.sim, tick++, kDefaultSeed, ops, {}, cells, false,
               chunk, true, true, spawns, 0, fluid,
               std::min(kFluidCap, c.world.Snap().fluidLive + fluidSpawned));
    c.ctx.WaitIdle();
    c.ctx.ProcessEvents();
  };
  // Counted off the GPU grid directly, not the CPU mirror: the mirror is
  // re-read only for chunks the CPU believes dirty, and a particle landing is
  // a GPU-decided write it may not know about yet. The 3x3x3 chunks round the
  // basin, read synchronously -- a gate may; the frame path may not.
  auto mirrorWater = [&]() {
    long total = 0;
    std::vector<uint32_t> buf(kChunkVol);
    for (int cz = -1; cz <= 1; cz++)
      for (int cy = -1; cy <= 1; cy++)
        for (int cx = -1; cx <= 1; cx++) {
          const IVec3 cc{(chunk.x + cx) * (int)kChunk, (chunk.y + cy) * (int)kChunk,
                         (chunk.z + cz) * (int)kChunk};
          if (!c.world.CellInWindow(cc)) continue;
          ReadVoxelsSync(c.ctx, c.world, World::SlotCellIndex(cc) / kChunkVol, 1,
                         buf.data(), mpm ? "vessel-mpm" : "vessel-grid");
          for (uint32_t w : buf)
            if ((w & 0xFFFu) == mWater) total += ContainerCellUnits(w, c.mats[mWater]);
        }
    // ...PLUS THE WATER THAT IS NOT IN THE GRID. The MPM seam excites a
    // splashed surface into fluid particles (8 per cell at rest density, so
    // one particle is one eighth) and settles them back later; measured, a
    // landed pour read grid 112 + mpm 8 at tick 40 and grid 100 + mpm 20 at
    // tick 80 -- the same 120 both times. Counting the grid alone reads that
    // excursion as a loss. No other MPM source runs in this fixture.
    total += (long)c.world.Snap().fluidLive;
    return total;
  };
  // THE BASIN: a 7x7 stone floor with walls four high, and 3x3x2 of full water
  // in the middle of it -- standing on a 25x25 CATCH TRAY with a rim. The
  // tray is not decoration: sim.windMode drags every particle in flight
  // (sim_particle.wgsl), and measured here a stream poured from ten cells up
  // put most of its drops beside a 5x5 basin. In the sky with no tray those
  // fell 400 cells out of the counted box and read as "the pour loses water".
  // The pour's AIM is the pure gate's claim (no wind there); this one's is
  // CONSERVATION, so the fixture catches whatever the wind does.
  {
    std::vector<CellOp> cells;
    constexpr int kTray = 12;
    for (int dz = -kTray; dz <= kTray; dz++)
      for (int dx = -kTray; dx <= kTray; dx++)
        for (int dy = -1; dy <= 3; dy++) {
          const IVec3 p{base.x + dx, base.y + dy, base.z + dz};
          const int ring = std::max(std::abs(dx), std::abs(dz));
          const bool tray = dy == -1 || (ring == kTray && dy <= 1);
          const bool shell = ring <= 3 && (dy == -1 || ring == 3);
          const bool pool = ring <= 1 && dy >= 0 && dy <= 1;
          if (tray || shell)
            cells.push_back({World::SlotCellIndex(p), PackVoxNew(mStone, 0u)});
          else if (pool)
            cells.push_back({World::SlotCellIndex(p), PackVoxNew(mWater, 7u)});
        }
    step(cells);
  }
  for (int i = 0; i < 40; i++) step({});   // let it level and the snapshot catch up
  const long w0 = mirrorWater();

  // THE SCOOP, aimed each tick at the highest water the snapshot shows -- the
  // cell the pick ray would strike looking down into the basin.
  ItemStack st = Vs(c.items, flaskI, 1);
  ContainerScoopMemo memo;
  const Vec3 mouth{base.x + 0.5f, base.y + 10.5f, base.z + 0.5f};
  uint32_t ghosts = 0, ghostPeak = 0;
  for (int i = 0; i < 40; i++) {
    IVec3 aim{0, 0, 0};
    bool found = false;
    for (int dy = 3; dy >= 0 && !found; dy--)
      for (int dz = -2; dz <= 2 && !found; dz++)
        for (int dx = -2; dx <= 2 && !found; dx++) {
          const IVec3 p{base.x + dx, base.y + dy, base.z + dz};
          uint32_t w = 0;
          if (ContainerSnapWord(c.world, p, w) && (w & 0xFFFu) == mWater) {
            aim = p;
            found = true;
          }
        }
    std::vector<CellOp> cells;
    std::vector<FluidSpawnOp> stream;
    if (found) {
      ContainerScoop(*flask, st, aim,
                     [&](IVec3 p, uint32_t& w) { return ContainerSnapWord(c.world, p, w); },
                     c.world, c.mats, cells, nullptr, &memo, tick);
      if (mpm)
        for (const ContainerScoopMemo::Taken& t : memo.cells)
          if (t.tick == tick)
            ghosts += ContainerScoopStream(t.c, mWater, mouth, 8, 0x5Cu, tick,
                                           4096, stream);
    }
    step(cells, {}, stream);
    const WorldSnapshot& sn = c.world.Snap();
    if (sn.valid) ContainerSettle(memo, sn.tick, sn.scoopEighths, flask, &st);
    ghostPeak = std::max(ghostPeak, sn.fluidLive);
  }
  for (int i = 0; i < 12; i++) {
    step({});
    const WorldSnapshot& sn = c.world.Snap();
    if (sn.valid) ContainerSettle(memo, sn.tick, sn.scoopEighths, flask, &st);
  }
  const long w1 = mirrorWater();
  const uint32_t liveAfterScoop = c.world.Snap().fluidLive;
  const long taken = w0 - w1;

  // THE POUR: from ten cells above the basin's middle, onto it.
  const int held = st.fillAmt;
  const Vec3 target{base.x + 0.5f, base.y + 0.5f, base.z + 0.5f};
  for (int i = 0; i < 200 && st.Filled(); i++) {
    std::vector<ParticleSpawn> spawns;
    std::vector<FluidSpawnOp> fluid;
    if (mpm)
      ContainerPourFluid(*flask, st, mouth, Vec3{0, -1, 0}, &target, tick, 0x77u,
                         4096, fluid, nullptr);
    else
      ContainerPour(*flask, st, mouth, Vec3{0, -1, 0}, &target,
                    CurrentTuning().sim.partGravity, tick, 0x77u, spawns, nullptr);
    step({}, spawns, fluid);
  }
  // The landing, over time: a count that PEAKS and then falls is the liquid
  // CA thinning a film, one that never arrives is drops lost in flight.
  std::string timeline;
  for (int i = 1; i <= 80; i++) {
    step({});
    if (i == 5 || i == 10 || i == 20 || i == 40 || i == 80)
      timeline += Format(" +%d:%ld", i, mirrorWater() - w1);
  }
  const long w2 = mirrorWater();
  const uint32_t stillFlying = c.world.Snap().particleCount;
  const long back = w2 - w1;

  // Put the world back for whoever runs next.
  c.mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  const char* key = mpm ? "vesselMpm" : "vesselGrid";
  RecordObserved((std::string(key) + "Taken").c_str(), (double)taken);
  RecordObserved((std::string(key) + "Held").c_str(), (double)held);
  RecordObserved((std::string(key) + "Back").c_str(), (double)back);
  // EXACT: the flask is paid from the GPU's own ledger. With ghosts in the
  // stream this is also the proof they are not matter: they sat in the live
  // pool while they flew (ghostPeak), and one that settled or outlived its
  // death tick would read here as water the flask never paid for.
  const bool scoopOk = held > 0 && taken == held && memo.claims.empty() &&
                       (!mpm || (ghosts > 0 && ghostPeak > 0));
  // Everything poured is on the tray, EXACTLY, both roads: the fluid pour is
  // one particle per eighth, and the grid pour's last particle is MEASURED
  // (kPFlagMeasured) and lands at the fullness it was charged -- it used to
  // land full, which minted up to 7/8 of a cell per emptying.
  const bool pourOk = !st.Filled() && stillFlying == 0 && back == held;
  detail = Format("pool %ld/8 -> flask credited %d, grid lost %ld; poured back, "
                  "grid gained %ld (%u particles still live; by tick%s)",
                  w0, held, taken, back, stillFlying, timeline.c_str());
  detail += Format("; mpm live after scoop %u", liveAfterScoop);
  if (mpm) detail += Format("; %u ghost particles, live peak %u", ghosts, ghostPeak);
  return scoopOk && pourOk ? Status::Pass : Status::Fail;
}

Status GateVesselGrid(Ctx& c, std::string& detail) {
  return VesselRoundTrip(c, detail, false);
}
Status GateVesselMpm(Ctx& c, std::string& detail) {
  return VesselRoundTrip(c, detail, true);
}

// ---------------------------------------------------------------------------
// vessel-break: a thrown flask breaks, a set-down one does not, a struck one
// does -- through real Jolt contacts and the same ContainerBreakPass the tick
// runs (session.cpp phase H).
// ---------------------------------------------------------------------------
//
// Three flasks of water on a stone table (the audio-impact fixture's slab):
//   A  set down from two voxels up: must NOT break, and must still hold its
//      water when it has settled. This is the false-positive half, and the
//      one that matters most -- a flask that breaks when you drop it is not
//      a flask.
//   B  thrown straight down at full draw: must break, and its spill must be
//      the whole flask (1024 eighths of water).
//   C  resting on the table, then struck by a stone block flying at it: must
//      break -- "is hit by something with a high velocity".
Status GateVesselBreak(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  Physics& phys = c.phys;
  DebrisSystem& debris = c.debris;
  uint32_t mWater = 0;
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == "water") mWater = (uint32_t)i;
  const ItemDef* flask = c.items.At(c.items.Find("flask"));
  if (!flask || !mWater) {
    detail = "flask or water missing";
    return Status::Fail;
  }

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  debris.Reset();
  WorldItems ground;
  debris.SetOnBodyGone([&ground](uint64_t h) { ground.OnBodyGone(h); });
  struct Unhook {
    DebrisSystem& d;
    ~Unhook() { d.SetOnBodyGone(nullptr); }
  } unhook{debris};

  const int px = 100, pz = 100;
  const int slabY = World::TerrainHeight(px, pz, kDefaultSeed) + 6;
  uint32_t t = 3000;
  uint64_t aBody = 0;
  int aGone = -1;
  const char* aHow = "";
  Vec3 aWhere{};
  std::vector<std::pair<uint64_t, Vec3>> lastVel;
  std::vector<ContainerSpill> spills;
  auto tick = [&](std::vector<CellOp> cellOps) {
    std::vector<ParticleSpawn> spawns;
    debris.PreTick(t + 1, world, cellOps, spawns);
    ++t;
    SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, cellOps, false,
               {px / 16, slabY / 16, pz / 16}, true, false, spawns);
    ctx.WaitIdle();
    ctx.ProcessEvents();
    phys.Step(kTickDt);
    debris.PostStep();
    const size_t sp0 = spills.size();
    const int n = ContainerBreakPass(ground, c.items, phys, debris, lastVel, spills);
    // WHEN A WENT, AND HOW (rule 6): the break pass taking it leaves a spill
    // at its position; anything else (a cull) leaves none.
    if (aBody && aGone < 0 && !ground.Find(aBody)) {
      aGone = (int)(t - 3000);
      aHow = n > 0 && spills.size() > sp0 ? "broken" : "removed without a break";
      if (spills.size() > sp0) aWhere = spills.back().at;
    }
    return n;
  };
  {
    std::vector<CellOp> pad;
    for (int z = -10; z <= 10; z++)
      for (int x = -10; x <= 10; x++)
        for (int y = slabY - 3; y <= slabY; y++)
          pad.push_back(
              {World::SlotCellIndex({px + x, y, pz + z}), (uint32_t)kMatStone});
    tick(pad);
  }
  for (int i = 0; i < 24; i++) tick({});

  ItemInstance full1024{flask->name};
  full1024.fillMat = (uint16_t)mWater;
  full1024.fillAmt = 1024;
  auto drop = [&](Vec3 at, Vec3 vel) {
    return DropItemToWorld(*flask, full1024, at, vel, phys, debris, nullptr,
                           ground);
  };
  const float top = (float)slabY + 1.0f;
  bool ok = true;
  std::string why;
  auto fail = [&](const std::string& w) {
    ok = false;
    if (why.empty()) why = w;
  };

  // ---- A: set down gently ----------------------------------------------------
  // Well clear of the rock C is struck by, which flies along -z of C.
  const uint64_t a = drop(Vec3{(float)px - 7, top + 2.0f, (float)pz + 7}, Vec3{});
  aBody = a;
  const int aT0 = (int)(t - 3000);
  int brokeA = 0;
  for (int i = 0; i < 60; i++) brokeA += tick({});
  const WorldItem* wa = ground.Find(a);
  if (!a) fail("A: the flask did not become a body");
  else if (brokeA || !wa) fail("A: a flask SET DOWN from two voxels broke");
  else if (wa->fillAmt != 1024) fail("A: the set-down flask lost its water");

  // ---- B: thrown down hard ---------------------------------------------------
  spills.clear();
  const float full = flask->container.throwSpeed;
  const uint64_t b = drop(Vec3{(float)px + 5, top + 14.0f, (float)pz + 5},
                          Vec3{0, -full, 0});
  int brokeB = -1;
  for (int i = 0; i < 40 && brokeB < 0; i++)
    if (tick({}) > 0 && !ground.Find(b)) brokeB = i;
  int spilledB = 0;
  for (const ContainerSpill& sp : spills)
    if (sp.mat == mWater) spilledB += sp.units;
  if (!b) fail("B: the flask did not become a body");
  else if (brokeB < 0) fail("B: a flask thrown down at full draw did not break");
  else if (spilledB != 1024) fail("B: the broken flask did not spill all 1024 eighths");

  // ---- C: struck where it lies -----------------------------------------------
  spills.clear();
  const Vec3 cAt{(float)px + 5, top + 1.0f, (float)pz - 5};
  const uint64_t cf = drop(cAt, Vec3{});
  int brokeC0 = 0;
  for (int i = 0; i < 45; i++) brokeC0 += tick({});
  BodyTransform cx{};
  const bool cLive = ground.Find(cf) && phys.GetTransform(cf, cx);
  if (!cf || !cLive || brokeC0) fail("C: the target flask did not settle intact");
  int brokeC = -1;
  if (cLive) {
    std::vector<DebrisVoxel> vox;
    for (int8_t z = 0; z < 3; z++)
      for (int8_t y = 0; y < 3; y++)
        for (int8_t x = 0; x < 3; x++)
          vox.push_back(DebrisVoxel{x, y, z, 0, (uint16_t)kMatStone});
    std::vector<float> density(c.mats.size(), 1000.0f);
    for (size_t i = 0; i < c.mats.size(); i++)
      density[i] = std::max(1.0f, (float)c.mats[i].gpu.density);
    // Level with the flask, eight voxels off along x, flying at it.
    const Vec3 rAt{cx.pos.x - 9.0f, cx.pos.y, cx.pos.z - 1.0f};
    const uint64_t rock =
        phys.CreateDebrisBody(vox, {(int)rAt.x, (int)rAt.y, (int)rAt.z}, density);
    if (rock) {
      BodyTransform rx{};
      rx.pos = Vec3{(float)(int)rAt.x, (float)(int)rAt.y, (float)(int)rAt.z};
      rx.quat[3] = 1;
      debris.AdoptBody(rock, vox, rx);
      phys.SetBodyVelocity(rock, Vec3{MetresToCells(15.0f), MetresToCells(1.0f), 0});
      for (int i = 0; i < 30 && brokeC < 0; i++)
        if (tick({}) > 0 && !ground.Find(cf)) brokeC = i;
    }
    if (!rock) fail("C: Jolt refused the rock");
    else if (brokeC < 0) fail("C: a flask struck by a flying stone did not break");
  }

  // ---- D: thrown out of a body ---------------------------------------------
  // The player's throw (session.cpp): launched from the hand, i.e. inside the
  // capsule proxy AND inside the thrower's own limbs, which are kinematic
  // bodies on Layers::AVATAR. Released `thrown`, it must fly clear of both
  // unbroken. The CONTROL arm is the old release (plain AVATAR layer, which
  // collides with AVATAR): reported, not asserted -- it is the bug, and what
  // it does is the diagnosis, not a contract.
  int brokeD = -1, brokeCtl = -1;
  float flewD = 0.0f;
  {
    const Vec3 pc{(float)px - 5, top + 12.0f, (float)pz - 4};
    const uint64_t proxy = phys.CreatePlayerBody(Player::kHalfXZ, Player::kHalfY);
    std::vector<DebrisVoxel> arm;
    for (int8_t z = 0; z < 3; z++)
      for (int8_t y = 0; y < 3; y++)
        for (int8_t x = 0; x < 3; x++)
          arm.push_back(DebrisVoxel{x, y, z, 0, (uint16_t)kMatStone});
    auto throwArm = [&](bool thrown, float* flew) {
      BodyTransform ax{};
      ax.pos = pc + Vec3{-1.5f, 0.0f, -1.5f};
      ax.quat[3] = 1;
      const uint64_t limb =
          phys.CreateDebrisBodyXf(arm, ax, debris.DensityOf(), true);
      if (!limb) return -2;
      phys.SetBodyKinematic(limb, true);
      phys.SetBodyAvatarLayer(limb, true);
      const Vec3 vel{0, full * 0.25f, -full};
      const uint64_t f = drop(pc - Vec3{0.5f, 0.5f, 0.5f}, vel);
      if (!f) {
        phys.RemoveBody(limb);
        return -2;
      }
      phys.ReleaseToWorldWhenClear(f, thrown);
      BodyTransform f0{};
      phys.GetTransform(f, f0);
      int broke = -1;
      for (int i = 0; i < 5 && broke < 0; i++) {
        if (proxy) phys.MovePlayerBody(proxy, pc, kTickDt);
        phys.MoveKinematicBody(limb, ax.pos, ax.quat, kTickDt);
        if (tick({}) > 0 && !ground.Find(f)) broke = i;
      }
      BodyTransform f1{};
      if (flew && broke < 0 && phys.GetTransform(f, f1))
        *flew = (f1.pos - f0.pos).len();
      phys.RemoveBody(limb);
      if (ground.Find(f)) debris.DestroyBody(f);
      return broke;
    };
    brokeCtl = throwArm(false, nullptr);
    brokeD = throwArm(true, &flewD);
    if (proxy) phys.RemoveBody(proxy);
    // Five ticks at full draw is ~2 m; a flask that flew a third of that
    // left the hand at speed rather than being caught in it.
    if (brokeD == -2) fail("D: Jolt refused the arm or the flask");
    else if (brokeD >= 0)
      fail(Format("D: a flask thrown out of the thrower's own arm broke at +%d",
                  brokeD));
    else if (flewD < full * kTickDt * 5.0f / 3.0f)
      fail(Format("D: the thrown flask was held back (flew %.1f vox in 5 ticks)",
                  flewD));
  }

  // A must survive the WHOLE fixture, not only its own settle: B landing and
  // C being struck nearby must not take it with them.
  if (aGone >= 0)
    fail(Format("A: the set-down flask went at +%d ticks after its drop (%s, "
                "at %.1f,%.1f,%.1f)", aGone - aT0, aHow, aWhere.x, aWhere.y,
                aWhere.z));
  detail = Format("A intact throughout: %s; B broke at +%d (spill %d/1024); "
                  "C struck, broke at +%d; D thrown from an arm: %s (flew "
                  "%.1f vox), old AVATAR release %s",
                  aGone < 0 ? "yes" : "no", brokeB, spilledB, brokeC,
                  brokeD == -1 ? "intact" : "BROKE", flewD,
                  brokeCtl >= 0 ? Format("broke at +%d", brokeCtl).c_str()
                                : "intact");
  if (!ok) detail = why + " -- " + detail;
  std::printf("vessel-break: %s\n", detail.c_str());
  // Leave nothing behind but a regenerated world: the bodies and the slab.
  debris.Reset();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& VesselGates() {
  static const std::vector<Gate> g = {
      // No deps: a hand-built grid and one spawned human it cleans up.
      {"vessel", "player", {}, false, GateVessel},
      // Builds in the sky and regenerates the world after itself.
      {"vessel-grid", "player", {}, false, GateVesselGrid},
      // The same, the way the game pours water: MPM out, ghost stream in.
      {"vessel-mpm", "player", {}, false, GateVesselMpm},
      // Real Jolt on a stone table; resets debris and regenerates on entry.
      {"vessel-break", "player", {}, false, GateVesselBreak},
  };
  return g;
}

}  // namespace selftest
