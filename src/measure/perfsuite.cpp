// perfsuite.cpp — `--perf`. See perfsuite.h for what this harness is for and
// how it differs from --measure and --selftest.

#include "measure/perfsuite.h"

#include <algorithm>
#include <cinttypes>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "game/camera.h"
#include "gpu/context.h"
#include "gpu/passtimer.h"
#include "gpu/resources.h"
#include "math3d.h"
#include "measure/perfnodes.h"
#include "measure/perfscope.h"
#include "sim/celestial.h"
#include "sim/materials.h"
#include "sim/pagetable.h"
#include "sim/simulation.h"
#include "sim/stream.h"
#include "sim/tuning.h"
#include "sim/world.h"
#include "test/support.h"

namespace sandvox {
namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Material id IS the index into mats[] (air at 0). Stated here because getting
// it wrong is silent: an off-by-one hands you the next material along and the
// tree burns as sand.
uint32_t MatId(const std::vector<MaterialDef>& mats, const char* name) {
  for (size_t i = 0; i < mats.size(); i++)
    if (mats[i].name == name) return (uint32_t)i;
  return 0;
}

bool MatHasTag(const std::vector<MaterialDef>& mats, uint32_t id,
               const char* tag) {
  if (id >= mats.size()) return false;
  for (const std::string& t : mats[id].tags)
    if (t == tag) return true;
  return false;
}

double Percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const double idx = p * (double)(v.size() - 1);
  const size_t lo = (size_t)idx;
  const size_t hi = std::min(lo + 1, v.size() - 1);
  const double f = idx - (double)lo;
  return v[lo] * (1.0 - f) + v[hi] * f;
}

// JSON string escape. The only characters that can reach here are from our own
// literals and material names, but a scenario description with an apostrophe in
// it should not be able to produce a page that fails to parse.
std::string JStr(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
  }
  return o + "\"";
}

// Fixed 3-decimal doubles. printf's %g would emit `1e-05`, which is valid JSON
// but makes a diff of two runs unreadable, and `inf`/`nan`, which are not JSON
// at all — a single NaN from a divide-by-zero would take the whole page down.
std::string JNum(double v) {
  if (!std::isfinite(v)) return "0";
  char b[40];
  std::snprintf(b, sizeof b, "%.3f", v);
  return b;
}

// ---------------------------------------------------------------------------
// CPU scope clock
//
// One accumulator per frame. Scoped rather than paired Start/Stop calls because
// an early `continue` past a Stop silently attributes the rest of the frame to
// the wrong bar, and this harness exists to be trusted about exactly that.
// ---------------------------------------------------------------------------
struct FrameClock {
  double ms[kPerfScopeCount] = {};
  void Add(PerfScope s, double t0, double t1) {
    ms[(int)s] += (t1 - t0) * 1000.0;
  }
};

struct ScopeTimer {
  FrameClock& fc;
  PerfScope scope;
  double t0;
  ScopeTimer(FrameClock& f, PerfScope s) : fc(f), scope(s), t0(NowSeconds()) {}
  ~ScopeTimer() { fc.Add(scope, t0, NowSeconds()); }
};

// ---------------------------------------------------------------------------
// FRAME PACER — bound the frames in flight, or wall clock means nothing.
//
// THE TRAP, measured. Without this, the harness submits a sim tick and a render
// every iteration and never waits: the CPU runs away from the GPU, 899 frames
// cost 0.64 ms each (the time to RECORD a command buffer) and one frame costs
// 196 ms (the time for the GPU to catch up on all of them). p50 0.64 / p95
// 196.56 / p99 206.13 is not a frame-time distribution, it is a queue depth.
//
// A real frame loop cannot do that, because the swapchain has a fixed number of
// images and AcquireFrame blocks when they are all in flight. This harness
// renders offscreen and has no swapchain, so it reproduces the same constraint
// explicitly: a 4-byte buffer copied at the end of every frame and mapped
// deferred, with the map from `kDepth` frames ago waited on before the next
// frame starts. Three deep, which is what a mailbox swapchain gives you.
//
// The cost is one 4-byte copy per frame. The benefit is that `wallMs` is a
// frame time.
// ---------------------------------------------------------------------------
class FramePacer {
 public:
  bool Init(GpuContext& ctx) {
    src_ = CreateBuffer(ctx.device, 4, rhi::BufferUsage::CopySrc |
                                       rhi::BufferUsage::CopyDst, "perfPaceSrc");
    if (!src_) return false;
    for (int i = 0; i < kDepth; i++) {
      ring_[i] = CreateBuffer(ctx.device, 4, rhi::BufferUsage::MapRead |
                                             rhi::BufferUsage::CopyDst,
                              "perfPaceRing");
      if (!ring_[i]) return false;
    }
    return true;
  }
  // Record the fence-marker copy into this frame's command buffer.
  void Mark(const rhi::CommandEncoder& enc) {
    enc.CopyBufferToBuffer(src_, 0, ring_[head_], 0, 4);
  }
  // Called after the submit: arm this frame's ticket, then block until the
  // frame kDepth-1 back has retired.
  void Throttle(GpuContext& ctx) {
    tickets_[head_] = rhi::MapReadDeferred(ctx.device, ring_[head_], 0, 4);
    head_ = (head_ + 1) % kDepth;
    // The slot we are about to reuse is the oldest one. Waiting on it is
    // exactly "at most kDepth frames may be in flight".
    if (tickets_[head_]) {
      tickets_[head_].Wait();
      tickets_[head_].Unmap();
      tickets_[head_] = rhi::MapTicket();
    }
  }
  void Drain() {
    for (int i = 0; i < kDepth; i++)
      if (tickets_[i]) {
        tickets_[i].Wait();
        tickets_[i].Unmap();
        tickets_[i] = rhi::MapTicket();
      }
    head_ = 0;
  }

 private:
  static constexpr int kDepth = 3;
  rhi::Buffer src_;
  rhi::Buffer ring_[kDepth];
  rhi::MapTicket tickets_[kDepth];
  int head_ = 0;
};

// ---------------------------------------------------------------------------
// Scenario plumbing
// ---------------------------------------------------------------------------

// Everything a scenario's driver may read or write. One struct rather than a
// long parameter list because scenarios are added by people who should not have
// to re-derive the call signature.
struct Scene {
  GpuContext& ctx;
  World& world;
  Simulation& sim;
  Stream& stream;
  const std::vector<MaterialDef>& mats;

  // Camera the frame renders from. A scenario that does not move it renders a
  // fixed view, which is what makes the render number comparable across ticks.
  Vec3 eye{};
  Camera cam;

  // Scenario scratch. Union-by-convention: each scenario uses the fields it
  // needs and ignores the rest.
  IVec3 trunk{};        // treeburn: trunk base cell
  int trunkTop = 0;     // treeburn: highest wood cell found
  int crownR = 0;       // treeburn: crown radius in voxels
  IVec3 pond{};         // water: a surface cell of the pond found at setup
  uint32_t fluidLive = 0;
  // treeburn: wood + foliage voxels in the tree's box at ignition, so `verify`
  // can report what the fire actually consumed rather than leaving it inferred
  // from a wiggle in the active-chunk count.
  uint32_t treeVoxAtStart = 0;

  // Notes the setup wants on the page ("great oak, 187 voxels of trunk").
  std::string note;
};

// What a scenario asks the engine to do on one tick.
struct TickOps {
  std::vector<BrushOp> ops;
  std::vector<ExplosionOp> exps;
  std::vector<CellOp> cells;
  std::vector<ParticleSpawn> spawns;
  std::vector<FluidSpawnOp> fluidSpawns;
  bool particlesActive = false;
};

struct Scenario {
  const char* id;
  const char* label;
  // What this scenario is FOR. Shown on the page above its charts, because a
  // number with no scenario attached is the thing rule 6 warns about.
  const char* desc;
  // ';'-separated ARCH_NODES keys this scenario is designed to light up. The
  // page uses it to say "this run exercises caLoop, particleSys" and to grey
  // out bars nothing in the scenario could have touched.
  const char* stresses;
  uint32_t warmTicks;    // run but do not record: let the transient settle
  uint32_t ticks;        // recorded frames
  // Return false to skip the scenario (fixture not found) with `why` set.
  bool (*setup)(Scene&, std::string& why);
  void (*drive)(Scene&, uint32_t localTick, TickOps&);
  // Optional, run after the last recorded frame: did the scenario actually DO
  // the thing it is named after? Appends to `Scene::note`, which the page
  // prints under the charts.
  //
  // This exists because a performance chart is completely insensitive to it. A
  // tree that never caught fire produces a "tree burning" scenario with 900
  // perfectly plausible frames in it, and nothing on the page would look wrong.
  // The only defence is to measure the fixture, not the frame rate.
  void (*verify)(Scene&) = nullptr;
};

// ---------------------------------------------------------------------------
// Fixture finding: the tallest real trunk near spawn
//
// Trees are worldgen-only (worldgen.wgsl treeInfoAt, placed per TREE_TILE), and
// there is no CPU-side query for where one stands — the generator is a pure
// per-cell function on the GPU. So the harness does what a player does: it
// looks. A box of chunks around spawn is read back once, at setup, and the
// tallest contiguous run of `tag:wood` is the tree.
//
// Reading rather than deriving is also what keeps this honest across worldgen
// changes: the day the tree parameters move, this finds the new tallest tree
// instead of igniting a column of air where the old one used to be.
// ---------------------------------------------------------------------------
struct Trunk {
  bool found = false;
  IVec3 base{};      // lowest wood cell of the trunk column
  int top = 0;       // highest wood cell in that column
  int crownR = 0;    // horizontal reach of leaves around the trunk
  uint32_t voxels = 0;  // wood + leaf voxels in the tree's bounding column set
  std::string species;
};

// THE CHEAP HALF OF THE SEARCH.
//
// A blind voxel scan wide enough to find a tree is thousands of blocking 16 KiB
// readbacks; measured, a 4-chunk radius was 1,215 of them and found a meadow.
// The occupancy buffer already answers "does this chunk hold anything" for the
// whole world in one 128 KiB read, and a tree is one of very few things that
// puts solid voxels WELL ABOVE the local ground. So: find the candidate chunks
// with arithmetic, then read only those.
//
// Occupancy is refreshed over the whole world only on a hash tick, so the
// caller must force one (HashWorldNow) before calling — a stale read here is a
// tree that was found where there is now a crater.
std::vector<uint32_t> ReadOccupancy(GpuContext& ctx, World& world) {
  std::vector<uint32_t> occ(kNumChunks, 0);
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.occupancy, 0, occ.data(),
                        (size_t)kNumChunks * 4, "perfOcc");
  return occ;
}

// The HIGHEST ground under a chunk's 16x16 footprint, not the height at its
// corner. This distinction is not pedantry: sampled at the corner, a chunk on
// a hillside reads as "40 voxels above the ground" and wins a canopy search
// outright. Measured — the first version of this search returned a chunk whose
// contents were 188,934 stone and no wood at all.
int GroundMaxUnderChunk(int cx, int cz) {
  int g = 0;
  for (int dz = 0; dz <= (int)kChunk; dz += (int)kChunk / 2)
    for (int dx = 0; dx <= (int)kChunk; dx += (int)kChunk / 2)
      g = std::max(g, World::TerrainHeight(cx * (int)kChunk + dx,
                                           cz * (int)kChunk + dz, kDefaultSeed));
  return g;
}

// Chunks that hold something well above their own ground. Occupancy answers
// this for the whole world in one 128 KiB read and carries no material, so this
// is a CANDIDATE list — the caller reads the few chunks it returns and decides
// what they actually contain.
std::vector<IVec3> AboveGroundChunks(World& world,
                                     const std::vector<uint32_t>& occ,
                                     IVec3 centreChunk, int chunkRadius) {
  std::vector<IVec3> out;
  for (int cz = centreChunk.z - chunkRadius; cz <= centreChunk.z + chunkRadius; cz++)
    for (int cx = centreChunk.x - chunkRadius; cx <= centreChunk.x + chunkRadius; cx++) {
      const int ground = GroundMaxUnderChunk(cx, cz);
      // +16 voxels of clearance skips the grass and scrub layer that sits
      // directly on the ground everywhere and would otherwise be every chunk.
      const int cyLo = (ground + 16) / (int)kChunk + 1;
      const int cyHi = (ground + 240) / (int)kChunk;
      for (int cy = cyLo; cy <= cyHi; cy++) {
        const IVec3 wc{cx, cy, cz};
        if (!world.ChunkInWindow(wc)) continue;
        if ((occ[World::SlotChunkIndex(wc)] & 0xFFFFu) == 0) continue;
        out.push_back(wc);
      }
    }
  return out;
}

