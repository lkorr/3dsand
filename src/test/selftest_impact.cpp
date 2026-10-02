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
//                  leave (a dent -- over the ticks AFTER the blows, since
//                  2026-09-19: see Dissolve), and the blood is a fraction of
//                  a cut's.
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
#include "test/tickrig.h"

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

// ---- WHERE TEETH LAND: THE SKIN, NOT THE AXIS (2026-10-01) -----------------
//
// The cross-section `alongFrac` of the way down the limb, pushed out to the
// furthest surviving voxel across the axis -- the surface a probe would have
// stopped on (`bite-limbs`' "skin" aim, shared). A bite aimed at the AXIS is
// a closed cavity in the middle of the limb, and since the anatomy recipe put
// a bone core there (2026-09) that cavity is walled in bone: `bite-rot`,
// `bite-infect` and `zombify` all aimed there, and all read "0 rotflesh"
// because bone is not tissue -- a correct measurement of a bite nothing can
// deliver. SANDVOX_BITE_DEBUG's WoundStats line names that cause directly
// ("N not tissue").
Vec3 SkinAim(MobSystem& mobs, uint64_t id, int li, const LimbAxis& ax,
             float alongFrac = 0.5f) {
  const Vec3 core = ax.anchor + ax.along * (ax.reach * alongFrac);
  Vec3 bestDir{};
  float bestLen = 0.0f;
  for (uint32_t k = 0; k < 64; k++) {
    const Vec3 p = mobs.LimbVoxelPos(id, li, k * 6151u);
    const Vec3 rel = p - core;
    const float t = rel.dot(ax.along);
    // Near THIS cross-section only, so the push-out does not wander to the
    // far end of a tapering limb.
    if (std::fabs(t) > std::max(1.0f, ax.reach * 0.25f)) continue;
    const Vec3 perp = rel - ax.along * t;
    const float len = perp.len();
    if (len > bestLen) {
      bestLen = len;
      bestDir = perp;
    }
  }
  return bestLen > 1e-3f ? core + bestDir.normalized() * bestLen : core;
}

