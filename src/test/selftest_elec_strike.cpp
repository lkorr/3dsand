// Electricity, package E3 (docs/PLAN_electricity.md section 3): the strike path.
//
//   elec-strike   a FORCED WEATHER STRIKE (TickAuthorityCtx::strikes.forceNext,
//                 the storm's own two-step decide -> fire) aimed beside a
//                 fixture: an IRON ROD on a wooden pole, a wooden collar round
//                 the rod's head, and a TALLER plain wooden post nearby. The
//                 bolt must take the rod (a conductor beats a taller insulator
//                 by the conduct bonus), lightning cells must appear and be
//                 gone within elecStrike.goneTicksMax, the collar must catch
//                 (fire appears / wood is lost), and the strike must be
//                 charged to the tick's strike budget. Run TWICE in-process
//                 from a regenerated world: the two CellOp lists must be
//                 identical (rule 1). Plus the glyph half, CPU-only: `spark`
//                 places spark (not fire), `shock` / `lightning` are strike
//                 effects whose emission reaches the VM's strike list, and the
//                 lightning glyph's own spec targets the same rod. Wave 2:
//                 the collar must BOTH show fire and lose wood (two checks,
//                 not an OR), and after the bolt every charge page must come
//                 back and the fixture must sleep (<= elecStrike.awakeMax).
//
//   elec-bulk     (wave 2, the spreading loss) the same forced weather strike
//                 onto BULK conductors: a 128 x 128 basin of water (the sea)
//                 and a 128 x 128 pad of rain-wet dirt. The charge must stay
//                 LOCAL -- reach (P > 0) within elecBulk.reachMax cells of the
//                 struck column, pages at most elecBulk.pagesMax, nothing
//                 refused -- and still SHOCK nearby (P >= elecBulk.shockP, a
//                 wet body's threshold, out to elecBulk.shockReachMin cells in
//                 the water); then every page back and the fixture asleep.
//                 Before the loss one bolt into water charged ~800 cells and
//                 paged the window.
//
// Ticks THE tick (support::TickCursor -> TickAuthority): the strike lives in
// session.cpp's phase K (WeatherStrikes), so a sim-only ticker would test
// nothing. The fixture sits in open air over the harness terrain on a stone
// pad (FixtureYOver, never an absolute Y) and the world is regenerated on the
// way out.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "game/lightning.h"
#include "game/session.h"
#include "game/spell.h"
#include "gpu/resources.h"
#include "sim/elec.h"
#include "sim/worldgen_run.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

using namespace sandvox;

namespace selftest {
namespace {

int MatId(Ctx& c, const char* n) {
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == n) return (int)i;
  return -1;
}

struct Box {
  IVec3 lo, hi;  // inclusive
  bool Has(int x, int y, int z) const {
    return x >= lo.x && x <= hi.x && y >= lo.y && y <= hi.y && z >= lo.z && z <= hi.z;
  }
};

// Material histogram over a box, through the page table.
std::vector<uint32_t> Census(Ctx& c, const Box& b) {
  std::vector<uint32_t> h(c.mats.size() + 1, 0);
  std::vector<uint32_t> buf(kChunkVol);
  for (int cz = b.lo.z >> 4; cz <= (b.hi.z >> 4); cz++)
    for (int cy = b.lo.y >> 4; cy <= (b.hi.y >> 4); cy++)
      for (int cx = b.lo.x >> 4; cx <= (b.hi.x >> 4); cx++) {
        ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex({cx, cy, cz}), 1, buf.data(),
                       "elecStrikeCensus");
        for (uint32_t k = 0; k < kChunkVol; k++) {
          const int x = (int)(k % 16) + cx * 16, y = (int)((k / 16) % 16) + cy * 16,
                    z = (int)(k / 256) + cz * 16;
          if (!b.Has(x, y, z)) continue;
          const uint32_t m = buf[k] & 0xFFFu;
          h[m < c.mats.size() ? m : c.mats.size()]++;
        }
      }
  return h;
}

// The charge field's header (elec.h kEm*): one small readback.
std::vector<uint32_t> ElecHeader(Ctx& c) {
  c.ctx.WaitIdle();
  std::vector<uint32_t> h(kEmSnapWords, 0);
  rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.world.elecMeta, 0, h.data(),
                        kEmSnapWords * 4, "elecStrikeHeader");
  return h;
}
uint32_t PagesInUse(const std::vector<uint32_t>& h) { return h[kEmNextFresh] - h[kEmFreeTop]; }
// Chunks awake (the dirty set the next tick runs).
uint32_t AwakeAll(Ctx& c) {
  c.ctx.WaitIdle();
  std::vector<uint32_t> flags(kNumSlots, 0);
  rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.sim.DirtyActive(), 0, flags.data(),
                        kNumSlots * 4, "elecStrikeAwake");
  uint32_t n = 0;
  for (uint32_t f : flags) n += f != 0;
  return n;
}

