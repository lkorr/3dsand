#include "sim/simulation.h"

#include <algorithm>
#include <cstdio>

#include "gpu/resources.h"
#include "gpu/rhi_record.h"  // the Vulkan table-recording bridge (phase 4a)

// kPassStride (the passUBO dynamic-offset slice stride) moved to pass_table.h
// when the Vulkan recorder became a second consumer of it — see the note there.
using pass::kPassStride;

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

  // A backend that fails pipeline creation returns an INVALID handle (Vulkan:
  // Tint or vkCreateComputePipelines refused). Dawn reports errors through its
  // async error scope and always returns a valid handle, so this check is free
  // there — but on Vulkan a null pipeline would make the recorder silently
  // skip the row, which is a wrong SIM, not a crash. Fail the build instead.
  if (!worldgen_ || !worldgenList_ || !farFill_ || !farDown_ || !mutate_ ||
      !mutateCells_ || !compact_ || !compactNext_ || !step_ || !occupancy_ ||
      !occupancyDirty_ || !pick_ || !explodeMark_ || !explodeApply_ || !pArgs1_ ||
      !pSpawn_ || !pIntegrate_ || !pArgs2_ || !pResolve_) {
    if (err) *err = "compute pipeline creation failed (see stderr for the shader)";
    return false;
  }

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

// ===========================================================================
// TABLE-DRIVEN RECORDING (docs/PLAN_vulkan_port.md phase 2b)
//
// Every Encode* below records by WALKING src/sim/pass_table.def, not by issuing
// commands inline. Read that file's header first; the short version is that
// phase 3 generates Vulkan barriers from the same table, so the table's
// fidelity is what the port's determinism rests on — and the only way to keep a
// declaration faithful to a recording is to make the declaration BE the
// recording.
//
// Under Dawn this changes nothing about the command buffer: same encoders, same
// pass splits, same ClearBuffers, same copies, same conditionals, same dynamic
// offsets, same bind groups, same order. That is the acceptance criterion for
// this phase (world hash byte-identical), not an aspiration.
// ===========================================================================

namespace {

// Everything the recorder needs to resolve a row's selectors, gathered once per
// Encode* call. Conditions are all known on the CPU before recording begins
// (barrier_graph §2.3), which is what makes a skipped row a non-event.
struct RecordCtx {
  uint32_t opsCount = 0;
  uint32_t cellCount = 0;
  uint32_t expCount = 0;
  uint32_t spawnCount = 0;
  uint32_t genCount = 0;
  uint32_t farCount = 0;
  bool hashEnable = false;
  bool particlesActive = false;
};

bool CondHolds(pass::Cond c, const RecordCtx& cx) {
  switch (c) {
    case pass::Cond::Always:    return true;
    case pass::Cond::Ops:       return cx.opsCount > 0;
    case pass::Cond::Cells:     return cx.cellCount > 0;
    case pass::Cond::Exp:       return cx.expCount > 0;
    case pass::Cond::Spawn:     return cx.spawnCount > 0;
    case pass::Cond::Particles: return cx.particlesActive;
    case pass::Cond::Hash:      return cx.hashEnable;
    case pass::Cond::DirtyTick: return !cx.hashEnable;
    case pass::Cond::GenCount:  return cx.genCount > 0;
    case pass::Cond::FarCount:  return cx.farCount > 0;
  }
  return false;
}

// Dispatch extents. Values below kDynBase are literal; the rest are selectors
// resolved from this tick's counts.
uint32_t Extent(uint32_t v, const RecordCtx& cx) {
  if (v < (uint32_t)pass::DispatchSel::kDynBase) return v;
  switch ((pass::DispatchSel)v) {
    case pass::DispatchSel::Ops:      return 4 * cx.opsCount;
    case pass::DispatchSel::Cells:    return (cx.cellCount + 63) / 64;
    case pass::DispatchSel::Exp:      return kExplosionWg * cx.expCount;
    case pass::DispatchSel::ExpWg:    return kExplosionWg;
    case pass::DispatchSel::Spawn:    return (cx.spawnCount + 63) / 64;
    case pass::DispatchSel::Chunks:   return kNumChunks;
    case pass::DispatchSel::Chunks64: return kNumChunks / 64;
    case pass::DispatchSel::GenCount: return cx.genCount;
    case pass::DispatchSel::FarCount: return cx.farCount;
    default:                          return v;
  }
}

}  // namespace

