// Electricity wave 2, package E (docs/PLAN_electricity_wave2.md): the strike's
// gameplay fixes.
//
//   elec-strike-play
//     A. THE REAL TOP (CPU, PlanStrike over a scripted probe). A pole 100
//        cells above the aim -- four times the old 24-cell scan -- is the
//        struck top; a "canopy" column whose upper part no store knows (its
//        first known cell is solid, no known air above) does not compete and
//        is counted hidden; without the pole the bolt takes the ground, never
//        the inside of the canopy.
//     B. STRENGTH scales the bolt (StrikeSpecFromGlyph): half strength = half
//        the height and half the arcs (at least one); full = unscaled.
//     C. THE TARIFF rides the emission: a lightning effect's SpellStrike
//        carries exactly EffectTariff (what the refund pays back).
//     D. WARDS AT THE STRUCK CELL: a `lightning null self` ward refuses a
//        strike at a cell inside its radius whose AIM lay outside it
//        (SpellSystem::StrikeWarded), and nothing far away.
//     E. THROUGH THE TICK (support::TickCursor -> TickAuthority): the harness
//        body holds `lightning self` in its right hand and presses LMB, twice.
//        With the strike budget shut (cap 0) the bolt is REFUSED and its
//        tariff comes back to the caster's mana, exactly; with the budget open
//        it is EMITTED. Either way the search reaches above the mirror, so it
//        waits a stepped leader for the fetch first (spellLeaders), and it
//        fires within the leader's ticks.
//     F. SOUND SLOTS: `zap` and `shock` are in Cues::kSlotPrefix (the
//        `electric` namespace) -- check_invariants.py holds the schema side.
//
// Same fixture discipline as elec-strike: own pad in open air over the harness
// terrain, the world regenerated on the way out.
#include <algorithm>
#include <climits>
#include <cstdio>
#include <string>
#include <vector>

#include "audio/cues.h"
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

int MatIdOf(Ctx& c, const char* n) {
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == n) return (int)i;
  return -1;
}

// A scripted world for PlanStrike's generic (cell-by-cell) probe path: every
// column is ground up to `ground`; one POLE column is solid to `poleTop` with
// known air above it; one CANOPY column is solid to `canopyTop` but known only
// up to `knownTop` (unknown above, like a tree whose crown is above the
// mirror).
struct ScriptedColumns {
  int ground = 0;
  int poleX = 0, poleZ = 0, poleTop = 0;
  bool pole = true;
  int canopyX = 0, canopyZ = 0, canopyTop = 0, knownTop = 0;
  uint32_t solid = 1;
};

uint32_t ScriptedMat(void* ctx, int32_t x, int32_t y, int32_t z, bool& known) {
  const ScriptedColumns& s = *(const ScriptedColumns*)ctx;
  known = true;
  if (x == s.canopyX && z == s.canopyZ) {
    if (y > s.knownTop) {
      known = false;
      return 0;
    }
    return y <= s.canopyTop ? s.solid : 0u;
  }
  if (s.pole && x == s.poleX && z == s.poleZ) return y <= s.poleTop ? s.solid : 0u;
  return y <= s.ground ? s.solid : 0u;
}

void Regenerate(Ctx& c) {
  c.mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
}

