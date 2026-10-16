#pragma once
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "math3d.h"
#include "sim/world.h"

// flowfield.h — THE TERRAIN FLOW FIELD: which way the map's water runs, solved
// from the SHAPE of the water (DESIGN.md §9d.6).
//
// WHY THIS EXISTS BESIDE currentprim.h. The primitive list is the right tool
// for EVENTS (a drain's sink and whirlpool: a point, a radius, a decay) and the
// wrong one for TERRAIN. Its stream arm laid a straight slab along each 6.4 m of
// a river's centreline with one speed for the whole reach, so a river flowed
// like a conveyor belt: the same speed in a narrows as in a pool, as fast over
// the shallow bank as over the deep channel, and dead still the moment it
// reached a lake. Its bed-gradient lattice was worse: any pond on a slope got a
// uniform downhill current, which a closed basin cannot have.
//
// What decides how water moves is the shape it is in, so this field is solved
// FROM that shape. Depth-averaged steady flow with Manning friction,
// linearised: discharge per unit width q = -T grad(phi) with transmissivity
// T = h^(5/3), continuity div(q) = 0, and the depth-averaged velocity
// v = q / h = -h^(2/3) grad(phi). On a node grid over the map's water
// (worldmap::WaterColumnAt -- rivers, lakes, the sea) that is one sparse
// Laplacian, relaxed by SOR, and what falls out of it is everything the
// conveyor belt lacked:
//   * continuity: a channel that narrows or shoals speeds up, one that widens
//     into a pool slows down, with no rule written for either;
//   * the cross-section: v ~ h^(2/3), so the deep thalweg runs and the shallow
//     margin barely moves, and the bank itself is zero;
//   * lakes: a river mouth spreads into the lake as a fan, an outlet draws the
//     water toward it, and through-flow takes the shape of the lake between;
//   * closed water (a pond, a pool the map does not route) has no boundary
//     that drives it, so it is exactly still.
//
// THE BOUNDARY CONDITIONS come from the map network, not from the window:
// along a river, phi = C_r - G_r * s (s = arc length along its baked
// centreline, G_r from Chezy on the river's own fall and depth); in a lake or
// the sea, the analytic potential of its inlets (2D sources) and outlets
// (sinks), q = Q / (pi r). The constants C_r are chained downstream -> upstream
// (sea, then the lakes' outlets, then the rivers into them, then tributaries
// off their parents) so a river's potential meets its lake's at the mouth.
// Those analytic values are also the solver's starting guess, which is why a
// few hundred SOR sweeps converge.
//
// WORLD-ANCHORED, SO A WINDOW MOVE CANNOT TOUCH IT (the `rivers` gate's pass J,
// ticket 2026-10-11_18-39-38). The solve runs on fixed world BLOCKS -- cores
// kFlowBlock wide on a world-aligned lattice, each solved over its core plus a
// half-core apron -- and the field at a node is the tent blend of the (up to)
// four blocks whose centres surround it. Every quantity is a pure function of
// (map, seed, scale, world node), so the window only chooses WHICH nodes are
// uploaded, never their values. A block is solved once and cached.
//
// AUTHORITY. Pure function of (map, seed, tuning, world coordinates), computed
// on the tick thread when the window first needs it -- so it carries the sim
// licence the stream arm did: CurrentAtCpuSim (players, NPCs afloat) reads it.
// The GPU sim kernels do not: currentAtQ sums the primitive list only, and
// sim.currentMode is off. The renderer reads the uploaded window grid
// (common.wgsl flowGridAt, binding `flowGrid`).
//
// COST scales with travel, not with ticks: nothing runs while the window holds
// still; a window move re-composes ~4,500 nodes from cached blocks; a NEW block
// (one per ~51 m of travel, 15-35 ms: the column probe dominates) is solved on
// a BACKGROUND thread as soon as the window comes within half a block of
// needing it, and only joined when a composition actually needs it -- which
// is deterministic whatever the timing, because a block's result is a pure
// function of its world position. Never per frame.

namespace rhi {
class Buffer;
class Queue;
}  // namespace rhi

