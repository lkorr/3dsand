#pragma once
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include <webgpu/webgpu_cpp.h>

#include "math3d.h"

// World constants. These are the SINGLE source of truth: the matching WGSL
// consts are generated from them by ShaderConstantPrelude() (gpu/resources.cpp)
// and prepended ahead of common.wgsl, so shaders cannot drift from C++.
constexpr uint32_t kWorldN = 256;

// Physical edge length of one voxel. The engine runs entirely in voxel units;
// this is the single meters<->voxels conversion, and every physical constant
// (player size, speeds, gravity, fog/media densities) derives from it. Change
// it here and here only — shaders pick it up automatically.
// Note: at the same kWorldN, smaller voxels shrink the world's physical size.
constexpr float kVoxelMeters = 0.0625f;
constexpr uint32_t kChunk = 16;
constexpr uint32_t kNChunk = kWorldN / kChunk;          // 16
constexpr uint32_t kNumChunks = kNChunk * kNChunk * kNChunk;  // 4096
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
                   kMatAutumnLeaves = 36, kMatBirchWood = 37, kMatPetal = 38;

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

// Exact-cell MutationQueue op (8 bytes) — island removal / rubble handoff
// (DESIGN.md §7). Must match sim_mutate.wgsl entry `cells`.
struct CellOp {
  uint32_t cellIdx;  // linear chunk-major cell index
  uint32_t word;     // full voxel word to store (stamp byte included)
};
constexpr uint32_t kMaxCellOpsPerTick = 65536;
// CellOp.word flag in the spare bits (24..31): only write if the target cell
// is air (prefab paint mode). Masked off by sim_mutate before the store, so
// it never lands in the grid.
constexpr uint32_t kCellOpIfAir = 0x80000000u;

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
  uint32_t pad2 = 0, pad3 = 0, pad4 = 0;
};

// Must match struct Particle in common.wgsl (32 bytes). CPU-authored particle
// spawns: debris-body fragments re-entering the world as ballistic voxels
// (DESIGN.md §7 shatter). Appended to the live page by sim_particle.wgsl
// `spawn`; part of the per-tick input stream like BrushOp/CellOp.
struct ParticleSpawn {
  int32_t px, py, pz;   // position, fixed 24.8 voxels
  int32_t vx, vy, vz;   // velocity, fixed 24.8 voxels/tick
  uint32_t payload;     // bits 0..11 material, 12..15 state
  uint32_t flags;       // forced to PFLAG_ALIVE on the GPU
};
constexpr uint32_t kMaxParticleSpawnsPerTick = 4096;

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
  int32_t pad2 = 0;
};

// ---- far-field cascades (render-only LOD — DESIGN.md §9,
// docs/PLAN_far_field_cascades.md) ----
// kFarLevels nested toroidal 256^3 volumes around the residency window; level
// k (1-based) cells are 2^k fine voxels, so each level doubles view distance
// at constant memory. Derived data: filled from worldgen on the GPU, never
// read by the sim, excluded from the world hash — determinism rule #1 is
// untouched by construction.
constexpr uint32_t kFarLevels = 6;      // outermost half-extent: 1024 m
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
  void Init(const wgpu::Device& device);

  // Records the per-tick readback copies into the encoder. Call after the sim
  // passes. Returns false if all ring slots are still in flight (skip copies).
  // particleLivePage: which particleCounts index holds the post-tick count.
  // tick: the sim tick being encoded (stamps the snapshot + fetched chunks).
  bool EncodeReadbacks(const wgpu::Device& device, const wgpu::CommandEncoder& enc,
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
  wgpu::Buffer dirtyList;   // kNumChunks u32 — compacted dirty-chunk indices
  wgpu::Buffer argsStage;   // 3 u32 — compact shader writes (x = dirty count, y = z = 1)
  wgpu::Buffer dispatchArgs;// 3 u32 — indirect-only copy of argsStage; kept out of all
                            // bind groups (Dawn forbids indirect + bound-writable usage
                            // of one buffer in the same pass, even if statically unused)
  wgpu::Buffer occupancy;   // kNumChunks u32: (rayBlockers << 16) | nonAirCount
                            // (see the occupancy packing note in common.wgsl)
  wgpu::Buffer support;     // kNumChunks u32 — support-loss flags (sim_step writes,
                            // readback consumes + clears; drives island checks)
  wgpu::Buffer hash;        // 4 u32 (only [0] used)
  wgpu::Buffer tickUBO;     // TickParams
  wgpu::Buffer passUBO;     // 27 slices * 256 B (3x3x3 color phases)
  wgpu::Buffer opsBuf;      // kMaxOpsPerTick BrushOp
  wgpu::Buffer renderUBO;   // RenderParams
  wgpu::Buffer pick;        // 8 u32

  // ---- particles + explosions (M5, DESIGN.md §5/§7) ----
  wgpu::Buffer particles[2];    // kParticleCap Particle (32 B), double-buffered
  wgpu::Buffer particleCounts;  // 4 u32: [0]/[1] = live count per page
  wgpu::Buffer claim;           // kClaimSize u32 — reinsertion claim hash
  wgpu::Buffer pArgsStage;      // 8 u32: [0..3] draw args, [4..6] dispatch args
  wgpu::Buffer pDispatchArgs;   // 3 u32, indirect-only (see dispatchArgs note)
  wgpu::Buffer drawArgs;        // 4 u32, indirect-only draw args for particles
  wgpu::Buffer expOps;          // kMaxExplosionsPerTick ExplosionOp
  wgpu::Buffer expMask;         // per-op destruction scratch (see sim_explode.wgsl)
  wgpu::Buffer cellOps;         // kMaxCellOpsPerTick CellOp (island removal)
  wgpu::Buffer spawnOps;        // kMaxParticleSpawnsPerTick ParticleSpawn
  wgpu::Buffer sprites;         // kMaxSprites Sprite (CPU-written, render-only)
  wgpu::Buffer bodyInstances;   // debris-body voxel instances (render)
  wgpu::Buffer bodyXforms;      // debris-body transforms (render)
  wgpu::Buffer genList;         // worldgen streaming: slot indices to generate

  // ---- far-field cascades (render-only; never bound in any sim pipeline) ----
  wgpu::Buffer farVox;   // kFarLevels x 256^3 material bytes, packed 4/u32
                         // (atomic in the fill/downsample kernels: partial-word
                         // byte updates from neighboring dirty chunks race)
  wgpu::Buffer farOcc;   // kFarLevels x kNumChunks u32 non-air counts
  wgpu::Buffer farList;  // kFarListCap u32 fill entries: (level-1)<<12 | slot
  wgpu::Buffer farUBO;   // FarParams

 private:
  struct Slot {
    wgpu::Buffer buf;
    bool inFlight = false;
    IVec3 base{};      // world chunk coord of the mirror corner
    IVec3 origin{};    // window origin at encode time
    uint32_t particleLivePage = 0;
    uint32_t tick = 0;
    std::vector<IVec3> fetchIds;  // world chunks riding this slot
  };
  static constexpr int kSlots = 3;
  Slot slots_[kSlots];
  int lastSlot_ = -1;
  WorldSnapshot snap_;
  IVec3 origin_{0, 0, 0};

  std::vector<IVec3> fetchQueue_;
  std::unordered_map<uint64_t, uint8_t> fetchQueued_;   // dedup (packed key)
  std::unordered_map<uint64_t, CachedChunk> cache_;     // packed world key
};
