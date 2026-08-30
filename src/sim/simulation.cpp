#include "sim/simulation.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "gpu/resources.h"
#include "sim/pagetable.h"
#include "sim/tuning.h"      // fluidExciteMode gates the seam recording
#include "gpu/rhi_record.h"  // the Vulkan table-recording bridge (phase 4a)

// kPassStride (the passUBO dynamic-offset slice stride) moved to pass_table.h
// when the Vulkan recorder became a second consumer of it — see the note there.
using pass::kPassStride;

bool Simulation::Init(const rhi::Device& device, World& world,
                      const std::vector<MaterialDef>& mats,
                      const std::vector<ReactionGpu>& reactions,
                      const MicroSet& micro, const TreeAtlas& trees,
                      const std::string& shaderDir) {
  world_ = &world;
  device_ = device;
  shaderDir_ = shaderDir;
  rhi::Queue queue = device.GetQueue();

  // The baked tree atlas. Sized to what the assets actually hold rather than to
  // a ceiling constant: it is load-time asset data, it never grows, and the
  // buffer is created BEFORE the bind groups below because it is one of their
  // entries. A world with no .svtree files still gets a valid (header-only)
  // buffer -- a zero-length storage binding is not legal, and "no trees" has to
  // be a world rather than a crash.
  treeAtlasWords_ = std::max<size_t>(trees.words.size(), treeatlas::kHeaderWords);
  treeAtlasBuf_ = CreateBuffer(device, (uint64_t)treeAtlasWords_ * 4,
                               rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                               "treeAtlas");
  {
    std::vector<uint32_t> pad = trees.words;
    pad.resize(treeAtlasWords_, 0u);
    queue.WriteBuffer(treeAtlasBuf_, 0, pad.data(), pad.size() * 4);
  }

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
  // Zero-init: publish an empty table so a model index that arrives before any
  // real upload cannot read whatever the buffer happened to hold.
  MicroBodySet emptySet;
  UploadMicroBodies(queue, emptySet);

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
        // ---- the software page table (PLAN_page_table.md §5.2) ----
        // Group 0 is not negotiable, and that is what makes the shared
        // accessors in common.wgsl work: common.wgsl is prepended to every
        // shader, so the accessors must name a group whose meaning is
        // identical everywhere. Group 1 differs by pipeline, so a group-1
        // pageTable would have to be declared per shader, which dissolves the
        // single-seam property the whole design rests on.
        //
        // Bindings 17/18 in BOTH simBGL_ and simSlimBGL_, not 17/18 here and
        // 5/6 there: one WGSL identifier cannot carry two binding numbers
        // across modules that share common.wgsl, and 5/6 are already taken in
        // simBGL_ (PassParams, brush ops). The slim group therefore stops
        // being a dense prefix and becomes 0..4 + 17..18, which Vulkan is
        // perfectly happy with — sparse binding numbers are legal, and
        // maxPerStageDescriptorStorageBuffers here is 1,048,576.
        //
        // The consequence §5.2a wanted still holds and is now deliberate
        // rather than lucky: any pipeline built on simSlimBGL_ inherits
        // translation, which is how worldgen:fardown (on farPL_) gets it.
        entry(17, T::ReadOnlyStorage), // pageTable
        entry(18, T::Storage),         // pageFaults (atomic counter)
        // JITTER materialization list, (slot, entry) pairs. Its own buffer
        // rather than genList: Stream::FillSlots writes genList mid-frame while
        // this drains at the head of the next command buffer, and the two
        // deferred writes interleave (world.cpp's note).
        entry(19, T::ReadOnlyStorage), // pageFillList
        // MLS-MPM excited-fluid coupling (sim_step.wgsl bindings 20..22):
        // block map + node grid read-only, the seam's intent/flags scratch
        // read_write (the consume flag is the CA's one write into it).
        entry(20, T::ReadOnlyStorage), // fluidBlockMap
        entry(21, T::ReadOnlyStorage), // fluidGrid
        entry(22, T::Storage),         // fluidCellScratch
        entry(23, T::Storage),         // actVoxViz (per-voxel debug overlay)
        // The water-body drain ledger (docs/PLAN_water_master.md M2). GPU-owned
        // because the ledger must debit by what the shave ATOMICALLY reported,
        // and reading that back would put fence retirement inside a voxel
        // write's control path — see sim_waterbody.wgsl's header.
        entry(24, T::Storage),         // waterBodyState (atomic i32 ledger)
        // The discharge's emission seam (M3, component 6): sim_waterbody.wgsl
        // fills the CPU-reserved spawn-op block, because the head `h` it is
        // derived from is a level the GPU owns. Bound read_write HERE and
        // read-only in the fluid/seam groups, which is exactly what the pass
        // table's W(FluidSpawnOps) -> R(FluidSpawnOps) barrier is for.
        entry(25, T::Storage),         // fluidSpawnOps (drain writes)
        // The baked tree atlas (src/sim/treeatlas.h). Read-only asset data
        // uploaded once, like `materials` at binding 3 — worldgen samples it
        // per cell instead of evaluating implicit tree shapes. Binding 26 in
        // BOTH this layout and simSlimBGL_ for the same reason 17/18 are: one
        // WGSL identifier cannot carry two binding numbers across modules that
        // share common.wgsl, and the far-cascade pipelines (farPL_, on the slim
        // group) call genCell and therefore call the tree sampler.
        entry(26, T::ReadOnlyStorage), // treeAtlas
    };
    simBGL_ = device.CreateBindGroupLayout(entries, std::size(entries));

    // slim group 0 for the particle/explosion/far pipelines: bindings 0..4
    // plus the two page buffers at 17/18, which must keep the SAME binding
    // numbers they have in simBGL_ (see the note above). Those pipelines
    // genuinely do not need the other 12 bindings.
    rhi::BindGroupLayoutEntry sentries[] = {
        entry(0, T::Storage),          // voxels
        entry(1, T::Storage),          // dirtyIn
        entry(2, T::Storage),          // dirtyOut
        entry(3, T::ReadOnlyStorage),  // materials
        entry(4, T::Uniform),          // TickParams
        entry(17, T::ReadOnlyStorage), // pageTable
        entry(18, T::Storage),         // pageFaults
        // COMPONENT 7 put the water-body ledger in the SLIM group: the
        // excite/settle seam runs on this layout and its drain-shell trigger
        // asks a draining hole where it is. Binding 24 has to be the same
        // buffer in every module that names it, so it is added here rather
        // than given a seam-group entry of its own.
        entry(24, T::Storage),         // waterBodyState (atomic i32 ledger)
        // Same binding number as in simBGL_ above; `fardown`/`far` build on
        // this layout and both reach genCell -> treeAt.
        entry(26, T::ReadOnlyStorage), // treeAtlas
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

    // group 1: MLS-MPM fluid prototype (sim_fluid.wgsl). Same slim-group-0
    // pairing as the particle pipelines. fluidDispatchArgs is deliberately
    // absent — Indirect buffers are never bound (world.h dispatchArgs note).
    rhi::BindGroupLayoutEntry fentries[] = {
        entry(0, T::Storage),          // fluidParticles
        entry(1, T::ReadOnlyStorage),  // fluidSpawnOps
        entry(2, T::Storage),          // fluidBlockMap (atomic)
        entry(3, T::Storage),          // fluidBlockList
        entry(4, T::Storage),          // fluidGrid (atomic accumulators)
        entry(5, T::Storage),          // fluidArgs staging
        // Splash coupling (sim_fluid.wgsl g2p): the ballistic particle
        // system's WRITE page + counts, so fast free-surface fluid particles
        // can shed micro droplets. Paged like particleBG_ — see fluidBG_.
        entry(6, T::Storage),          // pWrite (particles, this tick's write page)
        entry(7, T::Storage),          // counts (atomic)
    };
    fluidBGL_ = device.CreateBindGroupLayout(fentries, std::size(fentries));

    // group 1: the excite/settle seam (sim_fluid_seam.wgsl). Pairs with the
    // slim group 0 (which carries voxels RW, dirtyOut, materials, TickUBO,
    // pageTable, pageFaults — everything the converters' voxel writes need).
    rhi::BindGroupLayoutEntry sfentries[] = {
        entry(0, T::ReadOnlyStorage),  // fluidParticles[page] (compact src)
        entry(1, T::Storage),          // fluidParticles[1-page] (working)
        entry(2, T::ReadOnlyStorage),  // fluidSpawnOps
        entry(3, T::Storage),          // fluidBlockMap (last substep's index
                                       // half, read; stainApply writes the
                                       // Y-occupancy half)
        entry(4, T::ReadOnlyStorage),  // fluidGrid (last substep's)
        entry(5, T::Storage),          // fluidArgsStage (FA_* words, atomic)
        entry(6, T::Storage),          // dirtyList (read; shared entry is RW)
        entry(7, T::Storage),          // fluidExciteScratch
        entry(8, T::Storage),          // fluidCalm
        entry(9, T::Storage),          // fluidSettleScratch
        entry(10, T::Storage),         // fluidCompactScratch
        entry(11, T::Storage),         // fluidCellScratch (intents + flags)
        entry(12, T::Storage),         // fluidBlockList (stainApply's slots)
        entry(13, T::Storage),         // fluidMirror (swimming fold)
    };
    fluidSeamBGL_ = device.CreateBindGroupLayout(sfentries, std::size(sfentries));
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
        // Software page table (PLAN_page_table.md §5.2a). raymarch.wgsl does
        // 17 raw voxel reads; under paging every one indexes the POOL with a
        // SLOT-derived address and samples the wrong chunk wherever the target
        // is a sentinel — the world would render as garbage while hashing
        // perfectly, because the render path is outside the hashed domain and
        // no determinism gate could catch it.
        //
        // ReadOnlyStorage, matching `voxels` at binding 0, and no pageFaults:
        // the renderer must never write the world. microBodyBGL_ shares
        // renderBGL_ as group 0 and inherits this for free — microbody.wgsl
        // reads its own brick pool, not voxels, so it needs nothing itself.
        entry(9, T::ReadOnlyStorage, S::Fragment),               // pageTable
        // MPM fluid surface (raymarch.wgsl MPM FLUID SURFACE block): the
        // solver's block map + node grid, read exactly like `voxels` — the
        // renderer samples the last substep's mass/velocity/species field
        // directly, zero upload. ReadOnly: the arrow points sim -> render.
        entry(10, T::ReadOnlyStorage, S::Fragment),              // fluidBlockMap
        entry(11, T::ReadOnlyStorage, S::Fragment),              // fluidGrid
        entry(12, T::ReadOnlyStorage, S::Fragment),              // dirtyViz
        entry(13, T::ReadOnlyStorage, S::Fragment),  // actVoxViz
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
        entry(5, T::ReadOnlyStorage, S::Vertex),  // MLS-MPM fluid particles
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
    // as group 1. 4 storage entries in slim + 5 here = 9, well under Dawn's
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
        entry(5, T::ReadOnlyStorage),  // farPatch (cascade edit persistence)
    };
    farBGL_ = device.CreateBindGroupLayout(entries, std::size(entries));

    rhi::BindGroupLayout farGroups[] = {simSlimBGL_, farBGL_};
    farPL_ = device.CreatePipelineLayout(farGroups, 2);

    rhi::BindGroupLayout fluidGroups[] = {simSlimBGL_, fluidBGL_};
    fluidPL_ = device.CreatePipelineLayout(fluidGroups, 2);

    rhi::BindGroupLayout fluidSeamGroups[] = {simSlimBGL_, fluidSeamBGL_};
    fluidSeamPL_ = device.CreatePipelineLayout(fluidSeamGroups, 2);
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
        b(17, world_->pageTable),
        b(18, world_->pageFaults),
        b(19, world_->pageFillList),
        b(20, world_->fluidBlockMap),
        b(21, world_->fluidGrid),
        b(22, world_->fluidCellScratch),
        b(23, world_->actVoxViz),
        b(24, world_->waterBodyState),
        b(25, world_->fluidSpawnOps),
        b(26, treeAtlasBuf_),
    };
    simBG_[page] = device.CreateBindGroup(simBGL_, entries, std::size(entries), "simBG");

    rhi::BindGroupEntry sentries[] = {
        b(0, world_->voxels),
        b(1, world_->dirty[page]),
        b(2, world_->dirty[1 - page]),
        b(3, materialBuf_),
        b(4, world_->tickUBO),
        b(17, world_->pageTable),
        b(18, world_->pageFaults),
        b(24, world_->waterBodyState),
        b(26, treeAtlasBuf_),
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
        // The fluid pair pages exactly like the ballistic particles: after
        // FlipPage, fluidParticles[Page()] is the buffer the tick just wrote.
        b(5, world_->fluidParticles[page]),
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
        b(9, world_->pageTable),
        b(10, world_->fluidBlockMap),
        b(11, world_->fluidGrid),
        b(12, world_->dirtyViz),
        b(13, world_->actVoxViz),
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
        b(5, world_->farPatch),
    };
    farBG_ = device.CreateBindGroup(farBGL_, entries, std::size(entries), "farBG");
  }
  for (int page = 0; page < 2; page++) {
    rhi::BindGroupEntry entries[] = {
        // The tick's WORKING buffer (the seam's compaction destination; the
        // renderer's source after the flip) — fluidParticles[1 - page_].
        b(0, world_->fluidParticles[1 - page]),
        b(1, world_->fluidSpawnOps),
        b(2, world_->fluidBlockMap),
        b(3, world_->fluidBlockList),
        b(4, world_->fluidGrid),
        b(5, world_->fluidArgsStage),
        // The particle WRITE page for this parity: fluid substeps run after
        // particleResolve, so droplets appended here are picked up by NEXT
        // tick's integrate (the page flip makes this the read page then).
        b(6, world_->particles[1 - page]),
        b(7, world_->particleCounts),
    };
    fluidBG_[page] =
        device.CreateBindGroup(fluidBGL_, entries, std::size(entries), "fluidBG");

    rhi::BindGroupEntry sentries[] = {
        b(0, world_->fluidParticles[page]),      // compact source (last tick)
        b(1, world_->fluidParticles[1 - page]),  // working buffer
        b(2, world_->fluidSpawnOps),
        b(3, world_->fluidBlockMap),
        b(4, world_->fluidGrid),
        b(5, world_->fluidArgsStage),
        b(6, world_->dirtyList),
        b(7, world_->fluidExciteScratch),
        b(8, world_->fluidCalm),
        b(9, world_->fluidSettleScratch),
        b(10, world_->fluidCompactScratch),
        b(11, world_->fluidCellScratch),
        b(12, world_->fluidBlockList),
        b(13, world_->fluidMirror),
    };
    fluidSeamBG_[page] = device.CreateBindGroup(fluidSeamBGL_, sentries,
                                                std::size(sentries), "fluidSeamBG");
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

void Simulation::UploadMicroBodies(const rhi::Queue& queue, MicroBodySet& set) {
  // Fixed-size GPU buffers: pad the table so a shrinking reload cannot leave a
  // stale model behind a still-live index, and never write past the ceiling.
  // The whole table is 16 bytes x kMaxMicroBodyModels — small enough that
  // tracking which records moved would cost more than the write.
  std::vector<MicroBodyModelGpu> table = set.models;
  if (table.size() > kMaxMicroBodyModels) table.resize(kMaxMicroBodyModels);
  table.resize(kMaxMicroBodyModels, MicroBodyModelGpu{kMicroBodyNoModel, 0, 1, 0});
  queue.WriteBuffer(mbModelBuf_, 0, table.data(),
                    table.size() * sizeof(MicroBodyModelGpu));

  // The POOL is 4 MiB and is sent by RANGE (MicroBodySet::MarkPool). Writing
  // all of it on any dirty was correct and free while only a carve dirtied it;
  // per-voxel body burning dirties it every tick, and 4 MiB/tick is four times
  // the whole CPU->GPU budget (DESIGN.md §11) for what is usually a handful of
  // changed words.
  const size_t poolWords =
      std::min<size_t>(set.pool.size(), kMicroBodyPoolWordsWorld);
  if (poolWords) {
    if (set.poolDirtyAll) {
      // Whole-pool: first publish, hot reload, or the ranges overflowed.
      queue.WriteBuffer(mbPoolBuf_, 0, set.pool.data(), poolWords * 4);
    } else {
      for (const auto& r : set.dirtyRanges) {
        const size_t lo = std::min<size_t>(r.first, poolWords);
        const size_t hi = std::min<size_t>(r.second, poolWords);
        if (hi <= lo) continue;
        queue.WriteBuffer(mbPoolBuf_, lo * 4, set.pool.data() + lo,
                          (hi - lo) * 4);
      }
    }
  }
  set.ClearDirty();

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
  rhi::ShaderModule mFluid = mod("sim_fluid.wgsl");
  rhi::ShaderModule mFluidSeam = mod("sim_fluid_seam.wgsl");
  rhi::ShaderModule mWaterBody = mod("sim_waterbody.wgsl");
  rhi::ShaderModule mRay = mod("raymarch.wgsl");
  rhi::ShaderModule mDebris = mod("debris.wgsl");
  rhi::ShaderModule mMicroBody = mod("microbody.wgsl");
  rhi::ShaderModule mDebugLines = mod("debug_lines.wgsl");
  rhi::ShaderModule mDebugWind = mod("debug_wind.wgsl");
  rhi::ShaderModule mDebugCur = mod("debug_current.wgsl");
  if (!mWorldgen || !mMutate || !mCompact || !mStep || !mOcc || !mPick ||
      !mExplode || !mParticle || !mFluid || !mFluidSeam || !mWaterBody ||
      !mRay || !mDebris ||
      !mMicroBody || !mDebugLines || !mDebugWind || !mDebugCur) {
    if (err) *err = "shader file read failure";
    return false;
  }

  worldgen_ = MakeComputePipeline(device, simPL_, mWorldgen, "main", "worldgen");
  worldgenList_ = MakeComputePipeline(device, simPL_, mWorldgen, "list", "worldgenList");
  // Same module as worldgen: the JITTER page fill shares genChunk's slot->world
  // mapping and must not drift from it (world.h's JITTER block).
  pageFill_ = MakeComputePipeline(device, simPL_, mWorldgen, "pagefill", "pageFill");
  farFill_ = MakeComputePipeline(device, farPL_, mWorldgen, "far", "farFill");
  farDown_ = MakeComputePipeline(device, farPL_, mWorldgen, "fardown", "farDown");
  mutate_ = MakeComputePipeline(device, simPL_, mMutate, "main", "mutate");
  mutateCells_ = MakeComputePipeline(device, simPL_, mMutate, "cells", "mutateCells");
  // The wind primitive footprint wake — same module, third entry point. It
  // needs only dirtyIn/dirtyOut and TickParams, all of which simPL_ already
  // binds, so a fan costs no new binding and no new layout.
  windWake_ = MakeComputePipeline(device, simPL_, mMutate, "windWake", "windWake");
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

  fluidMark_ = MakeComputePipeline(device, fluidPL_, mFluid, "mark", "fluidMark");
  fluidAlloc_ = MakeComputePipeline(device, fluidPL_, mFluid, "alloc", "fluidAlloc");
  fluidClear_ = MakeComputePipeline(device, fluidPL_, mFluid, "clearGrid", "fluidClear");
  fluidP2g_ = MakeComputePipeline(device, fluidPL_, mFluid, "p2g1", "fluidP2g1");
  fluidP2g2_ = MakeComputePipeline(device, fluidPL_, mFluid, "p2g2", "fluidP2g2");
  fluidGridUp_ = MakeComputePipeline(device, fluidPL_, mFluid, "gridUpdate", "fluidGridUp");
  fluidG2p_ = MakeComputePipeline(device, fluidPL_, mFluid, "g2p", "fluidG2p");

  // The excite/settle seam (sim_fluid_seam.wgsl; fluidSpawn_ moved here —
  // appends go through the seam's GPU-owned count now).
  fluidSpawn_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "spawnAppend", "seamSpawn");
  fluidCompactCount_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "compactCount", "seamCompactCount");
  fluidCompactScan_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "compactScan", "seamCompactScan");
  fluidCompactScatter_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "compactScatter", "seamCompactScatter");
  fluidExciteDetect_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "exciteDetect", "seamExciteDetect");
  fluidExciteScan_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "exciteScan", "seamExciteScan");
  fluidExciteEmit_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "exciteEmit", "seamExciteEmit");
  fluidPTick_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "particleTick", "seamParticleTick");
  fluidSettleJudge_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "settleJudge", "seamSettleJudge");
  fluidSettleScan_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "settleScan", "seamSettleScan");
  fluidSettleBin_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "settleBin", "seamSettleBin");
  fluidSettleCheck_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "settleCheck", "seamSettleCheck");
  fluidSettleCommit_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "settleCommit", "seamSettleCommit");
  fluidSettleKill_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "settleKill", "seamSettleKill");
  fluidConsumeApply_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "consumeApply", "seamConsumeApply");
  fluidStainApply_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "stainApply", "seamStainApply");
  fluidMirrorFold_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "mirrorFold", "seamMirrorFold");
  fluidCellClear_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "cellClear", "seamCellClear");

  // Water bodies (docs/PLAN_water_master.md M2). On simPL_ like the CA:
  // everything the shave needs to write a voxel — voxels, dirtyOut,
  // pageTable, pageFaults, TickParams — is already in that layout, and the
  // ledger buffer is one added binding rather than a new group.
  waterQuiet_ = MakeComputePipeline(device, simPL_, mWaterBody, "wbQuiet", "waterQuiet");
  waterLedger_ = MakeComputePipeline(device, simPL_, mWaterBody, "wbLedger", "waterLedger");
  waterReduce_ = MakeComputePipeline(device, simPL_, mWaterBody, "wbReduce", "waterReduce");
  waterShave_ = MakeComputePipeline(device, simPL_, mWaterBody, "wbShave", "waterShave");
  waterDrain_ = MakeComputePipeline(device, simPL_, mWaterBody, "wbDrain", "waterDrain");
  waterHole_ = MakeComputePipeline(device, simPL_, mWaterBody, "wbHole", "waterHole");
  // M5: the scheduled container sweep (components 2 case 2 + 10).
  waterSweep_ = MakeComputePipeline(device, simPL_, mWaterBody, "wbSweep", "waterSweep");
  waterSplit_ = MakeComputePipeline(device, simPL_, mWaterBody, "wbSplit", "waterSplit");

  // A backend that fails pipeline creation returns an INVALID handle (Vulkan:
  // Tint or vkCreateComputePipelines refused). Dawn reports errors through its
  // async error scope and always returns a valid handle, so this check is free
  // there — but on Vulkan a null pipeline would make the recorder silently
  // skip the row, which is a wrong SIM, not a crash. Fail the build instead.
  if (!worldgen_ || !worldgenList_ || !pageFill_ || !farFill_ || !farDown_ || !mutate_ ||
      !mutateCells_ || !windWake_ || !compact_ || !compactNext_ || !step_ || !occupancy_ ||
      !occupancyDirty_ || !pick_ || !explodeMark_ || !explodeApply_ || !pArgs1_ ||
      !pSpawn_ || !pIntegrate_ || !pArgs2_ || !pResolve_ || !fluidSpawn_ ||
      !fluidMark_ || !fluidAlloc_ || !fluidClear_ || !fluidP2g_ ||
      !fluidP2g2_ || !fluidGridUp_ || !fluidG2p_ || !fluidCompactCount_ ||
      !fluidCompactScan_ || !fluidCompactScatter_ || !fluidExciteDetect_ ||
      !fluidExciteScan_ || !fluidExciteEmit_ || !fluidPTick_ ||
      !fluidSettleJudge_ || !fluidSettleScan_ || !fluidSettleBin_ ||
      !fluidSettleCheck_ || !fluidSettleCommit_ || !fluidSettleKill_ ||
      !fluidConsumeApply_ || !fluidStainApply_ || !fluidMirrorFold_ ||
      !fluidCellClear_ || !waterDrain_ || !waterHole_ ||
      !waterQuiet_ || !waterLedger_ || !waterReduce_ ||
      !waterShave_ || !waterSweep_ || !waterSplit_) {
    if (err) *err = "compute pipeline creation failed (see stderr for the shader)";
    return false;
  }

  raymarchModule_ = mRay;
  debrisModule_ = mDebris;
  microBodyModule_ = mMicroBody;
  debugLineModule_ = mDebugLines;
  debugWindModule_ = mDebugWind;
  debugCurModule_ = mDebugCur;
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
// The restructure was landed hash-neutral: converting the hand-written
// recorder into a table walk changed nothing about the command buffer — same
// pass splits, ClearBuffers, copies, conditionals, dynamic offsets, bind
// groups and order — and a byte-identical world hash was the acceptance
// criterion, not an aspiration. That is still what the pinned 7cfa2420
// defends every time a row is edited.
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
  uint32_t fluidCount = 0;       // MLS-MPM particles alive AFTER this tick's spawns
  uint32_t fluidSpawnCount = 0;  // MLS-MPM spawn ops this tick
  uint32_t windWakeCount = 0;    // wind primitive footprint chunks this tick
  // Water-body chunk-list entries this tick (docs/PLAN_water_master.md M2).
  // Mirrors rhi::TableCtx / vk_record.h, like every field here. Zero at
  // sim.waterBodyMode 0, which is what makes C_WATERBODY false and leaves the
  // whole subsystem unrecorded.
  uint32_t waterChunkCount = 0;
  uint32_t waterDrainBodies = 0;   // reserved drain op blocks (M3)
  // M5: which body's container curve re-derives this tick, or kWaterBodyCap
  // for "none" — which is every tick of a basin nobody has dug into, and is
  // what leaves both sweep rows unrecorded (C_WATERSWEEP).
  uint32_t waterSweepSlot = kWaterBodyCap;
  bool hashEnable = false;
  bool particlesActive = false;
  // False under --residency paged: worldgen's whole-world dispatch is replaced
  // by batched worldgenList submits (PLAN_page_table.md §3.5c).
  bool denseWorldgen = true;
  // False ONLY when the CPU can prove the dirty set is empty (§3.4). Mirrors
  // vk_record.h's field; defaults TRUE so the CA records unless proven idle.
  bool caActive = true;
  bool vizActive = false;
};

