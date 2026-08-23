#include "sim/world.h"

#include <algorithm>
#include <cstring>

#include "gpu/resources.h"
#include "sim/pagetable.h"
#include "sim/pass_table.h"  // pass::Buf ids for the tracked readback copies
#include "sim/rng.h"
#include "sim/tuning.h"

// Readback slot layout (offsets in bytes).
constexpr uint64_t kChunkBytes = kChunkVol * 4;                 // 16 KB
constexpr uint64_t kMirrorBytes = 27 * kChunkBytes;             // 432 KB
constexpr uint64_t kDirtyOff = kMirrorBytes;
constexpr uint64_t kDirtyBytes = kNumChunks * 4;
constexpr uint64_t kOccOff = kDirtyOff + kDirtyBytes;
constexpr uint64_t kOccBytes = kNumChunks * 4;
constexpr uint64_t kHashOff = kOccOff + kOccBytes;
constexpr uint64_t kPickOff = kHashOff + 256;
constexpr uint64_t kPCountOff = kPickOff + 256;
constexpr uint64_t kSupportOff = kPCountOff + 256;
constexpr uint64_t kSupportBytes = kNumChunks * 4;
constexpr uint64_t kPageFaultOff = kSupportOff + kSupportBytes;
constexpr uint64_t kFetchOff = kPageFaultOff + 256;
constexpr uint64_t kSlotBytes = kFetchOff + (uint64_t)World::kFetchPerTick * kChunkBytes;

