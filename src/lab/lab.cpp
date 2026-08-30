// lab.cpp — the fluid lab: scenes, bench harness, tuning-file plumbing.
// See lab.h and docs/PLAN_fluid_overhaul.md §4 (WP1).

#include "lab/lab.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "gpu/context.h"
#include "gpu/passtimer.h"
#include "gpu/resources.h"
#include "sim/simulation.h"
#include "sim/tuning.h"
#include "test/support.h"

namespace sandvox {
namespace {

// ---- shared scene frame ----------------------------------------------------
// First air row above the slab, and the scene anchor. All scene geometry is
// absolute world voxels around the window-center column, window origin
// {0,0,0} — the lab never shifts the window.
constexpr int G = kLabSlabY + 1;   // 128
constexpr int CX = 256, CZ = 256;

// Per-tick pour ceiling in CELLS (8 particles each; kMaxFluidSpawnsPerTick /8).
constexpr uint32_t kPourCellsPerTick = kMaxFluidSpawnsPerTick / 8;  // 512

const char* kSceneNames[kLabSceneCount] = {"basin", "hill",  "faucet",
                                           "pool",  "slosh", "pond",
                                           "worldlake"};

// ---- the pond scenes (WP5) -------------------------------------------------
// Sized from worldgen.wgsl, not from what fits: pondInfo picks a radius in
// TUNE_POND_RADIUS_MIN .. +SPAN (68..127) and pondAt carves a parabolic bowl
// TUNE_POND_DEPTH_RIM..TUNE_POND_DEPTH (3..26) deep. The water volume of the
// SMALLEST such pond is
//     integral over the disc of (3 + 23*(1 - d^2/r^2)) = 14.5*pi*r^2
// = 210,600 voxels at r = 68. That is 1,684,800 eighths against a particle
// pool of 262,144 — the smallest thing worldgen calls a pond is 6.4x
// everything the solver can hold at once, and the largest is 22x. Every
// number in WP5 is decided against that ratio, which is exactly why none of
// the five pour scenes above could see the bug.
constexpr int kPondDepth = 26;     // TUNE_POND_DEPTH
constexpr int kPondDepthRim = 3;   // TUNE_POND_DEPTH_RIM
int sPondRadius = 68;              // TUNE_POND_RADIUS_MIN

// Bowl floor for a column d^2 from the centre of a radius-r pond, relative to
// the waterline. The worldgen expression verbatim (integer, same truncation).
int PondFloorBelowSurface(int d2, int r) {
  return kPondDepthRim +
         ((r * r - d2) * (kPondDepth - kPondDepthRim)) / (r * r);
}

// The drain, shared shape for both pond scenes: a 5x5 shaft from just under
// the water down into a sealed chamber with room for a real fraction of the
// body. Sealed on purpose — an open drain empties into cave systems and the
// mass ledger stops being an audit.
constexpr int kDrainHalf = 2;          // 5x5 shaft
constexpr int kChamberHalf = 20;       // 40x40 interior
constexpr int kChamberH = 20;          // 20 tall -> 32,000 voxels of capacity

// `pond` lives on the slab: waterline is the slab's top row, so the pond is
// flush with the ground exactly as worldgen's is.
constexpr int kPondSurf = G - 1;                         // 127
constexpr int kPondChamberTop = kPondSurf - kPondDepth - 12;   // 89
constexpr int kPondChamberLo = kPondChamberTop - kChamberH + 1;

// `worldlake` uses the authored lake in worldgen.wgsl genColumn: a disc at
// (420,420) of radius vlen(68) whose terrain is flattened to poolY and filled
// to poolY + vlen(24). ~348,600 water voxels (a cylinder, not a bowl — bigger
// than any generated pond of the same radius). It is authored rather than
// hash-placed, so it is at a KNOWN address in every seed, which is what makes
// the main-world arm of this measurement a scripted, repeatable run instead
// of a description of somebody flying around.
//
// DERIVED, never literals. `poolY` is `worldgen.spawnPlainY - vlen(15)` and
// the terrain overhaul has already moved `spawnPlainY` once (44 -> 200), which
// carried the lake from y 44..68 to y 185..209. These constants were baked at
// the OLD datum, so the audit box, the plug shaft and the sealed drain chamber
// all sat ~141 voxels of solid rock BELOW the water: every `worldlake` arm of
// `--fluid-bench` reported `plug pulled: 0 eighths standing`, and the three
// worldlake rows of PLAN_water_master §1's baseline table became
// unreproducible. `World::AuthoredPoolList` is the one authority for where
// that lake is (the `waterbody` gate reads the same list); ask it instead.
// Recomputed on every call rather than cached: tuning is loaded — and, in the
// bench, re-set per run — after static init.
struct LakeGeom {
  int cx, cz, r;
  int floorY;       // poolY: the flat terrain top inside the disc
  int surfY;        // the fill level; water occupies (floorY, surfY]
  int chamberTop;   // drain chamber roof, buried under the lake floor
  int chamberLo;
};
LakeGeom Lake() {
  World::AuthoredPool pools[World::kAuthoredPools];
  World::AuthoredPoolList(pools);
  const World::AuthoredPool& w = pools[0];   // basin 0 is the water lake
  LakeGeom g{};
  g.cx = w.cx;
  g.cz = w.cz;
  g.r = w.r;
  g.floorY = w.floorY;
  g.surfY = w.waterY;
  // Same 13-voxel rock plug under the floor the old literals described
  // (44 -> 31); the chamber then hangs kChamberH below its roof.
  g.chamberTop = g.floorY - 13;
  g.chamberLo = g.chamberTop - kChamberH + 1;
  return g;
}

// The hill ramp: solid up to `G + 19 - drop(x)` for ramp x-offsets 0..31.
// (dx*3)/5 steps 1 voxel down every 1-2 columns — a stepped ~31 deg slope,
// the plan's "~30° stepped stone ramp".
constexpr int kHillTop = 19;      // deck height above G
constexpr int kHillRampX0 = 216;  // first ramp column
constexpr int kHillRampX1 = 247;  // last ramp column (drop 18 -> surface G+1)
int HillDrop(int x) { return ((x - kHillRampX0) * 3) / 5; }

// Scene material at a world cell, or ~0u for "outside this scene's volume"
// (no CellOp is emitted at all — the cell keeps whatever worldgen put there).
// One function per scene keeps the build list and the reset trivially the
// same thing: LabSceneBuildOps just walks the volume and asks this.
uint32_t SceneMatAt(int scene, int x, int y, int z, uint32_t waterMat) {
  switch (scene) {
    case kLabPond: {
      const int r = sPondRadius;
      const int dx = x - CX, dz = z - CZ;
      const int d2 = dx * dx + dz * dz;
      if (d2 > r * r) return ~0u;   // outside the disc: plain slab, untouched
      const int floorY = kPondSurf - PondFloorBelowSurface(d2, r);
      if (y <= floorY) return kMatStone;
      if (y <= kPondSurf) return waterMat;
      return kMatAir;
    }
    // worldlake builds NOTHING: worldgen already made the lake. The only
    // scripted geometry is the plug (LabScenePlugOps).
    case kLabWorldLake: return ~0u;
    case kLabBasin: {
      // Walled 24x24 box, walls 2 thick, 16 high. Interior [244,267]^2.
      if (y > G + 15) return kMatAir;
      const bool wall = x < 244 || x > 267 || z < 244 || z > 267;
      return wall ? kMatStone : kMatAir;
    }
    case kLabHill: {
      // Channel z: interior [248,263], walls [246,247] and [264,265].
      const bool zWall = z < 248 || z > 263;
      // x zones: back wall | pour deck | stepped ramp | catch basin | far wall.
      if (x <= 203) return y <= G + 26 ? kMatStone : kMatAir;      // back wall
      int wallTop, floorTop;
      if (x <= 215) {              // pour deck
        floorTop = G + kHillTop;
        wallTop = G + 26;
      } else if (x <= kHillRampX1) {  // the ramp
        floorTop = G + kHillTop - HillDrop(x);
        floorTop = std::max(floorTop, G - 1);
        wallTop = floorTop + 6;
      } else if (x <= 269) {       // catch basin interior, carved into the slab
        // 9 deep below the slab surface: 22 x 16 x 17 air layers = 47,872
        // eighths of capacity against the 39,600-eighth pour. The original
        // slab-level floor held 22,528 — the basin CAPTURE metric was capped
        // at 57% by geometry once WP2 actually delivered the water.
        floorTop = G - 10;
        wallTop = G + 7;
      } else {                     // far wall
        return y <= G + 7 ? kMatStone : kMatAir;
      }
      if (zWall) return y <= wallTop ? kMatStone : kMatAir;
      return y <= floorTop ? kMatStone : kMatAir;
    }
    case kLabFaucet: {
      // Walled 20x20 basin under a point pour. Interior [246,265]^2, walls 10.
      if (y > G + 9) return kMatAir;
      const bool wall = x < 246 || x > 265 || z < 246 || z > 265;
      return wall ? kMatStone : kMatAir;
    }
    case kLabPool: {
      // Walled 16x16 pool. Interior [248,263]^2, walls 10 high.
      if (y > G + 9) return kMatAir;
      const bool wall = x < 248 || x > 263 || z < 248 || z > 263;
      return wall ? kMatStone : kMatAir;
    }
    case kLabSlosh: {
      // 48x10 channel, walls 2 thick, 12 high. Interior x [232,279], z
      // [251,260].
      if (y > G + 11) return kMatAir;
      const bool wall = x < 232 || x > 279 || z < 251 || z > 260;
      return wall ? kMatStone : kMatAir;
    }
  }
  return kMatAir;
}

// The volume LabSceneBuildOps enumerates. For the five pour scenes it IS the
// scene bounds (unchanged behaviour). The pond scenes keep the two apart: the
// mass-audit bounds have to reach down to the drain chamber, but enumerating
// a 90-tall box over a 141-wide disc would be 1.8 M cells of which 97% are
// untouched slab.
void SceneBuildBox(int scene, IVec3& lo, IVec3& hi) {
  if (scene == kLabPond) {
    const int r = sPondRadius;
    lo = {CX - r, kPondSurf - kPondDepth, CZ - r};
    hi = {CX + r, G + 2, CZ + r};
    return;
  }
  if (scene == kLabWorldLake) {   // worldgen builds it; nothing to enumerate
    lo = {0, 0, 0};
    hi = {-1, -1, -1};
    return;
  }
  LabSceneBounds(scene, lo, hi);
}

// One cell's 8 spawn particles on the half-cell lattice with deterministic
// jitter — byte-for-byte the mpm dev tool's shape (main.cpp pour), so the lab
// exercises exactly the path the user pours through. `salt` is the scene
// tick; jitter is hash(salt, index) only (rule 1 discipline).
void EmitCell(int x, int y, int z, int32_t vx, int32_t vy, int32_t vz,
              uint32_t mat, uint32_t salt, std::vector<FluidSpawnOp>& out) {
  for (int s = 0; s < 8; s++) {
    uint32_t h = (salt * 9781u + (uint32_t)out.size() * 6271u) * 747796405u +
                 2891336453u;
    FluidSpawnOp op{};
    op.px = (x << 16) + ((s & 1) ? 49152 : 16384) + (int32_t)(h % 8192u) - 4096;
    op.py = (y << 16) + ((s & 2) ? 49152 : 16384) +
            (int32_t)((h >> 13) % 8192u) - 4096;
    op.pz = (z << 16) + ((s & 4) ? 49152 : 16384) +
            (int32_t)((h >> 19) % 8192u) - 4096;
    op.vx = vx;
    op.vy = vy;
    op.vz = vz;
    op.species = 0;
    op.mat = mat;
    out.push_back(op);
  }
}

// Budget test for one more cell (rule 2: charged BEFORE emission, cell
// refused whole).
bool CellFits(uint32_t liveEstimate, const std::vector<FluidSpawnOp>& out) {
  if (liveEstimate + out.size() + 8 > kFluidCap) return false;
  if (out.size() + 8 > kMaxFluidSpawnsPerTick) return false;
  return true;
}

// Instant-block scenes (basin dam, slosh wave): the block's cells in a fixed
// order, spawned kPourCellsPerTick cells per tick starting at `firstTick`.
struct BlockSpawn {
  IVec3 lo, hi;   // inclusive cell box
  int32_t vx, vy, vz;
  uint32_t firstTick;
};

BlockSpawn SceneBlock(int scene) {
  if (scene == kLabBasin)
    // Dam column against the -x wall: 8 x 24 x 10 cells = 15,360 eighths,
    // standing (v = 0) — the wall "removal" is just that nothing holds it.
    return {{244, G, 244}, {251, G + 9, 267}, 0, 0, 0, 10};
  // Slosh: a raised block at the -x end of the channel, launched at +0.3
  // cells/tick — collapses into a travelling wave.
  return {{232, G, 251}, {241, G + 7, 260}, 19661, 0, 0, 5};
}

void BlockPour(const BlockSpawn& b, uint32_t sceneTick, uint32_t liveEstimate,
               uint32_t waterMat, std::vector<FluidSpawnOp>& out) {
  const int nx = b.hi.x - b.lo.x + 1, ny = b.hi.y - b.lo.y + 1,
            nz = b.hi.z - b.lo.z + 1;
  const uint32_t total = (uint32_t)(nx * ny * nz);
  if (sceneTick < b.firstTick) return;
  const uint32_t ti = sceneTick - b.firstTick;
  const uint32_t k0 = ti * kPourCellsPerTick;
  if (k0 >= total) return;
  const uint32_t k1 = std::min(k0 + kPourCellsPerTick, total);
  for (uint32_t k = k0; k < k1; k++) {
    if (!CellFits(liveEstimate, out)) return;
    const int x = b.lo.x + (int)(k % (uint32_t)nx);
    const int y = b.lo.y + (int)((k / (uint32_t)nx) % (uint32_t)ny);
    const int z = b.lo.z + (int)(k / (uint32_t)(nx * ny));
    EmitCell(x, y, z, b.vx, b.vy, b.vz, waterMat, sceneTick, out);
  }
}

// Sustained-pour scenes: a radius-r sphere of cells above the scene, every
// tick in [first, last], falling gently (-0.3 cells/tick) like the dev tool.
void SpherePour(IVec3 at, int r, uint32_t sceneTick, uint32_t first,
                uint32_t last, uint32_t liveEstimate, uint32_t waterMat,
                std::vector<FluidSpawnOp>& out) {
  if (sceneTick < first || sceneTick > last) return;
  for (int z = -r; z <= r; z++)
    for (int y = -r; y <= r; y++)
      for (int x = -r; x <= r; x++) {
        if (x * x + y * y + z * z > r * r) continue;
        if (!CellFits(liveEstimate, out)) return;
        EmitCell(at.x + x, at.y + y, at.z + z, 0, -19661, 0, waterMat,
                 sceneTick, out);
      }
}

double Pct(std::vector<double> v, double p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[(size_t)(p * (double)(v.size() - 1))];
}

}  // namespace

int LabSceneFromName(const std::string& name) {
  for (int i = 0; i < kLabSceneCount; i++)
    if (name == kSceneNames[i]) return i;
  return -1;
}

const char* LabSceneName(int scene) {
  return (scene >= 0 && scene < kLabSceneCount) ? kSceneNames[scene] : "?";
}

// A camera that can actually SEE INTO a walled box, derived from its bounds
// rather than guessed.
//
// Four of the five scenes used to be shot from a hand-picked eye that sat
// BELOW or barely above the wall top (faucet's eye was at y = G+24 against a
// box whose rim is G+26), so the near wall filled the frame and the water was
// not visible at all — the bench screenshots for `basin`, `pool`, `faucet` and
// `slosh` could not be used to judge anything, which is how a whole WP got
// written with only `hill` as visual evidence.
//
// The constraint is one inequality. With the eye at horizontal distance D from
// the centre of a box of half-footprint w and wall height h, the sightline to
// the floor centre has slope H/D and the sightline grazing the NEAR rim has
// slope h/(D - w). The floor is visible only when H/D > h/(D - w). Solving for
// H with a 60% margin gives a camera that is correct for any box the lab grows
// later, instead of a number that silently rots when a scene is resized.
static void LabBoxViewCamera(const IVec3& lo, const IVec3& hi, Vec3& eye,
                             Vec3& target) {
  const float cx = 0.5f * (float)(lo.x + hi.x);
  const float cz = 0.5f * (float)(lo.z + hi.z);
  const float w = 0.5f * (float)std::max(hi.x - lo.x, hi.z - lo.z);
  const float h = (float)(hi.y - lo.y);
  const float D = 1.35f * w + 20.0f;         // stand-off, scaled to the box
  const float H = 1.6f * D * h / (D - w);    // the inequality, with margin
  const float k = D * 0.70710678f;           // on the -x/-z diagonal
  eye = {cx - k, (float)lo.y + H, cz - k};
  target = {cx, (float)lo.y + 2.0f, cz};     // just above the floor
}

void LabSceneCamera(int scene, Vec3& eye, float& yaw, float& pitch) {
  // Eye + look target per scene; yaw/pitch derived (Camera convention:
  // yaw = atan2(dz, dx), the RunMobShot shape).
  // Eyes sit well above the wall tops so the interior — where the water is —
  // fills the frame rather than the outside of a stone box.
  Vec3 target{(float)CX, (float)G, (float)CZ};
  IVec3 blo, bhi;
  LabSceneBounds(scene, blo, bhi);
  switch (scene) {
    case kLabBasin:  LabBoxViewCamera(blo, bhi, eye, target); break;
    // Hill: from above the catch basin looking back UP the ramp, so the
    // stepped face, the deck pour and the basin are all in frame — the
    // mid-slope freeze (or the sheet-down that replaces it) is THE thing
    // this scene exists to show.
    case kLabHill:   eye = {268, (float)(G + 38), 214};
                     target = {224, (float)(G + 8), 256}; break;
    case kLabFaucet: LabBoxViewCamera(blo, bhi, eye, target); break;
    case kLabPool:   LabBoxViewCamera(blo, bhi, eye, target); break;
    // Slosh is a long shallow trough (51 x 13), not a box. The generic
    // diagonal eye does clear the rim, but it looks ALONG the trough, so the
    // far half hides behind the near wall and a ~1-cell-deep sheet reads as
    // nothing. Shoot it side-on instead: centred on the long axis, backed off
    // across the SHORT one, which puts the whole end-to-end wave in frame.
    // It also needs a STEEPER angle than the generic 1.6x margin: slosh's
    // water is a ~1-cell sheet lying on the floor of a 15-tall trough, and
    // clearing the rim by enough to see the floor CENTRE still leaves the near
    // half of that sheet behind the near wall. 58/34 = 1.7 against a rim slope
    // of 15/27.5 = 0.55, i.e. ~3x, which puts the whole floor in view.
    case kLabSlosh: {
      const float cx = 0.5f * (float)(blo.x + bhi.x);
      const float cz = 0.5f * (float)(blo.z + bhi.z);
      eye = {cx, (float)blo.y + 60.0f, cz - 34.0f};
      target = {cx, (float)blo.y + 2.0f, cz};
      break;
    }
    // The pond scenes have no walls to clear — the body is flush with the
    // ground — so the constraint is different: stand off far enough that the
    // whole disc is in frame, high enough that the surface reads as a surface
    // and not as a horizon line. 1.5r back on the diagonal at 0.9r up is
    // ~31 deg, which shows the far shore, the near shore and the drain
    // dimple at once.
    case kLabPond:
    case kLabWorldLake: {
      const LakeGeom L = Lake();
      const float r = (float)(scene == kLabPond ? sPondRadius : L.r);
      const float cx = (float)(scene == kLabPond ? CX : L.cx);
      const float cz = (float)(scene == kLabPond ? CZ : L.cz);
      const float surf = (float)(scene == kLabPond ? kPondSurf : L.surfY);
      const float k = 1.5f * r * 0.70710678f;
      eye = {cx - k, surf + 0.9f * r, cz - k};
      target = {cx, surf - 4.0f, cz};
      break;
    }
    default:         eye = {222, (float)(G + 30), 222}; break;
  }
  Vec3 d = target - eye;
  const float flat = std::sqrt(d.x * d.x + d.z * d.z);
  yaw = std::atan2(d.z, d.x);
  pitch = std::atan2(d.y, flat);
}

void LabSceneBuildOps(int scene, uint32_t sceneTick, uint32_t waterMat,
                      std::vector<CellOp>& out) {
  IVec3 lo, hi;
  SceneBuildBox(scene, lo, hi);
  if (hi.x < lo.x) return;                       // scene builds nothing
  const uint64_t nx = (uint64_t)(hi.x - lo.x + 1);
  const uint64_t nz = (uint64_t)(hi.z - lo.z + 1);
  const uint64_t total = nx * nz * (uint64_t)(hi.y - lo.y + 1);
  // The tick's slice of the volume, in a fixed enumeration order. Slicing on
  // the RAW cell index rather than on emitted ops keeps the schedule a pure
  // function of the box: a tick emits at most kMaxCellOpsPerTick, and which
  // cells land on which tick cannot drift when SceneMatAt changes.
  const uint64_t first = (uint64_t)(sceneTick - 1) * kMaxCellOpsPerTick;
  if (sceneTick < 1 || first >= total) return;
  const uint64_t last = std::min(first + kMaxCellOpsPerTick, total);
  for (uint64_t i = first; i < last; i++) {
    const int x = lo.x + (int)(i % nx);
    const int z = lo.z + (int)((i / nx) % nz);
    const int y = lo.y + (int)(i / (nx * nz));
    const uint32_t m = SceneMatAt(scene, x, y, z, waterMat);
    if (m == ~0u) continue;
    // Liquids are born FULL. sim_mutate's brush path does this itself
    // (LIQ_FULL_STATE), but a CellOp carries the whole word, and a state
    // nibble of 0 is fullness 1 — a pond built that way would be an eighth of
    // a pond and would collapse into films on the first tick.
    uint32_t word = m & 0xFFFu;
    if (m == waterMat) word |= 7u << 12;
    out.push_back({World::SlotCellIndex({x, y, z}), word});
  }
}

uint32_t LabSceneBuildEndTick(int scene) {
  IVec3 lo, hi;
  SceneBuildBox(scene, lo, hi);
  if (hi.x < lo.x) return 0;
  const uint64_t total = (uint64_t)(hi.x - lo.x + 1) *
                         (uint64_t)(hi.y - lo.y + 1) *
                         (uint64_t)(hi.z - lo.z + 1);
  return (uint32_t)((total + kMaxCellOpsPerTick - 1) / kMaxCellOpsPerTick);
}

uint32_t LabScenePlugTick(int scene) {
  // Late enough that the body is provably ASLEEP first: the build finishes,
  // the CA (or, after the flip, nothing) settles it, and the bench records a
  // stretch of zero live particles and zero active blocks. Without that
  // stretch "the pond burst" and "the pond never settled" are the same curve.
  if (scene == kLabPond) return LabSceneBuildEndTick(scene) + 60;
  if (scene == kLabWorldLake) return 60;
  return 0;
}

void LabScenePlugOps(int scene, std::vector<CellOp>& out) {
  if (scene != kLabPond && scene != kLabWorldLake) return;
  const bool pond = scene == kLabPond;
  const LakeGeom L = Lake();
  const int cx = pond ? CX : L.cx;
  const int cz = pond ? CZ : L.cz;
  // Shaft: from the cell just under the body's floor down to the chamber.
  const int shaftTop = pond ? kPondSurf - kPondDepthRim -
                                  (kPondDepth - kPondDepthRim)   // bowl centre
                            : L.floorY;
  const int chamberTop = pond ? kPondChamberTop : L.chamberTop;
  const int chamberLo = pond ? kPondChamberLo : L.chamberLo;
  for (int y = chamberTop + 1; y <= shaftTop; y++)
    for (int z = cz - kDrainHalf; z <= cz + kDrainHalf; z++)
      for (int x = cx - kDrainHalf; x <= cx + kDrainHalf; x++)
        out.push_back({World::SlotCellIndex({x, y, z}), kMatAir});
  // Chamber, with a one-voxel stone shell. The shell matters on `worldlake`,
  // where the surrounding rock is real terrain and can already be holed by a
  // cave: an open drain would empty the lake into the cave system and the
  // mass ledger would stop being an audit of the seam.
  for (int y = chamberLo - 1; y <= chamberTop + 1; y++)
    for (int z = cz - kChamberHalf - 1; z <= cz + kChamberHalf + 1; z++)
      for (int x = cx - kChamberHalf - 1; x <= cx + kChamberHalf + 1; x++) {
        const bool interior = y >= chamberLo && y <= chamberTop &&
                              x >= cx - kChamberHalf && x <= cx + kChamberHalf &&
                              z >= cz - kChamberHalf && z <= cz + kChamberHalf;
        // The shaft punches through the shell's roof.
        const bool underShaft = y == chamberTop + 1 &&
                                x >= cx - kDrainHalf && x <= cx + kDrainHalf &&
                                z >= cz - kDrainHalf && z <= cz + kDrainHalf;
        if (underShaft) continue;   // already air, emitted above
        out.push_back({World::SlotCellIndex({x, y, z}),
                       interior ? kMatAir : kMatStone});
      }
}

void LabScenePour(int scene, uint32_t sceneTick, uint32_t liveEstimate,
                  uint32_t waterMat, std::vector<FluidSpawnOp>& out) {
  switch (scene) {
    case kLabBasin:
    case kLabSlosh:
      BlockPour(SceneBlock(scene), sceneTick, liveEstimate, waterMat, out);
      return;
    case kLabHill:
      // Pour onto the deck; the only exit is the ramp. 33 cells x 8 = 264
      // particles/tick for 150 ticks = 39,600 eighths.
      SpherePour({210, G + 23, CZ}, 2, sceneTick, 10, 159, liveEstimate,
                 waterMat, out);
      return;
    case kLabFaucet:
      // The one open-ended pour: runs until the budget refuses (windowed) or
      // the bench run ends. 7 cells x 8 = 56 particles/tick.
      SpherePour({CX, G + 22, CZ}, 1, sceneTick, 10, ~0u, liveEstimate,
                 waterMat, out);
      return;
    case kLabPool:
      SpherePour({CX, G + 20, CZ}, 2, sceneTick, 10, 109, liveEstimate,
                 waterMat, out);
      return;
  }
}

uint32_t LabScenePourEnd(int scene) {
  switch (scene) {
    case kLabBasin:  return 13;    // 1,920 cells / 512 per tick from tick 10
    case kLabHill:   return 159;
    case kLabFaucet: return ~0u;   // never stops
    case kLabPool:   return 109;
    case kLabSlosh:  return 6;     // 800 cells / 512 per tick from tick 5
    // The pond scenes pour nothing. Their "input" is the standing body, and
    // the event that matters is the plug — so that is what the tick-of-settle
    // detector must run after.
    case kLabPond:
    case kLabWorldLake: return LabScenePlugTick(scene);
  }
  return 0;
}

uint32_t LabSceneBenchTicks(int scene) {
  switch (scene) {
    case kLabBasin:  return 400;
    case kLabHill:   return 600;
    case kLabFaucet: return 600;
    case kLabPool:   return 500;
    case kLabSlosh:  return 500;
    // Build + a settle window + 400 ticks (13 s) of drain. The burst is over
    // in one tick; what takes time is finding out whether the seam gets back
    // to a bounded live count or stays pinned at the pool ceiling.
    case kLabPond:   return LabScenePlugTick(kLabPond) + 400;
    case kLabWorldLake: return LabScenePlugTick(kLabWorldLake) + 400;
  }
  return 400;
}

void LabSceneBounds(int scene, IVec3& lo, IVec3& hi) {
  switch (scene) {
    case kLabBasin:  lo = {242, G, 242}; hi = {269, G + 23, 269}; return;
    case kLabHill:   lo = {202, G - 10, 246}; hi = {271, G + 30, 265}; return;
    case kLabFaucet: lo = {244, G, 244}; hi = {267, G + 26, 267}; return;
    case kLabPool:   lo = {246, G, 246}; hi = {265, G + 24, 265}; return;
    case kLabSlosh:  lo = {230, G, 249}; hi = {281, G + 15, 262}; return;
    // The audit box has to contain the drain chamber as well as the body:
    // "where did the water go" is the whole question these scenes ask, and a
    // sweep that stops at the bowl floor answers it LEAK every time.
    case kLabPond: {
      const int r = sPondRadius;
      lo = {CX - r - 2, kPondChamberLo - 2, CZ - r - 2};
      hi = {CX + r + 2, G + 2, CZ + r + 2};
      return;
    }
    case kLabWorldLake: {
      const LakeGeom L = Lake();
      lo = {L.cx - L.r - 4, L.chamberLo - 2, L.cz - L.r - 4};
      hi = {L.cx + L.r + 4, L.surfY + 4, L.cz + L.r + 4};
      return;
    }
  }
  lo = {0, 0, 0};
  hi = {0, 0, 0};
}

void LabSetPondRadius(int r) { sPondRadius = std::max(4, std::min(127, r)); }
int LabPondRadius() { return sPondRadius; }

bool LabSceneUsesLabWorld(int scene) { return scene != kLabWorldLake; }

// ---- --fluid-bench ---------------------------------------------------------

int RunFluidBench(GpuContext& ctx, World& world, Simulation& sim,
                  const std::vector<MaterialDef>& mats,
                  const std::string& sceneArg, const std::string& jsonPath) {
  struct BenchRun {
    int scene;
    int excite;         // sim.fluidExciteMode for the run
    std::string tag;    // scene name, "hill0" (excite-0 A/B), "pool-settle"
    bool settleTuning = false;  // the fluid-excite gate's settle overrides
    int ceiling = 0;    // sim.fluidExciteCeiling override (0 = shipped value)
    int radius = 0;     // pond disc radius (0 = shipped value)
    int perch = -1;     // sim.fluidExcitePerch override (-1 = shipped value)
    // sim.waterBodyMode override (-1 = shipped 0). The M3 arm: with it at 1
    // the puncture is drained by the water-body ledger's discharge law
    // instead of by the CA propagating pressure through the whole body, and
    // `-wb1` against the same scene's `-ex0` row is exactly the comparison
    // PLAN_water_master.md §1's baseline table is for.
    int water = -1;
  };
  std::vector<BenchRun> runs;
  // ---- the two WP5 sweeps ------------------------------------------------
  // `pond<R>` sets the disc radius; `pond<R>-ceil<C>` also overrides
  // sim.fluidExciteCeiling for that run (through the F5 reload path, since it
  // is a compiled-in const). Together they answer the two questions the plan
  // asks and one it does not:
  //   * ceil sweep  — what does the burst bound cost, and where should it sit?
  //   * radius sweep — the "maximum drainable body size". With a ceiling in
  //     place the honest answer turns out not to be a size at all: frame cost
  //     is flat in body size and what scales is drain TIME. The sweep is what
  //     shows that, so it stays in the harness rather than in a paragraph.
  // `<scene>-ceil<N>` works for ANY scene, not just the ponds: the ceiling is
  // a shared resource with explicit pours, so a scene that spawns 39,600
  // particles of its own leaves excite no headroom, and telling "the seam
  // refused this water" apart from "the solver cannot move this water" needs
  // the same scene run with the bound lifted.
  // `-ex<0|1>` and `-perch<0|1>` join `-ceil<N>` as run suffixes, in any order
  // and any combination: `pond68-ex0` is the CA-only reference arm and
  // `pond68-perch0` is the trigger A/B's second arm, so a whole comparison is
  // ONE binary invocation. Suffixes are the only per-run tuning the bench has,
  // and they all go through the F5 reload path because sim.fluid* are WGSL
  // compile-time consts.
  auto parseRun = [&](const std::string& a, BenchRun& r) -> bool {
    const size_t dash = a.find('-');
    const std::string head = a.substr(0, dash);
    int ceil = 0, perch = -1, ex = -1, wb = -1;
    bool anySuffix = false;
    for (size_t p = dash; p != std::string::npos; ) {
      const size_t next = a.find('-', p + 1);
      const std::string sfx = a.substr(p + 1, next == std::string::npos
                                                  ? std::string::npos
                                                  : next - p - 1);
      if (sfx.rfind("ceil", 0) == 0) ceil = std::atoi(sfx.c_str() + 4);
      else if (sfx.rfind("perch", 0) == 0) perch = std::atoi(sfx.c_str() + 5);
      else if (sfx.rfind("wb", 0) == 0) wb = std::atoi(sfx.c_str() + 2);
      else if (sfx.rfind("ex", 0) == 0) ex = std::atoi(sfx.c_str() + 2);
      else return false;
      anySuffix = true;
      p = next;
    }
    if (head.rfind("pond", 0) == 0) {
      const std::string rad = head.substr(4);
      if (!rad.empty() && !std::isdigit((unsigned char)rad[0])) return false;
      r = {kLabPond, ex < 0 ? 1 : ex, a, false, ceil,
           rad.empty() ? 0 : std::atoi(rad.c_str()), perch, wb};
      return true;
    }
    if (!anySuffix) return false;                  // plain name: normal path
    const int s = head == "hill0" ? kLabHill : LabSceneFromName(head);
    if (s < 0) return false;
    const int base = head == "hill0" ? 0 : 1;
    r = {s, ex < 0 ? base : ex, a, false, ceil, 0, perch, wb};
    return true;
  };
  BenchRun pondRun{};
  if (sceneArg == "wp5") {
    // The re-scoped WP5's whole decision set, on worldgen's SMALLEST real pond
    // (r=68, 203,298 water voxels = 6.4x the particle pool), in one
    // invocation. Order matters only for the reader:
    //   ex0        — the CA alone, with its four defects fixed (merge a2e723e).
    //                The reference every other arm is judged against.
    //   ceil262144 — excite on, bound lifted to the pool. Reproduces the
    //                reported "it turns the whole lake into fluid".
    //   perch1 / perch0 — excite on, bounded, with and without the perch
    //                trigger. This is the open design question: does a FIXED
    //                CA still need excite to unstick perched water?
    // `worldlake` repeats the first three on the REAL worldgen against its
    // authored 347,832-voxel lake, because that is the scene the reported bug
    // was measured on. `hill` carries the trigger A/B's other half: the pond
    // is a flat body and the perch trigger was designed for a stepped ramp, so
    // a pond that does not need it proves nothing on its own.
    //
    // Ordered so the compiled-in fluid consts change as few times as possible
    // — each change is a full Tint recompile.
    runs.push_back({kLabPond, 0, "pond68-ex0", false, 0, 68, -1});
    runs.push_back({kLabWorldLake, 0, "worldlake-ex0", false, 0, 0, -1});
    runs.push_back({kLabPond, 1, "pond68-perch1", false, 0, 68, 1});
    runs.push_back({kLabWorldLake, 1, "worldlake-perch1", false, 0, 0, 1});
    runs.push_back({kLabHill, 1, "hill-perch1", false, 0, 0, 1});
    runs.push_back({kLabPond, 1, "pond68-perch0", false, 0, 68, 0});
    runs.push_back({kLabWorldLake, 1, "worldlake-perch0", false, 0, 0, 0});
    runs.push_back({kLabHill, 1, "hill-perch0", false, 0, 0, 0});
    runs.push_back({kLabPond, 1, "pond68-ceil262144", false, 262144, 68, -1});
    runs.push_back({kLabWorldLake, 1, "worldlake-ceil262144", false, 262144, 0,
                    -1});
  } else if (sceneArg == "wp5b") {
    // Follow-up to `wp5`, and both halves exist because the first sweep came
    // back CONFOUNDED in one place and UNDER-SAMPLED in another.
    //
    // The ceiling sweep, on `worldlake` rather than the pond: the pond never
    // reaches the bound at all (0 slots refused even with the ceiling lifted
    // to the whole pool), so it cannot price it. worldlake can — it saturates
    // the pool without one.
    //
    // The hill arms carry ceil262144 because at the shipped 32,000 the hill
    // pours 39,600 particles of its own and excite's budget is therefore
    // exactly zero: `wp5` measured 6,161 vs 4,051 candidates and 0 vs 0
    // emitted, which says nothing about the trigger. Lifting the ceiling is
    // what makes the ramp arm a real A/B.
    for (int c : {4000, 8000, 16000})
      runs.push_back({kLabWorldLake, 1, "worldlake-ceil" + std::to_string(c),
                      false, c, 0, -1});
    runs.push_back({kLabHill, 1, "hill-ceil262144-perch1", false, 262144, 0, 1});
    runs.push_back({kLabHill, 1, "hill-ceil262144-perch0", false, 262144, 0, 0});
  } else if (sceneArg == "wp5c") {
    // THE M3 ARM (docs/PLAN_water_master.md 1.3). The same two puncture scenes
    // WP5 measured, with the water-body ledger governing the lake instead of
    // the CA propagating pressure through it. Each pair is
    // {CA-only reference, water-body drain} on ONE scene in ONE invocation, so
    // the throughput and the frame cost are read off the same run rather than
    // against a table written on another day.
    runs.push_back({kLabPond, 0, "pond68-ex0", false, 0, 68, -1, 0});
    runs.push_back({kLabPond, 0, "pond68-ex0-wb1", false, 0, 68, -1, 1});
    runs.push_back({kLabWorldLake, 0, "worldlake-ex0", false, 0, 0, -1, 0});
    runs.push_back({kLabWorldLake, 0, "worldlake-ex0-wb1", false, 0, 0, -1, 1});
    runs.push_back({kLabWorldLake, 1, "worldlake-perch1-wb1", false, 0, 0, 1, 1});
  } else if (sceneArg == "pondsweep") {
    // One invocation, both sweeps, on a body that is already 4.5x the pool.
    for (int c : {10000, 20000, 40000, 80000})
      runs.push_back({kLabPond, 1, "pond24-ceil" + std::to_string(c), false, c,
                      24});
    for (int r : {16, 32, 48, 68})
      runs.push_back({kLabPond, 1, "pond" + std::to_string(r), false, 0, r});
  } else if (sceneArg != "pond" && parseRun(sceneArg, pondRun)) {
    runs.push_back(pondRun);
  } else if (sceneArg.empty() || sceneArg == "all") {
    for (int s = 0; s < kLabSceneCount; s++) {
      runs.push_back({s, 1, LabSceneName(s)});
      // The hill A/B at exciteMode 0 reproduces the reported mid-slope
      // trapdoor until WP3 closes it (plan §4.2).
      if (s == kLabHill) runs.push_back({s, 0, "hill0"});
    }
    runs.push_back({kLabPool, 1, "pool-settle", true});
  } else if (sceneArg == "hill0") {
    runs.push_back({kLabHill, 0, "hill0"});
  } else if (sceneArg == "pool-settle") {
    runs.push_back({kLabPool, 1, "pool-settle", true});
  } else if (sceneArg == "pours") {   // the five pre-WP5 pour scenes only
    for (int s = 0; s <= kLabSlosh; s++) {
      runs.push_back({s, 1, LabSceneName(s)});
      if (s == kLabHill) runs.push_back({s, 0, "hill0"});
    }
    runs.push_back({kLabPool, 1, "pool-settle", true});
  } else {
    int s = LabSceneFromName(sceneArg);
    if (s < 0) {
      std::fprintf(stderr,
                   "--fluid-bench: unknown scene '%s' (want basin|hill|hill0|"
                   "faucet|pool|slosh|pond[N]|worldlake|pours|all|wp5|"
                   "pondsweep|wp5c; any of these may carry -ceil<N> -perch<0|1> "
                   "-ex<0|1> -wb<0|1>)\n",
                   sceneArg.c_str());
      return 1;
    }
    runs.push_back({s, 1, sceneArg});
  }

  uint32_t waterId = 0;
  for (size_t i = 0; i < mats.size(); i++)
    if (mats[i].name == "water") { waterId = (uint32_t)i; break; }
  if (waterId == 0) {
    std::fprintf(stderr, "--fluid-bench: no 'water' material\n");
    return 1;
  }
  const uint32_t splashMats[4] = {waterId, 0, 0, 0};

  SetHarnessSnapshotDrain(true);  // headless tick loop: see test/support.h

  PassTimer timer;
  const bool haveTimer = timer.Init(ctx, 128);
  if (!haveTimer)
    std::printf("--fluid-bench: TimestampQuery unavailable — per-pass GPU "
                "times will be zero (wall-clock rows still valid)\n");

  // Offscreen 1080p target, once for all runs. Render cost is measured as
  // WaitIdle-bracketed wall time (the render pass has no query hooks); with
  // the queue drained before each render that is GPU time + submit overhead.
  const uint32_t W = 1920, H = 1080;
  rhi::Texture offscreen = ctx.device.CreateTexture(
      {W, H, 1}, rhi::TextureFormat::RGBA8Unorm,
      rhi::TextureUsage::RenderAttachment | rhi::TextureUsage::CopySrc,
      "labBench");
  rhi::TextureView view = offscreen.CreateView();

  const Tuning savedTuning = CurrentTuning();
  const uint32_t skyTick =
      (uint32_t)(0.30 * (double)TicksPerDay(savedTuning));  // fixed daylight

  std::string outPath = jsonPath.empty() ? "fluid_bench.json" : jsonPath;
  std::ostringstream json;
  json << "[\n";

  for (size_t ri = 0; ri < runs.size(); ri++) {
    const BenchRun& run = runs[ri];
    const int scene = run.scene;
    // BEFORE anything asks the scene how big it is: the pond's build volume,
    // plug tick and run length are all derived from the radius, so a sweep
    // that set it later would run each size on the previous size's schedule.
    if (run.radius) LabSetPondRadius(run.radius);
    const uint32_t N = LabSceneBenchTicks(scene);
    const uint32_t pourEnd = LabScenePourEnd(scene);

    // Per-run tuning: the lab exercises the full excite/settle loop
    // (exciteMode is the only live CPU-read fluid knob), and the day phase is
    // frozen with both water sinks off — the fluid-excite gate's pinning — so
    // the mass ledger cannot be drained by the evaporation reactions while
    // the bench watches it. Everything else stays at whatever tuning.json
    // says: measuring the live configuration is the point.
    {
      Tuning t = savedTuning;
      t.sim.fluidExciteMode = run.excite;
      t.dayNight.freeze = 1;
      t.dayNight.freezePhase = (int)(kDaySunrise + 1024u);
      // THE SLEEP RUN (plan §7 item 2). At stock tuning nothing settles in any
      // scene (§9's headline finding — the WP2 CFL problem), so "fluid GPU time
      // after settle" is unmeasurable there. These are the fluid-excite gate's
      // overrides verbatim (selftest_sim.cpp): a softer, damped, tension-free
      // water that a sealed pool can actually calm out of. Every sim.fluid*
      // value is a WGSL compile-time const, so the run has to recompile — the
      // F5 path, exactly as the lab's tuning watcher uses it.
      // WP2 shrank the override set to the settle trio: stock is CFL-honest
      // and zero-tension now, so the gate's old stiffness/cohesion/attract
      // overrides are simply stock (keep this block mirroring the gate).
      // WP3 shrank it again: settleEps 6.0 and wakeSpeed 24.0 ARE stock now
      // (the at-rest speed floor scales with gravity, and the owner's is 900),
      // so this run differs from stock by ONE knob — the sealed-box damping.
      if (run.settleTuning) t.sim.fluidDamping = 0.9f;
      if (run.ceiling) t.sim.fluidExciteCeiling = run.ceiling;
      if (run.perch >= 0) t.sim.fluidExcitePerch = run.perch;
      // The M3 arm. sim.waterBodyMode is a CPU-read knob (no recompile), so
      // it does not join the shader-reload triple below.
      if (run.water >= 0) t.sim.waterBodyMode = run.water;
      if (run.radius) LabSetPondRadius(run.radius);
      SetCurrentTuning(t);
      // Recompile only when the compiled-in fluid consts actually change: a
      // reload is seconds of Tint, and every other run wants stock tuning.
      static bool shadersSettleTuned = false;
      static int shadersCeiling = 0;
      static int shadersPerch = -1;
      if (run.settleTuning != shadersSettleTuned ||
          run.ceiling != shadersCeiling || run.perch != shadersPerch) {
        shadersCeiling = run.ceiling;
        shadersPerch = run.perch;
        if (!sim.ReloadShaders(ctx.device)) {
          std::fprintf(stderr, "--fluid-bench: shader reload FAILED\n");
          return 1;
        }
        shadersSettleTuned = run.settleTuning;
      }
    }

    sim.SetPassTimer(nullptr);  // a timed worldgen would dangle its queries
    // `worldlake` is the MAIN-WORLD arm and must see real terrain: the flat
    // slab has no lake. SetLabWorld gates both the shader's worldgen tap and
    // the CPU TerrainHeight mirror, so it has to move before the worldgen
    // submit and stay put for the whole run.
    World::SetLabWorld(LabSceneUsesLabWorld(scene));
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    if (haveTimer) {
      timer.ResetStats();
      sim.SetPassTimer(&timer);
    }

    Vec3 eye;
    float yaw = 0, pitch = 0;
    LabSceneCamera(scene, eye, yaw, pitch);
    Camera cam;
    cam.yaw = yaw;
    cam.pitch = pitch;
    // The CPU mirror follows this chunk; it must sit on the body being
    // measured or the mirror-fed paths (and the streaming wake) look at the
    // wrong half of the world.
    const LakeGeom lake = Lake();
    const IVec3 pc =
        scene == kLabWorldLake
            ? IVec3{lake.cx / (int)kChunk, lake.surfY / (int)kChunk,
                    lake.cz / (int)kChunk}
            : IVec3{CX / (int)kChunk, G / (int)kChunk, CZ / (int)kChunk};

    // ---- the standing-water sweep, hoisted -------------------------------
    // Used twice on the pond scenes: once at the plug tick to establish what
    // the body actually WAS (they pour nothing, so there is no spawn count to
    // audit against), and once at the end. `basinOut` is the hill scene's
    // catch-basin subtotal; null everywhere else.
    const bool hasBasin = scene == kLabHill;
    const IVec3 basinLo{248, G - 9, 248}, basinHi{269, G + 7, 263};
    auto inBasin = [&](int x, int y, int z) {
      return x >= basinLo.x && x <= basinHi.x && y >= basinLo.y &&
             y <= basinHi.y && z >= basinLo.z && z <= basinHi.z;
    };
    // DRAIN COMPLETION, and it is the acceptance criterion the frame
    // percentiles cannot express: how much of the body has actually reached
    // the sealed chamber. A drain that is in budget because nothing is moving
    // is not a drain. Counted as a subtotal of the same sweep — the chamber is
    // inside the audit box already (LabSceneBounds reaches kPondChamberLo).
    const bool hasChamber = scene == kLabPond || scene == kLabWorldLake;
    const int chCx = scene == kLabPond ? CX : lake.cx;
    const int chCz = scene == kLabPond ? CZ : lake.cz;
    const int chLo = scene == kLabPond ? kPondChamberLo : lake.chamberLo;
    const int chHi = scene == kLabPond ? kPondChamberTop : lake.chamberTop;
    auto inChamber = [&](int x, int y, int z) {
      return y >= chLo && y <= chHi && x >= chCx - kChamberHalf &&
             x <= chCx + kChamberHalf && z >= chCz - kChamberHalf &&
             z <= chCz + kChamberHalf;
    };
    uint64_t chamberEighths = 0;
    auto sweepStanding = [&](uint64_t* basinOut) -> uint64_t {
      IVec3 lo, hi;
      LabSceneBounds(scene, lo, hi);
      // AUDIT-ONLY widening: splash carries over a scene's walls and settles
      // on the slab outside the build volume — WP4 recorded pool's ledger as
      // LEAK for exactly this (53 eighths on the slab past the sweep). The
      // scene geometry still builds from the tight bounds; only the mass
      // audit looks wider.
      lo.x -= 12; lo.z -= 12; hi.x += 12; hi.z += 12;
      uint64_t total = 0;
      std::vector<uint32_t> cbuf((size_t)kChunkVol);
      for (int cy = lo.y / 16; cy <= hi.y / 16; cy++)
        for (int cz = lo.z / 16; cz <= hi.z / 16; cz++)
          for (int cx = lo.x / 16; cx <= hi.x / 16; cx++) {
            ReadVoxelsSync(ctx, world, World::SlotChunkIndex({cx, cy, cz}), 1,
                           cbuf.data(), "benchVox");
            for (uint32_t i = 0; i < kChunkVol; i++) {
              const int x = (int)(i % 16) + cx * 16,
                        y = (int)((i / 16) % 16) + cy * 16,
                        z = (int)(i / 256) + cz * 16;
              if (x < lo.x || x > hi.x || y < lo.y || y > hi.y || z < lo.z ||
                  z > hi.z)
                continue;
              if ((cbuf[i] & 0xFFFu) == waterId) {
                const uint64_t e = ((cbuf[i] >> 12) & 0xFu) + 1u;
                total += e;
                if (basinOut && hasBasin && inBasin(x, y, z)) *basinOut += e;
                if (hasChamber && inChamber(x, y, z)) chamberEighths += e;
              }
            }
          }
      return total;
    };

    // Per-tick series. renderMs is the whole offscreen frame; the three
    // attribution series below are the WP4 render breakdown (plan §7 item 5).
    // Without them "render is 11-27 ms" is a number with no owner: the frame
    // contains a fluid isosurface march, the rasterized splash/foam droplets,
    // and the ordinary world+sky raymarch, and only one of those is fluid
    // rendering. Sampled every kAttribEvery ticks so the extra passes cost
    // ~20% of the run rather than 3x.
    std::vector<double> renderNoFluidMs, renderWorldOnlyMs;
    std::vector<double> frameMs, renderMs;
    std::map<std::string, uint64_t> prevNs;
    std::map<std::string, std::vector<double>> passMs;
    std::vector<uint32_t> liveCurve, blockCurve, clampCurve;
    std::vector<uint32_t> seenCurve, candidCurve;
    uint64_t poured = 0, settledSum = 0, excitedSum = 0, deadSum = 0,
             binnedSum = 0, consumedSum = 0, emittedSum = 0, refusedSum = 0,
             setRefusedSum = 0, setUnstableSum = 0, setBlocksSum = 0,
             clampedSum = 0, exSeenSum = 0, exCandidSum = 0;
    int tickOfSettle = -1;
    double idleFluidMs = -1.0;
    uint32_t idleFluidTicks = 0;
    uint32_t liveEst = 0;
    uint32_t tick = 0;

    const uint32_t plugTick = LabScenePlugTick(scene);
    // The pond scenes' ledger input. Measured, not counted: the body is
    // standing water placed by CellOps, and the tick before the plug is the
    // only moment at which "how much water was there" has an unambiguous
    // answer.
    uint64_t plugStanding = 0;
    uint32_t liveAtPlug = 0, livePeakAfterPlug = 0, peakTickAfterPlug = 0;
    uint32_t liveAtPlugPlus1 = 0;
    uint32_t idleTicksBeforePlug = 0;

    for (uint32_t st = 1; st <= N; st++) {
      const double f0 = NowSeconds();
      std::vector<CellOp> cops;
      LabSceneBuildOps(scene, st, waterId, cops);
      if (plugTick != 0 && st == plugTick) {
        plugStanding = sweepStanding(nullptr);
        std::printf("    [t%u] plug pulled: %llu eighths standing (%llu water "
                    "voxels), %u live particles, %u idle ticks first\n",
                    st, (unsigned long long)plugStanding,
                    (unsigned long long)(plugStanding / 8), liveEst,
                    idleTicksBeforePlug);
        LabScenePlugOps(scene, cops);
      }
      std::vector<FluidSpawnOp> fs;
      LabScenePour(scene, st, liveEst, waterId, fs);
      SubmitTick(ctx, world, sim, ++tick, kDefaultSeed, {}, {}, cops, false,
                 pc, false, liveEst + fs.size() > 0, {}, 0, fs, liveEst,
                 splashMats);
      poured += fs.size();
      ctx.WaitIdle();
      ctx.ProcessEvents();
      if (haveTimer) {
        timer.Collect(ctx);
        for (const PassTimer::Stat& s : timer.Stats()) {
          const uint64_t d = s.totalNs - prevNs[s.name];
          prevNs[s.name] = s.totalNs;
          // A label only enters Stats() the first tick its pass is recorded, so
          // without this pad its series is SHIFTED against the tick index and
          // the per-tick views (the idle-cost median) read the wrong ticks.
          std::vector<double>& v = passMs[s.name];
          v.resize((size_t)st - 1, 0.0);
          v.push_back((double)d * 1e-6);
        }
      }
      // The whole 128-byte FA_* buffer: the old 64-byte read made fa[16]
      // (consumed) an out-of-bounds stack read.
      uint32_t fa[32] = {};
      rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0, fa,
                            128, "benchArgs");
      liveEst = std::min(fa[7], kFluidCap);  // exact after the WaitIdle
      liveCurve.push_back(liveEst);
      // ---- the excite-burst meter (WP5, Risk B) ---------------------------
      // The reported bug is a ONE-TICK spike, so a p50 and an end-state cannot
      // see it and neither can `liveMax` alone once the scene also pours. What
      // says "the whole lake converted at once" is the jump across the plug
      // tick and how many ticks the peak took to arrive.
      if (plugTick != 0) {
        if (st < plugTick && liveEst == 0 && fa[3] == 0) idleTicksBeforePlug++;
        if (st == plugTick) liveAtPlug = liveEst;
        if (st == plugTick + 1) liveAtPlugPlus1 = liveEst;
        if (st >= plugTick && liveEst > livePeakAfterPlug) {
          livePeakAfterPlug = liveEst;
          peakTickAfterPlug = st - plugTick;
        }
      }
      blockCurve.push_back(fa[3]);
      clampCurve.push_back(fa[18]);  // FA_CLAMPED: VMAX truncations this tick
      clampedSum += fa[18];
      settledSum += fa[10];
      excitedSum += fa[11];
      deadSum += fa[8];
      binnedSum += fa[15];
      consumedSum += fa[16];
      emittedSum += fa[9];
      refusedSum += fa[12];
      setBlocksSum += fa[13];     // FA_SETBLOCKS: blocks the scan picked
      seenCurve.push_back(fa[27]);
      candidCurve.push_back(fa[28]);
      exSeenSum += fa[27];        // FA_EXSEEN: settled liquid cells detect saw
      exCandidSum += fa[28];      // FA_EXCANDID: of those, trigger-satisfying
      setRefusedSum += fa[25];    // FA_SETREFUSED: of those, infeasible
      setUnstableSum += fa[26];   // FA_SETUNSTABLE: of those, excite-unstable
      if (tickOfSettle < 0 && pourEnd != ~0u && st > pourEnd + 2 &&
          fa[7] == 0 && fa[3] == 0)
        tickOfSettle = (int)st;

      const double r0 = NowSeconds();
      WriteRenderParams(ctx.queue, world, eye, cam, (float)W / H, true, 11.7f,
                        kFarFogDensity, (float)H, skyTick, liveEst);
      rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
      rhi::RenderPass rp =
          sim.BeginRenderPass(enc, view, rhi::TextureFormat::RGBA8Unorm, W, H);
      sim.DrawWorld(rp);
      sim.DrawParticles(rp);
      if (CurrentTuning().render.fluidSurface < 0.5f)
        sim.DrawFluid(rp, liveEst);
      rp.End();
      ctx.queue.Submit(enc.Finish());
      ctx.WaitIdle();
      const double r1 = NowSeconds();
      renderMs.push_back((r1 - r0) * 1000.0);
      frameMs.push_back((r1 - f0) * 1000.0);

      // ---- render attribution (every kAttribEvery ticks) -----------------
      // (b) the same frame with the fluid surface march switched off — passing
      //     fluidCount 0 makes raymarch.wgsl skip it wholesale, so
      //     renderMs - renderNoFluidMs IS the fluid march.
      // (c) (b) minus the rasterized droplets — so renderNoFluidMs -
      //     renderWorldOnlyMs is the splash/foam particle raster and
      //     renderWorldOnlyMs is the world+sky floor this scene can never go
      //     under. Same camera, same target: only the work differs.
      // Never on the LAST tick: these passes overwrite the offscreen target and
      // the screenshot below is taken from it.
      constexpr uint32_t kAttribEvery = 16;
      if (st % kAttribEvery == 0 && st != N) {
        for (int mode = 0; mode < 2; mode++) {
          const double a0 = NowSeconds();
          WriteRenderParams(ctx.queue, world, eye, cam, (float)W / H, true,
                            11.7f, kFarFogDensity, (float)H, skyTick, 0);
          rhi::CommandEncoder aenc = ctx.device.CreateCommandEncoder();
          rhi::RenderPass arp = sim.BeginRenderPass(
              aenc, view, rhi::TextureFormat::RGBA8Unorm, W, H);
          sim.DrawWorld(arp);
          if (mode == 0) sim.DrawParticles(arp);
          arp.End();
          ctx.queue.Submit(aenc.Finish());
          ctx.WaitIdle();
          const double a1 = NowSeconds();
          if (mode == 0) renderNoFluidMs.push_back((a1 - a0) * 1000.0);
          else renderWorldOnlyMs.push_back((a1 - a0) * 1000.0);
        }
      }
    }
    sim.SetPassTimer(nullptr);

    // ---- end state: the mass ledger (eighths in == out) --------------------
    // In: every spawned particle carries one eighth (8 per cell at rest
    // density — the fluid-settle gate's equivalence). Out: live particles'
    // carried fullness + standing water eighths inside the scene bounds.
    // Catch-basin capture (plan §5 acceptance, hill scenes only): the fraction
    // of the poured ledger that ended INSIDE the catch basin at end-of-run —
    // standing water voxels plus particles still swimming there. This is the
    // quantitative form of "the water sheets down and arrives": mid-slope
    // freeze shows up directly as capture loss.
    uint64_t basinEighths = 0;
    uint64_t carriedEighths = 0;
    // Excited water inside the chamber counts as drained too: at the moment
    // the run stops, a particle three cells above the chamber floor has
    // arrived by every meaning that matters, and whether it has settled yet is
    // a settleTicks artifact rather than drain progress.
    uint64_t chamberCarried = 0;
    if (liveEst > 0) {
      std::vector<uint32_t> pbuf((size_t)liveEst * kFluidParticleWords);
      rhi::ReadbackBlocking(ctx.device, ctx.queue,
                            world.fluidParticles[sim.Page()], 0, pbuf.data(),
                            pbuf.size() * 4, "benchEndP");
      for (uint32_t k = 0; k < liveEst; k++) {
        const uint32_t* pw = pbuf.data() + (size_t)k * kFluidParticleWords;
        const uint32_t f = (pw[18] >> 12) & 0x7u;
        carriedEighths += f;
        const int px = (int32_t)pw[0] >> 16, py = (int32_t)pw[1] >> 16,
                  pz = (int32_t)pw[2] >> 16;
        if (hasBasin && inBasin(px, py, pz)) basinEighths += f;
        if (hasChamber && inChamber(px, py, pz)) chamberCarried += f;
      }
    }
    chamberEighths = 0;   // the plug-tick sweep also ran through this counter
    const uint64_t standingEighths = sweepStanding(&basinEighths);
    // The pond scenes' ledger input is the body measured at the plug tick, not
    // a spawn count — they pour nothing.
    if (plugTick != 0) poured = plugStanding;
    const bool massExact = standingEighths + carriedEighths == poured;
    const double basinCapture =
        (hasBasin && poured) ? (double)basinEighths / (double)poured : -1.0;

    // ---- screenshot (look acceptance is judged on these, plan §9) ----------
    {
      rhi::Buffer shot = CreateBuffer(
          ctx.device, (uint64_t)W * H * 4,
          rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst, "labShot");
      rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
      rhi::TexelCopyTexture srcT{};
      srcT.texture = offscreen;
      rhi::TexelCopyBuffer dstB{};
      dstB.buffer = shot;
      dstB.bytesPerRow = W * 4;
      dstB.rowsPerImage = H;
      enc.CopyTextureToBuffer(srcT, dstB, {W, H, 1});
      ctx.queue.Submit(enc.Finish());
      std::vector<uint8_t> pixels((size_t)W * H * 4);
      const std::string bmp = "screenshot_lab_" + run.tag + ".bmp";
      if (rhi::ReadBufferBlocking(ctx.device, shot, 0, pixels.data(),
                                  pixels.size()) &&
          WriteBmpFile(bmp, pixels, W, H))
        std::printf("wrote %s\n", bmp.c_str());
    }

    // ---- report ------------------------------------------------------------
    auto avg = [](const std::vector<double>& v) {
      double s = 0;
      for (double d : v) s += d;
      return v.empty() ? 0.0 : s / (double)v.size();
    };
    std::printf(
        "fluid-bench %-7s excite %d: %u ticks | frame ms p50 %.2f p95 %.2f "
        "p99 %.2f | render ms avg %.2f | live max %u end %u | settle t%d | "
        "mass %s (%llu poured = %llu standing + %llu carried)\n",
        run.tag.c_str(), run.excite, N, Pct(frameMs, 0.50), Pct(frameMs, 0.95),
        Pct(frameMs, 0.99), avg(renderMs),
        *std::max_element(liveCurve.begin(), liveCurve.end()), liveEst,
        tickOfSettle, massExact ? "EXACT" : "LEAK",
        (unsigned long long)poured, (unsigned long long)standingEighths,
        (unsigned long long)carriedEighths);
    // ---- THE EXCITE BURST (WP5 Risk B) ------------------------------------
    // The user-visible bug in one line: how much of a still body converted to
    // particles when the plug came out, how fast, and whether it hit the pool
    // ceiling. `+1t` against `peak` is the whole diagnosis — a peak reached on
    // the tick after the disturbance is a burst; a peak reached 200 ticks
    // later is a drain.
    if (plugTick != 0) {
      const uint64_t bodyVox = plugStanding / 8;
      std::printf(
          "    excite burst: body %llu vox (%llu eighths) | live at plug %u -> "
          "+1t %u -> peak %u at +%ut (%.1f%% of pool, %.1f%% of body) | frame "
          "at peak p99 %.2f ms\n",
          (unsigned long long)bodyVox, (unsigned long long)plugStanding,
          liveAtPlug, liveAtPlugPlus1, livePeakAfterPlug, peakTickAfterPlug,
          100.0 * (double)livePeakAfterPlug / (double)kFluidCap,
          plugStanding ? 100.0 * (double)livePeakAfterPlug /
                             (double)plugStanding
                       : 0.0,
          Pct(frameMs, 0.99));
      // Frame percentiles over the DRAIN WINDOW only: the build ticks (tens of
      // thousands of CellOps each) and the idle settle window would otherwise
      // dominate a whole-run p50 and hide the thing being measured.
      //
      // And the IDLE window beside it, which is the counterfactual the drain
      // number is meaningless without: the same camera, the same world, the
      // same body of water — with it asleep. Everything above that line is
      // what the fluid costs; everything under it is this scene's floor and no
      // amount of seam work can touch it.
      if (frameMs.size() > plugTick) {
        const size_t buildEnd = (size_t)LabSceneBuildEndTick(scene);
        std::vector<double> idle(frameMs.begin() + (ptrdiff_t)buildEnd,
                                 frameMs.begin() + (ptrdiff_t)plugTick - 1);
        std::vector<double> drain(frameMs.begin() + (ptrdiff_t)plugTick - 1,
                                  frameMs.end());
        if (!idle.empty())
          std::printf("    idle window frame ms:  p50 %.2f p95 %.2f "
                      "(body asleep, same camera — the scene's floor)\n",
                      Pct(idle, 0.50), Pct(idle, 0.95));
        std::printf("    drain window frame ms: p50 %.2f p95 %.2f p99 %.2f "
                    "max %.2f over %u ticks\n",
                    Pct(drain, 0.50), Pct(drain, 0.95), Pct(drain, 0.99),
                    *std::max_element(drain.begin(), drain.end()),
                    (unsigned)drain.size());
      }
      // DRAIN COMPLETION. Frame percentiles alone cannot tell "in budget
      // because the drain is cheap" from "in budget because nothing moved":
      // this is how much of the body has reached the sealed chamber by
      // end-of-run, settled voxels plus particles already inside it.
      if (hasChamber) {
        const uint64_t arrived = chamberEighths + chamberCarried;
        std::printf("    drain completion: %llu of %llu eighths in the chamber "
                    "(%.2f%% of the body) = %llu settled + %llu still "
                    "particles, over %u ticks after the plug\n",
                    (unsigned long long)arrived,
                    (unsigned long long)plugStanding,
                    plugStanding ? 100.0 * (double)arrived /
                                       (double)plugStanding
                                 : 0.0,
                    (unsigned long long)chamberEighths,
                    (unsigned long long)chamberCarried, N - plugTick);
      }
    }
    // The CFL-honesty probe (plan §5 item 1): node-substeps the VMAX clamp
    // truncated. Near-zero in steady flow is the acceptance; a persistent
    // count means the stiffness/substep budget is dishonest again.
    std::printf(
        "    VMAX clamp engagements: total %llu | per-tick max %u | ticks>0 "
        "%u of %u\n",
        (unsigned long long)clampedSum,
        clampCurve.empty()
            ? 0u
            : *std::max_element(clampCurve.begin(), clampCurve.end()),
        (unsigned)std::count_if(clampCurve.begin(), clampCurve.end(),
                                [](uint32_t c) { return c != 0; }),
        (unsigned)clampCurve.size());
    // ---- THE DRAIN COLUMN (WP5) -------------------------------------------
    // What the plug column actually looks like at end of run, bottom to top,
    // as a run-length string: '.' air, '~' water (with its fullness digit),
    // '#' anything solid. The reach probe says WHY nothing excited; this says
    // WHAT the world looks like, and between them there is no room left to
    // guess. Two columns: the one directly over the drain, and one 8 voxels
    // out, which is where a lateral trigger would have to act.
    if (plugTick != 0) {
      const int cx = scene == kLabPond ? CX : lake.cx;
      const int cz = scene == kLabPond ? CZ : lake.cz;
      IVec3 blo, bhi;
      LabSceneBounds(scene, blo, bhi);
      std::vector<uint32_t> cbuf((size_t)kChunkVol);
      for (int probe = 0; probe < 2; probe++) {
        const int px = cx + (probe ? 8 : 0);
        std::string col;
        int lastCy = INT32_MIN;
        for (int y = blo.y; y <= bhi.y; y++) {
          const int cy = y / 16;
          if (cy != lastCy) {
            ReadVoxelsSync(ctx, world,
                           World::SlotChunkIndex({px / 16, cy, cz / 16}), 1,
                           cbuf.data(), "benchCol");
            lastCy = cy;
          }
          const uint32_t w =
              cbuf[(size_t)(y % 16) * 16 + (size_t)(px % 16) +
                   (size_t)(cz % 16) * 256];
          const uint32_t m = w & 0xFFFu;
          if (m == 0) col += '.';
          else if (m == waterId) col += (char)('1' + ((w >> 12) & 0xFu));
          else col += '#';
        }
        std::printf("    drain column x%+d (y %d..%d, bottom first): %s\n",
                    probe ? 8 : 0, blo.y, bhi.y, col.c_str());
      }
    }
    // The excite REACH probe (WP5). Deleting CA liquid movement made the seam
    // the only thing that can move water, so "the pond did not drain" now has
    // three causes that look identical from the outside and are fixed in three
    // different places: the chunk never woke (seen == 0), no trigger matched
    // (seen high, candidates 0), or the burst bound refused it (candidates
    // high, refused high).
    std::printf("    excite reach: cells seen %llu -> candidates %llu -> "
                "emitted %llu eighths | slots refused %llu\n",
                (unsigned long long)exSeenSum, (unsigned long long)exCandidSum,
                (unsigned long long)excitedSum,
                (unsigned long long)refusedSum);
    // The SEAM FLOW (WP3). "nothing settled" has two opposite causes and this
    // line separates them: `picked 0` means the water never went calm, while
    // `picked N refused N` means it did and settleCheck turned it down (an
    // infeasible column, or a resulting cell that would immediately satisfy an
    // excite trigger). `settled` vs `re-excited` is the thrash meter — the two
    // being close means every eighth is converting over and over.
    std::printf("    seam flow: blocks picked %llu = %llu infeasible + %llu "
                "unstable + %llu committed | eighths settled %llu re-excited "
                "%llu binned %llu\n",
                (unsigned long long)setBlocksSum,
                (unsigned long long)setRefusedSum,
                (unsigned long long)setUnstableSum,
                (unsigned long long)(setBlocksSum - setRefusedSum -
                                     setUnstableSum),
                (unsigned long long)settledSum, (unsigned long long)excitedSum,
                (unsigned long long)binnedSum);
    if (hasBasin)
      std::printf("    basin capture: %.1f%% (%llu of %llu eighths inside the "
                  "catch basin)\n",
                  basinCapture * 100.0, (unsigned long long)basinEighths,
                  (unsigned long long)poured);
    // ---- IDLE COST (plan §7 item 2) ---------------------------------------
    // The sum of every fluid pass on the ticks where the GPU-owned live count
    // read ZERO. The tick tables are still RECORDED on those ticks — the CPU's
    // fluidCount is monotone precisely so that recording never depends on
    // readback timing — so this is the number that says whether "recorded but
    // asleep" really is free.
    {
      // MEDIAN, and tick 1 excluded: tick 1 carries the scene's whole CellOp
      // build (43,400 ops for hill) and the first-tick pipeline warm-up, which
      // an average of a handful of samples is entirely at the mercy of.
      std::vector<double> idle;
      for (size_t i = 1; i < liveCurve.size(); i++) {
        if (liveCurve[i] != 0) continue;
        double s = 0.0;
        for (auto& [name, v] : passMs)
          if (i < v.size() && name.rfind("fluid", 0) == 0) s += v[i];
        idle.push_back(s);
      }
      if (!idle.empty())
        std::printf(
            "    idle fluid GPU: %.4f ms/tick median over %u live==0 ticks "
            "(tables recorded, nothing alive)\n",
            Pct(idle, 0.50), (unsigned)idle.size());
      idleFluidMs = idle.empty() ? -1.0 : Pct(idle, 0.50);
      idleFluidTicks = (uint32_t)idle.size();
    }
    std::printf(
        "    render split: fluid march %.2f ms | droplet raster %.2f ms | "
        "world+sky %.2f ms  (of %.2f ms)\n",
        avg(renderMs) - avg(renderNoFluidMs),
        avg(renderNoFluidMs) - avg(renderWorldOnlyMs), avg(renderWorldOnlyMs),
        avg(renderMs));
    for (auto& [name, v] : passMs)
      if (avg(v) > 0.0005)
        std::printf("    %-24s avg %7.3f ms  p95 %7.3f ms\n", name.c_str(),
                    avg(v), Pct(v, 0.95));

    json << "  {\n    \"scene\": \"" << run.tag << "\",\n"
         << "    \"exciteMode\": " << run.excite << ",\n"
         << "    \"ticks\": " << N << ",\n"
         << "    \"tuning\": {\"stiffness\": " << savedTuning.sim.fluidStiffness
         << ", \"gravity\": " << savedTuning.sim.fluidGravity
         << ", \"eosPower\": " << savedTuning.sim.fluidEosPower
         << ", \"cohesion\": " << savedTuning.sim.fluidCohesion
         << ", \"attractSame\": " << savedTuning.sim.fluidAttractSame
         << ", \"attractDiff\": " << savedTuning.sim.fluidAttractDiff
         << ", \"viscosity\": " << savedTuning.sim.fluidViscosity
         << ", \"damping\": " << savedTuning.sim.fluidDamping << "},\n";
    json << "    \"frameMs\": {\"p50\": " << Pct(frameMs, 0.50)
         << ", \"p95\": " << Pct(frameMs, 0.95)
         << ", \"p99\": " << Pct(frameMs, 0.99) << "},\n";
    json << "    \"renderMsWall\": {\"avg\": " << avg(renderMs)
         << ", \"p95\": " << Pct(renderMs, 0.95) << "},\n";
    json << "    \"renderSplitMs\": {\"fluidMarch\": "
         << avg(renderMs) - avg(renderNoFluidMs)
         << ", \"dropletRaster\": "
         << avg(renderNoFluidMs) - avg(renderWorldOnlyMs)
         << ", \"worldSky\": " << avg(renderWorldOnlyMs) << "},\n";
    json << "    \"passesMs\": {";
    bool first = true;
    for (auto& [name, v] : passMs) {
      if (!first) json << ", ";
      first = false;
      json << "\"" << name << "\": {\"avg\": " << avg(v)
           << ", \"p95\": " << Pct(v, 0.95) << "}";
    }
    json << "},\n";
    json << "    \"exciteReach\": {\"cellsSeen\": " << exSeenSum
         << ", \"candidates\": " << exCandidSum << ", \"seenCurve\": [";
    for (size_t i = 0; i < seenCurve.size(); i++)
      json << (i ? "," : "") << seenCurve[i];
    json << "], \"candidCurve\": [";
    for (size_t i = 0; i < candidCurve.size(); i++)
      json << (i ? "," : "") << candidCurve[i];
    json << "]},\n";
    json << "    \"tickOfSettle\": " << tickOfSettle << ",\n";
    json << "    \"clampEngagements\": {\"total\": " << clampedSum
         << ", \"perTickMax\": "
         << (clampCurve.empty()
                 ? 0u
                 : *std::max_element(clampCurve.begin(), clampCurve.end()))
         << ", \"ticksNonzero\": "
         << std::count_if(clampCurve.begin(), clampCurve.end(),
                          [](uint32_t c) { return c != 0; })
         << "},\n";
    json << "    \"basinCapture\": " << basinCapture << ",\n";
    if (plugTick != 0) {
      std::vector<double> drain(
          frameMs.begin() +
              (size_t)std::min<uint32_t>(plugTick - 1, (uint32_t)frameMs.size()),
          frameMs.end());
      std::vector<double> idleW(
          frameMs.begin() + (ptrdiff_t)std::min<size_t>(
                                LabSceneBuildEndTick(scene), frameMs.size()),
          frameMs.begin() + (ptrdiff_t)std::min<size_t>(plugTick - 1,
                                                        frameMs.size()));
      json << "    \"idleFrameMs\": {\"p50\": " << Pct(idleW, 0.50)
           << ", \"p95\": " << Pct(idleW, 0.95) << "},\n";
      json << "    \"burst\": {\"exciteCeiling\": "
           << CurrentTuning().sim.fluidExciteCeiling
           << ", \"exciteRate\": " << CurrentTuning().sim.fluidExciteRate
           << ", \"excitePerch\": " << CurrentTuning().sim.fluidExcitePerch
           << ", \"exciteMode\": " << run.excite
           << ", \"plugTick\": " << plugTick
           << ", \"bodyEighths\": " << plugStanding
           << ", \"pondRadius\": " << (scene == kLabPond ? sPondRadius : 0)
           << ", \"chamberEighths\": " << chamberEighths
           << ", \"chamberCarried\": " << chamberCarried
           << ", \"idleTicksBeforePlug\": " << idleTicksBeforePlug
           << ", \"liveAtPlug\": " << liveAtPlug
           << ", \"liveAtPlugPlus1\": " << liveAtPlugPlus1
           << ", \"livePeak\": " << livePeakAfterPlug
           << ", \"peakAtTicksAfterPlug\": " << peakTickAfterPlug
           << ", \"drainFrameMs\": {\"p50\": " << Pct(drain, 0.50)
           << ", \"p95\": " << Pct(drain, 0.95)
           << ", \"p99\": " << Pct(drain, 0.99) << ", \"max\": "
           << (drain.empty() ? 0.0
                             : *std::max_element(drain.begin(), drain.end()))
           << "}},\n";
    }
    json << "    \"idleFluidMs\": " << idleFluidMs
         << ", \"idleFluidTicks\": " << idleFluidTicks << ",\n";
    json << "    \"massLedger\": {\"pouredEighths\": " << poured
         << ", \"standingEighths\": " << standingEighths
         << ", \"carriedEighths\": " << carriedEighths
         << ", \"exact\": " << (massExact ? "true" : "false")
         << ", \"settledFlow\": " << settledSum
         << ", \"excitedFlow\": " << excitedSum
         << ", \"deadParticles\": " << deadSum
         << ", \"binned\": " << binnedSum << ", \"consumed\": " << consumedSum
         << ", \"emittedDroplets\": " << emittedSum
         << ", \"exciteRefused\": " << refusedSum
         << ", \"settleBlocks\": " << setBlocksSum
         << ", \"settleRefused\": " << setRefusedSum
         << ", \"settleUnstable\": " << setUnstableSum << "},\n";
    json << "    \"liveMax\": "
         << *std::max_element(liveCurve.begin(), liveCurve.end())
         << ", \"liveEnd\": " << liveEst << ",\n";
    json << "    \"liveCurve\": [";
    for (size_t i = 0; i < liveCurve.size(); i++)
      json << (i ? "," : "") << liveCurve[i];
    json << "],\n    \"blockCurve\": [";
    for (size_t i = 0; i < blockCurve.size(); i++)
      json << (i ? "," : "") << blockCurve[i];
    json << "]\n  }" << (ri + 1 < runs.size() ? "," : "") << "\n";
  }
  json << "]\n";
  SetCurrentTuning(savedTuning);