// Map a table buffer id to the live rhi::Buffer. DirtyIn/DirtyOut and the two
// particle pages are SYMBOLIC (barrier_graph §2.2): `page_` decides which
// concrete buffer each names, resolved here at record time. DirtyIn and
// DirtyOut can never resolve to the same buffer for any page value — if they
// could, a tick's dirtyOut fill would silently clobber a day/night wake-all
// (§4.1's [NEW EDGE]). check_pass_table.py asserts that separately.
const rhi::Buffer& Simulation::PassBuffer(pass::Buf b) const {
  using B = pass::Buf;
  switch (b) {
    case B::Voxels:         return world_->voxels;
    case B::DirtyIn:        return world_->dirty[page_];
    case B::DirtyOut:       return world_->dirty[1 - page_];
    case B::Dirty0:         return world_->dirty[0];
    case B::Dirty1:         return world_->dirty[1];
    case B::Materials:      return materialBuf_;
    case B::TickUBO:        return world_->tickUBO;
    case B::PassUBO:        return world_->passUBO;
    case B::OpsBuf:         return world_->opsBuf;
    case B::Occupancy:      return world_->occupancy;
    case B::Hash:           return world_->hash;
    case B::Pick:           return world_->pick;
    case B::RenderUBO:      return world_->renderUBO;
    case B::Reactions:      return reactionBuf_;
    case B::DirtyList:      return world_->dirtyList;
    case B::ArgsStage:      return world_->argsStage;
    case B::CellOps:        return world_->cellOps;
    case B::Support:        return world_->support;
    case B::GenList:        return world_->genList;
    case B::DispatchArgs:   return world_->dispatchArgs;
    case B::ParticlesRead:  return world_->particles[page_];
    case B::ParticlesWrite: return world_->particles[1 - page_];
    case B::ParticleCounts: return world_->particleCounts;
    case B::Claim:          return world_->claim;
    case B::PArgsStage:     return world_->pArgsStage;
    case B::PDispatchArgs:  return world_->pDispatchArgs;
    case B::ExpOps:         return world_->expOps;
    case B::ExpMask:        return world_->expMask;
    case B::SpawnOps:       return world_->spawnOps;
    case B::DrawArgs:       return world_->drawArgs;
    case B::FarVox:         return world_->farVox;
    case B::FarOcc:         return world_->farOcc;
    case B::FarList:        return world_->farList;
    case B::FarUBO:         return world_->farUBO;
    default:                return world_->voxels;
  }
}

const rhi::ComputePipeline& Simulation::PassPipeline(pass::Pipe p) const {
  using P = pass::Pipe;
  switch (p) {
    case P::Worldgen:       return worldgen_;
    case P::WorldgenList:   return worldgenList_;
    case P::Mutate:         return mutate_;
    case P::MutateCells:    return mutateCells_;
    case P::Compact:        return compact_;
    case P::CompactNext:    return compactNext_;
    case P::Step:           return step_;
    case P::Occupancy:      return occupancy_;
    case P::OccupancyDirty: return occupancyDirty_;
    case P::Pick:           return pick_;
    case P::ExplodeMark:    return explodeMark_;
    case P::ExplodeApply:   return explodeApply_;
    case P::PArgs1:         return pArgs1_;
    case P::PSpawn:         return pSpawn_;
    case P::PIntegrate:     return pIntegrate_;
    case P::PArgs2:         return pArgs2_;
    case P::PResolve:       return pResolve_;
    case P::FarFill:        return farFill_;
    case P::FarDown:        return farDown_;
    default:                return step_;
  }
}