// The awake chunks with ATTRIBUTION: how many, how many of them the charge
// field holds (dirty reason "elec", bit 31), and every reason that holds any.
struct Awake {
  uint32_t total = 0, elec = 0, reasons = 0;
  std::string Names() const {
    std::string out;
    for (int i = 0; i < kDirtyReasonBits; i++)
      if ((reasons >> i) & 1u) out += (out.empty() ? "" : "+") + std::string(kDirtyReasonName[i]);
    return out.empty() ? std::string("none") : out;
  }
};
Awake AwakeWhy(Ctx& c) {
  c.ctx.WaitIdle();
  std::vector<uint32_t> flags(kNumSlots, 0);
  rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.sim.DirtyActive(), 0, flags.data(),
                        kNumSlots * 4, "elecBulkAwake");
  Awake a;
  for (uint32_t f : flags) {
    if (f == 0) continue;
    a.total++;
    a.elec += (f & (1u << 31)) != 0;
    a.reasons |= f;
  }
  return a;
}

void Regenerate(Ctx& c) {
  c.mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
}

struct Fixture {
  IVec3 s{};       // pad top + 1 (the floor the posts stand on)
  IVec3 rod{};     // the rod's column, at its TOP cell
  IVec3 tall{};    // the taller wooden post's top cell
  IVec3 aim{};
  Box watch{};
};

