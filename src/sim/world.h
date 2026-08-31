#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "gpu/rhi.h"

#include "math3d.h"
#include "sim/rng.h"   // rng::Hash3 — the JITTER palette-variant formula

// World constants. These are the SINGLE source of truth: the matching WGSL
// consts are generated from them by ShaderConstantPrelude() (gpu/resources.cpp)
// and prepended ahead of common.wgsl, so shaders cannot drift from C++.
// 512^3 residency window = 51.2 m per edge at 10 cm voxels (doubled from 256
// on 2026-08-19: the simulated world extends +-25.6 m around the player). The
// voxel buffer is kVoxelCount u32 = 512 MiB — exactly the storage limit
// context.cpp requests; growing this again means raising those limits AND
// accepting 8x that memory. Must stay a power of two (all window addressing
// is bitmasks).
constexpr uint32_t kWorldN = 512;

// Physical edge length of one voxel. The engine runs entirely in voxel units;
// this is the single meters<->voxels conversion, and every physical constant
// (player size, speeds, gravity, fog/media densities) derives from it. Change
// it here and here only — shaders pick it up automatically.
// Note: at the same kWorldN, smaller voxels shrink the world's physical size.
constexpr float kVoxelMeters = 0.10f;

// Voxels per metre — the integer reciprocal of the line above, DERIVED and
// never restated. worldgen.wgsl carried a hand-written `VOX_PER_M = 16` while
// the world ran at 10, which silently made every tree 1.6x the metre size its
// own size table documents; nothing could catch that, because the two numbers
// had no mechanical relationship. Emitted into WGSL as VOXELS_PER_M by
// ShaderConstantPrelude().
//
// THIS IS THE KNOB FOR A VOXEL-SIZE EXPERIMENT. Change kVoxelMeters, rebuild,
// and every length in worldgen follows: the tuning rows are authored at
// worldgen.refVoxelsPerMetre and LoadTuning rescales them, and the shader's own
// hardcoded lengths scale off VOXELS_PER_M / TUNE_REF_VOXELS_PER_METRE. What
// does NOT follow, and is the real cost of halving: kWorldN is a fixed voxel
// count, so the sim-live band halves in metres unless you double it (8x
// memory), and surface area — which is what the page pool is charged for —
// goes as the square.
constexpr int kVoxelsPerMetre = (int)(1.0f / kVoxelMeters + 0.5f);
constexpr uint32_t kChunk = 16;
constexpr uint32_t kNChunk = kWorldN / kChunk;          // 32
constexpr uint32_t kNumChunks = kNChunk * kNChunk * kNChunk;  // 32768
constexpr uint32_t kChunkVol = kChunk * kChunk * kChunk;      // 4096
constexpr uint64_t kVoxelCount = (uint64_t)kWorldN * kWorldN * kWorldN;

// ---- the fluid lab's flat-slab worldgen mode (docs/PLAN_fluid_overhaul.md §4)
// The `--lab` / `--fluid-bench` test world: solid stone for y <= kLabSlabY,
// air above, no biomes/trees/caves/ponds/POIs/flora. It is a MODE TAP through
// the normal worldgen path (TickParams.labMode guards genColumn in
// worldgen.wgsl), not a fork of genChunk. This constant is the ground height
// World::TerrainHeight returns in lab mode and the LAB_SLAB_Y the shader
// prelude emits — one value, two consumers, or the player falls through the
// slab they can see. Never set on any selftest/smoke path: the pinned world
// hash 7cfa2420 is a labMode=0 fact.
constexpr int32_t kLabSlabY = 127;

// Material IDs (fixed by materials.json order — append there, never reorder).
constexpr uint32_t kMatAir = 0, kMatStone = 1, kMatWood = 2, kMatSand = 3,
                   kMatGravel = 4, kMatWater = 5, kMatOil = 6, kMatSmoke = 7,
                   kMatSteam = 8, kMatFire = 9, kMatEmber = 10, kMatAsh = 11,
                   kMatLava = 12, kMatAcid = 13, kMatIce = 14, kMatSnow = 15,
                   kMatDirt = 16, kMatPlant = 17, kMatSeed = 18, kMatSprout = 19,
                   kMatStem = 20, kMatFlower = 21, kMatVine = 22, kMatFungus = 23,
                   kMatDust = 24, kMatMoltenGlass = 25, kMatGlass = 26,
                   kMatSourceWater = 27, kMatSourceSand = 28, kMatSourceLava = 29,
                   kMatVoid = 30, kMatMite = 31, kMatBlood = 32,
                   // forest set: inert (no growth reactions) — see materials.json
                   kMatGrass = 33, kMatLeaves = 34, kMatPineNeedles = 35,
                   kMatAutumnLeaves = 36, kMatBirchWood = 37, kMatPetal = 38,
                   // static micro-detail set (sim/microvox.h): ordinary solid
                   // materials whose CELLS the raymarcher draws as subdiv^3
                   // models. The four *_TUFT/BUSH/FLOWER ids carry a "micro"
                   // block in materials.json; the five below them are the
                   // ordinary materials those models are PAINTED with, so a
                   // petal knocked loose behaves like a petal.
                   kMatGrassTuft = 39, kMatFoliageBush = 40,
                   kMatFlowerPoppy = 41, kMatFlowerDaisy = 42,
                   kMatPetalRed = 43, kMatPetalWhite = 44, kMatPetalYellow = 45,
                   kMatLeafGreen = 46, kMatStemGreen = 47;

// ---- day/night cycle (DESIGN.md §12) ----------------------------------------
// The cycle phase is an INTEGER derived from the sim tick, never from wall
// clock. That is what lets sunlight gate reactions (water evaporating in the
// sun) without breaking CLAUDE.md rule 1: same seed + same tick => same phase
// => same world hash, on every machine.
//
// Phase is 0..kDayPhaseMax over one full day, with 0 = midnight, so:
//   0x0000 midnight   0x4000 sunrise   0x8000 noon   0xC000 sunset
// 16 bits is far finer than the sun visibly moves in one tick and keeps every
// derived comparison in integer range.
constexpr uint32_t kDayPhaseBits = 16;
constexpr uint32_t kDayPhaseMax = 1u << kDayPhaseBits;  // 65536
constexpr uint32_t kDayPhaseMask = kDayPhaseMax - 1u;

// Integer phase for a tick. ticksPerDay comes from tuning (cycle length in
// real minutes x 30 Hz); freeze pins the phase for shot setup and for the
// selftest, whose hash must not depend on how long the cycle happens to be.
// Deliberately integer division: a float here would be a determinism hazard.
inline uint32_t DayPhaseForTick(uint32_t tick, uint32_t ticksPerDay,
                                bool frozen, uint32_t frozenPhase) {
  if (frozen) return frozenPhase & kDayPhaseMask;
  if (ticksPerDay == 0) return 0;
  // (tick % ticksPerDay) first: keeps the multiply from overflowing on a long
  // session, and makes the phase exactly periodic in the tick counter.
  return (uint32_t)(((uint64_t)(tick % ticksPerDay) * kDayPhaseMax) /
                    ticksPerDay) & kDayPhaseMask;
}

// ---- the celestial clock (dev time-scale) ------------------------------------
// The sky is driven by a Keplerian orbital simulation (sim/celestial.h) whose
// only input is a CELESTIAL TICK. Normally that is the sim tick, one for one.
// The dev overlay's time-speed slider decouples them so the sun and moons can
// be run fast, slow, or backwards without touching the sim rate.
//
// Two decisions here are worth defending, because the obvious versions of both
// are wrong:
//
//   * IT IS AN INTEGER COUNTER, not a float accumulator. The SIM reads this
//     clock too — the daylight-gated reactions (water freezing at night, snow
//     melting in the sun) must respond to the accelerated time or cranking the
//     slider shows a racing sun over a world that ignores it, which is worse
//     than useless for tuning weather. Sim state therefore depends on this
//     value, so it has to reach TickParams.dayPhase as a u32 derived from
//     integers (CLAUDE.md rule 1). `scaleNum/scaleDen` is a rational multiplier
//     and `rem` carries the exact remainder, so no float ever touches the path.
//   * SCALE 1 IS BIT-IDENTICAL TO THE OLD BEHAVIOUR. At scaleNum == scaleDen
//     the counter advances by exactly one per tick from zero, so `Ticks()`
//     equals the sim tick and the pinned world hash cannot move. --selftest,
//     --shot and every headless path leave the clock at its default and are
//     structurally unable to observe this feature at all.
//
// Changing the scale away from 1 DOES change the world hash, and that is
// intended and documented: it is a dev tool, and it advertises itself in the
// overlay.
struct CelestialClock {
  // OFF by default, and this is the load-bearing part. While `engaged` is
  // false the clock is not consulted at all: the celestial tick IS the sim
  // tick, byte for byte, and every headless path (--selftest, --shot,
  // --frames, every gate) is structurally unable to observe this feature.
  // It is set true the first time the slider leaves 1.0x and stays true for
  // the session, since by then the two clocks have genuinely diverged and
  // silently snapping back to the sim tick would jump the sky.
  bool engaged = false;

  // Rational time multiplier, exact. den is never 0.
  int64_t scaleNum = 1;
  int64_t scaleDen = 1;
  // Accumulated celestial ticks (signed: reverse time is allowed) and the
  // exact fractional remainder, in units of 1/scaleDen.
  int64_t ticks = 0;
  int64_t rem = 0;
  // The value `ticks` held before the last Advance(). At 100x the clock jumps
  // ~100 day-phase ticks per sim tick, so "did daylight just switch on" cannot
  // be answered against `ticks - 1` — that is a phase the world never
  // occupied, and the day/night wake handshake would sail through several
  // dawns without firing. This is the previous phase the world ACTUALLY saw.
  int64_t prevTicks = 0;

  // Set the multiplier from the UI's float. Quantised to 1/1024 so the counter
  // stays exact; the slider's own step is coarser than that.
  //
  // `simTickNow` is the tick the clock adopts if this is the call that engages
  // it — so the sky does not jump when the slider first moves.
  void SetScale(float s, uint32_t simTickNow) {
    const int64_t den = 1024;
    double v = (double)s * (double)den;
    if (v > 1e6) v = 1e6;
    if (v < -1e6) v = -1e6;
    const int64_t num = (int64_t)(v >= 0.0 ? v + 0.5 : v - 0.5);
    if (!engaged) {
      if (num == den) return;  // still 1.0x: stay disengaged, stay identity
      engaged = true;
      ticks = (int64_t)simTickNow;
      prevTicks = ticks;
      rem = 0;
    }
    if (num == scaleNum && den == scaleDen) return;
    // Re-base the remainder onto the new denominator so a mid-flight scale
    // change does not jump the sky. Denominators are equal in practice; the
    // general form is cheap and means the invariant holds regardless.
    if (den != scaleDen && scaleDen != 0) rem = rem * den / scaleDen;
    scaleNum = num;
    scaleDen = den;
  }

  // Advance by one sim tick. A no-op while disengaged.
  void Advance() {
    if (!engaged || scaleDen <= 0) return;
    prevTicks = ticks;
    // One tick of scaled time, carried exactly: `acc` is the whole position in
    // units of 1/scaleDen, so nothing is ever rounded away and the counter is
    // identical whichever order the frames arrived in.
    //
    // Written as one accumulator rather than as a separate ticks/rem pair with
    // a division per step, because the pair form is easy to get subtly wrong:
    // an earlier version floor-divided `rem` after adding, which is correct in
    // isolation but double-applied the carry whenever a scale change had left
    // a remainder behind. This form has no carry to lose.
    const int64_t acc = ticks * scaleDen + rem + scaleNum;
    // Floor toward negative infinity so reverse time is the exact mirror of
    // forward time rather than drifting by a tick per second.
    int64_t q = acc / scaleDen;
    int64_t r = acc % scaleDen;
    if (r < 0) { q -= 1; r += scaleDen; }
    ticks = q;
    rem = r;
  }

  // The integer celestial tick the sim's day phase is derived from. Clamped
  // non-negative: DayPhaseForTick takes a u32, and a negative clock would wrap
  // into a phase that jumps. Reverse time still runs the RENDER sky backwards
  // (RenderTick is a double and handles negatives exactly) — it just holds the
  // sim's day phase at the epoch once it walks past it, which is the only
  // sensible reading of "the reactions ran before the world began".
  uint32_t SimTick(uint32_t simTick) const {
    if (!engaged) return simTick;
    return ticks <= 0 ? 0u : (uint32_t)ticks;
  }

  // The celestial tick the world occupied on the PREVIOUS sim tick. Used by
  // the day/night wake handshake, which asks "did daylight switch between then
  // and now" — a question `SimTick() - 1` answers wrongly at any scale but 1.
  uint32_t PrevSimTick(uint32_t simTick) const {
    if (!engaged) return simTick == 0 ? 0u : simTick - 1u;
    return prevTicks <= 0 ? 0u : (uint32_t)prevTicks;
  }

  // The (possibly fractional, possibly negative) tick the RENDER sky uses.
  double RenderTick(uint32_t simTick) const {
    if (!engaged) return (double)simTick;
    return (double)ticks + (scaleDen ? (double)rem / (double)scaleDen : 0.0);
  }

  // Interpolated render tick: advances the celestial clock by `frameFrac`
  // (0..1, how far into the next sim tick this frame is) scaled by the clock's
  // speed. At 1x the offset is sub-tick and invisible; at 100x it smooths the
  // 100-celestial-tick jumps that otherwise make the sun/moons visibly step.
  double RenderTickInterp(uint32_t simTick, double frameFrac) const {
    if (!engaged) return (double)simTick + frameFrac;
    double base = (double)ticks + (scaleDen ? (double)rem / (double)scaleDen : 0.0);
    double speed = scaleDen ? (double)scaleNum / (double)scaleDen : 1.0;
    return base + frameFrac * speed;
  }
};

// The one clock the game's sky and daylight-gated reactions run on.
//
// A global rather than a parameter threaded through SubmitTick/
// WriteRenderParams, because it must reach ~15 call sites across the frame
// loop, the shot paths and the selftest, and every one of them that DIDN'T
// pass it would silently fall back to the sim tick — a divergence between what
// you see and what the world does, which is precisely the failure this whole
// subsystem is built to prevent. It is written only by the frame loop
// (main.cpp), which advances it exactly once per sim tick.
//
// Every headless path (--selftest, --shot, --frames) leaves it at its default
// identity scale and never calls Advance() out of step with the tick, so those
// paths see celestialTick == tick and the pinned world hash is untouched.
CelestialClock& Celestial();

// Integer daylight strength for a phase: 0 at night, rising to 255 at noon.
// EXACT mirror of daylightStrength() in common.wgsl — the CPU uses it to
// decide which ticks need a wake-all, and the GPU uses it to gate reactions.
// If these two ever disagree the world still hashes identically (only the GPU
// copy touches voxel state), but chunks would wake on the wrong tick, so keep
// them in step.
constexpr uint32_t kDaySunrise = 16384, kDayNoon = 32768, kDaySunset = 49152;
inline uint32_t DaylightStrengthCpu(uint32_t phase) {
  uint32_t p = phase & kDayPhaseMask;
  if (p <= kDaySunrise || p >= kDaySunset) return 0;
  uint32_t d = p - kDaySunrise;
  uint32_t half = kDayNoon - kDaySunrise;
  uint32_t up = d <= half ? d : 2 * half - d;
  return (up * 255) / half;
}

// Must match BrushOp in common.wgsl (32 bytes).
struct BrushOp {
  int32_t x, y, z;
  int32_t radius;
  uint32_t material;
  uint32_t mode;  // 0 = paint into air, 1 = overwrite
  uint32_t pad0 = 0, pad1 = 0;
};
constexpr uint32_t kMaxOpsPerTick = 64;

// Must match ExplosionOp in common.wgsl (32 bytes). Part of the MutationQueue
// discipline: explosions enter the sim only through this op stream, so saves/
// replays/networking capture them for free (DESIGN.md §2).
struct ExplosionOp {
  int32_t x, y, z;
  int32_t radius;   // <= kMaxExplosionRadius
  int32_t power;    // hardness budget at the center
  uint32_t pad0 = 0, pad1 = 0, pad2 = 0;
};
constexpr uint32_t kMaxExplosionsPerTick = 8;
constexpr int32_t kMaxExplosionRadius = 20;  // EXP_R_MAX in common.wgsl
constexpr uint32_t kExplosionWg = 11;        // EXP_WG in common.wgsl

// Particle system sizes — must match common.wgsl.
constexpr uint32_t kParticleCap = 262144;
constexpr uint32_t kClaimSize = 262144;

