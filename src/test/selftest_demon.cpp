// selftest_demon.cpp — DEMONS (docs/PLAN_demons.md D1; game/demon.h).
//
//   demon-circle   A. THE NAME (CPU): `summon_skerrick` is a summon glyph naming
//                     `skerrick`; the VM reports it as a SpellSummon (never a
//                     brush op) at its tariff; the debug default does NOT grant
//                     it and SANDVOX_ALL_NAMES's path does; the demon library
//                     resolves Skerrick to a loaded mob def and behaviour.
//                  B. On a stone pad on the harness map, a salt ring poured on
//                     the floor (radius demonCircle.ringCells), the player's
//                     actor standing just outside it, the name cast at the
//                     ring's centre through the real queue (DemonQueueSummon,
//                     what phase I calls) and the real tick:
//                       1. CLOSED: Skerrick arrives CONTAINED; for
//                          demonCircle.holdTicks his footprint centre never
//                          leaves the circle while his brain targets you, and
//                          the fence refused moves (he tried) -- and blows,
//                          because you stand in his reach across the ring.
//                       2. BROKEN: one ring cell is cleared (a CellOp: the
//                          queue) -> within demonCircle.breakTicksMax he is
//                          UNBOUND, and within demonCircle.leaveTicksMax his
//                          centre is outside the old circle.
//                     Run twice from the same tick: the per-tick trace
//                     (position, state) must be identical.
//                  C. NO RING: the same cast on the bare pad -> UNBOUND at
//                     once (the goof).
//                  D. A GAP: the ring with one radial line missing at build
//                     time -> not a circle -> UNBOUND at once.
//                  E. THE BODY IS FLESH at under a metre: Skerrick's def on a
//                     bare pad burns when lit, bleeds blood from a blast
//                     beside it, and dies to a killing blow.
//
// Ticks THE tick (support::TickRig -> TickAuthority): the demon world ticks
// before phase H, the fence is asked in mobs.PreTick. Every fixture is built on
// its own stone pad over the harness terrain (FixtureYOver), spawns under an
// IdCounterScope, pins a clear sky (salt dissolves in rain) and regenerates
// the world on the way out.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "game/caster.h"
#include "game/demon.h"
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

uint32_t MatId(const Ctx& c, const char* name) {
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == name) return (uint32_t)i;
  return 0;
}

void Put(std::vector<CellOp>& ops, int x, int y, int z, uint32_t word) {
  ops.push_back({World::SlotCellIndex({x, y, z}), word});
}

void Regenerate(Ctx& c) {
  c.mobs.Reset();
  c.debris.Reset();
  c.stream.OnRegen();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
}

void Tick(support::TickRig& rig, const std::vector<CellOp>& cells = {}) {
  support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) { o.cells = cells; });
}

// ---- the fixture -------------------------------------------------------------
// A stone pad, air above it, and (when `ring`) a salt ring on the floor round
// (x, z): every column whose distance from the centre floors to r or r + 1.
// TWO wide on purpose: salt is a powder, and a one-wide digital circle's
// diagonal steps are single grains touching only at corners, which repose
// carries off within a few ticks (measured: 22 of 88 gone before an arrival).
// The Harrowby cellar's ring is the same shape (D2). Closed to a 4-connected
// fill by construction (a step changes the distance by at most 1).
struct Fix {
  int x = 0, y = 0, z = 0;
  IVec3 chunk{};
  std::vector<CellOp> pad, ring;
  std::vector<IVec3> ringAt;   // the ring's cells, world coords
};

