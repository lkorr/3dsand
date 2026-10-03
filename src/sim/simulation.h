#pragma once
#include <atomic>
#include <future>
#include <string>
#include <vector>

#include "gpu/passtimer.h"
#include "sim/materials.h"
#include "sim/microbody.h"
#include "sim/microvox.h"
#include "sim/pass_table.h"
#include "sim/solutes.h"
#include "sim/treeatlas.h"
#include "sim/worldmap.h"
#include "sim/world.h"

// Owns the compute pipelines + bind groups and records the fixed-tick GPU
// work: mutate -> explosions -> 27 color passes -> particles -> occupancy/hash
// -> pick. Also owns the render pipelines (raymarch + instanced-cube raster
// for particles/sprites) — they read the same buffers, composited through a
// shared reversed-Z depth attachment.
class Simulation {
 public:
  bool Init(const rhi::Device& device, World& world,
            const std::vector<MaterialDef>& mats,
            const std::vector<ReactionGpu>& reactions, const MicroSet& micro,
            const TreeAtlas& trees, const std::vector<uint32_t>& worldMapWords,
            const std::string& shaderDir);

  // Recompile all WGSL from disk; returns false (keeping old pipelines) on
  // compile error.
  bool ReloadShaders(const rhi::Device& device);
  // Re-upload the material + reaction tables (JSON hot reload).
  void UploadTables(const rhi::Queue& queue, const std::vector<MaterialDef>& mats,
                    const std::vector<ReactionGpu>& reactions);
  // The solute species table the GPU holds (solutes.json resolved against the
  // materials of the last UploadTables). Empty = nothing dissolves.
  const std::vector<SoluteDef>& Solutes() const { return solutes_; }
  // Pack `defs` into the kSolSpec* layout and upload it. UploadTables calls
  // this with LoadSolutes(solutes.json); a gate may call it directly.
  void UploadSolutes(const rhi::Queue& queue, const std::vector<SoluteDef>& defs,
                     const std::vector<MaterialDef>& mats);
  // Re-upload the static micro-detail brick pool + per-material table (rides
  // the same R hot-reload as materials — sim/microvox.h). Render-only data:
  // these buffers are bound to the raymarch pipeline and to nothing else.
  void UploadMicro(const rhi::Queue& queue, const MicroSet& micro);
  // Re-upload the dynamic micro-BODY model table + brick pool (mob defs load /
  // hot reload — sim/microbody.h). Render-only, same doctrine as UploadMicro.
  // Non-const: the set carries its own dirty-range bookkeeping and this is the
  // only thing that may clear it, so "uploaded" and "no longer dirty" cannot
  // drift apart at a call site that forgot the second half.
  void UploadMicroBodies(const rhi::Queue& queue, MicroBodySet& set);
  // Brick-pool bytes UploadMicroBodies has sent, whole run (--frames report).
  uint64_t MicroPoolBytesSent() const { return mbPoolBytesSent_; }
  // Publish the ART palette: per-voxel skin colours from loaded prefabs, which
  // are NOT material colours (a creature is one material all over and painted
  // per voxel — sim/voxload.h). They live in reserved material-table entries
  // (kArtPaletteBaseGpu, world.h), so they are CACHED here and re-applied by
  // UploadTables: a materials hot-reload rewrites the whole table and would
  // otherwise wipe them, repainting every mob in its raw material colours.
  // Render-only; nothing here can reach a world cell or the hash.
  void SetArtPalette(const rhi::Queue& queue, const std::vector<uint32_t>& rgb);
  // Re-upload the two AUTHORED-ENVIRONMENT tables after a reload
  // (docs/PLAN_environment_truth.md P-A): the tree atlas (binding 26) and the
  // world map -- biome records, cover rows, planes, sites (binding 31). Either
  // may have GROWN since Init (a new species, a bigger map, a new biome); then
  // the buffer is recreated and the two sim bind groups that hold it are
  // rebuilt, which is why the caller must have drained the GPU
  // (ctx.WaitIdle()) first. Data only: no pipeline is touched, and the next
  // worldgen reads the new tables the same way the first one read the old.
  void UploadEnvironment(const rhi::Device& device, const rhi::Queue& queue,
                         const TreeAtlas& trees, const std::vector<uint32_t>& worldMapWords);

  // A dense (denseGen = true) worldgen reads the column cache like `list`
  // does, so the caller runs WriteGenList over every slot first
  // (WriteDenseGenList) with the window origin it put in tickUBO.
  void EncodeWorldgen(const rhi::CommandEncoder& enc, bool denseGen = true);
  // THE GEN LIST UPLOAD: writes `slots` to world.genList AND builds the column
  // cache's input for them (genCols: each slot's chunk-column and the
  // chunk-columns' y-ranges, worldgen.wgsl's column-cache block) against the
  // CURRENT window origin -- the origin the caller writes into tickUBO for
  // the same dispatch. Every EncodeGenList must be preceded by one of these
  // for the same list; it remembers the chunk-column count the `cols`
  // pre-pass dispatches. Returns that count.
  uint32_t WriteGenList(const rhi::Queue& queue, const std::vector<uint32_t>& slots);
  // WriteGenList over slots 0 .. kNumSlots-1, for the dense `main` dispatch
  // (a slot is its own list position there).
  void WriteDenseGenList(const rhi::Queue& queue);
  // Generate `count` streamed-in chunks whose SLOT indices WriteGenList wrote
  // (and whose count + window origin are in tickUBO).
  void EncodeGenList(const rhi::CommandEncoder& enc, uint32_t count);
  // Post-load reset: clears transient state (hash/particles/claims) and
  // rebuilds occupancy over freshly uploaded voxels. Caller has already
  // written the voxel + dirty buffers (see worldio.cpp).
  void EncodeLoadReset(const rhi::CommandEncoder& enc);
  // Fill `count` far-field cascade level-chunks whose packed entries the
  // caller wrote to world.farList (count also in tickUBO.farCount). Render-
  // only derived data — safe to encode anywhere in the tick (DESIGN.md §9).
  void EncodeFarFill(const rhi::CommandEncoder& enc, uint32_t count);
  // JITTER page materialization: `count` (slot, entry) pairs already in
  // genList. Recorded at the HEAD of the tick's command buffer by
  // PageTable::DrainFills, alongside the one-pattern fills.
  void EncodePageFill(const rhi::CommandEncoder& enc, uint32_t count);

  // ---- the voxel-keyed shadow cache (world.h kShadowCacheBuckets) ----------
  // Resolves the patches last frame's raymarch asked for, so DrawWorld can read
  // their shadow factors instead of casting a ray per lit pixel.
  //
  // CALL IT ONCE PER FRAME, ON THE SAME ENCODER, BEFORE BeginRenderPass. It
  // cannot live inside DrawWorld: that takes a RenderPass and a render pass
  // cannot host a compute dispatch. It also must not be skipped on a frame that
  // draws — the fragment shader registers into a list this pass is the only
  // consumer of, so a draw without a preceding resolve leaves every patch stale
  // for a frame and lets the request list fill to its cap.
  //
  // No-ops when the cache is compiled out (render.shadowCache = 0 or the device
  // lacks fragmentStoresAndAtomics), which is the ONE place that is decided.
  void EncodeShadowResolve(const rhi::CommandEncoder& enc);
  bool ShadowCacheOn() const { return shadowCacheOn_; }
  // The GI gather request list (gi_gather.wgsl), for --render-budget's stats
  // read of its header words. Diagnostics only, never the frame path.
  const rhi::Buffer& GiReqBuffer() const { return giReqBuf_; }
  // Standalone whole-world hash pass (save/load verification): caller writes
  // TickParams with hashEnable=1 first, reads world.hash after submit.
  void EncodeHashOnly(const rhi::CommandEncoder& enc);
  // The solute layer's two between-tick doors (sim_solute.wgsl solEvict /
  // solRestore): `count` slots listed in solMeta[kSolMEvictList..], or `count`
  // records in solStage. Each wants its own command buffer, submitted while no
  // tick is in flight (Stream::ShiftAxis / FillSlots, LoadWorld).
  void EncodeSoluteEvict(const rhi::CommandEncoder& enc, uint32_t count);
  void EncodeSoluteRestore(const rhi::CommandEncoder& enc, uint32_t count);

  // Wake every chunk for the NEXT tick by setting all dirty-in flags.
  //
  // Needed because the daylight-gated reactions deliberately do NOT keep a
  // chunk awake while their condition is unmet (otherwise a lit pond would
  // spin forever at night and the settled world would never sleep — rule 2).
  // The cost is paid only when the day phase crosses a gate boundary
  // (sunrise/sunset), which is a handful of ticks per in-game day, and the
  // woken chunks that have nothing to do go straight back to sleep on the
  // following tick.
  //
  // Determinism: the caller must trigger this from the tick-derived day phase
  // ONLY (see main.cpp), never from frame timing — a wake that happens on a
  // different tick on another machine changes when reactions fire.
  void EncodeWakeAll(const rhi::Queue& queue);