// ---- MLS-MPM fluid (docs/PLAN_mpm_fluids.md; excite/settle seam Phase 2) ----
// The EXCITED state of liquid: GPU particles simulated by the fixed-point
// MLS-MPM solver (sim_fluid.wgsl). Settled liquid stays voxels. The seam
// (sim_fluid_seam.wgsl) converts both ways: excite turns disturbed settled
// cells into particles (one per fullness eighth), settle bins calm particles
// back into fullness voxels — exact integer mass accounting in both
// directions. The seam DOES write voxels, deterministically, so the world
// hash moves only when fluid converts; a world that never spawns fluid is
// bit-identical to one before this system existed (the pinned determinism
// hash is the gate on that claim).
//
// The particle COUNT is GPU-OWNED (fluidArgsStage[7]): settle kills
// particles and excite births them on the GPU, so no CPU-side count can be
// authoritative. A deterministic ping-pong compaction at the head of each
// fluid tick (slot-order scans, no atomicAdd slot assignment) removes the
// corpses; per-particle passes dispatch indirectly. The CPU keeps a
// CONSERVATIVE estimate from the async readback (world.Snap().fluidLive) for
// record/skip and render decisions only. Not persisted: save/load and
// worldgen drop the fluid, per the plan's force-settle-on-save policy.
constexpr uint32_t kFluidCap = 262144;            // hard particle budget (rule 2)
constexpr uint32_t kMaxFluidSpawnsPerTick = 4096; // spawn-op stream cap
// Sparse scratch-grid blocks: one 16^3 node block per ACTIVE chunk slot,
// allocated per substep by a deterministic scan. 256 blocks * 4096 nodes *
// 32 B = 32 MiB, and bounds simultaneously-active fluid to 256 chunks.
constexpr uint32_t kFluidBlocks = 256;
// MPM substeps per 30 Hz tick — the FALLBACK DEFAULT only. Since WP3 this is
// the tuning knob `sim.fluidSubsteps`, because it is the CFL budget that makes
// a chosen stiffness legal and, at the same time, the solver's whole per-tick
// price. EncodeTick records the substep table CurrentTuning().sim.fluidSubsteps
// times; the shader side derives FLUID_SUBSTEPS / FLUID_VMAX / FLUID_MARK_PAD
// from the same knob in common.wgsl. This constant must equal the .def default
// — scripts/check_invariants.py asserts the pair.
// CFL: |v| <= 0.45 cell/substep, so the fluid's terminal speed is
// 0.45 * substeps cells/tick (9 -> 4.05 cells/tick, ~12.2 m/s at 0.10 m
// voxels).
constexpr uint32_t kFluidSubsteps = 9;
// FluidParticle stride in u32 words — must match the struct in common.wgsl
// (32 words / 128 B, power-of-two for coalesced access).
constexpr uint32_t kFluidParticleWords = 32;
// Settle converts at most this many blocks per tick. Bounds the bin scratch
// (kFluidSettleMax * kChunkVol * 2 words) and, with the COLUMN exclusion in
// the settle scan (same chunk column, within one chunk in y — the exact reach
// of a column walk's spill), guarantees no two concurrently-committing blocks
// can read each other's writes. Lateral neighbours may settle together: their
// column walks are disjoint, and the check/commit pass barrier means every
// stability probe reads pre-commit voxels. A lake's worth of calm blocks drains through
// this in a few ticks; settle latency is invisible next to the calm window.
constexpr uint32_t kFluidSettleMax = 16;

// One CPU-authored fluid particle spawn (32 B) — must match FluidSpawnOp in
// common.wgsl. Positions are ABSOLUTE world cells in Q16.16 fixed point
// (fraction bits matter: particles sit at sub-cell lattice offsets), velocity
// is Q16.16 cells/tick. Part of the per-tick input stream like ParticleSpawn.
struct FluidSpawnOp {
  int32_t px, py, pz;   // position, fixed 16.16 world cells
  int32_t vx, vy, vz;   // velocity, fixed 16.16 cells/tick
  uint32_t species = 0; // 0..3: grid species-mass slot (colour + attraction)
  uint32_t mat = 0;     // material id for the particle's attr word — settle
                        // writes this back as the voxel, splash droplets and
                        // staining key on it
};

// Rigid-body render slots shared by debris + mob limbs (BodyVoxInst packs the
// slot in bits 16..27, so the hard ceiling is 4096). Debris bodies take slots
// [0, debrisCount), mob limbs stack after them.
constexpr uint32_t kMaxBodySlots = 512;

// CPU-authored render instance (grenades, markers) — must match Sprite in
// debris.wgsl (32 bytes). Render-only: floats are fine here.
struct Sprite {
  float pos[3];
  float halfSize;
  uint32_t color;   // 0xAABBGGRR
  float emission;
  uint32_t pad0 = 0, pad1 = 0;
};
constexpr uint32_t kMaxSprites = 64;

// One ORIENTED wireframe box for the collision-box debug overlay.
struct DebugBox {
  float pos[3];
  float pad0 = 0;
  float half[3];
  float pad1 = 0;
  float quat[4];
  uint32_t color;
  uint32_t pad2 = 0, pad3 = 0, pad4 = 0;
};
static_assert(sizeof(DebugBox) == 64, "must match DebugBox in debug_lines.wgsl");
constexpr uint32_t kMaxDebugBoxes = 1024;

// Exact-cell MutationQueue op (8 bytes) — island removal / rubble handoff
// (DESIGN.md §7). Must match sim_mutate.wgsl entry `cells`.
struct CellOp {
  uint32_t cellIdx;  // linear chunk-major cell index
  uint32_t word;     // full voxel word to store (stamp field included)
};
constexpr uint32_t kMaxCellOpsPerTick = 65536;
// CellOp.word flag in the TOP spare bit (31): only write if the target cell
// is air (prefab paint mode). Masked off by sim_mutate before the store, so
// it never lands in the grid. Bit 31 specifically, because bits 24..30 of a
// stored word are now the stain layer (below) and a CellOp carries real stain
// bits through to the grid.
constexpr uint32_t kCellOpIfAir = 0x80000000u;

// ---- the voxel word ----
// bits 0..11 material, 12..15 state, 16..18 tick-stamp, 19..23 FREE,
// 24..27 stain amount, 28..30 stain type, 31 kCellOpIfAir (never stored).
//
// ---- the tick stamp ----
// EXACT mirror of the STAMP_* consts in common.wgsl (that file is what the
// shaders see; this is for the CPU paths that build voxel words). The stamp is
// per-tick scheduling scratch, not state: it is excluded from the world hash
// (sim_occupancy.wgsl) and stripped on save (kPersistMask in stream.cpp).
//
// kStampNever is the ONLY value a CPU path should ever write. No stampFor()
// output equals it, so a voxel carrying it is free to move on the first tick
// it is simulated — which is what every CPU-built word wants: a brush paint, a
// prefab stamp, an RLE decode after a stream-in or a load. Writing a live code
// instead would make that voxel sit out a substep.
//
// This was a 0xFF byte before the field narrowed to 3 bits. 0xFF masked into
// 3 bits is 7 — a REAL stamp code — so any site still writing the old literal
// silently becomes "already acted" one tick in seven. If you are reading this
// because you found such a site, it is a bug; use kStampNever.
constexpr uint32_t kStampShift = 16, kStampMask = 0x7;
constexpr uint32_t kStampBits = 0x70000u;
constexpr uint32_t kStampNever = 0;
inline uint32_t VoxStamp(uint32_t w) { return (w >> kStampShift) & kStampMask; }
// The word a CPU path stores for a freshly created voxel.
inline uint32_t PackVoxNew(uint32_t mat, uint32_t state) {
  return (mat & 0xFFFu) | ((state & 0xFu) << 12) | (kStampNever << kStampShift);
}

// ---- the free span ----
// Bits 19..23: unallocated. Neither hashed nor persisted (see above), so they
// hold per-tick scratch only unless the hash mask in sim_occupancy.wgsl and
// kPersistMask in stream.cpp are BOTH widened to cover them. Claiming them
// means saying so here and in the common.wgsl allocation table.
constexpr uint32_t kFreeBits = 0x00F80000u;

// ---- sub-chunk occupancy bitmask (PLAN_surface_flight_perf.md A2) ----------
// RENDER-ONLY DERIVED DATA, appended to the `occupancy` buffer after the
// per-chunk count words. Deliberately NOT its own rhi::Buffer: every barrier,
// every pass_table.def `uses` row and every bind-group entry for Occupancy
// already exists and already covers it, and a new buffer with a missing `uses`
// row is a cross-vendor desync that reproduces on no local machine.
//
// The chunk-level skip is all-or-nothing at 1.6 m and keyed on a COUNT, so one
// grass voxel in 4,096 forces 16-48 per-voxel steps through the chunk. Two
// content classes make that systematic: micro/grass is excluded from the
// blocker count (isRayBlocker) but still counts in the total that PRIMARY rays
// test, and tree foliage has no micro block at all, so a canopy is a wall of
// half-full chunks. This mask lets both skip INTERNALLY.
//
// TWO CLASSES, because the two ray kinds ask different questions and one mask
// cannot answer both: primary rays test occTotal (any non-air cell), media-
// blind shadow/reflection rays test occBlockers. Using the blocker mask for a
// primary ray would skip gas, water and grass; using the total mask for a
// shadow ray is merely pessimistic. Both are stored, `wantMedia` picks.
//
// GRANULARITY IS THE KNOB. kSubOccShift is the block edge as a shift in
// voxels: 2 => 4-voxel (40 cm) blocks on a 4^3 grid, 3 => 8-voxel blocks on a
// 2^3 grid. Storage is FIXED at two words per class either way, so the shader
// is identical for both and the choice is a one-constant A/B (shift 3 simply
// leaves 56 of the 64 bits unused). Anything finer than shift 2 would need a
// third word and dynamic indexing in the producers' per-thread accumulators.
constexpr uint32_t kSubOccShift = 2;
constexpr uint32_t kSubOccDim = kChunk >> kSubOccShift;   // blocks per chunk edge
constexpr uint32_t kSubOccWords = 2;                      // 64 bits per class
constexpr uint32_t kSubOccStride = kSubOccWords * 2;      // total words, then blocker words
static_assert(kSubOccDim * kSubOccDim * kSubOccDim <= kSubOccWords * 32,
              "sub-chunk block grid does not fit in kSubOccWords");

// ---- the software page table (docs/PLAN_page_table.md) ---------------------
// DERIVED DATA ONLY: the page table is a physical-layout index, not world
// state. It is not hashed, not persisted, and not replicated. It is rebuilt
// from the chunk contents on every load, stream-in and worldgen (§4.2).
//
// One u32 per CHUNK SLOT (not per world chunk coord — memory is slot-indexed
// and never shifts, so the table shifts the way the voxels do, which is to say
// not at all). Indexed by exactly what SlotChunkIndex / chunkIndexOf produce.
//
//   bit 31 = 0  RESIDENT.  bits 0..30 = PAGE INDEX into the physical pool.
//   bit 31 = 1  SENTINEL.  bit 30 = JITTER tag, bits 12..29 = spare (zero),
//                          bits 0..11 = material id.
//
// EMPTY is UNIFORM(air): kPtEmpty == kPtSentinelBit | kMatAir with kMatAir 0,
// so there is ONE sentinel decode path and "empty" is not a special case
// anywhere in the shader. The material field shares the voxel word's material
// position, so synthesizing a word from a sentinel is a mask, not a repack.
//
// ---- the JITTER sentinel (bit 30) ----
// UNIFORM's whole-word rule is exact but nearly useless underground: worldgen
// gives every solid cell a palette variant `hash3(...) % 3` in the state
// nibble, so a chunk of plain stone has three distinct words and cannot be
// one. Commit 0 measured the gap: 41 of 32,768 chunks are whole-word uniform
// against 2,115 that are ONE MATERIAL with mixed state. Those 2,115 are the
// buried bulk — the voxels the player never touches until they dig.
//
// JITTER(mat) means "every cell is `mat`, stainless, kStampNever, and its
// state nibble is exactly the worldgen palette variant for that cell's WORLD
// position". That is representable because the variant is not random: it is
// `hash3(seed ^ 0xC0FFEE, x ^ (z << 12), y) % 3`, a pure function of position
// and seed (worldgen.wgsl genCell). So the chunk's 4,096 words are describable
// by 4 bytes plus a formula, and readers, the hash and materialization all
// reconstruct them on demand.
//
// TWO CONSEQUENCES, both load-bearing:
//   1. Synthesis is POSITIONAL. SynthWord(entry) alone cannot serve a JITTER
//      sentinel — it needs the cell's world coordinate. Every synthesis site
//      therefore takes a world cell, and the chunk-linear sites (which hold a
//      SLOT index) must recover the world chunk through the window origin.
//      A slot index is NOT a world position: the window is toroidal.
//   2. Materialization is no longer a vkCmdFillBuffer. A 32-bit fill pattern
//      cannot express per-cell variation, so a JITTER page is filled by a
//      small compute kernel (sim_pagefill.wgsl) instead. EMPTY and UNIFORM
//      keep the one-command fill.
//
// Liquids are excluded by construction: worldgen writes LIQ_FULL_STATE, not a
// variant, so a liquid chunk's cells are whole-word uniform and UNIFORM already
// covers them. JITTER is offered only for materials whose worldgen state is the
// `% 3` variant — see JitterStateFor.
constexpr uint32_t kPtJitterBit = 0x40000000u;
//
// These are mirrored into WGSL by ShaderConstantPrelude() (gpu/resources.cpp)
// per the "world constants are generated from world.h, never redeclared in
// WGSL" invariant — do not restate them in common.wgsl.
constexpr uint32_t kPtSentinelBit = 0x80000000u;
constexpr uint32_t kPtMatMask = 0x00000FFFu;
constexpr uint32_t kPtEmpty = kPtSentinelBit | kMatAir;   // 0x80000000
constexpr uint32_t kPtPageMask = 0x7FFFFFFFu;
// A sentinel holding material 0xFFF — an id that cannot exist (ids are handed
// out from the bottom, ~48 used, and the top entries are the inert stain/art
// palettes). NOT used on the tick path: it is the poison value an entry holds
// between "freed" and "rewritten", and a read through it synthesizes a
// material whose table entry is zeroed — class 0 (solid), density 0. That is
// deliberately VISIBLE: a translation bug shows up as a wall of impossible
// solid rather than as silent air.
//
// NB (JITTER): 0xFFFFFFFF also has the JITTER bit set, so a read through it
// now synthesizes a POSITION-VARYING impossible material rather than a uniform
// one. That is still exactly as visible and still impossible, so the diagnostic
// property above is intact. This value is never COMPARED against anywhere
// (grep: it is declared here and mirrored to WGSL, and that is all) — it is a
// payload, not a tag, so widening the tag space cannot alias it.
constexpr uint32_t kPtUnresident = 0xFFFFFFFFu;
// Not a valid word index. voxWordIndex() returns it for a sentinel chunk and
// voxStore() tests for it BEFORE indexing, which is what makes "a kernel can
// never write through a sentinel" a property of the signatures rather than of
// anyone remembering a rule (§2.4).
constexpr uint32_t kPtNoWord = 0xFFFFFFFFu;

// ---- the retire headroom, and why the pool is not exactly kNumChunks -------
//
// A page freed by the hysteresis free probe does not go straight back on the
// free list: it is PARKED for kPageRetireTicks so any in-flight eviction copy
// referencing it can complete first (pagetable.h, "THE RETIRE QUEUE"). Freeing
// is capped at kPageFreeProbesPerTick per tick, and the tick counter strictly
// increments once per SubmitTick, so the queue holds at most the product.
//
// THAT PRODUCT IS WHY A POOL OF EXACTLY kNumChunks ABORTS, which cost two
// crashes on 2026-08-30 and one wrong "structurally impossible" diagnosis.
// The reasoning that fails is seductive and worth spelling out: a slot holds
// at most one page and there are kNumChunks slots, so demand can never exceed
// kNumChunks, so a pool of kNumChunks can never run dry. Every step is true
// and the conclusion is false, because pages leave the free list by TWO doors
// and that argument only counts one. Live slots hold up to kNumChunks, the
// retire queue holds up to the ceiling below, and the free list is what is
// left. Exhaustion therefore begins once residency passes
// kNumChunks - kPageRetireCeiling — 93.75% at the 512 window, which is exactly
// the "94-100% resident" band every observed abort has landed in.
//
// These two live HERE rather than in pagetable.h because kPoolPages is derived
// from them and world.h cannot include pagetable.h (pagetable.h includes this
// file). pagetable.h's kMaxFreeProbesPerTick / kRetireTicks alias these, so
// there is still exactly one definition of each number.
constexpr uint32_t kPageFreeProbesPerTick = 128;
constexpr uint32_t kPageRetireTicks = 16;
constexpr uint32_t kPageRetireCeiling = kPageFreeProbesPerTick * kPageRetireTicks;

