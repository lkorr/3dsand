#include "gpu/resources.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "sim/materials.h"  // kRepose*/kMatRepose*: the REPOSE_* prelude consts
#include "sim/treeatlas.h"   // CurrentTreeLattice: the TREE_* prelude consts
#include "sim/tuning.h"
#include "sim/world.h"
#include "sim/worldmap.h"    // CurrentWorldMap().pondTile: the POND_TILE prelude const

// ---- GPU buffer budget -----------------------------------------------------
// Every storage/uniform/indirect buffer the engine owns is created through this
// one function, so tallying here is exhaustive by construction rather than by a
// list somebody has to remember to extend. Sizing questions ("what does the
// window / the far field actually cost?") were previously answered by
// re-deriving constants on paper, which is how ROADMAP_scale.md ended up with a
// memory anchor that has never been checked against an allocation.
// Diagnostics only: nothing reads the tally, and it is not in any hash.
//
// LIVE buffers, not every buffer ever made: each record holds a weak reference
// to the seam object, and a record whose buffer has been released is dropped
// at the next read. The tally used to be append-only, so every gate's staging
// read and every F5-rebuilt buffer stayed in the "budget" forever and the total
// only grew. Locked because buffers are created from build-pool threads.
namespace {
struct BufferLogEntry {
  GpuBufferRecord rec;
  std::weak_ptr<rhi::BufferImpl> live;
};
std::mutex& BufferLogMutex() {
  static std::mutex m;
  return m;
}
std::vector<BufferLogEntry>& BufferLog() {
  static std::vector<BufferLogEntry> log;
  return log;
}
// Caller holds BufferLogMutex.
void PruneBufferLog() {
  auto& log = BufferLog();
  log.erase(std::remove_if(log.begin(), log.end(),
                           [](const BufferLogEntry& e) { return e.live.expired(); }),
            log.end());
}
}  // namespace

rhi::Buffer CreateBuffer(const rhi::Device& device, uint64_t size,
                         rhi::BufferUsage usage, const char* label) {
  rhi::Buffer b = device.CreateBuffer(size, usage, label);
  if (b) {
    std::lock_guard<std::mutex> lock(BufferLogMutex());
    PruneBufferLog();  // keeps the log bounded by the live set
    BufferLog().push_back({{label ? label : "<unlabelled>", size}, b.Ref()});
  }
  return b;
}

uint64_t GpuBufferBytesTotal() {
  std::lock_guard<std::mutex> lock(BufferLogMutex());
  PruneBufferLog();
  uint64_t t = 0;
  for (const auto& e : BufferLog()) t += e.rec.bytes;
  return t;
}

std::vector<GpuBufferRecord> GpuBufferRecords() {
  std::lock_guard<std::mutex> lock(BufferLogMutex());
  PruneBufferLog();
  std::vector<GpuBufferRecord> out;
  out.reserve(BufferLog().size());
  for (const auto& e : BufferLog()) out.push_back(e.rec);
  return out;
}

void DumpGpuBufferBudget(const char* whenLabel) {
  auto sorted = GpuBufferRecords();
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const GpuBufferRecord& a, const GpuBufferRecord& b) {
                     return a.bytes > b.bytes;
                   });
  const double kMiB = 1024.0 * 1024.0;
  uint64_t total = 0;
  for (const auto& r : sorted) total += r.bytes;
  printf("---- GPU buffer budget (%s): %llu buffers, %.2f MiB ----\n", whenLabel,
         (unsigned long long)sorted.size(), (double)total / kMiB);
  // Everything at or above 1 MiB, individually; the rest as one line. The tail
  // is ~60 buffers of a few hundred bytes each and reading it teaches nothing.
  uint64_t tail = 0;
  size_t tailCount = 0;
  for (const auto& r : sorted) {
    if (r.bytes >= (1u << 20)) {
      printf("  %-24s %10.2f MiB\n", r.label.c_str(), (double)r.bytes / kMiB);
    } else {
      tail += r.bytes;
      tailCount++;
    }
  }
  printf("  %-24s %10.2f MiB  (%llu buffers under 1 MiB)\n", "<small>",
         (double)tail / kMiB, (unsigned long long)tailCount);
  fflush(stdout);
}

static bool ReadFileText(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

// common.wgsl is prepended to EVERY shader, so a BuildPipelines used to read
// it from disk once per shader file (23 times) and a --sweep / F5 loop paid
// that again per rebuild. Cached by (path, mtime, size): one stat per shader
// instead of one read, and an edit still takes effect on the next load — the
// F5 contract — because the edit moves the mtime. Thread-safe: LoadShader runs
// on the build pool.
static bool ReadCommonCached(const std::string& path, std::string& out) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const auto mtime = fs::last_write_time(path, ec);
  const auto size = ec ? 0 : fs::file_size(path, ec);
  if (ec) return ReadFileText(path, out);  // let the plain read report it
  static std::mutex m;
  static std::string cPath, cText;
  static fs::file_time_type cTime{};
  static uintmax_t cSize = 0;
  static bool cValid = false;
  {
    std::lock_guard<std::mutex> lock(m);
    if (cValid && cPath == path && cTime == mtime && cSize == size) {
      out = cText;
      return true;
    }
  }
  if (!ReadFileText(path, out)) return false;
  std::lock_guard<std::mutex> lock(m);
  cPath = path;
  cText = out;
  cTime = mtime;
  cSize = size;
  cValid = true;
  return true;
}

