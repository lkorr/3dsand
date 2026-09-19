// selftest_impact.cpp — THE OTHER TWO KINDS OF BLOW: trauma, and teeth.
//
// WHAT CHANGED, AND WHY THESE FOUR GATES EXIST
//
// Until 2026-09-15 every weapon in this engine arrived as a KERF. `EdgeSweep`
// carried one `damage` float and the only thing `MeleeSweepDamage` could do
// with a body it met was `Damage` it and `CutLimb` it — which is precisely why
// there was no mace and no fist. A strike is now THREE PARTS (game/impact.h
// StrikeProfile): a CUT, a BLUNT part and a BITE part, each a number, all three
// resolved by one sweep against whatever it hit.
//
// selftest_wound.cpp owns the cut and is untouched by any of this. These four
// own the other two, and each one is a different way the split could be wrong:
//
//   impact-blunt   a mace HURTS and does not DISMEMBER. Both halves, because
//                  a model that only bruises is as wrong as one that severs:
//                  hp falls, the limb stays attached past hp 0, voxels do
//                  leave (a dent), and the blood is a fraction of a cut's.
//   impact-armor   the other direction, against a plate. A sword takes a chip
//                  and the wearer is untouched; a mace takes a great deal more
//                  and the LIMB UNDER THE PLATE is hurt. That pair is the
//                  whole "plate stops swords almost entirely; maces go
//                  through", and neither arm proves it alone.
//   impact-fist    the ladder from a bare hand to an iron gauntlet. A punch
//                  removes NOTHING and still marks; a gauntlet removes real
//                  voxels, slowly — the owner's "caving a face in SLOWLY",
//                  bounded above by the fraction one sword cut takes.
//   bite-rot       a zombie's tear infects FLESH and is refused by ARMOUR.
//                  Again a pair: an infection that always landed would be a
//                  different feature, and one that never landed is no feature.
//
// FABRICATED, NEVER THROUGH THE AI — the rule selftest_wound.cpp states and
// for the same reason: these are claims about the WOUND MODEL, and routing
// them through a stroke would make every one of them also a claim about
// whether an NPC managed to reach. Package B's `unarmed-attack` and `lunge`
// gates are the ones that ask that question.
//
// ...WITH ONE DELIBERATE EXCEPTION. `impact-blunt`'s first arm drives a real
// `EdgeSweep` carrying the shipped mace's own profile through the real
// `MeleeSweepDamage`, against a fixture standing in the world. Everything
// after it calls `MobSystem::BluntHit` directly, which is faster to iterate on
// and isolates the wound model — but if only the direct path were tested, the
// RESOLVER could stop dispatching the blunt part entirely and all four gates
// would stay green. One arm through the front door is what forbids that.
//
// CONTENT-INDEPENDENT WHERE IT CAN BE. The victim is CHOSEN (the largest
// severable non-vital limb on any loaded def that bleeds, exactly as the wound
// gates choose theirs) rather than named. The WEAPONS are named — `mace`,
// `iron_cuirass`, `iron_gauntlets` — because a gate about what a mace does has
// no meaning without one, and a missing item SKIPS rather than fails: an
// asset that is not there is not a regression in this code.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "game/equipment.h"
#include "game/impact.h"
#include "game/item.h"
#include "game/melee.h"
#include "game/mob.h"
#include "sim/materials.h"
#include "sim/scale.h"
#include "sim/tuning.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// ---------------------------------------------------------------------------
// The fixture. Deliberately a near-copy of selftest_wound.cpp's rather than a
// shared header: the two files pick the same KIND of target for different
// reasons (that one wants something a blade can part, this one wants something
// a mace can dent without the gate having to care whether it parts), and a
// shared chooser would be one more thing that has to stay true for both.
// ---------------------------------------------------------------------------
struct Target {
  int defIndex = -1;
  int limb = -1;
  int torso = -1;          // the root limb, for the armoured arms
  uint32_t atSpawn = 0;
  std::string defName, limbName;
  bool valid() const { return defIndex >= 0 && limb >= 0; }
};

// Where a limb runs, measured from its own surviving voxels. Same construction
// as the wound gates': the anchor is one END of a limb and the mass extends
// away from it, so anchor -> centroid IS the long axis, with no PCA and no
// authored axis to go stale.
struct LimbAxis {
  Vec3 anchor{};
  Vec3 along{0, 1, 0};
  float reach = 1.0f;
  bool valid = false;
};

LimbAxis MeasureLimb(MobSystem& mobs, uint64_t id, int limb) {
  LimbAxis a;
  if (!mobs.LimbBody(id, limb)) return a;
  a.anchor = mobs.LimbAnchorPos(id, limb);
  const uint32_t kProbes = 24;
  Vec3 sum{};
  std::vector<Vec3> pts;
  pts.reserve(kProbes);
  for (uint32_t k = 0; k < kProbes; k++) {
    const Vec3 p = mobs.LimbVoxelPos(id, limb, k * 7919u);
    pts.push_back(p);
    sum += p;
  }
  const Vec3 centroid = sum * (1.0f / (float)kProbes);
  Vec3 along = centroid - a.anchor;
  if (along.len() < 1e-3f) along = Vec3{0, -1, 0};  // anchor sits in the mass
  a.along = along.normalized();
  float far = 0;
  for (const Vec3& p : pts) far = std::max(far, (p - a.anchor).dot(a.along));
  a.reach = std::max(far, 0.5f);
  a.valid = true;
  return a;
}

// Ground under a fixture column, anchored to the residency window. NEVER an
// absolute x or z (selftest.h's ordering note): by the time these gates run,
// `streaming` has walked the window origin ~20 chunks along x and a literal
// column lands outside it, where world writes are dropped and a mob despawns
// the instant it is spawned.
//
// The insets below (405/415/425/435) are this file's own and appear in no
// other gate, so two fixtures can never be stood on the same ground.
IVec3 FixtureSite(const World& world, int inset) {
  const IVec3 org = world.WindowOrigin();
  const int x = org.x * (int)kChunk + inset;
  const int z = org.z * (int)kChunk + inset;
  return IVec3{x, World::TerrainHeight(x, z, kDefaultSeed) + 1, z};
}

void PrepareWorld(Ctx& c) {
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
}

// Pick the victim: the biggest severable, non-vital limb in the library, on a
// def that can bleed. Measured by SPAWNING rather than by reading the def's
// prefab, because what is struck is the RUNTIME lattice — an art voxel count
// says nothing about physScale, skinScale, or whether the rig gave that limb a
// body at all. Each candidate is despawned again.
Target ChooseTarget(MobSystem& mobs, IVec3 at) {
  Target best;
  for (size_t d = 0; d < mobs.Defs().size(); d++) {
    const MobDef& def = mobs.Defs()[d];
    if (def.limbs.empty() || def.bleedMat == 0) continue;
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
      best.torso = def.rootLimb;
      best.atSpawn = n;
      best.defName = def.name;
      best.limbName = def.limbs[li].name;
    }
  }
  mobs.Reset();
  return best;
}

uint64_t SpawnTarget(Ctx& c, const Target& t, int inset) {
  MobSystem& mobs = c.mobs;
  mobs.Reset();
  c.debris.Reset();
  const uint64_t id = mobs.Spawn(t.defIndex, FixtureSite(c.world, inset));
  if (!id) return 0;
  // A few physics steps only — no world submit. The creature has to be posed
  // and its limb transforms real before anything is measured off them, and
  // nothing here needs the CA to have run.
  for (int i = 0; i < 8; i++) {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    mobs.PreTick(3000u + (uint32_t)i, c.world, ops, cellOps, spawns);
    c.phys.Step(kTickDt);
    mobs.PostStep();
  }
  mobs.ClearSeverEvents();
  mobs.ClearSeverStats();
  return id;
}

// ---- ONE BLOW, EACH KIND, EXPRESSED AS THE SWEEP EXPRESSES IT --------------
//
// `power` is already speed x edge-alignment, exactly as MeleeSweepDamage forms
// it, so these three read as the three calls that function makes and a tuning
// change moves the gate and the game together.

