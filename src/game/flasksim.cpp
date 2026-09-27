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
  SeedVessel((int)vessels_.size() - 1, c);
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
          if (PlaceGrain(g, (int)std::floor(w.x), (int)std::floor(w.y))) left--;
          else spilledUnits_[L.sub]++;  // no room at all: never silently lost
        }
      }
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
  for (const Vessel& v : vessels_) {
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
          if (Len(p - ClosestOnSeg(p, a, b, nullptr)) <= r) wall_[(size_t)y * W + x] = 1;
        }
    }
  }
}

// ---- liquid ----------------------------------------------------------------

void FlaskSim::BuildCells() {
  const int n = (int)px_.size();
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
  const int n = (int)px_.size();
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

  // 4. velocity from the net displacement; drop what left the panel.
  for (int i = 0; i < n; i++) pv_[i] = px_[i] - pprev_[i];
  int w = 0;
  for (int i = 0; i < n; i++) {
    V2 p = px_[i];
    bool out = p.x < -4 || p.x > cfg_.gridW + 4 || p.y < 0 || p.y > cfg_.gridH + 64;
    if (out) {
      spilledUnits_[psub_[i]] += pw_[i];
      continue;
    }
    px_[w] = px_[i]; pv_[w] = pv_[i]; pprev_[w] = pprev_[i];
    psub_[w] = psub_[i]; pw_[w] = pw_[i]; mbar_[w] = mbar_[i];
    w++;
  }
  px_.resize(w); pv_.resize(w); pprev_.resize(w); psub_.resize(w); pw_.resize(w); mbar_.resize(w);
}

void FlaskSim::CollideLiquid() {
  const int n = (int)px_.size();
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
        moved = true;
      }
      if (moved) px_[i] = {v.x.pos.x + cur.x * cw - cur.y * sw, v.x.pos.y + cur.x * sw + cur.y * cw};
    }
  }

  // Grains are walls. A particle that ended in a grain's pixel is pulled back
  // along its own path to where it entered that pixel (a continuous
  // collision, so it keeps no velocity INTO the grain and gains none). Only
  // if it started inside one too (the grain moved onto it) does it hop to
  // the nearest free neighbour.
  const int W = cfg_.gridW, H = cfg_.gridH;
  auto solid = [&](int x, int y) {
    if (x < 0 || y < 0 || x >= W || y >= H) return false;
    size_t k = (size_t)y * W + x;
    return grid_[k] != 0;
  };
  for (int i = 0; i < n; i++) {
    int x = (int)std::floor(px_[i].x), y = (int)std::floor(px_[i].y);
    if (!solid(x, y)) continue;
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
    }
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
  const size_t N = (size_t)W * H;
  fieldW_.assign(N, 0.f);
  fieldM_.assign(N, 0.f);
  fieldV_.assign(N, V2{});
  fieldVisc_.assign(N, 0.f);
  const float R = spacing_ * 1.05f, R2 = R * R;
  const int ri = (int)std::ceil(R);
  for (int i = 0; i < n; i++) {
    int cx = (int)std::floor(px_[i].x), cy = (int)std::floor(px_[i].y);
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
  }
  pixStart_.assign((size_t)W * H + 1, 0);
  pixIdx_.resize(n);
  std::vector<int> pix(n, -1);
  for (int i = 0; i < n; i++) {
    int x = (int)std::floor(px_[i].x), y = (int)std::floor(px_[i].y);
    if (x < 0 || y < 0 || x >= W || y >= H) continue;
    pix[i] = y * W + x;
    pixStart_[pix[i] + 1]++;
  }
  for (size_t k = 0; k < (size_t)W * H; k++) pixStart_[k + 1] += pixStart_[k];
  std::vector<int> fill(pixStart_.begin(), pixStart_.end() - 1);
  for (int i = 0; i < n; i++)
    if (pix[i] >= 0) pixIdx_[fill[pix[i]]++] = i;
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
  for (int s = pixStart_[k]; s < pixStart_[k + 1]; s++) {
    int i = pixIdx_[s];
    V2 d{(float)(fromX - toX), (float)(fromY - toY)};
    px_[i] = px_[i] + d;
    pprev_[i] = pprev_[i] + d;
  }
}

