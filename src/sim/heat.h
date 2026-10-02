#pragma once
// THE TEMPERATURE LAYER (docs/PLAN_temperature.md, DESIGN.md §4 "Heat").
//
// Three buffers, all addressed through the constants below. Each constant a
// shader reads is MIRRORED in that shader (sim_heat.wgsl, sim_step.wgsl) and
// held to this file by scripts/check_invariants.py `heat` -- declared there,
// not in common.wgsl, because only those two shaders agree on them and a
// common.wgsl edit misses the SPIR-V cache of every shader (CLAUDE.md).
//
//   heatPool    GPU-owned. kHeatPoolPages pages of kHeatPageWords. A page is
//               one window chunk's 8^3 blocks of 2^3 voxels, three PLANES of
//               512 words:
//                 plane 0  X (local excess, u8) | X* (target excess, u8)
//                          | E (hottest emitter, u8) | n (emitting cells, 4b)
//                          | k (inertia, 3b)
//                 plane 1  the tent filter's x pass: sum (u16) | max E (u8)
//                 plane 2  its y pass, same layout
//               A page on the free stack is ALL ZERO, always (heatRelax frees
//               only an all-zero page; heatShift zeroes what it releases).
//   heatMeta    GPU-owned. Header (counters, list counts, args, stats), then
//               per WINDOW slot: entry, flags, summary, owner chunk; the want
//               bitset; three work lists; the free stack. All zero = an empty
//               layer, which is what the worldgen / load fills leave.
//   heatParams  CPU-written (Simulation::PrepareHeat / UploadTables). Header
//               (mode, radius, snowline), per-biome climate, per-material
//               transitions, and the BIOME OF EVERY WINDOW CELL COLUMN.
//
// Temperature is in HEAT UNITS, 0 = water's freezing point, signed. Actual
// temperature T = ambient(biome, day, snowline) + X; X relaxes toward X*,
// X* = max(0, min(tent sum of sources, hottest source in reach - ambient)).
#include <cstdint>
#include <string>
#include <vector>


// ---- geometry ---------------------------------------------------------------
// The window's numbers, restated so this header stays light enough for
// materials.h to include; heat.cpp static_asserts them against world.h.
constexpr uint32_t kHeatChunk = 16;            // world.h kChunk
constexpr uint32_t kHeatWindowChunks = 32768;  // world.h kHeatWindowChunks
constexpr uint32_t kHeatWorldN = 512;          // world.h kWorldN
constexpr uint32_t kHeatBlockShift = 1;                      // 2^3 voxels a block
constexpr uint32_t kHeatBlocksPerAxis = kHeatChunk >> kHeatBlockShift;   // 8
constexpr uint32_t kHeatBlocks = kHeatBlocksPerAxis * kHeatBlocksPerAxis * kHeatBlocksPerAxis;  // 512
constexpr uint32_t kHeatPageWords = 3 * kHeatBlocks;          // 1,536 = 6 KiB
// Sized to MORE THAN TWICE the worst peak measured (2026-10-02, the [perf]
// "heat:" line): --perf forestfire 1,704 pages, --perf village-fire 2,278
// (three oil-filled houses burning). 5,120 = 2.25x the village; 30 MiB. A
// refused allocation is counted (kHmRefused, last_run.json heat.refused) and
// `heat-bound` fails on any.
constexpr uint32_t kHeatPoolPages = 5120;
constexpr uint32_t kHeatRadiusMax = 8;   // blocks; <= one chunk so every tap is in N27

