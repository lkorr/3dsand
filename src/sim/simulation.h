#pragma once
#include <string>
#include <vector>

#include <webgpu/webgpu_cpp.h>

#include "sim/materials.h"
#include "sim/microbody.h"
#include "sim/microvox.h"
#include "sim/world.h"

// Owns the compute pipelines + bind groups and records the fixed-tick GPU
// work: mutate -> explosions -> 27 color passes -> particles -> occupancy/hash
// -> pick. Also owns the render pipelines (raymarch + instanced-cube raster
// for particles/sprites) — they read the same buffers, composited through a
// shared reversed-Z depth attachment.
class Simulation {
 public:
  bool Init(const wgpu::Device& device, World& world,
            const std::vector<MaterialDef>& mats,
            const std::vector<ReactionGpu>& reactions, const MicroSet& micro,
            const std::string& shaderDir);

  // Recompile all WGSL from disk; returns false (keeping old pipelines) on
  // compile error.
  bool ReloadShaders(const wgpu::Device& device, const wgpu::Instance& instance);
  // Re-upload the material + reaction tables (JSON hot reload).
  void UploadTables(const wgpu::Queue& queue, const std::vector<MaterialDef>& mats,
                    const std::vector<ReactionGpu>& reactions);
  // Re-upload the static micro-detail brick pool + per-material table (rides
  // the same R hot-reload as materials — sim/microvox.h). Render-only data:
  // these buffers are bound to the raymarch pipeline and to nothing else.
  void UploadMicro(const wgpu::Queue& queue, const MicroSet& micro);
  // Re-upload the dynamic micro-BODY model table + brick pool (mob defs load /
  // hot reload — sim/microbody.h). Render-only, same doctrine as UploadMicro.
  void UploadMicroBodies(const wgpu::Queue& queue, const MicroBodySet& set);
  // Publish the ART palette: per-voxel skin colours from loaded prefabs, which
  // are NOT material colours (a creature is one material all over and painted
  // per voxel — sim/voxload.h). They live in reserved material-table entries
  // (kArtPaletteBaseGpu, world.h), so they are CACHED here and re-applied by
  // UploadTables: a materials hot-reload rewrites the whole table and would
  // otherwise wipe them, repainting every mob in its raw material colours.
  // Render-only; nothing here can reach a world cell or the hash.
  void SetArtPalette(const wgpu::Queue& queue, const std::vector<uint32_t>& rgb);

  void EncodeWorldgen(const wgpu::CommandEncoder& enc);
  // Generate `count` streamed-in chunks whose SLOT indices the caller wrote to
  // world.genList (and whose count + window origin are in tickUBO).
  void EncodeGenList(const wgpu::CommandEncoder& enc, uint32_t count);
  // Post-load reset: clears transient state (hash/particles/claims) and
  // rebuilds occupancy over freshly uploaded voxels. Caller has already
  // written the voxel + dirty buffers (see worldio.cpp).
  void EncodeLoadReset(const wgpu::CommandEncoder& enc);
  // Fill `count` far-field cascade level-chunks whose packed entries the
  // caller wrote to world.farList (count also in tickUBO.farCount). Render-
  // only derived data — safe to encode anywhere in the tick (DESIGN.md §9).
  void EncodeFarFill(const wgpu::CommandEncoder& enc, uint32_t count);
  // Standalone whole-world hash pass (save/load verification): caller writes
  // TickParams with hashEnable=1 first, reads world.hash after submit.
  void EncodeHashOnly(const wgpu::CommandEncoder& enc);

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
  void EncodeWakeAll(const wgpu::Queue& queue);

  // One 30 Hz tick. Caller writes tickUBO/opsBuf/expOps and zeroes the write-
  // page particle count via queue.WriteBuffer first, then submits the encoder
  // produced here before encoding the next tick. particlesActive lets a
  // settled world skip the particle passes entirely; the caller must derive it
  // ONLY from tick-deterministic inputs (see main.cpp) or determinism breaks.
  void EncodeTick(const wgpu::CommandEncoder& enc, uint32_t opsCount,
                  bool hashEnable, uint32_t expCount, bool particlesActive,
                  uint32_t cellCount, uint32_t spawnCount = 0);