// NOTE: the condition and dispatch-extent resolvers that used to live here
// were the DAWN walk's copies. The Vulkan recorder has always carried its own
// (Recorder::CondHolds / Recorder::Extent in gpu/vk_record.cpp), which is the
// only pair left now that the Dawn walk is gone. They read the same
// pass::Cond / pass::DispatchSel enums, so pass_table.def stays the one
// declaration.

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
    case B::PageFillList:   return world_->pageFillList;
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
    case B::FarPatch:       return world_->farPatch;
    case B::PageTable:      return world_->pageTable;
    case B::PageFaults:     return world_->pageFaults;
    case B::FluidParticlesRead:  return world_->fluidParticles[page_];
    case B::FluidParticlesWrite: return world_->fluidParticles[1 - page_];
    case B::FluidSpawnOps:     return world_->fluidSpawnOps;
    case B::FluidBlockMap:     return world_->fluidBlockMap;
    case B::FluidBlockList:    return world_->fluidBlockList;
    case B::FluidGrid:         return world_->fluidGrid;
    case B::FluidArgsStage:    return world_->fluidArgsStage;
    case B::FluidDispatchArgs: return world_->fluidDispatchArgs;
    case B::FluidPDispatchArgs: return world_->fluidPDispatchArgs;
    case B::FluidExciteScratch: return world_->fluidExciteScratch;
    case B::FluidCalm:          return world_->fluidCalm;
    case B::FluidSettleScratch: return world_->fluidSettleScratch;
    case B::FluidCompactScratch: return world_->fluidCompactScratch;
    case B::FluidCellScratch:    return world_->fluidCellScratch;
    case B::FluidMirror:         return world_->fluidMirror;
    case B::ActVoxViz:           return world_->actVoxViz;
    case B::WaterBodyState:      return world_->waterBodyState;
    case B::TreeAtlas:           return treeAtlasBuf_;
    default:                return world_->voxels;
  }
}