  // THE TEMPERATURE LAYER'S CPU HALF (src/sim/heat.h). Writes the heatParams
  // header (mode, radius, snowline, the F1 probe cell) every tick, and the
  // window's column biome table whenever the origin, seed or map moved since
  // the last write -- a pure function of (map, seed, window), so a replay
  // rebuilds the same table. Called by SubmitTick right after the TickParams
  // upload; deferred queue writes, so it lands before the tick's commands.
  //
  // LIVE KNOBS: returns true on a tick where a knob word of the header (mode,
  // radius, snowline, gain, the direction gains) differs from the last
  // upload. It then bumps the knob epoch (heat.h kHpKnobEpoch), so heatBegin
  // re-targets every live page, and the CALLER must wake the world this tick
  // (EncodeWakeAll, the day-flip wake): a settled neighbourhood would
  // otherwise neither run the heat rows nor be allowed to mark a chunk whose
  // new target crosses a melt / ignite / freeze threshold. The first call
  // after construction, a worldgen or a load only records the knobs.
  bool PrepareHeat(const rhi::Queue& queue, const int32_t origin[3], uint32_t seed);
  // The cell the F1 readout asks about (the player's feet); heatRelax copies
  // that block's X / X* / sources into heatMeta's probe words. Render-side
  // input: it changes no heat value, only which one is reported.
  void SetHeatProbe(bool on, int x, int y, int z) {
    heatProbeOn_ = on; heatProbe_[0] = x; heatProbe_[1] = y; heatProbe_[2] = z;
  }
  // Forget the column table (a map or biome reload): the next PrepareHeat
  // rewrites all of it.
  void InvalidateHeatColumns() { heatColValid_ = false; }
  // TEST ONLY: pin the CLIMATE of a rectangle of world columns (inclusive) --
  // how a heat gate puts a frozen and a hot climate side by side in one
  // sealed fixture, whatever biome the map has there. Pin k borrows climate
  // slot kHeatBiomesMax - 1 - k (no map has that many biomes). Empty = the
  // map's alone.
  struct HeatColumnPin { int x0, z0, x1, z1; int base, swing; };
  void SetHeatColumnPins(const std::vector<HeatColumnPin>& pins) {
    heatPins_ = pins;
    heatColValid_ = false;
  }

  // One 30 Hz tick. Caller writes tickUBO/opsBuf/expOps and zeroes the write-
  // page particle count via queue.WriteBuffer first, then submits the encoder
  // produced here before encoding the next tick. particlesActive lets a
  // settled world skip the particle passes entirely; the caller must derive it
  // ONLY from tick-deterministic inputs (see main.cpp) or determinism breaks.
  // fluidCount is the CPU's CONSERVATIVE MLS-MPM live estimate (the GPU owns
  // the real count — world.Snap().fluidLive plus spawns since that snapshot);
  // fluidSpawnCount is this tick's spawn-op count. The fluid seam + substeps
  // record while either is non-zero OR the disturbance-excite tuning mode is
  // on with an active CA (excite can birth particles from a world that has
  // none). All three inputs are tick-deterministic.
  // `windWakeCount` is the number of chunk slots this tick's wind primitives
  // want dirty-marked (TickParams.windWake). Zero on every tick of a world
  // with no fan in it, which is what skips the row entirely.
  void EncodeTick(const rhi::CommandEncoder& enc, uint32_t opsCount,
                  bool hashEnable, uint32_t expCount, bool particlesActive,
                  uint32_t cellCount, uint32_t spawnCount = 0,
                  uint32_t fluidCount = 0, uint32_t fluidSpawnCount = 0,
                  uint32_t windWakeCount = 0, bool vizActive = false,
                  uint32_t waterChunkCount = 0,
                  uint32_t waterDrainBodies = 0,
                  // M5: the scheduled sweep's body slot, or kWaterBodyCap
                  // for "nothing re-derives this tick" — which is the
                  // default and the state of every untouched lake.
                  uint32_t waterSweepSlot = 0xFFFFFFFFu);

  // The tick's RENDER-ONLY derived passes (pass_table.def PT_DERIVED: openness
  // + glow), recorded with the SAME counts, flags and bind-group page the last
  // EncodeTick used — so call it before FlipPage. docs/PLAN_async_compute.md.
  //
  // WHERE THEY RECORD (audit 2026-10-02): with SetDerivedDeferred(false) — the
  // default, render.asyncCompute OFF — EncodeTick records them itself, right
  // after the tick table, which is exactly where they sat as PT_TICK rows
  // before the async work: the command stream with the switch off is the
  // pre-async one plus the one 256-byte RenderUBOTick copy. They must NOT move
  // to the end of the tick in that mode: the MLS-MPM seam's settle writes
  // voxels after the tick table, and the derived walk would then read voxels
  // newer than the occupancy it skips empty bricks by (a render-visible change
  // for a switch that is meant to be scheduling only). With
  // SetDerivedDeferred(true) EncodeTick leaves them out and SubmitTick calls
  // EncodeDerived on the async encoder; `asyncQueue` drops the pass timer,
  // whose query pool belongs to the main queue.
  void SetDerivedDeferred(bool deferred) { derivedDeferred_ = deferred; }
  void EncodeDerived(const rhi::CommandEncoder& enc, bool asyncQueue = false);

  // ---- the settled-tick skip (ROADMAP_scale.md §3.4) ----------------------
  //
  // A fully settled world still recorded 54 `DispatchWorkgroupsIndirect` calls
  // whose indirect args said (0,1,1): 141.7 µs/tick measured, ~53% of a settled
  // tick, for provably zero work. That is a rule-2 violation, and it scales
  // with the DISPATCH COUNT, not the world — so it survives every other
  // optimization and costs 8x more at a 2048³ window.
  //
  // The fix is to not record the rows at all when the dirty set is provably
  // empty. "Provably" is the whole difficulty: the count lives in a GPU buffer
  // (sim_compact writes dispatchArgs), and reading it back synchronously to
  // decide would put a stall in the frame path — forbidden by rule 3's traffic
  // budget and far more expensive than the dispatches it saves.
  //
  // So the decision is made from a CONSERVATIVE CPU MIRROR, on exactly the
  // model `particlesActive` already uses at the main.cpp call site: the skip
  // is taken ONLY when every input that could dirty a chunk says no. The
  // asymmetry is deliberate and is the safety property —
  //
  //     a WRONG "active" costs 141 µs.   a WRONG "idle" loses world state.
  //
  // so every uncertain case must resolve to "active". Uncertainty here is
  // staleness, and it is now a KNOWN staleness: the snapshot is exactly
  // `World::kSnapshotLatency` ticks latent (DESIGN.md §2), on every machine and
  // at every frame rate, rather than "one tick latent, older when the readback
  // ring is saturated". `SettledSnapshot` still requires the snapshot to be
  // NEWER than the last tick anything could have dirtied, never merely
  // non-zero — the fixed age makes that test's outcome reproducible, it does
  // not make the test unnecessary.
  //
  // Determinism (rule 1) is not at risk here in the way a sim change would be,
  // and the reason is worth stating precisely rather than assuming: skipping
  // a dispatch whose workgroup count is zero removes NO invocation, so the
  // sequence of writes to the voxel buffer is bit-identical either way. The
  // skip is a pure recording-side decision, like a PassRow condition. What it
  // must NOT do is skip a dispatch that would have run one workgroup, which is
  // what the conservative mirror exists to prevent. The gate is the pinned
  // hash: a skip that ever fires wrongly moves 7cfa2420 immediately.
  //
  // Call once per tick BEFORE EncodeTick, with everything the CPU knows.
  // `dirtiedNow` is true if this tick has ANY input that marks a chunk: ops,
  // explosions, cell ops, spawns, streaming refills, a wake-all, a load.
  void NoteTickInputs(uint32_t tick, bool dirtiedNow);
  // Feed an arriving snapshot. `activeChunks` is its dirty-flag count,
  // `particleCount` its live voxels-in-flight count and `snapTick` the tick it
  // was stamped at. Only a snapshot showing ZERO active chunks AND ZERO
  // particles, stamped at or after the last dirtying tick, can license a skip.
  //
  // The particle conjunct is §3.2d: it is what lets the CA latch ignore
  // `particlesActive` (a 400-tick wall-clock timer in main.cpp) and reason from
  // the same evidence the rest of §3.4 uses. Both counts come off the SAME
  // snapshot word, captured at the same point in the tick, which is what makes
  // the reinsertion window closed rather than merely narrow — see the .cpp.
  // ---- gas particles (docs/PLAN_gas_particles.md stage 1) ----------------
  // Two one-line inputs to the C_GAS latch in EncodeTick, kept separate from
  // NoteSnapshot so adding gas did not change a signature every caller of the
  // settled-tick machinery already forwards.
  //
  // NoteGasLive: the live parcel count from the snapshot ring (async, several
  // ticks latent). NoteGasSpawns: CPU-authored spawns uploaded for THIS tick,
  // which the latch must see before EncodeTick decides whether to record the
  // drain that consumes them.
  // Also disarms the settled-tick skip directly, so the fix does not depend on
  // whether a caller happens to call this before or after NoteSnapshot. Both
  // orders are safe and both are in the conservative direction: a live parcel
  // can only COST a skip, never license one.
  void NoteGasLive(uint32_t live) {
    gasLive_ = live;
    if (live != 0) settledProven_ = false;
  }
  void NoteGasSpawns(uint32_t n) { gasSpawnsThisTick_ += n; }
  // Far fire plumes (world.h kGasFarEmitMax). How many frozen-fire emitters
  // the CPU has in the buffer for THIS tick -- the dispatch extent of the
  // splat row and half of the density box's clear condition. Set before
  // EncodeTick for NoteGasSpawns' reason: the recorder decides there whether
  // the row exists at all. Latched rather than per-call because the emitter
  // list only changes when the world does, and the buffer keeps holding the
  // last list until it does.
  // `wide` is the LONG-RANGE list (world.h kGasFarOuterN), which is disjoint
  // from `fine`: the CPU splits the surviving emitters by distance, so the two
  // counts never describe the same fire.
  void NoteFarPlumes(uint32_t fine, uint32_t wide) {
    farPlumeCount_ = fine;
    farPlumeWideCount_ = wide;
  }
  uint32_t FarPlumeCount() const { return farPlumeCount_; }
  uint32_t FarPlumeWideCount() const { return farPlumeWideCount_; }
  uint32_t GasLive() const { return gasLive_; }
  // NoteGasSeen: is there gas ANYWHERE the renderer would have to draw --
  // parcels outside the window OR gas voxels inside it. The second half is the
  // reason this is not just `gasLive_ > 0`: in-window smoke is a VOXEL and
  // leaves no parcel behind until it reaches a face, so a plume that never
  // leaves the window would arm nothing and the crossfade would be off exactly
  // where it matters most. Both halves come off the snapshot ring, so this is
  // latent in the same way and by the same amount as everything else there;
  // the hold in EncodeTick is what covers the latency.
  void NoteGasSeen(bool seen) { if (seen) gasSeenHold_ = kGasSeenTicks; }