// The fixture: a stone pad on a foundation down into the terrain (a floating
// box is an island the debris scan cuts loose), a wooden pole (floor..+6)
// with an iron rod on top (+7..+9), a two-high wooden collar round the rod's
// head (+9..+10, its four side neighbours), and a plain wooden post taller
// than the rod's head (to +12) four cells off. Scores: rod 9 + 6 (conductor)
// = 15, tall post 12, collar 10 -- the rod wins only because it conducts.
//
// TWO op lists, two ticks: `build` (the pad and the cleared air above it) and
// `posts`. Two cell ops on one cell in one tick keep the FIRST (CLAUDE.md
// rule 3), so a post pushed after its cell's clear in the same list never
// lands -- the first cut of this gate struck an empty pad.
Fixture BuildFixture(Ctx& c, std::vector<CellOp>& build, std::vector<CellOp>& posts,
                     uint32_t mStone, uint32_t mWood,
                     uint32_t mIron) {
  Fixture f;
  const IVec3 o = c.world.WindowOrigin();
  const int x = o.x * (int)kChunk + 300, z = o.z * (int)kChunk + 300;
  // Wider than the weather strike's search disc (8) round the aim, so every
  // column the bolt can choose is the fixture's or the cleared pad's.
  const int half = 10;
  const int y = FixtureYOver(x - half - 2, z - half - 2, x + half + 2, z + half + 2,
                             kDefaultSeed, 3);
  f.s = {x, y, z};
  for (int zz = z - half; zz <= z + half; zz++)
    for (int xx = x - half; xx <= x + half; xx++) {
      for (int yy = World::TerrainHeight(xx, zz, kDefaultSeed) - 2; yy < y; yy++)
        build.push_back({World::SlotCellIndex({xx, yy, zz}), PackVoxNew(mStone, 0)});
      // Clear the air above the pad (whatever the terrain put there).
      for (int yy = y; yy <= y + 40; yy++)
        build.push_back({World::SlotCellIndex({xx, yy, zz}), 0u});
    }
  const IVec3 p{x + 1, y, z - 1};
  for (int yy = y; yy <= y + 6; yy++)
    posts.push_back({World::SlotCellIndex({p.x, yy, p.z}), PackVoxNew(mWood, 0)});
  for (int yy = y + 7; yy <= y + 9; yy++)
    posts.push_back({World::SlotCellIndex({p.x, yy, p.z}), PackVoxNew(mIron, 0)});
  const int dxz[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
  for (const auto& d : dxz)
    for (int yy = y + 9; yy <= y + 10; yy++)
      posts.push_back({World::SlotCellIndex({p.x + d[0], yy, p.z + d[1]}), PackVoxNew(mWood, 0)});
  const IVec3 t{x - 3, y, z + 2};
  for (int yy = y; yy <= y + 12; yy++)
    posts.push_back({World::SlotCellIndex({t.x, yy, t.z}), PackVoxNew(mWood, 0)});
  f.rod = {p.x, y + 9, p.z};
  f.tall = {t.x, y + 12, t.z};
  // Aimed between the two, nearer the TALL post: the bolt has to choose.
  f.aim = {x - 1, y + 4, z + 1};
  f.watch = Box{{x - half, y, z - half}, {x + half, y + 40, z + half}};
  return f;
}

struct RunOut {
  bool fired = false;
  StrikePlan plan;
  uint32_t fireTick = 0;
  uint32_t lightningAtFire = 0, arcAtFire = 0;
  int goneAfter = -1;           // ticks after the fire tick until no lightning/arc
  uint32_t firePeak = 0;
  uint32_t wood0 = 0, woodEnd = 0;
  uint64_t budgetCells = 0, budgetEmitted = 0, budgetRefused = 0;
  uint64_t decided = 0, firedCount = 0;
  bool eventEmitted = false;
  bool glyphTakesRod = false;
  // After the bolt (wave 2): the tick every charge page was back (-1 never),
  // the chunks awake `settle` ticks later, the field's pages peak.
  int fadeTick = -1;
  uint32_t awake = 0, pagesPeak = 0, pagesEnd = 0;
};

RunOut RunStrike(Ctx& c, uint32_t mStone, uint32_t mWood, uint32_t mIron, uint32_t mLightning,
                 uint32_t mArc, uint32_t mFire, Fixture& fx, int observeTicks,
                 const GlyphStrike* glyph, int fadeMax = 0, int settle = 0) {
  RunOut r;
  Regenerate(c);
  std::vector<CellOp> build, posts;
  fx = BuildFixture(c, build, posts, mStone, mWood, mIron);
  uint32_t t = 81000;
  support::TickCursor tick{c, t, IVec3{fx.s.x >> 4, fx.s.y >> 4, fx.s.z >> 4}};
  tick(std::vector<BrushOp>{}, build);
  tick(std::vector<BrushOp>{}, posts);
  // Let the snapshot mirror publish the fixture (kSnapshotLatency + slack).
  for (int i = 0; i < 10; i++) tick();
  const std::vector<uint32_t> h0 = Census(c, fx.watch);
  r.wood0 = h0[mWood];
  r.woodEnd = r.wood0;
  // The lightning glyph's own spec against the same fixture, planned (not
  // emitted) on the mirror BEFORE the strike burns anything: it must choose
  // the same rod from a different aim.
  if (glyph) {
    StrikeMats sm;
    sm.Resolve(c.mats);
    const SpellProbe probe = WorldStrikeProbe(c.world);
    const StrikeSpec gs =
        StrikeSpecFromGlyph(*glyph, IVec3{fx.rod.x - 2, fx.s.y + 2, fx.rod.z + 1}, 1000, 0x1234u);
    const StrikePlan gp = PlanStrike(gs, sm, &probe, c.world);
    r.glyphTakesRod = gp.target.x == fx.rod.x && gp.target.z == fx.rod.z &&
                      gp.target.y == fx.rod.y && gp.targetConductive;
  }
  TickAuthorityCtx& w = tick.Rig().Authority();
  const uint64_t cells0 = w.strikes.budget.cells, emitted0 = w.strikes.budget.emitted,
                 refused0 = w.strikes.budget.refused;
  const uint64_t fired0 = w.strikes.weatherFired, decided0 = w.strikes.weatherDecided;
  w.strikes.forceNext = true;
  w.strikes.forceAimSet = true;
  w.strikes.forceAim = fx.aim;
  const uint32_t decideTick = t + 1;
  int sinceFire = -1;
  for (int i = 0; i < observeTicks; i++) {
    tick();
    if (!r.fired) {
      for (const auto& ev : w.strikes.recent)
        if (ev.weather && ev.tick == t && ev.tick > decideTick) {
          r.fired = true;
          r.fireTick = t;
          r.eventEmitted = ev.emitted;
          r.plan = w.strikes.lastPlan;
        }
      if (r.fired) {
        const std::vector<uint32_t> h = Census(c, fx.watch);
        r.lightningAtFire = h[mLightning];
        r.arcAtFire = h[mArc];
        r.firePeak = std::max(r.firePeak, h[mFire]);
        sinceFire = 0;
        if (h[mLightning] == 0 && h[mArc] == 0) r.goneAfter = 0;
      }
      continue;
    }
    sinceFire++;
    // Every tick until the bolt is gone, then every fifth (and the last).
    if (r.goneAfter >= 0 && sinceFire % 5 != 0 && i != observeTicks - 1) continue;
    const std::vector<uint32_t> h = Census(c, fx.watch);
    r.firePeak = std::max(r.firePeak, h[mFire]);
    if (r.goneAfter < 0 && h[mLightning] == 0 && h[mArc] == 0) r.goneAfter = sinceFire;
    r.woodEnd = h[mWood];
  }
  // THE FADE (wave 2): every charge page back, then the fixture asleep.
  if (fadeMax > 0) {
    for (int i = 0; i < fadeMax; i++) {
      if (PagesInUse(ElecHeader(c)) == 0) {
        r.fadeTick = i;
        break;
      }
      tick();
    }
    for (int i = 0; i < settle; i++) tick();
    r.awake = AwakeAll(c);
    const std::vector<uint32_t> h = ElecHeader(c);
    r.pagesPeak = h[kEmPagesPeak];
    r.pagesEnd = PagesInUse(h);
    ElecNoteRun(h.data());
  }
  r.budgetCells = w.strikes.budget.cells - cells0;
  r.budgetEmitted = w.strikes.budget.emitted - emitted0;
  r.budgetRefused = w.strikes.budget.refused - refused0;
  r.decided = w.strikes.weatherDecided - decided0;
  r.firedCount = w.strikes.weatherFired - fired0;
  return r;
}

Status GateElecStrike(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  const int mStone = MatId(c, "stone"), mWood = MatId(c, "wood"), mIron = MatId(c, "iron"),
            mLightning = MatId(c, "lightning"), mArc = MatId(c, "arc"),
            mSpark = MatId(c, "spark"), mFire = MatId(c, "fire");
  if (mStone < 0 || mWood < 0 || mIron < 0 || mLightning < 0 || mArc < 0 || mSpark < 0 ||
      mFire < 0) {
    detail = "missing one of stone/wood/iron/lightning/arc/spark/fire";
    std::printf("elec-strike: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  bool ok = true;
  std::string fails;
  auto check = [&](bool cond, const std::string& what) {
    if (!cond) {
      ok = false;
      fails += (fails.empty() ? "" : "; ") + what;
      std::printf("elec-strike: FAILED %s\n", what.c_str());
    }
  };

  // ---- A. the glyphs (CPU) ---------------------------------------------------
  GlyphLibrary lib;
  std::string gerr;
  if (!LoadGlyphs(AssetDir() + "/spells/glyphs.json", c.mats, lib, gerr)) {
    detail = "glyph load failed: " + gerr;
    std::printf("elec-strike: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  const int gSpark = lib.Find("spark"), gShock = lib.Find("shock"),
            gLight = lib.Find("lightning");
  check(gSpark >= 0 && lib.glyphs[gSpark].verb == SpellVerb::Place &&
            lib.glyphs[gSpark].material == (uint32_t)mSpark,
        "the spark glyph places spark");
  check(gShock >= 0 && lib.glyphs[gShock].verb == SpellVerb::Strike &&
            lib.glyphs[gShock].strike.splashMat == (uint32_t)mArc &&
            lib.glyphs[gShock].strike.height == 0,
        "the shock glyph is an arc burst (strike, no bolt)");
  check(gLight >= 0 && lib.glyphs[gLight].verb == SpellVerb::Strike &&
            lib.glyphs[gLight].strike.boltMat == (uint32_t)mLightning &&
            lib.glyphs[gLight].strike.height > 0,
        "the lightning glyph is a bolt of lightning");
  int32_t lightTariff = 0;
  if (gLight >= 0) {
    EffectInst e;
    e.verb = SpellVerb::Strike;
    e.glyph = gLight;
    SpellEmission em;
    ApplySpellEffect(lib, {e}, SpellFxVec{SpellFxFromFloat(10.5f), SpellFxFromFloat(20.5f),
                                          SpellFxFromFloat(30.5f)},
                     SpellFxVec{0, -kSpellFxOne, 0}, 1000, em);
    check(em.strikes.size() == 1 && em.strikes[0].glyph == gLight && em.ops.empty(),
          "a lightning effect reaches the VM's strike list (not a brush op)");
    lightTariff = EffectTariff(lib, e);
    check(lightTariff > 0, "a lightning effect has a price");
  }

  // ---- B. the forced weather strike, twice --------------------------------------
  const int observe = (int)BaselineNumber("elecStrike.observeTicks", 60);
  const int fadeMax = (int)BaselineNumber("elecStrike.fadeTicksMax", 120);
  const int settle = (int)BaselineNumber("elecStrike.settleTicks", 150);
  const uint32_t awakeMax = (uint32_t)BaselineNumber("elecStrike.awakeMax", 32);
  Fixture fx;
  const RunOut a = RunStrike(c, (uint32_t)mStone, (uint32_t)mWood, (uint32_t)mIron,
                             (uint32_t)mLightning, (uint32_t)mArc, (uint32_t)mFire, fx, observe,
                             gLight >= 0 ? &lib.glyphs[gLight].strike : nullptr, fadeMax, settle);
  Fixture fx2;
  const RunOut b = RunStrike(c, (uint32_t)mStone, (uint32_t)mWood, (uint32_t)mIron,
                             (uint32_t)mLightning, (uint32_t)mArc, (uint32_t)mFire, fx2, observe,
                             nullptr);
  Regenerate(c);

  const int goneMax = (int)BaselineNumber("elecStrike.goneTicksMax", 30);
  check(a.fired, "the forced strike fired (decide -> leader -> fire)");
  check(a.decided == 1 && a.firedCount >= 1, Format("one decision (%llu) fired (%llu)",
                                                     (unsigned long long)a.decided,
                                                     (unsigned long long)a.firedCount));
  check(a.plan.target.x == fx.rod.x && a.plan.target.y == fx.rod.y &&
            a.plan.target.z == fx.rod.z && a.plan.targetConductive,
        Format("the bolt took the iron rod (%d,%d,%d) over the taller wood post (%d,%d,%d): "
               "struck (%d,%d,%d) %s, %d columns scanned",
               fx.rod.x, fx.rod.y, fx.rod.z, fx.tall.x, fx.tall.y, fx.tall.z, a.plan.target.x,
               a.plan.target.y, a.plan.target.z,
               a.plan.targetConductive ? "conductive" : "NOT conductive", a.plan.columnsScanned));
  check(a.glyphTakesRod, "the lightning glyph's spec takes the same rod");
  check(a.lightningAtFire > 0, Format("lightning cells in the grid on the fire tick (%u)",
                                      a.lightningAtFire));
  check(a.goneAfter >= 0 && a.goneAfter <= goneMax,
        Format("lightning + arc gone within %d ticks (%d)", goneMax, a.goneAfter));
  // Two claims, not an OR (wave 2): fire appeared AND wood was lost.
  check(a.firePeak > 0, Format("the wooden collar caught fire (fire peak %u)", a.firePeak));
  check(a.woodEnd < a.wood0, Format("the strike cost the fixture wood (%u -> %u)", a.wood0,
                                    a.woodEnd));
  check(a.fadeTick >= 0 && a.pagesEnd == 0,
        Format("every charge page back after the bolt (%u left, freed %d ticks after the "
               "watch, max %d; pages peak %u)",
               a.pagesEnd, a.fadeTick, fadeMax, a.pagesPeak));
  check(a.awake <= awakeMax, Format("the fixture sleeps %d ticks after the field is gone (%u "
                                    "chunks awake, max %u)",
                                    settle, a.awake, awakeMax));
  check(a.eventEmitted && a.budgetEmitted >= 1 && a.budgetCells >= a.plan.cells.size() &&
            a.plan.cells.size() > 0 && a.plan.cells.size() <= kStrikeMaxCells &&
            a.budgetRefused == 0,
        Format("charged to the strike budget (%llu cells over %llu strikes, plan %zu, %llu "
               "refused)",
               (unsigned long long)a.budgetCells, (unsigned long long)a.budgetEmitted,
               a.plan.cells.size(), (unsigned long long)a.budgetRefused));
  bool same = a.plan.cells.size() == b.plan.cells.size() && a.fireTick == b.fireTick;
  for (size_t i = 0; same && i < a.plan.cells.size(); i++)
    same = a.plan.cells[i].cellIdx == b.plan.cells[i].cellIdx &&
           a.plan.cells[i].word == b.plan.cells[i].word;
  check(same, Format("two runs plan the same CellOp list (%zu vs %zu cells)",
                     a.plan.cells.size(), b.plan.cells.size()));
  // A strike that does not fit is refused WHOLE: a budget with no room left.
  {
    StrikeBudget tight;
    tight.cap = (uint32_t)a.plan.cells.size() > 0 ? (uint32_t)a.plan.cells.size() - 1 : 0;
    std::vector<CellOp> sink;
    const bool emitted = EmitStrike(a.plan, tight, sink);
    check(!emitted && sink.empty() && tight.refused == 1 && tight.used == 0,
          "a strike over the budget is refused whole and counted");
  }

  RecordObserved("elecStrike.goneTicks", (double)a.goneAfter);
  RecordObserved("elecStrike.planCells", (double)a.plan.cells.size());
  RecordObserved("elecStrike.fadeTicksObserved", (double)a.fadeTick);
  RecordObserved("elecStrike.awakeObserved", (double)a.awake);
  RecordObserved("elecStrike.pagesPeakObserved", (double)a.pagesPeak);
  detail = Format(
      "struck (%d,%d,%d) %s [rod (%d,%d,%d)], plan %zu cells (bolt %d, splash %d), lightning "
      "%u / arc %u on the fire tick, gone after %d ticks, fire peak %u, wood %u -> %u, "
      "lightning glyph tariff %d, pages peak %u, freed %d, awake %u, replay %s%s%s",
      a.plan.target.x, a.plan.target.y, a.plan.target.z,
      a.plan.targetConductive ? "conductive" : "insulator", fx.rod.x, fx.rod.y, fx.rod.z,
      a.plan.cells.size(), a.plan.boltCells, a.plan.splashCells, a.lightningAtFire,
      a.arcAtFire, a.goneAfter, a.firePeak, a.wood0, a.woodEnd, lightTariff, a.pagesPeak,
      a.fadeTick, a.awake, same ? "identical" : "DIFFERS", fails.empty() ? "" : "; FAILED: ",
      fails.c_str());
  std::printf("elec-strike: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ============================================================================
// elec-bulk (wave 2): a strike into BULK conductors stays local.
//
// The field and its pages, read whole after the queue drains (test-only).
struct FieldSnap {
  std::vector<uint32_t> meta, pool;
  void Read(Ctx& c) {
    c.ctx.WaitIdle();
    meta.assign(kEmWords, 0);
    rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.world.elecMeta, 0, meta.data(),
                          (uint64_t)kEmWords * 4, "elecBulkMeta");
    pool.assign((size_t)kElecPoolPages * kElecPageWords, 0);
    rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.world.elecPool, 0, pool.data(),
                          pool.size() * 4, "elecBulkPool");
  }
  uint32_t P(int x, int y, int z) const {
    const uint32_t e = meta[kEmEntry + World::SlotChunkIndex({x >> 4, y >> 4, z >> 4})];
    if ((e & kElecEntryHas) == 0) return 0;
    const uint32_t local = (((uint32_t)z & 15) * 16 + ((uint32_t)y & 15)) * 16 + ((uint32_t)x & 15);
    const uint32_t w = pool[(size_t)(e & kElecEntryPage) * kElecPageWords + (local >> 1)];
    return (w >> ((local & 1) * 16)) & 0xFFFFu;
  }
};

enum class BulkKind { Sea, WetGround };

struct BulkOut {
  bool fired = false, conductive = false;
  IVec3 target{};
  int reach = 0;              // farthest cell with P > 0 from the struck column (cells, horizontal)
  int shockReach = 0;         // farthest with P >= shockP
  uint32_t charged = 0;       // cells with P > 0, at the widest tick read
  uint32_t outsidePages = 0;  // most pages in use OUTSIDE the fixture's chunks at once
  uint32_t pagesPeak = 0, refused = 0, purges = 0, pPeak = 0;
  int fadeTick = -1;          // ticks after the watch until every page was back
  uint32_t pagesEnd = 0;
  Awake awake;
};

// THE FIXTURE: a 129 x 129 slab on a stone foundation down into the terrain
// (a floating slab is an island the debris scan cuts loose), the air above it
// cleared. Sea: a 3-deep basin of water inside a one-cell stone rim, flush
// with the water. Wet ground: one layer of dirt under a rain-strength water
// coat (stain amount elecBulk.wetStain) -- the dirt itself is an insulator,
// so only the coat conducts, as on a rain-soaked field. The ops go in slices
// (kMaxCellOpsPerTick), foundation first, then the clear, then the contents:
// two ops on one cell in one TICK keep the first, so a slice never mixes them.
BulkOut RunBulk(Ctx& c, BulkKind kind, int observe, int reachTicks, int fadeMax, int settle,
                uint32_t shockP, uint32_t wetStain) {
  BulkOut r;
  const uint32_t mStone = (uint32_t)MatId(c, "stone"), mWater = (uint32_t)MatId(c, "water"),
                 mDirt = (uint32_t)MatId(c, "dirt");
  Regenerate(c);
  const IVec3 o = c.world.WindowOrigin();
  constexpr int kHalf = 64, kDepth = 3;
  const int x = o.x * (int)kChunk + 256, z = o.z * (int)kChunk + 256;
  const int y = FixtureYOver(x - kHalf - 2, z - kHalf - 2, x + kHalf + 2, z + kHalf + 2,
                             kDefaultSeed, 3);
  const int top = kind == BulkKind::Sea ? y + kDepth - 1 : y;   // the conducting surface
  std::vector<CellOp> found, clear, fill;
  const uint32_t wetDirt = PackVoxNew(mDirt, 0) |
                           PackStain(c.mats[mWater].stainSlot, std::min(wetStain, kStainAmtMax));
  for (int zz = z - kHalf; zz <= z + kHalf; zz++)
    for (int xx = x - kHalf; xx <= x + kHalf; xx++) {
      for (int yy = World::TerrainHeight(xx, zz, kDefaultSeed) - 2; yy < y; yy++)
        found.push_back({World::SlotCellIndex({xx, yy, zz}), PackVoxNew(mStone, 0)});
      for (int yy = y; yy <= y + 40; yy++) clear.push_back({World::SlotCellIndex({xx, yy, zz}), 0u});
      const bool rim = xx == x - kHalf || xx == x + kHalf || zz == z - kHalf || zz == z + kHalf;
      if (kind == BulkKind::Sea) {
        for (int yy = y; yy < y + kDepth; yy++)
          fill.push_back({World::SlotCellIndex({xx, yy, zz}),
                          rim ? PackVoxNew(mStone, 0) : (mWater | (7u << 12))});
      } else {
        fill.push_back({World::SlotCellIndex({xx, y, zz}), wetDirt});
      }
    }
  uint32_t t = 82000;
  support::TickCursor tick{c, t, IVec3{x >> 4, y >> 4, z >> 4}};
  for (const std::vector<CellOp>* list : {&found, &clear, &fill})
    for (size_t at = 0; at < list->size(); at += 60000) {
      const size_t n = std::min<size_t>(60000, list->size() - at);
      tick(std::vector<BrushOp>{}, std::vector<CellOp>(list->begin() + (ptrdiff_t)at,
                                                       list->begin() + (ptrdiff_t)(at + n)));
    }
  // Let the snapshot mirror publish the fixture (the strike scans it).
  for (int i = 0; i < 10; i++) tick();
  TickAuthorityCtx& w = tick.Rig().Authority();
  w.strikes.forceNext = true;
  w.strikes.forceAimSet = true;
  w.strikes.forceAim = IVec3{x, top + 4, z};
  const uint32_t decideTick = t + 1;
  const int cx0 = (x - kHalf) >> 4, cx1 = (x + kHalf) >> 4, cz0 = (z - kHalf) >> 4,
            cz1 = (z + kHalf) >> 4;
  int sinceFire = -1;
  FieldSnap f;
  for (int i = 0; i < observe; i++) {
    tick();
    if (!r.fired) {
      for (const auto& ev : w.strikes.recent)
        if (ev.weather && ev.tick == t && ev.tick > decideTick) {
          r.fired = true;
          r.target = w.strikes.lastPlan.target;
          r.conductive = w.strikes.lastPlan.targetConductive;
        }
      if (!r.fired) continue;
      sinceFire = 0;
    } else {
      sinceFire++;
    }
    if (sinceFire > reachTicks) continue;
    // THE REACH, each of the first reachTicks ticks after the fire: every
    // fixture cell from the floor to a cell over the surface.
    f.Read(c);
    uint32_t charged = 0;
    for (int zz = z - kHalf; zz <= z + kHalf; zz++)
      for (int xx = x - kHalf; xx <= x + kHalf; xx++)
        for (int yy = y - 1; yy <= top + 1; yy++) {
          const uint32_t p = f.P(xx, yy, zz);
          if (p == 0) continue;
          charged++;
          const int dx = xx - r.target.x, dz = zz - r.target.z;
          const int d = (int)std::sqrt((double)(dx * dx + dz * dz));
          r.reach = std::max(r.reach, d);
          if (p >= shockP) r.shockReach = std::max(r.shockReach, d);
        }
    r.charged = std::max(r.charged, charged);
    // Pages OUTSIDE the fixture's x / z footprint (at any height: the bolt's
    // own column over the fixture is the strike, not the spread).
    uint32_t outside = 0;
    const IVec3 wo = c.world.WindowOrigin();
    for (uint32_t sl = 0; sl < kNumChunks; sl++) {
      if ((f.meta[kEmEntry + sl] & kElecEntryHas) == 0) continue;
      const int sx = (int)(sl % kNChunk), sz = (int)(sl / (kNChunk * kNChunk));
      const int wx = wo.x + ((sx - wo.x) & (int)(kNChunk - 1));
      const int wz = wo.z + ((sz - wo.z) & (int)(kNChunk - 1));
      if (wx < cx0 || wx > cx1 || wz < cz0 || wz > cz1) outside++;
    }
    r.outsidePages = std::max(r.outsidePages, outside);
  }
  for (int i = 0; i < fadeMax; i++) {
    if (PagesInUse(ElecHeader(c)) == 0) {
      r.fadeTick = i;
      break;
    }
    tick();
  }
  for (int i = 0; i < settle; i++) tick();
  r.awake = AwakeWhy(c);
  const std::vector<uint32_t> h = ElecHeader(c);
  r.pagesPeak = h[kEmPagesPeak];
  r.refused = h[kEmRefused];
  r.purges = h[kEmPurges];
  r.pPeak = h[kEmPPeak];
  r.pagesEnd = PagesInUse(h);
  ElecNoteRun(h.data());
  return r;
}

Status GateElecBulk(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  for (const char* m : {"stone", "water", "dirt", "lightning"})
    if (MatId(c, m) < 0) {
      detail = Format("missing material %s", m);
      std::printf("elec-bulk: FAIL (%s)\n", detail.c_str());
      return Status::Fail;
    }
  const int observe = (int)BaselineNumber("elecBulk.observeTicks", 40);
  const int reachTicks = (int)BaselineNumber("elecBulk.reachTicks", 8);
  const int fadeMax = (int)BaselineNumber("elecBulk.fadeTicksMax", 120);
  const int settle = (int)BaselineNumber("elecBulk.settleTicks", 90);
  const uint32_t awakeMax = (uint32_t)BaselineNumber("elecBulk.awakeMax", 32);
  const uint32_t wetAwakeMax = (uint32_t)BaselineNumber("elecBulk.wetAwakeMax", 200);
  const int reachMax = (int)BaselineNumber("elecBulk.reachMax", 56);
  const uint32_t pagesMax = (uint32_t)BaselineNumber("elecBulk.pagesMax", 160);
  const uint32_t shockP = (uint32_t)BaselineNumber("elecBulk.shockP", 20);
  const int shockMin = (int)BaselineNumber("elecBulk.shockReachMin", 15);
  const uint32_t wetStain = (uint32_t)BaselineNumber("elecBulk.wetStain", 8);
  const BulkOut sea = RunBulk(c, BulkKind::Sea, observe, reachTicks, fadeMax, settle, shockP,
                              wetStain);
  const BulkOut wet = RunBulk(c, BulkKind::WetGround, observe, reachTicks, fadeMax, settle,
                              shockP, wetStain);
  Regenerate(c);
  bool ok = true;
  std::string fails;
  auto check = [&](bool cond, const std::string& what) {
    if (!cond) {
      ok = false;
      fails += (fails.empty() ? "" : "; ") + what;
      std::printf("elec-bulk: FAILED %s\n", what.c_str());
    }
  };
  for (const BulkOut* b : {&sea, &wet}) {
    const char* nm = b == &sea ? "sea" : "wet ground";
    check(b->fired, Format("%s: the forced strike fired", nm));
    check(b->reach <= reachMax, Format("%s: the charge stays local (reach %d cells, max %d)", nm,
                                       b->reach, reachMax));
    check(b->outsidePages == 0, Format("%s: no page outside the fixture (%u)", nm,
                                       b->outsidePages));
    check(b->pagesPeak <= pagesMax && b->refused == 0,
          Format("%s: pages bounded (peak %u, max %u; refused %u)", nm, b->pagesPeak, pagesMax,
                 b->refused));
    check(b->fadeTick >= 0 && b->pagesEnd == 0,
          Format("%s: every page back (%u left, freed %d ticks after the watch)", nm,
                 b->pagesEnd, b->fadeTick));
    check(b->awake.elec == 0, Format("%s: no chunk held awake by charge (%u)", nm,
                                     b->awake.elec));
  }
  // The sea must sleep outright. The wet pad's COAT keeps drying after the
  // bolt (the rain system's own work: dirty reasons other than elec, named in
  // the detail), so there the claim is the charge's -- no chunk awake for it,
  // above -- and the total is held to its own, looser bound.
  check(sea.awake.total <= awakeMax, Format("sea: the fixture sleeps (%u chunks awake, max %u)",
                                            sea.awake.total, awakeMax));
  check(wet.awake.total <= wetAwakeMax,
        Format("wet ground: awake chunks bounded (%u, max %u; held by %s)", wet.awake.total,
               wetAwakeMax, wet.awake.Names().c_str()));
  check(sea.conductive, "sea: the bolt struck the water (a conductor)");
  check(sea.shockReach >= shockMin,
        Format("sea: a strike still shocks nearby (P >= %u out to %d cells, min %d)", shockP,
               sea.shockReach, shockMin));
  RecordObserved("elecBulk.seaReachObserved", (double)sea.reach);
  RecordObserved("elecBulk.seaShockReachObserved", (double)sea.shockReach);
  RecordObserved("elecBulk.seaPagesPeakObserved", (double)sea.pagesPeak);
  RecordObserved("elecBulk.wetReachObserved", (double)wet.reach);
  RecordObserved("elecBulk.wetShockReachObserved", (double)wet.shockReach);
  RecordObserved("elecBulk.wetPagesPeakObserved", (double)wet.pagesPeak);
  auto line = [&](const BulkOut& b, const char* nm) {
    return Format("%s: struck (%d,%d,%d) %s, reach %d cells (P >= %u to %d), %u cells charged, "
                  "pages peak %u (outside the fixture %u), P peak %u, refused %u, purges %u, "
                  "freed %d ticks after the watch, %u awake (%u for charge; held by %s)",
                  nm, b.target.x, b.target.y, b.target.z,
                  b.conductive ? "conductive" : "insulator", b.reach, shockP, b.shockReach,
                  b.charged, b.pagesPeak, b.outsidePages, b.pPeak, b.refused, b.purges,
                  b.fadeTick, b.awake.total, b.awake.elec, b.awake.Names().c_str());
  };
  detail = line(sea, "sea") + "; " + line(wet, "wet ground") +
           (fails.empty() ? "" : "; FAILED: " + fails);
  std::printf("elec-bulk: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& ElecStrikeGates() {
  static const std::vector<Gate> g = {
      {"elec-strike", "sim", {}, false, GateElecStrike},
      {"elec-bulk", "sim", {}, false, GateElecBulk},
  };
  return g;
}

}  // namespace selftest