const rhi::ComputePipeline& Simulation::PassPipeline(pass::Pipe p) const {
  using P = pass::Pipe;
  switch (p) {
    case P::Worldgen:       return worldgen_;
    case P::WorldgenList:   return worldgenList_;
    case P::PageFill:       return pageFill_;
    case P::Mutate:         return mutate_;
    case P::MutateCells:    return mutateCells_;
    case P::WindWake:       return windWake_;
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
    case P::FluidSpawn:     return fluidSpawn_;
    case P::FluidMark:      return fluidMark_;
    case P::FluidAlloc:     return fluidAlloc_;
    case P::FluidClear:     return fluidClear_;
    case P::FluidP2G:       return fluidP2g_;
    case P::FluidP2G2:      return fluidP2g2_;
    case P::FluidGridUp:    return fluidGridUp_;
    case P::FluidG2P:       return fluidG2p_;
    case P::FluidCompactCount:   return fluidCompactCount_;
    case P::FluidCompactScan:    return fluidCompactScan_;
    case P::FluidCompactScatter: return fluidCompactScatter_;
    case P::FluidExciteDetect:   return fluidExciteDetect_;
    case P::FluidExciteScan:     return fluidExciteScan_;
    case P::FluidExciteEmit:     return fluidExciteEmit_;
    case P::FluidPTick:          return fluidPTick_;
    case P::FluidSettleJudge:    return fluidSettleJudge_;
    case P::FluidSettleScan:     return fluidSettleScan_;
    case P::FluidSettleBin:      return fluidSettleBin_;
    case P::FluidSettleCheck:    return fluidSettleCheck_;
    case P::WaterQuiet:     return waterQuiet_;
    case P::WaterLedger:    return waterLedger_;
    case P::WaterReduce:    return waterReduce_;
    case P::WaterShave:     return waterShave_;
    case P::WaterDrain:     return waterDrain_;
    case P::WaterHole:      return waterHole_;
    case P::WaterSweep:     return waterSweep_;
    case P::WaterSplit:     return waterSplit_;
    case P::FluidSettleCommit:   return fluidSettleCommit_;
    case P::FluidSettleKill:     return fluidSettleKill_;
    case P::FluidConsumeApply:   return fluidConsumeApply_;
    case P::FluidStainApply:     return fluidStainApply_;
    case P::FluidMirrorFold:     return fluidMirrorFold_;
    case P::FluidCellClear:      return fluidCellClear_;
    default:                return step_;
  }
}

