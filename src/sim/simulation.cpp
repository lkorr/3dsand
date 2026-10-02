#include "sim/simulation.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gpu/resources.h"
#include "sim/bodyreact.h"  // CatchFormTable (what a coat flame lights, §6 clause 2c)
#include "sim/farplumes.h"  // FarPlumes::SetMaterials (what a frozen fire is)
#include "sim/pagetable.h"
#include "sim/rainexpo.h"  // the rain lattice (rainFall / rainExpo extents)
#include "sim/renderspec.h"  // LastRenderSpec(): which raymarch variant this frame takes
#include "sim/tuning.h"      // fluidExciteMode gates the seam recording
#include "gpu/rhi_record.h"  // the Vulkan table-recording bridge (phase 4a)
#include "gpu/rhi_vk.h"      // rhi::vkr::SavePipelineCache (EnsureRenderPipelines)
#include "sim/worldmap.h"    // CurrentWorldMap().pondTile: the POND_TILE prelude const (P-F)
#include "sim/solutes.h"     // LoadSolutes: the species table UploadTables packs
#include "sim/heat.h"        // the temperature layer's layout (heatParams, PackHeatMaterial)
#include "test/support.h"    // AssetDir(): the one asset-path chokepoint (solutes.json)

// The pond lattice the live shaders were compiled with (POND_TILE), so an
// environment reload that moves it recompiles them (UploadEnvironment).
static int sPondTileCompiled = -1;
// The same for the map's reference scale (REF_VOXELS_PER_METRE, P-G).
static int sRefVpmCompiled = -1;

// kPassStride (the passUBO dynamic-offset slice stride) moved to pass_table.h
// when the Vulkan recorder became a second consumer of it — see the note there.
using pass::kPassStride;

// ---- the worldgen COLUMN CACHE's layout (worldgen.wgsl, same-named block) --
// kColCacheWords / kColCacheHdr ARE the shader's CC_WORDS / CC_HDR
// (check_invariants.py holds them together); a block is one chunk-column.
// kGenColMax is how many distinct chunk-columns one list can name: the window's
// kNChunk^2, plus every ticket box's own kTicketBoxN^2 (tickets map slots
// outside the window). genCols holds the per-POSITION map ([0, kNumSlots),
// worldgen.wgsl GC_REC_BASE) and then kGenColRecWords words per chunk-column.
namespace {
constexpr uint32_t kColCacheWords = 25;
constexpr uint32_t kColCacheHdr = 16;
constexpr uint32_t kColCacheBlock = kColCacheHdr + kChunk * kChunk * kColCacheWords;
constexpr uint32_t kGenColMax = kNChunk * kNChunk + kTicketMax * kTicketBoxN * kTicketBoxN;
constexpr uint32_t kGenColRecWords = 4;
}  // namespace

bool Simulation::Init(const rhi::Device& device, World& world,
                      const std::vector<MaterialDef>& mats,
                      const std::vector<ReactionGpu>& reactions,
                      const MicroSet& micro, const TreeAtlas& trees,
                      const std::vector<uint32_t>& worldMapWords,
                      const std::string& shaderDir) {
  world_ = &world;
  device_ = device;
  shaderDir_ = shaderDir;
  rhi::Queue queue = device.GetQueue();

  // The tree lattice the atlas was loaded against becomes worldgen.wgsl's
  // TREE_TILE / TREE_SCAN / TREE_CAND_MAX prelude constants from here on
  // (gpu/resources.cpp WorldgenPrelude). Set BEFORE the first
  // LoadShader below, and it stays set for F5 reloads and --shader-stats.
  treeatlas::SetCurrentTreeLattice(trees.lattice);
  // The pond lattice the same way (POND_TILE, from the map already set as
  // CurrentWorldMap by the caller); remembered so UploadEnvironment knows
  // whether a reload moved it under the compiled shaders.
  sPondTileCompiled = worldmap::CurrentWorldMap().pondTile;
  sRefVpmCompiled = worldmap::CurrentTerrain().refVoxelsPerMetre;

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

  // The authored world map (src/sim/worldmap.h, docs/PLAN_world_map.md). Same
  // standing as the tree atlas above: read-only, dispatch-invariant asset data
  // uploaded once at load, sampled per column by worldgen. Created here because
  // it is a bind-group entry, and sized to the loaded map with a header-only
  // floor for the same reason -- a zero-length storage binding is not legal,
  // and "no map yet" has to be a world rather than a crash.
  //
  // P0 bound it EMPTY (plumbing proven with the hash pinned); P1 uploads the
  // biome record table worldmap::PackBiomeTable produced from
  // assets/biomes/*.json. Planes and sites extend the same words in P2/P5.
  worldMapWords_ = std::max<size_t>(worldMapWords.size(), worldmap::kHeaderWords);
  worldMapBuf_ = CreateBuffer(device, (uint64_t)worldMapWords_ * 4,
                              rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                              "worldMap");
  {
    std::vector<uint32_t> pad = worldMapWords;
    pad.resize(worldMapWords_, 0u);
    queue.WriteBuffer(worldMapBuf_, 0, pad.data(), pad.size() * 4);
  }

  // The worldgen column cache (worldgen.wgsl's block; WriteGenList). Sized for
  // the widest list: kGenColMax chunk-columns of 256 cached columns each —
  // 24 MiB at a 512^3 window, written only by the `cols` pre-pass.
  genColsBuf_ = CreateBuffer(device, (uint64_t)(kNumSlots + kGenColMax * kGenColRecWords) * 4,
                             rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                             "genCols");
  colCacheBuf_ = CreateBuffer(device, (uint64_t)kGenColMax * kColCacheBlock * 4,
                              rhi::BufferUsage::Storage, "colCache");

  materialBuf_ =CreateBuffer(device, sizeof(MaterialGpu) * 4096,
                              rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                              "materials");
  artPaletteLive_ = false;  // a new buffer holds no palette write
  reactionBuf_ = CreateBuffer(device, sizeof(ReactionGpu) * kMaxReactions,
                              rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                              "reactions");
  // The solute species table (world.h kSolSpec*): fixed size, filled by the
  // UploadTables below and by every materials reload after it.
  solSpecBuf_ = CreateBuffer(device, (uint64_t)kSolSpecWords * 4,
                             rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                             "solSpec");
  // The temperature layer's parameters (heat.h kHp*): the per-material words
  // are filled by UploadTables, the header and the column table by
  // PrepareHeat; zeroed once here so an unwritten region reads as "nothing".
  heatParamsBuf_ = CreateBuffer(device, (uint64_t)kHpWords * 4,
                                rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                                "heatParams");
  {
    static const std::vector<uint32_t> zeros(kHpWords, 0u);
    queue.WriteBuffer(heatParamsBuf_, 0, zeros.data(), zeros.size() * 4);
    heatColValid_ = false;
  }
  UploadTables(queue, mats, reactions);
  // The solute layer's free stack and empty table, before any tick can
  // allocate (a zeroed meta record would read as an EMPTY STACK, and the first
  // dissolve would be a fatal exhaustion). Worldgen and loads reset it again.
  world_->ResetSolutes(queue);

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
  mbModelLive_ = false;  // a new buffer holds no table write
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
        // group) call genCellIn and therefore call the tree sampler.
        entry(26, T::ReadOnlyStorage), // treeAtlas
        // The openness grid (world.h kOpenFaces, docs/PLAN_gi.md §2). Written
        // by sim_openness.wgsl and by nothing else; here rather than in a
        // group of its own because sim_openness already needs occupancy (7),
        // dirtyList (12), pageTable (17) and TickParams (4), i.e. most of this
        // layout — a private group would have to re-bind five buffers to save
        // two descriptors on pipelines that never name them.
        entry(27, T::Storage),         // openness (per block-face bytes)
        entry(28, T::Storage),         // opennessGen (per-slot world stamp)
        // The irradiance grid (world.h kIrradianceBytes, docs/PLAN_gi.md §3):
        // the openness walk decays it and deposits the off-screen sun sample.
        entry(29, T::Storage),         // irradiance (per block-face RGB9E5)
        // The deferred streaming wake's act verdict (world.h `genAct`,
        // docs/RESEARCH_streaming_hitch.md R1). simBGL_ only: `far`/`fardown`
        // run on the slim group and never reach genChunk, exactly like
        // pageFillList at 19.
        entry(30, T::Storage),         // genAct (per genList slot)
        // The authored world map (docs/PLAN_world_map.md). Read-only asset
        // data, exactly like treeAtlas at 26 -- and binding 31 in BOTH this
        // layout and simSlimBGL_ for the same reason 17/18 and 26 are: one
        // WGSL identifier cannot carry two binding numbers across modules that
        // share common.wgsl, and `far`/`fardown` run on the slim group and
        // reach the biome sampler (farSurfaceMat -> treeCanopyAt ->
        // treeInfoAt -> biomeAt).
        //
        // 31 and not 27: docs/PLAN_biomes.md §5(ii) says "27 is free", which
        // was true when it was written and stopped being true when openness
        // (27/28), irradiance (29) and genAct (30) landed. This layout is a
        // dense 0..30, so 31 is the first free slot.
        entry(31, T::ReadOnlyStorage), // worldMap
        // The glow field (src/sim/world.h kGlowBytes). Storage, not
        // ReadOnlyStorage: sim_glow.wgsl is its only writer and it reads the
        // slot's previous source word back to decide whether it moved. 32 is
        // the first free slot in this dense 0..31 layout, and it is NOT
        // mirrored in simSlimBGL_ -- the one-identifier-one-binding-number rule
        // above only binds layouts a shader declaring `glow` is recorded
        // against, and no slim-group pipeline names it.
        entry(32, T::Storage),         // glow (sources + field)
        // The window-edge gas outbox (docs/PLAN_gas_particles.md §2.3).
        // sim_step's gasLeave appends to it; sim_gas drains it later in the
        // same tick through the GAS group, where it is binding 3. The two
        // numbers differ on purpose and legally: `gasSpawn` is declared in two
        // modules that do NOT share the declaration through common.wgsl, which
        // is the condition the one-identifier-one-binding-number rule above is
        // about. 33 is the first free slot in this dense 0..32 layout.
        entry(33, T::Storage),         // gasSpawn (header + records)
        // The outer gas density box (docs/PLAN_gas_particles.md §2.5). Bound
        // to the CA since stage 1b, which splats every in-window gas VOXEL
        // into it once per tick so the renderer can crossfade a voxel plume
        // into the coarse one instead of cutting between them at the face.
        // Binding 6 in the GAS group and 21 in renderBGL_; the numbers differ
        // for gasSpawn's reason -- `gasOuter` is declared in three modules
        // that do NOT share the declaration through common.wgsl, which is the
        // condition the one-identifier-one-binding-number rule is about.
        // 34 is the first free slot in this dense 0..33 layout.
        entry(34, T::Storage),         // gasOuter (render-only density box)
        // The repose occupancy snapshot (world.h kReposeSnap*). Storage, not
        // ReadOnlyStorage: the `reposesnap` prepass writes it and `main` reads
        // it, both entry points of sim_step.wgsl, which WGSL cannot give two
        // access modes for one binding. 35 is the first free slot in this dense
        // 0..34 layout, and it is NOT mirrored in simSlimBGL_ -- no slim-group
        // pipeline names it.
        entry(35, T::Storage),         // reposeSnap (bits + per-slot stamps)
        // The surface-momentum store (docs/PLAN_water_relevel.md §4.1,
        // world.h kWaterFluxWords). Storage and not ReadOnlyStorage:
        // `wbFlux` writes the pipes, `wbSurface` writes the height and
        // the stamp, and `wbRelevel` reads both -- three entry points of
        // one module, which WGSL cannot give two access modes for one
        // binding. 36 is the first free slot in this dense 0..35 layout,
        // and it is NOT mirrored in simSlimBGL_: only sim_waterbody.wgsl
        // names it, and no slim-group pipeline does.
        entry(36, T::Storage),         // waterFlux (4 pipes + s + stamp)
        // The per-chunk digest table (world.h kChunkHashWords,
        // docs/PLAN_multiplayer_m9.md M9.3-A). Storage and not
        // ReadOnlyStorage: sim_occupancy.wgsl's `main` is its only writer and
        // nothing on the GPU reads it back. 37 is the first free slot in this
        // dense 0..36 layout, and it is NOT mirrored in simSlimBGL_ -- only
        // sim_occupancy names it, and no slim-group pipeline does.
        entry(37, T::Storage),         // chunkHash (per-slot digest + tick)
        // The worldgen column cache (worldgen.wgsl's block of that name,
        // Simulation::WriteGenList). genCols is CPU-written list input;
        // colCache is Storage because `cols` writes it and `main`/`list` read
        // it, three entry points of one module. Neither is mirrored in
        // simSlimBGL_: `far`/`fardown` never reach genChunk or `cols`.
        entry(38, T::ReadOnlyStorage), // genCols (list position -> chunk-column)
        entry(39, T::Storage),         // colCache (per-(x, z) column records)
        // The solute layer (world.h kSol* block, docs/PLAN_solutes.md). The
        // table and the meta record are Storage because sim_solute.wgsl's
        // allocator writes them; the CA reads the table and writes the pool
        // (atomically -- two 16-bit cells share a word) and the meta record
        // (requests, the ledger counters). simBGL_ only: no slim-group
        // pipeline names them. 40..43, the first free slots of this dense
        // 0..39 layout.
        entry(40, T::Storage),         // solTable (per-slot sentinel / page)
        entry(41, T::Storage),         // solPool (16-bit cells, 2 per word)
        entry(42, T::Storage),         // solMeta (free stack, lists, ledger)
        entry(43, T::ReadOnlyStorage), // solSpec (solutes.json, per material)
        entry(44, T::Storage),         // solStage (eviction / restore records)
        // The rain exposure map (sim_rain_expo.wgsl, src/sim/rainexpo.h):
        // written by its own row, read by the CA. simBGL_ only. 45 is the
        // first free slot of this dense 0..44 layout.
        entry(45, T::Storage),         // rainExpo (per lattice texel)
        // The wind-draft shelter volume (sim_draft.wgsl, world.h kDraft*):
        // masks + coarse solver + transfer field, and its meta words (the
        // changed flag, counters, the solve's args stage). 46 is ALSO in
        // simSlimBGL_: windAtQ reads it through common.wgsl, and sim_particle
        // / sim_fluid run on the slim layout -- one identifier, one number.
        entry(46, T::Storage),         // draft (masks | coarse | field)
        entry(47, T::Storage),         // draftMeta (atomic words)
        // The CA's air mask (sim_step.wgsl camask): simBGL_ only.
        entry(48, T::Storage),         // caMask (1 bit per cell, per slot)
        entry(49, T::Storage),         // caWind (ambient wind per 4^3 block)
        // The temperature layer (src/sim/heat.h, sim_heat.wgsl): the pool and
        // the meta record are GPU-owned (the CA reads the pool and raises
        // flags / counters in the meta); the params are CPU-written.
        // simBGL_ only.
        entry(50, T::Storage),         // heatPool (per-block X / X* / sources)
        entry(51, T::Storage),         // heatMeta (table, flags, lists, stack)
        entry(52, T::ReadOnlyStorage), // heatParams (climate, transitions, columns)
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
        // The compiled reaction table, SAME binding number as in simBGL_ (the
        // one-identifier-one-binding rule above). The excite/settle seam's
        // particleTick reads it (rule-unification W1-B1): an excited particle
        // wakes its chunk only when one of its own UNGATED rules has a
        // partner beside it, which it cannot know without the bucket.
        entry(11, T::ReadOnlyStorage), // reactions
        // Support-loss flags, SAME binding number as in simBGL_ for the reason
        // the page-table note above gives: one WGSL identifier cannot carry two
        // binding numbers across modules that share common.wgsl, and the
        // SUPPORT_LOSS block is shared exactly that way. sim_explode runs on
        // this layout and is the engine's biggest remover of supporting matter.
        entry(15, T::Storage),         // supportOut
        entry(17, T::ReadOnlyStorage), // pageTable
        entry(18, T::Storage),         // pageFaults
        // COMPONENT 7 put the water-body ledger in the SLIM group: the
        // excite/settle seam runs on this layout and its drain-shell trigger
        // asks a draining hole where it is. Binding 24 has to be the same
        // buffer in every module that names it, so it is added here rather
        // than given a seam-group entry of its own.
        entry(24, T::Storage),         // waterBodyState (atomic i32 ledger)
        // Same binding number as in simBGL_ above; `fardown`/`far` build on
        // this layout and both reach genCellIn -> treeAt.
        entry(26, T::ReadOnlyStorage), // treeAtlas
        // Same binding number as in simBGL_ above, same argument as treeAtlas:
        // `far`/`fardown` build on this layout and both reach genCellIn's biome
        // sampler, which reads the map.
        entry(31, T::ReadOnlyStorage), // worldMap
        // Same binding number as in simBGL_: the particle and MPM wind sites
        // reach windAtQ, which reads the draft volume (common.wgsl WIND DRAFTS).
        entry(46, T::Storage),         // draft
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
        // Chunk tickets P2: a particle OUTSIDE residency flies on and blocks
        // against the far cascade (sim_particle.wgsl FAR FLIGHT) — the same
        // two buffers the gas group binds for gasFarBlocked.
        entry(8, T::ReadOnlyStorage),  // farVox
        entry(9, T::Uniform),          // FarParams (the cascade origins)
    };
    particleBGL_ = device.CreateBindGroupLayout(pentries, std::size(pentries));

    // group 1: gas particles (sim_gas.wgsl, docs/PLAN_gas_particles.md).
    //
    // Its OWN group rather than an extension of particleBGL_, and the reason is
    // farVox: gas needs the far cascade to know what it is drifting into
    // outside the window, and that lives in farBGL_. A layout carrying both
    // would have to be bound by every ballistic row for no hazard, and the
    // fluid pair already established that a system with its own buffers gets
    // its own group-1.
    rhi::BindGroupLayoutEntry gentries[] = {
        entry(0, T::Storage),          // gasRead  (gasParticles[page])
        entry(1, T::Storage),          // gasWrite (gasParticles[1-page])
        entry(2, T::Storage),          // gasCounts (atomic)
        entry(3, T::Storage),          // gasSpawn: the CA's outbox + counters
        entry(4, T::Storage),          // gasClaim (re-entry claim hash)
        entry(5, T::Storage),          // gasArgs staging
        entry(6, T::Storage),          // gasOuter (render-only density box)
        entry(7, T::ReadOnlyStorage),  // farVox: blocking outside the window
        entry(8, T::Uniform),          // FarParams (the cascade origins)
        entry(9, T::ReadOnlyStorage),  // reactions: the RK_DECAY bucket
        entry(10, T::ReadOnlyStorage), // gasSpawnOps (CPU-authored spawns)
        // The far fire-plume emitter list (world.h kGasFarEmitMax). In THIS
        // group rather than a group of its own because the kernel that reads
        // it writes gasOuter, which is binding 6 here -- one more read-only
        // entry against a whole extra layout to bind.
        entry(11, T::ReadOnlyStorage), // gasFarEmit
        entry(12, T::Storage),         // gasFarOuter (the long-range box)
        entry(13, T::Storage),         // gasPlumeTrack (carried plume offsets)
    };
    gasBGL_ = device.CreateBindGroupLayout(gentries, std::size(gentries));

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
        // The solute layer (world.h kSol*): exciteDetect REFUSES a cell that
        // carries dissolved mass (a particle has no solute payload yet, so
        // exciting it would delete the mass) and counts the refusal.
        entry(14, T::Storage),         // solTable
        entry(15, T::Storage),         // solPool
        entry(16, T::Storage),         // solMeta
        entry(17, T::ReadOnlyStorage), // solSpec
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
        // occupancy is VERTEX-visible too since P0's debris wiring: debris.wgsl
        // lights per vertex and opennessScaleAtBody walks the blockers mask
        // down to the ground under the cube (docs/PLAN_gi.md §2 verdict).
        entry(1, T::ReadOnlyStorage, S::Fragment | S::Vertex),   // occupancy
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
        // THE ONE WRITABLE ENTRY IN THE RENDER BIND GROUP, and the invariant
        // stated above still holds: "the renderer must never write THE WORLD".
        // shadowCache is render-PRIVATE derived data (world.h) — not sim state,
        // not hashed, not saved, and the sim has no binding for it, so the
        // sim -> render arrow is unchanged. What flows the other way is a
        // render-only value the renderer itself produced last frame.
        //
        // Needs Caps::fragmentStoresAndAtomics, which is requested at device
        // creation; the SHADOW_CACHE prelude const is false without it, and
        // then this entry is bound but never written.
        entry(14, T::Storage, S::Fragment),  // shadowCache
        entry(15, T::Storage, S::Fragment),  // shadowReq (the append list)
        // RENDER_STATS step counters (world.h kRenderStat*). Bound always so
        // the layout is one layout; written only when the prelude const is
        // true, and never by the shipping shader.
        entry(16, T::Storage, S::Fragment),  // renderStats
        // The openness grid (world.h kOpenFaces, docs/PLAN_gi.md §2).
        // READ-ONLY here and read_write only in the sim group: the arrow is
        // sim-side-pass -> render, exactly like occupancy at binding 1.
        //
        // In THIS layout rather than in raymarch's alone because microBodyBGL_
        // and the debris/sprite pipelines share renderBGL_ as group 0, and
        // ambientAtP has to reach the same bytes or a mob glows in a cave
        // (PLAN_gi.md §1, last bullet).
        entry(17, T::ReadOnlyStorage, S::Fragment | S::Vertex),   // openness
        entry(18, T::ReadOnlyStorage, S::Fragment | S::Vertex),   // opennessGen
        // The irradiance grid (docs/PLAN_gi.md §3). Storage, not ReadOnly, so
        // P2's write-back (the receiver's gathered term feeds its own face)
        // needs no layout change; P1's raymarch declares it `read`.
        entry(19, T::Storage, S::Fragment),           // irradiance
        // The glow field (src/sim/world.h kGlowBytes). READ-ONLY here and
        // read_write only in the sim group, the same arrow openness has at 17.
        //
        // Fragment | Vertex, and the VERTEX half is the entire point of the
        // feature: debris.wgsl shades a rigid-body cube in the vertex stage,
        // where `voxels` (0) and `pageTable` (9) are Fragment-only, so it can
        // neither march a ray nor read a voxel. One position-keyed buffer load
        // is a shape it CAN consume, which is what lets the crown that falls
        // off a burning tree be lit by the fire it fell out of.
        entry(20, T::ReadOnlyStorage, S::Fragment | S::Vertex),   // glow
        // The outer gas density box (docs/PLAN_gas_particles.md 2.5). Declared
        // HERE, on the SIM side, so the far-march sampling that consumes it is
        // a WGSL-only change: a binding a shader names must exist in the layout
        // or the pipeline will not build, and that is the one thing a renderer
        // edit cannot add for itself.
        //
        // READ-ONLY and fragment-only. sim_gas's resolve is the sole writer,
        // on the TICK command buffer, and the raymarcher reads it in the
        // FRAGMENT stage of the same frame -- the compute->fragment hop that
        // has no other source of synchronisation in this engine, which is why
        // GasOuter is on the pass table at all.
        entry(21, T::ReadOnlyStorage, S::Fragment),               // gasOuter
        // W3: the surface-momentum store (PLAN_water_relevel.md §5, last
        // bullet; world.h kWaterFluxWords). The water surface reads its OWN
        // column's four pipes for the two things an eighth-quantised column
        // height cannot carry — which way the surface is moving and how hard.
        //
        // READ-ONLY, fragment-only, and the arrow is the one every render entry
        // here has: sim -> render. `wbFlux` is the sole writer and it runs on
        // the TICK command buffer; every command buffer opens with a global
        // memory barrier, which is what covers the compute -> fragment hop for
        // openness and gasOuter as well, so this needs no pass-table row of its
        // own (the writes already have theirs).
        //
        // Declared HERE, on the SIM side, for gasOuter's stated reason: a
        // binding a shader names must exist in the layout or the pipeline will
        // not build, and that is the one thing a renderer edit cannot add for
        // itself. At render.waveSimSlope/Foam 0 the shader never reads it and
        // the whole block const-folds away — the entry costs one descriptor.
        entry(22, T::ReadOnlyStorage, S::Fragment),               // waterFlux
        // The LONG-RANGE gas density box (world.h kGasFarOuterN). Same
        // standing and the same arrow as gasOuter at 21, at eight times
        // the cell: sim_gas's wide splat is the sole writer, on the TICK
        // command buffer, and the raymarcher reads it in the FRAGMENT
        // stage of the same frame. Declared HERE for gasOuter's stated
        // reason -- a binding a shader names must exist in the layout or
        // the pipeline will not build, and that is the one thing a
        // renderer edit cannot add for itself.
        entry(23, T::ReadOnlyStorage, S::Fragment),               // gasFarOuter
        // THE WATER VEIL (common.wgsl): one VEIL_WORDS record per pixel, how
        // the liquid a pixel's primary ray crossed transforms the light of a
        // raster body under it. Written by raymarch.wgsl's fragment stage,
        // read by debris.wgsl / microbody.wgsl after DrawWorld's
        // SplitAfterFragmentWrite. Render-private like shadowCache (14):
        // never hashed, never saved, no sim binding. Storage (not ReadOnly)
        // because the raymarch writes it; the body shaders declare it `read`.
        entry(24, T::Storage, S::Fragment),                       // waterVeil
        // THE CLOUDS (cloud.wgsl, common.wgsl's CloudParams block). The
        // accumulated low-res cloud image the sky composite upsamples (25);
        // the shadow + env maps (26), which the BODY paths read too — debris
        // shades per vertex, hence Vertex; and the cloud uniform (27). All
        // render-private derived data written by the ShadowCache table's
        // cloud rows earlier in the same command buffer.
        entry(25, T::ReadOnlyStorage, S::Fragment),               // cloudHist
        entry(26, T::ReadOnlyStorage, S::Fragment | S::Vertex),   // cloudMaps
        entry(27, T::Uniform, S::Fragment | S::Vertex),           // CloudParams
        // THE SOLUTE LAYER, read by the water surface shade (DESIGN.md §4
        // "Solutes", raymarch.wgsl `solLookAt`): brine, fairy water, ink and
        // vitriol tint the liquid by concentration, lumen glows. The table
        // (28), the pool (29) and the species table (30) -- the same buffers
        // the CA binds at 40/41/43. Same standing and arrow as waterFlux (22):
        // written on the TICK command buffer only, read in the FRAGMENT stage,
        // covered by the global barrier every command buffer opens with.
        // Render-only: nothing here is hashed, and the read is ONE table load
        // per liquid-surface pixel (EMPTY -> no pool fetch at all).
        entry(28, T::ReadOnlyStorage, S::Fragment),               // solTable
        entry(29, T::ReadOnlyStorage, S::Fragment),               // solPool
        entry(30, T::ReadOnlyStorage, S::Fragment),               // solSpec
        // THE FAR SURFACE MAP (world.h kFarMap*, LOD-seam package A): the
        // per-level sub-column heights + skin traceFar refines a surface cell
        // against. Same standing and arrow as farVox at 4: written by the
        // PT_FARFILL / PT_TICK far rows, read in the FRAGMENT stage, covered by
        // the global barrier every command buffer opens with.
        entry(31, T::ReadOnlyStorage, S::Fragment),               // farMap
        // THE RAY-START MAP (ray_start.wgsl): where each 2x2 pixel block's
        // primary ray may begin marching. Written by the ShadowCache
        // table's ray_start rows in the same command buffer, read here;
        // BeginRendering's flush is the compute->fragment barrier.
        entry(32, T::ReadOnlyStorage, S::Fragment),               // rayStart
        // THE RAIN SHADOW MAP (rain_map.wgsl): where precipitation lands.
        // Written by the ShadowCache table's rain_map rows in the same command
        // buffer, read by the rain overlay and the wet shading here.
        entry(34, T::ReadOnlyStorage, S::Fragment),               // rainMap
        // THE GUST STREAKS (wind_streak.wgsl): the pool the per-frame
        // `wind_streak` row just advected, read by the ribbon draw's VERTEX
        // stage. BeginRendering's flush is the compute->vertex barrier.
        entry(33, T::ReadOnlyStorage, S::Vertex),                 // windStreaks
        // THE WIND-DRAFT VOLUME (sim_draft.wgsl): the F4 arrows' vertex stage
        // calls windAt, which shelters the ambient field by it. Written on the
        // TICK command buffer, covered by the global barrier every command
        // buffer opens with.
        entry(35, T::ReadOnlyStorage, S::Vertex),                 // draft
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
        entry(6, T::Storage),          // farSig (fardown's unchanged-chunk skip)
        entry(7, T::Storage),          // farMap (the surface map: farmap / farpatch / fardown)
    };
    farBGL_ = device.CreateBindGroupLayout(entries, std::size(entries));

    rhi::BindGroupLayout farGroups[] = {simSlimBGL_, farBGL_};
    farPL_ = device.CreatePipelineLayout(farGroups, 2);

    rhi::BindGroupLayout fluidGroups[] = {simSlimBGL_, fluidBGL_};
    fluidPL_ = device.CreatePipelineLayout(fluidGroups, 2);

    rhi::BindGroupLayout fluidSeamGroups[] = {simSlimBGL_, fluidSeamBGL_};
    fluidSeamPL_ = device.CreatePipelineLayout(fluidSeamGroups, 2);

    rhi::BindGroupLayout gasGroups[] = {simSlimBGL_, gasBGL_};
    gasPL_ = device.CreatePipelineLayout(gasGroups, 2);
  }

  // ---- bind groups ----
  auto b = [](uint32_t binding, const rhi::Buffer& buf, uint64_t size = 0) {
    rhi::BindGroupEntry e{};
    e.binding = binding;
    e.buffer = buf;
    e.size = size;  // 0 = whole buffer, per rhi::BindGroupEntry
    return e;
  };
  // The rain exposure map (sim_rain_expo.wgsl): sized for the steepest slope
  // the weather can hand the sim (rainlat::kExpoMaxAxis per axis), so no
  // slope ever reallocates it. Its contents need no initial value: every
  // tick that reads it rebuilt it first (C_RAINEXPO).
  rainExpoBuf_ = CreateBuffer(
      device, (uint64_t)rainlat::kExpoMaxAxis * rainlat::kExpoMaxAxis * 4,
      rhi::BufferUsage::Storage | rhi::BufferUsage::CopySrc, "rainExpo");
  // The CA's air mask (pass_table.def caMask): written before each substep
  // for every dirty chunk and read only for those, so it needs no clear.
  caMaskBuf_ = CreateBuffer(device, (uint64_t)kNumSlots * 3u * (kChunkVol / 32u) * 4u,
                            rhi::BufferUsage::Storage, "caMask");
  // ...and its ambient wind cache (sim_step.wgsl caWind): 64 blocks x 3 words.
  caWindBuf_ = CreateBuffer(device, (uint64_t)kNumSlots * 64u * 3u * 4u,
                            rhi::BufferUsage::Storage, "caWind");
  // The wind-draft volume (world.h kDraft*). ZEROED: the masks a solve
  // compares against start as an all-air box, and the field as "no wind"
  // until the first (forced) rebuild tick solves it -- the renderer does not
  // read it before then (DraftValid).
  {
    using U = rhi::BufferUsage;
    draftBuf_ = CreateBuffer(device, (uint64_t)kDraftWords * 4,
                             U::Storage | U::CopySrc | U::CopyDst, "draft");
    draftMetaBuf_ = CreateBuffer(device, (uint64_t)kDraftMetaWords * 4,
                                 U::Storage | U::CopySrc | U::CopyDst, "draftMeta");
    draftArgsBuf_ = CreateBuffer(device, 16 * kDraftStages, U::Indirect | U::CopyDst, "draftArgs");
    const std::vector<uint32_t> zero((size_t)kDraftWords, 0u);
    device.GetQueue().WriteBuffer(draftBuf_, 0, zero.data(), zero.size() * 4);
    device.GetQueue().WriteBuffer(draftMetaBuf_, 0, zero.data(), kDraftMetaWords * 4);
    uint32_t args0[4 * kDraftStages] = {};
    for (uint32_t s = 0; s < kDraftStages; s++) args0[4 * s + 1] = args0[4 * s + 2] = 1u;
    device.GetQueue().WriteBuffer(draftArgsBuf_, 0, args0, sizeof(args0));
    draftForce_ = true;
    draftValid_ = false;
  }
  BuildSimBindGroups(device);
  for (int page = 0; page < 2; page++) {
    rhi::BindGroupEntry pentries[] = {
        b(0, world_->particles[page]),
        b(1, world_->particles[1 - page]),
        b(2, world_->particleCounts),
        b(3, world_->claim),
        b(4, world_->pArgsStage),
        b(5, world_->expOps),
        b(6, world_->expMask),
        b(7, world_->spawnOps),
        b(8, world_->farVox),
        b(9, world_->farUBO),
    };
    particleBG_[page] =
        device.CreateBindGroup(particleBGL_, pentries, std::size(pentries), "particleBG");

    // Gas: same parity convention as particleBG_ — gasParticles[page] is what
    // this tick READS and [1-page] is what it writes, and FlipPage swaps them.
    rhi::BindGroupEntry gentries[] = {
        b(0, world_->gasParticles[page]),
        b(1, world_->gasParticles[1 - page]),
        b(2, world_->gasCounts),
        b(3, world_->gasSpawn),
        b(4, world_->gasClaim),
        b(5, world_->gasArgs),
        b(6, world_->gasOuter),
        b(7, world_->farVox),
        b(8, world_->farUBO),
        b(9, reactionBuf_),
        b(10, world_->gasSpawnOps),
        b(11, world_->gasFarEmit),
        b(12, world_->gasFarOuter),
        b(13, world_->gasPlumeTrack),
    };
    gasBG_[page] =
        device.CreateBindGroup(gasBGL_, gentries, std::size(gentries), "gasBG");

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
    // THE CLOUDS' fixed-size buffers (world.h kCloud*). The screen-sized pair
    // starts at one pixel and is grown by EnsureClouds; every one of them is
    // initialised to "clear sky" so a read before the first cloud pass sees
    // transmittance 1, not the zeros that would black the sky out.
    using U = rhi::BufferUsage;
    cloudNoiseBuf_ = CreateBuffer(device, kCloudNoiseWords * 4, U::Storage, "cloudNoise");
    cloudWeatherBuf_ = CreateBuffer(device, (uint64_t)kCloudWeatherN * kCloudWeatherN * 4,
                                    U::Storage | U::CopyDst, "cloudWeather");
    cloudMapsBuf_ = CreateBuffer(device, kCloudMapsWords * 4, U::Storage | U::CopyDst,
                                 "cloudMaps");
    cloudPixels_ = 0;
    cloudBaked_ = false;
    {
      std::vector<uint32_t> init((size_t)kCloudMapsWords, 0u);
      const float one = 1.0f;
      uint32_t oneBits;
      std::memcpy(&oneBits, &one, 4);
      const size_t shadowWords = (size_t)kCloudShadowN * kCloudShadowN;
      for (size_t i = 0; i < shadowWords; i++) init[i] = oneBits;
      // pack2x16float(vec2(b = 0, T = 1)): half 1.0 is 0x3C00 in the high lane.
      const size_t envEnd = shadowWords + (size_t)kCloudEnvN * kCloudEnvN * 2;
      for (size_t i = shadowWords + 1; i < envEnd; i += 2) init[i] = 0x3C000000u;
      device.GetQueue().WriteBuffer(cloudMapsBuf_, 0, init.data(), init.size() * 4);
    }
    EnsureClouds(1, 1);
  }
  {
    // One record's worth is below VEIL_WORDS, so the shaders' bounds test
    // reads NO veil from either buffer until EnsureVeil sizes the live one.
    using U = rhi::BufferUsage;
    veilBuf_ = CreateBuffer(device, 4, U::Storage, "waterVeil");
    veilNone_ = CreateBuffer(device, 4, U::Storage, "waterVeilNone");
    // A placeholder until EnsureRayStart sizes it: its word 0 is no frame's
    // key, and the fragment shader's length check refuses it anyway.
    rayStartBuf_ = CreateBuffer(device, 16, U::Storage, "rayStart");
    // The rain shadow map (rain_map.wgsl): fixed size, world-anchored rather
    // than target-sized, so it is made once here. Zeroed: a zero header is
    // "not live" and a zero tag matches no generation (they run 1..255).
    {
      const uint64_t words = kRainMapHeaderWords + 2ull * kRainMapN * kRainMapN;
      rainMapBuf_ = CreateBuffer(device, words * 4, U::Storage, "rainMap");
      const std::vector<uint32_t> zero((size_t)words, 0u);
      device.GetQueue().WriteBuffer(rainMapBuf_, 0, zero.data(), words * 4);
    }
    // The gust streak pool: fixed, zeroed (lifetime 0 = never spawned).
    {
      const uint64_t words = (1ull + (uint64_t)kWindStreakCap * kWindStreakStride) * 4ull;
      windStreakBuf_ = CreateBuffer(device, words * 4ull, U::Storage | U::CopyDst, "windStreaks");
      std::vector<uint32_t> zero((size_t)words, 0u);
      device.GetQueue().WriteBuffer(windStreakBuf_, 0, zero.data(), zero.size() * 4);
    }
    veilPixels_ = 0;
    BuildRenderBindGroup(renderBG_, veilBuf_);
    BuildRenderBindGroup(renderBGNoVeil_, veilNone_);
  }
  // ---- the shadow-cache resolve pass (shadow_resolve.wgsl) ----
  // Its OWN layout rather than a reuse of renderBGL_ with widened visibility:
  // this is a COMPUTE pass, and adding S::Compute to thirteen fragment-only
  // render entries to serve one kernel would relax the stage mask on every
  // buffer the renderer reads. The overlap (voxels, occupancy, materials,
  // renderUBO, pageTable, shadowCache) is six bindings, and a buffer being in
  // two bind groups is free.
  {
    auto entry = [](uint32_t binding, rhi::BufferBindingType type) {
      rhi::BindGroupLayoutEntry e{};
      e.binding = binding;
      e.visibility = rhi::ShaderStage::Compute;
      e.type = type;
      return e;
    };
    using T = rhi::BufferBindingType;
    rhi::BindGroupLayoutEntry entries[] = {
        entry(0, T::ReadOnlyStorage),  // voxels
        entry(1, T::ReadOnlyStorage),  // occupancy
        entry(2, T::ReadOnlyStorage),  // materials
        entry(3, T::Uniform),          // renderUBO
        entry(4, T::ReadOnlyStorage),  // pageTable
        entry(5, T::Storage),          // shadowCache
        entry(6, T::Storage),          // shadowReq
        entry(7, T::Storage),          // shadowArgsStage
        // P1 direct injection (docs/PLAN_gi.md §3): the resolve pass blends each
        // published patch's lit radiance into its block-face, and reads the
        // slot stamp so a reused slot starts its blend from zero.
        entry(8, T::Storage),          // irradiance
        entry(9, T::ReadOnlyStorage),  // opennessGen
        // The openness bytes, so the deposit can cap the shadow lift by sky
        // visibility (shadowLiftCap in common.wgsl; see shadow_resolve.wgsl).
        entry(10, T::ReadOnlyStorage), // openness
        // The penumbra window (world.h kShadowHistBytes): the 16-frame
        // sliding window of sun-visibility samples per patch. Read-modify-
        // written here and bound nowhere else — the fragment shader reads the
        // published 8-bit value out of shadowCache and never this.
        entry(11, T::Storage),         // shadowHist
        entry(12, T::Uniform),         // CloudParams
        entry(13, T::Storage),         // cloudNoise
        entry(14, T::Storage),         // cloudWeather
        entry(15, T::Storage),         // cloudMaps
        entry(16, T::Storage),         // cloudRaw
        entry(17, T::Storage),         // cloudHist
        // The far cascade, read by the resolve so a patch's shadow ray that
        // leaves the window unblocked continues into it (shadow_resolve.wgsl,
        // CASTERS OUTSIDE THE WINDOW). The clouds do not use them. farOcc is
        // plain Storage: sky_top.wgsl's reduce writes the sky-bound tail.
        entry(18, T::ReadOnlyStorage), // farVox
        entry(19, T::Storage),         // farOcc (read_write in sky_top.wgsl)
        entry(20, T::Uniform),         // farUBO
        // The ray-start map (ray_start.wgsl): written by its two per-frame
        // rows, read by the raymarch at renderBGL_ 32.
        entry(21, T::Storage),         // rayStart
        // The rain shadow map (rain_map.wgsl): written by its two per-frame
        // rows, read by the raymarch at renderBGL_ 34.
        entry(23, T::Storage),         // rainMap
        // The gust streaks' pool (wind_streak.wgsl `update`), read-modify-
        // written once a frame; the draw reads it at renderBGL_ 33.
        entry(22, T::Storage),         // windStreaks
        // The wind-draft volume: the streak update advects by windAt, which
        // reads it (written on the tick command buffer).
        entry(24, T::ReadOnlyStorage), // draft
    };
    shadowBGL_ = device.CreateBindGroupLayout(entries, std::size(entries));

    // THE CLOUDS share this layout (cloud.wgsl): same per-frame table, same
    // compute stage, and binding 3 (RenderParams) is the one input they have
    // in common with the resolve. 12 is their uniform, 13..17 their buffers.
    // The bind group itself is built by BuildShadowBindGroup, because
    // EnsureClouds replaces two of those buffers whenever the target grows.
    rhi::BindGroupLayout shadowGroups[] = {shadowBGL_};
    shadowPL_ = device.CreatePipelineLayout(shadowGroups, 1);
    BuildShadowBindGroup();
  }
  // ---- the TAA resolve's layout (taa.wgsl) --------------------------------
  // Its OWN layout, sharing nothing with renderBGL_: this pass reads none of
  // the world (no voxels, no materials, no RenderParams) and the two buffers it
  // does read are sized on the WINDOW, not the world, so they are recreated
  // whenever the window or render.renderScale moves. Putting them in the
  // renderer's group would mean rebuilding the renderer's bind group on a
  // window resize, for a pass the renderer does not use.
  //
  // The LAYOUT is created here, in Init, because the pipeline is built from it
  // in EnsureRenderPipelines; the BIND GROUPS are created in EnsureTaa, which
  // is where the buffers they point at come into existence.
  {
    auto e = [](uint32_t binding, rhi::BufferBindingType type) {
      rhi::BindGroupLayoutEntry x{};
      x.binding = binding;
      x.visibility = rhi::ShaderStage::Fragment;
      x.type = type;
      return x;
    };
    using T = rhi::BufferBindingType;
    rhi::BindGroupLayoutEntry entries[] = {
        e(0, T::Uniform),            // TaaParams
        e(1, T::ReadOnlyStorage),    // srcColor  (this frame, render res)
        e(2, T::ReadOnlyStorage),    // srcDepth  (this frame, render res)
        e(3, T::ReadOnlyStorage),    // histIn    (native res, page ^ 1)
        e(4, T::Storage),            // histOut   (native res, page)
    };
    taaBGL_ = device.CreateBindGroupLayout(entries, std::size(entries));
    rhi::BindGroupLayout taaGroups[] = {taaBGL_};
    taaPL_ = device.CreatePipelineLayout(taaGroups, 1);
  }
  // The shading-LOD filter (denoise.wgsl): same arrangement as TAA — the
  // LAYOUT here, the bind groups in EnsureDenoise where the buffers exist.
  {
    auto e = [](uint32_t binding, rhi::BufferBindingType type) {
      rhi::BindGroupLayoutEntry x{};
      x.binding = binding;
      x.visibility = rhi::ShaderStage::Fragment;
      x.type = type;
      return x;
    };
    using T = rhi::BufferBindingType;
    rhi::BindGroupLayoutEntry entries[] = {
        e(0, T::Uniform),            // DenoiseParams (one per iteration)
        e(1, T::ReadOnlyStorage),    // srcColor  (render res)
        e(2, T::ReadOnlyStorage),    // srcDepth  (render res)
    };
    dnBGL_ = device.CreateBindGroupLayout(entries, std::size(entries));
    rhi::BindGroupLayout dnGroups[] = {dnBGL_};
    dnPL_ = device.CreatePipelineLayout(dnGroups, 1);
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
        b(6, world_->farSig),
        b(7, world_->farMap),
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
        b(14, world_->solTable),
        b(15, world_->solPool),
        b(16, world_->solMeta),
        b(17, solSpecBuf_),
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
//
// The top byte is TRANSPARENCY on the CPU side (voxload.cpp: 0 = opaque, so
// every colour that predates it is unchanged) and ALPHA on the GPU side, where
// microbody.wgsl reads color0 >> 24 as a voxel's screen-door coverage.
inline uint32_t ArtRgbToGpu(uint32_t rgb) {
  return ((rgb & 0xFFu) << 16) | (rgb & 0xFF00u) | ((rgb >> 16) & 0xFFu) |
         ((255u - (rgb >> 24)) << 24);
}
}  // namespace