// A SMALL DETERMINISTIC WANDER around the aim point, in world voxels.
//
// Nobody lands twenty-four punches on one cell, and a gate that does is not
// measuring what it says it is: a radial dent SATURATES -- the first blow
// empties the ball and every one after it finds that space already gone, which
// is the same failure the kerf's entry-plane snap exists to fix (Mob::CutLimb).
// Measured: 24 gauntlet blows on one point took 2 voxels, which is a true
// number about a thing nobody does.
//
// So the blows walk a little, over the same fixed sequence in every arm, which
// keeps a comparison between two profiles a comparison between the profiles.
// An LCG rather than rng::Hash3 because this is a TEST fixture and its only
// contract is reproducibility, not the counter-based discipline the sim owes.
Vec3 Wander(Vec3 at, const LimbAxis& ax, int k, float spread) {
  const uint32_t h = (uint32_t)k * 1664525u + 1013904223u;
  // Two axes across the limb, so the wander is over its SURFACE rather than
  // along its length -- walking down the limb would measure a different thing
  // (grinding it away) than hitting one area repeatedly.
  const Vec3 cand[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  int b = 0;
  float bd = 2.0f;
  for (int i = 0; i < 3; i++) {
    const float d = std::fabs(ax.along.dot(cand[i]));
    if (d < bd) { bd = d; b = i; }
  }
  const Vec3 u = ax.along.cross(cand[b]).normalized();
  const Vec3 v = u.cross(ax.along).normalized();
  const float a = ((float)((h >> 8) & 0xFFFFu) / 65535.0f) * 2.0f - 1.0f;
  const float c = ((float)((h >> 20) & 0xFFFu) / 4095.0f) * 2.0f - 1.0f;
  return at + u * (a * spread) + v * (c * spread);
}

void BluntOnce(MobSystem& mobs, World& world, uint64_t id, int limb, Vec3 at,
               const StrikeProfile& p, float power, uint32_t seed,
               std::vector<ParticleSpawn>& spawns) {
  const uint64_t body = mobs.LimbBody(id, limb);
  if (!body) return;
  BluntHit b;
  b.at = at;
  b.hp = p.blunt * power;
  b.power = power;
  b.carve = p.bluntCarve;
  b.armorBreak = p.armorBreak;
  // Zero on purpose: the knock-loose rule is a SPEED rule about items being
  // beaten out of a hand, and arming it here would sever the fixture for a
  // reason that has nothing to do with what is being measured.
  b.impactSpeed = 0.0f;
  b.seed = seed;
  mobs.BluntHit(body, b, world, spawns);
}

void BiteOnce(MobSystem& mobs, World& world, uint64_t id, int limb, Vec3 at,
              float hp, uint16_t infectMat, uint16_t infectStain, float power,
              uint32_t seed, std::vector<ParticleSpawn>& spawns) {
  const uint64_t body = mobs.LimbBody(id, limb);
  if (!body) return;
  BiteHit bt;
  bt.at = at;
  bt.hp = hp * power;
  bt.power = power;
  bt.infectMat = infectMat;
  bt.infectStain = infectStain;
  bt.seed = seed;
  mobs.BiteHit(body, bt, world, spawns);
}

void CutOnce(MobSystem& mobs, World& world, uint64_t id, int limb,
             const LimbAxis& ax, float alongLimb, float dmg, float power,
             uint32_t seed, std::vector<ParticleSpawn>& spawns) {
  const uint64_t body = mobs.LimbBody(id, limb);
  if (!body || !ax.valid) return;
  const auto& g = CurrentTuning().gore;
  // A frame for the kerf: along the limb, and across it. Any two unit vectors
  // spanning the cross-section will do — chosen against whichever world axis
  // the limb is LEAST aligned with, so the cross product is never degenerate.
  const Vec3 cand[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  int best = 0;
  float bestDot = 2.0f;
  for (int i = 0; i < 3; i++) {
    const float d = std::fabs(ax.along.dot(cand[i]));
    if (d < bestDot) { bestDot = d; best = i; }
  }
  const Vec3 travel = ax.along.cross(cand[best]).normalized();
  BladeCut cut;
  cut.at = ax.anchor + ax.along * alongLimb;
  cut.edgeAxis = travel.cross(ax.along).normalized();
  cut.cutDir = travel;
  cut.halfWidth = std::max(0.9f * g.cutWidth, 0.08f);
  cut.depth = (g.cutDepth + g.cutDepthPower * power);
  cut.length = g.cutLength * (0.4f + 0.6f * power);
  cut.power = power;
  cut.seed = seed;
  MobSystem::BladeCutScope blade(mobs, power);
  if (mobs.Damage(body, dmg * power, cut.at, 0.0f))
    mobs.CutLimb(body, cut, world, spawns);
}

// The mace, by name, or a default profile if the item is not shipped. Returns
// false when there is no mace: a missing asset SKIPS rather than fails, since
// it is not a regression in this code.
bool MaceProfile(const ItemLibrary& items, StrikeProfile& out) {
  const ItemDef* d = items.At(items.Find("mace"));
  if (!d) return false;
  out = d->strike;
  return true;
}

// ---------------------------------------------------------------------------
// impact-blunt — a mace hurts and does not dismember
// ---------------------------------------------------------------------------
Status GateImpactBlunt(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 405));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  StrikeProfile mace;
  if (!MaceProfile(c.items, mace)) {
    detail = "no `mace` in the item library";
    return Status::Skip;
  }
  if (mace.blunt <= 0.0f) {
    detail = Format("the shipped mace has blunt %.1f — nothing to measure",
                    mace.blunt);
    return Status::Fail;
  }

  const int kHits = (int)BaselineNumber("impactBluntHits", 24);
  const float kPower = 0.85f;
  std::vector<ParticleSpawn> spawns;

  // ---- ARM A: THROUGH THE REAL SWEEP --------------------------------------
  //
  // One `EdgeSweep` carrying the shipped mace's own profile, through
  // `MeleeSweepDamage` itself, against a fixture standing in the world. This
  // is the arm that forbids the resolver from silently dropping the blunt
  // part: every other arm in this file calls BluntHit directly and would stay
  // green if the dispatch were deleted.
  //
  // The edge is swept THROUGH the limb's own centre over one tick, fast enough
  // to clear melee.minSpeed, which is what makes the probe rays find it.
  uint32_t sweptLost = 0;
  float sweptHpDrop = 0.0f;
  bool sweptAttached = false;
  bool sweptRan = false;
  {
    // TWO CREATURES, because MeleeSweepDamage needs a WIELDER and refuses to
    // cut one (`wielder.OwnsBody`) -- handing it the victim would make every
    // probe skip and the arm would measure nothing while passing. The second
    // is the same def stood 55 voxels away, which is far enough that neither
    // walks into the other over the eight settle steps.
    mobs.Reset();
    c.debris.Reset();
    const uint64_t wid = mobs.Spawn(t.defIndex, FixtureSite(c.world, 405));
    const uint64_t id = mobs.Spawn(t.defIndex, FixtureSite(c.world, 460));
    if (!wid || !id) {
      detail = "spawn refused";
      return Status::Fail;
    }
    for (int i = 0; i < 8; i++) {
      std::vector<BrushOp> ops;
      std::vector<ParticleSpawn> st;
      std::vector<CellOp> cellOps;
      mobs.PreTick(3000u + (uint32_t)i, c.world, ops, cellOps, st);
      c.phys.Step(kTickDt);
      mobs.PostStep();
    }
    mobs.ClearSeverEvents();
    mobs.ClearSeverStats();
    Mob* wielder = mobs.FindMobById(wid);
    if (!wielder) {
      detail = "the wielder vanished before it could swing";
      return Status::Fail;
    }
    const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
    const uint32_t before = mobs.LimbArtVoxelCount(id, t.limb);
    const float hp0 = mobs.LimbHp(id, t.limb);
    const Vec3 mid = ax.anchor + ax.along * (ax.reach * 0.5f);
    // Across the limb, so the swept quad crosses it rather than running along
    // it. `travel` is the same least-aligned-axis construction CutOnce makes.
    const Vec3 cand[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    int b = 0;
    float bd = 2.0f;
    for (int i = 0; i < 3; i++) {
      const float d = std::fabs(ax.along.dot(cand[i]));
      if (d < bd) { bd = d; b = i; }
    }
    const Vec3 travel = ax.along.cross(cand[b]).normalized();
    const Vec3 edge = travel.cross(ax.along).normalized();
    MeleeTuning mt;
    ApplyMeleeTuning(mt);
    // Enough tip travel in one tick to sit at the top of the speed ramp: the
    // claim is about what a committed blow does, not about the ramp.
    const float step = mt.fullSpeed * kTickDt * 1.2f;
    for (int k = 0; k < kHits && mobs.LimbBody(id, t.limb); k++) {
      EdgeSweep sw;
      sw.aPrev = mid - edge * 1.5f - travel * step;
      sw.bPrev = mid + edge * 1.5f - travel * step;
      sw.aNow = mid - edge * 1.5f;
      sw.bNow = mid + edge * 1.5f;
      sw.flatNow = Vec3{};   // a mace has no flat: MeleeEdgeAlign returns 1
      sw.dt = kTickDt;
      sw.halfWidth = 0.4f;
      sw.strike = mace;
      sw.heft = 1.0f;
      sw.tick = 7000u + (uint32_t)k;
      sw.valid = true;
      // The wielder is the victim's own def standing elsewhere would be a
      // second creature to keep alive; the fixture is its own wielder and
      // `OwnsBody` then refuses every probe. So the sweep is given a mob that
      // owns nothing in the way: a second spawn is heavier than this gate
      // needs, and MobSystem::FindParry skips anything holding no item.
      const EdgeSweepResult r = MeleeSweepDamage(sw, mt, *wielder, c.phys, mobs,
                                                 c.debris, c.world, spawns);
      if (r.bodiesHit > 0) sweptRan = true;
    }
    const bool alive = mobs.LimbBody(id, t.limb) != 0;
    sweptAttached = alive;
    sweptLost = alive ? (before - std::min(before,
                                           mobs.LimbArtVoxelCount(id, t.limb)))
                      : before;
    sweptHpDrop = alive ? hp0 - mobs.LimbHp(id, t.limb) : hp0;
    mobs.Reset();
    c.debris.Reset();
  }

  // ---- ARM B: THE WOUND MODEL ITSELF --------------------------------------
  uint32_t before = 0, after = 0, bruise = 0, bruiseOne = 0, bruiseDeep = 0;
  float hp0 = 0, hp1 = 0, bluntBleed = 0;
  bool attached = false, alive = false;
  size_t severs = 0;
  const uint32_t bruiseMat = c.mobs.MaterialIdNamed(CurrentTuning().gore.bruiseMat);
  // MORE THAN ONE BLOW'S WORTH, in coat levels — the threshold the "it deepens"
  // claim below is measured against.
  //
  // NOT THE CEILING ITSELF, and the difference is load-bearing now that a voxel
  // AT the ceiling rolls to become blood instead (`gore.bruiseBleedChance`).
  // Counting only saturated BRUISE cells would fall back toward zero as the
  // middle of the patch went wet — the gate would go red precisely because the
  // feature was working. One step plus one is the honest line: no single blow
  // can reach it, so anything above it accumulated.
  const auto& gtune = CurrentTuning().gore;
  const uint32_t bruiseCap = (uint32_t)std::lround(
                                 std::clamp(gtune.bruiseStep, 0.0f, 15.0f)) +
                             1u;
  {
    const uint64_t id = SpawnTarget(c, t, 415);
    if (!id) {
      detail = "spawn refused";
      return Status::Fail;
    }
    const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
    before = mobs.LimbArtVoxelCount(id, t.limb);
    hp0 = mobs.LimbHp(id, t.limb);
    // ALWAYS THE SAME SPOT, a quarter of the limb's reach in — the same place
    // wound-accumulate hacks at, so "a mace at the place a sword parts a limb"
    // is the comparison being made and not two different comparisons.
    const Vec3 at = ax.anchor + ax.along * (ax.reach * 0.25f);
    for (int k = 0; k < kHits; k++) {
      if (!mobs.LimbBody(id, t.limb)) break;
      BluntOnce(mobs, c.world, id, t.limb, at, mace, kPower,
                0xB100u + (uint32_t)k * 2654435761u, spawns);
      // AFTER THE FIRST BLOW ONLY. A bruise is an accumulating COAT now
      // (DESIGN.md, "A bruise is an alpha that deepens"), so the claim worth
      // asserting is not "some voxels are bruised" but "the SAME voxels got
      // darker" — and that needs a reading from before the rest of the blows
      // landed to compare against.
      if (k == 0 && bruiseMat && mobs.LimbBody(id, t.limb))
        bruiseOne = mobs.LimbCoatMatCount(id, t.limb, bruiseMat, 1);
    }
    attached = mobs.LimbBody(id, t.limb) != 0;
    alive = mobs.IsAlive(id);
    severs = mobs.SeverEvents().size();
    if (attached) {
      after = mobs.LimbArtVoxelCount(id, t.limb);
      hp1 = mobs.LimbHp(id, t.limb);
      // THE COAT, NOT THE MATERIAL. Counting `skin_bruised` VOXELS was right
      // while a bruise was a material rewrite and is now always zero: the
      // voxel keeps its own material and carries the bruise as a 0..15 body
      // stain instead. `minAmt` 1 is "marked at all".
      bruise = bruiseMat ? mobs.LimbCoatMatCount(id, t.limb, bruiseMat, 1) : 0u;
      // ...and how many have been driven past a single blow's worth. This is
      // the half that proves the accumulation rather than merely the marking.
      bruiseDeep = (bruiseMat && bruiseCap)
                       ? mobs.LimbCoatMatCount(id, t.limb, bruiseMat, bruiseCap)
                       : 0u;
      bluntBleed = mobs.LimbBleedBudget(id, t.limb);
    }
    mobs.Reset();
    c.debris.Reset();
  }

  // ---- ARM C: THE SAME hp, AS CUTS ----------------------------------------
  //
  // The control for the bleed claim, and it has to be a MATCHED one: "a mace
  // bleeds less" is meaningless against a different amount of damage. Same
  // number of blows at the same place, each charging the same hp as the mace's
  // blunt part did, through the ordinary kerf.
  float cutBleed = 0.0f;
  {
    const uint64_t id = SpawnTarget(c, t, 425);
    if (!id) {
      detail = "spawn refused";
      return Status::Fail;
    }
    const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
    for (int k = 0; k < kHits; k++) {
      if (!mobs.LimbBody(id, t.limb)) break;
      CutOnce(mobs, c.world, id, t.limb, ax, ax.reach * 0.25f, mace.blunt,
              kPower, 0xC700u + (uint32_t)k * 2654435761u, spawns);
    }
    // A cut may genuinely have parted the limb in this many blows; the budget
    // then lives on the parent stump and the comparison is void. Read it while
    // it is there, and treat "gone" as "bled at least as much", which is the
    // conservative direction for the claim being made.
    cutBleed = mobs.LimbBody(id, t.limb)
                   ? mobs.LimbBleedBudget(id, t.limb)
                   : CurrentTuning().gore.bleedBudgetCap;
    mobs.Reset();
    c.debris.Reset();
  }

  const uint32_t lost = before > after ? before - after : 0u;
  const float frac = before ? (float)lost / (float)before : 0.0f;
  // THE BAND, NOT THE NUMBER. A dent must remove SOMETHING (a mace with
  // bluntCarve 0.6 that took nothing would be a fist) and must stay well short
  // of the collapse fraction that would have shed the limb — which is the
  // claim, since the sever is refused rather than merely unreached.
  const double dentMax = BaselineNumber("impactBluntDentMaxFraction", 0.60);
  RecordObserved("impactBluntLostFraction", (double)frac);
  RecordObserved("impactBluntHpDrop", (double)(hp0 - hp1));
  RecordObserved("impactBluntBruiseCells", (double)bruise);
  RecordObserved("impactBluntBruiseAfterOne", (double)bruiseOne);
  RecordObserved("impactBluntBruiseAtCap", (double)bruiseDeep);
  RecordObserved("impactBluntBleedBudget", (double)bluntBleed);
  RecordObserved("impactBluntCutBleedBudget", (double)cutBleed);
  RecordObserved("impactBluntSweptLost", (double)sweptLost);

  const bool hurt = hp1 < hp0;
  const bool sized = lost > 0 && frac <= (float)dentMax;
  // THE CLAIM. hp is at or below zero and the limb is STILL THERE — that is
  // "a blunt hit never takes a limb off", and nothing weaker states it: a
  // limb merely attached at positive hp has not been asked the question.
  const bool heldOn = attached && severs == 0;
  const bool pastZero = hp1 <= 0.0f;
  const bool marked = bruiseMat != 0 && bruise > 0;
  // ---- ...AND IT DEEPENED RATHER THAN SPREADING (2026-09-16) --------------
  //
  // TWO CLAIMS, because either alone is satisfied by the bug it replaced. A
  // rewrite spread: every blow converted a fresh hash-picked subset, so the
  // COUNT grew and nothing ever got darker. So the test is that `kHits` blows
  // on ONE SPOT drove some voxels all the way to the authored ceiling, which a
  // per-blow step of `gore.bruiseStep` can only reach by accumulating — and
  // that the patch did not simply keep spreading instead, which is what the
  // second half pins by holding the final count near the first blow's.
  //
  // `bruiseOne * 3` is deliberately loose: the radius scales with power and the
  // taper is jittered, so later blows legitimately catch a few rim cells the
  // first missed. What it refuses is the old behaviour, where the count grew
  // without bound because coverage was the only channel the mark had.
  const bool deepened = bruiseCap == 0 || bruiseDeep > 0;
  const bool concentrated = bruiseOne > 0 && bruise <= bruiseOne * 3;
  const bool drier = bluntBleed < cutBleed;
  // ...and the RESOLVER dispatched it: the sweep-driven arm did the same kind
  // of damage through the front door.
  const bool resolver = sweptRan && sweptHpDrop > 0.0f && sweptAttached;

  const bool ok = hurt && sized && heldOn && pastZero && marked && deepened &&
                  concentrated && drier && resolver;
  detail = Format(
      "%s/%s x%d mace(blunt %.0f, dent %.2f): hp %.1f -> %.1f, %u -> %u voxels "
      "(%.1f%%, cap %.0f%%), bruised %u after one blow -> %u after %d (%u past "
      "%u/15, i.e. more than one blow), attached=%d severs=%zu alive=%d | "
      "bleed budget %.2f vs %.2f for the same hp as cuts | through the real "
      "sweep: hp -%.1f, %u voxels, attached=%d",
      t.defName.c_str(), t.limbName.c_str(), kHits, mace.blunt,
      mace.bluntCarve, hp0, hp1, before, after, frac * 100.0f,
      dentMax * 100.0, bruiseOne, bruise, kHits, bruiseDeep, bruiseCap,
      attached ? 1 : 0, severs, alive ? 1 : 0,
      bluntBleed, cutBleed, sweptHpDrop, sweptLost, sweptAttached ? 1 : 0);
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// impact-armor — plate stops swords almost entirely; maces go through
// ---------------------------------------------------------------------------
//
// ONE FIXTURE, TWO WEAPONS, and the claim is the DIFFERENCE. Either arm alone
// is satisfiable by a bug: a cuirass that ignored everything would pass the
// sword arm, and one that ignored nothing would pass the mace arm.
Status GateImpactArmor(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 405));
  if (!t.valid() || t.torso < 0) {
    detail = "no loaded mob def with a root limb that bleeds";
    return Status::Fail;
  }
  const ItemDef* cuirass = c.items.At(c.items.Find("iron_cuirass"));
  const ItemDef* sword = c.items.At(c.items.Find("sword"));
  StrikeProfile mace;
  if (!cuirass || !sword || !MaceProfile(c.items, mace)) {
    detail = "needs `iron_cuirass`, `sword` and `mace` in the item library";
    return Status::Skip;
  }

  const int kHits = (int)BaselineNumber("impactArmorHits", 16);
  const float kPower = 0.85f;
  // The slot by ID, as selftest_equipment.cpp does it: the table in
  // game/equipment.h validates by KIND, so naming the slot here and letting
  // WearItem refuse a mismatch is the same check without a second lookup.
  const int chestSlot = (int)EquipSlotId::Chest;

  // One arm: dress the fixture, find the shell over the torso, hit it N times
  // with `p`, and report what came off the shell and what reached the body.
  struct Arm {
    uint32_t shellBefore = 0, shellAfter = 0;
    float hostHp0 = 0, hostHp1 = 0;
    float hostBleed = 0;
    uint32_t rot = 0;
    bool wore = false;
    bool alive = false;
  };
  const uint32_t rotMat = c.mobs.MaterialIdNamed("rotflesh");
  std::vector<ParticleSpawn> spawns;

  auto run = [&](int inset, const StrikeProfile& p) -> Arm {
    Arm a;
    const uint64_t id = SpawnTarget(c, t, inset);
    if (!id) return a;
    Mob* m = mobs.FindMobById(id);
    if (!m || !m->WearItem(cuirass, chestSlot)) return a;
    a.wore = true;
    // WHICH SLOT IS THE PLATE OVER THE TORSO. Asked of the rig rather than
    // assumed: a piece appends one slot per shell and their order is the cover
    // list's, which is authored data. The one whose host is the root limb is
    // the breastplate.
    int shell = -1;
    for (int li = m->AppendedBase(); li < m->LimbCount(); li++)
      if (m->WornHostOf(li) == t.torso) { shell = li; break; }
    if (shell < 0) return a;
    a.shellBefore = mobs.LimbArtVoxelCount(id, shell);
    a.hostHp0 = mobs.LimbHp(id, t.torso);
    const Vec3 at = mobs.LimbVoxelPos(id, shell, 11u);
    const LimbAxis shellAx = MeasureLimb(mobs, id, shell);
    const float spread = (float)BaselineNumber("impactArmorSpread", 0.6);
    for (int k = 0; k < kHits; k++) {
      if (!mobs.LimbBody(id, shell)) break;
      if (p.cut > 0.0f) {
        // A BLADE ON A PLATE. Through the kerf, which is where the hardness
        // gate that makes iron chip rather than part actually lives
        // (Mob::CutLimb) — a gate that expressed a sword as blunt trauma
        // would be measuring the wrong rule entirely.
        const LimbAxis ax = MeasureLimb(mobs, id, shell);
        CutOnce(mobs, c.world, id, shell, ax, ax.reach * 0.4f, p.cut, kPower,
                0x5040u + (uint32_t)k * 2654435761u, spawns);
      }
      if (p.blunt > 0.0f)
        // A dent SATURATES on one point (see Wander): sixteen blows on one
        // cell of a cuirass took thirteen voxels, which is a true number about
        // a thing nobody does. The same fixed sequence runs in both arms, so
        // the comparison stays a comparison between the two profiles.
        BluntOnce(mobs, c.world, id, shell,
                  shellAx.valid ? Wander(at, shellAx, k, spread) : at, p,
                  kPower, 0xB0A0u + (uint32_t)k * 2654435761u, spawns);
    }
    // THE WEARER MUST SURVIVE, or every reading below is about a corpse: the
    // host here is the ROOT limb, so hp reaching zero is a DEATH (HpZeroSevers)
    // and MobSystem answers -1 for the hp and the bleed budget of a creature
    // that is no longer there. That -1 would sail under the "no bleeding"
    // assertion and pass the gate while measuring nothing, which is the exact
    // shape of a test that quietly stops testing. So it is a checked
    // precondition with its own message, not a silent clamp.
    a.alive = mobs.IsAlive(id);
    a.shellAfter =
        mobs.LimbBody(id, shell) ? mobs.LimbArtVoxelCount(id, shell) : 0u;
    a.hostHp1 = mobs.LimbHp(id, t.torso);
    a.hostBleed = mobs.LimbBleedBudget(id, t.torso);
    a.rot = rotMat ? mobs.LimbMaterialCount(id, t.torso, rotMat) : 0u;
    mobs.Reset();
    c.debris.Reset();
    return a;
  };

  const Arm blade = run(415, sword->strike);
  const Arm club = run(425, mace);
  if (!blade.wore || !club.wore) {
    detail = "the cuirass did not go on the chosen rig";
    return Status::Skip;
  }
  if (!blade.alive || !club.alive) {
    detail = Format(
        "the wearer died inside the measurement (sword arm alive=%d, mace arm "
        "alive=%d) -- every reading below is about a corpse. Lower "
        "impactArmorHits in tests/baseline.json; %d blows of %.0f blunt "
        "through gear.bluntThrough is more than this rig's root limb holds.",
        blade.alive ? 1 : 0, club.alive ? 1 : 0, kHits, mace.blunt);
    return Status::Fail;
  }

  const uint32_t bladeLost =
      blade.shellBefore > blade.shellAfter ? blade.shellBefore - blade.shellAfter : 0u;
  const uint32_t clubLost =
      club.shellBefore > club.shellAfter ? club.shellBefore - club.shellAfter : 0u;
  const float bladeHost = blade.hostHp0 - blade.hostHp1;
  const float clubHost = club.hostHp0 - club.hostHp1;

  RecordObserved("impactArmorBladeShellLost", (double)bladeLost);
  RecordObserved("impactArmorMaceShellLost", (double)clubLost);
  RecordObserved("impactArmorBladeHostHp", (double)bladeHost);
  RecordObserved("impactArmorMaceHostHp", (double)clubHost);
  RecordObserved("impactArmorMaceHostBleed", (double)club.hostBleed);

  // A BLADE CHIPS IRON. The band is a FRACTION of the plate rather than a
  // voxel count, because a shell's size is a property of whichever rig was
  // chosen; `gear.cutHardnessMin` floors the kerf at one lattice cell on
  // purpose, so "zero" is the wrong claim and "a few per cent" is the right
  // one.
  const double chipMax = BaselineNumber("impactArmorBladeShellMaxFraction", 0.10);
  const float bladeFrac =
      blade.shellBefore ? (float)bladeLost / (float)blade.shellBefore : 0.0f;
  const float clubFrac =
      club.shellBefore ? (float)clubLost / (float)club.shellBefore : 0.0f;
  const bool skated = bladeFrac <= (float)chipMax;
  // ...and the wearer is untouched behind it. `gear.bluntThrough` reaches the
  // host only from the BLUNT part, and a sword's is 2 against a mace's 16 —
  // so a small drop here is correct and a large one is the failure.
  const bool sheltered = bladeHost < clubHost;
  // A MACE GOES THROUGH: strictly more plate off, and the body behind it hurt.
  const bool beatIn = clubFrac > bladeFrac;
  const bool got = clubHost > 0.0f;
  // ...WITHOUT OPENING HIM, and that is asserted against the MECHANISM rather
  // than against a constant. "A mace bleeds little" as an absolute number is a
  // rate claim in disguise (CLAUDE.md: a bare count is not a measurement): it
  // is satisfied by a rig whose blood is thin and by a run that landed fewer
  // blows, and it has to be re-derived by hand every time either moves.
  //
  // What is actually claimed is that the transmitted blow topped the budget up
  // at gore.bluntBleedScale of a CUT's rate. Both terms are readable here --
  // the hp really charged, and the creature's own bleed.perDamage -- so the
  // expectation is computed and the observation compared to it, with a little
  // slack for the budget's own cap and for float order.
  const float perDamage = mobs.Defs()[t.defIndex].bleedPerDamage;
  const float asACut = clubHost * perDamage;
  const float asTrauma =
      asACut * std::clamp(CurrentTuning().gore.bluntBleedScale, 0.0f, 1.0f);
  const bool dry = club.hostBleed <= asTrauma * 1.10f + 0.05f &&
                   club.hostBleed < asACut * 0.5f;
  RecordObserved("impactArmorMaceHostBleedAsCut", (double)asACut);
  // ...and no infection came from a weapon that has none. The zero that proves
  // the rewrite is gated on the STRIKE rather than firing on any wound.
  const bool clean = club.rot == 0 && blade.rot == 0;

  const bool ok = skated && sheltered && beatIn && got && dry && clean;
  detail = Format(
      "%s/%s in an iron cuirass, x%d each: SWORD took %u/%u shell voxels "
      "(%.1f%%, cap %.0f%%) and %.1f host hp | MACE took %u/%u (%.1f%%) and "
      "%.1f host hp, bleed budget %.2f (as trauma %.2f, as a cut %.2f), rot %u",
      t.defName.c_str(), mobs.Defs()[t.defIndex].limbs[t.torso].name.c_str(),
      kHits, bladeLost, blade.shellBefore, bladeFrac * 100.0f, chipMax * 100.0,
      bladeHost, clubLost, club.shellBefore, clubFrac * 100.0f, clubHost,
      club.hostBleed, asTrauma, asACut, club.rot);
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// impact-fist — a punch marks; a gauntlet caves in, SLOWLY
// ---------------------------------------------------------------------------
Status GateImpactFist(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 405));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  const ItemDef* gaunt = c.items.At(c.items.Find("iron_gauntlets"));
  if (!gaunt || !gaunt->strike.Any()) {
    detail = "no `iron_gauntlets` with a `strike` block in the item library";
    return Status::Skip;
  }

  // THE BARE FIST IS NOT AN ITEM and deliberately is not read from one: what a
  // creature's own hands are made of is Package B's `natural` block on the mob
  // sidecar, which does not exist in this tree yet. The claim here is about
  // the RESOLVER's handling of `bluntCarve == 0`, so the profile is stated:
  // some trauma, and no dent whatsoever.
  StrikeProfile fist;
  fist.blunt = (float)BaselineNumber("impactFistBlunt", 4.0);
  fist.bluntCarve = 0.0f;

  const int kHits = (int)BaselineNumber("impactFistHits", 24);
  const float kPower = 0.85f;
  const uint32_t bruiseMat =
      c.mobs.MaterialIdNamed(CurrentTuning().gore.bruiseMat);
  std::vector<ParticleSpawn> spawns;

  struct Arm { uint32_t before = 0, after = 0, bruise = 0; bool attached = false; };
  auto run = [&](int inset, const StrikeProfile& p, uint32_t salt) -> Arm {
    Arm a;
    const uint64_t id = SpawnTarget(c, t, inset);
    if (!id) return a;
    const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
    a.before = mobs.LimbArtVoxelCount(id, t.limb);
    const Vec3 at = ax.anchor + ax.along * (ax.reach * 0.5f);
    const float spread = (float)BaselineNumber("impactFistSpread", 0.5);
    for (int k = 0; k < kHits; k++) {
      if (!mobs.LimbBody(id, t.limb)) break;
      // The SAME wander sequence in both arms, so the only difference between
      // them is the profile -- which is the whole claim.
      BluntOnce(mobs, c.world, id, t.limb, Wander(at, ax, k, spread), p, kPower,
                salt + (uint32_t)k * 2654435761u, spawns);
    }
    a.attached = mobs.LimbBody(id, t.limb) != 0;
    if (a.attached) {
      a.after = mobs.LimbArtVoxelCount(id, t.limb);
      // The COAT, not the material -- a bruise no longer rewrites the voxel it
      // marks (DESIGN.md, "A bruise is an alpha that deepens"), so the old
      // material count is always zero now. `minAmt` 1 is "marked at all", which
      // is the claim this arm makes: a bare fist bruises and takes nothing.
      a.bruise = bruiseMat ? mobs.LimbCoatMatCount(id, t.limb, bruiseMat, 1) : 0u;
    }
    mobs.Reset();
    c.debris.Reset();
    return a;
  };

  const Arm bare = run(415, fist, 0xF15Du);
  const Arm iron = run(425, gaunt->strike, 0x1207u);

  const uint32_t bareLost = bare.before > bare.after ? bare.before - bare.after : 0u;
  const uint32_t ironLost = iron.before > iron.after ? iron.before - iron.after : 0u;
  const float ironPerHit =
      iron.before ? (float)ironLost / (float)iron.before / (float)kHits : 0.0f;

  RecordObserved("impactFistBareLost", (double)bareLost);
  RecordObserved("impactFistBareBruise", (double)bare.bruise);
  RecordObserved("impactFistIronLost", (double)ironLost);
  RecordObserved("impactFistIronPerHitFraction", (double)ironPerHit);

  // "Punching never dismembers, it bloodies a spot if you hit it repeatedly."
  // Zero voxels is an ABSOLUTE claim and it is the right one here: bluntCarve
  // 0 means CarveLimbRadial is never called at all, so anything above zero is
  // a dispatch bug rather than a tuning question.
  const bool bareTookNothing = bareLost == 0 && bare.attached;
  const bool bareMarked = bruiseMat != 0 && bare.bruise > 0;
  // "...with iron gauntlets it also deletes voxels." Both halves: something
  // came off, and it came off SLOWLY — bounded above by the fraction ONE
  // sword cut takes, which is the wound gates' own observed number and is
  // what "slowly" has to mean if it is to mean anything.
  const double slowCap = BaselineNumber("woundChipMaxFraction", 0.35);
  const bool ironBit = ironLost > 0 && iron.attached;
  const bool ironSlow = ironPerHit < (float)slowCap;

  const bool ok = bareTookNothing && bareMarked && ironBit && ironSlow;
  detail = Format(
      "%s/%s x%d each: BARE FIST (blunt %.1f, dent 0) took %u voxels and "
      "bruised %u | GAUNTLET (blunt %.1f, dent %.2f) took %u of %u, %.3f%% per "
      "hit (cap %.1f%%), attached=%d",
      t.defName.c_str(), t.limbName.c_str(), kHits, fist.blunt, bareLost,
      bare.bruise, gaunt->strike.blunt, gaunt->strike.bluntCarve, ironLost,
      iron.before, ironPerHit * 100.0f, slowCap * 100.0, iron.attached ? 1 : 0);
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// bite-rot — a zombie's tear infects flesh, and armour refuses it
// ---------------------------------------------------------------------------
Status GateBiteRot(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 405));
  if (!t.valid() || t.torso < 0) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  // BY NAME, at runtime, exactly as the wound model resolves them: a gate that
  // hardcoded an id would be pinned to this week's material file order.
  const uint32_t rotMat = c.mobs.MaterialIdNamed("rotflesh");
  const uint32_t ichor = c.mobs.MaterialIdNamed("ichor");
  if (!rotMat || !ichor) {
    detail = "materials.json has no `rotflesh` / `ichor`";
    return Status::Skip;
  }
  const uint32_t rotStain = mobs.StainTypeOf(ichor);
  if (!rotStain) {
    detail = "`ichor` declares no stain block — a bite would smear nothing";
    return Status::Fail;
  }

  const int kHits = (int)BaselineNumber("biteRotHits", 10);
  const float kHp = (float)BaselineNumber("biteRotHp", 7.0);
  const float kPower = 0.85f;
  std::vector<ParticleSpawn> spawns;

  // ---- ARM A: BARE FLESH --------------------------------------------------
  uint32_t rot = 0, stained = 0, lost = 0;
  float bleed = 0;
  bool attached = false;
  int bites = 0;
  {
    const uint64_t id = SpawnTarget(c, t, 415);
    if (!id) {
      detail = "spawn refused";
      return Status::Fail;
    }
    const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
    const uint32_t before = mobs.LimbArtVoxelCount(id, t.limb);
    const Vec3 at = ax.anchor + ax.along * (ax.reach * 0.5f);
    // ---- READ AFTER EVERY BITE, NOT AT THE END -----------------------------
    //
    // Because the limb is SUPPOSED to come off eventually: a bite severs by
    // collapse, which is the one rule separating it from a punch
    // (Mob::BiteScope). The first version of this arm bit ten times and then
    // measured, found the limb already gone, and reported four zeroes -- a
    // correct measurement of nothing. So the state is kept from the last bite
    // that LEFT THE LIMB ON THE BODY, and the sever is asserted separately as
    // the contrast it is.
    for (int k = 0; k < kHits; k++) {
      if (!mobs.LimbBody(id, t.limb)) break;
      BiteOnce(mobs, c.world, id, t.limb, at, kHp, (uint16_t)rotMat,
               (uint16_t)ichor, kPower, 0xB17Eu + (uint32_t)k * 2654435761u,
               spawns);
      if (!mobs.LimbBody(id, t.limb)) break;
      bites = k + 1;
      rot = mobs.LimbMaterialCount(id, t.limb, rotMat);
      stained = mobs.LimbStainCount(id, t.limb, 1);
      bleed = mobs.LimbBleedBudget(id, t.limb);
      lost = before - std::min(before, mobs.LimbArtVoxelCount(id, t.limb));
    }
    attached = mobs.LimbBody(id, t.limb) != 0;
    mobs.Reset();
    c.debris.Reset();
  }

  // ---- ARM B: THE SAME TEETH ON A PLATE -----------------------------------
  //
  // ARMOUR DEFENDS, and the zero this arm measures is the whole of it. Same
  // bite, same infection, onto the SHELL's body handle rather than the limb's
  // — which is what a sweep that met a cuirass would hand over.
  const ItemDef* cuirass = c.items.At(c.items.Find("iron_cuirass"));
  uint32_t shellRot = 0, hostRot = 0;
  float shellHostHp0 = 0, shellHostHp1 = 0;
  bool wore = false;
  if (cuirass) {
    const int chestSlot = (int)EquipSlotId::Chest;
    const uint64_t id = SpawnTarget(c, t, 425);
    Mob* m = id ? mobs.FindMobById(id) : nullptr;
    if (m && chestSlot >= 0 && m->WearItem(cuirass, chestSlot)) {
      int shell = -1;
      for (int li = m->AppendedBase(); li < m->LimbCount(); li++)
        if (m->WornHostOf(li) == t.torso) { shell = li; break; }
      if (shell >= 0) {
        wore = true;
        shellHostHp0 = mobs.LimbHp(id, t.torso);
        const Vec3 at = mobs.LimbVoxelPos(id, shell, 11u);
        for (int k = 0; k < kHits; k++) {
          if (!mobs.LimbBody(id, shell)) break;
          BiteOnce(mobs, c.world, id, shell, at, kHp, (uint16_t)rotMat,
                   (uint16_t)ichor, kPower,
                   0xA12Du + (uint32_t)k * 2654435761u, spawns);
        }
        shellRot = mobs.LimbMaterialCount(id, shell, rotMat);
        hostRot = mobs.LimbMaterialCount(id, t.torso, rotMat);
        shellHostHp1 = mobs.LimbHp(id, t.torso);
      }
    }
    mobs.Reset();
    c.debris.Reset();
  }

  // ---- ARM C: THE SAME TEETH ON A TUNIC -----------------------------------
  //
  // THE CONTRAST ARM B NEEDED. Arm B's zero was read as "armour defends", and
  // for a plate it is -- but the rule it was testing was `IsWornSlot`, which is
  // true of a LINEN SHIRT as well, and nothing in this gate could tell the two
  // apart. So a tunic was as bite-proof as steel: teeth on a clothed chest ran
  // no tear, carried no infection and left a faint bruise, which is exactly
  // what the owner reported as not being able to detect a bite at all.
  //
  // Same fixture, same bites, same infection as arm B -- only the GARMENT
  // differs, so a difference in the reading can only be the material. Linen is
  // hardness 4 against iron's 160 and the ramp is gear.biteThroughSoft(14)
  // ..biteThroughHard(60), so this arm must infect the flesh underneath while
  // arm B still must not. Two arms that share a column prove nothing (that is
  // its own gotcha); these two share every column but the one under test.
  const ItemDef* tunic = c.items.At(c.items.Find("tunic"));
  uint32_t clothHostRot = 0;
  float clothHostHp0 = 0, clothHostHp1 = 0;
  bool woreCloth = false;
  if (tunic) {
    const int chestSlot = (int)EquipSlotId::Chest;
    const uint64_t id = SpawnTarget(c, t, 435);
    Mob* m = id ? mobs.FindMobById(id) : nullptr;
    if (m && chestSlot >= 0 && m->WearItem(tunic, chestSlot)) {
      int shell = -1;
      for (int li = m->AppendedBase(); li < m->LimbCount(); li++)
        if (m->WornHostOf(li) == t.torso) { shell = li; break; }
      if (shell >= 0) {
        woreCloth = true;
        clothHostHp0 = mobs.LimbHp(id, t.torso);
        clothHostHp1 = clothHostHp0;
        const Vec3 at = mobs.LimbVoxelPos(id, shell, 11u);
        // ---- READ AFTER EVERY BITE, FOR ARM A'S REASON ---------------------
        //
        // The torso is VITAL, and teeth that reach it through linen reach it
        // at full strength: ten bites of 7 hp against 60 killed the limb, and
        // the end-of-loop read then reported `0 rotflesh, hp -1` -- a correct
        // measurement of nothing, and the exact trap arm A's own note warns
        // about. The state kept is the last from a bite that LEFT THE HOST
        // ALIVE, which is what "the rot reached the flesh" is a claim about.
        for (int k = 0; k < kHits; k++) {
          if (!mobs.LimbBody(id, shell) || !mobs.LimbBody(id, t.torso)) break;
          BiteOnce(mobs, c.world, id, shell, at, kHp, (uint16_t)rotMat,
                   (uint16_t)ichor, kPower,
                   0xC107u + (uint32_t)k * 2654435761u, spawns);
          if (!mobs.LimbBody(id, t.torso)) break;
          clothHostRot = mobs.LimbMaterialCount(id, t.torso, rotMat);
          clothHostHp1 = mobs.LimbHp(id, t.torso);
        }
      }
    }
    mobs.Reset();
    c.debris.Reset();
  }
  RecordObserved("biteRotThroughClothCells", (double)clothHostRot);

  RecordObserved("biteRotCells", (double)rot);
  RecordObserved("biteRotStainedCells", (double)stained);
  RecordObserved("biteRotLostVoxels", (double)lost);
  RecordObserved("biteRotBleedBudget", (double)bleed);
  RecordObserved("biteRotOnArmourCells", (double)(shellRot + hostRot));

  // A TEAR IS A HOLE: voxels really leave, and what is left around it is
  // rewritten to the biter's material and smeared with its liquid.
  const bool tore = lost > 0 && bites > 0;
  const bool infected = rot > 0;
  const bool wet = stained > 0;
  // ...and it BLEEDS, unlike a punch. This is the line that separates a bite
  // from trauma: it is an open wound and Mob::BiteHit opens no BluntCarveScope.
  const bool bled = bleed > 0.0f;
  // ARMOUR DEFENDS: not one rotten voxel, on the plate or on the man inside
  // it — and he was still hit, which is what makes the zero a refusal rather
  // than a miss.
  const bool defended = !wore || (shellRot == 0 && hostRot == 0);
  const bool landed = !wore || shellHostHp1 < shellHostHp0;

  // A BITE IS A WOUND, NOT AN AMPUTATION -- and this is the assertion that
  // earned its place. The collapse sever is left ON for a bite (Mob::BiteScope)
  // precisely so that enough of them take a hand off, and the first version of
  // this gate asserted exactly that. It was the wrong claim: with the plan's
  // starting radius of 1.1 world voxels ONE bite took 936 of a thigh's 1344
  // voxels and the SECOND collapsed it, so the assertion passed while
  // describing an amputation with teeth -- and it passed only sometimes,
  // because a different mob id moved the noise enough to collapse the limb on
  // bite one and leave every reading at zero. gore.biteRadius is now 0.45 and
  // the claim is the other way round: N bites at one spot hurt, infect and
  // bleed, and the limb is STILL THERE afterwards.
  //
  // How many it does take is RECORDED rather than asserted: "enough bites
  // sever" wants a band and a cap, which is wound-accumulate's shape, and a
  // second copy of that gate is not worth the run time.
  RecordObserved("biteRotBitesToSever", (double)(attached ? 0 : bites + 1));
  const bool held = attached && bites == kHits;

  // CLOTH IS NOT ARMOUR: teeth through a tunic must reach the flesh under it
  // and infect it. The hp drop alone would not prove it -- arm B's plate also
  // drops host hp, through the blunt share -- so the claim is the ROT, which
  // is the one thing `IsWornSlot` used to refuse unconditionally.
  const bool bitThroughCloth = !woreCloth || clothHostRot > 0;
  const bool ok = tore && infected && wet && bled && defended && landed &&
                  held && bitThroughCloth;
  detail = Format(
      "%s/%s x%d bites (%.1f hp, rotflesh + ichor): %d landed, %u voxels torn "
      "out, %u rotflesh, %u stained, bleed budget %.2f, limb %s | on an iron "
      "cuirass: %u rotflesh on the plate, %u on the body under it, host hp "
      "%.1f -> %.1f (%s) | through a tunic: %u rotflesh on the body under it, "
      "host hp %.1f -> %.1f (%s)",
      t.defName.c_str(), t.limbName.c_str(), kHits, kHp, bites, lost, rot,
      stained, bleed, attached ? "still attached" : "TORN OFF", shellRot,
      hostRot, shellHostHp0, shellHostHp1,
      wore ? "worn" : "no cuirass — armour arm skipped", clothHostRot,
      clothHostHp0, clothHostHp1,
      woreCloth ? "worn" : "no tunic — cloth arm skipped");
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// bite-infect — the rot a bite leaves GROWS, and it EATS the limb
// ---------------------------------------------------------------------------
//
// `bite-rot` above asserts the tear and the rewrite: that a bite leaves rotten
// flesh behind and that armour refuses it. It says nothing about what happens
// NEXT, and until 2026-09-16 the answer was "nothing at all" -- the rewrite sat
// at exactly the size the teeth left it, forever.
//
// A DIFFERENTIAL WITH A DATA-ONLY CONTROL ARM. Both arms bite the same limb of
// the same creature with the same seed and then run the same number of ticks;
// the ONLY difference is that arm B sets both rates to 0, which is the shipped
// "off" switch and therefore also the pre-feature engine. So a difference in
// the reading can only be the pass, and a broken pass cannot pass by accident:
// arm A's assertions are the exact statements arm B is asserted to contradict.
//
// RATES, NOT THE DEFAULTS. At the shipped 1.0 / 0.5 vox/min this gate would
// have to run for minutes; it cranks both and asserts the DIRECTION, because
// the amount is a tuning question and pinning it would make every retune a
// gate failure. What is bounded on purpose is the rot: hard enough to be
// unmistakable, gentle enough that the limb does not collapse-sever inside the
// window -- a gate whose fixture is destroyed before it measures reports a
// correct measurement of nothing (see arm A of `bite-rot` for the last time
// that happened here).
Status GateBiteInfect(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 405));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  const uint32_t rotMat = c.mobs.MaterialIdNamed("rotflesh");
  const uint32_t ichor = c.mobs.MaterialIdNamed("ichor");
  // Not required: a mob set with no `bone` simply skips the coat claim below
  // rather than failing a gate about the infection's clock.
  const uint32_t boneMat = c.mobs.MaterialIdNamed("bone");
  if (!rotMat || !ichor) {
    detail = "materials.json has no `rotflesh` / `ichor`";
    return Status::Skip;
  }

  const Tuning saved = CurrentTuning();
  const int kTicks = (int)BaselineNumber("biteInfectTicks", 300);
  const float kSpread = (float)BaselineNumber("biteInfectSpread", 6.0);
  const float kRot = (float)BaselineNumber("biteInfectRot", 2.0);

  struct Run {
    uint32_t rot0 = 0, rot1 = 0;   // infected voxels, after the bite / after
    uint32_t vox0 = 0, vox1 = 0;   // the limb's art voxels, same two moments
    uint32_t elsewhere = 0;        // rot on OTHER limbs at the end (the jump)
    uint32_t maxStep = 0;          // most voxels gained on any ONE tick
    uint32_t movedTicks = 0;       // ticks on which anything changed at all
    // ---- BONE THE ROT UNCOVERED (2026-09-17) ------------------------------
    // Coated BONE voxels, after the bite and at the end. The bite's own soak
    // already floors a coat on the bone inside its radius (CutSoak::boneMin),
    // so the claim can only ever be the DELTA -- bone the ROT exposed, past
    // where the teeth reached, is bone nothing has bloodied unless
    // Mob::InfectStep does it. `boneDeep1` is the same count at a high
    // threshold, which is how a FLAT coat is told from a varied one.
    uint32_t bone0 = 0, bone1 = 0, boneDeep1 = 0;
    // Cumulative coats applied by the infection (MobLimb::infectBoneCoated).
    uint32_t boneCoats = 0;
    bool attached = false;
  };
  auto run = [&](float spread, float rot, int inset) {
    Run r;
    Tuning tt = saved;
    tt.gore.infectSpreadRate = spread;
    tt.gore.infectRotRate = rot;
    // ...AND THE MULTIPLIER THAT SITS ON TOP OF BOTH. `infectMobMult` is a
    // debug crank (watch a bite advance without retuning the base rates) and
    // it multiplies exactly the two numbers this gate authors, so leaving it
    // at whatever tuning.json happens to hold means the arm is not the arm.
    // At 4.8 -- a value this tree was carrying -- arm A ran at 28.8 vox/min,
    // ate a thigh in 114 ticks, collapse-severed the limb and reported "the
    // infection did not spread", which is a correct measurement of a stump
    // (the same failure arm A of `bite-rot` has its own note about). A gate
    // that sets a rate has to set every factor of it.
    tt.gore.infectMobMult = 1.0f;
    SetCurrentTuning(tt);
    std::vector<ParticleSpawn> spawns;
    const uint64_t id = SpawnTarget(c, t, inset);
    if (!id) { SetCurrentTuning(saved); return r; }
    const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
    const Vec3 at = ax.anchor + ax.along * (ax.reach * 0.5f);
    BiteOnce(mobs, c.world, id, t.limb, at, 4.0f, (uint16_t)rotMat,
             (uint16_t)ichor, 0.85f, 0xB17Eu, spawns);
    r.rot0 = mobs.LimbMaterialCount(id, t.limb, rotMat);
    r.vox0 = mobs.LimbArtVoxelCount(id, t.limb);
    if (boneMat) r.bone0 = mobs.LimbStainedMatCount(id, t.limb, boneMat, 1);
    // SEEDED, not left at 0: the per-tick step below diffs against the previous
    // reading, and starting from zero would score the bite's own 65 voxels as
    // the first tick's step and make the burst assertion unfailable.
    r.rot1 = r.rot0;
    r.vox1 = r.vox0;
    r.bone1 = r.bone0;
    uint32_t tick = 40000;
    for (int i = 0; i < kTicks; i++) {
      if (!mobs.LimbBody(id, t.limb)) break;
      std::vector<BrushOp> ops;
      std::vector<CellOp> cellOps;
      spawns.clear();
      mobs.PreTick(tick++, c.world, ops, cellOps, spawns);
      // Kept from the last tick the limb was STILL ON, for arm A of
      // `bite-rot`'s reason: reading after a collapse measures a stump.
      const uint32_t was = r.rot1;
      r.rot1 = mobs.LimbMaterialCount(id, t.limb, rotMat);
      r.vox1 = mobs.LimbArtVoxelCount(id, t.limb);
      // Read every tick, for the reason the two above are: the last reading
      // taken while the limb was STILL ON is the one that means anything, and
      // a count taken after a collapse-sever is a count of a stump.
      if (boneMat) {
        r.bone1 = mobs.LimbStainedMatCount(id, t.limb, boneMat, 1);
        r.boneDeep1 = mobs.LimbStainedMatCount(id, t.limb, boneMat, 14);
        // ...AND THE COAT AS AN ACT, NOT AS A POPULATION (2026-09-18).
        // The two censuses above stopped being able to answer "did the rot
        // bloody what it uncovered" the day the rot was allowed to EAT bone
        // (materials.json bone.rotRate): a coated bone voxel is converted to
        // rotflesh a few ticks later and leaves the census, so a coat that is
        // working perfectly reads as a FALLING count -- observed 9 -> 7. This
        // counts every coat applied and never decrements.
        r.boneCoats = mobs.LimbInfectBoneCoated(id, t.limb);
      }
      // ---- HOW MUCH MOVES IN ONE TICK ---------------------------------------
      // The reading that catches the defect the first version shipped with. It
      // banked the fractional voxels owed and spent them in bursts of ~32, so
      // the AVERAGE was right and every assertion above passed while the rot
      // visibly advanced in slabs. A rate realised by chance moves 0, 1 or 2
      // voxels a tick at this arm's rate; a rate realised in instalments moves
      // nothing for a hundred ticks and then a slab. Only the average would
      // ever have distinguished them, and the average is the thing both get
      // right -- so the gate has to look at the STEP.
      if (r.rot1 > was) {
        r.maxStep = std::max(r.maxStep, r.rot1 - was);
        r.movedTicks++;
      }
    }
    r.attached = mobs.LimbBody(id, t.limb) != 0;
    // The JOINT JUMP, observed and deliberately not asserted: whether it fires
    // inside this window depends on how fast the bitten limb saturates, which
    // is the one number here that is properly a tuning question.
    if (const Mob* m = mobs.FindMobById(id))
      for (int li = 0; li < m->LimbCount(); li++)
        if (li != t.limb) r.elsewhere += mobs.LimbMaterialCount(id, li, rotMat);
    mobs.Reset();
    c.debris.Reset();
    SetCurrentTuning(saved);
    return r;
  };

  const Run a = run(kSpread, kRot, 415);
  const Run b = run(0.0f, 0.0f, 425);
  SetCurrentTuning(saved);

  RecordObserved("biteInfectRotAfterBite", (double)a.rot0);
  RecordObserved("biteInfectRotGrown", (double)a.rot1);
  RecordObserved("biteInfectVoxelsEaten",
                 (double)(a.vox0 - std::min(a.vox0, a.vox1)));
  RecordObserved("biteInfectAcrossJoint", (double)a.elsewhere);
  RecordObserved("biteInfectControlRot", (double)b.rot1);
  RecordObserved("biteInfectBoneCoatedAtBite", (double)a.bone0);
  RecordObserved("biteInfectBoneCoated", (double)a.bone1);
  RecordObserved("biteInfectBoneCoatedDeep", (double)a.boneDeep1);
  RecordObserved("biteInfectBoneCoats", (double)a.boneCoats);
  RecordObserved("biteInfectControlVoxels",
                 (double)(b.vox0 - std::min(b.vox0, b.vox1)));

  if (a.rot0 == 0) {
    detail = "the bite left no infection at all — this is `bite-rot`'s job";
    return Status::Fail;
  }
  // 1. IT GROWS. More of the limb is rotten than the teeth made rotten.
  const bool grew = a.rot1 > a.rot0;
  // 2. IT EATS. Voxels really left the limb, and the limb is still attached —
  //    a severed limb would be a different (and much cruder) claim.
  const bool ate = a.vox1 < a.vox0;
  // 3. ...AND NEITHER HAPPENS WITH THE RATES AT 0, which is what makes 1 and 2
  //    statements about this pass rather than about the bite, the burn, or
  //    anything else PreTick does to a creature for 300 ticks.
  const bool controlStill = b.rot1 <= b.rot0 && b.vox1 >= b.vox0;
  // 4. IT ARRIVES A VOXEL AT A TIME, NOT IN SLABS. The one assertion the first
  //    shipped version would have failed: at this arm's spread rate the pass
  //    owes ~1.7 lattice voxels a tick, so an honest per-tick draw can only
  //    ever hand over 1 or 2 and the ceiling is generous headroom on that. The
  //    burst version owed the same average and delivered it 32 at a time.
  const uint32_t kMaxStep = (uint32_t)BaselineNumber("biteInfectMaxStep", 4);
  const bool gradual = a.maxStep <= kMaxStep;
  // 5. WHAT IT UNCOVERS IS BLOODIED, NOT WHITE. The rot eats tissue and cannot
  //    touch bone, so a limb it works through ends as a skeleton -- and before
  //    2026-09-17 that skeleton was the authored near-white, because the only
  //    thing that had ever coated bone was a CUT's soak and its radius ends
  //    where the teeth stopped. Asserted as a delta against the reading taken
  //    the instant after the bite for exactly that reason: `bone0` is the
  //    teeth's work, everything past it is the infection's.
  //    Measured as COATS APPLIED rather than as coated voxels surviving: the
  //    rot eats bone now, so a voxel this pass bloodies is converted a few
  //    ticks later and drops out of any census of it (see the note at the
  //    reading). `boneCoats` only ever rises, so it states the claim -- the rot
  //    bloodied bone it uncovered -- without the answer depending on whether
  //    that bone is still standing at the end of the window.
  const bool bonedelta = boneMat == 0 || a.boneCoats > 0;
  //    ...and the control arm proves it is the ROT doing it: with both rates
  //    at 0 nothing new is uncovered, so nothing new is coated. The bite's own
  //    soak is not this pass and does not touch the counter.
  const bool controlBone = boneMat == 0 || b.boneCoats == 0;
  //    ...AND IT IS NOT ONE FLAT COLOUR. The coat is an alpha over the bone's
  //    own shade, so a varied amount is what keeps the exposure reading as
  //    bone under gore rather than as a slab of paint (gore.infectBoneStainVary
  //    is the row, and 0 there is the failure this catches). With the shipped
  //    11 +/- 5 roughly a fifth of the cells land at 14 or 15: all of them
  //    would mean the mean is pinned at the ceiling, none of them means the
  //    jitter is gone. Only asked once the sample is big enough for the
  //    fraction to mean anything -- a gate arm that turns on three voxels is a
  //    knife edge, not a claim.
  const uint32_t kVaryMin = (uint32_t)BaselineNumber("biteInfectBoneVaryMin", 12);
  const bool varied = boneMat == 0 || a.bone1 < kVaryMin ||
                      (a.boneDeep1 > 0 && a.boneDeep1 < a.bone1);

  std::string s = "grew " + std::to_string(a.rot0) + "->" +
                  std::to_string(a.rot1) + " rot over " +
                  std::to_string(a.movedTicks) + " ticks (max " +
                  std::to_string(a.maxStep) + "/tick), ate " +
                  std::to_string(a.vox0 - std::min(a.vox0, a.vox1)) +
                  " vox (limb " + (a.attached ? "on" : "OFF") + ", " +
                  std::to_string(a.elsewhere) + " across joints); control " +
                  std::to_string(b.rot0) + "->" + std::to_string(b.rot1) +
                  " rot, " + std::to_string(b.vox0) + "->" +
                  std::to_string(b.vox1) + " vox; bone coated " +
                  std::to_string(a.bone0) + "->" + std::to_string(a.bone1) +
                  " (" + std::to_string(a.boneDeep1) + " deep), control " +
                  std::to_string(b.bone0) + "->" + std::to_string(b.bone1) +
                  "; rot applied " + std::to_string(a.boneCoats) +
                  " bone coats (control " + std::to_string(b.boneCoats) + ")";
  detail = s;
  if (grew && ate && controlStill && gradual && bonedelta && controlBone &&
      varied)
    return Status::Pass;
  if (!grew) detail = "the infection did not spread: " + s;
  else if (!ate) detail = "the infection took no voxels: " + s;
  else if (!controlStill)
    detail = "the CONTROL arm moved with both rates at 0: " + s;
  else if (!bonedelta)
    detail = "the rot uncovered bone and left it CLEAN (white): " + s;
  else if (!controlBone)
    detail = "the CONTROL arm's rot applied bone coats with both rates at 0: " + s;
  else if (!varied)
    detail = "the bone coat is one FLAT amount, not a spectrum: " + s;
  else
    detail = "the rot arrived in SLABS, not voxel by voxel (max " +
             std::to_string(a.maxStep) + "/tick, ceiling " +
             std::to_string(kMaxStep) + "): " + s;
  return Status::Fail;
}

