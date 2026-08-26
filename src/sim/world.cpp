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
// The COUNT half of the occupancy buffer — the only half the snapshot ring
// carries. The sub-chunk bitmask that follows it on the GPU is render-only and
// no CPU reader wants it, so the staging slot stays exactly the size it was.
constexpr uint64_t kOccBytes = kNumChunks * 4;
// Render-only tail of the same GPU buffer; never staged, never read back.
constexpr uint64_t kSubOccBytes = (uint64_t)kNumChunks * kSubOccStride * 4;
constexpr uint64_t kHashOff = kOccOff + kOccBytes;
constexpr uint64_t kPickOff = kHashOff + 256;
constexpr uint64_t kPCountOff = kPickOff + 256;
constexpr uint64_t kSupportOff = kPCountOff + 256;
constexpr uint64_t kSupportBytes = kNumChunks * 4;
constexpr uint64_t kPageFaultOff = kSupportOff + kSupportBytes;
// MLS-MPM fluid seam: fluidArgsStage (16 u32, the FA_* map) + the block list
// (kFluidBlocks u32). Small enough to ride every snapshot; the block list
// feeds PageTable::UpdateFluidChunks and the FA words feed the CPU's
// conservative live count + the splash sound cue.
constexpr uint64_t kFluidArgsOff = kPageFaultOff + 256;
constexpr uint64_t kFluidBlocksOff = kFluidArgsOff + 256;
constexpr uint64_t kFluidBlocksBytes = kFluidBlocks * 4;
// The excited-fluid mirror fold: one byte per mirror cell, packed 4/word.
constexpr uint64_t kFluidMirrorOff = kFluidBlocksOff + kFluidBlocksBytes;
constexpr uint64_t kFluidMirrorBytes = 27ull * kChunkVol;
constexpr uint64_t kFetchOff = kFluidMirrorOff + kFluidMirrorBytes;
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
  // Counts first, then the sub-chunk bitmask (world.h kSubOccShift). One
  // buffer so every existing Occupancy barrier and bind-group entry covers the
  // mask too. Uninitialised content is not a hazard the way it would be for a
  // standalone buffer: every producer of the counts is also a producer of the
  // mask, and the first thing that ever runs over all kNumChunks slots is
  // worldgen `main` (or lr_occupancyFull on a load) — the same pass the counts
  // already depend on for not being garbage.
  occupancy = CreateBuffer(device, kOccBytes + kSubOccBytes,
                           U::Storage | U::CopySrc | U::CopyDst, "occupancy");
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

  // MLS-MPM fluid (world.h fluid block). CopySrc on the particle pair is for
  // the fluid gates' mass audits; the frame path reads back only the small
  // fluidArgsStage + block list through the snapshot ring.
  fluidParticles[0] = CreateBuffer(device,
                                   (uint64_t)kFluidCap * kFluidParticleWords * 4,
                                   U::Storage | U::CopySrc, "fluidParticlesA");
  fluidParticles[1] = CreateBuffer(device,
                                   (uint64_t)kFluidCap * kFluidParticleWords * 4,
                                   U::Storage | U::CopySrc, "fluidParticlesB");
  fluidSpawnOps = CreateBuffer(device, kMaxFluidSpawnsPerTick * sizeof(FluidSpawnOp),
                               U::Storage | U::CopyDst, "fluidSpawnOps");
  // TWO kNumChunks arrays: [slot] = blockIdx+1, [kNumChunks + slot] = the
  // chunk's 16-bit Y-OCCUPANCY mask (world.h). The whole-buffer Fill at the
  // head of PT_FLUIDMAP clears both halves, which is what the mask needs.
  fluidBlockMap = CreateBuffer(device, (uint64_t)kNumChunks * 2 * 4,
                               U::Storage | U::CopyDst, "fluidBlockMap");
  fluidBlockList = CreateBuffer(device, (uint64_t)kFluidBlocks * 4,
                                U::Storage | U::CopySrc, "fluidBlockList");
  // 8 i32 words per node (FLUID_GW in sim_fluid.wgsl): mass, momentum xyz,
  // per-species mass x3, foam — 32 MiB of per-substep scratch at 256 blocks.
  fluidGrid = CreateBuffer(device, (uint64_t)kFluidBlocks * kChunkVol * 32,
                           U::Storage, "fluidGrid");
  // 32 u32 — the FA_* word map in common.wgsl. CopyDst: the seam relies on a
  // zeroed live count after worldgen/reset (fill-cleared there).
  fluidArgsStage = CreateBuffer(device, 128,
                                U::Storage | U::CopySrc | U::CopyDst,
                                "fluidArgsStage");
  fluidDispatchArgs = CreateBuffer(device, 12, U::Indirect | U::CopyDst,
                                   "fluidDispatchArgs");
  fluidPDispatchArgs = CreateBuffer(device, 12, U::Indirect | U::CopyDst,
                                    "fluidPDispatchArgs");
  // Seam scratch (layouts documented at the members in world.h). The excite
  // scratch is 16 header words + counts + bases + the slot list; the settle
  // scratch is speed maxima + marks + the settle list + the bins.
  // CopySrc is not decoration: pass_table's `copy_exciteArgs` row stages the
  // emit dispatch's args OUT of this buffer's header (TR(FluidExciteScratch)
  // -> TW(FluidPDispatchArgs)), so it is a transfer SOURCE. Missing since the
  // seam landed and invisible until now, because --vk-smoke's two scenarios
  // never had live fluid and PT_FLUIDSEAM is only recorded when they do;
  // WP5's flip of sim.fluidExciteMode is what finally gave the smoke water to
  // excite, and validation answered immediately with 10x
  // VUID-vkCmdCopyBuffer-srcBuffer-00118.
  fluidExciteScratch = CreateBuffer(device,
                                    (uint64_t)(16 + 3 * kNumChunks) * 4,
                                    U::Storage | U::CopySrc | U::CopyDst,
                                    "fluidExciteScratch");
  fluidCalm = CreateBuffer(device, (uint64_t)kNumChunks * 4,
                           U::Storage | U::CopyDst, "fluidCalm");
  fluidSettleScratch = CreateBuffer(
      device,
      (uint64_t)(2 * kNumChunks + 16 + 2 + kFluidSettleMax * kChunkVol * 2 +
                 kFluidSettleMax * 8) * 4,
      U::Storage | U::CopyDst, "fluidSettleScratch");
  // THREE arrays of one word per 256-particle compaction span, not two:
  // [0..SPANS) survivors, [SPANS..2*SPANS) their exclusive bases, and
  // [2*SPANS..3*SPANS) the EXCITE-ORIGIN survivors among them — the population
  // sim.fluidExciteCeiling is charged against (sim_fluid_seam.wgsl compactCount
  // / the budget block in exciteScan).
  fluidCompactScratch = CreateBuffer(device, (uint64_t)(kFluidCap / 256) * 3 * 4,
                                     U::Storage, "fluidCompactScratch");
  fluidCellScratch = CreateBuffer(
      device, (uint64_t)kFluidBlocks * kChunkVol * 2 * 4,
      U::Storage | U::CopyDst, "fluidCellScratch");
  fluidMirror = CreateBuffer(device, 27ull * kChunkVol,
                             U::Storage | U::CopySrc, "fluidMirror");
  debugBoxes = CreateBuffer(device, (uint64_t)kMaxDebugBoxes * sizeof(DebugBox),
                            U::Storage | U::CopyDst, "debugBoxes");
  bodyInstances = CreateBuffer(device, 262144ull * 16, U::Storage | U::CopyDst,
                               "bodyInstances");
  bodyXforms = CreateBuffer(device, (uint64_t)kMaxBodySlots * 32,
                            U::Storage | U::CopyDst, "bodyXforms");
  genList = CreateBuffer(device, kNumChunks * 4, U::Storage | U::CopyDst, "genList");
  // JITTER page materialization gets its OWN list, deliberately NOT genList.
  // Two u32 per entry (slot, sentinel entry) against genList's one, and — the
  // reason it cannot be shared — Stream::FillSlots writes genList MID-FRAME
  // while a page fill drains at the head of the next command buffer, so the two
  // deferred writes interleave. Sharing produced a stale-list read that
  // diverged the world hash only after a window shift (loud smoke ticks 86/88).
  pageFillList = CreateBuffer(device, (uint64_t)kNumChunks * 8,
                              U::Storage | U::CopyDst, "pageFillList");

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
  // Edit patches for the cascade fill. The header half is rewritten by every
  // PrepareTick that dispatches fills (2 u32 per entry, <= 32 KiB), so the
  // kernel never reads an uninitialized count — this buffer deliberately does
  // NOT lean on zero-initialized allocation the way farVox does.
  farPatch = CreateBuffer(device, (uint64_t)kFarPatchWords * 4,
                          U::Storage | U::CopyDst, "farPatch");

  for (auto& s : slots_) {
    s.buf = CreateBuffer(device, kSlotBytes, U::MapRead | U::CopyDst, "readback");
    s.inFlight = false;
  }
  snap_.mirror.assign(27 * kChunkVol, 0);
  snap_.dirtyFlags.assign(kNumChunks, 0);
  snap_.supportFlags.assign(kNumChunks, 0);
  snap_.occupancy.assign(kNumChunks, 0);
  snap_.occStain.assign(kNumChunks, 0);
  snap_.fluidBlocks.assign(kFluidBlocks, 0);
  snap_.fluidMirror.assign(27ull * kChunkVol, 0);
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

  // clamp the 3x3x3 mirror to the residency window (world chunk coords).
  // MirrorBaseFor is the ONE clamp — the seam's mirrorFold kernel gets the
  // same value through TickParams.mirrorBase, or the fluid-occupancy fold
  // and the voxel mirror would describe two different cubes.
  s.base = MirrorBaseFor(playerChunkBase);

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
  // MLS-MPM fluid seam: the live count + event counters and the active block
  // list. 1.3 KB per snapshot; the block list is what keeps every chunk the
  // seam may write materialized (PageTable::UpdateFluidChunks).
  enc.CopyTracked(pass::Buf::FluidArgsStage, fluidArgsStage, 0, s.buf,
                  kFluidArgsOff, 128);
  enc.CopyTracked(pass::Buf::FluidBlockList, fluidBlockList, 0, s.buf,
                  kFluidBlocksOff, kFluidBlocksBytes);
  enc.CopyTracked(pass::Buf::FluidMirror, fluidMirror, 0, s.buf,
                  kFluidMirrorOff, kFluidMirrorBytes);
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
            //
            // A JITTER sentinel is POSITIONAL, so its cells cannot be one
            // repeated word: each takes the palette variant for its own world
            // coordinate. The mirror knows the world chunk of every one of its
            // 27 slots from sl.base, which is what makes that reconstructible
            // here. Getting this wrong would be invisible to the world hash and
            // would show up only as the player colliding with the wrong thing —
            // the mirror is CPU-only collision data.
            for (size_t m = 0; m < sl.mirrorSentinel.size(); m++) {
              const uint32_t e = sl.mirrorSentinel[m];
              if (e == 0u) continue;  // a real copy landed for this cell
              uint32_t* dst = snap_.mirror.data() + m * kChunkVol;
              const int mx = (int)(m % 3), my = (int)((m / 3) % 3),
                        mz = (int)(m / 9);
              const IVec3 wc{sl.base.x + mx, sl.base.y + my, sl.base.z + mz};
              if ((e & kPtJitterBit) == 0u) {
                const uint32_t w = SynthWord(e);
                for (uint32_t i = 0; i < kChunkVol; i++) dst[i] = w;
              } else {
                const int bx = wc.x * (int)kChunk, by = wc.y * (int)kChunk,
                          bz = wc.z * (int)kChunk;
                for (uint32_t i = 0; i < kChunkVol; i++)
                  dst[i] = SynthWordAt(e, bx + (int)(i % kChunk),
                                       by + (int)((i / kChunk) % kChunk),
                                       bz + (int)(i / (kChunk * kChunk)),
                                       mirrorSeed_);
              }
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
              // GPU word packs [31] anyStain | [30..16] blockers | [15..0]
              // nonAir (packOccStain, common.wgsl). Existing CPU consumers
              // (streaming evict, voxelTotal) want the non-air COUNT, so that
              // stays the stored value — but the STAIN FLAG is carried across
              // in its own array rather than masked away, because the page
              // table's free path needs it and reading it back per candidate
              // was costing a blocking WaitIdle + 16 KiB per chunk.
              snap_.occupancy[i] = occW[i] & 0xFFFFu;
              snap_.occStain[i] = (occW[i] >> 31) & 1u;
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
            // MLS-MPM fluid seam: live count, event counters, block list.
            {
              uint32_t fa[32];
              std::memcpy(fa, p + kFluidArgsOff, 128);
              snap_.fluidLive = std::min(fa[7], kFluidCap);
              snap_.fluidSettledEighths = fa[10];
              snap_.fluidExcitedEighths = fa[11];
              snap_.fluidExciteRefused = fa[12];
              snap_.fluidLastSlot = fa[14];
              snap_.fluidBlockCount = std::min(fa[3], kFluidBlocks);
              std::memcpy(snap_.fluidBlocks.data(), p + kFluidBlocksOff,
                          kFluidBlocksBytes);
              // The occupancy fold is only meaningful while fluid is live —
              // the seam stops recording (and refreshing the buffer) at
              // zero, so a stale fold must read as no water.
              if (snap_.fluidLive > 0) {
                std::memcpy(snap_.fluidMirror.data(), p + kFluidMirrorOff,
                            kFluidMirrorBytes);
              } else {
                std::fill(snap_.fluidMirror.begin(), snap_.fluidMirror.end(),
                          (uint8_t)0);
              }
            }
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
                  cc.voxels.assign(kChunkVol, 0u);  // §2.1a
                  const IVec3 wc = sl.fetchIds[i];
                  const int bx = wc.x * (int)kChunk, by = wc.y * (int)kChunk,
                            bz = wc.z * (int)kChunk;
                  // Positional for JITTER, one repeated word otherwise —
                  // SynthWordAt collapses to SynthWord when the bit is clear,
                  // so this one loop is correct for every sentinel form.
                  for (uint32_t k = 0; k < kChunkVol; k++)
                    cc.voxels[k] = SynthWordAt(
                        e, bx + (int)(k % kChunk),
                        by + (int)((k / kChunk) % kChunk),
                        bz + (int)(k / (kChunk * kChunk)), mirrorSeed_);
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
// The legacy 0..255 vnoise is NOT mirrored any more. Its only CPU caller was
// TerrainHeight, and the terrain octaves moved to vnoise2d below; the fourteen
// call sites it still has in the shader are all decorative fields (flower
// clumps, undergrowth masks) that no CPU path asks about.
//
// WGSL's `select(a, b, cond)` spelled the same way, so the mirrored bodies
// below can be read side by side with the shader's.
static int select(int a, int b, bool c) { return c ? b : a; }
static const Tuning::Worldgen& WG() { return CurrentTuning().worldgen; }

// MIRROR-BEGIN noise
// The Q14 value noise of worldgen.wgsl, mirrored line for line. Read that
// file's block for why 14 bits and why the cell is a log2 shift; the one thing
// worth repeating HERE is the reason this can be written twice at all:
//
//   `>>` ON A NEGATIVE SIGNED INTEGER IS ARITHMETIC IN BOTH LANGUAGES.
//   WGSL sign-extends by definition; C++20 (P0907) fixed signed integers as
//   two's complement and `>>` as floor-division by a power of two. So
//   `x >> csl` is the same value on both sides at every negative coordinate,
//   which is what lets it replace fdiv(), and `x & mask` replace fmodp().
//   That is the single place where these two languages could have disagreed,
//   and they do not.
//
// scripts/check_invariants.py compares the normalised token streams of the two
// blocks tagged `noise` below, so an edit to one that is not made to the other
// fails at edit time rather than as a player falling through visible ground.
struct N2 {
  int n;
  int dx;
  int dz;
};
static int vsmooth(int t) {
  int t2 = (t * t) >> 15;
  int t3 = (t2 * t) >> 15;
  return 3 * t2 - 2 * t3;
}
static int vsmoothd(int t) {
  return (6 * t * (32768 - t)) >> 15;
}
static int q15frac(int f, uint32_t csl) {
  if (csl <= 15u) { return f << (15u - csl); }
  return f >> (csl - 15u);
}
static int vbilerp(int c00, int c10, int c01, int c11, int sx, int sz) {
  int a = c00 + (((c10 - c00) * sx) >> 15);
  int b = c01 + (((c11 - c01) * sx) >> 15);
  return a + (((b - a) * sz) >> 15);
}
static N2 vnoise2d(int x, int z, uint32_t csl, uint32_t seed) {
  int gx = x >> csl;
  int gz = z >> csl;
  int mask = (int)((1u << csl) - 1u);
  int tx = q15frac(x & mask, csl);
  int tz = q15frac(z & mask, csl);
  int c00 = (int)(hash3(seed, (uint32_t)(gx), (uint32_t)(gz)) & 0x3FFFu);
  int c10 = (int)(hash3(seed, (uint32_t)(gx + 1), (uint32_t)(gz)) & 0x3FFFu);
  int c01 = (int)(hash3(seed, (uint32_t)(gx), (uint32_t)(gz + 1)) & 0x3FFFu);
  int c11 = (int)(hash3(seed, (uint32_t)(gx + 1), (uint32_t)(gz + 1)) & 0x3FFFu);
  int sx = vsmooth(tx);
  int sz = vsmooth(tz);
  N2 o;
  o.n = vbilerp(c00, c10, c01, c11, sx, sz);
  int ga = c10 - c00;
  int gb = c11 - c01;
  o.dx = ((ga + (((gb - ga) * sz) >> 15)) * vsmoothd(tx)) >> 15;
  int ha = c01 - c00;
  int hb = c11 - c10;
  o.dz = ((ha + (((hb - ha) * sx) >> 15)) * vsmoothd(tz)) >> 15;
  return o;
}
[[maybe_unused]] static int isin16(int a) {
  int p = a & 65535;
  int half = p & 32767;
  int y = (4 * half * (32768 - half)) >> 15;
  int r = (y * (25395 + ((7373 * y) >> 15))) >> 15;
  if (p >= 32768) { return -r; }
  return r;
}
[[maybe_unused]] static int vnoise3(int x, int y, int z, uint32_t cxl,
                                    uint32_t cyl, uint32_t seed) {
  int gx = x >> cxl;
  int gz = z >> cxl;
  int gy = y >> cyl;
  int mxz = (int)((1u << cxl) - 1u);
  int my = (int)((1u << cyl) - 1u);
  int sx = vsmooth(q15frac(x & mxz, cxl));
  int sz = vsmooth(q15frac(z & mxz, cxl));
  int sy = vsmooth(q15frac(y & my, cyl));
  uint32_t pz0 = pcg((uint32_t)(gz));
  uint32_t pz1 = pcg((uint32_t)(gz + 1));
  uint32_t i00 = pcg((uint32_t)(gx) ^ pz0);
  uint32_t i10 = pcg((uint32_t)(gx + 1) ^ pz0);
  uint32_t i01 = pcg((uint32_t)(gx) ^ pz1);
  uint32_t i11 = pcg((uint32_t)(gx + 1) ^ pz1);
  uint32_t s0 = seed ^ ((uint32_t)(gy) * 2654435769u);
  uint32_t s1 = seed ^ ((uint32_t)(gy + 1) * 2654435769u);
  int v0 = vbilerp((int)(pcg(s0 ^ i00) & 0x3FFFu), (int)(pcg(s0 ^ i10) & 0x3FFFu),
                   (int)(pcg(s0 ^ i01) & 0x3FFFu), (int)(pcg(s0 ^ i11) & 0x3FFFu),
                   sx, sz);
  int v1 = vbilerp((int)(pcg(s1 ^ i00) & 0x3FFFu), (int)(pcg(s1 ^ i10) & 0x3FFFu),
                   (int)(pcg(s1 ^ i01) & 0x3FFFu), (int)(pcg(s1 ^ i11) & 0x3FFFu),
                   sx, sz);
  return v0 + (((v1 - v0) * sy) >> 15);
}
// MIRROR-END noise

// The shader's vec2<i32>, so pondAt can be mirrored with the same shape.
// Outside the mirrored region: WGSL gets this type from the language.
struct IV2 {
  int x;
  int y;
};
static IV2 iv2(int a, int b) { IV2 v; v.x = a; v.y = b; return v; }

// MIRROR-BEGIN height
// The height chain, mirrored. Everything here is a pure function of (x, z,
// seed) and of the worldgen tuning; nothing reads a material id, which is what
// keeps it comparable token-for-token against the shader. The declarations are
// in the SHADER'S order, because check_invariants.py concatenates the tagged
// blocks in file order and compares the streams.
//
// `WG().foo` on this side is `TUNE_FOO` on the shader's — check_invariants.py
// derives that mapping from sim/tuning_params.def rather than hardcoding it, so
// a renamed knob keeps the check honest instead of silencing it.
struct Land {
  int h;
  int slope;
};
static Land landAt(int x, int z, uint32_t seed) {
  N2 a = vnoise2d(x, z, WG().hillLog2, seed ^ 1u);
  N2 b = vnoise2d(x, z, WG().detailLog2, seed ^ 2u);
  Land l;
  l.h = WG().baseHeight + ((a.n * WG().hillAmplitude) >> 14)
                        + ((b.n * WG().detailAmplitude) >> 14);
  int gx = ((a.dx * WG().hillAmplitude) >> (6u + WG().hillLog2))
         + ((b.dx * WG().detailAmplitude) >> (6u + WG().detailLog2));
  int gz = ((a.dz * WG().hillAmplitude) >> (6u + WG().hillLog2))
         + ((b.dz * WG().detailAmplitude) >> (6u + WG().detailLog2));
  l.slope = std::abs(gx) + std::abs(gz);
  return l;
}
static int baseHeight(int x, int z, uint32_t seed) {
  return landAt(x, z, seed).h;
}
struct Pond {
  bool present;
  int cx;
  int cz;
  int r;
  int surf;
};
static Pond pondInfo(int pt, int pz, uint32_t seed) {
  Pond p;
  p.present = false; p.cx = 0; p.cz = 0; p.r = 0; p.surf = -1;
  uint32_t rh = hash3(seed ^ 0xB0A7u, (uint32_t)(pt), (uint32_t)(pz));
  if (rh % (uint32_t)WG().pondChance != 0u) { return p; }
  int r = WG().pondRadiusMin + (int)((rh >> 4u) % (uint32_t)WG().pondRadiusSpan);
  int maxR = WG().pondRadiusMin + (int)(WG().pondRadiusSpan) - 1;
  int inset = maxR + 4;
  uint32_t span = (uint32_t)(std::max(WG().pondTile - 2 * inset, 1));
  if (WG().pondTile - 2 * inset < 1) { return p; }
  int cx = pt * WG().pondTile + inset + (int)((rh >> 9u) % span);
  int cz = pz * WG().pondTile + inset + (int)((rh >> 17u) % span);
  if (cx >= -44 && cx <= 264 && cz >= -44 && cz <= 264) { return p; }
  int q1x = cx - 420; int q1z = cz - 420;
  int q2x = cx - 260; int q2z = cz - 300;
  int q3x = cx - 220; int q3z = cz - 520;
  if (q1x * q1x + q1z * q1z < 128 * 128) { return p; }
  if (q2x * q2x + q2z * q2z < 128 * 128) { return p; }
  if (q3x * q3x + q3z * q3z < 128 * 128) { return p; }
  Land c = landAt(cx, cz, seed);
  if (c.slope > WG().pondMaxSlope) { return p; }
  p.present = true; p.cx = cx; p.cz = cz; p.r = r; p.surf = c.h;
  return p;
}
static int bermLift(int h, int surf, int past) {
  int bw = WG().pondBermWidth;
  int core = std::max(bw / 4, 2);
  if (past < core) { return std::max(h, surf + WG().pondBerm); }
  int span = std::max(bw - core, 1);
  int t = span - (past - core);
  if (t <= 0) { return h; }
  return std::max(h, h + ((surf + WG().pondBerm - h) * t) / span);
}
static IV2 pondAt(int x, int z, uint32_t seed) {
  IV2 none = iv2(-1, -1);
  Pond p = pondInfo(fdiv(x, WG().pondTile), fdiv(z, WG().pondTile), seed);
  if (!p.present) { return none; }
  int dx = x - p.cx;
  int dz = z - p.cz;
  int d2 = dx * dx + dz * dz;
  if (d2 > p.r * p.r) { return none; }
  int surf = p.surf;
  int depth = WG().pondDepthRim +
              ((p.r * p.r - d2) * (WG().pondDepth - WG().pondDepthRim)) / (p.r * p.r);
  return iv2(surf - depth, surf);
}
struct Shore {
  bool onShore;
  int past;
  int surf;
};
static Shore pondNear(int x, int z, uint32_t seed) {
  Shore s;
  s.onShore = false; s.past = 0; s.surf = -1;

  int band = std::max(WG().shoreBand, WG().pondBermWidth);
  if (band <= 0) { return s; }

  int pt = fdiv(x, WG().pondTile);
  int pz = fdiv(z, WG().pondTile);
  int lx = fmodp(x, WG().pondTile);
  int lz = fmodp(z, WG().pondTile);
  int sx = select(select(0, 1, lx >= WG().pondTile - band), -1, lx < band);
  int sz = select(select(0, 1, lz >= WG().pondTile - band), -1, lz < band);

  int best = 0x7FFFFFFF;
  Pond bestP;
  bestP.present = false; bestP.cx = 0; bestP.cz = 0; bestP.r = 0; bestP.surf = -1;
  for (int iz = 0; iz < 2; iz++) {
    int oz = select(0, sz, iz == 1);
    if (iz == 1 && sz == 0) { continue; }
    for (int ix = 0; ix < 2; ix++) {
      int ox = select(0, sx, ix == 1);
      if (ix == 1 && sx == 0) { continue; }
      Pond p = pondInfo(pt + ox, pz + oz, seed);
      if (!p.present) { continue; }
      int dx = x - p.cx;
      int dz = z - p.cz;
      int d2 = dx * dx + dz * dz;
      if (d2 <= p.r * p.r) { return s; }
      int outer = p.r + band;
      if (d2 > outer * outer) { continue; }
      if (d2 < best) { best = d2; bestP = p; }
    }
  }
  if (!bestP.present) { return s; }

  int lo = 0;
  int hi = band;
  for (int i = 0; i < 8; i++) {
    if (lo >= hi) { break; }
    int mid = (lo + hi) / 2;
    int rr = bestP.r + mid;
    if (best <= rr * rr) { hi = mid; } else { lo = mid + 1; }
  }
  s.onShore = true;
  s.past = std::max(lo - 1, 0);
  s.surf = bestP.surf;
  return s;
}
// MIRROR-END height

// Fluid-lab flat-slab mode (world.h kLabSlabY). Process-wide, set once at
// startup by --lab / --fluid-bench, mirrored to the GPU as TickParams.labMode.
static bool sLabWorld = false;
void World::SetLabWorld(bool on) { sLabWorld = on; }
bool World::LabWorld() { return sLabWorld; }

// MIRROR-BEGIN landheight
// THE HEIGHT CONTRACT (DESIGN.md; landColumn in worldgen.wgsl):
//
//     World::TerrainHeight(x, z, seed)  ==  genColumn(x, z, seed).h,  exactly,
//     for all inputs.
//
// Not "the topmost solid voxel" — that includes canopy, ruin walls, grass tufts
// and the arena deck, and it cannot be mirrored cheaply (a tree tile scan in a
// tick path). Every one of this function's ~30 callers is asking where the
// GROUND is so it can stand something on it, and this is that.
//
// This is the ONE function on the CPU side that the shader's landColumn is not
// token-compared against — it branches on a process-wide bool where the shader
// branches on a uniform, and it discards the fields the CPU has no use for.
// check_invariants.py instead compares the two blocks' INTEGER LITERALS as a
// multiset, which is the drift that actually happens here: the deleted
// surfHeightAt was a copy of exactly this arithmetic and it had already gone
// stale. The `terrain` gate's pass C1 is the per-voxel proof.
//
// COST: ~25 hash3 (two octaves, one pond tile, one pond centre, up to four
// neighbour tiles, one more centre). That is fine at O(1) per frame — spawn
// placement, fixture anchoring, a mob ground probe. NEVER call it in a
// per-voxel loop; the GPU has genColumn for that and it is hoisted per column.
int World::TerrainHeight(int x, int z, uint32_t seed) {
  // Lab slab: the same guard landColumn takes in worldgen.wgsl. Before the
  // tuning reads on purpose — the lab surface must not move when worldgen
  // knobs are tuned, or every scene's fixture heights drift.
  if (sLabWorld) return kLabSlabY;
  int h = baseHeight(x, z, seed);
  // Authored origin-area set pieces, at their absolute world coordinates.
  const int poolY = 44;
  int pdx = x - 420; int pdz = z - 420;
  int pd2 = pdx * pdx + pdz * pdz;
  if (pd2 < 68 * 68) {
    h = poolY;
  } else if (pd2 < 80 * 80) {
    h = std::max(h, poolY + 26);
  }
  int odx = x - 260; int odz = z - 300;
  int od2 = odx * odx + odz * odz;
  if (od2 < 32 * 32) {
    h = poolY + 6;
  } else if (od2 < 42 * 42) {
    h = std::max(h, poolY + 26);
  }
  int ldx = x - 220; int ldz = z - 520;
  int ld2 = ldx * ldx + ldz * ldz;
  if (ld2 < 24 * 24) {
    h = poolY + 2;
  } else if (ld2 < 34 * 34) {
    h = std::max(h, poolY + 22);
  }
  const bool inRim = pd2 < 80 * 80 || od2 < 42 * 42 || ld2 < 34 * 34;
  // Disc ponds: carve the bowl inside, raise the berm outside.
  IV2 pw = pondAt(x, z, seed);
  if (pw.y >= 0) {
    h = std::min(h, pw.x);
  } else if (!inRim) {
    Shore near = pondNear(x, z, seed);
    if (near.onShore && near.past < WG().pondBermWidth) {
      h = bermLift(h, near.surf, near.past);
    }
  }
  return h;
}
// MIRROR-END landheight

// The same branch landColumn takes, reported instead of applied. Kept adjacent
// to TerrainHeight on purpose: if one grows a case the other has to, and the
// `terrain` gate's berm assertion is only meaningful while they agree.
World::PondQuery World::PondNearColumn(int x, int z, uint32_t seed) {
  PondQuery q{false, false, 0, -1};
  if (sLabWorld) return q;
  const int pdx = x - 420, pdz = z - 420;
  const int odx = x - 260, odz = z - 300;
  const int ldx = x - 220, ldz = z - 520;
  const bool inRim = pdx * pdx + pdz * pdz < 80 * 80 ||
                     odx * odx + odz * odz < 42 * 42 ||
                     ldx * ldx + ldz * ldz < 34 * 34;
  const IV2 pw = pondAt(x, z, seed);
  if (pw.y >= 0) {
    q.inDisc = true;
    q.surf = pw.y;
    return q;
  }
  if (inRim) return q;
  const Shore near = pondNear(x, z, seed);
  q.near = near.onShore;
  q.past = near.past;
  q.surf = near.surf;
  return q;
}

// ---- MPM fluid render bounds (RenderParams::fluidLo/fluidHi) ---------------
// See the RenderParams block in world.h. Render-only DERIVED data: no sim
// kernel and no sim decision reads either of these, so the readback latency
// they ride is a picture question, never a determinism one.

void World::NoteFluidSpawnBounds(const FluidSpawnOp* ops, uint32_t n,
                                 uint32_t tick) {
  if (n == 0) return;
  IVec3 lo{INT32_MAX, INT32_MAX, INT32_MAX};
  IVec3 hi{INT32_MIN, INT32_MIN, INT32_MIN};
  for (uint32_t i = 0; i < n; i++) {
    const IVec3 c{ops[i].px >> 16, ops[i].py >> 16, ops[i].pz >> 16};  // Q16.16
    lo.x = std::min(lo.x, c.x); lo.y = std::min(lo.y, c.y); lo.z = std::min(lo.z, c.z);
    hi.x = std::max(hi.x, c.x); hi.y = std::max(hi.y, c.y); hi.z = std::max(hi.z, c.z);
  }
  // Union with the box already held UNLESS it has expired: a pour that walks
  // (the dev tool's brush) must not drag a stale tail across the world.
  const bool live = spawnBoxHi_.x >= spawnBoxLo_.x &&
                    tick - spawnBoxTick_ < kFluidSpawnBoundsTicks;
  if (live) {
    lo.x = std::min(lo.x, spawnBoxLo_.x); lo.y = std::min(lo.y, spawnBoxLo_.y);
    lo.z = std::min(lo.z, spawnBoxLo_.z);
    hi.x = std::max(hi.x, spawnBoxHi_.x); hi.y = std::max(hi.y, spawnBoxHi_.y);
    hi.z = std::max(hi.z, spawnBoxHi_.z);
  }
  spawnBoxLo_ = lo;
  spawnBoxHi_ = hi;
  spawnBoxTick_ = tick;
}

bool World::FluidRenderBounds(uint32_t tick, IVec3& lo, IVec3& hi) const {
  // No snapshot yet: hand back the whole window. The one thing this must never
  // do is clip water the CPU cannot see.
  if (!snap_.valid) {
    lo = {origin_.x * (int)kChunk, origin_.y * (int)kChunk,
          origin_.z * (int)kChunk};
    hi = {lo.x + (int)kWorldN - 1, lo.y + (int)kWorldN - 1,
          lo.z + (int)kWorldN - 1};
    return true;
  }
  lo = {INT32_MAX, INT32_MAX, INT32_MAX};
  hi = {INT32_MIN, INT32_MIN, INT32_MIN};
  // The active block list: every chunk the solver allocated node storage for
  // as of that snapshot — i.e. every chunk whose grid can hold fluid mass.
  const uint32_t nb = std::min<uint32_t>(
      snap_.fluidBlockCount, (uint32_t)snap_.fluidBlocks.size());
  for (uint32_t i = 0; i < nb; i++) {
    const IVec3 wc = SlotToWorldChunk(snap_.fluidBlocks[i]);
    const IVec3 c0{wc.x * (int)kChunk, wc.y * (int)kChunk, wc.z * (int)kChunk};
    lo.x = std::min(lo.x, c0.x); lo.y = std::min(lo.y, c0.y); lo.z = std::min(lo.z, c0.z);
    hi.x = std::max(hi.x, c0.x + (int)kChunk - 1);
    hi.y = std::max(hi.y, c0.y + (int)kChunk - 1);
    hi.z = std::max(hi.z, c0.z + (int)kChunk - 1);
  }
  if (spawnBoxHi_.x >= spawnBoxLo_.x &&
      tick - spawnBoxTick_ < kFluidSpawnBoundsTicks) {
    lo.x = std::min(lo.x, spawnBoxLo_.x); lo.y = std::min(lo.y, spawnBoxLo_.y);
    lo.z = std::min(lo.z, spawnBoxLo_.z);
    hi.x = std::max(hi.x, spawnBoxHi_.x); hi.y = std::max(hi.y, spawnBoxHi_.y);
    hi.z = std::max(hi.z, spawnBoxHi_.z);
  }
  if (hi.x < lo.x) {               // no blocks, no recent pour: nothing to draw
    lo = {0, 0, 0};
    hi = {-1, -1, -1};             // the canonical empty box the shader tests
    return false;
  }
  lo.x -= kFluidRenderPadVox; lo.y -= kFluidRenderPadVox; lo.z -= kFluidRenderPadVox;
  hi.x += kFluidRenderPadVox; hi.y += kFluidRenderPadVox; hi.z += kFluidRenderPadVox;
  return true;
}
