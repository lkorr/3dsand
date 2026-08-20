#include "sim/simulation.h"

#include <cstdio>

#include "gpu/resources.h"

static constexpr uint32_t kPassStride = 256;  // min uniform dynamic-offset alignment

bool Simulation::Init(const wgpu::Device& device, World& world,
                      const std::vector<MaterialDef>& mats,
                      const std::vector<ReactionGpu>& reactions,
                      const std::string& shaderDir) {
  world_ = &world;
  device_ = device;
  shaderDir_ = shaderDir;
  wgpu::Queue queue = device.GetQueue();

  materialBuf_ = CreateBuffer(device, sizeof(MaterialGpu) * 4096,
                              wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst,
                              "materials");
  reactionBuf_ = CreateBuffer(device, sizeof(ReactionGpu) * kMaxReactions,
                              wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst,
                              "reactions");
  UploadTables(queue, mats, reactions);

  // 27 color-phase slices x 2 gravity substeps (54 total)
  {
    std::vector<uint32_t> phases(54 * kPassStride / 4, 0);
    for (uint32_t k = 0; k < 54; k++) {
      uint32_t* p = &phases[k * kPassStride / 4];
      uint32_t c = k % 27;
      p[0] = c % 3;
      p[1] = (c / 3) % 3;
      p[2] = c / 9;
      p[3] = k / 27;  // substep
    }
    queue.WriteBuffer(world_->passUBO, 0, phases.data(), phases.size() * 4);
  }

  // ---- bind group layouts ----
  {
    auto entry = [](uint32_t binding, wgpu::BufferBindingType type,
                    bool dynamic = false) {
      wgpu::BindGroupLayoutEntry e{};
      e.binding = binding;
      e.visibility = wgpu::ShaderStage::Compute;
      e.buffer.type = type;
      e.buffer.hasDynamicOffset = dynamic;
      return e;
    };
    using T = wgpu::BufferBindingType;
    wgpu::BindGroupLayoutEntry entries[] = {
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
        entry(10, T::Uniform),         // RenderParams (pick ray)
        entry(11, T::ReadOnlyStorage), // reactions
        entry(12, T::Storage),         // dirtyList (compact writes, step reads)
        entry(13, T::Storage),         // dispatch args (compact writes)
        entry(14, T::ReadOnlyStorage), // exact-cell ops (island removal)
        entry(15, T::Storage),         // support-loss flags (sim_step writes)
        entry(16, T::ReadOnlyStorage), // genList (worldgen streaming slots)
    };
    wgpu::BindGroupLayoutDescriptor d{};
    d.entryCount = std::size(entries);
    d.entries = entries;
    simBGL_ = device.CreateBindGroupLayout(&d);

    // slim group 0 (bindings 0..4 only) for particle/explosion pipelines:
    // pairing the full simBGL_ with particleBGL_ would exceed the
    // 16-storage-buffer per-stage layout limit
    wgpu::BindGroupLayoutEntry sentries[] = {
        entry(0, T::Storage),          // voxels
        entry(1, T::Storage),          // dirtyIn
        entry(2, T::Storage),          // dirtyOut
        entry(3, T::ReadOnlyStorage),  // materials
        entry(4, T::Uniform),          // TickParams
    };
    d.entryCount = std::size(sentries);
    d.entries = sentries;
    simSlimBGL_ = device.CreateBindGroupLayout(&d);

    // group 1: particle machinery (explode/integrate/resolve/args kernels)
    wgpu::BindGroupLayoutEntry pentries[] = {
        entry(0, T::Storage),          // particles read page
        entry(1, T::Storage),          // particles write page
        entry(2, T::Storage),          // counts
        entry(3, T::Storage),          // claim hash
        entry(4, T::Storage),          // pArgsStage
        entry(5, T::ReadOnlyStorage),  // explosion ops
        entry(6, T::Storage),          // explosion destruction scratch
        entry(7, T::ReadOnlyStorage),  // CPU particle spawns (debris shatter)
    };
    d.entryCount = std::size(pentries);
    d.entries = pentries;
    particleBGL_ = device.CreateBindGroupLayout(&d);
  }
  {
    auto entry = [](uint32_t binding, wgpu::BufferBindingType type,
                    wgpu::ShaderStage vis) {
      wgpu::BindGroupLayoutEntry e{};
      e.binding = binding;
      e.visibility = vis;
      e.buffer.type = type;
      return e;
    };
    using T = wgpu::BufferBindingType;
    using S = wgpu::ShaderStage;
    wgpu::BindGroupLayoutEntry entries[] = {
        entry(0, T::ReadOnlyStorage, S::Fragment),               // voxels
        entry(1, T::ReadOnlyStorage, S::Fragment),               // occupancy
        entry(2, T::ReadOnlyStorage, S::Fragment | S::Vertex),   // materials
        entry(3, T::Uniform, S::Fragment | S::Vertex),           // RenderParams
        entry(4, T::ReadOnlyStorage, S::Fragment),               // farVox
        entry(5, T::ReadOnlyStorage, S::Fragment),               // farOcc
        entry(6, T::Uniform, S::Fragment),                       // FarParams
    };
    wgpu::BindGroupLayoutDescriptor d{};
    d.entryCount = std::size(entries);
    d.entries = entries;
    renderBGL_ = device.CreateBindGroupLayout(&d);

    wgpu::BindGroupLayoutEntry pentries[] = {
        entry(0, T::ReadOnlyStorage, S::Vertex),  // particles (live page)
        entry(1, T::ReadOnlyStorage, S::Vertex),  // sprites
        entry(2, T::ReadOnlyStorage, S::Vertex),  // debris body voxel instances
        entry(3, T::ReadOnlyStorage, S::Vertex),  // debris body transforms
    };
    d.entryCount = std::size(pentries);
    d.entries = pentries;
    renderPartBGL_ = device.CreateBindGroupLayout(&d);
  }
  {
    wgpu::PipelineLayoutDescriptor d{};
    d.bindGroupLayoutCount = 1;
    d.bindGroupLayouts = &simBGL_;
    simPL_ = device.CreatePipelineLayout(&d);

    wgpu::BindGroupLayout simGroups[] = {simSlimBGL_, particleBGL_};
    d.bindGroupLayoutCount = 2;
    d.bindGroupLayouts = simGroups;
    simPL2_ = device.CreatePipelineLayout(&d);

    wgpu::BindGroupLayout renderGroups[] = {renderBGL_, renderPartBGL_};
    d.bindGroupLayoutCount = 2;
    d.bindGroupLayouts = renderGroups;
    renderPL_ = device.CreatePipelineLayout(&d);
  }
  {
    // far-field cascade fill: slim sim group 0 (the `far` entry statically
    // uses only materials + TickParams there) + far buffers as group 1
    auto entry = [](uint32_t binding, wgpu::BufferBindingType type) {
      wgpu::BindGroupLayoutEntry e{};
      e.binding = binding;
      e.visibility = wgpu::ShaderStage::Compute;
      e.buffer.type = type;
      return e;
    };
    using T = wgpu::BufferBindingType;
    wgpu::BindGroupLayoutEntry entries[] = {
        entry(0, T::Storage),          // farVox
        entry(1, T::Storage),          // farOcc
        entry(2, T::ReadOnlyStorage),  // farList
        entry(3, T::Uniform),          // FarParams
    };
    wgpu::BindGroupLayoutDescriptor d{};
    d.entryCount = std::size(entries);
    d.entries = entries;
    farBGL_ = device.CreateBindGroupLayout(&d);

    wgpu::BindGroupLayout farGroups[] = {simSlimBGL_, farBGL_};
    wgpu::PipelineLayoutDescriptor pd{};
    pd.bindGroupLayoutCount = 2;
    pd.bindGroupLayouts = farGroups;
    farPL_ = device.CreatePipelineLayout(&pd);
  }

  // ---- bind groups ----
  auto b = [](uint32_t binding, const wgpu::Buffer& buf, uint64_t size = 0) {
    wgpu::BindGroupEntry e{};
    e.binding = binding;
    e.buffer = buf;
    e.size = size ? size : wgpu::kWholeSize;
    return e;
  };
  for (int page = 0; page < 2; page++) {
    wgpu::BindGroupEntry entries[] = {
        b(0, world_->voxels),
        b(1, world_->dirty[page]),
        b(2, world_->dirty[1 - page]),
        b(3, materialBuf_),
        b(4, world_->tickUBO),
        b(5, world_->passUBO, 16),  // dynamic-offset window
        b(6, world_->opsBuf),
        b(7, world_->occupancy),
        b(8, world_->hash),
        b(9, world_->pick),
        b(10, world_->renderUBO),
        b(11, reactionBuf_),
        b(12, world_->dirtyList),
        b(13, world_->argsStage),
        b(14, world_->cellOps),
        b(15, world_->support),
        b(16, world_->genList),
    };
    wgpu::BindGroupDescriptor d{};
    d.layout = simBGL_;
    d.entryCount = std::size(entries);
    d.entries = entries;
    simBG_[page] = device.CreateBindGroup(&d);

    wgpu::BindGroupEntry sentries[] = {
        b(0, world_->voxels),
        b(1, world_->dirty[page]),
        b(2, world_->dirty[1 - page]),
        b(3, materialBuf_),
        b(4, world_->tickUBO),
    };
    d.layout = simSlimBGL_;
    d.entryCount = std::size(sentries);
    d.entries = sentries;
    simSlimBG_[page] = device.CreateBindGroup(&d);

    wgpu::BindGroupEntry pentries[] = {
        b(0, world_->particles[page]),
        b(1, world_->particles[1 - page]),
        b(2, world_->particleCounts),
        b(3, world_->claim),
        b(4, world_->pArgsStage),
        b(5, world_->expOps),
        b(6, world_->expMask),
        b(7, world_->spawnOps),
    };
    d.layout = particleBGL_;
    d.entryCount = std::size(pentries);
    d.entries = pentries;
    particleBG_[page] = device.CreateBindGroup(&d);

    wgpu::BindGroupEntry rpentries[] = {
        b(0, world_->particles[page]),
        b(1, world_->sprites),
        b(2, world_->bodyInstances),
        b(3, world_->bodyXforms),
    };
    d.layout = renderPartBGL_;
    d.entryCount = std::size(rpentries);
    d.entries = rpentries;
    renderPartBG_[page] = device.CreateBindGroup(&d);
  }
  {
    wgpu::BindGroupEntry entries[] = {
        b(0, world_->voxels),
        b(1, world_->occupancy),
        b(2, materialBuf_),
        b(3, world_->renderUBO),
        b(4, world_->farVox),
        b(5, world_->farOcc),
        b(6, world_->farUBO),
    };
    wgpu::BindGroupDescriptor d{};
    d.layout = renderBGL_;
    d.entryCount = std::size(entries);
    d.entries = entries;
    renderBG_ = device.CreateBindGroup(&d);
  }
  {
    wgpu::BindGroupEntry entries[] = {
        b(0, world_->farVox),
        b(1, world_->farOcc),
        b(2, world_->farList),
        b(3, world_->farUBO),
    };
    wgpu::BindGroupDescriptor d{};
    d.layout = farBGL_;
    d.entryCount = std::size(entries);
    d.entries = entries;
    farBG_ = device.CreateBindGroup(&d);
  }

  std::string err;
  if (!BuildPipelines(device, &err)) {
    std::fprintf(stderr, "pipeline build failed:\n%s\n", err.c_str());
    return false;
  }
  return true;
}