// The surface voxel of limb `li` nearest a world point (MobSystem::
// LimbSurfacePos, sampled): where teeth close on a limb near a joint.
Vec3 SurfaceNear(MobSystem& mobs, uint64_t id, int li, Vec3 p) {
  Vec3 best = p;
  float bestD = 1e30f;
  for (uint32_t k = 0; k < 512; k++) {
    const Vec3 q = mobs.LimbSurfacePos(id, li, k * 7919u + 3u);
    const float d = (q - p).dot(q - p);
    if (d < bestD) {
      bestD = d;
      best = q;
    }
  }
  return best;
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
      // Not hair: a `bloodless` limb (Mob::IsBloodless) is severable,
      // non-vital and can be big, and it never bleeds -- a fixture that
      // expects blood must not land on a long-haired character's mane.
      if (def.limbs[li].bloodless) continue;
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
  // PINNED (the authored `dummy` profile, mobile: false), found by W2-O: the
  // dissolution arms measure a limb on a body standing still, and an
  // unprofiled creature only stood still under the hand-rolled ticks because
  // they never submitted a world and it had no ground. On the real tick the
  // bare-fist arm's body walked 61.9 voxels during its 60 dissolve ticks.
  mobs.SetMobBehavior(id, "dummy");
  // A few physics steps only — no world submit. The creature has to be posed
  // and its limb transforms real before anything is measured off them, and
  // nothing here needs the CA to have run.
  // DIRECT PHASE CALLS ON PURPOSE (W2-O): fixture POSING, not the subject, and
  // every spawn replays ticks 3000.. so the arms' targets are posed alike.
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
  cut.halfWidth = std::max(0.9f * g.cutWidth, g.cutWidthMin);
  cut.depth = (g.cutDepth + g.cutDepthPower * power);
  cut.length = g.cutLength * (0.4f + 0.6f * power);
  cut.power = power;
  cut.seed = seed;
  if (mobs.Damage(body, dmg * power, cut.at, 0.0f, DamageCtx(DamageCause::Blade, power)))
    mobs.CutLimb(body, cut, world, spawns, power);
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

// ---- WHAT THE LIMB STILL HAS (2026-09-19) ---------------------------------
//
// Voxels the limb still HAS, tombstones excluded. A dissolved (or burnt, or
// rotted) voxel is set to material 0 IN PLACE and only compacted out of the
// lattice when Mob::FlushBurn's batch threshold fills (kBurnRebuildFloor, or a
// 64th of the limb), so LimbArtVoxelCount alone lags the dissolution by up to
// one flush and reads "nothing left" for a pulped patch too small to ever fill
// one. The tombstones are counted as material 0 and taken off.
uint32_t LiveVoxels(MobSystem& mobs, uint64_t id, int limb) {
  const uint32_t art = mobs.LimbArtVoxelCount(id, limb);
  const uint32_t dead = mobs.LimbMaterialCount(id, limb, 0u);
  return art > dead ? art - dead : 0u;
}

// ---- THE DISSOLUTION PHASE (2026-09-19, gore.pulpRotRate) ------------------
//
// A dent is no longer carved by the blow that earns it. Since 54ba62b
// Mob::BluntHit only FLAGS the limb (gore.bluntCarveRadius 0 -> 1.1,
// pulpCarveFrom 0.3) and Mob::BluntPulpTick eats the pulped voxels one at a
// time over the ticks that follow, so a fixture that is struck and read in the
// same instant always reads "took 0 voxels" -- which is what impact-blunt and
// impact-fist reported for a day. The fixture has to be TICKED before anything
// can have left.
//
// The rate it is ticked at is an ARM of the gate, not a claim, exactly as
// joint-rot cranks the infection: the shipped 1.5 vox/min makes a thigh's
// crater a minute of sim, and a gate that waited that long for one number
// would cost more than it tells. Both numbers come from tests/baseline.json.
// Blood loss is switched off for the phase (gore.bleedHpPerVoxel 0) because a
// fixture that bleeds white mid-measurement is measuring its own hp, not the
// dent. Stops early when the limb leaves or the creature dies, and says which.
struct DissolveResult {
  bool attached = false;
  bool alive = false;
  int ticks = 0;
};
DissolveResult Dissolve(Ctx& c, uint64_t id, int limb, int ticks, float rate,
                        uint32_t tick0) {
  MobSystem& mobs = c.mobs;
  const Tuning saved = CurrentTuning();
  Tuning tt = saved;
  tt.gore.pulpRotRate = rate;
  tt.gore.bleedHpPerVoxel = 0.0f;
  SetCurrentTuning(tt);
  DissolveResult r;
  // THE REAL TICK (W2-O, test/tickrig.h): the dent is eaten by the body's own
  // tick, and the rest of the tick (the world, the step, the contact pass)
  // runs around it as it does in play. Every arm replays tick0.. so the arms
  // differ by the profile, not by the clock.
  const Vec3 o = mobs.MobOrigin(id);
  uint32_t tick = tick0 - 1;
  support::TickCursor ticker{
      c, tick, IVec3{ifloor(o.x) >> 4, ifloor(o.y) >> 4, ifloor(o.z) >> 4}};
  for (int i = 0; i < ticks; i++) {
    if (!mobs.IsAlive(id) || !mobs.LimbBody(id, limb)) break;
    ticker();
    r.ticks = i + 1;
  }
  r.alive = mobs.IsAlive(id);
  r.attached = mobs.LimbBody(id, limb) != 0;
  SetCurrentTuning(saved);
  return r;
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
  // The contact point the sweep reports, and how far off the struck limb's own
  // axis it ever fell. `sweptHitMissing` is the honest failure: a sweep that hit
  // a body and reported no position at all.
  bool sweptHitReported = false, sweptHitMissing = false;
  float sweptHitOff = 0.0f;
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
    // DIRECT PHASE CALLS ON PURPOSE (W2-O): fixture posing (SpawnTarget's).
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
      // ---- WHERE THE BLOW SAYS IT LANDED (2026-09-19) --------------------
      //
      // The sweep now reports its own contact point, because the IMPACT SOUND is
      // made there and its callers had nothing better than the middle of the
      // blade — half a metre from the wound on a long weapon, which is audible
      // as the blow coming from the wrong place and was reported as exactly
      // that. So: a sweep that hit somebody must SAY where, and the place must
      // be on the limb it hit. Measured against the limb's own axis segment,
      // which is what this fixture already built to aim the blow.
      if (r.bodiesHit > 0 && r.hasHitAt) {
        sweptHitReported = true;
        const Vec3 d = r.hitAt - ax.anchor;
        const float tAlong =
            std::clamp(d.dot(ax.along), 0.0f, std::max(ax.reach, 0.001f));
        const float off = (r.hitAt - (ax.anchor + ax.along * tAlong)).len();
        sweptHitOff = std::max(sweptHitOff, off);
      } else if (r.bodiesHit > 0) {
        sweptHitMissing = true;
      }
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
  //
  // `before` / `afterBlows` / `after` are LIVE counts (LiveVoxels): what the
  // limb has at spawn, the instant the last blow lands, and after the
  // dissolution phase has run. The middle reading is the one the mechanic's
  // change is asserted on -- see `noInstantCarve` below.
  uint32_t before = 0, afterBlows = 0, after = 0, bruise = 0, bruiseOne = 0,
           bruiseDeep = 0;
  float hp0 = 0, hp1 = 0, bluntBleed = 0;
  bool attached = false, alive = false;
  size_t severs = 0;
  // The dissolution ARM (see Dissolve): rate in world voxels/min and how many
  // ticks it is given. 2026-09-19.
  const float kPulpRot = (float)BaselineNumber("impactPulpRot", 30.0);
  const int kPulpTicks = (int)BaselineNumber("impactPulpTicks", 60);
  int dissolveTicks = 0;
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
  // ---- THE LADDER'S MIDDLE RUNG, MEASURED (2026-09-19) --------------------
  //
  // Blood ON the limb, at the depth that counts as pulp -- i.e. skin that the
  // repeated blows BROKE, not blood that ran onto it. The dent below cannot
  // happen without it (Tuning::Gore::pulpCarveFrom), so asserting only "a mace
  // removes something" leaves the interesting failure invisible: the rung
  // shipped with `RaiseBodyStain` refusing every conversion, which no gate
  // could see because `bluntCarveRadius` was 0 as well and the dent claim was
  // therefore red for an unrelated-looking reason.
  const uint32_t bloodMat = mobs.Defs()[t.defIndex].bleedMat;
  // PULPED = the skin SPLIT (voxload.h kBruiseBroken), read off the bruise
  // byte since 2026-09-26 -- it was "blood at gore.pulpAmt" while the bruise
  // was a coat, which a rinse could undo.
  const uint32_t pulpAt = kBruiseBroken;
  uint32_t pulped = 0, pulpedOne = 0;
  {
    const uint64_t id = SpawnTarget(c, t, 415);
    if (!id) {
      detail = "spawn refused";
      return Status::Fail;
    }
    const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
    before = LiveVoxels(mobs, id, t.limb);
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
      if (k == 0 && mobs.LimbBody(id, t.limb)) {
        if (bruiseMat) bruiseOne = mobs.LimbBruiseCount(id, t.limb, 1);
        // ZERO IS THE CLAIM HERE, and it is half of what makes the ladder a
        // ladder: one blow on intact skin bruises and breaks nothing.
        if (bloodMat)
          pulpedOne = mobs.LimbBruiseCount(id, t.limb, pulpAt);
      }
    }
    attached = mobs.LimbBody(id, t.limb) != 0;
    alive = mobs.IsAlive(id);
    severs = mobs.SeverEvents().size();
    if (attached) {
      afterBlows = LiveVoxels(mobs, id, t.limb);
      hp1 = mobs.LimbHp(id, t.limb);
      // THE BRUISE BYTE, NOT THE MATERIAL OR THE COAT (2026-09-26): the voxel
      // keeps its own material and carries the bruise as a 0..15 level on the
      // skin itself (voxload.h PrefabVoxel::bruise). Level 1 is "marked at
      // all".
      bruise = bruiseMat ? mobs.LimbBruiseCount(id, t.limb, 1) : 0u;
      // ...and how many have been driven past a single blow's worth. This is
      // the half that proves the accumulation rather than merely the marking.
      bruiseDeep = (bruiseMat && bruiseCap)
                       ? mobs.LimbBruiseCount(id, t.limb, bruiseCap)
                       : 0u;
      pulped = bloodMat ? mobs.LimbBruiseCount(id, t.limb, pulpAt) : 0u;
      bluntBleed = mobs.LimbBleedBudget(id, t.limb);
      // ---- ...AND THEN IT COMES APART (2026-09-19, gore.pulpRotRate) -------
      //
      // Every reading above is taken the instant the last blow lands, which is
      // where this arm used to stop -- and where, since 54ba62b, nothing has
      // left yet. The dent is now a DISSOLUTION: the ticks below are what let
      // Mob::BluntPulpTick eat the pulped patch, and `after` is the limb once
      // it has. Attachment is re-read afterwards because that is the claim:
      // blunt never amputates, and "never" has to include the crater opening.
      const DissolveResult d =
          Dissolve(c, id, t.limb, kPulpTicks, kPulpRot, 8000u);
      dissolveTicks = d.ticks;
      attached = d.attached;
      alive = d.alive;
      severs = mobs.SeverEvents().size();
      after = attached ? LiveVoxels(mobs, id, t.limb) : 0u;
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

  const uint32_t lostAtBlows =
      before > afterBlows ? before - afterBlows : 0u;
  const uint32_t lost = before > after ? before - after : 0u;
  const float frac = before ? (float)lost / (float)before : 0.0f;
  // THE BAND, NOT THE NUMBER. A dent must remove SOMETHING (a mace with
  // bluntCarve 0.6 that took nothing would be a fist) and must stay well short
  // of the collapse fraction that would have shed the limb — which is the
  // claim, since the sever is refused rather than merely unreached.
  //
  // ...AND NOT IN THE SWING (2026-09-19). `lost` is read AFTER the dissolution
  // phase; `lostAtBlows` the instant the last blow lands, and it must be zero:
  // pulped tissue dissolves over the ticks that follow (gore.pulpRotRate), it
  // does not vanish in one swing. That is the whole of what 54ba62b changed,
  // and the old instant CarveLimbRadial would fail exactly this line.
  const double dentMax = BaselineNumber("impactBluntDentMaxFraction", 0.60);
  RecordObserved("impactBluntLostAtBlows", (double)lostAtBlows);
  RecordObserved("impactBluntLost", (double)lost);
  RecordObserved("impactBluntDissolveTicks", (double)dissolveTicks);
  RecordObserved("impactBluntLostFraction", (double)frac);
  RecordObserved("impactBluntHpDrop", (double)(hp0 - hp1));
  RecordObserved("impactBluntBruiseCells", (double)bruise);
  RecordObserved("impactBluntBruiseAfterOne", (double)bruiseOne);
  RecordObserved("impactBluntBruiseAtCap", (double)bruiseDeep);
  RecordObserved("impactBluntPulpedAfterOne", (double)pulpedOne);
  RecordObserved("impactBluntPulped", (double)pulped);
  RecordObserved("impactBluntBleedBudget", (double)bluntBleed);
  RecordObserved("impactBluntCutBleedBudget", (double)cutBleed);
  RecordObserved("impactBluntSweptLost", (double)sweptLost);

  const bool hurt = hp1 < hp0;
  const bool noInstantCarve = lostAtBlows == 0;
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
  // ---- ...AND IT CLIMBED THE LADDER IN ORDER (2026-09-19) -----------------
  //
  // THREE CLAIMS, and the ORDER is the whole of what is being tested. A blunt
  // weapon that broke skin on contact would pass the second alone; one that
  // dented on contact would pass the dent band alone. What neither can fake is
  // "nothing at all on the first blow, both after two dozen in the same spot".
  const bool openedNothingAtFirst = pulpedOne == 0 && bruiseOne > 0;
  const bool broke = bloodMat != 0 && pulped > 0;
  // ...and the mark did not go ENTIRELY wet. A FLOOR, not a majority, and the
  // difference matters: this arm lands every one of two dozen blows on the
  // SAME point, which is the case the owner wants to be gruesome, so blood
  // outnumbering bruise here is the feature and not a bug. (The "bulk of a
  // mace kill is still bruises" half of the spec is about blows that land in
  // different places, and it holds by construction -- the bruise radius is
  // more than twice the dent's and applies on EVERY blow where the dent
  // applies on few.)
  //
  // What this refuses is measured and specific. The taper used to live only in
  // the STEP, so every cell in the radius eventually crawled to the same
  // global ceiling and the whole patch converted: 431 bruised voxels became 4,
  // with 538 bloodied and a hard edge where the mark used to fade. A bruise
  // ceiling that tapers with the blow (Mob::BruiseLimb, `voxCap`) is the fix,
  // and this is the line that would have caught it.
  const bool spectrum = pulped == 0 || bruise * 2 >= pulped;
  const bool drier = bluntBleed < cutBleed;
  // ...and the RESOLVER dispatched it: the sweep-driven arm did the same kind
  // of damage through the front door.
  const bool resolver = sweptRan && sweptHpDrop > 0.0f && sweptAttached;
  // ...and it said WHERE, on the limb. The tolerance is the fixture's own
  // geometry: a 0.4-voxel blade half-width plus the probe's 0.87-voxel cell
  // reach plus the limb's radius, which for every rig in the game is under 3
  // voxels from its axis. Loose enough not to care which rig ChooseTarget picks,
  // tight enough that a stale midpoint (~5+ voxels away along the blade, and
  // free to be anywhere at all) cannot pass.
  const bool located =
      sweptHitReported && !sweptHitMissing && sweptHitOff < 4.0f;

  const bool ok = hurt && noInstantCarve && sized && heldOn && pastZero &&
                  marked && deepened && concentrated && openedNothingAtFirst &&
                  broke && spectrum && drier && resolver && located;
  detail = Format(
      "%s/%s x%d mace(blunt %.0f, dent %.2f): hp %.1f -> %.1f, %u voxels -> %u "
      "the instant the blows end (%u gone in the swing, must be 0) -> %u after "
      "%d ticks dissolving at %.0f vox/min (%.1f%% gone, cap %.0f%%), bruised "
      "%u after one blow -> %u after %d (%u past %u/15, i.e. more than one "
      "blow), attached=%d severs=%zu alive=%d | ladder: %u pulped (bruise "
      ">= %u/15, split) after one blow -> %u after %d | bleed budget %.2f vs %.2f for "
      "the same hp as cuts | through the real sweep: hp -%.1f, %u voxels, "
      "attached=%d, contact reported=%d/%d, worst %.2f vox off the limb axis",
      t.defName.c_str(), t.limbName.c_str(), kHits, mace.blunt,
      mace.bluntCarve, hp0, hp1, before, afterBlows, lostAtBlows, after,
      dissolveTicks, kPulpRot, frac * 100.0f, dentMax * 100.0, bruiseOne,
      bruise, kHits, bruiseDeep, bruiseCap, attached ? 1 : 0, severs,
      alive ? 1 : 0, pulpedOne, pulpAt, pulped, kHits, bluntBleed, cutBleed,
      sweptHpDrop, sweptLost, sweptAttached ? 1 : 0, sweptHitReported ? 1 : 0,
      sweptHitMissing ? 0 : 1, sweptHitOff);
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

  // ---- ARM C: THE SWORD THROUGH THE REAL SWEEP (2026-09-19) ---------------
  //
  // The two arms above build the kerf ON THE SHELL SLOT by hand, so they
  // measure the hardness rule and nothing about how a stroke FINDS the shell.
  // The owner's report was "my sword is cutting directly through the plate",
  // and it was: the probes tiling a blade that has swept into the body start
  // past the one-micro-thick plate and meet the flesh collider first, and that
  // flesh was struck as if bare. This arm sweeps the shipped sword THROUGH the
  // torso's own centre, exactly as impact-blunt's arm A does with the mace,
  // once dressed and once bare, and the claim is the DIFFERENCE: the bare
  // torso is cut (or the fixture is measuring nothing) and the dressed one
  // keeps its flesh while the plate takes the chips.
  struct Swept {
    uint32_t fleshBefore = 0, fleshLost = 0;
    uint32_t shellBefore = 0, shellLost = 0;
    float hpDrop = 0.0f;
    int covered = 0;
    bool ran = false, wore = false;
  };
  const int kSweptHits = (int)BaselineNumber("impactArmorSweptHits", 3);
  auto sweep = [&](bool dressed) -> Swept {
    Swept r;
    mobs.Reset();
    c.debris.Reset();
    // A wielder that owns nothing in the way, as impact-blunt explains: the
    // sweep refuses to cut its own wielder, so the victim cannot be it.
    const uint64_t wid = mobs.Spawn(t.defIndex, FixtureSite(c.world, 405));
    const uint64_t id = mobs.Spawn(t.defIndex, FixtureSite(c.world, 435));
    if (!wid || !id) return r;
    Mob* m = mobs.FindMobById(id);
    if (!m) return r;
    if (dressed) {
      if (!m->WearItem(cuirass, chestSlot)) return r;
      r.wore = true;
    }
    // DIRECT PHASE CALLS ON PURPOSE (W2-O): fixture posing (SpawnTarget's).
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
    m = mobs.FindMobById(id);
    if (!wielder || !m) return r;
    int shell = -1;
    if (dressed) {
      for (int li = m->AppendedBase(); li < m->LimbCount(); li++)
        if (m->WornHostOf(li) == t.torso) { shell = li; break; }
      if (shell < 0) return r;
      r.shellBefore = mobs.LimbArtVoxelCount(id, shell);
    }
    const LimbAxis ax = MeasureLimb(mobs, id, t.torso);
    r.fleshBefore = mobs.LimbArtVoxelCount(id, t.torso);
    const float hp0 = mobs.LimbHp(id, t.torso);
    const Vec3 mid = ax.anchor + ax.along * (ax.reach * 0.5f);
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
    const float step = mt.fullSpeed * kTickDt * 1.2f;
    uint32_t fleshNow = r.fleshBefore;
    uint32_t shellNow = r.shellBefore;
    float hpNow = hp0;
    for (int k = 0; k < kSweptHits && mobs.LimbBody(id, t.torso); k++) {
      // The edge ends INSIDE the torso, so the sub-steps between the two poses
      // carry the probes through the plate and into the flesh behind it --
      // which is the whole failure path this arm exists to run.
      EdgeSweep sw;
      sw.aPrev = mid - edge * 1.5f - travel * step;
      sw.bPrev = mid + edge * 1.5f - travel * step;
      sw.aNow = mid - edge * 1.5f;
      sw.bNow = mid + edge * 1.5f;
      sw.flatNow = Vec3{};   // alignment 1: the claim is not about the ramp
      sw.dt = kTickDt;
      sw.halfWidth = 0.12f;
      sw.strike = sword->strike;
      sw.heft = 1.0f;
      sw.tick = 7100u + (uint32_t)k;
      sw.valid = true;
      const EdgeSweepResult res = MeleeSweepDamage(sw, mt, *wielder, c.phys,
                                                   mobs, c.debris, c.world,
                                                   spawns);
      if (res.bodiesHit > 0) r.ran = true;
      r.covered += res.probesCovered;
      // Read after every blow, so a control arm whose victim dies of the last
      // cut still reports what the cuts before it took rather than "all of it".
      if (mobs.IsAlive(id) && mobs.LimbBody(id, t.torso)) {
        fleshNow = mobs.LimbArtVoxelCount(id, t.torso);
        hpNow = mobs.LimbHp(id, t.torso);
        if (shell >= 0 && mobs.LimbBody(id, shell))
          shellNow = mobs.LimbArtVoxelCount(id, shell);
      }
    }
    r.fleshLost = r.fleshBefore > fleshNow ? r.fleshBefore - fleshNow : 0u;
    r.shellLost = r.shellBefore > shellNow ? r.shellBefore - shellNow : 0u;
    r.hpDrop = hp0 - hpNow;
    mobs.Reset();
    c.debris.Reset();
    return r;
  };
  const Swept sweptBare = sweep(false);
  const Swept sweptDressed = sweep(true);
  if (!sweptDressed.wore) {
    detail = "the cuirass did not go on the swept rig";
    return Status::Skip;
  }
  RecordObserved("impactArmorSweptBareFleshLost", (double)sweptBare.fleshLost);
  RecordObserved("impactArmorSweptDressedFleshLost",
                 (double)sweptDressed.fleshLost);
  RecordObserved("impactArmorSweptDressedShellLost",
                 (double)sweptDressed.shellLost);
  RecordObserved("impactArmorSweptCovered", (double)sweptDressed.covered);
  // The bare torso is CUT, or nothing below is a measurement.
  const bool sweptRan = sweptBare.ran && sweptDressed.ran;
  const bool bareCut = sweptBare.fleshLost > 0;
  // The dressed one keeps its flesh: at most a fraction of what the bare one
  // lost AND at most a sliver of the torso, because "a fraction of bare" alone
  // is satisfied by a bare arm that happened to lose a great deal.
  const double sweptFrac =
      BaselineNumber("impactArmorSweptDressedMaxFraction", 0.10);
  const double sweptTorsoMax =
      BaselineNumber("impactArmorSweptDressedMaxTorsoFraction", 0.01);
  const bool sweptSheltered =
      (double)sweptDressed.fleshLost <=
          std::max(1.0, (double)sweptBare.fleshLost * sweptFrac) &&
      (double)sweptDressed.fleshLost <=
          (double)sweptDressed.fleshBefore * sweptTorsoMax;
  // ...and the plate took the blows: chipped, and at least one probe that
  // found flesh was turned back onto it. A sweep that never reached the flesh
  // at all would pass the two lines above while proving nothing about the
  // redirect.
  const bool plateTook = sweptDressed.shellLost > 0 && sweptDressed.covered > 0;

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

  const bool ok = skated && sheltered && beatIn && got && dry && clean &&
                  sweptRan && bareCut && sweptSheltered && plateTook;
  detail = Format(
      "%s/%s in an iron cuirass, x%d each: SWORD took %u/%u shell voxels "
      "(%.1f%%, cap %.0f%%) and %.1f host hp | MACE took %u/%u (%.1f%%) and "
      "%.1f host hp, bleed budget %.2f (as trauma %.2f, as a cut %.2f), rot %u "
      "| swept sword x%d through the torso: bare lost %u/%u flesh (hp -%.1f); "
      "dressed lost %u flesh (hp -%.1f, cap %.0f%% of bare and %.1f%% of the "
      "torso), plate chipped %u/%u, %d probes turned back onto it%s",
      t.defName.c_str(), mobs.Defs()[t.defIndex].limbs[t.torso].name.c_str(),
      kHits, bladeLost, blade.shellBefore, bladeFrac * 100.0f, chipMax * 100.0,
      bladeHost, clubLost, club.shellBefore, clubFrac * 100.0f, clubHost,
      club.hostBleed, asTrauma, asACut, club.rot, kSweptHits,
      sweptBare.fleshLost, sweptBare.fleshBefore, sweptBare.hpDrop,
      sweptDressed.fleshLost, sweptDressed.hpDrop, sweptFrac * 100.0,
      sweptTorsoMax * 100.0, sweptDressed.shellLost, sweptDressed.shellBefore,
      sweptDressed.covered, sweptRan ? "" : " (A SWEEP HIT NOTHING)");
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

  // ---- THE DENT IS A DISSOLUTION NOW (2026-09-19, gore.pulpRotRate) --------
  //
  // Both arms are TICKED after their blows, at the same cranked rate for the
  // same number of ticks (see Dissolve): a gauntlet's dent is a flag on the
  // limb that Mob::BluntPulpTick then spends, so read the instant the blows
  // end it is always zero, which is what this gate reported after 54ba62b. The
  // bare fist is ticked too -- bluntCarve 0 means the flag is never raised, so
  // the SAME ticks at the SAME rate must still take nothing from it, and that
  // is a stronger statement of "a punch removes nothing" than the instant read
  // was. Counts are LIVE counts (LiveVoxels), tombstones excluded.
  const float kPulpRot = (float)BaselineNumber("impactPulpRot", 30.0);
  const int kPulpTicks = (int)BaselineNumber("impactPulpTicks", 60);
  const uint32_t bloodMat = mobs.Defs()[t.defIndex].bleedMat;
  const uint32_t pulpAt = kBruiseBroken;   // split skin (voxload.h)

  struct Arm {
    uint32_t before = 0, afterBlows = 0, after = 0, bruise = 0, pulped = 0;
    bool attached = false, alive = false;
    int ticks = 0;
    // WHAT THE DISSOLVE PHASE DID BESIDES THE PULP (CLAUDE.md rule 6): hp by
    // the cause that charged it, falls billed, and how far the body moved.
    std::string why;
  };
  auto run = [&](int inset, const StrikeProfile& p, uint32_t salt,
                 uint32_t tick0) -> Arm {
    Arm a;
    const uint64_t id = SpawnTarget(c, t, inset);
    if (!id) return a;
    const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
    a.before = LiveVoxels(mobs, id, t.limb);
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
    a.alive = mobs.IsAlive(id);
    if (a.attached) {
      a.afterBlows = LiveVoxels(mobs, id, t.limb);
      // The skin's BRUISE byte (voxload.h PrefabVoxel::bruise) -- neither the
      // material nor, since 2026-09-26, a coat. Level 1 is "marked at all",
      // which is the claim this arm makes: a bare fist bruises and takes
      // nothing.
      a.bruise = bruiseMat ? mobs.LimbBruiseCount(id, t.limb, 1) : 0u;
      // ...and how much of the patch the blows SPLIT (kBruiseBroken),
      // which is what the dissolution below has to work with. Reported, not
      // asserted: the gauntlet's ladder is impact-blunt's claim, made there.
      a.pulped =
          bloodMat ? mobs.LimbBruiseCount(id, t.limb, pulpAt) : 0u;
      float hp0[(int)DamageCause::Count] = {};
      uint32_t falls0 = 0;
      Vec3 o0{};
      if (const Mob* m = mobs.FindMobById(id)) {
        for (int k = 0; k < (int)DamageCause::Count; k++)
          hp0[k] = m->HpLostBy((DamageCause)k);
        falls0 = m->FallsBilled();
        o0 = m->Origin();
      }
      const DissolveResult d =
          Dissolve(c, id, t.limb, kPulpTicks, kPulpRot, tick0);
      if (const Mob* m = mobs.FindMobById(id)) {
        a.why = Format("moved %.1f vox, falls %u, hp by cause:",
                       (m->Origin() - o0).len(), m->FallsBilled() - falls0);
        for (int k = 0; k < (int)DamageCause::Count; k++) {
          const float dh = m->HpLostBy((DamageCause)k) - hp0[k];
          if (dh > 1e-4f)
            a.why += Format(" %s %.2f", DamageCauseName((DamageCause)k), dh);
        }
      }
      a.ticks = d.ticks;
      a.attached = d.attached;
      a.alive = d.alive;
      a.after = a.attached ? LiveVoxels(mobs, id, t.limb) : 0u;
    }
    mobs.Reset();
    c.debris.Reset();
    return a;
  };

  const Arm bare = run(415, fist, 0xF15Du, 9000u);
  const Arm iron = run(425, gaunt->strike, 0x1207u, 9500u);
  std::printf("impact-fist dissolve: BARE %s | GAUNTLET %s\n", bare.why.c_str(),
              iron.why.c_str());
  // THE UNSTRUCK CONTROL (W2-O attribution): the same limb, same ticks, same
  // rate, no blow at all. Whatever it loses is not the fist's.
  {
    const uint64_t cid = SpawnTarget(c, t, 435);
    if (cid) {
      const uint32_t b0 = LiveVoxels(mobs, cid, t.limb);
      const uint32_t art0 = mobs.LimbArtVoxelCount(cid, t.limb);
      const uint32_t tomb0 = mobs.LimbMaterialCount(cid, t.limb, 0u);
      Dissolve(c, cid, t.limb, kPulpTicks, kPulpRot, 9800u);
      const uint32_t b1 = mobs.LimbBody(cid, t.limb) ? LiveVoxels(mobs, cid, t.limb) : 0u;
      std::printf("impact-fist control (no blows): live %u -> %u (art %u -> %u, "
                  "tombstones %u -> %u)\n", b0, b1, art0,
                  mobs.LimbArtVoxelCount(cid, t.limb), tomb0,
                  mobs.LimbMaterialCount(cid, t.limb, 0u));
      mobs.Reset();
      c.debris.Reset();
    }
    // ...and the same at the SHIPPED pulp rate: tombstones that still appear
    // here are not the pulp tick's at all.
    const uint64_t cid2 = SpawnTarget(c, t, 445);
    if (cid2) {
      const uint32_t tomb0 = mobs.LimbMaterialCount(cid2, t.limb, 0u);
      Dissolve(c, cid2, t.limb, kPulpTicks, CurrentTuning().gore.pulpRotRate,
               9900u);
      std::printf("impact-fist control (no blows, shipped pulp rate %.2f): "
                  "tombstones %u -> %u\n", CurrentTuning().gore.pulpRotRate,
                  tomb0, mobs.LimbMaterialCount(cid2, t.limb, 0u));
      mobs.Reset();
      c.debris.Reset();
    }
  }

  const uint32_t bareLost = bare.before > bare.after ? bare.before - bare.after : 0u;
  const uint32_t ironLost = iron.before > iron.after ? iron.before - iron.after : 0u;
  const uint32_t ironAtBlows =
      iron.before > iron.afterBlows ? iron.before - iron.afterBlows : 0u;
  const float ironPerHit =
      iron.before ? (float)ironLost / (float)iron.before / (float)kHits : 0.0f;

  RecordObserved("impactFistBareLost", (double)bareLost);
  RecordObserved("impactFistBareBruise", (double)bare.bruise);
  RecordObserved("impactFistIronLost", (double)ironLost);
  RecordObserved("impactFistIronLostAtBlows", (double)ironAtBlows);
  RecordObserved("impactFistIronPulped", (double)iron.pulped);
  RecordObserved("impactFistIronPerHitFraction", (double)ironPerHit);

  // "Punching never dismembers, it bloodies a spot if you hit it repeatedly."
  // Zero voxels is an ABSOLUTE claim and it is the right one here: bluntCarve
  // 0 means the limb is never flagged for dissolution at all, so anything
  // above zero -- after the same ticks that empty the gauntlet's crater -- is
  // a dispatch bug rather than a tuning question.
  const bool bareTookNothing = bareLost == 0 && bare.attached;
  const bool bareMarked = bruiseMat != 0 && bare.bruise > 0;
  // "...with iron gauntlets it also deletes voxels." Both halves: something
  // came off, and it came off SLOWLY — bounded above by the fraction ONE
  // sword cut takes, which is the wound gates' own observed number and is
  // what "slowly" has to mean if it is to mean anything.
  //
  // ...and none of it in the swing (2026-09-19): the instant read must still
  // be zero, or the dent has gone back to being carved on contact.
  const double slowCap = BaselineNumber("woundChipMaxFraction", 0.35);
  const bool ironBit = ironLost > 0 && iron.attached;
  const bool ironNoInstant = ironAtBlows == 0;
  const bool ironSlow = ironPerHit < (float)slowCap;

  const bool ok = bareTookNothing && bareMarked && ironBit && ironNoInstant &&
                  ironSlow;
  detail = Format(
      "%s/%s x%d each, then %d ticks dissolving at %.0f vox/min: BARE FIST "
      "(blunt %.1f, dent 0) took %u voxels and bruised %u, attached=%d "
      "alive=%d | GAUNTLET (blunt %.1f, dent %.2f) pulped %u, took %u in the "
      "swing (must be 0) and %u of %u after %d ticks, %.3f%% per hit (cap "
      "%.1f%%), attached=%d alive=%d",
      t.defName.c_str(), t.limbName.c_str(), kHits, kPulpTicks, kPulpRot,
      fist.blunt, bareLost, bare.bruise, bare.attached ? 1 : 0,
      bare.alive ? 1 : 0, gaunt->strike.blunt, gaunt->strike.bluntCarve,
      iron.pulped, ironAtBlows, ironLost, iron.before, iron.ticks,
      ironPerHit * 100.0f, slowCap * 100.0, iron.attached ? 1 : 0,
      iron.alive ? 1 : 0);
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
    // On the SKIN at mid-limb, where teeth close (SkinAim): the axis is bone.
    const Vec3 at = SkinAim(mobs, id, t.limb, ax);
    // ---- READ AFTER EVERY BITE, NOT AT THE END -----------------------------
    //
    // Because the limb is SUPPOSED to come off eventually: a bite severs by
    // collapse, which is the one rule separating it from a punch
    // (the Bite row's collapseSevers, game/severpolicy.h). The first version of this arm bit ten times and then
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
  // from trauma: it is an open wound: Mob::BiteHit carves as DamageCause::Bite, whose
  // row bleeds at the full rate (game/severpolicy.h).
  const bool bled = bleed > 0.0f;
  // ARMOUR DEFENDS: not one rotten voxel, on the plate or on the man inside
  // it — and he was still hit, which is what makes the zero a refusal rather
  // than a miss.
  const bool defended = !wore || (shellRot == 0 && hostRot == 0);
  const bool landed = !wore || shellHostHp1 < shellHostHp0;

  // A BITE IS A WOUND, NOT AN AMPUTATION -- and this is the assertion that
  // earned its place. The collapse sever is left ON for a bite (the Bite row of
  // game/severpolicy.h)
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
    const Vec3 at = SkinAim(mobs, id, t.limb, ax);  // the skin, not the bone core
    BiteOnce(mobs, c.world, id, t.limb, at, 4.0f, (uint16_t)rotMat,
             (uint16_t)ichor, 0.85f, 0xB17Eu, spawns);
    r.rot0 = mobs.LimbMaterialCount(id, t.limb, rotMat);
    r.vox0 = LiveVoxels(mobs, id, t.limb);
    if (boneMat) r.bone0 = mobs.LimbStainedMatCount(id, t.limb, boneMat, 1);
    // SEEDED, not left at 0: the per-tick step below diffs against the previous
    // reading, and starting from zero would score the bite's own 65 voxels as
    // the first tick's step and make the burst assertion unfailable.
    r.rot1 = r.rot0;
    r.vox1 = r.vox0;
    r.bone1 = r.bone0;
    // THE REAL TICK (W2-O, test/tickrig.h): the rot spreads through the body's
    // own tick with the world, the step and the contact pass running round it.
    uint32_t tick = 39999;
    const Vec3 fo = mobs.MobOrigin(id);
    support::TickCursor ticker{
        c, tick, IVec3{ifloor(fo.x) >> 4, ifloor(fo.y) >> 4, ifloor(fo.z) >> 4}};
    for (int i = 0; i < kTicks; i++) {
      if (!mobs.LimbBody(id, t.limb)) break;
      ticker();
      // Kept from the last tick the limb was STILL ON, for arm A of
      // `bite-rot`'s reason: reading after a collapse measures a stump.
      const uint32_t was = r.rot1;
      r.rot1 = mobs.LimbMaterialCount(id, t.limb, rotMat);
      // Tombstones excluded: a per-voxel eat is flushed in batches.
      r.vox1 = LiveVoxels(mobs, id, t.limb);
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
// that makes "cut at the joint" work for a sword was gated on a blade cut (then the `inBladeCut_` flag; now the `joint`
// column of game/severpolicy.h, IfInfected for Burn-eaten rot).
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
    std::string trace;   // socket / neck hold every 100 ticks
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
    // A BIGGER BITE AT THE SOCKET (2026-10-01). The per-voxel rot spreads
    // the same way in every direction, so a small seed beside the joint is a
    // race between eating the socket and eating the torso to death; a bite
    // that already opens the shoulder and an eat-heavy rot (R < 1, baseline)
    // puts the work where the claim is.
    tt.gore.biteRadius = saved.gore.biteRadius *
                         (float)BaselineNumber("jointRotBiteScale", 2.0);
    // ...and the blood out of it is not hp here: measured, both arms BLED OUT
    // (tick 176 / 203) one tick before the socket crossed its threshold, a
    // race about the fixture's blood loss, not about the joint.
    tt.gore.bleedHpPerVoxel = 0.0f;
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
    // THE LIMB'S OWN PARENT, on its SURFACE nearest the joint (2026-10-01).
    // It bit `t.torso` -- the ROOT limb -- at the joint point, and the root is
    // not the parent of every limb (zeus's arm hangs from the chest, the root
    // is the hips): the bite landed in the air beside the hips, carved
    // nothing, and the gate reported "the bite landed no infection".
    const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
    int parent = t.torso;
    {
      const MobDef& d = mobs.Defs()[(size_t)t.defIndex];
      const std::string& pn = d.limbs[(size_t)t.limb].parent;
      for (size_t li = 0; li < d.limbs.size(); li++)
        if (d.limbs[li].name == pn) parent = (int)li;
    }
    // ...AT THE SOCKET the joint rule counts round (MobSystem::LimbSocketWorld),
    // on the parent's surface there: nearest the arm's anchor is not always
    // it (measured: a 438-cell bite there never touched the 13-cell socket).
    Vec3 sock = ax.anchor;
    mobs.LimbSocketWorld(id, t.limb, sock);
    BiteOnce(mobs, c.world, id, parent, SurfaceNear(mobs, id, parent, sock),
             5.0f, (uint16_t)rotMat, (uint16_t)ichor, 0.85f, 0xB17Eu, spawns);
    r.rot0 = mobs.LimbMaterialCount(id, parent, rotMat);
    // ...and every reading after this is about the ARM, which nothing has
    // touched. If it leaves, the socket is the only thing that can have taken
    // it.
    r.vox0 = mobs.LimbArtVoxelCount(id, t.limb);
    r.voxLast = r.vox0;
    // THE REAL TICK (W2-O, test/tickrig.h), as bite-rot's.
    uint32_t tick = 39999;
    const Vec3 fo = mobs.MobOrigin(id);
    support::TickCursor ticker{
        c, tick, IVec3{ifloor(fo.x) >> 4, ifloor(fo.y) >> 4, ifloor(fo.z) >> 4}};
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
      // ATTRIBUTION: what holds the arm on (Mob::JointAttached's two counts).
      if (i % 100 == 0) {
        float sock = -1.0f, neck = -1.0f;
        mobs.LimbJointHold(id, t.limb, sock, neck);
        r.trace += Format(" t%d socket %.2f neck %.2f", i, sock, neck);
        if (i % 300 == 0)
          r.trace += " [" + mobs.LimbSocketMaterials(id, t.limb) + "]";
        r.trace += ";";
      }
      ticker();
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
      " at " + std::to_string((int)(frac(b) * 100.0f)) + "% | hold (need >= " +
      Format("%.2f", saved.gore.woundNeckFraction) + "):" + a.trace;
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

// ---------------------------------------------------------------------------
// bite-limbs — A BITE INFECTS WHATEVER IT LANDS ON (2026-09-19)
// ---------------------------------------------------------------------------
//
// Owner report: "zombie bites on the arm most of the time do not create
// infection voxels, just the green stained normal bite ones; the ones on the
// chest and head tend to infect." That is a report about a DISTRIBUTION over
// limbs, and every gate above it bites exactly one limb — the biggest severable
// non-vital one `ChooseTarget` can find, i.e. a thigh. So the whole family of
// bite gates was green while the feature only worked on slabs.
//
// A BARE COUNT BUYS ONE HYPOTHESIS (CLAUDE.md rule 6), and "the arm did not
// infect" had at least four: the teeth carve nothing on a thin limb
// (gore.biteRadius is 0.42 WORLD voxels), the carve collapse-severs it, the
// rewrite finds no flesh-class cell inside the rim, or the contact point is
// outside the limb's own lattice so none of the above ever runs. This gate
// answers all of them at once by biting EVERY base limb and printing a row per
// limb, in two arms that differ only in WHERE the teeth land:
//
//   CORE — the middle of the limb, which is where every existing bite gate
//          aims (a rig anchor plus half the reach). Inside the meat.
//   SKIN — the outermost voxel of the same cross-section, which is where teeth
//          actually arrive: `MeleeSweepDamage` hands `BiteHit` the probe's
//          contact point and a probe stops at the surface it met.
//
// So a limb that infects in CORE and not in SKIN is a CONTACT-POINT defect, and
// one that fails both is a LIMB-SIZE defect. Two different fixes, told apart by
// one run and no A/B.
//
// The claim is the SKIN arm: a bite that lands anywhere on a creature made of
// flesh leaves rot in it. The core arm is reported, not asserted — it is the
// diagnosis, and asserting a control twice is how a gate starts failing for
// something it does not own.
Status GateBiteLimbs(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const uint32_t rotMat = mobs.MaterialIdNamed("rotflesh");
  const uint32_t ichor = mobs.MaterialIdNamed("ichor");
  if (!rotMat || !ichor) {
    detail = "materials.json has no `rotflesh` / `ichor`";
    return Status::Skip;
  }
  // WHICH CREATURE: the most-limbed thing in the library that bleeds and has an
  // anatomy. By SHAPE and not by name, for the reason the cast-hardcoding
  // gotcha gives: a gate that names `human` starts measuring nothing the day
  // the cast changes, and this one is about limbs being SMALL, so the def with
  // the most of them is the one that has small ones.
  int defIndex = -1;
  size_t most = 0;
  for (size_t d = 0; d < mobs.Defs().size(); d++) {
    const MobDef& def = mobs.Defs()[d];
    if (def.bleedMat == 0 || def.tissue.empty()) continue;
    // Counted WITHOUT bloodless limbs: a long-haired character's `hair` and
    // `mane` are extra limbs that no bite infects, and counting them would
    // hand the gate a body whose "most limbs" is two locks of hair.
    size_t n = 0;
    for (const MobLimbDef& ld : def.limbs) n += ld.bloodless ? 0 : 1;
    if (n > most) {
      most = n;
      defIndex = (int)d;
    }
  }
  if (defIndex < 0) {
    detail = "no loaded mob def both bleeds and has an anatomy";
    return Status::Skip;
  }

  // One spawn, posed, at its own ground column. Every bite below gets a FRESH
  // one: a bite carves, and a limb already torn is a different fixture.
  auto spawn = [&](int inset) -> uint64_t {
    mobs.Reset();
    c.debris.Reset();
    const uint64_t id = mobs.Spawn(defIndex, FixtureSite(c.world, inset));
    if (!id) return 0;
    // DIRECT PHASE CALLS ON PURPOSE (W2-O): fixture posing (SpawnTarget's).
    for (int i = 0; i < 8; i++) {
      std::vector<BrushOp> ops;
      std::vector<ParticleSpawn> spawns;
      std::vector<CellOp> cellOps;
      mobs.PreTick(6000u + (uint32_t)i, c.world, ops, cellOps, spawns);
      c.phys.Step(kTickDt);
      mobs.PostStep();
    }
    mobs.ClearSeverEvents();
    mobs.ClearSeverStats();
    return id;
  };

  // The limb roster, read off a live rig rather than off the def: only the rig
  // knows which limbs physics actually gave a body.
  struct Row {
    int limb = -1;
    std::string name, tag;
    uint32_t atSpawn = 0;
    uint32_t core = 0, skin = 0;      // rot voxels left by the bite
    uint32_t coreLost = 0, skinLost = 0;
    int skinHits = 0;                 // of kSeeds surface bites, how many infected
    bool coreOn = true, skinOn = true;
    // WHICH LIMBS THE CLAIM IS ABOUT. The owner's report is about arms, hands
    // and legs, and those are the tags asserted. The rest are REPORTED and not
    // asserted, because each has a reason of its own that this gate does not own:
    //
    //   spine — a torso's tissue is a THREE-CELL SHELL over bone (human.json
    //           anatomy: skin 1, flesh 1, muscle 1, then bone open-ended), so a
    //           hole punched in a chest exposes mostly rib, and the rewrite
    //           refuses bone by design. Measured 0 rot from 58 carved voxels.
    //           Asserting it would be asserting that a chest is made of meat.
    //   head  — a bite at the MIDDLE of a head reaches `brain`, which is vital:
    //           the creature dies and the whole rig drops, so the reading is of
    //           a corpse. The surface bite it is actually asked for works (25).
    //   foot  — comes OFF to a single 4 hp bite, both at the surface and at the
    //           core, and has done since before this gate existed. That is worth
    //           a look and it is not this gate's claim; it is why `foot` is not
    //           in the bite `target` table (assets/mobs/attack_styles.json).
    bool asserted = false;
  };
  std::vector<Row> rows;
  {
    const uint64_t id = spawn(445);
    if (!id) {
      detail = "the fixture would not spawn";
      return Status::Fail;
    }
    const Mob* m = mobs.FindMobById(id);
    if (m == nullptr) {
      detail = "the fixture spawned and could not be found";
      return Status::Fail;
    }
    for (int li = 0; li < m->AppendedBase(); li++) {
      if (!mobs.LimbBody(id, li)) continue;
      if (m->LimbDefAt(li).bloodless) continue;  // hair: nothing to infect
      Row r;
      r.limb = li;
      r.name = m->LimbDefAt(li).name;
      r.tag = m->LimbDefAt(li).tag;
      r.atSpawn = mobs.LimbVoxelsAtSpawn(id, li);
      r.asserted = r.tag == "arm" || r.tag == "hand" || r.tag == "leg";
      rows.push_back(r);
    }
    mobs.Reset();
    c.debris.Reset();
  }
  if (rows.empty()) {
    detail = "the fixture has no limbs with bodies";
    return Status::Fail;
  }

  // The rot must not GROW inside the measurement: this gate is about the bite,
  // and `bite-infect` owns the clock. Both rates to 0 and no ticks are run.
  const Tuning saved = CurrentTuning();
  {
    Tuning tt = saved;
    tt.gore.infectSpreadRate = 0.0f;
    tt.gore.infectRotRate = 0.0f;
    SetCurrentTuning(tt);
  }

  // WHERE THE TEETH LAND. `core` is the middle of the limb; `skin` is that same
  // cross-section pushed out to the furthest voxel across the axis, which is
  // the surface a probe would have stopped on. Measured off surviving voxels
  // (MeasureLimb's own construction) so nothing here reads an authored box.
  auto aimPoints = [&](uint64_t id, int li, Vec3& core, Vec3& skin) -> bool {
    const LimbAxis ax = MeasureLimb(mobs, id, li);
    if (!ax.valid) return false;
    core = ax.anchor + ax.along * (ax.reach * 0.5f);
    Vec3 bestDir{};
    float bestLen = 0.0f;
    for (uint32_t k = 0; k < 64; k++) {
      const Vec3 p = mobs.LimbVoxelPos(id, li, k * 6151u);
      const Vec3 rel = p - core;
      const Vec3 perp = rel - ax.along * rel.dot(ax.along);
      const float len = perp.len();
      if (len > bestLen) {
        bestLen = len;
        bestDir = perp;
      }
    }
    skin = bestLen > 1e-3f ? core + bestDir.normalized() * bestLen : core;
    return true;
  };

  // ONE BITE IS A DRAW, AND A GATE ON ONE DRAW IS A KNIFE EDGE. The rewrite is
  // a hash-thresholded fraction of the flesh a hole exposed, so "this limb took
  // 1 rot voxel" and "this limb took none" are the same claim measured twice.
  // The surface arm is therefore run at kSeeds different seeds, on a fresh
  // creature each time, and what is asserted is the RATE: a bite on a limb
  // infects it, not one time in three.
  const int kSeeds = (int)BaselineNumber("biteLimbsSeeds", 3);
  const int kNeed = (int)BaselineNumber("biteLimbsNeed", 2);
  const float kHp = (float)BaselineNumber("biteLimbsHp", 4.0);
  int inset = 445;
  for (Row& r : rows) {
    // arm -1 = the core control (once); 0..kSeeds-1 = the surface claim.
    for (int arm = -1; arm < kSeeds; arm++) {
      const uint64_t id = spawn(inset);
      inset = inset == 445 ? 455 : 445;   // alternate two columns, never share
      if (!id) continue;
      Vec3 core{}, skin{};
      if (!aimPoints(id, r.limb, core, skin)) continue;
      const uint32_t before = mobs.LimbArtVoxelCount(id, r.limb);
      std::vector<ParticleSpawn> spawns;
      BiteOnce(mobs, c.world, id, r.limb, arm < 0 ? core : skin, kHp,
               (uint16_t)rotMat, (uint16_t)ichor, 0.85f,
               0xB17Eu + (uint32_t)r.limb * 131u + (uint32_t)(arm + 1) * 7919u,
               spawns);
      const bool on = mobs.LimbBody(id, r.limb) != 0;
      const uint32_t rot = on ? mobs.LimbMaterialCount(id, r.limb, rotMat) : 0;
      const uint32_t after = on ? mobs.LimbArtVoxelCount(id, r.limb) : 0;
      const uint32_t lost = before > after ? before - after : 0;
      if (arm < 0) {
        r.core = rot;
        r.coreLost = lost;
        r.coreOn = on;
      } else {
        // The FIRST surface seed is the one reported as the row's reading, so
        // the readout stays one number per limb; the rest only vote.
        if (arm == 0) {
          r.skin = rot;
          r.skinLost = lost;
          r.skinOn = on;
        }
        if (rot > 0) r.skinHits++;
      }
      mobs.Reset();
      c.debris.Reset();
    }
  }
  SetCurrentTuning(saved);

  // ---- THE READOUT: one row per limb, both arms ----------------------------
  std::string s = mobs.Defs()[defIndex].name + ":";
  std::string weak;                   // the asserted limbs that failed the rate
  uint32_t asserted = 0, skinDead = 0, coreDead = 0, sever = 0;
  for (const Row& r : rows) {
    s += " " + r.name + "[" + r.tag + "," + std::to_string(r.atSpawn) + "v skin " +
         std::to_string(r.skin) + "rot/" + std::to_string(r.skinLost) + "lost " +
         std::to_string(r.skinHits) + "/" + std::to_string(kSeeds) +
         " core " + std::to_string(r.core) + "rot/" +
         std::to_string(r.coreLost) + "lost" +
         (r.skinOn && r.coreOn ? "" : " SEVERED") + (r.asserted ? "" : " (fyi)") +
         "]";
    if (r.core == 0) coreDead++;
    if (!r.skinOn || !r.coreOn) sever++;
    if (!r.asserted) continue;
    asserted++;
    if (r.skinHits < kNeed) {
      skinDead++;
      weak += " " + r.name + "(" + std::to_string(r.skinHits) + "/" +
              std::to_string(kSeeds) + ")";
    }
  }
  RecordObserved("biteLimbsLimbs", (double)rows.size());
  RecordObserved("biteLimbsAsserted", (double)asserted);
  RecordObserved("biteLimbsSkinNoRot", (double)skinDead);
  RecordObserved("biteLimbsCoreNoRot", (double)coreDead);
  RecordObserved("biteLimbsSevered", (double)sever);
  detail = std::to_string(asserted) + " arm/hand/leg limbs of " +
           std::to_string(rows.size()) + ", " + std::to_string(skinDead) +
           " infected by fewer than " + std::to_string(kNeed) + " of " +
           std::to_string(kSeeds) + " surface bites (" + std::to_string(coreDead) +
           " limbs took nothing from a CORE bite, " + std::to_string(sever) +
           " severed); " + s;
  if (asserted == 0) {
    detail = "no limb on the fixture is tagged arm/hand/leg: " + s;
    return Status::Skip;
  }
  if (skinDead == 0) return Status::Pass;
  detail = "a bite on the surface of" + weak +
           " left NO infection — this is the owner's report (arms tear and do "
           "not rot): " + detail;
  return Status::Fail;
}


// ---- WOOD BLEEDS A FIFTH (2026-09-29, materials.json `bleed`) ---------------
//
// How much a wound bleeds is a property of the MATTER the blow opens, not of
// the creature: flesh 1, wood / bark / leaves / crystal 0.2. Three arms, the
// same blade cuts at the same place on a forearm:
//   A  a human's forearm, as it is (the control);
//   B  the SAME forearm with every voxel rewritten to `wood` first -- "a human
//      turned their arm into wood" -- which must bleed about a fifth of A;
//   C  a sylvan's forearm (a wood spirit: bark over wood, bleeding sap), which
//      must bleed far less than A too.
// The ratio ceiling lives in tests/baseline.json (woodBleedMaxRatio).
float WoodBleedArm(Ctx& c, int defIndex, int limb, bool toWood, int inset,
                   std::string& why) {
  MobSystem& mobs = c.mobs;
  mobs.Reset();
  c.debris.Reset();
  const uint64_t id = mobs.Spawn(defIndex, FixtureSite(c.world, inset));
  if (!id) { why = "spawn refused"; return -1.0f; }
  mobs.SetMobBehavior(id, "dummy");
  // DIRECT PHASE CALLS ON PURPOSE (W2-O): fixture POSING, as SpawnTarget.
  for (int i = 0; i < 8; i++) {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    mobs.PreTick(3000u + (uint32_t)i, c.world, ops, cellOps, spawns);
    c.phys.Step(kTickDt);
    mobs.PostStep();
  }
  if (toWood) {
    uint32_t wood = 0;
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == "wood") wood = (uint32_t)i;
    for (uint32_t m = 1; m < (uint32_t)c.mats.size(); m++) {
      if (m == wood) continue;
      if (mobs.LimbMaterialCount(id, limb, m))
        mobs.RewriteLimbMaterial(id, limb, m, wood, 1u << 30);
    }
    if (mobs.LimbMaterialCount(id, limb, wood) == 0) {
      why = "the forearm did not turn to wood";
      return -1.0f;
    }
  }
  const LimbAxis ax = MeasureLimb(mobs, id, limb);
  std::vector<ParticleSpawn> spawns;
  const int kCuts = (int)BaselineNumber("woodBleedCuts", 2);
  for (int k = 0; k < kCuts; k++)
    CutOnce(mobs, c.world, id, limb, ax, ax.reach * 0.5f, 6.0f, 0.6f,
            0x0B1EEDu + (uint32_t)k * 2654435761u, spawns);
  const float b = mobs.LimbBody(id, limb) ? mobs.LimbBleedBudget(id, limb) : -1.0f;
  if (b < 0) why = "the forearm came off";
  mobs.Reset();
  c.debris.Reset();
  return b;
}

int LimbNamed(const MobDef& d, const char* name) {
  for (size_t i = 0; i < d.limbs.size(); i++)
    if (d.limbs[i].name == name) return (int)i;
  return -1;
}

// ARM D: THE AMPUTATION. Sever the forearm outright and count everything of
// the creature's bleed material that leaves over the gout's window: droplets,
// thrown voxels and the drip's paint ops. The gout, the thrown voxels and the
// stump's standing top-up are scaled by the matter at the cut (MobLimb::
// woundScale), so a wooden stump must pay a fraction of a flesh one.
int SeverBleedCount(Ctx& c, int defIndex, int limb, bool toWood, int inset,
                    std::string& why, std::string* parts = nullptr) {
  MobSystem& mobs = c.mobs;
  mobs.Reset();
  c.debris.Reset();
  const uint64_t id = mobs.Spawn(defIndex, FixtureSite(c.world, inset));
  if (!id) { why = "spawn refused"; return -1; }
  mobs.SetMobBehavior(id, "dummy");
  // EVERY LIQUID it leaks, not only the def's `bleed.material`: since
  // materials.h bleedFluid a wooden stump on a man leaks SYRUP, and counting
  // his blood alone would score it 0 and pass without measuring anything.
  auto isFluid = [&](uint32_t m) {
    return m != 0 && m < c.mats.size() && c.mats[m].gpu.klass == CLASS_LIQUID;
  };
  // DIRECT PHASE CALLS ON PURPOSE (W2-O): the subject is the stump's bleed,
  // which Mob::BleedTick (inside PreTick) pays; nothing else is wanted.
  int count = 0;
  // WHICH EMITTER paid it: micro droplets (gout + spray), whole voxels (the
  // sever's throw), paint ops (the stump's drip). A bare total cannot say
  // why one stump pays a twentieth of another at the same woundScale.
  int nMicro = 0, nWhole = 0, nOps = 0, nOther = 0;
  auto tick = [&](uint32_t t, bool measure) {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    mobs.PreTick(t, c.world, ops, cellOps, spawns);
    c.phys.Step(kTickDt);
    mobs.PostStep();
    if (!measure) return;
    for (const ParticleSpawn& p : spawns)
      if (isFluid(p.payload & 0xFFFu)) {
        count++;
        ((p.flags & kPFlagMicro) ? nMicro : nWhole)++;
      } else {
        nOther++;
      }
    for (const BrushOp& o : ops)
      if (isFluid(o.material)) { count += 1 + 6 * o.radius; nOps++; }
  };
  for (int i = 0; i < 8; i++) tick(3000u + (uint32_t)i, false);
  if (toWood) {
    uint32_t wood = 0;
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == "wood") wood = (uint32_t)i;
    // The forearm AND its parent: the stump is the upper arm's end.
    const MobDef& dd = mobs.Defs()[defIndex];
    int parent = -1;
    for (size_t i = 0; i < dd.limbs.size(); i++)
      if (dd.limbs[i].name == dd.limbs[limb].parent) parent = (int)i;
    for (int l : {limb, parent})
      if (l >= 0)
        for (uint32_t m = 1; m < (uint32_t)c.mats.size(); m++)
          if (m != wood && mobs.LimbMaterialCount(id, l, m))
            mobs.RewriteLimbMaterial(id, l, m, wood, 1u << 30);
  }
  int parentLimb = -1;
  {
    const MobDef& dd = mobs.Defs()[defIndex];
    for (size_t i = 0; i < dd.limbs.size(); i++)
      if (dd.limbs[i].name == dd.limbs[limb].parent) parentLimb = (int)i;
  }
  mobs.Sever(id, limb);
  const float budget0 = parentLimb >= 0 ? mobs.LimbBleedBudget(id, parentLimb) : -1.0f;
  const bool open0 = parentLimb >= 0 && mobs.LimbWoundOpen(id, parentLimb);
  const int kTicks = (int)BaselineNumber("woodBleedSeverTicks", 90);
  int closedAt = -1;
  for (int i = 0; i < kTicks; i++) {
    tick(3100u + (uint32_t)i, true);
    if (closedAt < 0 && parentLimb >= 0 && !mobs.LimbWoundOpen(id, parentLimb))
      closedAt = i;
  }
  if (parts)
    *parts = Format("[micro %d, whole %d, ops %d, other spawns %d; stump "
                    "budget %.1f open %d, closed at tick %d]",
                    nMicro, nWhole, nOps, nOther, budget0, open0 ? 1 : 0,
                    closedAt);
  mobs.Reset();
  c.debris.Reset();
  return count;
}

Status GateWoodBleed(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const int human = mobs.FindDef("human");
  if (human < 0) { detail = "no `human` def"; return Status::Fail; }
  const int limb = LimbNamed(mobs.Defs()[human], "armL.R");
  if (limb < 0) { detail = "the human has no armL.R"; return Status::Fail; }
  std::string why;
  const float flesh = WoodBleedArm(c, human, limb, false, 445, why);
  const float wooden = WoodBleedArm(c, human, limb, true, 445, why);
  if (flesh <= 0.0f || wooden < 0.0f) {
    detail = "arm did not measure: " + why;
    return Status::Fail;
  }
  RecordObserved("woodBleedFlesh", flesh);
  RecordObserved("woodBleedWood", wooden);
  const double maxRatio = BaselineNumber("woodBleedMaxRatio", 0.3);
  const float ratio = wooden / flesh;
  detail = Format("human forearm bled %.2f; turned to wood %.2f (x%.2f)",
                  flesh, wooden, ratio);
  bool ok = ratio <= maxRatio && ratio >= 0.05f;
  // ...and a wood spirit, when one is loaded (assets/mobs/sylvan/).
  const int deku = mobs.FindDef("deku");
  if (deku >= 0) {
    const int dl = LimbNamed(mobs.Defs()[deku], "armL.R");
    const float sap = dl >= 0 ? WoodBleedArm(c, deku, dl, false, 445, why) : -1.0f;
    RecordObserved("woodBleedDeku", sap);
    detail += Format("; deku forearm %.2f (x%.2f)", sap, sap / flesh);
    ok = ok && sap >= 0.0f && sap / flesh <= maxRatio;
  }
  // ---- the amputation: gout, thrown voxels and the stump's drip ----
  std::string pFlesh, pWood, pDeku;
  const int sevFlesh = SeverBleedCount(c, human, limb, false, 445, why, &pFlesh);
  const int sevWood = SeverBleedCount(c, human, limb, true, 445, why, &pWood);
  RecordObserved("woodBleedSeverFlesh", (double)sevFlesh);
  RecordObserved("woodBleedSeverWood", (double)sevWood);
  detail += Format(" | severed forearm: flesh %d %s, wood %d %s", sevFlesh,
                   pFlesh.c_str(), sevWood, pWood.c_str());
  ok = ok && sevFlesh > 0 && sevWood >= 0 &&
       (float)sevWood / (float)sevFlesh <= maxRatio;
  if (deku >= 0) {
    const int dl = LimbNamed(mobs.Defs()[deku], "armL.R");
    const int sevDeku = dl >= 0 ? SeverBleedCount(c, deku, dl, false, 445, why, &pDeku) : -1;
    RecordObserved("woodBleedSeverDeku", (double)sevDeku);
    detail += Format(", deku %d %s", sevDeku, pDeku.c_str());
    ok = ok && sevDeku >= 0 && (float)sevDeku / (float)sevFlesh <= maxRatio;
  }
  return ok ? Status::Pass : Status::Fail;
}

// ---- A WOUND LEAKS WHAT IT OPENED (2026-09-29, materials.h bleedFluid) ------
//
// The FLUID is the matter's, like the amount (`wood-bleed` above): struck
// voxel -> the limb's majority -> the creature's `bleed.material`. So a human
// whose forearm has become wood leaks syrup from it, a sylvan that grew a
// flesh forearm bleeds blood from it, and when either is cut OFF each end of
// the cut leaks its own: the stump what the upper arm is made of, the piece
// what the forearm is. Six arms on the same blade and the same forearm:
//   cut   human as is -> blood      human turned to wood -> syrup
//         deku as is  -> syrup      deku turned to flesh -> blood
//   sever human, wooden forearm -> stump blood, piece syrup
//         deku, flesh forearm   -> stump syrup, piece blood
// Every cut arm also counts what the body EMITTED for 60 ticks after the cut
// (droplets + paint ops), which must be all the expected fluid and none of
// the other: the wound state and the emitters agree, not just the state.
struct FluidProbe {
  uint32_t wound = 0;      // the cut limb's wound fluid (or the stump's)
  uint32_t piece = 0;      // sever: the severed piece's debris bleed material
  int want = 0, other = 0; // emitted voxels of the expected / any other liquid
  bool ok = false;
};

FluidProbe ProbeBleedFluid(Ctx& c, int defIndex, int limb, uint32_t rewriteTo,
                           bool sever, uint32_t expect, std::string& why) {
  FluidProbe r;
  MobSystem& mobs = c.mobs;
  mobs.Reset();
  c.debris.Reset();
  const uint64_t id = mobs.Spawn(defIndex, FixtureSite(c.world, 445));
  if (!id) { why = "spawn refused"; return r; }
  mobs.SetMobBehavior(id, "dummy");
  auto isFluid = [&](uint32_t m) {
    return m != 0 && m < c.mats.size() && c.mats[m].gpu.klass == CLASS_LIQUID;
  };
  // DIRECT PHASE CALLS ON PURPOSE (W2-O): fixture posing, then the body's own
  // bleed (Mob::BleedTick inside PreTick) and nothing else.
  auto tick = [&](uint32_t t, bool measure) {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    mobs.PreTick(t, c.world, ops, cellOps, spawns);
    c.phys.Step(kTickDt);
    mobs.PostStep();
    if (!measure) return;
    for (const ParticleSpawn& p : spawns) {
      const uint32_t m = p.payload & 0xFFFu;
      if (isFluid(m)) (m == expect ? r.want : r.other)++;
    }
    for (const BrushOp& o : ops)
      if (isFluid(o.material)) (o.material == expect ? r.want : r.other) += 1;
  };
  for (int i = 0; i < 8; i++) tick(3000u + (uint32_t)i, false);
  if (rewriteTo) {
    for (uint32_t m = 1; m < (uint32_t)c.mats.size(); m++)
      if (m != rewriteTo && mobs.LimbMaterialCount(id, limb, m))
        mobs.RewriteLimbMaterial(id, limb, m, rewriteTo, 1u << 30);
    if (mobs.LimbMaterialCount(id, limb, rewriteTo) == 0) {
      why = "the forearm was not rewritten";
      return r;
    }
  }
  if (sever) {
    const MobDef& dd = mobs.Defs()[defIndex];
    int parent = -1;
    for (size_t i = 0; i < dd.limbs.size(); i++)
      if (dd.limbs[i].name == dd.limbs[limb].parent) parent = (int)i;
    const uint64_t pieceBody = mobs.LimbBody(id, limb);
    mobs.Sever(id, limb);
    r.wound = parent >= 0 ? mobs.LimbWoundFluid(id, parent) : 0u;
    r.piece = c.debris.BodyBleedMat(pieceBody);
  } else {
    const LimbAxis ax = MeasureLimb(mobs, id, limb);
    std::vector<ParticleSpawn> spawns;
    CutOnce(mobs, c.world, id, limb, ax, ax.reach * 0.5f, 6.0f, 0.6f,
            0x0B1EEDu, spawns);
    if (!mobs.LimbBody(id, limb)) { why = "the forearm came off"; return r; }
    r.wound = mobs.LimbWoundFluid(id, limb);
  }
  for (int i = 0; i < 60; i++) tick(3100u + (uint32_t)i, true);
  mobs.Reset();
  c.debris.Reset();
  r.ok = true;
  return r;
}

Status GateBleedFluid(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  auto mat = [&](const char* n) -> uint32_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == n) return (uint32_t)i;
    return 0;
  };
  const uint32_t blood = mat("blood"), syrup = mat("syrup"), wood = mat("wood"),
                 flesh = mat("flesh");
  if (!blood || !syrup || !wood || !flesh) {
    detail = "blood / syrup / wood / flesh missing from materials.json";
    return Status::Fail;
  }
  // The data the whole rule rests on, checked by name before any body: flesh
  // DERIVES blood from its rubble, wood AUTHORS syrup, bone has no opinion.
  const uint32_t bone = mat("bone");
  const bool data = c.mats[flesh].bleedFluid == blood &&
                    c.mats[wood].bleedFluid == syrup &&
                    (!bone || c.mats[bone].bleedFluid == 0);
  const int human = mobs.FindDef("human");
  const int deku = mobs.FindDef("deku");
  if (human < 0) { detail = "no `human` def"; return Status::Fail; }
  auto name = [&](uint32_t m) {
    return m && m < c.mats.size() ? c.mats[m].name : std::string("none");
  };
  std::string why;
  bool ok = data;
  detail = data ? "" : "[DATA: flesh/wood/bone bleedFluid wrong] ";
  auto arm = [&](const char* label, int def, uint32_t rewrite, bool sever,
                 uint32_t expectWound, uint32_t expectPiece) {
    const int limb = LimbNamed(mobs.Defs()[def], "armL.R");
    if (limb < 0) { detail += Format("%s: no armL.R; ", label); ok = false; return; }
    const FluidProbe r = ProbeBleedFluid(c, def, limb, rewrite, sever,
                                         expectWound, why);
    const bool good = r.ok && r.wound == expectWound &&
                      (!sever || r.piece == expectPiece) && r.want > 0 &&
                      r.other == 0;
    detail += Format("%s: wound %s", label, name(r.wound).c_str());
    if (sever) detail += Format(", piece %s", name(r.piece).c_str());
    detail += Format(", emitted %d/%d stray%s; ", r.want, r.other,
                     good ? "" : r.ok ? " [WRONG]" : (" [" + why + "]").c_str());
    ok = ok && good;
  };
  arm("human", human, 0, false, blood, 0);
  arm("human wood-arm", human, wood, false, syrup, 0);
  arm("human wood-arm severed", human, wood, true, blood, syrup);
  if (deku >= 0) {
    arm("deku", deku, 0, false, syrup, 0);
    arm("deku flesh-arm", deku, flesh, false, blood, 0);
    arm("deku flesh-arm severed", deku, flesh, true, syrup, blood);
  } else {
    detail += "(no deku def: sylvan arms skipped)";
  }
  return ok ? Status::Pass : Status::Fail;
}

