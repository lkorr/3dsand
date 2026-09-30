#include "game/flasksim.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>

namespace alchemy {

namespace {

constexpr float kLiquidThresh = 0.28f;

inline V2 operator+(V2 a, V2 b) { return {a.x + b.x, a.y + b.y}; }
inline V2 operator-(V2 a, V2 b) { return {a.x - b.x, a.y - b.y}; }
inline V2 operator*(V2 a, float s) { return {a.x * s, a.y * s}; }
inline float Dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
inline float Cross(V2 a, V2 b) { return a.x * b.y - a.y * b.x; }
inline float Len(V2 a) { return std::sqrt(Dot(a, a)); }

// Closest point on segment ab to p, and the segment parameter.
inline V2 ClosestOnSeg(V2 p, V2 a, V2 b, float* tOut) {
  V2 ab = b - a;
  float l2 = Dot(ab, ab);
  float t = l2 > 1e-9f ? Dot(p - a, ab) / l2 : 0.f;
  t = std::clamp(t, 0.f, 1.f);
  if (tOut) *tOut = t;
  return a + ab * t;
}

inline uint32_t PackRGBA(int r, int g, int b, int a) {
  auto c = [](int v) { return (uint32_t)std::clamp(v, 0, 255); };
  return c(r) | c(g) << 8 | c(b) << 16 | c(a) << 24;
}
inline int ChR(uint32_t c) { return c & 255; }
inline int ChG(uint32_t c) { return (c >> 8) & 255; }
inline int ChB(uint32_t c) { return (c >> 16) & 255; }
// Scale an 0xAABBGGRR colour's RGB by f/16 (integer steps: the look is
// posterised on purpose, the UI is pixel art).
inline uint32_t Shade(uint32_t c, int f16, int alpha) {
  return PackRGBA(ChR(c) * f16 / 16, ChG(c) * f16 / 16, ChB(c) * f16 / 16, alpha);
}
inline uint32_t Lift(uint32_t c, int amount, int alpha) {
  return PackRGBA(ChR(c) + (255 - ChR(c)) * amount / 16,
                  ChG(c) + (255 - ChG(c)) * amount / 16,
                  ChB(c) + (255 - ChB(c)) * amount / 16, alpha);
}
inline uint32_t MixRGB(uint32_t a, uint32_t b, float t, int alpha) {
  return PackRGBA((int)(ChR(a) + (ChR(b) - ChR(a)) * t), (int)(ChG(a) + (ChG(b) - ChG(a)) * t),
                  (int)(ChB(a) + (ChB(b) - ChB(a)) * t), alpha);
}
inline uint32_t AddRGB(uint32_t c, float r, float g, float b, int alpha) {
  return PackRGBA(ChR(c) + (int)r, ChG(c) + (int)g, ChB(c) + (int)b, alpha);
}

// The material palette is LINEAR light (the world's frame is lit, tonemapped
// and gamma-encoded at the end); the bench draws straight to the screen, so
// it encodes it here. Raw, blood (#5a0606) read as black; a full 1/2.2
// encode (an albedo under full sun) washed water grey and oil khaki. 1/1.5
// sits where the world's daylight puts them.
inline uint32_t ToDisplay(uint32_t c) {
  static const auto lut = [] {
    std::array<uint8_t, 256> t{};
    for (int i = 0; i < 256; i++) t[i] = (uint8_t)std::lround(255.0 * std::pow(i / 255.0, 1.0 / 1.5));
    return t;
  }();
  return PackRGBA(lut[ChR(c)], lut[ChG(c)], lut[ChB(c)], 255);
}

// What a liquid LOOKS like, from the authored fields the world's renderer
// reads (common.wgsl): the opaque flag (lava is a surface, not a medium),
// isViscousLiquid (moveEvery > 1 and opacity >= 150: a smooth wet sheen, no
// ripples), and media opacity for everything else (how fast depth darkens).
enum LookKind : uint8_t { kLookClear = 0, kLookViscous = 1, kLookMolten = 2 };

}  // namespace

VesselShape FlaskShape(float width, float height) {
  VesselShape s;
  s.width = width;
  s.height = height;
  // Round-bottomed flask: a bulb in the lower 60%, a neck above, a small
  // flared lip. (halfWidth, height) in 0..1 of the box.
  s.profile = {{0.34f, 0.00f}, {0.62f, 0.035f}, {0.84f, 0.11f}, {0.96f, 0.22f},
               {0.98f, 0.33f}, {0.90f, 0.45f}, {0.68f, 0.56f}, {0.36f, 0.64f},
               {0.24f, 0.70f}, {0.23f, 0.95f}, {0.30f, 1.00f}};
  return s;
}

VesselShape PouchShape(float width, float height) {
  VesselShape s;
  s.width = width;
  s.height = height;
  s.wall = 1.5f;
  s.glass = false;
  s.profile = {{0.55f, 0.00f}, {0.86f, 0.06f}, {0.98f, 0.22f}, {0.96f, 0.45f},
               {0.80f, 0.64f}, {0.46f, 0.78f}, {0.34f, 0.84f}, {0.42f, 0.92f},
               {0.52f, 1.00f}};
  return s;
}

float ShapeArea(const VesselShape& s) {
  // Trapezoids between consecutive profile rows, both sides.
  float a = 0;
  for (size_t i = 0; i + 1 < s.profile.size(); i++) {
    const V2 p = s.profile[i], q = s.profile[i + 1];
    a += (p.x + q.x) * (q.y - p.y);   // (hw0 + hw1)/2 * dh, times 2 sides
  }
  return a * (s.width * 0.5f) * s.height;
}

VesselShape ShapeWithArea(VesselShape s, float area) {
  // Usable inside at scale k: k^2 A - k P c, with A and P the polygon's area
  // and perimeter at scale 1 and c the unusable band (half the glass plus
  // half a pixel). Solved for k.
  const float A = ShapeArea(s);
  float P = 0;
  {
    const float hw = s.width * 0.5f;
    for (size_t i = 0; i + 1 < s.profile.size(); i++) {
      const float dx = (s.profile[i + 1].x - s.profile[i].x) * hw;
      const float dy = (s.profile[i + 1].y - s.profile[i].y) * s.height;
      P += 2.0f * std::sqrt(dx * dx + dy * dy);
    }
    P += 2.0f * s.profile.front().x * hw;   // the base
  }
  const float c = s.wall + 0.75f;
  if (A > 1e-3f && area > 0) {
    const float k = (P * c + std::sqrt(P * c * P * c + 4.0f * A * area)) / (2.0f * A);
    s.width *= k;
    s.height *= k;
  }
  return s;
}

FlaskSim::FlaskSim(const SimConfig& cfg) : cfg_(cfg), rng_(cfg.seed | 1u) {
  // The lattice a settled liquid sits on: hexagonal, cell area =
  // unitsPerParticle pixels, so particle count x unitsPerParticle is the
  // area the liquid covers at rest.
  restArea_ = (float)cfg_.unitsPerParticle;
  spacing_ = std::sqrt(restArea_ / 0.8660254f);
  h_ = spacing_ * cfg_.kernelScale;

  // Rest density of an interior lattice particle, and the near density at
  // the same spot. Clavet's near pressure is always a push, so the rest
  // density is raised until the first ring's pair displacement is zero —
  // the seeded lattice is then an equilibrium, not a compressed spring.
  float rho = 0, rhoN = 0, q1 = 1;
  for (int j = -6; j <= 6; j++)
    for (int i = -6; i <= 6; i++) {
      if (!i && !j) continue;
      // Odd rows sit half a spacing over.
      float xo = (float)i * spacing_ + ((j & 1) ? 0.5f * spacing_ : 0.f);
      float y = (float)j * spacing_ * 0.8660254f;
      float r = std::sqrt(xo * xo + y * y);
      float q = r / h_;
      if (q >= 1) continue;
      rho += (1 - q) * (1 - q);
      rhoN += (1 - q) * (1 - q) * (1 - q);
      q1 = std::min(q1, q);
    }
  rho0_ = rho + (cfg_.stiffnessNear / cfg_.stiffness) * rhoN * (1 - q1);

  // Cells as wide as the neighbour list's reach, so a 3x3 scan finds every
  // pair inside it. (They were h wide against a 1.15 h reach, and the pairs
  // in between were missed whenever they straddled two cells.)
  cellSize_ = h_ * cfg_.listSlack;
  cellsW_ = (int)std::ceil(cfg_.gridW / cellSize_) + 1;
  cellsH_ = (int)std::ceil(cfg_.gridH / cellSize_) + 1;
  grid_.assign((size_t)cfg_.gridW * cfg_.gridH, 0);
  wall_.assign((size_t)cfg_.gridW * cfg_.gridH, 0);
  gasSub_.assign((size_t)cfg_.gridW * cfg_.gridH, 0xFF);
  gasAmt_.assign((size_t)cfg_.gridW * cfg_.gridH, 0);
  gasAge_.assign((size_t)cfg_.gridW * cfg_.gridH, 0);
  gasListed_.assign((size_t)cfg_.gridW * cfg_.gridH, 0);
}

void FlaskSim::SetSubstances(const std::vector<Substance>& subs) {
  subs_ = subs;
  mass_.resize(subs.size());
  visc_.resize(subs.size());
  spilledUnits_.assign(subs.size(), 0);
  exitBy_.clear();
  slotOf_.clear();
  for (size_t i = 0; i < subs.size(); i++) {
    if (subs[i].mat >= slotOf_.size()) slotOf_.resize((size_t)subs[i].mat + 1, -1);
    slotOf_[subs[i].mat] = (int)i;
  }
  produced_.assign(subs.size(), 0);
  consumed_.assign(subs.size(), 0);
  gasBank_.assign((vessels_.size() + 1) * subs.size(), 0);
  seeded_.assign(subs.size(), 0);
  removed_.assign(subs.size(), 0);
  drained_.assign(subs.size(), 0);
  present_.assign(subs.size(), 0);
  activeSlot_.assign(subs.size(), 0);
  for (size_t i = 0; i < subs.size(); i++) {
    // The 3D sim's density decides the ORDER; the exponent stretches the
    // gaps (water 1000 vs oil 900 is 10%, which relaxes into a mushy
    // boundary) without ever reordering two substances.
    // Bounded and strictly monotonic: slope massGain at water, saturating
    // at e^(massCeil*pi/2), so lava (2800) and sand (1600) keep their order
    // where a clamped power curve would give both the ceiling.
    float lr = std::log(std::max(1, subs[i].density) / 1000.f);
    float b = cfg_.massGain / cfg_.massCeil;
    mass_[i] = std::exp(cfg_.massCeil * std::atan(b * lr));
    float me = (float)std::max<uint32_t>(1, subs[i].moveEvery);
    visc_[i] = cfg_.viscosity * me * me;
  }
  look_.resize(subs.size());
  for (size_t i = 0; i < subs.size(); i++) {
    const Substance& s = subs[i];
    Look& L = look_[i];
    for (int k = 0; k < 3; k++) L.col[k] = ToDisplay(s.color[k]);
    const float op = s.opacity / 255.0f;
    L.kind = s.opaque ? kLookMolten : (s.moveEvery > 1 && s.opacity >= 150) ? kLookViscous : kLookClear;
    L.film = s.opaque ? 1.0f : 0.55f + 0.35f * op;
    L.absorb = 0.02f + 0.10f * op;
    L.glow = s.emission / 255.0f;
    // Alpha by depth below the surface: the film at the top, thickening
    // toward what the opacity allows.
    const float maxA = s.opaque || L.kind == kLookViscous ? 1.0f : 0.72f + 0.28f * op;
    for (int d = 0; d < 256; d++)
      L.alpha[d] = (uint8_t)std::lround(255.0f * (L.film + (maxA - L.film) * (1.0f - std::exp(-L.absorb * d))));
  }
}

// ---- the workers -------------------------------------------------------------
namespace {
class BenchPool {
 public:
  static BenchPool& Get() {
    static BenchPool pool;
    return pool;
  }
  int Lanes() const { return (int)workers_.size() + 1; }
  // Runs job(c) for every c in [0, chunks). False (nothing run) when the pool
  // is busy with another caller's loop: the caller runs it serially.
  bool Run(int chunks, const std::function<void(int)>& job) {
    std::unique_lock<std::mutex> own(runMu_, std::try_to_lock);
    if (!own.owns_lock() || workers_.empty()) return false;
    {
      std::unique_lock<std::mutex> lk(mu_);
      // A worker that woke late for the last loop may still be in its pull
      // loop (it will find nothing left); it must be out before the counters
      // are reset under it.
      idle_.wait(lk, [&] { return inFlight_ == 0; });
      job_ = &job;
      chunks_ = chunks;
      next_.store(0);
      done_.store(0);
      gen_++;
      genA_.store(gen_, std::memory_order_release);
    }
    wake_.notify_all();
    Pull(&job, chunks);
    std::unique_lock<std::mutex> lk(mu_);
    idle_.wait(lk, [&] { return done_.load() == chunks; });
    return true;
  }
  ~BenchPool() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      quit_ = true;
      quitA_.store(true);
    }
    wake_.notify_all();
    for (std::thread& t : workers_) t.join();
  }

 private:
  BenchPool() {
    int n = 3;   // + the caller: four lanes, on a 16-thread machine that also runs the game
    if (const char* e = std::getenv("SANDVOX_BENCH_THREADS")) n = std::max(0, std::atoi(e) - 1);
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw > 0) n = std::min(n, (int)hw - 1);
    for (int i = 0; i < n; i++) workers_.emplace_back([this] { Work(); });
  }
  void Pull(const std::function<void(int)>* job, int chunks) {
    for (;;) {
      const int c = next_.fetch_add(1);
      if (c >= chunks) break;
      (*job)(c);
      if (done_.fetch_add(1) + 1 == chunks) {
        std::lock_guard<std::mutex> lk(mu_);
        idle_.notify_all();
      }
    }
  }
  void Work() {
    uint64_t seen = 0;
    for (;;) {
      const std::function<void(int)>* job;
      int chunks;
      {
        std::unique_lock<std::mutex> lk(mu_);
        wake_.wait(lk, [&] { return quit_ || gen_ != seen; });
        if (quit_) return;
        seen = gen_;
        job = job_;
        chunks = chunks_;
        inFlight_++;
      }
      Pull(job, chunks);
      std::lock_guard<std::mutex> lk(mu_);
      inFlight_--;
      idle_.notify_all();
    }
  }
  std::vector<std::thread> workers_;
  std::mutex runMu_, mu_;
  std::condition_variable wake_, idle_;
  const std::function<void(int)>* job_ = nullptr;
  int chunks_ = 0, inFlight_ = 0;
  uint64_t gen_ = 0;
  bool quit_ = false;
  std::atomic<int> next_{0}, done_{0};
  std::atomic<uint64_t> genA_{0};
  std::atomic<bool> quitA_{false};
};
}  // namespace

void BenchParallelFor(int n, int grain, const std::function<void(int, int)>& fn) {
  if (n <= 0) return;
  BenchPool& pool = BenchPool::Get();
  const int lanes = pool.Lanes();
  const int chunks = std::min(lanes * 2, (n + std::max(1, grain) - 1) / std::max(1, grain));
  if (lanes <= 1 || chunks <= 1) { fn(0, n); return; }
  // The split is a function of n and the chunk count only.
  auto job = [&](int c) {
    const int b = (int)((int64_t)n * c / chunks), e = (int)((int64_t)n * (c + 1) / chunks);
    if (b < e) fn(b, e);
  };
  if (!pool.Run(chunks, job))
    for (int c = 0; c < chunks; c++) job(c);
}

// The last angle's cos and sin, per direction and thread: the hot loops
// (the gas's carry, the mouth's draw, the hot glass per cell) convert
// thousands of points through one pose, and the trig was most of that. The
// same expressions as ever, so the same bits.
namespace {
struct TrigMemo {
  float a = std::numeric_limits<float>::quiet_NaN(), c = 1, s = 0;
};
thread_local TrigMemo tToLocal, tToWorld;
}  // namespace
V2 FlaskSim::ToLocal(const Xform& x, V2 w) const {
  V2 d = w - x.pos;
  TrigMemo& m = tToLocal;
  if (!(m.a == x.angle)) { m.a = x.angle; m.c = std::cos(-x.angle); m.s = std::sin(-x.angle); }
  const float c = m.c, s = m.s;
  return {d.x * c - d.y * s, d.x * s + d.y * c};
}
V2 FlaskSim::ToWorld(const Xform& x, V2 l) const {
  TrigMemo& m = tToWorld;
  if (!(m.a == x.angle)) { m.a = x.angle; m.c = std::cos(x.angle); m.s = std::sin(x.angle); }
  const float c = m.c, s = m.s;
  return {x.pos.x + l.x * c - l.y * s, x.pos.y + l.x * s + l.y * c};
}

void FlaskSim::BuildOutline(Vessel& v, bool raster) const {
  const auto& pr = v.shape.profile;
  float hw = v.shape.width * 0.5f, H = v.shape.height;
  v.outline.clear();
  for (int i = (int)pr.size() - 1; i >= 0; i--) v.outline.push_back({-pr[i].x * hw, pr[i].y * H});
  for (size_t i = 0; i < pr.size(); i++) v.outline.push_back({pr[i].x * hw, pr[i].y * H});
  v.reach = 1;
  for (V2 o : v.outline) v.reach = std::max(v.reach, Len(o));
  if (!raster) return;
  // The near-glass raster, in the vessel's own frame (its shape never
  // changes): each 1 px cell is far-inside, far-outside, or near the glass.
  // "Far" is past the contact radius plus a step's worth of relative motion
  // (a particle's speed cap plus the glass's), plus a cell's diagonal.
  const float R = v.shape.wall + spacing_ * 0.45f;
  const float far = R + cfg_.maxSpeedFrac * h_ + cfg_.maxVesselStep + 1.5f;
  float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
  for (V2 o : v.outline) { x0 = std::min(x0, o.x); x1 = std::max(x1, o.x); y0 = std::min(y0, o.y); y1 = std::max(y1, o.y); }
  v.nx0 = std::floor(x0 - far - 2);
  v.ny0 = std::floor(y0 - far - 2);
  v.nw = (int)std::ceil(x1 + far + 2 - v.nx0);
  v.nh = (int)std::ceil(y1 + far + 2 - v.ny0);
  v.near.assign((size_t)v.nw * v.nh, Vessel::kNear);
  for (int yy = 0; yy < v.nh; yy++)
    for (int xx = 0; xx < v.nw; xx++) {
      const V2 p{v.nx0 + xx + 0.5f, v.ny0 + yy + 0.5f};
      float dmin = 1e9f;
      for (size_t k = 0; k + 1 < v.outline.size(); k++)
        dmin = std::min(dmin, Len(p - ClosestOnSeg(p, v.outline[k], v.outline[k + 1], nullptr)));
      // The mouth is not glass, but a particle crossing it changes side:
      // near the mouth line counts as near.
      dmin = std::min(dmin, Len(p - ClosestOnSeg(p, v.outline.front(), v.outline.back(), nullptr)));
      if (dmin > far) v.near[(size_t)yy * v.nw + xx] = InsideLocal(v, p) ? Vessel::kFarIn : Vessel::kFarOut;
    }
}

bool FlaskSim::InsideLocal(const Vessel& v, V2 p) const {
  // Even-odd over the outline closed across the mouth.
  bool in = false;
  const auto& o = v.outline;
  size_t n = o.size();
  for (size_t i = 0, j = n - 1; i < n; j = i++) {
    if ((o[i].y > p.y) != (o[j].y > p.y)) {
      float xi = o[j].x + (p.y - o[j].y) * (o[i].x - o[j].x) / (o[i].y - o[j].y);
      if (p.x < xi) in = !in;
    }
  }
  return in;
}

bool FlaskSim::InsideVessel(const Vessel& v, V2 w) const { return InsideLocal(v, ToLocal(v.x, w)); }

int FlaskSim::AddVessel(const VesselShape& shape, const Xform& x, const Composition& c,
                        bool stoppered, const VesselLayout* layout) {
  Vessel v;
  v.shape = shape;
  v.x = v.prevX = v.target = x;
  v.stoppered = stoppered;
  BuildOutline(v);
  vessels_.push_back(std::move(v));
  gasBank_.resize((vessels_.size() + 1) * subs_.size(), 0);
  RebuildWalls();
  const int vi = (int)vessels_.size() - 1;
  wakeAll_ = true;
  // THE LEDGER'S OPENING BALANCE: what came on, in units, by the slot of its
  // BASE material (a dissolved portion is its powder).
  for (int i = 0; i < c.n; i++) {
    const int sl = SlotOf(BaseMat(c.p[i].mat));
    if (sl >= 0) seeded_[sl] += (int64_t)c.p[i].eighths * cfg_.unitsPerEighth;
  }
  if (!cfg_.settleOnAdd) {
    SeedVessel(vi, c, layout);
    Partition();
    return vi;
  }
  // SETTLED ON ARRIVAL. The seed is a lattice, not a resting liquid: shown
  // as seeded, a flask appeared brim-full, sagged, and shed the particles
  // that had no lattice point over its lip. So the contents are run to rest
  // in a scratch table holding only this vessel, heavily damped (nothing to
  // look at there), and adopted from it: the vessel appears at rest, asleep.
  SimConfig sc = cfg_;
  sc.settleOnAdd = false;
  sc.calmSteps = 0;
  sc.damping = 0.03f;
  sc.xsph = 0.2f;
  sc.sleepDrift = 0.4f;
  sc.sleepSteps = 24;
  FlaskSim tmp(sc);
  tmp.SetSubstances(subs_);
  // The solute table only (which liquids dissolve what, and how much they
  // hold); no rule fires while a vessel is being settled.
  tmp.SetChemistry(chem_);
  tmp.PauseChemistry(true);
  tmp.AddVessel(shape, x, c, stoppered, layout);
  for (int s = 0; s < 1500; s++) {
    tmp.Step(1);
    if (s > 30 && tmp.VesselAsleep(0) && tmp.grainMoves_ == 0 && tmp.MovingCount(0.25f) == 0) break;
  }
  AdoptSettled(vi, tmp);
  return vi;
}