// Walk one table's rows and record them.
//
// A row whose condition is false is skipped entirely — no pass is opened for
// it, nothing is recorded, and no buffer's last-access state is touched.
// barrier_graph §3.9/§7.5: that is the only correct handling, and it is why
// barriers must be computed at record time against live state rather than
// precomputed per adjacent table-index pair.
//
// SINCE THE DAWN REMOVAL (2026-08-22) there is one walker again. The second
// one — an inline wgpu-shaped walk that opened a ComputePassEncoder per
// `group` string and let Dawn derive barriers — is gone with the backend it
// drove. What survives is the phase-3b/4a shape that mattered: the rows are
// the Vulkan recorder's LOOP VARIABLE, never a parameter that a call site
// could forget, and what crosses the bridge (rhi_record.h) is only the
// RESOLUTION — page-symbolic buffer ids and pipelines, resolved here by
// PassBuffer/PassPipeline.
void Simulation::RecordTable(const rhi::CommandEncoder& enc, pass::Table which,
                             const void* ctxOpaque) {
  const RecordCtx& cx = *(const RecordCtx*)ctxOpaque;

  rhi::TableCtx tc{};
  tc.opsCount = cx.opsCount;
  tc.cellCount = cx.cellCount;
  tc.expCount = cx.expCount;
  tc.spawnCount = cx.spawnCount;
  tc.genCount = cx.genCount;
  tc.farCount = cx.farCount;
  tc.fluidCount = cx.fluidCount;
  tc.fluidSpawnCount = cx.fluidSpawnCount;
  tc.windWakeCount = cx.windWakeCount;
  tc.waterChunkCount = cx.waterChunkCount;
  tc.waterDrainBodies = cx.waterDrainBodies;
  tc.waterSweepSlot = cx.waterSweepSlot;
  tc.hashEnable = cx.hashEnable;
  tc.particlesActive = cx.particlesActive;
  tc.denseWorldgen = cx.denseWorldgen;
  tc.caActive = cx.caActive;
  tc.vizActive = cx.vizActive;

  rhi::TableBindings tb{};
  for (int i = 0; i < (int)pass::Buf::kCount; i++)
    tb.buffers[i] = PassBuffer((pass::Buf)i);
  for (int i = 1; i < (int)pass::Pipe::FarDown + 1; i++)
    tb.pipelines[i] = PassPipeline((pass::Pipe)i);
  tb.simLayout = simPL_;
  tb.slimPartLayout = simPL2_;
  tb.slimFarLayout = farPL_;
  tb.slimFluidLayout = fluidPL_;
  tb.slimFluidSeamLayout = fluidSeamPL_;
  tb.simSet = simBG_[page_];
  tb.slimSet = simSlimBG_[page_];
  tb.particleSet = particleBG_[page_];
  tb.farSet = farBG_;
  tb.fluidSet = fluidBG_[page_];
  tb.fluidSeamSet = fluidSeamBG_[page_];

  rhi::RecordTableVulkan(enc, which, tc, tb,
                         passTimer_ && passTimer_->Valid() ? passTimer_ : nullptr);
}