Trunk FindTallestTrunk(GpuContext& ctx, World& world,
                       const std::vector<MaterialDef>& mats, IVec3 centreChunk,
                       int chunkRadius) {
  Trunk best;
  // WHICH MATERIALS ARE TRUNK, and why this is a name list rather than a tag
  // query. Canopy is easy — every leaf material carries `tag:foliage`, so that
  // half is data-driven. Trunk is not: there is no `wood` tag in
  // materials.json, and the flammable/organic pair that would stand in for one
  // also matches grass, petals, robe cloth and skin. Rather than invent a tag
  // (a data change to suit a test) or match on `class == solid && flammable`
  // (which finds a hedge), the two materials worldgen actually paints trunks
  // with are named, and anything tall the scan finds that ISN'T one of them is
  // REPORTED — so the day a third species lands, the failure says
  // "tallest column was 187 voxels of `oak_heartwood`, not a trunk material"
  // instead of "no tree found".
  static const char* kTrunkMats[] = {"wood", "birch_wood"};
  std::vector<uint8_t> isWood(mats.size(), 0), isLeaf(mats.size(), 0);
  for (uint32_t i = 0; i < mats.size(); i++)
    isLeaf[i] = MatHasTag(mats, i, "foliage");
  for (const char* nm : kTrunkMats) {
    const uint32_t id = MatId(mats, nm);
    if (id != 0 && id < isWood.size()) isWood[id] = 1;
  }
  // Attribution, so a failure names its cause. CLAUDE.md rule 6: "no tree
  // found" is a bare count, and chasing it by turning scan parameters off one
  // at a time buys one hypothesis per run. Every one of these is filled in
  // whether or not the scan succeeds, and printed when it does not.
  std::vector<uint64_t> matCount(mats.size(), 0);
  uint64_t chunksRead = 0, chunksOutOfWindow = 0, nonAir = 0;
  uint32_t colsWithWood = 0, rejectedShort = 0, rejectedNoLeaf = 0;
  int longestWoodRun = 0;

  // Column-major accumulation over the read box: for each (x,z) column, the
  // lowest and highest wood cell and the leaf count around it.
  struct Col { int lo = INT_MAX, hi = INT_MIN; };
  const int cr = chunkRadius;
  const int span = (2 * cr + 1) * (int)kChunk;
  std::vector<Col> cols((size_t)span * span);
  std::vector<uint32_t> leafAt((size_t)span * span, 0);
  const int x0 = (centreChunk.x - cr) * (int)kChunk;
  const int z0 = (centreChunk.z - cr) * (int)kChunk;

  // Vertical extent: from a little below the local ground to well above the
  // tallest crown. Anchored to TerrainHeight rather than a literal Y, per the
  // fixture-anchoring rule in test/support.h — the datum has moved before.
  const int ground = World::TerrainHeight(centreChunk.x * (int)kChunk,
                                          centreChunk.z * (int)kChunk,
                                          kDefaultSeed);
  const int yLo = std::max(0, (ground - 8) / (int)kChunk);
  const int yHi = (ground + 220) / (int)kChunk;

  std::vector<uint32_t> chunk(kChunkVol);
  for (int cy = yLo; cy <= yHi; cy++)
    for (int cz = centreChunk.z - cr; cz <= centreChunk.z + cr; cz++)
      for (int cx = centreChunk.x - cr; cx <= centreChunk.x + cr; cx++) {
        const IVec3 wc{cx, cy, cz};
        if (!world.ChunkInWindow(wc)) { chunksOutOfWindow++; continue; }
        const uint32_t slot = World::SlotChunkIndex(wc);
        ReadVoxelsSync(ctx, world, slot, 1, chunk.data(), "perfTreeScan");
        chunksRead++;
        for (uint32_t li = 0; li < kChunkVol; li++) {
          const uint32_t m = chunk[li] & 0xFFFu;
          if (m == 0 || m >= mats.size()) continue;
          nonAir++;
          matCount[m]++;
          if (!isWood[m] && !isLeaf[m]) continue;
          const int lx = (int)(li % kChunk);
          const int ly = (int)((li / kChunk) % kChunk);
          const int lz = (int)(li / (kChunk * kChunk));
          const int wx = cx * (int)kChunk + lx;
          const int wy = cy * (int)kChunk + ly;
          const int wz = cz * (int)kChunk + lz;
          const int ix = wx - x0, iz = wz - z0;
          if (ix < 0 || iz < 0 || ix >= span || iz >= span) continue;
          const size_t ci = (size_t)iz * span + ix;
          if (isWood[m]) {
            cols[ci].lo = std::min(cols[ci].lo, wy);
            cols[ci].hi = std::max(cols[ci].hi, wy);
          } else {
            leafAt[ci]++;
          }
        }
      }

  // The winner is the column with the longest wood run. A trunk, not a fence
  // post: require the run to clear 40 voxels so a woodpile or a bridge cannot
  // win, and require leaves nearby so a bare snag cannot either — a tree with
  // no canopy is a much smaller fire than the one this scenario is named after.
  int bestRun = 0;
  size_t bestCi = 0;
  for (size_t ci = 0; ci < cols.size(); ci++) {
    if (cols[ci].lo == INT_MAX) continue;
    colsWithWood++;
    const int run = cols[ci].hi - cols[ci].lo;
    longestWoodRun = std::max(longestWoodRun, run);
    if (run < 40) { rejectedShort++; continue; }
    const int ix = (int)(ci % span), iz = (int)(ci / span);
    uint32_t nearLeaf = 0;
    for (int dz = -12; dz <= 12; dz++)
      for (int dx = -12; dx <= 12; dx++) {
        const int jx = ix + dx, jz = iz + dz;
        if (jx < 0 || jz < 0 || jx >= span || jz >= span) continue;
        nearLeaf += leafAt[(size_t)jz * span + jx];
      }
    if (nearLeaf < 200) { rejectedNoLeaf++; continue; }
    if (run > bestRun) { bestRun = run; bestCi = ci; }
  }
  if (bestRun == 0) {
    // Record at the point of FAILURE, with the numbers that discriminate
    // between the four ways this can go wrong: nothing read (window/Y range),
    // nothing organic (wrong place), wood but short (threshold), wood but bare
    // (canopy detection).
    std::printf("    tree scan: %llu chunks read (%llu outside the window), "
                "%llu non-air voxels, y %d..%d\n",
                (unsigned long long)chunksRead,
                (unsigned long long)chunksOutOfWindow,
                (unsigned long long)nonAir, yLo * (int)kChunk,
                (yHi + 1) * (int)kChunk - 1);
    std::printf("    trunk columns %u (longest run %d voxels); rejected: %u "
                "shorter than 40, %u with no canopy within 12\n",
                colsWithWood, longestWoodRun, rejectedShort, rejectedNoLeaf);
    // The five most common solids in the box, by name. If the tallest thing
    // out there is a material this list does not call a trunk, this is where
    // it says so.
    std::vector<std::pair<uint64_t, uint32_t>> top;
    for (uint32_t i = 1; i < mats.size(); i++)
      if (matCount[i]) top.emplace_back(matCount[i], i);
    std::sort(top.rbegin(), top.rend());
    std::printf("    most common materials:");
    for (size_t i = 0; i < top.size() && i < 6; i++)
      std::printf(" %s=%llu", mats[top[i].second].name.c_str(),
                  (unsigned long long)top[i].first);
    std::printf("\n    (trunk materials recognised:");
    for (const char* nm : kTrunkMats) std::printf(" %s", nm);
    std::printf(")\n");
    return best;
  }

  const int ix = (int)(bestCi % span), iz = (int)(bestCi / span);
  best.found = true;
  best.base = {x0 + ix, cols[bestCi].lo, z0 + iz};
  best.top = cols[bestCi].hi;
  // Crown radius: the furthest ring around the trunk that still holds leaves.
  best.crownR = 4;
  for (int r = 4; r <= 40; r++) {
    uint32_t hits = 0;
    for (int dz = -r; dz <= r; dz++)
      for (int dx = -r; dx <= r; dx++) {
        if (std::max(std::abs(dx), std::abs(dz)) != r) continue;
        const int jx = ix + dx, jz = iz + dz;
        if (jx < 0 || jz < 0 || jx >= span || jz >= span) continue;
        hits += leafAt[(size_t)jz * span + jx];
      }
    if (hits > 0) best.crownR = r;
  }
  for (size_t ci = 0; ci < cols.size(); ci++) best.voxels += leafAt[ci];
  return best;
}

// ---------------------------------------------------------------------------
// SCENARIO: idle
//
// A settled world, a standing player, no input at all. Rule 2 says this should
// cost almost nothing, and every other bar on the page is only meaningful
// against it: "the CA cost 4 ms" means one thing if idle is 0.03 ms and another
// entirely if idle is 3 ms.
// ---------------------------------------------------------------------------
bool SetupIdle(Scene& s, std::string&) {
  const int gx = 256, gz = 256;
  const int h = World::TerrainHeight(gx, gz, kDefaultSeed);
  s.eye = {(float)gx, (float)(h + 18), (float)gz};
  s.cam.yaw = 0.785f;
  s.cam.pitch = -0.15f;
  s.note = "settled world, no input";
  return true;
}
void DriveIdle(Scene&, uint32_t, TickOps&) {}

// ---------------------------------------------------------------------------
// SCENARIO: treeburn — the one this page was asked for.
//
// Find the biggest real worldgen tree near spawn, stand the player in front of
// it at the distance the crown fills the view, set the trunk base alight and
// watch for 30 seconds of sim time (900 ticks at 30 Hz).
//
// What it stresses, and why it is the interesting scenario: a canopy fire is
// the engine's widest REACTION front. It is thousands of simultaneous CA rule
// firings spread over a tall, thin, mostly-air region — so it lights up the CA
// and the dirty-chunk count without the bulk-material cost of a landslide, and
// the fire/smoke/ember gases keep chunks awake for the whole 30 s instead of
// settling after a second like an explosion does. It is also the scenario where
// the RENDER cost and the SIM cost move in opposite directions: the canopy
// burning away opens the view, so raymarch gets cheaper exactly as the CA gets
// more expensive.
// ---------------------------------------------------------------------------
uint32_t CountTreeVoxels(Scene& s);   // defined below, beside VerifyTreeburn

bool SetupTreeburn(Scene& s, std::string& why) {
  // TWO PHASES, because a blind voxel scan wide enough to find a tree is
  // thousands of blocking readbacks. Phase 1 is one 128 KiB occupancy read that
  // says WHERE the canopy is; phase 2 reads voxels only around it.
  //
  // Measured on seed 1337: the window centre is a meadow — 723,027 non-air
  // voxels within 4 chunks and not one of them wood. Searching outward is not
  // an optimisation here, it is the difference between the scenario existing
  // and not.
  const IVec3 org = s.world.WindowOrigin();
  const int cx = org.x + (int)kNChunk / 2;
  const int cz = org.z + (int)kNChunk / 2;
  const int ground = World::TerrainHeight(cx * (int)kChunk, cz * (int)kChunk,
                                          kDefaultSeed);
  const IVec3 centre{cx, ground / (int)kChunk, cz};

  // A full-world occupancy pass, so phase 1 reads this tick's truth rather than
  // whatever the last hash tick left behind.
  HashWorldNow(s.ctx, s.world, s.sim, kDefaultSeed);
  s.ctx.WaitIdle();
  const std::vector<uint32_t> occ = ReadOccupancy(s.ctx, s.world);

  // Search rings outward. Worldgen places trunk sites every TREE_TILE (90
  // voxels), so 14 chunks (224 voxels) contains several sites in any biome that
  // has trees at all — and the spawn meadow measured above has none within 4.
  //
  // The winner is the candidate chunk holding the most FOLIAGE. Foliage is the
  // one half of a tree that is genuinely data-driven (`tag:foliage`, 30-odd
  // materials), it is far wider than a trunk so it is much easier to hit, and
  // it is the thing that distinguishes a tree from a rock overhang — which is
  // what the previous version of this search kept finding.
  std::vector<uint32_t> chunkBuf(kChunkVol);
  std::vector<uint8_t> foliage(s.mats.size(), 0);
  for (uint32_t i = 0; i < s.mats.size(); i++)
    foliage[i] = MatHasTag(s.mats, i, "foliage");

  IVec3 canopy{0, -1, 0};
  uint32_t bestLeaf = 0, candidates = 0;
  for (int r : {4, 9, 14}) {
    const std::vector<IVec3> cand = AboveGroundChunks(s.world, occ, centre, r);
    candidates = (uint32_t)cand.size();
    for (const IVec3& wc : cand) {
      ReadVoxelsSync(s.ctx, s.world, World::SlotChunkIndex(wc), 1,
                     chunkBuf.data(), "perfCanopyScan");
      uint32_t leaves = 0;
      for (uint32_t li = 0; li < kChunkVol; li++) {
        const uint32_t m = chunkBuf[li] & 0xFFFu;
        if (m < foliage.size() && foliage[m]) leaves++;
      }
      if (leaves > bestLeaf) { bestLeaf = leaves; canopy = wc; }
    }
    if (bestLeaf >= 200) break;   // a real crown, not a bramble on a ledge
  }
  std::printf("    canopy scan: %u candidate chunks above ground, best holds "
              "%u foliage voxels at chunk (%d,%d,%d)\n",
              candidates, bestLeaf, canopy.x, canopy.y, canopy.z);
  if (bestLeaf < 200) {
    why = "no chunk with 200+ foliage voxels within 14 chunks of the window "
          "centre — this seed's spawn has no canopy, so there is no tree to burn";
    return false;
  }

  Trunk t = FindTallestTrunk(s.ctx, s.world, s.mats,
                             {canopy.x, canopy.y, canopy.z}, /*chunkRadius=*/2);
  if (!t.found) {
    why = "found above-ground content but no 40+ voxel trunk with a canopy "
          "under it — the scenario refuses rather than igniting a stump";
    return false;
  }
  s.trunk = t.base;
  s.trunkTop = t.top;
  s.crownR = t.crownR;

  // Stand back far enough that the whole crown is in frame, and put the eye at
  // player eye height above the LOCAL ground rather than at an offset from the
  // trunk base — a trunk on a slope has its base below the ground you stand on.
  const float dist = (float)(t.crownR + 14);
  const float ex = (float)t.base.x - dist * 0.7071f;
  const float ez = (float)t.base.z - dist * 0.7071f;
  const int eg = World::TerrainHeight((int)ex, (int)ez, kDefaultSeed);
  s.eye = {ex, (float)(eg + 15), ez};   // 15 voxels = the avatar's eye height
  // Look at the middle of the trunk, so the crown and the base are both in shot.
  const float dx = (float)t.base.x - ex, dz = (float)t.base.z - ez;
  const float dy = (float)((t.base.y + t.top) / 2) - s.eye.y;
  s.cam.yaw = std::atan2(dx, dz);
  s.cam.pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz));

  char note[256];
  std::snprintf(note, sizeof note,
                "trunk %d voxels tall at (%d,%d,%d), crown radius %d, %u canopy "
                "voxels; viewer %.0f voxels back",
                t.top - t.base.y, t.base.x, t.base.y, t.base.z, t.crownR,
                t.voxels, dist);
  s.note = note;
  s.treeVoxAtStart = CountTreeVoxels(s);
  return true;
}