// World constants, emitted as WGSL from the C++ definitions in world.h so the
// two can never disagree. Previously common.wgsl redeclared these by hand and a
// mismatch was silent: kVoxelMeters drifting from VOXEL_METERS just meant the
// renderer lit the world at a different physical scale than the one the player
// walked in. Anything derived from world.h belongs here, not in common.wgsl.

// The one exception, and it is flagged rather than hidden: a DEVICE CAPABILITY,
// not a world constant — see resources.h. Defaults true so every prelude
// consumer that has no device (check_shaders.sh, the save fingerprint)
// assembles the shipping variant of raymarch.wgsl rather than a fallback one
// nobody runs.
static bool g_fragmentStoresAvailable = true;
void SetFragmentStoresAvailable(bool available) {
  g_fragmentStoresAvailable = available;
}
bool FragmentStoresAvailable() { return g_fragmentStoresAvailable; }

static bool g_renderStatsEnabled = false;
void SetRenderStatsEnabled(bool on) { g_renderStatsEnabled = on; }
bool RenderStatsEnabled() { return g_renderStatsEnabled; }

// The generated text depends on compile-time world.h constants plus exactly
// these load-time inputs; ShaderConstantPrelude memoizes on them so the build
// pool's 23 LoadShader calls format it once instead of 23 times. ADD AN INPUT
// HERE when the prelude grows a non-constant line, or a reload that moves it
// will keep serving the old text. (The asset-derived worldgen constants are
// NOT in this prelude -- see WorldgenPrelude.)
static std::string PreludeInputsKey() {
  return std::to_string((int)g_fragmentStoresAvailable) + "," +
         std::to_string((int)g_renderStatsEnabled);
}
static std::string BuildShaderConstantPrelude();
std::string ShaderConstantPrelude() {
  static std::mutex m;
  static std::string key, text;
  const std::string k = PreludeInputsKey();
  {
    std::lock_guard<std::mutex> lock(m);
    if (!text.empty() && key == k) return text;
  }
  std::string t = BuildShaderConstantPrelude();
  std::lock_guard<std::mutex> lock(m);
  key = k;
  text = t;
  return t;
}

