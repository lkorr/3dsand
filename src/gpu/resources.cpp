#include "gpu/resources.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#include "sim/tuning.h"
#include "sim/world.h"

wgpu::Buffer CreateBuffer(const wgpu::Device& device, uint64_t size,
                          wgpu::BufferUsage usage, const char* label) {
  wgpu::BufferDescriptor d{};
  d.size = size;
  d.usage = usage;
  d.label = label;
  return device.CreateBuffer(&d);
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
  // Stain palette: reserved material-table entries holding stain-type colours
  // (world.h). The renderer indexes materials[STAIN_PALETTE_BASE + type].
  o << "const STAIN_PALETTE_BASE : u32 = " << kStainPaletteBase << "u;\n";
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

wgpu::ShaderModule LoadShader(const wgpu::Device& device, const std::string& shaderDir,
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
  // Tuning constants sit between the world prelude and common.wgsl: they may
  // reference nothing, but common.wgsl and every shader body may reference
  // them. Re-read from the live Tuning on every load, which is what makes F5
  // (ReloadShaders) pick up an edited tuning.json without a rebuild.
  std::string src = ShaderConstantPrelude() + "\n" +
                    TuningWgslBlock(CurrentTuning()) + "\n" + common + "\n" +
                    body;

  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = src.c_str();
  wgpu::ShaderModuleDescriptor d{};
  d.nextInChain = &wgsl;
  d.label = name.c_str();
  return device.CreateShaderModule(&d);
}

wgpu::ComputePipeline MakeComputePipeline(const wgpu::Device& device,
                                          const wgpu::PipelineLayout& layout,
                                          const wgpu::ShaderModule& module,
                                          const char* entry, const char* label) {
  wgpu::ComputePipelineDescriptor d{};
  d.layout = layout;
  d.compute.module = module;
  d.compute.entryPoint = entry;
  d.label = label;
  return device.CreateComputePipeline(&d);
}