void FlaskSim::AdoptSettled(int vi, const FlaskSim& from) {
  for (size_t i = 0; i < from.px_.size(); i++) {
    px_.push_back(from.px_[i]);
    pprev_.push_back(from.px_[i]);
    pv_.push_back({0, 0});
    psub_.push_back(from.psub_[i]);
    pw_.push_back(from.pw_[i]);
    mbar_.push_back(from.mbar_[i]);
    calm_.push_back(0);
    phome_.push_back(from.phome_[i] == 0 ? (int8_t)vi : (int8_t)-1);
    psrc_.push_back(from.phome_[i] == 0 ? (int8_t)vi : (int8_t)-1);
    pvar_.push_back(from.pvar_[i]);
    panc_.push_back(from.px_[i]);
    pheat_.push_back(from.pheat_[i]);
    psol_.push_back(from.psol_[i]);
    pmass_.push_back(from.pmass_[i]);
  }
  // The gas it settled with: the same table, the same pose, pixel for pixel.
  // (GAS units both sides; what cannot sit on its pixel -- another gas is
  // there -- goes in beside it, inside the new vessel, or waits in its pool.)
  for (int k : from.gasList_) {
    if (!from.gasAmt_[k]) continue;
    uint32_t q = from.gasAmt_[k];
    const int within = from.inside_[k] == 1 ? vi : -2;
    if (!AddGasAt(from.gasSub_[k], q, k % cfg_.gridW, k / cfg_.gridW, within, 10))
      AddPool(within >= 0 ? vi : -1, from.gasSub_[k], q, {(k % cfg_.gridW) + 0.5f, (k / cfg_.gridW) + 0.5f});
  }
  // ...and its bank: the scratch's vessel 0 is this vessel, its outside ours.
  {
    const size_t S = subs_.size();
    for (size_t s = 0; s < S && s < from.subs_.size(); s++) {
      if (from.gasBank_.size() >= S) gasBank_[s] += from.gasBank_[s];
      if (from.gasBank_.size() >= 2 * S) gasBank_[((size_t)vi + 1) * S + s] += from.gasBank_[S + s];
    }
  }
  for (const Pool& p0 : from.pools_) {
    Pool p = p0;
    p.vessel = p0.vessel == 0 ? vi : -1;
    pools_.push_back(p);
  }
  for (const Grain& g0 : from.grains_) {
    Grain g = g0;
    g.home = g0.home == 0 ? (int8_t)vi : (int8_t)-1;
    g.src = g.home;
    g.vx = g.vy = 0;
    if (!PlaceGrain(g, g.x, g.y)) { spilledUnits_[g.sub]++; NoteFrom(vi, g.sub, 1); }
  }
  for (size_t s = 0; s < spilledUnits_.size() && s < from.spilledUnits_.size(); s++)
    spilledUnits_[s] += from.spilledUnits_[s];
  // Anything that did not come to rest (it could not, in 1500 steps) is left
  // awake; a resting vessel starts asleep and costs nothing until touched.
  Vessel& v = vessels_[vi];
  v.asleep = from.vessels_[0].asleep;
  v.quiet = 0;
  Partition();
}

void FlaskSim::SetVesselXform(int vi, const Xform& x) { vessels_[vi].target = x; }

void FlaskSim::UnwindAngle(int vi, float turn) {
  Vessel& v = vessels_[vi];
  v.x.angle -= turn;
  v.prevX.angle -= turn;
  v.target.angle -= turn;
}

void FlaskSim::TeleportVessel(int vi, const Xform& x) {
  Vessel& v = vessels_[vi];
  const bool moved = v.x.pos.x != x.pos.x || v.x.pos.y != x.pos.y || v.x.angle != x.angle;
  v.x = v.prevX = v.target = x;
  v.vel = {0, 0};
  v.angVel = 0;
  // (frameVel is kept: the next step's frame acceleration stops the
  // contents with the glass, at the share they feel.)
  RebuildWalls();
  wakeAll_ = true;
  // Glass placed somewhere else leaves its contents where they were: they
  // must not hang there asleep.
  if (moved && v.asleep) {
    v.asleep = false;
    v.quiet = 0;
    Partition();
  }
}

void FlaskSim::SetStick(bool on, V2 a, V2 b, float r) {
  if (on && !stickOn_) { stickPrevA_ = a; stickPrevB_ = b; }
  stickOn_ = on;
  stickA_ = a;
  stickB_ = b;
  stickR_ = r;
}

uint32_t FlaskSim::Rand() {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return rng_;
}

// ---- seeding ---------------------------------------------------------------

// A particle's HEAT PHASE (the look of molten matter): smooth value noise of
// where it was seeded, so neighbours start alike and the blobs they make are
// carried and torn by the flow. Random per particle it was salt and pepper.
uint8_t FlaskSim::SeedHeat(V2 p) {
  auto hash = [](int x, int y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 1023) / 1023.0f;
  };
  float v = 0, amp = 0.65f, f = 1.0f / 14.0f;
  for (int o = 0; o < 2; o++, amp *= 0.35f, f *= 2.3f) {
    const float x = p.x * f, y = p.y * f;
    const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    float ax = x - x0, ay = y - y0;
    ax = ax * ax * (3 - 2 * ax);
    ay = ay * ay * (3 - 2 * ay);
    const float a = hash(x0, y0), b = hash(x0 + 1, y0), c = hash(x0, y0 + 1), d = hash(x0 + 1, y0 + 1);
    v += amp * ((a + (b - a) * ax) * (1 - ay) + (c + (d - c) * ax) * ay);
  }
  return (uint8_t)std::clamp((int)(v / 0.8775f * 255.0f), 0, 255);
}

void FlaskSim::SeedVessel(int vi, const Composition& c, const VesselLayout* layout) {
  if (layout && LayoutFits(*layout, c)) {
    SeedFromLayout(vi, c, *layout);
    SeedExtras(vi, c);
    return;
  }
  Vessel& v = vessels_[vi];
  // Substance slots present, in PORTION ORDER: bottom-up (composition.h).
  struct Layer { int sub; uint32_t units; };
  std::vector<Layer> layers;
  for (int i = 0; i < c.n; i++) {
    // Dissolved matter and gas have no layer: SeedExtras puts them into the
    // solvent and the headspace once the layers are down.
    if (IsDissolved(c.p[i].mat)) continue;
    const int sub = SlotOf(c.p[i].mat);
    if (sub < 0 || c.p[i].eighths == 0 || subs_[sub].gas) continue;
    layers.push_back({sub, c.p[i].eighths * (uint32_t)cfg_.unitsPerEighth});
  }
  // Density reorders only a pair physics would: one of them a liquid, the
  // upper one heavier (a powder sinks through a liquid, a liquid under a
  // lighter one rises, a powder floats on a heavier liquid). Two powders
  // stay as they were put in -- grains never sort themselves, so dirt laid
  // on sodium is dirt on sodium. Every swap removes one density inversion,
  // so this ends.
  for (bool swapped = true; swapped;) {
    swapped = false;
    for (size_t i = 0; i + 1 < layers.size(); i++) {
      const Substance& lo = subs_[layers[i].sub];
      const Substance& up = subs_[layers[i + 1].sub];
      if ((!lo.powder || !up.powder) && up.density > lo.density) {
        std::swap(layers[i], layers[i + 1]);
        swapped = true;
      }
    }
  }

  // The vessel's interior pixels by local row, bottom up (pixel centres
  // inside the outline and clear of the glass).
  const float H = v.shape.height;
  const float clear = v.shape.wall + 0.5f;
  auto clearOfGlass = [&](V2 p, float r) {
    for (size_t i = 0; i + 1 < v.outline.size(); i++)
      if (Len(p - ClosestOnSeg(p, v.outline[i], v.outline[i + 1], nullptr)) < r) return false;
    return true;
  };

  float yCursor = 0;  // local height the next layer starts at
  for (const Layer& L : layers) {
    if (subs_[L.sub].powder) {
      // Pack whole rows of pixels until the units are spent.
      uint32_t left = L.units;
      int row = (int)std::floor(yCursor);
      for (; left > 0 && row < (int)H + 8; row++) {
        std::vector<int> xs;
        float half = v.shape.width * 0.5f + 2;
        for (int x = (int)-half; x <= (int)half; x++) {
          V2 p{x + 0.5f, row + 0.5f};
          if (InsideLocal(v, p) && clearOfGlass(p, clear)) xs.push_back(x);
        }
        // A partial top row fills from the middle out, so it reads as a
        // mound rather than a ragged edge.
        std::sort(xs.begin(), xs.end(), [](int a, int b) { return std::abs(a) < std::abs(b); });
        for (int x : xs) {
          if (!left) break;
          V2 w = ToWorld(v.x, {x + 0.5f, row + 0.5f});
          Grain g{};
          g.sub = (uint8_t)L.sub;
          g.variant = (uint8_t)(Rand() % 3);
          g.home = (int8_t)vi;
          g.src = (int8_t)vi;
          if (PlaceGrain(g, (int)std::floor(w.x), (int)std::floor(w.y))) left--;
          else { spilledUnits_[L.sub]++; NoteFrom(vi, L.sub, 1); }  // no room at all: never silently lost
        }
      }
      // Ran out of vessel before the grains ran out: the rest is spilled,
      // never dropped (it used to vanish -- 800 grains of a full pouch).
      spilledUnits_[L.sub] += left;
      NoteFrom(vi, L.sub, (uint32_t)left);
      yCursor = (float)row;
    } else {
      // Hex lattice points inside and clear of the glass, lowest first.
      uint32_t upp = (uint32_t)cfg_.unitsPerParticle;
      uint32_t count = (L.units + upp - 1) / upp;
      uint32_t lastW = L.units - (count - 1) * upp;
      std::vector<V2> pts;
      float rowH = spacing_ * 0.8660254f;
      float half = v.shape.width * 0.5f;
      float y = yCursor + rowH * 0.5f;
      for (int j = 0; pts.size() < count && y < H * 1.6f; j++, y += rowH) {
        float x0 = -half + ((j & 1) ? spacing_ * 0.5f : 0.f);
        for (float x = x0; x <= half; x += spacing_) {
          V2 p{x, y};
          if (InsideLocal(v, p) && clearOfGlass(p, clear + spacing_ * 0.1f)) pts.push_back(p);
        }
      }
      // A partial top row keeps its middle: among the points on the last
      // row used, the ones nearest the axis go first.
      if (pts.size() > count) {
        float topY = pts[count - 1].y;
        size_t a = 0, b = 0;
        while (a < pts.size() && pts[a].y < topY - 0.01f) a++;
        b = a;
        while (b < pts.size() && pts[b].y < topY + 0.01f) b++;
        std::sort(pts.begin() + a, pts.begin() + b,
                  [](V2 p, V2 q) { return std::fabs(p.x) < std::fabs(q.x); });
      }
      // Not enough lattice inside (a brim-full vessel): the rest is stacked
      // on the same lattice in a column over the mouth, and pours in while
      // AddVessel settles it. It used to be dropped at one point by the neck,
      // all on top of each other, which blew droplets out over the glass.
      if (pts.size() < count) {
        const float mouth = std::max(spacing_, v.shape.profile.back().x * half - clear - spacing_);
        float yy = std::max(y, H + rowH);
        for (int j = 0; pts.size() < count && j < 4096; j++, yy += rowH)
          for (float xx = -mouth + ((j & 1) ? spacing_ * 0.5f : 0.f); xx <= mouth && pts.size() < count;
               xx += spacing_)
            pts.push_back({xx, yy});
      }
      for (uint32_t k = 0; k < count; k++) {
        V2 lp = pts[k];
        V2 w = ToWorld(v.x, lp);
        px_.push_back(w);
        pprev_.push_back(w);
        pv_.push_back({0, 0});
        psub_.push_back((uint8_t)L.sub);
        pw_.push_back((uint16_t)(k + 1 == count ? lastW : upp));
        mbar_.push_back(mass_[L.sub]);
        calm_.push_back((uint8_t)std::clamp(cfg_.calmSteps, 0, 255));
        phome_.push_back((int8_t)vi);
        psrc_.push_back((int8_t)vi);
        pvar_.push_back((uint8_t)(Rand() >> 7));
        panc_.push_back(w);
        pheat_.push_back(SeedHeat(lp));
        psol_.push_back(0);
        pmass_.push_back(0);
      }
      float top = count ? pts[count - 1].y : yCursor;
      yCursor = top + rowH * 0.5f;
    }
  }
  SeedExtras(vi, c);
}

// ---- layouts (VesselLayout) -------------------------------------------------

Composition FlaskSim::LayeredOf(const Composition& c) const {
  Composition out;
  for (int i = 0; i < c.n; i++) {
    if (IsDissolved(c.p[i].mat) || !c.p[i].eighths) continue;
    const int sl = SlotOf(c.p[i].mat);
    if (sl < 0 || subs_[sl].gas) continue;
    out.Add(c.p[i].mat, c.p[i].eighths);
  }
  return out;
}

bool FlaskSim::LayoutFits(const VesselLayout& L, const Composition& c) const {
  return !L.layered.Empty() && LayeredOf(c).SameAs(L.layered);
}

VesselLayout FlaskSim::SnapshotVessel(int vi) const {
  VesselLayout L;
  if (!VesselAlive(vi)) return L;
  const Vessel& v = vessels_[vi];
  // The same membership RemoveVessel and Count use: liquid by the outline,
  // a grain by the inside mask (one in the glass band by its home).
  for (size_t i = 0; i < px_.size(); i++) {
    if (!InsideVessel(v, px_[i])) continue;
    const V2 l = ToLocal(v.x, px_[i]);
    L.drops.push_back({l.x, l.y, subs_[psub_[i]].mat, pw_[i], pvar_[i], pheat_[i]});
  }
  const int W = cfg_.gridW;
  for (const Grain& g : grains_) {
    const size_t k = (size_t)g.y * W + g.x;
    const int owner = wall_[k] ? g.home : (int)inside_[k] - 1;
    if (owner != vi) continue;
    const V2 l = ToLocal(v.x, {g.x + 0.5f, g.y + 0.5f});
    L.grains.push_back({l.x, l.y, subs_[g.sub].mat, g.variant});
  }
  return L;
}

void FlaskSim::FinishLayout(VesselLayout& L, Composition& c) const {
  L.layered = LayeredOf(c);
  // Mean height of each material in the picture, by the units it holds.
  auto meanY = [&](uint16_t mat) {
    double sum = 0, w = 0;
    for (const VesselLayout::Grain& g : L.grains)
      if (g.mat == mat) { sum += g.y; w += 1; }
    for (const VesselLayout::Drop& d : L.drops)
      if (d.mat == mat) { sum += (double)d.y * d.units; w += d.units; }
    return w > 0 ? sum / w : 1e9;  // nowhere in the picture (a pool): on top
  };
  struct Keyed { Portion p; int group; double y; };
  std::vector<Keyed> k;
  for (int i = 0; i < c.n; i++) {
    const uint16_t m = c.p[i].mat;
    const bool layered = L.layered.AmountOf(m) != 0;
    k.push_back({c.p[i], layered ? 0 : 1, layered ? meanY(m) : 0.0});
  }
  std::stable_sort(k.begin(), k.end(), [](const Keyed& a, const Keyed& b) {
    if (a.group != b.group) return a.group < b.group;
    return a.group == 0 && a.y < b.y;
  });
  for (int i = 0; i < c.n; i++) c.p[i] = k[i].p;
}

void FlaskSim::SeedFromLayout(int vi, const Composition& c, const VesselLayout& L) {
  const Vessel& v = vessels_[vi];
  const uint32_t upe = (uint32_t)cfg_.unitsPerEighth;
  // What each slot must hold, in units: exactly what the contents say (the
  // ledger's opening balance). The picture can differ by the part of an
  // eighth the tally rounded away; restored bottom-up, a surplus is the top.
  std::vector<int64_t> want(subs_.size(), 0);
  std::vector<float> topY(subs_.size(), -1.0f);
  const Composition lay = LayeredOf(c);
  for (int i = 0; i < lay.n; i++) want[SlotOf(lay.p[i].mat)] += (int64_t)lay.p[i].eighths * upe;
  std::vector<int> order(L.grains.size());
  for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return L.grains[a].y < L.grains[b].y; });
  for (int i : order) {
    const VesselLayout::Grain& e = L.grains[i];
    const int sl = SlotOf(e.mat);
    // A material that is no longer a powder (an R reload) is left to the
    // shortfall below, which seeds it as what it is now.
    if (sl < 0 || !subs_[sl].powder || want[sl] <= 0) continue;
    const V2 w = ToWorld(v.x, {e.x, e.y});
    Grain g{};
    g.sub = (uint8_t)sl;
    g.variant = e.variant;
    g.home = (int8_t)vi;
    g.src = (int8_t)vi;
    if (PlaceGrain(g, (int)std::floor(w.x), (int)std::floor(w.y), vi)) {
      want[sl]--;
      topY[sl] = std::max(topY[sl], e.y);
    }
  }
  std::vector<int> dorder(L.drops.size());
  for (size_t i = 0; i < dorder.size(); i++) dorder[i] = (int)i;
  std::stable_sort(dorder.begin(), dorder.end(), [&](int a, int b) { return L.drops[a].y < L.drops[b].y; });
  for (int i : dorder) {
    const VesselLayout::Drop& e = L.drops[i];
    const int sl = SlotOf(e.mat);
    if (sl < 0 || subs_[sl].powder || subs_[sl].gas || want[sl] <= 0 || !e.units) continue;
    const int u = (int)std::min<int64_t>(e.units, want[sl]);
    const int k = SpawnParticle(ToWorld(v.x, {e.x, e.y}), sl, u, vi);
    pvar_[k] = e.var;
    pheat_[k] = e.heat;
    calm_[k] = (uint8_t)std::clamp(cfg_.calmSteps, 0, 255);
    want[sl] -= u;
    topY[sl] = std::max(topY[sl], e.y);
  }
  // THE SHORTFALL (the rounding, a grain with no pixel left, a material that
  // changed kind): just over where its kind was, or low in the middle.
  const uint32_t upp = (uint32_t)std::max(1, cfg_.unitsPerParticle);
  for (size_t s = 0; s < want.size(); s++) {
    if (want[s] <= 0) continue;
    const float y = topY[s] >= 0 ? topY[s] + 1.0f : v.shape.height * 0.3f;
    const V2 at = ToWorld(v.x, {0.0f, y});
    if (subs_[s].powder) {
      AddPool(vi, (int)s, (uint32_t)want[s], at);
      continue;
    }
    for (int64_t left = want[s]; left > 0;) {
      const uint32_t u = (uint32_t)std::min<int64_t>(left, upp);
      const V2 j{at.x + (float)((int)(Rand() % 5) - 2) * 0.4f, at.y + (float)((int)(Rand() % 5) - 2) * 0.4f};
      SpawnParticle(j, (int)s, (int)u, vi);
      left -= u;
    }
  }
}

// ---- walls -----------------------------------------------------------------

void FlaskSim::RebuildWalls() {
  std::fill(wall_.begin(), wall_.end(), 0);
  const int W = cfg_.gridW, Hh = cfg_.gridH;
  // Each glass pixel holds its vessel's index + 1 (the renderer lights the
  // highlighted one); everything else only asks "is it glass".
  for (size_t vIdx = 0; vIdx < vessels_.size(); vIdx++) {
    const Vessel& v = vessels_[vIdx];
    float r = v.shape.wall + 0.25f;
    // A STOPPERED vessel's mouth is glass too (the closing segment, a little
    // thicker: a cork, not a pane), so nothing leaves it.
    const size_t nseg = v.outline.empty() ? 0 : v.outline.size() - 1 + (v.stoppered ? 1 : 0);
    for (size_t i = 0; i < nseg; i++) {
      const bool mouth = i + 1 >= v.outline.size();
      V2 a = ToWorld(v.x, v.outline[i]), b = ToWorld(v.x, mouth ? v.outline.front() : v.outline[i + 1]);
      const float r0 = r;
      r = mouth ? r0 + 1.0f : r0;
      int x0 = std::max(0, (int)std::floor(std::min(a.x, b.x) - r - 1));
      int x1 = std::min(W - 1, (int)std::ceil(std::max(a.x, b.x) + r + 1));
      int y0 = std::max(0, (int)std::floor(std::min(a.y, b.y) - r - 1));
      int y1 = std::min(Hh - 1, (int)std::ceil(std::max(a.y, b.y) + r + 1));
      for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
          V2 p{x + 0.5f, y + 0.5f};
          if (Len(p - ClosestOnSeg(p, a, b, nullptr)) <= r) wall_[(size_t)y * W + x] = (uint8_t)std::min<size_t>(vIdx + 1, 255);
        }
      r = r0;
    }
  }
  // The inside mask: each vessel's outline scanline-filled (even-odd over the
  // polygon closed across the mouth), first vessel wins.
  inside_.assign((size_t)W * Hh, 0);
  for (size_t vi = 0; vi < vessels_.size(); vi++) {
    const Vessel& v = vessels_[vi];
    if (v.outline.empty()) continue;
    std::vector<V2> P(v.outline.size());
    float y0 = 1e9f, y1 = -1e9f;
    for (size_t k = 0; k < P.size(); k++) {
      P[k] = ToWorld(v.x, v.outline[k]);
      y0 = std::min(y0, P[k].y);
      y1 = std::max(y1, P[k].y);
    }
    float xs[64];
    for (int y = std::max(0, (int)std::floor(y0)); y <= std::min(Hh - 1, (int)std::ceil(y1)); y++) {
      const float py = y + 0.5f;
      int n = 0;
      for (size_t a = 0, b = P.size() - 1; a < P.size(); b = a++)
        if ((P[a].y > py) != (P[b].y > py) && n < 64)
          xs[n++] = P[b].x + (py - P[b].y) * (P[a].x - P[b].x) / (P[a].y - P[b].y);
      std::sort(xs, xs + n);
      for (int k = 0; k + 1 < n; k += 2) {
        const int xa = std::max(0, (int)std::ceil(xs[k] - 0.5f));
        const int xb = std::min(W - 1, (int)std::ceil(xs[k + 1] - 0.5f) - 1);
        for (int x = xa; x <= xb; x++) {
          uint8_t& c = inside_[(size_t)y * W + x];
          if (!c) c = (uint8_t)(vi + 1);
        }
      }
    }
  }
}

// ---- liquid ----------------------------------------------------------------

