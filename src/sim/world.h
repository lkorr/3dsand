#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
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

// ---- TICKET SLOTS: storage that is resident but not in the window ----------
// docs/PLAN_chunk_tickets.md §2.1. THE SLOT SPACE IS NO LONGER THE WINDOW.
//
// A chunk's slot used to be its world coordinate modulo the window
// (`chunkSlotIndex`, common.wgsl), so there were exactly kNumChunks slots and
// arithmetic assigned every one of them. That is why "simulate a chunk 40
// chunks away" could not be done by allocating a page: page space was never the
// constraint, slot IDENTITY was. Tickets add slots that no window coordinate
// maps to, so a distant chunk can be resident without evicting a near one.
//
//   slots [0, kNumChunks)           the window, addressed by the toroidal mask.
//                                   UNCHANGED, and still the only slots the
//                                   mask can produce.
//   slots [kNumChunks, kNumSlots)   ticket slots, addressed only through the
//                                   ticket map (a CPU-built wc -> slot table).
//
// THE DISTINCTION THAT MATTERS EVERYWHERE BELOW: kNumChunks is a statement
// about the WINDOW's geometry (how many chunks fit in the box, what the mask
// wraps at); kNumSlots is a statement about STORAGE (how many per-slot records
// exist). Anything that sizes a buffer, bounds a dispatch over slots, or is a
// plane stride inside a per-slot buffer is kNumSlots. Anything that describes
// the window box — the wrap, `chunkInWindow`, the raymarch's clip — stays
// kNumChunks. Conflating them is how a ticket slot ends up reading a window
// chunk's memory, which is the failure docs/PLAN_chunk_tickets.md §5 names as
// the risk of this phase.
//
// P0 SHIPS kTicketMax = 0, so kTicketSlots is 0 and kNumSlots == kNumChunks
// exactly. Every buffer is byte-identical, every dispatch is the same size, and
// the acceptance for this phase is that the world hash and the smoke probes do
// not move. What P0 buys is that the ~160 addressing sites are CLASSIFIED and
// routed through named functions, so P1 turns tickets on by changing this one
// constant plus the ticket map's contents, not by re-auditing 15 shaders.
constexpr uint32_t kTicketMax = 0;          // concurrent tickets; P1 raises it
constexpr uint32_t kTicketBoxN = 5;         // 5^3 chunk box, inner 3^3 active
constexpr uint32_t kTicketChunks = kTicketBoxN * kTicketBoxN * kTicketBoxN;  // 125
// Rounded up to a multiple of 64 because sim_compact dispatches 64 slots per
// workgroup over the whole slot space and kNumChunks is already a multiple of
// 64; a ragged tail would need a second bound in the shader.
constexpr uint32_t kTicketSlots =
    ((kTicketMax * kTicketChunks + 63u) / 64u) * 64u;          // 0 at kTicketMax=0
constexpr uint32_t kNumSlots = kNumChunks + kTicketSlots;      // 32768 at P0
static_assert(kNumSlots % 64 == 0,
              "sim_compact dispatches kNumSlots/64 workgroups of 64");

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
                   kMatLeafGreen = 46, kMatStemGreen = 47,
                   // analytic plants (sim/microvox.h `plant` blocks): the
                   // meadow flowers, the forest floor set and the tall grass.
                   // Array positions in materials.json, like everything here.
                   kMatFlowerBluebell = 65, kMatFlowerFoxglove = 66,
                   kMatFlowerButtercup = 67, kMatFern = 88,
                   kMatMushroomCluster = 89, kMatToadstoolPale = 90,
                   kMatTallGrass = 95, kMatTallGrassHead = 96,
                   kMatMushroomLarge = 122;

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

// ---- THE DIRTY-REASON NAMES, in ONE place ---------------------------------
// Bit b of a chunk's dirty word is the rule that asked for it: bits 0..11 are
// common.wgsl's DIRTY_R_*, bits 12.. are sim_step.wgsl's DIRTY_M_*. This table
// used to exist twice — once in World's snapshot fold and once in the `sleep`
// gate — with a comment on each saying it must match the other. It did not
// survive the first bit added after that comment was written, which is the
// standing argument against two lists (tuning_params.def, pass_table.def).
//
// Order is bit order. Adding a bit means adding a row HERE and nowhere else.
constexpr int kDirtyReasonBits = 26;
inline constexpr const char* kDirtyReasonName[kDirtyReasonBits] = {
    "write",      "react-idle", "stain-idle", "flow",
    "viscous",    "seam",       "part",       "wbody",
    "mutate",     "MOVE",       "STAIN-WROTE","REACT-FIRED",
    // sim_step's DIRTY_M_* liquid-stage split
    "down",       "diag",       "equalize",   "split",
    "film",       "displace",   "bridge",     "SUBMERGED",
    "film-press", "powder",     "gas",        "solo",
    // docs/PLAN_gas_particles.md P0: gas moving FLAT (the top-plane sheet) and
    // gas whose intent pointed out of the residency window (the sink).
    "gas-lat",    "gas-edge"};

// The bit for a reason NAME, resolved from the one table above rather than
// written down as a number a second time -- 22/24/25 in a header is exactly
// the drift the single-list rule exists to stop. Returns 0 for an unknown
// name, which the static_assert below turns into a build error.
constexpr uint32_t DirtyReasonBit(const char* name) {
  for (int i = 0; i < kDirtyReasonBits; i++) {
    const char* a = kDirtyReasonName[i];
    const char* b = name;
    while (*a != '\0' && *a == *b) { a++; b++; }
    if (*a == '\0' && *b == '\0') return 1u << i;
  }
  return 0u;
}

// Every mark a MOVING GAS VOXEL leaves on its chunk. The renderer's "gas may
// be present this frame" flag (RenderParams bit 3, PLAN_gas_particles stage
// 1b) is armed from this OR'd over the whole dirty array plus the live parcel
// count: parcels alone are not enough, because in-window smoke is a voxel and
// leaves no parcel behind until it reaches a face.
constexpr uint32_t kDirtyGasMask = DirtyReasonBit("gas") |
                                   DirtyReasonBit("gas-lat") |
                                   DirtyReasonBit("gas-edge");
static_assert(kDirtyGasMask == (DirtyReasonBit("gas") |
                                DirtyReasonBit("gas-lat") |
                                DirtyReasonBit("gas-edge")) &&
                  DirtyReasonBit("gas") != 0 &&
                  DirtyReasonBit("gas-lat") != 0 &&
                  DirtyReasonBit("gas-edge") != 0,
              "a gas dirty-reason name was renamed in kDirtyReasonName without "
              "updating kDirtyGasMask");

// Particle system sizes — must match common.wgsl.
constexpr uint32_t kParticleCap = 262144;
constexpr uint32_t kClaimSize = 262144;

// ---- GAS PARTICLES (docs/PLAN_gas_particles.md stage 1) --------------------
// Gas that has left the residency window. Its OWN pool, not a share of
// kParticleCap, and the plan's §2.9 recommendation for two reasons that both
// turned out to matter: the density splat wants to walk gas and only gas, and
// the two populations must not be able to starve each other — a fight full of
// ballistic debris must not thin a plume, and a forest fire must not stop a
// sword from shattering.
//
// EVERY ONE OF THESE IS A BOUND (rule 2). At the caps the system degrades:
// gasLeave is refused and the voxel behaves exactly as it does today, which is
// the old behaviour, not a failure.
//
// Must match the GAS_* block in assets/shaders/sim_gas.wgsl (and GAS_SPAWN_CAP
// in sim_step.wgsl); scripts/check_invariants.py compares them.
// sim.gasMode. Two states, and the names say what each PROMISES rather than
// what it switches: at Wall the edge behaves as it did before stage 1 and no
// gas pass is recorded, so the feature cannot be seen at all.
constexpr uint32_t kGasModeWall = 0;
constexpr uint32_t kGasModeSink = 1;

constexpr uint32_t kGasParticleCap = 262144;   // 8 MiB per page, 16 MiB paired
// 65,536 and not the 8,192 this shipped with for one afternoon, and the reason
// is RULE 1 rather than throughput. `gasLeave` charges a shared atomicAdd
// cursor, so WHICH voxels are refused when the list fills is decided by which
// workgroup arrived first — and a refused voxel STAYS IN THE GRID, so that
// choice is visible in the world hash. (The same shape as sim_particle's
// `append` at kParticleCap, which the engine has always had; the difference is
// that a vaporized particle writes no voxel on the tick it is dropped.)
//
// Two ways out, and this is the cheap one: make the cap unreachable so the
// binding constraint is the POOL instead, whose overflow drops a parcel that
// is already outside the window and therefore cannot move a voxel. 65,536 is a
// quarter of the window's top face in ONE tick, and four such ticks exhaust
// kGasParticleCap anyway. The `gas-leave` gate asserts refusals == 0, so the
// day this is not enough it is a printed number and not a silent divergence.
//
// The real fix, if that day comes, is mark+apply: the CA flags cells that want
// to leave and a second pass converts them in a deterministic order, which is
// the pattern sim_explode already uses for exactly this reason.
constexpr uint32_t kGasSpawnPerTick = 65536;   // window-edge conversions per tick
constexpr uint32_t kGasCpuSpawnPerTick = 1024; // CPU-authored gas spawns per tick
constexpr uint32_t kGasClaimSize = kClaimSize; // re-entry claim hash
constexpr int32_t  kGasCeilingVox = 192;       // die this far above the window top

// The outer density box: one 16-BIT COUNT per cell, two to a word, GAS_OUTER_N
// cells per axis at 2^kGasOuterShift fine voxels each. The edge is 2x the window's
// and the box is centred on the window, which is what makes the mapping the
// renderer reproduces a single expression with no per-frame state:
//
//     originVox = windowOriginChunks * kChunkSize - kWorldN/2
//     cell      = (worldVoxel - originVox) >> kGasOuterShift
//
// DEVIATION from the plan, which asked for 0.4 m cells AND a 2x-window span
// AND 2 MiB. 128^3 bytes IS 2 MiB and 128 x 0.4 m is 51.2 m, half the stated
// span — the three numbers never agreed. Span and memory are kept; the cell is
// 8 voxels = 0.8 m.
constexpr uint32_t kGasOuterN = 128;
constexpr uint32_t kGasOuterShift = 3;
constexpr uint32_t kGasOuterCells = kGasOuterN * kGasOuterN * kGasOuterN;
// WIDENED FROM A BYTE (stage 1b). A byte capped a 0.8 m cell at the 192 the
// splat guard allowed, i.e. 192/512 = 37.5% full, which was fine while the box
// only ever held PARCELS — everything in it had already left the window and was
// tens of metres away. Stage 1b splats every in-window gas voxel into the same
// box so the two representations can be crossfaded, and an in-window smoke
// column is routinely denser than that: a cell can legitimately be all 512 of
// its fine voxels, and several parcels may share one on top. Fading a crisp
// voxel plume INTO a representation that saturates at 37.5% would visibly thin
// the plume exactly where the crossfade is supposed to be invisible. 4 MiB.
constexpr uint32_t kGasOuterWords = kGasOuterCells / 2;   // 1 Mi u32 = 4 MiB
static_assert(kGasOuterN << kGasOuterShift == 2 * kWorldN,
              "the gas outer box must span exactly two window edges — the "
              "renderer derives its origin from that identity");

// ---- FAR FIRE PLUMES: emitters for fires the window has left behind --------
//
// THE GAP. A chunk that leaves the residency window mid-burn is FROZEN: its
// `ember`/`lava`/burning-foliage voxels stay baked into the far cascade by the
// last `fardown`, so the fire is still visibly orange from 60 m away — forever.
// Its smoke is not. Smoke parcels are only ever BORN at the window face by the
// running CA (sim_step's `gasLeave`), and the ones already in flight die on
// smoke's authored decay in ~3.7 s. The distant fire goes dark within seconds
// of being evicted and then burns silently for the rest of the session.
//
// So the CPU keeps an index of where the frozen fires are (src/sim/farplumes.h,
// fed by the same eviction harvest that feeds FarEdits) and hands the GPU a
// short list of EMITTERS, one per gasOuter column footprint. `gasFarPlume` in
// sim_gas.wgsl synthesizes a rising, drifting, billowing column into gasOuter
// for each of them.
//
// RENDER-ONLY, and structurally so: the only buffer this feature writes is
// gasOuter, which the sim never reads and the world hash never covers. It does
// not touch the parcel pool, whose digest IS hashed (kGasSpDigest) — a frozen
// fire must not be able to move the world.
//
// 256 emitters is ~100 simultaneous distant fires' worth of column (a burning
// tree is one or two footprints), and the list is sorted nearest-first, so
// overflow drops the emitters least likely to be on screen. The cap is what
// makes the per-tick cost a constant rather than a function of how much of the
// world has ever been on fire (rule 2).
constexpr uint32_t kGasFarEmitMax = 256;
// Header: [0] = FINE emitter count, [1] = the box SHIFT the fine section was
// built for (kGasOuterShift), [2] = WIDE emitter count, [3] = the wide box
// shift (kGasFarOuterShift). Words 2/3 were reserved when the fine half
// landed, for exactly this: a second, coarser density box consuming the same
// buffer and the same record shape without either side guessing which scale
// the other meant.
constexpr uint32_t kGasFarEmitHdr = 4;
// Per record: world voxel x, y, z (i32; y is the TOPMOST hot voxel in the
// column) then the strength word: bits 0..23 the hot-voxel count (a column's,
// or the wide list's SUM of columns), bits 24..31 the CROSSFADE WEIGHT 0..255
// (see kGasFarBlendVox). WORLD VOXELS, never cell coords — which is what lets
// one record shape serve two boxes of different cell sizes.
constexpr uint32_t kGasFarEmitStride = 4;
constexpr uint32_t kGasFarEmitStrengthMask = 0xFFFFFFu;
constexpr uint32_t kGasFarEmitWeightShift = 24;
// THE CROSSFADE SHELL (2026-09-19). The two lists used to be split STRICTLY at
// the fine box's half-extent, per EMITTER — and a burning tree is several
// emitters (2x2 columns per chunk, several chunks), so as it crossed 409.6 m
// its columns crossed one at a time: the fine plume lost columns and SHRANK
// while the wide aggregate gained them, and the player saw the trail collapse
// to a wisp before the coarse blob appeared. Now an emitter whose max-norm
// distance is within this many voxels INSIDE the fine box's face feeds BOTH
// lists, with complementary weights that the kernels multiply into the mass:
// the fine plume fades out over the shell at full size, the wide one fades in
// at full size, and the sum of what a ray collects is the same at every point
// of the shell.
//
// THE SHELL IS THE WHOLE FINE BAND (2026-09-19, second pass). It was one wide
// cell (64 vox = 6.4 m) out of a 25.6 m band, which is a crossfade only in the
// sense that a hard edge with a 6 m bevel on it is a crossfade: the player
// still saw the coarse representation arrive over about two paces. The band an
// emitter can be in the fine list at all is (windowHalf, fineHalf] = (256, 512]
// voxels of max norm, so the shell is now 256 voxels and the handover occupies
// ALL of it — an emitter is full-fine at the window face, 50/50 at 38.4 m, and
// full-wide at the fine box's face. The weight curve is a SMOOTHSTEP, not the
// linear ramp it was: a linear weight has a slope discontinuity at each end of
// the shell, and a slope discontinuity in a plume's brightness against fog is
// visible as the ring the shell exists to remove. The smoothstep's flat ends
// are also what keeps the coarse twin from mattering in the near field — at
// 30 m it is 8% of the mass, drawn at 6.4 m cells, under a fine plume at 92%.
constexpr int32_t kGasFarBlendVox = 256;  // the whole fine band; pinned below
// ...and the far end of the wide list is a shell too. The wide list's outer
// edge is min(render.farPlumeRange, the box's own half-extent), and until now a
// plume simply stopped existing one voxel past it. Nothing lies beyond to fade
// INTO, so this fades to nothing — which is the same thing fog does to it a few
// hundred metres earlier and therefore reads as "it faded out", not "it went
// out". 512 voxels = 51.2 m, one window, at a distance where that subtends
// very little.
constexpr int32_t kGasFarRangeFadeVox = 512;
// BOTH SHELLS ARE MEASURED FROM THE EYE, NOT THE WINDOW CENTRE, and these two
// are what makes that safe and cheap.
//
// The centre is the right origin for deciding WHICH LIST an emitter is in (it
// is exactly the half-extent at every point of all six faces, so the handover
// radius does not depend on where the camera stands). It is the wrong origin
// for a WEIGHT, because it only moves when the window SHIFTS — one chunk,
// 1.6 m, `Stream::ShiftAxis` — so a weight keyed on it walks down a 256-voxel
// shell in sixteen steps of ~6% each, one per chunk boundary crossed. That is
// the "it changes in discrete steps as I travel" report. The eye moves every
// frame, so a weight keyed on IT is continuous.
//
// THE SLACK is what keeps the two origins from contradicting each other. The
// weight must already be 0 by the time the CPU drops the record, or the plume
// pops out at whatever weight it still had. Max-norm triangle inequality:
// eyeDist >= centreDist - |eye - centre|, so ending every ramp this far short
// of its list boundary is a proof rather than a margin — provided the bound
// holds. Stream recentres when the player is 2 chunks off centre and shifts
// ONE AXIS PER FRAME, so |eye - centre| is ~3-4 chunks in steady state; 6
// chunks is headroom for the diagonal case where two axes are waiting their
// turn. Outrun it (a teleport, a debug fly at absurd speed) and the fade
// degrades to the step it used to be — never to a wrong plume.
constexpr int32_t kGasFarEyeSlackVox = 96;   // 6 chunks
// How far the eye must move to earn a rebuild. Build is otherwise called and
// returns immediately; this is what stops a walking player from rebuilding and
// re-uploading the list on all 60 ticks a second. 2 voxels is 0.8% of the
// shell — below the quantisation of the 0..255 weight byte it feeds.
constexpr int32_t kGasFarEyeStepVox = 2;
// The WIDE section (the long-range box below). Its own cap rather than a share
// of the fine one: the two lists cover disjoint distance bands and a frame with
// 256 near fires should not be able to starve the far ones.
constexpr uint32_t kGasFarEmitMaxWide = 256;
constexpr uint32_t kGasFarEmitWideBase =
    kGasFarEmitHdr + kGasFarEmitMax * kGasFarEmitStride;   // 1,028
