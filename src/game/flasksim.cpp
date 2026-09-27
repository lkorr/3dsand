#include "game/flasksim.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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

  cellsW_ = (int)std::ceil(cfg_.gridW / h_) + 1;
  cellsH_ = (int)std::ceil(cfg_.gridH / h_) + 1;
  grid_.assign((size_t)cfg_.gridW * cfg_.gridH, 0);
  wall_.assign((size_t)cfg_.gridW * cfg_.gridH, 0);
}

void FlaskSim::SetSubstances(const std::vector<Substance>& subs) {
  subs_ = subs;
  mass_.resize(subs.size());
  visc_.resize(subs.size());
  spilledUnits_.assign(subs.size(), 0);
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
}

V2 FlaskSim::ToLocal(const Xform& x, V2 w) const {
  V2 d = w - x.pos;
  float c = std::cos(-x.angle), s = std::sin(-x.angle);
  return {d.x * c - d.y * s, d.x * s + d.y * c};
}
V2 FlaskSim::ToWorld(const Xform& x, V2 l) const {
  float c = std::cos(x.angle), s = std::sin(x.angle);
  return {x.pos.x + l.x * c - l.y * s, x.pos.y + l.x * s + l.y * c};
}

void FlaskSim::BuildOutline(Vessel& v) const {
  const auto& pr = v.shape.profile;
  float hw = v.shape.width * 0.5f, H = v.shape.height;
  v.outline.clear();
  for (int i = (int)pr.size() - 1; i >= 0; i--) v.outline.push_back({-pr[i].x * hw, pr[i].y * H});
  for (size_t i = 0; i < pr.size(); i++) v.outline.push_back({pr[i].x * hw, pr[i].y * H});
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

int FlaskSim::AddVessel(const VesselShape& shape, const Xform& x, const Composition& c) {
  Vessel v;
  v.shape = shape;
  v.x = v.prevX = v.target = x;
  BuildOutline(v);
  vessels_.push_back(std::move(v));
  RebuildWalls();
  for (Vessel& o : vessels_) o.asleep = false, o.quiet = 0;
  SeedVessel((int)vessels_.size() - 1, c);
  nAct_ = (int)px_.size();
  return (int)vessels_.size() - 1;
}

void FlaskSim::SetVesselXform(int vi, const Xform& x) { vessels_[vi].target = x; }

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

void FlaskSim::SeedVessel(int vi, const Composition& c) {
  Vessel& v = vessels_[vi];
  // Substance slots present, heaviest first.
  struct Layer { int sub; uint32_t units; };
  std::vector<Layer> layers;
  for (int i = 0; i < c.n; i++) {
    int sub = -1;
    for (size_t s = 0; s < subs_.size(); s++)
      if (subs_[s].mat == c.p[i].mat) sub = (int)s;
    if (sub < 0 || c.p[i].eighths == 0) continue;
    layers.push_back({sub, c.p[i].eighths * (uint32_t)cfg_.unitsPerEighth});
  }
  std::stable_sort(layers.begin(), layers.end(), [&](const Layer& a, const Layer& b) {
    return subs_[a.sub].density > subs_[b.sub].density;
  });

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
          if (PlaceGrain(g, (int)std::floor(w.x), (int)std::floor(w.y))) left--;
          else spilledUnits_[L.sub]++;  // no room at all: never silently lost
        }
      }
      // Ran out of vessel before the grains ran out: the rest is spilled,
      // never dropped (it used to vanish -- 800 grains of a full pouch).
      spilledUnits_[L.sub] += left;
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
          if (InsideLocal(v, p) && clearOfGlass(p, clear + spacing_ * 0.35f)) pts.push_back(p);
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
      for (uint32_t k = 0; k < count; k++) {
        V2 lp = k < pts.size() ? pts[k] : V2{0, H * 0.9f + (float)(k % 7)};
        V2 w = ToWorld(v.x, lp);
        px_.push_back(w);
        pprev_.push_back(w);
        pv_.push_back({0, 0});
        psub_.push_back((uint8_t)L.sub);
        pw_.push_back((uint16_t)(k + 1 == count ? lastW : upp));
        mbar_.push_back(mass_[L.sub]);
        calm_.push_back((uint8_t)std::clamp(cfg_.calmSteps, 0, 255));
        phome_.push_back((int8_t)vi);
      }
      float top = count ? (count <= pts.size() ? pts[count - 1].y : H) : yCursor;
      yCursor = top + rowH * 0.5f;
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
    for (size_t i = 0; i + 1 < v.outline.size(); i++) {
      V2 a = ToWorld(v.x, v.outline[i]), b = ToWorld(v.x, v.outline[i + 1]);
      int x0 = std::max(0, (int)std::floor(std::min(a.x, b.x) - r - 1));
      int x1 = std::min(W - 1, (int)std::ceil(std::max(a.x, b.x) + r + 1));
      int y0 = std::max(0, (int)std::floor(std::min(a.y, b.y) - r - 1));
      int y1 = std::min(Hh - 1, (int)std::ceil(std::max(a.y, b.y) + r + 1));
      for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
          V2 p{x + 0.5f, y + 0.5f};
          if (Len(p - ClosestOnSeg(p, a, b, nullptr)) <= r) wall_[(size_t)y * W + x] = (uint8_t)std::min<size_t>(vIdx + 1, 255);
        }
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
    int cx = std::clamp((int)(px_[i].x / h_), 0, cellsW_ - 1);
    int cy = std::clamp((int)(px_[i].y / h_), 0, cellsH_ - 1);
    cellOf_[i] = cy * cellsW_ + cx;
    cellStart_[cellOf_[i] + 1]++;
  }
  for (int c = 0; c < nc; c++) cellStart_[c + 1] += cellStart_[c];
  std::vector<int> fill(cellStart_.begin(), cellStart_.end() - 1);
  for (int i = 0; i < n; i++) cellIdx_[fill[cellOf_[i]]++] = i;
}

