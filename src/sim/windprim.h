#pragma once
#include <cstdint>
#include <vector>

#include "math3d.h"
#include "sim/world.h"

// windprim.h — WIND PRIMITIVES: the player-facing half of the wind system.
//
// docs/RESEARCH_wind.md §4.3 is the plan of record; DESIGN.md §12 states the
// invariants. Phase 1 gave the world an ambient field — weather, a mood,
// scenery. This is the part that makes wind a TOOL: a fan blows, a spell gusts,
// a tornado lifts, and each of those is a small parametric object rather than a
// stored field or a special case in a kernel.
//
// THE ONE-PARAGRAPH VERSION. A primitive is ~48 bytes of parameters — position,
// axis, strength, radius, reach, lifetime. It is evaluated ANALYTICALLY at any
// sample point, exactly the way a point light is: there is no lattice, no
// resolution to choose, and nothing stored per voxel or per chunk. The bounded
// list rides the per-tick input stream inside TickParams (and RenderParams, so
// the grass leans in a fan's blast), which is why adding one costs no new
// binding, no new barrier and no new dispatch. With ZERO primitives every
// consumer takes an exact-identity early-out and the world is bit-identical to
// one where this file does not exist.
//
// WHY IT IS NOT A LIST OF LIVE OBJECTS ON THE GPU. Movement is analytic in
// time: a travelling gust's position is `origin + vel * (tick - spawnTick)`,
// not an accumulator. So the GPU never needs to mutate a primitive, and the
// CPU resolves the whole list once per tick — thirty-two evaluations instead
// of the millions a per-sample re-derivation would cost. The resolved form is
// what ships; the authored form (below) is what the op stream carries.
//
// WHY IT IS AN OP AND NOT A SIDE CHANNEL (rule 3, and invariant 5). Player and
// world wind exists ONLY as a spawn op. Spells emit through SpellEmission,
// placed fans register on place and deregister on break, and both land here.
// The consequence is the usual one: saves, replays and a future network stream
// inherit wind for free, and there is no second way to make the world blow.
//
// THE FOOTPRINT WAKE — the reason phase 2 blocks phase 4's entrainment.
// The ambient field must NEVER wake a chunk (invariant 3): an exposed dune
// under a steady breeze would re-mark its own chunks forever, which is rule 2
// with the sign flipped. A primitive is different — it is bounded, it is
// player-caused, and it can therefore declare its footprint. Each tick it
// lives, it dirty-marks the chunks it covers THROUGH THE MUTATION PATH, which
// is what makes those chunks CPU-known: they enter `opTargets`, they are
// materialized with their 26-ring before the command buffer is built, and they
// are charged against a per-tick budget before anything moves.
//
// That is not merely tidy. The page table's materialization set is TIGHTENED
// against a lagging snapshot, and the tightening is sound only because settled
// matter writes nothing (PLAN_page_table.md §3.2, RESEARCH_wind.md §10).
// Entrainment is the first rule in the engine that makes resting voxels move,
// and switching it on globally lost 62 voxels to page faults in a 160-tick
// run. Inside a declared footprint it cannot: the chunk was declared before the
// dispatch that writes it. So entrainment is enabled BY A PRIMITIVE, in the
// region that primitive woke, and nowhere else.

// ---- ceilings -------------------------------------------------------------
//
// kWindPrimCap / kWindPrimWords / kWindWakeCap / kWindPrimMaxExtent /
// kWindPrimMaxSpeed / kWindPrimForever and the kWindPrim{Cone,Burst,Vortex} +
// kWindPrim{Air,Water,Entrain} encodings live in sim/world.h next to
// TickParams, because they are the GPU LAYOUT and world.h is where the layout
// that a shader must agree with is stated. Only the CPU-side ones are here.

// Largest footprint, in CHUNKS, a primitive may declare and still be allowed to
// carry the entrainment licence. 512 = an 8x8x8 chunk box = 128 cells a side.
// This bounds the WAKE SCAN as well as the wake itself: without it a 512-cell
// vortex would walk 274,625 chunk slots per tick just to discover that most of
// them are sky. A primitive over the limit still blows — it simply cannot pick
// settled matter up (WindPrimSystem::Spawn).
constexpr uint64_t kWindWakeMaxChunks = 512;