  void NoteSnapshot(uint32_t snapTick, uint32_t activeChunks,
                    uint32_t particleCount);
  // Everything that wakes chunks outside the op path funnels here, so a caller
  // that forgets one keeps the CA running rather than silently losing it.
  // Stamps the current tick — NOT a forever-dirty sentinel; see the .cpp for
  // why a sentinel silently disabled the skip for the life of the process.
  void NoteWakeAll();
  // Whether the CA rows would be recorded for the NEXT tick. Measurement and
  // gates only — nothing in the sim may branch on this.
  bool CaSkipped() const { return caSkipped_; }
  // Whether the last EncodeTick recorded the MPM seam (and so its last row,
  // the swimming fold). The snapshot readback copies the fold only then.
  // Readback plumbing only — nothing in the sim may branch on this either.
  bool FluidSeamRecorded() const { return fluidSeamRecorded_; }
  uint64_t CaSkipCount() const { return caSkipCount_; }
  // MEASUREMENT / TEST ONLY: force the CA rows to be recorded every tick, i.e.
  // defeat the §3.4 skip. Two uses, both of which need it to be a switch rather
  // than an #ifdef:
  //   - the `ca-skip` gate runs one scripted explosion twice, skip-on and
  //     skip-off, and asserts the per-tick hash sequences are IDENTICAL. That
  //     differential is the only test that can catch "a chunk was processed one
  //     tick late", which a single-run hash cannot.
  //   - --measure reads the content-free dispatch floor off a settled world.
  // Forcing can only ADD work whose indirect count is zero, so it is
  // hash-neutral by the same argument the skip itself is (see the .cpp).
  // Also settable process-wide with SANDVOX_CA_FORCE=1.
  void SetCaForced(bool on) { caForced_ = on; }

  // Render pass with the shared depth target (raymarch writes frag_depth,
  // raster geometry depth-tests against it). At render.renderScale 1 the
  // caller draws the UI into this same pass; below 1 it blits the world up and
  // draws the UI in BeginOverlayRenderPass instead.
  rhi::RenderPass BeginRenderPass(const rhi::CommandEncoder& enc,
                                          const rhi::TextureView& target,
                                          rhi::TextureFormat format,
                                          uint32_t width, uint32_t height);
  // Same pass, on the SECOND depth cache and with a caller-chosen clear
  // colour. For an offscreen pass drawn in the same frame as the main one at a
  // different size — today the character panel's avatar portrait.
  //
  // It exists as its own entry point rather than as a parameter on
  // BeginRenderPass because the reason is not the clear colour, it is the
  // DEPTH CACHE: EnsureDepth keys on (width, height) alone, so alternating a
  // 448x640 portrait with a 1600x900 frame through one cache recreates a
  // full-screen depth texture twice every frame the panel is open.
  rhi::RenderPass BeginAuxRenderPass(const rhi::CommandEncoder& enc,
                                     const rhi::TextureView& target,
                                     rhi::TextureFormat format, uint32_t width,
                                     uint32_t height, const float clear[4]);
  // The native-resolution pass the UI draws into when the world was rendered
  // at render.renderScale < 1 and blitted up: colour LOADS (the blit put the
  // world there), depth is its own cache cleared fresh. ImGui's pipeline is
  // built against kDepthFormat, so the pass must carry a depth attachment even
  // though nothing in it depth-tests. A THIRD depth cache, for the EnsureDepth
  // reason: the world pass now keys its cache on the internal size, and a
  // native-size pass sharing it would recreate both every frame.
  rhi::RenderPass BeginOverlayRenderPass(const rhi::CommandEncoder& enc,
                                         const rhi::TextureView& target,
                                         rhi::TextureFormat format,
                                         uint32_t width, uint32_t height);
  void DrawWorld(const rhi::RenderPass& pass);
  void DrawParticles(const rhi::RenderPass& pass);
  // MLS-MPM fluid prototype: instanced cubes from the fluid particle buffer.
  // `count` is the CPU-owned particle count; 0 draws nothing at all.
  void DrawFluid(const rhi::RenderPass& pass, uint32_t count);
  void DrawSprites(const rhi::RenderPass& pass, uint32_t count);
  // Collision-box debug overlay: one oriented wireframe box per physics body.
  // Drawn LAST of the world passes (after the micro bodies, before ImGui) with
  // depth testing off, so a collider is visible through whatever contains it.
  // `count` of 0 draws nothing at all — the overlay is free when it is off.
  void DrawDebugBoxes(const rhi::RenderPass& pass, uint32_t count);
  // The vessel's pour point (game/container.h ContainerPourPoint): ONE soft,
  // mostly transparent sphere read from debugBoxes[`at`] (centre = pos,
  // radius = half.x, colour + alpha = color). Depth TESTED, unlike the
  // wireframes -- it marks a place in the world, so the ground in front of it
  // hides it. `at` of UINT32_MAX draws nothing.
  void DrawPourMarker(const rhi::RenderPass& pass, uint32_t at);
  // Wind slope-field overlay (docs/RESEARCH_wind.md §4.8, F4): one arrow per
  // lattice point around the camera, oriented and coloured by `windAt` — the
  // same field function the foliage sway samples. `arrows` is
  // WindDebugArrowCount(tuning) (sim/wind.h); 0 draws nothing at all.
  //
  // Nothing is uploaded for this: the vertex shader derives every lattice
  // point from its instance index and R.camPos, so there is no arrow buffer,
  // no per-arrow CPU work, and no new bind group.
  void DrawWindField(const rhi::RenderPass& pass, uint32_t arrows);
  // The gust streaks (wind_streak.wgsl): `count` pool slots, `trail` points
  // each. Zero count is skipped outright; see WindStreakDrawCount.
  void DrawWindStreaks(const rhi::RenderPass& pass, uint32_t count, uint32_t trail);
  // Kill every streak (zero the pool, lifetime 0 = never spawned). For a
  // harness that pinned a windy regime and must not leave its streaks, frozen,
  // in the frames that follow.
  void ClearWindStreaks();
  // The pool itself, for a gate that seeds probe streaks and reads back where
  // one update moved them (heat-updraft: the render windAt over heat). Test
  // only; the frame path never reads it back.
  const rhi::Buffer& WindStreakBuffer() const { return windStreakBuf_; }
  // The current field's arrows (docs/PLAN_water_master.md component 8).
  void DrawCurrentField(const rhi::RenderPass& pass, uint32_t arrows);
  // Body cubes: vertices per instance (three camera-facing faces, see
  // debris.wgsl bodyVertex). Every body draw uses this count.
  static constexpr uint32_t kBodyCubeVerts = 18;
  // The whole instance list [0, voxInstances): the harness/gate path.
  void DrawBodies(const rhi::RenderPass& pass, uint32_t voxInstances);
  // Culled per-body draws: [firstInstance, count] pairs (CullBodyRanges,
  // game/bodyreg.h). The frame loop's path.
  void DrawBodyRanges(const rhi::RenderPass& pass,
                      const std::vector<std::pair<uint32_t, uint32_t>>& draws);
  // Microvoxel bodies (PLAN §C): one 36-vertex OBB per entry in `insts`, drawn
  // between DrawBodies and DrawSprites. `insts` is the compacted (slot, model)
  // list built by the caller from the frame's body slots; an empty list draws
  // nothing at all, so a world with no micro bodies pays zero.
  // Upload this frame's micro-body instance list. MUST be called BEFORE
  // BeginRenderPass, and it is a separate call for exactly that reason.
  //
  // This used to live inside DrawMicroBodies, i.e. a queue.WriteBuffer issued
  // with the render pass open. WebGPU defines that as legal (the write is
  // ordered on the QUEUE, not in the command buffer, so it lands before the
  // submit that contains the pass), and Dawn happily accepted it. Vulkan does
  // NOT: vkCmdUpdateBuffer and vkCmdCopyBuffer are forbidden inside a render
  // pass instance / dynamic rendering scope, so the pattern had to be hoisted
  // regardless of backend (docs/vulkan_barrier_graph.md §4.6).
  //
  // Returns the number of instances actually uploaded — DrawMicroBodies takes
  // that count, so passing a stale or unuploaded list cannot silently draw
  // garbage. The signature change is deliberate: the old call shape no longer
  // compiles, which is what stops the in-pass write regressing invisibly.
  uint32_t UploadMicroBodyInsts(const rhi::Queue& queue,
                                const std::vector<MicroBodyInstGpu>& insts);
  // Pure draw: no uploads, no queue. `count` comes from UploadMicroBodyInsts.
  void DrawMicroBodies(const rhi::RenderPass& pass, uint32_t count);