void FlaskSim::BuildCells() {
  const int n = nAct_;
  const int nc = cellsW_ * cellsH_;
  cellStart_.assign(nc + 1, 0);
  cellIdx_.resize(n);
  cellOf_.resize(n);
  for (int i = 0; i < n; i++) {
    int cx = std::clamp((int)(px_[i].x / cellSize_), 0, cellsW_ - 1);
    int cy = std::clamp((int)(px_[i].y / cellSize_), 0, cellsH_ - 1);
    cellOf_[i] = cy * cellsW_ + cx;
    cellStart_[cellOf_[i] + 1]++;
  }
  for (int c = 0; c < nc; c++) cellStart_[c + 1] += cellStart_[c];
  std::vector<int> fill(cellStart_.begin(), cellStart_.end() - 1);
  for (int i = 0; i < n; i++) cellIdx_[fill[cellOf_[i]]++] = i;
}

void FlaskSim::StepLiquid() {
  const int n = nAct_;
  if (!n) return;
  const float g = cfg_.gravity;
  const float h = h_;
  const float invH = 1.f / h;

  // ONE neighbour list per step, built on the start-of-step positions with
  // a slack radius, serves viscosity AND the relaxation after advection
  // (distances are recomputed; a pair outside h is skipped). With the speed
  // cap below a pair cannot close more than 2 x maxSpeedFrac x h in a step,
  // and the slack covers the typical case — the grid scan was two thirds of
  // the liquid's cost when each pass did its own.
  BuildCells();
  const float reach = h * cfg_.listSlack;
  const float reach2 = reach * reach;
  // Each pair is found ONCE (a half stencil: this cell's later entries, and
  // the cells right, up-left, up and up-right) and written into both
  // particles' lists.
  pairs_.clear();
  nbrStart_.assign(n + 1, 0);
  for (int cy = 0; cy < cellsH_; cy++)
    for (int cx = 0; cx < cellsW_; cx++) {
      const int c = cy * cellsW_ + cx;
      const int a0 = cellStart_[c], a1 = cellStart_[c + 1];
      if (a0 == a1) continue;
      static const int kHalf[4][2] = {{1, 0}, {-1, 1}, {0, 1}, {1, 1}};
      for (int a = a0; a < a1; a++) {
        const int i = cellIdx_[a];
        const V2 pi = px_[i];
        for (int b = a + 1; b < a1; b++) {
          const int j = cellIdx_[b];
          const V2 d = px_[j] - pi;
          if (Dot(d, d) < reach2) pairs_.push_back({i, j});
        }
        for (auto& o : kHalf) {
          const int xx = cx + o[0], yy = cy + o[1];
          if (xx < 0 || xx >= cellsW_ || yy >= cellsH_) continue;
          const int cc = yy * cellsW_ + xx;
          for (int b = cellStart_[cc]; b < cellStart_[cc + 1]; b++) {
            const int j = cellIdx_[b];
            const V2 d = px_[j] - pi;
            if (Dot(d, d) < reach2) pairs_.push_back({i, j});
          }
        }
      }
    }
  for (const auto& pr : pairs_) { nbrStart_[pr.first + 1]++; nbrStart_[pr.second + 1]++; }
  for (int i = 0; i < n; i++) nbrStart_[i + 1] += nbrStart_[i];
  nbr_.resize(nbrStart_[n]);
  {
    std::vector<int>& fill = nbrFill_;
    fill.assign(nbrStart_.begin(), nbrStart_.end() - 1);
    for (const auto& pr : pairs_) { nbr_[fill[pr.first]++] = pr.second; nbr_[fill[pr.second]++] = pr.first; }
  }

  // 1. gravity + buoyancy + pairwise viscosity.
  //
  // Buoyancy is explicit: a particle whose neighbourhood is heavier than it
  // is pushed up by the difference (Boussinesq-style), a heavier one down.
  // The mass-split relaxation alone sorts bulk layers, but a lone droplet
  // inside another liquid is squeezed out by the interface push to the
  // NEAREST free surface, not the one its density says. The neighbourhood
  // mass is last step's (measured in the relaxation pass).
  const float buoy = cfg_.buoyancy;
  // THE VESSEL'S FRAME (SimConfig::vesselFeel). A particle inside a vessel
  // (the inside raster at the start of the step: pframe_) rides that
  // vessel's frame: it takes the vessel's acceleration -- all but the share
  // it feels as slosh -- its speed is capped relative to the frame, and it is
  // moved by the frame's WHOLE-PIXEL shift, the very shift CarryGrains gave
  // the grains, plus its own motion relative to the frame. Its velocity
  // stays a world velocity (the frame's plus its own). So a carried flask
  // carries its liquid WITH its sand, pixel for pixel, instead of the glass
  // shoving the liquid while the sand was lifted into it.
  const int W = cfg_.gridW, Hh = cfg_.gridH;
  const float carryX = 1.0f - std::clamp(cfg_.vesselFeel, 0.0f, 1.0f);
  const float carryY = 1.0f - std::clamp(cfg_.vesselFeelLift, 0.0f, 1.0f);
  bool anyFrame = false;
  for (const Vessel& v : vessels_)
    anyFrame |= v.frameAcc.x != 0 || v.frameAcc.y != 0 || v.frameVel.x != 0 || v.frameVel.y != 0;
  pframe_.assign(n, -1);
  if (anyFrame && !inside_.empty())
    for (int i = 0; i < n; i++) {
      const int x = (int)std::floor(px_[i].x), y = (int)std::floor(px_[i].y);
      if (x < 0 || y < 0 || x >= W || y >= Hh) continue;
      const int in = inside_[(size_t)y * W + x];
      if (in > 0 && in <= (int)vessels_.size()) pframe_[i] = (int8_t)(in - 1);
    }
  for (int i = 0; i < n; i++) {
    float mi = mass_[psub_[i]];
    pv_[i].y -= g * (1.f + buoy * (mi - mbar_[i]) / mi);
    if (pframe_[i] >= 0) {
      const V2 a = vessels_[pframe_[i]].frameAcc;
      pv_[i] = pv_[i] + V2{a.x * carryX, a.y * carryY};
    }
  }
  // The sort drive (SimConfig::sortDrive) rides the same pair loop, and
  // counts each vessel's inverted pairs for UpdateSleep.
  for (Vessel& v : vessels_) v.inverted = 0;
  const float sortF = cfg_.sortDrive * g;
  const float invDy = spacing_ * 0.5f;
  for (const auto& pr : pairs_) {
    {
      const int i = pr.first, j = pr.second;
      V2 d = px_[j] - px_[i];
      float r = Len(d);
      if (r <= 1e-5f || r >= h) continue;
      if (psub_[i] != psub_[j] && !subs_[psub_[i]].powder && !subs_[psub_[j]].powder) {
        const float mi = mass_[psub_[i]], mj = mass_[psub_[j]];
        if (std::fabs(mi - mj) > 0.01f * std::min(mi, mj)) {
          // Heavy above light? `up` is how far the heavy one sits above.
          const bool iHeavy = mi > mj;
          const float up = iHeavy ? -d.y : d.y;
          if (up > 0) {
            const float f = sortF * (1 - r / h) * (up / r);
            const float mh = iHeavy ? mi : mj, ml = iHeavy ? mj : mi;
            const float dvh = f * ml / (mh + ml), dvl = f * mh / (mh + ml);
            pv_[iHeavy ? i : j].y -= dvh;
            pv_[iHeavy ? j : i].y += dvl;
            if (up > invDy) {
              const int hv = phome_[i];
              if (hv >= 0 && hv < (int)vessels_.size() && hv == phome_[j]) vessels_[hv].inverted++;
            }
          }
        }
      }
      V2 rn = d * (1.f / r);
      float u = Dot(pv_[i] - pv_[j], rn);
      if (u <= 0) continue;
      float q = r * invH;
      float sigma = 0.5f * (visc_[psub_[i]] + visc_[psub_[j]]);
      float I = (1 - q) * sigma * u;
      float mi = mass_[psub_[i]], mj = mass_[psub_[j]];
      float wi = mj / (mi + mj), wj = mi / (mi + mj);
      pv_[i] = pv_[i] - rn * (I * wi);
      pv_[j] = pv_[j] + rn * (I * wj);
    }
  }

  // 2. advect, with a speed cap so one step never tunnels a wall -- RELATIVE
  // TO ITS VESSEL'S FRAME: capped in the world, a flask carried at 2 px a
  // step left its liquid behind against the trailing glass.
  // From here to the velocity update (4), a framed particle's pprev_ is
  // where the step began CARRIED by the frame's shift: px_ - pprev_ is its
  // motion relative to the frame, and the grains' pull-back (CollideLiquid)
  // reads it against grains that took the same shift.
  const float vmax = cfg_.maxSpeedFrac * h;
  for (int i = 0; i < n; i++) {
    const int fv = pframe_[i];
    const V2 base = fv >= 0 ? vessels_[fv].frameVel : V2{0, 0};
    V2 rel = pv_[i] - base;
    const float s = Len(rel);
    if (s > vmax) {
      rel = rel * (vmax / s);
      pv_[i] = base + rel;
    }
    pprev_[i] = px_[i];
    if (fv >= 0) pprev_[i] = pprev_[i] + V2{(float)vessels_[fv].shiftX, (float)vessels_[fv].shiftY};
    px_[i] = pprev_[i] + rel;
  }

  // 3. double-density relaxation (Gauss-Seidel, Clavet §4). Number density,
  // not mass density: mass only decides who gives way.
  const float k = cfg_.stiffness, kn = cfg_.stiffnessNear;
  const float crossRest = cfg_.crossRest;
  const float h2 = h * h;
  for (int it = 0; it < cfg_.relaxIters; it++)
  for (int ii = 0; ii < n; ii++) {
    const int i = ((step_ + it) & 1) && cfg_.relaxAlternate ? n - 1 - ii : ii;
    const int e0 = nbrStart_[i], e1 = nbrStart_[i + 1];
    const float mi = mass_[psub_[i]];
    float rho = 0, rhoN = 0, rhoSame = 0, sw = 1.f, sm = mi;
    // i's pairs inside h, with their distance: the push below reuses them
    // (nothing moves between the two loops but the j already pushed).
    int nc = 0;
    if ((int)rcJ_.size() < e1 - e0) { rcJ_.resize(e1 - e0); rcQ_.resize(e1 - e0); rcN_.resize(e1 - e0); }
    for (int e = e0; e < e1; e++) {
      int j = nbr_[e];
      V2 d = px_[j] - px_[i];
      float r2 = Dot(d, d);
      if (r2 >= h2 || r2 < 1e-10f) continue;
      const float r = std::sqrt(r2);
      float q = r * invH;
      float w = (1 - q) * (1 - q);
      rcJ_[nc] = j;
      rcQ_[nc] = q;
      rcN_[nc] = d * (1.f / r);
      nc++;
      rho += w;
      rhoN += w * (1 - q);
      if (psub_[j] == psub_[i]) rhoSame += w;
      sw += w;
      sm += w * mass_[psub_[j]];
    }
    mbar_[i] = sm / sw;
    // A particle whose neighbours are other substances wants fewer of them:
    // its rest density drops toward crossRest, so the interface pushes.
    float same = rho > 1e-6f ? rhoSame / rho : 1.f;
    const float cross = cfg_.crossLone > 0 && same < cfg_.crossLone
                            ? 1.0f + (crossRest - 1.0f) * (same / cfg_.crossLone)
                            : crossRest;
    float rest = rho0_ * (same + cross * (1 - same));
    float P = k * (rho - rest);
    // Only the first pass PULLS (a negative P is Clavet's cohesion). The
    // second is there to hold up the bottom of a deep column, which is all
    // push; letting it pull too doubled the cohesion, and a pour left the lip
    // as a sticky glob wider than the mouth under it (alchemy-pour: 96 of 200
    // oil arrived).
    if (it > 0) P = std::max(P, 0.0f);
    float PN = kn * rhoN;
    V2 dx{0, 0};
    for (int c = 0; c < nc; c++) {
      const int j = rcJ_[c];
      const float q = rcQ_[c];
      float D = (P * (1 - q) + PN * (1 - q) * (1 - q)) * h * 0.5f;
      const V2 rn = rcN_[c];
      float mj = mass_[psub_[j]];
      float wj = mi / (mi + mj);  // j gives way in proportion to i's mass
      px_[j] = px_[j] + rn * (D * wj);
      dx = dx - rn * (D * (1 - wj));
    }
    px_[i] = px_[i] + dx;
  }

  CollideLiquid();

  // 4. velocity from the net displacement, then where the energy goes: a
  // linear drag, the calm-in of fresh particles, and XSPH smoothing over the
  // step's neighbours (a velocity pulled toward its neighbourhood's), which
  // is what stops a stirred mix from swirling for ever.
  // THE DRAG IS RELATIVE TO THE VESSEL the particle is in: `vv` is the
  // glass's own rigid motion at that point this step. Measured in the world
  // instead, a flask carried at 2 px a step had its liquid dragged back at
  // 1.6 g -- a sideways gravity that ran it up the trailing wall and out of
  // the mouth on every shake. Liquid in flight takes a plain air drag.
  struct Frame { bool moved; float c0, s0, c1, s1; V2 p0, p1; };
  std::vector<Frame> fr(vessels_.size());
  for (size_t k = 0; k < vessels_.size(); k++) {
    const Vessel& v = vessels_[k];
    fr[k].moved = !v.outline.empty() && (v.x.pos.x != v.prevX.pos.x || v.x.pos.y != v.prevX.pos.y ||
                                         v.x.angle != v.prevX.angle);
    fr[k].c0 = std::cos(-v.prevX.angle); fr[k].s0 = std::sin(-v.prevX.angle);
    fr[k].c1 = std::cos(v.x.angle); fr[k].s1 = std::sin(v.x.angle);
    fr[k].p0 = v.prevX.pos; fr[k].p1 = v.x.pos;
  }
  const float keep = 1.0f - cfg_.damping, keepAir = 1.0f - cfg_.airDamping;
  for (int i = 0; i < n; i++) {
    // (A framed particle's displacement is relative to its frame: its world
    // velocity is the frame's plus that -- the frame's smooth velocity, not
    // the whole-pixel shift, which only spends it.)
    const V2 vel = px_[i] - pprev_[i] + (pframe_[i] >= 0 ? vessels_[pframe_[i]].frameVel : V2{0, 0});
    const int hv = phome_[i];
    if (hv < 0 || hv >= (int)vessels_.size() || vessels_[hv].outline.empty()) {
      pv_[i] = vel * keepAir;
      continue;
    }
    V2 vv{0, 0};
    if (fr[hv].moved) {
      const Frame& f = fr[hv];
      const V2 d = px_[i] - f.p0;
      const V2 l{d.x * f.c0 - d.y * f.s0, d.x * f.s0 + d.y * f.c0};
      vv = V2{f.p1.x + l.x * f.c1 - l.y * f.s1, f.p1.y + l.x * f.s1 + l.y * f.c1} - px_[i];
    }
    pv_[i] = vv + (vel - vv) * keep;
    if (calm_[i]) {
      pv_[i] = vv + (pv_[i] - vv) * 0.5f;
      calm_[i]--;
    }
  }
  if (cfg_.xsph > 0) {
    xs_.assign(n, V2{});
    xw_.assign(n, 0.f);
    for (const auto& pr : pairs_) {
      const int i = pr.first, j = pr.second;
      const V2 d = px_[j] - px_[i];
      const float r2 = Dot(d, d);
      if (r2 >= h2) continue;
      const float q = std::sqrt(r2) * invH;
      const float w = (1 - q) * (1 - q);
      const V2 dv = (pv_[j] - pv_[i]) * w;
      xs_[i] = xs_[i] + dv;
      xs_[j] = xs_[j] - dv;
      xw_[i] += w;
      xw_[j] += w;
    }
    for (int i = 0; i < n; i++)
      if (xw_[i] > 1e-6f) pv_[i] = pv_[i] + xs_[i] * (cfg_.xsph / std::max(xw_[i], 1.0f));
  }
  int w = 0;
  const int total = (int)px_.size();
  for (int i = 0; i < total; i++) {
    // Only an awake particle can have left; the sleeping tail is copied down.
    if (i < n) {
      V2 p = px_[i];
      bool out = p.x < -4 || p.x > cfg_.gridW + 4 || p.y < 0 || p.y > cfg_.gridH + 64;
      if (out) {
        spilledUnits_[psub_[i]] += pw_[i];
        NoteExit(p.x, pw_[i]);
        NoteFrom(psrc_[i], psub_[i], pw_[i]);
        // What was dissolved in it leaves with it, as its powder.
        if (pmass_[i]) {
          const ChemSolute* sp = chem_.Species(psol_[i]);
          if (sp && sp->from >= 0) {
            spilledUnits_[sp->from] += pmass_[i];
            NoteFrom(psrc_[i], sp->from, pmass_[i]);
          }
        }
        continue;
      }
    }
    if (w != i) {
      px_[w] = px_[i]; pv_[w] = pv_[i]; pprev_[w] = pprev_[i];
      psub_[w] = psub_[i]; pw_[w] = pw_[i]; mbar_[w] = mbar_[i]; calm_[w] = calm_[i];
      phome_[w] = phome_[i]; psrc_[w] = psrc_[i]; pvar_[w] = pvar_[i]; panc_[w] = panc_[i]; pheat_[w] = pheat_[i];
      psol_[w] = psol_[i]; pmass_[w] = pmass_[i];
    }
    w++;
  }
  nAct_ -= total - w;
  px_.resize(w); pv_.resize(w); pprev_.resize(w); psub_.resize(w); pw_.resize(w); mbar_.resize(w);
  calm_.resize(w); phome_.resize(w); psrc_.resize(w); pvar_.resize(w); panc_.resize(w); pheat_.resize(w);
  psol_.resize(w); pmass_.resize(w);
}