constexpr uint32_t kGasFarEmitWords =
    kGasFarEmitWideBase + kGasFarEmitMaxWide * kGasFarEmitStride;  // 2,052 u32 = 8.2 KiB
// A column footprint is one gasOuter cell in x/z by one fine CHUNK in y, so the
// most hot voxels one emitter can stand for is (1<<kGasOuterShift)^2 * kChunk.
// The shader divides by it, so it is a constant both sides must agree on.
constexpr uint32_t kGasFarEmitStrengthMax =
    (1u << kGasOuterShift) * (1u << kGasOuterShift) * kChunk;   // 1,024
static_assert(kChunk % (1u << kGasOuterShift) == 0,
              "a fine chunk must tile the gasOuter cell in x/z, or an emitter "
              "column would straddle two cells and the CPU aggregation would "
              "not match the cell the shader splats into");

// ---- THE LONG-RANGE BOX: fires past the gasOuter box still smoke -----------
//
// gasOuter spans exactly two window edges — ±51.2 m — which is a fraction of
// what the cascade DRAWS. A fire 200 m away is plainly visible as frozen orange
// in the far field and has nowhere to put its smoke. So there is a second,
// coarser density box, identical in every respect except its cell size.
//
// WHY 64 VOXELS (6.4 m) PER CELL, i.e. shift 6. The box is 128 cells per axis
// like its sibling, so the edge is 128 << 6 = 8,192 voxels = **exactly far
// cascade LEVEL 4's box edge** (`kFarN << (4 + kFarShiftBase)`, which reduces
// to 16 * kWorldN because the cascade's alignment constant makes
// `kFarN << kFarShiftBase == kWorldN`). That is the number this cell size is
// chosen FOR: ±409.6 m is the band where a fire is still a distinct shape
// rather than a fogged smear — levels 5..8 sit behind the pinned fog at ~90%
// and up — and matching a cascade box edge means the plume LOD boundary and
// the terrain LOD boundary are the same distance instead of two rings. At the
// far edge a 6.4 m cell still subtends ~16 px at 1080p, so the coarseness is
// under the pixel budget rather than over it. Same 4 MiB as gasOuter.
constexpr uint32_t kGasFarOuterN = 128;
constexpr uint32_t kGasFarOuterShift = 6;
// ...IN X AND Z. The cell is ANISOTROPIC (2026-09-19): 64 voxels across but only
// 8 tall — the fine box's own height cell. A 51.2 m-TALL cell was the "ground
// under the trees is covered in smoke" report: a plume whose base sits at
// canopy height lands in a cell that reaches the ground, and the sampler's
// filter then bleeds it a cell further down. Nothing in the vertical needed
// 51.2 m: the box's job in y is the same ±409.6 m the fine box covers (fires
// are on the terrain, plumes climb tens of metres), so 128 cells of 8 voxels
// span exactly that and the x/z reach is untouched. Same 4 MiB. It also makes
// a plume the same number of height cells in both boxes, so the wide column is
// no longer one cell tall against the fine box's four.
constexpr uint32_t kGasFarOuterShiftY = 3;
static_assert(kGasFarOuterShiftY == kGasOuterShift,
              "the wide box's height cell IS the fine box's, so a plume is the "
              "same number of height cells in both");
static_assert(kGasFarOuterN << kGasFarOuterShiftY == 2 * kWorldN,
              "the long-range box's VERTICAL span is the fine box's: ±kWorldN "
              "around the window centre");
constexpr uint32_t kGasFarOuterCells =
    kGasFarOuterN * kGasFarOuterN * kGasFarOuterN;
constexpr uint32_t kGasFarOuterWords = kGasFarOuterCells / 2;  // 4 MiB, two u16/word
static_assert(kGasFarOuterN << kGasFarOuterShift == 16 * kWorldN,
              "the long-range box must span exactly far cascade level 4's box "
              "edge — see the paragraph above; both the splat and the sampler "
              "derive their origin from that identity");
// The box is centred on the window, so its min corner sits this far below the
// window's. Unlike gasOuter's, the origin is then FLOORED to the cell size: the
// window origin is a multiple of 16 voxels and the cell is 64, so without the
// floor the whole cell lattice would slide 1.6 m sideways every window shift
// and a settled plume would visibly re-quantise as the player walked. Flooring
// costs up to 48 voxels of off-centreness out of ±409.6 m and buys a lattice
// fixed in WORLD space.
constexpr int32_t kGasFarOuterOffsetVox =
    (int32_t)((kGasFarOuterN << kGasFarOuterShift) / 2) - (int32_t)(kWorldN / 2);
static_assert(kGasFarOuterOffsetVox % (int32_t)(1u << kGasFarOuterShift) == 0,
              "the centring offset must itself be a whole number of cells, or "
              "flooring the origin would not keep the lattice world-fixed");
// The y offset: with the vertical span equal to the fine box's, this is
// kWorldN/2 — the wide box's y origin IS the fine box's y origin, and the floor
// to an 8-voxel cell is a no-op on a window origin that is a multiple of 16.
constexpr int32_t kGasFarOuterOffsetVoxY =
    (int32_t)((kGasFarOuterN << kGasFarOuterShiftY) / 2) - (int32_t)(kWorldN / 2);
static_assert(kGasFarOuterOffsetVoxY % (int32_t)(1u << kGasFarOuterShiftY) == 0,
              "the y centring offset must be a whole number of y cells");
// The most fine emitters one WIDE record can aggregate, for the shader's
// normalisation: a coarse cell is (1 << (wide - fine)) fine cells per axis in
// x/z, and a fine emitter spans one chunk in y, so a coarse cell holds
// (ratio^2 in x/z) x (cell/chunk in y) fine columns.
constexpr uint32_t kGasFarWideRatio = 1u << (kGasFarOuterShift - kGasOuterShift);
// The shell is exactly the band an emitter can be in the fine list: from the
// window face (where the fine list starts, because a resident chunk's fire is
// drawn by the CA instead) to the fine box's face. Wider than that and the
// weight would still be climbing when the fine list drops the emitter, i.e. a
// step; narrower and the handover is a bevelled edge again.
static_assert(kGasFarBlendVox == (int32_t)kWorldN - (int32_t)(kWorldN / 2),
              "the crossfade shell spans the whole fine-emitter band "
              "(see kGasFarBlendVox)");
static_assert(kGasFarBlendVox % (int32_t)(1u << kGasFarOuterShift) == 0,
              "...and a whole number of wide cells across");
static_assert(kGasFarOuterShift > kGasOuterShift,
              "the long-range box must be COARSER than gasOuter");

// ---- THE FLAME BOX: what a fire outside the window is MISSING -------------
//
// The plume system above gives a frozen fire its SMOKE back. It does not give
// it back its FLAME, and the reason is structural rather than an oversight:
// `fire` is CLASS_GAS, and a gas is never written to the far cascade at all.
// So outside the residency window a fire is a bed of frozen ember/lava voxels
// with nothing above it, and the flame VOLUME — the thing that actually reads
// as fire — springs into existence the moment the chunk streams back in. That
// is the "fire voxels pop in when you get close enough" report.
//
// This box is the flame, synthesized from the SAME FarPlumes emitter list the
// smoke comes from, by the same two kernels' worth of code one LOD apart. Its
// contents are an EMISSIVE density, folded by the raymarcher into the one
// fire-glow accumulator CA fire already uses — so the far flame takes the same
// temperature ramp, the same breath and the same tonemap as the near one, and
// the seam at the window face is a crossfade rather than a switch.
//
// ONE BUFFER, TWO GRIDS, and that is a deliberate saving rather than a packing
// trick. A grid per LOD would be two more storage bindings in BOTH the gas
// group and the render group, two clear rows and two pass-table buffer ids,
// for two arrays that are always written together and always read together.
// Concatenating them costs one offset constant, and the offset is a whole
// number of words so neither grid's u16 pairing is disturbed.
//
//   [0, kGasFlameFineWords)                 the FINE grid  — gasOuter's
//                                           geometry EXACTLY (origin, cell,
//                                           index), so the sampler is the same
//                                           expression with a different base
//   [kGasFlameWideBase, kGasFlameWords)     the WIDE grid  — gasFarOuter's
//                                           geometry exactly, same argument
//
// "Exactly" is load-bearing: the splatter and the sampler agreeing on where
// cell (0,0,0) is IS the interface, and reusing a geometry that two kernels
// and two sampler functions already agree on is how this feature avoids
// inventing a third lattice that could drift from the other two.
constexpr uint32_t kGasFlameFineWords = kGasOuterWords;      // 1 Mi u32 = 4 MiB
constexpr uint32_t kGasFlameWideBase = kGasFlameFineWords;
constexpr uint32_t kGasFlameWideWords = kGasFarOuterWords;   // 1 Mi u32 = 4 MiB
constexpr uint32_t kGasFlameWords = kGasFlameFineWords + kGasFlameWideWords;
static_assert(kGasFlameWideBase * 2u % 2u == 0,
              "the wide grid must start on a WORD boundary or its two-u16 "
              "packing would be half a cell out of phase with its index");

// The flame column is SHORT — render.farFlameHeight defaults to 4 m against
// the plume's 28 — so it needs far fewer height steps than FAR_PLUME_STEPS,
// and the workgroup is sized to it. One thread per height cell, as the plume
// kernels are.
constexpr uint32_t kGasFlameSteps = 16;

// THE ANTI-CARRY PROOF, and it is this box's version of the one at
// FAR_PLUME_ADD_MAX rather than a copy of it. Two u16 counts share a word, so
// a count that overflows 0xFFFF carries into its neighbour and paints a cell
// that has no fire anywhere near it. The bound is the same shape: one emitter
// reaches a given cell at most once per height step (the kernels merge
// same-cell puffs before they add, exactly as the plume kernels do), so the
// worst case is every emitter in a section landing on one cell.
//
// Deliberately TIGHTER than the plume's. A flame is a small, bright, local
// thing: its whole job is to saturate at its core and be gone a few metres
// away, so it needs neither the plume's 240-per-add headroom nor its 4,096
// ceiling, and a lower ceiling buys a bigger safety margin under the same u16.
constexpr uint32_t kGasFlameAddMax = 160;
constexpr uint32_t kGasFlameCeil = 2048;
static_assert(kGasFlameCeil + (kGasFarEmitMax - 1u) * kGasFlameAddMax <= 0xFFFFu,
              "fine flame counts must not carry into the neighbouring u16");
static_assert(kGasFlameCeil + (kGasFarEmitMaxWide - 1u) * kGasFlameAddMax <=
                  0xFFFFu,
              "wide flame counts must not carry into the neighbouring u16");

// gasSpawn / gasSpawnOps header words. The buffer is an 8-word header followed
// by Particle-shaped records; the header doubles as this tick's gas counters,
// which is free because the whole buffer is cleared before the CA runs.
// Must match GAS_SP_* in sim_gas.wgsl and sim_step.wgsl.
enum : uint32_t {
  kGasSpCount = 0,     // gasLeave append cursor (may exceed kGasSpawnPerTick)
  kGasSpRefused = 1,   // gasLeave refused: the per-tick list was full
  kGasSpEdge = 2,      // gas voxels whose intent pointed out of the window
  kGasSpPoolFull = 3,  // spawn refused: the particle pool was full
  kGasSpReenter = 4,   // particles that became voxels this tick
  kGasSpDied = 5,      // decay / outer box / ceiling
  kGasSpAbove = 6,     // live particles above the window's top face
  kGasSpLive = 7,      // live particles after integrate
  // The gas population's DETERMINISM DIGEST: the sum of every surviving
  // parcel's particlePriority. A sum because it must be ORDER-INDEPENDENT —
  // append order in the pool is scheduling-dependent by construction, so a
  // digest that depended on it would report a false divergence every run. It
  // covers position and payload, which is the whole of a gas parcel's state
  // (velocity is always zero). Outside the window a parcel touches no voxel,
  // so the world hash cannot see it and this is the only thing that can.
  kGasSpDigest = 8,
  kGasSpHdr = 16,      // first record word
  kGasSpStride = 8,    // u32 per record (a 32-byte Particle)
  kGasSpHdrBytes = kGasSpHdr * 4,
};

// One CPU-authored gas spawn. Same 32-byte record the GPU list holds, so the
// two streams are drained by one kernel: position is 24.8 with a ZERO fraction
// (a gas parcel lives ON a cell), velocity is zero, and the flags are forced
// GPU-side. `WorldVoxel` builds one from a cell.
struct GasSpawnOp {
  int32_t px = 0, py = 0, pz = 0;   // 24.8 fixed voxels
  int32_t vx = 0, vy = 0, vz = 0;   // always zero for gas
  uint32_t payload = 0;             // bits 0..11 material, 12..15 state
  uint32_t flags = 0;               // forced to PFLAG_ALIVE|PFLAG_GAS on the GPU
};
static_assert(sizeof(GasSpawnOp) == 32, "GasSpawnOp is a Particle record");
inline GasSpawnOp MakeGasSpawn(int32_t cx, int32_t cy, int32_t cz,
                               uint32_t mat, uint32_t state = 0) {
  GasSpawnOp o{};
  o.px = cx << 8;
  o.py = cy << 8;
  o.pz = cz << 8;
  o.payload = (mat & 0xFFFu) | ((state & 0xFu) << 12);
  return o;
}

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
// 512: a spell flight is up to ~20 sprites (core, lobes, tail, orbit) and
// up to maxLiveProjectiles fly at once, plus impact flashes and markers.
// 16 KiB, CPU-written once a frame.
constexpr uint32_t kMaxSprites = 512;

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
// bits 0..11 material, 12..15 state, 16..18 tick-stamp, 19..23 excite scratch,
// 24..27 stain amount, 28..30 stain type, 31 kCellOpIfAir (never stored).
//
// THERE ARE NO FREE BITS. Every span above is spoken for; bits 19..23 in
// particular are NOT free despite what this comment said until 2026-08-30 (see
// the excite-scratch block below). Anything needing new per-voxel state must
// either reinterpret a span it already owns — the way MATF_TINTED reinterprets
// the state nibble — or go to a sparse auxiliary layer (CLAUDE.md).
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