// ---- EVERY DEF KNOWS ITS RACE (MobDef::race, the F1 spawn list's filter) ----
// The shipped human declares "human"; a generated character inherits it or
// says its own (a sylvan's sidecar `race`, or its genome's body.race).
Status GateMobRace(Ctx& c, std::string& detail) {
  const MobSystem& mobs = c.mobs;
  auto raceOf = [&](const char* n) -> std::string {
    const int d = mobs.FindDef(n);
    return d < 0 ? std::string("<no def>") : mobs.Defs()[d].race;
  };
  int humans = 0, sylvans = 0, other = 0;
  for (const MobDef& d : mobs.Defs())
    (d.race == "human" ? humans : d.race == "sylvan" ? sylvans : other)++;
  detail = Format("human '%s', newcomer '%s', deku '%s', flew '%s' | %d human, %d sylvan, "
                  "%d other defs", raceOf("human").c_str(),
                  raceOf("newcomer").c_str(), raceOf("deku").c_str(),
                  raceOf("flew").c_str(), humans,
                  sylvans, other);
  const bool ok = raceOf("human") == "human" &&
                  (mobs.FindDef("newcomer") < 0 || raceOf("newcomer") == "human") &&
                  (mobs.FindDef("deku") < 0 || raceOf("deku") == "sylvan") &&
                  // a sylvan saved BEFORE the `race` key existed: its genome
                  // must win over the "human" it inherits from human.json
                  (mobs.FindDef("flew") < 0 || raceOf("flew") == "sylvan");
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// venom-wound -- an infection that is a MATERIAL, seeded by a COAT, that burns
// itself out (docs/PLAN_weapon_coats.md package B)
// ---------------------------------------------------------------------------
//
// Two humans, the same four limbs (the first segment of every arm and leg:
// tag "arm"/"leg" whose parent is not), the same soak of `venom` at full coat.
// Human A has a small hole carved in the outside of each of those limbs first;
// human B is whole. Then the real tick runs until A has no `envenomed` left.
//
//   1. SEEDED: the coat put envenomed into A's open wounds (CoatInfectTick),
//      and spread made more of it -- an infection, not a stain.
//   2. BURNT OUT: A's envenomed count returns to 0 inside the window, and it
//      takes about as long as the arithmetic says (DESIGN.md "Infection is a
//      material": a dose of D cells lasts D / (e - s)).
//   3. 3x THE DOSE: everything seeded or spread is eaten, and eaten / seeded
//      sits in baseline.json's band round 3 -- e = 1.5 s means 2 D spread and
//      3 D eaten, whatever the rate. Summed over four limbs because a single
//      small dose is a short random walk and its ratio is noisy.
//   4. IT HURTS: A booked hp to DamageCause::Infection (the venom's own `hp`
//      plus the volume charge), and its limbs' hp fell.
//   5. BONE STOPS IT: A's bone count on the four limbs did not move.
//   6. SKIN STOPS IT: B -- the same coat on whole skin -- seeded nothing, grew
//      nothing and booked nothing.
//
// SPEEDED UP AS DATA, the way bite-infect cranks its tuning rows: the venom's
// rates live in materials.json, so the gate hands MobSystem a COPY of the
// table with `envenomed`'s spread and eat both scaled by venomWoundTimeScale
// and puts the real one back on the way out. Both are scaled, so the ratio --
// the claim -- is untouched, and the window is checked against the same
// arithmetic at the scaled rate. The hole is made with DamageCause::SpawnRot
// (a hole the body "arrived with": no drip, no bleed hp), so the only hp moving
// on A that is booked to Infection is the venom's.
Status GateVenomWound(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const int human = mobs.FindDef("human");
  const uint32_t venom = mobs.MaterialIdNamed("venom");
  const uint32_t envenomed = mobs.MaterialIdNamed("envenomed");
  const uint32_t boneMat = mobs.MaterialIdNamed("bone");
  if (human < 0 || !venom || !envenomed) {
    detail = "no `human` def or no `venom` / `envenomed` material";
    return Status::Skip;
  }
  int envIdx = -1;
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == "envenomed") envIdx = (int)i;
  if (envIdx < 0 || !c.mats[envIdx].infect || c.mats[envIdx].infectSpread < 0.0f) {
    detail = "`envenomed` has no infect block with its own spread / eat";
    return Status::Fail;
  }
  const MaterialDef& envDef = c.mats[envIdx];

  const float kScale = (float)BaselineNumber("venomWoundTimeScale", 8.0);
  const int kMaxTicks = (int)BaselineNumber("venomWoundMaxTicks", 3000);
  const float kRadius = (float)BaselineNumber("venomWoundRadius", 0.25);
  const double kRatioLo = BaselineNumber("venomWoundEatRatioLo", 1.8);
  const double kRatioHi = BaselineNumber("venomWoundEatRatioHi", 4.5);
  const double kWindowLo = BaselineNumber("venomWoundWindowLo", 0.2);
  const double kWindowHi = BaselineNumber("venomWoundWindowHi", 3.0);

  // The first segment of each arm and leg on the human. COPIED out of the def
  // (names, scale): a spawn may compose and append defs, and a reference into
  // mobs.Defs() is not something to hold across one.
  std::vector<int> limbs;
  std::vector<std::string> limbName;
  uint32_t defSkinScale = 1;
  {
    const MobDef& def = mobs.Defs()[human];
    defSkinScale = def.skinScale ? def.skinScale : 1u;
    for (size_t li = 0; li < def.limbs.size(); li++) {
      const MobLimbDef& ld = def.limbs[li];
      limbName.push_back(ld.name);
      // EVERY arm and leg segment (eight on a human): the eaten / seeded
      // ratio is a sum of branching processes whose spread is ~0.6 of its
      // mean at a 50-cell dose (offspring variance R / (1 - R)^3 = 18 a seed
      // at R = 2/3), so the sample has to be large for a band to mean much.
      if (ld.tag != "arm" && ld.tag != "leg") continue;
      limbs.push_back((int)li);
    }
  }
  if (limbs.empty()) {
    detail = "the human has no limb tagged arm / leg";
    return Status::Fail;
  }

  // THE FAST TABLE (see the note above): restored on every exit below.
  std::vector<MaterialDef> fast = c.mats;
  fast[envIdx].infectSpread = envDef.infectSpread * kScale;
  fast[envIdx].infectEat = envDef.infectEat * kScale;
  mobs.Reset();
  c.debris.Reset();
  mobs.OnMaterialsReloaded(fast, c.reactions);
  // ...and the ROT, for the rot + venom arm: its rates are the gore knobs, so
  // it is cranked through gore.infectMobMult (venomWoundRotCrank); at the
  // shipped per-voxel rates a 20-cell seed acts about once in this window.
  const Tuning savedTune = CurrentTuning();
  {
    Tuning tt = savedTune;
    tt.gore.infectMobMult =
        savedTune.gore.infectMobMult *
        (float)BaselineNumber("venomWoundRotCrank", 100.0);
    SetCurrentTuning(tt);
  }
  auto restore = [&]() {
    mobs.Reset();
    c.debris.Reset();
    mobs.OnMaterialsReloaded(c.mats, c.reactions);
    SetCurrentTuning(savedTune);
  };

  const uint64_t a = mobs.Spawn(human, FixtureSite(c.world, 415));
  const uint64_t b = mobs.Spawn(human, FixtureSite(c.world, 425));
  // THE CONTROL (orchestrator, 2026-10-01): the same pits as A, no venom, the
  // same ticks. Whatever bone it loses is the wound model's, not the venom's.
  const uint64_t ctl = mobs.Spawn(human, FixtureSite(c.world, 435));
  // ROT AND VENOM ON ONE LIMB (owner, 2026-10-01): a zombie's bite and the
  // venom soak on the same upper arm; both infections must run side by side
  // on the per-voxel rule, with nothing on the limb saying which is there.
  const uint64_t both = mobs.Spawn(human, FixtureSite(c.world, 445));
  if (!a || !b || !ctl || !both) {
    restore();
    detail = "could not spawn three humans";
    return Status::Fail;
  }
  mobs.SetMobBehavior(a, "dummy");
  mobs.SetMobBehavior(b, "dummy");
  mobs.SetMobBehavior(ctl, "dummy");
  mobs.SetMobBehavior(both, "dummy");
  // DIRECT PHASE CALLS ON PURPOSE (W2-O): fixture POSING, not the subject --
  // SpawnTarget's eight settling steps, for two bodies at once.
  for (int i = 0; i < 8; i++) {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    mobs.PreTick(3000u + (uint32_t)i, c.world, ops, cellOps, spawns);
    c.phys.Step(kTickDt);
    mobs.PostStep();
  }

  // THE HOLES, on A only: a small pit on the OUTSIDE of each limb, half way
  // down it -- the probe voxel farthest from the limb's axis in its middle
  // third, i.e. a surface cell -- so the wall exposes skin, flesh and muscle
  // the way a shallow cut does.
  std::vector<ParticleSpawn> spawns;
  auto carvePits = [&](uint64_t id) {
    uint32_t n = 0;
    for (int li : limbs) {
      const LimbAxis ax = MeasureLimb(mobs, id, li);
      if (!ax.valid) continue;
      Vec3 best = ax.anchor + ax.along * (ax.reach * 0.5f);
      float bestR = -1.0f;
      for (uint32_t k = 0; k < 96; k++) {
        const Vec3 p = mobs.LimbVoxelPos(id, li, k * 7919u + 13u);
        const float t = (p - ax.anchor).dot(ax.along);
        if (t < ax.reach * 0.33f || t > ax.reach * 0.66f) continue;
        const float r = (p - ax.anchor - ax.along * t).len();
        if (r > bestR) {
          bestR = r;
          best = p;
        }
      }
      if (mobs.CarveLimbRadial(mobs.LimbBody(id, li), best, kRadius, false,
                               false, c.world, spawns,
                               DamageCtx(DamageCause::SpawnRot)))
        n++;
    }
    return n;
  };
  const uint32_t holes = carvePits(a);
  const uint32_t holesCtl = carvePits(ctl);
  const uint32_t rotMat = mobs.MaterialIdNamed("rotflesh");
  const uint32_t ichorMat = mobs.MaterialIdNamed("ichor");
  const int bothLimb = limbs.front();
  uint32_t bothBitten = 0;
  if (rotMat) {
    const LimbAxis ax = MeasureLimb(mobs, both, bothLimb);
    if (ax.valid) {
      Vec3 at = ax.anchor + ax.along * (ax.reach * 0.5f);
      float bestR = -1.0f;
      for (uint32_t k = 0; k < 96; k++) {
        const Vec3 p = mobs.LimbVoxelPos(both, bothLimb, k * 7919u + 13u);
        const float t = (p - ax.anchor).dot(ax.along);
        if (t < ax.reach * 0.33f || t > ax.reach * 0.66f) continue;
        const float r = (p - ax.anchor - ax.along * t).len();
        if (r > bestR) {
          bestR = r;
          at = p;
        }
      }
      // ONE real bite on the surface. (A loop re-bit "until the rot took"
      // while StainWoundAs's mottle decided a whole bite on one noise value;
      // with it recentred on the wound one bite infects -- 2026-10-01.)
      BiteOnce(mobs, c.world, both, bothLimb, at, 4.0f, (uint16_t)rotMat,
               (uint16_t)ichorMat, 1.0f, 0xB0B0u, spawns);
      bothBitten = mobs.LimbMaterialCount(both, bothLimb, rotMat);
      // ...and the venom's own pit on the FAR side of the same limb, as arm A
      // has. A bite's wall is rot now (the rewrite reaches the wall it tore,
      // 2026-10-01), and rot is not soft tissue: venom poured into the bite
      // itself finds nothing to seed, which is the infections' diets doing
      // their job, not a failure to coexist.
      Vec3 far = at;
      float farD = -1.0f;
      for (uint32_t k = 0; k < 96; k++) {
        const Vec3 p = mobs.LimbVoxelPos(both, bothLimb, k * 7919u + 13u);
        const float t = (p - ax.anchor).dot(ax.along);
        if (t < ax.reach * 0.33f || t > ax.reach * 0.66f) continue;
        const float d = (p - at).len();
        if (d > farD) {
          farD = d;
          far = p;
        }
      }
      mobs.CarveLimbRadial(mobs.LimbBody(both, bothLimb), far, kRadius, false,
                           false, c.world, spawns,
                           DamageCtx(DamageCause::SpawnRot));
    }
  }

  // Measured AFTER the carve and BEFORE the venom, so the deltas are the
  // venom's alone.
  auto sumLimbs = [&](uint64_t id, uint32_t mat) {
    uint32_t n = 0;
    for (int li : limbs) n += mobs.LimbMaterialCount(id, li, mat);
    return n;
  };
  auto sumAll = [&](uint64_t id, uint32_t mat) {
    uint32_t n = 0;
    const Mob* m = mobs.FindMobById(id);
    const int nl = m ? m->LimbCount() : 0;
    for (int li = 0; li < nl; li++) n += mobs.LimbMaterialCount(id, li, mat);
    return n;
  };
  auto hpOf = [&](uint64_t id) {
    float h = 0.0f;
    for (int li : limbs) h += mobs.LimbHp(id, li);
    return h;
  };
  const uint32_t bone0 = boneMat ? sumLimbs(a, boneMat) : 0u;
  const uint32_t boneC0 = boneMat ? sumLimbs(ctl, boneMat) : 0u;
  const float hpA0 = hpOf(a);
  // The flush-tail ledger at the start, so only this run's share is read.
  uint32_t tailBoneA0[(int)DamageCause::Count] = {};
  uint32_t tailBoneC0[(int)DamageCause::Count] = {};
  if (Mob* m = mobs.FindMobById(a))
    for (int k = 0; k < (int)DamageCause::Count; k++)
      tailBoneA0[k] = m->FlushTailBone((DamageCause)k);
  if (Mob* m = mobs.FindMobById(ctl))
    for (int k = 0; k < (int)DamageCause::Count; k++)
      tailBoneC0[k] = m->FlushTailBone((DamageCause)k);

  uint32_t tick = 41999;
  uint32_t soakedA = 0, soakedB = 0;
  for (int li : limbs) {
    soakedA += mobs.SoakLimb(a, li, venom, 15, tick);
    soakedB += mobs.SoakLimb(b, li, venom, 15, tick);
    if (li == bothLimb) mobs.SoakLimb(both, li, venom, 15, tick);
  }

  const Vec3 fo = mobs.MobOrigin(a);
  support::TickCursor ticker{
      c, tick, IVec3{ifloor(fo.x) >> 4, ifloor(fo.y) >> 4, ifloor(fo.z) >> 4}};
  uint32_t peak = 0, firstSeen = 0, clearedAt = 0, ticks = 0, bMax = 0;
  uint32_t doseMax = 0;   // the largest single-limb count on the first tick
  bool aliveA = true;
  for (int i = 0; i < kMaxTicks; i++) {
    ticker();
    ticks = (uint32_t)i + 1;
    const uint32_t now = sumAll(a, envenomed);
    bMax = std::max(bMax, sumAll(b, envenomed));
    if (now > 0 && firstSeen == 0) {
      firstSeen = ticks;
      for (int li : limbs)
        doseMax = std::max(doseMax, mobs.LimbMaterialCount(a, li, envenomed));
    }
    peak = std::max(peak, now);
    if (const Mob* m = mobs.FindMobById(a)) aliveA = m->Alive();
    if (firstSeen && now == 0) {
      clearedAt = ticks;
      break;
    }
  }
  const Mob::InfectStat sa = mobs.InfectStatsOf(a, envenomed);
  const Mob::InfectStat sb = mobs.InfectStatsOf(b, envenomed);
  const Mob::InfectStat bothRot = mobs.InfectStatsOf(both, rotMat);
  const Mob::InfectStat bothVen = mobs.InfectStatsOf(both, envenomed);
  // WHERE IT IS STILL SITTING, when it did not burn out (rule 6: a bare count
  // is not a measurement): every limb still holding envenomed, with whether it
  // has a body and which infections its slots carry.
  std::string left;
  if (!clearedAt) {
    if (Mob* m = mobs.FindMobById(a)) {
      for (int li = 0; li < m->LimbCount(); li++) {
        const uint32_t n = mobs.LimbMaterialCount(a, li, envenomed);
        if (!n) continue;
        left += Format(" %s:%u%s[", li < (int)limbName.size() ? limbName[li].c_str() : "?",
                       n, mobs.LimbBody(a, li) ? "" : " NO BODY");
        left += m->LimbInfected(li) ? "latched" : "NOT latched";
        left += "]";
      }
    }
  }
  // What it converted, by the material each cell was.
  std::string took;
  uint32_t tookBone = 0, tookSkin = 0;
  const uint32_t skinMat = mobs.MaterialIdNamed("skin");
  for (const auto& [mat, n] : sa.took) {
    took += Format(" %s %u", mat < c.mats.size() ? c.mats[mat].name.c_str() : "?", n);
    if (boneMat && mat == boneMat) tookBone += n;
    if (skinMat && mat == skinMat) tookSkin += n;
  }
  float hpInfA = 0.0f, hpInfB = 0.0f;
  if (Mob* m = mobs.FindMobById(a)) hpInfA = m->HpLostBy(DamageCause::Infection);
  if (Mob* m = mobs.FindMobById(b)) hpInfB = m->HpLostBy(DamageCause::Infection);
  const float hpA1 = hpOf(a);
  const uint32_t bone1 = boneMat ? sumLimbs(a, boneMat) : 0u;
  const uint32_t boneC1 = boneMat ? sumLimbs(ctl, boneMat) : 0u;
  // WHERE A's AND THE CONTROL's BONE WENT: the bone FlushBurn's carve tail
  // (CarveLimb's connectivity split / collider re-derive) dropped, by the
  // flush's cause. Venom converts no bone (`took`), so any bone A loses
  // beyond the control's leaves by this door or by none.
  std::string tailA, tailC;
  uint32_t tailBoneA = 0, tailBoneC = 0;
  if (Mob* m = mobs.FindMobById(a))
    for (int k = 0; k < (int)DamageCause::Count; k++) {
      const uint32_t d = m->FlushTailBone((DamageCause)k) - tailBoneA0[k];
      if (d) tailA += Format(" %s %u", DamageCauseName((DamageCause)k), d);
      tailBoneA += d;
    }
  if (Mob* m = mobs.FindMobById(ctl))
    for (int k = 0; k < (int)DamageCause::Count; k++) {
      const uint32_t d = m->FlushTailBone((DamageCause)k) - tailBoneC0[k];
      if (d) tailC += Format(" %s %u", DamageCauseName((DamageCause)k), d);
      tailBoneC += d;
    }
  const int boneLossA = (int)bone0 - (int)bone1;
  const int boneLossC = (int)boneC0 - (int)boneC1;
  const int boneLossVenom = boneLossA - boneLossC;
  restore();

  // THE EXPECTED WINDOW, from the authored numbers at the scaled rate. Per
  // voxel the venom is a subcritical birth-death process: each cell is eaten
  // at mu = eat * s per second (s = lattice cells per world voxel) and spawns
  // at lambda = spread * s while it lives, R = lambda / mu. The mean time for
  // a dose of D to die out is H(D, R) / mu with
  //   H(D, R) = sum_{i=1..D} sum_{j>=0} R^j / (i + j),
  // which is the harmonic number H_D at R = 0 (pure decay). Taken for the
  // largest single-limb dose (the last of the four to clear sets the tick).
  const uint32_t scale = defSkinScale;
  const double R = envDef.infectEat > 0.0f
                       ? (double)envDef.infectSpread / (double)envDef.infectEat
                       : 0.0;
  double H = 0.0;
  for (uint32_t i = 1; i <= doseMax; i++) {
    double rj = 1.0;
    for (int j = 0; j < 400 && rj > 1e-9; j++, rj *= R) H += rj / (double)(i + j);
  }
  const double muPerTick = (double)envDef.infectEat * kScale * scale / 30.0;
  const double expectTicks = muPerTick > 0.0 ? H / muPerTick : 0.0;
  const double window = expectTicks > 0.0 && clearedAt
                            ? (double)(clearedAt - firstSeen + 1) / expectTicks
                            : 0.0;
  const double ratio = sa.seeded ? (double)sa.eaten / (double)sa.seeded : 0.0;
  // Real-time equivalent of the measured burn-out, for the reader.
  const double realSec = (double)clearedAt * kScale / 30.0;

  RecordObserved("venomWoundSeeded", (double)sa.seeded);
  RecordObserved("venomWoundSpread", (double)sa.spread);
  RecordObserved("venomWoundEaten", (double)sa.eaten);
  RecordObserved("venomWoundEatRatio", ratio);
  RecordObserved("venomWoundPeak", (double)peak);
  RecordObserved("venomWoundClearTicks", (double)clearedAt);
  RecordObserved("venomWoundRealSeconds", realSec);
  RecordObserved("venomWoundWindow", window);
  RecordObserved("venomWoundHpInfection", (double)hpInfA);
  RecordObserved("venomWoundControlSeeded", (double)sb.seeded);
  RecordObserved("venomWoundBoneLossA", (double)boneLossA);
  RecordObserved("venomWoundBoneLossControl", (double)boneLossC);
  RecordObserved("venomWoundBoneLossVenom", (double)boneLossVenom);

  const bool seeded = sa.seeded > 0 && firstSeen > 0;
  const bool spread = sa.spread > 0;
  const bool burntOut = clearedAt > 0;
  // REPORTED, not asserted: a cell that reaches a joint's twin overlap is
  // copied to (and removed from) the other limb's copy by the twin sync, which
  // neither seeds nor eats, so the books can be off by a cell or two there.
  const bool conserved = sa.eaten == sa.seeded + sa.spread;
  const bool threeX = ratio >= kRatioLo && ratio <= kRatioHi;
  const bool onTime = window >= kWindowLo && window <= kWindowHi;
  const bool hurt = hpInfA > 0.0f && sa.hp > 0.0f && hpA1 < hpA0;
  // BONE AND SKIN STOP IT: not one cell of either was ever converted. (The
  // bone CENSUS is reported, not asserted: the carve's connectivity split
  // drops stranded specks of bone once the flesh round them is gone, which is
  // the wound model's business and not the venom's.)
  const bool boneKept = tookBone == 0 && tookSkin == 0;
  const bool skinStops = sb.seeded == 0 && sb.spread == 0 && bMax == 0 &&
                         hpInfB == 0.0f;
  // ...AND THE BONE CENSUS, MINUS THE CONTROL: whatever A loses beyond the
  // same pits without venom is the venom's, and that must be (about) none.
  const int kBoneLossMax = (int)BaselineNumber("venomWoundBoneLossMax", 2);
  const bool boneCensus = boneLossVenom <= kBoneLossMax;
  // ROT AND VENOM TOGETHER: both ran on the one limb.
  const bool coexist = !rotMat || bothBitten == 0 ||
                       ((bothRot.spread + bothRot.eaten) > 0 &&
                        (bothVen.seeded > 0 && bothVen.spread + bothVen.eaten > 0));

  detail = Format(
      "%u holes, venom on %u / %u cells (A / B) | A: seeded %u, spread %u, "
      "eaten %u (eaten/seeded %.2f, band %.1f..%.1f), peak %u, first tick %u, "
      "clear at %u of %d (x%.0f: %.0f s real; %.2f of the %.0f-tick "
      "expectation for the largest dose %u), hp to Infection %.2f (flat %.2f), "
      "limbs hp %.1f -> %.1f, bone %u -> %u%s | B (whole skin): seeded %u, "
      "spread %u, max envenomed %u, hp to Infection %.2f",
      holes, soakedA, soakedB, sa.seeded, sa.spread, sa.eaten, ratio, kRatioLo,
      kRatioHi, peak, firstSeen, clearedAt, kMaxTicks, kScale, realSec, window,
      expectTicks, doseMax, hpInfA, sa.hp, hpA0, hpA1, bone0, bone1,
      aliveA ? "" : " (A DIED)", sb.seeded, sb.spread, bMax, hpInfB);
  detail += conserved ? " | books balance" : " | books off (twin overlap)";
  detail += " | took:" + took;
  detail += Format(" | BONE: A %u -> %u (-%d), control (same pits, no venom, "
                   "%u pits) %u -> %u (-%d), venom-attributable %d; flush-tail "
                   "bone A %u [%s ], control %u [%s ]",
                   bone0, bone1, boneLossA, holesCtl, boneC0, boneC1, boneLossC,
                   boneLossVenom, tailBoneA, tailA.c_str(), tailBoneC,
                   tailC.c_str());
  detail += Format(" | ROT+VENOM on one arm: bite left %u rot; rot spread %u "
                   "eaten %u, venom seeded %u spread %u eaten %u",
                   bothBitten, bothRot.spread, bothRot.eaten, bothVen.seeded,
                   bothVen.spread, bothVen.eaten);
  RecordObserved("venomWoundBothRotActs", (double)(bothRot.spread + bothRot.eaten));
  RecordObserved("venomWoundBothVenomActs", (double)(bothVen.spread + bothVen.eaten));
  if (!left.empty()) detail += " | still envenomed:" + left;
  if (seeded && spread && burntOut && threeX && onTime && hurt && boneKept &&
      skinStops && boneCensus && coexist)
    return Status::Pass;
  const char* why = !seeded      ? "the coat seeded nothing in the wounds"
                    : !spread    ? "the venom never spread"
                    : !burntOut  ? "the venom did not burn itself out in the window"
                    : !threeX    ? "eaten / seeded is outside the band round 3"
                    : !onTime    ? "the burn-out took far from H(D, R) / mu"
                    : !hurt      ? "no hp was booked to Infection"
                    : !boneKept  ? "the venom converted bone or skin"
                    : !boneCensus ? "A lost more bone than the control"
                    : !coexist   ? "rot and venom did not both run on one limb"
                                 : "a venom coat on whole skin seeded or hurt";
  detail = std::string(why) + ": " + detail;
  return Status::Fail;
}

