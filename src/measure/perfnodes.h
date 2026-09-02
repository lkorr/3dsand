// perfnodes.h — the ONE list of engine components the performance page bills
// time to, and the map from a measured source onto each of them.
//
// WHY THIS FILE EXISTS
// --------------------
// The Performance tab wants one sentence per component: "caLoop cost 4.2 ms of
// this frame, over 1,847 chunks". Three separate things have to agree for that
// sentence to be true:
//
//   1. the ARCH_NODES key in assets/tuner.html, so the bar the user clicks is
//      the same box they clicked on the Engine map;
//   2. the pass ROW names in sim/pass_table.def, so a GPU number is attributed
//      to the system that actually issued the dispatch;
//   3. the CPU scope the frame loop opens, so a CPU number is attributed to the
//      call that actually ran.
//
// Three lists that must agree is the drift CLAUDE.md keeps warning about, so
// this is one list and the other two are CHECKED against it:
// scripts/check_invariants.py fails if a `node` here is not an ARCH_NODES key,
// or if a name in `passKeys` is not a PASS() row in pass_table.def. A pass row
// that bills to nothing is reported too — an unattributed dispatch is time that
// silently vanishes from the page, which is the one failure mode a performance
// view must not have.
//
// MEASUREMENT-ONLY. The frame loop consults kPerfNodes only when telemetry is
// live (--telemetry) and the harness only under --perf. Nothing here is on the
// default tick path and nothing here can move the world hash: a CPU scope is a
// clock read, and a GPU timestamp is a pass-descriptor attachment that observes
// a dispatch without reordering it (gpu/passtimer.h says the same, and the
// --perf harness asserts the hash matches an untimed run).

#pragma once

#include <cstdint>