namespace sandvox {

// Cells between grid nodes. 0.8 m: a 4 m brook still gets five nodes across.
constexpr int kFlowStep = 8;
// A block's core width, cells, and the tent spacing of the blend. Each block is
// solved over [centre - kFlowBlock, centre + kFlowBlock], i.e. its core plus a
// half-core apron on every side, so the solve's own edge (the least trusted
// part of it) always sits where the tent gives it zero weight.
constexpr int kFlowBlock = 512;
constexpr int kFlowBlockNodes = 2 * kFlowBlock / kFlowStep + 1;   // 129
// The uploaded window grid: world-aligned nodes from one step before the
// window's low corner to one step past its high one.
constexpr int kFlowWinNodes = (int)kWorldN / kFlowStep + 3;        // 67
// GPU layout (u32 words; common.wgsl flowGridAt mirrors it):
//   [0] origin x (world cells, i32)   [1] origin z   [2] nodes per axis (0 =
//   no field)   [3] reserved
//   then per node, row-major in z: two words
//     w0 = vx | vz << 16      i16 each, 1/kFlowVelQ cells/s
//     w1 = surf | bed << 16   i16 each, world cells; surf = kFlowDry for land
constexpr uint32_t kFlowHeaderWords = 4;
constexpr uint32_t kFlowWords =
    kFlowHeaderWords + (uint32_t)(kFlowWinNodes * kFlowWinNodes * 2);
constexpr int kFlowVelQ = 128;
constexpr int kFlowDry = -32768;

// THE MEASURED FLOW (DESIGN.md §9d.12): which way the water the simulation
// is ACTUALLY moving goes, recorded on the GPU. The solved field above is a
// function of the MAP, so it cannot see a dam somebody breached; this one is a
// function of the moves. The renderers draw it over the solved field, and
// since 2026-10-15 the bodies the water holds feel it too: the snapshot ring
// copies GRID (World::EncodeReadbacks), SubmitTick publishes the copy owed to
// the tick (PublishMeasured below, World::kSnapshotLatency ticks late) and
// CurrentAtCpuSim blends it in. One buffer (`flowMeasure`), three regions, all
// on the node lattice above (a node every kFlowStep cells, the window grid's
// kFlowWinNodes per axis):
//   ACC   per toroidal slot (world node mod kFlowMeasN), four i32: the net
//         horizontal liquid flux this tick (x, z; eighths x cells, scaled by
//         kFlowMeasAccQ) and the highest / lowest cell that moved (y + bias,
//         bias - y; 0 = nothing moved). Atomically ADDED / MAXed by the CA
//         (sim_step.wgsl) and the MPM `mark` pass, zeroed by a Fill after the
//         fold. Integer adds and maxes commute, so the sum is a function of
//         the moves and not of the schedule; no GPU sim kernel reads it.
//   STATE per slot, four words: the smoothed velocity (vx, vz, i32 in
//         1/16384 cells/s -- the fold is INTEGER because bodies read its
//         output, flow_measure.wgsl's AUTHORITY note), the world node the slot
//         holds (a tag: the window moves under it), and the water band (top |
//         bot, i16 each).
//   GRID  the render's view, the flowGrid layout exactly: a 4-word header
//         ([0] origin x [1] origin z [2] nodes per axis, 0 = no field [3] the
//         tick it was folded) and two words a node, w0 = vx | vz (i16,
//         1/kFlowVelQ cells/s), w1 = top | bot (i16 world cells; top =
//         kFlowDry where nothing flows).
// The fold (flow_measure.wgsl) runs on ticks the CA or the fluid seam ran
// and for a tail after (Simulation::EncodeTick), so a settled world records
// nothing; when the tail ends one Fill zeroes STATE + GRID.
constexpr int kFlowMeasN = kFlowWinNodes;
constexpr uint32_t kFlowMeasSlots = (uint32_t)(kFlowMeasN * kFlowMeasN);
constexpr uint32_t kFlowMeasAccWords = kFlowMeasSlots * 4u;
constexpr uint32_t kFlowMeasStateBase = kFlowMeasAccWords;
constexpr uint32_t kFlowMeasGridBase = kFlowMeasStateBase + kFlowMeasSlots * 4u;
constexpr uint32_t kFlowMeasGridWords = kFlowHeaderWords + kFlowMeasSlots * 2u;
constexpr uint32_t kFlowMeasWords = kFlowMeasGridBase + kFlowMeasGridWords;
constexpr int kFlowMeasAccQ = 16;
// The fold's dispatch: 8 x 8 threads a group, one thread per grid node.
constexpr uint32_t kFlowMeasGroups = (uint32_t)(kFlowMeasN + 7) / 8u;
// Ticks the fold keeps running after the last tick anything fed it: long
// enough for the slowest decay to fall under the fold's zero snap
// (flow_measure.wgsl FLOWM_TAU_FALL_S x ~7), so the closing Fill is not a step.
constexpr uint32_t kFlowMeasTailTicks = 300;

class WaterFlowField {
 public:
  // Re-compose the window grid when the window's XZ origin (chunks), the seed,
  // the map or the flow scale changed. Solves any block that has never been
  // needed before, synchronously, and pre-solves at most one block of the ring
  // the window is about to need.
  void Update(IVec3 originChunks, uint32_t seed);
  // Same, against an explicit window base in world cells (gates).
  void UpdateAt(int baseX, int baseZ, uint32_t seed);
  void Clear();
  // Wait for every background solve. Called before the current world map is
  // replaced (worldmap.cpp), because a solve reads it.
  void Quiesce();