// Write the cached art palette into the reserved run of a material table.
void Simulation::ApplyArtPalette(std::vector<MaterialGpu>& table) const {
  for (size_t i = 0; i < artPalette_.size() && i < kArtPaletteSlotsGpu; i++)
    table[kArtPaletteBaseGpu + i].color0 = ArtRgbToGpu(artPalette_[i]);
}

void Simulation::SetArtPalette(const rhi::Queue& queue,
                               const std::vector<uint32_t>& rgb) {
  // UNCHANGED AND ALREADY ON THE GPU: nothing to send. This is called on every
  // dirty micro-body frame, and the palette changes only when prefabs load.
  const size_t n = std::min<size_t>(rgb.size(), kArtPaletteSlotsGpu);
  if (artPaletteLive_ && n == artPalette_.size() &&
      std::equal(artPalette_.begin(), artPalette_.end(), rgb.begin()))
    return;
  artPalette_.assign(rgb.begin(), rgb.begin() + n);
  if (artPalette_.empty()) return;
  // Patch just the reserved run rather than re-uploading all 4096 entries: the
  // rest of the table is unchanged and may be mid-frame on the GPU.
  artRunScratch_.assign(kArtPaletteSlotsGpu, MaterialGpu{});
  for (size_t i = 0; i < artPalette_.size(); i++)
    artRunScratch_[i].color0 = ArtRgbToGpu(artPalette_[i]);
  queue.WriteBuffer(materialBuf_, (uint64_t)kArtPaletteBaseGpu * sizeof(MaterialGpu),
                    artRunScratch_.data(), artRunScratch_.size() * sizeof(MaterialGpu));
  artPaletteLive_ = true;
}

void Simulation::BuildSimBindGroups(const rhi::Device& device) {
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
        b(27, world_->openness),
        b(28, world_->opennessGen),
        b(29, world_->irradiance),
        b(30, world_->genAct),
        b(31, worldMapBuf_),
        b(32, world_->glow),
        b(33, world_->gasSpawn),
        b(34, world_->gasOuter),
        b(35, world_->reposeSnap),
        b(36, world_->waterFlux),
        b(37, world_->chunkHash),
        b(38, genColsBuf_),
        b(39, colCacheBuf_),
        b(40, world_->solTable),
        b(41, world_->solPool),
        b(42, world_->solMeta),
        b(43, solSpecBuf_),
        b(44, world_->solStage),
        b(45, rainExpoBuf_),
        b(46, draftBuf_),
        b(47, draftMetaBuf_),
        b(48, caMaskBuf_),
        b(49, caWindBuf_),
        b(50, world_->heatPool),
        b(51, world_->heatMeta),
        b(52, heatParamsBuf_),
    };
    simBG_[page] = device.CreateBindGroup(simBGL_, entries, std::size(entries), "simBG");

    rhi::BindGroupEntry sentries[] = {
        b(0, world_->voxels),
        b(1, world_->dirty[page]),
        b(2, world_->dirty[1 - page]),
        b(3, materialBuf_),
        b(4, world_->tickUBO),
        b(11, reactionBuf_),
        b(15, world_->support),
        b(17, world_->pageTable),
        b(18, world_->pageFaults),
        b(24, world_->waterBodyState),
        b(26, treeAtlasBuf_),
        b(31, worldMapBuf_),
        b(46, draftBuf_),
    };
    simSlimBG_[page] =
        device.CreateBindGroup(simSlimBGL_, sentries, std::size(sentries), "simSlimBG");
  }
}