static std::string BuildShaderConstantPrelude() {
  std::ostringstream o;
  o << "// GENERATED from src/sim/world.h by ShaderConstantPrelude() — do not\n"
       "// edit, and do not redeclare these in common.wgsl.\n";
  o << "const WORLD_N : u32 = " << kWorldN << "u;\n";
  o << "const CHUNK : u32 = " << kChunk << "u;\n";
  o << "const NCHUNK : u32 = " << kNChunk << "u;\n";
  o << "const NUM_CHUNKS : u32 = " << kNumChunks << "u;\n";
  o << "const CHUNK_VOL : u32 = " << kChunkVol << "u;\n";
  // TICKET SLOTS (world.h kTicketMax, docs/PLAN_chunk_tickets.md §2.1). The
  // split every shader below depends on:
  //   NUM_CHUNKS   how many chunks the WINDOW holds. The toroidal mask wraps at
  //                it, chunkInWindow measures against it, and the raymarch clips
  //                to it. Purely geometric.
  //   NUM_SLOTS    how many per-slot RECORDS exist. Buffer extents, dispatch
  //                bounds over the slot space, and the plane strides inside
  //                multi-plane per-slot buffers are all this.
  // At kTicketMax = 0 they are equal, which is what makes P0 bit-identical.
  o << "const TICKET_SLOTS : u32 = " << kTicketSlots << "u;\n";
  o << "const NUM_SLOTS : u32 = " << kNumSlots << "u;\n";
  // Toroidal addressing masks/shifts (DESIGN.md §3) — sizes are powers of two,
  // so world->slot mapping is a bitmask even for negative world coords.
  uint32_t chunkShift = 0;
  while ((1u << chunkShift) < kChunk) chunkShift++;
  o << "const CHUNK_SHIFT : u32 = " << chunkShift << "u;\n";
  o << "const CHUNK_MASK : i32 = " << (kChunk - 1) << ";\n";
  o << "const WORLD_MASK : i32 = " << (kWorldN - 1) << ";\n";
  o << "const NCHUNK_MASK : i32 = " << (kNChunk - 1) << ";\n";
  o << "const CELLOP_IF_AIR : u32 = 0x" << std::hex << kCellOpIfAir << std::dec
    << "u;\n";
  // Sub-chunk occupancy bitmask (world.h kSubOccShift, PLAN_surface_flight_perf
  // A2). Lives in the tail of the `occupancy` buffer, past the count words, so
  // SUBOCC_BASE is a WORD offset and not a separate binding.
  o << "const SUBOCC_SHIFT : u32 = " << kSubOccShift << "u;\n";
  o << "const SUBOCC_DIM : u32 = " << kSubOccDim << "u;\n";
  o << "const SUBOCC_WORDS : u32 = " << kSubOccWords << "u;\n";
  o << "const SUBOCC_STRIDE : u32 = " << kSubOccStride << "u;\n";
  // NUM_SLOTS, not NUM_CHUNKS: this is the word where the COUNT half of the
  // occupancy buffer ends, and that half is one word per SLOT.
  o << "const SUBOCC_BASE : u32 = " << kNumSlots << "u;\n";
  // Openness grid (world.h kOpenFaces block). Its own buffer, so what the
  // shaders need is the per-chunk WORD stride and the face count; the block
  // count is SUBOCC_DIM^3, already above.
  o << "const OPEN_FACES : u32 = " << kOpenFaces << "u;\n";
  o << "const OPEN_WORDS_PER_CHUNK : u32 = " << kOpenWordsPerChunk << "u;\n";
  // log2(kWorldN): the shadow request record packs a window-relative cell as
  // three fields of this width plus a 3-bit face, so it is the constant that
  // decides whether that record still fits in a u32 (world.h static_asserts it).
  {
    uint32_t ws = 0;
    while ((1u << ws) < kWorldN) ws++;
    o << "const WORLD_SHIFT : u32 = " << ws << "u;\n";
  }
  // Voxel-keyed shadow cache (world.h's kShadowCacheBuckets block). Shared by
  // raymarch.wgsl (reads + registers) and shadow_resolve.wgsl (casts + writes),
  // which is exactly why these are generated here rather than written into
  // either shader: two copies of a layout constant is the failure this prelude
  // exists to prevent.
  o << "const SHADOW_CACHE_AVAILABLE : bool = "
    << (g_fragmentStoresAvailable ? "true" : "false") << ";\n";
  o << "const SHADOW_CACHE_BUCKETS : u32 = " << kShadowCacheBuckets << "u;\n";
  o << "const SHADOW_REQ_HEADER : u32 = " << kShadowReqHeaderWords << "u;\n";
  o << "const SHADOW_REQ_WORDS : u32 = " << kShadowReqWords << "u;\n";
  o << "const SHADOW_REQ_CAP : u32 = " << kShadowReqCap << "u;\n";
  o << "const SHADOW_SUBDIV_MAX : u32 = " << kShadowSubdivMax << "u;\n";
  // The raymarch's step counters (world.h kRenderStat* block). ANDed with the
  // fragment-stores cap in the shader, like the shadow cache: the counters are
  // atomics from a fragment shader and need the same device feature.
  o << "const RENDER_STATS : bool = "
    << ((g_renderStatsEnabled && g_fragmentStoresAvailable) ? "true" : "false")
    << ";\n";
  o << "const RENDER_STATS_SLOTS : u32 = " << kRenderStatSlots << "u;\n";
  o << "const RENDER_STATS_STRIPES : u32 = " << kRenderStatStripes << "u;\n";
  // Software page table (docs/PLAN_page_table.md §2.2). One u32 per chunk SLOT:
  // bit 31 clear = a page index into the physical pool, bit 31 set = a sentinel
  // carrying a material id in bits 0..11. EMPTY is UNIFORM(air), so there is
  // one sentinel decode path and "empty" is not a special case in the shader.
  // PT_NO_WORD is not a valid word index — voxWordIndex returns it for a
  // sentinel chunk and voxStore tests it before indexing.
  o << "const PT_SENTINEL_BIT : u32 = 0x" << std::hex << kPtSentinelBit
    << std::dec << "u;\n";
  o << "const PT_MAT_MASK : u32 = 0x" << std::hex << kPtMatMask << std::dec
    << "u;\n";
  // Bit 30 of a sentinel: the chunk is one material carrying worldgen's
  // per-cell palette variant, so its words vary by POSITION and are
  // synthesized by synthWordAt/synthJitterState rather than synthWord. This is
  // what collapses the buried bulk that UNIFORM's whole-word rule refuses
  // (world.h's JITTER block).
  o << "const PT_JITTER_BIT : u32 = 0x" << std::hex << kPtJitterBit << std::dec
    << "u;\n";
  o << "const PT_EMPTY : u32 = 0x" << std::hex << kPtEmpty << std::dec << "u;\n";
  o << "const PT_PAGE_MASK : u32 = 0x" << std::hex << kPtPageMask << std::dec
    << "u;\n";
  o << "const PT_UNRESIDENT : u32 = 0x" << std::hex << kPtUnresident << std::dec
    << "u;\n";
  o << "const PT_NO_WORD : u32 = 0x" << std::hex << kPtNoWord << std::dec
    << "u;\n";
  // Stain palette: reserved material-table entries holding stain-type colours
  // (world.h). The renderer indexes materials[STAIN_PALETTE_BASE + type].
  o << "const STAIN_PALETTE_BASE : u32 = " << kStainPaletteBase << "u;\n";
  // Art palette: reserved material-table entries holding a mob skin's per-voxel
  // ART colours, which are independent of its material (world.h). The micro and
  // cube body passes index materials[ART_PALETTE_BASE + slot].
  // Both body passes index materials[ART_PALETTE_BASE + (art - 1)] with a
  // 1-BASED merged art index; 0 means unpainted. There is deliberately no
  // ART_SLOT_MIN any more — the .vox palette slot is converted to a merged
  // index once, on the CPU at load (MicroBodyMergeArt), so no shader needs to
  // know where the .vox art range starts.
  o << "const ART_PALETTE_BASE : u32 = " << kArtPaletteBaseGpu << "u;\n";
  // Tint palette: a third reserved run holding per-material GRID colours, which
  // a MATF_TINTED material's state nibble indexes (world.h). paletteColor()
  // reads materials[TINT_PALETTE_BASE + tintBase + state].
  o << "const TINT_PALETTE_BASE : u32 = " << kTintPaletteBaseGpu << "u;\n";
  // Far slot palette: a fourth reserved run mapping a far cascade cell's 7-bit
  // FAR SLOT back to the material id it paints (world.h). raymarch.wgsl's
  // farSlotMat() reads materials[FAR_PALETTE_BASE + slot].flags; the forward
  // direction (material -> slot) rides in every real material's own `flags`
  // word at MATF_FAR_SLOT_SHIFT, so worldgen needs no table lookup at all.
  o << "const FAR_PALETTE_BASE : u32 = " << kFarPaletteBaseGpu << "u;\n";
  // Static micro-detail (render-only, DESIGN.md §9): the size of the brick pool
  // the raymarcher bounds-checks its nested DDA fetches against.
  o << "const MICRO_POOL_WORDS : u32 = " << kMicroPoolWordsWorld << "u;\n";
  // Dynamic microvoxel bodies (render-only, DESIGN.md §9): the pool the
  // microbody fragment march bounds-checks its brick fetches against.
  o << "const MICRO_BODY_POOL_WORDS : u32 = " << kMicroBodyPoolWordsWorld << "u;\n";
  o << "const MATERIAL_SLOTS : u32 = " << kMaterialSlots << "u;\n";
  // ---- angle of repose (src/sim/materials.h kRepose* / kMatRepose*) -------
  // The five run:rise TIER CODES and the (codeA, codeB, blend) packing of
  // MaterialGpu.repose, emitted rather than restated in sim_step.wgsl for the
  // reason every other layout constant is: two places that must agree is a
  // silent bug. The ORDER of the codes is load-bearing on the shader side --
  // `code < REPOSE_1_2` is how the diagonal gate says "not a steep tier".
  o << "const REPOSE_1_1 : u32 = " << kRepose1To1 << "u;\n";
  o << "const REPOSE_2_1 : u32 = " << kRepose2To1 << "u;\n";
  o << "const REPOSE_3_1 : u32 = " << kRepose3To1 << "u;\n";
  o << "const REPOSE_1_2 : u32 = " << kRepose1To2 << "u;\n";
  o << "const REPOSE_1_3 : u32 = " << kRepose1To3 << "u;\n";
  o << "const MAT_REPOSE_A_SHIFT : u32 = " << kMatReposeCodeAShift << "u;\n";
  o << "const MAT_REPOSE_A_MASK : u32 = " << kMatReposeCodeAMask << "u;\n";
  o << "const MAT_REPOSE_B_SHIFT : u32 = " << kMatReposeCodeBShift << "u;\n";
  o << "const MAT_REPOSE_B_MASK : u32 = " << kMatReposeCodeBMask << "u;\n";
  o << "const MAT_REPOSE_BLEND_SHIFT : u32 = " << kMatReposeBlendShift << "u;\n";
  o << "const MAT_REPOSE_BLEND_MASK : u32 = " << kMatReposeBlendMask << "u;\n";
  // Water bodies (docs/PLAN_water_master.md; sim_waterbody.wgsl). The caps that
  // size the TickParams arrays and the GPU ledger buffer, generated here for
  // the same reason every other layout constant is: a shader that redeclared
  // them would be a second place the cap has to be changed.
  o << "const WATERBODY_CAP : u32 = " << kWaterBodyCap << "u;\n";
  o << "const WATERBODY_WORDS : u32 = " << kWaterBodyWords << "u;\n";
  o << "const WATERBODY_STATE_WORDS : u32 = " << kWaterBodyStateWords << "u;\n";
  o << "const WATER_CHUNK_CAP : u32 = " << kWaterChunkCap << "u;\n";
  o << "const WATER_DRAIN_OPS : u32 = " << kWaterDrainOpsPerBody << "u;\n";
  // M5: the measured container curve and the split map, which live past the end
  // of the ledger in the SAME buffer (world.h's kWaterCurveBase block says why).
  // Generated for the reason every layout constant is: WATER_CURVE_BASE is
  // arithmetic over three other caps, and a shader that recomputed it would go
  // on reading the old offset the day one of them moved.
  o << "const WATER_SPLIT_GRID : u32 = " << kWaterSplitGrid << "u;\n";
  o << "const WATER_SPLIT_CELLS : u32 = " << kWaterSplitCells << "u;\n";
  o << "const WATER_SPLIT_WORDS : u32 = " << kWaterSplitWords << "u;\n";
  o << "const WATER_CURVE_MAXY : u32 = " << kWaterCurveMaxY << "u;\n";
  o << "const WATER_SWEEP_HEADER : u32 = " << kWaterSweepHeaderWords << "u;\n";
  o << "const WATER_CURVE_WORDS : u32 = " << kWaterCurveWords << "u;\n";
  o << "const WATER_CURVE_BASE : u32 = " << kWaterCurveBase << "u;\n";
  o << "const WATER_SCRATCH_BASE : u32 = " << kWaterSweepScratchBase << "u;\n";
  // W1: the relevel histogram, past the sweep scratch in the same buffer
  // (docs/PLAN_water_relevel.md §3.2). WATER_RELEVEL_HIST_BASE is arithmetic
  // over four other caps, which is exactly why it is generated and never
  // restated in WGSL.
  o << "const WATER_RELEVEL_DEPTH_MAX : i32 = " << kWaterRelevelDepthMax
    << ";\n";
  o << "const WATER_RELEVEL_BUCKETS : u32 = " << kWaterRelevelBuckets << "u;\n";
  o << "const WATER_RELEVEL_HIST_BASE : u32 = " << kWaterRelevelHistBase
    << "u;\n";
  // W2: the per-column stride of `waterFlux` (docs/PLAN_water_relevel.md §4.1).
  // Generated rather than restated for the histogram's reason: the record is
  // four pipes plus a height plus a stamp, and a shader that hard-coded 6 would
  // go on striding by 6 the day a seventh word landed.
  o << "const WATER_FLUX_WORDS : u32 = " << kWaterFluxWords << "u;\n";
  // Far-field cascades (render-only LOD, DESIGN.md §9). The far field lives on
  // its own kFarN^3 grid, decoupled from the window; level k (1-based) cells
  // span 2^(k + FAR_SHIFT_BASE) fine voxels (see world.h).
  o << "const FAR_LEVELS : u32 = " << kFarLevels << "u;\n";
  o << "const FAR_N : u32 = " << kFarN << "u;\n";
  o << "const FAR_NCHUNK : u32 = " << kFarNChunk << "u;\n";
  o << "const FAR_NUM_CHUNKS : u32 = " << kFarNumChunks << "u;\n";
  // Fill-queue packing: (level-1) << FAR_SLOT_SHIFT | chunk slot.
  o << "const FAR_SLOT_SHIFT : u32 = " << kFarSlotShift << "u;\n";
  o << "const FAR_SLOT_MASK : u32 = " << kFarSlotMask << "u;\n";
  o << "const FAR_VOX : u32 = " << kFarVox << "u;\n";
  o << "const FAR_MASK : i32 = " << (kFarN - 1) << ";\n";
  o << "const FAR_NCHUNK_MASK : i32 = " << (kFarNChunk - 1) << ";\n";
  o << "const FAR_SHIFT_BASE : u32 = " << kFarShiftBase << "u;\n";
  // Cascade edit patches (world.h's kFarPatch* block): the farPatch buffer is
  // a (offset, count) header indexed by dispatch entry, then a payload of
  // (mat << 12) | cellIndex words starting here.
  o << "const FAR_PATCH_BASE : u32 = " << kFarPatchBase << "u;\n";
  // Fluid-lab flat-slab ground height (world.h kLabSlabY): the y the lab
  // worldgen mode fills up to, and the value World::TerrainHeight returns in
  // lab mode. Emitted here so the shader guard and the CPU mirror share one
  // number (docs/PLAN_fluid_overhaul.md §4).
  o << "const LAB_SLAB_Y : i32 = " << kLabSlabY << ";\n";
  // Render-only: the sim never reads it, so voxel state stays integer and
  // scale-free. Emitted at full precision so it round-trips the f32 exactly.
  o.precision(9);
  o << "const VOXEL_METERS : f32 = " << kVoxelMeters << ";\n";
  // The same number as an INTEGER reciprocal, for the sim/worldgen side. It has
  // to be integer and it has to come from here: everything worldgen authors in
  // metres (the whole tree and cactus size table) converts through it, and the
  // hand-written duplicate this replaces had drifted to 16 against a world
  // running at 10 — every tree 1.6x its documented size, with nothing able to
  // notice. See world.h kVoxelsPerMetre.
  o << "const VOXELS_PER_M : i32 = " << kVoxelsPerMetre << ";\n";
  // The residency window's half-extent in metres (world.h's
  // kWindowHalfExtentMeters). The in-window LOD handoff clamps against this:
  // beyond it there are no fine voxels to march anyway, so a handoff distance
  // past the window is the old "switch only at window exit" behaviour.
  o << "const WINDOW_HALF_EXTENT_METERS : f32 = " << kWindowHalfExtentMeters
    << ";\n";
  // Fine voxels per level-1 cascade cell (world.h's kFarCellVox(1)). This is
  // what the handoff actually trades away: at the switch distance a 1-voxel
  // cell becomes a FAR_CELL1_VOX-voxel one, so it is the honest measure of the
  // quantisation the LOD introduces.
  o << "const FAR_CELL1_VOX : f32 = " << (float)kFarCellVox(1) << ";\n";
  return o.str();
}