// ---- heatMeta ---------------------------------------------------------------
constexpr uint32_t kHmHdrWords = 128;
constexpr uint32_t kHmWantCount = 0;
constexpr uint32_t kHmFreeTop = 1;     // pages on the free stack
constexpr uint32_t kHmNextFresh = 2;   // pages never handed out start here
constexpr uint32_t kHmSrcCount = 3;
constexpr uint32_t kHmRecompCount = 4;
constexpr uint32_t kHmRelaxCount = 5;
constexpr uint32_t kHmLastDay = 6;     // 1 night, 2 day, 0 unknown
constexpr uint32_t kHmDayFlip = 7;
constexpr uint32_t kHmOrigin = 8;      // 8..10 last origin, 11 = set
constexpr uint32_t kHmOriginSet = 11;
// Chunks that set their bit in the kHmPend bitset during the last heatRelax
// (still relaxing, not woken for the CA): heatBegin arms heatPend from it and
// zeroes it.
constexpr uint32_t kHmPendCount = 12;
// Why heatShift is armed this tick (bit set = do it): kHeatShiftRelease (the
// window moved, or the layer is off), kHeatShiftDawn (the daylight switch to
// DAY: every kept page's X and X* drop by the ambient step, so the actual
// temperature T = ambient + X is continuous and never passes the hottest
// source in reach -- the ceiling).
constexpr uint32_t kHmShiftWhy = 13;
constexpr uint32_t kHeatShiftRelease = 1u, kHeatShiftDawn = 2u;
// Stats, words 16..31 -- the snapshot carries words 0..31 (kHeatSnapWords).
constexpr uint32_t kHmPagesPeak = 16;
constexpr uint32_t kHmRefused = 17;    // monotonic: wanted chunks the pool could not page
constexpr uint32_t kHmMelts = 18;      // monotonic firings, per transition kind
constexpr uint32_t kHmIgnites = 19;
constexpr uint32_t kHmFreezes = 20;
constexpr uint32_t kHmSrcPeak = 21;
constexpr uint32_t kHmRecompPeak = 22;
constexpr uint32_t kHmRelaxPeak = 23;
constexpr uint32_t kHmRelaxTicks = 24; // chunk-ticks whose X moved, monotonic
constexpr uint32_t kHmFrees = 25;
constexpr uint32_t kHmAllocs = 26;
constexpr uint32_t kHmReleased = 27;   // pages released by heatShift
constexpr uint32_t kHmLastRelax = 28;  // relax chunks THIS tick
constexpr uint32_t kHmLastRecomp = 29; // recompute chunks THIS tick
// The F1 probe, written by heatBegin every CA-active tick (so it follows the
// player into a cold, unpaged chunk instead of keeping the last hot reading):
// X | X* << 8 | 0x10000 if paged | the probe BLOCK's tag << 17 (HeatProbeTag),
// so a reader can tell a reading of where it stands from a stale one.
constexpr uint32_t kHmProbeX = 30;
constexpr uint32_t kHmProbeE = 31;     // the probe block's plane-0 high half
constexpr uint32_t kHeatProbeTagShift = 17;
constexpr uint32_t kHeatSnapWords = 32;
// Indirect args: six 16-byte records, copied to heatArgs by copy_heatArgs.
constexpr uint32_t kHmArgs = 32;
constexpr uint32_t kHeatArgAlloc = 0, kHeatArgSrc = 1, kHeatArgRecomp = 2,
                   kHeatArgRelax = 3, kHeatArgShift = 4, kHeatArgPend = 5, kHeatArgRecords = 6;
constexpr uint32_t kHeatArgsBytes = kHeatArgRecords * 16;
static_assert(kHmArgs * 4 == 128, "pass_table.def copy_heatArgs reads byte 128");
static_assert(kHeatArgsBytes == 96, "pass_table.def copy_heatArgs copies 96 bytes");
static_assert(kHmArgs + kHeatArgRecords * 4 <= kHmHdrWords, "the args fit the header");
// THE FIRING LOG (diagnostic, never read by the sim): the first
// kHeatFireLogMax thermal transitions since the layer was last reset (a
// worldgen or a load) record their cell and kind -- kHmFireLog counts every
// firing, entry i at kHmFireLog + 1 + 4i is x, y, z, kind. A bare "1,538
// freezes" names no lake; this names the cell. WHICH firings land in the log
// depends on GPU scheduling; it is a report, so that is harmless.
constexpr uint32_t kHmFireLog = 64;
constexpr uint32_t kHeatFireLogMax = 8;
static_assert(kHmFireLog + 1 + 4 * kHeatFireLogMax <= kHmHdrWords, "the log fits the header");