void Simulation::UploadEnvironment(const rhi::Device& device, const rhi::Queue& queue,
                                   const TreeAtlas& trees,
                                   const std::vector<uint32_t>& worldMapWords) {
  // Grow-only: a table that shrank keeps its buffer and is zero-padded, the
  // same way Init pads to the header floor. Only a table that no longer fits
  // costs a new buffer -- and with it the two bind groups, because a bind
  // group names a buffer, not a slot.
  bool rebind = false;
  if (trees.words.size() > treeAtlasWords_) {
    treeAtlasWords_ = trees.words.size();
    treeAtlasBuf_ = CreateBuffer(device, (uint64_t)treeAtlasWords_ * 4,
                                 rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                                 "treeAtlas");
    rebind = true;
  }
  if (worldMapWords.size() > worldMapWords_) {
    worldMapWords_ = worldMapWords.size();
    worldMapBuf_ = CreateBuffer(device, (uint64_t)worldMapWords_ * 4,
                                rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                                "worldMap");
    rebind = true;
  }
  {
    std::vector<uint32_t> pad = trees.words;
    pad.resize(treeAtlasWords_, 0u);
    queue.WriteBuffer(treeAtlasBuf_, 0, pad.data(), pad.size() * 4);
  }
  {
    std::vector<uint32_t> pad = worldMapWords;
    pad.resize(worldMapWords_, 0u);
    queue.WriteBuffer(worldMapBuf_, 0, pad.data(), pad.size() * 4);
  }
  if (rebind) BuildSimBindGroups(device);
  // The tree lattice is a PRELUDE CONSTANT (TREE_TILE / TREE_SCAN /
  // TREE_CAND_MAX size worldgen's candidate array), so a reload that changed
  // the finest biome tile or the widest species has to recompile the shaders
  // or genChunk would scan the old lattice against the new table. Same
  // recompile F5 does; skipped when the lattice is unchanged, which is every
  // reload that only repaints a map or retunes a density.
  {
    const treeatlas::TreeLattice& live = treeatlas::CurrentTreeLattice();
    const treeatlas::TreeLattice& want = trees.lattice;
    if (live.tile != want.tile || live.scan != want.scan || live.candMax != want.candMax) {
      treeatlas::SetCurrentTreeLattice(want);
      std::printf("environment: tree lattice %d/%d/%d -> %d/%d/%d, recompiling shaders\n",
                  live.tile, live.scan, live.candMax, want.tile, want.scan, want.candMax);
      if (!ReloadShaders(device))
        std::fprintf(stderr, "environment: shader reload FAILED; the old lattice's pipelines stay live\n");
      sPondTileCompiled = worldmap::CurrentWorldMap().pondTile;
  sRefVpmCompiled = worldmap::CurrentTerrain().refVoxelsPerMetre;
    }
  }
  // The pond lattice (POND_TILE) is a prelude constant for the same reason
  // (P-F): a reload whose finest water tile moved recompiles, or worldgen
  // would roll the old lattice against the new rows.
  {
    const int want = worldmap::CurrentWorldMap().pondTile;
    const int wantRef = worldmap::CurrentTerrain().refVoxelsPerMetre;
    if (want != sPondTileCompiled || wantRef != sRefVpmCompiled) {
      std::printf("environment: pond lattice %d -> %d / reference scale %d -> %d, recompiling shaders\n",
                  sPondTileCompiled, want, sRefVpmCompiled, wantRef);
      if (!ReloadShaders(device))
        std::fprintf(stderr, "environment: shader reload FAILED; the old lattice's pipelines stay live\n");
      sPondTileCompiled = want;
      sRefVpmCompiled = wantRef;
    }
  }
}

void Simulation::UploadTables(const rhi::Queue& queue,
                              const std::vector<MaterialDef>& mats,
                              const std::vector<ReactionGpu>& reactions) {
  std::vector<MaterialGpu> table(4096, MaterialGpu{});
  anyRepose_ = false;
  // Which materials block the wind (sim_draft.wgsl draftBlocks) may have
  // changed: the next tick re-masks the whole draft box.
  draftForce_ = true;
  for (size_t i = 0; i < mats.size() && i < 4096; i++) {
    table[i] = mats[i].gpu;
    // Cond::ReposeActive. Latched HERE rather than tested per tick: it is a
    // property of the authored table, it changes only on a materials reload
    // (which comes back through this function), and a per-tick scan of 4096
    // entries to answer a constant would be the thing rule 2 is about.
    if (mats[i].gpu.repose != 0) anyRepose_ = true;
  }
  // WHAT A FROZEN FIRE IS MADE OF, latched here for anyRepose_'s reason and at
  // the same instant: it is a property of the authored table, it changes only
  // on a materials reload, and a reload comes back through this function. The
  // far fire-plume index (src/sim/farplumes.h) asks this per evicted voxel and
  // must not be scanning tag STRINGS to do it.
  FarPlumes::SetMaterials(mats, reactions);

  // WHAT EACH MATERIAL CATCHES AS, in its spare `_r2` (DESIGN.md §6 clause
  // 2c; sim/bodyreact.h CatchFormTable, the one derivation the body uses
  // too): sim_step.wgsl coatReact rewrites a cell to it when the cell's coat
  // makes a flame on it. Real material ids only -- the stain palette entries
  // above kStainPaletteBase own their `_r2` (coat glow, below).
  {
    const std::vector<uint32_t> catchForm = CatchFormTable(mats, reactions);
    for (size_t i = 0; i < catchForm.size() && i < kStainPaletteBase; i++)
      table[i]._r2 = catchForm[i] & 0xFFFu;
  }
  // THE TEMPERATURE LAYER (src/sim/heat.h): each material's emit, inertia and
  // transition count ride the rest of `_r2` (bits 12..24; what caMask, heatSrc
  // and the CA's heatReact read), and its transitions go to heatParams at
  // kHpMat + id * kHpMatStride. Inertia defaults by class: a gas answers in a
  // tick or two, water slowest.
  {
    std::vector<uint32_t> words((size_t)kHpMatStride * kHeatMatMax, 0u);
    for (size_t i = 0; i < mats.size() && i < kStainPaletteBase && i < kHeatMatMax; i++) {
      const ThermalDef& t = mats[i].thermal;
      int k = t.inertia;
      if (k < 0) {
        const uint32_t kl = mats[i].gpu.klass;
        k = kl == CLASS_GAS ? 1 : kl == CLASS_LIQUID ? 4 : 3;
      }
      const uint32_t n = (uint32_t)std::min<size_t>(t.transitions.size(), kHeatMaxTransitions);
      table[i]._r2 |= ((t.emit & 0xFFu) << kHeatR2EmitShift) |
                      (((uint32_t)k & 7u) << kHeatR2InertiaShift) | ((n & 3u) << kHeatR2TransShift);
      PackHeatMaterial(t, words.data() + i * kHpMatStride);
    }
    queue.WriteBuffer(heatParamsBuf_, (uint64_t)kHpMat * 4, words.data(), words.size() * 4);
  }

  // Mirror the stain palette into the reserved top entries (kStainPaletteBase,
  // materials.h): the renderer maps a voxel's 3-bit stain TYPE to a colour by
  // indexing there, which avoids a dedicated buffer + bind slot for what is at
  // most eight RGBA values. Every staining material writes its own slot; two
  // materials sharing a stain name share a slot and the FIRST one wins (see
  // below) -- they are by definition drawn as the same stain.
  bool claimed[8] = {};
  for (size_t mi = 0; mi < mats.size() && mi < kStainPaletteBase; mi++) {
    const MaterialDef& d = mats[mi];
    // EVERY staining material's own coat glow + pulse, in its own row's spare
    // `_r3` (materials.h kCoatGlow*). A BODY draws a coat by its material
    // (microbody.wgsl reads materials[mat].stainColor and this), so every
    // material that stains is drawn as itself on skin whatever slot it has.
    if (d.stainSlot != 0)
      table[mi]._r3 = (d.coatGlow & kCoatGlowMask) |
                      (((uint32_t)std::lround(d.coatPulseHz * 100.0f) & kCoatPulseMask)
                       << kCoatPulseShift);
    // The GROUND palette from here down: slots 1..7 only. A `bodyOnly` stain
    // has a slot of 8+ (materials.cpp StainRegistry) and no palette entry --
    // it never reaches a grid cell, and a body draws it by material above.
    uint32_t type = d.stainSlot;
    if (type == 0 || type >= 8u) continue;
    // WHAT THE STAIN IS MADE OF, in the entry's spare `_r3`: the material id
    // behind a GROUND stain type, which is how sim_step.wgsl's coat rules find
    // the coat's reaction bucket (DESIGN.md §6 "A coat is a co-located virtual
    // neighbour"). The FIRST material to claim the slot, the same answer
    // MobSystem's matOfStainType_ gives a body. Ground types only: a `bodyOnly`
    // slot (no type bits in stainPack) is never on a grid cell, so it keeps 0
    // ("no coat material") and could not be read as one if it were.
    // Bits 12..21 beside it: the stain's ground drying chance (kStainPalDry*,
    // sim_step.wgsl stainDry), from the same first claimant.
    const uint32_t groundType = d.gpu.stainPack & kStainPackTypeMask;
    if (groundType != 0 && (table[kStainPaletteBase + groundType]._r3 & kStainPalMatMask) == 0)
      table[kStainPaletteBase + groundType]._r3 =
          ((uint32_t)mi & kStainPalMatMask) |
          ((d.stainDries & kStainPalDryMask) << kStainPalDryShift) |
          ((d.stainGroundOpacity & kStainPalOpacityMask) << kStainPalOpacityShift);
    // Entry 0 (type 0 = clean, never drawn): the material that falls as rain
    // (sim_mutate.wgsl rainFall). A ground stainer only; the first one wins.
    if (groundType != 0 && d.stainIsRain && (table[kStainPaletteBase]._r3 & kStainPalMatMask) == 0)
      table[kStainPaletteBase]._r3 = (uint32_t)mi & kStainPalMatMask;
    // THE LOOK IS THE FIRST CLAIMANT'S TOO (2026-09-27, alchemy package D).
    // It was "last one wins", which was harmless while no two materials
    // shared a stain name. The enchanted liquids now ride blood's and wet's
    // slots (the palette is full; their coat word still names THEM, so every
    // gameplay reader tells them apart), and they are appended after the
    // originals -- last-wins would have repainted every bloodstain in the
    // world pink. First-wins is the rule `_r3` above and MobSystem's
    // matOfStainType_ already follow, so a slot now has ONE owner everywhere.
    if (claimed[type]) continue;
    claimed[type] = true;
    table[kStainPaletteBase + type].stainColor = d.gpu.stainColor;
    // ...and the body coat's glow + pulse in the palette entry's spare word
    // (materials.h kCoatGlow*). Only microbody.wgsl reads it.
    table[kStainPaletteBase + type]._r2 =
        (d.coatGlow & kCoatGlowMask) |
        (((uint32_t)std::lround(d.coatPulseHz * 100.0f) & kCoatPulseMask)
         << kCoatPulseShift);
  }

  // Tint palette, same trick one range lower again (world.h): a MATF_TINTED
  // material's state nibble indexes its own 16-entry run here, which is how a
  // GRID cell carries a colour at all. Unlike the art palette this needs no
  // re-apply hook — tints are authored in materials.json, so they arrive with
  // the very table being rebuilt and cannot go stale against it.
  //
  // `color0`, deliberately the same field the art palette uses: paletteColor()
  // reads one field for both runs, so a tint and an art colour with the same
  // RGB are literally the same bytes in the same place, and the "a mob's skin
  // and its rubble shade identically" claim is structural rather than a pair of
  // tables that happen to agree.
  for (const auto& d : mats) {
    if (d.tints.empty()) continue;
    const uint32_t base = (d.gpu.flags >> kMatTintBaseShift) & kMatTintBaseMask;
    for (size_t i = 0; i < d.tints.size() && i < kMatTintsMax; i++)
      table[kTintPaletteBaseGpu + base + i].color0 = ArtRgbToGpu(d.tints[i]);
  }

  // Far slot palette, a fourth reserved run (world.h kFarPaletteBaseGpu) and
  // the REVERSE of the slot every material carries in its own flags word. A
  // far cascade cell's byte is seven bits of palette slot; raymarch.wgsl's
  // farPalMat() reads the material id back out of `flags` here, and sim_gas
  // does the same to ask whether a plume is drifting into a solid.
  //
  // `flags` and not a colour field, because what a slot maps to is a MATERIAL
  // -- the far field wants its palette jitter, its opacity and its class, not
  // just an RGB. Only the owner writes: an alias shares the slot precisely so
  // that the byte resolves to the material it named.
  for (size_t i = 0; i < mats.size() && i < 4096; i++) {
    if (!mats[i].farPalOwner) continue;
    table[kFarPaletteBaseGpu + mats[i].farPalSlot].flags = (uint32_t)i;
  }

  // Art palette, same trick one range lower (world.h). Re-applied here because
  // this function rebuilds the WHOLE table: without it, hot-reloading
  // materials.json would silently repaint every mob in its raw material
  // colours until something reloaded the mob defs.
  ApplyArtPalette(table);

  queue.WriteBuffer(materialBuf_, 0, table.data(), table.size() * sizeof(MaterialGpu));
  // The run now holds THIS table's entries, not SetArtPalette's last write,
  // so the next SetArtPalette must send even an unchanged palette.
  artPaletteLive_ = false;

  std::vector<ReactionGpu> rtable(kMaxReactions, ReactionGpu{});
  for (size_t i = 0; i < reactions.size() && i < kMaxReactions; i++)
    rtable[i] = reactions[i];
  queue.WriteBuffer(reactionBuf_, 0, rtable.data(), rtable.size() * sizeof(ReactionGpu));

  // THE SOLUTE SPECIES (docs/PLAN_solutes.md §2.3). Re-read here, on every
  // table upload, because the species name their powders and solvents BY
  // MATERIAL NAME: a materials reload that moves an id must re-resolve them,
  // and this is the one function every reload path already comes through.
  // A table that fails to load is reported and uploaded EMPTY -- nothing
  // dissolves, which is a world rather than a crash.
  {
    std::vector<SoluteDef> defs;
    std::string err;
    if (!LoadSolutes(sandvox::AssetDir() + "/materials/solutes.json", mats, defs, err)) {
      std::fprintf(stderr, "solutes.json: %s", err.c_str());
      defs.clear();
    }
    UploadSolutes(queue, defs, mats);
  }
}

void Simulation::UploadSolutes(const rhi::Queue& queue, const std::vector<SoluteDef>& defs,
                               const std::vector<MaterialDef>& mats) {
  std::vector<uint32_t> w(kSolSpecWords, 0u);
  uint32_t count = 0;
  for (const SoluteDef& d : defs) {
    if (d.species == 0 || d.species > kSolSpeciesMax) continue;
    count = std::max<uint32_t>(count, d.species);
    uint32_t* r = &w[kSolSpecBase + d.species * kSolSpecStride];
    // Units per EIGHTH of a powder voxel -- the granularity mass enters and
    // leaves the world at (a powder cell holds eighths), and the contract's
    // unit bridge: one dissolved eighth is yieldPerVoxel / 8 units.
    r[0] = std::max<uint32_t>(1u, d.yieldPerVoxel / 8u);
    r[1] = std::min<uint32_t>(255u, d.saturation);
    // Per mille in the file, units of 1/kReactChanceDen on the GPU: the CA
    // rolls it against the same counter-hash the reaction rules do.
    r[2] = std::min<uint32_t>(1000u, d.dissolveChance) * kReactChanceScale;
    r[3] = std::min<uint32_t>(256u, d.diffusivity);
    r[4] = d.floor;
    r[5] = (uint32_t)d.densityPerUnit;
    r[6] = d.tint;
    r[7] = d.tintStrength;
    r[8] = d.glow;
    r[9] = d.precipitatesTo;
    const uint32_t nc = std::min<uint32_t>((uint32_t)d.converts.size(), kSolConvertsMax);
    r[10] = nc;
    for (uint32_t k = 0; k < nc; k++) {
      const SoluteConvert& c = d.converts[k];
      r[11 + k] = (c.solvent & 0xFFFu) | ((uint32_t)(c.into & 0xFFFu) << 12) |
                  (std::min<uint32_t>(c.cMin, 255u) << 24);
    }
    r[15] = d.from;
    // Per material: which species a powder dissolves AS, and which species a
    // liquid is a SOLVENT of. The mask is kSolSolventSpeciesMax wide; a
    // species past it simply has no solvent on the GPU (reported once).
    if (d.from < 4096) w[kSolSpecMatBase + d.from] |= d.species & 0xFFu;
    if (d.species <= kSolSolventSpeciesMax) {
      for (uint16_t sv : d.solvents)
        if (sv < 4096) w[kSolSpecMatBase + sv] |= 1u << (7 + d.species);
    } else {
      std::fprintf(stderr, "solutes.json: species \"%s\" (%u) is past the %u the GPU "
                   "solvent mask holds; it dissolves into nothing\n",
                   d.name.c_str(), d.species, kSolSolventSpeciesMax);
    }
  }
  w[0] = count;
  // ---- the concentration conditions (the rule side array) ----------------
  // MaterialDef::ruleFx[k].solute names the species rule reactOffset + k needs
  // its self cell to carry; resolved BY NAME here, where the species table
  // is. An unknown name compiles to species 255 -- a condition nothing can
  // meet -- and is reported, so a typo disables its rule loudly rather than
  // letting it fire unconditionally.
  static_assert(kSolSpecRuleCount >= kMaxReactions, "the rule side array must cover every rule");
  for (const MaterialDef& m : mats) {
    for (uint32_t k = 0; k < m.ruleFx.size() && k < m.gpu.reactCount; k++) {
      const RuleFx& f = m.ruleFx[k];
      if (f.solute.empty()) continue;
      const uint32_t idx = m.gpu.reactOffset + k;
      if (idx >= kSolSpecRuleCount) continue;
      uint32_t sp = 255u;
      for (const SoluteDef& d : defs)
        if (d.name == f.solute) sp = d.species & 0xFFu;
      if (sp == 255u)
        std::fprintf(stderr, "reactions.json: rule %u of \"%s\" names solute \"%s\", "
                     "which solutes.json does not have; the rule can never fire\n",
                     k, m.name.c_str(), f.solute.c_str());
      w[kSolSpecRuleBase + idx] = sp | ((f.soluteMin & 0xFFu) << 8) | ((f.soluteMax & 0xFFu) << 16);
    }
  }
  queue.WriteBuffer(solSpecBuf_, 0, w.data(), w.size() * 4);
  solutes_ = defs;
  SetCurrentSolutes(defs);
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
  // tracking which records moved would cost more than the write. But a dirty
  // frame is usually a POOL change (a burning limb's pokes), not a table one,
  // so the padded table is built in member scratch and compared with the last
  // one sent: an unchanged table is not written again.
  std::vector<MicroBodyModelGpu>& table = mbModelScratch_;
  table.assign(set.models.begin(),
               set.models.begin() + std::min<size_t>(set.models.size(),
                                                     kMaxMicroBodyModels));
  table.resize(kMaxMicroBodyModels, MicroBodyModelGpu{kMicroBodyNoModel, 0, 1, 0});
  if (!mbModelLive_ || mbModelLast_.size() != table.size() ||
      std::memcmp(mbModelLast_.data(), table.data(),
                  table.size() * sizeof(MicroBodyModelGpu)) != 0) {
    queue.WriteBuffer(mbModelBuf_, 0, table.data(),
                      table.size() * sizeof(MicroBodyModelGpu));
    mbModelLast_ = table;
    mbModelLive_ = true;
  }

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
      mbPoolBytesSent_ += poolWords * 4;
    } else {
      set.TakeDirtyRanges(mbRangeScratch_);
      for (const auto& r : mbRangeScratch_) {
        const size_t lo = std::min<size_t>(r.first, poolWords);
        const size_t hi = std::min<size_t>(r.second, poolWords);
        if (hi <= lo) continue;
        queue.WriteBuffer(mbPoolBuf_, lo * 4, set.pool.data() + lo,
                          (hi - lo) * 4);
        mbPoolBytesSent_ += (hi - lo) * 4;
      }
    }
  }
  set.ClearDirty();

  // The skin's art colours ride the same upload the bricks do: they are
  // published together or a painted brick indexes colours that are not there
  // yet. Free when the palette has not changed since it was last sent
  // (SetArtPalette compares before it writes).
  SetArtPalette(queue, set.artColors);
}

// ---------------------------------------------------------------------------
// THE PIPELINE BUILD POOL (docs/PLAN_shader_compile.md package A)
//
// A cold build was ~17 minutes, serial, on the boot thread: the cost is
// vkCreateComputePipelines in the NVIDIA driver, one entry point at a time,
// and worldgen.wgsl alone is five of them (main 80 s, list 75 s, far 746 s,
// fardown 98 s, pagefill).
//
// WHY PARALLEL IS LEGAL. vkCreateComputePipelines takes no externally-
// synchronized parameter, and the VkPipelineCache every call here shares is
// the one Vulkan object explicitly documented as internally synchronized. The
// unsafe part was OURS — vk::Backend's label table and Tint module cache —
// and both are now under their own mutex (rhi_vulkan.h). Nothing in a create
// touches Simulation state; each job writes exactly one member and the join
// is the happens-before edge for every reader after it.
//
// Deliberately not a general thread pool: the jobs are a fixed batch issued
// once at load (and once per F5), they never enqueue more work, and a
// pull-from-an-atomic-index worker needs no condition variable to prove.
namespace {
class PipelineBuildPool {
 public:
  void Add(std::function<void()> job) { jobs_.push_back(std::move(job)); }
  // Runs everything queued and returns when the last job has finished. One
  // thread means SERIAL and no thread is spawned at all — which is what
  // `--shader-stats` needs (concurrent pipeline-executable-properties queries
  // are exactly the case the extension attributes worst).
  void Run(unsigned threads) {
    if (jobs_.empty()) return;
    if (threads <= 1) {
      for (auto& j : jobs_) j();
      jobs_.clear();
      return;
    }
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    const unsigned n = std::min<unsigned>(threads, (unsigned)jobs_.size());
    for (unsigned i = 0; i < n; i++)
      pool.emplace_back([this, &next] {
        for (size_t k = next.fetch_add(1); k < jobs_.size(); k = next.fetch_add(1))
          jobs_[k]();
      });
    for (std::thread& t : pool) t.join();
    jobs_.clear();
  }

 private:
  std::vector<std::function<void()>> jobs_;
};

// Six, matching scripts/build.sh's core cap: the driver compile is one busy
// core per call and this machine has to stay usable while it runs.
constexpr unsigned kBuildThreads = 6;
}  // namespace

// ---- the specialized raymarch variant (W2-A) -------------------------------
//
// IMPLEMENTED, MEASURED, DEFAULT OFF. Kept in the shape this repo keeps its
// other refuted optimizations (SUBOCC_SKIP in raymarch.wgsl, A3's cascade
// shadow): flip the const below and the whole experiment re-runs, with the
// `nospec` --render-budget arm as its instrument. Everything from here down is
// live code; the only thing `false` costs the shipping build is that
// BuildRaymarchVariant returns immediately, so no second module is assembled,
// no second pipeline is compiled and no second SPIR-V cache entry exists.
//
// THE PREMISE. voxelbit.net compiles two variants of its trace pipeline and
// selects one per frame; the branch it specializes away — its see-through-
// foliage check — cost +0.169 ms even fully DISABLED, because a dynamic branch
// in a hot loop is paid in REGISTERS whether or not it is taken. This engine
// has the same shape and a measured cliff to go with it: 168 registers cost
// 3.5 ms against 128 in trace() (the long note there), and `shadow0` minus
// `noshadow` prices one call site's mere existence at 3.59 ms.
//
// WHAT IT ACTUALLY BOUGHT (RTX 3060 Ti, 1920x1080, exclusive lock,
// --budget-arms baseline,nospec, 2026-09-08):
//
//   cascade camera   6.83 ms specialized   6.86 universal   -0.03 ms (-0.4%)
//   meadow camera   20.94 ms specialized  20.98 universal   -0.04 ms (-0.2%)
//
// Nothing. Both deltas are inside the run-to-run spread of the harness.
//
// AND --shader-stats SAYS WHY, which is the part worth inheriting:
//
//   pipeline        Register Count   Binary Size   spill
//   raymarch                   168     1,605,888   +192 B/thread
//   raymarchLean               168     1,223,424   +192 B/thread
//
// The BINARY shrank 23.8% (matching the 24.2% the optimized SPIR-V shrank) and
// the REGISTER COUNT DID NOT MOVE AT ALL. Occupancy is what buys frames on this
// shader, and the specialization did not touch it.
//
// THE LESSON, because it is the opposite of what `shadow0` taught and the two
// look identical from a distance: a call site costs registers when its live
// ranges INTERLEAVE with the code around it. `sunShadowAt` was an inlined
// trace() in the middle of the primary shading path, so its values and the
// shading's values were alive at the same program points and the allocator had
// to hold both. `fluidMarch` is called ONCE, into a 4-field struct, under a
// uniform branch; its internal values die inside its own region and never
// coexist with trace()'s DDA, which is what actually sets this shader's
// 168-register ceiling. Deleting it removes a quarter of the BINARY and none of
// the PEAK. Instruction footprint is not occupancy, and this shader is not
// instruction-fetch bound.
//
// So the general form of "specialize a frame-constant branch out of the hot
// shader" is NOT free money here. The test to apply before trying it again is
// not "how much code does the branch guard" — it is "does that code's register
// pressure coexist with the hot loop's". --shader-stats answers it for a few
// minutes and no code at all.
//
// WHAT IT COSTS WHEN ON, since that is the other half of the verdict: a second
// full compile of the biggest fragment shader in the engine. Measured on this
// tree, cold: spirv-opt 7.6-48.8 s for `raymarch.lean:fs` on top of the
// universal's 11.7-17.8 s (the wide range is contention with the far cascades'
// own optimizer run on a neighbouring thread), plus its driver compile, plus a
// second SPIR-V disk-cache entry. Deferred off the first-frame path, so it is
// not latency the player sees — but it is real machine time for a measured
// zero.
//
constexpr bool kRaymarchVariantOn = false;
//
// WHY THE SUBSTITUTION IS HERE AND NOT IN THE PRELUDE. The natural home for a
// per-variant `const` is ShaderConstantPrelude() in gpu/resources.cpp, next to
// RENDER_STATS which is exactly this pattern. Two reasons it is not there:
//
//   1. That file was held by a concurrent session when this landed.
//   2. THE ONE THAT WOULD HAVE DECIDED IT ANYWAY: the prelude goes into EVERY
//      shader, and the SPIR-V disk cache keys on assembled source. One new
//      prelude line is a cache miss for all 21 shaders — including worldgen's
//      `far`, which is 170 s of driver compile with the optimizer on and 746 s
//      without it. A per-variant constant must touch ONLY the shader it
//      specializes, and taking the assembled source back out of the module is
//      how that is achieved without a second copy of LoadShader's
//      concatenation (rhi::vkr::ModuleSource says the same from its side).
//
// The cost of the mechanism, stated plainly: the substitution targets are three
// literal lines of raymarch.wgsl, and nothing but scripts/check_shaders.sh
// (which performs the same substitution and fails if a target is missing)
// stops a reformat from silently disabling the feature.
void Simulation::BuildRaymarchVariant(const rhi::Device& device,
                                      const rhi::ShaderModule& base) {
  raymarchLeanModule_ = {};
  // The one line the refutation above costs the shipping build. Everything
  // downstream keys on `raymarchLeanModule_` being invalid, so an off build
  // assembles no second source, compiles no second pipeline, and adds no
  // second SPIR-V cache entry.
  if (!kRaymarchVariantOn) return;
  if (!base) return;
  std::string src = rhi::vkr::ModuleSource(base);
  if (src.empty()) return;
  static const char* const kSpecConsts[] = {"SPEC_FLUID", "SPEC_DEBUG_VIZ",
                                            "SPEC_SHORT_RANGE"};
  for (const char* name : kSpecConsts) {
    const std::string from = std::string("const ") + name + " : bool = true;";
    const std::string to = std::string("const ") + name + " : bool = false;";
    const size_t at = src.find(from);
    if (at == std::string::npos) {
      std::fprintf(stderr,
                   "raymarch specialization: \"%s\" not found in the assembled "
                   "source — the variant is disabled and every frame will draw "
                   "the universal pipeline. Fix the spelling in "
                   "assets/shaders/raymarch.wgsl (and note that "
                   "scripts/check_shaders.sh checks the same three lines).\n",
                   from.c_str());
      return;
    }
    // find() once, not replace-all: each const is declared exactly once, and a
    // second occurrence would mean the marker text had leaked into a comment,
    // which is worth failing on rather than silently patching.
    src.replace(at, from.size(), to);
  }
  raymarchLeanModule_ = device.CreateShaderModule(src, "raymarch.lean");
}