// Wood + foliage voxels in the chunk box around the trunk. Used before and
// after the burn; the difference is the fire.
uint32_t CountTreeVoxels(Scene& s) {
  std::vector<uint8_t> want(s.mats.size(), 0);
  want[MatId(s.mats, "wood")] = 1;
  want[MatId(s.mats, "birch_wood")] = 1;
  for (uint32_t i = 0; i < s.mats.size(); i++)
    if (MatHasTag(s.mats, i, "foliage")) want[i] = 1;
  want[0] = 0;   // MatId returns 0 for a name that is not there; air is not wood

  const int cr = std::max(2, (s.crownR + (int)kChunk) / (int)kChunk);
  const int cx0 = s.trunk.x / (int)kChunk, cz0 = s.trunk.z / (int)kChunk;
  const int cy0 = s.trunk.y / (int)kChunk;
  const int cy1 = (s.trunkTop + s.crownR) / (int)kChunk;
  uint32_t n = 0;
  std::vector<uint32_t> buf(kChunkVol);
  for (int cy = cy0; cy <= cy1; cy++)
    for (int cz = cz0 - cr; cz <= cz0 + cr; cz++)
      for (int cx = cx0 - cr; cx <= cx0 + cr; cx++) {
        const IVec3 wc{cx, cy, cz};
        if (!s.world.ChunkInWindow(wc)) continue;
        ReadVoxelsSync(s.ctx, s.world, World::SlotChunkIndex(wc), 1, buf.data(),
                       "perfTreeCount");
        for (uint32_t li = 0; li < kChunkVol; li++) {
          const uint32_t m = buf[li] & 0xFFFu;
          if (m < want.size() && want[m]) n++;
        }
      }
  return n;
}

void VerifyTreeburn(Scene& s) {
  s.ctx.WaitIdle();
  const uint32_t after = CountTreeVoxels(s);
  const uint32_t before = s.treeVoxAtStart;
  const double pct = before ? 100.0 * (double)(before - after) / (double)before : 0.0;
  char b[256];
  std::snprintf(b, sizeof b,
                "  BURN: %u of %u wood+foliage voxels consumed in 30 s (%.1f%%)%s",
                before > after ? before - after : 0u, before, pct,
                pct < 5.0 ? "  <-- the fire barely took; this scenario is not "
                            "measuring a canopy burn"
                          : "");
  s.note += b;
}

void DriveTreeburn(Scene& s, uint32_t lt, TickOps& out) {
  // IGNITION, once, on tick 0. A ring of fire voxels around the trunk base:
  // reactions.json already has `wood + tag:hot -> ember`, so this is a match
  // held to the bark rather than a special-cased "set tree on fire" op. The
  // burn that follows is entirely the authored reaction table.
  //
  // Placed as CellOps (exact cells) rather than a brush sphere so the ignition
  // cannot carve the trunk it is supposed to light.
  if (lt == 0) {
    const uint32_t fire = MatId(s.mats, "fire");
    const uint32_t word = fire;  // stamp 0 = STAMP_NEVER, the new-voxel value
    for (int dy = 0; dy < 4; dy++)
      for (int dz = -2; dz <= 2; dz++)
        for (int dx = -2; dx <= 2; dx++) {
          if (dx == 0 && dz == 0) continue;   // do not overwrite the trunk
          const IVec3 c{s.trunk.x + dx, s.trunk.y + dy, s.trunk.z + dz};
          if (!s.world.CellInWindow(c)) continue;
          out.cells.push_back({World::SlotCellIndex(c), word | kCellOpIfAir});
        }
  }
  // Ember and ash fall as particles; nothing else is emitted. `particlesActive`
  // stays true for the whole run because a burning canopy is dropping debris
  // the entire time, and a false "settled" here would take the CA skip and
  // measure a scenario that is not the one running.
  out.particlesActive = true;
}

// ---------------------------------------------------------------------------
// SCENARIO: flythrough
//
// A diagonal descent across the world, the traversal --autofly-hard uses for
// residency sizing. Nothing here is on fire; the point is the systems the tree
// burn never touches — window shifts, chunk fetch and evict, worldgen for newly
// resident chunks, page fills, and the far-field cascade.
// ---------------------------------------------------------------------------
bool SetupFlythrough(Scene& s, std::string&) {
  const IVec3 org = s.world.WindowOrigin();
  const int cx = (org.x + (int)kNChunk / 2) * (int)kChunk;
  const int cz = (org.z + (int)kNChunk / 2) * (int)kChunk;
  s.eye = {(float)cx, (float)(World::TerrainHeight(cx, cz, kDefaultSeed) + 150),
           (float)cz};
  s.cam.yaw = 0.785f;
  s.cam.pitch = -0.35f;
  s.note = "diagonal descent, 1.5 voxels/tick, the --autofly-hard traversal";
  return true;
}
void DriveFlythrough(Scene& s, uint32_t lt, TickOps&) {
  // A FIXED schedule, not a velocity integrated from frame time: the whole run
  // has to be reproducible, and a path that depends on how fast the machine ran
  // makes every streaming number a function of the machine.
  const float step = 1.5f;
  s.eye.x += step * 0.7071f;
  s.eye.z += step * 0.7071f;
  // Descend for the first half, level out for the second, so the run covers
  // both the "new chunks below" and the "new chunks ahead" streaming shapes.
  if (lt < 300) s.eye.y -= 0.35f;
}

// ---------------------------------------------------------------------------
// SCENARIO: explosion
//
// A blast every 20 ticks, walked around the world so successive ones do not
// land in the last one's crater. Explosions are the cheapest lever the engine
// has for "make a lot of things move at once": mark+apply, a burst of ballistic
// particles, a wide dirty set, and a settle back to rest.
// ---------------------------------------------------------------------------
bool SetupExplosion(Scene& s, std::string&) {
  const IVec3 org = s.world.WindowOrigin();
  const int cx = (org.x + (int)kNChunk / 2) * (int)kChunk;
  const int cz = (org.z + (int)kNChunk / 2) * (int)kChunk;
  const int h = World::TerrainHeight(cx, cz, kDefaultSeed);
  s.eye = {(float)(cx - 60), (float)(h + 40), (float)(cz - 60)};
  s.cam.yaw = 0.785f;
  s.cam.pitch = -0.35f;
  s.note = "radius-14 blast every 20 ticks, walked so no two share a crater";
  return true;
}
void DriveExplosion(Scene& s, uint32_t lt, TickOps& out) {
  if (lt % 20 == 0) {
    const IVec3 org = s.world.WindowOrigin();
    const int base = (org.x + (int)kNChunk / 2) * (int)kChunk;
    const int basez = (org.z + (int)kNChunk / 2) * (int)kChunk;
    const int gx = base - 40 + (int)((lt / 20 * 37u) % 80u);
    const int gz = basez - 40 + (int)((lt / 20 * 53u) % 80u);
    const int h = World::TerrainHeight(gx, gz, kDefaultSeed);
    out.exps.push_back({gx, h, gz, 14, 400, 0, 0, 0});
  }
  out.particlesActive = true;
}