void FlaskSim::CollideLiquid() {
  const int n = nAct_;
  const float pr = spacing_ * 0.45f;
  // The stick first, the glass after: whatever the stick shoves, the glass
  // has the last word, so a stick dragged against a wall cannot push liquid
  // through it. The stick is a capsule; pushed out along its normal.
  if (stickOn_) {
    for (int i = 0; i < n; i++) {
      V2 c = ClosestOnSeg(px_[i], stickA_, stickB_, nullptr);
      V2 d = px_[i] - c;
      float dist = Len(d);
      float R = stickR_ + pr;
      if (dist >= R) continue;
      V2 nrm = dist > 1e-5f ? d * (1.f / dist) : V2{0, 1};
      px_[i] = c + nrm * R;
    }
  }

  // Grains are walls. A particle that ended in a grain's pixel is pulled back
  // along its own path to where it entered that pixel (a continuous
  // collision, so it keeps no velocity INTO the grain and gains none). Only
  // if it started inside one too (the grain moved onto it) does it hop to
  // the nearest free neighbour.
  //
  // A grain IN FLIGHT (flung: moving, carried by the liquid it is in, or
  // falling) is no wall: the liquid passes round it, and the drag between
  // them (StepGrains, PayDrag) is how each moves the other. As a wall, the
  // grains of a falling blob held its liquid up on them.
  const int W = cfg_.gridW, H = cfg_.gridH;
  auto wallGrain = [&](size_t k) {
    const int gk = grid_[k];
    if (!gk) return false;
    const Grain& gr = grains_[gk - 1];
    const bool flying = (gr.vx != 0 || gr.vy != 0) && (gr.home < 0 || gr.vx * gr.vx + gr.vy * gr.vy > 0.25f);
    return !flying;
  };
  auto grainAt = [&](int x, int y) {
    if (x < 0 || y < 0 || x >= W || y >= H) return false;
    return wallGrain((size_t)y * W + x);
  };
  // Glass counts as solid for the pull-back: the glass may have moved onto
  // where the particle came from (a rising bottom), and pulling it back there
  // would put it through the wall.
  auto solid = [&](int x, int y) {
    if (x < 0 || y < 0 || x >= W || y >= H) return false;
    size_t k = (size_t)y * W + x;
    return wallGrain(k) || wall_[k] != 0;
  };
  deadEpoch_++;
  for (int i = 0; i < n; i++) {
    int x = (int)std::floor(px_[i].x), y = (int)std::floor(px_[i].y);
    if (!grainAt(x, y)) continue;
    V2 a = pprev_[i], b = px_[i];
    if (!solid((int)std::floor(a.x), (int)std::floor(a.y))) {
      // Bisect for the last free point on a..b.
      float lo = 0, hi = 1;
      for (int it = 0; it < 8; it++) {
        float m = 0.5f * (lo + hi);
        V2 p = a + (b - a) * m;
        if (solid((int)std::floor(p.x), (int)std::floor(p.y))) hi = m;
        else lo = m;
      }
      px_[i] = a + (b - a) * lo;
      continue;
    }
    float best = 1e9f;
    int bx = -1, by = -1;
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++) {
        int xx = x + dx, yy = y + dy;
        if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
        size_t k = (size_t)yy * W + xx;
        if (wallGrain(k) || wall_[k]) continue;
        float dd = (float)(dx * dx + dy * dy) - (dy > 0 ? 0.5f : 0.f);
        if (dd < best) { best = dd; bx = xx; by = yy; }
      }
    if (bx >= 0) {
      V2 d{(float)(bx - x), (float)(by - y)};
      px_[i] = px_[i] + d;
      pprev_[i] = pprev_[i] + d;  // carried, not launched
      continue;
    }
    // BURIED: grains (and glass) all round it -- the glass swept a grain onto
    // it, or a carried grain was held back while the liquid moved on. The
    // liquid SEEPS OUT: it goes to the nearest free pixel reached through
    // the grains (on its own side of the glass, within kSeepReach pixels),
    // and the bed keeps its grains. It used to shove the grain instead,
    // along the pile to the nearest free pixel -- mostly a liquid pixel, so
    // the next particle was buried in its turn: a tilted flask churned its
    // sand bed into a sponge of liquid and air (lab: 600 grain chains a
    // second, the bed 5-10% bigger while it moved).
    {
      constexpr int kSeepReach = 320;   // pixels searched, about a 10 px radius
      const uint8_t side = inside_.empty() ? 0 : inside_[(size_t)y * W + x];
      bfsQueue_.clear();
      if (bfsSeen_.size() != (size_t)W * H) bfsSeen_.assign((size_t)W * H, 0);
      if (++bfsStamp_ == 0) { std::fill(bfsSeen_.begin(), bfsSeen_.end(), 0); bfsStamp_ = 1; }
      const int s0 = y * W + x;
      bfsQueue_.push_back(s0);
      bfsSeen_[s0] = bfsStamp_;
      int found = -1;
      static const int kD[4][2] = {{0, 1}, {-1, 0}, {1, 0}, {0, -1}};
      for (size_t q = 0; q < bfsQueue_.size() && found < 0 && bfsQueue_.size() < kSeepReach; q++) {
        const int cur = bfsQueue_[q], cx = cur % W, cy = cur / W;
        for (const auto& d : kD) {
          const int xx = cx + d[0], yy = cy + d[1];
          if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
          const int k = yy * W + xx;
          if (bfsSeen_[k] == bfsStamp_ || wall_[k]) continue;
          if (!inside_.empty() && inside_[k] != side) continue;
          bfsSeen_[k] = bfsStamp_;
          if (!wallGrain((size_t)k)) { found = k; break; }
          bfsQueue_.push_back(k);
        }
      }
      if (found >= 0) {
        const V2 d{(float)(found % W - x), (float)(found / W - y)};
        px_[i] = px_[i] + d;
        pprev_[i] = pprev_[i] + d;  // carried, not launched
        continue;
      }
    }
    // (one sweep memo epoch for this pass)
    // TRAPPED past the seep's reach (liquid squeezed between a rising
    // bottom and a sand bed it cannot push). The liquid wins: the GRAIN is
    // shoved along its pile to the nearest free pixel on its side, which is
    // how pressure lifts a bed. Without this the particle stayed inside the
    // grain and the next pull-back put it through the glass.
    //
    // ...but only while that vessel MOVES. At rest there is no glass
    // squeezing anything, only the relaxation's shimmer nudging a particle
    // into the bed, and shoving grains for it churned the sand for ever (and
    // kept the vessel awake): the particle looks a little further for room,
    // or waits a step.
    const int gi = grid_[(size_t)y * W + x] - 1;
    const int home = grains_[gi].home;
    const bool live = home >= 0 && home < (int)vessels_.size() && !vessels_[home].outline.empty();
    const bool moving = live && (vessels_[home].x.pos.x != vessels_[home].prevX.pos.x ||
                                 vessels_[home].x.pos.y != vessels_[home].prevX.pos.y ||
                                 vessels_[home].x.angle != vessels_[home].prevX.angle);
    if (moving) {
      SweepChain(gi, vessels_[home], true);
      continue;
    }
    bool placed = false;
    for (int dy = 2; dy >= -2 && !placed; dy--)
      for (int dx = -2; dx <= 2 && !placed; dx++) {
        const int xx = x + dx, yy = y + dy;
        if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
        const size_t k = (size_t)yy * W + xx;
        if (wallGrain(k) || wall_[k]) continue;
        const V2 d{(float)dx, (float)dy};
        px_[i] = px_[i] + d;
        pprev_[i] = pprev_[i] + d;
        placed = true;
      }
  }

  // (The glass goes LAST, after the grains: it rewrites pprev_ into the
  // velocity the contact leaves, and the grains' pull-back walks a particle
  // back toward pprev_ as if it were where the step began -- which, for a
  // particle on the glass, could be the far side of it. Oil under a sand bed
  // dripped out through the bottom of a flask that way.)

  // THE GLASS. Each particle is on one SIDE of each vessel's glass -- inside
  // or outside the outline closed across the mouth -- and the glass keeps it
  // there. The side is decided by the polygon test at both ends of the step
  // (each in the vessel's frame at its own time, so the wall's motion is the
  // particle's relative motion): a particle that changed side without going
  // through the mouth went through the glass, and goes back to the side it
  // came from. The old per-segment sign test missed a crossing at a vertex
  // (t clamped to an end) and then pushed the particle out along the vector
  // from the glass -- further out -- which is how a carried flask seeped.
  for (const Vessel& v : vessels_) {
    if (v.outline.empty()) continue;
    const float R = v.shape.wall + pr;
    const auto& o = v.outline;
    // A STOPPERED mouth is one more segment (right lip -> left lip, which
    // continues the loop, so its inside is on its left like the rest).
    const size_t nGlass = std::min<size_t>(o.size() - 1, 64);
    const size_t ns = nGlass + (v.stoppered ? 1 : 0);
    V2 sa[65], sb[65];
    for (size_t k = 0; k < nGlass; k++) { sa[k] = o[k]; sb[k] = o[k + 1]; }
    if (v.stoppered) { sa[nGlass] = o.back(); sb[nGlass] = o.front(); }
    // Inward normals: the outline runs left lip -> bottom -> right lip, so
    // the inside is on the left of every segment.
    V2 nIn[65];
    for (size_t k = 0; k < ns; k++) {
      const V2 ab = sb[k] - sa[k];
      const float l = std::max(1e-6f, Len(ab));
      nIn[k] = {-ab.y / l, ab.x / l};
    }
    const V2 mouthA = o.front(), mouthB = o.back();
    const float c0 = std::cos(-v.x.angle), s0 = std::sin(-v.x.angle);
    const float c1 = std::cos(-v.prevX.angle), s1 = std::sin(-v.prevX.angle);
    const float cw = std::cos(v.x.angle), sw = std::sin(v.x.angle);
    float bx0 = 1e9f, bx1 = -1e9f, by0 = 1e9f, by1 = -1e9f;
    for (V2 p : o) { bx0 = std::min(bx0, p.x); bx1 = std::max(bx1, p.x); by0 = std::min(by0, p.y); by1 = std::max(by1, p.y); }
    bx0 -= R + 1; bx1 += R + 1; by0 -= R + 1; by1 += R + 1;
    auto segCross = [](V2 p1, V2 q1, V2 p2, V2 q2) {
      const float d1 = Cross(q2 - p2, p1 - p2), d2 = Cross(q2 - p2, q1 - p2);
      const float d3 = Cross(q1 - p1, p2 - p1), d4 = Cross(q1 - p1, q2 - p1);
      return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
    };
    for (int i = 0; i < n; i++) {
      // Where the step began, in the world: pprev_ less the frame's carry.
      const int fi = (size_t)i < pframe_.size() ? pframe_[i] : -1;
      const V2 shift = fi >= 0 ? V2{(float)vessels_[fi].shiftX, (float)vessels_[fi].shiftY} : V2{0, 0};
      V2 d0 = px_[i] - v.x.pos, d1 = pprev_[i] - shift - v.prevX.pos;
      V2 cur{d0.x * c0 - d0.y * s0, d0.x * s0 + d0.y * c0};
      const V2 prv{d1.x * c1 - d1.y * s1, d1.x * s1 + d1.y * c1};
      if ((cur.x < bx0 || cur.x > bx1 || cur.y < by0 || cur.y > by1) &&
          (prv.x < bx0 || prv.x > bx1 || prv.y < by0 || prv.y > by1))
        continue;
      // Far from the glass at both ends, and on the same side: nothing to
      // do (a step moves a particle less than the margin, relative to the
      // glass). Most of a vessel's liquid is in here.
      const uint8_t mc = v.Near(cur), mp = v.Near(prv);
      if (mc == mp && (mc == Vessel::kFarIn || mc == Vessel::kFarOut)) continue;
      const bool inPrev = InsideLocal(v, prv), inCur = InsideLocal(v, cur);
      // Which side it belongs on: where it came from, unless it came through
      // the mouth (then it is simply somewhere new).
      const bool crossed = inPrev != inCur && (v.stoppered || !segCross(prv, cur, mouthA, mouthB));
      const bool side = crossed ? inPrev : inCur;
      const float sg = side ? 1.0f : -1.0f;
      bool moved = false;
      V2 hitN{0, 0}, hitC{0, 0};
      if (crossed) {
        // Back through the nearest glass, to its own side of it.
        float best = 1e30f;
        size_t bs = 0;
        V2 bc{};
        for (size_t k = 0; k < ns; k++) {
          const V2 c = ClosestOnSeg(cur, sa[k], sb[k], nullptr);
          const float dd = Dot(cur - c, cur - c);
          if (dd < best) { best = dd; bs = k; bc = c; }
        }
        hitN = nIn[bs] * sg;
        hitC = bc;
        cur = bc + hitN * R;
        moved = true;
      }
      // Kept a glass-thickness off every segment, on its side.
      for (size_t k = 0; k < ns; k++) {
        float t;
        const V2 c = ClosestOnSeg(cur, sa[k], sb[k], &t);
        const V2 d = cur - c;
        const float dist = Len(d);
        if (dist >= R) continue;
        V2 nrm;
        // Off the end of a segment (a vertex, a lip) the way out is straight
        // away from it; along its middle, the segment's own normal to the
        // particle's side -- never the vector from the glass, which points to
        // the wrong side for a particle already past the line.
        const bool end = t <= 0.0f || t >= 1.0f;
        if (dist > 1e-5f && (end || Dot(d, nIn[k]) * sg > 0)) nrm = d * (1.f / dist);
        else nrm = nIn[k] * sg;
        cur = c + nrm * R;
        hitN = nrm;
        hitC = c;
        moved = true;
      }
      if (!moved) continue;
      // Last word: still on its side, or back where it was (relative to the
      // glass) -- a push at a narrow neck must not become a crossing.
      if (InsideLocal(v, cur) != side && InsideLocal(v, prv) == side) cur = prv;
      px_[i] = {v.x.pos.x + cur.x * cw - cur.y * sw, v.x.pos.y + cur.x * sw + cur.y * cw};
      // THE CONTACT'S VELOCITY. The push above is a position correction,
      // and a position-based step turns every correction into velocity: a
      // wall that moved 2 px launched the particle at the overlap it had
      // to undo, not at the wall's speed. Relative to the glass at the
      // contact, the part into or off the wall is zeroed (no bounce) and
      // the part along it loses `wallFriction`.
      const V2 cw1 = ToWorld(v.x, hitC), cw0 = ToWorld(v.prevX, hitC);
      const V2 vw = cw1 - cw0;
      const V2 nw{hitN.x * cw - hitN.y * sw, hitN.x * sw + hitN.y * cw};
      // (In velocities: a framed particle's is its frame's plus px_ - pprev_,
      // as the update (4) reads it.)
      const V2 fvel = fi >= 0 ? vessels_[fi].frameVel : V2{0, 0};
      V2 rel = (px_[i] - pprev_[i] + fvel) - vw;
      rel = rel - nw * Dot(rel, nw);
      rel = rel * (1.0f - cfg_.wallFriction);
      pprev_[i] = px_[i] - (vw + rel) + fvel;
    }
  }

}

// ---- powder ----------------------------------------------------------------

bool FlaskSim::GrainFree(int x, int y) const {
  if (x < 0 || y < 0 || x >= cfg_.gridW || y >= cfg_.gridH) return false;
  size_t k = (size_t)y * cfg_.gridW + x;
  return !grid_[k] && !wall_[k];
}

bool FlaskSim::PlaceGrain(Grain g, int nx, int ny, int within) {
  // Nearest free pixel within a small ring search.
  for (int r = 0; r <= 4; r++)
    for (int dy = -r; dy <= r; dy++)
      for (int dx = -r; dx <= r; dx++) {
        if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
        int x = nx + dx, y = ny + dy;
        if (!GrainFree(x, y)) continue;
        if (within >= -1 && inside_[(size_t)y * cfg_.gridW + x] != (uint8_t)(within + 1)) continue;
        g.x = (int16_t)x;
        g.y = (int16_t)y;
        if (g.vx == 0 && g.vy == 0) { g.fx = x + 0.5f; g.fy = y + 0.5f; }
        grains_.push_back(g);
        grid_[(size_t)y * cfg_.gridW + x] = (int)grains_.size();
        if (!awake_.empty()) Wake(x, y);
        return true;
      }
  return false;
}

// A powder PRODUCT appears where what made it was: salt refreezing out of
// molten salt at the bottom of a pile over the burner. That spot is buried,
// PlaceGrain's 4-px ring is full, and the units went to a pool whose centroid
// stayed buried -- counted in the tally, drawn nowhere, retried every step
// (half a flask of salt vanished that way, 2026-09-27). Past the ring, the
// nearest free pixel inside the same vessel: the grain surfaces at the edge
// or top of the pile and falls from there. Ring perimeters only, so a vessel
// that is genuinely full costs ~8*R^2/2 probes, once per flush.
bool FlaskSim::PlaceDeposit(Grain g, int nx, int ny, int within) {
  if (PlaceGrain(g, nx, ny, within)) return true;
  constexpr int kDepositReach = 64;
  const int W = cfg_.gridW;
  auto tryAt = [&](int x, int y) {
    if (!GrainFree(x, y)) return false;
    if (within >= -1 && inside_[(size_t)y * W + x] != (uint8_t)(within + 1)) return false;
    g.x = (int16_t)x;
    g.y = (int16_t)y;
    g.fx = x + 0.5f;
    g.fy = y + 0.5f;
    g.vx = g.vy = 0;
    grains_.push_back(g);
    grid_[(size_t)y * W + x] = (int)grains_.size();
    if (!awake_.empty()) Wake(x, y);
    return true;
  };
  for (int r = 5; r <= kDepositReach; r++) {
    // Top row first (a pile surfaces upward), then the sides, then the bottom.
    for (int dx = -r; dx <= r; dx++)
      if (tryAt(nx + dx, ny + r)) return true;
    for (int dy = r - 1; dy > -r; dy--)
      if (tryAt(nx - r, ny + dy) || tryAt(nx + r, ny + dy)) return true;
    for (int dx = -r; dx <= r; dx++)
      if (tryAt(nx + dx, ny - r)) return true;
  }
  return false;
}

int FlaskSim::EmitGrains(int sub, V2 at, int units, V2 vel) {
  int placed = 0;
  for (int k = 0; k < units; k++) {
    Grain g{};
    g.sub = (uint8_t)sub;
    g.variant = (uint8_t)(Rand() % 3);
    float jx = ((Rand() & 255) / 255.f - 0.5f) * 2.f;
    g.fx = at.x + jx;
    g.fy = at.y;
    g.vx = vel.x + jx * 0.05f;
    g.vy = vel.y == 0 && vel.x == 0 ? -0.01f : vel.y;
    if (PlaceGrain(g, (int)std::floor(g.fx), (int)std::floor(g.fy))) placed++;
  }
  if (sub >= 0 && sub < (int)seeded_.size()) seeded_[sub] += placed;
  return placed;
}

void FlaskSim::BucketPixels() {
  const int W = cfg_.gridW, H = cfg_.gridH;
  const int n = (int)px_.size();
  // The smoothed liquid field (the same splat the renderer draws): a pixel
  // is liquid when enough particle footprint covers it, and its mass and
  // velocity are the footprint-weighted means. One pixel is far smaller than
  // a particle, so reading particle CENTRES leaves holes grains sift through.
  // Only last step's written box is cleared, not the grid.
  const size_t N = (size_t)W * H;
  if (fieldW_.size() != N) {
    fieldW_.assign(N, 0.f);
    fieldM_.assign(N, 0.f);
    fieldV_.assign(N, V2{});
    fieldVisc_.assign(N, 0.f);
    pixHead_.assign(N, -1);
  }
  for (int y = fby0_; y <= fby1_; y++) {
    const size_t k0 = (size_t)y * W + fbx0_, len = (size_t)(fbx1_ - fbx0_ + 1);
    std::fill_n(&fieldW_[k0], len, 0.f);
    std::fill_n(&fieldM_[k0], len, 0.f);
    std::fill_n(&fieldV_[k0], len, V2{});
    std::fill_n(&fieldVisc_[k0], len, 0.f);
  }
  for (int k : pixOf_)
    if (k >= 0) pixHead_[k] = -1;
  const float R = spacing_ * 1.05f, R2 = R * R;
  const int ri = (int)std::ceil(R);
  fbx0_ = W; fby0_ = H; fbx1_ = -1; fby1_ = -1;
  pixOf_.assign(n, -1);
  pixNext_.resize(n);
  for (int i = 0; i < n; i++) {
    int cx = (int)std::floor(px_[i].x), cy = (int)std::floor(px_[i].y);
    if (cx < -ri || cy < -ri || cx >= W + ri || cy >= H + ri) continue;
    // Moving liquid keeps the sand round it awake (a grain reads it to sink
    // or float); a sleeping vessel's liquid does not.
    if (i < nAct_ && cx >= 0 && cy >= 0 && cx < W && cy < H) {
      Wake(cx - 4, cy - 4);
      Wake(cx + 4, cy + 4);
    }
    // Only liquid within a tile of sand is read by the sand (and by the
    // carry's support test); the rest is not splatted.
    {
      const int tx = cx >> 4, ty = cy >> 4;
      bool near = false;
      for (int oy = -1; oy <= 1 && !near; oy++)
        for (int ox = -1; ox <= 1 && !near; ox++) {
          const int ax = tx + ox, ay = ty + oy;
          near = ax >= 0 && ay >= 0 && ax < tilesW_ && ay < tilesH_ && sandTile_[ay * tilesW_ + ax];
        }
      if (!near) continue;
    }
    fbx0_ = std::min(fbx0_, std::max(0, cx - ri));
    fbx1_ = std::max(fbx1_, std::min(W - 1, cx + ri));
    fby0_ = std::min(fby0_, std::max(0, cy - ri));
    fby1_ = std::max(fby1_, std::min(H - 1, cy + ri));
    float m = mass_[psub_[i]], vi = visc_[psub_[i]];
    for (int dy = -ri; dy <= ri; dy++)
      for (int dx = -ri; dx <= ri; dx++) {
        int x = cx + dx, y = cy + dy;
        if (x < 0 || y < 0 || x >= W || y >= H) continue;
        float ex = x + 0.5f - px_[i].x, ey = y + 0.5f - px_[i].y;
        float d2 = ex * ex + ey * ey;
        if (d2 >= R2) continue;
        float w = 1.f - d2 / R2;
        w *= w;
        size_t k = (size_t)y * W + x;
        fieldW_[k] += w;
        fieldM_[k] += w * m;
        fieldV_[k] = fieldV_[k] + pv_[i] * w;
        fieldVisc_[k] += w * vi;
      }
    if (cx < 0 || cy < 0 || cx >= W || cy >= H) continue;
    const int k = cy * W + cx;
    pixOf_[i] = k;
    pixNext_[i] = pixHead_[k];
    pixHead_[k] = i;
  }
}

// The liquid at a pixel, from the smoothed field: mean mass (0 = no liquid),
// and optionally its velocity and viscosity.
float FlaskSim::LiquidMassAt(int x, int y, int* count, V2* vel) const {
  const int W = cfg_.gridW, H = cfg_.gridH;
  if (count) *count = 0;
  if (vel) *vel = V2{};
  if (x < 0 || y < 0 || x >= W || y >= H) return 0.f;
  size_t k = (size_t)y * W + x;
  float w = fieldW_[k];
  if (w <= kLiquidThresh) return 0.f;
  if (count) *count = 1;
  if (vel) *vel = fieldV_[k] * (1.f / w);
  return fieldM_[k] / w;
}

float FlaskSim::WetSettle(const Grain& g, float ml, size_t k) const {
  const float mg = mass_[g.sub];
  float p = std::clamp(1.f - ml / mg, 0.05f, 1.f) * 0.6f;
  // The liquid's thickness (the field's weighted viscosity at the pixel).
  if (k < fieldW_.size() && fieldW_[k] > 1e-6f) p /= 1.f + 8.f * fieldVisc_[k] / fieldW_[k];
  return p;
}

void FlaskSim::PayDrag(const Grain& g, V2 dv, float mg) {
  // The awake particles bucketed in the grain's pixel and the 3x3 round it
  // (BucketPixels, this step), sharing the grain's momentum change (a grain
  // is one unit; a particle pw_ units) by their share of the mass.
  const int W = cfg_.gridW, H = cfg_.gridH;
  if (pixHead_.empty() || (dv.x == 0 && dv.y == 0)) return;
  int idx[32], n = 0;
  float msum = 0;
  for (int dy = -1; dy <= 1; dy++)
    for (int dx = -1; dx <= 1; dx++) {
      const int x = g.x + dx, y = g.y + dy;
      if (x < 0 || y < 0 || x >= W || y >= H) continue;
      for (int i = pixHead_[(size_t)y * W + x]; i >= 0 && n < 32; i = pixNext_[i]) {
        if (i >= nAct_) continue;
        idx[n++] = i;
        msum += mass_[psub_[i]] * pw_[i];
      }
    }
  if (n == 0 || msum <= 0) return;
  const V2 dvl = dv * (-mg / msum);
  for (int c = 0; c < n; c++) pv_[idx[c]] = pv_[idx[c]] + dvl;
}

void FlaskSim::ShoveLiquid(int fromX, int fromY, int toX, int toY) {
  // A grain moved from (from) into (to): whatever liquid was in (to) takes
  // the pixel it vacated. Keeps sub-pixel offsets, keeps velocity.
  const int W = cfg_.gridW;
  size_t k = (size_t)toY * W + toX;
  for (int i = pixHead_[k]; i >= 0; i = pixNext_[i]) {
    V2 d{(float)(fromX - toX), (float)(fromY - toY)};
    px_[i] = px_[i] + d;
    pprev_[i] = pprev_[i] + d;
  }
}

void FlaskSim::MoveGrain(int gi, int x, int y, bool shove) {
  grainMoves_++;
  Grain& g = grains_[gi];
  Wake(g.x, g.y);
  Wake(x, y);
  const int W = cfg_.gridW;
  grid_[(size_t)g.y * W + g.x] = 0;
  if (shove) ShoveLiquid(g.x, g.y, x, y);
  g.x = (int16_t)x;
  g.y = (int16_t)y;
  // A resting grain's sub-pixel position is its pixel's centre. Left as it
  // was, the carry (CarryGrains, which moves fx/fy) put every grain that had
  // fallen since the last carry back where it had been: sand that snapped
  // up into the air and poured down again whenever the vessel moved.
  if (g.vx == 0 && g.vy == 0) { g.fx = x + 0.5f; g.fy = y + 0.5f; }
  grid_[(size_t)y * W + x] = gi + 1;
  // It moved inside a vessel: that vessel's liquid must not sleep through it.
  const uint8_t in = inside_.empty() ? 0 : inside_[(size_t)y * W + x];
  if (in && in <= vessels_.size()) vessels_[in - 1].grainBusy = true;
}