// Main thread only, and every field it touches is main-thread-only, so there is
// no atomic here and none is needed: the future IS the happens-before edge, and
// `wait_for(0)` is the non-blocking half of the same question `get()` answers.
void Simulation::PollRaymarchVariant(bool block) {
  if (rayLeanPublished_) return;
  if (!rayLeanFuture_.valid()) { rayLeanPublished_ = true; return; }
  if (!block && rayLeanFuture_.wait_for(std::chrono::seconds(0)) !=
                    std::future_status::ready) {
    return;
  }
  raymarchLean_ = rayLeanFuture_.get();
  rayLeanPublished_ = true;
}

bool Simulation::BuildPipelines(const rhi::Device& device, std::string* err) {
  // F5 REBUILDS MUST NOT RACE THE DEFERRED SET. vkCreateComputePipelines is
  // not cancellable, and the modules the background thread is compiling from
  // are owned by the previous call's locals. Join first, always — a no-op on
  // the first build and on any build whose predecessor already published.
  WaitForFarPipelines();
  // Same argument for the raymarch variant's background compile: F5 replaces
  // the module it is reading from.
  PollRaymarchVariant(/*block=*/true);
  raymarchLean_ = {};

  // `--shader-stats` forces the whole build serial: capture is a create flag
  // and the executable-properties query it feeds reads per-pipeline driver
  // state, which is not a thing to interrogate from six threads at once.
  const unsigned buildThreads =
      rhi::vkr::CaptureStats(device) ? 1u : kBuildThreads;

  // ---- module loads, in parallel -----------------------------------------
  // LoadShader is a file read plus the generated preludes, and today that is
  // cheap: Tint runs LATER, inside GetShaderModule, because the entry point is
  // not known until pipeline creation — so this batch is fanned out mostly to
  // be the right shape. Each job reads only globals that were set before this
  // function was entered (the tuning, the tree lattice, the world map) and
  // writes only its own handle.
  rhi::ShaderModule mWorldgen, mMutate, mCompact, mStep, mOcc, mPick;
  // The rain exposure map (sim_rain_expo.wgsl): one tick entry on simPL_.
  rhi::ShaderModule mRainExpo;
  rhi::ShaderModule mDraft;
  // The solute layer's allocator, diffusion, compaction and hash
  // (docs/PLAN_solutes.md). Its own module: the CA carries mass through its
  // liquid moves and this file does everything else.
  rhi::ShaderModule mSolute;
  // The temperature layer (sim_heat.wgsl, src/sim/heat.h): pages, sources,
  // targets and relaxation, after the CA on its dirty list.
  rhi::ShaderModule mHeat;
  // The openness grid's writer (docs/PLAN_gi.md §2). A RENDER-path module among
  // the sim ones for shadow_resolve.wgsl's reason: BuildPipelines is the single
  // place F5 recompiles, and the pass and the raymarch's reader must be
  // rebuilt together or one of them is a tick behind the other's layout.
  rhi::ShaderModule mOpenness;
  // The glow field's writer (src/sim/world.h kGlowBytes). A render-path module
  // among the sim ones for mOpenness's reason: BuildPipelines is the single
  // place F5 recompiles, and the producer and its readers must be rebuilt
  // together or one of them is a tick behind the other's layout.
  rhi::ShaderModule mGlow;
  // The shadow cache's resolve. A RENDER-path module living among the sim ones
  // because BuildPipelines is the single place F5 recompiles, and the cache's
  // enable flag has to be recomputed in lockstep with raymarch.wgsl's const.
  rhi::ShaderModule mShadow;
  // The clouds (cloud.wgsl): six render-path entry points on shadowPL_, loaded
  // here for mShadow's reason — F5 recompiles everything through this function.
  rhi::ShaderModule mCloud;
  // The far cascade's sky bound (sky_top.wgsl): two per-frame entries on
  // shadowPL_, its own module because it writes the farOcc shadow_resolve reads.
  rhi::ShaderModule mSkyTop;
  // The ray-start map (ray_start.wgsl): two per-frame entries on shadowPL_.
  rhi::ShaderModule mRayStart;
  // The rain shadow map (rain_map.wgsl): two per-frame entries on shadowPL_.
  rhi::ShaderModule mRainMap;
  rhi::ShaderModule mWindStreak;
  rhi::ShaderModule mExplode, mParticle, mFluid, mFluidSeam, mWaterBody, mGas;
  rhi::ShaderModule mRay, mDebris, mMicroBody, mDebugLines, mDebugWind, mDebugCur;
  // The TAA resolve (taa.wgsl). Its own tiny module on purpose: the render
  // pipeline it feeds must stay a fast driver compile, and growing raymarch's
  // entry to hold it would put it behind the one shader on the critical path.
  rhi::ShaderModule mTaa;
  rhi::ShaderModule mDenoise;
  {
    PipelineBuildPool loads;
    auto mod = [&](rhi::ShaderModule* into, const char* name) {
      loads.Add([this, into, name, &device] {
        *into = LoadShader(device, shaderDir_, name);
      });
    };
    mod(&mWorldgen, "worldgen.wgsl");
    mod(&mMutate, "sim_mutate.wgsl");
    mod(&mRainExpo, "sim_rain_expo.wgsl");
    mod(&mDraft, "sim_draft.wgsl");
    mod(&mCompact, "sim_compact.wgsl");
    mod(&mStep, "sim_step.wgsl");
    mod(&mSolute, "sim_solute.wgsl");
    mod(&mHeat, "sim_heat.wgsl");
    mod(&mOcc, "sim_occupancy.wgsl");
    mod(&mPick, "sim_pick.wgsl");
    mod(&mOpenness, "sim_openness.wgsl");
    mod(&mGlow, "sim_glow.wgsl");
    mod(&mShadow, "shadow_resolve.wgsl");
    mod(&mSkyTop, "sky_top.wgsl");
    mod(&mRayStart, "ray_start.wgsl");
    mod(&mRainMap, "rain_map.wgsl");
    mod(&mWindStreak, "wind_streak.wgsl");
    mod(&mCloud, "cloud.wgsl");
    mod(&mExplode, "sim_explode.wgsl");
    mod(&mParticle, "sim_particle.wgsl");
    mod(&mGas, "sim_gas.wgsl");
    mod(&mFluid, "sim_fluid.wgsl");
    mod(&mFluidSeam, "sim_fluid_seam.wgsl");
    mod(&mWaterBody, "sim_waterbody.wgsl");
    mod(&mRay, "raymarch.wgsl");
    mod(&mDebris, "debris.wgsl");
    mod(&mMicroBody, "microbody.wgsl");
    mod(&mDebugLines, "debug_lines.wgsl");
    mod(&mDebugWind, "debug_wind.wgsl");
    mod(&mDebugCur, "debug_current.wgsl");
    mod(&mTaa, "taa.wgsl");
    mod(&mDenoise, "denoise.wgsl");
    loads.Run(buildThreads);
  }
  if (!mWorldgen || !mMutate || !mCompact || !mStep || !mSolute || !mHeat || !mOcc || !mPick ||
      !mOpenness || !mGlow ||
      !mExplode || !mParticle || !mGas || !mFluid || !mFluidSeam || !mWaterBody ||
      !mRay || !mDebris ||
      !mMicroBody || !mDebugLines || !mDebugWind || !mDebugCur || !mSkyTop ||
      !mRayStart || !mRainMap || !mWindStreak) {
    if (err) *err = "shader file read failure";
    return false;
  }

  // ---- batch A: worldgen's SYNCHRONOUS entry points --------------------
  // Its own batch, and not merged with batch B, so the cache save below still
  // means what it meant when this was serial: worldgen's entry points are the
  // multi-minute compile, and a launch killed anywhere past that line
  // (build.sh's taskkill, a user giving up on a stalled load) would otherwise
  // throw the work away and pay it again on every retry. `far`/`fardown` are
  // NOT here — see the deferred block after batch B.
  {
    PipelineBuildPool pool;
    pool.Add([&] {
      worldgen_ = MakeComputePipeline(device, simPL_, mWorldgen, "main", "worldgen");
    });
    pool.Add([&] {
      worldgenList_ =
          MakeComputePipeline(device, simPL_, mWorldgen, "list", "worldgenList");
    });
    // The column-cache pre-pass both of the above read (worldgen.wgsl `cols`).
    pool.Add([&] {
      worldgenCols_ =
          MakeComputePipeline(device, simPL_, mWorldgen, "cols", "worldgenCols");
    });
    // Same module as worldgen: the JITTER page fill shares genChunk's
    // slot->world mapping and must not drift from it (world.h's JITTER block).
    pool.Add([&] {
      pageFill_ = MakeComputePipeline(device, simPL_, mWorldgen, "pagefill", "pageFill");
    });
    pool.Run(buildThreads);
  }
  rhi::vkr::SavePipelineCache(device_);

  // ---- batch B: everything else that is not far ------------------------
  // ~45 pipelines, 30-60 s of driver work on a cold cache when it was serial
  // (raymarch and the fluid family dominate). Every job writes exactly one
  // member, so the pool's join is all the synchronization the reads below
  // need.
  PipelineBuildPool pool;
  // The shadow cache's two render-path kernels. Loaded unconditionally even
  // when the cache is compiled out of raymarch.wgsl: the pipelines are cheap,
  // and EncodeShadowResolve is what decides whether they ever run, so the
  // enable path is ONE test in one place rather than a load-time fork.
  pool.Add([&] { shadowPrepare_ = MakeComputePipeline(device, shadowPL_, mShadow, "prepare", "shadowPrepare"); });
  pool.Add([&] { shadowResolve_ = MakeComputePipeline(device, shadowPL_, mShadow, "resolve", "shadowResolve"); });
  pool.Add([&] { skyTopClear_ = MakeComputePipeline(device, shadowPL_, mSkyTop, "skyTopClear", "skyTopClear"); });
  pool.Add([&] { skyTopReduce_ = MakeComputePipeline(device, shadowPL_, mSkyTop, "skyTopReduce", "skyTopReduce"); });
  pool.Add([&] { rayStartTrace_ = MakeComputePipeline(device, shadowPL_, mRayStart, "rayStartTrace", "rayStartTrace"); });
  pool.Add([&] { rayStartMin_ = MakeComputePipeline(device, shadowPL_, mRayStart, "rayStartMin", "rayStartMin"); });
  pool.Add([&] { rainMapPrep_ = MakeComputePipeline(device, shadowPL_, mRainMap, "rainMapPrep", "rainMapPrep"); });
  pool.Add([&] { rainMapBuild_ = MakeComputePipeline(device, shadowPL_, mRainMap, "rainMapBuild", "rainMapBuild"); });
  pool.Add([&] { windStreak_ = MakeComputePipeline(device, shadowPL_, mWindStreak, "update", "windStreak"); });
  if (mCloud) {
    pool.Add([&] { cloudNoise_ = MakeComputePipeline(device, shadowPL_, mCloud, "noise", "cloudNoise"); });
    pool.Add([&] { cloudWeather_ = MakeComputePipeline(device, shadowPL_, mCloud, "weather", "cloudWeather"); });
    pool.Add([&] { cloudShadow_ = MakeComputePipeline(device, shadowPL_, mCloud, "shadow", "cloudShadow"); });
    pool.Add([&] { cloudEnv_ = MakeComputePipeline(device, shadowPL_, mCloud, "env", "cloudEnv"); });
    pool.Add([&] { cloudMarch_ = MakeComputePipeline(device, shadowPL_, mCloud, "march", "cloudMarch"); });
    pool.Add([&] { cloudResolve_ = MakeComputePipeline(device, shadowPL_, mCloud, "resolve", "cloudResolve"); });
  }
  pool.Add([&] { mutate_ = MakeComputePipeline(device, simPL_, mMutate, "main", "mutate"); });
  pool.Add([&] { mutateCells_ = MakeComputePipeline(device, simPL_, mMutate, "cells", "mutateCells"); });
  // The wind primitive footprint wake — same module, third entry point. It
  // needs only dirtyIn/dirtyOut and TickParams, all of which simPL_ already
  // binds, so a fan costs no new binding and no new layout.
  pool.Add([&] { windWake_ = MakeComputePipeline(device, simPL_, mMutate, "windWake", "windWake"); });
  pool.Add([&] { rainFall_ = MakeComputePipeline(device, simPL_, mMutate, "rainFall", "rainFall"); });
  pool.Add([&] { rainExpo_ = MakeComputePipeline(device, simPL_, mRainExpo, "build", "rainExpo"); });
  pool.Add([&] { draftMaskAll_ = MakeComputePipeline(device, simPL_, mDraft, "maskAll", "draftMaskAll"); });
  pool.Add([&] { draftMaskDirty_ = MakeComputePipeline(device, simPL_, mDraft, "maskDirty", "draftMaskDirty"); });
  pool.Add([&] { draftArgs_ = MakeComputePipeline(device, simPL_, mDraft, "args", "draftArgs"); });
  pool.Add([&] { draftCoarseBuild_ = MakeComputePipeline(device, simPL_, mDraft, "coarseBuild", "draftCoarseBuild"); });
  pool.Add([&] { draftCoarseFaces_ = MakeComputePipeline(device, simPL_, mDraft, "coarseFaces", "draftCoarseFaces"); });
  pool.Add([&] { draftCoarseSolve_ = MakeComputePipeline(device, simPL_, mDraft, "coarseSolve", "draftCoarseSolve"); });
  pool.Add([&] { draftFineFirst_ = MakeComputePipeline(device, simPL_, mDraft, "fineFirst", "draftFineFirst"); });
  pool.Add([&] { draftFineMid_ = MakeComputePipeline(device, simPL_, mDraft, "fineMid", "draftFineMid"); });
  pool.Add([&] { draftFineMid2_ = MakeComputePipeline(device, simPL_, mDraft, "fineMid2", "draftFineMid2"); });
  pool.Add([&] { draftFineLast_ = MakeComputePipeline(device, simPL_, mDraft, "fineLast", "draftFineLast"); });
  // The solute POUR (world.h CellOpSolute): in the mutate module because it
  // writes voxels (the powder a pour with no solvent leaves).
  pool.Add([&] { solPour_ = MakeComputePipeline(device, simPL_, mMutate, "solPour", "solPour"); });
  pool.Add([&] { compact_ = MakeComputePipeline(device, simPL_, mCompact, "main", "compact"); });
  pool.Add([&] { compactNext_ = MakeComputePipeline(device, simPL_, mCompact, "mainNext", "compactNext"); });
  pool.Add([&] { step_ = MakeComputePipeline(device, simPL_, mStep, "main", "step"); });
  // Same module as the CA: the snapshot has to agree with the kernel that
  // reads it about the bit layout, and one module is how that is enforced.
  pool.Add([&] {
    reposeSnap_ =
        MakeComputePipeline(device, simPL_, mStep, "reposesnap", "reposeSnap");
  });
  pool.Add([&] { caMask_ = MakeComputePipeline(device, simPL_, mStep, "camask", "caMask"); });
  pool.Add([&] { occupancy_ = MakeComputePipeline(device, simPL_, mOcc, "main", "occupancy"); });
  pool.Add([&] { occupancyDirty_ = MakeComputePipeline(device, simPL_, mOcc, "mainDirty", "occupancyDirty"); });
  pool.Add([&] { opennessDirty_ = MakeComputePipeline(device, simPL_, mOpenness, "dirty", "opennessDirty"); });
  pool.Add([&] { opennessRefresh_ = MakeComputePipeline(device, simPL_, mOpenness, "refresh", "opennessRefresh"); });
  pool.Add([&] { glowSrc_ = MakeComputePipeline(device, simPL_, mGlow, "src", "glowSrc"); });
  pool.Add([&] { glowField_ = MakeComputePipeline(device, simPL_, mGlow, "field", "glowField"); });
  pool.Add([&] { glowRefresh_ = MakeComputePipeline(device, simPL_, mGlow, "refresh", "glowRefresh"); });
  pool.Add([&] { pick_ = MakeComputePipeline(device, simPL_, mPick, "main", "pick"); });
  // The solute layer (docs/PLAN_solutes.md). On simPL_ like the CA it rides
  // beside: the table, pool, meta and species table are bindings 40..43.
  pool.Add([&] { solWant_ = MakeComputePipeline(device, simPL_, mSolute, "solWant", "solWant"); });
  pool.Add([&] { solArgs_ = MakeComputePipeline(device, simPL_, mSolute, "solArgs", "solArgs"); });
  pool.Add([&] { solAlloc_ = MakeComputePipeline(device, simPL_, mSolute, "solAlloc", "solAlloc"); });
  pool.Add([&] { solDiffuse_ = MakeComputePipeline(device, simPL_, mSolute, "solDiffuse", "solDiffuse"); });
  pool.Add([&] { solCompact_ = MakeComputePipeline(device, simPL_, mSolute, "solCompact", "solCompact"); });
  pool.Add([&] { solScoop_ = MakeComputePipeline(device, simPL_, mSolute, "solScoop", "solScoop"); });
  pool.Add([&] { solHash_ = MakeComputePipeline(device, simPL_, mSolute, "solHash", "solHash"); });
  pool.Add([&] { solEvict_ = MakeComputePipeline(device, simPL_, mSolute, "solEvict", "solEvict"); });
  pool.Add([&] { solRestore_ = MakeComputePipeline(device, simPL_, mSolute, "solRestore", "solRestore"); });
  // The temperature layer (sim_heat.wgsl). On simPL_: bindings 50..52.
  pool.Add([&] { heatBegin_ = MakeComputePipeline(device, simPL_, mHeat, "heatBegin", "heatBegin"); });
  pool.Add([&] { heatShift_ = MakeComputePipeline(device, simPL_, mHeat, "heatShift", "heatShift"); });
  pool.Add([&] { heatWant_ = MakeComputePipeline(device, simPL_, mHeat, "heatWant", "heatWant"); });
  pool.Add([&] { heatArgs_ = MakeComputePipeline(device, simPL_, mHeat, "heatArgs", "heatArgs"); });
  pool.Add([&] { heatAlloc_ = MakeComputePipeline(device, simPL_, mHeat, "heatAlloc", "heatAlloc"); });
  pool.Add([&] { heatSrc_ = MakeComputePipeline(device, simPL_, mHeat, "heatSrc", "heatSrc"); });
  pool.Add([&] { heatTent_ = MakeComputePipeline(device, simPL_, mHeat, "heatTent", "heatTent"); });
  pool.Add([&] { heatRelax_ = MakeComputePipeline(device, simPL_, mHeat, "heatRelax", "heatRelax"); });

  pool.Add([&] { explodeMark_ = MakeComputePipeline(device, simPL2_, mExplode, "mark", "explodeMark"); });
  pool.Add([&] { explodeApply_ = MakeComputePipeline(device, simPL2_, mExplode, "apply", "explodeApply"); });
  pool.Add([&] { pArgs1_ = MakeComputePipeline(device, simPL2_, mParticle, "args1", "pArgs1"); });
  pool.Add([&] { pSpawn_ = MakeComputePipeline(device, simPL2_, mParticle, "spawn", "pSpawn"); });
  pool.Add([&] { pIntegrate_ = MakeComputePipeline(device, simPL2_, mParticle, "integrate", "pIntegrate"); });
  pool.Add([&] { pArgs2_ = MakeComputePipeline(device, simPL2_, mParticle, "args2", "pArgs2"); });
  pool.Add([&] { pResolve_ = MakeComputePipeline(device, simPL2_, mParticle, "resolve", "pResolve"); });
  pool.Add([&] { gArgs1_ = MakeComputePipeline(device, gasPL_, mGas, "gasArgs1", "gasArgs1"); });
  pool.Add([&] { gSpawn_ = MakeComputePipeline(device, gasPL_, mGas, "gasSpawnStep", "gasSpawn"); });
  pool.Add([&] { gIntegrate_ = MakeComputePipeline(device, gasPL_, mGas, "gasIntegrate", "gasIntegrate"); });
  pool.Add([&] { gArgs2_ = MakeComputePipeline(device, gasPL_, mGas, "gasArgs2", "gasArgs2"); });
  pool.Add([&] { gResolve_ = MakeComputePipeline(device, gasPL_, mGas, "gasResolve", "gasResolve"); });
  pool.Add([&] { gFarPlume_ = MakeComputePipeline(device, gasPL_, mGas, "gasFarPlume", "gasFarPlume"); });
  pool.Add([&] { gFarPlumeW_ = MakeComputePipeline(device, gasPL_, mGas, "gasFarPlumeWide", "gasFarPlumeWide"); });

  pool.Add([&] { fluidMark_ = MakeComputePipeline(device, fluidPL_, mFluid, "mark", "fluidMark"); });
  pool.Add([&] { fluidAlloc_ = MakeComputePipeline(device, fluidPL_, mFluid, "alloc", "fluidAlloc"); });
  pool.Add([&] { fluidClear_ = MakeComputePipeline(device, fluidPL_, mFluid, "clearGrid", "fluidClear"); });
  pool.Add([&] { fluidP2g_ = MakeComputePipeline(device, fluidPL_, mFluid, "p2g1", "fluidP2g1"); });
  pool.Add([&] { fluidP2g2_ = MakeComputePipeline(device, fluidPL_, mFluid, "p2g2", "fluidP2g2"); });
  pool.Add([&] { fluidGridUp_ = MakeComputePipeline(device, fluidPL_, mFluid, "gridUpdate", "fluidGridUp"); });
  pool.Add([&] { fluidG2p_ = MakeComputePipeline(device, fluidPL_, mFluid, "g2p", "fluidG2p"); });

  // The excite/settle seam (sim_fluid_seam.wgsl; fluidSpawn_ moved here —
  // appends go through the seam's GPU-owned count now).
  pool.Add([&] { fluidSpawn_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "spawnAppend", "seamSpawn"); });
  pool.Add([&] { fluidCompactCount_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "compactCount", "seamCompactCount"); });
  pool.Add([&] { fluidCompactScan_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "compactScan", "seamCompactScan"); });
  pool.Add([&] { fluidCompactScatter_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "compactScatter", "seamCompactScatter"); });
  pool.Add([&] { fluidExciteDetect_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "exciteDetect", "seamExciteDetect"); });
  pool.Add([&] { fluidExciteScan_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "exciteScan", "seamExciteScan"); });
  pool.Add([&] { fluidExciteEmit_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "exciteEmit", "seamExciteEmit"); });
  pool.Add([&] { fluidPTick_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "particleTick", "seamParticleTick"); });
  pool.Add([&] { fluidSettleJudge_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "settleJudge", "seamSettleJudge"); });
  pool.Add([&] { fluidSettleScan_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "settleScan", "seamSettleScan"); });
  pool.Add([&] { fluidSettleBin_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "settleBin", "seamSettleBin"); });
  pool.Add([&] { fluidSettleCheck_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "settleCheck", "seamSettleCheck"); });
  pool.Add([&] { fluidSettleCommit_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "settleCommit", "seamSettleCommit"); });
  pool.Add([&] { fluidSettleKill_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "settleKill", "seamSettleKill"); });
  pool.Add([&] { fluidConsumeApply_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "consumeApply", "seamConsumeApply"); });
  pool.Add([&] { fluidStainApply_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "stainApply", "seamStainApply"); });
  pool.Add([&] { fluidMirrorFold_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "mirrorFold", "seamMirrorFold"); });
  pool.Add([&] { fluidCellClear_ = MakeComputePipeline(device, fluidSeamPL_, mFluidSeam, "cellClear", "seamCellClear"); });

  // Water bodies (docs/PLAN_water_master.md M2). On simPL_ like the CA:
  // everything the shave needs to write a voxel — voxels, dirtyOut,
  // pageTable, pageFaults, TickParams — is already in that layout, and the
  // ledger buffer is one added binding rather than a new group.
  pool.Add([&] { waterQuiet_ = MakeComputePipeline(device, simPL_, mWaterBody, "wbQuiet", "waterQuiet"); });
  pool.Add([&] { waterLedger_ = MakeComputePipeline(device, simPL_, mWaterBody, "wbLedger", "waterLedger"); });
  pool.Add([&] { waterReduce_ = MakeComputePipeline(device, simPL_, mWaterBody, "wbReduce", "waterReduce"); });
  pool.Add([&] { waterShave_ = MakeComputePipeline(device, simPL_, mWaterBody, "wbShave", "waterShave"); });
  pool.Add([&] { waterDrain_ = MakeComputePipeline(device, simPL_, mWaterBody, "wbDrain", "waterDrain"); });
  pool.Add([&] { waterHole_ = MakeComputePipeline(device, simPL_, mWaterBody, "wbHole", "waterHole"); });
  // W1: the relevel apply and the free-surface measure (PLAN_water_relevel.md).
  pool.Add([&] { waterRelevel_ = MakeComputePipeline(device, simPL_, mWaterBody, "wbRelevel", "waterRelevel"); });
  pool.Add([&] { waterSurface_ = MakeComputePipeline(device, simPL_, mWaterBody, "wbSurface", "waterSurface"); });
  pool.Add([&] { waterFlux_ = MakeComputePipeline(device, simPL_, mWaterBody, "wbFlux", "waterFlux"); });
  // M5: the scheduled container sweep (components 2 case 2 + 10).
  pool.Add([&] { waterSweep_ = MakeComputePipeline(device, simPL_, mWaterBody, "wbSweep", "waterSweep"); });
  pool.Add([&] { waterSplit_ = MakeComputePipeline(device, simPL_, mWaterBody, "wbSplit", "waterSplit"); });
  pool.Run(buildThreads);

  // Decided HERE, in the function that also compiles raymarch.wgsl, and from the
  // same two inputs its SHADOW_CACHE const is built from. That co-location is
  // the point: the pass and the shader must agree, and F5 recompiles both
  // through this function, so a tuning flip cannot leave one side switched.
  // AFTER the join, because it reads two pipelines the pool produced.
  // A fresh cloud pipeline may be a fresh noise function: rebake on the next
  // frame rather than trust a volume the old shader wrote.
  cloudBaked_ = false;
  shadowCacheOn_ = FragmentStoresAvailable() &&
                   CurrentTuning().render.shadowCache != 0 &&
                   (bool)shadowPrepare_ && (bool)shadowResolve_;

  // Second save: the rest of the compute block above is another 30-60 s of
  // driver work on a cold cache (raymarch and the fluid family dominate).
  rhi::vkr::SavePipelineCache(device_);

  // ---- deferred: the far cascades --------------------------------------
  //
  // `far` is 746 s of driver compile and `fardown` 98 s — between them, the
  // whole reason a cold boot was 17 minutes. worldgen.wgsl's far block and
  // DESIGN.md §9 both say the cascades are RENDER-ONLY DERIVED DATA with no
  // determinism attached, so the world can tick, hash and save without them;
  // all that is missing is the horizon. So they are built off the critical
  // path (StartFarBuild) and this function returns while they are still
  // compiling — or, under FarBuild::Lazy, before they have started: a run that
  // never asks for cascade content never compiles them at all.
  //
  // The previous build's far set is dropped here whichever policy applies.
  // WaitForFarPipelines at the top of this function joined its thread, so
  // nothing is compiling from the module these handles came from.
  farFill_ = {};
  farMapFill_ = {};
  farPatchFill_ = {};
  farDown_ = {};
  farPublished_ = false;
  farStarted_ = false;
  farReady_.store(false, std::memory_order_release);
  farModule_ = mWorldgen;
  farBuildThreads_ = buildThreads;
  // Serial under `--shader-stats` for the same reason the pool is: that mode
  // exists to interrogate every pipeline the driver compiled this run — so it
  // is also never lazy.
  if (farBuild_ == FarBuild::Eager || buildThreads <= 1) StartFarBuild(buildThreads);
  MarkInteractiveReady();

  // A backend that fails pipeline creation returns an INVALID handle (Vulkan:
  // Tint or vkCreateComputePipelines refused). Dawn reports errors through its
  // async error scope and always returns a valid handle, so this check is free
  // there — but on Vulkan a null pipeline would make the recorder silently
  // skip the row, which is a wrong SIM, not a crash. Fail the build instead.
  //
  // The far set is NOT in this list: it may still be compiling, and a
  // skipped far row is a missing horizon rather than a wrong sim. Their
  // verdict is checked where they are published (PublishFarPipelines).
  if (!worldgen_ || !worldgenList_ || !worldgenCols_ || !pageFill_ || !mutate_ ||
      !mutateCells_ || !windWake_ || !rainFall_ || !rainExpo_ ||
      !draftMaskAll_ || !draftMaskDirty_ || !draftArgs_ || !draftCoarseBuild_ || !draftCoarseFaces_ ||
      !draftCoarseSolve_ || !draftFineFirst_ || !draftFineMid_ || !draftFineMid2_ || !draftFineLast_ || !compact_ || !compactNext_ || !step_ || !occupancy_ ||
      !occupancyDirty_ || !pick_ || !explodeMark_ || !explodeApply_ || !pArgs1_ ||
      !solWant_ || !solArgs_ || !solAlloc_ || !solDiffuse_ || !solCompact_ || !solScoop_ || !solPour_ || !solHash_ || !solEvict_ || !solRestore_ ||
      !heatBegin_ || !heatShift_ || !heatWant_ || !heatArgs_ || !heatAlloc_ || !heatSrc_ || !heatTent_ || !heatRelax_ ||
      !pSpawn_ || !pIntegrate_ || !pArgs2_ || !pResolve_ ||
      !gArgs1_ || !gSpawn_ || !gIntegrate_ || !gArgs2_ || !gResolve_ ||
      !fluidSpawn_ ||
      !fluidMark_ || !fluidAlloc_ || !fluidClear_ || !fluidP2g_ ||
      !fluidP2g2_ || !fluidGridUp_ || !fluidG2p_ || !fluidCompactCount_ ||
      !fluidCompactScan_ || !fluidCompactScatter_ || !fluidExciteDetect_ ||
      !fluidExciteScan_ || !fluidExciteEmit_ || !fluidPTick_ ||
      !fluidSettleJudge_ || !fluidSettleScan_ || !fluidSettleBin_ ||
      !fluidSettleCheck_ || !fluidSettleCommit_ || !fluidSettleKill_ ||
      !fluidConsumeApply_ || !fluidStainApply_ || !fluidMirrorFold_ ||
      !fluidCellClear_ || !waterDrain_ || !waterHole_ ||
      !waterQuiet_ || !waterLedger_ || !waterReduce_ ||
      !waterShave_ || !waterSweep_ || !waterSplit_ ||
      !waterRelevel_ || !waterSurface_ || !waterFlux_) {
    if (err) *err = "compute pipeline creation failed (see stderr for the shader)";
    return false;
  }

  raymarchModule_ = mRay;
  // The lean variant's MODULE (its pipeline is deferred, in
  // EnsureRenderPipelines). Derived here, serially, and deliberately not inside
  // the load pool above: it reads mRay's assembled source, which does not exist
  // until that pool has joined.
  BuildRaymarchVariant(device, mRay);
  taaModule_ = mTaa;
  dnModule_ = mDenoise;
  debrisModule_ = mDebris;
  microBodyModule_ = mMicroBody;
  debugLineModule_ = mDebugLines;
  debugWindModule_ = mDebugWind;
  windStreakModule_ = mWindStreak;
  debugCurModule_ = mDebugCur;
  targetFormat_ = rhi::TextureFormat::Undefined;  // force render pipeline rebuild
  return true;
}

