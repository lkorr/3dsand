// selftest_demon_seals.cpp — SEALS, CHANNELS, STRENGTH, RELEASE, GAZE
// (docs/PLAN_demons.md D3; game/demon_seals.h).
//
//   demon-seals  On a stone pad on the harness map, a 2-wide salt ring (D1's
//                fixture, radius demonSeals.ringCells), Skerrick cast into it
//                through the real queue and the real tick, the player's actor
//                standing just outside on +x, within an imp's reach across
//                the ring.
//                A. SEALED: a sulfur pile (north), an iron pile (east, beside
//                   you), two cells of quicksilver inlaid in the floor (west:
//                   below his blink resistance) and two lit candles in the
//                   band.
//                   0. IRON WEAKENS (D6): his effective power is his own
//                      minus potency x ironSusceptibility / 100 (capped);
//                   1. cast_out SEVERED: AllowCastOut to your position is
//                      refused, to a point inside the circle allowed;
//                   2. blink NOT severed (2 cells < resistance): AllowBlink
//                      to a point inside is allowed and he stays contained;
//                   3. SALT BLOCKS BLOWS (D6): over demonSeals.holdTicks the
//                      fence refused his blows across the ring;
//                   4. THE WIND: all but two sulfur cells cleared through the
//                      queue -> within demonSeals.flipTicksMax cast_out is
//                      open and AllowCastOut to you is allowed;
//                   5. RELEASE (TB_DEMON_RELEASE through TickInput): his
//                      power (20) is under the circle's strength -> RELEASED,
//                      not targeting, the def's `released` profile.
//                   Run twice from the same tick: the per-tick trace (centre,
//                   state, severed mask, strength) must be identical.
//                B. PLAIN RING, no piles:
//                   1. cast_out open: AllowCastOut to you allowed;
//                   2. no iron: no cut off his power -- and the salt alone
//                      still refuses his blows across the ring (D6);
//                   3. GAZE (Skerrick: avert): the summoner within range
//                      staring at his head -> strain rises, a full meter costs
//                      the circle strainPenalty; looking away lowers it;
//                   4. OVERPOWER: his power raised past the circle's strength
//                      -> release -> UNBOUND and hostile.
//                C. TELLS: the line table answers by margin band.
//
// Ticks THE tick (support::TickRig -> TickAuthority). Own pads, IdCounterScope,
// a pinned clear sky, regenerates on the way out (demon-circle's discipline).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "game/caster.h"
#include "game/demon.h"
#include "game/demon_seals.h"
#include "game/mob.h"
#include "game/session.h"
#include "game/spell.h"
#include "sim/weather.h"
#include "sim/worldgen_run.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

using namespace sandvox;

