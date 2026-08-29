#pragma once
#include <cstdint>
#include <vector>

#include "math3d.h"
#include "sim/world.h"

// currentprim.h — THE CURRENT FIELD (docs/PLAN_water_master.md component 8).
//
// DESIGN.md §9c states the invariants. This file is a deliberate clone of
// windprim.h, and the cloning is the design rather than an accident of
// authorship: water flow is the same KIND of object as wind — a bounded list of
// parametric shapes summed analytically at any sample point, like point lights
// — so it gets the same list, the same three-row GPU packing, the same union
// AABB, the same float/integer transcription pair in common.wgsl, and the same
// `ptr<uniform, T>` rule. Inventing a second shape for it would have meant a
// second set of overflow arguments to get wrong.
//
// WHAT IT IS FOR. A pond is a still blue disc until something in it moves.
// This is the pure function that says which way the water is going at a point,
// and it is what pushes the player, drags debris, drifts the surface waves
// (component 9) and draws the foam. It owns NO MASS: the ledger (component 3)
// is the only thing in this design that moves an eighth of water, and it never
// reads this field. That is why the accuracy envelope below is acceptable.
//
// SUPERPOSITION ONLY — no neighbour coupling, no stored field, no relaxation
// pass, nothing to wake. The owner's original framing was a field where nearby
// vectors influence each other so a river entering a pool dissipates outward;
// that behaviour is what a point SOURCE does for free under superposition
// (plan §0 correction 4), and implementing it as real vector diffusion means
// stored state, a solver, per-tick cost, determinism exposure and a system that
// does not sleep. Superposition of sources, sinks and vortices is a real
// solution of Laplace's equation: incompressible irrotational flow away from
// boundaries is approximately what pond water does.
//
// THE ACCURACY ENVELOPE, and why the errors are harmless. No no-flow boundary
// at terrain, no separation, no eddies behind obstacles, no turbulence. A wrong
// current pushes a leaf the wrong way; it cannot lose water. That is the payoff
// for keeping it a pure function, and it is why this tier can be tuned by eye.
//
// ---- THE AUTHORITY LINE, which is the one thing wind did not need ---------
//
// A wind primitive is authored by an op, so its parameters are trivially a pure
// function of the tick input stream. A water current wants to be seeded from
// things the CPU can only learn ASYNCHRONOUSLY — most obviously whether the GPU
// ledger's `WBS_EMIT` is non-zero this tick, which arrives (if at all) through a
// readback scheduled by fence retirement. That is exactly the hazard M1's §1.1
// correction 2 names: "seeded when the CPU got around to noticing" is a
// scheduling-dependent outcome, and it becomes a rule-1 violation the moment a
// kernel reads it.
//
// So every primitive carries kCurrentPrimSim, and it is set ONLY when the
// primitive's parameters are a pure function of (seed, window, tuning, tick):
//
//   * the STREAM arm  — a function of the landform, so yes.
//   * a DRAIN's sink and vortex — seeded from the MUTATION that cut the hole,
//     which rides the mutation queue and is therefore on the tick stream. Yes.
//   * anything read back from `waterBodyState` — NO, and there is deliberately
//     no code here that does it.
//
// currentAtQ (the sim evaluator) skips primitives without the licence;
// currentAt (the render evaluator) sums all of them. A render field cannot
// write a voxel, so the split costs nothing and is what lets the look ship
// while the sim arm stays off.

