// vk_sim.cpp — Vulkan-side sim resources and the recorded paths that drive them.
//
// Read vk_sim.h's header for why this exists as a second set of declarations.
// The buffer sizes, layout bindings and bind-group contents below are copied
// from sim/world.cpp's World::Init and sim/simulation.cpp's Simulation::Init
// and MUST match them; --vk-smoke's hash comparison against Dawn is what proves
// they do, and a mismatch shows up there rather than as something a reader has
// to spot.

#include "gpu/vk_sim.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "gpu/resources.h"  // ShaderConstantPrelude
#include "gpu/vk_spirv.h"
#include "sim/tuning.h"

namespace vk {
namespace {

bool ReadFileText(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

// The blocking-hash staging buffer. 256 B is ample for the 16 B hash buffer and
// keeps the allocation aligned to anything a driver might want.
constexpr uint64_t kReadbackBytes = 256;

// ---------------------------------------------------------------------------
// The readback slot layout — COPIED VERBATIM from sim/world.cpp.
//
// These offsets are not an implementation detail of either backend: they are
// the contract between the copies recorded on the GPU and the pointer
// arithmetic the snapshot population does on the host. Dawn's copy of them
// lives at the top of world.cpp; if the two ever disagree the CPU mirror reads
// the wrong bytes and `KindAt` returns confident nonsense, which is a walking
// player falling through visible ground rather than anything that looks like a
// GPU bug.
//
// They are duplicated for the same reason vk_sim.h says the resource
// declarations are: `World` owns rhi:: handles and cannot be instantiated
// against this backend. What proves they agree is that a Vulkan-driven
// snapshot and a Dawn-driven snapshot of the same world must produce the same
// activeChunks / voxelTotal / hash — which is what the loud smoke compares.
constexpr uint64_t kChunkBytes = kChunkVol * 4;
constexpr uint64_t kMirrorBytes = 27 * kChunkBytes;
constexpr uint64_t kDirtyOff = kMirrorBytes;
constexpr uint64_t kDirtyBytes = kNumChunks * 4;
constexpr uint64_t kOccOff = kDirtyOff + kDirtyBytes;
constexpr uint64_t kOccBytes = kNumChunks * 4;
constexpr uint64_t kHashOff = kOccOff + kOccBytes;
constexpr uint64_t kPickOff = kHashOff + 256;
constexpr uint64_t kPCountOff = kPickOff + 256;
constexpr uint64_t kSupportOff = kPCountOff + 256;
constexpr uint64_t kSupportBytes = kNumChunks * 4;
constexpr uint64_t kFetchOff = kSupportOff + kSupportBytes;
constexpr uint64_t kSlotBytes = kFetchOff + (uint64_t)World::kFetchPerTick * kChunkBytes;

// Eviction staging: same batch bound as Stream's (256 chunks = 4 MB).
constexpr uint32_t kEvictBatch = 256;

}  // namespace

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bool SimBackend::Init(const std::string& assetDir, const std::vector<MaterialDef>& mats,
                      const std::vector<ReactionGpu>& reactions, bool lowPower,
                      bool validation, BarrierMode mode, std::string& err) {
  mode_ = mode;
  // Sync validation follows validation: it is the barrier document's primary
  // detector for a missing barrier, and this is the phase that generates them.
  if (!be_.Init(lowPower, validation, validation, err)) return false;

  using U = rhi::BufferUsage;
  auto mk = [&](uint64_t size, rhi::BufferUsage usage, const char* label) {
    return be_.CreateBuffer(size, usage, label);
  };

  // ---- buffers, in World::Init's order and with World::Init's sizes ------
  //
  // The usages are copied verbatim too. They matter less on Vulkan than on
  // WebGPU (CreateBuffer adds TRANSFER_DST unconditionally for zero-init), but
  // keeping them identical means a buffer that grows a usage on the Dawn side
  // is a visible diff here rather than a silent divergence.
  constexpr uint64_t kDirtyBytes = (uint64_t)kNumChunks * 4;
  res_.voxels = mk(kVoxelCount * 4, U::Storage | U::CopySrc | U::CopyDst, "voxels");
  res_.dirty[0] = mk(kDirtyBytes, U::Storage | U::CopySrc | U::CopyDst, "dirtyA");
  res_.dirty[1] = mk(kDirtyBytes, U::Storage | U::CopySrc | U::CopyDst, "dirtyB");
  res_.dirtyList = mk(kNumChunks * 4, U::Storage, "dirtyList");
  res_.argsStage = mk(12, U::Storage | U::CopySrc | U::CopyDst, "argsStage");
  res_.dispatchArgs = mk(12, U::Indirect | U::CopyDst, "dispatchArgs");
  res_.occupancy = mk(kDirtyBytes, U::Storage | U::CopySrc | U::CopyDst, "occupancy");
  res_.support = mk(kDirtyBytes, U::Storage | U::CopySrc | U::CopyDst, "supportFlags");
  res_.hash = mk(16, U::Storage | U::CopySrc | U::CopyDst, "worldHash");
  res_.tickUBO = mk(sizeof(TickParams), U::Uniform | U::CopyDst, "tickUBO");
  res_.passUBO = mk(54 * pass::kPassStride, U::Uniform | U::CopyDst, "passUBO");
  res_.opsBuf = mk(kMaxOpsPerTick * sizeof(BrushOp), U::Storage | U::CopyDst, "brushOps");
  res_.renderUBO = mk(sizeof(RenderParams), U::Uniform | U::CopyDst, "renderUBO");
  res_.pick = mk(32, U::Storage | U::CopySrc | U::CopyDst, "pick");
  res_.particles[0] = mk((uint64_t)kParticleCap * 32, U::Storage, "particlesA");
  res_.particles[1] = mk((uint64_t)kParticleCap * 32, U::Storage, "particlesB");
  res_.particleCounts = mk(16, U::Storage | U::CopySrc | U::CopyDst, "particleCounts");
  res_.claim = mk((uint64_t)kClaimSize * 4, U::Storage | U::CopyDst, "claim");
  res_.pArgsStage = mk(32, U::Storage | U::CopySrc, "pArgsStage");
  res_.pDispatchArgs = mk(12, U::Indirect | U::CopyDst, "pDispatchArgs");
  res_.drawArgs = mk(16, U::Indirect | U::CopyDst, "drawArgs");
  res_.expOps = mk(kMaxExplosionsPerTick * sizeof(ExplosionOp),
                   U::Storage | U::CopyDst, "explosionOps");
  res_.expMask = mk((uint64_t)kMaxExplosionsPerTick * 68928 * 4,
                    U::Storage | U::CopyDst, "explosionMask");
  res_.cellOps = mk(kMaxCellOpsPerTick * sizeof(CellOp), U::Storage | U::CopyDst, "cellOps");
  res_.spawnOps = mk(kMaxParticleSpawnsPerTick * sizeof(ParticleSpawn),
                     U::Storage | U::CopyDst, "spawnOps");
  res_.genList = mk(kNumChunks * 4, U::Storage | U::CopyDst, "genList");
  res_.farVox = mk((uint64_t)kFarLevels * kFarVox, U::Storage | U::CopySrc, "farVox");
  res_.farOcc = mk((uint64_t)kFarLevels * kFarNumChunks * 4, U::Storage, "farOcc");
  res_.farList = mk(kFarListCap * 4, U::Storage | U::CopyDst, "farList");
  res_.farUBO = mk(sizeof(FarParams), U::Uniform | U::CopyDst, "farUBO");
  res_.materials = mk(sizeof(MaterialGpu) * kMaterialSlots,
                      U::Storage | U::CopyDst, "materials");
  res_.reactions = mk(sizeof(ReactionGpu) * kMaxReactions,
                      U::Storage | U::CopyDst, "reactions");
  res_.readback = mk(kReadbackBytes, U::MapRead | U::CopyDst, "hashReadback");

  // The readback ring: 3 host-visible slots, same size and same layout as
  // World::Init allocates (barrier_graph §4.2).
  for (auto& s : slots_) {
    s.buf = mk(kSlotBytes, U::MapRead | U::CopyDst, "readback");
    s.inFlight = false;
    s.fence = VK_NULL_HANDLE;
  }
  snap_.mirror.assign(27 * kChunkVol, 0);
  snap_.dirtyFlags.assign(kNumChunks, 0);
  snap_.supportFlags.assign(kNumChunks, 0);
  snap_.occupancy.assign(kNumChunks, 0);

  for (Buffer* b : {res_.voxels, res_.dirty[0], res_.dirty[1], res_.materials,
                    res_.reactions, res_.readback}) {
    if (!b) {
      err = "buffer allocation failed";
      return false;
    }
  }

  // ZERO-INIT EVERYTHING before anything reads it (barrier_graph §4.8). WebGPU
  // guarantees zero-initialised buffers and Vulkan guarantees nothing, and the
  // determinism note there matters more than the fill: garbage in the tick-stamp
  // bits of never-written voxels does not show up in the hash (which covers bits
  // 0..15 and 24..30) but DOES change which voxels act on which substep, so a
  // partial policy produces a divergence one tick late, attributed to the wrong
  // thing.
  if (!be_.ZeroInitAll(err)) return false;

  // ---- the tables the shaders read -------------------------------------
  //
  // Same content Simulation::UploadTables builds. The stain palette mirror and
  // the art palette are part of that table on the Dawn side; the stain mirror
  // is reproduced because staining materials are hashed state, the art palette
  // is not because it is render-only and no compute kernel reads it.
  {
    std::vector<MaterialGpu> table(kMaterialSlots, MaterialGpu{});
    for (size_t i = 0; i < mats.size() && i < kMaterialSlots; i++) table[i] = mats[i].gpu;
    for (const auto& d : mats) {
      uint32_t type = d.gpu.stainPack & kStainPackTypeMask;
      if (type == 0) continue;
      table[kStainPaletteBase + type].stainColor = d.gpu.stainColor;
    }
    be_.QueueWrite(res_.materials, 0, table.data(), table.size() * sizeof(MaterialGpu));

    std::vector<ReactionGpu> rt(kMaxReactions, ReactionGpu{});
    for (size_t i = 0; i < reactions.size() && i < kMaxReactions; i++) rt[i] = reactions[i];
    be_.QueueWrite(res_.reactions, 0, rt.data(), rt.size() * sizeof(ReactionGpu));
  }

  // passUBO: 27 colour-phase slices x 2 gravity substeps. Byte-identical to
  // Simulation::Init's construction — this is what the CA row's dynamic offset
  // indexes, so an error here is a wrong colour order, i.e. a different world.
  {
    std::vector<uint32_t> phases(54 * pass::kPassStride / 4, 0);
    for (uint32_t k = 0; k < 54; k++) {
      uint32_t* p = &phases[k * pass::kPassStride / 4];
      uint32_t c = k % 27;
      p[0] = c % 3;
      p[1] = (c / 3) % 3;
      p[2] = c / 9;
      p[3] = k / 27;  // substep
    }
    be_.QueueWrite(res_.passUBO, 0, phases.data(), phases.size() * 4);
  }

  if (!BuildPipelines(assetDir, err)) return false;
  if (!BuildDescriptors(err)) return false;

  // Drain the uploads queued above. They flush at the head of the next command
  // buffer from whichever path records one (barrier_graph §4.1), and nothing has
  // recorded one yet — so a submit here is what makes the tables live before the
  // first worldgen dispatch reads them.
  VkCommandBuffer cmd = be_.BeginCommands("initUploads");
  if (cmd == VK_NULL_HANDLE) {
    err = "could not begin the init upload command buffer";
    return false;
  }
  if (be_.SubmitCommands(cmd, err) == VK_NULL_HANDLE) return false;
  return be_.WaitIdle(err);
}

bool SimBackend::BuildPipelines(const std::string& assetDir, std::string& err) {
  const std::string shaderDir = assetDir + "/shaders";
  std::string common;
  if (!ReadFileText(shaderDir + "/common.wgsl", common)) {
    err = "cannot read common.wgsl";
    return false;
  }
  // ASSEMBLED EXACTLY AS LoadShader DOES — prelude + tuning + common + body. A
  // compiler that succeeds on a different string than the engine feeds it has
  // proven nothing, and the tuning block in particular means the source depends
  // on live tuning values (which is why the offline-.spv fallback was rejected
  // in phase 3a: F5 would freeze them).
  const std::string prefix = ShaderConstantPrelude() + "\n" +
                             TuningWgslBlock(CurrentTuning()) + "\n" + common + "\n";
  const uint32_t bodyLines = vkspv::CountLines(prefix);

  // Descriptor set layouts, mirroring Simulation::Init.
  auto entry = [](uint32_t binding, rhi::BufferBindingType type, bool dynamic = false) {
    rhi::BindGroupLayoutEntry e{};
    e.binding = binding;
    e.visibility = rhi::ShaderStage::Compute;
    e.type = type;
    e.hasDynamicOffset = dynamic;
    return e;
  };
  using T = rhi::BufferBindingType;
  static const rhi::BindGroupLayoutEntry kSimEntries[] = {
      entry(0, T::Storage),          // voxels
      entry(1, T::Storage),          // dirtyIn
      entry(2, T::Storage),          // dirtyOut
      entry(3, T::ReadOnlyStorage),  // materials
      entry(4, T::Uniform),          // TickParams
      entry(5, T::Uniform, true),    // PassParams (dynamic offset)
      entry(6, T::ReadOnlyStorage),  // brush ops
      entry(7, T::Storage),          // occupancy
      entry(8, T::Storage),          // world hash
      entry(9, T::Storage),          // pick
      entry(10, T::Uniform),         // RenderParams
      entry(11, T::ReadOnlyStorage), // reactions
      entry(12, T::Storage),         // dirtyList
      entry(13, T::Storage),         // argsStage
      entry(14, T::ReadOnlyStorage), // cellOps
      entry(15, T::Storage),         // support
      entry(16, T::ReadOnlyStorage), // genList
  };
  static const rhi::BindGroupLayoutEntry kSlimEntries[] = {
      entry(0, T::Storage), entry(1, T::Storage), entry(2, T::Storage),
      entry(3, T::ReadOnlyStorage), entry(4, T::Uniform),
  };
  static const rhi::BindGroupLayoutEntry kPartEntries[] = {
      entry(0, T::Storage),          // particles read page
      entry(1, T::Storage),          // particles write page
      entry(2, T::Storage),          // counts
      entry(3, T::Storage),          // claim
      entry(4, T::Storage),          // pArgsStage
      entry(5, T::ReadOnlyStorage),  // explosion ops
      entry(6, T::Storage),          // explosion scratch
      entry(7, T::ReadOnlyStorage),  // spawn ops
  };
  static const rhi::BindGroupLayoutEntry kFarEntries[] = {
      entry(0, T::Storage),          // farVox
      entry(1, T::Storage),          // farOcc
      entry(2, T::ReadOnlyStorage),  // farList
      entry(3, T::Uniform),          // FarParams
      entry(4, T::ReadOnlyStorage),  // dirtyList
  };

  simSetL_ = be_.CreateSetLayout(kSimEntries, std::size(kSimEntries));
  slimSetL_ = be_.CreateSetLayout(kSlimEntries, std::size(kSlimEntries));
  partSetL_ = be_.CreateSetLayout(kPartEntries, std::size(kPartEntries));
  farSetL_ = be_.CreateSetLayout(kFarEntries, std::size(kFarEntries));
  if (!simSetL_ || !slimSetL_ || !partSetL_ || !farSetL_) {
    err = "descriptor set layout creation failed";
    return false;
  }

  simPL_ = be_.CreatePipelineLayout(&simSetL_, 1);
  VkDescriptorSetLayout slimPart[] = {slimSetL_, partSetL_};
  simPL2_ = be_.CreatePipelineLayout(slimPart, 2);
  VkDescriptorSetLayout slimFar[] = {slimSetL_, farSetL_};
  farPL_ = be_.CreatePipelineLayout(slimFar, 2);
  if (!simPL_ || !simPL2_ || !farPL_) {
    err = "pipeline layout creation failed";
    return false;
  }

  // Pipelines, keyed by pass::Pipe so the recorder can index straight in. The
  // layout choice per pipeline is Simulation::BuildPipelines' choice, and it is
  // not cosmetic: a pipeline layout must declare every descriptor set the
  // SPIR-V references or the driver faults (phase 3a found this with the far
  // kernels' @group(1)).
  struct Spec {
    pass::Pipe pipe;
    const char* file;
    const char* entry;
    VkPipelineLayout layout;
  };
  const Spec specs[] = {
      {pass::Pipe::Worldgen, "worldgen.wgsl", "main", simPL_},
      {pass::Pipe::WorldgenList, "worldgen.wgsl", "list", simPL_},
      {pass::Pipe::FarFill, "worldgen.wgsl", "far", farPL_},
      {pass::Pipe::FarDown, "worldgen.wgsl", "fardown", farPL_},
      {pass::Pipe::Mutate, "sim_mutate.wgsl", "main", simPL_},
      {pass::Pipe::MutateCells, "sim_mutate.wgsl", "cells", simPL_},
      {pass::Pipe::Compact, "sim_compact.wgsl", "main", simPL_},
      {pass::Pipe::CompactNext, "sim_compact.wgsl", "mainNext", simPL_},
      {pass::Pipe::Step, "sim_step.wgsl", "main", simPL_},
      {pass::Pipe::Occupancy, "sim_occupancy.wgsl", "main", simPL_},
      {pass::Pipe::OccupancyDirty, "sim_occupancy.wgsl", "mainDirty", simPL_},
      {pass::Pipe::Pick, "sim_pick.wgsl", "main", simPL_},
      {pass::Pipe::ExplodeMark, "sim_explode.wgsl", "mark", simPL2_},
      {pass::Pipe::ExplodeApply, "sim_explode.wgsl", "apply", simPL2_},
      {pass::Pipe::PArgs1, "sim_particle.wgsl", "args1", simPL2_},
      {pass::Pipe::PSpawn, "sim_particle.wgsl", "spawn", simPL2_},
      {pass::Pipe::PIntegrate, "sim_particle.wgsl", "integrate", simPL2_},
      {pass::Pipe::PArgs2, "sim_particle.wgsl", "args2", simPL2_},
      {pass::Pipe::PResolve, "sim_particle.wgsl", "resolve", simPL2_},
  };

  std::string lastFile, body;
  for (const Spec& s : specs) {
    if (lastFile != s.file) {
      if (!ReadFileText(shaderDir + "/" + s.file, body)) {
        err = std::string("cannot read ") + s.file;
        return false;
      }
      lastFile = s.file;
    }
    std::string diag;
    VkShaderModule m = be_.GetShaderModule(prefix + body, s.file, s.entry, bodyLines, diag);
    if (m == VK_NULL_HANDLE) {
      err = std::string("shader compile failed for ") + s.file + "::" + s.entry + "\n" + diag;
      return false;
    }
    VkPipeline p = be_.CreateComputePipeline(s.layout, m, s.entry, s.file);
    if (p == VK_NULL_HANDLE) {
      err = std::string("pipeline creation failed for ") + s.file + "::" + s.entry;
      return false;
    }
    pipelines_[(int)s.pipe] = p;
  }
  return true;
}

bool SimBackend::BuildDescriptors(std::string& err) {
  auto b = [](uint32_t binding, Buffer* buf, uint64_t size = 0) {
    rhi::BindGroupEntry e{};
    e.binding = binding;
    e.size = size;  // 0 = rest of the buffer from offset
    (void)buf;
    return e;
  };
  // The entry array carries the binding + size; the buffer travels in a
  // parallel vector, matching Backend::CreateDescriptorSet's signature (the
  // seam's rhi::BindGroupEntry holds an rhi::Buffer, which is a Dawn handle and
  // meaningless here).
  auto entry = [](uint32_t binding, rhi::BufferBindingType type, bool dynamic = false) {
    rhi::BindGroupLayoutEntry e{};
    e.binding = binding;
    e.visibility = rhi::ShaderStage::Compute;
    e.type = type;
    e.hasDynamicOffset = dynamic;
    return e;
  };
  using T = rhi::BufferBindingType;
  const rhi::BindGroupLayoutEntry simL[] = {
      entry(0, T::Storage), entry(1, T::Storage), entry(2, T::Storage),
      entry(3, T::ReadOnlyStorage), entry(4, T::Uniform), entry(5, T::Uniform, true),
      entry(6, T::ReadOnlyStorage), entry(7, T::Storage), entry(8, T::Storage),
      entry(9, T::Storage), entry(10, T::Uniform), entry(11, T::ReadOnlyStorage),
      entry(12, T::Storage), entry(13, T::Storage), entry(14, T::ReadOnlyStorage),
      entry(15, T::Storage), entry(16, T::ReadOnlyStorage),
  };
  const rhi::BindGroupLayoutEntry slimL[] = {
      entry(0, T::Storage), entry(1, T::Storage), entry(2, T::Storage),
      entry(3, T::ReadOnlyStorage), entry(4, T::Uniform),
  };
  const rhi::BindGroupLayoutEntry partL[] = {
      entry(0, T::Storage), entry(1, T::Storage), entry(2, T::Storage),
      entry(3, T::Storage), entry(4, T::Storage), entry(5, T::ReadOnlyStorage),
      entry(6, T::Storage), entry(7, T::ReadOnlyStorage),
  };
  const rhi::BindGroupLayoutEntry farL[] = {
      entry(0, T::Storage), entry(1, T::Storage), entry(2, T::ReadOnlyStorage),
      entry(3, T::Uniform), entry(4, T::ReadOnlyStorage),
  };

  for (int page = 0; page < 2; page++) {
    // simBG: bindings in Simulation::Init's order. passUBO at binding 5 takes a
    // 16-BYTE range — the dynamic-offset WINDOW, not the whole buffer. The
    // dynamic offset then slides that window to k * kPassStride. Binding the
    // whole 13.5 KiB buffer with a dynamic offset would put offset+range past
    // the end for k > 0 and is invalid.
    rhi::BindGroupEntry e[17];
    std::vector<Buffer*> bufs;
    auto set = [&](int i, uint32_t binding, Buffer* buf, uint64_t size = 0) {
      e[i] = b(binding, buf, size);
      bufs.push_back(buf);
    };
    set(0, 0, res_.voxels);
    set(1, 1, res_.dirty[page]);
    set(2, 2, res_.dirty[1 - page]);
    set(3, 3, res_.materials);
    set(4, 4, res_.tickUBO);
    set(5, 5, res_.passUBO, 16);
    set(6, 6, res_.opsBuf);
    set(7, 7, res_.occupancy);
    set(8, 8, res_.hash);
    set(9, 9, res_.pick);
    set(10, 10, res_.renderUBO);
    set(11, 11, res_.reactions);
    set(12, 12, res_.dirtyList);
    set(13, 13, res_.argsStage);
    set(14, 14, res_.cellOps);
    set(15, 15, res_.support);
    set(16, 16, res_.genList);
    simSet_[page] = be_.CreateDescriptorSet(simSetL_, simL, e, 17, bufs);

    rhi::BindGroupEntry se[5];
    std::vector<Buffer*> sbufs;
    auto sset = [&](int i, uint32_t binding, Buffer* buf) {
      se[i] = b(binding, buf);
      sbufs.push_back(buf);
    };
    sset(0, 0, res_.voxels);
    sset(1, 1, res_.dirty[page]);
    sset(2, 2, res_.dirty[1 - page]);
    sset(3, 3, res_.materials);
    sset(4, 4, res_.tickUBO);
    slimSet_[page] = be_.CreateDescriptorSet(slimSetL_, slimL, se, 5, sbufs);

    rhi::BindGroupEntry pe[8];
    std::vector<Buffer*> pbufs;
    auto pset = [&](int i, uint32_t binding, Buffer* buf) {
      pe[i] = b(binding, buf);
      pbufs.push_back(buf);
    };
    pset(0, 0, res_.particles[page]);
    pset(1, 1, res_.particles[1 - page]);
    pset(2, 2, res_.particleCounts);
    pset(3, 3, res_.claim);
    pset(4, 4, res_.pArgsStage);
    pset(5, 5, res_.expOps);
    pset(6, 6, res_.expMask);
    pset(7, 7, res_.spawnOps);
    partSet_[page] = be_.CreateDescriptorSet(partSetL_, partL, pe, 8, pbufs);

    if (!simSet_[page] || !slimSet_[page] || !partSet_[page]) {
      err = "descriptor set allocation failed";
      return false;
    }
  }

  rhi::BindGroupEntry fe[5];
  std::vector<Buffer*> fbufs;
  auto fset = [&](int i, uint32_t binding, Buffer* buf) {
    fe[i] = b(binding, buf);
    fbufs.push_back(buf);
  };
  fset(0, 0, res_.farVox);
  fset(1, 1, res_.farOcc);
  fset(2, 2, res_.farList);
  fset(3, 3, res_.farUBO);
  fset(4, 4, res_.dirtyList);
  farSet_ = be_.CreateDescriptorSet(farSetL_, farL, fe, 5, fbufs);
  if (!farSet_) {
    err = "far descriptor set allocation failed";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Symbolic resolution — barrier_graph §2.2, at record time, exactly where
// Simulation::PassBuffer resolves.
// ---------------------------------------------------------------------------
Bindings SimBackend::Resolve() const {
  Bindings bd{};
  using B = pass::Buf;
  auto& b = bd.buffers;
  b[(int)B::Voxels] = res_.voxels;
  b[(int)B::DirtyIn] = res_.dirty[page_];
  b[(int)B::DirtyOut] = res_.dirty[1 - page_];
  b[(int)B::Dirty0] = res_.dirty[0];
  b[(int)B::Dirty1] = res_.dirty[1];
  b[(int)B::Materials] = res_.materials;
  b[(int)B::TickUBO] = res_.tickUBO;
  b[(int)B::PassUBO] = res_.passUBO;
  b[(int)B::OpsBuf] = res_.opsBuf;
  b[(int)B::Occupancy] = res_.occupancy;
  b[(int)B::Hash] = res_.hash;
  b[(int)B::Pick] = res_.pick;
  b[(int)B::RenderUBO] = res_.renderUBO;
  b[(int)B::Reactions] = res_.reactions;
  b[(int)B::DirtyList] = res_.dirtyList;
  b[(int)B::ArgsStage] = res_.argsStage;
  b[(int)B::CellOps] = res_.cellOps;
  b[(int)B::Support] = res_.support;
  b[(int)B::GenList] = res_.genList;
  b[(int)B::DispatchArgs] = res_.dispatchArgs;
  b[(int)B::ParticlesRead] = res_.particles[page_];
  b[(int)B::ParticlesWrite] = res_.particles[1 - page_];
  b[(int)B::ParticleCounts] = res_.particleCounts;
  b[(int)B::Claim] = res_.claim;
  b[(int)B::PArgsStage] = res_.pArgsStage;
  b[(int)B::PDispatchArgs] = res_.pDispatchArgs;
  b[(int)B::ExpOps] = res_.expOps;
  b[(int)B::ExpMask] = res_.expMask;
  b[(int)B::SpawnOps] = res_.spawnOps;
  b[(int)B::DrawArgs] = res_.drawArgs;
  b[(int)B::FarVox] = res_.farVox;
  b[(int)B::FarOcc] = res_.farOcc;
  b[(int)B::FarList] = res_.farList;
  b[(int)B::FarUBO] = res_.farUBO;

  for (int i = 0; i < 32; i++) bd.pipelines[i] = pipelines_[i];
  bd.simLayout = simPL_;
  bd.slimPartLayout = simPL2_;
  bd.slimFarLayout = farPL_;
  bd.simSet = simSet_[page_];
  bd.slimSet = slimSet_[page_];
  bd.particleSet = partSet_[page_];
  bd.farSet = farSet_;
  return bd;
}

bool SimBackend::RunTable(pass::Table which, const RecordCtx& cx, std::string& err) {
  VkCommandBuffer cmd = be_.BeginCommands("table");
  if (cmd == VK_NULL_HANDLE) {
    err = "could not begin a command buffer";
    return false;
  }
  Recorder rec(be_, Resolve(), mode_);
  rec.Begin(cmd);
  rec.RecordTable(which, cx);
  rec.Finish();
  lastStats_ = rec.Stats();
  if (be_.SubmitCommands(cmd, err) == VK_NULL_HANDLE) return false;
  return true;
}

// ---------------------------------------------------------------------------
// The recorded paths
// ---------------------------------------------------------------------------

bool SimBackend::SubmitWorldgen(uint32_t seed, std::string& err) {
  // EncodeWorldgen resets the page to 0 (simulation.cpp), and so must this: the
  // worldgen dispatch writes dirtyIn and dirtyOut through the page-0 bind group,
  // and the first tick reads dirtyIn expecting the same page.
  page_ = 0;
  TickParams tp{};
  tp.tick = 0;
  tp.seed = seed;
  be_.QueueWrite(res_.tickUBO, 0, &tp, sizeof(tp));
  RecordCtx cx{};
  return RunTable(pass::Table::Worldgen, cx, err);
}

bool SimBackend::SubmitHashOnly(uint32_t seed, std::string& err) {
  TickParams tp{};
  tp.tick = 0;
  tp.seed = seed;
  tp.hashEnable = 1;
  be_.QueueWrite(res_.tickUBO, 0, &tp, sizeof(tp));
  RecordCtx cx{};
  return RunTable(pass::Table::HashOnly, cx, err);
}

bool SimBackend::SubmitTick(uint32_t tick, uint32_t seed, bool hashEnable,
                            std::string& err) {
  // TickParams built exactly as SubmitTick (test/support.cpp) builds it for a
  // quiet world: no ops, no explosions, no cells, no spawns, no far-field. The
  // day phase is derived from `tick` alone, which is what keeps the
  // daylight-gated reactions deterministic (CLAUDE.md rule 1) — and it is
  // therefore load-bearing for hash equality with Dawn, not incidental.
  TickParams tp{};
  tp.tick = tick;
  tp.seed = seed;
  tp.opsCount = 0;
  tp.hashEnable = hashEnable ? 1u : 0u;
  tp.expCount = 0;
  tp.page = page_;
  tp.cellCount = 0;
  const Tuning& t = CurrentTuning();
  const uint32_t ticksPerDay =
      (uint32_t)(t.dayNight.cycleMinutes < 1 ? 1 : t.dayNight.cycleMinutes) * 60u * 30u;
  tp.dayPhase = DayPhaseForTick(tick, ticksPerDay, t.dayNight.freeze != 0,
                                (uint32_t)t.dayNight.freezePhase);
  be_.QueueWrite(res_.tickUBO, 0, &tp, sizeof(tp));

  RecordCtx cx{};
  cx.hashEnable = hashEnable;
  // particlesActive stays false: a quiet world has no particles, and the Dawn
  // side derives the same false from the same (empty) inputs. If this ever
  // differs from what SubmitTick would compute, the particle rows record on one
  // backend and not the other and the hashes diverge — which is the smoke
  // test's job to catch.
  if (!RunTable(pass::Table::Tick, cx, err)) return false;
  // The page flip happens on the CPU immediately after submit, exactly as
  // Simulation::FlipPage does (barrier_graph §4.4). It only changes which
  // pre-built descriptor set the NEXT recording references; no descriptor set
  // is mutated and no buffer is touched, so a tick still in flight is
  // unaffected.
  page_ = 1 - page_;
  return true;
}

bool SimBackend::ReadHash(uint32_t& out, std::string& err) {
  VkCommandBuffer cmd = be_.BeginCommands("hashRead");
  if (cmd == VK_NULL_HANDLE) {
    err = "could not begin the hash read command buffer";
    return false;
  }
  Recorder rec(be_, Resolve(), mode_);
  rec.Begin(cmd);
  // Goes through the recorder, not a bare vkCmdCopyBuffer: the hash buffer's
  // last writer (occupancy, in a PREVIOUS submit) is ordered by the head global
  // barrier, and the staging destination gets its TRANSFER_WRITE -> HOST_READ
  // barrier from Finish().
  rec.CopyToHost(res_.hash, 0, res_.readback, 0, 4);
  rec.Finish();
  if (be_.SubmitCommands(cmd, err) == VK_NULL_HANDLE) return false;
  if (!be_.WaitIdle(err)) return false;
  if (!res_.readback->mapped) {
    err = "readback buffer is not host-mapped";
    return false;
  }
  std::memcpy(&out, res_.readback->mapped, 4);
  return true;
}

Buffer* SimBackend::Buf(pass::Buf id) const { return Resolve().buffers[(int)id]; }

// ---------------------------------------------------------------------------
// The readback ring — barrier_graph §4.2 / §2.4 phase 7a+7b.
//
// This is `World::EncodeReadbacks` + `World::EncodeDirtyCopy` + the body of
// `World::KickReadback`'s map callback, against the Vulkan resources. The
// division into three functions there was forced by WebGPU's async map; here
// the copies are one function and the callback body is `PopulateSnapshot`.
//
// EVERY COPY GOES THROUGH THE TRACKER. `CopyTracked` takes the source as a
// `pass::Buf` id, so the RAW against whatever wrote `voxels` / `occupancy` /
// `hash` / `support` earlier in this same command buffer is derived by §3.3,
// not asserted here. The `support` copy-then-clear pair is the §7.4 WAR, and it
// falls out because `FillTracked` declares a TransferWrite on the buffer the
// tracker has just seen a TransferRead on.
// ---------------------------------------------------------------------------
bool SimBackend::EncodeReadbacks(Recorder& rec, IVec3 playerChunkBase,
                                 uint32_t particleLivePage, uint32_t tick) {
  int slot = -1;
  for (int i = 0; i < kSlots; i++) {
    if (!slots_[i].inFlight) { slot = i; break; }
  }
  // All three in flight: skip the copies entirely, exactly as world.cpp does.
  // Under Dawn this was already a true statement about the GPU; here the flag
  // is backed by a real fence, so it means what it claims rather than what a
  // callback happened to have run.
  if (slot < 0) return false;
  ReadbackSlot& s = slots_[slot];
  s.particleLivePage = particleLivePage;
  s.tick = tick;
  s.origin = origin_;

  // NOTE: the chunk-fetch queue (World::RequestChunkFetch) has no Vulkan-side
  // consumer yet — nothing on this backend reads the CPU chunk cache, because
  // the cache feeds island detection and terrain meshing, both of which live
  // above the seam in systems phase 3c does not drive. The fetch RANGE of the
  // slot is still allocated and still copied-to-size-zero, so the layout is
  // identical and a phase-4/5 consumer only has to fill fetchIds.
  s.fetchIds.clear();

  auto clampBase = [&](int v, int lo) {
    if (v < lo) v = lo;
    if (v > lo + (int)kNChunk - 3) v = lo + (int)kNChunk - 3;
    return v;
  };
  s.base = {clampBase(playerChunkBase.x, origin_.x),
            clampBase(playerChunkBase.y, origin_.y),
            clampBase(playerChunkBase.z, origin_.z)};

  for (int dz = 0; dz < 3; dz++)
    for (int dy = 0; dy < 3; dy++)
      for (int dx = 0; dx < 3; dx++) {
        uint32_t ci = World::SlotChunkIndex({s.base.x + dx, s.base.y + dy, s.base.z + dz});
        uint64_t dst = (uint64_t)((dz * 3 + dy) * 3 + dx) * kChunkBytes;
        rec.CopyTracked(pass::Buf::Voxels, (uint64_t)ci * kChunkBytes, s.buf, dst,
                        kChunkBytes);
      }
  rec.CopyTracked(pass::Buf::Occupancy, 0, s.buf, kOccOff, kOccBytes);
  rec.CopyTracked(pass::Buf::Hash, 0, s.buf, kHashOff, 16);
  rec.CopyTracked(pass::Buf::Pick, 0, s.buf, kPickOff, 32);
  rec.CopyTracked(pass::Buf::ParticleCounts, 0, s.buf, kPCountOff, 16);
  // Support-loss flags are one-shot: consume into this slot, then clear so the
  // next window of ticks accumulates fresh flags. The clear is a genuine WAR
  // against the copy immediately above it (§7.4) and is routed through the
  // tracker for exactly that reason.
  rec.CopyTracked(pass::Buf::Support, 0, s.buf, kSupportOff, kSupportBytes);
  rec.FillTracked(pass::Buf::Support);

  // Phase 7b: the dirty copy. In world.cpp this is a separate function called
  // after EncodeReadbacks returns, which is what forced the host barrier to be
  // emitted at Finish() rather than at a table index. Here it is recorded in
  // line — but the host barrier STILL comes from Finish(), because making it
  // depend on this call's position is precisely the fragility §2.4 phase 7b
  // warns about.
  rec.CopyTracked(pass::Buf::DirtyOut, 0, s.buf, kDirtyOff, kDirtyBytes);

  lastSlot_ = slot;
  return true;
}

// The body of Dawn's MapAsync callback (world.cpp), verbatim in effect. The
// CPU-side consumers of WorldSnapshot must not be able to tell the backends
// apart, so this reads the same offsets and derives the same fields — including
// the occupancy word's low half (the GPU packs blockers<<16 | nonAir and every
// CPU consumer wants the non-air count).
void SimBackend::PopulateSnapshot(ReadbackSlot& s) {
  const uint8_t* p = (const uint8_t*)s.buf->mapped;
  if (!p) return;
  std::memcpy(snap_.mirror.data(), p, kMirrorBytes);
  snap_.mirrorBase = s.base;
  snap_.windowOrigin = s.origin;
  const uint32_t* dirtyW = (const uint32_t*)(p + kDirtyOff);
  const uint32_t* occW = (const uint32_t*)(p + kOccOff);
  uint32_t active = 0;
  uint64_t total = 0;
  for (uint32_t i = 0; i < kNumChunks; i++) {
    snap_.dirtyFlags[i] = dirtyW[i] != 0 ? 1 : 0;
    active += snap_.dirtyFlags[i];
    snap_.occupancy[i] = occW[i] & 0xFFFFu;
    total += occW[i] & 0xFFFFu;
  }
  snap_.activeChunks = active;
  snap_.voxelTotal = total;
  std::memcpy(&snap_.worldHash, p + kHashOff, 4);
  std::memcpy(snap_.pick, p + kPickOff, 32);
  const uint32_t* supW = (const uint32_t*)(p + kSupportOff);
  for (uint32_t i = 0; i < kNumChunks; i++)
    snap_.supportFlags[i] = supW[i] != 0 ? 1 : 0;
  uint32_t pcounts[2];
  std::memcpy(pcounts, p + kPCountOff, 8);
  snap_.particleCount = std::min(pcounts[s.particleLivePage & 1], kParticleCap);
  snap_.tick = s.tick;
  snap_.valid = true;
}

// ctx.ProcessEvents()' replacement, at the same point in the frame. Polls, never
// blocks (barrier_graph §4.2).
void SimBackend::PollReadbacks() {
  be_.PollFences();
  for (auto& s : slots_) {
    if (!s.inFlight) continue;
    if (be_.FenceStatus(s.fence) != VK_SUCCESS) continue;
    PopulateSnapshot(s);
    // Release the borrow only AFTER reading: the retain is what kept this
    // handle meaning this submit (see RetainFence in rhi_vulkan.h).
    be_.ReleaseFence(s.fence);
    s.fence = VK_NULL_HANDLE;
    s.inFlight = false;
  }
}

// ---------------------------------------------------------------------------
// The full tick — test/support.cpp's SubmitTick against Vulkan resources.
// ---------------------------------------------------------------------------
bool SimBackend::SubmitTickFull(const TickInputs& in, std::string& err) {
  origin_ = in.windowOrigin;

  const uint32_t cellCount = std::min(in.cellCount, (uint32_t)kMaxCellOpsPerTick);
  const uint32_t spawnCount = std::min(in.spawnCount, (uint32_t)kMaxParticleSpawnsPerTick);
  // Identical derivation to SubmitTick: explosions and spawns force particles
  // active. A backend that computed this differently would record the particle
  // rows on one side and not the other, and the hashes would diverge — which is
  // the loud smoke's job to catch, so it is written the same way rather than
  // being made "obviously" right.
  const bool particlesActive =
      in.particlesActive || in.expCount > 0 || spawnCount > 0;

  TickParams tp{};
  tp.tick = in.tick;
  tp.seed = in.seed;
  tp.opsCount = in.opsCount;
  tp.hashEnable = in.hashEnable ? 1u : 0u;
  tp.expCount = in.expCount;
  tp.page = page_;
  tp.cellCount = cellCount;
  tp.spawnCount = spawnCount;
  tp.farCount = in.farCount;
  const Tuning& t = CurrentTuning();
  const uint32_t ticksPerDay =
      (uint32_t)(t.dayNight.cycleMinutes < 1 ? 1 : t.dayNight.cycleMinutes) * 60u * 30u;
  tp.dayPhase = DayPhaseForTick(in.tick, ticksPerDay, t.dayNight.freeze != 0,
                                (uint32_t)t.dayNight.freezePhase);
  tp.origin[0] = in.windowOrigin.x;
  tp.origin[1] = in.windowOrigin.y;
  tp.origin[2] = in.windowOrigin.z;
  be_.QueueWrite(res_.tickUBO, 0, &tp, sizeof(tp));

  if (in.opsCount) be_.QueueWrite(res_.opsBuf, 0, in.ops, in.opsCount * sizeof(BrushOp));
  if (in.expCount)
    be_.QueueWrite(res_.expOps, 0, in.exps, in.expCount * sizeof(ExplosionOp));
  if (cellCount) be_.QueueWrite(res_.cellOps, 0, in.cells, cellCount * sizeof(CellOp));
  if (spawnCount)
    be_.QueueWrite(res_.spawnOps, 0, in.spawns, spawnCount * sizeof(ParticleSpawn));
  if (particlesActive) {
    // The write page starts each tick empty; survivors + emissions repopulate.
    // This is the §7.7 partial write into a live buffer.
    uint32_t zero = 0;
    be_.QueueWrite(res_.particleCounts, (uint64_t)(1 - page_) * 4, &zero, 4);
  }

  RecordCtx cx{};
  cx.opsCount = in.opsCount;
  cx.cellCount = cellCount;
  cx.expCount = in.expCount;
  cx.spawnCount = spawnCount;
  cx.farCount = in.farCount;
  cx.hashEnable = in.hashEnable;
  cx.particlesActive = particlesActive;

  VkCommandBuffer cmd = be_.BeginCommands("tick");
  if (cmd == VK_NULL_HANDLE) {
    err = "could not begin the tick command buffer";
    return false;
  }
  Recorder rec(be_, Resolve(), mode_);
  rec.Begin(cmd);
  rec.RecordTable(pass::Table::Tick, cx);
  // EncodeFarFill rides the tick's command buffer after EncodeTick, exactly as
  // support.cpp does. Its row is C_FARCOUNT, so farCount 0 records nothing.
  rec.RecordTable(pass::Table::FarFill, cx);

  bool doCopy = false;
  if (in.wantReadback)
    doCopy = EncodeReadbacks(rec, in.playerChunkBase, 1 - page_, in.tick);

  rec.Finish();
  lastStats_ = rec.Stats();
  VkFence fence = be_.SubmitCommands(cmd, err);
  if (fence == VK_NULL_HANDLE) return false;

  // The page flip happens on the CPU immediately after submit (§4.4).
  page_ = 1 - page_;

  if (doCopy && lastSlot_ >= 0) {
    // §4.2: the slot BORROWS this submit's fence. Retain it so the pool cannot
    // recycle it out from under the poll — that recycle is the corruption
    // described in rhi_vulkan.h's RetainFence comment.
    ReadbackSlot& s = slots_[lastSlot_];
    s.fence = fence;
    s.inFlight = true;
    be_.RetainFence(fence);
    lastSlot_ = -1;
  }
  return true;
}

void SimBackend::WakeAll() {
  // dirty[page_] is the buffer the NEXT compact pass reads (dirtyIn), matching
  // Simulation::EncodeWakeAll exactly. 16 KB, Class A by the size rule.
  static const std::vector<uint32_t> ones(kNumChunks, 1u);
  be_.QueueWrite(res_.dirty[page_], 0, ones.data(), ones.size() * sizeof(uint32_t));
}

bool SimBackend::ReadBufferBlocking(pass::Buf src, uint64_t offset, void* out,
                                    uint64_t size, std::string& err) {
  if (size > kReadbackBytes) {
    // The small staging buffer is sized for hash/counts reads. Anything larger
    // borrows slot 0's buffer, which is idle whenever a blocking read is legal
    // (the sanctioned synchronous path is test-only and never overlaps a frame).
    if (!slots_[0].buf || size > slots_[0].buf->size) {
      err = "blocking read larger than any staging buffer";
      return false;
    }
  }
  Buffer* dst = (size > kReadbackBytes) ? slots_[0].buf : res_.readback;

  VkCommandBuffer cmd = be_.BeginCommands("blockingRead");
  if (cmd == VK_NULL_HANDLE) {
    err = "could not begin the blocking-read command buffer";
    return false;
  }
  Recorder rec(be_, Resolve(), mode_);
  rec.Begin(cmd);
  rec.CopyTracked(src, offset, dst, 0, size);
  rec.Finish();
  if (be_.SubmitCommands(cmd, err) == VK_NULL_HANDLE) return false;
  if (!be_.WaitIdle(err)) return false;
  if (!dst->mapped) {
    err = "staging buffer is not host-mapped";
    return false;
  }
  std::memcpy(out, dst->mapped, size);
  return true;
}

// ---------------------------------------------------------------------------
// Streaming — barrier_graph §4.3 (eviction) and §4.1/§4.7 (refill).
//
// THE ORDERING GUARANTEE, AND WHY ITS MECHANISM CHANGED.
//
// stream.cpp's comment says "submit BEFORE FillSlots writes: queue order makes
// the copy read the leaving plane's data". That claim stays TRUE here, but the
// reason is no longer "both are submits and submits are ordered": under the
// pending-upload queue the refill writes are DEFERRED and, when every slot hits
// the store, there is no refill submit at all. What actually orders them is
// that `EvictSlots` submits EAGERLY while `FillSlots` only ENQUEUES — so the
// copy-out is on the queue before the overwrite is even enqueued, let alone
// recorded. §4.3 step 2, and the stream.cpp comment is corrected in this same
// commit per CLAUDE.md's rule about docs that contradict code.
//
// The memory half of the dependency comes from §3.4's head-of-command-buffer
// global barrier in whatever command buffer later drains the fill.
// ---------------------------------------------------------------------------
bool SimBackend::EvictSlots(const uint32_t* slots, uint32_t count, EvictBatch& out,
                            std::string& err) {
  if (count == 0) { out = EvictBatch{}; return true; }
  if (count > kEvictBatch) count = kEvictBatch;

  Buffer* staging = nullptr;
  if (!stagingPool_.empty()) {
    staging = stagingPool_.back();
    stagingPool_.pop_back();
  } else {
    staging = be_.CreateBuffer((uint64_t)kEvictBatch * kChunkBytes,
                               rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                               "evictStaging");
    if (!staging) { err = "eviction staging allocation failed"; return false; }
  }

  VkCommandBuffer cmd = be_.BeginCommands("evict");
  if (cmd == VK_NULL_HANDLE) { err = "could not begin the evict command buffer"; return false; }
  Recorder rec(be_, Resolve(), mode_);
  rec.Begin(cmd);
  for (uint32_t i = 0; i < count; i++)
    rec.CopyTracked(pass::Buf::Voxels, (uint64_t)slots[i] * kChunkBytes, staging,
                    (uint64_t)i * kChunkBytes, kChunkBytes);
  rec.Finish();
  // EAGER submit. This is the whole ordering argument (§4.3 step 2).
  VkFence fence = be_.SubmitCommands(cmd, err);
  if (fence == VK_NULL_HANDLE) return false;
  be_.RetainFence(fence);

  out.staging = staging;
  out.fence = fence;
  out.count = count;
  return true;
}

bool SimBackend::CompleteEvict(EvictBatch& b, const void*& data, std::string& err) {
  data = nullptr;
  if (!b.staging) return true;
  // §4.3 step 5: CompleteOldest becomes a genuine fence wait, exactly as
  // instance.WaitAny(p.future, UINT64_MAX) blocks today.
  if (!be_.WaitFence(b.fence, err)) return false;
  be_.ReleaseFence(b.fence);
  data = b.staging->mapped;
  stagingPool_.push_back(b.staging);
  b = EvictBatch{};
  return true;
}

void SimBackend::FillSlotFromStore(uint32_t slot, const uint32_t* voxels,
                                   uint32_t occWord) {
  const uint32_t one = 1;
  // All four writes are DEFERRED and this function SUBMITS NOTHING — the case
  // §4.1 names as the one a naive port has no home for. They drain into
  // whichever command buffer is recorded next, which is normally the next tick.
  be_.QueueWrite(res_.voxels, (uint64_t)slot * kChunkBytes, voxels, kChunkBytes);
  be_.QueueWrite(res_.occupancy, (uint64_t)slot * 4, &occWord, 4);
  // Wake once: neighbours may have changed since this chunk was saved.
  be_.QueueWrite(res_.dirty[0], (uint64_t)slot * 4, &one, 4);
  be_.QueueWrite(res_.dirty[1], (uint64_t)slot * 4, &one, 4);
}

bool SimBackend::FillSlotsByGen(const uint32_t* slots, uint32_t count, uint32_t seed,
                                IVec3 windowOrigin, std::string& err) {
  if (count == 0) return true;
  origin_ = windowOrigin;
  be_.QueueWrite(res_.genList, 0, slots, (uint64_t)count * 4);
  TickParams tp{};
  tp.seed = seed;
  tp.genCount = count;
  tp.origin[0] = windowOrigin.x;
  tp.origin[1] = windowOrigin.y;
  tp.origin[2] = windowOrigin.z;
  // tickUBO LAST-WRITE-WINS (§4.1/§4.7). If this submit happens, the queue
  // drains here and this TickParams is what genChunk reads. If a tick's own
  // tickUBO write is issued later, the tick's write lands later and wins —
  // which is the same ordering WebGPU produces today.
  be_.QueueWrite(res_.tickUBO, 0, &tp, sizeof(tp));

  RecordCtx cx{};
  cx.genCount = count;
  return RunTable(pass::Table::GenList, cx, err);
}

void SimBackend::UploadChunk(uint32_t slot, const uint32_t* voxels) {
  be_.QueueWrite(res_.voxels, (uint64_t)slot * kChunkBytes, voxels, kChunkBytes);
}

void SimBackend::UploadDirtyWord(uint32_t slot, uint32_t value) {
  be_.QueueWrite(res_.dirty[0], (uint64_t)slot * 4, &value, 4);
  be_.QueueWrite(res_.dirty[1], (uint64_t)slot * 4, &value, 4);
}

bool SimBackend::SubmitLoadReset(uint32_t seed, std::string& err) {
  TickParams tp{};
  tp.seed = seed;
  tp.origin[0] = origin_.x;
  tp.origin[1] = origin_.y;
  tp.origin[2] = origin_.z;
  be_.QueueWrite(res_.tickUBO, 0, &tp, sizeof(tp));
  RecordCtx cx{};
  return RunTable(pass::Table::LoadReset, cx, err);
}

void SimBackend::Shutdown() {
  // Release every borrowed fence before the backend tears the pool down, so the
  // retain map is empty and nothing is double-destroyed.
  for (auto& s : slots_) {
    if (s.inFlight && s.fence != VK_NULL_HANDLE) be_.ReleaseFence(s.fence);
    s.fence = VK_NULL_HANDLE;
    s.inFlight = false;
  }
  be_.Shutdown();
}

}  // namespace vk