// Physical pages in the pool under --residency paged. DERIVED FROM kWorldN,
// never a literal: one page per chunk slot in the window, plus the retire
// headroom above.
//
// The derivation is what makes exhaustion impossible rather than merely
// unlikely, and the proof is three lines:
//
//   - Alloc() is only ever reached for a SENTINEL slot, so at the moment of a
//     request at most kNumChunks - 1 slots hold a page.
//   - The retire queue holds at most kPageRetireCeiling.
//   - freePages = kPoolPages - resident - retired >= 1. Always.
//
// It is not "headroom for an unbounded demand" — it is a pool that exceeds a
// demand with a hard geometric ceiling. There cannot be a request for page
// kNumChunks + 1 because there is no chunk slot to hand it to.
//
// CHANGE kWorldN AND THIS FOLLOWS, which is the point: the pool tracks the
// window instead of being re-measured and re-guessed every time the window
// moves. The static_assert below is the guard rail for that — see it for why
// kWorldN = 1024 does NOT "just work" on current hardware.
//
// This constant is the RESERVED pool, not resident content; conflating the two
// is how a phase claims a win it did not get (§3.7). The paging win is
// entirely a TYPICAL-case win — half the resident set in ordinary play — and
// explicitly NOT a worst-case one.
//
// SIZED FROM THE ADVERSARIAL TRAVERSAL, which is the only sizing input that
// ever held up. The history is worth keeping because each number was honestly
// measured and each was still too small:
//
//   4,975 resident / 14,934 suite high-water   — the phase-7 harness numbers.
//     True, but the harness window is mostly SKY. A pool sized here aborts.
//   16,420 settled / 16,744 peak (game window) — the default-flip numbers, a
//     player STANDING on terrain, half the window underground. A 16,384 pool
//     aborted ON LAUNCH. This is what 24,576 was sized to, at "1.47x steady
//     state" against a self-invented "1.25x headroom rule".
//   23,236 peak (game window, FLYING)          — sustained sprint-flight runs
//     a ~20,230 working set. 24,576 is 1.06x THAT, not 1.47x anything, and
//     sustained flight duly aborted the process (2026-08-23).
//   32,365 peak (ADVERSARIAL: diagonal + descending into solid rock)
//     — 98.8% OF DENSE. cpuDirty stayed 1,500-4,800 throughout, so this is
//     genuine residency, not mirror dilation: underground there is no sky to
//     sentinel away and nearly every slot needs a real page.
//
// THE STRUCTURAL CONCLUSION, which is why this is dense-sized and not 28,000:
// the page table's worst case IS dense, so no pool below dense can make
// exhaustion impossible, and any value in between only picks how unlucky the
// player has to be. §3.8 keeps exhaustion fatal — with the pool at dense PLUS
// the retire ceiling that abort becomes a genuine assertion (an allocator bug)
// rather than a routine outcome a fast descent or a world-wide explosion can
// provoke.
//
// A NOTE ON WHAT ACTUALLY FILLS IT, because the two adversarial harnesses do
// not: --autofly-hard peaks at 17,769 of 32,768 (54.2%) and a full --selftest
// at 23,631 (72.1%), yet the game aborted twice in ordinary play. Both
// harnesses stress STREAMING (window shifts). What reaches the 94-100% band is
// ACTIVITY: Materialize's set is cpuDirty u hasMatter u opTargets u
// particleChunks u fluidChunks, and smoke/fire/particles rising into open sky
// convert the PT_EMPTY sky sentinels — normally about half the window, and
// free — into real pages. Size against a world-wide wake, not against flight.
//
// "Synthetic numbers lie", now twice over: measure with the GAME window AND an
// adversarial path. `--autofly-hard` (main.cpp) is that path — diagonal strafe
// plus descent on a fixed TICK schedule so it is reproducible run to run —
// and `SANDVOX_PT_DEBUG=1` prints per-tick residency.
//
// The real lever on the underground working set is compression, not a bigger
// pool (there is no bigger pool): ~all of it is single-material-with-state
// chunks a WIDENED SENTINEL could represent (PLAN_page_table.md §3.6's
// 2,115-chunk finding, which the game window multiplies). That matters most
// for the 5 cm / extended-radius target, where volume grows 8x and bulk
// terrain gets MORE internally uniform, not less — sentinel compression
// improves at finer resolution while sky compression stays flat.
constexpr uint32_t kPoolPages = kNumChunks + kPageRetireCeiling;

// THE WINDOW-SIZE GUARD RAIL. `voxels` is ONE storage buffer of kPoolPages
// pages, and Vulkan's maxStorageBufferRange is a uint32_t — so 4 GiB - 1 is
// not this machine's limit, it is the SPEC's absolute ceiling, and no driver
// can offer more for a single binding. (Measured on the 3060 Ti: exactly
// 4294967295. --vk-info prints it.)
//
// This is what stands between "change kWorldN and it just works" and a runtime
// failure that would surface as a buffer-creation error or, worse, as silent
// out-of-range addressing. Concretely, at the next power of two up:
//
//   kWorldN = 1024 -> kNumChunks = 262,144 -> kPoolPages = 264,192
//                  -> voxels = 4,328,521,728 B = 4.03 GiB. DOES NOT FIT.
//
// So 1024 is not a one-constant change: the voxel buffer has to be SPLIT
// across bindings (or the pool decoupled from dense) before the window can
// double again. Note this bites at 1024 even ignoring VRAM — 4.03 GiB of pool
// plus 1.0 GiB of farVox is 5 GiB on an 8 GiB card before render targets, so
// the descriptor limit and the memory budget arrive at roughly the same place.
//
// A compile error here is the correct outcome: it names the real blocker at
// the moment the constant changes, instead of after a build and a crash.
static_assert((uint64_t)kPoolPages * kChunkVol * 4ull <= 0xFFFFFFFFull,
              "voxels buffer exceeds the 4 GiB storage-binding ceiling: "
              "kWorldN is too large for a single binding. Split the pool "
              "across buffers, or reduce kWorldN.");

// The word a sentinel chunk's cells read as. THIS IS THE HASH CONTRACT (§4.1):
// it must be bit-identical to what a materialized page would hold, which is
// guaranteed structurally — the materializing fill pattern, the shader read
// accessor and the analytic hash branch all come from this one rule, mirrored
// once into WGSL as synthWord(). synthWord(kPtEmpty) == 0.
//
// The state nibble is 0 and the stamp is kStampNever, both load-bearing:
// a UNIFORM sentinel carries only 12 bits of material and so cannot represent
// a chunk whose cells differ in state, which is why promotion is by WHOLE-WORD
// equality and never by material equality (§2.3, risk 3). And a sentinel chunk
// is by definition one that has not been simulated in place, so every voxel in
// it must be free to act on the first tick it is dispatched.
inline uint32_t SynthWord(uint32_t entry) {
  const uint32_t mat = entry & kPtMatMask;
  if (mat == kMatAir) return 0u;
  return PackVoxNew(mat, 0u);
}

// ---- the JITTER synthesis rule: the SECOND half of the hash contract -------
// EXACT mirror of genCell's variant assignment (worldgen.wgsl) and of
// synthJitterState / synthWordAt in common.wgsl. Four copies of one formula is
// three too many, but the alternatives are worse: worldgen is a shader, the
// hash path is a shader, and the CPU needs it for eviction and the mirror. The
// page-roundtrip gate asserts all of them agree, which is what makes this a
// checked duplication rather than a "two places must agree" bug in waiting.
//
// Integer-only (rule 1): pcg/hash3 are the same u32 mask-shift-multiply chain
// the sim RNG uses, so there is no arithmetic a compiler may contract. It uses
// rng::Hash3 — the ONE CPU mirror of common.wgsl's hash3 — rather than a local
// copy, for exactly the reason rng.h's header comment gives.
//
// The palette variant worldgen gives the cell at world position (x,y,z).
// Mirrors worldgen.wgsl genCell: rnd = hash3(seed ^ 0xC0FFEE,
// x ^ (z << 12), y), state = rnd % 3. The bitcast to u32 of a negative
// coordinate is two's complement in both languages (the documented WGSL
// bitcast trap) — C++ gets it from the same (uint32_t) cast.
inline uint32_t JitterStateFor(int x, int y, int z, uint32_t seed) {
  const uint32_t rnd = rng::Hash3(seed ^ 0xC0FFEEu,
                                  (uint32_t)x ^ ((uint32_t)z << 12), (uint32_t)y);
  return rnd % 3u;
}
// The word a JITTER(mat) sentinel's cell at world (x,y,z) reads as.
inline uint32_t SynthWordAt(uint32_t entry, int x, int y, int z, uint32_t seed) {
  const uint32_t mat = entry & kPtMatMask;
  if (mat == kMatAir) return 0u;
  if ((entry & kPtJitterBit) == 0u) return PackVoxNew(mat, 0u);
  return PackVoxNew(mat, JitterStateFor(x, y, z, seed));
}

// ---- the ROW form of the JITTER rule: same words, two thirds the work ------
//
// SynthWordAt is the DEFINITION and stays the definition. This is a strictly
// derived helper for the two paths that synthesize or verify a whole chunk
// (eviction's sentinel RLE, Classify's JITTER test), both of which walk cells
// in x-fastest order and therefore call the definition 4,096 times with only
// `x` changing across each 16-cell row.
//
// Hash3(a,b,c) = Pcg(a ^ Pcg(b ^ Pcg(c))), and the JITTER key is
// a = seed^0xC0FFEE, b = x ^ (z << 12), c = y. Across one row y and z are
// FIXED, so Pcg(c) is loop-invariant and only the outer two rounds vary. The
// row form hoists it, which is a pure strength reduction: the arithmetic that
// remains is bit-identical to what the definition computes, so every word this
// produces equals SynthWordAt's for the same cell. It is not a second rule and
// must never become one — if the definition changes, this changes with it, and
// the page-roundtrip gate compares them.
//
// Measured motivation: eviction synthesized 1.37 M chunks (5.6 G hash evals)
// over one --autofly-hard run, at 55% of the paged frame cost. Removing one of
// three PCG rounds is the cheapest third of that, and it is hash-invisible by
// construction.
inline uint32_t JitterRowSeed(int y, int z, uint32_t seed) {
  // Everything in Hash3's inner round that does not depend on x: Pcg(y), and
  // the z half of b. `x` is xor'd in by JitterStateInRow below.
  (void)seed;
  return rng::Pcg((uint32_t)y) ^ ((uint32_t)z << 12);
}
inline uint32_t JitterStateInRow(uint32_t rowSeed, int x, uint32_t seed) {
  // rowSeed = Pcg(y) ^ (z << 12); b ^ Pcg(c) = (x ^ (z<<12)) ^ Pcg(y) = x ^ rowSeed
  return (rng::Pcg((seed ^ 0xC0FFEEu) ^ rng::Pcg((uint32_t)x ^ rowSeed))) % 3u;
}

// ---- the stain layer (DESIGN.md §3) ----
// Bits 24..30 of the voxel word: 4-bit amount, 3-bit type. EXACT mirror of the
// STAIN_* consts in common.wgsl — that file is the one the shaders see, this
// one is for the CPU paths that build voxel words (prefabs, debris rubble,
// worldio) and must not scribble on a stain or invent one.
// Type 0 = unstained; 1..7 are palette slots registered from materials.json in
// file order (see MaterialDef::stain in materials.h).
constexpr uint32_t kStainAmtShift = 24, kStainAmtMask = 0xF;
constexpr uint32_t kStainTypeShift = 28, kStainTypeMask = 0x7;
constexpr uint32_t kStainAmtMax = 15, kStainTypeMax = 7;
constexpr uint32_t kStainBits = 0x7F000000u;
inline uint32_t VoxStainAmt(uint32_t w) { return (w >> kStainAmtShift) & kStainAmtMask; }
inline uint32_t VoxStainType(uint32_t w) { return (w >> kStainTypeShift) & kStainTypeMask; }
inline uint32_t PackStain(uint32_t type, uint32_t amt) {
  return ((amt & kStainAmtMask) << kStainAmtShift) |
         ((type & kStainTypeMask) << kStainTypeShift);
}

// ---- the stain palette ----
// The renderer needs stain TYPE (3 bits in the voxel word) -> colour, but a
// type is not a material id, so it cannot index the material table directly.
// Rather than add a buffer and a bind slot for eight colours, the palette is
// mirrored into the TOP 8 entries of the 4096-entry material table, which is
// otherwise all zeroes (there are ~39 real materials and the 12-bit id space
// is nowhere near full). Entry kStainPaletteBase + type holds that stain's
// colour in its `stainColor` field; Simulation::UploadTables fills them.
//
// This rides the existing table upload, so stains hot-reload with R along with
// everything else in materials.json and no shader gains a binding. The entries
// are inert otherwise: class 0, density 0, no reactions, and nothing can
// reference them because material ids are assigned from the bottom up.
//
// Lives in world.h rather than materials.h because ShaderConstantPrelude() and
// scripts/check_shaders.sh both generate the WGSL constants from THIS file.
constexpr uint32_t kMaterialSlots = 4096;
constexpr uint32_t kStainPaletteBase = kMaterialSlots - 8;

// ---- the art palette ----
// Same trick as the stain palette above, for the same reason. A mob's skin
// carries a per-voxel ART COLOUR that is independent of its material — a
// creature is "meat" everywhere and painted all over — so the renderer needs
// slot -> colour for something that is not a material id. These 128 entries
// sit just below the stain palette, are filled by Simulation::UploadTables
// from the loaded prefabs' art palettes, and are inert as materials (nothing
// can reference them: ids are assigned from the bottom up).
//
// 128 matches kArtPaletteSlots in sim/voxload.h, which is the .vox side of the
// same range; the two must agree. Art colour is render-only and never reaches
// a world cell, so none of this can move the world hash (rule 1).
constexpr uint32_t kArtPaletteSlotsGpu = 128;
constexpr uint32_t kArtPaletteBaseGpu = kStainPaletteBase - kArtPaletteSlotsGpu;

// ---- static micro-detail brick pool (render-only — sim/microvox.h) ----------
// The raymarcher substitutes a subdiv^3 voxel model for cells of a MATF_MICRO
// material. All frames of all such materials live in ONE pool buffer, indexed
// by a per-material table of kMaterialSlots entries. Both are bound to the
// raymarch pipeline and to nothing else: they are render data, and putting them
// on a sim shader's bind group would make render state a sim input (rule 1).
//
// Lives here rather than in microvox.h because ShaderConstantPrelude() and
// scripts/check_shaders.sh both generate their WGSL constants from THIS file.
// 4 MiB is room for ~32k subdiv-8 frames — a hard bound, not an open-ended
// allocation (rule 2).
constexpr uint32_t kMicroPoolWordsWorld = 1u << 20;

// ---- dynamic microvoxel bodies (render-only — sim/microbody.h) -------------
// A mob def with "skinScale": 2|4|8 authors its limbs at that many skin voxels
// per WORLD voxel. Those limb models are packed once at load into their own brick
// pool and drawn by rasterizing each body's OBB and marching the brick per
// fragment (microbody.wgsl), instead of one cube instance per voxel.
//
// Same rules as the static pool above: render-only, never bound in a sim
// shader, constants generated from THIS file so the WGSL and the C++ agree.
//
// Sized for the worst case rather than the typical one, because a pool ceiling
// is a worst-case structure: the whole scale-2 critter is 218 words, but a
// detailed 8x-skin limb runs to tens of thousands, and copy-on-write doubles the
// live set exactly when it matters — the detailed entities are the ones taking
// hits. The old 64 KiW ceiling predated the skin/collider split and had no room
// for that at all.
//
// The buffer is allocated at this size UNCONDITIONALLY (simulation.cpp), so this
// number is VRAM every world pays whether or not it has a micro body. 1 MiW =
// 4 MiB buys room for ~4M micro voxels — a fleet of 8x-skin characters — while
// staying in the same order as the static pool above rather than quietly
// becoming the largest buffer in the engine. It remains a HARD ceiling (rule 2):
// past it MicroBodyOwn fails and the body keeps a stale skin, which is the
// documented graceful degradation, not an unbounded allocation.
constexpr uint32_t kMicroBodyPoolWordsWorld = 1u << 20;
// Per-limb model records, indexed by body slot through kMaxBodySlots-sized
// slot table. One record per (def, limb) pair across every loaded mob def.
constexpr uint32_t kMaxMicroBodyModels = 256;
// A micro body's model has no micro model when its slot maps here.
constexpr uint32_t kMicroBodyNoModel = 0xFFFFFFFFu;

// ---- WIND PRIMITIVE ceilings and encoding (src/sim/windprim.h owns the
// system; these live here because they are the GPU LAYOUT, which is world.h's
// job — must match WIND_PRIM_* / WPRIM_* in assets/shaders/common.wgsl) ----
//
// Live primitives world-wide. Every one is summed at every wind sample inside
// the union AABB, so this is a per-sample cost as much as a memory one. 32 is
// far more than a scene needs and keeps the worst-case inner loop comparable
// to windAtQ's own ~150 integer ops.
constexpr uint32_t kWindPrimCap = 32;
// i32 words per resolved primitive: 3 x vec4<i32>.
constexpr uint32_t kWindPrimWords = 12;
// kWindPrimCap * kWindPrimWords, written as a LITERAL on purpose.
// scripts/check_invariants.py's `params` check parses a C++ array dimension as
// a bare identifier bound to an integer literal; an expression makes it fail to
// parse the field and SKIP the whole TickParams comparison silently — which is
// exactly the class of failure that check exists to catch. The static_assert
// below is what keeps the literal honest.
constexpr uint32_t kWindPrimScalars = 384;
static_assert(kWindPrimScalars == kWindPrimCap * kWindPrimWords,
              "kWindPrimScalars must equal kWindPrimCap * kWindPrimWords");
// Chunks a tick may WAKE across all primitives — the rule-2 budget, and the
// one number that decides whether a fan is free. 128 chunks is the scale of a
// small explosion, refreshed per tick; a primitive that does not fit is
// refused rather than silently trimmed.
constexpr uint32_t kWindWakeCap = 128;

// ---- WATER BODIES (docs/PLAN_water_master.md; src/sim/waterbody.h) --------
// Live still-water descriptors world-wide, and the rule-2 bound on the whole
// subsystem. Here rather than in waterbody.h for the wind-primitive reason:
// from M2 this sizes a TickParams array and a per-body state buffer, which
// makes it a GPU LAYOUT, and world.h is where a layout a shader must agree with
// is stated. `sim.waterBodyMaxCount` clamps against it, so LoadTuning can see it
// without taking a dependency on the water subsystem's own header.
//
// 64 is generous: a residency window is 51 m across and holds a handful of
// tarns. At the cap the smallest candidate is refused, and an unadopted body is
// simply simulated the way it is today.
constexpr uint32_t kWaterBodyCap = 64;
// i32 words per body in the TickParams descriptor array — the CPU->GPU half,
// rewritten every tick. Two vec4<i32> rows: the basin geometry the shave tests
// a cell against, plus the seed values the ledger adopts with. See
// WATERBODY_* in assets/shaders/sim_waterbody.wgsl for the field order; that
// file is the only decoder.
constexpr uint32_t kWaterBodyWords = 8;
// kWaterBodyCap * kWaterBodyWords, written as a LITERAL for the kWindPrimScalars
// reason: check_invariants.py's `params` check parses a C++ array dimension as a
// bare identifier bound to an integer literal, and an expression makes it SKIP
// the whole TickParams comparison silently. The static_assert keeps it honest.
constexpr uint32_t kWaterBodyScalars = 512;
static_assert(kWaterBodyScalars == kWaterBodyCap * kWaterBodyWords,
              "kWaterBodyScalars must equal kWaterBodyCap * kWaterBodyWords");
