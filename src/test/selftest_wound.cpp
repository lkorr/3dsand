// selftest_wound.cpp — THE WOUND MODEL: a blade cuts, it does not amputate.
//
// WHAT CHANGED, AND WHY THESE FOUR GATES EXIST
//
// Severing used to be an EVENT. Mob::Damage carried three thresholds — a hit
// within 1.75 voxels of a joint anchor, a hit past the limb's authored
// severImpactSpeed, and hp reaching zero — and a sword tripped all three on
// first contact. Touching a creature with a blade removed whatever it touched,
// instantly and identically every time, and the per-voxel carving that ran
// alongside it was decoration on a decision already made.
//
// It is now a CONSEQUENCE. A hit carves a KERF along the swept edge, soaks the
// exposed flesh in the creature's own wound material, and dismembers only when
// the limb's voxel lattice has genuinely been cut through — either because a
// substantial piece is no longer connected to the joint, or because the flesh
// AT the joint is gone (game/mob.h BladeCut, Mob::CutLimb, and the two
// structural rules in Mob::CarveLimb).
//
// So the claims worth gating are geometric and cumulative, and each one is a
// different way the change could be wrong:
//
//   wound-chip        ONE cut is a wound, not an amputation. This is the
//                     owner's complaint stated as an assertion.
//   wound-accumulate  ...and repeated cuts to one cross-section DO take the
//                     limb, through the ordinary sever machinery. A model
//                     that only ever chips is the opposite failure and would
//                     pass wound-chip perfectly.
//   wound-heft        a heavier weapon needs strictly fewer of them. This is
//                     what makes the weapon matter rather than the count.
//   wound-bleed       the blood that follows is BOUNDED and STOPS (rule 2).
//
// THE BAND, NOT THE NUMBER. wound-accumulate asserts kMin <= hits <= kMax with
// both ends in tests/baseline.json. An exact count would pin the gate to one
// rig's arm thickness and one set of tuning values, and every future tweak to
// gore.cut* would "fail" it; the property being protected is that the model is
// neither instant nor asymptotic.
//
// CONTENT-INDEPENDENT BY CONSTRUCTION. No def is named here and no limb is
// named here. The fixture is CHOSEN: the largest severable, non-vital limb on
// any loaded mob def, which is a property every rig either has or does not.
// Naming the cast is how `ragdoll-joints` came to fail on "waistChecked == 9"
// the day somebody added a correct mob (gotcha-gate-hardcodes-asset-cast), and
// picking the LARGEST limb is also the answer to the other trap — a fixture
// that erodes below the collapse floor mid-test reports a confusing failure
// about geometry when the real problem was that it was too small to cut.

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "game/item.h"
#include "game/equipment.h"
#include "game/player.h"
#include "game/mob.h"
#include "sim/microbody.h"
#include "sim/tuning.h"
#include "test/selftest.h"
#include "sim/scale.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// The fixture: which creature, which limb, and where its flesh is.
struct Target {
  int defIndex = -1;
  int limb = -1;
  uint32_t atSpawn = 0;   // authoritative-lattice voxels when intact
  std::string defName, limbName;
  bool valid() const { return defIndex >= 0 && limb >= 0; }
};

// Where a limb runs, measured from its own surviving voxels.
//
// The joint anchor is one END of a limb and the mass extends away from it, so
// the direction from the anchor to the centroid IS the long axis — no PCA, no
// authored axis, and correct for a leg, an arm, a tail or a wing without any
// of them having to say so. `reach` is how far the flesh extends along it,
// which is what tells a mid-limb cut where the middle is.
struct LimbAxis {
  Vec3 anchor{};
  Vec3 along{0, 1, 0};   // unit, anchor -> body of the limb
  Vec3 edge{1, 0, 0};    // unit, in the cross-section plane
  Vec3 travel{0, 0, 1};  // unit, ditto, perpendicular to `edge`
  float reach = 1.0f;
  bool valid = false;
};