// Walk one table's rows and record them.
//
// The open compute pass is carried across rows: consecutive compute rows that
// declare the same `group` string share one ComputePassEncoder, and a Fill or
// Copy row (group == nullptr) closes it, because ClearBuffer and
// CopyBufferToBuffer are encoder-level commands. That reproduces today's pass
// structure exactly rather than approximating it.
//
// A row whose condition is false is skipped entirely — no pass is opened for
// it, nothing is recorded, and in phase 3 no buffer's last-access state is
// touched. barrier_graph §3.9/§7.5: that is the only correct handling, and it
// is why barriers must be computed at record time against live state rather
// than precomputed per adjacent table-index pair.
void Simulation::RecordTable(const rhi::CommandEncoder& enc, pass::Table which,
                             const void* ctxOpaque) {
  const RecordCtx& cx = *(const RecordCtx*)ctxOpaque;

  // -------------------------------------------------------------------------
  // VULKAN (phase 4a): the SAME table, walked by the generated-barrier
  // recorder. Barrier generation stays exactly the phase-3b shape — the rows
  // are the recorder's loop variable, never a parameter — and what crosses the
  // bridge is only the RESOLUTION: the page-symbolic ids and the pipelines,
  // resolved by the very same PassBuffer/PassPipeline the Dawn walk below uses.
  // This is what deleted vk_sim.cpp's parallel copy of that resolution.
  // -------------------------------------------------------------------------
  if (device_.Kind() == rhi::BackendKind::Vulkan) {
    rhi::TableCtx tc{};
    tc.opsCount = cx.opsCount;
    tc.cellCount = cx.cellCount;
    tc.expCount = cx.expCount;
    tc.spawnCount = cx.spawnCount;
    tc.genCount = cx.genCount;
    tc.farCount = cx.farCount;
    tc.hashEnable = cx.hashEnable;
    tc.particlesActive = cx.particlesActive;

    rhi::TableBindings tb{};
    for (int i = 0; i < (int)pass::Buf::kCount; i++)
      tb.buffers[i] = PassBuffer((pass::Buf)i);
    for (int i = 1; i < (int)pass::Pipe::FarDown + 1; i++)
      tb.pipelines[i] = PassPipeline((pass::Pipe)i);
    tb.simLayout = simPL_;
    tb.slimPartLayout = simPL2_;
    tb.slimFarLayout = farPL_;
    tb.simSet = simBG_[page_];
    tb.slimSet = simSlimBG_[page_];
    tb.particleSet = particleBG_[page_];
    tb.farSet = farBG_;

    rhi::RecordTableVulkan(enc, which, tc, tb,
                           passTimer_ && passTimer_->Valid() ? passTimer_ : nullptr);
    return;
  }

  rhi::ComputePass open;
  const char* openGroup = nullptr;
  auto closePass = [&]() {
    if (openGroup) {
      open.End();
      open = rhi::ComputePass{};
      openGroup = nullptr;
    }
  };

  for (int i = 0; i < pass::kRowCount; i++) {
    const pass::Row& r = pass::kRows[i];
    if (r.table != which) continue;
    if (!CondHolds(r.cond, cx)) continue;

    if (r.kind == pass::Kind::Fill) {
      closePass();
      enc.ClearBuffer(PassBuffer(r.uses[0].buf), 0, rhi::kWholeSize);
      continue;
    }
    if (r.kind == pass::Kind::Copy) {
      closePass();
      // Copy rows carry (srcOffset, dstOffset, size) in x/y/z, and exactly two
      // uses: the transfer read then the transfer write.
      enc.CopyBufferToBuffer(PassBuffer(r.uses[0].buf), r.x,
                             PassBuffer(r.uses[1].buf), r.y, r.z);
      continue;
    }

    // Compute / ComputeIndirect.
    if (!openGroup || r.group != openGroup) {
      closePass();
      open = BeginPass(enc, r.group);
      openGroup = r.group;
    }

    // Bind groups are set per ROW rather than once per pass. The hand-written
    // recorder set them before each dispatch block inside the shared prep pass
    // (mutate, mutateCells, explode, spawn and compact each re-bound), and
    // SetBindGroup to the same group is idempotent, so per-row is both faithful
    // and free of a "did the previous row leave the right groups bound?"
    // question that a future row insertion would silently get wrong.
    {
      uint32_t off = 0;
      switch (r.groups) {
        case pass::Groups::Sim:
          open.SetBindGroup(0, simBG_[page_], 1, &off);
          break;
        case pass::Groups::SlimPart:
          open.SetBindGroup(0, simSlimBG_[page_]);
          open.SetBindGroup(1, particleBG_[page_]);
          break;
        case pass::Groups::SlimFar:
          open.SetBindGroup(0, simSlimBG_[page_]);
          open.SetBindGroup(1, farBG_);
          break;
        default:
          break;
      }
    }

    open.SetPipeline(PassPipeline(r.pipe));

    for (uint32_t k = 0; k < r.repeat; k++) {
      if (r.dyn == pass::Dyn::Ca) {
        // The per-iteration passUBO slice: colour phase + gravity substep. Two
        // iterations are two DIFFERENT colours, which is exactly why they must
        // never overlap — see the lattice note in pass_table.def's header.
        uint32_t offset = k * kPassStride;
        open.SetBindGroup(0, simBG_[page_], 1, &offset);
      }
      if (r.kind == pass::Kind::ComputeIndirect) {
        const rhi::Buffer& args =
            (pass::DispatchSel)r.x == pass::DispatchSel::IndPDispatchArgs
                ? world_->pDispatchArgs
                : world_->dispatchArgs;
        open.DispatchWorkgroupsIndirect(args, 0);
      } else {
        open.DispatchWorkgroups(Extent(r.x, cx), Extent(r.y, cx),
                                Extent(r.z, cx));
      }
    }
  }
  closePass();
}

void Simulation::EncodeWorldgen(const rhi::CommandEncoder& enc) {
  page_ = 0;
  RecordCtx cx{};
  RecordTable(enc, pass::Table::Worldgen, &cx);
}

void Simulation::EncodeGenList(const rhi::CommandEncoder& enc, uint32_t count) {
  RecordCtx cx{};
  cx.genCount = count;
  RecordTable(enc, pass::Table::GenList, &cx);
}

void Simulation::EncodeFarFill(const rhi::CommandEncoder& enc, uint32_t count) {
  RecordCtx cx{};
  cx.farCount = count;
  RecordTable(enc, pass::Table::FarFill, &cx);
}

void Simulation::EncodeLoadReset(const rhi::CommandEncoder& enc) {
  page_ = 0;
  RecordCtx cx{};
  RecordTable(enc, pass::Table::LoadReset, &cx);
}

void Simulation::EncodeHashOnly(const rhi::CommandEncoder& enc) {
  RecordCtx cx{};
  RecordTable(enc, pass::Table::HashOnly, &cx);
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
  RecordCtx cx{};
  cx.opsCount = opsCount;
  cx.cellCount = cellCount;
  cx.expCount = expCount;
  cx.spawnCount = spawnCount;
  cx.hashEnable = hashEnable;
  cx.particlesActive = particlesActive;
  RecordTable(enc, pass::Table::Tick, &cx);

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
