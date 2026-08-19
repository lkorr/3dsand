#pragma once
#include <cstdint>
#include <functional>
#include <vector>

#include <webgpu/webgpu_cpp.h>

#include "math3d.h"

// World constants — must match common.wgsl.
constexpr uint32_t kWorldN = 256;
constexpr uint32_t kChunk = 16;
constexpr uint32_t kNChunk = kWorldN / kChunk;          // 16
constexpr uint32_t kNumChunks = kNChunk * kNChunk * kNChunk;  // 4096
constexpr uint32_t kChunkVol = kChunk * kChunk * kChunk;      // 4096
constexpr uint64_t kVoxelCount = (uint64_t)kWorldN * kWorldN * kWorldN;

// Worldgen material IDs (fixed by materials.json order).
constexpr uint32_t kMatAir = 0, kMatStone = 1, kMatWood = 2, kMatSand = 3,
                   kMatGravel = 4, kMatWater = 5, kMatOil = 6, kMatSmoke = 7,
                   kMatSteam = 8;

// Must match BrushOp in common.wgsl (32 bytes).
struct BrushOp {
  int32_t x, y, z;
  int32_t radius;
  uint32_t material;
  uint32_t mode;  // 0 = paint into air, 1 = overwrite
  uint32_t pad0 = 0, pad1 = 0;
};
constexpr uint32_t kMaxOpsPerTick = 64;

// Must match TickParams in common.wgsl.
struct TickParams {
  uint32_t tick;
  uint32_t seed;
  uint32_t opsCount;
  uint32_t hashEnable;
};

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
  float pad1;
};

enum class CellKind { Unknown, Air, Solid, Liquid, Gas };

// CPU-visible snapshot of GPU state, one tick latent by design (DESIGN.md §2).
struct WorldSnapshot {
  bool valid = false;
  IVec3 mirrorBase{};                 // chunk coord of the 3x3x3 mirror corner
  std::vector<uint32_t> mirror;       // 27 chunks of voxel words
  uint32_t activeChunks = 0;
  uint64_t voxelTotal = 0;
  uint32_t worldHash = 0;
  uint32_t pick[8] = {};
};

// Owns every GPU buffer of the simulation plus the async readback ring.
class World {
 public:
  void Init(const wgpu::Device& device);

  // Records the per-tick readback copies into the encoder. Call after the sim
  // passes. Returns false if all ring slots are still in flight (skip copies).
  bool EncodeReadbacks(const wgpu::Device& device, const wgpu::CommandEncoder& enc,
                       IVec3 playerChunkBase);
  // Copies the next-tick dirty buffer into the pending slot (caller knows
  // which of dirty[0]/dirty[1] that is this tick).
  void EncodeDirtyCopy(const wgpu::CommandEncoder& enc, const wgpu::Buffer& dirtyNext);
  // Kick MapAsync for the slot used by the last EncodeReadbacks. Call after
  // queue.Submit.
  void KickReadback();

  // Latest consumed snapshot (updated by MapAsync callbacks during
  // instance.ProcessEvents()).
  const WorldSnapshot& Snap() const { return snap_; }

  // Voxel word at cell from the mirror; kind Unknown outside mirror coverage.
  CellKind KindAt(IVec3 cell, const std::vector<uint32_t>& classOf) const;

  // Deterministic worldgen height — exact CPU mirror of worldgen.wgsl.
  static int TerrainHeight(int x, int z, uint32_t seed);

  wgpu::Buffer voxels;      // kVoxelCount u32, chunk-major
  wgpu::Buffer dirty[2];    // kNumChunks u32
  wgpu::Buffer occupancy;   // kNumChunks u32
  wgpu::Buffer hash;        // 4 u32 (only [0] used)
  wgpu::Buffer tickUBO;     // TickParams
  wgpu::Buffer passUBO;     // 27 slices * 256 B (3x3x3 color phases)
  wgpu::Buffer opsBuf;      // kMaxOpsPerTick BrushOp
  wgpu::Buffer renderUBO;   // RenderParams
  wgpu::Buffer pick;        // 8 u32

 private:
  struct Slot {
    wgpu::Buffer buf;
    bool inFlight = false;
    IVec3 base{};
  };
  static constexpr int kSlots = 3;
  Slot slots_[kSlots];
  int lastSlot_ = -1;
  WorldSnapshot snap_;
};