LimbAxis MeasureLimb(MobSystem& mobs, uint64_t id, int limb) {
  LimbAxis a;
  if (!mobs.LimbBody(id, limb)) return a;
  a.anchor = mobs.LimbAnchorPos(id, limb);
  // Sampled rather than exhaustive: LimbVoxelPos wraps, so 24 probes describe
  // the limb's extent whether it has 60 collider voxels or 6000, and the cost
  // does not track the rig's resolution.
  const uint32_t kProbes = 24;
  Vec3 sum{};
  uint32_t n = 0;
  std::vector<Vec3> pts;
  pts.reserve(kProbes);
  for (uint32_t k = 0; k < kProbes; k++) {
    const Vec3 p = mobs.LimbVoxelPos(id, limb, k * 7919u);
    pts.push_back(p);
    sum += p;
    n++;
  }
  if (!n) return a;
  const Vec3 centroid = sum * (1.0f / (float)n);
  Vec3 along = centroid - a.anchor;
  if (along.len() < 1e-3f) along = Vec3{0, -1, 0};  // anchor sits in the mass
  a.along = along.normalized();
  float far = 0;
  for (const Vec3& p : pts) far = std::max(far, (p - a.anchor).dot(a.along));
  a.reach = std::max(far, 0.5f);
  // Any two unit vectors spanning the cross-section. Chosen against whichever
  // world axis the limb is LEAST aligned with, so the cross product is never
  // near-degenerate — a leg hanging along -Y and an outstretched arm along +X
  // must both get a well-conditioned frame.
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

// One blade hit, expressed exactly as main.cpp's melee sweep expresses one.
//
// The BladeCutScope matters and is not decoration: it is what marks the sever
// `byBlade`, which is the cause the dismember audio switches on, and a gate
// that omitted it would be testing a cut nobody in the game ever makes.
bool CutOnce(MobSystem& mobs, World& world, uint64_t id, int limb,
             const LimbAxis& ax, float alongLimb, float power, float heft,
             uint32_t seed, std::vector<ParticleSpawn>& spawns) {
  const uint64_t body = mobs.LimbBody(id, limb);
  if (!body || !ax.valid) return false;
  const auto& g = CurrentTuning().gore;
  BladeCut cut;
  cut.at = ax.anchor + ax.along * alongLimb;
  cut.edgeAxis = ax.edge;
  cut.cutDir = ax.travel;
  // The same three lines main.cpp computes, so a tuning change moves the gate
  // and the game together instead of leaving one of them behind.
  cut.halfWidth = std::max(0.9f * g.cutWidth, 0.08f);
  cut.depth = (g.cutDepth + g.cutDepthPower * power) * heft;
  cut.length = g.cutLength * (0.4f + 0.6f * power) * heft;
  cut.power = power;
  cut.seed = seed;
  MobSystem::BladeCutScope blade(mobs, power);
  return mobs.CutLimb(body, cut, world, spawns);
}

// Pick the fixture: the biggest severable, non-vital limb in the library.
//
// Measured by SPAWNING rather than by reading the def's prefab, because what
// the test cuts is the runtime lattice — a def's art voxel count says nothing
// about physScale, skinScale, or whether the rig gave that limb a body at all.
// Each candidate is despawned again, so this leaves the system as it found it.
Target ChooseTarget(MobSystem& mobs, IVec3 at) {
  Target best;
  for (size_t d = 0; d < mobs.Defs().size(); d++) {
    const MobDef& def = mobs.Defs()[d];
    if (def.limbs.empty() || def.bleedMat == 0) continue;  // must be able to bleed
    mobs.Reset();
    const uint64_t id = mobs.Spawn((int)d, at);
    if (!id) continue;
    for (size_t li = 0; li < def.limbs.size(); li++) {
      if ((int)li == def.rootLimb) continue;
      if (!def.limbs[li].severable || def.limbs[li].vital) continue;
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

// Ground under a fixture column, anchored to the residency window.
//
// NEVER AN ABSOLUTE X OR Z (selftest.h's ordering note, and the in-suite
// failure `mob-burn` paid for it with): by the time these gates run,
// `streaming` has walked the window origin ~20 chunks along x, and a literal
// column lands outside it where world writes are dropped and a mob despawns
// the moment it is spawned.
IVec3 FixtureSite(const World& world, int inset) {
  const IVec3 org = world.WindowOrigin();
  const int x = org.x * (int)kChunk + inset;
  const int z = org.z * (int)kChunk + inset;
  return IVec3{x, World::TerrainHeight(x, z, kDefaultSeed) + 1, z};
}

// Spawn the chosen fixture and let it settle onto the ground.
uint64_t SpawnTarget(Ctx& c, const Target& t, int inset, IVec3& outChunk) {
  MobSystem& mobs = c.mobs;
  mobs.Reset();
  c.debris.Reset();
  const IVec3 site = FixtureSite(c.world, inset);
  outChunk = IVec3{site.x >> 4, site.y >> 4, site.z >> 4};
  const uint64_t id = mobs.Spawn(t.defIndex, site);
  if (!id) return 0;
  // A few physics steps only — no world submit. The creature has to be posed
  // and its limb transforms real before anything is measured off them, and
  // nothing here needs the CA to have run.
  for (int i = 0; i < 8; i++) {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    mobs.PreTick(1000u + (uint32_t)i, c.world, ops, cellOps, spawns);
    c.phys.Step(kTickDt);
    mobs.PostStep();
  }
  mobs.ClearSeverEvents();
  mobs.ClearSeverStats();
  return id;
}

// The id-counter guard these four gates open with is shared: test/support.h
// IdCounterScope, with the whole argument for why it exists written there.

// Pristine terrain under the fixtures.
//
// Called by EVERY one of the four rather than declared as a dependency,
// because a gate has to be verifiable with `--gate <name>` alone (CLAUDE.md,
// "authoring cheap-to-verify work") and none of these wants another gate's
// leftovers — they only want ground to stand a creature on. One worldgen
// dispatch is cheap next to what they then do to the creature.
void PrepareWorld(Ctx& c) {
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
}

// ---------------------------------------------------------------------------
// wound-chip: one cut is a wound
// ---------------------------------------------------------------------------
Status GateWoundChip(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 170));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  IVec3 pchunk{};
  const uint64_t id = SpawnTarget(c, t, 170, pchunk);
  if (!id) {
    detail = "spawn refused";
    return Status::Fail;
  }
  const uint32_t woundMat = mobs.Defs()[t.defIndex].woundMat;
  const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
  const uint32_t before = mobs.LimbArtVoxelCount(id, t.limb);
  const uint32_t stainBefore = mobs.LimbMaterialCount(id, t.limb, woundMat);

  // MID-LIMB, at moderate commitment. Deliberately not at the joint: the
  // claim here is "an ordinary hit does not dismember", and cutting the
  // thinnest part of the limb would be testing the other gate's subject.
  std::vector<ParticleSpawn> spawns;
  const bool hit = CutOnce(mobs, c.world, id, t.limb, ax, ax.reach * 0.5f,
                           0.5f, 1.0f, 0x9001u, spawns);

  const uint32_t after =
      mobs.LimbBody(id, t.limb) ? mobs.LimbArtVoxelCount(id, t.limb) : 0;
  const uint32_t stainAfter =
      mobs.LimbBody(id, t.limb) ? mobs.LimbMaterialCount(id, t.limb, woundMat)
                                : 0;
  const bool attached = mobs.LimbBody(id, t.limb) != 0;
  const bool noSever = mobs.SeverEvents().empty();
  const bool alive = mobs.IsAlive(id);
  const uint32_t lost = before > after ? before - after : 0u;
  // The wound must be a WOUND: real voxels gone, and not so many that the
  // limb has effectively been amputated by arithmetic. The upper bound is the
  // one that would have caught the old behaviour, where the "carve" removed a
  // sphere the size of the arm before Damage() severed it anyway.
  const double maxFrac = BaselineNumber("woundChipMaxFraction", 0.35);
  const float frac = before ? (float)lost / (float)before : 0.0f;
  const bool sized = lost > 0 && frac <= (float)maxFrac;
  const bool stained = stainAfter > stainBefore;

  RecordObserved("woundChipLostFraction", (double)frac);
  RecordObserved("woundChipStained", (double)(stainAfter - stainBefore));

  const bool ok = hit && sized && stained && attached && noSever && alive;
  detail = Format(
      "%s/%s: %u -> %u voxels (%.1f%% of the limb, cap %.0f%%), %u stained, "
      "limb attached=%d severs=%zu alive=%d",
      t.defName.c_str(), t.limbName.c_str(), before, after, frac * 100.0f,
      maxFrac * 100.0, stainAfter - stainBefore, attached ? 1 : 0,
      mobs.SeverEvents().size(), alive ? 1 : 0);
  mobs.Reset();
  c.debris.Reset();
  return ok ? Status::Pass : Status::Fail;
}

// How many cuts to one cross-section it takes to part the limb, or `cap` if
// it never does. Shared by wound-accumulate and wound-heft so the two are
// measuring the same thing at two hefts and nothing else.
struct CutRun {
  int hits = 0;
  bool severed = false;
  bool byBlade = false;
  bool aliveAfter = false;
  bool limbGone = false;
  uint32_t debrisGained = 0;
  uint32_t lastCount = 0;
};

CutRun HackThrough(Ctx& c, const Target& t, int inset, float heft, int cap) {
  CutRun r;
  MobSystem& mobs = c.mobs;
  IVec3 pchunk{};
  const uint64_t id = SpawnTarget(c, t, inset, pchunk);
  if (!id) return r;
  const uint32_t debris0 = c.debris.BodyCount();
  const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
  std::vector<ParticleSpawn> spawns;
  for (int k = 0; k < cap; k++) {
    if (!mobs.LimbBody(id, t.limb)) break;
    // ALWAYS THE SAME CROSS-SECTION. `ax.anchor` is a world point that carves
    // do not move (MobSystem::LimbAnchorPos), so this really is the player
    // hacking at one place rather than a test walking down the limb — which
    // is the distinction between "cut through" and "grind away", and only the
    // first of them is what the owner asked for.
    //
    // A QUARTER OF THE LIMB'S REACH IN, not on the anchor itself: the joint is
    // the boundary between two limbs and a slot centred exactly on it would
    // spend half its depth in the parent.
    CutOnce(mobs, c.world, id, t.limb, ax, ax.reach * 0.25f, 0.75f, heft,
            0x5C07u + (uint32_t)k * 2654435761u, spawns);
    r.hits = k + 1;
    if (!mobs.SeverEvents().empty()) {
      r.severed = true;
      for (const auto& se : mobs.SeverEvents())
        if (se.limbIndex == t.limb) r.byBlade = se.byBlade;
      break;
    }
    r.lastCount = mobs.LimbArtVoxelCount(id, t.limb);
  }
  r.aliveAfter = mobs.IsAlive(id);
  r.limbGone = mobs.LimbBody(id, t.limb) == 0;
  const uint32_t d1 = c.debris.BodyCount();
  r.debrisGained = d1 > debris0 ? d1 - debris0 : 0u;
  mobs.Reset();
  c.debris.Reset();
  return r;
}

// ---------------------------------------------------------------------------
// wound-accumulate: sustained cuts to one place DO take the limb
// ---------------------------------------------------------------------------
Status GateWoundAccumulate(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 200));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  const int kMin = (int)BaselineNumber("woundAccumMinHits", 2);
  const int kMax = (int)BaselineNumber("woundAccumMaxHits", 14);
  // The cap is deliberately well past kMax: "never severed" and "severed on
  // the 30th" are different failures and a cap at kMax would report them
  // identically.
  const CutRun r = HackThrough(c, t, 200, 1.0f, kMax * 3 + 4);

  RecordObserved("woundAccumHits", (double)r.hits);
  const bool band = r.severed && r.hits >= kMin && r.hits <= kMax;
  // ...AND IT WENT THROUGH THE ORDINARY MACHINERY. A structural rule that
  // detached the limb by clearing its body handle would satisfy "the limb is
  // gone" and quietly skip the gout, the audio cause, the loco state rules and
  // the debris hand-off. Three independent signals that Sever() really ran.
  const bool machinery = r.limbGone && r.byBlade && r.debrisGained > 0;
  const bool ok = band && machinery;
  detail = Format(
      "%s/%s (%u voxels): severed after %d hits (band %d..%d), byBlade=%d, "
      "limb detached=%d, +%u debris bodies, creature alive=%d",
      t.defName.c_str(), t.limbName.c_str(), t.atSpawn, r.hits, kMin, kMax,
      r.byBlade ? 1 : 0, r.limbGone ? 1 : 0, r.debrisGained,
      r.aliveAfter ? 1 : 0);
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// wound-heft: a heavier weapon needs strictly fewer hits
// ---------------------------------------------------------------------------
Status GateWoundHeft(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 230));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  const int cap = (int)BaselineNumber("woundAccumMaxHits", 14) * 3 + 4;
  const float heavy = (float)BaselineNumber("woundHeftHeavy", 2.5);
  const CutRun light = HackThrough(c, t, 230, 1.0f, cap);
  const CutRun heavyRun = HackThrough(c, t, 230, heavy, cap);

  // STRICTLY fewer, not "no more". "No more" is satisfied by heft doing
  // nothing at all, which is exactly the regression this exists to catch: the
  // factor is computed in main.cpp and consumed three call levels down, and a
  // dropped multiplication there is invisible in play.
  const bool fewer = heavyRun.severed && light.severed &&
                     heavyRun.hits < light.hits;

  // ...and the DATA half: a bigger weapon must derive a bigger heft off its
  // own art, or the mechanism above has nothing to scale. Both items are
  // optional — an installation with only the sword reports and passes that
  // half, the same way a missing asset is a SKIP everywhere else here.
  const auto& g = CurrentTuning().gore;
  const ItemDef* sword = c.items.At(c.items.Find("sword"));
  const ItemDef* cleaver = c.items.At(c.items.Find("cleaver"));
  const float hs =
      sword ? sword->HeftFactor(g.woundHeftRef, g.woundHeftMax) : 0.0f;
  const float hc =
      cleaver ? cleaver->HeftFactor(g.woundHeftRef, g.woundHeftMax) : 0.0f;
  const bool derived = !sword || !cleaver || hc > hs;
  RecordObserved("woundHeftSword", (double)hs);
  RecordObserved("woundHeftCleaver", (double)hc);

  const bool ok = fewer && derived;
  detail = Format(
      "%s/%s: heft 1.0 severs in %d hits, heft %.1f in %d; item heft sword "
      "%.2f cleaver %.2f (ref %.2f world voxels)",
      t.defName.c_str(), t.limbName.c_str(), light.hits, heavy, heavyRun.hits,
      hs, hc, g.woundHeftRef);
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// wound-bleed: the blood is bounded, and it stops
// ---------------------------------------------------------------------------
Status GateWoundBleed(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);

  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 260));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  IVec3 pchunk{};
  const uint64_t id = SpawnTarget(c, t, 260, pchunk);
  if (!id) {
    detail = "spawn refused";
    return Status::Fail;
  }
  const uint32_t bleedMat = mobs.Defs()[t.defIndex].bleedMat;
  const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
  {
    std::vector<ParticleSpawn> spawns;
    CutOnce(mobs, world, id, t.limb, ax, ax.reach * 0.5f, 0.9f, 1.0f, 0xB100Du,
            spawns);
  }

  // Now run real ticks and watch the wound. Three things are being measured
  // and they are three different rule-2 claims:
  //   * it BLEEDS at all (a wound model that carves and never drips is a
  //     silent regression of the existing gore path),
  //   * no tick exceeds the authored global drip budget, and
  //   * it STOPS, on its own, without anything taking the wound away.
  const auto& gore = CurrentTuning().gore;
  const int opCap = std::max(1, gore.bleedOpsPerTick);
  uint32_t t0 = 20000;
  uint32_t bloodOps = 0, bloodDrops = 0, worstTickOps = 0;
  int lastBleedTick = -1;
  const int kWatch = 600;
  for (int i = 0; i < kWatch; i++) {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    mobs.PreTick(t0 + 1, world, ops, cellOps, spawns);
    uint32_t tickOps = 0;
    for (const BrushOp& op : ops)
      if (op.material == bleedMat) tickOps++;
    for (const ParticleSpawn& s : spawns)
      if ((s.payload & 0xFFFu) == bleedMat) bloodDrops++;
    if (tickOps || !mobs.BleedSources().empty()) lastBleedTick = i;
    bloodOps += tickOps;
    worstTickOps = std::max(worstTickOps, tickOps);
    c.debris.QueueSupportEvents(world.Snap());
    c.debris.PreTick(t0 + 1, world, cellOps, spawns);
    ++t0;
    SubmitTick(ctx, world, sim, t0, kDefaultSeed, ops, {}, cellOps, false,
               pchunk, true, true, spawns);
    ctx.WaitIdle();
    ctx.ProcessEvents();
    c.phys.Step(kTickDt);
    c.debris.PostStep();
    mobs.PostStep();
  }

  const int stopBy = (int)BaselineNumber("woundBleedStopTicks", 400);
  const bool bled = bloodOps > 0 || bloodDrops > 0;
  const bool bounded = worstTickOps <= (uint32_t)opCap;
  const bool stopped = lastBleedTick >= 0 && lastBleedTick < stopBy;

  // ...AND THE WORLD SLEEPS AFTERWARDS. Blood is real matter the CA carries,
  // so a wound that keeps a chunk awake is rule 2 broken however tidy the
  // budget looked. Settled by ticking on with nothing left to emit.
  for (int i = 0; i < 400; i++)
    SubmitTick(ctx, world, sim, ++t0, kDefaultSeed, {}, {}, {}, false, pchunk,
               false, false);
  ctx.WaitIdle();
  const uint32_t active = ReadActiveChunksSync(ctx, world, sim);
  const uint32_t restCap = (uint32_t)BaselineNumber("woundBleedRestChunks", 32);
  const bool quiet = active <= restCap;

  RecordObserved("woundBleedOps", (double)bloodOps);
  RecordObserved("woundBleedLastTick", (double)lastBleedTick);
  RecordObserved("woundBleedRestActive", (double)active);

  mobs.Reset();
  c.debris.Reset();
  // LEAVE THE WORLD AS THIS GATE FOUND IT. It poured real blood at absolute
  // coordinates inside the residency window; every gate after it in kOrder
  // assumes pristine terrain (the same restore mob-burn does, and for the same
  // reason).
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  const bool ok = bled && bounded && stopped && quiet;
  detail = Format(
      "%s/%s: %u blood ops + %u droplets, worst tick %u/%d, last bleed at "
      "tick %d (must be < %d), %u chunks awake at rest (cap %u)",
      t.defName.c_str(), t.limbName.c_str(), bloodOps, bloodDrops,
      worstTickOps, opCap, lastBleedTick, stopBy, active, restCap);
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// bleed-out: every drop of blood is hp, and an amputation is fatal (Gore §F)
// ---------------------------------------------------------------------------
//
// Two claims, and the second is the one the owner asked for in as many words:
//   * IDENTITY. Across the whole life of a wound, hp lost equals blood emitted
//     x gore.bleedHpPerVoxel, counting whole voxels at 1 and micro droplets at
//     1/microScale^3. Not "hp goes down while bleeding" — that would pass on a
//     drain that ignored the blood and ran on a timer.
//   * AN OPEN STUMP KILLS. Sever a limb, tick without touching the wound, and
//     the creature must die of it within the time the rate predicts; with
//     gore.stumpBleedsOpen off, the SAME fixture must survive the same window
//     and go dry. That differential is what proves the top-up is the
//     mechanism and not a coincidence of the stump budget.
//
// CPU ONLY: PreTick / physics / PostStep, no tick submitted. The drain is
// charged where the op is EMITTED, so the CA never has to see the blood for
// hp to move, and a gate that ran the sim for two thousand ticks to watch a
// number fall would be paying for a claim it is not making.
Status GateBleedOut(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 290));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  const Tuning saved = CurrentTuning();
  const auto& gore = saved.gore;
  const uint32_t bleedMat = mobs.Defs()[t.defIndex].bleedMat;
  const int ms = std::max(2, gore.microScale);
  const double dropletVox = 1.0 / (double)(ms * ms * ms);
  const int opCap = std::max(1, gore.bleedOpsPerTick);

  // One amputation, then the clock. `stumpOpen` decides whether it stops.
  struct Run {
    float hp0 = 0, hpBefore = 0;  // before the sever / after it (baseline)
    double counted = 0;           // whole-voxel equivalents emitted since hpBefore
    double countedAtLast = 0;     // ...as of the last tick the creature lived
    float hpAtLast = 0;
    int deathTick = -1;
    int lastBleedTick = -1;
    uint32_t worstTickOps = 0;
    bool monotone = true;
    int ticks = 0;
    float bloodLost = 0;
  };
  auto run = [&](bool stumpOpen, int inset, int window) {
    Run r;
    Tuning tt = saved;
    tt.gore.stumpBleedsOpen = stumpOpen;
    SetCurrentTuning(tt);
    IVec3 pchunk{};
    const uint64_t id = SpawnTarget(c, t, inset, pchunk);
    if (!id) { SetCurrentTuning(saved); return r; }
    r.hp0 = mobs.TotalHp(id);
    mobs.Sever(id, t.limb);
    // The thrown sever voxels are charged INSIDE Sever(); the baseline is
    // taken after it so the identity below is over what the ticks emit.
    r.hpBefore = mobs.TotalHp(id);
    r.hpAtLast = r.hpBefore;
    uint32_t tick = 30000;
    for (int i = 0; i < window; i++) {
      std::vector<BrushOp> ops;
      std::vector<ParticleSpawn> spawns;
      std::vector<CellOp> cellOps;
      mobs.PreTick(tick++, c.world, ops, cellOps, spawns);
      uint32_t tickOps = 0;
      double emitted = 0;
      for (const BrushOp& op : ops)
        if (op.material == bleedMat) {
          tickOps++;
          emitted += (double)BleedClumpVoxels(op.radius);
        }
      for (const ParticleSpawn& s : spawns)
        if ((s.payload & 0xFFFu) == bleedMat && (s.flags & kPFlagMicro))
          emitted += dropletVox;
      // Whole-voxel spawns (the sever's thrown gobbets, drained through
      // pendingSpawns_ on this first tick) were charged before the baseline
      // and are deliberately not counted here.
      r.counted += emitted;
      r.worstTickOps = std::max(r.worstTickOps, tickOps);
      if (tickOps || emitted > 0) r.lastBleedTick = i;
      r.ticks = i + 1;
      if (!mobs.IsAlive(id)) { r.deathTick = i; break; }
      const float hp = mobs.TotalHp(id);
      if (hp > r.hpAtLast + 1e-3f) r.monotone = false;
      r.hpAtLast = hp;
      r.countedAtLast = r.counted;
      r.bloodLost = mobs.BloodLost(id);
      c.phys.Step(kTickDt);
      mobs.PostStep();
    }
    mobs.Reset();
    c.debris.Reset();
    SetCurrentTuning(saved);
    return r;
  };

  // How long the rate says a full bleed-out takes, from the knobs alone: one
  // clump every bleedDripTicks at bleedHpPerVoxel per voxel. The window is
  // three times that plus a margin, so "never died" and "died late" are
  // different failures rather than the same timeout.
  const float clumpVox = (float)BleedClumpVoxels(std::max(0, gore.bleedClumpRadius));
  const float hpPerDrip = clumpVox * gore.bleedHpPerVoxel;
  const int dripTicks = std::max(1, gore.bleedDripTicks);
  // hp0 is only known after a spawn; size the window off the def's authored
  // total instead, which is what the creature starts with.
  float authored = 0;
  for (const MobLimbDef& ld : mobs.Defs()[t.defIndex].limbs) authored += ld.hp;
  const int expected =
      hpPerDrip > 0 ? (int)std::ceil(authored / hpPerDrip) * dripTicks : 0;
  const int window = expected * 3 + 300;

  const Run open = run(true, 290, window);
  const Run shut = run(false, 290, window);

  // A. the identity, over the last tick the creature lived. The tick it dies
  // on overshoots by construction (the drain that kills takes more than is
  // left), so the comparison stops one tick short.
  const double lostHp = (double)open.hpBefore - (double)open.hpAtLast;
  const double owedHp = open.countedAtLast * (double)gore.bleedHpPerVoxel;
  const double tol = 0.25 + 1e-4 * (double)open.hpBefore;
  const bool identity = gore.bleedHpPerVoxel > 0.0f && open.countedAtLast > 0 &&
                        std::fabs(lostHp - owedHp) <= tol;
  // B. the amputation kills, in the time the rate predicts, and it never
  // outran the drip budget doing it.
  const bool fatal = open.deathTick >= 0 && open.deathTick <= expected * 3 + 60;
  const bool bounded = open.worstTickOps <= (uint32_t)opCap;
  // C. with the stump allowed to close, the same cut is survivable and goes
  // dry — the old finite stump. The differential is the proof the knob is
  // the mechanism.
  const bool survivable = shut.deathTick < 0 && shut.lastBleedTick >= 0 &&
                          shut.lastBleedTick < shut.ticks - 1;
  const bool ok = identity && fatal && bounded && open.monotone && survivable &&
                  open.hpBefore > 0.0f;

  RecordObserved("bleedOutTicks", (double)open.deathTick);
  RecordObserved("bleedOutIdentityErr", std::fabs(lostHp - owedHp));
  RecordObserved("bleedOutShutLastTick", (double)shut.lastBleedTick);

  detail = Format(
      "%s/%s: hp %.1f -> %.1f at sever, %.2f blood vox = %.2f hp owed vs %.2f "
      "lost (tol %.2f), died at tick %d (expected ~%d), worst tick %u/%d, "
      "monotone=%d; stump shut: alive=%d, dry at tick %d of %d",
      t.defName.c_str(), t.limbName.c_str(), open.hp0, open.hpBefore,
      open.countedAtLast, owedHp, lostHp, tol, open.deathTick, expected,
      open.worstTickOps, opCap, open.monotone ? 1 : 0,
      shut.deathTick < 0 ? 1 : 0, shut.lastBleedTick, shut.ticks);
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// burn-cap: the burnt fraction of a body caps what it can hold (Gore §G)
// ---------------------------------------------------------------------------
//
//   A. the CURVE, with no fixture: intact = full, mid knot = authored, death
//      knot = zero, and never rising in between.
//   B. the REAL PATH: light a creature and let the burn pass run. The cap it
//      reports must be the curve of the fraction it reports, every live limb's
//      hp must sit under its authored max x that cap, and the cap must have
//      actually bitten (< 1) — a cap that stays at 1 while the body chars is
//      the count wired to nothing.
//   C. DEATH BY BURNS: stand the creature in a real fire and it must die,
//      with the fraction at (or past) the death knot — not because a vital
//      limb burnt through, which was already possible and is mob-burn's
//      subject, but because the cap reached zero.
//
// A and B are CPU only: ignition through MobSystem::IgniteLimb and the burn
// pass through PreTick, no tick submitted. C has to be a world fire (see the
// note at the phase) and regenerates the world on the way out.
Status GateBurnCap(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  if (!mobs.BurnTablesReady()) {
    detail = "burn tables not loaded";
    return Status::Fail;
  }
  const auto& gore = CurrentTuning().gore;

  // A. the curve.
  bool curve = std::fabs(Mob::BurnHealthCapFor(0.0f) - 1.0f) < 1e-6f &&
               std::fabs(Mob::BurnHealthCapFor(gore.burnCapMidFraction) -
                         gore.burnCapMidHealth) < 1e-5f &&
               Mob::BurnHealthCapFor(gore.burnDeathFraction) == 0.0f &&
               Mob::BurnHealthCapFor(1.0f) == 0.0f;
  {
    float prev = 2.0f;
    for (int i = 0; i <= 100; i++) {
      const float v = Mob::BurnHealthCapFor((float)i / 100.0f);
      if (v > prev + 1e-6f) curve = false;
      prev = v;
    }
  }

  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 320));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  const MobDef& def = mobs.Defs()[t.defIndex];
  const int nLimbs = (int)def.limbs.size();
  IVec3 pchunk{};
  const uint64_t id = SpawnTarget(c, t, 320, pchunk);
  if (!id) {
    detail = "spawn refused";
    return Status::Fail;
  }
  auto tickOnce = [&](uint32_t tick) {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    mobs.PreTick(tick, c.world, ops, cellOps, spawns);
    c.phys.Step(kTickDt);
    mobs.PostStep();
  };

  // B. light the fixture limb only, run the pass, read the cap.
  uint32_t tick = 40000;
  const uint32_t lit = mobs.IgniteLimb(id, t.limb, 400, 0);
  for (int i = 0; i < 240 && mobs.IsAlive(id); i++) tickOnce(tick++);
  const float fracB = mobs.BurnFraction(id);
  const float capB = mobs.BurnHealthCap(id);
  const bool aliveB = mobs.IsAlive(id);
  bool underCap = aliveB;
  float worstOver = 0.0f;
  if (aliveB)
    for (int li = 0; li < nLimbs; li++) {
      if (!mobs.LimbBody(id, li)) continue;
      const float hp = mobs.LimbHp(id, li);
      const float capHp = def.limbs[li].hp * capB;
      if (hp > capHp + 1e-3f) {
        underCap = false;
        worstOver = std::max(worstOver, hp - capHp);
      }
    }
  const bool bit = lit > 0 && aliveB && fracB > 0.0f && capB < 1.0f &&
                   std::fabs(Mob::BurnHealthCapFor(fracB) - capB) < 1e-5f;

  // C. now a REAL FIRE, until it dies of it.
  //
  // Direct ignition cannot burn a body through: it lights SURFACE voxels, and
  // once the surface has charred the layer under it never sees three hot
  // faces (reactions.json's minCount note) — measured, 1500 ticks of forced
  // ignition on every limb plateaued at 14% burnt. What burns a body is a
  // fire in the WORLD, whose tangential cells the burn pass reads as a wide
  // front, so this phase soaks a column of fire around the creature every
  // tick exactly as mob-burn's fixture does, and submits real ticks for it.
  // The world is regenerated on the way out (rule 7).
  uint32_t mFire = 0;
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == "fire") mFire = (uint32_t)i;
  const int rootLimb = def.rootLimb;
  int deathTick = -1;
  float fracAtDeath = -1.0f, capAtDeath = -1.0f, lastFrac = fracB;
  float fracAt[2] = {-1.0f, -1.0f};  // at 400 and 800 ticks: is it climbing?
  uint32_t simTick = 41000;
  if (mFire && rootLimb >= 0) {
    for (int i = 0; i < 1200; i++) {
      std::vector<BrushOp> ops;
      std::vector<ParticleSpawn> spawns;
      std::vector<CellOp> cellOps;
      mobs.PreTick(simTick + 1, c.world, ops, cellOps, spawns);
      if (mobs.LimbBody(id, rootLimb)) {
        const Vec3 at = mobs.LimbVoxelPos(id, rootLimb, 0);
        const IVec3 b{ifloor(at.x), ifloor(at.y), ifloor(at.z)};
        // Wider than mob-burn's column (+-3): the human's arms hang at about
        // +-4 and half its surface stayed raw skin at +-3 — measured, 5,883
        // of 10,899 surface voxels never cooked in 1,200 ticks. A body in a
        // BONFIRE, not beside one.
        // ...and ENGULFED, not merely standing in it. With `kCellOpIfAir`
        // the lower legs and feet stayed raw (legL.L 636 of 668 surface
        // voxels, foot.L 368 of 382, after 1,200 ticks in the column): the
        // cells beside a standing body's shins are the ground's own cover,
        // not air, so the flag never put flame there. Every cell above the
        // terrain is overwritten with fire instead; the terrain itself is
        // left alone so the body has something to stand on.
        for (int dy = -8; dy <= 20; dy++)
          for (int dz = -6; dz <= 6; dz++)
            for (int dx = -6; dx <= 6; dx++) {
              const IVec3 cc{b.x + dx, b.y + dy, b.z + dz};
              if (!c.world.CellInWindow(cc)) continue;
              if (cellOps.size() >= kMaxCellOpsPerTick) break;
              const bool aboveGround =
                  cc.y > World::TerrainHeight(cc.x, cc.z, kDefaultSeed);
              cellOps.push_back(
                  {World::SlotCellIndex(cc),
                   PackVoxNew(mFire, 7u) | (aboveGround ? 0u : kCellOpIfAir)});
            }
      }
      c.debris.QueueSupportEvents(c.world.Snap());
      c.debris.PreTick(simTick + 1, c.world, cellOps, spawns);
      ++simTick;
      SubmitTick(c.ctx, c.world, c.sim, simTick, kDefaultSeed, ops, {}, cellOps,
                 false, pchunk, true, false, spawns);
      c.ctx.WaitIdle();
      c.ctx.ProcessEvents();
      c.phys.Step(kTickDt);
      c.debris.PostStep();
      mobs.PostStep();
      if (!mobs.IsAlive(id)) {
        deathTick = i;
        break;
      }
      lastFrac = mobs.BurnFraction(id);
      if (i == 399) fracAt[0] = lastFrac;
      if (i == 799) fracAt[1] = lastFrac;
    }
  }
  // ATTRIBUTION, not a bare number (CLAUDE.md rule 6): what the body is made
  // of when the phase ends, so "died at tick -1 with 23% burnt" names the
  // material fire could not reach instead of leaving the next reader to
  // guess between bone, interior flesh and a fire that never took.
  std::string census;
  if (mobs.IsAlive(id)) {
    const char* names[] = {"skin",          "flesh",        "muscle",
                           "bone",          "linen",        "flesh_cooked",
                           "flesh_burning", "flesh_charred", "flesh_cinder",
                           "linen_burning", "linen_charred", "ash"};
    for (const char* nm : names) {
      uint32_t mat = 0;
      for (size_t i = 0; i < c.mats.size(); i++)
        if (c.mats[i].name == nm) mat = (uint32_t)i;
      if (!mat) continue;
      uint32_t n = 0;
      for (int li = 0; li < nLimbs; li++)
        if (mobs.LimbBody(id, li)) n += mobs.LimbMaterialCount(id, li, mat);
      if (n) census += Format("%s %u ", nm, n);
    }
    uint32_t live = 0, spawn = 0, surface = 0;
    for (int li = 0; li < nLimbs; li++) {
      spawn += mobs.LimbVoxelsAtSpawn(id, li);
      surface += mobs.LimbSurfaceAtSpawn(id, li);
      if (mobs.LimbBody(id, li)) live += mobs.LimbArtVoxelCount(id, li);
    }
    census += Format("| %u of %u voxels present, surface %u | raw skin by "
                     "limb:",
                     live, spawn, surface);
    uint32_t mSkin = 0;
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == "skin") mSkin = (uint32_t)i;
    for (int li = 0; li < nLimbs && mSkin; li++)
      if (mobs.LimbBody(id, li))
        census += Format(" %s %u/%u", def.limbs[li].name.c_str(),
                         mobs.LimbMaterialCount(id, li, mSkin),
                         mobs.LimbSurfaceAtSpawn(id, li));
  }
  // The fraction is read before the tick that killed, since Die() drops the
  // limb list and the readout with it; a body one recount short of the death
  // knot is the closest observable, so allow one cadence of burning.
  fracAtDeath = lastFrac;
  capAtDeath = Mob::BurnHealthCapFor(lastFrac);
  const bool died = deathTick >= 0;
  // Dead OF THE BURNS: the last fraction seen was inside one recount of the
  // death knot (the pass recounts every Mob::kBurnRecountTicks ticks and
  // burning is fast under forced ignition, so the last live reading can sit a
  // little under it). A creature that died with 30% of it burnt died of
  // something else — a vital limb burning through — and that is not this cap.
  const bool ofBurns = died && fracAtDeath >= gore.burnDeathFraction - 0.12f;

  RecordObserved("burnCapFractionB", (double)fracB);
  RecordObserved("burnCapB", (double)capB);
  RecordObserved("burnCapDeathTick", (double)deathTick);
  RecordObserved("burnCapFractionAtDeath", (double)fracAtDeath);

  mobs.Reset();
  c.debris.Reset();
  // LEAVE THE WORLD AS THIS GATE FOUND IT: phase C lit a real fire at
  // absolute coordinates inside the window (the same restore mob-burn and
  // wound-bleed do, for the same reason).
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  const bool ok = curve && bit && underCap && died && ofBurns;
  detail = Format(
      "%s/%s: curve %s; lit %u -> %.1f%% burnt, cap %.3f (curve says %.3f), "
      "limbs under cap=%d (worst over %.2f hp), alive=%d; in a fire: died "
      "at tick %d with %.1f%% burnt (death knot %.0f%%, cap %.3f; %.1f%% at "
      "400, %.1f%% at 800) %s",
      t.defName.c_str(), t.limbName.c_str(), curve ? "ok" : "WRONG", lit,
      fracB * 100.0f, capB, Mob::BurnHealthCapFor(fracB), underCap ? 1 : 0,
      worstOver, aliveB ? 1 : 0, deathTick, fracAtDeath * 100.0f,
      gore.burnDeathFraction * 100.0f, capAtDeath, fracAt[0] * 100.0f,
      fracAt[1] * 100.0f, census.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// one-hit: a single sword blow never kills a healthy creature outright
// ---------------------------------------------------------------------------
//
// Owner report, 2026-09-02, the day blood became hp: "I hit an NPC with a
// sword and every single one of his limbs fell off" — which is a DEATH (only
// Die() releases every limb at once), from one blow. Whatever the cause turns
// out to be, this is the property: one committed swing that crosses several
// limbs — exactly the sequence main.cpp's sweep runs, Damage() then CutLimb()
// per limb hit — leaves the creature alive, with every limb attached, and
// still alive after the wound has bled for five seconds.
Status GateOneHit(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 350));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  IVec3 pchunk{};
  const uint64_t id = SpawnTarget(c, t, 350, pchunk);
  if (!id) {
    detail = "spawn refused";
    return Status::Fail;
  }
  const MobDef& def = mobs.Defs()[t.defIndex];
  const int nLimbs = (int)def.limbs.size();
  // The stock sword's damage at full commitment, and a committed tip speed
  // (melee.fullSpeedMps 3.4 m/s = 34 vox/s) with headroom for the body's
  // own motion.
  const ItemDef* sword = c.items.At(c.items.Find("sword"));
  const float dmg = sword ? sword->damage : 14.0f;
  const float tipSpeed = 45.0f;
  // Three limbs one swing can cross: the fixture limb, its parent, and the
  // root. Hitting the root and a parent is what makes this a whole-body
  // question rather than a thigh question.
  std::vector<int> hits{t.limb};
  for (int li = 0; li < nLimbs; li++)
    if (def.limbs[li].name == def.limbs[t.limb].parent) hits.push_back(li);
  if (def.rootLimb >= 0 && def.rootLimb != t.limb &&
      std::find(hits.begin(), hits.end(), def.rootLimb) == hits.end())
    hits.push_back(def.rootLimb);

  const float hp0 = mobs.TotalHp(id);
  std::vector<ParticleSpawn> spawns;
  int landed = 0;
  {
    MobSystem::BladeCutScope blade(mobs, 1.0f);
    for (int li : hits) {
      const uint64_t body = mobs.LimbBody(id, li);
      if (!body) continue;
      const LimbAxis ax = MeasureLimb(mobs, id, li);
      const Vec3 at = ax.anchor + ax.along * (ax.reach * 0.5f);
      if (!mobs.Damage(body, dmg, at, tipSpeed)) continue;
      landed++;
      if (!mobs.IsAlive(id)) break;
      CutOnce(mobs, c.world, id, li, ax, ax.reach * 0.5f, 1.0f, 1.0f,
              0x0A11u + (uint32_t)li, spawns);
      if (!mobs.IsAlive(id)) break;
    }
  }
  const bool aliveAtBlow = mobs.IsAlive(id);
  const float hpAfterBlow = aliveAtBlow ? mobs.TotalHp(id) : 0.0f;
  const size_t severs = mobs.SeverEvents().size();
  std::string cause = mobs.DeathCause(id);

  // Then the wound bleeds for five seconds of game time.
  uint32_t tick = 50000;
  int deathTick = -1;
  for (int i = 0; i < 150 && mobs.IsAlive(id); i++) {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> sp;
    std::vector<CellOp> cellOps;
    mobs.PreTick(tick++, c.world, ops, cellOps, sp);
    if (!mobs.IsAlive(id)) {
      deathTick = i;
      cause = mobs.DeathCause(id);
      break;
    }
    c.phys.Step(kTickDt);
    mobs.PostStep();
  }
  const bool alive = mobs.IsAlive(id);
  const float hpEnd = alive ? mobs.TotalHp(id) : 0.0f;
  int attached = 0;
  for (int li = 0; li < nLimbs; li++)
    if (mobs.LimbBody(id, li)) attached++;
  const bool intact = severs == 0 && alive && attached == nLimbs;
  // Bounded: a blow across three limbs plus five seconds of bleeding takes
  // well under half the creature. (Three limbs x 14 damage is 42 hp out of a
  // human's 322; the bleed adds 3 x 21 voxels x 0.6 = 38 more.)
  const bool bounded = hp0 > 0.0f && hpEnd > 0.5f * hp0;

  RecordObserved("oneHitHpLostFraction",
                 hp0 > 0.0f ? (double)(hp0 - hpEnd) / (double)hp0 : 1.0);
  mobs.Reset();
  c.debris.Reset();
  const bool ok = landed > 0 && aliveAtBlow && intact && bounded;
  detail = Format(
      "%s: %d limbs hit with the sword (%.0f dmg, tip %.0f vox/s): alive at "
      "the blow=%d (hp %.1f -> %.1f), severs=%zu, %d/%d limbs attached, alive "
      "after 150 ticks=%d (hp %.1f, floor %.1f)%s%s",
      t.defName.c_str(), landed, dmg, tipSpeed, aliveAtBlow ? 1 : 0, hp0,
      hpAfterBlow, severs, attached, nLimbs, alive ? 1 : 0, hpEnd, 0.5f * hp0,
      deathTick >= 0 ? Format(", died at tick %d", deathTick).c_str() : "",
      cause.empty() ? "" : Format(" cause: %s", cause.c_str()).c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// corpse-intact: a corpse stays in one piece when the blade keeps going
// ---------------------------------------------------------------------------
//
// Owner report, 2026-09-02, after one-hit landed and did not reproduce it:
// "when killing an NPC duelist with a sword, every single one of his limbs
// pops off all together." one-hit could not see it because it stops at the
// blow. The sweep does not: MeleeSweepDamage keeps probing for the rest of
// the stroke, the creature's limbs are DEBRIS from the tick it died (Die()
// hands every body to DebrisSystem with its joints on), and a probe that
// finds one of them goes MeltBodyAt -> DamageBody -> RebuildCollider, which
// built a new Jolt body and REMOVED the old one — and Physics::RemoveBody
// destroys every joint on the body it removes. One nick to the torso took the
// neck, both shoulders and both hips off in the same call.
//
// The property: the joints Die() leaves on the corpse survive the corpse
// being carved. Kill the fixture the way the sword does (root limb to zero,
// which Sever() routes to Die()), melt its torso where the swing crosses it
// for three ticks, let it settle. The torso body must have been REBUILT (or
// the gate proves nothing), the joint count must not have moved by one, and
// every body the creature was made of must still have a joint on it.
Status GateCorpseIntact(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 380));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  IVec3 pchunk{};
  const uint64_t id = SpawnTarget(c, t, 380, pchunk);
  if (!id) {
    detail = "spawn refused";
    return Status::Fail;
  }
  const MobDef& def = mobs.Defs()[t.defIndex];
  const int nLimbs = (int)def.limbs.size();
  const int root = def.rootLimb;
  const uint64_t torso = root >= 0 ? mobs.LimbBody(id, root) : 0;
  if (!torso) {
    detail = "fixture has no root limb body";
    return Status::Fail;
  }
  int attached = 0;
  for (int li = 0; li < nLimbs; li++)
    if (mobs.LimbBody(id, li)) attached++;
  const uint32_t jointsAlive = c.phys.JointCount();
  // Where the swing crosses the torso: mid-limb, measured off the live body
  // before it dies, because a corpse has no limbs to measure.
  const LimbAxis ax = MeasureLimb(mobs, id, root);
  const Vec3 mid = ax.anchor + ax.along * (ax.reach * 0.5f);

  // The killing blow. The root limb at zero hp is a death, not an amputation
  // (Mob::HpZeroSevers), and the corpse keeps every joint (Mob::Die).
  {
    MobSystem::BladeCutScope blade(mobs, 1.0f);
    mobs.Damage(torso, 1.0e6f, mid, 45.0f);
  }
  if (mobs.IsAlive(id)) {
    detail = Format("%s: root limb at zero hp did not kill", t.defName.c_str());
    mobs.Reset();
    c.debris.Reset();
    return Status::Fail;
  }
  const std::string cause = mobs.DeathCause(id);
  const uint32_t jointsDead = c.phys.JointCount();

  auto indexOf = [&](uint64_t h) -> int {
    for (uint32_t i = 0; i < c.debris.BodyCount(); i++)
      if (c.debris.BodyHandle(i) == h) return (int)i;
    return -1;
  };
  uint64_t cur = torso;
  int idx = indexOf(cur);
  if (idx < 0) {
    detail = Format("%s: the torso was not adopted as debris on death",
                    t.defName.c_str());
    mobs.Reset();
    c.debris.Reset();
    return Status::Fail;
  }
  const uint32_t vox0 = c.debris.BodyVoxelCount((uint32_t)idx);

  uint32_t tick = 52000;
  auto step = [&]() {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> sp;
    std::vector<CellOp> cellOps;
    mobs.PreTick(tick++, c.world, ops, cellOps, sp);
    c.phys.Step(kTickDt);
    mobs.PostStep();
    c.debris.PostStep();
  };
  // The rest of the stroke: three ticks of the edge crossing the torso at the
  // sword's own kerf width (halfWidth + carveBonus is about a voxel), exactly
  // what MeleeSweepDamage does with a debris hit.
  const float radius = 1.0f;
  std::vector<ParticleSpawn> spawns;
  int melts = 0;
  for (int i = 0; i < 3 && idx >= 0; i++) {
    if (c.debris.MeltBodyAt(cur, mid, radius, c.world, spawns)) melts++;
    spawns.clear();
    cur = c.debris.BodyHandle((uint32_t)idx);  // a rebuild changes the handle
    step();
    idx = indexOf(cur);
  }
  const uint32_t jointsCarved = c.phys.JointCount();
  const uint32_t vox1 = idx >= 0 ? c.debris.BodyVoxelCount((uint32_t)idx) : 0;
  const bool rebuilt = cur != torso && idx >= 0;

  // Then it settles for two seconds.
  for (int i = 0; i < 60; i++) step();
  const uint32_t jointsSettled = c.phys.JointCount();
  int jointed = 0, bodies = 0;
  for (uint32_t i = 0; i < c.debris.BodyCount(); i++) {
    bodies++;
    if (c.phys.JointCount(c.debris.BodyHandle(i)) > 0) jointed++;
  }
  // How far the corpse spread: the largest distance from the torso to any
  // other body of it. Recorded, not asserted — a settling ragdoll spreads
  // legitimately, and the joint account above is the claim.
  float spread = 0.0f;
  if (idx >= 0) {
    const Vec3 tp = c.debris.BodyPosition((uint32_t)idx);
    for (uint32_t i = 0; i < c.debris.BodyCount(); i++)
      spread = std::max(spread, (c.debris.BodyPosition(i) - tp).len());
  }
  RecordObserved("corpseSpreadVox", (double)spread);

  const bool jointsKept = jointsAlive > 0 && jointsDead == jointsAlive &&
                          jointsCarved == jointsAlive &&
                          jointsSettled == jointsAlive;
  const bool ok = melts > 0 && rebuilt && vox1 < vox0 && jointsKept &&
                  jointed == attached;
  mobs.Reset();
  c.debris.Reset();
  detail = Format(
      "%s: died of '%s' with %d/%d limbs on, %u joints; torso melted %d ticks "
      "(%u -> %u voxels, rebuilt=%d): joints %u after the kill, %u after the "
      "carve, %u after settling; %d of %d debris bodies still jointed, spread "
      "%.1f vox",
      t.defName.c_str(), cause.c_str(), attached, nLimbs, jointsAlive, melts,
      vox0, vox1, rebuilt ? 1 : 0, jointsDead, jointsCarved, jointsSettled,
      jointed, bodies, spread);
  return ok ? Status::Pass : Status::Fail;
}

