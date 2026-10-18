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
#include <climits>
#include <cstring>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "game/item.h"
#include "game/avatar.h"   // player-corpse: the avatar's rig moves into mobs_
#include "game/equipment.h"
#include "game/player.h"
#include "game/mob.h"
#include "game/melee.h"  // hit-drive swings the real sweep
#include "game/pierce.h"  // pierce: PierceAlongRay, the projectile entry point
#include "game/arrows.h"  // arrow: the bow, the flights, the embed
#include "game/worlditems.h"  // arrow: a stuck arrow is a ground item
#include "game/lookpick.h"    // look-pick: the forgiving pick under the reticle
#include "game/itemcoat.h"   // poison-arrow: an arrow's own lattice and coat
#include "game/itemstage.h"  // poison-arrow: brush one, copy to N
#include "game/quiver.h"     // poison-arrow: the quiver
#include "game/caster.h"     // poison-arrow: the PLYR section's refs
#include "game/persist.h"    // poison-arrow: a quiver through a save
#include "sim/rng.h"         // poison-arrow: the draw's key
#include "game/anim.h"   // QuatRotate: a joint anchor is body-local
#include "game/bodyreg.h"  // heal-restore's optional picture sequence
#include "gpu/resources.h"  // CreateBuffer, ditto
#include "sim/currentprim.h"  // float-current: a stream through the basin
#include "sim/microbody.h"
#include "phys/bodystain.h"   // bruise-is-skin: SoakBruise and the coat rules
#include "phys/floatrho.h"    // corpse-float: the one floating-density rule
#include "phys/coatcontact.h"  // lava-quench: BuildLatticeOcc (which voxels are surface)
#include "sim/tuning.h"
#include "sim/weather.h"  // corpse-sleep: the rain word, for attribution
#include "test/selftest.h"
#include "sim/scale.h"
#include "test/support.h"
#include "test/tickrig.h"

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
// CutLimb is always DamageCause::Blade (it used to need a BladeCutScope around
// it to mark the sever `byBlade`, the cause the dismember audio switches on);
// `power` rides along as the audio's severity, as melee passes it.
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
  cut.halfWidth = std::max(0.9f * g.cutWidth, g.cutWidthMin);
  cut.depth = (g.cutDepth + g.cutDepthPower * power) * heft;
  cut.length = g.cutLength * (0.4f + 0.6f * power) * heft;
  cut.power = power;
  cut.seed = seed;
  return mobs.CutLimb(body, cut, world, spawns, power);
}

// Pick the fixture: the biggest severable, non-vital limb in the library.
//
// Measured by SPAWNING rather than by reading the def's prefab, because what
// the test cuts is the runtime lattice — a def's art voxel count says nothing
// about physScale, skinScale, or whether the rig gave that limb a body at all.
// Each candidate is despawned again, so this leaves the system as it found it.
// `only` non-null = that def alone (a gate whose claim is about a human body
// should not ride whichever creature the library's biggest limb belongs to).
Target ChooseTarget(MobSystem& mobs, IVec3 at, const char* only = nullptr) {
  Target best;
  for (size_t d = 0; d < mobs.Defs().size(); d++) {
    const MobDef& def = mobs.Defs()[d];
    if (def.limbs.empty() || def.bleedMat == 0) continue;  // must be able to bleed
    if (only && def.name != only) continue;
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
  // DIRECT PHASE CALLS ON PURPOSE (W2-O): fixture POSING, not the subject, and
  // every spawn replays ticks 1000.. so each target is posed identically; the
  // real tick would advance the world clock between fixtures.
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
// hair-rooted: a blade cuts hair, it does not take it off whole
// ---------------------------------------------------------------------------
// The owner, 2026-09-25: "when in combat entire hair pieces fall off; hair
// shouldn't come off of mobs entirely as if it's an independent limb / wig".
// A hair piece is `severable` only so that losing it is no death, and that
// flag opted it into the arm's whole-limb severs -- the one-cell joint test at
// its anchor above all, which a single nick at the scalp tripped.
// SeverPolicy::rooted takes those away (severpolicy.h).
//
// FIXTURE. The biggest hair piece on any loaded def, cut hard and repeatedly,
// half the strokes AT THE ANCHOR (the joint test's whole subject) and half
// through the mass. Asserted: the cuts took hair (it is a wound, not armour)
// and the piece is still on the creature at the end.
// The biggest bloodless (hair) limb, measured by spawning -- ChooseTarget's
// method, with its hair exclusion inverted. Invalid if nothing has hair.
Target ChooseHairTarget(Ctx& c, bool brainHeadOnly = false) {
  MobSystem& mobs = c.mobs;
  // Hair lives on the random-human POOL bodies (MobSystem::PoolDef), which no
  // startup load lists, so the candidates are the loaded defs plus the first
  // pool body that has any.
  auto hasHair = [&](int d) {
    for (const MobLimbDef& ld : mobs.Defs()[d].limbs)
      if (ld.bloodless) return true;
    return false;
  };
  // THE AUTHORED CAST ONLY, never a RUNTIME def (a composed pool body, a
  // "+zombie" an earlier gate built). Which runtime defs exist when this runs
  // is the SUITE's history, so a candidate list that included them picked a
  // different creature in the full run than alone -- corpse-head-laser read a
  // dead hair turn of 0.037 in the suite against 0.000 standalone (suite triage
  // 2026-10-01). The pool below is the fallback for a cast with no hair at all.
  std::vector<int> cands;
  std::string skipped;
  for (size_t d = 0; d < mobs.Defs().size(); d++) {
    if (mobs.Defs()[d].name.empty() || !hasHair((int)d)) continue;
    if (d >= mobs.LoadedDefCount()) {
      skipped += " " + mobs.Defs()[d].name;
      continue;
    }
    cands.push_back((int)d);
  }
  if (!skipped.empty())
    std::printf("hair target: skipped runtime defs with hair:%s\n",
                skipped.c_str());
  for (const std::string& n : mobs.PoolNames()) {
    if (!cands.empty()) break;
    const int d = mobs.PoolDef(n, nullptr);
    if (d >= 0 && hasHair(d)) cands.push_back(d);
  }
  Target t;
  const IVec3 site = FixtureSite(c.world, 170);
  for (int d : cands) {
    const MobDef& def = mobs.Defs()[d];
    mobs.Reset();
    const uint64_t id = mobs.Spawn(d, site);
    if (!id) continue;
    for (size_t li = 0; li < def.limbs.size(); li++) {
      if (!def.limbs[li].bloodless || !mobs.LimbBody(id, (int)li)) continue;
      // Optionally: hair whose PARENT holds a brain (see corpse-head-laser).
      if (brainHeadOnly) {
        const uint32_t brain = mobs.MaterialIdNamed("brain");
        int pi = -1;
        for (size_t k = 0; k < def.limbs.size(); k++)
          if (def.limbs[k].name == def.limbs[li].parent) pi = (int)k;
        if (brain == 0 || pi < 0 ||
            mobs.LimbMaterialCount(id, pi, brain) == 0)
          continue;
      }
      const uint32_t n = mobs.LimbVoxelsAtSpawn(id, (int)li);
      if (n <= t.atSpawn) continue;
      t.defIndex = d;
      t.limb = (int)li;
      t.atSpawn = n;
      t.defName = def.name;
      t.limbName = def.limbs[li].name;
    }
  }
  mobs.Reset();
  return t;
}

Status GateHairRooted(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const Target t = ChooseHairTarget(c);
  if (!t.valid()) {
    detail = "no loaded or pool mob def has a hair (bloodless) limb with a body";
    return Status::Fail;
  }
  IVec3 pchunk{};
  const uint64_t id = SpawnTarget(c, t, 170, pchunk);
  if (!id) { detail = "spawn refused"; return Status::Fail; }

  const uint32_t before = mobs.LimbArtVoxelCount(id, t.limb);
  std::vector<ParticleSpawn> spawns;
  const int kCuts = 16;
  int landed = 0, cutsMade = 0;
  for (int k = 0; k < kCuts && mobs.LimbBody(id, t.limb); k++) {
    const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
    const float along = (k & 1) ? ax.reach * (0.25f + 0.1f * (float)(k % 5))
                                : ax.reach * 0.05f;
    cutsMade++;
    if (CutOnce(mobs, c.world, id, t.limb, ax, along, 1.0f, 1.5f,
                0xA1Au + (uint32_t)k, spawns))
      landed++;
  }
  const bool attached = mobs.LimbBody(id, t.limb) != 0;
  const uint32_t after = attached ? mobs.LimbArtVoxelCount(id, t.limb) : 0;
  const bool cut = landed > 0 && after < before;

  // ...AND THE CUT HAIR STAYS ON THE CORPSE'S HEAD. Owner report 2026-09-27:
  // "hit an npc with hair with a sword and his corpse had another set of hair
  // offset from the head". A carve that moves the brick's min corner rebases
  // the limb (ReskinLimbMicro), and RebuildLimbBody re-created the hair's
  // Fixed joint with the live limb one rebase shift off its art. In life the
  // rig re-poses the hair every tick, so nothing showed; on death the joint
  // takes over and holds the hair that shift away from the scalp, next to the
  // head's own painted cap.
  //
  // So the hair is carved again from its FIRST voxel (the low side of its
  // brick, which is what forces a rebase), and two things are measured once
  // the rig has re-posed it: how far that rebase moved the hair's origin in
  // the head's frame (`rebase`, recorded: nonzero says the fixture exercised
  // the path), and how far apart the hair-head joint's two anchors are
  // through the LIVE poses (`mismatch`, asserted: that gap is exactly what
  // the joint will pull the hair through the moment the creature dies). Not
  // the corpse's own drift: that is mostly the solver's sag under a heavy
  // afro and measured 0.01..0.59 vox on the same tree.
  float rebase = -1.0f, mismatch = -1.0f;
  const int headLi = [&]() {
    const MobDef& def = mobs.Defs()[t.defIndex];
    for (size_t k = 0; k < def.limbs.size(); k++)
      if (def.limbs[k].name == def.limbs[t.limb].parent) return (int)k;
    return -1;
  }();
  auto hairInHead = [&](Vec3& out) {
    const uint64_t hb = mobs.LimbBody(id, t.limb);
    const uint64_t pb = headLi >= 0 ? mobs.LimbBody(id, headLi) : 0;
    if (!hb || !pb) return false;
    BodyTransform hx{}, px{};
    c.phys.GetTransform(hb, hx);
    c.phys.GetTransform(pb, px);
    const Quat pq{px.quat[0], px.quat[1], px.quat[2], px.quat[3]};
    out = QuatRotateInv(pq, hx.pos - px.pos);
    return true;
  };
  auto jointGap = [&]() {
    const uint64_t hb = mobs.LimbBody(id, t.limb);
    const uint64_t pb = headLi >= 0 ? mobs.LimbBody(id, headLi) : 0;
    if (!hb || !pb) return -1.0f;
    std::vector<Physics::BodyJoint> hj, pj;
    c.phys.JointsOn(hb, hj);
    c.phys.JointsOn(pb, pj);
    const Physics::BodyJoint* a = nullptr;
    const Physics::BodyJoint* b = nullptr;
    for (const auto& j : hj) if (j.other == pb) a = &j;
    for (const auto& j : pj) if (j.other == hb) b = &j;
    if (!a || !b) return -1.0f;
    BodyTransform hx{}, px{};
    c.phys.GetTransform(hb, hx);
    c.phys.GetTransform(pb, px);
    const Quat hq{hx.quat[0], hx.quat[1], hx.quat[2], hx.quat[3]};
    const Quat pq{px.quat[0], px.quat[1], px.quat[2], px.quat[3]};
    return ((hx.pos + QuatRotate(hq, a->anchorLocalVox)) -
            (px.pos + QuatRotate(pq, b->anchorLocalVox))).len();
  };
  if (attached && headLi >= 0) {
    uint32_t tick = 61999;
    support::TickCursor ticker{c, tick, pchunk};
    for (int i = 0; i < 3; i++) ticker();
    Vec3 pre{}, live{};
    hairInHead(pre);
    // Measured after EVERY carve, not once at the end: a later carve that
    // does not rebase rebuilds the joint from the (by then re-posed) live
    // pose and heals it, so only the gap right after the rebasing carve shows
    // what a creature killed by that blow would have kept.
    for (int k = 0; k < 4 && mobs.LimbBody(id, t.limb); k++) {
      mobs.CarveLimbRadial(mobs.LimbBody(id, t.limb),
                           mobs.LimbVoxelPos(id, t.limb, 0), 1.5f,
                           /*ragged=*/false, /*eject=*/false, c.world, spawns);
      spawns.clear();
      for (int i = 0; i < 2; i++) ticker();
      mismatch = std::max(mismatch, jointGap());
    }
    if (hairInHead(live)) rebase = (live - pre).len();
  }
  const float gapMax = (float)BaselineNumber("hairJointGapMax", 0.05);
  const bool stays = mismatch >= 0.0f && mismatch <= gapMax;
  mobs.Reset();
  c.debris.Reset();

  const bool ok = attached && cut && stays;
  detail = Format("%s/%s: %d/%d cuts landed, %u -> %u voxels, %s; hair rebased "
                  "%.2f vox, hair-head joint gap %.3f vox (max %.3f)",
                  t.defName.c_str(), t.limbName.c_str(), landed, cutsMade,
                  before, after, attached ? "still on" : "CAME OFF WHOLE",
                  rebase, mismatch, gapMax);
  std::printf("hair-rooted: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// corpse-head-laser: a beam kill leaves the head on the neck, the hair on the head
// ---------------------------------------------------------------------------
// Owner report 2026-09-29: "when killing a mob via a laser their head gets
// offset and displaced on the corpse ... the head becomes decoupled from their
// hair". The laser is the one weapon that keeps carving the SAME limb every
// tick, alive and dead, and that kills from the head (the brain's materials.json organHp), so
// the head is rebased and rebuilt dozens of times across the moment the rig
// is handed to the solver.
//
// FIXTURE. The haired def hair-rooted uses; the hair piece whose parent is the
// head (vital). The beam bores the head tick after tick through the real
// entry points (MobSystem::LaserHit, then the phase-K CarveLimbRadial clean
// bore), aimed alternately at a surviving voxel and at the brick's FIRST voxel
// (its low side: the carve that forces a rebase), until the creature dies and
// for kPostDeath ticks after; then the corpse settles.
//
// Measured every tick, at the joints (MobSystem::LimbJointGap, invariant
// across rebases): hair-head and head-neck, through the poses the ART is drawn
// at and through the colliders, plus each limb's art-vs-collider split and the
// hair's rotation in the head frame (its joint is Fixed: that must not turn).
Status GateCorpseHeadLaser(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  // The gate's own ids (head-cleave's reason): the neck transient is keyed on
  // the creature id through the carve noise, and read 0.37 and 0.55 on two
  // gate lists with the counter inherited. SpawnTarget resets the mobs, so no
  // live creature shares an id; the scope restores the suite's counter.
  mobs.SetNextIdCounter(1);
  PrepareWorld(c);
  // A HEAD WITH A BRAIN IN IT. The laser kills through the brain's organHp (materials.json),
  // and the claim (the head stays on its neck, the hair on the head, through
  // that kill and the corpse after it) was measured at adbc47d (2026-09-29
  // 11:29) on a human: head-neck 0.33 vox transient against the 0.6 cap.
  // a411808 (the same day, 17:08) added the sylvan `deku`, whose leafy crown
  // is now the biggest hair piece loaded, so the pick moved onto a wooden,
  // brainless head that dies by volume 49 ticks in -- and its neck transient
  // (0.51 in one suite, 0.61 / 0.67 in others) straddled a cap set for the
  // creature the report was about. The same correction laser-head needed.
  Target t = ChooseHairTarget(c, /*brainHeadOnly=*/true);
  if (!t.valid()) {
    detail = "no loaded or pool mob def has a hair (bloodless) limb with a body";
    return Status::Fail;
  }
  // The hair on the HEAD, not the mane on the torso.
  int headLi = -1;
  {
    const MobDef& def = mobs.Defs()[t.defIndex];
    int best = -1;
    uint32_t bestN = 0;
    mobs.Reset();
    const uint64_t probe = mobs.Spawn(t.defIndex, FixtureSite(c.world, 170));
    for (size_t li = 0; li < def.limbs.size(); li++) {
      if (!def.limbs[li].bloodless) continue;
      int pi = -1;
      for (size_t k = 0; k < def.limbs.size(); k++)
        if (def.limbs[k].name == def.limbs[li].parent) pi = (int)k;
      if (pi < 0 || !def.limbs[pi].vital) continue;
      const uint32_t n = probe ? mobs.LimbVoxelsAtSpawn(probe, (int)li) : 0;
      if (best < 0 || n > bestN) { best = (int)li; bestN = n; headLi = pi; }
    }
    mobs.Reset();
    if (best < 0) {
      detail = t.defName + " has no hair piece on a vital limb";
      return Status::Fail;
    }
    t.limb = best;
    t.limbName = def.limbs[best].name;
  }
  IVec3 pchunk{};
  const uint64_t id = SpawnTarget(c, t, 170, pchunk);
  if (!id) { detail = "spawn refused"; return Status::Fail; }
  uint32_t tick = 63999;
  support::TickCursor ticker{c, tick, pchunk};
  for (int i = 0; i < 3; i++) ticker();

  float hairRef[4] = {0, 0, 0, 1};
  mobs.LimbJointGap(id, t.limb, false, hairRef);
  struct Worst {
    float hairArt = 0, hairJolt = 0, neckArt = 0, neckJolt = 0;
    float headSplit = 0, hairSplit = 0, hairTurn = 0;
    int hairAt = -1;  // step (beam tick, then settle ticks after) of hairArt
  } alive, dead;
  int step = 0;
  auto sample = [&](Worst& w) {
    float rel[4];
    const float ha = mobs.LimbJointGap(id, t.limb, false, rel);
    if (ha > w.hairArt) w.hairAt = step;
    w.hairArt = std::max(w.hairArt, ha);
    w.hairJolt = std::max(w.hairJolt, mobs.LimbJointGap(id, t.limb, true));
    w.neckArt = std::max(w.neckArt, mobs.LimbJointGap(id, headLi, false));
    w.neckJolt = std::max(w.neckJolt, mobs.LimbJointGap(id, headLi, true));
    w.headSplit = std::max(w.headSplit, mobs.LimbArtColliderGap(id, headLi));
    w.hairSplit = std::max(w.hairSplit, mobs.LimbArtColliderGap(id, t.limb));
    if (ha >= 0.0f) {
      const float d = std::fabs(rel[0] * hairRef[0] + rel[1] * hairRef[1] +
                                rel[2] * hairRef[2] + rel[3] * hairRef[3]);
      w.hairTurn = std::max(w.hairTurn,
                            2.0f * std::acos(std::min(1.0f, d)));
    }
  };
  const float bore = (float)CurrentTuning().tools.laserCarveRadius;
  const int kMaxBeam = 240, kPostDeath = 45, kSettle = 90;
  int beamTicks = 0, diedAt = -1, bored = 0;
  std::vector<ParticleSpawn> spawns;
  for (int k = 0; k < kMaxBeam; k++) {
    if (diedAt >= 0 && k - diedAt > kPostDeath) break;
    const uint64_t hb = mobs.LimbBody(id, headLi);
    if (!hb) break;
    const uint32_t n = (k & 1) ? (uint32_t)k * 7919u : 0u;
    const Vec3 at = mobs.LimbVoxelPos(id, headLi, n);
    float b = 0.0f;
    if (mobs.LaserHit(hb, at, b) &&
        mobs.CarveLimbRadial(hb, at, b > 0.0f ? b : bore, /*ragged=*/false,
                             /*eject=*/false, c.world, spawns,
                             DamageCtx(DamageCause::Beam)))
      bored++;
    spawns.clear();
    ticker();
    beamTicks++;
    const bool isAlive = mobs.IsAlive(id);
    if (!isAlive && diedAt < 0) diedAt = k;
    step = k;
    sample(isAlive ? alive : dead);
  }
  for (int i = 0; i < kSettle; i++) {
    ticker();
    step = beamTicks + i;
    sample(dead);
  }
  const float hairEnd = mobs.LimbJointGap(id, t.limb, false);
  const float neckEnd = mobs.LimbJointGap(id, headLi, false);
  const bool headOn = mobs.LimbBody(id, headLi) != 0;
  const bool hairOn = mobs.LimbBody(id, t.limb) != 0;
  const std::string headName = mobs.Defs()[t.defIndex].limbs[headLi].name;
  mobs.Reset();
  c.debris.Reset();

  const float hairMax = (float)BaselineNumber("corpseHairGapMax", 0.05);
  const float neckMax = (float)BaselineNumber("corpseNeckGapMax", 0.6);
  const float turnMax = (float)BaselineNumber("corpseHairTurnMax", 0.02);
  const bool ok = diedAt >= 0 && headOn && hairOn &&
                  dead.hairArt <= hairMax && dead.hairTurn <= turnMax &&
                  dead.neckArt <= neckMax;
  detail = Format(
      "%s %s on %s: %d beam ticks, %d bored, died at %d, head %s hair %s | "
      "alive: hair gap art %.3f jolt %.3f, neck art %.3f jolt %.3f, split "
      "head %.3f hair %.3f, hair turn %.3f | dead: hair gap art %.3f jolt "
      "%.3f, neck art %.3f jolt %.3f, split head %.3f hair %.3f, hair turn "
      "%.3f at step %d | end hair %.3f neck %.3f (max hair %.3f neck %.3f "
      "turn %.3f)",
      t.defName.c_str(), t.limbName.c_str(), headName.c_str(), beamTicks,
      bored, diedAt, headOn ? "on" : "OFF", hairOn ? "on" : "OFF",
      alive.hairArt, alive.hairJolt, alive.neckArt, alive.neckJolt,
      alive.headSplit, alive.hairSplit, alive.hairTurn, dead.hairArt,
      dead.hairJolt, dead.neckArt, dead.neckJolt, dead.headSplit,
      dead.hairSplit, dead.hairTurn, dead.hairAt, hairEnd, neckEnd, hairMax,
      neckMax, turnMax);
  std::printf("corpse-head-laser: %s (%s)\n", ok ? "PASS" : "FAIL",
              detail.c_str());
  return ok ? Status::Pass : Status::Fail;
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
  // THE REAL TICK (W2-O, test/tickrig.h). The blood counted is the MOB
  // PHASE's own (TickRig::MobPhase): gore.bleedOpsPerTick is the creature
  // system's shared drip budget, so that is the span it bounds.
  support::TickCursor bleedTicker{c, t0, pchunk};
  for (int i = 0; i < kWatch; i++) {
    bleedTicker();
    const OpBatch& tb = bleedTicker.Rig().LastBatch();
    const auto& mp = bleedTicker.Rig().MobPhase();
    uint32_t tickOps = 0;
    for (size_t k = mp.ops0; k < mp.ops1 && k < tb.ops.size(); k++)
      if (tb.ops[k].material == bleedMat) tickOps++;
    for (size_t k = mp.spawns0; k < mp.spawns1 && k < tb.spawns.size(); k++)
      if ((tb.spawns[k].payload & 0xFFFu) == bleedMat) bloodDrops++;
    if (tickOps || !mobs.BleedSources().empty()) lastBleedTick = i;
    bloodOps += tickOps;
    worstTickOps = std::max(worstTickOps, tickOps);
  }

  const int stopBy = (int)BaselineNumber("woundBleedStopTicks", 400);
  const bool bled = bloodOps > 0 || bloodDrops > 0;
  const bool bounded = worstTickOps <= (uint32_t)opCap;
  const bool stopped = lastBleedTick >= 0 && lastBleedTick < stopBy;

  // ...AND THE WORLD SLEEPS AFTERWARDS. Blood is real matter the CA carries,
  // so a wound that keeps a chunk awake is rule 2 broken however tidy the
  // budget looked. Settled by ticking on with nothing left to emit.
  // On the real tick too: the creature is still standing there, and whatever
  // it goes on doing to the world (staining, dripping) is part of the claim.
  for (int i = 0; i < 400; i++) bleedTicker();
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
    double otherBlood = 0;        // blood other authors put in the same ticks
  };
  auto run = [&](bool stumpOpen, int inset, int window) {
    Run r;
    Tuning tt = saved;
    tt.gore.stumpBleedsOpen = stumpOpen;
    SetCurrentTuning(tt);
    IVec3 pchunk{};
    const uint64_t id = SpawnTarget(c, t, inset, pchunk);
    if (!id) { SetCurrentTuning(saved); return r; }
    // PINNED (the authored `dummy` profile, mobile: false), found by W2-O: the
    // claim is about a body standing where it was put, and an unprofiled one
    // only stood still under the hand-rolled ticks because they never
    // submitted a world, so it had no ground. On the real tick it walked: the
    // one-legged body dragged its stump and tracked its own blood.
    mobs.SetMobBehavior(id, "dummy");
    r.hp0 = mobs.TotalHp(id);
    mobs.Sever(id, t.limb);
    // The thrown sever voxels are charged INSIDE Sever(); the baseline is
    // taken after it so the identity below is over what the ticks emit.
    r.hpBefore = mobs.TotalHp(id);
    r.hpAtLast = r.hpBefore;
    uint32_t tick = 29999;
    // THE REAL TICK (W2-O, test/tickrig.h). The identity is about what the
    // CREATURE emitted — Mob::DrainBlood's "every drop is hp" — so what is
    // counted is the mob phase's own span of the batch (TickRig::MobPhase).
    // The rest of the tick's blood is other authors (reported in the detail
    // line). Counted over the WHOLE batch on the first W2-O run the identity
    // read 1483.16 vox owed against 877.07 hp lost: 21 voxels the creature was
    // never charged for, because it never emitted them.
    support::TickCursor ticker{c, tick, pchunk};
    for (int i = 0; i < window; i++) {
      ticker();
      const OpBatch& tb = ticker.Rig().LastBatch();
      const auto& mp = ticker.Rig().MobPhase();
      uint32_t tickOps = 0;
      double emitted = 0;
      for (size_t k = 0; k < tb.ops.size(); k++) {
        const BrushOp& op = tb.ops[k];
        if (op.material != bleedMat) continue;
        const double v = (double)BleedClumpVoxels(op.radius);
        if (k >= mp.ops0 && k < mp.ops1) {
          tickOps++;
          emitted += v;
        } else {
          r.otherBlood += v;
        }
      }
      for (size_t k = 0; k < tb.spawns.size(); k++) {
        const ParticleSpawn& s = tb.spawns[k];
        if ((s.payload & 0xFFFu) != bleedMat || !(s.flags & kPFlagMicro)) continue;
        if (k >= mp.spawns0 && k < mp.spawns1) emitted += dropletVox;
        else r.otherBlood += dropletVox;
      }
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
      "monotone=%d; stump shut: alive=%d, dry at tick %d of %d | blood from "
      "other authors in the same ticks (not the creature's): %.2f vox",
      t.defName.c_str(), t.limbName.c_str(), open.hp0, open.hpBefore,
      open.countedAtLast, owedHp, lostHp, tol, open.deathTick, expected,
      open.worstTickOps, opCap, open.monotone ? 1 : 0,
      shut.deathTick < 0 ? 1 : 0, shut.lastBleedTick, shut.ticks,
      open.otherBlood);
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
  // DIRECT PHASE CALLS ON PURPOSE (W2-O): arm B is the burn pass ALONE on a
  // directly ignited limb (the note above the gate: "A and B are CPU only").
  // Arm C, the world fire, runs the real tick.
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
  // THE REAL TICK (W2-O, test/tickrig.h); the bonfire is the gate's own cell
  // ops, pushed at the top of each tick.
  support::TickCursor fireTicker{c, simTick, pchunk};
  // How long the bonfire burns: a ceiling, not a target -- the loop ends at
  // the death. 1,200 was the burn rate's own margin on 2026-09-02 and had
  // shrunk to nothing by 10-01 (died at 1184 and 1152 in two runs, survived
  // to 1200 at 66% in a third), so the window is a baseline row.
  const int fireTicks = (int)BaselineNumber("burnCapFireTicks", 1200);
  if (mFire && rootLimb >= 0) {
    for (int i = 0; i < fireTicks; i++) {
      support::TickOps pre;
      std::vector<CellOp>& cellOps = pre.cells;
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
        // ...DOWN TO THE GROUND under each column, not to a fixed 8 below
        // the hips' first voxel: that sat at the ankles, so the feet and shins
        // were never in the fire (raw skin foot 573/610, legL 706/1068 after
        // 1,200 ticks) and the burnt fraction levelled off just under the
        // death knot -- 69.9% at tick 1184 in one suite, 66.0% in the next,
        // the same fixture passing or failing on the ground it stood on.
        for (int dz = -6; dz <= 6; dz++)
          for (int dx = -6; dx <= 6; dx++) {
            const int th =
                World::TerrainHeight(b.x + dx, b.z + dz, kDefaultSeed);
            for (int y = std::min(b.y - 8, th); y <= b.y + 20; y++) {
              const IVec3 cc{b.x + dx, y, b.z + dz};
              if (!c.world.CellInWindow(cc)) continue;
              if (cellOps.size() >= kMaxCellOpsPerTick) break;
              const bool aboveGround = cc.y > th;
              cellOps.push_back(
                  {World::SlotCellIndex(cc),
                   PackVoxNew(mFire, 7u) | (aboveGround ? 0u : kCellOpIfAir)});
            }
          }
      }
      fireTicker(pre);
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
    for (int li : hits) {
      const uint64_t body = mobs.LimbBody(id, li);
      if (!body) continue;
      const LimbAxis ax = MeasureLimb(mobs, id, li);
      const Vec3 at = ax.anchor + ax.along * (ax.reach * 0.5f);
      if (!mobs.Damage(body, dmg, at, tipSpeed, DamageCtx(DamageCause::Blade, 1.0f))) continue;
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

  // Then the wound bleeds for five seconds of game time, on THE REAL TICK
  // (W2-O, test/tickrig.h).
  uint32_t tick = 49999;
  int deathTick = -1;
  support::TickCursor ticker{c, tick, pchunk};
  for (int i = 0; i < 150 && mobs.IsAlive(id); i++) {
    ticker();
    if (!mobs.IsAlive(id)) {
      deathTick = i;
      cause = mobs.DeathCause(id);
      break;
    }
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
// the stroke, and a probe that finds a corpse limb carves it — and a carve
// REBUILDS the limb's collider, and Physics::RemoveBody destroys every joint
// on the body it removes. One nick to the torso took the neck, both shoulders
// and both hips off in the same call (the corpse was debris then; it is a dead
// Mob now, docs/PLAN_corpse_is_a_mob.md, and the carve is Mob::CarveLimb's).
//
// The property: the joints a corpse keeps survive the corpse being carved.
// Kill the fixture the way the sword does (root limb to zero, which is a
// death, not an amputation), melt its torso where the swing crosses it for
// three ticks, let it settle. The torso body must have been REBUILT (or the
// gate proves nothing), the joint count must not have moved by one, and every
// body the creature was made of must still have a joint on it.
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
  // before it dies, while it still stands in its rest shape.
  const LimbAxis ax = MeasureLimb(mobs, id, root);
  // WHERE ON THE ROOT, and why not its middle any more. The claim is that a
  // REBUILT root keeps every joint, not that a hip survives losing its socket:
  // since c7a5109 (2026-10-01) every socket is measured (snapped to the
  // parent's nearest voxel) and the joint rule detaches a child whose socket
  // flesh is carved away (Mob::JointAttached) -- a corpse's leg falls off a
  // hip cut through, by design. The 1-voxel melt at the human hips' middle
  // takes ~2/3 of them (3200 -> ~990 skin voxels) and with it the left hip
  // socket, so legU.L dropped and the rig counted 15 joints of 16. The carve
  // now lands on the sampled root voxel FARTHEST from every child socket,
  // which still rebuilds the root (the subject) without cutting a socket out.
  Vec3 mid = ax.anchor + ax.along * (ax.reach * 0.5f);
  {
    std::vector<Vec3> sockets;
    for (int li = 0; li < nLimbs; li++) {
      Vec3 s{};
      if (li != root && def.limbs[li].parent == def.limbs[root].name &&
          mobs.LimbSocketWorld(id, li, s))
        sockets.push_back(s);
    }
    const uint32_t n = mobs.LimbVoxelCount(id, root);
    float best = -1.0f;
    for (uint32_t k = 0; k < n && !sockets.empty();
         k += std::max(1u, n / 512u)) {
      const Vec3 p = mobs.LimbVoxelPos(id, root, k);
      float dmin = 1e30f;
      for (const Vec3& s : sockets) dmin = std::min(dmin, (p - s).len());
      if (dmin > best) {
        best = dmin;
        mid = p;
      }
    }
  }

  // The killing blow. The root limb at zero hp is a death, not an amputation
  // (Mob::HpZeroSevers), and the corpse keeps every joint (Mob::Die).
  {
    mobs.Damage(torso, 1.0e6f, mid, 45.0f, DamageCtx(DamageCause::Blade, 1.0f));
  }
  if (mobs.IsAlive(id)) {
    detail = Format("%s: root limb at zero hp did not kill", t.defName.c_str());
    mobs.Reset();
    c.debris.Reset();
    return Status::Fail;
  }
  const std::string cause = mobs.DeathCause(id);
  const uint32_t jointsDead = c.phys.JointCount();
  // THE CORPSE IS THE DEAD MOB: its torso is still limb `root`, the same
  // body it was a tick ago.
  if (mobs.LimbBody(id, root) != torso) {
    detail = Format("%s: the torso did not stay the corpse's own limb on death",
                    t.defName.c_str());
    mobs.Reset();
    c.debris.Reset();
    return Status::Fail;
  }
  const uint32_t vox0 = mobs.LimbArtVoxelCount(id, root);

  // THE REAL TICK (W2-O, test/tickrig.h): this gate exists because the game
  // kept going past where the old fixture stopped.
  uint32_t tick = 51999;
  support::TickCursor ticker{c, tick, pchunk};
  auto step = [&]() { ticker(); };
  // The rest of the stroke: three ticks of the edge crossing the torso at the
  // sword's own kerf width (halfWidth + carveBonus is about a voxel) — the
  // laser's sphere carve, which is what the melt on dead flesh always was.
  const float radius = 1.0f;
  std::vector<ParticleSpawn> spawns;
  int melts = 0;
  uint64_t cur = torso;
  for (int i = 0; i < 3 && cur; i++) {
    if (mobs.CarveLimbRadial(cur, mid, radius, /*ragged=*/false,
                             /*eject=*/false, c.world, spawns))
      melts++;
    spawns.clear();
    cur = mobs.LimbBody(id, root);  // a rebuild changes the handle
    step();
    cur = mobs.LimbBody(id, root);
  }
  const uint32_t jointsCarved = c.phys.JointCount();
  // WHICH joint a carve lost (CLAUDE.md rule 6: "31 -> 30" names nothing):
  // every limb's own joint count against the rig's, with its socket hold.
  std::string jointLoss;
  if (jointsCarved != jointsDead)
    for (int li = 0; li < nLimbs; li++) {
        const uint64_t h = mobs.LimbBody(id, li);
        float sock = 0.0f, neck = 0.0f;
        mobs.LimbJointHold(id, li, sock, neck);
        if (li != root && h && def.limbs[li].parent == def.limbs[root].name)
          jointLoss += Format(" %s(j%u sock %.2f neck %.2f)",
                              def.limbs[li].name.c_str(),
                              c.phys.JointCount(h), (double)sock,
                              (double)neck);
      }
  const uint32_t vox1 = cur ? mobs.LimbArtVoxelCount(id, root) : 0;
  const bool rebuilt = cur != torso && cur != 0;

  // Then it settles for two seconds.
  for (int i = 0; i < 60; i++) step();
  const uint32_t jointsSettled = c.phys.JointCount();
  int jointed = 0, bodies = 0;
  for (int li = 0; li < nLimbs; li++) {
    const uint64_t h = mobs.LimbBody(id, li);
    if (!h) continue;
    bodies++;
    if (c.phys.JointCount(h) > 0) jointed++;
  }
  // ...and anything that came off it as loose debris is a body of it too,
  // with no joint: a limb that parted would show up here.
  for (uint32_t i = 0; i < c.debris.BodyCount(); i++) {
    bodies++;
    if (c.phys.JointCount(c.debris.BodyHandle(i)) > 0) jointed++;
  }
  // How far the corpse spread: the largest distance from the torso to any
  // other limb of it. Recorded, not asserted — a settling ragdoll spreads
  // legitimately, and the joint account above is the claim.
  float spread = 0.0f;
  if (mobs.LimbBody(id, root)) {
    const Vec3 tp = mobs.LimbPosition(id, root);
    for (int li = 0; li < nLimbs; li++)
      if (mobs.LimbBody(id, li))
        spread = std::max(spread, (mobs.LimbPosition(id, li) - tp).len());
  }
  RecordObserved("corpseSpreadVox", (double)spread);

  const bool jointsKept = jointsAlive > 0 && jointsDead == jointsAlive &&
                          jointsCarved == jointsAlive &&
                          jointsSettled == jointsAlive;
  const bool ok = melts > 0 && rebuilt && vox1 < vox0 && jointsKept &&
                  jointed == attached && bodies == attached;
  mobs.Reset();
  c.debris.Reset();
  detail = Format(
      "%s: died of '%s' with %d/%d limbs on, %u joints; torso melted %d ticks "
      "(%u -> %u voxels, rebuilt=%d): joints %u after the kill, %u after the "
      "carve, %u after settling; %d of %d corpse bodies still jointed, spread "
      "%.1f vox",
      t.defName.c_str(), cause.c_str(), attached, nLimbs, jointsAlive, melts,
      vox0, vox1, rebuilt ? 1 : 0, jointsDead, jointsCarved, jointsSettled,
      jointed, bodies, spread);
  if (!jointLoss.empty()) detail += " | root's children after the carve:" + jointLoss;
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
// A carve on a fine-skinned body works on TWO lattices: the SKIN (the art, and
// what the player sees) and the COLLIDER derived from it by a majority
// downsample. A blade's kerf is a sliver about a tenth of a world voxel across,
// and one collider cell of a human is eight skin cells that only flip when half
// of them go — so a carve that decided "nothing in range" on the collider took
// nothing, bled nothing and left no gore, while the corpse still shoved around
// exactly as before. `Mob::CarveLimb` has refused to make that decision on the
// collider since the fine skin existed, and a corpse is a dead Mob now, so the
// corpse's cut IS that carve (docs/PLAN_corpse_is_a_mob.md).
//
// So the claim is the smallest one that the failure breaks: ONE sword-shaped
// kerf against a corpse removes voxels from the authoritative lattice, and the
// next two keep removing them (the entry snap must find the new surface, or
// the wound saturates and a corpse becomes uncuttable after one blow). The
// mace arm is the control: a blunt carve is a radius of voxels rather than a
// slot, it moves the collider by itself — so a run where the blade takes
// nothing and the mace takes plenty is exactly the signature of the bug, and
// the two numbers are printed side by side.
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
  // standing, where the limb's axis is the authored one.
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
  // the corpse keeps every limb and joint (Mob::Die).
  {
    mobs.Damage(torso, 1.0e6f, at, 45.0f, DamageCtx(DamageCause::Blade, 1.0f));
  }
  if (mobs.IsAlive(id)) {
    detail = Format("%s: root limb at zero hp did not kill", t.defName.c_str());
    mobs.Reset();
    c.debris.Reset();
    return Status::Fail;
  }
  if (mobs.LimbBody(id, t.limb) != limbBody) {
    detail = Format("%s: the limb did not stay the corpse's own on death",
                    t.defName.c_str());
    mobs.Reset();
    c.debris.Reset();
    return Status::Fail;
  }

  // THE SAME SIX LINES MELEE BUILDS THE KERF FROM (game/melee.cpp
  // BuildStrikeParts), so a gore retune moves this gate and the game together.
  const auto& g = CurrentTuning().gore;
  const float kPower = 1.0f, kHeft = 1.0f, kRadius = 0.9f;
  std::vector<ParticleSpawn> spawns;
  const uint32_t vox0 = mobs.LimbArtVoxelCount(id, t.limb);
  uint32_t voxAfter[3] = {vox0, vox0, vox0};
  int cuts = 0;
  for (int i = 0; i < 3; i++) {
    // THE LIMB INDEX IS THE IDENTITY, NOT THE HANDLE. A carve that moved the
    // collider rebuilds it, and a rebuild REPLACES the Jolt handle; the rig
    // slot is untouched.
    const uint64_t cur = mobs.LimbBody(id, t.limb);
    if (!cur) break;   // parted: nothing of it left on the corpse to cut
    KerfCut cut;
    cut.at = at;
    cut.edgeAxis = ax.edge;
    cut.cutDir = ax.travel;
    cut.halfWidth = std::max(kRadius * g.cutWidth, g.cutWidthMin);
    cut.depth = (g.cutDepth + g.cutDepthPower * kPower) * kHeft;
    cut.length = g.cutLength * (0.4f + 0.6f * kPower) * kHeft;
    cut.power = kPower;
    cut.seed = 0x50D5u + (uint32_t)i * 40503u;
    if (mobs.CutLimb(cur, cut, c.world, spawns)) cuts++;
    spawns.clear();
    voxAfter[i] = mobs.LimbArtVoxelCount(id, t.limb);
  }
  const uint32_t bladeTook = vox0 > voxAfter[2] ? vox0 - voxAfter[2] : 0u;
  const bool firstCut = voxAfter[0] < vox0;
  const bool deepens = voxAfter[2] < voxAfter[0];
  // The wound the cut armed: a corpse that is cut must BLEED from where it was
  // cut, which is the second half of the same report ("no blood comes out").
  const bool bleeds = mobs.LimbWoundOpen(id, t.limb);

  // ---- THE CONTROL ARM: a mace-sized crater, on the torso of the same corpse
  // A blunt carve is a radius, not a slot, so it moves the collider on its own
  // and was never affected. If the blade takes nothing and this takes plenty,
  // the lattice the blade carves is the thing that is broken.
  uint32_t maceTook = 0;
  if (const uint64_t tb = mobs.LimbBody(id, root)) {
    const uint32_t before = mobs.LimbArtVoxelCount(id, root);
    // On a surviving voxel, so the crater is guaranteed to land on matter.
    const Vec3 tAt = mobs.LimbVoxelPos(id, root, 0);
    mobs.CarveLimbRadial(tb, tAt, std::max(g.bluntCarveRadius, 0.5f),
                         /*ragged=*/true, /*eject=*/true, c.world, spawns);
    spawns.clear();
    const uint32_t after = mobs.LimbArtVoxelCount(id, root);
    maceTook = before > after ? before - after : 0u;
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
// next few seconds (Mob::BluntPulpTick). A corpse is a dead Mob now and takes
// exactly that path; what it lacks is hp to lose and a voice.
//
// The claim, in the order the rungs fire: sustained mace blows on one spot of
// a corpse (1) lay a bruise coat that was not there before, (2) arm the
// dissolution, and (3) go on taking voxels AFTER the blows stop, without being
// touched again. A first blow on clean flesh must take nothing: the carve is
// EARNED by pulp, exactly as it is on the living.
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
    mobs.Damage(torso, 1.0e6f, at, 45.0f, DamageCtx(DamageCause::Blade, 1.0f));
  }
  if (mobs.IsAlive(id)) {
    detail = Format("%s: root limb at zero hp did not kill", t.defName.c_str());
    mobs.Reset();
    c.debris.Reset();
    return Status::Fail;
  }
  if (mobs.LimbBody(id, t.limb) != limbBody) {
    detail = Format("%s: the limb did not stay the corpse's own on death",
                    t.defName.c_str());
    mobs.Reset();
    c.debris.Reset();
    return Status::Fail;
  }

  // A MACE, resolved exactly as melee resolves it on a creature (the corpse is
  // one): the bruise first, the dissolution only as far as the pulp earns it.
  //
  // LIVE voxels, tombstones excluded: the pulp tick eats by TOMBSTONING
  // (Mob::BluntPulpTick -> FlushBurn), and below FlushBurn's batch threshold a
  // tombstone stays in the lattice, so the lattice's SIZE did not move when a
  // crater opened (impact-blunt's LiveVoxels, the same count).
  auto live = [&](uint64_t mid, int li) -> uint32_t {
    const uint32_t art = mobs.LimbArtVoxelCount(mid, li);
    const uint32_t dead = mobs.LimbMaterialCount(mid, li, 0u);
    return art > dead ? art - dead : 0u;
  };
  const uint32_t vox0 = live(id, t.limb);
  const uint32_t coat0 = mobs.LimbBruiseCount(id, t.limb, 1);
  std::vector<ParticleSpawn> spawns;
  const float kHp = 16.0f;
  uint32_t voxAfterFirst = vox0;
  uint32_t coatAfterFirst = coat0;
  const int kBlows = 24;
  for (int i = 0; i < kBlows; i++) {
    const uint64_t h = mobs.LimbBody(id, t.limb);
    if (!h) break;
    ::BluntHit hit;
    hit.at = at;
    hit.hp = kHp;
    hit.power = 1.0f;
    hit.carve = 0.6f;
    hit.armorBreak = 0.0f;
    hit.impactSpeed = 0.0f;
    hit.seed = 0xB1u + (uint32_t)i * 977u;
    hit.unarmed = false;
    mobs.BluntHit(h, hit, c.world, spawns);
    spawns.clear();
    if (i == 0) {
      voxAfterFirst = live(id, t.limb);
      coatAfterFirst = mobs.LimbBruiseCount(id, t.limb, 1);
    }
  }
  const uint32_t vox1 = live(id, t.limb);
  const uint32_t coat1 = mobs.LimbBruiseCount(id, t.limb, 1);
  const bool pulping = mobs.LimbPulping(id, t.limb);

  // ...AND IT KEEPS GOING WITH NOBODY TOUCHING IT. The blows have stopped;
  // only the corpse's own tick (Mob::BluntPulpTick, in the dead burn pot)
  // runs from here. The limb is addressed by its rig slot, which neither a
  // collider rebuild nor anything settling around it can renumber.
  //
  // THE RATE IS AN ARM, NOT THE CLAIM -- impact-blunt's (tests/baseline.json
  // _impactPulp_about). The debris version of this gate saw voxels leave in 45
  // ticks only because its fixture CARVED during the blows (BluntBody on
  // `earned`); a dead Mob takes the living ladder, where nothing leaves in the
  // swing and the shipped 1.5 vox/min is a minute of sim per crater. So the
  // window runs at the same cranked impactPulpRot the living gates use.
  const Tuning savedTune = CurrentTuning();
  {
    Tuning tt = savedTune;
    tt.gore.pulpRotRate = (float)BaselineNumber("impactPulpRot", 30.0);
    SetCurrentTuning(tt);
  }
  // THE REAL TICK (W2-O, test/tickrig.h): "nobody touching it" is the whole
  // tick running around the corpse, not two of its phases.
  uint32_t tick = 60999;
  support::TickCursor ticker{c, tick, pchunk};
  for (int i = 0; i < 45; i++) ticker();
  SetCurrentTuning(savedTune);
  const uint32_t vox2 = live(id, t.limb);

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
      "%s limb '%s' as a corpse, %d mace blows (%.0f hp) on one spot: bruised "
      "%u -> %u (first blow %u, took %u voxels), lattice %u -> %u, then "
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
// TWO THINGS A CORPSE COULD NOT DO. It could not be TAKEN APART — a blade could
// part a neck completely and the head stayed attached. And it could not be
// HEARD: main.cpp picks the cue for a blow by differencing the sever and voice
// queues, and dead flesh filled neither, so hacking a body apart made the noise
// a crate makes.
//
// A corpse is a dead Mob now (docs/PLAN_corpse_is_a_mob.md), so both are the
// living's own machinery: CarveLimb's joint rule takes the head off where the
// neck has been cut through, and Sever reports it on the sever queue with its
// cause. The claim: sustained blade cuts at a corpse's NECK part the joint
// within a sane number of blows, both ends bleed, and the blow that did it
// pushes a sever event that names the creature and says an edge did it.
//
// THE BOUND MATTERS AS MUCH AS THE EVENT. Unbounded, this passes on a model
// that needs four hundred blows, which is the same as not working.
// `corpseDismemberMaxCuts` is the ceiling and `corpse-intact` is the other
// side of the bet: a cut in the MIDDLE of a limb must not part anything.
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
  // Killed the way the sword kills. The rig stays the corpse's (Mob::Die).
  {
    mobs.Damage(torso, 1.0e6f, mobs.LimbAnchorPos(id, head), 45.0f, DamageCtx(DamageCause::Blade, 1.0f));
  }
  if (mobs.IsAlive(id)) {
    detail = Format("%s: root limb at zero hp did not kill", t.defName.c_str());
    mobs.Reset();
    c.debris.Reset();
    return Status::Fail;
  }
  mobs.ClearSeverEvents();
  c.debris.ClearGoreEvents();
  if (mobs.LimbBody(id, head) != headBody0) {
    detail = Format("%s: the head did not stay the corpse's own on death",
                    t.defName.c_str());
    mobs.Reset();
    c.debris.Reset();
    return Status::Fail;
  }
  // WHERE THE NECK JOINT IS, asked of physics: the joint knows its own anchor
  // (Physics::JointsOn), and the corpse has fallen over since it was posed.
  std::vector<Physics::BodyJoint> js;
  c.phys.JointsOn(headBody0, js);
  uint64_t neckJoint = 0;
  Vec3 anchorW{};
  for (const Physics::BodyJoint& j : js)
    if (j.other == neckBody0) {
      neckJoint = j.joint;
      // THROUGH THE BODY'S OWN ROTATION. The anchor is body-local and a corpse
      // has just fallen over; adding it to the position alone puts the blow
      // somewhere off in the air.
      BodyTransform hx{};
      c.phys.GetTransform(headBody0, hx);
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
  const uint32_t vox0 = mobs.LimbArtVoxelCount(id, head);
  // THE NUMBER THAT ACTUALLY DECIDES: how much flesh is within the hold radius
  // of the joint. Reported at both ends, so a gate that fails says WHY instead
  // of saying the head stayed on.
  const float hold = CurrentTuning().gore.corpseJointHold;
  const uint32_t held0 = mobs.LimbVoxelsNearWorld(id, head, anchorW, hold);
  int cuts = 0;
  bool parted = false;
  uint64_t lastHead = headBody0;   // the handle the head had at the last cut
  for (int i = 0; i < kCap && !parted; i++) {
    const uint64_t hNow = mobs.LimbBody(id, head);
    if (!hNow) break;
    lastHead = hNow;
    // ---- A CHOP LANDS ON MATTER, NOT ON A COORDINATE --------------------
    //
    // The blade meets the body at its SURFACE nearest the joint and travels
    // INTO the joint from there. Aiming at the anchor itself puts the blow in
    // the air (the anchor sits at the boundary between two bodies, which is
    // exactly where neither one's lattice is), and the kerf's entry snap only
    // reaches about a voxel. Re-derived every blow, so a second chop lands in
    // the groove the first one opened.
    const Vec3 surf = mobs.LimbNearestVoxelWorld(id, head, anchorW);
    Vec3 into = anchorW - surf;
    if (into.len() < 1e-3f) into = Vec3{0, -1, 0};
    into = into.normalized();
    KerfCut cut;
    cut.at = surf;
    Vec3 edge = into.cross(Vec3{0, 1, 0});
    if (edge.len() < 0.15f) edge = into.cross(Vec3{1, 0, 0});
    cut.edgeAxis = edge.normalized();
    cut.cutDir = into;
    cut.halfWidth = std::max(kRadius * g.cutWidth, g.cutWidthMin);
    cut.depth = (g.cutDepth + g.cutDepthPower * kPower) * kHeft;
    cut.length = g.cutLength * (0.4f + 0.6f * kPower) * kHeft;
    cut.power = kPower;
    cut.seed = 0xBEEFu + (uint32_t)i * 40503u;
    {
      // A CutLimb is DamageCause::Blade, which is what marks a sever
      // `byBlade`, the cause the dismember cue switches on.
      mobs.CutLimb(hNow, cut, c.world, spawns, kPower);
    }
    spawns.clear();
    cuts++;
    // The head is off when it is no longer one of the corpse's limbs. (The
    // joint's own id is no test: a carve that rebuilds the head's collider
    // re-makes its joint under a new id while the head is still on.)
    parted = mobs.LimbBody(id, head) == 0;
  }
  // Where the head went: a severed piece is loose dead flesh (DebrisSystem).
  auto indexOf = [&](uint64_t h) -> int {
    for (uint32_t i = 0; i < c.debris.BodyCount(); i++)
      if (c.debris.BodyHandle(i) == h) return (int)i;
    return -1;
  };
  uint64_t headNow = mobs.LimbBody(id, head);
  int hidx = -1;
  // The severed head keeps the handle it had at the cut (DetachLimb hands the
  // same body to DebrisSystem); the nearest dead flesh to the joint is the
  // fallback for a piece a carve rebuilt on its way out.
  if (!headNow) hidx = indexOf(lastHead);
  if (!headNow && hidx < 0) {
    float best = 1e30f;
    for (uint32_t i = 0; i < c.debris.BodyCount(); i++) {
      const uint64_t h = c.debris.BodyHandle(i);
      if (!c.debris.BodyIsDeadFlesh(h)) continue;
      const float d = (c.debris.BodyPosition(i) - anchorW).len();
      if (d < best) { best = d; hidx = (int)i; }
    }
  }
  if (!headNow && hidx >= 0) headNow = c.debris.BodyHandle((uint32_t)hidx);
  const uint32_t vox1 = hidx >= 0 ? c.debris.BodyVoxelCount((uint32_t)hidx)
                                  : mobs.LimbArtVoxelCount(id, head);
  const uint32_t held1 =
      hidx >= 0 ? c.debris.VoxelsNearWorld(headNow, anchorW, hold)
                : mobs.LimbVoxelsNearWorld(id, head, anchorW, hold);
  const uint32_t jointsAfter = c.phys.JointCount();

  // ---- WHAT THE ROOM HEARD -------------------------------------------------
  // The sever queue (MobSystem::SeverEvents) for the corpse, and the debris
  // gore queue for anything its pieces reported on their own.
  int goreEvents = 0, severEvents = 0, bladeEvents = 0, named = 0;
  for (const MobSystem::SeverEvent& se : mobs.SeverEvents()) {
    if (se.mobId != id) continue;
    goreEvents++;
    severEvents++;
    if (se.byBlade) bladeEvents++;
    if (se.defIndex >= 0 && se.defIndex < (int)mobs.Defs().size()) named++;
  }
  for (const DebrisSystem::GoreEvent& ge : c.debris.GoreEvents()) {
    goreEvents++;
    if (ge.severed) severEvents++;
    if (ge.severed && ge.byBlade) bladeEvents++;
    if (ge.defIndex >= 0 && ge.defIndex < (int)mobs.Defs().size()) named++;
  }
  const bool headBleeds = hidx >= 0 && c.debris.BodyWoundOpen((uint32_t)hidx);
  const bool neckBleeds = mobs.LimbWoundOpen(id, neck);

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
    bool deadRig = false;  // ...and that mob is a corpse (not a creature)
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
    // DIRECT PHASE CALLS ON PURPOSE (W2-O): fixture posing. Each arm replays
    // ticks 3000.. so the three bodies meet the blow in the same pose and the
    // arms differ only by the drive state under test.
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
        if (torso) mobs.Damage(torso, 1.0e6f, mid, 45.0f, DamageCtx(DamageCause::Blade, 1.0f));
      }
      // Die() keeps the rig (docs/PLAN_corpse_is_a_mob.md) and flips it
      // dynamic without changing a handle, so the one measured above is still
      // the corpse's limb.
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
    arms[a].deadRig = arms[a].owned && !mobs.IsAlive(id);
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
  // still has something to say. A corpse's limb is a DEAD Mob's (it keeps its
  // rig, docs/PLAN_corpse_is_a_mob.md): it has no hp to lose and says nothing.
  // Asserting both halves is what keeps the unification honest — it would
  // otherwise be one short step from "the dead and the merely knocked-down are
  // the same thing", which they are not.
  const bool livingHurt = arms[0].hpDrop > 0.0f && arms[1].hpDrop > 0.0f &&
                          arms[0].owned && arms[1].owned;
  const bool deadIsNot = arms[2].deadRig && arms[2].hpDrop <= 0.0f &&
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
      "owned=%d hp-%.1f voice=%d | dead owned-by-corpse=%d hp-%.1f voice=%d "
      "(floor %.2f vox/s, impulse %.0f kg*m/s)",
      t.defName.c_str(), t.limbName.c_str(), (double)mace.blunt,
      arms[0].dynamic ? 1 : 0, (double)arms[0].dv, (double)arms[0].dw,
      arms[1].dynamic ? 1 : 0, (double)arms[1].dv, (double)arms[1].dw,
      arms[2].dynamic ? 1 : 0, (double)arms[2].dv, (double)arms[2].dw,
      arms[0].owned ? 1 : 0, (double)arms[0].hpDrop, arms[0].voiced ? 1 : 0,
      arms[1].owned ? 1 : 0, (double)arms[1].hpDrop, arms[1].voiced ? 1 : 0,
      arms[2].deadRig ? 1 : 0, (double)arms[2].hpDrop, arms[2].voiced ? 1 : 0,
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
  // STILL HAND-ROLLED (W2-O), ON PURPOSE: the claim is the cost of the
  // PHYSICS STEP ALONE with the proxy pinned between the submit and the step,
  // and the real tick cannot time one of its phases from outside.
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
  // ...and WHICH shell each handle is, for the strap attribution below.
  std::unordered_map<uint64_t, std::string> shellName;
  for (int sIdx = bareLimbs; sIdx < mob->LimbCount(); sIdx++)
    if (const uint64_t h = mobs.LimbBody(id, sIdx)) {
      shellBodies.insert(h);
      shellName[h] = mob->LimbDefAt(sIdx).name;
    }

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
    if (const uint64_t h = mobs.LimbBody(id, sIdx)) {
      shellBodies.insert(h);
      shellName[h] = mob->LimbDefAt(sIdx).name;
    }
  std::unordered_set<uint64_t> mine = shellBodies;
  for (int li = 0; li < mob->LimbCount(); li++)
    if (const uint64_t h = mobs.LimbBody(id, li)) mine.insert(h);

  // Then the kill, the way the sword delivers it: the root limb at zero hp is
  // a death, not an amputation (Mob::HpZeroSevers), and the corpse keeps every
  // joint (Mob::Die).
  if (mobs.IsAlive(id)) {
    const LimbAxis ax = MeasureLimb(mobs, id, root);
    mobs.Damage(torso, 1.0e6f, ax.anchor + ax.along * (ax.reach * 0.5f), 45.0f, DamageCtx(DamageCause::Blade, 1.0f));
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
  // THE CORPSE'S PIECES, EVERY TICK: the dead Mob's own limbs (a corpse keeps
  // its rig, docs/PLAN_corpse_is_a_mob.md) by rig slot, and the loose pieces
  // that came off it before it died (the severed limb and its shells) as the
  // debris bodies they are. `follower` is a slot or body whose pose is its
  // host's (a worn shell on the corpse, a strapped shell on a loose piece).
  struct PieceNow {
    uint64_t h;
    Vec3 p;
    bool follower;
  };
  auto piecesNow = [&]() {
    std::vector<PieceNow> out;
    const Mob* dead = mobs.FindMobById(id);
    for (int li = 0; dead && li < dead->LimbCount(); li++)
      if (const uint64_t h = mobs.LimbBody(id, li))
        out.push_back({h, mobs.LimbPosition(id, li), dead->WornHostOf(li) >= 0});
    for (uint32_t b = 0; b < c.debris.BodyCount(); b++) {
      const uint64_t h = c.debris.BodyHandle(b);
      if (!mine.count(h)) continue;   // not this corpse: see above
      out.push_back({h, c.debris.BodyPosition(b), c.debris.WornHostOf(h) != 0});
    }
    return out;
  };
  for (int i = 0; i < kTicks; i++) {
    double ms = 0.0;
    step(&ms);
    if (ms > worstStepMs) { worstStepMs = ms; worstStepTick = i; }
    Vec3 anchor{};
    bool haveAnchor = false;
    for (const PieceNow& pn : piecesNow()) {
      const uint64_t h = pn.h;
      const Vec3 p = pn.p;
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
      if (!pn.follower && c.phys.GetBodyVelocities(h, lin, ang)) {
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
  const uint32_t ours = (uint32_t)piecesNow().size();

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
  // A shell still on the corpse is a FOLLOWER of its limb (MobLimb::wornHost,
  // driven by DriveWornShells exactly as in life); one that left on the
  // severed limb is strapped to that piece (DebrisSystem::StrapBody). Both are
  // "strapped", and neither may carry a joint.
  uint32_t shellsLeft = 0, shellJoints = 0, shellsStrapped = 0;
  if (const Mob* dead = mobs.FindMobById(id)) {
    for (int sIdx = bareLimbs; sIdx < dead->LimbCount(); sIdx++) {
      const uint64_t h = mobs.LimbBody(id, sIdx);
      if (!h || dead->LimbDefAt(sIdx).tag != "worn") continue;
      shellsLeft++;
      shellJoints += c.phys.JointCount(h);
      if (dead->WornHostOf(sIdx) >= 0) shellsStrapped++;
    }
  }
  // A RAG IS NOT A ROPE. Mob::ShedGearBeforeDetach: when a piece's IDENTITY
  // shell leaves (here, the left gauntlet's, on the severed hand) the item is
  // shed and its other shells "fall as rags" -- the right gauntlet comes off
  // the right hand as loose debris, which is the behaviour armor-wear asserts
  // ("every shell of the robe has left the body"). Such a shell has no host by
  // design, so it is counted apart: still in `mine` (speed, spin and spread
  // bound it like every other piece), still held to zero joints, and NOT a
  // strap failure. Recognised by its item no longer being worn by the corpse.
  // Seen once the hand actually came off -- 9 strokes from 2026-10-01; in the
  // suite before that the gauntlet held for all 120 and this path never ran.
  auto itemOf = [](const std::string& shell) {
    // "worn:<item>:<limb>"
    const size_t a = shell.find(':');
    const size_t b = a == std::string::npos ? a : shell.find(':', a + 1);
    return (a == std::string::npos || b == std::string::npos)
               ? std::string()
               : shell.substr(a + 1, b - a - 1);
  };
  auto stillWorn = [&](const std::string& item) {
    const Mob* dead = mobs.FindMobById(id);
    if (!dead || item.empty()) return false;
    for (int p = 0; p < dead->WornPieceCount(); p++)
      for (int sl : dead->WornSlotsAt(p))
        if (itemOf(dead->LimbDefAt(sl).name) == item) return true;
    return false;
  };
  uint32_t shellRags = 0;
  std::string unstrapped;
  for (uint32_t b = 0; b < c.debris.BodyCount(); b++) {
    const uint64_t h = c.debris.BodyHandle(b);
    if (!shellBodies.count(h)) continue;
    shellsLeft++;
    shellJoints += c.phys.JointCount(h);
    if (c.debris.WornHostOf(h)) {
      shellsStrapped++;
      continue;
    }
    const std::string nm = shellName.count(h) ? shellName[h] : std::string("?");
    const bool rag = !stillWorn(itemOf(nm));
    if (rag) shellRags++;
    unstrapped += Format(" %s(debris, %u vox, %s)", nm.c_str(),
                         c.debris.BodyVoxelCount(b),
                         rag ? "a shed piece's rag" : "ROPED");
  }
  // A strapped shell must ALSO still be on its corpse: a follower whose host
  // was culled unstraps and falls, and one that never moved would pass the
  // joint claim while hanging in the air. `maxSpread` above is what bounds
  // that, over every piece including these.
  const bool strapsHold =
      shellsStrapped == 0 ||
      (shellJoints == 0 && shellsStrapped + shellRags == shellsLeft);
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
    {
      const std::vector<PieceNow> ps = piecesNow();
      if (!ps.empty()) at = ps.front().p;
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
      "its %u bodies left (%u debris bodies were in the world before it); "
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
  if (!unstrapped.empty()) detail += " | unstrapped shells:" + unstrapped;
  if (!unstrapped.empty())
    std::printf("corpse-armor: unstrapped shells:%s\n", unstrapped.c_str());
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
// The head is loose dead flesh (DebrisSystem) once it is off; the corpse it
// came off is a dead Mob that keeps its rig (docs/PLAN_corpse_is_a_mob.md),
// so the neck's wound is the neck LIMB's, drained by the corpse's own
// BleedTick with the living's pump switched off.
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
  // The neck stays ON the corpse, as its own limb: present = still attached.
  auto neckOn = [&]() { return mobs.LimbBody(id, neck) != 0; };
  const int hi0 = indexOf(headBody);
  const bool ni0 = neckOn();
  // "No joint left" means no joint back to the CORPSE. Since the head grew a
  // jointed hair limb (8b73865, 2026-09-24; rooted hair d34c3ba/f268330) the
  // severed head legitimately keeps the joint to its own hair, which leaves
  // with it -- so a joint whose far end is still one of the corpse's limbs is
  // what fails, not any joint at all (was JointCount(headBody) == 0).
  uint32_t headJointsToCorpse = 0;
  {
    std::vector<Physics::BodyJoint> hj;
    c.phys.JointsOn(headBody, hj);
    for (const Physics::BodyJoint& j : hj)
      for (size_t li = 0; li < def.limbs.size(); li++)
        if (j.other != 0 && mobs.LimbBody(id, (int)li) == j.other)
          headJointsToCorpse++;
  }
  const bool headOff = hi0 >= 0 && headJointsToCorpse == 0 &&
                       mobs.LimbBody(id, head) == 0;
  const bool bothWounded = hi0 >= 0 && ni0 &&
                           c.debris.BodyWoundOpen((uint32_t)hi0) &&
                           mobs.LimbWoundOpen(id, neck);
  const float headBudget0 = hi0 >= 0 ? c.debris.BodyWoundBudget((uint32_t)hi0) : 0.0f;
  const float neckBudget0 = ni0 ? mobs.LimbBleedBudget(id, neck) : 0.0f;
  auto woundedNow = [&]() {
    return c.debris.WoundedBodyCount() + mobs.WoundedLimbCount(id);
  };
  const uint32_t wounded0 = woundedNow();

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
  // THE REAL TICK (W2-O, test/tickrig.h). The wound points are read at the
  // top of each tick and the tick's blood drops sorted between them after it.
  support::TickCursor ticker{c, tick, pchunk};
  for (int i = 0; i < window; i++) {
    // The corpse's own tick drains the NECK (Mob::BleedTick on a dead Mob);
    // the debris tick drains the severed head. Both streams are counted.
    const int hi = indexOf(headBody);
    const bool ni = neckOn();
    const Vec3 hw = hi >= 0 ? c.debris.BodyWoundWorld((uint32_t)hi) : Vec3{};
    const Vec3 nw = ni ? mobs.LimbWoundWorld(id, neck) : Vec3{};
    if (ni) {
      neckLast = mobs.LimbPosition(id, neck);
      if (i == 0) neckFirst = neckLast;
      neckBudgetAtGone = mobs.LimbBleedBudget(id, neck);
    } else if (neckGoneTick < 0) {
      neckGoneTick = i;
    }
    if (hi < 0 && headGoneTick < 0) headGoneTick = i;
    ticker();
    const std::vector<ParticleSpawn>& sp = ticker.Rig().LastBatch().spawns;
    for (size_t k = 0; k < sp.size(); k++) {
      const ParticleSpawn& p = sp[k];
      if ((p.payload & 0xFFFu) != (bleedMat & 0xFFFu)) continue;
      const Vec3 at{(float)p.px / 256.0f, (float)p.py / 256.0f,
                    (float)p.pz / 256.0f};
      const float dh = hi >= 0 ? (at - hw).len() : 1e9f;
      const float dn = ni ? (at - nw).len() : 1e9f;
      if (dh < dn) bloodHead += 1.0; else bloodNeck += 1.0;
    }
    // (A REAL TICK was already needed here because the corpse needs ground:
    // debris terrain meshes are built from the chunk cache the tick's readback
    // fills, and without one the ragdoll fell 225 voxels out of the window.)
    if (closedTick < 0 && woundedNow() == 0) {
      closedTick = i;
      // ---- D. a cut on the corpse opens a wound again ----------------------
      // The moment every wound has paid out, at the neck's own wound point (a
      // point ON the limb), the laser's sphere carve — the melt dead flesh has
      // always taken. A carve rebuilds the collider, which replaces the
      // handle; the rig slot is the identity, so the wound is read back by
      // limb index.
      if (!neckOn()) {
        cutSkipped = true;
      } else {
        neckBody = mobs.LimbBody(id, neck);
        const Vec3 at = mobs.LimbWoundWorld(id, neck);
        melted = mobs.CarveLimbRadial(neckBody, at, 1.5f, /*ragged=*/false,
                                      /*eject=*/false, c.world, spawns);
        spawns.clear();
        reopened = melted && mobs.LimbWoundOpen(id, neck) &&
                   mobs.LimbBleedBudget(id, neck) > 0.0f;
        neckBody = mobs.LimbBody(id, neck);  // the rebuild replaced it
      }
    } else if (closedTick >= 0 && closedAgainTick < 0 && woundedNow() == 0) {
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
      "neck wound=%d (%.0f vox), %u wounded bodies/limbs; over %d ticks the head "
      "shed %.0f blood spawns and the neck %.0f, wounds closed at t+%d; corpse "
      "cut %s, closed again at t+%d; neck body gone at t+%d (budget %.0f left, "
      "moved %.1f vox, last y %.1f), head body gone at t+%d, %u bodies at end",
      t.defName.c_str(), t.limbName.c_str(), wantSoak ? 1 : 0, stainedExposed,
      rateExposed, stainedBuried, rateBuried, nonTissueStained, cause.c_str(),
      headOff ? 1 : 0, hi0 >= 0 ? 1 : 0,
      headBudget0, ni0 ? 1 : 0, neckBudget0, wounded0, window, bloodHead,
      bloodNeck, closedTick,
      cutSkipped ? "skipped (the neck had left the corpse)"
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
  // THE REAL TICK (W2-O, test/tickrig.h). The pyre is the gate's own cell ops
  // (pushed at the top of the tick, so they are the batch's FIRST entries);
  // everything after them is what the tick's systems authored.
  support::TickCursor ticker{c, tick, pchunk};
  auto tickOnce = [&](bool soak) {
    support::TickOps pre;
    std::vector<CellOp>& cellOps = pre.cells;
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
    const size_t own = cellOps.size();
    ticker(pre);
    // BOTH passes' ops, since 2026-09-22: a corpse's flesh burns in the living
    // limb pass (MobSystem::BurnDeadFlesh, inside mobs.PreTick) and only its
    // garments in the debris one. The counters are zeroed after the death
    // (phase B below), when the corpse is the only thing left emitting.
    const std::vector<CellOp>& tc = ticker.Rig().LastBatch().cells;
    for (size_t k = own; k < tc.size(); k++) {
      const uint32_t m = tc[k].word & 0xFFFu;
      if (m == mFire) debrisFireOps++;
      else if (m == mSmoke || m == mAsh) debrisResidueOps++;
    }
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
  // THE CORPSE IS THE DEAD MOB (docs/PLAN_corpse_is_a_mob.md): its limbs are
  // still its own and burn in the living limb pass, and anything the fire took
  // off it while it lived (a limb burnt through) is loose dead flesh in
  // DebrisSystem. A PIECE is either: a rig slot of the corpse (stable identity:
  // the slot index), or a debris body (tracked by handle, as before).
  const Mob* deadMob = mobs.FindMobById(id);
  const int nSlots = deadMob ? deadMob->LimbCount() : 0;
  auto sumMats = [&](bool limb, int li, uint32_t bi, const uint32_t* mats,
                     size_t n) {
    uint32_t s = 0;
    for (size_t k = 0; k < n; k++) {
      if (!mats[k]) continue;
      s += limb ? mobs.LimbMaterialCount(id, li, mats[k])
                : c.debris.BodyMaterialCount(bi, mats[k]);
    }
    return s;
  };
  auto limbAlight = [&](int li) { return sumMats(true, li, 0, alight, 3); };
  auto limbSpent = [&](int li) { return sumMats(true, li, 0, spent, 4); };
  auto alightOf = [&](uint32_t bi) { return sumMats(false, 0, bi, alight, 3); };
  auto spentOf = [&](uint32_t bi) { return sumMats(false, 0, bi, spent, 4); };
  auto limbOn = [&](int li) { return mobs.LimbBody(id, li) != 0; };
  // Every body of the corpse, counted the same way on both sides.
  auto bodyCount = [&]() {
    uint32_t n = c.debris.BodyCount();
    for (int li = 0; li < nSlots; li++) n += limbOn(li) ? 1u : 0u;
    return n;
  };
  auto lowestY = [&]() {
    float lo = 1e9f;
    for (uint32_t bi = 0; bi < c.debris.BodyCount(); bi++)
      lo = std::min(lo, c.debris.BodyPosition(bi).y);
    for (int li = 0; li < nSlots; li++)
      if (limbOn(li)) lo = std::min(lo, mobs.LimbPosition(id, li).y);
    return lo;
  };
  // Loose pieces are tracked by HANDLE. A burn rebuild replaces the handle
  // (ReplaceBody) and a piece burnt below body-worthiness is swap-removed, so
  // neither an index nor a handle is stable across the window; a piece whose
  // handle is no longer present is matched by position among the unclaimed
  // bodies, and one that matches nothing has finished burning (gone). A rig
  // slot needs none of that: its index IS its identity.
  struct Piece {
    bool limb = false;
    int li = -1;
    uint64_t handle = 0;
    uint32_t alight0 = 0, spent0 = 0, voxels0 = 0;
    Vec3 pos{};
    uint32_t alight1 = 0, spent1 = 0;
    bool matched = false, gone = false;
  };
  std::vector<Piece> pieces;
  uint32_t alight0 = 0, spent0 = 0;
  for (int li = 0; li < nSlots; li++) {
    if (!limbOn(li)) continue;
    Piece p;
    p.limb = true;
    p.li = li;
    p.alight0 = limbAlight(li);
    p.spent0 = limbSpent(li);
    p.voxels0 = mobs.LimbArtVoxelCount(id, li);
    p.pos = mobs.LimbPosition(id, li);
    alight0 += p.alight0;
    spent0 += p.spent0;
    pieces.push_back(p);
  }
  for (uint32_t bi = 0; bi < c.debris.BodyCount(); bi++) {
    Piece p;
    p.handle = c.debris.BodyHandle(bi);
    p.alight0 = alightOf(bi);
    p.spent0 = spentOf(bi);
    p.voxels0 = c.debris.BodyVoxelCount(bi);
    p.pos = c.debris.BodyPosition(bi);
    alight0 += p.alight0;
    spent0 += p.spent0;
    pieces.push_back(p);
  }
  const uint32_t bodies0 = bodyCount();

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
    uint32_t nMean = 0;
    for (uint32_t bi = 0; bi < c.debris.BodyCount(); bi++, nMean++)
      mean += c.debris.BodyPosition(bi);
    for (int li = 0; li < nSlots; li++)
      if (limbOn(li)) {
        mean += mobs.LimbPosition(id, li);
        nMean++;
      }
    if (nMean) mean = mean * (1.0f / (float)nMean);
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
      uint32_t built = 0, unfetched = 0, empty = 0;
      c.debris.TerrainCensus(built, unfetched, empty);
      fall += Format(" t+%d:y%.1f/%ub%uu%ue", i, lowestY(), built, unfetched,
                     empty);
    }
    tickOnce(false);
    if (bodyCount() > 0) {
      lastBodyTick = i;
      lastY = lowestY();
    }
    if (i == window / 2) bodiesMid = bodyCount();
  }
  const uint32_t settled = c.debris.SettledBack() - settled0;

  std::vector<int> claimed(c.debris.BodyCount(), 0);
  auto claim = [&](Piece& p, uint32_t bi) {
    claimed[bi] = 1;
    p.matched = true;
    p.alight1 = alightOf(bi);
    p.spent1 = spentOf(bi);
  };
  for (Piece& p : pieces) {
    if (!p.limb) continue;
    // A limb that burnt through and left mid-window is matched as a loose
    // piece below, by where it lay.
    if (!limbOn(p.li)) continue;
    p.matched = true;
    p.alight1 = limbAlight(p.li);
    p.spent1 = limbSpent(p.li);
  }
  for (Piece& p : pieces) {
    if (p.matched || p.limb) continue;
    for (uint32_t bi = 0; bi < c.debris.BodyCount(); bi++)
      if (!claimed[bi] && c.debris.BodyHandle(bi) == p.handle) {
        claim(p, bi);
        break;
      }
  }
  // Handle gone (or a limb that left the rig): the piece was rebuilt (new
  // handle), cut loose, or burnt away. Match the nearest unclaimed body to
  // where the piece lay; a still corpse does not travel, so anything farther
  // than a body length is not it.
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
  for (int li = 0; li < nSlots; li++)
    if (limbOn(li)) {
      alight1 += limbAlight(li);
      spent1 += limbSpent(li);
    }
  // Every piece that died with a real ember count must have MOVED: fewer
  // alight or more spent. Not "retired half of it" -- a corpse with fire on
  // it burns THROUGH, cooked flesh under the char catching from the embers
  // beside it, so the alight count on a piece can hold or climb for hundreds
  // of ticks while the char under it grows. What must never happen is a piece
  // whose numbers do not change at all, which is the starved body the report
  // describes. 8 is the floor: a piece with three embers on it can lose them
  // all to the dice or keep them all, and neither says anything.
  std::string stalled;
  uint32_t stalledCount = 0, litPieces = 0;
  for (const Piece& p : pieces) {
    if (p.alight0 < 8) continue;
    litPieces++;
    if (p.gone) continue;
    if (p.alight1 * 2 > p.alight0 && p.spent1 <= p.spent0) {
      stalledCount++;
      if (stalled.size() < 200)
        stalled += Format(" [%s %u vox: %u->%u alight, %u->%u spent]",
                          p.limb ? "limb" : "piece", p.voxels0, p.alight0,
                          p.alight1, p.spent0, p.spent1);
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
  // to a third body that is still very much alive.
  uint32_t bricks = 0, disagree = 0, sharedBricks = 0;
  std::string disagreeDetail;
  std::unordered_map<uint32_t, uint32_t> holdersOf;
  if (const MicroBodySet* set = c.debris.MicroSet()) {
    auto checkBrick = [&](uint32_t model, uint32_t latAlight, uint32_t latSpent) {
      if (model == kMicroBodyNoModel || model >= set->models.size()) return;
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
      if (brickAlight != latAlight || brickSpent != latSpent) {
        disagree++;
        if (disagreeDetail.size() < 200)
          disagreeDetail +=
              Format(" [brick %u/%u vs lattice %u/%u alight/spent]",
                     brickAlight, brickSpent, latAlight, latSpent);
      }
    };
    for (uint32_t bi = 0; bi < c.debris.BodyCount(); bi++)
      checkBrick(c.debris.BodyMicroModel(bi), alightOf(bi), spentOf(bi));
    for (int li = 0; li < nSlots; li++) {
      if (!limbOn(li)) continue;
      const int32_t mm = mobs.LimbMicroModel(id, li);
      if (mm < 0) continue;
      checkBrick((uint32_t)mm, limbAlight(li), limbSpent(li));
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
  const uint32_t bodies1 = bodyCount();
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
  // A WORLD-ONLY tick on purpose (W2-O): fixture construction, before any
  // creature exists -- walls and pools going in through the queue.
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
  // THE REAL TICK (W2-O, test/tickrig.h); the liquid is the gate's own cell
  // ops, written at the top of the tick, and the mirror follows the creature.
  uint32_t simTick = 29000;
  support::TickCursor liquidTicker{c, simTick, pchunk};
  auto liquidTick = [&](const std::function<void(std::vector<CellOp>&)>& fill) {
    support::TickOps pre;
    fill(pre.cells);
    IVec3 centre = pchunk;
    if (root >= 0 && mobs.LimbBody(id, root)) {
      const Vec3 at = mobs.LimbVoxelPos(id, root, 0);
      centre = IVec3{ifloor(at.x) >> 4, ifloor(at.y) >> 4, ifloor(at.z) >> 4};
    }
    liquidTicker.chunk = centre;
    liquidTicker(pre);
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
    // THE REAL TICK (W2-O), on the burst's own clock: the replay is keyed to
    // the tick after `ev.tick`, so the gate's clock is moved there first.
    simTick = 29900u;
    for (int i = 0; i < 2; i++) liquidTick([](std::vector<CellOp>&) {});
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
    simTick = 30000u;   // the burst's own clock (see the unreachable arm)
    for (int i = 0; i < 6; i++) {
      liquidTick([](std::vector<CellOp>&) {});
      // The chip above left a bleeding wound; its drip spray must queue
      // bursts of its own (counted before the age-out at the next PreTick).
      queuedByWound = std::max(queuedByWound, mobs.SplatterEventsQueued());
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
  // What the river is judged on is the BLOOD left, not any coat: water now
  // leaves what it rinsed WET (bodystain.h WashBodyStain), so "stained" would
  // count the wash itself. Read off the forced ledger (the coat ledger of a
  // dead Mob is the same one), not any coat census.
  auto bloodVoxels = [&]() -> uint32_t {
    if (!mobs.LimbBody(id, root)) return 0u;
    mobs.RecountCoatOn(id, simTick);
    const LimbCoat* lc = mobs.LimbCoatOf(id, root);
    uint32_t n = 0;
    if (lc)
      for (const CoatEntry& en : lc->top)
        if (en.mat == mBlood) n += en.voxels;
    return n;
  };
  if (root >= 0 && mBlood && mWater && mobs.LimbBody(id, root)) {
    pooled = soakPhase(mBlood, 8u, 30);
    soakPhase(mWater, 8u, 60);
    washed = bloodVoxels();
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
// Nothing ran the contact pass on a corpse at all — Mob::StainTick walked a
// creature's limbs, and from the tick it died those limbs were DebrisSystem
// bodies, so a corpse's coat froze at the moment of death. A corpse is a dead
// Mob now (docs/PLAN_corpse_is_a_mob.md) and gets the full StainTick — the
// living's own contact, rain, drying and wet passes — from its own budget pot.
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
  // joint kept, so the torso stays the corpse's own root limb.
  const int root = def.rootLimb;
  {
    const LimbAxis ax = MeasureLimb(mobs, id, root);
    mobs.Damage(torso, 1.0e6f, ax.anchor, 0.0f);
  }
  // The torso by RIG SLOT: its identity survives every collider rebuild.
  auto torsoOn = [&]() { return mobs.LimbBody(id, root) != 0; };
  auto torsoCentre = [&]() {
    Vec3 at{};
    const Mob* m = mobs.FindMobById(id);
    if (!m || !m->LimbCentreWorld(root, at)) at = mobs.LimbVoxelPos(id, root, 0);
    return at;
  };
  if (mobs.IsAlive(id) || mobs.LimbBody(id, root) != torso) {
    detail = Format("%s: the torso did not stay the corpse's own on death",
                    t.defName.c_str());
    mobs.Reset();
    c.debris.Reset();
    return Status::Fail;
  }
  const uint32_t coat0 = mobs.LimbCoatMatCount(id, root, mBlood, 1);

  // One real tick with liquid written round the torso (body-stain's
  // liquidTick, with the mirror centred on the corpse rather than a limb).
  // THE REAL TICK (W2-O, test/tickrig.h); the liquid is the gate's own cell
  // ops at the top of each tick, the mirror centred on the corpse.
  uint32_t simTick = 33000;
  support::TickCursor ticker{c, simTick, pchunk};
  auto soak = [&](uint32_t mat, int ticks) -> uint32_t {
    for (int i = 0; i < ticks; i++) {
      support::TickOps pre;
      std::vector<CellOp>& cellOps = pre.cells;
      IVec3 centre = pchunk;
      if (torsoOn()) {
        const Vec3 at = torsoCentre();
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
      ticker.chunk = centre;
      ticker(pre);
    }
    return torsoOn() ? mobs.LimbCoatMatCount(id, root, mBlood, 1) : 0u;
  };
  const uint32_t pooled = soak(mBlood, 30);
  const uint32_t washed = soak(mWater, 60);
  const bool present = torsoOn();

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
      present ? "" : "; the torso left the corpse mid-test");
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// corpse-crossheat / corpse-worn / corpse-splatter: three things the living
// had and the dead did not (owner report 2026-09-22)
// ---------------------------------------------------------------------------
//
// From the tick a creature died its limbs WERE DebrisSystem bodies, and three
// mechanisms that lived only in the creature's own passes stopped (a corpse is
// a dead Mob now, docs/PLAN_corpse_is_a_mob.md, and runs those passes itself):
//   * HEAT ACROSS A JOINT (Mob::BuildCrossLimbHeat) -- a burning torso on the
//     ground did not light the thighs jointed to it.
//   * ARMOUR -- a corpse in plate burnt and dissolved as if naked, because the
//     corpse burn pass had no worn-occlusion probe.
//   Both because a corpse burned in DebrisSystem::BurnBodies, a fork of the
//   living pass. It now burns in the living pass itself (MobSystem::
//   BurnDeadFlesh -> BurnOneLimb), with the cross-heat builder and the shell
//   march shared with the living rather than copied.
//   * SPLATTER -- a burst of blood was replayed against living limbs only.
//     MobSystem::SplatterDeadFlesh replays it against every dead-flesh body
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
  // THE REAL TICK (W2-O, test/tickrig.h); `fill` writes the gate's own cell
  // ops at the top of it.
  std::unique_ptr<support::TickRig> rig;
  void Step(const std::function<void(std::vector<CellOp>&)>& fill = nullptr) {
    if (!rig) rig = std::make_unique<support::TickRig>(c, tick, pchunk);
    rig->tick = tick;
    rig->SetFixtureChunk(pchunk);
    if (fill) {
      support::RunTicks(*rig, 1, [&](uint32_t, support::TickOps& o) {
        fill(o.cells);
      });
    } else {
      support::RunTicks(*rig, 1);
    }
    tick = rig->tick;
  }
  // THE TORSO IS THE CORPSE'S ROOT LIMB, addressed by rig slot: a burn or a
  // carve rebuilds its collider under a new handle, and the slot is the one
  // identity that survives it. -1 once it has left the corpse.
  int root = -1;
  int TorsoIndex() {
    if (root < 0) return -1;
    torso = c.mobs.LimbBody(id, root);
    return torso ? root : -1;
  }
  // Voxels the torso still HAS, tombstones excluded: a corpse is a dead Mob,
  // whose burn/acid/rot removals sit in the lattice as material 0 until
  // FlushBurn's batch threshold fills (selftest_impact.cpp LiveVoxels' note),
  // so the raw lattice size reads "nothing eaten" for a bite too small to
  // flush. DebrisSystem compacted at once, which is why this never mattered.
  uint32_t TorsoVoxels() {
    if (TorsoIndex() < 0) return 0u;
    const uint32_t art = c.mobs.LimbArtVoxelCount(id, root);
    const uint32_t dead = c.mobs.LimbMaterialCount(id, root, 0u);
    return art > dead ? art - dead : 0u;
  }
  uint32_t TorsoCoat(uint32_t mat) {
    return TorsoIndex() >= 0 ? c.mobs.LimbCoatMatCount(id, root, mat, 1) : 0u;
  }
  // The middle of the torso's box through its live pose (a limb's transform
  // origin is its lattice CORNER, which is no place to aim a splash).
  Vec3 TorsoCentre() {
    Vec3 at{};
    const Mob* m = c.mobs.FindMobById(id);
    if (!m || !m->LimbCentreWorld(root, at)) at = c.mobs.LimbVoxelPos(id, root, 0);
    return at;
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
    root = def.rootLimb;
    torso = mobs.LimbBody(id, def.rootLimb);
    const LimbAxis ax = MeasureLimb(mobs, id, def.rootLimb);
    mobs.Damage(torso, 1.0e6f, ax.anchor, 0.0f);
    if (mobs.IsAlive(id) || mobs.LimbBody(id, root) != torso) {
      why = "the root at zero hp did not leave the torso on the corpse";
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
  // the burn chain. Excluding the torso (the source) is the whole point. The
  // corpse's own limbs (anatomy only: a garment is not flesh), and any loose
  // dead flesh that has come off it.
  auto othersTouched = [&](uint32_t* pieces) {
    uint32_t n = 0, lit = 0;
    const Mob* m = c.mobs.FindMobById(f.id);
    const int base = m ? m->AppendedBase() : 0;
    for (int li = 0; li < base; li++) {
      if (li == f.root || !c.mobs.LimbBody(f.id, li)) continue;
      uint32_t here = 0;
      for (uint32_t mat : touchedMats)
        if (mat) here += c.mobs.LimbMaterialCount(f.id, li, mat);
      n += here;
      if (here) lit++;
    }
    for (uint32_t i = 0; i < c.debris.BodyCount(); i++) {
      if (!c.debris.BodyIsDeadFlesh(c.debris.BodyHandle(i))) continue;
      uint32_t here = 0;
      for (uint32_t mat : touchedMats)
        if (mat) here += c.debris.BodyMaterialCount(i, mat);
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
        uint32_t lit = c.mobs.RewriteLimbMaterial(f.id, ti, mSkin, mBurning, 200);
        if (lit < 200 && mFlesh)
          lit += c.mobs.RewriteLimbMaterial(f.id, ti, mFlesh, mBurning, 200 - lit);
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
  const uint32_t bodies =
      c.debris.BodyCount() +
      (c.mobs.FindMobById(f.id) ? c.mobs.FindMobById(f.id)->LimbBodyCount() : 0u);
  c.mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  RecordObserved("corpseCrossheatTouched", (double)peak);
  const double minTouched = BaselineNumber("corpseCrossheatMinTouched", 100.0);
  // The spread, and proof the living pass is what burned the dead: it built a
  // cross-heat snapshot for the corpse (crossCells) — Mob::BurnTick's own
  // BuildCrossLimbHeat, run on the dead Mob.
  const bool ok = relit > 0 && (double)peak >= minTouched && st.crossCells > 0;
  detail = Format(
      "torso re-lit %u voxels, no world fire: other pieces touched by fire %u "
      "(need %.0f) across %u pieces of %u bodies; living burn pass on the dead: "
      "%u cross-heat cells built, %u faces read one, %u rules armed by it alone",
      relit, peak, minTouched, peakPieces, bodies, st.crossCells, st.crossFaces,
      st.crossOnly);
  return ok ? Status::Pass : Status::Fail;
}

// ---- garment-burn ----------------------------------------------------------
// A GARMENT THAT CATCHES BURNS AWAY, AND THE SKIN UNDER IT CATCHES (owner,
// 2026-09-25: "when you set someone on fire the clothes just become burnt; it
// should turn to burnt -> nothing if the burnt clothes are still on fire,
// which should destroy clothing and further light skin on fire").
//
// A living human in the stock linen smock, lit at a few dozen voxels of the
// garment and nothing else -- NO world fire, because the case that was broken
// is a person on fire, not a person standing in one (armor-react's standing
// bath already relit charred cloth from three hot faces; linen never burned
// away at all, its char was an inert terminus, and an inert char over the
// skin reads as cold cloth to the occlusion probe forever). Three claims:
//   A the garment is CONSUMED: at least garmentBurnMinGone of its voxels are
//     gone (air), not merely blackened;
//   B burnt goes on to NOTHING while it is still on fire: more of what burned
//     became air than is left hanging as char;
//   C the fire reaches the wearer: flesh under the garment is ALIGHT
//     (flesh_burning) at some tick -- lit, not only seared.
//   D ...AND REACHES THEM WHILE THEY ARE STILL DRESSED (owner, 2026-10-05:
//     "the entire clothes burns off before the body beneath it takes any
//     damage"): by the tick a tenth of the garment is gone the wearer has lost
//     at least garmentBurnSkinAtTenthMin raw skin. Measured on this fixture:
//     181 before the fix (burning cloth refused as a seed, read as one hot
//     face, and the heat through a hole charged crossLimbPct), ~540 after
//     standalone and 372 under --verify behind other gates (scope moves it).
// Burn death is parked (gore.burnDeathFraction 1) for the same reason
// mob-burn parks it: the claim is how far the fire gets, and a body that dies
// of the first 70% stops being counted. Regenerates the world on the way out.
//
// MEASURED BOTH WAYS on this fixture, same spark: on the rules before
// 2026-09-25 the fire crossed the whole smock (peak 5,980 alight) and left
// 11,214 voxels of char and 0 gone -- A and B FAIL, the owner's report. After:
// 86% gone, 833 char left. C held on the old rules too (flesh alight peak 311,
// burning cloth against the skin already lights it); the change roughly
// doubles it (599), because the holes let the fire at the skin -- C is kept as
// the "the wearer burns" floor, not as the discriminating claim.
Status GateGarmentBurn(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  MobSystem& mobs = c.mobs;
  const uint32_t mLinen = MatIdOf(c, "linen"),
                 mLinenBurn = MatIdOf(c, "linen_burning"),
                 mFleshBurning = MatIdOf(c, "flesh_burning");
  const uint32_t charMats[] = {MatIdOf(c, "cloth_charred"),
                               MatIdOf(c, "linen_charred")};
  if (!mLinen || !mLinenBurn || !mFleshBurning) {
    detail = "linen / linen_burning / flesh_burning missing from materials.json";
    return Status::Fail;
  }
  const ItemDef* smock = c.items.At(c.items.Find("smock"));
  const int hd = mobs.FindDef("human");
  if (smock == nullptr || hd < 0) {
    detail = "no `smock` item or `human` def to dress";
    return Status::Skip;
  }
  PrepareWorld(c);
  mobs.Reset();
  c.debris.Reset();
  const Tuning saved = CurrentTuning();
  {
    Tuning tt = saved;
    tt.gore.burnDeathFraction = 1.0f;
    SetCurrentTuning(tt);
  }
  // A DRY FLOOR. Worldgen at this inset is a lake: measured, a human stood
  // there for 8 ticks came out with a water coat on ~76% of every smock panel,
  // and burning cloth is doused by water at 450 per-mille -- the spark was out
  // in three ticks. So, like armor-react, the fixture stamps its own ground:
  // two layers of stone flush above the highest column under it and air above,
  // through the real tick, and stands the creature on that.
  const IVec3 site = FixtureSite(c.world, 460);
  int padTop = 0;
  std::vector<CellOp> pad;
  {
    const uint32_t mStone = MatIdOf(c, "stone");
    int hMax = 0;
    for (int x = site.x - 10; x <= site.x + 10; x++)
      for (int z = site.z - 10; z <= site.z + 10; z++)
        hMax = std::max(hMax, World::TerrainHeight(x, z, kDefaultSeed));
    padTop = hMax + 2;
    for (int x = site.x - 10; x <= site.x + 10; x++)
      for (int z = site.z - 10; z <= site.z + 10; z++)
        for (int y = padTop - 1; y <= padTop + 28; y++) {
          const IVec3 cc{x, y, z};
          if (c.world.CellInWindow(cc))
            pad.push_back({World::SlotCellIndex(cc),
                           PackVoxNew(y <= padTop ? mStone : 0u, 0u)});
        }
  }
  const IVec3 pchunk{site.x >> 4, padTop >> 4, site.z >> 4};
  support::TickRig rig(c, 70000u, pchunk);
  support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) { o.cells = pad; });
  const uint64_t id = mobs.Spawn(hd, {site.x, padTop + 1, site.z});
  Mob* mob = id ? mobs.FindMobById(id) : nullptr;
  int home = -1;
  for (int sl = 0; sl < kEquipSlotCount && home < 0; sl++)
    if (EquipSlotAccepts(sl, smock->kind)) home = sl;
  if (!mob || home < 0 || !mob->WearItem(smock, home)) {
    SetCurrentTuning(saved);
    detail = "could not spawn and dress the human in the smock";
    return Status::Fail;
  }
  mobs.SetMobBehavior(id, "dummy");
  // Every rig slot the smock occupies (a garment is several cover panels).
  std::vector<int> shell;
  for (int p = 0; p < mob->WornPieceCount(); p++)
    for (int sl : mob->WornSlotsAt(p)) shell.push_back(sl);
  const int base = mob->AppendedBase();
  support::RunTicks(rig, 8);

  auto live = [&](int sl) {
    const uint32_t art = mobs.LimbArtVoxelCount(id, sl);
    const uint32_t dead = mobs.LimbMaterialCount(id, sl, 0u);
    return mobs.LimbBody(id, sl) && art > dead ? art - dead : 0u;
  };
  auto shellVox = [&]() {
    uint32_t n = 0;
    for (int sl : shell) n += live(sl);
    return n;
  };
  auto shellCount = [&](uint32_t m) {
    uint32_t n = 0;
    for (int sl : shell)
      if (m && mobs.LimbBody(id, sl)) n += mobs.LimbMaterialCount(id, sl, m);
    return n;
  };
  auto shellChar = [&]() {
    uint32_t n = 0;
    for (uint32_t m : charMats) n += shellCount(m);
    return n;
  };
  auto bodyAlight = [&]() {
    uint32_t n = 0;
    for (int li = 0; li < base; li++)
      if (mobs.LimbBody(id, li))
        n += mobs.LimbMaterialCount(id, li, mFleshBurning);
    return n;
  };
  // Raw SKIN left on the body: what it has lost is the damage the wearer has
  // taken (seared, alight, charred or gone). Traced so "the whole garment went
  // before the body felt anything" (owner, 2026-10-05) reads off the line.
  const uint32_t mSkin = MatIdOf(c, "skin");
  auto bodySkin = [&]() {
    uint32_t n = 0;
    for (int li = 0; li < base && mSkin; li++)
      if (mobs.LimbBody(id, li)) n += mobs.LimbMaterialCount(id, li, mSkin);
    return n;
  };
  // THE FIXTURE MUST BE DRY, and says so if it is not: a wet garment is a
  // doused garment, and "it did not burn" would then be about the ground.
  uint32_t wet = 0;
  const uint32_t mWater = MatIdOf(c, "water");
  for (int sl : shell) wet += mobs.LimbCoatMatCount(id, sl, mWater, 1);
  const uint32_t vox0 = shellVox();
  const uint32_t alight0 = bodyAlight();
  const uint32_t skin0 = bodySkin();
  // THE SPARK: 48 voxels of the biggest panel (the body of the smock), once.
  int sparkSlot = -1;
  for (int sl : shell)
    if (sparkSlot < 0 || live(sl) > live(sparkSlot)) sparkSlot = sl;
  const uint32_t lit =
      sparkSlot >= 0 && wet * 100u <= vox0
          ? mobs.RewriteLimbMaterial(id, sparkSlot, mLinen, mLinenBurn, 48)
          : 0u;
  const int kTicks = 300;
  uint32_t voxEnd = vox0, charEnd = 0, peakAlight = 0, peakBurning = 0;
  int firstAlight = -1, diedAt = -1;
  // Skin lost by the tick a TENTH of the garment is gone (claim D); -1 = the
  // garment never got that far.
  int64_t skinAtTenth = -1;
  std::string trace;
  for (int i = 0; i < kTicks && lit > 0; i++) {
    support::RunTicks(rig, 1);
    if (!mobs.FindMobById(id)) break;
    if (diedAt < 0 && !mobs.IsAlive(id)) diedAt = i;
    voxEnd = shellVox();
    charEnd = shellChar();
    const uint32_t a = bodyAlight();
    const uint32_t burning = shellCount(mLinenBurn) + shellCount(MatIdOf(c, "cloth_burning"));
    peakBurning = std::max(peakBurning, burning);
    if (a > alight0 && firstAlight < 0) firstAlight = i;
    if (skinAtTenth < 0 && (vox0 - std::min(vox0, voxEnd)) * 10u >= vox0) {
      const uint32_t sk = bodySkin();
      skinAtTenth = skin0 > sk ? skin0 - sk : 0;
    }
    peakAlight = std::max(peakAlight, a);
    // The garment's course every 25 ticks: alive / alight / char / skin
    // lost / flesh lit.
    if (i % 25 == 0) {
      const uint32_t sk = bodySkin();
      trace += Format(" t+%d %u/%u/%u/%u/%u", i, voxEnd, burning, charEnd,
                      skin0 > sk ? skin0 - sk : 0u, a);
    }
  }
  std::printf("  garment-burn trace (voxels / alight / charred / skin lost / flesh alight):%s\n",
              trace.c_str());
  SetCurrentTuning(saved);
  mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  const uint32_t gone = vox0 > voxEnd ? vox0 - voxEnd : 0u;
  const double goneFrac = vox0 ? (double)gone / (double)vox0 : 0.0;
  RecordObserved("garmentBurnGoneFrac", goneFrac);
  RecordObserved("garmentBurnPeakAlight", (double)peakAlight);
  const double minGone = BaselineNumber("garmentBurnMinGone", 0.5);
  const bool a = lit > 0 && goneFrac >= minGone;
  const bool b = gone > charEnd;
  const bool cc = peakAlight > alight0;
  RecordObserved("garmentBurnSkinAtTenth", (double)skinAtTenth);
  const double minSkin = BaselineNumber("garmentBurnSkinAtTenthMin", 275.0);
  const bool dd = skinAtTenth >= (int64_t)minSkin;
  if (wet * 100u > vox0) {
    detail = Format("fixture is WET (%u smock voxels, over 1%%, carry a water coat before "
                    "the spark): the ground under it holds water", wet);
    return Status::Fail;
  }
  detail = Format(
      "smock (%zu panels) %u voxels, %u lit (peak %u alight), no world fire, %d ticks: A "
      "consumed %u (%.0f%%, need %.0f%%) %s | B left charred %u vs gone %u %s "
      "| C flesh alight peak %u (first at t+%s) %s | D skin lost by 10%% of the "
      "garment gone %lld (need %.0f) %s%s",
      shell.size(), vox0, lit, peakBurning, kTicks, gone, goneFrac * 100.0, minGone * 100.0,
      a ? "ok" : "FAIL", charEnd, gone, b ? "ok" : "FAIL", peakAlight,
      firstAlight < 0 ? "never" : std::to_string(firstAlight).c_str(),
      cc ? "ok" : "FAIL", (long long)skinAtTenth, minSkin, dd ? "ok" : "FAIL",
      diedAt >= 0 ? Format(" | died at t+%d", diedAt).c_str() : "");
  return a && b && cc && dd ? Status::Pass : Status::Fail;
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
    arms[a].v0 = f.TorsoVoxels();
    c.mobs.ResetWornStats();
    for (int i = 0; i < kTicks; i++) {
      f.Step([&](std::vector<CellOp>& cellOps) {
        if (f.TorsoIndex() < 0) return;
        const Vec3 at = f.TorsoCentre();
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
    arms[a].v1 = f.TorsoVoxels();
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
//
// THE ROOT LIMB, AND A FOCUSED SPLASH (fight64 R, 2026-10-04). The coat is
// counted on the root limb because it is the one limb the corpse's OWN gout
// (it bleeds from there) never lands on -- SplatterView skips the bleeding
// limb -- so any rise is this burst's. The burst used to be body-stain's wide
// one (cone 0.35): from a metre that hands the root limb ~5 of the 24 trials
// (its bounding disc over the burst's solid angle), each aimed at a random
// point of that disc, and the pelvis fills ~30% of it -- about a 1-in-6
// chance of no landing at all. Which pose the corpse settled in decided it:
// "0 -> 9" before the corpse hull (fight64 P), "0 -> 0" after, while
// SANDVOX_SPLAT_TRACE showed every OTHER limb of the same corpse taking that
// burst (the flight is analytic against the voxel lattice and never meets a
// collider, so the hull is not in its path). A focused splash (cone 0.12)
// throws every trial at the root limb, and the claim -- blood thrown at a
// corpse lands on it -- no longer rides on a coin flip.
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
  const uint32_t before = f.TorsoCoat(mBlood);
  const Vec3 at = f.TorsoCentre();
  SplatterEvent ev;
  ev.origin = at + Vec3{MetresToCells(1.0f), 0.3f, 0.0f};
  ev.axis = Vec3{-1.0f, 0.0f, 0.0f};
  ev.cone = 0.12f;   // focused: see the note above the gate
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
  const uint32_t after = f.TorsoCoat(mBlood);
  c.mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  RecordObserved("corpseSplatterMarked", (double)(after > before ? after - before : 0));
  const bool ok = after > before;
  detail = Format("a focused 24-droplet burst at a dead torso from 1 m: blood coat %u -> %u",
                  before, after);
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// corpse-acid: acid on the DEAD does what it does on the living
// ---------------------------------------------------------------------------
// Owner report 2026-09-23 ("acid stains aren't working on corpses"). The dead
// ran a parallel path (SplatterDeadFlesh -> StainDeadFlesh -> BurnDeadFlesh); a
// corpse is a dead Mob now and runs the living's own, so this pins that it
// still does. A flask-pour-shaped burst of acid at a
// dead torso must COAT it and then EAT it, and the coat must be spent.
Status GateCorpseAcid(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  const uint32_t mAcid = MatIdOf(c, "acid");
  if (!mAcid) {
    detail = "acid missing from materials.json";
    return Status::Fail;
  }
  PrepareWorld(c);
  // THE LIVING CONTROL: the identical burst at a standing creature's torso,
  // so "the dead take less" is a differential and not a guess.
  uint32_t livePeak = 0, liveN0 = 0, liveN90 = 0;
  {
    DeadFixture g(c);
    const Target t = ChooseTarget(c.mobs, FixtureSite(c.world, 420));
    const uint64_t lid = t.valid() ? SpawnTarget(c, t, 420, g.pchunk) : 0;
    if (lid) {
      c.mobs.SetMobBehavior(lid, "dummy");
      g.tick = 61500;
      for (int i = 0; i < 8; i++) g.Step();
      const MobDef& def = c.mobs.Defs()[t.defIndex];
      const int nl2 = (int)def.limbs.size();
      auto census = [&](uint32_t& vox, uint32_t& coat) {
        vox = coat = 0;
        for (int li = 0; li < nl2; li++) {
          vox += c.mobs.LimbSkinVoxelCount(lid, li);
          coat += c.mobs.LimbCoatMatCount(lid, li, mAcid, 1);
        }
      };
      uint32_t coat = 0;
      census(liveN0, coat);
      SplatterEvent le;
      le.origin = c.mobs.LimbVoxelPos(lid, def.rootLimb, 0) +
                  Vec3{0.0f, MetresToCells(1.0f), 0.0f};
      le.axis = Vec3{0.0f, -1.0f, 0.0f};
      le.cone = 0.35f;
      le.reach = MetresToCells(2.0f);
      le.speed = MetresToCells(3.0f);
      le.life = 70;
      le.count = 48;
      le.mat = mAcid;
      le.amount = 6;
      le.tick = g.tick;
      le.seed = 0xAC1Du;
      c.mobs.QueueSplatter(le);
      for (int i = 0; i < 90 && c.mobs.IsAlive(lid); i++) {
        g.Step();
        uint32_t v = 0;
        census(v, coat);
        livePeak = std::max(livePeak, coat);
        liveN90 = v;
      }
    }
    c.mobs.Reset();
    c.debris.Reset();
  }
  DeadFixture f(c);
  if (!f.Make(420, 62000)) {
    detail = f.why;
    return Status::Fail;
  }
  const uint32_t n0 = f.TorsoVoxels();
  const Vec3 at = f.TorsoCentre();
  SplatterEvent ev;
  ev.origin = at + Vec3{0.0f, MetresToCells(1.0f), 0.0f};
  ev.axis = Vec3{0.0f, -1.0f, 0.0f};
  ev.cone = 0.35f;
  ev.reach = MetresToCells(2.0f);
  ev.speed = MetresToCells(3.0f);
  ev.life = 70;
  ev.count = 48;
  ev.mat = mAcid;
  ev.amount = 6;
  ev.tick = f.tick;
  ev.seed = 0xAC1Du;
  c.mobs.QueueSplatter(ev);
  uint32_t coatPeak = 0;
  std::string trace;
  for (int i = 0; i < 90; i++) {
    f.Step();
    const uint32_t coat = f.TorsoCoat(mAcid);
    coatPeak = std::max(coatPeak, coat);
    if (i % 10 == 0) trace += Format("%u/%u ", f.TorsoVoxels(), coat);
  }
  const uint32_t n90 = f.TorsoVoxels();
  uint32_t left = f.TorsoCoat(mAcid);
  const uint32_t coatAt90 = left;
  for (int i = 0; i < 900 && left; i++) {
    f.Step();
    left = f.TorsoCoat(mAcid);
  }
  c.mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  // Eats SOME, and is GONE within 90 ticks (3 s): an acid stain dries off on
  // its own in a few seconds, bone included (owner, 2026-09-23).
  const uint32_t at90 = coatAt90;
  const bool ok = coatPeak > 0 && n90 < n0 && at90 == 0 && left == 0;
  detail = Format("acid burst on a dead torso: coat peak %u, voxels %u -> %u in "
                  "90 ticks, acid on it at 90 ticks %u (want 0), after 900 more %u "
                  "| trace %s| LIVING "
                  "same burst: coat peak %u, voxels %u -> %u",
                  coatPeak, n0, n90, at90, left, trace.c_str(), livePeak, liveN0,
                  liveN90);
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// lava-quench: a lava-coated SEVERED HEAD dropped in water is quenched
// ---------------------------------------------------------------------------
// Owner ticket 2026-10-08_23-13-24: "put a disembodied head, covered in lava
// into water. the water should stain and clean off the lava immediately, and
// probably produce smoke as it does so". The head is severed flesh -- a
// DebrisSystem body whose coat runs through MobSystem's passes
// (StainDeadFlesh, the burn pass) -- so this cuts a real one off a corpse,
// coats it in lava, and bathes it.
//
// Two arms on the same fixture, kTicks each: WET (water written round the
// head every tick, fill-if-air so the steam it makes is not painted over) and
// DRY (nothing). The claims:
//   A. the fixture took the coat (lava on the head before the bath);
//   B. in water the lava is GONE: at most lavaQuenchMaxFrac of it is left
//      after lavaQuenchTicks ticks;
//   C. ...and it was the WATER: the dry arm still wears at least
//      lavaQuenchDryMinFrac of its coat at the same tick (lava cools on its
//      own in seconds, coat.decay, so "gone eventually" proves nothing);
//   D. the quench STEAMS: steam cells round the head in the wet arm (summed
//      over the ticks), and none in the dry arm.
// Thresholds in tests/baseline.json.
Status GateLavaQuench(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  const uint32_t mLava = MatIdOf(c, "lava"), mWater = MatIdOf(c, "water"),
                 mSteam = MatIdOf(c, "steam");
  if (!mLava || !mWater || !mSteam) {
    detail = "lava, water or steam missing from materials.json";
    return Status::Fail;
  }
  const int kTicks = (int)BaselineNumber("lavaQuenchTicks", 45);
  struct Arm {
    uint32_t coat0 = 0, coatEnd = 0, steamTicks = 0, steamPeak = 0;
    uint32_t pieces = 0, bare0 = 0, bareEnd = 0;
    uint64_t coatFired = 0, boiled = 0;
    std::string trace, why;
  };
  // EVERY loose body that came off `corpse`, flesh or not: a severed head
  // takes its BLOODLESS hair limb with it (MobLimbDef::bloodless), and that
  // piece is not DebrisSystem::IsFlesh (no bleedMat), so ForEachCoatBody used
  // not to see it at all.
  auto forPieces = [&](uint64_t corpse,
                       const std::function<void(DebrisSystem::FleshLattice&, bool)>& fn) {
    for (uint32_t bi = 0; bi < c.debris.BodyCount(); bi++) {
      const uint64_t h = c.debris.BodyHandle(bi);
      DebrisSystem::FleshLattice f;
      if (!c.debris.CoatLatticeOf(h, f) || f.creature != corpse) continue;
      fn(f, c.debris.BodyIsDeadFlesh(h));
    }
  };
  // Lava-coated voxels (any amount) over every severed piece of `corpse`;
  // `onBare` gets the share on pieces that are not flesh (the hair); `centre`
  // the biggest piece's box centre.
  auto census = [&](uint64_t corpse, Vec3* centre, uint32_t* pieces,
                    uint32_t* onBare = nullptr) {
    uint32_t n = 0, best = 0;
    if (pieces) *pieces = 0;
    if (onBare) *onBare = 0;
    forPieces(corpse, [&](DebrisSystem::FleshLattice& f, bool flesh) {
      if (pieces) (*pieces)++;
      const size_t sz = f.skin ? f.skin->size() : (f.coll ? f.coll->size() : 0);
      for (size_t i = 0; i < sz; i++) {
        const uint16_t s = f.skin ? (*f.skin)[i].stain : (*f.coll)[i].stain;
        if (BodyStainAmt(s) != 0 && BodyStainMat(s) == mLava) {
          n++;
          if (!flesh && onBare) (*onBare)++;
        }
      }
      if (centre && f.xf && sz > best) {
        best = (uint32_t)sz;
        const float ps = 1.0f / (float)std::max(1u, f.physScale);
        const Vec3 mid{(float)(f.lo.x + f.hi.x) * 0.5f * ps,
                       (float)(f.lo.y + f.hi.y) * 0.5f * ps,
                       (float)(f.lo.z + f.hi.z) * 0.5f * ps};
        const Quat q{f.xf->quat[0], f.xf->quat[1], f.xf->quat[2], f.xf->quat[3]};
        *centre = f.xf->pos + QuatRotate(q, mid);
      }
    });
    return n;
  };
  // What the head IS, beside what it wears: voxels left, and how many of them
  // are a HOT material (tag "hot": flesh_burning, hair_burning, ...) -- the
  // other thing that reads as "still covered in lava" on screen.
  std::vector<uint8_t> isHot(c.mats.size(), 0);
  for (size_t m = 0; m < c.mats.size(); m++)
    for (const std::string& tg : c.mats[m].tags)
      if (tg == "hot") isHot[m] = 1;
  auto bodyCensus = [&](uint64_t corpse, uint32_t& vox, uint32_t& hot) {
    vox = hot = 0;
    forPieces(corpse, [&](DebrisSystem::FleshLattice& f, bool) {
      const size_t sz = f.skin ? f.skin->size() : (f.coll ? f.coll->size() : 0);
      for (size_t i = 0; i < sz; i++) {
        const uint32_t m = f.skin ? ((*f.skin)[i].material & 0xFFFu)
                                  : ((*f.coll)[i].payload & 0xFFFu);
        if (m == 0) continue;
        vox++;
        if (m < isHot.size() && isHot[m]) hot++;
      }
    });
  };
  auto matNear = [&](const Vec3& at, uint32_t mat, int r) -> uint32_t {
    const IVec3 b{ifloor(at.x), ifloor(at.y), ifloor(at.z)};
    uint32_t n = 0;
    for (int dy = -r; dy <= r; dy++)
      for (int dz = -r; dz <= r; dz++)
        for (int dx = -r; dx <= r; dx++) {
          const IVec3 cc{b.x + dx, b.y + dy, b.z + dz};
          if (!c.world.CellInWindow(cc)) continue;
          const CachedChunk* k = c.world.Cached(IVec3{cc.x >> 4, cc.y >> 4, cc.z >> 4});
          if (!k || k->voxels.size() != kChunkVol) continue;
          const uint32_t w = k->voxels[((uint32_t)(cc.z & 15) * kChunk +
                                        (uint32_t)(cc.y & 15)) * kChunk +
                                       (uint32_t)(cc.x & 15)];
          if ((w & 0xFFFu) == mat) n++;
        }
    return n;
  };
  auto steamNear = [&](const Vec3& at) -> uint32_t {
    const IVec3 b{ifloor(at.x), ifloor(at.y), ifloor(at.z)};
    uint32_t n = 0;
    for (int dy = -3; dy <= 10; dy++)
      for (int dz = -5; dz <= 5; dz++)
        for (int dx = -5; dx <= 5; dx++) {
          const IVec3 cc{b.x + dx, b.y + dy, b.z + dz};
          if (!c.world.CellInWindow(cc)) continue;
          const CachedChunk* cc16 = c.world.Cached(IVec3{cc.x >> 4, cc.y >> 4, cc.z >> 4});
          if (!cc16 || cc16->voxels.size() != kChunkVol) continue;
          const uint32_t w = cc16->voxels[((uint32_t)(cc.z & 15) * kChunk +
                                           (uint32_t)(cc.y & 15)) * kChunk +
                                          (uint32_t)(cc.x & 15)];
          if ((w & 0xFFFu) == mSteam) n++;
        }
    return n;
  };
  auto runArm = [&](bool wet) -> Arm {
    Arm a;
    PrepareWorld(c);
    DeadFixture f(c);
    if (!f.Make(300, 63000)) {
      a.why = f.why;
      return a;
    }
    const Mob* mob = c.mobs.FindMobById(f.id);
    const MobDef* def = mob ? mob->Def() : nullptr;
    int head = -1;
    if (def)
      for (size_t li = 0; li < def->limbs.size(); li++)
        if (def->limbs[li].vital && def->limbs[li].severable &&
            (int)li != def->rootLimb && c.mobs.LimbBody(f.id, (int)li)) {
          head = (int)li;
          break;
        }
    if (head < 0) {
      a.why = "the corpse has no severable head";
      return a;
    }
    // THE BATH FIRST: water round the head while it is still on the corpse,
    // so the claim is about the quench and not about how long the fill takes
    // to arrive. Fill-if-air every tick (the steam the quench makes is not
    // painted over, and what flows away is topped up).
    Vec3 at{};
    if (!mob->LimbCentreWorld(head, at)) at = c.mobs.LimbVoxelPos(f.id, head, 0);
    auto bath = [&](std::vector<CellOp>& cellOps) {
      if (!wet) return;
      const IVec3 b{ifloor(at.x), ifloor(at.y), ifloor(at.z)};
      for (int dy = -3; dy <= 3; dy++)
        for (int dz = -4; dz <= 4; dz++)
          for (int dx = -4; dx <= 4; dx++) {
            const IVec3 cc{b.x + dx, b.y + dy, b.z + dz};
            if (!c.world.CellInWindow(cc)) continue;
            if (cellOps.size() >= kMaxCellOpsPerTick) return;
            if (cc.y <= World::TerrainHeight(cc.x, cc.z, kDefaultSeed)) continue;
            cellOps.push_back({World::SlotCellIndex(cc),
                               PackVoxNew(mWater, 8u) | kCellOpIfAir});
          }
    };
    {
      const IVec3 b{ifloor(at.x), ifloor(at.y), ifloor(at.z)};
      f.pchunk = IVec3{b.x >> 4, b.y >> 4, b.z >> 4};
    }
    for (int i = 0; i < 8; i++) f.Step(bath);
    c.mobs.Sever(f.id, head);
    // ...THEN THE LAVA, on the SURFACE of the head and of everything that
    // came off with it (the hair limb, bloodless): what a dunk in a lava pool
    // leaves -- the contact pass coats exposed voxels only, so a coat on the
    // inside of a lattice is nothing the game can make and nothing water
    // could reach.
    forPieces(f.id, [&](DebrisSystem::FleshLattice& p, bool) {
      const StainLattice L{p.skin, p.coll};
      if (!L.skin && !L.coll) return;
      const LatticeOcc occ = BuildLatticeOcc(L);
      for (size_t i = 0; i < L.Size(); i++)
        if (L.Mat(i) != 0 && occ.Exposed(L.At(i)))
          L.SetStain(i, PackBodyStain(mLava, 10));
    });
    a.coat0 = census(f.id, &at, &a.pieces, &a.bare0);
    if (a.pieces == 0) {
      a.why = "the severed head is not a loose body";
      return a;
    }
    c.mobs.ResetBurnStats();
    for (int i = 0; i < kTicks; i++) {
      census(f.id, &at, nullptr);
      const IVec3 b{ifloor(at.x), ifloor(at.y), ifloor(at.z)};
      f.pchunk = IVec3{b.x >> 4, b.y >> 4, b.z >> 4};
      f.Step(bath);
      const uint32_t s = steamNear(at);
      a.steamTicks += s;
      a.steamPeak = std::max(a.steamPeak, s);
      if (i % 5 == 0 || i + 1 == kTicks) {
        uint32_t vox = 0, hot = 0;
        bodyCensus(f.id, vox, hot);
        a.trace += Format(" t+%d %u/%u/%u/%u/%u", i, census(f.id, nullptr, nullptr), s,
                          vox, hot, matNear(at, mWater, 3));
      }
    }
    a.coatEnd = census(f.id, nullptr, nullptr, &a.bareEnd);
    a.coatFired = c.mobs.Burn().coatFired;
    a.boiled = c.mobs.Burn().coatBoiled;
    c.mobs.Reset();
    c.debris.Reset();
    return a;
  };
  const Arm w = runArm(true);
  const Arm d = w.why.empty() ? runArm(false) : Arm{};
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  if (!w.why.empty() || !d.why.empty()) {
    detail = !w.why.empty() ? w.why : d.why;
    return Status::Fail;
  }
  RecordObserved("lavaQuenchWetLeft", w.coat0 ? (double)w.coatEnd / w.coat0 : 0.0);
  RecordObserved("lavaQuenchDryLeft", d.coat0 ? (double)d.coatEnd / d.coat0 : 0.0);
  RecordObserved("lavaQuenchSteam", (double)w.steamTicks);
  const double maxWet = BaselineNumber("lavaQuenchMaxFrac", 0.1);
  const double minDry = BaselineNumber("lavaQuenchDryMinFrac", 0.3);
  const bool a = w.coat0 > 0 && d.coat0 > 0;
  const bool b = a && (double)w.coatEnd <= maxWet * (double)w.coat0;
  const bool cc = a && (double)d.coatEnd >= minDry * (double)d.coat0;
  const bool dd = w.steamTicks > 0 && w.boiled > 0 && d.steamTicks == 0;
  detail = Format(
      "severed head (%u pieces), lava coat, %d ticks: A coated %u (wet, %u on bloodless pieces) / %u (dry) %s | B in "
      "water %u -> %u (%u on bloodless pieces; max %.0f%%) %s | C dry %u -> %u (min %.0f%%) %s | D steam "
      "cell-ticks wet %u (peak %u, %llu boils) dry %u %s | coat rules fired wet %llu dry %llu "
      "| wet trace (lava/steam/voxels/hot/water r3):%s | dry trace:%s",
      w.pieces, kTicks, w.coat0, w.bare0, d.coat0, a ? "ok" : "FAIL", w.coat0, w.coatEnd,
      w.bareEnd, maxWet * 100.0,
      b ? "ok" : "FAIL", d.coat0, d.coatEnd, minDry * 100.0, cc ? "ok" : "FAIL",
      w.steamTicks, w.steamPeak, (unsigned long long)w.boiled, d.steamTicks, dd ? "ok" : "FAIL",
      (unsigned long long)w.coatFired, (unsigned long long)d.coatFired, w.trace.c_str(),
      d.trace.c_str());
  return a && b && cc && dd ? Status::Pass : Status::Fail;
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
// ---- mob-rain: rain wets a creature standing in it (MobSystem::RainOneLimb) --
//
// Three arms, each a FRESH pinned dummy for 60 ticks under one rain word: a dry
// sky (the control — nothing may come out coated), a drizzle's word and a
// storm's. Fresh per arm so a drizzle's coat cannot pre-soak the storm arm and
// flatten the ratio. The claims: the word reaches the stain pass, the column
// probe reads open sky, top voxels take water — and the RATE climbs steeply
// with the rain (storm >= rainMobStormOverDrizzle x drizzle; the curve is a
// fourth power, ~20x). Counted over every limb. Pure CPU stain pass, like
// body-coat's poseTick; the mobs are Reset on the way out.
Status GateMobRain(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  constexpr int kInset = 300;
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, kInset));
  if (!t.valid()) {
    detail = "no loaded mob def to stand in the rain";
    return Status::Fail;
  }
  const MobDef& def = mobs.Defs()[t.defIndex];
  constexpr int kTicks = 60;
  uint32_t simTick = 52000;
  bool spawned = true;
  MobSystem::RainStats stormStats{};
  auto arm = [&](uint32_t rainWord) -> uint32_t {
    IVec3 pchunk{};
    const uint64_t id = SpawnTarget(c, t, kInset, pchunk);
    if (!id) { spawned = false; return 0; }
    mobs.SetMobBehavior(id, "dummy");
    mobs.SetWeatherRain(rainWord);
    mobs.rainStats_ = {};
    // DIRECT PHASE CALLS ON PURPOSE (W2-O): the rain word is this fixture's
    // INPUT, set on MobSystem per arm. The real tick latches the weather's own
    // word in phase H every tick and would overwrite it; pinning a weather
    // preset instead would also rain on the GPU grid, a different fixture.
    for (int k = 0; k < kTicks; k++) {
      ++simTick;
      std::vector<BrushOp> ops;
      std::vector<ParticleSpawn> spawns;
      std::vector<CellOp> cellOps;
      mobs.PreTick(simTick, c.world, ops, cellOps, spawns);
      c.phys.Step(kTickDt);
      mobs.PostStep();
    }
    stormStats = mobs.rainStats_;
    // LimbStainCount (any coat), NOT LimbStainedMatCount: the latter counts
    // voxels whose OWN material is `mat` ("the bone is bloodied"), which for
    // water is none. The dry arm is the control for any coat the creature
    // arrived with; the only coat this fixture can add is the rain's.
    // DEPTH, not coverage: sum over levels of "voxels at >= level" is the
    // summed coat amount. A storm covers every top voxel in well under the
    // arm, so a count of coated voxels saturates and hides the rate.
    uint32_t coated = 0;
    for (int li = 0; li < (int)def.limbs.size(); li++)
      if (mobs.LimbBody(id, li))
        for (uint32_t lv = 1; lv <= kBodyStainAmtMax; lv++)
          coated += mobs.LimbStainCount(id, li, lv);
    mobs.SetWeatherRain(0u);
    mobs.Reset();
    return coated;
  };
  // weather::SimRainWord's layout: rain | damp << 8 | wetness << 16.
  const uint32_t dry = arm(0u);
  const uint32_t drizzle = arm(115u | (153u << 8) | (184u << 16));
  const uint32_t storm = arm(242u | (153u << 8) | (255u << 16));
  const MobSystem::RainStats rs = stormStats;
  const double ratio = BaselineNumber("rainMobStormOverDrizzle", 3.0);
  const bool ok = spawned && dry == 0 && drizzle > 0 &&
                  (double)storm >= ratio * (double)drizzle;
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "%s: %s summed coat depth after %d ticks — dry %u (want 0), drizzle "
                "%u (want > 0), storm %u (want >= %.1fx drizzle) [storm arm: "
                "calls %u, off-cadence %u, roofed %u, no view %u, sampled %u, "
                "not top %u, capped %u, wrote %u]%s",
                ok ? "PASS" : "FAIL", t.defName.c_str(), kTicks, dry, drizzle,
                storm, ratio, rs.calls, rs.offCadence, rs.roofed, rs.noView,
                rs.sampled, rs.notTop, rs.capped, rs.wrote,
                spawned ? "" : " (SPAWN REFUSED)");
  detail = buf;
  std::printf("mob-rain: %s\n", buf);
  return ok ? Status::Pass : Status::Fail;
}

// ---- living-blood: a LIVING body's anatomy blood does not dry -----------------
//
// Harrowby, 2026-09-29: over one quiet day every villager ended up tracking
// blood round the green with no blood lost. SANDVOX_COAT_TRACE named it: the
// anatomy recipe's blood (speckled through the muscle, assets/mobs/human.json)
// left a LIVING limb by its own decay rule -- blood's `"drying"` rule, blood ->
// air, authored for a pool on the ground -- on a limb with no world threat
// near it, awake only because a coat pass (a foot on stained ground, rain, a
// wet coat) had built its burn index and the index seeded the front with every
// self-active voxel. Each voxel that left bared bone; the bared-bone pass
// painted the bone with the creature's blood; the feet printed it.
//
// The fixture is the general case, not the village: a pinned dummy under a
// storm (rain holds the index every tick, mob-rain's word), then the SAME
// dummy dead under the same storm. Claims, per arm, over every base limb:
//   * LIVING: not one blood voxel gone, not one skin voxel gone, no blood coat,
//     nothing bled -- and the fixture reached the path: the rain coated it and
//     a limb holds a burn index (the build that used to seed the front with
//     the anatomy blood), or the fixture proves nothing.
//   * DEAD (the control that proves the counter can see a loss): a corpse's
//     blood still dries, so its count falls. The dead do not heal
//     (Mob::WoundsHeal), and drying is what a corpse does.
// DIRECT PHASE CALLS ON PURPOSE (W2-O): as mob-rain, the rain word is this
// fixture's input on MobSystem, which the real tick's phase H overwrites.
Status GateLivingBlood(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  constexpr int kInset = 300;
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, kInset));
  const uint32_t mBlood = mobs.MaterialIdNamed("blood");
  if (!t.valid() || !mBlood) {
    detail = "no bleeding mob def / no blood material";
    return Status::Fail;
  }
  const MobDef& def = mobs.Defs()[t.defIndex];
  IVec3 pchunk{};
  const uint64_t id = SpawnTarget(c, t, kInset, pchunk);
  if (!id) {
    detail = "spawn refused";
    return Status::Fail;
  }
  mobs.SetMobBehavior(id, "dummy");
  const int nLimbs = (int)def.limbs.size();
  auto count = [&](uint64_t& skin, uint64_t& blood) {
    skin = blood = 0;
    for (int li = 0; li < nLimbs; li++) {
      skin += mobs.LimbSkinVoxelCount(id, li);
      blood += mobs.LimbMaterialCount(id, li, mBlood);
    }
  };
  uint32_t simTick = 54000;
  auto run = [&](int ticks) {
    for (int k = 0; k < ticks; k++) {
      ++simTick;
      std::vector<BrushOp> ops;
      std::vector<ParticleSpawn> spawns;
      std::vector<CellOp> cellOps;
      mobs.PreTick(simTick, c.world, ops, cellOps, spawns);
      c.phys.Step(kTickDt);
      mobs.PostStep();
    }
  };
  auto coatOf = [&](uint32_t mat) -> uint32_t {
    mobs.RecountCoatOn(id, simTick);
    const LimbCoat body = mobs.BodyCoat(id);
    uint32_t sum = 0;
    for (const CoatEntry& en : body.top)
      if (en.mat == mat) sum += en.sumAmt;
    return sum;
  };
  const uint32_t mWater = mobs.MaterialIdNamed("water");
  const int ticks = (int)BaselineNumber("livingBloodTicks", 600.0);
  // weather::SimRainWord's layout: rain | damp << 8 | wetness << 16.
  const uint32_t storm = 242u | (153u << 8) | (255u << 16);
  uint64_t skin0 = 0, blood0 = 0, skin1 = 0, blood1 = 0;
  count(skin0, blood0);
  mobs.ResetBurnStats();
  mobs.SetWeatherRain(storm);
  run(ticks);
  count(skin1, blood1);
  const uint32_t refused = mobs.Burn().livingDryRefused;
  // THE FIXTURE REACHED THE PATH: the rain holds a burn index on the limbs,
  // and building one is what seeded the front with the anatomy blood before
  // the fix (BuildBurnIndex now leaves it off, so the rule loop's refusal is
  // a backstop and may count 0).
  int indexed = 0;
  for (int li = 0; li < nLimbs; li++)
    if (mobs.LimbBody(id, li) && mobs.LimbBurnStateOf(id, li).indexed) indexed++;
  const uint32_t bloodCoat = coatOf(mBlood);
  const uint32_t wet = mWater ? coatOf(mWater) : 0u;
  const float bled = mobs.BloodLost(id);
  // THE CONTROL: the same body, dead, the same storm.
  uint64_t deadSkin0 = 0, deadBlood0 = 0, deadSkin1 = 0, deadBlood1 = 0;
  bool died = false;
  if (Mob* m = mobs.FindMobById(id)) {
    m->Die();
    died = true;
  }
  count(deadSkin0, deadBlood0);
  run(ticks);
  count(deadSkin1, deadBlood1);
  mobs.SetWeatherRain(0u);
  mobs.Reset();
  const bool livingOk = blood1 == blood0 && skin1 == skin0 && bloodCoat == 0 && bled <= 0.0f;
  const bool exercised = blood0 > 0 && indexed > 0 && wet > 0;
  const bool controlOk = died && deadBlood1 < deadBlood0;
  const bool ok = livingOk && exercised && controlOk;
  detail = Format("%s: %s under a storm for %d ticks -- LIVING: anatomy blood %llu -> %llu, skin %llu -> %llu, "
                  "blood coat %u, bled %.1f (want all unchanged / 0); %d limbs holding a burn index (want > 0), "
                  "drying refused in the rule loop %u, water coat %u (want > 0) | DEAD control: blood %llu -> "
                  "%llu (want it to fall)",
                  ok ? "PASS" : "FAIL", t.defName.c_str(), ticks, (unsigned long long)blood0,
                  (unsigned long long)blood1, (unsigned long long)skin0, (unsigned long long)skin1, bloodCoat,
                  bled, indexed, refused, wet, (unsigned long long)deadBlood0, (unsigned long long)deadBlood1);
  std::printf("living-blood: %s\n", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- rain-oil: oil goes on OVER rain, and rain takes it back off ---------
//
// Owner report 2026-09-23: oiled in a storm, the HUD said only "water", and
// the rain never cleaned the oil. Two defects, one fixture:
//   * OIL LANDS ON A WET BODY. A storm-soaked dummy (wet ~12 on every top
//     voxel) is poured with oil (SoakLimb, oil's pour amount, surface only).
//     Under Raise's "a different coat repaints only when strictly heavier"
//     the wet voxels refused it; MobSystem::CoatBeneath now lets a washer's
//     coat give way. Claim: the water ledger's voxel count falls to at most
//     rainOilWetLeftMax of what it was (oil and rain both live on the
//     surface, so oil should displace nearly all of it), and the body ledger
//     names BOTH substances once rain has had a few ticks back on it.
//   * RAIN RINSES IT, SIDES INCLUDED. The storm keeps going; the oil's summed
//     amount must fall to at most rainOilLeftMax of what was poured. Tops
//     alone left a standing figure's sides oiled indefinitely, which is what
//     RainOneLimb's runoff roll is for.
// Pure CPU stain pass, like mob-rain; mobs Reset on the way out.
Status GateRainOil(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  constexpr int kInset = 300;
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, kInset));
  if (!t.valid()) {
    detail = "no loaded mob def to stand in the rain";
    return Status::Fail;
  }
  uint32_t mOil = 0, mWater = 0;
  for (size_t i = 0; i < c.mats.size(); i++) {
    if (c.mats[i].name == "oil") mOil = (uint32_t)i;
    if (c.mats[i].name == "water") mWater = (uint32_t)i;
  }
  if (!mOil || !mWater) {
    detail = "materials oil / water not loaded";
    return Status::Fail;
  }
  const MobDef& def = mobs.Defs()[t.defIndex];
  IVec3 pchunk{};
  const uint64_t id = SpawnTarget(c, t, kInset, pchunk);
  if (!id) {
    detail = "spawn refused";
    return Status::Fail;
  }
  mobs.SetMobBehavior(id, "dummy");
  uint32_t simTick = 53000;
  // DIRECT PHASE CALLS ON PURPOSE (W2-O): as mob-rain, the rain word is the
  // fixture's input on MobSystem, which the real tick's phase H overwrites.
  auto run = [&](int ticks) {
    for (int k = 0; k < ticks; k++) {
      ++simTick;
      std::vector<BrushOp> ops;
      std::vector<ParticleSpawn> spawns;
      std::vector<CellOp> cellOps;
      mobs.PreTick(simTick, c.world, ops, cellOps, spawns);
      c.phys.Step(kTickDt);
      mobs.PostStep();
    }
  };
  auto entry = [&](uint32_t mat) -> CoatEntry {
    mobs.RecountCoatOn(id, simTick);
    const LimbCoat body = mobs.BodyCoat(id);
    for (const CoatEntry& en : body.top)
      if (en.mat == mat) return en;
    return CoatEntry{};
  };
  // weather::SimRainWord's layout: rain | damp << 8 | wetness << 16.
  const uint32_t storm = 242u | (153u << 8) | (255u << 16);
  const int soakTicks = (int)BaselineNumber("rainOilSoakTicks", 90.0);
  const int washTicks = (int)BaselineNumber("rainOilWashTicks", 900.0);
  mobs.SetWeatherRain(storm);
  run(soakTicks);
  const CoatEntry wetBefore = entry(mWater);

  // The substance's own authored per-contact amount (materials.json stain).
  const uint32_t packed =
      (c.mats[mOil].gpu.stainPack >> kStainPackAmtShift) & kStainPackAmtMask;
  const uint32_t pour = packed ? packed : 6u;
  uint32_t marked = 0;
  for (int li = 0; li < (int)def.limbs.size(); li++)
    if (mobs.LimbBody(id, li)) marked += mobs.SoakLimb(id, li, mOil, pour, simTick);
  const CoatEntry wetAfterPour = entry(mWater);
  const CoatEntry oilPoured = entry(mOil);
  // OILED FEET SLIP (2026-09-23): the slip reads the SOLE fraction, and a
  // freshly oiled sole has to clear player.slipCoatFull -- the whole-foot
  // fraction it used to read divided the coat by the foot's interior and
  // never reached onset, so oily feet did nothing. Both are printed.
  const float soleSlip = mobs.CoatTagFraction(id, "slippery", "foot", true);
  const float footSlip = mobs.CoatTagFraction(id, "slippery", "foot", false);
  const float slipFull = CurrentTuning().player.slipCoatFull;
  const bool slips = soleSlip >= slipFull;

  run(30);
  const CoatEntry oilEarly = entry(mOil), wetEarly = entry(mWater);
  const bool bothNamed = oilEarly.sumAmt > 0 && wetEarly.sumAmt > 0;

  run(washTicks - 30);
  const CoatEntry oilLeft = entry(mOil);
  mobs.SetWeatherRain(0u);
  mobs.Reset();

  const double wetLeftMax = BaselineNumber("rainOilWetLeftMax", 0.25);
  const double oilLeftMax = BaselineNumber("rainOilLeftMax", 0.35);
  const double wetLeft = wetBefore.voxels
                             ? (double)wetAfterPour.voxels / wetBefore.voxels
                             : 1.0;
  const double oilFrac = oilPoured.sumAmt
                             ? (double)oilLeft.sumAmt / oilPoured.sumAmt
                             : 1.0;
  const bool landed = wetBefore.voxels > 0 && oilPoured.voxels > 0 &&
                      wetLeft <= wetLeftMax;
  const bool washed = oilPoured.sumAmt > 0 && oilFrac <= oilLeftMax;
  const bool ok = landed && bothNamed && washed && slips;
  char buf[800];
  std::snprintf(
      buf, sizeof(buf),
      "%s: %s — storm soak %d ticks: %u wet voxels; oil pour (amount %u) "
      "marked %u, oil on %u voxels, wet voxels left %u (%.2f, want <= %.2f)%s; "
      "30 ticks on: oil %u + water %u summed (both named: %s); after %d storm "
      "ticks oil %u of %u summed (%.2f, want <= %.2f)%s; slippery soles %.3f "
      "(want >= slipCoatFull %.2f; whole foot %.3f)%s",
      ok ? "PASS" : "FAIL", t.defName.c_str(), soakTicks, wetBefore.voxels,
      pour, marked, oilPoured.voxels, wetAfterPour.voxels, wetLeft, wetLeftMax,
      landed ? "" : " [OIL REFUSED BY WATER]", oilEarly.sumAmt, wetEarly.sumAmt,
      bothNamed ? "yes" : "NO", washTicks, oilLeft.sumAmt, oilPoured.sumAmt,
      oilFrac, oilLeftMax, washed ? "" : " [RAIN DID NOT RINSE]", soleSlip,
      slipFull, footSlip, slips ? "" : " [OILED FEET DO NOT SLIP]");
  detail = buf;
  std::printf("rain-oil: %s\n", buf);
  return ok ? Status::Pass : Status::Fail;
}

Status GateBodyCoat(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  // ISOLATION (suite triage 2026-10-01): IdCounterScope only PUTS BACK the
  // id counter; the gate used to START from whatever earlier gates left it at.
  // A creature's gore profile and its bleed/stain rolls hash its id
  // (Mob::MakeGoreProfile, BleedTick), so the same fixture bled differently by
  // suite position -- red in the full suite, green in every subset. Pinned.
  mobs.SetNextIdCounter(1);
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
  // A WORLD-ONLY tick on purpose (W2-O): fixture construction, before any
  // creature exists -- walls and pools going in through the queue.
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
  // DIRECT PHASE CALLS ON PURPOSE (W2-O): the ledger and drying claims below
  // are about the BODY's own passes. A world ticking under it adds the contact
  // pass's coats from the ground (the cut's own blood, once it lands) to the
  // same ledger, which is a different quantity from the one asserted.
  auto poseTick = [&]() {
    ++simTick;
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    mobs.PreTick(simTick, c.world, ops, cellOps, spawns);
    c.phys.Step(kTickDt);
    mobs.PostStep();
  };
  // A tick that DOES submit, for the deposit (the particle kernel has to run):
  // THE REAL TICK (W2-O, test/tickrig.h), the mirror on the creature.
  support::TickCursor worldTicker{c, simTick, pchunk};
  auto worldTick = [&]() {
    IVec3 centre = pchunk;
    if (root >= 0 && mobs.LimbBody(id, root)) {
      const Vec3 at = mobs.LimbVoxelPos(id, root, 0);
      centre = IVec3{ifloor(at.x) >> 4, ifloor(at.y) >> 4, ifloor(at.z) >> 4};
    }
    worldTicker.chunk = centre;
    worldTicker();
  };
  for (int i = 0; i < 20; i++) worldTick();  // let it find the floor

  // ---- 1. the cut, and the ledger it leaves --------------------------------
  const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
  bool hit = false;
  // ATTRIBUTION (rule 6, suite triage 2026-10-01): the ledger claim failed
  // only at suite scope with "top = oil, frac 0" -- i.e. the cut left no
  // smear. Say what the cut did and what was on the limb before it.
  const uint32_t cutVox0 = mobs.LimbArtVoxelCount(id, t.limb);
  LimbCoat preCut{};
  if (const LimbCoat* lc = mobs.LimbCoatOf(id, t.limb)) preCut = *lc;
  const Vec3 cutAnchor = ax.anchor;
  // Which limbs carry what, and what is around the body in the CPU mirror the
  // contact pass reads: the material or stain the coat could have come from.
  std::string preCoats;
  if (const Mob* pm = mobs.FindMobById(id))
    for (int li = 0; li < pm->LimbCount(); li++)
      if (const LimbCoat* lc = mobs.LimbCoatOf(id, li))
        for (const CoatEntry& e : lc->top)
          if (e.mat != 0 && e.sumAmt != 0)
            preCoats += Format(" L%d:%u/%u", li, e.mat, e.sumAmt);
  std::string around;
  {
    const uint32_t mOilA = mobs.MaterialIdNamed("oil");
    const uint32_t oilType = mOilA ? mobs.StainTypeOf(mOilA) : 0u;
    uint32_t oilCells = 0, oilStained = 0, uncached = 0, liquid = 0;
    IVec3 firstAt{0, 0, 0};
    for (int z = -10; z <= 10; z++)
      for (int y = -20; y <= 8; y++)
        for (int x = -10; x <= 10; x++) {
          const IVec3 cc{ifloor(cutAnchor.x) + x, ifloor(cutAnchor.y) + y,
                         ifloor(cutAnchor.z) + z};
          const CachedChunk* k =
              c.world.Cached(IVec3{cc.x >> 4, cc.y >> 4, cc.z >> 4});
          if (!k || k->voxels.size() != kChunkVol) { uncached++; continue; }
          const uint32_t w = k->voxels[(((uint32_t)cc.z & 15u) * kChunk +
                                        ((uint32_t)cc.y & 15u)) * kChunk +
                                       ((uint32_t)cc.x & 15u)];
          const uint32_t m = w & 0xFFFu;
          if (m == mOilA && m) { if (!oilCells) firstAt = cc; oilCells++; }
          if (m < c.mats.size() && c.mats[m].gpu.klass == CLASS_LIQUID) liquid++;
          if (oilType && VoxStainType(w) == oilType && VoxStainAmt(w) > 0) {
            if (!oilCells && !oilStained) firstAt = cc;
            oilStained++;
          }
        }
    around = Format(" mirror: oil cells %u, oil-stained %u, liquid %u, uncached %u, first (%d,%d,%d)",
                    oilCells, oilStained, liquid, uncached, firstAt.x, firstAt.y, firstAt.z);
  }
  {
    std::vector<ParticleSpawn> spawns;
    hit = CutOnce(mobs, c.world, id, t.limb, ax, ax.reach * 0.5f, 0.9f, 1.0f,
                  0x5C0A7u, spawns);
  }
  const uint32_t cutVox1 = mobs.LimbArtVoxelCount(id, t.limb);
  poseTick();  // the first recount runs the moment anything is dirty
  const bool attached = mobs.LimbBody(id, t.limb) != 0;
  const std::string cutWhy = Format(
      " [cut: hit=%d valid=%d reach %.2f, art %u -> %u, anchor (%.1f,%.1f,%.1f), "
      "pre-cut top mat %u sum %u, mob %llu, coats%s;%s]",
      hit ? 1 : 0, ax.valid ? 1 : 0, ax.reach, cutVox0, cutVox1, cutAnchor.x,
      cutAnchor.y, cutAnchor.z, preCut.top[0].mat, preCut.sumAmt,
      (unsigned long long)id, preCoats.c_str(), around.c_str());
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

  // ---- A WET BODY DOES NOT CATCH (BurnOneLimb section 0) ------------------
  // Every limb but one soaked in water (doused enough times that any blood is
  // rinsed and the skin left wet), then the creature stood in a column of
  // world fire. Since W2-J2 water boils off at its own reaction chance (no
  // tuning knob), so a soaked limb in a standing fire is PROTECTED, not
  // immune: once the water on a voxel has boiled off, that voxel may sear
  // (owner accepted, 2026-09-24). The claim is that the wet root sears at
  // most `bodyCoatWetSearFrac` (tests/baseline.json, default 2%) of what the
  // one dry limb -- the control -- sears; the control must sear, or "the wet
  // limb did not burn" says nothing about the water.
  uint32_t mWater = 0, mFire = 0;
  for (size_t i = 0; i < c.mats.size(); i++) {
    if (c.mats[i].name == "water") mWater = (uint32_t)i;
    if (c.mats[i].name == "fire") mFire = (uint32_t)i;
  }
  int dryLimb = -1;
  for (int li = 0; li < (int)def.limbs.size(); li++)
    if (li != root && mobs.LimbBody(id, li) &&
        def.limbs[li].parent == def.limbs[root].name) {
      dryLimb = li;
      break;
    }
  auto searedOn = [&](int li) {
    // The stages skin and flesh walk through under heat, by name.
    uint32_t n = 0;
    for (const char* nm : {"flesh_cooked", "flesh_burning", "flesh_charred"})
      for (size_t m = 1; m < c.mats.size(); m++)
        if (c.mats[m].name == nm)
          n += mobs.LimbMaterialCount(id, li, (uint32_t)m);
    return n;
  };
  uint32_t wetSeared = 0, drySeared = 0, fireOpsPushed = 0, fireSeen = 0,
           dryBurningPeak = 0, probeMat = 0xFFFFFFFFu, probeVer = 0;
  IVec3 probeAt{};
  constexpr int kFireTicks = 90;
  if (mWater && mFire && dryLimb >= 0 && mobs.LimbBody(id, root)) {
    for (int li = 0; li < (int)def.limbs.size(); li++)
      if (li != dryLimb && mobs.LimbBody(id, li))
        for (int k = 0; k < 8; k++)
          mobs.DouseLimb(id, li, mWater, 15, ++simTick);
    // THE REAL TICK (W2-O, test/tickrig.h); the fire column is the gate's own
    // cell ops, written at the top of each tick.
    support::TickCursor fireTicker{c, simTick, pchunk};
    for (int k = 0; k < kFireTicks; k++) {
      support::TickOps pre;
      std::vector<CellOp>& cellOps = pre.cells;
      const Vec3 at = mobs.LimbVoxelPos(id, root, 0);
      const IVec3 b{ifloor(at.x), ifloor(at.y), ifloor(at.z)};
      for (int dy = -8; dy <= 8; dy++)
        for (int dz = -3; dz <= 3; dz++)
          for (int dx = -3; dx <= 3; dx++) {
            const IVec3 cc{b.x + dx, b.y + dy, b.z + dz};
            if (!c.world.CellInWindow(cc)) continue;
            if (cellOps.size() >= kMaxCellOpsPerTick) break;
            cellOps.push_back({World::SlotCellIndex(cc),
                               PackVoxNew(mFire, 7u) | kCellOpIfAir});
            fireOpsPushed++;
          }
      // ATTRIBUTION: does the CPU mirror the burn pass reads see the fire?
      {
        const IVec3 probe{b.x + 2, b.y, b.z};
        const CachedChunk* cch =
            c.world.Cached({probe.x >> 4, probe.y >> 4, probe.z >> 4});
        probeAt = probe;
        if (cch && cch->voxels.size() == kChunkVol) {
          probeMat = cch->voxels[((uint32_t)(probe.z & 15) * kChunk +
                                  (uint32_t)(probe.y & 15)) * kChunk +
                                 (uint32_t)(probe.x & 15)] & 0xFFFu;
          probeVer = cch->version;
          if (probeMat == mFire) fireSeen++;
        } else {
          probeMat = 0xFFFFFFFFu;
        }
      }
      if (mobs.LimbBody(id, dryLimb))
        dryBurningPeak =
            std::max(dryBurningPeak, mobs.LimbBurningCount(id, dryLimb));
      // (The debris island scan is what RE-FETCHES a dirty chunk the mirror
      // already holds; the real tick runs it, phase J.)
      fireTicker.chunk = IVec3{b.x >> 4, b.y >> 4, b.z >> 4};
      fireTicker(pre);
    }
    wetSeared = mobs.LimbBody(id, root) ? searedOn(root) : 0u;
    drySeared = mobs.LimbBody(id, dryLimb) ? searedOn(dryLimb) : 0u;
  }
  const bool wetOk =
      mWater && mFire && dryLimb >= 0 && drySeared > 0 &&
      (double)wetSeared <=
          BaselineNumber("bodyCoatWetSearFrac", 0.02) * (double)drySeared;
  RecordObserved("bodyCoatWetSeared", (double)wetSeared);
  RecordObserved("bodyCoatDrySeared", (double)drySeared);

  mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  const bool ok = ledgerOk && holdOk && decayOk && depositOk && printOk && wetOk;
  detail = Format(
      "%s/%s%s: in fire, wet root seared %u (want <= 2%% of dry) vs dry control %s seared %u (want >0) [fire ops %u, mirror saw fire %u/%d ticks (last: mat %d ver %u at (%d,%d,%d), tick %u), control burning peak %u]; "
      "ledger top = mat %u (want %u, blood %u) over %u voxels, "
      "frac %.4f, body sum %u >= limb sum %u, unknown tag %.2f; "
      "stained above the authored decayFloor %u: %u -> %u over %u ticks at "
      "the authored 20 s/level%s "
      "(floor %.0f%%), then -> %u over %u ticks at 2 ticks/level "
      "(cap %.0f%%); deposit %s on stone at (%d,%d,%d): word %08x -> %08x, "
      "stain type %u amount %u; footfall on %s: %u voxels soaked (ledger %u), "
      "%u droplet(s) down after %u plant(s), %u floor cell(s) at y%d newly "
      "bloodied (amount %u), sole ledger %u -> %u",
      t.defName.c_str(), t.limbName.c_str(), pinned ? "" : " (NOT pinned)",
      wetSeared, dryLimb >= 0 ? def.limbs[dryLimb].name.c_str() : "-",
      drySeared, fireOpsPushed, fireSeen, kFireTicks, (int)probeMat, probeVer,
      probeAt.x, probeAt.y, probeAt.z, simTick, dryBurningPeak,
      limbLedger.top[0].mat, coatMat, mBlood, limbLedger.voxels,
      (double)limbLedger.Frac(), bodyLedger.sumAmt, limbLedger.sumAmt,
      (double)noSuchTag, coatFloor, cutStain, heldStain, kHoldTicks,
      holdWindowClean ? "" : " (WINDOW CROSSED A DECAY PERIOD)", holdMin * 100.0,
      decayedStain, ranTicks, decayMax * 100.0, queued ? "queued" : "REFUSED",
      floorCell.x, floorCell.y, floorCell.z, depBefore, depAfter,
      VoxStainType(depAfter), VoxStainAmt(depAfter), footName.c_str(),
      footPainted, footSumPainted, shedDroplets, shedTries, printCells, printY,
      printAmt, footSumBefore, footSumAfter);
  if (!ledgerOk) detail += cutWhy;
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
  const Vec3 blastAt = limbAt + out * (radius * 0.8f);

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
    // THE REAL TICK (W2-O, test/tickrig.h). Both arms replay ticks 50000.. so
    // they differ by the healing rule, not by the clock.
    uint32_t tick = 49999;
    support::TickCursor ticker{c, tick, pchunk};
    for (int i = 0; i < kTicks && mobs.IsAlive(id); i++) ticker();
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
//   * THE DEATH. the brain's organHp (materials.json) charges every brain cell the bore
//     crosses, so the head reached hp 0 within a few ticks, and a vital
//     severable limb at zero went through Sever() — which DETACHES it before
//     it calls Die(). hp now kills in place (Mob::Damage, Mob::CarveLimb);
//     only geometry takes a head off.
//   * THE CORPSE. From the kill on, the head WAS debris, and the beam switched
//     to laserMeltRadius (a 2-voxel ball per tick, sized for rock). A corpse
//     is a dead Mob now (docs/PLAN_corpse_is_a_mob.md): its head is still its
//     own limb and the beam goes on carving it through the creature path,
//     exactly as it did while it lived; loose dead flesh still melts at the
//     carve radius (session.cpp phase C).
//
// The beam is the one session.cpp fires, step for step: CastRayBody, then
// Damage + CarveLimbRadial on a creature's limb (alive or dead), MeltBodyAt at
// the carve radius on loose dead flesh. Asserted: it killed (or the gate
// proved nothing), nothing was severed, and every body of the corpse is still
// jointed. Whether the bore
// came out the far side is recorded, not asserted — the ray reads the
// collider, which is coarser than the skin being bored.
Status GateLaserHead(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  // The fixture: the LARGEST vital, severable limb that bleeds — a head, by
  // property rather than by name. Largest for ChooseTarget's reason: a tiny
  // head is mostly neck, and a bore through it is a bore through the joint.
  // ...AND THAT HOLDS A BRAIN. The death half of the claim is the brain's
  // (the brain's materials.json organHp); a head with none is bored straight through
  // without dying. a411808 (2026-09-29) added the sylvan `deku`, whose wooden
  // head is the largest vital severable limb that bleeds (syrup) and has no
  // brain, so the property pick moved onto it and the gate measured a head
  // the claim was never about (10 hits, bored through at tick 10, alive).
  const uint32_t brainMat = mobs.MaterialIdNamed("brain");
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
      if (brainMat && mobs.LimbMaterialCount(sid, (int)li, brainMat) == 0)
        continue;
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
  // PINNED (the authored `dummy` profile, mobile: false), found by W2-O: the
  // claim is about a body standing where it was put, and an unprofiled one
  // only stood still under the hand-rolled ticks because they never submitted
  // a world, so it had no ground. On the real tick it walked (the head left the beam after three hits).
  mobs.SetMobBehavior(id, "dummy");
  const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
  const Vec3 centre = ax.anchor + ax.along * (ax.reach * 0.5f);
  const Vec3 dir = ax.edge;
  const Vec3 muzzle = centre - dir * 12.0f;
  const float range = 24.0f;
  const auto& tools = CurrentTuning().tools;

  uint32_t tick = 52999;
  // THE REAL TICK between beam hits (W2-O, test/tickrig.h).
  support::TickCursor ticker{c, tick, pchunk};
  // How far the target moved under the beam (rule 6): a beam that stops
  // hitting has either bored through or lost its target.
  const Vec3 origin0 = mobs.MobOrigin(id);
  float movedAtThrough = -1.0f;
  int deathTick = -1, throughTick = -1, hits = 0, severTick = -1,
      severLimb = -1;
  std::string cause;
  std::vector<ParticleSpawn> spawns;
  for (int i = 0; i < 300; i++) {
    float frac = 1.0f;
    const uint64_t hit = c.phys.CastRayBody(muzzle, dir, range, frac);
    if (!hit) {
      if (throughTick < 0) {
        throughTick = i;
        movedAtThrough = (mobs.MobOrigin(id) - origin0).len();
      }
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
    ticker();
    // Held on after the kill for a second, so the corpse half is exercised.
    if (deathTick >= 0 && i >= deathTick + 30) break;
  }
  const size_t severs = mobs.SeverEvents().size();
  // Every body of the corpse: its own limbs, and anything loose that came off
  // it (which has no joint and would fail the claim, as it should).
  int bodies = 0, jointed = 0;
  if (const Mob* dead = mobs.FindMobById(id))
    for (int li = 0; li < dead->LimbCount(); li++)
      if (const uint64_t h = mobs.LimbBody(id, li)) {
        bodies++;
        if (c.phys.JointCount(h) > 0) jointed++;
      }
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
      bodies,
      throughTick >= 0
          ? Format("tick %d (target moved %.1f vox by then)", throughTick,
                   movedAtThrough).c_str()
          : "no",
      cause.empty() ? "" : Format(" cause: %s", cause.c_str()).c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- heart -----------------------------------------------------------------
// A STAB INTO THE HEART KILLS; THE SAME STABS BESIDE IT DO NOT (owner,
// 2026-10-12; docs/PLAN_projectiles.md package A).
//
// The heart is a material (materials.json `heart`, baked into the chest by the
// torso's anatomy recipe) whose every lost cell costs its `organHp` flat --
// the brain's rule, and nothing more. So the claim is about volume: enough of
// the heart gone takes the torso's hp to 0 through the ordinary rule (vital
// limb at zero -> Die), and the same blows that miss it leave the creature
// standing.
//
// FIXTURE. The stock `human` (the body heart.organHp is calibrated against:
// 15% of its 25 heart cells takes its 180-hp torso to 0), else the authored
// def with the most heart in a vital limb. The SHIPPED dagger and sword,
// through the real MeleeSweepDamage (blade-wounds' sweep, point first along
// the blade -- a stab), from in front of the chest straight back along the
// torso's own model axis, at heartStabSpeed of melee.fullSpeed. Each stab is a
// new stroke; stab k lands at aim + a fixed pattern of small offsets across
// the heart's face (kPattern), so repeated stabs open fresh heart rather than
// re-boring one channel. The HEART arm aims at the heart's centroid; the
// BESIDE arm makes the SAME stabs (same pattern, same count the heart arm
// needed) shifted to the creature's right by the heart's own half-width plus
// the blade's slit plus a margin, clear of the heart.
//
// Asserted per blade: the heart arm dies within heartStabMax stabs and the
// death names the heart (Mob::OrganDeathCause: the organ charge took at least
// half the torso's hp, whichever blow then finished it); the beside arm, after
// as many stabs, is alive with its heart untouched. PRINTED: heart cells, cells lost per stab, the hp the
// organ charge took, the torso's hp, and what a stab costs beside the heart.
Status GateHeart(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const uint32_t heartMat = mobs.MaterialIdNamed("heart");
  if (!heartMat || heartMat >= c.mats.size()) {
    detail = "materials.json has no `heart`";
    return Status::Fail;
  }
  const float organHp = c.mats[heartMat].organHp;
  // ---- the fixture: the human, else the most heart in a vital limb --------
  Target t;
  const IVec3 probeSite = FixtureSite(c.world, 410);
  auto consider = [&](int d) {
    const MobDef& def = mobs.Defs()[(size_t)d];
    mobs.Reset();
    const uint64_t sid = mobs.Spawn(d, probeSite);
    if (!sid) return;
    for (size_t li = 0; li < def.limbs.size(); li++) {
      if (!def.limbs[li].vital || !mobs.LimbBody(sid, (int)li)) continue;
      const uint32_t n = mobs.LimbMaterialCount(sid, (int)li, heartMat);
      if (n <= t.atSpawn) continue;
      t.defIndex = d;
      t.limb = (int)li;
      t.atSpawn = n;
      t.defName = def.name;
      t.limbName = def.limbs[li].name;
    }
  };
  if (const int h = mobs.FindDef("human"); h >= 0 && (size_t)h < mobs.LoadedDefCount())
    consider(h);
  for (size_t d = 0; !t.valid() && d < mobs.LoadedDefCount(); d++)
    consider((int)d);
  mobs.Reset();
  if (!t.valid()) {
    detail = "no authored mob def has heart in a vital limb (anatomy recipe?)";
    return Status::Fail;
  }
  const ItemDef* blades[2] = {c.items.At(c.items.Find("dagger")),
                              c.items.At(c.items.Find("sword"))};
  const char* names[2] = {"dagger", "sword"};
  if (!blades[0] && !blades[1]) {
    detail = "the item library has no dagger and no sword";
    return Status::Skip;
  }
  const auto& g = CurrentTuning().gore;
  MeleeTuning mt;
  ApplyMeleeTuning(mt);
  const float speed = (float)BaselineNumber("heartStabSpeed", 0.8);
  const float step = mt.fullSpeed * kTickDt * speed;
  const int maxStabs = std::clamp((int)BaselineNumber("heartStabMax", 9), 1, 9);
  // Offsets across the heart's face, in quarters of its width (x) / height (y).
  static const float kPattern[9][2] = {{0, 0},   {0, 1},   {0, -1},
                                       {1, 0},   {-1, 0},  {1, 1},
                                       {-1, -1}, {1, -1},  {-1, 1}};

  struct Arm {
    bool ran = false, died = false;
    int stabs = 0, landed = 0;
    uint32_t heart0 = 0, heartLeft = 0;
    float hp0 = 0, hpEnd = 0;
    std::string cause, perStab;
  };
  // `shift` > 0 = the beside arm: every aim moved that far to the right.
  auto runArm = [&](const ItemDef& it, float shift, int stabs) -> Arm {
    Arm a;
    IVec3 pchunk{};
    const uint64_t id = SpawnTarget(c, t, 170, pchunk);
    const uint64_t wid = id ? mobs.Spawn(t.defIndex, FixtureSite(c.world, 225)) : 0;
    Mob* wielder = wid ? mobs.FindMobById(wid) : nullptr;
    MobSystem::MaterialSpot spot;
    if (!id || !wielder || !mobs.LimbMaterialSpot(id, t.limb, heartMat, spot)) {
      mobs.Reset();
      c.debris.Reset();
      return a;
    }
    a.ran = true;
    a.heart0 = spot.cells;
    a.hp0 = mobs.LimbHp(id, t.limb);
    const Vec3 right = spot.axis[0] * -1.0f, up = spot.axis[1];
    const Vec3 in = spot.axis[2] * -1.0f;   // from the front, toward the back
    const float w4 = (spot.hi[0] - spot.lo[0]) * 0.25f;
    const float h4 = (spot.hi[1] - spot.lo[1]) * 0.25f;
    const float L = 3.0f;   // world voxels of blade behind the point
    std::vector<ParticleSpawn> spawns;
    uint32_t before = a.heart0;
    for (int k = 0; k < stabs && mobs.IsAlive(id); k++) {
      const Vec3 tip1 = spot.centroid + right * shift +
                        spot.axis[0] * (kPattern[k][0] * w4) +
                        up * (kPattern[k][1] * h4);
      const Vec3 tip0 = tip1 - in * step;
      EdgeSweep sw;
      sw.aPrev = tip0 - in * L; sw.bPrev = tip0;
      sw.aNow = tip1 - in * L;  sw.bNow = tip1;
      // The flat faces up, so the slit runs ACROSS the chest (melee.cpp
      // BuildStrikeParts: across = travel x flat).
      sw.flatNow = up;
      sw.dt = kTickDt;
      sw.halfWidth = it.edgeHalfWidth;
      sw.carveBonus = it.carveBonus;
      sw.strike = it.strike;
      sw.heft = it.HeftFactor(g.woundHeftRef, g.woundHeftMax);
      sw.tick = 8100u + (uint32_t)k;
      std::vector<uint64_t> struck;   // a new stroke per stab
      sw.struck = &struck;
      sw.valid = true;
      spawns.clear();
      const EdgeSweepResult r = MeleeSweepDamage(sw, mt, *wielder, c.phys, mobs,
                                                 c.debris, c.world, spawns);
      if (r.bodiesHit > 0) a.landed++;
      a.stabs = k + 1;
      const uint32_t now = mobs.LimbBody(id, t.limb)
                               ? mobs.LimbMaterialCount(id, t.limb, heartMat)
                               : 0u;
      a.perStab += Format("%s-%u", k ? "," : "", before > now ? before - now : 0u);
      before = now;
    }
    a.heartLeft = mobs.LimbBody(id, t.limb)
                      ? mobs.LimbMaterialCount(id, t.limb, heartMat)
                      : 0u;
    a.hpEnd = mobs.LimbHp(id, t.limb);
    a.died = !mobs.IsAlive(id);
    if (a.died) a.cause = mobs.DeathCause(id);
    mobs.Reset();
    c.debris.Reset();
    return a;
  };

  bool ok = true;
  std::string why, line;
  for (int b = 0; b < 2; b++) {
    if (!blades[b]) continue;
    const ItemDef& it = *blades[b];
    const std::string nm = names[b];
    const Arm heart = runArm(it, 0.0f, maxStabs);
    // Clear of the heart: its extent to the right of the centroid, the
    // pattern's widest x offset, the slit's half-length across the blade
    // (BuildStrikeParts: radius x stabWidth), and a margin of a fine cell.
    MobSystem::MaterialSpot s0;
    float shift = 0.0f;
    {
      IVec3 pc{};
      const uint64_t id = SpawnTarget(c, t, 170, pc);
      if (id && mobs.LimbMaterialSpot(id, t.limb, heartMat, s0))
        shift = -s0.lo[0] + (s0.hi[0] - s0.lo[0]) * 0.25f +
                (it.edgeHalfWidth + it.carveBonus) * g.stabWidth + 0.25f;
      mobs.Reset();
      c.debris.Reset();
    }
    const int n = std::max(1, heart.stabs);
    const Arm beside = runArm(it, shift, n);
    const uint32_t lostH = heart.heart0 > heart.heartLeft ? heart.heart0 - heart.heartLeft : 0u;
    const uint32_t lostB = beside.heart0 > beside.heartLeft ? beside.heart0 - beside.heartLeft : 0u;
    RecordObserved(("heartStabsToKill_" + nm).c_str(), heart.died ? (double)heart.stabs : -1.0);
    RecordObserved(("heartCellsLost_" + nm).c_str(), (double)lostH);
    RecordObserved(("heartBesideHpLost_" + nm).c_str(), (double)(beside.hp0 - beside.hpEnd));
    auto fail = [&](const std::string& s) { ok = false; why += " | " + nm + ": " + s; };
    if (!heart.ran || !beside.ran) fail("spawn or heart probe refused");
    if (heart.landed == 0) fail("no stab landed on the chest");
    if (!heart.died) fail(Format("heart arm alive after %d stabs", heart.stabs));
    if (heart.died && heart.cause.find(c.mats[heartMat].name) == std::string::npos)
      fail("death does not name the heart (\"" + heart.cause + "\")");
    if (lostH == 0) fail("no heart cell lost");
    if (beside.landed == 0) fail("no beside stab landed");
    if (beside.died) fail("beside arm died (\"" + beside.cause + "\")");
    if (lostB != 0) fail(Format("beside stabs took %u heart cells", lostB));
    line += Format(
        "%s: heart %u cells, %s on stab %d of %d (heart lost %s = %u cells, "
        "%.0f%%, organ %.1f hp; torso %.1f -> %.1f%s); beside (%.2f vox right) "
        "%d stabs: heart lost %u, torso %.1f -> %.1f, %s. ",
        nm.c_str(), heart.heart0, heart.died ? "DIED" : "alive", heart.stabs,
        maxStabs, heart.perStab.c_str(), lostH,
        heart.heart0 ? 100.0 * lostH / heart.heart0 : 0.0, lostH * organHp,
        heart.hp0, heart.hpEnd,
        heart.cause.empty() ? "" : (", \"" + heart.cause + "\"").c_str(), shift,
        beside.stabs, lostB, beside.hp0, beside.hpEnd,
        beside.died ? "DIED" : "alive");
  }
  detail = Format("%s/%s, heart.organHp %.2f, %.2f speed: %s%s",
                  t.defName.c_str(), t.limbName.c_str(), organHp, speed,
                  line.c_str(), why.c_str());
  std::printf("heart: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
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
    // Not a long-haired character: its `hair` limb overlaps the head and so
    // brings twin cells of its own, which would win this by hair alone and
    // put the probe on a pair where one side is not tissue (IsBloodless
    // refuses the infection this gate paints with). No existing def has one.
    bool hair = false;
    for (const MobLimbDef& ld : def.limbs) hair |= ld.bloodless;
    if (hair) continue;
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
    // DIRECT PHASE CALLS ON PURPOSE (W2-O): "the only writer is this gate and
    // the sync; one tick of PreTick is what the claim is about" (above).
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

  // DIRECT PHASE CALLS ON PURPOSE (W2-O): the twin sync alone, as above.
  auto tickOnce = [&](uint32_t tick) {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    mobs.PreTick(tick, c.world, ops, cellOps, spawns);
    c.phys.Step(kTickDt);
    mobs.PostStep();
  };
  const MobDef& def = mobs.Defs()[t.defIndex];

  // ---- REBUILD KEEPS THE DIVERGENCE (a sever in the same tick) -------------
  // A change to a twin cell and a sever ELSEWHERE on the creature, both before
  // one sync. The sever changes which limbs have a body, so the sync rebuilds
  // its links -- and a rebuild that only re-baselines leaves the child's copy
  // pristine for good. The sever is the real Mob::Sever; the sync, its
  // rebuild and its propagation are the real BurnTick path.
  std::string rebuildRes = "not run";
  bool rebuildOk = false;
  {
    const uint32_t n4 = mobs.JointTwinCount(id);
    const Probe p4 = n4 > 2 ? probeAt(n4 / 4) : Probe{};
    // A LEAF that is in neither limb of the probed pair: severable, not
    // vital, nobody's parent. Its sever reshapes nothing the probe touches.
    int leaf = -1;
    for (int li = (int)def.limbs.size() - 1; li >= 0 && p4.a >= 0; li--) {
      if (li == p4.a || li == p4.b || li == def.rootLimb) continue;
      if (!def.limbs[li].severable || def.limbs[li].vital) continue;
      // Hair is appended last and so would be the first leaf met here; it is
      // not anatomy, so it is not the leaf this probe means (IsBloodless).
      if (def.limbs[li].bloodless) continue;
      bool parent = false;
      for (const auto& o : def.limbs) parent |= o.parent == def.limbs[li].name;
      if (parent) continue;
      leaf = li;
      break;
    }
    uint32_t ma = 0, mb = 0;
    uint16_t sa = 0, sb = 0;
    if (leaf >= 0 && mobs.LimbCellAt(id, p4.a, p4.rest, ma, sa) &&
        mobs.LimbCellAt(id, p4.b, p4.rest, mb, sb) && ma != rotMat) {
      mobs.SetLimbCellAt(id, p4.a, p4.rest, rotMat, sa);
      mobs.Sever(id, leaf);
      tickOnce(2001u);
      uint32_t seen = 0;
      uint16_t s4 = 0;
      rebuildOk = mobs.LimbCellAt(id, p4.b, p4.rest, seen, s4) && seen == rotMat;
      rebuildRes = Format("rebuild (sever %s + %s/%s cell) %s, child reads %u",
                          def.limbs[leaf].name.c_str(),
                          def.limbs[p4.a].name.c_str(),
                          def.limbs[p4.b].name.c_str(),
                          rebuildOk ? "KEEPS the change" : "LOST the change",
                          seen);
    } else {
      rebuildRes = "rebuild: no leaf limb / probe cell to use";
    }
  }

  // ---- A TWIN'S HOLE IS CARVED, NOT STRIPPED --------------------------------
  // Tombstone the PARENT's copies of one pair, one more than the CHILD's
  // FlushBurn batching threshold (max(kBurnRebuildFloor 12, n >> 6), mirrored
  // here); the sync removes the child's copies and flushes them. The child is
  // not burning and holds no burn index -- the case that used to take the
  // StripBurnTombstones fallback: a bare compaction, no hp charge, no collider
  // re-derive, no joint rule. Now it is a CarveLimb read off the tombstones,
  // so the child's hp moves (or the carve severs it -- also only a carve can).
  std::string carveRes = "not run";
  bool carveOk = false;
  {
    const uint32_t n3 = mobs.JointTwinCount(id);
    std::map<std::pair<int, int>, std::vector<IVec3>> byPair;
    for (uint32_t k = 0; k < n3; k++) {
      Probe p = probeAt(k);
      byPair[{p.a, p.b}].push_back(p.rest);
    }
    int a3 = -1, b3 = -1;
    uint32_t need = 0;
    for (auto& pr : byPair) {
      const uint32_t nb = mobs.LimbArtVoxelCount(id, pr.first.second);
      const uint32_t thr = std::max(12u, nb >> 6);
      if (pr.second.size() >= thr + 1 && (b3 < 0 || thr < need)) {
        a3 = pr.first.first;
        b3 = pr.first.second;
        need = thr + 1;
      }
    }
    if (b3 >= 0) {
      const std::vector<IVec3>& all = byPair[{a3, b3}];
      const std::vector<IVec3> rests(all.begin(), all.begin() + need);
      const float hpB0 = mobs.LimbHp(id, b3);
      for (const IVec3& r : rests) mobs.SetLimbCellAt(id, a3, r, 0, 0);
      tickOnce(2002u);
      uint32_t left = 0;
      for (const IVec3& r : rests) {
        uint32_t m = 0;
        uint16_t st2 = 0;
        if (mobs.LimbCellAt(id, b3, r, m, st2)) left++;
      }
      const bool attached = mobs.LimbBody(id, b3) != 0;
      const float hpB1 = mobs.LimbHp(id, b3);
      carveOk = left == 0 && (!attached || hpB1 < hpB0);
      carveRes = Format("twin hole on %s (%u cells, its threshold): %u child "
                        "copies left, hp %.4f -> %.4f%s (%s)",
                        def.limbs[b3].name.c_str(), need, left, (double)hpB0,
                        (double)hpB1, attached ? "" : ", severed",
                        carveOk ? "CARVED" : "NOT carved");
    } else {
      carveRes = "twin hole: no pair has as many links as its child's "
                 "flush threshold";
    }
  }

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
           ")" + buf + "; " + rebuildRes + "; " + carveRes;
  (void)m0;
  return matOk && coatOk && goneOk && moved == 0 && linksAfter == n - 1 &&
                 rebuildOk && carveOk
             ? Status::Pass
             : Status::Fail;
}

// ---------------------------------------------------------------------------
// acid-coat: a CORROSIVE coat eats the body it is on, and then it is spent
// ---------------------------------------------------------------------------
//
// Owner report 2026-09-23: pouring acid on a character neither showed nor
// dissolved anything. Acid had no stain block, so every coat write (pour,
// splash, contact) refused it, and a coat could not act on the voxel under it
// anyway. Now a coat whose material's rules rewrite body matter is evaluated
// against that voxel as a grid cell of it would be (mob.cpp BurnOneLimb,
// section 3). Claims, on one pinned creature, no world submit:
//   * THE MATERIAL IS WIRED: acid has a stain slot for bodies, NO GPU stain
//     type (bodyOnly: it never marks the ground) and a glow for the renderer.
//   * A COAT OF IT EATS: the acid-soaked limb loses voxels (or comes off) over
//     90 ticks, and the ledger reported it as corrosive.
//   * A COAT THAT IS NOT CORROSIVE DOES NOT: the control limb, soaked in blood
//     at the same amount, loses nothing.
//   * ACID TAKES A BLOODIED VOXEL: acid poured over blood replaces it
//     (CoatBeneath) rather than waiting to be heavier.
//   * IT IS SPENT: the acid coat is gone within 1,200 ticks, and the limb
//     then stops losing voxels (bounded by what was poured -- rule 2).
//   * THE BONE IT BARES IS BLOODY (2026-09-23): acid cannot eat bone, and the
//     bone it uncovers wears the creature's blood at bone's `bareBlood` odds,
//     so of the bone a clean pour newly exposes, bareBlood's share (+-20
//     points) wears blood.
Status GateAcidCoat(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  constexpr int kInset = 300;
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, kInset));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb";
    return Status::Fail;
  }
  uint32_t mAcid = 0, mBlood = 0, mBone = 0;
  for (size_t i = 0; i < c.mats.size(); i++) {
    if (c.mats[i].name == "acid") mAcid = (uint32_t)i;
    if (c.mats[i].name == "blood") mBlood = (uint32_t)i;
    if (c.mats[i].name == "bone") mBone = (uint32_t)i;
  }
  if (!mAcid || !mBlood || !mBone) {
    detail = "acid, blood or bone material missing";
    return Status::Fail;
  }
  const MaterialDef& acid = c.mats[mAcid];
  const bool wired = acid.stainSlot != 0 &&
                     (acid.gpu.stainPack & kStainPackTypeMask) == 0 &&
                     acid.coatGlow > 0 && mobs.StainTypeOf(mAcid) != 0;

  IVec3 pchunk{};
  const uint64_t id = SpawnTarget(c, t, kInset, pchunk);
  if (!id) {
    detail = "spawn refused";
    return Status::Fail;
  }
  mobs.SetMobBehavior(id, "dummy");
  const MobDef& def = mobs.Defs()[t.defIndex];
  // The control: the non-vital, non-root limb with the most voxels that is
  // not the target -- acid does not cross a joint, so it cannot reach it.
  int ctl = -1;
  uint32_t ctlN = 0;
  for (size_t li = 0; li < def.limbs.size(); li++) {
    if ((int)li == t.limb || (int)li == def.rootLimb || def.limbs[li].vital)
      continue;
    if (!mobs.LimbBody(id, (int)li)) continue;
    const uint32_t n = mobs.LimbSkinVoxelCount(id, (int)li);
    if (n > ctlN) { ctlN = n; ctl = (int)li; }
  }

  // THE REAL TICK (W2-O, test/tickrig.h): the acid eats a body standing in a
  // world, and whatever it drips reaches that world.
  uint32_t simTick = 35000;
  support::TickCursor poseTicker{c, simTick, pchunk};
  auto poseTick = [&]() { poseTicker(); };
  for (int i = 0; i < 10; i++) poseTick();

  // Blood first on BOTH limbs, then acid on the target only: the acid has to
  // displace the blood to land at all (claim 4).
  constexpr uint32_t kAmt = 6;
  const uint32_t n0 = mobs.LimbSkinVoxelCount(id, t.limb);
  const uint32_t c0 = ctl >= 0 ? mobs.LimbSkinVoxelCount(id, ctl) : 0u;
  mobs.SoakLimb(id, t.limb, mBlood, 15, simTick);
  if (ctl >= 0) mobs.SoakLimb(id, ctl, mBlood, kAmt, simTick);
  const uint32_t acidOn = mobs.SoakLimb(id, t.limb, mAcid, kAmt, simTick);
  const uint32_t acidCoat = mobs.LimbCoatMatCount(id, t.limb, mAcid, 1);
  const LimbCoat* led = mobs.LimbCoatOf(id, t.limb);
  const uint32_t corrosive = led ? led->corrosive : 0u;

  std::string trace;
  for (int i = 0; i < 90 && mobs.LimbBody(id, t.limb); i++) {
    poseTick();
    // voxels/acid-coated, every 6 ticks: the shape of the bite over time.
    if (i % 6 == 0)
      trace += std::to_string(mobs.LimbSkinVoxelCount(id, t.limb)) + "/" +
               std::to_string(mobs.LimbCoatMatCount(id, t.limb, mAcid, 1)) + " ";
  }
  const bool severed = mobs.LimbBody(id, t.limb) == 0;
  const uint32_t n90 = severed ? 0u : mobs.LimbSkinVoxelCount(id, t.limb);
  const uint32_t c90 = ctl >= 0 ? mobs.LimbSkinVoxelCount(id, ctl) : 0u;
  const bool eats = severed || n90 + 32 < n0;
  const bool ctlKept = ctl < 0 || c90 == c0;

  // Spent: run until no acid is left on the limb (or it is gone), then 30
  // more ticks must take nothing.
  uint32_t spentAt = 0;
  for (int i = 0; i < 1200 && mobs.LimbBody(id, t.limb); i++) {
    poseTick();
    if (mobs.LimbCoatMatCount(id, t.limb, mAcid, 1) == 0) {
      spentAt = (uint32_t)i + 1;
      break;
    }
  }
  const bool gone = mobs.LimbBody(id, t.limb) == 0;
  // ATTRIBUTION for a coat that will not go: which of the acid cells left are
  // joint twins (Mob::SyncJointTwins), and what the OTHER copy wears there.
  std::string residue;
  if (!gone && mobs.LimbCoatMatCount(id, t.limb, mAcid, 1) > 0) {
    const uint32_t nt = mobs.JointTwinCount(id);
    uint32_t onTwin = 0;
    for (uint32_t k = 0; k < nt; k++) {
      int a = -1, b = -1;
      IVec3 r{};
      if (!mobs.JointTwinAt(id, k, a, b, r)) break;
      if (a != t.limb && b != t.limb) continue;
      const int other = a == t.limb ? b : a;
      uint32_t mm = 0, mo = 0;
      uint16_t sm = 0, so = 0;
      const bool hm = mobs.LimbCellAt(id, t.limb, r, mm, sm);
      const bool ho = mobs.LimbCellAt(id, other, r, mo, so);
      if (!hm || (sm & 0xFFFu) != (mAcid & 0xFFFu) || (sm >> 12) == 0) continue;
      if (onTwin++ < 3)
        residue += Format(" [%s side, (%d,%d,%d) mat %u coat 0x%04x | %s %s mat %u coat 0x%04x]",
                          a == t.limb ? "parent" : "child", r.x, r.y, r.z, mm,
                          sm, def.limbs[other].name.c_str(),
                          ho ? "has" : "LACKS", mo, so);
    }
    residue = Format(" | residue: %u acid cells, %u on twin links",
                     mobs.LimbCoatMatCount(id, t.limb, mAcid, 1), onTwin) +
              residue + mobs.LimbCoatResidue(id, t.limb, mAcid);
  }
  const uint32_t nSpent = gone ? 0u : mobs.LimbSkinVoxelCount(id, t.limb);
  for (int i = 0; i < 30 && !gone; i++) poseTick();
  const uint32_t nAfter = gone ? 0u : mobs.LimbSkinVoxelCount(id, t.limb);
  // ...and it is gone in a few seconds, not half a minute (spentAt counts
  // from tick 90 of the bite, so 60 here is 5 s after the pour).
  const bool spent = gone || (spentAt != 0 && spentAt <= 60 && nAfter == nSpent);

  // ---- THE BONE IT BARES IS BLOODY (2026-09-23) -----------------------------
  // A second creature and a CLEAN pour -- no blood first, because on a limb
  // this thin the surface soak already reaches bone and the acid then
  // replaces that coat, so the first creature cannot tell "blood the removal
  // laid" from "blood the soak laid". Exposed bone is counted before and after
  // (the joint faces at the lattice's ends are exposed from the start), and of
  // what the acid newly bared about bone's `bareBlood` share must be bloody.
  //
  // SEVERAL POURS, each bone cell judged ONCE, the pour that bared it (suite
  // triage 2026-10-01). Since acid dries off in a few seconds and eats flesh
  // at half its first rate (owner, 2026-09-23: coat.decay 0.15, acid +
  // tag:organic 250 -> 125) one 6-level pour on the arm bares 2-4 bone cells
  // -- under the 10 the ratio needs -- so the fixture pours again until
  // enough bone is bared, as a player would. A later pour lays acid over the
  // blood on bone bared earlier (CoatBeneath) and that acid then dries off,
  // so a single before/after count would undercount the blood: instead every
  // newly exposed bone cell is recorded, with its coat, right after the pour
  // that exposed it, and never re-read.
  uint32_t bare0 = 0, bareBlood0 = 0, bare1 = 0, bareBlood1 = 0;
  uint32_t baredN = 0, baredBlood = 0, pours = 0;
  bool bareRan = false;
  const float bareOdds = c.mats[mBone].bareBlood;
  if (const uint64_t id2 = SpawnTarget(c, t, kInset, pchunk)) {
    mobs.SetMobBehavior(id2, "dummy");
    for (int i = 0; i < 10; i++) poseTick();
    bare0 = mobs.LimbExposedMatCount(id2, t.limb, mBone, mBlood, &bareBlood0);
    auto cellKey = [](int x, int y, int z) {
      return ((uint64_t)(uint16_t)x << 32) | ((uint64_t)(uint16_t)y << 16) |
             (uint64_t)(uint16_t)z;
    };
    // Exposed bone cells of the limb now, with whether each wears blood.
    auto exposedBone = [&](std::unordered_map<uint64_t, bool>& out) {
      out.clear();
      const std::vector<PrefabVoxel> lat = mobs.LimbLattice(id2, t.limb);
      std::unordered_set<uint64_t> occ;
      occ.reserve(lat.size() * 2);
      for (const PrefabVoxel& v : lat) occ.insert(cellKey(v.x, v.y, v.z));
      static const int kD[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                   {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
      for (const PrefabVoxel& v : lat) {
        if ((v.material & 0xFFFu) != (mBone & 0xFFFu)) continue;
        bool open = false;
        for (const auto& d : kD)
          if (!occ.count(cellKey(v.x + d[0], v.y + d[1], v.z + d[2]))) {
            open = true;
            break;
          }
        if (open)
          out[cellKey(v.x, v.y, v.z)] =
              BodyStainAmt(v.stain) && BodyStainMat(v.stain) == mBlood;
      }
    };
    std::unordered_map<uint64_t, bool> seen, now;
    exposedBone(seen);  // the joint faces, exposed before any acid
    const int kMaxPours = (int)BaselineNumber("acidCoatBarePours", 6);
    for (int pour = 0; pour < kMaxPours && mobs.LimbBody(id2, t.limb); pour++) {
      pours++;
      mobs.SoakLimb(id2, t.limb, mAcid, kAmt, simTick);
      for (int i = 0; i < 400 && mobs.LimbBody(id2, t.limb); i++) {
        poseTick();
        if (mobs.LimbCoatMatCount(id2, t.limb, mAcid, 1) == 0) break;
      }
      if (!mobs.LimbBody(id2, t.limb)) break;
      exposedBone(now);
      for (const auto& kv : now) {
        if (seen.count(kv.first)) continue;
        seen.emplace(kv.first, kv.second);
        baredN++;
        if (kv.second) baredBlood++;
      }
      bareRan = true;
      if (baredN >= 10) break;
    }
    if (mobs.LimbBody(id2, t.limb))
      bare1 = mobs.LimbExposedMatCount(id2, t.limb, mBone, mBlood, &bareBlood1);
  }
  const float baredFrac = baredN ? (float)baredBlood / (float)baredN : 0.0f;
  const bool boneBloodied = bareRan && baredN >= 10 &&
                            std::fabs(baredFrac - bareOdds) <= 0.2f;

  char buf[640];
  std::snprintf(buf, sizeof buf,
                "%s.%s: wired %d | acid over blood %u marked, %u coated, "
                "ledger corrosive %u | voxels %u -> %u in 90 ticks%s | control "
                "%s %u -> %u | acid spent after %u more ticks, then %u -> %u | "
                "clean pours x%u: exposed bone %u -> %u, newly bared %u of "
                "which %u bloody (%.0f%%, bareBlood %.0f%%)%s",
                t.defName.c_str(), t.limbName.c_str(), wired ? 1 : 0, acidOn,
                acidCoat, corrosive, n0, n90, severed ? " (severed)" : "",
                ctl >= 0 ? def.limbs[ctl].name.c_str() : "-", c0, c90, spentAt,
                nSpent, nAfter, pours, bare0, bare1, baredN, baredBlood,
                100.0f * baredFrac, 100.0f * bareOdds,
                bareRan ? "" : " (did not run)");
  detail = std::string(buf) + residue + " | trace " + trace;
  const bool ok = wired && acidOn > 0 && acidCoat > 0 && corrosive > 0 &&
                  eats && ctlKept && spent && boneBloodied;
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// lava-oil-coat: a HOT coat burns what it is on, a FUEL coat flashes
// ---------------------------------------------------------------------------
//
// Owner report 2026-09-23: lava poured on yourself left no mark and did
// nothing; oil did not stain at all. On the human's forearms, pose ticks only:
//   * LAVA: the coat lands and is drawn (a stain slot, no GPU ground type,
//     glow), sets the forearm ALIGHT (burning voxels) and DISINTEGRATES it
//     (voxels gone) within 90 ticks, and cools off by itself.
//   * OIL: the coat lands and is INERT on its own (30 ticks: nothing burns,
//     the coat stays). Then lava on BOTH hands: the fire crosses each wrist,
//     and the oiled forearm must burn far more than the clean one.
Status GateLavaOilCoat(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  constexpr int kInset = 300;
  int di = -1;
  for (size_t d = 0; d < mobs.Defs().size(); d++)
    if (mobs.Defs()[d].name == "human") di = (int)d;
  uint32_t mLava = 0, mOil = 0;
  for (size_t i = 0; i < c.mats.size(); i++) {
    if (c.mats[i].name == "lava") mLava = (uint32_t)i;
    if (c.mats[i].name == "oil") mOil = (uint32_t)i;
  }
  if (di < 0 || !mLava || !mOil) {
    detail = "human def, lava or oil missing";
    return Status::Fail;
  }
  const MobDef& def = mobs.Defs()[di];
  auto limbNamed = [&](const char* n) {
    for (size_t i = 0; i < def.limbs.size(); i++)
      if (def.limbs[i].name == n) return (int)i;
    return -1;
  };
  const int foreL = limbNamed("armL.L"), foreR = limbNamed("armL.R"),
            handL = limbNamed("hand.L"), handR = limbNamed("hand.R");
  if (foreL < 0 || foreR < 0 || handL < 0 || handR < 0) {
    detail = "human has no armL.L/armL.R/hand.L/hand.R";
    return Status::Fail;
  }
  const MaterialDef& lava = c.mats[mLava];
  const MaterialDef& oil = c.mats[mOil];
  const bool wired = lava.stainSlot != 0 &&
                     (lava.gpu.stainPack & kStainPackTypeMask) == 0 &&
                     lava.coatGlow > 0 && mobs.StainTypeOf(mLava) != 0 &&
                     oil.stainSlot != 0 &&
                     (oil.gpu.stainPack & kStainPackTypeMask) != 0 &&
                     mobs.StainTypeOf(mOil) != 0;
  Target t;
  t.defIndex = di;
  t.limb = foreL;

  // THE REAL TICK (W2-O, test/tickrig.h), the mirror on the fixture's chunk.
  uint32_t simTick = 37000;
  IVec3 pchunk{};
  support::TickCursor poseTicker{c, simTick, pchunk};
  auto poseTick = [&]() {
    poseTicker.chunk = pchunk;
    poseTicker();
  };

  // ---- LAVA -----------------------------------------------------------------
  uint64_t id = SpawnTarget(c, t, kInset, pchunk);
  if (!id) {
    detail = "spawn refused";
    return Status::Fail;
  }
  mobs.SetMobBehavior(id, "dummy");
  for (int i = 0; i < 10; i++) poseTick();
  const uint32_t n0 = mobs.LimbSkinVoxelCount(id, foreL);
  const uint32_t lavaOn = mobs.SoakLimb(id, foreL, mLava, 10, simTick);
  const uint32_t lavaCoat = mobs.LimbCoatMatCount(id, foreL, mLava, 1);
  uint32_t burnPeak = 0;
  std::string trace;
  for (int i = 0; i < 90 && mobs.LimbBody(id, foreL); i++) {
    poseTick();
    burnPeak = std::max(burnPeak, mobs.LimbBurningCount(id, foreL));
    if (i % 10 == 0)
      trace += std::to_string(mobs.LimbSkinVoxelCount(id, foreL)) + "/" +
               std::to_string(mobs.LimbBurningCount(id, foreL)) + "/" +
               std::to_string(mobs.LimbCoatMatCount(id, foreL, mLava, 1)) + " ";
  }
  const bool severed = mobs.LimbBody(id, foreL) == 0;
  const uint32_t n90 = severed ? 0u : mobs.LimbSkinVoxelCount(id, foreL);
  const bool eats = severed || n90 + 32 < n0;
  uint32_t cooledAt = 0;
  for (int i = 0; i < 900 && mobs.LimbBody(id, foreL); i++) {
    poseTick();
    if (mobs.LimbCoatMatCount(id, foreL, mLava, 1) == 0) {
      cooledAt = (uint32_t)i + 1;
      break;
    }
  }
  const bool cooled = mobs.LimbBody(id, foreL) == 0 || cooledAt != 0;

  // ---- OIL ------------------------------------------------------------------
  id = SpawnTarget(c, t, kInset, pchunk);
  if (!id) {
    detail = "second spawn refused";
    return Status::Fail;
  }
  mobs.SetMobBehavior(id, "dummy");
  for (int i = 0; i < 10; i++) poseTick();
  const uint32_t oilOn = mobs.SoakLimb(id, foreR, mOil, 12, simTick);
  const uint32_t oil0 = mobs.LimbCoatMatCount(id, foreR, mOil, 1);
  for (int i = 0; i < 30; i++) poseTick();
  const uint32_t oil30 = mobs.LimbCoatMatCount(id, foreR, mOil, 1);
  const uint32_t idleBurn = mobs.LimbBurningCount(id, foreR);
  const bool inert = idleBurn == 0 && oil30 * 10 >= oil0 * 9;
  mobs.SoakLimb(id, handL, mLava, 15, simTick);
  mobs.SoakLimb(id, handR, mLava, 15, simTick);
  uint64_t burnOiled = 0, burnClean = 0;  // burning voxel-ticks
  for (int i = 0; i < 150; i++) {
    poseTick();
    burnOiled += mobs.LimbBurningCount(id, foreR);
    burnClean += mobs.LimbBurningCount(id, foreL);
  }
  const uint32_t oilLeft = mobs.LimbCoatMatCount(id, foreR, mOil, 1);
  const bool flashes = burnOiled > 0 && burnOiled >= 2 * burnClean + 50 &&
                       oilLeft < oil30;

  char buf[640];
  std::snprintf(buf, sizeof buf,
                "wired %d | LAVA %u marked, %u coated, forearm %u -> %u in 90 "
                "ticks%s, burning peak %u, cooled after %u more | OIL %u marked, "
                "%u coated, %u after 30 idle ticks, %u burning idle | lava on both "
                "hands, 150 ticks: oiled forearm %llu burning voxel-ticks vs clean "
                "%llu, oil %u -> %u",
                wired ? 1 : 0, lavaOn, lavaCoat, n0, n90, severed ? " (severed)" : "",
                burnPeak, cooledAt, oilOn, oil0, oil30, idleBurn,
                (unsigned long long)burnOiled, (unsigned long long)burnClean, oil30,
                oilLeft);
  detail = std::string(buf) + " | lava trace vox/burning/coated " + trace;
  const bool ok = wired && lavaOn > 0 && lavaCoat > 0 && burnPeak > 0 && eats &&
                  cooled && oilOn > 0 && inert && flashes;
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// severed-hand: a part that comes off keeps what is attached to it
// ---------------------------------------------------------------------------
//
// Owner report 2026-09-23: acid from a flask melted the player's forearm off
// and the hand stayed where it was, floating at a wrist that had gone. Two
// faults, one claim each, on the human's left arm, pose ticks only:
//   * SPLIT: a ball carved through the middle of the forearm parts it; the
//     elbow end stays the limb and the wrist end leaves. The hand must leave
//     WITH the wrist end — off the rig and jointed to a body that is not.
//     (Before: nothing asked what was seated in the piece that left.)
//   * WHOLE: severing the upper arm keeps the chain jointed — forearm to upper
//     arm, hand to forearm — and 40 ticks later the hand is still within
//     reach of the forearm. (Before: DetachLimb destroyed every child joint,
//     so a cut-off arm landed as three unrelated bodies.)
// The acid run is the owner's own stroke (the pour brush on the side of the
// forearm). Only "a hand it took off is on a piece" is asserted there: WHETHER
// the acid parts the forearm depends on coat depth tuning, and both routes it
// can take (split, collapse) are pinned by the two claims above.
Status GateSeveredHand(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  constexpr int kInset = 300;
  int di = -1;
  for (size_t d = 0; d < mobs.Defs().size(); d++)
    if (mobs.Defs()[d].name == "human") di = (int)d;
  if (di < 0) {
    detail = "no human def";
    return Status::Fail;
  }
  const MobDef& def = mobs.Defs()[di];
  auto limbNamed = [&](const char* n) {
    for (size_t i = 0; i < def.limbs.size(); i++)
      if (def.limbs[i].name == n) return (int)i;
    return -1;
  };
  const int upper = limbNamed("armU.L"), fore = limbNamed("armL.L"),
            hand = limbNamed("hand.L");
  if (upper < 0 || fore < 0 || hand < 0) {
    detail = "human has no armU.L/armL.L/hand.L";
    return Status::Fail;
  }
  Target t;
  t.defIndex = di;
  t.limb = fore;

  // THE REAL TICK (W2-O, test/tickrig.h): a piece that comes off falls in the
  // world the game runs, not in two phases of it.
  uint32_t simTick = 36000;
  IVec3 pchunkT{};
  support::TickCursor poseTicker{c, simTick, pchunkT};
  auto poseTick = [&]() {
    poseTicker.chunk = pchunkT;
    poseTicker();
  };
  // Is `body` jointed to something, and is that something off the rig?
  auto rigHas = [&](uint64_t id, uint64_t h) {
    for (size_t i = 0; i < def.limbs.size(); i++)
      if (h && mobs.LimbBody(id, (int)i) == h) return true;
    return false;
  };
  auto jointedTo = [&](uint64_t body, uint64_t other) {
    std::vector<Physics::BodyJoint> js;
    c.phys.JointsOn(body, js);
    for (const auto& j : js)
      if (other == 0 ? j.other != 0 : j.other == other) return j.other;
    return (uint64_t)0;
  };
  auto dist = [&](uint64_t a, uint64_t b) {
    BodyTransform xa{}, xb{};
    if (!c.phys.GetTransform(a, xa) || !c.phys.GetTransform(b, xb)) return 1e9f;
    return (xa.pos - xb.pos).len();
  };

  // ---- SPLIT ----------------------------------------------------------------
  IVec3& pchunk = pchunkT;   // the fixture chunk the pose ticks centre on
  uint64_t id = SpawnTarget(c, t, kInset, pchunk);
  if (!id) {
    detail = "spawn refused";
    return Status::Fail;
  }
  mobs.SetMobBehavior(id, "dummy");
  for (int i = 0; i < 10; i++) poseTick();
  const uint64_t handBody = mobs.LimbBody(id, hand);
  const LimbAxis ax = MeasureLimb(mobs, id, fore);
  const uint32_t fore0 = mobs.LimbVoxelCount(id, fore);
  bool splitOk = false, foreStayed = false, handOff = false, onRig = false;
  uint64_t splitPartner = 0;
  float splitGap = -1.0f;
  if (handBody && ax.valid) {
    // A SLAB, not a ball: small carves across the cross-section at mid-length,
    // so the forearm parts in two without losing enough to collapse (a ball
    // big enough to cut through a forearm eats most of it). The limb's handle
    // changes on every rebuild, so it is re-read per carve; stops the moment
    // the hand leaves or the forearm does.
    const Vec3 mid = ax.anchor + ax.along * (0.5f * ax.reach);
    for (int u = -4; u <= 4 && mobs.LimbBody(id, hand) && mobs.LimbBody(id, fore); u++)
      for (int v = -4; v <= 4 && mobs.LimbBody(id, hand) && mobs.LimbBody(id, fore); v++) {
        std::vector<ParticleSpawn> cs;
        mobs.CarveLimbRadial(mobs.LimbBody(id, fore),
                             mid + ax.edge * (0.25f * (float)u) +
                                 ax.travel * (0.25f * (float)v),
                             0.3f, /*ragged=*/false, /*eject=*/false, c.world, cs);
      }
    for (int i = 0; i < 3; i++) poseTick();
    foreStayed = mobs.LimbBody(id, fore) != 0;
    handOff = mobs.LimbBody(id, hand) == 0;
    splitPartner = jointedTo(handBody, 0);
    onRig = rigHas(id, splitPartner);
    // The forearm must have STAYED, or this was a whole-limb sever (the WHOLE
    // claim's case) and the split rule was never asked.
    splitOk = foreStayed && handOff && splitPartner != 0 && !onRig;
    for (int i = 0; i < 40; i++) poseTick();
    if (splitPartner) splitGap = dist(handBody, splitPartner);
  }
  const uint32_t fore1 = foreStayed ? mobs.LimbVoxelCount(id, fore) : 0u;

  // ---- WHOLE ----------------------------------------------------------------
  id = SpawnTarget(c, t, kInset, pchunk);
  if (!id) {
    detail = "respawn refused";
    return Status::Fail;
  }
  mobs.SetMobBehavior(id, "dummy");
  for (int i = 0; i < 10; i++) poseTick();
  const uint64_t bU = mobs.LimbBody(id, upper), bL = mobs.LimbBody(id, fore),
                 bH = mobs.LimbBody(id, hand);
  mobs.Sever(id, upper);
  const bool chainOff = mobs.LimbBody(id, upper) == 0 &&
                        mobs.LimbBody(id, fore) == 0 && mobs.LimbBody(id, hand) == 0;
  const bool foreToUpper = jointedTo(bL, bU) != 0;
  const bool handToFore = jointedTo(bH, bL) != 0;
  for (int i = 0; i < 40; i++) poseTick();
  const float wholeGap = dist(bH, bL);
  const bool stillJointed = jointedTo(bH, bL) != 0;
  // "Within reach": the forearm's length plus a voxel. A hand that fell as its
  // own body is free to roll anywhere; one on its joint cannot.
  const bool wholeOk = chainOff && foreToUpper && handToFore && stillJointed &&
                       wholeGap < ax.reach + 1.5f;

  // ---- ACID (reported) --------------------------------------------------------
  uint32_t mAcid = 0;
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == "acid") mAcid = (uint32_t)i;
  std::string acid = "no acid material";
  bool acidOk = true;  // a hand the acid took off must be on a piece
  if (mAcid) {
    id = SpawnTarget(c, t, kInset, pchunk);
    if (id) {
      mobs.SetMobBehavior(id, "dummy");
      for (int i = 0; i < 10; i++) poseTick();
      const uint64_t aH = mobs.LimbBody(id, hand);
      // The owner's stroke: the portrait pour brush, held over the side of the
      // forearm at mid-length (a disc on the skin, not a whole-limb soak --
      // a soak thins the limb evenly and never parts it).
      const LimbAxis aax = MeasureLimb(mobs, id, fore);
      const Vec3 aMid = aax.anchor + aax.along * (0.5f * aax.reach);
      const MobSystem::BodyRayHit hit =
          mobs.PickBody(id, aMid + aax.travel * 20.0f, aax.travel * -1.0f, 40.0f);
      int foreGone = -1, handGone = -1;
      const uint32_t aF0 = mobs.LimbVoxelCount(id, fore);
      for (int i = 0; i < 600; i++) {
        if (i < 20 && hit.hit)
          mobs.PourOnBody(id, hit, aax.travel * -1.0f, 0.6f, mAcid, 3, simTick);
        poseTick();
        if (foreGone < 0 && !mobs.LimbBody(id, fore)) foreGone = i;
        if (handGone < 0 && !mobs.LimbBody(id, hand)) handGone = i;
      }
      const uint64_t partner = jointedTo(aH, 0);
      const uint32_t aF1 = mobs.LimbVoxelCount(id, fore);
      if (handGone >= 0) acidOk = partner != 0 && !rigHas(id, partner);
      char ab[200];
      std::snprintf(ab, sizeof ab,
                    "pour on %s, forearm %u -> %u, off at t%d, hand off at t%d, "
                    "hand jointed %s",
                    !hit.hit ? "NOTHING"
                             : (hit.limb == fore ? "the forearm" : "another limb"),
                    aF0, aF1, foreGone, handGone,
                    partner == 0 ? "to nothing"
                                 : (rigHas(id, partner) ? "to the RIG" : "to a piece"));
      acid = ab;
    }
  }

  char buf[640];
  std::snprintf(buf, sizeof buf,
                "split: forearm %u -> %u collider voxels (%s), hand %s, jointed "
                "to %s, gap after 40 ticks %.2f | whole: chain off %d, "
                "forearm-upper %d, hand-forearm %d, after 40 ticks jointed %d "
                "gap %.2f (reach %.2f) | acid: %s",
                fore0, fore1, foreStayed ? "stayed" : "came off",
                handOff ? "off" : "STILL ON THE RIG",
                splitPartner == 0 ? "nothing"
                                  : (onRig ? "the rig" : "a piece"),
                splitGap, chainOff ? 1 : 0, foreToUpper ? 1 : 0,
                handToFore ? 1 : 0, stillJointed ? 1 : 0, wholeGap, ax.reach,
                acid.c_str());
  detail = buf;
  return splitOk && wholeOk && acidOk ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// corpse-sleep: a settled corpse costs nothing, dries anyway, and a blow wakes it
// ---------------------------------------------------------------------------
//
// A corpse is a dead Mob now (docs/PLAN_corpse_is_a_mob.md) and keeps its rig,
// so rule 2 has a new population to be stated for: a body lying still in a
// settled world must not cost a terrain anchor, a read-back, an untunnel and
// a DriveWornShells activation of every garment on it every tick, forever.
//
// AT THE AUTHORED DRYING RATES (P2d). Even a corpse killed without a wound
// carries its anatomy's `blood` voxels, which decay out of the torso over a
// few hundred ticks and leave a blood COAT that dries at 20 s a level --
// ~15,000 ticks of "still drying", which held P1's corpse awake throughout. A
// passive coat no longer keeps a corpse awake (MobSystem::CoatDriesAsleep):
// it sleeps, and is visited only on the tick its next level is due
// (Mob::DeadDryVisit).
//
// The claims, on a DRESSED corpse (so there are garments to activate) SOAKED
// in blood after the kill (every limb, so the coat does not depend on the
// site), lying on a site with no liquid near it:
//   A. it falls ASLEEP (Mob::DeadAsleep) within corpseSleepMaxTicks of lying
//      down, with a blood coat still drying on it (a due tick) that P1's rule
//      would have held it awake for;
//   B. asleep, over corpseSleepWindow ticks it runs NO dead PostStep and
//      registers terrain anchors only on its keep-alive stride
//      (MobSystem::kDeadAnchorStride), and no body of it is active in Jolt;
//   D. it STAYS asleep to corpseSleepDryTicks while its coat dries: at least
//      one drying visit, the summed coat falls, and on EVERY tick it slept
//      through, the awake stain pass run as a shadow
//      (MobSystem::ShadowStainTick) writes nothing -- so the skipped ticks are
//      exactly the ones an awake corpse spends writing nothing, and the coat
//      is voxel for voxel what continuous drying leaves (the visit tick IS
//      the awake call);
//   C. a CUT wakes it at once — a sleeping corpse is still flesh — and the
//      cut takes voxels.
Status GateCorpseSleep(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  PrepareWorld(c);
  DeadFixture f(c);
  // A garment, so the asleep claim covers DriveWornShells' activations too.
  const ItemDef* wear = nullptr;
  for (const ItemDef& it : c.items.items)
    if (ItemKindIsWorn(it.kind)) {
      wear = &it;
      break;
    }
  const int maxTicks = (int)BaselineNumber("corpseSleepMaxTicks", 300);
  const int window = (int)BaselineNumber("corpseSleepWindow", 60);
  const int dryTicks = std::max(
      maxTicks + window + 1, (int)BaselineNumber("corpseSleepDryTicks", 1500));
  // Liquid cells the CPU mirror (what the contact pass reads) shows in the box
  // `reach` round `ctr` across, -4..+8 in y; `known` = every chunk of it was
  // cached. `low`/`ver`: the lowest such cell and its chunk's version.
  auto liquidAround = [&](IVec3 ctr, int reach, bool* known, IVec3* low,
                          uint32_t* ver) {
    uint32_t n = 0;
    if (known) *known = true;
    if (low) *low = IVec3{0, 1 << 30, 0};
    for (int y = ctr.y - 4; y <= ctr.y + 8; y++)
      for (int z = ctr.z - reach; z <= ctr.z + reach; z++)
        for (int x = ctr.x - reach; x <= ctr.x + reach; x++) {
          const CachedChunk* cc = c.world.Cached(IVec3{x >> 4, y >> 4, z >> 4});
          if (!cc || cc->voxels.size() != kChunkVol) {
            if (known) *known = false;
            continue;
          }
          const uint32_t m = cc->voxels[((uint32_t)(z & 15) * kChunk +
                                         (uint32_t)(y & 15)) * kChunk +
                                        (uint32_t)(x & 15)] & 0xFFFu;
          if (m == 0 || m >= c.mats.size() || c.mats[m].gpu.klass != CLASS_LIQUID)
            continue;
          n++;
          if (low && y < low->y) {
            *low = IVec3{x, y, z};
            if (ver) *ver = cc->version;
          }
        }
    return n;
  };
  constexpr int kDryReach = 12;
  int inset = 470;
  uint32_t siteLiquid = 0;
  uint32_t soaked = 0;  // voxels given a blood coat after the kill
  // DeadFixture's body, killed WITHOUT A WOUND: the root severed is a death
  // (Sever routes the root to Die) with no stump armed and no hp blow's bleed
  // budget, so nothing drips a pool for the corpse to lie in. A corpse lying
  // in its own blood is re-stained by the contact pass, which is not passive,
  // and is correctly awake while that goes on.
  {
    // A DRY SITE, CHOSEN OFF THE CPU MIRROR. Inset 470 is the shore of a
    // worldgen pond (556 liquid cells within 12 voxels): where the ragdoll
    // comes to rest is chaotic, and in a --verify list it rolled into the
    // water -- wet and in contact, correctly never asleep, and not this
    // claim. So the first candidate with no liquid within kDryReach of it
    // is used. The mirror follows the tick's player chunk (SubmitTick's
    // `pchunk`), so each candidate is primed with a few empty ticks first;
    // that also puts the mirror under the creature SpawnTarget poses, whose
    // own settling steps submit no tick.
    const int insets[] = {470, 350, 390, 430, 510};
    f.tick = 62980;
    for (int cand : insets) {
      const IVec3 site = FixtureSite(c.world, cand);
      f.pchunk = IVec3{site.x >> 4, site.y >> 4, site.z >> 4};
      bool known = false;
      uint32_t n = 0;
      for (int i = 0; i < 6 && !known; i++) {
        f.Step();
        n = liquidAround(site, kDryReach, &known, nullptr, nullptr);
      }
      inset = cand;
      siteLiquid = n;
      if (known && n == 0) break;
    }
    const Target t = ChooseTarget(c.mobs, FixtureSite(c.world, inset));
    f.id = t.valid() ? SpawnTarget(c, t, inset, f.pchunk) : 0;
    Mob* mob = f.id ? c.mobs.FindMobById(f.id) : nullptr;
    if (!mob || !mob->Def() || mob->Def()->rootLimb < 0) {
      detail = "spawn refused";
      return Status::Fail;
    }
    c.mobs.SetMobBehavior(f.id, "dummy");
    f.tick = 63000;
    if (wear != nullptr) {
      int home = -1;
      for (int sl = 0; sl < kEquipSlotCount; sl++)
        if (EquipSlotAccepts(sl, wear->kind)) { home = sl; break; }
      if (home >= 0) mob->WearItem(wear, home);
    }
    for (int i = 0; i < 8; i++) f.Step();
    f.root = mob->Def()->rootLimb;
    c.mobs.Sever(f.id, f.root);
    if (c.mobs.IsAlive(f.id)) {
      detail = "severing the root did not kill";
      c.mobs.Reset();
      return Status::Fail;
    }
    // A BLOODED CORPSE, on purpose: every limb soaked in its own blood at
    // kSoak. The anatomy blood that decays out of the torso leaves a coat on
    // some sites and none on others (it did at 470, not at 490 or 510), and
    // the claim is about the coat, so the coat is put there.
    constexpr uint32_t kSoak = 6;
    if (const Mob* m = c.mobs.FindMobById(f.id); m != nullptr && m->Def())
      for (int li = 0; li < m->LimbCount(); li++)
        soaked += c.mobs.SoakLimb(f.id, li, m->Def()->bleedMat, kSoak, f.tick);
    for (int i = 0; i < 20; i++) f.Step();
  }
  auto dead = [&]() { return c.mobs.FindMobById(f.id); };

  int asleepAt = -1;            // step it was first seen asleep
  uint32_t dueAtSleep = 0;      // its drying due tick then (0 = nothing)
  std::string stateAtSleep;     // DeadAwakeReason then
  std::string p1AtSleep;        // what P1's rule said then
  uint32_t awakeTicks = 0, wakes = 0, visits = 0;
  uint32_t shadowTicks = 0, shadowWrites = 0;
  std::string shadowFirst;      // the first shadow tick that wrote, named
  uint64_t anchors0 = 0, posts0 = 0, anchors = 0, posts = 0;
  uint32_t activeTicks = 0;
  bool windowAsleep = false;
  uint64_t sumAtSleep = 0, sumEnd = 0;
  std::string history;          // the trace (rule 6)
  bool was = false;
  for (int i = 0; i < dryTicks; i++) {
    const Mob* before = dead();
    const uint32_t dueBefore =
        before != nullptr && before->DeadAsleep() ? before->DeadDryDueTick() : 0u;
    f.Step();
    const Mob* m = dead();
    const bool asleep = m != nullptr && m->DeadAsleep();
    if (!asleep) awakeTicks++;
    if (was && !asleep) wakes++;
    if (asleep && asleepAt < 0) {
      asleepAt = i;
      dueAtSleep = m->DeadDryDueTick();
      const char* s = m->DeadAwakeReason();
      stateAtSleep = s ? s : "(null)";
      const char* p1 = c.mobs.DeadAwakeCriterionOf(f.id, false);
      p1AtSleep = p1 ? p1 : "quiet";
      c.mobs.CoatDigest(f.id, &sumAtSleep);
      anchors0 = c.mobs.DeadAnchorsTotal();
      posts0 = c.mobs.DeadPostStepsTotal();
      windowAsleep = true;
    } else if (asleep && was) {
      if (f.tick == dueBefore) {
        visits++;
      } else {
        // THE SHADOW: what the awake pass would have written on this tick
        // the corpse slept through. Not on a visit tick -- that one ran the
        // awake pass already, and a second run would dry it twice.
        shadowTicks++;
        const uint8_t w = c.mobs.ShadowStainTick(f.id, f.tick, c.world);
        if (w != 0) {
          if (shadowWrites == 0)
            shadowFirst = Format(" (first at tick %u: writers 0x%x)", f.tick,
                                 (unsigned)w);
          shadowWrites++;
        }
      }
    }
    // B: the window right after it fell asleep.
    if (asleepAt >= 0 && i > asleepAt && i <= asleepAt + window) {
      if (!asleep) windowAsleep = false;
      for (int li = 0; m && li < m->LimbCount(); li++)
        if (const uint64_t h = c.mobs.LimbBody(f.id, li); h && c.phys.IsActive(h)) {
          activeTicks++;
          break;
        }
      if (i == asleepAt + window) {
        anchors = c.mobs.DeadAnchorsTotal() - anchors0;
        posts = c.mobs.DeadPostStepsTotal() - posts0;
      }
    }
    // The trace: coat sum and state every 150 steps and at every wake.
    if (m != nullptr && (i % 150 == 0 || (was && !asleep))) {
      const char* w = m->DeadAwakeReason();
      uint64_t sum = 0;
      c.mobs.CoatDigest(f.id, &sum);
      history += Format(" t%d:%llu%s'%s'", i, (unsigned long long)sum,
                        was && !asleep
                            ? Format("(woke how %u, visit writers 0x%x limb %d)",
                                     (unsigned)m->DeadWokeHow(),
                                     (unsigned)m->DeadDryWokeBy(),
                                     m->DeadDryWokeLimb())
                                  .c_str()
                            : "",
                        w ? w : "quiet");
      // WHERE it is and what it wears: a corpse that fell into water is
      // wet and in contact, and awake for that, which is not this claim.
      if (i == 0 || i == 150) {
        Vec3 at{};
        m->LimbCentreWorld(f.root, at);
        const IVec3 site = FixtureSite(c.world, 470);
        // ...and where water could have come from: the world's rain this
        // tick (weather::SimRainWord's rain byte) and live MPM fluid.
        history += Format("(root at %.1f,%.1f,%.1f, site y %d, top coat mat %u, "
                          "world rain %u, mpm fluid live %u)",
                          at.x, at.y, at.z, site.y,
                          c.mobs.BodyCoat(f.id).top[0].mat,
                          weather::LastSimRainWord() & 0xFFu,
                          (unsigned)c.world.Snap().fluidLive);
        // ...and the liquid the CPU mirror shows round it (what the contact
        // pass reads): count, the lowest cell, its chunk's version.
        IVec3 low{};
        uint32_t ver = 0;
        const uint32_t wet = liquidAround(
            IVec3{(int)std::floor(at.x), (int)std::floor(at.y),
                  (int)std::floor(at.z)},
            kDryReach, nullptr, &low, &ver);
        history += Format("(mirror liquid cells %u, lowest at %d,%d,%d ver %u, "
                          "tick %u)",
                          wet, low.x, low.y, low.z, ver, f.tick);
      }
    }
    was = asleep;
  }
  std::string endState;
  {
    const Mob* m = dead();
    const char* s = m ? m->DeadAwakeReason() : "gone";
    endState = s ? s : "quiet";
  }
  c.mobs.CoatDigest(f.id, &sumEnd);
  // ---- C. a cut wakes it ----------------------------------------------------
  bool woke = false;
  uint32_t took = 0;
  if (const Mob* m = dead(); m != nullptr && m->DeadAsleep()) {
    const int li = f.root;
    const uint64_t h = c.mobs.LimbBody(f.id, li);
    const uint32_t v0 = c.mobs.LimbArtVoxelCount(f.id, li);
    const Vec3 at = c.mobs.LimbVoxelPos(f.id, li, 0);
    const auto& g = CurrentTuning().gore;
    KerfCut cut;
    cut.at = at;
    cut.edgeAxis = Vec3{1, 0, 0};
    cut.cutDir = Vec3{0, -1, 0};
    cut.halfWidth = std::max(0.9f * g.cutWidth, g.cutWidthMin);
    cut.depth = g.cutDepth + g.cutDepthPower;
    cut.length = g.cutLength;
    cut.power = 1.0f;
    cut.seed = 0x51EEu;
    std::vector<ParticleSpawn> spawns;
    if (h) {
      c.mobs.Damage(h, 10.0f, at, 20.0f, DamageCtx(DamageCause::Blade, 1.0f));
      c.mobs.CutLimb(h, cut, c.world, spawns, 1.0f);
    }
    const Mob* after = dead();
    woke = after != nullptr && !after->DeadAsleep();
    const uint32_t v1 = c.mobs.LimbArtVoxelCount(f.id, li);
    took = v0 > v1 ? v0 - v1 : 0u;
  }
  c.mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  RecordObserved("corpseSleepTicks", (double)asleepAt);
  RecordObserved("corpseSleepAnchors", (double)anchors);
  RecordObserved("corpseSleepAwakeTicks", (double)awakeTicks);
  RecordObserved("corpseSleepDryVisits", (double)visits);
  RecordObserved("corpseSleepShadowWrites", (double)shadowWrites);
  const uint64_t anchorCap = (uint64_t)window / MobSystem::kDeadAnchorStride + 1u;
  const bool asleepOk = asleepAt >= 0 && asleepAt <= maxTicks;
  // The case this package is about: a coat still drying, which P1's rule
  // (drying = awake) would not have let it sleep with.
  const bool drying = dueAtSleep != 0 && p1AtSleep != "quiet";
  const bool quiet =
      windowAsleep && posts == 0 && anchors <= anchorCap && activeTicks == 0;
  const bool dried = wakes == 0 && visits >= 1 && sumEnd < sumAtSleep &&
                     shadowTicks > 0 && shadowWrites == 0;
  const bool ok = asleepOk && drying && quiet && dried && woke && took > 0;
  detail = Format(
      "site inset %d (%u liquid cells within %d), dressed in %s, %u voxels "
      "soaked in blood, authored drying: asleep after %d ticks (cap %d) as '%s' "
      "[P1's rule then: '%s']; asleep for %d ticks: %llu dead PostSteps (need "
      "0), %llu anchors (cap %llu), %u ticks with a limb active in Jolt, stayed "
      "asleep=%d | over %d ticks: awake %u, %u wake(s) (need 0), %u drying "
      "visit(s) (need >= 1), coat sum %llu at sleep -> %llu; shadow awake pass "
      "on %u slept-through ticks wrote on %u (need 0)%s; end '%s' | a cut took "
      "%u voxels and woke it=%d | trace%s",
      inset, siteLiquid, kDryReach, wear ? wear->name.c_str() : "nothing",
      soaked, asleepAt, maxTicks, stateAtSleep.c_str(), p1AtSleep.c_str(), window,
      (unsigned long long)posts, (unsigned long long)anchors,
      (unsigned long long)anchorCap, activeTicks, windowAsleep ? 1 : 0,
      dryTicks, awakeTicks, wakes, visits, (unsigned long long)sumAtSleep,
      (unsigned long long)sumEnd, shadowTicks, shadowWrites, shadowFirst.c_str(),
      endState.c_str(), took, woke ? 1 : 0, history.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// player-corpse: the player's body is a dead Mob too (PLAN_corpse_is_a_mob.md P2b)
// ---------------------------------------------------------------------------
//
// PlayerAvatar is a Mob that does not live in mobs_, so its dead rig is MOVED
// there (MobSystem::AdoptDeadAvatar) at the top of the next PreTick. The
// claims, on a registered avatar that was carved on one limb and set alight
// on another before it died:
//   A. one PreTick after the death, exactly one PlayerCorpse() dead Mob is in
//      mobs_, holding EVERY body handle the avatar held at death, lying where
//      the avatar fell, NOT lootable, and on the corpse's own fresh id; every
//      limb the fire did not touch has exactly the live voxels it died with
//      (the carve came across), and the burning limb is still burning or has
//      lost more (the fire came across). The avatar is a husk: same part list,
//      no bodies;
//   B. Revive gives a WHOLE avatar whose bodies are all new — none of them is
//      a handle the corpse owns — and adopts nothing a second time;
//   C. the corpse falls ASLEEP (it sleeps like any corpse);
//   D. a cut on it after the respawn reaches it: FindOwner names the corpse,
//      the cut takes voxels and wakes it;
//   E. every corpse body is off the avatar layer once the respawned player's
//      capsule is somewhere else.
Status GatePlayerCorpse(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  PrepareWorld(c);
  c.mobs.Reset();
  c.mobs.ClearRisings();
  c.debris.Reset();
  DeadFixture f(c);
  // DRY GROUND. A corpse lying in water is wetted by contact every tick and
  // never sleeps (measured at insets 440 and 470 of the standalone window:
  // 'twin dirty (stain writers 0xd)', a water coat on every limb) — a fact
  // about corpses in water, not about whose corpse this is. A fixed inset is
  // wet or dry depending on where streaming has left the window, so the site
  // is the first candidate no pond disc (rolled tarn or authored lake) covers.
  auto wetAt = [&](IVec3 s) {
    auto covers = [&](const World::PondDisc& d) {
      if (!d.present || d.surf < s.y - 2) return false;
      const int dx = s.x - d.cx, dz = s.z - d.cz, r = d.r + 12;
      return dx * dx + dz * dz <= r * r;
    };
    const int T = World::PondTileSize();
    if (T > 0) {
      const int tx = (int)std::floor((float)s.x / (float)T);
      const int tz = (int)std::floor((float)s.z / (float)T);
      for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++)
          if (covers(World::PondTile(tx + dx, tz + dz, kDefaultSeed))) return true;
    }
    for (int i = 0; i < World::WaterSiteCount(); i++)
      if (covers(World::WaterSiteDisc(i, kDefaultSeed))) return true;
    return false;
  };
  // ...and of those the HIGHEST ground: water that is not a pond (a stream, a
  // flooded hollow — 448 water cells round the corpse at inset 470, measured)
  // lies low, and a knoll drains.
  int inset = -1, bestY = INT_MIN;
  for (int cand : {470, 300, 360, 200, 420, 250, 150, 330, 100, 400}) {
    const IVec3 s = FixtureSite(c.world, cand);
    if (wetAt(s) || s.y <= bestY) continue;
    inset = cand;
    bestY = s.y;
  }
  if (inset < 0) {
    detail = "every candidate site is under a pond disc";
    return Status::Fail;
  }
  const IVec3 site = FixtureSite(c.world, inset);
  f.pchunk = IVec3{site.x >> 4, site.y >> 4, site.z >> 4};
  f.tick = 64000;

  PlayerAvatar av;
  av.Init(&c.phys, &c.world, &c.debris, c.mats, &c.mobs);
  av.SetDefs(&c.mobs.Defs(), kAvatarDefName);
  if (!av.HasDef()) {
    detail = std::string("no mob def named \"") + kAvatarDefName + "\"";
    return Status::Fail;
  }
  Player pl;
  pl.fly = false;
  pl.grounded = true;
  auto standAt = [&](int x, int z) {
    pl.pos = Vec3{(float)x + 0.5f,
                  (float)(World::TerrainHeight(x, z, kDefaultSeed) + 2) +
                      Player::kHalfY,
                  (float)z + 0.5f};
  };
  standAt(site.x, site.z);
  const uint64_t proxy = c.phys.CreatePlayerBody(Player::kHalfXZ, Player::kHalfY);
  c.phys.MovePlayerBody(proxy, pl.pos, kTickDt);
  auto cleanup = [&]() {
    c.mobs.SetAvatar(nullptr);
    av.Despawn();
    c.phys.RemoveBody(proxy);
    c.mobs.Reset();
    c.mobs.ClearRisings();
    c.debris.Reset();
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
  };
  if (!av.Spawn(pl, 0.0f)) {
    cleanup();
    detail = "avatar Spawn refused";
    return Status::Fail;
  }
  c.mobs.SetAvatar(&av);
  // The counter is the system's lifetime total; earlier gates in a suite
  // (mob-loot's kit arm) have their own avatars die.
  const uint64_t adopted0 = c.mobs.AdoptedAvatarsTotal();
  const MobDef& def = *av.Def();
  const int nBase = (int)def.limbs.size();
  const int root = def.rootLimb;

  // The PlayerCorpse() dead Mobs in mobs_, in list order.
  auto playerCorpses = [&]() {
    std::vector<uint64_t> ids;
    for (uint32_t i = 0; i < c.mobs.MobCount(); i++) {
      const Mob* m = c.mobs.FindMobById(c.mobs.MobIdAt(i));
      if (m != nullptr && m->PlayerCorpse()) ids.push_back(m->Id());
    }
    return ids;
  };
  // Every part of the avatar has a body, and each one is the AVATAR's —
  // FindOwner walks mobs_ first, so a handle a corpse still owned would
  // answer with the corpse.
  auto respawnWhole = [&](int& whole, int& aliased) {
    whole = aliased = 0;
    for (int i = 0; i < nBase; i++) {
      const uint64_t h = av.PartBody(i);
      if (!h) continue;
      whole++;
      int li = -1;
      if (c.mobs.FindOwner(h, &li) != &av) aliased++;
    }
    return av.IsAlive() && whole == nBase && aliased == 0;
  };

  // ======== DEATH 1: clean — the corpse C and D are asserted on ============
  //
  // No wound: a corpse that bleeds lies in its own blood, re-stains from the
  // ground by contact as fast as it dries, and stays (correctly, for now)
  // awake — measured on death 2 below, 'twin dirty (stain writers 0x5)' past
  // 1800 ticks. corpse-sleep kills its fixture the same way for the same
  // reason. This is the property "sleeps like any corpse" can be stated for.
  for (int i = 0; i < 4; i++) f.Step();
  std::vector<uint64_t> handles1;
  for (int i = 0; i < av.PartCount(); i++)
    if (av.PartBody(i)) handles1.push_back(av.PartBody(i));
  const Vec3 fell1 = av.Origin();
  av.Die();
  const bool died1 = !av.IsAlive();
  f.Step();   // the PreTick that adopts it
  std::vector<uint64_t> ids = playerCorpses();
  const uint64_t cid = ids.size() == 1 ? ids[0] : 0;
  const Mob* corpse1 = cid ? c.mobs.FindMobById(cid) : nullptr;
  uint32_t kept1 = 0;
  for (uint64_t h : handles1) {
    int li = -1;
    if (corpse1 != nullptr && c.mobs.FindOwner(h, &li) == corpse1) kept1++;
  }
  const float maxDrift = (float)BaselineNumber("playerCorpseMaxDrift", 24.0);
  const float drift1 = corpse1 ? (corpse1->Origin() - fell1).len() : 1e9f;
  const int lootable1 = corpse1 ? (corpse1->Lootable() ? 1 : 0) : -1;
  int husk1 = 0;
  for (int i = 0; i < av.PartCount(); i++)
    if (av.PartBody(i)) husk1++;
  const bool a1 = died1 && ids.size() == 1 && corpse1 != nullptr &&
                  !corpse1->Alive() && lootable1 == 0 && cid != av.Id() &&
                  !handles1.empty() && kept1 == handles1.size() &&
                  drift1 <= maxDrift && husk1 == 0 &&
                  av.PartCount() >= nBase && c.mobs.AdoptedAvatarsTotal() - adopted0 == 1;
  corpse1 = nullptr;   // points into mobs_; later ticks may reallocate it

  // ---- respawn 1, clear of the corpse ---------------------------------------
  standAt(site.x - 48, site.z);
  c.phys.MovePlayerBody(proxy, pl.pos, kTickDt);
  av.Revive(pl, 0.0f);
  int whole1 = 0, aliased1 = 0;
  const bool b1 = respawnWhole(whole1, aliased1) &&
                  c.mobs.AdoptedAvatarsTotal() - adopted0 == 1 &&
                  c.mobs.FindMobById(cid) != nullptr;

  // ======== DEATH 2: carved and burning — what the move carries ============
  //
  // The carved limb and the burning one, far apart so the fire's first tick
  // on the corpse cannot touch the carve's count: an arm and a leg.
  auto limbTagged = [&](const char* tag) {
    for (int i = 0; i < nBase; i++)
      if (i != root && def.limbs[i].tag.find(tag) != std::string::npos &&
          av.PartBody(i))
        return i;
    return -1;
  };
  const int carveL = limbTagged("arm");
  const int burnL = limbTagged("leg");
  if (carveL < 0 || burnL < 0 || carveL == burnL) {
    cleanup();
    detail = Format("no arm/leg pair on '%s' (arm %d, leg %d)",
                    def.name.c_str(), carveL, burnL);
    return Status::Fail;
  }
  for (int i = 0; i < 4; i++) f.Step();
  auto liveOf = [&](int i) -> uint32_t {   // tombstones out
    const uint32_t n = av.PartVoxelCount(i), dead = av.PartMaterialCount(i, 0u);
    return n > dead ? n - dead : 0u;
  };
  const uint32_t carve0 = liveOf(carveL);
  {
    std::vector<ParticleSpawn> sp;
    Vec3 at{};
    c.phys.BodyCenterOfMass(av.PartBody(carveL), at);
    // A HOLE, not an amputation: 0.5 world voxels is 4 skin voxels at the
    // human's skinScale, well inside an arm's width.
    c.mobs.CarveLimbRadial(av.PartBody(carveL), at, 0.5f, /*ragged=*/false,
                           /*eject=*/false, c.world, sp);
  }
  const uint32_t lit = av.IgnitePart(
      burnL, (uint32_t)BaselineNumber("playerCorpseIgnite", 60.0));
  const bool carvedStillOn = av.PartBody(carveL) != 0;
  std::vector<uint32_t> liveAtDeath((size_t)nBase, 0u);
  std::vector<uint64_t> handles2;
  for (int i = 0; i < av.PartCount(); i++) {
    if (i < nBase) liveAtDeath[(size_t)i] = liveOf(i);
    if (av.PartBody(i)) handles2.push_back(av.PartBody(i));
  }
  const uint32_t burningAtDeath = av.PartBurningCount(burnL);
  const Vec3 fell2 = av.Origin();
  av.Die();
  f.Step();
  ids = playerCorpses();
  uint64_t cid2 = 0;
  for (uint64_t id : ids)
    if (id != cid) cid2 = id;
  const Mob* corpse2 = cid2 ? c.mobs.FindMobById(cid2) : nullptr;
  uint32_t kept2 = 0;
  for (uint64_t h : handles2) {
    int li = -1;
    if (corpse2 != nullptr && c.mobs.FindOwner(h, &li) == corpse2) kept2++;
  }
  const float drift2 = corpse2 ? (corpse2->Origin() - fell2).len() : 1e9f;
  const int lootable2 = corpse2 ? (corpse2->Lootable() ? 1 : 0) : -1;
  // THE CARVED LIMB EXACTLY: nothing but the move happened to it. Every
  // other limb has had one dead tick of the matter passes by now (the
  // anatomy's `blood` voxels decay out of a corpse, corpse-sleep's note), so
  // it may have lost a few and never gained one: a lattice rebuilt from the
  // def on the far side of the move would read as a GAIN on the carve and as
  // the def's full count everywhere else. The burning limb's joint
  // neighbours share its heat (cross-limb heat) and are the fire's.
  const uint32_t maxFirstTickLoss =
      (uint32_t)BaselineNumber("playerCorpseFirstTickLoss", 8.0);
  int countsKept = 0, countsChecked = 0;
  std::string countWhy;
  for (int i = 0; corpse2 && i < nBase; i++) {
    if (i == burnL || !c.mobs.LimbBody(cid2, i)) continue;
    if (def.limbs[i].parent == def.limbs[burnL].name ||
        def.limbs[burnL].parent == def.limbs[i].name)
      continue;
    const uint32_t art = c.mobs.LimbArtVoxelCount(cid2, i);
    const uint32_t dead = c.mobs.LimbMaterialCount(cid2, i, 0u);
    const uint32_t now = art > dead ? art - dead : 0u;
    const uint32_t at = liveAtDeath[(size_t)i];
    countsChecked++;
    const bool kept = i == carveL ? now == at
                                  : (now <= at && at - now <= maxFirstTickLoss);
    if (kept) countsKept++;
    else if (countWhy.empty())
      countWhy = Format(" [limb %d: %u at death, %u on the corpse]", i, at, now);
  }
  // THE FIRE CAME ACROSS: still burning, or it has gone on eating. (The hot
  // count at death is reported, not required: Ignite seeds the front and the
  // hot count is the burn pass's, which the avatar never ran before dying.)
  uint32_t burnNow = 0, burnLive = 0;
  if (corpse2 && c.mobs.LimbBody(cid2, burnL)) {
    burnNow = c.mobs.LimbBurningCount(cid2, burnL);
    const uint32_t art = c.mobs.LimbArtVoxelCount(cid2, burnL);
    const uint32_t dead = c.mobs.LimbMaterialCount(cid2, burnL, 0u);
    burnLive = art > dead ? art - dead : 0u;
  }
  const bool fireCame =
      lit > 0 && (burnNow > 0 || burnLive < liveAtDeath[(size_t)burnL]);
  const bool carved = liveAtDeath[(size_t)carveL] < carve0;
  const bool a2 = ids.size() == 2 && corpse2 != nullptr && cid2 != cid &&
                  !corpse2->Alive() && lootable2 == 0 && !handles2.empty() &&
                  kept2 == handles2.size() && drift2 <= maxDrift && carved &&
                  carvedStillOn && countsChecked > 0 &&
                  countsKept == countsChecked && fireCame &&
                  c.mobs.AdoptedAvatarsTotal() - adopted0 == 2;
  corpse2 = nullptr;

  // ---- respawn 2 ------------------------------------------------------------
  standAt(site.x - 48, site.z - 48);
  c.phys.MovePlayerBody(proxy, pl.pos, kTickDt);
  av.Revive(pl, 0.0f);
  int whole2 = 0, aliased2 = 0;
  const bool b2 = respawnWhole(whole2, aliased2) &&
                  c.mobs.AdoptedAvatarsTotal() - adopted0 == 2;

  // ======== C. the clean corpse sleeps ======================================
  const int maxTicks = (int)BaselineNumber("playerCorpseSleepMaxTicks", 1800);
  const Tuning sleepTune = CurrentTuning();
  {
    Tuning fast = sleepTune;
    fast.coat.decayScale = (float)BaselineNumber("corpseSleepDecayScale", 300.0);
    SetCurrentTuning(fast);
  }
  int asleepAt = -1;
  std::string history;
  for (int i = 0; i < maxTicks; i++) {
    f.Step();
    const Mob* m = c.mobs.FindMobById(cid);
    if (m == nullptr) break;
    if (m->DeadAsleep()) {
      asleepAt = i;
      break;
    }
    if (i == 0 || i == 100 || i == 400 || i == 900 || i == 1500) {
      const char* w = m->DeadAwakeReason();
      history += Format(" t%d:'%s'", i, w ? w : "quiet");
    }
  }
  SetCurrentTuning(sleepTune);
  std::string awakeWhy;
  if (asleepAt < 0) {
    const Mob* m = c.mobs.FindMobById(cid);
    const char* w = m ? m->DeadAwakeReason() : "gone";
    awakeWhy = Format(" [still awake: '%s'; earlier%s; coats",
                      w ? w : "(quiet now)", history.c_str());
    // What is ON it (rule 6): a coat still changing is the usual reason.
    for (int li = 0; m != nullptr && li < m->LimbCount(); li++)
      if (const LimbCoat* lc = c.mobs.LimbCoatOf(cid, li))
        for (const CoatEntry& e : lc->top)
          if (e.mat != 0 && e.sumAmt != 0)
            awakeWhy += Format(" L%d:%s %u/%uvox", li,
                               e.mat < c.mats.size() ? c.mats[e.mat].name.c_str()
                                                     : "?",
                               e.sumAmt, e.voxels);
    awakeWhy += "]";
    // ...and what is AROUND it: the liquid cells in a 13-voxel box about the
    // pelvis, by material, from the CPU mirror the contact pass itself reads.
    if (m != nullptr) {
      const Vec3 p = m->Origin();
      std::map<uint32_t, uint32_t> census;
      uint32_t uncached = 0;
      for (int z = -6; z <= 6; z++)
        for (int y = -6; y <= 6; y++)
          for (int x = -6; x <= 6; x++) {
            const IVec3 cell{ifloor(p.x) + x + 4, ifloor(p.y) + y + 2,
                             ifloor(p.z) + z + 4};
            const CachedChunk* cc =
                c.world.Cached(IVec3{cell.x >> 4, cell.y >> 4, cell.z >> 4});
            if (cc == nullptr || cc->voxels.size() != kChunkVol) {
              uncached++;
              continue;
            }
            const uint32_t mat =
                cc->voxels[((uint32_t)(cell.z & 15) * kChunk +
                            (uint32_t)(cell.y & 15)) * kChunk +
                           (uint32_t)(cell.x & 15)] & 0xFFFu;
            if (mat != 0 && mat < c.mats.size() &&
                c.mats[mat].gpu.klass == CLASS_LIQUID)
              census[mat]++;
          }
      awakeWhy += Format(" [liquid cells near it (uncached %u):", uncached);
      for (const auto& [mat, n] : census)
        awakeWhy += Format(" %s %u", c.mats[mat].name.c_str(), n);
      awakeWhy += "]";
    }
  }
  // Reported, not asserted: the bleeding corpse's state after the same wait.
  std::string wounded = "gone";
  if (const Mob* m2 = c.mobs.FindMobById(cid2)) {
    const char* w = m2->DeadAwakeReason();
    wounded = m2->DeadAsleep() ? "asleep" : (w ? w : "quiet");
  }

  // ======== E. off the avatar layer =========================================
  int onAvatarLayer = 0, corpseBodies = 0;
  for (uint64_t id : {cid, cid2})
    if (const Mob* m = c.mobs.FindMobById(id))
      for (int i = 0; i < m->LimbCount(); i++)
        if (const uint64_t h = c.mobs.LimbBody(id, i)) {
          corpseBodies++;
          if (c.phys.BodyObjectLayer(h) == 3) onAvatarLayer++;
        }

  // ======== D. a cut after the respawns reaches the sleeping corpse =========
  bool reached = false, woke = false;
  uint32_t took = 0;
  const bool wasAsleep = asleepAt >= 0;
  if (root >= 0 && c.mobs.FindMobById(cid) != nullptr) {
    const uint64_t h = c.mobs.LimbBody(cid, root);
    int li = -1;
    reached = h != 0 && c.mobs.FindOwner(h, &li) == c.mobs.FindMobById(cid) &&
              li == root;
    const uint32_t v0 = c.mobs.LimbArtVoxelCount(cid, root);
    const Vec3 at = c.mobs.LimbVoxelPos(cid, root, 0);
    const auto& g = CurrentTuning().gore;
    KerfCut cut;
    cut.at = at;
    cut.edgeAxis = Vec3{1, 0, 0};
    cut.cutDir = Vec3{0, -1, 0};
    cut.halfWidth = std::max(0.9f * g.cutWidth, g.cutWidthMin);
    cut.depth = g.cutDepth + g.cutDepthPower;
    cut.length = g.cutLength;
    cut.power = 1.0f;
    cut.seed = 0x9C0Bu;
    std::vector<ParticleSpawn> spawns;
    if (h) {
      c.mobs.Damage(h, 10.0f, at, 20.0f, DamageCtx(DamageCause::Blade, 1.0f));
      c.mobs.CutLimb(h, cut, c.world, spawns, 1.0f);
    }
    const Mob* m = c.mobs.FindMobById(cid);
    woke = m != nullptr && !m->DeadAsleep();
    const uint32_t v1 = c.mobs.LimbArtVoxelCount(cid, root);
    took = v0 > v1 ? v0 - v1 : 0u;
  }
  const bool dOk = reached && took > 0 && wasAsleep && woke;
  const bool eOk = corpseBodies > 0 && onAvatarLayer == 0;
  const uint64_t adopted = c.mobs.AdoptedAvatarsTotal() - adopted0;
  const uint64_t avatarId = av.Id();

  // ======== F. saved and loaded, still the player's (P3) ====================
  // MOBS v6 flags bit 1: a loaded player corpse is still PlayerCorpse(), still
  // not lootable, back in the player-corpse id band on distinct ids, and the
  // load spent no NPC id on it (nextId_ moves only by the other records).
  int pcSaved = 0, pcLoaded = 0, pcLootable = 0, pcOffBand = 0;
  uint32_t records = 0;
  uint64_t idSpent = 0;
  std::set<uint64_t> pcIds;
  {
    for (uint32_t i = 0; i < c.mobs.MobCount(); i++)
      if (const Mob* m = c.mobs.MobAt(i); m && !m->Alive() && m->PlayerCorpse())
        pcSaved++;
    std::vector<uint8_t> buf;
    c.mobs.SaveState(buf);
    if (buf.size() >= 4) std::memcpy(&records, buf.data(), 4);
    c.mobs.Reset(/*rewindIds=*/false);
    const uint64_t next0 = c.mobs.NextIdCounter();
    c.mobs.LoadState(buf.data(), buf.size(), MobSystem::kSaveVersion);
    idSpent = c.mobs.NextIdCounter() - next0;
    for (uint32_t i = 0; i < c.mobs.MobCount(); i++) {
      const Mob* m = c.mobs.MobAt(i);
      if (m == nullptr || m->Alive() || !m->PlayerCorpse()) continue;
      pcLoaded++;
      pcIds.insert(m->Id());
      if (m->Lootable()) pcLootable++;
      if (((m->Id() >> 61) & 1u) == 0) pcOffBand++;   // kPlayerCorpseIdBase
    }
  }
  const bool fOk = pcSaved == 2 && pcLoaded == pcSaved &&
                   (int)pcIds.size() == pcLoaded && pcLootable == 0 &&
                   pcOffBand == 0 &&
                   idSpent == (uint64_t)records - (uint64_t)pcLoaded;
  cleanup();
  RecordObserved("playerCorpseSleepTicks", (double)asleepAt);
  const bool ok = a1 && b1 && a2 && b2 && asleepAt >= 0 && dOk && eOk && fOk;
  detail = Format(
      "site inset %d | death 1 %s: corpse id %llu (avatar %llu), %u/%zu "
      "handles kept, drift %.1f (cap %.1f), lootable %d, husk bodies %d | "
      "respawn 1 %s: %d/%d parts, %d aliased | death 2 %s: corpse id %llu, "
      "%zu player corpses, %u/%zu handles kept, drift %.1f, lootable %d, "
      "carve %u -> %u (limb on %d), %d/%d limbs kept their count%s, fire lit "
      "%u (hot %u at death) -> %u hot, %u of %u live on the corpse | respawn "
      "2 %s: %d/%d parts, %d aliased, adopted %llu | C: clean corpse asleep "
      "after %d ticks (cap %d)%s; wounded corpse then: '%s' | D %s: owner %d, "
      "cut took %u, woke %d | E %s: %d/%d corpse bodies on the avatar layer "
      "| F %s: saved %d player corpses in %u records, loaded %d (%zu distinct "
      "ids, %d off the band, %d lootable), NPC ids spent %llu",
      inset, a1 ? "PASS" : "FAIL", (unsigned long long)cid,
      (unsigned long long)avatarId, kept1, handles1.size(), (double)drift1,
      (double)maxDrift, lootable1, husk1, b1 ? "PASS" : "FAIL", whole1, nBase,
      aliased1, a2 ? "PASS" : "FAIL", (unsigned long long)cid2, ids.size(),
      kept2, handles2.size(), (double)drift2, lootable2, carve0,
      liveAtDeath[(size_t)carveL], carvedStillOn ? 1 : 0, countsKept,
      countsChecked, countWhy.c_str(), lit, burningAtDeath, burnNow, burnLive,
      liveAtDeath[(size_t)burnL], b2 ? "PASS" : "FAIL", whole2, nBase,
      aliased2, (unsigned long long)adopted, asleepAt, maxTicks,
      awakeWhy.c_str(), wounded.c_str(), dOk ? "PASS" : "FAIL",
      reached ? 1 : 0, took, woke ? 1 : 0, eOk ? "PASS" : "FAIL",
      onAvatarLayer, corpseBodies, fOk ? "PASS" : "FAIL", pcSaved, records,
      pcLoaded, pcIds.size(), pcOffBand, pcLootable,
      (unsigned long long)idSpent);
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// corpse-cap: the dead are bounded, and the oldest decays to debris
// ---------------------------------------------------------------------------
//
// Rule 2 for the new population (docs/PLAN_corpse_is_a_mob.md "Bounds"). The
// living cap (kMaxMobs) counts the LIVING only; the dead have their own cap
// (kMaxDeadMobs) and a body budget over their limbs (kMaxDeadBodies). Past
// either, the OLDEST corpse decays to debris through Mob::ReleaseRigToDebris,
// where DebrisSystem's own cull finishes it.
//
// Claims: kill kMaxDeadMobs + corpseCapExtra creatures in order, run one tick,
// and
//   A. the dead are within both caps;
//   B. the ones that went are the OLDEST (the first killed), their limbs are
//      loose dead flesh in DebrisSystem now, and the newest is still a corpse;
//   C. the living cap is untouched by the dead: kMaxMobs creatures still
//      spawn beside the corpses, and the next one is refused.
// The first FixtureSite candidate with no liquid within 8 voxels of an R-wide
// basin and ground no higher than `maxGround`; every rejection is appended to
// `siteWhy`. Shared by corpse-float and water-entry (see corpse-float for why
// each condition exists).
static bool FindDryLowSite(Ctx& c, int R, int maxGround, IVec3& site, int& hMax,
                           std::string& siteWhy) {
  std::vector<uint32_t> ch(kChunkVol, 0);
  for (int inset : {480, 400, 380, 300, 250, 200, 150, 100}) {
    const IVec3 s = FixtureSite(c.world, inset);
    int h = 0, hLo = 1 << 30;
    for (int x = s.x - R; x <= s.x + R; x++)
      for (int z = s.z - R; z <= s.z + R; z++) {
        const int th = World::TerrainHeight(x, z, kDefaultSeed);
        h = std::max(h, th);
        hLo = std::min(hLo, th);
      }
    if (h > maxGround) {
      siteWhy += Format("%d: ground y%d; ", inset, h);
      continue;
    }
    uint32_t wet = 0;
    constexpr int kMargin = 8;
    for (int cy = (hLo - 4) >> 4; cy <= (h + 32) >> 4; cy++)
      for (int cz = (s.z - R - kMargin) >> 4; cz <= (s.z + R + kMargin) >> 4; cz++)
        for (int cx = (s.x - R - kMargin) >> 4; cx <= (s.x + R + kMargin) >> 4; cx++) {
          if (!c.world.ChunkInWindow({cx, cy, cz})) continue;
          ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex({cx, cy, cz}), 1,
                         ch.data(), "corpseFloatSite");
          for (uint32_t w : ch) {
            const uint32_t mm = w & 0xFFFu;
            if (mm && mm < c.mats.size() && c.mats[mm].gpu.klass == CLASS_LIQUID)
              wet++;
          }
        }
    if (wet > 0) {
      siteWhy += Format("%d: %u liquid cells near; ", inset, wet);
      continue;
    }
    site = s;
    hMax = h;
    siteWhy += Format("%d: chosen", inset);
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// corpse-float: a dead body in water floats or sinks by what it is made of
// ---------------------------------------------------------------------------
// Owner, 2026-10-09: "corpses should slightly float in water with a bit of
// buoyancy, depending on the material the corpse is made out of. So robots
// obviously wouldn't float." Before Mob::FloatLimpLimbs a corpse fell through a
// lake as if it were air (liquids are not in the collider) and lay on the bed.
//
// One stamped basin (stone bed and walls, 14 layers of water, air above),
// four corpses dropped into it one at a time from just above the surface:
//   * HUMAN must FLOAT LOW: its pelvis never gets near the bed, ends within a
//     few voxels of the waterline, and the rig comes to rest there in Jolt
//     (rule 2: buoyancy is not what keeps it moving);
//   * and A CORPSE LYING IN WATER SLEEPS, floating (human) or sunk
//     (automaton). Before 2026-10-09 the water coat dried and the contact pass
//     re-wetted it every tick, so a corpse in a pond was awake forever; now a
//     limb in water keeps its water coat (BodyBurnState::washTouch).
//   * AUTOMATON (tinker, brass) must SINK to the bed;
//   * ANDROID (courier, synth shell) must SINK too;
//   * SYLVAN (deku, wood) must ride at least as high as the human.
// And a LIVE human knocked limp (StartRagdoll, held down for the whole arm)
// must float like the corpse plus ragdoll.limpWaterLift: owner, 2026-10-10,
// "ragdoll physics in water should be basically identical to corpse physics
// in water with a soft upward drifting force applied". Before that a limp
// creature fell through the water to the bed. Its pelvis must stay off the
// bed, end within the band of the waterline, and ride no lower than the
// corpse's.
// Each arm reports its mean limb density, so a failure says whether the matter
// or the push is wrong. Thresholds in tests/baseline.json (corpseFloat*).
Status GateCorpseFloat(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  MobSystem& mobs = c.mobs;
  const uint32_t mStone = MatIdOf(c, "stone"), mWater = MatIdOf(c, "water");
  if (!mStone || !mWater) {
    detail = "stone / water missing from materials.json";
    return Status::Fail;
  }
  PrepareWorld(c);
  mobs.Reset();
  c.debris.Reset();
  // THE SITE: DRY AND LOW, chosen off the GPU's own voxels. Two ways a
  // basin went wrong here, both measured:
  //   * inset 445 is inside the harness map's fixture lake, and there a
  //     sealed basin with intact walls was topped up from y197 to the lake's
  //     own y206 mid-gate (the water body releveling it as part of the lake);
  //   * inset 350 is high ground (y228) where the surface froze to ice and
  //     snow settled on it, so the corpses floated under a lid.
  // So: the first candidate with no liquid near it and ground no higher than
  // corpseFloatMaxGround. Every rejection is in the detail.
  constexpr int R = 17, kDepth = 14;
  const int maxGround = (int)BaselineNumber("corpseFloatMaxGround", 205);
  IVec3 site{};
  int hMax = 0;
  std::string siteWhy;
  const bool sited = FindDryLowSite(c, R, maxGround, site, hMax, siteWhy);
  if (!sited) {
    detail = "no dry, low site: " + siteWhy;
    return Status::Fail;
  }
  const int bedY = hMax + 1;              // top stone layer of the floor
  const int waterTop = bedY + kDepth;     // topmost water layer
  std::vector<CellOp> basin;
  for (int x = site.x - R; x <= site.x + R; x++)
    for (int z = site.z - R; z <= site.z + R; z++)
      for (int y = bedY - 1; y <= waterTop + 22; y++) {
        const IVec3 cc{x, y, z};
        if (!c.world.CellInWindow(cc)) continue;
        const bool wall = std::abs(x - site.x) >= R - 1 ||
                          std::abs(z - site.z) >= R - 1;
        uint32_t w = PackVoxNew(0u, 0u);
        // Walls the FULL height: inset 445 is in the harness map's fixture
        // lake, whose surface (y~204 measured) poured over a 2-high rim and
        // set this basin's waterline instead of the stamp.
        if (y <= bedY || wall) w = PackVoxNew(mStone, 0u);
        else if (y <= waterTop) w = (mWater & 0xFFFu) | (7u << 12);
        basin.push_back({World::SlotCellIndex(cc), w});
      }
  const IVec3 pchunk{site.x >> 4, waterTop >> 4, site.z >> 4};
  support::TickRig rig(c, 71000u, pchunk);

  const int kTicks = (int)BaselineNumber("corpseFloatTicks", 600);
  struct Arm {
    const char* label;
    const char* def;
    bool ran = false;
    float rho = 0;          // volume-weighted mean over the dynamic limbs
    float lowest = 1e9f;    // the pelvis's lowest centre of mass
    float final = -1;       // ...and where it ended
    int asleepAt = -1;      // first tick seen asleep after it went in
    uint32_t pushes = 0;    // FloatLimpLimbs limb-impulses
    bool rigActive = true;  // Jolt still moving it at the end
    const char* why = "";   // DeadAwakeReason at the end
    std::string trace;      // pelvis y every 60 ticks (rule 6)
    std::string limbs;      // each limb's final y / density
    float surf = -1;        // the waterline it was last pushed against
    int wokeHow = 0;        // Mob::DeadWokeHow at the end (rule 6)
    // WHAT MOVED IN THE MIRROR (rule 6): "last woken how 2" means a chunk
    // round the corpse re-versioned, which is every cause at once. Over the
    // basin's chunks, every version bump the watch saw, split by the field
    // that moved (material / liquid fullness / stain / bits 16-23 only),
    // plus the last moved cell's old and new word.
    uint32_t bumps = 0, cMat = 0, cFull = 0, cStain = 0, cOther = 0;
    // ...and where the stained cells were: a WALL (the stamp's outer ring),
    // the FLOOR, or inside the basin; the y band they fell in; and how many
    // were wetter after than before.
    uint32_t sWall = 0, sFloor = 0, sIn = 0, sUp = 0, yLo = 1u << 30, yHi = 0;
    // ...and what touched each one in the mirror that carried the change:
    // a liquid face-neighbour (soak from the water), else air straight above
    // (an exposed top: rain, a drip), else neither (wicked through the rock).
    uint32_t sByLiquid = 0, sByAirAbove = 0, sByNeither = 0;
    // Wake checks a re-versioned chunk passed because no cell in the body's
    // box moved (MobSystem::DeadMatterSameTotal over the arm).
    uint64_t matterSame = 0;
    uint32_t sTypes[8] = {};
    std::string lastMoved, drift;   // drift: root x,z and current every 120
  };
  Arm arms[] = {{"human", "human"},
                {"automaton", "tinker"},
                {"android", "courier"},
                {"sylvan", "deku"}};
  for (Arm& a : arms) {
    const int d = mobs.FindDef(a.def);
    if (d < 0) continue;
    mobs.Reset();
    c.debris.Reset();
    // A FRESH BASIN PER ARM: each arm is measured against the stamp, not
    // against whatever the arm before it left in the water.
    support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) { o.cells = basin; });
    support::RunTicks(rig, 20);  // the water settles; the mirror fetches it
    const uint64_t id = mobs.Spawn(d, {site.x, waterTop + 3, site.z});
    Mob* m = id ? mobs.FindMobById(id) : nullptr;
    if (!m || !m->Def() || m->Def()->rootLimb < 0) continue;
    mobs.SetMobBehavior(id, "dummy");
    const int root = m->Def()->rootLimb;
    m->Die();
    a.ran = true;
    std::unordered_map<uint64_t, std::pair<uint32_t, std::vector<uint32_t>>> seen;
    auto watchMirror = [&](bool count) {
      for (int cy = (bedY - 1) >> 4; cy <= (waterTop + 22) >> 4; cy++)
        for (int cz = (site.z - R) >> 4; cz <= (site.z + R) >> 4; cz++)
          for (int cx = (site.x - R) >> 4; cx <= (site.x + R) >> 4; cx++) {
            const IVec3 wc{cx, cy, cz};
            const CachedChunk* cc = c.world.Cached(wc);
            if (!cc || cc->voxels.size() != kChunkVol) continue;
            auto& e = seen[World::PackChunkKey(wc)];
            if (e.second.size() == kChunkVol && e.first != cc->version && count) {
              a.bumps++;
              for (uint32_t k = 0; k < kChunkVol; k++) {
                const uint32_t o = e.second[k], n = cc->voxels[k];
                if (o == n) continue;
                if ((o ^ n) & 0xFFFu) a.cMat++;
                else if ((o ^ n) & 0xF000u) a.cFull++;
                else if ((o ^ n) & 0xFF000000u) {
                  a.cStain++;
                  const int wx = cx * 16 + (int)(k & 15),
                            wy = cy * 16 + (int)((k >> 4) & 15),
                            wz = cz * 16 + (int)(k >> 8);
                  if (std::abs(wx - site.x) >= R - 1 || std::abs(wz - site.z) >= R - 1)
                    a.sWall++;
                  else if (wy <= bedY)
                    a.sFloor++;
                  else
                    a.sIn++;
                  if (((n >> 24) & 0xFu) > ((o >> 24) & 0xFu)) a.sUp++;
                  a.sTypes[(n >> 28) & 7u]++;
                  auto mirMat = [&](int x, int y, int z) -> uint32_t {
                    const CachedChunk* q = c.world.Cached({x >> 4, y >> 4, z >> 4});
                    if (!q || q->voxels.size() != kChunkVol) return 0xFFFFu;
                    return q->voxels[((uint32_t)(z & 15) * 16u + (uint32_t)(y & 15)) * 16u +
                                     (uint32_t)(x & 15)] & 0xFFFu;
                  };
                  bool wetNb = false;
                  const int nb[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
                  for (const auto& d : nb) {
                    const uint32_t mm = mirMat(wx + d[0], wy + d[1], wz + d[2]);
                    if (mm != 0xFFFFu && mm != 0u && mm < c.mats.size() &&
                        c.mats[mm].gpu.klass == CLASS_LIQUID)
                      wetNb = true;
                  }
                  if (wetNb) a.sByLiquid++;
                  else if (mirMat(wx, wy + 1, wz) == 0u) a.sByAirAbove++;
                  else a.sByNeither++;
                  a.yLo = std::min(a.yLo, (uint32_t)wy);
                  a.yHi = std::max(a.yHi, (uint32_t)wy);
                }
                else { a.cOther++; continue; }
                a.lastMoved = Format(
                    "(%d,%d,%d) 0x%08x->0x%08x v%u", cx * 16 + (int)(k & 15),
                    cy * 16 + (int)((k >> 4) & 15), cz * 16 + (int)(k >> 8), o,
                    n, cc->version);
              }
            }
            e.first = cc->version;
            e.second = cc->voxels;
          }
    };
    watchMirror(false);
    const uint64_t matterSame0 = mobs.DeadMatterSameTotal();
    for (int i = 0; i < kTicks; i++) {
      support::RunTicks(rig, 1);
      watchMirror(true);
      m = mobs.FindMobById(id);
      if (!m) break;
      Vec3 com{};
      const uint64_t h = mobs.LimbBody(id, root);
      if (h && c.phys.BodyCenterOfMass(h, com)) {
        a.lowest = std::min(a.lowest, com.y);
        a.final = com.y;
        if (i % 120 == 0) {
          const Vec3 cur = sandvox::CurrentOnFloater(com, com.y - 2.0f,
                                                     com.y + 2.0f, (float)(waterTop + 1));
          a.drift += Format(" (%.1f,%.1f cur %.2f,%.2f)", com.x - (float)site.x,
                            com.z - (float)site.z, cur.x, cur.z);
        }
        if (i % 60 == 0) {
          const char* r = m->DeadAwakeReason();
          a.trace += Format(" %.1f", com.y);
          if (r != nullptr && a.label[0] == 'h') a.trace += Format("'%.40s'", r);
        }
      }
      if (m->DeadAsleep() && a.asleepAt < 0) a.asleepAt = i;
      if (!m->DeadAsleep()) a.asleepAt = -1;
    }
    if (m) {
      double rv = 0, n = 0;
      for (int li = 0; li < m->LimbCount(); li++) {
        if (!mobs.LimbBody(id, li)) continue;
        rv += m->LimbFloatDensity(li);
        n += 1;
      }
      a.rho = n > 0 ? (float)(rv / n) : 0.0f;
      // Where every limb ended (rule 6): name, centre-of-mass y, density.
      for (int li = 0; li < m->LimbCount(); li++) {
        const uint64_t hl = mobs.LimbBody(id, li);
        Vec3 lc{};
        if (!hl || !c.phys.BodyCenterOfMass(hl, lc)) continue;
        a.limbs += Format(" %d:%.1f/%.0f", li, lc.y, m->LimbFloatDensity(li));
      }
      a.pushes = m->FloatedLimbs();
      a.surf = m->FloatSurfaceY();
      const uint64_t hr = mobs.LimbBody(id, root);
      a.rigActive = hr != 0 && c.phys.IsActive(hr);
      const char* w = m->DeadAwakeReason();
      a.why = w ? w : "";
      a.wokeHow = m->DeadWokeHow();
    }
    a.matterSame = mobs.DeadMatterSameTotal() - matterSame0;
  }
  // THE LIVE LIMP ARM. Same basin, same drop; StartRagdoll instead of Die,
  // with a minimum longer than the watch so the get-up never starts and what
  // is measured is the limp body in the water.
  Arm limp{"limp human", "human"};
  bool limpHeld = false;
  if (const int d = mobs.FindDef(limp.def); d >= 0) {
    mobs.Reset();
    c.debris.Reset();
    support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) { o.cells = basin; });
    support::RunTicks(rig, 20);
    const uint64_t id = mobs.Spawn(d, {site.x, waterTop + 3, site.z});
    Mob* m = id ? mobs.FindMobById(id) : nullptr;
    if (m && m->Def() && m->Def()->rootLimb >= 0) {
      mobs.SetMobBehavior(id, "dummy");
      const int root = m->Def()->rootLimb;
      const int kLimpTicks = (int)BaselineNumber("corpseFloatLimpTicks", 300);
      m->StartRagdoll(kLimpTicks / 30.0f + 30.0f, "corpse-float limp arm");
      limp.ran = true;
      for (int i = 0; i < kLimpTicks; i++) {
        support::RunTicks(rig, 1);
        m = mobs.FindMobById(id);
        if (!m) break;
        Vec3 com{};
        const uint64_t h = mobs.LimbBody(id, root);
        if (h && c.phys.BodyCenterOfMass(h, com)) {
          limp.lowest = std::min(limp.lowest, com.y);
          limp.final = com.y;
          if (i % 30 == 0) limp.trace += Format(" %.1f", com.y);
        }
      }
      if (m) {
        limp.pushes = m->FloatedLimbs();
        limp.surf = m->FloatSurfaceY();
        limpHeld = m->Alive() && m->Ragdoll() == Mob::RagdollPhase::Limp;
      }
    }
  }
  // THE SEVERED ARM. Owner, 2026-10-14: "corpses will float but their limbs
  // will slowly sink. we need consistency in how corpses and limbs float under
  // the same rules." A severed piece is DEBRIS (DebrisSystem::FloatBodies),
  // which floated it as its single most common material -- muscle, heavier
  // than water -- while the rig floats a limb on its mean matter less the
  // corpse's air. Same basin, same drop: a human loses its right arm at the
  // shoulder and its left leg at the knee, then dies. Each piece must float at
  // the density its rig limb did (the piece's FloatingDensityOf against the
  // limb's LimbFloatDensity x (1 - air), within 1 kg/m^3), and each chain's
  // top piece, lighter than water as a limb, must come to rest at the waterline as
  // the corpse does rather than sink to the bed.
  struct Piece {
    const char* limb;
    bool top;            // the cut limb (its children ride jointed to it)
    uint64_t h = 0;      // the body handle, kept across the sever
    float rigRho = 0;    // what the rig floated it at
    float debrisRho = 0; // what debris floats it at
    float lowest = 1e9f, final = -1;
  };
  Piece pieces[] = {{"armU.R", true}, {"armL.R", false}, {"hand.R", false},
                    {"legL.L", true}, {"foot.L", false}};
  bool severRan = false;
  std::string severWhy;
  if (const int d = mobs.FindDef("human"); d >= 0) {
    mobs.Reset();
    c.debris.Reset();
    support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) { o.cells = basin; });
    support::RunTicks(rig, 20);
    const uint64_t id = mobs.Spawn(d, {site.x, waterTop + 3, site.z});
    Mob* m = id ? mobs.FindMobById(id) : nullptr;
    if (m && m->Def()) {
      mobs.SetMobBehavior(id, "dummy");
      const float air = CurrentTuning().ragdoll.corpseAirFraction;
      auto limbIdx = [&](const char* n) {
        for (int li = 0; li < (int)m->Def()->limbs.size(); li++)
          if (m->Def()->limbs[li].name == n) return li;
        return -1;
      };
      for (Piece& p : pieces) {
        const int li = limbIdx(p.limb);
        if (li < 0) continue;
        p.h = mobs.LimbBody(id, li);
        p.rigRho = FloatingDensity(m->LimbFloatDensity(li), true, air);
      }
      for (const Piece& p : pieces)
        if (p.top && limbIdx(p.limb) >= 0) mobs.Sever(id, limbIdx(p.limb));
      if (Mob* mm = mobs.FindMobById(id)) mm->Die();
      severRan = true;
      for (int i = 0; i < kTicks; i++) {
        support::RunTicks(rig, 1);
        for (Piece& p : pieces) {
          Vec3 com{};
          if (p.h && c.phys.BodyCenterOfMass(p.h, com)) {
            p.lowest = std::min(p.lowest, com.y);
            p.final = com.y;
          }
        }
      }
      for (Piece& p : pieces) p.debrisRho = c.debris.FloatingDensityOf(p.h);
    }
  }
  bool severOk = severRan;
  for (const Piece& p : pieces) {
    const bool sameRule = p.h != 0 && p.debrisRho > 0 &&
                          std::fabs(p.debrisRho - p.rigRho) <= 1.0f;
    // Where it RESTS, not how deep it went: a piece dropped in goes under
    // and bobs back up (measured: the cut leg dipped to 3.6 voxels off the
    // bed and ended at the waterline). `lowest` is reported, not asserted.
    const bool floats = !p.top || p.rigRho >= 1000.0f ||
                        std::fabs(p.final - (float)(waterTop + 1)) <=
                            (float)BaselineNumber("corpseFloatBand", 4.0);
    severOk = severOk && sameRule && floats;
    severWhy += Format(" %s%s: rig %.0f debris %.0f, lowest y%.1f final y%.1f%s;",
                       p.limb, p.top ? "(cut)" : "", p.rigRho, p.debrisRho,
                       p.lowest, p.final,
                       !sameRule ? " RULES DIFFER" : !floats ? " SANK" : "");
  }
  mobs.Reset();
  c.debris.Reset();
  PrepareWorld(c);

  const Arm &hu = arms[0], &au = arms[1], &an = arms[2], &sy = arms[3];
  const float band = (float)BaselineNumber("corpseFloatBand", 4.0);
  const bool humanOk = hu.ran && hu.pushes > 0 &&
                       hu.lowest >= (float)(bedY + 4) &&
                       std::fabs(hu.final - (float)(waterTop + 1)) <= band &&
                       !hu.rigActive && hu.asleepAt >= 0;
  auto sank = [&](const Arm& a) {
    return !a.ran || a.lowest <= (float)(bedY + 5);
  };
  // Riding higher = less of it under the surface it was pushed against.
  const bool sylvanOk =
      !sy.ran || (sy.final - sy.surf) >= (hu.final - hu.surf) - 0.5f;
  const bool limpOk = !limp.ran ||
                      (limpHeld && limp.pushes > 0 &&
                       limp.lowest >= (float)(bedY + 4) &&
                       std::fabs(limp.final - (float)(waterTop + 1)) <= band &&
                       limp.final >= hu.final - 0.5f);
  const bool ok = humanOk && sank(au) && (!au.ran || au.asleepAt >= 0) &&
                  sank(an) && sylvanOk && limpOk && severOk;
  auto line = [&](const Arm& a) {
    if (!a.ran) return Format("%s: (no def %s); ", a.label, a.def);
    return Format("%s: surface y%.0f, ", a.label, a.surf) + Format("rho %.0f, pelvis lowest y%.1f final y%.1f, %u pushes, "
                  "rig %s, corpse %s (%s, last woken how %d, slept through %llu "
                  "chunk changes outside its box), mirror bumps %u "
                  "(cells moved: mat %u, fullness %u, stain %u [wall %u floor %u "
                  "inside %u, wetter %u, y%u..%u; by liquid %u, open top %u, "
                  "neither %u; types %u/%u/%u/%u/%u/%u/%u/%u], stamp-only %u; "
                  "last %s), drift from centre every 120:%s, y every 60:%s; ",
                  a.rho, a.lowest, a.final, a.pushes,
                  a.rigActive ? "MOVING" : "still",
                  a.asleepAt >= 0 ? "asleep" : "awake", a.why, a.wokeHow,
                  (unsigned long long)a.matterSame, a.bumps, a.cMat, a.cFull, a.cStain, a.sWall, a.sFloor, a.sIn,
                  a.sUp, a.yLo == (1u << 30) ? 0u : a.yLo, a.yHi, a.sByLiquid,
                  a.sByAirAbove, a.sByNeither, a.sTypes[0], a.sTypes[1],
                  a.sTypes[2], a.sTypes[3], a.sTypes[4], a.sTypes[5], a.sTypes[6],
                  a.sTypes[7], a.cOther,
                  a.lastMoved.empty() ? "-" : a.lastMoved.c_str(),
                  a.drift.c_str(), a.trace.c_str())
           + " | limbs" + a.limbs + "; ";
  };
  detail = Format("site [%s] bed y%d waterline y%d | ", siteWhy.c_str(), bedY,
                  waterTop) + line(hu) +
           line(au) + line(an) + line(sy) +
           (limp.ran ? Format("limp human: %s, surface y%.0f, pelvis lowest "
                              "y%.1f final y%.1f, %u pushes, y every 30:%s",
                              limpHeld ? "held limp" : "NOT LIMP AT END",
                              limp.surf, limp.lowest, limp.final, limp.pushes,
                              limp.trace.c_str())
                     : std::string("limp human: (no def)")) +
           (severRan ? " | severed:" + severWhy
                     : std::string(" | severed: (no human)"));
  std::printf("corpse-float: %s %s\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// water-entry: a limp body that falls into water goes IN, metres deep
// ---------------------------------------------------------------------------
// Owner, 2026-10-11: "if ragdolling from high up and falling into water, the
// character needs to fall into it and go in a certain amount; right now they
// just splat against the surface ... falling into water should apply a counter
// force that slows you down pretty quickly, so you fall a few meters deep at
// least. its ok if from extremely high heights hitting the water still kills
// you." Jolt's per-limb water drag stopped every limb in the tick it touched
// the surface, and the arrest billed the whole fall as a landing on it.
// Mob::FloatLimpLimbs now brakes the rig as one body above
// ragdoll.waterEntrySpeed (ragdoll.waterEntryLength).
//
// One stamped basin, deep enough that no arm reaches the bed (the bed would be
// a landing, and the claim is about the WATER). A live human knocked limp two
// metres over the surface is handed the speed of a fall:
//   * 14 m/s (a 10 m fall): its pelvis goes waterEntryMinDepthSoft voxels under
//     the waterline and it SURVIVES;
//   * fallSplatSpeed and maxFall (terminal): pelvis at least
//     waterEntryMinDepthHard voxels under. Alive or not is reported, not
//     asserted: owner, "ok if ... still kills you".
// Every arm reports its entry-brake ticks and a pelvis-depth trace (rule 6).
Status GateWaterEntry(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  MobSystem& mobs = c.mobs;
  const uint32_t mStone = MatIdOf(c, "stone"), mWater = MatIdOf(c, "water");
  if (!mStone || !mWater) {
    detail = "stone / water missing from materials.json";
    return Status::Fail;
  }
  PrepareWorld(c);
  mobs.Reset();
  c.debris.Reset();
  constexpr int R = 15;
  const int kDepth = (int)BaselineNumber("waterEntryBasinDepth", 90);
  IVec3 site{};
  int hMax = 0;
  std::string siteWhy;
  if (!FindDryLowSite(c, R, (int)BaselineNumber("corpseFloatMaxGround", 205),
                      site, hMax, siteWhy)) {
    detail = "no dry, low site: " + siteWhy;
    return Status::Fail;
  }
  const int bedY = hMax + 1;
  const int waterTop = bedY + kDepth;
  std::vector<CellOp> basin;
  for (int x = site.x - R; x <= site.x + R; x++)
    for (int z = site.z - R; z <= site.z + R; z++)
      for (int y = bedY - 1; y <= waterTop + 30; y++) {
        const IVec3 cc{x, y, z};
        if (!c.world.CellInWindow(cc)) continue;
        const bool wall = std::abs(x - site.x) >= R - 1 ||
                          std::abs(z - site.z) >= R - 1;
        uint32_t w = PackVoxNew(0u, 0u);
        if (y <= bedY || (wall && y <= waterTop + 4)) w = PackVoxNew(mStone, 0u);
        else if (y <= waterTop) w = (mWater & 0xFFFu) | (7u << 12);
        basin.push_back({World::SlotCellIndex(cc), w});
      }
  const IVec3 pchunk{site.x >> 4, waterTop >> 4, site.z >> 4};
  support::TickRig rig(c, 72000u, pchunk);
  // kMaxCellOpsPerTick: the stamp goes in over as many ticks as it needs.
  auto stamp = [&]() {
    const size_t step = kMaxCellOpsPerTick / 2;
    for (size_t at = 0; at < basin.size(); at += step) {
      const size_t end = std::min(basin.size(), at + step);
      support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) {
        o.cells.assign(basin.begin() + at, basin.begin() + end);
      });
    }
    support::RunTicks(rig, 20);  // the water settles; the mirror fetches it
  };

  const int d = mobs.FindDef("human");
  if (d < 0) {
    detail = "no human def";
    return Status::Fail;
  }
  const float surface = (float)(waterTop + 1);
  struct Arm {
    float ms;
    bool ran = false, alive = false;
    float deepest = 0;          // voxels of pelvis below the waterline, max
    uint32_t brakeTicks = 0;    // Mob::WaterEntryTicks
    std::string trace;          // pelvis depth every 3 ticks for 30 ticks
  };
  Arm arms[] = {{14.0f}, {CurrentTuning().player.fallSplatSpeed},
                {CurrentTuning().player.maxFall}};
  const int kTicks = 90;
  for (Arm& a : arms) {
    mobs.Reset();
    c.debris.Reset();
    stamp();
    const uint64_t id = mobs.Spawn(d, {site.x, waterTop + 20, site.z});
    Mob* m = id ? mobs.FindMobById(id) : nullptr;
    if (!m || !m->Def() || m->Def()->rootLimb < 0) continue;
    mobs.SetMobBehavior(id, "dummy");
    const int root = m->Def()->rootLimb;
    m->StartRagdoll(kTicks / 30.0f + 30.0f, "water-entry");
    m->SetLimbVelocities(Vec3{0.0f, -a.ms / kVoxelMeters, 0.0f});
    a.ran = true;
    for (int i = 0; i < kTicks; i++) {
      support::RunTicks(rig, 1);
      m = mobs.FindMobById(id);
      if (!m) break;
      Vec3 com{};
      const uint64_t h = mobs.LimbBody(id, root);
      if (h && c.phys.BodyCenterOfMass(h, com)) {
        a.deepest = std::max(a.deepest, surface - com.y);
        if (i < 30 && i % 3 == 0) a.trace += Format(" %.0f", surface - com.y);
      }
    }
    if (m) {
      a.alive = m->Alive();
      a.brakeTicks = m->WaterEntryTicks();
    }
  }
  mobs.Reset();
  c.debris.Reset();
  PrepareWorld(c);

  const float soft = (float)BaselineNumber("waterEntryMinDepthSoft", 20);
  const float hard = (float)BaselineNumber("waterEntryMinDepthHard", 30);
  const float bedRoom = (float)(kDepth - 4);
  bool ok = true;
  detail = Format("site [%s] bed y%d waterline y%d |", siteWhy.c_str(), bedY,
                  waterTop);
  for (size_t i = 0; i < 3; i++) {
    const Arm& a = arms[i];
    const float need = i == 0 ? soft : hard;
    const bool armOk = a.ran && a.brakeTicks > 0 && a.deepest >= need &&
                       a.deepest < bedRoom && (i != 0 || a.alive);
    ok = ok && armOk;
    detail += Format(" %s %.1f m/s: pelvis %.0f vox deep (need %.0f, bed at "
                     "%d), %u brake ticks, %s, depth every 3:%s;",
                     armOk ? "ok" : "BAD", a.ms, a.deepest, need, kDepth,
                     a.brakeTicks, a.alive ? "alive" : "dead", a.trace.c_str());
  }
  std::printf("water-entry: %s %s\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// The three body tickets of 2026-10-17 (gates added 2026-10-18)
// ---------------------------------------------------------------------------

// A pose for a debris body lying flat, turned `yawDeg` about Y, whose lattice
// box (`dims`, voxels) is centred on `centre`. Turned on purpose: a body
// within ~20 degrees of the lattice axes settles back into the grid after two
// seconds asleep (DebrisSystem::SettleBodies), and a log that became voxels
// would be ground the voxel probe can see -- the opposite of the fixture.
static BodyTransform LyingPose(Vec3 centre, IVec3 dims, float yawDeg) {
  const float a = yawDeg * 3.14159265f / 180.0f;
  const float ca = std::cos(a), sa = std::sin(a);
  const Vec3 half{0.5f * (float)dims.x, 0.5f * (float)dims.y,
                  0.5f * (float)dims.z};
  // R_y(a) applied to the half extent: (x cos + z sin, y, -x sin + z cos).
  const Vec3 rh{half.x * ca + half.z * sa, half.y, -half.x * sa + half.z * ca};
  BodyTransform xf{};
  xf.pos = Vec3{centre.x - rh.x, centre.y - rh.y, centre.z - rh.z};
  xf.quat[0] = 0.0f;
  xf.quat[1] = std::sin(0.5f * a);
  xf.quat[2] = 0.0f;
  xf.quat[3] = std::cos(0.5f * a);
  return xf;
}

static std::vector<DebrisVoxel> SolidBox(IVec3 dims, uint32_t mat) {
  std::vector<DebrisVoxel> vox;
  for (int z = 0; z < dims.z; z++)
    for (int y = 0; y < dims.y; y++)
      for (int x = 0; x < dims.x; x++)
        vox.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 1u, (uint16_t)mat});
  return vox;
}

// The cells of a stamped fixture go in over as many ticks as kMaxCellOpsPerTick
// needs -- every non-liquid cell first, then the liquid in as few ticks as
// fit, LAST: a basin filled slab by slab against unstamped ground sloshes for
// hundreds of ticks, and since the measured flow carries floaters
// (DESIGN.md 9d.12, and float-current is about exactly that) a fixture whose
// claims are about still water has to BE still (selftest_aquatic.cpp
// StampPoolStill, the same rule).
static void StampCells(support::TickRig& rig, std::vector<CellOp> cells,
                       uint32_t liquid = 0u) {
  const uint32_t lm = liquid & 0xFFFu;
  const auto mid = std::stable_partition(cells.begin(), cells.end(), [&](const CellOp& op) {
    return lm == 0u || (op.word & 0xFFFu) != lm;
  });
  const size_t firstLiquid = (size_t)(mid - cells.begin());
  auto stamp = [&](size_t from, size_t to, size_t step) {
    for (size_t at = from; at < to; at += step) {
      const size_t end = std::min(to, at + step);
      support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) {
        o.cells.assign(cells.begin() + (ptrdiff_t)at, cells.begin() + (ptrdiff_t)end);
      });
    }
  };
  stamp(0, firstLiquid, kMaxCellOpsPerTick / 2);
  stamp(firstLiquid, cells.size(), kMaxCellOpsPerTick);
}

// ---------------------------------------------------------------------------
// player-stand-body: standing on a toppled log is standing
// ---------------------------------------------------------------------------
// Owner, ticket 2026-10-17_21-33-53: "standing on top of rigidbody objects
// that are knocked over right now counts for 'in air time' which will knock
// you over after a few seconds. it should not". The voxel ground probe cannot
// see a rigid body; ApplyPush (Physics::PlayerPushOut) held the player up and
// set `grounded`, but AFTER the tick's Update, which had already re-probed the
// voxels and reported "airborne" to the avatar's air clock. At
// ragdoll.fallSeconds the clock knocked the player down.
//
// The fixture, on THE tick (the rig's own session, made a walking player with
// an avatar and a capsule proxy): a flat stone pad, a wooden log lying on it
// turned 35 degrees (so it cannot settle back into the grid and become voxel
// ground), the player stood on the log's middle with no input for
// standBodyTicks (default 10 s, over three times ragdoll.fallSeconds). Claims:
//   * the avatar never goes limp;
//   * the player is still ON the log at the end (feet within 1.5 voxels of its
//     top, the log under them) -- what held them up was the body, not a fall
//     through to the pad;
//   * `grounded` after the tick's Update read true on >= standBodyGroundedPct
//     of the ticks (Player::bodySupport carries the push into it).
Status GatePlayerStandBody(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  const uint32_t mStone = MatIdOf(c, "stone"), mWood = MatIdOf(c, "wood");
  if (!mStone || !mWood) {
    detail = "stone / wood missing from materials.json";
    return Status::Fail;
  }
  PrepareWorld(c);
  c.mobs.Reset();
  c.debris.Reset();
  constexpr int R = 12;
  IVec3 site{};
  int hMax = 0;
  std::string siteWhy;
  if (!FindDryLowSite(c, R, (int)BaselineNumber("corpseFloatMaxGround", 205),
                      site, hMax, siteWhy)) {
    detail = "no dry, low site: " + siteWhy;
    return Status::Fail;
  }
  int hLo = hMax;
  for (int x = site.x - R; x <= site.x + R; x++)
    for (int z = site.z - R; z <= site.z + R; z++)
      hLo = std::min(hLo, World::TerrainHeight(x, z, kDefaultSeed));
  // A flat stone pad, its top face at `floorTop`, and clear air above it.
  const int floorTop = hMax + 2;
  std::vector<CellOp> pad;
  for (int x = site.x - R; x <= site.x + R; x++)
    for (int z = site.z - R; z <= site.z + R; z++)
      for (int y = hLo - 1; y <= floorTop + 40; y++) {
        const IVec3 cc{x, y, z};
        if (!c.world.CellInWindow(cc)) continue;
        pad.push_back({World::SlotCellIndex(cc),
                       y < floorTop ? PackVoxNew(mStone, 0u) : PackVoxNew(0u, 0u)});
      }
  const IVec3 pchunk{site.x >> 4, floorTop >> 4, site.z >> 4};
  support::TickRig rig(c, 73000u, pchunk);
  StampCells(rig, pad);
  support::RunTicks(rig, 20);

  // THE LOG: 30 x 8 x 8 voxels of wood (~1.1 t: far past PlayerPushOut's
  // kick-it-aside floor), lying on the pad. Thicker than a step
  // (Player::kMaxStepUpVoxels), so no voxel probe from its top reaches the
  // pad: what holds the player up is the body or nothing.
  const IVec3 dims{30, 8, 8};
  std::vector<float> dens;
  for (const auto& m : c.mats) dens.push_back((float)m.gpu.density);
  const std::vector<DebrisVoxel> vox = SolidBox(dims, mWood);
  const BodyTransform xf =
      LyingPose(Vec3{(float)site.x + 0.5f, (float)floorTop + 0.5f * (float)dims.y + 0.05f,
                     (float)site.z + 0.5f},
                dims, 35.0f);
  const uint64_t log = c.phys.CreateDebrisBodyXf(vox, xf, dens);
  if (log == 0) {
    detail = "the log body would not build";
    return Status::Fail;
  }
  c.debris.AdoptBody(log, vox, xf);
  support::RunTicks(rig, 30);  // the log comes to rest on the pad
  Vec3 logCom{};
  c.phys.BodyCenterOfMass(log, logCom);
  float lmn[3], lmx[3];
  c.phys.BodyWorldBoundsM(log, lmn, lmx);
  const float logTop0 = (float)floorTop + (float)dims.y;

  // THE PLAYER: the rig's session, made a walking body standing on the log.
  PlayerSession& s = rig.Session();
  Player& pl = s.player;
  PlayerAvatar& av = s.avatar;
  av.Init(&c.phys, &c.world, &c.debris, c.mats, &c.mobs);
  av.SetDefs(&c.mobs.Defs(), kAvatarDefName);
  if (!av.HasDef()) {
    detail = std::string("no mob def named \"") + kAvatarDefName + "\"";
    return Status::Fail;
  }
  pl.fly = false;
  pl.vel = Vec3{};
  pl.pos = Vec3{logCom.x, logTop0 + Player::kHalfY + 0.2f, logCom.z};
  pl.grounded = true;
  pl.SnapRender();
  s.playerBody = c.phys.CreatePlayerBody(Player::kHalfXZ, Player::kHalfY);
  c.phys.MovePlayerBody(s.playerBody, pl.pos, kTickDt);
  const bool spawned = av.Spawn(pl, 0.0f);

  const int kTicks = (int)BaselineNumber("standBodyTicks", 300);
  int limpAt = -1, groundedTicks = 0, supportTicks = 0;
  float feetLo = 1e9f, feetHi = -1e9f;
  std::string trace;
  for (int i = 0; spawned && i < kTicks; i++) {
    support::RunTicks(rig, 1);
    // `bodySupport` after the tick is what the NEXT Update will fold into
    // `grounded`; `grounded` after the tick has the push's own write on top
    // of Update's, so it is the support count that says the carry happened.
    if (pl.grounded) groundedTicks++;
    if (pl.bodySupport) supportTicks++;
    const float feet = pl.pos.y - Player::kHalfY;
    feetLo = std::min(feetLo, feet);
    feetHi = std::max(feetHi, feet);
    if (av.Ragdolled() && limpAt < 0) limpAt = i;
    if (i % 30 == 0)
      trace += Format(" %.1f%s", feet, av.Ragdolled() ? "L" : "");
  }
  Vec3 logEnd{};
  const bool logThere = c.phys.BodyCenterOfMass(log, logEnd);
  const float feetEnd = pl.pos.y - Player::kHalfY;
  const float dxz = std::sqrt((pl.pos.x - logEnd.x) * (pl.pos.x - logEnd.x) +
                              (pl.pos.z - logEnd.z) * (pl.pos.z - logEnd.z));
  av.Despawn();
  c.phys.RemoveBody(s.playerBody);
  s.playerBody = 0;
  pl.fly = true;
  c.mobs.Reset();
  c.debris.Reset();
  PrepareWorld(c);

  const float pct = kTicks > 0 ? 100.0f * (float)supportTicks / (float)kTicks : 0.0f;
  const float minPct = (float)BaselineNumber("standBodyGroundedPct", 90);
  const bool onLog = logThere && std::fabs(feetEnd - logTop0) <= 1.5f && dxz < 12.0f;
  const bool ok = spawned && limpAt < 0 && onLog && pct >= minPct;
  detail = Format(
      "site [%s] pad top y%d, log top y%.0f (bounds y%.1f..%.1f) | %s: limp %s, "
      "feet end y%.2f (lowest %.2f, highest %.2f), %.1f vox from the log's "
      "centre, body support %d/%d ticks (%.0f%%, want >= %.0f%%), grounded "
      "after the push %d; feet every 30:%s",
      siteWhy.c_str(), floorTop, logTop0, lmn[1] / kVoxelMeters,
      lmx[1] / kVoxelMeters, spawned ? "spawned" : "AVATAR DID NOT SPAWN",
      limpAt < 0 ? "never" : Format("at tick %d", limpAt).c_str(), feetEnd,
      feetLo, feetHi, dxz, supportTicks, kTicks, pct, minPct, groundedTicks,
      trace.c_str());
  std::printf("player-stand-body: %s %s\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// float-current: a floating limb and a floating corpse drift with the water
// ---------------------------------------------------------------------------
// Owner, ticket 2026-10-17_20-54-57: "limbs are frozen when they should be
// moving in the direction of water current; water current doesnt seem to be
// moving some physics objects when it should". Physics::ApplyBuoyancy aimed
// its drag at STILL water (Jolt's fluid velocity was zero), so every rigid
// body afloat -- debris, a severed limb, a corpse's limbs -- was braked to
// rest where it went in and fell asleep there; and asleep, nothing woke it.
//
// One stamped basin (stone, kDepth of water, the water stamped LAST so the
// pool starts still), two floaters dropped in upstream of its middle: a human
// corpse (a dead Mob: Mob::FloatLimpLimbs) and a loose limb of flesh (DEBRIS,
// dead flesh: DebrisSystem::FloatBodies -- the path a severed limb takes).
// The limb is a 3x10x3 flesh lattice rather than a cut-off arm on purpose: a
// cut BLEEDS, the blood moving through the water is measured flow that
// carries the floaters as it should, and nothing in a bleeding basin is still
// enough to claim anything about still water. Three phases on THE tick:
//   A. STILL, floatCurrentSettleTicks: both come to rest -- the limb asleep
//      in Jolt, the corpse asleep as a corpse -- and drift less than
//      floatCurrentStillMax over the phase's last 150 ticks.
//   B. A +x CURRENT (a sim-licensed stream primitive, 0.5 m/s, through the
//      basin) for floatCurrentTicks: both drift +x at least
//      floatCurrentMinDrift voxels. They start ASLEEP, so this is also the
//      claim that a current wakes a sleeper (physics.waterCurrentWake).
//   C. STILL AGAIN, floatCurrentSettleTicks: both settle back to sleep.
Status GateFloatCurrent(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  MobSystem& mobs = c.mobs;
  const uint32_t mStone = MatIdOf(c, "stone"), mWater = MatIdOf(c, "water");
  if (!mStone || !mWater) {
    detail = "stone / water missing from materials.json";
    return Status::Fail;
  }
  PrepareWorld(c);
  mobs.Reset();
  c.debris.Reset();
  CurrentPrims().Clear();
  constexpr int R = 26, kDepth = 12;
  IVec3 site{};
  int hMax = 0;
  std::string siteWhy;
  if (!FindDryLowSite(c, R, (int)BaselineNumber("corpseFloatMaxGround", 205),
                      site, hMax, siteWhy)) {
    detail = "no dry, low site: " + siteWhy;
    return Status::Fail;
  }
  const int bedY = hMax + 1;
  const int waterTop = bedY + kDepth;
  std::vector<CellOp> basin;
  for (int x = site.x - R; x <= site.x + R; x++)
    for (int z = site.z - R; z <= site.z + R; z++)
      for (int y = bedY - 1; y <= waterTop + 22; y++) {
        const IVec3 cc{x, y, z};
        if (!c.world.CellInWindow(cc)) continue;
        const bool wall = std::abs(x - site.x) >= R - 1 ||
                          std::abs(z - site.z) >= R - 1;
        uint32_t w = PackVoxNew(0u, 0u);
        if (y <= bedY || wall) w = PackVoxNew(mStone, 0u);
        else if (y <= waterTop) w = (mWater & 0xFFFu) | (7u << 12);
        basin.push_back({World::SlotCellIndex(cc), w});
      }
  const IVec3 pchunk{site.x >> 4, waterTop >> 4, site.z >> 4};
  support::TickRig rig(c, 74000u, pchunk);
  StampCells(rig, basin, mWater);
  support::RunTicks(rig, 20);  // the water settles; the mirror fetches it

  const int d = mobs.FindDef("human");
  if (d < 0) {
    detail = "no human def";
    return Status::Fail;
  }
  // Upstream of the middle, so the current has room to carry them.
  const uint64_t id = mobs.Spawn(d, {site.x - 14, waterTop + 3, site.z - 8});
  Mob* m = id ? mobs.FindMobById(id) : nullptr;
  if (!m || !m->Def() || m->Def()->rootLimb < 0) {
    detail = "the human would not spawn";
    return Status::Fail;
  }
  mobs.SetMobBehavior(id, "dummy");
  const int root = m->Def()->rootLimb;
  m->Die();
  // THE LIMB: dead flesh (AdoptBody's `dead`, as Mob::DetachLimb hands a cut
  // one over), no wound, lying turned on the surface.
  const uint32_t mFlesh = MatIdOf(c, "flesh");
  if (!mFlesh) {
    detail = "flesh missing from materials.json";
    return Status::Fail;
  }
  std::vector<float> dens;
  for (const auto& mm : c.mats) dens.push_back((float)mm.gpu.density);
  const IVec3 limbDims{3, 10, 3};
  const std::vector<DebrisVoxel> limbVox = SolidBox(limbDims, mFlesh);
  BodyTransform limbXf = LyingPose(
      Vec3{(float)site.x - 14.0f, (float)waterTop + 2.0f, (float)site.z + 8.0f},
      limbDims, 30.0f);
  {
    // Lying down: a quarter turn about Z after the yaw, so its length is
    // horizontal (LyingPose builds the yaw; compose the roll here).
    const float h = 0.70710678f;
    const float q0[4] = {limbXf.quat[0], limbXf.quat[1], limbXf.quat[2], limbXf.quat[3]};
    const float r[4] = {0.0f, 0.0f, h, h};
    // q = q0 * r (Hamilton, x y z w).
    limbXf.quat[0] = q0[3] * r[0] + q0[0] * r[3] + q0[1] * r[2] - q0[2] * r[1];
    limbXf.quat[1] = q0[3] * r[1] - q0[0] * r[2] + q0[1] * r[3] + q0[2] * r[0];
    limbXf.quat[2] = q0[3] * r[2] + q0[0] * r[1] - q0[1] * r[0] + q0[2] * r[3];
    limbXf.quat[3] = q0[3] * r[3] - q0[0] * r[0] - q0[1] * r[1] - q0[2] * r[2];
  }
  const uint64_t arm = c.phys.CreateDebrisBodyXf(limbVox, limbXf, dens);
  if (arm == 0) {
    detail = "the limb body would not build";
    return Status::Fail;
  }
  c.debris.AdoptBody(arm, limbVox, limbXf, {}, 0, {}, 0, {}, /*dead=*/true);

  struct Track {
    Vec3 armAt{}, rootAt{};
    bool armOk = false, rootOk = false;
  };
  auto sample = [&]() {
    Track t;
    t.armOk = arm != 0 && c.phys.BodyCenterOfMass(arm, t.armAt);
    const uint64_t rh = mobs.LimbBody(id, root);
    t.rootOk = rh != 0 && c.phys.BodyCenterOfMass(rh, t.rootAt);
    return t;
  };
  auto flat = [](Vec3 a, Vec3 b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
  };
  auto corpseAsleep = [&]() {
    const Mob* mm = mobs.FindMobById(id);
    return mm != nullptr && mm->DeadAsleep();
  };
  // Why the corpse is still awake (rule 6): DeadAwakeReason, or "-".
  auto corpseWhy = [&]() {
    const Mob* mm = mobs.FindMobById(id);
    const char* w = mm != nullptr ? mm->DeadAwakeReason() : "gone";
    return std::string(w != nullptr ? w : "-");
  };
  const int kSettle = (int)BaselineNumber("floatCurrentSettleTicks", 600);
  const int kCarry = (int)BaselineNumber("floatCurrentTicks", 180);
  const float stillMax = (float)BaselineNumber("floatCurrentStillMax", 1.5);
  const float minDrift = (float)BaselineNumber("floatCurrentMinDrift", 4.0);

  // ---- A. STILL ----
  Track a150{};
  for (int i = 0; i < kSettle; i++) {
    support::RunTicks(rig, 1);
    if (i == kSettle - 150) a150 = sample();
  }
  const Track aEnd = sample();
  const bool aArmAsleep = aEnd.armOk && !c.phys.IsActive(arm);
  const bool aCorpseAsleep = corpseAsleep();
  const std::string aWhy = corpseWhy();
  const float aArmDrift = aEnd.armOk && a150.armOk ? flat(aEnd.armAt, a150.armAt) : 1e9f;
  const float aRootDrift =
      aEnd.rootOk && a150.rootOk ? flat(aEnd.rootAt, a150.rootAt) : 1e9f;

  // ---- B. THE CURRENT ----
  const auto fp0 = c.debris.Floaters();
  {
    CurrentPrim st;
    st.kind = kCurrentPrimStream;
    st.flags = kCurrentPrimSim;
    st.x = site.x; st.y = waterTop; st.z = site.z;
    st.radius = 64;
    st.reach = 64;
    st.decayTicks = kCurrentPrimForever;
    st.ownerId = 0x7E5Au;
    CurrentPrimAim(st, Vec3{1.0f, 0.0f, 0.0f}, 0.5f);
    CurrentPrims().Spawn(st);
  }
  std::string bTrace;
  Vec3 field{}, prim{};
  for (int i = 0; i < kCarry; i++) {
    support::RunTicks(rig, 1);
    if (i == 0) {
      // What a body feels at the basin's middle (prims + the solved field +
      // the measured flow) and the primitives' own share (the render arm, no
      // measured flow): two numbers, so a churning pool reads as their
      // difference. After the first tick, which resolves the new primitive.
      field = CurrentAtCpuSim(Vec3{(float)site.x, (float)waterTop, (float)site.z});
      prim = CurrentAtCpu(Vec3{(float)site.x, (float)waterTop, (float)site.z});
    }
    if (i % 30 == 29) {
      const Track t = sample();
      bTrace += Format(" %.1f/%.1f", t.armOk ? t.armAt.x - aEnd.armAt.x : -99.0f,
                       t.rootOk ? t.rootAt.x - aEnd.rootAt.x : -99.0f);
    }
  }
  const Track bEnd = sample();
  const auto fp1 = c.debris.Floaters();
  const Mob* mb = mobs.FindMobById(id);
  const int wokeHow = mb ? (int)mb->DeadWokeHow() : -1;
  const float bArmDrift = bEnd.armOk && aEnd.armOk ? bEnd.armAt.x - aEnd.armAt.x : -1e9f;
  const float bRootDrift =
      bEnd.rootOk && aEnd.rootOk ? bEnd.rootAt.x - aEnd.rootAt.x : -1e9f;

  // ---- C. STILL AGAIN ----
  CurrentPrims().Clear();
  for (int i = 0; i < kSettle; i++) support::RunTicks(rig, 1);
  const Track cEnd = sample();
  const bool cArmAsleep = cEnd.armOk && !c.phys.IsActive(arm);
  const bool cCorpseAsleep = corpseAsleep();
  const std::string cWhy = corpseWhy();

  mobs.Reset();
  c.debris.Reset();
  CurrentPrims().Clear();
  PrepareWorld(c);

  const bool aOk = aArmAsleep && aCorpseAsleep && aArmDrift <= stillMax &&
                   aRootDrift <= stillMax;
  const bool bOk = bArmDrift >= minDrift && bRootDrift >= minDrift;
  const bool cOk = cArmAsleep && cCorpseAsleep;
  const bool ok = aOk && bOk && cOk;
  detail = Format(
      "site [%s] bed y%d waterline y%d | A still: limb %s drift %.2f, corpse %s "
      "(%s) drift %.2f (max %.1f) | B current %.2f vox/s +x at the middle (the "
      "primitive alone %.2f): limb "
      "+%.1f, corpse root +%.1f (want >= %.1f); debris %u current wakes, %u "
      "pinned, %u carried impulses; corpse woke how %d (5 = current); "
      "limb/root dx every 30:%s | C still: limb %s, corpse %s (%s)",
      siteWhy.c_str(), bedY, waterTop, aArmAsleep ? "asleep" : "AWAKE",
      aArmDrift, aCorpseAsleep ? "asleep" : "AWAKE", aWhy.c_str(), aRootDrift,
      stillMax,
      field.x, prim.x, bArmDrift, bRootDrift, minDrift,
      fp1.currentWakes - fp0.currentWakes, fp1.currentPinned - fp0.currentPinned,
      fp1.currentCarried - fp0.currentCarried, wokeHow, bTrace.c_str(),
      cArmAsleep ? "asleep" : "AWAKE", cCorpseAsleep ? "asleep" : "AWAKE",
      cWhy.c_str());
  std::printf("float-current: %s %s\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// debris-outside: a body that leaves the window lands on the ground out there
// ---------------------------------------------------------------------------
// Owner, ticket 2026-10-17_00-12-10: "limb was melted by acid and fell off a
// mountain, then when progressing down a lod seam to find the limb again its
// floating in the air". Two halves, one fixture:
//   * a terrain patch on a window face read everything outside the window as
//     SOLID, which for a collider is an invisible wall up every side face and
//     a floor across the bottom one -- a limb rolling off a cliff could come
//     to rest in mid-air on it. Outside is now the analytic ground
//     (World::TerrainHeight): solid below it, air above;
//   * a body that does get outside had nothing to collide with and fell to the
//     cull, or hung asleep where the window left it. It is now laid on the
//     analytic ground, asleep, and kept (DebrisSystem::DropOutside).
// A 3x3x3 wooden block launched at 6 m/s from just inside the +x face, high
// over the ground, through the face. Claims: it crosses the face (no wall);
// it is still a body when the arm ends (kept, not culled); it is asleep; and
// it rests ON the analytic ground under it -- its bottom within
// [-0.5, debrisOutsideBand] voxels of that ground's top face.
Status GateDebrisOutside(Ctx& c, std::string& detail) {
  const uint32_t mWood = MatIdOf(c, "wood");
  if (!mWood) {
    detail = "wood missing from materials.json";
    return Status::Fail;
  }
  PrepareWorld(c);
  c.mobs.Reset();
  c.debris.Reset();
  const IVec3 wo = c.world.WindowOrigin();
  const int faceX = wo.x * (int)kChunk + (int)kWorldN;  // first x outside
  const int fz = wo.z * (int)kChunk + (int)kWorldN / 2;
  const int fx = faceX - 6;
  // High over everything between the start and well past the face.
  int hTop = INT_MIN;
  for (int x = fx - 4; x <= faceX + 40; x++)
    for (int z = fz - 3; z <= fz + 3; z++)
      hTop = std::max(hTop, World::TerrainHeight(x, z, kDefaultSeed));
  const int startY = hTop + 24;
  const int winTop = wo.y * (int)kChunk + (int)kWorldN;
  if (startY + 4 >= winTop || startY <= wo.y * (int)kChunk) {
    detail = Format("the +x face's ground (y%d) leaves no room in the window "
                    "(y%d..%d)", hTop, wo.y * (int)kChunk, winTop);
    return Status::Fail;
  }
  support::TickRig rig(c, 75000u, IVec3{fx >> 4, startY >> 4, fz >> 4});
  support::RunTicks(rig, 10);
  std::vector<float> dens;
  for (const auto& m : c.mats) dens.push_back((float)m.gpu.density);
  const std::vector<DebrisVoxel> vox = SolidBox({3, 3, 3}, mWood);
  BodyTransform xf{};
  xf.pos = Vec3{(float)fx - 1.0f, (float)startY, (float)fz - 1.0f};
  xf.quat[3] = 1.0f;
  const uint64_t h = c.phys.CreateDebrisBodyXf(vox, xf, dens);
  if (h == 0) {
    detail = "the block would not build";
    return Status::Fail;
  }
  c.debris.AdoptBody(h, vox, xf);
  c.phys.SetBodyVelocities(h, Vec3{60.0f, 0.0f, 0.0f}, Vec3{});
  const auto s0 = c.debris.Settle();
  const int kTicks = (int)BaselineNumber("debrisOutsideTicks", 90);
  float maxX = -1e9f;
  int outAt = -1;
  std::string trace;
  for (int i = 0; i < kTicks; i++) {
    support::RunTicks(rig, 1);
    Vec3 p{};
    if (!c.phys.BodyCenterOfMass(h, p)) break;
    maxX = std::max(maxX, p.x);
    if (p.x >= (float)faceX && outAt < 0) outAt = i;
    if (i % 10 == 0) trace += Format(" (%.1f,%.1f)", p.x - (float)faceX, p.y);
  }
  Vec3 end{};
  const bool alive = c.phys.BodyCenterOfMass(h, end);
  const bool asleep = alive && !c.phys.IsActive(h);
  float mn[3] = {0, 0, 0}, mx[3] = {0, 0, 0};
  if (alive) c.phys.BodyWorldBoundsM(h, mn, mx);
  const float bottom = mn[1] / kVoxelMeters;
  int ground = INT_MIN;
  if (alive) {
    const float lo[2] = {mn[0] / kVoxelMeters, mn[2] / kVoxelMeters};
    const float hi[2] = {mx[0] / kVoxelMeters, mx[2] / kVoxelMeters};
    const float xs[5] = {lo[0], hi[0], lo[0], hi[0], 0.5f * (lo[0] + hi[0])};
    const float zs[5] = {lo[1], lo[1], hi[1], hi[1], 0.5f * (lo[1] + hi[1])};
    for (int k = 0; k < 5; k++)
      ground = std::max(ground, World::TerrainHeight(ifloor(xs[k]), ifloor(zs[k]),
                                                     kDefaultSeed));
  }
  const auto s1 = c.debris.Settle();
  c.debris.Reset();
  PrepareWorld(c);

  const float band = (float)BaselineNumber("debrisOutsideBand", 2.0);
  const float above = bottom - (float)(ground + 1);
  const bool crossed = outAt >= 0 && alive && end.x > (float)faceX;
  const bool onGround = alive && above >= -0.5f && above <= band;
  const bool ok = crossed && alive && asleep && onGround;
  detail = Format(
      "face x%d, start y%d over ground y%d | %s, %s: crossed the face %s, "
      "max x face%+.1f, end (face%+.1f, y%.1f), bottom y%.2f vs analytic ground "
      "top y%d (%+.2f, band [-0.5, %.1f]); %u drops (last fell %.1f vox), %u "
      "kept-outside steps, %u culled; (dx, y) every 10:%s",
      faceX, startY, hTop, alive ? "kept" : "GONE", asleep ? "asleep" : "AWAKE",
      outAt >= 0 ? Format("at tick %d", outAt).c_str() : "NEVER",
      maxX - (float)faceX, end.x - (float)faceX, end.y, bottom, ground + 1,
      above, band, s1.outsideDrops - s0.outsideDrops, s1.lastDropFell,
      s1.outsideKept - s0.outsideKept, s1.outsideCulled - s0.outsideCulled,
      trace.c_str());
  std::printf("debris-outside: %s %s\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

Status GateCorpseCap(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 380));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  mobs.Reset();
  c.debris.Reset();
  const int extra = (int)BaselineNumber("corpseCapExtra", 3);
  const int kills = (int)MobSystem::kMaxDeadMobs + extra;
  const uint64_t evicted0 = mobs.DeadEvictedTotal();
  std::vector<uint64_t> order;               // in death order
  std::vector<std::vector<uint64_t>> handles;  // each one's limbs at death
  std::vector<std::vector<int>> handleLimb;    // ...and which slot each was
  const int root = mobs.Defs()[t.defIndex].rootLimb;
  for (int k = 0; k < kills; k++) {
    const IVec3 site = FixtureSite(c.world, 200 + (k % 8) * 16);
    const uint64_t id =
        mobs.Spawn(t.defIndex, {site.x, site.y, site.z + (k / 8) * 16});
    if (!id) break;
    std::vector<uint64_t> hs;
    std::vector<int> hl;
    for (int li = 0; li < (int)mobs.Defs()[t.defIndex].limbs.size(); li++)
      if (const uint64_t h = mobs.LimbBody(id, li)) {
        hs.push_back(h);
        hl.push_back(li);
      }
    const uint64_t torso = root >= 0 ? mobs.LimbBody(id, root) : 0;
    if (torso) mobs.Damage(torso, 1.0e6f, mobs.LimbAnchorPos(id, root), 0.0f);
    if (mobs.IsAlive(id)) break;
    order.push_back(id);
    handles.push_back(std::move(hs));
    handleLimb.push_back(std::move(hl));
  }
  const bool fixtureOk = (int)order.size() == kills;
  // One tick: the cap is enforced at the top of PreTick (MobSystem::EvictDead).
  // DIRECT PHASE CALLS ON PURPOSE (W2-O): the subject is that one step of the
  // mob phase, and every count below is read before anything else in a tick
  // (the husk sweep, the step) could move a body.
  {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> sp;
    std::vector<CellOp> cellOps;
    mobs.PreTick(64000u, c.world, ops, cellOps, sp);
    c.debris.PreTick(64000u, c.world, cellOps, sp);
  }
  uint32_t deadBodies = 0;
  for (uint32_t i = 0; i < mobs.MobCount(); i++) {
    const Mob* m = mobs.MobAt(i);
    if (m && !m->Alive() && !m->RigReleased()) deadBodies += m->LimbBodyCount();
  }
  const uint32_t deadNow = mobs.DeadMobCount();
  const bool withinCaps = deadNow <= MobSystem::kMaxDeadMobs &&
                          deadBodies <= MobSystem::kMaxDeadBodies;
  // B: which ones went. A corpse still standing is a dead Mob with its rig.
  auto stillCorpse = [&](uint64_t id) {
    const Mob* m = mobs.FindMobById(id);
    return m != nullptr && !m->Alive() && !m->RigReleased();
  };
  int goneOldest = 0, firstKept = -1;
  for (int k = 0; k < (int)order.size(); k++) {
    if (!stillCorpse(order[(size_t)k])) {
      if (firstKept < 0) goneOldest++;
    } else if (firstKept < 0) {
      firstKept = k;
    }
  }
  // Every corpse after the first one kept must still be one: eviction takes
  // the oldest first, never one out of the middle.
  bool orderly = firstKept >= 0;
  for (int k = std::max(firstKept, 0); k < (int)order.size(); k++)
    orderly = orderly && stillCorpse(order[(size_t)k]);
  // ...and what went is DEBRIS now, not deleted: its limb bodies are loose dead
  // flesh the debris cull will finish.
  uint32_t decayedBodies = 0, decayedExpected = 0;
  // WHICH handles did not become loose dead flesh, and what they are now
  // (rule 6): limb slot, then "gone" (no debris body), "kept" (still a
  // creature's), or "debris-not-flesh".
  std::string notFlesh;
  for (int k = 0; k < goneOldest; k++)
    for (size_t j = 0; j < handles[(size_t)k].size(); j++) {
      const uint64_t h = handles[(size_t)k][j];
      decayedExpected++;
      // HAIR IS NOT FLESH. A bloodless slot (zeus's `hair` and `mane`, which
      // the human prototypes grew with d5ed2cb, 2026-09-25) goes to the debris
      // as loose matter like the rest, but it was never flesh and the debris
      // system rightly does not call it dead flesh. The claim for it is the
      // one that matters: it was handed over, not deleted.
      const bool bloodless =
          mobs.Defs()[t.defIndex].limbs[(size_t)handleLimb[(size_t)k][j]]
              .bloodless;
      if (c.debris.HasBody(h) && (bloodless || c.debris.BodyIsDeadFlesh(h))) {
        decayedBodies++;
        continue;
      }
      int slot = -1;
      std::string where = "gone";
      if (c.debris.HasBody(h)) where = "debris-not-flesh";
      else if (mobs.FindOwner(h, &slot) != nullptr) where = "kept";
      if (notFlesh.size() < 400)
        notFlesh += Format(
            " #%d %s:%s", k,
            mobs.Defs()[t.defIndex].limbs[(size_t)handleLimb[(size_t)k][j]]
                .name.c_str(),
            where.c_str());
    }
  const bool decayed = goneOldest >= extra && decayedExpected > 0 &&
                       decayedBodies == decayedExpected;
  const bool newestKept = !order.empty() && stillCorpse(order.back());
  const uint64_t evicted = mobs.DeadEvictedTotal() - evicted0;
  // C: the living cap, beside the dead.
  int live = 0;
  bool refusedAtCap = false;
  for (uint32_t k = 0; k <= MobSystem::MaxLiveMobs(); k++) {
    const IVec3 site = FixtureSite(c.world, 120 + (int)(k % 8) * 16);
    const uint64_t id = mobs.Spawn(
        t.defIndex, {site.x, site.y, site.z + 300 + (int)(k / 8) * 16});
    if (id) live++;
    else if (k == MobSystem::MaxLiveMobs()) refusedAtCap = true;
  }
  const bool livingCap =
      live == (int)MobSystem::MaxLiveMobs() && refusedAtCap &&
      mobs.LiveMobCount() == MobSystem::MaxLiveMobs() && mobs.DeadMobCount() > 0;
  RecordObserved("corpseCapEvicted", (double)evicted);
  mobs.Reset();
  c.debris.Reset();
  const bool ok = fixtureOk && withinCaps && orderly && decayed && newestKept &&
                  livingCap;
  detail = Format(
      "%s: %zu of %d killed; after one tick %u corpses (cap %u), %u corpse "
      "bodies (cap %u), %llu evicted; the first %d gone (need >= %d) with "
      "%u/%u of their limbs loose dead flesh, oldest-first=%d, newest kept=%d; "
      "%d living spawned beside the dead (cap %u), next refused=%d",
      t.defName.c_str(), order.size(), kills, deadNow, MobSystem::kMaxDeadMobs,
      deadBodies, MobSystem::kMaxDeadBodies, (unsigned long long)evicted,
      goneOldest, extra, decayedBodies, decayedExpected, orderly ? 1 : 0,
      newestKept ? 1 : 0, live, MobSystem::MaxLiveMobs(), refusedAtCap ? 1 : 0);
  if (!notFlesh.empty()) detail += " | not loose dead flesh:" + notFlesh;
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// head-cleave: a blade takes a head when the blow can PAY for the neck
// ---------------------------------------------------------------------------
// The owner, 2026-09-25: "some sword hits [should] be able to just sever a head
// entirely upon killing a mob, but it has to depend on the size of the sword,
// how many voxels it chips, how well it hits the head ... and also give
// players the ability to deliberately sever heads with corpses by positioning
// the body around and purposefully striking the head/neck". Neither happened:
// a fixed-depth kerf could never come out the far side of a neck, and
// hand-aimed chops each opened their own slit beside the last one.
//
// phys/kerf.h now prices the plane a blow cuts along (KerfBite). The claims:
//
//   A. PRICE. The neck is the cheapest plane on the head, and a plane through
//      the skull costs several times more (bone at 3x skin). Measured with
//      no-cleave chips on fresh spawns; the neck's along-axis offset found
//      here is where B and C aim.
//   B. COMBAT. A perfect swing of the stock sword across a live neck goes
//      THROUGH: the head comes off by a blade and the creature dies of it. A
//      swing at headCleaveWeakPower does not, and a perfect swing into the
//      skull does not.
//   C. DELIBERATE. On a corpse, committed (not perfect) chops with hand-aim
//      jitter along the neck take the head off within headCorpseMaxChops on
//      average, and slow chops with no cleave in them still do within
//      headCorpseSlowMaxChops: patience pays, because every chip is cells no
//      longer in the plane.
namespace {

struct CleaveBlow {
  float along = 0.5f;   // world voxels from the head's anchor, along its axis
  float power = 1.0f;
  float heft = 1.0f;
  float edgeHalf = 2.0f;
  float halfWidth = 0.1f;
  float spin = 0.0f;    // radians about the head's axis (approach side)
  float tilt = 0.0f;    // radians the cut plane leans off square
  uint32_t seed = 0;
};

Vec3 RotateAbout(Vec3 p, Vec3 axis, float ang) {
  const float c = std::cos(ang), s = std::sin(ang);
  return p * c + axis.cross(p) * s + axis * (axis.dot(p) * (1.0f - c));
}

// One blade blow on the head, built by the same lines melee.cpp's
// BuildStrikeParts builds one with (the cleave included), so retuning moves
// the gate and the game together.
bool LandCleave(MobSystem& mobs, World& world, uint64_t id, int head,
                const CleaveBlow& b, std::vector<ParticleSpawn>& spawns) {
  const LimbAxis ax = MeasureLimb(mobs, id, head);
  const uint64_t body = mobs.LimbBody(id, head);
  if (!body || !ax.valid) return false;
  const auto& g = CurrentTuning().gore;
  Vec3 edge = RotateAbout(ax.edge, ax.along, b.spin);
  Vec3 travel = RotateAbout(ax.travel, ax.along, b.spin);
  travel = RotateAbout(travel, edge, b.tilt);
  BladeCut cut;
  cut.at = ax.anchor + ax.along * b.along;
  cut.edgeAxis = edge;
  cut.cutDir = travel;
  cut.halfWidth = b.halfWidth;
  cut.depth = (g.cutDepth + g.cutDepthPower * b.power) * b.heft;
  cut.length = g.cutLength * (0.4f + 0.6f * b.power) * b.heft;
  cut.edgeHalf = b.edgeHalf;
  const float from = std::clamp(g.cleaveFrom, 0.0f, 0.95f);
  const float x = std::clamp((b.power - from) / (1.0f - from), 0.0f, 1.0f);
  cut.cleave = std::max(g.cleaveArea, 0.0f) * b.heft * x * x;
  cut.power = b.power;
  cut.seed = b.seed;
  return mobs.CutLimb(body, cut, world, spawns, b.power);
}

}  // namespace

Status GateHeadCleave(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  // THE IDS ARE THE GATE'S OWN, not whatever the gates before it left. C's
  // ragged rim is keyed on the creature id (the note at `pooled` below), so
  // with the counter inherited the same tree measured mean 8.5 chops in the
  // suite and 8.3 in a --verify subset (2026-10-01, HEAD 11413bc) -- a pooled
  // claim whose sample moved with the gate list. Every spawn here goes through
  // SpawnTarget's Reset, so no live creature can share a pinned id; the scope
  // above puts the suite's counter back on the way out.
  mobs.SetNextIdCounter(1);
  PrepareWorld(c);
  constexpr int kInset = 260;
  // THE HUMAN, by name: the claim is about a man's neck, and the fallback of
  // "the first def with a vital severable limb" could land on anything.
  Target t;
  for (size_t d = 0; d < mobs.Defs().size() && !t.valid(); d++) {
    const MobDef& def = mobs.Defs()[d];
    if (def.name != "human") continue;
    for (size_t li = 0; li < def.limbs.size(); li++)
      if (def.limbs[li].vital && def.limbs[li].severable &&
          (int)li != def.rootLimb) {
        t.defIndex = (int)d;
        t.limb = (int)li;
        t.defName = def.name;
        t.limbName = def.limbs[li].name;
        break;
      }
  }
  if (!t.valid()) {
    detail = "no 'human' def with a vital severable head";
    return Status::Skip;
  }
  const int head = t.limb;
  // The stock sword's own numbers: heft off its art, the edge it swings.
  const auto& g = CurrentTuning().gore;
  const ItemDef* sword = c.items.At(c.items.Find("sword"));
  const float heft =
      sword ? sword->HeftFactor(g.woundHeftRef, g.woundHeftMax) : 1.0f;
  const float edgeHalf =
      sword && sword->hasEdge ? (sword->edgeTo - sword->edgeFrom).len() * 0.5f
                              : 2.0f;
  const float halfWidth =
      std::max((sword ? sword->edgeHalfWidth : 0.5f) * g.cutWidth, g.cutWidthMin);
  std::vector<ParticleSpawn> spawns;
  IVec3 chunk{};
  auto fresh = [&]() -> uint64_t {
    const uint64_t id = SpawnTarget(c, t, kInset, chunk);
    mobs.ClearCutBites();
    return id;
  };
  auto headOn = [&](uint64_t id) { return mobs.LimbBody(id, head) != 0; };

  // ---- A. the price of every plane up the head ---------------------------
  float neckAlong = 0.5f, neckCost = 1e30f, reach = 1.0f;
  std::string prices;
  for (int k = 1; k <= 12; k++) {
    const uint64_t id = fresh();
    if (!id) {
      detail = "spawn refused";
      return Status::Fail;
    }
    reach = MeasureLimb(mobs, id, head).reach;
    const float along = 0.1f * (float)k;
    CleaveBlow b;
    b.along = along;
    b.power = 0.0f;  // a chip, no cleave: only the price is wanted
    b.heft = heft;
    b.edgeHalf = edgeHalf;
    b.halfWidth = halfWidth;
    b.seed = 0xC1EAu + (uint32_t)k;
    LandCleave(mobs, c.world, id, head, b, spawns);
    spawns.clear();
    const MobSystem::CutBite& cb = mobs.LastCutBite();
    prices += Format("%s%.1f:%.2f%s", k > 1 ? " " : "", (double)along,
                     (double)cb.planeCost, cb.blocked ? "!" : "");
    if (cb.planeCost > 0.0f && !cb.blocked && cb.planeCost < neckCost) {
      neckCost = cb.planeCost;
      neckAlong = along;
    }
  }
  const float skullAlong = std::max(neckAlong + 1.0f, reach * 0.6f);
  float skullCost = 0.0f;
  {
    const uint64_t id = fresh();
    CleaveBlow b;
    b.along = skullAlong;
    b.power = 0.0f;
    b.heft = heft;
    b.edgeHalf = edgeHalf;
    b.halfWidth = halfWidth;
    b.seed = 0x5C11u;
    LandCleave(mobs, c.world, id, head, b, spawns);
    spawns.clear();
    skullCost = mobs.LastCutBite().planeCost;
  }
  const double skullRatio =
      (double)BaselineNumber("headCleaveSkullRatio", 2.0);
  const bool priceOk = neckCost < 1e29f && skullCost >= skullRatio * neckCost;

  // ---- B. one swing on a live neck ----------------------------------------
  std::string why;
  bool lastTrim = false;
  auto oneBlow = [&](float along, float power, bool& died, float& budget,
                     float& cost, bool& byBlade) {
    const uint64_t id = fresh();
    mobs.ClearSeverEvents();
    mobs.ClearCutBites();
    CleaveBlow b;
    b.along = along;
    b.power = power;
    b.heft = heft;
    b.edgeHalf = edgeHalf;
    b.halfWidth = halfWidth;
    b.seed = 0xB10Bu;
    LandCleave(mobs, c.world, id, head, b, spawns);
    spawns.clear();
    budget = mobs.LastCutBite().budget;
    cost = mobs.LastCutBite().planeCost;
    const MobSystem::CutBite& cb = mobs.LastCutBite();
    why += Format(" [through=%d blocked=%d depth %.2f; %u pieces, %u of %u "
                  "left, kept %u; neck %u/%u; trimmed %u below %.2f]",
                  cb.through ? 1 : 0, cb.blocked ? 1 : 0, (double)cb.depth,
                  cb.comps, cb.parted, cb.before, cb.kept, cb.neckNow,
                  cb.neckSpawn, cb.neckTrimmed, (double)cb.skullBase);
    // The trim was ASKED (a skull base was found) and left no neck behind on
    // the head -- 0 trimmed is right when the cut was already at the base.
    lastTrim = cb.skullBase > 0.0f && cb.neckLeft == 0;
    died = !mobs.IsAlive(id);
    byBlade = false;
    for (const MobSystem::SeverEvent& se : mobs.SeverEvents())
      if (se.mobId == id && se.limbIndex == head && se.byBlade) byBlade = true;
    return !headOn(id);
  };
  const float weakPower = (float)BaselineNumber("headCleaveWeakPower", 0.8);
  bool diedP = false, diedW = false, diedS = false;
  bool bladeP = false, bladeW = false, bladeS = false;
  float budP = 0, costP = 0, budW = 0, costW = 0, budS = 0, costS = 0;
  // EVERY PLANE OF THE NECK, not only the cheapest: the cheapest is the
  // skin cap at the very bottom of the model, inside the collar, and a claim
  // about "a swing across the neck" must hold wherever on the neck it lands.
  const float bandLo = (float)BaselineNumber("headCleaveNeckFrom", 0.3);
  const float bandHi = (float)BaselineNumber("headCleaveNeckTo", 0.9);
  bool offPerfect = true, offWeak = false;
  for (float a = bandLo; a <= bandHi + 1e-3f; a += 0.2f) {
    bool d = false, bl = false;
    float bu = 0, co = 0;
    const bool off = oneBlow(a, 1.0f, d, bu, co, bl);
    // ...and it took the SKULL, not the neck with it (Mob::TrimNeckForSever).
    offPerfect = offPerfect && off && d && bl && lastTrim;
    budP = bu;
    costP = std::max(costP, co);
    diedP = d;
    bladeP = bl;
    offWeak = oneBlow(a, weakPower, d, budW, co, bl) || offWeak;
    costW = std::max(costW, co);
  }
  const bool offSkull = oneBlow(skullAlong, 1.0f, diedS, budS, costS, bladeS);
  const bool combatOk = offPerfect && diedP && bladeP && !offWeak && !offSkull;

  // ---- C. deliberate chops at a corpse's neck -----------------------------
  const int cap = (int)BaselineNumber("headCorpseMaxChops", 8);
  const float chopPower = (float)BaselineNumber("headCorpseChopPower", 0.75);
  const float jitAlong = (float)BaselineNumber("headCorpseJitter", 0.2);
  const float jitSpin = 0.35f, jitTilt = 0.15f;  // radians: ~20 and ~9 deg
  auto chopRun = [&](uint32_t run, float power, int limit, int& chops,
                     std::string& trace) {
    const uint64_t id = fresh();
    const MobDef& def = mobs.Defs()[t.defIndex];
    const uint64_t torso =
        def.rootLimb >= 0 ? mobs.LimbBody(id, def.rootLimb) : 0;
    // Killed the way corpse-dismember kills: the rig stays the corpse's.
    if (torso)
      mobs.Damage(torso, 1.0e6f, mobs.LimbAnchorPos(id, head), 45.0f,
                  DamageCtx(DamageCause::Blade, 1.0f));
    const bool dead = !mobs.IsAlive(id) && headOn(id);
    mobs.ClearCutBites();
    chops = 0;
    for (int i = 0; i < limit && dead && headOn(id); i++) {
      auto r = [&](uint32_t salt) {
        const uint32_t h = rng::Hash3(0xDECAu + run, (uint32_t)i, salt);
        return (float)(h & 0xFFFFu) / 65535.0f * 2.0f - 1.0f;
      };
      CleaveBlow b;
      b.along = neckAlong + r(1) * jitAlong;
      b.power = power;
      b.heft = heft;
      b.edgeHalf = edgeHalf;
      b.halfWidth = halfWidth;
      b.spin = r(2) * jitSpin;
      b.tilt = r(3) * jitTilt;
      b.seed = 0xC4A0u + run * 7919u + (uint32_t)i * 40503u;
      LandCleave(mobs, c.world, id, head, b, spawns);
      spawns.clear();
      chops++;
      trace += Format(" %.2f", (double)mobs.LastCutBite().planeCost);
    }
    return dead && !headOn(id);
  };
  // POOLED OVER SEVERAL WANDERS. One run is a lottery: the ragged rim is keyed
  // on the creature's id, so the same aim parts a neck in 9 chops at one
  // fixture and 18 at another. So: every wander must part the neck, and the
  // MEAN is held under a cap, at two chop powers -- a committed chop (the cleave
  // does most of it) and a slow one (chip after chip, no cleave at all).
  const int runs = std::max(1, (int)BaselineNumber("headCorpseRuns", 6));
  // A wander that never parts the neck counts its whole allowance (3x cap).
  auto pooled = [&](float power, int armCap, int& parted,
                    std::string& trace) {
    int total = 0;
    parted = 0;
    for (int r = 0; r < runs; r++) {
      int ch = 0;
      std::string tr;
      if (chopRun((uint32_t)r, power, armCap * 3, ch, tr)) parted++;
      total += ch;
      trace += Format(" %d", ch);
    }
    return (double)total / runs;
  };
  const float slowPower = (float)BaselineNumber("headCorpseSlowPower", 0.5);
  const int slowCap = (int)BaselineNumber("headCorpseSlowMaxChops", 30);
  int partedG = 0, partedS = 0;
  std::string traceG, traceS;
  const double meanG = pooled(chopPower, cap, partedG, traceG);
  const double meanS = pooled(slowPower, slowCap, partedS, traceS);
  const bool corpseOk = partedG == runs && partedS == runs && meanG >= 2.0 &&
                        meanG <= (double)cap && meanS <= (double)slowCap;
  RecordObserved("headCleaveNeckCost", (double)neckCost);
  RecordObserved("headCleaveSkullCost", (double)skullCost);
  RecordObserved("headCorpseChops", meanG);
  RecordObserved("headCorpseSlowChops", meanS);
  mobs.Reset();
  c.debris.Reset();

  const bool ok = priceOk && combatOk && corpseOk;
  detail = Format(
      "sword heft %.2f edge +-%.2f vox. A price [along:cost] %s -> neck %.2f "
      "at %.1f, skull %.2f at %.1f (need >= %.1fx). B perfect swing: budget "
      "%.2f vs %.2f -> head %s, died=%d byBlade=%d; power %.2f: budget %.2f vs "
      "%.2f -> head %s; skull: budget %.2f vs %.2f -> head %s. C corpse, "
      "jitter +-%.2f vox, %d wanders: power %.2f %d/%d parted, mean %.1f chops "
      "(cap %d); power %.2f %d/%d parted, mean %.1f (cap %d)",
      (double)heft, (double)edgeHalf, prices.c_str(), (double)neckCost,
      (double)neckAlong, (double)skullCost, (double)skullAlong, skullRatio,
      (double)budP, (double)costP, offPerfect ? "OFF" : "on", diedP ? 1 : 0,
      bladeP ? 1 : 0, (double)weakPower, (double)budW, (double)costW,
      offWeak ? "OFF" : "on", (double)budS, (double)costS,
      offSkull ? "OFF" : "on", (double)jitAlong, runs, (double)chopPower,
      partedG, runs, meanG, cap, (double)slowPower, partedS, runs, meanS,
      slowCap);
  detail += " | B attribution:" + why + " | C chops per wander:" + traceG +
            " | slow:" + traceS;
  return ok ? Status::Pass : Status::Fail;
}

// ============================================================================
// bruise-is-skin: a bruise is the SKIN's, not a coat (owner report 2026-09-26)
//
// "Bruises shouldn't be stains but changes to the skin; they shouldn't wash
// off with water and should be able to be stained with other things; and a
// bruise hit again gets worse until the micro voxels bleed and break up."
//
// Pure lattice arithmetic -- SoakBruise, the washer's rule, the coat raise and
// the micro brick's stain lattice -- over a synthetic block of skin, because
// every claim here is about which FIELD a rule writes, and a world, a rig and
// a tick would add nothing but ways for the answer to be about something else.
// The living ladder over a real limb is impact-blunt's; the corpse's is
// corpse-blunt's. Claims:
//   A  one blow bruises and splits nothing;
//   B  repeat blows DEEPEN the same cells, then split the saturated ones, and
//      a split cell wears this body's blood;
//   C  washing every coat off (the rule rain and rivers use) leaves every
//      bruise level exactly as it was -- in the lattice AND in the brick;
//   D  another substance's coat goes on OVER a bruise without touching it;
//   E  the next blow still finds the split cells (washing did not un-beat the
//      skin) and they BLEED AGAIN.
// ============================================================================
Status GateBruiseIsSkin(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  const uint32_t skin = mobs.MaterialIdNamed("skin");
  const uint32_t blood = mobs.MaterialIdNamed("blood");
  const uint32_t water = mobs.MaterialIdNamed("water");
  const uint32_t oil = mobs.MaterialIdNamed("oil");
  if (!skin || !blood || !water || !oil || skin > 255) {
    detail = Format("materials missing: skin %u blood %u water %u oil %u", skin,
                    blood, water, oil);
    return Status::Fail;
  }
  const auto& gt = CurrentTuning().gore;
  constexpr int N = 16;
  std::vector<PrefabVoxel> lat;
  for (int z = 0; z < N; z++)
    for (int y = 0; y < N; y++)
      for (int x = 0; x < N; x++) {
        PrefabVoxel v{};
        v.x = (int16_t)x; v.y = (int16_t)y; v.z = (int16_t)z;
        v.material = (uint16_t)skin;
        lat.push_back(v);
      }
  // A private brick with a known look table, so the check reads real words.
  // The bruise draws in `skin` here only because it is a material the table
  // says has a look; any drawn id would do.
  MicroBodySet set;
  std::vector<uint8_t> draws(std::max({skin, blood, water, oil}) + 1u, 0);
  draws[blood] = 1; draws[water] = 1; draws[oil] = 1; draws[skin] = 1;
  MicroBodySetCoatLooks(set, draws, /*bruiseMat=*/(uint16_t)skin);
  std::string log;
  const int model = MicroBodyPack(set, lat, IVec3{N, N, N}, 8, "bruise-is-skin", log);
  if (model < 0) {
    detail = "brick pack refused: " + log;
    return Status::Fail;
  }
  const int own = MicroBodyOwn(set, (uint32_t)model);
  if (own < 0) {
    detail = "brick own refused";
    return Status::Fail;
  }
  // The brick's stain word for lattice voxel i (coat half low, bruise high).
  auto brickCell = [&](size_t i) -> uint32_t {
    const MicroBodyModelGpu& m = set.models[(size_t)own];
    if (!(m.dims & kMicroBodyDimsStainBit)) return 0;
    const size_t cells = (size_t)N * N * N;
    const size_t idx = ((size_t)lat[i].z * N + lat[i].y) * N + lat[i].x;
    return set.pool[m.base + (cells + 1) / 2 + idx];
  };

  StainLattice L;
  L.skin = &lat;
  BruiseSoak bs;
  bs.centre = Vec3{N * 0.5f, (float)N, N * 0.5f};   // on the top face
  bs.radius = 7.0f;
  bs.bloodMat = blood;
  bs.step = std::max(1.0f, gt.bruiseStep);
  bs.cap = (uint32_t)std::lround(std::clamp(gt.bruiseMax, 2.0f, 14.0f));
  bs.bleedFrom = (uint32_t)std::lround(
      (float)bs.cap * std::clamp(gt.bruiseBleedFrom, 0.0f, 1.0f));
  bs.bleedChance = 0.5f;   // an arm, not the shipped odds: the rung must fire
  bs.blowScale = 1.0f;
  auto blow = [&](uint32_t k) {
    bs.seed = 0xB2015Eu + k * 2654435761u;
    BruiseTally t;
    SoakBruise(L, bs, &t, &set, own);
    return t;
  };
  auto count = [&](uint32_t minLvl) {
    uint32_t n = 0;
    for (const PrefabVoxel& v : lat) n += BruiseLevel(v.bruise) >= minLvl;
    return n;
  };

  // A
  blow(0);
  const uint32_t bruised1 = count(1), deep1 = count(bs.cap), split1 = count(kBruiseBroken);
  bool ok = true;
  std::string why;
  if (bruised1 == 0) { ok = false; why += " A:first blow marked nothing"; }
  if (split1 != 0) { ok = false; why += " A:first blow split skin"; }

  // B
  uint32_t k = 1;
  for (; k < 12 && count(kBruiseBroken) == 0; k++) blow(k);
  const uint32_t deepB = count(bs.cap), splitB = count(kBruiseBroken);
  uint32_t splitBleeding = 0;
  for (const PrefabVoxel& v : lat)
    if (BruiseBroken(v.bruise) && BodyStainMat(v.stain) == blood) splitBleeding++;
  if (deepB <= deep1) { ok = false; why += " B:blows did not deepen"; }
  if (splitB == 0) { ok = false; why += " B:never split"; }
  if (splitBleeding != splitB) { ok = false; why += " B:a split cell is dry"; }

  // brick mirrors the lattice
  uint32_t brickMismatch = 0;
  for (size_t i = 0; i < lat.size(); i++) {
    const uint32_t lvl = BruiseLevel(lat[i].bruise);
    const uint32_t want = (uint32_t)PackBodyStain(skin, lvl) << 16;
    if ((brickCell(i) & 0xFFFF0000u) != want) brickMismatch++;
  }
  if (brickMismatch) { ok = false; why += " brick bruise byte disagrees"; }

  // C: wash everything, the way water does, until nothing foreign is left.
  std::vector<uint8_t> before(lat.size());
  for (size_t i = 0; i < lat.size(); i++) before[i] = lat[i].bruise;
  std::vector<uint32_t> brickHiBefore(lat.size());
  for (size_t i = 0; i < lat.size(); i++) brickHiBefore[i] = brickCell(i) & 0xFFFF0000u;
  for (int pass = 0; pass < 16; pass++)
    for (size_t i = 0; i < lat.size(); i++) {
      const uint16_t next = WashBodyStain(lat[i].stain, water, 4, 15);
      lat[i].stain = next;
      MicroBodyPokeStain(set, (uint32_t)own, lat[i].x, lat[i].y, lat[i].z, next);
    }
  uint32_t washedBlood = 0, bruiseMoved = 0, brickMoved = 0;
  for (size_t i = 0; i < lat.size(); i++) {
    washedBlood += BodyStainMat(lat[i].stain) == blood;
    bruiseMoved += lat[i].bruise != before[i];
    brickMoved += (brickCell(i) & 0xFFFF0000u) != brickHiBefore[i];
  }
  if (washedBlood) { ok = false; why += " C:blood survived the wash"; }
  if (bruiseMoved) { ok = false; why += " C:the wash moved a bruise"; }
  if (brickMoved) { ok = false; why += " C:the wash moved a brick bruise byte"; }

  // D: oil over everything.
  for (size_t i = 0; i < lat.size(); i++) {
    const uint16_t next = PackBodyStain(oil, 10);
    lat[i].stain = next;
    MicroBodyPokeStain(set, (uint32_t)own, lat[i].x, lat[i].y, lat[i].z, next);
  }
  uint32_t oilMoved = 0, oilOnBruise = 0;
  for (size_t i = 0; i < lat.size(); i++) {
    oilMoved += lat[i].bruise != before[i] ||
                (brickCell(i) & 0xFFFF0000u) != brickHiBefore[i];
    oilOnBruise += BruiseLevel(lat[i].bruise) > 0 &&
                   (brickCell(i) & 0xFFFFu) == PackBodyStain(oil, 10);
  }
  if (oilMoved) { ok = false; why += " D:a coat moved a bruise"; }
  if (oilOnBruise == 0) { ok = false; why += " D:no coat sits on a bruise"; }

  // E
  const BruiseTally tE = blow(k + 1);
  uint32_t rebled = 0;
  for (const PrefabVoxel& v : lat)
    if (BruiseBroken(v.bruise) && BodyStainMat(v.stain) == blood) rebled++;
  if (tE.pulped == 0) { ok = false; why += " E:the wash un-beat the skin"; }
  if (rebled == 0) { ok = false; why += " E:split skin did not bleed again"; }

  detail = Format(
      "A bruised %u after 1 blow (split %u) | B after %u blows: %u at cap %u "
      "(was %u), %u split, %u of them bleeding | brick mismatches %u | C wash: "
      "%u blood left, %u bruises moved, %u brick bytes moved | D oil: %u moved, "
      "%u bruised cells wearing it | E next blow: pulped %u/%u core, %u split "
      "cells bleeding again%s",
      bruised1, split1, k, deepB, bs.cap, deep1, splitB, splitBleeding,
      brickMismatch, washedBlood, bruiseMoved, brickMoved, oilMoved, oilOnBruise,
      tE.pulped, tE.core, rebled, why.empty() ? "" : (" |" + why).c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// blade-wounds: a sword and a dagger, slashed and stabbed, through the sweep
// ---------------------------------------------------------------------------
// The owner, 2026-09-26: "swords, daggers and cuts don't seem to be chipping /
// removing voxels. I want a stab to remove some voxels as if it's a stab
// wound." Every other wound gate builds its KerfCut by hand (CutOnce), so none
// of them could see what a REAL blow's kerf is made of -- the dagger's derived
// heft (0.2) scales a slash's depth under half a skin cell.
//
// FIXTURE. Two creatures (MeleeSweepDamage refuses to cut its wielder); the
// SHIPPED sword and dagger (profile, edge half-width, carve bonus, heft); one
// EdgeSweep per blow at `bladeWoundSpeed` of melee.fullSpeed, into the middle
// of the target limb along its measured cross-section axis. A SLASH moves the
// blade across itself; a STAB moves it hilt -> point along itself. Each arm is
// a fresh spawn. The stab arms run twice, the second with gore.stabAlign
// above 1 (stabs off: the old thrust-as-kerf path) as the control.
//
// Asserted: every arm lands; each stab takes at least bladeStabMinVox art
// voxels in one blow and more than its control; a stab held in the limb for a
// second tick of the same stroke carves nothing more (once per stroke).
// Slash counts are REPORTED and recorded, and asserted only against
// bladeSlashMinVox (0 = report only).
Status GateBladeWounds(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 170));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  const ItemDef* blades[2] = {c.items.At(c.items.Find("sword")),
                              c.items.At(c.items.Find("dagger"))};
  if (!blades[0] || !blades[1]) {
    detail = "the item library has no sword or no dagger";
    return Status::Skip;
  }
  const Tuning base = CurrentTuning();
  const auto& g = base.gore;
  MeleeTuning mt;
  ApplyMeleeTuning(mt);
  const float speed = (float)BaselineNumber("bladeWoundSpeed", 0.8);
  const float step = mt.fullSpeed * kTickDt * speed;
  const int kBlows = (int)BaselineNumber("bladeWoundBlows", 3);

  struct Arm {
    uint32_t firstLost = 0, lost = 0, heldExtra = 0;
    int landed = 0;
    float power = 0.0f;
    bool severed = false, ran = false;
  };
  // mode 0 = slash, 1 = stab, 2 = stab with stabs OFF (control).
  auto runArm = [&](const ItemDef& it, int mode) -> Arm {
    Arm a;
    Tuning tu = base;
    if (mode == 2) tu.gore.stabAlign = 2.0f;
    SetCurrentTuning(tu);
    mobs.Reset();
    c.debris.Reset();
    const uint64_t wid = mobs.Spawn(t.defIndex, FixtureSite(c.world, 225));
    const uint64_t id = mobs.Spawn(t.defIndex, FixtureSite(c.world, 170));
    if (!wid || !id) { SetCurrentTuning(base); return a; }
    // DIRECT PHASE CALLS ON PURPOSE (W2-O): fixture posing (SpawnTarget's).
    for (int i = 0; i < 8; i++) {
      std::vector<BrushOp> ops;
      std::vector<ParticleSpawn> st;
      std::vector<CellOp> cellOps;
      mobs.PreTick(1000u + (uint32_t)i, c.world, ops, cellOps, st);
      c.phys.Step(kTickDt);
      mobs.PostStep();
    }
    mobs.ClearSeverEvents();
    mobs.ClearSeverStats();
    Mob* wielder = mobs.FindMobById(wid);
    if (!wielder) { SetCurrentTuning(base); return a; }
    a.ran = true;
    const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
    const Vec3 mid = ax.anchor + ax.along * (ax.reach * 0.5f);
    const uint32_t before = mobs.LimbArtVoxelCount(id, t.limb);
    std::vector<ParticleSpawn> spawns;
    const float L = 3.0f;   // world voxels of edge; enough to span the limb
    auto sweep = [&](const Vec3& a0, const Vec3& b0, const Vec3& a1,
                     const Vec3& b1, const Vec3& flat, uint32_t tick,
                     std::vector<uint64_t>* struck) {
      EdgeSweep sw;
      sw.aPrev = a0; sw.bPrev = b0; sw.aNow = a1; sw.bNow = b1;
      sw.flatNow = flat;
      sw.dt = kTickDt;
      sw.halfWidth = it.edgeHalfWidth;
      sw.carveBonus = it.carveBonus;
      sw.strike = it.strike;
      sw.heft = it.HeftFactor(g.woundHeftRef, g.woundHeftMax);
      sw.tick = tick;
      sw.struck = struck;
      sw.valid = true;
      return MeleeSweepDamage(sw, mt, *wielder, c.phys, mobs, c.debris,
                              c.world, spawns);
    };
    for (int k = 0; k < kBlows && mobs.LimbBody(id, t.limb); k++) {
      std::vector<uint64_t> struck;
      EdgeSweepResult r;
      if (mode == 0) {
        // Across the limb: the edge along `ax.edge`, travelling `ax.travel`,
        // the flat's normal therefore along the limb (edge-on, full power).
        r = sweep(mid - ax.edge * L - ax.travel * step,
                  mid + ax.edge * L - ax.travel * step, mid - ax.edge * L,
                  mid + ax.edge * L, ax.along, 7000u + (uint32_t)k, &struck);
      } else {
        // Point first along `ax.travel`, the tip ending at the limb's middle;
        // the flat faces along the limb, so the slit runs ACROSS it.
        const Vec3 tip0 = mid - ax.travel * step, tip1 = mid;
        r = sweep(tip0 - ax.travel * L, tip0, tip1 - ax.travel * L, tip1,
                  ax.along, 7000u + (uint32_t)k, &struck);
        // ...and the blade stays in for one more tick of the SAME stroke,
        // pushing on a little: it must not stab again.
        if (k == 0 && mobs.LimbBody(id, t.limb)) {
          const uint32_t n1 = mobs.LimbArtVoxelCount(id, t.limb);
          const Vec3 tip2 = mid + ax.travel * (step * 0.25f);
          sweep(tip1 - ax.travel * L, tip1, tip2 - ax.travel * L, tip2,
                ax.along, 7100u, &struck);
          const uint32_t n2 = mobs.LimbBody(id, t.limb)
                                  ? mobs.LimbArtVoxelCount(id, t.limb)
                                  : 0u;
          a.heldExtra = n1 > n2 ? n1 - n2 : 0u;
        }
      }
      if (r.bodiesHit > 0) a.landed++;
      a.power = std::max(a.power, r.power * r.edgeAlign);
      if (k == 0) {
        const uint32_t n = mobs.LimbBody(id, t.limb)
                               ? mobs.LimbArtVoxelCount(id, t.limb)
                               : 0u;
        a.firstLost = before > n ? before - n : 0u;
        // The held tick above counted separately, not as the first blow's.
        a.firstLost -= std::min(a.firstLost, a.heldExtra);
      }
    }
    a.severed = mobs.LimbBody(id, t.limb) == 0;
    const uint32_t after =
        a.severed ? 0u : mobs.LimbArtVoxelCount(id, t.limb);
    a.lost = before > after ? before - after : 0u;
    mobs.Reset();
    c.debris.Reset();
    SetCurrentTuning(base);
    return a;
  };

  const char* names[2] = {"sword", "dagger"};
  const double stabMin = BaselineNumber("bladeStabMinVox", 12);
  const double slashMin = BaselineNumber("bladeSlashMinVox", 0);
  bool ok = true;
  std::string why, line;
  for (int b = 0; b < 2; b++) {
    const Arm slash = runArm(*blades[b], 0);
    const Arm stab = runArm(*blades[b], 1);
    const Arm ctl = runArm(*blades[b], 2);
    const std::string nm = names[b];
    RecordObserved(("bladeSlashFirstVox_" + nm).c_str(), (double)slash.firstLost);
    RecordObserved(("bladeStabFirstVox_" + nm).c_str(), (double)stab.firstLost);
    RecordObserved(("bladeStabOffFirstVox_" + nm).c_str(), (double)ctl.firstLost);
    auto fail = [&](const std::string& s) { ok = false; why += " | " + nm + ": " + s; };
    if (!slash.ran || !stab.ran || !ctl.ran) fail("spawn refused");
    if (slash.landed == 0) fail("slash never landed");
    if (stab.landed == 0) fail("stab never landed");
    if ((double)stab.firstLost < stabMin) fail("stab took too little");
    if (stab.firstLost <= ctl.firstLost) fail("stab no bigger than stabs-off");
    if (stab.heldExtra > 0) fail("held blade stabbed again");
    if (slashMin > 0 && (double)slash.firstLost < slashMin)
      fail("slash took too little");
    line += Format(
        "%s (heft %.2f): slash %u first / %u in %d (%d landed, power %.2f%s); "
        "stab %u first / %u (%d landed, power %.2f, held +%u%s); stabs-off "
        "%u first / %u. ",
        nm.c_str(), blades[b]->HeftFactor(g.woundHeftRef, g.woundHeftMax),
        slash.firstLost, slash.lost, kBlows, slash.landed, slash.power,
        slash.severed ? ", SEVERED" : "", stab.firstLost, stab.lost,
        stab.landed, stab.power, stab.heldExtra,
        stab.severed ? ", SEVERED" : "", ctl.firstLost, ctl.lost);
  }
  detail = Format("%s/%s at %.2f speed: %s(need stab >= %.0f%s)%s",
                  t.defName.c_str(), t.limbName.c_str(), speed, line.c_str(),
                  stabMin,
                  slashMin > 0 ? Format(", slash >= %.0f", slashMin).c_str()
                               : "",
                  why.c_str());
  std::printf("blade-wounds: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// wound-rebleed: a bleeding wound washed clean fills back up with blood
// ---------------------------------------------------------------------------
// The owner, 2026-09-26: "I can stab someone, reveal their ribcage, then clean
// up the wound with water, and the wound will still be bleeding voxels into
// the world, but the area directly around the wound should still get stained
// with blood." The cut's smear used to be laid once, on the hit tick.
//
// FIXTURE. A living target limb is cut (CutOnce) and hit (Damage, which opens
// the bleed budget and puts the wound where the cut is); the whole limb is
// then washed with water (SoakLimb, which scrubs every coat), and THE REAL
// TICK runs for woundRebloodTicks * 2 + 2 ticks. Two arms on fresh spawns:
// re-bleed on, and gore.woundRebloodTicks 0 (off) as the control -- the drip's
// own spray can splatter the creature, and this separates that from the rule.
//
// Asserted: the wound is still bleeding after the wash, the wash really took
// the blood off, and the ON arm has more
// blood-coated voxels than the control
// (baseline woundRebleedMin).
Status GateWoundRebleed(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, 170));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb that bleeds";
    return Status::Fail;
  }
  const uint32_t blood = mobs.Defs()[t.defIndex].bleedMat;
  const uint32_t water = mobs.MaterialIdNamed("water");
  if (!blood || !water) {
    detail = "no blood or no water material";
    return Status::Fail;
  }
  const Tuning base = CurrentTuning();
  struct Arm {
    uint32_t cut = 0, washed = 0, after = 0;
    float budgetWashed = 0.0f;
    bool ran = false, alive = false;
  };
  auto runArm = [&](bool on) -> Arm {
    Arm a;
    Tuning tu = base;
    if (!on) tu.gore.woundRebloodTicks = 0;
    SetCurrentTuning(tu);
    // Pristine ground PER ARM: the first arm's blood is still in the world
    // otherwise, and the second target would be splattered by it.
    PrepareWorld(c);
    IVec3 pchunk{};
    const uint64_t id = SpawnTarget(c, t, 170, pchunk);
    if (!id) { SetCurrentTuning(base); return a; }
    const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
    std::vector<ParticleSpawn> spawns;
    // Damage FIRST, as the sweep does: it opens the bleed budget, and the cut
    // after it moves the wound to the mouth of the slot (Mob::CutLimb).
    if (const uint64_t b0 = mobs.LimbBody(id, t.limb))
      mobs.Damage(b0, 8.0f, ax.anchor + ax.along * (ax.reach * 0.5f), 0.0f,
                  DamageCtx(DamageCause::Blade, 1.0f));
    CutOnce(mobs, c.world, id, t.limb, ax, ax.reach * 0.5f, 1.0f, 1.0f,
            0x2EB1Du, spawns);
    if (!mobs.LimbBody(id, t.limb)) {
      SetCurrentTuning(base);
      mobs.Reset();
      return a;
    }
    a.cut = mobs.LimbCoatMatCount(id, t.limb, blood, 1);
    mobs.SoakLimb(id, t.limb, water, 15, 40999u);
    a.washed = mobs.LimbCoatMatCount(id, t.limb, blood, 1);
    a.budgetWashed = mobs.LimbBleedBudget(id, t.limb);
    uint32_t simTick = 41000;
    support::TickCursor ticker{c, simTick, pchunk};
    const int n = std::max(1, base.gore.woundRebloodTicks) * 2 + 2;
    for (int i = 0; i < n; i++) ticker();
    a.alive = mobs.IsAlive(id) && mobs.LimbBody(id, t.limb) != 0;
    a.after = a.alive ? mobs.LimbCoatMatCount(id, t.limb, blood, 1) : 0u;
    a.ran = true;
    mobs.Reset();
    c.debris.Reset();
    SetCurrentTuning(base);
    return a;
  };
  const Arm on = runArm(true);
  const Arm off = runArm(false);
  const double need = BaselineNumber("woundRebleedMin", 8);
  RecordObserved("woundRebleedOn", (double)on.after);
  RecordObserved("woundRebleedOff", (double)off.after);
  std::string why;
  if (!on.ran || !off.ran) why += " | spawn refused";
  if (!on.alive || !off.alive) why += " | the limb or the creature did not last";
  if (on.budgetWashed < 1.0f) why += " | the wound was not bleeding after the wash";
  if (on.washed >= on.cut && on.cut > 0) why += " | the wash took no blood off";
  if ((double)on.after < (double)off.after + need)
    why += " | the washed wound did not bleed back";
  const bool ok = why.empty();
  detail = Format(
      "%s/%s: blood-coated voxels cut %u -> washed %u (budget %.1f vox) -> "
      "%u after %d ticks with re-bleed every %d, %u with it off (need +%.0f)%s",
      t.defName.c_str(), t.limbName.c_str(), on.cut, on.washed,
      on.budgetWashed, on.after,
      std::max(1, base.gore.woundRebloodTicks) * 2 + 2,
      base.gore.woundRebloodTicks, off.after, need, why.c_str());
  std::printf("wound-rebleed: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// heal-restore / heal-wound: A COAT THAT HEALS (alchemy package D, 2026-09-27)
// ---------------------------------------------------------------------------
//
// Enchanted blood and enchanted water carry `coat.restore` (materials.json):
// while a limb wears one, Mob::HealTick rebuilds it toward its authored recipe
// (missing cells grow back, cells that are not the recipe's are put back, then
// hp up to the burn cap) and PAYS for every cell out of the coat. Both gates
// are the largest severable non-vital limb (ChooseTarget), a dummy creature,
// THE tick (support::TickCursor).
namespace {
struct HealMats {
  uint32_t eBlood = 0, eWater = 0, blood = 0, water = 0, skin = 0, cooked = 0;
  bool ok() const { return eBlood && eWater && blood && water && skin && cooked; }
};
HealMats FindHealMats(const std::vector<MaterialDef>& mats) {
  HealMats h;
  for (size_t i = 0; i < mats.size(); i++) {
    const std::string& n = mats[i].name;
    if (n == "enchanted_blood") h.eBlood = (uint32_t)i;
    if (n == "enchanted_water") h.eWater = (uint32_t)i;
    if (n == "blood") h.blood = (uint32_t)i;
    if (n == "water") h.water = (uint32_t)i;
    if (n == "skin") h.skin = (uint32_t)i;
    if (n == "flesh_cooked") h.cooked = (uint32_t)i;
  }
  return h;
}
// Coat levels of `mat` on one limb, off a freshly forced ledger.
uint32_t HealCoatLevels(MobSystem& mobs, uint64_t id, int limb, uint32_t mat,
                        uint32_t tick) {
  mobs.RecountCoatOn(id, tick);
  const LimbCoat* lc = mobs.LimbCoatOf(id, limb);
  if (!lc) return 0;
  for (const CoatEntry& e : lc->top)
    if (e.mat == mat) return e.sumAmt;
  return 0;
}
// Lattice cells one coat level of `m` buys on this def's authoritative lattice.
double HealCellsPerLevel(const MobDef& def, const MaterialDef& m) {
  const double s = (double)std::max(def.skinScale, def.physScale);
  return (double)m.coatRestore * s * s * s;
}
// HOW SCATTERED the regrowth was (Mob::HealStats::orderD2: each grown cell's
// squared distance to the joint, in the order it grew). The FIRST 20% of the
// cells to grow back, each ranked by that distance among ALL the cells that
// grew (0 = the nearest the joint, 1 = the farthest): `meanRank` is their
// mean rank and `nearFrac` the fraction of them that are also among the
// nearest 20%. A front marching out from the joint reads ~0.1 and ~1.0; a
// wound filling everywhere at once reads ~0.5 and ~0.2.
struct HealScatter {
  uint32_t n = 0, first = 0;
  double meanRank = 0.0, nearFrac = 0.0;
};
// A PICTURE of one limb, for looking at a heal (SANDVOX_HEAL_SHOTS=1 on
// heal-restore: build/heal_<tag>.bmp). The --shot-mob recipe (main.cpp
// RunMobShot's `shoot`): bodies + micro bricks uploaded, a noon camera `dist`
// voxels out along `dir` from `target`, the world and the bodies drawn, read
// back. Diagnostic only; off by default because a gate should not write files
// nobody asked for.
void HealShot(Ctx& c, Vec3 target, Vec3 dir, float dist, const char* path) {
  if (MicroBodySet* mbs = c.debris.MicroSet(); mbs != nullptr && mbs->dirty)
    c.sim.UploadMicroBodies(c.ctx.queue, *mbs);
  BodyRegistry reg(c.debris, c.mobs, nullptr);
  std::vector<BodyXformGpu> xf;
  reg.BuildXforms(xf);
  if (!xf.empty())
    c.ctx.queue.WriteBuffer(c.world.bodyXforms, 0, xf.data(),
                            xf.size() * sizeof(BodyXformGpu));
  std::vector<MicroBodyInstGpu> microInsts;
  reg.BuildMicroInsts(microInsts);
  std::vector<BodyVoxInst> inst;
  reg.BuildInstances(inst);
  if (!inst.empty())
    c.ctx.queue.WriteBuffer(c.world.bodyInstances, 0, inst.data(),
                            inst.size() * sizeof(BodyVoxInst));
  const uint32_t W = 960, H = 720;
  const Vec3 eye = target + dir.normalized() * dist;
  const Vec3 look = (target - eye).normalized();
  Camera cam;
  cam.yaw = std::atan2(look.z, look.x);
  cam.pitch = std::asin(std::clamp(look.y, -1.0f, 1.0f));
  WriteRenderParams(c.ctx.queue, c.world, eye, cam, (float)W / H, true, 0.0f,
                    kFarFogDensity, (float)H, TicksPerDay(CurrentTuning()) / 2);
  rhi::Texture tex = c.ctx.device.CreateTexture(
      {W, H, 1}, rhi::TextureFormat::RGBA8Unorm,
      rhi::TextureUsage::RenderAttachment | rhi::TextureUsage::CopySrc,
      "healShot");
  const uint32_t microCount = c.sim.UploadMicroBodyInsts(c.ctx.queue, microInsts);
  rhi::CommandEncoder enc = c.ctx.device.CreateCommandEncoder();
  c.sim.EncodeShadowResolve(enc);
  rhi::RenderPass rp = c.sim.BeginRenderPass(enc, tex.CreateView(),
                                             rhi::TextureFormat::RGBA8Unorm, W, H);
  c.sim.DrawWorld(rp);
  c.sim.DrawBodies(rp, (uint32_t)inst.size());
  c.sim.DrawMicroBodies(rp, microCount);
  rp.End();
  rhi::Buffer buf = CreateBuffer(c.ctx.device, (uint64_t)W * H * 4,
                                 rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                                 "healShotRead");
  rhi::TexelCopyTexture srcT{};
  srcT.texture = tex;
  rhi::TexelCopyBuffer dstB{};
  dstB.buffer = buf;
  dstB.bytesPerRow = W * 4;
  dstB.rowsPerImage = H;
  enc.CopyTextureToBuffer(srcT, dstB, rhi::Extent3D{W, H, 1});
  c.ctx.queue.Submit(enc.Finish());
  std::vector<uint8_t> px((size_t)W * H * 4, 0);
  if (rhi::ReadBufferBlocking(c.ctx.device, buf, 0, px.data(), px.size()) &&
      WriteBmpFile(path, px, W, H))
    std::printf("wrote %s\n", path);
}
HealScatter HealOrderScatter(const std::vector<float>& d2) {
  HealScatter r;
  r.n = (uint32_t)d2.size();
  if (r.n < 10) return r;
  std::vector<float> sorted = d2;
  std::sort(sorted.begin(), sorted.end());
  r.first = std::max(1u, r.n / 5);
  const float nearCut = sorted[r.first - 1];
  double sum = 0.0;
  uint32_t near = 0;
  for (uint32_t i = 0; i < r.first; i++) {
    // Mid-rank among equal distances, so a tie does not bias the statistic.
    const auto lo = std::lower_bound(sorted.begin(), sorted.end(), d2[i]);
    const auto hi = std::upper_bound(sorted.begin(), sorted.end(), d2[i]);
    const double rank =
        0.5 * (double)((lo - sorted.begin()) + (hi - sorted.begin()) - 1);
    sum += rank / (double)(r.n - 1);
    if (d2[i] <= nearCut) near++;
  }
  r.meanRank = sum / (double)r.first;
  r.nearFrac = (double)near / (double)r.first;
  return r;
}
}  // namespace

// heal-restore. Claims:
//   * WIRED: both enchanted liquids heal, blood and water do not, and each
//     rides the stain slot of the liquid it is made from (the palette is full).
//   * BOUNDED: a crater carved in the side of the limb, then a SMALL pour of
//     enchanted blood (a 0.3-voxel disc, +1): cells rebuilt > 0 and never more
//     than the poured levels buy (levels x restore x scale^3, +1 for the
//     carried change); the coat is spent; once it is, 60 more ticks rebuild
//     nothing (rule 2: healing cannot feed itself); and if the pour could not
//     pay for the whole hole (the fixture asserts it cannot), the hole is NOT whole.
//   * RESTORED, SLOWLY AND NOISILY: a HEAVY wound (a crater on every side of
//     the limb that a ray reaches, at two heights) on a fresh creature, the
//     limb soaked (+8, surface) and SOAKED AGAIN every heal.repourTicks
//     until it is whole: a soak dries off in ~20 s (coat.decay 1.2, the
//     owner 2026-09-27), well short of a heavy wound's ~30 s heal, so a heavy
//     wound takes repeated pours -- which is what this measures. At 5 s (150
//     ticks) at least heal.minLeftAt5s of the wound is still there (it is not
//     an instant heal); at 35 s at most heal.maxLeftAt35s is; it ends back at
//     its recipe -- 0 missing, 0 changed -- with hp at least what it was
//     before the carve, and the coat is still on it when it does; every cell
//     rebuilt is paid for by a level spent (levels summed over every pour);
//     and the FIRST
//     20% of the cells to grow back are scattered along the wound, not the
//     ones nearest the joint (HealOrderScatter's mean rank >=
//     heal.minScatterRank). Thresholds: tests/baseline.json heal.*.
Status GateHealRestore(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  constexpr int kInset = 300;
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, kInset));
  const HealMats hm = FindHealMats(c.mats);
  if (!t.valid() || !hm.ok()) {
    detail = "no fixture limb, or a heal material missing";
    return Status::Fail;
  }
  const MaterialDef& eb = c.mats[hm.eBlood];
  const MaterialDef& ew = c.mats[hm.eWater];
  const bool wired = eb.coatRestore > 0.0f && ew.coatRestore > 0.0f &&
                     c.mats[hm.blood].coatRestore == 0.0f &&
                     c.mats[hm.water].coatRestore == 0.0f &&
                     // Their OWN stains (2026-09-27): drawn as themselves on
                     // skin and on the ground, not as blood's and wet's.
                     eb.stainSlot != 0 && eb.stainSlot != c.mats[hm.blood].stainSlot &&
                     ew.stainSlot != 0 && ew.stainSlot != c.mats[hm.water].stainSlot &&
                     mobs.StainTypeOf(hm.eBlood) != 0;
  const MobDef& def = mobs.Defs()[t.defIndex];
  const double cpl = HealCellsPerLevel(def, eb);

  const IVec3 site = FixtureSite(c.world, kInset);
  IVec3 pchunk{site.x >> 4, site.y >> 4, site.z >> 4};
  uint32_t simTick = 36000;
  support::TickCursor ticker{c, simTick, pchunk};
  auto poseTick = [&]() {
    ticker.chunk = pchunk;
    ticker();
  };
  // A round crater in the SIDE of the limb at mid-length, where a ray across
  // it first meets skin; `ro`/`rd` are that ray, for the pour.
  Vec3 ro{}, rd{};
  auto spawnWounded = [&](uint64_t& id) -> bool {
    id = SpawnTarget(c, t, kInset, pchunk);
    if (!id) return false;
    mobs.SetMobBehavior(id, "dummy");
    for (int i = 0; i < 10; i++) poseTick();
    const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
    const Vec3 mid = ax.anchor + ax.along * (0.5f * ax.reach);
    // The first of four sides from which the ray meets THIS limb first (an
    // arm hangs against the torso on one side of it).
    const Vec3 sides[4] = {ax.travel, ax.travel * -1.0f, ax.edge, ax.edge * -1.0f};
    MobSystem::BodyRayHit hit;
    for (const Vec3& s : sides) {
      ro = mid + s * 20.0f;
      rd = s * -1.0f;
      hit = mobs.PickBody(id, ro, rd, 40.0f);
      if (hit.hit && hit.limb == t.limb) break;
    }
    if (!hit.hit || hit.limb != t.limb) return false;
    std::vector<ParticleSpawn> spawns;
    mobs.CarveLimbRadial(mobs.LimbBody(id, t.limb), hit.pos, 1.0f, true, false,
                         c.world, spawns);
    for (int i = 0; i < 5; i++) poseTick();
    return mobs.LimbBody(id, t.limb) != 0;
  };

  // ---- A: a small pour, bounded ---------------------------------------------
  uint64_t idA = 0;
  if (!spawnWounded(idA)) {
    detail = t.defName + "." + t.limbName + ": fixture (spawn / ray / carve) failed";
    return Status::Fail;
  }
  uint32_t missA0 = 0, diffA0 = 0;
  mobs.LimbRecipeDiff(idA, t.limb, missA0, diffA0);
  const MobSystem::BodyRayHit hA = mobs.PickBody(idA, ro, rd, 40.0f);
  uint32_t markedA = 0;
  const uint32_t didA =
      hA.hit ? mobs.PourOnBody(idA, hA, rd, 0.3f, hm.eBlood, 1, simTick, &markedA) : 0u;
  const uint32_t levelsA = HealCoatLevels(mobs, idA, hA.limb, hm.eBlood, simTick);
  const double purseA = (double)levelsA * cpl;
  uint32_t spentAt = 0;
  for (int i = 0; i < 900; i++) {
    poseTick();
    if (mobs.LimbCoatMatCount(idA, hA.limb, hm.eBlood, 1) == 0) {
      spentAt = (uint32_t)i + 1;
      break;
    }
  }
  const Mob::HealStats sA = mobs.HealStatsOf(idA);
  for (int i = 0; i < 60; i++) poseTick();
  const Mob::HealStats sA2 = mobs.HealStatsOf(idA);
  uint32_t missA1 = 0, diffA1 = 0;
  mobs.LimbRecipeDiff(idA, t.limb, missA1, diffA1);
  const uint32_t rebuiltA = sA.grown + sA.mended;
  const bool boundedOk =
      hA.hit && hA.limb == t.limb && markedA > 0 &&
      (didA & MobSystem::kRemedyRestore) && rebuiltA > 0 &&
      (double)rebuiltA <= purseA + 1.0 && sA.levelsSpent <= levelsA &&
      spentAt != 0 && sA2.grown == sA.grown && sA2.mended == sA.mended &&
      sA2.levelsSpent == sA.levelsSpent && missA1 < missA0 &&
      purseA < (double)(missA0 + diffA0) && missA1 + diffA1 > 0;

  // ---- B: a heavy wound, soaked: it comes back, slowly and noisily ----------
  uint64_t idB = 0;
  const float hpPre = [&]() {
    // The limb's hp BEFORE the carve, off an identical fresh spawn.
    uint64_t id = SpawnTarget(c, t, kInset, pchunk);
    return id ? mobs.LimbHp(id, t.limb) : -1.0f;
  }();
  const double minLeft5 = BaselineNumber("heal.minLeftAt5s", 0.5);
  const double maxLeft35 = BaselineNumber("heal.maxLeftAt35s", 0.15);
  const double minScatter = BaselineNumber("heal.minScatterRank", 0.3);
  const double minWound = BaselineNumber("heal.minHeavyWoundCells", 600);
  const uint32_t repour =
      (uint32_t)std::max(1.0, BaselineNumber("heal.repourTicks", 150));
  bool restoredOk = false;
  uint32_t missB0 = 0, diffB0 = 0, missB1 = 0, diffB1 = 0, levelsB = 0,
           doneAt = 0, craters = 0, coatAtDone = 0, poursB = 0;
  uint32_t left5 = 0, left35 = 0;
  float hpB0 = 0.0f, hpB1 = 0.0f;
  Mob::HealStats sB;
  HealScatter scat;
  bool fixtureB = false;
  const float hpGoal = std::min(hpPre, def.limbs[t.limb].hp);
  if (spawnWounded(idB)) {
    // ...and more craters: every side a ray reaches this limb from, at two
    // heights, so the wound is a few world voxels of tissue -- a heavy one.
    const LimbAxis ax = MeasureLimb(mobs, idB, t.limb);
    const Vec3 sides[4] = {ax.travel, ax.travel * -1.0f, ax.edge, ax.edge * -1.0f};
    const float heights[2] = {0.3f, 0.7f};
    for (float hf : heights)
      for (const Vec3& sd : sides) {
        const Vec3 at = ax.anchor + ax.along * (hf * ax.reach);
        const Vec3 o = at + sd * 20.0f;
        const MobSystem::BodyRayHit h = mobs.PickBody(idB, o, sd * -1.0f, 40.0f);
        if (!h.hit || h.limb != t.limb) continue;
        std::vector<ParticleSpawn> sp;
        mobs.CarveLimbRadial(mobs.LimbBody(idB, t.limb), h.pos, 1.0f, true, false,
                             c.world, sp);
        craters++;
        if (!mobs.LimbBody(idB, t.limb)) break;
      }
    for (int i = 0; i < 5; i++) poseTick();
    fixtureB = mobs.LimbBody(idB, t.limb) != 0;
  }
  if (fixtureB) {
    mobs.LimbRecipeDiff(idB, t.limb, missB0, diffB0);
    hpB0 = mobs.LimbHp(idB, t.limb);
    mobs.LogHealOrder(idB, true);
    // One pour: the levels it ADDED join the purse (what is already on the
    // limb was paid for by an earlier pour).
    auto soakB = [&]() {
      const uint32_t before = HealCoatLevels(mobs, idB, t.limb, hm.eBlood, simTick);
      mobs.SoakLimb(idB, t.limb, hm.eBlood, 8, simTick);
      const uint32_t after = HealCoatLevels(mobs, idB, t.limb, hm.eBlood, simTick);
      levelsB += after > before ? after - before : 0u;
      poursB++;
    };
    soakB();
    left5 = left35 = missB0 + diffB0;
    // SANDVOX_HEAL_SHOTS=1: a picture of the limb at 0, 5, 10, 15, 20, 30 s
    // and when it is whole, from the first side a crater was carved from.
    const char* shotsEnv = std::getenv("SANDVOX_HEAL_SHOTS");
    const bool shots = shotsEnv && *shotsEnv && *shotsEnv != '0';
    auto shotAt = [&](const char* tag) {
      if (!shots) return;
      const LimbAxis a = MeasureLimb(mobs, idB, t.limb);
      const Vec3 mid = a.anchor + a.along * (0.5f * a.reach);
      // From the side the first crater was carved from (the ray that met
      // this limb before anything else: the outside of the arm).
      const Vec3 dir = rd * -1.0f + Vec3{0.0f, 0.25f, 0.0f};
      HealShot(c, mid, dir, std::max(10.0f, 2.5f * a.reach),
               (std::string("build/heal_") + tag + ".bmp").c_str());
    };
    shotAt("00s");
    for (int i = 0; i < 3600 && mobs.LimbBody(idB, t.limb); i++) {
      poseTick();
      const uint32_t tk = (uint32_t)i + 1;
      if (tk % repour == 0) soakB();
      if (tk == 150) shotAt("05s");
      if (tk == 300) shotAt("10s");
      if (tk == 450) shotAt("15s");
      if (tk == 600) shotAt("20s");
      if (tk == 900) shotAt("30s");
      if (tk == 150 || tk == 1050 || tk % 10 == 0) {
        mobs.LimbRecipeDiff(idB, t.limb, missB1, diffB1);
        if (tk <= 150) left5 = missB1 + diffB1;
        if (tk <= 1050) left35 = missB1 + diffB1;
      }
      if (tk % 10 != 0) continue;
      if (missB1 == 0 && diffB1 == 0 &&
          mobs.LimbHp(idB, t.limb) + 1e-3f >= hpGoal) {
        doneAt = tk;
        coatAtDone = mobs.LimbCoatMatCount(idB, t.limb, hm.eBlood, 1);
        shotAt("whole");
        break;
      }
    }
    mobs.LimbRecipeDiff(idB, t.limb, missB1, diffB1);
    hpB1 = mobs.LimbHp(idB, t.limb);
    sB = mobs.HealStatsOf(idB);
    scat = HealOrderScatter(sB.orderD2);
    const double total0 = (double)(missB0 + diffB0);
    restoredOk = total0 >= minWound && missB1 == 0 && diffB1 == 0 &&
                 hpB1 + 1e-3f >= hpGoal && doneAt != 0 && coatAtDone > 0 &&
                 (double)left5 >= minLeft5 * total0 &&
                 (double)left35 <= maxLeft35 * total0 &&
                 sB.levelsSpent <= levelsB &&
                 (double)(sB.grown + sB.mended) <= (double)sB.levelsSpent * cpl + 1.0 &&
                 scat.n >= 10 && scat.meanRank >= minScatter;
  }
  mobs.Reset();
  c.debris.Reset();

  detail = Format(
      "%s.%s: wired %d, %.2f cells/level | A pour: %u marked, %u levels (purse "
      "%.0f cells) on a hole of %u missing + %u changed -> rebuilt %u (%u grown, "
      "%u mended, %u levels spent), coat spent after %u ticks, +60 ticks: %u "
      "more; left %u missing | B heavy (%u craters), %u soaks every %u ticks: "
      "%u levels on %u "
      "missing + %u changed; left %u at 5 s (>= %.2f), %u at 35 s (<= %.2f) -> "
      "%u missing + %u changed, whole+hp at tick %u (%.1f s) with %u voxels "
      "still coated, %u grown, %u mended, %u levels spent (%.0f cells), hp "
      "%.1f -> %.1f (pre-carve %.1f) | scatter: first %u of %u grown, mean "
      "joint-distance rank %.2f (>= %.2f; joint-first ~0.1), %.0f%% among the "
      "nearest 20%% | cost: %u steps, %.0f us mean, %.0f us max, %u collider "
      "rebuilds",
      t.defName.c_str(), t.limbName.c_str(), wired ? 1 : 0, cpl, markedA,
      levelsA, purseA, missA0, diffA0, rebuiltA, sA.grown, sA.mended,
      sA.levelsSpent, spentAt, (sA2.grown + sA2.mended) - rebuiltA, missA1,
      craters, poursB, repour, levelsB, missB0, diffB0, left5, minLeft5, left35, maxLeft35,
      missB1, diffB1, doneAt, doneAt / 30.0, coatAtDone, sB.grown, sB.mended,
      sB.levelsSpent, sB.levelsSpent * cpl, hpB0, hpB1, hpPre, scat.first,
      scat.n, scat.meanRank, minScatter, 100.0 * scat.nearFrac, sB.timedSteps,
      sB.timedSteps ? sB.stepUs / sB.timedSteps : 0.0, sB.stepUsMax,
      sB.bodyRebuilds);
  return wired && boundedOk && restoredOk ? Status::Pass : Status::Fail;
}

// heal-wound. Claims, on a limb CUT (a blade kerf: it bleeds, the soak
// rewrites the flesh round it) and then BURNT (surface skin rewritten to
// flesh_cooked, the way chlorine or a fire leaves it -- the burn cap drops):
//   * DOUSED in enchanted blood (the health panel's Apply): the remedy bits
//     say it stanches and restores; the wound closes; every cooked and soaked
//     cell returns to the recipe; the burnt fraction falls and the cap rises;
//     the limb's hp rises -- and on NO tick is it above hp x the burn cap.
//     PACED: the ~1600 cooked cells are not back at 5 s (heal.minLeftAt5s of
//     them remain), are mostly back at 35 s (heal.maxLeftAt35s), and the
//     coat is still on the limb when it is whole. Doused AGAIN every
//     heal.repourTicks until then: one douse dries off in ~20 s (coat.decay
//     1.2), so a wound this size takes repeated applications.
//   * ENCHANTED WATER, the gentle one, on a fresh cooked limb: it mends
//     (> 0 cells), does not claim to stanch, and never rebuilds more than its
//     levels buy.
Status GateHealWound(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  constexpr int kInset = 300;
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, kInset));
  const HealMats hm = FindHealMats(c.mats);
  if (!t.valid() || !hm.ok()) {
    detail = "no fixture limb, or a heal material missing";
    return Status::Fail;
  }
  const MobDef& def = mobs.Defs()[t.defIndex];
  const float hpMax = def.limbs[t.limb].hp;
  const IVec3 site = FixtureSite(c.world, kInset);
  IVec3 pchunk{site.x >> 4, site.y >> 4, site.z >> 4};
  uint32_t simTick = 37000;
  support::TickCursor ticker{c, simTick, pchunk};
  auto poseTick = [&]() {
    ticker.chunk = pchunk;
    ticker();
  };

  // ---- enchanted blood on a cut, burnt limb ----------------------------------
  const uint64_t id = SpawnTarget(c, t, kInset, pchunk);
  if (!id) {
    detail = "spawn refused";
    return Status::Fail;
  }
  mobs.SetMobBehavior(id, "dummy");
  for (int i = 0; i < 10; i++) poseTick();
  const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
  std::vector<ParticleSpawn> spawns;
  CutOnce(mobs, c.world, id, t.limb, ax, 0.5f * ax.reach, 1.0f, 1.0f, 0x4EA1u,
          spawns);
  for (int i = 0; i < 3; i++) poseTick();
  const bool open0 = mobs.LimbWoundOpen(id, t.limb);
  const uint32_t cooked =
      mobs.RewriteLimbSurface(id, t.limb, hm.skin, hm.cooked, 2000, simTick);
  poseTick();
  const float cap0 = mobs.BurnHealthCap(id), frac0 = mobs.BurnFraction(id);
  const float hp0 = mobs.LimbHp(id, t.limb);
  uint32_t miss0 = 0, diff0 = 0;
  mobs.LimbRecipeDiff(id, t.limb, miss0, diff0);
  const uint32_t did = mobs.DouseLimb(id, t.limb, hm.eBlood, 6, simTick);
  const uint32_t repour =
      (uint32_t)std::max(1.0, BaselineNumber("heal.repourTicks", 150));
  uint32_t douses = 1;
  const double minLeft5 = BaselineNumber("heal.minLeftAt5s", 0.5);
  const double maxLeft35 = BaselineNumber("heal.maxLeftAt35s", 0.15);
  uint32_t overCap = 0, wholeAt = 0, left5 = miss0 + diff0, left35 = miss0 + diff0,
           coatAtWhole = 0;
  float worst = 0.0f;
  std::string trace;
  for (int i = 0; i < 3600 && mobs.LimbBody(id, t.limb); i++) {
    poseTick();
    if ((uint32_t)(i + 1) % repour == 0) {
      mobs.DouseLimb(id, t.limb, hm.eBlood, 6, simTick);
      douses++;
    }
    if (i + 1 == 150 || i + 1 == 1050) {
      uint32_t m = 0, d = 0;
      mobs.LimbRecipeDiff(id, t.limb, m, d);
      if (i + 1 == 150) left5 = m + d;
      left35 = m + d;
    }
    const float hp = mobs.LimbHp(id, t.limb);
    const float lim = hpMax * mobs.BurnHealthCap(id);
    if (hp > lim + 1e-3f) {
      overCap++;
      worst = std::max(worst, hp - lim);
    }
    if (i % 10 != 9) continue;
    uint32_t m = 0, d = 0;
    mobs.LimbRecipeDiff(id, t.limb, m, d);
    // The shape of the recovery, every 150 ticks (5 s): missing / changed /
    // coated cells / cells grown so far (attribution for a limb that will not
    // close).
    if (i % 150 == 149)
      trace += Format(" %u/%u/%u/%u", m, d,
                      mobs.LimbCoatMatCount(id, t.limb, hm.eBlood, 1),
                      mobs.HealStatsOf(id).grown);
    if (m == 0 && d == 0 && hp >= lim - 1e-3f) {
      wholeAt = (uint32_t)i + 1;
      coatAtWhole = mobs.LimbCoatMatCount(id, t.limb, hm.eBlood, 1);
      if (wholeAt < 150) left5 = 0;
      if (wholeAt < 1050) left35 = 0;
      break;
    }
  }
  const bool open1 = mobs.LimbWoundOpen(id, t.limb);
  const float cap1 = mobs.BurnHealthCap(id), frac1 = mobs.BurnFraction(id);
  const float hp1 = mobs.LimbHp(id, t.limb);
  uint32_t miss1 = 0, diff1 = 0;
  std::string why1;
  mobs.LimbRecipeDiff(id, t.limb, miss1, diff1, &why1);
  const Mob::HealStats sb = mobs.HealStatsOf(id);
  const bool bloodOk = open0 && cooked >= 100 && cap0 < 1.0f &&
                       (did & MobSystem::kRemedyStanch) &&
                       (did & MobSystem::kRemedyRestore) && !open1 &&
                       miss1 == 0 && diff1 == 0 && cap1 > cap0 && frac1 < frac0 &&
                       hp1 > hp0 && overCap == 0 && wholeAt != 0 &&
                       coatAtWhole > 0 &&
                       (double)left5 >= minLeft5 * (double)(miss0 + diff0) &&
                       (double)left35 <= maxLeft35 * (double)(miss0 + diff0);

  // ---- enchanted water, the gentle one --------------------------------------
  const MaterialDef& ew = c.mats[hm.eWater];
  const double cplW = HealCellsPerLevel(def, ew);
  bool waterOk = false;
  uint32_t cookedW = 0, didW = 0, levelsW = 0;
  Mob::HealStats sw;
  if (const uint64_t idW = SpawnTarget(c, t, kInset, pchunk)) {
    mobs.SetMobBehavior(idW, "dummy");
    for (int i = 0; i < 10; i++) poseTick();
    cookedW = mobs.RewriteLimbSurface(idW, t.limb, hm.skin, hm.cooked, 2000, simTick);
    didW = mobs.DouseLimb(idW, t.limb, hm.eWater, 2, simTick);
    levelsW = HealCoatLevels(mobs, idW, t.limb, hm.eWater, simTick);
    for (int i = 0; i < 300; i++) poseTick();
    sw = mobs.HealStatsOf(idW);
    waterOk = cookedW >= 100 && (didW & MobSystem::kRemedyRestore) &&
              !(didW & MobSystem::kRemedyStanch) && sw.mended > 0 &&
              (double)(sw.grown + sw.mended) <= (double)levelsW * cplW + 1.0;
  }
  mobs.Reset();
  c.debris.Reset();

  detail = Format(
      "%s.%s: cut (open %d), %u skin cooked -> cap %.3f frac %.3f, hp %.2f, %u "
      "missing + %u changed | enchanted blood: did 0x%x, %u douses every %u "
      "ticks, left %u at 5 s (>= "
      "%.2f), %u at 35 s (<= %.2f), whole at tick %u (%.1f s, %u voxels still "
      "coated): open %d, %u missing + %u changed, cap %.3f frac %.3f, hp %.2f "
      "(max %.1f), %u ticks over the cap (worst +%.3f), %u grown %u mended %u "
      "levels, %u steps at %.0f us mean / %.0f us max | enchanted water: %u "
      "cooked, did 0x%x, %u levels (%.2f cells each) -> %u mended %u grown in "
      "10 s",
      t.defName.c_str(), t.limbName.c_str(), open0 ? 1 : 0, cooked, cap0, frac0,
      hp0, miss0, diff0, did, douses, repour, left5, minLeft5, left35, maxLeft35, wholeAt,
      wholeAt / 30.0, coatAtWhole, open1 ? 1 : 0, miss1, diff1, cap1, frac1, hp1,
      hpMax, overCap, worst, sb.grown, sb.mended, sb.levelsSpent, sb.timedSteps,
      sb.timedSteps ? sb.stepUs / sb.timedSteps : 0.0, sb.stepUsMax, cookedW,
      didW, levelsW, cplW, sw.mended, sw.grown);
  detail += " | blood trace (missing/changed/coated/grown per 5 s):" + trace +
            " " + why1;
  return bloodOk && waterOk ? Status::Pass : Status::Fail;
}

// scar-heal: THE BODY HEALS ITSELF, SLOWLY (gore.scarHeals, 2026-10-14)
// ---------------------------------------------------------------------------
// Mob::ScarTick with no coat. The clock is sped up `scar.speedup` times (120:
// two game minutes a second) through gore.scarMinutes and gore.scarHpPerMin;
// the gate prints what the run took in UNSCALED game minutes. Claims:
//   A. A CUT, left alone: nothing is rebuilt while it still bleeds; once it
//      stops, missing tissue grows back as `scar` (never the recipe's skin or
//      flesh: their counts do not rise), the TISSUE left missing is at most
//      scar.maxLeftFrac of the hole (bone is not regrown, so a crater down to
//      the bone keeps its notch), hp rises and is never above the
//      burn cap, and the SKIN CLOSES LAST: the share of regrown cells that
//      are recipe-surface cells is lower in the first half of the regrowth
//      than in the second. Once closed it SETTLES (rule 2): no passive step
//      rebuilds anything in a further scar.settleTicks.
//   B. BURNT: surface skin rewritten to flesh_cooked mends to scar, the
//      burnt fraction falls and the burn cap rises.
//   C. OFF (gore.scarHeals false; the undead's woundHeals off is the same
//      gate in Mob::HealTick): the same cut grows nothing.
//   D. THE STAB IS PRICED (phys/kerf.h KerfStabDepth), on a synthetic bore
//      of known matter: skin goes the budget, scar twice it (capped at the
//      reach), a bone layer stops it short, and a cavity is free.
//   E. ONE CELL AT A TIME: a plain cut (no crater) closes, and sooner than
//      A's deeper crater; the mean wait of a running cell clock over both is
//      gore.scarMinutes within scar.waitTol (HealStats::scarExposure).
Status GateScarHeal(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  constexpr int kInset = 300;
  // THE HUMAN: the claim is about skin over flesh over bone, the anatomy
  // recipe's layers (the library's biggest limb is a naga's tail).
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, kInset), "human");
  uint32_t scarMat = 0, skin = 0, flesh = 0, cooked = 0;
  for (size_t i = 0; i < c.mats.size(); i++) {
    const std::string& n = c.mats[i].name;
    if (n == "scar") scarMat = (uint32_t)i;
    if (n == "skin") skin = (uint32_t)i;
    if (n == "flesh") flesh = (uint32_t)i;
    if (n == "flesh_cooked") cooked = (uint32_t)i;
  }
  if (!t.valid() || !scarMat || !skin || !flesh || !cooked) {
    detail = "no fixture limb, or scar/skin/flesh/flesh_cooked missing";
    return Status::Fail;
  }
  const MobDef& def = mobs.Defs()[t.defIndex];
  const float hpMax = def.limbs[t.limb].hp;
  const IVec3 site = FixtureSite(c.world, kInset);
  IVec3 pchunk{site.x >> 4, site.y >> 4, site.z >> 4};
  uint32_t simTick = 37000;
  support::TickCursor ticker{c, simTick, pchunk};
  auto poseTick = [&]() {
    ticker.chunk = pchunk;
    ticker();
  };
  const Tuning saved = CurrentTuning();
  const double speed = std::max(1.0, BaselineNumber("scar.speedup", 120));
  auto setTune = [&](bool on) {
    Tuning tt = saved;
    tt.gore.scarHeals = on;
    tt.gore.scarHpPerMin = (float)(saved.gore.scarHpPerMin * speed);
    tt.gore.scarMinutes = (float)(saved.gore.scarMinutes / speed);
    SetCurrentTuning(tt);
  };
  const uint32_t maxTicks = (uint32_t)BaselineNumber("scar.maxTicks", 3600);
  const double maxLeft = BaselineNumber("scar.maxLeftFrac", 0.02);
  const uint32_t settle = (uint32_t)BaselineNumber("scar.settleTicks", 300);

  // ---- A. a cut, left alone ------------------------------------------------
  struct CutRun {
    bool spawned = false, cut = false;
    uint32_t bledFor = 0, grownWhileOpen = 0;
    uint32_t miss0 = 0, missEnd = 0, closedAt = 0, tissueLeft = 0;
    uint32_t skin0 = 0, flesh0 = 0, skinEnd = 0, fleshEnd = 0, scarEnd = 0;
    float hp0 = 0, hpEnd = 0;
    uint32_t overCap = 0;
    uint32_t halfGrown = 0, halfSurface = 0;
    Mob::HealStats end, settled;
    std::string why;
  };
  auto cutRun = [&](bool on, CutRun& r, bool withCrater) {
    setTune(on);
    const uint64_t id = SpawnTarget(c, t, kInset, pchunk);
    if (!id) return;
    r.spawned = true;
    mobs.SetMobBehavior(id, "dummy");
    for (int i = 0; i < 10; i++) poseTick();
    // A round crater in the side of the limb at mid-length (heal-restore's
    // fixture): deep enough to reach under the skin, so there is a bottom to
    // fill and a skin layer to close over it. Then one blade cut across it,
    // so the wound bleeds.
    const LimbAxis ax = MeasureLimb(mobs, id, t.limb);
    const Vec3 mid = ax.anchor + ax.along * (0.5f * ax.reach);
    const Vec3 sides[4] = {ax.travel, ax.travel * -1.0f, ax.edge, ax.edge * -1.0f};
    MobSystem::BodyRayHit hit;
    for (const Vec3& sd : sides) {
      hit = mobs.PickBody(id, mid + sd * 20.0f, sd * -1.0f, 40.0f);
      if (hit.hit && hit.limb == t.limb) break;
    }
    std::vector<ParticleSpawn> spawns;
    if (withCrater && hit.hit && hit.limb == t.limb)
      mobs.CarveLimbRadial(mobs.LimbBody(id, t.limb), hit.pos,
                           (float)BaselineNumber("scar.craterRadius", 1.0), true,
                           false, c.world, spawns);
    r.cut = hit.hit && hit.limb == t.limb && mobs.LimbBody(id, t.limb) != 0 &&
            CutOnce(mobs, c.world, id, t.limb, ax, 0.5f * ax.reach, 1.0f, 1.0f,
                    0x5CA1u, spawns);
    poseTick();
    // Still bleeding: nothing may be rebuilt.
    while (mobs.LimbWoundOpen(id, t.limb) && r.bledFor < 3000) {
      poseTick();
      r.bledFor++;
    }
    r.grownWhileOpen = mobs.HealStatsOf(id).scarGrown;
    uint32_t d = 0;
    mobs.LimbRecipeDiff(id, t.limb, r.miss0, d);
    r.skin0 = mobs.LimbMaterialCount(id, t.limb, skin);
    r.flesh0 = mobs.LimbMaterialCount(id, t.limb, flesh);
    r.hp0 = mobs.LimbHp(id, t.limb);
    // Cells grown before the half-way mark (half the starting hole).
    const uint32_t halfway = r.miss0 / 2;
    r.missEnd = r.miss0;
    for (uint32_t i = 0; i < maxTicks && mobs.LimbBody(id, t.limb); i++) {
      poseTick();
      const float lim = hpMax * mobs.BurnHealthCap(id);
      if (mobs.LimbHp(id, t.limb) > lim + 1e-3f) r.overCap++;
      if (i % 30 != 29) continue;
      uint32_t m = 0;
      mobs.LimbRecipeDiff(id, t.limb, m, d);
      const Mob::HealStats s = mobs.HealStatsOf(id);
      if (r.halfGrown == 0 && r.miss0 > 0 && m <= r.miss0 - halfway) {
        r.halfGrown = s.scarGrown;
        r.halfSurface = s.scarSurface;
      }
      // The tick the hole last shrank: when the healing finished.
      if (m < r.missEnd) r.closedAt = i + 1;
      r.missEnd = m;
    }
    r.end = mobs.HealStatsOf(id);
    for (uint32_t i = 0; i < settle; i++) poseTick();
    r.settled = mobs.HealStatsOf(id);
    uint32_t m = 0;
    mobs.LimbRecipeDiff(id, t.limb, m, d, &r.why);
    r.missEnd = m;
    // What is still missing that the scar could have regrown: the diff's
    // per-material tally ("mats id:n id:n") less what is not tissue (bone
    // never regrows, by design).
    r.tissueLeft = m;
    if (const size_t at = r.why.find("mats "); at != std::string::npos) {
      size_t p = at + 5;
      while (p < r.why.size() && std::isdigit((unsigned char)r.why[p])) {
        const uint32_t mat = (uint32_t)std::strtoul(r.why.c_str() + p, nullptr, 10);
        const size_t colon = r.why.find(':', p);
        if (colon == std::string::npos) break;
        const uint32_t n = (uint32_t)std::strtoul(r.why.c_str() + colon + 1, nullptr, 10);
        const bool tissue = def.tissue.empty() ||
                            (mat < def.tissue.size() && def.tissue[mat] != 0);
        if (!tissue) r.tissueLeft -= std::min(r.tissueLeft, n);
        p = r.why.find_first_of(" ];", colon);
        if (p == std::string::npos || r.why[p] != ' ') break;
        p++;
      }
    }
    r.skinEnd = mobs.LimbMaterialCount(id, t.limb, skin);
    r.fleshEnd = mobs.LimbMaterialCount(id, t.limb, flesh);
    r.scarEnd = mobs.LimbMaterialCount(id, t.limb, scarMat);
    r.hpEnd = mobs.LimbHp(id, t.limb);
    mobs.RemoveMob(id);
  };
  CutRun a;
  cutRun(true, a, true);
  const double shareFirst =
      a.halfGrown ? (double)a.halfSurface / (double)a.halfGrown : 0.0;
  const uint32_t lateGrown = a.end.scarGrown - a.halfGrown;
  const double shareLate =
      lateGrown ? (double)(a.end.scarSurface - a.halfSurface) / (double)lateGrown
                : 0.0;
  const bool aOk = a.cut && a.miss0 >= 20 && a.grownWhileOpen == 0 &&
                   a.closedAt != 0 &&
                   (double)a.tissueLeft <= maxLeft * (double)a.miss0 &&
                   a.scarEnd > 0 && a.skinEnd <= a.skin0 &&
                   a.fleshEnd <= a.flesh0 && a.hpEnd >= a.hp0 && a.overCap == 0 &&
                   a.halfGrown > 0 && shareFirst < shareLate &&
                   a.settled.scarSteps == a.end.scarSteps;

  // ---- B. burnt ---------------------------------------------------------------
  setTune(true);
  bool bOk = false;
  uint32_t burnt = 0, cookedEnd = 0, scarB = 0;
  float cap0 = 0, cap1 = 0, frac0 = 0, frac1 = 0;
  if (const uint64_t id = SpawnTarget(c, t, kInset, pchunk)) {
    mobs.SetMobBehavior(id, "dummy");
    for (int i = 0; i < 10; i++) poseTick();
    burnt = mobs.RewriteLimbSurface(id, t.limb, skin, cooked, 600, simTick);
    poseTick();
    cap0 = mobs.BurnHealthCap(id);
    frac0 = mobs.BurnFraction(id);
    for (uint32_t i = 0; i < maxTicks; i++) poseTick();
    cap1 = mobs.BurnHealthCap(id);
    frac1 = mobs.BurnFraction(id);
    cookedEnd = mobs.LimbMaterialCount(id, t.limb, cooked);
    scarB = mobs.LimbMaterialCount(id, t.limb, scarMat);
    bOk = burnt >= 100 && cookedEnd < burnt && scarB > 0 && frac1 < frac0 &&
          cap1 >= cap0;
    mobs.RemoveMob(id);
  }

  // ---- C. off --------------------------------------------------------------
  CutRun off;
  cutRun(false, off, true);

  // ---- E. one CELL at a time, and deep takes longer ---------------------------
  // The blade cut alone, no crater: a shallow sliver. It closes, and closes
  // SOONER than A's crater, which has to fill layer by layer from the bottom
  // (each layer's clocks start only as the one beneath fills). And the mean
  // wait of a running cell clock, over both wounds (HealStats::scarExposure /
  // cells scarred), is gore.scarMinutes within scar.waitTol: the per-cell rate
  // the owner asked for, measured rather than assumed.
  CutRun e;
  cutRun(true, e, false);
  const double eMin = e.closedAt * speed / (30.0 * 60.0);
  const double aMin = a.closedAt * speed / (30.0 * 60.0);
  const double wantMin = saved.gore.scarMinutes;
  const double cells = (double)a.end.scarGrown + a.end.scarMended + e.end.scarGrown +
                       e.end.scarMended;
  const double waitMin =
      cells > 0 ? (a.end.scarExposure + e.end.scarExposure) / cells * speed / (30.0 * 60.0)
                : 0.0;
  const double tol = BaselineNumber("scar.waitTol", 0.25);
  const bool eOk = e.cut && e.miss0 > 0 && e.grownWhileOpen == 0 && e.closedAt != 0 &&
                   (double)e.tissueLeft <= maxLeft * (double)e.miss0 &&
                   e.closedAt < a.closedAt && waitMin >= (1.0 - tol) * wantMin &&
                   waitMin <= (1.0 + tol) * wantMin;
  const bool cOk = off.cut && off.end.scarGrown == 0 && off.scarEnd == 0;
  SetCurrentTuning(saved);

  // ---- D. the priced bore -------------------------------------------------
  // A one-column bore at lattice scale 1 down +x, cells 0..15; `cost(k)` is
  // the resistance of the matter in layer k (0 = no cell).
  auto bore = [&](float budget, float reach, auto cost) {
    KerfSlot s;
    s.c = Vec3{0.0f, 0.5f, 0.5f};
    s.u = Vec3{0, 1, 0};
    s.v = Vec3{0, 0, 1};
    s.w = Vec3{1, 0, 0};
    s.halfW = s.halfL = 0.5f;
    return KerfStabDepth(s, 1.0f, budget, reach, [&](auto&& probe) {
      for (int k = 0; k < 16; k++) {
        const float cst = cost(k);
        if (cst > 0.0f) probe((float)k - 0.5f, 0.0f, 0.0f, cst);
      }
    });
  };
  const float dSkin = bore(4.0f, 12.0f, [](int) { return 1.0f; });
  const float dScar = bore(4.0f, 12.0f, [](int) { return 0.5f; });
  const float dScarCap = bore(4.0f, 6.0f, [](int) { return 0.5f; });
  const float dBone = bore(4.0f, 12.0f, [](int k) { return k >= 2 ? 3.0f : 1.0f; });
  const float dHole = bore(4.0f, 12.0f, [](int k) { return k < 2 ? 0.0f : 1.0f; });
  auto near = [](float a, float b) { return std::fabs(a - b) < 0.02f; };
  const bool dOk = near(dSkin, 4.0f) && near(dScar, 8.0f) &&
                   near(dScarCap, 6.0f) && near(dBone, 2.0f + 2.0f / 3.0f) &&
                   near(dHole, 6.0f);
  mobs.Reset();
  c.debris.Reset();

  detail = Format(
      "%s.%s x%.0f | A cut: bled %u ticks (grown meanwhile %u), %u missing -> "
      "%u (%u of it tissue), closed at tick %u = %.1f game min (unscaled), scar %u, skin %u->%u "
      "flesh %u->%u, hp %.2f->%.2f (max %.1f, %u ticks over cap), surface "
      "share %.2f first half vs %.2f second, steps %u then %u after %u settle "
      "ticks | B burn: %u cooked -> %u, scar %u, frac %.3f->%.3f cap "
      "%.3f->%.3f | C off: grown %u scar %u | D bore depths skin %.2f scar "
      "%.2f scar@reach6 %.2f bone %.2f hole %.2f | E plain cut: %u missing -> "
      "%u tissue, closed %.1f game min vs crater %.1f; mean cell wait %.2f min "
      "over %.0f cells (scarMinutes %.1f), recipe walks %u",
      t.defName.c_str(), t.limbName.c_str(), speed, a.bledFor,
      a.grownWhileOpen, a.miss0, a.missEnd, a.tissueLeft, a.closedAt,
      a.closedAt * speed / (30.0 * 60.0), a.scarEnd, a.skin0, a.skinEnd,
      a.flesh0, a.fleshEnd, a.hp0, a.hpEnd, hpMax, a.overCap, shareFirst,
      shareLate, a.end.scarSteps, a.settled.scarSteps, settle, burnt, cookedEnd,
      scarB, frac0, frac1, cap0, cap1, off.end.scarGrown, off.scarEnd, dSkin,
      dScar, dScarCap, dBone, dHole, e.miss0, e.tissueLeft, eMin, aMin, waitMin, cells, wantMin,
      a.end.timedSteps + e.end.timedSteps);
  detail += Format(" | A %d B %d C %d D %d E %d | A left: ", aOk, bOk, cOk, dOk, eOk) +
            a.why;
  return aOk && bOk && cOk && dOk && eOk ? Status::Pass : Status::Fail;
}

// heal-joint. The two healing findings of docs/AUDIT_alchemy_chemistry.md:
//   * R3, HEALING UNDER A STARVED BURN POT. HealTick runs in the burn pass's
//     finish (Mob::BurnHeadFinish). Until fight64 round 3 B (d7eefc7) the
//     creature loop stopped at the first creature that found the shared pot
//     (kBurnFrontPerTick / kBurnOpsPerTick) empty, so near a big fire that
//     spent it the healing of everybody after stalled. Arm A empties the pot
//     outright (MobSystem::SetBurnPotForTest(0, 0): the fire elsewhere spent
//     ALL of it) and asserts a soaked crater still knits: cells grown > 0 and
//     fewer missing than before. The old loop grows nothing here.
//   * R4, A JOINT CELL PAID FOR ONCE. A crater carved at a limb's JOINT (the
//     first limb the rig twinned with its parent: same lattice scale) takes
//     both copies of the joint twins (the sync removes the parent's copy
//     too). Both limbs soaked and re-soaked until whole: both end at their
//     recipe, and every cell the PARENT lost -- each one a twin copy --
//     came back as a pair's unpaid second copy (HealStats::twinFree == the
//     parent's loss). Before the fix both copies were charged.
Status GateHealJoint(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  PrepareWorld(c);
  constexpr int kInset = 300;
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, kInset));
  const HealMats hm = FindHealMats(c.mats);
  if (!t.valid() || !hm.ok()) {
    detail = "no fixture limb, or a heal material missing";
    return Status::Fail;
  }
  const MobDef& def = mobs.Defs()[t.defIndex];
  const double cpl = HealCellsPerLevel(def, c.mats[hm.eBlood]);
  const IVec3 site = FixtureSite(c.world, kInset);
  IVec3 pchunk{site.x >> 4, site.y >> 4, site.z >> 4};
  uint32_t simTick = 38000;
  support::TickCursor ticker{c, simTick, pchunk};
  auto poseTick = [&]() {
    ticker.chunk = pchunk;
    ticker();
  };
  auto spawnPosed = [&]() -> uint64_t {
    const uint64_t id = SpawnTarget(c, t, kInset, pchunk);
    if (!id) return 0;
    mobs.SetMobBehavior(id, "dummy");
    for (int i = 0; i < 10; i++) poseTick();
    return id;
  };
  // A crater on `limb` at `alongFrac` of its length (0 = the joint), from the
  // first side whose ray meets this limb first.
  auto crater = [&](uint64_t id, int limb, float alongFrac, float radius) -> bool {
    const LimbAxis ax = MeasureLimb(mobs, id, limb);
    if (!ax.valid) return false;
    const Vec3 at = ax.anchor + ax.along * (alongFrac * ax.reach);
    const Vec3 sides[4] = {ax.travel, ax.travel * -1.0f, ax.edge, ax.edge * -1.0f};
    for (const Vec3& s : sides) {
      const MobSystem::BodyRayHit h = mobs.PickBody(id, at + s * 20.0f, s * -1.0f, 40.0f);
      if (!h.hit || h.limb != limb) continue;
      std::vector<ParticleSpawn> sp;
      mobs.CarveLimbRadial(mobs.LimbBody(id, limb), h.pos, radius, true, false, c.world, sp);
      return true;
    }
    // The joint itself sits inside the parent: carve at the anchor.
    std::vector<ParticleSpawn> sp;
    mobs.CarveLimbRadial(mobs.LimbBody(id, limb), at, radius, true, false, c.world, sp);
    return true;
  };

  // ---- A: R3, the pot is empty --------------------------------------------
  bool starvedOk = false;
  uint32_t missA0 = 0, diffA0 = 0, missA1 = 0, diffA1 = 0;
  Mob::HealStats sA;
  if (const uint64_t id = spawnPosed()) {
    crater(id, t.limb, 0.5f, 1.0f);
    for (int i = 0; i < 5; i++) poseTick();
    mobs.LimbRecipeDiff(id, t.limb, missA0, diffA0);
    mobs.SoakLimb(id, t.limb, hm.eBlood, 8, simTick);
    mobs.SetBurnPotForTest(0, 0);
    for (int i = 0; i < 300; i++) poseTick();
    mobs.ResetBurnPotForTest();
    mobs.LimbRecipeDiff(id, t.limb, missA1, diffA1);
    sA = mobs.HealStatsOf(id);
    starvedOk = missA0 > 0 && sA.grown > 0 && missA1 < missA0;
  }
  mobs.ResetBurnPotForTest();

  // ---- B: R4, a crater at the joint ---------------------------------------
  bool jointOk = false, fixtureB = false;
  int parent = -1;
  uint32_t mc0 = 0, dc0 = 0, mp0 = 0, dp0 = 0, mc1 = 0, dc1 = 0, mp1 = 0, dp1 = 0;
  uint32_t levels = 0, pours = 0, wholeAt = 0;
  Mob::HealStats sB;
  float jointFrac = -1.0f;
  uint64_t id = 0;
  uint32_t mpPre = 0, dpPre = 0;
  // THE JOINT LIMB: the target limb if the rig paired it with its parent
  // (same lattice scale: Mob::BuildJointTwins), else the first severable
  // base limb that has twins and a parent. Measured on a posed spawn.
  int jl = -1;
  uint32_t twins = 0;
  if (const uint64_t probe = spawnPosed()) {
    auto ok = [&](int li) {
      return li >= 0 && li < (int)def.limbs.size() && mobs.LimbParentOf(probe, li) >= 0 &&
             mobs.LimbBody(probe, li) != 0 && mobs.LimbTwinCells(probe, li) > 0;
    };
    if (ok(t.limb)) jl = t.limb;
    for (int li = 0; jl < 0 && li < (int)def.limbs.size(); li++)
      if (def.limbs[(size_t)li].severable && !def.limbs[(size_t)li].vital && ok(li)) jl = li;
    if (jl >= 0) twins = mobs.LimbTwinCells(probe, jl);
  }
  // A small notch near the joint, from the side: the first of a few depths
  // into the limb whose crater takes cells the PARENT also carries (the twin
  // sync removes the parent's copies) and leaves the limb on (a crater AT the
  // pivot can sever it). Each try is a fresh creature.
  for (float f : {0.0f, 0.05f, 0.1f, 0.15f, 0.2f, 0.3f}) {
    if (jl < 0) break;
    id = spawnPosed();
    if (!id) break;
    parent = mobs.LimbParentOf(id, jl);
    if (parent < 0) break;
    mobs.LimbRecipeDiff(id, parent, mpPre, dpPre);
    if (!crater(id, jl, f, 0.8f)) continue;
    for (int i = 0; i < 5; i++) poseTick();
    mobs.LimbRecipeDiff(id, jl, mc0, dc0);
    mobs.LimbRecipeDiff(id, parent, mp0, dp0);
    fixtureB = mc0 > 0 && mp0 > mpPre && mobs.LimbBody(id, jl) != 0;
    if (fixtureB) {
      jointFrac = f;
      break;
    }
  }
  {
    if (fixtureB) {
      auto soak = [&]() {
        for (int li : {jl, parent}) {
          const uint32_t before = HealCoatLevels(mobs, id, li, hm.eBlood, simTick);
          mobs.SoakLimb(id, li, hm.eBlood, 8, simTick);
          const uint32_t after = HealCoatLevels(mobs, id, li, hm.eBlood, simTick);
          levels += after > before ? after - before : 0u;
        }
        pours++;
      };
      soak();
      const uint32_t repour =
          (uint32_t)std::max(1.0, BaselineNumber("heal.repourTicks", 150));
      for (int i = 0; i < 3600 && mobs.LimbBody(id, jl); i++) {
        poseTick();
        const uint32_t tk = (uint32_t)i + 1;
        if (tk % repour == 0) soak();
        if (tk % 10 != 0) continue;
        mobs.LimbRecipeDiff(id, jl, mc1, dc1);
        mobs.LimbRecipeDiff(id, parent, mp1, dp1);
        if (mc1 == 0 && mp1 <= mpPre) {
          wholeAt = tk;
          break;
        }
      }
      mobs.LimbRecipeDiff(id, jl, mc1, dc1);
      mobs.LimbRecipeDiff(id, parent, mp1, dp1);
      sB = mobs.HealStatsOf(id);
      const double paidCells = (double)(sB.grown + sB.mended) - (double)sB.twinFree;
      // Every cell the PARENT lost was a twin copy (the sync took it with the
      // limb's), so every one of them is the second copy of a pair and comes
      // back unpaid: exactly mp0 free grows. (The coat also buys hp once a
      // part is whole, so levels spent are not a count of cells grown.)
      (void)paidCells;
      jointOk = wholeAt != 0 && sB.twinFree > 0 && sB.twinFree == mp0 &&
                sB.levelsSpent <= levels;
    }
  }
  mobs.Reset();
  c.debris.Reset();

  detail = Format(
      "%s.%s (parent %d), %.1f cells/level | A pot EMPTIED: %u missing + %u changed -> "
      "%u + %u after 300 ticks, %u grown %u mended %u levels | B joint crater (at %.2f of "
      "joint limb %s, %u twin cells): limb %u missing, parent %u missing (fixture %d); %u soaks, %u levels poured; whole at "
      "tick %u: limb %u missing, parent %u; %u grown (%u twin second copies, unpaid) "
      "%u mended, %u levels spent = %.0f cells vs %.0f grown + mended - twin (want twin second copies == the parent's %u lost)",
      t.defName.c_str(), t.limbName.c_str(), parent, cpl, missA0, diffA0, missA1, diffA1,
      sA.grown, sA.mended, sA.levelsSpent, jointFrac,
      jl >= 0 ? def.limbs[(size_t)jl].name.c_str() : "NONE", twins, mc0, mp0, fixtureB ? 1 : 0, pours, levels,
      wholeAt, mc1, mp1, sB.grown, sB.twinFree, sB.mended, sB.levelsSpent,
      sB.levelsSpent * cpl, (double)(sB.grown + sB.mended) - (double)sB.twinFree, mp0);
  return starvedOk && jointOk ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// liquid-coats: EVERY liquid coats a body (owner report 2026-09-29)
// ---------------------------------------------------------------------------
//
// "Syrup doesn't stain characters when they walk through it, nor does it show
// when applied manually." Syrup had no stain block, so it had no stain slot:
// the contact pass (StainOneLimb) skipped it and the micro renderer's coat
// filter (microbody.cpp DrawsCoat) drew the coat SoakLimb did write as clean.
// A liquid with no stain block now gets materials.json `liquidStainDefault`
// (materials.cpp DefaultLiquidStain). Claims:
//   * EVERY LIQUID IS WIRED: a body stain slot, a nonzero coat opacity, a
//     nonzero contact rate and a look the renderer draws -- unless it opted out
//     with `"stain": false` (named in the detail).
//   * SYRUP LOOKS LIKE SYRUP: its coat colour is its own base colour at the
//     default opacity, and it never marks the ground (no GPU stain type).
//   * APPLIED BY HAND IT LANDS: SoakLimb puts syrup on a limb.
//   * WALKED THROUGH IT LANDS: a creature standing in a syrup pool picks the
//     coat up through the real contact pass.
Status GateLiquidCoats(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  // ---- 1. the table ------------------------------------------------------
  const MicroBodySet* set = mobs.MicroSet();
  uint32_t liquids = 0, wired = 0;
  std::string unwired, optedOut;
  uint32_t mSyrup = 0;
  for (size_t i = 1; i < c.mats.size(); i++) {
    const MaterialDef& m = c.mats[i];
    if (m.name == "syrup") mSyrup = (uint32_t)i;
    if (m.gpu.klass != CLASS_LIQUID) continue;
    liquids++;
    if (m.stain.empty()) {  // `"stain": false`
      optedOut += " " + m.name;
      continue;
    }
    const uint32_t groundChance =
        (m.gpu.stainPack >> kStainPackChanceShift) & kStainPackChanceMask;
    const bool contact = m.coatContact > 0 || (m.coatContact < 0 && groundChance > 0);
    const bool draws = set && i < set->drawsCoat.size() && set->drawsCoat[i] != 0;
    const bool ok = m.stainSlot != 0 && mobs.StainTypeOf((uint32_t)i) != 0 &&
                    (m.gpu.stainColor >> 24) != 0 && contact && draws;
    if (ok) wired++;
    else
      unwired += Format(" %s(slot %u opacity %u contact %d draws %d)", m.name.c_str(),
                        m.stainSlot, m.gpu.stainColor >> 24, m.coatContact, draws ? 1 : 0);
  }
  const bool tableOk = wired + (uint32_t)std::count(optedOut.begin(), optedOut.end(), ' ') ==
                           liquids && liquids > 0;
  if (!mSyrup) {
    detail = "syrup material missing";
    return Status::Fail;
  }
  const MaterialDef& syrup = c.mats[mSyrup];
  const bool syrupLook = syrup.stainAuto &&
                         (syrup.gpu.stainPack & kStainPackTypeMask) == 0 &&
                         (syrup.gpu.stainColor & 0xFFFFFFu) == (syrup.gpu.color0 & 0xFFFFFFu) &&
                         std::abs((int)(syrup.gpu.stainColor >> 24) - 128) <= 1;

  // ---- 2. by hand, and 3. walked through --------------------------------
  PrepareWorld(c);
  constexpr int kInset = 330;
  const Target t = ChooseTarget(mobs, FixtureSite(c.world, kInset));
  if (!t.valid()) {
    detail = "no loaded mob def has a severable non-vital limb";
    return Status::Fail;
  }
  IVec3 pchunk{};
  const uint64_t id = SpawnTarget(c, t, kInset, pchunk);
  if (!id) {
    detail = "spawn refused";
    return Status::Fail;
  }
  mobs.SetMobBehavior(id, "dummy");
  const MobDef& def = mobs.Defs()[t.defIndex];
  const int root = def.rootLimb;
  uint32_t simTick = 29000;
  support::TickCursor ticker{c, simTick, pchunk};
  auto tick = [&](const std::function<void(std::vector<CellOp>&)>& fill) {
    support::TickOps pre;
    fill(pre.cells);
    if (root >= 0 && mobs.LimbBody(id, root)) {
      const Vec3 at = mobs.LimbVoxelPos(id, root, 0);
      ticker.chunk = IVec3{ifloor(at.x) >> 4, ifloor(at.y) >> 4, ifloor(at.z) >> 4};
    }
    ticker(pre);
  };
  for (int i = 0; i < 20; i++) tick([](std::vector<CellOp>&) {});

  // By hand: the target limb, which the pool below cannot reach from the feet
  // unless it IS a leg -- counted separately either way.
  const uint32_t handMarked = mobs.SoakLimb(id, t.limb, mSyrup, 5, simTick);
  const uint32_t handCoat = mobs.LimbCoatMatCount(id, t.limb, mSyrup, 1);

  // Walked through: two voxels of syrup round the feet, re-poured into air
  // (or syrup) cells every tick for 40 ticks, as body-stain's shallow pool.
  auto mirrorMat = [&](IVec3 cc) -> int {
    const CachedChunk* k = c.world.Cached(IVec3{cc.x >> 4, cc.y >> 4, cc.z >> 4});
    if (!k || k->voxels.size() != kChunkVol) return -1;
    return (int)(k->voxels[(((uint32_t)cc.z & 15u) * kChunk + ((uint32_t)cc.y & 15u)) * kChunk +
                           ((uint32_t)cc.x & 15u)] & 0xFFFu);
  };
  float footLo = 1e30f;
  for (size_t li = 0; li < def.limbs.size(); li++) {
    float lo = 0.0f, hi = 0.0f;
    if ((int)li != t.limb && mobs.LimbBody(id, (int)li) &&
        mobs.LimbStainWorldYRange(id, (int)li, 0, lo, hi))
      footLo = std::min(footLo, lo);
  }
  uint32_t poolCells = 0, walkCoat = 0;
  if (footLo < 1e29f && root >= 0 && mobs.LimbBody(id, root)) {
    const int footY = ifloor(footLo);
    const Vec3 at = mobs.LimbVoxelPos(id, root, 0);
    const int gx = ifloor(at.x), gz = ifloor(at.z);
    for (int i = 0; i < 40; i++) {
      tick([&](std::vector<CellOp>& ops) {
        for (int dz = -5; dz <= 5; dz++)
          for (int dx = -5; dx <= 5; dx++)
            for (int dy = 0; dy <= 1; dy++) {
              const IVec3 cc{gx + dx, footY + dy, gz + dz};
              if (!c.world.CellInWindow(cc) || ops.size() >= kMaxCellOpsPerTick) continue;
              const int m = mirrorMat(cc);
              if (m != 0 && m != (int)mSyrup) continue;
              ops.push_back({World::SlotCellIndex(cc), PackVoxNew(mSyrup, 8u)});
              poolCells++;
            }
      });
    }
    // Every limb but the hand-soaked one: only the pool can have coated them.
    for (size_t li = 0; li < def.limbs.size(); li++)
      if ((int)li != t.limb && mobs.LimbBody(id, (int)li))
        walkCoat += mobs.LimbCoatMatCount(id, (int)li, mSyrup, 1);
  }
  const bool handOk = handMarked > 0 && handCoat > 0;
  const bool walkOk = walkCoat > 0;
  mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);

  const bool ok = tableOk && syrupLook && handOk && walkOk;
  detail = Format(
      "%s: %u/%u liquids wired%s%s%s%s | syrup auto %d, gpu type %u, colour "
      "0x%06x vs base 0x%06x, opacity %u%s | by hand on %s.%s: marked %u, coated %u%s | "
      "pool (%u cell writes over 40 ticks): %u syrup-coated voxels on the other "
      "limbs%s",
      ok ? "PASS" : "FAIL", wired, liquids, optedOut.empty() ? "" : ", opted out:",
      optedOut.c_str(), unwired.empty() ? "" : " UNWIRED:", unwired.c_str(),
      syrup.stainAuto ? 1 : 0, syrup.gpu.stainPack & kStainPackTypeMask,
      syrup.gpu.stainColor & 0xFFFFFFu, syrup.gpu.color0 & 0xFFFFFFu,
      syrup.gpu.stainColor >> 24, syrupLook ? "" : " [WRONG LOOK]", t.defName.c_str(),
      t.limbName.c_str(), handMarked, handCoat, handOk ? "" : " [HAND POUR REFUSED]",
      poolCells, walkCoat, walkOk ? "" : " [WALKING THROUGH SYRUP LEFT NOTHING]");
  std::printf("liquid-coats: %s\n", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// pierce: a point driven through a body, priced by what the body is made of
// ---------------------------------------------------------------------------
// docs/PLAN_projectiles.md package B (phys/kerf.h PierceChannel, Mob::
// PierceLimb, game/pierce.h PierceAlongRay). Every shot goes through the one
// entry point a flight will call, on a FRESH spawn posed like every wound
// fixture, along a ray aimed from the body's own measured geometry:
//
//   HEAD   sideways through the head's centroid at three energies: a thrown
//          stone (GLANCES), a half-draw arrow (stops short of the brain), a
//          full-draw arrow (breaches into the brain and STAYS there --
//          Embedded, not out the far side).
//   ARM    sideways through the upper arm's centroid toward the torso's: at
//          full draw it EMBEDS in the arm (owner, 2026-10-13: an arrow stays
//          in what it hits; until then it went through and on into the
//          torso); at half draw it embeds shallower.
//   TORSO  front to back, a little off the midline, at full draw: EMBEDS.
//   HAND   sideways through the hanging hand: printed, not asserted -- the
//          thin matter a full draw is ALLOWED to pass through.
//   HARD   the same torso shot into a body whose skin is harder than skin
//          (the automaton's brass, the sylvan's bark): it must NOT exit, and
//          must stop shallower than the human's channel.
//
// Plus: the full-draw head shot repeated on a fresh spawn is bit-identical
// (cells, materials, depth, hp); every living-limb channel carved EXACTLY the
// cells it paid for (the carve is the channel); and after the full-draw head
// shot the real tick runs until the creature dies or `pierceBleedTicks` pass.
//
// THE NUMBERS ARE THE POINT (decision 1): every shot prints the cells it
// crossed per material, the joules it spent and the hp the wound cost. There
// is no kill rule for the gate to assert, and it asserts none -- whether a
// thin arrow through a brain kills is the brain charge's answer, printed.
Status GatePierce(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  mobs.SetNextIdCounter(1);
  PrepareWorld(c);
  const int human = mobs.FindDef("human");
  if (human < 0) {
    detail = "no `human` mob def loaded";
    return Status::Fail;
  }
  // THE PROJECTILES (baseline.json, so a recalibration costs no rebuild).
  Penetrator arrow;
  arrow.massKg = (float)BaselineNumber("pierceArrowKg", 0.03);
  arrow.tipRadius = (float)BaselineNumber("pierceArrowTip", 1.0);
  arrow.hardness = (float)BaselineNumber("pierceArrowHard", 200.0);  // steel
  arrow.sharpness = (float)BaselineNumber("pierceArrowSharp", 1.0);
  arrow.seed = 0x7A11u;
  const float fullMps = (float)BaselineNumber("pierceFullMps", 65.0);
  // HALF DRAW = half the speed (a draw fraction scales speed; package C).
  const float halfMps = fullMps * 0.5f;
  Penetrator stone;
  stone.massKg = (float)BaselineNumber("pierceStoneKg", 0.05);
  stone.speedMps = (float)BaselineNumber("pierceStoneMps", 10.0);
  stone.tipRadius = (float)BaselineNumber("pierceStoneTip", 2.0);
  stone.hardness = (float)BaselineNumber("pierceStoneHard", 90.0);
  stone.sharpness = (float)BaselineNumber("pierceStoneSharp", 0.05);
  stone.seed = 0x5707Eu;
  const int bleedTicks = (int)BaselineNumber("pierceBleedTicks", 300);

  auto limbNamed = [&](int d, const char* nm) {
    const auto& L = mobs.Defs()[d].limbs;
    for (size_t i = 0; i < L.size(); i++)
      if (L[i].name == nm) return (int)i;
    return -1;
  };
  auto centroid = [&](uint64_t id, int li) {
    Vec3 sum{};
    const uint32_t n = 512;
    for (uint32_t k = 0; k < n; k++)
      sum += mobs.LimbVoxelPos(id, li, k * 7919u);
    return sum * (1.0f / (float)n);
  };
  auto matName = [&](uint32_t m) {
    return m < c.mats.size() ? c.mats[m].name : std::string("?");
  };
  // One shot's line: per hit, limb, outcome, depth (cm), cells by material,
  // J spent, hp charged, and what stopped it.
  auto describe = [&](int d, const PierceFlight& f) {
    std::string s;
    for (const PierceHit& h : f.hits) {
      const PierceResult& r = h.r;
      const std::string ln =
          h.limb >= 0 && h.limb < (int)mobs.Defs()[d].limbs.size()
              ? mobs.Defs()[d].limbs[h.limb].name
              : std::string(r.loose ? "loose" : "?");
      std::string mats;
      float spent = 0.0f;
      for (const PierceMatCells& m : r.byMat) {
        mats += Format("%s%s %u", mats.empty() ? "" : ", ",
                       matName(m.mat).c_str(), m.cells);
        spent += m.joules;
      }
      s += Format(
          "[%s %s %.1f cm, %u cells (%s) of %u carved, %.1f of %.1f J, "
          "entry %.2f J, hp %.2f%s%s%s]",
          ln.c_str(), PierceOutcomeName(r.outcome),
          r.depth * kVoxelMeters * 100.0f, r.cells,
          mats.empty() ? "none" : mats.c_str(), r.carved, spent, r.energyIn,
          r.firstLayerJ, r.hpCharged,
          r.outcome != PierceOutcome::Exited
              ? (", stopped by " + matName(r.stopMat)).c_str()
              : Format(", out at %.1f m/s", r.exitSpeedMps).c_str(),
          r.killed ? ", KILLED" : "", r.severed ? ", SEVERED" : "");
    }
    return s.empty() ? std::string("[no body met]") : s;
  };
  auto cellsOf = [&](const PierceResult& r, const char* name) {
    uint32_t n = 0;
    for (const PierceMatCells& m : r.byMat)
      if (matName(m.mat) == name) n += m.cells;
    return n;
  };

  // ---- ONE SHOT ON A FRESH SPAWN ---------------------------------------------
  // `aim`: 0 head (sideways), 1 arm -> torso, 2 torso (front, off the midline),
  // 3 hand (sideways through the hanging hand).
  struct Shot {
    PierceFlight f;
    uint64_t id = 0;
    int def = -1, aimLimb = -1, torso = -1;
    IVec3 chunk{};
    bool ran = false;
  };
  std::vector<ParticleSpawn> spawns;
  auto shoot = [&](int d, int aim, const Penetrator& pen,
                   float offsetFine = 0.0f) -> Shot {
    Shot sh;
    sh.def = d;
    Target t;
    t.defIndex = d;
    t.limb = 0;
    sh.id = SpawnTarget(c, t, 170, sh.chunk);
    if (!sh.id) return sh;
    const int head = limbNamed(d, "head"), torso = limbNamed(d, "torso");
    const int arm = limbNamed(d, "armU.L") >= 0 ? limbNamed(d, "armU.L")
                                                : limbNamed(d, "armU.R");
    sh.torso = torso;
    if (head < 0 || torso < 0 || arm < 0 || !mobs.LimbBody(sh.id, head) ||
        !mobs.LimbBody(sh.id, torso) || !mobs.LimbBody(sh.id, arm))
      return sh;
    const Vec3 headC = centroid(sh.id, head), torsoC = centroid(sh.id, torso),
               armC = centroid(sh.id, arm);
    // Sideways = from the arm toward the torso, level; front = across that.
    Vec3 side = torsoC - armC;
    side.y = 0.0f;
    side = side.len() > 1e-3f ? side.normalized() : Vec3{1, 0, 0};
    const Vec3 front = Vec3{0, 1, 0}.cross(side).normalized();
    constexpr float kStandOff = 12.0f;  // start well outside the body
    Vec3 from{}, dir{};
    if (aim == 0) {
      sh.aimLimb = head;
      dir = side;
      // `offsetFine` moves the axis across the lattice (up and forward, in
      // fine cells): the sweep below reads how much of what the brain charge
      // costs is the channel's alignment with the cells.
      const float off = offsetFine / (kPierceRefCellsPerMetre * kVoxelMeters);
      from = headC - side * kStandOff + (Vec3{0, 1, 0} + front) * off;
    } else if (aim == 1) {
      sh.aimLimb = arm;
      dir = side;  // level, through the arm's centroid, on into the torso
      from = armC - side * kStandOff;
    } else if (aim == 3) {
      const int hand = limbNamed(d, "hand.L") >= 0 ? limbNamed(d, "hand.L")
                                                   : limbNamed(d, "hand.R");
      if (hand < 0 || !mobs.LimbBody(sh.id, hand)) return sh;
      sh.aimLimb = hand;
      dir = side;  // level, through the hand's centroid, toward the hip
      from = centroid(sh.id, hand) - side * kStandOff;
    } else {
      sh.aimLimb = torso;
      dir = front;
      // Off the midline by 6 cm, clear of the sternum and spine.
      from = torsoC - side * 0.6f - front * kStandOff;
    }
    sh.f = PierceAlongRay(c.phys, mobs, c.debris, c.world, spawns, from, dir,
                          2.0f * kStandOff, pen);
    spawns.clear();
    sh.ran = true;
    return sh;
  };
  auto penAt = [](Penetrator p, float mps) {
    p.speedMps = mps;
    return p;
  };

  bool ok = true;
  std::string why, lines;
  auto fail = [&](const std::string& s) {
    ok = false;
    why += " | " + s;
  };
  // Every living-limb hit carved exactly what it paid for.
  uint32_t carveMismatch = 0;
  auto audit = [&](const Shot& sh) {
    for (const PierceHit& h : sh.f.hits)
      if (!h.r.loose && !h.r.severed && h.r.cells != h.r.carved)
        carveMismatch++;
  };
  auto first = [](const Shot& sh) -> const PierceHit* {
    return sh.f.hits.empty() ? nullptr : &sh.f.hits.front();
  };

  // ---- HEAD ------------------------------------------------------------------
  const Shot hStone = shoot(human, 0, stone);
  const Shot hHalf = shoot(human, 0, penAt(arrow, halfMps));
  const Shot hFull = shoot(human, 0, penAt(arrow, fullMps));
  for (const Shot* s : {&hStone, &hHalf, &hFull}) audit(*s);
  lines += Format("HEAD stone %.1f J: %s | ", stone.EnergyJ(),
                  describe(human, hStone.f).c_str());
  lines += Format("HEAD half %.1f J: %s | ", penAt(arrow, halfMps).EnergyJ(),
                  describe(human, hHalf.f).c_str());
  lines += Format("HEAD full %.1f J: %s | ", penAt(arrow, fullMps).EnergyJ(),
                  describe(human, hFull.f).c_str());
  {
    const PierceHit* a = first(hStone);
    if (!hStone.ran || !a || a->limb != hStone.aimLimb)
      fail("stone: did not meet the head");
    else if (a->r.outcome != PierceOutcome::Glanced)
      fail("stone did not glance");
    const PierceHit* b = first(hHalf);
    if (!hHalf.ran || !b || b->limb != hHalf.aimLimb)
      fail("half draw: did not meet the head");
    else if (b->r.outcome != PierceOutcome::Embedded ||
             cellsOf(b->r, "brain") > 0)
      fail("half draw was not stopped short of the brain");
    const PierceHit* f = first(hFull);
    if (!hFull.ran || !f || f->limb != hFull.aimLimb)
      fail("full draw: did not meet the head");
    else if (cellsOf(f->r, "brain") <
             (uint32_t)BaselineNumber("pierceBrainMinCells", 1))
      fail("full draw did not reach the brain");
    else if (f->r.outcome != PierceOutcome::Embedded)
      fail("full draw did not stay in the head (it went out the far side)");
  }
  // ---- ...AND HOW MUCH OF THAT IS WHERE THE AXIS CROSSED THE LATTICE --------
  // The PRICE of a channel is per depth (PierceChannel), but the brain charge
  // is per CELL destroyed, and a one-cell-radius channel holds 2 to 5 cells
  // per cell of depth by alignment alone. Reported, never asserted: the same
  // full-draw shot moved across the lattice in fifths of a fine cell, brain
  // cells and hp each time (decision 1's range, not one sample of it).
  {
    uint32_t bMin = UINT32_MAX, bMax = 0;
    float hMin = 1e30f, hMax = 0.0f;
    int kills = 0;
    std::string sweep;
    for (int k = 1; k <= 4; k++) {
      const Shot sh = shoot(human, 0, penAt(arrow, fullMps), 0.2f * (float)k);
      audit(sh);
      const PierceHit* h = first(sh);
      if (!h || h->limb != sh.aimLimb) continue;
      const uint32_t b = cellsOf(h->r, "brain");
      bMin = std::min(bMin, b);
      bMax = std::max(bMax, b);
      hMin = std::min(hMin, h->r.hpCharged);
      hMax = std::max(hMax, h->r.hpCharged);
      kills += h->r.killed ? 1 : 0;
      sweep += Format("%s+%.1f: %u brain, hp %.1f %s%s", sweep.empty() ? "" : "; ",
                      0.2f * (float)k, b, h->r.hpCharged,
                      PierceOutcomeName(h->r.outcome),
                      h->r.killed ? " KILLED" : "");
    }
    if (const PierceHit* h = first(hFull)) {
      bMin = std::min(bMin, cellsOf(h->r, "brain"));
      bMax = std::max(bMax, cellsOf(h->r, "brain"));
      hMin = std::min(hMin, h->r.hpCharged);
      hMax = std::max(hMax, h->r.hpCharged);
      kills += h->r.killed ? 1 : 0;
    }
    RecordObserved("pierceHeadBrainCellsMin", (double)bMin);
    RecordObserved("pierceHeadBrainCellsMax", (double)bMax);
    RecordObserved("pierceHeadHpMax", (double)hMax);
    lines += Format("HEAD full across the lattice (fine-cell offsets): %s -> "
                    "brain %u..%u cells, hp %.1f..%.1f, %d of 5 killed outright | ",
                    sweep.c_str(), bMin, bMax, hMin, hMax, kills);
  }
  // ---- ARM, THEN THE TORSO BEHIND IT ------------------------------------------
  const Shot aHalf = shoot(human, 1, penAt(arrow, halfMps));
  const Shot aFull = shoot(human, 1, penAt(arrow, fullMps));
  audit(aHalf);
  audit(aFull);
  lines += Format("ARM half: %s | ", describe(human, aHalf.f).c_str());
  lines += Format("ARM full: %s | ", describe(human, aFull.f).c_str());
  {
    const PierceHit* a = first(aHalf);
    if (!aHalf.ran || !a || a->limb != aHalf.aimLimb)
      fail("arm half: did not meet the arm");
    else if (a->r.outcome != PierceOutcome::Embedded)
      fail("half draw did not embed in the arm");
    const PierceHit* b = first(aFull);
    if (!aFull.ran || !b || b->limb != aFull.aimLimb)
      fail("arm full: did not meet the arm");
    else if (b->r.outcome != PierceOutcome::Embedded || !aFull.f.stopped)
      fail("full draw did not stay in the arm (it went through)");
  }
  // ---- THE HAND: thin matter, printed -----------------------------------------
  {
    const Shot hd = shoot(human, 3, penAt(arrow, fullMps));
    audit(hd);
    lines += Format("HAND full: %s | ", describe(human, hd.f).c_str());
  }

  // ---- TORSO, HUMAN AND HARD ------------------------------------------------
  const Shot tHuman = shoot(human, 2, penAt(arrow, fullMps));
  audit(tHuman);
  lines += Format("TORSO full: %s | ", describe(human, tHuman.f).c_str());
  const PierceHit* th = first(tHuman);
  if (!tHuman.ran || !th || th->limb != tHuman.aimLimb)
    fail("torso: did not meet the torso");
  else if (th->r.outcome != PierceOutcome::Embedded || !tHuman.f.stopped)
    fail("full draw did not stay in the torso (it went through)");
  int hardRan = 0;
  for (const char* nm : {"tinker", "dryad"}) {
    const int d = mobs.FindDef(nm);
    if (d < 0) {
      lines += Format("HARD %s: not loaded | ", nm);
      continue;
    }
    const Shot sh = shoot(d, 2, penAt(arrow, fullMps));
    audit(sh);
    lines += Format("HARD %s torso full: %s | ", nm, describe(d, sh.f).c_str());
    // About the BODY, not the first slot met: a creature can wear something
    // on its chest (the tinker's brass `bough`) that the arrow meets first and
    // pays for. Asserted: the flight met this creature, it STOPPED in it, and
    // whatever it did to the torso is shallower than the human's channel.
    const PierceHit* h = first(sh);
    if (!sh.ran || !h || h->mobId != sh.id) {
      fail(std::string(nm) + ": did not meet the creature");
      continue;
    }
    hardRan++;
    float torsoDepth = 0.0f;
    bool through = false;
    for (const PierceHit& x : sh.f.hits)
      if (x.mobId == sh.id && x.limb == sh.aimLimb) {
        torsoDepth = x.r.depth;
        through = x.r.outcome == PierceOutcome::Exited;
      }
    if (through || !sh.f.stopped)
      fail(std::string(nm) + ": the arrow went through");
    if (th && torsoDepth >= th->r.depth)
      fail(std::string(nm) + ": not shallower than the human's channel");
  }
  if (hardRan == 0) fail("no hard-skinned def (tinker, dryad) to shoot");
  // ---- ...THE SAME SHOT AGAIN, AND THEN THE CLOCK ----------------------------
  // Replayed on a fresh spawn of the same creature at the same place: every
  // number the channel produced must come back identical.
  const Shot hAgain = shoot(human, 0, penAt(arrow, fullMps));
  {
    const PierceHit* a = first(hFull);
    const PierceHit* b = first(hAgain);
    bool same = a && b && a->r.outcome == b->r.outcome &&
                a->r.cells == b->r.cells && a->r.depth == b->r.depth &&
                a->r.hpCharged == b->r.hpCharged &&
                a->r.byMat.size() == b->r.byMat.size();
    for (size_t k = 0; same && k < a->r.byMat.size(); k++)
      same = a->r.byMat[k].mat == b->r.byMat[k].mat &&
             a->r.byMat[k].cells == b->r.byMat[k].cells;
    if (!same) fail("the full-draw head shot did not replay identically");
  }
  // How long the full-draw head shot takes to kill, by the bleed it opened.
  // Ticked on the replay's spawn (still standing, the wound fresh).
  int diedAt = -1;
  float hpEnd = 0.0f;
  if (hAgain.ran && mobs.IsAlive(hAgain.id)) {
    uint32_t tick = 66999;
    IVec3 chunk = hAgain.chunk;
    support::TickCursor ticker{c, tick, chunk};
    for (int k = 0; k < bleedTicks; k++) {
      ticker();
      if (!mobs.IsAlive(hAgain.id)) {
        diedAt = k + 1;
        break;
      }
    }
    hpEnd = mobs.IsAlive(hAgain.id)
                ? mobs.LimbHp(hAgain.id, limbNamed(human, "head"))
                : 0.0f;
  } else if (hAgain.ran) {
    diedAt = 0;  // the channel itself killed
  }
  RecordObserved("pierceHeadDiedTicks", (double)diedAt);
  if (const PierceHit* a = first(hFull)) {
    RecordObserved("pierceHeadBrainCells", (double)cellsOf(a->r, "brain"));
    RecordObserved("pierceHeadHp", (double)a->r.hpCharged);
  }
  lines += diedAt == 0
               ? std::string("the full-draw head shot KILLED outright | ")
           : diedAt > 0
               ? Format("after the full-draw head shot: died %d ticks (%.1f s) "
                        "later | ",
                        diedAt, (float)diedAt * kTickDt)
               : Format("after the full-draw head shot: ALIVE after %d ticks "
                        "(%.1f s), head hp %.1f | ",
                        bleedTicks, (float)bleedTicks * kTickDt, hpEnd);

  if (carveMismatch)
    fail(Format("%u channels carved other than what they paid for",
                carveMismatch));
  mobs.Reset();
  c.debris.Reset();
  // The clock bled onto the ground: pristine terrain for whoever is next.
  PrepareWorld(c);

  detail = Format("arrow %.0f g, tip %.1f fine cells, hardness %.0f; %s%s",
                  arrow.massKg * 1000.0f, arrow.tipRadius, arrow.hardness,
                  lines.c_str(), why.c_str());
  std::printf("pierce: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// arrow: the bow and its arrows, through the real tick
// ---------------------------------------------------------------------------
// docs/PLAN_projectiles.md package C (game/arrows.h). Every arrow below is an
// ordinary flight in the rig's TickAuthorityCtx::arrows, stepped by THE tick
// (support::RunTicks), against a fresh fixture human posed like the pierce
// gate's, aimed from its own measured geometry:
//
//   (a) EMBED: a full draw (arrowEmbedArmDraw) into the upper arm stops in
//       it with its head in (ArrowHeadIn; a half draw no longer does: it
//       stops short of the head and drops), an arrow slot exists
//       on THAT limb, and it rides the limb -- its pose in the limb's frame is
//       unchanged after the creature is killed and its corpse has fallen (the
//       limb moved, the arrow went with it), and after the arm is severed the
//       arrow is a debris body strapped to the severed piece and registered
//       as the item.
//   (b) A FULL DRAW STAYS IN: full draw at the torso EMBEDS (owner,
//       2026-10-13: an arrow buries itself in a chest; until then it went
//       through). ...and THROUGH, as a MECHANISM: the same torso shot at
//       arrowThroughSpeedX x the full-draw speed -- a speed no bow gives, so
//       this is the residual-speed hand-off itself being tested (the thing a
//       hand, a scrap or a heavier launcher will lean on), not a calibration --
//       exits (Exited) and the flight carries on past the body.
//   (c) TERRAIN: an arrow into the ground sticks there as a registered,
//       motionless ground item, and the ground pickup takes it.
//   (d) PULL-OUT: G's take (PullArrowToKit) on an embedded arrow puts it in
//       the kit and the slot is gone.
//   (e) FLAT DAMAGE ONLY WHEN IT GOT IN: every embedded/through hit charged
//       at least the arrow's `damage`; a glance charged less than it.
//   (f) A 5-ARROW VOLLEY replayed on a fresh world is bit-identical.
//   (g) THE INPUT PATH: a bow in the rig's kit hand, TB_ATTACK held past a
//       full draw and released, looses one arrow at the full draw speed and
//       spends one from the pack; a tap under minDraw looses nothing.
//   (i) THE RETICLE (ticket 2026-10-16_11-02-47, "arrows are a little offset
//       when they fire"): the same input path, the crosshair on a fresh
//       human's torso, from four sight lines -- first person on the head and
//       off it, the third-person boom, the over-shoulder boom (the render eye
//       the frame settles TickInput::aimAt down, as main.cpp does) -- at 2-5 m and
//       from four sides. The arrow leaves from the rest and must ENTER the
//       target no more than arrowSightTolVox across the reticle's line
//       (camera right), and no more than arrowSightDropVox below it
//       (gravity). And BowSightDistance's grid half on its own: a pick cell
//       on the line answers the face it enters at; one well off it is not
//       used.
//
// PRINTED, for the owner's numbers (decision 1, and "a couple of arrows to
// the head, not always one"): full-draw arrows to kill through the torso and
// through the head. The torso count is asserted against arrowTorsoKillMin..
// arrowTorsoKillMax (baseline.json), the calibration the `damage` row aims at.
Status GateArrow(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  const int human = mobs.FindDef("human");
  const ItemDef* arrowDef = c.items.Named("arrow");
  const ItemDef* bowDef = c.items.Named("bow");
  if (human < 0 || arrowDef == nullptr || !arrowDef->IsAmmo() ||
      bowDef == nullptr || !bowDef->IsBow()) {
    detail = "no `human` def, or no bow + arrow in the item library";
    return Status::Fail;
  }
  const int arrowIdx = c.items.Find("arrow"), bowIdx = c.items.Find("bow");
  // THE QUIVER (game/quiver.h): the only thing the bow draws from, and where
  // a pulled or picked-up arrow goes first. The first in the library that
  // takes these arrows.
  int quiverIdx = -1;
  for (int i = 0; i < (int)c.items.items.size() && quiverIdx < 0; i++)
    if (c.items.items[(size_t)i].IsQuiver() && c.items.items[(size_t)i].quiver.ammo == "arrow")
      quiverIdx = i;
  if (quiverIdx < 0) {
    detail = "no quiver for `arrow` in the item library";
    return Status::Fail;
  }
  // A kit wearing an (empty) quiver on its back.
  auto wearQuiver = [&](Kit& k) {
    k.equip.slots[(int)EquipSlotId::Back] = StackOf(c.items, quiverIdx);
  };
  const float fullVox = bowDef->bow.drawSpeed;
  const int torsoKillMin = (int)BaselineNumber("arrowTorsoKillMin", 3);
  const int torsoKillMax = (int)BaselineNumber("arrowTorsoKillMax", 5);
  const float rideTol = (float)BaselineNumber("arrowRideTolVox", 0.05);
  const float fallMin = (float)BaselineNumber("arrowFallMinVox", 1.0);
  const int volleyTicks = (int)BaselineNumber("arrowVolleyTicks", 40);
  const float throughX = (float)BaselineNumber("arrowThroughSpeedX", 3.0);
  // The draws (a) and (d) shoot at, as fractions of the full draw's speed:
  // each must bury the head (ArrowHeadIn) or the arrow does not stay to be
  // ridden or pulled.
  const float armDraw = (float)BaselineNumber("arrowEmbedArmDraw", 1.0);
  const float pullDraw = (float)BaselineNumber("arrowPullDraw", 1.0);

  // THE GROUND REGISTRY the game binds (main.cpp): shed gear and stuck arrows
  // land in it, a released body leaves it.
  WorldItems ground;
  c.debris.SetOnBodyGone([&ground](uint64_t h) { ground.OnBodyGone(h); });
  mobs.SetOnItemShed(
      [&ground](uint64_t h, const ItemInstance& it) { ground.Add(h, it); });
  struct Unhook {
    MobSystem& m;
    DebrisSystem& d;
    ~Unhook() {
      m.SetOnItemShed(nullptr);
      d.SetOnBodyGone(nullptr);
    }
  } unhook{mobs, c.debris};

  bool ok = true;
  std::string why, lines;
  auto fail = [&](const std::string& s) {
    ok = false;
    why += " | " + s;
  };
  auto limbNamed = [&](const char* nm) {
    const auto& L = mobs.Defs()[human].limbs;
    for (size_t i = 0; i < L.size(); i++)
      if (L[i].name == nm) return (int)i;
    return -1;
  };
  auto centroid = [&](uint64_t id, int li) {
    Vec3 sum{};
    for (uint32_t k = 0; k < 512; k++)
      sum += mobs.LimbVoxelPos(id, li, k * 7919u);
    return sum * (1.0f / 512.0f);
  };
  const int head = limbNamed("head"), torso = limbNamed("torso");
  const int arm = limbNamed("armU.L") >= 0 ? limbNamed("armU.L")
                                           : limbNamed("armU.R");
  const int leg = limbNamed("legU.R");
  if (head < 0 || torso < 0 || arm < 0) {
    detail = "the human has no head / torso / upper arm";
    return Status::Fail;
  }

  // ---- ONE FIXTURE ------------------------------------------------------------
  // A fresh spawn on pristine ground, its aim frame measured off its own
  // limbs (the pierce gate's), and a rig on the real tick at a fixed clock.
  struct Fixture {
    uint64_t id = 0;
    IVec3 chunk{};
    Vec3 headC{}, torsoC{}, armC{}, legC{}, side{}, front{};
    std::unique_ptr<support::TickRig> rig;
  };
  auto fixture = [&](uint32_t clock) {
    Fixture f;
    // A FRESH PROCESS'S STATE for the same live set (mob-cap64-twice's
    // reset): every body torn down first, then the Jolt id history, creature
    // ids and the per-tick inputs the tick re-derives -- Jolt orders contact
    // constraints by body id, so ids are simulation input (physics.h
    // ResetBodyIdHistory).
    mobs.Reset();
    c.debris.Reset();
    c.phys.ResetBirthOrdinals();
    c.phys.ResetBodyIdHistory();
    mobs.SetNextIdCounter(1);
    mobs.SetWeatherRain(0u);
    mobs.SetRainSlope(0, 0);
    mobs.SetDayPhase(0u);
    PrepareWorld(c);
    Target t;
    t.defIndex = human;
    t.limb = 0;
    f.id = SpawnTarget(c, t, 170, f.chunk);
    if (!f.id) return f;
    f.headC = centroid(f.id, head);
    f.torsoC = centroid(f.id, torso);
    f.armC = centroid(f.id, arm);
    if (leg >= 0) f.legC = centroid(f.id, leg);
    Vec3 side = f.torsoC - f.armC;
    side.y = 0.0f;
    f.side = side.len() > 1e-3f ? side.normalized() : Vec3{1, 0, 0};
    f.front = Vec3{0, 1, 0}.cross(f.side).normalized();
    f.rig = std::make_unique<support::TickRig>(c, clock, f.chunk);
    f.rig->Authority().ground = &ground;
    return f;
  };
  constexpr float kStandOff = 12.0f;
  auto shoot = [&](Fixture& f, Vec3 at, Vec3 dir, float speedVox,
                   uint32_t seed) {
    return f.rig->Authority().arrows.Launch(*arrowDef, at - dir * kStandOff,
                                            dir, speedVox, Vec3{}, seed, {});
  };
  // The events of one arrow (by its seed), in order.
  auto eventsOf = [&](Fixture& f, uint32_t seed) {
    std::vector<ArrowEvent> out;
    for (const ArrowEvent& e : f.rig->Authority().arrows.Events())
      if (e.seed == seed) out.push_back(e);
    return out;
  };
  // A body's pose relative to another's: (offset in its frame, rotation).
  struct Rel {
    Vec3 p{};
    Quat q{};
    bool ok = false;
  };
  auto relOf = [&](uint64_t body, uint64_t host) {
    Rel r;
    BodyTransform a{}, h{};
    if (!c.phys.GetTransform(body, a) || !c.phys.GetTransform(host, h)) return r;
    const Quat qh{h.quat[0], h.quat[1], h.quat[2], h.quat[3]};
    const Quat qa{a.quat[0], a.quat[1], a.quat[2], a.quat[3]};
    r.p = QuatRotateInv(qh, a.pos - h.pos);
    r.q = QuatMul(QuatConj(qh), qa);
    r.ok = true;
    return r;
  };
  auto relDiff = [](const Rel& a, const Rel& b) {
    const float dq = 1.0f - std::fabs(QuatDot(a.q, b.q));
    return (a.p - b.p).len() + dq * 100.0f;
  };
  // (e)'s ledger: every hit that got IN must have charged the flat hp.
  int inHits = 0, inHitsShort = 0;
  auto auditHits = [&](const std::vector<ArrowEvent>& ev) {
    for (const ArrowEvent& e : ev)
      if (e.kind == ArrowEvent::Kind::Hit && e.r.hit && !e.r.missed &&
          e.r.outcome != PierceOutcome::Glanced && e.r.cells > 0 &&
          !e.r.loose) {
        inHits++;
        if (e.r.hpCharged + 1e-3f < arrowDef->arrow.damage) inHitsShort++;
      }
  };

  // ======== (a) EMBED, RIDE, DIE, SEVER ========================================
  {
    Fixture f = fixture(70000);
    if (!f.id) {
      detail = "the fixture human did not spawn";
      return Status::Fail;
    }
    const uint32_t seed = 0xA0001u;
    shoot(f, f.armC, f.side, armDraw * fullVox, seed);
    support::RunTicks(*f.rig, 2);
    const std::vector<ArrowEvent> ev = eventsOf(f, seed);
    auditHits(ev);
    const ArrowEvent* emb = nullptr;
    for (const ArrowEvent& e : ev)
      if (e.kind == ArrowEvent::Kind::Embedded) emb = &e;
    Mob* m = mobs.FindCreature(f.id);
    if (emb == nullptr || m == nullptr) {
      std::string seen;
      for (const ArrowEvent& e : ev)
        seen += Format(" kind %d outcome %s;", (int)e.kind,
                       PierceOutcomeName(e.r.outcome));
      fail(Format("(a) a %.2f draw into the arm did not embed:", armDraw) + seen);
    } else {
      const int slot = emb->slot;
      const bool onLimb = m->IsArrowSlot(slot) && m->WornHostOf(slot) == arm;
      if (!onLimb)
        fail(Format("(a) the arrow slot %d is not an arrow on armU (host %d)",
                    slot, m->WornHostOf(slot)));
      const uint64_t arrowBody = mobs.LimbBody(f.id, slot);
      const uint64_t armBody0 = mobs.LimbBody(f.id, arm);
      const Rel r0 = relOf(arrowBody, armBody0);
      BodyTransform armX0{};
      c.phys.GetTransform(armBody0, armX0);
      // Kill it; the corpse falls (the limb moves a lot), the arrow rides.
      m->Die();
      support::RunTicks(*f.rig, 45);
      const uint64_t armBody1 = mobs.LimbBody(f.id, arm);
      const uint64_t arrowBody1 = mobs.LimbBody(f.id, slot);
      BodyTransform armX1{};
      c.phys.GetTransform(armBody1, armX1);
      const float armMoved = (armX1.pos - armX0.pos).len();
      const Rel r1 = relOf(arrowBody1, armBody1);
      const bool stillThere = arrowBody1 != 0 && m->IsArrowSlot(slot);
      const float drift = r0.ok && r1.ok ? relDiff(r0, r1) : 1e9f;
      float depthCm = 0.0f, hpIn = 0.0f;
      for (const ArrowEvent& e : ev)
        if (e.kind == ArrowEvent::Kind::Hit &&
            e.r.outcome == PierceOutcome::Embedded) {
          depthCm = e.r.depth * kVoxelMeters * 100.0f;
          hpIn = e.r.hpCharged;
        }
      lines += Format(
          "(a) armU %.2f draw: embedded %.1f cm (hp %.1f), slot %d on armU; "
          "dead and fallen, arm moved %.2f vox, arrow drift in the arm's frame "
          "%.4f | ",
          armDraw, depthCm, hpIn, slot, armMoved, drift);
      if (!stillThere) fail("(a) the arrow is gone from the corpse");
      if (armMoved < fallMin)
        fail(Format("(a) the dead arm moved only %.2f vox (< %.2f): nothing "
                    "was shown to ride",
                    armMoved, fallMin));
      if (!(drift <= rideTol))
        fail(Format("(a) the arrow drifted %.4f in the arm's frame (> %.3f)",
                    drift, rideTol));
      // ...and comes off WITH the arm.
      mobs.Sever(f.id, arm);
      support::RunTicks(*f.rig, 2);
      const WorldItem* wi = ground.Find(arrowBody1);
      const uint64_t strapHost = c.debris.WornHostOf(arrowBody1);
      lines += Format("severed: arrow body %s, strapped to the arm piece %s | ",
                      wi && wi->name == "arrow" ? "registered" : "UNREGISTERED",
                      strapHost == armBody1 ? "yes" : "NO");
      if (!wi || wi->name != "arrow")
        fail("(a) the severed arm's arrow is not a registered ground item");
      if (strapHost != armBody1)
        fail("(a) the severed arm's arrow is not strapped to the arm piece");
    }
  }

  // ======== (b) THROUGH, (d) PULL-OUT, torso arrows to kill ====================
  int torsoKill = -1;
  {
    Fixture f = fixture(71000);
    if (!f.id) {
      detail = "the fixture human did not spawn (torso)";
      return Status::Fail;
    }
    // (d): half draw into the torso embeds; G's take pulls it into the kit.
    const uint32_t s0 = 0xB0001u;
    const Vec3 torsoAim = f.torsoC - f.side * 0.6f;   // off the midline
    shoot(f, torsoAim, f.front, pullDraw * fullVox, s0);
    support::RunTicks(*f.rig, 2);
    auditHits(eventsOf(f, s0));
    int slot = -1;
    for (const ArrowEvent& e : eventsOf(f, s0))
      if (e.kind == ArrowEvent::Kind::Embedded) slot = e.slot;
    Mob* m = mobs.FindCreature(f.id);
    if (slot < 0 || m == nullptr) {
      fail(Format("(d) a %.2f draw into the torso did not embed", pullDraw));
    } else {
      Kit kit;
      wearQuiver(kit);
      const uint64_t ab = mobs.LimbBody(f.id, slot);
      std::string got;
      const bool pulled = PullArrowToKit(mobs, ab, kit, c.items, &got);
      // Into the QUIVER on the back (KitReceiveArrow), wearing the wound's
      // blood (the pierce coat exchange, Mob::PullArrowLattice).
      const int inKit = KitAmmoCount(kit, "arrow");
      const ItemInstance& qd = kit.equip.At((int)EquipSlotId::Back);
      const bool bloody = !qd.held.empty() && !qd.held[0].Plain();
      lines += Format("(d) pulled the torso arrow: %s, quiver %d (%s), %d left in it | ",
                      pulled ? "yes" : "NO", inKit, bloody ? "coated" : "clean",
                      m->EmbeddedArrowCount());
      if (!pulled || inKit != 1 || m->EmbeddedArrowCount() != 0)
        fail("(d) the pull-out did not move the arrow into the quiver");
    }
    // (b) + the count: full draw into the torso until it dies (bounded).
    // The aim is re-measured before every shot: the creature is alive and
    // the real tick runs between them.
    std::string seq;
    for (int k = 0; k < 10 && mobs.IsAlive(f.id); k++) {
      const uint32_t sk = 0xB0100u + (uint32_t)k;
      const Vec3 tC = centroid(f.id, torso), aC = centroid(f.id, arm);
      Vec3 sd = tC - aC;
      sd.y = 0.0f;
      sd = sd.len() > 1e-3f ? sd.normalized() : f.side;
      const Vec3 fr = Vec3{0, 1, 0}.cross(sd).normalized();
      const Vec3 off = Vec3{0, 1, 0} * (0.5f * (float)(k % 3) - 0.5f);
      shoot(f, tC - sd * 0.6f + off, fr, fullVox, sk);
      support::RunTicks(*f.rig, 3);
      const std::vector<ArrowEvent> ev = eventsOf(f, sk);
      auditHits(ev);
      const ArrowEvent* first = nullptr;
      for (const ArrowEvent& e : ev)
        if (first == nullptr && e.kind == ArrowEvent::Kind::Hit && e.mobId == f.id)
          first = &e;
      seq += first ? Format("%s%.0f", k ? "," : "", first->r.hpCharged)
                   : std::string(k ? ",miss" : "miss");
      if (k == 0) {
        const bool stayed =
            first != nullptr && first->r.outcome == PierceOutcome::Embedded;
        bool embedded = false, flying = false;
        for (const ArrowEvent& e : ev)
          embedded |= e.kind == ArrowEvent::Kind::Embedded && e.mobId == f.id;
        for (const ArrowFlight& fl : f.rig->Authority().arrows.Flights())
          flying |= fl.seed == sk;
        lines += Format("(b) full draw at the torso: %s %.1f cm%s%s | ",
                        first ? PierceOutcomeName(first->r.outcome) : "no hit",
                        first ? first->r.depth * kVoxelMeters * 100.0f : 0.0f,
                        embedded ? ", an arrow slot in it" : "",
                        flying ? ", STILL FLYING" : "");
        if (!stayed || !embedded || flying)
          fail("(b) a full-draw arrow did not stay in the torso");
      }
      if (!mobs.IsAlive(f.id)) torsoKill = k + 1;
    }
    lines += Format("torso: %d full-draw arrows killed (hp per arrow %s) | ",
                    torsoKill, seq.c_str());
    RecordObserved("arrowTorsoKill", (double)torsoKill);
    if (torsoKill < torsoKillMin || torsoKill > torsoKillMax)
      fail(Format("torso arrows to kill %d outside %d..%d", torsoKill,
                  torsoKillMin, torsoKillMax));
  }
  // (b) THROUGH, the mechanism: far faster than any draw, the same torso
  // line -- out the back, and still flying past the body.
  {
    Fixture f = fixture(71500);
    if (!f.id) {
      detail = "the fixture human did not spawn (through)";
      return Status::Fail;
    }
    const uint32_t sk = 0xB0200u;
    shoot(f, f.torsoC - f.side * 0.6f, f.front, throughX * fullVox, sk);
    support::RunTicks(*f.rig, 2);
    const std::vector<ArrowEvent> ev = eventsOf(f, sk);
    auditHits(ev);
    const ArrowEvent* first = nullptr;
    bool after = false;
    for (const ArrowEvent& e : ev) {
      if (first == nullptr && e.kind == ArrowEvent::Kind::Hit && e.mobId == f.id)
        first = &e;
      else if (first != nullptr)
        after = true;
    }
    for (const ArrowFlight& fl : f.rig->Authority().arrows.Flights())
      if (fl.seed == sk && first != nullptr) after = true;
    const bool through =
        first != nullptr && first->r.outcome == PierceOutcome::Exited;
    lines += Format("(b) at %.1fx the full draw: %s, then %s | ", throughX,
                    first ? PierceOutcomeName(first->r.outcome) : "no hit",
                    after ? "it flew on" : "NOTHING after it");
    if (!through || !after)
      fail("(b) a far-faster arrow did not go through the torso and on");
  }

  // ======== the head: printed for the owner, not asserted ======================
  {
    Fixture f = fixture(72000);
    int headKill = -1;
    std::string seq;
    for (int k = 0; f.id && k < 8 && mobs.IsAlive(f.id); k++) {
      const uint32_t sk = 0xC0100u + (uint32_t)k;
      const Vec3 hC = centroid(f.id, head), tC = centroid(f.id, torso),
                 aC = centroid(f.id, arm);
      Vec3 sd = tC - aC;
      sd.y = 0.0f;
      sd = sd.len() > 1e-3f ? sd.normalized() : f.side;
      const Vec3 off = Vec3{0, 1, 0} * (0.25f * (float)(k % 3) - 0.25f);
      shoot(f, hC + off, sd, fullVox, sk);
      support::RunTicks(*f.rig, 3);
      const std::vector<ArrowEvent> ev = eventsOf(f, sk);
      auditHits(ev);
      float hp = 0.0f;
      for (const ArrowEvent& e : ev)
        if (e.kind == ArrowEvent::Kind::Hit && e.mobId == f.id) hp += e.r.hpCharged;
      seq += Format("%s%.0f", k ? "," : "", hp);
      if (!mobs.IsAlive(f.id)) headKill = k + 1;
    }
    RecordObserved("arrowHeadKill", (double)headKill);
    lines += Format("head: %d full-draw arrows killed (hp per arrow %s) | ",
                    headKill, seq.c_str());
  }

  // ======== (h) THE HEAD IS IN (ticket 2026-10-14_22-26-58) ====================
  // "it should only really stick out if it's actually embedded in the flesh. i
  // shouldn't just see the whole arrow head on the outside." Six shots at a
  // bare fixture and six at one in a tunic -- full, 3/4 and half draw into the
  // chest, full draw through the arm into the side, full draw 50 degrees off
  // the chest's normal, full draw into the head -- and for every arrow that
  // stayed in the body, the HEAD'S DEPTH MEASURED ON THE BODY: four rays
  // parallel to the shaft, three art cells off its axis (clear of the channel
  // the point bored and of the head's own socket), from behind the nock to
  // the point, each stopping on the first cell of the limb the arrow is in;
  // the median of the four is how far the point is under that limb's
  // surface. Asserted: every arrow that stayed is in by its head's length
  // (ArrowHeadLength, less one art cell), and every stop shallower than the
  // head (the channel's own depth) did NOT stay. And THROUGH THE TUNIC the
  // full-draw chest shot reaches the torso: the garment's channel used to
  // run front panel -> back panel across the torso it wraps (kerf.h
  // exitGapBins) and the arrow flew on through the man untouched.
  {
    const float headLen = ArrowHeadLength(*arrowDef);
    const float cell = 1.0f / (float)std::max(1u, arrowDef->scale);
    const ItemDef* tunic = c.items.Named("tunic");
    auto buriedOf = [&](uint64_t id, int slot, int host) {
      BodyTransform x{};
      if (!c.phys.GetTransform(mobs.LimbBody(id, slot), x)) return -1.0f;
      const Quat q{x.quat[0], x.quat[1], x.quat[2], x.quat[3]};
      const Vec3 d = QuatRotate(q, Vec3{1, 0, 0});
      const Vec3 tip = x.pos + QuatRotate(q, arrowDef->arrow.tip);
      const Vec3 u = (std::fabs(d.y) < 0.9f ? d.cross(Vec3{0, 1, 0})
                                            : d.cross(Vec3{1, 0, 0}))
                         .normalized();
      const Vec3 w = d.cross(u);
      const float L = arrowDef->arrow.length + 2.0f;
      float dep[4];
      for (int k = 0; k < 4; k++) {
        const Vec3 off = (k < 2 ? u : w) * ((k & 1) ? -3.0f * cell : 3.0f * cell);
        const Vec3 ro = tip + off - d * L;
        dep[k] = 0.0f;
        float t0 = 0.0f;
        for (int it = 0; it < 512 && t0 < L; it++) {
          const MobSystem::BodyRayHit h = mobs.PickBody(id, ro + d * t0, d, L - t0);
          if (!h.hit) break;
          const float t = t0 + h.t;
          if (h.limb == host) {
            dep[k] = L - t;
            break;
          }
          t0 = t + 0.75f * cell;   // past that cell (a sleeve, the arm, itself)
        }
      }
      std::sort(dep, dep + 4);
      return 0.5f * (dep[1] + dep[2]);
    };
    int stayed = 0, shallowStayed = 0, shallowDropped = 0, notBuried = 0;
    bool tunicReachedTorso = false;
    std::string rows;
    for (int dressed = 0; dressed < 2; dressed++) {
      Fixture f = fixture(72500 + 500 * (uint32_t)dressed);
      Mob* m = f.id ? mobs.FindCreature(f.id) : nullptr;
      if (m == nullptr) {
        fail("(h) the fixture did not spawn");
        break;
      }
      if (dressed && tunic != nullptr)
        m->WearItem(tunic, EquipSlotFor(tunic->kind, Equipment{}));
      for (int k = 0; k < 6; k++) {
        const Vec3 tC = centroid(f.id, torso), aC = centroid(f.id, arm),
                   hC = centroid(f.id, head);
        Vec3 sd = tC - aC;
        sd.y = 0.0f;
        sd = sd.len() > 1e-3f ? sd.normalized() : f.side;
        const Vec3 fr = Vec3{0, 1, 0}.cross(sd).normalized();
        const Vec3 up{0, 1, 0};
        Vec3 aim = tC, dir = fr;
        float speed = fullVox;
        const char* what = "";
        switch (k) {
          case 0: aim = tC - sd * 0.6f + up * 0.5f; what = "chest full"; break;
          case 1: aim = tC + sd * 0.6f + up * 0.5f; speed = 0.75f * fullVox;
                  what = "chest 3/4"; break;
          case 2: aim = tC - sd * 0.6f - up * 1.0f; speed = 0.5f * fullVox;
                  what = "chest 1/2"; break;
          case 3: aim = aC; dir = sd; what = "arm->side full"; break;
          case 4: aim = tC + sd * 0.4f - up * 0.5f;
                  dir = (fr * 0.64f + sd * 0.77f).normalized();
                  what = "chest 50deg full"; break;
          default: aim = hC; dir = sd; what = "head full"; break;
        }
        const uint32_t sk = 0xA8000u + 0x100u * (uint32_t)dressed + (uint32_t)k;
        shoot(f, aim, dir, speed, sk);
        support::RunTicks(*f.rig, 3);
        const std::vector<ArrowEvent> ev = eventsOf(f, sk);
        auditHits(ev);
        const ArrowEvent* lastHit = nullptr;
        const ArrowEvent* end = nullptr;
        std::string chain;
        for (const ArrowEvent& e : ev) {
          if (e.kind == ArrowEvent::Kind::Hit) {
            lastHit = &e;
            if (dressed && k == 0 && e.mobId == f.id && e.limb == torso)
              tunicReachedTorso = true;
            chain += Format("%s%s %s %.1fcm", chain.empty() ? "" : ">",
                            e.limb >= 0 ? m->LimbDefAt(e.limb).name.c_str() : "?",
                            PierceOutcomeName(e.r.outcome),
                            e.r.depth * kVoxelMeters * 100.0f);
          } else {
            end = &e;
          }
        }
        const bool stopIn = lastHit && lastHit->r.outcome == PierceOutcome::Embedded;
        const bool shallow = stopIn && lastHit->r.depth < headLen;
        const bool stays = end && end->kind == ArrowEvent::Kind::Embedded;
        float buried = -1.0f;
        if (stays) {
          stayed++;
          buried = buriedOf(f.id, end->slot, m->WornHostOf(end->slot));
          if (buried < headLen - cell) notBuried++;
          if (shallow) shallowStayed++;
        } else if (shallow) {
          shallowDropped++;
        }
        rows += Format("%s%s: [%s] -> %s%s; ", dressed ? "tunic " : "", what,
                       chain.c_str(),
                       end ? (end->kind == ArrowEvent::Kind::Embedded ? "STAYED"
                              : end->kind == ArrowEvent::Kind::Dropped ? "dropped"
                              : end->kind == ArrowEvent::Kind::Stuck   ? "stuck"
                                                                        : "other")
                           : "still flying",
                       stays ? Format(", head %.1f cm under", buried * kVoxelMeters * 100.0f).c_str()
                             : "");
      }
    }
    lines += Format("(h) head %.1f cm: %d stayed (%d not in by the head), %d shallow "
                    "stops dropped, %d shallow stayed: %s| ",
                    headLen * kVoxelMeters * 100.0f, stayed, notBuried,
                    shallowDropped, shallowStayed, rows.c_str());
    RecordObserved("arrowHeadInStayed", (double)stayed);
    if (notBuried) fail(Format("(h) %d arrows stayed with the head not in", notBuried));
    if (shallowStayed) fail(Format("(h) %d stops shallower than the head stayed", shallowStayed));
    if (stayed == 0) fail("(h) no arrow stayed in any body: nothing was measured");
    if (tunic != nullptr && !tunicReachedTorso)
      fail("(h) the full draw through the tunic never met the torso it covers");
  }

  // ======== (e) the glance + (c) TERRAIN ========================================
  {
    Fixture f = fixture(73000);
    // (e) THE GLANCE, on the fresh pose: a spent arrow on the torso -- through
    // the one entry point the flights use -- charges less than the flat damage.
    {
      std::vector<ParticleSpawn> spawns;
      Penetrator slow = ArrowPenetrator(*arrowDef, 1.5f, 0xE001u);
      const PierceFlight pf =
          PierceAlongRay(c.phys, mobs, c.debris, c.world, spawns,
                         f.torsoC - f.side * 0.6f - f.front * kStandOff,
                         f.front, 2.0f * kStandOff, slow);
      const PierceHit* g = pf.hits.empty() ? nullptr : &pf.hits.front();
      lines += Format("(e) a spent arrow (1.5 m/s): %s, hp %.2f (flat %.0f) | ",
                      g ? PierceOutcomeName(g->r.outcome) : "no hit",
                      g ? g->r.hpCharged : 0.0f, arrowDef->arrow.damage);
      if (g == nullptr || g->r.outcome != PierceOutcome::Glanced ||
          g->r.hpCharged >= arrowDef->arrow.damage)
        fail("(e) a glance was charged the flat damage (or did not glance)");
    }
    // Down into the ground a body-length off to the side, steeply.
    const Vec3 dir = (f.side * -1.0f + Vec3{0, -1.2f, 0}).normalized();
    const Vec3 aim = f.torsoC - f.side * 14.0f;
    const uint32_t s0 = 0xD0001u;
    f.rig->Authority().arrows.Launch(*arrowDef, aim - dir * 6.0f, dir, fullVox,
                                     Vec3{}, s0, {});
    support::RunTicks(*f.rig, 4);
    uint64_t stuck = 0;
    for (const ArrowEvent& e : eventsOf(f, s0))
      if (e.kind == ArrowEvent::Kind::Stuck) stuck = e.body;
    BodyTransform x0{}, x1{};
    const bool have = stuck && c.phys.GetTransform(stuck, x0);
    support::RunTicks(*f.rig, 30);
    const bool still = stuck && c.phys.GetTransform(stuck, x1) &&
                       (x1.pos - x0.pos).len() < 1e-3f;
    const WorldItem* wi = stuck ? ground.Find(stuck) : nullptr;
    // THE GROUND PICKUP, as main.cpp's take does it: the registry's instance
    // into the kit, then the body goes (its release hook drops the entry).
    Kit kit;
    wearQuiver(kit);
    bool picked = false;
    if (wi != nullptr) {
      const ItemStack one = KitStackFrom(wi->One(), c.items);
      picked = KitReceiveArrow(kit, c.items, one) && c.debris.DestroyBody(stuck);
    }
    const bool gone = stuck && ground.Find(stuck) == nullptr;
    lines += Format("(c) into the ground: %s, %s, %s, picked up %s (kit %d) | ",
                    have ? "stuck" : "NOT STUCK", still ? "motionless" : "MOVED",
                    wi ? "registered" : "UNREGISTERED", picked && gone ? "yes" : "NO",
                    KitAmmoCount(kit, "arrow"));
    if (!have || !still || wi == nullptr || !picked || !gone ||
        KitAmmoCount(kit, "arrow") != 1)
      fail("(c) the ground shot did not stick as a pickable, motionless item");
  }
  lines += Format("(e) %d hits got in, %d charged under the flat %.0f | ",
                  inHits, inHitsShort, arrowDef->arrow.damage);
  if (inHits == 0 || inHitsShort != 0)
    fail("(e) a hit that got in was not charged the arrow's damage");

  // ======== (f) THE VOLLEY, TWICE ==============================================
  // Every fact the volley produced, one line each in a fixed order, the bits
  // of every float: the digest is these lines, and a mismatch NAMES the first
  // line that differs (CLAUDE.md rule 6: attribution, not a bare count).
  auto volley = [&](std::string& summary, std::vector<std::string>& facts) {
    facts.clear();
    Fixture f = fixture(74000);
    if (!f.id) return uint64_t{0};
    const Vec3 aims[5] = {f.headC, f.torsoC - f.side * 0.6f, f.armC,
                          leg >= 0 ? f.legC : f.torsoC, f.torsoC + f.side * 0.6f};
    const Vec3 dirs[5] = {f.side, f.front, f.side, f.front, f.front * -1.0f};
    const float speeds[5] = {fullVox, 0.5f * fullVox, fullVox, 0.7f * fullVox,
                             fullVox};
    for (int k = 0; k < 5; k++)
      shoot(f, aims[k], dirs[k], speeds[k], 0xF0000u + (uint32_t)k);
    support::RunTicks(*f.rig, volleyTicks);
    auto bits = [](float v) {
      uint32_t u;
      std::memcpy(&u, &v, 4);
      return u;
    };
    int embedded = 0, stuck = 0, hits = 0;
    for (const ArrowEvent& e : f.rig->Authority().arrows.Events()) {
      facts.push_back(Format(
          "event kind %d tick %u seed %x mob %llu limb %d slot %d at "
          "%08x,%08x,%08x outcome %s cells %u carved %u depth %08x hp %08x",
          (int)e.kind, e.tick, e.seed, (unsigned long long)e.mobId, e.limb,
          e.slot, bits(e.at.x), bits(e.at.y), bits(e.at.z),
          PierceOutcomeName(e.r.outcome), e.r.cells, e.r.carved,
          bits(e.r.depth), bits(e.r.hpCharged)));
      if (e.kind == ArrowEvent::Kind::Embedded) embedded++;
      if (e.kind == ArrowEvent::Kind::Stuck) stuck++;
      if (e.kind == ArrowEvent::Kind::Hit) hits++;
    }
    facts.push_back(Format("total hp %08x", bits(mobs.TotalHp(f.id))));
    // Where every arrow stuck in the body is, now, and the limb it is in.
    if (Mob* m = mobs.FindCreature(f.id))
      for (int s = m->AppendedBase(); s < m->LimbCount(); s++)
        if (m->IsArrowSlot(s)) {
          BodyTransform x{}, hx{};
          c.phys.GetTransform(mobs.LimbBody(f.id, s), x);
          c.phys.GetTransform(mobs.LimbBody(f.id, m->WornHostOf(s)), hx);
          facts.push_back(Format(
              "arrow slot %d host %d at %08x,%08x,%08x host at %08x,%08x,%08x",
              s, m->WornHostOf(s), bits(x.pos.x), bits(x.pos.y), bits(x.pos.z),
              bits(hx.pos.x), bits(hx.pos.y), bits(hx.pos.z)));
        }
    uint64_t h = 1469598103934665603ull;
    for (const std::string& s : facts)
      for (char ch : s) {
        h ^= (uint8_t)ch;
        h *= 1099511628211ull;
      }
    summary = Format("%d hits, %d embedded, %d stuck, hp %.2f", hits, embedded,
                     stuck, mobs.TotalHp(f.id));
    return h;
  };
  std::string v1, v2;
  std::vector<std::string> f1, f2;
  const uint64_t d1 = volley(v1, f1), d2 = volley(v2, f2);
  lines += Format("(f) volley of 5: %s; digest %016llx vs %016llx %s | ",
                  v1.c_str(), (unsigned long long)d1, (unsigned long long)d2,
                  d1 == d2 && d1 != 0 ? "IDENTICAL" : "DIFFER");
  if (d1 == 0 || d1 != d2) {
    size_t k = 0;
    while (k < f1.size() && k < f2.size() && f1[k] == f2[k]) k++;
    fail(Format("(f) the volley did not replay bit-identically: first "
                "difference at fact %zu of %zu/%zu: [%s] vs [%s]",
                k, f1.size(), f2.size(), k < f1.size() ? f1[k].c_str() : "-",
                k < f2.size() ? f2[k].c_str() : "-"));
  }

  // ======== (g) THE INPUT PATH: draw, hold, release ============================
  {
    Fixture f = fixture(75000);
    PlayerSession& s = f.rig->Session();
    Kit& kit = s.kit();
    kit.equip.slots[EquipSlotOfHand(Hand::Right)] = StackOf(c.items, bowIdx);
    // ARROWS IN THE PACK ARE CARRIED, NOT SHOT (game/quiver.h): with no
    // quiver on the back a full draw looses nothing...
    kit.bag.Add(StackOf(c.items, arrowIdx, 3));
    ArrowFlights& fl = f.rig->Authority().arrows;
    const uint64_t launched0 = fl.stats.launched;
    auto hold = [&](int ticks, bool down) {
      support::RunTicks(*f.rig, ticks, [&](uint32_t, support::TickOps& o) {
        o.intent.handsUp = true;
        if (down) o.input.SetHeld(TB_ATTACK, true);
      });
    };
    hold(BowFullDrawTicks(*bowDef) + 3, true);
    hold(1, false);
    const uint64_t noQuiver = fl.stats.launched - launched0;
    if (noQuiver != 0) fail("(g) a bow with no quiver on the back loosed an arrow");
    // ...and dragged onto a worn quiver (Kit::Move LOADS it), they are.
    wearQuiver(kit);
    {
      int from = -1;
      for (int i = 0; i < Bag::kSlots && from < 0; i++)
        if (kit.bag.slots[i].name == "arrow") from = i;
      const MoveResult mr = kit.Move(KitRef{KitSpace::Bag, from},
                                     KitRef{KitSpace::Equip, (int)EquipSlotId::Back}, c.items);
      if (mr != MoveResult::Ok || KitAmmoCount(kit, "arrow") != 3)
        fail(Format("(g) arrows dragged onto the quiver did not load it (%s, %d in it)",
                    MoveResultText(mr, KitRef{KitSpace::Equip, (int)EquipSlotId::Back}),
                    KitAmmoCount(kit, "arrow")));
    }
    // A tap: one tick down, then up. Under minDraw: nothing flies.
    hold(1, true);
    hold(1, false);
    const uint64_t afterTap = fl.stats.launched - launched0;
    const int ammoAfterTap = KitAmmoCount(kit, "arrow");
    // A full draw (the windup and the pull), held a little past it, then let go.
    hold(BowFullDrawTicks(*bowDef) + 3, true);
    // The HUD's draw (UIState::bowDraw, written by the tick) at full; the
    // speed a full draw looses at is BowArrowSpeed of it, the bow's
    // drawSpeed exactly (the eased fraction is 1 there).
    const float drawn = f.rig->Authority().ui.bowDraw;
    hold(1, false);
    const uint64_t afterDraw = fl.stats.launched - launched0;
    const int ammoAfterDraw = KitAmmoCount(kit, "arrow");
    lines += Format("(g) input: no quiver loosed %llu; tap loosed %llu (quiver %d); "
                    "full draw %.2f loosed %llu (quiver %d)",
                    (unsigned long long)noQuiver, (unsigned long long)afterTap,
                    ammoAfterTap, drawn, (unsigned long long)afterDraw, ammoAfterDraw);
    if (afterTap != 0 || ammoAfterTap != 3)
      fail("(g) a tap under minDraw loosed an arrow");
    if (afterDraw != 1 || ammoAfterDraw != 2 || drawn < 0.999f)
      fail("(g) a full draw and release did not loose exactly one arrow");
  }

  // ======== (i) THE RETICLE: the loose lands on what the crosshair is on ======
  {
    const float sightTol = (float)BaselineNumber("arrowSightTolVox", 0.35);
    const float dropSlack = (float)BaselineNumber("arrowSightDropVox", 0.6);
    // The sight line's origin in the CAMERA's frame (right, up, forward),
    // world voxels from the head's eye: where the frame draws the crosshair
    // from (main.cpp's `pourFrom`, the render eye).
    struct SightCase {
      const char* name;
      Vec3 off;      // (right, up, fwd)
      float range;   // eye to the torso, horizontally
      float lift;    // the eye over the torso centre
      int side;      // which side of the body it shoots from (0..3)
    };
    const SightCase cases[] = {
        {"first person, on the head", {0.0f, 0.0f, 0.0f}, 20.0f, 4.0f, 0},
        {"first person, head off the line", {0.6f, 0.3f, 2.5f}, 45.0f, 6.0f, 1},
        {"third-person boom", {0.0f, 2.5f, -32.0f}, 30.0f, 5.0f, 2},
        {"over the shoulder", {5.5f, 2.5f, -17.0f}, 50.0f, 6.0f, 3},
    };
    const float gTick = ArrowGravityPerTick2();
    // (h.0) the grid half, on its own: a sight line straight up from high over
    // the fixture (nothing in Jolt up there).
    {
      Fixture f = fixture(76000);
      if (f.id) {
        const Vec3 sight = f.torsoC + Vec3{0.25f, 120.0f, 0.25f};
        const Vec3 up{0.0f, 1.0f, 0.0f};
        const Vec3 rest = sight + Vec3{0.0f, 7.0f, 0.0f};
        WorldSnapshot snap;
        snap.valid = true;
        snap.pick[0] = 1;
        const IVec3 cell{ifloor(sight.x), ifloor(sight.y) + 40, ifloor(sight.z)};
        snap.pick[2] = (uint32_t)cell.x;
        snap.pick[3] = (uint32_t)cell.y;
        snap.pick[4] = (uint32_t)cell.z;
        const float want = (float)cell.y - sight.y;   // the cell's lower face
        const float on = BowSightDistance(snap, c.phys, {}, sight, up, rest, 300.0f);
        snap.pick[2] = (uint32_t)(cell.x + 6);
        const float offLine =
            BowSightDistance(snap, c.phys, {}, sight, up, rest, 300.0f);
        lines += Format(" | (i) grid: on the line %.2f (face at %.2f), 6 off it %.1f",
                        on, want, offLine);
        if (std::fabs(on - want) > 0.01f)
          fail(Format("(i) a pick cell on the sight line answered %.2f, not the "
                      "face it enters at %.2f", on, want));
        if (std::fabs(offLine - 300.0f) > 0.01f)
          fail(Format("(i) a pick cell 6 voxels off the sight line was used (%.2f)",
                      offLine));
      }
    }
    int k = 0;
    for (const SightCase& sc : cases) {
      Fixture f = fixture(77000 + 1000 * (uint32_t)k++);
      if (!f.id) {
        fail(Format("(i) %s: the fixture human did not spawn", sc.name));
        continue;
      }
      // PINNED (the authored `dummy` profile, mobile: false; SpawnTarget's
      // note in selftest_impact.cpp): unprofiled, the human walked ~39
      // voxels off its mark over the draw, and the crosshair was on grass.
      mobs.SetMobBehavior(f.id, "dummy");
      PlayerSession& s = f.rig->Session();
      Kit& kit = s.kit();
      kit.equip.slots[EquipSlotOfHand(Hand::Right)] = StackOf(c.items, bowIdx);
      wearQuiver(kit);
      kit.bag.Add(StackOf(c.items, arrowIdx, 2));
      {
        int from = -1;
        for (int i = 0; i < Bag::kSlots && from < 0; i++)
          if (kit.bag.slots[i].name == "arrow") from = i;
        kit.Move(KitRef{KitSpace::Bag, from},
                 KitRef{KitSpace::Equip, (int)EquipSlotId::Back}, c.items);
      }
      // Stand the shooter `range` off on the case's side, the eye `lift`
      // over the torso, and turn the camera until the SIGHT line (not the
      // head's) runs through the torso's centre.
      const Vec3 sides[4] = {f.front, f.side, (f.front + f.side).normalized(),
                             f.side * -1.0f};
      const Vec3 T = f.torsoC;
      const Vec3 eyeAt = T + sides[sc.side] * sc.range + Vec3{0.0f, sc.lift, 0.0f};
      s.player.pos = eyeAt - Vec3{0.0f, s.player.EyeOffsetNow(), 0.0f};
      s.player.vel = Vec3{};
      Vec3 fwd = (T - eyeAt).normalized();
      auto sightOf = [&](Vec3 eye, Vec3 fw) {
        const Vec3 r = fw.cross(Vec3{0, 1, 0}).normalized();
        const Vec3 u = r.cross(fw).normalized();
        return eye + r * sc.off.x + u * sc.off.y + fw * sc.off.z;
      };
      // The player TRACKS the mark through the draw (a pinned body still
      // settles a couple of voxels as it is posed): the camera is turned onto
      // the torso's centre as it stands, every tick, as a player's hand would.
      auto aimAtTorso = [&](Vec3 eye) {
        const Vec3 at = centroid(f.id, torso);
        for (int it = 0; it < 8; it++)
          fwd = (at - sightOf(eye, fwd)).normalized();
        s.cam.pitch = std::asin(std::clamp(fwd.y, -1.0f, 1.0f));
        s.cam.yaw = std::atan2(fwd.z, fwd.x);
      };
      aimAtTorso(eyeAt);
      Vec3 camFwd{}, camRight{}, camUp{};
      ArrowFlights& fl = f.rig->Authority().arrows;
      const uint64_t launched0 = fl.stats.launched;
      const size_t ev0 = fl.Events().size();
      Vec3 sight{}, aimAt{}, eyeNow{};
      // What main.cpp's frame does before each tick's Consume: the point the
      // reticle is on, down the sight line, into the command.
      auto hold = [&](int ticks, bool down) {
        support::RunTicks(*f.rig, ticks, [&](uint32_t, support::TickOps& o) {
          o.intent.handsUp = true;
          aimAtTorso(s.player.EyePos());
          camFwd = s.cam.Forward();
          camRight = s.cam.Right();
          camUp = s.cam.Up();
          const Vec3 head = s.player.EyePos(), fw = s.cam.Forward();
          sight = sightOf(head, fw);
          const Vec3 rest = BowRestPoint(head, fw, s.cam.Right(), s.cam.Up(),
                                         false, false);
          o.input.aimValid = 1;
          o.input.aimAt = sight + fw * BowSightDistance(c.world.Snap(), c.phys, {},
                                                        sight, fw, rest, kBowZeroVox);
          aimAt = o.input.aimAt;
          eyeNow = head;
          if (down) o.input.SetHeld(TB_ATTACK, true);
        });
      };
      hold(BowFullDrawTicks(*bowDef) + 3, true);
      hold(1, false);
      const Vec3 aimLoosed = aimAt, eyeLoosed = eyeNow;
      const Vec3 torsoLoosed = centroid(f.id, torso);
      const Vec3 fwdL = camFwd, rightL = camRight, upL = camUp, sightL = sight;
      hold(20, false);
      const ArrowEvent* hit = nullptr;
      const auto& evs = fl.Events();
      for (size_t i = ev0; i < evs.size() && hit == nullptr; i++)
        if (evs[i].kind == ArrowEvent::Kind::Hit && evs[i].mobId == f.id)
          hit = &evs[i];
      if (fl.stats.launched - launched0 != 1) {
        fail(Format("(i) %s: the full draw loosed %llu arrows", sc.name,
                    (unsigned long long)(fl.stats.launched - launched0)));
        continue;
      }
      if (hit == nullptr) {
        std::string seen;
        for (size_t i = ev0; i < evs.size(); i++)
          seen += Format(" [kind %d mob %llu at %.1f,%.1f,%.1f]", (int)evs[i].kind,
                         (unsigned long long)evs[i].mobId, evs[i].at.x, evs[i].at.y,
                         evs[i].at.z);
        fail(Format("(i) %s: the arrow never hit the human under the crosshair "
                    "(eye at the loose %.1f,%.1f,%.1f, wanted %.1f,%.1f,%.1f; aim "
                    "%.1f,%.1f,%.1f, %.1f down the sight; torso %.1f,%.1f,%.1f, "
                    "at the loose %.1f,%.1f,%.1f):",
                    sc.name, eyeLoosed.x, eyeLoosed.y, eyeLoosed.z, eyeAt.x, eyeAt.y,
                    eyeAt.z, aimLoosed.x, aimLoosed.y, aimLoosed.z,
                    (aimLoosed - sight).len(), T.x, T.y, T.z, torsoLoosed.x,
                    torsoLoosed.y, torsoLoosed.z) + seen);
        continue;
      }
      // Off the RETICLE's line: across it (camera right) and up/down it.
      // (the camera and sight AT THE LOOSE: the tracking above keeps turning
      // after it)
      const Vec3 d = hit->at - sightL;
      const float along = d.dot(fwdL);
      const float across = d.dot(rightL), vert = d.dot(upL);
      // Ticks in the air: the flight starts at the rest, ~7.6 voxels down
      // the look from the head's eye, which is -off.z down it from the sight.
      const float flown = std::max(0.0f, along + sc.off.z - 7.6f);
      const float ticks = flown / std::max(1e-3f, fullVox * kTickDt);
      const float dropExp = 0.5f * gTick * ticks * ticks;
      lines += Format(" | (i) %s at %.0f vox: %.2f across, %.2f up (drop %.2f)",
                      sc.name, along, across, vert, dropExp);
      if (std::fabs(across) > sightTol)
        fail(Format("(i) %s: the arrow went in %.2f voxels across the reticle's "
                    "line (tolerance %.2f)", sc.name, across, sightTol));
      if (vert > dropSlack || vert < -(dropExp + dropSlack))
        fail(Format("(i) %s: the arrow went in %.2f voxels up/down the reticle's "
                    "line (gravity's drop %.2f, slack %.2f)", sc.name, vert,
                    dropExp, dropSlack));
    }
  }

  mobs.Reset();
  c.debris.Reset();
  PrepareWorld(c);
  detail = Format("arrow %.0f g, flat %.0f hp, full draw %.0f m/s; %s%s",
                  arrowDef->arrow.massKg * 1000.0f, arrowDef->arrow.damage,
                  CellsToMetres(fullVox), lines.c_str(), why.c_str());
  std::printf("arrow: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// poison-arrow: a QUIVER, an arrow brushed with venom and copied to the rest,
// shot into a man (docs/PLAN_poison_arrows.md, game/quiver.h)
// ---------------------------------------------------------------------------
//
//   A  the quiver   arrows dragged onto a worn quiver LOAD it (Kit::Move), to
//                   its capacity and no further; the draw is a deterministic
//                   function of its key (KitPeekAmmo == what KitTakeAmmo
//                   takes) and reaches every entry by its count; a quiver of
//                   plain and coated arrows survives the item record
//                   (WriteItemInstance / ReadItemInstance) byte for byte.
//   B  the stage    one arrow's head brushed with venom through
//                   itemstage::ApplyStroke (the stage's tick path, the flask
//                   paying), then ApplyCopy onto the rest: every arrow wears
//                   venom on the same cells, the flask paid for every copy,
//                   and a second copy finds nothing left to do.
//   C  the shot     a venomed arrow out of that quiver (the draw), into a
//                   fresh human's torso on the real tick: it embeds, the
//                   channel's walls take venom and the infection SEEDS
//                   (InfectStat), and the arrow pulled out comes back into the
//                   quiver on the puller's back wearing the WOUND's fluid.
//                   Printed: venom levels delivered and cells seeded (the
//                   owner's calibration numbers; no gate threshold on them
//                   beyond "it seeded").
//   D  replay       C again on a fresh spawn: every number identical.
Status GatePoisonArrow(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  const int human = mobs.FindDef("human");
  const ItemDef* arrowDef = c.items.Named("arrow");
  const ItemDef* bowDef = c.items.Named("bow");
  const ItemDef* quiverDef = nullptr;
  for (const ItemDef& d : c.items.items)
    if (d.IsQuiver() && d.quiver.ammo == "arrow") { quiverDef = &d; break; }
  const ItemDef* flask = nullptr;
  for (const ItemDef& d : c.items.items)
    if (d.IsContainer()) { flask = &d; break; }
  if (human < 0 || !arrowDef || !arrowDef->IsAmmo() || !bowDef || !bowDef->IsBow() ||
      !quiverDef || !flask) {
    detail = "no human, bow, arrow, quiver or vessel in the libraries";
    return Status::Fail;
  }
  const std::string* liqName = BaselineValue("venomBladeLiquid");
  const uint32_t venom = mobs.MaterialIdNamed(liqName ? *liqName : "venom");
  if (!venom || venom >= c.mats.size() || c.mats[venom].coatInfects == 0) {
    detail = "the test liquid is not a coat that seeds an infection";
    return Status::Skip;
  }
  const uint32_t infect = c.mats[venom].coatInfects;
  const int arrowIdx = c.items.Find("arrow");
  const int quiverIdx = c.items.Find(quiverDef->name);
  const int flaskIdx = c.items.Find(flask->name);
  const int cap = quiverDef->quiver.capacity;
  const KitRef backRef{KitSpace::Equip, (int)EquipSlotId::Back};

  bool ok = true;
  std::string why, lines;
  auto fail = [&](const std::string& s) {
    ok = false;
    why += " | " + s;
  };
  auto venomOn = [&](const std::vector<PrefabVoxel>* lat) {
    uint32_t n = 0;
    if (lat)
      for (const PrefabVoxel& v : *lat)
        if (v.stain && BodyStainMat(v.stain) == venom) n++;
    return n;
  };

  // ======== A: THE QUIVER ======================================================
  {
    Kit kit;
    kit.equip.slots[(int)EquipSlotId::Back] = StackOf(c.items, quiverIdx);
    kit.bag.slots[0] = StackOf(c.items, arrowIdx, cap + 5);
    const MoveResult mr = kit.Move(KitRef{KitSpace::Bag, 0}, backRef, c.items);
    const int in = KitAmmoCount(kit, "arrow");
    const int left = kit.bag.slots[0].Empty() ? 0 : kit.bag.slots[0].count;
    const MoveResult full = kit.Move(KitRef{KitSpace::Bag, 0}, backRef, c.items);
    lines += Format("A: drag %d onto a %d-quiver: %d in, %d left, again: %s | ", cap + 5,
                    cap, in, left, MoveResultText(full, backRef));
    if (mr != MoveResult::Ok || in != cap || left != 5 || full != MoveResult::QuiverFull)
      fail("A: dragging arrows onto the quiver did not load it to its capacity");
    // Two coated arrows among the plain ones: entries [plain x cap-2, a, b].
    ItemStack& q = kit.equip.slots[(int)EquipSlotId::Back];
    for (int k = 0; k < 2; k++) {
      ItemInstance one = QuiverTakeOne(q, 0);
      std::vector<PrefabVoxel>& lat = ItemLatticeMut(one, *arrowDef, 0);
      lat[(size_t)k].stain = PackBodyStain(venom, 9u);
      q.held.push_back(std::move(one));
    }
    // THE DRAW: a pure function of the key, and every entry reachable.
    int hits[3] = {0, 0, 0};
    bool same = true;
    for (uint32_t key = 0; key < 600; key++) {
      const int e = QuiverPick(q, rng::Hash3(0xA770B2u, key, 0u), "arrow");
      if (e >= 0 && e < 3) hits[e]++;
      Kit k2 = kit;
      ItemInstance took;
      const ItemInstance peek = KitPeekAmmo(k2, "arrow", rng::Hash3(0xA770B2u, key, 0u));
      KitTakeAmmo(k2, "arrow", rng::Hash3(0xA770B2u, key, 0u), &took);
      std::vector<uint8_t> b1, b2;
      ByteWriter w1{b1}, w2{b2};
      WriteItemInstance(w1, peek, kItemFmtLatest);
      WriteItemInstance(w2, took, kItemFmtLatest);
      same = same && b1 == b2 && KitAmmoCount(k2, "arrow") == cap - 1;
    }
    lines += Format("A: 600 draws: plain %d, coated %d + %d, peek==take %s | ", hits[0],
                    hits[1], hits[2], same ? "yes" : "NO");
    if (!same) fail("A: the arrow KitPeekAmmo names is not the one KitTakeAmmo takes");
    if (hits[1] == 0 || hits[2] == 0 || hits[0] < hits[1] + hits[2])
      fail("A: the draw does not reach every arrow by its count");
    // THE RECORD: a quiver of plain and coated arrows, written and read back.
    std::vector<uint8_t> b1, b2;
    ByteWriter w1{b1};
    WriteItemInstance(w1, q, kItemFmtLatest);
    ItemInstance back;
    ByteReader r{b1.data(), b1.size()};
    const bool read = ReadItemInstance(r, back, kItemFmtLatest);
    ByteWriter w2{b2};
    WriteItemInstance(w2, back, kItemFmtLatest);
    lines += Format("A: record %zu bytes, %zu entries, round trip %s | ", b1.size(),
                    back.held.size(), read && b1 == b2 ? "exact" : "DIFFERS");
    if (!read || b1 != b2 || back.held.size() != 3 || QuiverCount(back) != cap)
      fail("A: the quiver's arrows did not survive the item record");
    // ...AND A SAVE (PLYR v11, through MakeEntityIO for selftest_dye's reason:
    // a field written by a section nobody registered persists nothing): the
    // worn quiver and a second one in the pack, every arrow and its coat.
    {
      PlayerCaster caster;
      GlyphLibrary glyphs;
      Kit pk;
      pk.equip.slots[(int)EquipSlotId::Back] = q;
      pk.bag.slots[4] = q;
      PlayerKitRefs refs{&caster, &glyphs, &pk, &c.items};
      EntityIO io = MakeEntityIO(c.debris, c.mobs, nullptr, &refs);
      const EntitySection* plyr = nullptr;
      for (const EntitySection& sec : io.sections)
        if (sec.id == (uint32_t)('P' | ('L' << 8) | ('Y' << 16) | ((uint32_t)'R' << 24)))
          plyr = &sec;
      bool same = false;
      if (plyr != nullptr) {
        std::vector<uint8_t> bytes;
        plyr->save(bytes);
        plyr->reset();
        const bool loaded = plyr->load(bytes.data(), bytes.size(), kPlayerKitSaveVersion);
        auto rec = [&](const ItemInstance& it) {
          std::vector<uint8_t> b;
          ByteWriter w{b};
          WriteItemInstance(w, it, kItemFmtLatest);
          return b;
        };
        same = loaded && rec(pk.equip.At((int)EquipSlotId::Back)) == b1 &&
               rec(pk.bag.At(4)) == b1;
      }
      lines += Format("A: PLYR save/load of two quivers: %s | ", same ? "exact" : "DIFFERS");
      if (!same) fail("A: a quiver did not survive a PLYR save and load arrow for arrow");
    }
  }

  // ======== B: BRUSH ONE, COPY TO N ============================================
  // The quiver the shots below draw from: 6 plain arrows, then one brushed.
  const int kArrows = 6;
  Kit stageKit;
  stageKit.equip.slots[(int)EquipSlotId::Back] = StackOf(c.items, quiverIdx);
  {
    ItemStack a = StackOf(c.items, arrowIdx, kArrows);
    QuiverLoad(stageKit.equip.slots[(int)EquipSlotId::Back], *quiverDef, a);
  }
  stageKit.bag.slots[1] = StackOf(c.items, flaskIdx);
  stageKit.bag.slots[1].contents.Add((uint16_t)venom, (uint32_t)flask->container.capacity);
  const KitRef vesselRef{KitSpace::Bag, 1};
  // The head's cells (the art's foremost run in the head's matter): rays at
  // them from the four sides across the shaft.
  IVec3 hlo{1 << 20, 1 << 20, 1 << 20}, hhi{-(1 << 20), -(1 << 20), -(1 << 20)};
  for (const PrefabVoxel& v : arrowDef->voxels)
    if ((v.material & 0xFFFu) == arrowDef->arrow.headMaterial) {
      hlo = IVec3{std::min(hlo.x, (int)v.x), std::min(hlo.y, (int)v.y), std::min(hlo.z, (int)v.z)};
      hhi = IVec3{std::max(hhi.x, (int)v.x), std::max(hhi.y, (int)v.y), std::max(hhi.z, (int)v.z)};
    }
  const float cy = (hlo.y + hhi.y + 1) * 0.5f, cz = (hlo.z + hhi.z + 1) * 0.5f;
  int64_t milli = 0;
  uint32_t marked = 0, brushSpent = 0;
  const uint32_t flask0 = stageKit.bag.slots[1].contents.Total();
  for (int x = hlo.x; x <= hhi.x; x++)
    for (int side = 0; side < 4; side++) {
      itemstage::Stroke st;
      st.active = true;
      st.item = backRef;
      st.shell = 0;
      st.held = 0;
      st.radius = 2.0f;
      st.vessel = vesselRef;
      const Vec3 dir = side == 0 ? Vec3{0, 0, -1} : side == 1 ? Vec3{0, 0, 1}
                     : side == 2 ? Vec3{0, -1, 0} : Vec3{0, 1, 0};
      st.rd = dir;
      st.ro = Vec3{x + 0.5f, cy, cz} - dir * 8.0f;
      for (int k = 0; k < 5; k++) {
        const itemstage::StrokeResult sr =
            itemstage::ApplyStroke(stageKit, nullptr, 0, c.items, c.mats, st, milli);
        marked += sr.marked;
        brushSpent += sr.spent;
      }
    }
  ItemStack& sq = stageKit.equip.slots[(int)EquipSlotId::Back];
  const uint32_t srcVenom = sq.held.empty() ? 0u : venomOn(ItemLatticeIfAny(sq.held[0], 0));
  itemstage::CopyOrder co;
  co.active = true;
  co.quiver = backRef;
  co.src = 0;
  co.n = kArrows;   // more than there are: it stops at the last arrow
  co.vessel = vesselRef;
  const itemstage::CopyResult cr = itemstage::ApplyCopy(stageKit, c.items, c.mats, co);
  const itemstage::CopyResult again = itemstage::ApplyCopy(stageKit, c.items, c.mats, co);
  int coatedArrows = 0, sameCells = 0;
  for (const ItemInstance& h : sq.held) {
    const uint32_t n = venomOn(ItemLatticeIfAny(h, 0));
    if (n > 0) coatedArrows += h.count;
    if (n == srcVenom) sameCells += h.count;
  }
  const uint32_t flaskNow = stageKit.bag.slots[1].contents.Total();
  lines += Format("B: brushed %u cells of the head (%u venom cells, %u/8 paid); copy: %d "
                  "arrows, %u/8 paid; again: %d; %d of %d arrows venomed, %d on the "
                  "same cells; flask %u -> %u | ",
                  marked, srcVenom, brushSpent, cr.done, cr.spent, again.done, coatedArrows,
                  QuiverCount(sq), sameCells, flask0, flaskNow);
  if (srcVenom == 0) fail("B: the stroke put no venom on the arrow's head");
  if (cr.refused) fail(std::string("B: the copy refused: ") + cr.refused);
  if (cr.done != kArrows - 1 || coatedArrows != kArrows || sameCells != kArrows)
    fail("B: the copy did not put the brushed arrow's venom on every other arrow");
  if (again.done != 0) fail("B: a second copy found arrows still to coat");
  if (cr.spent == 0 || flaskNow + brushSpent + cr.spent != flask0)
    fail("B: the flask did not pay exactly what the brush and the copy took");
  if (QuiverCount(sq) != kArrows) fail("B: the stage changed how many arrows the quiver holds");

  // ======== C + D: THE SHOT, TWICE ===============================================
  WorldItems ground;
  c.debris.SetOnBodyGone([&ground](uint64_t h) { ground.OnBodyGone(h); });
  mobs.SetOnItemShed([&ground](uint64_t h, const ItemInstance& it) { ground.Add(h, it); });
  struct Unhook {
    MobSystem& m;
    DebrisSystem& d;
    ~Unhook() {
      m.SetOnItemShed(nullptr);
      d.SetOnBodyGone(nullptr);
    }
  } unhook{mobs, c.debris};
  auto limbNamed = [&](const char* nm) {
    const auto& L = mobs.Defs()[human].limbs;
    for (size_t i = 0; i < L.size(); i++)
      if (L[i].name == nm) return (int)i;
    return -1;
  };
  const int torso = limbNamed("torso");
  const int arm = limbNamed("armU.L") >= 0 ? limbNamed("armU.L") : limbNamed("armU.R");
  if (torso < 0 || arm < 0) {
    detail = "the human has no torso / upper arm";
    return Status::Fail;
  }
  auto centroid = [&](uint64_t id, int li) {
    Vec3 sum{};
    for (uint32_t k = 0; k < 512; k++) sum += mobs.LimbVoxelPos(id, li, k * 7919u);
    return sum * (1.0f / 512.0f);
  };
  const int settle = (int)BaselineNumber("poisonArrowSettleTicks", 120);
  struct Shot {
    std::string err;
    bool embedded = false, pulled = false, intoQuiver = false;
    uint32_t delivered = 0, wallVenom = 0, seeded = 0, peak = 0;
    uint32_t arrowVenom = 0, arrowOther = 0;
    uint64_t digest = 0;
  };
  auto shot = [&](uint32_t clock) {
    Shot r;
    mobs.Reset();
    c.debris.Reset();
    c.phys.ResetBirthOrdinals();
    c.phys.ResetBodyIdHistory();
    mobs.SetNextIdCounter(1);
    mobs.SetWeatherRain(0u);
    mobs.SetRainSlope(0, 0);
    mobs.SetDayPhase(0u);
    PrepareWorld(c);
    Target t;
    t.defIndex = human;
    t.limb = 0;
    IVec3 chunk{};
    const uint64_t id = SpawnTarget(c, t, 170, chunk);
    if (!id) {
      r.err = "the fixture human did not spawn";
      return r;
    }
    const Vec3 tC = centroid(id, torso), aC = centroid(id, arm);
    Vec3 side = tC - aC;
    side.y = 0.0f;
    side = side.len() > 1e-3f ? side.normalized() : Vec3{1, 0, 0};
    const Vec3 front = Vec3{0, 1, 0}.cross(side).normalized();
    support::TickRig rig(c, clock, chunk);
    rig.Authority().ground = &ground;
    // THE DRAW, as the bow takes it: a copy of the stage's quiver, one key.
    Kit kit = stageKit;
    const uint32_t key = rng::Hash3(0xA770B2u, clock, 0u);
    ItemInstance nocked;
    if (!KitTakeAmmo(kit, "arrow", key, &nocked) || venomOn(ItemLatticeIfAny(nocked, 0)) == 0) {
      r.err = "the drawn arrow wears no venom";
      return r;
    }
    const Vec3 aim = tC - side * 0.6f;
    const uint32_t seed = 0x9015A0u;
    rig.Authority().arrows.Launch(*arrowDef, aim - front * 12.0f, front,
                                  bowDef->bow.drawSpeed, Vec3{}, seed, {}, 0, &nocked);
    support::RunTicks(rig, 2);
    int slot = -1;
    for (const ArrowEvent& e : rig.Authority().arrows.Events())
      if (e.seed == seed && e.kind == ArrowEvent::Kind::Embedded) slot = e.slot;
    r.embedded = slot >= 0;
    r.delivered = mobs.LastCoatContact().toTarget;
    {
      const std::vector<PrefabVoxel> lat = mobs.LimbLattice(id, torso);
      r.wallVenom = venomOn(&lat);
    }
    // The venom works; the arrow stays in.
    for (int k = 0; k < settle; k += 10) {
      support::RunTicks(rig, 10);
      if (const Mob* m = mobs.FindMobById(id)) {
        r.peak = std::max(r.peak, mobs.LimbMaterialCount(id, torso, infect));
        if (const Mob::InfectStat* s = m->InfectStatOf(infect)) r.seeded = s->seeded;
      }
    }
    // THE PULL: into the quiver on the puller's back, wearing what it went
    // through.
    if (slot >= 0) {
      Kit puller;
      puller.equip.slots[(int)EquipSlotId::Back] = StackOf(c.items, quiverIdx);
      std::string got;
      r.pulled = PullArrowToKit(mobs, mobs.LimbBody(id, slot), puller, c.items, &got);
      const ItemInstance& pq = puller.equip.At((int)EquipSlotId::Back);
      r.intoQuiver = KitAmmoCount(puller, "arrow") == 1;
      if (!pq.held.empty())
        if (const std::vector<PrefabVoxel>* lat = ItemLatticeIfAny(pq.held[0], 0))
          for (const PrefabVoxel& v : *lat) {
            if (!v.stain) continue;
            (BodyStainMat(v.stain) == venom ? r.arrowVenom : r.arrowOther)++;
          }
    }
    uint64_t h = 1469598103934665603ull;
    for (uint32_t v : {(uint32_t)r.embedded, (uint32_t)r.pulled, r.delivered, r.wallVenom,
                       r.seeded, r.peak, r.arrowVenom, r.arrowOther}) {
      h ^= v;
      h *= 1099511628211ull;
    }
    h ^= (uint64_t)std::llround(mobs.TotalHp(id) * 1000.0);
    r.digest = h;
    return r;
  };
  const Shot s1 = shot(81000u);
  const Shot s2 = shot(81000u);
  if (!s1.err.empty()) fail("C: " + s1.err);
  lines += Format("C: embedded %s; %u venom levels into the channel, %u venom cells on the "
                  "torso, %u seeded (peak %u infected); pulled %s into the quiver %s, "
                  "wearing %u venom + %u other coat cells | ",
                  s1.embedded ? "yes" : "NO", s1.delivered, s1.wallVenom, s1.seeded, s1.peak,
                  s1.pulled ? "out" : "NOT out", s1.intoQuiver ? "yes" : "NO", s1.arrowVenom,
                  s1.arrowOther);
  if (!s1.embedded) fail("C: the venomed arrow did not embed in the torso");
  if (s1.wallVenom == 0) fail("C: no venom reached the channel's walls");
  if (s1.seeded == 0) fail("C: the venom on the arrow seeded no infection");
  if (!s1.pulled || !s1.intoQuiver) fail("C: the arrow did not come out into the quiver");
  if (s1.arrowOther == 0 && s1.arrowVenom == 0)
    fail("C: the pulled arrow came back clean (it should wear the wound's fluid)");
  lines += Format("D: replay %016llx vs %016llx %s",
                  (unsigned long long)s1.digest, (unsigned long long)s2.digest,
                  s1.digest == s2.digest ? "IDENTICAL" : "DIFFER");
  if (s1.digest != s2.digest) fail("D: the shot did not replay bit-identically");

  mobs.Reset();
  c.debris.Reset();
  PrepareWorld(c);
  detail = lines + why;
  std::printf("poison-arrow: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// spell-pierce: the matter IS the projectile
// ---------------------------------------------------------------------------
// docs/PLAN_projectiles.md decision 3, package D (game/spell.h FlightMatter,
// game/pierce.h SpellMatterStrike). Every shot is a REAL CAST into the rig
// session's own SpellSystem, flown by THE tick (TickAuthority phase I, the
// player's spell probe and its matterHit), at a FRESH human posed like every
// wound fixture, aimed at its torso from the front 6 cm off the midline (the
// `pierce` gate's torso ray). Per shot it prints what the VM logged (kind,
// outcome, depth, cells by material, joules, hp) and what the body shows
// `spellPierceSettleTicks` later (coat cells, cells eaten, burn hp).
//
//   stone projectile  a solid lump: it must get IN (cells taken) and not out.
//   stone bolt        the same matter twice as fast: deeper, or through.
//   water projectile  a liquid: a coat on the skin, NOTHING carved, then or
//                     after.
//   acid projectile   a coat, and the acid then eats what it sits on.
//   lava projectile   a coat, and the wound burns (burn hp / fire / cells).
//                     ...a DROP at x1: at most spellPierceSplashX1MaxCells
//                     coated skin cells on the whole body; x0.25 still
//                     lands; x4 coats spellPierceSplashX4MinRatio times more
//                     (the splash's width grows linearly with the matter).
//   sand bolt         a powder: a scour that stops in the first body.
//   price             stone bolt > stone projectile > stone lob, by the kinetic
//                     term; the NPC kits' words priced with it and without.
//   replay            the stone bolt again on a fresh spawn: identical numbers.
Status GateSpellPierce(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  mobs.SetNextIdCounter(1);
  PrepareWorld(c);
  const int human = mobs.FindDef("human");
  if (human < 0) {
    detail = "no `human` mob def loaded";
    return Status::Fail;
  }
  const int settle = (int)BaselineNumber("spellPierceSettleTicks", 60);
  bool ok = true;
  std::string why, lines;
  auto fail = [&](const std::string& s) {
    ok = false;
    why += " | " + s;
  };
  auto matName = [&](uint32_t m) {
    return m < c.mats.size() ? c.mats[m].name : std::string("?");
  };
  auto matId = [&](const char* nm) -> uint32_t {
    for (uint32_t i = 0; i < (uint32_t)c.mats.size(); i++)
      if (c.mats[i].name == nm) return i;
    return 0;
  };

  // ---- THE RIG, and the glyphs the tick casts with ----------------------------
  Target tgt;
  tgt.defIndex = human;
  tgt.limb = 0;
  IVec3 chunk{};
  if (!SpawnTarget(c, tgt, 170, chunk)) {
    detail = "the fixture human did not spawn";
    return Status::Fail;
  }
  support::TickRig rig(c, 71000, chunk);
  GlyphLibrary& lib = rig.Glyphs();
  {
    std::string gerr;
    if (!LoadGlyphs(AssetDir() + "/spells/glyphs.json", c.mats, lib, gerr)) {
      detail = "glyph load failed: " + gerr;
      std::printf("spell-pierce: FAIL (%s)\n", detail.c_str());
      return Status::Fail;
    }
  }
  SpellSystem& spells = rig.Session().spells;
  spells.SetLibrary(&lib);
  auto speak = [&](const GlyphLibrary& L, const std::string& words) {
    SpellStack st;
    size_t pos = 0;
    while (pos <= words.size()) {
      size_t sp = words.find(' ', pos);
      if (sp == std::string::npos) sp = words.size();
      const std::string w = words.substr(pos, sp - pos);
      if (!w.empty()) {
        int32_t mag = kMagUnset;
        SpellTiming tm;
        const int gi = ParseWord(L, w, mag, tm);
        if (gi >= 0) st.Push(gi, mag, tm);
      }
      pos = sp + 1;
    }
    return CompileSpell(L, st);
  };

  // ---- ONE SHOT ------------------------------------------------------------------
  struct Shot {
    std::string words;
    bool ran = false, logged = false;
    SpellStrikeLog log;
    uint64_t id = 0;
    int torso = -1;
    float hp0 = 0, hpHit = 0, hpEnd = 0, burnHp = 0;
    uint32_t vox0 = 0, voxHit = 0, voxEnd = 0;
    uint32_t coatHit = 0, fireEnd = 0;
    // THE SPLASH'S FOOTPRINT, WHOLE BODY (owner 2026-10-13: "the lava splashed
    // on the character is too wide"): coated cells on every limb and how many
    // limbs wear any at the hit, then after the settle the coat left, what
    // burnt (fire + the flesh/undercloth/hair/cloth burn chain) and what is
    // gone -- so "the splash was wide" and "the fire spread" are two numbers.
    uint32_t bodyCoatHit = 0, limbsCoatHit = 0, bodyCoatEnd = 0, bodyBurntEnd = 0;
    uint32_t bodyVox0 = 0, bodyVoxEnd = 0;
    // ...and at the SKIN's resolution: a drop-sized coat works on the fine
    // lattice (1.25 cm) and can eat or sear there without taking a single
    // cell of the coarse collider the `vox` counts above are made of.
    uint32_t bodySkin0 = 0, bodySkinEnd = 0;
    int ticksToHit = -1;
  };
  // Every base limb of the fixture, summed: the splash is not the torso's.
  auto bodySum = [&](uint64_t id, auto&& per) {
    uint32_t n = 0;
    const size_t nl = mobs.Defs()[human].limbs.size();
    for (size_t li = 0; li < nl; li++) n += per(id, (int)li);
    return n;
  };
  auto bodyCells = [&](uint64_t id) {
    return bodySum(id, [&](uint64_t m, int li) { return mobs.LimbVoxelCount(m, li); });
  };
  // Live skin cells (a burn's material-0 tombstones are not skin).
  auto bodySkin = [&](uint64_t id) {
    return bodySum(id, [&](uint64_t m, int li) {
      const uint32_t n = mobs.LimbSkinVoxelCount(m, li), dead = mobs.LimbMaterialCount(m, li, 0);
      return n > dead ? n - dead : 0u;
    });
  };
  std::vector<uint32_t> burntMats;
  for (const char* nm : {"fire", "flesh_cooked", "flesh_burning", "flesh_charred",
                         "undercloth_seared", "undercloth_burning", "undercloth_charred",
                         "hair_burning", "cloth_burning", "cloth_charred"})
    for (uint32_t i = 0; i < (uint32_t)c.mats.size(); i++)
      if (c.mats[i].name == nm) burntMats.push_back(i);
  auto bodyBurnt = [&](uint64_t id) {
    return bodySum(id, [&](uint64_t m, int li) {
      uint32_t k = 0;
      for (uint32_t bm : burntMats) k += mobs.LimbMaterialCount(m, li, bm);
      return k;
    });
  };
  auto shoot = [&](const std::string& words, uint32_t coatMat) -> Shot {
    Shot s;
    s.words = words;
    spells.Clear();
    s.id = SpawnTarget(c, tgt, 170, chunk);
    rig.SetFixtureChunk(chunk);
    if (!s.id) return s;
    const auto& L = mobs.Defs()[human].limbs;
    int arm = -1;
    for (size_t i = 0; i < L.size(); i++) {
      if (L[i].name == "torso") s.torso = (int)i;
      if (L[i].name == "armU.L" || (arm < 0 && L[i].name == "armU.R")) arm = (int)i;
    }
    if (s.torso < 0 || arm < 0) return s;
    auto centroid = [&](int li) {
      Vec3 sum{};
      const uint32_t n = 512;
      for (uint32_t k = 0; k < n; k++) sum += mobs.LimbVoxelPos(s.id, li, k * 7919u);
      return sum * (1.0f / (float)n);
    };
    const Vec3 torsoC = centroid(s.torso), armC = centroid(arm);
    Vec3 side = torsoC - armC;
    side.y = 0.0f;
    side = side.len() > 1e-3f ? side.normalized() : Vec3{1, 0, 0};
    const Vec3 front = Vec3{0, 1, 0}.cross(side).normalized();
    // The hand resolves its payload at reach (glyphs.json hand.reach) and the
    // flight leaves from there: 10 voxels back puts the muzzle ~5 voxels off
    // the chest, close enough that a lob's drop stays on the torso.
    const Vec3 from = torsoC - side * 0.6f - front * 10.0f;
    s.hp0 = mobs.TotalHp(s.id);
    s.vox0 = mobs.LimbVoxelCount(s.id, s.torso);
    s.bodyVox0 = bodyCells(s.id);
    s.bodySkin0 = bodySkin(s.id);
    const CastList cl = speak(lib, words);
    CasterState cs;
    cs.mana = cs.manaMax = 1 << 20;
    CasterHealth hp;
    hp.get = [](void*) { return (int32_t)(1 << 20); };
    SpellEmission e0;
    const SpellProbe probe = WorldSpellProbe(c.world);
    spells.Cast(cl, cs, hp, kPlayerCasterId,
                {SpellFxFromFloat(from.x), SpellFxFromFloat(from.y), SpellFxFromFloat(from.z)},
                {SpellFxFromFloat(front.x), SpellFxFromFloat(front.y), SpellFxFromFloat(front.z)},
                rig.tick + 1, e0, &probe);
    s.ran = spells.LiveCount() > 0;
    for (int k = 0; k < 40 && spells.LiveCount() > 0 && spells.StrikeLog().empty(); k++) {
      support::RunTicks(rig, 1);
      if (!spells.StrikeLog().empty()) s.ticksToHit = k + 1;
    }
    if (!spells.StrikeLog().empty()) {
      s.logged = true;
      s.log = spells.StrikeLog().front();
    }
    s.hpHit = mobs.TotalHp(s.id);
    s.voxHit = mobs.LimbVoxelCount(s.id, s.torso);
    s.coatHit = coatMat ? mobs.LimbCoatMatCount(s.id, s.torso, coatMat, 1) : 0;
    if (coatMat) {
      s.bodyCoatHit = bodySum(s.id, [&](uint64_t m, int li) {
        return mobs.LimbCoatMatCount(m, li, coatMat, 1);
      });
      s.limbsCoatHit = bodySum(s.id, [&](uint64_t m, int li) {
        return mobs.LimbCoatMatCount(m, li, coatMat, 1) > 0 ? 1u : 0u;
      });
    }
    support::RunTicks(rig, settle);
    s.hpEnd = mobs.TotalHp(s.id);
    s.voxEnd = mobs.LimbVoxelCount(s.id, s.torso);
    s.fireEnd = mobs.LimbMaterialCount(s.id, s.torso, matId("fire"));
    if (coatMat)
      s.bodyCoatEnd = bodySum(s.id, [&](uint64_t m, int li) {
        return mobs.LimbCoatMatCount(m, li, coatMat, 1);
      });
    s.bodyBurntEnd = bodyBurnt(s.id);
    s.bodyVoxEnd = bodyCells(s.id);
    s.bodySkinEnd = bodySkin(s.id);
    if (const Mob* m = mobs.FindMobById(s.id)) s.burnHp = m->HpLostBy(DamageCause::Burn);
    spells.Clear();
    return s;
  };
  auto describe = [&](const Shot& s) {
    if (!s.ran) return std::string("[not cast]");
    if (!s.logged) return std::string("[no body met]");
    const SpellBodyStrike& r = s.log.r;
    std::string mats;
    for (const SpellBodyStrike::MatCells& m : r.byMat)
      mats += Format("%s%s %u", mats.empty() ? "" : ", ", matName(m.mat).c_str(), m.cells);
    const SpellMatterKind k = s.log.shot.kind;
    const bool point = k == SpellMatterKind::Pierce || k == SpellMatterKind::Scour;
    return Format(
        "[%s %s %.2f kg @ %.1f m/s = %.1f J (tip %.2f, sharp %.2f): %s%s, %.1f cm, %u cells "
        "(%s), spent %.1f J, hp %.1f%s; coat %u then; after %d ticks: torso cells %u -> %u "
        "-> %u, hp %.1f -> %.1f -> %.1f, burn hp %.1f, fire %u]",
        SpellMatterKindName(k), matName(s.log.shot.material).c_str(), s.log.shot.massKg,
        s.log.shot.speedMps, r.energyJ, s.log.shot.tipRadius, s.log.shot.sharpness,
        point ? PierceOutcomeName((PierceOutcome)r.outcome) : (r.tookMatter ? "coated" : "touched"),
        r.through ? Format(", OUT at %.1f m/s", r.exitSpeedMps).c_str() : "",
        r.depthVox * kVoxelMeters * 100.0f, r.cells, mats.empty() ? "none" : mats.c_str(),
        r.spentJ, r.hp, r.killed ? ", KILLED" : "", s.coatHit, settle, s.vox0, s.voxHit,
        s.voxEnd, s.hp0, s.hpHit, s.hpEnd, s.burnHp, s.fireEnd) +
           (k == SpellMatterKind::Splash
                ? Format("[splash coat %u r %.2f vox (%.1f cm): body coat %u cells on %u limbs "
                         "at the hit, %u after %d ticks; burnt %u, body cells %u -> %u, "
                         "skin %u -> %u]",
                         s.log.shot.coatAmount, s.log.shot.splashRadius,
                         s.log.shot.splashRadius * kVoxelMeters * 100.0f,
                         s.bodyCoatHit, s.limbsCoatHit,
                         s.bodyCoatEnd, settle, s.bodyBurntEnd, s.bodyVox0, s.bodyVoxEnd,
                         s.bodySkin0, s.bodySkinEnd)
                : std::string());
  };

  const uint32_t mWater = matId("water"), mAcid = matId("acid"), mLava = matId("lava");
  const Shot stoneP = shoot("stone projectile", 0);
  const Shot stoneB = shoot("stone bolt", 0);
  const Shot water = shoot("water projectile", mWater);
  const Shot acid = shoot("acid projectile", mAcid);
  const Shot lava = shoot("lava projectile", mLava);
  // ...and the same lava at the least and the most a page can scale one noun
  // (x0.25, x4): the splash's growth with magnitude, end to end.
  const Shot lavaDrop = shoot("lava@0.25 projectile", mLava);
  const Shot lavaBig = shoot("lava@4 projectile", mLava);
  const Shot sand = shoot("sand bolt", 0);
  const Shot again = shoot("stone bolt", 0);
  lines += "STONE PROJECTILE " + describe(stoneP) + " | ";
  lines += "STONE BOLT " + describe(stoneB) + " | ";
  lines += "WATER " + describe(water) + " | ";
  lines += "ACID " + describe(acid) + " | ";
  lines += "LAVA " + describe(lava) + " | ";
  lines += "LAVA x0.25 " + describe(lavaDrop) + " | ";
  lines += "LAVA x4 " + describe(lavaBig) + " | ";
  lines += "SAND BOLT " + describe(sand) + " | ";

  auto hitTorso = [&](const Shot& s, SpellMatterKind k, const char* nm) {
    if (!s.ran) {
      fail(std::string(nm) + ": not cast");
      return false;
    }
    if (!s.logged || s.log.r.mobId != s.id) {
      fail(std::string(nm) + ": its matter met no body (or not the fixture)");
      return false;
    }
    if (s.log.shot.kind != k) {
      fail(Format("%s: flew as %s, not %s", nm, SpellMatterKindName(s.log.shot.kind),
                  SpellMatterKindName(k)));
      return false;
    }
    return true;
  };
  // ---- solids: in, and deeper at speed ----------------------------------------
  if (hitTorso(stoneP, SpellMatterKind::Pierce, "stone projectile")) {
    const SpellBodyStrike& r = stoneP.log.r;
    if (r.cells < (uint32_t)BaselineNumber("spellPierceStoneMinCells", 1))
      fail("stone projectile took no cells (it did not get in)");
    if (r.through) fail("stone projectile went through (it should embed)");
  }
  if (hitTorso(stoneB, SpellMatterKind::Pierce, "stone bolt") && stoneP.logged) {
    const SpellBodyStrike& p = stoneP.log.r;
    const SpellBodyStrike& b = stoneB.log.r;
    if (!(b.through || b.depthVox > p.depthVox))
      fail(Format("stone bolt was no deeper than the projectile (%.2f vs %.2f vox)", b.depthVox,
                  p.depthVox));
    if (b.energyJ <= p.energyJ) fail("stone bolt carried no more energy than the projectile");
  }
  // ---- liquids: a coat, never a channel ---------------------------------------
  if (hitTorso(water, SpellMatterKind::Splash, "water projectile")) {
    if (water.log.r.coated == 0 || water.coatHit == 0) fail("water: no coat on the skin");
    if (water.log.r.cells != 0 || water.voxHit != water.vox0 || water.voxEnd != water.vox0)
      fail(Format("water carved the torso (%u -> %u -> %u cells)", water.vox0, water.voxHit,
                  water.voxEnd));
  }
  if (hitTorso(acid, SpellMatterKind::Splash, "acid projectile")) {
    if (acid.log.r.coated == 0) fail("acid: no coat on the skin");
    if (acid.log.r.cells != 0 || acid.voxHit != acid.vox0)
      fail("acid PENETRATED (a liquid never makes a channel)");
    // Eaten at EITHER resolution: a drop-sized coat (x1, ~2 cm) eats the fine
    // skin it sits on and may not take one coarse collider cell in 2 s.
    const uint32_t eaten = acid.vox0 > acid.voxEnd ? acid.vox0 - acid.voxEnd : 0;
    const uint32_t skinEaten =
        acid.bodySkin0 > acid.bodySkinEnd ? acid.bodySkin0 - acid.bodySkinEnd : 0;
    if ((float)eaten < (float)BaselineNumber("spellPierceAcidMinEaten", 1) &&
        (float)skinEaten < (float)BaselineNumber("spellPierceAcidMinEaten", 1) &&
        acid.burnHp <= 0.0f)
      fail(Format("acid: the coat ate nothing in %d ticks", settle));
    RecordObserved("spellPierceAcidEaten", (double)eaten);
    RecordObserved("spellPierceAcidSkinEaten", (double)skinEaten);
  }
  if (hitTorso(lava, SpellMatterKind::Splash, "lava projectile")) {
    if (lava.log.r.coated == 0) fail("lava: no coat on the skin");
    // ...and the same for the burn: seared (the flesh chain, fire) or lost
    // skin under a drop counts as a burn at the wound.
    const uint32_t lost = lava.vox0 > lava.voxEnd ? lava.vox0 - lava.voxEnd : 0;
    const uint32_t skinLost =
        lava.bodySkin0 > lava.bodySkinEnd ? lava.bodySkin0 - lava.bodySkinEnd : 0;
    if (lava.burnHp < (float)BaselineNumber("spellPierceLavaMinBurnHp", 0.01) &&
        lava.fireEnd == 0 && lost == 0 && skinLost == 0 && lava.bodyBurntEnd == 0)
      fail(Format("lava: no burn at the wound in %d ticks", settle));
    RecordObserved("spellPierceLavaBurnHp", (double)lava.burnHp);
    // A ONE-VOXEL SPLASH IS A DROP, NOT A DRENCH (owner 2026-10-13): the coat
    // a x1 lava projectile lays on the whole body at the hit is bounded, and
    // the cast scaled to x4 lays strictly more -- the width grows with the
    // magnitude (glyphs.json splashRadiusMille, SplashRadiusMille).
    RecordObserved("spellPierceSplashX1Cells", (double)lava.bodyCoatHit);
    const double x1Max = BaselineNumber("spellPierceSplashX1MaxCells", 40);
    if ((double)lava.bodyCoatHit > x1Max)
      fail(Format("lava x1: the splash coated %u cells (ceiling %.0f) -- too wide for one "
                  "voxel of matter",
                  lava.bodyCoatHit, x1Max));
  }
  if (hitTorso(lavaDrop, SpellMatterKind::Splash, "lava x0.25") && lavaDrop.log.r.coated == 0)
    fail("lava x0.25: no coat at all (the smallest splash must still land)");
  if (hitTorso(lavaBig, SpellMatterKind::Splash, "lava x4")) {
    RecordObserved("spellPierceSplashX4Cells", (double)lavaBig.bodyCoatHit);
    const double grow = BaselineNumber("spellPierceSplashX4MinRatio", 4.0);
    if ((double)lavaBig.bodyCoatHit < grow * (double)std::max(1u, lava.bodyCoatHit))
      fail(Format("lava x4: coated %u cells, under %.1fx the x1 splash's %u -- the width does "
                  "not grow with the magnitude",
                  lavaBig.bodyCoatHit, grow, lava.bodyCoatHit));
  }
  // ---- powder: a scour, never a channel through ------------------------------
  if (hitTorso(sand, SpellMatterKind::Scour, "sand bolt") && sand.log.r.through)
    fail("sand went through (a scour always stops)");
  // ---- the same bolt again: identical -----------------------------------------
  if (stoneB.logged && again.logged) {
    const SpellBodyStrike& a = stoneB.log.r;
    const SpellBodyStrike& b = again.log.r;
    bool same = a.cells == b.cells && a.depthVox == b.depthVox && a.hp == b.hp &&
                a.through == b.through && a.exitSpeedMps == b.exitSpeedMps &&
                a.byMat.size() == b.byMat.size();
    for (size_t k = 0; same && k < a.byMat.size(); k++)
      same = a.byMat[k].mat == b.byMat[k].mat && a.byMat[k].cells == b.byMat[k].cells;
    if (!same) fail("the stone bolt did not replay identically on a fresh spawn");
  } else {
    fail("the stone bolt replay met nothing");
  }
  if (stoneP.logged) {
    RecordObserved("spellPierceStoneCells", (double)stoneP.log.r.cells);
    RecordObserved("spellPierceStoneDepthCm", (double)(stoneP.log.r.depthVox * kVoxelMeters * 100.0f));
  }
  if (stoneB.logged) {
    RecordObserved("spellPierceBoltCells", (double)stoneB.log.r.cells);
    RecordObserved("spellPierceBoltHp", (double)stoneB.log.r.hp);
  }

  // ---- THE PRICE: speed is not free -------------------------------------------
  // Each sentence priced as shipped and with the kinetic term off (the price
  // it had before package D), so the readout is the before/after table.
  GlyphLibrary noKin = lib;
  noKin.budgets.rateKinetic = 0;
  std::string prices;
  int32_t pProj = 0, pBolt = 0, pLob = 0;
  for (const char* w :
       {"stone projectile", "stone bolt", "stone lob", "steel bolt", "gold projectile",
        "sand projectile", "water projectile", "acid projectile", "lava projectile",
        "stone pierce projectile", "stone speed@4 projectile", "fire fire bolt",
        "fire fire explosive@1.5 bolt", "anything transmute lava bolt", "lift@0.5 aura bolt"}) {
    const CastList now = speak(lib, w), was = speak(noKin, w);
    prices += Format("%s%s %d (was %d; word %d tariff %d carry %d)", prices.empty() ? "" : "; ",
                     w, now.manaCost, was.manaCost, now.wordCost, now.tariff, now.carryCost);
    if (std::string(w) == "stone projectile") pProj = now.manaCost;
    if (std::string(w) == "stone bolt") pBolt = now.manaCost;
    if (std::string(w) == "stone lob") pLob = now.manaCost;
  }
  lines += "PRICE " + prices + " | ";
  if (!(pBolt > pProj && pProj > pLob))
    fail(Format("price order: stone bolt %d, projectile %d, lob %d", pBolt, pProj, pLob));
  RecordObserved("spellPiercePriceStoneProjectile", (double)pProj);
  RecordObserved("spellPiercePriceStoneBolt", (double)pBolt);
  {
    const SpellReadout rd = DescribeSpell(lib, speak(lib, "stone projectile"));
    lines += "READOUT stone projectile: " + rd.verdict + " | ";
  }

  mobs.Reset();
  c.debris.Reset();
  PrepareWorld(c);
  detail = lines + why;
  std::printf("spell-pierce: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ============================================================================
// look-pick — G TAKES WHAT THE RETICLE IS ON, WITH WIGGLE ROOM (game/lookpick.h)
// ============================================================================
//
// Owner ticket 2026-10-16_11-00-17: "picking up items off of the floor can be
// a nightmare, same with targeting/picking up arrows ... give it a little bit
// of wiggle room". The take used to be one pixel-thin Jolt ray; this asserts
// the cone that replaced it, on REAL bodies (registered ground items, a real
// arrow slot on a real creature) through the one function main.cpp's prompt
// and press both read. Terrain is a synthetic kindAt (a flat floor, and a wall
// when a scene asks for one): the subject is the picker's rule, not the mirror.
//
//   S1 a chunky item half a cone off the reticle: the bare ray misses it, the
//      pick takes it.
//   S2 ...the same item past the cone: nothing.
//   S3 a THIN arrow lying on the floor, between the two cones: taken (and
//      only because it is thin).
//   S4 two items both in the cone: the one nearer the reticle wins, both ways.
//   S5 a stone wall between the eye and an item aimed dead on: refused, and
//      the same aim without the wall takes it (the control arm).
//   S6 an arrow STANDING in the ground (stuck, kinematic, its point in the
//      floor), aimed at the floor beside it: taken.
//   S7 an arrow hidden behind a creature's body, aimed dead on: refused; the
//      same arrow moved out of its shadow: taken.
//   S8 an arrow EMBEDDED in that creature, half a thin cone off: the arrow
//      (EmbeddedArrow, IsEmbeddedArrowBody), not the body it stands in.
//   S9 a corpse-with-loot exactly under the reticle (exactActionable) beats an
//      item a few degrees off; without the predicate the item wins.
Status GateLookPick(Ctx& c, std::string& detail) {
  MobSystem& mobs = c.mobs;
  IdCounterScope idScope(mobs);
  const int human = mobs.FindDef("human");
  const ItemDef* arrowDef = c.items.Named("arrow");
  const ItemDef* chunkyDef = c.items.Named("iron_helm");
  if (!chunkyDef) chunkyDef = c.items.Named("flask");
  const ItemDef* chunky2Def = c.items.Named("flask");
  if (human < 0 || !arrowDef || !chunkyDef || !chunky2Def) {
    detail = "no `human` def, or no arrow / iron_helm / flask in the item library";
    return Status::Fail;
  }
  int torso = -1;
  {
    const auto& L = mobs.Defs()[human].limbs;
    for (size_t i = 0; i < L.size(); i++)
      if (L[i].name == "torso") torso = (int)i;
  }
  if (torso < 0) {
    detail = "the human has no torso";
    return Status::Fail;
  }
  PrepareWorld(c);
  Target t;
  t.defIndex = human;
  t.limb = 0;
  IVec3 chunk{};
  const uint64_t mobId = SpawnTarget(c, t, 170, chunk);
  if (!mobId) {
    detail = "the fixture human did not spawn";
    return Status::Fail;
  }
  // The registry the game binds: a destroyed body leaves it.
  WorldItems ground;
  c.debris.SetOnBodyGone([&ground](uint64_t h) { ground.OnBodyGone(h); });
  struct Unhook {
    DebrisSystem& d;
    ~Unhook() { d.SetOnBodyGone(nullptr); }
  } unhook{c.debris};

  Vec3 torsoC{};
  for (uint32_t k = 0; k < 512; k++) torsoC += mobs.LimbVoxelPos(mobId, torso, k * 7919u);
  torsoC = torsoC * (1.0f / 512.0f);

  const Tuning::Player& tp = CurrentTuning().player;
  lookpick::Params pp;
  pp.coneDeg = tp.pickConeDeg;
  pp.coneSmallDeg = tp.pickConeSmallDeg;
  pp.smallSize = tp.pickSmallSize;
  if (!(pp.coneSmallDeg > pp.coneDeg + 1.0f)) {
    detail = Format("player.pickConeSmallDeg %.1f is not wider than pickConeDeg %.1f: "
                    "S3 cannot tell the thin cone from the plain one",
                    pp.coneSmallDeg, pp.coneDeg);
    return Status::Fail;
  }

  // ---- the scene's terrain: a floor, and a wall when a scene asks ----------
  const Vec3 S = torsoC + Vec3{40.0f, -8.0f, 0.0f};   // the item floor, clear of the creature
  const int floorY = ifloor(S.y);
  bool wallOn = false;
  IVec3 wlo{}, whi{};
  auto kindAt = [&](IVec3 cc) {
    if (wallOn && cc.x >= wlo.x && cc.x <= whi.x && cc.y >= wlo.y && cc.y <= whi.y &&
        cc.z >= wlo.z && cc.z <= whi.z)
      return CellKind::Solid;
    return cc.y < floorY ? CellKind::Solid : CellKind::Air;
  };

  bool ok = true;
  std::string why, lines;
  auto fail = [&](const std::string& s) {
    ok = false;
    why += " | " + s;
  };

  // ---- items, kinematic so nothing moves between the aim and the pick ------
  auto place = [&](const ItemDef& d, Vec3 at, Quat q = Quat{}) {
    BodyTransform xf{};
    xf.pos = at;
    xf.quat[0] = q.x;
    xf.quat[1] = q.y;
    xf.quat[2] = q.z;
    xf.quat[3] = q.w;
    ItemInstance inst;
    return DropItemToWorldXf(d, inst, xf, Vec3{}, c.phys, c.debris, nullptr, ground,
                             nullptr, /*stuck=*/true);
  };
  auto centreOf = [&](uint64_t b) {
    BodyTransform xf{};
    Vec3 lo, hi;
    if (!c.phys.GetTransform(b, xf) || !c.phys.GetLocalBounds(b, lo, hi)) return Vec3{};
    const Quat q{xf.quat[0], xf.quat[1], xf.quat[2], xf.quat[3]};
    return xf.pos + QuatRotate(q, (lo + hi) * 0.5f);
  };
  // The angle (deg) of body `b`'s own box off the ray from `from` along `dir`:
  // the measure lookpick uses, re-derived here from its exposed primitive.
  auto angleTo = [&](uint64_t b, Vec3 from, Vec3 dir) {
    BodyTransform xf{};
    Vec3 lo, hi, qL;
    if (!c.phys.GetTransform(b, xf) || !c.phys.GetLocalBounds(b, lo, hi)) return 999.0f;
    const Quat q{xf.quat[0], xf.quat[1], xf.quat[2], xf.quat[3]};
    return lookpick::BoxAngleDeg(QuatRotateInv(q, from - xf.pos), QuatRotateInv(q, dir),
                                 200.0f, lo, hi, qL);
  };
  constexpr float kDeg = 0.0174532925f;
  const Vec3 up{0, 1, 0};
  // A look `deg` degrees off body `b` (by its box), yawed toward `away`.
  auto aimOff = [&](uint64_t b, Vec3 from, float deg, Vec3 away) {
    const Vec3 base = (centreOf(b) - from).normalized();
    const float s =
        (QuatRotate(QuatAxisAngle(up, kDeg), base) - base).dot(away) >= 0.0f ? 1.0f : -1.0f;
    Vec3 d = base;
    for (float th = 0.0f; th < 60.0f; th += 0.05f) {
      d = QuatRotate(QuatAxisAngle(up, s * th * kDeg), base);
      if (angleTo(b, from, d) >= deg) break;
    }
    return d;
  };
  auto pick = [&](Vec3 from, Vec3 dir,
                  std::function<bool(uint64_t)> actionable = nullptr) {
    lookpick::Query q;
    q.from = from;
    q.dir = dir;
    q.hand = from;
    q.kindAt = kindAt;
    q.exactActionable = std::move(actionable);
    return lookpick::Pick(q, pp, c.phys, mobs, ground);
  };
  auto kindName = [](lookpick::Kind k) {
    switch (k) {
      case lookpick::Kind::GroundItem: return "item";
      case lookpick::Kind::EmbeddedArrow: return "embedded arrow";
      case lookpick::Kind::Exact: return "exact";
      default: return "none";
    }
  };
  auto line = [&](const char* tag, const lookpick::Result& r, uint64_t want) {
    lines += Format("%s: %s %s at %.1f deg (bare ray %s; reach %d, cone %d, occluded %d) | ",
                    tag, r.body == want && want ? "PICKED" : (r.body ? "other" : "nothing"),
                    kindName(r.kind), r.angleDeg,
                    r.exactBody == want && want ? "on it" : (r.exactBody ? "elsewhere" : "none"),
                    r.inReach, r.inCone, r.occluded);
  };
  // The long axis of the arrow's own box, from an unrotated probe.
  auto arrowAxis = [&](Vec3& ctrL) {
    const uint64_t probe = place(*arrowDef, S);
    Vec3 lo, hi;
    c.phys.GetLocalBounds(probe, lo, hi);
    c.debris.DestroyBody(probe);
    ctrL = (lo + hi) * 0.5f;
    const Vec3 ext = hi - lo;
    return ext.x >= ext.y && ext.x >= ext.z ? Vec3{1, 0, 0}
           : ext.y >= ext.z                 ? Vec3{0, 1, 0}
                                            : Vec3{0, 0, 1};
  };
  const Vec3 eye = S + Vec3{0.0f, 14.0f, -8.0f};
  const float midDeg = 0.5f * (pp.coneDeg + pp.coneSmallDeg);

  // ======== S1 / S2: a chunky item off the reticle ===========================
  {
    const uint64_t h = place(*chunkyDef, S + Vec3{-4.0f, 0.0f, 0.0f});
    if (!h) {
      detail = "the chunky item would not go down";
      return Status::Fail;
    }
    const lookpick::Result r1 = pick(eye, aimOff(h, eye, 0.5f * pp.coneDeg, Vec3{-1, 0, 0}));
    line("S1 half a cone off", r1, h);
    if (r1.exactBody == h) fail("S1: the bare ray hit the item: not an off-centre aim");
    if (r1.body != h) fail("S1: an item half a cone off the reticle was not picked");
    const lookpick::Result r2 = pick(eye, aimOff(h, eye, pp.coneDeg + 3.0f, Vec3{-1, 0, 0}));
    line("S2 past the cone", r2, h);
    if (r2.body != 0) fail("S2: an item 3 deg past the cone was still picked");
    c.debris.DestroyBody(h);
  }
  // ======== S3: a thin arrow on the floor, between the cones =================
  {
    const uint64_t h = place(*arrowDef, S + Vec3{0.0f, 0.0f, 6.0f});
    Vec3 lo, hi;
    c.phys.GetLocalBounds(h, lo, hi);
    float e[3] = {hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
    std::sort(e, e + 3);
    lines += Format("arrow box %.1f x %.1f x %.1f | ", e[0], e[1], e[2]);
    if (!(e[1] < pp.smallSize)) fail("S3: the arrow does not read as thin");
    const lookpick::Result r = pick(eye, aimOff(h, eye, midDeg, Vec3{1, 0, 0}));
    line("S3 thin, between the cones", r, h);
    if (r.exactBody == h) fail("S3: the bare ray hit the arrow");
    if (r.body != h) fail("S3: a thin arrow inside the thin cone was not picked");
    c.debris.DestroyBody(h);
  }
  // ======== S4: two in the cone, the nearer the reticle wins =================
  {
    const uint64_t ha = place(*chunkyDef, S + Vec3{-3.0f, 0.0f, 2.0f});
    const uint64_t hb = place(*chunky2Def, S + Vec3{3.0f, 0.0f, 2.0f});
    const Vec3 ca = centreOf(ha), cb = centreOf(hb);
    // Slide the aim from one centre toward the other until the far one is
    // inside the plain cone too (half a degree in), the near one still the
    // nearer: both are candidates and the angle has to decide.
    auto between = [&](uint64_t hn, Vec3 cn, uint64_t hf, Vec3 cf) {
      Vec3 d = (cn - eye).normalized();
      for (float s = 0.0f; s < 0.5f; s += 0.01f) {
        d = (cn + (cf - cn) * s - eye).normalized();
        const float an = angleTo(hn, eye, d), af = angleTo(hf, eye, d);
        if (af <= pp.coneDeg - 0.5f && an < af) break;
      }
      return d;
    };
    const lookpick::Result ra = pick(eye, between(ha, ca, hb, cb));
    const lookpick::Result rb = pick(eye, between(hb, cb, ha, ca));
    line("S4 nearer A", ra, ha);
    line("S4 nearer B", rb, hb);
    if (ra.inCone < 2 || rb.inCone < 2) fail("S4: the two items were not both in the cone");
    if (ra.body != ha || rb.body != hb) fail("S4: the item nearer the reticle did not win");
    c.debris.DestroyBody(ha);
    c.debris.DestroyBody(hb);
  }
  // ======== S5: a wall in the way, and the control =============================
  {
    const uint64_t h = place(*chunkyDef, S + Vec3{0.0f, 0.0f, 3.0f});
    const Vec3 ctr = centreOf(h);
    const Vec3 dir = (ctr - eye).normalized();
    const lookpick::Result open = pick(eye, dir);
    const Vec3 mid = eye + (ctr - eye) * 0.5f;
    wlo = IVec3{ifloor(mid.x) - 4, ifloor(mid.y) - 4, ifloor(mid.z) - 1};
    whi = IVec3{ifloor(mid.x) + 4, ifloor(mid.y) + 4, ifloor(mid.z) + 1};
    wallOn = true;
    const lookpick::Result walled = pick(eye, dir);
    wallOn = false;
    line("S5 open", open, h);
    line("S5 behind a wall", walled, h);
    if (open.body != h) fail("S5: aimed dead on with nothing in the way, not picked");
    if (walled.body == h) fail("S5: picked THROUGH a stone wall");
    if (walled.occluded < 1) fail("S5: the wall refusal was not counted as occluded");
    c.debris.DestroyBody(h);
  }
  // ======== S6: an arrow standing in the ground ==============================
  {
    // Turned to point DOWN, centred two voxels above the floor so the point
    // is in it.
    Vec3 ctrL;
    const Quat q = QuatFromTo(arrowAxis(ctrL), Vec3{0, -1, 0});
    const Vec3 want = S + Vec3{-4.0f, 2.0f, 6.0f};
    const uint64_t h = place(*arrowDef, want - QuatRotate(q, ctrL), q);
    const lookpick::Result r = pick(eye, aimOff(h, eye, midDeg, Vec3{-1, 0, 0}));
    line("S6 standing in the ground", r, h);
    if (r.exactBody == h) fail("S6: the bare ray hit the arrow");
    if (r.body != h) fail("S6: an arrow standing in the ground beside the reticle was not picked");
    c.debris.DestroyBody(h);
  }
  // ======== S7: behind a body, and out of its shadow ============================
  const Vec3 eye2 = torsoC + Vec3{-14.0f, 0.0f, 0.0f};
  {
    Vec3 ctrL;
    const Quat q = QuatFromTo(arrowAxis(ctrL), Vec3{1, 0, 0});   // along the view
    const uint64_t h = place(*arrowDef, torsoC + Vec3{7.0f, 0.0f, 0.0f} - QuatRotate(q, ctrL), q);
    const lookpick::Result hid = pick(eye2, (centreOf(h) - eye2).normalized());
    line("S7 behind the body", hid, h);
    if (hid.body == h) fail("S7: picked an arrow THROUGH a creature's body");
    c.debris.DestroyBody(h);
    const uint64_t h2 =
        place(*arrowDef, torsoC + Vec3{7.0f, 0.0f, 9.0f} - QuatRotate(q, ctrL), q);
    const lookpick::Result seen = pick(eye2, (centreOf(h2) - eye2).normalized());
    line("S7 out of its shadow", seen, h2);
    if (seen.body != h2) fail("S7: the same arrow out of the body's shadow was not picked");
    c.debris.DestroyBody(h2);
  }
  // ======== S8: an arrow embedded in the creature ===============================
  {
    Mob* m = mobs.FindCreature(mobId);
    const int slot = m ? m->EmbedArrow(*arrowDef, torso, torsoC, Vec3{1, 0, 0}) : -1;
    const uint64_t ab = slot >= 0 ? mobs.LimbBody(mobId, slot) : 0;
    if (!ab) {
      fail("S8: the arrow would not embed in the torso");
    } else {
      const lookpick::Result r =
          pick(eye2, aimOff(ab, eye2, 0.5f * pp.coneSmallDeg, Vec3{0, 0, 1}));
      line("S8 embedded, half a thin cone off", r, ab);
      if (r.exactBody == ab) fail("S8: the bare ray hit the arrow");
      if (r.body != ab || r.kind != lookpick::Kind::EmbeddedArrow ||
          !IsEmbeddedArrowBody(mobs, r.body))
        fail("S8: the embedded arrow beside the reticle was not picked");
      m->PullArrow(slot);
    }
  }
  // ======== S9: a corpse with loot under the reticle =============================
  {
    // The look stays dead on the torso; the item floats seven voxels out
    // along it, raised until its box is `wantDeg` off the ray (placed by its
    // unrotated box, measured before it exists), so the bare ray passes under
    // it and lands on the creature.
    const Vec3 dir = (torsoC - eye2).normalized();
    const float wantDeg = std::min(pp.coneDeg - 0.5f, pp.exactBiasDeg + 2.5f);
    Vec3 lo, hi;
    {
      const uint64_t probe = place(*chunkyDef, S);
      c.phys.GetLocalBounds(probe, lo, hi);
      c.debris.DestroyBody(probe);
    }
    const Vec3 ctrL = (lo + hi) * 0.5f;
    Vec3 at = eye2 + dir * 7.0f - ctrL;
    bool setUp = false;
    for (float dy = 0.0f; dy < 12.0f && !setUp; dy += 0.05f) {
      at = eye2 + dir * 7.0f + up * dy - ctrL;
      Vec3 q;
      setUp = lookpick::BoxAngleDeg(eye2, dir, 200.0f, at + lo, at + hi, q) >= wantDeg;
    }
    const uint64_t h = place(*chunkyDef, at);
    auto onMob = [&](uint64_t b) {
      const Mob* o = mobs.FindOwner(b);
      return o != nullptr && o->Id() == mobId;
    };
    const lookpick::Result withCorpse = pick(eye2, dir, onMob);
    const lookpick::Result itemOnly = pick(eye2, dir);
    line("S9 corpse under the reticle", withCorpse, withCorpse.exactBody);
    line("S9 no corpse rule", itemOnly, h);
    if (!setUp || !onMob(withCorpse.exactBody)) {
      fail(Format("S9: could not set up a look on the creature with the item %.1f deg off",
                  wantDeg));
    } else {
      if (withCorpse.kind != lookpick::Kind::Exact)
        fail("S9: a lootable body under the reticle lost to an item a few degrees off");
      if (itemOnly.body != h) fail("S9: without the corpse rule the item was not picked");
    }
    c.debris.DestroyBody(h);
  }

  mobs.Reset();
  c.debris.Reset();
  detail = lines + why;
  std::printf("look-pick: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

Status GateArcher(Ctx& c, std::string& detail);   // selftest_archer.cpp

const std::vector<Gate>& WoundGates() {
  static const std::vector<Gate> g = {
      {"wound-chip", "mob", {}, false, GateWoundChip, /*needsRender=*/false},
      {"hair-rooted", "mob", {}, false, GateHairRooted, /*needsRender=*/false},
      {"corpse-head-laser", "mob", {}, false, GateCorpseHeadLaser,
       /*needsRender=*/false},
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
      {"head-cleave", "mob", {}, false, GateHeadCleave, false},
      {"blade-wounds", "mob", {}, false, GateBladeWounds, false},
      // A point driven through: arrows at three energies, through an arm into
      // the torso, and a harder skin that stops what goes through a man.
      {"pierce", "mob", {}, false, GatePierce, false},
      // The bow and its arrows on the real tick: embed and ride, through and
      // on, stick in the ground, pull out, flat damage only when it got in, a
      // volley twice, and the draw/release input path.
      {"arrow", "mob", {}, false, GateArrow, false},
      // G's pick under the reticle (game/lookpick.h, ticket 2026-10-16_11-00-17):
      // a cone, not a ray -- off-centre items and arrows (on the floor, in the
      // ground, in a body) are taken, nothing through a wall or a body.
      {"look-pick", "mob", {}, false, GateLookPick, false},
      // ...and NPC ARCHERS (game/mob_archery.h, selftest_archer.cpp): an
      // `archer` draws, looses through the same flights and gets into a
      // dummy; backs off from one inside its minRange; stops at 0 arrows and
      // draws its sidearm; a scene replays bit for bit.
      {"archer", "mob", {}, false, GateArcher, false},
      // ...and POISONED ARROWS (docs/PLAN_poison_arrows.md): a quiver that loads
      // and draws by key, one arrow brushed with venom and copied to the rest,
      // a venomed shot that seeds the wound and comes back out bloody, twice.
      {"poison-arrow", "mob", {}, false, GatePoisonArrow, false},
      // ...and a SPELL's matter as the point: a stone projectile embeds, a
      // stone bolt goes deeper, water/acid/lava splash a coat that acts.
      {"spell-pierce", "mob", {}, false, GateSpellPierce, false},
      {"wound-rebleed", "mob", {}, false, GateWoundRebleed, false},
      {"corpse-blunt", "mob", {}, false, GateCorpseBlunt, false},
      {"bruise-is-skin", "mob", {}, false, GateBruiseIsSkin, false},
      {"corpse-armor", "mob", {}, false, GateCorpseArmor, false},
      {"corpse-bleed", "mob", {}, false, GateCorpseBleed, false},
      {"body-stain", "mob", {}, false, GateBodyStain, false},
      {"corpse-wash", "mob", {}, false, GateCorpseWash, false},
      {"corpse-crossheat", "mob", {}, false, GateCorpseCrossheat, false},
      // A lit garment burns to nothing and lights the wearer (no world fire).
      {"garment-burn", "mob", {}, false, GateGarmentBurn, false},
      {"corpse-worn", "mob", {}, false, GateCorpseWorn, false},
      {"corpse-splatter", "mob", {}, false, GateCorpseSplatter, false},
      {"body-coat", "mob", {}, false, GateBodyCoat, false},
      {"mob-rain", "mob", {}, false, GateMobRain, false},
      // A living body's anatomy blood does not dry (no wound, no loss).
      {"living-blood", "mob", {}, false, GateLivingBlood, false},
      {"rain-oil", "mob", {}, false, GateRainOil, false},
      {"blast-stain", "mob", {}, false, GateBlastStain, false},
      {"wound-heal", "mob", {}, false, GateWoundHeal, false},
      {"corpse-burn", "mob", {}, false, GateCorpseBurn, false},
      {"laser-head", "mob", {}, false, GateLaserHead, false},
      // A stab into the heart kills; the same stabs beside it do not.
      {"heart", "mob", {}, false, GateHeart, false},
      {"joint-twins", "mob", {}, false, GateJointTwins, false},
      {"acid-coat", "mob", {}, false, GateAcidCoat, false},
      {"corpse-acid", "mob", {}, false, GateCorpseAcid, false},
      {"lava-oil-coat", "mob", {}, false, GateLavaOilCoat, false},
      // A lava-coated severed head in water is quenched and steams.
      {"lava-quench", "mob", {}, false, GateLavaQuench, false},
      // Every liquid coats a body; syrup by hand and by walking through it.
      {"liquid-coats", "mob", {}, false, GateLiquidCoats, false},
      // A HEALING coat (alchemy package D): enchanted blood / water rebuild
      // the limb toward its recipe, bounded by what was poured.
      {"heal-restore", "mob", {}, false, GateHealRestore, false},
      {"heal-wound", "mob", {}, false, GateHealWound, false},
      // Audit R3 + R4: healing under an emptied burn pot; a joint cell paid once.
      {"heal-joint", "mob", {}, false, GateHealJoint, false},
      {"scar-heal", "mob", {}, false, GateScarHeal, false},
      {"severed-hand", "mob", {}, false, GateSeveredHand, false},
      {"corpse-sleep", "mob", {}, false, GateCorpseSleep, false},
      {"corpse-cap", "mob", {}, false, GateCorpseCap, false},
      // A corpse in water floats or sinks by its matter (Mob::FloatLimpLimbs).
      {"corpse-float", "mob", {}, false, GateCorpseFloat, false},
      {"water-entry", "mob", {}, false, GateWaterEntry, false},
      // The body tickets of 2026-10-17: standing on a log is standing, a
      // floater drifts with the current, a body outside the window lies on
      // the ground out there.
      {"player-stand-body", "mob", {}, false, GatePlayerStandBody, false},
      {"float-current", "mob", {}, false, GateFloatCurrent, false},
      {"debris-outside", "mob", {}, false, GateDebrisOutside, false},
      {"player-corpse", "mob", {}, false, GatePlayerCorpse, false},
  };
  return g;
}

}  // namespace selftest
