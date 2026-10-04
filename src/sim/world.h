#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gpu/rhi.h"

#include "math3d.h"
#include "sim/rng.h"   // rng::Hash3 — the JITTER palette-variant formula
#include "sim/elec.h"  // the body query's ElecQuery / ElecHit (package E4)

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
// P0 shipped kTicketMax = 0 (bit-identical, the addressing audit only). P1
// (docs/PLAN_chunk_tickets.md §3) raises it: 16 tickets x 125 slots = 2,000,
// rounded to 2,048 ticket slots. Ticket i owns the FIXED slot range
// [kNumChunks + i*kTicketChunks, +kTicketChunks); inside it a box chunk wc
// lives at the slot its coordinate takes MODULO the box edge (the window's
// own toroidal rule, at kTicketBoxN), so a ticket re-centres one plane at a
// time exactly as the window shifts (P4). The live boxes are a CPU-built
// table uploaded into the tail of `pageTable` (kTicketTable* below); see
// src/sim/tickets.h for the lifecycle.
constexpr uint32_t kTicketMax = 16;         // concurrent tickets (the cap)
constexpr uint32_t kTicketBoxN = 5;         // 5^3 chunk box, inner 3^3 active
constexpr uint32_t kTicketChunks = kTicketBoxN * kTicketBoxN * kTicketBoxN;  // 125
// Rounded up to a multiple of 64 because sim_compact dispatches 64 slots per
// workgroup over the whole slot space and kNumChunks is already a multiple of
// 64; a ragged tail would need a second bound in the shader.
constexpr uint32_t kTicketSlots =
    ((kTicketMax * kTicketChunks + 63u) / 64u) * 64u;          // 0 at kTicketMax=0
constexpr uint32_t kNumSlots = kNumChunks + kTicketSlots;      // 34816 at P1
static_assert(kNumSlots % 64 == 0,
              "sim_compact dispatches kNumSlots/64 workgroups of 64");
static_assert(kTicketMax >= 1, "the ticket table and RenderParams size arrays by it");
static_assert(kTicketBoxN >= 3, "a ticket needs an active interior inside its shell");

// ---- THE TICKET TABLE: the tail of the `pageTable` buffer ------------------
// docs/PLAN_chunk_tickets.md §2.2 asked for a separate GPU-read-only map bound
// into every sim shader. It rides the page table's buffer instead, past the
// kNumSlots entries: every kernel that resolves a cell already binds
// `pageTable` (read-only on the GPU everywhere), so the map costs no binding,
// no bind-group layout and no pass-table row. CPU-built by `Tickets`, uploaded
// between ticks as one deferred write, GPU read-only (the plan's contract).
//
// Layout, in u32 words from kTicketTableBase (= kNumSlots):
//   [0]                         live count n (the compact list's length)
//   [1..3]                      reserved
//   [4 + 4*j], j < n            the COMPACT LIVE LIST: box lo x, y, z (world
//                               chunks), ticket index. What ticketSlotOf
//                               scans: a resolution costs `n` box tests, so a
//                               world with no ticket pays one load.
//   [4 + 4*kTicketMax + 4*i]    PER INDEX i: box lo x, y, z, live (0/1). What
//                               the inverse (slot -> world chunk) and the
//                               active test read.
// A box test rather than an open-addressed hash: 16 boxes, each a 5^3 of
// chunks, so "is wc inside box j" is three compares and the slot inside the
// box is arithmetic (mod 5). The plan's hash existed to map 2,000 arbitrary
// chunks; tickets are never arbitrary chunks, they are 16 boxes.
constexpr uint32_t kTicketTableBase = kNumSlots;
constexpr uint32_t kTicketTableListOff = 4;
constexpr uint32_t kTicketTableIdxOff = 4 + 4 * kTicketMax;
constexpr uint32_t kTicketTableWords = 4 + 8 * kTicketMax;
// The whole `pageTable` buffer: the per-slot entries plus the ticket table.
constexpr uint32_t kPageTableWords = kNumSlots + kTicketTableWords;

// THE TICKET POLICY'S TIME CONSTANTS (§2.4). In ticks, because the release
// clock is the published snapshot (World::Snap(), a fixed kSnapshotLatency
// behind), and both are pure functions of the tick: a release lands on the
// same tick in every run.
//   kTicketIdleTicks    all 27 ACTIVE chunks clean in this many consecutive
//                       published snapshots -> release. 30 = 1 s.
//   kTicketMaxTicks     released regardless after this long: the portal-ticket
//                       shape, so a waterfall spraying debris into one cannot
//                       pin it. 1,800 = 60 s.
//   kTicketRecentreTicks a ticket re-centres (P4) at most this often.
// Selftest thresholds that depend on these live in tests/baseline.json.
constexpr uint32_t kTicketIdleTicks = 30;
constexpr uint32_t kTicketMaxTicks = 1800;
constexpr uint32_t kTicketRecentreTicks = 60;

// ---- the per-chunk digest table (docs/PLAN_multiplayer_m9.md M9.3-A) ------
// One word per slot, plus ONE trailing word carrying the `tick` of the pass
// that wrote the table. The stamp rides the buffer rather than a C++ side
// channel because the buffer is what the readback copies: a table and the tick
// it describes that arrive in the same copy cannot disagree.
constexpr uint32_t kChunkHashTickWord = kNumSlots;
constexpr uint32_t kChunkHashWords = kNumSlots + 1;
constexpr uint64_t kChunkHashBytes = (uint64_t)kChunkHashWords * 4;

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
                   // 123, not 122: `brain` was inserted at 122 on 2026-09-17
                   // (6b8623f) and this row went unmoved, so the --shot flora
                   // painter drew two toadstools out of brain until the
                   // `plants` gate caught it. The gate now resolves every
                   // plant id by NAME and prints when one of these drifts.
                   kMatMushroomLarge = 123;

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
//
// IDENTITY AND ORDER. An op carries no tick and no sequence number: its
// identity is (tick, index in this vector), and the index is its PRIORITY.
// CPU push order is deterministic (a fixed sequence in the tick body), and
// since docs/PLAN_multiplayer_now.md N3 the shader honours it — sim_mutate's
// `main` gives a contested cell to the LOWEST op index that covers it and
// would write it, the same rule sim_explode's `apply` has always used. Two
// brush ops on one cell in one tick therefore have a defined winner.
//
// `_p0/_p1` are NOT spare: the spell transmute's from-filter owns them
// (game/spell.cpp Convert). The AUTHOR of an op lives in a CPU-side side table
// (sim/oprecord.h's OpMeta) rather than here, because the GPU has no use for
// it and this struct is full.
struct BrushOp {
  int32_t x, y, z;
  int32_t radius;
  uint32_t material;
  uint32_t mode;  // 0 = paint into air, 1 = overwrite
  // pad0: a transmute's FROM filter; pad1 bit 0: its wildcard-matches-matter
  // flag; pad1 bits 4..7: a powder brush's GRAIN size in eighths (0 = whole
  // cells, 15 = MIXED: each cell a crumble) -- sim_mutate.wgsl reads all three
// by the names _p0/_p1.
  uint32_t pad0 = 0, pad1 = 0;
};
constexpr uint32_t kBrushGrainShift = 4;
constexpr uint32_t kMaxOpsPerTick = 64;

// Must match ExplosionOp in common.wgsl (32 bytes). Part of the MutationQueue
// discipline: explosions enter the sim only through this op stream, so saves/
// replays/networking capture them for free (DESIGN.md §2).
//
// `author` was `pad0`. It is the ONE op that carries its author in the uploaded
// record rather than only in the CPU side table, because an explosion is the
// one op that FANS OUT — it destroys a ball of cells and ejects particles from
// them, so "who did this" is the question asked about it most often and the
// word was free. Zero = the local player. The shader ignores it; the layout is
// unchanged (three pad words became two plus a named one).
struct ExplosionOp {
  int32_t x, y, z;
  int32_t radius;   // <= kMaxExplosionRadius
  int32_t power;    // hardness budget at the center
  uint32_t author = 0;
  uint32_t pad1 = 0, pad2 = 0;
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
constexpr int kDirtyReasonBits = 32;
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
    "gas-lat",    "gas-edge",
    // docs/PLAN_solutes.md: dissolved mass still moving -- a pair exchange,
    // a dissolve waiting for its page, or a boundary gradient a neighbour
    // owns (sim_solute.wgsl / sim_step.wgsl DIRTY_R_SOLUTE). Not in
    // FILM_LICENCE: solute moving is not liquid progress.
    "solute",
    // ...and its back-check: a pair the -axis neighbour owns has work, so the
    // owner is woken for a whole phase cycle (sim_solute.wgsl DIRTY_R_SOLBACK).
    "solute-back",
    // docs/PLAN_temperature.md: the chunk's local temperature moved this tick
    // (sim_heat.wgsl heatRelax DIRTY_R_HEAT), so the CA re-reads its cells'
    // heat. Not in FILM_LICENCE: heat moving is not liquid progress.
    "heat",
    // sim_step.wgsl stainDry: a wet stain DRYING (idle: covered, still wet),
    // and a drying step that wrote. Split off stain-idle / STAIN-WROTE
    // 2026-10-03 because those also mean a liquid SOAKING IN, which spends
    // liquid; drying touches none, and sim_waterbody.wgsl wbQuiet must not
    // read a drying bank as a disturbed body (DIRTY_R_DRY / DIRTY_R_DRYW).
    "dry", "DRY-WROTE",
    // docs/PLAN_electricity.md: the chunk holds charge (sim_elec.wgsl
    // elecSettle DIRTY_R_ELEC), so the CA -- and with it the charge field's
    // own rows -- keeps running while any exists. Not in FILM_LICENCE.
    "elec"};

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
static_assert(DirtyReasonBit("elec") == (1u << 31),
              "sim_elec.wgsl DIRTY_R_ELEC is bit 31; kDirtyReasonName must name it there");
static_assert(DirtyReasonBit("dry") == (1u << 29) &&
                  DirtyReasonBit("DRY-WROTE") == (1u << 30),
              "sim_step.wgsl / sim_waterbody.wgsl DIRTY_R_DRY / DIRTY_R_DRYW are "
              "bits 29 and 30; kDirtyReasonName must name them there");

// Particle system sizes — must match common.wgsl.
constexpr uint32_t kParticleCap = 262144;
constexpr uint32_t kClaimSize = 262144;
// The claim buffer holds kClaimWords u32: the reinsertion claim itself in
// [0, kClaimSize), then five more planes for GRAIN LANDING (sim_particle.wgsl:
// every same-material grain aimed at one cell lands in the SAME tick): the
// summed mass, and the max / max-of-complement of a two-word key that proves
// every proposer to the slot agreed on (cell, material, cell mass).
constexpr uint32_t kClaimWords = kClaimSize * 6;

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
// The size of the CA's outbox, and the CEILING on a tick's leave budget.
//
// UNTIL 2026-10-03 this was sized to be unreachable, for RULE 1: a conversion
// charged a shared atomicAdd cursor against it, so WHICH voxels were refused
// when the list filled was decided by which workgroup arrived first -- and a
// refused voxel STAYS IN THE GRID, where the world hash sees it. The pool's
// overflow had the same shape one step later (which parcel vaporized was
// whichever thread's atomicAdd came last, and a vaporized parcel can never
// re-enter). Both are closed now and the cap is only a size:
//   * sim_gas `gasLeavePrep` fixes the tick's budget BEFORE the CA:
//     min(kGasSpawnPerTick, pool room after the live parcels and the CPU list);
//   * sim_step `camask` counts the dirty chunks that touch the residency edge
//     (kGasSpEdgeChunks) and each gets budget / count, spent in dispatch order
//     (the per-chunk words at kGasSpChunkBase);
//   * within one dispatch a chunk is one workgroup's, which collects its
//     leavers and, over its share, accepts the lowest by a per-cell hash.
// Every refusal is a function of the world. `gas-leave-overflow` forces the
// overflow (kGasOpsLeaveCap) and checks the twice-run series agree.
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
// lists, with weights the kernels multiply into the mass: the wide twin fades
// in over the first half of the shell at full size, the fine plume fades out
// over the second, and the renderer takes the per-sample MAX of the two boxes,
// so visibility never dips while the look hands over (2026-09-30; it used to
// be complementary weights summed, which dipped -- each half is eroded alone).
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
// THE PLUME TRACKS (2026-09-30): each far plume's lateral offset per height
// cell, CARRIED from tick to tick (sim_gas.wgsl plumeTrack). A stateless column
// leaning by the wind of THIS tick swings as one rigid line when the wind
// turns; a carried one shifts up by the tick's rise and adds the tick's drift
// at every height, so a change enters at the fire and climbs the column the
// way the CA's voxel smoke does. Keyed by the emitter's position in an
// open-addressed table (the emitter lists are re-ranked whenever the eye
// moves, so an index is no identity). Two tables, fine then wide.
// Render-only derived data: not hashed, not saved, and a lost slot only costs
// that plume a re-seed from the current wind.
constexpr uint32_t kGasPlumeTrackSlots = 1024;   // per table; >= 4x either list cap
constexpr uint32_t kGasPlumeTrackHdr = 4;        // key x, y, z, stamp
constexpr uint32_t kGasPlumeTrackStride = 132;  // hdr + a vec2 per height cell (FAR_PLUME_STEPS 64)
static_assert(kGasPlumeTrackStride == kGasPlumeTrackHdr + 2 * 64, "track slot layout");
constexpr uint32_t kGasPlumeTrackWords =
    2 * kGasPlumeTrackSlots * kGasPlumeTrackStride;   // 270,336 u32 = 1.03 MiB
static_assert(kGasPlumeTrackSlots >= 4 * kGasFarEmitMax &&
              kGasPlumeTrackSlots >= 4 * kGasFarEmitMaxWide,
              "the track table must stay sparse at a full emitter list, or "
              "probing fails and plumes fall back to a per-tick re-seed");
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
  // THE EDGE'S LEAVE BUDGET (see kGasSpawnPerTick). 9 is written by sim_gas's
  // gasLeavePrep before the CA, 10 by sim_step's camask (substep 0), 11 only if
  // the accounting is broken -- an accepted conversion found the list full.
  kGasSpBudget = 9,      // conversions this tick may make, whole window
  kGasSpEdgeChunks = 10, // dirty chunks touching the residency edge this tick
  kGasSpOverrun = 11,    // MUST stay 0
  // THE CA'S COST ATTRIBUTION (2026-10-03, fire-gpu): diagnostic, unhashed,
  // written by sim_step.wgsl's colour rows and camask (substep 0).
  kGasSpCaGas = 12,      // gas cells the colour rows ran, both substeps
  kGasSpCaOther = 13,    // non-gas, non-inert cells they ran, both substeps
  kGasSpGasOnlyCells = 14, // gas cells in gas-only awake chunks
  kGasSpGasOnly = 15,    // lo16 gas-only awake chunks, hi16 matterless ones
  kGasSpHdr = 16,      // first record word
  kGasSpStride = 8,    // u32 per record (a 32-byte Particle)
  kGasSpHdrBytes = kGasSpHdr * 4,
};
// Behind gasSpawn's record list: one budget word per SLOT (sim_step.wgsl
// GAS_SP_CHUNK0). Bit 31 = counted into kGasSpEdgeChunks this tick, bits 0..30
// = conversions made so far this tick. camask initialises the word of every
// chunk on the dirty list and the CA reads no other, so it is never cleared.
constexpr uint32_t kGasSpChunkBase = kGasSpHdr + kGasSpawnPerTick * kGasSpStride;
// gasSpawnOps header word 1: a TEST-ONLY ceiling on the tick's leave budget,
// 0 = none (World::SetGasLeaveCapForTest). NOT part of the replay record: only
// `gas-leave-overflow` sets it, and it resets it before it returns.
constexpr uint32_t kGasOpsLeaveCap = 1;

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
// allocated once per TICK by a deterministic scan (the FluidMap table is
// recorded once, not per substep — see simulation.cpp's seam recording). 256 blocks * 4096 nodes *
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
  // Spawn FLAGS (nothing reads bits 0..7).
  // Bit 8 = GHOST (kFluidOpGhost, game/container.h), bits 16..23 = ghost
  // life in ticks. A particle has ONE identity, its material (`mat` below):
  // colour, splash, attraction and density all come from materials[mat].
  uint32_t flags = 0;
  uint32_t mat = 0;     // material id for the particle's attr word — settle
                        // writes this back as the voxel, splash droplets and
                        // staining key on it, the renderer colours it and the
                        // solver weighs it by materials[mat].density
};

// THE LIVE CREATURE CAP. MobSystem::kMaxMobs IS this constant; it lives here,
// in the GPU-layout header, because the body render slots and the micro-body
// model table below are sized from it, and so may anything in sim/ that sizes a
// per-creature table (sim/ cannot include game/mob.h). 16 until 2026-10-04
// (PLAN_electricity_wave2 package C). Counts the LIVING only: the dead have
// their own caps (MobSystem::kMaxDeadMobs / kMaxDeadBodies).
constexpr uint32_t kMaxLiveMobs = 64;
// Body render slots ONE living creature may need: its base limbs, a held item
// in each hand, worn garment shells and hair. The armed, dressed human is the
// largest shipped body (the `mob-cap64` gate records the real peak per
// creature as mobCap64.peakSlotsPerMob); a creature past this still renders up
// to kMaxBodySlots, it just eats the slack below.
constexpr uint32_t kBodySlotsPerLiveMob = 24;

