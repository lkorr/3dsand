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
  uint32_t before = 0, after = 0, bruise = 0;
  float hp0 = 0, hp1 = 0, bluntBleed = 0;
  bool attached = false, alive = false;
  size_t severs = 0;
  const uint32_t bruiseMat = c.mobs.MaterialIdNamed(CurrentTuning().gore.bruiseMat);
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
    }
    attached = mobs.LimbBody(id, t.limb) != 0;
    alive = mobs.IsAlive(id);
    severs = mobs.SeverEvents().size();
    if (attached) {
      after = mobs.LimbArtVoxelCount(id, t.limb);
      hp1 = mobs.LimbHp(id, t.limb);
      bruise = bruiseMat ? mobs.LimbMaterialCount(id, t.limb, bruiseMat) : 0u;
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
  const bool drier = bluntBleed < cutBleed;
  // ...and the RESOLVER dispatched it: the sweep-driven arm did the same kind
  // of damage through the front door.
  const bool resolver = sweptRan && sweptHpDrop > 0.0f && sweptAttached;

  const bool ok = hurt && sized && heldOn && pastZero && marked && drier &&
                  resolver;
  detail = Format(
      "%s/%s x%d mace(blunt %.0f, dent %.2f): hp %.1f -> %.1f, %u -> %u voxels "
      "(%.1f%%, cap %.0f%%), %u bruised, attached=%d severs=%zu alive=%d | "
      "bleed budget %.2f vs %.2f for the same hp as cuts | through the real "
      "sweep: hp -%.1f, %u voxels, attached=%d",
      t.defName.c_str(), t.limbName.c_str(), kHits, mace.blunt,
      mace.bluntCarve, hp0, hp1, before, after, frac * 100.0f,
      dentMax * 100.0, bruise, attached ? 1 : 0, severs, alive ? 1 : 0,
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
      a.bruise = bruiseMat ? mobs.LimbMaterialCount(id, t.limb, bruiseMat) : 0u;
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

  const bool ok = tore && infected && wet && bled && defended && landed &&
                  held;
  detail = Format(
      "%s/%s x%d bites (%.1f hp, rotflesh + ichor): %d landed, %u voxels torn "
      "out, %u rotflesh, %u stained, bleed budget %.2f, limb %s | on an iron "
      "cuirass: %u rotflesh on the plate, %u on the body under it, host hp "
      "%.1f -> %.1f (%s)",
      t.defName.c_str(), t.limbName.c_str(), kHits, kHp, bites, lost, rot,
      stained, bleed, attached ? "still attached" : "TORN OFF", shellRot,
      hostRot, shellHostHp0, shellHostHp1,
      wore ? "worn" : "no cuirass — armour arm skipped");
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& ImpactGates() {
  static const std::vector<Gate> g = {
      {"impact-blunt", "mob", {}, false, GateImpactBlunt, /*needsRender=*/false},
      {"impact-armor", "mob", {}, false, GateImpactArmor, false},
      {"impact-fist", "mob", {}, false, GateImpactFist, false},
      {"bite-rot", "mob", {}, false, GateBiteRot, false},
  };
  return g;
}

}  // namespace selftest
