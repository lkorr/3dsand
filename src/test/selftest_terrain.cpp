// selftest_terrain.cpp — the terrain gate.
//
// WHY THIS EXISTS AT ALL.
//
// Until this file, NOTHING in the suite measured terrain. Worldgen was asserted
// only three ways, all of them indirect:
//
//   * `determinism` — one 32-bit hash over the whole world. It detects ANY
//     worldgen change and localizes NONE of them.
//   * `smokeQuiet[0]` / `smokeLoud[0]` — pinned hashes at tick 0, which IS pure
//     worldgen output, and again a single number.
//   * `sleep` — the settle gate. In practice this has been the worldgen-quality
//     gate (the disc-pond redesign was driven by it: 82 chunks still awake
//     around one pond after 600 settle ticks), but it says nothing about shape.
//
// Nothing asserted the terrain's height range, its continuity, that the CPU
// mirror agrees with the GPU, or that the fixture columns every other gate
// plants bodies on are actually clear. Those were prose in comments. A terrain
// change therefore landed on a suite that could only report it as a confusing
// secondary symptom somewhere else — `debris` dropping wrong, `player-walk`
// missing the ground, `screenshots` burying the camera in a hillside, or a
// page-pool abort.
//
// That was survivable while the world had 5.4 m of relief. It is not survivable
// through the terrain overhaul (docs/RESEARCH_worldgen.md), which moves relief
// by more than an order of magnitude. So: build the instrument first.
//
// ---- WHAT IT ASSERTS, and what each assertion is FOR ----------------------
//
// Pass A (analytic, CPU only, ~50 ms) — properties of the height function:
//   A1 WINDOW CONTAINMENT. The surface fits inside the 512-voxel residency
//      window with margin. This is the highest-value assertion in the file and
//      it does not exist in the research doc: once relief exceeds the window,
//      "the terrain is not in the window at all" becomes the DEFAULT failure,
//      and every other gate degrades from it silently and confusingly.
//   A2 RELIEF STATISTICS. min/max/mean/p95 of surface Y. The headline number of
//      any scale pass, and what tells you at a glance whether the datum moved.
//   A3 CRENELLATION. max and p99.9 of |dh| between adjacent columns. Slope, not
//      amplitude, is what makes cliffs; and per PLAN_page_table.md it is
//      SURFACE AREA rather than depth that costs resident pages. So this is the
//      residency and avalanche predictor, and it costs nothing.
//   A4 RAMP CONTINUITY. The same slope metric on transects crossing the spawn
//      region boundary at four headings. A terrain flattening applied over too
//      short a fade builds a cliff at exactly the boundary — an avalanche
//      generator and a page wall constructed by the test fixture itself. This
//      is the assertion that measures the fade width so nobody has to guess it.
//   A5 TREELINE BRACKETING. TUNE_TREELINE sits strictly inside [p5, p95] of the
//      surface. A scale pass that leaves the treeline behind produces a world
//      with either no trees or nothing but trees, and neither is loud.
//   A7 OVERFLOW GUARD. 255*cs^2 < INT32_MAX for every live noise cell size.
//      vnoise's numerator crosses 2^31 at cs = 2901 voxels; C++ signed overflow
//      is UB and WGSL's wraps, so the mirror and the GPU would diverge SILENTLY
//      and seed-dependently. Five lines, mostly subsumed by C1, kept as
//      belt-and-braces because it names the cause where C1 only shows a symptom.
//
//   (A6, the dense pond-rim sweep, is deliberately NOT here — see the note at
//    the end of PassA.)
//
// Pass B (whole window, no readback) — occupancy already comes back CPU-side
//   under the harness snapshot drain, so the topmost non-empty CHUNK of every
//   chunk-column is free. Coarse (16-voxel granularity) but it covers all 1024
//   chunk-columns, which makes it the cheap global net for a gross CPU/GPU
//   divergence: an overflow, a rule only one side implements, a datum mismatch.
//
// Pass C (targeted readback, ~0.5 s) — the exact checks:
//   C1 CPU/GPU HEIGHT AGREEMENT, per voxel. THE assertion this file was written
//      for. `World::TerrainHeight` is a hand-written mirror of shader
//      arithmetic and the consequence of a mismatch is a player falling through
//      ground they can see, at some seeds, in some places. That hazard has been
//      enforced by a COMMENT (world.cpp) and nothing else.
//   C2 FIXTURE CLEARANCE. No blocking voxel in [h+1, h+24] at the columns other
//      gates drop bodies onto. This is exactly the property `inSpawnClearing`
//      and `onFixturePad` exist to provide, and it too was asserted only in
//      prose. When it breaks, it breaks `debris`/`prefab`/`player-body` in ways
//      that look like physics bugs.
//   C3 LIQUID SEPARATION. No water/oil cell face-adjacent to a lava cell.
//      Vacuous today and that is fine — it goes live the moment a global sea
//      level exists, where it guards a genuine world-sized rule-2 catastrophe
//      (reactions.json has water->steam on tag:hot AND lava->stone on water,
//      both directions, so a shared face is a planetary reaction front).
//
// Pass D (settle proxy, ~2 s) — 120 ticks then count awake chunks. This
//   OVERLAPS `sleep` on purpose and does not replace it: it is a two-second
//   early warning so a bad fill rule is caught here rather than sixty seconds
//   into --gate sleep. Advisory bound, deliberately loose.
//
// ---- BUDGET ----------------------------------------------------------------
//
// Pass C is the only expensive part, and it is kept to ~28 blocking readbacks
// by exploiting the slot layout: SlotChunkIndex is contiguous in cx, so a run
// of chunks along X is ONE call. Reading a vertical stack instead would be one
// call per chunk at stride kNChunk. The cy range comes from the mirror, and
// pass B is what independently validates that the range is the right range, so
// the narrowing is not circular.
//
// Thresholds live in tests/baseline.json (see selftest::BaselineNumber), not in
// this file, so retuning one costs an edit rather than a rebuild.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "sim/biomes.h"
#include "sim/stream.h"
#include "sim/tuning.h"
#include "sim/worldedit.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

// (A8, the ruin-pad check, went with the ruin scatter in the world map's P2b;
// authored sites return through the map's site table in P5.)