template <class F>
void FlaskSim::ForNeighbours(int i, F&& f) const {
  int c = cellOf_[i];
  int cx = c % cellsW_, cy = c / cellsW_;
  for (int dy = -1; dy <= 1; dy++) {
    int yy = cy + dy;
    if (yy < 0 || yy >= cellsH_) continue;
    for (int dx = -1; dx <= 1; dx++) {
      int xx = cx + dx;
      if (xx < 0 || xx >= cellsW_) continue;
      int cc = yy * cellsW_ + xx;
      for (int k = cellStart_[cc]; k < cellStart_[cc + 1]; k++) {
        int j = cellIdx_[k];
        if (j != i) f(j);
      }
    }
  }
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
  nbrStart_.assign(n + 1, 0);
  nbr_.clear();
  for (int i = 0; i < n; i++) {
    ForNeighbours(i, [&](int j) {
      V2 d = px_[j] - px_[i];
      if (Dot(d, d) < reach2) nbr_.push_back(j);
    });
    nbrStart_[i + 1] = (int)nbr_.size();
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
  for (int i = 0; i < n; i++) {
    float mi = mass_[psub_[i]];
    pv_[i].y -= g * (1.f + buoy * (mi - mbar_[i]) / mi);
  }
  for (int i = 0; i < n; i++) {
    for (int e = nbrStart_[i]; e < nbrStart_[i + 1]; e++) {
      int j = nbr_[e];
      if (j < i) continue;
      V2 d = px_[j] - px_[i];
      float r = Len(d);
      if (r <= 1e-5f || r >= h) continue;
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

  // 2. advect, with a speed cap so one step never tunnels a wall.
  const float vmax = cfg_.maxSpeedFrac * h;
  for (int i = 0; i < n; i++) {
    float s = Len(pv_[i]);
    if (s > vmax) pv_[i] = pv_[i] * (vmax / s);
    pprev_[i] = px_[i];
    px_[i] = px_[i] + pv_[i];
  }

  // 3. double-density relaxation (Gauss-Seidel, Clavet §4). Number density,
  // not mass density: mass only decides who gives way.
  const float k = cfg_.stiffness, kn = cfg_.stiffnessNear;
  const float crossRest = cfg_.crossRest;
  const float h2 = h * h;
  for (int i = 0; i < n; i++) {
    const int e0 = nbrStart_[i], e1 = nbrStart_[i + 1];
    const float mi = mass_[psub_[i]];
    float rho = 0, rhoN = 0, rhoSame = 0, sw = 1.f, sm = mi;
    for (int e = e0; e < e1; e++) {
      int j = nbr_[e];
      V2 d = px_[j] - px_[i];
      float r2 = Dot(d, d);
      if (r2 >= h2) continue;
      float q = std::sqrt(r2) * invH;
      float w = (1 - q) * (1 - q);
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
    float rest = rho0_ * (same + crossRest * (1 - same));
    float P = k * (rho - rest);
    float PN = kn * rhoN;
    V2 dx{0, 0};
    for (int e = e0; e < e1; e++) {
      int j = nbr_[e];
      V2 d = px_[j] - px_[i];
      float r2 = Dot(d, d);
      if (r2 >= h2 || r2 < 1e-10f) continue;
      float r = std::sqrt(r2);
      float q = r * invH;
      float D = (P * (1 - q) + PN * (1 - q) * (1 - q)) * h * 0.5f;
      V2 rn = d * (1.f / r);
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
  const float keep = 1.0f - cfg_.damping;
  for (int i = 0; i < n; i++) {
    pv_[i] = (px_[i] - pprev_[i]) * keep;
    if (calm_[i]) {
      // Damped RELATIVE TO ITS VESSEL: halving the velocity in the world
      // froze the liquid of a flask picked up in its first second and a half
      // in place, and the rising glass pushed it through.
      V2 vv{0, 0};
      const int hv = phome_[i];
      if (hv >= 0 && hv < (int)vessels_.size() && !vessels_[hv].outline.empty()) {
        const Vessel& v = vessels_[hv];
        vv = ToWorld(v.x, ToLocal(v.prevX, px_[i])) - px_[i];
      }
      pv_[i] = vv + (pv_[i] - vv) * 0.5f;
      calm_[i]--;
    }
  }
  if (cfg_.xsph > 0) {
    xs_.assign(n, V2{});
    for (int i = 0; i < n; i++) {
      V2 acc{0, 0};
      float ws = 0;
      for (int e = nbrStart_[i]; e < nbrStart_[i + 1]; e++) {
        const int j = nbr_[e];
        const V2 d = px_[j] - px_[i];
        const float r2 = Dot(d, d);
        if (r2 >= h2) continue;
        const float q = std::sqrt(r2) * invH;
        const float w = (1 - q) * (1 - q);
        acc = acc + (pv_[j] - pv_[i]) * w;
        ws += w;
      }
      if (ws > 1e-6f) xs_[i] = acc * (cfg_.xsph / std::max(ws, 1.0f));
    }
    for (int i = 0; i < n; i++) pv_[i] = pv_[i] + xs_[i];
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
        continue;
      }
    }
    if (w != i) {
      px_[w] = px_[i]; pv_[w] = pv_[i]; pprev_[w] = pprev_[i];
      psub_[w] = psub_[i]; pw_[w] = pw_[i]; mbar_[w] = mbar_[i]; calm_[w] = calm_[i];
      phome_[w] = phome_[i];
    }
    w++;
  }
  nAct_ -= total - w;
  px_.resize(w); pv_.resize(w); pprev_.resize(w); psub_.resize(w); pw_.resize(w); mbar_.resize(w);
  calm_.resize(w); phome_.resize(w);
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

  for (const Vessel& v : vessels_) {
    const float R = v.shape.wall + pr;
    const auto& o = v.outline;
    const float c0 = std::cos(-v.x.angle), s0 = std::sin(-v.x.angle);
    const float c1 = std::cos(-v.prevX.angle), s1 = std::sin(-v.prevX.angle);
    const float cw = std::cos(v.x.angle), sw = std::sin(v.x.angle);
    // The outline's local bounding box grown by R: most particles are far
    // from most of the glass.
    float bx0 = 1e9f, bx1 = -1e9f, by0 = 1e9f, by1 = -1e9f;
    for (V2 p : o) { bx0 = std::min(bx0, p.x); bx1 = std::max(bx1, p.x); by0 = std::min(by0, p.y); by1 = std::max(by1, p.y); }
    bx0 -= R + 1; bx1 += R + 1; by0 -= R + 1; by1 += R + 1;
    for (int i = 0; i < n; i++) {
      // Both ends in the vessel's frame at their own time: the wall's motion
      // is then the particle's relative motion, and a crossing is a sign
      // flip of the cross product along a segment.
      V2 d0 = px_[i] - v.x.pos, d1 = pprev_[i] - v.prevX.pos;
      V2 cur{d0.x * c0 - d0.y * s0, d0.x * s0 + d0.y * c0};
      V2 prv{d1.x * c1 - d1.y * s1, d1.x * s1 + d1.y * c1};
      if ((cur.x < bx0 || cur.x > bx1 || cur.y < by0 || cur.y > by1) &&
          (prv.x < bx0 || prv.x > bx1 || prv.y < by0 || prv.y > by1))
        continue;
      bool moved = false;
      V2 hitN{0, 0}, hitC{0, 0};
      for (size_t s = 0; s + 1 < o.size(); s++) {
        V2 a = o[s], b = o[s + 1];
        float t;
        V2 c = ClosestOnSeg(cur, a, b, &t);
        V2 d = cur - c;
        float dist = Len(d);
        V2 ab = b - a;
        float sPrev = Cross(ab, prv - a), sCur = Cross(ab, cur - a);
        bool crossed = (sPrev > 0) != (sCur > 0) && t > 0 && t < 1;
        if (!crossed && dist >= R) continue;
        V2 nrm;
        if (crossed || dist < 1e-5f) {
          // Back to the side it came from.
          float l = Len(ab);
          V2 perp{-ab.y / l, ab.x / l};
          nrm = sPrev > 0 ? perp : perp * -1.f;
          c = ClosestOnSeg(cur, a, b, nullptr);
        } else {
          nrm = d * (1.f / dist);
        }
        cur = c + nrm * R;
        hitN = nrm;
        hitC = c;
        moved = true;
      }
      if (moved) {
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
        V2 rel = (px_[i] - pprev_[i]) - vw;
        const float vn = Dot(rel, nw);
        rel = rel - nw * vn;
        rel = rel * (1.0f - cfg_.wallFriction);
        pprev_[i] = px_[i] - (vw + rel);
      }
    }
  }

  // Grains are walls. A particle that ended in a grain's pixel is pulled back
  // along its own path to where it entered that pixel (a continuous
  // collision, so it keeps no velocity INTO the grain and gains none). Only
  // if it started inside one too (the grain moved onto it) does it hop to
  // the nearest free neighbour.
  const int W = cfg_.gridW, H = cfg_.gridH;
  auto grainAt = [&](int x, int y) {
    if (x < 0 || y < 0 || x >= W || y >= H) return false;
    return grid_[(size_t)y * W + x] != 0;
  };
  // Glass counts as solid for the pull-back: the glass may have moved onto
  // where the particle came from (a rising bottom), and pulling it back there
  // would put it through the wall.
  auto solid = [&](int x, int y) {
    if (x < 0 || y < 0 || x >= W || y >= H) return false;
    size_t k = (size_t)y * W + x;
    return grid_[k] != 0 || wall_[k] != 0;
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
        if (grid_[k] || wall_[k]) continue;
        float dd = (float)(dx * dx + dy * dy) - (dy > 0 ? 0.5f : 0.f);
        if (dd < best) { best = dd; bx = xx; by = yy; }
      }
    if (bx >= 0) {
      V2 d{(float)(bx - x), (float)(by - y)};
      px_[i] = px_[i] + d;
      pprev_[i] = pprev_[i] + d;  // carried, not launched
      continue;
    }
    // (one sweep memo epoch for this pass)
    // TRAPPED: grains and glass all round (liquid squeezed between a rising
    // bottom and a sand bed it cannot push). The liquid wins: the GRAIN is
    // shoved along its pile to the nearest free pixel on its side, which is
    // how pressure lifts a bed. Without this the particle stayed inside the
    // grain and the next pull-back put it through the glass.
    const int gi = grid_[(size_t)y * W + x] - 1;
    const int home = grains_[gi].home;
    if (home >= 0 && home < (int)vessels_.size() && !vessels_[home].outline.empty())
      SweepChain(gi, vessels_[home], true);
  }
}

// ---- powder ----------------------------------------------------------------

bool FlaskSim::GrainFree(int x, int y) const {
  if (x < 0 || y < 0 || x >= cfg_.gridW || y >= cfg_.gridH) return false;
  size_t k = (size_t)y * cfg_.gridW + x;
  return !grid_[k] && !wall_[k];
}

bool FlaskSim::PlaceGrain(Grain g, int nx, int ny) {
  // Nearest free pixel within a small ring search.
  for (int r = 0; r <= 4; r++)
    for (int dy = -r; dy <= r; dy++)
      for (int dx = -r; dx <= r; dx++) {
        if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
        int x = nx + dx, y = ny + dy;
        if (!GrainFree(x, y)) continue;
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
    fbx0_ = std::min(fbx0_, std::max(0, cx - ri));
    fbx1_ = std::max(fbx1_, std::min(W - 1, cx + ri));
    fby0_ = std::min(fby0_, std::max(0, cy - ri));
    fby1_ = std::max(fby1_, std::min(H - 1, cy + ri));
    // Moving liquid keeps the sand round it awake (a grain reads it to sink
    // or float); a sleeping vessel's liquid does not.
    if (i < nAct_ && cx >= 0 && cy >= 0 && cx < W && cy < H) {
      Wake(cx - 4, cy - 4);
      Wake(cx + 4, cy + 4);
    }
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

void FlaskSim::MoveGrain(int gi, int x, int y) {
  grainMoves_++;
  Grain& g = grains_[gi];
  Wake(g.x, g.y);
  Wake(x, y);
  const int W = cfg_.gridW;
  grid_[(size_t)g.y * W + g.x] = 0;
  ShoveLiquid(g.x, g.y, x, y);
  g.x = (int16_t)x;
  g.y = (int16_t)y;
  grid_[(size_t)y * W + x] = gi + 1;
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
      // FLUNG: ballistic, dragged toward the liquid's velocity.
      int cnt;
      V2 lv;
      LiquidMassAt(g.x, g.y, &cnt, &lv);
      g.vy -= cfg_.gravity;
      if (cnt) {
        float drag = 0.25f;
        g.vx += (lv.x - g.vx) * drag;
        g.vy += (lv.y - g.vy) * drag;
      }
      float nx = g.fx + g.vx, ny = g.fy + g.vy;
      if (nx < 0 || nx >= W || ny < 0) {
        spilledUnits_[g.sub] += 1;
        NoteExit((float)g.x + 0.5f, 1);
        grid_[(size_t)g.y * W + g.x] = 0;
        dead.push_back(gi);
        Wake(g.x, g.y);
        continue;
      }
      // Walk pixel by pixel toward the target; stop at the first blocked one.
      int steps = (int)std::ceil(std::max(std::fabs(g.vx), std::fabs(g.vy)));
      bool blocked = false;
      for (int s = 1; s <= steps; s++) {
        float t = (float)s / steps;
        int tx = (int)std::floor(g.fx + g.vx * t), ty = (int)std::floor(g.fy + g.vy * t);
        if (ty >= H) ty = H - 1;
        if (tx == g.x && ty == g.y) continue;
        if (!GrainFree(tx, ty)) { blocked = true; break; }
        MoveGrain(gi, tx, ty);
      }
      if (blocked) {
        g.vx *= 0.3f;
        g.vy = 0;
        g.fx = g.x + 0.5f;
        g.fy = g.y + 0.5f;
      } else {
        g.fx = nx;
        g.fy = std::min(ny, H - 0.5f);
      }
      if (std::fabs(g.vx) + std::fabs(g.vy) < 0.35f && (blocked || !GrainFree(g.x, g.y - 1))) {
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
        grid_[(size_t)g.y * W + g.x] = 0;
        dead.push_back(gi);
        Wake(g.x, g.y);
      }
      continue;
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
        if (ml >= mg) return false;  // floats on it
        float p = std::clamp(1.f - ml / mg, 0.05f, 1.f) * 0.6f;
        // viscosity of the liquid below: the heaviest-weighted guess is
        // the pixel's first particle, good enough for a settling rate.
        size_t k = (size_t)ty * W + tx;
        p /= 1.f + 8.f * fieldVisc_[k] / fieldW_[k];
        if ((Rand() & 1023) > (uint32_t)(p * 1023)) return true;  // waits, but blocked for now
      }
      MoveGrain(gi, tx, ty);
      return true;
    };
    if (trySink(g.x, g.y - 1)) continue;
    int d = (Rand() & 1) ? 1 : -1;
    if (!GrainFree(g.x + d, g.y) && !GrainFree(g.x - d, g.y)) continue;
    if (GrainFree(g.x + d, g.y) && trySink(g.x + d, g.y - 1)) continue;
    if (GrainFree(g.x - d, g.y)) trySink(g.x - d, g.y - 1);
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
  BuildOutline(tmp);
  return PoseClear(tmp, x);
}

// ---- sleep ------------------------------------------------------------------

void FlaskSim::Partition() {
  // Awake particles first. A stable partition by index so neighbours stay
  // near each other in memory.
  const int total = (int)px_.size();
  std::vector<int> order;
  order.reserve(total);
  auto awake = [&](int i) {
    const int h = phome_[i];
    return h < 0 || h >= (int)vessels_.size() || !vessels_[h].asleep;
  };
  for (int i = 0; i < total; i++) if (awake(i)) order.push_back(i);
  const int na = (int)order.size();
  for (int i = 0; i < total; i++) if (!awake(i)) order.push_back(i);
  auto perm = [&](auto& v) {
    auto c = v;
    for (int k = 0; k < total; k++) v[k] = c[order[k]];
  };
  perm(px_); perm(pv_); perm(pprev_); perm(psub_); perm(pw_); perm(mbar_); perm(calm_); perm(phome_);
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
    bool disturbed = moving;
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
      continue;
    }
    // Awake and undisturbed: is its liquid at rest? Judged on the RMS speed
    // and on how many are visibly moving, not on the fastest one: a settled
    // column never goes perfectly still (the relaxation leaves a ~0.1 px
    // shimmer in its deepest particles), and a max test never sleeps.
    double sum = 0;
    int cnt = 0, fast = 0;
    const float fast2 = 0.3f * 0.3f;
    for (int i = 0; i < nAct_; i++)
      if (phome_[i] == (int)vi) {
        const float v2 = Dot(pv_[i], pv_[i]);
        sum += v2;
        cnt++;
        fast += v2 > fast2;
      }
    const bool grainsBusy = [&] {
      for (const Grain& g : grains_)
        if (g.home == (int)vi && (g.vx != 0 || g.vy != 0)) return true;
      return false;
    }();
    if (!grainsBusy && (cnt == 0 || (sum / cnt < cfg_.sleepSpeed * cfg_.sleepSpeed && fast * 50 <= cnt))) {
      if (++v.quiet >= cfg_.sleepSteps) {
        v.asleep = true;
        changed = true;
        for (int i = 0; i < nAct_; i++)
          if (phome_[i] == (int)vi) { pv_[i] = {0, 0}; pprev_[i] = px_[i]; }
      }
    } else {
      v.quiet = 0;
    }
  }
  (void)total;
  if (changed) Partition();
}

void FlaskSim::UpdateGrainHomes() {
  const int W = cfg_.gridW;
  for (Grain& g : grains_) {
    if (wall_[(size_t)g.y * W + g.x]) continue;   // in glass: keep the last
    g.home = (int8_t)((int)inside_[(size_t)g.y * W + g.x] - 1);
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
      // Only a SUPPORTED grain is carried -- one resting on a grain, the
      // glass or liquid. A grain in the air is falling, not held, and a
      // vessel carrying its falling grains along held a cloud of sand up
      // inside a tipped flask.
      const size_t NF = fieldW_.size();
      auto supported = [&](const Grain& g) {
        if (g.y == 0) return true;
        const size_t k = (size_t)(g.y - 1) * W + g.x;
        return grid_[k] != 0 || wall_[k] != 0 || (k < NF && fieldW_[k] > kLiquidThresh);
      };
      std::vector<int> mine;
      if (d.x != 0 || d.y != 0)
        for (size_t i = 0; i < grains_.size(); i++) {
          const Grain& g = grains_[i];
          if (g.home == (int)vi && g.vx == 0 && g.vy == 0 && supported(g)) mine.push_back((int)i);
        }
      // A counting sort on the whole-pixel projection onto the move,
      // descending (leading edge first, so a grain moves into space its
      // neighbour left): O(n) where a comparison sort was most of a lift.
      std::vector<int> order(mine.size());
      if (!mine.empty()) {
        const float l = Len(d);
        const V2 dir = d * (1.f / l);
        const int span = W + cfg_.gridH + 2;
        const int NB = 2 * span;
        std::vector<int> cnt(NB + 1, 0), key(mine.size());
        for (size_t k = 0; k < mine.size(); k++) {
          const float pr = grains_[mine[k]].fx * dir.x + grains_[mine[k]].fy * dir.y;
          key[k] = std::clamp(span - 1 - (int)std::floor(pr), 0, NB - 1);
          cnt[key[k] + 1]++;
        }
        for (int bk = 0; bk < NB; bk++) cnt[bk + 1] += cnt[bk];
        for (size_t k = 0; k < mine.size(); k++) order[cnt[key[k]]++] = (int)k;
      }
      for (int k : order) {
        Grain& g = grains_[mine[k]];
        const float fx = g.fx + d.x, fy = g.fy + d.y;
        const int nx = (int)std::floor(fx), ny = (int)std::floor(fy);
        if (nx == g.x && ny == g.y) { g.fx = fx; g.fy = fy; continue; }
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
      if (!SweepChain((int)i, v, wasIn)) wedged_.push_back((int)i);
    }
  }
}

bool FlaskSim::SweepChain(int gi, const Vessel& v, bool wantIn) {
  const int W = cfg_.gridW, H = cfg_.gridH;
  const Grain& g0 = grains_[gi];
  const int start = g0.y * W + g0.x;
  const int g0x = g0.x, g0y = g0.y;
  const int vi = (int)(&v - vessels_.data());
  const uint32_t deadKey = deadEpoch_ * 2 + (wantIn ? 1u : 0u);
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
      // reaching the inside.
      if (glass && std::abs(x - g0x) + std::abs(y - g0y) > 5) continue;
      if (!glass && (inside_[k] == (uint8_t)(vi + 1)) != wantIn) continue;
      if (!glass && bfsDead_[k] == deadKey) continue;
      bfsSeen_[k] = bfsStamp_;
      bfsParent_[k] = cur;
      if (!glass && !grid_[k]) { found = k; break; }
      q.push_back(k);
    }
  }
  if (found < 0) {
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
    Wake(to % W, to / W);
    Wake(from % W, from / W);
    to = from;
  }
  return true;
}