// ---- the deferred far set's three main-thread entry points ----------------
//
// Only this thread ever writes the far pipelines (the compile thread writes
// them into the future's result and nothing else), so PassPipeline's reads
// need no synchronization: a row either sees the pre-publish INVALID handle
// and is skipped, or sees the published one. `farReady_` is the release/
// acquire edge that makes the second case's handle fully constructed.

void Simulation::StartFarBuild(unsigned threads) {
  if (farStarted_ || !farModule_) return;
  farStarted_ = true;
  // Everything the compile thread touches is captured BY VALUE (the device,
  // the layout, the module — all seam handles, all shared_ptr) so it names no
  // Simulation member and cannot race this object. The result is published on
  // the main thread by PollFarPipelines / WaitForFarPipelines.
  const rhi::Device dev = device_;
  const rhi::PipelineLayout layout = farPL_;
  const rhi::ShaderModule module = farModule_;
  auto build = [dev, layout, module]() {
    FarPipelines r;
    // One thread per entry point: sequentially these are the sum, in
    // parallel they are max(), which is the wall clock this whole package is
    // bounded by. `farpatch` joined the set with package C's split of `far`
    // into sweep + edit-patch — the split only pays if the halves compile
    // CONCURRENTLY, so it gets a thread of its own like `fardown` did.
    std::thread down([&] {
      r.down = MakeComputePipeline(dev, layout, module, "fardown", "farDown");
    });
    std::thread patch([&] {
      r.patch = MakeComputePipeline(dev, layout, module, "farpatch",
                                    "farPatchFill");
    });
    // The surface map's fill (LOD-seam package A): its own genColumn + skin
    // copies, so its own thread for the same max()-not-sum reason.
    std::thread map([&] {
      r.map = MakeComputePipeline(dev, layout, module, "farmap", "farMapFill");
    });
    r.fill = MakeComputePipeline(dev, layout, module, "far", "farFill");
    map.join();
    patch.join();
    down.join();
    // SANDVOX_FAR_DELAY_S: hold the horizon back by N seconds. The cascades'
    // arrival is a several-second transient the game has to survive — the
    // frame loop holds FarField's queue for the whole of it
    // (Simulation::FarFillsDeferred) so the fog and the valid box do not
    // claim a horizon that does not exist yet — and the only way to reach
    // that state honestly was a cold SPIR-V cache, i.e. a ~12 minute driver
    // compile of `far`. One env var makes it a normal run, which is the
    // difference between a path that is tested and one that is argued about.
    if (const char* d = std::getenv("SANDVOX_FAR_DELAY_S")) {
      const double secs = std::atof(d);
      if (secs > 0.0) {
        std::printf("far-cascade pipelines: holding %.1f s "
                    "(SANDVOX_FAR_DELAY_S)\n", secs);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::duration<double>(secs));
      }
    }
    MarkFarReady();
    // THE HORIZON'S ARRIVAL TIME, printed unconditionally. It is the number
    // this whole package is measured by and there is no other record of it
    // in a windowed run (build/last_run.json is a headless-mode artifact),
    // so it must not be behind SANDVOX_SHADER_TIMING.
    std::printf("far-cascade pipelines ready %.1f s after the first pipeline "
                "create (the game has been running since %.1f s)\n",
                FarReadyMs() / 1000.0, InteractiveReadyMs() / 1000.0);
    std::fflush(stdout);
    // The horizon's ISA is the single most expensive thing on this disk.
    // Saved from this thread the moment it exists, for the same reason the
    // two saves in BuildPipelines exist: the next launch must not pay it
    // again because this one was killed.
    rhi::vkr::SavePipelineCache(dev);
    return r;
  };
  if (threads <= 1) {
    FarPipelines r = build();
    farFill_ = std::move(r.fill);
    farMapFill_ = std::move(r.map);
    farPatchFill_ = std::move(r.patch);
    farDown_ = std::move(r.down);
    farPublished_ = true;
    farReady_.store(true, std::memory_order_release);
  } else {
    farFuture_ = std::async(std::launch::async, build);
  }
}

void Simulation::PublishFarPipelines() {
  FarPipelines r = farFuture_.get();  // blocks if the thread is still running
  farFill_ = std::move(r.fill);
  farMapFill_ = std::move(r.map);
  farPatchFill_ = std::move(r.patch);
  farDown_ = std::move(r.down);
  farPublished_ = true;
  farReady_.store(true, std::memory_order_release);
  // Not fatal: a failed far compile costs the horizon, not the sim. Say so
  // once — silence here would read as "the cascades are just empty".
  if (!farFill_ || !farMapFill_ || !farPatchFill_ || !farDown_)
    std::fprintf(stderr,
                 "far-cascade pipelines failed to compile; the horizon will "
                 "stay empty (worldgen.wgsl far/farmap/farpatch/fardown)\n");
}