namespace sandvox {

class WaterBodySystem;

// Decay sentinel: a current runs until something removes it. Terrain-derived
// primitives (the stream arm) carry it; event-derived ones never do, because
// "the swirl decays when the flow stops" is the whole point of the field having
// a release at all.
constexpr uint32_t kCurrentPrimForever = 0xFFFFFFFFu;

// Ticks a primitive fades IN over. Short, because a drain's swirl genuinely
// spins up fast once the water starts moving; the fade exists so a seeded
// vortex does not switch a 5 m/s tangential field on between two frames.
constexpr uint32_t kCurrentPrimAttack = 6;

// ---- the stream arm's probe schedule --------------------------------------
// Voxels between lattice probes over the residency window, and also each stream
// primitive's own radius — so the probes tile the window without the primitives
// overlapping into a wall of flow. 64 voxels = 6.4 m.
constexpr int kStreamProbeStride = 64;
// Central-difference baseline for the bed direction, voxels. Wide ON PURPOSE:
// see the slope-trap note over SeedStreams. 32 voxels = four grain-octave cells.
constexpr int kStreamGradBaseline = 32;
// Stream primitives one window may carry. Half the cap, so a drain seeded in
// the same tick can always still find a slot — rule 2 charged before emission.
constexpr int kStreamMaxPrims = 16;
// Owner handles. The stream arm is one owner (it is retired and rebuilt whole
// when the window moves); each body's drain owns two consecutive handles.
constexpr uint64_t kStreamOwnerId = 0x57524541u;      // 'STREA'
constexpr uint64_t kDrainOwnerBase = 0x44524149u;     // 'DRAI'

// ---- the authored primitive -----------------------------------------------
//
// Integers in the engine's existing conventions: world cells for position,
// Q16.16 for the unit axis and for speeds. Floats appear at the AUTHORING
// boundary only (CurrentPrimAim below), exactly as WindPrim does, and for the
// same reason: this is destined for a replay log and retrofitting fixed point
// after the format ships is a rewrite.
struct CurrentPrim {
  int32_t x = 0, y = 0, z = 0;              // origin, world cells
  int32_t dirX = 0, dirY = 1 << 16, dirZ = 0;  // unit axis, Q16.16
  // CORE SPEED, Q16.16 world cells/s — the speed at the throat radius
  // (radius/8). Every kind means the same thing by it, which is what lets the
  // four shapes share one cap and one overflow argument: a profile can only
  // ever attenuate it.
  int32_t strengthQ = 0;
  int32_t radius = 32;                      // world cells
  int32_t reach = 32;                       // world cells (axial)
  int32_t swirlQ = 1 << 16;                 // tangential share, Q16.16 (vortex)
  int32_t riseQ = 0;                        // axial share, Q16.16 (vortex)
  uint32_t kind = kCurrentPrimStream;
  uint32_t flags = 0;                       // kCurrentPrimSim
  uint32_t spawnTick = 0;                   // for the attack ramp
  // LAST TICK THE SEEDER RE-ASSERTED THIS PRIMITIVE. The release ramp is
  // measured from here, not from spawnTick, which is what lets a seeder refresh
  // a live whirlpool every tick without restarting its attack.
  uint32_t seenTick = 0;
  // TICKS THE PRIMITIVE SURVIVES ITS LAST SIGHTING, fading linearly to zero
  // over that window, or kCurrentPrimForever. This is plan component 8's
  // "Gamma must decay when flow stops": without it a funnel stands open in
  // still water, which is instantly and obviously wrong.
  // sim.currentVortexDecay is what sets it for a drain.
  uint32_t decayTicks = 30;
  uint64_t ownerId = 0;                     // 0 = unowned
};

// The GPU form: 12 i32 words = 3 std140 rows. Must match currentPrimEvalF /
// currentPrimEvalQ in assets/shaders/common.wgsl, which are the only readers.
//
// RESOLVED, not authored: `strengthQ` here already carries the envelope, so the
// shader never has to know when a primitive started.
struct CurrentPrimGpu {
  int32_t w[kCurrentPrimWords] = {};
};

// Turn a float aim direction and a speed in METRES PER SECOND into the integer
// fields a primitive carries. The ONE place floats become a current primitive.
// A zero-length `dir` yields +Y (a vertical whirlpool axis, which is what a
// drain wants), because a NaN axis would spread through every sample in range.
void CurrentPrimAim(CurrentPrim& p, Vec3 dir, float speedMs);

// Circulation Gamma (m^2/s) -> the core speed this encoding wants, m/s.
// v_theta = Gamma / (2 pi r), and `strength` is defined at r = radius/8, so
// this is the one place the physical quantity becomes the packed one.
float CurrentGammaToCoreMs(float gammaM2s, int radiusCells);

// Inclusive world-cell AABB the primitive can influence. Tighter than
// pos +- max(radius, reach): a stream reaches along its axis and a sink does
// not, and the box is the shader's whole-loop reject.
void CurrentPrimBounds(const CurrentPrim& p, IVec3& lo, IVec3& hi);

// The envelope, Q10 (1024 = full strength): attack over kCurrentPrimAttack
// ticks, then a linear release over `decayTicks` measured from `seenTick`.
int32_t CurrentPrimEnvelope(const CurrentPrim& p, uint32_t tick);

// Resolve one primitive to its GPU form at `tick`. Exposed for the selftest.
CurrentPrimGpu CurrentPrimResolve(const CurrentPrim& p, uint32_t tick);

// ---- the system ------------------------------------------------------------
//
// Deterministic by construction: the contents are a pure function of (the seeds
// that arrived, the tick), the order is insertion order, and expiry is a tick
// comparison. Nothing here integrates.
class CurrentPrimSystem {
 public:
  // Add or REFRESH a primitive. A spawn carrying a non-zero ownerId that
  // matches a live primitive updates it in place and stamps `seenTick` — which
  // is what a per-tick seeder wants, and is what keeps a whirlpool from being
  // re-created (and re-attacked) thirty times a second.
  //
  // Returns false — and changes nothing — if the list is full: budgets are
  // charged BEFORE emission (rule 2), so a caller learns its current did not
  // happen instead of silently displacing someone else's.
  bool Spawn(const CurrentPrim& p);