constexpr uint32_t kHmEntry = kHmHdrWords;                     // HAS | page
constexpr uint32_t kHmFlags = kHmEntry + kHeatWindowChunks;           // kHf* bits
constexpr uint32_t kHmSummary = kHmFlags + kHeatWindowChunks;         // kHs* bits
constexpr uint32_t kHmOwner = kHmSummary + kHeatWindowChunks;         // packed world chunk
constexpr uint32_t kHmWant = kHmOwner + kHeatWindowChunks;            // bitset
// Still-relaxing chunks NOT woken for the CA (heatRelax sets, heatPend
// consumes next tick): the field keeps walking to its target without running
// the CA over the chunk, which is what the halo round a fire used to cost.
constexpr uint32_t kHmPend = kHmWant + kHeatWindowChunks / 32;    // bitset
constexpr uint32_t kHmSrcList = kHmPend + kHeatWindowChunks / 32;
constexpr uint32_t kHmRecompList = kHmSrcList + kHeatWindowChunks;
constexpr uint32_t kHmRelaxList = kHmRecompList + kHeatWindowChunks;
constexpr uint32_t kHmStack = kHmRelaxList + kHeatWindowChunks;
constexpr uint32_t kHmWords = kHmStack + kHeatPoolPages;

constexpr uint32_t kHeatEntryHas = 0x80000000u;
constexpr uint32_t kHeatEntryPage = 0x00FFFFFFu;
constexpr uint32_t kHfEmit = 1u, kHfSrc = 2u, kHfRecomp = 4u, kHfRelax = 8u, kHfNew = 16u;
constexpr uint32_t kHsSource = 1u, kHsNonzero = 2u;
// The summary's TRIGGERS (heatSrc, from the chunk's cells): the lowest
// melt / ignite threshold and the highest freeze threshold of any material
// in the chunk, each + kHeatThrBias in 10 bits, 0 = none. heatRelax wakes the
// CA over a chunk only where a transition-bearing block's temperature range
// [T, T*] reaches one of them -- elsewhere a moving field cannot change a
// cell, and the chunk relaxes on the pend list without the CA.
constexpr uint32_t kHsTrigAboveShift = 2, kHsTrigBelowShift = 12, kHsTrigMask = 1023u;
// Plane-0 bit 31 (above the inertia nibble): some cell of this block carries a
// thermal transition (heatSrc).
constexpr uint32_t kHeatBlockTrans = 0x80000000u;

// ---- heatParams -------------------------------------------------------------
constexpr uint32_t kHpHdrWords = 32;
constexpr uint32_t kHpMode = 0;        // sim.heatMode
constexpr uint32_t kHpRadius = 1;      // sim.heatRadius, blocks
constexpr uint32_t kHpSnowlineY = 2;   // i32: cells at or above take the snowline climate
constexpr uint32_t kHpSnowBase = 3;    // i32
constexpr uint32_t kHpSnowSwing = 4;   // i32
constexpr uint32_t kHpProbe = 5;       // 5..7 probe cell (i32), 8 = probe on
constexpr uint32_t kHpProbeOn = 8;
constexpr uint32_t kHpGain = 9;        // sim.heatGain: coverage saturates at 1/G
constexpr uint32_t kHeatBiomesMax = 64;
constexpr uint32_t kHpBiome = kHpHdrWords;                     // base, swing (i32) per biome
constexpr uint32_t kHpMat = kHpBiome + 2 * kHeatBiomesMax;     // 8 words per material
constexpr uint32_t kHpMatStride = 8;
constexpr uint32_t kHeatMatMax = 4096;
constexpr uint32_t kHpCol = kHpMat + kHpMatStride * kHeatMatMax;  // biome byte per window column
constexpr uint32_t kHpColWords = kHeatWorldN * kHeatWorldN / 4;
constexpr uint32_t kHpWords = kHpCol + kHpColWords;