Fix Build(Ctx& c, int r, bool ring, bool gap) {
  Fix f;
  const uint32_t mStone = MatId(c, "stone"), mSalt = MatId(c, "salt");
  const IVec3 o = c.world.WindowOrigin();
  const int hx = r + 10, hz = r + 10;
  f.x = o.x * (int)kChunk + 200;
  f.z = o.z * (int)kChunk + 200;
  f.y = FixtureYOver(f.x - hx - 2, f.z - hz - 2, f.x + hx + 2, f.z + hz + 2, kDefaultSeed, 3);
  for (int zz = f.z - hz; zz <= f.z + hz; zz++)
    for (int xx = f.x - hx; xx <= f.x + hx; xx++) {
      for (int yy = World::TerrainHeight(xx, zz, kDefaultSeed) - 2; yy < f.y; yy++)
        Put(f.pad, xx, yy, zz, PackVoxNew(mStone, 0));
      for (int yy = f.y; yy <= f.y + 16; yy++) Put(f.pad, xx, yy, zz, 0u);
    }
  // The gap / the break: the band's radial line due +x of the centre
  // (toward the player, who stands on +x) -- two cells, r and r + 1.
  if (ring)
    for (int dz = -r - 2; dz <= r + 2; dz++)
      for (int dx = -r - 2; dx <= r + 2; dx++) {
        const int d = (int)std::floor(std::sqrt((double)(dx * dx + dz * dz)));
        if (d != r && d != r + 1) continue;
        if (gap && dz == 0 && dx > 0) continue;
        Put(f.ring, f.x + dx, f.y, f.z + dz, PackVoxNew(mSalt, 0));
        f.ringAt.push_back(IVec3{f.x + dx, f.y, f.z + dz});
      }
  f.chunk = {f.x >> 4, f.y >> 4, f.z >> 4};
  return f;
}

struct TraceRow {
  int32_t qx = 0, qz = 0;   // footprint centre, 1/16 voxel
  int state = -1;
  bool operator==(const TraceRow& o) const {
    return qx == o.qx && qz == o.qz && state == o.state;
  }
};

struct RunOut {
  bool spawned = false;
  uint64_t id = 0;
  DemonState arrived = DemonState::Unbound;
  CircleVerdict verdict = CircleVerdict::Unknown;
  int inside = 0, ringSalt = 0;
  // the hold
  int heldTicks = 0, leftWhileHeld = 0, targetedTicks = 0;
  uint32_t movesRefused = 0, blowsRefused = 0;
  uint32_t blowsLoophole = 0;   // D3: let through (no iron: `touch` unsevered)
  float maxR = 0;           // furthest the centre got from the circle's centroid
  float circleR = 0;
  // the break
  int unboundAfter = -1, outsideAfter = -1;
  std::vector<TraceRow> trace;
  std::string why;
};

