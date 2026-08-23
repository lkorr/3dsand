#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "gpu/rhi.h"

#include "math3d.h"

// World constants. These are the SINGLE source of truth: the matching WGSL
// consts are generated from them by ShaderConstantPrelude() (gpu/resources.cpp)
// and prepended ahead of common.wgsl, so shaders cannot drift from C++.
// 512^3 residency window = 32 m per edge at 6.25 cm voxels (doubled from 256
// on 2026-08-19: the simulated world extends +-16 m around the player). The
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
constexpr uint32_t kChunk = 16;
constexpr uint32_t kNChunk = kWorldN / kChunk;          // 32
constexpr uint32_t kNumChunks = kNChunk * kNChunk * kNChunk;  // 32768
constexpr uint32_t kChunkVol = kChunk * kChunk * kChunk;      // 4096
constexpr uint64_t kVoxelCount = (uint64_t)kWorldN * kWorldN * kWorldN;

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

// ---- MLS-MPM fluid prototype (docs/PLAN_mpm_fluids.md; side-by-side demo) ---
// An EXPERIMENTAL second liquid representation living alongside the CA liquid:
// GPU particles simulated by a fixed-point MLS-MPM solver (sim_fluid.wgsl).
// Deliberately OUTSIDE the hashed sim domain in this prototype — the fluid
// never writes a voxel, never touches dirty flags, and no CA kernel reads any
// fluid buffer, so the world hash is untouched by construction. The fluid is
// still bit-deterministic in its own right (integer-only math, integer-atomic
// P2G scatter — addition is associative, so accumulation order cannot matter),
// which the `fluid_det` selftest gate verifies twice-run. That is the plan's
// Phase-0 determinism spike, run inside the engine.
//
// The particle COUNT is CPU-owned (main loop / selftest gate): particles are
// only ever appended by the spawn kernel at CPU-known offsets and never die,
// so every dispatch extent is a pure function of the op stream. Not persisted:
// save/load and worldgen drop the fluid (count resets to 0), per the plan's
// force-settle-on-save policy — acceptable for a comparison prototype.
constexpr uint32_t kFluidCap = 262144;            // hard particle budget (rule 2)
constexpr uint32_t kMaxFluidSpawnsPerTick = 4096; // spawn-op stream cap
// Sparse scratch-grid blocks: one 16^3 node block per ACTIVE chunk slot,
// allocated per substep by a deterministic scan. 256 blocks * 4096 nodes *
// 16 B = 16 MiB, and bounds simultaneously-active fluid to 256 chunks.
constexpr uint32_t kFluidBlocks = 256;
// MPM substeps per 30 Hz tick. CFL: |v| <= 0.45 cell/substep, so the fluid's
// terminal speed is 0.45 * 6 = 2.7 cells/tick (~8.1 m/s at 0.10 m voxels).
constexpr uint32_t kFluidSubsteps = 6;

