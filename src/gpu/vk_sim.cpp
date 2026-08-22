// vk_sim.cpp — Vulkan-side sim resources and the recorded paths that drive them.
//
// Read vk_sim.h's header for why this exists as a second set of declarations.
// The buffer sizes, layout bindings and bind-group contents below are copied
// from sim/world.cpp's World::Init and sim/simulation.cpp's Simulation::Init
// and MUST match them; --vk-smoke's hash comparison against Dawn is what proves
// they do, and a mismatch shows up there rather than as something a reader has
// to spot.

#include "gpu/vk_sim.h"

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

void SimBackend::Shutdown() { be_.Shutdown(); }

}  // namespace vk