// One fixture through the real tick. `breakAt` < 0: no break.
RunOut RunFix(Ctx& c, int r, bool ring, bool gap, int holdTicks, int breakTicks, int leaveTicks,
              const GlyphLibrary& lib, int gSummon, uint32_t t0, uint64_t idBase) {
  RunOut out;
  Regenerate(c);
  // THE SAME MOB ID EVERY RUN: ids seed per-creature variance, so the
  // twice-run comparison has to spawn the same id twice.
  c.mobs.SetNextIdCounter(idBase);
  Fix f = Build(c, r, ring, gap);
  support::TickRig rig(c, t0, f.chunk);
  rig.Glyphs() = lib;
  // The pad in slices under the per-tick cell-op cap (kMaxCellOpsPerTick:
  // past it the tail is TRUNCATED, and a truncated pad is a floor with holes
  // the ring's grains fall into -- measured, 88,520 ops lost in one tick).
  for (size_t i = 0; i < f.pad.size(); i += kMaxCellOpsPerTick / 2)
    Tick(rig, std::vector<CellOp>(f.pad.begin() + (ptrdiff_t)i,
                                  f.pad.begin() + (ptrdiff_t)std::min(
                                      f.pad.size(), i + kMaxCellOpsPerTick / 2)));
  Tick(rig, f.ring);
  // The pour SETTLES before the name is cast, as a real one would have.
  const int settle = (int)BaselineNumber("demonCircle.settleTicks", 30);
  for (int i = 0; i < settle; i++) Tick(rig);
  // THE PLAYER: an actor standing just outside the ring on +x, inside an
  // imp's reach of the ring's inner edge, so a contained imp has something to
  // want and to swing at across the salt.
  const Vec3 you{(float)(f.x + r + 4) + 0.5f, (float)f.y + 8.5f, (float)f.z + 0.5f};
  c.mobs.SetPlayerActor(you, 3.0f, 17.0f, true);
  // THE CAST: the VM's report of the name at the ring's centre, handed to the
  // demon world exactly as phase I hands it.
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
  for (int i = 0; i < 40 && dw.live.empty(); i++) Tick(rig);
  if (dw.live.empty()) {
    out.why = "nothing arrived";
    return out;
  }
  const LiveDemon arrived = dw.live[0];
  // THE RING AS THE SNAPSHOT SEES IT (attribution: a ring the scan calls
  // open names the cell that is not salt, and what it is instead).
  if (ring) {
    const WorldSnapshot& sn = c.world.Snap();
    const uint32_t mSalt = MatId(c, "salt");
    int salt = 0, other = 0, unseen = 0;
    std::string first;
    for (const IVec3& p : f.ringAt) {
      const int cx = (p.x >> 4) - sn.mirrorBase.x, cy = (p.y >> 4) - sn.mirrorBase.y,
                cz = (p.z >> 4) - sn.mirrorBase.z;
      if (!sn.valid || cx < 0 || cy < 0 || cz < 0 || cx > 2 || cy > 2 || cz > 2) {
        unseen++;
        continue;
      }
      const uint32_t m = sn.mirror[(size_t)((cz * 3 + cy) * 3 + cx) * kChunkVol +
                                   (size_t)(((p.z & 15) * 16 + (p.y & 15)) * 16 + (p.x & 15))] &
                         0xFFFu;
      if (m == mSalt) salt++;
      else {
        other++;
        if (first.empty())
          first = Format("(%d,%d,%d) is %s", p.x, p.y, p.z,
                         m < c.mats.size() ? c.mats[m].name.c_str() : "?");
      }
    }
    // ...and the radial line due +x (the gap, or where the break will be),
    // y from the floor to two above it.
    std::string line;
    for (int dx = r - 1; dx <= r + 2; dx++)
      for (int y = f.y - 1; y <= f.y + 1; y++) {
        const IVec3 p{f.x + dx, y, f.z};
        const int cx = (p.x >> 4) - sn.mirrorBase.x, cy = (p.y >> 4) - sn.mirrorBase.y,
                  cz = (p.z >> 4) - sn.mirrorBase.z;
        if (!sn.valid || cx < 0 || cy < 0 || cz < 0 || cx > 2 || cy > 2 || cz > 2) continue;
        const uint32_t w = sn.mirror[(size_t)((cz * 3 + cy) * 3 + cx) * kChunkVol +
                                     (size_t)(((p.z & 15) * 16 + (p.y & 15)) * 16 + (p.x & 15))];
        const uint32_t m = w & 0xFFFu;
        line += Format(" (%d,%d)=%s/%u", dx, y - f.y, m < c.mats.size() ? c.mats[m].name.c_str() : "?",
                       (w >> 12) & 15u);
      }
    std::printf("demon-circle: radial line +x:%s\n", line.c_str());
    std::printf("demon-circle: ring as seen at arrival: %d salt, %d other%s%s, %d unseen\n", salt,
                other, first.empty() ? "" : ", first ", first.c_str(), unseen);
  }
  out.spawned = true;
  out.id = arrived.mobId;
  out.arrived = arrived.state;
  out.verdict = arrived.circle.verdict;
  out.inside = arrived.circle.cells;
  out.ringSalt = arrived.circle.ringCells;
  out.circleR = arrived.circle.radius;
  out.why = arrived.why;
  const CircleShape circle0 = arrived.circle;
  auto centre = [&](float& x, float& z) {
    const Mob* m = c.mobs.FindMobById(out.id);
    if (!m || !m->Def()) return false;
    x = m->Origin().x + m->Def()->worldSize.x * 0.5f;
    z = m->Origin().z + m->Def()->worldSize.z * 0.5f;
    return true;
  };
  auto record = [&]() {
    float x = 0, z = 0;
    centre(x, z);
    const LiveDemon* ld = dw.Find(out.id);
    out.trace.push_back({(int32_t)std::lround(x * 16), (int32_t)std::lround(z * 16),
                         ld ? (int)ld->state : -1});
  };
  if (out.arrived != DemonState::Contained) {
    for (int i = 0; i < 30; i++) {
      Tick(rig);
      record();
    }
    return out;
  }
  // ---- 1. the hold ---------------------------------------------------------------
  for (int i = 0; i < holdTicks; i++) {
    Tick(rig);
    record();
    const LiveDemon* ld = dw.Find(out.id);
    if (!ld || ld->state != DemonState::Contained) break;
    out.heldTicks++;
    float x = 0, z = 0;
    if (!centre(x, z)) break;
    if (!circle0.Inside(x, z)) out.leftWhileHeld++;
    out.maxR = std::max(out.maxR, std::hypot(x - circle0.cx, z - circle0.cz));
    if (const ai::Brain* b = c.mobs.MobBrain(out.id))
      out.targetedTicks += b->hasTarget && b->targetId == ai::kPlayerActorId;
  }
  if (const LiveDemon* ld = dw.Find(out.id)) {
    out.movesRefused = ld->movesRefused;
    out.blowsRefused = ld->blowsRefused;
    out.blowsLoophole = ld->bind.blowsLoophole;
  }
  if (breakTicks <= 0) return out;
  // ---- 2. the break: you step back out of his reach, and one ring cell goes to
  //         air, through the queue. Loose, he has to come OUT of the circle to
  //         reach you -- which is the claim.
  c.mobs.SetPlayerActor(Vec3{you.x + 14.0f, you.y, you.z}, 3.0f, 17.0f, true);
  std::vector<CellOp> brk;
  Put(brk, f.x + r, f.y, f.z, 0u);
  Put(brk, f.x + r + 1, f.y, f.z, 0u);
  Tick(rig, brk);
  record();
  for (int i = 1; i < breakTicks + leaveTicks; i++) {
    const LiveDemon* ld = dw.Find(out.id);
    if (out.unboundAfter < 0 && ld && ld->state == DemonState::Unbound) out.unboundAfter = i;
    float x = 0, z = 0;
    if (out.unboundAfter >= 0 && out.outsideAfter < 0 && centre(x, z) && !circle0.Inside(x, z))
      out.outsideAfter = i - out.unboundAfter;
    if (out.outsideAfter >= 0) break;
    Tick(rig);
    record();
  }
  return out;
}