Status GateElecStrikePlay(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  bool ok = true;
  std::string fails;
  auto check = [&](bool cond, const std::string& what) {
    if (!cond) {
      ok = false;
      fails += (fails.empty() ? "" : "; ") + what;
      std::printf("elec-strike-play: FAILED %s\n", what.c_str());
    }
  };
  const int mStone = MatIdOf(c, "stone");
  GlyphLibrary lib;
  std::string gerr;
  if (mStone < 0 || !LoadGlyphs(AssetDir() + "/spells/glyphs.json", c.mats, lib, gerr)) {
    detail = "missing stone or glyph load failed: " + gerr;
    std::printf("elec-strike-play: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  const int gLight = lib.Find("lightning");
  if (gLight < 0 || !lib.glyphs[gLight].strike.has) {
    detail = "no lightning glyph with a strike block";
    std::printf("elec-strike-play: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  const GlyphStrike& gs = lib.glyphs[gLight].strike;
  StrikeMats sm;
  sm.Resolve(c.mats);

  // ---- A. the real top ---------------------------------------------------------
  const IVec3 o = c.world.WindowOrigin();
  const IVec3 aim{o.x * (int)kChunk + 200, o.y * (int)kChunk + 120, o.z * (int)kChunk + 200};
  ScriptedColumns cols;
  cols.solid = (uint32_t)mStone;
  cols.ground = aim.y;
  cols.poleX = aim.x - 3;
  cols.poleZ = aim.z;
  cols.poleTop = aim.y + 100;
  cols.canopyX = aim.x + 2;
  cols.canopyZ = aim.z;
  cols.canopyTop = aim.y + 200;
  cols.knownTop = aim.y + 40;
  SpellProbe sp;
  sp.matAt = ScriptedMat;
  sp.ctx = &cols;
  StrikeSpec spec = StrikeSpecFromGlyph(gs, aim, 1000, 0xE0E0u);
  spec.searchRadius = 4;
  const StrikePlan pa = PlanStrike(spec, sm, &sp, c.world);
  check(pa.targetKnown && pa.target.x == cols.poleX && pa.target.z == cols.poleZ &&
            pa.target.y == cols.poleTop,
        Format("the 100-cell pole is the struck top: struck (%d,%d,%d), pole (%d,%d,%d)",
               pa.target.x, pa.target.y, pa.target.z, cols.poleX, cols.poleTop, cols.poleZ));
  check(pa.columnsHidden >= 1,
        Format("the canopy column entered from inside is hidden (%d hidden)", pa.columnsHidden));
  cols.pole = false;
  const StrikePlan pb = PlanStrike(spec, sm, &sp, c.world);
  check(pb.targetKnown && pb.target.y == cols.ground &&
            !(pb.target.x == cols.canopyX && pb.target.z == cols.canopyZ),
        Format("no pole: the ground, never the inside of the canopy: struck (%d,%d,%d)",
               pb.target.x, pb.target.y, pb.target.z));

  // ---- B. strength ---------------------------------------------------------------
  const StrikeSpec full = StrikeSpecFromGlyph(gs, aim, 1000, 1u);
  const StrikeSpec half = StrikeSpecFromGlyph(gs, aim, 1000, 1u, 500);
  check(full.height == std::min(gs.height, kStrikeMaxHeight) && half.height == full.height / 2 &&
            half.arcs == std::max(1, gs.arcs / 2),
        Format("half strength halves the bolt: height %d -> %d, arcs %d -> %d", full.height,
               half.height, full.arcs, half.arcs));

  // ---- C. the tariff rides the emission -----------------------------------------
  EffectInst le;
  le.verb = SpellVerb::Strike;
  le.glyph = gLight;
  SpellEmission em;
  ApplySpellEffect(lib, {le}, SpellFxVec{SpellFxFromFloat(10.5f), SpellFxFromFloat(20.5f),
                                         SpellFxFromFloat(30.5f)},
                   SpellFxVec{0, -kSpellFxOne, 0}, 1000, em);
  const int32_t tariff = EffectTariff(lib, le);
  check(em.strikes.size() == 1 && tariff > 0 && em.strikes[0].tariff == tariff,
        Format("the strike carries its tariff (%d, want %d)",
               em.strikes.empty() ? -1 : em.strikes[0].tariff, tariff));

  // ---- D. a ward at the struck cell ---------------------------------------------
  int wardRadius = 0;
  bool wardInside = false, wardAimOutside = false, wardFar = true;
  {
    SpellSystem ssys;
    ssys.SetLibrary(&lib);
    CasterState cs;
    cs.mana = cs.manaMax = 100000;
    struct Hp {
      int32_t hp = 100000;
    } hp;
    CasterHealth cb;
    cb.ctx = &hp;
    cb.get = [](void* p) { return ((Hp*)p)->hp; };
    cb.spend = [](void* p, int32_t a) { ((Hp*)p)->hp -= a; };
    const SpellFxVec wat{SpellFxFromFloat((float)aim.x + 0.5f), SpellFxFromFloat((float)aim.y + 0.5f),
                         SpellFxFromFloat((float)aim.z + 0.5f)};
    SpellStack st;
    for (const char* w : {"lightning", "null", "self"})
      if (lib.Find(w) >= 0) st.spoken.push_back(lib.Find(w));
    SpellEmission e;
    ssys.Cast(CompileSpell(lib, st), cs, cb, 9, wat, {kSpellFxOne, 0, 0}, 400, e, nullptr);
    if (!ssys.Filters().empty()) {
      wardRadius = ssys.Filters()[0].radius;
      // The aim just outside the edge, the struck cell inside it.
      wardAimOutside = !ssys.StrikeWarded(aim.x + wardRadius + 2, aim.y, aim.z);
      wardInside = ssys.StrikeWarded(aim.x + 1, aim.y, aim.z);
      wardFar = ssys.StrikeWarded(aim.x + 500, aim.y, aim.z);
    }
  }
  check(wardRadius > 0 && wardInside && wardAimOutside && !wardFar,
        Format("a strike ward refuses the struck cell inside it whatever the aim (radius %d, "
               "inside %d, aim-outside clear %d, far %d)",
               wardRadius, wardInside, wardAimOutside, wardFar));

  // ---- E. through the tick: refund on refusal, a leader, an emitted bolt --------
  struct Arm {
    uint64_t strikes = 0, leaders = 0, refunded = 0, emitted = 0, refused = 0;
    int ticksToFire = -1;
  };
  Arm shut, open;
  Regenerate(c);
  {
    const int fxX = o.x * (int)kChunk + 220, fxZ = o.z * (int)kChunk + 220;
    const int fy = FixtureYOver(fxX - 4, fxZ - 4, fxX + 4, fxZ + 4, kDefaultSeed, 3);
    uint32_t t = 83000;
    support::TickCursor tick{c, t, IVec3{fxX >> 4, fy >> 4, fxZ >> 4}};
    support::TickRig& rig = tick.Rig();
    std::string lerr;
    LoadGlyphs(AssetDir() + "/spells/glyphs.json", c.mats, rig.Glyphs(), lerr);
    PlayerSession& s = rig.Session();
    s.spells.SetLibrary(&rig.Glyphs());
    s.player.pos = Vec3{(float)fxX + 0.5f, (float)fy + 6.0f, (float)fxZ + 0.5f};
    SpellStack st;
    for (const char* w : {"lightning", "self"})
      if (rig.Glyphs().Find(w) >= 0) st.spoken.push_back(rig.Glyphs().Find(w));
    s.caster.hand[0].slot = 0;
    s.caster.hand[0].name = "lightning-self";
    s.caster.hand[0].compiled = CompileSpell(rig.Glyphs(), st);
    TickAuthorityCtx& w = rig.Authority();
    for (int i = 0; i < 6; i++) tick();  // the mirror publishes the fixture
    auto run = [&](Arm& a, uint32_t cap) {
      w.strikes.budget.cap = cap;
      s.caster.mana.manaMax = 100000;
      s.caster.mana.mana = 50000;
      const uint64_t st0 = w.strikes.spellStrikes, ld0 = w.strikes.spellLeaders,
                     rf0 = w.strikes.spellRefunded, em0 = w.strikes.budget.emitted,
                     rj0 = w.strikes.budget.refused;
      support::TickOps press;
      press.input.SetPressed(TB_ATTACK, true);
      press.input.SetHeld(TB_ATTACK, true);
      tick(press);
      for (int i = 0; i < 14 && w.strikes.spellStrikes == st0; i++) {
        tick();
        if (w.strikes.spellStrikes != st0) a.ticksToFire = i + 1;
      }
      if (a.ticksToFire < 0 && w.strikes.spellStrikes != st0) a.ticksToFire = 0;
      a.strikes = w.strikes.spellStrikes - st0;
      a.leaders = w.strikes.spellLeaders - ld0;
      a.refunded = w.strikes.spellRefunded - rf0;
      a.emitted = w.strikes.budget.emitted - em0;
      a.refused = w.strikes.budget.refused - rj0;
      w.strikes.budget.cap = kStrikeCellsPerTick;
      for (int i = 0; i < 20; i++) tick();  // the bolt decays before the next arm
    };
    run(shut, 0);
    run(open, kStrikeCellsPerTick);
  }
  Regenerate(c);
  check(shut.strikes == 1 && shut.refused == 1 && shut.emitted == 0 &&
            shut.refunded == (uint64_t)tariff,
        Format("a refused strike refunds its tariff (strikes %llu, refused %llu, refunded %llu, "
               "tariff %d)",
               (unsigned long long)shut.strikes, (unsigned long long)shut.refused,
               (unsigned long long)shut.refunded, tariff));
  check(open.strikes == 1 && open.emitted == 1 && open.refunded == 0,
        Format("an open budget emits the bolt (strikes %llu, emitted %llu)",
               (unsigned long long)open.strikes, (unsigned long long)open.emitted));
  check(shut.leaders == 1 && shut.ticksToFire >= 1 && shut.ticksToFire <= 10,
        Format("a search above the mirror waits a stepped leader (leaders %llu, fired after %d "
               "ticks)",
               (unsigned long long)shut.leaders, shut.ticksToFire));

  // ---- F. the sound slots ----------------------------------------------------------
  const auto& sp2 = audio::Cues::kSlotPrefix;
  check(sp2.count("zap") && sp2.at("zap") == "electric" && sp2.count("shock") &&
            sp2.at("shock") == "electric",
        "zap / shock slots under electric/");

  detail = Format(
      "pole top %d struck (hidden %d); no pole -> y %d; strength 500: h %d->%d arcs %d->%d; "
      "tariff %d; ward r %d; shut: leaders %llu, fired +%d, refunded %llu; open: emitted %llu, "
      "leaders %llu, fired +%d%s%s",
      pa.target.y - aim.y, pa.columnsHidden, pb.target.y - aim.y, full.height, half.height,
      full.arcs, half.arcs, tariff, wardRadius, (unsigned long long)shut.leaders,
      shut.ticksToFire, (unsigned long long)shut.refunded, (unsigned long long)open.emitted,
      (unsigned long long)open.leaders, open.ticksToFire, fails.empty() ? "" : "; FAILED: ",
      fails.c_str());
  std::printf("elec-strike-play: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& ElecPlayGates() {
  static const std::vector<Gate> g = {
      {"elec-strike-play", "sim", {}, false, GateElecStrikePlay},
  };
  return g;
}

}  // namespace selftest
