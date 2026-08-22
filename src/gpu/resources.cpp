#include "gpu/resources.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "sim/tuning.h"
#include "sim/voxload.h"  // kArtPaletteBase, mirrored into the WGSL prelude
#include "sim/world.h"

rhi::Buffer CreateBuffer(const rhi::Device& device, uint64_t size,
                         rhi::BufferUsage usage, const char* label) {
  return device.CreateBuffer(size, usage, label);
}

static bool ReadFileText(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

// World constants, emitted as WGSL from the C++ definitions in world.h so the
// two can never disagree. Previously common.wgsl redeclared these by hand and a
// mismatch was silent: kVoxelMeters drifting from VOXEL_METERS just meant the
// renderer lit the world at a different physical scale than the one the player
// walked in. Anything derived from world.h belongs here, not in common.wgsl.
std::string ShaderConstantPrelude() {
  std::ostringstream o;
  o << "// GENERATED from src/sim/world.h by ShaderConstantPrelude() — do not\n"
       "// edit, and do not redeclare these in common.wgsl.\n";
  o << "const WORLD_N : u32 = " << kWorldN << "u;\n";
  o << "const CHUNK : u32 = " << kChunk << "u;\n";
  o << "const NCHUNK : u32 = " << kNChunk << "u;\n";
  o << "const NUM_CHUNKS : u32 = " << kNumChunks << "u;\n";
  o << "const CHUNK_VOL : u32 = " << kChunkVol << "u;\n";
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
  o << "const ART_PALETTE_BASE : u32 = " << kArtPaletteBaseGpu << "u;\n";
  // Lowest .vox palette index that means "art colour" rather than material id
  // (kArtPaletteBase, sim/voxload.h). Slot s maps to ART_PALETTE_BASE + s -
  // ART_SLOT_MIN.
  o << "const ART_SLOT_MIN : u32 = " << kArtPaletteBase << "u;\n";
  // Static micro-detail (render-only, DESIGN.md §9): the size of the brick pool
  // the raymarcher bounds-checks its nested DDA fetches against.
  o << "const MICRO_POOL_WORDS : u32 = " << kMicroPoolWordsWorld << "u;\n";
  // Dynamic microvoxel bodies (render-only, DESIGN.md §9): the pool the
  // microbody fragment march bounds-checks its brick fetches against.
  o << "const MICRO_BODY_POOL_WORDS : u32 = " << kMicroBodyPoolWordsWorld << "u;\n";
  o << "const MATERIAL_SLOTS : u32 = " << kMaterialSlots << "u;\n";
  // Far-field cascades (render-only LOD, DESIGN.md §9). The far field lives on
  // its own kFarN^3 grid, decoupled from the window; level k (1-based) cells
  // span 2^(k + FAR_SHIFT_BASE) fine voxels (see world.h).
  o << "const FAR_LEVELS : u32 = " << kFarLevels << "u;\n";
  o << "const FAR_N : u32 = " << kFarN << "u;\n";
  o << "const FAR_NCHUNK : u32 = " << kFarNChunk << "u;\n";
  o << "const FAR_NUM_CHUNKS : u32 = " << kFarNumChunks << "u;\n";
  o << "const FAR_VOX : u32 = " << kFarVox << "u;\n";
  o << "const FAR_MASK : i32 = " << (kFarN - 1) << ";\n";
  o << "const FAR_NCHUNK_MASK : i32 = " << (kFarNChunk - 1) << ";\n";
  o << "const FAR_SHIFT_BASE : u32 = " << kFarShiftBase << "u;\n";
  // Render-only: the sim never reads it, so voxel state stays integer and
  // scale-free. Emitted at full precision so it round-trips the f32 exactly.
  o.precision(9);
  o << "const VOXEL_METERS : f32 = " << kVoxelMeters << ";\n";
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

bool BodyAddressesVoxels(const std::string& body) {
  return body.find("> voxels") != std::string::npos;
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

rhi::ShaderModule LoadShader(const rhi::Device& device, const std::string& shaderDir,
                             const std::string& name) {
  std::string common, body;
  if (!ReadFileText(shaderDir + "/common.wgsl", common)) {
    std::fprintf(stderr, "cannot read %s/common.wgsl\n", shaderDir.c_str());
    return {};
  }
  if (!ReadFileText(shaderDir + "/" + name, body)) {
    std::fprintf(stderr, "cannot read %s/%s\n", shaderDir.c_str(), name.c_str());
    return {};
  }
  if (!BodyAddressesVoxels(body)) {
    common = StripBlock(common, kPageBlockBegin, kPageBlockEnd);
    common = StripBlock(common, kPageWriteBegin, kPageWriteEnd);
  } else if (!BodyWritesVoxels(body)) {
    common = StripBlock(common, kPageWriteBegin, kPageWriteEnd);
  }
  // Tuning constants sit between the world prelude and common.wgsl: they may
  // reference nothing, but common.wgsl and every shader body may reference
  // them. Re-read from the live Tuning on every load, which is what makes F5
  // (ReloadShaders) pick up an edited tuning.json without a rebuild.
  std::string src = ShaderConstantPrelude() + "\n" +
                    TuningWgslBlock(CurrentTuning()) + "\n" + common + "\n" +
                    body;

  return device.CreateShaderModule(src, name.c_str());
}

rhi::ComputePipeline MakeComputePipeline(const rhi::Device& device,
                                         const rhi::PipelineLayout& layout,
                                         const rhi::ShaderModule& module,
                                         const char* entry, const char* label) {
  return device.CreateComputePipeline(layout, module, entry, label);
}