// ---------------------------------------------------------------------------
// SCENARIO: water
//
// Puncture the bank of a worldgen pond and let it drain. This is the only
// scenario that lights up the MLS-MPM solver and the CA<->MPM seam, and it is
// deliberately a REAL pond rather than a lab basin: the lab scenes run on a
// flat slab world, so their numbers say nothing about what water costs on
// terrain.
// ---------------------------------------------------------------------------
bool SetupWater(Scene& s, std::string& why) {
  // ASK WORLDGEN, do not go looking — and ask the right question.
  //
  // Two wrong turns are recorded here because both look like "the feature is
  // broken" and neither is:
  //
  //   1. Scanning chunks for the water material found nothing within 6 chunks.
  //      That was not a missing pond; ponds sit on a 448-voxel tile grid and a
  //      6-chunk (96-voxel) box is far finer than the thing it is looking for.
  //   2. Asking `World::PondTile` over the window found nothing either — and
  //      that one is DELIBERATE. pondInfo() rejects any tarn whose centre lands
  //      in -128..640 on both axes, and world.cpp says why in as many words:
  //      that box is the authored origin region, "exactly the residency window
  //      the harness runs in". A generated pond can never appear here.
  //
  // The water that IS here is authored: `World::AuthoredPoolList` returns the
  // three set-piece pools at the origin — the lake, the oil pond, the lava pool
  // — which is the same fixture `--fluid-bench pond68` measures. So this
  // scenario uses the lake, and only falls back to a generated tarn if the
  // window has been moved away from the origin.
  {
    World::AuthoredPool pools[World::kAuthoredPools];
    World::AuthoredPoolList(pools);
    const IVec3 wo = s.world.WindowOrigin();
    std::printf("    authored pools (window origin chunk %d,%d,%d = voxels "
                "%d..%d, %d..%d, %d..%d):\n",
                wo.x, wo.y, wo.z,
                wo.x * (int)kChunk, (wo.x + (int)kNChunk) * (int)kChunk - 1,
                wo.y * (int)kChunk, (wo.y + (int)kNChunk) * (int)kChunk - 1,
                wo.z * (int)kChunk, (wo.z + (int)kNChunk) * (int)kChunk - 1);
    for (const World::AuthoredPool& p : pools)
      std::printf("      %-6s r=%-4d at (%d,%d,%d)  %s\n", p.mat, p.r,
                  p.cx, p.waterY, p.cz,
                  s.world.CellInWindow({p.cx, p.waterY, p.cz}) ? "IN WINDOW"
                                                               : "outside");
    for (const World::AuthoredPool& p : pools) {
      if (std::strcmp(p.mat, "water") != 0) continue;
      if (!s.world.CellInWindow({p.cx, p.waterY, p.cz})) continue;
      s.pond = {p.cx, p.waterY, p.cz};
      s.crownR = p.r;
      const float d = (float)(p.r + 24);
      s.eye = {(float)p.cx - d * 0.7071f, (float)(p.waterY + 20),
               (float)p.cz - d * 0.7071f};
      s.cam.yaw = 0.785f;
      s.cam.pitch = -0.32f;
      char note[224];
      std::snprintf(note, sizeof note,
                    "authored lake: disc r=%d at (%d,%d), floor y=%d, surface "
                    "y=%d (%d voxels deep); bank punctured on tick 60",
                    p.r, p.cx, p.cz, p.floorY, p.waterY, p.waterY - p.floorY);
      s.note = note;
      return true;
    }
  }

  const IVec3 org = s.world.WindowOrigin();
  const int ccx = (org.x + (int)kNChunk / 2) * (int)kChunk;
  const int ccz = (org.z + (int)kNChunk / 2) * (int)kChunk;
  const int tile = World::PondTileSize();
  const int here = 0;
  (void)here;

  World::PondDisc best;
  int bestD2 = INT_MAX;
  // Attribution, so a skip names its cause rather than sending the next reader
  // to turn thresholds off one at a time (CLAUDE.md rule 6).
  int tilesChecked = 0, pondsPresent = 0, tooSmall = 0, outOfWindow = 0,
      biggestR = 0;
  // Tiles covering the residency window, centred on the window middle.
  //
  // ROUNDED UP, AND AT LEAST 1. The pond tile pitch is 448 voxels and the
  // window is 512, so the obvious `(kWorldN / 2) / tile` is 256/448 = ZERO —
  // the scan checked exactly one tile, found nothing, and reported "worldgen
  // places no pond" about a world that has plenty. A reach of 1 covers +/-448
  // voxels, comfortably past the window's 256-voxel half width.
  const int reach = std::max(1, ((int)kWorldN / 2 + tile - 1) / std::max(1, tile));
  // Floor division, not truncation: the window origin is unbounded and signed,
  // and -1/448 == 0 would fold two tiles into one.
  auto tileOf = [&](int v) { return (int)std::floor((double)v / (double)tile); };
  for (int tz = -reach; tz <= reach; tz++)
    for (int tx = -reach; tx <= reach; tx++) {
      tilesChecked++;
      const World::PondDisc d =
          World::PondTile(tileOf(ccx) + tx, tileOf(ccz) + tz, kDefaultSeed);
      if (!d.present || d.surf < 0) continue;
      pondsPresent++;
      biggestR = std::max(biggestR, d.r);
      if (d.r < 6) { tooSmall++; continue; }
      // Must be resident, or the scenario punctures a bank that is not there.
      if (!s.world.CellInWindow({d.cx, d.surf, d.cz})) { outOfWindow++; continue; }
      const int dx = d.cx - ccx, dz = d.cz - ccz;
      const int d2 = dx * dx + dz * dz;
      if (d2 < bestD2) { bestD2 = d2; best = d; }
    }
  std::printf("    pond scan: %d tiles (pitch %d), %d ponds placed, biggest "
              "r=%d; rejected %d under r6, %d outside the window\n",
              tilesChecked, tile, pondsPresent, biggestR, tooSmall, outOfWindow);
  if (!best.present) {
    why = "no authored lake in the window and no generated pond either — "
          "pondInfo() excludes tarns from the -128..640 authored origin region, "
          "so this only happens once the window has moved off it";
    return false;
  }

  s.pond = {best.cx, best.surf, best.cz};
  s.crownR = best.r;   // reused: the disc radius, for the camera pull-back
  // Stand on the bank looking across the water, high enough to see the surface
  // rather than edge-on.
  const float d = (float)(best.r + 20);
  s.eye = {(float)best.cx - d * 0.7071f, (float)(best.surf + 18),
           (float)best.cz - d * 0.7071f};
  s.cam.yaw = 0.785f;
  s.cam.pitch = -0.35f;
  char note[224];
  std::snprintf(note, sizeof note,
                "worldgen pond: disc r=%d at (%d,%d), surface y=%d, %d voxels "
                "from the window centre; bank punctured on tick 60",
                best.r, best.cx, best.cz, best.surf,
                (int)std::sqrt((double)bestD2));
  s.note = note;
  return true;
}
void DriveWater(Scene& s, uint32_t lt, TickOps& out) {
  // One blast through the bank, once, after the pond has had 60 ticks to settle
  // — a puncture into water that is still moving measures the settle, not the
  // drain. Aimed at the RIM (centre + radius) and below the surface, which is
  // where a hole actually drains from; a hole in the middle of the floor is
  // under the whole head and drains straight down into rock.
  if (lt == 60) {
    const int rx = s.pond.x + s.crownR;
    out.exps.push_back({rx, s.pond.y - 4, s.pond.z, 9, 500, 0, 0, 0});
  }
  out.particlesActive = lt >= 60;
}

const Scenario kScenarios[] = {
    {"idle", "Idle (settled)",
     "A settled world with no input. Rule 2's floor: every other scenario's "
     "numbers are only meaningful measured against this one.",
     "simTick;renderPass", 60, 240, SetupIdle, DriveIdle},

    {"treeburn", "Tree burning (30 s)",
     "The tallest real worldgen tree near the window centre, set alight at the "
     "trunk base and watched for 30 seconds of sim time. The engine's widest "
     "reaction front: thousands of CA rules firing across a tall, thin, mostly "
     "empty region, with fire and smoke keeping chunks awake for the whole run.",
     "caLoop;particleSys;compact;occupancy;renderPass", 60, 900, SetupTreeburn,
     DriveTreeburn, VerifyTreeburn},

    {"flythrough", "Flythrough (streaming)",
     "A diagonal descent across the world at a fixed 1.5 voxels/tick. Lights "
     "up everything the tree burn never touches: window shifts, chunk fetch and "
     "evict, worldgen, page fills and the far-field cascade.",
     "worldStorage;worldgen;pageTable;farField;renderPass", 30, 600,
     SetupFlythrough, DriveFlythrough},

    {"explosion", "Explosions + debris",
     "A radius-14 blast every 20 ticks, walked around so no two land in the "
     "same crater. Mark+apply, a burst of ballistic particles, a wide dirty "
     "set, and the settle back to rest.",
     "explode;particleSys;caLoop;mutQueue", 60, 600, SetupExplosion,
     DriveExplosion},

    {"water", "Pond drain (MLS-MPM)",
     "A real worldgen pond with its bank punctured. The only scenario that "
     "lights up the MPM solver and the CA/MPM seam, and it runs on terrain "
     "rather than the lab slab so the numbers describe the game.",
     "fluidSys;waterBodies;caLoop;renderPass", 60, 600, SetupWater, DriveWater},
};
constexpr int kScenarioCount = (int)(sizeof(kScenarios) / sizeof(kScenarios[0]));

// ---------------------------------------------------------------------------
// Attribution: pass name -> node index.
//
// Built once from kPerfNodes. A pass the table does not mention is counted into
// `unattributed` and REPORTED — silently dropping it would make the page's
// "GPU total" quietly disagree with the sum of its own bars, which is the one
// way a performance view can lie without anyone noticing.
// ---------------------------------------------------------------------------
// Attribution now lives in measure/perfnodes.h (PerfNodeForPass) so the live
// telemetry path in main.cpp uses the SAME map. It used to be a local table
// here, which is one copy away from a pass being double-counted on one view and
// missing from the other.

// ---------------------------------------------------------------------------
// One scenario's recorded run.
// ---------------------------------------------------------------------------
struct Run {
  std::string id, label, desc, stresses, note;
  bool skipped = false;
  std::string skipWhy;
  std::vector<PerfSample> samples;
  // Per-pass totals, kept beside the node rollup so the page can drill from
  // "fluidSys cost 3 ms" to "of which seam_settle_scan was 2.1".
  std::vector<PassTimer::Stat> passStats;
  uint64_t unattributedNs = 0;
  std::vector<std::string> unattributedNames;
  uint32_t worldHash = 0;
  double warmupMs = 0;
};

// ---------------------------------------------------------------------------
// The recorder.
// ---------------------------------------------------------------------------
class PerfRunner {
 public:
  PerfRunner(GpuContext& ctx, World& world, Simulation& sim, Stream& stream,
             const std::vector<MaterialDef>& mats, const PerfOptions& opt)
      : ctx_(ctx), world_(world), sim_(sim), stream_(stream), mats_(mats),
        opt_(opt) {}

  bool Init() {
    // Capacity is per COMMAND BUFFER, in pass pairs. Row granularity turns one
    // `prep` group into six rows and one `fluidSettle` group into eleven, so
    // the tick table needs far more than --measure's 32. 192 covers the widest
    // tick (fluid live + water bodies + far fill) with room to spare; a pass
    // that does not fit goes untimed rather than failing, and lands in the
    // unattributed bucket where it is visible.
    haveTimer_ = timer_.Init(ctx_, 192);
    if (haveTimer_) {
      timer_.SetRowGranularity(true);
      sim_.SetPassTimer(&timer_);
    }
    // The render pass is not in the pass table and gets its own query set:
    // it lives in a different command buffer from the tick, so it needs its own
    // resolve, and sharing one set would mean resolving a half-written range.
    haveRenderTimer_ = renderTimer_.Init(ctx_, 2);

    origin0_ = world_.WindowOrigin();
    if (!pacer_.Init(ctx_)) return false;
    offscreen_ = ctx_.device.CreateTexture(
        {opt_.width, opt_.height, 1}, rhi::TextureFormat::RGBA8Unorm,
        rhi::TextureUsage::RenderAttachment | rhi::TextureUsage::CopySrc,
        "perfOffscreen");
    if (!offscreen_) return false;
    view_ = offscreen_.CreateView();
    return true;
  }

  bool HaveTimer() const { return haveTimer_; }

  Run Record(const Scenario& sc);

 private:
  // Render one offscreen frame from the scene camera, timed on both sides.
  void RenderFrame(Scene& s, FrameClock& fc, uint32_t frame);

  GpuContext& ctx_;
  World& world_;
  Simulation& sim_;
  Stream& stream_;
  const std::vector<MaterialDef>& mats_;
  const PerfOptions& opt_;

  PassTimer timer_;
  PassTimer renderTimer_;
  bool haveTimer_ = false, haveRenderTimer_ = false;
  rhi::Texture offscreen_;
  rhi::TextureView view_;
  FramePacer pacer_;

  // Frame -> sample index, so a GPU result arriving three frames late lands in
  // the row it belongs to instead of the present one.
  std::vector<int> frameToSample_;

  // Tick numbers continue ACROSS scenarios. They share one World, and a tick
  // number that went backwards would make the 3-bit stamp field gate the wrong
  // substep — the same reason selftest.cpp's kOrder exists.
  uint32_t tickCursor_ = 400;   // past the per-scenario 300-tick settle, with slack
  // The residency window as it stood before any scenario ran. Every Record()
  // reloads it — see the note there.
  IVec3 origin0_{};
};

void PerfRunner::RenderFrame(Scene& s, FrameClock& fc, uint32_t frame) {
  const double t0 = NowSeconds();
  WriteRenderParams(ctx_.queue, world_, s.eye, s.cam,
                    (float)opt_.width / (float)opt_.height, /*shadows=*/true,
                    0.0f, kFarFogDensity, (float)opt_.height, frame,
                    s.fluidLive);
  rhi::CommandEncoder enc = ctx_.device.CreateCommandEncoder();
  uint32_t rb = 0, re = 0;
  const bool timed =
      haveRenderTimer_ && renderTimer_.AllocPassPair("render", rb, re);
  // Bracket AROUND BeginRenderPass/End, not inside: a timestamp write is not
  // legal inside a dynamic-rendering scope on the ALL_COMMANDS path.
  if (timed) enc.WriteTimestamp(renderTimer_.NativeQuerySet(), rb, false);
  // Inside the bracket: the resolve is part of what a shadow costs, so leaving
  // it out would flatter the cache in the very table that judges it.
  sim_.EncodeShadowResolve(enc);
  {
    rhi::RenderPass rp = sim_.BeginRenderPass(
        enc, view_, rhi::TextureFormat::RGBA8Unorm, opt_.width, opt_.height);
    sim_.DrawWorld(rp);
    sim_.DrawParticles(rp);
    if (CurrentTuning().render.fluidSurface < 0.5f)
      sim_.DrawFluid(rp, s.fluidLive);
    rp.End();
  }
  if (timed) enc.WriteTimestamp(renderTimer_.NativeQuerySet(), re, true);
  renderTimer_.EncodeResolve(enc);
  pacer_.Mark(enc);
  ctx_.queue.Submit(enc.Finish());
  renderTimer_.KickDeferred(ctx_, frame);
  fc.Add(PerfScope::RenderCpu, t0, NowSeconds());

  // The frame-in-flight bound. Charged to `present`, because that is what it
  // is: the wait a real frame loop pays inside AcquireFrame when the swapchain
  // is full. Putting it anywhere else would make the CPU bars add up to a
  // number that is not the frame time.
  const double p0 = NowSeconds();
  pacer_.Throttle(ctx_);
  fc.Add(PerfScope::Present, p0, NowSeconds());
}

