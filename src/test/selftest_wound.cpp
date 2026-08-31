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
#include <cstdio>
#include <string>
#include <vector>

#include "game/item.h"
#include "game/mob.h"
#include "sim/tuning.h"
#include "test/selftest.h"
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

// LEAVE THE SUITE'S RANDOMNESS AS WE FOUND IT.
//
// These gates spawn a lot of creatures — ChooseTarget tries every def, and each
// gate stands up its own fixture — and a mob id seeds id-keyed draws all over
// the engine (gore profile, crater noise, the burn/dissolve RNG key). Gates
// share one MobSystem, so spawning here re-rolls every gate after us.
//
// It is not hypothetical: the first full run with these four in kOrder failed
// `armor-react`, whose acid bath found the BARE arm losing 0 skin voxels in 120
// ticks where it had lost 18. Nothing about acid, armour or occlusion had
// changed — the creature simply had a different id and therefore a different
// draw. That gate's own comment already records the same numbers swinging by an
// order of magnitude between `--gate` and `--suite` scope for the same reason.
//
// Restoring the counter is the honest fix. MobSystem::Reset(rewindIds=true) is
// not: setting it to 1 is a different perturbation rather than no perturbation,
// and mob.cpp records a gate that moved when somebody tried exactly that.
struct IdCounterScope {
  MobSystem& sys;
  uint64_t saved;
  explicit IdCounterScope(MobSystem& s) : sys(s), saved(s.NextIdCounter()) {}
  ~IdCounterScope() { sys.SetNextIdCounter(saved); }
};

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

}  // namespace

const std::vector<Gate>& WoundGates() {
  static const std::vector<Gate> g = {
      {"wound-chip", "mob", {}, false, GateWoundChip, /*needsRender=*/false},
      {"wound-accumulate", "mob", {}, false, GateWoundAccumulate, false},
      {"wound-heft", "mob", {}, false, GateWoundHeft, false},
      {"wound-bleed", "mob", {}, false, GateWoundBleed, false},
  };
  return g;
}

}  // namespace selftest