namespace selftest {
namespace {

// ---- the sampled region ---------------------------------------------------
//
// Pass A samples analytically and can go as wide as it likes; ±1024 voxels
// around the window centre is 2 km of ground at 10 cm/voxel, wide enough to
// contain the spawn region AND its ramp-out.
constexpr int kAnalyticHalf = 1024;
constexpr int kAnalyticStep = 16;   // 129x129 columns, ~16k TerrainHeight calls

// Pass C reads voxels, so it is bounded by readback cost AND by what can be
// legitimately compared. The window is x,z in [48,144]:
//
//   * it contains every fixture column the suite uses (60, 80, 90, 100, 108,
//     110, 120, 140), which is the point;
//   * it excludes the authored set pieces that legitimately put solid matter
//     ABOVE the terrain height and would read as false ground — the combat
//     arena needs x >= 148, the wood platform occupies x,z in 146..186, and the
//     nearest ruin tile is 256 voxels out (tile (0,0) is excluded by
//     worldgen.wgsl);
//   * `inSpawnClearing` suppresses tree TRUNKS in 0..220 and the nearest trunk
//     outside it reaches ~67 voxels in, i.e. to x ~= 153 — clear of 144;
//   * `pondInfo`'s keep-out box (-44..264) means no pond can carve here.
//
// So inside this box the terrain is the height function plus the fixture-pad
// sand caps and nothing else, which is what makes C1 a clean equality rather
// than a tolerance with a list of exceptions.
constexpr int kVoxLo = 48, kVoxHi = 144;

// Columns other gates plant fixtures on. C2 asserts each is clear overhead.
// Kept here rather than in each gate because the guarantee is a WORLDGEN
// property (inSpawnClearing / onFixturePad) and this is the file that tests
// worldgen; a gate that trips over a buried fixture should be able to point
// here rather than re-derive why its drop went wrong.
struct FixtureCol { int x, z; const char* who; };
constexpr FixtureCol kFixtures[] = {
    {60, 60, "debris islands"},      {80, 80, "prefab"},
    {90, 90, "burn plank / shatter"},{100, 100, "determinism ops, support"},
    {108, 108, "screenshots eye"},   {110, 110, "sleep blast"},
    {120, 120, "player-body"},       {140, 140, "player-walk"},
};
constexpr int kClearAbove = 24;   // voxels of headroom a fixture column needs

double Pct(std::vector<int>& v, double p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return (double)v[(size_t)(p * (double)(v.size() - 1))];
}

// ---------------------------------------------------------------------------
// Pass A — analytic. No GPU, no readback.
// ---------------------------------------------------------------------------
struct PassAOut {
  bool ok = true;
  int hMin = 0, hMax = 0;
  double hMean = 0, hP5 = 0, hP95 = 0;
  int slopeMax = 0;
  double slopeP999 = 0;
  int rampMax = 0;
  int localRelief = 0;   // max surface range over a window-wide (512 vox) span
  int farMin = 0, farMax = 0;   // relief out where the home ramp is fully spent
  int boxMin = 0, boxMax = 0;   // surface range over pass C's readback box ONLY
  int pondCols = 0;      // columns sampled inside a tarn
  int bermCols = 0;      // columns sampled in a tarn's berm core
  std::string why;
};

// A6 — THE BERM INVARIANT, which is what pond containment IS now.
//
// The formulation this replaced set the waterline to `min(24 rim samples) - 2`
// and hoped the minimum was a good enough estimate; its own comment justified 24
// directions for "the largest (r=36) pond" while tuning had taken the radius to
// 127. Containment was a SAMPLING DENSITY dressed as an invariant, and the honest
// test of it would have been a 512-direction sweep — expensive, and still only
// evidence.
//
// The waterline now comes from the pond's own CENTRE column and the annulus
// outside the disc is FORCED to (surface + pondBerm), so containment is a
// property of the height function and the test is a statement of it:
//
//     inside a disc      -> the ground is BELOW the waterline (a bowl exists)
//     in the berm core   -> the ground is AT OR ABOVE waterline + pondBerm
//
// Two comparisons per sampled column, no rim sweep, and it holds at every seed
// and every radius rather than at the ones somebody checked.
bool CheckPondColumn(int x, int z, int h, uint32_t seed, PassAOut& o) {
  const World::PondQuery q = World::PondNearColumn(x, z, seed);
  if (q.inDisc) {
    o.pondCols++;
    if (h >= q.surf) {
      o.ok = false;
      o.why += Format("%spond column (%d,%d): ground y%d is not below its own "
                      "waterline y%d — the bowl was not carved",
                      o.why.empty() ? "" : "; ", x, z, h, q.surf);
      return false;
    }
    return true;
  }
  // The core is the flat part of the berm — the wall the water cannot cross.
  // Outside it the lift ramps back to natural ground on purpose, so only the
  // core carries the guarantee. bermLift() in worldgen.wgsl defines it, from
  // the PRESET's berm (P-F): the query carries the height and width the
  // pond's own preset gave it.
  const int core = std::max(q.bermW / 4, 2);
  if (!q.near || q.past >= core) return true;
  o.bermCols++;
  if (h < q.surf + q.bermH) {
    o.ok = false;
    o.why += Format("%sberm column (%d,%d) at %d past the rim: ground y%d is "
                    "under waterline y%d + berm %d (preset %u) — this tarn leaks",
                    o.why.empty() ? "" : "; ", x, z, q.past, h, q.surf,
                    q.bermH, q.preset);
    return false;
  }
  return true;
}

PassAOut PassA(World& world, uint32_t seed, std::string* log) {
  PassAOut o;
  const IVec3 org = world.WindowOrigin();
  const int cx = org.x * (int)kChunk + (int)kWorldN / 2;
  const int cz = org.z * (int)kChunk + (int)kWorldN / 2;
  const int winLo = org.y * (int)kChunk;
  const int winHi = winLo + (int)kWorldN;

  std::vector<int> hs;
  hs.reserve(((2 * kAnalyticHalf / kAnalyticStep) + 1) *
             ((2 * kAnalyticHalf / kAnalyticStep) + 1));
  // Columns the coarse grid found inside a tarn; A6's directed sweep walks out
  // from a bounded number of them so the berm core (2-3 voxels wide) is actually
  // sampled rather than stepped over by a 16-voxel stride.
  std::vector<std::pair<int, int>> inPond;
  for (int dz = -kAnalyticHalf; dz <= kAnalyticHalf; dz += kAnalyticStep)
    for (int dx = -kAnalyticHalf; dx <= kAnalyticHalf; dx += kAnalyticStep) {
      const int x = cx + dx, z = cz + dz;
      const int h = World::TerrainHeight(x, z, seed);
      hs.push_back(h);
      CheckPondColumn(x, z, h, seed, o);
      if (inPond.size() < 24 && World::PondNearColumn(x, z, seed).inDisc)
        inPond.push_back({x, z});
    }

  // A6's directed half: from each recorded interior column walk out along +x
  // until the disc ends, then check every column of the berm core. Bounded by
  // construction — 24 probes of at most (maxR + bermWidth) steps.
  {
    // The widest disc + band any preset or authored lake can produce (P-F):
    // the walk stops at the berm's outer edge long before that.
    const int reach = World::PondReachMax();
    for (auto [px, pz] : inPond) {
      for (int i = 0; i <= reach; i++) {
        const int x = px + i;
        const int h = World::TerrainHeight(x, pz, seed);
        if (!CheckPondColumn(x, pz, h, seed, o)) break;
        const World::PondQuery q = World::PondNearColumn(x, pz, seed);
        if (!q.inDisc && (!q.near || q.past >= q.bermW)) break;
      }
    }
  }

  o.hMin = *std::min_element(hs.begin(), hs.end());
  o.hMax = *std::max_element(hs.begin(), hs.end());
  double sum = 0;
  for (int h : hs) sum += h;
  o.hMean = sum / (double)hs.size();
  std::vector<int> sorted = hs;
  o.hP5 = Pct(sorted, 0.05);
  o.hP95 = Pct(sorted, 0.95);

  // A1 — SPAWN CONTAINMENT. Not "all sampled terrain fits in the window": that
  // is neither true nor desirable. The window is 512 voxels and the world will
  // have ~2000 voxels of relief, so distant peaks are SUPPOSED to be outside it
  // and render from the cascades, and the ground continues below it down to the
  // magma table at y-80 (already outside today).
  //
  // The property that actually matters is local: wherever the window is
  // centred, the ground under it must be inside it with sky above. That is what
  // breaks when a datum moves without the spawn logic following, and it is the
  // failure that makes every other gate fail confusingly instead of loudly.
  const int margin = (int)BaselineNumber("terrain.windowMargin", 48);
  {
    const int hCentre = World::TerrainHeight(cx, cz, seed);
    if (hCentre < winLo || hCentre > winHi - margin) {
      o.ok = false;
      o.why = Format("window-centre ground y%d is not inside the window "
                     "y%d..y%d with %d of sky above it",
                     hCentre, winLo, winHi, margin);
    }
  }

  // A2b — LOCAL RELIEF. The window-sized version of the same question: over any
  // 512-voxel horizontal span, how much vertical range is there? If a span's
  // range exceeds the window, a player standing at its low end has the high end
  // outside residency — legal (that is what cascades are for) but it bounds how
  // much of a slope can be sim-live at once, so it is a number worth tracking
  // rather than discovering. Measured on the transects below; see o.localRelief.

  // A3 — crenellation, on the FINE lattice. Sampled every voxel along four
  // 1024-voxel transects rather than on the coarse grid above: adjacent-column
  // slope is meaningless at a 16-voxel stride, and slope is the number that
  // predicts both avalanches and resident pages.
  {
    std::vector<int> d;
    d.reserve(4 * 2 * kAnalyticHalf);
    for (int t = 0; t < 4; t++) {
      const bool alongX = (t & 1) == 0;
      const int off = (t < 2) ? 0 : 512;
      std::vector<int> line;
      line.reserve(2 * kAnalyticHalf + 1);
      for (int i = -kAnalyticHalf; i <= kAnalyticHalf; i++) {
        const int x = alongX ? cx + i : cx + off;
        const int z = alongX ? cz + off : cz + i;
        const int h = World::TerrainHeight(x, z, seed);
        line.push_back(h);
        CheckPondColumn(x, z, h, seed, o);   // A6, on the fine lattice
      }
      for (size_t i = 1; i < line.size(); i++)
        d.push_back(std::abs(line[i] - line[i - 1]));
      // A2b — local relief over a sliding window-width span.
      const size_t span = kWorldN;
      for (size_t i = 0; i + span < line.size(); i += 64) {
        const auto lo = std::min_element(line.begin() + i, line.begin() + i + span);
        const auto hi = std::max_element(line.begin() + i, line.begin() + i + span);
        o.localRelief = std::max(o.localRelief, *hi - *lo);
      }
    }
    o.slopeMax = *std::max_element(d.begin(), d.end());
    o.slopeP999 = Pct(d, 0.999);
    const int cap = (int)BaselineNumber("terrain.slopeMax", 64);
    if (o.slopeMax > cap) {
      o.ok = false;
      o.why += Format("%smax adjacent-column step %d > %d (the CA's angle of "
                      "repose is 1 voxel/column; above that, loose material on "
                      "this ground avalanches forever)",
                      o.why.empty() ? "" : "; ", o.slopeMax, cap);
    }
  }

  // A4 — ramp continuity across the spawn-region boundary, and the only look
  // this gate gets at the world OUTSIDE the calm home area. A fade applied over
  // too short a distance shows up here as a step that dwarfs the ambient one,
  // and the whole point is that the fade width is READ off this number rather
  // than guessed. Walk out from the origin along +x, +z and both diagonals,
  // since a Chebyshev-shaped region has its steepest boundary on the axes and
  // its longest on the diagonal.
  //
  // The transects also carry the FAR RELIEF, which the ±1024 grid above cannot
  // see: with a 320-voxel calm radius and a 2048-voxel fade, the grid's own
  // corners are still only ~45% of the way to full amplitude, so its
  // "relief" number is a property of the ramp rather than of the terrain. These
  // run to 3072 and are fully ramped over most of that.
  {
    int worst = 0;
    o.farMin = INT32_MAX;
    o.farMax = INT32_MIN;
    for (int t = 0; t < 4; t++) {
      const int sx = (t == 0 || t == 2 || t == 3) ? 1 : 0;
      const int sz = (t == 1 || t == 2) ? 1 : (t == 3 ? -1 : 0);
      int prev = World::TerrainHeight(0, 0, seed);
      for (int i = 1; i <= 3072; i++) {
        const int h = World::TerrainHeight(i * sx, i * sz, seed);
        worst = std::max(worst, std::abs(h - prev));
        o.farMin = std::min(o.farMin, h);
        o.farMax = std::max(o.farMax, h);
        prev = h;
      }
    }
    o.rampMax = worst;
  }

  // The surface range over pass C's readback box, and ONLY that box. Pass C
  // reads one contiguous X run per (cy, cz), so its cost is linear in the cy
  // band it has to cover — and with 200 m of relief the WORLD's hMin..hMax is
  // an order of magnitude wider than the 96-voxel box it actually reads. Using
  // the global range there cost 150 readbacks for a box that needs 40.
  {
    o.boxMin = INT32_MAX;
    o.boxMax = INT32_MIN;
    for (int z = kVoxLo; z <= kVoxHi; z++)
      for (int x = kVoxLo; x <= kVoxHi; x++) {
        const int h = World::TerrainHeight(x, z, seed);
        o.boxMin = std::min(o.boxMin, h);
        o.boxMax = std::max(o.boxMax, h);
      }
  }

  // A5 — treeline bracketing. Against the FAR transects' band, not the home
  // region's: since the world map (P4) the ground within 2 km of the origin
  // is whatever the map paints there -- flat forest by design, with the
  // snowline where alpine is painted -- so "the treeline is inside the
  // surface band around spawn" stopped being a property of a sane world.
  // What still must hold is that the treeline is reachable SOMEWHERE the
  // transects see: below it there are trees, above it there is snow.
  {
    const int treeline = worldmap::CurrentTerrain().treeline;
    if (treeline <= o.farMin || treeline >= o.farMax) {
      o.ok = false;
      o.why += Format("%streeline y%d outside the far-transect surface band "
                      "y%d..y%d (a world with no trees, or nothing but)",
                      o.why.empty() ? "" : "; ", treeline, o.farMin, o.farMax);
    }
  }

  // A7 — noise-cell range guard. The old form of this checked 255*cs^2 against
  // 2^31, because the legacy vnoise multiplied by cs^2 and crossed INT32_MAX at
  // a 2901-voxel cell — an overflow that is UB in C++ and defined wraparound in
  // WGSL, i.e. a silent, seed-dependent CPU/GPU desync. The terrain octaves
  // moved to vnoise2d, whose cell is a LOG2 SHIFT: there is no cs^2 left to
  // overflow, and the failure mode became a range one instead. Below 3 the
  // Q15 in-cell fraction has no bits left; above 15 q15frac shifts DOWN and the
  // field goes blocky. LoadWorldMap clamps the map's terrain to that window
  // (P-G: the cells are map.json `terrain`); this asserts the clamp is
  // actually reaching the four live octaves.
  {
    const worldmap::TerrainParams& w = worldmap::CurrentTerrain();
    const struct { const char* name; int log2; } cells[] = {
        {"rangeLog2", w.rangeLog2},
        {"hillLog2", w.hillLog2},
        {"detailLog2", w.detailLog2},
        {"grainLog2", w.grainLog2},
    };
    for (const auto& c : cells) {
      if (c.log2 < 3 || c.log2 > 15) {
        o.ok = false;
        o.why += Format("%sterrain.%s = %d is outside vnoise2d's 3..15 log2 "
                        "window (LoadWorldMap is supposed to clamp it)",
                        o.why.empty() ? "" : "; ", c.name, c.log2);
      }
    }
  }

  // A8 — RUIN PADS. Pass C1's readback box can never hold a ruin (worldgen
  // skips tile (0,0)), so a pad rule added to landColumn and not mirrored into
  // World::TerrainHeight would pass every other assertion in this file. This
  // is the CPU half of that proof: find the nearest accepted site, and assert
  // its footprint is FLAT and its apron never exceeds the angle of repose.
  //
  // "No site within the scan" is a FAILURE, not a skip. A predicate that
  // silently finds nothing is the circular assertion CLAUDE.md warns about —
  // it would go on passing after the feature was deleted.

  if (log)
    *log = Format(
        "surface y%d..y%d (mean %.0f, p5 %.0f, p95 %.0f, relief %d vox = %.1f m)"
        " | far transects y%d..y%d (%.1f m) | pass-C box y%d..y%d"
        " | local relief over 512 vox: %d (window is %u) | adjacent step max %d "
        "p99.9 %.0f | spawn-transect max step %d | tarns: %d bowl cols, %d berm "
        "cols checked",
        o.hMin, o.hMax, o.hMean, o.hP5, o.hP95, o.hMax - o.hMin,
        (double)(o.hMax - o.hMin) * kVoxelMeters, o.farMin, o.farMax,
        (double)(o.farMax - o.farMin) * kVoxelMeters, o.boxMin, o.boxMax,
        o.localRelief, kWorldN,
        o.slopeMax, o.slopeP999, o.rampMax, o.pondCols, o.bermCols);
  return o;
}

// ---------------------------------------------------------------------------
// Pass B — whole-window CPU/GPU sanity from the occupancy snapshot. Free.
// ---------------------------------------------------------------------------
// Per chunk-column, the topmost chunk holding anything, against the chunk the
// mirror says the ground is in. Slack is asymmetric on purpose: flora, trees
// and ruins legitimately stack chunks ABOVE the ground and nothing may sit
// BELOW it, so a column reading LOW is always a real divergence while a column
// reading high may just be a tall oak.
//
// AND THE OAKS ARE TALL. worldgen.wgsl's tree table is authored in decimetres
// and converted with `VOX_PER_M = 16`, while the world is 10 voxels to the
// metre — so every tree is 1.6x the metre size its own table documents, and a
// "11.9 m" great oak is 190 voxels of trunk. TREE_MAX_ABOVE is ~278 voxels,
// which is 18 chunks. That is where terrain.chunkColSlack's value comes from;
// tightening it to something that looks reasonable (3 was the first guess)
// reports 73 false divergences on stock HEAD.
int PassB(World& world, uint32_t seed, int slackAbove, std::string* worst) {
  const WorldSnapshot& snap = world.Snap();
  if (!snap.valid || snap.occupancy.size() < kNumSlots) {
    if (worst) *worst = "no snapshot";
    return -1;
  }
  const IVec3 org = world.WindowOrigin();
  int bad = 0, worstDelta = 0;
  std::string worstAt;
  for (int cz = 0; cz < (int)kNChunk; cz++) {
    for (int cx = 0; cx < (int)kNChunk; cx++) {
      const int wcx = org.x + cx, wcz = org.z + cz;
      int top = -1000;
      for (int cy = (int)kNChunk - 1; cy >= 0; cy--) {
        const uint32_t s = World::SlotChunkIndex({wcx, org.y + cy, wcz});
        if ((snap.occupancy[s] & 0xFFFFu) != 0u) { top = org.y + cy; break; }
      }
      const int mirror =
          World::TerrainHeight(wcx * (int)kChunk + 8, wcz * (int)kChunk + 8, seed) >> 4;
      // A chunk-column whose ground the mirror puts OUTSIDE the window has
      // nothing to compare against — that is pass A1's failure, not this one's,
      // and double-reporting it would bury A1's diagnosis under 1024 lines.
      if (mirror < org.y || mirror >= org.y + (int)kNChunk) continue;
      if (top == -1000) {
        // Empty column where the mirror says there is ground: always a real
        // divergence, and the direction that cannot be explained by flora.
        bad++;
        if (worstAt.empty())
          worstAt = Format("chunk-col (%d,%d): entirely empty, mirror says y%d",
                           wcx, wcz, mirror);
        continue;
      }
      const int delta = top - mirror;
      if (delta < 0 || delta > slackAbove) {
        bad++;
        if (std::abs(delta) > std::abs(worstDelta)) {
          worstDelta = delta;
          worstAt = Format("chunk-col (%d,%d): top chunk y%d, mirror y%d",
                           wcx, wcz, top, mirror);
        }
      }
    }
  }
  if (worst) *worst = worstAt;
  return bad;
}

// ---------------------------------------------------------------------------
// Pass C — targeted voxel readback.
// ---------------------------------------------------------------------------
// Which materials count as TERRAIN BODY, i.e. the thing TerrainHeight claims to
// describe. Resolved by NAME against the live material table rather than by
// hardcoded id, because ids are assigned by load order and this file must not
// become a second place that has to agree about them.
//
// C1 IS NOT "FIND THE TOPMOST BODY VOXEL AND COMPARE". That was the first
// formulation and it is wrong in two ways that stock HEAD demonstrates:
//
//   * `flowerAt` places a one-cell grass TUFT at y == h+1, and it uses the same
//     material id as the grass SKIN at y == h. No search over materials can
//     tell those two apart, so 53 perfectly correct meadow columns read as
//     divergences.
//   * the fixture pads keep a LOOSE SAND cap on purpose ("avalanches into
//     repose piles"), so a single CA tick legitimately moves grains upward by
//     one at a pile's shoulder — another 34 false positives.
//
// So test the mirror's actual CLAIM instead, which is also the exact statement
// of the hazard this gate exists for — "a player falls through ground they can
// see":
//
//   GROUND EXISTS:   voxel(x, h, z) and voxel(x, h-1, z) are both body matter.
//                    A mirror reading HIGH means the game thinks there is
//                    ground where the world has air. That is the dangerous
//                    direction and it has zero tolerance.
//   NOTHING BURIES IT: voxel(x, h+2, z) is not BULK body matter. Bulk excludes
//                    grass (the tuft) and sand/snow (which settle), so a
//                    one-cell plant or a shifted grain is allowed while a metre
//                    of stone above the reported surface is not.
struct BodyMask {
  std::vector<uint8_t> body;   // counts as terrain
  std::vector<uint8_t> bulk;   // terrain that never sits one cell above ground
};

BodyMask BuildBodyMask(const std::vector<MaterialDef>& mats) {
  static const char* kBody[] = {"stone", "dirt", "sand", "grass",
                                "snow",  "mud",  "gravel", "sandstone"};
  static const char* kBulk[] = {"stone", "dirt", "mud", "gravel"};
  BodyMask m;
  m.body.assign(mats.size(), 0);
  m.bulk.assign(mats.size(), 0);
  for (size_t i = 0; i < mats.size(); i++) {
    for (const char* n : kBody) if (mats[i].name == n) m.body[i] = 1;
    for (const char* n : kBulk) if (mats[i].name == n) m.bulk[i] = 1;
  }
  return m;
}

struct PassCOut {
  bool ok = true;
  int cols = 0;
  int hollow = 0;    // mirror says ground, world says air — the dangerous way
  int buried = 0;    // bulk terrain well above the reported surface
  std::string hollowWhy, buriedWhy;
  std::map<uint16_t, int> matHist;   // what was found where ground should be
  int fixturesBlocked = 0;
  std::string fixtureWhy;
  int liquidFaces = 0;
  std::string liquidWhy;
};

PassCOut PassC(GpuContext& ctx, World& world,
               const std::vector<MaterialDef>& mats, uint32_t seed,
               const PassAOut& a, int* readCalls) {
  PassCOut o;
  const BodyMask mask = BuildBodyMask(mats);
  const std::vector<uint8_t>& body = mask.body;
  const std::vector<uint8_t>& bulk = mask.bulk;
  const IVec3 org = world.WindowOrigin();

  // The cy band to read: everything the mirror says the ground could be in,
  // plus headroom for the fixture-clearance check above it and a little below
  // for the liquid check. Pass B independently validated that the mirror is in
  // the right neighbourhood, so narrowing to it is not circular.
  const int cyLo = std::max(org.y, (a.boxMin - 8) >> 4);
  const int cyHi =
      std::min(org.y + (int)kNChunk - 1, (a.boxMax + kClearAbove + 8) >> 4);
  const int cxLo = kVoxLo >> 4, cxHi = kVoxHi >> 4;
  const int czLo = kVoxLo >> 4, czHi = kVoxHi >> 4;
  const int runLen = cxHi - cxLo + 1;

  // ONE CONTIGUOUS X RUN PER (cy, cz). That is the whole readback budget, and
  // it is why the footprint is shaped this way: SlotChunkIndex is contiguous in
  // cx, so a row of chunks along X is a single call while a vertical stack
  // would be one call per chunk at stride kNChunk.
  std::vector<uint32_t> run((size_t)runLen * kChunkVol, 0);
  int curCy = INT32_MIN, curCz = INT32_MIN;
  int calls = 0;
  auto ensureRun = [&](int cy, int cz) {
    if (cy == curCy && cz == curCz) return;
    ReadVoxelsSync(ctx, world, World::SlotChunkIndex({cxLo, cy, cz}),
                   (uint32_t)runLen, run.data(), "terrain");
    curCy = cy;
    curCz = cz;
    calls++;
  };
  // Word at a world cell. Loads the run it belongs to first, so a caller can
  // never read a stale buffer — the bug this shape exists to make impossible.
  auto wordAt = [&](int x, int y, int z) -> uint32_t {
    ensureRun(y >> 4, z >> 4);
    const size_t chunk = (size_t)((x >> 4) - cxLo);
    const uint32_t local = ((uint32_t)(z & 15) * kChunk + (uint32_t)(y & 15)) *
                               kChunk + (uint32_t)(x & 15);
    return run[chunk * kChunkVol + local];
  };
  auto blocks = [&](uint32_t m) {
    // "Would this stop a falling body" — the COLLISION question, not the class
    // question: passable vegetation is a solid that bodies go straight through.
    if (m == 0 || m >= mats.size()) return false;
    const uint32_t k = mats[m].gpu.klass;
    if (k != CLASS_SOLID && k != CLASS_POWDER) return false;
    return (mats[m].gpu.flags & kMatFlagPassable) == 0;
  };

  const int nx = kVoxHi - kVoxLo + 1;
  // The four voxels the mirror's claim is about, gathered during the sweep:
  // h-1, h, h+2, and the material actually found at h.
  std::vector<uint8_t> atBelow((size_t)nx * nx, 0), atH((size_t)nx * nx, 0),
      atAbove2((size_t)nx * nx, 0);
  std::vector<uint16_t> matH((size_t)nx * nx, 0);

  // Iterate cz outer, cy inner so each run is touched exactly once.
  for (int cz = czLo; cz <= czHi; cz++) {
    for (int cy = cyLo; cy <= cyHi; cy++) {
      ensureRun(cy, cz);
      const int y0 = cy << 4, z0 = cz << 4;
      for (int lz = 0; lz < (int)kChunk; lz++) {
        const int z = z0 + lz;
        if (z < kVoxLo || z > kVoxHi) continue;
        for (int x = kVoxLo; x <= kVoxHi; x++) {
          const size_t ci = (size_t)(z - kVoxLo) * nx + (size_t)(x - kVoxLo);
          const int h = World::TerrainHeight(x, z, seed);
          for (int ly = 0; ly < (int)kChunk; ly++) {
            const int y = y0 + ly;
            const size_t chunk = (size_t)((x >> 4) - cxLo);
            const uint32_t local =
                ((uint32_t)lz * kChunk + (uint32_t)ly) * kChunk +
                (uint32_t)(x & 15);
            const uint32_t m = run[chunk * kChunkVol + local] & 0xFFFu;
            if (m == 0 || m >= mats.size()) continue;
            if (y == h - 1 && body[m]) atBelow[ci] = 1;
            if (y == h && body[m]) { atH[ci] = 1; matH[ci] = (uint16_t)m; }
            if (y >= h + 2 && bulk[m]) atAbove2[ci] = 1;
            // C3 — a liquid must never share a face with lava. Only the +x/+y
            // faces are tested here (both inside this run); +z would cross into
            // the next cz run and reloading mid-scan would thrash the readback
            // budget. Every x/y pair is still seen exactly once, and a
            // world-sized front — which is the failure this guards — cannot be
            // z-aligned only.
            if (mats[m].gpu.klass == CLASS_LIQUID) {
              const bool hot = mats[m].name == "lava";
              const int nb[2][2] = {{x + 1, y}, {x, y + 1}};
              for (const auto& n : nb) {
                if (n[0] > kVoxHi || (n[1] >> 4) != cy) continue;
                const size_t c2 = (size_t)((n[0] >> 4) - cxLo);
                const uint32_t l2 =
                    ((uint32_t)lz * kChunk + (uint32_t)(n[1] & 15)) * kChunk +
                    (uint32_t)(n[0] & 15);
                const uint32_t m2 = run[c2 * kChunkVol + l2] & 0xFFFu;
                if (m2 == 0 || m2 >= mats.size()) continue;
                if (mats[m2].gpu.klass != CLASS_LIQUID) continue;
                if (hot == (mats[m2].name == "lava")) continue;
                o.liquidFaces++;
                if (o.liquidWhy.empty())
                  o.liquidWhy = Format("%s at (%d,%d,%d) touches %s",
                                       mats[m].name.c_str(), x, y, z,
                                       mats[m2].name.c_str());
              }
            }
          }
        }
      }
    }
  }

  // C1 — the mirror's claim, tested directly. See BuildBodyMask for why this is
  // not a "topmost voxel" search.
  for (int z = kVoxLo; z <= kVoxHi; z++) {
    for (int x = kVoxLo; x <= kVoxHi; x++) {
      const size_t ci = (size_t)(z - kVoxLo) * nx + (size_t)(x - kVoxLo);
      const int h = World::TerrainHeight(x, z, seed);
      // Only columns whose h-1..h+2 window was actually read.
      if ((h - 1) >> 4 < cyLo || (h + 2) >> 4 > cyHi) continue;
      o.cols++;
      o.matHist[matH[ci]]++;
      if (!atH[ci] || !atBelow[ci]) {
        o.hollow++;
        if (o.hollowWhy.empty())
          o.hollowWhy = Format(
              "(%d,%d): mirror says ground at y%d but the world has %s there "
              "and %s below it",
              x, z, h, atH[ci] ? "terrain" : "no terrain",
              atBelow[ci] ? "terrain" : "no terrain");
      }
      if (atAbove2[ci]) {
        o.buried++;
        if (o.buriedWhy.empty())
          o.buriedWhy = Format("(%d,%d): bulk terrain at y>=%d, %d above the "
                               "reported surface y%d",
                               x, z, h + 2, 2, h);
      }
    }
  }
  if (o.hollow || o.buried) o.ok = false;

  // C2 — fixture clearance.
  for (const FixtureCol& f : kFixtures) {
    if (f.x < kVoxLo || f.x > kVoxHi || f.z < kVoxLo || f.z > kVoxHi) continue;
    const int h = World::TerrainHeight(f.x, f.z, seed);
    for (int y = h + 1; y <= h + kClearAbove; y++) {
      if ((y >> 4) < cyLo || (y >> 4) > cyHi) continue;
      const uint32_t m = wordAt(f.x, y, f.z) & 0xFFFu;
      if (!blocks(m)) continue;
      o.fixturesBlocked++;
      if (o.fixtureWhy.empty())
        o.fixtureWhy = Format("%s column (%d,%d): %s at y%d, %d above ground",
                              f.who, f.x, f.z, mats[m].name.c_str(), y, y - h);
      break;
    }
  }
  if (o.fixturesBlocked) o.ok = false;
  if (o.liquidFaces) o.ok = false;
  if (readCalls) *readCalls = calls;
  return o;
}

// ---- WHICH FIELD OF A WORD MOVED -------------------------------------------
//
// Pass D's per-word diff, factored so `gen-settle` below attributes its
// modified chunks with the SAME machinery rather than a second copy of it.
// A pond soaking into its bed, a grain creeping down a slope and a stamp
// churning all read as "N words changed"; they are three different bugs, and
// the fix for one is not the fix for another — so a diff says WHICH FIELD
// (material / state nibble / stain) and WHICH MATERIAL, not just how many.
//
// `mask` is applied to both words first: pass D compares whole words (a stamp
// churn is still activity), gen-settle compares under kPersistMask (only what
// a save would store is a reason to store it).
struct WordDiff {
  int moved = 0, matChanged = 0, stateChanged = 0, stainChanged = 0;
  std::map<uint32_t, int> delta;    // material -> net count change
  std::map<uint32_t, int> touched;  // material of a word that changed in place

