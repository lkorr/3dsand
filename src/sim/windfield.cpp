#include "sim/windfield.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "sim/intmath.h"
#include "sim/tuning.h"
#include "sim/weather.h"
#include "sim/windprim.h"
#include "sim/worldmap.h"
#include "test/support.h"  // AssetDir(): the one asset-path chokepoint

namespace windfield {

// ============================================================================
// THE ADVECTION CLOCK
// ============================================================================
//
// A(t) = sum over ticks of U(tick), U the reference mean speed in Q16.16 cells
// per second, so A / (30 * 65536) is cells travelled. Sampled once per
// kAdvBlock-tick block and held, which integrates a step function exactly —
// the sum is a pure function of (tuning, seed, tick) however it is reached.
//
// The memo is keyed on the seed and a FINGERPRINT of every knob the reference
// wind reads. Moving one of those knobs re-sums from tick 0: the gust pattern
// jumps once, when a slider moves, which is honest (the history of the wind
// genuinely changed) and cheap (a few thousand integer weather evaluations).

namespace {

// FNV-1a over explicit fields. Not a memcpy of the struct: Tuning::Wind holds
// a std::string, and hashing its bytes would hash a heap pointer and padding.
struct Fp {
  uint64_t h = 1469598103934665603ull;
  void Bytes(const void* p, size_t n) {
    const auto* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
  }
  void F(float v) { Bytes(&v, sizeof v); }
  void I(int64_t v) { Bytes(&v, sizeof v); }
};

uint64_t RefFingerprint(const Tuning& t) {
  Fp f;
  const Tuning::Wind& w = t.wind;
  f.F(w.windSpeed);
  f.F(w.windDirDeg);
  f.F(w.gustStrength);
  f.I(w.weatherAuto ? 1 : 0);
  f.F(w.gustWavelength);
  f.F(w.gustAdvect);
  f.Bytes(w.regime.data(), w.regime.size());
  f.F(w.intensity);
  f.F(w.moodSpread);
  f.F(w.speedCalmMul);
  f.F(w.speedGaleMul);
  f.F(w.speedMaxMul);
  f.F(w.galeHold);
  f.F(w.stormCycle);
  f.F(w.stormLull);
  f.F(w.stormFront);
  f.F(w.stormDecay);
  f.F(w.stormJump);
  f.I((int64_t)RegimeFingerprint());
  f.I((int64_t)weather::SimWindFingerprint(t));
  return f.h;
}

struct AdvMemo {
  uint64_t key = ~0ull;
  // At the START of block b: the speed sum and the displacement sums.
  std::vector<int64_t> a, dx, dz;
  // The reference sample held across block b.
  std::vector<int32_t> u, ux, uz;
};
std::map<uint32_t, AdvMemo> gAdv;  // one per seed (the renderer's and the sim's)

// The speed the fronts ride, Q16.16 cells/s, with gustAdvect applied, and the
// reference wind vector the air drifts by (no gustAdvect: that is a look knob
// for the bands, not a claim about the air).
void RefSample(const Tuning& t, uint32_t seed, uint32_t tick, int32_t& u,
               int32_t& ux, int32_t& uz) {
  const WindStateQ q = WindWeatherQ(t, seed, tick);
  const int64_t advQ =
      (int64_t)((double)std::clamp(t.wind.gustAdvect, 0.0f, 4.0f) * 65536.0 + 0.5);
  u = (int32_t)imath::MulShiftRound(q.speed, advQ, 16);
  ux = (int32_t)imath::MulShiftRound(q.speed, q.dirX, 16);
  uz = (int32_t)imath::MulShiftRound(q.speed, q.dirZ, 16);
}

AdvMemo& Memo(const Tuning& t, uint32_t seed, uint32_t block) {
  AdvMemo& m = gAdv[seed];
  const uint64_t key = RefFingerprint(t);
  if (m.key != key) {
    m = AdvMemo{};
    m.key = key;
    m.a.push_back(0);
    m.dx.push_back(0);
    m.dz.push_back(0);
  }
  while (m.u.size() <= block) {
    const uint32_t b = (uint32_t)m.u.size();
    int32_t u, ux, uz;
    RefSample(t, seed, b * kAdvBlock, u, ux, uz);
    m.u.push_back(u);
    m.ux.push_back(ux);
    m.uz.push_back(uz);
    m.a.push_back(m.a[b] + (int64_t)u * kAdvBlock);
    m.dx.push_back(m.dx[b] + (int64_t)ux * kAdvBlock);
    m.dz.push_back(m.dz[b] + (int64_t)uz * kAdvBlock);
  }
  return m;
}

// Sum at tick + frac. frac = 0 on the sim path, and then no float is involved.
int64_t SumAt(const std::vector<int64_t>& pre, const std::vector<int32_t>& s,
              uint32_t tick, float frac) {
  const uint32_t b = tick / kAdvBlock;
  int64_t v = pre[b] + (int64_t)s[b] * (int64_t)(tick - b * kAdvBlock);
  if (frac > 0.0f) v += (int64_t)((double)s[b] * (double)std::min(frac, 1.0f));
  return v;
}

}  // namespace

int32_t GustKBase(const Tuning& t) {
  // The shader's const-eval, in f32, operation for operation (WINDQ_K_BASE).
  const float cellsPerWave = std::max(t.wind.gustWavelength / 0.1f, 1.0f);
  const float k = std::nearbyint(65536.0f / cellsPerWave);
  return std::min((int32_t)k, 27000);
}

uint32_t AdvPhase(const Tuning& t, uint32_t seed, uint32_t tick, float frac) {
  AdvMemo& m = Memo(t, seed, tick / kAdvBlock);
  const int64_t a = SumAt(m.a, m.u, tick, frac);  // cells * 30 * 65536
  const int64_t den = 30ll * 65536ll;
  const int64_t cells = a / den, rem = a % den;
  const int64_t k = GustKBase(t);
  const int64_t turns10 = 655360;
  const int64_t ph = ((cells % turns10) * k + (rem * k) / den) % turns10;
  return (uint32_t)ph;
}

void AirDrift(const Tuning& t, uint32_t seed, uint32_t tick, float frac,
              double& dxM, double& dzM) {
  AdvMemo& m = Memo(t, seed, tick / kAdvBlock);
  const double toM = (double)kVoxelMeters / (30.0 * 65536.0);
  dxM = (double)SumAt(m.dx, m.ux, tick, frac) * toM;
  dzM = (double)SumAt(m.dz, m.uz, tick, frac) * toM;
}

// ============================================================================
// THE TERRAIN TABLE (docs/RESEARCH_wind.md §13.2)
// ============================================================================
//
// 64 x 64 cells of 32 voxels, centred on the window, each one word:
//   bits  0..15  surface height, i16 world Y (standing water's surface counts)
//   bits 16..23  exposure s, i8 x 127 = clamp(TPI / tpiScale, -1, 1)
//   bits 24..31  water fraction within seaRadius, u8 x 255
// stored TOROIDALLY: world cell (cx, cz) lives at slot (cx & 63, cz & 63), so
// a window shift leaves every surviving cell where it was.
//
// WHERE THE NUMBERS COME FROM. World::TerrainColumn is the CPU twin of the
// shader's genColumn (bit-exact, integer), so the table is a pure function of
// (seed, map, world cell, tuning) and never of the live grid. The fine cells
// sample it at their centres; the neighbourhood mean the TPI needs and the
// water fraction come from a COARSE lattice (128-voxel cells) summed with a
// prefix-sum box filter and interpolated bilinearly to the fine cell -- a
// 100 m neighbourhood averaged at 3.2 m resolution would be 60x the column
// queries for a number that is smooth by definition.
//
// COST. A cold build is 4,096 fine + ~1,200 coarse column queries; both are
// cached by world cell, so a window shift (which moves the base by at most one
// cell) queries one new row or column of each and re-runs the ~16k-add box
// filter. Integer throughout; the order of summation cannot matter.

namespace {

constexpr int32_t kCoarse = 128;  // coarse cell, voxels

int64_t CellKey(int32_t x, int32_t z) {
  return (int64_t)(((uint64_t)(uint32_t)x << 32) | (uint64_t)(uint32_t)z);
}

// Surface height (i16, water surface included) | water flag << 16.
int32_t SampleColumn(int32_t x, int32_t z, uint32_t seed) {
  const World::Column c = World::TerrainColumn(x, z, seed);
  int32_t surf = c.h;
  bool water = false;
  if (c.water != INT32_MIN && c.water > c.h) {
    surf = c.water;
    water = true;
  }
  surf = std::clamp(surf, -32768, 32767);
  return (surf & 0xFFFF) | (water ? 0x10000 : 0);
}
int32_t ColH(int32_t v) { return (int32_t)(int16_t)(v & 0xFFFF); }
int32_t ColW(int32_t v) { return (v >> 16) & 1; }

int32_t FloorDiv(int32_t a, int32_t b) {
  const int32_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

struct TerrState {
  uint64_t key = ~0ull;
  int32_t bx = INT32_MIN, bz = INT32_MIN;
  bool valid = false;
  uint32_t words[kWindTerrWords] = {};
  std::unordered_map<int64_t, int32_t> fine, coarse;
  // Readout: column queries the last rebuild paid.
  uint32_t lastQueries = 0;
};
TerrState gTerr;

uint64_t TerrKey(const Tuning& t, uint32_t seed) {
  Fp f;
  f.I(seed);
  f.I(World::LabWorld() ? 1 : 0);
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  f.Bytes(m.name.data(), m.name.size());
  f.I(m.seaLevelY);
  f.I(m.width);
  f.I(m.height);
  f.I(m.originCellX);
  f.I(m.originCellZ);
  f.F(t.wind.tpiRadius);
  f.F(t.wind.tpiScale);
  f.F(t.wind.seaRadius);
  return f.h;
}

int32_t Cached(std::unordered_map<int64_t, int32_t>& c, int32_t x, int32_t z,
               uint32_t seed, uint32_t& queries) {
  const int64_t k = CellKey(x, z);
  auto it = c.find(k);
  if (it != c.end()) return it->second;
  const int32_t v = SampleColumn(x, z, seed);
  queries++;
  c.emplace(k, v);
  return v;
}

void BuildTable(const Tuning& t, uint32_t seed, int32_t bx, int32_t bz) {
  TerrState& S = gTerr;
  if (S.fine.size() > 200000) S.fine.clear();
  if (S.coarse.size() > 50000) S.coarse.clear();
  uint32_t queries = 0;
  const int32_t N = (int32_t)kWindTerrN;

  // ---- fine heights -------------------------------------------------------
  static int32_t fineV[kWindTerrWords];
  for (int32_t j = 0; j < N; j++)
    for (int32_t i = 0; i < N; i++)
      fineV[j * N + i] = Cached(S.fine, (bx + i) * kWindTerrCell + 16,
                                (bz + j) * kWindTerrCell + 16, seed, queries);

  // ---- the coarse lattice -------------------------------------------------
  // Radii in coarse cells, rounded, at least one.
  auto cells = [](float metres) {
    const int32_t v = (int32_t)((double)metres * 10.0 / kCoarse + 0.5);
    return std::max(v, 1);
  };
  const int32_t rT = cells(t.wind.tpiRadius);
  const int32_t rW = cells(t.wind.seaRadius);
  const int32_t rMax = std::max(rT, rW);
  // Coarse lattice points (centres at 128i + 64) bracketing every fine centre.
  const int32_t lo0x = FloorDiv(bx * kWindTerrCell + 16 - 64, kCoarse);
  const int32_t hi0x = FloorDiv((bx + N - 1) * kWindTerrCell + 16 - 64, kCoarse) + 1;
  const int32_t lo0z = FloorDiv(bz * kWindTerrCell + 16 - 64, kCoarse);
  const int32_t hi0z = FloorDiv((bz + N - 1) * kWindTerrCell + 16 - 64, kCoarse) + 1;
  const int32_t gx0 = lo0x - rMax, gz0 = lo0z - rMax;
  const int32_t gw = (hi0x + rMax) - gx0 + 1, gh = (hi0z + rMax) - gz0 + 1;
  // Prefix sums, (gw+1) x (gh+1), of height and of the water flag.
  std::vector<int64_t> ph((size_t)(gw + 1) * (gh + 1), 0), pw(ph.size(), 0);
  auto at = [&](int32_t i, int32_t j) { return (size_t)j * (gw + 1) + i; };
  for (int32_t j = 0; j < gh; j++) {
    for (int32_t i = 0; i < gw; i++) {
      const int32_t v = Cached(S.coarse, (gx0 + i) * kCoarse + 64,
                               (gz0 + j) * kCoarse + 64, seed, queries);
      ph[at(i + 1, j + 1)] = ColH(v) + ph[at(i, j + 1)] + ph[at(i + 1, j)] - ph[at(i, j)];
      pw[at(i + 1, j + 1)] = ColW(v) + pw[at(i, j + 1)] + pw[at(i + 1, j)] - pw[at(i, j)];
    }
  }
  auto box = [&](const std::vector<int64_t>& P, int32_t i, int32_t j, int32_t r) {
    const int32_t a0 = i - r, a1 = i + r + 1, b0 = j - r, b1 = j + r + 1;
    return P[at(a1, b1)] - P[at(a0, b1)] - P[at(a1, b0)] + P[at(a0, b0)];
  };
  // Box means at the bracketing lattice points: height (whole voxels) and the
  // water fraction (x 255), both rounded to nearest.
  const int32_t lw = hi0x - lo0x + 1, lh = hi0z - lo0z + 1;
  std::vector<int32_t> meanH((size_t)lw * lh), meanW((size_t)lw * lh);
  const int64_t aT = (int64_t)(2 * rT + 1) * (2 * rT + 1);
  const int64_t aW = (int64_t)(2 * rW + 1) * (2 * rW + 1);
  for (int32_t j = 0; j < lh; j++) {
    for (int32_t i = 0; i < lw; i++) {
      const int32_t ci = lo0x + i - gx0, cj = lo0z + j - gz0;
      meanH[(size_t)j * lw + i] = (int32_t)imath::DivRound(box(ph, ci, cj, rT), aT);
      meanW[(size_t)j * lw + i] = (int32_t)imath::DivRound(box(pw, ci, cj, rW) * 255, aW);
    }
  }
  auto bilin = [&](const std::vector<int32_t>& M, int32_t x, int32_t z) {
    const int32_t ux = x - 64, uz = z - 64;
    const int32_t ci = FloorDiv(ux, kCoarse), cj = FloorDiv(uz, kCoarse);
    const int32_t fx = ux - ci * kCoarse, fz = uz - cj * kCoarse;
    const int32_t i = ci - lo0x, j = cj - lo0z;
    const int64_t m00 = M[(size_t)j * lw + i], m10 = M[(size_t)j * lw + i + 1];
    const int64_t m01 = M[(size_t)(j + 1) * lw + i], m11 = M[(size_t)(j + 1) * lw + i + 1];
    const int64_t v = m00 * (kCoarse - fx) * (kCoarse - fz) + m10 * fx * (kCoarse - fz) +
                      m01 * (kCoarse - fx) * fz + m11 * fx * fz;
    return (int32_t)imath::DivRound(v, (int64_t)kCoarse * kCoarse);
  };

  // ---- pack ---------------------------------------------------------------
  const int64_t scale = std::max<int64_t>((int64_t)((double)t.wind.tpiScale * 10.0 + 0.5), 1);
  for (int32_t j = 0; j < N; j++) {
    for (int32_t i = 0; i < N; i++) {
      const int32_t x = (bx + i) * kWindTerrCell + 16, z = (bz + j) * kWindTerrCell + 16;
      const int32_t h = ColH(fineV[j * N + i]);
      const int64_t tpi = (int64_t)h - bilin(meanH, x, z);
      const int32_t e = (int32_t)std::clamp<int64_t>(imath::DivRound(tpi * 127, scale), -127, 127);
      const int32_t w = std::clamp(bilin(meanW, x, z), 0, 255);
      const uint32_t slot = (uint32_t)(((bz + j) & (N - 1)) * N + ((bx + i) & (N - 1)));
      S.words[slot] = ((uint32_t)h & 0xFFFFu) | (((uint32_t)e & 0xFFu) << 16) |
                      ((uint32_t)w << 24);
    }
  }
  S.bx = bx;
  S.bz = bz;
  S.valid = true;
  S.lastQueries = queries;
}

// The table for the window at `origin` (chunk units), rebuilt when the base
// cell or the key moved.
const TerrState& EnsureTable(const Tuning& t, uint32_t seed, const int32_t origin[3]) {
  const uint64_t key = TerrKey(t, seed);
  const int32_t half = (int32_t)kWorldN / 2;
  const int32_t cxv = origin[0] * (int32_t)kChunk + half;
  const int32_t czv = origin[2] * (int32_t)kChunk + half;
  const int32_t bx = FloorDiv(cxv, kWindTerrCell) - (int32_t)kWindTerrN / 2;
  const int32_t bz = FloorDiv(czv, kWindTerrCell) - (int32_t)kWindTerrN / 2;
  if (gTerr.key != key) {
    gTerr.key = key;
    gTerr.valid = false;
    gTerr.fine.clear();
    gTerr.coarse.clear();
  }
  if (!gTerr.valid || gTerr.bx != bx || gTerr.bz != bz) BuildTable(t, seed, bx, bz);
  return gTerr;
}

// ---- the height profile, as the 16-knot table (world.h kWindProfKnots) ----
// p(h) = log2(h / z0 + 1) / log2(href / z0 + 1): the log-law in base 2, which
// is the same ratio (the base cancels) and is what imath::Log2Q16 computes
// exactly. Knot 0 is h = 0; knot k >= 1 is h = 2^(k-1) voxels.
void BuildProfile(const Tuning& t, int32_t out[kWindProfKnots]) {
  const Tuning::Wind& w = t.wind;
  const int64_t z0Q = std::max<int64_t>(
      (int64_t)((double)std::max(w.roughness, 0.001f) * 65536.0 + 0.5), 1);
  const int64_t hrefQ = (int64_t)((double)std::max(w.profileRef, 0.1f) * 65536.0 + 0.5);
  const int64_t lref = std::max<int64_t>(
      imath::Log2Q16((uint64_t)(65536 + (hrefQ << 16) / z0Q)), 1);
  const int64_t floorQ = (int64_t)((double)w.profileFloor * 65536.0 + 0.5);
  const int64_t capQ = std::max(floorQ, (int64_t)((double)w.profileCap * 65536.0 + 0.5));
  for (uint32_t k = 0; k < kWindProfKnots; k++) {
    const int64_t hv = k == 0 ? 0 : (int64_t)1 << (k - 1);   // voxels
    // h_m / z0 = hv * 0.1 / z0 = hv / (10 z0); in Q16: (hv << 32) / (10 z0Q).
    const int64_t x = 65536 + (hv << 32) / (10 * z0Q);
    const int64_t l = imath::Log2Q16((uint64_t)x);
    out[k] = (int32_t)std::clamp<int64_t>((l * 65536) / lref, floorQ, capQ);
  }
}

}  // namespace

uint32_t TerrainQueries() { return gTerr.lastQueries; }

// ============================================================================
// FILL
// ============================================================================

namespace {

// Q16 from a float knob: one exact double multiply and a rounding.
int32_t Q16(float v) {
  const double r = (double)v * 65536.0;
  return (int32_t)(r >= 0.0 ? r + 0.5 : r - 0.5);
}
int32_t Vox(float metres) { return std::max((int32_t)((double)metres * 10.0 + 0.5), 1); }
// smoothstep on a Q16 x in [0, 1].
int64_t SmoothQ(int64_t x) {
  x = std::clamp<int64_t>(x, 0, 65536);
  return (x * x * (3 * 65536 - 2 * x)) >> 32;
}
int32_t LerpQ(int32_t a, int32_t b, int64_t u) {
  return (int32_t)(a + (((int64_t)(b - a) * u) >> 16));
}

// The block, resolved once and copied into either struct. A template over the
// destination because the two structs carry the same member NAMES at
// different offsets, and a second hand-written copy is how they drift.
struct Block {
  uint32_t advPhase = 0;
  int32_t couplingQ = 65536, decoupleH = 1;
  int32_t wanderAmp = 0, wanderK = 1;
  uint32_t wanderPhase = 0;
  int32_t thermalQ = 0, thermalK = 1;
  uint32_t thermalPhase = 0;
  int32_t leeQ = 0, leeSlopeQ = 32768, leeDepth = 1, leeRevQ = 0, leeGustQ = 0;
  int32_t slopeWindQ = 0, slopeDepth = 1, seaBreezeQ = 0, seaDepth = 1;
  int32_t ridgeQ = 0, valleyQ = 0, expDepth = 1, absGainQ = 0;
  int32_t seaRadius = 1, seaLevelY = 0, neutralQ = 65536;
  int32_t prof[kWindProfKnots] = {};
  const TerrState* terr = nullptr;
};

// A clock that completes one turn (BAM16 65536) every `periodS` seconds, at
// tick + frac. Integer on the sim path (frac == 0); the render path adds the
// sub-tick fraction in double, which only ever reaches RenderParams.
uint32_t Clock(float periodS, uint32_t tick, float frac) {
  const int64_t pt = std::max<int64_t>((int64_t)((double)periodS * 30.0 + 0.5), 1);
  if (frac <= 0.0f) return (uint32_t)((((uint64_t)tick * 65536ull) / (uint64_t)pt) & 0xFFFFull);
  const double v = ((double)tick + (double)frac) * 65536.0 / (double)pt;
  return (uint32_t)((uint64_t)v & 0xFFFFull);
}
// BAM16 per voxel of a spatial wave `metres` long.
int32_t KOf(float metres) {
  return std::max((int32_t)(65536.0 / ((double)std::max(metres, 0.1f) * 10.0) + 0.5), 1);
}

Block Resolve(const Tuning& t, uint32_t seed, uint32_t tick, float frac,
              uint32_t dayPhase, const int32_t origin[3]) {
  const Tuning::Wind& w = t.wind;
  Block b;
  b.advPhase = AdvPhase(t, seed, tick, frac);
  const WindStateQ q = WindWeatherQ(t, seed, tick, dayPhase);
  b.ridgeQ = q.ridgeGain;
  b.valleyQ = q.valleyGain;
  b.couplingQ = q.coupling;
  b.decoupleH = Vox(w.decoupleHeight);
  b.wanderAmp = q.wanderAmp;
  b.wanderK = KOf(w.wanderWavelength);
  b.wanderPhase = Clock(w.wanderPeriod, tick, frac);
  b.thermalQ = q.thermal;
  b.thermalK = KOf(w.thermalWavelength);
  b.thermalPhase = Clock(w.thermalPeriod, tick, frac);
  b.leeQ = q.lee;
  b.leeSlopeQ = Q16(w.leeSlope);
  b.leeDepth = Vox(w.leeDepth);
  b.leeRevQ = Q16(w.leeReverse);
  b.leeGustQ = Q16(w.leeGust);
  b.slopeWindQ = q.slopeWind;
  b.slopeDepth = Vox(w.slopeDepth);
  b.seaBreezeQ = q.seaBreeze;
  b.seaDepth = Vox(w.seaDepth);
  b.expDepth = Vox(w.exposureDepth);
  // Per 100 m -> per 1024 voxels: x 1.024.
  b.absGainQ = (int32_t)((double)w.absGain * 1.024 * 65536.0 + (w.absGain >= 0 ? 0.5 : -0.5));
  b.seaRadius = Vox(w.seaRadius);
  b.seaLevelY = worldmap::CurrentWorldMap().seaLevelY;
  b.neutralQ = Q16(w.profileNeutral);
  BuildProfile(t, b.prof);
  b.terr = &EnsureTable(t, seed, origin);
  return b;
}

template <class P>
void Write(P& p, const Block& b) {
  p.wfAdvPhase = b.advPhase;
  p.wfWanderPhase = b.wanderPhase;
  p.wfWanderAmp = b.wanderAmp;
  p.wfWanderK = b.wanderK;
  p.wfThermalQ = b.thermalQ;
  p.wfThermalPhase = b.thermalPhase;
  p.wfThermalK = b.thermalK;
  p.wfCouplingQ = b.couplingQ;
  p.wfDecoupleH = b.decoupleH;
  p.wfLeeQ = b.leeQ;
  p.wfLeeSlopeQ = b.leeSlopeQ;
  p.wfLeeDepth = b.leeDepth;
  p.wfLeeRevQ = b.leeRevQ;
  p.wfLeeGustQ = b.leeGustQ;
  p.wfSlopeWindQ = b.slopeWindQ;
  p.wfSlopeDepth = b.slopeDepth;
  p.wfSeaBreezeQ = b.seaBreezeQ;
  p.wfSeaDepth = b.seaDepth;
  p.wfRidgeQ = b.ridgeQ;
  p.wfValleyQ = b.valleyQ;
  p.wfExpDepth = b.expDepth;
  p.wfAbsGainQ = b.absGainQ;
  p.wfSeaRadius = b.seaRadius;
  p.wfSeaLevelY = b.seaLevelY;
  p.wfNeutralQ = b.neutralQ;
  for (uint32_t i = 0; i < kWindProfKnots; i++) p.wfProf[i] = b.prof[i];
  if (b.terr && b.terr->valid) {
    p.wfTerrOn = 1;
    p.wfTerrBase[0] = b.terr->bx;
    p.wfTerrBase[1] = b.terr->bz;
    std::memcpy(p.wfTerr, b.terr->words, sizeof(p.wfTerr));
  } else {
    p.wfTerrOn = 0;
  }
}

}  // namespace

void FillWindField(TickParams& tp, const Tuning& t, uint32_t seed, uint32_t tick,
                   uint32_t dayPhase) {
  Write(tp, Resolve(t, seed, tick, 0.0f, dayPhase, tp.origin));
}

void FillWindField(RenderParams& rp, const Tuning& t, uint32_t seed, uint32_t tick,
                   float frac, uint32_t dayPhase, const int32_t origin[3]) {
  Write(rp, Resolve(t, seed, tick, frac, dayPhase, origin));
}

// ============================================================================
// THE STORM TIMELINE and LOCAL WINDS (docs/RESEARCH_wind.md §13.4)
// ============================================================================
//
// A THUNDERSTORM is a timeline, not a stronger breeze. The clock runs in
// cycles of wind.stormCycle seconds whether or not a storm is on; the regime's
// convective weight decides how much of it you feel, so a sky easing into a
// storm grows the cycle in rather than switching it on. One cycle:
//
//   u 0.00 - 0.15   the wind falls away toward the LULL (stormLull x mean)
//   u 0.15 - 0.25   the lull: the air goes still and heavy ahead of the cell
//   u 0.25 - 0.28   the GUST FRONT: the heading jumps by ~stormJump degrees
//                   and the mean spikes to stormFront x (2-3x)
//   u 0.28 - 0.45   the spike decays to stormDecay x, the heading holds
//   u 0.45 - 1.00   gusty decay back to 1x, heading easing home, gust
//                   fraction raised by stormGust and falling off
//
// Integer on Q16 of the cycle position; every number is a pure function of
// (tuning, seed, tick). The spatial structure — a gust front sweeping across
// the window, downbursts — is added by WeatherPrims below through the wind
// primitive list, not by a new mechanism.

namespace {

constexpr uint32_t kStormSalt = 0x57A4u;

// Piecewise-linear through (x_i, y_i), all Q16, x ascending.
int64_t Pw(int64_t u, const int64_t* xs, const int64_t* ys, int n) {
  if (u <= xs[0]) return ys[0];
  for (int i = 1; i < n; i++) {
    if (u <= xs[i]) {
      const int64_t span = std::max<int64_t>(xs[i] - xs[i - 1], 1);
      return ys[i - 1] + ((ys[i] - ys[i - 1]) * (u - xs[i - 1])) / span;
    }
  }
  return ys[n - 1];
}

int64_t StormTicks(const Tuning& t) {
  return std::max<int64_t>((int64_t)((double)t.wind.stormCycle * 30.0 + 0.5), 300);
}

}  // namespace

void StormTimeline(const Tuning& t, uint32_t seed, uint32_t tick, int32_t convective,
                   WindStateQ& o, int64_t& envelope, int64_t& stormGust,
                   uint32_t& jumpBam) {
  const Tuning::Wind& w = t.wind;
  envelope = 65536;
  stormGust = 0;
  jumpBam = 0;
  o.stormPhase = -1;
  if (convective <= 0) return;
  const int64_t P = StormTicks(t);
  const uint32_t n = (uint32_t)((int64_t)tick / P);
  const int64_t u = (((int64_t)tick % P) * 65536) / P;
  o.stormPhase = (int32_t)u;
  o.stormCycle = n;
  const int64_t C = convective;

  const int64_t lull = Q16(w.stormLull), front = Q16(w.stormFront), dec = Q16(w.stormDecay);
  static const int64_t xe[] = {0, 9830, 16384, 18350, 29491, 65536};   // 0 .15 .25 .28 .45 1
  const int64_t ye[] = {65536, lull, lull, front, dec, 65536};
  const int64_t f = Pw(u, xe, ye, 6);
  envelope = 65536 + ((f - 65536) * C >> 16);

  // The jump: its size and sign are this cycle's draw.
  const uint32_t h = rng::Hash3(seed ^ kStormSalt, n, 0u);
  const int64_t mag = 45875 + (int64_t)((h & 0xFFFFu) * 39322 >> 16);    // 0.7 .. 1.3
  const int64_t jdeg = ((int64_t)Q16(w.stormJump) * mag) >> 16;          // Q16 degrees
  static const int64_t xj[] = {0, 16384, 18350, 29491, 65536};
  static const int64_t yj[] = {0, 0, 65536, 65536, 0};
  const int64_t jw = (Pw(u, xj, yj, 5) * C) >> 16;
  // Q16 degrees x weight -> BAM32: deg / 360 * 2^32 = deg * 2^32 / 360.
  int64_t bam = (((jdeg * jw) >> 16) * 11930465) >> 16;   // 2^32 / 360 = 11930464.7
  if ((h >> 16) & 1u) bam = -bam;
  jumpBam = (uint32_t)bam;

  static const int64_t xg[] = {0, 16384, 18350, 65536};
  const int64_t yg[] = {0, 0, Q16(w.stormGust), 0};
  stormGust = (Pw(u, xg, yg, 4) * C) >> 16;
}

void LocalWinds(const Tuning& t, WindStateQ& o, int64_t speedQ, int64_t S) {
  const Tuning::Wind& w = t.wind;
  // They fade out as the synoptic wind rises past wind.localFade: a thermal
  // circulation is only visible when nothing larger is blowing.
  const int64_t fadeRef = std::max<int64_t>(winddetail::MetresPerSecToCellsQ(w.localFade), 65536);
  const int64_t fade = std::clamp<int64_t>(65536 - (speedQ * 65536) / fadeRef, 0, 65536);
  const int64_t Sp = std::max<int64_t>(S, 0), Sn = std::max<int64_t>(-S, 0);
  // By day (S > 0) the slopes heat: air flows UP them; the land heats past the
  // water: the breeze blows ONSHORE. At night both reverse, weaker.
  const int64_t slope = winddetail::MetresPerSecToCellsQ(w.slopeWind);
  const int64_t sea = winddetail::MetresPerSecToCellsQ(w.seaBreeze);
  const int64_t sDay = (slope * Sp) >> 16;
  const int64_t sNight = (((slope * Q16(w.slopeNight)) >> 16) * Sn) >> 16;
  const int64_t bDay = (sea * Sp) >> 16;
  const int64_t bNight = (((sea * Q16(w.seaNight)) >> 16) * Sn) >> 16;
  o.slopeWind = winddetail::ClampQ(((sDay - sNight) * fade) >> 16);
  o.seaBreeze = winddetail::ClampQ(((bDay - bNight) * fade) >> 16);
}

// ---- weather primitives -----------------------------------------------------
//
// A storm's gust front and its downbursts, emitted as ordinary wind
// primitives (windprim.h) so every consumer — grass, arrows, streaks, the
// particle tier, the CA drift bias — feels them with no code of its own. They
// are NOT spawned into WindPrims(): they are a pure function of (tuning,
// seed, tick, window), resolved here for the tick and appended to the list
// the tick ships, the render copy getting the same call. kWindPrimAir only:
// no entrainment licence, so no footprint wake (rule 2: the ambient weather
// never wakes a chunk; windprim.h).
//
// Positions are relative to the WINDOW CENTRE, which is itself on the tick
// stream, and grounded with World::TerrainHeight (the worldgen twin), so the
// list is reproducible.
uint32_t WeatherPrims(const Tuning& t, uint32_t seed, uint32_t tick,
                      const int32_t origin[3], WindPrimGpu* out, uint32_t cap,
                      int32_t lo[3], int32_t hi[3]) {
  const Tuning::Wind& w = t.wind;
  const WindStateQ q = WindWeatherQ(t, seed, tick);
  if (q.convective <= 0 || q.stormPhase < 0 || cap == 0) return 0;
  const int64_t P = StormTicks(t);
  const int64_t cycle0 = (int64_t)q.stormCycle * P;   // tick the cycle began
  const int32_t half = (int32_t)kWorldN / 2;
  const int32_t cx = origin[0] * (int32_t)kChunk + half;
  const int32_t cy = origin[1] * (int32_t)kChunk + half;
  const int32_t cz = origin[2] * (int32_t)kChunk + half;
  const int64_t C = q.convective;
  uint32_t n = 0;
  auto emit = [&](const WindPrim& p) {
    if (n >= cap) return;
    out[n++] = WindPrimResolve(p, tick);
    IVec3 a, b;
    WindPrimBounds(p, tick, a, b);
    lo[0] = std::min(lo[0], a.x); lo[1] = std::min(lo[1], a.y); lo[2] = std::min(lo[2], a.z);
    hi[0] = std::max(hi[0], b.x); hi[1] = std::max(hi[1], b.y); hi[2] = std::max(hi[2], b.z);
  };

  // The GUST FRONT: a wide jet sweeping across the window along the new
  // heading at the front's speed, arriving over the window centre at u = 0.26.
  if (w.stormFrontJet) {
    const int64_t tStart = cycle0 + (P * 14418) / 65536;   // u = 0.22
    const int64_t tEnd = cycle0 + (P * 26214) / 65536;     // u = 0.40
    if ((int64_t)tick >= tStart && (int64_t)tick < tEnd) {
      WindPrim p;
      p.kind = kWindPrimCone;
      p.flags = kWindPrimAir;
      p.radius = kWindPrimMaxExtent * 3 / 4;
      p.reach = kWindPrimMaxExtent;
      p.dirX = q.dirX;
      p.dirY = 0;
      p.dirZ = q.dirZ;
      // Front speed: the storm's own mean (with its envelope), per tick.
      const int64_t vt = (int64_t)q.speed / 30;                       // Q16.16 cells/tick
      p.velX = (int32_t)((vt * q.dirX) >> 16);
      p.velZ = (int32_t)((vt * q.dirZ) >> 16);
      // At u = 0.26 the jet's mouth is `reach` upwind of the centre, so the
      // strong band of the cone is over it.
      const int64_t tArrive = cycle0 + (P * 17039) / 65536;          // u = 0.26
      const int64_t back = ((vt * (tArrive - tStart)) >> 16) + p.reach / 2;
      p.x = cx - (int32_t)((back * q.dirX) >> 16);
      p.z = cz - (int32_t)((back * q.dirZ) >> 16);
      p.y = World::TerrainHeight(cx, cz, seed) + 16;
      p.spawnTick = (uint32_t)tStart;
      p.ttl = (uint32_t)(tEnd - tStart);
      const int64_t s = (((int64_t)q.speed * std::max<int64_t>(Q16(w.stormFront) - 65536, 0)) >> 17) * C >> 16;
      p.strengthQ = (int32_t)std::min<int64_t>(s, (int64_t)kWindPrimMaxSpeed << 16);
      emit(p);
    }
  }

  // DOWNBURSTS: a burst centred half its radius above the ground spreads
  // outward along it and pushes down from above — a microburst's footprint.
  const int32_t nb = std::clamp(w.stormBursts, 0, 6);
  const int32_t rad = std::clamp(Vox(w.stormBurstRadius), 16, kWindPrimMaxExtent);
  for (int32_t i = 0; i < nb; i++) {
    const uint32_t h0 = rng::Hash3(seed ^ kStormSalt, q.stormCycle, 16u + (uint32_t)i);
    const uint32_t h1 = rng::Hash3(seed ^ kStormSalt, q.stormCycle, 32u + (uint32_t)i);
    const int64_t uStart = 17039 + (int64_t)((h0 & 0xFFFFu) * 16384 >> 16);  // 0.26..0.51
    const int64_t tStart = cycle0 + (P * uStart) / 65536;
    const uint32_t ttl = 450;                                                 // 15 s
    if ((int64_t)tick < tStart || (int64_t)tick >= tStart + ttl) continue;
    WindPrim p;
    p.kind = kWindPrimBurst;
    p.flags = kWindPrimAir;
    p.radius = rad;
    p.reach = rad;
    const int32_t ox = (int32_t)((h1 & 0x3FFu) % 601u) - 300;
    const int32_t oz = (int32_t)(((h1 >> 10) & 0x3FFu) % 601u) - 300;
    p.x = cx + ox;
    p.z = cz + oz;
    p.y = World::TerrainHeight(p.x, p.z, seed) + rad / 2;
    (void)cy;
    p.spawnTick = (uint32_t)tStart;
    p.ttl = ttl;
    const int64_t s = (((int64_t)q.speed * 78643) >> 16) * C >> 16;   // 1.2 x mean
    p.strengthQ = (int32_t)std::min<int64_t>(s, (int64_t)kWindPrimMaxSpeed << 16);
    emit(p);
  }
  return n;
}

// ============================================================================
// THE REGIME LIBRARY (assets/wind/regimes.json)
// ============================================================================

namespace {

struct RegimeLib {
  bool loaded = false;
  std::vector<Regime> list;
  uint64_t fp = 0;
};
RegimeLib gReg;

std::vector<Regime> BuiltInRegimes() {
  auto r = [](const char* n, const char* l, float i, float g, float c) {
    Regime x;
    x.name = n;
    x.label = l;
    x.intensity = i;
    x.gale = g;
    x.convective = c;
    return x;
  };
  return {r("calm", "Calm", 0.05f, 0, 0),         r("light", "Light air", 0.16f, 0, 0),
          r("breezy", "Breezy", 0.32f, 0, 0),     r("windy", "Windy day", 0.52f, 0, 0),
          r("gale", "Gale", 0.78f, 1, 0),         r("thunderstorm", "Thunderstorm", 0.5f, 0, 1)};
}

void EnsureRegimes() {
  if (gReg.loaded) return;
  gReg.loaded = true;
  gReg.list.clear();
  const std::string path = std::string(sandvox::AssetDir()) + "/wind/regimes.json";
  std::ifstream f(path);
  if (f) {
    try {
      nlohmann::json j;
      f >> j;
      for (const auto& e : j.at("regimes")) {
        Regime r;
        r.name = e.at("name").get<std::string>();
        r.label = e.value("label", r.name);
        r.about = e.value("about", std::string());
        r.intensity = std::clamp((float)e.value("intensity", 0.32), 0.0f, 1.0f);
        r.gale = std::clamp((float)e.value("gale", 0.0), 0.0f, 1.0f);
        r.convective = std::clamp((float)e.value("convective", 0.0), 0.0f, 1.0f);
        gReg.list.push_back(r);
      }
    } catch (const std::exception& ex) {
      std::fprintf(stderr, "wind: %s: %s; using the built-in regimes\n", path.c_str(), ex.what());
      gReg.list.clear();
    }
  }
  if (gReg.list.empty()) gReg.list = BuiltInRegimes();
  Fp fp;
  for (const Regime& r : gReg.list) {
    fp.Bytes(r.name.data(), r.name.size());
    fp.F(r.intensity);
    fp.F(r.gale);
    fp.F(r.convective);
  }
  gReg.fp = fp.h;
}

}  // namespace

const std::vector<Regime>& Regimes() {
  EnsureRegimes();
  return gReg.list;
}

const Regime* FindRegime(const std::string& name) {
  EnsureRegimes();
  for (const Regime& r : gReg.list)
    if (r.name == name) return &r;
  return nullptr;
}

void ReloadRegimes() {
  gReg.loaded = false;
  EnsureRegimes();
}

uint64_t RegimeFingerprint() {
  EnsureRegimes();
  return gReg.fp;
}

}  // namespace windfield

// ============================================================================
// THE WEATHER, IN INTEGERS (wind.h WindWeatherQ)
// ============================================================================

namespace {
WindRegimeSource gRegimeSource = nullptr;

int32_t KnobQ16(float v) {
  const double r = (double)v * 65536.0;
  return (int32_t)(r >= 0.0 ? r + 0.5 : r - 0.5);
}
int64_t Smooth16(int64_t x) {
  x = std::clamp<int64_t>(x, 0, 65536);
  return (x * x * (3 * 65536 - 2 * x)) >> 32;
}
int64_t Lerp16(int64_t a, int64_t b, int64_t u) { return a + (((b - a) * u) >> 16); }
int64_t Clamp01(int64_t v) { return std::clamp<int64_t>(v, 0, 65536); }

// Heading as a Q30 vector from the epoch blend at `tick`, epochs 2^shift ticks
// long. Returns the smoothstepped Q24 position too.
void EpochHeading(uint32_t seed, uint32_t tick, uint32_t shift, uint32_t salt,
                  int64_t& vx, int64_t& vz, int64_t& uQ24,
                  winddetail::EpochQ& e0, winddetail::EpochQ& e1) {
  const uint32_t epoch = tick >> shift;
  const uint32_t span = 1u << shift;
  int64_t u = (int64_t)(tick & (span - 1u)) << (24 - shift);
  const int64_t uu = (u * u) >> 24;
  u = (uu * (3ll * kWQOne - 2 * u)) >> 24;
  e0 = winddetail::EpochTargetQ(seed ^ salt, epoch);
  e1 = winddetail::EpochTargetQ(seed ^ salt, epoch + 1u);
  vx = (((int64_t)imath::SinQ30(e0.headBam) * (kWQOne - u) +
         (int64_t)imath::SinQ30(e1.headBam) * u) >> 24);
  vz = (((int64_t)imath::CosQ30(e0.headBam) * (kWQOne - u) +
         (int64_t)imath::CosQ30(e1.headBam) * u) >> 24);
  uQ24 = u;
}

// The intensity -> mean speed curve, Q16 multiple of wind.windSpeed:
// piecewise linear through (0, calm), (0.3, 1), (0.75, gale), (1, max).
int64_t SpeedCurveQ(const Tuning::Wind& w, int64_t iQ) {
  const int64_t k0 = KnobQ16(w.speedCalmMul), k1 = 65536;
  const int64_t k2 = KnobQ16(w.speedGaleMul), k3 = KnobQ16(w.speedMaxMul);
  const int64_t a = 19661, b = 49152;   // 0.3, 0.75
  iQ = Clamp01(iQ);
  if (iQ <= a) return k0 + ((k1 - k0) * iQ) / a;
  if (iQ <= b) return k1 + ((k2 - k1) * (iQ - a)) / (b - a);
  return k2 + ((k3 - k2) * (iQ - b)) / (65536 - b);
}

}  // namespace

void SetWindRegimeSource(WindRegimeSource src) { gRegimeSource = src; }

WindStateQ WindWeatherQ(const Tuning& t, uint32_t seed, uint32_t tick, uint32_t dayPhase) {
  const Tuning::Wind& w = t.wind;
  WindStateQ o;

  // ---- 1. the epochs: heading and the two mood draws -------------------------
  uint32_t headBam = imath::BamFromDegrees((double)w.windDirDeg);
  int64_t vx = imath::SinQ30(headBam), vz = imath::CosQ30(headBam);
  int32_t sp01 = kWQOne / 2, gu01 = kWQOne / 2;
  int64_t stormW = 0;  // epoch storm draw, blended, Q16
  if (w.weatherAuto) {
    int64_t u;
    winddetail::EpochQ e0, e1;
    EpochHeading(seed, tick, kWindEpochShift, 0u, vx, vz, u, e0, e1);
    sp01 = winddetail::LerpQ24(e0.speed01, e1.speed01, u);
    gu01 = winddetail::LerpQ24(e0.gust01, e1.gust01, u);
    stormW = ((e0.storm ? (kWQOne - u) : 0) + (e1.storm ? u : 0)) >> 8;
  }
  o.speed01 = (sp01 + 128) >> 8;
  o.gust01 = (gu01 + 128) >> 8;

  // ---- 2. the regime --------------------------------------------------------
  // Pinned preset > manual > the source (the sky) > the wind's own epochs, and
  // the intensity override beats all of them.
  WeatherRegime r;
  const windfield::Regime* pin =
      (w.regime.empty() || w.regime == "auto") ? nullptr : windfield::FindRegime(w.regime);
  if (pin) {
    r.intensity = KnobQ16(pin->intensity);
    r.gale = KnobQ16(pin->gale);
    r.convective = KnobQ16(pin->convective);
    o.source = kWindSrcPreset;
  } else if (!w.weatherAuto) {
    o.source = kWindSrcManual;
  } else {
    bool got = false;
    if (gRegimeSource) {
      got = gRegimeSource(t, seed, tick, r);
    } else {
      got = weather::SimWindRegime(t, seed, tick, r.intensity, r.gale, r.convective, r.cover);
    }
    if (got) {
      o.source = kWindSrcSky;
      // The mood: the epoch draw swings the sky's intensity by +-moodSpread.
      const int64_t m = ((int64_t)o.speed01 * 2 - 65536);   // [-1, 1]
      const int64_t sprQ = KnobQ16(w.moodSpread);
      r.intensity = (int32_t)Clamp01(r.intensity + ((int64_t)r.intensity * ((m * sprQ) >> 16) >> 16));
      // A windy sky whose epoch drew a storm becomes a GALE for that epoch:
      // the sky has no gale preset of its own, and a gale that arrived with
      // every rain would be a gale most afternoons.
      const int64_t windy = Clamp01(((int64_t)r.intensity - 26214) * 10);   // 0.4..0.5
      r.gale = (int32_t)std::max<int64_t>(r.gale, (stormW * windy) >> 16);
      r.intensity = (int32_t)Lerp16(r.intensity, std::max<int64_t>(r.intensity, 51118), r.gale);
    } else {
      // No sky: the epochs alone, mapped onto the intensity scale.
      o.source = kWindSrcEpochs;
      const int64_t s01 = ((int64_t)sp01 + 128) >> 8;   // Q16, 0.1..1.0
      r.intensity = (int32_t)Clamp01(3277 + ((s01 - 6554) * 52429) / 58982);  // 0.05..0.85
      r.gale = (int32_t)stormW;
    }
  }
  if (w.intensity >= 0.0f) r.intensity = (int32_t)Clamp01(KnobQ16(w.intensity));
  o.intensity = r.intensity;
  o.gale = r.gale;
  o.convective = r.convective;
  o.cover = r.cover;
  o.storm = std::max(r.gale, r.convective) > 32768;
  const int64_t I = r.intensity;

  // ---- 3. the convective storm timeline (stage 4 fills this) -----------------
  int64_t envelope = 65536, stormGust = 0;
  uint32_t jump = 0;
  windfield::StormTimeline(t, seed, tick, r.convective, o, envelope, stormGust, jump);

  // ---- 4. heading: a gale holds it -------------------------------------------
  // The slow heading: the same draw on epochs 16x longer (~18 min), salted
  // apart. A gale pulls the weather heading toward it by galeHold x gale.
  if (w.weatherAuto && r.gale > 0) {
    int64_t lx, lz, lu;
    winddetail::EpochQ l0, l1;
    EpochHeading(seed, tick, kWindEpochShift + 4, 0x6A1Eu, lx, lz, lu, l0, l1);
    const int64_t g = (KnobQ16(w.galeHold) * (int64_t)r.gale) >> 16;
    vx = vx + (((lx - vx) * g) >> 16);
    vz = vz + (((lz - vz) * g) >> 16);
  }
  const int64_t len = (int64_t)imath::Sqrt64((uint64_t)(vx * vx + vz * vz));
  if (len > 107374) {
    o.dirX = (int32_t)imath::DivRound(vx << 16, len);
    o.dirZ = (int32_t)imath::DivRound(vz << 16, len);
  } else {
    o.dirX = imath::SinQ16(headBam);
    o.dirZ = imath::CosQ16(headBam);
  }
  if (jump != 0) {
    // Rotate by the storm's jump: (x, z) -> (x c + z s, -x s + z c) turns the
    // heading by +jump in the engine's 0 = +Z, toward +X convention.
    const int64_t c = imath::CosQ16(jump), s = imath::SinQ16(jump);
    const int64_t x = o.dirX, z = o.dirZ;
    o.dirX = (int32_t)((x * c + z * s) >> 16);
    o.dirZ = (int32_t)((z * c - x * s) >> 16);
  }
  o.jumpBam = jump;

  // ---- 5. the mean speed at the reference height -------------------------------
  const int64_t baseQ = winddetail::MetresPerSecToCellsQ(w.windSpeed);
  int64_t speedQ = imath::MulShiftRound(baseQ, SpeedCurveQ(w, I), 16);
  speedQ = imath::MulShiftRound(speedQ, envelope, 16);
  o.speed = winddetail::ClampQ(speedQ);
  o.envelope = (int32_t)envelope;

  // ---- 6. stability ----------------------------------------------------------
  // Convective by day (the sun heats the ground), stable at night (it cools),
  // both damped by cloud, both mixed away by wind. [-1, 1].
  int64_t S = 0;
  if (dayPhase != kWindNoDayPhase) {
    const int64_t day = DaylightStrengthCpu(dayPhase);            // 0..255
    const int64_t raw = (day * 2 * 65536) / 255 - 65536;
    const int64_t damp = 65536 - ((45875 * Clamp01(r.cover)) >> 16);   // 1 - 0.7 cover
    const int64_t mix = Clamp01(65536 - (I * 65536) / std::max<int64_t>(KnobQ16(w.mixIntensity), 1));
    S = (((raw * damp) >> 16) * mix) >> 16;
  }
  o.stability = (int32_t)S;
  const int64_t Sp = std::max<int64_t>(S, 0), Sn = std::max<int64_t>(-S, 0);

  // ---- 7. the field parameters -------------------------------------------------
  // Gust fraction: light -> strong over intensity 0..0.8, plus thermals in
  // light convective air, converging on the gale's; the storm adds its own.
  const int64_t iS = Smooth16((I * 5) / 4);
  int64_t gf = Lerp16(KnobQ16(w.gustLight), KnobQ16(w.gustStrong), iS);
  gf += (KnobQ16(w.gustConvective) * ((Sp * (65536 - iS)) >> 16)) >> 16;
  gf = Lerp16(gf, KnobQ16(w.galeGust), r.gale);
  gf += stormGust;
  gf = (gf * KnobQ16(w.gustStrength)) >> 16;
  if (w.weatherAuto) gf = (gf * (52429 + ((26214 * (int64_t)o.gust01) >> 16))) >> 16;  // 0.8 + 0.4u
  o.gustFrac = (int32_t)std::max<int64_t>(gf, 0);
  o.gust = winddetail::ClampQ(imath::MulShiftRound(speedQ, o.gustFrac, 16));

  o.coupling = (int32_t)(65536 - ((KnobQ16(w.stableDecouple) * Sn) >> 16));
  const int64_t wl = (int64_t)(w.wanderLight * 65536.0f / 360.0f);
  const int64_t ws = (int64_t)(w.wanderStrong * 65536.0f / 360.0f);
  int64_t wa = Lerp16(wl, ws, Smooth16((I * 10) / 7));
  wa = (wa * (65536 - ((KnobQ16(w.galeHold) * (int64_t)r.gale) >> 16))) >> 16;
  o.wanderAmp = (int32_t)std::clamp<int64_t>(wa, 0, 16384);
  o.thermal = winddetail::ClampQ((winddetail::MetresPerSecToCellsQ(w.thermalGust) * Sp) >> 16);
  const int64_t tS = Smooth16(I);
  o.ridgeGain = (int32_t)Lerp16(KnobQ16(w.ridgeLight), KnobQ16(w.ridgeStrong), tS);
  o.valleyGain = (int32_t)Lerp16(KnobQ16(w.valleyLight), KnobQ16(w.valleyStrong), tS);
  o.lee = (int32_t)((KnobQ16(w.leeStrength) *
                     Clamp01(((I - KnobQ16(w.leeOnset)) * 65536) / 9830)) >> 16);   // over 0.15
  windfield::LocalWinds(t, o, speedQ, S);
  return o;
}

