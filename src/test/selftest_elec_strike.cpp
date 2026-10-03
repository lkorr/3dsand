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
//                 lightning glyph's own spec targets the same rod.
//
// Ticks THE tick (support::TickCursor -> TickAuthority): the strike lives in
// session.cpp's phase K (WeatherStrikes), so a sim-only ticker would test
// nothing. The fixture sits in open air over the harness terrain on a stone
// pad (FixtureYOver, never an absolute Y) and the world is regenerated on the
// way out.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "game/lightning.h"
#include "game/session.h"
#include "game/spell.h"
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
};

RunOut RunStrike(Ctx& c, uint32_t mStone, uint32_t mWood, uint32_t mIron, uint32_t mLightning,
                 uint32_t mArc, uint32_t mFire, Fixture& fx, int observeTicks,
                 const GlyphStrike* glyph) {
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
  Fixture fx;
  const RunOut a = RunStrike(c, (uint32_t)mStone, (uint32_t)mWood, (uint32_t)mIron,
                             (uint32_t)mLightning, (uint32_t)mArc, (uint32_t)mFire, fx, observe,
                             gLight >= 0 ? &lib.glyphs[gLight].strike : nullptr);
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
  check(a.firePeak > 0 || a.woodEnd < a.wood0,
        Format("the wooden collar caught (fire peak %u, wood %u -> %u)", a.firePeak, a.wood0,
               a.woodEnd));
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
  detail = Format(
      "struck (%d,%d,%d) %s [rod (%d,%d,%d)], plan %zu cells (bolt %d, splash %d), lightning "
      "%u / arc %u on the fire tick, gone after %d ticks, fire peak %u, wood %u -> %u, "
      "lightning glyph tariff %d, replay %s%s%s",
      a.plan.target.x, a.plan.target.y, a.plan.target.z,
      a.plan.targetConductive ? "conductive" : "insulator", fx.rod.x, fx.rod.y, fx.rod.z,
      a.plan.cells.size(), a.plan.boltCells, a.plan.splashCells, a.lightningAtFire,
      a.arcAtFire, a.goneAfter, a.firePeak, a.wood0, a.woodEnd, lightTariff,
      same ? "identical" : "DIFFERS", fails.empty() ? "" : "; FAILED: ", fails.c_str());
  std::printf("elec-strike: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& ElecStrikeGates() {
  static const std::vector<Gate> g = {
      {"elec-strike", "sim", {}, false, GateElecStrike},
  };
  return g;
}

}  // namespace selftest