// ---------------------------------------------------------------------------
// rot-clock -- HOW FAST A ROT WOUND ON AN ARM GOES ANYWHERE (a measurement)
// ---------------------------------------------------------------------------
//
// The owner's per-voxel infection (PLAN_weapon_coats B, follow-up) changes the
// rot from a per-limb clock to a branching process, and the rot's rates have
// to be retuned so a bitten arm behaves on roughly the same timescale as
// before. This is the ruler for that. It reports; it asserts only that the
// rot was seeded and went somewhere.
//
// A human's upper arm gets a small pit (the venom gate's), and the pit is
// soaked with ichor whose coat -- in a COPY of the material table, data only
// -- infects `rotflesh`, so the seed is the generic coat path and identical
// before and after the change. The tick runs at gore.infectMobMult x
// rotClockCrank (every rate of the rot scales by it, so time scales by it
// exactly) until the rot reaches a SECOND limb (the shoulder: what a bite has
// to do before it can ever reach the torso and kill) or rotClockMaxTicks.
// Reported: seeded cells, rot on the arm at checkpoints, the tick the rot
// crossed, and that tick in real (uncranked) minutes.
Status GateRotClock(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const int human = mobs.FindDef("human");
  const uint32_t rot = mobs.MaterialIdNamed("rotflesh");
  int ichorIdx = -1;
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == "ichor") ichorIdx = (int)i;
  if (human < 0 || !rot || ichorIdx < 0) {
    detail = "no `human`, `rotflesh` or `ichor`";
    return Status::Skip;
  }
  const float kCrank = (float)BaselineNumber("rotClockCrank", 40.0);
  const int kMaxTicks = (int)BaselineNumber("rotClockMaxTicks", 4000);
  int armLimb = -1;
  std::vector<std::string> names;
  {
    const MobDef& def = mobs.Defs()[human];
    for (size_t li = 0; li < def.limbs.size(); li++) {
      names.push_back(def.limbs[li].name);
      const MobLimbDef& ld = def.limbs[li];
      if (armLimb >= 0 || ld.tag != "arm") continue;
      bool parentSame = false;
      for (const MobLimbDef& p : def.limbs)
        if (p.name == ld.parent && p.tag == ld.tag) parentSame = true;
      if (!parentSame) armLimb = (int)li;
    }
  }
  if (armLimb < 0) {
    detail = "the human has no upper arm";
    return Status::Fail;
  }
  const Tuning saved = CurrentTuning();
  Tuning tt = saved;
  tt.gore.infectMobMult = saved.gore.infectMobMult * kCrank;
  SetCurrentTuning(tt);
  mobs.Reset();
  c.debris.Reset();
  auto restore = [&]() {
    mobs.Reset();
    c.debris.Reset();
    SetCurrentTuning(saved);
  };
  const uint64_t id = mobs.Spawn(human, FixtureSite(c.world, 415));
  if (!id) {
    restore();
    detail = "could not spawn a human";
    return Status::Fail;
  }
  mobs.SetMobBehavior(id, "dummy");
  // DIRECT PHASE CALLS ON PURPOSE (W2-O): fixture POSING, as SpawnTarget.
  for (int i = 0; i < 8; i++) {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> sp;
    std::vector<CellOp> cellOps;
    mobs.PreTick(3000u + (uint32_t)i, c.world, ops, cellOps, sp);
    c.phys.Step(kTickDt);
    mobs.PostStep();
  }
  std::vector<ParticleSpawn> spawns;
  {
    const LimbAxis ax = MeasureLimb(mobs, id, armLimb);
    Vec3 best = ax.anchor + ax.along * (ax.reach * 0.5f);
    float bestR = -1.0f;
    for (uint32_t k = 0; k < 96; k++) {
      const Vec3 p = mobs.LimbVoxelPos(id, armLimb, k * 7919u + 13u);
      const float t = (p - ax.anchor).dot(ax.along);
      if (t < ax.reach * 0.33f || t > ax.reach * 0.66f) continue;
      const float r = (p - ax.anchor - ax.along * t).len();
      if (r > bestR) {
        bestR = r;
        best = p;
      }
    }
    // A ZOMBIE'S BITE, through the real BiteHit (the tear, the rewrite of
    // the flesh it exposed to rotflesh, the ichor smear), at full power.
    BiteOnce(mobs, c.world, id, armLimb, best, 4.0f, (uint16_t)rot,
             (uint16_t)ichorIdx, 1.0f, 0xB17E5EEDu, spawns);
  }
  const uint32_t bitten = mobs.LimbMaterialCount(id, armLimb, rot);
  uint32_t tick = 43999;
  const Vec3 fo = mobs.MobOrigin(id);
  support::TickCursor ticker{
      c, tick, IVec3{ifloor(fo.x) >> 4, ifloor(fo.y) >> 4, ifloor(fo.z) >> 4}};
  uint32_t crossedAt = 0, ticks = 0, peakArm = 0, diedAt = 0;
  std::string crossedTo, curve;
  for (int i = 0; i < kMaxTicks; i++) {
    ticker();
    ticks = (uint32_t)i + 1;
    Mob* m = mobs.FindMobById(id);
    if (!m) break;
    const uint32_t arm = mobs.LimbMaterialCount(id, armLimb, rot);
    peakArm = std::max(peakArm, arm);
    if (ticks % 250 == 0) curve += Format(" %u", arm);
    if (!m->Alive() && !diedAt) diedAt = ticks;
    if (!crossedAt)
      for (int li = 0; li < m->LimbCount(); li++)
        if (li != armLimb && mobs.LimbMaterialCount(id, li, rot)) {
          crossedAt = ticks;
          crossedTo = li < (int)names.size() ? names[li] : "?";
          break;
        }
    if (crossedAt) break;
  }
  const Mob::InfectStat st = mobs.InfectStatsOf(id, rot);
  const uint32_t armEnd = mobs.LimbMaterialCount(id, armLimb, rot);
  restore();
  const double realMin = (double)crossedAt * kCrank / 30.0 / 60.0;
  RecordObserved("rotClockBitten", (double)bitten);
  RecordObserved("rotClockCrossTicks", (double)crossedAt);
  RecordObserved("rotClockCrossRealMinutes", realMin);
  RecordObserved("rotClockPeakArm", (double)peakArm);
  detail = Format(
      "x%.0f crank: the bite left %u rot; spread %u, eaten %u; rot on the arm every 250 "
      "ticks:%s (peak %u, end %u); crossed to %s at tick %u of %d = %.1f real "
      "minutes%s",
      kCrank, bitten, st.spread, st.eaten, curve.c_str(), peakArm, armEnd,
      crossedAt ? crossedTo.c_str() : "NOTHING", crossedAt, kMaxTicks, realMin,
      diedAt ? Format(" (DIED at tick %u)", diedAt).c_str() : "");
  return bitten > 0 && (st.spread > 0 || st.eaten > 0) ? Status::Pass
                                                       : Status::Fail;
}