Run PerfRunner::Record(const Scenario& sc) {
  Run r;
  r.id = sc.id;
  r.label = sc.label;
  r.desc = sc.desc;
  r.stresses = sc.stresses;

  // EVERY SCENARIO STARTS FROM THE SAME WORLD, regenerated and re-settled.
  //
  // They share one World, and without this they also share its history: the
  // flythrough leaves the residency window somewhere else, the explosion
  // scenario then measures a different piece of terrain, and the tree burn
  // ignites a tree that the previous scenario may already have moved. It also
  // made `--scenario treeburn` and `treeburn inside --perf` produce different
  // world hashes (495f8515 vs 06043231) — two numbers for one measurement,
  // which is the standing invitation to compare the wrong pair.
  //
  // A worldgen plus a 300-tick settle is about ten seconds per scenario. That
  // buys independence: any subset of scenarios produces the same numbers as the
  // full suite, which is the only property that makes `--scenario` useful for
  // iterating.
  //
  // THE WINDOW ORIGIN HAS TO GO BACK TOO, and it is easy to miss because
  // worldgen does not touch it. `flythrough` flies 900 voxels across the world,
  // which shifts the residency window; the next scenario then regenerates a
  // world it is looking at from somewhere else entirely. Measured: `water` found
  // its authored lake when run alone and reported "no authored lake in the
  // window" inside the full suite, because by then the window had moved off the
  // origin region the lake is authored in.
  // THE GAME'S OWN REGEN SEQUENCE, in the game's order (main.cpp, ui.regenWorld):
  // drop the stream's bookkeeping, put the window back at the origin, regenerate.
  //
  // NOT ReloadWindow(), which is the other public "reset the window" call and
  // the obvious-looking choice: it force-fills all kNumChunks slots, which
  // materializes a page for every chunk and exhausts the pool outright — measured
  // `FATAL: page pool exhausted: 32382 pages needed, 0 free`. It exists for
  // LoadWorld, where the chunks come from a store and there is no worldgen pass
  // behind it. Here SubmitWorldgen regenerates everything a line later, so the
  // fill is both fatal and redundant.
  stream_.OnRegen();
  world_.SetWindowOrigin(origin0_);
  SubmitWorldgen(ctx_, world_, sim_, kDefaultSeed);
  ctx_.WaitIdle();
  for (uint32_t t = 1; t <= 300; t++)
    SubmitTick(ctx_, world_, sim_, t, kDefaultSeed, {}, {}, {}, t % 15 == 0,
               {8, 3, 8}, false, false);
  ctx_.WaitIdle();
  tickCursor_ = 400;

  Scene s{ctx_, world_, sim_, stream_, mats_};
  std::string why;
  if (!sc.setup(s, why)) {
    r.skipped = true;
    r.skipWhy = why;
    std::printf("  %-12s SKIPPED — %s\n", sc.id, why.c_str());
    return r;
  }
  r.note = s.note;

  timer_.ResetStats();
  renderTimer_.ResetStats();
  frameToSample_.assign(sc.warmTicks + sc.ticks + 8, -1);

  const uint64_t fills0 =
      world_.pages ? world_.pages->FillsIssued() : (uint64_t)0;
  uint64_t prevFills = fills0;

  // Tick numbering continues across scenarios — the world is shared and a tick
  // number that went backwards would make the stamp field lie.
  uint32_t tick = tickCursor_;
  const double runStart = NowSeconds();

  for (uint32_t i = 0; i < sc.warmTicks + sc.ticks; i++) {
    const bool recording = i >= sc.warmTicks;
    const uint32_t lt = recording ? i - sc.warmTicks : 0;
    const double frameT0 = NowSeconds();
    FrameClock fc;
    // Fold in whatever the spans below the harness billed on the PREVIOUS
    // iteration but after its drain — in practice nothing, since the drain is
    // the last thing a frame does, but a leftover silently misattributed to
    // this frame is exactly the kind of accounting bug this page exists to
    // catch, so the accumulator starts each frame empty by construction.
    PerfScopesDrain(fc.ms);
    for (int k = 0; k < kPerfScopeCount; k++) fc.ms[k] = 0;

    TickOps ops;
    {
      ScopeTimer sc1(fc, PerfScope::GameLogic);
      sc.drive(s, recording ? lt : 0, ops);
    }

    const IVec3 playerChunk{(int)s.eye.x >> 4, (int)s.eye.y >> 4,
                            (int)s.eye.z >> 4};
    {
      ScopeTimer sc2(fc, PerfScope::Stream);
      stream_.Update(playerChunk, tick);
      for (const ExplosionOp& e : ops.exps)
        stream_.MarkModifiedBox({e.x - e.radius, e.y - e.radius, e.z - e.radius},
                                {e.x + e.radius, e.y + e.radius, e.z + e.radius});
    }

    // SubmitTick BILLS ITSELF now (measure/perfscope.h), in five spans —
    // upload / waterBody / pageTableCpu / encode / submit — which land in the
    // process-global accumulator and are drained into `fc` at the bottom of
    // this frame. This used to be one ScopeTimer labelled `submit`, with a
    // comment arguing that splitting it would mean a second copy of SubmitTick
    // and that one honest bar beat three invented ones. That was right about
    // the copy and wrong about the conclusion: the fix is for the function to
    // measure its own parts, not for the caller to guess or to give up. The
    // live path made the same bundle and it is what made "submit spiked to
    // 30 ms while idle" undiagnosable.
    SubmitTick(ctx_, world_, sim_, tick, kDefaultSeed, ops.ops, ops.exps,
               ops.cells, /*hashEnable=*/tick % 15 == 0, playerChunk,
               /*wantReadback=*/true, ops.particlesActive, ops.spawns,
               /*farCount=*/0, ops.fluidSpawns, s.fluidLive);
    if (haveTimer_) timer_.KickDeferred(ctx_, i);

    RenderFrame(s, fc, i);

    {
      // The pump. This is where the async snapshot map and the deferred
      // timestamp maps retire — the game gets this for free by having real time
      // pass between submit and pump, and so, here, does the offscreen render
      // above. Nothing blocks.
      ScopeTimer sc4(fc, PerfScope::Readback);
      ctx_.ProcessEvents();
    }

    // Counters, from the snapshot the pump may just have landed.
    const WorldSnapshot& sn = world_.Snap();
    if (sn.valid) s.fluidLive = sn.fluidLive;

    // SubmitTick's five spans, drained into this frame's clock. Done here, one
    // statement before the sample is built, so the spans and the ScopeTimers
    // above are one accounting.
    PerfScopesDrain(fc.ms);

    if (recording) {
      PerfSample smp;
      smp.tick = tick;
      smp.frame = i;
      for (int k = 0; k < kPerfScopeCount; k++) smp.cpuMs[k] = fc.ms[k];
      // The residual: harness wall clock no scope claimed. Same row and same
      // meaning as the live path's, and it is what proves the bars add up.
      smp.cpuMs[(int)PerfScope::Other] += std::max(
          0.0, (NowSeconds() - frameT0) * 1000.0 - PerfCpuTotal(smp));
      smp.counters[(int)PerfCounter::ActiveChunks] =
          sn.valid ? (double)sn.activeChunks : 0.0;
      smp.counters[(int)PerfCounter::Particles] =
          sn.valid ? (double)sn.particleCount : 0.0;
      smp.counters[(int)PerfCounter::FluidParticles] =
          sn.valid ? (double)sn.fluidLive : 0.0;
      smp.counters[(int)PerfCounter::Ops] = (double)ops.ops.size();
      smp.counters[(int)PerfCounter::CellOps] = (double)ops.cells.size();
      smp.counters[(int)PerfCounter::Explosions] = (double)ops.exps.size();
      smp.counters[(int)PerfCounter::PageFaults] =
          sn.valid ? (double)sn.pageFaults : 0.0;
      smp.counters[(int)PerfCounter::VoxelsNonAir] =
          sn.valid ? (double)sn.voxelTotal : 0.0;
      if (world_.pages) {
        smp.counters[(int)PerfCounter::PagesResident] =
            (double)world_.pages->PagesInUse();
        const uint64_t f = world_.pages->FillsIssued();
        smp.counters[(int)PerfCounter::PageFills] = (double)(f - prevFills);
        prevFills = f;
      }
      smp.counters[(int)PerfCounter::DrawCalls] = 3;   // world, particles, fluid
      smp.wallMs = (NowSeconds() - frameT0) * 1000.0;
      frameToSample_[i] = (int)r.samples.size();
      r.samples.push_back(smp);
    }

    // Harvest whatever GPU timings have landed and post them to their own rows.
    auto post = [&](PassTimer& t, bool isRender) {
      const uint32_t tag = t.LastFrameTag();
      if (tag >= frameToSample_.size()) return;
      const int si = frameToSample_[tag];
      if (si < 0) return;   // a warmup frame: its numbers are not recorded
      PerfSample& dst = r.samples[(size_t)si];
      for (const PassSample& ps : t.LastFrame()) {
        // Render spans go through kPerfRenderSpans — the same table the live
        // path in main.cpp uses, so "render" bills to the same row on both.
        int node = isRender ? PerfNodeForRenderSpan(ps.name)
                            : PerfNodeForPass(ps.name);
        if (node < 0) {
          r.unattributedNs += ps.ns;
          bool seen = false;
          for (const std::string& u : r.unattributedNames)
            if (u == ps.name) { seen = true; break; }
          if (!seen) r.unattributedNames.emplace_back(ps.name);
          continue;
        }
        dst.gpuMs[node] += (double)ps.ns / 1e6;
        dst.gpuValid = true;
      }
    };
    if (haveTimer_ && timer_.PollDeferred(ctx_) > 0) post(timer_, false);
    if (haveRenderTimer_ && renderTimer_.PollDeferred(ctx_) > 0)
      post(renderTimer_, true);

    tick++;
  }

  // Drain the last few frames of in-flight timestamps so the tail of the chart
  // is not a cliff of gpuValid=false rows.
  pacer_.Drain();
  ctx_.WaitIdle();
  for (int drain = 0; drain < 8; drain++) {
    ctx_.ProcessEvents();
    if (haveTimer_ && timer_.PollDeferred(ctx_) > 0) {
      const uint32_t tag = timer_.LastFrameTag();
      if (tag < frameToSample_.size() && frameToSample_[tag] >= 0) {
        PerfSample& dst = r.samples[(size_t)frameToSample_[tag]];
        for (const PassSample& ps : timer_.LastFrame()) {
          const int node = PerfNodeForPass(ps.name);
          if (node < 0) { r.unattributedNs += ps.ns; continue; }
          dst.gpuMs[node] += (double)ps.ns / 1e6;
          dst.gpuValid = true;
        }
      }
    }
    if (haveRenderTimer_) renderTimer_.PollDeferred(ctx_);
  }

  if (sc.verify) sc.verify(s);
  r.note = s.note;

  r.passStats = timer_.Stats();
  r.warmupMs = (NowSeconds() - runStart) * 1000.0;
  r.worldHash = HashWorldNow(ctx_, world_, sim_, kDefaultSeed);
  ctx_.WaitIdle();
  tickCursor_ = tick + 1;

  // Terminal summary. The JSON is for the page; this is for the person who ran
  // the command and wants the headline without opening a browser.
  std::vector<double> wall;
  for (const PerfSample& smp : r.samples) wall.push_back(smp.wallMs);
  const double p50 = Percentile(wall, 0.50), p95 = Percentile(wall, 0.95),
               p99 = Percentile(wall, 0.99);
  int gpuValid = 0;
  for (const PerfSample& smp : r.samples) gpuValid += smp.gpuValid ? 1 : 0;
  std::printf("  %-12s %4zu frames  p50 %6.2f  p95 %6.2f  p99 %6.2f ms  "
              "(%.0f fps p50)  gpu rows %d/%zu  hash %08x\n",
              sc.id, r.samples.size(), p50, p95, p99,
              p50 > 0 ? 1000.0 / p50 : 0.0, gpuValid, r.samples.size(),
              r.worldHash);
  return r;
}