// ---- the excite scratch span (NOT free) ------------------------------------
// Bits 19..23 belong to the MPM excite/settle seam. EXACT mirror of
// EXCITE_PEND_BIT / EXCITE_DEPTH_SHIFT / EXCITE_SCRATCH_BITS in common.wgsl,
// which is the file the shaders see; this is for the CPU paths that build voxel
// words and must not scribble on them. exciteDetect sets bit 19 and stashes a
// capped hydrostatic depth in 20..23; exciteEmit consumes the mark the SAME
// tick, so the span is per-tick scratch and never persists.
//
// This comment used to read "unallocated / FREE", which it had not been since
// the excite seam landed. The disagreement was live for months and is exactly
// the "two places must agree" class CLAUDE.md keeps a checker for — the voxel
// word allocation must agree here, in common.wgsl's voxMat block, and in
// CLAUDE.md's table.
//
// WHY IT MATTERS THAT THESE STAY OUT OF THE HASH. The excite seam's own safety
// argument is that the bits are excluded from the world hash (sim_occupancy.wgsl)
// and stripped on save (kPersistMask in stream.h), and that no kernel runs
// between detect and emit — so a half-finished mark can never leak into hashed
// state. Widening either mask to claim these bits for durable state DELETES
// that argument. `sim.exciteMode` defaults to 0, so nothing in the current test
// set would catch the breakage; it would surface later as nondeterministic
// water. Per-voxel colour went to the state nibble instead (MATF_TINTED).
constexpr uint32_t kExciteScratchBits = 0x00F80000u;

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

// ---- the OPENNESS (sky-visibility) grid (docs/PLAN_gi.md §2, W3 P0) --------
// RENDER-ONLY DERIVED DATA, exactly like the shadow cache and the far-field
// cascades: never hashed, never saved, the sim has no binding for it, and a
// determinism hash that moves when this changes is a bug in this code rather
// than a rebaseline.
//
// WHAT IT HOLDS. One byte per (chunk SLOT, 4^3 sub-occupancy block, face):
// 0..255 = the unblocked fraction of that face's hemisphere within
// `render.opennessReach` metres, measured by marching the BLOCKERS-class 4^3
// mask (the same mask traceOpaque's coarse mode reads). `ambientAt` scales the
// hemisphere ambient by it, so a cave floor stops being lit as brightly as a
// meadow. Nothing finer would fit: a per-VOXEL byte is 134 MB in-window before
// faces, and the voxel word has no spare bits (the kExciteScratchBits note).
//
// SIX VALUES PER BLOCK, NOT ONE. This world is full of one-voxel walls, floors
// and trunks; a scalar per block would average the lit side of a wall with the
// dark side. A receiver reads only the face that faces it, which is the same
// (axis, sgn) pair the shadow cache already keys on.
//
// WHY A SEPARATE BUFFER, unlike the sub-occupancy mask which rides in the tail
// of `occupancy`: the mask is written by every producer of the occupancy
// counts and read by everything that reads them, so one buffer is one hazard.
// This grid is written by ONE pass and read by the render path only; folding it
// into `occupancy` would put a 12 MiB render-only payload behind every sim
// barrier that touches occupancy, and make `--gate openness`'s readback a
// readback of the world's occupancy.
//
// LAYOUT is byte index ((slot * blocks + block) * 6 + face), viewed through an
// array<u32>. 6 bytes per block does not divide a word, which is deliberate:
// the WRITER owns one WHOLE WORD per thread (4 consecutive (block, face) pairs,
// which may straddle a block boundary) and stores it with a plain store. The
// alternative — a thread per (block, face) — would be a read-modify-write of a
// word three other threads are also writing, i.e. a race, and the only cures
// are atomics or 25% padding. A packing that costs a `% 6` in the writer and
// two shifts in the reader is cheaper than either.
constexpr uint32_t kOpenFaces = 6;
constexpr uint32_t kOpenBlocksPerChunk = kSubOccDim * kSubOccDim * kSubOccDim;
constexpr uint32_t kOpenBytesPerChunk = kOpenBlocksPerChunk * kOpenFaces;
static_assert(kOpenBytesPerChunk % 4 == 0,
              "openness bytes per chunk must be a whole number of words");
constexpr uint32_t kOpenWordsPerChunk = kOpenBytesPerChunk / 4;
constexpr uint64_t kOpennessBytes =
    (uint64_t)kNumSlots * kOpenWordsPerChunk * 4;           // 12 MiB at 512^3
// THREE PLANES, not one (2026-09-05, PLAN_frame_perf.md §3 item 4):
//   [0, kNumChunks)              the world-chunk STAMP per slot (below)
//   [kNumChunks, 2 kNumChunks)   the tick of the slot's last FULL walk
//   [2 kNumChunks, + kNChunk^2)  per slot COLUMN (x, z): the tick something
//                                within opennessReach of that column last
//                                changed geometry (a dirty walk, or a chunk
//                                arriving in a slot with a stale stamp).
// The rolling refresh skips the five-ray march for a slot whose stamp matches
// and whose column has not been touched since its last full walk, keeping only
// the irradiance maintenance (sun re-sample / decay) that must not stop. The
// DESIGN.md note that "dilating the dirty list is not available" still holds:
// this is 17x17 column STAMPS per dirty chunk, not 15^3 chunk WALKS. WGSL
// mirrors the layout as OPEN_WALKED_BASE / OPEN_TOUCH_BASE (common.wgsl).
// The first two planes are PER SLOT (kNumSlots, so ticket slots get a stamp and
// a walk tick like any other). The third is per window COLUMN and stays
// kNChunk^2: a ticket has no place in the window's (x, z) column grid, and the
// refresh cursor that reads it only ever walks window slots.
constexpr uint32_t kOpennessGenWords = 2 * kNumSlots + kNChunk * kNChunk;
constexpr uint64_t kOpennessGenBytes = (uint64_t)kOpennessGenWords * 4;   // 260 KiB

// ---- the IRRADIANCE grid (docs/PLAN_gi.md §3, W3 P1) -----------------------
// One u32 per (slot, 4^3 block, face) — the SAME identity the openness byte
// has, so the two grids share a stamp (`opennessGen`) and a face encoding —
// holding the RGB9E5-packed radiance LEAVING that block-face: albedo × sun ×
// lambert × lit, averaged over the patches the shadow resolve pass has seen
// there. Render-only derived data with exactly the openness grid's standing:
// never hashed, never saved, the sim has no binding for it, and it is written
// by two render-path passes (shadow_resolve.wgsl on publish, sim_openness.wgsl
// on the dirty/refresh walk) and read by the raymarch's one-bounce gather.
//
// WHY ONE PACKED WORD AND NOT SUM + COUNT. An atomic sum needs a count to
// divide by, and 256 patches per block-face per frame (16 voxel faces × 16
// patches at subdiv 4) overflow any bounded count in a handful of frames; a
// count that resets per frame needs a frame stamp the word has no room for.
// The writer instead blends toward each new sample (an EMA, sim_openness.wgsl
// says the rates), which needs nothing but the value, converges within a frame
// where deposits are dense and is bounded by construction. RGB9E5 rather than
// three fixed-point lanes because the sun-lit day value and a moonlit night
// value differ by ~400x and the tint of the bounce is the whole point.
// TWO PLANES since 2026-09-05 (PLAN_frame_perf.md §3 item 1). The first is
// the OUTGOING radiance above. The second, at the same index + GI_CACHE_BASE
// (common.wgsl), is the GATHERED INCOMING irradiance at the block-face's
// centre: what `giGather`'s nine rays return, cached so the raymarch reads one
// word per hit and re-gathers a chunk slot's faces only on its scheduled frame
// (render.giCachePeriod) or when a word is 0 ("never gathered" -- a gathered
// zero is stored with its low bit set). Same stamp, same writers' standing:
// the walk zeroes it for a slot the window reused, the raymarch fills it.
constexpr uint32_t kIrradiancePlanes = 2;

// NO MIP PYRAMID OVER PLANE 0 (tried and removed, 2026-09-11). A gather ray
// that reaches ~12 m wants a prefiltered cell at the far end rather than a
// point sample of one 40 cm block-face, and three chunk-local levels were
// built for exactly that -- then deleted, because READING them cost the
// raymarch fragment shader 128 -> 168 registers (--shader-stats), which is the
// wrong side of its occupancy cliff, and slowed --render-budget cameras with
// no indirect light in them at all. The gather's reach came from its STEP
// BUDGET instead, which is free in registers. assets/shaders/raymarch.wgsl's
// giGatherRays carries the full measurement and says what would have to change
// (move the gather to a compute pass) before trying again.
constexpr uint64_t kIrradianceBytes =
    (uint64_t)kNumSlots * kOpenBlocksPerChunk * kOpenFaces * 4 *
    kIrradiancePlanes;   // 96 MiB at 512^3

// ---- the GLOW field (docs/PLAN_glow.md; assets/shaders/sim_glow.wgsl) ------
// A coarse, world-space, POSITION-KEYED field of the light emitters are
// throwing into the space around them. Render-only derived data with exactly
// the openness grid's standing: never hashed, never saved, the sim has no
// binding for it, rebuilt from the voxels on load.
//
// WHY IT EXISTS, given the irradiance grid already carries emission. The
// irradiance grid is keyed on a block FACE and is only readable through
// `giGather` — nine coarse DDA rays that reach TUNE_GI_GATHER_BLOCKS (3) blocks
// = 1.2 m, and that need `voxels`, `occupancy` and `pageTable` bound in the
// FRAGMENT stage. Neither is available to the raster body paths: debris.wgsl
// shades a rigid-body cube in the VERTEX stage, where renderBGL_ marks voxels
// and pageTable Fragment-only, and it cannot afford nine rays per vertex
// either. So a burning crown, a mob standing in a lava pit and a severed limb
// beside a fire are all lit by the sky alone — the emitter half of
// docs/PLAN_rigidbody_lighting.md's gap. This field answers "how much emitter
// light is at this POINT" with ONE buffer read and no ray, which is the only
// shape of answer those paths can consume.
//
// LAYOUT. Two regions in one buffer, so it costs ONE binding in each of the two
// bind-group layouts that carry it rather than two.
//
//   SRC   [slot * 4 + 0] the chunk's own emitted radiance, RGB9E5
//         [slot * 4 + 1] the world-chunk stamp (OpennessStamp — SAME hash)
//         [slot * 4 + 2] how many emitting cells it holds (diagnostic; the
//                        `glow` gate reads it, and "0 emitters" is a different
//                        failure from "emitters but no field")
//         [slot * 4 + 3] 1 if word 0 moved on this visit (the ring trigger)
//   FIELD [kNumChunks * 4 + slot * kOpenBlocksPerChunk + block]
//         the incident glow at that 4^3 block, RGB9E5
//
// GRANULARITY. The SOURCE is per CHUNK (1.6 m) and the FIELD is per 4^3 BLOCK
// (40 cm) — the same block grid openness and the sub-occupancy mask already
// index, so the reader reuses `subOccBitLocal` and adds no indexing math and no
// prelude constant. Quantising the source to a chunk is what makes the producer
// affordable: the field at a block is a 27-tap gather over a 512 KiB source
// region, not a search over voxels. Quantising the FIELD to a chunk as well
// would have been 128 KiB total, but a 1.6 m light blob reads as a cube; the
// falloff has to be evaluated at a finer grid than the source for the result to
// look like light. 40 cm blocks × 32,768 slots = 2,097,152 words = 8 MiB.
//
// RGB9E5, not voxelbit's four bits. Three reasons and only the first is size:
// `packRgb9e5`/`unpackRgb9e5` already exist for the irradiance grid, so there
// is no second packing convention to keep in agreement; the HDR exponent means
// a lava lake and one ember differ by three orders of magnitude with no global
// scale constant to tune; and it carries COLOUR, so lava is orange and
// gem_arcane is violet — which a scalar cannot express and voxelbit works
// around by testing the emitter's Y coordinate.
constexpr uint32_t kGlowSrcWordsPerSlot = 4;
constexpr uint64_t kGlowSrcWords =
    (uint64_t)kNumSlots * kGlowSrcWordsPerSlot;                    // 512 KiB
constexpr uint64_t kGlowFieldWords =
    (uint64_t)kNumSlots * kOpenBlocksPerChunk;                     // 8 MiB
constexpr uint64_t kGlowBytes = (kGlowSrcWords + kGlowFieldWords) * 4;
// The WGSL side derives this as `NUM_CHUNKS * 4u` from constants the prelude
// already emits, so the field needs no new prelude constant and no
// ShaderConstantPrelude / check_shaders.sh edit. This mirror exists for the
// gate's readback arithmetic only.
constexpr uint64_t kGlowFieldBaseWord = kGlowSrcWords;

// ---- THE REPOSE OCCUPANCY SNAPSHOT (assets/shaders/sim_step.wgsl) ---------
//
// One bit per voxel: "a powder could drop into this cell", i.e. the cell holds
// neither a solid nor a powder. Written by `reposesnap`, a prepass recorded
// BEFORE the CA's 54 substep dispatches over the SAME compacted dirty list the
// CA uses, and read by the angle-of-repose stage in sim_step.wgsl.
//
// WHY IT HAS TO EXIST, in one paragraph, because it is the only licence in this
// engine for a CA read past distance 1. The 3x3x3 colour lattice keeps acting
// cells >=3 apart and bounds writes to <=1 cell, so the 3x3x3 write boxes of
// same-colour cells TILE SPACE: every cell in the world is inside exactly one
// acting cell's box. A read at distance <=1 is inside mine and only I can write
// it; a read at distance >=2 is inside somebody else's box, and whether it
// shows the pre-pass word or their write is decided by GPU scheduling. Per-
// material repose needs to see 2 and 3 cells out (a 2:1 face and a 1:1 face are
// locally IDENTICAL — the difference is at distance 2, which is a fact about
// the lattice, not about the rule). So the answer is read from a snapshot of
// the world at TICK START, which is the same in every run of the same tick,
// instead of from live voxels.
//
// DELIBERATELY STALE. A drop that fills in mid-tick still reads open, the grain
// slides toward it, the ordinary tryMove refuses, and next tick's snapshot says
// so. Staleness is bounded by one tick and is identical in every run, which is
// the only property rule 1 asks for.
//
// THE SET IT COVERS is each dirty chunk plus the NINE neighbours a probe can
// reach -- not the 3x3x3 block's 26 -- on two one-line facts about the probe
// offsets in sim_step.wgsl: every probe has dy <= -1 (nothing looks up), and no
// probe offsets both lateral axes. See reposeRingOffset there, which owns the
// table and the obligation that comes with it.
//
// A SIDE TABLE, per design guideline 2: the voxel word is full, and this is
// derived data — not hashed, not saved, rebuilt every tick for exactly the
// chunks that could be read.
//
// IT IS NOT A SPARSE ONE, and calling it sparse would be the wrong word for the
// price. The ALLOCATION is DENSE: one bit for every cell in the window, all
// 16.125 MiB resident from World::Init, about +36% on the page pool's own
// resident bytes, paid by any world whose materials.json carries a `repose`
// line. What is sparse is the WRITING — only the chunks a probe can reach are
// refilled, so a settled world touches none of it. A genuinely sparse form
// (bits only for chunks holding a repose material) would trade that flat cost
// for an indirection in the CA's hottest read; at one bit per voxel the dense
// form is small enough that the trade is not worth making, but it IS a trade. Indexed by the SLOT cell index (cellIndexW), so
// word = idx >> 5 and bit = idx & 31, which is why the bitfield is exactly
// kVoxelCount/32 words with no per-chunk arithmetic anywhere.
//
// PER-SLOT VALIDITY, and it is required rather than belt-and-braces: the
// residency window is toroidal, so a slot is reused by a new world chunk as the
// window walks, and a probe that read the previous occupant's bits would be
// reading another place in the world. Each slot carries `tick + 1` in the tick
// region (0 is therefore never a valid stamp, which is what makes the zeroed
// allocation cold-start correctly) and a probe into a slot whose stamp is not
// this tick's REFUSES. Nothing finer is needed: the window origin is constant
// for the whole of one command buffer, so a slot cannot change occupant between
// the prepass and the CA rows of the same tick.
constexpr uint64_t kReposeSnapBitWords = kVoxelCount / 32;       // 16 MiB at 512^3
constexpr uint64_t kReposeSnapTickBase = kReposeSnapBitWords;    // + kNumChunks stamps
constexpr uint64_t kReposeSnapWords = kReposeSnapBitWords + kNumChunks;
constexpr uint64_t kReposeSnapBytes = kReposeSnapWords * 4;      // 16.125 MiB
// The WGSL side derives both from constants the prelude already emits
// (NUM_CHUNKS * CHUNK_VOL / 32u), so this needs no new prelude constant; the
// mirrors exist for the allocation and for the gate's arithmetic.
static_assert(kVoxelCount % 32 == 0, "repose snapshot is 1 bit per voxel");

