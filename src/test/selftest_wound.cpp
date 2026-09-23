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
// neither instant nor asymptotic. The HEFT it hacks with is a baseline row too
// (woundAccumHeft): since the 2026-09-19 gore retune a heft-1.0 kerf is
// narrower than the thigh and only grooves it, see GateWoundAccumulate.
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
#include "game/melee.h"  // hit-drive swings the real sweep
#include "game/anim.h"   // QuatRotate: a joint anchor is body-local
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
  const uint32_t tintBefore = mobs.LimbStainCount(id, t.limb, 1);

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
  const uint32_t tintAfter =
      mobs.LimbBody(id, t.limb) ? mobs.LimbStainCount(id, t.limb, 1) : 0;
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

  // BLOOD ON THE WOUND IS TWO MECHANISMS WITH TWO KNOBS (2026-09-19). The
  // SOAK is a rewrite -- exposed tissue within gore.woundStainRadius of the
  // kerf BECOMES the wound material (Mob::StainWoundAs) -- and the SMEAR is a
  // tint: stain bits laid over everything within gore.stainCutRadius
  // (bodystain.h SoakCut), bone included. This gate used to read only the
  // rewrite. The owner's retune took woundStainRadius 0.9 -> 0.1 world voxels,
  // which at skinScale 8 is under one skin cell, so an ordinary cut now
  // rewrites nothing and "0 stained" is the tuning speaking, not a regression;
  // stainCutRadius went 1.6 -> 0.4 and still tints the kerf's walls (body-coat
  // measures 43 tinted voxels from one deeper cut on the same limb).
  //
  // So each half is asserted only while its OWN radius can reach a cell, read
  // off the live Tuning rather than a constant: turn the soak back up and the
  // rewrite claim re-arms by itself. The arming radii are baseline rows. If
  // the owner turns BOTH under a cell the cut is bloodless by design and the
  // detail line says so instead of failing on it.
  const auto& g = CurrentTuning().gore;
  const float soakArm = (float)BaselineNumber("woundChipSoakArmRadius", 0.5);
  const float tintArm = (float)BaselineNumber("woundChipTintArmRadius", 0.25);
  const bool wantSoak = g.woundStainRadius >= soakArm;
  const bool wantTint = g.stainCutRadius >= tintArm;
  const bool soaked = stainAfter > stainBefore;
  const bool tinted = tintAfter > tintBefore;
  const bool stained = (!wantSoak || soaked) && (!wantTint || tinted);

  RecordObserved("woundChipLostFraction", (double)frac);
  RecordObserved("woundChipStained", (double)(stainAfter - stainBefore));
  RecordObserved("woundChipTinted", (double)(tintAfter - tintBefore));

  const bool ok = hit && sized && stained && attached && noSever && alive;
  detail = Format(
      "%s/%s: %u -> %u voxels (%.1f%% of the limb, cap %.0f%%), %u soaked "
      "(rewrite %s at woundStainRadius %.2f, arms at %.2f), %u tinted (smear "
      "%s at stainCutRadius %.2f, arms at %.2f), limb attached=%d severs=%zu "
      "alive=%d",
      t.defName.c_str(), t.limbName.c_str(), before, after, frac * 100.0f,
      maxFrac * 100.0, stainAfter - stainBefore, wantSoak ? "ASSERTED" : "off",
      (double)g.woundStainRadius, (double)soakArm, tintAfter - tintBefore,
      wantTint ? "ASSERTED" : "off", (double)g.stainCutRadius, (double)tintArm,
      attached ? 1 : 0, mobs.SeverEvents().size(), alive ? 1 : 0);
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
  // INSET 230, THE SAME SITE AS wound-heft (2026-09-19). This gate stood at
  // 200 and was red at suite scope for months on a site-specific cause nobody
  // ran down (the old _woundAccumulate_about in baseline.json: heft severed
  // the same limb in 5 at 230 while this never did at 200). Now that the run
  // below is the SAME run as heft's heavy arm -- same limb, same heft, same
  // seeds, same cross-section -- it is measured where that arm was measured,
  // and a number this gate reports is one the other gate can be checked
  // against. Nothing is shared between the two: each regenerates the world.
  constexpr int kInset = 230;
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, kInset));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  const int kMin = (int)BaselineNumber("woundAccumMinHits", 3);
  const int kMax = (int)BaselineNumber("woundAccumMaxHits", 40);
  // THE HEFT IS A BASELINE ROW, NOT 1.0 (2026-09-19). The owner's retune
  // (b48fb4d: gore.cutLength 0.9 -> 0.64, cutDepthPower 0.32 -> 0.15) made a
  // heft-1.0 kerf at power 0.75 only 0.54 world voxels along the edge, and
  // the human thigh is ~0.75 across: the slot advances THROUGH the limb but
  // never spans it, so it saws a groove with a strip of flesh left on either
  // side and neither structural rule in CarveLimb ever sees a disconnection
  // (46 of 46 hits at heft 1.0 with the limb still on; heft 2.5 parts it in
  // 13). That is the game as tuned, not a fault in the machinery this gate
  // exists to exercise -- the claim is that SUSTAINED cuts sever THROUGH
  // Sever(), and it is made with a kerf that can reach the far side. The
  // sword's own heft is still measured against a heavier one by wound-heft.
  const float heft = (float)BaselineNumber("woundAccumHeft", 2.5);
  // The cap is deliberately well past kMax: "never severed" and "severed on
  // the 30th" are different failures and a cap at kMax would report them
  // identically.
  const CutRun r = HackThrough(c, t, kInset, heft, kMax * 3 + 4);

  RecordObserved("woundAccumHits", (double)r.hits);
  const bool band = r.severed && r.hits >= kMin && r.hits <= kMax;
  // ...AND IT WENT THROUGH THE ORDINARY MACHINERY. A structural rule that
  // detached the limb by clearing its body handle would satisfy "the limb is
  // gone" and quietly skip the gout, the audio cause, the loco state rules and
  // the debris hand-off. Three independent signals that Sever() really ran.
  const bool machinery = r.limbGone && r.byBlade && r.debrisGained > 0;
  const bool ok = band && machinery;
  detail = Format(
      "%s/%s (%u voxels): heft %.1f %s after %d hits (band %d..%d, cap %d), "
      "byBlade=%d, limb detached=%d, +%u debris bodies, creature alive=%d",
      t.defName.c_str(), t.limbName.c_str(), t.atSpawn, (double)heft,
      r.severed ? "severed" : "NOT SEVERED", r.hits, kMin, kMax, kMax * 3 + 4,
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
  const int cap = (int)BaselineNumber("woundAccumMaxHits", 40) * 3 + 4;
  const float heavy = (float)BaselineNumber("woundHeftHeavy", 2.5);
  const CutRun light = HackThrough(c, t, 230, 1.0f, cap);
  const CutRun heavyRun = HackThrough(c, t, 230, heavy, cap);

  // STRICTLY fewer, not "no more". "No more" is satisfied by heft doing
  // nothing at all, which is exactly the regression this exists to catch: the
  // factor is computed in main.cpp and consumed three call levels down, and a
  // dropped multiplication there is invisible in play.
  //
  // THE LIGHT ARM MAY NEVER SEVER (2026-09-19). Since the owner's retune
  // (gore.cutLength 0.9 -> 0.64, cutDepthPower 0.32 -> 0.15) a heft-1.0 kerf
  // is narrower than the thigh it is cutting and grooves it to the cap --
  // 46 of 46 here, where it took 5 before -- see the note in
  // GateWoundAccumulate. "Never within the cap" is MORE hits than any severed
  // count, so the differential still reads and still fails the right way: a
  // dropped heft factor makes the heavy arm the light arm, and the heavy arm
  // not severing fails it. What is no longer required is that a sword alone
  // can take a leg off; that is a tuning statement and wound-accumulate makes
  // its claim at a heft that can.
  const bool fewer = heavyRun.severed &&
                     (!light.severed || heavyRun.hits < light.hits);

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
      "%s/%s: heft 1.0 %s %d hits, heft %.1f %s %d (cap %d); item heft sword "
      "%.2f cleaver %.2f (ref %.2f world voxels)",
      t.defName.c_str(), t.limbName.c_str(),
      light.severed ? "severs in" : "NOT severed in", light.hits, heavy,
      heavyRun.severed ? "severs in" : "NOT severed in", heavyRun.hits, cap,
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
  const float dmg = sword ? sword->Damage() : 14.0f;
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

// corpse-cut: a sword takes flesh off a corpse, and keeps taking it
// ---------------------------------------------------------------------------
//
// Owner report, 2026-09-20: "swords and weapons doing damage to corpses and
// rigid bodies appears to be gone... it does nothing. Physics and body
// interaction remain."
//
// THE PROPERTY, AND THE REASON IT COULD GO MISSING WITHOUT A GATE NOTICING.
// `DebrisSystem::DamageBody` carves TWO lattices: the SKIN (the art, and what
// the player sees) and the COLLIDER derived from it by a majority downsample.
// It used to decide "nothing in range" from the collider predicate alone and
// return before the skin was touched — and a blade's kerf is a sliver about a
// tenth of a world voxel across, while one collider cell of a human corpse is
// eight skin cells that only flip when half of them go. So every sword blow on
// a corpse took nothing, bled nothing and left no gore, while the corpse still
// shoved around exactly as before: damage gone, physics intact, which is the
// report word for word. `Mob::CarveLimb` has refused to make that decision on
// the collider since the fine skin existed; this is the same carve on the same
// art one function later, and nothing exercised it — every corpse gate until
// now went through `MeltBodyAt` (the laser's sphere), which is coarse enough
// to move the collider on its own.
//
// So the claim is the smallest one that the failure breaks: ONE sword-shaped
// kerf against a corpse removes voxels from the authoritative lattice, and the
// next two keep removing them (the entry snap must find the new surface, or
// the wound saturates and a corpse becomes uncuttable after one blow). The
// mace arm is the control: a blunt carve is a radius of voxels rather than a
// slot, it moves the collider by itself, and it was WORKING throughout — so a
// run where the blade takes nothing and the mace takes plenty is exactly the
// signature of the bug, and the two numbers are printed side by side.
//
// CPU ONLY: no tick is submitted, so this leaves the world as it found it.
Status GateCorpseCut(Ctx& c, std::string& detail) {
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
  // The blade goes through the LIMB the fixture chose, not the torso: the
  // torso is the root and its cut has to survive being the thing every joint
  // hangs off (that is corpse-intact's claim). Measured on the live rig,
  // because a corpse has no limbs left to measure.
  const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
  const uint64_t limbBody = mobs.LimbBody(id, t.limb);
  if (!torso || !limbBody || !ax.valid) {
    detail = Format("%s: fixture has no root/limb body to cut",
                    t.defName.c_str());
    mobs.Reset();
    c.debris.Reset();
    return Status::Fail;
  }
  const Vec3 at = ax.anchor + ax.along * (ax.reach * 0.5f);

  // Killed the way the sword kills: the root limb at zero hp is a death, and
  // Die() hands every limb to DebrisSystem with its joints on.
  {
    MobSystem::BladeCutScope blade(mobs, 1.0f);
    mobs.Damage(torso, 1.0e6f, at, 45.0f);
  }
  if (mobs.IsAlive(id)) {
    detail = Format("%s: root limb at zero hp did not kill", t.defName.c_str());
    mobs.Reset();
    c.debris.Reset();
    return Status::Fail;
  }

  auto indexOf = [&](uint64_t h) -> int {
    for (uint32_t i = 0; i < c.debris.BodyCount(); i++)
      if (c.debris.BodyHandle(i) == h) return (int)i;
    return -1;
  };
  int idx = indexOf(limbBody);
  if (idx < 0) {
    detail = Format("%s: the limb was not adopted as debris on death",
                    t.defName.c_str());
    mobs.Reset();
    c.debris.Reset();
    return Status::Fail;
  }

  // THE SAME SIX LINES MELEE BUILDS THE KERF FROM (game/melee.cpp's debris
  // branch, which builds it from the same tuning rows the live path does), so
  // a gore retune moves this gate and the game together.
  const auto& g = CurrentTuning().gore;
  const float kPower = 1.0f, kHeft = 1.0f, kRadius = 0.9f;
  std::vector<ParticleSpawn> spawns;
  uint64_t cur = c.debris.BodyHandle((uint32_t)idx);
  const uint32_t vox0 = c.debris.BodyVoxelCount((uint32_t)idx);
  uint32_t voxAfter[3] = {vox0, vox0, vox0};
  int cuts = 0;
  for (int i = 0; i < 3 && idx >= 0; i++) {
    KerfCut cut;
    cut.at = at;
    cut.edgeAxis = ax.edge;
    cut.cutDir = ax.travel;
    cut.halfWidth = std::max(kRadius * g.cutWidth, 0.08f);
    cut.depth = (g.cutDepth + g.cutDepthPower * kPower) * kHeft;
    cut.length = g.cutLength * (0.4f + 0.6f * kPower) * kHeft;
    cut.power = kPower;
    cut.seed = 0x50D5u + (uint32_t)i * 40503u;
    if (c.debris.CutBody(cur, cut, c.world, spawns)) cuts++;
    spawns.clear();
    // THE INDEX IS THE IDENTITY, NOT THE HANDLE. A carve that moved the
    // collider rebuilds it, and a rebuild REPLACES the Jolt handle — so the
    // handle recorded before the cut names nothing afterwards, while the slot
    // in DebrisSystem's own list is untouched (fragments are appended past the
    // end). This is corpse-intact's rule for the same reason.
    const uint32_t now = c.debris.BodyVoxelCount((uint32_t)idx);
    const uint32_t prev = i == 0 ? vox0 : voxAfter[i - 1];
    if (now > prev) break;   // the body was destroyed and something took its slot
    voxAfter[i] = now;
    cur = c.debris.BodyHandle((uint32_t)idx);
  }
  const uint32_t bladeTook = vox0 > voxAfter[2] ? vox0 - voxAfter[2] : 0u;
  const bool firstCut = voxAfter[0] < vox0;
  const bool deepens = voxAfter[2] < voxAfter[0];
  // The wound the cut armed: a corpse that is cut must BLEED from where it was
  // cut (DamageBody::ArmWound), which is the second half of the same report
  // ("no blood comes out").
  const bool bleeds = idx >= 0 && c.debris.BodyWoundOpen((uint32_t)idx);

  // ---- THE CONTROL ARM: a mace, on a second body of the same corpse --------
  // A blunt carve is a radius, not a slot, so it moves the collider on its own
  // and was never affected. If the blade takes nothing and this takes plenty,
  // the lattice the blade carves is the thing that is broken.
  uint32_t maceTook = 0;
  int tidx = indexOf(torso);
  if (tidx >= 0) {
    const uint32_t before = c.debris.BodyVoxelCount((uint32_t)tidx);
    const Vec3 tAt = c.debris.BodyPosition((uint32_t)tidx);
    c.debris.BluntBody(c.debris.BodyHandle((uint32_t)tidx), tAt,
                       std::max(g.bluntCarveRadius, 0.5f), 0xB1U, c.world,
                       spawns);
    spawns.clear();
    // The torso's handle may have been replaced too; find it by position.
    int best = -1;
    float bestD = 1e9f;
    for (uint32_t k = 0; k < c.debris.BodyCount(); k++) {
      const float d = (c.debris.BodyPosition(k) - tAt).len();
      if (d < bestD) { bestD = d; best = (int)k; }
    }
    if (best >= 0) {
      const uint32_t after = c.debris.BodyVoxelCount((uint32_t)best);
      maceTook = before > after ? before - after : 0u;
    }
  }

  RecordObserved("corpseCutVoxels", (double)bladeTook);
  const bool ok = cuts == 3 && firstCut && deepens && bladeTook > 0 && bleeds &&
                  maceTook > 0;
  mobs.Reset();
  c.debris.Reset();
  detail = Format(
      "%s limb '%s': 3 sword kerfs on the corpse took %u of %u voxels "
      "(%u -> %u -> %u -> %u, cuts accepted %d, deepens=%d, wound open=%d); "
      "the mace's control carve took %u",
      t.defName.c_str(), t.limbName.c_str(), bladeTook, vox0, vox0, voxAfter[0],
      voxAfter[1], voxAfter[2], cuts, deepens ? 1 : 0, bleeds ? 1 : 0,
      maceTook);
  return ok ? Status::Pass : Status::Fail;
}

// corpse-blunt: a mace marks a corpse, and what it pulps crumbles
// ---------------------------------------------------------------------------
//
// Owner, 2026-09-20: "mutilation etc" should work on corpses "just like with
// living bodies".
//
// THE LADDER, NOT THE CRATER. A blunt blow on a living creature climbs three
// rungs (Mob::BluntHit): the flesh MARKS, a saturated mark BREAKS and goes
// bloody, and tissue that is bloody at depth is PULPED and crumbles over the
// next few seconds. A corpse got none of that - it went straight to an instant
// sphere, which is the exact shape the living path was given a bruise to stop
// using. Dead tissue still marks and still crumbles; what it lacks is hp and a
// voice.
//
// The claim, in the order the rungs fire: sustained mace blows on one spot of
// a corpse (1) lay a bruise coat that was not there before, (2) arm the
// dissolution, and (3) go on taking voxels AFTER the blows stop, through
// DebrisSystem::PulpTick, without being touched again. A first blow on clean
// flesh must take nothing: the carve is EARNED by pulp, exactly as it is on
// the living.
Status GateCorpseBlunt(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 380));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  const auto& gt = CurrentTuning().gore;
  const uint32_t bruiseMat = mobs.MaterialIdNamed(gt.bruiseMat);
  if (bruiseMat == 0 || gt.bruiseStep <= 0.0f) {
    detail = Format("gore.bruiseMat '%s' resolves to nothing: no ladder to test",
                    gt.bruiseMat.c_str());
    return Status::Skip;
  }
  IVec3 pchunk{};
  const uint64_t id = SpawnTarget(c, t, 380, pchunk);
  if (!id) {
    detail = "spawn refused";
    return Status::Fail;
  }
  const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
  const uint64_t limbBody = mobs.LimbBody(id, t.limb);
  const int root = mobs.Defs()[t.defIndex].rootLimb;
  const uint64_t torso = root >= 0 ? mobs.LimbBody(id, root) : 0;
  if (!limbBody || !torso || !ax.valid) {
    detail = Format("%s: no limb to beat", t.defName.c_str());
    mobs.Reset();
    c.debris.Reset();
    return Status::Fail;
  }
  const Vec3 at = ax.anchor + ax.along * (ax.reach * 0.5f);
  {
    MobSystem::BladeCutScope blade(mobs, 1.0f);
    mobs.Damage(torso, 1.0e6f, at, 45.0f);
  }
  if (mobs.IsAlive(id)) {
    detail = Format("%s: root limb at zero hp did not kill", t.defName.c_str());
    mobs.Reset();
    c.debris.Reset();
    return Status::Fail;
  }
  auto indexOf = [&](uint64_t h) -> int {
    for (uint32_t i = 0; i < c.debris.BodyCount(); i++)
      if (c.debris.BodyHandle(i) == h) return (int)i;
    return -1;
  };
  int idx = indexOf(limbBody);
  if (idx < 0) {
    detail = Format("%s: the limb was not adopted as debris on death",
                    t.defName.c_str());
    mobs.Reset();
    c.debris.Reset();
    return Status::Fail;
  }

  // A MACE, resolved exactly as melee resolves it on loose matter: the bruise
  // first, the carve only as far as the pulp earns it.
  const uint32_t vox0 = c.debris.BodyVoxelCount((uint32_t)idx);
  const uint32_t coat0 = c.debris.BodyCoatCount((uint32_t)idx, bruiseMat);
  std::vector<ParticleSpawn> spawns;
  const float kHp = 16.0f;
  uint32_t voxAfterFirst = vox0;
  uint32_t coatAfterFirst = coat0;
  const int kBlows = 24;
  for (int i = 0; i < kBlows && idx >= 0; i++) {
    const uint64_t h = c.debris.BodyHandle((uint32_t)idx);
    const float ripe =
        c.debris.BruiseBody(h, at, gt.bruiseRadius, 0xB1u + (uint32_t)i * 977u,
                            1.0f, kHp, false);
    const float ripeFrom = std::clamp(gt.pulpCarveFrom, 0.0f, 1.0f);
    const float earned =
        ripeFrom >= 1.0f
            ? 0.0f
            : std::clamp((ripe - ripeFrom) / (1.0f - ripeFrom), 0.0f, 1.0f);
    if (earned > 0.0f && gt.bluntCarveRadius > 0.0f)
      c.debris.BluntBody(h, at, gt.bluntCarveRadius * 0.6f * earned,
                         0xB2u + (uint32_t)i * 977u, c.world, spawns);
    spawns.clear();
    if (i == 0) {
      voxAfterFirst = c.debris.BodyVoxelCount((uint32_t)idx);
      coatAfterFirst = c.debris.BodyCoatCount((uint32_t)idx, bruiseMat);
    }
  }
  const uint32_t vox1 = c.debris.BodyVoxelCount((uint32_t)idx);
  const uint32_t coat1 = c.debris.BodyCoatCount((uint32_t)idx, bruiseMat);
  const bool pulping = c.debris.BodyPulping((uint32_t)idx);

  // ...AND IT KEEPS GOING WITH NOBODY TOUCHING IT. The blows have stopped;
  // only DebrisSystem::PulpTick runs from here.
  // WHICH BODY IT IS, by the one identity that survives: neither the index nor
  // the handle does. The handle is replaced by the carve PulpTick
  // expresses itself as, and the INDEX is shuffled by the corpse's other
  // pieces settling back into the grid (DebrisSystem::SettleBodies swap-pops
  // them), which is what made the first two versions of this gate report a
  // limb that had dissolved to nothing when it had lost 98 voxels.
  const uint32_t serial = c.debris.BodySerial((uint32_t)idx);
  // Short of debris.settleAfterTicks, so the piece under test is still a body
  // at the end of the window rather than a patch of world voxels.
  uint32_t tick = 61000;
  for (int i = 0; i < 45; i++) {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> st;
    std::vector<CellOp> cellOps;
    mobs.PreTick(tick, c.world, ops, cellOps, st);
    c.debris.PreTick(tick, c.world, cellOps, st);
    c.phys.Step(kTickDt);
    mobs.PostStep();
    c.debris.PostStep();
    tick++;
  }
  // BY INDEX, NOT BY HANDLE. PulpTick expresses itself as a carve, a carve
  // rebuilds the collider, and a rebuild REPLACES the Jolt handle — so the
  // handle recorded before the ticks names nothing afterwards and a
  // handle-keyed lookup reads "0 voxels left" for a body that is simply
  // wearing a new number. That artifact made the first version of this gate
  // report a limb dissolving to nothing when it had lost 98 voxels.
  const int idx2 = c.debris.FindBodySerial(serial);
  const uint32_t vox2 =
      idx2 >= 0 ? c.debris.BodyVoxelCount((uint32_t)idx2) : 0;

  RecordObserved("corpseBluntCoat", (double)(coat1 - coat0));
  RecordObserved("corpseBluntDissolved", (double)(vox1 > vox2 ? vox1 - vox2 : 0));
  const bool marked = coat1 > coat0;
  const bool firstBlowTookNothing = voxAfterFirst == vox0;
  const bool firstBlowMarked = coatAfterFirst > coat0;
  const bool dissolved = vox2 < vox1;
  const bool ok = marked && firstBlowMarked && firstBlowTookNothing &&
                  (pulping || dissolved) && dissolved;
  mobs.Reset();
  c.debris.Reset();
  detail = Format(
      "%s limb '%s' as a corpse, %d mace blows (%.0f hp) on one spot: bruise "
      "coat %u -> %u (first blow %u, took %u voxels), lattice %u -> %u, then "
      "45 ticks untouched -> %u (pulping=%d)",
      t.defName.c_str(), t.limbName.c_str(), kBlows, (double)kHp, coat0, coat1,
      coatAfterFirst, vox0 - voxAfterFirst, vox0, vox1, vox2, pulping ? 1 : 0);
  return ok ? Status::Pass : Status::Fail;
}