// ---------------------------------------------------------------------------
// --render-budget: WHERE INSIDE THE RAYMARCH THE FRAME WENT.
//
// WHY THIS EXISTS. `--perf` reports the render pass as ONE number — `raymarch`,
// which on the idle scenario is 16.6 ms of a 16.6 ms GPU frame. That is a bare
// count in exactly the sense CLAUDE.md's rule 6 means it: it says the frame is
// the raymarch and stops, and the only way forward from it is to start turning
// features off one at a time, one binary invocation per hypothesis, which is
// the sequence that rule names as indefensible.
//
// So this is the attribution instead. ONE process, ONE world, ONE camera, and
// an ARM per suspected cost centre — each arm renders the identical frame with
// exactly one knob moved and reports its own GPU span. The delta from baseline
// is that feature's cost, and the whole table lands in one run.
//
// WHAT AN ARM'S NUMBER IS AND IS NOT. `noshadow` minus `baseline` is the cost
// of the shadow ray. It is NOT a proposal to ship without shadows — an arm is a
// measurement, not a setting, and several of them (nofar, primary256) would be
// visibly wrong to play with. Read the column as "this many ms are spent here",
// then decide separately whether that work can be made cheaper or skipped.
//
// EVERY ARM RENDERS THE SAME WORLD STATE. The world is generated and settled
// ONCE, before the first arm, and no arm ticks the sim — a re-settle between
// arms would put different voxels in front of the camera and every delta would
// be measuring terrain instead of the knob. That also makes the run cheap: the
// ~10 s setup is paid once rather than per arm.
//
// A RECOMPILE IS NOT A CONFOUND. Most knobs here are TUNE_* WGSL constants, so
// an arm has to reload the shaders (the F5 path) to take effect. The reload
// happens BEFORE the arm's warmup frames and is not inside the timed span; the
// GPU timestamps bracket the render pass itself.
// ---------------------------------------------------------------------------
// RenderParams.time — the ANIMATION clock (waves, flicker, sway), NOT the time
// of day. That trap cost a run: `time` looks like an hour-of-day parameter and
// --shot passes 11.7 to it, but WriteRenderParams derives the sun and moons
// from Celestial().RenderTickInterp(TICK, ...) and ignores `time` entirely for
// lighting. Setting it to "midday" produced a starfield. The time of day is
// chosen by kNoonTick below, which is a real search over the celestial cycle.
constexpr float kBudgetAnimTime = 11.7f;

// The celestial tick whose sky has the sun highest, found by scanning the cycle
// rather than hardcoded: cycleMinutes is a tuning value, so any constant here
// would silently become the wrong time of day the first time it moved.
//
// WHY DAYLIGHT MATTERS TO A RENDER BUDGET AT ALL: sunShadowAt is gated on
// `lambert > 0.0`, so at night the key light is the moon, most of the frame
// fails that test, and the largest row in this table is measured on the pixels
// that would NOT cast it in a real session. Budget under the lighting the game
// is played in.
uint32_t FindNoonTick(const Tuning& tun) {
  uint32_t best = 0;
  float bestUp = -2.0f;
  // Coarse scan over a generous superset of any authored cycle length; the sun
  // elevation is smooth, so step 64 cannot miss the peak by anything that
  // matters to a lighting condition.
  for (uint32_t t = 0; t < 200000u; t += 64u) {
    const float up = ComputeSky(tun, (double)t).sunDir[1];
    if (up > bestUp) { bestUp = up; best = t; }
  }
  return best;
}

struct RenderArm {
  const char* id;
  const char* label;      // what moved, in the units the knob is written in
  // Mutates a COPY of the baseline tuning. nullptr = leave tuning alone.
  void (*apply)(Tuning& t);
  bool shadows;           // the RenderParams shadow bit for this arm
  uint32_t widthDiv;      // 1 = full res, 2 = half res in each axis
  const char* means;      // what the delta from baseline is the cost OF
};

const RenderArm kRenderArms[] = {
    {"baseline", "everything on", nullptr, true, 1,
     "the frame as it ships — every other row is a delta from this"},

    // The shadow ray is a SECOND FULL trace() per lit pixel (sunShadowAt), and
    // with shadowMaxDist at 999 m nothing in a 512-voxel window ever reaches
    // the cheap cascade path, so every one of them is a fine march.
    {"noshadow", "sun shadows off (RenderParams bit)", nullptr, false, 1,
     "the shadow ray's TRAVERSAL — the call site stays in the shader"},
    // THE PAIR THAT SEPARATES WORK FROM FOOTPRINT, and the reason both halves
    // are here. `noshadow` turns the shadow ray off through a RenderParams bit,
    // at RUNTIME: the branch is not taken, but the inlined trace() is still in
    // the compiled shader and still owns its registers. `shadow0` turns it off
    // through a WGSL const, at COMPILE time, so the whole call can be folded
    // away. The difference between the two is not traversal — no ray is cast in
    // either — it is the OCCUPANCY TAX of that call site existing at all.
    {"shadow0", "shadowSteps 384 -> 0 (const-folded away)",
     [](Tuning& t) { t.render.shadowSteps = 0; }, true, 1,
     "traversal PLUS the register footprint of the shadow call site"},
    {"shadow64", "shadowSteps 384 -> 64",
     [](Tuning& t) { t.render.shadowSteps = 64; }, true, 1,
     "how DEEP the shadow march runs past 64 steps before it finds a blocker"},
    {"shadow16", "shadowSteps 384 -> 16",
     [](Tuning& t) { t.render.shadowSteps = 16; }, true, 1,
     "the same, past 16 — with shadow64 this brackets the march's real depth"},

    // The primary ray. 4096 steps is a budget, not a cost: the DDA breaks on
    // the first blocker. Clamping it says how far the average ray actually got.
    {"primary256", "primarySteps 4096 -> 256",
     [](Tuning& t) { t.render.primarySteps = 256; }, true, 1,
     "primary DDA steps past 256 — near zero here means rays terminate early"},

    // In-window LOD. Everything nearer than lodHandoffDist marches at full 10 cm
    // resolution; past it the cascade takes over.
    {"lod8", "lodHandoffDist 24 -> 8 m",
     [](Tuning& t) { t.render.lodHandoffDist = 8.0f; }, true, 1,
     "fine marching between 8 m and 24 m that the cascade could have covered"},

    {"nofar", "farSteps 384 -> 0",
     [](Tuning& t) { t.render.farSteps = 0; }, true, 1,
     "the far-field cascade march for rays that leave the fine window"},

    {"nomicro", "microMaxPerRay 8 -> 0",
     [](Tuning& t) { t.render.microMaxPerRay = 0; }, true, 1,
     "micro-brick traversal: grass strands, petals, tufts"},

    {"noreflect", "reflectionSteps 96 -> 0",
     [](Tuning& t) { t.render.reflectionSteps = 0; }, true, 1,
     "secondary reflection rays off water and ice"},
    // The Fresnel gate, not the step budget: this stops traceReflection from
    // being CALLED at the three water sites (it does not gate ice, which has
    // its own iceReflectMin). Against `noreflect` it separates "the reflection
    // ray is expensive" from "the reflection ray fires on far more pixels than
    // it should".
    // The same work/footprint pair as noshadow vs shadow0, for reflections.
    // These three constants gate every traceReflection() call site there is
    // (water x2, MPM surface, and ice, which has its own threshold), so no
    // reflection ray can be cast — but reflectionSteps is unchanged, so the
    // inlined trace() inside traceReflection is still compiled in. Against
    // `noreflect` this is the entire difference between casting the ray and
    // merely being able to.
    {"reflgate", "all reflection call sites gated off (steps unchanged)",
     [](Tuning& t) {
       t.render.reflectionCutoff = 1.0f;
       t.render.iceReflectMin = 2.0f;
       t.render.fluidReflect = 0.0f;
     },
     true, 1, "reflection ray TRAVERSAL only, with the code still resident"},
    // ---- the voxel-keyed shadow cache (world.h kShadowCacheBuckets) ----
    // `nocache` is THE A/B for the whole feature: it recompiles raymarch.wgsl
    // with the old inline shadow ray and turns the resolve pass off, so
    // baseline - nocache is what the cache is worth, in one process, against
    // the identical world and camera.
    {"nocache", "shadow cache off (inline per-pixel ray, the old path)",
     [](Tuning& t) { t.render.shadowCache = 0; }, true, 1,
     "the whole shadow cache: rays deduplicated PLUS the trace() call site "
     "removed from the fragment shader"},
    // The granularity curve. Patch size is a quality knob, but it is also what
    // decides how much dedup there IS: a patch finer than a pixel dedupes
    // nothing, and at 1080p a 10 cm voxel is already only ~6.5 px across at the
    // 24 m fine-march limit, so subdiv 4 makes patches sub-pixel over most of an
    // overlook frame. These arms measure that instead of arguing about it.
    {"cachesub1", "shadow patch subdiv 4 -> 1 (whole voxel face)",
     [](Tuning& t) { t.render.shadowCacheSubdiv = 1; }, true, 1,
     "how much of the resolve pass's cost is patches finer than a pixel"},
    {"cachesub2", "shadow patch subdiv 4 -> 2",
     [](Tuning& t) { t.render.shadowCacheSubdiv = 2; }, true, 1,
     "the same, at the midpoint"},
    {"shadow32", "shadowSteps 384 -> 32",
     [](Tuning& t) { t.render.shadowSteps = 32; }, true, 1,
     "shadow march past 32 steps"},

    // Not a feature — the denominator. If the frame halves in cost at a quarter
    // of the pixels it is per-pixel ray work, and the fix is fewer rays or
    // cheaper ones; if it does not, something fixed-cost dominates.
    {"halfres", "render 960x540 instead of 1920x1080", nullptr, true, 2,
     "pixel-linearity: a 4x pixel cut should be a ~4x time cut"},
};
constexpr int kRenderArmCount =
    (int)(sizeof(kRenderArms) / sizeof(kRenderArms[0]));

// One arm's measured result.
struct ArmResult {
  const RenderArm* arm = nullptr;
  double gpuP50 = 0, gpuP95 = 0;
  int frames = 0;
  bool ok = false;
  std::string why;
};

class RenderBudgetRunner {
 public:
  RenderBudgetRunner(GpuContext& ctx, World& world, Simulation& sim,
                     Stream& stream, const PerfOptions& opt)
      : ctx_(ctx), world_(world), sim_(sim), stream_(stream), opt_(opt) {}

  bool Init() {
    haveTimer_ = timer_.Init(ctx_, 2);
    if (!pacer_.Init(ctx_)) return false;
    for (int i = 0; i < 2; i++) {
      const uint32_t d = i == 0 ? 1u : 2u;
      tex_[i] = ctx_.device.CreateTexture(
          {opt_.width / d, opt_.height / d, 1}, rhi::TextureFormat::RGBA8Unorm,
          rhi::TextureUsage::RenderAttachment | rhi::TextureUsage::CopySrc,
          "renderBudgetTarget");
      if (!tex_[i]) return false;
      view_[i] = tex_[i].CreateView();
    }
    return haveTimer_;
  }

  // Generate + settle the world ONCE. Every arm renders this exact state.
  void SettleWorld() {
    stream_.OnRegen();
    SubmitWorldgen(ctx_, world_, sim_, kDefaultSeed);
    ctx_.WaitIdle();
    for (uint32_t t = 1; t <= 300; t++)
      SubmitTick(ctx_, world_, sim_, t, kDefaultSeed, {}, {}, {}, t % 15 == 0,
                 {8, 3, 8}, false, false);
    ctx_.WaitIdle();
  }

  ArmResult Measure(const RenderArm& arm, const Tuning& base, const Scene& s,
                    uint32_t warm, uint32_t frames) {
    ArmResult res;
    res.arm = &arm;

    // Apply the arm to a COPY of the baseline, then take the F5 path so the
    // TUNE_* constants are re-const-folded into the shader.
    Tuning t = base;
    if (arm.apply) {
      arm.apply(t);
      SetCurrentTuning(t);
      if (!sim_.ReloadShaders(ctx_.device)) {
        SetCurrentTuning(base);
        sim_.ReloadShaders(ctx_.device);
        res.why = "shader reload failed";
        return res;
      }
    } else {
      SetCurrentTuning(t);
    }

    const uint32_t d = arm.widthDiv;
    const uint32_t W = opt_.width / d, H = opt_.height / d;
    const rhi::TextureView& view = view_[d == 1 ? 0 : 1];

    std::vector<double> ms;
    timer_.ResetStats();
    for (uint32_t f = 0; f < warm + frames; f++) {
      // The camera and the frame index are FIXED across arms. `frame` feeds
      // the dither key and every animated term (waves, flicker, sway); letting
      // it advance would make each arm a slightly different picture and put
      // that difference into the delta.
      // MIDDAY (11.7), not the 0.0 the --perf harness passes. Time of day is
      // not cosmetic here: at night keyLightDir is the moon, `lambert > 0.0`
      // rejects most of the frame, and the shadow ray — the single largest row
      // in this table — is never cast on the pixels that would cast it in a
      // real session. A budget must be taken under the lighting the game is
      // actually played in.
      WriteRenderParams(ctx_.queue, world_, s.eye, s.cam, (float)W / (float)H,
                        arm.shadows, kBudgetAnimTime, kFarFogDensity, (float)H,
                        noonTick_, s.fluidLive);
      rhi::CommandEncoder enc = ctx_.device.CreateCommandEncoder();
      uint32_t rb = 0, re = 0;
      const bool timed = timer_.AllocPassPair("render", rb, re);
      if (timed) enc.WriteTimestamp(timer_.NativeQuerySet(), rb, false);
      sim_.EncodeShadowResolve(enc);
      {
        rhi::RenderPass rp = sim_.BeginRenderPass(
            enc, view, rhi::TextureFormat::RGBA8Unorm, W, H);
        sim_.DrawWorld(rp);
        rp.End();
      }
      if (timed) enc.WriteTimestamp(timer_.NativeQuerySet(), re, true);
      timer_.EncodeResolve(enc);
      pacer_.Mark(enc);
      ctx_.queue.Submit(enc.Finish());
      timer_.KickDeferred(ctx_, f);
      pacer_.Throttle(ctx_);
      ctx_.ProcessEvents();
      if (timer_.PollDeferred(ctx_) > 0) {
        const uint32_t tag = timer_.LastFrameTag();
        if (tag >= warm)
          for (const PassSample& ps : timer_.LastFrame())
            ms.push_back((double)ps.ns / 1e6);
      }
    }
    pacer_.Drain();
    ctx_.WaitIdle();
    for (int drain = 0; drain < 8; drain++) {
      ctx_.ProcessEvents();
      if (timer_.PollDeferred(ctx_) > 0 && timer_.LastFrameTag() >= warm)
        for (const PassSample& ps : timer_.LastFrame())
          ms.push_back((double)ps.ns / 1e6);
    }

    if (ms.empty()) {
      res.why = "no GPU timestamps landed";
      return res;
    }
    res.gpuP50 = Percentile(ms, 0.50);
    res.gpuP95 = Percentile(ms, 0.95);
    res.frames = (int)ms.size();
    res.ok = true;
    return res;
  }