// ---------------------------------------------------------------------------
// infect-perf -- THE INFECTION COSTS WHAT IT IS, NOT WHAT THE LIMB IS (rule 2)
// ---------------------------------------------------------------------------
//
// A crowd of bitten humans (every arm, leg and the torso of each), the rot
// cranked so it moves inside the window, ticked through the real tick twice in
// one process: arm A with MobSystem::SetInfectFullSweep (every InfectStep
// re-derives its cells with a whole-lattice sweep and the burn index is let go
// on the idle grace -- the pre-2026-10-01 cost model), arm B with the limb's
// cell list. Same creatures, same ids, same ticks.
//
// CLAIMS: the two arms end with EXACTLY the same infection (cells and eaten,
// per material) -- the list is a cost change, not a behaviour change, because
// it visits the cells in the sweep's storage order -- and B's InfectTick time
// is at most infectPerfMaxRatio of A's. The times are wall clock and noisy;
// the ratio is the claim, the absolute numbers are recorded.
Status GateInfectPerf(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const int human = mobs.FindDef("human");
  const uint32_t rot = mobs.MaterialIdNamed("rotflesh");
  const uint32_t ichor = mobs.MaterialIdNamed("ichor");
  if (human < 0 || !rot || !ichor) {
    detail = "no `human`, `rotflesh` or `ichor`";
    return Status::Skip;
  }
  const int kCrowd = (int)BaselineNumber("infectPerfCrowd", 8);
  const int kTicks = (int)BaselineNumber("infectPerfTicks", 300);
  const float kCrank = (float)BaselineNumber("infectPerfCrank", 1.0);
  const float kIdCrank = (float)BaselineNumber("infectPerfIdentityCrank", 40.0);
  const double kMaxRatio = BaselineNumber("infectPerfMaxRatio", 0.5);
  std::vector<int> bitLimbs;
  {
    const MobDef& def = mobs.Defs()[human];
    for (size_t li = 0; li < def.limbs.size(); li++) {
      const std::string& tg = def.limbs[li].tag;
      if (tg == "arm" || tg == "leg" || (int)li == def.rootLimb ||
          def.limbs[li].name == "torso")
        bitLimbs.push_back((int)li);
    }
  }
  const Tuning saved = CurrentTuning();
  struct Out {
    double ms = 0.0, selectMs = 0.0, ruleMs = 0.0;
    uint64_t sweeps = 0, steps = 0;
    uint32_t cells = 0, eaten = 0, spread = 0, bitten = 0;
    int mobs = 0;
  };
  auto run = [&](bool full, float crank) -> Out {
    Out o;
    IdCounterScope ids(mobs);   // both arms number their creatures alike
    mobs.Reset();
    c.debris.Reset();
    // Every arm on the same fresh ground (a crowd bleeding for 300 ticks
    // stains it), and the ground handed back clean (below).
    PrepareWorld(c);
    Tuning tt = saved;
    tt.gore.infectMobMult = saved.gore.infectMobMult * crank;
    SetCurrentTuning(tt);
    mobs.SetInfectFullSweep(full);
    std::vector<uint64_t> crowd;
    for (int i = 0; i < kCrowd; i++) {
      const uint64_t id = mobs.Spawn(human, FixtureSite(c.world, 300 + 12 * i));
      if (!id) continue;
      mobs.SetMobBehavior(id, "dummy");
      crowd.push_back(id);
    }
    // DIRECT PHASE CALLS ON PURPOSE (W2-O): fixture POSING, as SpawnTarget.
    for (int i = 0; i < 8; i++) {
      std::vector<BrushOp> ops;
      std::vector<ParticleSpawn> sp;
      std::vector<CellOp> cellOps;
      mobs.PreTick(3000u + (uint32_t)i, c.world, ops, cellOps, sp);
      c.phys.Step(kTickDt);
      mobs.PostStep();
    }
    std::vector<ParticleSpawn> spawns;
    for (uint64_t id : crowd)
      for (int li : bitLimbs) {
        if (!mobs.LimbBody(id, li)) continue;
        const LimbAxis ax = MeasureLimb(mobs, id, li);
        if (!ax.valid) continue;
        BiteOnce(mobs, c.world, id, li, SkinAim(mobs, id, li, ax), 4.0f,
                 (uint16_t)rot, (uint16_t)ichor, 1.0f,
                 0x9E7Fu + (uint32_t)li * 977u, spawns);
      }
    for (uint64_t id : crowd)
      for (int li : bitLimbs) o.bitten += mobs.LimbMaterialCount(id, li, rot);
    mobs.ResetInfectCost();
    {
      const IVec3 fc = FixtureSite(c.world, 300 + 6 * kCrowd);
      support::TickRig rig(c, 52000u, IVec3{fc.x >> 4, fc.y >> 4, fc.z >> 4});
      support::RunTicks(rig, kTicks);
    }
    const MobSystem::InfectCost cost = mobs.InfectCostStats();
    o.ms = cost.ms;
    o.selectMs = cost.selectMs;
    o.ruleMs = cost.ruleMs;
    o.sweeps = cost.sweeps;
    o.steps = cost.limbSteps;
    for (uint64_t id : crowd) {
      const Mob* m = mobs.FindMobById(id);
      if (!m) continue;
      o.mobs++;
      for (int li = 0; li < m->LimbCount(); li++)
        o.cells += mobs.LimbMaterialCount(id, li, rot);
      const Mob::InfectStat st = mobs.InfectStatsOf(id, rot);
      o.eaten += st.eaten;
      o.spread += st.spread;
    }
    mobs.SetInfectFullSweep(false);
    mobs.Reset();
    c.debris.Reset();
    SetCurrentTuning(saved);
    return o;
  };
  // IDENTITY at a crank that makes the rot busy (spread, eat, flushes, joint
  // crossings all happen), COST at the shipped rates (what a crowd of rotting
  // creatures actually pays a tick).
  const Out ia = run(true, kIdCrank);
  const Out ib = run(false, kIdCrank);
  const Out a = run(true, kCrank);
  const Out b = run(false, kCrank);
  // THE GROUND BACK AS IT WAS: eight humans bled on it for 1,200 ticks, and a
  // later gate's fixture site (player-corpse's inset 360) is among theirs.
  PrepareWorld(c);
  RecordObserved("infectPerfFullMs", a.ms);
  RecordObserved("infectPerfListMs", b.ms);
  RecordObserved("infectPerfFullSweeps", (double)a.sweeps);
  RecordObserved("infectPerfListSweeps", (double)b.sweeps);
  const double perStepA = a.steps ? a.ms * 1000.0 / (double)a.steps : 0.0;
  const double perStepB = b.steps ? b.ms * 1000.0 / (double)b.steps : 0.0;
  auto sameOut = [](const Out& x, const Out& y) {
    return x.cells == y.cells && x.eaten == y.eaten && x.spread == y.spread &&
           x.bitten == y.bitten;
  };
  const bool same = sameOut(ia, ib) && sameOut(a, b);
  // THE PHASE THAT CHANGED is finding the cells; the rule over them and the
  // flush are the same work in both arms (and the same outcome).
  const bool cheaper = b.selectMs <= a.selectMs * kMaxRatio && b.ms <= a.ms;
  const bool bit = a.bitten > 0 && a.steps > 0 && ia.eaten > 0;
  detail = Format(
      "%d humans x %zu bitten limbs (%u rot at the bite), %d ticks | AT x%.0f "
      "(identity): full %u rot / %u eaten / %u spread, list %u / %u / %u, "
      "%.1f -> %.1f ms | AT x%.0f (cost): FULL SWEEP %.2f ms over %llu "
      "limb-steps (%.1f us each, %llu sweeps), CELL LIST %.2f ms (%.1f us "
      "each, %llu sweeps) = %.0f%%; finding the cells %.2f -> %.2f ms "
      "(%.0f%%), the rule %.2f -> %.2f ms | outcome %s",
      a.mobs, bitLimbs.size(), a.bitten, kTicks, kIdCrank, ia.cells, ia.eaten,
      ia.spread, ib.cells, ib.eaten, ib.spread, ia.ms, ib.ms, kCrank, a.ms,
      (unsigned long long)a.steps, perStepA, (unsigned long long)a.sweeps, b.ms,
      perStepB, (unsigned long long)b.sweeps,
      a.ms > 0.0 ? 100.0 * b.ms / a.ms : 0.0, a.selectMs, b.selectMs,
      a.selectMs > 0.0 ? 100.0 * b.selectMs / a.selectMs : 0.0, a.ruleMs,
      b.ruleMs, same ? "IDENTICAL" : "DIFFERS");
  if (!bit) return (detail = "the crowd was not infected: " + detail, Status::Fail);
  if (!same) return (detail = "the cell list changed the outcome: " + detail, Status::Fail);
  if (!cheaper)
    return (detail = "the cell list is not cheaper than the sweep: " + detail,
            Status::Fail);
  return Status::Pass;
}

}  // namespace

const std::vector<Gate>& ImpactGates() {
  static const std::vector<Gate> g = {
      {"impact-blunt", "mob", {}, false, GateImpactBlunt, /*needsRender=*/false},
      {"impact-armor", "mob", {}, false, GateImpactArmor, false},
      {"impact-fist", "mob", {}, false, GateImpactFist, false},
      {"bite-rot", "mob", {}, false, GateBiteRot, false},
      {"bite-infect", "mob", {}, false, GateBiteInfect, false},
      {"bite-limbs", "mob", {}, false, GateBiteLimbs, false},
      {"joint-rot", "mob", {}, false, GateJointRot, false},
      {"wood-bleed", "mob", {}, false, GateWoodBleed, false},
      {"bleed-fluid", "mob", {}, false, GateBleedFluid, false},
      {"mob-race", "mob", {}, false, GateMobRace, false},
      {"venom-wound", "mob", {}, false, GateVenomWound, false},
      {"rot-clock", "mob", {}, false, GateRotClock, false},
      {"infect-perf", "mob", {}, false, GateInfectPerf, false},
  };
  return g;
}

}  // namespace selftest
