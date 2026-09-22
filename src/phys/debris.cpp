#include "phys/debris.h"

#include "sim/bytestream.h"  // nothing here encodes; net/debrissync.h does

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unordered_set>

#include "phys/bodystain.h"
#include "phys/lattice.h"
#include "phys/marching_cubes.h"
#include "measure/perfscope.h"
#include "sim/bytestream.h"
#include "sim/reactcpu.h"
#include "sim/rng.h"
#include "sim/tuning.h"

namespace {

// Local printf-to-string, for ProfileReport. selftest.h has one but this file
// is engine code and must not include the harness.
std::string Fmt(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return std::string(buf);
}

// One ballistic blood particle, world voxels in, the particle system's fixed
// 24.8 voxels/tick out at the sim's 30 Hz (the twin of Mob's MakeDroplet).
// `micro` is sub-voxel spray that stains and dies; otherwise a real voxel.
ParticleSpawn BloodSpawn(Vec3 posVoxel, Vec3 vel, uint32_t material,
                         bool micro, int lifeTicks, int microScale) {
  ParticleSpawn s{};
  s.px = (int32_t)std::lround(posVoxel.x * 256.0f);
  s.py = (int32_t)std::lround(posVoxel.y * 256.0f);
  s.pz = (int32_t)std::lround(posVoxel.z * 256.0f);
  s.vx = (int32_t)std::lround(vel.x * 256.0f / 30.0f);
  s.vy = (int32_t)std::lround(vel.y * 256.0f / 30.0f);
  s.vz = (int32_t)std::lround(vel.z * 256.0f / 30.0f);
  s.payload = (uint16_t)(material & 0xFFFu);
  s.flags = kPFlagAlive;
  if (micro) {
    const int life = lifeTicks < 1 ? 1 : (lifeTicks > 255 ? 255 : lifeTicks);
    s.flags |= kPFlagMicro | ParticleMicroBits(microScale, life);
  }
  return s;
}

// The scan region, per axis. 256 since PLAN_rigidbody_islands.md §4: the flood
// is sparse (it costs what it walks, not the box), so the box is sized by what
// has to fit INSIDE it to be judged whole -- a redwood is 217 tall -- rather
// than by what a dense mask could afford. A component touching a face this far
// from the change is terrain, and terrain is anchored anyway.
constexpr int kMaxRegionCells = 256;
// The flood aborts past this and declares the component anchored: a bound on
// what one scan may mobilise, not a size estimate. A great_oak is ~90k cells.
constexpr uint32_t kMaxIslandVoxels = 250000;
// A body shard's extent per axis (PLAN §5). Under the int8 body lattice's 120
// with headroom; an oak (92) is one shard, a redwood (217) is three.
constexpr int kShardCells = 96;
// A component that reaches this far BELOW the changed box is anchored: it is
// the ground. The 80-cell box used to supply this bound by accident (its floor
// was 40 cells under the change); with a 256 box a flood into stone-filled
// terrain walked chunk after chunk of never-fetched rock, was held for a fetch
// round each time, and gave up after 32 (51 abandoned scans over one burning
// oak). What this wrongly pins is a genuinely unsupported piece hanging more
// than 6.4 m below the cut that freed it -- a severed stalactite that long.
constexpr int kAnchorDropBelowSeed = 48;
// ...and this far SIDEWAYS from the changed box while at or below its top: a
// span that long, attached at the far end and no higher than the cut, is a
// hillside, not a thing that came loose. Above the cut nothing is bounded but
// the region, because that is where a crown is. The mirror holds ~1024
// chunks; a terrain walk wider than this evicts what it fetched last round.
constexpr int kAnchorReachBesideSeed = 96;
constexpr uint32_t kTerrainEvictTicks = 300;
constexpr uint32_t kTerrainRefreshTicks = 8;
// ---- WHAT ONE BODY MAY ASK THE TERRAIN SYSTEM FOR (CLAUDE.md rule 2) ------
//
// Measured 2026-09-12 on `tree-fell`'s cut pass, one 28,478-voxel oak: 220,896
// chunk visits over 301 ticks -- 734 a tick, 900 in the worst one -- 26,232
// occupancy gathers and up to 454 chunk fetch requests in a SINGLE tick
// against World::kFetchPerTick = 64. For ONE body, to build six patches.
//
// The cause was a BOUNDING SPHERE: `radiusVoxels` for a 59 x 81 x 59 crown is
// 60, needAround reached radius + 6, and 133 voxels a side is ~900 chunks of
// which the body can touch about 150. So bodies ask around their AABB (see
// needBox) with this skirt, which is one voxel more than the largest step
// LinearCast resolves plus a cell of slop.
constexpr float kTerrainSkirtVox = 4.0f;
// ...and three budgets, so no scene can make this sweep unbounded whatever the
// anchors ask for. All three are NEAREST-FIRST over the same sorted list, and
// none of them DROPS anything: a chunk that misses its slot this tick is at
// the head of the next tick's sweep, exactly the contract kTerrainBuildsPerTick
// has always had. That is what keeps a body from falling through ground it was
// about to be given -- the patch is late, never absent.
//   - the list itself, so the per-chunk staleness scan is O(1) in scene size;
//   - fetch REQUESTS, so this system can never flood a readback queue that
//     drains 64 a tick (that starves the player's own mirror, the island
//     scans' own fetches, and every other consumer, for as long as it lasts);
//   - occupancy GATHERS, since an 18^3 sample of the mirror is ~17 us and a
//     version-churn storm used to run 87 of them a tick.
constexpr uint32_t kTerrainNeedCeiling = 512;
constexpr uint32_t kTerrainFetchPerTick = 24;
constexpr uint32_t kTerrainGatherPerTick = 24;
// A mob's planning horizon is listed on one tick in this many (see
// needHorizon in ManageTerrain); its own body is listed every tick.
constexpr uint32_t kTerrainHorizonStride = 4;
// The per-body chunk grid needBody marks into: an int8 lattice rotated is at
// most ~430 world voxels a side, 28 chunks, 22k cells. Past this it falls back
// to the AABB sweep rather than allocate.
constexpr size_t kTerrainNeedGridCells = 65536;
// Real marching-cubes + Jolt rebuilds per tick (ManageTerrain). Six covers a
// chunk-boundary crossing's new face of anchor chunks in two ticks; the
// unbudgeted version did 64 in one and that was the walking hitch.
constexpr uint32_t kTerrainBuildsPerTick = 6;
// The A/B arm for that number, in ONE binary (CLAUDE.md: a differential
// measured across two builds measures the builds too). Unset = the constant;
// SANDVOX_TERRAIN_BUILDS_PER_TICK=100000 is the pre-budget behaviour.
static uint32_t TerrainBuildsPerTick() {
  static const uint32_t v = [] {
    if (const char* e = std::getenv("SANDVOX_TERRAIN_BUILDS_PER_TICK")) {
      const long n = std::strtol(e, nullptr, 10);
      if (n >= 1) return (uint32_t)n;
    }
    return kTerrainBuildsPerTick;
  }();
  return v;
}
// ---- ANTI-TUNNELLING (DebrisSystem::UntunnelBody) ---------------------------
// How far ahead along its own velocity a body asks for collision patches, and
// the ceiling on that. 0.35 s is 21 ticks at 60 Hz, which comfortably covers a
// chunk fetch plus a slot in kTerrainBuildsPerTick; the 64-voxel cap (four
// chunks) is what keeps a body at Jolt's 500 m/s from asking for a corridor
// across the whole window. The samples are the chunks the swept segment passes
// through and nothing else -- a box around each would multiply the fetch
// budget by the lookahead, and the ground a falling body needs is directly
// under it.
constexpr float kTerrainLookaheadSeconds = 0.35f;
constexpr float kTerrainLookaheadVox = 64.0f;
// Consecutive steps a body may be held at a collider boundary before it is let
// through anyway. Half a second at 60 Hz: long enough that the patch normally
// lands first, short enough that a starved readback reads as a stutter rather
// than as a body frozen in the sky.
constexpr uint8_t kUntunnelHoldTicks = 30;
// Samples along the step when looking for where a body left vouched space.
// A 60 Hz step at Jolt's velocity ceiling is 83 voxels, so this resolves every
// reachable step to better than 1.5 voxels.
constexpr int kUntunnelSamples = 64;

// Support-loss events: a chunk re-flags constantly while sand pours or fire
// burns, so rescans are rate-limited per chunk. The final flags after activity
// stops always land (pendingSupport_ is never dropped), so the cooldown only
// delays detection, never loses it.
constexpr uint32_t kSupportCooldownTicks = 45;
// Support events use a wider margin than blast events (24 -> 64^3 region):
// the flagged chunk holds the support POINT, but the structure that may float
// (a grown plant, a burnt-through pillar) extends well beyond it, and a
// component touching the region boundary is conservatively kept.
constexpr int kSupportMargin = 24;
// TRIED AND REVERTED (2026-09-04): a narrow 8-cell first tier that escalated
// to this margin when it clipped something. Near a tree everything clips a
// 32^3 box -- 627 of 714 narrow scans escalated -- so it doubled the work and
// the residue went up, not down. The per-scan cost is attacked inside the
// scan instead (seed from the changed box, stop at the first anchor).
constexpr int kSupportDrainPerTick = 8;
// ---- event queue drain rates ----------------------------------------------
//
// How many entries of the event queue are EXAMINED per tick, how many of those
// may actually be scanned, and how many may push chunk fetches while being
// examined. The probe exists because the queue used to be strictly head-only
// and a single unfetchable region stalled every event behind it (see the drain
// in PreTick). The scan count stays small on purpose: one scan is a dense
// mask over a region up to 80^3, so this is the knob that decides how much CPU
// island detection can take in a tick, and CLAUDE.md rule 2 is what bounds it.
constexpr uint32_t kEventProbePerTick = 32;
// The scan budget is in CELLS, not scans, so a tick may run two wide 64^3
// scans or sixteen narrow 32^3 ones for the same CPU: the dense mask over the
// region is what a scan costs, and counting scans would let the cheap tier
// starve behind the expensive one or vice versa.
// Cells the sparse flood may visit per tick, all scans together. Since §4 a
// scan costs what it walks rather than a 64^3 mask, so this is a HITCH bound
// (a hash-map visit is ~50-100 ns: this is about 15-25 ms in the worst tick)
// more than a throughput one -- a typical scan near a burning tree is a few
// thousand visits.
constexpr uint32_t kIslandScanCellsPerTick = 256u * 1024u;
constexpr uint32_t kEventFetchProbes = 4;
// How long an event may sit unready before it is taken out of the queue. Same
// 120 ticks the old head-only drain used, minus the second 300-tick arm, which
// only existed to distinguish "alone in the queue" — a distinction the probe
// above makes unnecessary, since a lone event no longer blocks anything.
constexpr uint32_t kEventStuckTicks = 120;
// Cooldown re-arm (see QueueSupportEvents): how many suppressed chunks may be
// promoted per tick, and how deep the rotation scan looks for ready ones.
constexpr int kSupportRearmPerTick = 4;
constexpr int kSupportRearmScan = 32;
// How many times one region may be DEFERRED and re-queued before the scan
// gives up on it. A deferral happens when a budget (grid ops, particle ring)
// runs out with components still unconverted; the event comes back next tick,
// by which time this tick's writes have landed and the remainder is smaller.
// Progress is therefore monotonic in practice and the cap is a backstop, not a
// schedule — but it MUST exist, because "re-queue until it fits" against a
// permanently saturated budget is an infinite loop, and this file's whole
// contract is that nothing here grows without bound (CLAUDE.md rule 2).
constexpr uint8_t kMaxEventRetries = 8;
// Fetch rounds an event may wait through: a chunk layer per round, 64 chunks a
// tick, so a whole tree crown lands in two or three. Separate from the budget
// retries above because waiting for the mirror is not a budget failure.
constexpr uint8_t kMaxFetchRetries = 32;
// Chunk fetches one scan may ISSUE. The world serves fetches from one FIFO at
// kFetchPerTick (64) a tick, shared with the terrain colliders every body
// needs under it, and a terrain flood at region scale could ask for hundreds
// in one scan -- the `audio-impact` slab's refresh then queued behind them and
// the block fell through it. Chunks past the cap are still marked as waited
// on (the event re-queues), just not requested until the next round.
//
// Was 32, and counted every RE-ENTRY into an unfetched chunk against itself
// (the flood zigzags across a chunk face hundreds of times), so a scan's real
// reach was a handful of chunks a round. Now the whole tick budget: a scan
// requests only for components a fetch could still change (an anchored
// terrain flood asks for none of its rock frontier), so the number of chunks
// actually put on the FIFO per tick is smaller than before at twice the cap.
constexpr uint32_t kIslandFetchPerScan = World::kFetchPerTick;
// Ceiling on the spill queue. pendingSupport_ never drops entries by design
// (a missed final flag is a floating island forever), which is exactly why it
// needs a ceiling somewhere: without one, a caller looping on destruction
// could grow it to the size of the window. At 2 chunks/tick this is already
// ~34 s of backlog, far past the point where a bigger number would help.
constexpr size_t kMaxPendingSupport = 4096;

// Body budget policy. Anything that comes loose as a coherent object earns a
// rigidbody — a felled trunk falls as a log, not as a puff of powder. The only
// gates are the size floor (below it matter is individual voxels, not an
// object) and the global kMaxBodies ceiling.
//
// The per-scan cap used to be 4, which quietly decided that the 5th-largest
// piece of a shattered tree — however big — crumbled to rubble instead. That
// is what made felled wood change material on screen. It is now the body
// ceiling itself: a scan may fill every free slot, and PostStep's oldest-first
// despawn is what keeps the population bounded.
constexpr uint32_t kMinBodyVoxels = 8;               // below this: rubble
constexpr uint32_t kMaxNewBodiesPerScan = kMaxBodies;  // only the global cap
constexpr uint32_t kMaxNewBodiesPerTick = 4;   // shatter's share, all bodies

// Fragments broken off a BURNING body face a much higher bar than a fresh
// island. A body disintegrating in a fire re-fragments every few ticks, and
// each fragment that earns a body re-fragments in turn — that recursion is
// what turned one burning tree into hundreds of tiny bodies chugging the CPU.
// Charred bits that fall off a burning object are visually just embers, so
// below this they become ballistic particles and stay in the CA.
constexpr uint32_t kMinBurnFragmentVoxels = 24;

// Body burn budgets: voxel scans across all bodies per tick, grid writes
// (emitted fire / escaping ash+smoke) per tick, and how many voxels must burn
// away before the Jolt collider is rebuilt to match the charred shape.
constexpr uint32_t kBurnScanPerTick = 4096;
// The least any scanning body is offered of that per tick, however many
// bodies are scanning: a body's share is the larger of this and an even split
// of what is left (BurnBodies, "FAIR SHARE"). 256 keeps a scale-8 finger
// (a few hundred skin voxels) covered every tick or two even in a crowd.
constexpr uint32_t kBurnScanMinShare = 256;
constexpr uint32_t kBurnOpsPerTick = 384;
constexpr uint32_t kBurnRebuildVoxels = 12;
// Connectivity re-check after burning is O(n) per body; run it only when
// enough matter has actually left to plausibly disconnect the remainder.
constexpr uint32_t kShatterCheckVoxels = 6;
// How often a body with something eatable on it asks the world whether a
// SOLVENT is sitting against it (BurnBodies' willScan, DebrisSystem::ThreatNear).
// A settled pool of acid leaves no dirty chunk, so this is the only thing that
// ever wakes a corpse lying in one. Staggered by body serial, so 8 means one
// eighth of the bodies pay a handful of cached reads on any given tick.
constexpr uint32_t kInboundProbeTicks = 8;
// Lattice-box corners plus the centre, in the units of whichever lattice is
// being probed. Nine reads is enough to find a pool a body is lying in and
// cheap enough to run on every corpse on the field.
constexpr int kThreatProbePoints = 9;

// ---- impact cue budgets (DESIGN.md §12b) ------------------------------------
// The fixed sim rate, which is also the rate PostStep runs at, so the per-body
// impact gap authored in SECONDS can be expressed in PostStep counts without
// threading a clock through eight call sites.
constexpr float kSimTicksPerSecond = 30.0f;
// At most this many impacts are VOICED per step, loudest first. See the
// ImpactEvent comment in debris.h for why the cap picks rather than truncates.
constexpr size_t kMaxImpactsPerStep = 4;

// sim/rng.h — so burn rolls replay identically for a given
// (body serial, tick, voxel, rule).
using rng::Hash3;
using rng::Pcg;

uint32_t LocalKey(int x, int y, int z) {
  return (uint32_t)(x & 0xFF) | ((uint32_t)(y & 0xFF) << 8) |
         ((uint32_t)(z & 0xFF) << 16);
}

Vec3 QuatRot(const float q[4], Vec3 v) {
  Vec3 u{q[0], q[1], q[2]};
  Vec3 t = u.cross(v) * 2.0f;
  return v + t * q[3] + u.cross(t);
}

// world CHUNK coord of a world cell (floor shift: valid for negatives)
IVec3 ChunkOfCell(int x, int y, int z) { return {x >> 4, y >> 4, z >> 4}; }

// A body's AUTHORITATIVE voxel lattice, addressed uniformly.
//
// A body has one lattice or two: `skinVoxels` (int16 PrefabVoxel, skinScale
// units) when its skin is finer than its collider, and `voxels` (int8
// DebrisVoxel, physScale units) otherwise. Burning must edit whichever one is
// authoritative — writing to the derived collider of a fine-skinned body would
// be undone by the next majority-fill re-derive, silently — and it has to
// behave identically on both, because a corpse is a fine-skinned body and a
// plank is not, and "cloth catches faster than flesh" must not be two
// implementations that happen to agree.
//
// The twin of LimbLattice in game/mob.cpp; the two populations meet at
// AdoptBody, and the same voxel must burn the same on either side of it.
struct BodyLattice {
  std::vector<PrefabVoxel>* skin = nullptr;
  std::vector<DebrisVoxel>* coll = nullptr;
  uint32_t scale = 1;  // lattice units per world voxel

  size_t Size() const { return skin ? skin->size() : coll->size(); }
  IVec3 At(size_t i) const {
    return skin ? IVec3{(*skin)[i].x, (*skin)[i].y, (*skin)[i].z}
                : IVec3{(*coll)[i].x, (*coll)[i].y, (*coll)[i].z};
  }
  uint32_t Mat(size_t i) const {
    return skin ? (uint32_t)((*skin)[i].material & 0xFFFu)
                : (uint32_t)((*coll)[i].payload & 0xFFFu);
  }
  // Merged art-palette index, 0 = unpainted. The one thing a body voxel knows
  // about its colour, and the input to the grid-tint quantization when its
  // matter escapes into the world (DebrisSystem::GridStateFor).
  uint32_t Color(size_t i) const {
    return skin ? (uint32_t)(*skin)[i].color : (uint32_t)(*coll)[i].color;
  }
  // Rewrite a voxel's material, keeping a cosmetic variant and ZEROING the art
  // slot — microbody.wgsl lets a nonzero art colour override the material
  // colour, so a charred voxel that kept its slot goes on being painted
  // robe-purple and the charring is invisible on exactly the painted surfaces
  // it matters most on.
  void Set(size_t i, uint32_t mat, uint32_t variant) const {
    const uint16_t w = (uint16_t)((mat & 0xFFFu) | ((variant & 3u) << 12));
    if (skin) {
      (*skin)[i].material = w;
      (*skin)[i].color = 0;
    } else {
      (*coll)[i].payload = w;
      (*coll)[i].color = 0;
    }
  }
};
// slot linear cell index (what cellOps / sim_mutate `cells` consume)
uint32_t CellIndexOf(int x, int y, int z) {
  return World::SlotCellIndex({x, y, z});
}

int FindMaterialId(const std::vector<MaterialDef>& mats, const std::string& name) {
  for (size_t i = 0; i < mats.size(); i++)
    if (mats[i].name == name) return (int)i;
  return -1;
}

// Index of the entry in `tints` closest to `rgb`, both packed 0x00RRGGBB.
//
// Plain squared distance in integer RGB. Not a perceptual space, deliberately:
// this runs once at load over at most 255 x 16 pairs, the inputs are an
// author's dye list and an author's palette, and a Lab conversion would put
// floats and a colour-science dependency in the path of a decision that only
// has to pick the obvious neighbour. int32 is enough — the worst case is
// 3 * 255^2 = 195075.
//
// Ties go to the LOWEST index, which is what makes tint 0 (the natural colour,
// by the kMatFlagTinted convention) the answer when nothing is closer.
uint32_t NearestTint(const std::vector<uint32_t>& tints, uint32_t rgb) {
  const int32_t r = (int32_t)((rgb >> 16) & 0xFF), g = (int32_t)((rgb >> 8) & 0xFF),
                b = (int32_t)(rgb & 0xFF);
  uint32_t best = 0;
  int32_t bestD = INT32_MAX;
  for (size_t i = 0; i < tints.size() && i < kMatTintsMax; i++) {
    const int32_t dr = r - (int32_t)((tints[i] >> 16) & 0xFF);
    const int32_t dg = g - (int32_t)((tints[i] >> 8) & 0xFF);
    const int32_t db = b - (int32_t)(tints[i] & 0xFF);
    const int32_t d = dr * dr + dg * dg + db * db;
    if (d < bestD) {
      bestD = d;
      best = (uint32_t)i;
    }
  }
  return best;
}

}  // namespace

void DebrisSystem::Init(Physics* phys, World* world, const std::vector<MaterialDef>& mats,
                        const std::vector<ReactionGpu>& reactions) {
  phys_ = phys;
  world_ = world;
  // SANDVOX_DEBRIS_PROFILE=1 turns the per-phase clock on for a live session,
  // so the Performance tab's `debrisSys` bar can be split without a rebuild.
  if (const char* e = std::getenv("SANDVOX_DEBRIS_PROFILE")) {
    prof_.on = e[0] != '0';
    prof_.autoReport = prof_.on;
  }
  OnMaterialsReloaded(mats, reactions);
}

void DebrisSystem::OnMaterialsReloaded(const std::vector<MaterialDef>& mats,
                                       const std::vector<ReactionGpu>& reactions) {
  classOf_.clear();
  densityOf_.clear();
  rubbleOf_.clear();
  foliageOf_.clear();
  matTints_.clear();
  tintMapValid_ = false;  // tint lists just moved: the art map derived from them
  matGpu_.clear();
  matSelfActive_.clear();
  matSelfScaled_.clear();
  matHasPair_.clear();
  matHasScaled_.clear();
  matRewritesNbr_.clear();
  matInboundTarget_.clear();
  reactions_ = reactions;
  uint32_t selfIdx = 0;
  matNames_.clear();
  for (const auto& m : mats) {
    classOf_.push_back(m.gpu.klass);
    densityOf_.push_back((float)m.gpu.density);
    matGpu_.push_back(m.gpu);
    // Name -> id, for the one thing on this side that is authored BY NAME: the
    // bruise coat (gore.bruiseMat). MobSystem has its own lookup for the same
    // string; this is the loose-matter half of it.
    matNames_.push_back(m.name);
    matTints_.push_back(m.tints);
    // which rule shapes this material owns (drives the burn-pass gates).
    //
    // A self rule behind a neighbour-count ramp is NOT "self-driven": it
    // cannot fire until something hot sits next to the voxel, and until then
    // it is exactly as inert as a pair rule with no partner. flesh_charred
    // and flesh_cooked own only such rules (relight under a four-face front,
    // catch under a three-face one), and counting them as active made every
    // charred corpse a body BurnBodies scanned to its budget every tick for
    // the rest of the session -- the light-gated-rules-never-sleep trap
    // (CLAUDE.md rule 2) wearing a different condition. Gated self rules go
    // in their own column and wake a body only with fire nearby.
    uint8_t selfActive = 0, selfScaled = 0, hasPair = 0, rewrites = 0;
    for (uint32_t ri = 0; ri < m.gpu.reactCount; ri++) {
      const ReactionGpu& r = reactions_[m.gpu.reactOffset + ri];
      uint32_t kind = r.packed & 3u;
      if (kind == kReactDecay || kind == kReactEmit) {
        if (ReactScaleArmed(r)) selfScaled = 1;
        else selfActive = 1;
      }
      if (kind == kReactPair) {
        hasPair = 1;
        // ...and can it act on whatever is next to it? That is the rule shape
        // the inbound pass runs, and it is the only shape that matters when
        // the neighbour is a BODY: a rule that keeps its neighbour has nothing
        // to say to one.
        if (r.prodNbr != kProdKeep) rewrites = 1;
      }
    }
    matSelfActive_.push_back(selfActive);
    matSelfScaled_.push_back(selfScaled);
    matHasPair_.push_back(hasPair);
    matRewritesNbr_.push_back(rewrites);
    // Does ANY of this material's rules use the neighbour-count ramp? The burn
    // pass needs the body-local occupancy map to count neighbours, and an
    // inactive body normally skips building it — so without this flag a scaled
    // rule on an inactive body would count every direction as "world", which is
    // the divergence in the opposite direction from the one reactcpu.h fixes.
    uint8_t hasScaled = 0;
    for (uint32_t ri = 0; ri < m.gpu.reactCount; ri++)
      if (ReactScaleArmed(reactions_[m.gpu.reactOffset + ri])) hasScaled = 1;
    matHasScaled_.push_back(hasScaled);
    // Rubble = what a voxel becomes when it crumbles to loose matter. An
    // undeclared rubble form defaults to the material ITSELF: a scrap of wood
    // is still wood, and transmuting it (the old organic->dust / ->gravel
    // guess) is why a felled tree's leftovers came out yellow-tan instead of
    // brown. Materials that genuinely pulverize say so in JSON.
    int r = m.rubble.empty() ? -1 : FindMaterialId(mats, m.rubble);
    if (r < 0) r = (int)selfIdx;
    rubbleOf_.push_back((uint32_t)r);
    selfIdx++;
    uint8_t foliage = 0;
    for (const auto& t : m.tags)
      if (t == "foliage") foliage = 1;
    foliageOf_.push_back(foliage);
  }
  // ---- WHICH MATERIALS THE GRID CAN EAT ------------------------------------
  // A second pass, because the predicate test needs the whole material table
  // and the loop above is still building it. Every rewrite rule is asked, once,
  // which of the materials it would match — so a body can later answer "is
  // there anything on me the world could act on?" with one array read per
  // voxel instead of a rule walk. Nothing is hardcoded: adding a new solvent to
  // reactions.json extends this table on the next reload, exactly as it
  // extends the GPU's.
  matInboundTarget_.assign(matGpu_.size(), 0);
  for (size_t sm = 1; sm < matGpu_.size(); sm++) {
    if (!matRewritesNbr_[sm]) continue;
    const MaterialGpu& g = matGpu_[sm];
    for (uint32_t ri = 0; ri < g.reactCount; ri++) {
      const ReactionGpu& r = reactions_[g.reactOffset + ri];
      if ((r.packed & 3u) != kReactPair || r.prodNbr == kProdKeep) continue;
      for (size_t tm = 1; tm < matGpu_.size(); tm++)
        if (ReactNbrMatches(r, (uint32_t)tm, matGpu_)) matInboundTarget_[tm] = 1;
    }
  }
  for (Body& b : bodies_) RecountBurn(b);  // hot-reload can change rule sets
}

void DebrisSystem::RefreshTintMap() {
  // FNV-1a over the merged palette. 255 words at most, once a tick, against a
  // rebuild that only happens when it actually changed — see the header for why
  // this is a content stamp and not a length.
  uint64_t stamp = 1469598103934665603ull;
  const size_t art = microSet_ ? microSet_->artColors.size() : 0;
  for (size_t i = 0; i < art; i++)
    stamp = (stamp ^ microSet_->artColors[i]) * 1099511628211ull;
  stamp = (stamp ^ (uint64_t)art) * 1099511628211ull;
  if (tintMapValid_ && stamp == tintArtStamp_) return;
  tintArtStamp_ = stamp;
  tintMapValid_ = true;

  tintOfArt_.assign(matTints_.size(), {});
  for (size_t m = 0; m < matTints_.size(); m++) {
    if (matTints_[m].empty()) continue;  // not MATF_TINTED: no table, no cost
    std::vector<uint8_t>& tbl = tintOfArt_[m];
    // +1 for the unpainted slot at 0. It stays 0 = the natural colour: an
    // unpainted voxel has no colour to quantize, and running it through
    // NearestTint against black would dye every plain limb with whichever tint
    // happened to be darkest.
    tbl.assign(art + 1, 0);
    for (size_t a = 0; a < art; a++)
      tbl[a + 1] = (uint8_t)NearestTint(matTints_[m], microSet_->artColors[a]);
  }
}

uint32_t DebrisSystem::GridStateFor(uint32_t mat, uint32_t art,
                                    uint32_t fallback) const {
  if (mat >= tintOfArt_.size()) return fallback;
  const std::vector<uint8_t>& tbl = tintOfArt_[mat];
  if (tbl.empty()) return fallback;  // untinted: the caller's jitter variant
  // Past the end means the palette shrank under a body that still holds an old
  // index — a hot reload between a limb being severed and it landing. Tint 0
  // rather than a wrap: an undyed corpse is a miss, a randomly dyed one is a bug
  // report.
  return art < tbl.size() ? (uint32_t)tbl[art] : 0u;
}

void DebrisSystem::RecountBurn(Body& b) const {
  b.activeCount = 0;
  b.pairCount = 0;
  b.scaledCount = 0;
  b.inboundCount = 0;
  b.internalPair = false;
  // WHICH MATERIALS ARE ON THIS BODY, for the internal-pair test below. Reused
  // across calls and cleared by the same walk that fills it, so a recount stays
  // one pass over the lattice plus one over the materials it actually found.
  if (presentScratch_.size() < matGpu_.size())
    presentScratch_.assign(matGpu_.size(), 0);
  std::vector<uint32_t>& found = foundScratch_;
  found.clear();
  // Counted on the AUTHORITATIVE lattice. On a fine-skinned body `voxels` is a
  // majority-filled derivation, so a handful of burning skin voxels can vanish
  // from it entirely — and a body whose activeCount reads 0 is a body this pass
  // skips, i.e. a corpse that stops burning for no visible reason.
  auto tally = [&](uint32_t m) {
    if (m == 0 || m >= matGpu_.size()) return;
    if (matSelfActive_[m]) b.activeCount++;
    if (matSelfScaled_[m]) b.scaledCount++;
    if (matHasPair_[m]) b.pairCount++;
    if (m < matInboundTarget_.size() && matInboundTarget_[m]) b.inboundCount++;
    if (!presentScratch_[m]) {
      presentScratch_[m] = 1;
      found.push_back(m);
    }
  };
  if (b.HasFineSkin()) {
    for (const PrefabVoxel& v : b.skinVoxels) tally(v.material & 0xFFFu);
  } else {
    for (const DebrisVoxel& v : b.voxels) tally(v.payload & 0xFFFu);
  }
  // ---- CAN IT REACT WITH ITSELF? (Body::internalPair) ----------------------
  // Every material present, against every pair rule every material present
  // authors. Bounded by the number of DISTINCT materials on one body — four or
  // five on a limb, a dozen on a dressed corpse — not by its voxel count, so a
  // 14k-voxel torso pays the same as a hand.
  for (uint32_t sm : found) {
    if (b.internalPair) break;
    if (!matHasPair_[sm]) continue;
    const MaterialGpu& g = matGpu_[sm];
    for (uint32_t ri = 0; ri < g.reactCount && !b.internalPair; ri++) {
      const ReactionGpu& r = reactions_[g.reactOffset + ri];
      if ((r.packed & 3u) != kReactPair) continue;
      for (uint32_t tm : found)
        if (ReactNbrMatches(r, tm, matGpu_)) { b.internalPair = true; break; }
    }
  }
  for (uint32_t sm : found) presentScratch_[sm] = 0;
  // Cached identity for the impact cue, refreshed here because this is already
  // the one function every site that rewrites a body's voxel lattice calls —
  // adoption, split, shatter fragment, damage, materials hot-reload. A body
  // that burns down to mostly ash starts sounding like ash, which is right.
  b.domMat = DominantMaterial(b.voxels);
}

void DebrisSystem::Reset() {
  for (Body& b : bodies_) ReleaseBody(b);
  bodies_.clear();
  for (auto& [ci, t] : terrain_)
    if (t.handle) phys_->RemoveBody(t.handle);
  terrain_.clear();
  events_.clear();
  gore_.clear();
  pendingSupport_.clear();
  supportPending_.clear();
  supportCooldown_.clear();
  supportLateQueue_.clear();
  supportLate_.clear();
  // Undrained breaks belong to the world that just went away; voicing them
  // after a regen or a LoadWorld would fire a burst of snaps at coordinates
  // that now mean something else entirely.
  breaks_.clear();
  // Same reasoning for undrained impacts: the bodies that made them are gone.
  impacts_.clear();
  // Every patch above was just removed, so every handle still being held at a
  // boundary is being held against a chunk that no longer has an entry.
  untunnelHold_.clear();
  instancesDirty_ = true;
  instanceCount_ = 0;
  // Body serials seed the burn RNG (Hash3(serial, tick, rule)), so a counter
  // that survives Reset() makes every body's burn sequence depend on how many
  // bodies happened to exist before it. That silently couples unrelated
  // scenarios: a worldgen tweak that changes how many islands an earlier
  // destruction produces re-rolls a later fire's outcome entirely. Resetting
  // here makes a body's burn a function of the scenario, not of history.
  nextSerial_ = 1;
  // The ghost bookkeeping goes with the bodies it described. NOT the ownership
  // FUNCTIONS: those are wiring the session set once at join, and a world regen
  // does not disconnect anybody. Same rule Reset() already applies to
  // densityOf_ and the material tables.
  itemTakeOut_.clear();
  itemGrantIn_.clear();
  ownerProbe_ = OwnerProbe{};
  nextAssembly_ = 1;
  chunkWriteTick_.clear();
  pendingVacate_.clear();
  BeginTickLabels();
  labelPages_.clear();
}

// Every world chunk the SEED box covers, onto the queue that never
// drops. Deduped through supportPending_ exactly as a GPU support flag is, so
// spilling the same region twice costs one entry — and deliberately WITHOUT
// touching supportCooldown_: the cooldown throttles the CA's repeating flags
// (sand pouring, fire burning), and borrowing it here would let a spilled
// explosion suppress a genuine flag from the same chunk moments later.
void DebrisSystem::SpillRegionToSupport(const Event& e) {
  for (int cz = e.seedLo.z >> 4; cz <= (e.seedHi.z >> 4); cz++)
    for (int cy = e.seedLo.y >> 4; cy <= (e.seedHi.y >> 4); cy++)
      for (int cx = e.seedLo.x >> 4; cx <= (e.seedHi.x >> 4); cx++) {
        IVec3 wc{cx, cy, cz};
        if (!world_->ChunkInWindow(wc)) continue;
        if (pendingSupport_.size() >= kMaxPendingSupport) {
          floaters_.eventQueueFullDropped++;
          return;
        }
        uint64_t key = World::PackChunkKey(wc);
        if (supportPending_.count(key)) continue;
        supportPending_[key] = 1;
        pendingSupport_.push_back(wc);
        floaters_.eventQueueFullSpilled++;
      }
}

bool DebrisSystem::AddDestructionEvent(uint32_t tick, IVec3 lo, IVec3 hi,
                                       int margin, bool spillOnFull) {
  Event e;
  e.tick = tick;
  // One cell of slack around what changed: whatever was resting on an erased
  // cell is 6-adjacent to it, so it is inside this box.
  e.seedLo = {lo.x - 1, lo.y - 1, lo.z - 1};
  e.seedHi = {hi.x + 1, hi.y + 1, hi.z + 1};
  // expand for support context, clamp to the bounded region + world
  e.lo = {lo.x - margin, lo.y - margin, lo.z - margin};
  e.hi = {hi.x + margin, hi.y + margin, hi.z + margin};
  IVec3 c{(e.lo.x + e.hi.x) / 2, (e.lo.y + e.hi.y) / 2, (e.lo.z + e.hi.z) / 2};
  // ALWAYS the full region, not "at most": the flood is sparse, so a box
  // costs nothing until something the flood reaches touches its face, and a
  // face 128 cells from the change is what lets a tree be judged whole instead
  // of being anchored at a wall 40 cells above the cut. `margin` still sizes
  // nothing here; the seed box above is what bounds the work.
  auto clampAxis = [&](int& lo, int& hi, int center) {
    if (hi - lo + 1 != kMaxRegionCells) {
      lo = center - kMaxRegionCells / 2;
      hi = lo + kMaxRegionCells - 1;
    }
  };
  clampAxis(e.lo.x, e.hi.x, c.x);
  clampAxis(e.lo.y, e.hi.y, c.y);
  clampAxis(e.lo.z, e.hi.z, c.z);
  // clamp to the residency window (world coords)
  IVec3 wlo = world_->WindowOrigin();
  wlo = {wlo.x * (int)kChunk, wlo.y * (int)kChunk, wlo.z * (int)kChunk};
  IVec3 whi{wlo.x + (int)kWorldN - 1, wlo.y + (int)kWorldN - 1, wlo.z + (int)kWorldN - 1};
  e.lo.x = std::max(e.lo.x, wlo.x); e.lo.y = std::max(e.lo.y, wlo.y); e.lo.z = std::max(e.lo.z, wlo.z);
  e.hi.x = std::min(e.hi.x, whi.x); e.hi.y = std::min(e.hi.y, whi.y); e.hi.z = std::min(e.hi.z, whi.z);
  if (e.lo.x > e.hi.x || e.lo.y > e.hi.y || e.lo.z > e.hi.z) return true;  // degenerate: done
  if (events_.size() >= 64) {
    // THE QUEUE IS FULL, BUT THE EVENT IS NOT LOST. Every caller of this
    // function — the brush, the grenade, the laser — ignored the false it used
    // to return here, so a busy moment (one grenade into a burning forest)
    // silently threw away the island check for a hole it had just made, and
    // whatever that hole was holding up floated for the rest of the session.
    //
    // Spilling to pendingSupport_ makes the failure a DELAY instead of a loss:
    // that queue is drained two chunks a tick and never drops, so the scan
    // still happens, just later. The return value stays for the one caller
    // that genuinely wants to retry in place (the drain itself, which passes
    // spillOnFull = false so it cannot re-enqueue what it is draining).
    // NOT counted as a drop when spillOnFull is false: that caller is the
    // pendingSupport_ drain, which leaves the chunk queued and comes back next
    // tick. Counting its back-pressure as lost matter put 114 phantom entries
    // in the leak column on the first full-suite run and made a healthy queue
    // look like a haemorrhage — the exact failure mode this probe exists to
    // prevent, committed by the probe itself.
    if (spillOnFull) SpillRegionToSupport(e);
    else floaters_.drainBackpressure++;
    return false;
  }
  events_.push_back(e);
  return true;
}

void DebrisSystem::QueueSupportEvents(const WorldSnapshot& snap) {
  if (!snap.valid || snap.tick == lastSupportSnapTick_) return;  // one pass per snapshot
  sandvox::PerfSpan span(sandvox::PerfScope::Debris, sandvox::PerfScope::GameLogic);
  lastSupportSnapTick_ = snap.tick;
  int m = (int)kNChunk - 1;
  for (uint32_t ci = 0; ci < (uint32_t)snap.supportFlags.size(); ci++) {
    if (!snap.supportFlags[ci]) continue;
    // slot -> world chunk under the origin the snapshot was captured at
    IVec3 s{(int)(ci % kNChunk), (int)((ci / kNChunk) % kNChunk),
            (int)(ci / (kNChunk * kNChunk))};
    IVec3 o = snap.windowOrigin;
    IVec3 wc{o.x + ((s.x - o.x) & m), o.y + ((s.y - o.y) & m),
             o.z + ((s.z - o.z) & m)};
    // NOT MY CHUNK, NOT MY SCAN (M9.4-C). A support-loss flag is a GPU
    // observation about a region of grid, and both machines see it -- the CA
    // ran on both. If both also queued the scan, both would flood the same
    // component, both would emit the same erase ops and both would create the
    // body: one island, two rigid bodies, in the same space, on one machine.
    // The chunk's authority is the one that looks. Null fn = every chunk is
    // mine = today's behaviour exactly.
    if (!ChunkOwned(wc)) {
      ownerProbe_.chunksSkipped++;
      continue;
    }
    uint64_t key = World::PackChunkKey(wc);
    if (supportPending_.count(key)) continue;
    auto it = supportCooldown_.find(key);
    if (it != supportCooldown_.end() &&
        snap.tick < it->second + kSupportCooldownTicks) {
      // THE COOLDOWN WAS A DROP, NOT A DELAY, and that is the difference
      // between "detection is rate-limited" and "the last change is never
      // looked at". The comment on kSupportCooldownTicks claims the final flags
      // after activity stops always land because pendingSupport_ never drops
      // entries — but a flag suppressed HERE never reaches that queue at all,
      // and when the CA goes quiet nothing re-raises it. So the very last state
      // of a burnt tree, the one the player is standing in front of, was the
      // one state no scan ever saw.
      //
      // Measured, and it took making the queue fast to expose: while island
      // detection was head-of-line blocked the backlog ran LATE and happened to
      // scan the settled world, so the bug was masked by a second bug. Draining
      // the queue promptly dropped the residue from 6 floaters to 38.
      //
      // Re-armed instead: the chunk is remembered and queued the moment its
      // cooldown expires, which is what "only delays detection" was always
      // supposed to mean. Deduped per chunk, so a chunk flagged four hundred
      // times while it burns costs one entry.
      if (!supportLate_.count(key) &&
          supportLateQueue_.size() < kMaxPendingSupport) {
        supportLate_[key] = 1;
        supportLateQueue_.push_back(wc);
        floaters_.supportLateHeld++;
      }
      continue;
    }
    supportCooldown_[key] = snap.tick;
    supportPending_[key] = 1;
    pendingSupport_.push_back(wc);
  }
}

// Chunks whose support flag was suppressed by the cooldown, promoted once it
// has expired. Bounded work per tick and bounded queue; see the long note in
// QueueSupportEvents for why dropping them was the bug.
void DebrisSystem::RearmLateSupport(uint32_t tick) {
  // ROTATED, not popped-until-blocked. Entries enter in SUPPRESSION order but
  // each chunk's cooldown started whenever that chunk was last accepted, so the
  // front is not necessarily the first to come ready and a `break` on the front
  // would let one chunk hold the rest back — the same head-of-line shape as the
  // event queue above, and there is no reason to rebuild it here. A bounded
  // number of entries is examined; the ones not yet ready go to the back.
  int promoted = 0;
  size_t examine = supportLateQueue_.size();
  if (examine > (size_t)kSupportRearmScan) examine = (size_t)kSupportRearmScan;
  for (size_t i = 0; i < examine && promoted < kSupportRearmPerTick; i++) {
    const IVec3 wc = supportLateQueue_.front();
    supportLateQueue_.pop_front();
    const uint64_t key = World::PackChunkKey(wc);
    auto it = supportCooldown_.find(key);
    if (it != supportCooldown_.end() &&
        tick < it->second + kSupportCooldownTicks) {
      supportLateQueue_.push_back(wc);  // not yet: come back to it
      continue;
    }
    supportLate_.erase(key);
    if (!world_->ChunkInWindow(wc)) continue;  // streamed out: nothing to check
    if (supportPending_.count(key)) continue;  // already queued the normal way
    supportCooldown_[key] = tick;
    supportPending_[key] = 1;
    pendingSupport_.push_back(wc);
    floaters_.supportLateRearmed++;
    promoted++;
  }
}

// The mirror version a chunk must carry before a scan may read it: the event's
// own tick, or the tick of the last write the debris system itself made into
// that chunk (island removal, settle-back), whichever is later. Per chunk
// rather than one global watermark: a body made on the far side of the world
// used to make EVERY cached chunk stale for EVERY event (`lastCellWriteTick_`),
// which under a fire re-fetched whole regions for nothing.
uint32_t DebrisSystem::RequiredVersion(IVec3 wc, uint32_t eventTick) const {
  // Only the event's own tick. This system's OWN writes are not a freshness
  // requirement any more: they are read through the overlay (pendingVacate_),
  // so a chunk a fire rewrites every tick is usable from any copy the event
  // may see. Requiring the write tick here made every such chunk permanently
  // stale for every event -- 70 M stale waits and 253 abandoned scans over
  // one burning oak.
  (void)wc;
  return eventTick;
}

// Readiness is now the SEED BOX ONLY (the changed cells plus one ring of
// chunks), not the whole region. The region is 256 cells a side since §4 of
// PLAN_rigidbody_islands.md — 4096 chunks, of which a tree touches ~60 — and
// gating on all of it would block every scan for a minute. The flood fetches
// what it actually reaches, on demand, and defers itself while it waits (see
// RunIslandDetection's `needFetch`).
bool DebrisSystem::EventReady(const Event& e, World& world, bool requestFetch) const {
  bool ready = true;
  const int cx0 = std::max(e.lo.x, e.seedLo.x - 1) >> 4;
  // A fetch-deferred event waits for the chunk its flood stopped at (Event::
  // waiting). A chunk that streamed out meanwhile is not waited for: the
  // re-scan reads it as UNKNOWN and anchors, which is the right verdict for
  // matter that continues into the unloaded world.
  // SANDVOX_NO_FETCH_WAIT=1: the pre-2026-09-12 behaviour (re-run at once),
  // as an A/B arm in one binary.
  static const bool noWait = std::getenv("SANDVOX_NO_FETCH_WAIT") != nullptr;
  if (!noWait && e.waiting && world.ChunkInWindow(e.waitChunk)) {
    const CachedChunk* cc = world.Cached(e.waitChunk);
    if (!cc || cc->voxels.size() != kChunkVol) {
      if (requestFetch) world.RequestChunkFetch(e.waitChunk, World::FetchSource::IslandScan);
      return false;
    }
  }
  const int cx1 = std::min(e.hi.x, e.seedHi.x + 1) >> 4;
  const int cy0 = std::max(e.lo.y, e.seedLo.y - 1) >> 4;
  const int cy1 = std::min(e.hi.y, e.seedHi.y + 1) >> 4;
  const int cz0 = std::max(e.lo.z, e.seedLo.z - 1) >> 4;
  const int cz1 = std::min(e.hi.z, e.seedHi.z + 1) >> 4;
  for (int cz = cz0; cz <= cz1; cz++)
    for (int cy = cy0; cy <= cy1; cy++)
      for (int cx = cx0; cx <= cx1; cx++) {
        IVec3 wc{cx, cy, cz};
        if (!world.ChunkInWindow(wc)) continue;  // streamed out: skip
        const CachedChunk* cc = world.Cached(wc);
        if (!cc || cc->version < RequiredVersion(wc, e.tick)) {
          if (requestFetch) world.RequestChunkFetch(wc, World::FetchSource::IslandScan);
          ready = false;
        }
      }
  return ready;
}

namespace {

// A world cell packed for the flood's visited map. 21 bits per axis, offset so
// negative world coordinates (the window walks anywhere in the 20 km map) pack
// without collision.
inline uint64_t PackCell(int x, int y, int z) {
  const uint64_t ox = (uint64_t)(uint32_t)(x + (1 << 20)) & 0x1FFFFFu;
  const uint64_t oy = (uint64_t)(uint32_t)(y + (1 << 20)) & 0x1FFFFFu;
  const uint64_t oz = (uint64_t)(uint32_t)(z + (1 << 20)) & 0x1FFFFFu;
  return (ox << 42) | (oy << 21) | oz;
}

}  // namespace

// ---- THE SCAN, SPARSE (PLAN_rigidbody_islands.md §4) -----------------------
//
// This used to build three dense masks over the whole region (5 bytes + a
// 4-byte label per cell, 4.7 MB at 80^3) and could therefore never be asked
// about anything taller than 80 cells: a tree crown poked out of the box on
// every scan and was anchored at the boundary every time. Now the region is
// 256 a side and the scan touches ONLY the cells the flood walks: a visited
// map keyed by world cell, a component as a list of world cells. Memory and
// time are proportional to the component, which is the thing that is actually
// bounded (kMaxIslandVoxels), not to a box that is 99.5% air around a tree.
//
// Chunks are read from the mirror as the flood reaches them. One that is not
// cached, or whose copy predates the event, is REQUESTED and the cells behind
// it are skipped; every component that touched such a cell is incomplete and
// is neither converted nor trusted this tick, and the event re-queues itself
// (`needFetch`) to run again once the fetch lands. A tree's crown is reached
// in two or three such rounds. Components that touched nothing unknown are
// judged and converted immediately — the stump below a cut never holds the
// tree above it hostage to a fetch.
DebrisSystem::LabelPageData* DebrisSystem::LabelPage(IVec3 wc) {
  const uint64_t key = World::PackChunkKey(wc);
  auto it = labelPageOf_.find(key);
  if (it != labelPageOf_.end()) return labelPages_[it->second].get();
  if (labelPagesUsed_ >= kMaxLabelPages) return nullptr;
  if (labelPagesUsed_ == labelPages_.size())
    labelPages_.push_back(std::make_unique<LabelPageData>());
  labelPages_[labelPagesUsed_]->label.fill(-1);  // `index` is written before it is read
  labelPageOf_[key] = labelPagesUsed_;
  return labelPages_[labelPagesUsed_++].get();
}

void DebrisSystem::BeginTickLabels() {
  labelPageOf_.clear();
  labelPagesUsed_ = 0;
  tickComps_.clear();
}

void DebrisSystem::RunIslandDetection(const Event& e, uint32_t tick, World& world,
                                      std::vector<CellOp>& cellOps,
                                      std::vector<ParticleSpawn>& spawns) {
  // ---- cell access through the mirror, one chunk lookup cached ------------
  enum : int { CELL_AIR = 0, CELL_SOLID = 1, CELL_POWDER = 2, CELL_UNKNOWN = 3 };
  struct ChunkRef {
    IVec3 wc{INT32_MIN, INT32_MIN, INT32_MIN};
    const CachedChunk* cc = nullptr;
    bool usable = false;
    const std::unordered_map<uint16_t, uint32_t>* overlay = nullptr;
    // The chunk's page of this tick's label map, looked up on first use only
    // (a chunk read for a boundary probe never needs one). nullptr after a
    // lookup means the page cap is spent -- see kMaxLabelPages.
    LabelPageData* labels = nullptr;
    bool labelTried = false;
  };
  ChunkRef last;
  bool needFetch = false;
  IVec3 firstWait{};  // the first chunk this scan met that the mirror lacks
  // Chunks the flood met and could not read, WITH the component that met
  // them. NOTHING IS REQUESTED DURING THE FLOOD. A component that ends up
  // anchored is anchored whatever those chunks hold (the verdict is monotone),
  // and requesting its frontier anyway -- three chunks of never-fetched rock
  // under every terrain flood -- was fetch bandwidth the tree's crown then
  // queued behind. The requests go out after the verdicts, for the components
  // a fetch could still change, and the same pass speculates around them.
  struct Want {
    IVec3 wc;
    int32_t comp;  // -1: met while seeding
  };
  std::vector<Want> wanted;
  std::unordered_map<uint64_t, uint8_t> wantedSet;
  int32_t curComp = -1;
  auto chunkOf = [&](int x, int y, int z) -> ChunkRef& {
    const IVec3 wc{x >> 4, y >> 4, z >> 4};
    if (wc.x == last.wc.x && wc.y == last.wc.y && wc.z == last.wc.z) return last;
    last.wc = wc;
    last.cc = world.Cached(wc);
    // ANY cached copy will do out here. Freshness matters where the change
    // is -- the seed box, which EventReady holds to the event's tick -- and
    // for this system's own writes, which the overlay carries. Elsewhere a
    // copy a few ticks old is matter the CA has since burned (read as still
    // there: conservative) or powder that has since settled (read as absent:
    // a slab freed a tick early, and it lands). Holding far chunks to the
    // event's tick made every scan near a fire wait on a refresh of the whole
    // tree, re-flood the crown while it waited, and give up after 32 rounds:
    // 380 M cells visited and 244 abandoned scans over one burning oak.
    last.usable = last.cc && last.cc->voxels.size() == kChunkVol;
    last.overlay = nullptr;
    last.labels = nullptr;
    last.labelTried = false;
    if (last.usable) {
      auto ov = pendingVacate_.find(World::PackChunkKey(wc));
      if (ov != pendingVacate_.end()) {
        if (last.cc->version >= ov->second.tick) pendingVacate_.erase(ov);  // caught up
        else last.overlay = &ov->second.cells;
      }
    }
    if (!last.usable && world.ChunkInWindow(wc)) {
      const uint64_t key = World::PackChunkKey(wc);
      if (!wantedSet.count(key)) {  // once per chunk, not once per re-entry
        wantedSet[key] = 1;
        wanted.push_back({wc, curComp});
        if (!needFetch) firstWait = wc;
        needFetch = true;
        if (!last.cc) floaters_.fetchWaitNoCache++;
        else floaters_.fetchWaitNoVoxels++;
      }
    }
    return last;
  };
  auto labelsOf = [&](ChunkRef& cr) -> LabelPageData* {
    if (!cr.labelTried) {
      cr.labelTried = true;
      cr.labels = LabelPage(cr.wc);
    }
    return cr.labels;
  };
  auto localIdx = [](int x, int y, int z) -> uint32_t {
    return ((uint32_t)(z & 15) * kChunk + (uint32_t)(y & 15)) * kChunk +
           (uint32_t)(x & 15);
  };
  // SANDVOX_ISLAND_WATCH=x,y,z (trace only): one world cell whose every read,
  // label and conversion this scan reports, for a floater the sweep names
  // and the counters cannot explain. `overlayHid` / `overlayShow` count seed
  // cells whose CLASS the pendingVacate_ overlay changed, whichever cell they
  // are: the overlay is the one thing that can make a scan disagree with the
  // mirror the sweep reads.
  static const IVec3 watch = [] {
    IVec3 w{INT32_MIN, INT32_MIN, INT32_MIN};
    if (const char* v = std::getenv("SANDVOX_ISLAND_WATCH"))
      std::sscanf(v, "%d,%d,%d", &w.x, &w.y, &w.z);
    return w;
  }();
  uint32_t overlayHid = 0, overlayShow = 0;
  bool watchSeen = false;
  // SANDVOX_ISLAND_WATCH_Y=<y> (trace only): every solid seed cell read at
  // that height, with the fate of its component, so a floater the sweep
  // names at the END of a run can be looked up afterwards.
  static const int watchY = [] {
    const char* v = std::getenv("SANDVOX_ISLAND_WATCH_Y");
    return v ? std::atoi(v) : INT32_MIN;
  }();
  struct BandSeed {
    IVec3 c;
    uint32_t word;
    int32_t label;  // -1 fresh, else the label found (< labelBase: inherited)
    int32_t comp;   // this scan's component, or -1
  };
  std::vector<BandSeed> band;
  auto classOfWord = [&](uint32_t w) -> int {
    const uint32_t mat = w & 0xFFFu;
    if (mat == 0 || mat >= classOf_.size()) return CELL_AIR;
    if (classOf_[mat] == CLASS_SOLID) return CELL_SOLID;
    if (classOf_[mat] == CLASS_POWDER) return CELL_POWDER;
    return CELL_AIR;
  };
  auto wordAt = [&](int x, int y, int z, uint32_t& w) -> int {
    if (!world.CellInWindow({x, y, z})) return CELL_UNKNOWN;
    const ChunkRef& cr = chunkOf(x, y, z);
    if (!cr.usable) return CELL_UNKNOWN;
    const uint32_t li = localIdx(x, y, z);
    w = cr.cc->voxels[li];
    if (cr.overlay) {
      auto ov = cr.overlay->find((uint16_t)li);
      if (ov != cr.overlay->end()) w = ov->second;  // our own write, not yet mirrored
    }
    if (x == watch.x && y == watch.y && z == watch.z && !watchSeen) {
      watchSeen = true;
      const uint32_t raw = cr.cc->voxels[li];
      auto pv = pendingVacate_.find(World::PackChunkKey(cr.wc));
      std::printf("island-watch: tick %u cell (%d,%d,%d) mirror 0x%08x ver %u class %d"
                  " | overlay %s word 0x%08x class %d pvTick %u pvCells %zu\n",
                  tick, x, y, z, raw, cr.cc->version, classOfWord(raw),
                  cr.overlay ? "yes" : "no", w, classOfWord(w),
                  pv != pendingVacate_.end() ? pv->second.tick : 0u,
                  pv != pendingVacate_.end() ? pv->second.cells.size() : (size_t)0);
    }
    const uint32_t mat = w & 0xFFFu;
    if (mat == 0 || mat >= classOf_.size()) return CELL_AIR;
    if (classOf_[mat] == CLASS_SOLID) return CELL_SOLID;
    if (classOf_[mat] == CLASS_POWDER) return CELL_POWDER;
    return CELL_AIR;
  };
  auto inRegion = [&](int x, int y, int z) {
    return x >= e.lo.x && x <= e.hi.x && y >= e.lo.y && y <= e.hi.y &&
           z >= e.lo.z && z <= e.hi.z;
  };

  struct Comp {
    std::vector<IVec3> cells;
    std::vector<uint32_t> words;
    bool anchored = false;
    bool boundaryAnchor = false;  // a known solid continues outside the box
    bool unknownAnchor = false;   // outside the window: assumed solid
    bool oversizeAnchor = false;  // over kMaxIslandVoxels: too big to judge
    bool powderAnchor = false;    // resting on powder: genuinely supported
    bool complete = true;         // `cells` is the whole component
    bool touchedUnfetched = false;  // reached a chunk the mirror lacks: not judged
    // Touches a component an earlier scan of this tick left in the grid for
    // want of budget: the same matter, judged already, converted later.
    bool heldByEarlier = false;
    IVec3 waitChunk{};   // the first unfetched chunk it touched...
    bool waitSet = false;
    bool viaNeighbor = false;  // anchored by touching an anchored component
    IVec3 viaCell{};           // ...that component's cell it touched
    bool made = false;   // every cell left the grid this scan
    // The first cell that anchored this component (trace only): the answer to
    // "anchored by WHAT" is a place, not a flag.
    IVec3 anchorAt{};
    bool anchorSet = false;
  };
  std::vector<Comp> comps;
  // Labels are TICK-global: this scan's component i is label labelBase + i,
  // anything below labelBase belongs to an earlier scan of the same tick and
  // is looked up in tickComps_.
  const int32_t labelBase = (int32_t)tickComps_.size();
  std::vector<IVec3> stack;
  int32_t next = 0;
  bool labelOverflow = false;  // the page cap refused a seed: retry next tick
  // What a seed already labelled by an earlier scan this tick hands over.
  bool inheritedWait = false, inheritedHeld = false;
  IVec3 inheritedWaitChunk{};
  auto anchorHere = [](Comp& comp, const IVec3& c) {
    comp.anchored = true;
    if (!comp.anchorSet) {
      comp.anchorSet = true;
      comp.anchorAt = c;
    }
  };
  floaters_.scans++;
  floaters_.scanCellsCovered +=
      (uint64_t)(e.seedHi.x - e.seedLo.x + 1) * (e.seedHi.y - e.seedLo.y + 1) *
      (e.seedHi.z - e.seedLo.z + 1);

  static const bool fullFlood = std::getenv("SANDVOX_ISLAND_FULL_FLOOD") != nullptr;
  const IVec3 sLo{std::max(e.lo.x, e.seedLo.x), std::max(e.lo.y, e.seedLo.y),
                  std::max(e.lo.z, e.seedLo.z)};
  const IVec3 sHi{std::min(e.hi.x, e.seedHi.x), std::min(e.hi.y, e.seedHi.y),
                  std::min(e.hi.z, e.seedHi.z)};
  // DOWN LAST in this table, so it is what the depth-first stack pops FIRST.
  // What anchors almost everything is the ground, and the ground is below:
  // a flood that descends before it wanders reaches the trunk foot or the
  // powder under a slab in about as many steps as the structure is tall, and
  // stops there. Left to wander a crown first, every flag on a burning tree
  // re-walked the whole tree (41 M cells over one burn) before it found the
  // ground it was standing on.
  const int nb[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                        {0, 0, 1}, {0, 0, -1}, {0, -1, 0}};
  for (int sz = sLo.z; sz <= sHi.z; sz++)
  for (int sy = sLo.y; sy <= sHi.y; sy++)
  for (int sx = sLo.x; sx <= sHi.x; sx++) {
    uint32_t sw = 0;
    const int sk = wordAt(sx, sy, sz, sw);
    if (last.overlay && last.usable) {
      const uint32_t raw = last.cc->voxels[localIdx(sx, sy, sz)];
      if (raw != sw) {
        const int rk = classOfWord(raw);
        if (rk == CELL_SOLID && sk != CELL_SOLID) overlayHid++;
        if (rk != CELL_SOLID && sk == CELL_SOLID) overlayShow++;
      }
    }
    const bool isWatch = sx == watch.x && sy == watch.y && sz == watch.z;
    if (sk != CELL_SOLID) {
      if (isWatch) std::printf("island-watch: tick %u seed reads class %d, not flooded\n", tick, sk);
      continue;
    }
    LabelPageData* spage = labelsOf(last);  // `last` is the seed's chunk: wordAt just read it
    int32_t* slab = spage ? spage->label.data() : nullptr;
    if (isWatch)
      std::printf("island-watch: tick %u seed solid, label %d (labelBase %d, next %d)\n",
                  tick, slab ? slab[localIdx(sx, sy, sz)] : -2, labelBase, next);
    if (sy == watchY && slab) {
      const int32_t L0 = slab[localIdx(sx, sy, sz)];
      band.push_back({IVec3{sx, sy, sz}, sw, L0, L0 < 0 ? next : (L0 >= labelBase ? L0 - labelBase : -1)});
    }
    if (!slab) {
      labelOverflow = true;
      continue;
    }
    const uint32_t sli = localIdx(sx, sy, sz);
    if (slab[sli] >= 0) {
      const int32_t L = slab[sli];
      if (L < labelBase) {
        // Labelled by an earlier scan this tick: its verdict is this seed's.
        // An anchored or converted component needs nothing more from us; one
        // waiting on a fetch makes this event wait on the same chunk, so both
        // run in the tick it lands and only the first of them floods.
        const TickComp& tc = tickComps_[(size_t)L];
        floaters_.sharedSeedCells++;
        if (tc.verdict == TICK_WAIT && !inheritedWait) {
          inheritedWait = true;
          inheritedWaitChunk = tc.waitChunk;
        } else if (tc.verdict == TICK_HELD) {
          inheritedHeld = true;
        }
      }
      continue;
    }
    Comp comp;
    curComp = next;
    stack.assign(1, IVec3{sx, sy, sz});
    slab[sli] = labelBase + next;
    while (!stack.empty()) {
      const IVec3 c = stack.back();
      stack.pop_back();
      uint32_t w = 0;
      wordAt(c.x, c.y, c.z, w);
      // `last` is c's chunk (wordAt just read it) and its page exists (c was
      // labelled when it was pushed) -- but `last` is a ONE-chunk cache, so
      // the page pointer must be re-looked-up, not read off the struct.
      labelsOf(last)->index[localIdx(c.x, c.y, c.z)] = (int32_t)comp.cells.size();
      comp.cells.push_back(c);
      comp.words.push_back(w);
      floaters_.scanCellsVisited++;
      if (comp.cells.size() > kMaxIslandVoxels) {  // abort: too big to judge
        anchorHere(comp, c);
        comp.oversizeAnchor = true;
      }
      if (c.y < e.seedLo.y - kAnchorDropBelowSeed ||  // this is the ground
          (c.y <= e.seedHi.y &&
           (c.x < e.seedLo.x - kAnchorReachBesideSeed ||
            c.x > e.seedHi.x + kAnchorReachBesideSeed ||
            c.z < e.seedLo.z - kAnchorReachBesideSeed ||
            c.z > e.seedHi.z + kAnchorReachBesideSeed))) {
        anchorHere(comp, c);
        comp.boundaryAnchor = true;
      }
      // Leaving the region only anchors when the structure actually CONTINUES
      // outside; a crown that merely grazes a face has air out there.
      // A region face is 128 cells from what changed, so in practice only
      // terrain gets here.
      for (auto& d : nb) {
        const int nx = c.x + d[0], ny = c.y + d[1], nz = c.z + d[2];
        if (inRegion(nx, ny, nz)) continue;
        uint32_t ow = 0;
        const int k = wordAt(nx, ny, nz, ow);
        if (k == CELL_AIR) continue;
        anchorHere(comp, c);
        if (k == CELL_UNKNOWN) comp.unknownAnchor = true;
        else comp.boundaryAnchor = true;
      }
      // resting on powder = supported: without this, every slab on a sand
      // pile would convert to a body the moment a support-loss scan runs.
      {
        uint32_t bw = 0;
        if (inRegion(c.x, c.y - 1, c.z) &&
            wordAt(c.x, c.y - 1, c.z, bw) == CELL_POWDER) {
          anchorHere(comp, c);
          comp.powderAnchor = true;
        }
      }
      for (auto& d : nb) {
        const int nx = c.x + d[0], ny = c.y + d[1], nz = c.z + d[2];
        if (!inRegion(nx, ny, nz)) continue;
        uint32_t nw = 0;
        const int k = wordAt(nx, ny, nz, nw);
        if (k == CELL_UNKNOWN) {
          if (world.CellInWindow({nx, ny, nz})) {
            comp.touchedUnfetched = true;
            if (!comp.waitSet) {
              comp.waitSet = true;
              comp.waitChunk = {nx >> 4, ny >> 4, nz >> 4};
            }
          } else {
            anchorHere(comp, c);
            comp.unknownAnchor = true;
          }
          continue;
        }
        if (k != CELL_SOLID) continue;
        // `last` is the neighbour's chunk (wordAt read it, and SOLID means it
        // was usable), so the label is one array index away.
        LabelPageData* npage = labelsOf(last);
        int32_t* nl = npage ? npage->label.data() : nullptr;
        if (!nl) {  // the page cap: too big to judge, exactly as kMaxIslandVoxels
          anchorHere(comp, c);
          comp.oversizeAnchor = true;
          continue;
        }
        const uint32_t nli = localIdx(nx, ny, nz);
        const int32_t L = nl[nli];
        if (L < 0) {
          nl[nli] = labelBase + next;
          stack.push_back({nx, ny, nz});
          continue;
        }
        if (L >= labelBase) {
          if (L != labelBase + next && comps[(size_t)(L - labelBase)].anchored) {
            // touching a component already judged anchored: so is this one
            const Comp& other = comps[(size_t)(L - labelBase)];
            anchorHere(comp, c);
            if (!comp.viaNeighbor) { comp.viaNeighbor = true; comp.viaCell = {nx, ny, nz}; }
            comp.boundaryAnchor = comp.boundaryAnchor || other.boundaryAnchor;
            comp.unknownAnchor = comp.unknownAnchor || other.unknownAnchor;
            comp.oversizeAnchor = comp.oversizeAnchor || other.oversizeAnchor;
            comp.powderAnchor = comp.powderAnchor || other.powderAnchor;
          }
          continue;
        }
        // A component an earlier scan of this tick labelled. Anchored: so is
        // this one. Waiting on a fetch: this one waits on the same chunk.
        // Held for budget: judged already, not ours to convert. Made: its
        // cells read as air through the overlay and never get here.
        const TickComp& tc = tickComps_[(size_t)L];
        if (tc.verdict == TICK_ANCHORED) {
          anchorHere(comp, c);
          if (!comp.viaNeighbor) { comp.viaNeighbor = true; comp.viaCell = {nx, ny, nz}; }
          comp.boundaryAnchor = comp.boundaryAnchor || (tc.anchorFlags & 1u);
          comp.unknownAnchor = comp.unknownAnchor || (tc.anchorFlags & 2u);
          comp.oversizeAnchor = comp.oversizeAnchor || (tc.anchorFlags & 4u);
          comp.powderAnchor = comp.powderAnchor || (tc.anchorFlags & 8u);
        } else if (tc.verdict == TICK_WAIT) {
          comp.touchedUnfetched = true;
          if (!comp.waitSet) {
            comp.waitSet = true;
            comp.waitChunk = tc.waitChunk;
          }
        } else if (tc.verdict == TICK_HELD) {
          comp.heldByEarlier = true;
        }
      }
      if (comp.anchored && !fullFlood) {  // nothing more to learn: see above
        stack.clear();
        comp.complete = false;
        break;
      }
    }
    comps.push_back(std::move(comp));
    next++;
  }
  curComp = -1;

  for (const Comp& cm : comps) {
    const bool small = cm.complete && cm.cells.size() < kMinBodyVoxels;
    if (!cm.anchored) {
      if (cm.touchedUnfetched) floaters_.deferredUnfetched++;
      else if (cm.heldByEarlier) {}
      else if (small) floaters_.smallUnanchored++;
      continue;
    }
    if (cm.oversizeAnchor) floaters_.anchoredByOversizeFlood++;
    if (cm.unknownAnchor) floaters_.anchoredByUnknownChunk++;
    if (cm.boundaryAnchor) floaters_.anchoredByRegionBoundary++;
    if (!small) continue;
    if (cm.unknownAnchor) floaters_.smallAnchoredUnknown++;
    else if (cm.boundaryAnchor) floaters_.smallAnchoredBoundary++;
    else if (cm.powderAnchor) floaters_.smallAnchoredPowder++;
  }

  // A fetch is pending: come back for the rest -- but only if it could change
  // a verdict. An ANCHORED component that also touched an unfetched chunk is
  // anchored whatever is in that chunk (the verdict is monotone), and most of
  // what a scan under a burning tree meets is exactly that: terrain, judged
  // by the drop anchor, with a frontier of never-fetched rock beyond it.
  // Waiting on those held terrain-only events for 32 rounds each.
  //
  // `waitOn` is the chunk the event will wait for: the first unfetched chunk
  // of the first component a fetch could change, or the one inherited from an
  // earlier scan this tick. NOT the first chunk the scan met -- that was
  // usually an anchored terrain flood's rock, and the event then waited on a
  // chunk that could not change its answer.
  bool fetchMatters = inheritedWait;
  IVec3 waitOn = inheritedWaitChunk;
  bool waitOnSet = inheritedWait;
  bool heldMatters = inheritedHeld;
  std::vector<uint8_t> matters(comps.size(), 0);
  for (size_t i = 0; i < comps.size(); i++) {
    const Comp& cm = comps[i];
    if (cm.anchored) continue;
    if (cm.heldByEarlier) heldMatters = true;
    if (!cm.touchedUnfetched) continue;
    matters[i] = 1;
    fetchMatters = true;
    if (!waitOnSet && cm.waitSet) {
      waitOn = cm.waitChunk;
      waitOnSet = true;
    }
  }
  if (!waitOnSet) waitOn = firstWait;
  needFetch = fetchMatters;
  bool deferEvent = needFetch || heldMatters || labelOverflow;

  // ---- THE REQUESTS, after the verdicts -----------------------------------
  //
  // First the frontier the components that matter actually stopped at, in
  // the order the flood met it. Then SPECULATION around that frontier, under
  // the same cap, so a structure lands in two or three rounds instead of one
  // chunk layer per round: a round used to fetch exactly the chunks the
  // flood could see the far side of, and a 6-chunk-tall trunk under a
  // 2-chunk-wide crown took ~10 rounds of re-flooding the whole tree. The
  // face ring of each frontier chunk first (the flood's next step), then its
  // COLUMN down to the drop-anchor floor and up to the region top (the
  // ground verdict wants the column below the seed all at once, a trunk is a
  // column above it), then the rest of the 26-ring. Every one of these is a
  // chunk the flood may never reach; the cost is bounded fetch bandwidth,
  // and a chunk already cached costs nothing.
  //
  // AND THE DIVE FRONTIER, VERDICT OR NOT. The verdict is monotone; the COST
  // is not. A terrain flood whose first dive stops at an unfetched chunk two
  // layers under the seed spreads sideways instead, through every chunk of
  // rock the mirror happens to hold, until a powder patch or the 96-cell
  // reach anchors it -- 8k, then 33k, then 200k cells a scan as the fire's
  // other events fetched more of the slab -- and being anchored it would
  // never ask for the chunks below, so it paid that on every support flag
  // for the rest of the burn (11.7 M cells over one oak, 5x the old code,
  // which requested whatever it met). With the column under the seed
  // fetched, the dive hits kAnchorDropBelowSeed in ~48 cells and each
  // neighbouring seed column inherits that anchored component after one.
  uint32_t fetchesIssued = 0, speculative = 0;
  if (!wanted.empty()) {
    const int cx0 = e.lo.x >> 4, cx1 = e.hi.x >> 4;
    const int cy0 = e.lo.y >> 4, cy1 = e.hi.y >> 4;
    const int cz0 = e.lo.z >> 4, cz1 = e.hi.z >> 4;
    std::unordered_map<uint64_t, uint8_t> issued;
    auto request = [&](IVec3 wc, bool spec) {
      if (fetchesIssued >= kIslandFetchPerScan) return;
      if (wc.x < cx0 || wc.x > cx1 || wc.y < cy0 || wc.y > cy1 || wc.z < cz0 ||
          wc.z > cz1)
        return;  // outside the region: the flood never goes there
      if (!world.ChunkInWindow(wc)) return;
      const uint64_t key = World::PackChunkKey(wc);
      if (issued.count(key)) return;
      issued[key] = 1;
      if (spec) {
        const CachedChunk* cc = world.Cached(wc);
        if (cc && cc->voxels.size() == kChunkVol) return;  // already readable
      }
      world.RequestChunkFetch(wc, World::FetchSource::IslandScan);  // the world dedupes against its own FIFO
      fetchesIssued++;
      if (spec) {
        speculative++;
        floaters_.speculativeFetches++;
      }
    };
    auto counts = [&](const Want& w) { return w.comp < 0 || matters[(size_t)w.comp]; };
    for (const Want& w : wanted)
      if (counts(w)) request(w.wc, false);
    if (inheritedWait) request(inheritedWaitChunk, false);
    const int floorCy = (e.seedLo.y - kAnchorDropBelowSeed) >> 4;
    {
      // the dive frontier of the components already judged: what lies under
      // the seed box, nearest the seed's column first, each with the column
      // under it down to the drop floor
      std::vector<const Want*> below;
      for (const Want& w : wanted)
        if (!counts(w) && w.wc.y * (int)kChunk + (int)kChunk - 1 < e.seedLo.y)
          below.push_back(&w);
      const int scx = (e.seedLo.x + e.seedHi.x) >> 5, scz = (e.seedLo.z + e.seedHi.z) >> 5;
      std::sort(below.begin(), below.end(), [&](const Want* a, const Want* b) {
        const int da = std::abs(a->wc.x - scx) + std::abs(a->wc.z - scz);
        const int db = std::abs(b->wc.x - scx) + std::abs(b->wc.z - scz);
        return da != db ? da < db : a->wc.y > b->wc.y;
      });
      for (const Want* w : below) {
        request(w->wc, false);
        for (int cy = w->wc.y - 1; cy >= floorCy; cy--) request({w->wc.x, cy, w->wc.z}, true);
      }
    }
    if (needFetch) {  // speculation, only for what a fetch could change
    for (const Want& w : wanted) {
      if (!counts(w)) continue;
      for (auto& d : nb) request({w.wc.x + d[0], w.wc.y + d[1], w.wc.z + d[2]}, true);
    }
    for (const Want& w : wanted) {
      if (!counts(w)) continue;
      for (int cy = w.wc.y - 1; cy >= floorCy; cy--) request({w.wc.x, cy, w.wc.z}, true);
      for (int cy = w.wc.y + 1; cy <= cy1; cy++) request({w.wc.x, cy, w.wc.z}, true);
    }
    for (const Want& w : wanted) {
      if (!counts(w)) continue;
      for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++)
            if (dx | dy | dz) request({w.wc.x + dx, w.wc.y + dy, w.wc.z + dz}, true);
    }
    }
  }

  std::vector<uint32_t> order;
  order.reserve(comps.size());
  for (uint32_t c = 0; c < (uint32_t)comps.size(); c++)
    if (!comps[c].anchored && !comps[c].touchedUnfetched && !comps[c].heldByEarlier)
      order.push_back(c);
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    return comps[a].cells.size() > comps[b].cells.size();
  });

  // Every grid write below is recorded per chunk, for two readers: EventReady
  // (a re-scan must not see cells this tick removed) and ManageTerrain (a
  // collider must not keep a body's own former cells as ground — see
  // NoteVacated).
  auto noteWrite = [&](const IVec3& c, uint32_t word) {
    if (c.x == watch.x && c.y == watch.y && c.z == watch.z)
      std::printf("island-watch: tick %u WRITE 0x%08x (ops %zu spawns %zu)\n", tick, word,
                  cellOps.size(), spawns.size());
    NoteGridWrite(c, tick, word);
  };
  if (watch.x != INT32_MIN)
    for (size_t ci = 0; ci < comps.size(); ci++)
      for (const IVec3& c : comps[ci].cells)
        if (c.x == watch.x && c.y == watch.y && c.z == watch.z) {
          const Comp& cm = comps[ci];
          bool inOrder = false;
          for (uint32_t o : order) inOrder = inOrder || o == (uint32_t)ci;
          std::printf("island-watch: tick %u in comp %zu of %zu cells anchored %d (bnd %d unk %d"
                      " big %d pwd %d) at (%d,%d,%d) complete %d unf %d held %d inOrder %d\n",
                      tick, ci, cm.cells.size(), cm.anchored ? 1 : 0, cm.boundaryAnchor ? 1 : 0,
                      cm.unknownAnchor ? 1 : 0, cm.oversizeAnchor ? 1 : 0, cm.powderAnchor ? 1 : 0,
                      cm.anchorAt.x, cm.anchorAt.y, cm.anchorAt.z, cm.complete ? 1 : 0,
                      cm.touchedUnfetched ? 1 : 0, cm.heldByEarlier ? 1 : 0, inOrder ? 1 : 0);
        }

  uint32_t madeThisScan = 0;
  for (uint32_t ci : order) {
    Comp& comp = comps[ci];
    if (cellOps.size() + kMinBodyVoxels > kMaxCellOpsPerTick) {
      // The op budget is spent: the region is genuinely re-queued (the event
      // was popped by the caller), and a re-scan re-derives what is left.
      floaters_.deferredCellOpBudget++;
      deferEvent = true;
      break;
    }

    bool worthBody = comp.cells.size() >= kMinBodyVoxels &&
                     madeThisScan < kMaxNewBodiesPerScan &&
                     bodies_.size() < kMaxBodies;
    if (!worthBody) {
      // Rubble handoff: too small to read as an object, so it crumbles to
      // individual voxels. It keeps its own material unless the JSON names a
      // rubble form (stone -> gravel, glass -> sand); matter is never
      // transmuted just because it came loose. Foliage clumps vanish instead:
      // a loose leaf is not a thing. Solid rubble (a material whose rubble
      // form is itself solid) goes out as particles so it cannot hang.
      if (cellOps.size() + comp.cells.size() > kMaxCellOpsPerTick) {
        floaters_.deferredCellOpBudget++;
        deferEvent = true;
        break;
      }
      comp.made = true;  // ...unless the spawn ring refuses a cell below
      // Trace only: a single freed TWICE is a conversion that had no effect
      // (or a cell something re-filled), which is the one shape of floater
      // the sweep can name and no counter here could -- see island-watch.
      {
        static const bool traceFreed = std::getenv("SANDVOX_ISLAND_TRACE") != nullptr;
        static std::unordered_map<uint64_t, uint32_t> freedAt;
        if (traceFreed && comp.cells.size() <= 2) {
          if (freedAt.size() > 200000) freedAt.clear();
          for (size_t i = 0; i < comp.cells.size(); i++) {
            const IVec3 c = comp.cells[i];
            const uint64_t key = PackCell(c.x, c.y, c.z);
            auto it = freedAt.find(key);
            if (it != freedAt.end()) {
              uint32_t raw = 0;
              const ChunkRef& cr = chunkOf(c.x, c.y, c.z);
              if (cr.usable) raw = cr.cc->voxels[localIdx(c.x, c.y, c.z)];
              std::printf("island-watch: tick %u RE-FREED (%d,%d,%d) first freed tick %u | now word "
                          "0x%08x mirror 0x%08x ver %u overlay %s | seed (%d,%d,%d)..(%d,%d,%d)\n",
                          tick, c.x, c.y, c.z, it->second, comp.words[i], raw,
                          cr.usable ? cr.cc->version : 0u, cr.overlay ? "yes" : "no",
                          e.seedLo.x, e.seedLo.y, e.seedLo.z, e.seedHi.x, e.seedHi.y, e.seedHi.z);
            }
            freedAt[key] = tick;
          }
        }
      }
      for (size_t i = 0; i < comp.cells.size(); i++) {
        const IVec3 c = comp.cells[i];
        const uint32_t mat = comp.words[i] & 0xFFF;
        const uint32_t cellIdx = CellIndexOf(c.x, c.y, c.z);
        if (mat < foliageOf_.size() && foliageOf_[mat]) {
          cellOps.push_back({cellIdx, 0u});
          noteWrite(c, 0u);
          continue;
        }
        uint32_t rub = mat < rubbleOf_.size() ? rubbleOf_[mat] : 0;
        uint32_t state = ((cellIdx * 2654435761u) >> 8) % 3u;
        if (rub < matGpu_.size() && matGpu_[rub].klass == CLASS_LIQUID) {
          state = 7u;  // LIQ_FULL_STATE: the nibble is fullness for liquids
        } else if (rub < matGpu_.size() &&
                   (matGpu_[rub].flags & kMatFlagTinted)) {
          const bool srcTinted =
              mat < matGpu_.size() && (matGpu_[mat].flags & kMatFlagTinted);
          state = srcTinted ? ((comp.words[i] >> 12) & 0xFu) : 0u;
        }
        bool frozen = rub < matGpu_.size() && matGpu_[rub].klass == CLASS_SOLID;
        if (frozen && spawns.size() >= kMaxParticleSpawnsPerTick) {
          floaters_.deferredSpawnRing++;
          deferEvent = true;
          comp.made = false;
          continue;
        }
        if (frozen) {
          ParticleSpawn s{};
          s.px = (int32_t)((c.x * 256) + 128);
          s.py = (int32_t)((c.y * 256) + 128);
          s.pz = (int32_t)((c.z * 256) + 128);
          s.payload = (uint16_t)((rub & 0xFFF) | (state << 12));
          s.flags = 1u;  // PFLAG_ALIVE
          spawns.push_back(s);
          cellOps.push_back({cellIdx, 0u});  // vacate the grid cell
          noteWrite(c, 0u);
          continue;
        }
        cellOps.push_back({cellIdx, PackVoxNew(rub, state)});
        noteWrite(c, PackVoxNew(rub, state));
      }
      continue;
    }

    // ---- SHARDING (PLAN §5) -----------------------------------------------
    // A component is diced on a lattice of at most kShardCells per axis, and
    // each lattice cell's 6-connected pieces become one rigid body each,
    // welded to their neighbours with fixed joints along a spanning TREE
    // rooted at the heaviest shard (a loop lattice of stiff joints is the
    // classic solver jitter). An oak (92 tall) is one shard; a redwood (217)
    // is three; the int8 body lattice is never exceeded and a felled trunk
    // flexes at the welds as it goes over. Shards that fit this tick's op
    // budget are made now, largest first; the rest stay in the grid and are
    // re-derived (smaller) by the re-queued event. Under kMinBodyVoxels a
    // lattice-cut sliver is rubble, exactly as a small component is.
    IVec3 mn{INT32_MAX, INT32_MAX, INT32_MAX}, mx{INT32_MIN, INT32_MIN, INT32_MIN};
    for (const IVec3& c : comp.cells) {
      mn.x = std::min(mn.x, c.x); mn.y = std::min(mn.y, c.y); mn.z = std::min(mn.z, c.z);
      mx.x = std::max(mx.x, c.x); mx.y = std::max(mx.y, c.y); mx.z = std::max(mx.z, c.z);
    }
    const int ext[3] = {mx.x - mn.x + 1, mx.y - mn.y + 1, mx.z - mn.z + 1};
    int n[3], size[3];
    for (int a = 0; a < 3; a++) {
      n[a] = (ext[a] + kShardCells - 1) / kShardCells;
      size[a] = (ext[a] + n[a] - 1) / n[a];
    }
    auto latticeOf = [&](const IVec3& c) {
      return ((c.z - mn.z) / size[2] * n[1] + (c.y - mn.y) / size[1]) * n[0] +
             (c.x - mn.x) / size[0];
    };
    // shard id per cell (by index into comp.cells): connected pieces within
    // one lattice cell. Neighbours are found through the label pages -- a
    // cell is this component's iff its label is, and `index` then says
    // which -- instead of two hash maps rebuilt over the whole component.
    const int32_t myLabel = labelBase + (int32_t)ci;
    auto indexOf = [&](const IVec3& q) -> int32_t {
      if (!world.CellInWindow(q)) return -1;
      ChunkRef& cr = chunkOf(q.x, q.y, q.z);
      if (!cr.usable) return -1;
      LabelPageData* pg = labelsOf(cr);
      if (!pg) return -1;
      const uint32_t li = localIdx(q.x, q.y, q.z);
      return pg->label[li] == myLabel ? pg->index[li] : -1;
    };
    std::vector<int32_t> shardOfIdx(comp.cells.size(), -1);
    struct Shard {
      std::vector<uint32_t> idx;  // indices into comp.cells
      IVec3 mn{INT32_MAX, INT32_MAX, INT32_MAX}, mx{INT32_MIN, INT32_MIN, INT32_MIN};
      float mass = 0;
      uint64_t handle = 0;
      size_t bodyIndex = 0;
      bool made = false;
    };
    std::vector<Shard> shards;
    std::vector<uint32_t> sstack;
    for (uint32_t i = 0; i < (uint32_t)comp.cells.size(); i++) {
      const IVec3 c0 = comp.cells[i];
      if (shardOfIdx[i] >= 0) continue;
      const int lat = latticeOf(c0);
      const int32_t sid = (int32_t)shards.size();
      shards.push_back(Shard{});
      shardOfIdx[i] = sid;
      sstack.assign(1, i);
      while (!sstack.empty()) {
        const uint32_t j = sstack.back();
        sstack.pop_back();
        const IVec3 c = comp.cells[j];
        Shard& sh = shards[(size_t)sid];
        sh.idx.push_back(j);
        sh.mn.x = std::min(sh.mn.x, c.x); sh.mn.y = std::min(sh.mn.y, c.y); sh.mn.z = std::min(sh.mn.z, c.z);
        sh.mx.x = std::max(sh.mx.x, c.x); sh.mx.y = std::max(sh.mx.y, c.y); sh.mx.z = std::max(sh.mx.z, c.z);
        const uint32_t mat = comp.words[j] & 0xFFF;
        sh.mass += mat < densityOf_.size() ? densityOf_[mat] : 1000.0f;
        for (auto& d : nb) {
          const IVec3 q{c.x + d[0], c.y + d[1], c.z + d[2]};
          const int32_t qi = indexOf(q);
          if (qi < 0) continue;
          if (latticeOf(q) != lat) continue;
          if (shardOfIdx[(size_t)qi] >= 0) continue;
          shardOfIdx[(size_t)qi] = sid;
          sstack.push_back((uint32_t)qi);
        }
      }
    }
    // Largest shards first, so what the budget covers is the trunk and not the
    // twigs; slivers below the body floor crumble as rubble.
    std::vector<uint32_t> sorder(shards.size());
    for (uint32_t s = 0; s < (uint32_t)shards.size(); s++) sorder[s] = s;
    std::sort(sorder.begin(), sorder.end(), [&](uint32_t a, uint32_t b) {
      return shards[a].idx.size() > shards[b].idx.size();
    });
    uint32_t bodyShards = 0;
    for (uint32_t s : sorder)
      if (shards[s].idx.size() >= kMinBodyVoxels) bodyShards++;
    if (bodies_.size() + bodyShards > kMaxBodies) {
      // Not enough body slots for the assembly: hold the whole component in
      // the grid and come back (PostStep retires the oldest bodies over time;
      // settle-back frees slots as things come to rest).
      floaters_.deferredBodyCap++;
      deferEvent = true;
      continue;
    }
    uint32_t assembly = 0;
    if (bodyShards > 1) assembly = nextAssembly_++;
    bool anyDeferred = false;
    for (uint32_t s : sorder) {
      Shard& sh = shards[s];
      if (cellOps.size() + sh.idx.size() > kMaxCellOpsPerTick) {
        floaters_.deferredCellOpBudget++;
        anyDeferred = true;
        continue;
      }
      if (sh.idx.size() < kMinBodyVoxels) {
        // a lattice-cut sliver: rubble, as a small component would be
        for (uint32_t j : sh.idx) {
          const IVec3 c = comp.cells[j];
          const uint32_t mat = comp.words[j] & 0xFFF;
          const uint32_t cellIdx = CellIndexOf(c.x, c.y, c.z);
          if (mat < foliageOf_.size() && foliageOf_[mat]) {
            cellOps.push_back({cellIdx, 0u});
            noteWrite(c, 0u);
            continue;
          }
          const uint32_t rub = mat < rubbleOf_.size() ? rubbleOf_[mat] : 0;
          const bool frozen = rub < matGpu_.size() && matGpu_[rub].klass == CLASS_SOLID;
          const uint32_t state = ((cellIdx * 2654435761u) >> 8) % 3u;
          if (frozen) {
            if (spawns.size() >= kMaxParticleSpawnsPerTick) {
              floaters_.deferredSpawnRing++;
              anyDeferred = true;
              continue;
            }
            ParticleSpawn sp{};
            sp.px = (int32_t)((c.x * 256) + 128);
            sp.py = (int32_t)((c.y * 256) + 128);
            sp.pz = (int32_t)((c.z * 256) + 128);
            sp.payload = (uint16_t)((rub & 0xFFF) | (state << 12));
            sp.flags = 1u;
            spawns.push_back(sp);
            cellOps.push_back({cellIdx, 0u});
            noteWrite(c, 0u);
            continue;
          }
          cellOps.push_back({cellIdx, PackVoxNew(rub, state)});
          noteWrite(c, PackVoxNew(rub, state));
        }
        continue;
      }
      Body body;
      body.voxels.reserve(sh.idx.size());
      for (uint32_t j : sh.idx) {
        const IVec3 c = comp.cells[j];
        DebrisVoxel v;
        v.x = (int8_t)(c.x - sh.mn.x);
        v.y = (int8_t)(c.y - sh.mn.y);
        v.z = (int8_t)(c.z - sh.mn.z);
        v.payload = (uint16_t)(comp.words[j] & 0xFFFF);
        body.voxels.push_back(v);
        cellOps.push_back({CellIndexOf(c.x, c.y, c.z), 0u});
        noteWrite(c, 0u);
      }
      const IVec3 origin = sh.mn;
      {
        PhaseTimer pt(prof_, Phase::BodyCreate);
        body.handle = phys_->CreateDebrisBody(body.voxels, origin, densityOf_);
      }
      if (prof_.on) {
        prof_.bodiesCreated++;
        prof_.bodyVoxCreated += body.voxels.size();
      }
      if (body.handle == 0) continue;
      body.xf.pos = Vec3{(float)origin.x, (float)origin.y, (float)origin.z};
      body.xf.quat[0] = body.xf.quat[1] = body.xf.quat[2] = 0;
      body.xf.quat[3] = 1;
      const float ex = (float)(sh.mx.x - sh.mn.x + 1), ey = (float)(sh.mx.y - sh.mn.y + 1),
                  ez = (float)(sh.mx.z - sh.mn.z + 1);
      body.radiusVoxels = 0.5f * std::sqrt(ex * ex + ey * ey + ez * ez) + 2.0f;
      body.serial = nextSerial_++;
      // OWNERSHIP IS STAMPED WITH THE SERIAL, not defaulted: the two together
      // ARE the global id, and a client whose localPlayerId_ is not 0 must
      // mint into its own band or the two machines' ids collide.
      body.owner = body.ownerAtCreate = localPlayerId_;
      body.assembly = assembly;
      RecountBurn(body);
      body.domMat = DominantMaterial(body.voxels);
      breaks_.push_back(BreakEvent{
          Vec3{(float)origin.x + 0.5f * ex, (float)origin.y + 0.5f * ey,
               (float)origin.z + 0.5f * ez},
          body.domMat, (int32_t)body.voxels.size()});
      sh.handle = body.handle;
      sh.bodyIndex = bodies_.size();
      sh.made = true;
      bodies_.push_back(std::move(body));
      instancesDirty_ = true;
      madeThisScan++;
      floaters_.shardsMade++;
    }
    if (anyDeferred) deferEvent = true;
    comp.made = !anyDeferred;

    // ---- WELDS: a spanning tree over face-adjacent shards ------------------
    if (bodyShards > 1) {
      struct Edge {
        uint32_t a, b;
        Vec3 anchorSum{};
        uint32_t count = 0;
      };
      std::vector<Edge> edges;
      std::unordered_map<uint64_t, size_t> edgeIndex;
      for (uint32_t i = 0; i < (uint32_t)comp.cells.size(); i++) {
        const IVec3 c = comp.cells[i];
        const int32_t sa = shardOfIdx[i];
        if (!shards[(size_t)sa].made) continue;
        static const int plus[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        for (int d = 0; d < 3; d++) {  // +x, +y, +z only: each pair once
          const IVec3 q{c.x + plus[d][0], c.y + plus[d][1], c.z + plus[d][2]};
          const int32_t qi = indexOf(q);
          if (qi < 0) continue;
          const int32_t sb = shardOfIdx[(size_t)qi];
          if (sb == sa || !shards[(size_t)sb].made) continue;
          const uint32_t lo = (uint32_t)std::min(sa, sb), hi = (uint32_t)std::max(sa, sb);
          const uint64_t ek = ((uint64_t)lo << 32) | hi;
          auto ei = edgeIndex.find(ek);
          if (ei == edgeIndex.end()) {
            edgeIndex[ek] = edges.size();
            edges.push_back(Edge{lo, hi, Vec3{}, 0});
            ei = edgeIndex.find(ek);
          }
          Edge& ed = edges[ei->second];
          ed.anchorSum = ed.anchorSum + Vec3{(float)c.x + 0.5f + 0.5f * (float)plus[d][0],
                                             (float)c.y + 0.5f + 0.5f * (float)plus[d][1],
                                             (float)c.z + 0.5f + 0.5f * (float)plus[d][2]};
          ed.count++;
        }
      }
      // BFS from the heaviest made shard; only tree edges become joints
      uint32_t root = UINT32_MAX;
      for (uint32_t s = 0; s < (uint32_t)shards.size(); s++)
        if (shards[s].made && (root == UINT32_MAX || shards[s].mass > shards[root].mass))
          root = s;
      std::vector<uint64_t> handles;
      if (root != UINT32_MAX) {
        std::vector<uint8_t> seen(shards.size(), 0);
        std::vector<uint32_t> q{root};
        seen[root] = 1;
        for (size_t qi = 0; qi < q.size(); qi++) {
          const uint32_t s = q[qi];
          handles.push_back(shards[s].handle);
          for (const Edge& ed : edges) {
            const uint32_t o = ed.a == s ? ed.b : (ed.b == s ? ed.a : UINT32_MAX);
            if (o == UINT32_MAX || seen[o]) continue;
            seen[o] = 1;
            q.push_back(o);
            Physics::JointDesc jd;
            jd.type = Physics::JointType::Fixed;
            jd.anchorVoxel = ed.anchorSum * (1.0f / (float)std::max(ed.count, 1u));
            if (phys_->CreateJoint(shards[s].handle, shards[o].handle, jd) != 0)
              floaters_.weldsMade++;
          }
        }
      }
      if (handles.size() > 1) phys_->DisableCollisionsAmong(handles);
    }
    std::printf("debris: island of %zu voxels -> %u body shard%s (total %zu)\n",
                comp.cells.size(), bodyShards, bodyShards == 1 ? "" : "s",
                bodies_.size());
  }

  for (const BandSeed& b : band) {
    if (b.comp < 0) {
      const TickComp& tc = tickComps_[(size_t)b.label];
      std::printf("island-band: tick %u (%d,%d,%d) mat %u INHERITED label %d verdict %u flags %u\n",
                  tick, b.c.x, b.c.y, b.c.z, b.word & 0xFFFu, b.label, tc.verdict, tc.anchorFlags);
      continue;
    }
    const Comp& cm = comps[(size_t)b.comp];
    std::printf("island-band: tick %u (%d,%d,%d) mat %u comp %d of %zu cells anch %d bnd %d unk %d "
                "big %d pwd %d via %d at (%d,%d,%d) viaCell (%d,%d,%d) complete %d unf %d held %d "
                "made %d | seed (%d,%d,%d)..(%d,%d,%d)\n",
                tick, b.c.x, b.c.y, b.c.z, b.word & 0xFFFu, b.comp, cm.cells.size(),
                cm.anchored ? 1 : 0, cm.boundaryAnchor ? 1 : 0, cm.unknownAnchor ? 1 : 0,
                cm.oversizeAnchor ? 1 : 0, cm.powderAnchor ? 1 : 0, cm.viaNeighbor ? 1 : 0,
                cm.anchorAt.x, cm.anchorAt.y, cm.anchorAt.z, cm.viaCell.x, cm.viaCell.y,
                cm.viaCell.z, cm.complete ? 1 : 0, cm.touchedUnfetched ? 1 : 0,
                cm.heldByEarlier ? 1 : 0, cm.made ? 1 : 0, e.seedLo.x, e.seedLo.y, e.seedLo.z,
                e.seedHi.x, e.seedHi.y, e.seedHi.z);
  }

  // The tick's verdict table: one entry per component, in label order, so a
  // later scan this tick that meets these cells inherits instead of
  // re-flooding (see the seed loop). Must stay exactly comps.size() long --
  // labels index it.
  for (const Comp& cm : comps) {
    TickComp tc;
    if (cm.anchored) {
      tc.verdict = TICK_ANCHORED;
      tc.anchorFlags = (uint8_t)((cm.boundaryAnchor ? 1u : 0u) |
                                 (cm.unknownAnchor ? 2u : 0u) |
                                 (cm.oversizeAnchor ? 4u : 0u) |
                                 (cm.powderAnchor ? 8u : 0u));
    } else if (cm.touchedUnfetched) {
      tc.verdict = TICK_WAIT;
      tc.waitChunk = cm.waitSet ? cm.waitChunk : waitOn;
    } else if (cm.made) {
      tc.verdict = TICK_MADE;
    } else {
      tc.verdict = TICK_HELD;
    }
    tickComps_.push_back(tc);
  }

  // SANDVOX_ISLAND_TRACE=1: one line per scan, naming what the scan saw and
  // what it is waiting for. The live --fell-tree harness found a cut tree
  // that never became a body (99 scans, 3 fetch give-ups, 165k cells read
  // from unfetched chunks) and the FloaterProbe totals could not say whether
  // the fetches were never issued, never landed, or landed and were re-read
  // stale. This line says it per scan; off unless asked.
  static const bool traceScans = std::getenv("SANDVOX_ISLAND_TRACE") != nullptr;
  if (traceScans) {
    size_t largest = 0, cellsAll = 0;
    int lFlags = 0;
    IVec3 lLo{}, lHi{}, lAnchor{};
    for (const Comp& cm : comps) {
      cellsAll += cm.cells.size();
      if (cm.cells.size() > largest) {
        largest = cm.cells.size();
        lFlags = (cm.anchored ? 1 : 0) | (cm.boundaryAnchor ? 2 : 0) |
                 (cm.unknownAnchor ? 4 : 0) | (cm.oversizeAnchor ? 8 : 0) |
                 (cm.powderAnchor ? 16 : 0) | (cm.touchedUnfetched ? 32 : 0) |
                 (cm.complete ? 64 : 0);
        lAnchor = cm.anchorAt;
        lLo = lHi = cm.cells.empty() ? IVec3{} : cm.cells[0];
        for (const IVec3& cc : cm.cells) {
          lLo.x = std::min(lLo.x, cc.x); lLo.y = std::min(lLo.y, cc.y); lLo.z = std::min(lLo.z, cc.z);
          lHi.x = std::max(lHi.x, cc.x); lHi.y = std::max(lHi.y, cc.y); lHi.z = std::max(lHi.z, cc.z);
        }
      }
    }
    const CachedChunk* fw = needFetch ? world.Cached(waitOn) : nullptr;
    std::printf("island-trace: tick %u ev (%d,%d,%d)..(%d,%d,%d) seed (%d,%d,%d).."
                "(%d,%d,%d) retry %u fetchRetry %u | comps %zu cells %zu largest "
                "%zu flags %d (1 anch 2 bnd 4 unk 8 big 16 pwd 32 unf 64 done) box "
                "(%d,%d,%d)..(%d,%d,%d) anchorAt (%d,%d,%d) | "
                "wanted %zu fetches %u spec %u needFetch %d waitOn (%d,%d,%d) inWin %d "
                "cached %d ver %u words %zu | shared %d held %d ovHid %u ovShow %u | defer %d made %u\n",
                tick, e.lo.x, e.lo.y, e.lo.z, e.hi.x, e.hi.y, e.hi.z, e.seedLo.x,
                e.seedLo.y, e.seedLo.z, e.seedHi.x, e.seedHi.y, e.seedHi.z,
                (unsigned)e.retries, (unsigned)e.fetchRetries, comps.size(),
                cellsAll, largest, lFlags, lLo.x, lLo.y, lLo.z, lHi.x, lHi.y,
                lHi.z, lAnchor.x, lAnchor.y, lAnchor.z, wanted.size(),
                fetchesIssued, speculative, needFetch ? 1 : 0,
                waitOn.x, waitOn.y, waitOn.z,
                world.ChunkInWindow(waitOn) ? 1 : 0, fw ? 1 : 0,
                fw ? fw->version : 0u, fw ? fw->voxels.size() : (size_t)0,
                inheritedWait ? 1 : 0, heldMatters ? 1 : 0, overlayHid, overlayShow,
                deferEvent ? 1 : 0, madeThisScan);
  }
  if (deferEvent) {
    if (needFetch ? e.fetchRetries < kMaxFetchRetries : e.retries < kMaxEventRetries) {
      Event again = e;
      if (needFetch) {
        again.fetchRetries = (uint8_t)(e.fetchRetries + 1);
        again.waiting = true;
        again.waitChunk = waitOn;
      } else {
        again.retries = (uint8_t)(e.retries + 1);
        again.waiting = false;
      }
      // The event KEEPS its tick. Re-stamping it with the current tick made
      // every chunk fetched in the previous round stale for this one, so a
      // flood that needed three rounds to reach a crown never converged: it
      // gave up after 32 (4.3 M stale waits over one burn). The one thing the
      // re-stamp used to buy -- a re-scan must not see cells this tick removed
      // -- is carried per chunk by chunkWriteTick_ (RequiredVersion).
      if (events_.size() < 64) events_.push_back(again);
      else SpillRegionToSupport(again);  // never dropped, only late
    } else {
      floaters_.deferGaveUp++;
      if (needFetch) {
        floaters_.deferGaveUpFetch++;
        floaters_.gaveUpChunk = waitOn;
        floaters_.gaveUpSeedLo = e.seedLo;
        floaters_.gaveUpSeedHi = e.seedHi;
        floaters_.gaveUpChunkInWindow = world.ChunkInWindow(last.wc) ? 1 : 0;
        floaters_.gaveUpChunkCached = world.Cached(last.wc) ? 1 : 0;
      }
    }
  }
}

// Every cell the debris system writes into the grid, per chunk: the tick (so a
// scan or a collider build knows when the mirror is fresh enough to trust) and,
// for a VACATED cell, the cell itself. ManageTerrain subtracts pending vacates
// from the occupancy it meshes until the mirror has caught up, so a body is
// never born inside a collider of its own former shape — that was a severed
// cactus standing in the air for as long as it liked (gate `cactus-fell`).
void DebrisSystem::NoteGridWrite(const IVec3& c, uint32_t tick, uint32_t word) {
  const IVec3 wc{c.x >> 4, c.y >> 4, c.z >> 4};
  const uint64_t key = World::PackChunkKey(wc);
  uint32_t& wt = chunkWriteTick_[key];
  wt = std::max(wt, tick);
  // Accumulates across ticks until the mirror catches up (dropped once the
  // chunk is cached at or past `tick`); the stamp changes with every tick
  // that adds to it so a collider built against an older list rebuilds.
  PendingVacate& pv = pendingVacate_[key];
  if (pv.tick != tick) {
    pv.tick = std::max(pv.tick, tick);
    pv.stamp = nextVacateStamp_++;
  }
  pv.cells[(uint16_t)(((uint32_t)(c.z & 15) * kChunk + (uint32_t)(c.y & 15)) * kChunk +
                      (uint32_t)(c.x & 15))] = word & ~kCellOpIfAir;
}

void DebrisSystem::PreTick(uint32_t tick, World& world, std::vector<CellOp>& cellOps,
                           std::vector<ParticleSpawn>& spawns) {
  // Billed as `debris` on the Performance tab, debited from the game-logic
  // span this runs inside; ManageTerrain at the bottom debits THIS one.
  sandvox::PerfSpan span(sandvox::PerfScope::Debris, sandvox::PerfScope::GameLogic);
  // Per-phase attribution for this tick (off unless something asked). See the
  // PhaseProfile comment in debris.h for why this exists rather than an A/B.
  const auto profT0 = prof_.on ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
  for (int i = 0; i < kPhaseCount; i++) prof_.curUs[i] = 0.0;
  // Cheap and idempotent: an unchanged art palette early-outs on a stamp
  // compare. Here rather than at a load-time call site because the palette is
  // rebuilt by the mob loader, the item loader and every R hot-reload, and the
  // owner has no one place that is after all three.
  RefreshTintMap();
  // ---- WHO STEPS WHAT, THIS TICK (M9.4-C) ---------------------------------
  //
  // FIRST, before anything reads a body position: RefreshOwnership can flip a
  // body to kinematic and DriveGhosts teleports every ghost onto its newest
  // received pose, and the terrain sweep, the burn scan and the settle test
  // all read `Body::xf`. Driving after them would spend a tick of terrain
  // patches and threat probes on where a ghost was LAST tick.
  //
  // Both are no-ops with no ownership function set (the single-player case and
  // every gate but `debris-ghost`), which is why the world hash does not move.
  RefreshOwnership();
  DriveGhosts();
  // chunks whose flag the cooldown suppressed, now that it has expired
  RearmLateSupport(tick);
  // promote flagged support-loss chunks into events while there is queue room
  for (int i = 0; i < kSupportDrainPerTick && !pendingSupport_.empty(); i++) {
    IVec3 wc = pendingSupport_.front();
    IVec3 lo{wc.x * (int)kChunk, wc.y * (int)kChunk, wc.z * (int)kChunk};
    IVec3 hi{lo.x + (int)kChunk - 1, lo.y + (int)kChunk - 1, lo.z + (int)kChunk - 1};
    // A chunk can change hands while it sits in this queue (the peer walked
    // closer). Re-asked here rather than trusted from the enqueue, because the
    // queue is explicitly the one that NEVER DROPS and can therefore hold an
    // entry for a long time.
    if (!ChunkOwned(wc)) {
      ownerProbe_.chunksSkipped++;
      pendingSupport_.pop_front();
      supportPending_.erase(World::PackChunkKey(wc));
      continue;
    }
    if (world_->ChunkInWindow(wc)) {  // streamed out: forget it
      // spillOnFull = false: this IS the spill queue draining. Re-enqueueing
      // the chunk we are holding would be a loop that never empties.
      if (!AddDestructionEvent(tick, lo, hi, kSupportMargin, false))
        break;  // full: leave it queued and retry next tick
    }
    pendingSupport_.pop_front();
    supportPending_.erase(World::PackChunkKey(wc));
  }

  // ---- drain the event queue -----------------------------------------------
  //
  // The flood's label map and verdict table are shared by every scan of this
  // tick and forgotten here (debris.h, labelPages_).
  BeginTickLabels();
  //
  // THE HEAD OF THIS QUEUE USED TO BLOCK ALL OF IT. Only `events_.front()` was
  // ever examined: if the head's chunks had not arrived, the tick did nothing
  // at all, however many events behind it were sitting on fully cached regions.
  // A head takes 120 ticks to time out, and everything queued behind it aged
  // toward its own timeout the whole while — so one region that streamed out or
  // lost a readback took the next four seconds of island detection down with
  // it. Measured on the `tree-fell` fixture: 312 events dropped over one burning
  // tree, each one a region whose floaters nothing would ever look at again.
  //
  // So: probe a bounded PREFIX of the queue and run the ready ones. Nothing
  // here is unbounded — kEventProbePerTick entries examined, at most
  // kIslandScanCellsPerTick cells of dense mask flooded (two wide scans, or
  // sixteen narrow ones), and the scan stops early once this tick's grid-op
  // budget is half spent (a scan that cannot emit its ops just defers itself,
  // which costs a re-scan for nothing).
  {
    uint32_t cellsLeft = kIslandScanCellsPerTick;
    uint32_t probed = 0;
    size_t qi = 0;
    while (qi < events_.size() && probed < kEventProbePerTick) {
      const Event e = events_[qi];
      probed++;
      // THE ISLAND SCAN IS THE BIGGEST OP EMITTER IN THIS FILE (it erases a
      // whole component out of the grid), so it is gated on the REGION rather
      // than on any body: whoever owns the chunk the change happened in runs
      // the scan, including for a crater a peer blew. Dropped rather than
      // deferred -- the owner is running it, so there is nothing here to wait
      // for, and a deferred event would just age to its timeout.
      if (!ChunkOwned(EventChunk(e))) {
        events_.erase(events_.begin() + (long)qi);
        ownerProbe_.chunksSkipped++;
        continue;
      }
      // Only the first couple of probes are allowed to REQUEST fetches. Beyond
      // that the probe is a cache lookup: an event deep in the queue that
      // happens to be ready should run, but it must not push 64 more chunks
      // into a fetch queue that drains 64 a tick and is what the events in
      // front of it are waiting on.
      const bool mayFetch = probed <= kEventFetchProbes;
      bool ready;
      {
        PhaseTimer pt(prof_, Phase::EventDrain);
        ready = EventReady(e, world, mayFetch);
      }
      if (ready) {
        // The budget is spent by the scan that overruns it, not refused: a
        // wide scan must still be able to run on a tick whose budget is
        // mostly gone, or a stream of cheap scans could starve it forever.
        if (cellsLeft == 0) break;
        if (cellOps.size() > kMaxCellOpsPerTick / 2) break;
        events_.erase(events_.begin() + (long)qi);
        // CHARGED WHAT IT COST, not what it covered. The first version of this
        // budget charged the region's volume, so the 68x cheaper flood (seed
        // from the changed box, stop at the first anchor) bought exactly zero
        // extra scans a tick -- still two 64^3 regions -- and a real fire, with
        // a whole crown re-flagging every 45 ticks, starved just as before.
        // The mask build over the region is charged too, since it is real
        // work: a scan costs its region once plus whatever the flood walked.
        const uint64_t visitedBefore = floaters_.scanCellsVisited;
        {
          PhaseTimer pt(prof_, Phase::IslandScan);
          RunIslandDetection(e, tick, world, cellOps, spawns);
        }
        const uint64_t cost = (floaters_.scanCellsVisited - visitedBefore) +
                              (uint64_t)(e.seedHi.x - e.seedLo.x + 1) *
                                  (uint64_t)(e.seedHi.y - e.seedLo.y + 1) *
                                  (uint64_t)(e.seedHi.z - e.seedLo.z + 1) / 8u;
        cellsLeft = cost >= cellsLeft ? 0u : cellsLeft - (uint32_t)cost;
        // terrain under the blast changed: sleeping debris nearby must re-check
        // Around what CHANGED, not the 256-cell region: waking every body in
        // a 128-cell radius on every scan would keep a whole battlefield up.
        Vec3 c{(float)(e.seedLo.x + e.seedHi.x) * 0.5f,
               (float)(e.seedLo.y + e.seedHi.y) * 0.5f,
               (float)(e.seedLo.z + e.seedHi.z) * 0.5f};
        settle_.blastWakes++;
        settle_.lastWakeTick = tick;
        phys_->WakeNear(c, 64.0f);
        continue;  // qi now indexes the entry that followed it
      }
      if (tick > e.tick + kEventStuckTicks) {
        // STUCK, AND NO LONGER DROPPED. Readback starvation and a region that
        // streamed out are not the same thing and used to share one counter and
        // one outcome. A region still IN the window is spilled onto
        // pendingSupport_ — the queue that never drops, deduped per chunk with
        // a cooldown, so this costs one entry and the scan happens late instead
        // of never. Only a region that has genuinely left the window is
        // dropped, and that one is not a leak: there is nothing there to float.
        //
        // WHAT BOUNDS THE CYCLE, since a spill can come back as events and be
        // stuck again: the per-chunk `supportCooldown_` (45 ticks) and the
        // `kMaxPendingSupport` ceiling, exactly as they bound the ordinary flag
        // path — a chunk that keeps burning re-flags forever too, and that is
        // the design. So this is rate-limited, not eliminated, and the rate is
        // one visit per chunk per 45 ticks. `stuckEventRequeued` is what would
        // show a runaway; on a whole burning tree it reads 28.
        events_.erase(events_.begin() + (long)qi);
        const bool resident = world.ChunkInWindow(
            {(e.lo.x + e.hi.x) >> 5, (e.lo.y + e.hi.y) >> 5,
             (e.lo.z + e.hi.z) >> 5});
        if (resident && e.retries < kMaxEventRetries) {
          floaters_.stuckEventRequeued++;
          SpillRegionToSupport(e);
        } else {
          floaters_.stuckEventDropped++;
        }
        continue;
      }
      qi++;
    }
  }
  // Bookkeeping the mirror never came back for: a chunk written into and
  // then never needed by a scan or a collider keeps its entry until here.
  if ((tick & 255u) == 0u) {
    for (auto it = chunkWriteTick_.begin(); it != chunkWriteTick_.end();)
      it = it->second + 1200 < tick ? chunkWriteTick_.erase(it) : std::next(it);
    for (auto it = pendingVacate_.begin(); it != pendingVacate_.end();)
      it = it->second.tick + 600 < tick ? pendingVacate_.erase(it) : std::next(it);
  }
  { PhaseTimer pt(prof_, Phase::Burn); BurnBodies(tick, world, cellOps, spawns); }
  // ...and the third rung of a beating, on the same clock as the burn: pulped
  // tissue crumbling out of a body somebody caved in (PulpTick). Charged to
  // the burn phase because it is the same shape of work on the same bodies,
  // and because a body nothing has beaten pays one bool for it.
  { PhaseTimer pt(prof_, Phase::Burn); PulpTick(tick, world, spawns); }
  { PhaseTimer pt(prof_, Phase::Bleed); BleedBodies(tick, world, spawns); }
  // Before the settle test, not after: buoyancy is what decides whether a body
  // is still moving this tick, and SettleBodies counts inactive ticks.
  FloatBodies(tick, world);
  { PhaseTimer pt(prof_, Phase::Settle); SettleBodies(tick, world, cellOps); }
  ManageTerrain(tick, world);
  if (prof_.on) {
    const double tickUs = std::chrono::duration<double, std::micro>(
                              std::chrono::steady_clock::now() - profT0).count();
    for (int i = 0; i < kPhaseCount; i++) prof_.totalUs[i] += prof_.curUs[i];
    prof_.ticks++;
    prof_.tickUsTotal += tickUs;
    if (tickUs > 8000.0) prof_.ticksOver8ms++;
    if (tickUs > 16000.0) prof_.ticksOver16ms++;
    if (tickUs > 33000.0) prof_.ticksOver33ms++;
    if (tickUs > prof_.worstTickUs) {
      prof_.worstTickUs = tickUs;
      prof_.worstTick = tick;
      for (int i = 0; i < kPhaseCount; i++) prof_.worstUs[i] = prof_.curUs[i];
    }
    // A LIVE SESSION GETS THE SAME LINE THE GATE PRINTS. Ten seconds of sim at
    // a time, and only when something happened -- a settled world with no
    // bodies has nothing to say and should not say it every ten seconds.
    if (prof_.autoReport && prof_.ticks >= 300) {
      if (prof_.worstTickUs > 1000.0 || prof_.bodiesCreated)
        std::printf("[debris-prof] %s\n", ProfileReport().c_str());
      ResetProfile();
    }
  }
}

const char* DebrisSystem::PhaseName(Phase p) {
  switch (p) {
    case Phase::EventDrain: return "eventProbe";
    case Phase::IslandScan: return "islandScan";
    case Phase::BodyCreate: return "  bodyCreate(of scan)";
    case Phase::Burn: return "burnBodies";
    case Phase::Bleed: return "bleedBodies";
    case Phase::Settle: return "settleBodies";
    case Phase::TerrainNeed: return "terrainNeed";
    case Phase::TerrainScan: return "terrainScan";
    case Phase::TerrainGather: return "terrainGather";
    case Phase::TerrainPoly: return "terrainPoly";
    case Phase::TerrainJolt: return "terrainJolt";
    case Phase::TerrainEvict: return "terrainEvict";
    case Phase::Instances: return "buildInstances";
    default: return "?";
  }
}

std::string DebrisSystem::ProfileReport() const {
  const PhaseProfile& p = prof_;
  const double ticks = p.ticks ? (double)p.ticks : 1.0;
  std::string s = Fmt(
      "%u ticks, %.1f ms total (%.2f ms/tick avg), worst tick %u at %.1f ms; "
      "over 8/16/33 ms: %u/%u/%u",
      p.ticks, p.tickUsTotal / 1000.0, p.tickUsTotal / 1000.0 / ticks,
      p.worstTick, p.worstTickUs / 1000.0, p.ticksOver8ms, p.ticksOver16ms,
      p.ticksOver33ms);
  double named = 0;
  for (int i = 0; i < kPhaseCount; i++)
    if (i != (int)Phase::BodyCreate && i != (int)Phase::Instances)
      named += p.totalUs[i];
  for (int i = 0; i < kPhaseCount; i++) {
    if (p.totalUs[i] < 1.0 && p.worstUs[i] < 1.0) continue;
    s += Fmt(" | %s %.1f ms tot (%.2f/tick, worst %.1f) x%llu",
                PhaseName((Phase)i), p.totalUs[i] / 1000.0,
                p.totalUs[i] / 1000.0 / ticks, p.worstUs[i] / 1000.0,
                (unsigned long long)p.calls[i]);
  }
  s += Fmt(
      " | unattributed %.1f ms | chunks needed %llu (max %u in one tick), "
      "fetches asked %llu (max %u), gathers %llu, polygonizes %llu, jolt "
      "meshes %llu, bodies %llu (%llu vox), need blocks %llu, anchor chunks "
      "%llu",
      (p.tickUsTotal - named) / 1000.0,
      (unsigned long long)p.chunksNeeded, p.maxNeededOneTick,
      (unsigned long long)p.fetchesAsked, p.maxFetchOneTick,
      (unsigned long long)p.gathers, (unsigned long long)p.polys,
      (unsigned long long)p.joltMeshes, (unsigned long long)p.bodiesCreated,
      (unsigned long long)p.bodyVoxCreated, (unsigned long long)p.needBlocks,
      (unsigned long long)p.anchorChunks);
  return s;
}

namespace {

// Collapse a MICRO-unit voxel set (scale micro voxels per world voxel) down to
// world voxels, one output voxel per scale^3 block. A block that is less than
// half full becomes air, and a block that survives takes its most common
// material — so a scale-2 critter leg settles as the solid 3x1x1 stub it
// visually is, rather than as a 6x2x2 lump at twice its real size.
//
// The vote is a linear scan over a tiny fixed array rather than a map: a limb
// block holds at most scale^3 (8 or 64) voxels and realistically one or two
// distinct materials, and at those sizes a hash map is all overhead. Past
// kMaxDistinct materials in one block the first-seen ones win, which is
// invisible — no authored limb has eight materials in a 2x2x2 box.
//
// The plurality is over the (payload, ART COLOUR) PAIR, for the reason
// DownsampleSkin in phys/lattice.h spells out: this is the lattice settle-back
// reads, so dropping the colour here is dropping the dye on the way into the
// grid (sim/materials.h kMatFlagTinted).
std::vector<DebrisVoxel> DownsampleMicro(const std::vector<DebrisVoxel>& src,
                                         uint32_t scale) {
  const int s = (int)scale;
  const uint32_t full = (uint32_t)(s * s * s);
  constexpr int kMaxDistinct = 8;
  struct Blk {
    uint32_t count = 0;
    int n = 0;
    uint32_t pair[kMaxDistinct]{};  // payload | color << 16
    uint32_t hits[kMaxDistinct]{};
  };
  std::unordered_map<uint64_t, Blk> blocks;
  for (const DebrisVoxel& v : src) {
    // DebrisVoxel coords are non-negative by construction (bodies are rebased
    // to their min corner), so a plain divide is the block index.
    uint64_t key = ((uint64_t)(uint8_t)((int)v.x / s) << 32) |
                   ((uint64_t)(uint8_t)((int)v.y / s) << 16) |
                   (uint8_t)((int)v.z / s);
    const uint32_t pair = (uint32_t)v.payload | ((uint32_t)v.color << 16);
    Blk& blk = blocks[key];
    blk.count++;
    int k = 0;
    for (; k < blk.n; k++)
      if (blk.pair[k] == pair) break;
    if (k == blk.n && blk.n < kMaxDistinct) blk.pair[blk.n++] = pair;
    if (k < kMaxDistinct) blk.hits[k]++;
  }

  std::vector<DebrisVoxel> out;
  out.reserve(blocks.size());
  for (const auto& [key, blk] : blocks) {
    if (blk.count * 2 < full) continue;  // majority-fill: mostly air -> air
    int best = 0;
    for (int k = 1; k < blk.n; k++)
      if (blk.hits[k] > blk.hits[best]) best = k;
    out.push_back({(int8_t)(uint8_t)(key >> 32), (int8_t)(uint8_t)(key >> 16),
                   (int8_t)(uint8_t)key, (uint8_t)(blk.pair[best] >> 16),
                   (uint16_t)(blk.pair[best] & 0xFFFFu)});
  }
  return out;
}

}  // namespace

// Is anything in the GRID holding this footprint up?
//
// Walks the snapped footprint and asks, for each voxel, whether the cell
// directly below it is solid or powder. Cells belonging to the footprint
// itself are skipped — a body's own lower voxels are not support for its upper
// ones. Early-outs on the first support found, so a body sitting flat on the
// ground costs one lookup.
//
// AN UNKNOWN CELL BELOW DOES NOT REFUSE THE SETTLE, and that is a correction
// paid for by a full-suite run. The first version read an uncached chunk as
// "no support" on the theory that refusing is always the safe direction. It is
// not: a refusal is permanent in effect, because a body whose underside is
// never cached never settles, and `--gate floaters` could not see that (the
// gate-scope cache held the fixture, the full-suite cache did not — the same
// subset-versus-suite trap CLAUDE.md rule 7 describes, and it turned the
// CONTROL arm red while the arm under test stayed green).
//
// So the rule is: refuse only when the cells below are AFFIRMATIVELY empty.
// Unknown chunks are requested and abstain. That also puts this in line with
// RunIslandDetection's convention (an unknown cell reads as solid) instead of
// deliberately against it.
//
// It costs nothing on the bug this exists for. The body-on-body case — a body
// resting on another body, which the world knows nothing about — has KNOWN AIR
// underneath, because ManageTerrain fetches every chunk within radius + 6 of
// every body precisely so it can build collision there. A body whose underside
// really is unfetched has no collision either, so it is falling, not sleeping,
// and cannot reach this code.
bool DebrisSystem::SettleFootprintSupported(const std::vector<DebrisVoxel>& src,
                                            const int (*snap)[3], IVec3 base,
                                            World& world) const {
  // A/B SWITCH, in the spirit of SANDVOX_PT_DEBUG / SANDVOX_PT_AUDIT. Setting
  // SANDVOX_NO_SETTLE_SUPPORT=1 restores the pre-2026-09-03 behaviour of
  // settling anything that is asleep and aligned, so "did the support test
  // cause this" is ONE run against the same binary rather than a revert, a
  // rebuild and a bisect. Read once; costs a branch on a path that runs for at
  // most one body per tick.
  static const bool kDisabled = [] {
    const char* e = std::getenv("SANDVOX_NO_SETTLE_SUPPORT");
    return e && *e && *e != '0';
  }();
  if (kDisabled) return true;
  // The footprint as a set, so "the cell below is my own voxel" is answerable
  // without an O(n^2) scan. Bodies that reach here are at most a few thousand
  // voxels (kMaxIslandVoxels bounds the island that made them), and this runs
  // for at most one body per tick.
  // Its own packer rather than World::PackChunkKey: that one is named for
  // CHUNK coordinates and these are CELL coordinates, and a key function
  // borrowed across two coordinate systems is exactly the kind of quiet
  // aliasing this codebase pays for elsewhere. Same 21-bits-per-axis layout,
  // which covers +-1,048,576 cells — four orders of magnitude past the
  // residency window.
  auto cellKey = [](IVec3 c) -> uint64_t {
    auto u = [](int v) { return (uint64_t)(uint32_t)(v + (1 << 20)) & 0x1FFFFF; };
    return u(c.x) | (u(c.y) << 21) | (u(c.z) << 42);
  };
  std::unordered_set<uint64_t> occupied;
  occupied.reserve(src.size() * 2);
  auto cellOf = [&](const DebrisVoxel& v) {
    const float lx = (float)v.x + 0.5f, ly = (float)v.y + 0.5f,
                lz = (float)v.z + 0.5f;
    return IVec3{
        base.x + ifloor(snap[0][0] * lx + snap[0][1] * ly + snap[0][2] * lz),
        base.y + ifloor(snap[1][0] * lx + snap[1][1] * ly + snap[1][2] * lz),
        base.z + ifloor(snap[2][0] * lx + snap[2][1] * ly + snap[2][2] * lz)};
  };
  for (const DebrisVoxel& v : src) {
    IVec3 c = cellOf(v);
    occupied.insert(cellKey(c));
  }
  bool sawUnknown = false;
  for (const DebrisVoxel& v : src) {
    IVec3 c = cellOf(v);
    IVec3 below{c.x, c.y - 1, c.z};
    if (occupied.count(cellKey(below))) continue;  // my own voxel, not support
    if (!world.CellInWindow(below)) return true;  // outside the window is solid
    IVec3 wc = ChunkOfCell(below.x, below.y, below.z);
    const CachedChunk* cc = world.Cached(wc);
    if (!cc || cc->voxels.size() != kChunkVol) {
      world.RequestChunkFetch(wc, World::FetchSource::Settle);
      sawUnknown = true;  // abstain: cannot prove this body is over a void
      continue;
    }
    uint32_t mat = cc->voxels[((uint32_t)(below.z & 15) * kChunk +
                               (uint32_t)(below.y & 15)) * kChunk +
                              (uint32_t)(below.x & 15)] & 0xFFF;
    if (mat != 0 && mat < classOf_.size() &&
        (classOf_[mat] == CLASS_SOLID || classOf_[mat] == CLASS_POWDER))
      return true;
  }
  // Nothing supported it. An UNREADABLE cell below is NOT support -- this used
  // to abstain by returning true, which stamped the body into the grid on a
  // guess, exactly the guess DESIGN.md section 7 says this test must not make
  // ("assuming empty means do not stamp into the world"). Measured on the
  // `tree-fell` fixture: a 12-voxel leaf body rolled to the edge of the fetched
  // region, its below-chunk was not cached, it settled, and it was the biggest
  // floater left after the fire. The fetch has been requested; the body stays
  // a body and is re-tested in 30 ticks, by which time the chunk is here.
  (void)sawUnknown;
  return false;
}

// ---- Archimedes for rigid bodies (PLAN_debris_buoyancy.md phase 3) ---------
//
// A tree blown apart produces both populations: the chips that fly are
// particles and sink or float by sim_particle.wgsl's own rules, and the
// severed remainder is a Jolt body. Liquids are not in the collider — a body
// falls through water as though it were air — so before this a floating log
// was not slow, it was on the lake BED.
//
// The work is one plane and one impulse. Jolt computes the submerged volume
// from the collider exactly, per sub-shape, and applies the lift at the centre
// of the submerged part rather than at the centre of mass; that is what makes a
// half-beached log right itself and a raft bob instead of sliding down like a
// lift. Nothing here integrates anything.
//
// THE COST ARGUMENT, since this runs over every body every tick: the dry case
// is TWO mirror reads (the cell under the body, then the body's own cell) and a
// return. Only a body that is actually touching liquid walks a column, and that
// walk is capped. Bodies asleep are skipped before any of it, which is also
// what lets a raft that has come to rest stay asleep (rule 2) — buoyancy is not
// applied to a sleeping body and therefore cannot be the reason one never
// sleeps.
void DebrisSystem::FloatBodies(uint32_t tick, World& world) {
  (void)tick;
  if (!phys_ || bodies_.empty()) return;
  const float dt = 1.0f / kSimTicksPerSecond;
  const float linDrag = CurrentTuning().physics.waterLinearDrag;
  const float angDrag = CurrentTuning().physics.waterAngularDrag;

  // One-chunk memo: a column walk hits the same chunk 16 times running, and
  // World::Cached is a hash lookup. Nothing is cached ACROSS bodies — a stale
  // pointer there would outlive a streaming eviction.
  IVec3 memoWc{INT32_MIN, INT32_MIN, INT32_MIN};
  const CachedChunk* memoCc = nullptr;
  // Material id at a world cell, or 0xFFFF for "cannot see" — distinct from 0
  // (air), because "no water here" and "no idea" must not be the same answer.
  auto matAt = [&](int x, int y, int z) -> uint32_t {
    if (!world.CellInWindow({x, y, z})) return 0xFFFFu;
    IVec3 wc = ChunkOfCell(x, y, z);
    if (wc.x != memoWc.x || wc.y != memoWc.y || wc.z != memoWc.z) {
      memoWc = wc;
      memoCc = world.Cached(wc);
    }
    if (!memoCc || memoCc->voxels.size() != kChunkVol) return 0xFFFFu;
    return memoCc->voxels[((uint32_t)(z & 15) * kChunk + (uint32_t)(y & 15)) *
                              kChunk + (uint32_t)(x & 15)] & 0xFFFu;
  };
  auto isLiquid = [&](uint32_t mat) {
    return mat != 0 && mat != 0xFFFFu && mat < matGpu_.size() &&
           matGpu_[mat].klass == CLASS_LIQUID;
  };

  for (Body& b : bodies_) {
    // No follower test: a driven body (a garment strapped to the limb it
    // covers) is KINEMATIC, and ApplyBuoyancy refuses anything that is not
    // dynamic. One guard, in the place that knows why.
    if (b.handle == 0) continue;
    if (b.domMat == 0 || b.domMat >= matGpu_.size()) continue;
    const MaterialGpu& mg = matGpu_[b.domMat];
    const uint32_t lift =
        (mg.fluidPack >> kFluidPackLiftShift) & kFluidPackLiftMask;
    if (lift == 0) continue;  // this stuff does not interact with liquids
    if (!phys_->IsActive(b.handle)) continue;  // asleep: let it lie
    Vec3 com;
    if (!phys_->BodyCenterOfMass(b.handle, com)) continue;

    // Is any of it wet? Under-side first: a floating body's centre is in the
    // air and only its keel is in the water, which is the case that matters and
    // also the cheapest to ask about.
    floaters_.floatProbes++;
    const int cx = ifloor(com.x), cz = ifloor(com.z);
    const int comY = ifloor(com.y);
    const int keelY = ifloor(com.y - b.radiusVoxels);
    uint32_t mat = matAt(cx, keelY, cz);
    int seedY = keelY;
    if (!isLiquid(mat)) {
      mat = matAt(cx, comY, cz);
      seedY = comY;
      if (!isLiquid(mat)) continue;  // dry: two reads and out
    }
    floaters_.floatProbesWet++;

    // Walk up to the waterline. Capped: a body at the bottom of an ocean
    // trench must not walk the whole column, and being wrong about the surface
    // height by the cap only means a deeply submerged body is pushed up with a
    // slightly smaller plane above it — it is going up either way.
    constexpr int kMaxColumnWalk = 96;
    int surfY = seedY + 1;
    for (int i = 0; i < kMaxColumnWalk; i++) {
      const uint32_t m = matAt(cx, surfY, cz);
      if (!isLiquid(m)) break;
      surfY++;
    }

    // Jolt's parameter IS rhoFluid/rhoBody (1 = neutral, >1 floats), which is
    // the same ratio sim_particle.wgsl uses for a voxel in flight, off the same
    // `density` field. `lift` scales it toward neutral rather than toward zero:
    // a half-lift material should be half as buoyant, not weightless.
    const float rhoF = (float)matGpu_[mat].density;
    const float rhoB = (float)std::max(1, matGpu_[b.domMat].density);
    const float full = rhoF / rhoB;
    const float buoy = 1.0f + (full - 1.0f) * ((float)lift / (float)kFluidMax);
    if (phys_->ApplyBuoyancy(b.handle, (float)surfY, buoy, linDrag, angDrag,
                             dt)) {
      floaters_.floatedBodies++;
      floaters_.floatLastSurfaceY = (float)surfY;
    }
  }
}

void DebrisSystem::SettleBodies(uint32_t tick, World& world,
                                std::vector<CellOp>& cellOps) {
  constexpr uint32_t kSettleAfterTicks = 60;   // 2 s asleep before converting
  constexpr float kAlignCos = 0.94f;           // ~20°: snap or stay a body

  // Is this body's rotation within ~20° of some axis permutation? Fills the
  // snapped integer basis when it is.
  auto alignedBasis = [&](const Body& b, int snap[3][3]) -> bool {
    const float x = b.xf.quat[0], y = b.xf.quat[1], z = b.xf.quat[2],
                w = b.xf.quat[3];
    float m[3][3] = {
        {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
        {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
        {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    for (int r = 0; r < 3; r++)
      for (int c = 0; c < 3; c++) snap[r][c] = 0;
    bool rowUsed[3] = {};
    for (int col = 0; col < 3; col++) {
      int best = 0;
      for (int row = 1; row < 3; row++)
        if (std::abs(m[row][col]) > std::abs(m[best][col])) best = row;
      if (std::abs(m[best][col]) < kAlignCos || rowUsed[best]) return false;
      rowUsed[best] = true;
      snap[best][col] = m[best][col] > 0 ? 1 : -1;
    }
    return true;
  };

  for (size_t bi = 0; bi < bodies_.size(); bi++) {
    Body& b = bodies_[bi];
    // A FOLLOWER NEVER SETTLES BACK ON ITS OWN. It is not asleep in any sense
    // the grid cares about — it is being driven — and stamping a breastplate
    // into the world while the corpse inside it went on lying there would leave
    // a plate of iron in the air with a body still tumbling through it. When
    // its HOST settles back the host's body is released, DriveStraps finds no
    // host next tick, and the garment falls and settles on its own terms.
    if (b.Follower()) {
      b.inactiveTicks = 0;
      continue;
    }
    // A GHOST MUST NEVER RE-ENTER THE GRID. Settle-back is the single most
    // destructive thing a body can do -- it writes its whole lattice into the
    // world as voxels and frees itself -- and a ghost is a render proxy for
    // matter another machine is stepping. If both copies settled, the block
    // would be stamped twice and the owner would then delete a body the ghost
    // machine still holds. It is also permanently "inactive" by construction
    // (kinematic bodies sleep), so without this it would settle FIRST.
    if (!OwnedLocally(b)) {
      b.inactiveTicks = 0;
      ownerProbe_.emittersSkipped++;
      continue;
    }
    if (phys_->IsActive(b.handle)) {
      b.inactiveTicks = 0;
      continue;
    }
    ++b.inactiveTicks;
    settle_.maxInactiveTicks =
        std::max(settle_.maxInactiveTicks, b.inactiveTicks);
    if (b.inactiveTicks < kSettleAfterTicks) continue;
    if (b.wound.open) continue;
    if (b.inactiveTicks % 30 != 0) continue;  // re-test alignment cheaply

    // THE GROUP: this body alone, or every shard of its assembly. A welded
    // island settles as a unit or not at all (PLAN §5): stamping one shard
    // back removes its body and with it the joints holding the rest, which
    // then wake, fall a little, and never line up with the half already in
    // the grid.
    std::vector<size_t> group;
    if (b.assembly == 0) {
      group.push_back(bi);
    } else {
      for (size_t j = 0; j < bodies_.size(); j++)
        if (bodies_[j].assembly == b.assembly) group.push_back(j);
    }

    struct Member {
      size_t idx = 0;
      int snap[3][3] = {};
      IVec3 base{};
      const std::vector<DebrisVoxel>* src = nullptr;
      std::vector<DebrisVoxel> down;
    };
    std::vector<Member> members;
    members.reserve(group.size());
    bool ready = true;
    size_t totalOps = 0;
    for (size_t j : group) {
      Body& m = bodies_[j];
      if (phys_->IsActive(m.handle) || m.inactiveTicks < kSettleAfterTicks ||
          m.wound.open) {
        ready = false;
        break;
      }
      Member mem;
      mem.idx = j;
      if (!alignedBasis(m, mem.snap)) {
        ready = false;
        break;
      }
      mem.src = &m.voxels;
      if (m.physScale > 1) {
        mem.down = DownsampleMicro(m.voxels, m.physScale);
        if (mem.down.empty()) {  // nothing survives: stay a body
          ready = false;
          break;
        }
        mem.src = &mem.down;
      }
      mem.base = IVec3{(int)std::lround(m.xf.pos.x), (int)std::lround(m.xf.pos.y),
                       (int)std::lround(m.xf.pos.z)};
      totalOps += mem.src->size();
      members.push_back(std::move(mem));
    }
    if (!ready) continue;
    if (cellOps.size() + totalOps > kMaxCellOpsPerTick) continue;

    // Support: a lone body needs ground under its own footprint; an assembly
    // rests as one object, so ground under ANY shard carries all of them (a
    // felled trunk's middle shard hangs between the two that touch down).
    bool supported = false;
    for (Member& mem : members)
      if (SettleFootprintSupported(*mem.src, mem.snap, mem.base, world)) {
        supported = true;
        break;
      }
    if (!supported) {
      floaters_.settleWithoutSupport++;
      for (Member& mem : members) {
        bodies_[mem.idx].inactiveTicks = 0;
        phys_->ActivateBody(bodies_[mem.idx].handle);
      }
      continue;
    }

    const size_t opsStart = cellOps.size();
    bool inWindow = true;
    std::vector<std::pair<IVec3, uint32_t>> written;
    written.reserve(totalOps);
    for (Member& mem : members) {
      const int (*snap)[3] = mem.snap;
      const IVec3 base = mem.base;
      for (const DebrisVoxel& v : *mem.src) {
        const float lx = (float)v.x + 0.5f, ly = (float)v.y + 0.5f,
                    lz = (float)v.z + 0.5f;
        const IVec3 cell{
            base.x + ifloor(snap[0][0] * lx + snap[0][1] * ly + snap[0][2] * lz),
            base.y + ifloor(snap[1][0] * lx + snap[1][1] * ly + snap[1][2] * lz),
            base.z + ifloor(snap[2][0] * lx + snap[2][1] * ly + snap[2][2] * lz)};
        if (!world.CellInWindow(cell)) {
          inWindow = false;
          break;
        }
        const uint32_t mat = (uint32_t)v.payload & 0xFFFu;
        const uint32_t state =
            GridStateFor(mat, v.color, ((uint32_t)v.payload >> 12) & 0xFu);
        const uint32_t word = PackVoxNew(mat, state) | kCellOpIfAir;
        cellOps.push_back({World::SlotCellIndex(cell), word});
        written.push_back({cell, word});
      }
      if (!inWindow) break;
    }
    if (!inWindow) {
      cellOps.resize(opsStart);
      continue;
    }

    IVec3 slo{INT32_MAX, INT32_MAX, INT32_MAX}, shi{INT32_MIN, INT32_MIN, INT32_MIN};
    for (const auto& [cell, word] : written) {
      NoteGridWrite(cell, tick, word);
      slo.x = std::min(slo.x, cell.x); shi.x = std::max(shi.x, cell.x);
      slo.y = std::min(slo.y, cell.y); shi.y = std::max(shi.y, cell.y);
      slo.z = std::min(slo.z, cell.z); shi.z = std::max(shi.z, cell.z);
    }
    // A settle stamp vacates nothing, so it raises no support flag of its
    // own; the box is scanned explicitly so whatever the body was leaning on
    // (and whatever now leans on it) gets judged.
    AddDestructionEvent(tick, slo, shi, kSupportMargin);

    std::vector<uint64_t> gone;
    gone.reserve(members.size());
    for (Member& mem : members) gone.push_back(bodies_[mem.idx].handle);
    for (uint64_t h : gone)
      for (size_t j = 0; j < bodies_.size(); j++)
        if (bodies_[j].handle == h) {
          ReleaseBody(bodies_[j]);
          bodies_[j] = std::move(bodies_.back());
          bodies_.pop_back();
          break;
        }
    instancesDirty_ = true;
    settledBack_ += (uint32_t)gone.size();
    break;  // one body (or one assembly) per tick: bounded CPU + op traffic
  }
}

// ---- body burn: fire continuity on rigidbodies ----
// A detached island is CPU state no CA pass touches, so without this a burning
// plank froze mid-flame the moment it became a body: its embers never advanced,
// never spread, never lit anything. This pass runs the SAME reaction table
// (per-material buckets, file order, first-fire-wins, per-mille chances) over
// body voxel payloads each tick:
//   - decay/emit rules advance in place: ember -> ash, ember emits fire. The
//     emitted fire and any non-solid product (ash, smoke) land in the GRID at
//     the voxel's world cell as fill-air-only CellOps — real fire voxels that
//     rise, spread, and ignite neighbors through the normal CA rules. Escaped
//     voxels leave the body, so burning debris visibly wastes away.
//   - pair rules match body-internal 6-neighbors (ember ignites the wood next
//     to it inside the body) and world cells sampled from the chunk cache
//     (already fetched for terrain meshing around every live body), so grid
//     fire licking a cold wooden body ignites it and water douses its embers.
// Grid writes ride the MutationQueue like settle-back; RNG is counter-based
// (serial, tick, voxel, rule). Idle cost is zero: bodies with no self-driven
// voxels skip unless the sim is actually moving in a chunk they overlap.
void DebrisSystem::BurnBodies(uint32_t tick, World& world,
                              std::vector<CellOp>& cellOps,
                              std::vector<ParticleSpawn>& spawns) {
  if (reactions_.empty() || bodies_.empty()) return;
  const WorldSnapshot& snap = world.Snap();
  uint32_t scanBudget = kBurnScanPerTick;
  uint32_t opsBudget = kBurnOpsPerTick;
  bool rebuiltOne = false;
  // shared across every body this tick: a forest fire breaks many bodies at
  // once, and each new body is a compound-shape build plus permanent per-tick
  // upkeep. Past the budget, fragments become particles instead.
  uint32_t newBodyBudget = kMaxNewBodiesPerTick;
  // fragment bodies split off by ShatterBody, appended after the loop (a
  // push_back into bodies_ mid-iteration would invalidate `b`)
  std::vector<Body> fragments;
  // bodies that burned below body-worthiness, erased after the loop: the
  // rotated visit order below cannot survive a mid-loop swap-remove
  std::vector<size_t> dead;
  // Per body: is a solvent in contact with it right now? Filled below, read by
  // `willScan` and by the inbound pass. See the note where it is filled.
  std::vector<uint8_t> threat;

  // FAIR SHARE OF THE SCAN BUDGET, or the tail of the list never burns.
  //
  // kBurnScanPerTick is spent in list order, and it used to be spent to the
  // last voxel by whichever bodies came first: a body took min(n, whatever is
  // left) and everything after it got `scanBudget == 0` and skipped. A corpse
  // is fifteen bodies adopted in limb order, about 14k skin voxels on the
  // human, against a 4,096 budget — so the first three or four pieces burned
  // and the other eleven (torso included, 5,102 voxels of it) kept the exact
  // ember count they died with, forever: the `corpse-burn` gate measured
  // 622 -> 622 alight over 400 ticks on the torso, 245 -> 245, 333 -> 333,
  // 160 -> 160, 396 -> 396 on the limbs behind it. On screen that is the
  // owner report of 2026-09-02: the microvoxels on the corpse stay the colour
  // they died in, glowing embers pulsing forever. The live creature had the
  // same bug on its limbs (Mob::BurnTick, "ROTATE THE START LIMB BY TICK");
  // this is the same fix for the same reason.
  //
  // Two parts. The START BODY rotates by tick, so no body is always last. And
  // each body that will scan takes at most its SHARE — the budget left divided
  // among the scanners left, with a floor so a crowd of tiny bodies does not
  // grind every one of them to a handful of voxels — and hands the remainder
  // on: a 60-voxel hand does not need a fifteenth of anything, and the torso
  // behind it gets what the hand did not use. The per-body cursor
  // (b.burnCursor) carries the scan across ticks, so a body larger than its
  // share is covered in a few ticks rather than never. Deterministic: the
  // order is a function of the tick and the body list, the rolls of the
  // (serial, voxel, tick, rule) key, and the budget only decides WHICH voxels
  // roll this tick.
  auto willScan = [&](const Body& b) {
    // A ghost is not scanned and is not counted among the scanners, so the
    // fair-share divisor below is the number of bodies that will really burn.
    // Counting ghosts would quietly starve the owner's own bodies of budget in
    // proportion to how much of the peer's battlefield is in view.
    if (!OwnedLocally(b)) return false;
    const uint32_t n = (uint32_t)(b.HasFineSkin() ? b.skinVoxels.size()
                                                  : b.voxels.size());
    if (n == 0) return false;
    if (b.activeCount > 0) return true;
    // A BODY THAT REACTS WITH ITSELF NEEDS NOTHING OUTSIDE IT. Rot creeping
    // out of a bite through a corpse's own tissue is the case: it is a pair
    // rule between two materials both ON the body, so the world can be
    // perfectly settled and the process is still live. Demanding a dirty chunk
    // for it is what stopped rot the moment the fight ended (Body::internalPair
    // carries the rule-2 argument for why this still sleeps).
    if (b.internalPair) return true;
    // Nothing alight on it: only fire in the world can change it, through a
    // pair rule (skin + hot) or a gated self rule (char relighting), and the
    // world's fire shows as a dirty chunk. A body that is all char, lying in
    // a settled world, costs these two reads and nothing else.
    if ((b.pairCount > 0 || b.scaledCount > 0) && AnyDirtyNear(b, snap, world))
      return true;
    // ...and the SOLVENT case, which is `threat[]` below: asked once per body
    // per tick and cached, because this predicate runs twice.
    return threat[&b - bodies_.data()] != 0;
  };
  const size_t nb = bodies_.size();
  const size_t startBody = (size_t)(tick % (uint32_t)nb);
  // ---- ...AND THE THING EATING IT MAY BE HOLDING PERFECTLY STILL -----------
  // A pool of acid with a corpse in it settles: nothing in the grid changes,
  // the chunk sleeps, and `AnyDirtyNear` says no forever — while the acid is
  // still right there. Dirtiness is a proxy for "something is happening
  // nearby", and for a solvent it is the wrong proxy, because the solvent
  // doing its job is the body's business and not the grid's.
  //
  // So a body with something eatable on it asks the world DIRECTLY: nine
  // cached chunk reads (ThreatNear), and only for bodies that have an inbound
  // target at all — organics, i.e. corpses and dropped cloth, never a rock.
  //
  // THE ANSWER IS ALSO A BUDGET GATE, and that is the more important half. The
  // inbound pass needs the body-local occupancy map to tell "outside me" from
  // "inside me", and building that map is O(lattice) with a hash entry per
  // voxel — 14k of them on a human torso. Keyed on `inboundCount` alone it
  // would be built every tick for every corpse on the field whether or not
  // anything was eating it, which measured as a 25x slowdown of the whole
  // selftest suite. Keyed on a solvent actually being in contact, it costs
  // nine reads and is paid only while a body is genuinely dissolving.
  threat.assign(nb, 0);
  for (size_t i = 0; i < nb; i++)
    if (bodies_[i].inboundCount > 0 && ThreatNear(bodies_[i], world))
      threat[i] = 1;
  uint32_t scanners = 0;
  for (const Body& b : bodies_)
    if (willScan(b)) scanners++;

  for (size_t k = 0; k < nb; k++) {
    const size_t bi = (startBody + k) % nb;
    Body& b = bodies_[bi];
    // Burning emits fire and smoke ops into the grid and turns embers to ash
    // in the lattice: both are the owner's to author, and the ash would
    // otherwise be applied twice to two copies that then disagree about shape.
    if (!OwnedLocally(b)) {
      ownerProbe_.emittersSkipped++;
      continue;
    }
    // THE AUTHORITATIVE LATTICE, which is not always `voxels`.
    //
    // A body with a finer skin carves `skinVoxels` and DERIVES `voxels` from it
    // by majority-fill, so burning the collider would be writing to derived
    // data — the "unowned diverging representation" failure the design
    // guidelines name, and it would be silently undone by the next re-derive.
    const bool fine = b.HasFineSkin();
    BodyLattice lat{fine ? &b.skinVoxels : nullptr, fine ? nullptr : &b.voxels,
                    fine ? b.micro.skinScale : b.physScale};
    uint32_t n = (uint32_t)lat.Size();
    bool active = b.activeCount > 0;
    // MICRO BODIES BURN. They did not in v1, for two structural reasons that
    // have both since expired: the copy-on-write brick pool shipped (so a
    // per-body edit IS visible — MicroBodyPoke rewrites one 16-bit voxel in
    // place), and this pass no longer maps body-local coordinates straight onto
    // world cells, it divides by the lattice scale like everything else here.
    //
    // Leaving them out was the visible half of "a corpse does not burn": every
    // mob limb of every rig with skinScale > 1 becomes a micro body the instant
    // it is severed or its owner dies, and so does every dropped item.
    if (!willScan(b) || scanBudget == 0) continue;
    // This body's share of what is left (see the note above the loop).
    const uint32_t share =
        std::max(kBurnScanMinShare, scanBudget / std::max(1u, scanners));
    scanners = scanners > 0 ? scanners - 1 : 0;
    // A micro body must OWN its brick before a poke can land: a shared model
    // backs every instance of its def, and charring one would char them all.
    //
    // OWNED LAZILY, AT THE FIRST WRITE. This used to happen up front, "so the
    // poke sites stay branch-free; a body that never changes pays one clone it
    // did not need, which only happens to a body the pass has already decided
    // is reactive." Reactive is not the same as CHANGING: `willScan` admits any
    // body with a pair rule and a dirty chunk anywhere near it, which during a
    // fight is every corpse on the ground. So a pile of corpses cloned fifteen
    // bricks apiece for smoke drifting past, and a clone costs BOTH ceilings —
    // a model record and ~1.5x the model's words (world.h kMaxMicroBodyModels,
    // and the owner report it is now sized against). The branch it saved is one
    // predictable test per poked voxel.
    int ownState = 0;  // 0 not tried, 1 owned, -1 refused (don't retry per voxel)
    auto poke = [&](IVec3 p, uint8_t mat) {
      if (ownState == 0) {
        if (!b.micro.Valid() || !microSet_) { ownState = -1; return; }
        const int own = MicroBodyOwn(*microSet_, b.micro.model);
        // Pool full: the body still really burns, its skin just stops keeping
        // up — and now says so once (MicroBodySet::refusals).
        if (own < 0) { ownState = -1; return; }
        b.micro.model = (uint32_t)own;
        ownState = 1;
      }
      if (ownState < 0) return;
      MicroBodyPoke(*microSet_, b.micro.model, p.x, p.y, p.z, mat, 0);
    };
    // The lattice scale is also the divisor for every world-space quantity
    // derived from a body-local coordinate. Getting this wrong does not fail
    // loudly — it puts a scale-8 limb's fire eight cells away from the limb.
    const float latInv = 1.0f / (float)std::max(1u, lat.scale);

    // rotation helpers (same quaternion sandwich as SplitBody)
    const float qx = b.xf.quat[0], qy = b.xf.quat[1], qz = b.xf.quat[2],
                qw = b.xf.quat[3];
    auto rotQ = [&](Vec3 v) {
      Vec3 u{qx, qy, qz};
      Vec3 t = u.cross(v) * 2.0f;
      return v + t * qw + u.cross(t);
    };
    auto rotInvQ = [&](Vec3 v) {
      Vec3 u{-qx, -qy, -qz};
      Vec3 t = u.cross(v) * 2.0f;
      return v + t * qw + u.cross(t);
    };
    // Body-local LATTICE coordinate -> the world cell it sits in. The division
    // by the lattice scale is what makes this correct for a micro body: a
    // scale-8 voxel is one eighth of a world cell, and mapping its coordinate
    // straight onto a cell — which is what this did while micro bodies were
    // excluded — puts a limb's fire eight cells from the limb.
    auto worldOfLocal = [&](IVec3 v) {
      return b.xf.pos + rotQ(Vec3{((float)v.x + 0.5f) * latInv,
                                  ((float)v.y + 0.5f) * latInv,
                                  ((float)v.z + 0.5f) * latInv});
    };
    auto worldCellOf = [&](IVec3 v) {
      const Vec3 wp = worldOfLocal(v);
      return IVec3{ifloor(wp.x), ifloor(wp.y), ifloor(wp.z)};
    };
    // world direction -> nearest body-local lattice offset (occlusion checks)
    auto localDirOf = [&](IVec3 d) {
      Vec3 l = rotInvQ(Vec3{(float)d.x, (float)d.y, (float)d.z});
      return IVec3{(int)std::lround(l.x), (int)std::lround(l.y),
                   (int)std::lround(l.z)};
    };
    // grid material at a world cell via the chunk cache (terrain meshing keeps
    // chunks around live bodies fetched + refreshed while they are dirty).
    // Unknown/missing reads as air: ignition is best-effort, never wrong-way.
    auto worldMatAt = [&](IVec3 c) -> uint32_t {
      if (!world.CellInWindow(c)) return 0u;
      const CachedChunk* cc = world.Cached(ChunkOfCell(c.x, c.y, c.z));
      if (!cc || cc->voxels.size() != kChunkVol) return 0u;
      uint32_t lx = (uint32_t)(c.x & 15), ly = (uint32_t)(c.y & 15),
               lz = (uint32_t)(c.z & 15);
      return cc->voxels[(lz * kChunk + ly) * kChunk + lx] & 0xFFFu;
    };
    auto nbrMatches = [&](uint32_t nm, const ReactionGpu& r) -> bool {
      return ReactNbrMatches(r, nm, matGpu_);
    };

    // Does any material present in this body use the neighbour-count ramp? If
    // so the local occupancy map has to exist even on an inactive body, or the
    // count would read every direction as world content.
    bool anyScaled = false;
    for (uint32_t i = 0; i < n && !anyScaled; i++) {
      uint32_t m = lat.Mat(i);
      if (m < matHasScaled_.size() && matHasScaled_[m]) anyScaled = true;
    }
    // local occupancy for internal spread; values are voxel indices, entries
    // whose payload was zeroed this pass read as absent
    //
    // THE INBOUND PASS NEEDS IT TOO, and needs it for a reason worth stating:
    // without the map every direction reads as "world", so an INTERIOR voxel
    // of a body lying in acid would find acid on all six faces and the whole
    // torso would dissolve at once instead of from its surface inward. The map
    // is what makes "outside me" mean outside me.
    //
    // ...but ONLY while something is actually eating it (`threat`), never
    // merely because it is edible. See where `threat` is filled: the
    // difference between those two conditions was a 25x suite slowdown.
    const bool inbound = threat[bi] != 0;
    const bool haveLocal = active || anyScaled || inbound;
    std::unordered_map<uint32_t, uint32_t> local;
    if (haveLocal) {
      local.reserve(n * 2);
      for (uint32_t i = 0; i < n; i++) {
        const IVec3 p = lat.At(i);
        local[LocalKey(p.x, p.y, p.z)] = i;
      }
    }
    auto localMatAt = [&](int x, int y, int z) -> uint32_t {
      if (!haveLocal) return 0u;
      auto it = local.find(LocalKey(x, y, z));
      if (it == local.end()) return 0u;
      return lat.Mat(it->second);
    };

    uint32_t removed = 0;
    bool changed = false;
    // rewrite a voxel to a rule product. Solids swap in place; anything else
    // (ash, smoke, fire, air) escapes into the grid at the voxel's world cell
    // and the voxel leaves the body.
    auto applyProduct = [&](uint32_t vi, uint32_t prod, uint32_t rr) {
      if (prod == kProdKeep) return;
      const IVec3 p = lat.At(vi);
      uint32_t pm = prod & 0xFFFu;
      if (pm != 0 && pm < matGpu_.size() && matGpu_[pm].klass == CLASS_SOLID) {
        lat.Set(vi, pm, (rr >> 6u) % 3u);
        // One 16-bit word of one model's block. NOT ReskinMicro, which
        // re-derives dims, rebases the origin and re-packs the whole payload —
        // right for a carve (the shape changed), catastrophic per tick for a
        // state change (the shape did not; one voxel's material did).
        poke(p, (uint8_t)pm);
      } else {
        if (pm != 0 && pm < matGpu_.size() && opsBudget > 0 &&
            cellOps.size() < kMaxCellOpsPerTick) {
          IVec3 cell = worldCellOf(p);
          if (world.CellInWindow(cell)) {
            // THIS VOXEL'S OWN MATTER leaving the body (ash off a burning robe,
            // blood off a shattering limb) — not the emission in the branch
            // below, which puts fire into a neighbouring AIR cell and has no
            // colour to inherit. So the dye follows it: quantize the voxel's art
            // colour to the PRODUCT's tints, since a purple robe's ash is
            // whatever ash a purple robe leaves, not whatever cloth is nearest.
            uint32_t state = matGpu_[pm].klass == CLASS_LIQUID
                                 ? 7u  // LIQ_FULL_STATE
                                 : GridStateFor(pm, lat.Color(vi),
                                                (rr >> 6u) % 3u);
            cellOps.push_back({World::SlotCellIndex(cell),
                               PackVoxNew(pm, state) | kCellOpIfAir});
            opsBudget--;
          }
        }
        lat.Set(vi, 0, 0);  // compacted below
        poke(p, 0);
        removed++;
      }
      changed = true;
    };

    const IVec3 kDirs[6] = {{0, 1, 0}, {0, -1, 0}, {1, 0, 0},
                            {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
    // body-local lattice offset -> nearest WORLD direction (the inverse of
    // localDirOf, for the neighbour-count ramp's world fallback)
    auto worldDirOf = [&](IVec3 d) {
      Vec3 w = rotQ(Vec3{(float)d.x, (float)d.y, (float)d.z});
      return IVec3{(int)std::lround(w.x), (int)std::lround(w.y),
                   (int)std::lround(w.z)};
    };
    // The six face neighbours as scaleByNeighbors counts them: the body's own
    // lattice first, and the world cell in that direction when the lattice has
    // nothing there — because a SURFACE voxel's neighbours genuinely are grid
    // cells, and treating them as air would make every rule that ramps on
    // "matching neighbours" read a body's whole skin as isolated.
    auto countMatches = [&](IVec3 v, const ReactionGpu& r) {
      const bool invert = ReactScaleInverted(r);
      uint32_t count = 0;
      // ---- ...AND HOW WIDE THE FIRE OUTSIDE IS (2026-09-19) ---------------
      // The widest single WORLD-pitch face found below. A corpse is the same
      // scale-8 lattice a live limb is, and the whole argument
      // MobSystem::BurnOneLimb makes about this applies here word for word:
      // five of a skin sub-voxel's six faces are its own flesh however large
      // the fire outside it is, so a lattice count reaches flesh's authored
      // minCount 3 only on a convex CORNER. Measured on the live creature
      // before the same fix: 92% of every ignition roll refused, 11 of 22,775
      // flesh voxels lost over a 90-tick immersion. A CORPSE HAD NOTHING
      // EQUIVALENT — which is the owner report that corpses do not really
      // burn, and it is the same bug one population later.
      //
      // So a face pointing into matter the rule reacts to counts for as much
      // as that matter is WIDE across the face (1..5), taken as a MAX against
      // the lattice count rather than a sum: the two are the same quantity
      // measured at two pitches and adding them would double-count the cell
      // that produced the face in the first place. One cell of flame is still
      // 1 and still gutters out; a wall of flame is 5 and takes hold. Direct
      // ramps only, for the reason the live pass gives: an INVERTED ramp is
      // counting what is NOT there and the wide reading is a different
      // question.
      uint32_t widest = 0;
      for (const IVec3& d : kDirs) {
        uint32_t nm = localMatAt(v.x + d.x, v.y + d.y, v.z + d.z);
        bool fromWorld = false;
        IVec3 wc{};
        if (nm == 0) {
          // ONE LATTICE STEP, not one world cell: on a scale-8 body a whole
          // cell steps over eight of the body's own voxels and reads a
          // neighbourhood the voxel is nowhere near.
          const Vec3 wv = worldOfLocal(v) + rotQ(Vec3{(float)d.x * latInv,
                                                      (float)d.y * latInv,
                                                      (float)d.z * latInv});
          wc = IVec3{ifloor(wv.x), ifloor(wv.y), ifloor(wv.z)};
          nm = worldMatAt(wc);
          fromWorld = nm != 0;
        }
        const bool match = ReactNbrMatches(r, nm, matGpu_);
        if (match != invert) count++;
        if (invert || !fromWorld || !match) continue;
        // The tangential ring around that world cell, at WORLD pitch. The
        // direction the face points is `d` in LATTICE space; the ring is the
        // four world axes perpendicular to where that points in the world.
        const IVec3 wd = worldDirOf(d);
        uint32_t w = 1;  // the face's own cell, already known to match
        for (const IVec3& e : kDirs) {
          if (e.x * wd.x + e.y * wd.y + e.z * wd.z != 0) continue;  // parallel
          if (ReactNbrMatches(r, worldMatAt({wc.x + e.x, wc.y + e.y,
                                             wc.z + e.z}), matGpu_))
            w++;
        }
        widest = std::max(widest, w);
      }
      return std::max(count, widest);
    };
    uint32_t steps = std::min(n, std::min(share, scanBudget));
    scanBudget -= steps;
    for (uint32_t s = 0; s < steps; s++) {
      uint32_t vi = (b.burnCursor + s) % n;
      const IVec3 v = lat.At(vi);
      uint32_t m = lat.Mat(vi);
      if (m == 0 || m >= matGpu_.size()) continue;
      const MaterialGpu& mg = matGpu_[m];
      // WHETHER THE SELF PASS RUNS AT ALL is a separate question from whether
      // the INBOUND pass below does, and conflating them is what made bone
      // (which authors no rules whatsoever, on purpose) immune to acid on a
      // corpse while dissolving on the creature it came off. A voxel with
      // nothing to say still has something that can be said to it.
      bool fired = false;
      const bool runSelf = mg.reactCount != 0 &&
                           (active || matHasPair_[m] || matSelfScaled_[m]);

      for (uint32_t ri = 0; runSelf && ri < mg.reactCount; ri++) {
        const ReactionGpu& r = reactions_[mg.reactOffset + ri];
        uint32_t kind = r.packed & 3u;
        uint32_t dmask = (r.packed >> 2u) & 7u;
        // The two gates sim_step.wgsl applies before the roll, in the same
        // order. Both were silently absent on this side until sim/reactcpu.h —
        // a rule authored with a day/night condition or a neighbour-count ramp
        // fired unconditionally at base chance on a body. See that header.
        //
        // seesSky = true: a rigidbody has no column to raycast, and refusing
        // instead would make a sky-gated rule permanently inert on bodies.
        if (!ReactLightMatches(r, dayPhase_, /*seesSky=*/true)) continue;
        uint32_t chance = r.chance;
        if (ReactScaleArmed(r)) {
          chance = ReactScaledChance(r, countMatches(v, r));
          if (chance == 0) continue;  // below minCount: no frontier, no rule
        }
        uint32_t rr = Hash3(b.serial * 0x9E3779B9u + vi, tick, ri);
        // one roll per rule, GPU-style — chance is in 1/kReactChanceDen units,
        // so this must use the same denominator sim_step.wgsl rolls against
        if (rr % kReactChanceDen >= chance) continue;
        fired = false;

        if (kind == kReactDecay) {
          applyProduct(vi, r.prodSelf, rr);
          fired = true;
        } else if (kind == kReactEmit) {
          // emit in an allowed WORLD direction (fire rises in world space no
          // matter how the body tumbles), first direction not occluded by the
          // body itself; IfAir lets grid content win
          IVec3 cand[6];
          int nc = 0;
          if (dmask & kDirUp) cand[nc++] = {0, 1, 0};
          if (dmask & kDirDown) cand[nc++] = {0, -1, 0};
          if (dmask & kDirSide) {
            const IVec3 side[4] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
            uint32_t rot = rr >> 12u;
            for (int k = 0; k < 4; k++) cand[nc++] = side[(rot + k) & 3u];
          }
          IVec3 wc0 = worldCellOf(v);
          for (int k = 0; k < nc; k++) {
            IVec3 ld = localDirOf(cand[k]);
            if (localMatAt(v.x + ld.x, v.y + ld.y, v.z + ld.z) != 0) continue;
            IVec3 t{wc0.x + cand[k].x, wc0.y + cand[k].y, wc0.z + cand[k].z};
            if (world.CellInWindow(t) && opsBudget > 0 &&
                cellOps.size() < kMaxCellOpsPerTick) {
              cellOps.push_back({World::SlotCellIndex(t),
                                 PackVoxNew(r.prodNbr, (rr >> 8u) % 3u) |
                                     kCellOpIfAir});
              opsBudget--;
            }
            fired = true;  // rule consumed even if the write missed the budget
            break;
          }
        } else {  // kReactPair
          // body-internal neighbors first (ember ignites adjacent wood inside
          // the plank), then the voxel's own world cell + 6 world neighbors
          // (grid fire drifts into / around the body's footprint)
          int matched = -2;  // -2 none, -1 world, >=0 internal voxel index
          if (haveLocal) {
            for (const IVec3& d : kDirs) {
              uint32_t nm = localMatAt(v.x + d.x, v.y + d.y, v.z + d.z);
              if (nm != 0 && nbrMatches(nm, r)) {
                matched = (int)local[LocalKey(v.x + d.x, v.y + d.y, v.z + d.z)];
                break;
              }
            }
          }
          if (matched == -2 && r.prodNbr == kProdKeep) {
            // world neighbors are read-only: only rules that keep the
            // neighbor are eligible (a body cannot rewrite grid content)
            const Vec3 wp = worldOfLocal(v);
            IVec3 wc{ifloor(wp.x), ifloor(wp.y), ifloor(wp.z)};
            if (nbrMatches(worldMatAt(wc), r)) {
              matched = -1;
            } else {
              for (const IVec3& d : kDirs) {
                // ONE LATTICE STEP (see countMatches): on a scale-8 limb a
                // whole-cell step reads eight voxels past its own surface.
                const Vec3 wn = wp + rotQ(Vec3{(float)d.x * latInv,
                                               (float)d.y * latInv,
                                               (float)d.z * latInv});
                if (nbrMatches(worldMatAt({ifloor(wn.x), ifloor(wn.y),
                                           ifloor(wn.z)}), r)) {
                  matched = -1;
                  break;
                }
              }
            }
          }
          if (matched != -2) {
            applyProduct(vi, r.prodSelf, rr);
            if (matched >= 0 && r.prodNbr != kProdKeep)
              applyProduct((uint32_t)matched, r.prodNbr, Pcg(rr));
            fired = true;
          }
        }
        if (fired) {
          changed = true;
          break;  // at most one rule per voxel per tick, file order
        }
      }

      // ---- 2. INBOUND: a world neighbour's rule that rewrites ME -----------
      //
      // THE DIRECTION A RIGIDBODY COULD NOT BE ACTED ON FROM, until now. Acid
      // is authored as `acid + tag:organic -> neighborBecomes air`, i.e. from
      // the ACID's side, which is the direction the GPU evaluates it over the
      // grid and the direction MobSystem::BurnOneLimb evaluates it over a live
      // limb. The pair branch above can only ever match a world cell whose
      // rule KEEPS its neighbour — a body may not rewrite grid content — so
      // every rule shaped like acid's fell straight through it.
      //
      // The consequence was the owner report of 2026-09-19 in one line: a
      // creature standing in acid dissolved and its corpse, which is the same
      // fifteen limbs one function call later, did not. Mirroring the rules
      // per body material would be an N x M table whose two halves drift, so
      // this is the same shape mob.cpp uses — evaluate the neighbour's own
      // rule, in the neighbour's own direction, from the one table.
      //
      // WORLD NEIGHBOURS ONLY. A body-internal neighbour was already offered
      // its rewrite by the pair branch above (`matched >= 0`), and running it
      // again here would give one voxel two chances at the same rule.
      if (!fired && inbound && m < matInboundTarget_.size() &&
          matInboundTarget_[m]) {
        const Vec3 wp = worldOfLocal(v);
        for (const IVec3& d : kDirs) {
          if (fired) break;
          // ONE LATTICE STEP, for the reason countMatches gives.
          if (localMatAt(v.x + d.x, v.y + d.y, v.z + d.z) != 0) continue;
          const Vec3 wn = wp + rotQ(Vec3{(float)d.x * latInv,
                                         (float)d.y * latInv,
                                         (float)d.z * latInv});
          const uint32_t wm =
              worldMatAt({ifloor(wn.x), ifloor(wn.y), ifloor(wn.z)});
          if (wm == 0 || wm >= matRewritesNbr_.size() || !matRewritesNbr_[wm])
            continue;
          const MaterialGpu& wg = matGpu_[wm];
          for (uint32_t rj = 0; rj < wg.reactCount; rj++) {
            const ReactionGpu& r = reactions_[wg.reactOffset + rj];
            if ((r.packed & 3u) != kReactPair || r.prodNbr == kProdKeep)
              continue;
            if (!ReactNbrMatches(r, m, matGpu_)) continue;
            if (!ReactLightMatches(r, dayPhase_, /*seesSky=*/true)) continue;
            // A distinct rule-index space (+64) from the self pass, the same
            // separation BurnOneLimb draws: a voxel that is both burning and
            // dissolving must not roll one stream twice and correlate them.
            const uint32_t rr =
                Hash3(b.serial * 668265263u + vi, tick, 64u + rj);
            if (rr % kReactChanceDen >= r.chance) continue;
            applyProduct(vi, r.prodNbr, rr);
            fired = true;
            changed = true;
            break;
          }
        }
      }
    }
    b.burnCursor = n > 0 ? (b.burnCursor + steps) % n : 0;

    if (removed) {
      // Compact the AUTHORITATIVE lattice, then re-derive the collider from it.
      // Data flows skin -> collider and never the other way, so a fine-skinned
      // body must not have its `voxels` edited here: the next re-derive would
      // silently undo it.
      if (fine) {
        b.skinVoxels.erase(
            std::remove_if(b.skinVoxels.begin(), b.skinVoxels.end(),
                           [](const PrefabVoxel& v) {
                             return (v.material & 0xFFFu) == 0;
                           }),
            b.skinVoxels.end());
        DeriveColliderFromSkin(b);
      } else {
        b.voxels.erase(
            std::remove_if(b.voxels.begin(), b.voxels.end(),
                           [](const DebrisVoxel& v) { return v.payload == 0; }),
            b.voxels.end());
      }
      b.burnedSinceRebuild += removed;
      const uint32_t latNow = (uint32_t)lat.Size();
      if (latNow) b.burnCursor %= latNow;
      // removals can disconnect the remainder: split fragments off (bodies /
      // ballistic particles) before recounting. The connectivity flood is O(n)
      // over the body, so it waits until enough matter has actually burned
      // away to plausibly disconnect anything — a body losing one voxel a tick
      // to embers doesn't re-flood every tick.
      // Threshold scales with the body: losing 4 voxels out of 13 can easily
      // sever it, out of 2000 it cannot. Small bodies therefore re-check
      // immediately (correctness — a clump must detach before the remainder
      // burns under the dissolve floor), big ones amortize (perf).
      b.burnedSinceShatter += removed;
      uint32_t shatterEvery =
          std::max(1u, std::min(kShatterCheckVoxels,
                                (uint32_t)b.voxels.size() / 16u));
      if (b.burnedSinceShatter >= shatterEvery) {
        b.burnedSinceShatter = 0;
        ShatterBody(b, world, fragments, spawns, kMinBurnFragmentVoxels,
                    newBodyBudget);
      }
    }
    if (changed) {
      RecountBurn(b);
      instancesDirty_ = true;
      // NOTE: burn ops deliberately do NOT bump lastCellWriteTick_. They are
      // additive fill-air-only writes a stale island scan can safely miss;
      // bumping it every burning tick would hold EventReady's required
      // version at the current tick forever and starve island detection.
    }

    // burned/broken below body-worthiness: the remainder re-enters the world
    // as ballistic voxels carrying the body's momentum (the moving-body
    // analogue of the <8-voxel island rubble handoff). Gated on THIS pass
    // having removed voxels — a small body that isn't burning (a 4-voxel
    // split half, a mob hand) is legitimate and must persist.
    if (removed > 0 && b.voxels.size() < 8) {
      Vec3 lin{}, ang{};
      phys_->GetBodyVelocities(b.handle, lin, ang);
      VoxelsToParticles(b, b.voxels, lin, ang, world, spawns);
      ReleaseBody(b);
      b.voxels.clear();  // nothing below may burn it again this tick
      b.skinVoxels.clear();
      dead.push_back(bi);
      instancesDirty_ = true;
      continue;
    }

    // batched collider refresh: the charred shape sheds its burned voxels
    // (at most one Jolt rebuild per tick across all bodies)
    if (b.burnedSinceRebuild >= kBurnRebuildVoxels && !rebuiltOne) {
      // Re-pack the brick at the SAME batched cadence. The per-voxel pokes
      // above already made the burn visible; this only shrinks the OBB the
      // fragment shader marches, so it belongs on the expensive-work clock, not
      // on the per-voxel one. It may also rebase the origin, which is why it
      // runs BEFORE the collider is rebuilt from those voxels.
      if (b.micro.Valid()) ReskinMicro(b);
      Vec3 lin{}, ang{};
      phys_->GetBodyVelocities(b.handle, lin, ang);
      // Micro bodies build at 1/physScale: a scale-4 body's voxels are quarter
      // size, and building at pitch 1 would give it 64x its real volume and
      // mass (RebuildCollider says the same thing at the other call site).
      uint64_t nh = phys_->CreateDebrisBodyXf(
          b.voxels, b.xf, densityOf_, /*allowKinematic=*/b.Follower(),
          1.0f / (float)std::max(1u, b.physScale));
      if (nh != 0) {
        // ReplaceBody, never RemoveBody: a corpse's joints ride to the new
        // handle (see RebuildCollider).
        const uint64_t oh = b.handle;
        phys_->ReplaceBody(b.handle, nh);
        b.handle = nh;
        CarryStrap(oh, nh);
        phys_->SetBodyVelocities(nh, lin, ang);
        b.burnedSinceRebuild = 0;
        rebuiltOne = true;
      }
    }
  }

  // Erase the dead, highest index first so each swap-remove moves a body that
  // is not itself waiting to be erased.
  std::sort(dead.begin(), dead.end());
  for (size_t i = dead.size(); i-- > 0;) {
    const size_t bi = dead[i];
    bodies_[bi] = std::move(bodies_.back());
    bodies_.pop_back();
  }
  for (Body& f : fragments) {
    bodies_.push_back(std::move(f));
    instancesDirty_ = true;
  }
}

void DebrisSystem::ShatterBody(Body& b, World& world, std::vector<Body>& fragments,
                               std::vector<ParticleSpawn>& spawns,
                               uint32_t minFragment, uint32_t& budget,
                               bool fineConnectivity) {
  // ---- CONNECTIVITY IS DECIDED ON THE AUTHORITATIVE LATTICE (2026-09-19) ---
  //
  // It used to be decided on the COLLIDER, always, with a fine skin joining
  // whichever collider block each of its voxels sat in — "running a second
  // connectivity pass on the skin would be both slower and wrong", on the
  // grounds that the two could disagree about how many pieces there are.
  //
  // They can disagree, and when they do the SKIN is the one that is right,
  // because the skin is what carving edits and what the player sees. The
  // collider is a majority-fill of it: at skinScale 8 over physScale 2, a
  // block reads as solid when 32 of its 64 skin cells are, so a kerf four skin
  // voxels wide — a sword's cut, all the way through a neck — leaves every
  // collider block along the cut still filled and the body still one
  // component. That is the owner report of 2026-09-19, exactly: "you can sever
  // the entire bottom half of a head but the top half stays and it doesn't
  // actually detach". The art was cut through; the lattice the split was
  // measured on was not.
  //
  // So the flood runs on the skin when there is one, and each fragment's
  // COLLIDER is derived from its own skin part (DownsampleSkin) rather than
  // partitioned alongside it. The two cannot disagree about how many pieces
  // there are, because only one of them is asked — the same skin-is-
  // authoritative rule DamageBody and DeriveColliderFromSkin already follow.
  //
  // COST: one flood over the fine lattice instead of the coarse one, on a
  // carve or every kShatterCheckVoxels burned away, never per tick.
  const uint32_t ratio =
      b.HasFineSkin() ? std::max(1u, b.micro.skinScale / b.physScale) : 1u;
  // 21 bits per axis, like DownsampleSkin's: the coarse lattice fits in
  // LocalKey's byte per axis but a skin one does not (a human torso is ~240
  // skin voxels tall), and a packed-byte key would silently alias two ends of
  // it into one component.
  auto key64 = [](int x, int y, int z) -> uint64_t {
    return ((uint64_t)(uint32_t)(x + 1024) << 42) |
           ((uint64_t)(uint32_t)(y + 1024) << 21) |
           (uint64_t)(uint32_t)(z + 1024);
  };
  // 6-connected components over whichever lattice is being asked. Returns the
  // component count; `comp` is per-voxel and `compSize` per component.
  std::vector<int32_t> comp;
  std::vector<uint32_t> compSize;
  std::vector<uint32_t> stack;
  // THE LOOKUP IS A DENSE GRID over the lattice's bounding box whenever that
  // box is a sane size, and the hash map only past it (a sparse fine skin can
  // span far more cells than it holds). This runs on EVERY carve -- each probe
  // of a sword stroke that lands on a body -- and on a felled 35k-voxel tree
  // the hash map made it most of a 7-27 ms CutBody (2026-09-22, --fell-tree);
  // an index grid is the same flood, the same seed order and therefore the
  // same component numbering, at array speed.
  constexpr int64_t kDenseFloodCells = 4 << 20;  // 16 MiB of int32
  std::vector<int32_t> denseIdx;
  auto flood = [&](uint32_t count, auto posAt) {
    comp.assign(count, -1);
    compSize.clear();
    if (count == 0) return 0u;
    IVec3 mn = posAt(0), mx = mn;
    for (uint32_t i = 1; i < count; i++) {
      const IVec3 p = posAt(i);
      mn.x = std::min(mn.x, p.x); mn.y = std::min(mn.y, p.y); mn.z = std::min(mn.z, p.z);
      mx.x = std::max(mx.x, p.x); mx.y = std::max(mx.y, p.y); mx.z = std::max(mx.z, p.z);
    }
    const int64_t gx = (int64_t)mx.x - mn.x + 1, gy = (int64_t)mx.y - mn.y + 1,
                  gz = (int64_t)mx.z - mn.z + 1;
    const bool dense = gx * gy * gz <= kDenseFloodCells;
    std::unordered_map<uint64_t, uint32_t> map;
    if (dense) {
      denseIdx.assign((size_t)(gx * gy * gz), -1);
      for (uint32_t i = 0; i < count; i++) {
        const IVec3 p = posAt(i);
        denseIdx[(size_t)(((int64_t)(p.z - mn.z) * gy + (p.y - mn.y)) * gx +
                          (p.x - mn.x))] = (int32_t)i;
      }
    } else {
      map.reserve(count * 2);
      for (uint32_t i = 0; i < count; i++) {
        const IVec3 p = posAt(i);
        map[key64(p.x, p.y, p.z)] = i;
      }
    }
    auto indexAt = [&](int x, int y, int z) -> int32_t {
      if (dense) {
        if (x < mn.x || y < mn.y || z < mn.z || x > mx.x || y > mx.y || z > mx.z)
          return -1;
        return denseIdx[(size_t)(((int64_t)(z - mn.z) * gy + (y - mn.y)) * gx +
                                 (x - mn.x))];
      }
      auto it = map.find(key64(x, y, z));
      return it == map.end() ? -1 : (int32_t)it->second;
    };
    for (uint32_t seed = 0; seed < count; seed++) {
      if (comp[seed] != -1) continue;
      int32_t c = (int32_t)compSize.size();
      uint32_t size = 0;
      stack.assign(1, seed);
      comp[seed] = c;
      while (!stack.empty()) {
        uint32_t i = stack.back();
        stack.pop_back();
        size++;
        const IVec3 v = posAt(i);
        const int d[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                             {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (auto& dd : d) {
          const int32_t j = indexAt(v.x + dd[0], v.y + dd[1], v.z + dd[2]);
          if (j >= 0 && comp[(size_t)j] == -1) {
            comp[(size_t)j] = c;
            stack.push_back((uint32_t)j);
          }
        }
      }
      compSize.push_back(size);
    }
    return (uint32_t)compSize.size();
  };

  // THE COLLIDER FIRST, because it is the cheap one and it is usually right:
  // anything that erodes a body (fire, acid, a blast) removes matter at every
  // scale and parts the coarse lattice too.
  if (b.voxels.size() >= 2)
    flood((uint32_t)b.voxels.size(), [&](uint32_t i) {
      return IVec3{b.voxels[i].x, b.voxels[i].y, b.voxels[i].z};
    });
  bool fine = false;
  if (compSize.size() <= 1 && fineConnectivity && b.HasFineSkin() &&
      b.skinVoxels.size() >= 2) {
    // ...AND THE SKIN WHEN A BLADE WENT THROUGH IT. The collider is a
    // majority-fill of the skin: at skinScale 8 over physScale 4 a block reads
    // as solid when 32 of its 64 skin cells are, so a kerf a few skin voxels
    // wide — a sword's cut, all the way through a neck — leaves every block
    // along the cut still filled and the body still one component. That is the
    // owner report of 2026-09-19 exactly: "you can sever the entire bottom
    // half of a head but the top half stays and it doesn't actually detach".
    // The art was cut through; the lattice the split was measured on was not.
    //
    // When the skin disagrees, the SKIN is right — it is what carving edits
    // and what the player sees — so the partition is taken from it and each
    // fragment's collider is DERIVED from its own skin part below. The two
    // cannot then disagree about how many pieces there are, because only one
    // of them is asked.
    fine = true;
    flood((uint32_t)b.skinVoxels.size(), [&](uint32_t i) {
      return IVec3{b.skinVoxels[i].x, b.skinVoxels[i].y, b.skinVoxels[i].z};
    });
  }
  if (compSize.size() <= 1) return;
  const uint32_t n = (uint32_t)(fine ? b.skinVoxels.size() : b.voxels.size());

  uint32_t keep = 0;
  for (uint32_t c = 1; c < compSize.size(); c++)
    if (compSize[c] > compSize[keep]) keep = c;

  // The partition, on whichever lattice answered. `parts` is always the
  // collider one — every consumer below (the fragment's Jolt shape, the
  // particle handoff, the size floor) speaks in collider voxels — so on a fine
  // body it is DERIVED from the skin part, component by component.
  std::vector<std::vector<PrefabVoxel>> skinParts;
  std::vector<std::vector<DebrisVoxel>> parts(compSize.size());
  if (fine) {
    skinParts.resize(compSize.size());
    for (uint32_t c = 0; c < compSize.size(); c++)
      skinParts[c].reserve(compSize[c]);
    for (uint32_t i = 0; i < n; i++)
      skinParts[comp[i]].push_back(b.skinVoxels[i]);
    bool overflow = false;
    for (uint32_t c = 0; c < compSize.size(); c++)
      parts[c] = DownsampleSkin(skinParts[c], ratio, &overflow);
  } else {
    for (uint32_t c = 0; c < compSize.size(); c++) parts[c].reserve(compSize[c]);
    for (uint32_t i = 0; i < n; i++) parts[comp[i]].push_back(b.voxels[i]);
    // THE COLLIDER ANSWERED, so a fine skin follows its partition: each skin
    // voxel joins whichever collider block it sits in. Unchanged from before
    // the escalation above existed, and it is the right shape for the case it
    // still serves — the coarse lattice really did come apart, so the two
    // cannot disagree about the piece count and the cheap mapping is exact.
    if (b.HasFineSkin()) {
      std::unordered_map<uint64_t, uint32_t> compOf;
      compOf.reserve(n * 2);
      for (uint32_t i = 0; i < n; i++)
        compOf[key64(b.voxels[i].x, b.voxels[i].y, b.voxels[i].z)] = comp[i];
      skinParts.resize(compSize.size());
      for (const PrefabVoxel& sv : b.skinVoxels) {
        auto it = compOf.find(key64((int)sv.x / (int)ratio,
                                    (int)sv.y / (int)ratio,
                                    (int)sv.z / (int)ratio));
        // A skin voxel whose collider block did not survive majority-fill has
        // no component to join; it is interior detail of a block that reads as
        // air, and dropping it keeps skin and collider describing one object.
        if (it != compOf.end()) skinParts[it->second].push_back(sv);
      }
    }
  }
  // A SKIN COMPONENT CAN MAJORITY-FILL TO NOTHING: a four-voxel splinter of a
  // scale-8 skin is far short of half a collider block, so its derived lattice
  // is empty. It still has to GO somewhere — leaving it on the parent would
  // put art on a body whose collider does not cover it — so the skin follows
  // whatever its own component became, and an empty one becomes particles
  // below like any under-floor piece.
  if (fine && parts[keep].empty()) {
    // ...and if the largest piece is the one that vanished, there is nothing
    // to split: leave the body exactly as it was rather than dissolving it.
    for (uint32_t c = 0; c < parts.size(); c++)
      if (!parts[c].empty()) { keep = c; break; }
    if (parts[keep].empty()) return;
  }

  Vec3 lin{}, ang{};
  phys_->GetBodyVelocities(b.handle, lin, ang);
  b.voxels = std::move(parts[keep]);
  if (!skinParts.empty()) b.skinVoxels = std::move(skinParts[keep]);
  bool madeBody = false;

  for (uint32_t c = 0; c < (uint32_t)parts.size(); c++) {
    if (c == keep) continue;
    if (parts[c].size() >= minFragment && budget > 0 &&
        bodies_.size() + fragments.size() < kMaxBodies) {
      // body-worthy fragment: its own body at the same pose, rebased to its
      // min corner (like SplitBody halves), keeping the parent's momentum
      IVec3 mn{127, 127, 127};
      for (const DebrisVoxel& v : parts[c]) {
        mn.x = std::min<int>(mn.x, v.x);
        mn.y = std::min<int>(mn.y, v.y);
        mn.z = std::min<int>(mn.z, v.z);
      }
      for (DebrisVoxel& v : parts[c]) {
        v.x = (int8_t)(v.x - mn.x);
        v.y = (int8_t)(v.y - mn.y);
        v.z = (int8_t)(v.z - mn.z);
      }
      Body nb;
      nb.xf = b.xf;
      // A fragment of a micro body is itself a micro body: it inherits the
      // scale so its collider pitch, radius and particle conversion all stay
      // right, and gets its OWN brick below (the parent's brick still shows
      // the whole original shape, so sharing it would draw each half as the
      // intact object). If that brick can't be allocated the fragment cannot
      // be drawn at all — the cube path would render it at scale-1 size,
      // twice too big — so it falls through to particles instead, which is
      // the same handoff every under-floor piece already takes.
      //
      // This is a model INDEX, not a brick: it is CLONED below, before the
      // re-skin, and see the note there for why the re-skin cannot be trusted
      // to do it.
      nb.micro = b.micro;
      nb.physScale = b.physScale;  // MUST precede the pitch below
      nb.bleedMat = b.bleedMat;
      nb.dead = b.dead;  // half a corpse is still a corpse
      nb.defIndex = b.defIndex;   // ...and still that creature's flesh
      // The fragment's skin rebases by the SAME corner, expressed in skin
      // units. Both lattices must land on one origin or the art slides off the
      // collider — the same agreement ReskinMicro maintains after a carve.
      if (!skinParts.empty()) {
        const int ratio = (int)std::max(1u, b.micro.skinScale / b.physScale);
        nb.skinVoxels = std::move(skinParts[c]);
        for (PrefabVoxel& sv : nb.skinVoxels) {
          sv.x = (int16_t)(sv.x - mn.x * ratio);
          sv.y = (int16_t)(sv.y - mn.y * ratio);
          sv.z = (int16_t)(sv.z - mn.z * ratio);
        }
      }
      // IN WORLD VOXELS, WHICH `mn` IS NOT. The rebase moved the fragment's
      // origin by `mn` COLLIDER voxels, and a collider voxel is 1/physScale of
      // a world one — so a scale-2 fragment used to be teleported twice as far
      // as its lattice actually moved. Latent until this commit because a
      // fine-skinned body's collider never came apart (see the connectivity
      // note above): the only bodies that reached here were physScale 1, where
      // the divisor is 1 and the two readings coincide.
      const float mnInv = 1.0f / (float)std::max(1u, nb.physScale);
      nb.xf.pos += QuatRot(b.xf.quat, Vec3{(float)mn.x * mnInv,
                                           (float)mn.y * mnInv,
                                           (float)mn.z * mnInv});
      const float pitch = 1.0f / (float)std::max(1u, nb.physScale);
      nb.handle =
          phys_->CreateDebrisBodyXf(parts[c], nb.xf, densityOf_, false, pitch);
      if (nb.handle != 0) {
        // Born where the parent is: a piece splitting off a body that is
        // still inside the player is inside the player too (CarryLayer).
        phys_->CarryLayer(b.handle, nb.handle);
        phys_->SetBodyVelocities(nb.handle, lin, ang);
        nb.voxels = std::move(parts[c]);
        float r = 0;
        for (const DebrisVoxel& v : nb.voxels)
          r = std::max(r, Vec3{(float)v.x, (float)v.y, (float)v.z}.len());
        nb.radiusVoxels = r / (float)std::max(1u, nb.physScale) + 2.0f;
        nb.serial = nextSerial_++;
        // A fragment is minted HERE, by this machine, even though the body it
        // came off may have been handed to us: the far side is told about it
        // by an announce like any other new body.
        nb.owner = nb.ownerAtCreate = localPlayerId_;
        // ---- ITS OWN BRICK, CLONED EXPLICITLY -------------------------------
        //
        // ReskinMicro reaches "a brick I may edit" through MicroBodyOwn, which
        // is a NO-OP on a model that is ALREADY OWNED — correct for the damage
        // path (the second hit on a body must not clone again) and wrong here,
        // because `nb.micro` was copied off the parent a moment ago and the
        // question for a NEW body is not "may I edit this?" but "is this mine?"
        //
        // A parent reaching this point is usually already owned: DamageBody
        // re-skins after its carve, and BurnBodies takes ownership on its first
        // per-voxel poke — which is the very case this split exists to serve.
        // Measured 2026-09-14 with a counter on this branch: ONE full
        // --selftest pass took it 29 times.
        // So the fragment kept the PARENT's record, its re-skin below rewrote
        // the parent's payload to the fragment's shape, and whichever of the
        // two was released first zeroed the dims of the brick the other was
        // still being drawn from. MicroBodyClone carries the rest of the story:
        // the freed record goes back on the free list while a live body still
        // points at it, so the damage eventually lands on an unrelated
        // creature — a living mob's torso going invisible when something else
        // on the field came apart.
        //
        // A failed clone is a failed re-skin: fall through to the undo below
        // and become particles. Drawing it through the cube path instead would
        // put a scale-2 fragment on screen at twice its size.
        bool brick = !nb.micro.Valid();
        if (!brick && microSet_) {
          const int mc = MicroBodyClone(*microSet_, nb.micro.model);
          if (mc >= 0) {
            nb.micro.model = (uint32_t)mc;
            brick = ReskinMicro(nb);
            if (!brick) MicroBodyFree(*microSet_, nb.micro.model);
          }
        }
        if (brick) {
          RecountBurn(nb);
          fragments.push_back(std::move(nb));
          madeBody = true;
          budget--;
          continue;
        }
        // Brick pool full: undo the body and let it become particles below.
        phys_->RemoveBody(nb.handle);
        parts[c] = std::move(nb.voxels);
      }
      // body creation failed: fall through to particles (coords were rebased,
      // but VoxelsToParticles reads them against nb.xf — rebuild not worth it;
      // un-rebase instead)
      for (DebrisVoxel& v : parts[c]) {
        v.x = (int8_t)(v.x + mn.x);
        v.y = (int8_t)(v.y + mn.y);
        v.z = (int8_t)(v.z + mn.z);
      }
    }
    // small clump: back to loose voxels, flying with the body's point velocity
    VoxelsToParticles(b, parts[c], lin, ang, world, spawns);
  }

  if (madeBody) {
    // fragment bodies occupy space the parent's old compound still covers:
    // rebuild the parent NOW or the ghost boxes fight the new bodies.
    //
    // The pitch is NOT optional here: it was omitted before the skin/collider
    // split, so a micro parent was rebuilt at pitch 1 and its collider snapped
    // to physScale times its real size until the next RebuildCollider. The
    // fragment path two branches up (and RebuildCollider) always passed it.
    const float pitch = 1.0f / (float)std::max(1u, b.physScale);
    uint64_t nh = phys_->CreateDebrisBodyXf(
        b.voxels, b.xf, densityOf_, /*allowKinematic=*/b.Follower(), pitch);
    if (nh != 0) {
      const uint64_t oh = b.handle;
      phys_->ReplaceBody(b.handle, nh);  // joints ride along (RebuildCollider)
      b.handle = nh;
      CarryStrap(oh, nh);  // ...and so does the strap (CarryStrap)
      phys_->SetBodyVelocities(nh, lin, ang);
      b.burnedSinceRebuild = 0;
    }
  }
}

void DebrisSystem::VoxelsToParticles(const Body& b,
                                     const std::vector<DebrisVoxel>& voxels,
                                     Vec3 lin, Vec3 ang, World& world,
                                     std::vector<ParticleSpawn>& spawns) const {
  // A micro body's voxels are 1/scale of a world voxel, so local coords must be
  // divided down to reach world space. They are also SMALLER than a particle:
  // emitting one particle per micro voxel would turn a scale-2 body into 8x the
  // matter it had. Sub-sampling on the micro lattice (one particle per world
  // cell) conserves the visible volume — the grid has no sub-voxel resolution
  // to receive the detail anyway.
  // THE LAST-RESORT EMITTER, gated at the source rather than at its callers.
  // Shatter, burn-through and the fragment-budget overflow all funnel matter
  // back into the world through here, and each of them is reached from more
  // than one place; one test on the BODY covers every route, present and
  // future, and cannot be forgotten by the next caller.
  if (!OwnedLocally(b)) return;
  const float inv = 1.0f / (float)std::max(1u, b.physScale);
  const int step = (int)std::max(1u, b.physScale);
  for (const DebrisVoxel& v : voxels) {
    if (spawns.size() >= kMaxParticleSpawnsPerTick) return;  // ring full: lost
    if (step > 1 && (((int)v.x % step) || ((int)v.y % step) || ((int)v.z % step)))
      continue;
    Vec3 wp = b.xf.pos + QuatRot(b.xf.quat, Vec3{((float)v.x + 0.5f) * inv,
                                                 ((float)v.y + 0.5f) * inv,
                                                 ((float)v.z + 0.5f) * inv});
    if (!world.CellInWindow({ifloor(wp.x), ifloor(wp.y), ifloor(wp.z)})) continue;
    // rigid point velocity (voxels/s), converted to fixed 24.8 voxels/tick
    Vec3 vel = lin + ang.cross(wp - b.xf.pos);
    ParticleSpawn s;
    s.px = (int32_t)std::lround(wp.x * 256.0f);
    s.py = (int32_t)std::lround(wp.y * 256.0f);
    s.pz = (int32_t)std::lround(wp.z * 256.0f);
    s.vx = (int32_t)std::lround(vel.x * 256.0f / 30.0f);
    s.vy = (int32_t)std::lround(vel.y * 256.0f / 30.0f);
    s.vz = (int32_t)std::lround(vel.z * 256.0f / 30.0f);
    // Same tint quantization settle-back does, and for the same reason: a
    // particle carries its payload verbatim through the ballistic pass and
    // rejoins the grid as that word, so the dye has to be in the nibble BEFORE
    // the matter leaves the body. Skipping it here is what would make a
    // shattered corpse's chunks the right colour and its spray the wrong one.
    const uint32_t pmat = (uint32_t)v.payload & 0xFFFu;
    const uint32_t pstate =
        GridStateFor(pmat, v.color, ((uint32_t)v.payload >> 12) & 0xFu);
    s.payload = (uint16_t)(pmat | (pstate << 12));
    s.flags = 1u;  // PFLAG_ALIVE
    spawns.push_back(s);
  }
}

bool DebrisSystem::AnyDirtyNear(const Body& b, const WorldSnapshot& snap,
                                World& world) const {
  if (!snap.valid || snap.dirtyFlags.empty()) return false;
  float r = b.radiusVoxels + 1.0f;
  int lo[3] = {ifloor(b.xf.pos.x - r) >> 4, ifloor(b.xf.pos.y - r) >> 4,
               ifloor(b.xf.pos.z - r) >> 4};
  int hi[3] = {ifloor(b.xf.pos.x + r) >> 4, ifloor(b.xf.pos.y + r) >> 4,
               ifloor(b.xf.pos.z + r) >> 4};
  for (int cz = lo[2]; cz <= hi[2]; cz++)
    for (int cy = lo[1]; cy <= hi[1]; cy++)
      for (int cx = lo[0]; cx <= hi[0]; cx++) {
        IVec3 wc{cx, cy, cz};
        if (!world.ChunkInWindow(wc)) continue;
        uint32_t si = World::SlotChunkIndex(wc);
        if (si < snap.dirtyFlags.size() && snap.dirtyFlags[si]) return true;
      }
  return false;
}

// ---- IS SOMETHING SITTING AGAINST THIS BODY THAT COULD EAT IT? -------------
//
// The still-pool answer to AnyDirtyNear (see willScan). Nine world cells —
// the body's own centre and the eight corners of its local lattice box, all
// transformed into world space — read through the chunk cache and tested
// against matRewritesNbr_. Not a containment test and not meant to be: it is a
// WAKE signal, and the per-voxel pass that follows is what decides where the
// acid actually touches. A miss costs a tick of probe cadence, never a wrong
// dissolve.
bool DebrisSystem::ThreatNear(const Body& b, World& world) const {
  RefreshLocalBounds(const_cast<Body&>(b));
  const float inv = 1.0f / (float)std::max(1u, b.physScale);
  const float lo[3] = {(float)b.lmin[0], (float)b.lmin[1], (float)b.lmin[2]};
  const float hi[3] = {(float)b.lmax[0] + 1.0f, (float)b.lmax[1] + 1.0f,
                       (float)b.lmax[2] + 1.0f};
  Vec3 pts[kThreatProbePoints];
  int np = 0;
  for (int c = 0; c < 8; c++)
    pts[np++] = Vec3{(c & 1 ? hi[0] : lo[0]) * inv, (c & 2 ? hi[1] : lo[1]) * inv,
                     (c & 4 ? hi[2] : lo[2]) * inv};
  pts[np++] = Vec3{(lo[0] + hi[0]) * 0.5f * inv, (lo[1] + hi[1]) * 0.5f * inv,
                   (lo[2] + hi[2]) * 0.5f * inv};
  for (int i = 0; i < np; i++) {
    const Vec3 wp = b.xf.pos + QuatRot(b.xf.quat, pts[i]);
    const IVec3 c{ifloor(wp.x), ifloor(wp.y), ifloor(wp.z)};
    if (!world.CellInWindow(c)) continue;
    const CachedChunk* cc = world.Cached(ChunkOfCell(c.x, c.y, c.z));
    if (!cc || cc->voxels.size() != kChunkVol) continue;
    const uint32_t m =
        cc->voxels[(((uint32_t)c.z & 15u) * kChunk + ((uint32_t)c.y & 15u)) *
                       kChunk + ((uint32_t)c.x & 15u)] & 0xFFFu;
    if (m != 0 && m < matRewritesNbr_.size() && matRewritesNbr_[m]) return true;
  }
  return false;
}

void DebrisSystem::AddTerrainAnchor(Vec3 posVoxel, float radiusVoxels,
                                    Vec3 velVoxPerSec, float horizonVoxels) {
  extraAnchors_.push_back({posVoxel, radiusVoxels, velVoxPerSec, horizonVoxels});
}

void DebrisSystem::AdoptBody(uint64_t handle, std::vector<DebrisVoxel> voxels,
                             const BodyTransform& xf, MicroBodyRef micro,
                             uint32_t physScale,
                             std::vector<PrefabVoxel> skinVoxels,
                             uint32_t bleedMat, BodyWound wound, bool dead,
                             int defIndex) {
  if (handle == 0 || voxels.empty()) return;
  Body body;
  body.handle = handle;
  // The limb's wound comes with it (a severed head bleeds from its neck; a
  // corpse's stump keeps dripping). Only a wound with something left to pay
  // stays open, and only on a body that has blood to pay it with.
  body.wound = wound;
  body.wound.open = wound.open && bleedMat != 0 &&
                    (wound.budget >= 1.0f || wound.gushTicks > 0);
  body.voxels = std::move(voxels);
  body.xf = xf;
  IVec3 mn{127, 127, 127}, mx{-128, -128, -128};
  for (const DebrisVoxel& v : body.voxels) {
    mn.x = std::min<int>(mn.x, v.x); mn.y = std::min<int>(mn.y, v.y); mn.z = std::min<int>(mn.z, v.z);
    mx.x = std::max<int>(mx.x, v.x); mx.y = std::max<int>(mx.y, v.y); mx.z = std::max<int>(mx.z, v.z);
  }
  float ex = (float)(mx.x - mn.x + 1), ey = (float)(mx.y - mn.y + 1),
        ez = (float)(mx.z - mn.z + 1);
  // Micro rendering comes in with the body (PLAN §C4): a severed microvoxel
  // limb keeps its detail with no mob-specific code here, because the caller
  // hands over what it already knows. The extents above are in COLLIDER units
  // for such a body, so the radius divides by physScale — the +2 slack does not.
  //
  // physScale defaults to the skin scale, which is the pre-split behaviour and
  // stays exact for every caller whose two lattices coincide. A caller with a
  // genuinely finer skin passes the coarser collider scale explicitly and also
  // hands over `skinVoxels` (see AdoptBodySkin).
  body.micro = micro;
  body.physScale = physScale ? physScale : std::max(1u, micro.skinScale);
  // Only keep the fine lattice when it actually IS finer. A caller that hands
  // over a redundant copy at the collider's own resolution would otherwise pay
  // for a second array that carries no extra information.
  if (micro.skinScale > body.physScale) body.skinVoxels = std::move(skinVoxels);
  body.radiusVoxels = 0.5f * std::sqrt(ex * ex + ey * ey + ez * ez) /
                          (float)std::max(1u, body.physScale) +
                      2.0f;
  body.serial = nextSerial_++;
  // OWNERSHIP IS STAMPED WITH THE SERIAL, not defaulted: the two together
  // ARE the global id, and a client whose localPlayerId_ is not 0 must
  // mint into its own band or the two machines' ids collide.
  body.owner = body.ownerAtCreate = localPlayerId_;
  body.bleedMat = bleedMat;
  body.dead = dead;
  body.defIndex = defIndex;
  RecountBurn(body);
  bodies_.push_back(std::move(body));
  instancesDirty_ = true;
}

void DebrisSystem::ReleaseBody(Body& b) {
  // Free the brick BEFORE the handle so an early return can never strand pool
  // words. Shared models (mob-def art, the cached sphere balls) are ignored by
  // MicroBodyFree — only copy-on-write clones this body owns are reclaimed.
  if (b.micro.Valid() && microSet_) MicroBodyFree(*microSet_, b.micro.model);
  b.micro = MicroBodyRef{};
  // Told BEFORE the handle is destroyed, so a listener may still ask physics
  // about it. Every path that lets go of a body funnels through here, which is
  // what makes one notification enough (see SetOnBodyGone).
  if (b.handle && onBodyGone_) onBodyGone_(b.handle);
  if (b.handle) untunnelHold_.erase(b.handle);
  if (b.handle) phys_->RemoveBody(b.handle);
  b.handle = 0;
}

// ---- THE STRAP: a garment follows the limb it is on -------------------------
//
// The whole of the quaternion algebra this needs, local to the two functions
// below rather than pulled in from game/anim.h: phys/ knows nothing about the
// animation runtime and should not start now for eight lines of maths.
namespace {
struct StrapQ {
  float x = 0, y = 0, z = 0, w = 1;
};
StrapQ StrapQOf(const float q[4]) { return StrapQ{q[0], q[1], q[2], q[3]}; }
StrapQ StrapQMul(const StrapQ& a, const StrapQ& b) {
  return StrapQ{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
StrapQ StrapQConj(const StrapQ& q) { return StrapQ{-q.x, -q.y, -q.z, q.w}; }
Vec3 StrapRotate(const StrapQ& q, Vec3 v) {
  const Vec3 u{q.x, q.y, q.z};
  const Vec3 t = u.cross(v) * 2.0f;
  return v + t * q.w + u.cross(t);
}
}  // namespace

bool DebrisSystem::StrapBody(uint64_t shell, uint64_t host) {
  if (shell == 0 || host == 0 || shell == host) return false;
  Body* s = nullptr;
  Body* h = nullptr;
  for (Body& b : bodies_) {
    if (b.handle == shell) s = &b;
    if (b.handle == host) h = &b;
  }
  if (!s || !h) return false;
  // A follower may not itself be followed, and a chain would need an ordering
  // pass this deliberately does not have: one hop, always. (Nothing builds one
  // today — a shell's host is always a base limb — so this is the assertion
  // that keeps it that way rather than a case being handled.)
  if (h->Follower()) return false;
  // Whatever pose the pair holds RIGHT NOW is the rest offset. Not the wear
  // pose: by the time a creature dies its rig has been through a fall, a fight
  // and a dismemberment, and re-deriving from the authored anchors would snap
  // every plate back to a T-pose at the moment of death.
  const StrapQ hq = StrapQOf(h->xf.quat);
  const StrapQ rel = StrapQMul(StrapQConj(hq), StrapQOf(s->xf.quat));
  const Vec3 relPos = StrapRotate(StrapQConj(hq), s->xf.pos - h->xf.pos);
  s->wornHost = host;
  s->wornRelPos = relPos;
  s->wornRelQuat[0] = rel.x;
  s->wornRelQuat[1] = rel.y;
  s->wornRelQuat[2] = rel.z;
  s->wornRelQuat[3] = rel.w;
  // KINEMATIC IS THE WHOLE POINT. A dynamic follower would be teleported by
  // DriveStraps and integrated by Jolt in the same tick, and the two would
  // fight; a kinematic one has no gravity, no mass in any contact and no
  // depenetration response, so the host's dynamics are the pair's dynamics.
  phys_->SetBodyKinematic(shell, true);
  return true;
}

uint64_t DebrisSystem::WornHostOf(uint64_t handle) const {
  for (const Body& b : bodies_)
    if (b.handle == handle) return b.wornHost;
  return 0;
}

void DebrisSystem::FollowersOf(uint64_t host, std::vector<uint64_t>& out) const {
  if (host == 0) return;
  for (const Body& b : bodies_)
    if (b.wornHost == host && b.handle) out.push_back(b.handle);
}

uint32_t DebrisSystem::StrappedCount() const {
  uint32_t n = 0;
  for (const Body& b : bodies_)
    if (b.Follower()) n++;
  return n;
}

void DebrisSystem::CarryStrap(uint64_t oldHandle, uint64_t newHandle) {
  if (oldHandle == 0 || newHandle == 0 || oldHandle == newHandle) return;
  // The strap is keyed on handles and a collider rebuild mints a new one, so
  // without this a breastplate came off its corpse the first time the torso
  // under it lost voxels to a sword or to fire — the same class of bug
  // ReplaceBody exists for on the joint side.
  for (Body& b : bodies_)
    if (b.wornHost == oldHandle) b.wornHost = newHandle;
  // ...and if the REBUILT body is itself a follower, it has to come back
  // kinematic: ReplaceBody carries the collision group and the object layer,
  // not the motion type. A burnt-through sleeve would otherwise start falling
  // out of the arm it is strapped to while DriveStraps kept teleporting it
  // back, which is the pair fighting each other one tick at a time.
  for (const Body& b : bodies_)
    if (b.handle == newHandle && b.Follower()) {
      phys_->SetBodyKinematic(newHandle, true);
      break;
    }
}

bool DebrisSystem::UnstrapBody(uint64_t handle) {
  for (Body& b : bodies_)
    if (b.handle == handle && b.Follower()) {
      UnstrapBody(b);
      return true;
    }
  return false;
}

void DebrisSystem::UnstrapBody(Body& b) {
  if (!b.Follower()) return;
  b.wornHost = 0;
  if (!b.handle) return;
  // It keeps the velocity it was being driven at (DriveStraps wrote the
  // rigid-body velocity at this body's own origin), so a plate whose limb was
  // culled mid-tumble carries on rather than dropping out of the air.
  phys_->SetBodyKinematic(b.handle, false);
}

void DebrisSystem::DriveStraps() {
  for (Body& b : bodies_) {
    if (!b.Follower()) continue;
    const Body* host = nullptr;
    for (const Body& h : bodies_)
      if (h.handle == b.wornHost) { host = &h; break; }
    if (host == nullptr) {
      // The limb this was on has stopped existing. A garment is not a ghost:
      // it becomes debris of its own, here, rather than hanging in the air
      // waiting for a handle that will never come back.
      UnstrapBody(b);
      continue;
    }
    const StrapQ hq = StrapQOf(host->xf.quat);
    const StrapQ q = StrapQMul(hq, StrapQOf(b.wornRelQuat));
    const Vec3 pos = host->xf.pos + StrapRotate(hq, b.wornRelPos);
    const float quat[4] = {q.x, q.y, q.z, q.w};
    // The host's RIGID-BODY velocity at this body's origin (omega x r included).
    // A kinematic body with a stale velocity integrates away from where it was
    // just put, one with none reports every contact as a standing hit, and the
    // instant the strap is cut this is the velocity the garment leaves with.
    Vec3 lin{}, ang{};
    phys_->GetBodyVelocities(host->handle, lin, ang);
    if (!DriveKinematicTo(b, pos, quat, lin + ang.cross(pos - host->xf.pos),
                          ang))
      UnstrapBody(b);  // the handle died under us: it is debris again
  }
}

// ---- THE ONE KINEMATIC DRIVE (see the note in debris.h) ---------------------
bool DebrisSystem::DriveKinematicTo(Body& b, Vec3 pos, const float quat[4],
                                    Vec3 lin, Vec3 ang) {
  if (!b.handle) return false;
  // SetBodyTransform rather than MoveKinematicBody: the latter aims a body at
  // a pose it reaches at the END of the next step, and one tick of lag on a
  // body falling at 40 m/s is four voxels of daylight. See physics.h.
  if (!phys_->SetBodyTransform(b.handle, pos, quat)) return false;
  phys_->SetBodyVelocities(b.handle, lin, ang);
  // Body::xf is what the render instance, the terrain sweep, the threat probe
  // and every world-space query read -- NOT Jolt. Leaving it stale is how a
  // driven body renders a tick behind the collider it is inside.
  b.xf.pos = pos;
  b.xf.quat[0] = quat[0];
  b.xf.quat[1] = quat[1];
  b.xf.quat[2] = quat[2];
  b.xf.quat[3] = quat[3];
  return true;
}

// ============================================================================
//  OWNERSHIP AND GHOST BODIES (PLAN_multiplayer_m9.md M9.4-C)
// ============================================================================
//
// The long argument is in debris.h. What is here is the mechanism, and the one
// thing worth restating at the top of it: EVERY FUNCTION BELOW IS INERT IN A
// SINGLE-PLAYER PROCESS. `ownershipFn_` and `chunkOwnedFn_` are null, so
// RefreshOwnership returns on its first line, ChunkOwned answers yes to
// everything, no body is ever a ghost, and the five emitter tests are a
// compare against a member that is 0 on both sides. That is what the unmoved
// world hash means: this package added a seam, not a behaviour.

void DebrisSystem::SetLocalPlayerId(uint32_t id) { localPlayerId_ = id; }

void DebrisSystem::SetOwnershipFn(
    std::function<uint32_t(uint64_t, Vec3)> fn) {
  ownershipFn_ = std::move(fn);
}

// The position-only binding, ADAPTED rather than stored in a second member:
// one function pointer answers "who owns this body" whatever shape the caller
// had, so `RefreshOwnership` has one call site and there is no "which of the
// two is set" question anywhere below this line.
void DebrisSystem::SetOwnershipFn(std::function<uint32_t(Vec3)> fn) {
  if (!fn) {
    ownershipFn_ = nullptr;
    return;
  }
  ownershipFn_ = [f = std::move(fn)](uint64_t, Vec3 p) { return f(p); };
}

void DebrisSystem::ClearOwnershipFn() { ownershipFn_ = nullptr; }

void DebrisSystem::SetChunkOwnedFn(std::function<bool(IVec3)> fn) {
  chunkOwnedFn_ = std::move(fn);
}

void DebrisSystem::SetItemLookupFn(
    std::function<bool(uint64_t, std::string&, uint32_t&, uint32_t&)> fn) {
  itemLookupFn_ = std::move(fn);
}

void DebrisSystem::SetItemTakeFn(std::function<bool(uint64_t)> fn) {
  itemTakeFn_ = std::move(fn);
}

uint64_t DebrisSystem::GlobalIdOf(uint64_t handle) const {
  if (!handle) return 0;
  for (const Body& b : bodies_)
    if (b.handle == handle)
      return net::MakeGlobalBodyId(b.ownerAtCreate, b.serial);
  return 0;
}

uint64_t DebrisSystem::HandleOfGlobalId(uint64_t globalId) const {
  if (!globalId) return 0;
  const uint32_t mint = net::OwnerOfGlobalId(globalId);
  const uint32_t serial = net::SerialOfGlobalId(globalId);
  for (const Body& b : bodies_)
    if (b.serial == serial && b.ownerAtCreate == mint) return b.handle;
  return 0;
}

uint32_t DebrisSystem::OwnerOfBody(uint64_t handle) const {
  for (const Body& b : bodies_)
    if (b.handle == handle) return b.owner;
  return localPlayerId_;  // unknown handle: nothing here to stop stepping
}

bool DebrisSystem::IsGhost(uint64_t handle) const {
  for (const Body& b : bodies_)
    if (b.handle == handle) return !OwnedLocally(b);
  return false;
}

uint32_t DebrisSystem::GhostCount() const {
  uint32_t n = 0;
  for (const Body& b : bodies_)
    if (b.handle && !OwnedLocally(b)) n++;
  return n;
}

// ---- becoming, and stopping being, a ghost ---------------------------------

void DebrisSystem::MakeGhost(Body& b, uint32_t newOwner) {
  b.owner = newOwner;
  // WHERE THE GHOST STARTS IS WHERE THE BODY IS. Without this a body that
  // changed hands would sit at the origin (or at whatever stale pose an old
  // announce left) until the first pose arrived -- one tick of a sword
  // teleporting across the map every time authority moved. Only seeded when
  // there is no pose yet: a live pose stream is newer than the local copy.
  if (!b.hasPose) {
    b.ghostXf = b.xf;
    b.ghostVel = Vec3{};
  }
  // The settle countdown is meaningless on a body we do not step, and a
  // kinematic body is "inactive" forever -- leaving the count running is how a
  // ghost would qualify for settle-back the moment ownership came back.
  b.inactiveTicks = 0;
  // Wounds and burns keep their STATE (the far side may hand the body back and
  // it should still be bleeding) but stop being SERVICED: the emitter tests do
  // that, not a flag cleared here, so nothing is lost across the round trip.
  if (b.handle) phys_->SetBodyKinematic(b.handle, true);
}

void DebrisSystem::MakeOwned(Body& b) {
  b.owner = localPlayerId_;
  b.hasPose = false;   // the pose stream for it has stopped being authoritative
  b.poseTick = 0;
  b.inactiveTicks = 0;
  if (b.handle) phys_->SetBodyKinematic(b.handle, false);
}

void DebrisSystem::RefreshOwnership() {
  if (!ownershipFn_) return;  // single player: nothing ever changes hands
  for (Body& b : bodies_) {
    if (!b.handle) continue;
    // A FOLLOWER TAKES ITS HOST'S ANSWER, below, rather than its own. Asking
    // separately would let a breastplate and the corpse inside it end up on
    // two different machines the moment the pair straddles a chunk edge --
    // and the shell's pose is DERIVED from the host's, so the answer would be
    // unusable anyway.
    if (b.Follower()) continue;
    // THE BODY'S OWN IDENTITY GOES WITH THE QUESTION (M9.4-E). The global id
    // is what lets the answerer keep ONE incumbent per body instead of one
    // per chunk -- see SetOwnershipFn's note and net::EntitySync::BodyOwner.
    const uint32_t want =
        ownershipFn_(net::MakeGlobalBodyId(b.ownerAtCreate, b.serial), b.xf.pos);
    if (want == b.owner) continue;
    if (want == localPlayerId_)
      MakeOwned(b);
    else
      MakeGhost(b, want);
  }
  // Second pass for the straps, now that every host has settled.
  for (Body& b : bodies_) {
    if (!b.Follower()) continue;
    for (const Body& h : bodies_)
      if (h.handle == b.wornHost) {
        b.owner = h.owner;
        break;
      }
  }
}

void DebrisSystem::DriveGhosts() {
  if (!ownershipFn_ && !chunkOwnedFn_) return;  // single player: no ghosts
  uint32_t live = 0;
  for (Body& b : bodies_) {
    if (!b.handle || OwnedLocally(b)) continue;
    live++;
    // A FOLLOWER IS ALREADY DRIVEN, by its host, in PostStep. Driving it from
    // a pose too would be two owners of one transform fighting each tick --
    // exactly the failure StrapBody's kinematic argument exists to prevent.
    if (b.Follower()) continue;
    if (!b.hasPose) continue;  // announced but not yet posed: leave it put
    if (DriveKinematicTo(b, b.ghostXf.pos, b.ghostXf.quat, b.ghostVel, Vec3{}))
      ownerProbe_.ghostsDriven++;
  }
  ownerProbe_.ghostBodies = live;
}

// ---- receiving --------------------------------------------------------------

bool DebrisSystem::ApplyBodyPose(const net::BodyPose& pose) {
  for (Body& b : bodies_) {
    if (b.serial != net::SerialOfGlobalId(pose.globalId) ||
        b.ownerAtCreate != net::OwnerOfGlobalId(pose.globalId))
      continue;
    // A PEER MAY NOT MOVE A BODY I AM STEPPING. Accepting one would be a
    // silent desync: the pose would be overwritten by Jolt the same tick and
    // nothing would look wrong until the two copies settled in different
    // places. Counted instead, because a stream of these means authority is
    // disputed and that is a real bug one level up.
    if (OwnedLocally(b)) {
      ownerProbe_.posesRefused++;
      return false;
    }
    // OUT-OF-ORDER ARRIVAL IS NORMAL on any datagram transport, and a pose is
    // absolute rather than incremental, so the rule is simply "newest wins".
    // Without it a late packet drags the body backwards and the ghost
    // stutters at exactly the rate the link reorders.
    if (b.hasPose && pose.tick < b.poseTick) {
      ownerProbe_.posesRefused++;
      return false;
    }
    b.ghostXf = pose.xf;
    b.ghostVel = pose.vel;
    b.poseTick = pose.tick;
    b.hasPose = true;
    return true;
  }
  ownerProbe_.posesRefused++;
  return false;
}

uint64_t DebrisSystem::ApplyBodyAnnounce(const net::BodyAnnounce& a) {
  if (a.voxels.empty()) return 0;
  // IDEMPOTENT BY GLOBAL ID. An announce is re-sent whenever a body enters a
  // peer's interest set, and a body can leave and re-enter one several times
  // while a player walks a boundary. Making a second copy each time is how a
  // ghost list grows without bound.
  if (const uint64_t have = HandleOfGlobalId(a.globalId)) {
    for (Body& b : bodies_)
      if (b.handle == have) {
        if (!OwnedLocally(b)) {
          b.ghostXf = a.xf;
          b.hasPose = true;
        }
        return have;
      }
  }
  if (bodies_.size() >= kMaxBodies) return 0;  // the ceiling spawning obeys
  const uint32_t physScale = std::max(1u, a.physScale);
  // allowKinematic = true: this arrives as a ghost and is posed from the wire.
  // Created DYNAMIC (Jolt has no other way in) and flipped below, which is the
  // same two-step CarryStrap takes on a rebuilt follower.
  const uint64_t h = phys_->CreateDebrisBodyXf(a.voxels, a.xf, densityOf_, true,
                                               1.0f / (float)physScale);
  if (h == 0) return 0;
  MicroBodyRef micro{};
  if (a.hadMicro) micro.skinScale = std::max(1u, a.skinScale);
  micro.dye = a.dye;
  AdoptBody(h, a.voxels, a.xf, micro, physScale, a.skinVoxels, a.bleedMat, {},
            a.dead != 0, -1);
  for (Body& b : bodies_)
    if (b.handle == h) {
      // THE ID TRAVELS, THE SERIAL DOES NOT GET RE-MINTED. AdoptBody just gave
      // this body a serial out of MY counter; overwriting both halves with the
      // announce's is what makes `HandleOfGlobalId` on the two machines name
      // the same matter. Nothing else in this file reads `serial` as a dense
      // index -- it is an RNG stream id and a name, both of which survive this.
      b.serial = net::SerialOfGlobalId(a.globalId);
      b.ownerAtCreate = net::OwnerOfGlobalId(a.globalId);
      b.ghostXf = a.xf;
      b.hasPose = true;
      MakeGhost(b, a.owner);
      break;
    }
  return h;
}

uint64_t DebrisSystem::ApplyBodyHandoff(const net::BodyHandoff& h) {
  // ALREADY HERE AS A GHOST: flip it, do not rebuild it. The collider, the
  // brick and the lattice are all correct already, and recreating would churn
  // a Jolt shape and a micro brick for nothing -- and would lose the handle
  // every other system (the grab, the item registry) is holding.
  uint64_t handle = HandleOfGlobalId(h.announce.globalId);
  if (!handle) handle = ApplyBodyAnnounce(h.announce);
  if (!handle) return 0;
  for (Body& b : bodies_)
    if (b.handle == handle) {
      MakeOwned(b);
      // Its momentum crosses the seam with it. Without this a thrown rock
      // stops dead in the air the instant it enters the other player's half
      // of the world, which is the one property gate (c) measures.
      b.xf = h.announce.xf;
      phys_->SetBodyTransform(handle, b.xf.pos, b.xf.quat);
      phys_->SetBodyVelocities(handle, h.vel, h.angVel);
      ownerProbe_.handoffsIn++;
      break;
    }
  return handle;
}

bool DebrisSystem::ApplyBodyGone(const net::BodyGone& g) {
  const uint64_t h = HandleOfGlobalId(g.globalId);
  if (!h) return false;
  for (Body& b : bodies_)
    if (b.handle == h) {
      // ONLY A GHOST. A BodyGone about a body I own is a peer telling me to
      // delete my own matter, which no message is allowed to do -- the owner
      // is the only authority on whether its body still exists.
      if (OwnedLocally(b)) return false;
      break;
    }
  return DestroyBody(h);
}

// ---- sending ----------------------------------------------------------------

bool DebrisSystem::BuildAnnounce(uint64_t handle, net::BodyAnnounce& out) const {
  for (const Body& b : bodies_) {
    if (b.handle != handle) continue;
    if (!OwnedLocally(b)) return false;
    out = net::BodyAnnounce{};
    out.globalId = net::MakeGlobalBodyId(b.ownerAtCreate, b.serial);
    out.owner = b.owner;
    out.xf = b.xf;
    out.voxels = b.voxels;
    out.skinVoxels = b.skinVoxels;
    out.physScale = b.physScale;
    out.skinScale = std::max(1u, b.micro.skinScale);
    out.dye = b.micro.dye;
    out.hadMicro = b.micro.Valid() ? 1u : 0u;
    out.bleedMat = b.bleedMat;
    out.dead = b.dead ? 1u : 0u;
    // The item identity, if the registry above us says this body is one.
    if (itemLookupFn_) itemLookupFn_(handle, out.item, out.itemDye,
                                     out.itemDamage);
    return true;
  }
  return false;
}

bool DebrisSystem::BuildPose(uint64_t handle, uint32_t tick,
                             net::BodyPose& out) const {
  for (const Body& b : bodies_) {
    if (b.handle != handle) continue;
    if (!OwnedLocally(b)) return false;
    out.globalId = net::MakeGlobalBodyId(b.ownerAtCreate, b.serial);
    out.tick = tick;
    out.xf = b.xf;
    Vec3 lin{}, ang{};
    phys_->GetBodyVelocities(handle, lin, ang);
    out.vel = lin;
    return true;
  }
  return false;
}

bool DebrisSystem::BuildHandoff(uint64_t handle, uint32_t newOwner,
                                net::BodyHandoff& out) {
  if (!BuildAnnounce(handle, out.announce)) return false;
  out.announce.owner = newOwner;
  out.newOwner = newOwner;
  phys_->GetBodyVelocities(handle, out.vel, out.angVel);
  // A HANDOFF IS THE MOMENT AUTHORITY MOVES, not a query about it. Waiting for
  // an ack before letting go would leave both machines stepping the same body
  // for the round-trip time, and a rigid body's trajectory diverges inside one
  // tick of that. So the local copy becomes a ghost here, held at this pose
  // until the new owner's first pose arrives.
  for (Body& b : bodies_)
    if (b.handle == handle) {
      MakeGhost(b, newOwner);
      ownerProbe_.handoffsOut++;
      break;
    }
  return true;
}

void DebrisSystem::OwnedBodiesNear(IVec3 peerWindowOrigin, int marginChunks,
                                   std::vector<uint64_t>& outGlobalIds) const {
  const int lo = -marginChunks;
  const int hi = (int)kNChunk + marginChunks;
  for (const Body& b : bodies_) {
    if (!b.handle || !OwnedLocally(b)) continue;
    // A SETTLED BODY IS NOT SENT. `inactiveTicks` is reset by the settle pass
    // the moment anything moves, so this is free to compute and exactly right:
    // two hundred corpses on a battlefield cost no bandwidth, and the first
    // one somebody kicks resumes at full rate. Rule 2, on the wire.
    if (b.inactiveTicks > 1) continue;
    const IVec3 wc{(int)std::floor(b.xf.pos.x) >> 4,
                   (int)std::floor(b.xf.pos.y) >> 4,
                   (int)std::floor(b.xf.pos.z) >> 4};
    const int dx = wc.x - peerWindowOrigin.x, dy = wc.y - peerWindowOrigin.y,
              dz = wc.z - peerWindowOrigin.z;
    if (dx < lo || dx >= hi || dy < lo || dy >= hi || dz < lo || dz >= hi)
      continue;
    outGlobalIds.push_back(net::MakeGlobalBodyId(b.ownerAtCreate, b.serial));
  }
}

// ---- ground items -----------------------------------------------------------

bool DebrisSystem::RequestItemTake(uint64_t globalId) {
  const uint64_t h = HandleOfGlobalId(globalId);
  if (!h) return false;
  // MINE: the pickup happens now and the grant is on the queue this tick. The
  // caller's code is identical to the remote case -- that is the whole point
  // of routing the owner's own E through here too.
  if (!IsGhost(h)) {
    net::ItemTake self{globalId, localPlayerId_};
    ApplyItemTake(self);
    return true;
  }
  // NOT MINE: ask. The ghost is NOT removed here. Removing it optimistically
  // would mean a refused request (the owner picked it up first) leaves a
  // sword that exists on one machine and not the other, with no message that
  // would ever put it back.
  itemTakeOut_.push_back(net::ItemTake{globalId, localPlayerId_});
  return true;
}

bool DebrisSystem::PopItemTake(net::ItemTake& out) {
  if (itemTakeOut_.empty()) return false;
  out = itemTakeOut_.front();
  itemTakeOut_.pop_front();
  return true;
}

void DebrisSystem::ApplyItemTake(const net::ItemTake& t) {
  net::ItemGrant g{};
  g.globalId = t.globalId;
  g.toPlayer = t.byPlayer;
  const uint64_t h = HandleOfGlobalId(t.globalId);
  // A REFUSAL IS A REPLY. The asking machine has to be able to tell "you have
  // it" from "there was nothing there": without the second the ghost item sits
  // on its ground forever and no E will ever take it again.
  if (h && !IsGhost(h) && itemLookupFn_ &&
      itemLookupFn_(h, g.item, g.dye, g.damage) && itemTakeFn_ &&
      itemTakeFn_(h)) {
    g.granted = 1;
    ownerProbe_.itemsGranted++;
  } else {
    ownerProbe_.itemsRefused++;
  }
  itemGrantIn_.push_back(g);
}

bool DebrisSystem::PopItemGrant(net::ItemGrant& out) {
  if (itemGrantIn_.empty()) return false;
  out = itemGrantIn_.front();
  itemGrantIn_.pop_front();
  return true;
}

void DebrisSystem::ApplyItemGrant(const net::ItemGrant& g) {
  // The peer answered. A GRANT IS WHAT DESTROYS THE GHOST -- the request did
  // not (see RequestItemTake), so a refusal correctly leaves the item on the
  // ground for the next try.
  if (g.granted) {
    if (const uint64_t h = HandleOfGlobalId(g.globalId)) DestroyBody(h);
  }
  itemGrantIn_.push_back(g);
}

void DebrisSystem::RebaseVoxels(std::vector<DebrisVoxel>& voxels,
                                BodyTransform& xf) {
  if (voxels.empty()) return;
  IVec3 mn{127, 127, 127};
  for (const DebrisVoxel& v : voxels) {
    mn.x = std::min<int>(mn.x, v.x);
    mn.y = std::min<int>(mn.y, v.y);
    mn.z = std::min<int>(mn.z, v.z);
  }
  if (mn.x == 0 && mn.y == 0 && mn.z == 0) return;
  for (DebrisVoxel& v : voxels) {
    v.x = (int8_t)(v.x - mn.x);
    v.y = (int8_t)(v.y - mn.y);
    v.z = (int8_t)(v.z - mn.z);
  }
  // The origin moved to the new min corner, so the transform moves with it or
  // the body teleports by the rebase amount.
  xf.pos += QuatRot(xf.quat, Vec3{(float)mn.x, (float)mn.y, (float)mn.z});
}

bool DebrisSystem::RebuildCollider(Body& b) {
  if (b.voxels.empty()) return false;
  Vec3 lin{}, ang{};
  phys_->GetBodyVelocities(b.handle, lin, ang);
  // A micro body's voxels are 1/scale world voxels on a side. Building at
  // pitch 1 would inflate a scale-2 body to twice its size and 8x its mass.
  const float pitch = 1.0f / (float)std::max(1u, b.physScale);
  uint64_t nh = phys_->CreateDebrisBodyXf(
      b.voxels, b.xf, densityOf_, /*allowKinematic=*/b.Follower(), pitch);
  if (nh == 0) return false;
  // REPLACE, NOT REMOVE. A corpse is a set of debris bodies that Die() left
  // JOINTED (game/mob.cpp: "joints stay so the corpse hangs together"), and
  // Physics::RemoveBody destroys every joint on the body it removes. This
  // used to be CreateBody + RemoveBody, so the first sword probe to land on a
  // corpse's torso after the killing blow — MeleeSweepDamage keeps probing
  // for the rest of the stroke, and a dead limb is debris that MeltBodyAt
  // carves — rebuilt the torso and took the neck, both shoulders and both
  // hips off in one call. That was "every single one of his limbs pops off"
  // (owner, 2026-09-02). ReplaceBody rebuilds each joint against the new
  // handle and carries the collision group over, so a body comes apart only
  // where the carve actually disconnects it (ShatterBody). Gate:
  // corpse-intact.
  const uint64_t oh = b.handle;
  phys_->ReplaceBody(b.handle, nh);
  b.handle = nh;
  CarryStrap(oh, nh);  // followers of this body, and this body's own strap
  phys_->SetBodyVelocities(nh, lin, ang);
  b.burnedSinceRebuild = 0;
  // A damaged sphere is no longer a sphere: the analytic collider it spawned
  // with is replaced by the greedy-boxed voxel compound built above, so a
  // carved ball stops rolling like a perfect one. That is the whole reason
  // this goes through CreateDebrisBodyXf rather than patching the old shape.
  return true;
}

// Re-derive the collider lattice from the (authoritative) skin lattice.
//
// This is the ONE direction data flows between the two: skin -> collider,
// never back. Keeping it one-directional is what keeps the split outside the
// hashed domain — the skin is render state, and a collider decision that read
// from it in the other direction would drag render state into physics.
//
// Called after any edit to `skinVoxels`. Cheap relative to the carve that
// preceded it (one pass over the fine lattice, one hash map).
void DebrisSystem::DeriveColliderFromSkin(Body& b) {
  if (!b.HasFineSkin()) return;
  const uint32_t ratio = std::max(1u, b.micro.skinScale / b.physScale);
  bool overflow = false;
  b.voxels = DownsampleSkin(b.skinVoxels, ratio, &overflow);
  if (overflow && !skinOverflowWarned_) {
    skinOverflowWarned_ = true;
    std::printf(
        "debris: skin lattice exceeded the collider's +-127 bound; part of a "
        "body was dropped from its collider (physScale too fine for the art)\n");
  }
}

bool DebrisSystem::ReskinMicro(Body& b) {
  if (!b.micro.Valid() || !microSet_) return false;
  int own = MicroBodyOwn(*microSet_, b.micro.model);
  if (own < 0) return false;  // pool full: keep the stale skin, stay damaged
  b.micro.model = (uint32_t)own;
  // The brick is packed from the SKIN when there is one, and from the collider
  // voxels when the two lattices coincide. This is the only difference between
  // a fine-skinned body and a pre-split one, and it is why carving must edit
  // `skinVoxels` — whatever this reads is what the player sees.
  std::vector<PrefabVoxel> mv;
  const bool fine = b.HasFineSkin();
  if (fine) {
    mv = b.skinVoxels;
  } else {
    mv.reserve(b.voxels.size());
    for (const DebrisVoxel& v : b.voxels)
      mv.push_back({(int16_t)v.x, (int16_t)v.y, (int16_t)v.z,
                    (uint16_t)(v.payload & 0xFFF), 0, v.stain});
  }
  IVec3 shift{};
  if (!MicroBodyEdit(*microSet_, b.micro.model, mv, shift)) return false;
  // MicroBodyEdit rebased the brick to ITS OWN min corner. The body's voxels
  // may not be rebased to the same corner yet — ShatterBody re-skins fragments
  // whose coords it already rebased (shift 0), but DamageBody re-skins a
  // survivor whose min corner has just moved inward as voxels were carved off
  // (shift > 0). The brick march runs [0..dims) from the body ORIGIN, so any
  // nonzero shift must move the transform or the art slides off the collider.
  // Doing it here, from what MicroBodyEdit actually chose, is what keeps the
  // two frames agreeing without either side having to assume the other's.
  if (shift.x || shift.y || shift.z) {
    const float inv = 1.0f / (float)std::max(1u, b.micro.skinScale);
    b.xf.pos += QuatRot(b.xf.quat, Vec3{(float)shift.x * inv,
                                        (float)shift.y * inv,
                                        (float)shift.z * inv});
    // Bring the body's own voxels into the brick's frame so collider rebuilds
    // and particle conversion measure from the same origin the art does.
    if (fine) {
      // The shift is in SKIN units, and a skin-unit shift is not generally a
      // whole number of collider voxels: at skin 8 / collider 2 a shift of 3
      // is 0.75 of a collider voxel. So the skin rebases exactly and the
      // collider is RE-DERIVED from it rather than shifted to match. The
      // derived lattice inherits the brick's origin instead of negotiating for
      // it, which is what makes the two frames agree by construction.
      for (PrefabVoxel& sv : b.skinVoxels) {
        sv.x = (int16_t)(sv.x - shift.x);
        sv.y = (int16_t)(sv.y - shift.y);
        sv.z = (int16_t)(sv.z - shift.z);
      }
      DeriveColliderFromSkin(b);
    } else {
      for (DebrisVoxel& v : b.voxels) {
        v.x = (int8_t)(v.x - shift.x);
        v.y = (int8_t)(v.y - shift.y);
        v.z = (int8_t)(v.z - shift.z);
      }
    }
  }
  return true;
}

bool DebrisSystem::DamageBody(size_t bi, World& world,
                              std::vector<ParticleSpawn>& spawns,
                              std::vector<Body>& fragments,
                              uint32_t& newBodyBudget, bool eject,
                              const CarveFactory& carveAt, DamageCause cause,
                              float severity, const SpallParams* spall) {
  Body& b = bodies_[bi];
  phys_->GetTransform(b.handle, b.xf);

  const bool fine = b.HasFineSkin();
  // The collider predicate always exists; the skin one only when the skin is a
  // separate lattice. Both describe the SAME world-space volume.
  const auto keep = carveAt((float)std::max(1u, b.physScale));

  // ---- THE SKIN DECIDES WHETHER ANYTHING HAPPENED (2026-09-20) -------------
  //
  // A SWORD DID NOTHING TO A CORPSE, and this line is why: the whole function
  // used to test the COLLIDER predicate first and `return true` on an empty
  // set, so the carve never reached the skin. A corpse's collider is a
  // majority downsample of the art (DeriveColliderFromSkin) — a human dies at
  // skinScale 8 over physScale 4, so one collider cell is 8 skin cells and
  // flips only when half of them go — and a blade's kerf is a SLIVER:
  // `gore.cutWidth` x the blade's half-thickness is about a tenth of a world
  // voxel across (phys/kerf.h; tuning.json cutDepth 0.08 + 0.15 x power).
  // Nothing that thin removes a collider cell on the first blow, so a sword
  // swung at a body on the ground took nothing off it, left no blood and no
  // gore, and the corpse only shoved — physics with no damage behind it, which
  // is the owner report of 2026-09-20.
  //
  // `Mob::CarveLimb` has said the same thing since the fine skin existed
  // ("deciding 'nothing in range' on the collider would silently make fine
  // tools no-ops on exactly the detailed art the skin exists to serve"), and
  // the corpse path is the same carve on the same lattice one function later.
  // So the order is the live path's order now: carve the AUTHORITATIVE lattice
  // first, derive the collider from what survived, and let the collider's loss
  // be a DIFFERENCE rather than a prediction — which is also the honest set to
  // bill the gore and the physics rebuild from.
  std::vector<DebrisVoxel> removed;   // what the COLLIDER lost
  Vec3 skinLostSum{};                 // centroid accumulator, skin units
  size_t skinLostN = 0;
  if (fine) {
    // The SKIN is authoritative: carve it at its own resolution, then re-derive
    // the collider from what survived. Carving the two independently would let
    // them disagree about the shape; deriving one from the other cannot.
    const auto keepSkin = carveAt((float)std::max(1u, b.micro.skinScale));
    std::vector<DebrisVoxel> colliderBefore;
    if (!b.voxels.empty()) colliderBefore = b.voxels;
    b.skinVoxels.erase(
        std::remove_if(b.skinVoxels.begin(), b.skinVoxels.end(),
                       [&](const PrefabVoxel& v) {
                         if (keepSkin((float)v.x, (float)v.y, (float)v.z))
                           return false;
                         skinLostSum += Vec3{(float)v.x + 0.5f,
                                             (float)v.y + 0.5f,
                                             (float)v.z + 0.5f};
                         skinLostN++;
                         return true;
                       }),
        b.skinVoxels.end());
    if (skinLostN == 0 && !(spall && spall->rounds > 0)) {
      return true;  // nothing in range, on either lattice
    }
    // ---- THE HOLE GROWS INTO ITS OWN RIM, ON THE LATTICE THAT MATTERS -----
    //
    // Between the predicate and the derive, exactly where Mob::CarveLimb runs
    // it and for the reason stated there: the spall must run on the
    // AUTHORITATIVE lattice so the collider re-derive picks the result up for
    // free. One implementation (phys/lattice.h), both populations.
    if (spall != nullptr && spall->rounds > 0) {
      SpallParams sp = *spall;
      const float sk = (float)std::max(1u, b.micro.skinScale);
      sp.centre = spall->centre * sk;
      sp.radius = spall->radius * sk;
      SpallGrow(b.skinVoxels, sp, [&](const PrefabVoxel& v) {
        skinLostSum += Vec3{(float)v.x + 0.5f, (float)v.y + 0.5f,
                            (float)v.z + 0.5f};
        skinLostN++;
      });
      if (skinLostN == 0) return true;
    }
    DeriveColliderFromSkin(b);
    // What the collider actually lost, by differencing the two lists. The
    // predicate-built set this replaces was only ever an approximation of it,
    // and on a kerf it was empty while whole skin voxels left.
    std::unordered_set<uint64_t> after;
    after.reserve(b.voxels.size() * 2);
    auto ck = [](const DebrisVoxel& v) -> uint64_t {
      return ((uint64_t)(uint32_t)(v.x + 32768) << 42) |
             ((uint64_t)(uint32_t)(v.y + 32768) << 21) |
             (uint64_t)(uint32_t)(v.z + 32768);
    };
    for (const DebrisVoxel& v : b.voxels) after.insert(ck(v));
    for (const DebrisVoxel& v : colliderBefore)
      if (!after.count(ck(v))) removed.push_back(v);
  } else {
    // One pass, one predicate call per voxel: the survivors compact in place
    // (the order remove_if kept) and the losses go out in lattice order.
    size_t w = 0;
    for (size_t i = 0; i < b.voxels.size(); i++) {
      const DebrisVoxel v = b.voxels[i];
      if (keep((float)v.x, (float)v.y, (float)v.z)) b.voxels[w++] = v;
      else removed.push_back(v);
    }
    b.voxels.resize(w);
    if (removed.empty() && !(spall && spall->rounds > 0)) {
      return true;  // nothing in range (and nothing moved: w == size)
    }
    // The collider IS the authoritative lattice here, so the spalled cells are
    // the gore: recorded whole, because a gobbet needs the material it was
    // made of (same as the limb path).
    if (spall != nullptr && spall->rounds > 0) {
      SpallParams sp = *spall;
      const float ps = (float)std::max(1u, b.physScale);
      sp.centre = spall->centre * ps;
      sp.radius = spall->radius * ps;
      SpallGrow(b.voxels, sp,
                [&](const DebrisVoxel& v) { removed.push_back(v); });
      if (removed.empty()) return true;
    }
  }
  instancesDirty_ = true;

  Vec3 lin{}, ang{};
  phys_->GetBodyVelocities(b.handle, lin, ang);
  if (eject && !removed.empty())
    VoxelsToParticles(b, removed, lin, ang, world, spawns);

  // CORPSE BLEEDING is a WOUND, not a puff: the cut is remembered on every
  // piece it leaves (the surviving body and each fragment ShatterBody splits
  // off) and BleedBodies drains it from wherever that piece is. Armed at the
  // bottom of this function, after the rebase, because a wound is a body-
  // local point and the frame is about to move. The world position of the
  // cut and how much came off are what survive to that point.
  //
  // MEASURED ON THE LATTICE THAT WAS CARVED, which is the skin whenever there
  // is one: a kerf that takes 40 skin voxels and no collider cell is still a
  // wound, and billing it from the collider would say a cut bled nothing at
  // all until the blow that happened to flip a block.
  Vec3 woundW{};
  float carvedWorldVox = 0.0f;
  const float lostScale =
      (float)std::max(1u, fine ? b.micro.skinScale : b.physScale);
  Vec3 lostCentroid{};   // in the carved lattice's own units
  if (fine) {
    lostCentroid = skinLostSum * (1.0f / (float)skinLostN);
  } else if (!removed.empty()) {
    for (const DebrisVoxel& v : removed)
      lostCentroid +=
          Vec3{(float)v.x + 0.5f, (float)v.y + 0.5f, (float)v.z + 0.5f};
    lostCentroid = lostCentroid * (1.0f / (float)removed.size());
  }
  const size_t lostN = fine ? skinLostN : removed.size();
  if (b.bleedMat != 0 && lostN > 0) {
    woundW = b.xf.pos + QuatRot(b.xf.quat, lostCentroid * (1.0f / lostScale));
    carvedWorldVox =
        (float)lostN / (lostScale * lostScale * lostScale);
  }

  // THE SMEAR on a corpse's cut (bodystain.h SoakCut): the same rule the live
  // limb's kerf and crater get, so a limb cut off and cut again is bloodied
  // both times. Applied to the authoritative lattice AFTER the carve (so the
  // hole's walls count as exposed) and BEFORE the shatter (so every fragment
  // carries its share); the re-skin below writes it into the brick. Until
  // 2026-09-13 a corpse cut showed clean flesh and clean bone.
  if (b.bleedMat != 0 && lostN > 0 && b.bleedMat < matGpu_.size()) {
    const uint32_t stainType = matGpu_[b.bleedMat].stainPack & kStainPackTypeMask;
    const auto& gt = CurrentTuning().gore;
    if (stainType != 0 && gt.stainCutRadius > 0.0f) {
      // Tissue = what crumbles to this body's blood (MobDef::tissue's rule);
      // everything else (bone) takes the floor and nothing more.
      std::vector<uint8_t> tissue(rubbleOf_.size(), 0);
      bool any = false;
      for (size_t m = 0; m < rubbleOf_.size(); m++)
        if (rubbleOf_[m] == b.bleedMat || m == b.bleedMat) { tissue[m] = 1; any = true; }
      if (!any) tissue.clear();
      const float sk = (float)std::max(1u, fine ? b.micro.skinScale : b.physScale);
      // The centroid is already on the lattice this soaks (the skin when there
      // is one), so it needs no conversion.
      const Vec3 centroid = lostCentroid;
      CutSoak soak;
      // The coat names the SUBSTANCE, not its palette slot: a corpse's cut is
      // smeared with whatever that body bled. `stainType` above is still what
      // decides the cut smears at all -- a bleed material with no stain block
      // has nothing to draw.
      soak.mat = b.bleedMat;
      soak.radius = gt.stainCutRadius * sk;
      soak.amountExposed = gt.stainCutAmount;
      soak.amountBuried = gt.stainCutBuried;
      soak.buriedChance = gt.stainCutBuriedChance;
      soak.boneMin = gt.stainBoneMin;
      soak.tissue = &tissue;
      StainLattice L;
      if (fine) L.skin = &b.skinVoxels; else L.coll = &b.voxels;
      SoakCut(L, centroid, soak, (uint32_t)b.serial * 2654435761u ^ 0xC0125Eu,
              nullptr, -1);
      if (fine) DeriveColliderFromSkin(b);  // the coarse lattice carries it too
    }
  }

  // THE SMEAR on a corpse's cut (bodystain.h SoakCut): the same rule the live
  // limb's kerf and crater get, so a limb cut off and cut again is bloodied
  // both times. Applied to the authoritative lattice AFTER the carve (so the
  // hole's walls count as exposed) and BEFORE the shatter (so every fragment
  // carries its share); the re-skin below writes it into the brick. Until
  // 2026-09-13 a corpse cut showed clean flesh and clean bone.
  if (b.bleedMat != 0 && !removed.empty() && b.bleedMat < matGpu_.size()) {
    const uint32_t stainType = matGpu_[b.bleedMat].stainPack & kStainPackTypeMask;
    const auto& gt = CurrentTuning().gore;
    if (stainType != 0 && gt.stainCutRadius > 0.0f) {
      // Tissue = what crumbles to this body's blood (MobDef::tissue's rule);
      // everything else (bone) takes the floor and nothing more.
      std::vector<uint8_t> tissue(rubbleOf_.size(), 0);
      bool any = false;
      for (size_t m = 0; m < rubbleOf_.size(); m++)
        if (rubbleOf_[m] == b.bleedMat || m == b.bleedMat) { tissue[m] = 1; any = true; }
      if (!any) tissue.clear();
      const float ps = (float)std::max(1u, b.physScale);
      const float sk = (float)std::max(1u, fine ? b.micro.skinScale : b.physScale);
      Vec3 centroid{};
      for (const DebrisVoxel& v : removed)
        centroid += Vec3{(float)v.x + 0.5f, (float)v.y + 0.5f, (float)v.z + 0.5f};
      centroid = centroid * (sk / (ps * (float)removed.size()));
      CutSoak soak;
      // The coat names the SUBSTANCE, not its palette slot: a corpse's cut is
      // smeared with whatever that body bled. `stainType` above is still what
      // decides the cut smears at all -- a bleed material with no stain block
      // has nothing to draw.
      soak.mat = b.bleedMat;
      soak.radius = gt.stainCutRadius * sk;
      soak.amountExposed = gt.stainCutAmount;
      soak.amountBuried = gt.stainCutBuried;
      soak.buriedChance = gt.stainCutBuriedChance;
      soak.boneMin = gt.stainBoneMin;
      soak.tissue = &tissue;
      StainLattice L;
      if (fine) L.skin = &b.skinVoxels; else L.coll = &b.voxels;
      SoakCut(L, centroid, soak, (uint32_t)b.serial * 2654435761u ^ 0xC0125Eu,
              nullptr, -1);
      if (fine) DeriveColliderFromSkin(b);  // the coarse lattice carries it too
    }
  }

  // Wholly destroyed, or blown under the body-worthiness floor: the remainder
  // rejoins the world as loose voxels, exactly like the burn dissolve path.
  if (b.voxels.size() < kMinBodyVoxels) {
    VoxelsToParticles(b, b.voxels, lin, ang, world, spawns);
    ReleaseBody(b);
    bodies_[bi] = std::move(bodies_.back());
    bodies_.pop_back();
    return false;
  }

  // Blowing a body in half must yield two bodies, not one body with a hole:
  // the connectivity split is what turns "removed voxels" into "separated
  // pieces". Fresh blast damage uses the ISLAND floor (8), not the much higher
  // burn-fragment floor — a body blown apart by an explosion is a one-off
  // event, not the every-few-ticks re-fragmentation that forced burn's bar up.
  const size_t fragmentsBefore = fragments.size();
  const size_t collBefore = b.voxels.size();
  // fineConnectivity: a CARVE is the one cause whose cut can be narrower than
  // a collider block, so it is the one that may escalate to the skin lattice.
  ShatterBody(b, world, fragments, spawns, kMinBodyVoxels, newBodyBudget,
              /*fineConnectivity=*/true);

  // Rebase AFTER shatter (which rebases fragments itself) so the surviving
  // body's coords stay tight in int8 range, then rebuild the collider.
  //
  // Micro bodies rebase through ReskinMicro INSTEAD, not as well: RebaseVoxels
  // shifts the transform in world-voxel units, but a micro body's coords are
  // 1/scale of one, so using it here would move a scale-2 body twice as far as
  // its voxels actually moved. ReskinMicro divides by the scale and takes the
  // corner MicroBodyEdit really chose, so art, collider and voxels stay in one
  // frame. Exactly one of these two runs for any body.
  const Vec3 posBeforeRebase = b.xf.pos;
  if (b.micro.Valid())
    ReskinMicro(b);
  else
    RebaseVoxels(b.voxels, b.xf);
  // ...AND ONLY WHEN THERE IS A NEW COLLIDER TO BUILD. A kerf that reached the
  // skin and not the derived collider leaves the physics shape bit-identical,
  // and RebuildCollider is not free: it is a fresh Jolt compound plus a
  // ReplaceBody that re-ties every joint on a corpse. Blades now land here on
  // every swing tick (that is the fix above), so rebuilding unconditionally
  // would pay a rig-wide body swap per tick for a shape nobody changed. A
  // derived collider can only LOSE cells to a carve, so a count that did not
  // move means a lattice that did not move; the rebase is asked separately
  // because it moves the frame without changing the count.
  if (!removed.empty() || b.voxels.size() != collBefore ||
      (b.xf.pos - posBeforeRebase).len() > 1e-6f)
    RebuildCollider(b);
  float r = 0;
  for (const DebrisVoxel& v : b.voxels)
    r = std::max(r, Vec3{(float)v.x, (float)v.y, (float)v.z}.len());
  b.radiusVoxels = r / (float)std::max(1u, b.physScale) + 2.0f;
  b.burnCursor = b.voxels.empty() ? 0 : b.burnCursor % (uint32_t)b.voxels.size();
  RecountBurn(b);

  // Arm the wound(s), now that every frame is final. A cut that took a piece
  // off is an amputation and gets what Mob::Sever gives one (the gout, the
  // stump budget and a throw of whole blood voxels) on the stump AND on the
  // piece, each at its own end of the cut: blood comes from each of the
  // rigidbodies that are dismembered, at their own locations. A cut that
  // only carved gets the drip, in proportion to the flesh it took.
  if (b.bleedMat != 0 && carvedWorldVox > 0.0f) {
    const auto& gore = CurrentTuning().gore;
    const bool amputation = fragments.size() > fragmentsBefore;
    const float drip = carvedWorldVox * gore.corpseBleedPerVoxel +
                       (amputation ? gore.severStumpBudget : 0.0f);
    // ---- A CUT GOUTS, NOT ONLY AN AMPUTATION (2026-09-19) -----------------
    //
    // The gout — gore.severSpray droplets front-loaded over severDecayTicks,
    // BleedBodies' arterial branch — used to be armed ONLY when the carve
    // happened to split a piece off. Everything short of that got the drip:
    // one whole blood voxel every gore.bleedDripTicks, three droplets with it.
    // So opening a corpse's skull with a mace produced a bead of blood every
    // eighth of a second and nothing else, which is the owner report of
    // 2026-09-19 ("no blood comes out; there should be tons of blood") almost
    // word for word.
    //
    // A wound that takes flesh bleeds in proportion to the flesh it took, so
    // the gout's LENGTH is that proportion of a dismemberment's — measured
    // against `severStumpBudget`, which is already this file's number for "a
    // whole limb's worth of blood". No new knob: the two dials that set how
    // wet this is (corpseBleedPerVoxel, severStumpBudget) are the same two
    // that set the drip, so they cannot drift apart.
    const float goutFrac =
        amputation
            ? 1.0f
            : std::clamp(carvedWorldVox * gore.corpseBleedPerVoxel /
                             std::max(1.0f, gore.severStumpBudget),
                         0.0f, 1.0f);
    const int gush =
        (int)std::lround(goutFrac * (float)std::max(1, gore.severDecayTicks));
    ArmWound(b, woundW, Vec3{0, 1, 0}, drip, gush);
    for (size_t fi = fragmentsBefore; fi < fragments.size(); fi++)
      ArmWound(fragments[fi], woundW, Vec3{0, 1, 0}, drip, gush);
    // ...and the whole VOXELS a wound throws scale with it too. These are the
    // lasting mess (the droplets above are sub-voxel and expire), so a deep
    // cut into a corpse leaves a real spatter and a scratch leaves none.
    if (amputation || goutFrac > 0.0f) {
      // The conserved voxels a dismemberment throws (gore.severVoxels): few,
      // and they are the lasting mess. Thrown from the cut, up and outward.
      const int nVox =
          (int)std::lround(goutFrac * (float)std::max(0, gore.severVoxels));
      const float sprd = gore.severGobbetSpread;
      for (int k = 0; k < nVox; k++) {
        if (spawns.size() >= kMaxParticleSpawnsPerTick) break;
        const uint32_t h = rng::Hash3(b.serial * 22695477u, (uint32_t)k, 0x5EEDu);
        Vec3 dir{rng::SignedUnit(h) * 0.7f,
                 0.5f + 0.5f * std::fabs(rng::SignedUnit(rng::Pcg(h ^ 0x31u))),
                 rng::SignedUnit(rng::Pcg(h ^ 0x9Fu)) * 0.7f};
        const float sp = gore.severVoxelSpeed *
                         (0.6f + 0.8f * rng::Unit01(rng::Pcg(h ^ 0x77u)));
        const Vec3 at{woundW.x + rng::SignedUnit(rng::Pcg(h ^ 0x2A5u)) * sprd,
                      woundW.y + rng::SignedUnit(rng::Pcg(h ^ 0xB77u)) * sprd,
                      woundW.z + rng::SignedUnit(rng::Pcg(h ^ 0xC3Du)) * sprd};
        spawns.push_back(BloodSpawn(at, dir * sp, b.bleedMat, false, 0, 0));
      }
    }
  }

  // ---- AND A CORPSE COMES APART WHERE IT WAS CUT (2026-09-20) -------------
  //
  // The connectivity split above can only part ONE body; a corpse is a dozen
  // of them, held together by the joints Mob::Die deliberately leaves on. So
  // the same question the living sever by is asked of each of those joints:
  // is there any flesh left at the anchor? A neck cut through now takes the
  // head off, which is what "you can sever the entire bottom half of a head
  // but the top half stays" was only half of.
  const uint32_t partedHere = PartJointsAt(b, cause, severity);

  // ---- WHAT THE ROOM HEARD (2026-09-20) -----------------------------------
  //
  // A corpse used to be silent under a blade. main.cpp infers what a blow hit
  // from the sever and voice queues; dead flesh fills neither, so hacking a
  // body apart on the ground made the cue a CRATE gets. It is the one thing it
  // is not, and the fix is not a special case in the audio code: dead flesh
  // reports what happened to it the same way living flesh does, and the frame
  // drains both lists side by side.
  //
  // Only for a body that was ALIVE (Body::dead), only for a cause that is a
  // blow, and only when something actually came off: a fire eating a corpse is
  // its own sound elsewhere, and a probe that removed nothing is not an event.
  if (b.dead && lostN > 0 && cause != DamageCause::Other &&
      cause != DamageCause::Beam) {
    GoreEvent ge;
    ge.posVoxel = b.bleedMat != 0 ? woundW
                                  : b.xf.pos + QuatRot(b.xf.quat, lostCentroid *
                                                                      (1.0f / lostScale));
    ge.defIndex = b.defIndex;
    ge.severity = std::clamp(severity, 0.0f, 1.0f);
    // A PIECE CAME OFF: the body split (a fragment), or a joint let go.
    ge.severed = fragments.size() > fragmentsBefore || partedHere > 0;
    ge.byBlade = cause == DamageCause::Blade;
    gore_.push_back(ge);
  }
  return true;
}

// ---- THE JOINT RULE, AND IT IS THE LIVING ONE ------------------------------
//
// `Mob::CutLimb` severs when "the flesh AT the joint is gone" — a count of
// voxels in a sphere about the anchor, taken against what was there at spawn.
// A corpse has no spawn count to measure against and no rig to ask, but it has
// the same physical fact available in a simpler form: the anchor is a place on
// this body's own lattice, and either there is matter within a hold radius of
// it or there is not. Nothing to tune per creature, nothing authored, and it
// cannot disagree with the geometry because it IS the geometry.
//
// `gore.corpseJointHold` is that radius in world voxels. The joint is asked
// from THIS body's side only: the other end asks for itself when it is carved,
// which is what makes a neck cut from either direction part the same way.
uint32_t DebrisSystem::PartJointsAt(Body& b, DamageCause cause,
                                    float severity) {
  if (!phys_ || b.handle == 0) return 0;
  const auto& gtune = CurrentTuning().gore;
  const float hold = std::max(gtune.corpseJointHold, 0.0f);
  const float cutFrac = std::clamp(gtune.corpseJointCut, 0.0f, 1.0f);
  if (hold <= 0.0f) return 0;
  if (phys_->JointCount(b.handle) == 0) return 0;
  std::vector<Physics::BodyJoint> js;
  phys_->JointsOn(b.handle, js);
  if (js.empty()) return 0;
  const bool fine = b.HasFineSkin();
  const float scale = (float)std::max(1u, fine ? b.micro.skinScale : b.physScale);
  const float r = hold * scale;
  const float r2 = r * r;
  uint32_t parted = 0;
  for (const Physics::BodyJoint& j : js) {
    // The anchor on the lattice this body is actually made of.
    const Vec3 c = j.anchorLocalVox * scale;
    uint32_t now = 0;
    if (fine) {
      for (const PrefabVoxel& v : b.skinVoxels) {
        const Vec3 d{(float)v.x + 0.5f - c.x, (float)v.y + 0.5f - c.y,
                     (float)v.z + 0.5f - c.z};
        if (d.dot(d) < r2) now++;
      }
    } else {
      for (const DebrisVoxel& v : b.voxels) {
        const Vec3 d{(float)v.x + 0.5f - c.x, (float)v.y + 0.5f - c.y,
                     (float)v.z + 0.5f - c.z};
        if (d.dot(d) < r2) now++;
      }
    }
    // THE DENOMINATOR, taken the first time anything asks about this joint.
    uint32_t base = 0;
    bool known = false;
    for (auto& e : b.jointHold)
      if (e.first == j.joint) { base = e.second; known = true; break; }
    if (!known) {
      b.jointHold.emplace_back(j.joint, now);
      base = now;
    }
    // A FRACTION, NOT A ZERO. One straggler inside the radius kept a head on
    // forever: measured 476 -> 4 voxels over 60 chops with the joint intact.
    const uint32_t floorCount =
        base == 0 ? 0u : (uint32_t)std::lround((float)base * cutFrac);
    if (now > floorCount) continue;
    // NOTHING LEFT TO HOLD. The joint goes, and both ends bleed from where it
    // was: an amputation's worth on each, exactly as the split above pays a
    // fragment and its parent (Mob::Sever's rule, one population later).
    phys_->DestroyJoint(j.joint);
    parted++;
    const Vec3 anchorW = b.xf.pos + QuatRot(b.xf.quat, j.anchorLocalVox);
    const auto& gore = CurrentTuning().gore;
    if (b.bleedMat != 0)
      ArmWound(b, anchorW, Vec3{0, 1, 0}, gore.severStumpBudget,
               gore.severDecayTicks);
    for (Body& o : bodies_)
      if (o.handle == j.other && o.bleedMat != 0) {
        phys_->GetTransform(o.handle, o.xf);
        ArmWound(o, anchorW, Vec3{0, 1, 0}, gore.severStumpBudget,
                 gore.severDecayTicks);
        break;
      }
    // A piece that let go of its host is nobody's follower any more: a strap
    // is a garment riding a limb, and the limb it was riding has left.
    for (Body& o : bodies_)
      if (o.wornHost == b.handle && o.handle == j.other) o.wornHost = 0;
  }
  if (parted > 0) instancesDirty_ = true;
  return parted;
}

Vec3 DebrisSystem::NearestVoxelWorld(uint64_t handle, Vec3 worldPoint) const {
  for (const Body& b : bodies_) {
    if (b.handle != handle) continue;
    const bool fine = b.HasFineSkin();
    const float sc = (float)std::max(1u, fine ? b.micro.skinScale : b.physScale);
    const float qi[4] = {-b.xf.quat[0], -b.xf.quat[1], -b.xf.quat[2],
                         b.xf.quat[3]};
    const Vec3 pl = QuatRot(qi, worldPoint - b.xf.pos) * sc;
    float best = 1e30f;
    Vec3 bestC = pl;
    auto probe = [&](float x, float y, float z) {
      const Vec3 cc{x + 0.5f, y + 0.5f, z + 0.5f};
      const Vec3 d = cc - pl;
      const float d2 = d.dot(d);
      if (d2 < best) { best = d2; bestC = cc; }
    };
    if (fine)
      for (const PrefabVoxel& v : b.skinVoxels)
        probe((float)v.x, (float)v.y, (float)v.z);
    else
      for (const DebrisVoxel& v : b.voxels)
        probe((float)v.x, (float)v.y, (float)v.z);
    if (best > 1e29f) return worldPoint;
    return b.xf.pos + QuatRot(b.xf.quat, bestC * (1.0f / sc));
  }
  return worldPoint;
}

uint32_t DebrisSystem::VoxelsNearWorld(uint64_t handle, Vec3 worldPoint,
                                      float radiusVox) const {
  for (const Body& b : bodies_) {
    if (b.handle != handle) continue;
    const bool fine = b.HasFineSkin();
    const float sc = (float)std::max(1u, fine ? b.micro.skinScale : b.physScale);
    const float qi[4] = {-b.xf.quat[0], -b.xf.quat[1], -b.xf.quat[2],
                         b.xf.quat[3]};
    const Vec3 pl = QuatRot(qi, worldPoint - b.xf.pos) * sc;
    const float r = radiusVox * sc, r2 = r * r;
    uint32_t n = 0;
    auto probe = [&](float x, float y, float z) {
      const Vec3 d{x + 0.5f - pl.x, y + 0.5f - pl.y, z + 0.5f - pl.z};
      if (d.dot(d) < r2) n++;
    };
    if (fine)
      for (const PrefabVoxel& v : b.skinVoxels)
        probe((float)v.x, (float)v.y, (float)v.z);
    else
      for (const DebrisVoxel& v : b.voxels)
        probe((float)v.x, (float)v.y, (float)v.z);
    return n;
  }
  return 0;
}

Vec3 DebrisSystem::BodyWoundWorld(uint32_t i) const {
  if (i >= bodies_.size()) return Vec3{};
  const Body& b = bodies_[i];
  return b.xf.pos + QuatRot(b.xf.quat, b.wound.local);
}

bool DebrisSystem::WoundBody(uint64_t handle, Vec3 woundW, float budget,
                             int gushTicks) {
  if (!phys_) return false;
  for (Body& b : bodies_)
    if (b.handle == handle) {
      if (b.bleedMat == 0) return false;
      phys_->GetTransform(b.handle, b.xf);
      ArmWound(b, woundW, Vec3{0, 1, 0}, budget, gushTicks);
      return true;
    }
  return false;
}

void DebrisSystem::ArmWound(Body& b, Vec3 woundW, Vec3 dirW, float budget,
                            int gushTicks) const {
  if (b.voxels.empty()) return;
  const float ps = (float)std::max(1u, b.physScale);
  const float qi[4] = {-b.xf.quat[0], -b.xf.quat[1], -b.xf.quat[2], b.xf.quat[3]};
  const Vec3 pl = QuatRot(qi, woundW - b.xf.pos) * ps;  // collider units
  // The wound sits ON the body: its voxel nearest the cut, so a piece that
  // was split off far from the cut's centroid still bleeds from its own end
  // of it rather than from a point in mid-air. One pass; hit ticks only.
  float best = 1e30f;
  Vec3 bestC = pl, centroid{};
  for (const DebrisVoxel& v : b.voxels) {
    const Vec3 c{(float)v.x + 0.5f, (float)v.y + 0.5f, (float)v.z + 0.5f};
    centroid += c;
    const Vec3 d = c - pl;
    const float d2 = d.dot(d);
    if (d2 < best) { best = d2; bestC = c; }
  }
  centroid = centroid * (1.0f / (float)b.voxels.size());
  b.wound.open = true;
  b.wound.local = bestC * (1.0f / ps);
  // Blood leaves outward through the wound: from the piece's own centre out
  // through the cut, tilted up so the spray lands on something (the same
  // construction Mob::Sever uses for the stump). `dirW` only breaks a tie.
  Vec3 out = (bestC - centroid) * (1.0f / ps);
  if (out.len() < 0.5f) out = dirW;
  float len = out.len();
  out = len > 1e-3f ? out * (1.0f / len) : Vec3{0, 1, 0};
  out.y += 0.5f;
  len = out.len();
  b.wound.dir = QuatRot(qi, len > 1e-3f ? out * (1.0f / len) : Vec3{0, 1, 0});
  b.wound.budget = AddBleedBudget(b.wound.budget, budget);
  b.wound.gushTicks = std::max(b.wound.gushTicks, gushTicks);
}

void DebrisSystem::BleedBodies(uint32_t tick, World& world,
                               std::vector<ParticleSpawn>& spawns) {
  if (!phys_ || bodies_.empty()) return;
  const auto& gore = CurrentTuning().gore;
  const int ms = std::max(2, gore.microScale);
  int drips = 0;  // gore.bleedOpsPerTick bounds the corpses' drips too
  for (Body& b : bodies_) {
    BodyWound& w = b.wound;
    if (!w.open) continue;
    // The owner's wound pays out once. A ghost bleeding too would double every
    // corpse's blood and would do it from a pose that is one tick stale.
    if (!OwnedLocally(b)) {
      ownerProbe_.emittersSkipped++;
      continue;
    }
    if (b.bleedMat == 0 || (w.gushTicks <= 0 && w.budget < 1.0f)) {
      w.open = false;  // paid out: a corpse does not pump
      continue;
    }
    phys_->GetTransform(b.handle, b.xf);
    const Vec3 at = b.xf.pos + QuatRot(b.xf.quat, w.local);
    const Vec3 axis = QuatRot(b.xf.quat, w.dir);
    if (!world.CellInWindow({(int)std::floor(at.x), (int)std::floor(at.y),
                             (int)std::floor(at.z)}))
      continue;  // streamed out: the wound waits

    // ---- the dismemberment gout: front-loaded, every tick (Mob::BleedTick)
    if (w.gushTicks > 0) {
      const int decay = std::max(1, gore.severDecayTicks);
      const float frac = (float)w.gushTicks / (float)decay;
      const int want = (int)std::lround(2.0f * (float)gore.severSpray * frac /
                                        (float)decay);
      for (int k = 0; k < want; k++) {
        if (spawns.size() >= kMaxParticleSpawnsPerTick) break;
        const uint32_t h = rng::Hash3(b.serial * 2654435761u, tick,
                                      (uint32_t)k * 0x9E3779B9u);
        const float cone = gore.severSprayCone;
        Vec3 dir{axis.x + rng::SignedUnit(h) * cone,
                 axis.y + rng::SignedUnit(rng::Pcg(h ^ 0x51A17u)) * cone,
                 axis.z + rng::SignedUnit(rng::Pcg(h ^ 0xB0011u)) * cone};
        const float sp = gore.severSpraySpeed *
                         (0.75f + 0.5f * rng::Unit01(rng::Pcg(h ^ 0x1234u)));
        spawns.push_back(BloodSpawn(at, dir * sp, b.bleedMat, true,
                                    gore.microLifeTicks, ms));
      }
      w.gushTicks--;
    }

    // ---- the drip: one whole voxel per gore.bleedDripTicks, and its spray
    if (w.budget < 1.0f || drips >= gore.bleedOpsPerTick) continue;
    if (tick % (uint32_t)std::max(1, gore.bleedDripTicks) != 0) continue;
    {
      // A body is not in the grid, so the drop is a ballistic voxel released
      // just outside the wound, drifting out along it; it lands wherever the
      // corpse is lying. That is the corpse's puddle.
      const uint32_t h = rng::Hash3(b.serial * 40503u, tick ^ 0xB1005u, 0u);
      Vec3 dir{axis.x + rng::SignedUnit(h) * 0.3f, axis.y,
               axis.z + rng::SignedUnit(rng::Pcg(h ^ 0x77u)) * 0.3f};
      spawns.push_back(BloodSpawn(at + axis * 0.6f, dir * 1.5f, b.bleedMat,
                                  false, 0, 0));
      w.budget -= 1.0f;
      drips++;
    }
    const int sprayN = std::max(0, (int)std::lround(gore.bleedSprayPerDrip));
    for (int k = 0; k < sprayN; k++) {
      if (spawns.size() >= kMaxParticleSpawnsPerTick) break;
      const uint32_t h = rng::Hash3(b.serial * 40503u, tick ^ 0xB1005u,
                                    (uint32_t)(k + 1) * 2246822519u);
      const float cone = gore.bleedSprayCone;
      Vec3 dir{rng::SignedUnit(h) * cone,
               0.6f + 0.4f * std::fabs(rng::SignedUnit(rng::Pcg(h ^ 0x77u))),
               rng::SignedUnit(rng::Pcg(h ^ 0xC0FFEEu)) * cone};
      spawns.push_back(BloodSpawn(at, dir * gore.bleedSpraySpeed, b.bleedMat,
                                  true, gore.microLifeTicks, ms));
    }
  }
}

void DebrisSystem::DamageBodiesRadial(Vec3 centerVoxel, float radiusVoxels,
                                      World& world,
                                      std::vector<ParticleSpawn>& spawns) {
  if (!phys_ || radiusVoxels <= 0.0f) return;
  std::vector<Body> fragments;
  uint32_t budget = kMaxNewBodiesPerTick;
  const float r2 = radiusVoxels * radiusVoxels;

  for (size_t bi = 0; bi < bodies_.size();) {
    Body& b = bodies_[bi];
    phys_->GetTransform(b.handle, b.xf);
    // cheap reject: blast sphere vs body bounding sphere
    if ((b.xf.pos - centerVoxel).len() > radiusVoxels + b.radiusVoxels + 2.0f) {
      bi++;
      continue;
    }
    // Blast centre into body-local units. The predicate is built PER LATTICE:
    // the same world-space sphere, expressed at whatever resolution the lattice
    // it is testing lives in. That is what lets one carve edit a fine skin and
    // a coarse collider without either one having to know about the other.
    const float q[4] = {-b.xf.quat[0], -b.xf.quat[1], -b.xf.quat[2], b.xf.quat[3]};
    const Vec3 cBody = QuatRot(q, centerVoxel - b.xf.pos);
    // Deterministic-enough jitter so the crater rim is ragged rather than a
    // billiard-ball scoop. This is CPU gameplay state (bodies are outside the
    // hashed domain), so a float hash is fine here — rule 1 governs the grid.
    const uint32_t seed = b.serial;
    // Jitter is keyed on the SKIN lattice for both tests, so the crater's rim
    // pattern is a property of the art rather than of whatever collider
    // resolution the engine happened to pick. Without this the same explosion
    // would rag differently depending on physScale.
    const uint32_t jScale = std::max(1u, b.micro.skinScale);

    auto carve = [&](float lat) {
      const float sc = lat;
      const Vec3 cLocal = cBody * sc;
      const float rLocal2 = r2 * sc * sc;
      const float jMul = (float)jScale / sc;  // lattice coord -> skin coord
      return [=](float vx, float vy, float vz) {
        Vec3 c{vx + 0.5f, vy + 0.5f, vz + 0.5f};
        Vec3 dv = c - cLocal;
        float d2 = dv.dot(dv);
        if (d2 >= rLocal2) return true;  // outside the blast
        // Falloff: certain removal in the core, thinning toward the rim.
        float t = std::sqrt(d2 / rLocal2);  // 0 centre .. 1 rim
        float chance = 1.0f - t * t;        // quadratic, wide core
        int jx = (int)(vx * jMul), jy = (int)(vy * jMul), jz = (int)(vz * jMul);
        uint32_t h = Hash3(seed, (uint32_t)jx * 73856093u,
                           (uint32_t)jy * 19349663u ^ (uint32_t)jz * 83492791u);
        return (float)(h & 0xFFFFu) / 65535.0f >= chance;
      };
    };

    bool alive = DamageBody(bi, world, spawns, fragments, budget, true, carve,
                            DamageCause::Blast, 1.0f);
    if (alive) bi++;
  }
  for (Body& f : fragments) {
    bodies_.push_back(std::move(f));
    instancesDirty_ = true;
  }
}

bool DebrisSystem::MeltBodyAt(uint64_t handle, Vec3 pointVoxel,
                              float radiusVoxels, World& world,
                              std::vector<ParticleSpawn>& spawns) {
  if (!phys_) return false;
  size_t bi = 0;
  for (; bi < bodies_.size(); bi++)
    if (bodies_[bi].handle == handle) break;
  if (bi == bodies_.size()) return false;

  Body& b = bodies_[bi];
  phys_->GetTransform(b.handle, b.xf);
  const float q[4] = {-b.xf.quat[0], -b.xf.quat[1], -b.xf.quat[2], b.xf.quat[3]};
  const Vec3 pBody = QuatRot(q, pointVoxel - b.xf.pos);
  const float r2 = radiusVoxels * radiusVoxels;

  std::vector<Body> fragments;
  uint32_t budget = kMaxNewBodiesPerTick;
  // Same world-space sphere, re-expressed per lattice — see DamageBodiesRadial.
  // A clean bore, so no jitter and nothing keyed on a lattice at all.
  auto carve = [&](float lat) {
    const Vec3 pLocal = pBody * lat;
    const float rLocal2 = r2 * lat * lat;
    return [=](float vx, float vy, float vz) {
      Vec3 c{vx + 0.5f, vy + 0.5f, vz + 0.5f};
      Vec3 dv = c - pLocal;
      return dv.dot(dv) >= rLocal2;
    };
  };
  // eject=false: the beam vaporizes. A held laser damages every tick, and
  // spraying particles from each one would drain the spawn ring in a second.
  // The return says whether the body survived; either way the hit landed, and
  // fragments still have to be adopted below.
  DamageBody(bi, world, spawns, fragments, budget, false, carve,
             DamageCause::Beam, 0.5f);
  for (Body& f : fragments) {
    bodies_.push_back(std::move(f));
    instancesDirty_ = true;
  }
  return true;
}

bool DebrisSystem::CutBody(uint64_t handle, const KerfCut& cut, World& world,
                           std::vector<ParticleSpawn>& spawns) {
  if (!phys_) return false;
  size_t bi = 0;
  for (; bi < bodies_.size(); bi++)
    if (bodies_[bi].handle == handle) break;
  if (bi == bodies_.size()) return false;

  Body& b = bodies_[bi];
  phys_->GetTransform(b.handle, b.xf);
  // Into the body's own frame. The conjugate sandwich, the same one
  // DamageBodiesRadial uses to put a blast centre in body space.
  const float qi[4] = {-b.xf.quat[0], -b.xf.quat[1], -b.xf.quat[2],
                       b.xf.quat[3]};
  const Vec3 cLocal = QuatRot(qi, cut.at - b.xf.pos);
  const bool fine = b.HasFineSkin();
  const float latScale =
      (float)std::max(1u, fine ? b.micro.skinScale : b.physScale);
  // The rim is quantized onto the SKIN lattice on both populations, so a cut
  // tears the same way whatever collider resolution this body derived.
  const float jitter = (float)std::max(1u, b.micro.skinScale);
  // Keyed on the body rather than on the tick, so the same stroke replayed
  // against the same corpse tears identically (KerfCut::seed).
  const uint32_t seed = b.serial * 2654435761u + cut.seed;
  const KerfSlot frame =
      KerfFrame(cLocal, QuatRot(qi, cut.edgeAxis), QuatRot(qi, cut.cutDir),
                cut.depth, cut.halfWidth, cut.length, jitter, seed);
  // WHERE THE EDGE MEETS MEAT, on the authoritative lattice. Without this a
  // second blow into an existing gash finds the space already gone and takes
  // nothing — "the wound stops getting deeper", which on a corpse reads as a
  // sword bouncing off it.
  const float entry = KerfEntry(frame, latScale, [&](auto&& probe) {
    if (fine)
      for (const PrefabVoxel& pv : b.skinVoxels)
        probe((float)pv.x, (float)pv.y, (float)pv.z);
    else
      for (const DebrisVoxel& dv : b.voxels)
        probe((float)dv.x, (float)dv.y, (float)dv.z);
  });
  KerfSlot slot = frame;
  slot.c = cLocal + frame.w * entry;

  // ---- AND THE HOLE GROWS INTO ITS OWN RIM (2026-09-20) -------------------
  //
  // THE SAME SPALL THE LIVING GET, from the same two knobs, built by the same
  // four lines Mob::CutLimb builds it from (phys/lattice.h SpallGrow). Without
  // it a kerf on a corpse SATURATES: the next identical blow tests the same
  // slot, finds the space already gone and takes almost nothing, so a wound
  // stipples instead of deepening and nothing ever comes apart. That is what
  // made "sustained hits dismember" true of a creature and false of its own
  // corpse one function call later.
  //
  // Centred on the cut and sized to the SLOT, not to a blast radius: the spall
  // pass is a sphere test, and one sized to the depth is the volume the edge
  // actually disturbed.
  const auto& gt = CurrentTuning().gore;
  SpallParams spall;
  if (gt.cutSpallRounds > 0 && gt.cutSpallStrength > 0.0f && cut.depth > 0.0f) {
    spall.centre = slot.c + slot.w * (cut.depth * 0.5f);   // scaled below
    spall.radius = std::max(cut.depth, slot.halfW * 2.0f);
    spall.strength = std::clamp(gt.cutSpallStrength, 0.0f, 1.0f);
    spall.rounds = gt.cutSpallRounds;
    spall.seed = seed;
  }

  std::vector<Body> fragments;
  uint32_t budget = kMaxNewBodiesPerTick;
  // eject=true, unlike the beam: a blade takes matter OFF and that matter is
  // the gore. The beam vaporizes and is the only carve here that should not.
  DamageBody(bi, world, spawns, fragments, budget, /*eject=*/true,
             [slot](float lat) -> CarveKeep {
               const KerfKeep k = KerfKeepAt(slot, lat);
               return [k](float x, float y, float z) { return k(x, y, z); };
             },
             DamageCause::Blade, std::clamp(cut.power, 0.0f, 1.0f), &spall);
  for (Body& f : fragments) {
    bodies_.push_back(std::move(f));
    instancesDirty_ = true;
  }
  return true;
}

float DebrisSystem::BruiseBody(uint64_t handle, Vec3 atVoxel,
                               float radiusVoxels, uint32_t seed, float power,
                               float hp, bool unarmed) {
  if (!phys_ || radiusVoxels <= 0.0f) return 0.0f;
  size_t bi = 0;
  for (; bi < bodies_.size(); bi++)
    if (bodies_[bi].handle == handle) break;
  if (bi == bodies_.size()) return 0.0f;
  Body& b = bodies_[bi];
  if (b.bleedMat == 0) return 0.0f;   // matter that was never flesh
  const auto& gt = CurrentTuning().gore;
  uint32_t bruiseMat = 0;
  for (size_t m = 0; m < matNames_.size(); m++)
    if (matNames_[m] == gt.bruiseMat) { bruiseMat = (uint32_t)m; break; }
  if (bruiseMat == 0) return 0.0f;
  // The same unarmed overrides the living take (Mob::BluntHit): a negative
  // override means "same as the base".
  const float effStep = (unarmed && gt.unarmedBruiseStep >= 0.0f)
                            ? gt.unarmedBruiseStep
                            : gt.bruiseStep;
  const float effBleedChance = (unarmed && gt.unarmedBleedChance >= 0.0f)
                                   ? gt.unarmedBleedChance
                                   : gt.bruiseBleedChance;
  const uint32_t cap =
      (uint32_t)std::lround(std::clamp(gt.bruiseMax, 0.0f, 15.0f));
  if (cap == 0 || effStep <= 0.0f) return 0.0f;

  phys_->GetTransform(b.handle, b.xf);
  const float qi[4] = {-b.xf.quat[0], -b.xf.quat[1], -b.xf.quat[2],
                       b.xf.quat[3]};
  const bool fine = b.HasFineSkin();
  const float scale =
      (float)std::max(1u, fine ? b.micro.skinScale : b.physScale);
  const Vec3 cLocal = QuatRot(qi, atVoxel - b.xf.pos) * scale;
  const float pw = std::clamp(power, 0.0f, 1.0f);
  float blowScale = 1.0f;
  if (hp > 0.0f && gt.bruiseHpRef > 0.0f)
    blowScale = std::clamp(std::sqrt(hp / gt.bruiseHpRef),
                           std::clamp(gt.bruiseHpFloor, 0.0f, 1.0f), 1.0f);

  // Tissue = what crumbles to this body own blood, the same census the cut
  // soak takes: bone is not bruised, it is uncovered.
  std::vector<uint8_t> tissue(rubbleOf_.size(), 0);
  bool any = false;
  for (size_t m = 0; m < rubbleOf_.size(); m++)
    if (rubbleOf_[m] == b.bleedMat || m == b.bleedMat) {
      tissue[m] = 1;
      any = true;
    }
  if (!any) tissue.clear();

  BruiseSoak bs;
  bs.centre = cLocal;
  bs.radius = radiusVoxels * scale;
  bs.bruiseMat = bruiseMat;
  bs.bloodMat = b.bleedMat;
  bs.step = effStep;
  bs.cap = cap;
  bs.bleedFrom = (uint32_t)std::lround(
      (float)cap * std::clamp(gt.bruiseBleedFrom, 0.0f, 1.0f));
  bs.bleedChance = std::clamp(effBleedChance, 0.0f, 1.0f) * pw * blowScale;
  bs.pulpAt = (uint32_t)std::lround(std::clamp(gt.pulpAmt, 1.0f, 15.0f));
  bs.blowScale = blowScale;
  bs.tissue = tissue.empty() ? nullptr : &tissue;
  bs.seed = b.serial * 2654435761u + seed;
  StainLattice L;
  if (fine)
    L.skin = &b.skinVoxels;
  else
    L.coll = &b.voxels;
  BruiseTally tally;
  SoakBruise(L, bs, &tally, nullptr, -1);
  if (tally.marked) {
    // The coarse lattice carries the mark too, and the brick is what the
    // player sees: re-skin so the bruise actually appears.
    if (fine) DeriveColliderFromSkin(b);
    if (b.micro.Valid()) ReskinMicro(b);
    instancesDirty_ = true;
  }
  // RUNG 3 ARMS THE CLOCK, it does not carve. Same as the living: what the
  // blow found already pulped is what crumbles, and it crumbles over the next
  // few seconds rather than in one swing (PulpTick).
  if (tally.pulped > 0) b.pulp = true;
  return tally.Ripeness();
}

// ---- THE THIRD RUNG, OVER TIME ---------------------------------------------
//
// Pulped tissue crumbles at gore.pulpRotRate, surface first so the hole opens
// outward rather than hollowing the body invisibly. Expressed as an ordinary
// carve so it inherits everything DamageBody does - the re-skin, the collider
// rebuild, the connectivity split, the wound - and BATCHED for the same
// reason: a carve per voxel per tick would rebuild a Jolt compound sixty times
// a second.
//
// COST (rule 2): a body nothing has beaten pays one bool. A flagged body that
// draws zero this tick pays one hash and a compare.
void DebrisSystem::PulpTick(uint32_t tick, World& world,
                            std::vector<ParticleSpawn>& spawns) {
  const auto& gt = CurrentTuning().gore;
  if (gt.pulpRotRate <= 0.0f) return;
  const float perTick = 1.0f / (60.0f * 30.0f);   // per minute -> per tick
  const uint32_t pulpAt =
      (uint32_t)std::lround(std::clamp(gt.pulpAmt, 1.0f, 15.0f));
  auto key = [](int x, int y, int z) -> uint64_t {
    return ((uint64_t)(uint32_t)(x + 32768) << 42) |
           ((uint64_t)(uint32_t)(y + 32768) << 21) |
           (uint64_t)(uint32_t)(z + 32768);
  };
  for (size_t bi = 0; bi < bodies_.size(); bi++) {
    Body& b = bodies_[bi];
    if (!b.pulp || b.bleedMat == 0) continue;
    // The third per-body spawn emitter, on the same rule as the burn and the
    // bleed: pulped tissue crumbles out of the OWNER's copy only.
    if (!OwnedLocally(b)) {
      ownerProbe_.emittersSkipped++;
      continue;
    }
    const bool fine = b.HasFineSkin();
    const float sc =
        (float)std::max(1u, fine ? b.micro.skinScale : b.physScale);
    const float lat = sc * sc * sc;
    // How many cells this tick: the integer part plus a Bernoulli remainder,
    // keyed on the body and the tick so a replay dissolves identically.
    const float want = gt.pulpRotRate * lat * perTick;
    uint32_t n = (uint32_t)want;
    const uint32_t h = rng::Hash3(b.serial * 0x9E3779B9u, tick, 0xD1550u);
    if ((float)(h & 0xFFFFu) / 65535.0f < want - (float)n) n++;
    if (n == 0) continue;

    // WHO IS PULPED, and prefer a surface cell so the cave-in opens outward.
    StainLattice L;
    if (fine)
      L.skin = &b.skinVoxels;
    else
      L.coll = &b.voxels;
    const size_t cells = L.Size();
    std::unordered_set<uint64_t> live;
    live.reserve(cells * 2);
    for (size_t i = 0; i < cells; i++) {
      const IVec3 v = L.At(i);
      live.insert(key(v.x, v.y, v.z));
    }
    // BONE DOES NOT DISSOLVE — the corpse's half of the living rule (see
    // Mob::BluntPulpTick). The census is the one BruiseBody above builds, and
    // for the same reason it builds it: what crumbles to this body's own blood
    // is tissue, and a skeleton is what is left when the tissue has gone.
    std::vector<uint8_t> tissue(rubbleOf_.size(), 0);
    bool anyTissue = false;
    for (size_t m = 0; m < rubbleOf_.size(); m++)
      if (rubbleOf_[m] == b.bleedMat || m == b.bleedMat) {
        tissue[m] = 1;
        anyTissue = true;
      }
    if (!anyTissue) tissue.clear();

    std::vector<IVec3> face, buried;
    for (size_t i = 0; i < cells; i++) {
      const uint32_t mat = L.Mat(i) & 0xFFFu;
      if (mat == 0) continue;
      if (!tissue.empty() && (mat >= tissue.size() || !tissue[mat])) continue;
      const uint16_t st = L.Stain(i);
      if (BodyStainMat(st) != b.bleedMat || BodyStainAmt(st) < pulpAt) continue;
      const IVec3 v = L.At(i);
      static constexpr int kN[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                       {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
      bool open = false;
      for (const auto& d : kN)
        if (!live.count(key(v.x + d[0], v.y + d[1], v.z + d[2]))) {
          open = true;
          break;
        }
      (open ? face : buried).push_back(v);
    }
    if (face.empty() && buried.empty()) {
      b.pulp = false;   // nothing left to eat
      continue;
    }
    std::vector<IVec3>& pool = face.empty() ? buried : face;
    std::unordered_set<uint64_t> doomed;
    for (uint32_t k = 0; k < n && !pool.empty(); k++) {
      const uint32_t pick =
          rng::Hash3(b.serial, tick * 2654435761u, k * 40503u) %
          (uint32_t)pool.size();
      doomed.insert(key(pool[pick].x, pool[pick].y, pool[pick].z));
      pool[pick] = pool.back();
      pool.pop_back();
    }
    if (doomed.empty()) continue;
    // As a carve, at the AUTHORITATIVE lattice only: the collider is derived
    // from what survives, which is the one direction data flows.
    const float authScale = sc;
    std::vector<Body> fragments;
    uint32_t budget = kMaxNewBodiesPerTick;
    const bool alive = DamageBody(
        bi, world, spawns, fragments, budget, /*eject=*/true,
        [doomed, key, authScale](float lattice) -> CarveKeep {
          const bool auth = std::fabs(lattice - authScale) < 0.5f;
          return [doomed, key, auth](float x, float y, float z) {
            if (!auth) return true;
            return doomed.count(key((int)x, (int)y, (int)z)) == 0;
          };
        },
        DamageCause::Blunt, 0.2f);
    for (Body& f : fragments) {
      bodies_.push_back(std::move(f));
      instancesDirty_ = true;
    }
    if (!alive) bi--;   // swap-and-pop moved another body into this slot
  }
}

bool DebrisSystem::BluntBody(uint64_t handle, Vec3 atVoxel, float radiusVoxels,
                             uint32_t seed, World& world,
                             std::vector<ParticleSpawn>& spawns,
                             DamageCause cause) {
  if (!phys_ || radiusVoxels <= 0.0f) return false;
  size_t bi = 0;
  for (; bi < bodies_.size(); bi++)
    if (bodies_[bi].handle == handle) break;
  if (bi == bodies_.size()) return false;

  Body& b = bodies_[bi];
  phys_->GetTransform(b.handle, b.xf);
  const float qi[4] = {-b.xf.quat[0], -b.xf.quat[1], -b.xf.quat[2],
                       b.xf.quat[3]};
  const Vec3 cBody = QuatRot(qi, atVoxel - b.xf.pos);
  const float r2 = radiusVoxels * radiusVoxels;
  const uint32_t jScale = std::max(1u, b.micro.skinScale);
  const uint32_t key = b.serial * 40503u + seed;

  // The blast crater's shape at a mace's scale: certain removal in the core,
  // thinning toward the rim, quantized onto the skin so the dent is a property
  // of the art. Shared with DamageBodiesRadial in everything but its size,
  // which is the only thing that distinguishes a hammer from a grenade.
  auto carve = [&](float lat) -> CarveKeep {
    const float sc = lat;
    const Vec3 cLocal = cBody * sc;
    const float rLocal2 = r2 * sc * sc;
    const float jMul = (float)jScale / sc;
    return [=](float vx, float vy, float vz) {
      Vec3 c{vx + 0.5f, vy + 0.5f, vz + 0.5f};
      Vec3 dv = c - cLocal;
      float d2 = dv.dot(dv);
      if (d2 >= rLocal2) return true;
      float t = std::sqrt(d2 / rLocal2);
      float chance = 1.0f - t * t;
      int jx = (int)(vx * jMul), jy = (int)(vy * jMul), jz = (int)(vz * jMul);
      uint32_t h = Hash3(key, (uint32_t)jx * 73856093u,
                         (uint32_t)jy * 19349663u ^ (uint32_t)jz * 83492791u);
      return (float)(h & 0xFFFFu) / 65535.0f >= chance;
    };
  };

  std::vector<Body> fragments;
  uint32_t budget = kMaxNewBodiesPerTick;
  DamageBody(bi, world, spawns, fragments, budget, /*eject=*/true, carve,
             cause, 1.0f);
  for (Body& f : fragments) {
    bodies_.push_back(std::move(f));
    instancesDirty_ = true;
  }
  return true;
}

bool DebrisSystem::MicroDirty() const {
  return microSet_ && microSet_->dirty;
}

bool DebrisSystem::SplitBody(uint64_t handle, Vec3 planePointVoxel,
                             Vec3 planeNormal) {
  size_t bi = 0;
  for (; bi < bodies_.size(); bi++)
    if (bodies_[bi].handle == handle) break;
  if (bi == bodies_.size()) return false;
  Body& b = bodies_[bi];
  // Micro bodies are not cuttable in v1: both halves would need their own
  // brick, and the pool holds per-DEF models shared across instances (see the
  // BurnBodies note). The laser passes through instead of silently producing
  // two wrongly-scaled cube bodies.
  if (b.micro.Valid()) return false;
  phys_->GetTransform(b.handle, b.xf);

  // plane into body-local space (conjugate rotation)
  const float qx = -b.xf.quat[0], qy = -b.xf.quat[1], qz = -b.xf.quat[2],
              qw = b.xf.quat[3];
  auto rotInv = [&](Vec3 v) {
    Vec3 u{qx, qy, qz};
    Vec3 t = u.cross(v) * 2.0f;
    return v + t * qw + u.cross(t);
  };
  Vec3 pLocal = rotInv(planePointVoxel - b.xf.pos);
  Vec3 nLocal = rotInv(planeNormal).normalized();

  std::vector<DebrisVoxel> halves[2];
  for (const DebrisVoxel& v : b.voxels) {
    Vec3 c{(float)v.x + 0.5f, (float)v.y + 0.5f, (float)v.z + 0.5f};
    halves[(c - pLocal).dot(nLocal) >= 0 ? 1 : 0].push_back(v);
  }
  if (halves[0].size() < 4 || halves[1].size() < 4) return false;

  Vec3 lin{}, ang{};
  phys_->GetBodyVelocities(b.handle, lin, ang);
  auto rot = [&](Vec3 v) {
    Vec3 u{-qx, -qy, -qz};
    Vec3 t = u.cross(v) * 2.0f;
    return v + t * qw + u.cross(t);
  };

  Body newBodies[2];
  for (int h = 0; h < 2; h++) {
    // rebase to the half's own min corner (keeps int8 coords tight) and
    // shift the body position by the rotated offset so nothing moves
    IVec3 mn{127, 127, 127};
    for (const DebrisVoxel& v : halves[h]) {
      mn.x = std::min<int>(mn.x, v.x);
      mn.y = std::min<int>(mn.y, v.y);
      mn.z = std::min<int>(mn.z, v.z);
    }
    for (DebrisVoxel& v : halves[h]) {
      v.x = (int8_t)(v.x - mn.x);
      v.y = (int8_t)(v.y - mn.y);
      v.z = (int8_t)(v.z - mn.z);
    }
    BodyTransform xf = b.xf;
    Vec3 shift = rot(Vec3{(float)mn.x, (float)mn.y, (float)mn.z});
    xf.pos += shift;
    newBodies[h].handle = phys_->CreateDebrisBodyXf(halves[h], xf, densityOf_);
    if (newBodies[h].handle == 0) {
      if (h == 1 && newBodies[0].handle) phys_->RemoveBody(newBodies[0].handle);
      return false;
    }
    phys_->CarryLayer(b.handle, newBodies[h].handle);  // same place as the parent
    newBodies[h].voxels = std::move(halves[h]);
    newBodies[h].xf = xf;
    float r = 0;
    for (const DebrisVoxel& v : newBodies[h].voxels)
      r = std::max(r, Vec3{(float)v.x, (float)v.y, (float)v.z}.len());
    newBodies[h].radiusVoxels = r + 2.0f;
    newBodies[h].serial = nextSerial_++;
    newBodies[h].owner = newBodies[h].ownerAtCreate = localPlayerId_;
    newBodies[h].bleedMat = b.bleedMat;
    newBodies[h].dead = b.dead;
    newBodies[h].defIndex = b.defIndex;
    RecountBurn(newBodies[h]);
    phys_->SetBodyVelocities(newBodies[h].handle, lin, ang);
  }

  ReleaseBody(b);
  bodies_[bi] = std::move(newBodies[0]);
  bodies_.push_back(std::move(newBodies[1]));
  instancesDirty_ = true;
  return true;
}

bool DebrisSystem::DestroyBody(uint64_t handle) {
  if (!handle) return false;
  for (size_t i = 0; i < bodies_.size(); i++) {
    if (bodies_[i].handle != handle) continue;
    ReleaseBody(bodies_[i]);
    bodies_.erase(bodies_.begin() + i);
    instancesDirty_ = true;
    return true;
  }
  return false;
}

bool DebrisSystem::BodyLatticeOf(uint64_t handle, std::vector<PrefabVoxel>& out,
                                 uint32_t& outScale) const {
  for (const Body& b : bodies_) {
    if (b.handle != handle) continue;
    out.clear();
    if (b.HasFineSkin()) {
      outScale = b.micro.skinScale;
      out = b.skinVoxels;
    } else {
      outScale = b.physScale ? b.physScale : 1u;
      out.reserve(b.voxels.size());
      for (const DebrisVoxel& v : b.voxels)
        out.push_back(PrefabVoxel{(int16_t)v.x, (int16_t)v.y, (int16_t)v.z,
                                  v.payload, v.color});
    }
    return true;
  }
  return false;
}

void DebrisSystem::RefreshLocalBounds(Body& b) {
  if (b.boundsCount == (uint32_t)b.voxels.size()) return;
  b.boundsCount = (uint32_t)b.voxels.size();
  if (b.voxels.empty()) {
    b.lmin[0] = b.lmin[1] = b.lmin[2] = 0;
    b.lmax[0] = b.lmax[1] = b.lmax[2] = 0;
    b.needBlocks.clear();
    b.needDim[0] = b.needDim[1] = b.needDim[2] = 0;
    return;
  }
  int mn[3] = {127, 127, 127}, mx[3] = {-128, -128, -128};
  for (const DebrisVoxel& v : b.voxels) {
    const int c[3] = {v.x, v.y, v.z};
    for (int a = 0; a < 3; a++) {
      if (c[a] < mn[a]) mn[a] = c[a];
      if (c[a] > mx[a]) mx[a] = c[a];
    }
  }
  for (int a = 0; a < 3; a++) {
    b.lmin[a] = (int8_t)mn[a];
    b.lmax[a] = (int8_t)mx[a];
  }
  // The coarse occupancy (Body::needBlocks). An int8 lattice is at most 256
  // a side, 32 blocks, 32^3 bits = 4 KiB worst case; an oak's 59 x 81 x 59
  // is 8 x 11 x 8 = 704 bits.
  int dim[3];
  for (int a = 0; a < 3; a++) {
    dim[a] = (mx[a] - mn[a]) / kNeedBlock + 1;
    b.needDim[a] = (uint8_t)dim[a];
  }
  const size_t nbits = (size_t)dim[0] * (size_t)dim[1] * (size_t)dim[2];
  b.needBlocks.assign((nbits + 63) / 64, 0ull);
  for (const DebrisVoxel& v : b.voxels) {
    const int bx = (v.x - mn[0]) / kNeedBlock, by = (v.y - mn[1]) / kNeedBlock,
              bz = (v.z - mn[2]) / kNeedBlock;
    const size_t i = ((size_t)bz * (size_t)dim[1] + (size_t)by) * (size_t)dim[0] +
                     (size_t)bx;
    b.needBlocks[i >> 6] |= 1ull << (i & 63);
  }
}

void DebrisSystem::ManageTerrain(uint32_t tick, World& world) {
  // Its own row on the Performance tab, debited from the game-logic span it
  // runs inside. This function was the "Game Systems spiked to 50-100 ms while
  // walking" report: it rebuilt every stale patch it wanted in ONE tick, and a
  // chunk-boundary crossing (or a mob acquiring a target, which widens its
  // anchor to navRadius) stales a whole face of them at once — up to
  // World::kFetchPerTick = 64 landing on the same tick, each one a marching-
  // cubes pass, a Jolt mesh tree build and a broadphase add/remove.
  sandvox::PerfSpan span(sandvox::PerfScope::TerrainMesh,
                         sandvox::PerfScope::Debris);
  const WorldSnapshot& snap = world.Snap();
  lastTerrainTick_ = tick;

  // Which chunks need collision right now? Around every dynamic body and this
  // tick's registered mob-limb anchors. Each entry carries the squared distance
  // from the chunk's centre to the anchor that asked for it: that is the BUILD
  // ORDER under the budget below, so the ground under a body is always the
  // first patch made and the far edge of a navigating mob's 30-voxel horizon
  // (which only the A* planner reads, through the fetch) is the last.
  std::vector<std::pair<IVec3, float>>& needed = terrainNeed_;
  needed.clear();
  {
  PhaseTimer ptNeed(prof_, Phase::TerrainNeed);
  // THE BOX A THING CAN ACTUALLY TOUCH, and the distance is measured to that
  // box rather than to a point: a chunk the body is INSIDE sorts at 0, which
  // is what makes the nearest-first budgets below serve the ground under a
  // falling tree before the sky beside its crown. (A point metric measured
  // from `xf.pos` -- the lattice's min corner, not its centre -- ordered a
  // 59 x 81 x 59 crown's chunks by distance from one bottom corner.)
  auto needBoxWorld = [&](Vec3 blo, Vec3 bhi) {
    int lo[3] = {ifloor(blo.x) >> 4, ifloor(blo.y) >> 4, ifloor(blo.z) >> 4};
    int hi[3] = {ifloor(bhi.x) >> 4, ifloor(bhi.y) >> 4, ifloor(bhi.z) >> 4};
    for (int cz = lo[2]; cz <= hi[2]; cz++)
      for (int cy = lo[1]; cy <= hi[1]; cy++)
        for (int cx = lo[0]; cx <= hi[0]; cx++) {
          if (!world.ChunkInWindow({cx, cy, cz})) continue;
          const float c[3] = {(float)(cx * (int)kChunk + (int)kChunk / 2),
                              (float)(cy * (int)kChunk + (int)kChunk / 2),
                              (float)(cz * (int)kChunk + (int)kChunk / 2)};
          const float l[3] = {blo.x, blo.y, blo.z}, h[3] = {bhi.x, bhi.y, bhi.z};
          float d2 = 0;
          for (int a = 0; a < 3; a++) {
            const float e = c[a] < l[a] ? l[a] - c[a]
                                        : (c[a] > h[a] ? c[a] - h[a] : 0.0f);
            d2 += e * e;
          }
          needed.push_back({{cx, cy, cz}, d2});
        }
  };
  // The sphere form, kept for mob-limb anchors: those are human-scale radii
  // whose AABB and bounding sphere are the same handful of chunks, and the
  // caller hands over a radius rather than a lattice.
  auto needAround = [&](Vec3 pos, float radius) {
    const float r = radius + 6.0f;
    needBoxWorld(Vec3{pos.x - r, pos.y - r, pos.z - r},
                 Vec3{pos.x + r, pos.y + r, pos.z + r});
  };
  // A BODY asks around the chunks ITS MATTER lands in, not around its rotated
  // AABB. The AABB was already the fix for a bounding sphere (see the note at
  // kTerrainSkirtVox), but the AABB of a tumbling 81-voxel trunk with a crown
  // at one end is mostly empty: measured 2026-09-12 in the live --fell-tree
  // harness, one oak sat AT kTerrainNeedCeiling (505 of 512 chunks a tick)
  // for as long as it turned, 111k chunk visits over the fall. So the lattice
  // box is diced into kNeedBlock^3 blocks with one bit each (Body::needBlocks,
  // cached with lmin/lmax), and only the SET blocks are transformed: each
  // block's centre goes through the body transform, its rotated extent is the
  // fixed half-diagonal of a cube of that size, and the chunks that box plus
  // the skirt covers are marked in a dense per-body grid holding the least
  // distance from any chunk centre to any block box. A chunk holding matter
  // is therefore still at 0 and the nearest-first budgets below still serve
  // the ground under the body first; what changes is that the sky between a
  // rotated trunk and the corner of its AABB is never listed.
  //
  // `physScale` is collider voxels per world voxel, so the lattice is divided
  // by it exactly as every other body-local -> world conversion in this file.
  // SANDVOX_TERRAIN_NEED_AABB=1 is the 66567da behaviour in the same binary.
  static const bool needAabb =
      std::getenv("SANDVOX_TERRAIN_NEED_AABB") != nullptr;
  std::vector<float>& grid = terrainNeedGrid_;
  auto needBody = [&](Body& b) {
    RefreshLocalBounds(b);
    if (b.voxels.empty()) return;
    const float inv = 1.0f / (float)std::max(1u, b.physScale);
    const float k = kTerrainSkirtVox;
    const Vec3 ex = QuatRot(b.xf.quat, Vec3{1.0f, 0.0f, 0.0f});
    const Vec3 ey = QuatRot(b.xf.quat, Vec3{0.0f, 1.0f, 0.0f});
    const Vec3 ez = QuatRot(b.xf.quat, Vec3{0.0f, 0.0f, 1.0f});
    auto toWorld = [&](float lx, float ly, float lz) {
      return b.xf.pos + ex * lx + ey * ly + ez * lz;
    };
    if (needAabb) {
      Vec3 wlo{1e30f, 1e30f, 1e30f}, whi{-1e30f, -1e30f, -1e30f};
      for (int corner = 0; corner < 8; corner++) {
        const Vec3 w = toWorld(
            ((corner & 1) ? (float)b.lmax[0] + 1.0f : (float)b.lmin[0]) * inv,
            ((corner & 2) ? (float)b.lmax[1] + 1.0f : (float)b.lmin[1]) * inv,
            ((corner & 4) ? (float)b.lmax[2] + 1.0f : (float)b.lmin[2]) * inv);
        wlo.x = std::min(wlo.x, w.x); whi.x = std::max(whi.x, w.x);
        wlo.y = std::min(wlo.y, w.y); whi.y = std::max(whi.y, w.y);
        wlo.z = std::min(wlo.z, w.z); whi.z = std::max(whi.z, w.z);
      }
      needBoxWorld(Vec3{wlo.x - k, wlo.y - k, wlo.z - k},
                   Vec3{whi.x + k, whi.y + k, whi.z + k});
      return;
    }
    // The grid this body may mark: the AABB of the BLOCK grid (whose far
    // corner is past lmax when an edge block is partly empty), plus skirt.
    const int dim[3] = {b.needDim[0], b.needDim[1], b.needDim[2]};
    Vec3 wlo{1e30f, 1e30f, 1e30f}, whi{-1e30f, -1e30f, -1e30f};
    for (int corner = 0; corner < 8; corner++) {
      float lc[3];
      for (int a = 0; a < 3; a++)
        lc[a] = ((float)b.lmin[a] +
                 ((corner >> a) & 1 ? (float)(dim[a] * kNeedBlock) : 0.0f)) *
                inv;
      const Vec3 w = toWorld(lc[0], lc[1], lc[2]);
      wlo.x = std::min(wlo.x, w.x); whi.x = std::max(whi.x, w.x);
      wlo.y = std::min(wlo.y, w.y); whi.y = std::max(whi.y, w.y);
      wlo.z = std::min(wlo.z, w.z); whi.z = std::max(whi.z, w.z);
    }
    const int glo[3] = {ifloor(wlo.x - k) >> 4, ifloor(wlo.y - k) >> 4,
                        ifloor(wlo.z - k) >> 4};
    const int ghi[3] = {ifloor(whi.x + k) >> 4, ifloor(whi.y + k) >> 4,
                        ifloor(whi.z + k) >> 4};
    const int gn[3] = {ghi[0] - glo[0] + 1, ghi[1] - glo[1] + 1,
                       ghi[2] - glo[2] + 1};
    const size_t cells = (size_t)gn[0] * (size_t)gn[1] * (size_t)gn[2];
    if (cells > kTerrainNeedGridCells) {  // cannot happen for an int8 lattice
      needBoxWorld(Vec3{wlo.x - k, wlo.y - k, wlo.z - k},
                   Vec3{whi.x + k, whi.y + k, whi.z + k});
      return;
    }
    grid.assign(cells, 1e30f);
    // A rotated cube's AABB half-extent per world axis is the half-side times
    // the sum of that row's absolute basis components; every block is the
    // same cube, so this is three numbers per body, not per block.
    const float hb = 0.5f * (float)kNeedBlock * inv;
    const float h[3] = {
        hb * (std::fabs(ex.x) + std::fabs(ey.x) + std::fabs(ez.x)),
        hb * (std::fabs(ex.y) + std::fabs(ey.y) + std::fabs(ez.y)),
        hb * (std::fabs(ex.z) + std::fabs(ey.z) + std::fabs(ez.z))};
    for (int bz = 0; bz < dim[2]; bz++)
      for (int by = 0; by < dim[1]; by++)
        for (int bx = 0; bx < dim[0]; bx++) {
          const size_t i = ((size_t)bz * (size_t)dim[1] + (size_t)by) *
                               (size_t)dim[0] + (size_t)bx;
          if (((b.needBlocks[i >> 6] >> (i & 63)) & 1ull) == 0ull) continue;
          if (prof_.on) prof_.needBlocks++;
          const Vec3 c = toWorld(
              ((float)b.lmin[0] + ((float)bx + 0.5f) * (float)kNeedBlock) * inv,
              ((float)b.lmin[1] + ((float)by + 0.5f) * (float)kNeedBlock) * inv,
              ((float)b.lmin[2] + ((float)bz + 0.5f) * (float)kNeedBlock) * inv);
          const float blo[3] = {c.x - h[0], c.y - h[1], c.z - h[2]};
          const float bhi[3] = {c.x + h[0], c.y + h[1], c.z + h[2]};
          int clo[3], chi[3];
          for (int a = 0; a < 3; a++) {
            clo[a] = std::max(glo[a], ifloor(blo[a] - k) >> 4);
            chi[a] = std::min(ghi[a], ifloor(bhi[a] + k) >> 4);
          }
          for (int cz = clo[2]; cz <= chi[2]; cz++)
            for (int cy = clo[1]; cy <= chi[1]; cy++)
              for (int cx = clo[0]; cx <= chi[0]; cx++) {
                const float cc[3] = {
                    (float)(cx * (int)kChunk + (int)kChunk / 2),
                    (float)(cy * (int)kChunk + (int)kChunk / 2),
                    (float)(cz * (int)kChunk + (int)kChunk / 2)};
                float d2 = 0;
                for (int a = 0; a < 3; a++) {
                  const float e = cc[a] < blo[a] ? blo[a] - cc[a]
                                  : (cc[a] > bhi[a] ? cc[a] - bhi[a] : 0.0f);
                  d2 += e * e;
                }
                float& g = grid[((size_t)(cz - glo[2]) * (size_t)gn[1] +
                                 (size_t)(cy - glo[1])) * (size_t)gn[0] +
                                (size_t)(cx - glo[0])];
                if (d2 < g) g = d2;
              }
        }
    for (int cz = 0; cz < gn[2]; cz++)
      for (int cy = 0; cy < gn[1]; cy++)
        for (int cx = 0; cx < gn[0]; cx++) {
          const float g = grid[((size_t)cz * (size_t)gn[1] + (size_t)cy) *
                                   (size_t)gn[0] + (size_t)cx];
          if (g >= 1e30f) continue;
          const IVec3 wc{glo[0] + cx, glo[1] + cy, glo[2] + cz};
          if (!world.ChunkInWindow(wc)) continue;
          needed.push_back({wc, g});
        }
  };
  // A MOB'S PLANNING HORIZON IS LISTED ON A STRIDE. The core anchor (the
  // creature's own body, radius + 6) is collision and goes through needAround
  // every tick as it always did. The horizon (navRadius + 4, ~216 chunks for
  // a 30-voxel planner) exists so the A* search reads fetched chunks instead
  // of UNKNOWN, and a chunk 30 voxels from a standing creature does not need
  // its staleness re-examined 30 times a second: each horizon chunk is listed
  // on one tick in kTerrainHorizonStride, phased by a hash of its coordinate
  // so the work is spread evenly rather than bursting. Nothing is dropped
  // (an entry that missed a tick is listed on its next one, and patches
  // survive kTerrainEvictTicks), a fresh horizon still arrives at the fetch
  // budget's pace, and the ceiling's tail -- the horizon, by distance -- is
  // that much shorter, so a big body's own chunks are never behind it.
  // SANDVOX_TERRAIN_HORIZON_STRIDE=1 is the every-tick behaviour.
  static const uint32_t horizonStride = [] {
    if (const char* e = std::getenv("SANDVOX_TERRAIN_HORIZON_STRIDE")) {
      const long n = std::strtol(e, nullptr, 10);
      if (n >= 1) return (uint32_t)n;
    }
    return kTerrainHorizonStride;
  }();
  auto needHorizon = [&](Vec3 pos, float coreRadius, float horizon) {
    const float rc = coreRadius + 6.0f;  // needAround's box, already listed
    if (horizon <= rc) return;
    const int clo[3] = {ifloor(pos.x - rc) >> 4, ifloor(pos.y - rc) >> 4,
                        ifloor(pos.z - rc) >> 4};
    const int chi[3] = {ifloor(pos.x + rc) >> 4, ifloor(pos.y + rc) >> 4,
                        ifloor(pos.z + rc) >> 4};
    const int lo[3] = {ifloor(pos.x - horizon) >> 4, ifloor(pos.y - horizon) >> 4,
                       ifloor(pos.z - horizon) >> 4};
    const int hi[3] = {ifloor(pos.x + horizon) >> 4, ifloor(pos.y + horizon) >> 4,
                       ifloor(pos.z + horizon) >> 4};
    for (int cz = lo[2]; cz <= hi[2]; cz++)
      for (int cy = lo[1]; cy <= hi[1]; cy++)
        for (int cx = lo[0]; cx <= hi[0]; cx++) {
          if (cx >= clo[0] && cx <= chi[0] && cy >= clo[1] && cy <= chi[1] &&
              cz >= clo[2] && cz <= chi[2])
            continue;  // the core box
          const IVec3 wc{cx, cy, cz};
          if (!world.ChunkInWindow(wc)) continue;
          const uint64_t key = World::PackChunkKey(wc) * 0x9E3779B97F4A7C15ull;
          if ((((uint32_t)(key >> 58) + tick) % horizonStride) != 0u) continue;
          const float dx = (float)(cx * (int)kChunk + (int)kChunk / 2) - pos.x;
          const float dy = (float)(cy * (int)kChunk + (int)kChunk / 2) - pos.y;
          const float dz = (float)(cz * (int)kChunk + (int)kChunk / 2) - pos.z;
          needed.push_back({wc, dx * dx + dy * dy + dz * dz});
          if (prof_.on) prof_.anchorChunks++;
        }
  };
  // A FALLING BODY NEEDS THE GROUND IT IS ABOUT TO REACH.
  //
  // needAround's box reaches radius + 6 voxels, i.e. about one chunk past a
  // human-sized body, and a patch is not free on demand: the chunk fetch is
  // async and the polygonize then waits for a slot in kTerrainBuildsPerTick.
  // A ragdoll three seconds into a fall covers that whole margin in three
  // ticks and was arriving in chunks with no collision mesh in them at all.
  // So ALSO ask for the chunks the body's own velocity says it is entering,
  // sampled along the swept segment (just those chunks -- a box around each
  // would multiply the fetch budget by the lookahead). They carry the distance
  // from the SAMPLE, not from the body, so the build order below puts the
  // ground a body is falling onto ahead of the ground it has already left.
  const bool lookahead = !AntiTunnelOff(AntiTunnel::Lookahead);
  auto sweepAhead = [&](Vec3 pos, Vec3 vel) {
    const float speed = vel.len();
    if (!lookahead || speed < 1e-3f) return;
    const float reach =
        std::min(speed * kTerrainLookaheadSeconds, kTerrainLookaheadVox);
    if (reach < (float)kChunk * 0.5f) return;
    const Vec3 dir = vel * (1.0f / speed);
    for (float t = (float)kChunk * 0.5f; t <= reach; t += (float)kChunk * 0.5f) {
      const Vec3 p = pos + dir * t;
      const IVec3 wc{ifloor(p.x) >> 4, ifloor(p.y) >> 4, ifloor(p.z) >> 4};
      if (!world.ChunkInWindow(wc)) continue;
      const float dx = (float)(wc.x * (int)kChunk + (int)kChunk / 2) - p.x;
      const float dy = (float)(wc.y * (int)kChunk + (int)kChunk / 2) - p.y;
      const float dz = (float)(wc.z * (int)kChunk + (int)kChunk / 2) - p.z;
      needed.push_back({wc, dx * dx + dy * dy + dz * dz});
    }
  };
  for (Body& b : bodies_) {
    // A sleeping body is not going anywhere and is not worth a Jolt lookup;
    // GetBodyVelocities already answers in VOXELS per second.
    Vec3 lin{}, ang{};
    if (phys_->IsActive(b.handle)) phys_->GetBodyVelocities(b.handle, lin, ang);
    needBody(b);
    sweepAhead(b.xf.pos, lin);
  }
  for (const Anchor& a : extraAnchors_) {
    const size_t before = needed.size();
    needAround(a.pos, a.radius);
    if (prof_.on) prof_.anchorChunks += needed.size() - before;
    needHorizon(a.pos, a.radius, a.horizon);
    sweepAhead(a.pos, a.vel);
  }
  extraAnchors_.clear();
  // Dedupe by chunk, keeping the NEAREST distance any anchor gave it, then
  // order nearest-first. Sorted by (key, d2) so unique's survivor is the
  // minimum; stable so equal distances keep a deterministic order (this runs
  // under the selftest, whose debris gates compare against a baseline).
  std::sort(needed.begin(), needed.end(),
            [](const std::pair<IVec3, float>& a,
               const std::pair<IVec3, float>& b) {
              const uint64_t ka = World::PackChunkKey(a.first);
              const uint64_t kb = World::PackChunkKey(b.first);
              return ka != kb ? ka < kb : a.second < b.second;
            });
  needed.erase(std::unique(needed.begin(), needed.end(),
                           [](const std::pair<IVec3, float>& a,
                              const std::pair<IVec3, float>& b) {
                             return a.first.x == b.first.x &&
                                    a.first.y == b.first.y &&
                                    a.first.z == b.first.z;
                           }),
               needed.end());
  std::stable_sort(needed.begin(), needed.end(),
                   [](const std::pair<IVec3, float>& a,
                      const std::pair<IVec3, float>& b) {
                     return a.second < b.second;
                   });
  // THE LIST CEILING. Sorted nearest-first above, so the tail is the chunks
  // furthest from anything that asked -- a distant mob's navigation horizon,
  // the sky beside a crown. Cutting it here bounds the per-chunk staleness
  // scan below at O(kTerrainNeedCeiling) whatever the scene holds, and costs
  // only latency: nothing is forgotten, because a chunk still needed next tick
  // is listed again and its patch (if it has one) survives kTerrainEvictTicks.
  if (needed.size() > kTerrainNeedCeiling) needed.resize(kTerrainNeedCeiling);
  }  // end Phase::TerrainNeed

  uint32_t builds = 0;
  uint32_t fetchesThisTick = 0;
  uint32_t gathers = 0;
  if (prof_.on) {
    prof_.chunksNeeded += needed.size();
    if (needed.size() > prof_.maxNeededOneTick)
      prof_.maxNeededOneTick = (uint32_t)needed.size();
  }
  for (const auto& need : needed) {
    const IVec3 wc = need.first;
    PhaseSwitch pst(prof_, Phase::TerrainScan);
    TerrainEntry& t = terrain_[World::PackChunkKey(wc)];
    t.wc = wc;
    t.lastNeeded = tick;
    const CachedChunk* cc = world.Cached(wc);
    if (!cc) {
      // THE FETCH BUDGET. World::kFetchPerTick is 64 for the whole engine and
      // a felled tree used to ask for 454 in one tick; everything else that
      // needs the mirror -- the player's own 3x3x3, an island scan's
      // EventReady, another body's ground -- then waited behind it. Asking for
      // fewer than arrive costs nothing: the queue is deduped and the chunks
      // this tick declined are the head of the next sweep.
      if (fetchesThisTick < kTerrainFetchPerTick) {
        world.RequestChunkFetch(wc, World::FetchSource::Terrain);
        fetchesThisTick++;
      }
      continue;
    }
    // A chunk this system wrote into is re-fetched until the mirror reflects
    // the write, WHATEVER the dirty flags say. The dirty-gated refresh below
    // is for the CA's own activity; a vacated chunk is asleep again before
    // its 8-tick window opens, and the collider then kept a body's former
    // cells as ground for as long as the body sat there (gate `cactus-fell`).
    {
      auto wt = chunkWriteTick_.find(World::PackChunkKey(wc));
      if (wt != chunkWriteTick_.end()) {
        if (cc->version < wt->second) {
          if (fetchesThisTick < kTerrainFetchPerTick) {
            world.RequestChunkFetch(wc, World::FetchSource::Terrain);
            fetchesThisTick++;
          }
        } else {
          chunkWriteTick_.erase(wt);
        }
      }
    }
    // The pending-vacate lists this mesh will be built against: this chunk's
    // and its 26 neighbours' (the border ring of the occupancy). Lists the
    // mirror has caught up with are dropped here; the rest form an identity
    // that forces a rebuild when it changes.
    // The 27 lookups are skipped outright while no write is pending, which is
    // every tick but the few after a cut. Measured headless on tree-fell's
    // cut pass (2026-09-12): 0.28 us a visit with the loop, 0.19-0.22 without
    // -- a fifth of a visit that is mostly two other hash lookups, not the
    // bulk of it. SANDVOX_TERRAIN_VACATE_SCAN=1 is the old unconditional loop.
    static const bool vacateScanAlways =
        std::getenv("SANDVOX_TERRAIN_VACATE_SCAN") != nullptr;
    uint64_t vacateKey = 0;
    if (vacateScanAlways || !pendingVacate_.empty())
    for (int ncz = -1; ncz <= 1; ncz++)
      for (int ncy = -1; ncy <= 1; ncy++)
        for (int ncx = -1; ncx <= 1; ncx++) {
          const IVec3 nwc{wc.x + ncx, wc.y + ncy, wc.z + ncz};
          const uint64_t nk = World::PackChunkKey(nwc);
          auto pv = pendingVacate_.find(nk);
          if (pv == pendingVacate_.end()) continue;
          const CachedChunk* n = world.Cached(nwc);
          if (n && n->version >= pv->second.tick) {
            pendingVacate_.erase(pv);
            continue;
          }
          vacateKey ^= (nk * 0x9E3779B97F4A7C15ull) ^
                       ((uint64_t)pv->second.stamp * 0xC2B2AE3D27D4EB4Full);
        }
    const bool vacateChanged = vacateKey != t.vacateKey;
    // refresh when the sim says the chunk changed (rate-limited). dirtyFlags
    // are slot-indexed under the CURRENT window origin.
    uint32_t slot = World::SlotChunkIndex(wc);
    if (slot < snap.dirtyFlags.size() && snap.dirtyFlags[slot] &&
        tick > t.lastRefreshReq + kTerrainRefreshTicks) {
      if (fetchesThisTick < kTerrainFetchPerTick) {
        t.lastRefreshReq = tick;
        world.RequestChunkFetch(wc, World::FetchSource::Terrain);
        fetchesThisTick++;
      }
    }
    if (!vacateChanged && t.builtVersion >= cc->version && t.handle != 0) continue;
    if (!vacateChanged && t.builtVersion >= cc->version && t.handle == 0 &&
        t.builtVersion != 0)
      continue;  // built empty at this version

    // ---- THE BUDGET ------------------------------------------------------
    // A stale patch that does not fit this tick stays stale (builtVersion
    // behind the cache) and is the first thing the next tick's sweep finds,
    // nearest-first. A COUNT, not a time budget, on purpose: the debris gates
    // run this under the selftest and a body that lands on a patch one tick
    // later on a slower machine would settle somewhere else. What the count
    // limits is REAL rebuilds — the unchanged-surface early-out below is a
    // hash compare and costs nothing against it.
    // The budget check has moved BELOW the occupancy gather: that is an 18^3
    // read of the mirror, and a chunk of sky (most of what surrounds a felled
    // tree, whose radius reaches ten chunks) yields nothing to mesh and costs
    // the budget nothing. Only a real polygonize is a build.

    // (re)build the marching-cubes patch. Solids AND powders carry weight;
    // liquids don't (debris sinks). Missing neighbor chunks sample as empty —
    // transient until their fetch lands.
    //
    // Sample occupancy into an 18^3 bitmask up front, one source chunk at a
    // time: the 5832 samples touch at most 27 chunks, so the chunk-cache hash
    // lookup happens 27 times instead of once per sample (and the polygonizer
    // then reads bits, not a std::function).
    // THE GATHER BUDGET. The 18^3 occupancy read below is ~17 us and the
    // surface-identity hash cannot be computed without it, so a tick in which
    // 87 chunks all went stale at once paid 1.5 ms before deciding that 80 of
    // them had not moved. Deferring costs the same as deferring a build: the
    // entry keeps its old builtVersion and is re-examined, nearest-first, next
    // tick. Charged ahead of the build budget because a gather that ends in
    // "identical surface" or "sky" never reaches that one.
    if (gathers >= kTerrainGatherPerTick) {
      settle_.terrainGatherDeferred++;
      continue;
    }
    gathers++;
    IVec3 origin{wc.x * (int)kChunk, wc.y * (int)kChunk, wc.z * (int)kChunk};
    pst.To(Phase::TerrainGather);
    if (prof_.on) prof_.gathers++;
    uint32_t occ[kMcOccWords] = {};
    for (int ncz = -1; ncz <= 1; ncz++)
      for (int ncy = -1; ncy <= 1; ncy++)
        for (int ncx = -1; ncx <= 1; ncx++) {
          IVec3 nwc{wc.x + ncx, wc.y + ncy, wc.z + ncz};
          // the sub-box of this neighbor chunk that lands inside the 18^3 box,
          // in occ coords (chunk-local + 1)
          int blo[3], bhi[3];
          const int nc[3] = {ncx, ncy, ncz};
          for (int a = 0; a < 3; a++) {
            blo[a] = nc[a] < 0 ? 0 : (nc[a] == 0 ? 1 : kMcOccDim - 1);
            bhi[a] = nc[a] < 0 ? 0 : (nc[a] == 0 ? kMcOccDim - 2 : kMcOccDim - 1);
          }
          bool inWin = world.ChunkInWindow(nwc);
          const CachedChunk* n = inWin ? world.Cached(nwc) : nullptr;
          bool haveVox = n && n->voxels.size() == kChunkVol;
          // Outside the residency window is solid and inert (matches the sim
          // rule); in-window but not yet fetched reads as empty until it lands.
          if (!inWin) {
            for (int z = blo[2]; z <= bhi[2]; z++)
              for (int y = blo[1]; y <= bhi[1]; y++)
                for (int x = blo[0]; x <= bhi[0]; x++) McOccSet(occ, x, y, z);
            continue;
          }
          if (!haveVox) continue;
          for (int z = blo[2]; z <= bhi[2]; z++)
            for (int y = blo[1]; y <= bhi[1]; y++)
              for (int x = blo[0]; x <= bhi[0]; x++) {
                // occ coord -> world cell -> local cell in THIS neighbor
                int lx = (origin.x + x - 1) & 15, ly = (origin.y + y - 1) & 15,
                    lz = (origin.z + z - 1) & 15;
                uint32_t mat =
                    n->voxels[(lz * kChunk + ly) * kChunk + lx] & 0xFFF;
                if (mat == 0 || mat >= classOf_.size()) continue;
                if (classOf_[mat] == CLASS_SOLID || classOf_[mat] == CLASS_POWDER)
                  McOccSet(occ, x, y, z);
              }
          // This system's own writes the mirror copy does not show yet: a
          // vacated cell is not ground, a settled one is. See NoteGridWrite.
          auto pv = pendingVacate_.find(World::PackChunkKey(nwc));
          if (pv != pendingVacate_.end()) {
            for (const auto& [li, word] : pv->second.cells) {
              const int ox = nwc.x * (int)kChunk + (int)(li % kChunk) - origin.x + 1;
              const int oy = nwc.y * (int)kChunk + (int)((li / kChunk) % kChunk) - origin.y + 1;
              const int oz = nwc.z * (int)kChunk + (int)(li / (kChunk * kChunk)) - origin.z + 1;
              if (ox < 0 || oy < 0 || oz < 0 || ox >= kMcOccDim || oy >= kMcOccDim ||
                  oz >= kMcOccDim)
                continue;
              const uint32_t m = word & 0xFFFu;
              const bool matter = m != 0 && m < classOf_.size() &&
                                  (classOf_[m] == CLASS_SOLID || classOf_[m] == CLASS_POWDER);
              if (matter) McOccSet(occ, ox, oy, oz);
              else McOccClear(occ, ox, oy, oz);
            }
          }
        }

    // ---- IDENTICAL COLLISION SURFACE => NO MESH, NO JOLT, NO WAKE ---------
    // The mesh is a pure function of (origin, occ), so the occupancy box IS the
    // surface's identity, and it is known BEFORE the polygonizer runs. The old
    // check hashed the mesh bytes AFTER building them, which caught the "liquid
    // flowed, blood dried, gas moved" case (do NOT wake sleeping bodies) but
    // still paid the marching cubes for it — and every chunk with a dirty flag
    // is re-fetched on an 8-tick cadence whether or not its solids moved, so
    // that was most of the rebuilds under a settling world. Hashing 183 words
    // instead of ~50 KB of mesh is the smaller half of the win.
    uint64_t h = 1469598103934665603ull;  // FNV-1a over the occupancy words
    bool anyOcc = false;
    for (uint32_t i = 0; i < kMcOccWords; i++) {
      h = (h ^ occ[i]) * 1099511628211ull;
      anyOcc = anyOcc || occ[i] != 0u;
    }
    if (t.builtVersion != 0 && h == t.occHash) {
      t.builtVersion = cc->version;
      t.vacateKey = vacateKey;
      settle_.terrainSame++;
      continue;
    }
    if (!anyOcc) {  // sky: nothing to mesh, nothing to charge
      if (t.handle) phys_->RemoveBody(t.handle);
      t.handle = 0;
      t.builtVersion = cc->version;
      t.occHash = h;
      t.vacateKey = vacateKey;
      continue;
    }
    if (builds >= TerrainBuildsPerTick()) {
      settle_.terrainDeferred++;
      continue;
    }
    builds++;
    settle_.terrainBuilds++;

    pst.To(Phase::TerrainPoly);
    if (prof_.on) prof_.polys++;
    std::vector<float> verts;
    std::vector<uint32_t> indices;
    PolygonizeChunk(origin, occ, verts, indices);

    pst.To(Phase::TerrainJolt);
    if (prof_.on) prof_.joltMeshes++;
    if (t.handle) phys_->RemoveBody(t.handle);
    t.handle = indices.empty() ? 0 : phys_->CreateTerrainMesh(verts, indices);
    t.builtVersion = cc->version;
    t.occHash = h;
    t.vacateKey = vacateKey;
    // ground under sleeping debris may have moved: let them re-settle
    settle_.terrainWakes++;
    settle_.lastWakeTick = tick;
    settle_.lastWakeChunk = wc;
    phys_->WakeNear(Vec3{(float)origin.x + 8, (float)origin.y + 8,
                         (float)origin.z + 8},
                    24.0f);
  }

  if (prof_.on) {
    prof_.fetchesAsked += fetchesThisTick;
    if (fetchesThisTick > prof_.maxFetchOneTick)
      prof_.maxFetchOneTick = fetchesThisTick;
  }
  // evict patches nothing has needed for a while
  PhaseTimer ptEvict(prof_, Phase::TerrainEvict);
  for (auto it = terrain_.begin(); it != terrain_.end();) {
    if (it->second.lastNeeded + kTerrainEvictTicks < tick) {
      if (it->second.handle) phys_->RemoveBody(it->second.handle);
      it = terrain_.erase(it);
    } else {
      ++it;
    }
  }
}

bool DebrisSystem::ColliderVouched(IVec3 wc) const {
  // Outside the residency window there is no sim, no voxel data and no body
  // that survives arriving (PostStep despawns past kPad), and the occupancy
  // sampler above reads it as solid — nothing to vouch for.
  if (world_ == nullptr || !world_->ChunkInWindow(wc)) return true;
  auto it = terrain_.find(World::PackChunkKey(wc));
  // builtVersion != 0 is "polygonized at least once from real voxels", which
  // is the same three-way split TerrainCensus reports: a patch with triangles
  // and a patch that came out EMPTY both vouch, an unfetched one does not.
  return it != terrain_.end() && it->second.builtVersion != 0;
}

bool DebrisSystem::UntunnelBody(uint64_t handle, const Vec3& prevPosVoxel) {
  if (handle == 0 || phys_ == nullptr || world_ == nullptr) return false;
  if (AntiTunnelOff(AntiTunnel::Clamp)) return false;
  BodyTransform now{};
  if (!phys_->GetTransform(handle, now)) return false;
  auto chunkOf = [](const Vec3& p) {
    return IVec3{ifloor(p.x) >> 4, ifloor(p.y) >> 4, ifloor(p.z) >> 4};
  };
  const IVec3 endChunk = chunkOf(now.pos);
  if (ColliderVouched(endChunk)) {
    untunnelHold_.erase(handle);
    return false;
  }
  // Already out there before the step: let it go. A body that spawned in an
  // unvouched chunk, or whose patch was evicted from under it, must not be
  // pinned where it stands — that is the deadlock the player's first blind-fall
  // fix walked into, and the reason this is a clamp and not a veto.
  if (!ColliderVouched(chunkOf(prevPosVoxel))) {
    untunnelHold_.erase(handle);
    return false;
  }
  uint8_t& held = untunnelHold_[handle];
  if (held == 0) untunnel_.bodiesHeld++;
  if (held >= kUntunnelHoldTicks) {
    // THE RELEASE IS THE BUG, not the hold: the patches never arrived. Report
    // it once per release and get out of the body's way.
    untunnel_.released++;
    untunnelHold_.erase(handle);
    std::printf("untunnel: released body %llu into unvouched chunk "
                "(%d, %d, %d) after %u steps held\n",
                (unsigned long long)handle, endChunk.x, endChunk.y, endChunk.z,
                (unsigned)kUntunnelHoldTicks);
    return false;
  }
  held++;

  // Walk the step and keep the last sample that was vouched. Sample 0 is
  // `prevPosVoxel`, which the test above proved vouched, so there is always an
  // answer and the worst case is putting the body back where it started.
  const Vec3 delta = now.pos - prevPosVoxel;
  const float len = delta.len();
  const int samples =
      std::clamp((int)std::ceil(len), 1, kUntunnelSamples);
  Vec3 last = prevPosVoxel;
  for (int i = 1; i <= samples; i++) {
    const Vec3 p = prevPosVoxel + delta * ((float)i / (float)samples);
    if (!ColliderVouched(chunkOf(p))) break;
    last = p;
  }
  phys_->SetBodyPosition(handle, last);
  untunnel_.holds++;
  untunnel_.maxStepVox = std::max(untunnel_.maxStepVox, len);
  untunnel_.lastChunk = endChunk;
  return true;
}

bool DebrisSystem::UntunnelRig(const std::vector<uint64_t>& handles,
                               const std::vector<Vec3>& prevPosVoxel) {
  if (phys_ == nullptr || world_ == nullptr) return false;
  if (AntiTunnelOff(AntiTunnel::Clamp)) return false;
  const size_t n = std::min(handles.size(), prevPosVoxel.size());
  if (n == 0) return false;
  auto chunkOf = [](const Vec3& p) {
    return IVec3{ifloor(p.x) >> 4, ifloor(p.y) >> 4, ifloor(p.z) >> 4};
  };
  // ONE CHUNK LOOKUP MEMO for the whole call. The samples below are walked per
  // body along a segment, so consecutive tests hit the same chunk over and
  // over; without this a 35-body rig at speed costs 35 * 64 hash finds in the
  // starvation window. One entry is enough because the coherence is WITHIN a
  // body's walk, which is where all the repetition is.
  uint64_t memoKey = ~0ull;
  bool memoVouched = false;
  auto vouched = [&](const Vec3& p) {
    const IVec3 wc = chunkOf(p);
    const uint64_t key = World::PackChunkKey(wc);
    if (key == memoKey) return memoVouched;
    memoKey = key;
    memoVouched = ColliderVouched(wc);
    return memoVouched;
  };

  // Read every member's end-of-step position first: the decision needs all of
  // them, and a handle that died mid-tick drops out of the rig rather than
  // failing the whole clamp.
  std::vector<Vec3> now(n);
  std::vector<bool> live(n, false);
  float maxLen = 0.0f;
  bool anyStartedOut = false, anyEndedOut = false;
  for (size_t i = 0; i < n; i++) {
    if (handles[i] == 0) continue;
    BodyTransform xf{};
    if (!phys_->GetTransform(handles[i], xf)) continue;
    live[i] = true;
    now[i] = xf.pos;
    maxLen = std::max(maxLen, (xf.pos - prevPosVoxel[i]).len());
    if (!vouched(prevPosVoxel[i])) anyStartedOut = true;
    if (!vouched(xf.pos)) anyEndedOut = true;
  }
  // THE HOLD IS KEYED ON THE FIRST LIVE MEMBER, not on handles[0]: a rig that
  // loses its root limb mid-fall would otherwise key on handle 0 and share one
  // counter with every other rig in that state.
  uint64_t key = 0;
  for (size_t i = 0; i < n; i++)
    if (live[i]) { key = handles[i]; break; }
  if (key == 0) return false;
  // Nothing left the patches: the common case, and it must cost no more than
  // the reads above.
  if (!anyEndedOut) {
    untunnelHold_.erase(key);
    return false;
  }
  // Already out there before the step — see the rig-scope note in debris.h.
  if (anyStartedOut) {
    untunnelHold_.erase(key);
    return false;
  }
  uint8_t& held = untunnelHold_[key];
  if (held == 0) untunnel_.bodiesHeld++;
  if (held >= kUntunnelHoldTicks) {
    untunnel_.released++;
    untunnelHold_.erase(key);
    std::printf("untunnel: released rig (%zu bodies, lead %llu) into unvouched "
                "space after %u steps held\n",
                n, (unsigned long long)key, (unsigned)kUntunnelHoldTicks);
    return false;
  }
  held++;

  // The fraction of this step every member can still vouch for. Walked on a
  // COMMON sample grid sized by the longest displacement in the rig, so the
  // fractions are directly comparable and the resolution is set by the body
  // that moved furthest.
  const int samples = std::clamp((int)std::ceil(maxLen), 1, kUntunnelSamples);
  float frac = 1.0f;
  for (size_t i = 0; i < n && frac > 0.0f; i++) {
    if (!live[i]) continue;
    const Vec3 delta = now[i] - prevPosVoxel[i];
    float mine = 1.0f;
    for (int s = 1; s <= samples; s++) {
      const float f = (float)s / (float)samples;
      if (f > frac) break;   // a stricter member has already cut it back here
      if (!vouched(prevPosVoxel[i] + delta * f)) {
        mine = (float)(s - 1) / (float)samples;
        break;
      }
    }
    frac = std::min(frac, mine);
  }

  for (size_t i = 0; i < n; i++) {
    if (!live[i]) continue;
    phys_->SetBodyPosition(handles[i],
                           prevPosVoxel[i] + (now[i] - prevPosVoxel[i]) * frac);
  }
  untunnel_.rigHolds++;
  untunnel_.minRigFrac = std::min(untunnel_.minRigFrac, frac);
  untunnel_.maxStepVox = std::max(untunnel_.maxStepVox, maxLen);
  for (size_t i = 0; i < n; i++)
    if (live[i] && !vouched(now[i])) { untunnel_.lastChunk = chunkOf(now[i]); break; }
  return true;
}

// The piece's MOST COMMON material, not the first voxel's: an island is
// usually one substance, but a burnt stem carries a few ash voxels and a wall
// a few of whatever hit it, and picking voxel 0 would let that minority decide
// what the break or the impact sounds like. Counted over a small map rather
// than a full histogram because the voxel count is bounded by the body cap.
uint32_t DebrisSystem::DominantMaterial(const std::vector<DebrisVoxel>& voxels) {
  // A flat tally over the 12-bit id space, not a hash map: this runs on every
  // lattice rewrite (RecountBurn) and a felled tree is 35k voxels of it.
  std::array<uint32_t, 4096> tally{};
  for (const DebrisVoxel& v : voxels) tally[v.payload & 0xFFFu]++;
  uint32_t domMat = 0, domCount = 0;
  // Ascending id with a strict `>`: ties break toward the lower id, for
  // stability of the REPORT -- the world hash never sees this, but an unstable
  // pick would make the cue flip between runs and a bug here hard to repeat.
  for (uint32_t mat = 0; mat < 4096u; mat++) {
    if (tally[mat] > domCount) {
      domMat = mat;
      domCount = tally[mat];
    }
  }
  return domMat;
}

// Turn this step's Jolt contacts into audible impacts (DESIGN.md §12b).
//
// WHAT IS ALREADY FILTERED OUT BEFORE WE GET HERE. Physics' contact listener
// only reports NEW manifolds (a landing, never a rest), only above the speed
// gate, and never anything touching the player proxy or a Layers::AVATAR limb.
// So a settled pile of debris and a walking player both cost zero here — which
// is the CLAUDE.md rule 2 property this hook has to have.
//
// A LIVE MOB'S LIMB IS NOT DEBRIS, and gets dropped for free: limb bodies are
// owned by MobSystem, so the handle lookup below misses and the contact is
// discarded. A SEVERED limb has been AdoptBody'd into bodies_ by then, so it
// does start thudding — which is exactly right.
void DebrisSystem::CollectImpacts() {
  const std::vector<Physics::ContactImpact>& raw = phys_->ContactImpacts();
  if (raw.empty()) return;
  const Tuning::Audio& ta = CurrentTuning().audio;
  // Voxels/sec. The gate the listener already applied is the same number; this
  // is the top of the ramp, and it must stay above the gate or every impact
  // reports full energy.
  const float fullVox = std::max(ta.impactFullSpeed, ta.impactMinSpeed + 0.1f) /
                        kVoxelMeters;
  const float minVox = ta.impactMinSpeed / kVoxelMeters;
  const uint32_t gapSteps =
      (uint32_t)std::max(1.0f, ta.impactMinGap * kSimTicksPerSecond);

  // grid material at a world cell via the chunk cache, the same read the body
  // burn uses. Chunks around live bodies are kept fetched by the terrain
  // meshing, so a body landing on terrain almost always resolves; a miss reads
  // as air and falls through to the body material below.
  auto worldMatAt = [&](Vec3 p) -> uint32_t {
    IVec3 c{ifloor(p.x), ifloor(p.y), ifloor(p.z)};
    if (!world_->CellInWindow(c)) return 0u;
    const CachedChunk* cc = world_->Cached(ChunkOfCell(c.x, c.y, c.z));
    if (!cc || cc->voxels.size() != kChunkVol) return 0u;
    uint32_t lx = (uint32_t)(c.x & 15), ly = (uint32_t)(c.y & 15),
             lz = (uint32_t)(c.z & 15);
    return cc->voxels[(lz * kChunk + ly) * kChunk + lx] & 0xFFFu;
  };
  auto bodyIndexOf = [&](uint64_t h) -> size_t {
    if (h == 0) return (size_t)-1;
    for (size_t i = 0; i < bodies_.size(); i++)
      if (bodies_[i].handle == h) return i;
    return (size_t)-1;
  };

  // Candidates for this step, before the per-step cap.
  std::vector<ImpactEvent> cand;
  for (const Physics::ContactImpact& ci : raw) {
    const size_t ia = bodyIndexOf(ci.bodyA), ib = bodyIndexOf(ci.bodyB);
    if (ia == (size_t)-1 && ib == (size_t)-1) continue;  // neither is debris

    // Per-body gap. Charged to every debris body in the contact, so a rock
    // bouncing down a slope is one thud per bounce group rather than one per
    // manifold, and two bodies clattering together do not each report it.
    bool fresh = false;
    for (size_t i : {ia, ib}) {
      if (i == (size_t)-1) continue;
      if (stepCount_ - bodies_[i].lastImpactStep < gapSteps &&
          bodies_[i].lastImpactStep != 0)
        continue;
      fresh = true;
    }
    if (!fresh) continue;
    for (size_t i : {ia, ib})
      if (i != (size_t)-1) bodies_[i].lastImpactStep = stepCount_;

    // WHAT WAS STRUCK decides the sound (sound_schema.js: "debris striking
    // THIS material"), so probe the grid on both sides of the contact plane
    // before falling back to the bodies. Two probes, because the contact point
    // sits exactly on the surface and which side of the cell boundary it lands
    // on is a rounding accident.
    uint32_t mat = worldMatAt(ci.posVoxel + ci.normal * 0.6f);
    if (mat == 0) mat = worldMatAt(ci.posVoxel - ci.normal * 0.6f);
    if (mat == 0) mat = worldMatAt(ci.posVoxel);
    // Body-vs-body, or terrain we could not read: the OTHER body's material,
    // else our own. A limb hitting a boulder should sound like the boulder.
    if (mat == 0 && ib != (size_t)-1 && ia != (size_t)-1)
      mat = bodies_[ib].domMat;
    if (mat == 0) mat = bodies_[ia != (size_t)-1 ? ia : ib].domMat;
    if (mat == 0) continue;

    const float k = std::clamp((ci.speedVoxPerSec - minVox) / (fullVox - minVox),
                               0.0f, 1.0f);
    cand.push_back(ImpactEvent{ci.posVoxel, mat, k});
  }
  if (cand.empty()) return;

  // Loudest first, then take the cap. partial_sort rather than sort: the tail
  // is discarded, so ordering it is work nobody reads.
  const size_t keep = std::min(cand.size(), kMaxImpactsPerStep);
  std::partial_sort(cand.begin(), cand.begin() + keep, cand.end(),
                    [](const ImpactEvent& a, const ImpactEvent& b) {
                      return a.energy > b.energy;
                    });
  impacts_.insert(impacts_.end(), cand.begin(), cand.begin() + keep);
}

void DebrisSystem::PostStep() {
  stepCount_++;
  // The gate the contact listener applies, refreshed from tuning every step so
  // an F5 reload takes effect. Set here (game thread, between Updates) rather
  // than inside Step: the Jolt job threads must never read CurrentTuning().
  phys_->SetContactReportSpeed(CurrentTuning().audio.impactMinSpeed /
                               kVoxelMeters);
  CollectImpacts();
  IVec3 wo = world_->WindowOrigin();
  Vec3 wlo{(float)(wo.x * (int)kChunk), (float)(wo.y * (int)kChunk),
           (float)(wo.z * (int)kChunk)};
  for (size_t i = 0; i < bodies_.size();) {
    Body& b = bodies_[i];
    // BEFORE the read-back, because b.xf is still where the body was when the
    // step began and that is the segment this has to test (UntunnelBody).
    //
    // A FOLLOWER IS NOT UNTUNNELLED AND IS NOT READ BACK. It has no trajectory
    // of its own to clamp — it went wherever DriveStraps put it — and reading
    // Jolt here would hand it back the pose it is about to be re-derived from,
    // one tick stale. Its host is clamped and read like anything else, which is
    // the same rule Mob's limp-rig path applies to a worn shell.
    if (!b.Follower()) {
      UntunnelBody(b.handle, b.xf.pos);
      phys_->GetTransform(b.handle, b.xf);
    }
    // bodies that leave the residency window despawn: there is no terrain to
    // collide with out there (Noita despawns offscreen bodies the same way)
    const float kPad = 32.0f;
    bool dead = b.xf.pos.x < wlo.x - kPad || b.xf.pos.y < wlo.y - kPad ||
                b.xf.pos.z < wlo.z - kPad ||
                b.xf.pos.x > wlo.x + (float)kWorldN + kPad ||
                b.xf.pos.y > wlo.y + (float)kWorldN + kPad ||
                b.xf.pos.z > wlo.z + (float)kWorldN + kPad;
    if (dead) {
      ReleaseBody(b);
      bodies_[i] = std::move(bodies_.back());
      bodies_.pop_back();
      instancesDirty_ = true;
    } else {
      i++;
    }
  }
  // body budget: oldest bodies despawn first (they are usually settled rubble)
  while (bodies_.size() > kMaxBodies) {
    ReleaseBody(bodies_.front());
    bodies_.erase(bodies_.begin());
    instancesDirty_ = true;
  }
  // LAST: the cull above is what decides whether a host still exists, and the
  // read-back above is what makes its transform final for this tick.
  DriveStraps();
}

// ---- THE EXPOSED-VOXEL CULL (BuildInstances) --------------------------------
// A body voxel with all six neighbours occupied by the SAME body can never
// produce a visible fragment: every one of its faces is coincident with a
// neighbour's face pointing the other way, and both are opaque. Emitting it as
// an instance still costs 36 vertex-shader invocations (two buffer reads, four
// quaternion rotations, a palette lookup, a burn tint and a six-read openness
// probe each) and 12 triangles the rasteriser has to set up and depth-reject.
// The felled oak (--fell-tree) is 28.4k voxels of which a solid trunk and a
// 35-92% dense crown make most interior — see the numbers on the commit.
//
// Occupancy is a padded byte lattice over the body's local AABB, rebuilt per
// body per call: BuildInstances runs only when a body's geometry changed
// (instancesDirty_), so this is O(voxels) per geometry edit, not per frame.
// The one-cell pad means a voxel on the AABB hull is always exposed without a
// bounds test in the neighbour probe.
//
// What it must NOT do: cull by anything the GPU changes later. Burn tints are
// payload rewrites and go through the instance list unchanged (a fully
// enclosed ember is invisible anyway); carving removes voxels and marks the
// list dirty, which rebuilds the cull. Another body's voxels never count as
// cover — two bodies are never in the same lattice.
//
// SANDVOX_BODY_DRAW_ALL=1 emits every voxel (the pre-2026-09-12 list): the
// one-binary A/B arm for the drawBodies span, kept so the cull's share of that
// span can be re-measured without a build.
namespace {
bool BodyDrawAllVoxels() {
  static const bool all = [] {
    const char* e = std::getenv("SANDVOX_BODY_DRAW_ALL");
    return e && e[0] != '0';
  }();
  return all;
}

// Emits the instances for one body into `out`, culling enclosed voxels.
// `scratch` is the occupancy lattice, reused across bodies and calls.
void EmitExposedBodyVoxels(const std::vector<DebrisVoxel>& voxels,
                           int8_t lmin[3], int8_t lmax[3], uint32_t slot,
                           std::vector<uint8_t>& scratch,
                           std::vector<BodyVoxInst>& out,
                           uint32_t& exposed) {
  const int dx = (int)lmax[0] - (int)lmin[0] + 3;  // +1 pad each side
  const int dy = (int)lmax[1] - (int)lmin[1] + 3;
  const int dz = (int)lmax[2] - (int)lmin[2] + 3;
  const size_t vol = (size_t)dx * (size_t)dy * (size_t)dz;
  scratch.assign(vol, 0);
  auto idx = [&](const DebrisVoxel& v) -> size_t {
    return (size_t)(v.x - lmin[0] + 1) +
           (size_t)dx * ((size_t)(v.y - lmin[1] + 1) +
                         (size_t)dy * (size_t)(v.z - lmin[2] + 1));
  };
  for (const DebrisVoxel& v : voxels) scratch[idx(v)] = 1;
  const size_t sx = 1, sy = (size_t)dx, sz = (size_t)dx * (size_t)dy;
  for (const DebrisVoxel& v : voxels) {
    if (out.size() >= kMaxBodyVoxInstances) break;
    const size_t i = idx(v);
    const bool enclosed = scratch[i - sx] && scratch[i + sx] &&
                          scratch[i - sy] && scratch[i + sy] &&
                          scratch[i - sz] && scratch[i + sz];
    if (enclosed) continue;
    exposed++;
    out.push_back({(float)v.x, (float)v.y, (float)v.z,
                   (uint32_t)v.payload | (slot << 16)});
  }
}
}  // namespace

void DebrisSystem::BuildInstances(std::vector<BodyVoxInst>& out) {
  // Charged straight into totalUs rather than through curUs: this runs on the
  // RENDER side, after PreTick has already rolled its tick up, so it belongs
  // to no tick's breakdown. It is here because "28k instanced boxes rebuilt
  // every tick while the body moves" was one of the five hypotheses and the
  // only way to retire it is a number.
  const auto t0 = prof_.on ? std::chrono::steady_clock::now()
                           : std::chrono::steady_clock::time_point{};
  out.clear();
  static std::vector<uint8_t> occScratch;
  uint32_t sourceVoxels = 0, exposed = 0;
  for (size_t bi = 0; bi < bodies_.size() && bi < kMaxBodies; bi++) {
    // Micro bodies draw through the OBB/brick-march pass instead. They still
    // OWN their slot (the two passes share bodyXforms), they just contribute
    // no cube instances — emitting both would double-draw at the wrong size.
    if (bodies_[bi].micro.Valid()) continue;
    Body& b = bodies_[bi];
    sourceVoxels += (uint32_t)b.voxels.size();
    if (BodyDrawAllVoxels()) {
      for (const DebrisVoxel& v : b.voxels) {
        if (out.size() >= kMaxBodyVoxInstances) break;
        out.push_back({(float)v.x, (float)v.y, (float)v.z,
                       (uint32_t)v.payload | ((uint32_t)bi << 16)});
      }
      continue;
    }
    RefreshLocalBounds(b);
    EmitExposedBodyVoxels(b.voxels, b.lmin, b.lmax, (uint32_t)bi, occScratch,
                          out, exposed);
  }
  // One line per DISTINCT population, so the --frames harness can quote the
  // cull's ratio: burning rewrites payloads and leaves both counts alone, so a
  // burning body does not print every tick.
  static uint32_t lastPrintedSrc = 0xFFFFFFFFu, lastPrintedOut = 0xFFFFFFFFu;
  if (sourceVoxels >= 1024 &&
      (sourceVoxels != lastPrintedSrc || (uint32_t)out.size() != lastPrintedOut)) {
    lastPrintedSrc = sourceVoxels;
    lastPrintedOut = (uint32_t)out.size();
    std::printf("debris: BuildInstances %u cube instances from %u voxels "
                "(%u exposed, %s)\n",
                (uint32_t)out.size(), sourceVoxels, exposed,
                BodyDrawAllVoxels() ? "SANDVOX_BODY_DRAW_ALL" : "enclosed culled");
  }
  instanceCount_ = (uint32_t)out.size();
  instancesDirty_ = false;
  if (prof_.on) {
    prof_.totalUs[(int)Phase::Instances] +=
        std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - t0).count();
    prof_.calls[(int)Phase::Instances]++;
  }
}

void DebrisSystem::BuildXforms(std::vector<BodyXformGpu>& out) const {
  out.clear();
  for (size_t i = 0; i < bodies_.size() && i < kMaxBodies; i++) {
    const Body& b = bodies_[i];
    BodyXformGpu x{};
    x.pos[0] = b.xf.pos.x;
    x.pos[1] = b.xf.pos.y;
    x.pos[2] = b.xf.pos.z;
    std::memcpy(x.quat, b.xf.quat, sizeof(x.quat));
    out.push_back(x);
  }
}

void DebrisSystem::AppendMicroInsts(std::vector<MicroBodyInstGpu>& out) const {
  for (size_t i = 0; i < bodies_.size() && i < kMaxBodies; i++)
    if (bodies_[i].micro.Valid())
      // No hit flash on debris (it is not a limb anybody can strike into a
      // flash), but the DYE travels: a sleeve cut off a red shirt is still red
      // on the ground. game/dye.h, sim/microbody.h MicroBodyRef::dye.
      out.push_back({(uint32_t)i, bodies_[i].micro.model, 0,
                     bodies_[i].micro.dye});
}

void DebrisSystem::AppendMicroHolders(std::vector<MicroHolder>& out) const {
  // EVERY body, not just the ones the walks above draw. A body past kMaxBodies
  // is still a holder — it owns its record and its ReleaseBody will still free
  // it — and it is exactly the one an overflowing cull is about to retire.
  for (size_t i = 0; i < bodies_.size(); i++) {
    const Body& b = bodies_[i];
    if (!b.micro.Valid()) continue;
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "debris body %zu/%zu (handle %llu, serial %u%s)", i,
                  bodies_.size(), (unsigned long long)b.handle, b.serial,
                  i >= kMaxBodies ? ", PAST kMaxBodies" : "");
    out.push_back({b.micro.model, buf});
  }
}

uint32_t DebrisSystem::ActiveBodyCount() const {
  uint32_t n = 0;
  for (const Body& b : bodies_)
    if (phys_->IsActive(b.handle)) n++;
  return n;
}

bool DebrisSystem::BodyActive(uint32_t i) const {
  return i < bodies_.size() && phys_->IsActive(bodies_[i].handle);
}

// ---- persistence (entities.sve section 'DBRS') ------------------------------

void DebrisSystem::SaveState(std::vector<uint8_t>& out) const {
  ByteWriter w{out};
  w.U32((uint32_t)bodies_.size());
  for (const Body& b : bodies_) {
    w.Pod(b.xf);
    w.U32(b.physScale);
    w.U32(b.micro.skinScale);
    // Only "did it render as micro" travels; the model index is meaningless
    // across sessions (the pool is rebuilt), so load re-packs a brick from the
    // lattice below.
    w.U32(b.micro.Valid() ? 1u : 0u);
    // The dye (v4). Not derivable from the lattice — see kSaveVersion's note.
    w.U32(b.micro.dye);
    w.U32(b.bleedMat);
    // THE STRAP, BY INDEX INTO THIS LIST. Jolt handles do not survive a
    // session, and load recreates the bodies in exactly this order, so the
    // index is the only thing that can name a host across the boundary.
    // 0xFFFFFFFF = not a follower. Without this an armoured corpse reloaded as
    // a pile of free bodies sharing the same space, which is the OTHER way to
    // build the motor StrapBody exists to avoid.
    uint32_t hostIdx = 0xFFFFFFFFu;
    if (b.Follower())
      for (size_t j = 0; j < bodies_.size(); j++)
        if (bodies_[j].handle == b.wornHost) { hostIdx = (uint32_t)j; break; }
    w.U32(hostIdx);
    w.Pod(b.wornRelPos);
    for (float q : b.wornRelQuat) w.F32(q);
    w.PodVec(b.voxels);
    w.PodVec(b.skinVoxels);
  }
}

bool DebrisSystem::LoadState(const uint8_t* data, size_t len, uint32_t version) {
  if (version != kSaveVersion) {
    std::fprintf(stderr, "debris: unknown DBRS section version %u\n", version);
    return false;
  }
  ByteReader r{data, len};
  uint32_t count = 0;
  r.U32(count);
  // The straps are tied in a SECOND PASS: a follower may be written before its
  // host, and a host that was skipped (empty lattice, Jolt refusal, the body
  // ceiling) has no handle to strap to. `handleOfSaved` is the map from the
  // saved index the section names to the handle that index actually became,
  // 0 for one that did not survive the load.
  std::vector<uint64_t> handleOfSaved(count, 0);
  struct PendingStrap {
    uint32_t self = 0, host = 0;
    Vec3 relPos{};
    float relQuat[4] = {0, 0, 0, 1};
  };
  std::vector<PendingStrap> straps;
  for (uint32_t i = 0; i < count && r.ok; i++) {
    BodyTransform xf{};
    uint32_t physScale = 1, skinScale = 1, hadMicro = 0, bleedMat = 0, dye = 0;
    uint32_t hostIdx = 0xFFFFFFFFu;
    Vec3 relPos{};
    float relQuat[4] = {0, 0, 0, 1};
    std::vector<DebrisVoxel> voxels;
    std::vector<PrefabVoxel> skinVoxels;
    r.Pod(xf);
    r.U32(physScale);
    r.U32(skinScale);
    r.U32(hadMicro);
    r.U32(dye);
    r.U32(bleedMat);
    r.U32(hostIdx);
    r.Pod(relPos);
    for (float& q : relQuat) r.F32(q);
    r.PodVec(voxels);
    r.PodVec(skinVoxels);
    if (!r.ok || voxels.empty()) continue;
    if (bodies_.size() >= kMaxBodies) break;  // same ceiling spawning obeys

    physScale = std::max(1u, physScale);
    const float pitch = 1.0f / (float)physScale;
    // A saved follower has to come back able to BE kinematic — the strap pass
    // below switches it, and Jolt refuses the switch on a body that was not
    // created with the allowance.
    uint64_t h = phys_->CreateDebrisBodyXf(
        voxels, xf, densityOf_, /*allowKinematic=*/hostIdx < count, pitch);
    if (h == 0) {
      std::fprintf(stderr, "debris: Jolt refused a loaded body (skipped)\n");
      continue;
    }

    // Re-pack the micro brick from the authoritative lattice — the same
    // skin-first rule ReskinMicro applies — and mark it OWNED so ReleaseBody
    // returns its words to the pool like any copy-on-write clone. Variant bits
    // are stripped: the brick stores plain 8-bit material ids.
    MicroBodyRef micro{};
    if (hadMicro && microSet_) {
      std::vector<PrefabVoxel> mv;
      const bool fine = skinScale > physScale && !skinVoxels.empty();
      const std::vector<PrefabVoxel>* src = fine ? &skinVoxels : nullptr;
      if (src) {
        mv.reserve(src->size());
        for (const PrefabVoxel& v : *src)
          mv.push_back({v.x, v.y, v.z, (uint16_t)(v.material & 0xFFu), 0, v.stain});
      } else {
        mv.reserve(voxels.size());
        for (const DebrisVoxel& v : voxels)
          mv.push_back({(int16_t)v.x, (int16_t)v.y, (int16_t)v.z,
                        (uint16_t)(v.payload & 0xFFu), 0, v.stain});
      }
      IVec3 mx{0, 0, 0};
      for (const PrefabVoxel& v : mv) {
        mx.x = std::max<int>(mx.x, v.x);
        mx.y = std::max<int>(mx.y, v.y);
        mx.z = std::max<int>(mx.z, v.z);
      }
      std::string plog;
      int mi = MicroBodyPack(*microSet_, mv, {mx.x + 1, mx.y + 1, mx.z + 1},
                             skinScale, "load", plog);
      if (mi >= 0) {
        microSet_->owned[mi] = 1;  // freeable: this body is the sole holder
        micro = MicroBodyRef{(uint32_t)mi, skinScale, dye};
      } else if (!plog.empty()) {
        std::fprintf(stderr, "%s", plog.c_str());
      }
    }

    AdoptBody(h, std::move(voxels), xf, micro, physScale, std::move(skinVoxels),
              bleedMat);
    handleOfSaved[i] = h;
    if (hostIdx < count) {
      PendingStrap ps;
      ps.self = i;
      ps.host = hostIdx;
      ps.relPos = relPos;
      for (int q = 0; q < 4; q++) ps.relQuat[q] = relQuat[q];
      straps.push_back(ps);
    }
    // Reload ASLEEP with zero velocity (worldio.h's rigidbody rule): a settled
    // pile reloads settled, and rule 2's sleep invariant holds from tick one.
    phys_->DeactivateBody(h);
  }
  // ---- second pass: tie the straps ----------------------------------------
  // The SAVED offset is restored, not re-derived from the loaded poses, for
  // the same reason StrapBody captures the live one: the pair's offset is a
  // fact about where they were, and a garment whose host was skipped simply
  // stays loose debris.
  for (const PendingStrap& ps : straps) {
    const uint64_t sh = handleOfSaved[ps.self], ho = handleOfSaved[ps.host];
    if (!sh || !ho) continue;
    if (!StrapBody(sh, ho)) continue;   // takes the kinematic switch with it
    for (Body& b : bodies_)
      if (b.handle == sh) {
        b.wornRelPos = ps.relPos;
        for (int q = 0; q < 4; q++) b.wornRelQuat[q] = ps.relQuat[q];
        break;
      }
  }
  return r.ok;
}