// Chunk-list entries a tick may declare across every live body, packed
// `(bodyIndex << 16) | chunkSlot`. The dispatch extent of the quiescence probe,
// the adoption reduce and the surface shave, and therefore the rule-2 bound on
// all three: a body whose footprint does not fit is not listed, which means
// "simulated the way it is today".
//
// 512 covered the harness's authored lake (r=68 -> ~69 chunk columns x 2
// layers) with room for a second body. M5 doubled it to 1024 because a basin
// someone has DUG IN proposes a SPLIT CHILD alongside it (see
// kWaterBodyChildren) and the child re-lists the same footprint under its own
// body index. It is a CEILING, not a per-tick cost: the list is empty at
// sim.waterBodyMode 0, holds only the bodies the CPU proposed, and the child is
// proposed only while the parent's curve is dirty — so an untouched lake pays
// exactly what it paid at M4.
constexpr uint32_t kWaterChunkCap = 1024;
// u32 words per body in the GPU-OWNED ledger buffer (world.waterBodyState).
// The level, the debit and everything derived from what the shave actually
// removed live here and not on the CPU, because the only honest source for
// "what was shaved" is a GPU atomic and reading it back would put fence
// retirement — i.e. scheduling — inside a voxel write's control path (rule 1).
// M3 widened it from 16 to 24: the hole record (component 6) and the drain's
// published emission are ledger state for the same reason the debit is — they
// are derived from a level the GPU owns, and the discharge law's single
// evaluation of `h` has to reach the op writer without a round trip through the
// CPU. The four attribution words plan §7 asked for BEFORE they were needed are
// still there and are still spare. M5 widened it again, to 27: the re-audit
// arm, its attribution tick, and the RESOLVED sweep level (WBS_REAUDIT /
// WBS_AUDITTICK / WBS_SWEEPY in common.wgsl).
constexpr uint32_t kWaterBodyStateWords = 27;
// DRAIN OP SLOTS PER BODY (component 6). The discharge is emitted through the
// existing spawnAppend seam, which reads a CPU-sized op stream — so the CPU
// RESERVES a contiguous block per body it proposes and the GPU fills it. This
// is the rule-2 bound on the jet: a hole can never emit more than this many
// eighths in one tick whatever the head, `sim.drainMaxEighthsPerTick` clamps
// against it, and the ledger debits by what was ACTUALLY written into the block
// (discipline 3.2) rather than by the analytic Q.
//
// 512 eighths/tick is 64 voxels/tick, ~2.9x the CA-only baseline's 177
// eighths/tick on `worldlake` (PLAN_water_master.md §1) — enough headroom for
// the milestone's throughput claim, and 16 KiB of upload per body per tick,
// which is inside the <1 MB/tick CPU->GPU budget with three orders to spare.
constexpr uint32_t kWaterDrainOpsPerBody = 512;

// ---- M5: the measured container curve + the split map (components 2/10) ----
//
// THE SWEEP'S FOUR OUTPUTS (plan §2, case 2) all live in the SAME buffer as the
// ledger, past the end of it, rather than in a buffer of their own. That is a
// deliberate refusal to add a binding: `waterBodyState` is already
// `array<atomic<i32>>` at simBGL_ 24, already an `A(WaterBodyState)` row in
// every water pass, and every accumulator the sweep needs (atomicAdd for the
// area count, atomicMin for the spill, atomicOr for the openness bitmap) is a
// sanctioned order-free atomic. A second buffer would have bought nothing and
// cost a binding, a pass-table resource and a barrier class.
//
//   [0, kWaterBodyCap * kWaterBodyStateWords)                the ledger
//   [kWaterCurveBase, + kWaterBodyCap * kWaterCurveWords)    per-body sweep
//   [kWaterSweepScratchBase, + kWaterSweepScratchWords)      shared scratch
//
// THE SPLIT GRID IS A DOWNSAMPLE, and that is the one approximation in M5.
// Connectivity is computed on a `kWaterSplitGrid`^2 grid laid over the basin's
// column AABB, one grid cell spanning `ceil(diameter / 48)` columns — 3 columns
// for the harness lake's 137. A grid cell is OPEN at a level if ANY of its
// columns is, which is the LIBERAL direction on purpose: it can only ever
// UNDER-split (report one pool where there are two), and under-splitting is the
// status quo — the CA handles it, exactly as it handles every basin this system
// declines. Over-splitting would strand water in a puddle nobody drains, which
// is the direction that is not safe. A partition thinner than 2*step-1 columns
// may therefore go unseen; that is documented rather than hidden, and
// `--gate waterbody` pass B builds a wall thick enough to be seen.
constexpr uint32_t kWaterSplitGrid = 48;
constexpr uint32_t kWaterSplitCells = 2304;   // kWaterSplitGrid^2
static_assert(kWaterSplitCells == kWaterSplitGrid * kWaterSplitGrid,
              "kWaterSplitCells must equal kWaterSplitGrid^2");
// Two bits per grid cell: the component index a body's footprint test compares
// against. 0..2 are real components, 3 is "blocked / no water here".
constexpr uint32_t kWaterSplitWords = 144;    // kWaterSplitCells * 2 / 32
// area(y) entries per body. One per world Y from the basin floor up; a basin
// deeper than this is truncated and says so (SW_TRUNC), which is a reported
// approximation and not a mass error — the table is a SCHEDULE (plan §3.2).
constexpr uint32_t kWaterCurveMaxY = 64;
// Header words before the area table. See SW_* in common.wgsl.
constexpr uint32_t kWaterSweepHeaderWords = 10;
constexpr uint32_t kWaterCurveWords = 218;    // 10 + 64 + 144
static_assert(kWaterCurveWords == kWaterSweepHeaderWords + kWaterCurveMaxY +
                                      kWaterSplitWords,
              "kWaterCurveWords must equal the header + area + split blocks");
// SHARED scratch, not per body: exactly one basin is swept per tick (the
// schedule is `basinId % N == tick % N`, plan §3.4), so the openness bitmap the
// label propagation reads has one writer and one reader per tick.
constexpr uint32_t kWaterSweepScratchWords = 128;
constexpr uint32_t kWaterCurveBase = kWaterBodyCap * kWaterBodyStateWords;  // 1664
constexpr uint32_t kWaterSweepScratchBase =
    kWaterCurveBase + kWaterBodyCap * kWaterCurveWords;
constexpr uint32_t kWaterBodyStateTotalWords =
    kWaterSweepScratchBase + kWaterSweepScratchWords;
// THE SCHEDULE PERIOD (plan §3.4). A basin re-derives on ticks where
// `slot % kWaterSweepPeriod == tick % kWaterSweepPeriod`, and one LEVEL of its
// column AABB per scheduled tick. So a full re-derive of a 26-deep bowl costs
// 26 scheduled ticks = 104 world ticks = 3.5 s, during which the table is
// stale. That is fine and it is the plan's own argument: water is slow, and the
// table is a schedule, so a stale one costs pace and never mass.
constexpr uint32_t kWaterSweepPeriod = 4;
// SPLIT CHILDREN a dug basin may propose. One, so a basin becomes at most two
// pools; a third component is left to the CA, which is the same safe
// degradation every refusal in this subsystem takes. Raising it costs a chunk
// list per child and nothing else.
constexpr uint32_t kWaterBodyChildren = 1;
// `TickParams.waterSweepLevel` sentinel: sweep the body's LIVE level rather
// than a cursor level.
//
// The active split map is only ever asked about at the level the shave is
// working, and during a drain that level moves faster than a cursor walking
// the whole basin can follow. But the live level is a GPU fact — it is what
// M2 moved off the CPU on purpose — so the CPU cannot name it. It names this
// sentinel instead and the LEDGER resolves it, publishing the answer into
// WBS_SWEEPY for the two sweep passes to read. One evaluation, two consumers,
// exactly as the discharge law publishes WBS_EMIT and WBS_JETV: if the sweep
// re-resolved it for itself it would disagree with the clear the ledger
// already did, on any tick the shave moved the level in between.
constexpr int32_t kWaterSweepLive = -0x7FFFFFFF;

// Largest radius / axial reach a primitive may declare, world cells (51 m).
// Load-bearing twice: it bounds the footprint the wake budget is spent on, and
// it bounds every intermediate in the integer field evaluation (see the
// overflow argument above windPrimAtQ in common.wgsl).
constexpr int32_t kWindPrimMaxExtent = 512;
// Largest strength, world cells/s (40 m/s). Caps the summed field at 32x that,
// which is what keeps the Q16.16 accumulator inside i32.
constexpr int32_t kWindPrimMaxSpeed = 400;
// TTL sentinel: a fan blows until something removes it.
constexpr uint32_t kWindPrimForever = 0xFFFFFFFFu;

// Primitive kinds — must match WPRIM_* in common.wgsl. Three shapes, chosen
// because they span what the requirements ask for: a directed jet (fans,
// gusts, wind walls), an omnidirectional push or pull (blast fronts, vacuums),
// and a swirl about an axis (tornado, whirlpool). Anything else is these
// composed, which is the point of making them summable.
constexpr uint32_t kWindPrimCone = 0;
constexpr uint32_t kWindPrimBurst = 1;
constexpr uint32_t kWindPrimVortex = 2;

// Primitive flags — must match WPRIM_F_* in common.wgsl.
//
// The MEDIUM MASK is carried from day one on purpose (RESEARCH_wind.md §8):
// the field core is medium-agnostic and an ocean current or a whirlpool is the
// same primitive with a different mask. Deciding it later would mean an
// op-format change after the format is already a save format.
constexpr uint32_t kWindPrimAir = 1u << 0;
constexpr uint32_t kWindPrimWater = 1u << 1;
// THE ENTRAINMENT LICENCE. Inside this primitive's footprint, settled powder
// whose authored friction the wind beats may be pulled loose. Off by default:
// it is the flag that costs chunk wakes, and a decorative gust has no business
// rearranging the terrain it blows past.
constexpr uint32_t kWindPrimEntrain = 1u << 2;

// ---- CURRENT PRIMITIVE ceilings and encoding (docs/PLAN_water_master.md
// component 8; src/sim/currentprim.h owns the system) ----
//
// Here rather than in currentprim.h for the wind-primitive reason: these are
// the GPU LAYOUT, and world.h is where a layout a shader must agree with is
// stated. Must match CURRENT_PRIM_* / CPRIM_* in assets/shaders/common.wgsl.
//
// 32 is the same cap wind carries, and for the same argument: every live
// primitive is summed at every sample inside the union AABB, so this is a
// per-sample cost as much as a memory one. A whirlpool per drain plus a stream
// per river reach in the residency window is a handful.
constexpr uint32_t kCurrentPrimCap = 32;
constexpr uint32_t kCurrentPrimWords = 12;   // 3 x vec4<i32>
// kCurrentPrimCap * kCurrentPrimWords, written as a LITERAL for exactly the
// reason kWindPrimScalars is — check_invariants.py's `params` check parses a
// C++ array dimension as a bare identifier bound to an integer literal, and an
// expression makes it SKIP the whole struct comparison silently.
constexpr uint32_t kCurrentPrimScalars = 384;
static_assert(kCurrentPrimScalars == kCurrentPrimCap * kCurrentPrimWords,
              "kCurrentPrimScalars must equal cap * words");
// Largest radius / axial reach, world cells. Bounds every intermediate in the
// integer evaluation (see the overflow argument above currentPrimEvalQ).
constexpr int32_t kCurrentPrimMaxExtent = 512;
// Largest core speed, world cells/s (20 m/s). A whirlpool's throat at 20 m/s is
// already faster than anything in this engine swims; the cap is what keeps the
// Q16.16 accumulator inside i32 across a full list.
constexpr int32_t kCurrentPrimMaxSpeed = 200;

// Primitive kinds — must match CPRIM_* in common.wgsl.
constexpr uint32_t kCurrentPrimSink = 0;
constexpr uint32_t kCurrentPrimSource = 1;
constexpr uint32_t kCurrentPrimVortex = 2;
constexpr uint32_t kCurrentPrimStream = 3;
// THE SIM LICENCE. Set only on primitives whose parameters are a pure function
// of the tick input stream; currentAtQ skips every primitive without it, so a
// primitive seeded from anything the CPU learned asynchronously can reach the
// renderer and the player and never the hashed world. See the authority note
// over currentAtQ in common.wgsl.
constexpr uint32_t kCurrentPrimSim = 1u << 0;

// Impact-ripple ring size (component 9) — must match WAVE_IMPACT_CAP in
// common.wgsl. Sixteen is a second and a half of splashes at a plausible rate,
// and the ring is what makes "a ripple is the memory of an event" bounded by
// construction instead of a growing list.
constexpr uint32_t kWaveImpactCap = 16;

// Must match TickParams in common.wgsl.
struct TickParams {
  uint32_t tick;
  uint32_t seed;
  uint32_t opsCount;
  uint32_t hashEnable;
  uint32_t expCount;   // explosion ops this tick
  uint32_t page;       // particle read page (0/1)
  uint32_t cellCount;  // exact-cell ops this tick
  uint32_t genCount = 0;   // chunks in genList (worldgen streaming dispatch)
  int32_t origin[3] = {0, 0, 0};  // residency window origin, chunk units
  uint32_t spawnCount = 0;  // CPU particle spawns this tick (debris shatter)
  uint32_t farCount = 0;    // far-field fill entries in farList this tick
  // Integer day phase (0..kDayPhaseMask) for THIS tick — see DayPhaseForTick.
  // Feeds voxel state through the daylight-gated reactions, so it is
  // determinism-critical: derived from `tick` only, never from frame timing.
  uint32_t dayPhase = 0;
  // MLS-MPM fluid: the disturbance-excite switch (sim.fluidExciteMode read
  // CPU-side each tick, the dayPhase precedent — tick input stream, so
  // replays and determinism gates capture it) and this tick's spawn-op count.
  // The live count is GPU-owned (fluidArgsStage[7]); see the fluid block
  // above kFluidCap.
  uint32_t fluidExciteEnable = 0;
  uint32_t fluidSpawnCount = 0;
  // Material id each MPM species splashes micro droplets as (0 = species never
  // poured -> no droplets). Recorded from the pour's brush material by the main
  // loop; vec4<u32> on the WGSL side, so keep this 16-byte aligned.
  uint32_t fluidSplashMat[4] = {0, 0, 0, 0};
  // WORLD chunk coord of the 3x3x3 CPU-mirror corner (World::MirrorBaseFor of
  // the player chunk — the SAME clamp EncodeReadbacks uses, or the fold and
  // the voxel mirror would describe different cubes). The seam's mirrorFold
  // kernel packs excited-fluid occupancy for exactly these 27 chunks so
  // swimming sees particles the way it sees fullness voxels. vec3<i32> + pad
  // on the WGSL side.
  int32_t mirrorBase[3] = {0, 0, 0};
  // Fluid-lab worldgen mode (kLabSlabY above): 1 = genColumn generates the
  // flat slab, 0 = normal terrain. Rides the tick input stream like dayPhase
  // so every worldgen consumer (full gen, streamed genList, far cascades)
  // sees one flag. Always 0 outside --lab/--fluid-bench; was the padMb pad
  // word, so the layout (and the labMode=0 hash) is unchanged.
  uint32_t labMode = 0;
  // ---- wind, the SIM's copy (docs/RESEARCH_wind.md §4.2 — must match
  // TickParams in common.wgsl) ----
  // The same three weather numbers RenderParams carries, quantised to Q16.16 so
  // the kernels that read them stay integer (rule 1), and produced by the SAME
  // function on the same tick — WindWeather (sim/wind.h) is the only author of
  // wind weather anywhere in the engine. Two authors would mean grass and smoke
  // blowing different ways in one frame, which reads as a shader bug.
  //
  // On the tick input stream for the dayPhase reason: a replay reproduces the
  // stream and the twice-run gate compares it, so anything the world hash can
  // see has to arrive this way rather than be recomputed GPU-side.
  int32_t windDirQ[2] = {0, 65536};  // unit XZ pointing DOWNWIND, Q16.16
  int32_t windSpeedQ = 0;            // mean speed, Q16.16 world cells/s
  int32_t windGustQ = 0;             // gust amplitude, Q16.16 world cells/s
  // The gate: sim.windMode, read CPU-side per tick (the fluidExciteMode
  // precedent, so a per-gate SetCurrentTuning moves it with no rebuild).
  uint32_t windMode = 1;
  // DEV FORCE MULTIPLIERS, Q8 (256 = 1.0x), one per TIER — see kWindScaleOne.
  // They ride the tick stream rather than being const-folded into the shaders
  // like the rest of the wind coupling, which is the whole point: they are
  // dev-panel sliders and a knob you have to press F5 to see is a knob nobody
  // drags. Deterministic all the same (integers, on the input stream), and at
  // the 1.0x default every consumer takes an exact-identity early-out, so the
  // pinned hash cannot move until someone moves a slider.
  int32_t windGasScaleQ = 256;    // the CA voxel tier: drift-bias probability
  int32_t windPartScaleQ = 256;   // the particle tier: debris, spray, MPM nodes
  // RAMP REFERENCE for the drag-tier RATE (sim.windDragRef), Q16.16 world
  // cells/s. The wind speed at which sim.windDrag applies in full; below it the
  // rate scales linearly with the local wind, so CALM AIR IS BALLISTIC. See
  // windDragRampQ in common.wgsl — this is the difference between a drag law
  // and a universal air resistance, and the fixed rate it replaces made every
  // particle in the world fall at a seventh of its old speed the moment wind
  // switched on. On the tick stream with the two multipliers, and for the same
  // reason: it exists to be dragged.
  int32_t windDragRefQ = (int32_t)(40.0f / kVoxelMeters * 65536.0f + 0.5f);