void Simulation::UploadTables(const wgpu::Queue& queue,
                              const std::vector<MaterialDef>& mats,
                              const std::vector<ReactionGpu>& reactions) {
  std::vector<MaterialGpu> table(4096, MaterialGpu{});
  for (size_t i = 0; i < mats.size() && i < 4096; i++) table[i] = mats[i].gpu;
  queue.WriteBuffer(materialBuf_, 0, table.data(), table.size() * sizeof(MaterialGpu));

  std::vector<ReactionGpu> rtable(kMaxReactions, ReactionGpu{});
  for (size_t i = 0; i < reactions.size() && i < kMaxReactions; i++)
    rtable[i] = reactions[i];
  queue.WriteBuffer(reactionBuf_, 0, rtable.data(), rtable.size() * sizeof(ReactionGpu));
}

bool Simulation::BuildPipelines(const wgpu::Device& device, std::string* err) {
  auto mod = [&](const char* name) { return LoadShader(device, shaderDir_, name); };
  wgpu::ShaderModule mWorldgen = mod("worldgen.wgsl");
  wgpu::ShaderModule mMutate = mod("sim_mutate.wgsl");
  wgpu::ShaderModule mCompact = mod("sim_compact.wgsl");
  wgpu::ShaderModule mStep = mod("sim_step.wgsl");
  wgpu::ShaderModule mOcc = mod("sim_occupancy.wgsl");
  wgpu::ShaderModule mPick = mod("sim_pick.wgsl");
  wgpu::ShaderModule mExplode = mod("sim_explode.wgsl");
  wgpu::ShaderModule mParticle = mod("sim_particle.wgsl");
  wgpu::ShaderModule mRay = mod("raymarch.wgsl");
  wgpu::ShaderModule mDebris = mod("debris.wgsl");
  if (!mWorldgen || !mMutate || !mCompact || !mStep || !mOcc || !mPick ||
      !mExplode || !mParticle || !mRay || !mDebris) {
    if (err) *err = "shader file read failure";
    return false;
  }

  worldgen_ = MakeComputePipeline(device, simPL_, mWorldgen, "main", "worldgen");
  worldgenList_ = MakeComputePipeline(device, simPL_, mWorldgen, "list", "worldgenList");
  farFill_ = MakeComputePipeline(device, farPL_, mWorldgen, "far", "farFill");
  mutate_ = MakeComputePipeline(device, simPL_, mMutate, "main", "mutate");
  mutateCells_ = MakeComputePipeline(device, simPL_, mMutate, "cells", "mutateCells");
  compact_ = MakeComputePipeline(device, simPL_, mCompact, "main", "compact");
  compactNext_ = MakeComputePipeline(device, simPL_, mCompact, "mainNext", "compactNext");
  step_ = MakeComputePipeline(device, simPL_, mStep, "main", "step");
  occupancy_ = MakeComputePipeline(device, simPL_, mOcc, "main", "occupancy");
  occupancyDirty_ = MakeComputePipeline(device, simPL_, mOcc, "mainDirty", "occupancyDirty");
  pick_ = MakeComputePipeline(device, simPL_, mPick, "main", "pick");

  explodeMark_ = MakeComputePipeline(device, simPL2_, mExplode, "mark", "explodeMark");
  explodeApply_ = MakeComputePipeline(device, simPL2_, mExplode, "apply", "explodeApply");
  pArgs1_ = MakeComputePipeline(device, simPL2_, mParticle, "args1", "pArgs1");
  pSpawn_ = MakeComputePipeline(device, simPL2_, mParticle, "spawn", "pSpawn");
  pIntegrate_ = MakeComputePipeline(device, simPL2_, mParticle, "integrate", "pIntegrate");
  pArgs2_ = MakeComputePipeline(device, simPL2_, mParticle, "args2", "pArgs2");
  pResolve_ = MakeComputePipeline(device, simPL2_, mParticle, "resolve", "pResolve");

  raymarchModule_ = mRay;
  debrisModule_ = mDebris;
  targetFormat_ = wgpu::TextureFormat::Undefined;  // force render pipeline rebuild
  return true;
}

