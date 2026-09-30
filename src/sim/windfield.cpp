#include "sim/windfield.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

#include "sim/intmath.h"
#include "sim/tuning.h"

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
// FILL
// ============================================================================

namespace {

// The block, resolved once and copied into either struct. A template over the
// destination because the two structs carry the same member NAMES at
// different offsets, and a second hand-written copy is how they drift.
struct Block {
  uint32_t advPhase = 0;
};

Block Resolve(const Tuning& t, uint32_t seed, uint32_t tick, float frac,
              uint32_t /*dayPhase*/) {
  Block b;
  b.advPhase = AdvPhase(t, seed, tick, frac);
  return b;
}

template <class P>
void Write(P& p, const Block& b) {
  p.wfAdvPhase = b.advPhase;
  // Identity until the later stages fill them: full coupling, a flat profile
  // at 1.0, no table.
  p.wfCouplingQ = 65536;
  p.wfNeutralQ = 65536;
  for (uint32_t i = 0; i < kWindProfKnots; i++) p.wfProf[i] = 65536;
  p.wfTerrOn = 0;
}

}  // namespace

void FillWindField(TickParams& tp, const Tuning& t, uint32_t seed, uint32_t tick,
                   uint32_t dayPhase) {
  Write(tp, Resolve(t, seed, tick, 0.0f, dayPhase));
}

void FillWindField(RenderParams& rp, const Tuning& t, uint32_t seed, uint32_t tick,
                   float frac, uint32_t dayPhase) {
  Write(rp, Resolve(t, seed, tick, frac, dayPhase));
}

}  // namespace windfield
