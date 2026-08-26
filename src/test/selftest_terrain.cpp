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

#include "sim/tuning.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

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
  const auto& w = CurrentTuning().worldgen;
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
  // core carries the guarantee. bermLift() in worldgen.wgsl defines it.
  const int core = std::max(w.pondBermWidth / 4, 2);
  if (!q.near || q.past >= core) return true;
  o.bermCols++;
  if (h < q.surf + w.pondBerm) {
    o.ok = false;
    o.why += Format("%sberm column (%d,%d) at %d past the rim: ground y%d is "
                    "under waterline y%d + berm %d — this tarn leaks",
                    o.why.empty() ? "" : "; ", x, z, q.past, h, q.surf,
                    w.pondBerm);
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
    const auto& w = CurrentTuning().worldgen;
    const int reach = w.pondRadiusMin + w.pondRadiusSpan + w.pondBermWidth + 4;
    for (auto [px, pz] : inPond) {
      for (int i = 0; i <= reach; i++) {
        const int x = px + i;
        const int h = World::TerrainHeight(x, pz, seed);
        if (!CheckPondColumn(x, pz, h, seed, o)) break;
        const World::PondQuery q = World::PondNearColumn(x, pz, seed);
        if (!q.inDisc && (!q.near || q.past >= w.pondBermWidth)) break;
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

  // A4 — ramp continuity across the spawn-region boundary. Today the spawn
  // clearing only suppresses FLORA, so this measures nothing and reports the
  // ambient slope. It goes live with the terrain flattening: a fade applied
  // over too short a distance shows up here as a step that dwarfs the ambient
  // one, and the whole point is that the fade width is READ off this number
  // rather than guessed. Walk out from the origin along +x, +z and both
  // diagonals, since a Chebyshev-shaped region has its steepest boundary on
  // the axes and its longest on the diagonal.
  {
    int worst = 0;
    for (int t = 0; t < 4; t++) {
      const int sx = (t == 0 || t == 2 || t == 3) ? 1 : 0;
      const int sz = (t == 1 || t == 2) ? 1 : (t == 3 ? -1 : 0);
      int prev = World::TerrainHeight(0, 0, seed);
      for (int i = 1; i <= 3072; i++) {
        const int h = World::TerrainHeight(i * sx, i * sz, seed);
        worst = std::max(worst, std::abs(h - prev));
        prev = h;
      }
    }
    o.rampMax = worst;
  }

  // A5 — treeline bracketing.
  {
    const int treeline = CurrentTuning().worldgen.treeline;
    if ((double)treeline <= o.hP5 || (double)treeline >= o.hP95) {
      o.ok = false;
      o.why += Format("%streeline y%d outside the p5..p95 surface band "
                      "y%.0f..y%.0f (a world with no trees, or nothing but)",
                      o.why.empty() ? "" : "; ", treeline, o.hP5, o.hP95);
    }
  }

  // A7 — noise-cell range guard. The old form of this checked 255*cs^2 against
  // 2^31, because the legacy vnoise multiplied by cs^2 and crossed INT32_MAX at
  // a 2901-voxel cell — an overflow that is UB in C++ and defined wraparound in
  // WGSL, i.e. a silent, seed-dependent CPU/GPU desync. The terrain octaves
  // moved to vnoise2d, whose cell is a LOG2 SHIFT: there is no cs^2 left to
  // overflow, and the failure mode became a range one instead. Below 3 the
  // Q15 in-cell fraction has no bits left; above 15 q15frac shifts DOWN and the
  // field goes blocky. LoadTuning clamps to that window; this asserts the clamp
  // is actually reaching these three knobs.
  {
    const auto& w = CurrentTuning().worldgen;
    const struct { const char* name; int log2; } cells[] = {
        {"hillLog2", w.hillLog2},
        {"detailLog2", w.detailLog2},
        {"biomeLog2", w.biomeLog2},
    };
    for (const auto& c : cells) {
      if (c.log2 < 3 || c.log2 > 15) {
        o.ok = false;
        o.why += Format("%sworldgen.%s = %d is outside vnoise2d's 3..15 log2 "
                        "window (LoadTuning is supposed to clamp it)",
                        o.why.empty() ? "" : "; ", c.name, c.log2);
      }
    }
  }

  if (log)
    *log = Format(
        "surface y%d..y%d (mean %.0f, p5 %.0f, p95 %.0f, relief %d vox = %.1f m)"
        " | local relief over 512 vox: %d (window is %u) | adjacent step max %d "
        "p99.9 %.0f | spawn-transect max step %d | tarns: %d bowl cols, %d berm "
        "cols checked",
        o.hMin, o.hMax, o.hMean, o.hP5, o.hP95, o.hMax - o.hMin,
        (double)(o.hMax - o.hMin) * kVoxelMeters, o.localRelief, kWorldN,
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
  if (!snap.valid || snap.occupancy.size() < kNumChunks) {
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
                                "snow",  "mud",  "gravel"};
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
  const int cyLo = std::max(org.y, (a.hMin - 8) >> 4);
  const int cyHi =
      std::min(org.y + (int)kNChunk - 1, (a.hMax + kClearAbove + 8) >> 4);
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

  // One tick with readback, purely to get the occupancy snapshot delivered
  // CPU-side (SetHarnessSnapshotDrain is on for the harness). Cheap, and pass B
  // is free once it lands.
  SubmitTick(ctx, world, sim, 1, seed, {}, {}, {}, false, {0, 0, 0}, true, false);
  ctx.WaitIdle();
  ctx.ProcessEvents();

  std::string bWorst;
  const int bSlack = (int)BaselineNumber("terrain.chunkColSlack", 3);
  const int bBad = PassB(world, seed, bSlack, &bWorst);
  const int bCap = (int)BaselineNumber("terrain.chunkColBadMax", 0);
  const bool bOk = bBad >= 0 && bBad <= bCap;
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
    for (uint32_t t = 2; t < 122; t++)
      SubmitTick(ctx, world, sim, t, seed, {}, {}, {}, false, {0, 0, 0},
                 t == 121, false);
    ctx.WaitIdle();
    ctx.ProcessEvents();
    // WHERE, not just how many. A count alone sends you guessing at which fill
    // rule is the avalanche; the world coords plus the mirror's ground height
    // there name the feature in one line. `ca-skip` needs this number to be
    // ZERO (its skip latch waits for an empty dirty set), so a "small" residue
    // here is a gate failure sixty lines of output away.
    std::vector<uint32_t> flags(kNumChunks, 0);
    rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DirtyActive(), 0,
                          flags.data(), kNumChunks * 4, "terrainActive");
    const IVec3 org = world.WindowOrigin();
    int shown = 0;
    for (uint32_t i = 0; i < kNumChunks; i++) {
      if (flags[i] == 0) continue;
      awake++;
      if (shown++ >= 6) continue;
      const int lx = (int)(i % kNChunk);
      const int ly = (int)((i / kNChunk) % kNChunk);
      const int lz = (int)(i / (kNChunk * kNChunk));
      const int wx = (org.x + lx) * (int)kChunk, wy = (org.y + ly) * (int)kChunk,
                wz = (org.z + lz) * (int)kChunk;
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
      awakeAt += Format(" (%d,%d,%d h%d:", wx, wy, wz,
                        World::TerrainHeight(wx + 8, wz + 8, seed));
      for (size_t k = 0; k < by.size() && k < 5; k++)
        awakeAt += Format(" %s x%d",
                          by[k].second < c.mats.size()
                              ? c.mats[by[k].second].name.c_str() : "?",
                          by[k].first);
      awakeAt += ")";
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

}  // namespace

const std::vector<Gate>& TerrainGates() {
  static const std::vector<Gate> g = {
      {"terrain", "sim", {}, false, GateTerrain},
  };
  return g;
}

}  // namespace selftest
