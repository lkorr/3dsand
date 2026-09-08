// perfsuite.cpp — `--perf`. See perfsuite.h for what this harness is for and
// how it differs from --measure and --selftest.

#include "measure/perfsuite.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
#include "sim/microvox.h"
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

  // surface-sprint: ticks flown so far. The pose is an exact function of this
  // counter (never of frame time), which is what keeps a REAL-TIME PACED
  // scenario reproducible: the number of frames varies with the machine, the
  // sequence of world states does not. Owned by the driver, not by Record,
  // because warm-up ticks have to fly too — a scenario whose warm-up stands
  // still spends its first recorded frames measuring the transient of setting
  // off, which is not the thing under test.
  uint32_t flightTick = 0;

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

  // ---- REAL-TIME PACING (P3-F) --------------------------------------------
  // Default false: one iteration of the record loop is one sim tick and one
  // rendered frame, which is what every scenario before `surface-sprint`
  // wanted. A frame that is always exactly one tick cannot reproduce the
  // engine's own worst feedback loop, though — main.cpp runs a fixed-dt
  // accumulator and owes up to World::kMaxTicksPerFrame ticks to a frame that
  // ran long, so a slow frame does four ticks' worth of CA, page fills and
  // window shifts and gets slower still. With this set, one iteration is a
  // FRAME, `ticks` counts frames, and the ticks inside it come from a real
  // wall-clock accumulator exactly as the game's do.
  //
  // The cost is that the tick count of a run is machine-dependent. It is paid
  // rather than avoided because the alternative measures a loop the game does
  // not have; the WORLD stays reproducible because every paced driver derives
  // its pose from the tick counter and never from dt.
  bool paced = false;
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

// ---------------------------------------------------------------------------
// SCENARIO: surface-sprint
//
// `--frames 600 --autofly-surface` as a recorded, attributed scenario.
//
// WHY IT EXISTS. `flythrough` descends diagonally at 1.5 voxels/tick and runs
// one tick per frame; measured on this tree it is raymarch-bound at ~14 ms and
// never produces a frame over ~25 ms. The frame-time TAIL the streaming work is
// chasing — p95 82 ms, p99 113 ms, 11-13 frames over 100 ms — only appears
// under the game's own sprint flight, which is SEVEN TIMES faster (10.6
// voxels/tick, 0.67 chunks/tick, a window shift every ~1.5 ticks) and which
// runs a real accumulator, so a long frame owes up to four ticks and pays for
// all of them before it presents. Neither of those is a property of the
// terrain; both are properties of the DRIVER, so the driver is what this
// scenario copies.
//
// WHAT IS COPIED, AND FROM WHERE. src/main.cpp's `--autofly-surface`:
//   * fly mode, forward held, sprint held  (main.cpp g_autofly block, ~4672)
//   * speed = player.flySprint / kVoxelMeters, along Camera::Forward() at the
//     camera's own defaults yaw 2.35 / pitch -0.2 (game/camera.h, player.cpp's
//     fly branch) — a near-diagonal heading, so both horizontal axes shift.
//   * altitude PINNED ANALYTICALLY to World::TerrainHeight + 35 or + 150
//     voxels, alternating on `tick / 90 & 1`  (main.cpp ~4713 and ~5147).
//     Held rather than flown for the reason main.cpp gives at length: the
//     quantity under test is a function of height above terrain, and a climb
//     driven by an input axis wanders with frame time.
// The two clearance constants are restated here and compared against main.cpp
// by scripts/check_invariants.py, because two places that must agree is
// exactly what that script is for.
//
// WHAT IS NOT COPIED: main.cpp advances the position once per FRAME by the real
// dt; this advances once per TICK by kTickDt. Same distance per unit of sim
// time, and it makes the path a pure function of the tick counter instead of
// the frame rate — which is the only reason a paced scenario can still be
// compared run to run.
constexpr float kAutoflySurfaceLowVox = 35.0f;    // main.cpp kAutoflySurfaceLowVox
constexpr float kAutoflySurfaceHighVox = 150.0f;  // main.cpp kAutoflySurfaceHighVox

bool SetupSurfaceSprint(Scene& s, std::string&) {
  const IVec3 org = s.world.WindowOrigin();
  const int cx = (org.x + (int)kNChunk / 2) * (int)kChunk;
  const int cz = (org.z + (int)kNChunk / 2) * (int)kChunk;
  // The GAME'S default look direction, not a scenario-chosen one: the heading
  // decides how often the window shifts and on how many axes, so borrowing the
  // camera's own defaults is the difference between reproducing --autofly-
  // surface and reproducing something that merely resembles it.
  s.cam.yaw = 2.35f;
  s.cam.pitch = -0.2f;
  s.flightTick = 0;
  s.eye = {(float)cx,
           (float)(World::TerrainHeight(cx, cz, kDefaultSeed) +
                   (int)kAutoflySurfaceLowVox),
           (float)cz};
  char note[256];
  std::snprintf(note, sizeof note,
                "sprint flight over the surface at %.1f vox/tick (%.2f "
                "chunks/tick), altitude alternating +%d / +%d voxels above "
                "World::TerrainHeight on tick/90; REAL-TIME PACED, so a long "
                "frame owes up to %d ticks exactly as the game's does",
                CurrentTuning().player.flySprint / kVoxelMeters * kTickDt,
                CurrentTuning().player.flySprint / kVoxelMeters * kTickDt /
                    (float)kChunk,
                (int)kAutoflySurfaceLowVox, (int)kAutoflySurfaceHighVox,
                World::kMaxTicksPerFrame);
  s.note = note;
  return true;
}

void DriveSurfaceSprint(Scene& s, uint32_t, TickOps&) {
  // Per TICK, from the tick counter, never from dt — see the header note.
  const uint32_t t = s.flightTick++;
  const float speed = CurrentTuning().player.flySprint / kVoxelMeters;
  const float cp = std::cos(s.cam.pitch);
  s.eye.x += std::cos(s.cam.yaw) * cp * speed * kTickDt;
  s.eye.z += std::sin(s.cam.yaw) * cp * speed * kTickDt;
  // The altitude pin. Integer terrain height, exactly as main.cpp does it, so
  // the two harnesses fly the same line over the same heightfield.
  const bool high = ((t / 90u) & 1u) != 0u;
  const int gh = World::TerrainHeight((int)std::floor(s.eye.x),
                                      (int)std::floor(s.eye.z), kDefaultSeed);
  s.eye.y = (float)gh + (high ? kAutoflySurfaceHighVox : kAutoflySurfaceLowVox);
}