  // SHOW WHAT WAS MEASURED.
  //
  // Every row of the table above is a property of ONE PICTURE, and "reflections
  // are 28% of the frame" means something completely different depending on
  // whether that picture is a lake or a hillside. A budget printed without the
  // frame beside it invites exactly the misreading the table exists to prevent,
  // so the run writes the baseline frame out and says what it summed to — an
  // all-black render is a plausible way for these numbers to be nonsense, and
  // "wrote the file" would not catch it.
  //
  // Own encoder, submitted after the render has retired: a copy folded into the
  // render encoder reads back zeros.
  void Shot(const Scene& s, const char* path) {
    const uint32_t W = opt_.width, H = opt_.height;
    WriteRenderParams(ctx_.queue, world_, s.eye, s.cam, (float)W / (float)H,
                      /*shadows=*/true, kBudgetAnimTime, kFarFogDensity,
                      (float)H, noonTick_, s.fluidLive);
    rhi::CommandEncoder enc = ctx_.device.CreateCommandEncoder();
    {
      rhi::RenderPass rp = sim_.BeginRenderPass(
          enc, view_[0], rhi::TextureFormat::RGBA8Unorm, W, H);
      sim_.DrawWorld(rp);
      rp.End();
    }
    ctx_.queue.Submit(enc.Finish());
    ctx_.WaitIdle();

    rhi::Buffer buf = CreateBuffer(
        ctx_.device, (uint64_t)W * H * 4,
        rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst, "budgetShot");
    rhi::CommandEncoder cenc = ctx_.device.CreateCommandEncoder();
    rhi::TexelCopyTexture src{};
    src.texture = tex_[0];
    rhi::TexelCopyBuffer dst{};
    dst.buffer = buf;
    dst.bytesPerRow = W * 4;
    dst.rowsPerImage = H;
    cenc.CopyTextureToBuffer(src, dst, {W, H, 1});
    ctx_.queue.Submit(cenc.Finish());
    ctx_.WaitIdle();

    std::vector<uint8_t> px((size_t)W * H * 4, 0);
    if (!rhi::ReadBufferBlocking(ctx_.device, buf, 0, px.data(), px.size()))
      return;
    uint64_t sum = 0;
    for (uint8_t b : px) sum += b;
    if (WriteBmpFile(path, px, W, H))
      std::printf("  frame written to %s (pixel sum %llu%s)\n", path,
                  (unsigned long long)sum,
                  sum == 0 ? " *** ALL BLACK — the table above is not of "
                             "anything ***" : "");
  }

 private:
  GpuContext& ctx_;
  World& world_;
  Simulation& sim_;
  Stream& stream_;
  const PerfOptions& opt_;
  PassTimer timer_;
  bool haveTimer_ = false;
  rhi::Texture tex_[2];
  rhi::TextureView view_[2];
  FramePacer pacer_;
 public:
  // Fixed across every arm: the frame each arm renders must be the SAME frame,
  // and this is the tick the sky is derived from.
  uint32_t noonTick_ = 0;
};

}  // namespace

int RunRenderBudget(GpuContext& ctx, World& world, Simulation& sim,
                    const std::vector<MaterialDef>& mats,
                    const PerfOptions& opt) {
  std::printf("=== sandvox --render-budget: where inside the raymarch ===\n");
  std::printf("adapter: %s\n", ctx.DeviceName().c_str());
  if (!ctx.timestampsEnabled) {
    std::fprintf(stderr,
                 "--render-budget: this device has no GPU timestamps. Every "
                 "number would be zero. Refusing.\n");
    return 1;
  }

  // Which camera. `--scenario <id>` borrows any --perf scenario's viewpoint;
  // with no scenario named, this uses its OWN overlook (below) rather than
  // `idle`'s.
  //
  // WHY NOT JUST USE `idle`. Because `idle` is not a view of the world. Its eye
  // sits 18 VOXELS — 1.8 m — above the terrain height at (256, 256), which in
  // this seed puts it hard against a rock face: the frame it renders is ~70% a
  // wall two metres away, a strip of grass, and a sliver of sky. That is a fine
  // scenario for --perf, whose job is a repeatable frame, and a bad one for
  // attribution, because a camera that sees no water reports the reflection
  // budget of a world with no water in it and a camera whose rays all terminate
  // in 20 voxels reports that the primary march is free.
  //
  // The screenshot at the end of this run exists because that misreading is
  // otherwise undetectable from the table.
  const Scenario* pick = nullptr;
  if (!opt.only.empty()) {
    for (const Scenario& s : kScenarios)
      if (opt.only == s.id) { pick = &s; break; }
    if (!pick) {
      std::fprintf(stderr, "--render-budget: no scenario named '%s'\n",
                   opt.only.c_str());
      return 1;
    }
  }

  Stream stream;
  stream.Init(&ctx, &world, &sim, kDefaultSeed);
  stream.OnMaterialsReloaded(mats);

  RenderBudgetRunner runner(ctx, world, sim, stream, opt);
  if (!runner.Init()) {
    std::fprintf(stderr, "--render-budget: could not set up the render target "
                         "or the timer\n");
    return 1;
  }
  runner.SettleWorld();

  Scene scene{ctx, world, sim, stream, mats};
  if (pick) {
    std::string why;
    if (!pick->setup(scene, why)) {
      std::fprintf(stderr, "--render-budget: scenario '%s' declined: %s\n",
                   pick->id, why.c_str());
      return 1;
    }
  } else {
    // THE OVERLOOK. High enough to clear the terrain it stands on, pitched down
    // far enough that the frame is roughly a third near ground, a third middle
    // distance and a third horizon-and-sky. That mix is the point: it exercises
    // the fine march, the in-window LOD handoff, the far cascade and the sky
    // path in one frame, in something like the proportion a player standing on
    // a hillside sees. A camera that sees only one of those reports a budget
    // for one of those.
    const int gx = 256, gz = 256;
    const int h = World::TerrainHeight(gx, gz, kDefaultSeed);
    scene.eye = {(float)gx, (float)(h + 120), (float)gz};
    scene.cam.yaw = 0.785f;
    scene.cam.pitch = -0.32f;
    scene.note = "overlook: 12 m up, pitched into the middle distance";
  }
  std::printf("camera: %s — %s\n", pick ? pick->id : "overlook",
              scene.note.c_str());
  runner.noonTick_ = FindNoonTick(CurrentTuning());
  std::printf("        eye (%.0f, %.0f, %.0f)  yaw %.2f  pitch %.2f\n",
              scene.eye.x, scene.eye.y, scene.eye.z, scene.cam.yaw,
              scene.cam.pitch);
  std::printf("        sky at celestial tick %u — sun elevation %+.2f, the "
              "highest in the cycle\n",
              runner.noonTick_,
              ComputeSky(CurrentTuning(), (double)runner.noonTick_).sunDir[1]);
  std::printf("render: %ux%u   arms: %d   (one settled world, one camera, "
              "one knob moved per arm)\n\n",
              opt.width, opt.height, kRenderArmCount);

  // The tuning every arm is a delta FROM. Restored after each arm, and again at
  // the end, so nothing here leaks into a later harness in the same process.
  const Tuning base = CurrentTuning();

  std::vector<ArmResult> out;
  for (const RenderArm& arm : kRenderArms) {
    ArmResult r = runner.Measure(arm, base, scene, /*warm=*/12, /*frames=*/48);
    std::printf("  %-11s %-42s ", arm.id, arm.label);
    if (!r.ok) std::printf("SKIPPED — %s\n", r.why.c_str());
    else std::printf("%7.2f ms\n", r.gpuP50);
    std::fflush(stdout);
    out.push_back(r);
  }
  SetCurrentTuning(base);
  sim.ReloadShaders(ctx.device);
  runner.Shot(scene, "build/render_budget.bmp");

  // ---- shadow cache attribution (CLAUDE.md rule 6) ----------------------
  // A bare "the resolve pass costs 3.9 ms" is the kind of number that invites
  // one elimination run per hypothesis. The two numbers that actually decide
  // between them — how many patches were asked for, and how many were REFUSED
  // because the request list was full — are two words the shader already
  // maintains, so read them instead of guessing. The Shot above rendered a
  // frame with the baseline tuning, so these describe that frame.
  if (sim.ShadowCacheOn()) {
    rhi::Buffer stage =
        CreateBuffer(ctx.device, 16,
                     rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                     "shadowStatsRead");
    rhi::CommandEncoder senc = ctx.device.CreateCommandEncoder();
    senc.CopyBufferToBuffer(world.shadowReq, 0, stage, 0, 16);
    ctx.queue.Submit(senc.Finish());
    uint32_t w[4] = {0, 0, 0, 0};
    if (rhi::ReadBufferBlocking(ctx.device, stage, 0, w, sizeof(w))) {
      const double px = (double)opt.width * opt.height;
      std::printf(
          "\n  shadow cache: %u patches requested, %u resolved (cap %u), %u "
          "refused\n            %.2f patches per 100 px — under 100 is real "
          "dedup, at the cap it is truncation\n",
          w[2], w[1], kShadowReqCap, w[3], 100.0 * (double)w[2] / px);
    }
  }

  // ---- the table the run exists to print --------------------------------
  const double b = out.empty() || !out[0].ok ? 0.0 : out[0].gpuP50;
  std::printf("\n  baseline raymarch: %.2f ms at %ux%u\n\n", b, opt.width,
              opt.height);
  std::printf("  %-11s %9s %9s  %s\n", "arm", "ms", "saved", "what the saving is the cost of");
  std::printf("  %-11s %9s %9s  %s\n", "---", "--", "-----", "------------------------------");
  for (const ArmResult& r : out) {
    if (!r.ok) { std::printf("  %-11s   SKIPPED  %s\n", r.arm->id, r.why.c_str()); continue; }
    if (r.arm == &kRenderArms[0]) continue;
    const double saved = b - r.gpuP50;
    std::printf("  %-11s %9.2f %8.2f%s  %s\n", r.arm->id, r.gpuP50, saved,
                b > 0 ? "" : " ", r.arm->means);
  }
  std::printf("\n  percentages of the %.2f ms baseline:\n", b);
  for (const ArmResult& r : out) {
    if (!r.ok || r.arm == &kRenderArms[0] || b <= 0) continue;
    std::printf("    %-11s %5.1f%%\n", r.arm->id, 100.0 * (b - r.gpuP50) / b);
  }
  std::printf(
      "\n  An arm is a MEASUREMENT, not a setting: `nofar` and `primary256` "
      "render\n  a visibly wrong world. Read each row as \"this many ms are "
      "spent here\".\n");
  return ctx.ReportVkValidation("--render-budget") > 0 ? 1 : 0;
}