bool Simulation::ReloadShaders(const wgpu::Device& device, const wgpu::Instance& instance) {
  device.PushErrorScope(wgpu::ErrorFilter::Validation);
  std::string err;
  bool built = BuildPipelines(device, &err);
  bool hadError = false;
  wgpu::Future f = device.PopErrorScope(
      wgpu::CallbackMode::WaitAnyOnly,
      [&](wgpu::PopErrorScopeStatus, wgpu::ErrorType type, wgpu::StringView msg) {
        if (type != wgpu::ErrorType::NoError) {
          hadError = true;
          std::fprintf(stderr, "shader reload error: %.*s\n", (int)msg.length, msg.data);
        }
      });
  instance.WaitAny(f, UINT64_MAX);
  return built && !hadError;
}

void Simulation::EncodeWorldgen(const wgpu::CommandEncoder& enc) {
  page_ = 0;
  enc.ClearBuffer(world_->dirty[0], 0, wgpu::kWholeSize);
  enc.ClearBuffer(world_->dirty[1], 0, wgpu::kWholeSize);
  enc.ClearBuffer(world_->hash, 0, wgpu::kWholeSize);
  enc.ClearBuffer(world_->support, 0, wgpu::kWholeSize);
  enc.ClearBuffer(world_->particleCounts, 0, wgpu::kWholeSize);
  enc.ClearBuffer(world_->claim, 0, wgpu::kWholeSize);
  enc.ClearBuffer(world_->drawArgs, 0, wgpu::kWholeSize);  // no ghost particles
  wgpu::ComputePassEncoder pass = enc.BeginComputePass();
  uint32_t off = 0;
  pass.SetBindGroup(0, simBG_[page_], 1, &off);
  // one workgroup per slot chunk; occupancy + dirty flags computed in-kernel
  pass.SetPipeline(worldgen_);
  pass.DispatchWorkgroups(kNumChunks, 1, 1);
  pass.End();
}