// The page-table accessor block in common.wgsl references `voxels`,
// `pageTable` and `pageFaults`, which only the shaders that address voxels
// declare. WGSL resolves module-scope references whether or not the function is
// reachable, so leaving the block in a shader that has no voxel bindings is a
// compile error — hence the strip.
//
// Delimited rather than conditionally generated so common.wgsl stays the ONE
// place the translation is written; this is a filter, not a second copy. The
// predicate is "does the body declare `voxels`", read off the body itself
// rather than kept as a list here, so adding a shader cannot desync a list.
// scripts/check_shaders.sh does the same strip, and preserves the line count so
// its error-line remapping stays exact.
// Two blocks, because the two capabilities have different prerequisites:
// the READ half needs `voxels` + `pageTable`, the WRITE half additionally
// needs `voxels` to be read_write and needs `pageFaults`. raymarch.wgsl has
// the first and not the second, which is exactly the access the render path
// should have.
constexpr const char* kPageBlockBegin = ">>>PAGE_TABLE_BEGIN<<<";
constexpr const char* kPageBlockEnd = ">>>PAGE_TABLE_END<<<";
constexpr const char* kPageWriteBegin = ">>>PAGE_TABLE_WRITE_BEGIN<<<";
constexpr const char* kPageWriteEnd = ">>>PAGE_TABLE_WRITE_END<<<";