void World::Init(const rhi::Device& device) {
  using U = rhi::BufferUsage;
  // THE PAGE POOL. Sized by the residency mode: dense reserves one page per
  // slot (kNumChunks) so the identity map is address-identical to the
  // pre-paging buffer; paged reserves kPoolPages. This is the ONLY place the
  // pool size is decided, and PoolPages() is the ONLY reader of the mode.
  voxels = CreateBuffer(device, (uint64_t)PoolPages() * kChunkVol * 4,
                        U::Storage | U::CopySrc | U::CopyDst, "voxels");
  pageTable = CreateBuffer(device, (uint64_t)kNumChunks * 4,
                           U::Storage | U::CopySrc | U::CopyDst, "pageTable");
  pageFaults = CreateBuffer(device, 16, U::Storage | U::CopySrc | U::CopyDst,
                            "pageFaults");

  // The allocator + conservative dirty mirror + materialization rule. It
  // installs the initial table: the IDENTITY MAP in both modes, because
  // worldgen writes every slot and its post-pass compaction is what demotes
  // the all-air chunks (§3.5c). Under the identity map voxWordAt(c) resolves
  // to exactly voxels[cellIndexW(c)] — the same physical address — so the
  // whole translation path executes while producing bit-identical addresses to
  // pre-paging code, which is why --residency dense is the phase's oracle.
  pages = new PageTable();
  pages->Init(device, *this);
  dirty[0] = CreateBuffer(device, kDirtyBytes, U::Storage | U::CopySrc | U::CopyDst, "dirtyA");
  dirty[1] = CreateBuffer(device, kDirtyBytes, U::Storage | U::CopySrc | U::CopyDst, "dirtyB");
  dirtyList = CreateBuffer(device, kNumChunks * 4, U::Storage, "dirtyList");
  argsStage = CreateBuffer(device, 12, U::Storage | U::CopySrc | U::CopyDst, "argsStage");
  dispatchArgs = CreateBuffer(device, 12, U::Indirect | U::CopyDst, "dispatchArgs");
  occupancy = CreateBuffer(device, kOccBytes, U::Storage | U::CopySrc | U::CopyDst, "occupancy");
  support = CreateBuffer(device, kSupportBytes, U::Storage | U::CopySrc | U::CopyDst,
                         "supportFlags");
  hash = CreateBuffer(device, 16, U::Storage | U::CopySrc | U::CopyDst, "worldHash");
  tickUBO = CreateBuffer(device, sizeof(TickParams), U::Uniform | U::CopyDst, "tickUBO");
  passUBO = CreateBuffer(device, 54 * 256, U::Uniform | U::CopyDst, "passUBO");
  opsBuf = CreateBuffer(device, kMaxOpsPerTick * sizeof(BrushOp),
                        U::Storage | U::CopyDst, "brushOps");
  renderUBO = CreateBuffer(device, sizeof(RenderParams), U::Uniform | U::CopyDst, "renderUBO");
  pick = CreateBuffer(device, 32, U::Storage | U::CopySrc | U::CopyDst, "pick");

  particles[0] = CreateBuffer(device, (uint64_t)kParticleCap * 32, U::Storage, "particlesA");
  particles[1] = CreateBuffer(device, (uint64_t)kParticleCap * 32, U::Storage, "particlesB");
  particleCounts = CreateBuffer(device, 16, U::Storage | U::CopySrc | U::CopyDst,
                                "particleCounts");
  claim = CreateBuffer(device, (uint64_t)kClaimSize * 4, U::Storage | U::CopyDst, "claim");
  pArgsStage = CreateBuffer(device, 32, U::Storage | U::CopySrc, "pArgsStage");
  pDispatchArgs = CreateBuffer(device, 12, U::Indirect | U::CopyDst, "pDispatchArgs");
  drawArgs = CreateBuffer(device, 16, U::Indirect | U::CopyDst, "drawArgs");
  expOps = CreateBuffer(device, kMaxExplosionsPerTick * sizeof(ExplosionOp),
                        U::Storage | U::CopyDst, "explosionOps");
  expMask = CreateBuffer(device, (uint64_t)kMaxExplosionsPerTick * 68928 * 4,
                         U::Storage | U::CopyDst, "explosionMask");
  cellOps = CreateBuffer(device, kMaxCellOpsPerTick * sizeof(CellOp),
                         U::Storage | U::CopyDst, "cellOps");
  spawnOps = CreateBuffer(device, kMaxParticleSpawnsPerTick * sizeof(ParticleSpawn),
                          U::Storage | U::CopyDst, "spawnOps");
  sprites = CreateBuffer(device, kMaxSprites * sizeof(Sprite), U::Storage | U::CopyDst,
                         "sprites");

  // MLS-MPM fluid prototype (world.h fluid block). CopySrc on fluidParticles
  // is selftest-only: the fluid_det gate hashes the buffer twice-run; the
  // frame path never reads it back.
  fluidParticles = CreateBuffer(device, (uint64_t)kFluidCap * 72,
                                U::Storage | U::CopySrc, "fluidParticles");
  fluidSpawnOps = CreateBuffer(device, kMaxFluidSpawnsPerTick * sizeof(FluidSpawnOp),
                               U::Storage | U::CopyDst, "fluidSpawnOps");
  fluidBlockMap = CreateBuffer(device, (uint64_t)kNumChunks * 4,
                               U::Storage | U::CopyDst, "fluidBlockMap");
  fluidBlockList = CreateBuffer(device, (uint64_t)kFluidBlocks * 4, U::Storage,
                                "fluidBlockList");
  // 8 i32 words per node (FLUID_GW in sim_fluid.wgsl): mass, momentum xyz,
  // per-species mass x3, one spare — 32 MiB of per-substep scratch at 256
  // blocks.
  fluidGrid = CreateBuffer(device, (uint64_t)kFluidBlocks * kChunkVol * 32,
                           U::Storage, "fluidGrid");
  fluidArgsStage = CreateBuffer(device, 16, U::Storage | U::CopySrc, "fluidArgsStage");
  fluidDispatchArgs = CreateBuffer(device, 12, U::Indirect | U::CopyDst,
                                   "fluidDispatchArgs");
  debugBoxes = CreateBuffer(device, (uint64_t)kMaxDebugBoxes * sizeof(DebugBox),
                            U::Storage | U::CopyDst, "debugBoxes");
  bodyInstances = CreateBuffer(device, 262144ull * 16, U::Storage | U::CopyDst,
                               "bodyInstances");
  bodyXforms = CreateBuffer(device, (uint64_t)kMaxBodySlots * 32,
                            U::Storage | U::CopyDst, "bodyXforms");
  genList = CreateBuffer(device, kNumChunks * 4, U::Storage | U::CopyDst, "genList");

  // Far-field cascades (render-only LOD). Zero-initialized = air, so unfilled
  // regions render as sky, never garbage.
  // CopySrc is selftest-only (the phase-2 downsample gate reads back one word);
  // the frame path stays readback-free per CLAUDE.md.
  farVox = CreateBuffer(device, (uint64_t)kFarLevels * kFarVox,
                        U::Storage | U::CopySrc, "farVox");
  farOcc = CreateBuffer(device, (uint64_t)kFarLevels * kFarNumChunks * 4,
                        U::Storage, "farOcc");
  farList = CreateBuffer(device, kFarListCap * 4, U::Storage | U::CopyDst, "farList");
  farUBO = CreateBuffer(device, sizeof(FarParams), U::Uniform | U::CopyDst, "farUBO");

  for (auto& s : slots_) {
    s.buf = CreateBuffer(device, kSlotBytes, U::MapRead | U::CopyDst, "readback");
    s.inFlight = false;
  }
  snap_.mirror.assign(27 * kChunkVol, 0);
  snap_.dirtyFlags.assign(kNumChunks, 0);
  snap_.supportFlags.assign(kNumChunks, 0);
  snap_.occupancy.assign(kNumChunks, 0);
}

