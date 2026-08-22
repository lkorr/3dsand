#include "sim/simulation.h"

#include <algorithm>
#include <cstdio>

#include "gpu/resources.h"

static constexpr uint32_t kPassStride = 256;  // min uniform dynamic-offset alignment

bool Simulation::Init(const rhi::Device& device, World& world,
                      const std::vector<MaterialDef>& mats,
                      const std::vector<ReactionGpu>& reactions,
                      const MicroSet& micro,
                      const std::string& shaderDir) {
  world_ = &world;
  device_ = device;
  shaderDir_ = shaderDir;
  rhi::Queue queue = device.GetQueue();

  materialBuf_ = CreateBuffer(device, sizeof(MaterialGpu) * 4096,
                              rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                              "materials");
  reactionBuf_ = CreateBuffer(device, sizeof(ReactionGpu) * kMaxReactions,
                              rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                              "reactions");
  UploadTables(queue, mats, reactions);

  // Static micro-detail (render-only — sim/microvox.h). Both buffers are bound
  // ONLY to the raymarch pipeline: they are render data, and a sim shader that
  // could read them would put the renderer on the sim's dependency graph.
  microTableBuf_ = CreateBuffer(device, sizeof(MicroBrickGpu) * kMaterialSlots,
                                rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                                "microBricks");
  microPoolBuf_ = CreateBuffer(device, (uint64_t)kMicroPoolWords * 4,
                               rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                               "microPool");
  UploadMicro(queue, micro);

  // Dynamic micro BODIES (PLAN §C). Sized here, filled by UploadMicroBodies
  // once the mob defs have loaded — mobs load after the Simulation exists, and
  // an empty table is a perfectly valid "no micro bodies" state.
  mbModelBuf_ = CreateBuffer(device, sizeof(MicroBodyModelGpu) * kMaxMicroBodyModels,
                             rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                             "microBodyModels");
  mbPoolBuf_ = CreateBuffer(device, (uint64_t)kMicroBodyPoolWordsWorld * 4,
                            rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                            "microBodyPool");
  mbInstBuf_ = CreateBuffer(device, sizeof(MicroBodyInstGpu) * kMaxBodySlots,
                            rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                            "microBodyInsts");
  UploadMicroBodies(queue, MicroBodySet{});

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
    auto entry = [](uint32_t binding, rhi::BufferBindingType type,
                    bool dynamic = false) {
      rhi::BindGroupLayoutEntry e{};
      e.binding = binding;
      e.visibility = rhi::ShaderStage::Compute;
      e.type = type;
      e.hasDynamicOffset = dynamic;
      return e;
    };
    using T = rhi::BufferBindingType;
    rhi::BindGroupLayoutEntry entries[] = {
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
    simBGL_ = device.CreateBindGroupLayout(entries, std::size(entries));

    // slim group 0 (bindings 0..4 only) for particle/explosion pipelines:
    // pairing the full simBGL_ with particleBGL_ would exceed the
    // 16-storage-buffer per-stage layout limit
    rhi::BindGroupLayoutEntry sentries[] = {
        entry(0, T::Storage),          // voxels
        entry(1, T::Storage),          // dirtyIn
        entry(2, T::Storage),          // dirtyOut
        entry(3, T::ReadOnlyStorage),  // materials
        entry(4, T::Uniform),          // TickParams
    };
    simSlimBGL_ = device.CreateBindGroupLayout(sentries, std::size(sentries));

    // group 1: particle machinery (explode/integrate/resolve/args kernels)
    rhi::BindGroupLayoutEntry pentries[] = {
        entry(0, T::Storage),          // particles read page
        entry(1, T::Storage),          // particles write page
        entry(2, T::Storage),          // counts
        entry(3, T::Storage),          // claim hash
        entry(4, T::Storage),          // pArgsStage
        entry(5, T::ReadOnlyStorage),  // explosion ops
        entry(6, T::Storage),          // explosion destruction scratch
        entry(7, T::ReadOnlyStorage),  // CPU particle spawns (debris shatter)
    };
    particleBGL_ = device.CreateBindGroupLayout(pentries, std::size(pentries));
  }
  {
    auto entry = [](uint32_t binding, rhi::BufferBindingType type,
                    rhi::ShaderStage vis) {
      rhi::BindGroupLayoutEntry e{};
      e.binding = binding;
      e.visibility = vis;
      e.type = type;
      return e;
    };
    using T = rhi::BufferBindingType;
    using S = rhi::ShaderStage;
    rhi::BindGroupLayoutEntry entries[] = {
        entry(0, T::ReadOnlyStorage, S::Fragment),               // voxels
        entry(1, T::ReadOnlyStorage, S::Fragment),               // occupancy
        entry(2, T::ReadOnlyStorage, S::Fragment | S::Vertex),   // materials
        entry(3, T::Uniform, S::Fragment | S::Vertex),           // RenderParams
        entry(4, T::ReadOnlyStorage, S::Fragment),               // farVox
        entry(5, T::ReadOnlyStorage, S::Fragment),               // farOcc
        entry(6, T::Uniform, S::Fragment),                       // FarParams
        // Static micro-detail. Two more storage entries here takes the render
        // pipeline layout to 7 storage buffers across both groups (5 here + 4
        // in renderPartBGL_ minus the uniforms), still well under Dawn's limit
        // of 16 LAYOUT ENTRIES per stage — the limit counts declarations, not
        // shader usage (see the simSlimBGL_ comment).
        entry(7, T::ReadOnlyStorage, S::Fragment),               // microBricks
        entry(8, T::ReadOnlyStorage, S::Fragment),               // microPool
    };
    renderBGL_ = device.CreateBindGroupLayout(entries, std::size(entries));

    rhi::BindGroupLayoutEntry pentries[] = {
        entry(0, T::ReadOnlyStorage, S::Vertex),  // particles (live page)
        entry(1, T::ReadOnlyStorage, S::Vertex),  // sprites
        entry(2, T::ReadOnlyStorage, S::Vertex),  // debris body voxel instances
        entry(3, T::ReadOnlyStorage, S::Vertex),  // debris body transforms
        // Collision-box debug overlay. Costs one LAYOUT entry whether or not
        // the overlay is on; the draw is skipped entirely at zero boxes, so an
        // off overlay costs nothing but this declaration.
        entry(4, T::ReadOnlyStorage, S::Vertex),  // debug wireframe boxes
    };
    renderPartBGL_ = device.CreateBindGroupLayout(pentries, std::size(pentries));

    // Micro bodies get their OWN group 1 rather than extending renderPartBGL_.
    // Three reasons: the model/pool reads happen in the FRAGMENT stage (the
    // cube path's body buffers are vertex-only), the pool is 4 MiB that no
    // other pipeline should have bound, and Dawn counts layout ENTRIES per
    // stage — pairing renderBGL_'s 7 fragment storage entries with these 4
    // gives 11, comfortably under 16, whereas piling everything into one group
    // would have to be re-audited every time either side grows.
    rhi::BindGroupLayoutEntry mbentries[] = {
        entry(0, T::ReadOnlyStorage, S::Vertex | S::Fragment),  // bodyXforms
        entry(1, T::ReadOnlyStorage, S::Vertex | S::Fragment),  // models
        entry(2, T::ReadOnlyStorage, S::Fragment),              // brick pool
        entry(3, T::ReadOnlyStorage, S::Vertex | S::Fragment),  // draw list
    };
    microBodyBGL_ = device.CreateBindGroupLayout(mbentries, std::size(mbentries));
  }
  {
    simPL_ = device.CreatePipelineLayout(&simBGL_, 1);

    rhi::BindGroupLayout simGroups[] = {simSlimBGL_, particleBGL_};
    simPL2_ = device.CreatePipelineLayout(simGroups, 2);

    rhi::BindGroupLayout renderGroups[] = {renderBGL_, renderPartBGL_};
    renderPL_ = device.CreatePipelineLayout(renderGroups, 2);

    rhi::BindGroupLayout mbGroups[] = {renderBGL_, microBodyBGL_};
    microBodyPL_ = device.CreatePipelineLayout(mbGroups, 2);
  }
  {
    // far-field cascade fill + downsample: slim sim group 0 (`far` statically
    // uses only materials + TickParams; `fardown` adds voxels) + far buffers
    // as group 1. 4 storage entries in slim + 4 here = 8, well under Dawn's
    // 16-per-stage layout limit.
    auto entry = [](uint32_t binding, rhi::BufferBindingType type) {
      rhi::BindGroupLayoutEntry e{};
      e.binding = binding;
      e.visibility = rhi::ShaderStage::Compute;
      e.type = type;
      return e;
    };
    using T = rhi::BufferBindingType;
    rhi::BindGroupLayoutEntry entries[] = {
        entry(0, T::Storage),          // farVox
        entry(1, T::Storage),          // farOcc
        entry(2, T::ReadOnlyStorage),  // farList
        entry(3, T::Uniform),          // FarParams
        entry(4, T::ReadOnlyStorage),  // dirtyList (phase-2 downsample work set)
    };
    farBGL_ = device.CreateBindGroupLayout(entries, std::size(entries));

    rhi::BindGroupLayout farGroups[] = {simSlimBGL_, farBGL_};
    farPL_ = device.CreatePipelineLayout(farGroups, 2);
  }

  // ---- bind groups ----
  auto b = [](uint32_t binding, const rhi::Buffer& buf, uint64_t size = 0) {
    rhi::BindGroupEntry e{};
    e.binding = binding;
    e.buffer = buf;
    e.size = size;  // 0 = whole buffer, per rhi::BindGroupEntry
    return e;
  };
  for (int page = 0; page < 2; page++) {
    rhi::BindGroupEntry entries[] = {
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
    simBG_[page] = device.CreateBindGroup(simBGL_, entries, std::size(entries), "simBG");

    rhi::BindGroupEntry sentries[] = {
        b(0, world_->voxels),
        b(1, world_->dirty[page]),
        b(2, world_->dirty[1 - page]),
        b(3, materialBuf_),
        b(4, world_->tickUBO),
    };
    simSlimBG_[page] =
        device.CreateBindGroup(simSlimBGL_, sentries, std::size(sentries), "simSlimBG");

    rhi::BindGroupEntry pentries[] = {
        b(0, world_->particles[page]),
        b(1, world_->particles[1 - page]),
        b(2, world_->particleCounts),
        b(3, world_->claim),
        b(4, world_->pArgsStage),
        b(5, world_->expOps),
        b(6, world_->expMask),
        b(7, world_->spawnOps),
    };
    particleBG_[page] =
        device.CreateBindGroup(particleBGL_, pentries, std::size(pentries), "particleBG");

    rhi::BindGroupEntry rpentries[] = {
        b(0, world_->particles[page]),
        b(1, world_->sprites),
        b(2, world_->bodyInstances),
        b(3, world_->bodyXforms),
        b(4, world_->debugBoxes),
    };
    renderPartBG_[page] = device.CreateBindGroup(renderPartBGL_, rpentries,
                                                 std::size(rpentries), "renderPartBG");
  }
  {
    rhi::BindGroupEntry entries[] = {
        b(0, world_->voxels),
        b(1, world_->occupancy),
        b(2, materialBuf_),
        b(3, world_->renderUBO),
        b(4, world_->farVox),
        b(5, world_->farOcc),
        b(6, world_->farUBO),
        b(7, microTableBuf_),
        b(8, microPoolBuf_),
    };
    renderBG_ = device.CreateBindGroup(renderBGL_, entries, std::size(entries), "renderBG");
  }
  {
    rhi::BindGroupEntry entries[] = {
        b(0, world_->bodyXforms),
        b(1, mbModelBuf_),
        b(2, mbPoolBuf_),
        b(3, mbInstBuf_),
    };
    microBodyBG_ =
        device.CreateBindGroup(microBodyBGL_, entries, std::size(entries), "microBodyBG");
  }
  {
    rhi::BindGroupEntry entries[] = {
        b(0, world_->farVox),
        b(1, world_->farOcc),
        b(2, world_->farList),
        b(3, world_->farUBO),
        b(4, world_->dirtyList),
    };
    farBG_ = device.CreateBindGroup(farBGL_, entries, std::size(entries), "farBG");
  }

  std::string err;
  if (!BuildPipelines(device, &err)) {
    std::fprintf(stderr, "pipeline build failed:\n%s\n", err.c_str());
    return false;
  }
  return true;
}

namespace {
// The loader packs art colours as 0x00RRGGBB; the shader's unpackColor reads R
// from bits 0..7 (RGBA8 little-endian), so the byte order flips on the way in.
// Getting this wrong swaps red and blue, which reads as an art mistake rather
// than a packing one — hence one definition, used by both writers below.
inline uint32_t ArtRgbToGpu(uint32_t rgb) {
  return ((rgb & 0xFFu) << 16) | (rgb & 0xFF00u) | ((rgb >> 16) & 0xFFu) |
         0xFF000000u;
}
}  // namespace

// Write the cached art palette into the reserved run of a material table.
void Simulation::ApplyArtPalette(std::vector<MaterialGpu>& table) const {
  for (size_t i = 0; i < artPalette_.size() && i < kArtPaletteSlotsGpu; i++)
    table[kArtPaletteBaseGpu + i].color0 = ArtRgbToGpu(artPalette_[i]);
}

void Simulation::SetArtPalette(const rhi::Queue& queue,
                               const std::vector<uint32_t>& rgb) {
  artPalette_ = rgb;
  if (artPalette_.size() > kArtPaletteSlotsGpu)
    artPalette_.resize(kArtPaletteSlotsGpu);
  if (artPalette_.empty()) return;
  // Patch just the reserved run rather than re-uploading all 4096 entries: the
  // rest of the table is unchanged and may be mid-frame on the GPU.
  std::vector<MaterialGpu> run(kArtPaletteSlotsGpu, MaterialGpu{});
  for (size_t i = 0; i < artPalette_.size(); i++)
    run[i].color0 = ArtRgbToGpu(artPalette_[i]);
  queue.WriteBuffer(materialBuf_, (uint64_t)kArtPaletteBaseGpu * sizeof(MaterialGpu),
                    run.data(), run.size() * sizeof(MaterialGpu));
}

void Simulation::UploadTables(const rhi::Queue& queue,
                              const std::vector<MaterialDef>& mats,
                              const std::vector<ReactionGpu>& reactions) {
  std::vector<MaterialGpu> table(4096, MaterialGpu{});
  for (size_t i = 0; i < mats.size() && i < 4096; i++) table[i] = mats[i].gpu;

  // Mirror the stain palette into the reserved top entries (kStainPaletteBase,
  // materials.h): the renderer maps a voxel's 3-bit stain TYPE to a colour by
  // indexing there, which avoids a dedicated buffer + bind slot for what is at
  // most eight RGBA values. Every staining material writes its own slot; two
  // materials sharing a stain name share a slot and the last one wins, which
  // is correct — they are by definition the same stain.
  for (const auto& d : mats) {
    uint32_t type = d.gpu.stainPack & kStainPackTypeMask;
    if (type == 0) continue;
    table[kStainPaletteBase + type].stainColor = d.gpu.stainColor;
  }

  // Art palette, same trick one range lower (world.h). Re-applied here because
  // this function rebuilds the WHOLE table: without it, hot-reloading
  // materials.json would silently repaint every mob in its raw material
  // colours until something reloaded the mob defs.
  ApplyArtPalette(table);

  queue.WriteBuffer(materialBuf_, 0, table.data(), table.size() * sizeof(MaterialGpu));

  std::vector<ReactionGpu> rtable(kMaxReactions, ReactionGpu{});
  for (size_t i = 0; i < reactions.size() && i < kMaxReactions; i++)
    rtable[i] = reactions[i];
  queue.WriteBuffer(reactionBuf_, 0, rtable.data(), rtable.size() * sizeof(ReactionGpu));
}

void Simulation::UploadMicro(const rhi::Queue& queue, const MicroSet& micro) {
  // The table is exactly kMaterialSlots entries by construction (LoadMicroVox
  // sizes it), but a caller that hands over a default-constructed MicroSet
  // must still leave the GPU with a well-formed "nothing has a micro model"
  // table rather than a stale one.
  std::vector<MicroBrickGpu> table = micro.table;
  table.resize(kMaterialSlots, MicroBrickGpu{kMicroNoBrick, 0, 0, 0});
  queue.WriteBuffer(microTableBuf_, 0, table.data(), table.size() * sizeof(MicroBrickGpu));

  if (!micro.pool.empty()) {
    size_t words = std::min<size_t>(micro.pool.size(), kMicroPoolWords);
    queue.WriteBuffer(microPoolBuf_, 0, micro.pool.data(), words * 4);
  }
}

void Simulation::UploadMicroBodies(const rhi::Queue& queue,
                                   const MicroBodySet& set) {
  // Fixed-size GPU buffers: pad the table so a shrinking reload cannot leave a
  // stale model behind a still-live index, and never write past the ceiling.
  std::vector<MicroBodyModelGpu> table = set.models;
  if (table.size() > kMaxMicroBodyModels) table.resize(kMaxMicroBodyModels);
  table.resize(kMaxMicroBodyModels, MicroBodyModelGpu{kMicroBodyNoModel, 0, 1, 0});
  queue.WriteBuffer(mbModelBuf_, 0, table.data(),
                    table.size() * sizeof(MicroBodyModelGpu));

  if (!set.pool.empty()) {
    size_t words = std::min<size_t>(set.pool.size(), kMicroBodyPoolWordsWorld);
    queue.WriteBuffer(mbPoolBuf_, 0, set.pool.data(), words * 4);
  }

  // The skin's art colours ride the same upload the bricks do: they are
  // published together or a painted brick indexes colours that are not there
  // yet. Cheap and idempotent when nothing painted (SetArtPalette early-outs).
  SetArtPalette(queue, set.artColors);
}

bool Simulation::BuildPipelines(const rhi::Device& device, std::string* err) {
  auto mod = [&](const char* name) { return LoadShader(device, shaderDir_, name); };
  rhi::ShaderModule mWorldgen = mod("worldgen.wgsl");
  rhi::ShaderModule mMutate = mod("sim_mutate.wgsl");
  rhi::ShaderModule mCompact = mod("sim_compact.wgsl");
  rhi::ShaderModule mStep = mod("sim_step.wgsl");
  rhi::ShaderModule mOcc = mod("sim_occupancy.wgsl");
  rhi::ShaderModule mPick = mod("sim_pick.wgsl");
  rhi::ShaderModule mExplode = mod("sim_explode.wgsl");
  rhi::ShaderModule mParticle = mod("sim_particle.wgsl");
  rhi::ShaderModule mRay = mod("raymarch.wgsl");
  rhi::ShaderModule mDebris = mod("debris.wgsl");
  rhi::ShaderModule mMicroBody = mod("microbody.wgsl");
  rhi::ShaderModule mDebugLines = mod("debug_lines.wgsl");
  if (!mWorldgen || !mMutate || !mCompact || !mStep || !mOcc || !mPick ||
      !mExplode || !mParticle || !mRay || !mDebris || !mMicroBody ||
      !mDebugLines) {
    if (err) *err = "shader file read failure";
    return false;
  }

  worldgen_ = MakeComputePipeline(device, simPL_, mWorldgen, "main", "worldgen");
  worldgenList_ = MakeComputePipeline(device, simPL_, mWorldgen, "list", "worldgenList");
  farFill_ = MakeComputePipeline(device, farPL_, mWorldgen, "far", "farFill");
  farDown_ = MakeComputePipeline(device, farPL_, mWorldgen, "fardown", "farDown");
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
  microBodyModule_ = mMicroBody;
  debugLineModule_ = mDebugLines;
  targetFormat_ = rhi::TextureFormat::Undefined;  // force render pipeline rebuild
  return true;
}

bool Simulation::ReloadShaders(const rhi::Device& device) {
  // Validation errors during pipeline creation must not take the old (working)
  // pipelines down with them — F5 on a broken shader keeps playing. The seam
  // owns the scope because WebGPU resolves it through a Future on the instance
  // while Vulkan reports compile failure inline.
  device.PushValidationScope();
  std::string err;
  bool built = BuildPipelines(device, &err);
  bool hadError = device.PopValidationScopeBlocking();
  return built && !hadError;
}

// Measurement seam: identical to enc.BeginComputePass() unless the --measure
// harness has attached a PassTimer (see Simulation::SetPassTimer).
rhi::ComputePass Simulation::BeginPass(const rhi::CommandEncoder& enc,
                                               const char* name) const {
  if (passTimer_ && passTimer_->Valid()) return passTimer_->BeginPass(enc, name);
  return enc.BeginComputePass();
}

void Simulation::EncodeWorldgen(const rhi::CommandEncoder& enc) {
  page_ = 0;
  enc.ClearBuffer(world_->dirty[0], 0, rhi::kWholeSize);
  enc.ClearBuffer(world_->dirty[1], 0, rhi::kWholeSize);
  enc.ClearBuffer(world_->hash, 0, rhi::kWholeSize);
  enc.ClearBuffer(world_->support, 0, rhi::kWholeSize);
  enc.ClearBuffer(world_->particleCounts, 0, rhi::kWholeSize);
  enc.ClearBuffer(world_->claim, 0, rhi::kWholeSize);
  enc.ClearBuffer(world_->drawArgs, 0, rhi::kWholeSize);  // no ghost particles
  rhi::ComputePass pass = BeginPass(enc, "worldgen");
  uint32_t off = 0;
  pass.SetBindGroup(0, simBG_[page_], 1, &off);
  // one workgroup per slot chunk; occupancy + dirty flags computed in-kernel
  pass.SetPipeline(worldgen_);
  pass.DispatchWorkgroups(kNumChunks, 1, 1);
  pass.End();
}

void Simulation::EncodeGenList(const rhi::CommandEncoder& enc, uint32_t count) {
  if (count == 0) return;
  rhi::ComputePass pass = BeginPass(enc, "worldgenList");
  uint32_t off = 0;
  pass.SetBindGroup(0, simBG_[page_], 1, &off);
  pass.SetPipeline(worldgenList_);
  pass.DispatchWorkgroups(count, 1, 1);
  pass.End();
}

void Simulation::EncodeFarFill(const rhi::CommandEncoder& enc, uint32_t count) {
  if (count == 0) return;
  rhi::ComputePass pass = BeginPass(enc, "farFill");
  pass.SetBindGroup(0, simSlimBG_[page_]);
  pass.SetBindGroup(1, farBG_);
  pass.SetPipeline(farFill_);
  pass.DispatchWorkgroups(count, 1, 1);
  pass.End();
}

void Simulation::EncodeLoadReset(const rhi::CommandEncoder& enc) {
  page_ = 0;
  enc.ClearBuffer(world_->hash, 0, rhi::kWholeSize);
  enc.ClearBuffer(world_->support, 0, rhi::kWholeSize);
  enc.ClearBuffer(world_->particleCounts, 0, rhi::kWholeSize);
  enc.ClearBuffer(world_->claim, 0, rhi::kWholeSize);
  enc.ClearBuffer(world_->drawArgs, 0, rhi::kWholeSize);
  rhi::ComputePass pass = BeginPass(enc, "occupancyFull(loadReset)");
  uint32_t off = 0;
  pass.SetBindGroup(0, simBG_[page_], 1, &off);
  pass.SetPipeline(occupancy_);
  pass.DispatchWorkgroups(kNumChunks, 1, 1);
  pass.End();
}

void Simulation::EncodeHashOnly(const rhi::CommandEncoder& enc) {
  enc.ClearBuffer(world_->hash, 0, rhi::kWholeSize);
  rhi::ComputePass pass = BeginPass(enc, "occupancyFull(hashOnly)");
  uint32_t off = 0;
  pass.SetBindGroup(0, simBG_[page_], 1, &off);
  pass.SetPipeline(occupancy_);
  pass.DispatchWorkgroups(kNumChunks, 1, 1);
  pass.End();
}

void Simulation::EncodeWakeAll(const rhi::Queue& queue) {
  // dirty[page_] is the buffer the NEXT compact pass reads (dirtyIn). One u32
  // flag per chunk; 4096 chunks = 16 KB, far inside the ~1 MB/tick CPU->GPU
  // budget, and only written on a phase boundary.
  static const std::vector<uint32_t> ones(kNumChunks, 1u);
  queue.WriteBuffer(world_->dirty[page_], 0, ones.data(),
                    ones.size() * sizeof(uint32_t));
}

void Simulation::EncodeTick(const rhi::CommandEncoder& enc, uint32_t opsCount,
                            bool hashEnable, uint32_t expCount, bool particlesActive,
                            uint32_t cellCount, uint32_t spawnCount) {
  enc.ClearBuffer(world_->dirty[1 - page_], 0, rhi::kWholeSize);
  enc.ClearBuffer(world_->argsStage, 0, rhi::kWholeSize);
  if (particlesActive) enc.ClearBuffer(world_->claim, 0, rhi::kWholeSize);
  if (expCount > 0) enc.ClearBuffer(world_->expMask, 0, rhi::kWholeSize);
  if (hashEnable) enc.ClearBuffer(world_->hash, 0, rhi::kWholeSize);

  uint32_t off = 0;

  // Prep pass: mutate + explode + compact (compact writes argsStage + dirtyList).
  {
    rhi::ComputePass prep = BeginPass(enc, "prep(mutate+explode+compact)");
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
    rhi::ComputePass pass = BeginPass(enc, "ca(54 color x substep)");
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
      rhi::ComputePass pass = BeginPass(enc, "particleArgs1");
      pass.SetBindGroup(0, simSlimBG_[page_]);
      pass.SetBindGroup(1, particleBG_[page_]);
      pass.SetPipeline(pArgs1_);
      pass.DispatchWorkgroups(1, 1, 1);
      pass.End();
    }
    enc.CopyBufferToBuffer(world_->pArgsStage, 16, world_->pDispatchArgs, 0, 12);
    {
      rhi::ComputePass pass = BeginPass(enc, "particleIntegrate+args2");
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
      rhi::ComputePass pass = BeginPass(enc, "particleResolve");
      pass.SetBindGroup(0, simSlimBG_[page_]);
      pass.SetBindGroup(1, particleBG_[page_]);
      pass.SetPipeline(pResolve_);
      pass.DispatchWorkgroupsIndirect(world_->pDispatchArgs, 0);
      pass.End();
    }
  }

  if (hashEnable) {
    // hash ticks need the whole-world scan anyway; it also refreshes occupancy
    rhi::ComputePass pass = BeginPass(enc, "occupancyFull+pick(hashTick)");
    pass.SetBindGroup(0, simBG_[page_], 1, &off);
    pass.SetPipeline(occupancy_);
    pass.DispatchWorkgroups(kNumChunks, 1, 1);
    pass.SetPipeline(pick_);
    pass.DispatchWorkgroups(1, 1, 1);
    pass.End();
  } else {
    // occupancy update over only the chunks written this tick: compact the
    // dirtyOut flags (superset of every voxel write) and dispatch indirect
    enc.ClearBuffer(world_->argsStage, 0, rhi::kWholeSize);
    {
      rhi::ComputePass pass = BeginPass(enc, "compactNext");
      pass.SetBindGroup(0, simBG_[page_], 1, &off);
      pass.SetPipeline(compactNext_);
      pass.DispatchWorkgroups(kNumChunks / 64, 1, 1);
      pass.End();
    }
    enc.CopyBufferToBuffer(world_->argsStage, 0, world_->dispatchArgs, 0, 12);
    {
      rhi::ComputePass pass = BeginPass(enc, "occupancyDirty+pick");
      pass.SetBindGroup(0, simBG_[page_], 1, &off);
      pass.SetPipeline(occupancyDirty_);
      pass.DispatchWorkgroupsIndirect(world_->dispatchArgs, 0);
      pass.SetPipeline(pick_);
      pass.DispatchWorkgroups(1, 1, 1);
      pass.End();
    }
    // Far-field phase 2: downsample the same compacted dirtyOut list into the
    // cascades, so edits stay visible after the player walks away (render-only
    // derived data — DESIGN.md §9). Rides the SAME indirect args as the
    // occupancy update, so a settled world dispatches nothing. Separate pass:
    // it uses farPL_ (slim group 0 + far group 1), not simPL_.
    // NOTE: hash ticks (every 15th) skip this — they take the whole-world
    // occupancy branch above and never compact dirtyOut, so there is no work
    // list. A chunk edited on a hash tick is still dirty the next tick and
    // downsamples then; only single-tick-then-settle edits landing exactly on
    // a hash tick propagate one tick late, which is invisible in practice.
    {
      rhi::ComputePass pass = BeginPass(enc, "farDown");
      pass.SetBindGroup(0, simSlimBG_[page_]);
      pass.SetBindGroup(1, farBG_);
      pass.SetPipeline(farDown_);
      pass.DispatchWorkgroupsIndirect(world_->dispatchArgs, 0);
      pass.End();
    }
  }

  // Measurement only: no-op unless --measure attached a PassTimer.
  EncodeTimerResolve(enc);
}