  void RetireOwner(uint64_t ownerId);
  void Clear();

  // Advance to `tick`: drop the faded, resolve the survivors to their GPU form,
  // recompute the union AABB. Called once per sim tick from SubmitTick, BEFORE
  // TickParams is filled, for the reason WindPrimSystem::Tick is: a second call
  // site resolving at a different tick would ship the sim a current the
  // renderer draws somewhere else.
  void Tick(uint32_t tick);

  // ---- the seeders (split from evaluation, plan component 8) --------------
  //
  // "Build the evaluator first with the stream arm and a debug/authored
  // primitive list; add seeders as components land." Both of these are
  // idempotent per tick and both only ever assert primitives whose parameters
  // are a pure function of the tick input stream — see the authority note at
  // the top of this file.

  // THE STREAM ARM. Manning/Chezy: v proportional to sqrt(slope * depth), with
  // the direction taken from the BED gradient. Independent of every other
  // component in this plan — no descriptor needed, and it would work in a world
  // where components 1-7 did not exist.
  //
  // Rebuilt only when the residency window moves, because it is a pure function
  // of (seed, window, tuning) and re-probing it every tick would be ~1,600
  // terrain hashes a tick for an answer that cannot have changed.
  void SeedStreams(const World& world, uint32_t seed, uint32_t tick);

  // THE DRAIN ARM (components 6 + 8). While a governed body's drain window is
  // live, a SINK at the hole plus a VORTEX about the vertical through it. The
  // hole position comes from the MUTATION that opened it, which is on the tick
  // stream; Gamma and its chirality come from hash3 of that position, so not
  // every drain in the world spins the same way — and a real bathtub vortex IS
  // residual ambient circulation being concentrated, so an initial condition
  // drawn from the position is physically the right shape rather than a fudge.
  void SeedDrains(const WaterBodySystem& water, uint32_t seed, uint32_t tick);

  const std::vector<CurrentPrimGpu>& Resolved() const { return resolved_; }
  uint32_t Count() const { return (uint32_t)resolved_.size(); }
  IVec3 BoundsLo() const { return lo_; }
  IVec3 BoundsHi() const { return hi_; }
  const std::vector<CurrentPrim>& Live() const { return live_; }
  // Primitives refused for want of a slot since the last Clear. Reported, not
  // hidden: a silently dropped whirlpool reads as "the drain sometimes swirls".
  uint32_t Dropped() const { return dropped_; }