void World::RequestChunkFetch(IVec3 worldChunk) {
  if (!ChunkInWindow(worldChunk)) return;  // not resident: nothing to read
  uint64_t key = PackChunkKey(worldChunk);
  if (fetchQueued_.count(key)) return;
  fetchQueued_[key] = 1;
  fetchQueue_.push_back(worldChunk);
}

const CachedChunk* World::Cached(IVec3 worldChunk) const {
  auto it = cache_.find(PackChunkKey(worldChunk));
  return it == cache_.end() ? nullptr : &it->second;
}

bool World::EncodeReadbacks(const rhi::Device&, const rhi::CommandEncoder& enc,
                            IVec3 playerChunkBase, uint32_t particleLivePage,
                            uint32_t tick) {
  int slot = -1;
  for (int i = 0; i < kSlots; i++) {
    if (!slots_[i].inFlight) { slot = i; break; }
  }
  if (slot < 0) return false;
  Slot& s = slots_[slot];
  s.particleLivePage = particleLivePage;
  s.tick = tick;
  s.origin = origin_;

  // drain queued chunk fetches into this slot (bounded per tick); anything
  // that streamed out since it was queued just drops
  s.fetchIds.clear();
  while (!fetchQueue_.empty() && s.fetchIds.size() < kFetchPerTick) {
    IVec3 wc = fetchQueue_.front();
    fetchQueue_.erase(fetchQueue_.begin());
    fetchQueued_.erase(PackChunkKey(wc));
    if (!ChunkInWindow(wc)) continue;
    s.fetchIds.push_back(wc);
  }
  // Every copy below is TRACKED (rhi::CommandEncoder::CopyTracked): the sources
  // are pass-table buffers written by the tick rows earlier in this SAME
  // command buffer, so each copy must declare its read to the generated-barrier
  // tracker (vk_record.h §3.3) — an untracked CopyBufferToBuffer here reads
  // whatever the GPU happens to have written, with no barrier ordering it.
  //
  // THE CPU SEAM (§2.1a): the source offset resolves through PageOffsetOfSlot,
  // never through slot * kChunkBytes. A sentinel slot is SKIPPED entirely and
  // its 4,096 words are synthesized CPU-side when the snapshot is consumed —
  // strictly cheaper than today, since a sentinel chunk costs a 4-byte table
  // read instead of a 16 KiB GPU->CPU copy, which also reduces the readback
  // traffic kFetchPerTick exists to bound.
  s.fetchSentinel.assign(s.fetchIds.size(), 0u);
  for (size_t i = 0; i < s.fetchIds.size(); i++) {
    const uint32_t slotIdx = SlotChunkIndex(s.fetchIds[i]);
    const uint64_t off = PageOffsetOfSlot(slotIdx);
    if (off == kNoPage) {
      s.fetchSentinel[i] = PageEntryOfSlot(slotIdx);
      continue;
    }
    enc.CopyTracked(pass::Buf::Voxels, voxels, off, s.buf,
                    kFetchOff + i * kChunkBytes, kChunkBytes);
  }

  // clamp the 3x3x3 mirror to the residency window (world chunk coords)
  auto clampBase = [&](int v, int lo) {
    if (v < lo) v = lo;
    if (v > lo + (int)kNChunk - 3) v = lo + (int)kNChunk - 3;
    return v;
  };
  s.base = {clampBase(playerChunkBase.x, origin_.x),
            clampBase(playerChunkBase.y, origin_.y),
            clampBase(playerChunkBase.z, origin_.z)};

  // THE CPU SEAM again, and this is the worst of the five sites (§2.1a): the
  // mirror is CPU-only collision data, so a corrupted mirror is the player
  // falling through the floor with a CORRECT world hash. Nothing in the
  // determinism gate can catch it. Sentinel slots are skipped and synthesized
  // on consumption, exactly like the fetch above.
  s.mirrorSentinel.fill(0u);
  for (int dz = 0; dz < 3; dz++)
    for (int dy = 0; dy < 3; dy++)
      for (int dx = 0; dx < 3; dx++) {
        uint32_t ci = SlotChunkIndex({s.base.x + dx, s.base.y + dy, s.base.z + dz});
        size_t m = (size_t)((dz * 3 + dy) * 3 + dx);
        uint64_t dst = (uint64_t)m * kChunkBytes;
        uint64_t off = PageOffsetOfSlot(ci);
        if (off == kNoPage) {
          s.mirrorSentinel[m] = PageEntryOfSlot(ci);
          continue;
        }
        enc.CopyTracked(pass::Buf::Voxels, voxels, off, s.buf, dst, kChunkBytes);
      }
  // dirty buffer note: caller copies the *next-tick* dirty buffer; we take a
  // buffer reference at encode time via these explicit copies instead.
  enc.CopyTracked(pass::Buf::Occupancy, occupancy, 0, s.buf, kOccOff, kOccBytes);
  enc.CopyTracked(pass::Buf::Hash, hash, 0, s.buf, kHashOff, 16);
  enc.CopyTracked(pass::Buf::Pick, pick, 0, s.buf, kPickOff, 32);
  enc.CopyTracked(pass::Buf::ParticleCounts, particleCounts, 0, s.buf, kPCountOff, 16);
  // support-loss flags are one-shot: consume into this slot, then clear so the
  // next window of ticks accumulates fresh flags (no readback = they persist).
  // The copy-then-clear pair is a genuine transfer WAR (barrier_graph §7.4);
  // routing the fill through the tracker is what makes it fall out on Vulkan.
  enc.CopyTracked(pass::Buf::Support, support, 0, s.buf, kSupportOff, kSupportBytes);
  enc.FillTracked(pass::Buf::Support, support);
  // The page-fault counter rides the ring (risk 1's residual mitigation). No
  // clear-after-copy, unlike `support`: the counter is MONOTONIC and a non-zero
  // value is a permanent "this build has a bug" latch, which is the semantics
  // wanted. That is what makes the detector work in ordinary play rather than
  // only under test.
  enc.CopyTracked(pass::Buf::PageFaults, pageFaults, 0, s.buf, kPageFaultOff, 16);
  lastSlot_ = slot;
  return true;
}