// corpse-dismember: a corpse comes apart where you cut it, and it is heard
// ---------------------------------------------------------------------------
//
// Owner, 2026-09-20: "need audio cues for gore with corpses just like with
// living bodies, mutilation etc."
//
// TWO THINGS A CORPSE COULD NOT DO. It could not be TAKEN APART: severing on
// the debris side is connectivity inside ONE body, and a corpse is a dozen
// bodies held together by the joints Mob::Die deliberately leaves on, which
// nothing ever cut — so a blade could part a neck completely and the head
// stayed attached. And it could not be HEARD: main.cpp picks the cue for a
// blow by differencing the sever and voice queues, dead flesh fills neither,
// so hacking a body apart made the noise a crate makes.
//
// Both are the same fix from the same direction — dead flesh reports what
// happens to it the way living flesh does — so they are one gate. The claim:
// sustained blade cuts at a corpse's NECK part the joint within a sane number
// of blows, both ends bleed, and the blow that did it pushes a GoreEvent that
// names the creature, says a piece came off, and says an edge did it.
//
// THE BOUND MATTERS AS MUCH AS THE EVENT. Unbounded, this passes on a model
// that needs four hundred blows, which is the same as not working — that is
// what it looked like before the corpse carve got the SPALL the living carve
// has always had (phys/lattice.h SpallGrow). `corpseDismemberMaxCuts` is the
// ceiling and `corpse-intact` is the other side of the bet: a cut in the
// MIDDLE of a limb must not part anything.
Status GateCorpseDismember(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 380));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  const MobDef& def = mobs.Defs()[t.defIndex];
  // The head and the limb it hangs off, the same discovery `corpse-bleed`
  // makes: a VITAL severable limb that is not the root, and its parent.
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
  const uint64_t id = SpawnTarget(c, t, 380, pchunk);
  if (!id) {
    detail = "spawn refused";
    return Status::Fail;
  }
  const uint64_t headBody0 = mobs.LimbBody(id, head);
  const uint64_t neckBody0 = mobs.LimbBody(id, neck);
  const int root = def.rootLimb;
  const uint64_t torso = root >= 0 ? mobs.LimbBody(id, root) : 0;
  if (!headBody0 || !neckBody0 || !torso) {
    detail = Format("%s: no head/neck/torso body to cut", t.defName.c_str());
    mobs.Reset();
    c.debris.Reset();
    return Status::Fail;
  }
  // Killed the way the sword kills. Handles do not change across adoption.
  {
    MobSystem::BladeCutScope blade(mobs, 1.0f);
    mobs.Damage(torso, 1.0e6f, mobs.LimbAnchorPos(id, head), 45.0f);
  }
  if (mobs.IsAlive(id)) {
    detail = Format("%s: root limb at zero hp did not kill", t.defName.c_str());
    mobs.Reset();
    c.debris.Reset();
    return Status::Fail;
  }
  c.debris.ClearGoreEvents();

  auto indexOf = [&](uint64_t h) -> int {
    for (uint32_t i = 0; i < c.debris.BodyCount(); i++)
      if (c.debris.BodyHandle(i) == h) return (int)i;
    return -1;
  };
  int hidx = indexOf(headBody0);
  if (hidx < 0) {
    detail = Format("%s: the head was not adopted as debris on death",
                    t.defName.c_str());
    mobs.Reset();
    c.debris.Reset();
    return Status::Fail;
  }
  // WHERE THE NECK JOINT IS, asked of physics rather than of the rig: the rig
  // is gone, and the joint knows its own anchor (Physics::JointsOn).
  std::vector<Physics::BodyJoint> js;
  c.phys.JointsOn(c.debris.BodyHandle((uint32_t)hidx), js);
  uint64_t neckJoint = 0;
  Vec3 anchorW{};
  for (const Physics::BodyJoint& j : js)
    if (j.other == neckBody0) {
      neckJoint = j.joint;
      // THROUGH THE BODY'S OWN ROTATION. The anchor is body-local and a corpse
      // has just fallen over; adding it to the position alone puts the blow
      // somewhere off in the air, which is how this gate first measured 60
      // cuts that removed 17 voxels between them.
      BodyTransform hx{};
      c.phys.GetTransform(c.debris.BodyHandle((uint32_t)hidx), hx);
      const Quat hq{hx.quat[0], hx.quat[1], hx.quat[2], hx.quat[3]};
      anchorW = hx.pos + QuatRotate(hq, j.anchorLocalVox);
      break;
    }
  if (!neckJoint) {
    detail = Format("%s: the head kept no joint to '%s' after death",
                    t.defName.c_str(), def.limbs[neck].name.c_str());
    mobs.Reset();
    c.debris.Reset();
    return Status::Fail;
  }
  const uint32_t jointsAtDeath = c.phys.JointCount();

  // Cuts at the joint, with the sword's own kerf (the six lines melee builds).
  const auto& g = CurrentTuning().gore;
  const int kCap = (int)BaselineNumber("corpseDismemberMaxCuts", 60);
  const float kPower = 1.0f, kHeft = 1.5f, kRadius = 0.9f;
  std::vector<ParticleSpawn> spawns;
  const uint32_t vox0 = c.debris.BodyVoxelCount((uint32_t)hidx);
  // THE NUMBER THAT ACTUALLY DECIDES (DebrisSystem::PartJointsAt): how much
  // flesh is within the hold radius of the anchor. Reported at both ends, so a
  // gate that fails says WHY instead of saying the head stayed on.
  const float hold = CurrentTuning().gore.corpseJointHold;
  const uint32_t held0 = c.debris.VoxelsNearWorld(
      c.debris.BodyHandle((uint32_t)hidx), anchorW, hold);
  int cuts = 0;
  bool parted = false;
  for (int i = 0; i < kCap && !parted && hidx >= 0; i++) {
    // ---- A CHOP LANDS ON MATTER, NOT ON A COORDINATE --------------------
    //
    // The blade meets the body at its SURFACE nearest the joint and travels
    // INTO the joint from there. Aiming at the anchor itself put 56 of 60
    // blows in the air (the anchor sits at the boundary between two bodies,
    // which is exactly where neither one's lattice is), and the kerf's entry
    // snap only reaches about a voxel. Re-derived every blow, so a second
    // chop lands in the groove the first one opened — which is what makes
    // this a repeated chop at one place rather than a stipple.
    const uint64_t hNow0 = c.debris.BodyHandle((uint32_t)hidx);
    const Vec3 surf = c.debris.NearestVoxelWorld(hNow0, anchorW);
    Vec3 into = anchorW - surf;
    if (into.len() < 1e-3f) into = Vec3{0, -1, 0};
    into = into.normalized();
    KerfCut cut;
    cut.at = surf;
    // The edge across the travel: any perpendicular will do for a chop.
    Vec3 edge = into.cross(Vec3{0, 1, 0});
    if (edge.len() < 0.15f) edge = into.cross(Vec3{1, 0, 0});
    cut.edgeAxis = edge.normalized();
    cut.cutDir = into;
    cut.halfWidth = std::max(kRadius * g.cutWidth, 0.08f);
    cut.depth = (g.cutDepth + g.cutDepthPower * kPower) * kHeft;
    cut.length = g.cutLength * (0.4f + 0.6f * kPower) * kHeft;
    cut.power = kPower;
    cut.seed = 0xBEEFu + (uint32_t)i * 40503u;
    c.debris.CutBody(c.debris.BodyHandle((uint32_t)hidx), cut, c.world, spawns);
    spawns.clear();
    cuts++;
    // The joint is gone when nothing on the head answers to it any more.
    const uint64_t hNow = c.debris.BodyHandle((uint32_t)hidx);
    c.phys.JointsOn(hNow, js);
    parted = true;
    for (const Physics::BodyJoint& j : js)
      if (j.joint == neckJoint) parted = false;
    if (c.debris.BodyVoxelCount((uint32_t)hidx) == 0) break;
  }
  const uint32_t vox1 = hidx >= 0 ? c.debris.BodyVoxelCount((uint32_t)hidx) : 0;
  const uint32_t held1 =
      hidx >= 0 ? c.debris.VoxelsNearWorld(
                      c.debris.BodyHandle((uint32_t)hidx), anchorW, hold)
                : 0;
  const uint32_t jointsAfter = c.phys.JointCount();

  // ---- WHAT THE ROOM HEARD -------------------------------------------------
  int goreEvents = 0, severEvents = 0, bladeEvents = 0, named = 0;
  for (const DebrisSystem::GoreEvent& ge : c.debris.GoreEvents()) {
    goreEvents++;
    if (ge.severed) severEvents++;
    if (ge.severed && ge.byBlade) bladeEvents++;
    if (ge.defIndex >= 0 && ge.defIndex < (int)mobs.Defs().size()) named++;
  }
  const bool headBleeds = hidx >= 0 && c.debris.BodyWoundOpen((uint32_t)hidx);
  const int nidx = indexOf(neckBody0);
  const bool neckBleeds = nidx >= 0 && c.debris.BodyWoundOpen((uint32_t)nidx);

  RecordObserved("corpseDismemberCuts", (double)cuts);
  const bool ok = parted && cuts <= kCap && jointsAfter < jointsAtDeath &&
                  severEvents > 0 && bladeEvents > 0 && named == goreEvents &&
                  goreEvents > 0 && headBleeds && neckBleeds;
  mobs.Reset();
  c.debris.Reset();
  detail = Format(
      "%s: %d sword cuts at the '%s' joint of a corpse -> parted=%d (cap %d), "
      "head %u -> %u voxels, flesh holding the joint %u -> %u (within %.2f "
      "vox), joints %u -> %u; %d gore events (%d severed, %d "
      "of those by a blade, %d named their species), head bleeds=%d neck "
      "bleeds=%d",
      t.defName.c_str(), cuts, def.limbs[neck].name.c_str(), parted ? 1 : 0,
      kCap, vox0, vox1, held0, held1, (double)hold, jointsAtDeath, jointsAfter,
      goreEvents, severEvents, bladeEvents, named, headBleeds ? 1 : 0,
      neckBleeds ? 1 : 0);
  return ok ? Status::Pass : Status::Fail;
}