namespace selftest {
namespace {

uint32_t SMat(const Ctx& c, const char* name) {
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == name) return (uint32_t)i;
  return 0;
}

void SPut(std::vector<CellOp>& ops, int x, int y, int z, uint32_t word) {
  ops.push_back({World::SlotCellIndex({x, y, z}), word});
}

void SRegenerate(Ctx& c) {
  c.mobs.Reset();
  c.debris.Reset();
  c.stream.OnRegen();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
}

struct SFix {
  int x = 0, y = 0, z = 0;
  IVec3 chunk{};
  std::vector<CellOp> pad, ring, piles;
  std::vector<IVec3> sulfurAt;
};

// D1's fixture (selftest_demon.cpp Build): a stone pad, air over it, a 2-wide
// salt ring, plus (when `sealed`) the piles.
SFix SBuild(Ctx& c, int r, bool sealed) {
  SFix f;
  const uint32_t mStone = SMat(c, "stone"), mSalt = SMat(c, "salt"), mSulfur = SMat(c, "sulfur"),
                 mIron = SMat(c, "iron"), mQuick = SMat(c, "quicksilver"),
                 mTallow = SMat(c, "tallow"), mFlame = SMat(c, "candle_flame");
  const IVec3 o = c.world.WindowOrigin();
  const int hx = r + 12, hz = r + 12;
  f.x = o.x * (int)kChunk + 200;
  f.z = o.z * (int)kChunk + 200;
  f.y = FixtureYOver(f.x - hx - 2, f.z - hz - 2, f.x + hx + 2, f.z + hz + 2, kDefaultSeed, 3);
  for (int zz = f.z - hz; zz <= f.z + hz; zz++)
    for (int xx = f.x - hx; xx <= f.x + hx; xx++) {
      for (int yy = World::TerrainHeight(xx, zz, kDefaultSeed) - 2; yy < f.y; yy++)
        SPut(f.pad, xx, yy, zz, PackVoxNew(mStone, 0));
      for (int yy = f.y; yy <= f.y + 16; yy++) SPut(f.pad, xx, yy, zz, 0u);
    }
  for (int dz = -r - 2; dz <= r + 2; dz++)
    for (int dx = -r - 2; dx <= r + 2; dx++) {
      const int d = (int)std::floor(std::sqrt((double)(dx * dx + dz * dz)));
      if (d == r || d == r + 1) SPut(f.ring, f.x + dx, f.y, f.z + dz, PackVoxNew(mSalt, 0));
    }
  if (sealed) {
    const int pr = r + 5;   // just outside the ring, inside the band
    // NORTH: sulfur, 3x3 + 1 (cast_out).
    for (int dx = -1; dx <= 1; dx++)
      for (int dz = -1; dz <= 1; dz++) {
        SPut(f.piles, f.x + dx, f.y, f.z - pr + dz, PackVoxNew(mSulfur, 0));
        f.sulfurAt.push_back({f.x + dx, f.y, f.z - pr + dz});
      }
    SPut(f.piles, f.x, f.y + 1, f.z - pr, PackVoxNew(mSulfur, 0));
    f.sulfurAt.push_back({f.x, f.y + 1, f.z - pr});
    // EAST, beside the player: iron, 3x3 + 1 (D6: weakens him). Off the radial line
    // the player's reach crosses (z + 4).
    for (int dx = -1; dx <= 1; dx++)
      for (int dz = -1; dz <= 1; dz++)
        SPut(f.piles, f.x + pr + dx, f.y, f.z + 6 + dz, PackVoxNew(mIron, 0));
    SPut(f.piles, f.x + pr, f.y + 1, f.z + 6, PackVoxNew(mIron, 0));
    // WEST: two cells of quicksilver inlaid in the floor (a liquid in a pit
    // does not run): below Skerrick's blink resistance.
    SPut(f.piles, f.x - pr, f.y - 1, f.z, PackVoxNew(mQuick, 7));
    SPut(f.piles, f.x - pr, f.y - 1, f.z + 1, PackVoxNew(mQuick, 7));
    // SOUTH-WEST and SOUTH-EAST: two candles (tallow, a flame on top).
    for (int sx : {-1, 1}) {
      const int cx = f.x + sx * 13, cz = f.z + 13;
      SPut(f.piles, cx, f.y, cz, PackVoxNew(mTallow, 0));
      SPut(f.piles, cx, f.y + 1, cz, PackVoxNew(mTallow, 0));
      SPut(f.piles, cx, f.y + 2, cz, PackVoxNew(mFlame, 0));
    }
  }
  f.chunk = {f.x >> 4, f.y >> 4, f.z >> 4};
  return f;
}

struct Row {
  int32_t qx = 0, qz = 0;
  int state = -1, severed = -1, strength = 0;
  bool operator==(const Row& o) const {
    return qx == o.qx && qz == o.qz && state == o.state && severed == o.severed &&
           strength == o.strength;
  }
};

struct SealsOut {
  bool spawned = false;
  std::string why;
  DemonState arrived = DemonState::Unbound;
  demon::SealReading reading;
  int strength0 = 0, power = 0;
  uint8_t severed0 = 0;
  // the hooks
  bool castOutToYou = true, castOutInside = false;
  bool blinkInside = false, stillContainedAfterBlink = false;
  uint32_t blowsRefused = 0;
  bool blowAcrossRefused = false;   // D6: the salt refuses a blow across the ring
  int heldTicks = 0;
  int basePower = 0, ironCut = 0;
  // the wind
  int flipAfter = -1;
  bool castOutAfterWind = false;
  int potencyAfterWind = -1;
  // gaze
  int strainAfterStare = 0, lapses = 0, strengthBeforeGaze = 0, strengthAfterGaze = 0;
  int strainAfterAvert = 0;
  // release
  demon::ReleaseResult release = demon::ReleaseResult::None;
  DemonState afterRelease = DemonState::Contained;
  bool targetingAfter = false;
  bool releasedProfile = false;
  int releaseMargin = 0;
  std::vector<Row> trace;
};

SealsOut RunSeals(Ctx& c, int r, bool sealed, int holdTicks, int flipMax, int gazeTicks,
                  const GlyphLibrary& lib, int gSummon, uint32_t t0, uint64_t idBase) {
  SealsOut out;
  SRegenerate(c);
  c.mobs.SetNextIdCounter(idBase);
  SFix f = SBuild(c, r, sealed);
  support::TickRig rig(c, t0, f.chunk);
  rig.Glyphs() = lib;
  // The summoner stands WEST of the ring (below) and, unless a step says
  // otherwise, looks further west: away from an `avert` demon, so the gaze
  // costs the circle nothing outside the gaze step (TickInput's default
  // forward, +x, would be a stare across the ring at him).
  TickInput away;
  away.lookFwd = Vec3{-1, 0, 0};
  auto tick = [&](const std::vector<CellOp>& cells = {}, std::optional<TickInput> in = {}) {
    support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) {
      o.cells = cells;
      o.input = in ? *in : away;
    });
  };
  for (size_t i = 0; i < f.pad.size(); i += kMaxCellOpsPerTick / 2)
    tick(std::vector<CellOp>(f.pad.begin() + (ptrdiff_t)i,
                             f.pad.begin() + (ptrdiff_t)std::min(f.pad.size(),
                                                                 i + kMaxCellOpsPerTick / 2)));
  tick(f.ring);
  if (!f.piles.empty()) tick(f.piles);
  const int settle = (int)BaselineNumber("demonSeals.settleTicks", 30);
  for (int i = 0; i < settle; i++) tick();
  const Vec3 you{(float)(f.x + r + 4) + 0.5f, (float)f.y + 8.5f, (float)f.z + 0.5f};
  c.mobs.SetPlayerActor(you, 3.0f, 17.0f, true);
  // The summoner's camera (the gaze): west of the ring, outside it, in range.
  rig.Session().player.pos = Vec3{(float)(f.x - r - 8) + 0.5f, (float)f.y, (float)f.z + 0.5f};
  {
    EffectInst e;
    e.verb = SpellVerb::Summon;
    e.glyph = gSummon;
    SpellEmission em;
    ApplySpellEffect(lib, {e},
                     SpellFxVec{SpellFxFromFloat(f.x + 0.5f), SpellFxFromFloat(f.y + 1.5f),
                                SpellFxFromFloat(f.z + 0.5f)},
                     SpellFxVec{0, -kSpellFxOne, 0}, 1000, em);
    if (em.summons.size() != 1) {
      out.why = "the VM reported no summoning";
      return out;
    }
    SessionTick st{&rig.Session(), {}, {}};
    DemonQueueSummon(rig.Authority(), std::span<SessionTick>(&st, 1), rig.Session().index,
                     em.summons[0], lib, rig.tick);
  }
  DemonWorld& dw = Demons(rig.Authority());
  for (int i = 0; i < 40 && dw.live.empty(); i++) tick();
  if (dw.live.empty()) {
    out.why = "nothing arrived";
    return out;
  }
  const uint64_t id = dw.live[0].mobId;
  out.spawned = true;
  out.arrived = dw.live[0].state;
  out.why = dw.live[0].why;
  if (out.arrived != DemonState::Contained) return out;
  const CircleShape circle = dw.live[0].circle;
  const Vec3 inside{circle.cx, (float)circle.feetY, circle.cz};
  auto ld = [&]() -> const LiveDemon* { return dw.Find(id); };
  auto record = [&]() {
    const Mob* m = c.mobs.FindMobById(id);
    const LiveDemon* d = ld();
    Row row;
    if (m && m->Def()) {
      row.qx = (int32_t)std::lround((m->Origin().x + m->Def()->worldSize.x * 0.5f) * 16);
      row.qz = (int32_t)std::lround((m->Origin().z + m->Def()->worldSize.z * 0.5f) * 16);
    }
    if (d) {
      row.state = (int)d->state;
      row.severed = d->bind.severed;
      row.strength = d->bind.strength;
    }
    out.trace.push_back(row);
  };
  // D6: this gate measures D3's seals. In the plain ring D6's malice would gust
  // the ring open (demon-malice's G), so here Skerrick knows no schemes.
  for (DemonDef& def : dw.lib.defs)
    if (def.id == dw.live[0].demon) def.schemes.clear();
  out.reading = ld()->bind.reading;
  out.strength0 = ld()->bind.strength;
  out.power = ld()->bind.power;
  out.basePower = ld()->bind.basePower;
  out.ironCut = ld()->bind.ironCut;
  out.severed0 = ld()->bind.severed;
  // ---- the hooks D4 will call ----------------------------------------------------
  TickAuthorityCtx& w = rig.Authority();
  out.castOutToYou = demon::AllowCastOut(w, id, inside, you);
  out.castOutInside = demon::AllowCastOut(w, id, inside, inside);
  {
    SessionTick st{&rig.Session(), {}, {}};
    out.blinkInside = demon::AllowBlink(w, std::span<SessionTick>(&st, 1), id, inside, rig.tick);
  }
  out.stillContainedAfterBlink = ld() && ld()->state == DemonState::Contained;
  // ---- the hold: blows across the ring ---------------------------------------------
  for (int i = 0; i < holdTicks; i++) {
    tick();
    record();
    if (!ld() || ld()->state != DemonState::Contained) break;
    out.heldTicks++;
  }
  if (const LiveDemon* d = ld()) {
    out.blowsRefused = d->blowsRefused;
    // The blow rule, asked directly (D6: he no longer swings at the salt when
    // he cannot reach -- plan item 10 -- so the fence is asked here).
    if (d->state == DemonState::Contained)
      out.blowAcrossRefused = !c.mobs.Fence().Allows(id, MobFence::Blow, inside.x, inside.z, you.x, you.z);
  }
  if (sealed) {
    // ---- the wind: all but two sulfur cells blown off, through the queue ------------
    std::vector<CellOp> gone;
    const uint32_t mSulfur = SMat(c, "sulfur");
    for (int dz = -3; dz <= 3; dz++)
      for (int dx = -3; dx <= 3; dx++)
        for (int dy = -1; dy <= 2; dy++) {
          const IVec3 p{f.x + dx, f.y + dy, f.z - (r + 5) + dz};
          if (dy >= 0 && !(dx == 0 && dz == 0 && dy == 0) && !(dx == 1 && dz == 0 && dy == 0))
            SPut(gone, p.x, p.y, p.z, 0u);
        }
    // ...keeping two cells (the centre and its east neighbour) as whole cells.
    SPut(gone, f.x, f.y, f.z - (r + 5), PackVoxNew(mSulfur, 0));
    SPut(gone, f.x + 1, f.y, f.z - (r + 5), PackVoxNew(mSulfur, 0));
    tick(gone);
    record();
    for (int i = 1; i <= flipMax; i++) {
      const LiveDemon* d = ld();
      if (d && d->state == DemonState::Contained && !d->bind.Severed(demon::Channel::CastOut)) {
        out.flipAfter = i;
        out.potencyAfterWind = d->bind.reading.potency[(size_t)demon::Channel::CastOut];
        break;
      }
      tick();
      record();
    }
    out.castOutAfterWind = demon::AllowCastOut(w, id, inside, you);
  } else {
    // ---- the gaze: stare at an `avert` demon, then look away -------------------------
    out.strengthBeforeGaze = ld() ? ld()->bind.strength : 0;
    auto lookAt = [&](bool at) {
      TickInput in;
      const Mob* m = c.mobs.FindMobById(id);
      if (m && m->Def()) {
        const Vec3 ws = m->Def()->worldSize;
        const Vec3 head = m->Origin() + Vec3{ws.x * 0.5f, ws.y * 0.85f, ws.z * 0.5f};
        const Vec3 v = (head - rig.Session().player.EyePos()).normalized();
        in.lookFwd = at ? v : v * -1.0f;
      }
      return in;
    };
    for (int i = 0; i < gazeTicks; i++) {
      tick({}, lookAt(true));
      record();
    }
    if (const LiveDemon* d = ld()) {
      out.strainAfterStare = d->bind.strain;
      out.lapses = (int)d->bind.lapses;
      out.strengthAfterGaze = d->bind.strength;
    }
    for (int i = 0; i < 30; i++) {
      tick({}, lookAt(false));
      record();
    }
    if (const LiveDemon* d = ld()) out.strainAfterAvert = d->bind.strain;
    // ---- overpower: a strong demon in this circle --------------------------------------
    for (DemonDef& def : dw.lib.defs)
      if (def.id == ld()->demon) def.power = 1000;
  }
  // ---- release, through TickInput -----------------------------------------------------
  {
    TickInput in = away;
    in.SetPressed(TB_DEMON_RELEASE, true);
    tick({}, in);
    record();
  }
  if (const LiveDemon* d = ld()) {
    out.release = d->bind.release;
    out.releaseMargin = d->bind.releaseMargin;
    out.afterRelease = d->state;
  }
  if (const ai::Brain* b = c.mobs.MobBrain(id)) {
    out.targetingAfter = b->hasTarget;
    out.releasedProfile = b->profile >= 0 && b->profile == c.mobs.Behaviors().Find("imp_bound");
  }
  for (int i = 0; i < 5; i++) {
    tick();
    record();
  }
  return out;
}

}  // namespace