// ---- corpse-armor ----------------------------------------------------------
//
// A CORPSE IN ARMOUR MUST NOT BECOME A MOTOR.
//
// Owner report, 2026-09-13: "I killed a player character with a suit of armour
// on; his head had been separated. The body spasmed out and flew in a billion
// directions, all of his limbs moving dramatically and flying everywhere while
// still technically being attached. The framerate plummeted and the whole game
// froze for several minutes; closing it took a minute or two."
//
// No crash.log, so this is NOT the FP-overflow kill 46e3848 closed: that one
// dies in JobIntegrateVelocity with 0xC0000091. "Still technically attached"
// says the joints held, which means the energy came from the SOLVER, not from
// a blast — a jointed rig that gains energy every step until every limb sits
// at Jolt's own clamp (47.1 rad/s, 500 m/s). That state is entirely inside
// what the FP net calls sane (kInsaneSpin 1e4, kInsaneSpeed 1e5), which is
// exactly why nothing reported it: the net is looking for garbage, and this is
// not garbage — it is a physically-typed number that is absurd for a severed
// arm.
//
// Armour is the ingredient that makes a corpse different from every corpse
// `corpse-intact` has ever tested. A worn shell is its own Jolt body, FIXED to
// the limb it wraps (Mob::AppendWornShell) and geometrically INSIDE it, so a
// dressed corpse has twice the bodies, twice the joints, mass ratios of iron
// against flesh across a stiff constraint, and a dozen deep overlaps held
// apart only by the mob's collision group. Every one of those is an energy
// source the undressed fixture does not have. The severed limb is the second:
// a limb cut off a LIVE creature takes its shells with it (DetachLimb's
// parent-name recursion), goes through the kinematic hold, and comes out of
// the mob's collision group on the far side (TickSeveredHolds) — so the pile
// the corpse lands in contains pieces that CAN hit it.
//
// THE PROPERTY, stated so it cannot pass by accident: after the corpse of a
// creature that was wearing everything the library ships has been dismembered
// and killed, no piece of it may move faster than `corpseArmorMaxSpeedVox`
// voxels/s or spin faster than `corpseArmorMaxSpinRad` rad/s on ANY tick of
// the three seconds that follow, and the remains may not spread further than
// `corpseArmorMaxSpreadVox`. Bounds live in tests/baseline.json so they cost
// no rebuild to tune (CLAUDE.md, "authoring cheap-to-verify work").
//
// EVERY PEAK CARRIES ITS ATTRIBUTION (rule 6). "204 vox/s" is a bare number
// and bisecting it costs a run per hypothesis; the same measurement also
// reports WHEN it peaked, WHICH body, where that body was, how far under the
// ground it was, and whether the velocity was straight down. A peak on tick 1
// is the kill's own impulse, a peak that grows smoothly at 98 vox/s^2 is a
// piece in free fall through terrain nobody built a collider for, and a peak
// that climbs while the body is ON the ground is the solver pumping. Those are
// three different bugs and the detail line names which one it saw.
//
// The worst single physics step is RECORDED, not asserted: it is the number
// the report was actually about, but wall-clock in a gate is a flake and the
// speed bound above is the deterministic statement of the same fact.
Status GateCorpseArmor(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 380));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  IVec3 pchunk{};
  const uint64_t id = SpawnTarget(c, t, 380, pchunk);
  if (!id) {
    detail = "spawn refused";
    return Status::Fail;
  }
  const MobDef& def = mobs.Defs()[t.defIndex];
  const int root = def.rootLimb;
  const uint64_t torso = root >= 0 ? mobs.LimbBody(id, root) : 0;
  Mob* mob = mobs.FindMobById(id);
  if (!torso || !mob) {
    detail = "fixture has no root limb body";
    return Status::Fail;
  }
  const int bareLimbs = mob->LimbCount();
  // WHOSE BODIES ARE THESE? Every measurement below is over DebrisSystem's
  // whole list, and in a full --selftest that list is not empty when this gate
  // starts: `corpse-armor` run alone saw 30 bodies and 28 joints, and the same
  // gate inside the suite saw 33 and 43 -- an earlier gate's rig is still
  // standing (SpawnTarget resets mobs and debris, it does not reset Physics,
  // and the avatar outlives a MobSystem::Reset). Measuring "spread" against
  // BodyPosition(0) then measured the distance from THIS corpse to a cactus
  // two gates ago and reported 409 voxels, which is a fixture measuring the
  // suite rather than the subject -- rule 7 in as few words as it gets. So the
  // pre-existing set is recorded here and excluded from everything after it.
  std::unordered_set<uint64_t> foreignBodies;
  for (uint32_t b = 0; b < c.debris.BodyCount(); b++)
    foreignBodies.insert(c.debris.BodyHandle(b));
  const uint32_t foreignCount = (uint32_t)foreignBodies.size();

  // THE KILLER IS STANDING OVER THE BODY, because that is where the report
  // happened and because it is the one thing this fixture cannot get for free:
  // a corpse that falls inside the player's capsule is the case
  // ReleaseToWorldWhenClear exists for, and a gate with no proxy in the world
  // takes the other branch of that function and tests nothing.
  const Vec3 stand = mobs.LimbAnchorPos(id, root) + Vec3{2.0f, 0.0f, 0.0f};
  const uint64_t proxy = c.phys.CreatePlayerBody(Player::kHalfXZ,
                                                 Player::kHalfY);
  uint32_t tick = 53000;
  // THE WHOLE TICK, not the physics half of it. A corpse needs GROUND, and
  // the ground under a rigidbody is a collider DebrisSystem::ManageTerrain
  // meshes out of the CPU mirror — which only exists if the world is actually
  // submitted. The first version of this gate ran mobs.PreTick + phys.Step
  // the way `corpse-intact` does (it only counts joints, so it does not care)
  // and measured a corpse in free fall 210 voxels UNDER the terrain: no
  // mirror, no collider, nothing to land on, and therefore none of the ground
  // contact that the report is about. That is the trap recorded as
  // "a CPU-only fixture has no ground", and it costs a GPU submit per tick.
  auto step = [&](double* outMs) {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> sp;
    std::vector<CellOp> cellOps;
    mobs.PreTick(tick + 1, c.world, ops, cellOps, sp);
    c.debris.QueueSupportEvents(c.world.Snap());
    c.debris.PreTick(tick + 1, c.world, cellOps, sp);
    ++tick;
    SubmitTick(c.ctx, c.world, c.sim, tick, kDefaultSeed, ops, {}, cellOps,
               false, pchunk, true, false, sp);
    c.ctx.WaitIdle();
    c.ctx.ProcessEvents();
    // Pinned, the way selftest_phys's release test pins it: MovePlayerBody
    // hands the proxy the velocity the move implies and the game re-teleports
    // it every tick, so a proxy left alone sails off at 30 m/s and every
    // overlap test after that reads a capsule that has left.
    if (proxy) {
      c.phys.MovePlayerBody(proxy, stand, kTickDt);
      c.phys.SetBodyVelocity(proxy, Vec3{});
    }
    const auto t0 = std::chrono::steady_clock::now();
    c.phys.Step(kTickDt);
    if (outMs)
      *outMs = std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - t0)
                   .count();
    c.debris.PostStep();
    mobs.PostStep();
  };

  // DRESS IT IN EVERYTHING THE LIBRARY SHIPS. Counted, never named: a tree
  // with no armour content in it still runs this gate, it simply runs it on a
  // naked corpse and says so in the detail line (which is the honest outcome,
  // not a silent pass — `armor-wear` is what asserts the content exists).
  int wornPieces = 0;
  for (const ItemDef& it : c.items.items) {
    if (!ItemKindIsWorn(it.kind)) continue;
    int home = -1;
    for (int s = 0; s < kEquipSlotCount; s++)
      if (EquipSlotAccepts(s, it.kind)) { home = s; break; }
    if (home < 0) continue;
    if (mob->WearItem(&it, home)) wornPieces++;
  }
  const int shells = mob->LimbCount() - bareLimbs;
  // Let the shells reach their hosts before anything is cut: a piece worn this
  // tick is placed by WearItem itself, but the drive loop is what proves the
  // two agree, and a gate that cut on the wear frame would be measuring the
  // placement rather than the corpse.
  for (int i = 0; i < 8; i++) step(nullptr);
  const uint32_t jointsDressed = c.phys.JointCount();

  // WHICH LIMB COMES OFF: the severable, non-vital one carrying the MOST
  // armour, chosen rather than named (the cast trap this file opens with). On
  // the shipped human that is a limb under a helm or a pauldron; on a rig with
  // no armour on any severable part it falls back to the biggest one, and the
  // gate still means something — it is then the undressed claim.
  int cutLimb = t.limb;
  {
    int bestShells = -1;
    for (int li = 0; li < bareLimbs && li < (int)def.limbs.size(); li++) {
      if (li == root || !def.limbs[li].severable || def.limbs[li].vital)
        continue;
      if (!mobs.LimbBody(id, li)) continue;
      int n = 0;
      for (int s = bareLimbs; s < mob->LimbCount(); s++)
        if (mob->LimbDefAt(s).parent == def.limbs[li].name) n++;
      if (n > bestShells) { bestShells = n; cutLimb = li; }
    }
  }
  const std::string cutName = mob->LimbDefAt(cutLimb).name;
  // WHICH OF THE CORPSE'S BODIES ARE ARMOUR. Captured here, while the rig
  // still knows: after Die() the handles are DebrisSystem's and nothing
  // downstream remembers that one of them used to be a pauldron. Handles
  // survive the adoption (Mob::Die hands the same body over), so membership in
  // this set is what lets the peak below say "it was a shell" instead of "it
  // was body 67108874" -- the difference between a finding and a number.
  std::unordered_set<uint64_t> shellBodies;
  for (int sIdx = bareLimbs; sIdx < mob->LimbCount(); sIdx++)
    if (const uint64_t h = mobs.LimbBody(id, sIdx)) shellBodies.insert(h);

  // TAKE IT OFF THE LIVE CREATURE, through the geometry (hp <= 0 no longer
  // dismembers — see the three-instant-severs note in Mob::Damage), which
  // means cutting the armour off it first. Bounded, and the count is reported:
  // a fixture that cannot be cut through in 120 strokes is a content change
  // worth seeing, not a hang.
  const LimbAxis hax = MeasureLimb(mobs, id, cutLimb);
  int strokes = 0;
  std::vector<ParticleSpawn> spawns;
  while (strokes < 120 && mobs.LimbBody(id, cutLimb) && mobs.IsAlive(id)) {
    CutOnce(mobs, c.world, id, cutLimb, hax, hax.reach * 0.5f, 1.0f, 1.6f,
            0xC0DEu + (uint32_t)strokes, spawns);
    spawns.clear();
    strokes++;
    if ((strokes & 3) == 0) step(nullptr);
  }
  const bool severed = mobs.LimbBody(id, cutLimb) == 0;
  // Through the hold, so the piece is dynamic and out of its collision group
  // before the kill (MobSystem::TickSeveredHolds) — the state the corpse has
  // to coexist with.
  for (int i = 0; i < 30 && mobs.IsAlive(id); i++) step(nullptr);

  // Then the kill, the way the sword delivers it: the root limb at zero hp is
  // a death, not an amputation (Mob::HpZeroSevers), and the corpse keeps every
  // joint (Mob::Die).
  if (mobs.IsAlive(id)) {
    const LimbAxis ax = MeasureLimb(mobs, id, root);
    MobSystem::BladeCutScope blade(mobs, 1.0f);
    mobs.Damage(torso, 1.0e6f, ax.anchor + ax.along * (ax.reach * 0.5f), 45.0f);
  }
  const bool died = !mobs.IsAlive(id);
  const uint32_t jointsDead = c.phys.JointCount();
  // THE CORPSE, BY HANDLE, CAPTURED ONCE. Excluding the bodies that existed
  // before the fixture ran is NOT enough, and the second version of this gate
  // is how we know: in a full --selftest an earlier gate's creature is still
  // standing when this one starts (43 joints in the world where this rig
  // accounts for 28), and when it dies DURING the 180 ticks below its limbs
  // enter DebrisSystem too -- 400 voxels away, which is exactly where "spread
  // 398.8 vox, cap 60" came from while every piece of THIS corpse was inside
  // 35. A snapshot taken before the fixture cannot see a body that arrives
  // after it; a snapshot of the corpse itself can.
  //
  // A handle changes if DamageBody rebuilds a collider, and such a piece drops
  // out of the measurement. Nothing carves during the settle below, so that
  // does not happen today; if it starts to, the symptom is the surviving count
  // falling over the run rather than a wrong number.
  std::unordered_set<uint64_t> mine;
  for (uint32_t b = 0; b < c.debris.BodyCount(); b++) {
    const uint64_t h = c.debris.BodyHandle(b);
    if (!foreignBodies.count(h)) mine.insert(h);
  }

  // Three seconds of corpse, measured every tick over every piece.
  c.phys.ResetRunawayProbe();
  float maxSpeed = 0.0f, maxSpin = 0.0f, maxSpread = 0.0f;
  int speedTick = -1, spinTick = -1;
  bool spinIsShell = false;
  // 90% of Jolt's default max angular velocity (15*pi rad/s). See the note at
  // the sample below for why the threshold is expressed against the CLAMP.
  const float kPegged = 0.9f * 47.1238898f;
  uint32_t peggedSamples = 0;
  int lastHotTick = -1;
  std::unordered_set<uint64_t> peggedBodies, peggedShells;
  Vec3 peakPos{}, peakVel{};
  float peakUnderGround = 0.0f;
  double worstStepMs = 0.0;
  int worstStepTick = -1;
  const int kTicks = 180;
  for (int i = 0; i < kTicks; i++) {
    double ms = 0.0;
    step(&ms);
    if (ms > worstStepMs) { worstStepMs = ms; worstStepTick = i; }
    Vec3 anchor{};
    bool haveAnchor = false;
    for (uint32_t b = 0; b < c.debris.BodyCount(); b++) {
      const uint64_t h = c.debris.BodyHandle(b);
      if (!mine.count(h)) continue;   // not this corpse: see above
      const Vec3 p = c.debris.BodyPosition(b);
      Vec3 lin{}, ang{};
      if (c.phys.GetBodyVelocities(h, lin, ang)) {
        const float sp = lin.len(), sq = ang.len();
        if (sp > maxSpeed) {
          maxSpeed = sp;
          speedTick = i;
          peakPos = p;
          peakVel = lin;
          const float ground = (float)World::TerrainHeight(
              (int)std::floor(p.x), (int)std::floor(p.z), kDefaultSeed);
          peakUnderGround = ground - p.y;
        }
        if (sq > maxSpin) {
          maxSpin = sq;
          spinTick = i;
          spinIsShell = shellBodies.count(h) != 0;
        }
        // IS IT A TRANSIENT OR A MOTOR? One peak says nothing; these three
        // numbers say which of the two this is. `kPegged` is 90% of Jolt's
        // own angular clamp (47.124 rad/s), because a body AT the clamp is by
        // definition a body whose spin the solver would have made larger.
        if (sq >= kPegged) {
          peggedSamples++;
          peggedBodies.insert(h);
          lastHotTick = i;
          if (shellBodies.count(h)) peggedShells.insert(h);
        }
      }
      if (!haveAnchor) { anchor = p; haveAnchor = true; }
      maxSpread = std::max(maxSpread, (p - anchor).len());
    }
  }
  // Is the fastest thing simply FALLING? Gravity is the null hypothesis and it
  // is one dot product, not a run: a piece in free fall points straight down.
  const float fallFrac =
      maxSpeed > 1e-3f ? (-peakVel.y / maxSpeed) : 0.0f;
  if (proxy) c.phys.RemoveBody(proxy);

  const Physics::RunawayProbe net = c.phys.Runaway();
  uint32_t ours = 0;
  for (uint32_t b = 0; b < c.debris.BodyCount(); b++)
    if (mine.count(c.debris.BodyHandle(b))) ours++;

  // ---- B. THE NET ITSELF, DRIVEN ON PURPOSE -------------------------------
  //
  // A safety net nobody has seen work is not a safety net -- the lesson
  // 46e3848 paid SANDVOX_PHYS_FAULT to learn, applied again. Part A above
  // measures an ORDINARY armoured corpse and the honest outcome there is that
  // the net never fires, which would ship the whole of SweepRunawayRigs
  // unexercised. So one surviving body is turned into the motor the report
  // describes: its spin is written back to the ceiling every single tick,
  // through the public setter and therefore through Jolt's own clamp, which is
  // exactly the state a constraint that is feeding a body leaves it in.
  //
  // The net must then do both of its stages -- slow it, and when that does not
  // take, cut whatever is attached and stop it. Driven this way there is no
  // constraint to blame, so `cut` proves the escalation happens at all and the
  // final reading proves the body ends up STOPPED rather than buzzing.
  uint32_t drivenTicks = 0, drivenDamped = 0, drivenCut = 0;
  uint32_t drivenJointsBefore = 0, drivenJointsAfter = 0;
  bool netHeld = false;
  float endSpin = -1.0f;
  {
    c.phys.ResetRunawayProbe();
    // TWO FRESH BODIES AND A JOINT, not a piece of the corpse above. The first
    // version of this arm drove a corpse limb and the limb was culled out from
    // under it on tick 38 of the 45 the escalation needs, which is a fixture
    // measuring its own bookkeeping rather than the net (and is why the arm
    // reported "damped 26, cut 0" -- a true statement about a body that no
    // longer existed). These two are owned by nothing, so they last exactly as
    // long as this block.
    Vec3 at{400.0f, 200.0f, 400.0f};
    for (uint32_t b = 0; b < c.debris.BodyCount(); b++)
      if (mine.count(c.debris.BodyHandle(b))) {
        at = c.debris.BodyPosition(b);
        break;
      }
    const Vec3 hi{at.x, at.y + 24.0f, at.z};
    const uint64_t a = c.phys.CreateSphereBody(hi, 2.0f, 2000.0f, Vec3{});
    const uint64_t b =
        c.phys.CreateSphereBody(Vec3{hi.x + 4.0f, hi.y, hi.z}, 2.0f, 2000.0f,
                                Vec3{});
    Physics::JointDesc jd;
    jd.type = Physics::JointType::Fixed;
    jd.anchorVoxel = Vec3{hi.x + 2.0f, hi.y, hi.z};
    const uint64_t j = (a && b) ? c.phys.CreateJoint(a, b, jd) : 0;
    drivenJointsBefore = a ? c.phys.JointCount(a) : 0;
    if (a) {
      const float ceiling = Physics::MaxBodySpinRad();
      // Written back to the ceiling EVERY tick, through the public setter and
      // therefore through Jolt's own clamp -- which is exactly the state a
      // constraint that is feeding a body leaves it in, and the one thing the
      // FP net above deliberately passes because it is a legal number.
      for (int i = 0; i < 200; i++) {
        Vec3 lin{}, ang{};
        if (!c.phys.GetBodyVelocities(a, lin, ang)) break;
        c.phys.SetBodyVelocities(a, lin, Vec3{0.0f, 0.0f, ceiling});
        c.phys.WakeNear(hi, 32.0f);
        drivenTicks++;
        step(nullptr);
        if (c.phys.Runaway().cut > 0) break;   // the escalation has happened
      }
      Vec3 lin{}, ang{};
      if (c.phys.GetBodyVelocities(a, lin, ang)) endSpin = ang.len();
      drivenJointsAfter = c.phys.JointCount(a);
    }
    const Physics::RunawayProbe drivenNet = c.phys.Runaway();
    drivenDamped = drivenNet.damped;
    drivenCut = drivenNet.cut;
    // Slowed, then cut -- joints and all -- and stopped. `endSpin` is read
    // AFTER the cut, so a body still buzzing at the ceiling fails here even
    // though both counters moved.
    netHeld = a != 0 && b != 0 && j != 0 && drivenJointsBefore > 0 &&
              drivenNet.damped > 0 && drivenNet.cut > 0 &&
              drivenJointsAfter == 0 && endSpin >= 0.0f &&
              endSpin < 0.9f * Physics::MaxBodySpinRad();
    if (a) c.phys.RemoveBody(a);
    if (b) c.phys.RemoveBody(b);
    RecordObserved("corpseArmorNetDamped", (double)drivenNet.damped);
    RecordObserved("corpseArmorNetCut", (double)drivenNet.cut);
  }

  const double speedCap = BaselineNumber("corpseArmorMaxSpeedVox", 150.0);
  const double spreadCap = BaselineNumber("corpseArmorMaxSpreadVox", 60.0);
  const double peggedCap = BaselineNumber("corpseArmorMaxPeggedTicks", 4.0);
  RecordObserved("corpseArmorMaxSpeedVox", (double)maxSpeed);
  RecordObserved("corpseArmorMaxSpinRad", (double)maxSpin);
  RecordObserved("corpseArmorMaxSpreadVox", (double)maxSpread);
  RecordObserved("corpseArmorWorstStepMs", worstStepMs);
  RecordObserved("corpseArmorPeggedBodyTicks", (double)peggedSamples);

  // THE PEAK SPIN IS RECORDED, NOT ASSERTED, and that is the whole shape of
  // the fix. 47.12 rad/s is Jolt's clamp and a limb genuinely thrown by a
  // sword blow may touch it for a tick; what may not happen is a limb SITTING
  // there, which is `peggedSamples`, and what may never happen is the net
  // having to intervene on an ordinary corpse, which is `cut` and `repaired`.
  // An assertion on the peak would have to be set above the clamp to pass at
  // all, and would then assert nothing.
  const bool ok = died && maxSpeed <= (float)speedCap &&
                  maxSpread <= (float)spreadCap &&
                  (double)peggedSamples <= peggedCap && net.cut == 0 &&
                  net.repaired == 0 && netHeld;
  detail = Format(
      "%s: %d base limbs + %d shells from %d worn pieces, %u joints dressed; "
      "%s cut off in %d strokes (severed=%d), died=%d with %u joints, %u of "
      "its %u debris bodies left (%u were in the world before it); over %d "
      "corpse ticks the fastest piece hit %.1f vox/s on "
      "tick %d (cap %.0f) at (%.1f, %.1f, %.1f), %.1f vox under ground, %.0f%% "
      "straight down; fastest spin %.2f rad/s on tick %d (%s, recorded not capped: %.2f); spread "
      "%.1f vox (cap %.0f); %u body-ticks pegged at Jolt's own clamp across %u "
      "bodies (%u of them armour, cap %.0f), last on tick %d; the net damped "
      "%u / cut %u / repaired %u; worst physics step %.1f ms on tick %d. "
      "Driven arm: %u ticks at the ceiling -> damped %u, cut %u, joints %u -> "
      "%u, ended at %.1f rad/s (%s)",
      t.defName.c_str(), bareLimbs, shells, wornPieces, jointsDressed,
      cutName.c_str(), strokes, severed ? 1 : 0, died ? 1 : 0, jointsDead,
      ours, (unsigned)mine.size(), foreignCount, kTicks, (double)maxSpeed,
      speedTick, speedCap,
      (double)peakPos.x, (double)peakPos.y, (double)peakPos.z,
      (double)peakUnderGround, (double)(fallFrac * 100.0f), (double)maxSpin,
      spinTick, spinIsShell ? "armour" : "flesh", (double)maxSpin,
      (double)maxSpread, spreadCap, peggedSamples,
      (unsigned)peggedBodies.size(), (unsigned)peggedShells.size(), peggedCap,
      lastHotTick, net.damped, net.cut, net.repaired, worstStepMs,
      worstStepTick, drivenTicks, drivenDamped, drivenCut, drivenJointsBefore,
      drivenJointsAfter, (double)endSpin, netHeld ? "held" : "DID NOT HOLD");
  mobs.Reset();
  c.debris.Reset();
  return ok ? Status::Pass : Status::Fail;
}

