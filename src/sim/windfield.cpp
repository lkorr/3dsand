#include "sim/windfield.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

#include "sim/intmath.h"
#include "sim/tuning.h"
#include "sim/worldmap.h"

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
  int32_t ridgeQ = 0, valleyQ = 0, expDepth = 1, absGainQ = 0;
  int32_t seaRadius = 1, seaLevelY = 0, neutralQ = 65536;
  int32_t prof[kWindProfKnots] = {};
  const TerrState* terr = nullptr;
};

Block Resolve(const Tuning& t, uint32_t seed, uint32_t tick, float frac,
              uint32_t /*dayPhase*/, const int32_t origin[3]) {
  const Tuning::Wind& w = t.wind;
  Block b;
  b.advPhase = AdvPhase(t, seed, tick, frac);
  const WindStateQ q = WindWeatherQ(t, seed, tick);
  // Terrain response by intensity: light -> strong, smoothstepped.
  const int64_t s = SmoothQ(q.speed01);
  b.ridgeQ = LerpQ(Q16(w.ridgeLight), Q16(w.ridgeStrong), s);
  b.valleyQ = LerpQ(Q16(w.valleyLight), Q16(w.valleyStrong), s);
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
  p.wfCouplingQ = 65536;
  p.wfDecoupleH = 1;
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

}  // namespace windfield