void Simulation::EncodeWorldgen(const rhi::CommandEncoder& enc, bool denseGen) {
  page_ = 0;
  RecordCtx cx{};
  // Under --residency paged the caller runs worldgen BATCHED through
  // worldgenList instead (§3.5c) and passes false, which suppresses only the
  // whole-world dispatch — the fill rows still clear the transient buffers.
  cx.denseWorldgen = denseGen;
  RecordTable(enc, pass::Table::Worldgen, &cx);
  // A freshly generated world is maximally unsettled — the first hundreds of
  // ticks ARE the settling. Same self-declaration rule (§3.4).
  NoteWakeAll();
}

void Simulation::EncodeGenList(const rhi::CommandEncoder& enc, uint32_t count) {
  RecordCtx cx{};
  cx.genCount = count;
  RecordTable(enc, pass::Table::GenList, &cx);
  // Streamed-in chunks arrive with fresh terrain that has never settled, and
  // worldgenList marks them dirty. Same self-declaration rule as EncodeWakeAll
  // (§3.4): the function that dirties the set invalidates the latch.
  NoteWakeAll();
}

void Simulation::EncodeFarFill(const rhi::CommandEncoder& enc, uint32_t count) {
  RecordCtx cx{};
  cx.farCount = count;
  RecordTable(enc, pass::Table::FarFill, &cx);
}