void Simulation::EncodeGenList(const wgpu::CommandEncoder& enc, uint32_t count) {
  if (count == 0) return;
  wgpu::ComputePassEncoder pass = enc.BeginComputePass();
  uint32_t off = 0;
  pass.SetBindGroup(0, simBG_[page_], 1, &off);
  pass.SetPipeline(worldgenList_);
  pass.DispatchWorkgroups(count, 1, 1);
  pass.End();
}

void Simulation::EncodeFarFill(const wgpu::CommandEncoder& enc, uint32_t count) {
  if (count == 0) return;
  wgpu::ComputePassEncoder pass = enc.BeginComputePass();
  pass.SetBindGroup(0, simSlimBG_[page_]);
  pass.SetBindGroup(1, farBG_);
  pass.SetPipeline(farFill_);
  pass.DispatchWorkgroups(count, 1, 1);
  pass.End();
}

void Simulation::EncodeLoadReset(const wgpu::CommandEncoder& enc) {
  page_ = 0;
  enc.ClearBuffer(world_->hash, 0, wgpu::kWholeSize);
  enc.ClearBuffer(world_->support, 0, wgpu::kWholeSize);
  enc.ClearBuffer(world_->particleCounts, 0, wgpu::kWholeSize);
  enc.ClearBuffer(world_->claim, 0, wgpu::kWholeSize);
  enc.ClearBuffer(world_->drawArgs, 0, wgpu::kWholeSize);
  wgpu::ComputePassEncoder pass = enc.BeginComputePass();
  uint32_t off = 0;
  pass.SetBindGroup(0, simBG_[page_], 1, &off);
  pass.SetPipeline(occupancy_);
  pass.DispatchWorkgroups(kNumChunks, 1, 1);
  pass.End();
}