// The support-loss block in common.wgsl references `supportOut` and
// `materials`, which only the three kernels that can REMOVE a voxel declare
// (sim_step, sim_mutate, sim_explode). Same filter as the page block above and
// for the same WGSL reason: an unreachable function's identifiers still have to
// resolve, so leaving it in raymarch.wgsl is a compile error.
//
// The predicate is read off the body — "does it declare supportOut" — rather
// than kept as a list here, so wiring a fourth removal path up cannot desync a
// list it forgot to edit.
constexpr const char* kSupportBlockBegin = ">>>SUPPORT_LOSS_BEGIN<<<";
constexpr const char* kSupportBlockEnd = ">>>SUPPORT_LOSS_END<<<";

// The WIND DRAFTS reader (common.wgsl's block of that name) reads
// `draftField`, which only the shaders that consume the shelter volume declare
// (sim_step, sim_particle, sim_fluid, sim_draft, wind_streak, debug_wind). Two
// blocks, exactly one kept: BOUND (the real accessor) for a body that declares
// it, UNBOUND (a constant-false gate and a stub) for every other shader, which
// still compiles windAt/windAtQ. Same body-derived predicate as the page block.
constexpr const char* kDraftBoundBegin = ">>>DRAFT_BOUND_BEGIN<<<";
constexpr const char* kDraftBoundEnd = ">>>DRAFT_BOUND_END<<<";
constexpr const char* kDraftUnboundBegin = ">>>DRAFT_UNBOUND_BEGIN<<<";
constexpr const char* kDraftUnboundEnd = ">>>DRAFT_UNBOUND_END<<<";