  // ---- TAA + temporal upscale (assets/shaders/taa.wgsl) --------------------
  //
  // The pass that REPLACES the NEAREST blit at the end of a scaled frame: it
  // accumulates sub-pixel-jittered low-resolution frames into a
  // native-resolution history and resolves that into the swapchain. See the
  // header of taa.wgsl for the mechanism, and main.cpp's jitter block for
  // where the camera perturbation is applied (CPU-side, folded into the basis,
  // so the raymarch and every raster path move together).
  //
  // ALL RENDER-ONLY DERIVED DATA. Nothing here is read by the sim, hashed, or
  // saved; a run with taa on and one with it off produce the same world.
  //
  // The camera a frame was rendered with, as this pass needs it. `eye` is
  // ABSOLUTE world position (not R.camPos, which is window-relative): the
  // residency window can shift between two frames, and the reprojection needs
  // a delta that survives that.
  struct TaaCamera {
    float right[3] = {1, 0, 0};
    float up[3] = {0, 1, 0};
    float fwd[3] = {0, 0, 1};
    double eye[3] = {0, 0, 0};
    float tanHalfFov = 1.0f;
    float aspect = 1.0f;
    // This frame's image shift, in RENDER pixels — the jitter that was folded
    // into the basis above, in the units the shader reconstructs with.
    float jitterX = 0.0f, jitterY = 0.0f;
  };
  // The R2 (plastic-constant) low-discrepancy jitter sequence, +/-0.5 px.
  // Public and pure so the caller can scale it (render.taaJitter) and so it is
  // one definition rather than a magic pair of constants in main.cpp.
  static void TaaJitter(uint32_t frameIdx, float* jx, float* jy);
  // Fragment stores are the hard requirement (the resolve writes its history
  // from a fragment shader, like the shadow cache); the pipeline is the soft
  // one — a shader that failed to compile leaves TAA off rather than crashing.
  bool TaaAvailable() const { return taaAvailable_; }
  // Size/allocate for this frame. Cheap and idempotent when nothing moved;
  // a size change resets the history, because a resized history is not one.
  void EnsureTaa(uint32_t renderW, uint32_t renderH, uint32_t nativeW,
                 uint32_t nativeH);
  // Copy the finished world frame (colour + the depth it wrote) into the
  // storage buffers the resolve reads. MUST be recorded after the world pass
  // has ENDED — a transfer inside a rendering scope is illegal, and the
  // recorder silently drops it.
  void EncodeTaaCapture(const rhi::CommandEncoder& enc,
                        const rhi::Texture& colorTex);
  // Upload the frame's params. Diffs against the camera stored by the previous
  // call, so the caller never tracks a previous frame itself. `reset` forces
  // the history away (first frame, a teleport, a knob change).
  void WriteTaaParams(const rhi::Queue& queue, const TaaCamera& cam,
                      float maxHist, float clampK, bool reset, bool bgraSource);
  rhi::RenderPass BeginTaaRenderPass(const rhi::CommandEncoder& enc,
                                     const rhi::TextureView& target,
                                     rhi::TextureFormat format, uint32_t width,
                                     uint32_t height);
  void DrawTaa(const rhi::RenderPass& pass);
  // Swap the history read/write halves. Call once per resolved frame, after
  // the submit, exactly like FlipPage.
  void FlipTaaPage() { taaPage_ = 1 - taaPage_; }
  // Drop the accumulated history at the next resolve (teleport, load, a scale
  // or toggle change). Cheap: one flag, no allocation.
  void ResetTaa() { taaReset_ = true; }

  // ---- the shading-LOD filter (assets/shaders/denoise.wgsl) ----------------
  //
  // A depth-guided a-trous pass over the finished world frame, IN PLACE, at
  // render resolution, before TAA or the upscale blit. It averages the
  // lighting of terrain whose voxels project smaller than `render.denoisePx*`
  // pixels — the mid-distance staircase speckle — and leaves near geometry,
  // the sky and depth edges alone. See the shader header for why a
  // path-tracing denoiser is the wrong tool for that noise.
  //
  // ALL RENDER-ONLY DERIVED DATA, like TAA: no history, nothing hashed or
  // saved, and the world is identical with the knob at 0 or 1.
  //
  // Same availability rule as TAA: the pipeline must have compiled. (No
  // fragment stores are needed — the pass reads two storage buffers and
  // writes its attachment.)
  bool DenoiseAvailable() const { return denoiseAvailable_; }
  static constexpr uint32_t kDenoiseMaxIters = 4;
  // Size the capture buffers and the per-iteration bind groups. Idempotent.
  void EnsureDenoise(uint32_t width, uint32_t height);
  // Upload every iteration's params. `tanHalfFov` is the projection the frame
  // was rendered with — the projected-size ramp is derived from it — and
  // `iters` is clamped to kDenoiseMaxIters. Call BEFORE any render pass opens
  // in the command buffer that will run the filter (a buffer write inside a
  // rendering scope is illegal in Vulkan).
  void WriteDenoiseParams(const rhi::Queue& queue, uint32_t width,
                          uint32_t height, float tanHalfFov, bool bgraSource);
  // Record the filter over `tex`: capture colour + depth, then one fullscreen
  // pass per iteration, the result landing back in `tex`. MUST be recorded
  // after the world pass has ENDED and before anything reads `tex`. Records
  // nothing when unavailable or when the last WriteDenoiseParams asked for 0
  // iterations.
  void EncodeDenoise(const rhi::CommandEncoder& enc, const rhi::Texture& tex,
                     const rhi::TextureView& view, rhi::TextureFormat format,
                     uint32_t width, uint32_t height);

  static constexpr rhi::TextureFormat kDepthFormat = rhi::TextureFormat::Depth32Float;
  // The world pass's depth attachment (reversed-Z, CopySrc-capable): the
  // `denoise` gate reads it back to classify pixels by distance.
  const rhi::Texture& DepthTexture() const { return depthTex_; }

  // Which dirty buffer the tick just encoded writes as "active next tick".
  const rhi::Buffer& DirtyNext() const { return world_->dirty[1 - page_]; }
  // The dirty buffer the NEXT tick will read (valid after FlipPage) — used by
  // the selftest to count active chunks in a settled world.
  const rhi::Buffer& DirtyActive() const { return world_->dirty[page_]; }
  // Particle page semantics: EncodeTick reads particles[Page()] and writes
  // particles[1 - Page()]; after FlipPage, Page() is the live buffer (what the
  // renderer draws and the next tick reads).
  uint32_t Page() const { return (uint32_t)page_; }
  // Call once after each EncodeTick has been submitted.
  void FlipPage();

  // MEASUREMENT ONLY (`--measure`, src/measure/measure.cpp). When non-null,
  // every compute pass EncodeTick/EncodeWorldgen opens carries GPU timestamp
  // writes labelled with the pass name. NULL in the game and in --selftest, so
  // the encoded command buffer is unchanged. Timestamps observe a pass; they
  // do not reorder or gate any dispatch, so the world hash is unaffected.
  void SetPassTimer(PassTimer* t) { passTimer_ = t; }
  // Resolve timestamp queries into the readback buffer. Encoded at the END of
  // EncodeTick, so a caller that does nothing special still gets a complete
  // command buffer — SubmitTick needed no changes at all. No-op without a
  // timer, which is every non-measure run.
  void EncodeTimerResolve(const rhi::CommandEncoder& enc) const {
    if (passTimer_) passTimer_->EncodeResolve(enc);
  }

  // Build the render pipelines NOW rather than on the first BeginRenderPass.
  // The one caller is `--shader-stats`: graphics pipelines are created lazily
  // (BuildPipelines leaves targetFormat_ Undefined), so a mode that walks the
  // pipeline list without drawing anything would find no `raymarch` — the row
  // it exists to print. Format-keyed like the lazy path, so a subsequent draw
  // in the same format is a no-op rather than a rebuild.
  void ForceRenderPipelines(rhi::TextureFormat format) {
    EnsureRenderPipelines(format);
  }

  // ---- deferred far pipelines (docs/PLAN_shader_compile.md package A) ------
  //
  // worldgen's `far` (746 s of NVIDIA driver compile) and `fardown` (98 s) are
  // built on a background thread and BuildPipelines returns without them, so a
  // worldgen edit is playable in ~100 s instead of ~17 min. That is only sound
  // because the cascades are RENDER-ONLY DERIVED DATA (worldgen.wgsl's far
  // block, DESIGN.md §9): the world ticks, hashes and saves identically
  // without them, it just has no horizon.

  // Who is allowed to run while they are still compiling. DEFAULT IS NO, so a
  // mode added later is correct without knowing this exists; main.cpp opts in
  // the interactive game (the point of the exercise) and the voxel-region
  // tools (which read no cascade at all). Everything else — every gate, every
  // --shot frame, every --render-budget arm, every smoke probe — blocks, so no
  // checked output can depend on when the driver happened to finish.
  void AllowDeferredFar(bool on) { deferFarOk_ = on; }