void Simulation::EncodeHashOnly(const wgpu::CommandEncoder& enc) {
  enc.ClearBuffer(world_->hash, 0, wgpu::kWholeSize);
  wgpu::ComputePassEncoder pass = enc.BeginComputePass();
  uint32_t off = 0;
  pass.SetBindGroup(0, simBG_[page_], 1, &off);
  pass.SetPipeline(occupancy_);
  pass.DispatchWorkgroups(kNumChunks, 1, 1);
  pass.End();
}

void Simulation::EncodeTick(const wgpu::CommandEncoder& enc, uint32_t opsCount,
                            bool hashEnable, uint32_t expCount, bool particlesActive,
                            uint32_t cellCount, uint32_t spawnCount) {
  enc.ClearBuffer(world_->dirty[1 - page_], 0, wgpu::kWholeSize);
  enc.ClearBuffer(world_->argsStage, 0, wgpu::kWholeSize);
  if (particlesActive) enc.ClearBuffer(world_->claim, 0, wgpu::kWholeSize);
  if (expCount > 0) enc.ClearBuffer(world_->expMask, 0, wgpu::kWholeSize);
  if (hashEnable) enc.ClearBuffer(world_->hash, 0, wgpu::kWholeSize);

  uint32_t off = 0;

  // Prep pass: mutate + explode + compact (compact writes argsStage + dirtyList).
  {
    wgpu::ComputePassEncoder prep = enc.BeginComputePass();
    if (opsCount > 0) {
      prep.SetBindGroup(0, simBG_[page_], 1, &off);
      prep.SetPipeline(mutate_);
      prep.DispatchWorkgroups(4 * opsCount, 4, 4);
    }
    if (cellCount > 0) {
      prep.SetBindGroup(0, simBG_[page_], 1, &off);
      prep.SetPipeline(mutateCells_);
      prep.DispatchWorkgroups((cellCount + 63) / 64, 1, 1);
    }
    if (expCount > 0) {
      prep.SetBindGroup(0, simSlimBG_[page_]);
      prep.SetBindGroup(1, particleBG_[page_]);
      prep.SetPipeline(explodeMark_);
      prep.DispatchWorkgroups(kExplosionWg * expCount, kExplosionWg, kExplosionWg);
      prep.SetPipeline(explodeApply_);
      prep.DispatchWorkgroups(kExplosionWg * expCount, kExplosionWg, kExplosionWg);
    }
    if (spawnCount > 0) {
      // CPU particle spawns (debris shatter) append to the read page here,
      // before args1 sizes the integrate dispatch — same page ejecta uses
      prep.SetBindGroup(0, simSlimBG_[page_]);
      prep.SetBindGroup(1, particleBG_[page_]);
      prep.SetPipeline(pSpawn_);
      prep.DispatchWorkgroups((spawnCount + 63) / 64, 1, 1);
    }
    // compact dirtyIn -> dense chunk list + indirect args (after mutate/explode
    // so freshly touched chunks simulate this tick)
    prep.SetBindGroup(0, simBG_[page_], 1, &off);
    prep.SetPipeline(compact_);
    prep.DispatchWorkgroups(kNumChunks / 64, 1, 1);
    prep.End();
  }

  // Feed the indirect-only args buffer (never bound in any bind group; Dawn
  // forbids indirect + bound-writable usage of one buffer in the same pass).
  enc.CopyBufferToBuffer(world_->argsStage, 0, world_->dispatchArgs, 0, 12);

  {
    wgpu::ComputePassEncoder pass = enc.BeginComputePass();
    pass.SetBindGroup(0, simBG_[page_], 1, &off);

    // 27 colors x 2 gravity substeps, one workgroup per dirty chunk. A settled
    // world dispatches zero workgroups here (DESIGN.md §11).
    pass.SetPipeline(step_);
    for (uint32_t k = 0; k < 54; k++) {
      uint32_t offset = k * kPassStride;
      pass.SetBindGroup(0, simBG_[page_], 1, &offset);
      pass.DispatchWorkgroupsIndirect(world_->dispatchArgs, 0);
    }
    pass.End();
  }

  // ---- particles: integrate (read page) -> resolve claims (write page) ----
  // Runs after the CA so flights see settled ground. Grid writes land in
  // dirtyOut, which the occupancy update below already covers.
  if (particlesActive) {
    {
      wgpu::ComputePassEncoder pass = enc.BeginComputePass();
      pass.SetBindGroup(0, simSlimBG_[page_]);
      pass.SetBindGroup(1, particleBG_[page_]);
      pass.SetPipeline(pArgs1_);
      pass.DispatchWorkgroups(1, 1, 1);
      pass.End();
    }
    enc.CopyBufferToBuffer(world_->pArgsStage, 16, world_->pDispatchArgs, 0, 12);
    {
      wgpu::ComputePassEncoder pass = enc.BeginComputePass();
      pass.SetBindGroup(0, simSlimBG_[page_]);
      pass.SetBindGroup(1, particleBG_[page_]);
      pass.SetPipeline(pIntegrate_);
      pass.DispatchWorkgroupsIndirect(world_->pDispatchArgs, 0);
      pass.SetPipeline(pArgs2_);
      pass.DispatchWorkgroups(1, 1, 1);
      pass.End();
    }
    enc.CopyBufferToBuffer(world_->pArgsStage, 16, world_->pDispatchArgs, 0, 12);
    enc.CopyBufferToBuffer(world_->pArgsStage, 0, world_->drawArgs, 0, 16);
    {
      wgpu::ComputePassEncoder pass = enc.BeginComputePass();
      pass.SetBindGroup(0, simSlimBG_[page_]);
      pass.SetBindGroup(1, particleBG_[page_]);
      pass.SetPipeline(pResolve_);
      pass.DispatchWorkgroupsIndirect(world_->pDispatchArgs, 0);
      pass.End();
    }
  }

  if (hashEnable) {
    // hash ticks need the whole-world scan anyway; it also refreshes occupancy
    wgpu::ComputePassEncoder pass = enc.BeginComputePass();
    pass.SetBindGroup(0, simBG_[page_], 1, &off);
    pass.SetPipeline(occupancy_);
    pass.DispatchWorkgroups(kNumChunks, 1, 1);
    pass.SetPipeline(pick_);
    pass.DispatchWorkgroups(1, 1, 1);
    pass.End();
  } else {
    // occupancy update over only the chunks written this tick: compact the
    // dirtyOut flags (superset of every voxel write) and dispatch indirect
    enc.ClearBuffer(world_->argsStage, 0, wgpu::kWholeSize);
    {
      wgpu::ComputePassEncoder pass = enc.BeginComputePass();
      pass.SetBindGroup(0, simBG_[page_], 1, &off);
      pass.SetPipeline(compactNext_);
      pass.DispatchWorkgroups(kNumChunks / 64, 1, 1);
      pass.End();
    }
    enc.CopyBufferToBuffer(world_->argsStage, 0, world_->dispatchArgs, 0, 12);
    {
      wgpu::ComputePassEncoder pass = enc.BeginComputePass();
      pass.SetBindGroup(0, simBG_[page_], 1, &off);
      pass.SetPipeline(occupancyDirty_);
      pass.DispatchWorkgroupsIndirect(world_->dispatchArgs, 0);
      pass.SetPipeline(pick_);
      pass.DispatchWorkgroups(1, 1, 1);
      pass.End();
    }
  }
}

