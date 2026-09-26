// voxregion.h — REAL VOXELS FOR THE TUNER'S TERRAIN VIEWER.
//
// WHY THIS EXISTS. `--heightmap` (main.cpp) answers "how high is the ground at
// (x,z)" for a grid of columns, and the Worldgen tab drew that. A column map
// cannot show a cave, an overhang, a tree, a pond's floor, the sediment wedge
// as anything but a number, or a single voxel — because none of those are
// functions of (x,z). The world's actual content is `genCellIn` in
// worldgen.wgsl, and genCellIn runs on the GPU.
//
// So this module boots the engine's real GPU worldgen and reads a box of it
// back. There is no second copy of the terrain here: the box is filled by the
// SAME genChunk dispatch the game fills its residency window with, through the
// SAME EncodeGenList primitive the streamer uses. What the viewer draws is
// what you spawn into, per voxel, including everything a column cannot express.
//
// WHY IT IS A SERVER AND NOT A FLAG. Device creation + SPIR-V compile + asset
// load is ~2-4 s. A viewer that streams regions as you fly needs tens of boxes
// per session, and paying the boot cost per box makes it a slideshow. So
// `--voxserve` boots ONCE and answers requests on stdin until told to quit;
// `--voxdump` is exactly one request through the same code path, kept because
// a mode you cannot invoke by hand is a mode you cannot debug.
//
// THE COST MODEL, which is the whole reason regions are the unit. Generating
// the full 512^3 window is 32,768 chunk dispatches and a 512 MiB page-pool
// dance. A viewer near the camera wants a 64^3 box — 64 chunks. So a request
// generates ONLY the chunks its box covers (EncodeGenList takes a slot list;
// the primitive was already there for the streamer) and reads only those back.
// Cost tracks the box, not the window, which is CLAUDE.md rule 2 applied to a
// tool.
//
// LOD. A request carries `lod` = world voxels per emitted sample. lod 1 is the
// truth. lod > 1 downsamples per CHUNK — 16 divides every supported lod, so a
// chunk's cells never straddle a sample and no cross-chunk state is needed —
// by MAJORITY over the non-air cells in the block. Majority and not
// point-sampling because a point sample deletes exactly the features you zoom
// out to see: a one-voxel-thick cave roof, a tree trunk, a shoreline. Air wins
// only if the block is entirely air.
//
// The output never holds the box at full resolution. A lod-8 64^3 region spans
// 512^3 voxels = 512 MiB of words; the sample grid it produces is 1 MiB. Chunks
// are downsampled as they are read, so peak memory is one readback run.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gpu/context.h"
#include "sim/materials.h"
#include "sim/simulation.h"
#include "sim/world.h"