void FlaskSim::StepGrains() {
  if (grains_.empty()) return;
  const int W = cfg_.gridW, H = cfg_.gridH;
  tilesW_ = (W + 15) >> 4;
  tilesH_ = (H + 15) >> 4;
  if ((int)awake_.size() != tilesW_ * tilesH_) {
    awake_.assign((size_t)tilesW_ * tilesH_, 1);
    awakeNext_.assign((size_t)tilesW_ * tilesH_, 1);
    wakeAll_ = true;
  }
  if (wakeAll_) std::fill(awakeNext_.begin(), awakeNext_.end(), 1);
  wakeAll_ = false;
  awake_.swap(awakeNext_);
  std::fill(awakeNext_.begin(), awakeNext_.end(), 0);
  sandTile_.assign((size_t)tilesW_ * tilesH_, 0);
  for (const Grain& g : grains_) sandTile_[(g.y >> 4) * tilesW_ + (g.x >> 4)] = 1;
  BucketPixels();

  // Bottom rows first, alternating sweep direction per step, so a pile
  // does not lean the way the loop runs.
  // Bucketed by row (a counting sort; grains never outnumber pixels).
  std::vector<int> rowStart(H + 1, 0), order(grains_.size());
  for (const Grain& g : grains_) rowStart[g.y + 1]++;
  for (int y = 0; y < H; y++) rowStart[y + 1] += rowStart[y];
  {
    std::vector<int> fill(rowStart.begin(), rowStart.end() - 1);
    for (size_t i = 0; i < grains_.size(); i++) order[fill[grains_[i].y]++] = (int)i;
  }
  if (step_ & 1) {
    for (int y = 0; y < H; y++) std::reverse(order.begin() + rowStart[y], order.begin() + rowStart[y + 1]);
  }

  std::vector<int> dead;
  for (int gi : order) {
    Grain& g = grains_[gi];
    const float mg = mass_[g.sub];

    if (g.vx != 0 || g.vy != 0) {
      // FLUNG: ballistic -- or, in liquid, CARRIED BY IT (below).
      int cnt;
      V2 lv;
      const float ml = LiquidMassAt(g.x, g.y, &cnt, &lv);
      const V2 fvel = frameVelOf(g);
      // Does its walk shove the liquid it moves into (MoveGrain)? Only if it
      // moves THROUGH the liquid; riding with it, it would push its own
      // liquid back a pixel at every pixel it crossed.
      V2 relLiq{g.vx, g.vy};
      bool wetStill = false;   // floating in liquid that is not moving: back to the CA
      if (cnt) {
        // A GRAIN IN LIQUID moves with the liquid, and settles through it
        // only where the liquid is held up. Its velocity (in its vessel's
        // frame, as the liquid's is taken here) is dragged toward the
        // liquid's; inside a vessel it is pulled down by its weight LESS the
        // liquid's buoyancy, so it settles at the speed the CA sinks it
        // (WetSettle: the drag is set so the terminal speed is that); out of
        // every vessel the liquid is in flight, weightless, and nothing
        // settles through it -- the grain falls with it (Maxey-Riley with
        // the fluid's own acceleration: a free-falling blob keeps its
        // grains). The drag is paid back to the liquid round it, equal and
        // opposite (momentum in units): the liquid feels what it moves, and
        // a sinking grain pulls a little liquid down with it.
        const V2 lr = lv - fvel;
        const float r = ml / mg;
        const float p = WetSettle(g, ml, (size_t)g.y * W + g.x);
        const bool inside = g.home >= 0;
        const float buoy = inside ? cfg_.gravity * (1.0f - r) : cfg_.gravity;
        // The drag rate that makes g |1 - r| / k the settling speed p, and
        // in flight at least a quarter a step (it keeps up with its liquid).
        const float k = std::clamp(cfg_.gravity * std::fabs(1.0f - r) / std::max(p, 0.01f), 0.02f, 1.0f);
        const float kk = inside ? k : std::max(k, 0.25f);
        g.vy -= buoy;
        const V2 dv{(lr.x - g.vx) * kk, (lr.y - g.vy) * kk};
        g.vx += dv.x;
        g.vy += dv.y;
        // Relative to its liquid it never outruns the liquid's own cap.
        const V2 rel{g.vx - lr.x, g.vy - lr.y};
        const float rs = Len(rel), vcap = cfg_.maxSpeedFrac * h_;
        if (rs > vcap) { g.vx = lr.x + rel.x * (vcap / rs); g.vy = lr.y + rel.y * (vcap / rs); }
        // The liquid round it takes the drag back.
        PayDrag(g, dv, mg);
        relLiq = V2{g.vx - lr.x, g.vy - lr.y};
        wetStill = ml >= mg && Len(lr) < 0.25f;
        if (g.vx == 0 && g.vy == 0) g.vy = -1e-4f;   // still in the flung state
      } else {
        // Gravity takes it to grainMaxFall and no further; a grain already
        // going faster (it left a vessel moving down faster) keeps its speed.
        const float vy0 = g.vy;
        g.vy -= cfg_.gravity;
        if (cfg_.grainFlow) g.vy = std::max(g.vy, std::min(vy0, -cfg_.grainMaxFall));
      }
      float nx = g.fx + g.vx, ny = g.fy + g.vy;
      if (nx < 0 || nx >= W || ny < 0) {
        spilledUnits_[g.sub] += 1;
        NoteExit((float)g.x + 0.5f, 1);
        NoteFrom(g.src, g.sub, 1);
        grid_[(size_t)g.y * W + g.x] = 0;
        dead.push_back(gi);
        Wake(g.x, g.y);
        continue;
      }
      // Walk pixel by pixel toward the target; stop at the first blocked one.
      int steps = (int)std::ceil(std::max(std::fabs(g.vx), std::fabs(g.vy)));
      bool blocked = false;
      int blocker = -1;   // the grain it ran into, if one
      for (int s = 1; s <= steps; s++) {
        float t = (float)s / steps;
        int tx = (int)std::floor(g.fx + g.vx * t), ty = (int)std::floor(g.fy + g.vy * t);
        if (ty >= H) ty = H - 1;
        if (tx == g.x && ty == g.y) continue;
        if (!GrainFree(tx, ty)) {
          blocked = true;
          if (tx >= 0 && ty >= 0 && tx < W) blocker = grid_[(size_t)ty * W + tx] - 1;
          break;
        }
        MoveGrain(gi, tx, ty, (float)(tx - g.x) * relLiq.x + (float)(ty - g.y) * relLiq.y > 0.05f);
      }
      // IN A STREAM: it ran into a grain that is itself falling. That is not
      // a landing -- it follows it, taking on its speed. Landed on (and
      // splashed off, below), the grains of one pour kicked each other out
      // sideways in mid-air and the stream smoked into a spray.
      if (blocked && cfg_.grainFlow && blocker >= 0) {
        const Grain& b = grains_[blocker];
        // (A resting grain with air under it is falling too, by the CA.)
        // Air under it, not liquid: a grain floating in the water's top pixel
        // has nothing but grain-free liquid under it and is not falling --
        // taken for falling, the grain on it followed it for ever and the
        // vessel never slept.
        int cbl = 0;
        if (b.vx == 0 && b.vy == 0) LiquidMassAt(b.x, b.y - 1, &cbl, nullptr);
        const bool bFalls = b.vx == 0 && b.vy == 0 && !cbl && GrainFree(b.x, b.y - 1);
        if (b.vx != 0 || b.vy != 0 || bFalls) {
          g.vx = 0.5f * (g.vx + b.vx);
          g.vy = std::max(g.vy, bFalls ? -1.0f : b.vy);
          if (g.vx == 0 && g.vy == 0) g.vy = -cfg_.gravity;
          g.fx = g.x + 0.5f;
          g.fy = g.y + 0.5f;
          continue;
        }
      }
      if (blocked) {
        g.vx *= 0.3f;
        // LANDING: a falling grain that hits something turns part of its
        // fall into a run sideways -- its own way, or either at random if it
        // came straight down -- so a stream builds a spreading heap that
        // cascades, not a spike.
        if (cfg_.grainFlow && g.vy < -0.3f) {
          const float side = std::fabs(g.vx) > 0.05f ? (g.vx > 0 ? 1.f : -1.f) : ((Rand() & 1) ? 1.f : -1.f);
          g.vx += side * -g.vy * cfg_.grainSplash;
        }
        g.vy = 0;
        g.fx = g.x + 0.5f;
        g.fy = g.y + 0.5f;
      } else {
        g.fx = nx;
        g.fy = std::min(ny, H - 0.5f);
      }
      if (std::fabs(g.vx) + std::fabs(g.vy) < 0.35f && (blocked || wetStill || !GrainFree(g.x, g.y - 1))) {
        // Comes to rest still sliding the way it was going.
        g.slide = (int8_t)(std::fabs(g.vx) > 0.08f ? (g.vx > 0 ? 2 : -2) : 0);
        g.vx = g.vy = 0;
        g.fx = g.x + 0.5f;
        g.fy = g.y + 0.5f;
      }
      continue;
    }

    // RESTING grain in a sleeping tile: nothing near it changed, so it would
    // fail every move it failed last step.
    if (!awake_[(g.y >> 4) * tilesW_ + (g.x >> 4)]) continue;
    // RESTING grain: the CA. One still inside the glass (the sweep could
    // not place it) waits for the glass to move off it; letting it fall
    // would drop it out through the wall.
    if (wall_[(size_t)g.y * W + g.x]) continue;
    if (g.y == 0) {
      // Falls out of the panel.
      if (!wall_[(size_t)g.x]) {
        spilledUnits_[g.sub] += 1;
        NoteExit((float)g.x + 0.5f, 1);
        NoteFrom(g.src, g.sub, 1);
        grid_[(size_t)g.y * W + g.x] = 0;
        dead.push_back(gi);
        Wake(g.x, g.y);
      }
      continue;
    }
    // (One pass through the rules; each `continue` below ends it, and the
    // grain's way is booked after.)
    const int ox = g.x, oy = g.y, s0 = g.slide;
    g.slide = 0;
    do {
    // WET: liquid in its pixel. It goes DYNAMIC -- the flung branch, carried
    // by its liquid and settling through it -- whenever nothing holds it:
    // heavier than the liquid with no resting grain or glass under it (a
    // grain that is itself moving holds nothing up), anywhere out of every
    // vessel (liquid in flight carries what is in it), or with the liquid
    // running past it (relative to its vessel) faster than kEntrainFree --
    // or, held or not, faster than kEntrainHeld (a hard slosh lifts a bed's
    // top). Left to the CA as a still pixel, a grain in a falling blob of
    // slime sank through it at the CA's settling rate, a tenth of a pixel a
    // step, and held the slime up on it: the blob hovered.
    if (cfg_.grainFlow) {
      int cs;
      V2 lvs;
      const float ml = LiquidMassAt(g.x, g.y, &cs, &lvs);
      if (cs) {
        const V2 lr = lvs - frameVelOf(g);
        const float sp = Len(lr);
        // Held: glass or a grain under it -- unless that grain is falling
        // away (a blob's grains leave together; a grain that only shuffles in
        // a bed holds up what is on it). Out of every vessel any grain going
        // down under it lets it go, at that grain's speed: a packed bed in a
        // falling blob leaves as one, not a row at a time.
        bool held = !GrainFree(g.x, g.y - 1);
        float follow = 0;
        if (held) {
          const int b = grid_[(size_t)(g.y - 1) * W + g.x];
          if (b && !wall_[(size_t)(g.y - 1) * W + g.x]) {
            const Grain& gb = grains_[b - 1];
            if (gb.vy < (g.home < 0 ? 0.0f : -0.3f) && (gb.vx != 0 || gb.vy != 0)) {
              held = false;
              follow = gb.vy;
            }
          }
        }
        // Sinking needs liquid to sink INTO: a wet grain over an air gap (a
        // pocket in a bed) is left to the CA, which drops it into the gap.
        int cb = 0;
        if (!held && ml < mg) LiquidMassAt(g.x, g.y - 1, &cb, nullptr);
        constexpr float kEntrainFree = 0.25f, kEntrainHeld = 1.0f;
        if ((!held && ((ml < mg && cb) || g.home < 0 || sp > kEntrainFree)) || sp > kEntrainHeld) {
          g.vx = lr.x;
          g.vy = std::min(lr.y, follow);
          if (g.vx == 0 && g.vy == 0) g.vy = -1e-4f;
          g.fx = g.x + 0.5f;
          g.fy = g.y + 0.5f;
          Wake(g.x, g.y);
          continue;
        }
      }
    }
    // FREE FALL: air under it and around it, and it drops -- ballistic
    // (the flung branch), carrying its slide sideways, so a stream leaving a
    // lip spreads and accelerates as poured powder does. In liquid it
    // settles by the CA below instead.
    if (cfg_.grainFlow && GrainFree(g.x, g.y - 1)) {
      int cb, cs;
      LiquidMassAt(g.x, g.y - 1, &cb, nullptr);
      LiquidMassAt(g.x, g.y, &cs, nullptr);
      if (!cb && !cs) {
        // (Relative to its vessel, if it is in one: CarryGrains moves it with
        // the vessel as well.)
        const float jit = ((float)(Rand() & 1023) / 511.5f - 1.f) * cfg_.grainFallJitter;
        g.vx = (float)s0 * cfg_.grainSlideSpeed + jit;
        g.vy = -cfg_.grainFallStart;
        g.fx = g.x + 0.5f;
        g.fy = g.y + 0.5f;
        Wake(g.x, g.y);
        continue;
      }
    }
    // Lighter than the liquid ABOVE it: it rises into that pixel and the
    // liquid takes its place (MoveGrain shoves it down). Asking the pixel
    // above rather than its own is what lets a buried bed invert: liquid
    // never gets INTO a packed pile, so a grain that waited to find liquid
    // in its own pixel would stay under lava forever. The top grain goes
    // first, the liquid fills in behind it, and the next one sees it.
    {
      int cu;
      const float mUp = LiquidMassAt(g.x, g.y + 1, &cu, nullptr);
      if (cu && mUp > mg) {
        if (GrainFree(g.x, g.y + 1) && (Rand() & 3) == 0) MoveGrain(gi, g.x, g.y + 1);
        continue;
      }
    }
    // Sinking. In liquid it settles at a rate set by how much heavier the
    // grain is and how thick the liquid is.
    auto trySink = [&](int tx, int ty) -> bool {
      if (!GrainFree(tx, ty)) return false;
      int c;
      float ml = LiquidMassAt(tx, ty, &c, nullptr);
      if (c) {
        if (ml >= mg) {
          // FLOATING POWDER SPREADS. It floats on this liquid -- but a grain
          // coming DOWN A SLOPE (diagonally, from air) may settle into the
          // liquid's TOP pixel (air over it), as powder poured on water
          // spreads into a skin instead of standing as a 45-degree heap.
          // Never deeper than the top row: a grain already in the liquid, or
          // a pixel with liquid over it, is refused. Half the steps wait (a
          // skin creeping out, not a splash), keeping the tile awake.
          if (!cfg_.floatSpread || tx == g.x) return false;
          // How far under the liquid's top the target is: liquid pixels
          // over it in its column, up to air. The field is a smoothed splat,
          // so its top reaches a pixel or so over the particles; the top two
          // rows count as the surface. A grain over it is not open surface
          // (a skin grain slipping under its neighbour churned for ever).
          int depth = 0;
          for (int yy = ty + 1; yy < H && depth <= 2; yy++) {
            if (grid_[(size_t)yy * W + tx]) { depth = 99; break; }
            int cl;
            LiquidMassAt(tx, yy, &cl, nullptr);
            if (!cl) break;
            depth++;
          }
          if (depth > 1) return false;
          if (Rand() & 1) { Wake(g.x, g.y); return true; }
          MoveGrain(gi, tx, ty);
          return true;
        }
        const float p = WetSettle(g, ml, (size_t)ty * W + tx);
        if ((Rand() & 1023) > (uint32_t)(p * 1023)) return true;  // waits, but blocked for now
      }
      MoveGrain(gi, tx, ty);
      return true;
    };
    if (trySink(g.x, g.y - 1)) continue;
    // A sliding grain tries its own way first.
    int d = s0 ? (s0 > 0 ? 1 : -1) : ((Rand() & 1) ? 1 : -1);
    // A FLOATING PILE SLUMPS. A heap on water has no friction under it, so
    // its slope lies at 1:2, not the 1:1 of a heap on the ground: a grain in
    // a floating pile (its column reaches, through grains, a liquid heavier
    // than it) steps sideways into open air when the slope goes on down
    // two pixels out -- and the diagonal fall carries it from there. Air to
    // air, so it shoves no liquid; it stops once every slope is 1:2, so the
    // pile settles and sleeps.
    if (cfg_.floatSpread) {
      bool floating = false;
      for (int yy = g.y - 1, n = 0; yy >= 0 && n < 12; yy--, n++) {
        if (grid_[(size_t)yy * W + g.x]) continue;
        int cl;
        const float ml = LiquidMassAt(g.x, yy, &cl, nullptr);
        floating = cl && ml > mg;
        break;
      }
      if (floating) {
        int to = INT32_MIN;
        for (int t = 0; t < 2 && to == INT32_MIN; t++) {
          const int dd = t ? -d : d, tx = g.x + dd;
          if (!GrainFree(tx, g.y) || !GrainFree(tx + dd, g.y - 1)) continue;
          int ch, cf, ca;
          LiquidMassAt(tx, g.y, &ch, nullptr);
          if (ch) continue;   // the step is into air, never along under the surface
          // Two out and one down: air, or the liquid's top (air over it).
          LiquidMassAt(tx + dd, g.y - 1, &cf, nullptr);
          LiquidMassAt(tx + dd, g.y, &ca, nullptr);
          if (cf && ca) continue;
          to = tx;
        }
        if (to != INT32_MIN) {
          if ((Rand() & 3) == 0) { MoveGrain(gi, to, g.y); continue; }
          Wake(g.x, g.y);   // waiting, not settled: keep the tile awake
        }
      }
    }
    if (!GrainFree(g.x + d, g.y) && !GrainFree(g.x - d, g.y)) continue;
    if (GrainFree(g.x + d, g.y) && trySink(g.x + d, g.y - 1)) continue;
    if (GrainFree(g.x - d, g.y) && trySink(g.x - d, g.y - 1)) continue;
    // SLUMP AND SLIDE (air only; the floating pile has its own rule above).
    // No diagonal is open: step sideways where the slope goes on down one in
    // two -- always when already sliding that way, else with
    // grainSlumpChance -- or, with way on (2+), across the flat.
    if (cfg_.grainFlow && g.x == ox && g.y == oy) {
      for (int t = 0; t < 2; t++) {
        const int dd = t ? -d : d, tx = g.x + dd;
        if (!GrainFree(tx, g.y)) continue;
        int ch;
        const float mh = LiquidMassAt(tx, g.y, &ch, nullptr);
        // The slope goes on down into AIR: over liquid the floating rules
        // above own what a grain does (a slump onto the water walked a skin
        // grain about the surface for ever and it never slept).
        //
        // ...unless the grain SINKS in that liquid: a submerged heap has a
        // slope to keep as much as a dry one, and slumps along it through
        // the liquid (at the liquid's settling pace). Held to air only, the
        // dirt a pour left on the shoulder of an upturned flask, coated in
        // slime, lay there on a 34-degree slope for ever.
        const bool sunk = ch && mh < mg;
        if (ch && !sunk) continue;
        int cd;
        const float mdn = LiquidMassAt(tx + dd, g.y - 1, &cd, nullptr);
        const bool slope = GrainFree(tx + dd, g.y - 1) && (!cd || (sunk && mdn < mg));
        const bool way = s0 * dd > 0;
        const float chance = sunk ? cfg_.grainSlumpChance * std::clamp(WetSettle(g, mh, (size_t)g.y * W + tx) / 0.3f, 0.1f, 1.0f)
                                  : cfg_.grainSlumpChance;
        const bool go = slope ? ((way && !sunk) || (float)(Rand() & 1023) < chance * 1024.f)
                              : !sunk && s0 * dd >= 2;
        if (!go) {
          if (slope) Wake(g.x, g.y);   // may go next step: keep the tile awake
          continue;
        }
        MoveGrain(gi, tx, g.y);
        break;
      }
    }
    } while (false);
    // THE GRAIN'S WAY: a sideways or diagonal move builds it (to 3), a step
    // across the level or straight down spends one, standing
    // still loses it.
    if (cfg_.grainFlow && g.vx == 0 && g.vy == 0) {
      if (g.x != ox) {
        const int dir = g.x > ox ? 1 : -1;
        const int mag = s0 * dir > 0 ? std::abs(s0) : 0;
        g.slide = (int8_t)(dir * (g.y < oy ? std::min(3, mag + 1) : std::max(1, mag - 1)));
      } else if (g.y != oy) {
        g.slide = (int8_t)(s0 - (s0 > 0) + (s0 < 0));
      }
    }
  }

  if (!dead.empty()) {
    // Compact, fixing grid indices.
    std::vector<uint8_t> kill(grains_.size(), 0);
    for (int d : dead) kill[d] = 1;
    size_t w = 0;
    for (size_t i = 0; i < grains_.size(); i++) {
      if (kill[i]) continue;
      grains_[w] = grains_[i];
      grid_[(size_t)grains_[w].y * W + grains_[w].x] = (int)w + 1;
      w++;
    }
    grains_.resize(w);
  }
}

namespace {
float SegSegDist(V2 p1, V2 q1, V2 p2, V2 q2) {
  // Proper intersection is distance 0; otherwise the least endpoint-to-
  // segment distance.
  auto orient = [](V2 a, V2 b, V2 c) { return Cross(b - a, c - a); };
  float d1 = orient(p2, q2, p1), d2 = orient(p2, q2, q1), d3 = orient(p1, q1, p2), d4 = orient(p1, q1, q2);
  if (((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0))) return 0.f;
  float m = Len(p1 - ClosestOnSeg(p1, p2, q2, nullptr));
  m = std::min(m, Len(q1 - ClosestOnSeg(q1, p2, q2, nullptr)));
  m = std::min(m, Len(p2 - ClosestOnSeg(p2, p1, q1, nullptr)));
  m = std::min(m, Len(q2 - ClosestOnSeg(q2, p1, q1, nullptr)));
  return m;
}
}  // namespace