// hit-drive: a blow rocks what it lands on, whoever is driving it
// ---------------------------------------------------------------------------
//
// Owner, 2026-09-20: "every time we switch behavior for a living mob that has
// to then be rewritten for corpse behavior? like right now mobs can ragdoll
// and when ragdolling they essentially behave just like corpses; so all the
// parts are practically there."
//
// THE AXIS IS HOW A BODY IS DRIVEN, NOT WHETHER IT IS ALIVE
// (docs/PLAN_struck_matter.md). A rig-posed limb answers a blow with a pose
// spring, because the pose pipeline is what writes its transform; a
// solver-owned one answers with an impulse, because Jolt is. A LIMP LIVING
// LIMB and a CORPSE LIMB are the same case, and `Mob::HitReact` has said so
// since it was written — it refuses both and names the impulse as what it
// defers to. That impulse did not exist, so until today the third of those
// three arms and the second one both got NOTHING: a ragdolling creature and a
// corpse took a mace without moving.
//
// THE CLAIM, in one fixture and three arms: the same blow, on the same limb of
// the same def, standing / limp / dead. `Physics::IsBodyDynamic` must say
// kinematic, dynamic, dynamic — that is the drive itself — and the change in
// the struck body's VELOCITY across the sweep must be nil for the first and
// real for the other two. Velocity across the call rather than position over
// time, because an impulse is instantaneous and a limp body is also falling:
// no physics step runs between the two samples, so gravity cannot contribute.
//
// The profile is a mace with NO carve (bluntCarve 0, cut 0): the reaction is
// the only thing under test, and a carve would rebuild the collider and
// replace the handle being measured.
Status GateHitDrive(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 380));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  const Tuning::CombatFx& fx = CurrentTuning().combatfx;
  if (!fx.hitReact || fx.hitReactImpulse <= 0.0f) {
    detail = Format(
        "combatfx.hitReact=%d / hitReactImpulse=%.1f: the reaction is turned "
        "off in tuning, so there is nothing to assert",
        fx.hitReact ? 1 : 0, (double)fx.hitReactImpulse);
    return Status::Skip;
  }

  // A MACE THAT DENTS NOTHING. Blunt hp only, so `StrikeProfile::Total()` is
  // nonzero (the reaction scales on it) while no resolver touches a lattice.
  StrikeProfile mace;
  mace.blunt = 18.0f;
  mace.bluntCarve = 0.0f;

  MeleeTuning mt;
  ApplyMeleeTuning(mt);
  std::vector<ParticleSpawn> spawns;

  struct Arm {
    const char* name;
    bool dynamic = false;
    float dv = 0.0f, dw = 0.0f;
    bool struck = false;
    // ---- ...AND THE HALF THAT IS NOT SHARED ------------------------------
    // A LIMP BODY IS STILL A LIVING ONE. It has hp, it can be hurt, it cries,
    // it can die of the blow; a corpse answers none of those. Recorded and
    // asserted because the first version of this gate reported that limp and
    // dead were "identical", which is true of the REACTION and false of the
    // blow — and a gate that asserts a false thing teaches it.
    float hpDrop = 0.0f;
    bool owned = false;    // the mob still holds this body
    bool voiced = false;   // the creature had something to say about it
  };
  Arm arms[3] = {{"standing"}, {"limp"}, {"dead"}};

  bool fixtureOk = true;
  std::string why;
  for (int a = 0; a < 3 && fixtureOk; a++) {
    mobs.Reset();
    c.debris.Reset();
    // Two creatures, for MeleeSweepDamage's reason: it refuses to cut the
    // wielder, so the victim cannot be its own attacker (selftest_impact.cpp).
    const uint64_t wid = mobs.Spawn(t.defIndex, FixtureSite(c.world, 405));
    const uint64_t id = mobs.Spawn(t.defIndex, FixtureSite(c.world, 460));
    if (!wid || !id) {
      fixtureOk = false;
      why = "spawn refused";
      break;
    }
    auto step = [&](uint32_t tick) {
      std::vector<BrushOp> ops;
      std::vector<ParticleSpawn> st;
      std::vector<CellOp> cellOps;
      mobs.PreTick(tick, c.world, ops, cellOps, st);
      c.phys.Step(kTickDt);
      mobs.PostStep();
      c.debris.PostStep();
    };
    for (int i = 0; i < 8; i++) step(3000u + (uint32_t)i);

    // Where the blow goes, measured while the rig still has limbs to measure.
    const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
    const Vec3 mid = ax.anchor + ax.along * (ax.reach * 0.5f);
    uint64_t body = mobs.LimbBody(id, t.limb);
    if (!body || !ax.valid) {
      fixtureOk = false;
      why = "the fixture has no limb to hit";
      break;
    }

    // ---- put the victim in this arm's DRIVE state ------------------------
    if (a == 1) {
      Mob* victim = mobs.FindMobById(id);
      if (victim) victim->StartRagdoll(2.0f, "hit-drive gate");
      for (int i = 0; i < 4; i++) step(3100u + (uint32_t)i);
      body = mobs.LimbBody(id, t.limb);   // still the mob's, now dynamic
    } else if (a == 2) {
      const int root = mobs.Defs()[t.defIndex].rootLimb;
      const uint64_t torso = root >= 0 ? mobs.LimbBody(id, root) : 0;
      {
        MobSystem::BladeCutScope blade(mobs, 1.0f);
        if (torso) mobs.Damage(torso, 1.0e6f, mid, 45.0f);
      }
      // Die() hands every limb to DebrisSystem WITHOUT changing its handle,
      // so the one measured above is still the body that was just adopted.
      for (int i = 0; i < 2; i++) step(3200u + (uint32_t)i);
    }
    if (!body) {
      fixtureOk = false;
      why = "the limb body was gone before the blow";
      break;
    }
    arms[a].dynamic = c.phys.IsBodyDynamic(body);

    Mob* wielder = mobs.FindMobById(wid);
    if (!wielder) {
      fixtureOk = false;
      why = "the wielder vanished before it could swing";
      break;
    }
    // The same swept quad `impact-blunt` builds: across the limb, fast enough
    // to sit at the top of the speed ramp.
    const Vec3 cand[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    int bi = 0;
    float bd = 2.0f;
    for (int i = 0; i < 3; i++) {
      const float d = std::fabs(ax.along.dot(cand[i]));
      if (d < bd) { bd = d; bi = i; }
    }
    const Vec3 travel = ax.along.cross(cand[bi]).normalized();
    const Vec3 edge = travel.cross(ax.along).normalized();
    const float stepLen = mt.fullSpeed * kTickDt * 1.2f;
    EdgeSweep sw;
    sw.aPrev = mid - edge * 1.5f - travel * stepLen;
    sw.bPrev = mid + edge * 1.5f - travel * stepLen;
    sw.aNow = mid - edge * 1.5f;
    sw.bNow = mid + edge * 1.5f;
    sw.flatNow = Vec3{};
    sw.dt = kTickDt;
    sw.halfWidth = 0.4f;
    sw.strike = mace;
    sw.heft = 1.0f;
    sw.tick = 7400u + (uint32_t)a;
    sw.valid = true;

    // NO STEP BETWEEN THESE TWO SAMPLES. An impulse is instantaneous and a
    // limp body is also in freefall; sampling across a physics step would mix
    // the blow with gravity and the arms would differ for the wrong reason.
    Vec3 v0{}, w0{}, v1{}, w1{};
    c.phys.GetBodyVelocities(body, v0, w0);
    const float hp0 = mobs.LimbBody(id, t.limb) ? mobs.LimbHp(id, t.limb) : 0.0f;
    const size_t voi0 = mobs.VoiceEvents().size();
    const EdgeSweepResult r = MeleeSweepDamage(sw, mt, *wielder, c.phys, mobs,
                                               c.debris, c.world, spawns);
    spawns.clear();
    c.phys.GetBodyVelocities(body, v1, w1);
    arms[a].struck = r.bodiesHit > 0;
    arms[a].dv = (v1 - v0).len();
    arms[a].dw = (w1 - w0).len();
    arms[a].owned = mobs.LimbBody(id, t.limb) == body;
    const float hp1 = arms[a].owned ? mobs.LimbHp(id, t.limb) : 0.0f;
    arms[a].hpDrop = arms[a].owned ? std::max(0.0f, hp0 - hp1) : 0.0f;
    arms[a].voiced = mobs.VoiceEvents().size() > voi0;
    mobs.ClearVoiceEvents();
  }
  mobs.Reset();
  c.debris.Reset();
  if (!fixtureOk) {
    detail = Format("%s: %s", t.defName.c_str(), why.c_str());
    return Status::Fail;
  }

  // voxels/s. A floor rather than a band: the claim is "it moved", and how far
  // is `hitReactImpulse`'s business, which the owner is free to retune.
  const float kMin = (float)BaselineNumber("hitDriveMinDeltaVox", 1.0);
  const bool drivesOk = !arms[0].dynamic && arms[1].dynamic && arms[2].dynamic;
  const bool struckAll =
      arms[0].struck && arms[1].struck && arms[2].struck;
  const bool stillOk = arms[0].dv < kMin;   // rig-posed: the spring, not a shove
  const bool movedOk = arms[1].dv >= kMin && arms[2].dv >= kMin;
  // ---- AND LIMP IS NOT DEAD ----------------------------------------------
  //
  // The REACTION is shared because it is a fact about drive; the BLOW is not,
  // because physiology is a fact about being alive. A limp creature is still
  // a creature: the mob still owns the body, the blow still costs hp, and it
  // still has something to say. A corpse's limb belongs to DebrisSystem, has
  // no hp to lose and says nothing. Asserting both halves is what keeps the
  // unification honest — it would otherwise be one short step from "the dead
  // and the merely knocked-down are the same thing", which they are not.
  const bool livingHurt = arms[0].hpDrop > 0.0f && arms[1].hpDrop > 0.0f &&
                          arms[0].owned && arms[1].owned;
  const bool deadIsNot = !arms[2].owned && arms[2].hpDrop <= 0.0f &&
                         !arms[2].voiced;
  RecordObserved("hitDriveLimpDeltaVox", (double)arms[1].dv);
  RecordObserved("hitDriveDeadDeltaVox", (double)arms[2].dv);
  const bool ok = drivesOk && struckAll && stillOk && movedOk && livingHurt &&
                  deadIsNot;
  detail = Format(
      "%s limb '%s', one mace blow (%.0f hp, no carve) in three drive states. "
      "REACTION (shared, by drive): standing dynamic=%d dv=%.2f dw=%.2f | limp "
      "dynamic=%d dv=%.2f dw=%.2f | dead dynamic=%d dv=%.2f dw=%.2f. "
      "PHYSIOLOGY (not shared): standing owned=%d hp-%.1f voice=%d | limp "
      "owned=%d hp-%.1f voice=%d | dead owned=%d hp-%.1f voice=%d "
      "(floor %.2f vox/s, impulse %.0f kg*m/s)",
      t.defName.c_str(), t.limbName.c_str(), (double)mace.blunt,
      arms[0].dynamic ? 1 : 0, (double)arms[0].dv, (double)arms[0].dw,
      arms[1].dynamic ? 1 : 0, (double)arms[1].dv, (double)arms[1].dw,
      arms[2].dynamic ? 1 : 0, (double)arms[2].dv, (double)arms[2].dw,
      arms[0].owned ? 1 : 0, (double)arms[0].hpDrop, arms[0].voiced ? 1 : 0,
      arms[1].owned ? 1 : 0, (double)arms[1].hpDrop, arms[1].voiced ? 1 : 0,
      arms[2].owned ? 1 : 0, (double)arms[2].hpDrop, arms[2].voiced ? 1 : 0,
      (double)kMin, (double)fx.hitReactImpulse);
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

  // THE CORPSE, NAMED BY ITS OWN HANDLES, ON THE LAST FRAME IT IS STILL A RIG.
  // Every limb that is about to become debris, plus the shells captured above
  // (the ones on the limb that already came off are not on the rig any more).
  // `Mob::Die` hands these exact handles to DebrisSystem, so membership in this
  // set IS "part of this corpse".
  //
  // Gathered HERE rather than after the kill, and that is the whole point: the
  // previous version took "every debris body that is not one I saw at gate
  // start", which is a set defined by exclusion and therefore grows. In a full
  // --selftest an earlier gate's creature dies during this fixture's 120
  // carving strokes, 400 voxels away, and its limbs land in that set: measured
  // spread 418.1 voxels against a cap of 60 while every piece of THIS corpse
  // was inside 29. Naming the subject cannot do that, however dirty the suite
  // around it is -- the trap this file already records once as "a fixture that
  // measures the suite rather than the subject".
  //
  // A handle changes if a collider is rebuilt, and such a piece drops out of
  // the measurement. Nothing carves during the settle below, so that does not
  // happen today; if it starts to, the symptom is the surviving count falling
  // over the run rather than a wrong number.
  // The shell set is refreshed here as well as captured before the strokes,
  // and needs both halves: carving a limb REBUILDS its collider under a new
  // handle, so the pre-stroke capture goes stale for anything the sword
  // touched -- and the shells that left on the severed limb are no longer on
  // the rig to be re-read. Union, so "was that peak armour?" stays right for
  // both.
  for (int sIdx = bareLimbs; sIdx < mob->LimbCount(); sIdx++)
    if (const uint64_t h = mobs.LimbBody(id, sIdx)) shellBodies.insert(h);
  std::unordered_set<uint64_t> mine = shellBodies;
  for (int li = 0; li < mob->LimbCount(); li++)
    if (const uint64_t h = mobs.LimbBody(id, li)) mine.insert(h);

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
  // `foreignBodies` is now only ever reported (`foreignCount`), which is the
  // honest use for it: how dirty the suite was when this gate started is worth
  // seeing in the detail line, and is not something to define the subject by.
  (void)foreignBodies;

  // Three seconds of corpse, measured every tick over every piece.
  c.phys.ResetRunawayProbe();
  float maxSpeed = 0.0f, maxSpin = 0.0f, maxSpread = 0.0f;
  int speedTick = -1, spinTick = -1;
  bool spinIsShell = false;
  // 90% of Jolt's default max angular velocity (15*pi rad/s). See the note at
  // the sample below for why the threshold is expressed against the CLAMP.
  const float kPegged = 0.9f * 47.1238898f;
  uint32_t peggedSamples = 0;
  // THE KILL HAS A TRANSIENT, AND A MOTOR IS NOT A TRANSIENT (2026-09-19).
  // The note above the assertion already exempts the PEAK for exactly this
  // reason -- a limb thrown by the killing blow may touch the clamp for a
  // tick -- but the pegged count was one bucket, so the corpse landing and
  // the corpse being driven read as the same number. Measured after the
  // owner's gore retune, with the corpse coming down WHOLE (hand.L under its
  // gauntlet is no longer cut through in 120 strokes: 89b4bb9 retargets a
  // probe that meets flesh under a worn shell onto the shell): 7 body-ticks
  // across 5 bodies, the last on tick 15, and not one after. That is the
  // landing. The report this gate exists for is minutes of every limb at the
  // clamp, which is LATE and SUSTAINED -- so the count is split at
  // corpseArmorSettleTicks: the early bucket gets a loose cap (a motor pegs
  // every body every tick and blows through it in one tick), the late bucket
  // keeps the tight one.
  const int settleTicks = (int)BaselineNumber("corpseArmorSettleTicks", 30);
  uint32_t peggedEarly = 0;
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
      // A FOLLOWER IS NOT A SECOND BODY, SO IT IS NOT A SECOND SAMPLE. Its
      // velocity is not a measurement of anything: DriveStraps WRITES its
      // host's rigid-body velocity into it every tick, so a shell on a limb
      // spinning at the clamp reads the clamp too, and counting both says "2
      // bodies pegged" about one. The suite run that caught this reported 5
      // pegged body-ticks across 3 bodies, "2 of them armour" -- and those two
      // were the same tumble as the flesh they are strapped to. Its POSITION
      // still counts toward `spread`, because "did the armour stay on the
      // corpse" is a real question about a real body.
      Vec3 lin{}, ang{};
      if (!c.debris.WornHostOf(h) && c.phys.GetBodyVelocities(h, lin, ang)) {
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
          if (i < settleTicks) peggedEarly++;
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

  // ---- THE MOTOR ITSELF, NOT ITS SYMPTOM ----------------------------------
  //
  // Everything above measures what the corpse DID. This measures what it IS,
  // which is the claim that survives a tuning change: a dressed corpse must be
  // the same object to the solver as a naked one. A worn shell is strapped to
  // the limb it covers (DebrisSystem::StrapBody) — kinematic, pose derived —
  // so it may carry NO joint. One that did would be the Fixed constraint
  // Mob::Die built until 2026-09-13, across an iron-against-flesh mass ratio
  // and a deep overlap, which is the energy source the whole runaway net
  // downstream of it exists to survive.
  //
  // Stated over the shells that are still bodies, because a piece that was
  // culled or looted is not evidence either way. The A/B arm
  // (SANDVOX_NO_RIGWELD=1) restores the jointed shape deliberately, and is
  // recognised by there being no straps at all rather than by a second env
  // read: with the arm on, `strapped` is 0 and the joint claim is not made.
  uint32_t shellsLeft = 0, shellJoints = 0, shellsStrapped = 0;
  for (uint32_t b = 0; b < c.debris.BodyCount(); b++) {
    const uint64_t h = c.debris.BodyHandle(b);
    if (!shellBodies.count(h)) continue;
    shellsLeft++;
    shellJoints += c.phys.JointCount(h);
    if (c.debris.WornHostOf(h)) shellsStrapped++;
  }
  // A strapped shell must ALSO still be on its corpse: a follower whose host
  // was culled unstraps and falls, and one that never moved would pass the
  // joint claim while hanging in the air. `maxSpread` above is what bounds
  // that, over every piece including these.
  const bool strapsHold =
      shellsStrapped == 0 ||
      (shellJoints == 0 && shellsStrapped == shellsLeft);
  RecordObserved("corpseArmorShellJoints", (double)shellJoints);
  RecordObserved("corpseArmorShellsStrapped", (double)shellsStrapped);

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
  const double peggedEarlyCap =
      BaselineNumber("corpseArmorMaxPeggedTicksEarly", 20.0);
  const uint32_t peggedLate = peggedSamples - peggedEarly;
  RecordObserved("corpseArmorMaxSpeedVox", (double)maxSpeed);
  RecordObserved("corpseArmorMaxSpinRad", (double)maxSpin);
  RecordObserved("corpseArmorMaxSpreadVox", (double)maxSpread);
  RecordObserved("corpseArmorWorstStepMs", worstStepMs);
  RecordObserved("corpseArmorPeggedBodyTicks", (double)peggedSamples);
  RecordObserved("corpseArmorPeggedBodyTicksEarly", (double)peggedEarly);
  RecordObserved("corpseArmorPeggedBodyTicksLate", (double)peggedLate);

  // THE PEAK SPIN IS RECORDED, NOT ASSERTED, and that is the whole shape of
  // the fix. 47.12 rad/s is Jolt's clamp and a limb genuinely thrown by a
  // sword blow may touch it for a tick; what may not happen is a limb SITTING
  // there, which is `peggedSamples`, and what may never happen is the net
  // having to intervene on an ordinary corpse, which is `cut` and `repaired`.
  // An assertion on the peak would have to be set above the clamp to pass at
  // all, and would then assert nothing.
  const bool ok = died && maxSpeed <= (float)speedCap &&
                  maxSpread <= (float)spreadCap &&
                  (double)peggedEarly <= peggedEarlyCap &&
                  (double)peggedLate <= peggedCap && net.cut == 0 &&
                  net.repaired == 0 && netHeld && strapsHold;
  detail = Format(
      "%s: %d base limbs + %d shells from %d worn pieces, %u joints dressed; "
      "%s cut off in %d strokes (severed=%d), died=%d with %u joints, %u of "
      "its %u debris bodies left (%u were in the world before it); "
      "%u shells still bodies, %u strapped to their limb, %u joints on them "
      "(%s); over %d "
      "corpse ticks the fastest piece hit %.1f vox/s on "
      "tick %d (cap %.0f) at (%.1f, %.1f, %.1f), %.1f vox under ground, %.0f%% "
      "straight down; fastest spin %.2f rad/s on tick %d (%s, recorded not capped: %.2f); spread "
      "%.1f vox (cap %.0f); %u body-ticks pegged at Jolt's own clamp across %u "
      "bodies (%u of them armour): %u in the first %d ticks (the kill, cap "
      "%.0f) and %u after (cap %.0f), last on tick %d; the net damped "
      "%u / cut %u / repaired %u; worst physics step %.1f ms on tick %d. "
      "Driven arm: %u ticks at the ceiling -> damped %u, cut %u, joints %u -> "
      "%u, ended at %.1f rad/s (%s)",
      t.defName.c_str(), bareLimbs, shells, wornPieces, jointsDressed,
      cutName.c_str(), strokes, severed ? 1 : 0, died ? 1 : 0, jointsDead,
      ours, (unsigned)mine.size(), foreignCount, shellsLeft, shellsStrapped,
      shellJoints, strapsHold ? "followers" : "ROPED TO THE CORPSE",
      kTicks, (double)maxSpeed,
      speedTick, speedCap,
      (double)peakPos.x, (double)peakPos.y, (double)peakPos.z,
      (double)peakUnderGround, (double)(fallFrac * 100.0f), (double)maxSpin,
      spinTick, spinIsShell ? "armour" : "flesh", (double)maxSpin,
      (double)maxSpread, spreadCap, peggedSamples,
      (unsigned)peggedBodies.size(), (unsigned)peggedShells.size(),
      peggedEarly, settleTicks, peggedEarlyCap, peggedLate, peggedCap,
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
  // ARMED BY ITS OWN RADIUS, exactly as `wound-chip` arms the same claim
  // (woundChipSoakArmRadius, and the long note beside it). The owner's gore
  // retune took gore.woundStainRadius to 0.1 world voxels — under one skin
  // cell at skinScale 8 — so an ordinary cut REWRITES NOTHING into the wound
  // material and "0 exposed" is the tuning speaking, not a regression. The
  // rest of the claim (never on bone, and the creature survives the cut) holds
  // whatever the radius is, so only the "something was stained, and more of it
  // on the surface than inside" half is gated. Turn the soak back up and this
  // re-arms by itself.
  const float soakArm = (float)BaselineNumber("woundChipSoakArmRadius", 0.5);
  const bool wantSoak = CurrentTuning().gore.woundStainRadius >= soakArm;
  const bool soakOk = hit && nonTissueStained == 0 && mobs.IsAlive(id) &&
                      (!wantSoak ||
                       (stainedExposed > 0 && rateExposed > rateBuried));
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
      "%s: soak on %s (armed=%d): %u exposed (rate %.3f) vs %u buried "
      "(rate %.3f), %u "
      "non-tissue stained; died of '%s', head off=%d, head wound=%d (%.0f vox) "
      "neck wound=%d (%.0f vox), %u wounded bodies; over %d ticks the head "
      "shed %.0f blood spawns and the neck %.0f, wounds closed at t+%d; corpse "
      "cut %s, closed again at t+%d; neck body gone at t+%d (budget %.0f left, "
      "moved %.1f vox, last y %.1f), head body gone at t+%d, %u bodies at end",
      t.defName.c_str(), t.limbName.c_str(), wantSoak ? 1 : 0, stainedExposed,
      rateExposed, stainedBuried, rateBuried, nonTissueStained, cause.c_str(),
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
    c.debris.QueueSupportEvents(c.world.Snap());
    c.debris.PreTick(tick + 1, c.world, cellOps, spawns);
    // BOTH passes' ops, since 2026-09-22: a corpse's flesh burns in the living
    // limb pass (MobSystem::BurnCorpses, inside mobs.PreTick) and only its
    // garments in the debris one. The counters are zeroed after the death
    // (phase B below), when the corpse is the only thing left emitting, so
    // counting from the top of the list measures the corpse and nothing else.
    for (size_t k = 0; k < cellOps.size(); k++) {
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
  //
  // ...and NO TWO BODIES ARE DRAWN FROM ONE RECORD. A brick record has exactly
  // one holder, and the whole lifecycle assumes it: an edit rewrites the block
  // in place and a release zeroes the record's dims and puts it back on the
  // free list. Two holders therefore means one of them is about to go INVISIBLE
  // (the other's release zeroed its dims) and the record is about to be handed
  // to a third body that is still very much alive, which is how a shattering
  // corpse used to take a living creature's torso with it. This is the shape
  // the count check below cannot see: two bodies sharing a record whose payload
  // happens to match one of their lattices disagree zero times.
  uint32_t bricks = 0, disagree = 0, sharedBricks = 0;
  std::string disagreeDetail;
  std::unordered_map<uint32_t, uint32_t> holdersOf;
  if (const MicroBodySet* set = c.debris.MicroSet()) {
    for (uint32_t bi = 0; bi < c.debris.BodyCount(); bi++) {
      const uint32_t model = c.debris.BodyMicroModel(bi);
      if (model == kMicroBodyNoModel || model >= set->models.size()) continue;
      if (++holdersOf[model] == 2) sharedBricks++;
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
  const bool drawn = disagree == 0 && sharedBricks == 0;

  RecordObserved("corpseBurnSharedBricks", (double)sharedBricks);
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
      "bricks, %u disagree with their lattice%s, %u shared by two bodies; %u settled back into the "
      "grid, %u bodies at t+%d, last body seen at t+%d with lowest y %.1f "
      "(ground %.0f); fall:%s",
      t.defName.c_str(), deathTick, cause.c_str(), alight0, spent0, bodies0,
      window, alight1, spent1, bodies1, stalledCount, litPieces,
      stalled.c_str(), debrisFireOps, debrisResidueOps, bricks, disagree,
      disagreeDetail.c_str(), sharedBricks, settled, bodiesMid, window / 2, lastBodyTick,
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
    ev.mat = mBlood;
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
    ev.mat = mBlood;
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
// corpse-wash: the dead take a coat and lose it, like the living
// ---------------------------------------------------------------------------
//
// Owner report, 2026-09-22: "water doesn't clean the stains off corpses".
// Nothing ran the contact pass on a corpse at all — Mob::StainTick walks a
// creature's limbs, and from the tick it dies those limbs are DebrisSystem
// bodies, so a corpse's coat froze at the moment of death. Now
// MobSystem::StainCorpses runs the living's own StainOneLimb / DryOneLimb over
// every dead-flesh body.
//
// Two claims, body-stain's phase 3 restated on the dead: the torso held in a
// blood pool is BLOODIED (it can take a coat at all), and the same torso then
// held in water is WASHED to at most bodyStainWashMaxFraction of that — the
// living's own threshold, because the rule is the same rule.
Status GateCorpseWash(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  constexpr int kInset = 440;
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, kInset));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  uint32_t mBlood = 0, mWater = 0;
  for (size_t i = 0; i < c.mats.size(); i++) {
    if (c.mats[i].name == "blood") mBlood = (uint32_t)i;
    if (c.mats[i].name == "water") mWater = (uint32_t)i;
  }
  if (!mBlood || !mWater) {
    detail = "blood or water is not a loaded material";
    return Status::Fail;
  }
  IVec3 pchunk{};
  const uint64_t id = SpawnTarget(c, t, kInset, pchunk);
  const MobDef& def = mobs.Defs()[t.defIndex];
  const uint64_t torso =
      id && def.rootLimb >= 0 ? mobs.LimbBody(id, def.rootLimb) : 0;
  if (!torso) {
    detail = "spawn refused, or the fixture has no root limb body";
    return Status::Fail;
  }
  // Killed the way corpse-intact kills: the root at zero is a death with every
  // joint kept, so the torso becomes one dead-flesh body and stays one.
  {
    const LimbAxis ax = MeasureLimb(mobs, id, def.rootLimb);
    mobs.Damage(torso, 1.0e6f, ax.anchor, 0.0f);
  }
  // By handle, and when a collider rebuild has replaced the handle, the
  // largest body -- the torso is 3-4x any other piece of a human.
  uint64_t torsoNow = torso;
  auto indexOf = [&](uint64_t h) -> int {
    (void)h;
    int best = -1;
    uint32_t bestN = 0;
    for (uint32_t i = 0; i < c.debris.BodyCount(); i++) {
      if (c.debris.BodyHandle(i) == torsoNow) return (int)i;
      const uint32_t n = c.debris.BodyVoxelCount(i);
      if (n > bestN) { bestN = n; best = (int)i; }
    }
    if (best >= 0) torsoNow = c.debris.BodyHandle((uint32_t)best);
    return best;
  };
  if (mobs.IsAlive(id) || indexOf(torso) < 0 || !c.debris.BodyIsDeadFlesh(torso)) {
    detail = Format("%s: the torso did not become dead flesh on death",
                    t.defName.c_str());
    mobs.Reset();
    c.debris.Reset();
    return Status::Fail;
  }
  const uint32_t coat0 = c.debris.BodyCoatCount((uint32_t)indexOf(torso), mBlood);

  // One real tick with liquid written round the torso (body-stain's
  // liquidTick, with the mirror centred on the corpse rather than a limb).
  uint32_t simTick = 33000;
  auto soak = [&](uint32_t mat, int ticks) -> uint32_t {
    for (int i = 0; i < ticks; i++) {
      std::vector<BrushOp> ops;
      std::vector<ParticleSpawn> spawns;
      std::vector<CellOp> cellOps;
      mobs.PreTick(simTick + 1, c.world, ops, cellOps, spawns);
      const int idx = indexOf(torso);
      IVec3 centre = pchunk;
      if (idx >= 0) {
        const Vec3 at = c.debris.BodyPosition((uint32_t)idx);
        const IVec3 b{ifloor(at.x), ifloor(at.y), ifloor(at.z)};
        centre = IVec3{b.x >> 4, b.y >> 4, b.z >> 4};
        for (int dy = -3; dy <= 3; dy++)
          for (int dz = -3; dz <= 3; dz++)
            for (int dx = -3; dx <= 3; dx++) {
              const IVec3 cc{b.x + dx, b.y + dy, b.z + dz};
              if (!c.world.CellInWindow(cc)) continue;
              if (cellOps.size() >= kMaxCellOpsPerTick) break;
              if (cc.y <= World::TerrainHeight(cc.x, cc.z, kDefaultSeed)) continue;
              cellOps.push_back({World::SlotCellIndex(cc), PackVoxNew(mat, 8u)});
            }
      }
      c.debris.QueueSupportEvents(c.world.Snap());
      c.debris.PreTick(simTick + 1, c.world, cellOps, spawns);
      ++simTick;
      SubmitTick(c.ctx, c.world, c.sim, simTick, kDefaultSeed, ops, {}, cellOps,
                 false, centre, true, false, spawns);
      c.ctx.WaitIdle();
      c.ctx.ProcessEvents();
      c.phys.Step(kTickDt);
      c.debris.PostStep();
      mobs.PostStep();
    }
    const int idx = indexOf(torso);
    return idx >= 0 ? c.debris.BodyCoatCount((uint32_t)idx, mBlood) : 0u;
  };
  const uint32_t pooled = soak(mBlood, 30);
  const uint32_t washed = soak(mWater, 60);
  const bool present = indexOf(torso) >= 0;

  RecordObserved("corpseWashPooled", (double)pooled);
  RecordObserved("corpseWashWashed", (double)washed);
  mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  const double washFrac = BaselineNumber("bodyStainWashMaxFraction", 0.5);
  const bool poolOk = pooled > coat0;
  const bool washOk = pooled > 0 && (double)washed <= washFrac * (double)pooled;
  const bool ok = present && poolOk && washOk;
  detail = Format(
      "%s torso (dead): blood coat %u -> %u in the pool (need more), -> %u "
      "after water (cap %.0f%% of the pool)%s",
      t.defName.c_str(), coat0, pooled, washed, washFrac * 100.0,
      present ? "" : "; the torso body was lost mid-test (handle changed?)");
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// corpse-crossheat / corpse-worn / corpse-splatter: three things the living
// had and the dead did not (owner report 2026-09-22)
// ---------------------------------------------------------------------------
//
// From the tick a creature dies its limbs are DebrisSystem bodies, and three
// mechanisms that lived only in the creature's own passes stopped:
//   * HEAT ACROSS A JOINT (Mob::BuildCrossLimbHeat) -- a burning torso on the
//     ground did not light the thighs jointed to it.
//   * ARMOUR -- a corpse in plate burnt and dissolved as if naked, because the
//     corpse burn pass had no worn-occlusion probe.
//   Both because a corpse burned in DebrisSystem::BurnBodies, a fork of the
//   living pass. It now burns in the living pass itself (MobSystem::
//   BurnCorpses -> BurnOneLimb), with the cross-heat builder and the shell
//   march shared with the living rather than copied.
//   * SPLATTER -- a burst of blood was replayed against living limbs only.
//     MobSystem::SplatterCorpses replays it against every dead-flesh body
//     through the same SplatterView the living use.
// Each claim is its own gate so each can be run alone (`--gate <name>`).
namespace {

// A dead human, lying where it fell: spawned, pinned, optionally dressed,
// settled, killed by its root at zero hp (a death with every joint kept, the
// way corpse-intact kills), and settled again. `step` is one whole tick --
// the world has to be submitted or a corpse has no ground (corpse-armor).
struct DeadFixture {
  Ctx& c;
  uint64_t id = 0;
  uint64_t torso = 0;
  IVec3 pchunk{};
  uint32_t tick = 0;
  std::string why;
  explicit DeadFixture(Ctx& ctx) : c(ctx) {}
  void Step(const std::function<void(std::vector<CellOp>&)>& fill = nullptr) {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> sp;
    std::vector<CellOp> cellOps;
    c.mobs.PreTick(tick + 1, c.world, ops, cellOps, sp);
    if (fill) fill(cellOps);
    c.debris.QueueSupportEvents(c.world.Snap());
    c.debris.PreTick(tick + 1, c.world, cellOps, sp);
    ++tick;
    SubmitTick(c.ctx, c.world, c.sim, tick, kDefaultSeed, ops, {}, cellOps,
               false, pchunk, true, false, sp);
    c.ctx.WaitIdle();
    c.ctx.ProcessEvents();
    c.phys.Step(kTickDt);
    c.debris.PostStep();
    c.mobs.PostStep();
  }
  // The torso body now: tracked by handle, and when a burn rebuild replaced
  // the handle, the largest body is the torso (it is 3-4x any other piece).
  int TorsoIndex() {
    int best = -1;
    uint32_t bestN = 0;
    for (uint32_t i = 0; i < c.debris.BodyCount(); i++) {
      if (c.debris.BodyHandle(i) == torso) return (int)i;
      // Dead flesh only: a strapped cuirass can outweigh the torso it covers.
      if (!c.debris.BodyIsDeadFlesh(c.debris.BodyHandle(i))) continue;
      const uint32_t n = c.debris.BodyVoxelCount(i);
      if (n > bestN) { bestN = n; best = (int)i; }
    }
    if (best >= 0) torso = c.debris.BodyHandle((uint32_t)best);
    return best;
  }
  bool Make(int inset, uint32_t tick0, const char* wear = nullptr) {
    MobSystem& mobs = c.mobs;
    const Target t = ChooseTarget(mobs, FixtureSite(c.world, inset));
    if (!t.valid()) { why = "no mob def with a severable limb that bleeds"; return false; }
    id = SpawnTarget(c, t, inset, pchunk);
    const MobDef& def = mobs.Defs()[t.defIndex];
    if (!id || def.rootLimb < 0 || !mobs.LimbBody(id, def.rootLimb)) {
      why = "spawn refused";
      return false;
    }
    mobs.SetMobBehavior(id, "dummy");
    tick = tick0;
    if (wear) {
      Mob* mob = mobs.FindMobById(id);
      const ItemDef* it = c.items.At(c.items.Find(wear));
      int home = -1;
      if (it)
        for (int sl = 0; sl < kEquipSlotCount; sl++)
          if (EquipSlotAccepts(sl, it->kind)) { home = sl; break; }
      if (!mob || !it || home < 0 || !mob->WearItem(it, home)) {
        why = Format("could not dress the fixture in '%s'", wear);
        return false;
      }
    }
    for (int i = 0; i < 8; i++) Step();
    torso = mobs.LimbBody(id, def.rootLimb);
    const LimbAxis ax = MeasureLimb(mobs, id, def.rootLimb);
    mobs.Damage(torso, 1.0e6f, ax.anchor, 0.0f);
    if (mobs.IsAlive(id) || !c.debris.BodyIsDeadFlesh(torso)) {
      why = "the root at zero hp did not leave a dead-flesh torso";
      return false;
    }
    for (int i = 0; i < 20; i++) Step();
    return TorsoIndex() >= 0;
  }
};

uint32_t MatIdOf(Ctx& c, const char* n) {
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == n) return (uint32_t)i;
  return 0;
}

}  // namespace

// ---- corpse-crossheat ------------------------------------------------------
// A torso set alight on its own lattice, with no world fire, must set the rest
// of the corpse alight -- the property the owner cares about -- and the dead
// must be burning in the living limb pass with its cross-joint heat built.
//
// NOT A DIFFERENTIAL, and that is a measured result rather than an omission.
// The first version ran the living gate's two arms (crossLimbPct shipped vs 0)
// and the CONTROL arm burned 808 voxels of the other pieces in 150 ticks: a
// corpse LIES DOWN, so the flame its torso emits into the grid drifts straight
// over the pieces beside it, and BurnOneLimb reads a sibling's heat only where
// the grid has nothing to say. On a standing creature the flame rises away from
// the legs and cross heat is the only path (selftest_mob's "heat across a
// joint" measures that); on a corpse the grid path already carries it, and
// cross heat got 0 faces in both arms. A differential that cannot be won is not
// a test. What is asserted instead is the spread and the wiring.
Status GateCorpseCrossheat(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  const uint32_t mSkin = MatIdOf(c, "skin"), mFlesh = MatIdOf(c, "flesh"),
                 mBurning = MatIdOf(c, "flesh_burning");
  const uint32_t touchedMats[] = {mBurning, MatIdOf(c, "flesh_cooked"),
                                  MatIdOf(c, "flesh_charred"),
                                  MatIdOf(c, "flesh_cinder")};
  if (!mSkin || !mBurning) {
    detail = "skin / flesh_burning missing from materials.json";
    return Status::Fail;
  }
  PrepareWorld(c);
  DeadFixture f(c);
  if (!f.Make(460, 57000)) {
    detail = f.why;
    return Status::Fail;
  }
  // What the OTHER pieces have been touched by: every voxel of theirs now in
  // the burn chain. Excluding the torso (the source) is the whole point.
  auto othersTouched = [&](uint32_t* pieces) {
    const int ti = f.TorsoIndex();
    uint32_t n = 0, lit = 0;
    for (uint32_t i = 0; i < c.debris.BodyCount(); i++) {
      if ((int)i == ti || !c.debris.BodyIsDeadFlesh(c.debris.BodyHandle(i)))
        continue;
      uint32_t here = 0;
      for (uint32_t m : touchedMats)
        if (m) here += c.debris.BodyMaterialCount(i, m);
      n += here;
      if (here) lit++;
    }
    if (pieces) *pieces = lit;
    return n;
  };
  const uint32_t base = othersTouched(nullptr);
  c.mobs.ResetBurnStats();
  const int kTicks = 150, kRelight = 20;
  uint32_t relit = 0, peak = 0, peakPieces = 0;
  for (int i = 0; i < kTicks; i++) {
    if (i % kRelight == 0) {
      const int ti = f.TorsoIndex();
      if (ti >= 0) {
        const uint64_t h = c.debris.BodyHandle((uint32_t)ti);
        uint32_t lit = c.debris.RewriteBodyMaterial(h, mSkin, mBurning, 200);
        if (lit < 200 && mFlesh)
          lit += c.debris.RewriteBodyMaterial(h, mFlesh, mBurning, 200 - lit);
        relit += lit;
      }
    }
    f.Step();
    uint32_t pieces = 0;
    const uint32_t now = othersTouched(&pieces);
    peak = std::max(peak, now > base ? now - base : 0u);
    peakPieces = std::max(peakPieces, pieces);
  }
  const MobSystem::BurnStats st = c.mobs.Burn();
  const uint32_t bodies = c.debris.BodyCount();
  c.mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  RecordObserved("corpseCrossheatTouched", (double)peak);
  const double minTouched = BaselineNumber("corpseCrossheatMinTouched", 100.0);
  // The spread, and proof the living pass is what burned the dead: it built a
  // cross-heat snapshot for the corpse (crossCells), which nothing but
  // MobSystem::BurnCorpses does for a debris body.
  const bool ok = relit > 0 && (double)peak >= minTouched && st.crossCells > 0;
  detail = Format(
      "torso re-lit %u voxels, no world fire: other pieces touched by fire %u "
      "(need %.0f) across %u pieces of %u bodies; living burn pass on the dead: "
      "%u cross-heat cells built, %u faces read one, %u rules armed by it alone",
      relit, peak, minTouched, peakPieces, bodies, st.crossCells, st.crossFaces,
      st.crossOnly);
  return ok ? Status::Pass : Status::Fail;
}

// ---- corpse-worn ----------------------------------------------------------
// The same acid bath round two corpses' torsos, one wearing the iron cuirass
// and one bare. Steel carries no tag:dissolvable, so on the living the acid
// eats nothing under the plate (armor-react's claim); on the dead it ate the
// torso as if the plate were not there. The claim is the difference in TORSO
// flesh lost, and that the probe is what made it (shielded faces > 0).
Status GateCorpseWorn(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  const uint32_t mAcid = MatIdOf(c, "acid");
  if (!mAcid) {
    detail = "acid missing from materials.json";
    return Status::Fail;
  }
  if (c.items.Find("iron_cuirass") < 0) {
    detail = "no iron_cuirass item to dress the fixture in";
    return Status::Skip;
  }
  struct Arm {
    uint32_t v0 = 0, v1 = 0, threats = 0, shielded = 0, seeds = 0;
  } arms[2];
  std::string why;
  const int kTicks = 60;
  for (int a = 0; a < 2 && why.empty(); a++) {
    PrepareWorld(c);
    DeadFixture f(c);
    if (!f.Make(500, 59000, a == 0 ? "iron_cuirass" : nullptr)) { why = f.why; break; }
    int ti = f.TorsoIndex();
    arms[a].v0 = ti >= 0 ? c.debris.BodyVoxelCount((uint32_t)ti) : 0;
    c.mobs.ResetWornStats();
    for (int i = 0; i < kTicks; i++) {
      f.Step([&](std::vector<CellOp>& cellOps) {
        const int k = f.TorsoIndex();
        if (k < 0) return;
        const Vec3 at = c.debris.BodyPosition((uint32_t)k);
        const IVec3 b{ifloor(at.x), ifloor(at.y), ifloor(at.z)};
        for (int dy = -3; dy <= 3; dy++)
          for (int dz = -3; dz <= 3; dz++)
            for (int dx = -3; dx <= 3; dx++) {
              const IVec3 cc{b.x + dx, b.y + dy, b.z + dz};
              if (!c.world.CellInWindow(cc)) continue;
              if (cellOps.size() >= kMaxCellOpsPerTick) return;
              if (cc.y <= World::TerrainHeight(cc.x, cc.z, kDefaultSeed)) continue;
              cellOps.push_back({World::SlotCellIndex(cc), PackVoxNew(mAcid, 8u)});
            }
      });
    }
    ti = f.TorsoIndex();
    arms[a].v1 = ti >= 0 ? c.debris.BodyVoxelCount((uint32_t)ti) : 0;
    arms[a].threats = c.mobs.Worn().nbrThreats;
    arms[a].shielded = c.mobs.Worn().nbrSubstituted;
    arms[a].seeds = c.mobs.Worn().seedsBlocked;
    c.mobs.Reset();
    c.debris.Reset();
  }
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  if (!why.empty()) {
    detail = why;
    return Status::Fail;
  }
  auto lost = [](const Arm& x) { return x.v0 > x.v1 ? x.v0 - x.v1 : 0u; };
  RecordObserved("corpseWornLostArmoured", (double)lost(arms[0]));
  RecordObserved("corpseWornLostBare", (double)lost(arms[1]));
  // The bare torso must actually be eaten (or the bath proved nothing), the
  // armoured one must lose at most half of that, and the shell must be what
  // answered.
  const bool ok = lost(arms[1]) > 20 &&
                  (arms[0].shielded > 0 || arms[0].seeds > 0) &&
                  lost(arms[0]) * 2 <= lost(arms[1]);
  detail = Format(
      "acid bath %d ticks on a dead torso: in the iron cuirass %u -> %u (lost "
      "%u; shell answered %u of %u threatening faces, blocked %u contact "
      "seeds), bare %u -> %u (lost %u); need armoured <= half of bare",
      kTicks, arms[0].v0, arms[0].v1, lost(arms[0]), arms[0].shielded,
      arms[0].threats, arms[0].seeds, arms[1].v0, arms[1].v1, lost(arms[1]));
  return ok ? Status::Pass : Status::Fail;
}

// ---- corpse-splatter ------------------------------------------------------
// body-stain's burst, aimed at a corpse's torso instead of a standing
// creature's: a splash thrown at 6 m/s from a metre away must mark the dead
// body it lands on.
Status GateCorpseSplatter(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  const uint32_t mBlood = MatIdOf(c, "blood");
  if (!mBlood) {
    detail = "blood missing from materials.json";
    return Status::Fail;
  }
  PrepareWorld(c);
  DeadFixture f(c);
  if (!f.Make(420, 61000)) {
    detail = f.why;
    return Status::Fail;
  }
  int ti = f.TorsoIndex();
  const uint32_t before = c.debris.BodyCoatCount((uint32_t)ti, mBlood);
  const Vec3 at = c.debris.BodyPosition((uint32_t)ti);
  SplatterEvent ev;
  ev.origin = at + Vec3{MetresToCells(1.0f), 0.3f, 0.0f};
  ev.axis = Vec3{-1.0f, 0.0f, 0.0f};
  ev.cone = 0.35f;
  ev.reach = MetresToCells(2.0f);
  ev.speed = MetresToCells(6.0f);
  ev.life = 70;
  ev.count = 24;
  ev.mat = mBlood;
  ev.amount = 6;
  ev.tick = f.tick;
  ev.seed = 0xC0A5Eu;
  c.mobs.QueueSplatter(ev);
  for (int i = 0; i < 4; i++) f.Step();
  ti = f.TorsoIndex();
  const uint32_t after = ti >= 0 ? c.debris.BodyCoatCount((uint32_t)ti, mBlood) : 0u;
  c.mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  RecordObserved("corpseSplatterMarked", (double)(after > before ? after - before : 0));
  const bool ok = after > before;
  detail = Format("a 24-droplet burst at a dead torso from 1 m: blood coat %u -> %u",
                  before, after);
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// body-coat: what is ON a body is a SUBSTANCE, and it behaves like one
// ---------------------------------------------------------------------------
//
// `body-stain` proves blood ARRIVES on a creature. This proves it is a
// material once it is there, which is the whole of what the coat word bought
// (a 3-bit palette slot could say "blood" but never WHICH blood, so nothing
// could look the substance up).
//
// Three claims, one fixture — the sealed stone room body-stain uses, for the
// same reason: a flat known floor with nothing that can flow, sink or fall.
//   * THE LEDGER NAMES THE SUBSTANCE. After a deep cut, the limb's coat ledger
//     (mob.h LimbCoat) names the creature's own blood as its heaviest coat,
//     the body's ledger sums to at least that limb's, and a tag no material
//     declares reads exactly zero.
//   * IT DRIES AT THE MATERIAL'S OWN RATE. Blood authors coat.decay 20 s per
//     level, so at the default scale a cut is still bloody a hundred ticks
//     later; at a scale that makes the period two ticks it walks itself clean.
//     Both arms run on the SAME cut, decay-off first, so the second is
//     measured against a number the first proved is stable.
//   * A COAT CAN GO BACK ON THE GROUND. Mob::DepositCoat puts one micro
//     droplet of the substance inside a floor cell; the ordinary particle
//     kernel resolves it into that cell's stain bits, so a tracked footprint
//     is a real world stain and not a second mechanism.
//   * ...AND A FOOTFALL IS WHAT SPENDS IT. Mob::ShedCoat, at the plant, puts
//     that deposit under the sole and takes the same amount OFF the foot: the
//     floor gains blood it did not have and the limb's ledger falls. Both
//     halves, because a print that only adds is a duplicator (rule 2) and one
//     that only subtracts is a leak.
Status GateBodyCoat(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  constexpr int kInset = 300;
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, kInset));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  uint32_t mStone = 0, mBlood = 0;
  for (size_t i = 0; i < c.mats.size(); i++) {
    if (c.mats[i].name == "stone") mStone = (uint32_t)i;
    if (c.mats[i].name == "blood") mBlood = (uint32_t)i;
  }

  // THE ROOM, exactly as body-stain builds it (and the long note there is the
  // argument for every part of it, including why it is written a few ticks
  // after PrepareWorld rather than on the tick right after).
  const IVec3 site0 = FixtureSite(c.world, kInset);
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
      for (uint32_t k = 0; k < 6; k++) bareTick(33990u + k, none);
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
    bareTick(33997u, cellOps);
    std::vector<CellOp> none;
    bareTick(33998u, none);
  }

  IVec3 pchunk{};
  const uint64_t id = SpawnTarget(c, t, kInset, pchunk);
  if (!id) {
    detail = "spawn refused";
    return Status::Fail;
  }
  // Pinned, for body-stain's reason: every claim is about a creature standing
  // where the fixture put it.
  const bool pinned = mobs.SetMobBehavior(id, "dummy");
  const MobDef& def = mobs.Defs()[t.defIndex];
  const int root = def.rootLimb;
  // The substance the cut smear is MADE of, resolved the way Mob::StainWound
  // resolves it, so the ledger claim is about the rule and not about a name.
  const uint32_t coatMat =
      mobs.StainTypeOf(def.woundMat) ? def.woundMat : def.bleedMat;
  // COUNT ABOVE THE SUBSTANCE'S OWN FLOOR (2026-09-19). Blood authors
  // coat.decayFloor 1 (materials.json, 28ebf8d 2026-09-16: a coat dries to a
  // faint residual, never to nothing) and Mob::StainTick skips a voxel at or
  // under it -- so a count of voxels at amount >= 1 can NEVER fall, and the
  // drying claim below was unpassable as written (43 -> 43 over 400 ticks at
  // 2 ticks/level: every voxel had walked down to 1 and stopped, exactly as
  // authored). Every stain count in this gate is therefore of voxels ABOVE
  // the floor, read off the material so a re-authored floor moves the gate
  // with it. The claim is what it was: at the authored rate the smear holds,
  // and at two ticks per level it walks itself down to the residual.
  const uint32_t coatFloor =
      coatMat < c.mats.size() ? c.mats[coatMat].coatDecayFloor : 0u;
  const uint32_t aboveFloor = coatFloor + 1u;

  // ONE MONOTONIC TICK COUNTER for the whole gate, and it is load-bearing
  // rather than tidy: the drying rule fires on `tick % period == 0` and the
  // splatter queue retires on `e.tick + 1 < tick`, so a phase that stepped the
  // clock backwards (which several gates here do, harmlessly, because nothing
  // they test reads the tick) would both mis-time the control arm and strand
  // spent bursts in a 64-entry queue.
  uint32_t simTick = 34000;
  // A tick with no world submit: the pose, the bleed, the stain pass and — at
  // its tail — the coat ledger and the drying sweep. Everything below except
  // the deposit is measured through this.
  auto poseTick = [&]() {
    ++simTick;
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    mobs.PreTick(simTick, c.world, ops, cellOps, spawns);
    c.phys.Step(kTickDt);
    mobs.PostStep();
  };
  // A tick that DOES submit, for the deposit (the particle kernel has to run).
  auto worldTick = [&]() {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    mobs.PreTick(simTick + 1, c.world, ops, cellOps, spawns);
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
  for (int i = 0; i < 20; i++) worldTick();  // let it find the floor

  // ---- 1. the cut, and the ledger it leaves --------------------------------
  const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
  bool hit = false;
  {
    std::vector<ParticleSpawn> spawns;
    hit = CutOnce(mobs, c.world, id, t.limb, ax, ax.reach * 0.5f, 0.9f, 1.0f,
                  0x5C0A7u, spawns);
  }
  poseTick();  // the first recount runs the moment anything is dirty
  const bool attached = mobs.LimbBody(id, t.limb) != 0;
  LimbCoat limbLedger{}, bodyLedger{};
  if (const LimbCoat* lc = mobs.LimbCoatOf(id, t.limb)) limbLedger = *lc;
  bodyLedger = mobs.BodyCoat(id);
  const float noSuchTag = mobs.CoatTagFraction(id, "nonexistent", nullptr);
  const bool ledgerOk = hit && attached && coatMat != 0 &&
                        limbLedger.top[0].mat == coatMat &&
                        limbLedger.Frac() > 0.0f &&
                        bodyLedger.sumAmt >= limbLedger.sumAmt &&
                        noSuchTag == 0.0f;

  // ---- 2. it does NOT dry at the authored rate over a hundred ticks --------
  // Blood is 20 s per level, i.e. a 600-tick period at the shipped scale, and
  // the window below must not CONTAIN a multiple of 600 — a legitimate decay
  // sweep landing inside the control arm would be the rule working and the arm
  // still has to be flat. The counter is at 34021 here and 34200 is the next
  // multiple, so 150 ticks clear it; the assertion after it is what would
  // catch this drifting.
  const uint32_t cutStain = mobs.LimbStainCount(id, t.limb, aboveFloor);
  const uint32_t holdFrom = simTick;
  constexpr uint32_t kHoldTicks = 150;
  for (uint32_t i = 0; i < kHoldTicks && mobs.LimbBody(id, t.limb); i++)
    poseTick();
  const bool holdWindowClean = (holdFrom / 600u) == (simTick / 600u);
  const uint32_t heldStain =
      mobs.LimbBody(id, t.limb) ? mobs.LimbStainCount(id, t.limb, aboveFloor)
                                : 0u;
  const double holdMin = BaselineNumber("bodyCoatHoldMinFraction", 0.9);
  const bool holdOk = cutStain > 0 && holdWindowClean &&
                      (double)heldStain >= holdMin * (double)cutStain;

  // ---- 3. and a coat can be put back on the ground -------------------------
  // BEFORE the drying arm, not after: that arm runs hundreds of ticks with an
  // open wound, and a creature that bled out in the middle of it would take
  // the deposit claim down with it for no reason of its own.
  //
  // The floor of the room is stone three deep, so this cell is a solid the
  // fixture itself wrote and its material is known without a survey.
  const IVec3 floorCell{site0.x + 3, site0.y - 1, site0.z + 3};
  uint32_t depBefore = 0, depAfter = 0;
  bool queued = false;
  if (mStone && mBlood && c.world.CellInWindow(floorCell)) {
    const IVec3 cc{floorCell.x >> 4, floorCell.y >> 4, floorCell.z >> 4};
    const uint32_t at =
        ((uint32_t)(floorCell.z & 15) * kChunk + (uint32_t)(floorCell.y & 15)) *
            kChunk + (uint32_t)(floorCell.x & 15);
    std::vector<uint32_t> chunk(kChunkVol, 0);
    ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex(cc), 1, chunk.data(),
                   "coatFloorPre");
    depBefore = chunk[at];
    queued = mobs.DepositCoatOn(id, mBlood, floorCell, simTick);
    // Three: the spawn is drained by the NEXT PreTick, the kernel appends it,
    // and integrate/resolve get their turn. Slack rather than a fence.
    for (int i = 0; i < 3; i++) worldTick();
    ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex(cc), 1, chunk.data(),
                   "coatFloorPost");
    depAfter = chunk[at];
  }
  const bool depositOk = queued && (depBefore & 0xFFFu) == mStone &&
                         (depAfter & 0xFFFu) == mStone &&
                         VoxStainType(depAfter) == mobs.StainTypeOf(mBlood) &&
                         VoxStainAmt(depAfter) > 0;

  // ---- 3b. A BLOODY FOOT LEAVES A PRINT WHERE IT LANDS (P2) ----------------
  // The claim above proves the DEPOSIT. This proves the FOOTFALL: the plant
  // itself moves the substance off the sole and onto the cells the sole
  // covers. Two halves, because either alone is satisfiable by a bug —
  //   * the floor under the foot gains blood it did NOT have before the plant
  //     (measured as a delta, because the cut limb has been dripping onto this
  //     floor for 150 ticks and a bare "is there blood here" reads yes), and
  //   * the foot's own ledger sums to LESS afterwards. A print that only adds
  //     is a duplicator, and an infinite one — a creature would paint the
  //     whole map from one wound (rule 2).
  //
  // BEFORE the drying arm, for claim 3's reason: that arm runs hundreds of
  // ticks with an open wound and may end with a corpse.
  //
  // The foot is bloodied through MobSystem::SoakLimb rather than by cutting
  // it, so this phase has NO other consequence: no wound, no bleed, no
  // splatter onto the limb claims 2 and 4 are measured on.
  int footLimb = -1;
  for (size_t li = 0; li < def.limbs.size() && footLimb < 0; li++)
    if (def.limbs[li].tag == "foot" && mobs.LimbBody(id, (int)li))
      footLimb = (int)li;
  for (size_t li = 0; li < def.limbs.size() && footLimb < 0; li++)
    if (def.limbs[li].tag == "leg" && mobs.LimbBody(id, (int)li))
      footLimb = (int)li;
  if (footLimb < 0) {  // untagged rig: the limb whose live pose sits lowest
    float best = 1e30f;
    for (size_t li = 0; li < def.limbs.size(); li++) {
      float lo = 0.0f, hi = 0.0f;
      if (mobs.LimbBody(id, (int)li) &&
          mobs.LimbStainWorldYRange(id, (int)li, 0, lo, hi) && lo < best) {
        best = lo;
        footLimb = (int)li;
      }
    }
  }
  std::string footName = footLimb >= 0 ? def.limbs[footLimb].name : "-";
  uint32_t footPainted = 0, shedDroplets = 0, shedTries = 0;
  uint32_t footSumPainted = 0, footSumBefore = 0, footSumAfter = 0;
  uint32_t printCells = 0, printAmt = 0;
  int printY = 0;
  if (footLimb >= 0 && mBlood) {
    footPainted = mobs.SoakLimb(id, footLimb, mBlood, kBodyStainAmtMax, simTick);
    if (const LimbCoat* lc = mobs.LimbCoatOf(id, footLimb))
      footSumPainted = lc->sumAmt;
    const Vec3 footAt = mobs.LimbVoxelPos(id, footLimb, 0);
    // The cells the print can land in: the sole's 3x3 column footprint on the
    // topmost SOLID cell under the foot. Found through the CPU mirror because
    // that is the same picture Mob::GroundHeightAt reads inside ShedCoat, so
    // this is the set of cells the shed could have chosen and not a guess.
    auto mirrorWord = [&](IVec3 cc) -> uint32_t {
      const CachedChunk* k =
          c.world.Cached(IVec3{cc.x >> 4, cc.y >> 4, cc.z >> 4});
      if (!k || k->voxels.size() != kChunkVol) return 0u;
      return k->voxels[(((uint32_t)cc.z & 15u) * kChunk +
                        ((uint32_t)cc.y & 15u)) * kChunk +
                       ((uint32_t)cc.x & 15u)];
    };
    const int fx = ifloor(footAt.x), fz = ifloor(footAt.z);
    printY = INT32_MIN;
    for (int dy = 2; dy >= -4; dy--) {
      const IVec3 cc{fx, ifloor(footAt.y) + dy, fz};
      if (!c.world.CellInWindow(cc)) continue;
      if ((mirrorWord(cc) & 0xFFFu) != 0u) { printY = cc.y; break; }
    }
    std::vector<IVec3> cells;
    if (printY != INT32_MIN)
      for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) {
          const IVec3 cc{fx + dx, printY, fz + dz};
          if (c.world.CellInWindow(cc)) cells.push_back(cc);
        }
    // Authoritative words for those cells, one readback per distinct chunk
    // (a 3x3 column straddles at most four).
    std::vector<uint32_t> chunk(kChunkVol, 0);
    auto readCells = [&](std::vector<uint32_t>& out) {
      out.assign(cells.size(), 0u);
      std::vector<IVec3> done;
      for (size_t i = 0; i < cells.size(); i++) {
        const IVec3 ch{cells[i].x >> 4, cells[i].y >> 4, cells[i].z >> 4};
        bool have = !done.empty() && done.back().x == ch.x &&
                    done.back().y == ch.y && done.back().z == ch.z;
        if (!have) {
          ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex(ch), 1,
                         chunk.data(), "coatPrint");
          done.assign(1, ch);
        }
        out[i] = chunk[(((uint32_t)cells[i].z & 15u) * kChunk +
                        ((uint32_t)cells[i].y & 15u)) * kChunk +
                       ((uint32_t)cells[i].x & 15u)];
      }
    };
    std::vector<uint32_t> before, after;
    readCells(before);
    // THE ROLL IS PER MILLE AND PER TICK, so a single plant can legitimately
    // print nothing (blood sheds at 400). Up to eight plants, one per tick,
    // and the gate stops at the first that puts something down — the same
    // thing a walk does, at a pace no faster than a real one.
    for (; shedTries < 8 && shedDroplets == 0; shedTries++) {
      // The ledger is re-read on EVERY attempt, so the before/after pair
      // brackets the successful plant alone: the drying sweep also takes
      // levels off, and a delta measured across the whole loop would credit
      // the footfall with whatever evaporated while it was rolling.
      mobs.RecountCoatOn(id, simTick);
      if (const LimbCoat* lc = mobs.LimbCoatOf(id, footLimb))
        footSumBefore = lc->sumAmt;
      shedDroplets = mobs.ShedCoatOn(id, footLimb, footAt, simTick, c.world);
      if (shedDroplets == 0) worldTick();
    }
    // Read the ledger back BEFORE any further tick: the drying sweep also
    // takes levels off, and this half of the claim is about the plant.
    mobs.RecountCoatOn(id, simTick);
    if (const LimbCoat* lc = mobs.LimbCoatOf(id, footLimb))
      footSumAfter = lc->sumAmt;
    // Three ticks, for claim 3's reason: drain, append, integrate + resolve.
    for (int i = 0; i < 3; i++) worldTick();
    readCells(after);
    const uint32_t bloodType = mobs.StainTypeOf(mBlood);
    for (size_t i = 0; i < cells.size(); i++) {
      const bool wasBlood =
          VoxStainType(before[i]) == bloodType && VoxStainAmt(before[i]) > 0;
      const bool isBlood =
          VoxStainType(after[i]) == bloodType && VoxStainAmt(after[i]) > 0;
      if (isBlood && (!wasBlood || VoxStainAmt(after[i]) > VoxStainAmt(before[i]))) {
        printCells++;
        printAmt += VoxStainAmt(after[i]);
      }
    }
  }
  const bool printOk = footLimb >= 0 && footPainted > 0 && shedDroplets > 0 &&
                       printCells > 0 && footSumAfter < footSumBefore;

  // ---- 4. ...and at a scale that makes the period two ticks, it DOES dry ---
  // decayScale DIVIDES the authored seconds, so 300 turns blood's 20 s per
  // level into 2 ticks. Restored below whatever happens: tuning is global.
  // Measured against `heldStain` — the number the control arm above just
  // proved is stable — so the two arms share one denominator.
  const Tuning savedTune = CurrentTuning();
  {
    Tuning fast = savedTune;
    fast.coat.decayScale = 300.0f;  // 20 s / 300 = 2 ticks per amount level
    SetCurrentTuning(fast);
  }
  constexpr uint32_t kDecayCap = 400;
  const double decayMax = BaselineNumber("bodyCoatDecayMaxFraction", 0.1);
  uint32_t decayedStain =
      mobs.LimbBody(id, t.limb) ? mobs.LimbStainCount(id, t.limb, aboveFloor)
                                : 0u;
  uint32_t ranTicks = 0;
  for (; ranTicks < kDecayCap && mobs.LimbBody(id, t.limb); ranTicks++) {
    poseTick();
    decayedStain = mobs.LimbStainCount(id, t.limb, aboveFloor);
    if ((double)decayedStain <= decayMax * (double)heldStain) break;
  }
  SetCurrentTuning(savedTune);
  const bool decayOk =
      heldStain > 0 && (double)decayedStain <= decayMax * (double)heldStain;

  RecordObserved("bodyCoatLimbFrac", (double)limbLedger.Frac());
  RecordObserved("bodyCoatLimbSum", (double)limbLedger.sumAmt);
  RecordObserved("bodyCoatBodySum", (double)bodyLedger.sumAmt);
  RecordObserved("bodyCoatCutStained", (double)cutStain);
  RecordObserved("bodyCoatHeldStained", (double)heldStain);
  RecordObserved("bodyCoatDecayedStained", (double)decayedStain);
  RecordObserved("bodyCoatDecayTicks", (double)ranTicks);
  RecordObserved("bodyCoatDepositAmount", (double)VoxStainAmt(depAfter));
  RecordObserved("bodyCoatFootPainted", (double)footPainted);
  RecordObserved("bodyCoatShedTries", (double)shedTries);
  RecordObserved("bodyCoatShedDroplets", (double)shedDroplets);
  RecordObserved("bodyCoatPrintCells", (double)printCells);
  RecordObserved("bodyCoatPrintAmount", (double)printAmt);
  RecordObserved("bodyCoatFootSumBefore", (double)footSumBefore);
  RecordObserved("bodyCoatFootSumAfter", (double)footSumAfter);

  mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  const bool ok = ledgerOk && holdOk && decayOk && depositOk && printOk;
  detail = Format(
      "%s/%s%s: ledger top = mat %u (want %u, blood %u) over %u voxels, "
      "frac %.4f, body sum %u >= limb sum %u, unknown tag %.2f; "
      "stained above the authored decayFloor %u: %u -> %u over %u ticks at "
      "the authored 20 s/level%s "
      "(floor %.0f%%), then -> %u over %u ticks at 2 ticks/level "
      "(cap %.0f%%); deposit %s on stone at (%d,%d,%d): word %08x -> %08x, "
      "stain type %u amount %u; footfall on %s: %u voxels soaked (ledger %u), "
      "%u droplet(s) down after %u plant(s), %u floor cell(s) at y%d newly "
      "bloodied (amount %u), sole ledger %u -> %u",
      t.defName.c_str(), t.limbName.c_str(), pinned ? "" : " (NOT pinned)",
      limbLedger.top[0].mat, coatMat, mBlood, limbLedger.voxels,
      (double)limbLedger.Frac(), bodyLedger.sumAmt, limbLedger.sumAmt,
      (double)noSuchTag, coatFloor, cutStain, heldStain, kHoldTicks,
      holdWindowClean ? "" : " (WINDOW CROSSED A DECAY PERIOD)", holdMin * 100.0,
      decayedStain, ranTicks, decayMax * 100.0, queued ? "queued" : "REFUSED",
      floorCell.x, floorCell.y, floorCell.z, depBefore, depAfter,
      VoxStainType(depAfter), VoxStainAmt(depAfter), footName.c_str(),
      footPainted, footSumPainted, shedDroplets, shedTries, printCells, printY,
      printAmt, footSumBefore, footSumAfter);
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