void Simulation::EnsureDepth(uint32_t width, uint32_t height) {
  if (depthView_ && depthW_ == width && depthH_ == height) return;
  depthW_ = width;
  depthH_ = height;
  wgpu::TextureDescriptor d{};
  d.size = {width, height, 1};
  d.format = kDepthFormat;
  d.usage = wgpu::TextureUsage::RenderAttachment;
  d.label = "depth";
  depthTex_ = device_.CreateTexture(&d);
  depthView_ = depthTex_.CreateView();
}

void Simulation::EnsureRenderPipelines(wgpu::TextureFormat format) {
  if (format == targetFormat_) return;
  targetFormat_ = format;

  wgpu::DepthStencilState dsAlways{};
  dsAlways.format = kDepthFormat;
  dsAlways.depthWriteEnabled = true;
  dsAlways.depthCompare = wgpu::CompareFunction::Always;

  wgpu::DepthStencilState dsTest{};
  dsTest.format = kDepthFormat;
  dsTest.depthWriteEnabled = true;
  dsTest.depthCompare = wgpu::CompareFunction::GreaterEqual;  // reversed-Z

  wgpu::ColorTargetState ct{};
  ct.format = format;

  {
    wgpu::FragmentState fs{};
    fs.module = raymarchModule_;
    fs.entryPoint = "fs";
    fs.targetCount = 1;
    fs.targets = &ct;
    wgpu::RenderPipelineDescriptor d{};
    d.layout = renderPL_;
    d.vertex.module = raymarchModule_;
    d.vertex.entryPoint = "vs";
    d.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    d.depthStencil = &dsAlways;
    d.fragment = &fs;
    d.label = "raymarch";
    raymarch_ = device_.CreateRenderPipeline(&d);
  }
  {
    wgpu::FragmentState fs{};
    fs.module = debrisModule_;
    fs.entryPoint = "fs";
    fs.targetCount = 1;
    fs.targets = &ct;
    wgpu::RenderPipelineDescriptor d{};
    d.layout = renderPL_;
    d.vertex.module = debrisModule_;
    d.vertex.entryPoint = "vsParticle";
    d.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    d.primitive.cullMode = wgpu::CullMode::None;
    d.depthStencil = &dsTest;
    d.fragment = &fs;
    d.label = "particleDraw";
    particleDraw_ = device_.CreateRenderPipeline(&d);

    d.vertex.entryPoint = "vsSprite";
    d.label = "spriteDraw";
    spriteDraw_ = device_.CreateRenderPipeline(&d);

    d.vertex.entryPoint = "vsBody";
    d.label = "bodyDraw";
    bodyDraw_ = device_.CreateRenderPipeline(&d);
  }
}