// ---- the authored primitive (what an op carries) ---------------------------
//
// Integers, in the engine's existing conventions: world cells for position,
// Q16.16 for the unit axis and for speeds. Floats appear at the AUTHORING
// boundary only (WindPrimAim below turns an aim vector and an m/s number into
// these), exactly as the spell VM keeps its projectile state fixed-point and
// converts at the rendering edge. The reason is the same: this is destined for
// a replay log and a lockstep network stream, and retrofitting fixed point
// after the op format ships is a rewrite.
struct WindPrim {
  int32_t x = 0, y = 0, z = 0;        // origin AT SPAWN, world cells
  int32_t dirX = 0, dirY = 0, dirZ = 65536;  // unit axis, Q16.16
  int32_t velX = 0, velY = 0, velZ = 0;      // origin drift, Q16.16 cells/tick
  int32_t strengthQ = 0;              // core speed, Q16.16 world cells/s
  int32_t radius = 8;                 // world cells
  int32_t reach = 32;                 // world cells (axial; unused by Burst)
  int32_t swirlQ = 0;                 // tangential share, Q16.16 (Vortex)
  int32_t riseQ = 0;                  // axial share, Q16.16 (Vortex)
  uint32_t kind = kWindPrimCone;
  uint32_t flags = kWindPrimAir;
  uint32_t spawnTick = 0;
  uint32_t ttl = 90;                  // ticks, or kWindPrimForever
  // Opaque owner handle, so a fan object can find and retire its own primitive
  // without this file knowing what a fan is (the spell VM's casterId
  // convention). 0 = unowned.
  uint64_t ownerId = 0;
};

// The GPU form: 12 i32 words = 3 std140 rows. Must match windPrimLoad in
// assets/shaders/common.wgsl, which is the only reader.
//
// This is RESOLVED, not authored: position is where the primitive is THIS
// tick, and strength already carries the lifetime envelope. Everything the
// shader would otherwise have to re-derive per sample was done once here.
struct WindPrimGpu {
  int32_t w[kWindPrimWords] = {};
};

// ---- authoring helpers -----------------------------------------------------

// Turn a float aim direction and a speed in METRES PER SECOND into the integer
// fields a primitive carries. The ONE place floats become a primitive — the
// same seam WindWeather has, and it is bounded by the same argument (a handful
// of scalars per op, quantised; see the determinism note in sim/wind.h).
//
// A zero-length `dir` yields +Y, because a fan pointing nowhere is an authoring
// mistake and a NaN axis would spread through every sample in its radius.
void WindPrimAim(WindPrim& p, Vec3 dir, float speedMs);

// Inclusive world-cell AABB the primitive can influence at `tick`. Tighter than
// pos +- max(radius, reach) for a cone, which matters: the box is what the wake
// budget is spent on, and a 32-cell fan whose box is a 512-cell cube would eat
// the whole budget on empty sky.
void WindPrimBounds(const WindPrim& p, uint32_t tick, IVec3& lo, IVec3& hi);

// ---- the system ------------------------------------------------------------
//
// Owns the live list. Deterministic by construction: the contents are a pure
// function of (the ops that arrived, the tick), the order is insertion order,
// and expiry is a tick comparison. Nothing here integrates, so a replay that
// re-issues the same ops gets the same list on the same tick.
class WindPrimSystem {
 public:
  // Emit a primitive. Returns false — and changes nothing — if the list is
  // full: budgets are charged BEFORE emission, never after (CLAUDE.md), so a
  // caller learns its gust did not happen instead of silently displacing
  // someone else's fan.
  bool Spawn(const WindPrim& p);

  // Retire everything a given owner spawned (a fan object broken, a caster
  // dying). Cheap and exact — the alternative, letting an orphan blow forever,
  // is a rule-2 leak that no amount of TTL tuning finds.
  void RetireOwner(uint64_t ownerId);

  void Clear();