// ---- corpse-bleed ----------------------------------------------------------
//
// The owner's second look at the anatomy (2026-09-02), as claims:
//   A. a wound is SEEN. The soak lands on exposed voxels at a higher rate
//      than on buried ones, and never on a material that is not tissue
//      (bone stays bone in the hole: MobDef::tissue).
//   B. a decapitation takes the head off as its OWN body (no joint left), and
//      both pieces, the head and the torso's neck stump, carry an open wound.
//   C. those wounds pay out from where each piece is (blood attributed to the
//      nearer of the two wounds), and then close: a corpse does not pump.
//   D. a cut on the corpse afterwards opens a wound again.
// CPU only: no tick is submitted. The debris system's own PreTick drains the
// wounds, exactly as main.cpp calls it.
Status GateCorpseBleed(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 440));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  const MobDef& def = mobs.Defs()[t.defIndex];
  int head = -1, neck = -1;
  for (size_t li = 0; li < def.limbs.size(); li++)
    if (def.limbs[li].vital && def.limbs[li].severable &&
        (int)li != def.rootLimb) {
      head = (int)li;
      break;
    }
  if (head >= 0)
    for (size_t li = 0; li < def.limbs.size(); li++)
      if (def.limbs[li].name == def.limbs[head].parent) neck = (int)li;
  if (head < 0 || neck < 0) {
    detail = Format("%s has no vital severable limb with a parent",
                    t.defName.c_str());
    return Status::Skip;
  }
  IVec3 pchunk{};
  const uint64_t id = SpawnTarget(c, t, 440, pchunk);
  if (!id) {
    detail = "spawn refused";
    return Status::Fail;
  }
  const uint32_t bleedMat = def.bleedMat, woundMat = def.woundMat;

  // ---- A. the soak is on what is exposed, and never on bone ---------------
  const std::vector<PrefabVoxel> L0 = mobs.LimbLattice(id, t.limb);
  const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
  std::vector<ParticleSpawn> spawns;
  const bool hit = CutOnce(mobs, c.world, id, t.limb, ax, ax.reach * 0.5f,
                           0.5f, 1.0f, 0xC0B5u, spawns);
  spawns.clear();
  const std::vector<PrefabVoxel> L1 = mobs.LimbLattice(id, t.limb);
  auto keyOf = [](int x, int y, int z) {
    return ((uint64_t)(uint16_t)x << 32) | ((uint64_t)(uint16_t)y << 16) |
           (uint64_t)(uint16_t)z;
  };
  std::unordered_map<uint64_t, uint16_t> was;
  was.reserve(L0.size() * 2);
  for (const PrefabVoxel& v : L0) was[keyOf(v.x, v.y, v.z)] = v.material & 0xFFFu;
  std::unordered_set<uint64_t> occ;
  occ.reserve(L1.size() * 2);
  for (const PrefabVoxel& v : L1) occ.insert(keyOf(v.x, v.y, v.z));
  auto exposed = [&](const PrefabVoxel& v) {
    return !occ.count(keyOf(v.x - 1, v.y, v.z)) || !occ.count(keyOf(v.x + 1, v.y, v.z)) ||
           !occ.count(keyOf(v.x, v.y - 1, v.z)) || !occ.count(keyOf(v.x, v.y + 1, v.z)) ||
           !occ.count(keyOf(v.x, v.y, v.z - 1)) || !occ.count(keyOf(v.x, v.y, v.z + 1));
  };
  uint32_t nExposed = 0, nBuried = 0, stainedExposed = 0, stainedBuried = 0,
           nonTissueStained = 0;
  for (const PrefabVoxel& v : L1) {
    const bool ex = exposed(v);
    (ex ? nExposed : nBuried)++;
    if ((v.material & 0xFFFu) != (woundMat & 0xFFFu)) continue;
    const auto it = was.find(keyOf(v.x, v.y, v.z));
    if (it == was.end() || it->second == (woundMat & 0xFFFu)) continue;
    const uint16_t m0 = it->second;
    if (!def.tissue.empty() && (m0 >= def.tissue.size() || !def.tissue[m0]))
      nonTissueStained++;
    (ex ? stainedExposed : stainedBuried)++;
  }
  const double rateExposed = nExposed ? (double)stainedExposed / nExposed : 0.0;
  const double rateBuried = nBuried ? (double)stainedBuried / nBuried : 0.0;
  const bool soakOk = hit && stainedExposed > 0 && rateExposed > rateBuried &&
                      nonTissueStained == 0 && mobs.IsAlive(id);
  RecordObserved("corpseBleedSoakExposedRate", rateExposed);
  RecordObserved("corpseBleedSoakBuriedRate", rateBuried);

  // ---- B. decapitation: the head is its own body, both pieces wounded -----
  const uint64_t headBody = mobs.LimbBody(id, head);
  uint64_t neckBody = mobs.LimbBody(id, neck);  // re-read after the melt
  mobs.Sever(id, head);
  const bool died = !mobs.IsAlive(id);
  const std::string cause = mobs.DeathCause(id);
  auto indexOf = [&](uint64_t h) -> int {
    for (uint32_t i = 0; i < c.debris.BodyCount(); i++)
      if (c.debris.BodyHandle(i) == h) return (int)i;
    return -1;
  };
  const int hi0 = indexOf(headBody), ni0 = indexOf(neckBody);
  const bool headOff = hi0 >= 0 && c.phys.JointCount(headBody) == 0;
  const bool bothWounded = hi0 >= 0 && ni0 >= 0 &&
                           c.debris.BodyWoundOpen((uint32_t)hi0) &&
                           c.debris.BodyWoundOpen((uint32_t)ni0);
  const float headBudget0 = hi0 >= 0 ? c.debris.BodyWoundBudget((uint32_t)hi0) : 0.0f;
  const float neckBudget0 = ni0 >= 0 ? c.debris.BodyWoundBudget((uint32_t)ni0) : 0.0f;
  const uint32_t wounded0 = c.debris.WoundedBodyCount();

  // ---- C. each piece bleeds from where it is, then the wounds close -------
  uint32_t tick = 61000;
  double bloodHead = 0.0, bloodNeck = 0.0;
  int closedTick = -1, closedAgainTick = -1;
  bool melted = false, reopened = false, cutSkipped = false;
  // WHERE THE BODIES WENT, so a wound that "closed" early names its cause: a
  // body that left is not a wound that paid out.
  int neckGoneTick = -1, headGoneTick = -1;
  Vec3 neckLast{}, neckFirst{};
  float neckBudgetAtGone = -1.0f;
  const int window = 700;
  for (int i = 0; i < window; i++) {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> sp;
    std::vector<CellOp> cellOps;
    mobs.PreTick(tick + 1, c.world, ops, cellOps, sp);  // sweeps the husk
    const size_t fromMobs = sp.size();
    const int hi = indexOf(headBody), ni = indexOf(neckBody);
    const Vec3 hw = hi >= 0 ? c.debris.BodyWoundWorld((uint32_t)hi) : Vec3{};
    const Vec3 nw = ni >= 0 ? c.debris.BodyWoundWorld((uint32_t)ni) : Vec3{};
    if (ni >= 0) {
      neckLast = c.debris.BodyPosition((uint32_t)ni);
      if (i == 0) neckFirst = neckLast;
      neckBudgetAtGone = c.debris.BodyWoundBudget((uint32_t)ni);
    } else if (neckGoneTick < 0) {
      neckGoneTick = i;
    }
    if (hi < 0 && headGoneTick < 0) headGoneTick = i;
    c.debris.QueueSupportEvents(c.world.Snap());
    c.debris.PreTick(tick + 1, c.world, cellOps, sp);
    for (size_t k = fromMobs; k < sp.size(); k++) {
      const ParticleSpawn& p = sp[k];
      if ((p.payload & 0xFFFu) != (bleedMat & 0xFFFu)) continue;
      const Vec3 at{(float)p.px / 256.0f, (float)p.py / 256.0f,
                    (float)p.pz / 256.0f};
      const float dh = hi >= 0 ? (at - hw).len() : 1e9f;
      const float dn = ni >= 0 ? (at - nw).len() : 1e9f;
      if (dh < dn) bloodHead += 1.0; else bloodNeck += 1.0;
    }
    // A REAL TICK, because the corpse needs ground: debris terrain meshes are
    // built from the chunk cache the tick's readback fills (ManageTerrain
    // requests a fetch and waits), and without one the whole ragdoll fell
    // 225 voxels out of the window at t+66 with 24 voxels of budget unspent.
    ++tick;
    SubmitTick(c.ctx, c.world, c.sim, tick, kDefaultSeed, ops, {}, cellOps,
               false, pchunk, true, true, sp);
    c.ctx.WaitIdle();
    c.ctx.ProcessEvents();
    c.phys.Step(kTickDt);
    c.debris.PostStep();
    mobs.PostStep();
    if (closedTick < 0 && c.debris.WoundedBodyCount() == 0) {
      closedTick = i;
      // ---- D. a cut on the corpse opens a wound again ----------------------
      // The moment every wound has paid out (a settled body has folded into
      // the grid and cannot be cut), at the neck's own wound point (a point
      // ON the body), the sword's kerf. MeltBodyAt rebuilds the collider,
      // which replaces the handle; the INDEX survives unless the body was
      // destroyed, so the wound is read back by index.
      const int ni1 = indexOf(neckBody);
      if (ni1 < 0) {
        cutSkipped = true;
      } else {
        const Vec3 at = c.debris.BodyWoundWorld((uint32_t)ni1);
        melted = c.debris.MeltBodyAt(neckBody, at, 1.5f, c.world, spawns);
        spawns.clear();
        reopened = melted && c.debris.BodyWoundOpen((uint32_t)ni1) &&
                   c.debris.BodyWoundBudget((uint32_t)ni1) > 0.0f;
        neckBody = c.debris.BodyHandle((uint32_t)ni1);  // the rebuild replaced it
      }
    } else if (closedTick >= 0 && closedAgainTick < 0 &&
               c.debris.WoundedBodyCount() == 0) {
      closedAgainTick = i;
    }
  }
  const bool drained = bloodHead > 0.0 && bloodNeck > 0.0;
  const bool closes = closedTick >= 0;
  RecordObserved("corpseBleedHeadSpawns", bloodHead);
  RecordObserved("corpseBleedNeckSpawns", bloodNeck);

  const bool ok = soakOk && died && headOff && bothWounded && drained &&
                  closes && (cutSkipped || (reopened && closedAgainTick >= 0));
  const uint32_t bodiesAtEnd = c.debris.BodyCount();
  mobs.Reset();
  c.debris.Reset();
  // Real blood went into the world at absolute coordinates: leave the world
  // as this gate found it, the same restore wound-bleed and mob-burn do.
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  detail = Format(
      "%s: soak on %s: %u exposed (rate %.3f) vs %u buried (rate %.3f), %u "
      "non-tissue stained; died of '%s', head off=%d, head wound=%d (%.0f vox) "
      "neck wound=%d (%.0f vox), %u wounded bodies; over %d ticks the head "
      "shed %.0f blood spawns and the neck %.0f, wounds closed at t+%d; corpse "
      "cut %s, closed again at t+%d; neck body gone at t+%d (budget %.0f left, "
      "moved %.1f vox, last y %.1f), head body gone at t+%d, %u bodies at end",
      t.defName.c_str(), t.limbName.c_str(), stainedExposed, rateExposed,
      stainedBuried, rateBuried, nonTissueStained, cause.c_str(),
      headOff ? 1 : 0, hi0 >= 0 ? 1 : 0,
      headBudget0, ni0 >= 0 ? 1 : 0, neckBudget0, wounded0, window, bloodHead,
      bloodNeck, closedTick,
      cutSkipped ? "skipped (the torso had settled into the grid)"
                 : (reopened ? "reopened the wound" : "did NOT reopen the wound"),
      closedAgainTick, neckGoneTick, neckBudgetAtGone,
      (neckLast - neckFirst).len(), neckLast.y, headGoneTick, bodiesAtEnd);
  return ok ? Status::Pass : Status::Fail;
}