  // The field at a world-voxel position, cells/s (horizontal; y is 0). Bilinear
  // over the QUANTISED window grid -- the same words and the same arithmetic
  // flowGridAt reads, so the player feels the current the surface shows. Zero
  // outside the window grid, over land, above the surface and under the bed.
  Vec3 Sample(Vec3 posVox) const;

  const std::vector<uint32_t>& Words() const { return words_; }
  // Bumped every time Words() changes; the uploader compares it.
  uint32_t Version() const { return version_; }
  // Blocks solved since the last Clear, for traces and gates.
  uint32_t BlocksSolved() const { return solved_; }

  struct Block;
  struct Net;

 private:
  const Block& BlockAt(int bx, int bz, uint32_t seed);
  // Start a background solve for every block of the ring the window would
  // need half a block from here, up to kFlowMaxPending at once.
  void Prefetch(int ox, int oz, uint32_t seed);
  void Harvest();

  std::vector<uint32_t> words_;
  uint32_t version_ = 0;
  uint32_t solved_ = 0;
  bool built_ = false;
  int baseX_ = 0, baseZ_ = 0;
  uint32_t seed_ = 0;
  uint32_t mapHash_ = 0;
  float scale_ = -1.0f;
  std::shared_ptr<Net> net_;
  std::map<std::pair<int, int>, std::shared_ptr<Block>> blocks_;
  std::map<std::pair<int, int>, std::future<std::shared_ptr<Block>>> pending_;
};

// THE field, for the same reason CurrentPrims() is a global: SubmitTick
// advances it, WriteRenderParams uploads it, the player and NPCs sample it.
WaterFlowField& WaterFlow();
// WaterFlow().Quiesce(), for worldmap.cpp, which must not include this header's
// world.h dependency chain just to call it.
void WaterFlowQuiesce();

namespace flowfield {
// Registers the GPU buffer (renderBGL 48 / shadowBGL 35, kFlowWords * 4
// bytes). Owned by Simulation.
void SetBuffer(const rhi::Buffer* buf);
// Writes WaterFlow().Words() when they changed since the last upload. Called
// from WriteRenderParams, the one place every drawing path sets its params.
void Upload(const rhi::Queue& queue);

// THE MEASURED FLOW at a world position, cells/s (horizontal), from a copy of
// the GRID region (`grid` = kFlowMeasGridWords words: the published snapshot's,
// or a gate's readback; nullptr = no field). TRANSCRIBED from common.wgsl
// flowMeasAt -- the same bilinear weights over the same quantised words and
// the same band gate.
Vec3 SampleMeasured(const uint32_t* grid, Vec3 posVox);
// The same field for a BODY spanning [yLo, yHi] vertically (world cells): the
// record is a BAND of rows the water moved in (the CA levels a body of water
// by its top cells), and a body is carried by it when any part of it is in
// that band, not only its sample point. yLo == yHi == posVox.y is
// SampleMeasured exactly.
Vec3 SampleMeasuredSpan(const uint32_t* grid, Vec3 posVox, float yLo, float yHi);
// THE PUBLISHED GRID: the snapshot's copy of GRID as of World::Snap() --
// exactly World::kSnapshotLatency ticks behind the tick being assembled, the
// fixed latency every gameplay readback has (DESIGN.md §2) -- installed by
// SubmitTick right after it publishes the snapshot, read by CurrentAtCpuSim.
// That fixed latency, plus the integer fold, is what makes it legal for a
// body the tick owns (an NPC afloat) to read: its value at tick T is a pure
// function of the ticks before T, never of when a fence retired or whose GPU
// ran them. nullptr / an empty table = no field (no snapshot yet, or nothing
// has moved since the last world reset). Window-keyed, like WaterFlow()'s
// grid: it covers the residency window it was recorded in.
void PublishMeasured(std::shared_ptr<const std::vector<uint32_t>> grid);
// The installed table's words, or nullptr.
const uint32_t* PublishedMeasured();
// The composition currentAt makes (common.wgsl): the measured flow counts in
// full and the solved field fades out under it, gone once the measured speed
// reaches kFlowMeasTakeover cells/s.
constexpr float kFlowMeasTakeover = 4.0f;
Vec3 BlendMeasured(Vec3 solved, Vec3 measured);
}  // namespace flowfield

}  // namespace sandvox
