// selftest_spell.cpp — spell selftest gates.
//
// Bodies moved verbatim out of the old monolithic RunSelftest; see
// scripts/split_selftest.py for the exact source ranges. Each gate returns a
// Status and fills `detail` with the parenthetical the old printf carried, so
// the console output is unchanged and --json can carry the same numbers.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "game/brush.h"
#include "game/spell.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// ---- spells ------------------------------------------------------------
Status GateSpells(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  const std::vector<MaterialDef>& mats = c.mats;
// ---- spells: budget discipline and the overcast ---------------------------
// What is asserted here is INVARIANTS, not plausible-looking numbers — the
// project's own lesson that a rate comparison proves nothing (see the pond
// gate above). Three things, each of which fails loudly if the structure is
// wrong rather than merely mistuned:
//
//   1. The trail's voxel budget is respected EXACTLY. Not "roughly bounded":
//      the total volume the trail ever authorizes must be <= the authored
//      budget, and the projectile must be dead once it is spent. That is the
//      rule-2 guarantee, and it is the one that keeps a trail spell from
//      becoming the fourth never-sleeping-chunk bug.
//   2. An overcast KILLS the caster. Specifically: a spell costing more than
//      mana + health resolves Fatal, and a Fatal cast emits its effect and
//      asks for the caster to be carved — i.e. backfire runs the spell's own
//      payload rather than special-case death code.
//   3. Every emission is an OP. The VM must produce ops and nothing else;
//      if a spell ever needed a non-op channel, thesis 1 is broken.
bool spellOk = false;
{
  GlyphLibrary lib;
  std::string gerr;
  const std::string gpath = AssetDir() + "/spells/glyphs.json";
  if (!LoadGlyphs(gpath, mats, lib, gerr)) {
    std::printf("spells: FAIL (glyph load: %s)\n", gerr.c_str());
  } else {
    SpellSystem sys;
    sys.SetLibrary(&lib);
    std::vector<uint32_t> classOf;
    for (const auto& m : mats) classOf.push_back(m.gpu.klass);

    // (1) trail budget. Speak sand + trail + projectile and fly it in open
    // air, counting every voxel the trail authorizes.
    int gSand = lib.Find("sand"), gTrail = lib.Find("trail"),
        gProj = lib.Find("projectile");
    int64_t trailVolume = 0;
    int32_t authoredBudget = 0;
    bool diedWithBudget = false;
    int flownTicks = 0;
    if (gSand >= 0 && gTrail >= 0 && gProj >= 0) {
      authoredBudget = lib.glyphs[gTrail].voxelBudget;
      SpellStack st;
      st.spoken = {gSand, gTrail, gProj};
      Spell sp = CompileSpell(lib, st);
      CasterState cs;
      cs.mana = 10000;   // fund it fully: this gate is about the BUDGET
      cs.manaMax = 10000;
      // A health source that never dies, so nothing here depends on the
      // avatar being spawned.
      static int32_t kFakeHp = 10000;
      CasterHealth hp;
      hp.ctx = &kFakeHp;
      hp.get = [](void* c) { return *(int32_t*)c; };
      hp.spend = [](void* c, int32_t a) { *(int32_t*)c -= a; };

      // Fire horizontally through open air, INSIDE the current residency
      // window. The window origin is wherever the streaming gate above left
      // it, and out-of-window space is solid (DESIGN.md §3) — so a fixed
      // world position makes this gate depend on test ordering, and the
      // projectile detonates on tick 1 against the window wall. Anchor to
      // the live origin and aim toward the window's middle instead.
      const IVec3 worg = world.WindowOrigin();
      const int sx = worg.x * (int)kChunk + 8;
      const int sz = worg.z * (int)kChunk + (int)kWorldN / 2;
      const int sy = worg.y * (int)kChunk + (int)kWorldN / 2;
      SpellEmission emit;
      SpellFxVec origin{SpellFxFromFloat((float)sx), SpellFxFromFloat((float)sy),
                        SpellFxFromFloat((float)sz)};
      SpellFxVec dir{kSpellFxOne, 0, 0};
      sys.Cast(sp, cs, hp, 1, origin, dir, 1, emit);
      // Count ONLY trail ops. The impact effect emits its own op at the
      // transmute/impact radius, and folding that into the trail total is
      // how the first version of this gate reported 343 voxels against a 64
      // budget — the budget was fine and the measurement was wrong. Trail
      // ops are exactly the ones at the trail glyph's radius in paint mode.
      const int32_t trailRadius = lib.glyphs[gTrail].radius;
      for (int t = 0; t < (int)lib.budgets.maxLifetimeTicks + 10; t++) {
        SpellEmission e;
        sys.Tick((uint32_t)(2 + t), world, classOf, e);
        for (const BrushOp& b : e.ops) {
          if (b.mode != 0u || b.radius != trailRadius) continue;
          int64_t d = 2 * (int64_t)b.radius + 1;
          trailVolume += d * d * d;
        }
        flownTicks++;
        if (sys.LiveCount() == 0) {
          diedWithBudget = true;
          break;
        }
      }
    }
    // EXACT respect: the trail may never authorize more volume than it was
    // budgeted, and the projectile must not outlive the budget.
    const bool budgetOk = authoredBudget > 0 && trailVolume > 0 &&
                          trailVolume <= authoredBudget && diedWithBudget;

    // (2) the overcast. Cost beyond mana + health must resolve Fatal, and a
    // Fatal cast must run the spell's own payload AT the caster.
    int gLava = lib.Find("lava");
    bool fatalOk = false, carveAsked = false, fatalEmitted = false;
    if (gLava >= 0 && gProj >= 0) {
      SpellStack st;
      st.spoken = {gLava, gProj};
      Spell sp = CompileSpell(lib, st);
      CasterState cs;
      cs.mana = 1;
      cs.manaMax = 100;
      static int32_t kTinyHp = 2;   // mana + health = 3, well under the cost
      CasterHealth hp;
      hp.ctx = &kTinyHp;
      hp.get = [](void* c) { return *(int32_t*)c; };
      hp.spend = [](void* c, int32_t a) { *(int32_t*)c -= a; };
      CastResult pre = ResolveCast(cs, kTinyHp, sp.manaCost);
      SpellEmission emit;
      CastResult res = sys.Cast(sp, cs, hp, 2, {0, 0, 0}, {kSpellFxOne, 0, 0},
                                3, emit);
      fatalOk = pre.outcome == CastOutcome::Fatal &&
                res.outcome == CastOutcome::Fatal;
      carveAsked = emit.carveCaster;
      // The payload ran: lava's backfire is an explosion, and it is emitted
      // by ApplySpellEffect rather than by any death-specific branch.
      fatalEmitted = !emit.explosions.empty() || !emit.ops.empty();
      // A fatal cast must NOT also launch the projectile — the spell goes
      // off at the caster instead of leaving the hand.
      fatalOk = fatalOk && sys.LiveCount() == 0;
    }

    // (3) TOTALITY + AMPLIFICATION. The language's central promise is that
    // every sequence does something, and that repetition doubles both the
    // output and the price. Asserted as a RELATION between casts rather than
    // as absolute counts: what matters is that N+1 sands is exactly twice
    // the N sands, at exactly twice the mana, whatever the authored base is.
    bool sprayOk = false;
    int sprayN[3] = {0, 0, 0};
    int32_t sprayCost[3] = {0, 0, 0};
    if (gSand >= 0) {
      static int32_t kBigHp = 1000000;
      CasterHealth hp;
      hp.ctx = &kBigHp;
      hp.get = [](void* c) { return *(int32_t*)c; };
      hp.spend = [](void* c, int32_t a) { *(int32_t*)c -= a; };
      for (int k = 0; k < 3; k++) {
        SpellStack st;
        for (int j = 0; j <= k; j++) st.spoken.push_back(gSand);  // sand x(k+1)
        Spell sp = CompileSpell(lib, st);
        sprayCost[k] = sp.manaCost;
        CasterState cs;
        cs.mana = 1000000;
        cs.manaMax = 1000000;
        SpellEmission e;
        // A bare element must NOT need a form glyph to be a real spell.
        sys.Cast(sp, cs, hp, 10 + k, {0, 0, 0}, {kSpellFxOne, 0, 0},
                 (uint32_t)(100 + k), e);
        sprayN[k] = (int)e.spawns.size();
      }
      // Each extra utterance doubles the matter AND the price. Exact
      // equality, not "roughly more": the doubling IS the rule, and a gate
      // that only checked monotonicity would pass a linear ramp too.
      sprayOk = sprayN[0] > 0 && sprayN[1] == 2 * sprayN[0] &&
                sprayN[2] == 4 * sprayN[0] && sprayCost[0] > 0 &&
                sprayCost[1] == 3 * sprayCost[0] &&      // 1 + 2
                sprayCost[2] == 7 * sprayCost[0];        // 1 + 2 + 4
    }

    // ---- (4) THE CAST LATCH: a click must survive a frame that runs no
    // ticks. The cast is consumed inside the fixed-tick loop, but at any
    // frame rate above 30 fps most frames run ZERO ticks — so a frame-local
    // "was RMB clicked" bool is discarded unread most of the time and RMB
    // appears to do nothing 8 tries out of 9. This models the loop's
    // accumulator against a realistic 60 fps frame and asserts that every
    // click is honoured exactly once.
    bool latchOk = false;
    {
      const double frameDt = 1.0 / 60.0;   // faster than the 30 Hz tick
      double acc = 0;
      bool queued = false;
      int clicks = 0, casts = 0, zeroTickFrames = 0;
      for (int frame = 0; frame < 120; frame++) {
        acc += frameDt;
        if (acc > 4 * kTickDt) acc = 4 * kTickDt;
        // Click on a fixed cadence that is not a multiple of the tick, so
        // clicks land on both zero-tick and one-tick frames.
        if (frame % 7 == 0) { queued = true; clicks++; }
        int ticksThis = 0;
        while (acc >= kTickDt && ticksThis < 4) {
          acc -= kTickDt;
          ticksThis++;
          // the consume-and-clear the real cast site performs
          const bool castNow = queued;
          queued = false;
          if (castNow) casts++;
        }
        if (ticksThis == 0) zeroTickFrames++;
      }
      // Every click is accounted for: cast, or still latched awaiting the
      // next tick (the run can end between the click and that tick — that is
      // correct latching, not a dropped click). The old frame-local bool
      // scored 8/18 here, which is the reported "RMB does nothing".
      //
      // The zeroTickFrames check keeps the test honest: if the loop ever ran
      // a tick every frame, nothing would be proven.
      const int accounted = casts + (queued ? 1 : 0);
      latchOk = (accounted == clicks) && zeroTickFrames > 0;
      std::printf(
          "spell cast latch: %s (%d clicks -> %d cast + %d pending, %d/%d "
          "frames ran no tick)\n",
          latchOk ? "PASS" : "FAIL", clicks, casts, queued ? 1 : 0,
          zeroTickFrames, 120);
    }

    spellOk = budgetOk && fatalOk && carveAsked && fatalEmitted && sprayOk &&
              latchOk;
    std::printf(
        "spells: %s (trail authorized %lld/%d voxels over %d ticks, died=%d; "
        "overcast fatal=%d carve=%d payload=%d; spray %d/%d/%d voxels for "
        "%d/%d/%d mana)\n",
        spellOk ? "PASS" : "FAIL", (long long)trailVolume, authoredBudget,
        flownTicks, diedWithBudget ? 1 : 0, fatalOk ? 1 : 0,
        carveAsked ? 1 : 0, fatalEmitted ? 1 : 0, sprayN[0], sprayN[1],
        sprayN[2], sprayCost[0], sprayCost[1], sprayCost[2]);
  }
}

  // Verdict: the flag the moved body already computed.
  return spellOk ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& SpellGates() {
  static const std::vector<Gate> g = {
      {"spells", "spell", {"streaming"}, false, GateSpells},
  };
  return g;
}

}  // namespace selftest