bool Simulation::PollFarPipelines() {
  if (farPublished_ || !farFuture_.valid()) return false;
  if (farFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    return false;
  PublishFarPipelines();
  return true;
}

void Simulation::WaitForFarPipelines() {
  if (farPublished_ || !farFuture_.valid()) return;
  // Announced, because from the outside this is a headless run sitting silent
  // for up to twelve minutes with no output.
  if (farFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    std::printf("waiting for the deferred far-cascade pipelines "
                "(worldgen.wgsl far/farmap/farpatch/fardown)...\n");
  std::fflush(stdout);
  PublishFarPipelines();
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
//
// THE recorder's own type (sim/pass_table.h pass::RecordCtx), not a mirror of
// it: RecordTable passes it through the bridge by reference. It used to be one
// of three hand-copied structs, and three fields went missing across the
// copies before this was one.
using RecordCtx = pass::RecordCtx;

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
    case B::FarSig:         return world_->farSig;
    case B::FarMap:         return world_->farMap;
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
    case B::ShadowCache:         return world_->shadowCache;
    case B::ShadowReq:           return world_->shadowReq;
    case B::ShadowHist:          return world_->shadowHist;
    case B::RayStart:            return rayStartBuf_;
    case B::RainMap:             return rainMapBuf_;
    case B::RainExpo:            return rainExpoBuf_;
    case B::Draft:               return draftBuf_;
    case B::DraftMeta:           return draftMetaBuf_;
    case B::DraftArgs:           return draftArgsBuf_;
    case B::CaMask:              return caMaskBuf_;
    case B::CaWind:              return caWindBuf_;
    case B::WindStreaks:         return windStreakBuf_;
    case B::ShadowArgsStage:     return world_->shadowArgsStage;
    case B::ShadowArgs:          return world_->shadowArgs;
    case B::Openness:            return world_->openness;
    case B::OpennessGen:         return world_->opennessGen;
    case B::Irradiance:          return world_->irradiance;
    case B::Glow:                return world_->glow;
    case B::GenAct:              return world_->genAct;
    case B::WaterBodyState:      return world_->waterBodyState;
    case B::WaterFlux:           return world_->waterFlux;
    case B::ChunkHash:           return world_->chunkHash;
    case B::TreeAtlas:           return treeAtlasBuf_;
    case B::WorldMap:            return worldMapBuf_;
    case B::GenCols:             return genColsBuf_;
    case B::ColCache:            return colCacheBuf_;
    case B::SolTable:            return world_->solTable;
    case B::SolPool:             return world_->solPool;
    case B::SolMeta:             return world_->solMeta;
    case B::SolSpec:             return solSpecBuf_;
    case B::SolArgs:             return world_->solArgs;
    case B::SolStage:            return world_->solStage;
    case B::HeatPool:            return world_->heatPool;
    case B::HeatMeta:            return world_->heatMeta;
    case B::HeatParams:          return heatParamsBuf_;
    case B::HeatArgs:            return world_->heatArgs;
    case B::GasParticlesRead:    return world_->gasParticles[page_];
    case B::GasParticlesWrite:   return world_->gasParticles[1 - page_];
    case B::GasCounts:           return world_->gasCounts;
    case B::GasClaim:            return world_->gasClaim;
    case B::GasSpawn:            return world_->gasSpawn;
    case B::GasSpawnOps:         return world_->gasSpawnOps;
    case B::GasArgsStage:        return world_->gasArgs;
    case B::GasDispatchArgs:     return world_->gasDispatchArgs;
    case B::GasOuter:            return world_->gasOuter;
    case B::GasFarEmit:          return world_->gasFarEmit;
    case B::GasFarOuter:         return world_->gasFarOuter;
    case B::GasPlumeTrack:       return world_->gasPlumeTrack;
    case B::ReposeSnap:          return world_->reposeSnap;
    case B::CloudUBO:            return world_->cloudUBO;
    case B::CloudNoise:          return cloudNoiseBuf_;
    case B::CloudWeather:        return cloudWeatherBuf_;
    case B::CloudMaps:           return cloudMapsBuf_;
    case B::CloudRaw:            return cloudRawBuf_;
    case B::CloudHist:           return cloudHistBuf_;
    default:                return world_->voxels;
  }
}

const rhi::ComputePipeline& Simulation::PassPipeline(pass::Pipe p) const {
  using P = pass::Pipe;
  switch (p) {
    case P::Worldgen:       return worldgen_;
    case P::WorldgenList:   return worldgenList_;
    case P::WorldgenCols:   return worldgenCols_;
    case P::PageFill:       return pageFill_;
    case P::Mutate:         return mutate_;
    case P::MutateCells:    return mutateCells_;
    case P::WindWake:       return windWake_;
    case P::RainFall:       return rainFall_;
    case P::RainExpo:       return rainExpo_;
    case P::DraftMaskAll:   return draftMaskAll_;
    case P::DraftMaskDirty: return draftMaskDirty_;
    case P::DraftArgsP:     return draftArgs_;
    case P::DraftCoarseBuild: return draftCoarseBuild_;
    case P::DraftCoarseFaces: return draftCoarseFaces_;
    case P::DraftCoarseSolve: return draftCoarseSolve_;
    case P::DraftFineFirst:   return draftFineFirst_;
    case P::DraftFineMid:     return draftFineMid_;
    case P::DraftFineMid2:    return draftFineMid2_;
    case P::DraftFineLast:    return draftFineLast_;
    case P::Compact:        return compact_;
    case P::CompactNext:    return compactNext_;
    case P::Step:           return step_;
    case P::ReposeSnap:     return reposeSnap_;
    case P::CaMask:         return caMask_;
    case P::Occupancy:      return occupancy_;
    case P::OccupancyDirty: return occupancyDirty_;
    case P::Pick:           return pick_;
    case P::ExplodeMark:    return explodeMark_;
    case P::ExplodeApply:   return explodeApply_;
    case P::GasArgs1:       return gArgs1_;
    case P::GasSpawnP:      return gSpawn_;
    case P::GasIntegrate:   return gIntegrate_;
    case P::GasArgs2:       return gArgs2_;
    case P::GasResolve:     return gResolve_;
    case P::GasFarPlume:    return gFarPlume_;
    case P::GasFarPlumeWide: return gFarPlumeW_;
    case P::PArgs1:         return pArgs1_;
    case P::PSpawn:         return pSpawn_;
    case P::PIntegrate:     return pIntegrate_;
    case P::PArgs2:         return pArgs2_;
    case P::PResolve:       return pResolve_;
    case P::FarFill:        return farFill_;
    case P::FarPatchFill:   return farPatchFill_;
    case P::FarMapFill:     return farMapFill_;
    case P::FarDown:        return farDown_;
    case P::OpennessDirty:   return opennessDirty_;
    case P::OpennessRefresh: return opennessRefresh_;
    case P::GlowSrc:         return glowSrc_;
    case P::GlowField:       return glowField_;
    case P::GlowRefresh:     return glowRefresh_;
    case P::CloudNoise:     return cloudNoise_;
    case P::CloudWeather:   return cloudWeather_;
    case P::CloudShadow:    return cloudShadow_;
    case P::CloudEnv:       return cloudEnv_;
    case P::CloudMarch:     return cloudMarch_;
    case P::CloudResolve:   return cloudResolve_;
    case P::SkyTopClear:    return skyTopClear_;
    case P::SkyTopReduce:   return skyTopReduce_;
    case P::RayStartTrace:  return rayStartTrace_;
    case P::RayStartMin:    return rayStartMin_;
    case P::RainMapPrep:    return rainMapPrep_;
    case P::RainMapBuild:   return rainMapBuild_;
    case P::WindStreak:     return windStreak_;
    case P::ShadowPrepare:  return shadowPrepare_;
    case P::ShadowResolve:  return shadowResolve_;
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
    case P::WaterRelevel:   return waterRelevel_;
    case P::WaterSurface:   return waterSurface_;
    case P::WaterFluxPipe:  return waterFlux_;
    case P::FluidSettleCommit:   return fluidSettleCommit_;
    case P::FluidSettleKill:     return fluidSettleKill_;
    case P::FluidConsumeApply:   return fluidConsumeApply_;
    case P::FluidStainApply:     return fluidStainApply_;
    case P::FluidMirrorFold:     return fluidMirrorFold_;
    case P::FluidCellClear:      return fluidCellClear_;
    case P::SolWant:        return solWant_;
    case P::SolArgsP:       return solArgs_;
    case P::SolAlloc:       return solAlloc_;
    case P::SolDiffuse:     return solDiffuse_;
    case P::SolCompact:     return solCompact_;
    case P::SolScoop:       return solScoop_;
    case P::SolPour:        return solPour_;
    case P::SolHash:        return solHash_;
    case P::SolEvict:       return solEvict_;
    case P::SolRestore:     return solRestore_;
    case P::HeatBegin:      return heatBegin_;
    case P::HeatShift:      return heatShift_;
    case P::HeatWant:       return heatWant_;
    case P::HeatArgsP:      return heatArgs_;
    case P::HeatAlloc:      return heatAlloc_;
    case P::HeatSrc:        return heatSrc_;
    case P::HeatTent:       return heatTent_;
    case P::HeatRelax:      return heatRelax_;
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

  // Resolved as RAW seam pointers (rhi_record.h says why): this runs up to
  // fluidSubsteps + 4 times a tick, and as refcounted handles every call cost
  // ~180 atomic increments and as many decrements for data only read below.
  rhi::TableBindings tb{};
  for (int i = 0; i < (int)pass::Buf::kCount; i++)
    tb.buffers[i] = PassBuffer((pass::Buf)i).Get();
  // Bound by the LAST enumerator, which is what the note in pass_table.h's
  // Pipe block is about: a pipeline added past this bound is silently never
  // handed to the recorder, which reads as a skipped row rather than a crash.
  for (int i = 1; i < (int)pass::Pipe::ShadowResolve + 1; i++)
    tb.pipelines[i] = PassPipeline((pass::Pipe)i).Get();
  tb.simLayout = simPL_.Get();
  tb.slimPartLayout = simPL2_.Get();
  tb.slimFarLayout = farPL_.Get();
  tb.slimFluidLayout = fluidPL_.Get();
  tb.slimFluidSeamLayout = fluidSeamPL_.Get();
  tb.slimGasLayout = gasPL_.Get();
  tb.shadowLayout = shadowPL_.Get();
  tb.simSet = simBG_[page_].Get();
  tb.slimSet = simSlimBG_[page_].Get();
  tb.particleSet = particleBG_[page_].Get();
  tb.farSet = farBG_.Get();
  tb.fluidSet = fluidBG_[page_].Get();
  tb.fluidSeamSet = fluidSeamBG_[page_].Get();
  tb.gasSet = gasBG_[page_].Get();
  tb.shadowSet = shadowBG_.Get();

  rhi::RecordTableVulkan(enc, which, cx, tb,
                         passTimer_ && passTimer_->Valid() ? passTimer_ : nullptr);
}

void Simulation::EncodeWorldgen(const rhi::CommandEncoder& enc, bool denseGen) {
  page_ = 0;
  RecordCtx cx{};
  // Under --residency paged the caller runs worldgen BATCHED through
  // worldgenList instead (§3.5c) and passes false, which suppresses only the
  // whole-world dispatch — the fill rows still clear the transient buffers.
  cx.denseWorldgen = denseGen;
  // The dense `main` reads the column cache; WriteDenseGenList sized it.
  cx.genColCount = genColCount_;
  RecordTable(enc, pass::Table::Worldgen, &cx);
  // A freshly generated world is maximally unsettled — the first hundreds of
  // ticks ARE the settling. Same self-declaration rule (§3.4).
  NoteWakeAll();
}

uint32_t Simulation::WriteGenList(const rhi::Queue& queue,
                                  const std::vector<uint32_t>& slots) {
  queue.WriteBuffer(world_->genList, 0, slots.data(), slots.size() * 4);
  // genCols (worldgen.wgsl's column-cache block): per list POSITION the
  // index of its chunk-column, then per chunk-column its world chunk x/z and
  // the lowest / highest chunk y the list asks for there. Columns are
  // numbered in first-appearance order, so the table is a pure function of
  // the list and the window origin -- the same list replays the same
  // dispatch (the op record carries the list, not this table).
  const size_t n = slots.size();
  std::vector<uint32_t>& g = genColScratch_;
  g.assign(n, 0u);
  std::vector<uint32_t> recs;
  recs.reserve(64 * kGenColRecWords);
  std::unordered_map<uint64_t, uint32_t> index;
  index.reserve(n);
  for (size_t p = 0; p < n; p++) {
    const IVec3 wc = world_->SlotToWorldChunk(slots[p]);
    const uint64_t key = ((uint64_t)(uint32_t)wc.x << 32) | (uint32_t)wc.z;
    auto it = index.find(key);
    uint32_t k;
    if (it == index.end()) {
      k = (uint32_t)(recs.size() / kGenColRecWords);
      if (k >= kGenColMax) {
        std::fprintf(stderr, "WriteGenList: %zu slots name more than kGenColMax = %u "
                             "chunk-columns -- the column cache cannot hold them\n",
                     n, kGenColMax);
        std::abort();
      }
      index.emplace(key, k);
      recs.push_back((uint32_t)wc.x);
      recs.push_back((uint32_t)wc.z);
      recs.push_back((uint32_t)wc.y);
      recs.push_back((uint32_t)wc.y);
    } else {
      k = it->second;
      const int32_t lo = std::min((int32_t)recs[k * kGenColRecWords + 2], wc.y);
      const int32_t hi = std::max((int32_t)recs[k * kGenColRecWords + 3], wc.y);
      recs[k * kGenColRecWords + 2] = (uint32_t)lo;
      recs[k * kGenColRecWords + 3] = (uint32_t)hi;
    }
    g[p] = k;
  }
  if (n > 0) queue.WriteBuffer(genColsBuf_, 0, g.data(), n * 4);
  if (!recs.empty())
    queue.WriteBuffer(genColsBuf_, (uint64_t)kNumSlots * 4, recs.data(), recs.size() * 4);
  genColCount_ = (uint32_t)(recs.size() / kGenColRecWords);
  return genColCount_;
}

void Simulation::WriteDenseGenList(const rhi::Queue& queue) {
  // The WINDOW's slots. The dense `main` dispatch covers NUM_SLOTS workgroups,
  // but a ticket slot returns before genChunk (worldgen.wgsl: no ticket
  // survives a regen), so its list position is never read.
  std::vector<uint32_t> all(kNumChunks);
  for (uint32_t s = 0; s < kNumChunks; s++) all[s] = s;
  WriteGenList(queue, all);
}

void Simulation::EncodeGenList(const rhi::CommandEncoder& enc, uint32_t count) {
  RecordCtx cx{};
  cx.genCount = count;
  cx.genColCount = count > 0 ? genColCount_ : 0u;
  RecordTable(enc, pass::Table::GenList, &cx);
  // Streamed-in chunks arrive with fresh terrain that has never settled, and
  // worldgenList marks them dirty. Same self-declaration rule as EncodeWakeAll
  // (§3.4): the function that dirties the set invalidates the latch.
  NoteWakeAll();
}

void Simulation::EncodeFarFill(const rhi::CommandEncoder& enc, uint32_t count) {
  // THE ONE PLACE THE DEFERRED FAR COMPILE IS WAITED ON. A caller with fills
  // to dispatch is a caller that is about to LOOK at the cascades — a --shot
  // frame, a far gate, a load's FullRefill drain — so unless it opted into
  // deferral it blocks here and its output cannot depend on when the driver
  // finished. `count == 0` is the settled case (SubmitTick passes it every
  // tick) and records nothing either way, so it must not wait: making it wait
  // charged every gate in the suite `far`'s 746 s for no dispatch at all.
  if (count == 0) return;
  EnsureFarPipelines();
  // The opted-in caller (the game) whose compile has not landed yet. The
  // recorder would skip the row anyway, but its drain loop has already popped
  // these entries out of FarField's queue, so returning early keeps that a
  // cheap no-op. Simulation::PollFarPipelines -> FullRefill re-queues them.
  // Both halves or neither: the sweep alone would write PRISTINE procgen over
  // cells the player has edited, which is a wrong horizon rather than a
  // missing one (the patch entry is what puts the edits back).
  // The map fill joins them: without it an X/Z plane would leave the surface
  // map holding the OUTGOING face's columns under the incoming one's cells.
  if (!farFill_ || !farMapFill_ || !farPatchFill_) return;
  RecordCtx cx{};
  cx.farCount = count;
  RecordTable(enc, pass::Table::FarFill, &cx);
}

void Simulation::SetTickRain(uint32_t rainWord, int32_t slopeQx, int32_t slopeQz,
                             IVec3 originChunk) {
  // The same integer domains the kernels bound themselves by (rainexpo.h):
  // rainFall's key tiles (8 keys) and the exposure map's texels (4 keys).
  rainlat::Box b;
  for (int a = 0; a < 3; a++) {
    const int32_t o = (a == 0 ? originChunk.x : a == 1 ? originChunk.y : originChunk.z) *
                      (int32_t)kChunk;
    b.lo[a] = o;
    b.hi[a] = o + (int32_t)kWorldN - 1;
  }
  const int32_t nx = rainlat::N(slopeQx), nz = rainlat::N(slopeQz);
  uint64_t tiles = 1;
  for (int i = 0; i < 2; i++) {
    const int a = i == 0 ? 0 : 2;
    const int32_t n = i == 0 ? nx : nz;
    const int32_t d0 = rainlat::Drift(n, b.lo[1]), d1 = rainlat::Drift(n, b.hi[1]);
    const int32_t lo = (b.lo[a] + std::min(d0, d1)) >> 3;
    const int32_t hi = (b.hi[a] + std::max(d0, d1)) >> 3;
    tiles *= (uint64_t)(hi - lo + 1);
  }
  // At most today's thread count: a slant's extra tiles are visited in a
  // window that advances every tick (sim_mutate THE COST IS STILL FIXED).
  constexpr uint64_t kRainThreads = (kWorldN / 8) * (kWorldN / 8);
  rainFallGroups_ = (uint32_t)((std::min(tiles, kRainThreads) + 63) / 64);
  // The map only on ticks the CA can read it: rain now, or wet ground (the
  // RAINDAMP rules' exposure) -- materials.h kRainAmountMask / kRainWetShift.
  const bool wants = (rainWord & 0xFFu) != 0u || ((rainWord >> 16) & 0xFFu) != 0u;
  rainExpoGroups_ = 0;
  if (wants) {
    int32_t lo[2], ext[2];
    rainlat::ExpoDomain(b, nx, nz, lo, ext);
    rainExpoGroups_ = (uint32_t)(((uint64_t)ext[0] * ext[1] + 63) / 64);
  }
}

// The draft box's rebuild verdict for the next EncodeTick. A rebuild re-masks
// every chunk of the box (maskAll) and always solves; otherwise only the
// active chunks are re-masked and the solve runs if one of them changed. The
// verdict must be a pure function of the tick input stream for the field to
// be one: the box moved (its origin rides TickParams), the gate flipped, the
// materials were re-uploaded, or the buffer is new -- each of which a replay
// reproduces at the same tick.
void Simulation::SetDraft(bool on, const int32_t origin[3]) {
  static_assert(kDraftChunks == 2048, "pass_table.def draftMaskAll's literal extent");
  static_assert(kDraftTiles == 256, "sim_draft.wgsl DRAFT_TILES");
  static_assert(kDraftStages == 6 && kDraftMetaArgs == 8,
                "pass_table.def copy_draftArgs (32, 96) and the rows' stage offsets");
  draftOn_ = on;
  const bool moved = origin[0] != draftLastOrigin_[0] || origin[1] != draftLastOrigin_[1] ||
                     origin[2] != draftLastOrigin_[2];
  draftRebuild_ = on && (draftForce_ || moved || !draftLastOn_);
  if (on) {
    for (int i = 0; i < 3; i++) {
      draftOrigin_[i] = origin[i];
      draftLastOrigin_[i] = origin[i];
    }
    if (draftRebuild_) {
      draftForce_ = false;
      draftValid_ = true;   // this tick records a solve for this origin
    }
  } else {
    draftValid_ = false;
  }
  draftLastOn_ = on;
}

void Simulation::EncodeShadowResolve(const rhi::CommandEncoder& enc) {
  // The per-FRAME table carries two systems now: the voxel shadow cache and
  // the clouds. Each has its own row condition, so this records whichever of
  // them is live and returns early only when neither is.
  const sandvox::CloudFrame& cf = sandvox::LastCloudFrame();
  const bool clouds = cf.on && cloudMarch_ && cloudResolve_ && cloudWeather_ &&
                      cloudShadow_ && cloudEnv_ && cloudNoise_;
  // No early return any more: the sky bound's two rows (C_ALWAYS) must run
  // every frame the far cascade is drawn, cache or clouds or neither.
  RecordCtx cx{};
  cx.cloudFlags = (shadowCacheOn_ ? 4u : 0u);
  // The gust streaks: one thread per live slot, nothing at all at alpha 0.
  {
    const Tuning::Wind& tw = CurrentTuning().wind;
    const uint32_t n = (uint32_t)std::clamp(tw.streakCount, 0, (int)kWindStreakCap);
    if (tw.streakAlpha > 0.0f && n > 0 && windStreak_) cx.streakGx = (n + 63) / 64;
  }
  // The ray-start map covers the largest target sized so far; each thread
  // bounds itself against THIS frame's size (ray_start.wgsl).
  if (rayStartW_ > 0 && rayStartH_ > 0 && !rayStartOff_) {
    cx.rayStartGx = ((rayStartW_ + 1) / 2 + 7) / 8;
    cx.rayStartGy = ((rayStartH_ + 1) / 2 + 7) / 8;
  }
  if (clouds) {
    EnsureClouds(cf.lowW, cf.lowH);
    cx.cloudFlags |= 1u;
    if (!cloudBaked_) {
      cx.cloudFlags |= 2u;
      cloudBaked_ = true;
    }
    cx.cloudGx = (cf.lowW + 7) / 8;
    cx.cloudGy = (cf.lowH + 7) / 8;
    // The rain shadow map rides the clouds: it needs the env pass's wind
    // probe, and without clouds nothing draws rain.
    if (cf.rainMap && !rainMapOff_ && rainMapPrep_ && rainMapBuild_)
      cx.cloudFlags |= 8u;
  }
  RecordTable(enc, pass::Table::ShadowCache, &cx);
  // The weather row just recorded: if the uniform asked it to rebuild the map
  // (kClfWeather), that map is now the one later frames reuse.
  if (clouds) sandvox::CommitCloudWeather();
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

void Simulation::EncodeSoluteEvict(const rhi::CommandEncoder& enc, uint32_t count) {
  if (count == 0) return;
  RecordCtx cx{};
  cx.genCount = count;
  RecordTable(enc, pass::Table::SolEvict, &cx);
}

void Simulation::EncodeSoluteRestore(const rhi::CommandEncoder& enc, uint32_t count) {
  if (count == 0) return;
  RecordCtx cx{};
  cx.genCount = count;
  RecordTable(enc, pass::Table::SolRestore, &cx);
}

// ---- THE TEMPERATURE LAYER'S CPU HALF (src/sim/heat.h) --------------------
// The header every tick (it is 32 words), the biome climates every tick (128),
// and the window's column biome table only when what it describes moved: the
// origin, the seed, the map, a climate or a test pin. The table is keyed by
// the WORLD column held in each window column (x & 511, z & 511), so a shift
// recomputes just the new strip on the CPU; the upload is then the whole
// 256 KiB (an x shift touches every row), on a shift tick only.
void Simulation::PrepareHeat(const rhi::Queue& queue, const int32_t origin[3], uint32_t seed) {
  const Tuning& tn = CurrentTuning();
  uint32_t hdr[kHpHdrWords] = {};
  hdr[kHpMode] = tn.sim.heatMode != 0 ? 1u : 0u;
  hdr[kHpRadius] = (uint32_t)std::clamp(tn.sim.heatRadius, 1, (int)kHeatRadiusMax);
  const HeatClimate snow = HeatSnowlineClimate();
  hdr[kHpSnowlineY] = (uint32_t)HeatSnowlineY();
  hdr[kHpSnowBase] = (uint32_t)snow.base;
  hdr[kHpSnowSwing] = (uint32_t)snow.swing;
  hdr[kHpProbe + 0] = (uint32_t)heatProbe_[0];
  hdr[kHpProbe + 1] = (uint32_t)heatProbe_[1];
  hdr[kHpProbe + 2] = (uint32_t)heatProbe_[2];
  hdr[kHpProbeOn] = heatProbeOn_ ? 1u : 0u;
  hdr[kHpGain] = (uint32_t)std::clamp(tn.sim.heatGain, 1, 64);
  queue.WriteBuffer(heatParamsBuf_, 0, hdr, sizeof(hdr));
  uint32_t bio[2 * kHeatBiomesMax];
  uint64_t key = 1469598103934665603ull ^ seed;
  auto mix = [&key](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
  for (uint32_t b = 0; b < kHeatBiomesMax; b++) {
    HeatClimate c = HeatBiomeClimate(b);
    const uint32_t pin = kHeatBiomesMax - 1 - b;
    if (pin < heatPins_.size()) { c.base = heatPins_[pin].base; c.swing = heatPins_[pin].swing; }
    bio[2 * b] = (uint32_t)c.base;
    bio[2 * b + 1] = (uint32_t)c.swing;
    mix(bio[2 * b]);
    mix(bio[2 * b + 1]);
  }
  queue.WriteBuffer(heatParamsBuf_, (uint64_t)kHpBiome * 4, bio, sizeof(bio));
  mix(worldmap::CurrentWorldMap().contentHash);
  for (const HeatColumnPin& p : heatPins_) {
    mix((uint32_t)p.x0); mix((uint32_t)p.z0); mix((uint32_t)p.x1); mix((uint32_t)p.z1);
  }
  const uint32_t n = kWorldN;
  if (!heatColValid_ || key != heatColKey_ || heatCol_.size() != (size_t)n * n) {
    heatCol_.assign((size_t)n * n, 0);
    heatColX_.assign(n, -0x7FFFFFFF);
    heatColZ_.assign(n, -0x7FFFFFFF);
    heatColKey_ = key;
    heatColValid_ = true;
  }
  const int x0 = origin[0] * (int)kChunk, z0 = origin[2] * (int)kChunk;
  auto pinned = [&](int x, int z, uint32_t& b) {
    for (size_t k = 0; k < heatPins_.size(); k++) {
      const HeatColumnPin& p = heatPins_[k];
      if (x >= p.x0 && x <= p.x1 && z >= p.z0 && z <= p.z1) {
        b = kHeatBiomesMax - 1 - (uint32_t)k;
        return true;
      }
    }
    return false;
  };
  auto biomeAt = [&](int x, int z) -> uint8_t {
    uint32_t b = 0;
    if (!pinned(x, z, b)) b = World::MapBiomeAt(x, z, seed);
    return (uint8_t)std::min<uint32_t>(b, kHeatBiomesMax - 1);
  };
  bool changed = false;
  std::vector<int> wantX(n), wantZ(n);
  for (uint32_t k = 0; k < n; k++) {
    // The world column window index k holds: x in [x0, x0 + n) with x & (n-1) == k.
    wantX[k] = x0 + (int)((k - (uint32_t)x0) & (n - 1));
    wantZ[k] = z0 + (int)((k - (uint32_t)z0) & (n - 1));
  }
  for (uint32_t i = 0; i < n; i++) {
    const bool colX = heatColX_[i] != wantX[i];
    for (uint32_t j = 0; j < n; j++) {
      if (!colX && heatColZ_[j] == wantZ[j]) continue;
      heatCol_[(size_t)j * n + i] = biomeAt(wantX[i], wantZ[j]);
      changed = true;
    }
  }
  for (uint32_t k = 0; k < n; k++) { heatColX_[k] = wantX[k]; heatColZ_[k] = wantZ[k]; }
  if (changed)
    queue.WriteBuffer(heatParamsBuf_, (uint64_t)kHpCol * 4, heatCol_.data(), heatCol_.size());
}

void Simulation::EncodeWakeAll(const rhi::Queue& queue) {
  // dirty[page_] is the buffer the NEXT compact pass reads (dirtyIn). One u32
  // flag per chunk; 32768 chunks = 128 KB, far inside the ~1 MB/tick CPU->GPU
  // budget, and only written on a phase boundary.
  static const std::vector<uint32_t> ones(kNumSlots, 1u);
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
  //
  // GAS PARCELS COUNT HERE TOO, and the omission was a real hole rather than a
  // tidiness point. A parcel outside the residency window is a GPU-side dirty
  // writer with no CPU-known target in exactly the sense fact (3) describes:
  // `gasResolve` can land it as a voxel through the claim path and dirty that
  // chunk, on a tick the CPU had proved settled. The landing is not LOST — the
  // gas rows are recorded under their own condition and the mark reaches
  // dirtyOut — but the chunk would then be simulated a tick or two late,
  // whenever the snapshot ring got around to reporting it, and "how late"
  // depends on readback scheduling. That is a scheduling-dependent outcome
  // (rule 1), and it is the same argument that put `particleCount` in this
  // conjunct in the first place.
  //
  // `gasLive_` is the snapshot's parcel count, so it is stale in the same
  // bounded way `particleCount` is, and staleness can only COST a skip.
  if (activeChunks != 0 || particleCount != 0 || gasLive_ != 0) {
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
  // NOTE ON `farDown`: this table's downsample row is a far pipeline, and it
  // is simply SKIPPED while the deferred compile is outstanding (the recorder
  // drops a row whose pipeline is null, touching no buffer state). That is
  // sound and it is not a wait, because the row only refreshes cascade cells
  // under edited chunks — render-only derived data — and every caller that
  // will actually READ a cascade fills it first through EncodeFarFill, which
  // is where the blocking lives. Waiting here instead would have made every
  // gate in the suite pay `far`'s 746 s for a row most of them never use.
  RecordCtx cx{};
  // The rain lattice's extents (SetTickRain, from this tick's TickParams).
  cx.rainFallGroups = rainFallGroups_;
  cx.rainExpoGroups = rainExpoGroups_;
  // Wind drafts (SetDraft): the gate and this tick's rebuild verdict.
  cx.draftOn = draftOn_;
  cx.draftRebuild = draftRebuild_;
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
  // W2: the surface-momentum row's condition (docs/PLAN_water_relevel.md §4.2).
  // Read from tuning HERE rather than passed in, for opennessChunks' reason: it
  // is a KNOB and not a count of this tick's work, so every caller of
  // EncodeTick would otherwise have to forward a value none of them owns. It is
  // ANDed with "a body is listed", because a wave row with no water in it is a
  // dispatch over nothing.
  cx.waveMode = (waterChunkCount > 0 && CurrentTuning().sim.waveMode > 0)
                    ? (uint32_t)CurrentTuning().sim.waveMode
                    : 0u;
  cx.hashEnable = hashEnable;
  cx.particlesActive = particlesActive;
  cx.vizActive = vizActive;
  // The openness grid's rolling refresh budget (docs/PLAN_gi.md §2). Read from
  // tuning HERE rather than passed in, because it is a KNOB and not a count of
  // this tick's work — every caller of EncodeTick would otherwise have to
  // forward a value none of them owns. Zero when the feature is off, which is
  // what leaves both rows unrecorded (C_OPENNESS).
  {
    const Tuning& tn = CurrentTuning();
    cx.opennessChunks =
        tn.render.opennessStrength > 0.0f
            ? (uint32_t)std::min<int>(std::max(tn.render.opennessChunksPerFrame, 0),
                                      (int)kNumChunks)
            : 0u;
    // The glow field's refresh budget (src/sim/world.h kGlowBytes), same shape
    // and same reasoning. It gates ALL THREE glow rows, not just the refresh
    // one: a zero here means the two dirty-walk rows go unrecorded too, so
    // `render.glowStrength = 0` is an exact off switch for the producer AND the
    // consumers' const-folded reads, which is what the `noglow` --render-budget
    // arm needs to measure.
    //
    // max(..., 1) and not max(..., 0): this ONE value carries two meanings —
    // C_GLOW's on/off and the refresh row's extent — so a
    // `glowChunksPerFrame = 0` typed into the tuner would silently turn the
    // whole field off, dirty walk included, which is not what that row says it
    // does. `render.glowStrength` is the off switch and it is the only one.
    cx.glowChunks =
        tn.render.glowStrength > 0.0f
            ? (uint32_t)std::min<int>(
                  std::max(tn.render.glowChunksPerFrame, 1), (int)kNumChunks)
            : 0u;
  }

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
  // gasSpawnsThisTick_ is in the disjunction for windWakeCount's reason: a
  // CPU-authored gas parcel IS a chunk-dirtying input, because it can re-enter
  // the window and land as a voxel. gasLeave conversions are deliberately NOT
  // here and do not need to be — a voxel can only reach the edge on a tick the
  // CA ran, and the CA running already means this world was not proved idle.
  const bool inputsThisTick = opsCount > 0 || expCount > 0 || cellCount > 0 ||
                              spawnCount > 0 || windWakeCount > 0 ||
                              fluidCount > 0 || fluidSpawnCount > 0 ||
                              gasSpawnsThisTick_ > 0;
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

  // ---- C_GAS: a LATCH, not a live count ----------------------------------
  // A gas parcel can only be CREATED on a tick the CA runs (sim_step's
  // gasLeave) or one the CPU queued spawns on, so those two are the arming
  // conditions. The disarming one cannot be either of them: parcels persist
  // for ~111 ticks after the fire that made them goes out, and the only count
  // of them is `gasLive_`, which arrives on the snapshot ring several ticks
  // late. Turning the rows off on a stale zero would freeze a plume in the sky
  // permanently — and it would stay frozen, because the pass that would have
  // stepped it is the one that is not recorded.
  //
  // So: arm on either creator, hold for kGasIdleTicks past the last one (long
  // enough for the ring to speak), and let a snapshot that says parcels are
  // alive keep it armed indefinitely. When the plume really is gone the count
  // reaches zero, the latch runs out, and not one gas row is recorded — which
  // is the rule-2 claim this whole condition exists to make.
  if (cx.caActive || gasSpawnsThisTick_ > 0) {
    gasIdleTicks_ = 0;
  } else if (gasIdleTicks_ < kGasIdleTicks) {
    gasIdleTicks_++;
  }
  // sim.gasMode 0 is an EXACT off switch, not a cheap path: no gas row is
  // recorded at all, so the buffers are never cleared, the passes never
  // dispatch, and the only remaining gas code in the build is a branch in the
  // CA that tests this same value. Read from CurrentTuning() here rather than
  // passed in, for the openness/glow budgets' reason — it is a KNOB, not a
  // count of this tick's work, and every caller of EncodeTick would otherwise
  // have to forward a value none of them owns.
  const bool gasOn = CurrentTuning().sim.gasMode != (int)kGasModeWall;
  cx.gasActive = gasOn && (cx.caActive || gasSpawnsThisTick_ > 0 ||
                           gasLive_ > 0 || gasIdleTicks_ < kGasIdleTicks);
  gasSpawnsThisTick_ = 0;
  // pass_table.def's fill_gasSpawn clears the HEADER only, by a literal byte
  // count (pass_table.cpp does not see world.h). Keep the two in step.
  static_assert(kGasSpHdrBytes == 64,
                "fill_gasSpawn's size (pass_table.def, 0,64,0) is the gasSpawn "
                "header: update the row with the header");

  // ---- C_GASFAR: the frozen fires, which are NOT a parcel population -------
  //
  // A fire the residency window has left behind smokes through a synthesized
  // splat (world.h kGasFarEmitMax), not through the parcel pool — so it gets
  // its own condition and deliberately does NOT arm cx.gasActive. Arming that
  // would record all five parcel rows, and gasSpawnStep alone is a fixed
  // 1,042-workgroup dispatch over both spawn lists whether or not anything is
  // in them. A distant fire burning for an hour would pay that every tick for
  // a population of zero, which is precisely what rule 2 forbids.
  //
  // `render.farPlumeStrength` 0 is an EXACT off switch on the same terms
  // sim.gasMode 0 is: the count goes to zero, the row is not recorded, and the
  // clear falls back to the parcel latch alone.
  cx.gasFarEmitCount =
      CurrentTuning().render.farPlumeStrength > 0.0f ? farPlumeCount_ : 0u;
  // The LONG-RANGE half, on the same terms. Its own count because the two
  // emitter lists are disjoint and cover different distance bands: a world
  // whose only frozen fire is 300 m out records the wide rows and not the fine
  // ones, and vice versa. `render.farPlumeRange` 0 empties the wide list
  // CPU-side, so the knob is an exact off switch here too.
  cx.gasFarWideCount =
      CurrentTuning().render.farPlumeStrength > 0.0f ? farPlumeWideCount_ : 0u;

  // ---- the repose snapshot prepass ----------------------------------------
  // `anyRepose_` is a property of the MATERIAL TABLE, latched in UploadTables,
  // so a world whose materials.json carries no `repose` line never records the
  // row and never touches the 16 MiB snapshot. Folded with caActive because the
  // snapshot exists only to be read by the CA rows behind it: on a tick the CA
  // skips there is nothing to serve.
  cx.reposeActive = anyRepose_ && cx.caActive;

  // ---- THE RENDER FLAG, which is a DIFFERENT question ---------------------
  // cx.gasActive above answers "must the gas PASSES run", and it is true
  // whenever the CA is, because a gas parcel can be created on any tick a
  // voxel reaches a face. That is the right answer for the sim and the wrong
  // one for the renderer: it would put the crossfade's band sampling on every
  // terrain pixel of every frame in any world with a running CA, including
  // every world that has never had a fire in it.
  //
  // So this one asks "is there gas ANYWHERE" -- parcels or in-window voxels --
  // and holds for kGasSeenTicks past the last snapshot that saw either. When
  // the plume is really gone the flag drops and raymarch.wgsl skips the fade,
  // the band and the fold entirely. See renderspec.h.
  //
  // A FROZEN FIRE ARMS IT DIRECTLY, with no hold: unlike a parcel population,
  // the emitter list is a CPU-side fact the CPU already has in hand this tick,
  // so there is nothing latent to cover. If emitters exist the box is being
  // written and must be sampled; when the last one leaves range the flag drops
  // on the same tick the splat stops, which is exactly right — there is no
  // frame on which the box holds a plume the renderer is told to ignore, and
  // none on which it is told to read a box nobody cleared.
  if (gasSeenHold_ > 0) gasSeenHold_--;
  sandvox::SetGasRenderActive((gasOn && gasSeenHold_ > 0) ||
                              cx.gasFarEmitCount > 0);
  // The LONG-RANGE box gets its OWN render flag rather than sharing that one.
  // Sharing would put a 16-sample walk of a second 4 MiB volume on every
  // terrain pixel of every frame in which a campfire smokes ten metres away,
  // which is the whole class of cost RFLAG_GAS exists to avoid -- and it is a
  // correctness gate on the same terms: the long-range box is only cleared on
  // ticks its own row is recorded, so with the flag off it is stale and must
  // not be sampled.
  sandvox::SetGasFarRenderActive(cx.gasFarWideCount > 0);

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
  //
  // THE NO-WATER TICK. Because excite can create particles the CPU cannot see
  // yet, the tables are recorded on every CA-awake tick whether or not any
  // water exists, so what they cost with FA_LIVE == 0 is a standing cost of
  // the CA itself. It is kept near the dispatch floor on the GPU side: no
  // scratch or map Fills (each region is cleared by its last reader), every
  // per-particle / per-block row dispatches off a GPU-written count that is
  // zero, and the fixed-shape rows (alloc, exciteScan, settleJudge,
  // settleScan, mirrorFold) skip their slot walks when nothing is or was live.
  // What remains is exciteDetect over this tick's dirty chunks (real work:
  // it is the excite trigger) plus ~55 dispatches — 36 of them the substep
  // table's four rows x 9 — that are empty, one workgroup, or an early-out.
  const bool exciteOn = CurrentTuning().sim.fluidExciteMode != 0;
  const bool seamActive =
      fluidCount > 0 || fluidSpawnCount > 0 || (exciteOn && cx.caActive);
  fluidSeamRecorded_ = seamActive;
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


void Simulation::BuildRenderBindGroup(rhi::BindGroup& out,
                                      const rhi::Buffer& veil) {
  auto b = [](uint32_t binding, const rhi::Buffer& buf) {
    rhi::BindGroupEntry e{};
    e.binding = binding;
    e.buffer = buf;
    e.size = 0;  // whole buffer
    return e;
  };
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
        b(14, world_->shadowCache),
        b(15, world_->shadowReq),
        b(16, world_->renderStats),
        b(17, world_->openness),
        b(18, world_->opennessGen),
        b(19, world_->irradiance),
        b(20, world_->glow),
        b(21, world_->gasOuter),
        b(22, world_->waterFlux),
        b(23, world_->gasFarOuter),
        b(24, veil),
        b(25, cloudHistBuf_),
        b(26, cloudMapsBuf_),
        b(27, world_->cloudUBO),
        b(28, world_->solTable),
        b(29, world_->solPool),
        b(30, solSpecBuf_),
        b(31, world_->farMap),
        b(32, rayStartBuf_),
        b(34, rainMapBuf_),
        b(33, windStreakBuf_),
        b(35, draftBuf_),
    };
    out = device_.CreateBindGroup(renderBGL_, entries, std::size(entries),
                                  "renderBG");
  }
}

// The veil is per PIXEL, so it follows the largest target a world pass has
// drawn into (kVeilWords x 4 = 28 bytes a pixel: ~58 MB at 1080p). Grow-only, like the depth
// caches are keyed, so alternating a native frame with a smaller --shot or
// portrait never reallocates; a rebuilt renderBG_ is the only side effect.
void Simulation::EnsureVeil(uint32_t width, uint32_t height) {
  // Row pitch is veilBase's, not the target's (renderspec.h LastVeilPitch):
  // they differ under TAA + taaSharpLod, and a buffer of width x height then
  // dropped every row past height * width / pitch.
  const uint64_t pitch =
      std::max<uint64_t>(width, (uint64_t)sandvox::LastVeilPitch() + 1u);
  const uint64_t px = pitch * height;
  if (px <= veilPixels_) return;
  veilPixels_ = px;
  // Must match VEIL_WORDS in common.wgsl: six words of record + the frame
  // stamp that marks it live (w6, which replaced the per-pixel dry clear).
  constexpr uint64_t kVeilWords = 7;
  veilBuf_ = CreateBuffer(device_, px * kVeilWords * 4, rhi::BufferUsage::Storage,
                          "waterVeil");
  BuildRenderBindGroup(renderBG_, veilBuf_);
}

// The ray-start map (ray_start.wgsl): a header plus two half-size planes of
// one f32 per 2x2 pixel block. Grow-only on BOTH axes, like the veil, so a
// smaller --shot or portrait never reallocates and the per-frame rows — which
// are recorded before the render pass that knows the size — always cover the
// target. A rebuilt buffer rebinds the render and shadow groups; its word 0 is
// no frame's key, so the first frame after a resize marches from the camera.
void Simulation::EnsureRayStart(uint32_t width, uint32_t height) {
  if (width <= rayStartW_ && height <= rayStartH_) return;
  rayStartW_ = std::max(rayStartW_, width);
  rayStartH_ = std::max(rayStartH_, height);
  // Must match ray_start.wgsl: RS_HEADER words, then two Wq x Hq planes.
  constexpr uint64_t kHeader = 8;
  const uint64_t q = (uint64_t)((rayStartW_ + 1) / 2) * ((rayStartH_ + 1) / 2);
  // THE OLD BUFFER IS STILL IN THIS COMMAND BUFFER. The ShadowCache table's
  // ray-start rows were recorded (through shadowBG_) before the render pass
  // that called this, and a released handle is destroyed once the LAST
  // SUBMITTED command buffer retires — which is before the one being recorded
  // (--vk-validation: "vkDestroyBuffer ... in use by VkDescriptorSet"). So the
  // replaced buffer is parked until the next growth, which is frames away.
  rayStartPrev_ = rayStartBuf_;
  rayStartBuf_ = CreateBuffer(device_, (kHeader + 2 * q) * 4,
                              rhi::BufferUsage::Storage, "rayStart");
  BuildRenderBindGroup(renderBG_, veilBuf_);
  BuildRenderBindGroup(renderBGNoVeil_, veilNone_);
  BuildShadowBindGroup();
}

// The shadow cache's bind group, which the clouds share (cloud.wgsl binds 3
// and 12..17 of it). Rebuilt by EnsureClouds when the screen-sized pair is
// replaced; the voxel side never changes after Init.
void Simulation::BuildShadowBindGroup() {
  auto b = [](uint32_t binding, const rhi::Buffer& buf) {
    rhi::BindGroupEntry e{};
    e.binding = binding;
    e.buffer = buf;
    e.size = 0;
    return e;
  };
  if (!shadowBGL_ || !cloudRawBuf_) return;
  rhi::BindGroupEntry bges[] = {
      b(0, world_->voxels),
      b(1, world_->occupancy),
      b(2, materialBuf_),
      b(3, world_->renderUBO),
      b(4, world_->pageTable),
      b(5, world_->shadowCache),
      b(6, world_->shadowReq),
      b(7, world_->shadowArgsStage),
      b(8, world_->irradiance),
      b(9, world_->opennessGen),
      b(10, world_->openness),
      b(11, world_->shadowHist),
      b(12, world_->cloudUBO),
      b(13, cloudNoiseBuf_),
      b(14, cloudWeatherBuf_),
      b(15, cloudMapsBuf_),
      b(16, cloudRawBuf_),
      b(17, cloudHistBuf_),
      b(18, world_->farVox),
      b(19, world_->farOcc),
      b(20, world_->farUBO),
      b(21, rayStartBuf_),
      b(23, rainMapBuf_),
      b(22, windStreakBuf_),
      b(24, draftBuf_),
  };
  shadowBG_ = device_.CreateBindGroup(shadowBGL_, bges, std::size(bges), "shadowBG");
}

// Grow-only, like EnsureVeil: a --shot or portrait at a smaller size never
// reallocates. The history is written "clear sky" (c = 0, T = 1) on creation so
// the first composite after a resize draws no phantom cloud from garbage.
void Simulation::EnsureClouds(uint32_t lowW, uint32_t lowH) {
  const uint64_t px = std::max<uint64_t>((uint64_t)lowW * lowH, 1);
  if (px <= cloudPixels_ && cloudRawBuf_ && cloudHistBuf_) return;
  cloudPixels_ = px;
  using U = rhi::BufferUsage;
  cloudRawBuf_ = CreateBuffer(device_, px * kCloudRawWords * 4, U::Storage, "cloudRaw");
  const uint64_t histWords = px * kCloudHistWords * 2;
  cloudHistBuf_ = CreateBuffer(device_, histWords * 4, U::Storage | U::CopyDst, "cloudHist");
  std::vector<uint32_t> init((size_t)histWords, 0u);
  // (r,g) = 0, (b 0, T 1), (Td 1, -): clear sky.
  for (size_t i = 0; i + 2 < init.size(); i += 3) {
    init[i + 1] = 0x3C000000u;
    init[i + 2] = 0x00003C00u;
  }
  device_.GetQueue().WriteBuffer(cloudHistBuf_, 0, init.data(), init.size() * 4);
  BuildShadowBindGroup();
  if (renderBGL_) {
    if (veilBuf_) BuildRenderBindGroup(renderBG_, veilBuf_);
    if (veilNone_) BuildRenderBindGroup(renderBGNoVeil_, veilNone_);
  }
}

void Simulation::EnsureDepth(uint32_t width, uint32_t height) {
  if (depthView_ && depthW_ == width && depthH_ == height) return;
  depthW_ = width;
  depthH_ = height;
  // CopySrc as well as RenderAttachment: the TAA resolve reprojects through the
  // depth this pass wrote, and the only way a shader in this engine reads an
  // image is a copy into a storage buffer (EncodeTaaCapture). It is a usage
  // FLAG on a texture that already exists — no extra memory, no extra pass —
  // so it is unconditional rather than gated on render.taa.
  depthTex_ = device_.CreateTexture(
      {width, height, 1}, kDepthFormat,
      rhi::TextureUsage::RenderAttachment | rhi::TextureUsage::CopySrc, "depth");
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

void Simulation::EnsureOverlayDepth(uint32_t width, uint32_t height) {
  if (overlayDepthView_ && overlayDepthW_ == width && overlayDepthH_ == height)
    return;
  overlayDepthW_ = width;
  overlayDepthH_ = height;
  overlayDepthTex_ =
      device_.CreateTexture({width, height, 1}, kDepthFormat,
                            rhi::TextureUsage::RenderAttachment, "depthOverlay");
  overlayDepthView_ = overlayDepthTex_.CreateView();
}

// ===========================================================================
// TAA + temporal upscale (assets/shaders/taa.wgsl)
// ===========================================================================

namespace {
// The uniform, mirrored by hand against `struct TaaParams` in taa.wgsl.
// std140-ish, vec3+scalar pairs, exactly like RenderParams — and unlike
// RenderParams this one is NOT checked by check_invariants.py, because it is
// not in world.h. Keep the two in the same order and the padding explicit.
struct TaaParamsGpu {
  float camRight[3]; float tanHalfFov;
  float camUp[3];    float aspect;
  float camFwd[3];   float sharpness;
  float pRight[3];   float maxHist;
  float pUp[3];      float clampK;
  float pFwd[3];     float pad0;
  float eyeDelta[3]; float pad1;
  float jitter[2];   float pJitter[2];
  uint32_t renderW, renderH, nativeW, nativeH;
  uint32_t srcPitch, flags, reset, pad2;
};
static_assert(sizeof(TaaParamsGpu) == 160,
              "TaaParamsGpu must match TaaParams in taa.wgsl");
static_assert(sizeof(TaaParamsGpu) % 16 == 0, "uniform must be 16-byte sized");

// Mirrored by hand against `struct DenoiseParams` in denoise.wgsl, same rules.
struct DenoiseParamsGpu {
  uint32_t width, height, pitch, flags;
  int32_t step;
  uint32_t iter, iters, pad0;
  float fpx, pxFull, pxStart, depthTol;
  float chromaTol, strength, pad1, pad2;
};
static_assert(sizeof(DenoiseParamsGpu) == 64,
              "DenoiseParamsGpu must match DenoiseParams in denoise.wgsl");
}  // namespace

// The R2 sequence (Roberts 2018): the 2D generalisation of the golden ratio,
// g = 1.32471795724474602596 (the plastic number), a_i = 1/g^i. It is the
// jitter voxelbit.net uses and the reason ONE sample per pixel per frame
// converges like two white-noise ones — successive frames land maximally far
// from every frame before them, so a short history already covers the pixel
// evenly, where a random offset clumps and leaves gaps.
//
// Range is +/-0.5 of a RENDER pixel: a full pixel of coverage, which is what
// the reconstruction filter in taa.wgsl integrates over. Wider would alias the
// filter; narrower would leave native detail permanently unsampled.
void Simulation::TaaJitter(uint32_t frameIdx, float* jx, float* jy) {
  constexpr double kA1 = 0.7548776662466927;   // 1/g
  constexpr double kA2 = 0.5698402909980532;   // 1/g^2
  const double n = (double)(frameIdx & 1023u);
  auto frac = [](double v) { return v - std::floor(v); };
  if (jx) *jx = (float)(frac(0.5 + kA1 * n) - 0.5);
  if (jy) *jy = (float)(frac(0.5 + kA2 * n) - 0.5);
}

void Simulation::EnsureTaa(uint32_t renderW, uint32_t renderH, uint32_t nativeW,
                           uint32_t nativeH) {
  if (!taaAvailable_) return;
  if (renderW == 0 || renderH == 0 || nativeW == 0 || nativeH == 0) return;
  if (taaRW_ == renderW && taaRH_ == renderH && taaNW_ == nativeW &&
      taaNH_ == nativeH && taaHist_[0] && taaBG_[0])
    return;
  using U = rhi::BufferUsage;
  const bool srcMoved = (taaRW_ != renderW || taaRH_ != renderH);
  const bool dstMoved = (taaNW_ != nativeW || taaNH_ != nativeH);
  taaRW_ = renderW;
  taaRH_ = renderH;
  taaNW_ = nativeW;
  taaNH_ = nativeH;
  if (srcMoved || !taaColor_) {
    // 4 bytes per texel each: one 8:8:8:8 unorm word of colour, one f32 of
    // reversed-Z depth. No row padding — Vulkan's bufferRowLength is in texels
    // and has no 256-byte rule (that is WebGPU's), so the pitch IS the width
    // and the shader's srcPitch is renderW.
    const uint64_t bytes = (uint64_t)renderW * renderH * 4;
    taaColor_ = CreateBuffer(device_, bytes, U::Storage | U::CopyDst, "taaColor");
    taaDepth_ = CreateBuffer(device_, bytes, U::Storage | U::CopyDst, "taaDepth");
  }
  if (dstMoved || !taaHist_[0]) {
    // Two words per native pixel: rgb + accumulated weight, four halves.
    const uint64_t bytes = (uint64_t)nativeW * nativeH * 8;
    taaHist_[0] = CreateBuffer(device_, bytes, U::Storage | U::CopyDst, "taaHistA");
    taaHist_[1] = CreateBuffer(device_, bytes, U::Storage | U::CopyDst, "taaHistB");
  }
  if (!taaUBO_) {
    taaUBO_ = CreateBuffer(device_, sizeof(TaaParamsGpu),
                           U::Uniform | U::CopyDst, "taaUBO");
  }
  auto b = [](uint32_t binding, const rhi::Buffer& buf) {
    rhi::BindGroupEntry e{};
    e.binding = binding;
    e.buffer = buf;
    return e;
  };
  for (int p = 0; p < 2; p++) {
    rhi::BindGroupEntry entries[] = {
        b(0, taaUBO_), b(1, taaColor_), b(2, taaDepth_),
        b(3, taaHist_[1 - p]),   // read the OTHER page
        b(4, taaHist_[p]),       // write this one
    };
    taaBG_[p] = device_.CreateBindGroup(taaBGL_, entries, std::size(entries),
                                        p == 0 ? "taaBG0" : "taaBG1");
  }
  // A resized history is not the same history. Say so rather than resolving one
  // frame of stretched garbage.
  taaReset_ = true;
}

void Simulation::EncodeTaaCapture(const rhi::CommandEncoder& enc,
                                  const rhi::Texture& colorTex) {
  if (!taaAvailable_ || !taaColor_ || !colorTex || !depthTex_) return;
  rhi::TexelCopyTexture src{};
  rhi::TexelCopyBuffer dst{};
  rhi::Extent3D ext{taaRW_, taaRH_, 1};
  src.texture = colorTex;
  dst.buffer = taaColor_;
  dst.bytesPerRow = taaRW_ * 4;
  dst.rowsPerImage = taaRH_;
  enc.CopyTextureToBuffer(src, dst, ext);
  // The DEPTH the same pass wrote, which is the whole reprojection input. The
  // recorder takes the aspect from the image, so a depth image copies through
  // the same call a colour one does; both are 4-byte texels.
  src.texture = depthTex_;
  dst.buffer = taaDepth_;
  enc.CopyTextureToBuffer(src, dst, ext);
}

void Simulation::WriteTaaParams(const rhi::Queue& queue, const TaaCamera& cam,
                                float maxHist, float clampK, bool reset,
                                bool bgraSource) {
  if (!taaAvailable_ || !taaUBO_) return;
  TaaParamsGpu p{};
  for (int i = 0; i < 3; i++) {
    p.camRight[i] = cam.right[i];
    p.camUp[i] = cam.up[i];
    p.camFwd[i] = cam.fwd[i];
  }
  p.tanHalfFov = cam.tanHalfFov;
  p.aspect = cam.aspect;
  // Read here rather than passed in: it is a pure look knob with no call site
  // that would ever want a different value, and routing it through the two
  // callers would only be two more places to forget it.
  p.sharpness = CurrentTuning().render.taaSharpness;
  p.maxHist = maxHist;
  p.clampK = clampK;
  p.jitter[0] = cam.jitterX;
  p.jitter[1] = cam.jitterY;
  const bool haveHistory = taaHavePrev_ && !reset && !taaReset_;
  if (haveHistory) {
    for (int i = 0; i < 3; i++) {
      p.pRight[i] = taaPrevCam_.right[i];
      p.pUp[i] = taaPrevCam_.up[i];
      p.pFwd[i] = taaPrevCam_.fwd[i];
      // curEye - prevEye, differenced in DOUBLE and narrowed once: the eyes are
      // absolute world coordinates and can be tens of thousands of voxels from
      // the origin, where a float subtraction of two nearby positions loses the
      // sub-voxel part that is the entire signal here.
      p.eyeDelta[i] = (float)(cam.eye[i] - taaPrevCam_.eye[i]);
    }
    p.pJitter[0] = taaPrevCam_.jitterX;
    p.pJitter[1] = taaPrevCam_.jitterY;
  }
  p.renderW = taaRW_;
  p.renderH = taaRH_;
  p.nativeW = taaNW_;
  p.nativeH = taaNH_;
  p.srcPitch = taaRW_;
  p.flags = bgraSource ? 1u : 0u;
  p.reset = haveHistory ? 0u : 1u;
  queue.WriteBuffer(taaUBO_, 0, &p, sizeof p);
  taaPrevCam_ = cam;
  taaHavePrev_ = true;
  taaReset_ = false;
}

rhi::RenderPass Simulation::BeginTaaRenderPass(const rhi::CommandEncoder& enc,
                                               const rhi::TextureView& target,
                                               rhi::TextureFormat format,
                                               uint32_t width, uint32_t height) {
  EnsureRenderPipelines(format);
  EnsureOverlayDepth(width, height);

  rhi::RenderPassDesc d{};
  d.label = "taa";
  d.color.view = target;
  // CLEAR, not Load: the resolve writes every pixel of the target, so loading
  // last frame's presented image would be a read nothing consumes.
  d.color.loadOp = rhi::LoadOp::Clear;
  d.color.storeOp = rhi::StoreOp::Store;
  d.hasDepth = true;
  d.depth.view = overlayDepthView_;
  d.depth.loadOp = rhi::LoadOp::Clear;
  d.depth.storeOp = rhi::StoreOp::Discard;
  d.depth.clearValue = 0.0f;
  return enc.BeginRenderPass(d);
}

void Simulation::DrawTaa(const rhi::RenderPass& pass) {
  if (!taaAvailable_ || !taaResolve_ || !taaBG_[taaPage_]) return;
  pass.SetPipeline(taaResolve_);
  pass.SetBindGroup(0, taaBG_[taaPage_]);
  pass.Draw(3);
}

// ===========================================================================
// The shading-LOD filter (assets/shaders/denoise.wgsl)
// ===========================================================================
//
// The chain, per frame, recorded after the world pass has ended and before
// anything consumes the world texture:
//
//   copy   world depth   -> dnDepth_        (transfer write, once)
//   copy   world colour  -> dnColor_        (transfer write)
//   pass 0 reads dnColor_/dnDepth_, renders into the world texture (Clear)
//   copy   world colour  -> dnColor_        (WAR against pass 0's read)
//   pass 1 ... and so on, up to dnIters_.
//
// EVERY barrier in it is derived by the recorder from state it already tracks:
// CopyImageToBuffer routes its destination through TouchExtra (so the second
// copy WARs against the previous pass's fragment read, which
// FlushForRenderDomain recorded), and TransitionImage carries the texture
// attachment-write -> transfer-read -> attachment-write. There is no
// hand-written barrier here and there must not be. The IN-PLACE render is the
// case the TAA gate's 1:1 arm already relies on: the pass reads only the
// buffer the copy landed, never the image it draws into.
void Simulation::EnsureDenoise(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0 || !dnBGL_) return;
  if (dnW_ == width && dnH_ == height && dnBG_[0]) return;
  using U = rhi::BufferUsage;
  dnW_ = width;
  dnH_ = height;
  // 4 bytes per texel each: one 8:8:8:8 unorm word of colour, one f32 of
  // depth. No row padding (Vulkan's bufferRowLength is in texels).
  const uint64_t bytes = (uint64_t)width * height * 4;
  dnColor_ = CreateBuffer(device_, bytes, U::Storage | U::CopyDst, "denoiseColor");
  dnDepth_ = CreateBuffer(device_, bytes, U::Storage | U::CopyDst, "denoiseDepth");
  for (uint32_t i = 0; i < kDenoiseMaxIters; i++) {
    if (!dnUBO_[i])
      dnUBO_[i] = CreateBuffer(device_, sizeof(DenoiseParamsGpu),
                               U::Uniform | U::CopyDst, "denoiseUBO");
  }
  dnDepthTex_ = device_.CreateTexture({width, height, 1}, kDepthFormat,
                                      rhi::TextureUsage::RenderAttachment,
                                      "depthDenoise");
  dnDepthView_ = dnDepthTex_.CreateView();
  auto b = [](uint32_t binding, const rhi::Buffer& buf) {
    rhi::BindGroupEntry e{};
    e.binding = binding;
    e.buffer = buf;
    return e;
  };
  for (uint32_t i = 0; i < kDenoiseMaxIters; i++) {
    rhi::BindGroupEntry entries[] = {b(0, dnUBO_[i]), b(1, dnColor_),
                                     b(2, dnDepth_)};
    dnBG_[i] = device_.CreateBindGroup(dnBGL_, entries, std::size(entries),
                                       "denoiseBG");
  }
}

void Simulation::WriteDenoiseParams(const rhi::Queue& queue, uint32_t width,
                                    uint32_t height, float tanHalfFov,
                                    bool bgraSource) {
  const Tuning::Render& r = CurrentTuning().render;
  dnIters_ = 0;
  if (r.denoise == 0 || !dnUBO_[0] || width == 0 || height == 0) return;
  dnIters_ = (uint32_t)std::clamp(r.denoiseIters, 0, (int)kDenoiseMaxIters);
  for (uint32_t i = 0; i < dnIters_; i++) {
    DenoiseParamsGpu p{};
    p.width = width;
    p.height = height;
    p.pitch = width;
    p.flags = bgraSource ? 1u : 0u;
    p.step = 1 << i;   // a-trous: 1, 2, 4, 8
    p.iter = i;
    p.iters = dnIters_;
    // Pixels per unit of depth at unit distance: a fine voxel at viewZ z
    // (voxels) covers fpx / z pixels.
    p.fpx = (float)height * 0.5f / std::max(tanHalfFov, 1e-4f);
    p.pxFull = r.denoisePxFull;
    // smoothstep's edges must be ordered; LoadTuning clamps but a hot reload
    // can hand over anything.
    p.pxStart = std::max(r.denoisePxStart, r.denoisePxFull + 0.01f);
    p.depthTol = r.denoiseDepthTol;
    p.chromaTol = r.denoiseChromaTol;
    p.strength = r.denoiseStrength;
    queue.WriteBuffer(dnUBO_[i], 0, &p, sizeof p);
  }
}

void Simulation::EncodeDenoise(const rhi::CommandEncoder& enc,
                               const rhi::Texture& tex,
                               const rhi::TextureView& view,
                               rhi::TextureFormat format, uint32_t width,
                               uint32_t height) {
  (void)format;
  if (!denoiseAvailable_ || !dnPipe_ || dnIters_ == 0) return;
  if (!tex || !view || !depthTex_ || !dnBG_[0]) return;
  // The world depth is the one the world pass just wrote; a size mismatch
  // means the caller sized the filter for a different frame.
  if (dnW_ != width || dnH_ != height || depthW_ != width || depthH_ != height)
    return;
  rhi::TexelCopyTexture src{};
  rhi::TexelCopyBuffer dst{};
  rhi::Extent3D ext{width, height, 1};
  dst.bytesPerRow = width * 4;
  dst.rowsPerImage = height;
  src.texture = depthTex_;
  dst.buffer = dnDepth_;
  enc.CopyTextureToBuffer(src, dst, ext);
  for (uint32_t i = 0; i < dnIters_; i++) {
    src.texture = tex;
    dst.buffer = dnColor_;
    enc.CopyTextureToBuffer(src, dst, ext);
    rhi::RenderPassDesc d{};
    d.label = "denoise";
    d.color.view = view;
    // CLEAR, not Load: the pass writes every pixel from the buffer copy.
    d.color.loadOp = rhi::LoadOp::Clear;
    d.color.storeOp = rhi::StoreOp::Store;
    d.hasDepth = true;
    d.depth.view = dnDepthView_;
    d.depth.loadOp = rhi::LoadOp::Clear;
    d.depth.storeOp = rhi::StoreOp::Discard;
    d.depth.clearValue = 0.0f;
    rhi::RenderPass rp = enc.BeginRenderPass(d);
    rp.SetPipeline(dnPipe_);
    rp.SetBindGroup(0, dnBG_[i]);
    rp.Draw(3);
    rp.End();
  }
}

// STILL SERIAL, deliberately (docs/PLAN_shader_compile.md package A audited
// it). The compute build fans out because its 50 calls are independent
// one-liners; this function builds nine pipelines by MUTATING one shared
// RenderPipelineDesc between them (`d.vertexEntry = "vsSprite"` and so on), so
// threading it means first splitting every desc, which is a bigger edit than
// the win: the whole block is 45-52 s against `far`'s 746, and it is not on
// the boot path at all — it runs on the first draw.
void Simulation::EnsureRenderPipelines(rhi::TextureFormat format) {
  if (format == targetFormat_) return;
  targetFormat_ = format;
  // A format change re-creates every pipeline including the variant, so a
  // compile still in flight from the previous format must be joined and
  // dropped first.
  PollRaymarchVariant(/*block=*/true);
  raymarchLean_ = {};
  // Part of the startup timeline (main.cpp StartupMark): the graphics
  // pipelines are created lazily on the first draw, which puts the driver's
  // compile of the raymarch fragment shader INSIDE the first frame.
  const auto tRp0 = std::chrono::steady_clock::now();

  // Fanned out over PipelineBuildPool like BuildPipelines' compute batches —
  // same legality argument (the backend's bookkeeping is mutexed, the shared
  // VkPipelineCache is internally synchronized), same `--shader-stats` serial
  // fallback. This block was left serial when `far` alone was 639 s and these
  // nine were 45-52 s; with the SPIR-V optimizer landed the compute block is
  // ~60 s total and a serial render block measured 64 s wall on a cold cache —
  // it had become HALF the time to first frame. The descs are built up front
  // as plain values (not one desc mutated between creates, which is what kept
  // this serial), and every state object a desc points into — `blend` below —
  // lives in this scope until the pool joins.
  PipelineBuildPool pool;
  const unsigned buildThreads =
      rhi::vkr::CaptureStats(device_) ? 1u : kBuildThreads;

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
    pool.Add([this, d] { raymarch_ = device_.CreateRenderPipeline(d); });
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
    pool.Add([this, d] { particleDraw_ = device_.CreateRenderPipeline(d); });

    d.vertexEntry = "vsSprite";
    d.label = "spriteDraw";
    pool.Add([this, d] { spriteDraw_ = device_.CreateRenderPipeline(d); });

    // THE ONE PIPELINE HERE WITH ITS OWN FRAGMENT ENTRY. Rigidbodies shade in
    // fsBody, not fs, because they are the only raster path that casts a sun
    // shadow — and `voxels`/`pageTable` are Fragment-only in renderBGL_ above,
    // so the ray cannot be cast from a vertex shader. See the BodyVSOut note in
    // debris.wgsl for why the rest of the raster paths stay per-vertex.
    d.vertexEntry = "vsBody";
    d.fragmentEntry = "fsBody";
    d.label = "bodyDraw";
    pool.Add([this, d] { bodyDraw_ = device_.CreateRenderPipeline(d); });
    // Its depth pre-pass (debris.wgsl vsBodyDepth): same layout, same depth
    // state (write, GreaterEqual), and a (Zero, One) blend so the colour target
    // is untouched — the RHI has no colour write mask, and a fragment stage is
    // required. DrawBodies draws through this first so fsBody's shadow ray
    // runs once per pixel instead of once per overdrawn fragment.
    {
      rhi::RenderPipelineDesc dz = d;
      dz.vertexEntry = "vsBodyDepth";
      dz.fragmentEntry = "fsBodyDepth";
      dz.label = "bodyDepth";
      static const rhi::BlendState keepColor = [] {
        rhi::BlendState b{};
        b.color.srcFactor = rhi::BlendFactor::Zero;
        b.color.dstFactor = rhi::BlendFactor::One;
        b.alpha.srcFactor = rhi::BlendFactor::Zero;
        b.alpha.dstFactor = rhi::BlendFactor::One;
        return b;
      }();
      dz.blend = &keepColor;
      pool.Add([this, dz] { bodyDepth_ = device_.CreateRenderPipeline(dz); });
    }
    d.fragmentEntry = "fs";  // restore for the pipelines that follow

    // MLS-MPM fluid prototype: same module, same layout, own entry point.
    // Opaque cubes for now — translucency across thousands of unsorted cubes
    // is a z-fighting mess, and the comparison the prototype exists for reads
    // fine with solid water-coloured droplets.
    d.vertexEntry = "vsFluid";
    d.label = "fluidDraw";
    pool.Add([this, d] { fluidDraw_ = device_.CreateRenderPipeline(d); });
  }
  // NOTE: `blend` (pointed into by the debug descs below) must outlive
  // pool.Run — it is declared at function scope, not inside the block, for
  // exactly that reason.
  rhi::BlendState blend{};
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
    pool.Add([this, d] { debugBoxDraw_ = device_.CreateRenderPipeline(d); });

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
    pool.Add([this, d] { debugWindDraw_ = device_.CreateRenderPipeline(d); });

    // The GUST STREAKS' ribbons (wind_streak.wgsl). The arrows' depth rule —
    // hidden by the world in front, writing nothing — and the same straight
    // alpha: a streak is a faint white wisp over the scene, not geometry.
    d.label = "windStreakDraw";
    d.vertexModule = windStreakModule_;
    d.vertexEntry = "vsStreak";
    d.fragmentModule = windStreakModule_;
    d.fragmentEntry = "fsStreak";
    d.depth = dsWind;
    pool.Add([this, d] { windStreakDraw_ = device_.CreateRenderPipeline(d); });

    // The CURRENT field's arrows (water plan component 8). Same pipeline
    // state, same depth rule, same argument for it — a different field.
    d.label = "debugCurrentDraw";
    d.vertexModule = debugCurModule_;
    d.vertexEntry = "vsCurArrow";
    d.fragmentModule = debugCurModule_;
    d.fragmentEntry = "fsCurArrow";
    d.depth = dsWind;
    pool.Add([this, d] { debugCurrentDraw_ = device_.CreateRenderPipeline(d); });

    // The vessel's pour-point sphere (DrawPourMarker). The wireframes' module
    // and blend, the arrows' depth rule: a marker in the world is hidden by
    // the world in front of it, and writes nothing so it never occludes.
    d.label = "pourMarkerDraw";
    d.vertexModule = debugLineModule_;
    d.vertexEntry = "vsSphere";
    d.fragmentModule = debugLineModule_;
    d.fragmentEntry = "fsSphere";
    d.depth = dsWind;
    pool.Add([this, d] { pourMarkerDraw_ = device_.CreateRenderPipeline(d); });
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
    pool.Add([this, d] { microBodyDraw_ = device_.CreateRenderPipeline(d); });
  }
  // The TAA resolve. It carries a depth ATTACHMENT it never uses, and shares
  // the overlay pass's native-size depth cache to do it: this pass runs
  // immediately before the UI pass, at the same size, so reusing that cache
  // costs one clear and no fourth texture — and it keeps every render pipeline
  // in this function built against the same depth format, which is the
  // arrangement dynamic rendering is least surprising under.
  if (taaModule_) {
    rhi::DepthState dsIgnore{};
    dsIgnore.format = kDepthFormat;
    dsIgnore.depthWriteEnabled = false;
    dsIgnore.depthCompare = rhi::CompareFunction::Always;

    rhi::RenderPipelineDesc d{};
    d.label = "taaResolve";
    d.layout = taaPL_;
    d.vertexModule = taaModule_;
    d.vertexEntry = "vs";
    d.fragmentModule = taaModule_;
    d.fragmentEntry = "fs";
    d.colorFormat = format;
    d.topology = rhi::PrimitiveTopology::TriangleList;
    d.cullMode = rhi::CullMode::None;
    d.depth = dsIgnore;
    pool.Add([this, d] { taaResolve_ = device_.CreateRenderPipeline(d); });
  }
  // The shading-LOD filter: a fullscreen pass at RENDER resolution, in place
  // over the world frame. Its depth attachment is its own (EnsureDenoise) —
  // the pass runs at render size, which is neither the world depth (still
  // holding the depth the filter reads) nor the overlay's native size.
  if (dnModule_) {
    rhi::DepthState dsIgnore{};
    dsIgnore.format = kDepthFormat;
    dsIgnore.depthWriteEnabled = false;
    dsIgnore.depthCompare = rhi::CompareFunction::Always;

    rhi::RenderPipelineDesc d{};
    d.label = "denoise";
    d.layout = dnPL_;
    d.vertexModule = dnModule_;
    d.vertexEntry = "vs";
    d.fragmentModule = dnModule_;
    d.fragmentEntry = "fs";
    d.colorFormat = format;
    d.topology = rhi::PrimitiveTopology::TriangleList;
    d.cullMode = rhi::CullMode::None;
    d.depth = dsIgnore;
    pool.Add([this, d] { dnPipe_ = device_.CreateRenderPipeline(d); });
  }
  pool.Run(buildThreads);

  // ---- deferred: the SPECIALIZED raymarch (W2-A) --------------------------
  // The same argument the far cascades' deferral runs on, one level smaller.
  // This is a SECOND full compile of the biggest fragment shader in the engine
  // — the block above is 45-64 s cold and the raymarch dominates it — and
  // nothing needs it to be finished: a frame whose predicates say "lean" draws
  // the universal pipeline instead, which is the same picture. So it must not
  // be on the path to the first frame, and it is not; it lands a few tens of
  // seconds later on a cold cache and instantly on a warm one, because the
  // SPIR-V disk cache keys on source and only this variant's key is new.
  //
  // Captured BY VALUE (device, layout, module, format, depth state) exactly as
  // the far block is, so the thread names no Simulation member. Serial under
  // --shader-stats for that mode's own reason: it exists to interrogate every
  // pipeline the driver compiled this run, and this is one of them.
  if (raymarchLeanModule_) {
    rhi::RenderPipelineDesc dl{};
    dl.label = "raymarchLean";
    dl.layout = renderPL_;
    dl.vertexModule = raymarchLeanModule_;
    dl.vertexEntry = "vs";
    dl.fragmentModule = raymarchLeanModule_;
    dl.fragmentEntry = "fs";
    dl.colorFormat = format;
    dl.topology = rhi::PrimitiveTopology::TriangleList;
    dl.depth = dsAlways;
    const rhi::Device dev = device_;
    auto build = [dev, dl]() {
      rhi::RenderPipeline p = dev.CreateRenderPipeline(dl);
      // Persist from this thread the moment it exists, for the reason the two
      // saves in BuildPipelines exist: the save at the bottom of this function
      // ran long before this compile finished, so without this the variant's
      // ISA is recompiled on every launch.
      rhi::vkr::SavePipelineCache(dev);
      return p;
    };
    if (buildThreads <= 1) {
      raymarchLean_ = build();
      rayLeanPublished_ = true;
    } else {
      rayLeanFuture_ = std::async(std::launch::async, build);
      rayLeanPublished_ = false;
    }
  }

  // Decided after the join, from the same two inputs the shadow cache's flag is:
  // the device must allow a fragment shader to write storage (the history IS a
  // fragment-stage write), and the pipeline must actually have compiled. A
  // machine that fails either renders the plain NEAREST blit and says nothing.
  taaAvailable_ = FragmentStoresAvailable() && (bool)taaResolve_;
  // No fragment-store requirement: the filter only reads storage and writes
  // its attachment. A shader that failed to compile leaves it off.
  denoiseAvailable_ = (bool)dnPipe_;
  std::fprintf(stderr, "[startup] render pipelines built in %.2f s\n",
               std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                             tRp0).count());
  // Persist the driver's pipeline cache RIGHT HERE, not only at shutdown: this
  // is the 45-52 s compile (vk::Backend::SavePipelineCache has the numbers),
  // and the processes that pay it are the ones build.sh taskkills before any
  // shutdown path runs. Also covers the F5 rebuild, which lands here too.
  rhi::vkr::SavePipelineCache(device_);
}

rhi::RenderPass Simulation::BeginRenderPass(const rhi::CommandEncoder& enc,
                                            const rhi::TextureView& target,
                                            rhi::TextureFormat format,
                                            uint32_t width, uint32_t height) {
  EnsureRenderPipelines(format);
  EnsureDepth(width, height);
  EnsureVeil(width, height);
  EnsureRayStart(width, height);
  veilLive_ = false;  // until this pass's DrawWorld writes it
  veilSplitPending_ = false;

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
  EnsureVeil(width, height);
  EnsureRayStart(width, height);
  veilLive_ = false;  // until this pass's DrawWorld writes it
  veilSplitPending_ = false;

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

rhi::RenderPass Simulation::BeginOverlayRenderPass(const rhi::CommandEncoder& enc,
                                                   const rhi::TextureView& target,
                                                   rhi::TextureFormat format,
                                                   uint32_t width, uint32_t height) {
  EnsureRenderPipelines(format);
  EnsureOverlayDepth(width, height);
  veilLive_ = false;  // until this pass's DrawWorld writes it
  veilSplitPending_ = false;

  rhi::RenderPassDesc d{};
  d.label = "overlay";
  d.color.view = target;
  d.color.loadOp = rhi::LoadOp::Load;  // the blitted world frame
  d.color.storeOp = rhi::StoreOp::Store;
  d.hasDepth = true;
  d.depth.view = overlayDepthView_;
  d.depth.loadOp = rhi::LoadOp::Clear;
  d.depth.storeOp = rhi::StoreOp::Discard;
  d.depth.clearValue = 0.0f;
  return enc.BeginRenderPass(d);
}

void Simulation::DrawWorld(const rhi::RenderPass& pass) {
  // ---- pick this frame's raymarch pipeline (W2-A) --------------------------
  // `LastRenderSpec()` is the flag word and fluidCount WriteRenderParams
  // uploaded for THIS frame, not a prediction of them (support.h), so "every
  // specialized branch is off" is a fact about the uniform the shader is about
  // to read. When it holds, the lean variant draws the identical picture with
  // ~24% less compiled shader; when it does not, or the variant is not
  // published, or --render-budget's `nospec` arm forced it, the universal
  // pipeline draws — which is always correct.
  bool lean = !forceUniversalRay_ && sandvox::LastRenderSpec().AllOff() &&
              (bool)raymarchLeanModule_;
  if (lean) {
    // Blocks only for a caller that did not opt into deferral, i.e. never for
    // the interactive game and always for a checked output.
    PollRaymarchVariant(/*block=*/!deferRayVariantOk_);
    lean = (bool)raymarchLean_;
  }
  pass.SetPipeline(lean ? raymarchLean_ : raymarch_);
  pass.SetBindGroup(0, renderBG_);
  pass.SetBindGroup(1, renderPartBG_[page_]);
  pass.Draw(3);
  // THE WATER VEIL (common.wgsl): the raymarch just wrote each pixel's record
  // from its fragment stage, and every raster draw after this reads it. No
  // barrier is legal inside a rendering scope, so the scope is split here —
  // in the one function every world pass calls — rather than at each of the
  // dozens of call sites that draw bodies after the world.
  //
  // WATER_VEIL (raymarch.wgsl) is SHADOW_CACHE_AVAILABLE: without fragment
  // stores the raymarch writes no record, so the body passes must not read
  // one — they keep renderBGNoVeil_, and the raymarch keeps its depth at the
  // liquid interface, which is the old contract whole.
  //
  // The split itself is LAZY (VeilReaderBG): only the first draw that reads
  // the veil takes it, so a pass that draws nothing after the world pays none.
  if (veilPixels_ > 0 && FragmentStoresAvailable()) veilSplitPending_ = true;
}

const rhi::BindGroup& Simulation::VeilReaderBG(const rhi::RenderPass& pass) {
  if (veilSplitPending_) {
    pass.SplitAfterFragmentWrite(veilBuf_);
    veilLive_ = true;
    veilSplitPending_ = false;
  }
  return BodyRenderBG();
}

void Simulation::DrawParticles(const rhi::RenderPass& pass) {
  pass.SetPipeline(particleDraw_);
  pass.SetBindGroup(0, VeilReaderBG(pass));
  pass.SetBindGroup(1, renderPartBG_[page_]);
  pass.DrawIndirect(world_->drawArgs, 0);
}

void Simulation::DrawSprites(const rhi::RenderPass& pass, uint32_t count) {
  if (count == 0) return;
  pass.SetPipeline(spriteDraw_);
  pass.SetBindGroup(0, VeilReaderBG(pass));
  pass.SetBindGroup(1, renderPartBG_[page_]);
  pass.Draw(36, count);
}

void Simulation::DrawFluid(const rhi::RenderPass& pass, uint32_t count) {
  if (count == 0) return;   // no fluid placed: costs nothing
  pass.SetPipeline(fluidDraw_);
  pass.SetBindGroup(0, VeilReaderBG(pass));
  pass.SetBindGroup(1, renderPartBG_[page_]);
  pass.Draw(36, count);
}

void Simulation::DrawDebugBoxes(const rhi::RenderPass& pass,
                               uint32_t count) {
  if (count == 0) return;   // overlay off: costs nothing
  pass.SetPipeline(debugBoxDraw_);
  pass.SetBindGroup(0, BodyRenderBG());
  pass.SetBindGroup(1, renderPartBG_[page_]);
  // 12 edges x 6 vertices (two triangles per edge quad).
  pass.Draw(72, count);
}

void Simulation::DrawPourMarker(const rhi::RenderPass& pass, uint32_t at) {
  if (at == UINT32_MAX) return;   // no vessel up: not even a bind
  pass.SetPipeline(pourMarkerDraw_);
  pass.SetBindGroup(0, BodyRenderBG());
  pass.SetBindGroup(1, renderPartBG_[page_]);
  // One camera-facing quad; the fragment shader cuts the disc out of it.
  pass.Draw(6, 1, 0, at);
}

void Simulation::DrawWindField(const rhi::RenderPass& pass, uint32_t arrows) {
  if (arrows == 0) return;   // overlay off: costs nothing, not even a bind
  pass.SetPipeline(debugWindDraw_);
  pass.SetBindGroup(0, BodyRenderBG());
  pass.SetBindGroup(1, renderPartBG_[page_]);
  // 3 segments (shaft + two head barbs) x 6 vertices (two triangles per
  // segment quad). No vertex or instance buffer: the shader derives its
  // lattice point from the instance index and R.camPos.
  pass.Draw(18, arrows);
}

void Simulation::ClearWindStreaks() {
  if (!windStreakBuf_) return;
  const size_t words = (1u + (size_t)kWindStreakCap * kWindStreakStride) * 4u;
  std::vector<uint32_t> zero(words, 0u);
  device_.GetQueue().WriteBuffer(windStreakBuf_, 0, zero.data(), zero.size() * 4);
}

void Simulation::DrawWindStreaks(const rhi::RenderPass& pass, uint32_t count,
                                 uint32_t trail) {
  if (count == 0 || trail < 2 || !windStreakDraw_) return;   // off: not even a bind
  pass.SetPipeline(windStreakDraw_);
  pass.SetBindGroup(0, BodyRenderBG());
  pass.SetBindGroup(1, renderPartBG_[page_]);
  // (trail - 1) ribbon segments x 6 vertices, one instance per pool slot. No
  // vertex buffer: the shader reads the pool at renderBGL_ 33.
  pass.Draw(6 * (trail - 1), count);
}

void Simulation::DrawCurrentField(const rhi::RenderPass& pass,
                                  uint32_t arrows) {
  if (arrows == 0) return;   // overlay off: costs nothing, not even a bind
  pass.SetPipeline(debugCurrentDraw_);
  pass.SetBindGroup(0, BodyRenderBG());
  pass.SetBindGroup(1, renderPartBG_[page_]);
  pass.Draw(18, arrows);
}

void Simulation::DrawBodies(const rhi::RenderPass& pass, uint32_t voxInstances) {
  if (voxInstances == 0) return;
  pass.SetBindGroup(0, VeilReaderBG(pass));
  pass.SetBindGroup(1, renderPartBG_[page_]);
  // Depth first, then colour: the second draw's fragments pass GreaterEqual
  // only where they are the nearest body surface, so fsBody's per-fragment
  // shadow ray is cast once per pixel (debris.wgsl, THE DEPTH PRE-PASS).
  //
  // 18 VERTICES, NOT 36 (2026-09-28): a cube seen from outside shows at most
  // three faces, one per axis, and debris.wgsl's bodyVertex picks each axis's
  // camera-facing sign itself — so the back three faces are never generated
  // rather than generated and collapsed.
  pass.SetPipeline(bodyDepth_);
  pass.Draw(kBodyCubeVerts, voxInstances);
  pass.SetPipeline(bodyDraw_);
  pass.Draw(kBodyCubeVerts, voxInstances);
}

void Simulation::DrawBodyRanges(
    const rhi::RenderPass& pass,
    const std::vector<std::pair<uint32_t, uint32_t>>& draws) {
  // Per-body (culled) draws of the same buffer: [firstInstance, count] pairs
  // from CullBodyRanges. `instance_index` in WGSL INCLUDES firstInstance, so
  // bodyInst[inst] and the ember-flicker key read exactly what the whole-list
  // draw would have for the same instance.
  if (draws.empty()) return;
  pass.SetBindGroup(0, VeilReaderBG(pass));
  pass.SetBindGroup(1, renderPartBG_[page_]);
  pass.SetPipeline(bodyDepth_);
  for (const auto& d : draws) pass.Draw(kBodyCubeVerts, d.second, 0, d.first);
  pass.SetPipeline(bodyDraw_);
  for (const auto& d : draws) pass.Draw(kBodyCubeVerts, d.second, 0, d.first);
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
  pass.SetBindGroup(0, VeilReaderBG(pass));
  pass.SetBindGroup(1, microBodyBG_);
  pass.Draw(36, count);
}

void Simulation::FlipPage() { page_ = 1 - page_; }