  // WHEN the far set compiles at all. `Eager` starts the background build at
  // the end of BuildPipelines, which is right for anything that will render a
  // horizon (the game, every --shot, the full suite). `Lazy` starts it only
  // when a caller DEMANDS cascade content (EnsureFarPipelines with fills to
  // record), and a run that never does never compiles it.
  //
  // The reason this exists is what a deferred-but-eager build cost the modes
  // that never look at a cascade: --voxdump, --voxserve and every `--gate`
  // that is not a far gate. Their three far threads (Tint + spirv-opt + the
  // driver, ~100 s of CPU each after a worldgen edit) ran alongside the real
  // work, slowing it, and then the process could not EXIT until they finished
  // — a std::async future's destructor joins its thread — so a 5 s gate sat
  // silent for minutes after printing its result. Measured 2026-09-09, one
  // --voxdump after a one-line genCellIn edit: see docs/PLAN_shader_compile.md
  // "Lazy far". Set BEFORE Init: BuildPipelines reads it.
  enum class FarBuild { Eager, Lazy };
  void SetFarBuild(FarBuild b) { farBuild_ = b; }

  // ---- the SPECIALIZED raymarch variant (W2-A) ------------------------------
  // Same deal as AllowDeferredFar and set from the same line in main.cpp. The
  // lean raymarch pipeline (raymarch.wgsl's SPEC_* block) compiles in the
  // background; when a frame is eligible for it and it is not published yet,
  // DrawWorld either draws the universal pipeline (deferral allowed: the
  // interactive game, which must not stall) or BLOCKS until the variant exists
  // (deferral not allowed: every --shot, gate and budget arm).
  //
  // The two pipelines are required to produce identical pixels — the
  // specialization is dead-code removal and nothing else — so the difference is
  // in principle unobservable. The blocking default is here anyway, because
  // "the picture is identical" is a claim this package MEASURED rather than a
  // property the type system enforces, and a checked output whose shader
  // depends on when a driver thread finished is not a checked output.
  void AllowDeferredRaymarchVariant(bool on) { deferRayVariantOk_ = on; }

  // Draw with the UNIVERSAL raymarch pipeline whatever the frame's predicates
  // say. The `nospec` --render-budget arm, which is the A/B this feature is
  // judged by: `baseline` minus `nospec` is what the variant is worth, measured
  // the same way `shadow0` measures the shadow call site's footprint.
  void SetForceUniversalRaymarch(bool on) { forceUniversalRay_ = on; }
  // Leave the ray-start map's rows unrecorded (the `norstart` arm): the
  // raymarch then reads a stale key and marches every ray from the camera.
  void SetRayStartOff(bool on) { rayStartOff_ = on; }
  // Leave the rain shadow map's rows unrecorded (the `norainmap` arm): the
  // raymarch's stamp check then reads the header as stale and gates rain and
  // wetness on openness, the pre-map path.
  void SetRainMapOff(bool on) { rainMapOff_ = on; }
  // THE RAIN LATTICE for the next EncodeTick (src/sim/rainexpo.h): this
  // tick's TickParams weatherRain + rainSlopeQx/Qz and window origin (chunks),
  // from which the rainFall and rainExpo dispatch extents are computed with
  // the kernels' own integer formulas. SubmitTick calls it beside the
  // TickParams upload; a caller that never does gets the vertical, dry
  // defaults (64 rainFall groups, no exposure map).
  void SetTickRain(uint32_t rainWord, int32_t slopeQx, int32_t slopeQz,
                   IVec3 originChunk);
  // The rain exposure map buffer, for a gate's readback (rain-lean) only.
  const rhi::Buffer& RainExpoBuffer() const { return rainExpoBuf_; }

  // ---- WIND DRAFTS: the shelter volume (sim_draft.wgsl, world.h kDraft*) ----
  // For the next EncodeTick: the gate (tuning sim.draftMode) and the box's
  // origin (TickParams.draftOrigin, world voxels). A change of either, a
  // material upload, or a fresh buffer makes that tick a REBUILD -- every
  // chunk of the box re-masked -- and otherwise the box re-masks only its
  // active chunks. SubmitTick calls it beside the TickParams upload.
  void SetDraft(bool on, const int32_t origin[3]);
  // What the renderer may read: the box's origin as last ENCODED, and whether
  // a solve has been recorded for it (RenderParams.draftMode).
  bool DraftValid() const { return draftValid_; }
  // This tick re-masks the whole box and runs the whole solve (a BURST).
  bool DraftRebuild() const { return draftRebuild_; }
  const int32_t* DraftOrigin() const { return draftOrigin_; }
  // The volume and its meta words, for a gate's readback (drafts) only.
  const rhi::Buffer& DraftBuffer() const { return draftBuf_; }
  const rhi::Buffer& DraftMetaBuffer() const { return draftMetaBuf_; }
  // Force the next tick to re-mask the whole box (a gate's purity check).
  void ForceDraftRebuild() { draftForce_ = true; }
  // The stack effect's knobs as the kernels see them (TickParams
  // updraftGainQ / updraftCapQ / draftStackQ): the stack field's b is solved
  // with them, so a change forces a rebuild. SubmitTick calls it before
  // SetDraft every tick; the first call only records.
  void NoteDraftHeatKnobs(int32_t gainQ, int32_t capQ, int32_t stackQ) {
    if (draftHeatKnobsSet_ && (gainQ != draftHeatKnobs_[0] || capQ != draftHeatKnobs_[1] ||
                               stackQ != draftHeatKnobs_[2]))
      draftForce_ = true;
    draftHeatKnobs_[0] = gainQ;
    draftHeatKnobs_[1] = capQ;
    draftHeatKnobs_[2] = stackQ;
    draftHeatKnobsSet_ = true;
  }

  // Publish a finished background compile and return true EXACTLY ONCE: on the
  // call that made the pipelines live. That is the caller's cue to
  // FarField::FullRefill — the cascades are empty (nothing was ever recorded
  // for them) and only a wholesale refill puts a horizon back. Never blocks.
  bool PollFarPipelines();
  bool FarPipelinesReady() const {
    return farReady_.load(std::memory_order_acquire);
  }
  // EncodeFarFill will record NOTHING this tick and is not allowed to block
  // to fix that: the deferred build is started but not landed, and this
  // caller opted into deferral. A caller in that state must not let FarField
  // pop entries into the hole (FarField::PrepareTick's `drain` argument, and
  // the long note there about the sky being drawn under the ground).
  //
  // All three conjuncts matter. Without `deferFarOk_` this would also fire
  // for --shot / --perf / the gates, which do NOT opt in: they block inside
  // EncodeFarFill until the pipelines exist, so their fills are never
  // dropped and holding their queue would only delay a checked output.
  // Without `farStarted_` it would fire forever under FarBuild::Lazy, where
  // nothing is compiling and nothing is coming.
  bool FarFillsDeferred() const {
    return deferFarOk_ && farStarted_ &&
           !farReady_.load(std::memory_order_acquire);
  }
  // Block until the deferred compile finishes, then publish. Idempotent, and a
  // no-op when the far pipelines were never deferred.
  void WaitForFarPipelines();