// The residency window is toroidal, so a slot is reused by a new chunk as the
// window walks. Its 384 openness bytes then describe geometry that is no longer
// there, and the reader has no way to tell — the grid is not dirty-tracked the
// way the voxels are. So every write stamps the slot with a hash of the WORLD
// CHUNK COORD it was computed for, and every read compares. A mismatch reads as
// "unknown" and falls back to the plain n.y hemisphere lerp, which is exactly
// the pre-P0 look — the safe direction, and the same policy the shadow cache's
// verifier uses one level down.
//
// MUST MATCH `opennessStamp` in common.wgsl. `--gate openness` is the check:
// it computes the stamp here and asserts the GPU wrote the same word.
// 0 is reserved for "never written" (the buffer is zeroed at creation), so a
// hash that lands on 0 is bumped to 1.
inline uint32_t OpennessStamp(int cx, int cy, int cz) {
  const uint32_t h = (uint32_t)cx * 73856093u ^ (uint32_t)cy * 19349663u ^
                     (uint32_t)cz * 83492791u;
  return h == 0u ? 1u : h;
}

// ---- the voxel-keyed shadow cache -----------------------------------------
// RENDER-ONLY, DERIVED, DISPOSABLE. Not hashed, not saved, not replicated, and
// the sim never reads it — the same standing as the far-field cascades. A
// determinism hash that MOVES when this changes is a bug in this code, not a
// rebaseline (CLAUDE.md rule 1 scopes to sim state).
//
// WHY IT EXISTS. sunShadowAt cast one full trace() per lit pixel. At
// kVoxelMeters = 0.10 and 1080p/70deg a voxel face spans ~157/d pixels — 31 px
// across at 5 m, 6.5 px at the 24 m fine-march limit — so the same shadow
// answer was being computed 40-1000x per frame. Measured with --render-budget
// on an RTX 3060 Ti, overlook camera, 1920x1080, 2026-09-01:
//
//   baseline  everything on                        14.56 ms
//   noshadow  shadows off, call site resident      12.27 ms   -2.29
//   shadow0   shadowSteps 384 -> 0, const-folded     8.68 ms   -5.88
//
// THE SECOND ROW IS THE INTERESTING ONE AND IT IS WHY THIS IS NOT JUST A CACHE.
// The shadow call site's REGISTER FOOTPRINT is 5.88 - 2.29 = 3.59 ms, 1.6x
// larger than all the traversal it performs. So deduplicating rays while
// leaving an inline fallback in the fragment shader would win a fraction of
// 2.29 ms and forfeit 3.59. The shadow trace() must be ABSENT from the compiled
// fragment shader, which is what makes SHADOW_CACHE a compile-time const and
// not a runtime branch, and what forces the miss policy below.
//
// THE STRUCTURE. One shadow factor per SURFACE PATCH — a voxel face subdivided
// kShadowSubdivMax-ways per axis. A lossy, hash-keyed, 8-WAY SET-ASSOCIATIVE
// table (common.wgsl shadowSetOf): a key picks a set of 8 adjacent slots (one
// cache line), a patch finds its own slot in the set or claims one no LIVE
// patch holds, and a live slot — requested this frame or last — is never
// stolen. A compute pass (shadow_resolve.wgsl) resolves each requested patch
// exactly once per frame; the fragment shader only reads and re-registers.
//
// WHY NOT DIRECT-MAPPED, which is what shipped first. At ~200k patches in 1M
// buckets, ~16% of a frame's patches shared a bucket with another visible
// patch — ~28k of them — and "collisions overwrite" meant each pair evicted
// the other every frame and shaded lit on the frames it lost, mid-frame, in
// fragment order. On screen that was shadow pixels spasming across the ground.
// Identity is 47 bits (32-bit key word + 15-bit verifier in the state word),
// so a same-identity false match is ~1e-4 per frame rather than the ~5
// flickering pairs the 32-bit key alone allowed.
//
// THE READER FILTERS. A patch's value is the answer at its CENTRE, and the
// fragment shader blends the four nearest centres bilinearly (raymarch.wgsl
// shadowCached), rolling into the neighbour cell's same face past an edge.
// That is what turns a 2.5 cm staircase into a one-patch ramp, and it is why
// subdiv 4 is enough. Taps with no resolved value carry weight 0 and the rest
// renormalise; a value that IS resolved is trusted even if a few frames old,
// because it is by construction the right patch's answer and it is recast on
// every frame the patch is on screen. "Lit until resolved" — the first
// version's miss policy — sparkled along every disocclusion fringe.
//
// STALENESS IS ONE FRAME, AND ONLY IN THE REQUEST SET. Values are cast fresh
// every frame, so destruction re-lights immediately and there is NO
// invalidation machinery — no per-chunk generation, no dirty-list coupling. The
// one-frame lag is in WHICH patches were asked for, so a newly disoccluded
// patch has no opinion for one frame and blends from its neighbours.
//
// WHY A COMPUTE PASS IS LOAD-BEARING rather than an optimisation: a fragment
// shader cannot elect one pixel per patch to do the work. Without it, "valid
// only if written last frame" degenerates into hit/miss on alternating frames.
// Declared as a SHIFT with the count derived, not as a bare `1u << 20`:
// scripts/check_shaders.sh scrapes world.h for literals and redoes the
// arithmetic itself, so an expression here would be unscrapable — the same
// reason kSubOccShift/kSubOccWords are the literals and kSubOccStride is
// derived.
constexpr uint32_t kShadowCacheShift = 20;
constexpr uint32_t kShadowCacheBuckets = 1u << kShadowCacheShift;   // 1,048,576
constexpr uint32_t kShadowCacheWords = 2;            // key, then packed state
constexpr uint64_t kShadowCacheBytes =
    (uint64_t)kShadowCacheBuckets * kShadowCacheWords * 4;   // 8 MiB
// Two ADJACENT words per slot, not two parallel arrays, and 8 slots per set:
// 64 bytes, one cache line, so probing a whole set costs about what one bucket
// read did. The key word holds the 32-bit key; the state word holds value,
// two 4-bit frame stamps, the valid bit and the 15-bit verifier
// (common.wgsl shadowPackState).
//
// Load factor at the ~200k patches an overlook frame resolves is under 20%,
// and with the bilinear taps' neighbour ring somewhat above; the Poisson tail
// past 8 live patches in one set is ~1e-5 of patches at 20% load.
// ---- THE PENUMBRA WINDOW (shadow_resolve.wgsl, 2026-09-11) -----------------
// One word per bucket, PARALLEL to the cache and never read by the fragment
// shader: a 16-frame sliding window of the patch's sun-visibility samples, plus
// the lift byte, packed as
//
//   bits 0..15  one bit per sample slot, set when that sample saw the sun
//   bits 16..20 how many slots have been filled since the slot was claimed
//   bits 21..28 the softening lift (0..255) from the last CENTRE ray
//
// WHAT IT BUYS. The cache used to publish ONE ray's verdict, so a patch was
// either lit or shadowed and a shadow edge was a hard step from full sun to
// TUNE_SHADOW_LIFT, one patch wide however far away the blocker was. The sun is
// not a point: with the ray jittered inside a cone each frame and the last 16
// verdicts averaged, a patch's published value is the FRACTION of the disc it
// can see, so the edge becomes a real penumbra that widens with blocker
// distance (owner report: "shadows expand pixels at a time; it should be a
// gradient").
//
// WHY A WINDOW AND NOT AN EXPONENTIAL BLEND. An EMA of a cycling sample set
// never settles — it oscillates with the sequence's period forever, which is a
// pulsing shadow, which is the defect next door. A sliding window over a fixed
// 16-sample sequence is EXACT after 16 frames and then stops moving entirely
// while the scene and the sun hold still: the same slot is rewritten with the
// same bit. --gate shadow-cache's flicker arm asserts that.
//
// WHY IT IS ITS OWN BUFFER rather than two more words on the cache slot: the
// reader probes a whole 8-way set on every lit pixel and that set is exactly
// one 64-byte cache line (see kShadowCacheWords). Widening the slot would put
// the hot path on two lines to carry state only the resolve pass ever touches.
constexpr uint64_t kShadowHistBytes = (uint64_t)kShadowCacheBuckets * 4;  // 4 MiB
constexpr uint32_t kShadowSamples = 16;   // window length; 16 bits of the word
constexpr uint32_t kShadowReqHeaderWords = 4;   // [0] atomic count, [1] saved count, [2..3] stats
constexpr uint32_t kShadowReqWords = 4;         // key, bucket, packed cell, packed face+sub
// 2^20 records (16 MiB), not the 2^18 (4 MiB) it shipped with: a frame's
// request count scales with PIXELS, not with the world — 1080p asks for
// ~180-200k, so 262k was a ceiling 1.3x above the harness and BELOW a
// maximised 1440p or 4K game window. Past the cap the refused set is whatever
// appended last in fragment order, which changes every frame, and a refused
// patch has no opinion, so the failure mode is a shimmer of lit patches that
// never settles. The buffer costs memory only; the resolve pass is sized by
// the count actually requested.
// ---- RENDER_STATS: the raymarch's step counters (measurement only) ---------
// A tiny buffer of atomic<u32> the raymarch fragment shader adds into when the
// RENDER_STATS prelude const is true (--telemetry / --perf; gpu/resources.h).
// Layout: kRenderStatStripes rows of kRenderStatSlots counters. STRIPED so the
// whole screen is not contending on sixteen addresses: a pixel adds into the
// stripe its screen position hashes to, and the CPU sums the stripes. The
// shader records on a 1-in-16 pixel sample (every fourth pixel on both axes)
// and the CPU scales by 16, so a slot reads as a per-frame total. Slot 0 is
// the sampled-pixel count (the denominator); slots 1.. are perfnodes.h's
// PerfCounter::RmPrimarySteps.. in order — raymarch.wgsl's RS_* names are
// that same order and the page's labels come from kPerfCounters, so the ONE
// place the meaning of a slot is written is the counter table.
// MONOTONIC: never cleared. The CPU differences consecutive readbacks in u32
// arithmetic, which removes the clear (and its barrier) from the frame path
// and makes a missed readback a gap rather than a corruption.
// Render-only, never hashed, never saved; the buffer exists whether or not the
// const is true so the render bind group has one layout.
// 19 since 2026-09-05: slots 16..18 are the foliage split (micro cells
// entered, plant evaluations, chunk skips) — raymarch.wgsl RS_MICRO_ENTER..
constexpr uint32_t kRenderStatSlots = 19;
constexpr uint32_t kRenderStatStripes = 64;
constexpr uint32_t kRenderStatWords = kRenderStatSlots * kRenderStatStripes;
constexpr uint64_t kRenderStatBytes = (uint64_t)kRenderStatWords * 4;
constexpr uint32_t kRenderStatSample = 16;   // 1 in 16 pixels; the CPU's scale
constexpr uint32_t kShadowReqCapShift = 20;
constexpr uint32_t kShadowReqCap = 1u << kShadowReqCapShift;  // 1,048,576 per frame
constexpr uint64_t kShadowReqBytes =
    (uint64_t)(kShadowReqHeaderWords + kShadowReqCap * kShadowReqWords) * 4;
// Overflow is GRACEFUL and silent by design: past the cap a patch simply is not
// registered, so it has no opinion next frame and blends from its neighbours.
// It is reported in the stats words rather than being an abort, because the
// failure is a slightly wrong pixel, not a lost voxel.
//
// GRANULARITY IS THE KNOB, exactly as it is for kSubOccShift above, and it is a
// QUALITY knob rather than a speed one. The win saturates early: at subdiv 4
// the ~200k resolved patches cost ~0.34 ms of traversal against a 2.29 ms
// budget, so coarsening further buys almost nothing while making shadow edges
// blockier. 4 gives 2.5 cm patches — ~8 px at 5 m, ~2 px at 20 m. Runtime knob
// is render.shadowCacheSubdiv; this is the ceiling the packing allows.
constexpr uint32_t kShadowSubdivMax = 8;        // 3 bits per axis in the request word
// The request record packs a window-relative cell as three log2(kWorldN)-wide
// fields plus a 3-bit face, all in one u32 (common.wgsl shadowPackCell). At the
// 512 window that is 9+9+9+3 = 30 bits. Doubling the window to 1024 makes it 33
// and the record silently truncates — the same class of overflow the kFarSlot
// note in this file records having already been paid for once, so it is a
// static_assert here rather than a comment there.
constexpr uint32_t kWorldShift = [] {
  uint32_t s = 0;
  while ((1u << s) < kWorldN) s++;
  return s;
}();
static_assert(kWorldShift * 3 + 3 <= 32,
              "shadowPackCell: cell+face no longer fits a u32 at this kWorldN — "
              "widen the request record to two words");
// And the sub-patch indices share the other word at 3 bits each.
static_assert(kShadowSubdivMax <= 8, "shadowPackSub gives each axis 3 bits");

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

// ---- THE PAGE-FAULT RECORD (P3-E) -----------------------------------------
//
// `pageFaults` is not a counter, it is a small fixed-layout record, and the
// layout is stated ONCE here because five places have to agree about it:
// world.cpp (the buffer and the snapshot copy), pagetable.cpp (two zeroing
// sites), support.cpp (the post-worldgen re-zero), selftest.cpp (the report)
// and common.wgsl's voxStore (the writer).
//
//   [0]      total faults, monotonic
//   [1]      max(refusing SLOT + 1)      legacy, 0 = never faulted
//   [2]      max(dropped WORD)           legacy
//   [3]      max(~SLOT) == min slot      legacy
//   [4]      first fault: kernel id + 1 (0 = none), common.wgsl's PT_K_*
//   [5..7]   first fault: refusing WORLD CHUNK x/y/z, as i32 bits
//   [8]      first fault: the dropped word
//   [9]      first fault: the refusing slot
//   [10]     first fault: the TICK it was dropped on
//   [11..15] LAST fault: kernel id + 1, world chunk x/y/z, tick (racy store)
//   [16]     first fault: the page-table ENTRY the resolve read
//   [17]     first fault: the in-chunk local index
//   [18]     LAST fault: the page-table entry (racy store)
//   [19]     reserved
//   [20..32] per-kernel fault tally, indexed by PT_K_*
//
// [16]/[18] are what separate a FREED page (PT_EMPTY) from a DEMOTED one
// (UNIFORM / JITTER): the first is the hysteresis free path, the second is
// Stream's classification. "A sentinel" on its own indicts every suspect.
//
// THE WORLD CHUNK IS RESOLVED IN THE SHADER, at fault time, and that is the
// point of the record existing at all: the slot is a memory ADDRESS, the
// window is toroidal, and decoding a slot at report time with the run's final
// origin names a chunk that has nothing to do with the fault. That decode cost
// an hour of chasing chunks nothing had touched (RESEARCH_streaming_hitch.md
// §6). A slot is an identity only within one origin.
// 40, not 32: the per-kernel tally starts at word 20 and is PT_K_COUNT wide,
// and PT_K_COUNT went 12 -> 13 when sim_gas.wgsl got its own bank. At 32 the
// gas kernel's counter would have been word 32 — one past the end — which the
// GPU would have written into whatever followed the buffer without a word of
// complaint. Sized with headroom for the same reason kMaxUses is.
constexpr uint32_t kPageFaultWords = 40;
constexpr uint32_t kPageFaultBytes = kPageFaultWords * 4;