void Simulation::EnsureDepth(uint32_t width, uint32_t height) {
  if (depthView_ && depthW_ == width && depthH_ == height) return;
  depthW_ = width;
  depthH_ = height;
  depthTex_ = device_.CreateTexture({width, height, 1}, kDepthFormat,
                                    rhi::TextureUsage::RenderAttachment, "depth");
  depthView_ = depthTex_.CreateView();
}

void Simulation::EnsureRenderPipelines(rhi::TextureFormat format) {
  if (format == targetFormat_) return;
  targetFormat_ = format;

  rhi::DepthState dsAlways{};
  dsAlways.format = kDepthFormat;
  dsAlways.depthWriteEnabled = true;
  dsAlways.depthCompare = rhi::CompareFunction::Always;

  rhi::DepthState dsTest{};
  dsTest.format = kDepthFormat;
  dsTest.depthWriteEnabled = true;
  dsTest.depthCompare = rhi::CompareFunction::GreaterEqual;  // reversed-Z

  {
    rhi::RenderPipelineDesc d{};
    d.label = "raymarch";
    d.layout = renderPL_;
    d.vertexModule = raymarchModule_;
    d.vertexEntry = "vs";
    d.fragmentModule = raymarchModule_;
    d.fragmentEntry = "fs";
    d.colorFormat = format;
    d.topology = rhi::PrimitiveTopology::TriangleList;
    d.depth = dsAlways;
    raymarch_ = device_.CreateRenderPipeline(d);
  }
  {
    rhi::RenderPipelineDesc d{};
    d.label = "particleDraw";
    d.layout = renderPL_;
    d.vertexModule = debrisModule_;
    d.vertexEntry = "vsParticle";
    d.fragmentModule = debrisModule_;
    d.fragmentEntry = "fs";
    d.colorFormat = format;
    d.topology = rhi::PrimitiveTopology::TriangleList;
    d.cullMode = rhi::CullMode::None;
    d.depth = dsTest;
    particleDraw_ = device_.CreateRenderPipeline(d);

    d.vertexEntry = "vsSprite";
    d.label = "spriteDraw";
    spriteDraw_ = device_.CreateRenderPipeline(d);

    d.vertexEntry = "vsBody";
    d.label = "bodyDraw";
    bodyDraw_ = device_.CreateRenderPipeline(d);
  }
  {
    // Collision-box wireframes. Its own module (debug_lines.wgsl) but the SAME
    // pipeline layout, so it needs no new bind groups.
    //
    // DEPTH TESTING OFF, WRITES OFF, and both are deliberate. A collider you
    // can only see when nothing is in front of it is useless precisely when you
    // need it — the reason to look at a limb's box is usually that the limb is
    // buried in something. Writes are off so the wireframe never occludes the
    // world it is annotating.
    rhi::DepthState dsNone{};
    dsNone.format = kDepthFormat;
    dsNone.depthWriteEnabled = false;
    dsNone.depthCompare = rhi::CompareFunction::Always;

    // Straight alpha over the frame: these are annotation, not lit geometry.
    rhi::BlendState blend{};
    blend.color.srcFactor = rhi::BlendFactor::SrcAlpha;
    blend.color.dstFactor = rhi::BlendFactor::OneMinusSrcAlpha;
    blend.color.operation = rhi::BlendOperation::Add;
    blend.alpha.srcFactor = rhi::BlendFactor::One;
    blend.alpha.dstFactor = rhi::BlendFactor::OneMinusSrcAlpha;
    blend.alpha.operation = rhi::BlendOperation::Add;

    rhi::RenderPipelineDesc d{};
    d.label = "debugBoxDraw";
    d.layout = renderPL_;
    d.vertexModule = debugLineModule_;
    d.vertexEntry = "vsBox";
    d.fragmentModule = debugLineModule_;
    d.fragmentEntry = "fsBox";
    d.colorFormat = format;
    d.blend = &blend;
    d.topology = rhi::PrimitiveTopology::TriangleList;
    d.cullMode = rhi::CullMode::None;
    d.depth = dsNone;
    debugBoxDraw_ = device_.CreateRenderPipeline(d);
  }
  {
    // Micro bodies: own layout (renderBGL_ + microBodyBGL_), own module, and
    // FRONT-face culling so only the far side of each OBB rasterizes. That is
    // what keeps a limb drawn when the camera is inside its box — the fragment
    // shader starts its march at the ray's slab entry, not at the triangle.
    rhi::RenderPipelineDesc d{};
    d.label = "microBodyDraw";
    d.layout = microBodyPL_;
    d.vertexModule = microBodyModule_;
    d.vertexEntry = "vs";
    d.fragmentModule = microBodyModule_;
    d.fragmentEntry = "fs";
    d.colorFormat = format;
    d.topology = rhi::PrimitiveTopology::TriangleList;
    d.cullMode = rhi::CullMode::Front;
    d.depth = dsTest;
    microBodyDraw_ = device_.CreateRenderPipeline(d);
  }
}