bool BodyReadsDrafts(const std::string& body) {
  return body.find("> draftField") != std::string::npos;
}

bool BodyFlagsSupportLoss(const std::string& body) {
  return body.find("> supportOut") != std::string::npos;
}

bool BodyAddressesVoxels(const std::string& body) {
  return body.find("> voxels") != std::string::npos;
}

// The world seed, for the JITTER sentinel's per-cell palette variant
// (common.wgsl synthWordAt). common.wgsl is prepended BEFORE the shader body,
// and the uniform carrying the seed is `T : TickParams` in a sim kernel but
// `R : RenderParams` in the render/pick path — one name does not serve both.
// So the accessor is GENERATED per shader from whichever uniform the body
// declares, which is what keeps all 46 voxWordAt call sites untouched.
//
// Emitted only for shaders that address voxels; the others have the whole page
// block stripped and would not compile a reference to either uniform. A shader
// that addresses voxels and declares NEITHER uniform is a build-time error
// rather than a silent wrong seed — a wrong seed here is a synthesized word
// that differs from the materialized page, i.e. a lost voxel.
// ptOrigin() rides along for the same reason: the chunk-linear read
// (voxWordInChunkAt) holds a SLOT index, and recovering the world position a
// JITTER variant keys on needs the toroidal window origin. Both uniforms carry
// `origin` in CHUNK units under the same field name.
std::string PtSeedAccessor(const std::string& body) {
  const bool hasT = body.find("uniform> T :") != std::string::npos;
  const bool hasR = body.find("uniform> R :") != std::string::npos;
  const char* u = hasT ? "T" : (hasR ? "R" : nullptr);
  if (!u) return "";
  // ptTick() rides along for the page-FAULT record (common.wgsl's voxStore):
  // "which chunk lost a write" is only half a diagnosis without "on which
  // tick", and the tick is what lines a fault up against the CPU-side free /
  // shift / deferred-wake log. Both uniforms carry `tick` under the same field
  // name, exactly like `seed` and `origin` above.
  //
  // ADDING ONE HERE MEANS ADDING ONE TO scripts/check_shaders.sh TOO — the
  // checker reproduces this accessor itself (and counts its lines to remap
  // diagnostics), so a one-sided addition fails every shader in the tree.
  return std::string("fn ptSeed() -> u32 { return ") + u + ".seed; }\n" +
         "fn ptOrigin() -> vec3<i32> { return " + u + ".origin; }\n" +
         "fn ptTick() -> u32 { return " + u + ".tick; }\n";
}
bool BodyWritesVoxels(const std::string& body) {
  return body.find("read_write> voxels") != std::string::npos;
}

// Replace the block's contents with blank lines, so every shader sees
// common.wgsl at the same length and a diagnostic's line number still maps.
std::string StripBlock(const std::string& common, const char* beginTag,
                       const char* endTag) {
  size_t b = common.find(beginTag);
  size_t e = common.find(endTag);
  if (b == std::string::npos || e == std::string::npos || e < b) return common;
  // Cut from the newline after the BEGIN marker's line to the start of the
  // END marker's line, so both marker comments survive intact.
  b = common.find('\n', b);
  if (b == std::string::npos) return common;
  size_t eLine = common.rfind('\n', e);
  if (eLine == std::string::npos || eLine < b) return common;
  size_t lines = (size_t)std::count(common.begin() + (long)b + 1,
                                    common.begin() + (long)eLine + 1, '\n');
  return common.substr(0, b + 1) + std::string(lines, '\n') +
         common.substr(eLine + 1);
}