// One CPU-authored fluid particle spawn (32 B) — must match FluidSpawnOp in
// common.wgsl. Positions are ABSOLUTE world cells in Q16.16 fixed point
// (fraction bits matter: particles sit at sub-cell lattice offsets), velocity
// is Q16.16 cells/tick. Part of the per-tick input stream like ParticleSpawn.
struct FluidSpawnOp {
  int32_t px, py, pz;   // position, fixed 16.16 world cells
  int32_t vx, vy, vz;   // velocity, fixed 16.16 cells/tick
  uint32_t species = 0; // 0..3: which liquid this is (colour + attraction id)
  uint32_t pad1 = 0;
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
//   bit 31 = 1  SENTINEL.  bits 12..30 = tag (all zero today),
//                          bits 0..11 = material id.
//
// EMPTY is UNIFORM(air): kPtEmpty == kPtSentinelBit | kMatAir with kMatAir 0,
// so there is ONE sentinel decode path and "empty" is not a special case
// anywhere in the shader. The material field shares the voxel word's material
// position, so synthesizing a word from a sentinel is a mask, not a repack.
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
constexpr uint32_t kPtUnresident = 0xFFFFFFFFu;
// Not a valid word index. voxWordIndex() returns it for a sentinel chunk and
// voxStore() tests for it BEFORE indexing, which is what makes "a kernel can
// never write through a sentinel" a property of the signatures rather than of
// anyone remembering a rule (§2.4).
constexpr uint32_t kPtNoWord = 0xFFFFFFFFu;

// Physical pages in the pool under --residency paged. 24,576 pages = 384 MiB,
// a 25% reservation cut from dense's 32,768 pages / 512 MiB. This constant is
// the RESERVED pool, not resident content; conflating the two is how a phase
// claims a win it did not get (§3.7).
//
// SIZED FROM THE REAL GAME, NOT THE HARNESS (2026-08-23, the default-flip
// lesson). The selftest window sits in mostly-sky coordinates and settles at
// 4,975 resident pages (77.7 MiB) with a suite high-water of 14,934 — those
// were the phase-7 sizing numbers, and they are true but UNREPRESENTATIVE:
// the GAME's window centers on a player standing on terrain, half of it
// underground solid, and settles at 16,420 resident pages (peak 16,744
// during the startup window slide, measured with a dense-size pool). A pool
// of 16,384 therefore aborted ON LAUNCH — the first windowed paged run ever
// made. 24,576 is 1.47x the game's measured steady state (comfortably above
// the 1.25x headroom rule) and holds the suite's 14,934 with room.
//
// "Synthetic numbers lie": if this is ever re-tuned, measure with the GAME
// window (--frames + SANDVOX_PT_DEBUG prints per-tick residency), not just
// the suite. Under §3.8 exhaustion stays a loud abort, and the suite
// re-measures its own margin on every paged run (the high-water line at the
// end of --selftest).
//
// The real lever on the underground working set is a follow-up, not a bigger
// pool: ~all of it is single-material-with-state chunks a widened sentinel
// could represent (PLAN_page_table.md §3.6's 2,115-chunk finding, which the
// game window multiplies).
constexpr uint32_t kPoolPages = 24576;

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
  // MLS-MPM fluid prototype: live particle count BEFORE this tick's spawns
  // (also the append base the spawn kernel writes at), and this tick's spawn-op
  // count. Both CPU-owned and pure functions of the op stream (see the fluid
  // block above kFluidCap).
  uint32_t fluidBase = 0;
  uint32_t fluidSpawnCount = 0;
  // Material id each MPM species splashes micro droplets as (0 = species never
  // poured -> no droplets). Recorded from the pour's brush material by the main
  // loop; vec4<u32> on the WGSL side, so keep this 16-byte aligned.
  uint32_t fluidSplashMat[4] = {0, 0, 0, 0};
};

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
  uint32_t pad_dn1 = 0, pad_dn2 = 0;
};
static_assert(sizeof(RenderParams) % 16 == 0,
              "RenderParams must be a whole number of std140 rows");

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
constexpr uint32_t kFarN = 256;
constexpr uint32_t kFarNChunk = kFarN / kChunk;  // 16
constexpr uint32_t kFarNumChunks = kFarNChunk * kFarNChunk * kFarNChunk;  // 4096
constexpr uint32_t kFarVox = kFarN * kFarN * kFarN;  // cells per level
constexpr uint32_t kFarShiftBase = [] {
  uint32_t s = 0;
  while ((kFarN << s) < kWorldN) s++;
  return s;
}();
static_assert(kWorldN >= kFarN && (kFarN << kFarShiftBase) == kWorldN,
              "kFarN must divide kWorldN by a power of two");
// 8 levels: outermost half-extent = (kWorldN << 8)/2 voxels = 4096 m at the
// 512 window and 6.25 cm voxels. farVox = kFarLevels * 16 MiB = 128 MiB.
constexpr uint32_t kFarLevels = 8;
static_assert((uint64_t)kFarLevels * kFarVox < (1ull << 32),
              "farVox byte indices must fit u32");
constexpr uint32_t kFarListCap = 4096;  // fill dispatches per tick (level-chunks)
// Fog reaches ~full opacity (exp(-4.5) ~= 1%) at whatever radius it is pinned
// to; kFogOpticalDepths is that budget, shared by the static pin below and by
// the adaptive term in WriteRenderParams.
constexpr float kFogOpticalDepths = 4.5f;
// Fog density such that opacity ~= 1 at the outermost level's half-extent from
// the centered player. This is the FLOOR of the adaptive density (phase 3B):
// the fully-filled cascade is the farthest anything is ever visible, so fog is
// never thinner than this.
constexpr float kFarFogDensity =
    kFogOpticalDepths / ((float)(kWorldN << kFarLevels) * kVoxelMeters * 0.5f);