// ---- fluidArgsStage: the FA_* word map -------------------------------------
// The seam's counter block (common.wgsl's FA_* names, plus the two refusal-site
// counters and the two force-settle counters declared in sim_fluid_seam.wgsl
// itself — a const only one shader reads belongs next to its consumer, and
// adding it to common.wgsl re-keys every shader in the engine).
//
// A NAMED SIZE because it USED to be the literal 128 in four places (the
// allocation, the snapshot copy, the snapshot decode and ReadFluidArgsSync's
// read) and the member comment still claimed "16 u32" long after the map had
// grown to 32. Four literals and a stale comment is exactly the "two places
// must agree" shape that goes wrong silently: a shader writing past the end
// scribbles on whatever the allocator put next.
constexpr uint32_t kFluidArgsWords = 40;
constexpr uint32_t kFluidArgsBytes = kFluidArgsWords * 4;
// The snapshot ring gives this block a fixed 256-byte slot (kFluidArgsOff ..
// kFluidBlocksOff in world.cpp). Growing the map past that would silently
// overwrite the block list that follows it.
static_assert(kFluidArgsBytes <= 256,
              "fluidArgs no longer fits its snapshot-ring slot; widen the "
              "gap between kFluidArgsOff and kFluidBlocksOff first");
// Where the per-kernel tally starts inside the record. Mirrored in
// common.wgsl's voxStore as PT_FAULT_KBASE — the one place a WGSL constant for
// it would have to be threaded through the prelude for no other reader.
constexpr uint32_t kPageFaultKernelBase = 20;

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
// The PROBE-LESS release BURST budget (P4-H). A candidate whose occupancy
// snapshot postdates every tick it could have been written on needs no 16 KiB
// copy to prove it is stainless air - the snapshot already said so - so it is
// released on the spot and never enters the probe queue. This number is what
// ONE TICK may release that way; it is not the sustained rate.
//
// THE SUSTAINED RATE IS kPageRetireCeiling / kPageRetireTicks AND NOTHING
// ELSE, whichever gate authorized the free, because every freed page parks in
// the retire queue for kPageRetireTicks. 2,048 / 16 = 128 pages per tick, and
// raising it costs 16 pages of POOL for each extra page per tick. Measured
// P4-H, --frames 1200 --autofly-surface: the pre-P4-H path achieved 26.0
// frees/tick against that same 128 limit, so the ceiling was never the binding
// constraint - the PROBE was, and it still had 5x of headroom nobody could
// reach. With the probe-less release the measured rate is 111.4/tick, 87% of
// what this pool already allows. Lifting the ceiling to 8,192 (pool +6,144
// pages = +96 MiB) buys 178.1/tick and about 2,000 pages of peak residency;
// that arm is one env var away (SANDVOX_PT_RETIRECAP) and was NOT taken,
// because the pool is a fixed VRAM reservation and the residency it buys is
// headroom against an abort the derivation below already makes unreachable.
constexpr uint32_t kPageFreeDirectPerTick = 384;
constexpr uint32_t kPageRetireTicks = 16;
// UNCHANGED by P4-H, and deliberately so: both free producers park into this
// one queue and share this one ceiling, so the pool derivation below is
// exactly what it was. What DID change is that the bound is now ENFORCED at
// PageTable::ReleasePage (a free that would overflow the queue waits a tick)
// instead of only being asserted in RetirePages. With two producers, "the
// queue cannot exceed the product" is one more reading that has to stay true,
// and a rate limiter that refuses is much cheaper to be wrong about than an
// abort that fires. The assert stays as belt to these braces.
constexpr uint32_t kPageRetireCeiling = kPageFreeProbesPerTick * kPageRetireTicks;

// Physical pages in the pool under --residency paged. DERIVED FROM kWorldN,
// never a literal: one page per chunk slot in the window, plus the retire
// headroom above.
//
// The derivation is what makes exhaustion impossible rather than merely
// unlikely, and the proof is three lines:
//
//   - Alloc() is only ever reached for a SENTINEL slot, so at the moment of a
//     request at most kNumSlots - 1 slots hold a page.
//   - The retire queue holds at most kPageRetireCeiling.
//   - freePages = kPoolPages - resident - retired >= 1. Always.
//
// It is not "headroom for an unbounded demand" — it is a pool that exceeds a
// demand with a hard geometric ceiling. There cannot be a request for page
// kNumSlots + 1 because there is no chunk slot to hand it to.
//
// TICKETS KEEP THE PROOF (docs/PLAN_chunk_tickets.md §2.1) because the first
// line of it counts SLOTS, not window chunks: a ticket slot can hold a page
// exactly as a window slot can, so the demand ceiling moves from kNumChunks to
// kNumSlots and the pool follows it. This is the same "derived, never a
// literal" rule kWorldN already gets — raise kTicketMax and the pool grows with
// it, at 2 MiB per ticket's 125 chunks.
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
constexpr uint32_t kPoolPages = kNumSlots + kPageRetireCeiling;

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
// slot -> colour for something that is not a material id. These entries
// sit just below the stain palette, are filled by Simulation::UploadTables
// from the loaded prefabs' art palettes, and are inert as materials (nothing
// can reference them: ids are assigned from the bottom up).
//
// ---- why this is 255 and voxload.h's kArtPaletteSlots is 128 ----
// They are two DIFFERENT limits and used to be conflated at 128.
//
//   kArtPaletteSlots (voxload.h) = 128 is the PER-FILE limit: a .vox addresses
//   art with palette indices 128..255, because material ids own the bottom half
//   of the same 256-entry palette. One garment cannot exceed it, and none comes
//   close.
//
//   This constant is the MERGED limit across every prefab loaded at once, and
//   it is bounded instead by the art byte in the micro voxel (microbody.h bits
//   8..15) and by MicroBodyMergeArt's uint8_t remap table. Art is stored there
//   as a 1-BASED merged index — 0 means "unpainted, use the material's own
//   colour" — so the ceiling is 255, not 256.
//
// Storing the merged INDEX rather than the .vox SLOT is what buys the extra
// range: the old encoding wrote slot = 128 + index into the same byte, which
// capped the merged palette at 128 for no reason other than the rebasing.
// debris.wgsl's cube path was already 1-based; this makes the micro path agree,
// so all three encodings (file, micro brick, cube) now mean the same thing.
//
// Art colour is render-only and never reaches a world cell, so none of this can
// move the world hash (rule 1). GRID colour is a separate mechanism that
// resolves through this same palette run — see MATF_TINTED.
constexpr uint32_t kArtPaletteSlotsGpu = 255;
constexpr uint32_t kArtPaletteBaseGpu = kStainPaletteBase - kArtPaletteSlotsGpu;

// ---- the tint palette (GRID colour — sim/materials.h kMatFlagTinted) --------
// Third reserved run in the material table, same trick as the stain and art
// palettes above and for the same reason: the renderer needs index -> colour
// for something that is not a material id, and a reserved run costs no new
// binding and rides the existing table upload (so tints hot-reload with R).
//
// A MATF_TINTED material reinterprets the voxel word's state nibble as an index
// into ITS OWN 16-entry run here, at kTintPaletteBaseGpu + tintBase + state.
//
// WHY THIS IS A SEPARATE RUN FROM THE ART PALETTE, given that both hold "a
// colour that is not a material's". They have different LIFETIMES. The art
// palette is rebuilt wholesale every time prefabs load (mob.cpp clears it, so
// model indices cannot go stale across a hot reload); the tint palette is
// authored in materials.json and must survive exactly as long as the material
// table does. Sharing one run would mean a prefab reload silently wiping the
// tints, which is the kind of coupling that shows up as the world quietly
// changing colour three reloads later.
//
// 256 entries = 16 materials at the 16-tint maximum, which is far past the
// handful of substances that want tinting. Real material ids number ~115, so
// this run is nowhere near them and nothing can reference it as a material
// (ids are assigned from the bottom up).
constexpr uint32_t kTintPaletteSlotsGpu = 256;
constexpr uint32_t kTintPaletteBaseGpu = kArtPaletteBaseGpu - kTintPaletteSlotsGpu;

// ---- the FAR SLOT palette (far-field cascade — sim/materials.h) ------------
// Fourth reserved run, same trick as the three above and for the same reason:
// the renderer needs index -> material for something that is not a material id.
//
// A far cascade cell is ONE byte: seven bits and a conservative blocker flag
// (common.wgsl FAR_SLOT_MASK / FAR_BLOCKER_BIT). Those seven bits used to BE a
// material id, which is why `LoadMaterials` refused a 129th material — the far
// field would have started painting the wrong colour at distance and claiming
// a blocker wherever bit 7 landed, with nothing to say so. They are now a FAR
// SLOT: an index into this run, where entry `kFarPaletteBaseGpu + slot` holds
// (in its `flags` word) the material id that slot paints. Materials that look
// alike at cascade distance share a slot by authoring `"far": "<material>"` in
// materials.json, so the 128 is now a budget on DISTINGUISHABLE FAR COLOURS
// rather than on the material table.
//
// Slots are assigned identity-first (material i takes slot i while i < 128), so
// a table with no aliases writes byte-for-byte what it wrote before this run
// existed. See LoadMaterials' slot assignment for the aliasing rules.
//
// 128 entries exactly fills the 7-bit field; a bigger run would be unreachable.
constexpr uint32_t kFarPaletteSlotsGpu = 128;
constexpr uint32_t kFarPaletteBaseGpu = kTintPaletteBaseGpu - kFarPaletteSlotsGpu;
static_assert(kFarPaletteBaseGpu > 1024,
              "reserved palette runs have grown down into the material id "
              "space — raise kMaterialSlots or shrink a run");

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
// number is VRAM every world pays whether or not it has a micro body. It remains
// a HARD ceiling (rule 2): past it MicroBodyOwn fails and the body keeps a stale
// skin, which is the documented graceful degradation, not an unbounded
// allocation.
//
// 2 MiW = 8 MiB, DOUBLED 2026-09-15 to stay behind the model table below rather
// than in front of it. The two ceilings have to be sized against the same
// scene or the cheaper one just moves the wall: 148 shared records cost ~200k
// words at load, and an owned clone is the model's payload plus a stain lattice
// — about 1.5x — so the ~350 clones a fight's worth of corpses and bloodied
// creatures can hold live run to ~800k. At 1 MiW the pool would have become the
// new "gore stopped working" the moment the table stopped being it.
constexpr uint32_t kMicroBodyPoolWordsWorld = 2u << 20;
// ---- THE MODEL TABLE IS SHARED RECORDS **PLUS** EVERY OWNED CLONE ----------
//
// Two populations, and sizing this to the first one is the bug it shipped with
// (owner report 2026-09-15: "after killing a bunch of zombies, new zombies
// spawn with 0 gore and won't get any when I hit them; it fixes itself when I
// leave the area with the corpses").
//
//   SHARED, packed at load, never freed: one record per (def, limb) pair across
//   every mob def, plus one per garment COVER PANEL and per held item model.
//   That second half is the one that grew — 87 limb models against 61 item
//   models on 2026-09-15, 148 of the 256 records gone before a single creature
//   has been hit, and every new garment is another four.
//
//   OWNED, copy-on-write, one per BODY PART that has been damaged, burnt,
//   bloodied or refitted (sim/microbody.h). A zombie's `rot` block carves ~11
//   of its 15 limbs at SPAWN, so a corpse is ~11 records and holds them until
//   it is culled. 108 free records is seven zombies, which is exactly how many
//   the owner got before every carve, char and blood mark stopped appearing.
//
// So the ceiling has to be `shared + the most owned clones that can be live at
// once`, and the second term is bounded by the render slot table: an owned
// brick belongs to a drawn body part, and kMaxBodySlots is how many of those
// there can be. 1024 is that sum with room for the content side to keep
// growing — 16 KiB of table, uploaded whole every dirty frame, which is still
// nothing next to the 4 MiB pool the records point into.
//
// It remains a HARD ceiling, and past it MicroBodyOwn still refuses and the
// body keeps a stale skin. What changed alongside this number is that the
// refusal is no longer SILENT (MicroBodySet::refusals) — a cap whose only
// symptom is "gore quietly stopped working" is a cap nobody can diagnose.
static_assert(kMaxBodySlots <= 512, "micro-body model ceiling derived below");
constexpr uint32_t kMaxMicroBodyModels = 1024;
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
//
// W1 widened it to 41: the RELEVEL block (docs/PLAN_water_relevel.md §3.2/§3.3
// — WBS_RV* in common.wgsl). Fourteen words, and every one of them is `RV`
// rather than `R`: WBS_RSUM (word 12) is ALREADY the adoption/re-audit
// reduce's running sum, and a re-audit can run while a body is hot and
// relevelling, so aliasing the two sets would have one pass zeroing the
// other's accumulator on the tick it mattered most. The twelfth is WBS_RVBASE,
// which the plan's word list did not have and the implementation needs: the
// ledger consumes LAST tick's histogram and may lower the level THIS tick, so
// the bucket origin has to be published by the pass that filled the buckets.
//
// W-D widened it to 42 for ONE word, WBS_RAREA — the free-surface CELL COUNT
// the adoption reduce measured. Discovery's size gate (§8.4) is "adopt iff the
// measured surface area is at least sim.waterAdoptMinArea", and before this
// word there was nowhere for that number to come from: `wbReduce` measured the
// volume and the level and nothing else, and WBS_AREA at adoption is the CPU's
// ANALYTIC seed, which for a discovered disc is a fabricated cylinder and not a
// measurement of anything. A gate that read the seed would be the circular
// probe assertion — the candidate's own guess deciding whether the candidate is
// real.
//
// W2 widened it to 46 for the SLEEP of the surface-momentum layer
// (docs/PLAN_water_relevel.md §4.3, WBS_WV* in common.wgsl). Four words, and
// they are `WV` rather than `W` for the reason the relevel block is `RV`: every
// short prefix in this map is already taken, and two accumulators sharing a
// word is one pass zeroing the other's tally on the tick it mattered.
//
// The pipes themselves are NOT here. They are per COLUMN, not per body, and
// kWorldN^2 columns will not fit in a 64-entry ledger — see kWaterFluxWords
// below for the buffer they live in and why it is a binding of its own.
constexpr uint32_t kWaterBodyStateWords = 46;
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
//   [kWaterRelevelHistBase, + kWaterBodyCap * kWaterRelevelBuckets)  W1 hist
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
constexpr uint32_t kWaterCurveBase = kWaterBodyCap * kWaterBodyStateWords;
constexpr uint32_t kWaterSweepScratchBase =
    kWaterCurveBase + kWaterBodyCap * kWaterCurveWords;