// ---------------------------------------------------------------------------
// corpse-burn: a body that dies alight goes on burning (Gore §G, corpses)
// ---------------------------------------------------------------------------
//
// Reported 2026-09-02: an NPC that died on fire lay there with its embers
// pulsing at the colour they died in, for good — no char, no smoke, no ash.
// The live creature burns through MobSystem::BurnOneLimb; the moment Die()
// hands its limbs to DebrisSystem the same voxels are DebrisSystem::BurnBodies'
// business, and mob-burn's corpse subtest only asks that the corpse lose at
// least one voxel in 120 ticks, which a single ember shedding ash satisfies.
//
//   A. the same bonfire burn-cap dies in, until the creature is dead, and it
//      must die with fire ON it (else there is nothing to test).
//   B. the corpse, with the fixture's fire gone, for `window` ticks: EVERY
//      piece that died with embers must have advanced them — burning falls,
//      char rises — and the corpse as a whole must have retired most of what
//      was alight. Per piece, because a shared scan budget spent in list
//      order starves the tail (Mob::BurnTick learned this on limbs), and a
//      total hides a foot that never burned behind a torso that did.
//   C. the corpse keeps putting real fire into the world while alight — the
//      smoke and flame a burning body shows are grid fire it emits.
//   D. THE BRICK AGREES WITH THE LATTICE. The lattice is what burns; the
//      brick is what is drawn. A poke that misses (wrong frame, unowned
//      model, pool full) leaves a corpse whose truth is char and whose
//      picture is embers, which is exactly the report.
Status GateCorpseBurn(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  if (!mobs.BurnTablesReady()) {
    detail = "burn tables not loaded";
    return Status::Fail;
  }
  auto matId = [&](const char* n) -> uint32_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == n) return (uint32_t)i;
    return 0;
  };
  const uint32_t mFire = matId("fire"), mSmoke = matId("smoke"),
                 mAsh = matId("ash");
  const uint32_t alight[] = {matId("flesh_burning"), matId("cloth_burning"),
                             matId("linen_burning")};
  const uint32_t spent[] = {matId("flesh_charred"), matId("flesh_cinder"),
                            matId("cloth_charred"), matId("linen_charred")};
  if (!mFire || !alight[0] || !spent[0]) {
    detail = "fire / flesh_burning / flesh_charred missing from materials.json";
    return Status::Fail;
  }

  PrepareWorld(c);
  // THE CENTRE OF THE WINDOW, not an edge inset like the other fixtures: a
  // burning NPC runs (measured: 32 voxels along z in the 177 ticks it took to
  // die), and from an inset of 480 that carried it across the window edge,
  // where no chunk is fetched, no terrain mesh is built, and the corpse fell
  // 225 voxels into nothing. The other gates hold their creature in place.
  const int inset = (int)(kWorldN / 2);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, inset));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  const MobDef& def = mobs.Defs()[t.defIndex];
  const int rootLimb = def.rootLimb;
  IVec3 pchunk{};
  const uint64_t id = SpawnTarget(c, t, inset, pchunk);
  if (!id || rootLimb < 0) {
    detail = "spawn refused";
    return Status::Fail;
  }
  // HOLD THE CREATURE IN PLACE, structurally.
  //
  // The window-centre inset above was chosen to buy margin against exactly one
  // thing: "a burning NPC runs", 32 voxels of it while dying. Margin is not a
  // fix, it is a bet on how far the locomotion will carry a body — and the bet
  // was lost the moment NPCs got the player's step budget and a footprint
  // collider that slides along obstacles instead of stalling against them. The
  // corpse ended up two voxels past the window edge, where no chunk is fetched,
  // and the gate reported "0 alight / 0 spent, 0 fire ops" about a body falling
  // through unfetched space. Nothing about the wander is what this gate tests.
  //
  // `dummy` is the authored profile whose entire content is `mobile: false`, so
  // this pins the subject without adding a test-only code path to the mob
  // driver — the same profile `ai-dummy` uses to assert exactly zero motion.
  mobs.SetMobBehavior(id, "dummy");

  uint32_t tick = 71000;
  uint32_t debrisFireOps = 0, debrisResidueOps = 0;
  // One real tick. `soak` lights burn-cap's bonfire around the creature. Ops
  // the DEBRIS pass pushed are counted before the fixture adds its own, and
  // only the debris pass's — the mob's go with the mob.
  Vec3 pyre{};
  auto tickOnce = [&](bool soak) {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    mobs.PreTick(tick + 1, c.world, ops, cellOps, spawns);
    const size_t fromMobs = cellOps.size();
    c.debris.QueueSupportEvents(c.world.Snap());
    c.debris.PreTick(tick + 1, c.world, cellOps, spawns);
    for (size_t k = fromMobs; k < cellOps.size(); k++) {
      const uint32_t m = cellOps[k].word & 0xFFFu;
      if (m == mFire) debrisFireOps++;
      else if (m == mSmoke || m == mAsh) debrisResidueOps++;
    }
    if (soak) {
      if (mobs.IsAlive(id) && mobs.LimbBody(id, rootLimb))
        pyre = mobs.LimbVoxelPos(id, rootLimb, 0);
      const IVec3 b{ifloor(pyre.x), ifloor(pyre.y), ifloor(pyre.z)};
      for (int dy = -8; dy <= 20; dy++)
        for (int dz = -6; dz <= 6; dz++)
          for (int dx = -6; dx <= 6; dx++) {
            const IVec3 cc{b.x + dx, b.y + dy, b.z + dz};
            if (!c.world.CellInWindow(cc)) continue;
            if (cellOps.size() >= kMaxCellOpsPerTick) break;
            const bool aboveGround =
                cc.y > World::TerrainHeight(cc.x, cc.z, kDefaultSeed);
            cellOps.push_back(
                {World::SlotCellIndex(cc),
                 PackVoxNew(mFire, 7u) | (aboveGround ? 0u : kCellOpIfAir)});
          }
    }
    ++tick;
    SubmitTick(c.ctx, c.world, c.sim, tick, kDefaultSeed, ops, {}, cellOps,
               false, pchunk, true, false, spawns);
    c.ctx.WaitIdle();
    c.ctx.ProcessEvents();
    c.phys.Step(kTickDt);
    c.debris.PostStep();
    mobs.PostStep();
  };

  // ---- A. die in the fire -------------------------------------------------
  int deathTick = -1;
  for (int i = 0; i < 1200; i++) {
    tickOnce(true);
    if (!mobs.IsAlive(id)) {
      deathTick = i;
      break;
    }
  }
  const std::string cause = mobs.DeathCause(id);
  // The husk is swept on the next PreTick; the bodies are already debris.
  auto sumOf = [&](uint32_t bi, const uint32_t* mats, size_t n) {
    uint32_t s = 0;
    for (size_t k = 0; k < n; k++)
      if (mats[k]) s += c.debris.BodyMaterialCount(bi, mats[k]);
    return s;
  };
  auto alightOf = [&](uint32_t bi) { return sumOf(bi, alight, 3); };
  auto spentOf = [&](uint32_t bi) { return sumOf(bi, spent, 4); };
  // Pieces are tracked by HANDLE. A burn rebuild replaces the handle
  // (ReplaceBody) and a piece burnt below body-worthiness is swap-removed, so
  // neither an index nor a handle is stable across the window; a piece whose
  // handle is no longer present is matched by voxel count among the unclaimed
  // bodies, and one that matches nothing has finished burning (gone).
  struct Piece {
    uint64_t handle;
    uint32_t alight0, spent0, voxels0;
    Vec3 pos;
    uint32_t alight1 = 0, spent1 = 0;
    bool matched = false, gone = false;
  };
  std::vector<Piece> pieces;
  uint32_t alight0 = 0, spent0 = 0;
  for (uint32_t bi = 0; bi < c.debris.BodyCount(); bi++) {
    Piece p{c.debris.BodyHandle(bi), alightOf(bi), spentOf(bi),
            c.debris.BodyVoxelCount(bi), c.debris.BodyPosition(bi)};
    alight0 += p.alight0;
    spent0 += p.spent0;
    pieces.push_back(p);
  }
  const uint32_t bodies0 = c.debris.BodyCount();

  // ---- B + C. the corpse, alone with its own fire ---------------------------
  // WHERE THE BODIES WENT is recorded per tick (rule 6): "0 bodies at the
  // end" is a bare count, and a corpse that settled into the grid, one that
  // fell out of the window and one that burned away are three different
  // stories with three different fixes.
  const int window = 400;
  debrisFireOps = 0;
  debrisResidueOps = 0;
  const uint32_t settled0 = c.debris.SettledBack();
  int lastBodyTick = -1;
  float lastY = 0.0f, groundY = 0.0f;
  {
    const IVec3 b{ifloor(pyre.x), ifloor(pyre.y), ifloor(pyre.z)};
    groundY = (float)World::TerrainHeight(b.x, b.z, kDefaultSeed);
  }
  uint32_t bodiesMid = 0;
  // The corpse's ground, tick by tick for the first moments: lowest body y
  // and ManageTerrain's census of the chunks it wanted around the bodies.
  std::string fall;
  // ...and the column under the pyre as the chunk cache (the terrain mesh's
  // source) sees it at death: a corpse falls through water and through a
  // chunk nobody fetched, and those are different from falling through rock.
  {
    const IVec3 b{ifloor(pyre.x), ifloor(pyre.y), ifloor(pyre.z)};
    Vec3 mean{};
    for (uint32_t bi = 0; bi < c.debris.BodyCount(); bi++)
      mean += c.debris.BodyPosition(bi);
    if (c.debris.BodyCount()) mean = mean * (1.0f / (float)c.debris.BodyCount());
    const IVec3 org = c.world.WindowOrigin();
    fall += Format(" pyre (%.0f,%.0f,%.0f) bodies mean (%.0f,%.0f,%.0f) window "
                   "origin chunk (%d,%d,%d) pchunk (%d,%d,%d) pyre chunk in "
                   "window=%d cached=%d;",
                   pyre.x, pyre.y, pyre.z, mean.x, mean.y, mean.z, org.x, org.y,
                   org.z, pchunk.x, pchunk.y, pchunk.z,
                   c.world.ChunkInWindow(IVec3{b.x >> 4, b.y >> 4, b.z >> 4}) ? 1 : 0,
                   c.world.Cached(IVec3{b.x >> 4, b.y >> 4, b.z >> 4}) ? 1 : 0);
    fall += " column";
    for (int y = (int)groundY + 4; y >= (int)groundY - 3; y--) {
      const IVec3 cc{b.x, y, b.z};
      const CachedChunk* ch = c.world.Cached(IVec3{cc.x >> 4, cc.y >> 4, cc.z >> 4});
      if (!ch || ch->voxels.size() != kChunkVol) {
        fall += Format(" %d:?", y);
        continue;
      }
      const uint32_t m =
          ch->voxels[((cc.z & 15) * (int)kChunk + (cc.y & 15)) * (int)kChunk +
                     (cc.x & 15)] & 0xFFFu;
      fall += Format(" %d:%s", y, m < c.mats.size() ? c.mats[m].name.c_str() : "?");
    }
    fall += ";";
  }
  for (int i = 0; i < window; i++) {
    if (i < 12 || i == 30 || i == 60) {
      float lo = 1e9f;
      for (uint32_t bi = 0; bi < c.debris.BodyCount(); bi++)
        lo = std::min(lo, c.debris.BodyPosition(bi).y);
      uint32_t built = 0, unfetched = 0, empty = 0;
      c.debris.TerrainCensus(built, unfetched, empty);
      fall += Format(" t+%d:y%.1f/%ub%uu%ue", i, lo, built, unfetched, empty);
    }
    tickOnce(false);
    if (c.debris.BodyCount() > 0) {
      lastBodyTick = i;
      float lo = 1e9f;
      for (uint32_t bi = 0; bi < c.debris.BodyCount(); bi++)
        lo = std::min(lo, c.debris.BodyPosition(bi).y);
      lastY = lo;
    }
    if (i == window / 2) bodiesMid = c.debris.BodyCount();
  }
  const uint32_t settled = c.debris.SettledBack() - settled0;

  std::vector<int> claimed(c.debris.BodyCount(), 0);
  auto claim = [&](Piece& p, uint32_t bi) {
    claimed[bi] = 1;
    p.matched = true;
    p.alight1 = alightOf(bi);
    p.spent1 = spentOf(bi);
  };
  for (Piece& p : pieces)
    for (uint32_t bi = 0; bi < c.debris.BodyCount(); bi++)
      if (!claimed[bi] && c.debris.BodyHandle(bi) == p.handle) {
        claim(p, bi);
        break;
      }
  // Handle gone: the piece was rebuilt (new handle) or burnt away. Match the
  // nearest unclaimed body to where the piece lay; a still corpse does not
  // travel, so anything farther than a body length is not it.
  for (Piece& p : pieces) {
    if (p.matched) continue;
    int best = -1;
    float bestD = 6.0f;
    for (uint32_t bi = 0; bi < c.debris.BodyCount(); bi++) {
      if (claimed[bi]) continue;
      const float d = (c.debris.BodyPosition(bi) - p.pos).len();
      if (d < bestD) {
        bestD = d;
        best = (int)bi;
      }
    }
    if (best >= 0) claim(p, (uint32_t)best);
    else p.gone = true;
  }
  uint32_t alight1 = 0, spent1 = 0;
  for (uint32_t bi = 0; bi < c.debris.BodyCount(); bi++) {
    alight1 += alightOf(bi);
    spent1 += spentOf(bi);
  }
  // Every piece that died with a real ember count must have MOVED: fewer
  // alight or more spent. Not "retired half of it" -- a corpse with fire on
  // it burns THROUGH, cooked flesh under the char catching from the embers
  // beside it, so the alight count on a piece can hold or climb for hundreds
  // of ticks while the char under it grows; measured 6,155 -> 1,872 alight
  // and 3,090 -> 7,880 spent over the window, with 6,066 fire ops emitted.
  // What must never happen is a piece whose numbers do not change at all,
  // which is the starved body the report describes. 8 is the floor: a piece
  // with three embers on it can lose them all to the dice or keep them all,
  // and neither says anything.
  std::string stalled;
  uint32_t stalledCount = 0, litPieces = 0;
  for (const Piece& p : pieces) {
    if (p.alight0 < 8) continue;
    litPieces++;
    if (p.gone) continue;
    if (p.alight1 * 2 > p.alight0 && p.spent1 <= p.spent0) {
      stalledCount++;
      if (stalled.size() < 200)
        stalled += Format(" [%u vox: %u->%u alight, %u->%u spent]", p.voxels0,
                          p.alight0, p.alight1, p.spent0, p.spent1);
    }
  }
  // ...and the corpse as a whole is retiring fire, not growing it: fewer
  // alight than it died with and more char, thirteen seconds on with no fire
  // but its own. A corpse whose fire GREW over that window with nothing
  // feeding it would be a closed loop in the fire economy (rule 2).
  const bool advanced = alight0 > 0 && alight1 < alight0 &&
                        spent1 > spent0 && stalledCount == 0;
  const bool emitted = debrisFireOps > 0;

  // ---- D. the brick agrees with the lattice --------------------------------
  // Per surviving body: count the alight materials in the brick it is drawn
  // from and compare with the same count on its lattice. A body on the cube
  // path (no brick) has nothing to disagree with.
  uint32_t bricks = 0, disagree = 0;
  std::string disagreeDetail;
  if (const MicroBodySet* set = c.debris.MicroSet()) {
    for (uint32_t bi = 0; bi < c.debris.BodyCount(); bi++) {
      const uint32_t model = c.debris.BodyMicroModel(bi);
      if (model == kMicroBodyNoModel || model >= set->models.size()) continue;
      const MicroBodyModelGpu& m = set->models[model];
      const uint32_t dx = m.dims & 1023u, dy = (m.dims >> 10) & 1023u,
                     dz = (m.dims >> 20) & 1023u;
      const size_t count = (size_t)dx * dy * dz;
      uint32_t brickAlight = 0, brickSpent = 0;
      for (size_t idx = 0; idx < count; idx++) {
        const uint32_t w = m.base + (uint32_t)(idx / 2);
        if (w >= set->pool.size()) break;
        const uint32_t mat = (set->pool[w] >> ((idx % 2) * 16u)) & 0xFFu;
        if (!mat) continue;
        for (uint32_t a : alight)
          if (a && mat == a) brickAlight++;
        for (uint32_t s : spent)
          if (s && mat == s) brickSpent++;
      }
      bricks++;
      const uint32_t latAlight = alightOf(bi), latSpent = spentOf(bi);
      if (brickAlight != latAlight || brickSpent != latSpent) {
        disagree++;
        if (disagreeDetail.size() < 200)
          disagreeDetail +=
              Format(" [brick %u/%u vs lattice %u/%u alight/spent]",
                     brickAlight, brickSpent, latAlight, latSpent);
      }
    }
  }
  const bool drawn = disagree == 0;

  RecordObserved("corpseBurnDeathTick", (double)deathTick);
  RecordObserved("corpseBurnAlightAtDeath", (double)alight0);
  RecordObserved("corpseBurnAlightAtEnd", (double)alight1);
  RecordObserved("corpseBurnFireOps", (double)debrisFireOps);
  RecordObserved("corpseBurnStalledPieces", (double)stalledCount);

  const bool died = deathTick >= 0;
  const bool ok = died && alight0 > 0 && advanced && emitted && drawn;
  const uint32_t bodies1 = c.debris.BodyCount();
  mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  detail = Format(
      "%s: died at tick %d of '%s' with %u alight / %u spent across %u bodies; "
      "after %d ticks alone: %u alight / %u spent across %u bodies, %u of %u "
      "lit pieces stalled%s; debris emitted %u fire + %u smoke/ash ops; %u "
      "bricks, %u disagree with their lattice%s; %u settled back into the "
      "grid, %u bodies at t+%d, last body seen at t+%d with lowest y %.1f "
      "(ground %.0f); fall:%s",
      t.defName.c_str(), deathTick, cause.c_str(), alight0, spent0, bodies0,
      window, alight1, spent1, bodies1, stalledCount, litPieces,
      stalled.c_str(), debrisFireOps, debrisResidueOps, bricks, disagree,
      disagreeDetail.c_str(), settled, bodiesMid, window / 2, lastBodyTick,
      lastY, groundY, fall.c_str());
  std::printf("corpse-burn: %s\n", detail.c_str());
  std::fflush(stdout);
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