rhi::RenderPass Simulation::BeginRenderPass(const rhi::CommandEncoder& enc,
                                            const rhi::TextureView& target,
                                            rhi::TextureFormat format,
                                            uint32_t width, uint32_t height) {
  EnsureRenderPipelines(format);
  EnsureDepth(width, height);

  rhi::RenderPassDesc d{};
  d.label = "world";
  d.color.view = target;
  d.color.loadOp = rhi::LoadOp::Clear;
  d.color.storeOp = rhi::StoreOp::Store;
  d.color.clearValue[0] = 0.1;
  d.color.clearValue[1] = 0.15;
  d.color.clearValue[2] = 0.25;
  d.color.clearValue[3] = 1.0;
  d.hasDepth = true;
  d.depth.view = depthView_;
  d.depth.loadOp = rhi::LoadOp::Clear;
  d.depth.storeOp = rhi::StoreOp::Store;
  d.depth.clearValue = 0.0f;  // reversed-Z: clear to far
  return enc.BeginRenderPass(d);
}

void Simulation::DrawWorld(const rhi::RenderPass& pass) {
  pass.SetPipeline(raymarch_);
  pass.SetBindGroup(0, renderBG_);
  pass.SetBindGroup(1, renderPartBG_[page_]);
  pass.Draw(3);
}