  // ---- WIND PRIMITIVES (docs/RESEARCH_wind.md §4.3; src/sim/windprim.h) ----
  //
  // The player-facing half of the wind system: fans, spell gusts, tornadoes —
  // a bounded list of parametric objects evaluated analytically at any sample
  // point, exactly the way a point light is. Everything above this line is the
  // ambient field, which is scenery; this is the part that is a tool.
  //
  // WHY THEY RIDE THE UNIFORM AND NOT A STORAGE BUFFER. The list is at most
  // 32 x 48 bytes of per-tick CPU-authored configuration, which is what a
  // uniform is for. Putting it in a storage buffer would mean a new binding in
  // BOTH group-0 layouts (common.wgsl is prepended to every shader, so one
  // identifier cannot carry two binding numbers), a new pass_table row set, a
  // new barrier class, and the same again on the render side — for 1.5 KiB
  // that changes once a tick. As it is, the whole feature costs no new
  // binding, no new barrier and no new dispatch except the wake below.
  //
  // ZERO PRIMITIVES IS AN EXACT IDENTITY. windPrimCount == 0 makes every
  // consumer take an untouched early-out, so the pinned world hash cannot move
  // until someone actually puts a fan in the world. `windPrimLo > windPrimHi`
  // is the empty-box convention (the fluid render AABB's), and it is the
  // whole-loop reject for samples outside every footprint.
  uint32_t windPrimCount = 0;
  // Chunk slots this tick's primitives want awake — see the wake note in
  // windprim.h. Consumed by sim_mutate.wgsl's `windWake` entry point, which is
  // the ONLY thing in the engine that dirty-marks a chunk without writing a
  // voxel, and which exists so that entrainment's writes land in chunks the
  // CPU declared before the command buffer was built.
  uint32_t windWakeCount = 0;
  uint32_t pad_wp0 = 0, pad_wp1 = 0;
  int32_t windPrimLo[3] = {1, 1, 1};   // union AABB of every live primitive,
  int32_t pad_wp2 = 0;                 // inclusive world cells (lo > hi = none)
  int32_t windPrimHi[3] = {0, 0, 0};
  int32_t pad_wp3 = 0;
  // kWindPrimCap primitives x kWindPrimWords i32 words. Declared WGSL-side as
  // array<vec4<i32>, 3 * WIND_PRIM_CAP>, which is the same bytes: std140
  // strides a uniform array to 16 B, so three rows per primitive is the
  // densest legal packing and windPrimLoad is the only decoder.
  int32_t windPrims[kWindPrimScalars] = {};
  uint32_t windWake[kWindWakeCap] = {};
  uint32_t vizActive = 0;   // debug: CA writes per-voxel activity bits when nonzero

  // ---- WATER BODIES (docs/PLAN_water_master.md M2; src/sim/waterbody.h) ----
  //
  // THE WHOLE CPU->GPU SURFACE of the drain ledger, and it rides the tick input
  // stream for the dayPhase/windMode reason: a replay reproduces the stream and
  // the twice-run determinism gate compares it, so anything the world hash can
  // see arrives this way rather than being recomputed from something the CPU
  // merely happened to know.
  //
  // It is also the fix for the M1 hazard (waterbody.h's note, DESIGN.md §5b.4).
  // At M1 the jurisdiction ladder's quiescence term read World::Snap(), the
  // ASYNC readback, which arrives on a schedule set by fence retirement. That
  // was harmless while the verdict was advisory; from M2 adoption gates a voxel
  // write, so the verdict had to stop being a function of when the CPU noticed.
  // What the CPU sends now is only what is a pure function of (seed, window,
  // tuning): basin geometry, the chunk list, the thresholds. QUIESCENCE AND
  // ADOPTION ARE DECIDED ON THE GPU, from the hashed world, in
  // sim_waterbody.wgsl — see the note over `waterBodyState` below.
  //
  // MODE 0 IS AN EXACT IDENTITY. `waterBodyMode` 0 means the CPU writes count 0
  // here, every pass row's condition is false and nothing is recorded, so the
  // pinned world hash cannot move. Same shape as windPrimCount == 0.
  uint32_t waterBodyMode = 0;
  uint32_t waterBodyCount = 0;    // live descriptors in `waterBodies`
  uint32_t waterChunkCount = 0;   // entries in `waterChunks`
  // TEST-ONLY DRAIN SOURCE, eighths per tick per adopted body. M2 has no
  // discharge law yet (component 6 is M3), so this is what gives the ledger
  // something to be exact about, and it is what `--gate waterbody` pass A
  // conserves across. Integer, on the tick stream, 0 by default — a shipped
  // world never sees it.
  int32_t waterTestDrain = 0;
  // The jurisdiction thresholds the GPU ladder compares against, forwarded from
  // tuning so a per-gate SetCurrentTuning moves them with no pipeline rebuild
  // (the windMode precedent).
  int32_t waterQuietTicks = 0;    // sim.waterBodyQuietTicks
  int32_t waterMinVolume = 0;     // sim.waterBodyMinVolume, EIGHTHS
  // ---- M3: the discharge law (component 6) and the local excite (7) --------
  //
  // THE OP-BLOCK RESERVATION. `spawnAppend` reads a CPU-sized stream, so the
  // discharge cannot allocate its own slots: the CPU reserves
  // `waterDrainBodies * kWaterDrainOpsPerBody` ops starting at
  // `waterDrainSpawnBase` (immediately after this tick's real CPU pours) and
  // sim_waterbody.wgsl's wbDrain fills every one of them — with a live op while
  // the discharge lasts and a DEAD op (mat 0) after it, because a slot the pass
  // skipped would still be counted live and would hold whatever two ticks ago
  // left there. Both numbers are pure functions of (seed, window, tuning), so
  // they ride the tick stream like every other water field.
  uint32_t waterDrainSpawnBase = 0;
  uint32_t waterDrainBodies = 0;
  // sim.drainMaxEighthsPerTick — the per-hole per-tick emission bound (rule 2,
  // and plan §6's second named trap). Clamped to kWaterDrainOpsPerBody CPU-side
  // so the ledger can never publish an emission the op block cannot hold.
  int32_t waterDrainMax = 0;
  // sim.drainExciteRadius — component 7's v1 knob, world cells. 0 disables the
  // shell entirely and is an exact identity (no cell can satisfy the trigger).
  int32_t waterExciteRadius = 0;
  // ---- M5: THE SCHEDULED RE-DERIVE (components 2 case 2 + 10) -------------
  //
  // Which body's container curve is being re-derived this tick, and at which
  // world Y. BOTH are pure functions of the tick: the slot is picked by
  // `slot % kWaterSweepPeriod == tick % kWaterSweepPeriod` and the level cycles
  // the basin's own Y span, so a re-derive can never happen "when the CPU got
  // around to it" (plan §3.4, and the whole subject of `--gate waterbody` pass
  // F). `kWaterBodyCap` means NOTHING IS SCHEDULED, which is the state of every
  // lake nobody has dug into and is what makes C_WATERSWEEP false.
  uint32_t waterSweepSlot = kWaterBodyCap;   // kWaterBodyCap == "none"
  int32_t waterSweepLevel = 0;
  // THREE pad words, and the count is arithmetic rather than taste: `windWake`
  // ends 16-byte aligned, `vizActive` puts us 4 B past that, and std140 will
  // align `waterBodies` (an array of vec4) to 16. So the header between them
  // must be 4 + 15*4 = 64 B or the two structs disagree — and they disagree
  // SILENTLY, because C++ packs an int32_t array at 4-byte alignment while the
  // shader's std140 rounds up to the next vec4, which slides every descriptor
  // by two scalars. check_invariants.py compares the shapes and is the only
  // thing that catches it; see vizActive's own note above for the last time.
  // M5 spent two of the five on the sweep schedule above; the total is
  // unchanged, which is why widening TickParams here moved nothing.
  uint32_t padWb2 = 0;
  uint32_t padWb3 = 0;
  uint32_t padWb4 = 0;
  // kWaterBodyCap bodies x kWaterBodyWords i32 words, declared WGSL-side as
  // array<vec4<i32>, 128> — the same bytes, since std140 strides a uniform
  // array to 16 B. sim_waterbody.wgsl's wbGeom/wbSeed are the only decoders.
  int32_t waterBodies[kWaterBodyScalars] = {};
  // (bodyIndex << 16) | chunkSlot, four to a std140 row.
  uint32_t waterChunks[kWaterChunkCap] = {};

  // ---- THE CURRENT FIELD, the SIM's copy (plan component 8;
  // src/sim/currentprim.h; must match TickParams in common.wgsl) ----
  //
  // A structural clone of the wind primitive block above — same cap, same
  // three-row packing, same union AABB, same float/integer transcription pair.
  // It rides the uniform for the same reasons: no new binding, no new barrier,
  // no new dispatch, and a count of zero is an exact identity.
  //
  // MODE 0 IS THE SHIPPING DEFAULT AND IS BIT-IDENTICAL. currentAtQ returns
  // zero on `currentMode == 0` before it reads anything else, so no sim kernel
  // can see this block and the pinned world hash cannot move. The RENDER arm
  // ships on regardless (RenderParams::currentRenderOn), because a renderer
  // cannot write a voxel — that split is what makes M4 visible without moving
  // the hash. See DESIGN.md §9c.
  uint32_t currentMode = 0;
  uint32_t currentPrimCount = 0;
  uint32_t padCp0 = 0, padCp1 = 0;
  int32_t currentPrimLo[3] = {1, 1, 1};   // union AABB, inclusive world cells
  int32_t padCp2 = 0;                     // (lo > hi = no primitives)
  int32_t currentPrimHi[3] = {0, 0, 0};
  int32_t padCp3 = 0;
  int32_t currentPrims[kCurrentPrimScalars] = {};
};

// Q8 unit for the two dev multipliers above — must match WINDQ_SCALE_ONE in
// assets/shaders/common.wgsl. Exactly 256 is the shipping value AND the
// identity guard: every consumer compares against it and takes the untouched
// path, so "the slider is at 1x" and "the sim is bit-identical to the pinned
// hash" are the same statement rather than two that have to be argued.
constexpr int32_t kWindScaleOne = 256;
// 16x. High enough that the gas slider reaches certainty (every moving voxel
// goes downwind) and debris is thrown at genuinely absurd speeds, which is what
// a "what does drastic look like" control is for; bounded so the Q8 arithmetic
// in the kernels stays inside i32 with room to spare.
constexpr int32_t kWindScaleMax = 16 * kWindScaleOne;

// sim.windMode ladder — must match WIND_MODE_* in assets/shaders/common.wgsl.
//
// An ordered ladder rather than independent bools because the steps genuinely
// nest, and because each one further out is a bigger promise about rule 2:
//   0 OFF     — no sim kernel evaluates the field at all. The pinned world hash
//               cannot move. This is the default and the shipping value until
//               phase 4's dedicated rebaseline commit.
//   1 DRIFT   — particles, MPM spray and the CA's drift bias are live. Rule-2
//               clean by construction: every one of those only ever runs on
//               matter that was ALREADY moving in an already-awake chunk, so
//               the ambient field still cannot wake anything (invariant 3).
//   2 ENTRAIN — settled powder can be pulled loose by a wind above its friction
//               threshold. NOT rule-2 clean on its own: an exposed dune, once
//               woken by anything, keeps re-marking itself for as long as the
//               wind blows. It needs phase 2's bounded primitive footprints and
//               a storm-wake budget (research doc §4.5, §8) before it can be a
//               default. It is here so the saltation rule can be looked at.
constexpr uint32_t kWindModeOff = 0;
constexpr uint32_t kWindModeDrift = 1;
constexpr uint32_t kWindModeEntrain = 2;

// Must match struct Particle in common.wgsl (32 bytes). CPU-authored particle
// spawns: debris-body fragments re-entering the world as ballistic voxels
// (DESIGN.md §7 shatter). Appended to the live page by sim_particle.wgsl
// `spawn`; part of the per-tick input stream like BrushOp/CellOp.
struct ParticleSpawn {
  int32_t px, py, pz;   // position, fixed 24.8 voxels
  int32_t vx, vy, vz;   // velocity, fixed 24.8 voxels/tick
  uint32_t payload;     // bits 0..11 material, 12..15 state
  uint32_t flags;       // PFLAG_ALIVE is forced on the GPU; micro bits survive
};
constexpr uint32_t kMaxParticleSpawnsPerTick = 4096;

// Particle flag bits — must match PFLAG_* / PMICRO_* in common.wgsl.
//
// A MICRO particle is sub-voxel spray (blood droplets, blast grit). It never
// reinserts into the grid: on contact it applies its material's authored stain
// and dies, and it dies on its own after `life` ticks regardless. See the long
// note in common.wgsl for why both properties are forced rather than optional.
constexpr uint32_t kPFlagAlive = 1u;
constexpr uint32_t kPFlagMicro = 4u;
constexpr uint32_t kPMicroScaleShift = 3, kPMicroScaleMask = 3u;
constexpr uint32_t kPMicroLifeShift = 5, kPMicroLifeMask = 0xFFu;

// Packs the micro scale + lifetime fields. `scale` is micro voxels per world
// voxel and must be one of 2/3/4/6 (the only values the 2-bit field encodes);
// anything else falls back to 4. `lifeTicks` saturates at 255 — the field is 8
// bits, and silently wrapping would make a long-lived droplet die instantly.
inline uint32_t ParticleMicroBits(int scale, int lifeTicks) {
  uint32_t idx = 2;  // default: 4 micro voxels per world voxel
  if (scale == 2) idx = 0;
  else if (scale == 3) idx = 1;
  else if (scale == 4) idx = 2;
  else if (scale == 6) idx = 3;
  uint32_t life = (uint32_t)(lifeTicks < 0 ? 0 : (lifeTicks > 255 ? 255 : lifeTicks));
  return (idx << kPMicroScaleShift) | (life << kPMicroLifeShift);
}

// Must match RenderParams in common.wgsl (std140-ish: vec3 + pad pairs).
struct RenderParams {
  float camPos[3];
  float tanHalfFov;
  float camRight[3];
  float aspect;
  float camUp[3];
  float time;
  float camFwd[3];
  uint32_t flags;
  float sunDir[3];
  float fogDensity = 0.0128f;  // per-meter; pinned to the far-field extent
  int32_t origin[3] = {0, 0, 0};  // residency window origin, chunk units
  // Render target HEIGHT in pixels. The water shader damps its ripple bands
  // against the angular size of one pixel (tanHalfFov*2/viewPx), so this has
  // to be the real target height or the water's apparent choppiness changes
  // with window size.
  float viewPx = 1080.0f;

  // ---- day/night (one std140 row) ----
  // moonDir is the antipode of the sun tilted by the lunar inclination, so the
  // moon is not simply "the sun at night". dayT is the phase as 0..1 for the
  // shader's smooth blends; the SIM uses the integer phase in TickParams
  // instead, and the two agree because both come from the same tick.
  float moonDir[3] = {0.0f, -1.0f, 0.0f};
  float dayT = 0.5f;
  // sunAboveHorizon: smoothed 0..1 daylight weight (drives ambient, fog tint
  // and star fade). moonPhase: 0=new, 0.5=full, 1=new again — drives both the
  // lit fraction of the disc and how much moonlight the world gets.
  float sunUp = 1.0f;
  float moonPhase = 0.5f;
  float starRot = 0.0f;   // radians, stars wheel about the celestial pole
  // ---- static micro-detail (render-only — sim/microvox.h) ----
  // The raymarcher needs the tick to pick a flipbook frame and the world seed
  // to key per-cell yaw/jitter. Both are already integers the sim owns; passing
  // them through RenderParams rather than reading TickParams keeps the render
  // bind group free of any sim uniform (which is what makes it obvious that the
  // arrow only ever points sim -> render).
  //
  // The tick is used ONLY as an animation clock here. It is deliberately not
  // wall time: a flipbook driven by frame timing would run at a different rate
  // on a different machine, and a replay would not reproduce the frame the
  // player saw.
  uint32_t tick = 0;
  // `seed` completes the row that starts at sunUp; the three pads then round
  // the struct out to a whole 16-byte std140 row. WebGPU pads a uniform binding
  // up to a multiple of 16 and validates the BOUND SIZE against that, so a
  // struct that ends mid-row is rejected outright ("bound with size 140 ...
  // requires at least 144"). Keep the total a multiple of 4 scalars.
  uint32_t seed = 0;
  // Live MPM fluid particle count. 0 skips the fluid surface march entirely
  // (raymarch.wgsl), so a world with no fluid pays nothing for the feature.
  uint32_t fluidCount = 0;