// Keep only the `const NAME : ...` lines of the tuning block whose NAME occurs
// as a whole identifier somewhere in `text` -- the shader body plus whatever of
// common.wgsl survived the block stripping above it. Comment lines stay so the
// block still says where it came from.
//
// WHY: TuningWgslBlock emits every row of tuning_params.def (351 consts), and
// the assembled source is the shader cache key (rhi_vulkan.cpp GetShaderModule
// hashes it; shader_cache/ and the in-process module cache are both keyed on
// it). Pasting all of them made EVERY shader's text change whenever ANY knob
// moved, so a selftest gate that flips render.giStrength or a sim.fluid* row
// and calls ReloadShaders re-keyed worldgen.wgsl -- which reads none of those
// -- and paid a cold `far` compile (~10 min since P-F) per flip: five of them
// in one 85-minute --selftest on 2026-09-07, each followed 0.3 s later by the
// restore arm's cache hit. An unreferenced `const` never reaches SPIR-V, so
// dropping it changes nothing the driver sees; the key just stops lying about
// what the pipeline is compiled from.
std::string ReferencedTuningBlock(const std::string& block, const std::string& text) {
  std::unordered_set<std::string> idents;
  {
    const size_t n = text.size();
    auto isStart = [](char c) { return std::isalpha((unsigned char)c) || c == '_'; };
    auto isBody = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
    size_t i = 0;
    while (i < n) {
      if (isStart(text[i])) {
        size_t j = i + 1;
        while (j < n && isBody(text[j])) j++;
        idents.emplace(text.substr(i, j - i));
        i = j;
      } else {
        i++;
      }
    }
  }
  std::string out;
  out.reserve(block.size());
  size_t pos = 0;
  while (pos < block.size()) {
    size_t eol = block.find('\n', pos);
    if (eol == std::string::npos) eol = block.size();
    std::string_view line(block.data() + pos, eol - pos);
    bool keep = true;
    if (line.rfind("const ", 0) == 0) {
      const size_t nameEnd = line.find(' ', 6);
      const std::string name(line.substr(6, nameEnd == std::string_view::npos
                                                ? std::string_view::npos
                                                : nameEnd - 6));
      keep = idents.count(name) != 0;
    }
    if (keep) {
      out.append(line.data(), line.size());
      out.push_back('\n');
    }
    pos = eol + 1;
  }
  return out;
}

// THE WORLDGEN-ONLY PRELUDE: load-time ASSET DATA that only worldgen.wgsl
// reads, emitted for that one shader so an edit to a biome's tree tile, a
// water tile or the map's reference scale changes one shader's source (one
// SPIR-V cache miss) instead of every shader's. They are constants rather
// than buffer words because worldgen divides by them in ~1000 inlined places
// and sizes an array by TREE_CAND_MAX; a runtime divisor there took the
// driver's compile from minutes to never. Simulation::Init / UploadEnvironment
// set the inputs before the first LoadShader and recompile when a reload
// moves them. scripts/check_shaders.sh appends the same block to worldgen.wgsl
// only, derived from the assets by tree_lattice.py / pond_lattice.py /
// map_terrain.py.
static std::string WorldgenPrelude() {
  std::ostringstream o;
  // The tree lattice (sim/treeatlas.h TreeLattice): the finest authored biome
  // tile, and the scan / candidate cap derived from it and the atlas's widest
  // reach.
  const treeatlas::TreeLattice& l = treeatlas::CurrentTreeLattice();
  o << "const TREE_TILE : i32 = " << l.tile << ";\n";
  o << "const TREE_SCAN : i32 = " << l.scan << ";\n";
  o << "const TREE_CAND_MAX : i32 = " << l.candMax << ";\n";
  // The pond lattice (worldmap.h kHPondTile): the finest live water tile of
  // any biome.
  o << "const POND_TILE : i32 = " << worldmap::CurrentWorldMap().pondTile << ";\n";
  // The scale the map's terrain is authored at (map.json
  // terrain.refVoxelsPerMetre): vlen() divides the kernel's hardcoded lengths
  // by it in module-scope consts.
  o << "const REF_VOXELS_PER_METRE : i32 = " << worldmap::CurrentTerrain().refVoxelsPerMetre << ";\n";
  return o.str();
}

bool AssembleShaderSource(const std::string& shaderDir, const std::string& name,
                          std::string& out) {
  std::string common, body;
  if (!ReadCommonCached(shaderDir + "/common.wgsl", common)) {
    std::fprintf(stderr, "cannot read %s/common.wgsl\n", shaderDir.c_str());
    return false;
  }
  if (!ReadFileText(shaderDir + "/" + name, body)) {
    std::fprintf(stderr, "cannot read %s/%s\n", shaderDir.c_str(), name.c_str());
    return false;
  }
  // The seed accessor the page block's JITTER synthesis calls. Empty unless
  // this shader addresses voxels, which is exactly when the block survives.
  std::string ptSeed;
  if (!BodyFlagsSupportLoss(body)) {
    common = StripBlock(common, kSupportBlockBegin, kSupportBlockEnd);
  }
  if (BodyReadsDrafts(body)) {
    common = StripBlock(common, kDraftUnboundBegin, kDraftUnboundEnd);
  } else {
    common = StripBlock(common, kDraftBoundBegin, kDraftBoundEnd);
  }
  if (!BodyAddressesVoxels(body)) {
    common = StripBlock(common, kPageBlockBegin, kPageBlockEnd);
    common = StripBlock(common, kPageWriteBegin, kPageWriteEnd);
  } else {
    if (!BodyWritesVoxels(body)) {
      common = StripBlock(common, kPageWriteBegin, kPageWriteEnd);
    }
    ptSeed = PtSeedAccessor(body);
    if (ptSeed.empty()) {
      std::fprintf(stderr,
                   "%s addresses voxels but declares neither `T : TickParams` "
                   "nor `R : RenderParams`, so the page table cannot resolve a "
                   "world seed for JITTER synthesis. Add one, or the sentinel "
                   "reads would differ from the page they synthesize.\n",
                   name.c_str());
      return false;
    }
  }
  // Tuning constants sit between the world prelude and common.wgsl: they may
  // reference nothing, but common.wgsl and every shader body may reference
  // them. Re-read from the live Tuning on every load, which is what makes F5
  // (ReloadShaders) pick up an edited tuning.json without a rebuild.
  // ptSeed sits BEFORE common.wgsl: the page block calls it. WGSL module scope
  // is order-independent, but keeping the definition ahead of its use matches
  // how every other generated declaration here reads.
  // Only the tuning consts this shader can see (ReferencedTuningBlock above):
  // the rest would only make the cache key move when they do.
  const std::string tuningBlock =
      ReferencedTuningBlock(TuningWgslBlock(CurrentTuning()), ptSeed + common + body);
  const std::string wgPrelude = name == "worldgen.wgsl" ? WorldgenPrelude() : std::string();
  out = ShaderConstantPrelude() + "\n" + tuningBlock + "\n" + ptSeed + wgPrelude + common +
        "\n" + body;
  return true;
}