wgpu::RenderPassEncoder Simulation::BeginRenderPass(const wgpu::CommandEncoder& enc,
                                                    const wgpu::TextureView& target,
                                                    wgpu::TextureFormat format,
                                                    uint32_t width, uint32_t height) {
  EnsureRenderPipelines(format);
  EnsureDepth(width, height);

  wgpu::RenderPassColorAttachment ca{};
  ca.view = target;
  ca.loadOp = wgpu::LoadOp::Clear;
  ca.storeOp = wgpu::StoreOp::Store;
  ca.clearValue = {0.1, 0.15, 0.25, 1.0};
  wgpu::RenderPassDepthStencilAttachment da{};
  da.view = depthView_;
  da.depthLoadOp = wgpu::LoadOp::Clear;
  da.depthStoreOp = wgpu::StoreOp::Store;
  da.depthClearValue = 0.0f;  // reversed-Z: clear to far
  wgpu::RenderPassDescriptor d{};
  d.colorAttachmentCount = 1;
  d.colorAttachments = &ca;
  d.depthStencilAttachment = &da;
  return enc.BeginRenderPass(&d);
}

void Simulation::DrawWorld(const wgpu::RenderPassEncoder& pass) {
  pass.SetPipeline(raymarch_);
  pass.SetBindGroup(0, renderBG_);
  pass.SetBindGroup(1, renderPartBG_[page_]);
  pass.Draw(3);
}

void Simulation::DrawParticles(const wgpu::RenderPassEncoder& pass) {
  pass.SetPipeline(particleDraw_);
  pass.SetBindGroup(0, renderBG_);
  pass.SetBindGroup(1, renderPartBG_[page_]);
  pass.DrawIndirect(world_->drawArgs, 0);
}

void Simulation::DrawSprites(const wgpu::RenderPassEncoder& pass, uint32_t count) {
  if (count == 0) return;
  pass.SetPipeline(spriteDraw_);
  pass.SetBindGroup(0, renderBG_);
  pass.SetBindGroup(1, renderPartBG_[page_]);
  pass.Draw(36, count);
}

void Simulation::DrawBodies(const wgpu::RenderPassEncoder& pass, uint32_t voxInstances) {
  if (voxInstances == 0) return;
  pass.SetPipeline(bodyDraw_);
  pass.SetBindGroup(0, renderBG_);
  pass.SetBindGroup(1, renderPartBG_[page_]);
  pass.Draw(36, voxInstances);
}

void Simulation::FlipPage() { page_ = 1 - page_; }