// ---------------------------------------------------------------------------
// body-stain: blood on a body (owner report 2026-09-13)
// ---------------------------------------------------------------------------
//
// Four claims, one fixture:
//   * A CUT BLOODIES WHAT IT EXPOSES, bone included. Before this, the soak was
//     a material rewrite that skipped bone on purpose, so a cut through bone
//     showed clean bone, and the blast/corpse paths did not soak at all.
//   * A BURST LANDS ON A BODY: a SplatterEvent replayed against a limb marks
//     the voxels its droplets reach (this is how killing something covers
//     you in it), and a bleeding wound queues such events by itself.
//   * A BLOOD POOL RUBS OFF: a limb standing in blood takes its stain from the
//     world through the CPU mirror, at the liquid's own authored rate.
//   * WATER WASHES IT: the same limb in water loses it again.
//   * ...AND ONLY WHERE IT TOUCHES (owner report 2026-09-13: "standing two
//     voxels deep in blood stains all the way up the legs"): an ankle-deep
//     pool marks nothing above its top plus the one-voxel contact dilation.
//   * A BURST THAT CANNOT ARRIVE MARKS NOTHING: the replay flies the visible
//     droplets' own gravity arc, so a drip's spray (3.5 vox/s) queued a metre
//     away leaves the body clean. Before this the replay marched a straight
//     line for speed x life and painted legs 80 cm above a wound.
// The pool phases submit real ticks (the mirror is fetched through the tick
// path) and regenerate the world on the way out, like wound-bleed.
Status GateBodyStain(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 330));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  uint32_t mStone = 0;
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == "stone") mStone = (uint32_t)i;
  // THE ROOM. A sealed stone box round the spawn site -- floor three deep
  // under it, walls and ceiling 2.4 m up, air inside -- written before the
  // creature exists, so every claim below stands on a flat floor at a known
  // height with nothing that can flow, sink or fall into it. The site is
  // FixtureSite's TerrainHeight+1, and TerrainHeight is NOT the surface: at
  // this inset the real ground was 2 voxels higher standalone and 13 higher in
  // the full suite (the window origin walked by `streaming` lands the site
  // under a hill), where the human climbed its 1.2 m escape cap and stayed
  // entombed with nothing round its feet but rock. Two earlier fixtures on
  // the open ground measured sand flowing into the pool and the body climbing
  // the mound it made.
  //
  // Written a few ticks AFTER PrepareWorld, not on the tick right after it:
  // SubmitWorldgen queues the authored edit layer and the paged fill in
  // batches that land over the next ticks, and a room written on the first
  // tick was generated straight over (the feet then stood at 215.5 on sand
  // over a floor meant to be at 207).
  const IVec3 site0 = FixtureSite(c.world, 330);
  const IVec3 roomChunk{site0.x >> 4, site0.y >> 4, site0.z >> 4};
  constexpr int kRoomHalf = 9, kRoomUp = 24;
  auto bareTick = [&](uint32_t tick, std::vector<CellOp>& cellOps) {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    SubmitTick(c.ctx, c.world, c.sim, tick, kDefaultSeed, ops, {}, cellOps, false,
               roomChunk, true, false, spawns);
    c.ctx.WaitIdle();
    c.ctx.ProcessEvents();
  };
  if (mStone) {
    {
      std::vector<CellOp> none;
      for (uint32_t k = 0; k < 6; k++) bareTick(28990u + k, none);
    }
    std::vector<CellOp> cellOps;
    for (int dz = -kRoomHalf; dz <= kRoomHalf; dz++)
      for (int dx = -kRoomHalf; dx <= kRoomHalf; dx++)
        for (int dy = -3; dy <= kRoomUp + 1; dy++) {
          const IVec3 cc{site0.x + dx, site0.y + dy, site0.z + dz};
          if (!c.world.CellInWindow(cc)) continue;
          if (cellOps.size() >= kMaxCellOpsPerTick) break;
          const bool shell = dy < 0 || dy > kRoomUp ||
                             std::max(std::abs(dx), std::abs(dz)) == kRoomHalf;
          cellOps.push_back({World::SlotCellIndex(cc),
                             shell ? PackVoxNew(mStone, 0u) : 0u});
        }
    bareTick(28997u, cellOps);
    std::vector<CellOp> none;
    bareTick(28998u, none);
  }
  IVec3 pchunk{};
  const uint64_t id = SpawnTarget(c, t, 330, pchunk);
  if (!id) {
    detail = "spawn refused";
    return Status::Fail;
  }
  // Pinned (the authored `dummy` profile, mobile: false): every claim below is
  // about blood on a body standing where the fixture put it, and the human's
  // default profile wanders -- the first shallow-pool fixture watched it walk
  // up the dune beside the pool and reported the climb as a stain.
  const bool pinned = mobs.SetMobBehavior(id, "dummy");
  uint32_t mBone = 0, mBlood = 0, mWater = 0;
  for (size_t i = 0; i < c.mats.size(); i++) {
    if (c.mats[i].name == "bone") mBone = (uint32_t)i;
    if (c.mats[i].name == "blood") mBlood = (uint32_t)i;
    if (c.mats[i].name == "water") mWater = (uint32_t)i;
  }
  const MobDef& def = mobs.Defs()[t.defIndex];
  const int root = def.rootLimb;
  const uint32_t bloodType = mobs.StainTypeOf(mBlood);

  // One real tick with liquid written round the creature: PreTick, the box,
  // the sim, the physics. Shared by the shallow pool and the two floods. The
  // CPU mirror (what the contact pass reads) is the 3x3x3 chunks round the
  // "player" chunk handed to SubmitTick: that is the CREATURE's chunk here,
  // not the spawn's -- the spawn site is TerrainHeight+1 and the body climbs
  // out to the real surface, which in the full suite (window origin walked
  // by `streaming`) was a chunk and a half higher, so a mirror centred on
  // the spawn never held the cells round the feet and the pass saw nothing.
  uint32_t simTick = 29000;
  auto liquidTick = [&](const std::function<void(std::vector<CellOp>&)>& fill) {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    mobs.PreTick(simTick + 1, c.world, ops, cellOps, spawns);
    fill(cellOps);
    c.debris.QueueSupportEvents(c.world.Snap());
    c.debris.PreTick(simTick + 1, c.world, cellOps, spawns);
    ++simTick;
    IVec3 centre = pchunk;
    if (root >= 0 && mobs.LimbBody(id, root)) {
      const Vec3 at = mobs.LimbVoxelPos(id, root, 0);
      centre = IVec3{ifloor(at.x) >> 4, ifloor(at.y) >> 4, ifloor(at.z) >> 4};
    }
    SubmitTick(c.ctx, c.world, c.sim, simTick, kDefaultSeed, ops, {}, cellOps,
               false, centre, true, false, spawns);
    c.ctx.WaitIdle();
    c.ctx.ProcessEvents();
    c.phys.Step(kTickDt);
    c.debris.PostStep();
    mobs.PostStep();
  };

  // ---- 0. the shallow pool, on a body that is not yet bleeding ------------
  // Two voxels of blood in the two cells the creature's FEET occupy, for 30
  // ticks. The claim is the CEILING: no stained voxel of any limb above the
  // pool's top plus the contact pass's one-voxel dilation. Measured through
  // the live pose (LimbStainWorldYRange), so it does not care which limb the
  // rig calls a foot. Before the cut so nothing sprays during it.
  //
  // The pool is placed off the BODY (the lowest limb voxel through the live
  // pose), on the room's floor, and only into cells the CPU mirror shows as
  // air or blood.
  float shallowRise = -1e30f;
  uint32_t shallowCount = 0, unmirrored = 0;
  std::string shallowLimb = "-";
  int poolTop = INT32_MIN;
  // Let the creature find the floor and stop before the pool is placed.
  auto lowestFoot = [&]() -> float {
    float best = 1e30f;
    for (size_t li = 0; li < def.limbs.size(); li++) {
      float lo = 0.0f, hi = 0.0f;
      if (mobs.LimbBody(id, (int)li) && mobs.LimbStainWorldYRange(id, (int)li, 0, lo, hi))
        best = std::min(best, lo);
    }
    return best;
  };
  for (int i = 0; i < 20; i++) liquidTick([](std::vector<CellOp>&) {});
  const float feetBefore = lowestFoot();
  float feetAfter = feetBefore;
  const int footY = feetBefore < 1e29f ? ifloor(feetBefore) : INT32_MAX;
  auto mirrorMat = [&](IVec3 cc) -> int {
    const CachedChunk* k = c.world.Cached(IVec3{cc.x >> 4, cc.y >> 4, cc.z >> 4});
    if (!k || k->voxels.size() != kChunkVol) return -1;  // not mirrored yet
    return (int)(k->voxels[(((uint32_t)cc.z & 15u) * kChunk + ((uint32_t)cc.y & 15u)) * kChunk +
                           ((uint32_t)cc.x & 15u)] & 0xFFFu);
  };
  const int floorMat = mirrorMat({site0.x, site0.y - 1, site0.z});
  const std::string floorName =
      floorMat < 0 ? "unmirrored" : floorMat == 0 ? "AIR" : c.mats[(size_t)floorMat].name;
  if (root >= 0 && mBlood && footY != INT32_MAX && mobs.LimbBody(id, root)) {
    const Vec3 at = mobs.LimbVoxelPos(id, root, 0);
    const int gx = ifloor(at.x), gz = ifloor(at.z);
    poolTop = footY + 2;  // cells footY and footY+1; top face at +2
    for (int i = 0; i < 30; i++) {
      liquidTick([&](std::vector<CellOp>& cellOps) {
        for (int dz = -5; dz <= 5; dz++)
          for (int dx = -5; dx <= 5; dx++)
            for (int dy = 0; dy <= 1; dy++) {
              const IVec3 cc{gx + dx, footY + dy, gz + dz};
              if (!c.world.CellInWindow(cc)) continue;
              if (cellOps.size() >= kMaxCellOpsPerTick) return;
              const int m = mirrorMat(cc);
              if (m < 0) unmirrored++;
              if (m != 0 && m != (int)mBlood) continue;  // ground, or not yet seen
              cellOps.push_back({World::SlotCellIndex(cc), PackVoxNew(mBlood, 8u)});
            }
      });
    }
    feetAfter = lowestFoot();
    for (size_t li = 0; li < def.limbs.size(); li++) {
      if (!mobs.LimbBody(id, (int)li)) continue;
      shallowCount += mobs.LimbStainCount(id, (int)li, 1);
      const float top = mobs.LimbStainMaxWorldY(id, (int)li, 1);
      if (top > -1e29f && top - (float)poolTop > shallowRise) {
        shallowRise = top - (float)poolTop;
        shallowLimb = def.limbs[li].name;
      }
    }
  }
  const double shallowMaxRise = BaselineNumber("bodyStainShallowMaxRise", 1.5);
  // A body that moved while the pool was written makes the ceiling meaningless
  // (the first fixture had the creature climbing out of the ground under it).
  const bool feetStill = std::fabs(feetAfter - feetBefore) <= 0.5f;
  const bool shallowOk = shallowCount > 0 && feetStill && shallowRise <= (float)shallowMaxRise;

  // ---- 0b. a burst that cannot arrive -------------------------------------
  // A drip's spray -- 3.5 vox/s, thrown upward -- queued a metre in front of
  // the root limb. Nothing that slow gets there (it rises a tenth of a voxel),
  // so the root must stay exactly as the pool left it.
  uint32_t unreachableMarks = 0;
  if (root >= 0 && mobs.LimbBody(id, root)) {
    const uint32_t before = mobs.LimbStainCount(id, root, 1);
    const Vec3 at = mobs.LimbVoxelPos(id, root, 0);
    SplatterEvent ev;
    ev.origin = at + Vec3{MetresToCells(1.0f), 0.0f, 0.0f};
    ev.axis = Vec3{0.0f, 1.0f, 0.0f};
    ev.cone = 0.55f;
    ev.reach = MetresToCells(4.0f);
    ev.speed = 3.5f;
    ev.life = 70;
    ev.count = 24;
    ev.type = bloodType;
    ev.amount = 6;
    ev.tick = 29900u;
    ev.seed = 0xD1E7u;
    mobs.QueueSplatter(ev);
    for (int i = 0; i < 2; i++) {
      std::vector<BrushOp> ops;
      std::vector<ParticleSpawn> spawns;
      std::vector<CellOp> cellOps;
      mobs.PreTick(29901u + (uint32_t)i, c.world, ops, cellOps, spawns);
      c.phys.Step(kTickDt);
      mobs.PostStep();
    }
    const uint32_t after = mobs.LimbStainCount(id, root, 1);
    unreachableMarks = after > before ? after - before : 0u;
  }
  const bool unreachableOk = unreachableMarks == 0;

  // ---- 1. the cut ---------------------------------------------------------
  const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
  const uint32_t stainBefore = mobs.LimbStainCount(id, t.limb, 1);
  bool hit = false;
  {
    std::vector<ParticleSpawn> spawns;
    // Deep (power 0.9, wound-bleed's), so the kerf reaches the bone core and
    // the bone claim below is measured against something.
    hit = CutOnce(mobs, c.world, id, t.limb, ax, ax.reach * 0.5f, 0.9f, 1.0f,
                  0x57A1u, spawns);
  }
  const bool attached = mobs.LimbBody(id, t.limb) != 0;
  const uint32_t stainCut = attached ? mobs.LimbStainCount(id, t.limb, 1) : 0;
  const uint32_t boneStained =
      attached ? mobs.LimbStainedMatCount(id, t.limb, mBone, 1) : 0;
  // How much bone the cut EXPOSED: bone voxels of the lattice with an empty
  // 6-neighbour. The claim is "every exposed bone is bloodied to some
  // degree", asserted as a fraction so a single rim voxel cannot fail it.
  uint32_t boneExposed = 0;
  if (attached) {
    const std::vector<PrefabVoxel> lat = mobs.LimbLattice(id, t.limb);
    std::unordered_set<uint64_t> occ;
    auto key = [](int x, int y, int z) {
      return ((uint64_t)(uint32_t)(x + 32768) << 34) |
             ((uint64_t)(uint32_t)(y + 32768) << 17) | (uint64_t)(uint32_t)(z + 32768);
    };
    for (const PrefabVoxel& v : lat)
      if ((v.material & 0xFFFu) != 0) occ.insert(key(v.x, v.y, v.z));
    for (const PrefabVoxel& v : lat) {
      if ((v.material & 0xFFFu) != mBone) continue;
      if (!occ.count(key(v.x - 1, v.y, v.z)) || !occ.count(key(v.x + 1, v.y, v.z)) ||
          !occ.count(key(v.x, v.y - 1, v.z)) || !occ.count(key(v.x, v.y + 1, v.z)) ||
          !occ.count(key(v.x, v.y, v.z - 1)) || !occ.count(key(v.x, v.y, v.z + 1)))
        boneExposed++;
    }
  }
  const double boneMinFrac = BaselineNumber("bodyStainBoneMinFraction", 0.5);
  const bool cutOk = hit && attached && stainCut > stainBefore;
  const bool boneOk =
      boneExposed == 0 || (double)boneStained >= boneMinFrac * (double)boneExposed;

  // ---- 2. the burst -------------------------------------------------------
  // A splash aimed at the root limb from a metre in front of it, replayed by
  // the next PreTick. Deterministic on purpose: the wound's own gout points
  // wherever the joint does, and the claim here is the landing, not the aim.
  // Thrown at 6 m/s: the replay flies the real arc, and over a metre that
  // drops 11 cm, inside the root's bounding sphere. (3 m/s drops 43 cm and
  // would hit the thighs, which is the point of the arc.)
  uint32_t rootBefore = 0, rootSplashed = 0;
  size_t queuedByWound = 0;
  if (root >= 0 && mobs.LimbBody(id, root)) {
    rootBefore = mobs.LimbStainCount(id, root, 1);
    const Vec3 at = mobs.LimbVoxelPos(id, root, 0);
    SplatterEvent ev;
    ev.origin = at + Vec3{MetresToCells(1.0f), 0.0f, 0.0f};
    ev.axis = Vec3{-1.0f, 0.0f, 0.0f};
    ev.cone = 0.35f;
    ev.reach = MetresToCells(2.0f);
    ev.speed = MetresToCells(6.0f);
    ev.life = 70;
    ev.count = 24;
    ev.type = bloodType;
    ev.amount = 6;
    ev.tick = 30000u;
    ev.seed = 0x5B1A7u;
    mobs.QueueSplatter(ev);
    for (int i = 0; i < 6; i++) {
      std::vector<BrushOp> ops;
      std::vector<ParticleSpawn> spawns;
      std::vector<CellOp> cellOps;
      mobs.PreTick(30001u + (uint32_t)i, c.world, ops, cellOps, spawns);
      // The chip above left a bleeding wound; its drip spray must queue
      // bursts of its own (counted before the age-out at the next PreTick).
      queuedByWound = std::max(queuedByWound, mobs.SplatterEventsQueued());
      c.phys.Step(kTickDt);
      mobs.PostStep();
    }
    rootSplashed = mobs.LimbBody(id, root) ? mobs.LimbStainCount(id, root, 1) : 0;
  }
  const bool splashOk = rootSplashed > rootBefore;
  const bool woundQueues = queuedByWound > 0;

  // ---- 3. the pool, then the river -----------------------------------------
  // Real ticks: a box of blood is written round the root limb every tick and
  // the mirror is fetched through the tick path; then the same box as water.
  uint32_t pooled = 0, washed = 0;
  simTick = 31000;
  auto soakPhase = [&](uint32_t mat, uint32_t state, int ticks) -> uint32_t {
    for (int i = 0; i < ticks; i++) {
      liquidTick([&](std::vector<CellOp>& cellOps) {
        if (!mobs.LimbBody(id, root)) return;
        const Vec3 at = mobs.LimbVoxelPos(id, root, 0);
        const IVec3 b{ifloor(at.x), ifloor(at.y), ifloor(at.z)};
        for (int dy = -3; dy <= 3; dy++)
          for (int dz = -3; dz <= 3; dz++)
            for (int dx = -3; dx <= 3; dx++) {
              const IVec3 cc{b.x + dx, b.y + dy, b.z + dz};
              if (!c.world.CellInWindow(cc)) continue;
              if (cellOps.size() >= kMaxCellOpsPerTick) return;
              if (cc.y <= World::TerrainHeight(cc.x, cc.z, kDefaultSeed)) continue;
              cellOps.push_back({World::SlotCellIndex(cc), PackVoxNew(mat, state)});
            }
      });
    }
    return mobs.LimbBody(id, root) ? mobs.LimbStainCount(id, root, 1) : 0;
  };
  if (root >= 0 && mBlood && mWater && mobs.LimbBody(id, root)) {
    pooled = soakPhase(mBlood, 8u, 30);
    washed = soakPhase(mWater, 8u, 60);
  }
  const bool poolOk = pooled > rootSplashed;
  const double washFrac = BaselineNumber("bodyStainWashMaxFraction", 0.5);
  const bool washOk = pooled > 0 && (double)washed <= washFrac * (double)pooled;

  RecordObserved("bodyStainCut", (double)stainCut);
  RecordObserved("bodyStainBoneExposed", (double)boneExposed);
  RecordObserved("bodyStainBoneStained", (double)boneStained);
  RecordObserved("bodyStainSplashed", (double)(rootSplashed - rootBefore));
  RecordObserved("bodyStainPooled", (double)pooled);
  RecordObserved("bodyStainWashed", (double)washed);
  RecordObserved("bodyStainShallowRise", (double)shallowRise);
  RecordObserved("bodyStainShallowCount", (double)shallowCount);
  RecordObserved("bodyStainUnreachable", (double)unreachableMarks);

  mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  const bool ok = cutOk && boneOk && splashOk && woundQueues && poolOk && washOk &&
                  shallowOk && unreachableOk;
  detail = Format(
      "%s/%s%s: shallow pool stained %u voxels, highest %.2f vox above the pool "
      "(%s, cap %.1f; feet %.2f -> %.2f, spawn y %d, room floor %s, %u pool cells "
      "unmirrored); "
      "slow burst marked %u (need 0); cut "
      "stained %u -> %u (bone %u of %u exposed, need %.0f%%); burst on %s %u -> "
      "%u, wound queued %zu bursts; pool %u -> water %u (cap %.0f%%)",
      t.defName.c_str(), t.limbName.c_str(), pinned ? "" : " (NOT pinned)",
      shallowCount, shallowRise, shallowLimb.c_str(), shallowMaxRise, feetBefore,
      feetAfter, site0.y, floorName.c_str(), unmirrored, unreachableMarks,
      stainBefore, stainCut,
      boneStained, boneExposed, boneMinFrac * 100.0, root >= 0 ? "root" : "-",
      rootBefore, rootSplashed, queuedByWound, pooled, washed, washFrac * 100.0);
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// blast-stain: a crater bloodies the crater (owner report 2026-09-13)
// ---------------------------------------------------------------------------
//
// "Explosions that cause minor damage to the player cause way too much random
// noisily spread blood spatter on the character. It should be more
// concentrated ... and concentrated at areas where actual voxels are removed."
//
// The cause was the blast path asking StainWound to soak the BLAST SPHERE:
// `StainWound(cBody, radiusVoxels + 0.5)`. Two things follow from that and
// both are wrong. CarveRadialAll calls the carve for EVERY limb whose
// bounding sphere is within `radius + r + 2`, so a limb the blast never took a
// voxel from was still soaked; and the sphere is the volume the crater
// predicate SEARCHED, not the hole it made, so a graze that chipped a dozen
// voxels off an arm repainted every exposed voxel within the whole blast
// radius -- and the outer skin is exposed BY DEFINITION, so at
// woundStainSurface 0.9 that is most of the limb's visible surface.
//
// Two claims, one graze:
//   * NO BLOOD ON A LIMB THE BLAST DID NOT TOUCH. A limb that lost no voxels
//     gains no soak and no stain. This is the half that put blood on the far
//     arm of a figure clipped on the near one.
//   * THE SOAK IS THE SIZE OF THE HOLE. On the limb that WAS hit, the
//     rewritten fraction of the lattice is bounded -- a chip's worth of
//     blood for a chip's worth of damage, not a red limb.
// The blast is deliberately a graze, placed OUTSIDE the body and clipping one
// limb, because that is the case the report is about: a direct hit makes a big
// crater and should make a big mess.
Status GateBlastStain(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 360));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  IVec3 pchunk{};
  const uint64_t id = SpawnTarget(c, t, 360, pchunk);
  if (!id) {
    detail = "spawn refused";
    return Status::Fail;
  }
  const MobDef& def = mobs.Defs()[t.defIndex];
  const uint32_t woundMat = def.woundMat;
  const int root = def.rootLimb;
  const int nl = (int)def.limbs.size();

  // Outward from the body, so the blast CLIPS the target limb instead of
  // engulfing it. `LimbVoxelPos` names a surviving voxel, not the centroid,
  // which is what makes "a voxel of this limb" a point that is actually flesh.
  const Vec3 limbAt = mobs.LimbVoxelPos(id, t.limb, 0);
  Vec3 out = root >= 0 && mobs.LimbBody(id, root)
                 ? limbAt - mobs.LimbVoxelPos(id, root, 0)
                 : Vec3{1, 0, 0};
  out.y = 0.0f;
  const float outLen = out.len();
  out = outLen > 1e-3f ? out * (1.0f / outLen) : Vec3{1, 0, 0};
  const float radius = 5.0f;  // world voxels; a grenade beside a shoulder
  const Vec3 blastAt = limbAt + out * (radius * 0.85f);

  std::vector<uint32_t> artBefore(nl, 0), stainBefore(nl, 0), soakBefore(nl, 0);
  for (int li = 0; li < nl; li++) {
    if (!mobs.LimbBody(id, li)) continue;
    artBefore[li] = mobs.LimbArtVoxelCount(id, li);
    stainBefore[li] = mobs.LimbStainCount(id, li, 1);
    soakBefore[li] = woundMat ? mobs.LimbMaterialCount(id, li, woundMat) : 0u;
  }

  {
    std::vector<ParticleSpawn> spawns;
    mobs.CarveMobsRadial(blastAt, radius, c.world, spawns);
  }

  // A limb that is GONE is not evidence either way (it lost everything), and
  // one that lost voxels is allowed its blood. The bug is the third case.
  uint32_t carved = 0, ghostSoaked = 0, ghostStained = 0, searched = 0;
  std::string ghostName = "-";
  uint32_t hitLost = 0, hitSoaked = 0, hitStained = 0, hitArt = 0;
  std::string hitName = "-";
  for (int li = 0; li < nl; li++) {
    if (!artBefore[li]) continue;
    searched++;
    if (!mobs.LimbBody(id, li)) {  // severed by the blast
      carved++;
      continue;
    }
    const uint32_t art = mobs.LimbArtVoxelCount(id, li);
    const uint32_t soak = woundMat ? mobs.LimbMaterialCount(id, li, woundMat) : 0u;
    const uint32_t stain = mobs.LimbStainCount(id, li, 1);
    const uint32_t lost = artBefore[li] > art ? artBefore[li] - art : 0u;
    if (lost) {
      carved++;
      if (lost > hitLost) {
        hitLost = lost;
        hitArt = artBefore[li];
        hitName = def.limbs[li].name;
        hitSoaked = soak > soakBefore[li] ? soak - soakBefore[li] : 0u;
        hitStained = stain > stainBefore[li] ? stain - stainBefore[li] : 0u;
      }
      continue;
    }
    if (soak > soakBefore[li] || stain > stainBefore[li]) {
      if (soak > soakBefore[li]) ghostSoaked++;
      if (stain > stainBefore[li]) ghostStained++;
      if (ghostName == "-") ghostName = def.limbs[li].name;
    }
  }

  // THE SOAK ON THE LIMB THAT WAS HIT, AS A FRACTION OF THE LIMB. The blast
  // took `hitLost` of `hitArt` voxels off it — a graze, a percent or two — and
  // the blood it left has to be of that order and not of the order of the
  // limb. The old behaviour soaked a ball the size of the BLAST, so a scratch
  // came back with most of the limb's visible surface rewritten to blood;
  // that is the number this bounds. Stated as a MULTIPLE of the damage
  // fraction, not an absolute, so it reads the same on any rig: blood may
  // cover several times what the blast removed (a wound is bigger than its
  // hole) but not tens of times.
  const double lostFrac = hitArt ? (double)hitLost / (double)hitArt : 0.0;
  const double soakFrac = hitArt ? (double)hitSoaked / (double)hitArt : 0.0;
  const double spreadRatio = lostFrac > 0.0 ? soakFrac / lostFrac : 0.0;
  const double spreadMax = BaselineNumber("blastStainSpreadMax", 8.0);

  RecordObserved("blastStainCarved", (double)carved);
  RecordObserved("blastStainGhostSoaked", (double)ghostSoaked);
  RecordObserved("blastStainGhostStained", (double)ghostStained);
  RecordObserved("blastStainLostFraction", lostFrac);
  RecordObserved("blastStainSoakFraction", soakFrac);
  RecordObserved("blastStainSpread", spreadRatio);

  // ---- and the other direction: A BITE STILL BLEEDS -----------------------
  // The rule above is a CAP, and a cap alone is satisfied by making craters
  // bloodless — which is what the first attempt at the rim did (`0 rewritten`
  // at a blotch size larger than the wound). So: a second blast, centred ON
  // the body this time, has to come back with blood of the order of the hole
  // it made. Measured against what it REMOVED, not against the limb, because
  // a bite takes a tenth of a leg and should read as a bloody bite.
  uint32_t biteLost = 0, biteSoaked = 0, biteStained = 0;
  if (root >= 0 && mobs.LimbBody(id, root)) {
    const uint32_t artWas = mobs.LimbArtVoxelCount(id, root);
    const uint32_t soakWas = woundMat ? mobs.LimbMaterialCount(id, root, woundMat) : 0u;
    const uint32_t stainWas = mobs.LimbStainCount(id, root, 1);
    const Vec3 on = mobs.LimbVoxelPos(id, root, 11u);
    {
      std::vector<ParticleSpawn> spawns;
      mobs.CarveMobsRadial(on, 2.5f, c.world, spawns);
    }
    if (mobs.LimbBody(id, root)) {
      const uint32_t art = mobs.LimbArtVoxelCount(id, root);
      biteLost = artWas > art ? artWas - art : 0u;
      const uint32_t soak = woundMat ? mobs.LimbMaterialCount(id, root, woundMat) : 0u;
      const uint32_t stain = mobs.LimbStainCount(id, root, 1);
      biteSoaked = soak > soakWas ? soak - soakWas : 0u;
      biteStained = stain > stainWas ? stain - stainWas : 0u;
    }
  }
  const double biteFloor = BaselineNumber("blastStainBiteStainMin", 0.5);
  const bool biteOk =
      biteLost == 0 ||
      (double)(biteSoaked + biteStained) >= biteFloor * (double)biteLost;
  RecordObserved("blastStainBiteLost", (double)biteLost);
  RecordObserved("blastStainBiteBlood", (double)(biteSoaked + biteStained));

  const bool hitOk = carved > 0 && hitLost > 0 && hitArt > 0;
  const bool bloodOk = hitSoaked > 0 || hitStained > 0;
  const bool ghostOk = ghostSoaked == 0 && ghostStained == 0;
  const bool sizedOk = spreadRatio <= spreadMax;
  const bool ok = hitOk && bloodOk && ghostOk && sizedOk && biteOk;
  detail = Format(
      "%s: graze r=%.1f beside %s hit %u of %u limbs; %s lost %u of %u voxels "
      "(%.1f%%) and came back %u rewritten + %u stained (%.1f%% of the limb "
      "soaked, %.1fx the damage, cap %.1fx); %u limbs soaked / %u stained with "
      "NOTHING removed (first %s, need 0); a bite of %u took %u rewritten + %u "
      "stained (floor %.1f per voxel lost)",
      t.defName.c_str(), radius, t.limbName.c_str(), carved, searched,
      hitName.c_str(), hitLost, hitArt, lostFrac * 100.0, hitSoaked, hitStained,
      soakFrac * 100.0, spreadRatio, spreadMax, ghostSoaked, ghostStained,
      ghostName.c_str(), biteLost, biteSoaked, biteStained, biteFloor);
  mobs.Reset();
  c.debris.Reset();
  return ok ? Status::Pass : Status::Fail;
}

const std::vector<Gate>& WoundGates() {
  static const std::vector<Gate> g = {
      {"wound-chip", "mob", {}, false, GateWoundChip, /*needsRender=*/false},
      {"wound-accumulate", "mob", {}, false, GateWoundAccumulate, false},
      {"wound-heft", "mob", {}, false, GateWoundHeft, false},
      {"wound-bleed", "mob", {}, false, GateWoundBleed, false},
      {"bleed-out", "mob", {}, false, GateBleedOut, false},
      {"burn-cap", "mob", {}, false, GateBurnCap, false},
      {"one-hit", "mob", {}, false, GateOneHit, false},
      {"corpse-intact", "mob", {}, false, GateCorpseIntact, false},
      {"corpse-armor", "mob", {}, false, GateCorpseArmor, false},
      {"corpse-bleed", "mob", {}, false, GateCorpseBleed, false},
      {"body-stain", "mob", {}, false, GateBodyStain, false},
      {"blast-stain", "mob", {}, false, GateBlastStain, false},
      {"corpse-burn", "mob", {}, false, GateCorpseBurn, false},
  };
  return g;
}

}  // namespace selftest