rhi::ShaderModule LoadShader(const rhi::Device& device, const std::string& shaderDir,
                             const std::string& name) {
  std::string src;
  if (!AssembleShaderSource(shaderDir, name, src)) return {};
  return device.CreateShaderModule(src, name.c_str());
}

// ---- pipeline compile times ------------------------------------------------
namespace {
std::mutex& PipeTimingMutex() {
  static std::mutex m;
  return m;
}
std::vector<PipelineCompileRecord>& PipeTimingRecords() {
  static std::vector<PipelineCompileRecord> v;
  return v;
}
// The zero of both milestone clocks. Taken on the first call rather than at
// static-init so it is the first pipeline create, not whatever the loader
// decided to run first.
std::chrono::steady_clock::time_point& PipeTimingEpoch() {
  static std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  return t0;
}
double gInteractiveReadyMs = -1;
double gFarReadyMs = -1;
}  // namespace

void RecordPipelineCompile(const char* label, const char* entry, double ms) {
  std::lock_guard<std::mutex> lock(PipeTimingMutex());
  PipeTimingEpoch();  // start the clock on the first create
  PipeTimingRecords().push_back({label ? label : "?", entry ? entry : "?", ms});
}

std::vector<PipelineCompileRecord> PipelineCompileRecords() {
  std::lock_guard<std::mutex> lock(PipeTimingMutex());
  return PipeTimingRecords();
}

void MarkInteractiveReady() {
  std::lock_guard<std::mutex> lock(PipeTimingMutex());
  gInteractiveReadyMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - PipeTimingEpoch())
                            .count();
}

void MarkFarReady() {
  std::lock_guard<std::mutex> lock(PipeTimingMutex());
  gFarReadyMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - PipeTimingEpoch())
                    .count();
}

double InteractiveReadyMs() {
  std::lock_guard<std::mutex> lock(PipeTimingMutex());
  return gInteractiveReadyMs;
}

double FarReadyMs() {
  std::lock_guard<std::mutex> lock(PipeTimingMutex());
  return gFarReadyMs;
}

std::string PipelineTimingJson(const char* indent) {
  const std::string ind = indent ? indent : "  ";
  std::vector<PipelineCompileRecord> recs = PipelineCompileRecords();
  std::string out = ind + "\"pipelineCompileMs\": {";
  // LAST value wins per key. F5 and --sweep call BuildPipelines again, and a
  // JSON object with the same key twice is a document whose meaning depends on
  // the parser. The rebuild's number is the interesting one anyway.
  std::vector<std::string> order;
  std::unordered_map<std::string, double> byKey;
  for (const PipelineCompileRecord& r : recs) {
    // label::entry, because worldgen alone contributes five rows and the label
    // is what pass_table.def names but the entry is what the driver compiled.
    const std::string key = r.label + "::" + r.entry;
    if (!byKey.count(key)) order.push_back(key);
    byKey[key] = r.ms;
  }
  for (size_t i = 0; i < order.size(); i++) {
    char num[32];
    std::snprintf(num, sizeof(num), "%.1f", byKey[order[i]]);
    out += std::string(i ? ", " : "") + "\"" + order[i] + "\": " + num;
  }
  out += "},\n";
  char a[64], b[64];
  std::snprintf(a, sizeof(a), "%.1f", InteractiveReadyMs());
  std::snprintf(b, sizeof(b), "%.1f", FarReadyMs());
  out += ind + "\"interactiveReadyMs\": " + a + ",\n";
  out += ind + "\"farReadyMs\": " + b;
  return out;
}

rhi::ComputePipeline MakeComputePipeline(const rhi::Device& device,
                                         const rhi::PipelineLayout& layout,
                                         const rhi::ShaderModule& module,
                                         const char* entry, const char* label) {
  // The driver's compile time is ALWAYS measured and recorded (it lands in
  // build/last_run.json via PipelineTimingJson). SANDVOX_SHADER_TIMING=1 only
  // decides whether it is also PRINTED, one line per pipeline, so a cold `sim
  // init` that takes minutes names the entry point that took them instead of
  // being one number (CLAUDE.md verification rule 6). worldgen.wgsl alone is
  // FIVE entry points (main, list, pagefill, far, fardown), each a full
  // compile of the kernel; that is where the minutes go.
  static const bool timing = std::getenv("SANDVOX_SHADER_TIMING") != nullptr;
  const auto t0 = std::chrono::steady_clock::now();
  rhi::ComputePipeline p = device.CreateComputePipeline(layout, module, entry, label);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  RecordPipelineCompile(label, entry, ms);
  if (timing) {
    // ONE printf per pipeline under one lock: the build pool means several
    // threads finish inside microseconds of each other, and an interleaved
    // line is the one piece of output this mode exists to produce.
    static std::mutex printMutex;
    std::lock_guard<std::mutex> lock(printMutex);
    std::printf("pipeline %-16s %-10s %9.1f ms\n", label, entry, ms);
    std::fflush(stdout);
  }
  return p;
}