 private:
  bool BuildPipelines(const rhi::Device& device, std::string* err);
  // What the deferred thread returns. A struct rather than two futures so the
  // "both are ready" test is one wait and the completion work (MarkFarReady,
  // SavePipelineCache) has one place to happen.
  struct FarPipelines {
    // `fill` is worldgen.wgsl's `far` sweep, `patch` its `farpatch` half
    // (PLAN_shader_compile package C item 1). Both are deferred and both are
    // waited on together: a cascade filled by the sweep alone would drop every
    // far-field edit, which is a WRONG horizon rather than a missing one.
    rhi::ComputePipeline fill, map, patch, down;
    // fardown's follow-up phases (worldgen.wgsl fardownClaim / fardownStalk /
    // fardownFeat; cross-vendor audit #9). Small entries: no procgen.
    rhi::ComputePipeline downClaim, downStalk, downFeat;
  };
  // Move the future's result onto the three far pipeline members. Main thread only.
  void PublishFarPipelines();
  // Launch the far set's compile from `farModule_` on farPL_. Idempotent per
  // BuildPipelines (farStarted_). `threads <= 1` builds serially, in place,
  // and publishes before returning (--shader-stats).
  void StartFarBuild(unsigned threads);
  // Blocks unless the caller opted into deferral. Called from EncodeFarFill
  // with work to do — the one point where a caller is about to depend on
  // cascade CONTENT. Recording a far row against a pipeline that does not
  // exist yet is legal (the recorder skips a null pipeline); what is not
  // acceptable is a checked output that silently depends on driver timing.
  // Under FarBuild::Lazy this is also where the compile STARTS.
  void EnsureFarPipelines() {
    if (deferFarOk_ || farReady_.load(std::memory_order_acquire)) return;
    StartFarBuild(farBuildThreads_);
    WaitForFarPipelines();
  }
  // The full and slim sim bind groups, both pages. Called by Init and again by
  // UploadEnvironment when a table buffer had to be recreated.
  void BuildSimBindGroups(const rhi::Device& device);
  void EnsureDepth(uint32_t width, uint32_t height);
  void EnsureAuxDepth(uint32_t width, uint32_t height);
  void EnsureOverlayDepth(uint32_t width, uint32_t height);
  // The water veil (common.wgsl THE WATER VEIL): grow the per-pixel record
  // buffer to cover a width x height target and rebuild renderBG_ around it.
  void EnsureVeil(uint32_t width, uint32_t height);
  // The ray-start map (ray_start.wgsl), grow-only like the veil: sized for
  // the largest target a world pass has drawn into, so the per-frame rows
  // (recorded BEFORE the pass that learns the size) cover any later one.
  void EnsureRayStart(uint32_t width, uint32_t height);
  // THE CLOUDS (cloud.wgsl): grow the low-res raw + history buffers to cover
  // a lowW x lowH target and rebuild the two bind groups that name them.
  // Grow-only, like the veil. Fresh buffers are written with "clear sky"
  // (T = 1), so a composite that runs before the first march draws the sky it
  // would have drawn with no clouds, never a black one.
  void EnsureClouds(uint32_t lowW, uint32_t lowH);
  void BuildShadowBindGroup();
  void BuildRenderBindGroup(rhi::BindGroup& out, const rhi::Buffer& veil);
  // The group-0 bind group a raster body draw uses: the live veil once this
  // pass's DrawWorld has written it, otherwise one bound to a zeroed record so
  // a body-only pass (the character portrait) never reads a veil some other
  // pass wrote for other pixels.
  const rhi::BindGroup& BodyRenderBG() const {
    return veilLive_ ? renderBG_ : renderBGNoVeil_;
  }
  // BodyRenderBG for a draw whose fragment stage READS the veil. The scope
  // split that makes the raymarch's veil writes visible is taken here, by the
  // first such draw after DrawWorld, instead of unconditionally in DrawWorld:
  // a pass that draws no veil reader after the world (--shot, a sky-only
  // frame) pays no split at all.
  const rhi::BindGroup& VeilReaderBG(const rhi::RenderPass& pass);
  void EnsureRenderPipelines(rhi::TextureFormat format);
  // Derive raymarchLeanModule_ from an already-loaded raymarch module by
  // flipping the three `const SPEC_* : bool = true;` lines in the source
  // LoadShader assembled. Leaves the module invalid (and says so on stderr) if
  // any of the three lines is not found verbatim — the only failure mode a
  // string substitution has, and one that would otherwise ship a "specialized"
  // pipeline byte-identical to the universal one.
  void BuildRaymarchVariant(const rhi::Device& device,
                            const rhi::ShaderModule& base);
  // Move a finished background compile onto raymarchLean_. Main thread only;
  // never blocks unless `block` is set.
  void PollRaymarchVariant(bool block);
  // Stamp the cached art palette into a material table being (re)built.
  void ApplyArtPalette(std::vector<MaterialGpu>& table) const;

  // ---- table-driven recording (docs/PLAN_vulkan_port.md phase 2b) ----------
  // Every Encode* above records by walking src/sim/pass_table.def rather than
  // issuing commands inline, because phase 3 generates Vulkan barriers from the
  // same table and a declaration that is not also the recording will drift from
  // it. See pass_table.def's header for what a row means and why.
  //
  // `ctx` is an opaque pointer to the anonymous-namespace RecordCtx in
  // simulation.cpp — the per-call counts and flags the row conditions and
  // dispatch selectors resolve against. It is deliberately not a public type:
  // nothing outside the recorder has any business constructing one.
  void RecordTable(const rhi::CommandEncoder& enc, pass::Table which,
                   const void* ctx);
  // Resolve a table buffer id to the live buffer. DirtyIn/DirtyOut and the two
  // particle pages are symbolic and resolve through page_.
  const rhi::Buffer& PassBuffer(pass::Buf b) const;
  const rhi::ComputePipeline& PassPipeline(pass::Pipe p) const;

  PassTimer* passTimer_ = nullptr;  // not owned; measurement harness only
  // EncodeDerived (async compute): the last EncodeTick's record context, and
  // "record without the pass timer" while an async encoder is being recorded.
  pass::RecordCtx lastTickCx_{};
  bool lastTickCxValid_ = false;
  bool recordNoTimer_ = false;
  bool derivedDeferred_ = false;  // SetDerivedDeferred

  World* world_ = nullptr;
  rhi::Device device_;
  std::string shaderDir_;
  // Cond::ReposeActive: does ANY loaded material author a non-default
  // `repose`? Latched in UploadTables, so a reload updates it.
  bool anyRepose_ = false;
  rhi::Buffer materialBuf_;
  rhi::Buffer reactionBuf_;
  // The baked tree atlas (src/sim/treeatlas.h): asset data, bound read-only
  // into simBGL_ and simSlimBGL_ at binding 26. Rewritten only by
  // UploadEnvironment (a reload from disk), never by the frame loop.
  rhi::Buffer treeAtlasBuf_;
  size_t treeAtlasWords_ = 0;
  // The authored world map (src/sim/worldmap.h): asset data, bound read-only
  // into simBGL_ and simSlimBGL_ at binding 31, on the same terms as the tree
  // atlas above.
  rhi::Buffer worldMapBuf_;
  size_t worldMapWords_ = 0;
  // The worldgen COLUMN CACHE (worldgen.wgsl's block of that name, bindings
  // 38/39 in simBGL_ only): genCols is the CPU-written map from list position
  // to chunk-column plus each chunk-column's coords and y-range; colCache is
  // what the `cols` pre-pass evaluates per (x, z) and genChunk reads.
  // genColCount_ is the chunk-column count of the last WriteGenList, the
  // `cols` dispatch extent. genColScratch_ is WriteGenList's staging.
  rhi::Buffer genColsBuf_, colCacheBuf_;
  // The solute species table (world.h kSolSpec* layout, binding 43), rebuilt
  // from assets/materials/solutes.json by every UploadTables -- so an R reload
  // of materials re-resolves the species' material names against the new
  // table, and a reload of solutes.json alone rides the same key.
  rhi::Buffer solSpecBuf_;
  std::vector<SoluteDef> solutes_;
  // The temperature layer's CPU-written parameters (heat.h kHp*, binding 52)
  // and its pipelines (sim_heat.wgsl). PrepareHeat's column-table cache: the
  // world column each table row/column currently describes.
  rhi::Buffer heatParamsBuf_;
  rhi::ComputePipeline heatBegin_, heatShift_, heatPend_, heatWant_, heatArgs_, heatAlloc_, heatSrc_,
      heatTent_, heatRelax_;
  std::vector<uint8_t> heatCol_;
  std::vector<int> heatColX_, heatColZ_;
  bool heatColValid_ = false;
  uint32_t heatColSeed_ = 0;
  uint64_t heatColKey_ = 0;
  bool heatProbeOn_ = false;
  int heatProbe_[3] = {0, 0, 0};
  // The live-knob tracker (PrepareHeat): the knob words last uploaded, and the
  // epoch heatBegin compares. Both restart (invalid / 0) on a worldgen or a
  // load, whose fill rows zero heatMeta's copy of the epoch.
  uint32_t heatKnobs_[kHpKnobEpoch] = {};
  bool heatKnobsValid_ = false;
  uint32_t heatKnobEpoch_ = 0;
  std::vector<HeatColumnPin> heatPins_;
  // The solute layer's pipelines (sim_solute.wgsl).
  rhi::ComputePipeline solWant_, solArgs_, solAlloc_, solDiffuse_, solCompact_, solScoop_, solPour_, solHash_,
      solEvict_, solRestore_;
  uint32_t genColCount_ = 0;
  std::vector<uint32_t> genColScratch_;
  // Art palette RGB (0x00RRGGBB), indexed from kArtPaletteBaseGpu. Cached so a
  // materials hot-reload can restore it — see SetArtPalette.
  std::vector<uint32_t> artPalette_;
  // UploadMicroBodies runs on every dirty frame (a burning limb is every
  // frame), and it re-sent the model table and the art run each time whether
  // or not either had changed. These remember what the GPU copy holds so an
  // unchanged table / palette is not written again. `*Live_` = "the buffer
  // holds exactly the last write recorded here"; anything else that writes the
  // same range (the whole material table, a buffer re-create) clears it.
  std::vector<MaterialGpu> artRunScratch_;
  bool artPaletteLive_ = false;
  std::vector<MicroBodyModelGpu> mbModelScratch_, mbModelLast_;
  bool mbModelLive_ = false;
  uint64_t mbPoolBytesSent_ = 0;
  std::vector<std::pair<uint32_t, uint32_t>> mbRangeScratch_;
  // Static micro-detail (render-only). Deliberately NOT in any sim bind group.
  rhi::Buffer microTableBuf_, microPoolBuf_;
  // Dynamic micro BODIES (render-only, same doctrine): per-def limb models, the
  // shared brick pool, and this frame's compacted (slot, model) draw list.
  rhi::Buffer mbModelBuf_, mbPoolBuf_, mbInstBuf_;

