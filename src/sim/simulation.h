#pragma once
#include <string>
#include <vector>

#include "gpu/passtimer.h"
#include "sim/materials.h"
#include "sim/microbody.h"
#include "sim/microvox.h"
#include "sim/pass_table.h"
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
            const std::string& shaderDir);

  // Recompile all WGSL from disk; returns false (keeping old pipelines) on
  // compile error.
  bool ReloadShaders(const rhi::Device& device);
  // Re-upload the material + reaction tables (JSON hot reload).
  void UploadTables(const rhi::Queue& queue, const std::vector<MaterialDef>& mats,
                    const std::vector<ReactionGpu>& reactions);
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
  // Publish the ART palette: per-voxel skin colours from loaded prefabs, which
  // are NOT material colours (a creature is one material all over and painted
  // per voxel — sim/voxload.h). They live in reserved material-table entries
  // (kArtPaletteBaseGpu, world.h), so they are CACHED here and re-applied by
  // UploadTables: a materials hot-reload rewrites the whole table and would
  // otherwise wipe them, repainting every mob in its raw material colours.
  // Render-only; nothing here can reach a world cell or the hash.
  void SetArtPalette(const rhi::Queue& queue, const std::vector<uint32_t>& rgb);

  void EncodeWorldgen(const rhi::CommandEncoder& enc, bool denseGen = true);
  // Generate `count` streamed-in chunks whose SLOT indices the caller wrote to
  // world.genList (and whose count + window origin are in tickUBO).
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
  // Standalone whole-world hash pass (save/load verification): caller writes
  // TickParams with hashEnable=1 first, reads world.hash after submit.
  void EncodeHashOnly(const rhi::CommandEncoder& enc);

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
                  uint32_t waterDrainBodies = 0);

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
  // mostly staleness: the snapshot is one tick latent (DESIGN.md §2) and can be
  // older when the readback ring is saturated, so `SettledSnapshot` requires
  // the snapshot to be NEWER than the last tick anything could have dirtied,
  // never merely non-zero.
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
  // raster geometry depth-tests against it). Caller draws UI into same pass.
  rhi::RenderPass BeginRenderPass(const rhi::CommandEncoder& enc,
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
  // Wind slope-field overlay (docs/RESEARCH_wind.md §4.8, F4): one arrow per
  // lattice point around the camera, oriented and coloured by `windAt` — the
  // same field function the foliage sway samples. `arrows` is
  // WindDebugArrowCount(tuning) (sim/wind.h); 0 draws nothing at all.
  //
  // Nothing is uploaded for this: the vertex shader derives every lattice
  // point from its instance index and R.camPos, so there is no arrow buffer,
  // no per-arrow CPU work, and no new bind group.
  void DrawWindField(const rhi::RenderPass& pass, uint32_t arrows);
  // The current field's arrows (docs/PLAN_water_master.md component 8).
  void DrawCurrentField(const rhi::RenderPass& pass, uint32_t arrows);
  void DrawBodies(const rhi::RenderPass& pass, uint32_t voxInstances);
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

  static constexpr rhi::TextureFormat kDepthFormat = rhi::TextureFormat::Depth32Float;

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

 private:
  bool BuildPipelines(const rhi::Device& device, std::string* err);
  void EnsureDepth(uint32_t width, uint32_t height);
  void EnsureRenderPipelines(rhi::TextureFormat format);
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

  World* world_ = nullptr;
  rhi::Device device_;
  std::string shaderDir_;
  rhi::Buffer materialBuf_;
  rhi::Buffer reactionBuf_;
  // Art palette RGB (0x00RRGGBB), indexed from kArtPaletteBaseGpu. Cached so a
  // materials hot-reload can restore it — see SetArtPalette.
  std::vector<uint32_t> artPalette_;
  // Static micro-detail (render-only). Deliberately NOT in any sim bind group.
  rhi::Buffer microTableBuf_, microPoolBuf_;
  // Dynamic micro BODIES (render-only, same doctrine): per-def limb models, the
  // shared brick pool, and this frame's compacted (slot, model) draw list.
  rhi::Buffer mbModelBuf_, mbPoolBuf_, mbInstBuf_;

  // simSlimBGL_ mirrors simBGL_ bindings 0..4 only — the particle/explosion
  // pipelines pair it with particleBGL_ to stay under the 16-storage-buffer
  // per-stage pipeline-layout limit (Dawn counts layout entries, not usage).
  rhi::BindGroupLayout simBGL_, simSlimBGL_, particleBGL_, renderBGL_, renderPartBGL_,
      farBGL_, microBodyBGL_, fluidBGL_, fluidSeamBGL_;
  rhi::PipelineLayout simPL_, simPL2_, renderPL_, farPL_, microBodyPL_, fluidPL_,
      fluidSeamPL_;
  rhi::ComputePipeline worldgen_, worldgenList_, mutate_, mutateCells_, compact_,
      compactNext_, step_, occupancy_, occupancyDirty_, pick_;
  // Wind primitive footprint wake (sim_mutate.wgsl `windWake`) — see
  // docs/RESEARCH_wind.md §4.3.
  rhi::ComputePipeline windWake_;
  rhi::ComputePipeline explodeMark_, explodeApply_, pArgs1_, pSpawn_, pIntegrate_,
      pArgs2_, pResolve_;
  rhi::ComputePipeline farFill_, farDown_;
  rhi::ComputePipeline pageFill_;   // JITTER page materialization (world.h)
  // Water bodies (sim_waterbody.wgsl): quiescence probe, drain ledger,
  // adoption reduce, surface shave — docs/PLAN_water_master.md components 3-5.
  rhi::ComputePipeline waterQuiet_, waterLedger_, waterReduce_, waterShave_;
  rhi::ComputePipeline waterDrain_, waterHole_;   // M3, components 6 + 7
  rhi::ComputePipeline fluidSpawn_, fluidMark_, fluidAlloc_, fluidClear_,
      fluidP2g_, fluidP2g2_, fluidGridUp_, fluidG2p_;
  // The excite/settle seam (sim_fluid_seam.wgsl).
  rhi::ComputePipeline fluidCompactCount_, fluidCompactScan_,
      fluidCompactScatter_, fluidExciteDetect_, fluidExciteScan_,
      fluidExciteEmit_, fluidPTick_, fluidSettleJudge_, fluidSettleScan_,
      fluidSettleBin_, fluidSettleCheck_, fluidSettleCommit_, fluidSettleKill_,
      fluidConsumeApply_, fluidStainApply_, fluidMirrorFold_, fluidCellClear_;
  rhi::RenderPipeline raymarch_, particleDraw_, spriteDraw_, bodyDraw_,
      microBodyDraw_, debugBoxDraw_, debugWindDraw_, debugCurrentDraw_,
      fluidDraw_;
  rhi::ShaderModule raymarchModule_, debrisModule_, microBodyModule_,
      debugLineModule_, debugWindModule_, debugCurModule_;
  rhi::TextureFormat targetFormat_ = rhi::TextureFormat::Undefined;

  rhi::Texture depthTex_;
  rhi::TextureView depthView_;
  uint32_t depthW_ = 0, depthH_ = 0;

  // Two bind groups: page 0 reads dirty[0]/writes dirty[1], page 1 reversed.
  // Particle groups follow the same paging (b0 = read page, b1 = write page).
  rhi::BindGroup simBG_[2], simSlimBG_[2], particleBG_[2];
  // fluidBG_ pages like particleBG_: binding 6 is THIS tick's particle write
  // page (next tick's read page), the splash droplets' destination. Binding 0
  // is the tick's WORKING fluid particle buffer, fluidParticles[1 - page]
  // (the seam's compaction target — same convention as ParticlesWrite).
  // fluidSeamBG_ additionally binds fluidParticles[page] as the compaction
  // source.
  rhi::BindGroup renderBG_, renderPartBG_[2], farBG_, microBodyBG_, fluidBG_[2],
      fluidSeamBG_[2];
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
  uint64_t caSkipCount_ = 0;    // how many ticks skipped (measurement only)
  bool caForced_ = false;       // SetCaForced / SANDVOX_CA_FORCE (test only)
};