  bool Add(uint32_t before, uint32_t after, uint32_t mask = 0xFFFFFFFFu) {
    before &= mask;
    after &= mask;
    if (before == after) return false;
    const uint32_t a = before & 0xFFFu, b = after & 0xFFFu;
    moved++;
    if (a != b) matChanged++;
    if (((before >> 12) & 0xF) != ((after >> 12) & 0xF)) stateChanged++;
    if ((before & 0x7F000000u) != (after & 0x7F000000u)) stainChanged++;
    if (a == b) touched[a]++;
    delta[a]--;
    delta[b]++;
    return true;
  }

  // " sand~3 water-12 air+12": in-place changes first, then net counts.
  std::string What(const std::vector<MaterialDef>& mats) const {
    std::string what;
    for (auto& kv : touched) {
      const char* nm = kv.first < mats.size() ? mats[kv.first].name.c_str()
                                              : "?";
      what += Format(" %s~%d", nm, kv.second);
    }
    for (auto& kv : delta) {
      if (kv.second == 0) continue;
      what += Format(" %s%+d",
                     kv.first == 0 ? "air"
                     : kv.first < mats.size() ? mats[kv.first].name.c_str()
                                              : "?",
                     kv.second);
    }
    return what;
  }
};

// ---------------------------------------------------------------------------
Status GateTerrain(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const uint32_t seed = kDefaultSeed;

  // Fresh procgen at the origin. This gate runs FIRST in kOrder and must leave
  // the window where `determinism` expects it, which is where it already is.
  SubmitWorldgen(ctx, world, sim, seed);

  std::string aLog;
  const PassAOut a = PassA(world, seed, &aLog);
  std::printf("terrain: %s\n", aLog.c_str());
  if (!a.ok) std::printf("terrain:   A FAILED: %s\n", a.why.c_str());

  // PASS C RUNS BEFORE ANY TICK, on purpose. It compares the world against the
  // procgen mirror, so it has to see PROCGEN — one tick of CA is enough to
  // shift a grain of the fixture pads' deliberately-loose sand cap and turn a
  // correct column into a reported divergence.
  int reads = 0;
  const PassCOut cc = PassC(ctx, world, c.mats, seed, a, &reads);
  std::printf("terrain: pass C %d columns checked, %d hollow (mirror claims "
              "ground where the world has air), %d buried; %d fixture columns "
              "blocked; %d liquid/lava faces (%d readbacks)\n",
              cc.cols, cc.hollow, cc.buried, cc.fixturesBlocked,
              cc.liquidFaces, reads);
  if (cc.hollow) std::printf("terrain:   C1 hollow: %s\n", cc.hollowWhy.c_str());
  if (cc.buried) std::printf("terrain:   C1 buried: %s\n", cc.buriedWhy.c_str());
  if (cc.hollow || cc.buried) {
    std::printf("terrain:   C1 material found at the mirror's height:");
    for (const auto& kv : cc.matHist)
      std::printf(" %s=%d",
                  kv.first == 0 ? "air"
                  : kv.first < c.mats.size() ? c.mats[kv.first].name.c_str()
                                             : "?",
                  kv.second);
    std::printf("\n");
  }
  if (cc.fixturesBlocked)
    std::printf("terrain:   C2: %s\n", cc.fixtureWhy.c_str());
  if (cc.liquidFaces)
    std::printf("terrain:   C3: %s\n", cc.liquidWhy.c_str());

  // ---- PASS B NEEDS A PUBLISHED SNAPSHOT, AND THE PUBLISH HAS A LATENCY ----
  //
  // This was ONE tick, because the harness drain used to make World::Snap()
  // arrive on the very tick that asked for it. It does not any more: the
  // snapshot is published at a FIXED World::kSnapshotLatency
  // (docs/PLAN_multiplayer_now.md N1), so for the first K ticks after a
  // worldgen reset there is no published snapshot AT ALL and pass B read an
  // invalid one and reported "-1 / no snapshot".
  //
  // THE FIXTURE IS WHAT WAS WRONG, not the pipeline. "Read the world one tick
  // after worldgen" is a statement about readback latency that this file had
  // baked in from the side that does not own it -- the same class of mistake
  // as hardcoding a fixture's world position instead of anchoring it to
  // WindowOrigin(). Ticking K + 1 times publishes the snapshot OF TICK 1: the
  // same single tick of CA pass B has always measured, with byte-identical
  // content. The pass asserts exactly what it did before and only sees it
  // later, which is why this cannot move a hash.
  //
  // The K + 1 ticks are taken OUT of pass D's budget below, so the world still
  // sees exactly 121 ticks between worldgen and the settle count.
  constexpr uint32_t kSnapWarmup = World::kSnapshotLatency + 1;
  for (uint32_t t = 1; t <= kSnapWarmup; t++)
    SubmitTick(ctx, world, sim, t, seed, {}, {}, {}, false, {0, 0, 0}, true,
               false);
  ctx.WaitIdle();
  ctx.ProcessEvents();

  std::string bWorst;
  const int bSlack = (int)BaselineNumber("terrain.chunkColSlack", 3);
  const int bBad = PassB(world, seed, bSlack, &bWorst);
  const int bCap = (int)BaselineNumber("terrain.chunkColBadMax", 0);
  const bool bOk = bBad >= 0 && bBad <= bCap;
  // A COUNT OF -1 IS NOT A MEASUREMENT (CLAUDE.md rule 6). The pass has two
  // distinct failures -- columns that genuinely disagree, and "I could not
  // look" -- and reporting the second as "-1/1024 chunk-columns disagree"
  // sends the reader hunting for a worldgen bug that is not there. Both still
  // FAIL; only one of them is about terrain.
  if (bBad < 0)
    std::printf("terrain: pass B COULD NOT RUN (%s) -- it reads World::Snap(), "
                "which is published at a FIXED latency of %u ticks "
                "(World::kSnapshotLatency), so a fixture must tick at least "
                "that many times after a world reset before it looks. This is "
                "a fixture bug, not a terrain one.\n",
                bWorst.empty() ? "no snapshot" : bWorst.c_str(),
                World::kSnapshotLatency);
  else
    std::printf("terrain: pass B %d/%u chunk-columns disagree with the mirror "
                "(slack +%d, cap %d)%s%s\n",
                bBad, kNChunk * kNChunk, bSlack, bCap,
                bWorst.empty() ? "" : " | worst ", bWorst.c_str());

  // Pass D — settle proxy. Advisory bound: `sleep` is the real gate, this is
  // the two-second version of the same question so a bad fill rule is caught
  // before anyone spends a minute finding out.
  uint32_t awake = 0;
  std::string awakeAt;
  {
    // FROM kSnapWarmup + 1, not from 2: pass B's warm-up above already spent
    // the first kSnapWarmup ticks on this world. The world still reaches tick
    // 121 with 121 ticks of CA behind it, which is what "settled after 120
    // ticks" has always meant here and what keeps this pass comparable with
    // every number recorded before the snapshot pipeline existed.
    for (uint32_t t = kSnapWarmup + 1; t < 122; t++)
      SubmitTick(ctx, world, sim, t, seed, {}, {}, {}, false, {0, 0, 0},
                 t == 121, false);
    ctx.WaitIdle();
    ctx.ProcessEvents();
    // WHERE, not just how many. A count alone sends you guessing at which fill
    // rule is the avalanche; the world coords plus the mirror's ground height
    // there name the feature in one line. `ca-skip` needs this number to be
    // ZERO (its skip latch waits for an empty dirty set), so a "small" residue
    // here is a gate failure sixty lines of output away.
    std::vector<uint32_t> flags(kNumSlots, 0);
    rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DirtyActive(), 0,
                          flags.data(), kNumSlots * 4, "terrainActive");
    const IVec3 org = world.WindowOrigin();
    // Report the SHALLOWEST awake chunks, not the first six in slot order.
    // Slot order is lz-major, so "the first six" is a scan line through the
    // window and says nothing; the chunks worth naming are the ones nearest the
    // surface, because that is where every worldgen fill rule puts matter.
    std::vector<std::pair<int, uint32_t>> awakeByDepth;
    for (uint32_t i = 0; i < kNumSlots; i++) {
      if (flags[i] == 0) continue;
      const IVec3 wc = world.SlotToWorldChunk(i);
      const int gh = World::TerrainHeight(wc.x * (int)kChunk + 8,
                                          wc.z * (int)kChunk + 8, seed);
      awakeByDepth.push_back({std::abs((gh >> 4) - wc.y), i});
    }
    std::sort(awakeByDepth.begin(), awakeByDepth.end());
    std::vector<uint8_t> pick(kNumSlots, 0);
    for (size_t k = 0; k < awakeByDepth.size() && k < 6; k++)
      pick[awakeByDepth[k].second] = 1;
    int deepest = awakeByDepth.empty() ? 0 : awakeByDepth.back().first;
    int shown = 0;
    // WHERE IN THE COLUMN, over ALL of them. Six named chunks tell you what is
    // moving; this tells you whether the thing that is moving is the SURFACE or
    // something buried, which is a different bug entirely and the one that
    // wastes the most time when it is not visible. Buckets are relative to the
    // ground: at or above it, the first chunk under it, and deeper.
    int atSurface = 0, justUnder = 0, buried = 0, aloft = 0;
    for (uint32_t i = 0; i < kNumSlots; i++) {
      if (flags[i] == 0) continue;
      awake++;
      const int lx = (int)(i % kNChunk);
      const int ly = (int)((i / kNChunk) % kNChunk);
      const int lz = (int)(i / (kNChunk * kNChunk));
      const int wx = (org.x + lx) * (int)kChunk, wy = (org.y + ly) * (int)kChunk,
                wz = (org.z + lz) * (int)kChunk;
      {
        const int gh = World::TerrainHeight(wx + 8, wz + 8, seed);
        const int d = (gh >> 4) - (wy >> 4);       // chunks below the ground
        if (d < 0) aloft++;
        else if (d == 0) atSurface++;
        else if (d == 1) justUnder++;
        else buried++;
      }
      if (!pick[i]) continue;
      shown++;
      // ...and WHAT. A chunk coordinate still leaves you guessing which fill
      // rule is the avalanche; the dominant material in it names the rule.
      // One readback per reported chunk, at most six, only when something is
      // still moving — free in the passing case, which is the whole point.
      std::vector<uint32_t> vox(kChunkVol, 0);
      ReadVoxelsSync(ctx, world, i, 1, vox.data(), "terrainAwake");
      std::map<uint32_t, int> hist;
      for (uint32_t w : vox)
        if ((w & 0xFFFu) != 0) hist[w & 0xFFFu]++;
      std::vector<std::pair<int, uint32_t>> by;
      for (auto& kv : hist) by.push_back({kv.second, kv.first});
      std::sort(by.rbegin(), by.rend());
      // The page-table entry, because "stone x4096" from a SENTINEL slot is not
      // a reading of anything: ReadVoxelsSync resolves a slot to a page, and a
      // sentinel has none, so the words come back from whatever page was last
      // recycled into that offset. A chunk reported as full of stone that is
      // actually an EMPTY sentinel sends you looking for an avalanche in solid
      // rock, which is exactly the hour this line exists to save.
      awakeAt += Format(" (%d,%d,%d h%d pt%s:", wx, wy, wz,
                        World::TerrainHeight(wx + 8, wz + 8, seed),
                        world.PageOffsetOfSlot(i) == World::kNoPage
                            ? "SENTINEL" : "page");
      for (size_t k = 0; k < by.size() && k < 5; k++)
        awakeAt += Format(" %s x%d",
                          by[k].second < c.mats.size()
                              ? c.mats[by[k].second].name.c_str() : "?",
                          by[k].first);
      awakeAt += ")";
    }
    // ---- WHAT IS ACTUALLY MOVING ----
    // A material histogram of an awake chunk says what is IN it, which is not
    // the same question and sends you looking in the wrong place: every chunk
    // around a tarn is mostly water and stone whether the water is flowing or
    // the sand is. So tick 20 more and DIFF: the materials whose counts change
    // are the ones still in motion, and that is a one-line answer instead of an
    // afternoon of turning worldgen features off one at a time.
    if (awake) {
      std::map<uint32_t, std::vector<uint32_t>> before;
      for (size_t k = 0; k < awakeByDepth.size() && k < 12; k++) {
        const uint32_t s = awakeByDepth[k].second;
        before[s].assign(kChunkVol, 0);
        ReadVoxelsSync(ctx, world, s, 1, before[s].data(), "terrainMove0");
      }
      for (uint32_t t = 122; t < 142; t++)
        SubmitTick(ctx, world, sim, t, seed, {}, {}, {}, false, {0, 0, 0},
                   t == 141, false);
      ctx.WaitIdle();
      ctx.ProcessEvents();
      WordDiff wd;
      for (auto& kv : before) {
        std::vector<uint32_t> now(kChunkVol, 0);
        ReadVoxelsSync(ctx, world, kv.first, 1, now.data(), "terrainMove1");
        // THE WHOLE WORD, not just the material. A pond soaking into its own
        // sand bed changes the stain nibble and the liquid's fullness and
        // nothing else, so a material-only diff reports "nothing changed" for
        // a chunk that is very much still working — which is exactly the
        // wrong answer when the question is "why is this awake".
        for (size_t v = 0; v < kChunkVol; v++) wd.Add(kv.second[v], now[v]);
      }
      const int moved = wd.moved, matChanged = wd.matChanged,
                stateChanged = wd.stateChanged, stainChanged = wd.stainChanged;
      const std::string what = wd.What(c.mats);
      // ---- WHERE THE LOST VOXELS GO ----
      // A page fault is a sim kernel writing into a chunk that is still a
      // SENTINEL, and write reach is one cell — so the faulting chunk is always
      // a face neighbour of an awake one. Naming which neighbour, and which
      // sentinel it is, turns "58 voxels were lost somewhere" into a direction:
      // a fall into EMPTY sky underneath a water body is a leak, a write into
      // UNIFORM rock beside it is something else entirely.
      std::string faults;
      int nsent = 0;
      for (auto& kv : before) {
        const IVec3 wc = world.SlotToWorldChunk(kv.first);
        static const IVec3 dirs[6] = {{1,0,0},{-1,0,0},{0,1,0},
                                      {0,-1,0},{0,0,1},{0,0,-1}};
        for (const IVec3& d : dirs) {
          const IVec3 n{wc.x + d.x, wc.y + d.y, wc.z + d.z};
          const uint32_t e =
              world.PageEntryOfSlot(World::SlotChunkIndex(n));
          if ((e & kPtSentinelBit) == 0u) continue;
          nsent++;
          if (nsent > 4) continue;
          const uint32_t m = e & kPtMatMask;
          // ...and WHAT SITS ON THE FACE. Write reach is one cell, so only the
          // 256 voxels of the awake chunk touching this sentinel can fault into
          // it. Their materials name the rule.
          std::map<uint32_t, int> face;
          for (int b = 0; b < 16; b++)
            for (int a = 0; a < 16; a++) {
              int lx = a, ly = b, lz = a;
              if (d.x != 0) { lx = d.x > 0 ? 15 : 0; ly = a; lz = b; }
              else if (d.y != 0) { ly = d.y > 0 ? 15 : 0; lx = a; lz = b; }
              else { lz = d.z > 0 ? 15 : 0; lx = a; ly = b; }
              const uint32_t w =
                  kv.second[(size_t)((lz * 16 + ly) * 16 + lx)] & 0xFFFu;
              if (w) face[w]++;
            }
          std::string faceStr;
          for (auto& fk : face)
            faceStr += Format(" %s x%d",
                              fk.first < c.mats.size()
                                  ? c.mats[fk.first].name.c_str() : "?",
                              fk.second);
          faults += Format(" [%d,%d,%d %+d%+d%+d -> %s; face:%s]", wc.x * 16,
                           wc.y * 16, wc.z * 16, d.x, d.y, d.z,
                           m == 0 ? "EMPTY"
                           : m < c.mats.size()
                               ? Format("%s%s", c.mats[m].name.c_str(),
                                        (e & kPtJitterBit) ? "/jitter" : "")
                                     .c_str()
                               : "?",
                           faceStr.empty() ? " (empty)" : faceStr.c_str());
        }
      }
      awakeAt = Format(" | %d aloft, %d at the surface, %d one chunk under, "
                       "%d buried deeper (deepest %d chunks off the ground)"
                       " | over 20 more ticks %d voxels changed (%d material,"
                       " %d fullness/jitter, %d stain):%s"
                       " | %d sentinel neighbours:%s%s",
                       aloft, atSurface, justUnder, buried, deepest, moved,
                       matChanged, stateChanged, stainChanged,
                       what.empty() ? " nothing" : what.c_str(), nsent,
                       faults.c_str(), awakeAt.c_str());
    }
  }
  const int dCap = (int)BaselineNumber("terrain.settleProxyMax", 400);
  const bool dOk = (int)awake <= dCap;
  std::printf("terrain: pass D %u chunks still awake after 120 ticks "
              "(cap %d; `sleep` is the real gate)%s\n", awake, dCap,
              awakeAt.c_str());