Status GateDemonSeals(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  std::string fails;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) {
      fails += (fails.empty() ? "" : "; ") + what;
      std::printf("demon-seals: FAILED %s\n", what.c_str());
    }
  };
  for (const char* m : {"salt", "sulfur", "iron", "quicksilver", "tallow", "candle_flame"})
    if (SMat(c, m) == 0) {
      detail = std::string("no `") + m + "` material";
      return Status::Fail;
    }
  GlyphLibrary lib;
  std::string gerr;
  if (!LoadGlyphs(AssetDir() + "/spells/glyphs.json", c.mats, lib, gerr)) {
    detail = "glyph load failed: " + gerr;
    return Status::Fail;
  }
  const int gSummon = lib.Find("summon_skerrick");
  DemonLibrary dl;
  std::string dlog;
  LoadDemons(AssetDir() + "/demons", dl, dlog);
  check(gSummon >= 0, "summon_skerrick exists");
  check(dl.seals.seals.size() >= 4, Format("seals.json loaded %zu seals", dl.seals.seals.size()));
  check(dl.Find("seals") == nullptr && dl.Find("tells") == nullptr,
        "seals.json / tells.json are not read as demons");
  check(dlog.empty(), "the demon library loads clean: " + dlog);
  if (!fails.empty()) {
    detail = fails;
    return Status::Fail;
  }

  // ---- C. tells ----------------------------------------------------------------
  {
    const std::string& hi = demon::TellFor(dl.seals, "skerrick", 1000, 7);
    const std::string& lo = demon::TellFor(dl.seals, "skerrick", -1000, 7);
    const std::string& dflt = demon::TellFor(dl.seals, "nobody", -5, 7);
    const std::string& again = demon::TellFor(dl.seals, "skerrick", -1000, 7);
    check(!hi.empty() && !lo.empty() && hi != lo && lo == again,
          "C: Skerrick's tells differ by margin and repeat for the same salt");
    check(!dflt.empty(), "C: the default table answers for a demon with none");
  }

  const int r = (int)BaselineNumber("demonSeals.ringCells", 14);
  const int hold = (int)BaselineNumber("demonSeals.holdTicks", 120);
  const int flipMax = (int)BaselineNumber("demonSeals.flipTicksMax", 40);
  const int gazeTicks = (int)BaselineNumber("demonSeals.gazeTicks", 120);
  const std::string weatherWas = weather::Override();
  weather::SetOverride("clear");
  const uint64_t ids0 = c.mobs.NextIdCounter();
  const SealsOut a = RunSeals(c, r, true, hold, flipMax, gazeTicks, lib, gSummon, 95000u, ids0);
  const SealsOut a2 = RunSeals(c, r, true, hold, flipMax, gazeTicks, lib, gSummon, 95000u, ids0);
  const SealsOut b = RunSeals(c, r, false, hold, flipMax, gazeTicks, lib, gSummon, 96000u, ids0);
  weather::SetOverride(weatherWas);
  SRegenerate(c);

  using demon::Channel;
  auto sev = [](uint8_t m, Channel ch) { return ((m >> (int)ch) & 1u) != 0; };
  const auto& ra = a.reading;
  const std::string readA = Format(
      "salt %d/8, potency move %d cast_out %d blink %d, iron %d, %d candles, %d cells read",
      ra.saltEighths, ra.potency[0], ra.potency[1], ra.potency[2], ra.weakenPotency,
      ra.candlesLit, ra.cellsRead);
  std::printf("demon-seals: A reading: %s; strength %d vs power %d\n", readA.c_str(), a.strength0,
              a.power);
  check(a.spawned && a.arrived == DemonState::Contained,
        "A: Skerrick arrived contained (" + a.why + ")");
  check(b.spawned && b.arrived == DemonState::Contained,
        "B: Skerrick arrived contained (" + b.why + ")");
  if (!fails.empty()) {
    detail = fails;
    return Status::Fail;
  }
  check(ra.valid && ra.candlesLit >= 2, Format("A: the band read the candles (%d)", ra.candlesLit));
  check(sev(a.severed0, Channel::Move), "A: move severed (the salt holds his legs)");
  check(sev(a.severed0, Channel::CastOut) && !a.castOutToYou && a.castOutInside,
        Format("A1: sulfur severs cast_out (potency %d): a cast at you refused, inside allowed",
               ra.potency[1]));
  check(!sev(a.severed0, Channel::Blink) && a.blinkInside && a.stillContainedAfterBlink,
        Format("A2: two cells of quicksilver (potency %d) leave blink open: a blink inside "
               "allowed, still contained",
               ra.potency[2]));
  {
    // A0. IRON WEAKENS (D6): the cut is potency x susceptibility / 100, capped.
    const DemonDef* sk = dl.Find("skerrick");
    const int want = sk ? std::min(ra.weakenPotency * sk->ironSusceptibility / 100,
                                   a.basePower * dl.seals.weakenCapPct / 100)
                        : -1;
    check(ra.weakenPotency > 0 && a.ironCut > 0 && a.ironCut == want &&
              a.power == a.basePower - a.ironCut,
          Format("A0: iron (potency %d) cuts his power %d -> %d (cut %d, expected %d)",
                 ra.weakenPotency, a.basePower, a.power, a.ironCut, want));
  }
  check(a.blowAcrossRefused,
        Format("A3: the salt refuses his blows across the ring (%u refused while held)",
               a.blowsRefused));
  check(a.heldTicks >= hold, Format("A: contained %d of %d ticks", a.heldTicks, hold));
  check(a.flipAfter >= 0 && a.flipAfter <= flipMax && a.castOutAfterWind,
        Format("A4: the wind leaves two sulfur cells -> cast_out open after %d ticks (max %d; "
               "potency now %d)",
               a.flipAfter, flipMax, a.potencyAfterWind));
  check(a.release == demon::ReleaseResult::Held && a.afterRelease == DemonState::Released &&
            !a.targetingAfter && a.releasedProfile,
        Format("A5: release with power %d under strength (margin %d) -> released, calm, "
               "imp_bound (result %d, state %s, targeting %d)",
               a.power, a.releaseMargin, (int)a.release, DemonStateName(a.afterRelease),
               (int)a.targetingAfter));
  check(!a.trace.empty() && a.trace == a2.trace,
        Format("A: the twice-run trace differs (%zu vs %zu rows)", a.trace.size(), a2.trace.size()));

  check(!sev(b.severed0, Channel::CastOut) && b.castOutToYou,
        "B1: no sulfur -> a cast at you is allowed");
  check(b.ironCut == 0 && b.power == b.basePower && b.blowAcrossRefused,
        Format("B2: no iron -> no cut (power %d of %d); the salt alone still refuses his blows "
               "across the ring (%u refused)",
               b.power, b.basePower, b.blowsRefused));
  check(b.lapses >= 1 && b.strengthAfterGaze < b.strengthBeforeGaze,
        Format("B3: staring at an avert demon fills the strain (%d lapses) and costs the circle "
               "(strength %d -> %d)",
               b.lapses, b.strengthBeforeGaze, b.strengthAfterGaze));
  check(b.strainAfterAvert < b.strainAfterStare || b.strainAfterStare == 0,
        Format("B3: looking away lowers the strain (%d -> %d)", b.strainAfterStare,
               b.strainAfterAvert));
  check(b.release == demon::ReleaseResult::Overpowered && b.afterRelease == DemonState::Unbound &&
            b.targetingAfter,
        Format("B4: power 1000 in a plain ring (margin %d) -> overpowered, unbound, hunting "
               "(result %d, state %s)",
               b.releaseMargin, (int)b.release, DemonStateName(b.afterRelease)));
  RecordObserved("demonSeals.strengthSealed", a.strength0);
  RecordObserved("demonSeals.strengthPlain", b.strength0);
  RecordObserved("demonSeals.flipAfter", a.flipAfter);
  detail = Format(
      "A sealed: %s; strength %d vs power %d-%d; cast_out %s, blink %s, %u blows refused; "
      "wind -> cast_out open +%d; release %s (margin %d); trace %zu rows %s. B plain: strength "
      "%d, %u blows refused; gaze %d lapse(s), strength %d -> %d, strain %d -> %d looking away; "
      "power 1000 release %s (margin %d)",
      readA.c_str(), a.strength0, a.basePower, a.ironCut,
      sev(a.severed0, Channel::CastOut) ? "severed" : "OPEN",
      sev(a.severed0, Channel::Blink) ? "SEVERED" : "open", a.blowsRefused, a.flipAfter,
      DemonStateName(a.afterRelease), a.releaseMargin, a.trace.size(),
      a.trace == a2.trace ? "identical twice" : "DIFFERS", b.strength0, b.blowsRefused, b.lapses,
      b.strengthBeforeGaze, b.strengthAfterGaze, b.strainAfterStare, b.strainAfterAvert,
      DemonStateName(b.afterRelease), b.releaseMargin);
  if (!fails.empty()) detail += "; FAILED: " + fails;
  std::printf("demon-seals: %s (%s)\n", fails.empty() ? "PASS" : "FAIL", detail.c_str());
  return fails.empty() ? Status::Pass : Status::Fail;
}

}  // namespace selftest