  std::ofstream out(outPath, std::ios::binary);
  out << json.str();
  out.close();
  std::printf("fluid-bench: wrote %s\n", outPath.c_str());
  return ctx.ReportVkValidation("--fluid-bench") > 0 ? 1 : 0;
}

// ---- live-tuning file plumbing ---------------------------------------------

int64_t LabFileMtimeNs(const std::string& path) {
  std::error_code ec;
  const auto t = std::filesystem::last_write_time(path, ec);
  if (ec) return -1;
  return (int64_t)t.time_since_epoch().count();
}

bool LabPatchTuningJson(const std::string& path, const Tuning& t,
                        int64_t* lastLoadedMtimeNs) {
  // Never overwrite a file newer than the one this process last loaded: that
  // is the tuner's edit, and the watcher is about to apply it. Last-writer-
  // wins, decided by mtime (lab.h block comment).
  const int64_t diskMtime = LabFileMtimeNs(path);
  if (diskMtime < 0) return false;
  if (lastLoadedMtimeNs && diskMtime > *lastLoadedMtimeNs) {
    std::printf("lab: tuning.json is newer on disk than in memory — ImGui "
                "edit NOT written (the watcher will load the file's values)\n");
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  std::string text = ss.str();
  in.close();

  // Patch the value text of one "key": <number> pair in place. Text surgery
  // rather than a re-serialize, so the tuner's formatting and every key this
  // panel does not own survive byte-for-byte.
  auto patch = [&text](const char* key, double v, bool isInt) -> bool {
    const std::string needle = std::string("\"") + key + "\"";
    size_t k = text.find(needle);
    if (k == std::string::npos) return false;
    size_t colon = text.find(':', k + needle.size());
    if (colon == std::string::npos) return false;
    size_t vs = colon + 1;
    while (vs < text.size() && (text[vs] == ' ' || text[vs] == '\t')) vs++;
    size_t ve = vs;
    while (ve < text.size() &&
           (std::isdigit((unsigned char)text[ve]) || text[ve] == '-' ||
            text[ve] == '+' || text[ve] == '.' || text[ve] == 'e' ||
            text[ve] == 'E'))
      ve++;
    if (ve == vs) return false;
    char buf[48];
    if (isInt)
      std::snprintf(buf, sizeof buf, "%lld", (long long)llround(v));
    else
      std::snprintf(buf, sizeof buf, "%g", v);
    text.replace(vs, ve - vs, buf);
    return true;
  };
  bool ok = true;
  ok &= patch("fluidGravity", t.sim.fluidGravity, false);
  ok &= patch("fluidStiffness", t.sim.fluidStiffness, false);
  ok &= patch("fluidEosPower", (double)t.sim.fluidEosPower, true);
  ok &= patch("fluidCohesion", t.sim.fluidCohesion, false);
  ok &= patch("fluidAttractSame", t.sim.fluidAttractSame, false);
  ok &= patch("fluidAttractDiff", t.sim.fluidAttractDiff, false);
  ok &= patch("fluidViscosity", t.sim.fluidViscosity, false);
  ok &= patch("fluidDamping", t.sim.fluidDamping, false);
  if (!ok) {
    std::fprintf(stderr, "lab: tuning.json fluid keys not found — file NOT "
                         "written\n");
    return false;
  }
  std::ofstream outF(path, std::ios::binary | std::ios::trunc);
  if (!outF) return false;
  outF << text;
  outF.close();
  if (lastLoadedMtimeNs) *lastLoadedMtimeNs = LabFileMtimeNs(path);
  return true;
}

}  // namespace sandvox