void World::EncodeDirtyCopy(const rhi::CommandEncoder& enc, const rhi::Buffer& dirtyNext) {
  if (lastSlot_ < 0) return;
  // dirtyNext is Simulation::DirtyNext() — the buffer the tick just encoded
  // writes as "active next tick", i.e. the table's symbolic DirtyOut (the page
  // has not flipped yet at this point in SubmitTick).
  enc.CopyTracked(pass::Buf::DirtyOut, dirtyNext, 0, slots_[lastSlot_].buf, kDirtyOff,
                  kDirtyBytes);
}

void World::KickReadback() {
  if (lastSlot_ < 0) return;
  Slot& s = slots_[lastSlot_];
  s.inFlight = true;
  int slot = lastSlot_;
  lastSlot_ = -1;
  rhi::MapReadAsync(
      s.buf, 0, kSlotBytes, [this, slot](const void* mapped) {
        Slot& sl = slots_[slot];
        {
          const uint8_t* p = (const uint8_t*)mapped;
          if (p) {
            std::memcpy(snap_.mirror.data(), p, kMirrorBytes);
            // Sentinel chunks were never copied (§2.1a); synthesize their words
            // now, through the SAME rule the shader uses. SynthWord (world.h)
            // and synthWord (common.wgsl) are the two halves of one contract —
            // the page-roundtrip gate asserts they agree.
            for (size_t m = 0; m < sl.mirrorSentinel.size(); m++) {
              const uint32_t e = sl.mirrorSentinel[m];
              if (e == 0u) continue;  // a real copy landed for this cell
              const uint32_t w = SynthWord(e);
              uint32_t* dst = snap_.mirror.data() + m * kChunkVol;
              for (uint32_t i = 0; i < kChunkVol; i++) dst[i] = w;
            }
            snap_.mirrorBase = sl.base;
            snap_.windowOrigin = sl.origin;
            const uint32_t* dirtyW = (const uint32_t*)(p + kDirtyOff);
            const uint32_t* occW = (const uint32_t*)(p + kOccOff);
            uint32_t active = 0;
            uint64_t total = 0;
            for (uint32_t i = 0; i < kNumChunks; i++) {
              snap_.dirtyFlags[i] = dirtyW[i] != 0 ? 1 : 0;
              active += snap_.dirtyFlags[i];
              // GPU word packs (blockers << 16) | nonAir; CPU consumers
              // (streaming evict, voxelTotal) want the non-air count
              snap_.occupancy[i] = occW[i] & 0xFFFFu;
              total += occW[i] & 0xFFFFu;
            }
            snap_.activeChunks = active;
            snap_.voxelTotal = total;
            std::memcpy(&snap_.worldHash, p + kHashOff, 4);
            std::memcpy(&snap_.pageFaults, p + kPageFaultOff, 4);
            std::memcpy(snap_.pick, p + kPickOff, 32);
            const uint32_t* supW = (const uint32_t*)(p + kSupportOff);
            for (uint32_t i = 0; i < kNumChunks; i++)
              snap_.supportFlags[i] = supW[i] != 0 ? 1 : 0;
            uint32_t pcounts[2];
            std::memcpy(pcounts, p + kPCountOff, 8);
            snap_.particleCount =
                std::min(pcounts[sl.particleLivePage & 1], kParticleCap);
            snap_.tick = sl.tick;
            snap_.valid = true;

            // fetched chunks land in the CPU cache keyed by WORLD chunk,
            // stamped with their tick
            for (size_t i = 0; i < sl.fetchIds.size(); i++) {
              CachedChunk& cc = cache_[PackChunkKey(sl.fetchIds[i])];
              if (cc.version <= sl.tick) {
                cc.version = sl.tick;
                const uint32_t e =
                    i < sl.fetchSentinel.size() ? sl.fetchSentinel[i] : 0u;
                if (e != 0u) {
                  cc.voxels.assign(kChunkVol, SynthWord(e));  // §2.1a
                } else {
                  cc.voxels.assign(
                      (const uint32_t*)(p + kFetchOff + i * kChunkBytes),
                      (const uint32_t*)(p + kFetchOff + (i + 1) * kChunkBytes));
                }
              }
            }
            // bound the cache (drop chunks far in the past)
            if (cache_.size() > 1024) {
              for (auto it = cache_.begin(); it != cache_.end();) {
                if (it->second.version + 600 < sl.tick) it = cache_.erase(it);
                else ++it;
              }
            }
          }
        }
        sl.inFlight = false;
      });
}