// Materialize `count` JITTER pages from the (slot, entry) pairs the caller has
// already written into genList. Deliberately does NOT call NoteWakeAll: unlike
// worldgen and genList, this changes only WHERE the world is stored, never what
// it is, so it must not invalidate the settled-skip latch (world.h's JITTER
// block, and the kernel's closing comment).
void Simulation::EncodePageFill(const rhi::CommandEncoder& enc, uint32_t count) {
  if (count == 0) return;
  RecordCtx cx{};
  cx.genCount = count;
  RecordTable(enc, pass::Table::PageFill, &cx);
}

void Simulation::EncodeLoadReset(const rhi::CommandEncoder& enc) {
  page_ = 0;
  RecordCtx cx{};
  RecordTable(enc, pass::Table::LoadReset, &cx);
  // A load replaces every voxel in the window and the caller has already
  // written both dirty pages (worldio.cpp). Nothing the latch believed about
  // the previous world survives that (§3.4).
  NoteWakeAll();
}

void Simulation::EncodeHashOnly(const rhi::CommandEncoder& enc) {
  RecordCtx cx{};
  RecordTable(enc, pass::Table::HashOnly, &cx);
}

void Simulation::EncodeWakeAll(const rhi::Queue& queue) {
  // dirty[page_] is the buffer the NEXT compact pass reads (dirtyIn). One u32
  // flag per chunk; 32768 chunks = 128 KB, far inside the ~1 MB/tick CPU->GPU
  // budget, and only written on a phase boundary.
  static const std::vector<uint32_t> ones(kNumChunks, 1u);
  queue.WriteBuffer(world_->dirty[page_], 0, ones.data(),
                    ones.size() * sizeof(uint32_t));

  // THE WAKE IS A DIRTY-SET MUTATION, so it mutates the CPU mirror of the
  // dirty set in the SAME CALL (PLAN_page_table.md §3.2a fix 1). Two
  // operations that must agree is the shape this repo has a checker for; one
  // operation cannot disagree with itself.
  //
  // Why this was the most dangerous hole in the design: without it, every
  // chunk in the window becomes dirtyIn next tick and may write, while
  // cpuDirty is near-empty because the world was settled — SILENT VOXEL LOSS
  // AT EVERY DAWN AND EVERY DUSK. And no gate would catch it: the suite pins
  // the day phase in both directions (selftest_sim.cpp freezes at midnight and
  // at noon), so wasDay != isDay is never true and this function is never
  // called. Gate D exists precisely because of that.
  //
  // Unioned at step (3) of the normative definitions — strictly AFTER the
  // tightening — so the 32,768 chunks survive regardless of when a snapshot
  // happened to land. What stops this demanding 32,768 PAGES from an
  // 8,192-page pool (a guaranteed abort twice per in-game day, under §3.8) is
  // the `n nonSentinel` filter on the bracketed half of the materialization
  // set: a dirty EMPTY chunk holds no matter, so nothing in it can move, and
  // the only way it can receive matter is from a neighbour that has some.
  if (world_->pages) world_->pages->WakeAll();
  // Same doctrine, second mirror: the §3.4 settled-skip latch is also a CPU
  // mirror of the dirty set, so the wake invalidates it HERE rather than at a
  // call site. This function is the only writer of all-ones into dirtyIn, and
  // it is now the only place that has to know that.
  NoteWakeAll();
}

// ---------------------------------------------------------------------------
// The settled-tick skip (ROADMAP_scale.md §3.4). See simulation.h for why this
// is safe; what follows is why it is CORRECT, which is a different question.
//
// The CA may be skipped for tick T only if dirtyIn(T) is empty. The CPU cannot
// read dirtyIn — it lives on the GPU — so it proves the statement from two
// facts it does own:
//
//   (1) a snapshot stamped at tick S reported ZERO active chunks. The snapshot
//       carries dirtyOut(S), and dirtyOut(S) IS dirtyIn(S+1) (the page flip is
//       the only thing between them — pagetable.cpp:296 states the same
//       identity for the same reason). So dirtyIn(S+1) is empty.
//
//   (2) no CPU input has dirtied a chunk since S. Every such input is declared
//       through NoteTickInputs / NoteWakeAll, which stamp lastDirtyTick_.
//
//   (3) that same snapshot reported ZERO live particles (§3.2d). The CA is NOT
//       the only writer of dirtyOut: sim_particle's `resolve` reinserts a
//       particle into the grid, which is a voxStore plus a markDirtyNext
//       (sim_particle.wgsl:251,:274). So "the dirty set is empty" is only half
//       of settled — the other half is "nothing is in flight that could refill
//       it from the GPU side, at a location the CPU never chose".
//
// Given all three, dirtyIn stays empty for every tick after S+1: an empty
// dirtyIn dispatches no CA workgroups and no workgroups write no dirty flags,
// and an empty particle read page dispatches no integrate and no resolve. The
// world is a fixed point, and it stays one until a CPU input breaks it. That is
// the induction the skip rests on, and it is why facts (2) and (3) must cover
// EVERY waking path rather than just brush ops.
//
// WHY (3) IS A SNAPSHOT CONJUNCT AND NOT A TIMER (§3.2d, the fix)
//
// Until 2026-08-24 (3) was carried by main.cpp's `particlesActive`, which is
// `everExploded && (tick - lastExplosionTick < 400 || particleCount > 0)`, fed
// into EncodeTick's `inputsThisTick`. That term re-stamped lastDirtyTick_ on
// EVERY tick for 13.3 seconds of wall clock after any explosion, so
// settledProven_ could never latch and the whole §3.4 mechanism was off. In
// --measure's ACTIVE scenario, 66 of 120 ticks had a provably empty dirty set
// and still recorded 54 indirect dispatches, the 32,768-flag compact scan and
// the args staging copy: ~17% of that scenario's CA GPU time spent on nothing.
// The 400-tick timer is a blunt stand-in for "a snapshot old enough to be
// conclusive", which is exactly what snapTick >= lastDirtyTick_ already is.
//
// THE REINSERTION WINDOW, and why it is CLOSED rather than merely narrow. The
// hazard to design against is the one-tick gap between a particle rejoining the
// grid and the CPU learning of it: a skip taken in that gap does not corrupt
// anything, it processes a dirty chunk one tick LATE, and the world hash moves.
// It cannot happen here because the two conjuncts are read from ONE snapshot
// word, captured at ONE point in the tick, downstream of the writer:
//
//   - within a tick the pass table records ... ca, particleIntegrate,
//     particleResolve, occupancy ... and World::EncodeReadbacks is recorded
//     after ALL of it. So the dirty flags in snapshot S already carry
//     `resolve`'s markDirtyNext for tick S. A landing is never invisible to
//     the snapshot that reports its own tick.
//   - snapshot S's particleCount is counts[1 - page] (support.cpp passes
//     `1 - sim.Page()` as particleLivePage), which is the count `integrate`
//     built at S and `resolve` then consumed at S — the exact population
//     resolve ran over. particleCount(S) == 0 therefore means resolve at S
//     processed zero particles, and that the read page for S+1 is empty, so
//     integrate at S+1 dispatches nothing and appends nothing. Zero is
//     self-propagating, which is what makes it an induction base.
//   - a particle that lands at S is still COUNTED at S (resolve clears its
//     flags but never decrements counts), so the landing tick reports
//     particleCount > 0 on top of activeChunks > 0. The conjunct errs one tick
//     in the safe direction at exactly the moment that matters.
//   - a particle spawned at S — explosion ejecta (sim_explode.wgsl:153,:175),
//     a CPU shatter spawn (sim_particle.wgsl:97) — appends to the READ page and
//     flies the same tick, so it too is inside counts[1-page] at S. MPM splash
//     droplets append to the WRITE page (sim_fluid.wgsl:924,:1116) from tables
//     recorded after PT_TICK, so they are also inside it. There is no spawn
//     path whose product is invisible to the snapshot of its own tick.
//
// So the CPU is not predicting the future here; it is reading one consistent
// end-of-tick state and propagating a fixed point forward. `particlesActive`
// keeps gating the particle PASSES (it must — dropping it mid-flight strands
// live particles), it just no longer speaks for the CA.
//
// The failure mode to design against is staleness, not logic: `valid` snapshots
// arrive one tick latent at best and can lag when the readback ring saturates.
// Requiring snapTick >= lastDirtyTick_ handles it — a snapshot older than the
// last dirtying input proves nothing about the state that input created, and is
// rejected rather than trusted.
// ---------------------------------------------------------------------------
void Simulation::NoteTickInputs(uint32_t tick, bool dirtiedNow) {
  curTick_ = tick;
  if (dirtiedNow) {
    lastDirtyTick_ = tick;
    // A fresh input invalidates any settled proof immediately: the snapshot
    // that proved it predates the write this tick is about to make.
    settledProven_ = false;
  }
}