bool FlaskSim::PoseClear(const Vessel& v, const Xform& x) const {
  std::vector<V2> mine(v.outline.size());
  for (size_t i = 0; i < mine.size(); i++) mine[i] = ToWorld(x, v.outline[i]);
  for (const Vessel& o : vessels_) {
    if (&o == &v) continue;
    const float gap = v.shape.wall + o.shape.wall + 1.f;
    std::vector<V2> theirs(o.outline.size());
    for (size_t i = 0; i < theirs.size(); i++) theirs[i] = ToWorld(o.x, o.outline[i]);
    for (size_t a = 0; a + 1 < mine.size(); a++)
      for (size_t b = 0; b + 1 < theirs.size(); b++)
        if (SegSegDist(mine[a], mine[a + 1], theirs[b], theirs[b + 1]) < gap) return false;
  }
  return true;
}

bool FlaskSim::PoseClear(int vi, const Xform& x) const { return PoseClear(vessels_[vi], x); }

bool FlaskSim::ShapeClear(const VesselShape& shape, const Xform& x) const {
  Vessel tmp;
  tmp.shape = shape;
  BuildOutline(tmp, false);
  return PoseClear(tmp, x);
}

// ---- sleep ------------------------------------------------------------------

void FlaskSim::Partition() {
  // Awake particles first, IN CELL ORDER: particles that are neighbours in
  // the liquid are neighbours in memory, so the pair scan and the
  // relaxation walk the arrays instead of jumping about them (Step re-sorts
  // every 16 steps as the liquid mixes). The sleeping tail keeps its order.
  const int total = (int)px_.size();
  std::vector<int>& order = orderScratch_;
  order.clear();
  order.reserve(total);
  auto awake = [&](int i) {
    const int h = phome_[i];
    return h < 0 || h >= (int)vessels_.size() || !vessels_[h].asleep;
  };
  {
    const int nc = cellsW_ * cellsH_;
    std::vector<int> start(nc + 1, 0), key;
    key.reserve(total);
    std::vector<int> aw;
    aw.reserve(total);
    for (int i = 0; i < total; i++) {
      if (!awake(i)) continue;
      const int cx = std::clamp((int)(px_[i].x / cellSize_), 0, cellsW_ - 1);
      const int cy = std::clamp((int)(px_[i].y / cellSize_), 0, cellsH_ - 1);
      aw.push_back(i);
      key.push_back(cy * cellsW_ + cx);
      start[key.back() + 1]++;
    }
    for (int c = 0; c < nc; c++) start[c + 1] += start[c];
    order.resize(aw.size());
    for (size_t k = 0; k < aw.size(); k++) order[start[key[k]]++] = aw[k];
  }
  const int na = (int)order.size();
  for (int i = 0; i < total; i++) if (!awake(i)) order.push_back(i);
  auto perm = [&](auto& v) {
    auto c = v;
    for (int k = 0; k < total; k++) v[k] = c[order[k]];
  };
  perm(px_); perm(pv_); perm(pprev_); perm(psub_); perm(pw_); perm(mbar_); perm(calm_); perm(phome_); perm(psrc_);
  perm(pvar_); perm(panc_); perm(pheat_); perm(psol_); perm(pmass_);
  nAct_ = na;
}

void FlaskSim::UpdateSleep() {
  const int total = (int)px_.size();
  // Homes of the awake particles, every 8 steps (a sleeping one does not move).
  if ((step_ & 7) == 0)
    for (int i = 0; i < nAct_; i++) {
      phome_[i] = -1;
      for (size_t vi = 0; vi < vessels_.size(); vi++)
        if (!vessels_[vi].outline.empty() && InsideVessel(vessels_[vi], px_[i])) {
          phome_[i] = (int8_t)vi;
          psrc_[i] = (int8_t)vi;
          break;
        }
    }
  bool changed = false;
  for (size_t vi = 0; vi < vessels_.size(); vi++) {
    Vessel& v = vessels_[vi];
    if (v.outline.empty()) continue;
    // The vessel's box, grown by a kernel radius.
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (V2 o : v.outline) {
      const V2 w = ToWorld(v.x, o);
      x0 = std::min(x0, w.x); x1 = std::max(x1, w.x); y0 = std::min(y0, w.y); y1 = std::max(y1, w.y);
    }
    x0 -= h_; x1 += h_; y0 -= h_; y1 += h_;
    auto inBox = [&](V2 p) { return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1; };
    const bool moving = v.x.pos.x != v.target.pos.x || v.x.pos.y != v.target.pos.y ||
                        v.x.angle != v.target.angle || v.x.pos.x != v.prevX.pos.x ||
                        v.x.pos.y != v.prevX.pos.y || v.x.angle != v.prevX.angle;
    bool disturbed = moving || v.grainBusy;
    v.grainBusy = false;
    if (stickOn_ && (inBox(stickA_) || inBox(stickB_))) disturbed = true;
    if (!disturbed)
      for (int i = 0; i < nAct_ && !disturbed; i++)
        if (phome_[i] != (int)vi && inBox(px_[i]) && Dot(pv_[i], pv_[i]) > 0.09f) disturbed = true;
    if (!disturbed)
      for (const Grain& g : grains_)
        if ((g.vx != 0 || g.vy != 0) && inBox({g.fx, g.fy})) { disturbed = true; break; }
    if (v.asleep) {
      if (disturbed) {
        v.asleep = false;
        v.quiet = 0;
        changed = true;
      }
      continue;
    }
    if (disturbed) {
      v.quiet = 0;
      v.invBest = INT32_MAX;   // a fresh shake starts the count again
      v.sortStall = 0;
      continue;
    }
    // Awake and undisturbed: has its liquid STOPPED CHANGING THE PICTURE?
    // Judged on how far each particle has drifted over a window of
    // sleepSteps, not on its speed: the relaxation never quite solves a deep
    // column, and the residue is a sub-pixel oscillation in place (RMS speed
    // 0.03..0.06 px a step, but no drift) that a speed test either never
    // sleeps through or, set loose enough to, freezes a slosh mid-motion --
    // which is what the old 0.12 px/step test did.
    const bool grainsBusy = [&] {
      for (const Grain& g : grains_)
        if (g.home == (int)vi && (g.vx != 0 || g.vy != 0)) return true;
      return false;
    }();
    if (grainsBusy) {
      v.quiet = 0;
      continue;
    }
    // LIQUIDS OUT OF ORDER: a heavier one still over a lighter one. However
    // still the picture is, it is not settled while the sort drive is still
    // bringing it toward its layers -- so it stays awake while the count of
    // inverted pairs keeps falling (sortStallSteps without a new low = done).
    if (v.inverted < v.invBest) {
      v.invBest = v.inverted;
      v.sortStall = 0;
    } else {
      v.sortStall++;
    }
    if (v.inverted > cfg_.sortAwakePairs && v.sortStall < cfg_.sortStallSteps) {
      int cnt = 0;
      for (int i = 0; i < nAct_; i++) cnt += phome_[i] == (int)vi;
      if (v.inverted * 200 > cnt) {
        v.quiet = 0;
        continue;
      }
    }
    if (v.quiet++ == 0) {
      for (int i = 0; i < nAct_; i++)
        if (phome_[i] == (int)vi) panc_[i] = px_[i];
      continue;
    }
    if (v.quiet < cfg_.sleepSteps) continue;
    // Asleep when the RMS drift is under the line and at most 1% of the
    // particles are over three times it (a straggler or two at the surface
    // must not keep a whole flask awake; a visible stream would be many).
    const float lim = cfg_.sleepDrift;
    double sum = 0;
    int cnt = 0, far = 0;
    for (int i = 0; i < nAct_; i++)
      if (phome_[i] == (int)vi) {
        const V2 d = px_[i] - panc_[i];
        const float d2 = Dot(d, d);
        sum += d2;
        far += d2 > 9.0f * lim * lim;
        cnt++;
      }
    if (cnt == 0 || (sum / cnt < lim * lim && far * 100 <= cnt)) {
      v.asleep = true;
      changed = true;
      for (int i = 0; i < nAct_; i++)
        if (phome_[i] == (int)vi) { pv_[i] = {0, 0}; pprev_[i] = px_[i]; }
    } else {
      v.quiet = 0;
    }
  }
  (void)total;
  if (changed) Partition();
}

void FlaskSim::UpdateGrainHomes() {
  const int W = cfg_.gridW;
  const int nv = (int)vessels_.size();
  for (Grain& g : grains_) {
    if (wall_[(size_t)g.y * W + g.x]) continue;   // in glass: keep the last
    const int8_t nh = (int8_t)((int)inside_[(size_t)g.y * W + g.x] - 1);
    // A flung grain's velocity is in its home vessel's frame (CarryGrains
    // moves it with the vessel): changing home, it changes frame -- out of a
    // swung vessel it leaves with the vessel's velocity added.
    if (cfg_.grainFlow && nh != g.home && (g.vx != 0 || g.vy != 0)) {
      // (The frame's velocity: the motion of the interior's centre, which
      // is what carried it -- CarryGrains.)
      if (g.home >= 0 && g.home < nv) { g.vx += vessels_[g.home].frameVel.x; g.vy += vessels_[g.home].frameVel.y; }
      if (nh >= 0 && nh < nv) { g.vx -= vessels_[nh].frameVel.x; g.vy -= vessels_[nh].frameVel.y; }
      if (g.vx == 0 && g.vy == 0) g.vy = -cfg_.gravity;
    }
    g.home = nh;
    if (nh >= 0) g.src = nh;
  }
}

void FlaskSim::CarryGrains() {
  // Two parts. The vessel CARRIES its grains by the motion of its interior's
  // centre (below), and the glass SWEEPS the rest: a grain the new walls
  // landed on moves to the nearest free pixel on the side of that vessel it
  // was on, and everything else is left to gravity -- so sand in a tilting
  // flask slides and pours as it tips, instead of riding along as a block.
  // Step() caps a vessel's motion at a couple of pixels per substep (less
  // than the glass is thick), so the sweep never jumps a grain.
  const int W = cfg_.gridW;
  for (size_t vi = 0; vi < vessels_.size(); vi++) {
    const Vessel& v = vessels_[vi];
    if (v.x.pos.x == v.prevX.pos.x && v.x.pos.y == v.prevX.pos.y && v.x.angle == v.prevX.angle) continue;
    // THE VESSEL CARRIES ITS GRAINS by one move: the motion of its INTERIOR'S
    // CENTRE. Lifting a flask lifts the sand in it (left to the glass alone,
    // liquid under a sand bed rode up with the bottom and blew through the
    // bed); turning it about its own middle carries nothing, and the glass
    // sweep below does the turning -- so tipped sand slides and pours. Turning
    // it about a far point (a hand holding it by the lip) carries it by the
    // centre's swing. An earlier per-grain version lifted every grain on the
    // rising side while the vessel turned, supported or not, and stood the
    // sand up in the air.
    wakeAll_ = true;   // a moving vessel can unblock anything
    {
      const V2 cL{0.0f, v.shape.height * 0.4f};
      const V2 d = ToWorld(v.x, cL) - ToWorld(v.prevX, cL);
      // EVERY GRAIN OF THE VESSEL is carried, in flight or not: a grain
      // inside it lives in its frame (a flung grain's velocity is relative to
      // it; UpdateGrainHomes converts at the glass). Carried only when
      // resting on something, the grains in the air over a pile -- and with
      // grainFlow any grain with air under it is in the air -- were left
      // where they were while the pile moved, and blocked the rows over
      // them: a pouch carried down dropped out from under its own sand.
      // Without grainFlow a resting grain is carried only when SUPPORTED --
      // on a grain, the glass or liquid: carried in the air, a tipped
      // flask held a cloud of sand up.
      const size_t NF = fieldW_.size();
      auto supported = [&](const Grain& g) {
        if (cfg_.grainFlow || g.y == 0) return true;
        const size_t k = (size_t)(g.y - 1) * W + g.x;
        return grid_[k] != 0 || wall_[k] != 0 || (k < NF && fieldW_[k] > kLiquidThresh);
      };
      std::vector<int> mine;
      if (d.x != 0 || d.y != 0)
        for (size_t i = 0; i < grains_.size(); i++) {
          const Grain& g = grains_[i];
          const bool flung = g.vx != 0 || g.vy != 0;
          if (g.home == (int)vi && (flung ? cfg_.grainFlow : supported(g))) {
            Grain& gm = grains_[i];
            if (!flung && ((int)std::floor(gm.fx) != gm.x || (int)std::floor(gm.fy) != gm.y)) { gm.fx = gm.x + 0.5f; gm.fy = gm.y + 0.5f; }
            mine.push_back((int)i);
          }
        }
      // THE PILE MOVES AS ONE: every carried grain takes the same whole-pixel
      // shift, the vessel's sub-pixel remainder kept for its next move.
      // Rounded per grain (each from its own fx), neighbours took different
      // shifts -- one pixel against two -- and blocked each other, so a
      // packed pile barely moved with its vessel and the trailing glass had
      // to bulldoze it through its own band.
      // (The shift is the vessel's, Step: its liquid takes the same one.)
      const int ix = v.shiftX, iy = v.shiftY;
      // Leading edge first (a grain moves into space its neighbour left): a
      // counting sort on the integer projection onto the shift, descending,
      // O(n) where a comparison sort was most of a lift.
      std::vector<int> order;
      if (!mine.empty() && (ix || iy)) {
        order.resize(mine.size());
        const int span = (W + cfg_.gridH) * (std::abs(ix) + std::abs(iy)) + 1;
        const int NB = 2 * span;
        std::vector<int> cnt(NB + 1, 0), key(mine.size());
        for (size_t k = 0; k < mine.size(); k++) {
          const Grain& g = grains_[mine[k]];
          key[k] = std::clamp(span - 1 - (g.x * ix + g.y * iy), 0, NB - 1);
          cnt[key[k] + 1]++;
        }
        for (int bk = 0; bk < NB; bk++) cnt[bk + 1] += cnt[bk];
        for (size_t k = 0; k < mine.size(); k++) order[cnt[key[k]]++] = (int)k;
      }
      for (int k : order) {
        Grain& g = grains_[mine[k]];
        const int nx = g.x + ix, ny = g.y + iy;
        const bool flung = g.vx != 0 || g.vy != 0;
        const float fx = flung ? g.fx + ix : nx + 0.5f, fy = flung ? g.fy + iy : ny + 0.5f;
        if (!GrainFree(nx, ny)) continue;   // blocked: the sweep deals with it
        // Never carried OUT: the target must be inside its own vessel (glass
        // is refused by GrainFree). A grain leaves a vessel only through
        // its mouth, under the CA's own moves.
        if (inside_[(size_t)ny * W + nx] != (uint8_t)(vi + 1)) continue;
        grid_[(size_t)g.y * W + g.x] = 0;
        g.x = (int16_t)nx;
        g.y = (int16_t)ny;
        g.fx = fx;
        g.fy = fy;
        grid_[(size_t)ny * W + nx] = mine[k] + 1;
      }
    }
    deadEpoch_++;   // one sweep memo epoch per vessel pass
    for (size_t i = 0; i < grains_.size(); i++) {
      Grain& g = grains_[i];
      size_t k = (size_t)g.y * W + g.x;
      if (!wall_[k]) continue;
      const bool wasIn = g.home == (int)vi;
      // Breadth-first through pixels on the grain's side of the glass,
      // occupied or not, to the nearest FREE one; then every grain on the
      // path shifts one pixel along it. In a packed pile the glass pushes
      // the whole column rather than squeezing a grain out the other side.
      //
      // UNTIL IT IS OUT OF THE GLASS. A path through glass pixels that hold
      // grains of their own moves this grain only to the next of them --
      // still glass, and a success. Left there, the next step's glass moved
      // on past it and dropped it OUTSIDE: the sand that fell through the
      // bottom of a lifted pouch. A narrow search that cannot reach the
      // inside (a grain deep in the band) gets one wider one before the
      // grain is given up as wedged.
      for (int tries = 0; tries < 4 && wall_[(size_t)g.y * W + g.x]; tries++)
        if (!SweepChain((int)i, v, wasIn) && !SweepChain((int)i, v, wasIn, 6)) {
          wedged_.push_back((int)i);
          break;
        }
    }
  }
}

bool FlaskSim::SweepChain(int gi, const Vessel& v, bool wantIn, int glassReach) {
  const int W = cfg_.gridW, H = cfg_.gridH;
  const Grain& g0 = grains_[gi];
  const int start = g0.y * W + g0.x;
  const int g0x = g0.x, g0y = g0.y;
  const int vi = (int)(&v - vessels_.data());
  const uint32_t deadKey = deadEpoch_ * 2 + (wantIn ? 1u : 0u);
  // The memo records what the NARROW search could not reach; a wider one
  // may, so it neither reads nor writes it.
  const bool memo = glassReach <= 2;
  if (bfsDead_.size() != (size_t)W * H) bfsDead_.assign((size_t)W * H, 0);
  bfsParent_.resize((size_t)W * H);
  bfsSeen_.resize((size_t)W * H, 0);
  if (++bfsStamp_ == 0) { std::fill(bfsSeen_.begin(), bfsSeen_.end(), 0); bfsStamp_ = 1; }
  std::vector<int>& q = bfsQueue_;
  q.clear();
  q.push_back(start);
  bfsSeen_[start] = bfsStamp_;
  int found = -1;
  static const int kD[4][2] = {{0, 1}, {1, 0}, {-1, 0}, {0, -1}};
  for (size_t h = 0; h < q.size() && q.size() < 24576 && found < 0; h++) {
    int cur = q[h], cx = cur % W, cy = cur / W;
    for (auto& d : kD) {
      int x = cx + d[0], y = cy + d[1];
      if (x < 0 || y < 0 || x >= W || y >= H) continue;
      int k = y * W + x;
      if (bfsSeen_[k] == bfsStamp_) continue;
      // Through the glass is allowed (a grain the wall landed on is IN the
      // glass, often with nothing but glass around it); the far side of it
      // is not, and only a free pixel off the glass on the grain's own side
      // ends the search.
      const bool glass = wall_[k] != 0;
      // ...but only a few pixels of it: the band runs round the whole vessel,
      // and a search let loose along it spends its budget there instead of
      // reaching the inside. Two, not five: a path along the band is a grain
      // hopping sideways through the glass, and at five a turning flask
      // shuffled the grains against its wall 3-5 px a step (twice as often).
      // (glassReach: 2 unless the narrow search has already failed.)
      if (glass && std::abs(x - g0x) + std::abs(y - g0y) > glassReach) continue;
      if (!glass && (inside_[k] == (uint8_t)(vi + 1)) != wantIn) continue;
      if (!glass && memo && bfsDead_[k] == deadKey) continue;
      bfsSeen_[k] = bfsStamp_;
      bfsParent_[k] = cur;
      if (!glass && !grid_[k]) { found = k; break; }
      q.push_back(k);
    }
  }
  if (found < 0) {
    if (memo)
      for (int k : q)
        if (!wall_[k]) bfsDead_[k] = deadKey;
    return false;
  }
  // Walk back from the free pixel: each grain on the path moves into the
  // nearest free slot ahead of it. Empty glass pixels on the path are
  // stepped over (the next grain back jumps them).
  int to = found;
  while (to != start) {
    int from = bfsParent_[to];
    while (from != start && !grid_[from]) from = bfsParent_[from];
    int idx = grid_[from] - 1;
    Grain& g = grains_[idx];
    g.x = (int16_t)(to % W);
    g.y = (int16_t)(to / W);
    g.fx = g.x + 0.5f;
    g.fy = g.y + 0.5f;
    grid_[to] = idx + 1;
    grid_[from] = 0;
    if (inside_[to] && inside_[to] <= vessels_.size()) vessels_[inside_[to] - 1].grainBusy = true;
    Wake(to % W, to / W);
    Wake(from % W, from / W);
    to = from;
  }
  return true;
}

// ---- stepping --------------------------------------------------------------

void FlaskSim::MoveVessels() {
  // THE MOTION PROFILE. Each vessel chases its target with a velocity of its
  // own, changed by at most vesselAccel x gravity a step (vesselDropAccel
  // downward), and braking in time to stop on it: the time-optimal approach
  // under that limit. The liquid feels the vessel's acceleration as a tilt
  // of gravity, so this is what bounds how hard a flick can throw it. A
  // target that jumps (the cursor) no longer becomes a jerk of the glass.
  const float g = cfg_.gravity;
  const float aSide = g * cfg_.vesselAccel;
  const float aDown = g * cfg_.vesselDropAccel;
  const float aBrake = std::min(aSide, aDown) * 0.8f;
  const float vmax = cfg_.maxVesselStep;
  for (Vessel& v : vessels_) {
    v.prevX = v.x;
    if (v.outline.empty()) continue;
    const V2 e = v.target.pos - v.x.pos;
    const float el = Len(e);
    V2 want{0, 0};
    if (el > 1e-4f) {
      float sp = std::min(vmax, std::sqrt(2.0f * aBrake * el));
      sp = std::min(sp, el);
      want = e * (sp / el);
    }
    V2 dv = want - v.vel;
    dv.x = std::clamp(dv.x, -aSide, aSide);
    dv.y = std::clamp(dv.y, -aDown, aSide);
    v.vel = v.vel + dv;
    // Turning: the same limit at the vessel's far edge.
    const float alpha = aSide * 1.5f / v.reach, wmax = vmax / v.reach;
    const float ea = v.target.angle - v.x.angle;
    float wantW = 0;
    if (std::fabs(ea) > 1e-6f) {
      wantW = std::min({wmax, std::sqrt(2.0f * alpha * 0.8f * std::fabs(ea)), std::fabs(ea)});
      if (ea < 0) wantW = -wantW;
    }
    v.angVel += std::clamp(wantW - v.angVel, -alpha, alpha);
    // Capped: no outline point moves more than maxVesselStep px, so the
    // glass sweeps grains and liquid instead of jumping them.
    const float move = Len(v.vel) + std::fabs(v.angVel) * v.reach;
    if (move > vmax) {
      v.vel = v.vel * (vmax / move);
      v.angVel *= vmax / move;
    }
    Xform next = v.x;
    if (el < 0.02f && std::fabs(ea) < 1e-4f && Len(v.vel) < 0.02f && std::fabs(v.angVel) * v.reach < 0.02f) {
      next = v.target;   // arrived: exactly on it, at rest
      v.vel = {0, 0};
      v.angVel = 0;
    } else {
      next.pos = next.pos + v.vel;
      next.angle += v.angVel;
    }
    if (next.pos.x == v.x.pos.x && next.pos.y == v.x.pos.y && next.angle == v.x.angle) continue;
    // Glass never passes through glass. A move that would bring this
    // vessel's walls into another's SLIDES instead of freezing: the
    // rotation alone, the translation alone, then half of each -- so a
    // flask swung toward another's neck rides round it the way a hand
    // would steer, rather than stopping dead at the first touch. What is
    // refused is lost from the velocity (it hit something).
    if (!PoseClear(v, next)) {
      Xform rotOnly = v.x, movOnly = v.x, half = v.x;
      rotOnly.angle = next.angle;
      movOnly.pos = next.pos;
      half.pos = (v.x.pos + next.pos) * 0.5f;
      half.angle = (v.x.angle + next.angle) * 0.5f;
      if (PoseClear(v, rotOnly)) { next = rotOnly; v.vel = {0, 0}; }
      else if (PoseClear(v, movOnly)) { next = movOnly; v.angVel = 0; }
      else if (PoseClear(v, half)) { next = half; v.vel = v.vel * 0.5f; v.angVel *= 0.5f; }
      else { v.vel = {0, 0}; v.angVel = 0; continue; }
    }
    v.x = next;
  }
}