// ---- E. THE IMP IS FLESH: it burns, it bleeds, it dies -----------------------
// Skerrick's body straight from its def on a bare pad (no summoning: the claim
// is about the BODY at under a metre, not the circle): a torso set alight
// must burn; a grenade-slot blast beside it must carve and bleed blood; a
// killing blow must kill it.
struct FleshOut {
  bool spawned = false;
  uint32_t ignited = 0;
  float burnPeak = 0;
  int bloodOps = 0;
  float hp0 = 0, hpBlast = 0;
  uint32_t vox0 = 0, voxBlast = 0;
  bool hpLost = false, died = false;
};

FleshOut RunFlesh(Ctx& c, const std::string& mobName, uint32_t t0, uint64_t idBase) {
  FleshOut out;
  Regenerate(c);
  c.mobs.SetNextIdCounter(idBase);
  Fix f = Build(c, 8, false, false);
  support::TickRig rig(c, t0, f.chunk);
  for (size_t i = 0; i < f.pad.size(); i += kMaxCellOpsPerTick / 2)
    Tick(rig, std::vector<CellOp>(f.pad.begin() + (ptrdiff_t)i,
                                  f.pad.begin() + (ptrdiff_t)std::min(
                                      f.pad.size(), i + kMaxCellOpsPerTick / 2)));
  for (int i = 0; i < 8; i++) Tick(rig);
  const int def = c.mobs.FindDef(mobName);
  if (def < 0) return out;
  const Vec3 ws = c.mobs.Defs()[(size_t)def].worldSize;
  const uint64_t id =
      c.mobs.Spawn(def, {f.x - (int)(ws.x * 0.5f), f.y, f.z - (int)(ws.z * 0.5f)});
  out.spawned = id != 0;
  if (!id) return out;
  for (int i = 0; i < 10; i++) Tick(rig);
  int torso = -1;
  for (size_t i = 0; i < c.mobs.Defs()[(size_t)def].limbs.size(); i++)
    if (c.mobs.Defs()[(size_t)def].limbs[i].name == "torso") torso = (int)i;
  // Burn.
  out.ignited = torso >= 0 ? c.mobs.IgniteLimb(id, torso, 8) : 0;
  for (int i = 0; i < 30; i++) {
    Tick(rig);
    if (const Mob* m = c.mobs.FindMobById(id)) {
      float hpFrac = 0, burning = 0;
      int lost = 0;
      m->BodyFacts(hpFrac, burning, lost);
      out.burnPeak = std::max(out.burnPeak, burning);
    }
  }
  // Bleed: a small blast level with the hips, beside the body.
  const uint32_t mBlood = MatId(c, "blood");
  auto countBlood = [&]() {
    const OpBatch& b = rig.LastBatch();
    // Wounds drip as BRUSH ops (Mob::BleedTick); gore flies as spawns.
    for (const BrushOp& o : b.ops) out.bloodOps += (o.material & 0xFFFu) == mBlood;
    for (const ParticleSpawn& p : b.spawns) out.bloodOps += (p.payload & 0xFFFu) == mBlood;
    for (const CellOp& o : b.cells) out.bloodOps += (o.word & 0xFFFu) == mBlood;
    for (const FluidSpawnOp& o : b.fluid) out.bloodOps += o.mat == mBlood;
  };
  out.hp0 = c.mobs.TotalHp(id);
  out.vox0 = torso >= 0 ? c.mobs.LimbArtVoxelCount(id, torso) : 0;
  if (const Mob* m = c.mobs.FindMobById(id)) {
    const Vec3 o = m->Origin();
    const ExplosionOp e{(int32_t)std::floor(o.x + ws.x * 0.5f + 2.0f),
                        (int32_t)std::floor(o.y + ws.y * 0.4f),
                        (int32_t)std::floor(o.z + ws.z * 0.5f), 3, 120, 0, 0, 0};
    support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& ops) { ops.exps = {e}; });
    countBlood();
  }
  for (int i = 0; i < 40; i++) {
    Tick(rig);
    countBlood();
  }
  out.hpBlast = c.mobs.TotalHp(id);
  out.voxBlast = torso >= 0 && c.mobs.LimbBody(id, torso) ? c.mobs.LimbArtVoxelCount(id, torso) : 0;
  out.hpLost = c.mobs.IsAlive(id) ? out.hpBlast < out.hp0 : true;
  // Die: a killing blow on the torso.
  if (torso >= 0 && c.mobs.IsAlive(id)) {
    const uint64_t body = c.mobs.LimbBody(id, torso);
    if (const Mob* m = c.mobs.FindMobById(id))
      c.mobs.Damage(body, 100000.0f, m->Origin());
    for (int i = 0; i < 4; i++) Tick(rig);
  }
  out.died = !c.mobs.IsAlive(id);
  return out;
}