void Simulation::NoteWakeAll() {
  // Stamped with the CURRENT tick rather than a forever-dirty sentinel. One
  // that no snapshot tick can ever exceed is not "conservative", it is a
  // permanent latch: the world settles, every snapshot reports zero, and the
  // skip never fires again for the life of the process. That is what the first
  // draft of this did, and --measure reporting `CA skipped on 0 / 120` in a
  // provably settled world is what caught it.
  //
  // A real tick number is both conservative AND recoverable: no snapshot
  // stamped BEFORE the wake can satisfy `snapTick >= lastDirtyTick_`, so the
  // wake's dirty flags can never be reasoned away, but a snapshot taken after
  // the woken chunks settle again can.
  lastDirtyTick_ = curTick_;
  settledProven_ = false;
}

void Simulation::NoteSnapshot(uint32_t snapTick, uint32_t activeChunks,
                              uint32_t particleCount) {
  // Fact (3): voxels in flight are a GPU-side dirty-writer with no CPU-known
  // target, so a non-empty particle population is "not settled" no matter what
  // the dirty flags say. Tested first because it is the cheap disqualifier and
  // because reading it as an ELSE of activeChunks would hide it.
  //
  // A stale non-zero count can only COST a skip, never license one. That is
  // the safe direction, and it is bounded in practice: the counts are zeroed
  // per tick while the particle pipeline runs, so the population genuinely
  // reaches 0 a couple of ticks after the last particle dies and stays there.
  if (activeChunks != 0 || particleCount != 0) {
    settledProven_ = false;
    return;
  }
  // Zero active chunks and zero particles. Conclusive only if nothing dirtied
  // the world at or after the tick this snapshot was stamped at — an older
  // snapshot describes a world that no longer exists.
  if (snapTick >= lastDirtyTick_) settledProven_ = true;
}