// Rigid-body render slots shared by debris + mob limbs (BodyVoxInst packs the
// slot in bits 16..27, so the hard ceiling is 4096). Debris bodies take slots
// [0, debrisCount), the avatars' parts after them, then every mob limb (the
// dead included). Every walk stops at this ceiling and the LAST walked (the
// mobs) are who a crowded frame starves, so it is sized to the sum of:
//   debris            kMaxBodies (200, phys/debris.h)
//   the living        kMaxLiveMobs * kBodySlotsPerLiveMob (64 * 24 = 1536)
//   the dead          MobSystem::kMaxDeadBodies (240)
//   the avatars       a few dozen
// = ~2000, so 2048. 512 until 2026-10-04 — sized for 16 creatures; 64 armed
// humans alone are ~1100 limb bodies. VRAM: bodyXforms 32 B and
// microBodyInsts 16 B per slot, 96 KiB in all (was 24 KiB).
constexpr uint32_t kMaxBodySlots = 2048;
static_assert(kMaxBodySlots >= 200 + kMaxLiveMobs * kBodySlotsPerLiveMob + 240,
              "body render slots must cover debris + the living cap + the dead");
static_assert(kMaxBodySlots <= 4096, "BodyVoxInst packs the slot in 12 bits");

// CPU-authored render instance (grenades, markers) — must match Sprite in
// debris.wgsl (32 bytes). Render-only: floats are fine here.
struct Sprite {
  float pos[3];
  float halfSize;
  uint32_t color;   // 0xAABBGGRR
  float emission;
  // The MATERIAL this sprite is a piece of (0 = none: a marker, a spell
  // flash). When set, debris.wgsl vsSprite takes the glow from that
  // material's own emission (through burnTint, as a flying particle does) on
  // top of `emission`, so matter drawn as a sprite -- a scooped mote, an
  // applied dab -- glows exactly as the same matter does in the grid.
  uint32_t mat = 0;
  uint32_t pad1 = 0;
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
//
// DEDUPED CPU-SIDE, not in the shader. There is no room in eight bytes for a
// filter a shader could re-derive, and the `cells` dispatch is one invocation
// per op with no ordering between them — two ops on one cellIdx raced. So the
// choke point (support.cpp's SubmitTick) canonicalizes the stream before
// upload: keep-FIRST in push order, survivors in push order, which leaves a
// duplicate-free tick byte-identical to what the producers built. See
// sim/oprecord.h's CanonicalizeCells.
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
// THE CONDITIONAL CLEAR: "empty this cell, but only if it still holds material
// M". kCellOpIfAir on a word whose material is AIR would be "write air where
// there is air" -- a no-op nobody needs -- so that combination is reused, with
// M in bits 12..23 (sim_mutate.wgsl `cells`). It exists for the vessels
// (game/container.h): a scoop is decided off a snapshot one tick old, and
// liquid moves, so an unconditional clear would delete whatever flowed into
// the cell since -- sand, a plant, somebody's foot. Refused ops cost the
// scooper nothing but accuracy; they never cost the world matter.
inline uint32_t CellOpClearIfMat(uint32_t mat) {
  return kCellOpIfAir | ((mat & 0xFFFu) << 12);
}
// THE SOLUTE POUR (docs/PLAN_alchemy_chemistry.md contract 2.5, package G):
// "put `units` of dissolved species `species` into the liquid at or under this
// cell". A conditional clear always has bits 24..30 clear (CellOpClearIfMat),
// so kCellOpIfAir + material AIR + a NONZERO species in bits 24..30 is a third
// meaning of the same flag, with the units in bits 12..23 (<= 4095, whole
// eighths of the powder: SoluteDef::yieldPerVoxel / 8 each). It writes NO
// voxel of its own: sim_mutate.wgsl `cells` only wakes the chunk and asks the
// solute allocator for pages (the request flag), and `solPour` -- after
// solAlloc, ONE invocation walking these ops in push order -- lays the mass
// into the solvent under the cell, spreading it over the cells round it up to
// the species' saturation, and whatever finds no solvent (dry ground, oil, a
// layer switched off) PRECIPITATES there as its powder, which re-dissolves
// the moment water covers it. Conserved either way, counted either way
// (kSolMPoured / kSolMPourPowder).
//
// Exempt from the keep-first dedupe (oprecord.cpp CanonicalizeCells): two
// pours onto one cell are two deposits, not a race -- solPour applies them one
// after the other. SubmitTick moves them to the TAIL of the stream so that
// single invocation reads only them.
constexpr uint32_t kCellOpSoluteSpeciesMax = 127;
constexpr uint32_t kCellOpSoluteUnitsMax = 0xFFF;
// The most pour ops one tick applies: solPour is ONE invocation walking the
// stream's tail, so it is bounded here (sim_mutate.wgsl SOL_POUR_MAX_OPS,
// declared there and checked equal by check_invariants.py). SubmitTick clamps
// the stream to it, keep-first, and counts the refused ops and their units
// into last_run.json's opstream block (solPourTrunc / solPourUnitsTrunc). The
// producer (container.h kMaxSolutePoursPerTick) sends far fewer.
constexpr uint32_t kMaxSolutePourOpsPerTick = 256;
inline uint32_t CellOpSolute(uint32_t species, uint32_t units) {
  return kCellOpIfAir | ((units & 0xFFFu) << 12) | ((species & 0x7Fu) << 24);
}
inline bool IsSoluteCellOp(uint32_t word) {
  return (word & kCellOpIfAir) != 0 && (word & 0xFFFu) == 0 && ((word >> 24) & 0x7Fu) != 0;
}

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
// A FOURTH plane after the columns, per slot: the ray-blocker signature of the
// chunk at its last DIRTY walk (sim_openness.wgsl OPEN_SIG_BASE). A dirty chunk
// whose blockers have not changed is skipped by the dirty pass -- it is then
// exactly a chunk that was not dirty, which the touch plane already handles.
// That is a burning forest's smoke: thousands of chunks awake for gas, which
// is not a blocker (2026-09-22, 2.7 ms/frame of opennessDirty).
constexpr uint32_t kOpennessGenWords = 3 * kNumSlots + kNChunk * kNChunk;
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
//
// SLOT space, not window volume (chunk tickets): a ticket's slots are
// dispatched by the prepass and probed by the CA like window slots, so the
// bitfield and the stamps cover kNumSlots (+1 MiB at kTicketSlots = 2,048).
// Sized by the window alone, every ticket centre's ring decoded as window
// coordinates and stamped arbitrary window slots (the audit of 2026-10-02).
constexpr uint64_t kReposeSnapBitWords =
    (uint64_t)kNumSlots * kChunkVol / 32;                        // 17 MiB at 512^3
constexpr uint64_t kReposeSnapTickBase = kReposeSnapBitWords;    // + kNumSlots stamps
constexpr uint64_t kReposeSnapWords = kReposeSnapBitWords + kNumSlots;
constexpr uint64_t kReposeSnapBytes = kReposeSnapWords * 4;      // ~17 MiB
// The WGSL side derives both from constants the prelude already emits
// (NUM_SLOTS * CHUNK_VOL / 32u), so this needs no new prelude constant; the
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
// and seed (worldgen.wgsl genCellIn). So the chunk's 4,096 words are describable
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
//   [33..35] reserved (tally headroom: PT_K_COUNT may grow)
//   [36]     THE SCOOP LEDGER (game/container.h): eighths of a cell that
//            CONDITIONAL CLEARS (CellOpClearIfMat) actually removed, monotonic.
//            Not a fault; it lives here because sim_mutate already binds this
//            record atomically and the snapshot already copies it, so the
//            one number a vessel needs back from the GPU costs no binding.
//   [37]     conditional clears applied, monotonic
//   [38]     conditional clears REFUSED (the cell no longer held the material)
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
//
// [40..63] THE REACTION-EFFECT RECORD (docs/PLAN_alchemy_chemistry.md package
//          A): which rules WITH EFFECTS fired where, THIS tick. Not a fault
//          either; it lives here for the scoop ledger's reason (sim_step
//          already binds the record atomically and the snapshot ring already
//          copies it every tick, at the fixed latency World::Snap() owns). But
//          unlike the rest of the record it is PER TICK: pass_table.def's
//          fill_reactFx zeroes [40..63] before the CA, every tick.
//   [40]   rule-with-effects firings this tick (atomicAdd: an order-free sum)
//   [41..43] T.origin (chunks) the cells below are relative to (stored by
//          every firing thread; all store the same value)
//   [44]   T.tick + 1 (0 = nothing fired; the parse checks it)
//   [45]   LIQUID EIGHTHS EATEN BY CA REACTIONS this tick (atomicAdd): every
//          time a reaction (bucket rule, either side of a pair, or a thermal
//          transition) rewrites a SETTLED liquid voxel to a different
//          material, that voxel's fullness is added here. The CA-side twin
//          of the seam's FA_CONSUMED, which counts only EXCITED fluid eaten;
//          the two together close a reaction gate's mass ledger exactly
//          (fluid-react). Diagnostic: nothing in the sim reads it. Zeroed
//          with the rest of [40..63] by fill_reactFx.
//   [46..47] reserved
//   [48..63] kPageFaultReactFxSlots SLOTS, each an atomicMax of
//          (fxId << 27) | scramble(slot cell = world cell mod kWorldN). A firing picks its
//          slot by hash3(seed, tick, cell), so WHICH firings survive a busy
//          tick is a pure function of the set of firings -- never of the
//          order threads ran in (rule 1: an order-free reduction, not an
//          append cursor). ReactFxDecodeCell inverts the scramble.
//
// [64..127] THE TICKET RECORD (chunk tickets P2, docs/PLAN_chunk_tickets.md):
//          what the particle kernel tells the CPU about matter that came to
//          rest OUTSIDE residency. Per tick: pass_table.def's fill_ticketRec
//          zeroes [64..127] before the particles run. Same reasons as the
//          reaction-effect record for living here (sim_particle already binds
//          the record atomically; the snapshot ring copies it at the fixed
//          latency). ORDER-FREE throughout (rule 1): atomicMax sets, never an
//          append cursor.
//   [64..95]  kTicketReqBuckets REQUEST buckets: a parked particle atomicMax's
//          (its chunk, packed relative to the window origin, + 1) into the
//          bucket its key hashes to. 0 = empty. The CPU decodes each into a
//          ticket request (Tickets::Tick).
//   [96..125] kTicketDepSlots DEPOSIT slots of kTicketDepStride words:
//          priority (atomicMax by the bidders in integrate), then the winner's
//          px, py, pz (24.8) and payload, written by it alone in resolve.
//   [126]  particles killed for leaving the far box this tick (atomicAdd)
//   [127]  particles parked this tick (atomicAdd)
// sim_particle.wgsl's TK_* consts mirror these (check_invariants `ticket record`).
constexpr uint32_t kPageFaultTicketBase = 64;    // fill_ticketRec starts here
constexpr uint32_t kTicketReqBase = 64;
constexpr uint32_t kTicketReqBuckets = 32;
constexpr uint32_t kTicketDepBase = 96;
constexpr uint32_t kTicketDepSlots = 6;
constexpr uint32_t kTicketDepStride = 5;
constexpr uint32_t kTicketFarKilled = 126;
constexpr uint32_t kTicketParkedNow = 127;
constexpr int32_t kTicketReqBias = 64;           // chunk offset bias, 7 bits a side
static_assert(kTicketReqBase + kTicketReqBuckets <= kTicketDepBase, "ticket record overlap");
static_assert(kTicketDepBase + kTicketDepSlots * kTicketDepStride <= kTicketFarKilled,
              "ticket record overlap");
constexpr uint32_t kPageFaultWords = 128;
constexpr uint32_t kPageFaultBytes = kPageFaultWords * 4;
constexpr uint32_t kPageFaultScoopEighths = 36;
constexpr uint32_t kPageFaultScoopApplied = 37;
constexpr uint32_t kPageFaultScoopRefused = 38;
constexpr uint32_t kPageFaultReactFxBase = 40;    // the fill starts here
constexpr uint32_t kPageFaultReactFxFires = 40;
constexpr uint32_t kPageFaultReactFxOrigin = 41;  // 41..43
constexpr uint32_t kPageFaultReactFxTick = 44;
constexpr uint32_t kPageFaultReactLiquidEaten = 45;
constexpr uint32_t kPageFaultReactFxSlot0 = 48;
constexpr uint32_t kPageFaultReactFxSlots = 16;
static_assert(kPageFaultReactFxSlot0 + kPageFaultReactFxSlots == kPageFaultTicketBase,
              "the reaction-effect slots end where the ticket record begins; "
              "fill_reactFx (pass_table.def, 160,96) clears [40..63]");
// The snapshot ring gives this record a 512-byte slot (world.cpp
// kPageFaultOff .. kFluidArgsOff).
static_assert(kPageFaultBytes <= 512, "pageFaults outgrew its snapshot slot");

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

// ============================================================================
// THE SOLUTE LAYER (docs/PLAN_solutes.md P1-P3, DESIGN.md "Solutes").
//
// Dissolved matter is not a material: a powder that dissolves (salt, fairy
// dust) becomes MASS carried by the liquid cell it dissolved into. The voxel
// word is full, so the mass lives in a SPARSE AUX LAYER keyed by chunk slot,
// shaped like the voxel page table:
//
//   solTable[slot]  0                          SOL_EMPTY   (no solute anywhere)
//                   kSolUniformBit | value16   SOL_UNIFORM (every cell holds value16)
//                   kSolPageBit | page         a real page in solPool
//   solPool         kSolutePoolPages pages x kSolWordsPerPage u32; a cell is a
//                   16-bit value (species << 8 | mass), two cells per word,
//                   the EVEN local index in the low half.
//
// Mass is 0..255 at a full cell (8 eighths); concentration = mass * 8 /
// fullness is DERIVED and never stored (PLAN_solutes §1). Species is the
// 1-based index into solutes.json, 0 = none.
//
// WHY THE TABLE IS GPU-OWNED, unlike the voxel page table. The voxel table is
// derived data the CPU allocates ahead of every write it can foresee, and it
// needs a whole conservative-mirror recurrence to foresee them. The solute
// table cannot be foreseen from the CPU at all: a grain of salt touching water
// is a CA decision. So allocation is a GPU PASS at a fixed point in the tick
// (sim_solute.wgsl solWant/solAlloc, before the CA): every chunk within one
// chunk of a dirty chunk whose neighbourhood carries solute (or asked for it
// last tick) is given a page. Which page index a slot gets depends on atomic
// scheduling -- harmless, because the index is a memory address, never an
// identity: the hash, the save and every reader key on the SLOT and the cell.
// WHETHER a slot has a page is a pure function of the dirty set and the
// solute state, so it is deterministic, and it is the only thing any kernel
// branches on (a write into a non-page is refused and counted as a fault).
//
// Pages are handed back at the END of the same tick (solCompact) when a chunk
// turns out uniform or empty, so a settled lake is table entries, not pages.
//
// THE POOL SIZE IS DERIVED, never a literal (the kPoolPages rule): a page is
// needed only for a chunk MID-DISPERSION -- a uniform body is a sentinel -- and
// the dilution floor bounds how far a plume can spread (mass / floor cells).
// One-eighth of the window's slots is 4,096 pages = 32 MiB, ~64x the largest
// plume a vessel can pour. Exhaustion is a FATAL ABORT with a verdict
// (SubmitTick, support.cpp), because which slots would be refused is
// scheduling-dependent and a refused page would be a lost write.
constexpr uint32_t kSolWordsPerPage = kChunkVol / 2;        // 2048 u32 = 8 KiB
constexpr uint32_t kSolPoolDivisor = 8;
constexpr uint32_t kSolutePoolPages = kNumSlots / kSolPoolDivisor;  // 4096
constexpr uint32_t kSolUniformBit = 0x80000000u;
constexpr uint32_t kSolPageBit = 0x40000000u;
constexpr uint32_t kSolPageMask = 0x3FFFFFFFu;
static_assert(kSolutePoolPages < kSolPageMask, "page index must fit the entry");
// solMeta (one buffer, u32 words): a 64-word header, then the free stack,
// the want list + want flags, the request flags, the per-chunk stall
// counters and the per-chunk aggregates. sim_solute.wgsl's SOLM_* block
// mirrors every offset below (check_invariants.py `solute`).
constexpr uint32_t kSolMetaHdrWords = 64;
constexpr uint32_t kSolMFree = 0;        // free-stack depth (pages available)
constexpr uint32_t kSolMWantCount = 1;   // want-list append cursor this tick
constexpr uint32_t kSolMFaults = 2;      // solute store into a non-page (a BUG)
constexpr uint32_t kSolMExhausted = 3;   // pops that found the stack empty (FATAL)
constexpr uint32_t kSolMHighWater = 4;   // max pages in use, monotonic
constexpr uint32_t kSolMDissolved = 5;   // units dissolved, monotonic (wraps)
constexpr uint32_t kSolMPrecip = 6;      // units precipitated back to powder
constexpr uint32_t kSolMDiscarded = 7;   // units discarded (floor / destroyed / conflict)
constexpr uint32_t kSolMConverted = 8;   // units spent converting a solvent
constexpr uint32_t kSolMFaultSlot = 9;   // first fault: slot + 1
constexpr uint32_t kSolMFaultTick = 10;  // first fault: tick
constexpr uint32_t kSolMPoured = 11;     // units added by solute cell ops (pours)
constexpr uint32_t kSolMScooped = 12;    // units removed by scoop clears, monotonic
constexpr uint32_t kSolMSeamRefused = 13; // MPM excites refused: the cell carried solute
// Units a solute POUR op (CellOpSolute) found no solvent for and laid down as
// its powder instead (sim_mutate.wgsl solPour), monotonic. kSolMPoured counts
// the units that went into solution; the two together are every unit poured.
constexpr uint32_t kSolMPourPowder = 14;
// Units a pour op could put NOWHERE (no solvent, and no air cell within reach
// to precipitate into -- a pour aimed into solid rock), monotonic. Lost and
// counted; a gate asserts it stays 0.
constexpr uint32_t kSolMPourLost = 15;
// THE SOLUTE SCOOP LEDGER (docs/PLAN_alchemy_chemistry.md contract 2.5): units
// of each species a vessel's conditional clears took out of the world, one
// monotonic word per species 1..kSolScoopSpecies at [20 .. 20 + N), written by
// sim_solute.wgsl solScoop (which walks the tick's cell ops after
// sim_mutate's `cells` applied them). Read by TickAuthority beside the
// eighths ledger (pageFaults [36]); a species past N is not credited (its
// mass stays on the cleared cell and solCompact discards it, counted).
constexpr uint32_t kSolMScoopBySpecies = 20;
constexpr uint32_t kSolScoopSpecies = 8;
constexpr uint32_t kSolMArgs = 16;       // [16..18] solAlloc indirect args staging
// pass_table.def's copy_solArgs row copies from byte 64 as a literal.
static_assert(kSolMArgs * 4 == 64, "pass_table.def copy_solArgs offset");
constexpr uint32_t kSolMStackBase = kSolMetaHdrWords;
constexpr uint32_t kSolMWantList = kSolMStackBase + kSolutePoolPages;
constexpr uint32_t kSolMWantFlag = kSolMWantList + kNumSlots;
constexpr uint32_t kSolMReqFlag = kSolMWantFlag + kNumSlots;
constexpr uint32_t kSolMStall = kSolMReqFlag + kNumSlots;
constexpr uint32_t kSolMAgg = kSolMStall + kNumSlots;       // (species << 24) | mass
// The slots a window SHIFT (or a chunk replace) is about to give to another
// world chunk: sim_solute.wgsl solEvict clears their entries, hands their pages
// back and copies what they held into the eviction staging (kSolEvict* below).
// One plane of the window at most.
constexpr uint32_t kSolMEvictList = kSolMAgg + kNumSlots;
constexpr uint32_t kSolMEvictMax = kNChunk * kNChunk;
constexpr uint32_t kSolMetaWords = kSolMEvictList + kSolMEvictMax;
// The eviction staging (world.solStage): a 16-word header ([0] = records
// written, [1] = records REFUSED because the staging was full -- mass lost to
// the store and counted), then records of kSolRecWords: [slot, entry, 2048
// page words (a copy of the page; meaningless for a sentinel entry)].
constexpr uint32_t kSolRecWords = 2 + kSolWordsPerPage;
constexpr uint32_t kSolEvictRecords = 256;
constexpr uint32_t kSolStageHdrWords = 16;
constexpr uint32_t kSolStageWords = kSolStageHdrWords + kSolEvictRecords * kSolRecWords;
constexpr uint32_t kSolMetaHdrBytes = kSolMetaHdrWords * 4;
// solSpec: the species table uploaded from solutes.json (Simulation::
// UploadSolutes). A 16-word header, 16 words per species (1-based, so slot 0
// is unused), then one word per material: bits 0..7 the species this powder
// dissolves AS, bits 8..31 a mask of the species (1..24) it is a SOLVENT of.
constexpr uint32_t kSolSpeciesMax = 255;
constexpr uint32_t kSolSolventSpeciesMax = 24;   // the mask's width
constexpr uint32_t kSolSpecStride = 16;
constexpr uint32_t kSolSpecBase = 16;
constexpr uint32_t kSolSpecMatBase = kSolSpecBase + (kSolSpeciesMax + 1) * kSolSpecStride;
// ...then one word per compiled REACTION RULE (the side array PLAN_solutes
// §4.1 chose over a 36-byte ReactionGpu): bits 0..7 the species the rule's
// self cell must carry (0 = no condition), 8..15 cMin, 16..23 cMax, in
// concentration units. Indexed by the rule's GPU index (reactOffset + k).
constexpr uint32_t kSolSpecRuleBase = kSolSpecMatBase + 4096;
constexpr uint32_t kSolSpecRuleCount = 4096;   // >= kMaxReactions (asserted in simulation.cpp)
constexpr uint32_t kSolSpecWords = kSolSpecRuleBase + kSolSpecRuleCount;
constexpr uint32_t kSolConvertsMax = 4;          // converts rows per species on the GPU

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
// EXACT mirror of genCellIn's variant assignment (worldgen.wgsl) and of
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
// Mirrors worldgen.wgsl genCellIn: rnd = hash3(seed ^ 0xC0FFEE,
// x ^ (z << 12), y), state = rnd % 3. The bitcast to u32 of a negative
// coordinate is two's complement in both languages (the documented WGSL
// bitcast trap) — C++ gets it from the same (uint32_t) cast.
inline uint32_t JitterStateFor(int x, int y, int z, uint32_t seed) {
  const uint32_t rnd = rng::Hash3(seed ^ 0xC0FFEEu,
                                  (uint32_t)x ^ ((uint32_t)z << 12), (uint32_t)y);
  return rnd % 3u;
}
// ---- POWDER MASS (docs/PLAN_powder_mass.md; common.wgsl POWDER MASS) ------
// A powder's state nibble is its MASS in eighths: 0..2 = FULL (the old palette
// variant, so every existing creator, sentinel, save and fixture still means
// full), 3..9 = 1..7 eighths, 10..15 reserved (read as full). Mites
// (kMatFlagWander) never carry a partial. check_invariants.py pins these four
// to the WGSL constants.
constexpr uint32_t kPowderFull = 8u;
constexpr uint32_t kPowderPartialLo = 3u;
constexpr uint32_t kPowderPartialHi = 9u;
// Render-only: shadow/AO/occupancy blocker threshold (isRayBlockerW).
constexpr uint32_t kPowderBlockMin = 5u;
// CPU-only: a partial powder lighter than this is walked THROUGH — KindOfWord
// reports it as air to the player, mobs and projectiles (owner, 2026-09-26:
// "thin films should be walked through").
constexpr uint32_t kPowderWalkMin = 3u;
// CPU-only: a CREATURE's ground is a whole cell height (mob.cpp / pose.cpp
// carry it as an int), so a partial powder cell ROUNDS for them -- ground
// from half a cell of grains up; feet sink at most 3/8 or float at most 4/8.
constexpr uint32_t kPowderMobSupportMin = 4u;
inline bool PowderStateIsPartial(uint32_t s) {
  return s >= kPowderPartialLo && s <= kPowderPartialHi;
}
inline uint32_t PowderMassOfState(uint32_t s) {
  return PowderStateIsPartial(s) ? s - 2u : kPowderFull;
}
// POWDER ENTERS THE WORLD AS GRAINS -- the C++ twin of common.wgsl's block of
// that name (read it for which creator uses which). GRAIN = one eighth, for
// flows that split conserved matter; CRUMBLE = a hashed 1..8 eighths, for
// powder created or converted from something that was not powder.
constexpr uint32_t kPowderGrainState = 3u;
inline uint32_t PowderCrumbleState(uint32_t h) {
  const uint32_t m = 1u + (h % 8u);
  return m == kPowderFull ? (h >> 3) % 3u : m + 2u;
}
// State nibble for `mass` eighths (1..8) at world (x,y,z); full takes the
// positional variant, as common.wgsl powderStateFor does.
inline uint32_t PowderStateFor(uint32_t mass, int x, int y, int z, uint32_t seed) {
  return mass >= kPowderFull ? JitterStateFor(x, y, z, seed) : mass + 2u;
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

// The 4,096 words a SENTINEL chunk reads as, written chunk-linear into `dst`
// (index = (lz * 16 + ly) * 16 + lx, the voxel buffer's own order). `wc` is
// the WORLD chunk, because JITTER is positional. ONE copy of the three cases
// every whole-chunk synthesis site used to spell out for itself (the snapshot
// mirror, the fetch cache, ReadVoxelsSync's slow form):
//   - air, whatever the JITTER bit says: all zeros (SynthWordAt returns 0 for
//     air, so the row branch below would produce 0 | state << 12 instead);
//   - non-JITTER: one word, SynthWord(entry);
//   - JITTER: row order, JitterRowSeed hoisting Pcg(y) and the z term.
// Strictly derived from SynthWordAt, like the row helpers above — the
// page-roundtrip gate compares the two. RleEncodeSentinelChunk (stream.cpp)
// fuses this with its run scan and Classify VERIFIES rather than writes, so
// neither calls it.
inline void SynthChunkWords(uint32_t entry, IVec3 wc, uint32_t seed,
                            uint32_t* dst) {
  const uint32_t mat = entry & kPtMatMask;
  if (mat == kMatAir) {
    for (uint32_t i = 0; i < kChunkVol; i++) dst[i] = 0u;
    return;
  }
  if ((entry & kPtJitterBit) == 0u) {
    const uint32_t w = SynthWord(entry);
    for (uint32_t i = 0; i < kChunkVol; i++) dst[i] = w;
    return;
  }
  const int bx = wc.x * (int)kChunk, by = wc.y * (int)kChunk,
            bz = wc.z * (int)kChunk;
  const uint32_t stampBits = kStampNever << kStampShift;
  uint32_t i = 0;
  for (int lz = 0; lz < (int)kChunk; lz++)
    for (int ly = 0; ly < (int)kChunk; ly++) {
      const uint32_t rowSeed = JitterRowSeed(by + ly, bz + lz, seed);
      for (int lx = 0; lx < (int)kChunk; lx++, i++)
        dst[i] = mat | (JitterStateInRow(rowSeed, bx + lx, seed) << 12) |
                 stampBits;
    }
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
// A far cascade cell is ONE byte (common.wgsl FAR_PAL_BLOCKER): 0 is air,
// kFarPalBlocker is "no material, but the conservative blocker", and every
// other value is a FAR SLOT: an index into this run, where entry
// `kFarPaletteBaseGpu + slot` holds (in its `flags` word) the material id that
// slot paints. Until 2026-10-02 the byte was seven bits of slot plus a blocker
// FLAG, and 128 slots for ~207 materials meant 79 of them painted somebody
// else's colour at distance; the flag only ever said anything on a cell with
// no material, so it became one reserved value instead of a bit.
//
// Slots are assigned identity-first (material i takes slot i), so with fewer
// than kFarPalBlocker materials every material paints itself; `"far":
// "<material>"` in materials.json makes two share one. See LoadMaterials' slot
// assignment for the aliasing rules.
//
// 256 entries exactly fills the byte; entry kFarPalBlocker is never a slot.
constexpr uint32_t kFarPaletteSlotsGpu = 256;
constexpr uint32_t kFarPalBlocker = 0xFFu;
// A far cell byte's palette slot: 0 for air and for a blocker-only cell
// (common.wgsl farCellSlot).
constexpr uint32_t FarCellSlot(uint32_t b) { return b == kFarPalBlocker ? 0u : b; }
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
// 4 MiW = 16 MiB since 2026-09-25, for the random-human pool: its 20 bodies
// are 1.29M words of SHARED bricks, and up to kMaxMobs live creatures plus
// kMaxDeadMobs corpses (28) can hold all of them at once on top of the load
// (~0.5M) and the fight's clones (~0.8M, above). Unheld bodies are evicted
// (MobSystem::EvictUnusedDefs), so this is sized to what can be ALIVE, not to
// what a session has ever spawned. Changing it misses the SPIR-V cache once
// (MICRO_BODY_POOL_WORDS is in the constant prelude).
// 8 MiW = 32 MiB since 2026-10-04 (kMaxLiveMobs 16 -> 64). The clone term
// scales with the creatures FIGHTING, and the `mob-cap64` brawl (64 mixed
// creatures, 300 ticks) filled 4 MiW to the last word and was refused 269k
// times: every new wound on a fresh limb left its skin stale. +16 MiB VRAM.
// The gate records the peak live words it reaches (mobCap64.peakLivePoolWords).
constexpr uint32_t kMicroBodyPoolWordsWorld = 8u << 20;
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
// 2048 since 2026-09-25: the random-human pool made SHARED records a runtime
// population too -- every pool body built is ~30 more, held until nothing uses
// it (MobSystem::EvictUnusedDefs) -- so the table now has to cover the load,
// the pool bodies alive at once AND the owned clones their fights make. 32 KiB.
// 4096 since 2026-10-04 (kMaxLiveMobs 16 -> 64): the owned term is bounded by
// kMaxBodySlots, which went 512 -> 2048, so the shared ~1000 (load + every pool
// body) plus 2048 owned no longer fit 2048. 64 KiB, +32 KiB of VRAM.
constexpr uint32_t kMaxMicroBodyModels = 4096;
static_assert(kMaxMicroBodyModels >= kMaxBodySlots + 1024,
              "micro-body model table: shared records + one owned clone per slot");
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

// ---- THE WIND FIELD BLOCK (src/sim/windfield.h owns it; must match the wf*
// members of TickParams AND RenderParams in assets/shaders/common.wgsl) ------
//
// Everything about the ambient field that is not a compile-time TUNE_*
// constant and not the three weather words above: the gust-front advection
// clock, the regime's per-tick field parameters, the height profile as a
// lookup table, and the TERRAIN TABLE. Flat members in both structs rather
// than one nested struct, because check_invariants.py's layout comparison
// does not model a nested struct and would skip TickParams silently.
//
// THE TERRAIN TABLE is the one stored field in the wind system (DESIGN.md
// §9b): ground height, exposure (topographic position) and water fraction per
// 32-voxel column cell, 64 x 64 cells = 204.8 m centred on the window. It is a
// pure function of (seed, world cell, tuning) — worldgen height, not the live
// grid — so it is derived data: rebuilt, never saved, never hashed on its own.
// It rides the tick stream because the sim reads it, and a replay therefore
// reproduces it by construction.
//
// Cells per axis. Coverage must exceed the 51.2 m window: grass, arrows and
// streaks are drawn out past it, and a table only one window wide would put
// its fallback seam right at the window face.
constexpr uint32_t kWindTerrN = 64;
constexpr int32_t kWindTerrShift = 5;        // 32-voxel cells
constexpr int32_t kWindTerrCell = 32;
// kWindTerrN * kWindTerrN, a LITERAL for kWindPrimScalars' reason.
constexpr uint32_t kWindTerrWords = 4096;
static_assert(kWindTerrWords == kWindTerrN * kWindTerrN,
              "kWindTerrWords must equal kWindTerrN^2");
static_assert((1 << kWindTerrShift) == kWindTerrCell, "cell size is 2^shift");
// The height profile's lookup table: knot 0 is height-above-ground 0, knot k
// (k >= 1) is 2^(k-1) voxels, so the last knot is 2^14 voxels = 1.6 km. The
// profile is log-law, and doubling knots are where a log curve is flattest to
// interpolate linearly.
constexpr uint32_t kWindProfKnots = 16;

// ---- GUST STREAKS (assets/shaders/wind_streak.wgsl; render-only) ----------
// The fixed particle pool: kWindStreakCap slots of kWindStreakStride vec4f
// rows (position+age, life/strength/ring head/timer, then the trail ring of
// kWindStreakTrail positions). Must match STREAK_TRAIL_MAX / STREAK_STRIDE in
// the shader (check_invariants `windstreak`). ~590 KiB; never hashed.
constexpr uint32_t kWindStreakCap = 2048;
constexpr uint32_t kWindStreakTrail = 16;
constexpr uint32_t kWindStreakStride = 18;
static_assert(kWindStreakStride == 2 + kWindStreakTrail, "streak row layout");

// ---- WIND DRAFTS: the shelter volume (assets/shaders/sim_draft.wgsl) -------
// docs/RESEARCH_wind.md §14; DESIGN.md §9b "Drafts". A box of 4-voxel cells
// round the player in which the wind is REDIRECTED by geometry: what the volume
// stores is not a wind but the TRANSFER of one -- per cell, the 3-vector a unit
// horizontal wind along +X becomes there, and the one a unit wind along +Z
// becomes (a porous-media potential-flow projection, solved by multigrid). A
// pure function of the geometry inside the box and its origin, so it is
// re-solved only on a tick whose blockers changed and is never saved or hashed.
//
// Buffer `draft` (sim group binding 46), in WORDS:
//   [0, 2 cells)                    row masks per fine cell: w0 = X rows | Y
//                                   rows << 16, w1 = Z rows (4x4 rows of 4
//                                   voxels through the cell, bit = a blocker)
//   [kDraftCoarseBase, ...)          the coarse grid, one cell per CHUNK of the
//                                   box: phi x | phi z | K+ | K-bnd | pocket
//                                   members (64 bits, 2 words)
//   [kDraftFieldBase, + 3 cells)     THE FIELD: 6 x i16 Q12 per fine cell,
//                                   R_x.xy | R_x.z R_z.x | R_z.yz
//   [kDraftPhiA, + 2 cells), [kDraftPhiB, + 2 cells)
//                                   the fine passes' scratch potentials
//   [kDraftKBase, + 1 cell)          the fine faces' packed coefficients
//   THE STACK EFFECT (wind phase 5, 2026-10-02): the same projection solved
//   for a third right-hand side, the heat updraft b (common.wgsl
//   windHeatUpQ, Q12 of sim.windUpdraftCap) sampled per fine cell at the
//   solve's snapshot:
//   [kDraftStackCoarse, + 2 coarse)  per coarse cell: mean b of its pocket |
//                                   phi b
//   [kDraftStackB, + 1 cell)         b per fine cell (Q12), the snapshot
//   [kDraftStackPhiA / PhiB, + 1 cell each)  the fine passes' phi b scratch
//   [kDraftStackField, + 2 cells)    THE STACK FIELD: S = P(b) - b, 3 x i16
//                                   Q12 of the cap: S.x | S.y << 16, S.z
//   [kDraftStackLive]                1 when the published solve saw heat
// Buffer `draftMeta` (binding 47, atomics): kDraftMeta* words below.
// MIRRORED as DRAFT_* in common.wgsl (check_invariants `drafts`).
constexpr uint32_t kDraftCellShift = 2;   // a cell is 4 voxels (0.4 m)
constexpr uint32_t kDraftNX = 64, kDraftNY = 32, kDraftNZ = 64;  // fine cells
constexpr uint32_t kDraftCells = kDraftNX * kDraftNY * kDraftNZ;  // 131,072
constexpr uint32_t kDraftChunksX = (kDraftNX << kDraftCellShift) / kChunk;  // 16
constexpr uint32_t kDraftChunksY = (kDraftNY << kDraftCellShift) / kChunk;  // 8
constexpr uint32_t kDraftChunksZ = (kDraftNZ << kDraftCellShift) / kChunk;  // 16
constexpr uint32_t kDraftChunks = kDraftChunksX * kDraftChunksY * kDraftChunksZ;
// The coarse grid is one cell per chunk of the box (16 x 8 x 16).
constexpr uint32_t kDraftCoarseCells = kDraftChunks;
constexpr uint32_t kDraftCoarseWords = 6;
constexpr uint32_t kDraftCoarseBase = 2 * kDraftCells;
constexpr uint32_t kDraftFieldBase = kDraftCoarseBase + kDraftCoarseWords * kDraftCoarseCells;
constexpr uint32_t kDraftPhiA = kDraftFieldBase + 3 * kDraftCells;
constexpr uint32_t kDraftPhiB = kDraftPhiA + 2 * kDraftCells;
// The fine faces' coefficients, packed per cell (K+x | K+y << 9 | K+z << 18),
// computed once a solve so the four fine passes read one word a cell.
constexpr uint32_t kDraftKBase = kDraftPhiB + 2 * kDraftCells;
constexpr uint32_t kDraftStackCoarse = kDraftKBase + kDraftCells;
constexpr uint32_t kDraftStackB = kDraftStackCoarse + 2 * kDraftCoarseCells;
constexpr uint32_t kDraftStackPhiA = kDraftStackB + kDraftCells;
constexpr uint32_t kDraftStackPhiB = kDraftStackPhiA + kDraftCells;
constexpr uint32_t kDraftStackField = kDraftStackPhiB + kDraftCells;
constexpr uint32_t kDraftStackLive = kDraftStackField + 2 * kDraftCells;
constexpr uint32_t kDraftWords = kDraftStackLive + 4;
// The fine pass works one 8^3-cell TILE per workgroup.
constexpr uint32_t kDraftTiles = (kDraftNX / 8) * (kDraftNY / 8) * (kDraftNZ / 8);
// draftMeta words.
constexpr uint32_t kDraftMetaChanged = 0;   // a mask changed this tick (OR)
constexpr uint32_t kDraftMetaSolves = 1;    // solves since the buffer was made
constexpr uint32_t kDraftMetaLastTick = 2;  // tick of the last solve
constexpr uint32_t kDraftMetaCells = 3;     // mask cells changed, summed
constexpr uint32_t kDraftMetaStage = 4;     // the solve pipeline's stage (0 = idle)
constexpr uint32_t kDraftMetaArgs = 8;      // 6 stages x 4 words: each stage's indirect args
constexpr uint32_t kDraftStages = 6;
// The stack effect's re-solve trigger (sim_draft.wgsl `args`): heat moving is
// not a mask change, so these say when the box's own heat last moved (heat.h
// kHmDraftHeatClock) and what the last snapshot saw. Past the args records,
// which end at word 31; word 32 is unused.
constexpr uint32_t kDraftMetaHeatClock = 33;  // kHmDraftHeatClock at the last solve's start
constexpr uint32_t kDraftMetaBoxHot = 34;     // the last snapshot saw b != 0 (OR)
constexpr uint32_t kDraftMetaHeatStart = 35;  // tick the last solve started
constexpr uint32_t kDraftMetaWords = 40;
static_assert(kDraftNX % 8 == 0 && kDraftNY % 8 == 0 && kDraftNZ % 8 == 0,
              "the fine passes work whole 8^3-cell tiles (2 x 2 x 2 chunks)");
static_assert((kChunk >> kDraftCellShift) == 4,
              "sim_draft.wgsl's coarse cell is 4^3 fine cells");
static_assert(((kDraftNX << kDraftCellShift) % kChunk) == 0 &&
              ((kDraftNY << kDraftCellShift) % kChunk) == 0,
              "the volume is a whole number of chunks");
static_assert(kDraftChunksX <= kNChunk && kDraftChunksY <= kNChunk,
              "the volume must fit inside the residency window");

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
  // PADDING (was the per-species splash material table, retired by W1-B2: a
  // particle splashes as its own material). Keeps mirrorBase 16-byte aligned
  // at the same offset; common.wgsl TickParams._padSplash is its twin.
  uint32_t _padSplash[4] = {0, 0, 0, 0};
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
  // THE WEATHER, the SIM's copy (weather::SimRainWord; was the pad_wp1 pad
  // word, so the struct layout is unchanged). Bits 0..7 rain reaching the
  // ground, 8..15 the ignition damp strength, 16..23 ground wetness — see
  // materials.h kRain*. Read by reactions authored "rain" (douse) and
  // "rainDamped" (ignition), in the CA, the gas edge and the body burners.
  // 0 = a dry sky and every such rule takes its dry path.
  uint32_t weatherRain = 0;
  int32_t windPrimLo[3] = {1, 1, 1};   // union AABB of every live primitive,
  // THE RAIN SLOPE (weather::SimRainSlopeQ; was the pad_wp2 / pad_wp3 pair,
  // so the layout is unchanged): Q16 horizontal cells a drop drifts per cell
  // it falls. Integer, latched once per tick beside weatherRain and recorded
  // with it for ops-replay; 0 whenever weatherRain's amount is 0.
  int32_t rainSlopeQx = 0;             // inclusive world cells (lo > hi = none)
  int32_t windPrimHi[3] = {0, 0, 0};
  int32_t rainSlopeQz = 0;
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

  // ---- THE WIND FIELD BLOCK, the SIM's copy (kWindTerrN's note above;
  // windfield.h FillWindField is the only writer). Integers, on the tick input
  // stream: a replay reproduces them and the twice-run gate compares them.
  // RenderParams carries the identical member list, filled from the same
  // resolved values (advection/wander/thermal clocks at the frame's sub-tick
  // time instead of the tick's).
  uint32_t wfAdvPhase = 0;     // gust-front advection, BAM16 mod 10 turns
  uint32_t wfWanderPhase = 0;  // direction-meander clock, BAM16
  int32_t wfWanderAmp = 0;     // max heading deviation, BAM16
  int32_t wfWanderK = 0;       // meander spatial frequency, BAM16 per cell
  int32_t wfThermalQ = 0;      // isotropic thermal gust, Q16.16 cells/s
  uint32_t wfThermalPhase = 0; // thermal clock, BAM16
  int32_t wfCouplingQ = 65536; // surface coupling at the ground, Q16
  int32_t wfDecoupleH = 1;     // height the coupling returns to 1, voxels
  int32_t wfRidgeQ = 0;        // exposure gain, ridges (s > 0), Q16
  int32_t wfValleyQ = 0;       // exposure gain, hollows (s < 0), Q16
  int32_t wfExpDepth = 1;      // height the exposure term fades out by, voxels
  int32_t wfAbsGainQ = 0;      // absolute-altitude gain, Q16 per 1024 voxels
  int32_t wfLeeQ = 0;          // lee turbulence strength, Q16 (0 = off)
  int32_t wfLeeSlopeQ = 32768; // downwind slope where the lee starts, Q16
  int32_t wfLeeDepth = 1;      // lee layer depth, voxels
  int32_t wfLeeRevQ = 0;       // reverse-flow share at full lee, Q16
  int32_t wfLeeGustQ = 0;      // gust boost at full lee, Q16
  int32_t wfSlopeWindQ = 0;    // up(+)/down(-)slope wind, Q16.16 cells/s
  int32_t wfSlopeDepth = 1;    // slope-wind layer depth, voxels
  int32_t wfSeaBreezeQ = 0;    // onshore(+)/offshore(-) breeze, Q16.16 cells/s
  int32_t wfSeaDepth = 1;      // breeze layer depth, voxels
  int32_t wfSeaRadius = 1;     // water-fraction radius, voxels
  int32_t wfSeaLevelY = 0;     // absolute-altitude term's zero, world Y
  int32_t wfNeutralQ = 65536;  // profile outside the table's coverage, Q16
  int32_t wfTerrBase[2] = {0, 0};  // table's min CELL (world cell >> 5)
  uint32_t wfTerrOn = 0;       // 0 = no table yet: neutral profile everywhere
  int32_t wfThermalK = 0;      // thermal cell spatial frequency, BAM16/cell
  int32_t wfProf[kWindProfKnots] = {};  // profile at the knots, Q16
  uint32_t wfTerr[kWindTerrWords] = {}; // h i16 | exposure i8 | water u8

  // ---- WIND DRAFTS (kDraft* above; sim_draft.wgsl) ----
  // The shelter volume's minimum corner, WORLD voxels, chunk-aligned and
  // inside the window (support.cpp's box placement), and its gate. draftMode 0
  // = no draft row is recorded and windAtQ is the ambient field exactly; bit 0
  // = on; bit 1 = BURST (run the whole solve this tick: the box moved, or the
  // gate / materials / buffer is new -- sim_draft.wgsl `args`).
  int32_t draftOrigin[3] = {0, 0, 0};
  uint32_t draftMode = 0;

  // ---- HEAT UPDRAFTS + THE STACK EFFECT (wind phase 5; common.wgsl
  // windHeatQ, sim_draft.wgsl). Converted from the sim.windUpdraft* /
  // sim.windStackGain knobs in SubmitTick, so the kernels see integers.
  // updraftGainQ 0 = no heat term at all (windAtQ is the field it was before).
  int32_t updraftGainQ = 0;    // lift per heat unit of excess, Q16.16 cells/s
  int32_t updraftCapQ = 0;     // the lift's ceiling, Q16.16 cells/s
  int32_t updraftInflowQ = 0;  // the base inflow per unit of lift gradient, Q16
  int32_t draftStackQ = 0;     // the stack field's share, Q8 (256 = 1x)
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
// POURED, not thrown: the particle kernel skips the wind drag for it and
// nothing else (sim_particle.wgsl PFLAG_CALM, which must agree -- it is
// declared in that shader, its only reader). Set by game/container.h.
constexpr uint32_t kPFlagCalm = 16384u;
// A DRIP off a wet body (MobSystem::WetOneLimb): a micro droplet that lands
// and vanishes WITHOUT staining what it hit -- the world's wet stain never
// dries, so a dripping creature must not paint a trail. Declared in
// sim_particle.wgsl as PFLAG_DRIP, its only reader, which must agree.
constexpr uint32_t kPFlagDrip = 32768u;
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
  // ---- the sky's weather (src/sim/weather.h; spent from the old pad_dn0) ----
  // bit 0 (RWF_CLOUDS): the cloud buffers were written this frame and may be
  // read (cloud.wgsl). Clear when weather.clouds is off, and then no shader
  // reads a cloud binding at all. bit 1 (RWF_RAIN): precipitation reaches the
  // ground somewhere near the camera, so the streak overlay runs.
  uint32_t weatherFlags = 0;

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
  // How much of the dome is under cloud, 0..1 (weather::State::overcast). The
  // shared hemisphere ambient (ambientAtP, common.wgsl) greys and softens by
  // it, so every shading path — terrain, far field, bodies — agrees that the
  // sun went in. The DIRECT light is not scaled here: it is shadowed per
  // position by the cloud shadow map, which is what makes the shadows of
  // individual clouds cross the ground.
  float overcast = 0.0f;
  // How soaked exposed ground is, 0..1 (weather::State::wetness): darker
  // albedo and a sheen on sky-facing surfaces. Render-only; the world's own
  // wet STAIN is a different, sim-side thing (plan §2.5 tier 2).
  float wetness = 0.0f;

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
  // Lightning this frame, 0 = none, ~1 = a close stroke. Added to the shared
  // ambient as a cold white flash (ambientAtP) and to the clouds' own
  // emission at the stroke (cloud.wgsl). Spent from the old pad_dn3.
  float lightning = 0.0f;

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
  // ---- THE FRAME'S LIGHT (ResolveFrameLight, src/test/support.cpp) --------
  // Everything below is a pure function of the fields above plus TUNE_*
  // constants, evaluated ONCE per frame on the CPU. It used to be evaluated
  // per pixel, in two hand-kept copies: keyLightColor/keyLightDir/ambientAt in
  // raymarch.wgsl and keyLightColorP/keyLightDirP/ambientAtP in common.wgsl.
  // The copies drifted — 8124088 taught the common.wgsl ambient about the
  // overcast and lightning and the terrain's copy never heard, so a storm
  // greyed every body and left the ground it stood on in blue daylight. With
  // one author there is nothing to drift. common.wgsl's accessors read these.
  //
  // dayWeight: sunUp after the eclipse (the old dayWeight()/eclipseDayWeightP).
  // moonLit: (moon A + moon B contribution) / moonLightIntensity — the scaled
  //   sum ambient and the fog tints key their night level on.
  float dayWeight = 1.0f;
  float moonLit = 0.0f;
  // Unit key-light direction (sun by day, the brighter moon by night) and the
  // overcast blend the ambient applies (clamp(overcast) * 0.8; 0 = clear).
  float keyDir[3] = {0.0f, 1.0f, 0.0f};
  float ambOvercast = 0.0f;
  // Key-light colour x intensity, eclipse and moon selection included.
  float keyCol[3] = {0.0f, 0.0f, 0.0f};
  float pad_kl0 = 0.0f;
  // The hemisphere ambient BEFORE overcast and lightning, at n.y = -1 (ground)
  // and n.y = +1 (sky). It is linear in n.y (every term of it is a mix on
  // n.y * 0.5 + 0.5), so these two ends reproduce it exactly.
  float ambGround[3] = {0.0f, 0.0f, 0.0f};
  float pad_kl1 = 0.0f;
  float ambSky[3] = {0.0f, 0.0f, 0.0f};
  float pad_kl2 = 0.0f;

  // ---- GUST STREAKS (wind_streak.wgsl; render-only, never hashed) ---------
  // Live uniforms rather than TUNE_* so the F1 sliders move them without F5.
  // streakA: master alpha, spawn threshold (cells/s), spawn span (cells/s),
  //          radius (voxels).
  // streakB: lifetime (s), trail spacing (s), ribbon width (voxels), frame dt.
  // streakN: live pool size, trail points, frame counter, 0.
  float streakA[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  float streakB[4] = {1.0f, 0.05f, 0.3f, 0.0f};
  uint32_t streakN[4] = {0u, 2u, 0u, 0u};

  // ---- THE WIND FIELD BLOCK, the RENDER copy (TickParams' twin; must match
  // RenderParams in common.wgsl). Same members, same resolved values; the
  // three clocks are evaluated at the frame's sub-tick time so the grass
  // animates between ticks.
  uint32_t wfAdvPhase = 0;
  uint32_t wfWanderPhase = 0;
  int32_t wfWanderAmp = 0;
  int32_t wfWanderK = 0;
  int32_t wfThermalQ = 0;
  uint32_t wfThermalPhase = 0;
  int32_t wfCouplingQ = 65536;
  int32_t wfDecoupleH = 1;
  int32_t wfRidgeQ = 0;
  int32_t wfValleyQ = 0;
  int32_t wfExpDepth = 1;
  int32_t wfAbsGainQ = 0;
  int32_t wfLeeQ = 0;
  int32_t wfLeeSlopeQ = 32768;
  int32_t wfLeeDepth = 1;
  int32_t wfLeeRevQ = 0;
  int32_t wfLeeGustQ = 0;
  int32_t wfSlopeWindQ = 0;
  int32_t wfSlopeDepth = 1;
  int32_t wfSeaBreezeQ = 0;
  int32_t wfSeaDepth = 1;
  int32_t wfSeaRadius = 1;
  int32_t wfSeaLevelY = 0;
  int32_t wfNeutralQ = 65536;
  int32_t wfTerrBase[2] = {0, 0};
  uint32_t wfTerrOn = 0;
  int32_t wfThermalK = 0;
  int32_t wfProf[kWindProfKnots] = {};
  uint32_t wfTerr[kWindTerrWords] = {};
  // ---- WIND DRAFTS, the render copy: the origin of the LAST solved volume
  // and whether its field is valid (Simulation::DraftValid). 0 = the render
  // reads the ambient field, as the sim does at draftMode 0.
  int32_t draftOrigin[3] = {0, 0, 0};
  uint32_t draftMode = 0;
  // ---- CHUNK TICKETS, the render copy (docs/PLAN_chunk_tickets.md P4) -----
  // The LIVE ticket boxes (world chunks, lo corner; .w = the ticket index,
  // whose slot range the raymarch reads directly), compacted to the front;
  // `ticketCount` of them are valid. The raymarch tests these AABBs after a
  // ray leaves the window and runs a DDA inside a hit box, so a ticket draws
  // at full voxel resolution from any distance. 0 = one compare per sky ray.
  uint32_t ticketCount = 0;
  uint32_t pad_tk[3] = {0, 0, 0};
  int32_t ticketBox[kTicketMax][4] = {};
  // ---- HEAT UPDRAFTS, the render copy (common.wgsl windHeatF) -------------
  // The LAST tick's TickParams words of the same names (SubmitTick converts
  // the sim.wind* heat rows), so the F4 arrows and the gust streaks add the
  // heat term the sim felt, from the same integers. Gain 0 when the sim's
  // wind is off (sim.windMode 0): the sim feels no heat term then either.
  int32_t updraftGainQ = 0;
  int32_t updraftCapQ = 0;
  int32_t updraftInflowQ = 0;
  int32_t draftStackQ = 0;
};
static_assert(sizeof(RenderParams) % 16 == 0,
              "RenderParams must be a whole number of std140 rows");

// RenderParams.weatherFlags bits. Mirrored as RWF_* in common.wgsl.
constexpr uint32_t kRwfClouds = 1u;  // cloud buffers valid this frame
constexpr uint32_t kRwfRain = 2u;    // precipitation near the camera

// ---- THE CLOUDS (cloud.wgsl, src/sim/weather.h, DESIGN.md §9.w) ------------
// Render-only derived data, in the sense DESIGN.md §9 gives the far cascades:
// produced on the GPU every frame from the weather and the clock, read by the
// raymarcher and the raster bodies, never read by the sim, never hashed, never
// saved. Every size below is a SHIFT-free literal so check_invariants.py can
// scrape it, and each is mirrored in cloud.wgsl / common.wgsl (CLOUD_* consts)
// — the prelude deliberately does NOT carry them, because a prelude constant
// re-keys the SPIR-V cache of every shader in the engine (CLAUDE.md).
//
// kCloudShapeN   edge of the tileable SHAPE noise volume: Perlin-Worley in R,
//                Worley fBm at three frequencies in G/B/A, RGBA8 per texel.
//                Baked once on the GPU (cloud.wgsl `noise`); 8 MiB.
// kCloudDetailN  edge of the DETAIL (erosion) volume, Worley fBm x3; 128 KiB.
// kCloudWeatherN the per-frame WEATHER MAP around the camera: local coverage,
//                cloud type, rain, base jitter. 512^2 at weatherTexelM.
// kCloudShadowN  the per-frame CLOUD SHADOW map: transmittance to the key
//                light through the deck, indexed at the deck-base plane so
//                one texel serves every receiver along its light ray.
// kCloudEnvN     octahedral cloud ENVIRONMENT map: what the sky looks like
//                through the clouds in every direction, for fog, reflections
//                and anything else that integrates over a solid angle (the
//                sky-tier rule — gotcha-sky-tiers-fog-target).
constexpr uint32_t kCloudShapeN = 128;
constexpr uint32_t kCloudDetailN = 32;
constexpr uint32_t kCloudWeatherN = 512;
constexpr uint32_t kCloudShadowN = 256;
constexpr uint32_t kCloudEnvN = 64;
constexpr uint64_t kCloudNoiseWords =
    (uint64_t)kCloudShapeN * kCloudShapeN * kCloudShapeN +
    (uint64_t)kCloudDetailN * kCloudDetailN * kCloudDetailN;
// cloudMaps = [shadow: N^2 f32][env: E^2 x 2 words (pack2x16float rgb, T)]
//             [probe: 8 words — 0..3 the weather map AT THE CAMERA: coverage,
//              type, rain, jitter as f32, written by the weather pass so the
//              raymarcher's rain overlay knows whether it is raining HERE;
//              4..6 the wind averaged over a 20 m disc round the camera,
//              m/s f32, written by the env pass — what the rain streaks lean
//              along; 7 spare]
constexpr uint32_t kCloudProbeWords = 8;
// THE RAIN SHADOW MAP (assets/shaders/rain_map.wgsl, DESIGN.md 9.w): a
// kRainMapN^2 toroidal grid of fall lines round the eye, one voxel a texel,
// two words each (cover height, tag), behind a kRainMapHeaderWords header.
// Render-only. NOT in the shader prelude on purpose (a prelude line misses the
// SPIR-V cache for every shader): rain_map.wgsl and raymarch.wgsl declare
// RMAP_N / RMAP_HEADER themselves, and must agree with these.
constexpr uint32_t kRainMapN = 512;
constexpr uint32_t kRainMapHeaderWords = 16;
constexpr uint64_t kCloudMapsWords =
    (uint64_t)kCloudShadowN * kCloudShadowN +
    (uint64_t)kCloudEnvN * kCloudEnvN * 2 + kCloudProbeWords;
// Per low-res pixel: raw = 4 words (pack2x16float (r,g), (b,T), (Td,-) + depth
// f32), history = 3 words (the first three), ping-ponged. T is the total
// transmittance INCLUDING the aerial haze in front of the cloud; Td is the
// DIRECT transmittance without it. The composite needs both: haze is
// in-scattered airlight, so it may lighten a cloud toward the sky behind it
// but must never let the sun disc or a star shine through a thick one.
constexpr uint32_t kCloudRawWords = 4;
constexpr uint32_t kCloudHistWords = 3;

// CloudParams.flags bits. Mirrored as CLF_* in common.wgsl.
constexpr uint32_t kClfOn = 1u;         // march and composite this frame
constexpr uint32_t kClfHistValid = 2u;  // history may be reprojected
constexpr uint32_t kClfBake = 4u;       // the noise volume needs (re)baking
// The weather map is regenerated THIS frame (cloud.wgsl CLF_WEATHER, declared
// there, its only reader). Clear = the map from the last regen is reused, read
// through CloudParams.spare.yz, the metres the drift has moved since then —
// see WriteCloudParams (support.cpp) for when a regen is due.
constexpr uint32_t kClfWeather = 8u;

// The cloud pass's own uniform. Must match CloudParams in common.wgsl, field
// for field (check_invariants.py `params`). Written ONCE per rendered frame by
// WriteRenderParams, next to RenderParams, from the same resolved weather.
//
// Everything that GROWS without bound over a session — the wind advection of
// three noise fields — is folded on the CPU in DOUBLE and handed over already
// wrapped to 0..1 of its tile (the *Off fields). The shader never sees a
// "seconds since boot" times a speed, which is the f32 cancellation that turns
// drifting clouds into jittering ones after an hour.
struct CloudParams {
  float prevRight[3]; uint32_t flags;
  float prevUp[3];    uint32_t frame;
  float prevFwd[3];   float prevTanHalfFov;
  // cur eye - prev eye in METRES, differenced in double (taa.wgsl's reason).
  float eyeDeltaM[3]; float prevAspect;
  uint32_t lowW = 0, lowH = 0, fullW = 0, fullH = 0;
  // Word offsets of the two history halves: `histCur` is written this frame
  // and is what the composite reads; `histPrev` is last frame's.
  uint32_t histCur = 0, histPrev = 0, resDiv = 2;
  // The precipitation's lean (weather::Preset windShare / maxLeanDeg, blended):
  // share of the camera wind a drop takes. common.wgsl `rainWindShare`.
  float rainWindShare = 0.4f;
  // ---- the weather (weather::State::mix) ----
  float coverage = 0.0f, cloudType = 0.5f, density = 1.0f, precip = 0.0f;
  float baseM = 1400.0f, thicknessM = 1600.0f, darkness = 0.0f, cirrus = 0.0f;
  float cirrusAltM = 8000.0f, precipType = 0.0f, overcast = 0.0f, wetness = 0.0f;
  // Camera in absolute world METRES (voxel coords x kVoxelMeters).
  float camM[3]; float mist = 0.0f;
  // Wrapped advection offsets, in TILE units (0..1).
  float shapeOff[3]; float weatherEvolve = 0.0f;
  float detailOff[3]; float cirrusEvolve = 0.0f;
  // The weather map's texel 0 corner, absolute metres, snapped to its texel
  // grid so the map does not swim as the camera moves.
  float weatherOrigin[2]; float weatherTexelM = 125.0f; float shadowTexelM = 40.0f;
  // The shadow map's texel 0 corner at the deck-base plane, and that plane.
  float shadowOrigin[2]; float shadowPlaneM = 1400.0f;
  // tan of the lean cap (0.7 = today's rain, 1.5 = snow). `rainLeanTan`.
  float rainLeanTan = 0.7f;
  float weatherOff[2]; float windX = 0.0f, windZ = 1.0f;
  // Lightning stroke: absolute metres (x, altitude, z) and brightness.
  float flash[3]; float flashAmp = 0.0f;
  // This frame's sub-pixel jitter of the LOW-RES grid, in low-res pixels, and
  // the cirrus deck's own wrapped advection.
  float jitter[2]; float cirrusOff[2];
  float spare[4];
};
static_assert(sizeof(CloudParams) % 16 == 0,
              "CloudParams must be a whole number of std140 rows");
// Dilation applied to the fluid render AABB, world voxels. Two chunks: one for
// the snapshot's readback latency (>= 5 ticks of travel at the CFL ceiling),
// one for `fluidChunkActive`'s face-neighbour reach.
constexpr int kFluidRenderPadVox = 2 * (int)kChunk;

// ---- far-field cascades (render-only LOD — DESIGN.md §9,
// docs/PLAN_far_field_cascades.md) ----
// kFarLevels nested toroidal kFarN^3 (512^3) volumes around the residency window; level
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

// ---- THE FAR SURFACE MAP (LOD-seam package A, 2026-09-28; DESIGN.md §9) -----
// A per-level 2D heightfield at TWICE the level's XZ resolution: level k holds
// kFarMapN^2 entries, one per SUB-COLUMN of 2^(k-1+kFarShiftBase) fine voxels,
// so level 1's map is at FINE resolution (one entry per 10 cm column) and every
// coarser level carries 2x its cells' XZ detail. Toroidal exactly like the
// level (entry (mx, mz) lives at slot (mx, mz) mod kFarMapN) and blocked 2x2
// per level CELL, so the four sub-columns under one cell are one 16-byte load
// for the raymarcher: word = ((level-1) * kFarN^2 + (cz mod kFarN) * kFarN +
// (cx mod kFarN)) * 4 + (mz & 1) * 2 + (mx & 1), with (cx, cz) = (mx, mz) >> 1.
//
// One u32 per entry:
//   bits  0..15  top: the fine y of the column's topmost far-solid voxel
//                (max(ground, standing fluid)), biased by kFarMapHBias
//   bits 16..22  the SKIN's far palette slot (what the top voxel wears)
//   bits 23..29  the SUB-SKIN's slot (the voxel under it: a side face below
//                the top voxel wears this — grass over dirt, sand over stone)
//   bit  31      VALID: the column is a pure heightfield in the band the
//                renderer refines. Zero-initialised = invalid = "use the 3D
//                cells", so an unfilled or edited entry can only ever fall
//                back to the old rendering, never invent terrain.
//
// Produced by worldgen.wgsl `farmap` (one entry per fill entry that carries
// kFarListMapBit: see FarField::EnqueuePlane), invalidated — never raised —
// by `farpatch` (a refilled EDITED level chunk) and `fardown` (a live edit
// that removed matter the entry vouches for). Render-only derived data: never
// hashed, never saved, never read by the sim.
constexpr uint32_t kFarMapN = kFarN * 2;
constexpr uint64_t kFarMapWords = (uint64_t)kFarLevels * kFarMapN * kFarMapN;
constexpr int32_t kFarMapHBias = 32768;
constexpr uint32_t kFarMapValid = 0x80000000u;
// A farList entry that also (re)fills the surface map under its level chunk's
// XZ footprint. Bit 31, clear of the (level-1) << kFarSlotShift field below it.
constexpr uint32_t kFarListMapBit = 0x80000000u;
static_assert(((uint64_t)kFarLevels << kFarSlotShift) <= kFarListMapBit,
              "the far list's map-fill flag must sit above the level field");
constexpr uint32_t kFarMapWord(uint32_t level, int mx, int mz) {
  const uint32_t cx = (uint32_t)(mx >> 1) & (kFarN - 1);
  const uint32_t cz = (uint32_t)(mz >> 1) & (kFarN - 1);
  return ((level - 1) * kFarN * kFarN + cz * kFarN + cx) * 4 +
         (uint32_t)(mz & 1) * 2 + (uint32_t)(mx & 1);
}

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

// ---- ONE REACTION WITH EFFECTS THAT FIRED IN THE GRID ----------------------
// Decoded from the pageFaults record's [40..63] (kPageFaultReactFx*) by the
// snapshot parse. `fxId` names the effect list (materials.h FindRuleFx),
// `cell` is the WORLD cell of the reacting (self) voxel, `tick` the tick it
// fired on. docs/PLAN_alchemy_chemistry.md package A; consumed by
// game/session.cpp's reaction-effect pass.
struct ReactFxEvent {
  uint32_t fxId = 0;
  IVec3 cell{};
  uint32_t tick = 0;
};
// The slot key's cell half: the SLOT linear cell (world cell mod kWorldN,
// z-major, x fastest; 27 bits at kWorldN 512) times an odd constant mod 2^27. The
// multiply is a bijection, so the key is still unique per cell and decodes
// exactly, but a slot's atomicMax then picks a spatially SCATTERED winner
// instead of always the cell in the window's high corner. MIRRORED in
// sim_step.wgsl (RFX_SCRAMBLE), check_invariants `pairs`.
constexpr uint32_t kReactFxCellBits = 27;
constexpr uint32_t kReactFxCellMask = (1u << kReactFxCellBits) - 1u;
constexpr uint32_t kReactFxScramble = 0x0B5AD4EBu;  // odd
static_assert(kWorldN * kWorldN * kWorldN <= (1ull << kReactFxCellBits),
              "the reaction-effect key holds a window cell in 27 bits; a "
              "bigger window needs a wider key (world.h kPageFaultReactFx*)");
constexpr uint32_t ReactFxScrambleInverse() {
  uint32_t inv = kReactFxScramble;  // Newton: each step doubles the good bits
  for (int i = 0; i < 5; i++) inv *= 2u - kReactFxScramble * inv;
  return inv;
}
static_assert((kReactFxScramble * ReactFxScrambleInverse()) == 1u,
              "kReactFxScramble must be odd");
// The key holds the cell's SLOT coordinates (world coords mod kWorldN, so
// the slot choice and the winner do not depend on the window origin); the
// origin names the one window cell with those slot coordinates.
inline IVec3 ReactFxDecodeCell(uint32_t key, IVec3 originChunks) {
  const uint32_t lin = (key * ReactFxScrambleInverse()) & kReactFxCellMask;
  const int n = (int)kWorldN;
  const int w[3] = {(int)(lin % kWorldN), (int)((lin / kWorldN) % kWorldN),
                    (int)(lin / (kWorldN * kWorldN))};
  const int o[3] = {originChunks.x * (int)kChunk, originChunks.y * (int)kChunk,
                    originChunks.z * (int)kChunk};
  int c[3];
  for (int a = 0; a < 3; a++) c[a] = o[a] + ((((w[a] - o[a]) % n) + n) % n);
  return {c[0], c[1], c[2]};
}

// CPU-visible snapshot of GPU state, exactly World::kSnapshotLatency ticks
// latent by design (DESIGN.md §2) — a FIXED age, not a readback-timing one.
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
  // Async, K ticks latent, exactly like everything else on this ring. The
  // per-tick counters come from gasSpawn's header, which is cleared before the
  // CA runs, so each is "this tick" and not a running total. `gasCount` is the
  // live population; the rest are the attributions CLAUDE.md rule 6 asks for,
  // so "the plume is thin" is answered by a number that names WHICH bound bit.
  uint32_t gasCount = 0;         // live gas particles (post-resolve that tick)
  uint32_t gasLeaveAccepted = 0; // voxels converted at the window edge
  uint32_t gasLeaveRefused = 0;  // conversions refused: the chunk's share was spent
  uint32_t gasLeaveBudget = 0;   // the tick's leave budget (gasLeavePrep)
  uint32_t gasLeaveEdgeChunks = 0; // dirty chunks it was split across
  uint32_t gasLeaveOverrun = 0;  // accepted but the list was full: MUST be 0
  uint32_t gasEdgeHits = 0;      // gas voxels whose intent left the window
  uint32_t gasPoolRefused = 0;   // spawns dropped: the gas pool was full
  uint32_t gasReentered = 0;     // particles that became voxels again
  uint32_t gasDied = 0;          // decay / outer box / ceiling
  uint32_t gasAboveWindow = 0;   // live parcels above the window's top face
  // THE CA'S COST ATTRIBUTION (kGasSpCa*): what the colour rows ran this tick
  // by kind, and how many awake chunks are gas and nothing else (the share a
  // gas-only fast path or a smoke rate LOD could touch).
  uint32_t caGasCells = 0;       // gas cells run, both substeps
  uint32_t caOtherCells = 0;     // non-gas non-inert cells run, both substeps
  uint32_t gasOnlyChunks = 0;    // awake chunks holding gas and nothing else
  uint32_t gasOnlyCells = 0;     // gas cells in those chunks
  uint32_t emptyAwakeChunks = 0; // awake chunks holding no matter at all
  uint32_t tick = 0;                  // sim tick this snapshot was captured at
  // World::TicksEncoded() as of the tick that encoded this copy: one per tick,
  // gap-free, and immune to the harness restarting its tick numbers. The save
  // path proves "every tick since the window was filled had its dirty flags
  // folded" by walking these (Stream::FoldSnapshot, PLAN_save_system.md S1).
  uint32_t submitSeq = 0;
  // Per-chunk next-tick dirty (kNumChunks). Nonzero = dirty; the VALUE says
  // one more thing: World::kDirtyMutateOnly when the only reason bit set was
  // DIRTY_R_MUTATE (an op landed, the CA did nothing else there that tick),
  // 1 otherwise. Every consumer but Stream's modified fold only tests != 0;
  // the fold uses the distinction to keep the authored edit layer's own ops
  // from marking a chunk modified (sim/worldedit.h, map-overhaul P7).
  std::vector<uint8_t> dirtyFlags;
  // The raw DIRTY_R_* word of ONE slot, World::SetDirtyWatch's (0 unset). An
  // attribution instrument (CLAUDE.md rule 6): dirtyFlags keeps one bit of
  // the reason, and "why is THIS chunk awake" needs all of them.
  uint32_t watchReason = 0;
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
  // ---- the per-chunk digest (M9.3-A) ----
  // Per-slot content digest as of `chunkHashTick`, which is NOT necessarily
  // this snapshot's `tick`: only the FULL occupancy pass writes the table, and
  // that runs on hash ticks (plus load-reset and the hash-only pass). A
  // consumer that wants a digest it can compare with a peer must check
  // `chunkHashTick` — a table from tick 30 is a perfectly good description of
  // tick 30 and says nothing about tick 34.
  //
  // Null/empty until the first full pass has been read back. SHARED between
  // consecutive snapshots: the readback copies the 128 KiB table only on ticks
  // that can have rewritten it (World::EncodeReadbacks' hashTick), and every
  // other snapshot points at the table it inherited — immutable once parsed.
  std::shared_ptr<const std::vector<uint32_t>> chunkHash;
  uint32_t chunkHashTick = 0;
  // Page faults since process start — voxStore()'s sentinel no-op path
  // (PLAN_page_table.md §2.4). MONOTONIC and never cleared: a non-zero value is
  // a permanent "this build has a bug" latch. Every gate asserts it is zero,
  // which is what turns §2.4's structural claim into a measurement made on
  // every run rather than in a special configuration.
  uint32_t pageFaults = 0;
  // The scoop ledger (pageFaults record [36..38], see kPageFaultScoop*): what
  // the vessels' conditional clears really took, monotonic, as of `tick`.
  // Zeroed with the rest of the record by a page-table reset or a worldgen,
  // which a reader sees as the total going DOWN.
  uint32_t scoopEighths = 0;
  uint32_t scoopApplied = 0;
  uint32_t scoopRefused = 0;
  // ---- the solute layer (the kSolM* header of solMeta, as of `tick`) ----
  // Monotonic counters since the last solute reset; `solFaults` and
  // `solExhausted` are the two "this build has a bug" latches, the others are
  // the mass ledger the solute gates balance (dissolved - precipitated -
  // discarded - converted + poured - scooped == mass now).
  uint32_t solFree = 0;
  // ---- the temperature layer (heatMeta's header, heat.h kHm*, words 0..31) --
  // List counts, the free-stack depth, the monotonic firing / alloc / refusal
  // counters and the F1 probe -- as of `tick`. Zeroed by a worldgen / load.
  uint32_t heat[32] = {};
  // ---- the charge field (elecMeta's header, elec.h kEm*, words 0..31) ----
  // As of `tick`: the list counts, the free-stack depth, the monotonic
  // counters. Pages in use = elec[kEmNextFresh] - elec[kEmFreeTop], which is
  // what World::ElecMayBeLive reads. Zeroed by a worldgen / load.
  uint32_t elec[32] = {};
  // ---- THE BODY QUERY's answers (package E4; elec.h ElecHit) ----
  // The boxes asked on `tick` (World::QueueElecQuery), in the order they were
  // asked, each with its answer. Empty on a tick that asked nothing.
  std::vector<ElecHit> elecHits;
  uint32_t solFaults = 0;
  uint32_t solExhausted = 0;
  uint32_t solHighWater = 0;
  uint32_t solDissolved = 0;
  uint32_t solPrecip = 0;
  uint32_t solDiscarded = 0;
  uint32_t solConverted = 0;
  uint32_t solPoured = 0;
  uint32_t solScooped = 0;
  uint32_t solFaultSlot = 0;
  uint32_t solFaultTick = 0;
  // Monotonic units per species (1..kSolScoopSpecies) the vessels' clears took.
  uint32_t solScoopedBy[kSolScoopSpecies] = {};
  // ---- reaction effects (pageFaults [40..63], kPageFaultReactFx*) ----
  // THIS snapshot's tick only (the record is cleared every tick). `reactFx`
  // is the surviving slot winners in SLOT order -- a fixed order, so the
  // consumer's "first N" is deterministic; `reactFxFires` counts every firing
  // of a rule with effects, winners or not (the attribution for "why only N
  // blasts": N slots, M firings).
  std::vector<ReactFxEvent> reactFx;
  uint32_t reactFxFires = 0;
  // ---- the ticket record (pageFaults [64..127], kTicketReq* / kTicketDep*) --
  // THIS snapshot's tick only. `ticketReq`: the world chunks parked particles
  // asked a ticket for, decoded against `windowOrigin`, in BUCKET order (a
  // fixed order). `ticketDeposits`: the particles that handed themselves to
  // the CPU this tick (zero velocity; Tickets parks them as far landings).
  std::vector<IVec3> ticketReq;
  std::vector<ParticleSpawn> ticketDeposits;
  // ---- the on-demand chunk fetches this readback carried (World::Cached) ----
  // Parsed here and landed in the fetch cache by PublishSnapshotsUpTo, i.e. at
  // the same FIXED latency as everything else on this ring. They used to be
  // written into the cache straight from the map callback, which made what
  // ManageTerrain meshed for Jolt, what a door captured and what the mob
  // ground probe stood on a function of how fast the GPU came back (det-cpu,
  // 2026-10-03): finding L1 again, on the one store N1 did not move.
  std::vector<uint64_t> fetchKeys;    // PackChunkKey, request order
  std::vector<uint32_t> fetchWords;   // kChunkVol words per key
  uint32_t ticketFarKilled = 0;
  uint32_t ticketParked = 0;
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

// ---- THE GENERATION VERDICT (genAct's word) ------------------------------
//
// genChunk's `list` entry point publishes one u32 per genList position into
// `genAct`: the deferred wake's act bit (bit 0, the only thing the word held
// before) plus the chunk's PAGE-TABLE CLASSIFICATION, reduced on the GPU over
// the words it just wrote. That second half is what lets the batched worldgen
// (test/support.cpp SubmitWorldgen) and a window shift's FULL chunks
// (Stream::ApplyGenVerdict) demote without reading 16 KiB of words per chunk
// back to the CPU — the worldgen read was 16 x 32 MiB synchronous, 512 MiB per
// regen. The class is PageTable::Classify's answer to the same words, in the
// same order (EMPTY, then whole-word UNIFORM, then JITTER) — see
// PageTable::ClassifyGenVerdict for the one CPU decoder (an unpublished
// verdict falls back to Classify over the read-back words).
//
// EXACT MIRROR of the GEN_V_* consts in worldgen.wgsl (its only writer).
constexpr uint32_t kGenVerdictAct = 1u << 0;     // a cell can act (deferred wake)
constexpr uint32_t kGenVerdictValid = 1u << 1;   // the class below was published
constexpr uint32_t kGenVerdictClassShift = 2;    // 2 bits:
constexpr uint32_t kGenVerdictClassMask = 0x3u;
constexpr uint32_t kGenVerdictNeedsPage = 0;     //   mixed: no sentinel form
constexpr uint32_t kGenVerdictEmpty = 1;         //   PT_EMPTY
constexpr uint32_t kGenVerdictUniform = 2;       //   UNIFORM(mat)
constexpr uint32_t kGenVerdictJitter = 3;        //   JITTER(mat)
constexpr uint32_t kGenVerdictMatShift = 4;      // 12 bits of material
// Slots per batched-worldgen dispatch (SubmitWorldgen): the transient page
// demand of one batch, and so the pool headroom a regen needs. It is NOT a
// readback-ring quantity — nothing about it follows from kReadbackSlots.
constexpr uint32_t kWorldgenBatch = 2048;
// genAct holds one verdict per genList position, so it must cover the widest
// list that asks for one: a shift plane (kNChunk^2, the deferred wake) or a
// worldgen batch. A longer list (ReloadWindow's whole window) is bounds-checked
// in the kernel and simply publishes nothing past this.
constexpr uint32_t kGenActEntries =
    kWorldgenBatch > kNChunk * kNChunk ? kWorldgenBatch : kNChunk * kNChunk;

// Owns every GPU buffer of the simulation plus the async readback ring.
class World {
 public:
  void Init(const rhi::Device& device);

  // Records the per-tick readback copies into the encoder. Call after the sim
  // passes. Returns false if all ring slots are still in flight (skip copies).
  // particleLivePage: which particleCounts index holds the post-tick count.
  // tick: the sim tick being encoded (stamps the snapshot + fetched chunks).
  //
  // The two 100+ KiB tables that are usually unchanged are copied only when
  // they can have changed (the rest of the slot is ~0.8 MiB at rest):
  //   hashTick      — the tick recorded the FULL occupancy pass, the only
  //                   in-tick writer of the per-chunk digest table. A slot
  //                   without the copy SHARES the previous snapshot's table
  //                   (WorldSnapshot::chunkHash is a shared pointer), which is
  //                   what the GPU buffer still holds. Every out-of-tick
  //                   writer forces the next copy: InvalidateSnapshot
  //                   (worldgen, load reset, tick rewind) and
  //                   NoteChunkHashWritten (the hash-only pass).
  //   fluidRecorded — the MPM seam was recorded this tick, the only writer of
  //                   the swimming fold. With no seam there is no live fluid
  //                   (the seam's recording predicate covers every particle,
  //                   Simulation::EncodeTick), and the parse zero-fills.
  // Both default to true, which is the old copy-everything behaviour.
  bool EncodeReadbacks(const rhi::Device& device, const rhi::CommandEncoder& enc,
                       IVec3 playerChunkBase, uint32_t particleLivePage,
                       uint32_t tick, bool hashTick = true,
                       bool fluidRecorded = true);
  // Something outside a hash tick rewrote the digest table (the standalone
  // hash-only pass): make the next readback copy it rather than share.
  void NoteChunkHashWritten() { chunkHashForce_ = true; }

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
  // ---- THE TICKET TABLE, CPU HALF (docs/PLAN_chunk_tickets.md §2.2) ------
  //
  // The C++ twin of the table `Tickets` uploads into pageTable's tail
  // (kTicketTable*). ONE writer: Tickets (src/sim/tickets.cpp), through
  // SetTicketBox, at the between-ticks point its ops are applied. Everything
  // here is a pure function of the table, so the CPU and the GPU resolve the
  // same chunk to the same slot.
  //
  // Ticket i's slots are [kNumChunks + i*kTicketChunks, +kTicketChunks); a
  // box chunk wc sits at the slot its coordinate takes MODULO kTicketBoxN
  // (TicketLocalIndex), the window's toroidal rule at the box's size — which is
  // what lets P4 re-centre a box by refilling one plane of 25 slots.
  struct TicketBox {
    IVec3 lo{0, 0, 0};   // box min corner, world chunks
    bool live = false;
  };
  static uint32_t TicketLocalIndex(IVec3 wc) {
    const int n = (int)kTicketBoxN;
    const int x = ((wc.x % n) + n) % n, y = ((wc.y % n) + n) % n,
              z = ((wc.z % n) + n) % n;
    return (uint32_t)((z * n + y) * n + x);
  }
  static uint32_t TicketSlotBase(uint32_t i) { return kNumChunks + i * kTicketChunks; }
  // The ticket index a slot belongs to, or kTicketMax for a window slot or a
  // slot of the 64-rounding tail no ticket owns.
  static uint32_t TicketOfSlot(uint32_t slotIdx) {
    if (slotIdx < kNumChunks) return kTicketMax;
    const uint32_t i = (slotIdx - kNumChunks) / kTicketChunks;
    return i < kTicketMax ? i : kTicketMax;
  }
  const TicketBox& Ticket(uint32_t i) const { return tickets_[i]; }
  void SetTicketBox(uint32_t i, IVec3 lo, bool live) {
    tickets_[i].lo = lo;
    tickets_[i].live = live;
  }
  // Push tickets_ into pageTable's tail (one deferred write, ordered before
  // the next submit like every other between-ticks upload).
  void UploadTicketTable(const rhi::Queue& queue) const;
  bool TicketSlotLive(uint32_t slotIdx) const {
    const uint32_t i = TicketOfSlot(slotIdx);
    return i < kTicketMax && tickets_[i].live;
  }
  // The world chunk a ticket slot holds — the inverse of TicketSlotOfChunk.
  // A dead ticket's slots name the box they last held (harmless: nothing
  // dispatches, fills or evicts a dead slot).
  IVec3 TicketSlotWorldChunk(uint32_t slotIdx) const {
    const uint32_t i = TicketOfSlot(slotIdx);
    if (i >= kTicketMax) return {0, 0, 0};
    const uint32_t l = (slotIdx - kNumChunks) % kTicketChunks;
    const int n = (int)kTicketBoxN;
    const IVec3 m{(int)(l % kTicketBoxN), (int)((l / kTicketBoxN) % kTicketBoxN),
                  (int)(l / (kTicketBoxN * kTicketBoxN))};
    const IVec3 lo = tickets_[i].lo;
    auto wrap = [n](int mm, int l0) { return l0 + ((((mm - l0) % n) + n) % n); };
    return {wrap(m.x, lo.x), wrap(m.y, lo.y), wrap(m.z, lo.z)};
  }
  // The live ticket slot holding world chunk wc, or kTicketSlotNone.
  static constexpr uint32_t kTicketSlotNone = 0xFFFFFFFFu;
  uint32_t TicketSlotOfChunk(IVec3 wc) const {
    for (uint32_t i = 0; i < kTicketMax; i++) {
      const TicketBox& t = tickets_[i];
      if (!t.live) continue;
      const int n = (int)kTicketBoxN;
      if (wc.x < t.lo.x || wc.y < t.lo.y || wc.z < t.lo.z || wc.x >= t.lo.x + n ||
          wc.y >= t.lo.y + n || wc.z >= t.lo.z + n)
        continue;
      return TicketSlotBase(i) + TicketLocalIndex(wc);
    }
    return kTicketSlotNone;
  }
  // Is this slot in its ticket's ACTIVE interior (the inner 3^3 of the 5^3),
  // i.e. dispatched by the CA? The shell is resident only (§2.3).
  bool TicketSlotActive(uint32_t slotIdx) const {
    if (!TicketSlotLive(slotIdx)) return false;
    const IVec3 wc = TicketSlotWorldChunk(slotIdx);
    const IVec3 lo = tickets_[TicketOfSlot(slotIdx)].lo;
    const int hi = (int)kTicketBoxN - 1;
    const IVec3 d{wc.x - lo.x, wc.y - lo.y, wc.z - lo.z};
    return d.x >= 1 && d.y >= 1 && d.z >= 1 && d.x < hi && d.y < hi && d.z < hi;
  }
  // THE RESIDENT-SLOT RESOLVER, the C++ twin of common.wgsl's chunkSlotOf:
  // the window slot, else a live ticket slot, else kTicketSlotNone.
  uint32_t ResidentSlotOfChunk(IVec3 wc) const {
    if (ChunkInWindow(wc)) return SlotChunkIndex(wc);
    return TicketSlotOfChunk(wc);
  }
  bool ChunkResident(IVec3 wc) const { return ResidentSlotOfChunk(wc) != kTicketSlotNone; }
  // The CellOp index of a RESIDENT cell (slot * kChunkVol + chunk-local), or
  // kTicketSlotNone. SlotCellIndex's ticket-aware form, for the producers that
  // must be able to target a ticket (the selftest's pours; the far landings).
  uint32_t ResidentCellIndex(IVec3 c) const {
    const uint32_t s = ResidentSlotOfChunk({c.x >> 4, c.y >> 4, c.z >> 4});
    if (s == kTicketSlotNone) return kTicketSlotNone;
    const uint32_t lx = (uint32_t)(c.x & 15), ly = (uint32_t)(c.y & 15),
                   lz = (uint32_t)(c.z & 15);
    return s * kChunkVol + (lz * kChunk + ly) * kChunk + lx;
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

  // ---- K: THE FIXED SNAPSHOT LATENCY (docs/PLAN_multiplayer_now.md N1) ----
  //
  // `World::Snap()` at tick T is the snapshot of tick T - kSnapshotLatency,
  // EXACTLY, on every machine and at every frame rate. Not "one tick latent,
  // older when the ring is saturated" — that was a readback-timing property,
  // and every gameplay decision that reads the snapshot (Brush::BuildOp, the
  // mob ground probe, the spell ladder, island EventReady, the wind wake)
  // inherited it. A decision whose input depends on how fast the GPU came
  // back is not reproducible across machines, which is finding L1 of
  // docs/RESEARCH_multiplayer_readiness.md; L1' is that the harness hid it,
  // because SetHarnessSnapshotDrain blocked for the map every tick and so
  // tested a 1-tick latency the game never ran at.
  //
  // IT IS THE SAME NUMBER AND THE SAME ARGUMENT as Stream::kWakeLatency
  // (sim/stream.h): K constant => the outcome is a pure function of
  // (inputs, tick), so the twice-run determinism comparison is untouched and
  // the value of K only moves the world hash once, when it is chosen.
  //
  // THE CEILING is SubmitTick's kPagedSnapshotMaxGap (test/support.cpp): the
  // page-table mirror tightens against a snapshot that is K ticks old and
  // dilates one N26 ring per tick of that gap, so K is an exponent on the
  // materialization set and not a latency knob. K must stay <= that gap; both
  // are 4 today.
  static constexpr uint32_t kSnapshotLatency = 4;
  // WorldSnapshot::dirtyFlags value for "dirty, and DIRTY_R_MUTATE (common.wgsl,
  // 256) was the only reason". The mirror of that WGSL constant is here and
  // only here; the `worldedit` gate fails if a MUTATE-only chunk is not
  // reported with it.
  static constexpr uint32_t kDirtyReasonMutate = 256u;
  static constexpr uint8_t kDirtyMutateOnly = 2;

  // The fixed-latency pipeline, counted. `waits` is the kill criterion of N1:
  // a tick that had to BLOCK for the snapshot it is contractually owed. Never
  // a skip, never a stale read — the wait is the price of the constant.
  struct SnapshotPipeStats {
    uint64_t ticks = 0;        // ticks that ran the publish
    uint64_t published = 0;    // ticks whose target snapshot was published
    uint64_t missing = 0;      // ticks with NO snapshot at the target (resets)
    uint64_t waits = 0;        // blocking waits (fences) the publish paid
    uint64_t slotWaits = 0;    // blocking waits to free a ring slot to encode
    uint64_t declines = 0;     // EncodeReadbacks refusals; must stay 0
    uint64_t dropped = 0;      // parsed snapshots discarded by a reset/rewind
  };

  // THE PUBLISHED SNAPSHOT. At tick T this is the snapshot of tick
  // T - kSnapshotLatency, exactly (see that constant). It is NOT "whatever the
  // last MapAsync callback happened to deliver": callbacks now parse into a
  // holding queue and SubmitTick publishes exactly one of them per tick, so a
  // fast GPU cannot run the CPU's world view ahead and a slow one cannot let
  // it fall behind. `valid` is false only for the first kSnapshotLatency ticks
  // after a world reset, which is itself a constant.
  const WorldSnapshot& Snap() const { return snap_; }

  // ---- REACTION EFFECTS, EXACTLY ONCE (docs/PLAN_alchemy_chemistry.md A) --
  // The reaction-effect winners of every snapshot published since the last
  // call, in tick order. On the fixed-latency path (published, never
  // LatestDelivered), so what a tick drains is a pure function of the tick:
  // at tick T it is the firings of tick T - kSnapshotLatency - 1 (the publish
  // runs inside SubmitTick, after the tick body that drains). The one
  // consumer is game/session.cpp's reaction-effect pass.
  static constexpr size_t kReactFxPendingMax = 256;
  std::vector<ReactFxEvent> TakeReactFx() {
    std::vector<ReactFxEvent> out;
    out.swap(reactFxPending_);
    return out;
  }
  // ---- FAR-LANDING DEPOSITS, EXACTLY ONCE (chunk tickets P2) ---------------
  // Every published snapshot's deposits, in tick order, until Tickets takes
  // them. Same discipline as TakeReactFx. Capped at kTicketDepositsPendingMax
  // (oldest dropped and COUNTED — a deposit is matter, so a drop is a loss and
  // must be a number) so a harness that never runs the ticket step cannot grow
  // it; cleared by InvalidateSnapshot.
  static constexpr size_t kTicketDepositsPendingMax = 4096;
  // ---- THE BODY QUERY (package E4: shocks reach bodies; elec.h) -----------
  //
  // MobSystem asks, once a tick, about one box per live limb of the bodies
  // near the window (QueueElecQuery, bounded at kElecQueryMax and every
  // refusal counted); SubmitTick takes the list (TakeElecQueries), uploads it
  // (Simulation::PrepareElecQueries) and hands it to the readback slot of the
  // same tick (SetElecQueriesInFlight), where it rides as the TAG of the
  // answers the GPU writes. The publish hands the answers over EXACTLY ONCE,
  // in tick order (TakeElecHits, TakeReactFx's discipline): what a tick
  // drains is the boxes of tick T - kSnapshotLatency - 1, never "whatever
  // landed", so the shocks a creature takes are a pure function of the tick.
  void QueueElecQuery(const ElecQuery& q) {
    if (elecQueryQueue_.size() >= kElecQueryMax) {
      elecQueryRefused_++;
      return;
    }
    elecQueryQueue_.push_back(q);
    elecQueriesAsked_++;
  }
  void TakeElecQueries(std::vector<ElecQuery>& out) {
    out.clear();
    out.swap(elecQueryQueue_);
  }
  void SetElecQueriesInFlight(std::vector<ElecQuery>&& q) { elecInFlight_ = std::move(q); }
  std::vector<ElecHit> TakeElecHits() {
    std::vector<ElecHit> out;
    out.swap(elecHitsPending_);
    return out;
  }
  static constexpr size_t kElecHitsPendingMax = 4 * kElecQueryMax;
  // CAN THE FIELD HOLD CHARGE AT `tick`? The body query is recorded only
  // when it can (rule 2: a world with no charge pays nothing). Two inputs,
  // both pure functions of the tick stream: a SOURCE op (a material with
  // electric.source, SubmitTick's NoteElecSourceOps) in the last
  // kSnapshotLatency + 2 ticks, which covers the latency before the field it
  // lit is visible -- or pages in use in the published snapshot.
  bool ElecMayBeLive(uint32_t tick) const {
    if (elecSourceSeen_ && tick <= elecSourceTick_ + kSnapshotLatency + 2u) return true;
    return snap_.valid && snap_.elec[kEmNextFresh] > snap_.elec[kEmFreeTop];
  }
  // The material table's sources (Simulation::UploadTables) and the latch.
  void SetElecSourceMats(std::vector<uint8_t> m) { elecSourceMat_ = std::move(m); }
  bool IsElecSourceMat(uint32_t mat) const {
    return mat < elecSourceMat_.size() && elecSourceMat_[mat] != 0;
  }
  void NoteElecSourceOp(uint32_t tick) {
    elecSourceSeen_ = true;
    elecSourceTick_ = tick;
  }
  struct ElecQueryStats {
    uint64_t asked = 0, refused = 0, hits = 0, charged = 0;
  };
  ElecQueryStats ElecQueryCounters() const {
    return {elecQueriesAsked_, elecQueryRefused_, elecHitsDelivered_, elecHitsCharged_};
  }
  std::vector<ParticleSpawn> TakeTicketDeposits() {
    std::vector<ParticleSpawn> out;
    out.swap(ticketDepositsPending_);
    return out;
  }
  uint64_t TicketDepositsDropped() const { return ticketDepositsDropped_; }

  // ---- the per-chunk digest and the quiet counter (M9.3-A) ---------------
  //
  // The digest of slot `slot` as of ChunkHashTick(), or 0 before any full
  // occupancy pass has been read back. Content-keyed (chunk-local cell index),
  // so the same 4,096 words digest the same on any machine, in any slot, under
  // any window origin, and whether the chunk is a real page or a sentinel.
  uint32_t ChunkHashOfSlot(uint32_t slot) const {
    const WorldSnapshot& s = snap_;
    return (s.valid && s.chunkHash && slot < s.chunkHash->size())
               ? (*s.chunkHash)[slot]
               : 0u;
  }
  // The tick the digest table above describes. Zero means "no full pass has
  // been read back yet". Always <= Snap().tick.
  uint32_t ChunkHashTick() const { return snap_.valid ? snap_.chunkHashTick : 0u; }

  // Ticks this slot has been quiet for, capped at 65535 — "no voxel of this
  // chunk was dirty for the last N published ticks". It is the only cheap
  // answer to "has this chunk settled enough that its digest is worth
  // comparing with a peer's", which is what M9.3-C's resync schedule needs:
  // hashing a chunk the CA is still churning produces a mismatch that means
  // nothing.
  //
  // DERIVED, NOT HASHED, NOT SAVED. It is folded out of the snapshot's dirty
  // flags (so it is kSnapshotLatency ticks latent, like everything else on
  // that ring), zeroed by Stream::MarkModifiedBox when a CPU edit touches the
  // chunk, and zeroed when a refill puts different world content in the slot.
  // Nothing that feeds the sim may read it: it counts PUBLISHED ticks, and a
  // run that published a different number of them would still be the same
  // world.
  uint32_t QuietTicks(uint32_t slot) const {
    return slot < quietTicks_.size() ? quietTicks_[slot] : 0u;
  }
  // "A CPU edit / a refill just changed this chunk" — reset the streak. The
  // world-chunk form is what Stream::MarkModifiedBox calls; the slot form is
  // what Stream::FillSlots calls, because a refilled slot's world chunk is the
  // NEW one and the streak belonged to the old.
  void NoteChunkTouched(IVec3 wc) {
    if (ChunkInWindow(wc)) NoteSlotTouched(SlotChunkIndex(wc));
  }
  void NoteSlotTouched(uint32_t slot) {
    if (slot < quietTicks_.size()) quietTicks_[slot] = 0;
  }
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
  //
  // Also drops every parsed-but-unpublished snapshot and every readback still
  // in flight (they describe the dead world too), by bumping the pipeline
  // epoch: a callback that fires for an older epoch frees its slot and throws
  // the bytes away.
  void InvalidateSnapshot();

  // ---- the fixed-latency pipeline (SubmitTick drives all three) -----------
  //
  // True when EncodeReadbacks would find a free ring slot. THE RING MAY NEVER
  // DECLINE on the sim path: a tick with no copy encoded has no snapshot to
  // publish K ticks later, and the latency would stop being a constant. The
  // caller waits (WaitOldestPendingMap) until this is true instead.
  bool ReadbackSlotFree() const;
  // True when some slot is still in flight for a tick at or before `tick` —
  // i.e. a snapshot the publish at `tick + kSnapshotLatency` is owed has not
  // landed yet. The caller blocks on it.
  bool ReadbackPendingAtOrBefore(uint32_t tick) const;
  // Publish, in tick order, every delivered snapshot whose tick is <= target;
  // `snap_` ends holding the NEWEST of them. Anything newer stays queued for
  // the ticks that own it. Returns true if `snap_` describes exactly `target`.
  bool PublishSnapshotsUpTo(uint32_t target);
  // Lands one snapshot's carried chunk fetches in the fetch cache (version
  // guard + the cache's size bound), and empties its fetch lists. Called by
  // the publish for every snapshot it walks, so Cached() at tick T holds
  // exactly the fetches of ticks <= T - kSnapshotLatency.
  void LandFetches(WorldSnapshot& s);

  // ---- THE FRESHEST DELIVERED SNAPSHOT: DERIVED DATA ONLY ----------------
  //
  // `Snap()` is the fixed-latency GAMEPLAY view and must stay exactly that.
  // The PAGE TABLE is a different customer with the opposite requirement, and
  // conflating the two is what N1's first cut got wrong:
  //
  //   * the page table is DERIVED data (PLAN_page_table.md: not hashed, not
  //     saved, reconstructible), so reading a timing-dependent snapshot there
  //     cannot make the world non-reproducible;
  //   * and it NEEDS freshness for cost reasons that turn into correctness
  //     ones. TightenFromSnapshot dilates the mirror one N26 ring per tick of
  //     gap, so at gap K the materialization set is the wrong SHAPE as well as
  //     ~2.3^K bigger, and chunks the GPU is about to write stop being
  //     materialized. A page fault IS that: a store dropped into a chunk that
  //     was left a sentinel. Measured on the first cut of N1, which routed the
  //     page table through the fixed view and removed the settle-window drain:
  //     `daylight-boundary` pageFaults 1 and `ca-level-pond` a 2-eighth mass
  //     leak, from a suite that had neither.
  //
  // Publishing and DELIVERY are separate now, which is what lets ONE ring
  // serve both: this returns the newest snapshot the GPU has handed back,
  // whatever its age, and SubmitTick may drain toward freshness for it without
  // ever letting Snap() run ahead of T - kSnapshotLatency.
  //
  // NOTHING THAT FEEDS THE SIM MAY READ THIS. It is timing-dependent by
  // construction; that is the entire reason Snap() exists.
  const WorldSnapshot& LatestDelivered() const {
    return ready_.empty() ? snap_ : ready_.back();
  }

  // ---- THE SAVE PATH'S VIEW OF THE PIPELINE (PLAN_save_system.md S1) ------
  //
  // A save stores only the chunks that differ from what genChunk would make,
  // and "differs" is the union of every tick's dirty flags since the window
  // was filled. Snap() is K ticks behind, so the save also needs the K
  // snapshots that are delivered but not yet published, and a way to prove
  // none is missing. Read-only, derived consumers only — the same rule as
  // LatestDelivered: NOTHING THAT FEEDS THE SIM MAY READ THESE.
  //
  // TicksEncoded: one per EncodeReadbacks call, i.e. one per sim tick
  // (the readback is unconditional since N1), counted BEFORE the ring can
  // decline — so a tick with no snapshot is a visible hole in submitSeq.
  uint32_t TicksEncoded() const { return ticksEncoded_; }
  // Which slot WorldSnapshot::watchReason reports (UINT32_MAX = none).
  void SetDirtyWatch(uint32_t slot) { dirtyWatch_ = slot; }
  uint32_t SnapshotEpoch() const { return snapEpoch_; }
  const std::deque<WorldSnapshot>& DeliveredUnpublished() const { return ready_; }
  const SnapshotPipeStats& SnapshotPipe() const { return snapPipe_; }
  SnapshotPipeStats TakeSnapshotPipe() {
    const SnapshotPipeStats s = snapPipe_;
    snapPipe_ = SnapshotPipeStats{};
    return s;
  }
  void NoteSnapshotWait() { snapPipe_.waits++; }
  void NoteSnapshotSlotWait() { snapPipe_.slotWaits++; }

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
  // TEST ONLY (kGasOpsLeaveCap): cap the tick's window-edge leave budget so a
  // gate can force the overflow the budget decides. 0 = no cap. Rides the gas
  // spawn header SubmitTick uploads every tick.
  void SetGasLeaveCapForTest(uint32_t n) { gasLeaveCapTest_ = n; }
  uint32_t GasLeaveCapForTest() const { return gasLeaveCapTest_; }

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

  // THE WORD -> COLLISION CLASS MAPPING, in ONE place.
  //
  // KindAt and KindAtCached read two different stores (the 3x3x3 snapshot
  // mirror and the on-demand chunk cache) and must answer identically for the
  // same voxel word, or a second player's sweeps disagree with the first's
  // about the same cell. Copying four switch arms was how they would stop
  // agreeing, so neither owns them.
  // HOW TALL the mirror's cell is, as a fraction of a voxel: a partial powder
  // cell (POWDER MASS) is its grains, mass/8; everything else is 1. Only
  // meaningful for a cell KindAt calls Solid. The player's slab collision
  // (Player::TopFn) reads it.
  float CellTopAt(IVec3 cell, const std::vector<uint32_t>& classOf) const;

  static CellKind KindOfWord(uint32_t word,
                             const std::vector<uint32_t>& classOf) {
    const uint32_t mat = word & 0xFFF;
    if (mat == 0) return CellKind::Air;
    if (mat >= classOf.size()) return CellKind::Air;
    switch (classOf[mat]) {
      case 0: return CellKind::Solid;
      case 1:
        // Powder carries weight -- except a thin FILM of grains (under
        // kPowderWalkMin eighths), which is walked and flown through.
        return PowderMassOfState((word >> 12) & 0xFu) < kPowderWalkMin
                   ? CellKind::Air : CellKind::Solid;
      case 2: return CellKind::Liquid;
      default: return CellKind::Gas;
    }
  }

  // ---- A COLLISION SOURCE FOR A BODY OUTSIDE THE MIRROR (M9.1 P3) ---------
  //
  // KindAt can only answer for the 3x3x3 chunk cube the snapshot mirrors
  // around ONE player's chunk, and `kWorldN = 512` at `kVoxelMeters = 0.10` is
  // a 51.2 m residency window: a second player walks out of the first's mirror
  // in seconds. Every cell then reads Unknown, and Unknown is PASSABLE for the
  // player controller (player.cpp `Collides` treats it as air on purpose — an
  // unfetched cell must not become an invisible wall). What stops the body
  // falling through is `KnownDrop`, which clamps the descent to the last row
  // the mirror vouches for — so a mirror-less player does not fall through the
  // world, it HOVERS, indefinitely, wherever it left the cube. (The engine map
  // said "Unknown mirror cells are SOLID for the player". They are not; the
  // clamp is upstream of the sweep, not inside it.)
  //
  // This is the answer for every body the mirror is not centred on. In order:
  //
  //   * outside the residency window -> Solid, the same conservative answer
  //     KindAt gives and the same rule the sim uses;
  //   * a chunk in the on-demand fetch cache (World::Cached, the store
  //     MobSystem's ground probe already walks) -> the REAL voxel, through
  //     KindOfWord above;
  //   * a cache miss -> queue the chunk (FetchSource::Mob; the request is
  //     coalesced if it is already queued) and answer from the ANALYTIC
  //     column, `TerrainHeight(x, z, WorldSeed())`: Solid at or below the
  //     ground height, Air above it.
  //
  // WHY THE ANALYTIC FALLBACK AND NOT Unknown. Unknown would re-create the
  // hover: the fetch lands 1-2 ticks plus kSnapshotLatency later, and a walking
  // body crosses a chunk boundary far more often than that. TerrainHeight is
  // worldgen's own height contract, so the fallback is the ground the world was
  // BUILT with — wrong only where the world has since been changed (a dug
  // tunnel, a collapsed cliff) or where worldgen put something the column does
  // not describe (a cave roof, an overhang, a ruin floor). Those are exactly
  // the cells the fetch then corrects, one to two ticks later.
  //
  // FLUID IS NOT ANSWERED HERE, deliberately. The game's `kindAt` folds
  // `FluidEighthsAt(c) >= 2 -> Liquid` in front of KindAt (main.cpp), and
  // FluidEighthsAt is MIRROR-BOUNDED: it returns 0 for any cell outside the
  // same 3x3x3 cube, so it already answers "no excited fluid" for a remote
  // player whether or not this function exists. The CA's own liquid voxels DO
  // come through, because those are ordinary words in the fetched chunk. What
  // a remote player cannot see for M9.1 is MLS-MPM fluid in flight, which is
  // render- and splash-facing, not footing. M9.2's ghost players carry their
  // own machine's answer across the wire, which is where this stops mattering.
  //
  // NOT const: a cache miss enqueues a fetch. ~25 hash3 per missed cell
  // (TerrainHeight's note), which is why the miss path must stay rare — see
  // PlayerSession::PrefetchAround, the one call site that keeps it that way.
  CellKind KindAtCached(IVec3 cell, const std::vector<uint32_t>& classOf);

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
  // The water-site index of the site whose map.json id is `id`, or -1 when
  // the loaded map has no such water site (e.g. the harness lake, asked of the
  // game's map).
  static int WaterSiteIndex(const std::string& id);
  // The mirrored bowl depth at squared distance d2 from the centre of a disc
  // of radius r wearing `preset`: the basin curve inverts THIS by bisection
  // rather than re-deriving the profile (worldmap.h explains the knots).
  static int BowlDepth(uint32_t preset, int r, int d2);
  // The widest disc + band any pond can reach, voxels: a probe walking out
  // of a tarn to its berm core stops after this many columns at most.
  static int PondReachMax();

  // The map's calm pad box (worldmap.h): the CPU twin of worldgen.wgsl's
  // inPadBox, for fixtures that want to know they are on it.
  static bool InPadBox(int x, int z);

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
  // ---- the solute layer (the kSol* block above) ----
  // AUTHORITATIVE STATE, unlike the page table: the cell values are hashed
  // (sim_solute.wgsl solHash) and saved (worldio 'SOLU'). The table and the
  // page indices are GPU-owned; the CPU writes them only at a reset or a
  // restore, when no tick is in flight.
  rhi::Buffer solTable;    // kNumSlots u32 entries
  rhi::Buffer solPool;     // kSolutePoolPages * kSolWordsPerPage u32
  rhi::Buffer solMeta;     // kSolMetaWords u32
  rhi::Buffer solArgs;     // 3 u32 — indirect-only copy of solMeta[kSolMArgs..]
  rhi::Buffer solStage;    // kSolStageWords u32: eviction / restore records
  // Zero the whole layer: every slot EMPTY, every page free, every counter 0.
  // Deferred queue writes, so they land at the head of the next submit -- the
  // same ordering every other reset here relies on.
  void ResetSolutes(const rhi::Queue& queue);
  // THE TEMPERATURE LAYER (src/sim/heat.h). GPU-owned; all zero = an empty
  // layer, which is what the worldgen and load-reset fill rows leave. Not
  // hashed, not saved: rebuilt from the emitters standing in the world.
  rhi::Buffer heatPool;    // kHeatPoolPages * kHeatPageWords u32
  rhi::Buffer heatMeta;    // kHmWords u32
  rhi::Buffer heatArgs;    // kHeatArgsBytes -- indirect-only copy of heatMeta[kHmArgs..]
  // THE CHARGE FIELD (src/sim/elec.h). GPU-owned, transient; all zero = no
  // charge anywhere, which is what the worldgen and load-reset fill rows
  // leave. Not hashed, not saved.
  rhi::Buffer elecPool;    // kElecPoolPages * kElecPageWords u32
  rhi::Buffer elecMeta;    // kEmWords u32
  rhi::Buffer elecArgs;    // kElecArgsBytes -- indirect-only copy of elecMeta[kEmArgs..]
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
  // ---- the per-chunk digest (docs/PLAN_multiplayer_m9.md M9.3-A) ----
  // kNumSlots digests + ONE trailing word holding the `tick` of the pass that
  // wrote them. Written only by sim_occupancy.wgsl's FULL entry point (hash
  // ticks, load-reset, the standalone hash-only pass); the dirty entry point
  // leaves it alone, so between hash ticks the table describes the last full
  // pass and the trailing word says which tick that was.
  //
  // DERIVED DATA, exactly like the page table: never hashed into the world
  // hash, never saved, reconstructible by one full pass. It exists so two
  // machines can ask "is your copy of world chunk C the same as mine?" without
  // shipping 16 KiB, which is M9.3-C's resync.
  rhi::Buffer chunkHash;   // kChunkHashWords u32
  rhi::Buffer tickUBO;     // TickParams
  rhi::Buffer passUBO;     // 27 slices * 256 B (3x3x3 color phases)
  rhi::Buffer opsBuf;      // kMaxOpsPerTick BrushOp
  rhi::Buffer renderUBO;   // RenderParams
  rhi::Buffer cloudUBO;    // CloudParams (render-only; WriteRenderParams)
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
  rhi::Buffer claim;           // kClaimWords u32 — reinsertion claim hash + grain landing
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
  // Per-plume carried lateral offsets (see kGasPlumeTrackSlots). PERSISTENT
  // across ticks, unlike the two boxes: never cleared after the creation fill.
  rhi::Buffer gasPlumeTrack;    // kGasPlumeTrackWords u32

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
  rhi::Buffer farVox;   // kFarLevels x kFarN^3 material bytes, packed 4/u32
                         // (atomic in the fill/downsample kernels: partial-word
                         // byte updates from neighboring dirty chunks race)
  rhi::Buffer farOcc;   // kFarLevels x kNumChunks u32 non-air counts
  rhi::Buffer farList;  // kFarListCap entries: (level-1)<<kFarSlotShift | slot
  rhi::Buffer farUBO;   // FarParams
  rhi::Buffer farPatch; // per-fill edit patches (kFarPatch* above)
  // One u32 per slot: a signature of the chunk's far-visible matter (every
  // cell farCellIsSolid keeps, keyed by position and material, mixed with the
  // world chunk coord and the level origins) as of its last `fardown`. A dirty
  // chunk whose signature has not moved is skipped: nothing the cascade can
  // show has changed. That is most of a burning forest, whose ~5,000 awake
  // chunks are awake for SMOKE, which the cascade never holds (2026-09-22:
  // 15.4 ms/frame of fardown). Derived, zero-initialised; FarField zeroes it
  // on every reset / full refill so a re-filled level is downsampled afresh.
  rhi::Buffer farSig;
  // The far SURFACE MAP (kFarMap* above): per-level fine-resolution column
  // heights + skin, the raymarcher's refine data. CopySrc for the far-surface
  // gate only.
  rhi::Buffer farMap;

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

  // The chunk-ticket lifecycle (src/sim/tickets.h), owned by Stream beside the
  // two indexes above. Forward-declared for their reason. Null before
  // Stream::Init; SubmitWorldgen uses it to drop every ticket before a world
  // is regenerated (a ticket of the old world must not survive into the new).
  class Tickets* tickets = nullptr;

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
  //   + 108 KiB fluid mirror + 128 KiB per-chunk digest + ~2 KiB small tables
  //   + kFetchPerTick x 16 KiB chunk fetches (1 MiB, the biggest single term)
  // ~= 2.03 MiB. 16 slots is ~32.5 MiB, up from 5.7 MiB at three. The slot is
  // SIZED for all of it; what is COPIED per tick is less — sentinel mirror
  // chunks and fetches are skipped, the digest rides hash ticks only and the
  // fluid fold only ticks that recorded the seam (EncodeReadbacks).
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

  // K, the fixed snapshot latency, and the pipeline's counters live up with
  // Snap() -- a nested type has to be declared before the member functions
  // that name it. See World::kSnapshotLatency.

 private:
  struct Slot {
    rhi::Buffer buf;
    bool inFlight = false;
    IVec3 base{};      // world chunk coord of the mirror corner
    IVec3 origin{};    // window origin at encode time
    uint32_t particleLivePage = 0;
    uint32_t tick = 0;
    uint32_t epoch = 0;  // pipeline epoch at encode time (see snapEpoch_)
    uint32_t seq = 0;    // ticksEncoded_ at encode time (WorldSnapshot::submitSeq)
    std::vector<IVec3> fetchIds;  // world chunks riding this slot
    // Sentinel slots are not copied at all (§2.1a); their table entry is
    // recorded here at encode time and their 4,096 words are synthesized on
    // consumption. 0 means "a real copy was issued for this index".
    std::vector<uint32_t> fetchSentinel;
    std::array<uint32_t, 27> mirrorSentinel{};
    // Which of the two conditional tables this slot's copy carries (see
    // EncodeReadbacks). A slot without the digest shares the previous
    // snapshot's; a slot without the fold parses as no water.
    bool hashCopied = true;
    bool fluidCopied = true;
    // The body query's boxes this tick (SetElecQueriesInFlight): the TAGS of
    // the answers this slot carries. Empty = no answers copied.
    std::vector<ElecQuery> elecQueries;
  };
  static constexpr int kSlots = kReadbackSlots;
  Slot slots_[kSlots];
  int lastSlot_ = -1;
  WorldSnapshot snap_;
  // ---- the holding queue that makes the latency a CONSTANT ---------------
  //
  // A map callback fires whenever its fence retires, and ProcessEvents fires
  // EVERY ready map at once, so on a fast GPU several ticks' snapshots land in
  // one pump. Consuming them straight into `snap_` is what made the latency a
  // timing property. They are parsed into `ready_` instead (tick order — the
  // Vulkan backend fires pending maps in submission order, rhi_vk.cpp
  // FirePendingMaps) and PublishSnapshotsUpTo moves exactly the one tick T is
  // owed into `snap_`. Storage is recycled through `snapPool_`, so the steady
  // state is kSnapshotLatency + 1 WorldSnapshots, not kReadbackSlots.
  std::deque<WorldSnapshot> ready_;
  std::vector<WorldSnapshot> snapPool_;
  // Every published snapshot's reaction-effect winners, in publish (= tick)
  // order, until TakeReactFx drains them. Appended in PublishSnapshotsUpTo, so
  // a publish that walks two snapshots at once loses neither; capped at
  // kReactFxPendingMax (oldest dropped) so a harness that never drains cannot
  // grow it; cleared by InvalidateSnapshot (a dead world's blasts).
  std::vector<ReactFxEvent> reactFxPending_;
  // Far-landing deposits (TakeTicketDeposits).
  std::vector<ParticleSpawn> ticketDepositsPending_;
  uint64_t ticketDepositsDropped_ = 0;
  // The body query (QueueElecQuery .. TakeElecHits).
  std::vector<ElecQuery> elecQueryQueue_, elecInFlight_;
  std::vector<ElecHit> elecHitsPending_;
  std::vector<uint8_t> elecSourceMat_;
  bool elecSourceSeen_ = false;
  uint32_t elecSourceTick_ = 0;
  uint64_t elecQueriesAsked_ = 0, elecQueryRefused_ = 0;
  uint64_t elecHitsDelivered_ = 0, elecHitsCharged_ = 0;
  // Bumped by InvalidateSnapshot and by a TICK REWIND (a harness scene
  // restarting its counter — see kOrder in test/selftest.cpp). Everything from
  // an older epoch is dropped rather than compared against the new tick base,
  // which unsigned arithmetic would otherwise read as "four billion ticks in
  // the future".
  uint32_t snapEpoch_ = 0;
  uint32_t ticksEncoded_ = 0;  // see TicksEncoded(); monotonic, never reset
  uint32_t dirtyWatch_ = 0xFFFFFFFFu;  // SetDirtyWatch
  uint32_t lastEncodeTick_ = 0;
  bool haveEncodeTick_ = false;
  SnapshotPipeStats snapPipe_;
  IVec3 origin_{0, 0, 0};
  // The ticket boxes (see TicketBox). Written only by Tickets.
  TicketBox tickets_[kTicketMax];
  // Drained into gasSpawnOps by SubmitTick, once, at the head of the tick.
  std::vector<GasSpawnOp> pendingGasSpawns_;
  uint32_t gasLeaveCapTest_ = 0;   // SetGasLeaveCapForTest

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
  // Per-slot quiet streak (see QuietTicks). uint16_t and saturating: 65535
  // ticks is 18 minutes of quiet and every consumer's question is "at least
  // N?", so the cap costs nothing and the array costs 64 KiB instead of 128.
  std::vector<uint16_t> quietTicks_;
  std::vector<uint8_t> snapBounce_;
  // The digest table of the most recently PARSED snapshot, which a slot that
  // skipped the copy inherits. Callbacks parse in submission order, so this is
  // always the table of the newest earlier tick. Dropped on a reset.
  std::shared_ptr<const std::vector<uint32_t>> lastChunkHash_;
  // The next readback must copy the digest table even on a non-hash tick:
  // something outside the tick recurrence rewrote it (see EncodeReadbacks).
  bool chunkHashForce_ = true;
};