double FlaskSim::NowMs() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void FlaskSim::Step(int substeps) {
  for (int s = 0; s < substeps; s++) {
    double t0 = NowMs();
    MoveVessels();
    // Each vessel's CONTENTS FRAME: the motion of its interior's centre this
    // step (the carry of grains and liquid alike) and its change.
    for (Vessel& v : vessels_) {
      V2 fv{0, 0};
      if (!v.outline.empty()) {
        const V2 cL{0.0f, v.shape.height * 0.4f};
        fv = ToWorld(v.x, cL) - ToWorld(v.prevX, cL);
      }
      v.frameAcc = fv - v.frameVel;
      v.frameVel = fv;
      v.carryRem = v.carryRem + fv;
      v.shiftX = (int)std::lround(v.carryRem.x);
      v.shiftY = (int)std::lround(v.carryRem.y);
      v.carryRem.x -= (float)v.shiftX;
      v.carryRem.y -= (float)v.shiftY;
    }
    bool anyMoved = false;
    for (const Vessel& v : vessels_)
      anyMoved |= v.x.pos.x != v.prevX.pos.x || v.x.pos.y != v.prevX.pos.y || v.x.angle != v.prevX.angle;
    if (anyMoved) {
      RebuildWalls();
      wedged_.clear();
      CarryGrains();
      CarryGas();
    }

    // The stick flings the grains it passes through.
    if (stickOn_) {
      const int W = cfg_.gridW;
      for (size_t i = 0; i < grains_.size(); i++) {
        Grain& g = grains_[i];
        V2 c{g.x + 0.5f, g.y + 0.5f};
        float t;
        V2 q = ClosestOnSeg(c, stickA_, stickB_, &t);
        if (Len(c - q) > stickR_ + 0.7f) continue;
        V2 va = stickA_ - stickPrevA_, vb = stickB_ - stickPrevB_;
        V2 sv = va * (1 - t) + vb * t;
        float jit = ((Rand() & 255) / 255.f - 0.5f) * 0.6f;
        g.vx = sv.x * 0.9f + jit;
        g.vy = sv.y * 0.9f + 0.3f;
        if (!awake_.empty()) Wake(g.x, g.y);
        g.fx = c.x;
        g.fy = c.y;
        (void)W;
      }
    }

    UpdateSleep();
    if ((step_ & 15) == 0 && nAct_ > 64) Partition();
    double t1 = NowMs();
    prof_.move += t1 - t0;
    grainMoves_ = 0;
    StepLiquid();
    t0 = NowMs();
    prof_.liquid += t0 - t1;
    StepGrains();
    t1 = NowMs();
    prof_.grains += t1 - t0;
    // THE CHEMISTRY and THE GAS (flaskchem.cpp): after the matter has moved,
    // so a rule reads this step's neighbours.
    const int fired0 = firedTotal_;
    if (cfg_.gasEvery > 0 && (step_ % (uint32_t)cfg_.gasEvery) == 0) StepGas();
    if (cfg_.chemEvery > 0 && (step_ % (uint32_t)cfg_.chemEvery) == 0) StepChemistry();
    prof_.steps++;
    prof_.peakGasPx = std::max(prof_.peakGasPx, (int)gasList_.size());
    prof_.peakParticles = std::max(prof_.peakParticles, (int)px_.size());
    prof_.peakGrains = std::max(prof_.peakGrains, (int)grains_.size());
    active_ = anyMoved || stickOn_ || nAct_ > 0 || grainMoves_ > 0 || !gasList_.empty() ||
              firedTotal_ != fired0 || anyDevice_;
    for (const Grain& g : grains_)
      if (g.vx != 0 || g.vy != 0) { active_ = true; break; }
    if (!grains_.empty()) UpdateGrainHomes();
    stickPrevA_ = stickA_;
    stickPrevB_ = stickB_;
    step_++;
  }
}

int FlaskSim::MovingCount(float speed) const {
  int c = 0;
  for (const V2& v : pv_) c += Len(v) > speed;
  for (const Grain& g : grains_) c += (g.vx != 0 || g.vy != 0);
  return c;
}

void FlaskSim::Settle(int maxSteps) {
  bool on = stickOn_;
  stickOn_ = false;
  // Done when nothing is IN FLIGHT: no liquid outside every vessel still
  // moving, no grain flung. Liquid sloshing inside a vessel stays in it; a
  // test on every particle's speed waited out the relaxation's sub-pixel
  // shimmer, and Finish runs this on the frame thread.
  for (int s = 0; s < maxSteps; s++) {
    Step(1);
    if ((s & 7) != 7) continue;
    bool flying = false;
    for (int i = 0; i < nAct_ && !flying; i++)
      flying = phome_[i] < 0 && Dot(pv_[i], pv_[i]) > 0.0025f;
    for (size_t g = 0; g < grains_.size() && !flying; g++)
      flying = grains_[g].vx != 0 || grains_[g].vy != 0;
    if (!flying) break;
  }
  stickOn_ = on;
}

int FlaskSim::HitVessel(V2 p, float slack) const {
  for (int vi = (int)vessels_.size() - 1; vi >= 0; vi--) {
    const Vessel& v = vessels_[vi];
    if (v.outline.empty()) continue;
    const V2 l = ToLocal(v.x, p);
    if (InsideLocal(v, l)) return vi;
    for (size_t i = 0; i + 1 < v.outline.size(); i++)
      if (Len(l - ClosestOnSeg(l, v.outline[i], v.outline[i + 1], nullptr)) < slack) return vi;
  }
  return -1;
}

// ---- removing a vessel -------------------------------------------------------

std::vector<V2> FlaskSim::VesselOutline(int vi) const {
  std::vector<V2> out;
  if (!VesselAlive(vi)) return out;
  const Vessel& v = vessels_[vi];
  for (V2 p : v.outline) out.push_back(ToWorld(v.x, p));
  return out;
}

Composition FlaskSim::RemoveVessel(int vi) {
  Composition out;
  if (!VesselAlive(vi)) return out;
  Vessel& v = vessels_[vi];
  const size_t S = subs_.size();
  std::vector<uint64_t> units(S, 0);
  // Liquid inside it leaves with it.
  size_t w = 0;
  int act = 0;
  std::vector<uint64_t> dissolved(S, 0);   // by the powder's slot
  for (size_t i = 0; i < px_.size(); i++) {
    if (InsideVessel(v, px_[i])) {
      units[psub_[i]] += pw_[i];
      if (pmass_[i]) {
        const ChemSolute* sp = chem_.Species(psol_[i]);
        if (sp && sp->from >= 0) dissolved[sp->from] += pmass_[i];
      }
      continue;
    }
    if ((int)i < nAct_) act++;
    px_[w] = px_[i]; pv_[w] = pv_[i]; pprev_[w] = pprev_[i];
    psub_[w] = psub_[i]; pw_[w] = pw_[i]; mbar_[w] = mbar_[i]; calm_[w] = calm_[i];
    phome_[w] = phome_[i]; psrc_[w] = psrc_[i]; pvar_[w] = pvar_[i]; panc_[w] = panc_[i]; pheat_[w] = pheat_[i];
    psol_[w] = psol_[i]; pmass_[w] = pmass_[i];
    w++;
  }
  nAct_ = act;
  px_.resize(w); pv_.resize(w); pprev_.resize(w); psub_.resize(w); pw_.resize(w); mbar_.resize(w);
  calm_.resize(w); phome_.resize(w); psrc_.resize(w); pvar_.resize(w); panc_.resize(w); pheat_.resize(w);
  psol_.resize(w); pmass_.resize(w);
  // Its gas, in GAS units: the grid inside it, its gas pools and its bank.
  // A STOPPERED vessel takes it with it (whole units of matter; the part of
  // a unit left over stays on the table, banked outside, so the count is
  // exact); an open one lets it go (into the spill, which the bench streams
  // out at the lip).
  std::vector<uint64_t> gasF(S, 0);
  for (int k : gasList_) {
    if (!gasAmt_[k] || inside_[k] != (uint8_t)(vi + 1)) continue;
    gasF[gasSub_[k]] += gasAmt_[k];
    if (!v.stoppered) NoteExit((float)(k % cfg_.gridW) + 0.5f, gasAmt_[k]);
    gasAmt_[k] = 0;
    gasSub_[k] = 0xFF;
  }
  // ...and whatever was waiting in it to become a particle, a grain or gas.
  for (Pool& p : pools_)
    if (p.vessel == vi && p.units) {
      if (subs_[p.sub].gas) gasF[p.sub] += p.units;
      else units[p.sub] += p.units;
      p.units = 0;
    }
  if (gasBank_.size() >= ((size_t)vi + 2) * S)
    for (size_t s = 0; s < S; s++) {
      gasF[s] += gasBank_[((size_t)vi + 1) * S + s];
      gasBank_[((size_t)vi + 1) * S + s] = 0;
    }
  for (size_t s = 0; s < S; s++) {
    if (!gasF[s]) continue;
    if (v.stoppered) {
      const uint64_t E = (uint64_t)GasE();
      units[s] += gasF[s] / E;
      gasBank_[s] += (uint32_t)(gasF[s] % E);
    } else {
      const uint32_t u = BankGas(0, (int)s, (uint32_t)gasF[s]);
      spilledUnits_[s] += u;
      NoteFrom(vi, (int)s, u);
    }
  }
  // ...and so do the grains.
  const int W = cfg_.gridW;
  size_t gw = 0;
  for (size_t i = 0; i < grains_.size(); i++) {
    const Grain& g = grains_[i];
    const size_t k = (size_t)g.y * W + g.x;
    grid_[k] = 0;
    const int owner = wall_[k] ? g.home : (int)inside_[k] - 1;
    if (owner == vi) {
      units[g.sub] += 1;
      continue;
    }
    grains_[gw++] = g;
  }
  wakeAll_ = true;
  grains_.resize(gw);
  for (size_t i = 0; i < grains_.size(); i++)
    grid_[(size_t)grains_[i].y * W + grains_[i].x] = (int)i + 1;
  // Units to eighths. A vessel's units are whole eighths plus the fraction
  // of an eighth the grains in it make; the fraction is carried by the
  // spill so the sum over the session stays exact (Count's rule).
  const uint64_t upe = (uint64_t)cfg_.unitsPerEighth;
  for (size_t s = 0; s < S; s++) {
    if (!units[s]) continue;
    const uint64_t e = units[s] / upe, r = units[s] % upe;
    if (e && out.Add(subs_[s].mat, (uint32_t)e)) removed_[s] += (int64_t)(e * upe);
    else { spilledUnits_[s] += (uint32_t)(e * upe); NoteFrom(vi, (int)s, (uint32_t)(e * upe)); }
    spilledUnits_[s] += (uint32_t)r;
    NoteFrom(vi, (int)s, (uint32_t)r);
  }
  // Dissolved: the same, as a DISSOLVED portion of the powder (contract 2.4).
  for (size_t s = 0; s < S; s++) {
    if (!dissolved[s]) continue;
    const uint64_t e = dissolved[s] / upe, r = dissolved[s] % upe;
    if (e && out.Add((uint16_t)(subs_[s].mat | kDissolvedBit), (uint32_t)e)) removed_[s] += (int64_t)(e * upe);
    else { spilledUnits_[s] += (uint32_t)(e * upe); NoteFrom(vi, (int)s, (uint32_t)(e * upe)); }
    spilledUnits_[s] += (uint32_t)r;
    NoteFrom(vi, (int)s, (uint32_t)r);
  }
  v.outline.clear();
  v.burner = false;
  v.shock = 0;
  RebuildWalls();
  return out;
}

// ---- counting --------------------------------------------------------------