void Simulation::EncodeTick(const rhi::CommandEncoder& enc, uint32_t opsCount,
                            bool hashEnable, uint32_t expCount, bool particlesActive,
                            uint32_t cellCount, uint32_t spawnCount,
                            uint32_t fluidCount, uint32_t fluidSpawnCount,
                            uint32_t windWakeCount, bool vizActive,
                            uint32_t waterChunkCount,
                            uint32_t waterDrainBodies,
                            uint32_t waterSweepSlot) {
  RecordCtx cx{};
  cx.opsCount = opsCount;
  cx.cellCount = cellCount;
  cx.expCount = expCount;
  cx.spawnCount = spawnCount;
  cx.fluidCount = fluidCount;
  cx.fluidSpawnCount = fluidSpawnCount;
  cx.windWakeCount = windWakeCount;
  // Water bodies (docs/PLAN_water_master.md M2). Zero at sim.waterBodyMode 0,
  // which is what makes C_WATERBODY false and the whole subsystem unrecorded.
  cx.waterChunkCount = waterChunkCount;
  // M3: the reserved discharge op blocks. C_WATERDRAIN, and zero whenever
  // the feature is off or nothing is proposed.
  cx.waterDrainBodies = waterDrainBodies;
  // M5: the scheduled re-derive. C_WATERSWEEP, and kWaterBodyCap ("none")
  // on every tick of a basin nobody has dug into.
  cx.waterSweepSlot = waterSweepSlot;
  cx.hashEnable = hashEnable;
  cx.particlesActive = particlesActive;
  cx.vizActive = vizActive;

  // §3.4. The counts are re-tested here as a BACKSTOP, not as the primary
  // signal: NoteTickInputs is the declaration and a caller that forgets it
  // would otherwise skip a tick that mutates the world. Testing what this call
  // actually carries means the ONE thing that can silently break the skip —
  // an undeclared op — cannot break it through the op path itself.
  //
  // particlesActive is deliberately NOT in this disjunction (§3.2d). It is not
  // an input at all — it is "the particle pipeline is recorded this tick", a
  // flag main.cpp latches for 400 ticks after any explosion so that spent
  // ejecta finishes its arc. Every tick on which a particle can be CREATED is
  // already listed here (expCount for ejecta, spawnCount for shatter fragments,
  // fluidCount/fluidSpawnCount for MPM splash), and the population that already
  // exists is covered by NoteSnapshot's particleCount conjunct, which is exact
  // rather than a timer. Putting it here re-stamped lastDirtyTick_ every tick
  // for 13.3 s after every explosion and disabled the whole skip.
  // windWakeCount belongs in this disjunction for exactly the reason the
  // backstop exists: a wind primitive's wake IS a chunk-dirtying input, and a
  // settled world with a fan pointed at a dune would otherwise prove itself
  // idle and skip the CA rows the wake had just made necessary — the fan would
  // mark chunks nothing then simulated.
  const bool inputsThisTick = opsCount > 0 || expCount > 0 || cellCount > 0 ||
                              spawnCount > 0 || windWakeCount > 0 ||
                              fluidCount > 0 || fluidSpawnCount > 0;
  if (inputsThisTick) {
    lastDirtyTick_ = curTick_;
    settledProven_ = false;
  }
  cx.caActive = !settledProven_;
  // MEASUREMENT / TEST ONLY (SetCaForced, SANDVOX_CA_FORCE=1): record the CA
  // rows unconditionally. On a settled tick that means 54 indirect dispatches
  // with an indirect count of ZERO — no invocation, no write, so the world hash
  // is bit-identical, which is precisely what makes it a usable oracle for the
  // `ca-skip` gate and a direct read of the content-free dispatch floor.
  {
    static const bool kForceCaEnv = [] {
      const char* e = std::getenv("SANDVOX_CA_FORCE");
      return e && e[0] == '1';
    }();
    if (caForced_ || kForceCaEnv) cx.caActive = true;
  }
  caSkipped_ = !cx.caActive;
  if (caSkipped_) caSkipCount_++;

  RecordTable(enc, pass::Table::Tick, &cx);

  // MLS-MPM fluid: seam front half (compaction, spawns, excite), the substep
  // table kFluidSubsteps times, then the seam back half (settle) — all into
  // the SAME command buffer (FarFill precedent), so the recorder's persistent
  // last-access tracker generates every inter-table barrier. Recorded while
  // the seam is LIVE: particles may exist (the CPU's conservative estimate),
  // spawns arrive this tick, or the disturbance-excite mode is on with an
  // active CA (excite can birth particles into an empty pool — and once it
  // does, the emitted dirt keeps caActive true until the readback catches
  // up, so this predicate can never strand live particles unsimulated).
  // Every input here is tick-deterministic — never frame timing (rule 1).
  const bool exciteOn = CurrentTuning().sim.fluidExciteMode != 0;
  const bool seamActive =
      fluidCount > 0 || fluidSpawnCount > 0 || (exciteOn && cx.caActive);
  if (seamActive) {
    RecordTable(enc, pass::Table::FluidSeam, &cx);
    // The chunk->block map is built ONCE here, not once per substep: max
    // displacement is 2.7 cells/tick against `mark`'s 3-cell pad, so the map a
    // substep would have rebuilt is the map it already has (plan §7 item 4,
    // and the PT_FLUIDMAP block in pass_table.def). Recorded after the seam
    // because exciteEmit and spawnAppend create particles it must cover.
    RecordTable(enc, pass::Table::FluidMap, &cx);
    // The substep count is a tuning knob (sim.fluidSubsteps): the CFL budget
    // and the solver's price are the same number. Reading CurrentTuning() at
    // record time is the same shape as exciteOn above — tuning is a
    // tick-deterministic input, never frame timing. The shader side derives
    // FLUID_SUBSTEPS from the identical knob, so the recorded count and the
    // compiled-in divisors cannot disagree.
    const uint32_t substeps = (uint32_t)std::clamp(
        CurrentTuning().sim.fluidSubsteps, 1, 32);
    for (uint32_t s = 0; s < substeps; s++)
      RecordTable(enc, pass::Table::Fluid, &cx);
    RecordTable(enc, pass::Table::FluidSettle, &cx);
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

void Simulation::EnsureAuxDepth(uint32_t width, uint32_t height) {
  if (auxDepthView_ && auxDepthW_ == width && auxDepthH_ == height) return;
  auxDepthW_ = width;
  auxDepthH_ = height;
  auxDepthTex_ =
      device_.CreateTexture({width, height, 1}, kDepthFormat,
                            rhi::TextureUsage::RenderAttachment, "depthAux");
  auxDepthView_ = auxDepthTex_.CreateView();
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

    // MLS-MPM fluid prototype: same module, same layout, own entry point.
    // Opaque cubes for now — translucency across thousands of unsorted cubes
    // is a z-fighting mess, and the comparison the prototype exists for reads
    // fine with solid water-coloured droplets.
    d.vertexEntry = "vsFluid";
    d.label = "fluidDraw";
    fluidDraw_ = device_.CreateRenderPipeline(d);
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

    // Wind slope-field arrows (docs/RESEARCH_wind.md §4.8). Same module story
    // as the wireframes — its own file, the SAME pipeline layout, so no new
    // bind group and (since it reads no storage buffer at all) nothing for
    // pass_table.def either: that table describes the sim's COMPUTE recording,
    // and a render draw is not in it.
    //
    // Where this differs from the boxes: it is DEPTH TESTED. A collider you
    // cannot see through a wall is useless because the point is the collider
    // inside the wall; an arrow field you can see through the ground is
    // actively misleading, because "is the wind above or below this ridge"
    // is exactly the question being asked. Writes stay off so arrows do not
    // occlude each other or the world they annotate.
    rhi::DepthState dsWind{};
    dsWind.format = kDepthFormat;
    dsWind.depthWriteEnabled = false;
    dsWind.depthCompare = rhi::CompareFunction::GreaterEqual;  // reversed-Z

    d.label = "debugWindDraw";
    d.vertexModule = debugWindModule_;
    d.vertexEntry = "vsArrow";
    d.fragmentModule = debugWindModule_;
    d.fragmentEntry = "fsArrow";
    d.depth = dsWind;
    debugWindDraw_ = device_.CreateRenderPipeline(d);

    // The CURRENT field's arrows (water plan component 8). Same pipeline
    // state, same depth rule, same argument for it — a different field.
    d.label = "debugCurrentDraw";
    d.vertexModule = debugCurModule_;
    d.vertexEntry = "vsCurArrow";
    d.fragmentModule = debugCurModule_;
    d.fragmentEntry = "fsCurArrow";
    d.depth = dsWind;
    debugCurrentDraw_ = device_.CreateRenderPipeline(d);
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

rhi::RenderPass Simulation::BeginAuxRenderPass(const rhi::CommandEncoder& enc,
                                               const rhi::TextureView& target,
                                               rhi::TextureFormat format,
                                               uint32_t width, uint32_t height,
                                               const float clear[4]) {
  EnsureRenderPipelines(format);
  EnsureAuxDepth(width, height);

  rhi::RenderPassDesc d{};
  d.label = "aux";
  d.color.view = target;
  d.color.loadOp = rhi::LoadOp::Clear;
  d.color.storeOp = rhi::StoreOp::Store;
  d.color.clearValue[0] = clear[0];
  d.color.clearValue[1] = clear[1];
  d.color.clearValue[2] = clear[2];
  d.color.clearValue[3] = clear[3];
  d.hasDepth = true;
  d.depth.view = auxDepthView_;
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

void Simulation::DrawFluid(const rhi::RenderPass& pass, uint32_t count) {
  if (count == 0) return;   // no fluid placed: costs nothing
  pass.SetPipeline(fluidDraw_);
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

void Simulation::DrawWindField(const rhi::RenderPass& pass, uint32_t arrows) {
  if (arrows == 0) return;   // overlay off: costs nothing, not even a bind
  pass.SetPipeline(debugWindDraw_);
  pass.SetBindGroup(0, renderBG_);
  pass.SetBindGroup(1, renderPartBG_[page_]);
  // 3 segments (shaft + two head barbs) x 6 vertices (two triangles per
  // segment quad). No vertex or instance buffer: the shader derives its
  // lattice point from the instance index and R.camPos.
  pass.Draw(18, arrows);
}

void Simulation::DrawCurrentField(const rhi::RenderPass& pass,
                                  uint32_t arrows) {
  if (arrows == 0) return;   // overlay off: costs nothing, not even a bind
  pass.SetPipeline(debugCurrentDraw_);
  pass.SetBindGroup(0, renderBG_);
  pass.SetBindGroup(1, renderPartBG_[page_]);
  pass.Draw(18, arrows);
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