// ---- W1: the RELEVEL HISTOGRAM (docs/PLAN_water_relevel.md §3.2) ----------
//
// One bucket per EIGHTH of free-surface height in the band the measure looks
// at, per body. `wbSurface` accumulates into it; `wbLedger` walks it next tick
// to publish the give/take cutoffs and ZEROES the block as it consumes it.
//
// WHY IT LIVES PAST THE SWEEP SCRATCH AND NOT AT `kWaterCurveBase +
// kWaterBodyCap * kWaterCurveWords`. That expression IS
// `kWaterSweepScratchBase` — the shared openness bitmap the split's label
// propagation reads — so the first draft of the plan would have laid 68 KiB of
// histogram straight over it, and the symptom would have been a basin that
// split wrongly only while some other basin was relevelling. Appended AFTER
// the scratch, which is what "past the END of the existing layout
// (kWaterBodyStateTotalWords)" has to mean. Still no new binding.
//
// THE DEPTH IS A KNOB BUT THE BLOCK IS NOT. `sim.waterRelevelDepth` decides
// how far below `level` the measure looks; the BUFFER has to be sized at
// compile time, so the knob is clamped to this ceiling in the shader and the
// block is sized from the ceiling. A column further down than this clamps into
// the end bucket and is treated as "deep" — bounded, and a column 32 voxels
// below the surface is a hole the MPM owns anyway.
constexpr uint32_t kWaterRelevelDepthMax = 32;
// 8 eighths per voxel, over [level - DEPTH, level + 1]: DEPTH + 2 voxels.
constexpr uint32_t kWaterRelevelBuckets = 272;
static_assert(kWaterRelevelBuckets == 8 * (kWaterRelevelDepthMax + 2),
              "kWaterRelevelBuckets must cover 8*(depth+2) eighths");
constexpr uint32_t kWaterRelevelHistBase =
    kWaterSweepScratchBase + kWaterSweepScratchWords;
constexpr uint32_t kWaterBodyStateTotalWords =
    kWaterRelevelHistBase + kWaterBodyCap * kWaterRelevelBuckets;

// ---- W2: THE SURFACE-MOMENTUM STORE (docs/PLAN_water_relevel.md §4.1) ------
//
// A dense XZ grid over the residency window, one record per COLUMN, indexed
// `(z & WORLD_MASK) * kWorldN + (x & WORLD_MASK)` exactly like every other
// window-relative buffer. It gets its own binding because it is per column and
// there are 262,144 of them: `waterBodyState` is a 64-entry ledger and the
// relevel histogram already sits past the end of it.
//
// FOUR NON-NEGATIVE OUTFLOW PIPES PER COLUMN, all owned by that column — the
// Mei/O'Brien virtual-pipes layout, and the reason it is outflows rather than
// two signed shared faces is a WRITE HAZARD and not taste. The outflow clamp
// has to scale a column's own outflows against what that column may give; with
// shared faces half of a column's outflows run through a face the NEIGHBOUR
// owns, so "write the scaled flux back" means two writers per word, which is
// exactly the mark/apply hazard this subsystem avoids everywhere else. With
// owned outflows every transfer amount lives in ONE word, written by its owner
// and gathered by its receiver, so conservation between two columns is exact
// by construction.
//
// THE FIFTH AND SIXTH WORDS ARE WHAT MAKE IT SAFE TO READ:
//   [4] `s` — the column's free-surface height in EIGHTHS, written by
//       `wbSurface` at the END of a tick. W2's flux pass gathers five of these
//       instead of re-walking five columns of voxels (§4.3).
//   [5] the STAMP. `((tick + 1) << 4) | pageX << 2 | pageZ`, where pageX/pageZ
//       are the low two bits of the column's WINDOW PAGE (`x / kWorldN`). It is
//       the whole answer to "what happens on a window shift": the residency
//       window is toroidal, so a slot is silently reused by a column kWorldN
//       voxels away, and momentum from the old occupant would otherwise read as
//       the new one's. The tick half alone would ALMOST do it — but a shift
//       moves the origin by one CHUNK, so the departed column's stamp can be
//       exactly one tick old on the tick its slot is re-read, and the page bits
//       are what separate two columns exactly kWorldN apart. Same idea as
//       `opennessStamp` (a hash of the world chunk coord) and `reposeSnap` (a
//       per-slot tick), in one word because one pass writes all six together.
//
// THE CONTRACT THE STAMP BUYS, and both passes depend on it: a record whose
// stamp is not exactly this tick's reads as ZERO, and `wbFlux` writes all four
// pipes of every column whose stamp IS this tick's. So "fresh stamp" means
// "wbFlux wrote these four pipes this tick", and a neighbour's pipe is either
// a real transfer both ends agree on or is not there at all.
//
// DERIVED DATA in guideline #3's sense: not hashed, not saved, dropped on a
// window shift (§6) — the same call the MPM makes for its particles. It IS
// state that decides voxel writes, so it is integer, tick-ordered, gather-only
// and one-writer-per-word, and the twice-run comparison covers it by
// construction.
constexpr uint32_t kWaterFluxWords = 6;
constexpr uint32_t kWaterFluxColumns = (uint32_t)kWorldN * (uint32_t)kWorldN;
constexpr uint64_t kWaterFluxBytes =
    (uint64_t)kWaterFluxColumns * kWaterFluxWords * 4;   // 6 MiB at 512^2

// ---- W3: THE IMPULSE DOOR (docs/PLAN_water_relevel.md §5) ------------------
//
// ONE door, two callers, and that is the whole design. §5 asks for a blast to
// shove a pond and for a swimmer to drag a wake behind them, and both are the
// same statement — "at this column, for one tick, push the surface THIS way
// this hard". So there is one record and one queue rather than two systems that
// would drift apart, and `sim_explode.wgsl` is left alone: rule 3 keeps the
// explosion's writes in the mutation path, and a compute pass that reached into
// `waterFlux` from the explosion kernel would be a second writer of a word
// `wbFlux` owns (§4.1's whole argument).
//
// IT RIDES THE TICK STREAM, in TickParams, exactly as `windPrims` does and for
// the same reason: a replay reproduces the stream and the twice-run determinism
// gate compares it, so an impulse can never be a function of when the CPU
// noticed something. `waterImpulseCount == 0` is an exact identity — `wbFlux`'s
// loop does not execute and no pipe sees a different number — which is what
// makes each of the three W3 knobs an identity at 0 (at 0 the CPU emits no
// record at all, so the count stays 0).
//
// Applied BEFORE the outflow clamp in `wbFlux`, so an impulse cannot move more
// water than the column is allowed to give and conservation is untouched: the
// clamp is what the ledger's credit term is argued from, and an impulse that
// bypassed it would be a mass pump with a knob on it.
constexpr uint32_t kWaterImpulseCap = 8;
// Two std140 vec4 rows per impulse:
//   row 0  (x, z, radius, strength)   world column, cells, Q8 flux at the centre
//   row 1  (dirX, dirZ, 0, 0)         Q8 direction; (0,0) means RADIAL OUTWARD
// A swimmer whose velocity rounds to (0,0) in Q8 is not moving, and the CPU
// does not emit a record for it — so the radial sense of (0,0) is never
// ambiguous in practice, and it is checked on the CPU side rather than argued.
constexpr uint32_t kWaterImpulseWords = 8;
constexpr uint32_t kWaterImpulseScalars = 64;
static_assert(kWaterImpulseScalars == kWaterImpulseCap * kWaterImpulseWords,
              "kWaterImpulseScalars must equal cap * words");
// How wide a swimmer's wake is, world cells. A player is roughly two cells
// across at kVoxelsPerMetre = 10 and the wake wants to be the body plus the
// water it drags with it, not a bow wave that reaches the far bank. Small
// enough that the outflow clamp, not the radius, is what bounds the total.
constexpr uint32_t kWaterSwimWakeRadius = 4;
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

// ---- W-D: DISCOVERY, the created body (PLAN_water_relevel.md §8) -----------
//
// A body the PLAYER makes — a basin dug and filled by hand, a pool a drain
// leaves behind — has no analytic container, so nothing in M1's registry can
// name it. §8's answer reuses M2's authority split verbatim: the CPU accounts
// EVIDENCE (eighths of liquid placed, where) off the tick input stream and
// PROPOSES a probe disc; the GPU measures the real water and adopts or refuses.
// The CPU never learns the verdict, so no fence ever reaches a voxel write.
//
// Live probe discs. Sixteen inside kWaterBodyCap, so discovery can never crowd
// the authored basins out of the ledger. Each costs one descriptor, and a
// REFUSED one costs three ledger loads a tick and no footprint work at all —
// which is "puddles can be ignored easily" implemented literally.
constexpr uint32_t kWaterDiscoveredCap = 16;
// The reserved basin-id range. Authored pools take 1..3, authored lakes
// 0x4xxxxxxx, tarns 0x8xxxxxxx; discovery takes 0xC0000000 | slot. A basin id
// is an IDENTITY (the hole hints, the curve-dirty latch and the carried ledger
// slot are all keyed by it), so it is derived from the probe's SLOT — which a
// deleted neighbour never renumbers — and never from its index in a vector.
constexpr uint32_t kWaterDiscoverIdBase = 0xC0000000u;
// Ticks an EVICTED probe keeps its descriptor, proposed with WBF_RELEASE, before
// the entry is dropped. Never drop a descriptor cold: the slot would be reused
// against a ledger still carrying the old body's debit, which is the
// carried-descriptor bug class the drain pass documents. 600 = 20 s, two orders
// more than a probe's debit (normally zero) needs to square.
constexpr uint32_t kWaterDiscoverReleaseTicks = 600;
// The EVIDENCE GRID: coarse XZ cells, world voxels on a side. 32 is four chunk
// columns and is chosen so a pond-sized pour lands in a handful of cells while a
// single bucket lands in one. Evidence is accumulated per (cell, liquid
// material) and is a pure function of the tick's op list.
constexpr int kWaterEvidenceGrid = 32;
// Live evidence cells tracked. Bounded by rule 2 rather than by hoping the
// player stops pouring: at the cap the LOWEST-evidence cell is evicted (ties by
// oldest tick, then by position), which is deterministic and costs at worst the
// forgetting of a puddle nobody was going to get a body out of.
constexpr uint32_t kWaterEvidenceCap = 192;
// Chunk-list entries every discovered body TOGETHER may claim, out of
// kWaterChunkCap. Rule 2 charges the budget before emission, and without a
// share of its own a probe disc — whose ANALYTIC volume is an over-predicting
// cylinder, so it sorts EARLY in Classify's biggest-first order — could take
// the list out from under the authored lake. A quarter of the cap leaves the
// lake's ~140 entries untouched in every fixture measured.
constexpr uint32_t kWaterDiscoverChunkShare = 256;
// Voxels of pad around the bounding circle of the contributing evidence (§8.3),
// and voxels of floor below the lowest evidence cell. The disc is only a BOUND
// — the sweep's component map trims ownership to the connected water exactly as
// it does for an authored basin — so the pad is generosity, not accuracy.
constexpr int kWaterDiscoverPad = 8;
constexpr int kWaterDiscoverFloorPad = 4;
// Largest probe disc. A probe is a proposal, and an unbounded one would list a
// chunk footprint the share above then refuses in full — better to bound the
// geometry than to discover a body the budget can never carry.
constexpr int kWaterDiscoverMaxRadius = 48;
// THE TWO NEW PER-BODY FLAG BITS, named here rather than written as literals in
// BuildGpu. They are a PROTOCOL between waterbody.cpp and sim_waterbody.wgsl —
// TickParams row 1, word 3 — and a protocol restated as a bare `| 32` in one
// place and a `const WBF_DISCOVER : i32 = 32` in the other is a two-places-
// must-agree bug with nothing behind it. `check_invariants.py`'s water-ledger
// check compares these against common.wgsl's WBF_* block; bits 0..4 predate it
// and are still literals, which is exactly why this pair is not.
constexpr int32_t kWbfDiscover = 32;   // this body is a discovered PROBE
constexpr int32_t kWbfReprobe = 64;    // one-tick pulse: new evidence landed

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

// Trample ring size (render-only, DESIGN.md §9 "Analytic plants") — must match
// the literal in RenderParams.tramples in common.wgsl (kTrampleCap * 2 vec4s).
// A stamp is one presser's footprint on the ground; the player walking lays a
// fresh one every ~half radius and each lives for hold + recover seconds, so
// 48 is ~ten seconds of wading plus a handful of mobs. Overflow overwrites the
// oldest, which is exactly what a faded footprint is.
constexpr uint32_t kTrampleCap = 48;