 private:
  std::vector<CurrentPrim> live_;
  std::vector<CurrentPrimGpu> resolved_;
  IVec3 lo_{1, 1, 1}, hi_{0, 0, 0};
  uint32_t tick_ = 0;
  uint32_t dropped_ = 0;
  // The window origin the stream arm was last probed for. A sentinel far
  // outside any legal origin so the first call always builds.
  IVec3 streamOrigin_{1 << 30, 1 << 30, 1 << 30};
  uint32_t streamSeed_ = 0;
};

// THE live list. A global for exactly the reason WindPrims() and WaterBodies()
// are: it has to reach the frame loop, the shot paths, every selftest gate and
// both smoke harnesses, and any one of those that did not get it passed would
// simulate a world with no currents in it while the renderer drew them.
// SubmitTick advances it; WriteRenderParams reads it.
//
// Every headless path leaves it EMPTY unless a gate puts something in it, and
// an empty list is an exact identity all the way down.
CurrentPrimSystem& CurrentPrims();

// ---- component 9: the impact-ripple ring -----------------------------------
//
// A ripple is the memory of an event, so unlike everything else in this file it
// needs state — and the plan is explicit about how much: a small BOUNDED ring
// of recent impacts, each drawn as an analytic expanding ring with amplitude
// decay, a pure function of (eventList, t). The upgrade path (a per-body 2D
// wave-equation texture) is stored state plus a solver. DO NOT START THERE.
//
// Render-only, like the waves it feeds: no sim kernel reads it, it is not
// hashed, not saved, and a full ring simply overwrites its oldest entry.
struct WaveImpact {
  float x = 0.0f, z = 0.0f;   // world voxels
  float t0 = 0.0f;            // the R.time the impact landed at
  float amp = 0.0f;           // metres of initial crest; <= 0 is a dead slot
};

class WaveImpactRing {
 public:
  // `amp` is in metres of initial crest. A splash from a body falling in is
  // centimetres; an explosion is tens of centimetres.
  void Add(float xVox, float zVox, float timeSec, float ampM);
  void Clear();
  const WaveImpact* Data() const { return ring_; }
  uint32_t Count() const { return count_; }

 private:
  WaveImpact ring_[kWaveImpactCap];
  uint32_t count_ = 0;   // how many slots have ever been filled, capped
  uint32_t next_ = 0;
};

WaveImpactRing& WaveImpacts();

// THE FIELD, ON THE CPU. World cells per second at a world-voxel position.
//
// THIS IS A THIRD TRANSCRIPTION AND IT IS NAMED AS ONE. common.wgsl carries the
// float evaluator (the renderer) and the integer evaluator (the sim), adjacent,
// with a standing obligation between them. This is a third copy of the same
// four profiles, and it exists because the player is CPU physics: the only
// alternatives were to push the player from a field the renderer draws
// differently, or not to push the player at all — and "the current does not
// move you" is the difference between a whirlpool and a painting of one.
//
// It is BOUNDED as a copy in the way that matters: it reads the RESOLVED rows
// (CurrentPrimGpu), so the envelope, the cap, the AABB and the packing are the
// shared code and only the four profile formulae are transcribed. If you change
// a profile in common.wgsl, change it in BOTH evaluators there and in this one.
//
// The player is not part of the world hash, so this is float and needs no
// integer twin.
Vec3 CurrentAtCpu(Vec3 posVox);

// ---- the arrow overlay's lattice -------------------------------------------
// THIS FORMULA IS MIRRORED in debug_current.wgsl's vertex shader, which derives
// its lattice coordinate from the instance index the same way — the CPU decides
// how many instances to draw, the shader decides where each one goes. Exactly
// the arrangement WindDebugArrowsPerAxis has, including the cap and the reason
// for it, and disagreement is graceful in both directions.
uint32_t CurrentDebugArrowsPerAxis();
// Instance count for the overlay draw. Cubic in reach/spacing, hence the cap:
// this is the number that decides whether the overlay is free.
uint32_t CurrentDebugArrowCount();

}  // namespace sandvox
