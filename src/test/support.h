// support.h — the sim/render plumbing shared by the frame loop, --shot, and
// every selftest gate.
//
// These lived in main.cpp's anonymous namespace, which meant the selftest could
// not be moved out of main.cpp without duplicating them — and a duplicated
// SubmitTick is exactly the kind of drift that makes a test pass against
// behaviour the game does not have. One definition, three consumers.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game/avatar.h"
#include "game/camera.h"
#include "gpu/context.h"
#include "math3d.h"
#include "phys/debris.h"
#include "game/mob.h"
#include "sim/biomes.h"
#include "sim/renderspec.h"
#include "sim/simulation.h"
#include "sim/tuning.h"
#include "sim/world.h"

namespace sandvox {

constexpr float kTickDt = 1.0f / 30.0f;
constexpr uint32_t kDefaultSeed = 1337;

// Which mob def the player wears. Swapping the player character is meant to be
// a one-line data change, so this lives in ONE place: the game's avatar and the
// selftest's avatar gate both read it. Two literals would let the test keep
// passing against a character nobody plays.
extern const char* kAvatarDefName;

// Time of day used by --shot, as a 0..1 fraction of the cycle (0 = midnight,
// 0.5 = noon). Set by `--time`.
extern float g_shotTimeOfDay;

std::string AssetDir();
double NowSeconds();

uint32_t TicksPerDay(const Tuning& t);
SkyState SkyForTick(const Tuning& t, uint32_t tick);

// The integer day phase for `tick`, EXACTLY as SubmitTick puts it on
// TickParams.dayPhase (celestial clock, freeze override and all).
//
// Exposed because the CPU reaction mirror (sim/reactcpu.h) has to gate a
// day/night-conditioned rule on the same value the GPU does, and the systems
// that run it -- DebrisSystem::BurnBodies, MobSystem::BurnLimbs -- run BEFORE
// SubmitTick in the tick order. Restating the expression at the second call
// site is the ordinary way for two places to stop agreeing.
uint32_t DayPhaseNow(uint32_t tick);

// ---- short-range mode (dev panel "short range (100 m + fog)") --------------
// Whether every ray is ceilinged at render.shortRangeDist with a fog ramp over
// the last stretch (RenderParams flag bit 2). WriteRenderParams OR's this into
// the flag word for EVERY drawing path, so --shot, the perf harness, the lab
// and the portrait all honour it without a per-call-site argument.
//
// Default comes from SANDVOX_SHORT_RANGE (or `--short-range`, which calls the
// setter); the windowed game re-asserts the panel checkbox every frame, so the
// checkbox wins there. Render-only — it reaches no sim input.
bool ShortRangeMode();
void SetShortRange(bool on);

// fluidCount: live MLS-MPM particle count — nonzero enables the fluid surface
// march in raymarch.wgsl; zero costs the renderer nothing.
void WriteRenderParams(const rhi::Queue& queue, const World& world,
                       const Vec3& eye, const Camera& cam, float aspect,
                       bool shadows, float time,
                       float fogDensity = kFarFogDensity,
                       float viewPx = 1080.0f, uint32_t tick = 0,
                       uint32_t fluidCount = 0,
                       float frameFrac = 0.0f,
                       uint32_t extraFlags = 0);

// WriteRenderParams also publishes the raymarch's per-frame specialization
// record — sim/renderspec.h, included above, says what it is and why it is
// written from here and nowhere else.

// ---- the TAA camera jitter (render.taa; assets/shaders/taa.wgsl) -----------
//
// Perturb `cam` by this frame's R2 sub-pixel offset, MEASURE what that did to
// the image in render pixels, and fill the TaaCamera the resolve pass
// reprojects through. One definition, two consumers — the frame loop and the
// `taa` gate — because a gate that jitters the camera differently from the game
// is a gate that passes against behaviour the game does not have, which is the
// whole reason this file exists.
//
// `outJittered` is what WriteRenderParams must then be given: perturbing the
// CAMERA rather than the basis is what keeps the raymarch's ray construction
// and projectView's raster projection moving together — both derive from the
// same RenderParams and there is no second place to keep in step.
//
// `amp` is render.taaJitter (1 = the full +/-0.5 px). `aspect` and the fov come
// from the same places WriteRenderParams reads them, so the measured shift is
// the shift that frame actually has.
void ApplyTaaJitter(const Camera& cam, const Vec3& eye, float aspect,
                    uint32_t renderW, uint32_t renderH, uint32_t frameIdx,
                    float amp, Camera& outJittered,
                    Simulation::TaaCamera& outTaa);

// Encode + submit one sim tick. `particlesActive` must be derived only from
// tick-deterministic inputs (explosion history + a settled particle count),
// never from frame timing — see DESIGN.md §2/§4.
// fluidLive is the caller's CONSERVATIVE MLS-MPM live estimate (the GPU owns
// the real count via the seam; snapshot count + spawns since) —
// CPU-owned (docs/PLAN_mpm_fluids.md prototype; world.h fluid block). The
// caller owns the running count and must add fluidSpawns.size() to it after
// this returns. Both default to nothing, which records zero fluid work.
void SubmitTick(GpuContext& ctx, World& world, Simulation& sim, uint32_t tick,
                uint32_t seed, const std::vector<BrushOp>& ops,
                const std::vector<ExplosionOp>& exps,
                const std::vector<CellOp>& cells, bool hashEnable,
                IVec3 playerChunk, bool wantReadback, bool particlesActive,
                const std::vector<ParticleSpawn>& spawns = {},
                uint32_t farCount = 0,
                const std::vector<FluidSpawnOp>& fluidSpawns = {},
                uint32_t fluidLive = 0,
                // Material id each MPM species splashes micro droplets as
                // (TickParams.fluidSplashMat). Null = no splash coupling.
                const uint32_t* fluidSplashMat = nullptr,
                bool vizActive = false);

void SubmitWorldgen(GpuContext& ctx, World& world, Simulation& sim,
                    uint32_t seed);

// Re-read the AUTHORED ENVIRONMENT from disk -- assets/biomes/*.json, the
// world map named by worldgen.mapLayer, assets/trees/* -- validate it the way
// boot does, and push it to the GPU (Simulation::UploadEnvironment) and to
// the CPU twins (worldmap::SetCurrentWorldMap). docs/PLAN_environment_truth.md
// P-A: this is what "regen world", F7, the tuner's Apply button and
// --voxserve's RELOAD all call, so a saved biome or a repainted map reaches
// the next worldgen without a restart. Drains the GPU first (a table that
// grew is a new buffer). On ANY failure nothing is replaced: the world keeps
// generating from the tables it had and `log` says which file refused. Does
// NOT regenerate anything itself -- the caller decides what to rebuild.
bool ReloadEnvironment(GpuContext& ctx, Simulation& sim,
                       const std::vector<MaterialDef>& mats,
                       biomes::EnvironmentStamp& stamp, std::string& log);

// ---- harness snapshot drain (PLAN_page_table.md, phase 7b) ----------------
//
// THE PROBLEM. `World::Snap().valid` is false on every tick of every HEADLESS
// harness. The async map is issued (KickReadback) and ProcessEvents is pumped,
// but a harness submits and pumps in lockstep within one loop iteration, so
// the fence for the just-submitted work has not retired when the callback
// would fire — and the next iteration overwrites the ring slot. The windowed
// game does not have this shape: real frames elapse between submit and pump,
// so its snapshots arrive normally (main.cpp's frame loop, ProcessEvents).
//
// This is PRE-EXISTING and orthogonal to paging — it is why World::KindAt has
// always returned Unknown under the harnesses — but paging is the first system
// that DEPENDS on the snapshot arriving: §3.2 step (2)'s intersection
// tightening is the only thing that shrinks the conservative dirty mirror, and
// §3.6's free condition reads occupancy from the same snapshot.
//
// THE FIX, and why it is here and not in the tick path. When this is set, the
// harness blocks after a kicked readback until the map completes, which makes
// a harness tick behave like a game frame rather than changing what a tick
// does. Blocking is sanctioned in exactly this place: CLAUDE.md names the
// selftest's synchronous reads as the one exception to the no-sync-readback
// rule, and this is the same exception in the same layer.
//
// OFF BY DEFAULT, so main.cpp's frame loop — which shares SubmitTick — is
// untouched and never pays a sync point. Only the harnesses opt in.
void SetHarnessSnapshotDrain(bool on);
bool HarnessSnapshotDrain();

// THE PAGED SNAPSHOT-STALENESS STALL, counted.
//
// SubmitTick's paged self-defence (see the long comment at its tail) runs
// `ctx.WaitIdle(); ctx.ProcessEvents();` when the snapshot in hand is older
// than `kPagedSnapshotMaxGap` ticks. On a harness that is a sanctioned sync
// point. On the GAME frame path it is a full-device stall — it waits for the
// render of the frame in front of it as well as the tick — and it is the one
// place the frame loop can block for milliseconds without any system looking
// busy. It fires from a CADENCE, not from anything the player did, which is
// exactly why it reads as "submit spikes randomly while I do nothing".
//
// So it is counted rather than argued about. Read-and-clear, per frame, and
// the Performance tab shows it as the denominator of the `readback` bar:
// "readback 12.4 ms over 2 snapshot stalls" is a diagnosis, "readback 12.4 ms"
// is not (CLAUDE.md rule 6).
uint32_t TakeSnapshotStalls();
// Ticks since the last call whose readback REQUEST was refused because every
// slot of World's ring was still in flight (EncodeReadbacks returned false).
// The other half of the stall diagnosis: stalls WITH declines mean the GPU is
// behind and a deeper ring would not help; declines WITHOUT stalls mean the
// ring itself is the limit. Read-and-cleared like TakeSnapshotStalls.
uint32_t TakeReadbackDeclines();

// ---- P2-D: WHY the stall fired, not just that it did (CLAUDE.md rule 6) ----
//
// The two counters above are a bare count, and a bare count buys one hypothesis
// per run. These say which of the two structurally different causes fired and
// which arm paid for it:
//
//   refusedArm  the ring had no free slot on this tick, so NO copy was issued
//               for it. The newest snapshot the ring can ever deliver is older
//               than this tick, and if it is older than maxGap the wait loop
//               cannot succeed no matter how long it blocks. That is a RING
//               DEPTH problem.
//   issuedArm   a copy WAS issued for this tick (and for the ticks before it);
//               they simply have not landed because the GPU queue is deep.
//               That is a LATENCY problem, and blocking is the only answer
//               short of tolerating a bigger gap.
//
//   mapArm      the WaitOldestPendingMap loop reached freshness (targeted, one
//               fence per iteration).
//   idleArm     it did not, and the full ctx.WaitIdle() drain ran.
//   idleFutile  ... and the snapshot was STILL stale afterwards. WaitIdle
//               cannot manufacture a snapshot that was never encoded, so every
//               count here is a full device drain that bought nothing.
//
// gapHist[i] is the staleness (tick - snap.tick) at the moment of the stall,
// clamped, with slot 9 meaning "no valid snapshot at all". mapArmMs/idleArmMs
// split the blocked wall time the same way ReadbackStall aggregates it.
struct SnapshotStallStats {
  uint32_t stalls = 0;
  uint32_t refusedArm = 0;
  uint32_t issuedArm = 0;
  uint32_t mapArm = 0;
  uint32_t idleArm = 0;
  uint32_t idleFutile = 0;
  uint32_t proceedStale = 0;  // nothing left to wait for; ran on a stale mirror
  uint32_t mapWaits = 0;
  uint32_t gapHist[10] = {};
  uint32_t gapMax = 0;
  double mapArmMs = 0;
  double idleArmMs = 0;
};
// Read-and-clear, like the two counters above. main.cpp accumulates it over the
// harness frames and prints it beside the stall line.
SnapshotStallStats TakeSnapshotStallStats();

// ---- P3-F: BILL THE TICK'S UNTABLED GPU WORK -------------------------------
//
// SubmitTick records three lots of GPU commands that pass_table.def does not
// describe, so `PerfNodeForPass` cannot name them and nothing on the
// Performance page ever accounted for them:
//
//   pageFillCmd    PageTable::DrainFills -- one 16 KiB vkCmdFillBuffer per page
//                  materialized from an EMPTY/UNIFORM sentinel. Thousands per
//                  tick under sustained flight, and the leading suspect for the
//                  sprint-flight frame-time tail.
//   freeProbeCopy  the free-confirmation probe's per-candidate chunk copies,
//                  which also carry their OWN vkQueueSubmit.
//   readbackCopy   EncodeReadbacks + EncodeDirtyCopy, the snapshot's way out.
//
// They are timed the way the render pass's draws are: a hand-allocated
// (begin, end) query pair on the SAME PassTimer the pass table uses, resolved
// by the resolve EncodeTick already encodes at the tail of the tick command
// buffer. Names come from `kPerfRenderSpans` in measure/perfnodes.h.
//
// A POINTER SET BY THE HARNESS, not a parameter, for the same reason
// SetHarnessSnapshotDrain is: SubmitTick has fifteen arguments and one call
// site that cares. NULL in the game, in --selftest and in every run that did
// not ask, so the recorded command buffer is byte-identical -- a timestamp
// write observes a command, it does not reorder one, and the --perf harness
// asserts the world hash matches an untimed run.
// `::PassTimer` explicitly: this header lives in `namespace sandvox` and the
// timer does not, so an unqualified name here would declare a second, empty
// sandvox::PassTimer that silently never matches the real one.
void SetSubmitTickPassTimer(::PassTimer* t);
::PassTimer* SubmitTickPassTimer();

// Body render plumbing moved to game/bodyreg.h: BodyRegistry owns the ONE
// definition of the debris | mob | avatar slot walk, and all three parallel
// arrays (xforms, cube instances, micro insts) are built through it. The free
// BuildBodyXforms/BuildMicroInsts helpers that lived here covered only two of
// the three arrays, which left the instance walk hand-rolled at every call
// site — the exact drift they existed to prevent.

bool WriteBmpFile(const std::string& path, const std::vector<uint8_t>& rgba,
                  uint32_t w, uint32_t h);

// ---- FIXTURE ANCHORING: never write an absolute Y in a gate ----------------
//
// A gate that builds a slab, a pond or a prefab at a literal height is pinned
// to whatever the terrain band happened to be the day it was written. Package B
// converted the paint/blast sites; package C moved the datum from y32..y86 to
// ~y200 and found eight more — a "pond" and a "stone slab in open air" buried
// in bedrock, a 48^3 cube counted against a box that now contains a hillside,
// and two cascade sites at y200 that used to be sky. Every one of them failed
// as something else: no evaporation, no blood landing, 257,391 voxels where
// 110,592 were placed.
//
// So: ask the terrain. `above` is the clearance in voxels, measured from the
// GROUND (the height contract — World::TerrainHeight ≡ genColumn.h), and the
// result is clamped to leave `pad` voxels of window above it so a fixture near
// the top of the band cannot be built outside residency.
//
// The MAX over a footprint, not the centre column, when the fixture is wide:
// a flat slab laid at the centre height has its uphill half buried.
int FixtureY(int x, int z, uint32_t seed, int above, int pad = 32);
int FixtureYOver(int x0, int z0, int x1, int z1, uint32_t seed, int above,
                 int pad = 32);

// Blocking readbacks. The selftest's synchronous hash read is the one
// sanctioned exception to the no-sync-readback rule (CLAUDE.md).
uint32_t ReadHashSync(GpuContext& ctx, World& world);
uint32_t HashWorldNow(GpuContext& ctx, World& world, Simulation& sim,
                      uint32_t seed);
void ReadCountsSync(GpuContext& ctx, World& world, uint32_t out[2]);
// The whole water-body ledger (kWaterBodyCap * kWaterBodyStateWords i32) —
// docs/PLAN_water_master.md M2. The ledger is GPU-owned on purpose, so this is
// the ONLY way a gate can see the level, the debit or the adoption verdict.
//
// Blocking, and therefore for use OUTSIDE a tick loop only: a blocking readback
// inside one dilates the page-table snapshot cadence, which is how a streaming
// run once acquired 217 page faults that had nothing to do with the feature
// under test.
void ReadWaterLedgerSync(GpuContext& ctx, World& world, int32_t* out);
// The page-fault counter (world.pageFaults[0..3]): the dropped word and the
// refusing chunk span, not just the count. A shave that landed in a sentinel
// chunk shows up here and nowhere else.
void ReadPageFaultsSync(GpuContext& ctx, World& world, uint32_t out[4]);
// The fluid seam's FA_* word map (common.wgsl), 32 u32. `--gate waterbody`
// pass H needs FA_LIVE and EX_COMPACT_LIVE's successor: the IN-FLIGHT MPM
// MASS is a real term of the conservation sum once a drain emits particles
// and component 7 excites a shell, and a gate that cannot see it reports a
// leak that is sitting in the particle pool.
void ReadFluidArgsSync(GpuContext& ctx, World& world, uint32_t* out32);
uint32_t ReadActiveChunksSync(GpuContext& ctx, World& world, Simulation& sim);

// THE CPU SEAM for gate voxel dumps (PLAN_page_table.md §2.1a, fifth site).
//
// Reads `count` chunks starting at slot `firstSlot` into `out` (count *
// kChunkVol words), resolving each slot through World::PageOffsetOfSlot and
// SYNTHESIZING sentinel chunks CPU-side. A gate that indexes the result with
// World::SlotCellIndex therefore gets a dense-looking snapshot in slot order
// whatever the residency mode is — which is what the gates want, since they
// test sim behaviour rather than residency.
//
// `page-roundtrip` is the gate that reads THROUGH the translation instead, on
// purpose; everything else goes through here.
void ReadVoxelsSync(GpuContext& ctx, World& world, uint32_t firstSlot,
                    uint32_t count, uint32_t* out, const char* label);

// The scripted mutation stream the determinism gate hashes over. A pure
// function of tick, so both runs of the twice-run comparison see the same ops.
std::vector<BrushOp> SelftestOps(uint32_t tick, uint32_t seed);
std::vector<ExplosionOp> SelftestExps(uint32_t tick, uint32_t seed);
bool SelftestParticlesActive(uint32_t tick);

// ---- LEAVE THE SUITE'S RANDOMNESS AS YOU FOUND IT --------------------------
//
// A mob id is not just a handle: it seeds the entity-scoped gore variance
// (Mob::MakeGoreProfile), the blast crater's noise and the per-limb RNG key the
// burn/dissolve pass draws against. Gates share ONE MobSystem, so a gate that
// merely SPAWNS creatures re-rolls every id-keyed draw in every gate after it.
//
// Measured (selftest.cpp's kOrder note): inserting four spawning gates ahead of
// `armor-react` made its acid bath dissolve 0 skin voxels in 120 ticks where it
// had dissolved 18, and nothing about acid had changed. Restoring the counter is
// the honest fix; MobSystem::Reset(rewindIds=true) is not — setting it to 1 is a
// DIFFERENT perturbation rather than the absence of one, and mob.cpp records a
// gate that moved when somebody tried exactly that.
//
// Lives here rather than in one gate's .cpp because five gates across three
// files now need it and a copy per file is five things to keep true.
struct IdCounterScope {
  MobSystem& sys;
  uint64_t saved;
  explicit IdCounterScope(MobSystem& s) : sys(s), saved(s.NextIdCounter()) {}
  ~IdCounterScope() { sys.SetNextIdCounter(saved); }
  IdCounterScope(const IdCounterScope&) = delete;
  IdCounterScope& operator=(const IdCounterScope&) = delete;
};

}  // namespace sandvox