  // Render pass with the shared depth target (raymarch writes frag_depth,
  // raster geometry depth-tests against it). Caller draws UI into same pass.
  wgpu::RenderPassEncoder BeginRenderPass(const wgpu::CommandEncoder& enc,
                                          const wgpu::TextureView& target,
                                          wgpu::TextureFormat format,
                                          uint32_t width, uint32_t height);
  void DrawWorld(const wgpu::RenderPassEncoder& pass);
  void DrawParticles(const wgpu::RenderPassEncoder& pass);
  void DrawSprites(const wgpu::RenderPassEncoder& pass, uint32_t count);
  // Collision-box debug overlay: one oriented wireframe box per physics body.
  // Drawn LAST of the world passes (after the micro bodies, before ImGui) with
  // depth testing off, so a collider is visible through whatever contains it.
  // `count` of 0 draws nothing at all — the overlay is free when it is off.
  void DrawDebugBoxes(const wgpu::RenderPassEncoder& pass, uint32_t count);
  void DrawBodies(const wgpu::RenderPassEncoder& pass, uint32_t voxInstances);
  // Microvoxel bodies (PLAN §C): one 36-vertex OBB per entry in `insts`, drawn
  // between DrawBodies and DrawSprites. `insts` is the compacted (slot, model)
  // list built by the caller from the frame's body slots; an empty list draws
  // nothing at all, so a world with no micro bodies pays zero.
  void DrawMicroBodies(const wgpu::RenderPassEncoder& pass,
                       const wgpu::Queue& queue,
                       const std::vector<MicroBodyInstGpu>& insts);

  static constexpr wgpu::TextureFormat kDepthFormat = wgpu::TextureFormat::Depth32Float;

  // Which dirty buffer the tick just encoded writes as "active next tick".
  const wgpu::Buffer& DirtyNext() const { return world_->dirty[1 - page_]; }
  // The dirty buffer the NEXT tick will read (valid after FlipPage) — used by
  // the selftest to count active chunks in a settled world.
  const wgpu::Buffer& DirtyActive() const { return world_->dirty[page_]; }
  // Particle page semantics: EncodeTick reads particles[Page()] and writes
  // particles[1 - Page()]; after FlipPage, Page() is the live buffer (what the
  // renderer draws and the next tick reads).
  uint32_t Page() const { return (uint32_t)page_; }
  // Call once after each EncodeTick has been submitted.
  void FlipPage();

 private:
  bool BuildPipelines(const wgpu::Device& device, std::string* err);
  void EnsureDepth(uint32_t width, uint32_t height);
  void EnsureRenderPipelines(wgpu::TextureFormat format);
  // Stamp the cached art palette into a material table being (re)built.
  void ApplyArtPalette(std::vector<MaterialGpu>& table) const;

  World* world_ = nullptr;
  wgpu::Device device_;
  std::string shaderDir_;
  wgpu::Buffer materialBuf_;
  wgpu::Buffer reactionBuf_;
  // Art palette RGB (0x00RRGGBB), indexed from kArtPaletteBaseGpu. Cached so a
  // materials hot-reload can restore it — see SetArtPalette.
  std::vector<uint32_t> artPalette_;
  // Static micro-detail (render-only). Deliberately NOT in any sim bind group.
  wgpu::Buffer microTableBuf_, microPoolBuf_;
  // Dynamic micro BODIES (render-only, same doctrine): per-def limb models, the
  // shared brick pool, and this frame's compacted (slot, model) draw list.
  wgpu::Buffer mbModelBuf_, mbPoolBuf_, mbInstBuf_;

  // simSlimBGL_ mirrors simBGL_ bindings 0..4 only — the particle/explosion
  // pipelines pair it with particleBGL_ to stay under the 16-storage-buffer
  // per-stage pipeline-layout limit (Dawn counts layout entries, not usage).
  wgpu::BindGroupLayout simBGL_, simSlimBGL_, particleBGL_, renderBGL_, renderPartBGL_,
      farBGL_, microBodyBGL_;
  wgpu::PipelineLayout simPL_, simPL2_, renderPL_, farPL_, microBodyPL_;
  wgpu::ComputePipeline worldgen_, worldgenList_, mutate_, mutateCells_, compact_,
      compactNext_, step_, occupancy_, occupancyDirty_, pick_;
  wgpu::ComputePipeline explodeMark_, explodeApply_, pArgs1_, pSpawn_, pIntegrate_,
      pArgs2_, pResolve_;
  wgpu::ComputePipeline farFill_, farDown_;
  wgpu::RenderPipeline raymarch_, particleDraw_, spriteDraw_, bodyDraw_,
      microBodyDraw_, debugBoxDraw_;
  wgpu::ShaderModule raymarchModule_, debrisModule_, microBodyModule_,
      debugLineModule_;
  wgpu::TextureFormat targetFormat_ = wgpu::TextureFormat::Undefined;

  wgpu::Texture depthTex_;
  wgpu::TextureView depthView_;
  uint32_t depthW_ = 0, depthH_ = 0;

  // Two bind groups: page 0 reads dirty[0]/writes dirty[1], page 1 reversed.
  // Particle groups follow the same paging (b0 = read page, b1 = write page).
  wgpu::BindGroup simBG_[2], simSlimBG_[2], particleBG_[2];
  wgpu::BindGroup renderBG_, renderPartBG_[2], farBG_, microBodyBG_;
  int page_ = 0;
};