  // ---- the second moon + eclipses (celestial overhaul) ----
  // Moon B rides its own Keplerian orbit with a 9-day synodic period against
  // moon A's 8 — coprime, so the pair of phases does not repeat for 72 days.
  // Everything here is a plain consequence of sim/celestial.cpp's geometry;
  // none of it is authored per-frame.
  //
  // These two u32s complete the row that fluidCount starts, so the struct
  // still ends on a whole std140 row. The three pads it used to carry are gone
  // — spend padding before adding more (world.h's note on the bound-size
  // validation above).
  uint32_t eclipseBody = 0;   // 0 none, 1 moon A in front of the sun, 2 moon B
  uint32_t pad_dn0 = 0;

  float moon2Dir[3] = {0.0f, -1.0f, 0.0f};
  float moon2Phase = 0.5f;
  // Apparent angular radii, RADIANS, modulated by orbital distance — a moon
  // at perigee is genuinely bigger. The tuner authors the base radius; this is
  // what the shader must draw with, so the discs and the eclipse test can
  // never disagree about how big a moon is.
  float moonAngRadius = 0.03f;
  float moon2AngRadius = 0.019f;
  // Signed lit-limb orientation, +1 or -1. Without it a waxing and a waning
  // crescent are the same picture and the terminator flips as the moon passes
  // full.
  float moonPhaseSign = 1.0f;
  float moon2PhaseSign = 1.0f;

  // Fraction of the SUN'S AREA currently occulted (circle-circle lens area),
  // 0 = clear sky. Dims the disc, the sky and the key light together, so a
  // partial eclipse is a partial dimming rather than a switch.
  float solarEclipse = 0.0f;
  // Fraction of moon B's disc hidden behind moon A. Render-only.
  float lunarEclipse = 0.0f;
  float pad_dn1 = 0.0f, pad_dn2 = 0.0f;

  // The CELESTIAL POLE in the local horizon frame — the axis the starfield
  // wheels about. It is an OUTPUT of latitude (the pole sits at elevation =
  // latitude, due north), which is why there is no knob for it: the stars and
  // the sun have to agree about where the axis is, and they only do if both
  // read the same latitude.
  //
  // DO NOT DELETE THIS ROW WITHOUT DELETING IT FROM common.wgsl. It was
  // removed once (ec764e8, "the shader derives it from latitude") while
  // raymarch.wgsl went on reading R.poleDir, so the shader read 16 bytes past
  // the end of what WriteRenderParams uploads. Robust buffer access returns
  // zero, normalize(vec3f(0)) is NaN, and both starField() and nightGlow()
  // rotate about that axis — so the NaN spread across the entire night sky and
  // skyAirglow's max(c, 0.0) collapsed it to pure black. Stars, Milky Way,
  // nebulae and aurora all vanished at once, silently, with no validation
  // error: an under-sized uniform binding is not a Vulkan error, it is just
  // zeros. check_invariants.py's `params` check now compares the two structs
  // field by field so this cannot recur.
  //
  // Starts its OWN std140 row: a vec3 aligns to 16 bytes, so it cannot begin
  // partway through the row solarEclipse opens. The two pads above are what
  // close that row, and the one below closes this one.
  float poleDir[3] = {0.0f, 1.0f, 0.0f};
  float pad_dn3 = 0.0f;

  // ---- MPM fluid render bounds (PLAN_fluid_overhaul.md §7 item 5) ---------
  // Inclusive world-VOXEL AABB of everything the fluid surface march can
  // possibly hit this frame. A ray that misses this box skips the march
  // wholesale (that is what makes off-screen fluid free), and a ray that hits
  // it marches only the [enter, exit] span instead of the whole view ray.
  //
  // DERIVED DATA, RENDER ONLY. It is unioned from the latest SNAPSHOT's active
  // fluid block list — which is a few ticks latent — plus the CPU-known recent
  // spawn positions, and then dilated by kFluidRenderPadVox. The dilation is
  // what makes the latency safe: a particle moves at most FLUID_VMAX
  // (2.7 cells) per tick, and `fluidChunkActive` in raymarch.wgsl reaches one
  // chunk past an active block, so the pad covers both with room to spare.
  //
  // EMPTY BOX CONVENTION: fluidLo > fluidHi on any axis means "no fluid
  // anywhere" and every ray misses. That is the post-settle idle state, and it
  // is why `pool` costs nothing once the water is voxels again — the monotone
  // CPU fluidCount does NOT decay, so this box is the only thing that can
  // switch the march off. Before the first snapshot arrives the box is the
  // whole residency window (conservative — never clip real water).
  int32_t fluidLo[3] = {0, 0, 0};
  int32_t pad_fb0 = 0;
  int32_t fluidHi[3] = {-1, -1, -1};
  int32_t pad_fb1 = 0;

  // ---- wind (docs/RESEARCH_wind.md §4.2 — must match RenderParams in
  // common.wgsl) ----
  // The one part of the wind field that is not a compile-time TUNE_* constant.
  // Everything else about the field (gust wavelength, gust rate, the altitude
  // ramp) is authored and const-folded into the shader; the WEATHER VECTOR
  // evolves chaotically over minutes, so it has to ride a per-frame uniform.
  //
  // These are OUTPUTS of WindWeather() (sim/wind.h), which is the only author
  // of them. That matters because the same function will fill the TickParams
  // copy in phase 4: if the renderer and the CA ever computed their own
  // weather, grass and smoke would blow different ways in the same frame.
  //
  // Units are world CELLS PER SECOND (m/s x 10 at kVoxelMeters 0.10) — the
  // conversion from the m/s knobs happens once, on the CPU, in that function.
  // One whole std140 row: two floats of direction plus two scalars.
  float windDir[2] = {0.0f, 1.0f};  // unit XZ, pointing DOWNWIND
  float windSpeed = 0.0f;           // mean speed, cells/s
  float windGust = 0.0f;            // gust band amplitude, cells/s

  // ---- wind primitives, the RENDER copy (docs/RESEARCH_wind.md §4.3) ------
  // The SAME resolved list TickParams carries, in the same encoding, written
  // from the same WindPrimSystem in the same frame. It is duplicated rather
  // than shared because the sim and the render bind groups deliberately have
  // no buffer in common (the arrow only ever points sim -> render), and 1.5 KiB
  // a frame is a cheaper price than a shared binding.
  //
  // THIS IS WHY THE GRASS LEANS IN A FAN'S BLAST without anyone wiring grass
  // to fans: raymarch.wgsl's sway samples windAt(), windAt() sums the
  // primitives, and the debug overlay samples the identical function. A
  // visualiser or a foliage path with its own idea of where the gusts are
  // would be a picture of a different wind (invariant 2).
  //
  // No fluidLo-style pad games needed: windGust ends a whole row, and the
  // array's own 16-byte stride starts the next one.
  uint32_t windPrimCount = 0;
  uint32_t pad_wpr0 = 0, pad_wpr1 = 0, pad_wpr2 = 0;
  int32_t windPrimLo[3] = {1, 1, 1};
  int32_t pad_wpr3 = 0;
  int32_t windPrimHi[3] = {0, 0, 0};
  int32_t pad_wpr4 = 0;
  int32_t windPrims[kWindPrimScalars] = {};

  // ---- the current field, the RENDER copy (plan component 8) -------------
  // The SAME resolved list TickParams carries, from the same
  // CurrentPrimSystem in the same frame — the windPrims argument exactly, and
  // it is what makes the surface waves drift with the flow, the arrow overlay
  // agree with the player's drift, and the foam sit on real convergence lines.
  //
  // `currentRenderOn` is its OWN gate rather than a copy of `currentMode`,
  // and the asymmetry is the milestone: the render arm ships ON and the sim
  // arm ships OFF, because a render field cannot write a voxel and a sim field
  // moves the pinned hash.
  uint32_t currentRenderOn = 0;
  uint32_t currentPrimCount = 0;
  uint32_t pad_cpr0 = 0, pad_cpr1 = 0;
  int32_t currentPrimLo[3] = {1, 1, 1};
  int32_t pad_cpr2 = 0;
  int32_t currentPrimHi[3] = {0, 0, 0};
  int32_t pad_cpr3 = 0;
  int32_t currentPrims[kCurrentPrimScalars] = {};
  // ---- component 9: the impact-ripple ring -------------------------------
  // (x, z) world voxels, `t0` the R.time value the impact landed at, `amp` its
  // strength in metres of initial crest. amp <= 0 is a dead slot. Bounded by
  // construction — see kWaveImpactCap.
  uint32_t waveImpactCount = 0;
  uint32_t pad_wi0 = 0, pad_wi1 = 0, pad_wi2 = 0;
  float waveImpacts[kWaveImpactCap * 4] = {};
};
static_assert(sizeof(RenderParams) % 16 == 0,
              "RenderParams must be a whole number of std140 rows");
// Dilation applied to the fluid render AABB, world voxels. Two chunks: one for
// the snapshot's readback latency (>= 5 ticks of travel at the CFL ceiling),
// one for `fluidChunkActive`'s face-neighbour reach.
constexpr int kFluidRenderPadVox = 2 * (int)kChunk;

// ---- far-field cascades (render-only LOD — DESIGN.md §9,
// docs/PLAN_far_field_cascades.md) ----
// kFarLevels nested toroidal 256^3 volumes around the residency window; level
// k (1-based) cells are 2^k fine voxels, so each level doubles view distance
// at constant memory. Derived data: filled from worldgen on the GPU, never
// read by the sim, excluded from the world hash — determinism rule #1 is
// untouched by construction.
// The far field lives on its OWN kFarN^3 grid, decoupled from the residency
// window (since the 512^3 window): tying cascade storage to kWorldN would have
// made each level 128 MiB. Level k (1-based) cells span 2^(k + kFarShiftBase)
// fine voxels, with kFarShiftBase = log2(kWorldN / kFarN) — chosen so level
// k's box edge is always 2^k WINDOW edges (half-extent = 2^k window radii)
// whatever kWorldN is. All cascade distances therefore scale WITH the window.
//
// ---- kFarN IS THE ONLY KNOB THAT CHANGES FAR-FIELD DETAIL ----
// A level's cell size and its box extent are rigidly coupled: level k holds
// kFarN^3 cells of 2^(k + kFarShiftBase) fine voxels, so the count of cells
// across a level's half-extent is the CONSTANT kFarN/2, whatever k is. A ray
// at distance d is served by the level whose box just contains d, so the cell
// serving it is always ~d / (kFarN/2) — and the projected size of that cell,
//     px_per_cell = (d / (kFarN/2)) / (2 d tan(fov/2) / H)
//                 = H / (kFarN * tan(fov/2)),
// has the distance cancel out. The far field renders at a CONSTANT angular
// resolution everywhere, fixed by kFarN alone.
//
// The corollary cost real time to learn, so state it plainly: changing
// kFarShiftBase or kFarLevels CANNOT change how detailed the far field looks.
// Shifting the base one step finer and adding a level renumbers the cascade
// (level 1 becomes what level 2 was) but leaves the cell size serving any
// given distance bit-identical — measured: a 9-level/finer-base build differed
// from the 8-level build by 26 pixels out of 2,073,600 in a view that is 50%
// cascade. Those two constants trade horizon distance against near coverage;
// only kFarN trades memory against detail.
//
// At kFarN=256 and 1080p/70deg that constant was ~6 px per cell, which was the
// LOD's real weakness: every cascade cell is a visible 6-pixel block, so a
// distant structure is quantised ~6x coarser than the screen can resolve and
// visibly restructures as it crosses into the residency window. 512 halves
// that to ~3 px, for 8x the per-level memory (kFarN^3).
//
// 512 SHIPS as of 2026-08-29, and it was paid for with VRAM rather than by
// dropping levels — see kFarLevels for why that trade went the other way than
// the comment there used to assume. Measured with SANDVOX_GPUMEM=1: farVox
// 128.00 -> 1024.00 MiB, World::Init total 793.72 -> 1690.59 MiB.
//
// This matters MOST for authored content, which is the thing the cascades
// serve worst: a cascade cell is POINT-SAMPLED at the fine voxel in its centre
// (see farSurfaceMat in worldgen.wgsl), so a structure is representable at
// level k only if its extent reaches a whole cell. Halving the cell size
// halves the size of the smallest building that survives the horizon.
constexpr uint32_t kFarN = 512;
constexpr uint32_t kFarNChunk = kFarN / kChunk;  // 32
constexpr uint32_t kFarNumChunks = kFarNChunk * kFarNChunk * kFarNChunk;
// Fill-queue entries pack (level-1) above a chunk SLOT index. The shift must
// be wide enough for kFarNumChunks slots — it was hardcoded to 12 (exactly
// right for the 4096 slots of kFarN=256, and silently wrong for anything
// bigger: at kFarN=512 a slot index runs to 32767 and would overflow into the
// level field, filling the wrong cascade level). Derived so it tracks kFarN.
constexpr uint32_t kFarSlotShift = [] {
  uint32_t s = 0;
  while ((1u << s) < kFarNumChunks) s++;
  return s;
}();
constexpr uint32_t kFarSlotMask = (1u << kFarSlotShift) - 1u;
constexpr uint32_t kFarVox = kFarN * kFarN * kFarN;  // cells per level
// kFarShiftAlign = log2(kWorldN / kFarN): the shift that makes a level's box
// edge equal the WINDOW edge, so level k's box is 2^k window edges across.
// kFarShiftBase sits here; moving it renumbers the cascade without changing
// how detailed any distance looks (see the kFarN note above).
constexpr uint32_t kFarShiftAlign = [] {
  uint32_t s = 0;
  while ((kFarN << s) < kWorldN) s++;
  return s;
}();
static_assert(kWorldN >= kFarN && (kFarN << kFarShiftAlign) == kWorldN,
              "kFarN must divide kWorldN by a power of two");
constexpr uint32_t kFarShiftBase = kFarShiftAlign;
// 8 levels at kFarN=512: outermost half-extent 6554 m at the 512 window and
// 10 cm voxels, farVox = 8 x 128 MiB = 1024 MiB (measured, not derived:
// SANDVOX_GPUMEM=1).
//
// This block previously described a 6-level configuration that was never the
// one building — it read "Two levels were REMOVED to pay for the resolution
// above" while the code sat at kFarN=256 and kFarLevels=8, i.e. it documented
// neither the old resolution nor the old level count. It is rewritten to the
// shipped configuration; do not restore that text.
//
// Dropping to 6 IS still the cheap escape if VRAM ever gets tight: it costs
// 2 x 128 MiB = 256 MiB and pulls the horizon 6554 m -> 1638 m, which is worth
// less than it sounds because fog is pinned so opacity reaches ~99% at the
// outermost half-extent — level 7 sits behind ~90% fog and level 8 behind
// ~99%. The fog pin follows kFarLevels automatically. The reason it was NOT
// taken here is that the resolution bump is a fixed 896 MiB either way and an
// 8 GiB card has the room, so paying for the horizon a second time with detail
// nobody can see was the worse of the two trades.
constexpr uint32_t kFarLevels = 8;
static_assert((uint64_t)kFarLevels * kFarVox < (1ull << 32),
              "farVox byte indices must fit u32");
static_assert(((uint64_t)(kFarLevels - 1) << kFarSlotShift) < (1ull << 32),
              "fill-queue packing (level-1)<<kFarSlotShift | slot must fit u32");
constexpr uint32_t kFarListCap = 4096;  // fill dispatches per tick (level-chunks)

// ---- cascade EDIT PATCHES (far-field edit persistence; src/sim/faredits.h) --
// The sieve fills a level chunk from pristine procgen, which used to un-do the
// live downsample every time a plane came back in or the world reloaded. Each
// farList entry now carries a patch range: cells the CPU's FarEdits index says
// the player changed, applied by the same `far` workgroup right after its
// pristine sweep.
//
// Layout of the farPatch buffer, in u32:
//   [0, 2*kFarListCap)                  per-fill-entry (payloadOffset, count)
//   [kFarPatchBase, kFarPatchBase+cap)  payload: (mat << 12) | cellIndex
// The header is indexed by the DISPATCH index (wg.x), not by slot, so it lines
// up with farList entry-for-entry.
constexpr uint32_t kFarPatchBase = kFarListCap * 2;
// Payload ceiling per tick. 128 Ki entries = 512 KiB of upload in the worst
// tick, which keeps the fill path inside the same CPU->GPU budget the mutation
// queue lives under (CLAUDE.md rule 3's <1 MB/tick). A level chunk holds
// kChunkVol = 4096 cells, so ONE entry can never exceed the cap and PrepareTick
// can simply stop popping when the budget is spent — the leftover entries stay
// queued and land next tick, exactly like the fill queue's own cap.
constexpr uint32_t kFarPatchCap = 1u << 17;
constexpr uint32_t kFarPatchWords = kFarPatchBase + kFarPatchCap;

// ---- cascade geometry, derived in ONE place ----
// Level k (1-based) holds kFarN cells per axis of 2^(k + kFarShiftBase) fine
// voxels, so its box edge is kFarN << (k + kFarShiftBase) fine voxels. Every
// distance below derives from these two, so changing kFarN, kFarLevels or the
// shift base moves fog, the trusted radius and the horizon together instead of
// leaving a hardcoded "2^k window edges" relation behind to drift.
constexpr uint32_t kFarCellVox(uint32_t k) {  // fine voxels per level-k cell
  return 1u << (k + kFarShiftBase);
}
constexpr float kFarHalfExtentMeters(uint32_t k) {  // level-k half-extent, m
  return (float)kFarN * 0.5f * (float)kFarCellVox(k) * kVoxelMeters;
}
// The residency window's own half-extent: the distance at which a ray leaves
// simulated voxels and picks up the cascade, and so the cold-start trusted
// radius before any level has filled.
constexpr float kWindowHalfExtentMeters = (float)(kWorldN >> 1) * kVoxelMeters;