// Must match TickParams in common.wgsl.
// The repose snapshot's epoch source -- see TickParams::snapEpoch below for
// why it is a counter and not the tick. A function-local static so there is one
// across every TU, and atomic because worldgen and the tick path can build a
// TickParams from different threads.
inline uint32_t NextSnapEpoch() {
  static std::atomic<uint32_t> n{0};
  return ++n;
}

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
  // Deferred streaming wake (docs/RESEARCH_streaming_hitch.md R1; was the
  // pad_wp0 pad word, so the struct layout and the pinned hash are unchanged
  // at the 0 default). Nonzero => worldgen's genChunk writes its act verdict
  // to `genAct` and leaves dirtyIn/dirtyOut CLEARED for the slots it wrote,
  // instead of waking them itself. Set only by Stream::ShiftAxis.
  uint32_t genDeferWake = 0;
  uint32_t pad_wp1 = 0;
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
  // ---- W1: RELEVEL, THE RATE AND THE ARM IN ONE WORD ---------------------
  //
  // `sim.waterRelevelMax` — eighths a column may move per tick — but ZEROED BY
  // THE CPU on any tick `WaterBodyGpu::writesThisTick` is false, and that is
  // the load-bearing half. The relevel is the second voxel WRITER in this
  // subsystem, so it may only run on a tick whose chunks were declared to the
  // page table; `writesThisTick` is the CPU's own answer to that and it does
  // not otherwise reach the GPU. Sending the knob through this word rather than
  // reading TUNE_WATER_RELEVEL_MAX in the kernel makes the arm and the rate ONE
  // number with one owner — a kernel that read the tuning const directly would
  // relevel into a JITTER sentinel the first tick the hot window closed, and
  // the symptom would be page faults, not a wrong lake.
  //
  // (The other two knobs, `sim.waterRelevelGain` and `sim.waterRelevelDepth`,
  // stay TUNE_* consts: they shape the rule, not whether it may write, and
  // const-eval keeps the divide and the loop bound out of the uniform.)
  int32_t waterRelevelMax = 0;
  // ---- W-D: THE SIZE GATE (PLAN_water_relevel.md §8.4) -------------------
  //
  // `sim.waterAdoptMinArea` — the measured free-surface CELL count below which
  // the ledger refuses a DISCOVERED probe and parks it in WB_REFUSED. Forwarded
  // through the uniform rather than read as a TUNE_* const for the reason
  // waterQuietTicks and waterMinVolume are: a gate that wants a different
  // threshold sets it with SetCurrentTuning and pays no pipeline rebuild.
  //
  // It applies ONLY to a body carrying WBF_DISCOVER. An authored basin is a
  // closed form the registry vouches for, and putting a size gate in front of
  // it would make `sim.waterDiscoverMinEighths = 0` stop being the exact
  // identity §8.6 promises — a small authored pool would start being refused by
  // a knob that is supposed to be about discovery.
  int32_t waterAdoptMinArea = 0;
  // ---- W2: THE SURFACE-MOMENTUM ARM (PLAN_water_relevel.md §4.2) ---------
  //
  // `sim.waveMode` — 0 OFF, 1 the virtual-pipe layer — and, like
  // `waterRelevelMax` above, ZEROED BY THE CPU on any tick
  // `WaterBodyGpu::writesThisTick` is false. The wave apply is folded into
  // `wbRelevel`, i.e. into the same voxel writer, so it inherits the same rule:
  // it may only run on a tick whose chunks were declared to the page table.
  //
  // It is an ARM and not a rate, so it rides `waterRelevelMax`'s arm rather
  // than replacing it: at `sim.waterRelevelMax = 0` nothing in this block
  // writes a voxel at all and the wave has no apply to fold into. The wave's
  // own knobs (gravity, depth cap, damping, sleep epsilon) stay TUNE_* consts
  // const-eval'd in the kernel — they shape the rule, not whether it may write.
  //
  // At `sim.waveMode` 0 the `waterFlux` pass row is not recorded, no pipe is
  // written and the wave term in the apply is not evaluated, so the pinned
  // world hash cannot see W2. It was the padWb4 pad word, so the struct
  // layout — which check_invariants.py compares against common.wgsl on TOTAL
  // SIZE — is unchanged.
  int32_t waveMode = 0;
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
  // sim.gasMode (docs/PLAN_gas_particles.md). 0 = the residency edge is a
  // WALL, 1 = a SINK. Read CPU-side per tick like windMode and dayPhase, so it
  // is part of the tick input stream a replay reproduces and the twice-run
  // determinism gate compares. It was the padCp0 pad word, so the struct
  // layout — which check_invariants.py compares against common.wgsl on TOTAL
  // SIZE — is unchanged.
  uint32_t gasMode = 1;
  // ---- THE REPOSE SNAPSHOT EPOCH (kReposeSnap* above) --------------------
  //
  // A MONOTONIC, NEVER-RESET counter, and every word of that is load-bearing.
  // The snapshot's per-slot validity stamp cannot be the TICK, because the tick
  // is not monotonic: F7 regen (src/main.cpp, `tick = 0`) rewinds it with the
  // stamp region untouched, and every selftest arm that replays a fixed tick
  // window does the same. A slot still carrying stamp k from before the rewind
  // would then SKIP its refill at tick k and hand the CA the PREVIOUS WORLD's
  // occupancy bits -- same seed, same tick, same inputs, different pile,
  // depending only on how long the session had run. This counter cannot alias
  // that way: it only ever increases, so a stale stamp is always strictly less
  // than the current one.
  //
  // INCREMENTED BY TickParams' OWN DEFAULT INITIALIZER, once per construction,
  // which is once per recorded tick at every one of the ten call sites that
  // build one -- no site can forget it, and a site that builds a TickParams it
  // never submits only burns a value, which costs nothing. Starts at 1, so 0
  // stays "no snapshot" and the zero-initialized buffer cold-starts correctly.
  //
  // NOT hashed state and not an input: two runs of the same seed and tick may
  // legitimately sit at different epochs (the determinism gate's second run
  // does, in the same process). The sim is invariant to the VALUE because the
  // prepass writes the same epoch the CA then compares against; what it is not
  // invariant to is a stamp from another tick reading as current, which
  // monotonicity is exactly what rules out. It was the padCp1 pad word, so the
  // struct layout -- which check_invariants.py compares against common.wgsl on
  // TOTAL SIZE -- is unchanged. Wraps after 2^32 ticks, i.e. 4.5 years of
  // continuous simulation at 30 Hz.
  uint32_t snapEpoch = NextSnapEpoch();
  int32_t currentPrimLo[3] = {1, 1, 1};   // union AABB, inclusive world cells
  int32_t padCp2 = 0;                     // (lo > hi = no primitives)
  int32_t currentPrimHi[3] = {0, 0, 0};
  int32_t padCp3 = 0;
  int32_t currentPrims[kCurrentPrimScalars] = {};
  // ---- W3: THE IMPULSE BLOCK (PLAN_water_relevel.md §5; kWaterImpulseCap) --
  //
    // AT THE TAIL, and the position is arithmetic rather than taste. Every other
  // water word is wedged into the 15-word header the note over `waterBodies`
  // measures, and that header is EXACTLY full — a sixteenth word there slides
  // every descriptor by two scalars, silently, because C++ packs an int32_t
  // array at 4-byte alignment while std140 rounds a vec4 array up to 16.
  // `currentPrims` is a multiple of four scalars, so it ends 16-byte aligned and
  // this count plus three pads is its own clean 16-byte row in front of the
  // vec4 array. check_invariants.py compares the two shapes on TOTAL SIZE and
  // is the only thing that catches a disagreement.
  //
  // Zero is the shipping state of every tick nobody blew anything up on, and
  // zero is an exact identity: `wbFlux`'s impulse loop does not execute.
  uint32_t waterImpulseCount = 0;
  uint32_t padWi0 = 0, padWi1 = 0, padWi2 = 0;
  int32_t waterImpulses[kWaterImpulseScalars] = {};
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

  // ---- the trample field (must match RenderParams in common.wgsl) --------
  // Render-only, like the wind sway it composes with: a bounded ring of
  // footprint stamps (player + mobs), each an analytic disc the plants flatten
  // under while it is pressed and spring back from after it is released. No
  // sim kernel reads it, it is not hashed and not saved. Two vec4 per stamp:
  //   [x, z, y, radius]           world voxels; y is the ground under the foot
  //   [t0, tEnd, strength, 0]     R.time it landed / was last pressed; 0..1
  // Lo/Hi is the union AABB (xz + y band) for the per-sample early reject.
  uint32_t trampleCount = 0;
  uint32_t pad_tr0 = 0, pad_tr1 = 0, pad_tr2 = 0;
  float trampleLo[3] = {1.0f, 1.0f, 1.0f};
  float pad_tr3 = 0.0f;
  float trampleHi[3] = {0.0f, 0.0f, 0.0f};
  float pad_tr4 = 0.0f;
  float tramples[kTrampleCap * 8] = {};

  // ---- the shadow cache's clock (must match RenderParams in common.wgsl) --
  // The RENDER frame counter, and deliberately not `tick` or `time`. `tick` is
  // the 30 Hz sim clock and the renderer runs uncapped, so several frames share
  // one tick — keying cache entries on it would make an entry live for however
  // many frames the machine happened to fit into a tick, i.e. a staleness bound
  // that changes with frame rate. `time` is a float animation clock and cannot
  // be compared for exact equality at all.
  //
  // Only the low 4 bits are ever compared (the bucket's resolvedFrame field),
  // so wraparound at 16 is fine and intended: an entry 16 frames stale is
  // indistinguishable from a fresh one, but nothing keeps an entry that long —
  // the resolve pass rewrites every registered patch every frame.
  uint32_t frameIdx = 0;
  // Live subdivision of a voxel face per axis (render.shadowCacheSubdiv,
  // clamped to kShadowSubdivMax). Passed rather than const-folded so the
  // quality knob hot-reloads on F5 like every other render tuning value; the
  // CACHE ITSELF is const-gated, but its granularity is not.
  uint32_t shadowSubdiv = 4;
  uint32_t pad_sc0 = 0, pad_sc1 = 0;
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