Tally FlaskSim::Count() const {
  const size_t S = subs_.size();
  const size_t B = vessels_.size() + 1;  // last bin = spilled
  std::vector<uint64_t> units(B * S, 0);
  auto binOf = [&](V2 w) -> size_t {
    for (size_t v = 0; v < vessels_.size(); v++)
      if (InsideVessel(vessels_[v], w)) return v;
    return B - 1;
  };
  // A second form per slot: DISSOLVED in a particle (contract 2.4). Rounded
  // together with the undissolved form, so a material's eighths out still
  // equal its eighths in whichever form it ended in.
  std::vector<uint64_t> dis(B * S, 0);
  for (size_t i = 0; i < px_.size(); i++) {
    const size_t b = binOf(px_[i]);
    units[b * S + psub_[i]] += pw_[i];
    if (pmass_[i]) {
      const ChemSolute* sp = chem_.Species(psol_[i]);
      if (sp && sp->from >= 0) dis[b * S + (size_t)sp->from] += pmass_[i];
    }
  }
  // Grains by the inside mask (the same answer UpdateGrainHomes and the
  // sweep use); one in the glass band belongs to its home vessel.
  const int GW = cfg_.gridW;
  for (const Grain& g : grains_) {
    const size_t k = (size_t)g.y * GW + g.x;
    int vi = wall_[k] ? g.home : (int)inside_[k] - 1;
    if (vi < 0 || vi >= (int)vessels_.size() || vessels_[vi].outline.empty()) vi = (int)B - 1;
    units[(size_t)vi * S + g.sub] += 1;
  }
  // Everything above is in matter units; from here the count is in GAS
  // units (SimConfig::gasExpand to one), so the gas -- the grid, gas pools,
  // and the bank's parts of a unit -- counts exactly where it is.
  const uint64_t E = (uint64_t)GasE();
  for (uint64_t& u : units) u *= E;
  for (uint64_t& u : dis) u *= E;
  // Gas by the inside mask; a cloud over the table is not in any vessel.
  for (int k : gasList_) {
    if (!gasAmt_[k]) continue;
    int vi = (int)inside_[k] - 1;
    if (vi < 0 || vi >= (int)vessels_.size() || vessels_[vi].outline.empty()) vi = (int)B - 1;
    units[(size_t)vi * S + gasSub_[k]] += gasAmt_[k];
  }
  for (const Pool& p : pools_) {
    if (!p.units) continue;
    const int vi = p.vessel >= 0 && p.vessel < (int)vessels_.size() && !vessels_[p.vessel].outline.empty()
                       ? p.vessel : (int)B - 1;
    units[(size_t)vi * S + p.sub] += subs_[p.sub].gas ? p.units : p.units * E;
  }
  for (size_t b = 0; b < gasBank_.size(); b++) {
    if (!gasBank_[b]) continue;
    const int v = (int)(b / S) - 1;   // bank bin 0 = outside
    const size_t bin = v >= 0 && v < (int)vessels_.size() && !vessels_[v].outline.empty() ? (size_t)v : B - 1;
    units[bin * S + b % S] += gasBank_[b];
  }
  for (size_t s = 0; s < S; s++) units[(B - 1) * S + s] += (uint64_t)spilledUnits_[s] * E;

  // Units -> eighths, per substance, by largest remainder: the bins' eighths
  // sum to exactly the substance's total, which is a whole number of eighths
  // because it went in as one.
  Tally t;
  t.vessel.resize(vessels_.size());
  const uint64_t upe = (uint64_t)cfg_.unitsPerEighth * E;
  for (size_t s = 0; s < S; s++) {
    uint64_t total = 0;
    for (size_t b = 0; b < B; b++) total += units[b * S + s] + dis[b * S + s];
    t.totalUnits += (uint32_t)(total / E);
    if (!total) continue;
    uint64_t eighths = (total + upe / 2) / upe;
    // Bins x forms: [0, B) undissolved, [B, 2B) dissolved.
    std::vector<uint64_t> e(2 * B);
    std::vector<std::pair<uint64_t, size_t>> rem;
    uint64_t given = 0;
    for (size_t b = 0; b < 2 * B; b++) {
      const uint64_t u = b < B ? units[b * S + s] : dis[(b - B) * S + s];
      e[b] = u / upe;
      given += e[b];
      rem.push_back({u % upe, b});
    }
    std::stable_sort(rem.begin(), rem.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (size_t r = 0; given < eighths && r < rem.size(); r++, given++) e[rem[r].second]++;
    for (size_t b = 0; b < 2 * B; b++) {
      if (!e[b]) continue;
      const size_t bin = b < B ? b : b - B;
      const uint16_t mat = (uint16_t)(b < B ? subs_[s].mat : (subs_[s].mat | kDissolvedBit));
      Composition& c = bin + 1 == B ? t.spilled : t.vessel[bin];
      // A vessel that would hold a 17th substance cannot: that portion is
      // counted as spilled (it would not fit through the lip either).
      if (!c.Add(mat, (uint32_t)e[b]) && &c != &t.spilled) t.spilled.Add(mat, (uint32_t)e[b]);
    }
  }
  return t;
}

Composition FlaskSim::DrainSpilled(float* exitX, SpillBy* by) {
  // Whole eighths only: the fraction stays in spilledUnits_, so Count() still
  // sees every unit and its per-substance totals stay whole eighths.
  Composition out;
  const uint32_t upe = (uint32_t)cfg_.unitsPerEighth;
  if (by) {
    by->vessel.assign(vessels_.size(), Composition{});
    by->unknown = Composition{};
  }
  auto ledger = [&](size_t v, size_t s) -> uint32_t* {
    if (v >= exitBy_.size() || s >= exitBy_[v].size()) return nullptr;
    return &exitBy_[v][s];
  };
  for (size_t s = 0; s < spilledUnits_.size(); s++) {
    const uint32_t e = spilledUnits_[s] / upe;
    if (e && out.Add(subs_[s].mat, e)) {   // a 17th substance waits a step
      spilledUnits_[s] -= e * upe;
      drained_[s] += (int64_t)e * upe;
      if (by) {
        // WHO SPILLED IT: whole eighths to each vessel whose ledger holds
        // them, then one eighth at a time to the largest remainder (lowest
        // vessel on a tie), the rest unknown -- a partition of the e eighths.
        uint32_t left = e;
        auto give = [&](size_t v, uint32_t n) {
          if (v >= by->vessel.size() || !by->vessel[v].Add(subs_[s].mat, n))
            by->unknown.Add(subs_[s].mat, n);
        };
        for (size_t v = 0; v < exitBy_.size() && left; v++) {
          uint32_t* l = ledger(v, s);
          if (!l) continue;
          const uint32_t take = std::min(left, *l / upe);
          if (!take) continue;
          give(v, take);
          *l -= take * upe;
          left -= take;
        }
        while (left) {
          size_t best = exitBy_.size();
          uint32_t bestU = 0;
          for (size_t v = 0; v < exitBy_.size(); v++) {
            const uint32_t* l = ledger(v, s);
            if (l && *l > bestU) { bestU = *l; best = v; }
          }
          if (best == exitBy_.size()) break;
          give(best, 1);
          exitBy_[best][s] -= std::min(bestU, upe);
          left--;
        }
        if (left) by->unknown.Add(subs_[s].mat, left);
      }
    }
    // The ledger never claims more than is still pending (gas notes, and
    // eighths drained with no `by`, can leave it ahead of spilledUnits_).
    uint32_t allow = spilledUnits_[s];
    for (size_t v = 0; v < exitBy_.size(); v++)
      if (uint32_t* l = ledger(v, s)) {
        *l = std::min(*l, allow);
        allow -= *l;
      }
  }
  if (exitX) *exitX = exitW_ > 0 ? (float)(exitSum_ / exitW_) : -1.0f;
  if (!out.Empty()) exitSum_ = exitW_ = 0;
  return out;
}

std::vector<float> FlaskSim::MeanHeights() const {
  std::vector<double> sum(subs_.size(), 0), cnt(subs_.size(), 0);
  for (size_t i = 0; i < px_.size(); i++) { sum[psub_[i]] += px_[i].y; cnt[psub_[i]] += 1; }
  for (const Grain& g : grains_) { sum[g.sub] += g.y + 0.5; cnt[g.sub] += 1; }
  std::vector<float> out(subs_.size(), -1.f);
  for (size_t s = 0; s < subs_.size(); s++)
    if (cnt[s] > 0) out[s] = (float)(sum[s] / cnt[s]);
  return out;
}

// ---- drawing ---------------------------------------------------------------

void FlaskSim::Render(std::vector<uint32_t>& out) const {
  struct Timed {
    Profile& p;
    double t0;
    ~Timed() { p.render += NowMs() - t0; p.renders++; }
  } timed{prof_, NowMs()};
  // THE LOOK. Everything here is derived from what the world's renderer
  // reads for the same material (Look, from SetSubstances): the opaque flag
  // (a molten surface that glows and churns), isViscousLiquid's pair (a dark
  // glossy sheen, no glints), media opacity (how much of the desk shows
  // through and how fast depth darkens it), and emission (the light it
  // throws on the glass, the sand and the air round it). The texture is
  // carried BY THE PARTICLES -- each has its own palette entry and phase --
  // so it flows with the liquid; a pattern in screen space read as a static
  // backdrop the liquid was a window onto.
  const int W = cfg_.gridW, H = cfg_.gridH;
  const size_t N = (size_t)W * H;
  out.assign(N, 0);
  const float t = (float)step_;
  const uint32_t elapsed = std::min<uint32_t>(16, step_ - lastRenderStep_);
  lastRenderStep_ = step_;

  // 1. Splat: total footprint per pixel, and the particle that covers it
  // most (its OWNER: the pixel wears that particle's look).
  rTotal_.assign(N, 0.f);
  rBest_.assign(N, 0.f);
  rOwner_.assign(N, -1);
  const float R = spacing_ * 1.05f;
  const float R2 = R * R;
  const int ri = (int)std::ceil(R);
  std::vector<int> perSub(subs_.size(), 0);
  for (size_t i = 0; i < px_.size(); i++) {
    const int cx = (int)std::floor(px_[i].x), cy = (int)std::floor(px_[i].y);
    if (cx < -ri || cy < -ri || cx >= W + ri || cy >= H + ri) continue;
    perSub[psub_[i]]++;
    for (int dy = -ri; dy <= ri; dy++) {
      const int y = cy + dy;
      if (y < 0 || y >= H) continue;
      for (int dx = -ri; dx <= ri; dx++) {
        const int x = cx + dx;
        if (x < 0 || x >= W) continue;
        const float ex = x + 0.5f - px_[i].x, ey = y + 0.5f - px_[i].y;
        const float d2 = ex * ex + ey * ey;
        if (d2 >= R2) continue;
        float w = 1.f - d2 / R2;
        w *= w;
        const size_t k = (size_t)y * W + x;
        rTotal_[k] += w;
        if (w > rBest_[k]) { rBest_[k] = w; rOwner_[k] = (int)i; }
      }
    }
  }
  const float thresh = kLiquidThresh;
  auto liquid = [&](size_t k) { return rTotal_[k] > thresh && !grid_[k]; };

  // 2. Depth below the liquid's surface, per column (1 = the surface pixel).
  rDepth_.assign(N, 0);
  for (int x = 0; x < W; x++) {
    int d = 0;
    for (int y = H - 1; y >= 0; y--) {
      const size_t k = (size_t)y * W + x;
      d = liquid(k) ? std::min(d + 1, 255) : 0;
      rDepth_[k] = (uint8_t)d;
    }
  }

  // Molten matter's heat, per particle: bands travelling through the blobs
  // its seed noise made (neighbours agree, so it reads as flow, not grain).
  rHeat_.resize(px_.size());
  for (size_t i = 0; i < px_.size(); i++)
    rHeat_[i] = look_[psub_[i]].kind == kLookMolten
                    ? 0.5f + 0.5f * std::sin(pheat_[i] * (12.566f / 255.0f) + t * 0.03f)
                    : 0.0f;
  auto heatOf = [&](int i) { return rHeat_[i]; };

  // 3. The glow field, at quarter resolution, blurred: what emissive liquid
  // throws on everything round it.
  const int gw = (W + 3) / 4, gh = (H + 3) / 4;
  bool anyGlow = false;
  for (const Look& L : look_) anyGlow |= L.glow > 0.05f;
  rGlow_.assign(anyGlow ? (size_t)gw * gh * 3 : 0, 0.f);
  if (anyGlow) {
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        const size_t k = (size_t)y * W + x;
        if (!liquid(k)) continue;
        const int o = rOwner_[k];
        const Look& L = look_[psub_[o]];
        if (L.glow <= 0.05f) continue;
        const float pulse = L.kind == kLookMolten ? 0.6f + 0.4f * heatOf(o)
                                                  : 0.75f + 0.25f * std::sin(t * 0.02f + pvar_[o] * 0.01f);
        const float s = L.glow * pulse / 16.0f;
        float* g = &rGlow_[((size_t)(y >> 2) * gw + (x >> 2)) * 3];
        const uint32_t c = L.col[2];
        g[0] += ChR(c) * s; g[1] += ChG(c) * s; g[2] += ChB(c) * s;
      }
    std::vector<float> tmp(rGlow_.size());
    for (int pass = 0; pass < 2; pass++) {
      for (int y = 0; y < gh; y++)
        for (int x = 0; x < gw; x++)
          for (int c = 0; c < 3; c++) {
            float a = 0;
            for (int o = -2; o <= 2; o++) a += rGlow_[((size_t)y * gw + std::clamp(x + o, 0, gw - 1)) * 3 + c];
            tmp[((size_t)y * gw + x) * 3 + c] = a * 0.2f;
          }
      for (int y = 0; y < gh; y++)
        for (int x = 0; x < gw; x++)
          for (int c = 0; c < 3; c++) {
            float a = 0;
            for (int o = -2; o <= 2; o++) a += tmp[((size_t)std::clamp(y + o, 0, gh - 1) * gw + x) * 3 + c];
            rGlow_[((size_t)y * gw + x) * 3 + c] = a * 0.2f;
          }
    }
  }
  // Which quarter cells have any glow worth sampling (with a cell's margin
  // for the bilinear reach): most of the table does not.
  rGlowOn_.assign(anyGlow ? (size_t)gw * gh : 0, 0);
  if (anyGlow)
    for (int y = 0; y < gh; y++)
      for (int x = 0; x < gw; x++) {
        const float* g = &rGlow_[((size_t)y * gw + x) * 3];
        if (g[0] + g[1] + g[2] < 0.3f) continue;
        for (int oy = -1; oy <= 1; oy++)
          for (int ox = -1; ox <= 1; ox++) {
            const int ax = x + ox, ay = y + oy;
            if (ax >= 0 && ay >= 0 && ax < gw && ay < gh) rGlowOn_[(size_t)ay * gw + ax] = 1;
          }
      }
  auto glowAt = [&](int x, int y, float* o) {
    // Bilinear over the quarter-resolution field.
    const float fx = (x + 0.5f) / 4.0f - 0.5f, fy = (y + 0.5f) / 4.0f - 0.5f;
    const int x0 = std::clamp((int)std::floor(fx), 0, gw - 1), y0 = std::clamp((int)std::floor(fy), 0, gh - 1);
    const int x1 = std::min(x0 + 1, gw - 1), y1 = std::min(y0 + 1, gh - 1);
    const float ax = std::clamp(fx - x0, 0.f, 1.f), ay = std::clamp(fy - y0, 0.f, 1.f);
    for (int c = 0; c < 3; c++) {
      const float a = rGlow_[((size_t)y0 * gw + x0) * 3 + c], b = rGlow_[((size_t)y0 * gw + x1) * 3 + c];
      const float d = rGlow_[((size_t)y1 * gw + x0) * 3 + c], e = rGlow_[((size_t)y1 * gw + x1) * 3 + c];
      o[c] = (a + (b - a) * ax) * (1 - ay) + (d + (e - d) * ax) * ay;
    }
  };

  // 4. Bubbles (look only): acid fizzes, molten matter blurps. Each rises
  // through its own liquid and bursts where that liquid ends.
  {
    uint32_t& r = lookRng_;
    auto rnd = [&r] { r ^= r << 13; r ^= r >> 17; r ^= r << 5; return r; };
    size_t w = 0;
    for (size_t b = 0; b < bubbles_.size(); b++) {
      Bubble bb = bubbles_[b];
      if (bb.age >= 200) continue;   // burst last frame
      const bool held = bb.vessel >= 0 && VesselAlive(bb.vessel);
      if (held) {
        const V2 w = ToWorld(vessels_[bb.vessel].x, {bb.lx, bb.ly});
        bb.x = w.x;
        bb.y = w.y;
      }
      bb.y += bb.vy * elapsed;
      bb.x += ((int)(rnd() % 3) - 1) * 0.35f;
      if (held) {
        const V2 l = ToLocal(vessels_[bb.vessel].x, {bb.x, bb.y});
        bb.lx = l.x;
        bb.ly = l.y;
      }
      const int x = (int)bb.x, y = (int)bb.y;
      bool alive = x >= 0 && y >= 0 && x < W && y < H;
      if (alive) {
        const size_t k = (size_t)y * W + x;
        alive = liquid(k) && psub_[rOwner_[k]] == bb.sub;
      }
      if (!alive) {
        if (look_[bb.sub].kind == kLookMolten) { bb.age = 200; bb.y -= 1; bubbles_[w++] = bb; }
        continue;
      }
      bb.age = (uint8_t)std::min(199, bb.age + 1);
      bubbles_[w++] = bb;
    }
    bubbles_.resize(w);
    for (size_t s = 0; s < subs_.size() && bubbles_.size() < 240; s++) {
      if (!perSub[s] || subs_[s].powder) continue;
      const Look& L = look_[s];
      float rate = 0;
      if (L.kind == kLookMolten) rate = 0.00012f;
      else if (L.glow > 0.05f) rate = 0.0012f * std::min(1.0f, L.glow * 4.0f);
      if (rate <= 0) continue;
      const float expect = rate * perSub[s] * elapsed;
      int spawn = (int)expect + ((rnd() & 1023) < (uint32_t)((expect - (int)expect) * 1024) ? 1 : 0);
      for (int k = 0; k < spawn && !px_.empty(); k++)
        for (int tries = 0; tries < 12; tries++) {
          const size_t i = rnd() % px_.size();
          if (psub_[i] != s) continue;
          const float vy = L.kind == kLookMolten ? 0.05f + (rnd() & 63) / 2000.f : 0.12f + (rnd() & 63) / 400.f;
          const int hv = phome_[i];
          const bool in = hv >= 0 && VesselAlive(hv);
          const V2 l = in ? ToLocal(vessels_[hv].x, px_[i]) : px_[i];
          bubbles_.push_back({px_[i].x, px_[i].y, vy, (uint8_t)s, 0, (int8_t)(in ? hv : -1), l.x, l.y});
          break;
        }
    }
  }

  // 5. Compose. A row at a time on the bench's workers: each pixel reads
  // the fields above and writes only itself.
  BenchParallelFor(H, 24, [&](int yBegin, int yEnd) {
  for (int y = yBegin; y < yEnd; y++) {
    const int row = H - 1 - y;  // image rows top-down
    for (int x = 0; x < W; x++) {
      const size_t k = (size_t)y * W + x;
      uint32_t c = 0;
      float gl[3] = {0, 0, 0};
      if (anyGlow && rGlowOn_[(size_t)(y >> 2) * gw + (x >> 2)]) glowAt(x, y, gl);
      const float glowI = gl[0] + gl[1] + gl[2];
      if (grid_[k]) {
        const Grain& g = grains_[grid_[k] - 1];
        c = ToDisplay(subs_[g.sub].color[g.variant % 3]);
        // WET sand is darker (the world's water darkens what it soaks), and
        // a grain with open space above catches the light.
        const bool wet = rTotal_[k] > thresh * 0.6f;
        if (wet) c = Shade(c, 12, 255);
        else if (y + 1 < H && !grid_[k + W] && !liquid(k + W)) c = Lift(c, 3, 255);
        if (glowI > 1) c = AddRGB(c, gl[0] * 0.5f, gl[1] * 0.5f, gl[2] * 0.5f, 255);
      } else if (liquid(k)) {
        const int o = rOwner_[k];
        const int s = psub_[o];
        const Look& L = look_[s];
        const int var = pvar_[o];
        const int d = rDepth_[k];
        // A viscous liquid is smooth (the world gives it a smoothed normal so
        // it does not read as gelatin cubes): its palette entries are pulled
        // halfway to the first, and its body sits a shade darker.
        const uint32_t base = L.kind == kLookViscous ? Shade(MixRGB(L.col[0], L.col[var % 3], 0.5f, 255), 13, 255)
                                                     : L.col[var % 3];
        // The pixel above is another liquid: this is an interface.
        const bool iface = y + 1 < H && liquid(k + W) && psub_[rOwner_[k + W]] != s;
        const float sp = std::sqrt(Dot(pv_[o], pv_[o]));
        if (L.kind == kLookMolten) {
          const float h = heatOf(o);
          const float deep = std::min(1.0f, d / 18.0f);
          if (d <= 2) {
            // The cooling skin: dark, broken where the heat under it wells up.
            c = h > 0.78f ? Lift(L.col[2], 5, 255) : Shade(L.col[1], d == 1 ? 8 : 11, 255);
          } else {
            c = MixRGB(L.col[1], L.col[2], std::clamp(h * 0.8f + deep * 0.35f, 0.f, 1.f), 255);
            c = Lift(c, (int)(L.glow * 4 * (0.4f + 0.6f * h)), 255);
          }
        } else {
          const bool viscous = L.kind == kLookViscous;
          // Deeper is darker and richer; posterised in steps of 5 px.
          const int f = 16 - std::min(viscous ? 4 : 5, d / 6);
          const int alpha = L.alpha[d];
          c = Shade(base, f, alpha);
          if (d == 1) {
            c = Lift(base, viscous ? 5 : 8, std::max(alpha, 235));   // the surface line
          } else if (viscous && d == 3) {
            c = Lift(base, 5, alpha);   // the wet sheen under a thick surface
          } else if (iface) {
            c = Shade(base, 11, std::max(alpha, 220));
          } else if (!viscous && sp > 0.5f) {
            // Moving water carries light: brighter the faster it runs.
            c = Lift(c, std::min(7, (int)(sp * 4)), std::max(alpha, 200));
          }
          if (L.glow > 0.05f) {
            // Its own light, breathing.
            const float br = L.glow * (0.75f + 0.25f * std::sin(t * 0.02f + var * 0.01f));
            c = Lift(c, (int)(br * 5), std::min(255, (int)(c >> 24) + (int)(br * 60)));
          }
        }
        if (glowI > 1 && L.glow <= 0.05f) c = AddRGB(c, gl[0] * 0.35f, gl[1] * 0.35f, gl[2] * 0.35f, (int)(c >> 24));
      } else if (glowI > 1.5f) {
        // Air round glowing matter: a halo in its colour.
        const float m = std::max({gl[0], gl[1], gl[2]});
        const float a = std::min(150.f, glowI * 0.9f);
        c = PackRGBA((int)(gl[0] * 255 / m), (int)(gl[1] * 255 / m), (int)(gl[2] * 255 / m), (int)a);
      }
      if (wall_[k]) {
        // Glass: pale, mostly transparent, a highlight on the left faces;
        // lit by whatever glows beside it.
        // A sack's edge is its leather, darker than its face and opaque.
        const int wv = wall_[k] - 1;
        const bool sack = wv < (int)vessels_.size() && !vessels_[wv].shape.glass;
        // Glass is lit along its whole left face (a continuous line; a dash
        // every 3 rows read as a seam in the glass). Leather keeps the dash:
        // it reads as stitching.
        bool lit = (x > 0 && !wall_[k - 1]) && (!sack || ((y / 3) & 1));
        if (highlight_ >= 0 && wall_[k] == highlight_ + 1)
          c = lit ? PackRGBA(255, 244, 196, 240) : PackRGBA(232, 208, 140, 200);
        else if (sack)
          c = lit ? PackRGBA(112, 76, 46, 255) : PackRGBA(62, 40, 24, 255);
        else
          c = lit ? PackRGBA(222, 238, 252, 170) : PackRGBA(190, 216, 238, 130);
        if (glowI > 1) c = AddRGB(c, gl[0] * 0.7f, gl[1] * 0.7f, gl[2] * 0.7f, std::min(255, (int)(c >> 24) + (int)glowI));
      }
      out[(size_t)row * W + x] = c;
    }
  }
  });

  // Bubbles over it all.
  for (const Bubble& b : bubbles_) {
    const int x = (int)b.x, y = (int)b.y;
    if (x < 0 || y < 0 || x >= W || y >= H) continue;
    const Look& L = look_[b.sub];
    auto put = [&](int xx, int yy, uint32_t c) {
      if (xx < 0 || yy < 0 || xx >= W || yy >= H || wall_[(size_t)yy * W + xx]) return;
      out[(size_t)(H - 1 - yy) * W + xx] = c;
    };
    if (L.kind == kLookMolten) {
      if (b.age >= 200) {
        // Burst at the surface: a spit of bright matter.
        put(x, y + 1, Lift(L.col[2], 12, 255));
        put(x - 1, y + 2, Lift(L.col[2], 8, 220));
        put(x + 1, y + 2, Lift(L.col[2], 8, 220));
      } else {
        const uint32_t rim = Lift(L.col[2], 9, 255);
        put(x, y, Shade(L.col[1], 10, 255));
        put(x - 1, y, rim); put(x + 1, y, rim); put(x, y + 1, rim); put(x, y - 1, rim);
      }
    } else {
      put(x, y, Lift(L.col[2], 12, 250));
    }
  }

  if (stickOn_) {
    int x0 = (int)std::floor(std::min(stickA_.x, stickB_.x) - stickR_ - 1);
    int x1 = (int)std::ceil(std::max(stickA_.x, stickB_.x) + stickR_ + 1);
    int y0 = (int)std::floor(std::min(stickA_.y, stickB_.y) - stickR_ - 1);
    int y1 = (int)std::ceil(std::max(stickA_.y, stickB_.y) + stickR_ + 1);
    for (int y = std::max(0, y0); y <= std::min(H - 1, y1); y++)
      for (int x = std::max(0, x0); x <= std::min(W - 1, x1); x++) {
        V2 p{x + 0.5f, y + 0.5f};
        float tt;
        float d = Len(p - ClosestOnSeg(p, stickA_, stickB_, &tt));
        if (d > stickR_) continue;
        bool rim = d > stickR_ - 1.f;
        out[(size_t)(H - 1 - y) * W + x] = rim ? PackRGBA(92, 60, 34, 255) : PackRGBA(150, 104, 62, 255);
      }
  }
  RenderChem(out);

  // 6. THE GLASS BODY. The sim is a cross-section; a bottle is round. Its
  // whole inside wears a faint pale tint that thickens toward the sides (the
  // eye looks through more glass there), and whatever is in it -- liquid,
  // grains, gas, the stick -- is seen THROUGH that front wall: veiled a
  // little, and darker toward the sides as it curves away. A highlight
  // stripe runs up the left of the belly and neck, a fainter one on the
  // right. All of it a CONTINUOUS function of the position across the
  // width (u, -1..1): stepped bands read as vertical seams in the glass.
  // Over everything else, so it goes last.
  // A SACK (a pouch, VesselShape::glass unset) is the same pass with the
  // glass swapped for LEATHER: opaque, so what it holds is hidden, shaded
  // round in the same bands, with a grain that rides with the sack.
  for (int vi = 0; vi < (int)vessels_.size(); vi++) {
    if (!VesselAlive(vi)) continue;
    const Vessel& v = vessels_[vi];
    if (v.broken || v.shape.profile.size() < 2) continue;
    const bool sack = !v.shape.glass;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (V2 o : v.outline) {
      const V2 w = ToWorld(v.x, o);
      x0 = std::min(x0, w.x); x1 = std::max(x1, w.x); y0 = std::min(y0, w.y); y1 = std::max(y1, w.y);
    }
    const int ix0 = std::max(0, (int)std::floor(x0)), ix1 = std::min(W - 1, (int)std::ceil(x1));
    const int iy0 = std::max(0, (int)std::floor(y0)), iy1 = std::min(H - 1, (int)std::ceil(y1));
    const float ca = std::cos(-v.x.angle), sa = std::sin(-v.x.angle);
    const auto& pr = v.shape.profile;
    const float hw = v.shape.width * 0.5f, VH = v.shape.height;
    const bool raster = !v.near.empty();
    BenchParallelFor(iy1 - iy0 + 1, 16, [&](int rb, int re) {
    for (int y = iy0 + rb; y < iy0 + re; y++)
      for (int x = ix0; x <= ix1; x++) {
        const size_t k = (size_t)y * W + x;
        if (wall_[k]) continue;
        const float dx = x + 0.5f - v.x.pos.x, dy = y + 0.5f - v.x.pos.y;
        const V2 l{dx * ca - dy * sa, dx * sa + dy * ca};
        // The near-glass raster answers for all but the pixels by the glass
        // (a far cell is a whole contact radius from it, a pixel's offset
        // cannot cross it); the polygon test only there.
        const uint8_t nr = raster ? v.Near(l) : Vessel::kNear;
        if (nr == Vessel::kFarOut || (nr == Vessel::kNear && !InsideLocal(v, l))) continue;
        // The inside's half-width at this height, from the profile.
        const float hy = std::clamp(l.y / VH, pr.front().y, pr.back().y);
        size_t j = 1;
        while (j + 1 < pr.size() && pr[j].y < hy) j++;
        const float span = std::max(1e-4f, pr[j].y - pr[j - 1].y);
        const float fw = pr[j - 1].x + (pr[j].x - pr[j - 1].x) * std::clamp((hy - pr[j - 1].y) / span, 0.f, 1.f);
        const float u = std::clamp(l.x / std::max(1.0f, fw * hw - v.shape.wall), -1.f, 1.f);
        const float e = std::fabs(u), e2 = e * e;
        // The two highlights: soft gaussian stripes, left strong, right faint.
        const float qL = (u + 0.56f) / 0.10f, qR = (u - 0.76f) / 0.055f;
        const float hiL = std::exp(-qL * qL), hiR = std::exp(-qR * qR);
        uint32_t& o = out[(size_t)(H - 1 - y) * W + x];
        if (sack) {
          float f = 1.0f - 0.32f * e2 + 0.07f * hiL;
          // Grain: a hash of the pixel in the sack's own frame.
          const uint32_t gx = (uint32_t)(int)std::floor(l.x + 512.f), gy = (uint32_t)(int)std::floor(l.y + 512.f);
          uint32_t hsh = gx * 73856093u ^ gy * 19349663u;
          hsh ^= hsh >> 13; hsh *= 0x5bd1e995u; hsh ^= hsh >> 15;
          if ((hsh & 7u) == 0) f -= 0.06f;
          o = PackRGBA((int)(138 * f), (int)(96 * f), (int)(60 * f), 255);
          continue;
        }
        // Thicker toward the sides, steeply only near the edge (Fresnel-ish):
        // ~10 of 255 in the middle, ~75 at the wall.
        const float glassA = 10.0f + 65.0f * e2 * e2;
        const float h = (46.0f * hiL + 22.0f * hiR) / 255.0f;
        const int a = (int)(o >> 24);
        float r, g, b, oa;
        if (a == 0) {
          r = 205; g = 228; b = 245; oa = glassA;
        } else {
          // Contents: darker toward the sides, then the glass laid over them.
          const float f = 1.0f - 0.16f * e2;
          const float veil = glassA / 255.0f * 0.8f;
          r = ChR(o) * f; g = ChG(o) * f; b = ChB(o) * f;
          r += (205 - r) * veil; g += (228 - g) * veil; b += (245 - b) * veil;
          oa = a + (255 - a) * glassA / 255.0f;
        }
        r += (255 - r) * h; g += (255 - g) * h; b += (255 - b) * h;
        if (a == 0) oa = std::max(oa, h * 255.0f);
        o = PackRGBA((int)std::lround(r), (int)std::lround(g), (int)std::lround(b), (int)std::lround(oa));
      }
    });
  }
}

}  // namespace alchemy