void Simulation::DrawParticles(const rhi::RenderPass& pass) {
  pass.SetPipeline(particleDraw_);
  pass.SetBindGroup(0, renderBG_);
  pass.SetBindGroup(1, renderPartBG_[page_]);
  pass.DrawIndirect(world_->drawArgs, 0);
}

void Simulation::DrawSprites(const rhi::RenderPass& pass, uint32_t count) {
  if (count == 0) return;
  pass.SetPipeline(spriteDraw_);
  pass.SetBindGroup(0, renderBG_);
  pass.SetBindGroup(1, renderPartBG_[page_]);
  pass.Draw(36, count);
}

void Simulation::DrawDebugBoxes(const rhi::RenderPass& pass,
                               uint32_t count) {
  if (count == 0) return;   // overlay off: costs nothing
  pass.SetPipeline(debugBoxDraw_);
  pass.SetBindGroup(0, renderBG_);
  pass.SetBindGroup(1, renderPartBG_[page_]);
  // 12 edges x 6 vertices (two triangles per edge quad).
  pass.Draw(72, count);
}

void Simulation::DrawBodies(const rhi::RenderPass& pass, uint32_t voxInstances) {
  if (voxInstances == 0) return;
  pass.SetPipeline(bodyDraw_);
  pass.SetBindGroup(0, renderBG_);
  pass.SetBindGroup(1, renderPartBG_[page_]);
  pass.Draw(36, voxInstances);
}

uint32_t Simulation::UploadMicroBodyInsts(const rhi::Queue& queue,
                                          const std::vector<MicroBodyInstGpu>& insts) {
  // Zero micro bodies costs exactly one branch: no upload at all.
  // The instance list is CPU-compacted rather than indirect because the count
  // is already known on the CPU (it is built from the body slots this frame),
  // and an indirect buffer could not also be bound in the draw pass anyway.
  if (insts.empty()) return 0;
  uint32_t n = (uint32_t)std::min<size_t>(insts.size(), kMaxBodySlots);
  queue.WriteBuffer(mbInstBuf_, 0, insts.data(), (size_t)n * sizeof(MicroBodyInstGpu));
  return n;
}

void Simulation::DrawMicroBodies(const rhi::RenderPass& pass, uint32_t count) {
  if (count == 0) return;  // nothing uploaded this frame: no bind, no draw
  pass.SetPipeline(microBodyDraw_);
  pass.SetBindGroup(0, renderBG_);
  pass.SetBindGroup(1, microBodyBG_);
  pass.Draw(36, count);
}

void Simulation::FlipPage() { page_ = 1 - page_; }
