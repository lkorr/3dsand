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
enum class PerfScope : uint8_t {
  Input,        // input sampling, camera, UI intent
  Stream,       // toroidal window shift, chunk fetch/evict, page fills
  GameLogic,    // brush/spell/melee/item drivers that emit ops for this tick
  WaterBody,    // WaterBodySystem::Tick (CPU half of the lake registry)
  Upload,       // op-stream assembly + WriteBuffer of the MutationQueue
  Encode,       // Simulation::EncodeTick — walking the pass table
  Submit,       // queue submit + present-queue bookkeeping
  Physics,      // Jolt step
  PostStep,     // debris/mob/avatar post-step, player push-out
  Readback,     // snapshot map callbacks, mirror rebuild
  Audio,        // cue dispatch + spatializer feed
  RenderCpu,    // render-pass encode: draw calls, instance buffers, overlay
  Present,      // AcquireFrame + Present (this is where vsync waits land)
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
     "blocking — if this is large the mirror copy is the reason."},

    // ---- world storage ----
    {"worldStorage", "World Storage", nullptr, PerfSide::Both, PerfScope::Stream,
     "", "Window shifts, chunk fetch/evict and worldgen. Zero while standing "
     "still; this is the row that flying lights up."},
    {"worldgen", "Worldgen", "worldStorage", PerfSide::Gpu, PerfScope::Count,
     "worldgen;worldgenList", "Per-cell pure function. Cost is per chunk "
     "generated, and only on the frames that generate one."},
    {"pageTable", "Page Table", "worldStorage", PerfSide::Gpu, PerfScope::Count,
     "pageFill", "Page fills issued when a sentinel chunk materializes. Page "
     "FAULTS are a bug, not a cost — the page shows them as a red counter."},
    {"farField", "Far-Field Cascades", "worldStorage", PerfSide::Gpu,
     PerfScope::Count, "farDown;farFill",
     "Downsample into the cascade pyramid. Flat per tick; the render-side cost "
     "of reading it is in raymarch."},

    // ---- render ----
    {"renderPass", "Rendering", nullptr, PerfSide::Both, PerfScope::RenderCpu,
     "", "CPU here is draw-call encode and instance uploads only. The GPU cost "
     "of the frame is the raymarch row."},
    {"raymarch", "Raymarch (GPU frame)", "renderPass", PerfSide::Gpu,
     PerfScope::Count, "",
     "The whole render pass, measured as one GPU span. Usually the largest "
     "single number on this page, and the one resolution scales."},
    {"uiOverlay", "Swapchain wait", "renderPass", PerfSide::Cpu,
     PerfScope::Present,
     "", "AcquireFrame + Present. Under vsync this row IS the wait, so a large "
     "value here means the CPU had nothing left to do — read the GPU bars, not "
     "this one, to find the frame's real cost."},

    // ---- game systems (CPU) ----
    {"gameSystems", "Game Systems", nullptr, PerfSide::Cpu, PerfScope::GameLogic,
     "", "Brush, spells, melee, held items, camera — everything that turns "
     "input into ops for the next tick."},
    {"player", "Player + Input", "gameSystems", PerfSide::Cpu, PerfScope::Input,
     "", "Movement integration and the voxel collision sweep against the CPU "
     "mirror."},
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
     "Command-buffer submit and the generated barriers. Large here means the "
     "CPU is bottlenecked recording, not the GPU running."},
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
    "encode", "submit", "physics", "postStep", "readback",
    "audio", "renderCpu", "present",
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