  // simSlimBGL_ mirrors simBGL_ bindings 0..4 only — the particle/explosion
  // pipelines pair it with particleBGL_ to stay under the 16-storage-buffer
  // per-stage pipeline-layout limit (Dawn counts layout entries, not usage).
  rhi::BindGroupLayout simBGL_, simSlimBGL_, particleBGL_, renderBGL_, renderPartBGL_,
      farBGL_, microBodyBGL_, fluidBGL_, fluidSeamBGL_, shadowBGL_, gasBGL_;
  rhi::PipelineLayout simPL_, simPL2_, renderPL_, farPL_, microBodyPL_, fluidPL_,
      fluidSeamPL_, shadowPL_, gasPL_;
  rhi::ComputePipeline worldgen_, worldgenList_, worldgenCols_, mutate_, mutateCells_, compact_,
      compactNext_, step_, reposeSnap_, caMask_, occupancy_, occupancyDirty_, pick_;
  // Wind primitive footprint wake (sim_mutate.wgsl `windWake`) — see
  // docs/RESEARCH_wind.md §4.3.
  rhi::ComputePipeline windWake_;
  // Rain on the ground + drying of wet top surfaces (sim_mutate.wgsl rainFall).
  rhi::ComputePipeline rainFall_;
  rhi::ComputePipeline rainExpo_;   // sim_rain_expo.wgsl `build`
  // The rain exposure map (rainexpo.h), 45 in simBGL_, and the two extents
  // SetTickRain derives for the next EncodeTick.
  rhi::Buffer rainExpoBuf_;
  uint32_t rainFallGroups_ = 64, rainExpoGroups_ = 0;
  // Wind drafts (sim_draft.wgsl): five entry points, the volume (46), its
  // meta words (47) and the solve's indirect args. draftForce_ starts TRUE:
  // the first tick after the buffer is made is a rebuild.
  rhi::ComputePipeline draftMaskAll_, draftMaskDirty_, draftArgs_, draftCoarseBuild_,
      draftCoarseFaces_, draftCoarseSolve_, draftFineFirst_, draftFineMid_, draftFineMid2_,
      draftFineLast_;
  rhi::Buffer draftBuf_, draftMetaBuf_, draftArgsBuf_;
  // The CA's air mask (pass_table.def caMask): 128 words per chunk slot.
  rhi::Buffer caMaskBuf_, caWindBuf_;
  // pass::Buf::RenderUBOTick (docs/PLAN_async_compute.md).
  rhi::Buffer renderUBOTickBuf_;
  bool draftOn_ = false, draftRebuild_ = false, draftForce_ = true, draftValid_ = false;
  bool draftLastOn_ = false;
  int32_t draftOrigin_[3] = {0, 0, 0};
  int32_t draftLastOrigin_[3] = {-2147483647 - 1, 0, 0};
  int32_t draftHeatKnobs_[3] = {0, 0, 0};
  bool draftHeatKnobsSet_ = false;
  rhi::ComputePipeline explodeMark_, explodeApply_, pArgs1_, pSpawn_, pIntegrate_,
      pArgs2_, pResolve_;
  // Gas particles (sim_gas.wgsl, docs/PLAN_gas_particles.md stage 1). Five
  // entry points shaped like the ballistic five above.
  rhi::ComputePipeline gArgs1_, gSpawn_, gIntegrate_, gArgs2_, gResolve_;
  // The window edge's leave budget, recorded before the CA (gasLeavePrep).
  rhi::ComputePipeline gLeavePrep_;
  // Far fire plumes (world.h kGasFarEmitMax): a sixth gas entry point on the
  // same layout, whose only output is the render-only density box.
  rhi::ComputePipeline gFarPlume_;
  // ...and its long-range sibling (world.h kGasFarOuterN).
  rhi::ComputePipeline gFarPlumeW_;
  // Live only after PublishFarPipelines. Until then both are INVALID handles
  // and the recorder skips their rows (vk_record.cpp's null-pipeline continue).
  rhi::ComputePipeline farFill_, farMapFill_, farPatchFill_, farDown_;
  rhi::ComputePipeline farDownClaim_, farDownStalk_, farDownFeat_;
  // The background compile. Valid between BuildPipelines and the publish;
  // `farPublished_` and `deferFarOk_` are main-thread-only, `farReady_` is the
  // one field any other thread may observe.
  std::future<FarPipelines> farFuture_;
  std::atomic<bool> farReady_{false};
  bool farPublished_ = false;
  bool deferFarOk_ = false;
  FarBuild farBuild_ = FarBuild::Eager;
  // What StartFarBuild compiles from: worldgen.wgsl's module as BuildPipelines
  // last loaded it, and the thread count that build used. Held so a Lazy start
  // thousands of ticks later compiles exactly what an Eager one would have.
  rhi::ShaderModule farModule_;
  unsigned farBuildThreads_ = 1;
  bool farStarted_ = false;
  // The openness grid (sim_openness.wgsl, docs/PLAN_gi.md §2): `dirty` walks
  // the tick's compacted dirty list, `refresh` walks a rolling slice of the
  // window. Render-path passes on the TICK table — see the .def rows for why
  // they are not on the per-frame shadow table.
  rhi::ComputePipeline opennessDirty_, opennessRefresh_;
  // The glow field (sim_glow.wgsl, src/sim/world.h kGlowBytes). Two stages of
  // the dirty walk plus the rolling refresh; same standing as the openness pair
  // above — render-path passes recorded on the TICK table.
  rhi::ComputePipeline glowSrc_, glowField_, glowRefresh_;
  rhi::ComputePipeline pageFill_;   // JITTER page materialization (world.h)
  // Shadow cache (shadow_resolve.wgsl): `prepare` turns last frame's request
  // count into a dispatch size, `resolve` casts one media-blind shadow ray per
  // requested patch. Render-path passes, encoded by EncodeShadowResolve rather
  // than on the tick table — they run per FRAME, not per tick.
  rhi::ComputePipeline shadowPrepare_, shadowResolve_;
  // The far cascade's per-level sky bound (shadow_resolve.wgsl skyTop*).
  rhi::ComputePipeline skyTopClear_, skyTopReduce_;
  // The ray-start map (ray_start.wgsl rayStartTrace / rayStartMin).
  rhi::ComputePipeline rayStartTrace_, rayStartMin_;
  // The rain shadow map (rain_map.wgsl rainMapPrep / rainMapBuild).
  rhi::ComputePipeline rainMapPrep_, rainMapBuild_;
  // The gust streaks' update (wind_streak.wgsl `update`, per-frame table).
  rhi::ComputePipeline windStreak_;
  // The god-ray sun visibility volume (godray_vis.wgsl `godrayVis`).
  rhi::ComputePipeline godrayVis_;
  // The GI gather cache's refresh (gi_gather.wgsl `giPrepare` / `giGather`).
  rhi::ComputePipeline giPrepare_, giGather_;
  rhi::ShaderModule shadowModule_;
  // Whether the cache is live this run. Recomputed in Init and ReloadShaders
  // from (device capability AND render.shadowCache), so F5 flips it with the
  // shader recompile that changes raymarch.wgsl's SHADOW_CACHE const — the two
  // MUST move together, or the fragment shader registers patches nothing
  // resolves (all shadows stale) or the resolve pass runs against a shader that
  // never asks (wasted dispatch, and a request list that never drains).
  bool shadowCacheOn_ = false;
  // The clouds (cloud.wgsl; world.h kCloud*). Six pipelines on shadowPL_ and
  // the render-private buffers they write. cloudNoise_ is baked once per
  // pipeline build (cloudBaked_ false = the next EncodeShadowResolve records
  // the bake row); the rest are per frame.
  rhi::ComputePipeline cloudNoise_, cloudWeather_, cloudShadow_, cloudEnv_,
      cloudMarch_, cloudResolve_;
  rhi::Buffer cloudNoiseBuf_, cloudWeatherBuf_, cloudMapsBuf_, cloudRawBuf_,
      cloudHistBuf_;
  uint64_t cloudPixels_ = 0;
  bool cloudBaked_ = false;
  // Water bodies (sim_waterbody.wgsl): quiescence probe, drain ledger,
  // adoption reduce, surface shave — docs/PLAN_water_master.md components 3-5.
  rhi::ComputePipeline waterQuiet_, waterLedger_, waterReduce_, waterShave_;
  rhi::ComputePipeline waterDrain_, waterHole_;   // M3, components 6 + 7
  rhi::ComputePipeline waterSweep_, waterSplit_;  // M5, components 2 + 10
  rhi::ComputePipeline waterRelevel_, waterSurface_;  // W1, relevel
  rhi::ComputePipeline waterFlux_;                    // W2, surface momentum
  rhi::ComputePipeline fluidSpawn_, fluidMark_, fluidAlloc_, fluidClear_,
      fluidP2g_, fluidP2g2_, fluidGridUp_, fluidG2p_;
  // The excite/settle seam (sim_fluid_seam.wgsl).
  rhi::ComputePipeline fluidCompactCount_, fluidCompactScan_,
      fluidCompactScatter_, fluidExciteDetect_, fluidExciteScan_,
      fluidExciteEmit_, fluidPTick_, fluidSettleJudge_, fluidSettleScan_,
      fluidSettleBin_, fluidSettleCheck_, fluidSettleCommit_, fluidSettleKill_,
      fluidConsumeApply_, fluidStainApply_, fluidMirrorFold_, fluidCellClear_;
  rhi::RenderPipeline raymarch_, particleDraw_, spriteDraw_, bodyDraw_, bodyDepth_,
      microBodyDraw_, debugBoxDraw_, pourMarkerDraw_, debugWindDraw_, debugCurrentDraw_,
      fluidDraw_, windStreakDraw_;
  rhi::ShaderModule raymarchModule_, debrisModule_, microBodyModule_,
      debugLineModule_, debugWindModule_, debugCurModule_, windStreakModule_;
  // ---- the specialized raymarch (W2-A) --------------------------------------
  // `raymarchLeanModule_` is raymarchModule_'s assembled source with the three
  // SPEC_* consts flipped to false (BuildRaymarchVariant). `raymarchLean_` is
  // the pipeline built from it, on a background thread like the far cascades,
  // published on the main thread by PollRaymarchVariant. INVALID until then,
  // and DrawWorld treats an invalid handle as "not available", never as an
  // error: a variant that failed to compile is a frame drawn by the universal
  // pipeline, which is the correct picture in every case.
  rhi::ShaderModule raymarchLeanModule_;
  rhi::RenderPipeline raymarchLean_;
  std::future<rhi::RenderPipeline> rayLeanFuture_;
  bool rayLeanPublished_ = true;   // "nothing pending", the pre-build state
  bool deferRayVariantOk_ = false;
  bool forceUniversalRay_ = false;
  bool rayStartOff_ = false;
  bool rainMapOff_ = false;
  rhi::TextureFormat targetFormat_ = rhi::TextureFormat::Undefined;