namespace sandvox {

// ---- the 'SVVX' region binary --------------------------------------------
//
// Little-endian, which every platform this runs on is. Keep this table and
// wgvxDecode() in assets/worldview.js together — they are the two halves of
// one format and there is nothing that would catch a drift.
//
//   0  u32  magic 'SVVX'          32 u32  lod (world voxels per sample)
//   4  u32  version (1)           36 u32  seed
//   8  i32  ox  (world voxel      40 i32  voxelsPerMetre
//   12 i32  oy   coord of the     44 u32  flags (0)
//   16 i32  oz   box min)         48 u32  runCount
//   20 u32  nx                    52 u32  solidCount (non-air samples)
//   24 u32  ny                    56 i32  yMinSolid  (sample y, -1 if none)
//   28 u32  nz                    60 i32  yMaxSolid
//   64 .. runCount * 8: { u32 word, u32 count }
//
// The payload is RLE over the sample grid in x-fastest, then y, then z order.
// RLE and not raw because worldgen output is overwhelmingly runny — a 64^3
// region of open sky is two runs — and because the wire is a local HTTP hop
// that a browser has to decode on the main thread before it can mesh.
//
// The WORD is the engine's full voxel word (world.h layout): material in bits
// 0..11, state nibble in 12..15, stain in 24..30. The state nibble is what
// makes a liquid's fullness and a solid's palette variant survive to the
// viewer, so water reads as water and a stone wall is not one flat grey.
constexpr uint32_t kVoxRegionMagic = 0x58565653u;  // 'SVVX'
constexpr uint32_t kVoxRegionVersion = 1;
constexpr uint32_t kVoxRegionHeaderBytes = 64;

// Caps. Reachable from a browser, so every one of them is a refusal and not a
// clamp-and-surprise: a viewer that silently got a smaller box than it asked
// for would place it wrong and blame the terrain.
constexpr uint32_t kVoxRegionMaxSamples = 128;  // per axis
constexpr uint32_t kVoxRegionMaxLod = 16;       // must divide kChunk

struct VoxRegionReq {
  int32_t ox = 0, oy = 0, oz = 0;  // world voxel coord of the box min
  uint32_t nx = 64, ny = 64, nz = 64;
  uint32_t lod = 1;
  uint32_t seed = 1337;
  // Patch the generated words with the process's edit layer (WorldEditLayer,
  // map-overhaul P7) — what the game does to every chunk it generates. Off
  // only for the biome swatch, whose synthetic map has no layer.
  bool applyEdits = true;
};

// Generates the region and writes the 'SVVX' binary into `out`. False + `err`
// on a bad request or a GPU failure; never partially fills `out`.
bool BuildVoxRegion(GpuContext& ctx, World& world, Simulation& sim,
                    const VoxRegionReq& req, std::vector<uint8_t>& out,
                    std::string& err);

// The material table the viewer colours with, as JSON.
//
// EMITTED FROM THE COMPILED TABLE, not from materials.json. The browser must
// never re-derive a material id by counting entries in the authored file: air
// is synthesised at index 0 by LoadAssets and is not in the file at all, so
// counting would offset every id by one and paint the whole world in the wrong
// material. This is the id the voxel words actually carry.
std::string VoxPaletteJson(const std::vector<MaterialDef>& mats);

// `--voxdump cx,cy,cz,nx,ny,nz,lod[,seed]` — one region to a file, then exit.
int RunVoxDump(GpuContext& ctx, World& world, Simulation& sim,
               const std::vector<MaterialDef>& mats, const std::string& spec,
               const std::string& outPath);

// `--voxserve` — the request loop the tuner drives.
//
// LINE PROTOCOL ON STDIN, ONE ACK LINE ON STDOUT, PAYLOAD TO A FILE. Framing a
// binary payload on the same stdout the engine prints its boot log to is a
// trap: every "loaded 96 materials" would land inside a region. Files also
// give the tuner its cache for free — a re-request of a box the camera already
// visited never reaches this process.
//
//   REGION <ox> <oy> <oz> <nx> <ny> <nz> <lod> <seed> <outPath>
//     -> "OK <bytes> <ms>"  or  "ERR <message>"
//   PALETTE <outPath>          -> "OK <bytes> 0"
//   RELOAD                     -> "OK 0 <ms>"   re-reads tuning.json
//   PING                       -> "OK 0 0"
//   QUIT                       -> exits 0
//
// Map-overhaul P7 (the edit layer as worldgen, and the engine's biome swatch).
// Commands that name two paths separate them with '|' (a path may contain
// spaces, never a pipe):
//
//   LAYER <@map | - | name>    which edit layer REGION applies: the map's
//                              (map.json editLayer, the default), none, or
//                              assets/worldedits/<name>.svedit. Sticky; the
//                              file is re-read whenever it changes on disk.
//   EDITS RESOLVE <seed> <in>|<out>   any .svedit -> v1 ABSOLUTE at <seed>
//                                     (what the viewer edits)
//   EDITS RELATIVE <seed> <in>|<out>  any .svedit -> v2 GROUND-RELATIVE, the
//                                     offsets taken against <seed>'s ground
//   EXPORT <seed> <saveDir>|<out>     a save's stored chunks, diffed against
//                                     the generator -> a v2 .svedit
//   SWATCH <biome> <seed> <sizeVox> <biomeJson>|<out>
//                              the biome as the ENGINE makes it: a synthetic
//                              one-biome map, sizeVox square, as an 'SVSW'
//                              bundle of SVVX regions (BuildBiomeSwatch)
//
// RELOAD is what makes a slider move the world: worldgen reads tuning through
// CurrentTuning(), so a live server would otherwise answer forever with the
// parameters it booted on. The tuner sends it after every save.
int RunVoxServe(GpuContext& ctx, World& world, Simulation& sim,
                const std::vector<MaterialDef>& mats);

// ---- the edit-layer export (map-overhaul P7) ------------------------------
// Every chunk a save stores (ChunkStore at `saveDir`), diffed against what
// genChunk makes of it at `seed`, written as a ground-relative .svedit to
// `outPath`. Stamp/excite scratch bits are ignored and GAS cells are skipped
// (smoke drifting through is not an edit). `report` says what was found.
// Needs the GPU (genChunk is WGSL); the window and page table are clobbered.
bool ExportSaveEdits(GpuContext& ctx, World& world, Simulation& sim,
                     const std::vector<MaterialDef>& mats, const std::string& saveDir,
                     uint32_t seed, const std::string& outPath, std::string& report);
// `--export-edits <saveDir>,<layerName>[,seed]`: ExportSaveEdits into
// assets/worldedits/<layerName>.svedit, then exit. The in-game half is F9 (save).
int RunExportEdits(GpuContext& ctx, World& world, Simulation& sim,
                   const std::vector<MaterialDef>& mats, const std::string& spec);

// ---- the biome swatch (map-overhaul P7) ---------------------------------
// A square of ONE biome as the engine generates it: the loaded map with every
// cell painted `biomeName` (flat landform, mid moisture, no sites — the
// synthetic map the env-truth gate measures), the biome file optionally read
// from `biomeJson` (the page's unsaved copy), `sizeVox` wide around the real
// map's spawn. Written as:
//
//   0 'SVSW'  4 u32 version (1)  8 u32 regionCount  12 i32 x0  16 i32 y0
//   20 i32 z0  24 u32 sizeVox  28 u32 lod, then per region: u32 bytes, SVVX blob
//
// The real environment is restored afterwards (ReloadEnvironment).
bool BuildBiomeSwatch(GpuContext& ctx, World& world, Simulation& sim,
                      const std::vector<MaterialDef>& mats, const std::string& biomeName,
                      const std::string& biomeJson, uint32_t seed, uint32_t sizeVox,
                      std::vector<uint8_t>& out, std::string& err);
constexpr uint32_t kBiomeSwatchMagic = 0x57535653u;  // 'SVSW'

}  // namespace sandvox