namespace sandvox {

// Which processor the row's time is spent on. A node can be Both — `mutQueue`
// costs CPU to assemble the op stream and GPU to apply it — and the page shows
// the two as separate bars, because "the mutation queue is slow" means very
// different things in the two cases.
enum class PerfSide : uint8_t { Cpu, Gpu, Both };

// The CPU scopes the frame loop and the harness open. One enumerator per
// timed region; kPerfNodes maps them onto architecture nodes below.
//
// ORDER IS THE DISPLAY ORDER inside a frame — keep it in tick order, because
// the stacked frame-timeline chart draws the bands in this sequence and a
// scrambled order makes a readable chart into a plate of spaghetti.
// A SCOPE MUST BE MEASURED, NEVER DERIVED. `Input` used to be the frame's
// residual (wallMs minus everything else), which meant every unmeasured span in
// the engine reported as input polling — see the header comment in
// measure/perfscope.h. If a span is not on the clock it belongs in `Other`,
// which is the ONE row here that is allowed to be a subtraction and is named so
// that reading it as a system is impossible.
enum class PerfScope : uint8_t {
  Input,        // input sampling, camera, UI intent
  Stream,       // toroidal window shift, chunk fetch/evict, page fills
  GameLogic,    // brush/spell/melee/item drivers that emit ops for this tick
  WaterBody,    // WaterBodySystem::Tick (CPU half of the lake registry)
  Upload,       // op-stream assembly + WriteBuffer of the MutationQueue
  PageTableCpu, // the CPU half of the page table: the dirty mirror, materialize
  Encode,       // Simulation::EncodeTick — walking the pass table
  Submit,       // command-buffer Finish + queue submit
  Physics,      // Jolt step
  PostStep,     // debris/mob/avatar post-step, player push-out
  Readback,     // snapshot map callbacks, mirror rebuild (never blocks)
  ReadbackStall,// the paged-mirror staleness fallback: a BLOCKING fence wait
  Audio,        // cue dispatch + spatializer feed
  RenderCpu,    // render-pass encode: draw calls, instance buffers, overlay
  Present,      // AcquireFrame + Present (this is where vsync waits land)
  Other,        // THE RESIDUAL: frame time no scope above claimed. Not a system.
  Count
};
constexpr int kPerfScopeCount = (int)PerfScope::Count;

// A component row on the performance page.
struct PerfNodeDef {
  // ARCH_NODES key in assets/tuner.html. Checked by check_invariants.py.
  const char* node;
  // Display label. Free-standing rather than read from the Engine tab so the
  // page renders before ARCH_NODES has been consulted; check_invariants.py does
  // NOT require these to match, because the perf page sometimes wants a shorter
  // one ("CA" vs "CA — 54 Passes") in a bar chart that is 200 px wide.
  const char* label;
  // Parent ARCH_NODES key, or nullptr for a top-level row. Drives the
  // collapse/expand tree on the page; children's time sums into the parent.
  const char* parent;
  PerfSide side;
  // The CPU scope whose clock bills here, or PerfScope::Count for GPU-only.
  PerfScope scope;
  // ';'-separated PASS() row names from sim/pass_table.def whose GPU timestamps
  // bill here. Empty for CPU-only rows. Every compute row in the table must
  // appear in exactly one node's list — check_invariants.py enforces both
  // halves (unknown name here, unattributed row there).
  const char* passKeys;
  // One-line "what this is" for the tooltip. Deliberately about COST, not about
  // architecture — the Engine tab already explains what the system does, and
  // repeating it here would be a second copy to keep current.
  const char* costNote;
};

// ---------------------------------------------------------------------------
// THE TABLE.
//
// Rows are in frame order, parents before children, because the page walks it
// once and builds the tree by `parent` back-reference.
// ---------------------------------------------------------------------------
inline constexpr PerfNodeDef kPerfNodes[] = {
    // ---- simulation tick (GPU-dominant) ----
    {"simTick", "Simulation Tick", nullptr, PerfSide::Both, PerfScope::Encode,
     "", "Encode cost is CPU walking the pass table; children hold the GPU."},
    {"mutQueue", "MutationQueue", "simTick", PerfSide::Both, PerfScope::Upload,
     "mutate;mutateCells",
     "CPU assembles + uploads the op stream, GPU applies it. Scales with ops, "
     "not with world size."},
    {"explode", "Explosions", "simTick", PerfSide::Gpu, PerfScope::Count,
     "explodeMark;explodeApply",
     "O(r^2) rays over an O(r^3) volume. Two-phase, so it costs twice per blast."},
    {"compact", "Dirty Compaction", "simTick", PerfSide::Gpu, PerfScope::Count,
     "compact;compactNext",
     "Flat per-tick cost over kNumChunks. This is the pass that makes every "
     "row below it scale with activity."},
    {"caLoop", "CA (54 passes)", "simTick", PerfSide::Gpu, PerfScope::Count,
     "ca",
     "27 colours x 2 substeps over the dirty list. Divide by active chunks for "
     "the per-chunk number the compute budget is denominated in."},
    {"particleSys", "Particles", "simTick", PerfSide::Gpu, PerfScope::Count,
     "particleSpawn;particleArgs1;particleIntegrate;particleArgs2;particleResolve",
     "Scales with the live particle count, not the world. Integrate is the "
     "DDA; resolve is the atomicMax claim."},
    {"fluidSys", "MLS-MPM Fluid", "simTick", PerfSide::Gpu, PerfScope::Count,
     "fluidMark;fluidAlloc;fluidClear;fluidP2g1;fluidP2g2;fluidGridUp;fluidG2p;"
     "seam_compact_count;seam_compact_scan;seam_compact_scatter;seam_spawn;"
     "seam_consume_apply;seam_excite_detect;seam_excite_scan;seam_excite_emit;"
     "seam_cell_clear;seam_particle_tick;seam_stain_apply;seam_settle_judge;"
     "seam_settle_scan;seam_settle_bin;seam_settle_check;seam_settle_commit;"
     "seam_settle_kill;seam_mirror_fold",
     "kFluidSubsteps x the solver table per tick, plus the CA<->MPM seam. "
     "Sleeps to 0.0 ms when no water is excited."},
    {"waterBodies", "Water Bodies", "simTick", PerfSide::Both,
     PerfScope::WaterBody,
     "waterQuiet;waterLedger;waterDrain;waterReduce;waterShave;waterHole;"
     "waterSweep;waterSplit",
     "Records nothing at all while sim.waterBodyMode is 0. Cost is per BODY, "
     "not per water voxel."},
    // `compactNext` belongs to `compact`, not here — it is the second run of
    // the dirty compaction, over the dirty-OUT flags. It was listed in both,
    // which double-counted its GPU time; check_invariants.py's `perfnodes`
    // check exists because that mistake is invisible on the page (the bars
    // still agree with each other, they just stop agreeing with the frame).
    {"occupancy", "Occupancy + Hash", "simTick", PerfSide::Gpu, PerfScope::Count,
     "occupancyFull;pick_hash;occupancyDirty;pick_dirty;"
     "lr_occupancyFull;ho_occupancyFull",
     "Full-world on hash ticks, dirty-list only otherwise. The hash-tick "
     "spike every 15 ticks is this row."},
    {"wind", "Wind", "simTick", PerfSide::Gpu, PerfScope::Count, "windWake",
     "windAt() is a pure function evaluated in the kernels that need it; only "
     "the wake pass is separately timed."},
    {"readback", "Async Readback", "simTick", PerfSide::Cpu, PerfScope::Readback,
     "", "Map callbacks + the 3x3x3 CPU mirror rebuild. One tick latent, never "
     "blocking — if this is large the mirror copy is the reason. The blocking "
     "half of what this row used to hold is `Snapshot Stall`, below."},
    // SPLIT OUT of `readback` on purpose. The two used to share one row, and
    // that row read as "async readback spiked to 90 ms" after a lake was
    // disturbed — but the map pump above never blocks; what spiked was the
    // paged mirror's staleness fallback (test/support.cpp SubmitTick), which
    // WAITS for the GPU to deliver a snapshot no older than 4 ticks. A row that
    // is a fence wait and a row that is a memcpy need opposite fixes, and the
    // one that waits belongs next to `present`, not next to `mirror copy`.
    {"readbackStall", "Snapshot Stall", "simTick", PerfSide::Cpu,
     PerfScope::ReadbackStall, "",
     "BLOCKING: the tick refused to run on a snapshot older than 4 ticks and "
     "waited for the GPU to deliver one. It fires when the GPU is more than a "
     "frame behind while the loop catches up at 4 ticks/frame, so it is almost "
     "always a relabelled GPU wait — read the GPU bars for the cause, and the "
     "`readback ring full` counter for whether the ring, not the GPU, was the "
     "limit."},

    // ---- world storage ----
    {"worldStorage", "World Storage", nullptr, PerfSide::Both, PerfScope::Stream,
     "", "Window shifts, chunk fetch/evict and worldgen. Zero while standing "
     "still; this is the row that flying lights up."},
    {"worldgen", "Worldgen", "worldStorage", PerfSide::Gpu, PerfScope::Count,
     "worldgen;worldgenList", "Per-cell pure function. Cost is per chunk "
     "generated, and only on the frames that generate one."},
    {"pageTable", "Page Table", "worldStorage", PerfSide::Both,
     PerfScope::PageTableCpu,
     "pageFill", "CPU is the dirty-mirror recurrence + materialize, which runs "
     "EVERY tick over the window; GPU is the fills a sentinel chunk needs. Page "
     "FAULTS are a bug, not a cost — the page shows them as a red counter."},
    {"farField", "Far-Field Cascades", "worldStorage", PerfSide::Gpu,
     PerfScope::Count, "farDown;farFill",
     "Downsample into the cascade pyramid. Flat per tick; the render-side cost "
     "of reading it is in raymarch."},

    // ---- render ----
    {"renderPass", "Rendering", nullptr, PerfSide::Both, PerfScope::RenderCpu,
     "", "CPU here is draw-call encode and instance uploads only. The GPU cost "
     "of the frame is the raymarch row."},
    {"shadowCache", "Shadow Cache", "renderPass", PerfSide::Gpu,
     PerfScope::Count, "shadow_prepare;shadow_resolve",
     "One media-blind shadow ray per visible surface PATCH, instead of one per "
     "lit pixel inside the raymarch. Its cost belongs next to raymarch, not "
     "inside it: this row going up while raymarch goes down by more is the "
     "trade working (world.h kShadowCacheBuckets)."},
    // THE RENDER PASS IS SEVEN SPANS NOW, NOT ONE. It used to be a single
    // timestamp pair around BeginRenderPass..End billed here, which said "the
    // GPU frame is the render pass" and nothing else — a bare count in exactly
    // CLAUDE.md rule 6's sense. The frame loop (main.cpp) now writes a pair
    // around each draw INSIDE the pass (legal: vkCmdWriteTimestamp2 may be
    // recorded inside a dynamic-rendering scope; only the query RESET and
    // RESOLVE may not, and both stay outside), and each pair bills to the
    // ARCH_NODES box that issues the draw. The names are kPerfRenderSpans.
    //
    // What this row therefore holds is the fullscreen raymarch fragment shader
    // ALONE. Everything inside that one draw — primary DDA, sun shadow rays,
    // media, far cascades, micro bricks, reflections, the MPM surface — is one
    // shader and cannot be timestamped apart; the `rm*` counters below are the
    // attribution inside it (RENDER_STATS in raymarch.wgsl: per-call-site step
    // counts over a 1-in-16 pixel sample), and --render-budget is the exact
    // per-feature millisecond answer when a knob is on trial.
    {"raymarch", "World raymarch", "renderPass", PerfSide::Gpu,
     PerfScope::Count, "",
     "The fullscreen raymarch draw alone (no raster geometry, no overlay). "
     "Usually the largest single number on this page, and the one resolution "
     "scales. The `rm*` counters say which trace inside it took the steps."},
    {"drawParticles", "Particles + fluid cubes", "renderPass", PerfSide::Gpu,
     PerfScope::Count, "",
     "Instanced cubes for ballistic particles, plus the MPM cube-debug draw "
     "when render.fluidSurface is 0. Scales with the live particle count."},
    {"drawBodies", "Debris bodies", "renderPass", PerfSide::Gpu,
     PerfScope::Count, "",
     "Instanced voxel draw for rigid debris. Scales with awake bodies x their "
     "voxel count, never with the world."},
    {"drawMicro", "Micro bodies", "renderPass", PerfSide::Gpu, PerfScope::Count,
     "", "Mob limbs and held items: one raymarched brick per instance. Scales "
     "with mobs on screen and the pixels they cover."},
    {"drawSprites", "Sprites", "renderPass", PerfSide::Gpu, PerfScope::Count,
     "", "Billboard sprites (spell glyphs, markers). Cheap unless something "
     "spawns thousands."},
    {"drawDebug", "Debug draws", "renderPass", PerfSide::Gpu, PerfScope::Count,
     "", "Collision boxes, wind and current arrows. Zero when every debug view "
     "is off — a non-zero here in play is a toggle left on."},
    {"uiOverlay", "Overlay + swapchain wait", "renderPass", PerfSide::Both,
     PerfScope::Present,
     "", "GPU: the ImGui overlay draw. CPU: AcquireFrame + Present — under "
     "vsync the CPU half IS the wait, so a large value there means the CPU had "
     "nothing left to do; read the GPU bars, not this one, for the frame's "
     "real cost."},

    // ---- game systems (CPU) ----
    {"gameSystems", "Game Systems", nullptr, PerfSide::Cpu, PerfScope::GameLogic,
     "", "Brush, spells, melee, held items, camera — everything that turns "
     "input into ops for the next tick."},
    {"player", "Player + Input", "gameSystems", PerfSide::Cpu, PerfScope::Input,
     "", "Key/mouse sampling, camera update, movement integration and the voxel "
     "collision sweep against the CPU mirror. Per FRAME, not per tick."},
    {"postStep", "Post-step", "gameSystems", PerfSide::Cpu, PerfScope::PostStep,
     "", "Mob steering, animation, avatar pose and player push-out, after Jolt "
     "has moved everything."},
    {"audioSys", "Audio", "gameSystems", PerfSide::Cpu, PerfScope::Audio, "",
     "Cue dispatch and the occlusion ray per voice. Silent under a headless "
     "harness."},

    // ---- physics ----
    {"physicsSys", "Physics (Jolt)", nullptr, PerfSide::Cpu, PerfScope::Physics,
     "", "One Jolt step. Overlaps the GPU tick on purpose, so its ms is only "
     "frame time if it exceeds the submit it hides behind."},

    // ---- backend ----
    {"gpuBackend", "GPU / RHI", nullptr, PerfSide::Cpu, PerfScope::Submit, "",
     "CommandEncoder::Finish plus vkQueueSubmit only — the pass-table walk that "
     "produced the buffer is `encode`. Large here is the driver, not us."},

    // NOTE: PerfScope::Other has NO ROW HERE, deliberately. It is the residual
    // — frame time no scope claimed — and it is not a system, so it is not a
    // box on the Engine map. `PerfNodeIndexForScope` returns -1 for it, the
    // JSON carries `"node": null`, and the page falls back to the scope key.
    // Giving it an ARCH_NODES entry would put an orphan "Unattributed" box on
    // the architecture diagram, which is exactly the category error the row
    // exists to prevent.
};
constexpr int kPerfNodeCount = (int)(sizeof(kPerfNodes) / sizeof(kPerfNodes[0]));

// ---------------------------------------------------------------------------
// COUNTERS — the denominators.
//
// "caLoop cost 4.2 ms" is a fact with no explanation attached; "4.2 ms over
// 1,847 active chunks" is a diagnosis. CLAUDE.md's rule 6 is that a bare count
// is not a measurement, and the mirror image holds too: a bare duration is not
// one either. Every counter here exists to be the denominator of some bar on
// the page.
// ---------------------------------------------------------------------------
enum class PerfCounter : uint8_t {
  ActiveChunks,      // dirty chunks the CA dispatched over
  Particles,         // live ballistic particles
  FluidParticles,    // live MPM particles
  Ops,               // MutationQueue brush ops this tick
  CellOps,           // MutationQueue exact-cell ops this tick
  Explosions,        // explosion ops this tick
  PagesResident,     // page-table pages in use
  PageFills,         // page fills issued this tick
  PageFaults,        // dropped stores — a BUG counter, never a cost
  ChunksStreamed,    // chunks fetched/generated this tick
  Bodies,            // Jolt bodies awake
  DrawCalls,         // draw calls in the render pass
  VoxelsNonAir,      // non-air voxels in the residency window
  WaterBodies,       // registered lakes
  SnapshotStalls,    // paged staleness fallbacks: a BLOCKING WaitIdle per count
  ReadbackDeclined,  // readback requests refused: the ring was full
  // The raymarch's inside (RENDER_STATS). Order matches kPerfCounters below
  // AND the RS_* slot order in raymarch.wgsl: RmPixels is the shader's slot 0
  // (the sampled-pixel denominator, scaled back up to pixels), RmPrimarySteps
  // its slot 1, and so on.
  RmPixels,
  RmPrimarySteps, RmMediaCells, RmShadowSteps, RmShadowCacheTaps,
  RmShadowCacheReqs, RmFarSteps, RmFarShadowSteps, RmMicroSteps,
  RmReflectSteps, RmFluidSteps, RmGodraySteps, RmPxSky, RmPxFar, RmPxWater,
  RmPxSubmerged,
  Count
};
constexpr int kPerfCounterCount = (int)PerfCounter::Count;

struct PerfCounterDef {
  const char* key;
  const char* label;
  const char* node;   // the ARCH_NODES row this counter explains
  bool isBug;         // render red and never as a cost: page faults
};

inline constexpr PerfCounterDef kPerfCounters[] = {
    {"activeChunks", "active chunks", "caLoop", false},
    {"particles", "particles", "particleSys", false},
    {"fluidParticles", "MPM particles", "fluidSys", false},
    {"ops", "brush ops", "mutQueue", false},
    {"cellOps", "cell ops", "mutQueue", false},
    {"explosions", "explosions", "explode", false},
    {"pagesResident", "pages resident", "pageTable", false},
    {"pageFills", "page fills", "pageTable", false},
    {"pageFaults", "PAGE FAULTS", "pageTable", true},
    {"chunksStreamed", "chunks streamed", "worldStorage", false},
    {"bodies", "bodies awake", "physicsSys", false},
    {"drawCalls", "draw calls", "renderPass", false},
    {"voxelsNonAir", "non-air voxels", "worldStorage", false},
    {"waterBodies", "water bodies", "waterBodies", false},
    // A BUG counter, like page faults, and for the same reason: CLAUDE.md says
    // never add a synchronous readback to the frame path, and each of these is
    // a full-device WaitIdle that got there anyway. It is the denominator that
    // turns "readback 12 ms" into "readback 12 ms over 2 stalls".
    {"snapshotStalls", "SNAPSHOT STALLS", "readbackStall", true},
    // Ticks whose readback REQUEST was refused because all kReadbackSlots of
    // the ring were still in flight. Not a bug: it is the ring saying the GPU
    // is more than three submits behind. Paired with the stall counter it
    // answers "was the ring or the GPU the limit" without a second run.
    {"readbackDeclined", "readback ring full", "readbackStall", false},
    // ---- the raymarch's INSIDE, from RENDER_STATS (raymarch.wgsl) ----------
    // Per-frame totals, already scaled up from the 1-in-16 pixel sample the
    // shader records on. A STEP is one DDA cell advance in the named trace; a
    // px row is a pixel count. The page turns the step rows into an ESTIMATED
    // millisecond split of the `raymarch` GPU span — estimated, because steps
    // are a proxy for time and the fixed per-pixel register cost of trace()
    // (memory: ~half the frame) is not a step. Exact per-feature ms is
    // --render-budget; this is the live, camera-following version of it.
    {"rmPixels", "pixels shaded", "raymarch", false},
    {"rmPrimarySteps", "primary DDA steps", "raymarch", false},
    {"rmMediaCells", "media cells (fire/smoke/water tau)", "raymarch", false},
    {"rmShadowSteps", "sun shadow ray steps", "raymarch", false},
    {"rmShadowCacheTaps", "shadow cache taps", "shadowCache", false},
    {"rmShadowCacheReqs", "shadow cache requests (misses)", "shadowCache", false},
    {"rmFarSteps", "far cascade steps", "farField", false},
    {"rmFarShadowSteps", "far cascade shadow steps", "farField", false},
    {"rmMicroSteps", "micro brick steps", "microDetail", false},
    {"rmReflectSteps", "reflection ray steps", "raymarch", false},
    {"rmFluidSteps", "MPM surface march steps", "fluidSys", false},
    {"rmGodraySteps", "god ray / silt steps", "raymarch", false},
    {"rmPxSky", "sky pixels", "sky", false},
    {"rmPxFar", "far cascade pixels", "farField", false},
    {"rmPxWater", "water surface pixels", "raymarch", false},
    {"rmPxSubmerged", "submerged pixels", "raymarch", false},
};
static_assert((int)(sizeof(kPerfCounters) / sizeof(kPerfCounters[0])) ==
                  kPerfCounterCount,
              "kPerfCounters must have one row per PerfCounter enumerator");

// ---------------------------------------------------------------------------
// The per-frame sample. One of these is produced per frame by the harness and
// per frame by the live telemetry path, and both serialize it the same way —
// which is what lets the Performance tab draw a recorded run and a live session
// with the same code.
// ---------------------------------------------------------------------------
struct PerfSample {
  uint32_t tick = 0;
  uint32_t frame = 0;
  double wallMs = 0;                    // whole frame, wall clock
  double cpuMs[kPerfScopeCount] = {};   // per-scope CPU time
  // Per-node GPU time. Indexed by kPerfNodes position, so a node with no
  // passKeys is simply always zero. Sparse in practice and cheap to send.
  double gpuMs[kPerfNodeCount] = {};
  double counters[kPerfCounterCount] = {};
  // True when this sample's GPU numbers came back from real timestamp queries.
  // A frame whose queries had not resolved yet carries the CPU half only, and
  // the page must NOT draw it as "the GPU cost nothing".
  bool gpuValid = false;
};

// Sum of every CPU scope — the CPU half of the frame.
inline double PerfCpuTotal(const PerfSample& s) {
  double t = 0;
  for (int i = 0; i < kPerfScopeCount; i++) t += s.cpuMs[i];
  return t;
}
// Sum of every node's GPU time — the GPU half of the frame.
inline double PerfGpuTotal(const PerfSample& s) {
  double t = 0;
  for (int i = 0; i < kPerfNodeCount; i++) t += s.gpuMs[i];
  return t;
}

// Scope key strings, for the wire format and the JSON. Indexed by PerfScope.
inline constexpr const char* kPerfScopeKeys[] = {
    "input", "stream", "gameLogic", "waterBody", "upload",
    "pageTableCpu", "encode", "submit", "physics", "postStep",
    "readback", "readbackStall", "audio", "renderCpu", "present", "other",
};
static_assert((int)(sizeof(kPerfScopeKeys) / sizeof(kPerfScopeKeys[0])) ==
                  kPerfScopeCount,
              "kPerfScopeKeys must have one entry per PerfScope enumerator");

// Which node a pass_table.def PASS() row bills to, or -1 if nothing claims it.
//
// ONE implementation, because there are two consumers — the --perf harness and
// the live telemetry path in main.cpp — and two copies of an attribution table
// is exactly how a pass ends up counted twice on one page and not at all on the
// other. Linear over ~25 rows x a few keys each, called once per timed pass per
// frame; the whole scan is a few hundred character comparisons against a table
// that fits in L1.
//
// A -1 here is not a benign miss: it is GPU time that the page's bars do not
// account for, so callers must COUNT what they drop and show it. See
// `unattributed` in the emitted JSON.
inline int PerfNodeForPass(const char* passName) {
  if (!passName) return -1;
  for (int n = 0; n < kPerfNodeCount; n++) {
    const char* k = kPerfNodes[n].passKeys;
    if (!k || !*k) continue;
    // Walk the ';'-separated list without allocating: match a full segment, so
    // "ca" does not match "cattail" and "compact" does not match "compactNext"
    // unless that name is listed in its own right.
    for (const char* seg = k; *seg;) {
      const char* end = seg;
      while (*end && *end != ';') end++;
      const size_t len = (size_t)(end - seg);
      size_t i = 0;
      while (i < len && passName[i] && passName[i] == seg[i]) i++;
      if (i == len && passName[i] == 0) return n;
      seg = *end ? end + 1 : end;
    }
  }
  return -1;
}

// The render pass's timestamp spans (main.cpp's frame loop and the --perf
// harness's RenderFrame both write them) and the node each bills to. A span,
// not a pass_table.def row, because a draw is not a table row — the barrier
// generator never sees it — so PerfNodeForPass cannot name it. ONE table for
// both writers, for the same reason PerfNodeForPass is one function.
struct PerfRenderSpanDef {
  const char* span;   // the name handed to PassTimer::AllocPassPair
  const char* node;   // kPerfNodes entry it bills to
};
inline constexpr PerfRenderSpanDef kPerfRenderSpans[] = {
    {"rm_world", "raymarch"},
    {"rm_particles", "drawParticles"},
    {"rm_bodies", "drawBodies"},
    {"rm_micro", "drawMicro"},
    {"rm_sprites", "drawSprites"},
    {"rm_debug", "drawDebug"},
    {"rm_overlay", "uiOverlay"},
    // The legacy whole-pass span. Still what --perf's RenderFrame writes when
    // it has no per-draw split (an offscreen harness frame is one draw), and
    // billed to the raymarch row so an old perf.json keeps reading.
    {"render", "raymarch"},
};
inline int PerfNodeForRenderSpan(const char* span) {
  if (!span) return -1;
  for (const PerfRenderSpanDef& r : kPerfRenderSpans) {
    const char* a = r.span;
    const char* b = span;
    while (*a && *a == *b) { a++; b++; }
    if (*a || *b) continue;
    for (int n = 0; n < kPerfNodeCount; n++) {
      const char* x = kPerfNodes[n].node;
      const char* y = r.node;
      while (*x && *x == *y) { x++; y++; }
      if (!*x && !*y) return n;
    }
  }
  return -1;
}

// Which node a CPU scope bills to. Linear over ~25 rows, called once per scope
// per frame at most — a map would be more code than it saves.
inline const PerfNodeDef* PerfNodeForScope(PerfScope s) {
  for (int i = 0; i < kPerfNodeCount; i++)
    if (kPerfNodes[i].scope == s) return &kPerfNodes[i];
  return nullptr;
}
inline int PerfNodeIndexForScope(PerfScope s) {
  for (int i = 0; i < kPerfNodeCount; i++)
    if (kPerfNodes[i].scope == s) return i;
  return -1;
}

}  // namespace sandvox