  // WHAT THIS GATE MEASURED, for --rebaseline. These are the numbers a terrain
  // change is judged by, and recording them means a scale pass is a JSON diff
  // ("relief 54 -> 1364") rather than a hash that moved for reasons unknown.
  // Keys must already exist in tests/baseline.json or the write is skipped —
  // --rebaseline says so out loud when it is.
  RecordObserved("terrain.reliefVox", a.hMax - a.hMin);
  RecordObserved("terrain.surfaceMinY", a.hMin);
  RecordObserved("terrain.surfaceMaxY", a.hMax);
  RecordObserved("terrain.slopeMaxObserved", a.slopeMax);
  RecordObserved("terrain.rampMaxObserved", a.rampMax);
  RecordObserved("terrain.localReliefObserved", a.localRelief);
  RecordObserved("terrain.farReliefObserved", a.farMax - a.farMin);
  RecordObserved("terrain.settleProxyObserved", (double)awake);

  const bool ok = a.ok && bOk && cc.ok && dOk;
  detail = Format("relief %d vox (y%d..y%d, local %d), slope max %d, mirror "
                  "%d/%d columns sound (%d hollow, %d buried), %d fixtures "
                  "blocked, %d chunk-cols off, %u awake @120",
                  a.hMax - a.hMin, a.hMin, a.hMax, a.localRelief, a.slopeMax,
                  cc.cols - cc.hollow - cc.buried, cc.cols, cc.hollow,
                  cc.buried, cc.fixturesBlocked, bBad, awake);
  std::printf("terrain: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ===========================================================================
// gen-settle — TERRAIN DOES NOT CHANGE JUST BECAUSE SOMEONE LOOKED AT IT
// (docs/PLAN_save_system.md S2)
//
// The save is a delta from the seed: Stream::EvictSlots skips every chunk
// whose sticky `modified_` bit is clear, because genChunk reproduces it. So
// every chunk that comes out modified WITHOUT player input is save bytes that
// scale with the area EXPLORED rather than with what the player did. The
// number was an anecdote ("9% of the real pages on a leaving plane under
// --autofly-surface"); this gate makes it an attributed, gated number.
//
// TWO ARMS, both with no brush, spell, explosion or cell op:
//
//   SETTLE   fresh worldgen, N ticks standing still, then every slot with
//            `modified_` set is read back, the window is REGENERATED at the
//            same origin, and the pristine words are diffed against it.
//   TRAVEL   fresh worldgen, then the interest point moves +X one chunk every
//            T ticks for D window shifts. Every leaving plane is recorded
//            with the modified set EvictSlots is about to use (the sticky
//            flags OR the snapshot this Update folds), the store is read back
//            afterwards, and every stored chunk is diffed against a regenerated
//            window that contains it. Planes whose chunks came from the
//            initial worldgen and planes whose chunks were STREAMED IN are
//            reported apart: only the second is the steady state of flight.
//
// ATTRIBUTION (CLAUDE.md rule 6 — a count is not a measurement). Every
// modified chunk gets exactly one verdict:
//   changed:<cause>   its persisted words (kPersistMask) differ from genChunk's,
//                     named by the writer class that changed the most cells:
//                     powder-move, liquid-flow, liquid-level, powder/liquid
//                     swap, plant, reaction, gas, solid, jitter, stain.
//   stale-snapshot    no persisted change, and its FIRST modified flag came
//                     from a snapshot older than the refill that installed the
//                     chunk — i.e. the dirty flag belonged to the slot's
//                     PREVIOUS occupant (traversal only).
//   neighbour-wake    no persisted change, face-adjacent to a changed chunk.
//   woke-no-change    no persisted change and nothing changed next to it:
//                     the chunk was scheduled to act and did nothing net.
// ...plus depth below the mirror's ground and the top material transitions,
// which name the worldgen rule.
//
// Thresholds live in tests/baseline.json (genSettle.*), measured, so the
// number cannot get WORSE silently.
namespace gensettle {

enum Cause : int {
  kPowder, kLiquidFlow, kLiquidLevel, kSwap, kPlant, kReaction, kGas, kSolid,
  kJitter, kStain, kNumCauses
};
const char* const kCauseName[kNumCauses] = {
    "powder-move", "liquid-flow", "liquid-level", "powder/liquid-swap",
    "plant",       "reaction",    "gas",          "solid",
    "jitter",      "stain"};

bool IsPlant(uint32_t m, const std::vector<MaterialDef>& mats) {
  if (m == 0 || m >= mats.size()) return false;
  if (mats[m].gpu.flags & kMatFlagMicro) return true;
  for (const auto& t : mats[m].tags)
    if (t == "foliage") return true;
  return false;
}

// a, b already masked with kPersistMask and a != b.
int Classify(uint32_t a, uint32_t b, const std::vector<MaterialDef>& mats) {
  const uint32_t ma = a & 0xFFFu, mb = b & 0xFFFu;
  auto klass = [&](uint32_t m) {
    return m < mats.size() ? mats[m].gpu.klass : (uint32_t)CLASS_SOLID;
  };
  if (ma != mb) {
    if (IsPlant(ma, mats) || IsPlant(mb, mats)) return kPlant;
    if (ma == 0 || mb == 0) {
      switch (klass(ma ? ma : mb)) {
        case CLASS_POWDER: return kPowder;
        case CLASS_LIQUID: return kLiquidFlow;
        case CLASS_GAS: return kGas;
        default: return kSolid;
      }
    }
    const uint32_t ka = klass(ma), kb = klass(mb);
    if ((ka == CLASS_POWDER && kb == CLASS_LIQUID) ||
        (ka == CLASS_LIQUID && kb == CLASS_POWDER))
      return kSwap;
    if (ka == CLASS_POWDER && kb == CLASS_POWDER) return kPowder;
    if (ka == CLASS_LIQUID && kb == CLASS_LIQUID) return kLiquidFlow;
    return kReaction;
  }
  if (((a >> 12) & 0xFu) != ((b >> 12) & 0xFu))
    return klass(ma) == CLASS_LIQUID ? kLiquidLevel : kJitter;
  return kStain;
}

// One modified chunk, as read: its current and pristine words.
struct Sample {
  IVec3 wc;
  std::vector<uint32_t> now, gen;
  bool stale = false;  // first modified flag came from the previous occupant
  bool page = true;    // held a real page (not a sentinel) when read
};

enum Verdict : int { kChanged, kStale, kNeighbour, kIdle, kNumVerdicts };

// WHERE A DEPARTED GRAIN STOOD. "sand moved" names the material and not the
// rule; the column it left names the rule. For every powder-move cell that
// became AIR (a grain that left), record the column's biome, the ground's
// TRUE local slope there (central difference of World::TerrainHeight, which
// includes the detail octaves worldgen's analytic `Col.slope` does not see),
// in the same Q8 units looseCoverDepth tapers on (256 = 1 voxel per column =
// the CA's angle of repose), and the cell's height against that ground.
struct GrainProbe {
  uint32_t seed = 0;
  int sedSlope = 96;                     // the taper's start (map terrain)
  std::vector<std::string> biomeName;    // by engine id
  std::map<uint64_t, int> hCache;
  int H(int x, int z) {
    const uint64_t k = ((uint64_t)(uint32_t)x << 32) | (uint32_t)z;
    auto it = hCache.find(k);
    if (it != hCache.end()) return it->second;
    const int h = World::TerrainHeight(x, z, seed);
    hCache[k] = h;
    return h;
  }
};

struct Tally {
  int chunks = 0, pages = 0;
  int verdict[kNumVerdicts] = {};
  int primary[kNumCauses] = {};   // changed chunks by dominant cause
  long cells[kNumCauses] = {};    // changed cells by cause
  int depth[4] = {};              // changed chunks: aloft/surface/-1/buried
  std::map<std::pair<uint32_t, uint32_t>, long> pairs;  // a->b material
  WordDiff wd;
  // Departed grains (GrainProbe): biome, ground slope bucket (< sedSlope,
  // sedSlope..repose, repose..2x, steeper), height vs ground (below the
  // surface cell, the surface cell or one above it, higher).
  std::map<std::string, long> grainBiome;
  long grainSlope[4] = {}, grainDy[3] = {}, grains = 0;

  void Attribute(std::vector<Sample>& ss, const std::vector<MaterialDef>& mats,
                 GrainProbe& gp) {
    const uint32_t seed = gp.seed;
    std::vector<int> prim(ss.size(), -1);
    std::map<uint64_t, bool> changedAt;
    for (size_t i = 0; i < ss.size(); i++) {
      Sample& s = ss[i];
      chunks++;
      pages += s.page ? 1 : 0;
      int local[kNumCauses] = {};
      bool any = false;
      for (size_t v = 0; v < kChunkVol; v++) {
        const uint32_t a = s.gen[v] & kPersistMask, b = s.now[v] & kPersistMask;
        if (!wd.Add(a, b)) continue;
        any = true;
        const int k = Classify(a, b, mats);
        local[k]++;
        cells[k]++;
        if (k == kPowder && (b & 0xFFFu) == 0) {
          const int x = s.wc.x * (int)kChunk + (int)(v % kChunk);
          const int y = s.wc.y * (int)kChunk + (int)((v / kChunk) % kChunk);
          const int z = s.wc.z * (int)kChunk + (int)(v / (kChunk * kChunk));
          const int h = gp.H(x, z);
          const int gx = std::abs(gp.H(x + 1, z) - gp.H(x - 1, z));
          const int gz = std::abs(gp.H(x, z + 1) - gp.H(x, z - 1));
          const int q8 = std::max(gx, gz) * 128;  // central diff /2, in Q8
          grainSlope[q8 < gp.sedSlope ? 0 : q8 < 256 ? 1 : q8 < 512 ? 2 : 3]++;
          const int dy = y - h;
          grainDy[dy < -1 ? 0 : dy <= 1 ? 1 : 2]++;
          const uint32_t bi = World::MapBiomeAt(x, z, seed);
          grainBiome[bi < gp.biomeName.size() && !gp.biomeName[bi].empty()
                         ? gp.biomeName[bi]
                         : Format("biome%u", bi)]++;
          grains++;
        }
        if ((a & 0xFFFu) != (b & 0xFFFu)) pairs[{a & 0xFFFu, b & 0xFFFu}]++;
      }
      if (!any) continue;
      int best = 0;
      for (int k = 1; k < kNumCauses; k++)
        if (local[k] > local[best]) best = k;
      prim[i] = best;
      changedAt[World::PackChunkKey(s.wc)] = true;
      primary[best]++;
      const int gh = World::TerrainHeight(s.wc.x * (int)kChunk + 8,
                                          s.wc.z * (int)kChunk + 8, seed);
      const int d = (gh >> 4) - s.wc.y;
      depth[d < 0 ? 0 : d == 0 ? 1 : d == 1 ? 2 : 3]++;
    }
    static const IVec3 dirs[6] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                  {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    for (size_t i = 0; i < ss.size(); i++) {
      if (prim[i] >= 0) { verdict[kChanged]++; continue; }
      if (ss[i].stale) { verdict[kStale]++; continue; }
      bool nb = false;
      for (const IVec3& d : dirs) {
        const IVec3 n{ss[i].wc.x + d.x, ss[i].wc.y + d.y, ss[i].wc.z + d.z};
        if (changedAt.count(World::PackChunkKey(n))) { nb = true; break; }
      }
      verdict[nb ? kNeighbour : kIdle]++;
    }
  }

  // Ranked, one line: verdicts, then changed chunks by cause, then cells by
  // cause, then depth, then the top transitions by material name.
  std::string Line(const std::vector<MaterialDef>& mats) const {
    static const char* const vn[kNumVerdicts] = {
        "changed", "stale-snapshot", "neighbour-wake", "woke-no-change"};
    std::string o = Format("%d modified (%d real pages):", chunks, pages);
    std::vector<std::pair<int, int>> r;
    for (int v = 0; v < kNumVerdicts; v++) r.push_back({verdict[v], v});
    std::sort(r.rbegin(), r.rend());
    for (auto& p : r)
      if (p.first) o += Format(" %s %d", vn[p.second], p.first);
    std::vector<std::pair<long, int>> pc, cc;
    for (int k = 0; k < kNumCauses; k++) {
      if (primary[k]) pc.push_back({primary[k], k});
      if (cells[k]) cc.push_back({cells[k], k});
    }
    std::sort(pc.rbegin(), pc.rend());
    std::sort(cc.rbegin(), cc.rend());
    o += " | changed chunks by cause:";
    if (pc.empty()) o += " none";
    for (auto& p : pc) o += Format(" %s %ld", kCauseName[p.second], p.first);
    o += " | cells by cause:";
    if (cc.empty()) o += " none";
    for (auto& p : cc) o += Format(" %s %ld", kCauseName[p.second], p.first);
    o += Format(" | changed depth: %d aloft, %d surface, %d one under, %d "
                "buried",
                depth[0], depth[1], depth[2], depth[3]);
    std::vector<std::pair<long, std::pair<uint32_t, uint32_t>>> tp;
    for (auto& kv : pairs) tp.push_back({kv.second, kv.first});
    std::sort(tp.rbegin(), tp.rend());
    auto nm = [&](uint32_t m) -> std::string {
      return m == 0 ? "air" : m < mats.size() ? mats[m].name : "?";
    };
    o += " | top transitions:";
    if (tp.empty()) o += " none";
    for (size_t i = 0; i < tp.size() && i < 6; i++)
      o += Format(" %s->%s x%ld", nm(tp[i].second.first).c_str(),
                  nm(tp[i].second.second).c_str(), tp[i].first);
    if (grains) {
      std::vector<std::pair<long, std::string>> gb;
      for (auto& kv : grainBiome) gb.push_back({kv.second, kv.first});
      std::sort(gb.rbegin(), gb.rend());
      o += Format(" | %ld departed grains by biome:", grains);
      for (auto& p : gb) o += Format(" %s %ld", p.second.c_str(), p.first);
      o += Format(" ; ground slope: flat %ld, taper %ld, 1-2x repose %ld, "
                  ">2x repose %ld ; vs ground: below %ld, at %ld, above %ld",
                  grainSlope[0], grainSlope[1], grainSlope[2], grainSlope[3],
                  grainDy[0], grainDy[1], grainDy[2]);
    }
    return o;
  }
};

bool IsPage(World& world, uint32_t s) {
  return world.PageOffsetOfSlot(s) != World::kNoPage;
}

}  // namespace gensettle

Status GateGenSettle(Ctx& c, std::string& detail) {
  using namespace gensettle;
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  Stream& stream = c.stream;
  const uint32_t seed = kDefaultSeed;
  const int half = (int)kNChunk / 2;
  const uint32_t settleTicks =
      (uint32_t)BaselineNumber("genSettle.settleTicks", 300);
  const uint32_t travelShifts =
      (uint32_t)BaselineNumber("genSettle.travelShifts", 48);
  const uint32_t ticksPerChunk =
      (uint32_t)BaselineNumber("genSettle.travelTicksPerChunk", 8);
  GrainProbe gp;
  gp.seed = seed;
  gp.sedSlope = worldmap::CurrentWorldMap().terrain.sedSlope;
  {
    biomes::BiomeSet set;
    std::string blog;
    if (biomes::LoadBiomeSet(AssetDir(), c.mats, set, blog))
      for (const auto& b : set.biomes)
        if (b.index >= 0) {
          if ((size_t)b.index >= gp.biomeName.size())
            gp.biomeName.resize(b.index + 1);
          gp.biomeName[b.index] = b.name;
        }
  }
  if (!WorldEditLayer().Empty())
    std::printf("gen-settle: NOTE the authored edit layer is loaded (%zu "
                "chunks) — its chunks are modified by design and count here\n",
                WorldEditLayer().ChunkCount());

  auto regen = [&](IVec3 origin) {
    stream.OnRegen();
    world.SetWindowOrigin(origin);
    SubmitWorldgen(ctx, world, sim, seed);
    ctx.WaitIdle();
    ctx.ProcessEvents();
  };
  auto readSlot = [&](uint32_t s, std::vector<uint32_t>& out, const char* l) {
    out.assign(kChunkVol, 0);
    ReadVoxelsSync(ctx, world, s, 1, out.data(), l);
  };

  // ---- SETTLE ARM ---------------------------------------------------------
  regen({0, 0, 0});
  const IVec3 centre{half, half, half};
  for (uint32_t t = 1; t <= settleTicks; t++) {
    stream.Update(centre, t);
    SubmitTick(ctx, world, sim, t, seed, {}, {}, {}, false, centre, false,
               false);
  }
  ctx.WaitIdle();
  ctx.ProcessEvents();
  stream.Update(centre, settleTicks);  // fold the last published snapshot
  Tally settle;
  int settlePages = 0;
  {
    std::vector<Sample> ss;
    std::vector<uint32_t> slots;
    const std::vector<uint8_t>& mod = stream.ModifiedFlags();
    for (uint32_t s = 0; s < kNumSlots; s++) {
      if (IsPage(world, s)) settlePages++;
      if (!mod[s]) continue;
      slots.push_back(s);
      Sample x;
      x.wc = world.SlotToWorldChunk(s);
      x.page = IsPage(world, s);
      readSlot(s, x.now, "genSettleNow");
      ss.push_back(std::move(x));
    }
    regen({0, 0, 0});
    for (size_t i = 0; i < ss.size(); i++)
      readSlot(slots[i], ss[i].gen, "genSettleGen");
    settle.Attribute(ss, c.mats, gp);
  }
  std::printf("gen-settle: SETTLE %u ticks, no input, %d real pages resident "
              "| %s\n",
              settleTicks, settlePages, settle.Line(c.mats).c_str());

  // ---- TRAVEL ARM ---------------------------------------------------------
  regen({0, 0, 0});
  struct Plane {
    bool streamed = false;
    int pages = 0, modified = 0, modPages = 0;
  };
  std::vector<Plane> planes;
  struct Ev {
    IVec3 wc;
    bool page, stale, streamed;
  };
  std::vector<Ev> evicted;                          // modified evictions only
  std::vector<uint32_t> fillTick(kNumSlots, 0);     // 0 = initial worldgen
  std::vector<uint8_t> stale(kNumSlots, 0);
  std::vector<uint8_t> streamedSlot(kNumSlots, 0);  // slot holds a streamed chunk
  std::vector<uint8_t> prevMod(kNumSlots, 0);
  uint32_t t = 0, staleFlags = 0;
  IVec3 pc = centre;
  auto step = [&]() {
    const IVec3 o0 = world.WindowOrigin();
    const uint32_t sh0 = stream.ShiftCount();
    // What EvictSlots will see this Update: the sticky set OR the snapshot
    // the top of Update folds. A slot newly set by that fold whose snapshot
    // is not newer than the slot's refill is carrying its PREVIOUS
    // occupant's dirty flag (the latent-snapshot inheritance).
    const WorldSnapshot& sn = world.Snap();
    std::vector<uint8_t> willMod = stream.ModifiedFlags();
    if (sn.valid)
      for (uint32_t s = 0; s < kNumSlots; s++) {
        if (!sn.dirtyFlags[s]) continue;
        if (!willMod[s] && !prevMod[s] && sn.tick <= fillTick[s] &&
            fillTick[s] != 0) {
          stale[s] = 1;
          staleFlags++;
        }
        willMod[s] = 1;
      }
    // The plane leaving on a +X shift is x == o0.x; capture its residency now,
    // before the refill replaces the page table entries.
    std::vector<uint8_t> planePage(kNChunk * kNChunk, 0);
    for (int u = 0; u < (int)kNChunk; u++)
      for (int v = 0; v < (int)kNChunk; v++)
        planePage[u * kNChunk + v] = IsPage(
            world, World::SlotChunkIndex({o0.x, o0.y + u, o0.z + v})) ? 1 : 0;
    stream.Update(pc, t);
    if (stream.ShiftCount() != sh0) {
      const IVec3 o1 = world.WindowOrigin();
      if (o1.x != o0.x + 1 || o1.y != o0.y || o1.z != o0.z)
        std::printf("gen-settle: WARNING unexpected shift (%d,%d,%d)->(%d,%d,%d)\n",
                    o0.x, o0.y, o0.z, o1.x, o1.y, o1.z);
      Plane p;
      for (int u = 0; u < (int)kNChunk; u++)
        for (int v = 0; v < (int)kNChunk; v++) {
          const IVec3 wc{o0.x, o0.y + u, o0.z + v};
          const uint32_t s = World::SlotChunkIndex(wc);
          const bool page = planePage[u * kNChunk + v] != 0;
          p.streamed = streamedSlot[s] != 0;
          p.pages += page;
          if (willMod[s]) {
            p.modified++;
            p.modPages += page;
            evicted.push_back({wc, page, stale[s] != 0, streamedSlot[s] != 0});
          }
          // The slot now holds the entering chunk (x = o0.x + kNChunk).
          fillTick[s] = t;
          stale[s] = 0;
          streamedSlot[s] = 1;
        }
      planes.push_back(p);
    }
    prevMod = stream.ModifiedFlags();
    SubmitTick(ctx, world, sim, ++t, seed, {}, {}, {}, false, pc, false,
               false);
  };
  for (uint32_t i = 0; stream.ShiftCount() < travelShifts &&
                       i < (travelShifts + 8) * ticksPerChunk;
       i++) {
    pc = {centre.x + (int)(i / ticksPerChunk), centre.y, centre.z};
    step();
  }
  // Drain: the real-page evictions are async readbacks harvested by Update.
  for (int k = 0; k < 32 && stream.PendingEvictions() > 0; k++) {
    ctx.WaitIdle();
    ctx.ProcessEvents();
    step();
  }
  ctx.WaitIdle();
  ctx.ProcessEvents();
  const size_t pendingLeft = stream.PendingEvictions();
  std::map<uint64_t, std::vector<uint32_t>> stored;
  stream.Store().ForEachStored(
      [&](IVec3 wc, const uint32_t* rle, size_t pairs) {
        stored[World::PackChunkKey(wc)].assign(rle, rle + pairs * 2);
      });
  // THE MODEL CHECK. The attribution is only as good as the claim that the
  // set recorded above IS the set EvictSlots stored; if they disagree, say so
  // and fail rather than attribute the wrong chunks.
  int notStored = 0;
  for (const Ev& e : evicted)
    if (!stored.count(World::PackChunkKey(e.wc))) notStored++;
  const int storeExtra = (int)stored.size() - ((int)evicted.size() - notStored);

  Tally travInit, travStream;
  {
    std::vector<Sample> ssI, ssS;
    std::vector<uint8_t> done(evicted.size(), 0);
    size_t left = evicted.size();
    while (left) {
      int ox = 1 << 30;
      for (size_t i = 0; i < evicted.size(); i++)
        if (!done[i]) ox = std::min(ox, evicted[i].wc.x);
      regen({ox, 0, 0});
      for (size_t i = 0; i < evicted.size(); i++) {
        if (done[i] || evicted[i].wc.x >= ox + (int)kNChunk) continue;
        done[i] = 1;
        left--;
        const Ev& e = evicted[i];
        auto it = stored.find(World::PackChunkKey(e.wc));
        if (it == stored.end()) continue;
        Sample x;
        x.wc = e.wc;
        x.page = e.page;
        x.stale = e.stale;
        x.now.assign(kChunkVol, 0);
        RleDecodeChunk(it->second.data(), it->second.size() / 2, x.now.data());
        readSlot(World::SlotChunkIndex(e.wc), x.gen, "genSettleTravelGen");
        (e.streamed ? ssS : ssI).push_back(std::move(x));
      }
    }
    travInit.Attribute(ssI, c.mats, gp);
    travStream.Attribute(ssS, c.mats, gp);
  }
  int nInit = 0, nStream = 0, pagesStream = 0, pagesInit = 0;
  for (const Plane& p : planes) {
    (p.streamed ? nStream : nInit)++;
    (p.streamed ? pagesStream : pagesInit) += p.pages;
  }
  const double perPlane = nStream ? (double)travStream.chunks / nStream : 0.0;
  const double changedPerPlane =
      nStream ? (double)travStream.verdict[kChanged] / nStream : 0.0;
  const double pctOfPages =
      pagesStream ? 100.0 * travStream.pages / pagesStream : 0.0;
  std::printf("gen-settle: TRAVEL %zu shifts +X, 1 chunk / %u ticks, %u ticks; "
              "%u stale-snapshot flags raised; store %zu chunks (%d recorded "
              "not stored, %d stored not recorded, %zu evictions pending)\n",
              planes.size(), ticksPerChunk, t, staleFlags, stored.size(),
              notStored, storeExtra, pendingLeft);
  std::printf("gen-settle:   initial-worldgen planes %d (%d real pages) | %s\n",
              nInit, pagesInit, travInit.Line(c.mats).c_str());
  std::printf("gen-settle:   STREAMED planes %d (%d real pages): %.1f modified "
              "/ plane = %.1f%% of real pages, %.1f changed / plane | %s\n",
              nStream, pagesStream, perPlane, pctOfPages, changedPerPlane,
              travStream.Line(c.mats).c_str());

  // Leave pristine worldgen at the origin, as the gates around this one expect.
  regen({0, 0, 0});

  const double sModCap = BaselineNumber("genSettle.settleModifiedMax", 1e9);
  const double sChgCap = BaselineNumber("genSettle.settleChangedMax", 1e9);
  const double tModCap = BaselineNumber("genSettle.travelModifiedPerPlaneMax", 1e9);
  const double tChgCap = BaselineNumber("genSettle.travelChangedPerPlaneMax", 1e9);
  const bool modelOk = notStored == 0 && storeExtra == 0 && pendingLeft == 0;
  const bool enough = nStream > 0;
  const bool ok = modelOk && enough && settle.chunks <= sModCap &&
                  settle.verdict[kChanged] <= sChgCap && perPlane <= tModCap &&
                  changedPerPlane <= tChgCap;
  RecordObserved("genSettle.settleModifiedObserved", (double)settle.chunks);
  RecordObserved("genSettle.settleChangedObserved",
                 (double)settle.verdict[kChanged]);
  RecordObserved("genSettle.travelModifiedPerPlaneObserved", perPlane);
  RecordObserved("genSettle.travelChangedPerPlaneObserved", changedPerPlane);
  RecordObserved("genSettle.travelPctOfRealPagesObserved", pctOfPages);

  detail = Format(
      "settle %u ticks: %s || travel streamed planes %d: %.1f modified/plane "
      "(%.1f%% of real pages, cap %.1f), %.1f changed/plane (cap %.1f): %s || "
      "initial planes %d: %s || caps settle modified %.0f changed %.0f%s%s",
      settleTicks, settle.Line(c.mats).c_str(), nStream, perPlane, pctOfPages,
      tModCap, changedPerPlane, tChgCap, travStream.Line(c.mats).c_str(), nInit,
      travInit.Line(c.mats).c_str(), sModCap, sChgCap,
      modelOk ? "" : " | MODEL CHECK FAILED (recorded != stored)",
      enough ? "" : " | NO STREAMED PLANE WAS EVICTED (travel too short)");
  std::printf("gen-settle: %s\n", ok ? "PASS" : "FAIL");
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& TerrainGates() {
  static const std::vector<Gate> g = {
      {"terrain", "sim", {}, false, GateTerrain},
      {"gen-settle", "worldio", {}, false, GateGenSettle},
  };
  return g;
}

}  // namespace selftest