// Ceiling of the adaptive density (phase 3B). While a cold start / teleport
// has cascade fills outstanding, fog closes in to hide the unfilled bands —
// but never nearer than cascade level 2's half-extent, which is 4x the
// residency window's own half-extent. That keeps the *simulated* world (the
// thing the player is standing in and editing) fully visible no matter how
// backlogged the fill queue is; only the LOD horizon ever gets fogged away.
constexpr float kFarFogDensityMax =
    kFogOpticalDepths / ((float)(kWorldN << 2) * kVoxelMeters * 0.5f);
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
  // Page faults since process start — voxStore()'s sentinel no-op path
  // (PLAN_page_table.md §2.4). MONOTONIC and never cleared: a non-zero value is
  // a permanent "this build has a bug" latch. Every gate asserts it is zero,
  // which is what turns §2.4's structural claim into a measurement made on
  // every run rather than in a special configuration.
  uint32_t pageFaults = 0;
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

  // Voxel word at cell from the mirror; kind Unknown outside mirror coverage.
  //
  // `classOf` is a COLLISION class table, not simply a copy of each material's
  // klass — build it with BuildCollisionClasses() below. The difference is
  // materials flagged passable (soft vegetation), which report as Air here so
  // bodies move through them while staying ordinary solids everywhere else.
  CellKind KindAt(IVec3 cell, const std::vector<uint32_t>& classOf) const;

  // Deterministic worldgen height — exact CPU mirror of worldgen.wgsl.
  static int TerrainHeight(int x, int z, uint32_t seed);

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
                            // (see the occupancy packing note in common.wgsl)
  rhi::Buffer support;     // kNumChunks u32 — support-loss flags (sim_step writes,
                            // readback consumes + clears; drives island checks)
  rhi::Buffer hash;        // 4 u32 (only [0] used)
  rhi::Buffer tickUBO;     // TickParams
  rhi::Buffer passUBO;     // 27 slices * 256 B (3x3x3 color phases)
  rhi::Buffer opsBuf;      // kMaxOpsPerTick BrushOp
  rhi::Buffer renderUBO;   // RenderParams
  rhi::Buffer pick;        // 8 u32

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

  // ---- MLS-MPM fluid prototype (see the fluid block above kFluidCap) ----
  // None of these is hashed, persisted or read by any CA kernel; fluidGrid,
  // fluidBlockMap and fluidBlockList are per-substep scratch, cleared and
  // rebuilt inside the tick. fluidParticles is the only carried state, and it
  // is reconstructible from the op stream (deterministic solver + spawn ops).
  rhi::Buffer fluidParticles;    // kFluidCap FluidParticle (72 B, see common.wgsl)
  rhi::Buffer fluidSpawnOps;     // kMaxFluidSpawnsPerTick FluidSpawnOp
  rhi::Buffer fluidBlockMap;     // kNumChunks u32: 0 = inactive, else blockIdx+1
  rhi::Buffer fluidBlockList;    // kFluidBlocks u32: blockIdx -> chunk slot
  rhi::Buffer fluidGrid;         // kFluidBlocks * 4096 nodes * 8 i32 (mass,
                                 // mom xyz, species mass x3, pad — FLUID_GW)
  rhi::Buffer fluidArgsStage;    // 4 u32: [0..2] node-pass dispatch args, [3] count
  rhi::Buffer fluidDispatchArgs; // 3 u32, indirect-only (see dispatchArgs note)
  rhi::Buffer debugBoxes;      // kMaxDebugBoxes DebugBox (collision overlay)
  rhi::Buffer bodyInstances;   // debris-body voxel instances (render)
  rhi::Buffer bodyXforms;      // debris-body transforms (render)
  rhi::Buffer genList;         // worldgen streaming: slot indices to generate

  // ---- far-field cascades (render-only; never bound in any sim pipeline) ----
  rhi::Buffer farVox;   // kFarLevels x 256^3 material bytes, packed 4/u32
                         // (atomic in the fill/downsample kernels: partial-word
                         // byte updates from neighboring dirty chunks race)
  rhi::Buffer farOcc;   // kFarLevels x kNumChunks u32 non-air counts
  rhi::Buffer farList;  // kFarListCap u32 fill entries: (level-1)<<12 | slot
  rhi::Buffer farUBO;   // FarParams

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

  std::vector<IVec3> fetchQueue_;
  std::unordered_map<uint64_t, uint8_t> fetchQueued_;   // dedup (packed key)
  std::unordered_map<uint64_t, CachedChunk> cache_;     // packed world key

  // CPU mirror of the page table — the authority for every C++ translation.
  // The GPU buffer is written FROM this, never read back into it: the table is
  // a pure function of CPU-side allocation history (§2.5), so a readback would
  // be asking the GPU about a decision the CPU made.
  std::vector<uint32_t> pageTableCpu_;
};