void VerifySurfaceSprint(Scene& s) {
  // A performance chart is insensitive to the fixture, and this fixture is
  // "did the window actually stream". A scenario that reports 600 beautiful
  // frames having shifted twice is measuring the wrong thing entirely, and
  // nothing else on the page would look wrong (the treeburn note says the same
  // about a fire that never took).
  const Stream::Timing& t = s.stream.Timings();
  char extra[224];
  std::snprintf(extra, sizeof extra,
                "  |  flew %u ticks, %u window shifts (%.2f/tick), %u wake-wait "
                "misses",
                s.flightTick, t.shifts,
                s.flightTick ? (double)t.shifts / (double)s.flightTick : 0.0,
                t.wakeWaits);
  s.note += extra;
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

    {"surface-sprint", "Surface sprint (the tail)",
     "The game's own --autofly-surface: sprint flight over the terrain at 10.6 "
     "voxels/tick with the altitude pinned analytically, REAL-TIME PACED so a "
     "long frame owes up to four ticks and pays for all of them. This is the "
     "only scenario that reproduces the streaming frame-time tail; flythrough "
     "runs one tick per frame at a seventh of the speed and is raymarch-bound.",
     "worldStorage;worldgen;pageTable;farField;caLoop;renderPass", 60, 600,
     SetupSurfaceSprint, DriveSurfaceSprint, VerifySurfaceSprint,
     /*paced=*/true},
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
      // The SAME timer for SubmitTick's three untabled spans (support.h): they
      // are recorded into the tick command buffer and resolved by the resolve
      // EncodeTick already puts at its tail, so they must share its query set.
      SetSubmitTickPassTimer(&timer_);
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
  // Scratch for the collector form of PollDeferred; a member so a 600-frame
  // run does not allocate a vector per poll.
  std::vector<PassTimer::PassFrame> harvest_;

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
  uint64_t prevJitterFills =
      world_.pages ? world_.pages->JitterFillsIssued() : (uint64_t)0;
  Stream::Timing prevStream = stream_.Timings();

  // ---- the real-time pacer (sc.paced only) --------------------------------
  // main.cpp's accumulator, verbatim in shape: fill from the real frame dt,
  // clamp at kMaxTicksPerFrame ticks' worth so a hitch cannot make the next
  // frame owe an unbounded backlog, then spend whole ticks. Every other
  // scenario keeps exactly one tick per iteration.
  //
  // ---- AND THE VSYNC FLOOR, WHICH IS NOT OPTIONAL -------------------------
  //
  // MEASURED, first attempt: without it, `surface-sprint` ran 133 sim ticks
  // over 600 frames (0.22/frame) where the game runs ~0.6. The harness renders
  // offscreen with no swapchain, so its frame is ~4.8 ms against the windowed
  // harness's 18.7 — and `dt` IS the frame time, so a cheap frame accrues a
  // fifth of a tick and the world barely advances. The run measured a scenario
  // that does not exist: 4 active chunks where `--autofly-surface` measures
  // ~550, and no tick backlog ever.
  //
  // This is the same hole FramePacer above was built to plug, one step further
  // out. FramePacer reproduces the swapchain's FRAMES-IN-FLIGHT bound because
  // there is no swapchain; this reproduces the swapchain's PRESENT INTERVAL for
  // the same reason. Under FIFO the game's frame cannot be shorter than the
  // refresh period no matter how little work it has, so its accumulator fills
  // at least one period per frame — and that, not the GPU cost, is what sets
  // the baseline ticks/frame that everything downstream (shift rate, active
  // chunks, materialize set) is a function of.
  //
  // A FLOOR, NOT A SLEEP. The harness must not actually wait: `wallMs` would
  // then be the floor plus jitter for every frame and the percentile the
  // scenario exists to report would be a constant. So the frame runs flat out
  // and only the accumulator is told that a refresh period elapsed. The
  // consequence is stated rather than hidden: `wallMs` here is the frame's WORK,
  // and the frame's WORK is what a hitch is made of; the vsync wait a real
  // client would also pay is `present`, which this harness already bills
  // separately through FramePacer.
  //
  // The rate is a knob because it is a cost question and a cost question
  // deserves a run rather than a rebuild (the SANDVOX_SNAP_MAXGAP argument in
  // support.cpp). 0 disables the floor entirely, which is the arm that produced
  // the 133-tick run above.
  static const double kPacedFloorSec = [] {
    double hz = 60.0;
    if (const char* e = std::getenv("SANDVOX_PERF_VSYNC_HZ")) {
      const double v = std::strtod(e, nullptr);
      if (v >= 0.0 && v <= 1000.0) {
        std::printf("[perf] SANDVOX_PERF_VSYNC_HZ=%.1f (default 60)\n", v);
        hz = v;
      }
    }
    return hz > 0.0 ? 1.0 / hz : 0.0;
  }();
  double accumulator = 0.0;
  double lastFrameEnd = NowSeconds();

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

    // HOW MANY TICKS THIS FRAME OWES. Unpaced: exactly one, forever, which is
    // what every scenario but surface-sprint was written against. Paced: the
    // game's own arithmetic, so a frame that ran 90 ms owes three ticks and the
    // GPU work of three ticks lands inside the next one.
    uint32_t ticksThisFrame = 1;
    if (sc.paced) {
      const double nowT = NowSeconds();
      // FIFO QUANTISES, it does not merely floor — and the difference is the
      // whole of hypothesis (b).
      //
      // Under VK_PRESENT_MODE_FIFO a frame is displayed on a refresh boundary,
      // so a frame whose work is 17 ms does not take 17 ms, it takes 33.3: the
      // client waits out the rest of the second period. The accumulator that
      // decides how many ticks the NEXT frame owes is filled with that 33.3, so
      // one frame that overruns the period by a millisecond buys the next one a
      // whole extra tick — which costs more GPU, which overruns by more. That
      // positive feedback is what turns a 21 ms GPU frame into an 82 ms one,
      // and a FLOOR cannot express it: floored, a 17 ms frame contributes 17 ms
      // and the loop has no gain at all. Measured with the floor: 322 ticks
      // over 600 frames, `ticksThisFrame` never once exceeded 1, p95 23 ms
      // against the windowed harness's 82. The loop was simply absent.
      //
      // So: round the elapsed time UP to the next whole refresh period, which
      // is what the swapchain this harness does not have would have done. It
      // stays a bookkeeping-only model — nothing sleeps, `wallMs` is still the
      // frame's real work — because a harness that actually waited would report
      // the refresh period as its own frame time and measure nothing.
      const double workSec = nowT - lastFrameEnd;
      accumulator += kPacedFloorSec > 0.0
                         ? std::ceil(workSec / kPacedFloorSec - 1e-9) *
                               kPacedFloorSec
                         : workSec;
      lastFrameEnd = nowT;
      const double cap = (double)World::kMaxTicksPerFrame * (double)kTickDt;
      if (accumulator > cap) accumulator = cap;
      ticksThisFrame = 0;
      while (accumulator >= (double)kTickDt &&
             ticksThisFrame < (uint32_t)World::kMaxTicksPerFrame) {
        accumulator -= (double)kTickDt;
        ticksThisFrame++;
      }
    }

    // THE DELTA BASELINES ARE RE-ARMED WHEN RECORDING STARTS, and skipping
    // this put the whole warm-up into frame 0: the first sample of the first
    // run read 37 window shifts, 37,888 chunks streamed, 4,810 page fills and
    // 120 ms of wake-wait in ONE frame, which is not a frame, it is the
    // warm-up wearing a frame's label. It also poisons every percentile: one
    // sample 100x the rest is the max, and on 600 samples it is inside the
    // p99.9.
    if (recording && i == sc.warmTicks) {
      if (world_.pages) {
        prevFills = world_.pages->FillsIssued();
        prevJitterFills = world_.pages->JitterFillsIssued();
      }
      prevStream = stream_.Timings();
    }

    uint32_t opsThisFrame = 0, cellOpsThisFrame = 0, expsThisFrame = 0;
    // The frame's first tick number, so the recorded sample can name the tick it
    // last ran rather than the one it is about to.
    const uint32_t frameFirstTick = tick;
    for (uint32_t sub = 0; sub < ticksThisFrame; sub++) {
      TickOps ops;
      {
        ScopeTimer sc1(fc, PerfScope::GameLogic);
        sc.drive(s, recording ? lt : 0, ops);
      }
      opsThisFrame += (uint32_t)ops.ops.size();
      cellOpsThisFrame += (uint32_t)ops.cells.size();
      expsThisFrame += (uint32_t)ops.exps.size();

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
      // ONE KickDeferred PER TIMED COMMAND BUFFER, all tagged with the FRAME.
      // Each SubmitTick resolves its own buffer into its own ring slot, so a
      // 4-tick frame arms four of them; PassTimer::kRing is sized for that.
      if (haveTimer_) timer_.KickDeferred(ctx_, i);

      tick++;
    }   // per-tick loop

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
      // The LAST tick this frame ran, not the next one it will. A paced frame
      // runs 0..kMaxTicksPerFrame ticks, so `tick` after the loop is not a tick
      // that happened; naming it here would put every paced sample one tick
      // ahead of its own numbers.
      smp.tick = tick > frameFirstTick ? tick - 1 : frameFirstTick;
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
      smp.counters[(int)PerfCounter::Ops] = (double)opsThisFrame;
      smp.counters[(int)PerfCounter::CellOps] = (double)cellOpsThisFrame;
      smp.counters[(int)PerfCounter::Explosions] = (double)expsThisFrame;
      smp.counters[(int)PerfCounter::TicksThisFrame] = (double)ticksThisFrame;
      smp.counters[(int)PerfCounter::PageFaults] =
          sn.valid ? (double)sn.pageFaults : 0.0;
      smp.counters[(int)PerfCounter::VoxelsNonAir] =
          sn.valid ? (double)sn.voxelTotal : 0.0;
      if (world_.pages) {
        smp.counters[(int)PerfCounter::PagesResident] =
            (double)world_.pages->PagesInUse();
        // THE TWO FILL HALVES, SEPARATELY. `pageFills` is the EMPTY/UNIFORM
        // half: one 16 KiB vkCmdFillBuffer each, so the count IS the command
        // count and that is the quantity hypothesis (a) is about. The JITTER
        // half is a single dispatch over however many slots, so its count is a
        // workgroup count and not a command count — adding them would produce a
        // number that is neither.
        const uint64_t f = world_.pages->FillsIssued();
        const uint64_t jf = world_.pages->JitterFillsIssued();
        smp.counters[(int)PerfCounter::PageFills] = (double)(f - prevFills);
        smp.counters[(int)PerfCounter::PageFillsJitter] =
            (double)(jf - prevJitterFills);
        smp.counters[(int)PerfCounter::PageFillBytes] =
            (double)((f - prevFills) + (jf - prevJitterFills)) *
            (double)kChunkVol * 4.0;
        prevFills = f;
        prevJitterFills = jf;
        smp.counters[(int)PerfCounter::CpuDirtyChunks] =
            (double)world_.pages->CpuDirty().Size();
      }
      // ---- the streaming shift, per frame -----------------------------------
      // Diffed from Stream's own cumulative Timing rather than re-timed here,
      // so the harness cannot disagree with the shift breakdown --frames
      // prints. `shiftCpuMs` is the four phases of ShiftAxis; `wakeWaitMs` is
      // the deferred wake's T+K poll BLOCKING because the readback it needs has
      // not landed, which is the one place a shift can still stall the frame.
      {
        const Stream::Timing& st = stream_.Timings();
        smp.counters[(int)PerfCounter::WindowShifts] =
            (double)(st.shifts - prevStream.shifts);
        smp.counters[(int)PerfCounter::ShiftWakeWaitMs] =
            st.wakeWaitMs - prevStream.wakeWaitMs;
        smp.counters[(int)PerfCounter::ShiftCpuMs] =
            (st.evictMs - prevStream.evictMs) +
            (st.fillStoreMs - prevStream.fillStoreMs) +
            (st.fillGenMs - prevStream.fillGenMs) +
            (st.demoteMs - prevStream.demoteMs);
        smp.counters[(int)PerfCounter::ChunksStreamed] =
            (double)(st.shifts - prevStream.shifts) * (double)(kNChunk * kNChunk);
        prevStream = st;
      }
      smp.counters[(int)PerfCounter::DrawCalls] = 3;   // world, particles, fluid
      smp.wallMs = (NowSeconds() - frameT0) * 1000.0;
      frameToSample_[i] = (int)r.samples.size();
      r.samples.push_back(smp);
    }

    // Harvest whatever GPU timings have landed and post them to their own rows.
    //
    // ONE map for both populations (PerfNodeForTimedName): a sample's name is
    // either a pass_table.def row or a hand-written span, and the caller has no
    // business knowing which — a bool at the call site is how a new span in the
    // tick buffer ends up silently unattributed.
    auto postFrame = [&](uint32_t tag, const std::vector<PassSample>& passes) {
      if (tag >= frameToSample_.size()) return;
      const int si = frameToSample_[tag];
      if (si < 0) return;   // a warmup frame: its numbers are not recorded
      PerfSample& dst = r.samples[(size_t)si];
      for (const PassSample& ps : passes) {
        const int node = PerfNodeForTimedName(ps.name);
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
    // THE COLLECTOR FORM, not LastFrame(). A paced frame submits up to four
    // timed command buffers and they can all retire in one poll; LastFrame()
    // keeps only the newest, so reading it here would have thrown away three
    // quarters of the GPU time of exactly the frames this scenario exists to
    // explain — and thrown it away SILENTLY, as a smaller number.
    if (haveTimer_) {
      harvest_.clear();
      if (timer_.PollDeferred(ctx_, &harvest_) > 0)
        for (const PassTimer::PassFrame& pf : harvest_)
          postFrame(pf.tag, pf.passes);
    }
    if (haveRenderTimer_) {
      harvest_.clear();
      if (renderTimer_.PollDeferred(ctx_, &harvest_) > 0)
        for (const PassTimer::PassFrame& pf : harvest_)
          postFrame(pf.tag, pf.passes);
    }
  }

  // Drain the last few frames of in-flight timestamps so the tail of the chart
  // is not a cliff of gpuValid=false rows.
  pacer_.Drain();
  ctx_.WaitIdle();
  for (int drain = 0; drain < 8; drain++) {
    ctx_.ProcessEvents();
    if (haveTimer_) {
      harvest_.clear();
      if (timer_.PollDeferred(ctx_, &harvest_) > 0) {
        for (const PassTimer::PassFrame& pf : harvest_) {
          if (pf.tag >= frameToSample_.size()) continue;
          if (frameToSample_[pf.tag] < 0) continue;
          PerfSample& dst = r.samples[(size_t)frameToSample_[pf.tag]];
          for (const PassSample& ps : pf.passes) {
            const int node = PerfNodeForTimedName(ps.name);
            if (node < 0) { r.unattributedNs += ps.ns; continue; }
            dst.gpuMs[node] += (double)ps.ns / 1e6;
            dst.gpuValid = true;
          }
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
  if (sc.paced) {
    // A paced run's tick count is machine-dependent (that is the whole point),
    // so it is REPORTED rather than assumed. Quoting a frame percentile without
    // it invites comparing two runs that did different amounts of work.
    double tk = 0, tkMax = 0;
    for (const PerfSample& smp : r.samples) {
      tk += smp.counters[(int)PerfCounter::TicksThisFrame];
      tkMax = std::max(tkMax, smp.counters[(int)PerfCounter::TicksThisFrame]);
    }
    std::printf("  %-12s PACED: %.0f sim ticks over %zu frames (%.2f/frame, "
                "max %.0f)\n",
                "", tk, r.samples.size(),
                r.samples.empty() ? 0.0 : tk / (double)r.samples.size(), tkMax);
  }
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

// The tick whose sun sits at a GIVEN elevation — the same scan, for the
// cameras that are not noon. No closed form, for the same reason FindNoonTick
// has none: cycleMinutes is a tuning value, so any constant written here
// becomes the wrong time of day the first time somebody moves it.
//
// PREFER THE DESCENDING SIDE. Every elevation below the peak is reached twice
// a day, once climbing and once falling, and for a render budget the two are
// not interchangeable: which side of the terrain is lit — and therefore which
// pixels pass `lambert > 0.0` and cast the shadow ray that is the largest row
// in this table — is opposite. A camera labelled `dusk` that measured dawn
// would be a quiet lie, so the scan keeps the best match found while the sun is
// FALLING and only falls back to the best match overall if the sun never
// descends through that elevation at all.
uint32_t FindTickAtElevation(const Tuning& tun, float sinElev) {
  uint32_t bestAny = 0, bestDesc = 0;
  float errAny = 1e9f, errDesc = 1e9f;
  bool haveDesc = false;
  float prev = ComputeSky(tun, 0.0).sunDir[1];
  for (uint32_t t = 64; t < 200000u; t += 64u) {
    const float up = ComputeSky(tun, (double)t).sunDir[1];
    const float err = std::fabs(up - sinElev);
    if (err < errAny) { errAny = err; bestAny = t; }
    if (up < prev && err < errDesc) {
      errDesc = err;
      bestDesc = t;
      haveDesc = true;
    }
    prev = up;
  }
  return haveDesc ? bestDesc : bestAny;
}

// Sun elevation for the `dusk` camera: 8 degrees above the horizon. Low enough
// that the terrain is raked and the shadows are long — the expensive lighting
// condition this camera exists to measure — and still above the 5-degree
// (`keyLightDir().y < 0.08`) gate that switches the key light off entirely, so
// the shadow ray is still being cast on the pixels that pay for it.
constexpr float kDuskSinElev = 0.1392f;  // sin(8 deg)

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
    // ---- the openness (sky-visibility) grid (docs/PLAN_gi.md §2) ----
    // THE A/B FOR THE WHOLE FEATURE, and it is an exact off switch on both
    // halves: render.opennessStrength = 0 const-folds opennessScale() to 1.0 in
    // the fragment shader AND makes C_OPENNESS false, so neither the dirty walk
    // nor the rolling refresh is recorded. baseline - noopenness is therefore
    // the pass plus the reads, in one number, on one world.
    {"noopenness", "openness grid off (pass unrecorded + reads const-folded)",
     [](Tuning& t) { t.render.opennessStrength = 0.0f; }, true, 1,
     "the whole openness grid: the compute pass that builds it AND the "
     "per-hit bilinear read in the raymarch"},
    // ---- one-bounce indirect light (docs/PLAN_gi.md §3) ----
    // giStrength = 0 const-folds the per-hit gather out of the raymarch, the
    // deposit out of the resolve pass and the sun sample out of the openness
    // walk, so baseline - nogi is the whole feature on one world.
    {"nogi", "one-bounce GI off (gather, injection and walk sample folded)",
     [](Tuning& t) { t.render.giStrength = 0.0f; }, true, 1,
     "the irradiance gather at every near-field hit plus both injection "
     "paths"},
    // ---- waterfall mist on opaque hits (Lin follow-ups T5.3) ----
    // mistDensity = 0 folds the liquid-pixel veil AND the three-cell probe
    // every opaque near-field pixel now makes toward a neighbouring fall.
    {"nomist", "waterfall mist off (mistDensity 0, folds the opaque-hit probe)",
     [](Tuning& t) { t.render.mistDensity = 0.0f; t.render.sprayDensity = 0.0f; },
     true, 1,
     "the mist veil on liquid pixels plus the three-voxel probe on every "
     "opaque near-field pixel"},
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
// The Rm* block of PerfCounter is the shader's slot table, one enumerator per
// kRenderStatSlots in RS_* order; main.cpp's slot->counter copy and the stats
// print below both index it that way.
static_assert((int)PerfCounter::Count - (int)PerfCounter::RmPixels ==
                  (int)kRenderStatSlots,
              "PerfCounter::Rm* must have exactly kRenderStatSlots rows");

// ---- arms that only one camera runs ---------------------------------------
// Deliberately NOT rows of kRenderArms. Two reasons, and the second is the one
// that matters:
//
//   1. The `noon` table stays exactly the 16 rows it has always had, so every
//      number in this repo's history stays comparable row for row.
//   2. God rays are shading INSIDE a liquid (`shadeSubmerged`, gated on
//      `h.liqT < 0.05`). On the overlook — on any camera in air — not one god
//      ray is cast, so both of these arms would land on the baseline and print
//      two 0.00 ms rows that read exactly like a measured result saying the
//      feature is free. An arm that cannot fire on a camera does not belong in
//      that camera's table.
const RenderArm kExtraArms[] = {
    {"nogodray", "godRaySteps 14 -> 0",
     [](Tuning& t) { t.render.godRaySteps = 0; }, true, 1,
     "the whole underwater god-ray march — 14 steps, each with its own shadow "
     "ray and its own waterAbove walk"},
    {"godshadow0", "godRayShadowSteps 20 -> 0",
     [](Tuning& t) { t.render.godRayShadowSteps = 0; }, true, 1,
     "ONLY the shadow ray cast at each god-ray step; the shafts still march, "
     "so nogodray minus this is the marching itself"},
    // ---- the foliage ceilings (meadow / canopy cameras) ----------------
    // Each is a CEILING, not a proposal: it deletes or caps one whole term
    // so the delta from baseline bounds what any optimisation of that term
    // could ever recover. `nomicro` (kRenderArms) is the fourth of the set —
    // microMaxPerRay 0 const-folds the plant/brick branch out of trace()
    // entirely, so it is the ceiling on everything plant-related.
    {"micro1", "microMaxPerRay 8 -> 1",
     [](Tuning& t) { t.render.microMaxPerRay = 1; }, true, 1,
     "every plant/brick evaluation past the FIRST cell a ray enters — the "
     "grazing-ray cost of a meadow"},
    {"plantlod4", "plantLodDist 16 -> 4 m",
     [](Tuning& t) { t.render.plantLodDist = 4.0f; }, true, 1,
     "column-plant evaluations between 4 m and 16 m (past the cut a plant "
     "cell is a solid cube)"},
    {"fine2m", "lodHandoffDist 26 -> 2 m",
     [](Tuning& t) { t.render.lodHandoffDist = 2.0f; }, true, 1,
     "the WHOLE in-window fine march past 2 m — everything the cascade "
     "could stand in for, plants included"},
};
constexpr int kExtraArmCount =
    (int)(sizeof(kExtraArms) / sizeof(kExtraArms[0]));

const RenderArm* FindArm(const char* id) {
  for (const RenderArm& a : kRenderArms)
    if (std::strcmp(a.id, id) == 0) return &a;
  for (const RenderArm& a : kExtraArms)
    if (std::strcmp(a.id, id) == 0) return &a;
  return nullptr;
}

// ---------------------------------------------------------------------------
// THE CAMERAS.
//
// One camera was one camera too few. Every row of the arm table is a property
// of ONE PICTURE, and the picture the budget was taken from — noon, on a hill,
// looking out — is the cheapest lighting condition the game has: the sun is
// overhead so shadow rays are short, nothing is submerged so no god ray, no
// caustic and no Snell's window is ever evaluated, and the frame is a third
// sky. Optimising against that budget optimises the easy case.
//
// So three: the original (kept byte-for-byte, because its history is the only
// baseline that exists), a low sun, and an eye INSIDE the lake.
//
// WHY THE EXTRA CAMERAS RUN FEWER ARMS. A full 16-arm pass is 16 shader
// reloads plus 16 x 60 frames, and most of those rows answer a question that
// does not change with the camera (shadow0 vs noshadow is a footprint claim
// about the compiled shader, not about the view). The five kept — baseline,
// noshadow, nofar, noreflect, halfres — are the ones whose answer is a
// property of the PICTURE, which is exactly what a second picture is for.
// ---------------------------------------------------------------------------
struct BudgetCam {
  const char* id;
  const char* label;
  // Fills eye/cam/note and the celestial tick the sky is derived from.
  // false + `why` = this camera cannot be set up here; the run says so and
  // carries on with the others rather than failing the whole table.
  bool (*setup)(Scene& s, uint32_t& tick, std::string& why);
  // nullptr-terminated list of arm ids; nullptr = every row of kRenderArms.
  const char* const* arms;
};

// THE OVERLOOK. High enough to clear the terrain it stands on, pitched down far
// enough that the frame is roughly a third near ground, a third middle distance
// and a third horizon-and-sky. That mix is the point: it exercises the fine
// march, the in-window LOD handoff, the far cascade and the sky path in one
// frame, in something like the proportion a player standing on a hillside sees.
// A camera that sees only one of those reports a budget for one of those.
//
// These five numbers are FROZEN. They are the camera every --render-budget
// number ever recorded was taken from; changing them does not improve the
// camera, it deletes the comparison.
void OverlookEye(Scene& s) {
  const int gx = 256, gz = 256;
  const int h = World::TerrainHeight(gx, gz, kDefaultSeed);
  s.eye = {(float)gx, (float)(h + 120), (float)gz};
  s.cam.yaw = 0.785f;
  s.cam.pitch = -0.32f;
}

bool CamNoon(Scene& s, uint32_t& tick, std::string& why) {
  (void)why;
  OverlookEye(s);
  s.note = "overlook: 12 m up, pitched into the middle distance";
  tick = FindNoonTick(CurrentTuning());
  return true;
}

bool CamDusk(Scene& s, uint32_t& tick, std::string& why) {
  (void)why;
  OverlookEye(s);
  s.note = "the SAME overlook, sun ~8 deg up: long shadow rays, raked terrain";
  tick = FindTickAtElevation(CurrentTuning(), kDuskSinElev);
  return true;
}

// THE CASCADE CAMERA — the far field's own budget, and it did not exist until
// 2026-09-07 even though the far march is the largest single term in a flight
// frame. Every camera above looks DOWN from moderate height, so nearly every
// pixel lands inside the residency window: measured on the overlook, the
// cascade resolves ZERO pixels (rmPxFar 0.000) while still costing 1.77 ms of a
// 14.06 ms frame. That is a real and quotable number — it is the price of
// SEARCHING empty air — but it can never report what the cascade costs when it
// is actually DRAWING, so no arm of any far-field change could be judged here.
//
// The numbers are `screenshot_cascade`'s (main.cpp), deliberately, so the frame
// this times is the frame the look review looks at. Both are load-bearing and
// the comment there says why: the eye must be well ABOVE the terrain, because
// the residency window is only half a window edge in radius (25.6 m), and the
// pitch must be near-horizontal or the frame fills with the window again.
// ~50% of its pixels come from traceFar.
bool CamCascade(Scene& s, uint32_t& tick, std::string& why) {
  (void)why;
  const int h108 = World::TerrainHeight(108, 108, kDefaultSeed);
  s.eye = {108.0f, (float)(h108 + 300), 108.0f};
  s.cam.yaw = 0.785f;
  s.cam.pitch = -0.06f;
  s.note = "300 voxels over the terrain, near-horizontal — the far cascades "
           "ARE this frame (~50% of its pixels)";
  tick = FindNoonTick(CurrentTuning());
  return true;
}

// INSIDE the lake. `shadeSubmerged` — god rays, silt, the caustic web on the
// bed, Snell's window at the underside of the surface — is reached by exactly
// one predicate in the whole renderer (`raymarch.wgsl`, `h.liqT < 0.05`), and
// the only way to reach it is to put the eye in the liquid. There is no
// RenderParams medium field to set and nothing to toggle: the camera position
// IS the switch.
//
// The pool must be INSIDE THE RESIDENCY WINDOW. Outside it a liquid shades
// through the far-field cascade as flat colour with no submerged path at all
// (main.cpp's oil shots relocate the window for exactly this reason), so the
// budget would be of a grey slab. Hence the CellInWindow test, and hence the
// authored lake rather than a generated tarn: pondInfo() refuses to place a
// tarn anywhere in -128..640, which is precisely the window this harness runs.
bool CamSubmerged(Scene& s, uint32_t& tick, std::string& why) {
  World::AuthoredPool pools[World::kAuthoredPools];
  World::AuthoredPoolList(pools);
  for (const World::AuthoredPool& p : pools) {
    if (std::strcmp(p.mat, "water") != 0) continue;
    // Mid-column: bed below, surface above, both inside the submerged path's
    // reach. An eye near the floor sees no Snell window; one near the surface
    // sees no caustics.
    const int ey = (p.floorY + p.waterY) / 2;
    if (!s.world.CellInWindow({p.cx, ey, p.cz})) continue;
    s.eye = {(float)p.cx, (float)ey, (float)p.cz};
    s.cam.yaw = 0.0f;    // +x — Camera::Forward is (cos yaw, sin pitch, sin yaw)
    s.cam.pitch = 0.0f;  // level: the far bank ahead, bed below, surface above
    char note[256];
    std::snprintf(note, sizeof note,
                  "INSIDE the authored lake: disc r=%d at (%d,%d), floor y=%d, "
                  "surface y=%d, eye y=%d — %d voxels of water overhead, "
                  "looking level toward the +x shore",
                  p.r, p.cx, p.cz, p.floorY, p.waterY, ey, p.waterY - ey);
    s.note = note;
    tick = FindNoonTick(CurrentTuning());  // god rays need the sun up
    return true;
  }
  why = "no authored water pool with its mid-column inside the residency "
        "window. There is no generated-pond fallback on purpose: pondInfo() "
        "excludes tarns from the -128..640 authored origin region this harness "
        "runs in, and PondDisc carries no floor height, so a fallback here "
        "could neither fire nor pick a depth";
  return false;
}

// ---------------------------------------------------------------------------
// THE FOLIAGE CAMERAS (2026-09-05).
//
// The owner's report: the GPU collapses standing in tall grass and looking
// through a tree canopy. Neither overlook sees that — from 12 m up a meadow is
// a few hundred plant cells at 20 m, and no ray of the overlook ever crosses
// a crown. So two cameras that are FOUND, not authored: the world is read
// back after it settles, every column scored by how much foliage stands
// around it, and the eye put where the score is highest. Procedural because
// the seed, the biome files and the tree bakes all move the meadows around;
// an authored coordinate would be measuring bare dirt within a month.
//
// The scan reads the whole residency window's surface band once (rows of 32
// chunk slots; sentinels are synthesised CPU-side by ReadVoxelsSync, so sky
// costs nothing) and both cameras share it through the cache below, reset per
// RunRenderBudget so it can never describe a previous world.
// ---------------------------------------------------------------------------
struct FoliageScan {
  bool valid = false;
  int x0 = 0, z0 = 0;   // world voxel origin of the maps below
  int span = 0;         // kNChunk * kChunk on each axis
  // Per (x,z) column of the window.
  std::vector<uint16_t> plantAt;   // cells whose material has a micro.plant block
  std::vector<uint16_t> leafAt;    // cells tagged `foliage`
  std::vector<int16_t> groundY;    // highest cell that is neither, nor air
  std::vector<int16_t> leafLo, leafHi;
  // Summed-area tables, (span+1)^2, so a box score is four reads.
  std::vector<uint32_t> plantSat, leafSat;
  uint64_t plantTotal = 0, leafTotal = 0;
  uint32_t plantMats = 0, leafMats = 0;
  uint32_t chunksRead = 0;
  double seconds = 0;
  // Box score over a column map: cells within +-r of (ix, iz) inclusive,
  // clipped to the maps.
  uint32_t Box(const std::vector<uint32_t>& sat, int ix, int iz, int r) const {
    const int w = span + 1;
    const int xa = std::max(ix - r, 0), za = std::max(iz - r, 0);
    const int xb = std::min(ix + r + 1, span), zb = std::min(iz + r + 1, span);
    if (xa >= xb || za >= zb) return 0;
    return sat[(size_t)zb * w + xb] - sat[(size_t)za * w + xb] -
           sat[(size_t)zb * w + xa] + sat[(size_t)za * w + xa];
  }
  // Highest-scoring column whose box lies fully inside the maps, or -1.
  int Best(const std::vector<uint32_t>& sat, int r, uint32_t& score) const {
    int best = -1;
    score = 0;
    for (int iz = r; iz < span - r; iz++)
      for (int ix = r; ix < span - r; ix++) {
        const uint32_t v = Box(sat, ix, iz, r);
        if (v > score) { score = v; best = iz * span + ix; }
      }
    return best;
  }
};
FoliageScan g_foliage;

void BuildSat(const std::vector<uint16_t>& src, int span,
              std::vector<uint32_t>& sat) {
  const int w = span + 1;
  sat.assign((size_t)w * w, 0);
  for (int z = 0; z < span; z++) {
    uint32_t row = 0;
    for (int x = 0; x < span; x++) {
      row += src[(size_t)z * span + x];
      sat[(size_t)(z + 1) * w + (x + 1)] = sat[(size_t)z * w + (x + 1)] + row;
    }
  }
}

const FoliageScan& ScanFoliage(GpuContext& ctx, World& world,
                               const std::vector<MaterialDef>& mats) {
  FoliageScan& f = g_foliage;
  if (f.valid) return f;
  const auto t0 = std::chrono::steady_clock::now();
  // WHICH MATERIALS ARE PLANTS: the micro loader is the one place that knows
  // which "micro" blocks resolved to an analytic plant (kMicroPlant), so ask
  // it — on a copy of the table, since it sets MATF_MICRO on what it is given.
  std::vector<uint8_t> isPlant(mats.size(), 0), isLeaf(mats.size(), 0);
  {
    std::vector<MaterialDef> copy = mats;
    MicroSet ms;
    std::string log;
    LoadMicroVox(AssetDir() + "/materials/materials.json", AssetDir(), copy, ms,
                 log);
    for (size_t i = 0; i < ms.table.size() && i < mats.size(); i++)
      if (ms.table[i].base != kMicroNoBrick &&
          (ms.table[i].flags & kMicroPlant) != 0) {
        isPlant[i] = 1;
        f.plantMats++;
      }
  }
  for (uint32_t i = 0; i < mats.size(); i++)
    if (MatHasTag(mats, i, "foliage")) { isLeaf[i] = 1; f.leafMats++; }

  const IVec3 org = world.WindowOrigin();
  f.span = (int)(kNChunk * kChunk);
  f.x0 = org.x * (int)kChunk;
  f.z0 = org.z * (int)kChunk;
  const size_t n = (size_t)f.span * f.span;
  f.plantAt.assign(n, 0);
  f.leafAt.assign(n, 0);
  f.groundY.assign(n, INT16_MIN);
  f.leafLo.assign(n, INT16_MAX);
  f.leafHi.assign(n, INT16_MIN);

  // One read per (y, z) chunk row: the 32 x-slots of a row are contiguous in
  // slot space (SlotChunkIndex is x-fastest), so a row is one range and
  // ReadVoxelsSync splits it into resident runs itself. The y band per row is
  // anchored to World::TerrainHeight across the row — from below the lowest
  // ground to above the tallest crown FindTallestTrunk allows for.
  std::vector<uint32_t> row((size_t)kNChunk * kChunkVol);
  for (int cz = org.z; cz < org.z + (int)kNChunk; cz++) {
    int gmin = INT_MAX, gmax = INT_MIN;
    for (int cx = org.x; cx < org.x + (int)kNChunk; cx++) {
      const int h = World::TerrainHeight(cx * (int)kChunk + 8,
                                         cz * (int)kChunk + 8, kDefaultSeed);
      gmin = std::min(gmin, h);
      gmax = std::max(gmax, h);
    }
    const int cyLo = std::max(org.y, (gmin - 32) / (int)kChunk);
    const int cyHi = std::min(org.y + (int)kNChunk - 1,
                              (gmax + 224) / (int)kChunk);
    for (int cy = cyLo; cy <= cyHi; cy++) {
      const uint32_t first =
          World::SlotChunkIndex({0, cy, cz}) & ~(kNChunk - 1);
      ReadVoxelsSync(ctx, world, first, kNChunk, row.data(), "budgetFoliage");
      f.chunksRead += kNChunk;
      for (uint32_t sx = 0; sx < kNChunk; sx++) {
        const IVec3 wc = world.SlotToWorldChunk(first + sx);
        const uint32_t* c = row.data() + (size_t)sx * kChunkVol;
        for (uint32_t li = 0; li < kChunkVol; li++) {
          const uint32_t m = c[li] & 0xFFFu;
          if (m == 0 || m >= mats.size()) continue;
          const int lx = (int)(li % kChunk), ly = (int)((li / kChunk) % kChunk),
                    lz = (int)(li / (kChunk * kChunk));
          const int wx = wc.x * (int)kChunk + lx, wy = wc.y * (int)kChunk + ly,
                    wz = wc.z * (int)kChunk + lz;
          const int ix = wx - f.x0, iz = wz - f.z0;
          if (ix < 0 || iz < 0 || ix >= f.span || iz >= f.span) continue;
          const size_t ci = (size_t)iz * f.span + ix;
          if (isPlant[m]) {
            f.plantAt[ci]++;
          } else if (isLeaf[m]) {
            f.leafAt[ci]++;
            f.leafLo[ci] = (int16_t)std::min((int)f.leafLo[ci], wy);
            f.leafHi[ci] = (int16_t)std::max((int)f.leafHi[ci], wy);
          } else {
            f.groundY[ci] = (int16_t)std::max((int)f.groundY[ci], wy);
          }
        }
      }
    }
  }
  for (size_t i = 0; i < n; i++) {
    f.plantTotal += f.plantAt[i];
    f.leafTotal += f.leafAt[i];
  }
  BuildSat(f.plantAt, f.span, f.plantSat);
  BuildSat(f.leafAt, f.span, f.leafSat);
  f.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                            t0).count();
  f.valid = true;
  std::printf("  foliage scan: %u chunks read in %.1f s — %llu plant cells "
              "(%u plant materials), %llu leaf cells (%u foliage materials) "
              "in the window\n",
              f.chunksRead, f.seconds, (unsigned long long)f.plantTotal,
              f.plantMats, (unsigned long long)f.leafTotal, f.leafMats);
  return f;
}

// Is a world cell open to an EYE — air, or a plant cell (passable, and an eye
// in a tuft is the case the meadow camera exists for)? Reads the scan's
// column classification rather than the GPU; a column the scan never saw is
// treated as open. Conservative about crowns: only the leaf SPAN per column
// is kept, so any cell inside it counts as closed.
bool EyeCellOpen(const FoliageScan& f, int wx, int wy, int wz) {
  const int ix = wx - f.x0, iz = wz - f.z0;
  if (ix < 0 || iz < 0 || ix >= f.span || iz >= f.span) return true;
  const size_t ci = (size_t)iz * f.span + ix;
  if (wy <= f.groundY[ci]) return false;
  if (f.leafAt[ci] > 0 && wy >= f.leafLo[ci] && wy <= f.leafHi[ci]) return false;
  return true;
}

// ATTRIBUTION for a chosen site (CLAUDE.md rule 6): what the page table and
// the occupancy table say about the chunk under a cell, and what the voxels
// there actually are. Printed for every foliage eye so a frame that renders
// nothing near-field names the chunk that refused, instead of leaving a bare
// "0 near hits" to be chased one hypothesis per run.
void ProbeCell(GpuContext& ctx, World& world,
               const std::vector<MaterialDef>& mats, IVec3 c,
               const std::vector<uint32_t>& occ, const char* label) {
  const IVec3 wc{c.x >> 4, c.y >> 4, c.z >> 4};
  if (!world.ChunkInWindow(wc)) {
    std::printf("    probe %-12s cell (%d,%d,%d): chunk (%d,%d,%d) is OUTSIDE "
                "the window\n", label, c.x, c.y, c.z, wc.x, wc.y, wc.z);
    return;
  }
  const uint32_t slot = World::SlotChunkIndex(wc);
  const uint32_t e = world.PageEntryOfSlot(slot);
  char pt[64];
  if ((e & kPtSentinelBit) == 0) std::snprintf(pt, sizeof pt, "page %u", e);
  else if (e == kPtEmpty) std::snprintf(pt, sizeof pt, "EMPTY");
  else {
    const uint32_t m = e & 0xFFFu;
    std::snprintf(pt, sizeof pt, "%s(%s)", (e & kPtJitterBit) ? "JITTER" : "UNIFORM",
                  m < mats.size() ? mats[m].name.c_str() : "?");
  }
  std::vector<uint32_t> chunk(kChunkVol);
  ReadVoxelsSync(ctx, world, slot, 1, chunk.data(), "budgetProbe");
  uint32_t nonAir = 0;
  for (uint32_t w : chunk) nonAir += (w & 0xFFFu) != 0;
  const uint32_t here = chunk[World::SlotCellIndex(c) - slot * kChunkVol] & 0xFFFu;
  std::printf("    probe %-12s cell (%d,%d,%d) = %s | chunk (%d,%d,%d) slot %u: "
              "page table %s, occupancy nonAir %u rayBlockers %u, readback "
              "nonAir %u\n",
              label, c.x, c.y, c.z,
              here < mats.size() ? mats[here].name.c_str() : "?", wc.x, wc.y,
              wc.z, slot, pt, occ[slot] & 0xFFFFu, occ[slot] >> 16, nonAir);
}

// Scoring radii, in voxels. Meadow 8 m: the frame is grass at 0-20 m, and 8 m
// is inside the band where a column plant is still TRACED rather than cubed
// (render.plantLodDist 16 m), so the densest 8 m box is the densest thing the
// plant march ever sees. Canopy 6 m: about one crown. Boxes, not discs — four
// summed-area reads each.
constexpr int kMeadowRadius = 80;
constexpr int kCanopyRadius = 60;

bool CamMeadow(Scene& s, uint32_t& tick, std::string& why) {
  const FoliageScan& f = ScanFoliage(s.ctx, s.world, s.mats);
  if (f.plantMats == 0) {
    why = "no material has a micro.plant block, so there is nothing to stand "
          "in";
    return false;
  }
  uint32_t score = 0;
  const int ci = f.Best(f.plantSat, kMeadowRadius, score);
  if (ci < 0 || score == 0) {
    why = "no plant cells in the residency window (debug.vegetation off, "
          "or no cover in this biome)";
    return false;
  }
  const int ix = ci % f.span, iz = ci / f.span;
  const int wx = f.x0 + ix, wz = f.z0 + iz;
  int ground = f.groundY[(size_t)ci];
  if (ground == INT16_MIN) ground = World::TerrainHeight(wx, wz, kDefaultSeed);
  // Standing height: 1.6 m over the ground under the eye, INSIDE the grass.
  int ey = ground + 16;
  while (!EyeCellOpen(f, wx, ey, wz) && ey < ground + 40) ey++;
  s.eye = {(float)wx + 0.5f, (float)ey + 0.5f, (float)wz + 0.5f};
  s.cam.yaw = 0.785f;
  s.cam.pitch = -0.15f;
  const uint32_t near = f.Box(f.plantSat, ix, iz, 20);
  char note[320];
  std::snprintf(note, sizeof note,
                "MEADOW: eye %d voxels over ground y=%d in the densest plant "
                "patch of the window — %u plant cells within %d m of (%d,%d) "
                "(%u within 2 m) of %llu in the window; pitched slightly down "
                "so the frame is grass at 0-20 m",
                ey - ground, ground, score, kMeadowRadius / 10, wx, wz, near,
                (unsigned long long)f.plantTotal);
  s.note = note;
  std::printf("  meadow site: eye (%d, %d, %d) — %u plant cells within %d m, "
              "%u within 2 m\n",
              wx, ey, wz, score, kMeadowRadius / 10, near);
  {
    const std::vector<uint32_t> occ = ReadOccupancy(s.ctx, s.world);
    ProbeCell(s.ctx, s.world, s.mats, {wx, ey, wz}, occ, "eye");
    ProbeCell(s.ctx, s.world, s.mats, {wx, ground, wz}, occ, "ground");
    ProbeCell(s.ctx, s.world, s.mats, {wx + 30, ground, wz + 30}, occ, "ground+3m");
    const int gz = World::TerrainHeight(256, 256, kDefaultSeed);
    ProbeCell(s.ctx, s.world, s.mats, {256, gz, 256}, occ, "noon-ground");
  }
  tick = FindNoonTick(CurrentTuning());
  return true;
}

bool CamCanopy(Scene& s, uint32_t& tick, std::string& why) {
  const FoliageScan& f = ScanFoliage(s.ctx, s.world, s.mats);
  if (f.leafMats == 0) {
    why = "no material carries tag:foliage, so there is no canopy to look "
          "through";
    return false;
  }
  uint32_t score = 0;
  const int ci = f.Best(f.leafSat, kCanopyRadius, score);
  if (ci < 0 || score == 0) {
    why = "no leaf cells in the residency window (no trees placed here)";
    return false;
  }
  const int ix = ci % f.span, iz = ci / f.span;
  const int wx = f.x0 + ix, wz = f.z0 + iz;
  // The crown's vertical extent over the 3 m around the winning column: the
  // eye goes 1 m under its lowest leaf so it looks UP through the whole mass.
  int lo = INT_MAX, hi = INT_MIN, ground = INT16_MIN;
  for (int dz = -30; dz <= 30; dz++)
    for (int dx = -30; dx <= 30; dx++) {
      const int jx = ix + dx, jz = iz + dz;
      if (jx < 0 || jz < 0 || jx >= f.span || jz >= f.span) continue;
      const size_t cj = (size_t)jz * f.span + jx;
      if (f.leafAt[cj] == 0) continue;
      lo = std::min(lo, (int)f.leafLo[cj]);
      hi = std::max(hi, (int)f.leafHi[cj]);
      ground = std::max(ground, (int)f.groundY[cj]);
    }
  if (lo == INT_MAX) {
    why = "the winning leaf column has no leaf span (scan inconsistency)";
    return false;
  }
  if (ground == INT16_MIN) ground = World::TerrainHeight(wx, wz, kDefaultSeed);
  // Stand 2.5-8 m off the crown's centre column on the -x-z diagonal, so yaw
  // 0.785 (+x+z) looks INTO the crown, and walk outward / downward until the
  // eye is in open air — the centre column is usually the trunk.
  int ex = wx, ey = std::max(ground + 16, lo - 10), ez = wz;
  bool placed = false;
  for (int off = 25; off <= 80 && !placed; off += 10) {
    const int tx = wx - (int)(off * 0.7071f), tz = wz - (int)(off * 0.7071f);
    for (int ty = std::max(ground + 16, lo - 10); ty >= ground + 12 && !placed;
         ty--) {
      if (EyeCellOpen(f, tx, ty, tz)) { ex = tx; ey = ty; ez = tz; placed = true; }
    }
  }
  s.eye = {(float)ex + 0.5f, (float)ey + 0.5f, (float)ez + 0.5f};
  s.cam.yaw = 0.785f;
  s.cam.pitch = 0.35f;
  char note[360];
  std::snprintf(note, sizeof note,
                "CANOPY: eye %d voxels under the lowest leaf of the largest "
                "crown in the window — %u leaf cells within %d m of (%d,%d), "
                "leaves y=%d..%d, ground y=%d — %s, looking up through the "
                "mass toward the sky",
                lo - ey, score, kCanopyRadius / 10, wx, wz, lo, hi, ground,
                placed ? "2.5-8 m off the centre column on the -x-z diagonal"
                       : "NOT placed in open air (every probe closed)");
  s.note = note;
  std::printf("  canopy site: eye (%d, %d, %d), crown centre (%d,%d), leaves "
              "y=%d..%d — %u leaf cells within %d m%s\n",
              ex, ey, ez, wx, wz, lo, hi, score, kCanopyRadius / 10,
              placed ? "" : "  *** eye not in open air ***");
  tick = FindNoonTick(CurrentTuning());
  return true;
}

const char* const kArmsReduced[] = {
    "baseline", "noshadow", "nofar", "noreflect", "halfres", nullptr};
// The cascade camera: the far march IS the frame here, so the rows that matter
// are the ones that price it and the ones that bound what is left. `nofar` is
// the ceiling on everything the cascade could ever cost.
const char* const kArmsCascade[] = {
    "baseline", "nofar", "noshadow", "halfres", nullptr};
// The foliage cameras: the picture-dependent rows plus the ceilings that only
// mean something with plants in the frame. `nomicro` is the ceiling on the
// whole plant march; `micro1` / `plantlod4` price its two knobs; `lod8` and
// `fine2m` bound the fine march the plants are part of.
const char* const kArmsFoliage[] = {
    "baseline", "noshadow",  "nogi", "nofar",  "halfres", "nomicro",
    "micro1",   "plantlod4", "lod8", "fine2m", nullptr};
const char* const kArmsSubmerged[] = {
    "baseline", "noshadow", "nofar",       "noreflect",
    "halfres",  "nogodray", "godshadow0",  nullptr};

const BudgetCam kBudgetCams[] = {
    {"noon",
     "the overlook at the sun's highest — the easy case, and the only one with "
     "history behind it",
     CamNoon, nullptr},
    {"dusk", "the same overlook with the sun ~8 deg above the horizon",
     CamDusk, kArmsReduced},
    {"cascade",
     "300 voxels up, near-horizontal — the only camera the FAR FIELD draws",
     CamCascade, kArmsCascade},
    {"submerged",
     "eye inside the authored lake — god rays, caustics, Snell's window",
     CamSubmerged, kArmsSubmerged},
    {"meadow",
     "standing in the densest grass the window has — the plant march",
     CamMeadow, kArmsFoliage},
    {"canopy", "under the largest crown, looking up through the leaves",
     CamCanopy, kArmsFoliage},
};
constexpr int kBudgetCamCount =
    (int)(sizeof(kBudgetCams) / sizeof(kBudgetCams[0]));

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
    //
    // AN ARM WITH NO TUNING MUTATION STILL NEEDS THE RELOAD, if the arm before
    // it had one. `SetCurrentTuning` moves the C++ struct; only ReloadShaders
    // re-const-folds the TUNE_* values into the compiled shader. `halfres` and
    // `noshadow` mutate nothing, so the old code skipped the reload for
    // them — and `halfres` is the LAST row of kRenderArms, immediately after
    // `shadow32`. Every `halfres` number this harness has ever printed was
    // therefore measured at half resolution AND shadowSteps=32, i.e. against a
    // baseline it does not share, which is precisely the confound the "one knob
    // moved per arm" design exists to prevent. `dirty_` is the fix: the shader
    // is reloaded whenever the constants it was built from are stale, and not
    // otherwise.
    Tuning t = base;
    if (arm.apply) {
      arm.apply(t);
      SetCurrentTuning(t);
      dirty_ = true;
      if (!sim_.ReloadShaders(ctx_.device)) {
        SetCurrentTuning(base);
        sim_.ReloadShaders(ctx_.device);
        res.why = "shader reload failed";
        return res;
      }
    } else {
      SetCurrentTuning(t);
      if (dirty_) {
        sim_.ReloadShaders(ctx_.device);
        dirty_ = false;
      }
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
                        skyTick_, s.fluidLive);
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
  // Put the baseline tuning back and rebuild the shaders from it. Called
  // between cameras and at the end of the run, so no arm's constants leak into
  // the shot, the next camera, or a later harness in the same process.
  void RestoreBase(const Tuning& base) {
    SetCurrentTuning(base);
    sim_.ReloadShaders(ctx_.device);
    dirty_ = false;
  }

  // `alsoPath`, when given, gets the identical pixels under a second name. That
  // exists for exactly one reason: `build/render_budget.bmp` is the path
  // everything downstream already knows, and the noon camera has to keep
  // writing it even though it now also writes render_budget_noon.bmp.
  void Shot(const Scene& s, const char* path, const char* alsoPath = nullptr) {
    const uint32_t W = opt_.width, H = opt_.height;
    WriteRenderParams(ctx_.queue, world_, s.eye, s.cam, (float)W / (float)H,
                      /*shadows=*/true, kBudgetAnimTime, kFarFogDensity,
                      (float)H, skyTick_, s.fluidLive);
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
    if (alsoPath && WriteBmpFile(alsoPath, px, W, H))
      std::printf("  the same frame also written to %s\n", alsoPath);
  }

  // THE COUNTERS INSIDE THE FRAME (RENDER_STATS, world.h kRenderStat*).
  //
  // The arm table says how many ms a feature costs; this says how many STEPS
  // it took to cost them — primary DDA cells, chunk skips, micro cells
  // entered, plant evaluations — so a ceiling can be read against the work it
  // deleted rather than against a guess.
  //
  // A SEPARATE PASS, AFTER THE ARMS, at the baseline tuning. RENDER_STATS is
  // a prelude const (gpu/resources.h): compiled in, the accumulators live in
  // registers across the whole of trace() and the flush is ~2M atomics a
  // frame, which is exactly the perturbation main.cpp keeps out of --perf.
  // So the arms are timed with the shipping shader and the counters are read
  // from a second compile that is never timed. Two reloads per camera; the
  // caller's RestoreBase does the second.
  //
  // Returns false (and leaves `out` zero) when the device compiled the
  // counters out — fragment atomics are a capability, not a given.
  // THE COUNTERS MUST BE TAKEN AT THE BASELINE TUNING, and until 2026-09-07
  // they were taken at whatever the LAST ARM left behind. `Measure` applies an
  // arm to a copy of the base and leaves it applied — the next arm restores it,
  // and after the last arm nobody does. With the default arm list that is
  // invisible: `halfres` is last, it mutates no tuning, and its own
  // SetCurrentTuning(base) puts the world back. Name a subset and it is not:
  // `--budget-arms baseline,nofar` printed a whole counter table measured with
  // farSteps = 0, i.e. `rmFarSteps 0.000` on a camera whose far march is 12% of
  // the frame. Restoring `base` here is the fix and costs nothing — Measure
  // already reloads whenever `dirty_` says the constants are stale, and this
  // reload was happening anyway.
  bool Stats(const Tuning& base, const Scene& s, double out[kRenderStatSlots]) {
    for (uint32_t k = 0; k < kRenderStatSlots; k++) out[k] = 0;
    SetCurrentTuning(base);
    SetRenderStatsEnabled(true);
    const bool reloaded = sim_.ReloadShaders(ctx_.device);
    SetRenderStatsEnabled(false);
    dirty_ = true;   // the shipping prelude has to be re-folded afterwards
    if (!reloaded) return false;
    const uint32_t W = opt_.width, H = opt_.height;
    rhi::Buffer stage = CreateBuffer(
        ctx_.device, kRenderStatBytes,
        rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst, "budgetStats");
    std::vector<uint32_t> prev(kRenderStatWords, 0), cur(kRenderStatWords, 0);
    // Three frames: the counters are monotonic and never cleared, so a frame
    // is the difference of two readbacks; the first pair absorbs whatever the
    // reload left in the buffer.
    for (int fr = 0; fr < 3; fr++) {
      WriteRenderParams(ctx_.queue, world_, s.eye, s.cam, (float)W / (float)H,
                        /*shadows=*/true, kBudgetAnimTime, kFarFogDensity,
                        (float)H, skyTick_, s.fluidLive);
      rhi::CommandEncoder enc = ctx_.device.CreateCommandEncoder();
      sim_.EncodeShadowResolve(enc);
      {
        rhi::RenderPass rp = sim_.BeginRenderPass(
            enc, view_[0], rhi::TextureFormat::RGBA8Unorm, W, H);
        sim_.DrawWorld(rp);
        rp.End();
      }
      ctx_.queue.Submit(enc.Finish());
      ctx_.WaitIdle();
      rhi::CommandEncoder cenc = ctx_.device.CreateCommandEncoder();
      cenc.CopyBufferToBuffer(world_.renderStats, 0, stage, 0, kRenderStatBytes);
      ctx_.queue.Submit(cenc.Finish());
      ctx_.WaitIdle();
      prev.swap(cur);
      if (!rhi::ReadBufferBlocking(ctx_.device, stage, 0, cur.data(),
                                   kRenderStatBytes))
        return false;
    }
    uint32_t tot[kRenderStatSlots] = {};
    for (uint32_t st = 0; st < kRenderStatStripes; st++)
      for (uint32_t k = 0; k < kRenderStatSlots; k++)
        tot[k] += (uint32_t)(cur[st * kRenderStatSlots + k] -
                             prev[st * kRenderStatSlots + k]);
    if (tot[0] == 0) return false;   // compiled out, or nothing sampled
    for (uint32_t k = 0; k < kRenderStatSlots; k++)
      out[k] = (double)tot[k] * (double)kRenderStatSample;
    return true;
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
  // True when the shaders currently compiled were built from a MUTATED tuning,
  // so the next arm that mutates nothing still has to reload. See Measure.
  bool dirty_ = false;
 public:
  // The celestial tick the sky is derived from. Fixed across every arm of a
  // camera — the frame each arm renders must be the SAME frame — and set per
  // CAMERA, which is how `dusk` is a different lighting condition rather than a
  // different viewpoint.
  uint32_t skyTick_ = 0;
};

}  // namespace

int RunRenderBudget(GpuContext& ctx, World& world, Simulation& sim,
                    const std::vector<MaterialDef>& mats,
                    const PerfOptions& opt,
                    std::vector<RenderBudgetRow>* rows) {
  std::printf("=== sandvox --render-budget: where inside the raymarch ===\n");
  // An arm name that matches nothing is a typo, and a typo that silently runs
  // the full table costs the minutes the subset was meant to save.
  for (const std::string& want : opt.arms) {
    if (!FindArm(want.c_str())) {
      std::fprintf(stderr, "--render-budget: no arm named '%s' (try: ",
                   want.c_str());
      for (const RenderArm& arm : kRenderArms) std::fprintf(stderr, "%s ", arm.id);
      for (const RenderArm& arm : kExtraArms) std::fprintf(stderr, "%s ", arm.id);
      std::fprintf(stderr, ")\n");
      return 1;
    }
  }
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
  g_foliage = FoliageScan{};   // the scan describes THIS settled world only

  // Which cameras. `--scenario <id>` keeps its old meaning exactly — one
  // camera, borrowed from a --perf scenario, all 16 arms — and bypasses the
  // camera table entirely. Otherwise `--budget-cams a,b,c` selects from
  // kBudgetCams; empty selects all three.
  std::vector<const BudgetCam*> cams;
  if (!pick) {
    if (opt.cams.empty()) {
      for (const BudgetCam& c : kBudgetCams) cams.push_back(&c);
    } else {
      std::string tok;
      std::string src = opt.cams + ",";
      for (char ch : src) {
        if (ch != ',') { tok += ch; continue; }
        if (tok.empty()) continue;
        const BudgetCam* f = nullptr;
        for (const BudgetCam& c : kBudgetCams)
          if (tok == c.id) { f = &c; break; }
        if (!f) {
          std::fprintf(stderr,
                       "--budget-cams: no camera named '%s'. Known: ",
                       tok.c_str());
          for (const BudgetCam& c : kBudgetCams)
            std::fprintf(stderr, "%s ", c.id);
          std::fprintf(stderr, "\n");
          return 1;
        }
        cams.push_back(f);
        tok.clear();
      }
    }
  }

  // The tuning every arm is a delta FROM. Restored after each arm, and again at
  // the end, so nothing here leaks into a later harness in the same process.
  const Tuning base = CurrentTuning();

  // What the JSON is written from, and what the summary at the end reads.
  struct CamRun {
    std::string id, label, note, bmp;
    Vec3 eye{};
    float yaw = 0, pitch = 0;
    uint32_t tick = 0;
    float sunUp = 0;
    std::vector<ArmResult> arms;
    bool haveCache = false;
    uint32_t cReq = 0, cRes = 0, cRef = 0;
    // RENDER_STATS per-frame totals at the baseline tuning (Runner::Stats);
    // slot k is perfnodes.h PerfCounter::RmPixels + k.
    bool haveStats = false;
    double stats[kRenderStatSlots] = {};
  };
  std::vector<CamRun> runs;
  // The baseline is the arm NAMED baseline, not arms[0]: under --budget-arms
  // the first arm measured can be any arm, and a delta against it is noise.
  // 0 when the subset left it out — every "saved" column then prints blank.
  auto baselineOf = [](const CamRun& cr) {
    for (const ArmResult& r : cr.arms)
      if (r.arm == &kRenderArms[0] && r.ok) return r.gpuP50;
    return 0.0;
  };

  // The shadow-cache attribution, per camera (CLAUDE.md rule 6).
  //
  // A bare "the resolve pass costs 3.9 ms" is the kind of number that invites
  // one elimination run per hypothesis. The two numbers that actually decide
  // between them — how many patches were asked for, and how many were REFUSED
  // because the request list was full — are two words the shader already
  // maintains, so read them instead of guessing. Read AFTER the Shot, which
  // renders one frame at the baseline tuning, so these always describe that
  // frame and not whichever arm happened to run last.
  //
  // Per camera and not once at the end, because dedup is a property of the
  // PICTURE: a submerged frame and an overlook frame request wildly different
  // patch counts, and one number labelled with neither is the misreading the
  // whole camera table exists to prevent.
  auto readCache = [&](CamRun& cr) {
    if (!sim.ShadowCacheOn()) return;
    rhi::Buffer stage =
        CreateBuffer(ctx.device, 16,
                     rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                     "shadowStatsRead");
    rhi::CommandEncoder senc = ctx.device.CreateCommandEncoder();
    senc.CopyBufferToBuffer(world.shadowReq, 0, stage, 0, 16);
    ctx.queue.Submit(senc.Finish());
    uint32_t w[4] = {0, 0, 0, 0};
    if (!rhi::ReadBufferBlocking(ctx.device, stage, 0, w, sizeof(w))) return;
    cr.haveCache = true;
    cr.cRes = w[1];
    cr.cReq = w[2];
    cr.cRef = w[3];
    const double px = (double)opt.width * opt.height;
    std::printf(
        "\n  shadow cache: %u patches requested, %u resolved (cap %u), %u "
        "refused\n            %.2f patches per 100 px — under 100 is real "
        "dedup, at the cap it is truncation\n",
        w[2], w[1], kShadowReqCap, w[3], 100.0 * (double)w[2] / px);
  };

  // One camera's whole pass: set it up, run its arms, print its table, shoot
  // its frame, read its cache counters.
  auto runCamera = [&](const char* id, const char* label,
                       Scene& scene, uint32_t tick,
                       const char* const* armIds) {
    CamRun cr;
    cr.id = id;
    cr.label = label ? label : "";
    cr.note = scene.note;
    cr.eye = scene.eye;
    cr.yaw = scene.cam.yaw;
    cr.pitch = scene.cam.pitch;
    cr.tick = tick;
    cr.sunUp = ComputeSky(CurrentTuning(), (double)tick).sunDir[1];
    cr.bmp = std::string("build/render_budget_") + id + ".bmp";
    runner.skyTick_ = tick;

    // Which arms. A nullptr list means every row of kRenderArms, in order.
    std::vector<const RenderArm*> arms;
    if (!armIds) {
      for (const RenderArm& a : kRenderArms) arms.push_back(&a);
    } else {
      for (const char* const* p = armIds; *p; p++) {
        const RenderArm* a = FindArm(*p);
        // A typo in the table is a build-time authoring error, not a runtime
        // condition; say so loudly rather than quietly measuring 4 arms.
        if (!a) {
          std::fprintf(stderr,
                       "--render-budget: camera '%s' names arm '%s', which "
                       "does not exist\n", id, *p);
          continue;
        }
        arms.push_back(a);
      }
    }

    // --budget-arms: the caller's subset, applied on top of the camera's own
    // arm list (validated against the full table at the top of the function).
    if (!opt.arms.empty()) {
      std::vector<const RenderArm*> kept;
      for (const RenderArm* a : arms)
        for (const std::string& want : opt.arms)
          if (want == a->id) { kept.push_back(a); break; }
      arms.swap(kept);
    }

    std::printf("\n=== camera %s — %s ===\n", id, cr.label.c_str());
    std::printf("        %s\n", scene.note.c_str());
    std::printf("        eye (%.0f, %.0f, %.0f)  yaw %.2f  pitch %.2f\n",
                scene.eye.x, scene.eye.y, scene.eye.z, scene.cam.yaw,
                scene.cam.pitch);
    std::printf("        sky at celestial tick %u — sun elevation %+.3f "
                "(%.1f deg above the horizon)\n",
                tick, cr.sunUp,
                std::asin(std::max(-1.0f, std::min(1.0f, cr.sunUp))) *
                    57.2957795f);
    std::printf("render: %ux%u   arms: %d   (one settled world, one camera, "
                "one knob moved per arm)\n\n",
                opt.width, opt.height, (int)arms.size());

    for (const RenderArm* arm : arms) {
      ArmResult r = runner.Measure(*arm, base, scene, /*warm=*/12,
                                   /*frames=*/48);
      std::printf("  %-11s %-42s ", arm->id, arm->label);
      if (!r.ok) std::printf("SKIPPED — %s\n", r.why.c_str());
      else std::printf("%7.2f ms\n", r.gpuP50);
      std::fflush(stdout);
      if (rows) {
        RenderBudgetRow row;
        row.cam = id;
        row.arm = arm->id;
        row.ok = r.ok;
        row.gpuP50Ms = r.gpuP50;
        row.gpuP95Ms = r.gpuP95;
        row.why = r.why;
        rows->push_back(std::move(row));
      }
      cr.arms.push_back(r);
    }
    // The counters, from a second compile at the baseline tuning (Stats).
    cr.haveStats = runner.Stats(base, scene, cr.stats);
    runner.RestoreBase(base);
    // `noon` keeps writing build/render_budget.bmp under its old name as well:
    // that path is what every previous run and every reader already knows.
    runner.Shot(scene, cr.bmp.c_str(),
                cr.id == "noon" ? "build/render_budget.bmp" : nullptr);
    readCache(cr);

    // ---- the table this camera exists to print --------------------------
    const double b = baselineOf(cr);
    std::printf("\n  baseline raymarch: %.2f ms at %ux%u\n\n", b, opt.width,
                opt.height);
    std::printf("  %-11s %9s %9s  %s\n", "arm", "ms", "saved",
                "what the saving is the cost of");
    std::printf("  %-11s %9s %9s  %s\n", "---", "--", "-----",
                "------------------------------");
    for (const ArmResult& r : cr.arms) {
      if (!r.ok) {
        std::printf("  %-11s   SKIPPED  %s\n", r.arm->id, r.why.c_str());
        continue;
      }
      if (r.arm == &kRenderArms[0]) continue;
      const double saved = b - r.gpuP50;
      std::printf("  %-11s %9.2f %8.2f%s  %s\n", r.arm->id, r.gpuP50, saved,
                  b > 0 ? "" : " ", r.arm->means);
    }
    std::printf("\n  percentages of the %.2f ms baseline:\n", b);
    for (const ArmResult& r : cr.arms) {
      if (!r.ok || r.arm == &kRenderArms[0] || b <= 0) continue;
      std::printf("    %-11s %5.1f%%\n", r.arm->id,
                  100.0 * (b - r.gpuP50) / b);
    }
    // ---- the counters, per pixel -----------------------------------------
    // Divided by the SAMPLED pixel count (slot 0, scaled back up), not W*H.
    if (cr.haveStats) {
      const double px = cr.stats[0];
      std::printf("\n  inside the baseline frame (RENDER_STATS, per pixel; "
                  "%.0f px sampled, x%u):\n", px / kRenderStatSample,
                  kRenderStatSample);
      for (uint32_t k = 1; k < kRenderStatSlots; k++) {
        const PerfCounterDef& d =
            kPerfCounters[(int)PerfCounter::RmPixels + (int)k];
        std::printf("    %-20s %8.3f  %s\n", d.key, cr.stats[k] / px, d.label);
      }
    } else {
      std::printf("\n  RENDER_STATS unavailable (no fragment atomics on this "
                  "device, or the reload failed) — no per-pixel counters\n");
    }
    runs.push_back(std::move(cr));
  };

  if (pick) {
    Scene scene{ctx, world, sim, stream, mats};
    std::string why;
    if (!pick->setup(scene, why)) {
      std::fprintf(stderr, "--render-budget: scenario '%s' declined: %s\n",
                   pick->id, why.c_str());
      return 1;
    }
    runCamera(pick->id, pick->label, scene, FindNoonTick(CurrentTuning()),
              nullptr);
  } else {
    for (const BudgetCam* c : cams) {
      // A FRESH Scene per camera. Scene is scenario scratch as well as a
      // camera, and carrying one across cameras would let a pond index or a
      // note leak into a view it has nothing to do with.
      Scene scene{ctx, world, sim, stream, mats};
      uint32_t tick = 0;
      std::string why;
      if (!c->setup(scene, tick, why)) {
        std::printf("\n=== camera %s — DECLINED ===\n        %s\n", c->id,
                    why.c_str());
        continue;
      }
      runCamera(c->id, c->label, scene, tick, c->arms);
    }
  }

  // ---- the machine-readable copy ----------------------------------------
  // So nobody re-runs a 3-camera, 26-arm pass to read one number back off the
  // terminal. Objects rather than the flat arrays --perf uses: this is a few
  // hundred numbers, not 35,000, and the readability is worth more than the
  // bytes here.
  {
    const char* jpath = "build/render_budget.json";
    std::FILE* f = std::fopen(jpath, "wb");
    if (!f) {
      std::fprintf(stderr, "--render-budget: cannot write %s\n", jpath);
    } else {
      std::fprintf(f, "{\n\"schema\":1,\n\"gpu\":%s,\n",
                   JStr(ctx.DeviceName()).c_str());
      std::fprintf(f, "\"width\":%u,\"height\":%u,\n", opt.width, opt.height);
      std::fprintf(f, "\"cameras\":[\n");
      for (size_t ci = 0; ci < runs.size(); ci++) {
        const CamRun& cr = runs[ci];
        const double b = baselineOf(cr);
        std::fprintf(f, "%s{\"id\":%s,\"label\":%s,\"note\":%s,\"bmp\":%s,\n",
                     ci ? "," : "", JStr(cr.id).c_str(),
                     JStr(cr.label).c_str(), JStr(cr.note).c_str(),
                     JStr(cr.bmp).c_str());
        std::fprintf(f, " \"eye\":[%s,%s,%s],\"yaw\":%s,\"pitch\":%s,\n",
                     JNum(cr.eye.x).c_str(), JNum(cr.eye.y).c_str(),
                     JNum(cr.eye.z).c_str(), JNum(cr.yaw).c_str(),
                     JNum(cr.pitch).c_str());
        std::fprintf(
            f, " \"tick\":%u,\"sunSinElev\":%s,\"sunElevDeg\":%s,\n", cr.tick,
            JNum(cr.sunUp).c_str(),
            JNum(std::asin(std::max(-1.0f, std::min(1.0f, cr.sunUp))) *
                 57.2957795f)
                .c_str());
        std::fprintf(f, " \"baselineMs\":%s,\n \"arms\":[", JNum(b).c_str());
        for (size_t ai = 0; ai < cr.arms.size(); ai++) {
          const ArmResult& r = cr.arms[ai];
          std::fprintf(f, "%s{\"id\":%s,\"label\":%s,\"means\":%s,\"ok\":%s",
                       ai ? "," : "", JStr(r.arm->id).c_str(),
                       JStr(r.arm->label).c_str(), JStr(r.arm->means).c_str(),
                       r.ok ? "true" : "false");
          if (r.ok)
            std::fprintf(f,
                         ",\"p50\":%s,\"p95\":%s,\"frames\":%d,"
                         "\"savedMs\":%s,\"pct\":%s",
                         JNum(r.gpuP50).c_str(), JNum(r.gpuP95).c_str(),
                         r.frames, JNum(b - r.gpuP50).c_str(),
                         JNum(b > 0 ? 100.0 * (b - r.gpuP50) / b : 0.0)
                             .c_str());
          else
            std::fprintf(f, ",\"why\":%s", JStr(r.why).c_str());
          std::fprintf(f, "}");
        }
        std::fprintf(f, "],\n \"stats\":");
        if (cr.haveStats) {
          // Per pixel of the sampled count, keyed by the perfnodes.h rm*
          // names; `rmPixels` itself is the absolute sampled count, scaled.
          std::fprintf(f, "{");
          for (uint32_t k = 0; k < kRenderStatSlots; k++) {
            const PerfCounterDef& d =
                kPerfCounters[(int)PerfCounter::RmPixels + (int)k];
            std::fprintf(f, "%s%s:%s", k ? "," : "", JStr(d.key).c_str(),
                         JNum(k == 0 ? cr.stats[0] : cr.stats[k] / cr.stats[0])
                             .c_str());
          }
          std::fprintf(f, "}");
        } else {
          std::fprintf(f, "null");
        }
        std::fprintf(f, ",\n \"shadowCache\":");
        if (cr.haveCache) {
          const double px = (double)opt.width * opt.height;
          std::fprintf(f,
                       "{\"requested\":%u,\"resolved\":%u,\"cap\":%u,"
                       "\"refused\":%u,\"perHundredPx\":%s}",
                       cr.cReq, cr.cRes, (unsigned)kShadowReqCap, cr.cRef,
                       JNum(100.0 * (double)cr.cReq / px).c_str());
        } else {
          std::fprintf(f, "null");
        }
        std::fprintf(f, "}\n");
      }
      std::fprintf(f, "]\n}\n");
      std::fclose(f);
      std::printf("\n  wrote %s (%d camera%s)\n", jpath, (int)runs.size(),
                  runs.size() == 1 ? "" : "s");
    }
  }

  std::printf(
      "\n  An arm is a MEASUREMENT, not a setting: `nofar` and `primary256` "
      "render\n  a visibly wrong world. Read each row as \"this many ms are "
      "spent here\".\n"
      "  And read each TABLE as a property of its picture: `noon` is the "
      "cheapest\n  lighting the game has, which is why it is no longer the "
      "only one here.\n");
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
      const int nodeIdx = PerfNodeForTimedName(st.name.c_str());
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
      // The new SubmitTick spans go through the SAME gate. They are the ones
      // that bracket raw transfer commands rather than dispatches, so if any
      // timestamp in this engine could perturb a result it would be these.
      SetSubmitTickPassTimer(t);
      SubmitWorldgen(ctx, world, sim, kDefaultSeed);
      ctx.WaitIdle();
      for (uint32_t k = 1; k <= 60; k++)
        SubmitTick(ctx, world, sim, k, kDefaultSeed, {}, {}, {}, k % 15 == 0,
                   {8, 3, 8}, false, false);
      ctx.WaitIdle();
      const uint32_t h = HashWorldNow(ctx, world, sim, kDefaultSeed);
      ctx.WaitIdle();
      sim.SetPassTimer(nullptr);
      SetSubmitTickPassTimer(nullptr);
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
  SetSubmitTickPassTimer(nullptr);
  if (!WriteJson(opt.out, runs, opt, ctx, world, runner.HaveTimer())) return 1;
  std::printf("\nwrote %s (%zu scenario%s)\n", opt.out.c_str(), runs.size(),
              runs.size() == 1 ? "" : "s");
  std::printf("open the tuner's Performance tab to read it.\n");
  return ctx.ReportVkValidation("--perf") > 0 ? 1 : 0;
}

}  // namespace sandvox