  // Advance to `tick`: drop the expired, resolve the survivors to their GPU
  // form, recompute the union AABB. Call once per tick BEFORE the tick's
  // TickParams is filled and before the page table's BeginTick/AddOp* pass, so
  // the footprints this tick's wake declares are the footprints it ships.
  void Tick(uint32_t tick);

  // The resolved list for the tick most recently passed to Tick().
  const std::vector<WindPrimGpu>& Resolved() const { return resolved_; }
  uint32_t Count() const { return (uint32_t)resolved_.size(); }
  // Union of every live primitive's AABB, world cells. The shader's whole-loop
  // early-out: a sample outside this box skips all 32 tests. `lo > hi` on any
  // axis is the EMPTY convention (no primitives), matching the fluid render
  // box, and is what makes a world with no wind objects cost exactly nothing.
  IVec3 BoundsLo() const { return lo_; }
  IVec3 BoundsHi() const { return hi_; }

  // Live primitives, in authored form — for the debug overlay and for a fan
  // object checking whether its own primitive still exists.
  const std::vector<WindPrim>& Live() const { return live_; }

  // Chunk SLOT indices this tick's primitives want awake, already filtered,
  // budgeted and deduplicated. Empty unless some primitive carries
  // kWindPrimEntrain: a gust that cannot move settled matter has no reason to
  // wake anything, and saying so here is what keeps a decorative spell free.
  //
  // `occupancy` is the one-tick-latent snapshot's per-slot non-air count, used
  // as a filter: a chunk of pure air has nothing to entrain, and skipping it
  // is what turns a fan's footprint from "a cube of sky" into "the surface it
  // is pointed at". Pass an empty span before the first snapshot arrives — the
  // filter then admits everything, which is the conservative direction.
  void BuildWake(const World& world, const std::vector<uint32_t>& occupancy,
                 uint32_t budget, std::vector<uint32_t>& out) const;

  // Wake slots refused for want of budget on the last BuildWake. Reported, not
  // hidden: a silently trimmed footprint reads as "entrainment is flaky".
  uint32_t WakeRefused() const { return wakeRefused_; }
  // Chunks the last BuildWake actually asked for — the rule-2 number the dev
  // panel shows, so "a fan is on" and "this is what it costs" are one line.
  uint32_t LastWakeCount() const { return wakeLast_; }

 private:
  std::vector<WindPrim> live_;
  std::vector<WindPrimGpu> resolved_;
  IVec3 lo_{1, 1, 1}, hi_{0, 0, 0};
  uint32_t tick_ = 0;
  mutable uint32_t wakeRefused_ = 0;
  mutable uint32_t wakeLast_ = 0;
};

// THE live primitive list.
//
// A global for exactly the reason `Celestial()` (world.h) is one: it has to
// reach the frame loop, the shot paths, every selftest gate and both smoke
// harnesses, and any one of those that DIDN'T get it passed would silently
// simulate a world with no fans in it while the renderer drew them — a
// divergence between what you see and what the world does, which is the
// failure this system exists to prevent. SubmitTick advances it (exactly once
// per sim tick, from the one place both the game and the harnesses go through)
// and WriteRenderParams reads it, so the sim and the render copy cannot
// describe different instants.
//
// Every headless path leaves it EMPTY unless a gate puts something in it, and
// an empty list is an exact identity all the way down — so the pinned world
// hash is untouched by the existence of this file.
WindPrimSystem& WindPrims();

// Resolve one primitive to its GPU form at `tick`. Exposed for the selftest,
// which builds a primitive and asserts the shader agrees with the CPU about
// where its footprint is.
WindPrimGpu WindPrimResolve(const WindPrim& p, uint32_t tick);

// The lifetime envelope, Q10 (1024 = full strength). A primitive fades in over
// kWindPrimAttack ticks and out over kWindPrimRelease, both clamped to a
// quarter of its TTL so a short gust still has a shape. Without this a spell
// gust switches a 40 m/s field on between two ticks, and the CA's drift bias —
// which is a probability, not a force — makes that read as smoke teleporting.
constexpr uint32_t kWindPrimAttack = 4;
constexpr uint32_t kWindPrimRelease = 12;
int32_t WindPrimEnvelope(const WindPrim& p, uint32_t tick);