// Fog reaches ~full opacity (exp(-4.5) ~= 1%) at whatever radius it is pinned
// to; kFogOpticalDepths is that budget, shared by the static pin below and by
// the adaptive term in WriteRenderParams.
constexpr float kFogOpticalDepths = 4.5f;
// Fog density such that opacity ~= 1 at the outermost level's half-extent from
// the centered player. This is the FLOOR of the adaptive density (phase 3B):
// the fully-filled cascade is the farthest anything is ever visible, so fog is
// never thinner than this.
constexpr float kFarFogDensity =
    kFogOpticalDepths / kFarHalfExtentMeters(kFarLevels);
// Ceiling of the adaptive density (phase 3B). While a cold start / teleport
// has cascade fills outstanding, fog closes in to hide the unfilled bands —
// but never nearer than 4x the residency window's own half-extent. That keeps
// the *simulated* world (the thing the player is standing in and editing)
// fully visible no matter how backlogged the fill queue is; only the LOD
// horizon ever gets fogged away.
constexpr float kFarFogDensityMax =
    kFogOpticalDepths / (kWindowHalfExtentMeters * 4.0f);
// Per-frame exponential approach of fog density toward its target. Cascade
// bands land in whole planes, so an instantly-applied density steps visibly as
// the queue drains; ~0.08/frame fades the horizon open over ~0.5 s instead.
// Render-only smoothing — the sim never sees it (CLAUDE.md rule 1).
constexpr float kFogLerpPerFrame = 0.08f;

// Must match FarParams in common.wgsl. Origins are per level, in that level's
// chunk units (one level-k chunk = 16 level cells = 2^k fine chunks).
struct FarParams {
  int32_t origins[kFarLevels][4];  // xyz = origin, w unused
};

enum class CellKind { Unknown, Air, Solid, Liquid, Gas };

// CPU-visible snapshot of GPU state, one tick latent by design (DESIGN.md §2).
struct WorldSnapshot {
  bool valid = false;
  IVec3 windowOrigin{};               // residency window origin AT CAPTURE (chunks)
  IVec3 mirrorBase{};                 // WORLD chunk coord of the 3x3x3 mirror corner
  std::vector<uint32_t> mirror;       // 27 chunks of voxel words
  uint32_t activeChunks = 0;
  uint64_t voxelTotal = 0;
  uint32_t worldHash = 0;
  uint32_t pick[8] = {};
  uint32_t particleCount = 0;         // live particles (post-resolve that tick)
  uint32_t tick = 0;                  // sim tick this snapshot was captured at
  std::vector<uint8_t> dirtyFlags;    // per-chunk next-tick dirty (kNumChunks)
  // Per-chunk support-loss flags (kNumChunks): the sim saw a supporting voxel
  // (solid/powder) vacate next to a solid there since the last readback.
  // One-shot: the GPU buffer is cleared after each copy. Feeds island checks.
  std::vector<uint8_t> supportFlags;
  std::vector<uint32_t> occupancy;    // per-slot non-air counts (streaming evict)
  // Per-slot "this chunk carries stain bits", bit 31 of the GPU occupancy word
  // (packOccStain, common.wgsl). Separate from `occupancy` so existing readers
  // of that array keep seeing a plain count. This is what lets the page-table
  // free path demote WITHOUT reading the chunk's words back: occupancy alone
  // cannot answer it, because a chunk can be all air and still carry hashed
  // stain state.
  std::vector<uint8_t> occStain;
  // Page faults since process start — voxStore()'s sentinel no-op path
  // (PLAN_page_table.md §2.4). MONOTONIC and never cleared: a non-zero value is
  // a permanent "this build has a bug" latch. Every gate asserts it is zero,
  // which is what turns §2.4's structural claim into a measurement made on
  // every run rather than in a special configuration.
  uint32_t pageFaults = 0;
  // ---- MLS-MPM fluid (seam) ----
  // The GPU-owned live particle count and the fluidArgsStage event counters
  // (the FA_* map in common.wgsl) as of this snapshot's tick. fluidLive is
  // the CPU's ONLY view of the population — conservative for record/skip
  // decisions, exact for the selftest's mass audits after a WaitIdle.
  uint32_t fluidLive = 0;
  uint32_t fluidSettledEighths = 0;   // event counter, that tick only
  uint32_t fluidExcitedEighths = 0;   // event counter, that tick only
  uint32_t fluidExciteRefused = 0;    // budget refusals, that tick only
  // THE WP5 EXCITE-REACH PROBE (fluidArgs FA_EXSEEN / FA_EXCANDID), that tick
  // only. Already in the readback's 128 bytes; surfaced here because plan §9's
  // ranked-first risk is measured in exactly this number.
  //
  // WP5 measured 169,616 candidates over 400 ticks on `worldlake` from a
  // DRAINING CA leaving transient gaps under cells — enough to convert the
  // whole 262,144-particle pool and hold ~70 ms frames. The surface shave
  // removes from the TOP so it should create no air-below, but the CA
  // re-levelling behind it might, and "should" is not a measurement. Summing
  // these two across a drain is what turns that into one.
  uint32_t fluidExciteSeen = 0;       // settled liquid cells detect looked at
  uint32_t fluidExciteCandidates = 0; // of those, trigger-satisfying
  uint32_t fluidLastSlot = 0;         // coarse position for the splash cue
  // Active fluid block slots at capture (first fluidBlockCount entries of the
  // block list). Feeds PageTable::UpdateFluidChunks so every chunk the seam
  // may write is materialized — the settle converter's >= 8 calm-tick floor
  // is what makes this latency safe.
  uint32_t fluidBlockCount = 0;
  std::vector<uint32_t> fluidBlocks;
  // Excited-fluid eighths per mirror cell (27 x 4096 bytes, mirrorBase
  // addressing — the same cube as `mirror`). Zeroed whenever no fluid is
  // live, so a stale fold can never report ghost water.
  std::vector<uint8_t> fluidMirror;
};

// One CPU-cached chunk of voxel data, fetched on demand through the async
// readback ring (island detection, terrain collision meshing).
struct CachedChunk {
  uint32_t version = 0;               // tick whose end-state this data reflects
  std::vector<uint32_t> voxels;       // kChunkVol words
};

// Owns every GPU buffer of the simulation plus the async readback ring.
class World {
 public:
  void Init(const rhi::Device& device);

  // Records the per-tick readback copies into the encoder. Call after the sim
  // passes. Returns false if all ring slots are still in flight (skip copies).
  // particleLivePage: which particleCounts index holds the post-tick count.
  // tick: the sim tick being encoded (stamps the snapshot + fetched chunks).
  bool EncodeReadbacks(const rhi::Device& device, const rhi::CommandEncoder& enc,
                       IVec3 playerChunkBase, uint32_t particleLivePage,
                       uint32_t tick);

  // Excited-fluid eighths (0..8) at a world cell, from the snapshot's fluid
  // mirror fold. 0 outside the 3x3x3 mirror, when no snapshot exists, or
  // when no fluid is live — the same Unknown-is-conservative shape KindAt
  // has, in the direction that never invents water.
  uint32_t FluidEighthsAt(IVec3 cell) const {
    if (!snap_.valid || snap_.fluidLive == 0 || snap_.fluidMirror.empty())
      return 0;
    IVec3 mc{cell.x >> 4, cell.y >> 4, cell.z >> 4};
    IVec3 d{mc.x - snap_.mirrorBase.x, mc.y - snap_.mirrorBase.y,
            mc.z - snap_.mirrorBase.z};
    if (d.x < 0 || d.x > 2 || d.y < 0 || d.y > 2 || d.z < 0 || d.z > 2)
      return 0;
    size_t m = (size_t)((d.z * 3 + d.y) * 3 + d.x);
    IVec3 lo{cell.x & 15, cell.y & 15, cell.z & 15};
    return snap_.fluidMirror[m * kChunkVol +
                             (size_t)((lo.z * 16 + lo.y) * 16 + lo.x)];
  }

  // The 3x3x3 mirror's clamped corner for a desired player-chunk corner.
  // Shared by EncodeReadbacks and the tick's mirrorBase (TickParams) so the
  // voxel mirror and the fluid-occupancy fold always describe the same cube.
  IVec3 MirrorBaseFor(IVec3 playerChunkBase) const {
    auto clampBase = [&](int v, int lo) {
      if (v < lo) v = lo;
      if (v > lo + (int)kNChunk - 3) v = lo + (int)kNChunk - 3;
      return v;
    };
    return {clampBase(playerChunkBase.x, origin_.x),
            clampBase(playerChunkBase.y, origin_.y),
            clampBase(playerChunkBase.z, origin_.z)};
  }

  // ---- toroidal residency window (DESIGN.md §3) ----
  // The resident cube covers world chunks [origin, origin+kNChunk) per axis;
  // world chunk c lives in slot (c mod kNChunk) per axis (bitmask — POT).
  // Set by the streaming manager between ticks; every tick/render uses it.
  void SetWindowOrigin(IVec3 o) { origin_ = o; }
  IVec3 WindowOrigin() const { return origin_; }
  bool ChunkInWindow(IVec3 wc) const {
    IVec3 d{wc.x - origin_.x, wc.y - origin_.y, wc.z - origin_.z};
    int n = (int)kNChunk;
    return d.x >= 0 && d.y >= 0 && d.z >= 0 && d.x < n && d.y < n && d.z < n;
  }
  bool CellInWindow(IVec3 c) const {
    return ChunkInWindow({c.x >> 4, c.y >> 4, c.z >> 4});
  }
  static uint32_t SlotChunkIndex(IVec3 wc) {
    int m = (int)kNChunk - 1;
    return (uint32_t)(((wc.z & m) * (int)kNChunk + (wc.y & m)) * (int)kNChunk +
                      (wc.x & m));
  }
  // slot linear cell index of a world cell (caller checked CellInWindow)
  static uint32_t SlotCellIndex(IVec3 c) {
    uint32_t lx = (uint32_t)(c.x & 15), ly = (uint32_t)(c.y & 15),
             lz = (uint32_t)(c.z & 15);
    return SlotChunkIndex({c.x >> 4, c.y >> 4, c.z >> 4}) * kChunkVol +
           (lz * kChunk + ly) * kChunk + lx;
  }
  IVec3 SlotToWorldChunk(uint32_t slotIdx) const {
    IVec3 s{(int)(slotIdx % kNChunk), (int)((slotIdx / kNChunk) % kNChunk),
            (int)(slotIdx / (kNChunk * kNChunk))};
    int m = (int)kNChunk - 1;
    return {origin_.x + ((s.x - origin_.x) & m), origin_.y + ((s.y - origin_.y) & m),
            origin_.z + ((s.z - origin_.z) & m)};
  }
  static uint64_t PackChunkKey(IVec3 wc) {
    auto u = [](int v) { return (uint64_t)(uint32_t)(v + (1 << 20)) & 0x1FFFFF; };
    return u(wc.x) | (u(wc.y) << 21) | (u(wc.z) << 42);
  }

  // ---- the page table: THE SECOND SEAM (PLAN_page_table.md §2.1a) ----------
  //
  // > Normative: any CPU path that computes a byte offset into `voxels` from a
  // > slot index must resolve through PageOffsetOfSlot(slot). There is no other
  // > way to address `voxels` from C++.
  //
  // The shader seam (voxWordAt / voxWordIndex in common.wgsl) covers every
  // world-coordinate access in every kernel. It says nothing about C++ that
  // computes `slot * kChunkBytes` — and every such site assumed slot s lives at
  // s * 16 KiB, which is exactly the assumption paging deletes. The five sites
  // are the chunk-fetch copy and the 3x3x3 mirror copy (world.cpp), eviction
  // and store-hit refill (stream.cpp), and the selftest voxel dumps.
  //
  // The mirror is the worst of them, because its failure is invisible to
  // everything else: it is CPU-only collision data, so a corrupted mirror is
  // the player falling through the floor with a CORRECT world hash.

  // Byte offset of slot `slot`'s page within `voxels`, or kNoPage if the slot
  // is a sentinel and has no physical page. Callers must test.
  static constexpr uint64_t kNoPage = ~(uint64_t)0;
  uint64_t PageOffsetOfSlot(uint32_t slot) const {
    const uint32_t e = pageTableCpu_[slot];
    if ((e & kPtSentinelBit) != 0u) return kNoPage;
    return (uint64_t)e * kChunkVol * 4;
  }
  // The raw table entry for a slot — for the paths that need to know WHICH
  // sentinel (eviction synthesizing RLE, the mirror synthesizing words).
  uint32_t PageEntryOfSlot(uint32_t slot) const { return pageTableCpu_[slot]; }

  // See mirrorSeed_. Set by Stream::Init at the same point the page table's
  // copy is set.
  void SetMirrorSeed(uint32_t s) { mirrorSeed_ = s; }

  // Residency mode. `dense` is the identity map — page i for slot i — which
  // makes every address bit-identical to pre-paging code while still running
  // the whole translation path (the load, the branch, the multiply-add). It is
  // the only live differential oracle the engine has now that Dawn is gone
  // (§6.3), so it is load-bearing test infrastructure, not a fallback: never
  // selected automatically, always available.
  enum class Residency { Dense, Paged };
  Residency residency = Residency::Dense;
  uint32_t PoolPages() const {
    return residency == Residency::Dense ? kNumChunks : kPoolPages;
  }

  // The CPU table itself. Read freely; the MUTABLE accessor is for PageTable
  // (sim/pagetable.h) alone — it owns the allocator and the materialization
  // rule, and there must be exactly one writer or the table and the free list
  // drift apart.
  const std::vector<uint32_t>& pageTableCpu() const { return pageTableCpu_; }
  std::vector<uint32_t>& pageTableCpuMutable() { return pageTableCpu_; }

  // The allocator + the conservative dirty mirror + the materialization rule
  // (sim/pagetable.h). Owned by World so every path that has a World has it —
  // there is no configuration in which the voxel buffer exists and the thing
  // that decides its layout does not. Held by pointer to keep pagetable.h out
  // of world.h's include set, which is otherwise the whole engine's.
  class PageTable* pages = nullptr;

  // ---- on-demand chunk fetches (island detection / terrain meshing) ----
  // Keyed by WORLD chunk coords: slots get recycled by streaming, world
  // chunks don't. Queue a chunk for CPU readback; duplicates are coalesced;
  // non-resident requests are ignored. Up to kFetchPerTick chunks ride each
  // tick's readback slot (bounded traffic).
  void RequestChunkFetch(IVec3 worldChunk);
  // Cached copy of a world chunk, or nullptr if never fetched. version is the
  // tick whose post-sim state the data reflects.
  const CachedChunk* Cached(IVec3 worldChunk) const;
  static constexpr uint32_t kFetchPerTick = 64;  // 1 MB/tick ceiling
  // Copies the next-tick dirty buffer into the pending slot (caller knows
  // which of dirty[0]/dirty[1] that is this tick).
  void EncodeDirtyCopy(const rhi::CommandEncoder& enc, const rhi::Buffer& dirtyNext);
  // Kick MapAsync for the slot used by the last EncodeReadbacks. Call after
  // queue.Submit.
  void KickReadback();

  // Latest consumed snapshot (updated by MapAsync callbacks during
  // instance.ProcessEvents()).
  const WorldSnapshot& Snap() const { return snap_; }
  // A world RESET (worldgen, LoadWorld) makes the held snapshot a description
  // of a DEAD WORLD, and callers must not be able to consume it: harness
  // scenes restart their tick counters, so a leftover snapshot's stamp can
  // read as "fresh" against the new scene's early ticks — the paged mirror
  // then skips its tightening (encodeTick <= snapTick) and, had the ranges
  // lined up, would have tightened against ANOTHER world's dirty flags.
  void InvalidateSnapshot() { snap_.valid = false; }

  // ---- MPM fluid render bounds (RenderParams::fluidLo/fluidHi) ------------
  // Record this tick's CPU-known spawn cells so a fresh pour is visible on the
  // frame it lands, rather than when the block list gets back from the GPU a
  // few ticks later. Render-only, and NOT an input to any sim decision.
  void NoteFluidSpawnBounds(const FluidSpawnOp* ops, uint32_t n, uint32_t tick);
  // Inclusive world-voxel AABB of everything the fluid surface march can hit,
  // already dilated by kFluidRenderPadVox. Returns false when there is no
  // fluid at all (the empty-box case the renderer turns into "skip"). Before
  // the first snapshot arrives it reports the whole residency window: never
  // clip water the CPU cannot yet see.
  bool FluidRenderBounds(uint32_t tick, IVec3& lo, IVec3& hi) const;

  // Voxel word at cell from the mirror; kind Unknown outside mirror coverage.
  //
  // `classOf` is a COLLISION class table, not simply a copy of each material's
  // klass — build it with BuildCollisionClasses() below. The difference is
  // materials flagged passable (soft vegetation), which report as Air here so
  // bodies move through them while staying ordinary solids everywhere else.
  CellKind KindAt(IVec3 cell, const std::vector<uint32_t>& classOf) const;

