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

// fluidCount: live MLS-MPM particle count — nonzero enables the fluid surface
// march in raymarch.wgsl; zero costs the renderer nothing.
void WriteRenderParams(const rhi::Queue& queue, const World& world,
                       const Vec3& eye, const Camera& cam, float aspect,
                       bool shadows, float time,
                       float fogDensity = kFarFogDensity,
                       float viewPx = 1080.0f, uint32_t tick = 0,
                       uint32_t fluidCount = 0,
                       float frameFrac = 0.0f);

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
                const uint32_t* fluidSplashMat = nullptr);

void SubmitWorldgen(GpuContext& ctx, World& world, Simulation& sim,
                    uint32_t seed);

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

// Body render plumbing moved to game/bodyreg.h: BodyRegistry owns the ONE
// definition of the debris | mob | avatar slot walk, and all three parallel
// arrays (xforms, cube instances, micro insts) are built through it. The free
// BuildBodyXforms/BuildMicroInsts helpers that lived here covered only two of
// the three arrays, which left the instance walk hand-rolled at every call
// site — the exact drift they existed to prevent.

bool WriteBmpFile(const std::string& path, const std::vector<uint8_t>& rgba,
                  uint32_t w, uint32_t h);

// Blocking readbacks. The selftest's synchronous hash read is the one
// sanctioned exception to the no-sync-readback rule (CLAUDE.md).
uint32_t ReadHashSync(GpuContext& ctx, World& world);
uint32_t HashWorldNow(GpuContext& ctx, World& world, Simulation& sim,
                      uint32_t seed);
void ReadCountsSync(GpuContext& ctx, World& world, uint32_t out[2]);
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
std::vector<BrushOp> SelftestOps(uint32_t tick);
std::vector<ExplosionOp> SelftestExps(uint32_t tick, uint32_t seed);
bool SelftestParticlesActive(uint32_t tick);

}  // namespace sandvox