  rhi::Texture depthTex_;
  rhi::TextureView depthView_;
  uint32_t depthW_ = 0, depthH_ = 0;
  // SECOND depth target, for a pass whose size is NOT the swapchain's — today
  // the character panel's avatar portrait (main.cpp). EnsureDepth caches on
  // size alone, so two differently-sized passes in one frame would destroy and
  // recreate a full-screen depth texture TWICE PER FRAME while the panel is
  // open. One extra cache, keyed independently, is the whole fix.
  rhi::Texture auxDepthTex_;
  rhi::TextureView auxDepthView_;
  uint32_t auxDepthW_ = 0, auxDepthH_ = 0;
  // THIRD depth target: the native-size UI pass over a scaled world frame
  // (BeginOverlayRenderPass).
  rhi::Texture overlayDepthTex_;
  rhi::TextureView overlayDepthView_;
  uint32_t overlayDepthW_ = 0, overlayDepthH_ = 0;

  // ---- TAA (taa.wgsl) ------------------------------------------------------
  // taaColor_/taaDepth_ are the render-resolution frame copied out of its
  // attachments; taaHist_ is the NATIVE-resolution accumulator, PING-PONGED
  // because the resolve reads it bilinearly at a reprojected position (other
  // pixels' texels) while writing its own. One buffer would be a race between
  // invocations of one draw — the dirtyIn/dirtyOut argument, applied to a frame.
  rhi::Buffer taaColor_, taaDepth_, taaHist_[2], taaUBO_;
  rhi::BindGroupLayout taaBGL_;
  rhi::PipelineLayout taaPL_;
  rhi::BindGroup taaBG_[2];
  rhi::RenderPipeline taaResolve_;
  rhi::ShaderModule taaModule_;
  uint32_t taaRW_ = 0, taaRH_ = 0, taaNW_ = 0, taaNH_ = 0;
  int taaPage_ = 0;
  bool taaReset_ = true;
  bool taaAvailable_ = false;
  bool taaHavePrev_ = false;
  TaaCamera taaPrevCam_{};

  // ---- shading-LOD filter (denoise.wgsl) -----------------------------------
  // dnColor_ is refilled by a copy between iterations (the pass reads it and
  // renders into the frame texture, which is then copied back); dnDepth_ is
  // captured once per frame. One UBO and one bind group per iteration, because
  // the a-trous step differs per pass and a uniform cannot change inside one
  // command buffer without a second buffer.
  rhi::Buffer dnColor_, dnDepth_, dnUBO_[kDenoiseMaxIters];
  rhi::BindGroupLayout dnBGL_;
  rhi::PipelineLayout dnPL_;
  rhi::BindGroup dnBG_[kDenoiseMaxIters];
  rhi::RenderPipeline dnPipe_;
  rhi::ShaderModule dnModule_;
  rhi::Texture dnDepthTex_;        // the pass's (unused) depth attachment
  rhi::TextureView dnDepthView_;
  uint32_t dnW_ = 0, dnH_ = 0;
  uint32_t dnIters_ = 0;           // what the last WriteDenoiseParams asked for
  bool denoiseAvailable_ = false;

  // Two bind groups: page 0 reads dirty[0]/writes dirty[1], page 1 reversed.
  // Particle groups follow the same paging (b0 = read page, b1 = write page).
  rhi::BindGroup simBG_[2], simSlimBG_[2], particleBG_[2];
  // Gas pages exactly like particleBG_: binding 0 is the read page.
  rhi::BindGroup gasBG_[2];
  // C_GAS latch state — see the block in EncodeTick.
  static constexpr uint32_t kGasIdleTicks = 8;
  // How long the RENDER flag (RenderParams bit 3) is held past the last
  // snapshot that saw gas. Much longer than kGasIdleTicks because it is a
  // FRAME-RATE-facing latch and its failure mode is different: the pass latch
  // going false one tick early costs a tick of simulation, while the render
  // flag going false one frame early makes a plume blink. One second of hold
  // against a ring that is a handful of ticks latent, so it cannot flicker.
  static constexpr uint32_t kGasSeenTicks = 60;
  uint32_t gasSeenHold_ = 0;
  uint32_t gasLive_ = 0;
  uint32_t gasSpawnsThisTick_ = 0;
  // Far fire-plume emitters currently in world_->gasFarEmit. NOT reset per
  // tick, unlike gasSpawnsThisTick_: the buffer is uploaded only when the
  // list CHANGES, so the count is a standing fact about the buffer rather
  // than a count of this tick's inputs.
  uint32_t farPlumeCount_ = 0;
  uint32_t farPlumeWideCount_ = 0;
  uint32_t gasIdleTicks_ = kGasIdleTicks;
  // fluidBG_ pages like particleBG_: binding 6 is THIS tick's particle write
  // page (next tick's read page), the splash droplets' destination. Binding 0
  // is the tick's WORKING fluid particle buffer, fluidParticles[1 - page]
  // (the seam's compaction target — same convention as ParticlesWrite).
  // fluidSeamBG_ additionally binds fluidParticles[page] as the compaction
  // source.
  rhi::BindGroup renderBG_, renderPartBG_[2], farBG_, microBodyBG_, fluidBG_[2],
      fluidSeamBG_[2], shadowBG_;
  // Water veil: renderBG_ binds veilBuf_ at 24 (grown by EnsureVeil, never
  // shrunk); renderBGNoVeil_ binds veilNone_, one zeroed record. veilLive_ is
  // true between a pass's DrawWorld and the next Begin*RenderPass.
  rhi::Buffer veilBuf_, veilNone_;
  // The ray-start map: bound at 32 in renderBGL_ (fragment, read) and at
  // 21 in shadowBGL_ (compute, written). rayStartW_/H_ = the largest target
  // it is sized for; 0 until the first world pass, and no row records.
  rhi::Buffer rayStartBuf_;
  // The gust streaks' fixed particle pool (world.h kWindStreakCap).
  rhi::Buffer windStreakBuf_;
  // The god-ray sun visibility volume (godray_vis.wgsl; pass_table.h
  // kGodVis*): fixed size, made at Init. 27 in shadowBGL_ (compute, written),
  // 38 in renderBGL_ (fragment, read).
  rhi::Buffer godVisBuf_;
  // The GI gather request list (gi_gather.wgsl; pass_table.h kGiReq*): fixed
  // size, zeroed at Init (the dedup bitmap must start empty). 28 in
  // shadowBGL_ (compute), 39 in renderBGL_ (fragment, appends). giArgsBuf_ is
  // the indirect-only copy of its header's args words.
  rhi::Buffer giReqBuf_;
  rhi::Buffer giArgsBuf_;
  // The buffer EnsureRayStart last replaced, kept alive one growth longer
  // because the frame that grew it had already recorded the prepass against it.
  rhi::Buffer rayStartPrev_;
  // The rain shadow map (rain_map.wgsl; world.h kRainMap*): fixed size, made
  // at Init. 22 in shadowBGL_ (compute, written), 33 in renderBGL_ (fragment).
  rhi::Buffer rainMapBuf_;
  uint32_t rayStartW_ = 0, rayStartH_ = 0;
  uint64_t veilPixels_ = 0;
  rhi::BindGroup renderBGNoVeil_;
  bool veilLive_ = false;
  // DrawWorld wrote the veil and no split has been taken yet (VeilReaderBG).
  bool veilSplitPending_ = false;
  int page_ = 0;

  // ---- settled-tick skip state (§3.4) -------------------------------------
  // The last tick a CPU input could have dirtied a chunk. A snapshot licenses
  // the skip only when it is stamped at or after this (Simulation::
  // NoteSnapshot), so a stale snapshot can never speak for a newer write.
  //
  // Starts at 0 rather than a never-reachable sentinel, and that is safe
  // because it is not what gates the FIRST skip: settledProven_ starts false,
  // and only a snapshot reporting zero active chunks can set it. Every path
  // that creates a world — worldgen, load, genList — calls NoteWakeAll() and
  // re-stamps this with a real tick anyway.
  uint32_t lastDirtyTick_ = 0;
  uint32_t curTick_ = 0;        // tick being encoded (NoteTickInputs)
  bool settledProven_ = false;  // a fresh snapshot showed 0 active chunks
  bool caSkipped_ = false;      // last EncodeTick omitted the CA rows
  bool fluidSeamRecorded_ = false;  // last EncodeTick recorded the MPM seam
  uint64_t caSkipCount_ = 0;    // how many ticks skipped (measurement only)
  bool caForced_ = false;       // SetCaForced / SANDVOX_CA_FORCE (test only)
};