// ---- THE PENDING-FACE WORD: FarParams.origins[k].w -------------------------
// Six counts of level-CHUNK layers still waiting on the sieve, one per box
// face in the order (-x, +x, -y, +y, -z, +z), packed low field first, plus a
// flag for "this whole level is a reset in flight". Written by
// FarField::FaceWord (src/sim/farfield.cpp), read by raymarch.wgsl's farBox.
// scripts/check_invariants.py pins the two together.
//
// THE FIELD IS SIZED BY kFarNChunk, not by convenience. It was four bits and
// saturated at 15 while a level is 32 chunks across, so a deeper backlog left
// stale toroidal slabs inside the box the renderer was told it could march
// (farfield.h's FaceWord note has the failure). Five bits holds every layer a
// box has; a count past that is not expressible and escalates to the flag.
// kFarNChunk - 1 layers is the most an exclusion can usefully name: at
// kFarNChunk the box is empty, which the flag already says.
constexpr uint32_t kFarFaceBits = [] {
  uint32_t b = 1;
  while ((1u << b) < kFarNChunk) b++;
  return b;
}();
constexpr uint32_t kFarFaceMax = (1u << kFarFaceBits) - 1u;
static_assert(kFarFaceMax >= kFarNChunk - 1, "a face field must hold a whole box");
// Kept clear of the sign bit: the word travels as int32_t in FarParams and the
// WGSL side reads it with u32(), which is a value conversion, not a bitcast.
constexpr uint32_t kFarFaceFlagBit = 30;
static_assert(kFarFaceBits * 6 <= kFarFaceFlagBit, "face fields overlap the flag");
constexpr uint32_t kFarFaceAllPending = 1u << kFarFaceFlagBit;

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
  // Every dirty-reason bit set by ANY chunk this snapshot, OR'd in the fold
  // that already walks the array for `activeChunks`. One `|=` per chunk, so it
  // is free where the diagnostic histogram (SANDVOX_DIRTY_REASONS) is not, and
  // it answers the one always-on question the histogram was too expensive for:
  // WHICH KINDS of rule are running at all. Read by the gas render flag.
  uint32_t dirtyReasonOr = 0;
  uint64_t voxelTotal = 0;
  uint32_t worldHash = 0;
  uint32_t pick[8] = {};
  uint32_t particleCount = 0;         // live particles (post-resolve that tick)
  // ---- gas particles (docs/PLAN_gas_particles.md) ----
  // Async, one tick latent, exactly like everything else on this ring. The
  // per-tick counters come from gasSpawn's header, which is cleared before the
  // CA runs, so each is "this tick" and not a running total. `gasCount` is the
  // live population; the rest are the attributions CLAUDE.md rule 6 asks for,
  // so "the plume is thin" is answered by a number that names WHICH bound bit.
  uint32_t gasCount = 0;         // live gas particles (post-resolve that tick)
  uint32_t gasLeaveAccepted = 0; // voxels converted at the window edge
  uint32_t gasLeaveRefused = 0;  // conversions refused: the spawn list was full
  uint32_t gasEdgeHits = 0;      // gas voxels whose intent left the window
  uint32_t gasPoolRefused = 0;   // spawns dropped: the gas pool was full
  uint32_t gasReentered = 0;     // particles that became voxels again
  uint32_t gasDied = 0;          // decay / outer box / ceiling
  uint32_t gasAboveWindow = 0;   // live parcels above the window's top face
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
  // The C++ twin of common.wgsl's `slotWorldChunk`. A ticket slot is NOT
  // window-coordinate arithmetic (docs/PLAN_chunk_tickets.md §2.2), so it reads
  // its world chunk from the CPU-side ticket table instead. At kTicketMax = 0
  // there are no such slots and the branch is dead.
  IVec3 SlotToWorldChunk(uint32_t slotIdx) const {
    if (kTicketSlots != 0 && slotIdx >= kNumChunks) {
      return TicketSlotWorldChunk(slotIdx);
    }
    IVec3 s{(int)(slotIdx % kNChunk), (int)((slotIdx / kNChunk) % kNChunk),
            (int)(slotIdx / (kNChunk * kNChunk))};
    int m = (int)kNChunk - 1;
    return {origin_.x + ((s.x - origin_.x) & m), origin_.y + ((s.y - origin_.y) & m),
            origin_.z + ((s.z - origin_.z) & m)};
  }
  // P0 STUB, the C++ half of common.wgsl's ticketSlotWorldChunk. P1 gives
  // `Tickets` a slot -> wc vector and returns from it; until then no slot can
  // reach here (kTicketSlots == 0) and the compiler drops the caller's branch.
  IVec3 TicketSlotWorldChunk(uint32_t slotIdx) const {
    (void)slotIdx;
    return {0, 0, 0};
  }
  // Is this slot one the window's arithmetic can produce? Everything that
  // decomposes a slot into (x, y, z) window-chunk coords must ask first.
  static bool IsWindowSlot(uint32_t slotIdx) { return slotIdx < kNumChunks; }
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
  // The world seed the mirror and the page table synthesize with — the same
  // seed worldgen ran under, so World::TerrainHeight(x, z, WorldSeed()) is
  // the ground a CPU system may assume where nothing has been fetched.
  uint32_t WorldSeed() const { return mirrorSeed_; }

  // Residency mode. `dense` is the identity map — page i for slot i — which
  // makes every address bit-identical to pre-paging code while still running
  // the whole translation path (the load, the branch, the multiply-add). It is
  // the only live differential oracle the engine has now that Dawn is gone
  // (§6.3), so it is load-bearing test infrastructure, not a fallback: never
  // selected automatically, always available.
  enum class Residency { Dense, Paged };
  Residency residency = Residency::Dense;
  // Dense is the IDENTITY map — page i for slot i — so it needs one page per
  // slot, ticket slots included (docs/PLAN_chunk_tickets.md §2.8: dense with
  // tickets active must produce the same hash as paged, which it cannot do if
  // a ticket slot has no page to be the identity of).
  uint32_t PoolPages() const {
    return residency == Residency::Dense ? kNumSlots : kPoolPages;
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
  //
  // WHO ASKED. One FIFO serves every consumer of the CPU mirror -- the island
  // scans (32 a scan), the terrain colliders (24 a tick), a mob's ground and
  // burn probes, settle-back's support test, spells, audio occlusion -- and
  // drains kFetchPerTick per readback slot, so a flood from one of them is
  // starvation for all the rest. Until 2026-09-12 that was invisible: the
  // gates force-fetch, and nothing reported the queue's depth, how old a
  // request was when it finally rode a slot, or how many were dropped because
  // the chunk streamed out first. `FetchProbe` counts all of that PER SOURCE;
  // the --frames summary and --fell-tree's FALL line print `FetchReport()`.
  // Tuning kFetchPerTick or any consumer's cap is justified by that line or
  // not at all. A tag is a hint about attribution, never about priority: the
  // queue is still strictly first-come.
  enum class FetchSource : uint8_t {
    Other = 0,   // untagged callers (spells, audio, harnesses, gates)
    IslandScan,  // EventReady / RunIslandDetection (phys/debris.cpp)
    Terrain,     // ManageTerrain's collider sweep (phys/debris.cpp)
    Mob,         // a creature's ground / burn probe (game/mob.cpp)
    Settle,      // SettleFootprintSupported (phys/debris.cpp)
    Count
  };
  static const char* FetchSourceName(FetchSource s);
  void RequestChunkFetch(IVec3 worldChunk,
                         FetchSource src = FetchSource::Other);
  struct FetchProbe {
    static constexpr int kSources = (int)FetchSource::Count;
    uint64_t requests[kSources] = {};   // accepted into the queue
    uint64_t coalesced[kSources] = {};  // already queued: no second entry
    uint64_t refused[kSources] = {};    // not resident when asked
    uint64_t carried[kSources] = {};    // rode a readback slot
    uint64_t dropped[kSources] = {};    // streamed out between ask and drain
    // Age = ticks from the request to the slot that carried it, where the
    // stamp is the tick of the most recent EncodeReadbacks: 1 means "the very
    // next slot", the documented one-tick latency. Anything above that is
    // time spent queued behind other consumers (or behind a tick with no free
    // readback slot, which drains nothing).
    uint64_t ageSum[kSources] = {};
    uint32_t ageMax[kSources] = {};
    uint32_t depthMax = 0;     // longest the queue ever got (after a push)
    uint32_t drains = 0;       // readback slots encoded
    uint32_t drainsFull = 0;   // ...that took kFetchPerTick and left some waiting
    uint32_t leftMax = 0;      // most left waiting after one drain
    uint64_t leftSum = 0;      // ...summed over drains (mean backlog)
    uint32_t sinceTick = 0;    // the window starts here (ResetFetchProbe)
  };
  const FetchProbe& Fetches() const { return fetchProbe_; }
  void ResetFetchProbe() {
    fetchProbe_ = FetchProbe{};
    fetchProbe_.sinceTick = fetchTick_;
  }
  // One line: totals, per-source counts, ages, depth. Never empty.
  std::string FetchReport() const;
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
  // A regenerated window makes EVERY cached chunk stale, so the chunk cache
  // goes with the snapshot. It did not, and the fetch path's version guard
  // (`cc.version <= sl.tick`, KickReadback) then kept the old contents for
  // any later reader whose tick numbers were LOWER than the gate that filled
  // the entry: the body-stain gate (ticks 29000+) after corpse-bleed (61000+)
  // wrote stone and blood the GPU held and the CPU mirror never showed, and
  // reported a contact pass that "saw nothing" (2026-09-13).
  void InvalidateSnapshot() {
    snap_.valid = false;
    cache_.clear();
  }

  // ---- MPM fluid render bounds (RenderParams::fluidLo/fluidHi) ------------
  // Record this tick's CPU-known spawn cells so a fresh pour is visible on the
  // frame it lands, rather than when the block list gets back from the GPU a
  // few ticks later. Render-only, and NOT an input to any sim decision.
  // ---- CPU-authored gas spawns (docs/PLAN_gas_particles.md) --------------
  //
  // RULE 3, and it is why this is a queue rather than a buffer write: a gas
  // parcel created from the CPU is an INPUT OP, on the same footing as a
  // BrushOp or a ParticleSpawn. It rides the per-tick stream, the GPU forces
  // its liveness bits, and a replay of the stream reproduces it. Nothing here
  // touches a voxel or a particle buffer directly.
  //
  // Queue before the tick; SubmitTick drains the list into `gasSpawnOps` and
  // tells Simulation how many there were (the C_GAS latch has to know, or the
  // pass that consumes them may not be recorded). Over kGasCpuSpawnPerTick in
  // one tick is REFUSED, not silently truncated at the far end: the budget is
  // charged where the caller can see it.
  //
  // Returns how many were accepted.
  uint32_t QueueGasSpawns(const GasSpawnOp* ops, uint32_t n);
  void TakeGasSpawns(std::vector<GasSpawnOp>& out);
  uint32_t PendingGasSpawns() const { return (uint32_t)pendingGasSpawns_.size(); }

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
  // The world map's biome for a column: the CPU twin of worldgen.wgsl's
  // mapBiomeAt (src/sim/worldmap.h). Reads worldmap::CurrentWorldMap(); 0
  // until a map is loaded. The `worldmap` gate holds the two together.
  static uint32_t MapBiomeAt(int x, int z, uint32_t seed);

  // The tarn a column stands in or beside. Exists so the `terrain` gate can
  // assert the BERM INVARIANT — every column in the berm core is above its
  // pond's waterline — which is what replaced pondSurface's 24-sample rim
  // minimum with a structural guarantee. Reports `near` only where landColumn
  // would actually apply the berm, so a passing assertion means exactly what
  // the shader promises and nothing looser.
  struct PondQuery {
    bool inDisc = false;   // this column is under a tarn's water
    bool near = false;     // outside a disc but inside the berm/shore scan band
    int past = 0;          // voxels beyond that disc's rim (0 = first column outside)
    int surf = -1;         // that tarn's water surface Y
    // P-F: the tarn's preset and the geometry the gates assert against --
    // the berm core height/width and the shore/berm scan band, voxels.
    uint32_t preset = 0;
    int bermH = 0, bermW = 0, band = 0;
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
    // P-F: the preset the pond wears and the geometry it gives the bowl --
    // what the basin registry's curve and the gates read instead of the
    // deleted worldgen.pond* knobs. Voxels; fillId 0 = a dry preset.
    uint32_t preset = 0;
    int depth = 0, rimDepth = 0, bermH = 0, bermW = 0, band = 0;
    uint32_t fillId = 0;
  };
  static PondDisc PondTile(int tileX, int tileZ, uint32_t seed);
  // Tile pitch in world cells: the one pond lattice (worldmap.h kHPondTile),
  // 0 when no biome authors water. Exposed so a caller can turn a box into a
  // tile range without taking a dependency on the map struct.
  static int PondTileSize();
  // The AUTHORED lakes of the loaded map (worldmap.h kSiteWater; P-F), by
  // index over the map's water sites, with the same waterline the shader
  // gives them. The registry adds these beside the rolled tarns.
  static int WaterSiteCount();
  static PondDisc WaterSiteDisc(int index, uint32_t seed);
  // The mirrored bowl depth at squared distance d2 from the centre of a disc
  // of radius r wearing `preset`: the basin curve inverts THIS by bisection
  // rather than re-deriving the profile (worldmap.h explains the knots).
  static int BowlDepth(uint32_t preset, int r, int d2);
  // The widest disc + band any pond can reach, voxels: a probe walking out
  // of a tarn to its berm core stops after this many columns at most.
  static int PondReachMax();

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
  static constexpr int kAuthoredPools = 1;
  // The harness pad box from the world map (worldmap.h): the CPU twin of
  // worldgen.wgsl's inHarness, for fixtures that want to know they are on it.
  static bool InHarness(int x, int z);
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
  // ---- shadow cache (the kShadowCacheBuckets block above) ----
  // The ONLY buffer a fragment shader writes. Render-only derived data: never
  // hashed, never saved, and zeroed rather than restored on load — a cold cache
  // costs one frame of stale shadows, which is the same thing a teleport costs.
  rhi::Buffer shadowCache;    // kShadowCacheBuckets * 2 u32 (8 MiB)
  rhi::Buffer shadowReq;      // header + kShadowReqCap * 4 u32 request records
  // The penumbra window (the kShadowHistBytes block above), one word per
  // bucket. Written and read by the resolve pass alone; the fragment shader
  // never touches it, which is the point of it being a separate buffer.
  rhi::Buffer shadowHist;     // kShadowCacheBuckets u32 (4 MiB)
  // ---- openness grid (the kOpenFaces block above) ----
  // Same standing as the shadow cache: render-only, derived, disposable, never
  // hashed or saved. CopySrc so `--gate openness` can read a block-face back;
  // CopyDst so both can be zeroed at creation (an unzeroed generation word
  // would read as a valid stamp for whatever garbage the driver left).
  rhi::Buffer openness;       // kNumChunks * kOpenWordsPerChunk u32 (12 MiB)
  rhi::Buffer opennessGen;    // kNumChunks u32 — the world-chunk stamp per slot
  // ---- irradiance grid (the kIrradianceBytes block above) ----
  // Written by the shadow resolve pass (compute, per frame) and the openness
  // walk (compute, per tick), read by the raymarch's gather; keyed like
  // `openness` and stamped by `opennessGen`. CopySrc for `--gate gi-bounce`.
  rhi::Buffer irradiance;     // kNumChunks * blocks * faces u32 (48 MiB)
  // ---- glow field (the kGlowBytes block above) ----
  // Same standing again: render-only, derived, disposable, never hashed or
  // saved, no sim binding. CopySrc so `--gate glow` can read a source word and
  // a field word back; CopyDst so it can be zeroed at creation and on load —
  // an unzeroed stamp would read as valid for whatever the driver left, which
  // is the failure mode the openness grid's own comment names.
  rhi::Buffer glow;           // kGlowSrcWords + kGlowFieldWords u32 (8.5 MiB)
  // 1 bit per voxel + one per-slot tick stamp; see the kReposeSnap* block.
  rhi::Buffer reposeSnap;     // kReposeSnapWords u32 (16.125 MiB)
  // The second buffer a fragment shader writes, and the same argument applies:
  // measurement-only counters (kRenderStat* above), read back by the telemetry
  // path through rhi::CommandEncoder::CopyRenderWritten. 4 KiB.
  rhi::Buffer renderStats;    // kRenderStatWords atomic<u32>
  // The resolve's dispatch size, in the two-buffer stage+copy shape argsStage /
  // dispatchArgs and fluidArgsStage / fluidDispatchArgs already use: the
  // compute pass WRITES the stage (so it must be bindable storage) and the
  // indirect read comes from a buffer that is in no bind group at all.
  rhi::Buffer shadowArgsStage;  // 4 u32 — prepare kernel writes (x = groups, y = z = 1)
  rhi::Buffer shadowArgs;       // 4 u32 — indirect-only copy of shadowArgsStage
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
  // ---- W2: the surface-momentum store (kWaterFluxWords above) ----
  // kWaterFluxColumns * kWaterFluxWords u32 (6 MiB at 512^2). Its own binding
  // because it is per COLUMN and the ledger is per BODY. Derived data like the
  // page table: not hashed, not saved, and safe across a window shift through
  // the per-column stamp rather than through an invalidation pass. CopySrc for
  // `--gate waterbody` pass S; CopyDst so a load/reset can zero it.
  rhi::Buffer waterFlux;

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

  // ---- gas particles (docs/PLAN_gas_particles.md stage 1) ----
  // Their OWN pool, paged with the same parity convention as `particles`:
  // gasParticles[Page()] is the buffer this tick READS, [1-Page()] the one it
  // writes, and after FlipPage the roles swap. The two populations never share
  // a buffer, a count word or a claim slot — see kGasParticleCap.
  rhi::Buffer gasParticles[2];  // kGasParticleCap Particle (32 B)
  rhi::Buffer gasCounts;        // 4 u32: [0]/[1] = live count per page
  rhi::Buffer gasClaim;         // kGasClaimSize u32 — re-entry claim hash
  // The CA's outbox AND this tick's gas counters (the 8-word header). Cleared
  // whole before the CA runs, appended to by sim_step's gasLeave, drained by
  // sim_gas's spawn pass later in the same tick.
  rhi::Buffer gasSpawn;         // kGasSpHdr + kGasSpawnPerTick*8 u32
  rhi::Buffer gasSpawnOps;      // same shape, CPU-authored (kGasCpuSpawnPerTick)
  rhi::Buffer gasArgs;          // 8 u32: [4..6] dispatch args
  rhi::Buffer gasDispatchArgs;  // 3 u32, indirect-only (see dispatchArgs note)
  // The outer density box: RENDER-ONLY derived data. Not hashed, not saved,
  // rebuilt from scratch every tick. CopySrc so a gate can read it back.
  rhi::Buffer gasOuter;         // kGasOuterWords u32 (two u16 counts each)
  // The far fire-plume emitter list (see kGasFarEmitMax). CPU-built from the
  // eviction harvest, uploaded only on the ticks it CHANGES, read by
  // sim_gas's `gasFarPlume`. Part of the per-tick input stream in shape, but
  // its only product is gasOuter, so nothing here is hashed.
  rhi::Buffer gasFarEmit;       // kGasFarEmitWords u32 (fine + wide sections)
  // The LONG-RANGE density box (see kGasFarOuterN). Same standing as gasOuter
  // in every respect — render-only, not hashed, not saved, cleared and
  // re-splatted on every tick that has a wide emitter — and differing only in
  // its cell size. CopySrc so a gate can read it back.
  rhi::Buffer gasFarOuter;      // kGasFarOuterWords u32 (two u16 counts each)
  // The FLAME box (see kGasFlameWords): both LODs of the synthesized emissive
  // flame in one allocation, fine grid first, wide grid at kGasFlameWideBase.
  // Exactly the standing of the two boxes above — render-only derived data,
  // not hashed, not saved, cleared and re-splatted on the ticks it is written
  // — and, like them, CopySrc so a gate can read it back.
  rhi::Buffer gasFlame;         // kGasFlameWords u32 (two u16 counts each)

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
  rhi::Buffer fluidArgsStage;    // kFluidArgsWords u32 — the FA_* word map:
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
  // THE DEFERRED-WAKE SIDE CHANNEL (docs/RESEARCH_streaming_hitch.md R1).
  // One u32 per genList POSITION (not per slot): 1 if genChunk found a cell
  // that matCanAct in that entry's chunk, 0 otherwise. It exists so the CA
  // wake of a streamed-in plane can be decided on the GPU at tick T and
  // ENACTED by the CPU at tick T+kWakeLatency, which is what removes the
  // 33 ms `omap.Wait()` fence from Stream::FillSlots. Only written when
  // TickParams::genDeferWake is set, i.e. only by a window shift — full-window
  // worldgen and the voxregion tool still wake in-kernel at T.
  //
  // Sized to ONE SHIFT PLANE, because that is the only producer. A caller that
  // defers a larger genList would run off the end, so Stream asserts the bound
  // rather than sizing this at kNumChunks "just in case".
  rhi::Buffer genAct;          // deferred-wake act verdict, per genList slot
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

  // The CPU's far fire-plume emitter index (src/sim/farplumes.h), owned by
  // Stream beside `farEdits` and fed by the same eviction harvest — the one
  // place the CPU ever sees an evicted chunk's voxels. Forward-declared for
  // farEdits' reason. Null before Stream::Init, and null-checked by SubmitTick
  // (no index = no emitters = exactly today's behaviour).
  class FarPlumes* farPlumes = nullptr;

  // ---- THE SNAPSHOT READBACK RING, SIZED FROM THE PIPELINE (P2-D) ---------
  //
  // Public because it is the bound on how many deliveries a caller draining
  // toward a FRESH snapshot can ever need — SubmitTick's paged staleness
  // fallback loops against it. The slots themselves stay private.
  //
  // It was the literal 3, and a literal cannot survive a pipeline that got
  // deeper. A snapshot slot is occupied from the tick that ENCODES it until
  // the submit that produced it retires, so the ring has to cover every tick
  // that can be in flight at once: the frame loop runs up to kMaxTicksPerFrame
  // ticks per frame, and the swapchain lets the GPU run kFramesInFlight frames
  // behind the CPU plus the frame being recorded. At 3 slots against 16
  // possible in-flight ticks, EncodeReadbacks declined on 128 of 540 frames
  // of `--frames 600 --autofly-surface` and 83 of the 157 staleness stalls
  // happened on a tick for which no copy had been encoded at all — the ring,
  // not the GPU, was the reason the mirror could not be refreshed.
  //
  // COST: one slot is a mapped host-visible staging buffer of
  //   27 x 16 KiB mirror + 128 KiB dirty + 128 KiB occupancy + 128 KiB support
  //   + 108 KiB fluid mirror + ~2 KiB of small tables
  //   + kFetchPerTick x 16 KiB chunk fetches (1 MiB, the biggest single term)
  // = 1,997,056 B = 1.904 MiB. 16 slots is 30.5 MiB, up from 5.7 MiB at three.
  // That is against a 360 MiB page pool and a 512 MiB voxel binding, and it is
  // the price of never refusing a snapshot the mirror's freshness depends on.
  //
  // Both factors are MIRRORS OF SOMEBODY ELSE'S CONSTANT and are checked:
  // main.cpp's tick loop uses kMaxTicksPerFrame directly (one definition), and
  // scripts/check_invariants.py compares kFramesInFlight against
  // rhi_vulkan.h's kAcquireSlots — world.h must not include a backend header
  // to read it.
  static constexpr int kMaxTicksPerFrame = 4;   // main.cpp frame-loop cap
  static constexpr int kFramesInFlight = 3;     // == rhi_vulkan.h kAcquireSlots
  static constexpr int kReadbackSlots =
      kMaxTicksPerFrame * (kFramesInFlight + 1);

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
  static constexpr int kSlots = kReadbackSlots;
  Slot slots_[kSlots];
  int lastSlot_ = -1;
  WorldSnapshot snap_;
  IVec3 origin_{0, 0, 0};
  // Drained into gasSpawnOps by SubmitTick, once, at the head of the tick.
  std::vector<GasSpawnOp> pendingGasSpawns_;

  // Recent CPU-side fluid spawn box (render bounds only — see
  // NoteFluidSpawnBounds). Held for kFluidSpawnBoundsTicks so a pour is
  // visible from the frame it lands until the snapshot's block list catches
  // up; after that the block list is the better (tighter) answer.
  static constexpr uint32_t kFluidSpawnBoundsTicks = 12;
  IVec3 spawnBoxLo_{0, 0, 0};
  IVec3 spawnBoxHi_{-1, -1, -1};
  uint32_t spawnBoxTick_ = 0;

  struct FetchReq {
    IVec3 wc;
    uint32_t tick;  // fetchTick_ when asked (see FetchProbe's age note)
    uint8_t src;    // FetchSource
  };
  std::deque<FetchReq> fetchQueue_;
  std::unordered_map<uint64_t, uint8_t> fetchQueued_;   // dedup (packed key)
  uint32_t fetchTick_ = 0;  // tick of the last EncodeReadbacks: request stamp
  FetchProbe fetchProbe_{};
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
