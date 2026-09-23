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
//   * STACKS: a filled flask never merges with empty ones, and filling one of
//     a stack sets the rest down first.
//   * PERSISTENCE: the fill round-trips through PLYR v6.
//   * THE HEALTH PANEL'S POUR: MobSystem::DouseLimb coats exactly the limb it
//     is given with the substance poured.

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
#include "game/persist.h"
#include "game/spell.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

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
    ItemStack st{pouchI, 1};
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
    ItemStack st{flaskI, 1};
    std::vector<CellOp> ops;
    const char* why = nullptr;
    ContainerScoop(*flask, st, {base.x + 3, base.y, base.z}, wordAt, c.world,
                   c.mats, ops, &why);
    check(!st.Filled() && ops.empty(), "a flask will not take sand");
    ItemStack blood{flaskI, 1, 0, (uint16_t)mBlood, 8};
    const char* why2 = nullptr;
    check(!ContainerAccepts(*flask, blood, mWater, c.mats, &why2),
          "a flask of blood will not take water");
  }

  // ---- matter is not minted ------------------------------------------------
  {
    ItemStack st{flaskI, 1};
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
      m2.haveLedger = true;
      m2.ledgerTick = 299;
      m2.ledgerEighths = 1000;
      m2.claims.push_back({300, (uint16_t)mWater, 32});
      ItemStack f2{flaskI, 1};
      ContainerSettle(m2, 300, 1000 + 8, flask, &f2);
      check(f2.fillAmt == 8, "a claim is paid what the GPU took, not what it asked");
    }
    // ...and capacity stops it.
    ItemStack nearlyFull{flaskI, 1, 0, (uint16_t)mWater,
                         (uint16_t)(flask->container.capacity - 4)};
    std::vector<CellOp> ops3;
    ContainerScoopMemo memo3;
    ContainerScoop(*flask, nearlyFull, hit, wordAt, c.world, c.mats, ops3, &why,
                   &memo3, 200);
    check(nearlyFull.fillAmt <= flask->container.capacity,
          "a scoop never overfills the flask");
  }

  // ---- the pour lands where it is aimed ------------------------------------
  {
    ItemStack st{flaskI, 1, 0, (uint16_t)mBlood, 1024};
    const Vec3 mouth{(float)base.x, (float)base.y + 20.0f, (float)base.z};
    const Vec3 target{(float)base.x + 30.0f, (float)base.y, (float)base.z + 10.0f};
    const int g = CurrentTuning().sim.partGravity;
    std::vector<ParticleSpawn> spawns;
    SplatterEvent ev;
    const int n = ContainerPour(*flask, st, mouth, target, g, 500, 0x1234u,
                                spawns, &ev);
    check(n == flask->container.pourPerTick && (int)spawns.size() == n,
          "a pour throws pourPerTick particles");
    check(st.fillAmt == 1024 - n * kContainerUnitsPerCell, "and charges a cell each");
    // Ten more ticks of stream, so the landing is a distribution and not two
    // samples of it.
    for (uint32_t t = 501; t < 511; t++)
      ContainerPour(*flask, st, mouth, target, g, t, 0x1234u, spawns, nullptr);
    check(ev.mat == mBlood && ev.count == n && ev.reach > 0.0f,
          "the splatter names the substance poured");
    bool payloadOk = true;
    float worst = 0.0f, sumX = 0.0f, sumZ = 0.0f;
    for (const ParticleSpawn& s : spawns) {
      payloadOk = payloadOk && (s.payload & 0xFFFu) == mBlood &&
                  (s.flags & kPFlagMicro) == 0;
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
    check(payloadOk, "poured particles are grid matter, not spray");
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
    // A target past the vessel's reach is pulled in to it.
    const Vec3 far = ContainerClampTarget(*flask, mouth, mouth + Vec3{1000, 0, 0});
    check(std::fabs((far - mouth).len() - flask->container.pourRange) < 0.01f,
          "a pour target is clamped to the vessel's reach");
  }

  // ---- stacks --------------------------------------------------------------
  {
    Inventory hb;
    hb.slots[0] = ItemStack{flaskI, 1, 0, (uint16_t)mWater, 40};
    const int where = hb.Add(flaskI, 1);
    check(where != 0 && hb.slots[0].count == 1,
          "an empty flask does not fold into a filled one");
    hb.slots[3] = ItemStack{flaskI, 3};
    Bag bag;
    check(ContainerIsolateOne(hb.slots, kItemSlots, 3, bag.slots, Bag::kSlots) &&
              hb.slots[3].count == 1,
          "filling one of a stack sets the others down");
    int rest = 0;
    for (const ItemStack& s : hb.slots)
      if (!s.Empty() && s.def == flaskI && !s.Filled()) rest += s.count;
    check(rest == 4, "and none of them is lost");
  }

  // ---- persistence ---------------------------------------------------------
  {
    GlyphLibrary glyphs;
    PlayerCaster caster;
    Inventory hb;
    PlayerKit kit;
    hb.slots[4] = ItemStack{flaskI, 1, 0, (uint16_t)mBlood, 77};
    kit.bag.slots[2] = ItemStack{pouchI, 1, 0, (uint16_t)mSand, 16};
    PlayerKitRefs refs{&caster, &glyphs, &hb, &kit, &c.items};
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
                hb.slots[4].def == flaskI,
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
    } else {
      check(false, "the human has a live limb to pour on");
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
//     nothing and plus at most the 7/8 rounding of the last drop.
Status GateVesselGrid(Ctx& c, std::string& detail) {
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

  // High in the window: sky, so nothing the terrain does reaches the basin.
  const IVec3 o = c.world.WindowOrigin();
  const IVec3 base{o.x * (int)kChunk + (int)kWorldN / 2 + 8,
                   o.y * (int)kChunk + (int)kWorldN - 40,
                   o.z * (int)kChunk + (int)kWorldN / 2 + 8};
  const IVec3 chunk{base.x >> 4, base.y >> 4, base.z >> 4};
  uint32_t tick = 71000;
  auto step = [&](const std::vector<CellOp>& cells,
                  const std::vector<ParticleSpawn>& spawns = {}) {
    std::vector<BrushOp> ops;
    SubmitTick(c.ctx, c.world, c.sim, tick++, kDefaultSeed, ops, {}, cells, false,
               chunk, true, true, spawns);
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
                         buf.data(), "vessel-grid");
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
  ItemStack st{flaskI, 1};
  ContainerScoopMemo memo;
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
    if (found)
      ContainerScoop(*flask, st, aim,
                     [&](IVec3 p, uint32_t& w) { return ContainerSnapWord(c.world, p, w); },
                     c.world, c.mats, cells, nullptr, &memo, tick);
    step(cells);
    const WorldSnapshot& sn = c.world.Snap();
    if (sn.valid) ContainerSettle(memo, sn.tick, sn.scoopEighths, flask, &st);
  }
  for (int i = 0; i < 12; i++) {
    step({});
    const WorldSnapshot& sn = c.world.Snap();
    if (sn.valid) ContainerSettle(memo, sn.tick, sn.scoopEighths, flask, &st);
  }
  const long w1 = mirrorWater();
  const long taken = w0 - w1;

  // THE POUR: from ten cells above the basin's middle, onto it.
  const int held = st.fillAmt;
  const Vec3 mouth{base.x + 0.5f, base.y + 10.5f, base.z + 0.5f};
  const Vec3 target{base.x + 0.5f, base.y + 0.5f, base.z + 0.5f};
  for (int i = 0; i < 200 && st.Filled(); i++) {
    std::vector<ParticleSpawn> spawns;
    ContainerPour(*flask, st, mouth, target, CurrentTuning().sim.partGravity, tick,
                  0x77u, spawns, nullptr);
    step({}, spawns);
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

  RecordObserved("vesselGridTaken", (double)taken);
  RecordObserved("vesselGridHeld", (double)held);
  RecordObserved("vesselGridBack", (double)back);
  // EXACT: the flask is paid from the GPU's own ledger.
  const bool scoopOk = held > 0 && taken == held && memo.claims.empty();
  // Everything poured is on the tray, plus at most the last drop's rounding
  // up to a whole cell (ContainerPour: under one cell per emptying).
  const bool pourOk = !st.Filled() && stillFlying == 0 && back >= held &&
                      back < held + kContainerUnitsPerCell;
  detail = Format("pool %ld/8 -> flask credited %d, grid lost %ld; poured back, "
                  "grid gained %ld (%u particles still live; by tick%s)",
                  w0, held, taken, back, stillFlying, timeline.c_str());
  return scoopOk && pourOk ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& VesselGates() {
  static const std::vector<Gate> g = {
      // No deps: a hand-built grid and one spawned human it cleans up.
      {"vessel", "player", {}, false, GateVessel},
      // Builds in the sky and regenerates the world after itself.
      {"vessel-grid", "player", {}, false, GateVesselGrid},
  };
  return g;
}

}  // namespace selftest
