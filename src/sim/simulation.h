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
                  uint32_t waterDrainBodies = 0,
                  // M5: the scheduled sweep's body slot, or kWaterBodyCap
                  // for "nothing re-derives this tick" — which is the
                  // default and the state of every untouched lake.
                  uint32_t waterSweepSlot = 0xFFFFFFFFu);

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

  // Publish a finished background compile and return true EXACTLY ONCE: on the
  // call that made the pipelines live. That is the caller's cue to
  // FarField::FullRefill — every fill queued while they were missing was
  // popped by PrepareTick and dropped, so the cascades are empty and only a
  // wholesale refill puts a horizon back. Never blocks.
  bool PollFarPipelines();
  bool FarPipelinesReady() const {
    return farReady_.load(std::memory_order_acquire);
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
    rhi::ComputePipeline fill, down;
  };
  // Move the future's result onto farFill_/farDown_. Main thread only.
  void PublishFarPipelines();
  // Blocks unless the caller opted into deferral. Called from EncodeFarFill
  // with work to do — the one point where a caller is about to depend on
  // cascade CONTENT. Recording a far row against a pipeline that does not
  // exist yet is legal (the recorder skips a null pipeline); what is not
  // acceptable is a checked output that silently depends on driver timing.
  void EnsureFarPipelines() {
    if (deferFarOk_ || farReady_.load(std::memory_order_acquire)) return;
    WaitForFarPipelines();
  }
  // The full and slim sim bind groups, both pages. Called by Init and again by
  // UploadEnvironment when a table buffer had to be recreated.
  void BuildSimBindGroups(const rhi::Device& device);
  void EnsureDepth(uint32_t width, uint32_t height);
  void EnsureAuxDepth(uint32_t width, uint32_t height);
  void EnsureOverlayDepth(uint32_t width, uint32_t height);
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

  World* world_ = nullptr;
  rhi::Device device_;
  std::string shaderDir_;
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
      farBGL_, microBodyBGL_, fluidBGL_, fluidSeamBGL_, shadowBGL_;
  rhi::PipelineLayout simPL_, simPL2_, renderPL_, farPL_, microBodyPL_, fluidPL_,
      fluidSeamPL_, shadowPL_;
  rhi::ComputePipeline worldgen_, worldgenList_, mutate_, mutateCells_, compact_,
      compactNext_, step_, occupancy_, occupancyDirty_, pick_;
  // Wind primitive footprint wake (sim_mutate.wgsl `windWake`) — see
  // docs/RESEARCH_wind.md §4.3.
  rhi::ComputePipeline windWake_;
  rhi::ComputePipeline explodeMark_, explodeApply_, pArgs1_, pSpawn_, pIntegrate_,
      pArgs2_, pResolve_;
  // Live only after PublishFarPipelines. Until then both are INVALID handles
  // and the recorder skips their rows (vk_record.cpp's null-pipeline continue).
  rhi::ComputePipeline farFill_, farDown_;
  // The background compile. Valid between BuildPipelines and the publish;
  // `farPublished_` and `deferFarOk_` are main-thread-only, `farReady_` is the
  // one field any other thread may observe.
  std::future<FarPipelines> farFuture_;
  std::atomic<bool> farReady_{false};
  bool farPublished_ = false;
  bool deferFarOk_ = false;
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
  rhi::ShaderModule shadowModule_;
  // Whether the cache is live this run. Recomputed in Init and ReloadShaders
  // from (device capability AND render.shadowCache), so F5 flips it with the
  // shader recompile that changes raymarch.wgsl's SHADOW_CACHE const — the two
  // MUST move together, or the fragment shader registers patches nothing
  // resolves (all shadows stale) or the resolve pass runs against a shader that
  // never asks (wasted dispatch, and a request list that never drains).
  bool shadowCacheOn_ = false;
  // Water bodies (sim_waterbody.wgsl): quiescence probe, drain ledger,
  // adoption reduce, surface shave — docs/PLAN_water_master.md components 3-5.
  rhi::ComputePipeline waterQuiet_, waterLedger_, waterReduce_, waterShave_;
  rhi::ComputePipeline waterDrain_, waterHole_;   // M3, components 6 + 7
  rhi::ComputePipeline waterSweep_, waterSplit_;  // M5, components 2 + 10
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
      fluidSeamBG_[2], shadowBG_;
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