// ---------------------------------------------------------------------------
// wound-heal: the blood dries back to flesh, it does not eat the limb
// ---------------------------------------------------------------------------
//
// THE BUG THIS PINS. `StainWound` rewrites the flesh around a cut to the
// creature's wound MATERIAL, and the body burn pass runs the ordinary authored
// reaction table over a limb's lattice. Blood's rule in reactions.json is
// `decay -> air` at 8 per-mille a tick, written so a pool on the ground dries
// up and its chunk goes back to sleep (rule 2) -- and it applied, unchanged, to
// blood INSIDE a limb. So one sword cut opened a hole that then ate itself
// outward at a ~3 s half-life, and a creature standing still after a single
// blow shed the limb with no further hits. Owner report, 2026-09-14: "those
// blood voxels ... entirely evaporate revealing the below structure, which
// causes limbs to fall off".
//
// TWO ARMS, because the claim is a DIFFERENCE and one arm cannot state it:
//
//   heal  (gore.woundHeals on)   the soak dries -- and the limb does not lose
//                                a single voxel doing it.
//   rot   (gore.woundHeals off)  the soak dries by LEAVING. This is the old
//                                behaviour, it is the undead setting, and it
//                                is also the gate's proof that the mechanism
//                                under test ran at all: if the rot arm loses
//                                nothing, the decay never fired in this
//                                fixture and the heal arm's clean bill of
//                                health is worth nothing (a fixture that
//                                measures itself).
//
// THE LIMB HAS TO BE AWAKE, and saying so is half of what this gate records.
// The body pass will not look at a limb with no burn index, and it is
// `BuildBurnIndex` that notices a self-decaying material at all -- it seeds
// the front from every self-active voxel and sets `alight` from that, which
// is how a soak with no fire anywhere near it comes to be rolling a decay in
// the first place. In the game the index is built by the drip's own splatter
// replay and by the contact stain; in this CPU-only fixture on open ground,
// with no pool to stand in and no burst that reaches the cut, neither happens
// and the soak simply sits there (measured: 0 of 92 voxels moved in 600
// ticks, both arms). So the gate builds the index through the existing test
// entry point -- `IgniteLimb` with a count of ZERO, which lights nothing and
// whose only effect is that index. Rule 2 is not being dodged: the wake is
// real in the game, this fixture is just too quiet to produce it.
Status GateWoundHeal(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  if (!mobs.BurnTablesReady()) {
    detail = "burn tables not loaded";
    return Status::Fail;
  }
  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 360));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  const uint32_t woundMat = mobs.Defs()[t.defIndex].woundMat;
  if (!woundMat) {
    detail = Format("%s has no wound material", t.defName.c_str());
    return Status::Skip;
  }

  // Long enough for the soak to be most of the way gone in BOTH arms: at the
  // authored 8 per-mille the half-life is ~87 ticks, at the healing arm's
  // halved rate ~173, so 600 ticks leaves under a tenth either way. Short
  // enough that the creature does not bleed out from the one cut.
  constexpr int kTicks = 600;
  const Tuning saved = CurrentTuning();

  struct Arm {
    uint32_t artCut = 0, artEnd = 0;    // limb voxels after the cut / at rest
    uint32_t soakCut = 0, soakEnd = 0;  // wound-material voxels, same two ticks
    bool attached = false, alive = false, cut = false;
  };
  auto run = [&](bool heals) -> Arm {
    Tuning tun = saved;
    tun.gore.woundHeals = heals;
    SetCurrentTuning(tun);
    Arm a;
    IVec3 pchunk{};
    const uint64_t id = SpawnTarget(c, t, 360, pchunk);
    if (!id) return a;
    const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
    std::vector<ParticleSpawn> spawns;
    a.cut = CutOnce(mobs, c.world, id, t.limb, ax, ax.reach * 0.5f, 0.6f, 1.0f,
                    0x5EA1u, spawns);
    spawns.clear();
    if (!mobs.LimbBody(id, t.limb)) return a;  // severed outright: no subject
    a.artCut = mobs.LimbArtVoxelCount(id, t.limb);
    a.soakCut = mobs.LimbMaterialCount(id, t.limb, woundMat);
    // Count 0: nothing is set alight, the index is built, and the soak in it
    // is what makes the limb `alight` (see the note above).
    mobs.IgniteLimb(id, t.limb, 0, 0);
    uint32_t tick = 50000;
    for (int i = 0; i < kTicks && mobs.IsAlive(id); i++) {
      std::vector<BrushOp> ops;
      std::vector<CellOp> cellOps;
      spawns.clear();
      mobs.PreTick(tick++, c.world, ops, cellOps, spawns);
      c.phys.Step(kTickDt);
      mobs.PostStep();
    }
    a.alive = mobs.IsAlive(id);
    a.attached = mobs.LimbBody(id, t.limb) != 0;
    if (a.attached) {
      a.artEnd = mobs.LimbArtVoxelCount(id, t.limb);
      a.soakEnd = mobs.LimbMaterialCount(id, t.limb, woundMat);
    }
    return a;
  };
  const Arm heal = run(true);
  const Arm rot = run(false);
  SetCurrentTuning(saved);

  // The fixture has to have produced a wound in both arms before any of the
  // rest means anything.
  const bool cutOk = heal.cut && rot.cut && heal.soakCut > 0 && rot.soakCut > 0;
  // ...and the decay has to have RUN, or the heal arm proves nothing. The rot
  // arm losing voxels is the evidence; it is the bug, reproduced on purpose.
  const bool rotRan = rot.attached ? rot.artEnd < rot.artCut : true;
  // THE CLAIM. The soak fades on a living body...
  const bool healed = heal.attached && heal.soakEnd * 4 < heal.soakCut;
  // ...and it fades by turning back into flesh, so not one voxel leaves.
  const bool intact = heal.attached && heal.artEnd == heal.artCut;
  const bool aliveOk = heal.alive;
  const bool ok = cutOk && rotRan && healed && intact && aliveOk;
  detail = Format(
      "%s %s over %d ticks | heal: %u soaked -> %u, limb %u -> %u voxels "
      "(%s, %s) | rot: %u soaked -> %u, limb %u -> %u voxels (%s) | "
      "cut %s, decay ran %s",
      t.defName.c_str(), t.limbName.c_str(), kTicks, heal.soakCut, heal.soakEnd,
      heal.artCut, heal.artEnd, heal.attached ? "attached" : "SEVERED",
      heal.alive ? "alive" : "DEAD", rot.soakCut, rot.soakEnd, rot.artCut,
      rot.artEnd, rot.attached ? "attached" : "severed", cutOk ? "yes" : "NO",
      rotRan ? "yes" : "NO");
  mobs.Reset();
  c.debris.Reset();
  if (!rotRan) return Status::Skip;  // nothing to say: the pass never woke
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// laser-head: a beam through a head kills it and leaves it ON, with a hole
// ---------------------------------------------------------------------------
//
// Owner report, 2026-09-22: "press F to shoot laser through an enemy's head
// dismembers or explodes the head instead of shooting a clean hole through it
// like it used to". Two causes, one per half of this gate:
//
//   * THE DEATH. gore.brainHpPerVoxel charges every brain cell the bore
//     crosses, so the head reached hp 0 within a few ticks, and a vital
//     severable limb at zero went through Sever() — which DETACHES it before
//     it calls Die(). hp now kills in place (Mob::Damage, Mob::CarveLimb);
//     only geometry takes a head off.
//   * THE CORPSE. From the kill on, the head is debris, and the beam switched
//     to laserMeltRadius (a 2-voxel ball per tick, sized for rock). Dead flesh
//     now takes laserCarveRadius like the living (session.cpp phase C).
//
// The beam is the one session.cpp fires, step for step: CastRayBody, then
// Damage + CarveLimbRadial on a live limb, MeltBodyAt at the carve radius on
// dead flesh. Asserted: it killed (or the gate proved nothing), nothing was
// severed, and every body of the corpse is still jointed. Whether the bore
// came out the far side is recorded, not asserted — the ray reads the
// collider, which is coarser than the skin being bored.
Status GateLaserHead(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  // The fixture: the LARGEST vital, severable limb that bleeds — a head, by
  // property rather than by name. Largest for ChooseTarget's reason: a tiny
  // head is mostly neck, and a bore through it is a bore through the joint.
  Target t;
  for (size_t d = 0; d < mobs.Defs().size(); d++) {
    const MobDef& def = mobs.Defs()[d];
    if (def.bleedMat == 0) continue;
    mobs.Reset();
    const uint64_t sid = mobs.Spawn((int)d, FixtureSite(c.world, 410));
    if (!sid) continue;
    for (size_t li = 0; li < def.limbs.size(); li++) {
      if ((int)li == def.rootLimb) continue;
      if (!def.limbs[li].vital || !def.limbs[li].severable) continue;
      if (!mobs.LimbBody(sid, (int)li)) continue;
      const uint32_t n = mobs.LimbVoxelsAtSpawn(sid, (int)li);
      if (n <= t.atSpawn) continue;
      t.defIndex = (int)d;
      t.limb = (int)li;
      t.atSpawn = n;
      t.defName = def.name;
      t.limbName = def.limbs[li].name;
    }
  }
  mobs.Reset();
  if (!t.valid()) {
    detail = "no loaded mob def has a vital severable limb that bleeds";
    return Status::Fail;
  }
  IVec3 pchunk{};
  const uint64_t id = SpawnTarget(c, t, 410, pchunk);
  if (!id || !mobs.LimbBody(id, t.limb)) {
    detail = "spawn refused";
    return Status::Fail;
  }
  const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
  const Vec3 centre = ax.anchor + ax.along * (ax.reach * 0.5f);
  const Vec3 dir = ax.edge;
  const Vec3 muzzle = centre - dir * 12.0f;
  const float range = 24.0f;
  const auto& tools = CurrentTuning().tools;

  uint32_t tick = 53000;
  int deathTick = -1, throughTick = -1, hits = 0, severTick = -1,
      severLimb = -1;
  std::string cause;
  std::vector<ParticleSpawn> spawns;
  for (int i = 0; i < 300; i++) {
    float frac = 1.0f;
    const uint64_t hit = c.phys.CastRayBody(muzzle, dir, range, frac);
    if (!hit) {
      if (throughTick < 0) throughTick = i;
      if (deathTick >= 0) break;
    } else {
      const Vec3 at = muzzle + dir * (frac * range);
      spawns.clear();
      if (mobs.Damage(hit, tools.laserDamage, at)) {
        hits++;
        mobs.CarveLimbRadial(hit, at, tools.laserCarveRadius, false, false,
                             c.world, spawns);
      } else if (c.debris.BodyIsDeadFlesh(hit)) {
        hits++;
        c.debris.MeltBodyAt(hit, at, tools.laserCarveRadius, c.world, spawns);
      }
    }
    if (deathTick < 0 && !mobs.IsAlive(id)) {
      deathTick = i;
      cause = mobs.DeathCause(id);
    }
    if (severTick < 0 && !mobs.SeverEvents().empty()) {
      severTick = i;
      severLimb = mobs.SeverEvents().front().limbIndex;
    }
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> sp;
    std::vector<CellOp> cellOps;
    mobs.PreTick(tick++, c.world, ops, cellOps, sp);
    c.phys.Step(kTickDt);
    mobs.PostStep();
    c.debris.PostStep();
    // Held on after the kill for a second, so the corpse half is exercised.
    if (deathTick >= 0 && i >= deathTick + 30) break;
  }
  const size_t severs = mobs.SeverEvents().size();
  int bodies = 0, jointed = 0;
  for (uint32_t i = 0; i < c.debris.BodyCount(); i++) {
    bodies++;
    if (c.phys.JointCount(c.debris.BodyHandle(i)) > 0) jointed++;
  }
  const bool died = deathTick >= 0;
  const bool ok = died && severs == 0 && bodies > 0 && jointed == bodies;
  mobs.Reset();
  c.debris.Reset();
  detail = Format(
      "%s.%s: %d beam hits, died=%d%s, severs=%zu%s, %d/%d corpse bodies "
      "jointed, bored through=%s%s",
      t.defName.c_str(), t.limbName.c_str(), hits, died ? 1 : 0,
      died ? Format(" at tick %d", deathTick).c_str() : "", severs,
      severTick >= 0
          ? Format(" (first: limb %d at tick %d)", severLimb, severTick).c_str()
          : "",
      jointed,
      bodies, throughTick >= 0 ? Format("tick %d", throughTick).c_str() : "no",
      cause.empty() ? "" : Format(" cause: %s", cause.c_str()).c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- joint-twins -----------------------------------------------------------
// ONE CELL OF FLESH, TWO COPIES, ONE STATE (Mob::SyncJointTwins).
//
// Owner report 2026-09-23: the hips and the torso overlap, so an infected torso
// voxel could sit under the hip's pristine copy of the same cell and never be
// seen. The overlap is authored and stays; what is asserted is that the two
// copies now share whatever happens to either:
//
//   MATERIAL  rot written into the PARENT's copy appears in the child's;
//   COAT      blood written into the CHILD's copy appears in the parent's;
//   REMOVAL   a hole in the child's copy is a hole in the parent's;
//   CONTROL   no OTHER twin cell's material moved (the sync copies changes,
//             never the parent over the child).
//
// The infection's own spread and rot are switched off for the duration, so
// the only writer is this gate and the sync; one tick of PreTick is what the
// claim is about ("next tick, both copies agree"). Picks the def with the most
// twin cells, by shape rather than by name; reports every pair's count.
Status GateJointTwins(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const uint32_t rotMat = mobs.MaterialIdNamed("rotflesh");
  if (!rotMat) {
    detail = "materials.json has no `rotflesh`";
    return Status::Skip;
  }
  Target t;
  uint32_t most = 0;
  for (size_t d = 0; d < mobs.Defs().size(); d++) {
    const MobDef& def = mobs.Defs()[d];
    if (def.limbs.empty() || def.bleedMat == 0) continue;
    mobs.Reset();
    const uint64_t id = mobs.Spawn((int)d, FixtureSite(c.world, 505));
    if (!id) continue;
    const uint32_t n = mobs.JointTwinCount(id);
    if (n > most) {
      most = n;
      t.defIndex = (int)d;
      t.defName = def.name;
    }
  }
  mobs.Reset();
  if (t.defIndex < 0) {
    detail = "no loaded bleeding def has a single twin cell (no limb overlaps "
             "its parent on the rest pose)";
    return Status::Fail;
  }
  const uint32_t blood = mobs.Defs()[t.defIndex].bleedMat;

  const Tuning saved = CurrentTuning();
  Tuning tt = saved;
  tt.gore.infectSpreadRate = 0.0f;
  tt.gore.infectRotRate = 0.0f;
  SetCurrentTuning(tt);

  IVec3 chunk{};
  const uint64_t id = SpawnTarget(c, t, 505, chunk);
  const uint32_t n = id ? mobs.JointTwinCount(id) : 0;
  if (n < 3) {
    SetCurrentTuning(saved);
    mobs.Reset();
    detail = t.defName + ": " + std::to_string(n) + " twin cells after spawn";
    return Status::Fail;
  }
  // Per-pair attribution: which joints overlap, and by how much.
  std::string pairs;
  {
    int pa = -2, pb = -2;
    uint32_t run = 0;
    auto flush = [&]() {
      if (pa < 0) return;
      const auto& L = mobs.Defs()[t.defIndex].limbs;
      pairs += (pairs.empty() ? "" : ", ") + L[pa].name + "/" + L[pb].name +
               " " + std::to_string(run);
    };
    for (uint32_t k = 0; k < n; k++) {
      int a = -1, b = -1;
      IVec3 r{};
      mobs.JointTwinAt(id, k, a, b, r);
      if (a != pa || b != pb) {
        flush();
        pa = a;
        pb = b;
        run = 0;
      }
      run++;
    }
    flush();
  }

  struct Probe { int a = -1, b = -1; IVec3 rest{}; };
  auto probeAt = [&](uint32_t k) {
    Probe p;
    mobs.JointTwinAt(id, k, p.a, p.b, p.rest);
    return p;
  };
  const Probe pm = probeAt(0), ps = probeAt(n / 2), pr = probeAt(n - 1);

  // Every twin cell's material on both sides, for the control.
  auto snapshot = [&]() {
    std::vector<uint32_t> s;
    for (uint32_t k = 0; k < n; k++) {
      const Probe p = probeAt(k);
      uint32_t ma = 0, mb = 0;
      uint16_t st = 0;
      mobs.LimbCellAt(id, p.a, p.rest, ma, st);
      mobs.LimbCellAt(id, p.b, p.rest, mb, st);
      s.push_back(ma);
      s.push_back(mb);
    }
    return s;
  };
  const std::vector<uint32_t> before = snapshot();

  uint32_t m0 = 0, m1 = 0;
  uint16_t s0 = 0, s1 = 0;
  // MATERIAL, parent -> child.
  mobs.LimbCellAt(id, pm.a, pm.rest, m0, s0);
  mobs.SetLimbCellAt(id, pm.a, pm.rest, rotMat, s0);
  // COAT, child -> parent: 9/15 of the creature's own blood.
  mobs.LimbCellAt(id, ps.b, ps.rest, m1, s1);
  const uint16_t coat = (uint16_t)((blood & 0xFFFu) | (9u << 12));
  mobs.SetLimbCellAt(id, ps.b, ps.rest, m1, coat);
  // REMOVAL, child -> parent.
  mobs.SetLimbCellAt(id, pr.b, pr.rest, 0, 0);

  {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    mobs.PreTick(2000u, c.world, ops, cellOps, spawns);
    c.phys.Step(kTickDt);
    mobs.PostStep();
  }

  uint32_t mat = 0;
  uint16_t st = 0;
  const bool matOk = mobs.LimbCellAt(id, pm.b, pm.rest, mat, st) && mat == rotMat;
  const uint32_t matSeen = mat;
  st = 0;
  const bool coatOk = mobs.LimbCellAt(id, ps.a, ps.rest, mat, st) && st == coat;
  const uint16_t coatSeen = st;
  const bool goneOk = !mobs.LimbCellAt(id, pr.a, pr.rest, mat, st);

  // The removed cell was the LAST link, so every surviving link keeps its
  // index and `after[2k..2k+1]` is the same rest cell as `before[2k..2k+1]`.
  const std::vector<uint32_t> after = snapshot();
  uint32_t moved = 0;
  for (uint32_t k = 1; k + 1 < n; k++) {
    if (k == n / 2) continue;  // the coat probe (its material is unchanged,
                               // but it is not a bystander)
    if (before[2 * k] != after[2 * k] || before[2 * k + 1] != after[2 * k + 1])
      moved++;
  }
  const uint32_t linksAfter = mobs.JointTwinCount(id);

  SetCurrentTuning(saved);
  mobs.Reset();
  c.debris.Reset();

  char buf[224];
  std::snprintf(buf, sizeof buf,
                "; material %s (child reads %u, want %u), coat %s (parent "
                "reads 0x%04x, want 0x%04x), removal %s, %u other cells moved, "
                "%u links left (want %u)",
                matOk ? "SHARED" : "NOT shared", matSeen, rotMat,
                coatOk ? "SHARED" : "NOT shared", coatSeen, coat,
                goneOk ? "SHARED" : "NOT shared", moved, linksAfter, n - 1);
  detail = t.defName + ": " + std::to_string(n) + " twin cells (" + pairs +
           ")" + buf;
  (void)m0;
  return matOk && coatOk && goneOk && moved == 0 && linksAfter == n - 1
             ? Status::Pass
             : Status::Fail;
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
      {"corpse-cut", "mob", {}, false, GateCorpseCut, false},
      {"hit-drive", "mob", {}, false, GateHitDrive, false},
      {"corpse-dismember", "mob", {}, false, GateCorpseDismember, false},
      {"corpse-blunt", "mob", {}, false, GateCorpseBlunt, false},
      {"corpse-armor", "mob", {}, false, GateCorpseArmor, false},
      {"corpse-bleed", "mob", {}, false, GateCorpseBleed, false},
      {"body-stain", "mob", {}, false, GateBodyStain, false},
      {"corpse-wash", "mob", {}, false, GateCorpseWash, false},
      {"corpse-crossheat", "mob", {}, false, GateCorpseCrossheat, false},
      {"corpse-worn", "mob", {}, false, GateCorpseWorn, false},
      {"corpse-splatter", "mob", {}, false, GateCorpseSplatter, false},
      {"body-coat", "mob", {}, false, GateBodyCoat, false},
      {"blast-stain", "mob", {}, false, GateBlastStain, false},
      {"wound-heal", "mob", {}, false, GateWoundHeal, false},
      {"corpse-burn", "mob", {}, false, GateCorpseBurn, false},
      {"laser-head", "mob", {}, false, GateLaserHead, false},
      {"joint-twins", "mob", {}, false, GateJointTwins, false},
  };
  return g;
}

}  // namespace selftest