void FlaskSim::MoveGrain(int gi, int x, int y) {
  Grain& g = grains_[gi];
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
        grid_[(size_t)g.y * W + g.x] = 0;
        dead.push_back(gi);
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

    // RESTING grain: the CA. One still inside the glass (the sweep could
    // not place it) waits for the glass to move off it; letting it fall
    // would drop it out through the wall.
    if (wall_[(size_t)g.y * W + g.x]) continue;
    if (g.y == 0) {
      // Falls out of the panel.
      if (!wall_[(size_t)g.x]) {
        spilledUnits_[g.sub] += 1;
        grid_[(size_t)g.y * W + g.x] = 0;
        dead.push_back(gi);
      }
      continue;
    }
    int cntHere;
    float mHere = LiquidMassAt(g.x, g.y, &cntHere, nullptr);
    // Lighter than the liquid it sits in: it floats up through it.
    if (cntHere && mHere > mg) {
      if (GrainFree(g.x, g.y + 1) && (Rand() & 3) == 0) {
        int cu;
        LiquidMassAt(g.x, g.y + 1, &cu, nullptr);
        if (cu) MoveGrain(gi, g.x, g.y + 1);
      }
      continue;
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

void FlaskSim::CarryGrains() {
  // The glass SWEEPS grains; it does not carry them. A grain the new walls
  // landed on moves to the nearest free pixel on the side of that vessel it
  // was on, and everything else is left to gravity — so sand in a tilting
  // flask slides and pours as it tips, instead of riding along as a block
  // and collapsing when the tilt stops. Step() caps a vessel's motion at
  // about a pixel per substep, so the sweep never jumps a grain.
  const int W = cfg_.gridW;
  for (size_t vi = 0; vi < vessels_.size(); vi++) {
    const Vessel& v = vessels_[vi];
    if (v.x.pos.x == v.prevX.pos.x && v.x.pos.y == v.prevX.pos.y && v.x.angle == v.prevX.angle) continue;
    for (size_t i = 0; i < grains_.size(); i++) {
      Grain& g = grains_[i];
      size_t k = (size_t)g.y * W + g.x;
      if (!wall_[k]) continue;
      V2 c{g.x + 0.5f, g.y + 0.5f};
      bool wasIn = InsideLocal(v, ToLocal(v.prevX, c));
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
  bfsParent_.resize((size_t)W * H);
  bfsSeen_.resize((size_t)W * H, 0);
  if (++bfsStamp_ == 0) { std::fill(bfsSeen_.begin(), bfsSeen_.end(), 0); bfsStamp_ = 1; }
  std::vector<int>& q = bfsQueue_;
  q.clear();
  q.push_back(start);
  bfsSeen_[start] = bfsStamp_;
  int found = -1;
  static const int kD[4][2] = {{0, 1}, {1, 0}, {-1, 0}, {0, -1}};
  for (size_t h = 0; h < q.size() && q.size() < 2048 && found < 0; h++) {
    int cur = q[h], cx = cur % W, cy = cur / W;
    for (auto& d : kD) {
      int x = cx + d[0], y = cy + d[1];
      if (x < 0 || y < 0 || x >= W || y >= H) continue;
      int k = y * W + x;
      if (bfsSeen_[k] == bfsStamp_ || wall_[k]) continue;
      if (InsideVessel(v, {x + 0.5f, y + 0.5f}) != wantIn) continue;
      bfsSeen_[k] = bfsStamp_;
      bfsParent_[k] = cur;
      if (!grid_[k]) { found = k; break; }
      q.push_back(k);
    }
  }
  if (found < 0) return false;
  // Walk back from the free pixel: each grain moves into its child's slot.
  int to = found;
  while (to != start) {
    int from = bfsParent_[to];
    int idx = grid_[from] - 1;
    Grain& g = grains_[idx];
    g.x = (int16_t)(to % W);
    g.y = (int16_t)(to / W);
    g.fx = g.x + 0.5f;
    g.fy = g.y + 0.5f;
    grid_[to] = idx + 1;
    grid_[from] = 0;
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
      // Glass never passes through glass: a move that would bring this
      // vessel's walls into another's is refused, and the vessel waits
      // where it is (the panel's hand is blocked, like a real one).
      if (!PoseClear(v, next)) continue;
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
        g.fx = c.x;
        g.fy = c.y;
        (void)W;
      }
    }

    StepLiquid();
    StepGrains();
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
    if ((s & 15) == 15 && MovingCount(cfg_.gravity * 6) == 0) break;
  }
  stickOn_ = on;
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
  for (const Grain& g : grains_) units[binOf({g.x + 0.5f, g.y + 0.5f}) * S + g.sub] += 1;
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
      c.Add(subs_[s].mat, (uint32_t)e[b]);
    }
  }
  return t;
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