  // THE HEIGHT CONTRACT (DESIGN.md; landColumn in worldgen.wgsl):
  //
  //     World::TerrainHeight(x, z, seed)  ==  genColumn(x, z, seed).h,
  //     exactly, for all inputs.
  //
  // The GROUND — terrain octaves, authored pool floors and rims, the pond bowl
  // carve, the tarn berm. NOT "the topmost solid voxel", which would include
  // canopy, ruin walls, grass tufts and the arena deck and could not be
  // mirrored cheaply. ~25 hash3 per call: fine at O(1)/frame (spawn placement,
  // fixture anchoring, a mob ground probe), never in a per-voxel loop.
  static int TerrainHeight(int x, int z, uint32_t seed);

  // The tarn a column stands in or beside. Exists so the `terrain` gate can
  // assert the BERM INVARIANT — every column in the berm core is above its
  // pond's waterline — which is what replaced pondSurface's 24-sample rim
  // minimum with a structural guarantee. Reports `near` only where landColumn
  // would actually apply the berm, so a passing assertion means exactly what
  // the shader promises and nothing looser.
  struct PondQuery {
    bool inDisc;   // this column is under a tarn's water
    bool near;     // outside a disc but inside the berm/shore scan band
    int past;      // voxels beyond that disc's rim (0 = first column outside)
    int surf;      // that tarn's water surface Y
  };
  static PondQuery PondNearColumn(int x, int z, uint32_t seed);

  // ---- THE MAP PROBE (assets/tuner.html, Worldgen tab) ---------------------
  //
  // Everything a picture of the terrain needs, from ONE evaluation of the
  // column instead of three. `--heightmap` renders a res x res grid of these
  // and the tuner draws it, which is how worldgen tuning stopped being "edit
  // a number, regen the world, fly somewhere, look".
  //
  // IT IS THE SAME CODE THE GAME USES, deliberately. A JavaScript
  // reimplementation of the octave ladder in the tuner would be a THIRD copy
  // of the height function beside the shader and this mirror, with nothing
  // enforcing it — and the whole reason `check_invariants.py` compares the two
  // that already exist is that the third one (the deleted `surfHeightAt`) had
  // silently gone stale. So the map costs a process launch and is exact,
  // rather than being instant and a guess.
  struct Column {
    int h;        // ground, the height contract
    int slope;    // landform gradient, Q8 (256 = the CA's angle of repose)
    int sed;      // loose wedge thickness inside h
    int water;    // standing-water surface Y, or INT32_MIN for none
  };
  static Column TerrainColumn(int x, int z, uint32_t seed);

  // ---- THE BASIN REGISTRY'S SOURCE (docs/PLAN_water_master.md component 2) --
  //
  // A body of water needs a CONTAINER before it can have an area-per-height
  // curve, and worldgen already knows every container it made: a tarn is
  // `pondInfo`'s disc and the three set pieces are authored literals. Both are
  // stated exactly once, in world.cpp, inside the block check_invariants.py
  // token-compares against the shader. These two accessors publish them.
  //
  // The alternative — sim/waterbody.cpp re-deriving the tile hash, the inset,
  // the keep-out boxes and the slope gates — would be the FOURTH copy of the
  // terrain, and the deleted `surfHeightAt` is this file's own evidence for how
  // that ends: it was a copy of TerrainHeight's arithmetic and it had already
  // gone stale before anyone noticed.

  // The analytic tarn belonging to one pond TILE, or `present = false` where
  // the tile rolled no pond, lost the slope gate, or landed in the authored
  // origin keep-out. A disc never leaves its own tile (pondInfo's inset), so
  // scanning the tiles a box touches finds every tarn that can reach into it.
  struct PondDisc {
    bool present = false;
    int cx = 0, cz = 0;  // centre, world cells
    int r = 0;           // radius, world cells
    int surf = -1;       // water surface Y
  };
  static PondDisc PondTile(int tileX, int tileZ, uint32_t seed);
  // Tile pitch in world cells (worldgen.pondTile). Exposed so a caller can turn
  // a box into a tile range without taking a dependency on the tuning struct.
  static int PondTileSize();

  // The three authored pools at the world origin — the lake, the oil pond and
  // the lava pool of `--fluid-bench wp5`'s pond68 scene. Flat floors, vertical
  // walls, a fixed fill level: the degenerate case of a container curve, and
  // the one every bench scene in docs/PLAN_fluid_overhaul.md §8 measures.
  //
  // `mat` is the material NAME, resolved by the consumer (design guideline #4).
  struct AuthoredPool {
    int cx, cz, r;     // disc, world cells (a column is inside iff d2 < r*r)
    int floorY;        // the flat floor
    int waterY;        // the fill level; water occupies (floorY, waterY]
    int rimY;          // the containment rim outside the disc = spill elevation
    const char* mat;   // "water", "oil", "lava"
  };
  static constexpr int kAuthoredPools = 3;
  static void AuthoredPoolList(AuthoredPool out[kAuthoredPools]);

  // Fluid-lab worldgen mode (kLabSlabY block above). A process-wide static
  // because TerrainHeight is static and the flag must gate BOTH the CPU
  // mirror and every TickParams.labMode write from one truth. Set exactly
  // once, at startup, by --lab / --fluid-bench; nothing else may touch it.
  static void SetLabWorld(bool on);
  static bool LabWorld();

  rhi::Buffer voxels;      // the PHYSICAL PAGE POOL: PoolPages() * kChunkVol
                            // u32. Under --residency dense this is kNumChunks
                            // pages and the page table is the identity map, so
                            // it is address-identical to the pre-paging dense
                            // buffer. Never index it from a slot without going
                            // through PageOffsetOfSlot (C++) or voxWordAt /
                            // voxWordIndex (WGSL) — see the seam note below.
  rhi::Buffer pageTable;   // kNumChunks u32, one per chunk SLOT. Derived data:
                            // not hashed, not persisted, not replicated.
  rhi::Buffer pageFaults;  // 4 u32 ([0] = the atomic counter). Permanently
                            // bound and unconditional: voxStore() increments it
                            // on the sentinel no-op path, every gate asserts it
                            // is zero, and there is exactly ONE bind-group
                            // layout and one pass_table.def rather than a
                            // flag-dependent pair (PLAN_page_table.md §5.1).
  rhi::Buffer dirty[2];    // kNumChunks u32
  rhi::Buffer dirtyList;   // kNumChunks u32 — compacted dirty-chunk indices
  rhi::Buffer argsStage;   // 3 u32 — compact shader writes (x = dirty count, y = z = 1)
  rhi::Buffer dispatchArgs;// 3 u32 — indirect-only copy of argsStage; kept out of all
                            // bind groups (Dawn forbids indirect + bound-writable usage
                            // of one buffer in the same pass, even if statically unused)
  rhi::Buffer occupancy;   // kNumChunks u32: (rayBlockers << 16) | nonAirCount
                            // (see the occupancy packing note in common.wgsl),
                            // FOLLOWED BY kNumChunks * kSubOccStride u32 of
                            // sub-chunk bitmask (the kSubOccShift block above).
                            // One buffer on purpose: the barriers and bind-group
                            // entries for Occupancy already cover it.
  rhi::Buffer support;     // kNumChunks u32 — support-loss flags (sim_step writes,
                            // readback consumes + clears; drives island checks)
  rhi::Buffer hash;        // 4 u32 (only [0] used)
  rhi::Buffer tickUBO;     // TickParams
  rhi::Buffer passUBO;     // 27 slices * 256 B (3x3x3 color phases)
  rhi::Buffer opsBuf;      // kMaxOpsPerTick BrushOp
  rhi::Buffer renderUBO;   // RenderParams
  rhi::Buffer dirtyViz;    // kNumChunks u32, CPU-uploaded snapshot for debug overlay
  rhi::Buffer actVoxViz;   // per-voxel activity bits (packed u32), debug overlay
  rhi::Buffer pick;        // 8 u32

  // ---- the water-body ledger (docs/PLAN_water_master.md components 3-5) ----
  // kWaterBodyCap * kWaterBodyStateWords u32 (4 KiB). GPU-OWNED, and that is
  // the load-bearing property rather than an implementation detail:
  //
  //   * The ledger debits by what the shave ACTUALLY removed (plan §3.2), and
  //     the only honest source for that is an atomic the shave increments.
  //     Reading it back to the CPU would put fence retirement inside the
  //     control path of a voxel write, which is rule 1 through the back door.
  //   * So `level`, `debit`, `area` and the adoption verdict all live here.
  //     The CPU sends geometry and thresholds; it is told nothing back except
  //     by a gate doing a blocking read, which no frame path does.
  //
  // Derived data like the page table: not hashed, not saved, rebuilt from the
  // voxels by the adoption reduce. CopySrc for `--gate waterbody`'s conservation
  // audit; CopyDst so a reset can zero it.
  rhi::Buffer waterBodyState;

  // ---- particles + explosions (M5, DESIGN.md §5/§7) ----
  rhi::Buffer particles[2];    // kParticleCap Particle (32 B), double-buffered
  rhi::Buffer particleCounts;  // 4 u32: [0]/[1] = live count per page
  rhi::Buffer claim;           // kClaimSize u32 — reinsertion claim hash
  rhi::Buffer pArgsStage;      // 8 u32: [0..3] draw args, [4..6] dispatch args
  rhi::Buffer pDispatchArgs;   // 3 u32, indirect-only (see dispatchArgs note)
  rhi::Buffer drawArgs;        // 4 u32, indirect-only draw args for particles
  rhi::Buffer expOps;          // kMaxExplosionsPerTick ExplosionOp
  rhi::Buffer expMask;         // per-op destruction scratch (see sim_explode.wgsl)
  rhi::Buffer cellOps;         // kMaxCellOpsPerTick CellOp (island removal)
  rhi::Buffer spawnOps;        // kMaxParticleSpawnsPerTick ParticleSpawn
  rhi::Buffer sprites;         // kMaxSprites Sprite (CPU-written, render-only)

  // ---- MLS-MPM fluid (see the fluid block above kFluidCap) ----
  // fluidGrid, fluidBlockMap and fluidBlockList are per-substep scratch,
  // cleared and rebuilt inside the tick. fluidParticles[2] is the carried
  // state: a ping-pong pair the seam's deterministic compaction copies
  // between once per tick (read page_, write 1-page_, exactly the ballistic
  // particles' parity convention), reconstructible from the op stream.
  // The seam's converters write VOXELS (excite clears cells, settle fills
  // them) — the settled side of the fluid lives in the hashed world domain.
  rhi::Buffer fluidParticles[2]; // kFluidCap FluidParticle (128 B, common.wgsl)
  rhi::Buffer fluidSpawnOps;     // kMaxFluidSpawnsPerTick FluidSpawnOp
  // 2 * kNumChunks u32. [slot] = 0 inactive, else blockIdx+1. [kNumChunks +
  // slot] = the chunk's Y-OCCUPANCY MASK: bit L set when local y level L of
  // that chunk can carry fluid density (MPM node mass or settled liquid,
  // dilated +-2 for the kernel's reach). Render-only derived data - the
  // solver never reads it - and it exists because `mark` pads the block set
  // by 3 cells, so "has a block" stopped meaning "has water in it".
  rhi::Buffer fluidBlockMap;
  rhi::Buffer fluidBlockList;    // kFluidBlocks u32: blockIdx -> chunk slot
  rhi::Buffer fluidGrid;         // kFluidBlocks * 4096 nodes * 8 i32 (mass,
                                 // mom xyz, species mass x3, foam — FLUID_GW)
  rhi::Buffer fluidArgsStage;    // 16 u32 — the FA_* word map in common.wgsl:
                                 // node args + live count + event counters
  rhi::Buffer fluidDispatchArgs; // 3 u32, indirect-only (see dispatchArgs note)
  rhi::Buffer fluidPDispatchArgs; // 3 u32, indirect-only: per-particle passes
                                  // + the seam's list-shaped dispatches (the
                                  // seam re-copies between uses)
  // Seam scratch (sim_fluid_seam.wgsl). All fill-cleared per fluid tick
  // except fluidCalm, which persists (per-slot calm counters, cleared on
  // worldgen/reset).
  rhi::Buffer fluidExciteScratch; // [0..15] header, [16..16+N) per-slot counts,
                                  // [16+N..16+2N) per-slot bases, then the
                                  // slot list (N = kNumChunks)
  rhi::Buffer fluidCalm;          // kNumChunks u32: consecutive calm ticks
  rhi::Buffer fluidSettleScratch; // [0..N) per-slot max speed, [N..2N) settle
                                  // marks (list idx | flags), [2N..2N+16]
                                  // settle list header+slots, then bins:
                                  // kFluidSettleMax * kChunkVol * 2 words,
                                  // then the per-column excite-unstable mask:
                                  // kFluidSettleMax * 8 words (256 columns)
  rhi::Buffer fluidCompactScratch; // per-256-span survivor counts + bases
                                   // (kFluidCap/256 * 2 u32)
  rhi::Buffer fluidMirror;        // 27 mirror chunks x 4096 cells, one byte
                                  // of excited-fluid eighths per cell packed
                                  // 4/word — the swimming query's view of the
                                  // particles, folded by the seam's
                                  // mirrorFold and read back with the
                                  // snapshot (World::FluidEighthsAt)
  rhi::Buffer fluidCellScratch;   // per active-block cell, 2 u32: [0] intent
                                  // (mat<<16 | stainAmt<<3 | stainType,
                                  // atomicMax by the seam's particleTick) and
                                  // [1] flags (bit0 = a CA reaction consumed
                                  // this cell's excited fluid, atomicOr by
                                  // sim_step). The occupancy/consumption
                                  // bridge between the CA and the particles;
                                  // fill-cleared each fluid tick.
  rhi::Buffer debugBoxes;      // kMaxDebugBoxes DebugBox (collision overlay)
  rhi::Buffer bodyInstances;   // debris-body voxel instances (render)
  rhi::Buffer bodyXforms;      // debris-body transforms (render)
  rhi::Buffer genList;         // worldgen streaming: slot indices to generate
  rhi::Buffer pageFillList;    // JITTER materialization: (slot, entry) pairs

  // ---- far-field cascades (render-only; never bound in any sim pipeline) ----
  rhi::Buffer farVox;   // kFarLevels x 256^3 material bytes, packed 4/u32
                         // (atomic in the fill/downsample kernels: partial-word
                         // byte updates from neighboring dirty chunks race)
  rhi::Buffer farOcc;   // kFarLevels x kNumChunks u32 non-air counts
  rhi::Buffer farList;  // kFarListCap entries: (level-1)<<kFarSlotShift | slot
  rhi::Buffer farUBO;   // FarParams
  rhi::Buffer farPatch; // per-fill edit patches (kFarPatch* above)

  // The CPU's far-field edit index (src/sim/faredits.h), owned by Stream —
  // it is fed by the same eviction path that fills the ChunkStore, and that
  // store is its reconstruction source. Forward-declared exactly like `pages`
  // so world.h stays free of the streaming headers. Null before Stream::Init,
  // and null-checked by FarField (a fill with no index is the old behavior).
  class FarEdits* farEdits = nullptr;

 private:
  struct Slot {
    rhi::Buffer buf;
    bool inFlight = false;
    IVec3 base{};      // world chunk coord of the mirror corner
    IVec3 origin{};    // window origin at encode time
    uint32_t particleLivePage = 0;
    uint32_t tick = 0;
    std::vector<IVec3> fetchIds;  // world chunks riding this slot
    // Sentinel slots are not copied at all (§2.1a); their table entry is
    // recorded here at encode time and their 4,096 words are synthesized on
    // consumption. 0 means "a real copy was issued for this index".
    std::vector<uint32_t> fetchSentinel;
    std::array<uint32_t, 27> mirrorSentinel{};
  };
  static constexpr int kSlots = 3;
  Slot slots_[kSlots];
  int lastSlot_ = -1;
  WorldSnapshot snap_;
  IVec3 origin_{0, 0, 0};

  // Recent CPU-side fluid spawn box (render bounds only — see
  // NoteFluidSpawnBounds). Held for kFluidSpawnBoundsTicks so a pour is
  // visible from the frame it lands until the snapshot's block list catches
  // up; after that the block list is the better (tighter) answer.
  static constexpr uint32_t kFluidSpawnBoundsTicks = 12;
  IVec3 spawnBoxLo_{0, 0, 0};
  IVec3 spawnBoxHi_{-1, -1, -1};
  uint32_t spawnBoxTick_ = 0;

  std::vector<IVec3> fetchQueue_;
  std::unordered_map<uint64_t, uint8_t> fetchQueued_;   // dedup (packed key)
  std::unordered_map<uint64_t, CachedChunk> cache_;     // packed world key

  // CPU mirror of the page table — the authority for every C++ translation.
  // The GPU buffer is written FROM this, never read back into it: the table is
  // a pure function of CPU-side allocation history (§2.5), so a readback would
  // be asking the GPU about a decision the CPU made.
  std::vector<uint32_t> pageTableCpu_;
  // The world seed, for reconstructing a JITTER sentinel's per-cell palette
  // variant in the CPU mirror (the readback callback synthesizes the words of
  // chunks that were never copied). Set alongside PageTable::SetWorldSeed so
  // the two synthesis paths cannot disagree.
  uint32_t mirrorSeed_ = 0;

  // Scratch for ONE sequential copy of the readback slot's per-chunk arrays
  // (dirty / occupancy / support) before they are scanned. The slot is a
  // persistently mapped host allocation, and a word-at-a-time scan of one is
  // the worst possible consumer of it — the same mistake Stream::HarvestEvict
  // documents and measures (stream.cpp, "One sequential copy out of
  // write-combined map memory"). The mirror memcpy at the top of the readback
  // callback already did this for its 432 KiB; these three arrays never got
  // the same treatment. Sized once, on first use.
  std::vector<uint8_t> snapBounce_;
};