CellKind World::KindAt(IVec3 cell, const std::vector<uint32_t>& classOf) const {
  if (!snap_.valid) return CellKind::Unknown;
  // outside the residency window = solid and inert (matches sim rule)
  IVec3 lo{snap_.windowOrigin.x * (int)kChunk, snap_.windowOrigin.y * (int)kChunk,
           snap_.windowOrigin.z * (int)kChunk};
  if (cell.x < lo.x || cell.y < lo.y || cell.z < lo.z ||
      cell.x >= lo.x + (int)kWorldN || cell.y >= lo.y + (int)kWorldN ||
      cell.z >= lo.z + (int)kWorldN)
    return CellKind::Solid;
  int cx = (cell.x >> 4) - snap_.mirrorBase.x;
  int cy = (cell.y >> 4) - snap_.mirrorBase.y;
  int cz = (cell.z >> 4) - snap_.mirrorBase.z;
  if (cx < 0 || cy < 0 || cz < 0 || cx >= 3 || cy >= 3 || cz >= 3)
    return CellKind::Unknown;
  int lx = cell.x & 15, ly = cell.y & 15, lz = cell.z & 15;
  uint32_t w = snap_.mirror[(size_t)((cz * 3 + cy) * 3 + cx) * kChunkVol +
                            (lz * (int)kChunk + ly) * (int)kChunk + lx];
  uint32_t mat = w & 0xFFF;
  if (mat == 0) return CellKind::Air;
  if (mat >= classOf.size()) return CellKind::Air;
  switch (classOf[mat]) {
    case 0: case 1: return CellKind::Solid;  // solid + powder both carry weight
    case 2: return CellKind::Liquid;
    default: return CellKind::Gas;
  }
}