// ---------------------------------------------------------------------------
// joint-rot — a limb whose ATTACHMENT has been eaten comes off (2026-09-19)
// ---------------------------------------------------------------------------
//
// `bite-infect` asserts the rot's clock; this asserts its STRUCTURAL
// consequence, which had none. Owner report: "all of the voxels connecting a
// shoulder to the torso can get rotted off but the limb is still attached" —
// and the reason was that the only dismemberment rule rot could reach was
// whole-limb volume (kLimbCollapseFraction, 25%), while an infection that opens
// a hole THROUGH a shoulder removes a few percent of an arm. The joint rule
// that makes "cut at the joint" work for a sword was gated `inBladeCut_`.
//
// THE TEETH GO INTO THE TORSO AND THE ARM IS WHAT COMES OFF. That is the exact
// shape of the report, and it is also the only fixture that states the claim
// UNAMBIGUOUSLY. Biting the arm itself does not: rot that hollows a limb splits
// it, the anchor component keeps the identity, and the pre-existing
// kMinFragmentVoxels rule takes it off on its own — measured, a control arm
// with this whole feature switched off lost the limb 21 ticks later by that
// route, at the same 80% volume, which is two readings of one event and not a
// differential. Nothing eats the ARM here, so nothing but the socket can
// explain it leaving.
//
// THE CLAIM IS ABOUT THE JOINT, NOT ABOUT THE VOLUME, so the assertion is a
// conjunction: the limb came OFF, the creature was ALIVE when it did (Die()
// takes every limb with it, so "no body" alone is not an amputation), and the
// limb still had most of ITSELF when it went.
//
// A DATA-ONLY CONTROL ARM (the pattern `bite-infect` uses and for its reason):
// both arms bite the same torso at the same shoulder with the same seed and the
// same rot rates, and the ONLY difference is `gore.woundNeckRadius = 0` in arm
// B, which is the shipped "off" switch for the joint measure itself
// (Mob::JointAttached returns early). So arm A's sever cannot be scored to the
// bite, the burn, the blood or anything else PreTick does for 600 ticks.
Status GateJointRot(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 465));
  if (!t.valid() || t.torso < 0 || t.torso == t.limb) {
    detail = "no loaded mob def has a severable non-vital limb on a torso";
    return Status::Fail;
  }
  const uint32_t rotMat = c.mobs.MaterialIdNamed("rotflesh");
  const uint32_t ichor = c.mobs.MaterialIdNamed("ichor");
  if (!rotMat || !ichor) {
    detail = "materials.json has no `rotflesh` / `ichor`";
    return Status::Skip;
  }

  const Tuning saved = CurrentTuning();
  const int kTicks = (int)BaselineNumber("jointRotTicks", 600);
  const float kSpread = (float)BaselineNumber("jointRotSpread", 12.0);
  const float kRot = (float)BaselineNumber("jointRotRate", 8.0);

  struct Run {
    uint32_t rot0 = 0;      // infected voxels the bite left (0 = it MISSED)
    uint32_t vox0 = 0;      // the limb's art voxels the instant after the bite
    uint32_t voxLast = 0;   // ...and on the last tick it was still attached
    int offAt = -1;         // tick the limb came off, or -1
    // ...AND WAS THE CREATURE STILL ALIVE WHEN IT DID. Without this the gate
    // cannot fail: Die() hands every limb to the debris system, so `LimbBody`
    // goes null on DEATH as well as on amputation, and both arms of the first
    // version were really measuring how fast the bite's blood loss killed the
    // fixture (98 ticks vs 119, 83% vs 80% — two readings of the same event).
    bool aliveAtOff = false;
  };
  auto run = [&](float neckRadius, int inset) {
    Run r;
    Tuning tt = saved;
    tt.gore.infectSpreadRate = kSpread;
    tt.gore.infectRotRate = kRot;
    // Every factor of the rate, for the reason `bite-infect` spells out: a
    // debug crank left at whatever tuning.json holds means the arm is not the
    // arm.
    tt.gore.infectMobMult = 1.0f;
    tt.gore.woundNeckRadius = neckRadius;
    SetCurrentTuning(tt);
    std::vector<ParticleSpawn> spawns;
    const uint64_t id = SpawnTarget(c, t, inset);
    if (!id) { SetCurrentTuning(saved); return r; }
    // ---- THE TEETH GO INTO THE TORSO, AT THE ARM'S SHOULDER ---------------
    // `LimbAnchorPos` of the ARM is the joint in world space — the same point
    // the socket measure is taken about, expressed in the parent's frame. A
    // generous bite radius because the anchor is a RIG POINT and may sit just
    // outside the torso's voxel cloud (Mob::SocketCentreInParent clamps for
    // exactly that reason); `rot0` below is what proves the teeth found flesh.
    const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
    BiteOnce(mobs, c.world, id, t.torso, ax.anchor, 5.0f, (uint16_t)rotMat,
             (uint16_t)ichor, 0.85f, 0xB17Eu, spawns);
    r.rot0 = mobs.LimbMaterialCount(id, t.torso, rotMat);
    // ...and every reading after this is about the ARM, which nothing has
    // touched. If it leaves, the socket is the only thing that can have taken
    // it.
    r.vox0 = mobs.LimbArtVoxelCount(id, t.limb);
    r.voxLast = r.vox0;
    uint32_t tick = 40000;
    for (int i = 0; i < kTicks; i++) {
      if (!mobs.LimbBody(id, t.limb)) {
        r.offAt = i;
        const Mob* m = mobs.FindMobById(id);
        r.aliveAtOff = m && m->Alive();
        break;
      }
      // The last reading taken while the limb was STILL ON is the only one
      // that means anything — after the sever this counts a stump.
      r.voxLast = mobs.LimbArtVoxelCount(id, t.limb);
      std::vector<BrushOp> ops;
      std::vector<CellOp> cellOps;
      spawns.clear();
      mobs.PreTick(tick++, c.world, ops, cellOps, spawns);
    }
    mobs.Reset();
    c.debris.Reset();
    SetCurrentTuning(saved);
    return r;
  };

  const Run a = run(saved.gore.woundNeckRadius, 475);
  const Run b = run(0.0f, 485);
  SetCurrentTuning(saved);

  const auto frac = [](const Run& r) {
    return r.vox0 ? (float)r.voxLast / (float)r.vox0 : 0.0f;
  };
  RecordObserved("jointRotOffAt", (double)a.offAt);
  RecordObserved("jointRotFracAtSever", (double)frac(a));
  RecordObserved("jointRotControlOffAt", (double)b.offAt);

  // How much of itself the limb must still have when it leaves, for the sever
  // to be a statement about the JOINT rather than about the volume. Comfortably
  // above kLimbCollapseFraction (25%), which is the rule that already worked.
  const float kFracMin = (float)BaselineNumber("jointRotFracMin", 0.5);
  // AN AMPUTATION, NOT A DEATH — see Run::aliveAtOff.
  const bool cameOff = a.offAt >= 0 && a.aliveAtOff;
  const bool stillALimb = frac(a) >= kFracMin;
  // The control must not reach the same verdict, and "it never lost the limb"
  // is the wrong way to say that: with the joint measure off the rot crosses
  // the joint and eventually eats the leg ITSELF, and the old whole-limb
  // collapse rule then takes it — measured at tick 338 with 28% of it left,
  // against arm A's tick 49 with 95%. Both are limbs coming off a live
  // creature, and the number that tells them apart is the one the claim is
  // about: with the joint measure off, nothing takes a limb that is still
  // MOSTLY THERE.
  const bool controlHeld =
      b.offAt < 0 || !b.aliveAtOff || frac(b) < kFracMin;

  std::string s =
      "torso bite left " + std::to_string(a.rot0) + " rotten at the " +
      t.limbName + " joint; the " + t.limbName + " (" +
      std::to_string(a.vox0) + " vox, untouched) came " +
      (a.offAt >= 0 ? "OFF at tick " + std::to_string(a.offAt) +
                          (a.aliveAtOff ? " (alive)" : " (ON DEATH)")
                    : std::string("off NEVER")) +
      " with " + std::to_string((int)(frac(a) * 100.0f)) +
      "% of itself left (floor " + std::to_string((int)(kFracMin * 100.0f)) +
      "%); control (neck measure off) bit " + std::to_string(b.rot0) + ", " +
      (b.offAt < 0 ? std::string("stayed on")
                   : "came off at tick " + std::to_string(b.offAt) +
                         (b.aliveAtOff ? " (alive)" : " (ON DEATH)")) +
      " at " + std::to_string((int)(frac(b) * 100.0f)) + "%";
  detail = s;
  // A BITE THAT MISSED MEASURES NOTHING — and it is the failure this fixture is
  // most likely to have, because the point it aims at is a rig anchor. Checked
  // first so it can never be reported as "the rule did not fire".
  if (a.rot0 == 0 || b.rot0 == 0) {
    detail = "the bite landed no infection — this fixture measured nothing: " + s;
    return Status::Fail;
  }
  if (cameOff && stillALimb && controlHeld) return Status::Pass;
  if (!cameOff)
    detail = a.offAt < 0
                 ? "the rot ate the joint and the limb stayed attached: " + s
                 : "the creature DIED with the limb still on — the rot never "
                   "took it: " + s;
  else if (!stillALimb)
    detail = "the limb collapsed on VOLUME, not at the joint: " + s;
  else
    detail = "the CONTROL arm lost a limb that was still mostly there, with "
             "the neck measure off — this gate's sever is not the joint rule: " + s;
  return Status::Fail;
}

}  // namespace

const std::vector<Gate>& ImpactGates() {
  static const std::vector<Gate> g = {
      {"impact-blunt", "mob", {}, false, GateImpactBlunt, /*needsRender=*/false},
      {"impact-armor", "mob", {}, false, GateImpactArmor, false},
      {"impact-fist", "mob", {}, false, GateImpactFist, false},
      {"bite-rot", "mob", {}, false, GateBiteRot, false},
      {"bite-infect", "mob", {}, false, GateBiteInfect, false},
      {"joint-rot", "mob", {}, false, GateJointRot, false},
  };
  return g;
}

}  // namespace selftest
