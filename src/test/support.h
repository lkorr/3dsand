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

void WriteRenderParams(const rhi::Queue& queue, const World& world,
                       const Vec3& eye, const Camera& cam, float aspect,
                       bool shadows, float time,
                       float fogDensity = kFarFogDensity,
                       float viewPx = 1080.0f, uint32_t tick = 0);

// Encode + submit one sim tick. `particlesActive` must be derived only from
// tick-deterministic inputs (explosion history + a settled particle count),
// never from frame timing — see DESIGN.md §2/§4.
// fluidBase is the MLS-MPM particle count BEFORE this tick's fluidSpawns —
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
                uint32_t fluidBase = 0);

void SubmitWorldgen(GpuContext& ctx, World& world, Simulation& sim,
                    uint32_t seed);

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