// ---- exact CPU mirror of worldgen.wgsl (integer-only, keep in sync) ----
// pcg/hash3 come from sim/rng.h; the lowercase wrappers keep this block
// reading like the WGSL it mirrors line-for-line.
static inline uint32_t pcg(uint32_t v) { return rng::Pcg(v); }
static inline uint32_t hash3(uint32_t a, uint32_t b, uint32_t c) {
  return rng::Hash3(a, b, c);
}
// floor division / positive modulo, matching worldgen.wgsl fdiv/fmodp (the
// noise lattice must be seamless across negative world coordinates)
static int fdiv(int a, int b) {
  int q = a / b;
  if ((a % b) != 0 && ((a < 0) != (b < 0))) q -= 1;
  return q;
}
static int fmodp(int a, int b) {
  int m = a % b;
  return m < 0 ? m + b : m;
}
static int vnoise(int x, int z, int cs, uint32_t seed) {
  int gx = fdiv(x, cs), gz = fdiv(z, cs);
  int fx = fmodp(x, cs), fz = fmodp(z, cs);
  int h00 = (int)(hash3(seed, (uint32_t)gx, (uint32_t)gz) & 0xFFu);
  int h10 = (int)(hash3(seed, (uint32_t)(gx + 1), (uint32_t)gz) & 0xFFu);
  int h01 = (int)(hash3(seed, (uint32_t)gx, (uint32_t)(gz + 1)) & 0xFFu);
  int h11 = (int)(hash3(seed, (uint32_t)(gx + 1), (uint32_t)(gz + 1)) & 0xFFu);
  int v0 = h00 * (cs - fx) + h10 * fx;
  int v1 = h01 * (cs - fx) + h11 * fx;
  return (v0 * (cs - fz) + v1 * fz) / (cs * cs);
}
// Must stay bit-identical to baseHeight() in worldgen.wgsl, INCLUDING the
// horizontal scale factor — kHScale here is that shader's HSCALE.
static constexpr int kHScale = 1;
int World::TerrainHeight(int x, int z, uint32_t seed) {
  // Amplitudes/wavelengths come from tuning.json so this cannot drift from the
  // shader when they are tuned: baseHeight() reads the same values through the
  // generated TUNE_* prelude. Integer math throughout, matching the shader
  // exactly — this feeds terrain collision, so a mismatch is a player falling
  // through the ground they can see.
  const auto& w = CurrentTuning().worldgen;
  return w.baseHeight +
         (vnoise(x, z, w.hillWavelength * kHScale, seed ^ 1u) * w.hillAmplitude) / 255 +
         (vnoise(x, z, w.detailWavelength * kHScale, seed ^ 2u) * w.detailAmplitude) / 255;
}