namespace {

// ---------------------------------------------------------------------------
// JSON emission
// ---------------------------------------------------------------------------

// Series are emitted as flat arrays rather than an array of objects: a 900-row
// scenario with 13 CPU scopes and 25 nodes is ~35k numbers, and the object form
// costs about 12x the bytes in repeated key strings. The page reads them by
// index against the `scopes` and `nodes` headers.
void EmitSeries(std::FILE* f, const char* key, const std::vector<double>& v) {
  std::fprintf(f, "%s:[", JStr(key).c_str());
  for (size_t i = 0; i < v.size(); i++)
    std::fprintf(f, "%s%s", i ? "," : "", JNum(v[i]).c_str());
  std::fprintf(f, "]");
}

bool WriteJson(const std::string& path, const std::vector<Run>& runs,
               const PerfOptions& opt, GpuContext& ctx, World& world,
               bool haveTimer) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    std::fprintf(stderr, "--perf: cannot write %s\n", path.c_str());
    return false;
  }
  std::fprintf(f, "{\n\"schema\":2,\n");

  // ---- build identity -----------------------------------------------------
  // The page's header says WHICH build these numbers describe. Without it a
  // stale perf.json reads exactly like a fresh one, and "the CA got slower"
  // becomes a claim about a file nobody rebuilt.
  std::fprintf(f, "\"build\":{");
  std::fprintf(f, "\"adapter\":%s,", JStr(ctx.DeviceName()).c_str());
  std::fprintf(f, "\"backend\":\"vulkan\",");
  std::fprintf(f, "\"worldN\":%u,\"chunk\":%u,\"chunks\":%u,",
               kWorldN, kChunk, kNumChunks);
  std::fprintf(f, "\"voxelMeters\":%s,", JNum(kVoxelMeters).c_str());
  std::fprintf(f, "\"tickHz\":30,");
  std::fprintf(f, "\"residency\":%s,",
               JStr(world.residency == World::Residency::Paged ? "paged"
                                                               : "dense").c_str());
  std::fprintf(f, "\"renderW\":%u,\"renderH\":%u,", opt.width, opt.height);
  std::fprintf(f, "\"timestamps\":%s", haveTimer ? "true" : "false");
  std::fprintf(f, "},\n");

  // ---- the node taxonomy, straight from perfnodes.h -----------------------
  std::fprintf(f, "\"nodes\":[");
  for (int i = 0; i < kPerfNodeCount; i++) {
    const PerfNodeDef& n = kPerfNodes[i];
    std::fprintf(f, "%s{\"id\":%s,\"label\":%s,\"parent\":%s,\"side\":%s,"
                    "\"note\":%s}",
                 i ? "," : "", JStr(n.node).c_str(), JStr(n.label).c_str(),
                 n.parent ? JStr(n.parent).c_str() : "null",
                 JStr(n.side == PerfSide::Cpu   ? "cpu"
                      : n.side == PerfSide::Gpu ? "gpu"
                                                : "both").c_str(),
                 JStr(n.costNote).c_str());
  }
  std::fprintf(f, "],\n");

  std::fprintf(f, "\"scopes\":[");
  for (int i = 0; i < kPerfScopeCount; i++) {
    const int ni = PerfNodeIndexForScope((PerfScope)i);
    std::fprintf(f, "%s{\"key\":%s,\"node\":%s}", i ? "," : "",
                 JStr(kPerfScopeKeys[i]).c_str(),
                 ni >= 0 ? JStr(kPerfNodes[ni].node).c_str() : "null");
  }
  std::fprintf(f, "],\n");

  std::fprintf(f, "\"counters\":[");
  for (int i = 0; i < kPerfCounterCount; i++)
    std::fprintf(f, "%s{\"key\":%s,\"label\":%s,\"node\":%s,\"bug\":%s}",
                 i ? "," : "", JStr(kPerfCounters[i].key).c_str(),
                 JStr(kPerfCounters[i].label).c_str(),
                 JStr(kPerfCounters[i].node).c_str(),
                 kPerfCounters[i].isBug ? "true" : "false");
  std::fprintf(f, "],\n");

  // ---- scenarios ----------------------------------------------------------
  std::fprintf(f, "\"scenarios\":[\n");
  for (size_t ri = 0; ri < runs.size(); ri++) {
    const Run& r = runs[ri];
    std::fprintf(f, "%s{", ri ? ",\n" : "");
    std::fprintf(f, "\"id\":%s,\"label\":%s,\"desc\":%s,\"note\":%s,",
                 JStr(r.id).c_str(), JStr(r.label).c_str(),
                 JStr(r.desc).c_str(), JStr(r.note).c_str());
    std::fprintf(f, "\"stresses\":%s,", JStr(r.stresses).c_str());
    std::fprintf(f, "\"skipped\":%s,\"skipWhy\":%s,",
                 r.skipped ? "true" : "false", JStr(r.skipWhy).c_str());
    if (r.skipped) { std::fprintf(f, "\"frames\":0}"); continue; }

    std::fprintf(f, "\"worldHash\":%s,",
                 JStr([&] { char b[16]; std::snprintf(b, sizeof b, "%08x", r.worldHash); return std::string(b); }()).c_str());
    std::fprintf(f, "\"frames\":%zu,\n", r.samples.size());

    // wall clock + fps
    std::vector<double> wall, tickNo;
    for (const PerfSample& s : r.samples) {
      wall.push_back(s.wallMs);
      tickNo.push_back((double)s.tick);
    }
    std::fprintf(f, "  \"series\":{");
    EmitSeries(f, "tick", tickNo);
    std::fprintf(f, ",");
    EmitSeries(f, "wallMs", wall);

    // per-scope CPU
    std::fprintf(f, ",\"cpu\":{");
    bool firstScope = true;
    for (int k = 0; k < kPerfScopeCount; k++) {
      std::vector<double> v;
      double sum = 0;
      for (const PerfSample& s : r.samples) { v.push_back(s.cpuMs[k]); sum += s.cpuMs[k]; }
      // Drop scopes that never fired: a headless run has no audio and no
      // present, and thirteen flat-zero arrays per scenario is 40% of the file.
      if (sum <= 0.0) continue;
      if (!firstScope) std::fprintf(f, ",");
      firstScope = false;
      EmitSeries(f, kPerfScopeKeys[k], v);
    }
    std::fprintf(f, "}");

    // per-node GPU
    std::fprintf(f, ",\"gpu\":{");
    bool first = true;
    for (int n = 0; n < kPerfNodeCount; n++) {
      std::vector<double> v;
      double sum = 0;
      for (const PerfSample& s : r.samples) { v.push_back(s.gpuMs[n]); sum += s.gpuMs[n]; }
      if (sum <= 0.0) continue;
      if (!first) std::fprintf(f, ",");
      first = false;
      EmitSeries(f, kPerfNodes[n].node, v);
    }
    std::fprintf(f, "}");

    // gpuValid mask: which rows carry real GPU numbers.
    std::vector<double> valid;
    for (const PerfSample& s : r.samples) valid.push_back(s.gpuValid ? 1 : 0);
    std::fprintf(f, ",");
    EmitSeries(f, "gpuValid", valid);

    // counters
    std::fprintf(f, ",\"counters\":{");
    first = true;
    for (int c = 0; c < kPerfCounterCount; c++) {
      std::vector<double> v;
      double sum = 0;
      for (const PerfSample& s : r.samples) { v.push_back(s.counters[c]); sum += s.counters[c]; }
      if (sum <= 0.0) continue;
      if (!first) std::fprintf(f, ",");
      first = false;
      EmitSeries(f, kPerfCounters[c].key, v);
    }
    std::fprintf(f, "}");
    std::fprintf(f, "},\n");   // close series

    // ---- per-pass totals, for the drill-down table ----
    std::fprintf(f, "  \"passes\":[");
    for (size_t i = 0; i < r.passStats.size(); i++) {
      const PassTimer::Stat& st = r.passStats[i];
      const double usPerFrame =
          r.samples.empty() ? 0.0
                            : (double)st.totalNs / 1000.0 / (double)r.samples.size();
      // The node each pass bills to, resolved HERE rather than re-derived by the
      // page. perfnodes.h is the authority; a second mapping in JavaScript is
      // the drift this whole file exists to avoid.
      const int nodeIdx = PerfNodeForPass(st.name.c_str());
      std::fprintf(f, "%s{\"name\":%s,\"node\":%s,\"usPerFrame\":%s,"
                      "\"samples\":%llu}",
                   i ? "," : "", JStr(st.name).c_str(),
                   nodeIdx >= 0 ? JStr(kPerfNodes[nodeIdx].node).c_str() : "null",
                   JNum(usPerFrame).c_str(), (unsigned long long)st.samples);
    }
    std::fprintf(f, "],\n");

    // ---- what did not get attributed ----
    std::fprintf(f, "  \"unattributed\":{\"ns\":%llu,\"names\":[",
                 (unsigned long long)r.unattributedNs);
    for (size_t i = 0; i < r.unattributedNames.size(); i++)
      std::fprintf(f, "%s%s", i ? "," : "",
                   JStr(r.unattributedNames[i]).c_str());
    std::fprintf(f, "]}\n}");
  }
  std::fprintf(f, "\n]\n}\n");
  std::fclose(f);
  return true;
}

}  // namespace

int RunPerf(GpuContext& ctx, World& world, Simulation& sim,
            const std::vector<MaterialDef>& mats, const PerfOptions& opt) {
  if (opt.list) {
    std::printf("=== --perf scenarios ===\n");
    for (const Scenario& s : kScenarios)
      std::printf("  %-12s %-24s %u ticks (%.1f s of sim)\n", s.id, s.label,
                  s.ticks, (double)s.ticks / 30.0);
    std::printf("\nrun one:  sandvox --perf --scenario <id>\n");
    return 0;
  }

  // Turn on the spans SubmitTick and the page table bill themselves with. Off
  // by default, so a selftest gate that shares SubmitTick pays a branch.
  PerfScopesEnable(true);

  std::printf("=== sandvox --perf: engine performance suite ===\n");
  std::printf("adapter: %s\n", ctx.DeviceName().c_str());
  std::printf("timestamps: %s   residency: %s   render: %ux%u\n",
              ctx.timestampsEnabled ? "yes" : "NO (GPU bars will be empty)",
              world.residency == World::Residency::Paged ? "paged" : "dense",
              opt.width, opt.height);

  Stream stream;
  stream.Init(&ctx, &world, &sim, kDefaultSeed);
  stream.OnMaterialsReloaded(mats);

  // ---- the timer-neutrality gate -----------------------------------------
  //
  // Every number on the performance page is measured with GPU timestamps
  // attached to passes that the game runs without them. The claim that this is
  // free — a timestamp observes a dispatch, it does not reorder or gate one —
  // is exactly the kind of claim that is true until it is not, and the failure
  // mode is a world hash that moves only in the profiled build.
  //
  // So it is CHECKED, here, before any measurement is taken: worldgen twice,
  // 60 identical ticks each, once with the timer detached and once with it
  // attached at ROW granularity (the finer of the two, so it brackets more
  // dispatches than --measure ever does). Two worldgens and 120 ticks is a few
  // seconds, and it turns a paragraph of reasoning into a gate.
  {
    auto hashAfter60 = [&](PassTimer* t) {
      sim.SetPassTimer(t);
      SubmitWorldgen(ctx, world, sim, kDefaultSeed);
      ctx.WaitIdle();
      for (uint32_t k = 1; k <= 60; k++)
        SubmitTick(ctx, world, sim, k, kDefaultSeed, {}, {}, {}, k % 15 == 0,
                   {8, 3, 8}, false, false);
      ctx.WaitIdle();
      const uint32_t h = HashWorldNow(ctx, world, sim, kDefaultSeed);
      ctx.WaitIdle();
      sim.SetPassTimer(nullptr);
      return h;
    };
    const uint32_t hOff = hashAfter60(nullptr);
    PassTimer probe;
    uint32_t hOn = hOff;
    if (probe.Init(ctx, 192)) {
      probe.SetRowGranularity(true);
      hOn = hashAfter60(&probe);
      probe.ResetStats();
    }
    std::printf("timer neutrality: %08x untimed vs %08x timed — %s\n", hOff, hOn,
                hOff == hOn ? "IDENTICAL" : "*** DIVERGED ***");
    if (hOff != hOn) {
      std::fprintf(stderr,
                   "--perf: attaching the pass timer moved the world hash. Every "
                   "number this harness would print describes a different "
                   "simulation from the one the game runs. Refusing.\n");
      return 1;
    }
  }

  // No worldgen here: PerfRunner::Record regenerates and re-settles the world
  // per scenario, so any subset produces the same numbers as the full run.
  PerfRunner runner(ctx, world, sim, stream, mats, opt);
  if (!runner.Init()) {
    std::fprintf(stderr, "--perf: could not create the offscreen target\n");
    return 1;
  }

  std::vector<Run> runs;
  for (const Scenario& sc : kScenarios) {
    if (!opt.only.empty() && opt.only != sc.id) continue;
    std::printf("\n[%s] %s\n", sc.id, sc.label);
    runs.push_back(runner.Record(sc));
  }
  if (runs.empty()) {
    std::fprintf(stderr, "--perf: no scenario matched '%s'\n", opt.only.c_str());
    return 1;
  }

  sim.SetPassTimer(nullptr);
  if (!WriteJson(opt.out, runs, opt, ctx, world, runner.HaveTimer())) return 1;
  std::printf("\nwrote %s (%zu scenario%s)\n", opt.out.c_str(), runs.size(),
              runs.size() == 1 ? "" : "s");
  std::printf("open the tuner's Performance tab to read it.\n");
  return ctx.ReportVkValidation("--perf") > 0 ? 1 : 0;
}

}  // namespace sandvox