// ---- stepping --------------------------------------------------------------

void FlaskSim::Step(int substeps) {
  for (int s = 0; s < substeps; s++) {
    // Kinematic vessels close the remaining distance to their target
    // evenly over the frame's substeps.
    float f = 1.f / (float)(substeps - s);
    bool anyMoved = false;
    for (Vessel& v : vessels_) {
      v.prevX = v.x;
      // Capped: no outline point moves more than maxVesselStep px, so the
      // glass sweeps grains and liquid instead of jumping them. A vessel
      // dragged faster than that lags its target and catches up.
      V2 dp = (v.target.pos - v.x.pos) * f;
      float da = (v.target.angle - v.x.angle) * f;
      if (dp.x == 0 && dp.y == 0 && da == 0) continue;
      float reach = 0;
      for (V2 o : v.outline) reach = std::max(reach, Len(o));
      float move = Len(dp) + std::fabs(da) * reach;
      if (move > cfg_.maxVesselStep) {
        float sc = cfg_.maxVesselStep / move;
        dp = dp * sc;
        da *= sc;
      }
      Xform next = v.x;
      next.pos = next.pos + dp;
      next.angle += da;
      // Glass never passes through glass. A move that would bring this
      // vessel's walls into another's SLIDES instead of freezing: the
      // rotation alone, the translation alone, then half of each -- so a
      // flask swung toward another's neck rides round it the way a hand
      // would steer, rather than stopping dead at the first touch.
      if (!PoseClear(v, next)) {
        Xform rotOnly = v.x, movOnly = v.x, half = v.x;
        rotOnly.angle += da;
        movOnly.pos = movOnly.pos + dp;
        half.pos = half.pos + dp * 0.5f;
        half.angle += da * 0.5f;
        if (PoseClear(v, rotOnly)) next = rotOnly;
        else if (PoseClear(v, movOnly)) next = movOnly;
        else if (PoseClear(v, half)) next = half;
        else continue;
      }
      v.x = next;
      anyMoved |= v.x.pos.x != v.prevX.pos.x || v.x.pos.y != v.prevX.pos.y || v.x.angle != v.prevX.angle;
    }
    if (anyMoved) {
      RebuildWalls();
      wedged_.clear();
      CarryGrains();
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
    grainMoves_ = 0;
    StepLiquid();
    StepGrains();
    active_ = anyMoved || stickOn_ || nAct_ > 0 || grainMoves_ > 0;
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
  for (int s = 0; s < maxSteps; s++) {
    Step(1);
    if ((s & 7) == 7 && MovingCount(cfg_.gravity * 6) == 0) break;
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
  for (size_t i = 0; i < px_.size(); i++) {
    if (InsideVessel(v, px_[i])) {
      units[psub_[i]] += pw_[i];
      continue;
    }
    if ((int)i < nAct_) act++;
    px_[w] = px_[i]; pv_[w] = pv_[i]; pprev_[w] = pprev_[i];
    psub_[w] = psub_[i]; pw_[w] = pw_[i]; mbar_[w] = mbar_[i]; calm_[w] = calm_[i];
    phome_[w] = phome_[i];
    w++;
  }
  nAct_ = act;
  px_.resize(w); pv_.resize(w); pprev_.resize(w); psub_.resize(w); pw_.resize(w); mbar_.resize(w);
  calm_.resize(w); phome_.resize(w);
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
    if (e) out.Add(subs_[s].mat, (uint32_t)e);
    spilledUnits_[s] += (uint32_t)r;
  }
  v.outline.clear();
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
  for (size_t i = 0; i < px_.size(); i++) units[binOf(px_[i]) * S + psub_[i]] += pw_[i];
  // Grains by the inside mask (the same answer UpdateGrainHomes and the
  // sweep use); one in the glass band belongs to its home vessel.
  const int GW = cfg_.gridW;
  for (const Grain& g : grains_) {
    const size_t k = (size_t)g.y * GW + g.x;
    int vi = wall_[k] ? g.home : (int)inside_[k] - 1;
    if (vi < 0 || vi >= (int)vessels_.size() || vessels_[vi].outline.empty()) vi = (int)B - 1;
    units[(size_t)vi * S + g.sub] += 1;
  }
  for (size_t s = 0; s < S; s++) units[(B - 1) * S + s] += spilledUnits_[s];

  // Units -> eighths, per substance, by largest remainder: the bins' eighths
  // sum to exactly the substance's total, which is a whole number of eighths
  // because it went in as one.
  Tally t;
  t.vessel.resize(vessels_.size());
  const uint64_t upe = (uint64_t)cfg_.unitsPerEighth;
  for (size_t s = 0; s < S; s++) {
    uint64_t total = 0;
    for (size_t b = 0; b < B; b++) total += units[b * S + s];
    t.totalUnits += (uint32_t)total;
    if (!total) continue;
    uint64_t eighths = (total + upe / 2) / upe;
    std::vector<uint64_t> e(B);
    std::vector<std::pair<uint64_t, size_t>> rem;
    uint64_t given = 0;
    for (size_t b = 0; b < B; b++) {
      e[b] = units[b * S + s] / upe;
      given += e[b];
      rem.push_back({units[b * S + s] % upe, b});
    }
    std::stable_sort(rem.begin(), rem.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (size_t r = 0; given < eighths && r < rem.size(); r++, given++) e[rem[r].second]++;
    for (size_t b = 0; b < B; b++) {
      if (!e[b]) continue;
      Composition& c = b + 1 == B ? t.spilled : t.vessel[b];
      // A vessel that would hold a 17th substance cannot: that portion is
      // counted as spilled (it would not fit through the lip either).
      if (!c.Add(subs_[s].mat, (uint32_t)e[b]) && &c != &t.spilled)
        t.spilled.Add(subs_[s].mat, (uint32_t)e[b]);
    }
  }
  return t;
}

Composition FlaskSim::DrainSpilled(float* exitX) {
  // Whole eighths only: the fraction stays in spilledUnits_, so Count() still
  // sees every unit and its per-substance totals stay whole eighths.
  Composition out;
  const uint32_t upe = (uint32_t)cfg_.unitsPerEighth;
  for (size_t s = 0; s < spilledUnits_.size(); s++) {
    const uint32_t e = spilledUnits_[s] / upe;
    if (!e) continue;
    if (!out.Add(subs_[s].mat, e)) continue;   // a 17th substance waits a step
    spilledUnits_[s] -= e * upe;
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
  const int W = cfg_.gridW, H = cfg_.gridH;
  out.assign((size_t)W * H, 0);
  // Liquid: splat each particle into a field; a pixel's colour is the
  // substance that contributed most to it.
  std::vector<float> total((size_t)W * H, 0.f), best((size_t)W * H, 0.f);
  std::vector<uint8_t> who((size_t)W * H, 0);
  const float R = spacing_ * 1.05f;
  const float R2 = R * R;
  const int ri = (int)std::ceil(R);
  for (size_t i = 0; i < px_.size(); i++) {
    int cx = (int)std::floor(px_[i].x), cy = (int)std::floor(px_[i].y);
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
        total[k] += w;
        if (w > best[k]) { best[k] = w; who[k] = psub_[i]; }
      }
  }
  const float thresh = kLiquidThresh;
  auto liquidAt = [&](int x, int y) {
    return x >= 0 && y >= 0 && x < W && y < H && total[(size_t)y * W + x] > thresh &&
           !grid_[(size_t)y * W + x];
  };
  for (int y = 0; y < H; y++) {
    const int row = H - 1 - y;  // image rows top-down
    for (int x = 0; x < W; x++) {
      size_t k = (size_t)y * W + x;
      uint32_t c = 0;
      if (grid_[k]) {
        const Grain& g = grains_[grid_[k] - 1];
        c = subs_[g.sub].color[g.variant % 3] | 0xFF000000u;
        // A grain with open space above catches the light.
        if (y + 1 < H && !grid_[k + W] && !liquidAt(x, y + 1)) c = Lift(c, 3, 255);
      } else if (total[k] > thresh) {
        const Substance& s = subs_[who[k]];
        bool surface = !liquidAt(x, y + 1);
        bool edge = who[k] != (y + 1 < H ? who[k + W] : who[k]) && liquidAt(x, y + 1);
        // Depth banding: every 6 px under the surface one step darker.
        int depth = 0;
        for (int yy = y + 1; yy < H && yy <= y + 24 && liquidAt(x, yy); yy++) depth++;
        int f = 16 - std::min(4, depth / 6);
        uint32_t base = s.color[((x / 3) + (y / 2)) % 3 == 0 ? 1 : 0];
        c = surface ? Lift(base, 5, 235) : edge ? Lift(base, 2, 225) : Shade(base, f, 215);
      }
      if (wall_[k]) {
        // Glass: pale, mostly transparent, a highlight on the left faces.
        bool lit = (x > 0 && !wall_[k - 1]) && ((y / 3) & 1);
        if (highlight_ >= 0 && wall_[k] == highlight_ + 1)
          c = lit ? PackRGBA(255, 244, 196, 240) : PackRGBA(232, 208, 140, 200);
        else
          c = lit ? PackRGBA(236, 246, 255, 210) : PackRGBA(170, 205, 230, 120);
      }
      out[(size_t)row * W + x] = c;
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
        float t;
        float d = Len(p - ClosestOnSeg(p, stickA_, stickB_, &t));
        if (d > stickR_) continue;
        bool rim = d > stickR_ - 1.f;
        out[(size_t)(H - 1 - y) * W + x] = rim ? PackRGBA(92, 60, 34, 255) : PackRGBA(150, 104, 62, 255);
      }
  }
}

}  // namespace alchemy