// One transition = three words at kHpMat + mat*8 + 3*k:
//   w0  threshold+512 (bits 0..9) | full+512 (10..19) | kind (20..21)
//       | scale (22..23) | surface (24)
//   w1  chance, in 1/kReactChanceDen per tick at `full`
//   w2  product (bits 0..11) | partial product (12..23, 0 = none)
constexpr uint32_t kHeatMaxTransitions = 2;
constexpr int32_t kHeatThrBias = 512;
constexpr uint32_t kHeatKindMelt = 1, kHeatKindIgnite = 2, kHeatKindFreeze = 3;
constexpr uint32_t kHeatScaleNotSelf = 1, kHeatScaleAir = 2;
// MaterialGpu._r2 (bits 0..11 are the catch form): emit, inertia, transitions.
constexpr uint32_t kHeatR2EmitShift = 12, kHeatR2InertiaShift = 20, kHeatR2TransShift = 23;

// ---- the authored data (materials.json "thermal") ---------------------------
struct HeatTransition {
  uint32_t kind = 0;                 // kHeatKind*
  int32_t threshold = 0;             // melt/ignite: fires ABOVE; freeze: BELOW
  int32_t full = 0;                  // the chance reaches `chance` here
  double chanceMille = 0.0;          // per tick at `full`, before any scaling
  std::string into, partialInto;     // names, resolved after the table loads
  uint32_t intoId = 0, partialIntoId = 0;
  bool surface = false;              // only with air directly above
};
struct ThermalDef {
  uint32_t emit = 0;                 // 0..255: the block is held at least this hot
  int32_t inertia = -1;              // 0..7, -1 = the class default
  std::vector<HeatTransition> transitions;
  bool Any() const { return emit != 0 || inertia >= 0 || !transitions.empty(); }
};

// ---- the per-biome climate (assets/biomes/<name>.json climate.ambient) ------
struct HeatClimate {
  int32_t base = 10;
  int32_t swing = 0;
  int32_t Day() const { return base + swing; }
  int32_t Night() const { return base - swing; }
};

// Packs one material's GPU words (8) from its ThermalDef. `chanceScale` is
// the per-transition chance multiplier the loader applies (spreadPct for an
// ignition). Pure; used by Simulation::UploadTables and the gates.
void PackHeatMaterial(const ThermalDef& t, uint32_t out[kHpMatStride]);
// What build/last_run.json's `heat` block reports: peaks and totals over every
// heatMeta header a gate handed HeatNoteRun (one per fixture run). Gates are
// sequential, so a plain global is enough; nothing in the sim reads it.
struct HeatRunStats {
  uint32_t runs = 0, pagesPeak = 0, refused = 0, melts = 0, ignites = 0, freezes = 0;
  uint32_t srcPeak = 0, recompPeak = 0, relaxPeak = 0;
};
void HeatNoteRun(const uint32_t* header);   // heatMeta words 0..kHeatSnapWords
const HeatRunStats& HeatRunTotals();
// Packed world-chunk key for heatMeta's owner word (10 bits an axis).
inline uint32_t HeatOwnerKey(int x, int y, int z) {
  return ((uint32_t)x & 1023u) | (((uint32_t)y & 1023u) << 10) | (((uint32_t)z & 1023u) << 20);
}
// Ambient at world cell (x, y, z) on the CPU (F1 readout, gates): the same
// arithmetic as sim_step.wgsl / sim_heat.wgsl heatAmbient.
int32_t HeatAmbientCpu(int x, int y, int z, uint32_t seed, bool day);
// The climate the engine uses for biome `index` (default if unknown), and the
// snowline climate + Y (worldmap::CurrentWorldMap / tuning / map terrain).
const HeatClimate& HeatBiomeClimate(uint32_t biomeIndex);
HeatClimate HeatSnowlineClimate();
int32_t HeatSnowlineY();