Status GateDemonCircle(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  std::string fails;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) {
      fails += (fails.empty() ? "" : "; ") + what;
      std::printf("demon-circle: FAILED %s\n", what.c_str());
    }
  };
  if (MatId(c, "salt") == 0) {
    detail = "no `salt` material";
    return Status::Fail;
  }

  // ---- A. the name -------------------------------------------------------------------
  GlyphLibrary lib;
  std::string gerr;
  if (!LoadGlyphs(AssetDir() + "/spells/glyphs.json", c.mats, lib, gerr)) {
    detail = "glyph load failed: " + gerr;
    return Status::Fail;
  }
  const int gSummon = lib.Find("summon_skerrick");
  check(gSummon >= 0 && lib.glyphs[gSummon].verb == SpellVerb::Summon &&
            lib.glyphs[gSummon].summon.demon == "skerrick",
        "A: summon_skerrick is a summon glyph naming skerrick");
  if (gSummon < 0) {
    detail = fails;
    return Status::Fail;
  }
  {
    EffectInst e;
    e.verb = SpellVerb::Summon;
    e.glyph = gSummon;
    SpellEmission em;
    ApplySpellEffect(lib, {e}, SpellFxVec{0, 0, 0}, SpellFxVec{0, -kSpellFxOne, 0}, 1000, em);
    check(em.summons.size() == 1 && em.ops.empty() && em.spawns.empty(),
          "A: the VM reports a summoning and writes nothing");
    check(EffectTariff(lib, e) == lib.glyphs[gSummon].summon.mana,
          Format("A: the tariff is the glyph's mana (%d vs %d)", EffectTariff(lib, e),
                 lib.glyphs[gSummon].summon.mana));
    GlyphInventory inv;
    inv.GrantAllAndBind(lib);
    check(!inv.Owns(gSummon), "A: the debug default does not grant a demon's name");
    inv.GrantAllAndBind(lib, true);
    check(inv.Owns(gSummon), "A: the all-names switch grants it");
  }
  DemonLibrary dl;
  std::string dlog;
  LoadDemons(AssetDir() + "/demons", dl, dlog);
  const DemonDef* sk = dl.Find("skerrick");
  check(sk != nullptr && c.mobs.FindDef(sk->mob) >= 0,
        "A: Skerrick resolves to a loaded mob def (" + (sk ? sk->mob : std::string("?")) + ")");
  check(sk != nullptr && c.mobs.Behaviors().Find(sk->behavior) >= 0,
        "A: Skerrick's behaviour profile exists");
  check(MatId(c, dl.circle.material.c_str()) != 0, "A: the circle material resolves");
  if (!fails.empty()) {
    detail = fails;
    return Status::Fail;
  }

  // ---- B..D. the fixtures -----------------------------------------------------------
  const int r = (int)BaselineNumber("demonCircle.ringCells", 14);
  const int hold = (int)BaselineNumber("demonCircle.holdTicks", 150);
  const int breakMax = (int)BaselineNumber("demonCircle.breakTicksMax", 40);
  const int leaveMax = (int)BaselineNumber("demonCircle.leaveTicksMax", 150);
  const std::string weatherWas = weather::Override();
  weather::SetOverride("clear");
  const uint64_t ids0 = c.mobs.NextIdCounter();
  const RunOut a = RunFix(c, r, true, false, hold, breakMax, leaveMax, lib, gSummon, 91000u, ids0);
  const RunOut a2 = RunFix(c, r, true, false, hold, breakMax, leaveMax, lib, gSummon, 91000u, ids0);
  const RunOut none = RunFix(c, r, false, false, 0, 0, 0, lib, gSummon, 92000u, ids0);
  const RunOut gap = RunFix(c, r, true, true, 0, 0, 0, lib, gSummon, 93000u, ids0);
  const FleshOut flesh = RunFlesh(c, sk ? sk->mob : std::string(), 94000u, ids0);
  weather::SetOverride(weatherWas);
  Regenerate(c);

  check(a.spawned, "B: Skerrick arrived (" + a.why + ")");
  check(a.arrived == DemonState::Contained,
        Format("B1: arrived %s in a closed ring (verdict %s: %s)", DemonStateName(a.arrived),
               CircleVerdictName(a.verdict), a.why.c_str()));
  check(a.heldTicks >= hold, Format("B1: contained %d of %d ticks", a.heldTicks, hold));
  check(a.leftWhileHeld == 0,
        Format("B1: his centre left the circle on %d contained ticks (furthest %.1f from the "
               "centre, circle radius %.1f)",
               a.leftWhileHeld, a.maxR, a.circleR));
  check(a.targetedTicks >= hold / 2,
        Format("B1: he targeted you on %d of %d ticks", a.targetedTicks, a.heldTicks));
  check(a.movesRefused > 0, "B1: the fence never refused a move (he never tried to leave)");
  // D3 made a blow across the ring the `touch` CHANNEL: this ring has no iron
  // pile, so for Skerrick it is a loophole and the fence lets the blow
  // through (demon-seals asserts the iron half). The claim left here is that
  // the fence was ASKED -- he swung at you across the salt.
  check(a.blowsRefused + a.blowsLoophole > 0,
        "B1: the fence was never asked about a blow across the ring");
  check(a.unboundAfter >= 0 && a.unboundAfter <= breakMax,
        Format("B2: unbound %d ticks after the break (max %d)", a.unboundAfter, breakMax));
  check(a.outsideAfter >= 0 && a.outsideAfter <= leaveMax,
        Format("B2: left the circle %d ticks after he was unbound (max %d)", a.outsideAfter,
               leaveMax));
  check(a.trace.size() > 0 && a.trace == a2.trace,
        Format("B: the twice-run trace differs (%zu vs %zu rows)", a.trace.size(),
               a2.trace.size()));
  check(none.spawned && none.arrived == DemonState::Unbound && none.verdict != CircleVerdict::Closed,
        Format("C: no ring -> %s (%s)", DemonStateName(none.arrived), CircleVerdictName(none.verdict)));
  check(gap.spawned && gap.arrived == DemonState::Unbound && gap.verdict != CircleVerdict::Closed,
        Format("D: a one-cell gap -> %s (%s)", DemonStateName(gap.arrived),
               CircleVerdictName(gap.verdict)));

  check(flesh.spawned && flesh.ignited > 0 && flesh.burnPeak > 0.0f,
        Format("E: the imp burns (%u voxels lit, burning peak %.2f)", flesh.ignited,
               flesh.burnPeak));
  check(flesh.bloodOps > 0 && flesh.hpLost,
        Format("E: a blast beside the imp hurts it and it bleeds blood (%d blood ops; hp "
               "%.1f -> %.1f, torso voxels %u -> %u)",
               flesh.bloodOps, flesh.hp0, flesh.hpBlast, flesh.vox0, flesh.voxBlast));
  check(flesh.died, "E: a killing blow kills the imp");
  RecordObserved("demonCircle.unboundAfter", a.unboundAfter);
  RecordObserved("demonCircle.outsideAfter", a.outsideAfter);
  RecordObserved("demonCircle.movesRefused", a.movesRefused);
  RecordObserved("demonCircle.blowsRefused", a.blowsRefused);
  detail = Format(
      "B ring r=%d: %s (%s, %d inside, %d salt, radius %.1f); held %d/%d ticks, never out "
      "(furthest %.1f), targeting you %d, fence refused %u moves + %u blows (%u through); break -> unbound "
      "+%d, out of the circle +%d; trace %zu rows %s. C no ring: %s (%s). D gap: %s (%s)",
      r, DemonStateName(a.arrived), CircleVerdictName(a.verdict), a.inside, a.ringSalt, a.circleR,
      a.heldTicks, hold, a.maxR, a.targetedTicks, a.movesRefused, a.blowsRefused, a.blowsLoophole,
      a.unboundAfter,
      a.outsideAfter, a.trace.size(), a.trace == a2.trace ? "identical twice" : "DIFFERS",
      DemonStateName(none.arrived), CircleVerdictName(none.verdict), DemonStateName(gap.arrived),
      CircleVerdictName(gap.verdict));
  detail += Format(". E flesh: %u lit (burning peak %.2f), %d blood ops after a blast, %s",
                   flesh.ignited, flesh.burnPeak, flesh.bloodOps,
                   flesh.died ? "dies to a killing blow" : "DID NOT DIE");
  if (!fails.empty()) detail += "; FAILED: " + fails;
  std::printf("demon-circle: %s (%s)\n", fails.empty() ? "PASS" : "FAIL", detail.c_str());
  return fails.empty() ? Status::Pass : Status::Fail;
}

}  // namespace

Status GateDemonSeals(Ctx& c, std::string& detail);   // selftest_demon_seals.cpp

const std::vector<Gate>& DemonGates() {
  static const std::vector<Gate> g = {
      {"demon-circle", "mob", {}, false, GateDemonCircle},
      // D3 (selftest_demon_seals.cpp): seals, channels, strength, release, gaze.
      {"demon-seals", "mob", {}, false, GateDemonSeals},
  };
  return g;
}

}  // namespace selftest
