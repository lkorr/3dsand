// THE ALCHEMY BENCH'S CHEMISTRY (docs/PLAN_alchemy_chemistry.md package C):
// the world's reaction rules on FlaskSim's particles, grains and gas pixels,
// the gas phase, the vessel's devices (stopper, burner, electrify), dissolving
// and the ledger. Engine-free, like flasksim.cpp: every rule, effect and
// species arrives through benchchem.h, built by flasksim_mats.h.
//
// WHY ON THE PARTICLES. The bench was a UI device whose reactions were to run
// on a vessel's Composition in the game tick; the owner wants reactions
// VISIBLE -- sand fizzing away in acid, fumes curling out of the neck, salt
// melting over a flame. A Composition has no neighbours, so the only place
// the world's PAIR rules can mean what they mean is where matter touches
// matter: here. The bench is still not world state (floats, no hash); what it
// changes reaches the game only through the tally, and the ledger below is
// what lets ValidateBench still prove that no matter came from nothing.
//
// WORLD SEMANTICS (sim_step.wgsl doReactions): per entity, its material's
// rules in order; each is gated (light), then rolled ONCE -- a pair rule
// against the first matching neighbour of a rotated scan, a decay on its own,
// an emit against the first free neighbour -- and the first that fires ends
// the entity's turn. Chances are per world tick; a chemistry step is
// chemEvery/8 of one at the bench's 240 substeps a second.
//
// QUANTA. A world cell is a whole voxel on both sides of a pair rule. Here a
// particle is unitsPerParticle units and a grain one, so a pair converts
// q = min(both) units of each side (a grain of sand eats one unit of an acid
// particle, not six), a decay or emit the whole entity. Units that change
// PHASE go where that phase lives: a gas into the gas grid, a powder into
// grains, a liquid into a POOL that becomes a particle once it holds a
// particle's worth -- and until then is counted where it is (Count, the
// audit). Nothing is ever rounded away.
//
// GAS IS VOLUMINOUS (SimConfig::gasExpand, 2026-09-27). The gas grid counts
// GAS units, gasExpand to a unit of matter, one a pixel at rest: a liquid
// unit becomes gasExpand pixels of vapour. Matter goes in x gasExpand; gas
// comes out through BankGas, which pays whole matter units and keeps the
// remainder as live gas. Pair quanta are measured in gas units (the finest).
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "game/flasksim.h"

namespace alchemy {

namespace {

inline V2 Add(V2 a, V2 b) { return {a.x + b.x, a.y + b.y}; }
inline V2 VSub(V2 a, V2 b) { return {a.x - b.x, a.y - b.y}; }
inline float Dot2(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
inline V2 operator+(V2 a, V2 b) { return {a.x + b.x, a.y + b.y}; }
inline V2 operator-(V2 a, V2 b) { return {a.x - b.x, a.y - b.y}; }
inline V2 operator*(V2 a, float s) { return {a.x * s, a.y * s}; }

inline uint32_t Pack(int r, int g, int b, int a) {
  auto c = [](int v) { return (uint32_t)std::clamp(v, 0, 255); };
  return c(r) | c(g) << 8 | c(b) << 16 | c(a) << 24;
}
inline int R8(uint32_t c) { return c & 255; }
inline int G8(uint32_t c) { return (c >> 8) & 255; }
inline int B8(uint32_t c) { return (c >> 16) & 255; }
inline int A8(uint32_t c) { return (c >> 24) & 255; }
// `src` over `dst` at alpha a (0..255), both 0xAABBGGRR; the result keeps the
// larger alpha so a cloud over empty table is seen.
inline uint32_t Over(uint32_t dst, int r, int g, int b, int a) {
  a = std::clamp(a, 0, 255);
  const int da = A8(dst);
  if (da == 0) return Pack(r, g, b, a);
  const int oa = std::max(da, a);
  return Pack(R8(dst) + (r - R8(dst)) * a / 255, G8(dst) + (g - G8(dst)) * a / 255,
              B8(dst) + (b - B8(dst)) * a / 255, oa);
}
// The palette is linear light; the bench draws to the screen (flasksim.cpp
// ToDisplay, the same 1/1.5 encode).
// A table: three pow() a gas pixel a picture were a third of RenderGas.
struct EncTable {
  uint8_t v[256];
  EncTable() {
    for (int i = 0; i < 256; i++) v[i] = (uint8_t)std::lround(255.0 * std::pow(i / 255.0, 1.0 / 1.5));
  }
};
inline int Enc(int v) {
  static const EncTable t;
  return t.v[std::clamp(v, 0, 255)];
}

inline uint8_t DirOf(float dx, float dy) {
  if (std::fabs(dy) >= std::fabs(dx)) return dy > 0 ? kChemUp : kChemDown;
  return kChemSide;
}

inline float Hash01(int x, int y, int z) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)z * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return ((h ^ (h >> 16)) & 1023) / 1023.0f;
}
// Smooth value noise (the smoke's wisps).
inline float Noise2(float x, float y) {
  const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
  float ax = x - x0, ay = y - y0;
  ax = ax * ax * (3 - 2 * ax);
  ay = ay * ay * (3 - 2 * ay);
  const float a = Hash01(x0, y0, 7), b = Hash01(x0 + 1, y0, 7), c = Hash01(x0, y0 + 1, 7),
              d = Hash01(x0 + 1, y0 + 1, 7);
  return (a + (b - a) * ax) * (1 - ay) + (c + (d - c) * ax) * ay;
}

}  // namespace

// ---- setup -----------------------------------------------------------------

void FlaskSim::SetChemistry(const Chemistry& c) {
  chem_ = c;
  if (chem_.rules.size() < subs_.size()) chem_.rules.resize(subs_.size());
  chemOn_ = !chem_.Empty();
}

// Dissolved portions go into their solvent, gas into the headspace -- after
// SeedVessel has laid the layers down (flasksim.cpp).
void FlaskSim::SeedExtras(int vi, const Composition& c) {
  const uint32_t upe = (uint32_t)cfg_.unitsPerEighth;
  const Vessel& v = vessels_[vi];
  const V2 bottom = ToWorld(v.x, {0.0f, v.shape.height * 0.15f});
  for (int i = 0; i < c.n; i++) {
    if (!c.p[i].eighths) continue;
    const uint16_t mat = c.p[i].mat;
    if (IsDissolved(mat)) {
      const int from = SlotOf(BaseMat(mat));
      if (from < 0) continue;
      uint64_t left = (uint64_t)c.p[i].eighths * upe;
      const ChemSolute* sp = chem_.SoluteFrom(from);
      if (sp) {
        // Every solvent particle of this vessel takes a share up to what it
        // can hold: proportional first, then a unit each round the rest.
        std::vector<int> cand;
        std::vector<uint32_t> cap;
        uint64_t capSum = 0;
        for (size_t k = 0; k < px_.size(); k++) {
          if (phome_[k] != vi || !sp->DissolvesIn(psub_[k])) continue;
          if (psol_[k] && psol_[k] != sp->species) continue;
          const uint32_t hold = sp->saturation * pw_[k] / std::max(1u, sp->yieldPerVoxel);
          if (hold <= pmass_[k]) continue;
          cand.push_back((int)k);
          cap.push_back(hold - pmass_[k]);
          capSum += hold - pmass_[k];
        }
        if (capSum) {
          const uint64_t give = std::min<uint64_t>(left, capSum);
          uint64_t given = 0;
          for (size_t a = 0; a < cand.size(); a++) {
            const uint32_t g = (uint32_t)std::min<uint64_t>(cap[a], cap[a] * give / capSum);
            pmass_[cand[a]] = (uint16_t)(pmass_[cand[a]] + g);
            if (g) psol_[cand[a]] = sp->species;
            cap[a] -= g;
            given += g;
          }
          for (size_t a = 0; given < give && a < cand.size() * 4; a++) {
            const size_t b = a % cand.size();
            if (!cap[b]) continue;
            pmass_[cand[b]]++;
            psol_[cand[b]] = sp->species;
            cap[b]--;
            given++;
          }
          left -= given;
        }
      }
      // More than the liquid can hold (or nothing to hold it): it stays
      // powder, at the bottom.
      if (left) AddPool(vi, from, (uint32_t)left, bottom);
      continue;
    }
    const int sl = SlotOf(mat);
    if (sl < 0 || !subs_[sl].gas) continue;
    // GAS: into the vessel's free inside pixels, top down, an even share a
    // pixel round and round so it starts as a cloud rather than a slab -- in
    // GAS units, gasExpand to a unit of matter (SimConfig::gasExpand).
    uint32_t left = c.p[i].eighths * upe * (uint32_t)GasE();
    const int W = cfg_.gridW, H = cfg_.gridH;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (V2 o : v.outline) {
      const V2 w = ToWorld(v.x, o);
      x0 = std::min(x0, w.x); x1 = std::max(x1, w.x); y0 = std::min(y0, w.y); y1 = std::max(y1, w.y);
    }
    std::vector<int> px;
    for (int y = std::min(H - 1, (int)y1); y >= std::max(0, (int)y0); y--)
      for (int x = std::max(0, (int)x0); x <= std::min(W - 1, (int)x1); x++) {
        const int k = y * W + x;
        if (inside_[k] != (uint8_t)(vi + 1) || wall_[k] || grid_[k]) continue;
        if (gasAmt_[k] && gasSub_[k] != sl) continue;
        px.push_back(k);
      }
    const uint32_t cap = (uint32_t)cfg_.gasPixelCap;
    const uint32_t share = px.empty() ? 1u : std::max<uint32_t>(1u, left / (uint32_t)px.size());
    for (int pass = 0; left && !px.empty() && pass < 64; pass++)
      for (int k : px) {
        if (!left) break;
        const uint32_t room = cap - std::min<uint32_t>(cap, gasAmt_[k]);
        const uint32_t put = std::min({left, room, share});
        if (!put) continue;
        gasSub_[k] = (uint8_t)sl;
        gasAmt_[k] = (uint16_t)(gasAmt_[k] + put);
        gasAge_[k] = 0;
        AddGasPixel(k);
        left -= put;
      }
    if (left) AddPool(vi, sl, left, ToWorld(v.x, {0.0f, v.shape.height * 0.8f}));
  }
}

// ---- devices ---------------------------------------------------------------

void FlaskSim::SetStopper(int vi, bool on) {
  if (!VesselAlive(vi) || vessels_[vi].stoppered == on) return;
  vessels_[vi].stoppered = on;
  vessels_[vi].asleep = false;
  RebuildWalls();
  wakeAll_ = true;
  needPartition_ = true;
  Partition();
}

void FlaskSim::SetBurner(int vi, bool on) {
  if (!VesselAlive(vi)) return;
  vessels_[vi].burner = on;
  anyDevice_ = true;
}

bool FlaskSim::Burning(int vi) const {
  if (!VesselAlive(vi)) return false;
  const Vessel& v = vessels_[vi];
  return v.burner && v.x.pos.y - cfg_.tableY <= cfg_.burnerReach && std::fabs(v.x.angle) < 0.6f;
}

void FlaskSim::Shock(int vi) {
  if (!VesselAlive(vi)) return;
  vessels_[vi].shock = cfg_.shockSteps;
  anyDevice_ = true;
}

void FlaskSim::UpdateDevices() {
  const float dt = cfg_.chemEvery / 240.0f;
  anyDevice_ = false;
  for (size_t vi = 0; vi < vessels_.size(); vi++) {
    Vessel& v = vessels_[vi];
    if (v.outline.empty()) continue;
    if (Burning((int)vi)) v.heat = std::min(1.0f, v.heat + dt / std::max(0.05f, cfg_.heatRiseSec));
    else v.heat = std::max(0.0f, v.heat - dt / std::max(0.05f, cfg_.heatFallSec));
    if (v.shock > 0) v.shock--;
    anyDevice_ |= v.burner || v.heat > 0.01f || v.shock > 0;
  }
}

int FlaskSim::GasUnits(int vi) const {
  if (!VesselAlive(vi)) return 0;
  int n = 0;
  for (int k : gasList_)
    if (gasAmt_[k] && inside_[k] == (uint8_t)(vi + 1)) n += gasAmt_[k];
  return n;
}

int FlaskSim::GasPixelsIn(int vi) const {
  if (!VesselAlive(vi)) return 0;
  int n = 0;
  for (int k : gasList_)
    if (gasAmt_[k] && inside_[k] == (uint8_t)(vi + 1)) n++;
  return n;
}

int FlaskSim::AirPixelsIn(int vi) const {
  if (!VesselAlive(vi)) return 0;
  const int W = cfg_.gridW, H = cfg_.gridH;
  if (inside_.size() != (size_t)W * H) return 0;
  // Only the vessel's own box: this runs every chemistry step now.
  const Vessel& v = vessels_[vi];
  float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
  for (V2 o : v.outline) {
    const V2 w = ToWorld(v.x, o);
    x0 = std::min(x0, w.x); x1 = std::max(x1, w.x); y0 = std::min(y0, w.y); y1 = std::max(y1, w.y);
  }
  int n = 0;
  for (int y = std::max(0, (int)y0); y <= std::min(H - 1, (int)y1); y++)
  for (int x = std::max(0, (int)x0); x <= std::min(W - 1, (int)x1); x++) {
    const size_t k = (size_t)y * W + x;
    if (inside_[k] != (uint8_t)(vi + 1) || wall_[k] || grid_[k] || gasAmt_[k]) continue;
    // Liquid, and the gaps in its packing (a particle's width round each
    // particle: no gas reaches those, they are not headspace): the
    // particles' pixel buckets (BucketChem, rebuilt every gas and chemistry
    // step).
    if (chemHead_.size() == inside_.size() && NearestParticle(x + 0.5f, y + 0.5f, spacing_) >= 0) continue;
    n++;
  }
  return n;
}

float FlaskSim::Pressure(int vi) const {
  if (!VesselAlive(vi)) return 0.0f;
  const Vessel& v = vessels_[vi];
  const int W = cfg_.gridW, H = cfg_.gridH;
  float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
  for (V2 o : v.outline) {
    const V2 w = ToWorld(v.x, o);
    x0 = std::min(x0, w.x); x1 = std::max(x1, w.x); y0 = std::min(y0, w.y); y1 = std::max(y1, w.y);
  }
  int area = 0, grains = 0;
  for (int y = std::max(0, (int)y0); y <= std::min(H - 1, (int)y1); y++)
    for (int x = std::max(0, (int)x0); x <= std::min(W - 1, (int)x1); x++) {
      const int k = y * W + x;
      if (inside_[k] != (uint8_t)(vi + 1) || wall_[k]) continue;
      area++;
      grains += grid_[k] != 0;
    }
  int liquid = 0;
  for (size_t i = 0; i < px_.size(); i++)
    if (phome_[i] == vi) liquid += pw_[i];
  const float free = std::max(2.0f + 0.03f * area, (float)(area - grains - liquid));
  // Hot gas pushes harder (P ~ nT): the glass's heat scales it up to 4x.
  // In natural volumes: gasRest units a pixel is one atmosphere.
  return (float)GasUnits(vi) / (free * (float)GasR()) * (1.0f + 3.0f * v.heat);
}

// ---- events ----------------------------------------------------------------

std::vector<SimEvent> FlaskSim::TakeEvents() {
  std::vector<SimEvent> out;
  out.swap(events_);
  return out;
}

void FlaskSim::RaiseEvent(const ChemEffect& fx, int hv, V2 at, uint16_t selfMat, uint16_t nbrMat,
                          const ChemRule& r) {
  for (SimEvent& e : events_)
    if (e.kind == fx.kind && e.vessel == hv) {
      // Merged: the event is as big as the biggest firing in it.
      e.count++;
      e.amount += fx.amount;
      e.radius = std::max(e.radius, fx.radius);
      e.power = std::max(e.power, fx.power);
      return;
    }
  SimEvent e;
  e.kind = fx.kind;
  e.vessel = hv;
  e.at = at;
  e.radius = fx.radius;
  e.power = fx.power;
  e.amount = fx.amount;
  e.what = fx.what;
  e.selfMat = selfMat;
  e.nbrMat = nbrMat;
  if (r.prodSelf >= 0) e.products.push_back(subs_[r.prodSelf].mat);
  if (r.prodNbr >= 0) e.products.push_back(subs_[r.prodNbr].mat);
  events_.push_back(e);
}

void FlaskSim::RaiseEvents(int hv, V2 at, uint16_t selfMat, uint16_t nbrMat, const ChemRule& r) {
  if (r.fx < 0) return;
  for (int i = 0; i < (int)r.fxCount && r.fx + i < (int)chem_.effects.size(); i++)
    RaiseEvent(chem_.effects[(size_t)(r.fx + i)], hv, at, selfMat, nbrMat, r);
}

void FlaskSim::RaisePressureEvent(const char* kind, int vi, float pressure) {
  SimEvent e;
  e.kind = kind;
  e.vessel = vi;
  const Vessel& v = vessels_[vi];
  e.at = ToWorld(v.x, {0.0f, v.shape.height});
  e.amount = pressure;
  // What was pushing: the vessel's gases, most first.
  std::vector<std::pair<int, int>> by;
  for (int k : gasList_)
    if (gasAmt_[k] && inside_[k] == (uint8_t)(vi + 1)) {
      bool found = false;
      for (auto& p : by)
        if (p.first == gasSub_[k]) { p.second += gasAmt_[k]; found = true; }
      if (!found) by.push_back({gasSub_[k], gasAmt_[k]});
    }
  std::sort(by.begin(), by.end(), [](auto& a, auto& b) { return a.second > b.second; });
  for (auto& p : by) e.products.push_back(subs_[p.first].mat);
  events_.push_back(e);
}

// A STOPPERED vessel whose gas outgrows its headspace: the stopper pops (the
// gas is free to leave) -- or, with the glass hot or the pressure far past
// the pop, the glass BURSTS and everything in it is loose on the table.
void FlaskSim::CheckPressure() {
  for (size_t vi = 0; vi < vessels_.size(); vi++) {
    Vessel& v = vessels_[vi];
    if (v.outline.empty() || !v.stoppered) continue;
    const float p = Pressure((int)vi);
    if (p < cfg_.popAt) continue;
    if (p >= cfg_.burstAt || v.heat > 0.25f || Burning((int)vi)) {
      RaisePressureEvent("burst", (int)vi, p);
      ShatterVessel((int)vi);
    } else {
      RaisePressureEvent("pop", (int)vi, p);
      SetStopper((int)vi, false);
    }
  }
}

void FlaskSim::ShatterVessel(int vi) {
  if (!VesselAlive(vi)) return;
  Vessel& v = vessels_[vi];
  const V2 c = ToWorld(v.x, {0.0f, v.shape.height * 0.4f});
  for (size_t i = 0; i < px_.size(); i++) {
    if (phome_[i] != vi && !InsideVessel(v, px_[i])) continue;
    phome_[i] = -1;
    V2 d = VSub(px_[i], c);
    const float l = std::max(1.0f, std::sqrt(Dot2(d, d)));
    pv_[i] = {d.x / l * 1.6f, d.y / l * 1.6f + 0.6f};
    pprev_[i] = VSub(px_[i], pv_[i]);
    calm_[i] = 0;
  }
  const int W = cfg_.gridW;
  for (Grain& g : grains_) {
    const size_t k = (size_t)g.y * W + g.x;
    if (g.home != vi && inside_[k] != (uint8_t)(vi + 1)) continue;
    g.home = -1;
    const float dx = g.x + 0.5f - c.x, dy = g.y + 0.5f - c.y;
    const float l = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
    g.fx = g.x + 0.5f;
    g.fy = g.y + 0.5f;
    g.vx = dx / l * 1.4f;
    g.vy = dy / l * 1.4f + 0.8f;
  }
  // The glass flies: shards off every stretch of the outline (look only).
  shardStep_ = step_;
  for (size_t k = 0; k + 1 < v.outline.size(); k++)
    for (int j = 0; j < 3; j++) {
      const float t = (j + 0.5f) / 3.0f;
      const V2 l{v.outline[k].x + (v.outline[k + 1].x - v.outline[k].x) * t,
                 v.outline[k].y + (v.outline[k + 1].y - v.outline[k].y) * t};
      const V2 w = ToWorld(v.x, l);
      V2 d = VSub(w, c);
      const float dl = std::max(1.0f, std::sqrt(Dot2(d, d)));
      const float sp = 0.6f + (Rand() % 100) / 100.0f;
      shards_.push_back({w.x, w.y, d.x / dl * sp, d.y / dl * sp + 0.5f, 90 + (int)(Rand() % 60)});
    }
  for (Pool& p : pools_)
    if (p.vessel == vi) {
      const V2 w = ToWorld(v.x, {(float)(p.sx / std::max(1e-9, p.w)), (float)(p.sy / std::max(1e-9, p.w))});
      p.sx = (double)w.x * p.w;
      p.sy = (double)w.y * p.w;
      p.vessel = -1;
    }
  // Its banked gas (under a unit of matter a gas) is loose now too.
  {
    const size_t S = subs_.size();
    if (gasBank_.size() >= ((size_t)vi + 2) * S)
      for (size_t s = 0; s < S; s++) {
        gasBank_[s] += gasBank_[((size_t)vi + 1) * S + s];
        gasBank_[((size_t)vi + 1) * S + s] = 0;
      }
  }
  v.outline.clear();
  v.broken = true;
  v.stoppered = false;
  v.burner = false;
  v.shock = 0;
  v.asleep = false;
  RebuildWalls();
  wakeAll_ = true;
  Partition();
}

// ---- ledger ------------------------------------------------------------------

int64_t FlaskSim::DissolvedUnits(int slot) const {
  int64_t n = 0;
  for (size_t i = 0; i < px_.size(); i++) {
    if (!pmass_[i]) continue;
    const ChemSolute* sp = chem_.Species(psol_[i]);
    if (sp && sp->from == slot) n += pmass_[i];
  }
  return n;
}

// Exact in GAS units (SimConfig::gasExpand to a matter unit): every matter
// count x E, plus the gas grid, gas pools and the bank as they are. A gas
// unit that left the grid without the bank paying for it -- or a bank that
// paid a matter unit it did not hold -- is a gap here.
bool FlaskSim::AuditUnits(std::string* why) const {
  const size_t S = subs_.size();
  const int64_t E = GasE();
  std::vector<int64_t> live(S, 0), gas(S, 0);
  for (size_t i = 0; i < px_.size(); i++) {
    live[psub_[i]] += pw_[i];
    if (pmass_[i]) {
      const ChemSolute* sp = chem_.Species(psol_[i]);
      if (sp && sp->from >= 0) live[sp->from] += pmass_[i];
    }
  }
  for (const Grain& g : grains_) live[g.sub] += 1;
  for (int k : gasList_)
    if (gasAmt_[k]) gas[gasSub_[k]] += gasAmt_[k];
  for (const Pool& p : pools_) (subs_[p.sub].gas ? gas : live)[p.sub] += p.units;
  for (size_t b = 0; b < gasBank_.size(); b++) gas[b % S] += gasBank_[b];
  bool ok = true;
  for (size_t s = 0; s < S; s++) {
    const int64_t have = (live[s] + spilledUnits_[s] + removed_[s] + drained_[s]) * E + gas[s];
    const int64_t want = (seeded_[s] + produced_[s] - consumed_[s]) * E;
    if (have != want) {
      ok = false;
      if (why) {
        char b[200];
        std::snprintf(b, sizeof b, "mat %u: have %lld/%lld units (live %lld + %lld gas units, spill %u off %lld drained %lld), ledger says %lld; ",
                      (unsigned)subs_[s].mat, (long long)have, (long long)E, (long long)live[s], (long long)gas[s],
                      spilledUnits_[s], (long long)removed_[s], (long long)drained_[s], (long long)want / E);
        *why += b;
      }
    }
  }
  return ok;
}

// ---- where units go --------------------------------------------------------

int FlaskSim::SpawnParticle(V2 p, int sub, int units, int home) {
  px_.push_back(p);
  pprev_.push_back(p);
  pv_.push_back({0, 0});
  psub_.push_back((uint8_t)sub);
  pw_.push_back((uint16_t)units);
  mbar_.push_back(mass_[sub]);
  calm_.push_back(0);
  phome_.push_back((int8_t)home);
  psrc_.push_back((int8_t)home);
  pvar_.push_back((uint8_t)(Rand() >> 7));
  panc_.push_back(p);
  pheat_.push_back(SeedHeat(p));
  psol_.push_back(0);
  pmass_.push_back(0);
  if (home >= 0 && home < (int)vessels_.size()) vessels_[home].asleep = false;
  needPartition_ = true;
  return (int)px_.size() - 1;
}

void FlaskSim::AddPool(int vessel, int sub, uint32_t q, V2 at) {
  if (!q) return;
  // A vessel's pool rides the vessel: kept in its own frame.
  if (vessel >= 0 && vessel < (int)vessels_.size() && !vessels_[vessel].outline.empty())
    at = ToLocal(vessels_[vessel].x, at);
  else
    vessel = -1;
  for (Pool& p : pools_)
    if (p.vessel == vessel && p.sub == sub) {
      p.units += q;
      p.sx += (double)at.x * q;
      p.sy += (double)at.y * q;
      p.w += q;
      return;
    }
  pools_.push_back({vessel, sub, q, (double)at.x * q, (double)at.y * q, (double)q});
}

bool FlaskSim::AddGasAt(int sub, uint32_t& q, int x, int y, int within, int reach) {
  const int W = cfg_.gridW, H = cfg_.gridH;
  const uint32_t cap = (uint32_t)cfg_.gasPixelCap;
  for (int r = 0; r <= reach && q; r++)
    for (int dy = -r; dy <= r && q; dy++)
      for (int dx = -r; dx <= r && q; dx++) {
        if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
        const int xx = x + dx, yy = y + dy;
        if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
        const int k = yy * W + xx;
        if (wall_[k]) continue;
        if (within >= -1 && inside_[k] != (uint8_t)(within + 1)) continue;
        if (gasAmt_[k] && gasSub_[k] != sub) continue;
        const uint32_t room = cap - std::min<uint32_t>(cap, gasAmt_[k]);
        const uint32_t put = std::min(q, room);
        if (!put) continue;
        if (!gasAmt_[k]) gasAge_[k] = 0;
        gasSub_[k] = (uint8_t)sub;
        gasAmt_[k] = (uint16_t)(gasAmt_[k] + put);
        AddGasPixel(k);
        q -= put;
      }
  return q == 0;
}

void FlaskSim::Deposit(int sub, uint32_t q, V2 at, int hv) {
  if (sub < 0 || !q) return;
  const Substance& S = subs_[sub];
  const int x = std::clamp((int)std::floor(at.x), 0, cfg_.gridW - 1);
  const int y = std::clamp((int)std::floor(at.y), 0, cfg_.gridH - 1);
  if (S.gas) {
    // Matter units in, GAS units on the grid: it expands -- and the room it
    // takes is a VOLUME SOURCE in the air where it was born (StepGasFlow):
    // fresh vapour pushes the air before it.
    uint32_t g = q * (uint32_t)GasE();
    EnsureGasFlow();
    gcBirth_[(size_t)(y / kGasCell) * gcW_ + x / kGasCell] += (float)g;
    if (!AddGasAt(sub, g, x, y, hv)) AddPool(hv, sub, g, at);
    return;
  }
  if (S.powder) {
    uint32_t left = q;
    while (left) {
      Grain g{};
      g.sub = (uint8_t)sub;
      g.variant = (uint8_t)(Rand() % 3);
      g.home = (int8_t)hv;
      g.chem = (uint8_t)chemStep_;
      if (!PlaceDeposit(g, x, y, hv)) break;
      if (grainDead_.size() < grains_.size()) grainDead_.resize(grains_.size(), 0);
      left--;
    }
    if (left) AddPool(hv, sub, left, at);
    if (hv >= 0 && hv < (int)vessels_.size()) vessels_[hv].grainBusy = true;
    return;
  }
  AddPool(hv, sub, q, at);
}

void FlaskSim::FlushPools() {
  const uint32_t upp = (uint32_t)cfg_.unitsPerParticle;
  for (Pool& p : pools_) {
    if (!p.units) continue;
    const V2 local{(float)(p.sx / std::max(1e-9, p.w)), (float)(p.sy / std::max(1e-9, p.w))};
    const Substance& S = subs_[p.sub];
    const int hv = p.vessel >= 0 && p.vessel < (int)vessels_.size() && !vessels_[p.vessel].outline.empty()
                       ? p.vessel : -1;
    const V2 at = hv >= 0 ? ToWorld(vessels_[hv].x, local) : local;
    if (S.gas) {
      AddGasAt(p.sub, p.units, (int)std::floor(at.x), (int)std::floor(at.y), hv);
    } else if (S.powder) {
      const int x = std::clamp((int)std::floor(at.x), 0, cfg_.gridW - 1);
      const int y = std::clamp((int)std::floor(at.y), 0, cfg_.gridH - 1);
      while (p.units) {
        Grain g{};
        g.sub = (uint8_t)p.sub;
        g.variant = (uint8_t)(Rand() % 3);
        g.home = (int8_t)hv;
        g.chem = (uint8_t)chemStep_;
        if (!PlaceDeposit(g, x, y, hv)) break;
        if (grainDead_.size() < grains_.size()) grainDead_.resize(grains_.size(), 0);
        p.units--;
      }
    } else {
      while (p.units >= upp) {
        const V2 j{at.x + (float)((int)(Rand() % 5) - 2) * 0.4f, at.y + (float)((int)(Rand() % 5) - 2) * 0.4f};
        SpawnParticle(j, p.sub, (int)upp, hv);
        p.units -= upp;
      }
    }
    p.sx = (double)local.x * p.units;
    p.sy = (double)local.y * p.units;
    p.w = p.units;
  }
  pools_.erase(std::remove_if(pools_.begin(), pools_.end(), [](const Pool& p) { return p.units == 0; }),
               pools_.end());
}

void FlaskSim::ReleaseSolute(int i, int hv) {
  if (!pmass_[i]) return;
  const ChemSolute* sp = chem_.Species(psol_[i]);
  if (!sp) { pmass_[i] = 0; psol_[i] = 0; return; }
  // What it can still hold: nothing if it is no longer a solvent (or no
  // longer anything), else its saturation at what it weighs now.
  uint32_t cap = 0;
  if (pw_[i] && sp->DissolvesIn(psub_[i]))
    cap = sp->saturation * pw_[i] / std::max(1u, sp->yieldPerVoxel);
  if (pmass_[i] <= cap) return;
  const uint32_t excess = pmass_[i] - cap;
  pmass_[i] = (uint16_t)cap;
  if (!pmass_[i]) psol_[i] = 0;
  // It comes out of solution as what the species precipitates to (its own
  // powder unless the table says otherwise -- then that is a conversion).
  const int to = sp->precipitate >= 0 ? sp->precipitate : sp->from;
  if (to != sp->from) {
    consumed_[sp->from] += excess;
    produced_[to] += excess;
  }
  Deposit(to, excess, px_[i], hv);
}

uint32_t FlaskSim::BankGas(int bin, int slot, uint32_t gasUnits) {
  const size_t S = subs_.size();
  const size_t need = (vessels_.size() + 1) * S;
  if (gasBank_.size() < need) gasBank_.resize(need, 0);
  uint32_t& b = gasBank_[(size_t)bin * S + (size_t)slot];
  b += gasUnits;
  const uint32_t E = (uint32_t)GasE();
  const uint32_t whole = b / E;
  b -= whole * E;
  return whole;
}

int64_t FlaskSim::FineOf(uint8_t type, int idx) const {
  switch (type) {
    case NbParticle: return (int64_t)pw_[idx] * GasE();
    case NbGrain: return GasE();
    case NbGas: return gasAmt_[idx];
    default: return INT64_MAX;
  }
}

// An entity's amount in ITS OWN units: matter units for a particle or a
// grain, GAS units for a gas pixel.
int FlaskSim::UnitsOf(uint8_t type, int idx) const {
  switch (type) {
    case NbParticle: return pw_[idx];
    case NbGrain: return 1;
    case NbGas: return gasAmt_[idx];
    default: return INT_MAX;
  }
}

void FlaskSim::TakeFromEnt(uint8_t type, int idx, int q, int hv) {
  if (type == NbParticle) {
    pw_[idx] = (uint16_t)(pw_[idx] - std::min<int>(q, pw_[idx]));
    ReleaseSolute(idx, hv);
    if (hv >= 0 && hv < (int)vessels_.size() && vessels_[hv].asleep) {
      vessels_[hv].asleep = false;
      needPartition_ = true;
    }
  } else if (type == NbGrain) {
    Grain& g = grains_[idx];
    if (grainDead_.size() < grains_.size()) grainDead_.resize(grains_.size(), 0);
    if (grainDead_[idx]) return;
    grainDead_[idx] = 1;
    grid_[(size_t)g.y * cfg_.gridW + g.x] = 0;
    Wake(g.x, g.y);
    const uint8_t in = inside_[(size_t)g.y * cfg_.gridW + g.x];
    if (in && in <= vessels_.size()) vessels_[in - 1].grainBusy = true;
  } else if (type == NbGas) {
    gasAmt_[idx] = (uint16_t)(gasAmt_[idx] - std::min<int>(q, gasAmt_[idx]));
    if (!gasAmt_[idx]) gasSub_[idx] = 0xFF;
  }
}

void FlaskSim::ConvertEnt(uint8_t type, int idx, int q, int to, int hv) {
  if (to == kChemKeep || q <= 0) return;
  int from = -1;
  V2 at{};
  if (type == NbParticle) { from = psub_[idx]; at = px_[idx]; }
  else if (type == NbGrain) { from = grains_[idx].sub; at = {grains_[idx].x + 0.5f, grains_[idx].y + 0.5f}; }
  else if (type == NbGas) { from = gasSub_[idx]; at = {(idx % cfg_.gridW) + 0.5f, (idx / cfg_.gridW) + 0.5f}; }
  if (from < 0 || from == to) return;
  if (type == NbGas) {
    // `q` is in GAS units. They leave the grid through the bank, which pays
    // the whole matter units they make (the ledger is in matter units) and
    // keeps the remainder as live gas of `from` -- so a wisp of one gas unit
    // that fades is not rounded away, and a pixel that turns into another
    // gas re-expands exactly the matter the bank paid.
    q = std::min<int>(q, gasAmt_[idx]);
    TakeFromEnt(type, idx, q, hv);
    const uint32_t m = BankGas(GasBin(hv), from, (uint32_t)q);
    consumed_[from] += m;
    if (to >= 0 && m) {
      produced_[to] += m;
      Deposit(to, m, at, hv);
    }
    return;
  }
  consumed_[from] += q;
  if (to >= 0) produced_[to] += q;
  const bool toLiquid = to >= 0 && !subs_[to].powder && !subs_[to].gas;
  // In place, when the whole entity changes and stays in its phase.
  if (type == NbParticle && to >= 0 && toLiquid && q >= pw_[idx]) {
    psub_[idx] = (uint8_t)to;
    ReleaseSolute(idx, hv);
    if (hv >= 0 && hv < (int)vessels_.size() && vessels_[hv].asleep) {
      vessels_[hv].asleep = false;
      needPartition_ = true;
    }
    return;
  }
  if (type == NbGrain && to >= 0 && subs_[to].powder) {
    Grain& g = grains_[idx];
    g.sub = (uint8_t)to;
    g.chem = (uint8_t)chemStep_;
    Wake(g.x, g.y);
    const uint8_t in = inside_[(size_t)g.y * cfg_.gridW + g.x];
    if (in && in <= vessels_.size()) vessels_[in - 1].grainBusy = true;
    return;
  }
  TakeFromEnt(type, idx, q, hv);
  if (to >= 0) Deposit(to, (uint32_t)q, at, hv);
}

// ---- neighbours --------------------------------------------------------------

void FlaskSim::BucketChem() {
  const int W = cfg_.gridW, H = cfg_.gridH;
  const size_t N = (size_t)W * H;
  if (chemHead_.size() != N) chemHead_.assign(N, -1);
  for (int k : chemTouched_) chemHead_[k] = -1;
  chemTouched_.clear();
  chemNext_.assign(px_.size(), -1);
  for (size_t i = 0; i < px_.size(); i++) {
    const int x = (int)std::floor(px_[i].x), y = (int)std::floor(px_[i].y);
    if (x < 0 || y < 0 || x >= W || y >= H) continue;
    const int k = y * W + x;
    if (chemHead_[k] < 0) chemTouched_.push_back(k);
    chemNext_[i] = chemHead_[k];
    chemHead_[k] = (int)i;
  }
}

int FlaskSim::NearestParticle(float x, float y, float r) const {
  const int W = cfg_.gridW, H = cfg_.gridH;
  const int ri = (int)std::ceil(r);
  const int cx = (int)std::floor(x), cy = (int)std::floor(y);
  int best = -1;
  float bd = r * r;
  for (int yy = std::max(0, cy - ri); yy <= std::min(H - 1, cy + ri); yy++)
    for (int xx = std::max(0, cx - ri); xx <= std::min(W - 1, cx + ri); xx++)
      for (int j = chemHead_[(size_t)yy * W + xx]; j >= 0; j = chemNext_[j]) {
        if (j >= (int)pw_.size() || !pw_[j]) continue;
        const float dx = px_[j].x - x, dy = px_[j].y - y, d2 = dx * dx + dy * dy;
        if (d2 < bd) { bd = d2; best = j; }
      }
  return best;
}

void FlaskSim::GatherParticleNbrs(int i, int hv, std::vector<ChemNb>& out) {
  out.clear();
  const int W = cfg_.gridW, H = cfg_.gridH;
  const V2 p = px_[i];
  const float R = spacing_ * 1.2f, R2 = R * R;
  const int ri = (int)std::ceil(R);
  const int cx = (int)std::floor(p.x), cy = (int)std::floor(p.y);
  for (int yy = std::max(0, cy - ri); yy <= std::min(H - 1, cy + ri) && out.size() < 10; yy++)
    for (int xx = std::max(0, cx - ri); xx <= std::min(W - 1, cx + ri) && out.size() < 10; xx++)
      for (int j = chemHead_[(size_t)yy * W + xx]; j >= 0 && out.size() < 10; j = chemNext_[j]) {
        if (j == i || !pw_[j]) continue;
        const V2 d = VSub(px_[j], p);
        if (Dot2(d, d) >= R2) continue;
        out.push_back({NbParticle, j, psub_[j], DirOf(d.x, d.y), px_[j]});
      }
  if (cx >= 0 && cy >= 0 && cx < W && cy < H) {
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++) {
        const int xx = cx + dx, yy = cy + dy;
        if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
        const int k = yy * W + xx;
        if (grid_[k]) {
          const int gi = grid_[k] - 1;
          if (gi < (int)grainDead_.size() && grainDead_[gi]) continue;
          out.push_back({NbGrain, gi, grains_[gi].sub, DirOf((float)dx, (float)dy), {xx + 0.5f, yy + 0.5f}});
        } else if (gasAmt_[k] && (std::abs(dx) + std::abs(dy)) <= 1) {
          out.push_back({NbGas, k, gasSub_[k], DirOf((float)dx, (float)dy), {xx + 0.5f, yy + 0.5f}});
        }
      }
    // AIR: a free pixel a particle's width off in one of the four
    // directions -- no glass, no grain, no GAS (a vapour is not air: a
    // surface under a headspace full of vapour stops evaporating), no other
    // liquid there -- and no particle of the list in that direction's 60-degree
    // cone. The cone is what keeps a particle INSIDE the liquid from seeing
    // air in a gap of the packing (the probe pixel alone did, and ether
    // boiled from inside its own bulk).
    //
    //
    // IN A VESSEL THE AIR IS THE VESSEL'S (owner, 2026-09-27: "the ether
    // gas should take up all of the space removing air, which should stop the
    // liquid from continuing to vaporize"). The vapour over the surface does
    // not stop it: what it makes is BORN as volume (StepGasFlow) and pushes
    // the vapour pool up and the air out of the mouth, so the bottle fills
    // from its liquid up -- until the vessel holds ~1 atmosphere (the count
    // below): a stoppered flask then holds, never popping on its own vapour;
    // an open one goes on only as its vapour pours over the lip. OUT OF
    // GLASS a probe holding vapour is air only while that vapour is under
    // SimConfig::gasSaturate of its natural volume: a puddle under its own
    // pooled vapour waits.
    static const int kD[4][2] = {{0, 1}, {1, 0}, {-1, 0}, {0, -1}};
    const size_t nPart = out.size();
    const bool inV = hv >= 0 && hv < (int)vesselAir_.size();
    const bool vesselAir = !inV || vesselAir_[hv] < (vessels_[hv].stoppered ? 970 : 1100);
    for (auto& d : kD) {
      if (!vesselAir) break;
      const float qx = p.x + d[0] * spacing_ * 0.9f, qy = p.y + d[1] * spacing_ * 0.9f;
      const int xx = (int)std::floor(qx), yy = (int)std::floor(qy);
      if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
      const int k = yy * W + xx;
      if (wall_[k] || grid_[k]) continue;
      if (gasAmt_[k] && !inV && (float)gasAmt_[k] >= cfg_.gasSaturate * (float)GasR()) continue;
      bool covered = false;
      for (size_t a = 0; a < nPart && !covered; a++) {
        if (out[a].type != NbParticle) continue;
        const V2 e = VSub(out[a].at, p);
        const float along = e.x * d[0] + e.y * d[1];
        covered = along > 0 && along * along > 0.25f * Dot2(e, e);
      }
      if (covered) continue;
      if (NearestParticle(xx + 0.5f, yy + 0.5f, spacing_ * 0.6f) >= 0) continue;
      out.push_back({NbAir, k, -1, DirOf((float)d[0], (float)d[1]), {xx + 0.5f, yy + 0.5f}});
    }
  }
  if (hv >= 0 && hv < (int)vessels_.size() && !vessels_[hv].outline.empty()) {
    const Vessel& v = vessels_[hv];
    if (v.heat > 0.02f && chem_.heat.on && v.Near(ToLocal(v.x, p)) == Vessel::kNear)
      out.push_back({NbHeat, hv, -1, kChemDown, p});
    if (v.shock > 0 && chem_.spark.on) out.push_back({NbSpark, hv, -1, kChemUp, p});
  }
}

void FlaskSim::GatherPixelNbrs(int x, int y, int hv, bool isGas, std::vector<ChemNb>& out) {
  out.clear();
  const int W = cfg_.gridW, H = cfg_.gridH;
  static const int kD[4][2] = {{0, 1}, {1, 0}, {-1, 0}, {0, -1}};
  if (isGas) {
    // A bubble: the liquid it is in.
    const int j = NearestParticle(x + 0.5f, y + 0.5f, spacing_ * 0.6f);
    if (j >= 0) out.push_back({NbParticle, j, psub_[j], kChemSide, px_[j]});
  }
  for (auto& d : kD) {
    const int xx = x + d[0], yy = y + d[1];
    if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
    const int k = yy * W + xx;
    const uint8_t dir = DirOf((float)d[0], (float)d[1]);
    const V2 at{xx + 0.5f, yy + 0.5f};
    if (wall_[k]) continue;   // the glass is inert
    if (grid_[k]) {
      const int gi = grid_[k] - 1;
      if (gi < (int)grainDead_.size() && grainDead_[gi]) continue;
      out.push_back({NbGrain, gi, grains_[gi].sub, dir, at});
      continue;
    }
    const int j = NearestParticle(at.x, at.y, spacing_ * 0.8f);
    if (j >= 0) {
      out.push_back({NbParticle, j, psub_[j], dir, px_[j]});
      continue;
    }
    if (gasAmt_[k]) {
      out.push_back({NbGas, k, gasSub_[k], dir, at});
      continue;
    }
    out.push_back({NbAir, k, -1, dir, at});
  }
  if (hv >= 0 && hv < (int)vessels_.size() && !vessels_[hv].outline.empty()) {
    const Vessel& v = vessels_[hv];
    const V2 p{x + 0.5f, y + 0.5f};
    if (v.heat > 0.02f && chem_.heat.on && v.Near(ToLocal(v.x, p)) == Vessel::kNear)
      out.push_back({NbHeat, hv, -1, kChemDown, p});
  }
}

// ---- the rules -------------------------------------------------------------

bool FlaskSim::TryRules(uint8_t selfType, int selfIdx, int slot, const std::vector<ChemNb>& nb, int hv,
                        V2 at, double scale) {
  const auto& rules = chem_.rules[slot];
  const double den = (double)chem_.chanceDen;
  const bool inVessel = hv >= 0 && hv < (int)vessels_.size() && !vessels_[hv].outline.empty();
  const bool sealed = inVessel && vessels_[hv].stoppered;
  const int64_t E = GasE();
  // Matter units for a side of a pair that converts `qf` GAS units' worth
  // (a gas side converts its gas units as they are).
  auto matterOf = [&](int64_t qf) { return (int)std::max<int64_t>(1, (qf + E - 1) / E); };
  const int nn = (int)nb.size();
  auto matches = [&](const ChemRule& r, const ChemNb& n) {
    switch (n.type) {
      case NbAir: return ChemNbrMatches(r, 0, 0, 0, true);
      case NbHeat: return ChemNbrMatches(r, chem_.heat.mat, chem_.heat.tags, chem_.heat.klass, false);
      case NbSpark: return ChemNbrMatches(r, chem_.spark.mat, chem_.spark.tags, chem_.spark.klass, false);
      default: {
        const Substance& s = subs_[n.slot];
        return ChemNbrMatches(r, s.mat, s.tagMask, s.klass, false);
      }
    }
  };
  auto nbMat = [&](const ChemNb& n) -> uint16_t {
    if (n.slot >= 0) return subs_[n.slot].mat;
    if (n.type == NbSpark && chem_.spark.mat != 0xFFFFFFFFu) return (uint16_t)chem_.spark.mat;
    return 0;
  };
  for (const ChemRule& r : rules) {
    if (!ChemGateOpen(r.cond, chem_.daylight)) continue;
    // THE CONCENTRATION CONDITION (benchchem.h ChemRule::soluteSpecies), the
    // world's solRuleAllows on a particle: only a LIQUID self (a particle)
    // carrying that species, at a concentration inside the rule's range.
    // Brine under the Electrify button splits; fresh water does not.
    if (r.soluteSpecies != 0) {
      if (selfType != NbParticle || r.soluteSpecies == kChemSoluteNever) continue;
      if (psol_[selfIdx] != r.soluteSpecies) continue;
      const ChemSolute* sp = chem_.Species(r.soluteSpecies);
      if (!sp) continue;
      const uint32_t c = ChemConcentration(pmass_[selfIdx], (uint32_t)pw_[selfIdx], sp->yieldPerVoxel);
      if (c < r.soluteMin || c > r.soluteMax) continue;
    }
    // WHAT TURNS TO AIR INSIDE GLASS (owner, 2026-09-27: "matter + air =
    // deletes should still take place except for things that are purposefully
    // to 'dry them up' like blood ... [and] should NOT occur if the stopper is
    // on"). A world rule whose SELF becomes air is the world's abstraction of
    // matter leaving into the open. Two cases differ in a vessel:
    //  - A DRYING rule (reactions.json "drying": blood -> air, a pool soaking
    //    into the ground) never fires on anything inside a vessel, open or
    //    stoppered: a flask has no ground.
    //  - A STOPPERED vessel has no open air at all: no decay to air fires in
    //    it, of any class (smoke, a vapour, an ember) -- which is also what
    //    lets gas build pressure there. EXCEPT a fast transient (at least a
    //    tenth of the chance scale a tick, a life of a few ticks: spark,
    //    glare), which is energy, not matter, and burns out where it is.
    //    A self that meets AIR and becomes air is the same abstraction: a
    //    sealed vessel's inside pixels are not open air (the pair scan below
    //    skips an air neighbour for such a rule).
    // In an OPEN vessel every other decay to air fires as in the world:
    // smoke fades, ether vapour disperses. A decay into MATTER (ether ->
    // ether vapour, steam -> water) is chemistry and fires anywhere; so do
    // pair rules that spend their self on another substance (acid on sand).
    if (r.drying && inVessel) continue;
    //  - A HEAVY gas (ether vapour, chlorine, choke damp) inside a vessel,
    //    open or not, is NOT in the open: it lies in the bottle, below the
    //    lip, and fills it (owner, 2026-09-27: "the gas is heavier than air
    //    and should build up"). Its dispersal waits until it has spilled
    //    over the lip; out there it sinks, and fades or vents to the world.
    if (inVessel && selfType == NbGas && subs_[slot].heavy && r.kind == kChemDecay &&
        r.prodSelf == kChemAir && r.chance * 10u < chem_.chanceDen)
      continue;
    if (sealed && r.kind == kChemDecay && r.prodSelf == kChemAir && r.chance * 10u < chem_.chanceDen)
      continue;
    //  - A GAS OUT OF EVERY VESSEL is in the room: it lingers, thins and
    //    VENTS into the world (StepGas), where the world's own rule disperses
    //    it. Its slow dispersal here too deleted what the world should get --
    //    fumes born under acid (noxious gas fades in ~2 s) crossed the mouth
    //    and were gone before a single eighth reached the world
    //    (alchemy-react vented 0).
    if (!inVessel && selfType == NbGas && r.kind == kChemDecay && r.prodSelf == kChemAir &&
        r.chance * 10u < chem_.chanceDen)
      continue;
    if (r.kind == kChemDecay) {
      uint32_t chance = r.chance;
      if (ChemScaleArmed(r.cond)) {
        uint32_t count = 0;
        for (int k = 0; k < nn && count < 6; k++)
          count += matches(r, nb[k]) != ChemScaleInverted(r.cond);
        chance = ChemScaledChance(r.chance, r.cond, count, chem_.chanceDen);
      }
      // A HEATED VESSEL IS A HOT SURROUNDING. A cooling rule (a decay that
      // rises with the neighbours NOT hot: molten salt freezing from its skin
      // inward) reads only touching matter, and over the burner only what
      // touches the glass sees the heat -- molten salt floated up off the
      // hot bottom into the salt and air above and froze again, so a flask of
      // salt on the flame held ~7 of 60 molten forever. The glass heats all
      // it holds: the cooling runs at (1 - heat), none while the burner is
      // full on, and the melt sets as the glass cools after.
      double cool = 1.0;
      if (inVessel && ChemScaleArmed(r.cond) && ChemScaleInverted(r.cond) && chem_.heat.on &&
          ChemNbrMatches(r, chem_.heat.mat, chem_.heat.tags, chem_.heat.klass, false))
        cool = 1.0 - std::clamp((double)vessels_[hv].heat, 0.0, 1.0);
      if (!chance || Rand01() * den >= chance * scale * cool) continue;
      RaiseEvents(hv, at, subs_[slot].mat, 0, r);
      // EVAPORATION (a liquid decaying into a gas by a rule scaled by its
      // AIR neighbours: ether -> ether vapour) must happen at the surface.
      // Counted, and whether liquid lay right above it, for the gates.
      if (selfType == NbParticle && r.prodSelf >= 0 && subs_[r.prodSelf].gas && ChemScaleArmed(r.cond) &&
          r.nbrMat == 0) {
        evapFires_++;
        const V2 p = px_[selfIdx];
        for (int dy = 1; dy <= 2; dy++)
          if (NearestParticle(p.x, p.y + dy * spacing_ * 0.9f, spacing_ * 0.45f) >= 0) { evapBuried_++; break; }
      }
      ConvertEnt(selfType, selfIdx, UnitsOf(selfType, selfIdx), r.prodSelf, hv);
      firedThisStep_++;
      firedTotal_++;
      return true;
    }
    if (!nn) continue;
    const int rot = (int)(Rand() % (uint32_t)nn);
    if (r.kind == kChemEmit) {
      int hit = -1;
      for (int k = 0; k < nn; k++) {
        const ChemNb& n = nb[(k + rot) % nn];
        if (n.type != NbAir || (n.dir & r.dirs) == 0) continue;
        hit = (k + rot) % nn;
        break;
      }
      if (hit < 0) continue;
      if (Rand01() * den >= r.chance * scale) continue;
      const int q = std::min(matterOf(FineOf(selfType, selfIdx)), cfg_.unitsPerParticle);
      if (r.prodNbr >= 0) {
        produced_[r.prodNbr] += q;
        Deposit(r.prodNbr, (uint32_t)q, nb[hit].at, hv);
      }
      RaiseEvents(hv, at, subs_[slot].mat, 0, r);
      if (r.prodSelf != kChemKeep) ConvertEnt(selfType, selfIdx, UnitsOf(selfType, selfIdx), r.prodSelf, hv);
      firedThisStep_++;
      firedTotal_++;
      return true;
    }
    // PAIR: the first matching neighbour of a rotated scan, one roll.
    int hit = -1;
    for (int k = 0; k < nn; k++) {
      const ChemNb& n = nb[(k + rot) % nn];
      if ((n.dir & r.dirs) == 0 || !matches(r, n)) continue;
      if (sealed && n.type == NbAir && r.prodSelf == kChemAir) continue;
      // A pair that would rewrite the neighbour into what it already is and
      // leave self alone is not a match (sim_step.wgsl, the grass-on-soil case).
      if (r.prodSelf == kChemKeep && n.slot >= 0 && r.prodNbr == n.slot) continue;
      hit = (k + rot) % nn;
      break;
    }
    if (hit < 0) continue;
    const ChemNb n = nb[hit];
    // Through the glass, heat is as strong as the glass is hot.
    const double factor = n.type == NbHeat ? std::max(0.0f, vessels_[n.idx].heat) : 1.0;
    if (Rand01() * den >= r.chance * scale * factor) continue;
    const bool virt = n.type == NbAir || n.type == NbHeat || n.type == NbSpark;
    // THE QUANTUM: the smaller side, in GAS units (the finest there is), so
    // a pixel of thin gas meets a particle as the little matter it is. Each
    // side converts that much in its own units -- a gas pixel exactly, a
    // particle or grain the whole matter units it takes (at least one).
    const int64_t fS = FineOf(selfType, selfIdx);
    const int64_t fN = virt ? INT64_MAX : FineOf(n.type, n.idx);
    const int64_t qf = std::max<int64_t>(1, std::min(fS, fN));
    auto sideQ = [&](uint8_t type, int idx) {
      if (type == NbGas) return (int)std::min<int64_t>(qf, gasAmt_[idx]);
      return std::min(matterOf(qf), UnitsOf(type, idx));
    };
    const uint16_t nm = nbMat(n);
    RaiseEvents(hv, at, subs_[slot].mat, nm, r);
    if (r.prodNbr != kChemKeep) {
      if (!virt) {
        ConvertEnt(n.type, n.idx, sideQ(n.type, n.idx), r.prodNbr, hv);
      } else if (r.prodNbr >= 0) {
        // A virtual neighbour's product is MATERIALISED where it touched
        // (the world's spark voxel becomes chlorine; ours leaves chlorine).
        const int q = matterOf(qf);
        produced_[r.prodNbr] += q;
        Deposit(r.prodNbr, (uint32_t)q, n.at, hv);
      }
    }
    if (r.prodSelf != kChemKeep) ConvertEnt(selfType, selfIdx, sideQ(selfType, selfIdx), r.prodSelf, hv);
    firedThisStep_++;
    firedTotal_++;
    return true;
  }
  return false;
}

// ---- one chemistry step ------------------------------------------------------

void FlaskSim::StepChemistry() {
  UpdateDevices();
  chemStep_++;
  if (!chemOn_ || chemPaused_) {
    FlushPools();
    CompactDead();
    if (needPartition_) { needPartition_ = false; Partition(); }
    return;
  }
  firedThisStep_ = 0;
  const double scale = cfg_.chemRate * cfg_.chemEvery / 8.0;
  const int W = cfg_.gridW;
  BucketChem();
  // Room left for vapour, per vessel: its gas against its free inside
  // (Pressure, without the heat's push), so "full" is "one atmosphere of
  // gas", whatever pockets the gas has not reached -- a count of air pixels
  // let a flask whose shoulder still held a pocket evaporate on to 2.3 atm.
  vesselAir_.assign(vessels_.size(), 0);
  for (size_t vi = 0; vi < vessels_.size(); vi++) {
    if (!VesselAlive((int)vi)) continue;
    const float atm = Pressure((int)vi) / (1.0f + 3.0f * vessels_[vi].heat);
    vesselAir_[vi] = (int)std::lround(atm * 1000.0f);   // per-mille of an atmosphere
  }
  grainDead_.assign(grains_.size(), 0);

  // WHAT COULD REACT at all, this step: a slot present with a rule whose
  // neighbour predicate some present slot, the air, or a live virtual
  // neighbour satisfies (or that needs none). Everything else is skipped
  // without gathering a neighbour, which is what keeps a table of water and
  // sand costing nothing here.
  std::fill(present_.begin(), present_.end(), 0);
  for (size_t i = 0; i < px_.size(); i++) present_[psub_[i]] = 1;
  for (const Grain& g : grains_) present_[g.sub] = 1;
  for (int k : gasList_)
    if (gasAmt_[k]) present_[gasSub_[k]] = 1;
  bool anyHeat = false, anyShock = false;
  for (const Vessel& v : vessels_)
    if (!v.outline.empty()) { anyHeat |= v.heat > 0.02f; anyShock |= v.shock > 0; }
  bool anyActive = false;
  for (size_t s = 0; s < subs_.size(); s++) {
    activeSlot_[s] = 0;
    if (!present_[s]) continue;
    for (const ChemRule& r : chem_.rules[s]) {
      if (!ChemGateOpen(r.cond, chem_.daylight)) continue;
      bool can = r.kind != kChemPair;
      if (!can && r.nbrMat == 0) can = true;
      if (!can && anyHeat && chem_.heat.on)
        can = ChemNbrMatches(r, chem_.heat.mat, chem_.heat.tags, chem_.heat.klass, false);
      if (!can && anyShock && chem_.spark.on)
        can = ChemNbrMatches(r, chem_.spark.mat, chem_.spark.tags, chem_.spark.klass, false);
      for (size_t t = 0; t < subs_.size() && !can; t++)
        can = present_[t] && ChemNbrMatches(r, subs_[t].mat, subs_[t].tagMask, subs_[t].klass, false);
      if (!can) continue;
      // Bit 1: it can fire somewhere. Bit 2: it can fire inside a STOPPERED
      // vessel too -- a slow decay to air or a drying rule cannot (TryRules),
      // and a flask full of vapour whose only rule is its dispersal need not
      // gather a neighbour per pixel per step to find that out.
      // Bit 4: it can fire inside an OPEN vessel (a drying rule cannot).
      activeSlot_[s] |= 1;
      // (Nor can a heavy gas's slow dispersal: TryRules keeps it in the bottle.)
      const bool heavyStays = subs_[s].gas && subs_[s].heavy && r.kind == kChemDecay && r.prodSelf == kChemAir &&
                              r.chance * 10u < chem_.chanceDen;
      if (!r.drying && !heavyStays) activeSlot_[s] |= 4;
      const bool blockedSealed =
          r.drying || (r.kind == kChemDecay && r.prodSelf == kChemAir && r.chance * 10u < chem_.chanceDen);
      if (!blockedSealed) { activeSlot_[s] |= 2 | 4; break; }
    }
    anyActive |= activeSlot_[s] != 0;
  }
  const bool anySolute = !chem_.solutes.empty();
  // Can a rule of slot s fire where vessel hv (-1: none) is? (activeSlot_'s bits.)
  auto canFire = [&](int s, int hv) {
    const uint8_t a = activeSlot_[s];
    if (hv < 0) return a != 0;
    return (a & (vessels_[hv].stoppered ? 2 : 4)) != 0;
  };

  // PARTICLES: their rules, then what is dissolved in them.
  const int n0 = (int)px_.size();
  for (int i = 0; i < n0 && firedThisStep_ < cfg_.chemMaxFires; i++) {
    if (!pw_[i]) continue;
    const int s = psub_[i];
    const bool sol = anySolute && psol_[i] != 0;
    if (!activeSlot_[s] && !sol) continue;
    int hv = phome_[i];
    if (hv >= (int)vessels_.size() || (hv >= 0 && vessels_[hv].outline.empty())) hv = -1;
    const bool fire = canFire(s, hv);
    if (!fire && !sol) continue;
    GatherParticleNbrs(i, hv, nbScratch_);
    if (fire && TryRules(NbParticle, i, s, nbScratch_, hv, px_[i], scale)) continue;
    if (!sol || !pw_[i]) continue;
    const ChemSolute* sp = chem_.Species(psol_[i]);
    if (!sp) continue;
    // CONVERTS: concentrated enough, the solvent BECOMES something else and
    // the solute is spent in it (fairy dust in water: enchanted water).
    const uint32_t c = (uint32_t)pmass_[i] * sp->yieldPerVoxel / std::max<uint32_t>(1, pw_[i]);
    bool conv = false;
    for (const ChemSolute::Convert& cv : sp->converts) {
      if (cv.solvent != s || cv.into < 0 || c < cv.cMin) continue;
      consumed_[s] += pw_[i];
      produced_[cv.into] += pw_[i];
      consumed_[sp->from] += pmass_[i];
      psub_[i] = (uint8_t)cv.into;
      pmass_[i] = 0;
      psol_[i] = 0;
      if (hv >= 0 && vessels_[hv].asleep) { vessels_[hv].asleep = false; needPartition_ = true; }
      conv = true;
      break;
    }
    if (conv) continue;
    // DIFFUSION: one unit toward a less concentrated neighbour of a solvent,
    // at the species' own rate.
    if ((double)(Rand() & 255) >= sp->diffusivity * scale * 4.0) continue;
    int cand[10], nc = 0;
    for (const ChemNb& nb : nbScratch_)
      if (nb.type == NbParticle && pw_[nb.idx] && sp->DissolvesIn(psub_[nb.idx]) &&
          (psol_[nb.idx] == 0 || psol_[nb.idx] == sp->species) && nc < 10)
        cand[nc++] = nb.idx;
    if (!nc) continue;
    const int j = cand[Rand() % (uint32_t)nc];
    const uint32_t capJ = sp->saturation * pw_[j] / std::max(1u, sp->yieldPerVoxel);
    if (pmass_[j] + 1u > capJ) continue;
    if ((uint32_t)(pmass_[i] - 1) * pw_[j] < (uint32_t)(pmass_[j] + 1) * pw_[i]) continue;
    pmass_[i]--;
    pmass_[j]++;
    psol_[j] = sp->species;
    if (!pmass_[i]) psol_[i] = 0;
  }

  // GRAINS: dissolving first (a species' powder touching its solvent), then
  // their rules.
  const int g0 = (int)grains_.size();
  for (int gi = 0; gi < g0 && firedThisStep_ < cfg_.chemMaxFires; gi++) {
    if (grainDead_[gi]) continue;
    Grain& g = grains_[gi];
    if (g.chem == (uint8_t)chemStep_) continue;
    const int s = g.sub;
    const ChemSolute* sp = anySolute ? chem_.SoluteFrom(s) : nullptr;
    if (!activeSlot_[s] && !sp) continue;
    const size_t k = (size_t)g.y * W + g.x;
    int hv = wall_[k] ? g.home : (int)inside_[k] - 1;
    if (hv >= (int)vessels_.size() || (hv >= 0 && vessels_[hv].outline.empty())) hv = -1;
    GatherPixelNbrs(g.x, g.y, hv, false, nbScratch_);
    if (sp) {
      bool gone = false;
      for (const ChemNb& nb : nbScratch_) {
        if (nb.type != NbParticle || !sp->DissolvesIn(psub_[nb.idx])) continue;
        const int j = nb.idx;
        if (psol_[j] && psol_[j] != sp->species) continue;
        const uint32_t capJ = sp->saturation * pw_[j] / std::max(1u, sp->yieldPerVoxel);
        if (pmass_[j] + 1u > capJ) continue;
        if (Rand01() * 1000.0 >= sp->dissolveChance * scale) break;   // one roll a step
        TakeFromEnt(NbGrain, gi, 1, hv);
        pmass_[j]++;
        psol_[j] = sp->species;
        if (hv >= 0 && vessels_[hv].asleep) { vessels_[hv].asleep = false; needPartition_ = true; }
        gone = true;
        break;
      }
      if (gone) continue;
    }
    if (canFire(s, hv)) TryRules(NbGrain, gi, s, nbScratch_, hv, {g.x + 0.5f, g.y + 0.5f}, scale);
  }

  // GAS: its rules (smoke fades, a spark dies, hydrogen meets the flame).
  if (anyActive) {
    const size_t nG = gasList_.size();
    for (size_t a = 0; a < nG && firedThisStep_ < cfg_.chemMaxFires; a++) {
      const int k = gasList_[a];
      if (!gasAmt_[k]) continue;
      const int s = gasSub_[k];
      if (!activeSlot_[s]) continue;
      const int x = k % W, y = k / W;
      int hv = (int)inside_[k] - 1;
      if (hv >= (int)vessels_.size() || (hv >= 0 && vessels_[hv].outline.empty())) hv = -1;
      if (!canFire(s, hv)) continue;
      GatherPixelNbrs(x, y, hv, true, nbScratch_);
      TryRules(NbGas, k, s, nbScratch_, hv, {x + 0.5f, y + 0.5f}, scale);
    }
  }

  FlushPools();
  CompactDead();
  CheckPressure();
  if (needPartition_) {
    needPartition_ = false;
    Partition();
  }
}

void FlaskSim::CompactDead() {
  const int W = cfg_.gridW;
  bool anyG = false;
  for (size_t i = 0; i < grainDead_.size() && i < grains_.size(); i++) anyG |= grainDead_[i] != 0;
  if (anyG) {
    size_t w = 0;
    for (size_t i = 0; i < grains_.size(); i++) {
      if (i < grainDead_.size() && grainDead_[i]) continue;
      grains_[w] = grains_[i];
      grid_[(size_t)grains_[w].y * W + grains_[w].x] = (int)w + 1;
      w++;
    }
    grains_.resize(w);
  }
  grainDead_.assign(grains_.size(), 0);
  bool anyP = false;
  for (uint16_t w : pw_) anyP |= w == 0;
  if (anyP) {
    size_t w = 0;
    int act = 0;
    for (size_t i = 0; i < px_.size(); i++) {
      if (!pw_[i]) continue;
      if ((int)i < nAct_) act++;
      if (w != i) {
        px_[w] = px_[i]; pv_[w] = pv_[i]; pprev_[w] = pprev_[i];
        psub_[w] = psub_[i]; pw_[w] = pw_[i]; mbar_[w] = mbar_[i]; calm_[w] = calm_[i];
        phome_[w] = phome_[i]; psrc_[w] = psrc_[i]; pvar_[w] = pvar_[i]; panc_[w] = panc_[i]; pheat_[w] = pheat_[i];
        psol_[w] = psol_[i]; pmass_[w] = pmass_[i];
      }
      w++;
    }
    nAct_ = act;
    px_.resize(w); pv_.resize(w); pprev_.resize(w); psub_.resize(w); pw_.resize(w); mbar_.resize(w);
    calm_.resize(w); phome_.resize(w); psrc_.resize(w); pvar_.resize(w); panc_.resize(w); pheat_.resize(w);
    psol_.resize(w); pmass_.resize(w);
    needPartition_ = true;
  }
}

// ---- the gas phase -----------------------------------------------------------
//
// THE GAS IS A FLUID (2026-09-27; owner: "look up gas simulations ... highly
// performant, look great, and behave consistently"). It replaced a pixel CA
// whose every behaviour was a special case -- a heavy wisp's hop walk, a
// dense pixel's push to the top of its pool, a brim rule, an even-out rule --
// and still left gas stacked, blinking out, or sliding for ever.
//
// Now: one velocity field for the AIR round the gas (a MAC grid of kGasCell
// px cells, Stam 1999 / Bridson's notes / Lague's "Simulating Smoke"), and
// the gas carried in it as integer units per pixel.
//  - BUOYANCY: a cell's gas weighs on its air -- a heavy vapour (ether,
//    chlorine) pulls it down, a light one (steam, hydrogen, smoke) lifts it,
//    hot glass lifts what is near it. Stratification, pooling, pouring over a
//    lip and down the glass, a plume -- all of it is this one force.
//  - EXPANSION: a pixel holding more than its natural volume (SimConfig::
//    gasRest units) pushes its excess out: a divergence source in the
//    pressure solve. Fresh vapour displaces the air above it out of the
//    mouth and then follows it; in a STOPPERED flask the source is balanced
//    over the closed air (its mean taken out), so the gas evens out through
//    the bottle and the count -- Pressure -- rises instead.
//  - PRESSURE PROJECTION: red-black SOR, glass / grains / liquid as walls
//    that move with what they are (a swung flask's glass pushes the smoke
//    outside it; sloshing liquid pushes the vapour over it). Inside a vessel
//    the air is simulated in the VESSEL'S FRAME (CarryGas moves the gas
//    rigidly), with the vessel's acceleration as a tilt of the gravity its
//    gas feels (only a DIFFERENCE of density moves anything in a carried
//    box, as a helium balloon in a braking car goes forward): shaken, the
//    vapour sloshes like the liquid does.
//  - VORTICITY CONFINEMENT (Fedkiw, Stam, Jensen 2001) keeps the curls the
//    coarse grid would otherwise smear out.
//  - TRANSPORT: each gas pixel sends units through its four faces by the
//    face velocity (upwind) plus a little diffusion, stochastically rounded:
//    integers in, integers out, AuditUnits exact.
// Liquid is a wall to gas, except that gas IN liquid is a bubble and rises
// through it whole. Outside every vessel gas lingers gasVentSteps, then thins
// into the room (it is in the world: through the bank into the spill, which
// the bench streams out at the lip of the flask in the hand).

namespace {
// How a gas's weight drives its air, x SimConfig::gasBuoyancy: + rises.
// The world's integer gas density is the ORDER (1 hydrogen .. 6 chlorine);
// a HEAVY gas (materials.h kMatFlagHeavyGas) sinks, harder the denser.
inline float GasLift(const Substance& S) {
  if (S.heavy) return -std::clamp(0.6f + 0.25f * (float)(S.density - 4), 0.3f, 1.5f);
  return std::clamp((4.5f - (float)S.density) / 3.5f, 0.15f, 1.2f);
}
}  // namespace

void FlaskSim::EnsureGasFlow() {
  const int W = cfg_.gridW, H = cfg_.gridH, C = kGasCell;
  const int cw = (W + C - 1) / C, ch = (H + C - 1) / C;
  if (cw == gcW_ && ch == gcH_ && !gu_.empty()) return;
  gcW_ = cw;
  gcH_ = ch;
  const size_t n = (size_t)cw * ch;
  gu_.assign((size_t)(cw + 1) * ch, 0.0f);
  gv_.assign((size_t)cw * (ch + 1), 0.0f);
  gu2_ = gu_;
  gv2_ = gv_;
  gp_.assign(n, 0.0f);
  gRhs_.assign(n, 0.0f);

  gcSolidV_.assign(n, V2{});
  gcFrame_.assign(n, 0);
  gcDen_.assign(n, 0.0f);
  gcBuoy_.assign(n, 0.0f);
  gcComp_.assign(n, -1);
  gcCurl_.assign(n, 0.0f);
  gcBirth_.assign(n, 0.0f);
  gtW_ = (W + kTexCell - 1) / kTexCell;
  gtH_ = (H + kTexCell - 1) / kTexCell;
  gtc_.assign((size_t)gtW_ * gtH_ * 5, 0.0f);
  for (int j = 0; j < gtH_; j++)
    for (int i = 0; i < gtW_; i++) {
      float* t = &gtc_[((size_t)j * gtW_ + i) * 5];
      t[0] = t[2] = (i + 0.5f) * kTexCell;
      t[1] = t[3] = (j + 0.5f) * kTexCell;
      t[4] = Hash01(i, j, 3) * 0.2f;
    }
  liqPx_.assign((size_t)W * H, 0);
  liqCellV_.assign(n, V2{});
  liqCellN_.assign(n, 0.0f);
  gtlW_ = (cw + kGasTile - 1) / kGasTile;
  gtlH_ = (ch + kGasTile - 1) / kGasTile;
  gTileOn_.assign((size_t)gtlW_ * gtlH_, 0);
  gTileWas_.assign((size_t)gtlW_ * gtlH_, 0);
  gTileSeed_.assign((size_t)gtlW_ * gtlH_, 0);
  gbx0_ = gby0_ = 0;
  gbx1_ = gby1_ = -1;
}

float FlaskSim::SampleGu(float x, float y, const float* from) const {
  const float C = (float)kGasCell;
  float fx = std::clamp(x / C, 0.0f, (float)gcW_), fy = std::clamp(y / C - 0.5f, 0.0f, (float)(gcH_ - 1));
  const int i0 = std::min((int)fx, gcW_ - 1), j0 = std::min((int)fy, gcH_ - 2);
  const float tx = fx - i0, ty = fy - j0;
  const float* u = from ? from : gu_.data();
  const int s = gcW_ + 1;
  const float a = u[j0 * s + i0], b = u[j0 * s + i0 + 1], c = u[(j0 + 1) * s + i0], d = u[(j0 + 1) * s + i0 + 1];
  return (a + (b - a) * tx) * (1 - ty) + (c + (d - c) * tx) * ty;
}

float FlaskSim::SampleGv(float x, float y, const float* from) const {
  const float C = (float)kGasCell;
  float fx = std::clamp(x / C - 0.5f, 0.0f, (float)(gcW_ - 1)), fy = std::clamp(y / C, 0.0f, (float)gcH_);
  const int i0 = std::min((int)fx, gcW_ - 2), j0 = std::min((int)fy, gcH_ - 1);
  const float tx = fx - i0, ty = fy - j0;
  const float* v = from ? from : gv_.data();
  const int s = gcW_;
  const float a = v[j0 * s + i0], b = v[j0 * s + i0 + 1], c = v[(j0 + 1) * s + i0], d = v[(j0 + 1) * s + i0 + 1];
  return (a + (b - a) * tx) * (1 - ty) + (c + (d - c) * tx) * ty;
}

// THE ROOM'S DRAUGHT (SimConfig::gasWind): a stream function, so the breeze
// is divergence-free before the solve sees it -- a drifting wind along the
// bench (stronger higher up, a little shear) plus a lattice of slow eddies
// that wander with it. Every phase is a function of the step count: the
// same bench does the same thing.
//   psi = U0(t) (0.7 y + 0.15 y^2 / H) + 0.6 A / k sin(k x + a(t)) sin(k y + b(t))
//   u = dpsi/dy = U0 (0.7 + 0.3 y / H) + 0.6 A sin(k x + a) cos(k y + b)
//   v = -dpsi/dx = -0.6 A cos(k x + a) sin(k y + b)
// U0 wanders between -A and A over ~20 s (a draught that comes and goes and
// sometimes turns round); the eddies drift over ~10 s.
namespace {
constexpr float kWindEddy = 96.0f;   // px, eddy wavelength
constexpr float kTwoPi = 6.2831853f;
constexpr float kBenchStepsPerSec = 240.0f;   // AlchemyBench: Step(4) at 60 Hz
constexpr float kMouthAbove = 4.0f;           // px over the lip the mouth's draw still reaches
constexpr float kMouthMaxDrift = 0.45f;       // px per gas step, the mouth drift at most (half the transport CFL)
}  // namespace
void FlaskSim::SetWindPhase() {
  const float A = cfg_.gasWind, t = (float)step_ / kBenchStepsPerSec;
  windU0_ = A * (0.65f * std::sin(kTwoPi * t / 19.0f + 0.4f) + 0.35f * std::sin(kTwoPi * t / 6.7f + 1.3f));
  windPhX_ = std::fmod(kTwoPi * t / 11.0f, kTwoPi);
  windPhY_ = std::fmod(kTwoPi * t / 13.0f + 0.9f, kTwoPi);
}
V2 FlaskSim::WindAt(V2 p) const {
  const float A = cfg_.gasWind, k = kTwoPi / kWindEddy;
  const float sx = std::sin(k * p.x + windPhX_), cx = std::cos(k * p.x + windPhX_);
  const float sy = std::sin(k * p.y + windPhY_), cy = std::cos(k * p.y + windPhY_);
  return {windU0_ * (0.7f + 0.3f * p.y / (float)std::max(1, cfg_.gridH)) + 0.6f * A * sx * cy, -0.6f * A * cx * sy};
}

void FlaskSim::StepGasFlow(int bx0, int by0, int bx1, int by1) {
  const int W = cfg_.gridW, H = cfg_.gridH, C = kGasCell, R = GasR();
  const int cw = gcW_;
  const float E = (float)std::max(1, cfg_.gasEvery);   // substeps a gas step
  const int U1 = cw + 1;   // gu_ row stride (gv_'s is cw)
  // THE BOX, padded by one cell all round: 0 air, 1 solid, 2 open (outside
  // the box -- still air at zero pressure -- or off the grid's top and
  // sides); below the grid is the table, solid.
  const int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1, LW = bw + 2;
  const size_t LN = (size_t)LW * (bh + 2);
  std::vector<uint8_t>& lf = gLf_;
  lf.assign(LN, 2);
  if (by0 == 0)
    for (int ii = 0; ii < LW; ii++) lf[ii] = 1;
  auto L = [&](int i, int j) { return (j - by0 + 1) * LW + (i - bx0 + 1); };
  auto cell = [cw](int i, int j) { return j * cw + i; };
  const int px0 = bx0 * C, py0 = by0 * C;
  const int px1 = std::min(W - 1, (bx1 + 1) * C - 1), py1 = std::min(H - 1, (by1 + 1) * C - 1);

  // A vessel's own velocity at a world point, px per gas step.
  auto vesselVel = [&](int vi, V2 p) -> V2 {
    const Vessel& v = vessels_[vi];
    return {(v.vel.x - v.angVel * (p.y - v.x.pos.y)) * E, (v.vel.y + v.angVel * (p.x - v.x.pos.x)) * E};
  };
  auto frameVel = [&](int frame, V2 p) -> V2 {
    if (frame <= 0 || frame > (int)vessels_.size() || vessels_[frame - 1].outline.empty()) return {};
    return vesselVel(frame - 1, p);
  };
  // Each vessel's acceleration this step, as a fraction of gravity (the
  // liquid's, px/substep^2, in px/gas step^2): in its frame gravity is
  // tilted by it, and a cell's buoyancy -- its gas's weight against the air
  // -- acts along the tilted gravity. A uniform push on ALL the air in a
  // flask is not physics: it pumped the air in and out of the mouth.
  if (gasPrevVel_.size() != vessels_.size()) gasPrevVel_.resize(vessels_.size(), V2{});
  V2 felt[256] = {};
  const float gEff = std::max(1e-4f, cfg_.gravity * E * E);
  for (size_t vi = 0; vi < vessels_.size() && vi < 255; vi++) {
    const V2 dv = vessels_[vi].vel - gasPrevVel_[vi];
    gasPrevVel_[vi] = vessels_[vi].vel;
    if (!vessels_[vi].outline.empty()) felt[vi + 1] = dv * (E / gEff);
  }

  // 0. A MOVED VESSEL CARRIES ITS AIR, as CarryGas carries its gas: the
  // velocity at each face inside it now is the velocity (in its frame,
  // turned with it) that was at the same point of the vessel at the last
  // gas step. Without it a carried flask's inside inherited the world air
  // it swept into -- a swing moves it several cells a step -- and that air,
  // pushed about by the glass, blew the vapour out of the neck.
  if (gasPrevPose_.size() != vessels_.size()) gasPrevPose_.resize(vessels_.size());
  {
    bool copied = false;
    for (size_t vi = 0; vi < vessels_.size(); vi++) {
      const Vessel& v = vessels_[vi];
      const Xform old = gasPrevPose_[vi];
      gasPrevPose_[vi] = v.x;
      if (v.outline.empty()) continue;
      if (old.pos.x == v.x.pos.x && old.pos.y == v.x.pos.y && old.angle == v.x.angle) continue;
      if (!copied) { gu2_ = gu_; gv2_ = gv_; copied = true; }
      float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
      for (V2 o : v.outline) {
        const V2 w = ToWorld(v.x, o);
        x0 = std::min(x0, w.x); x1 = std::max(x1, w.x); y0 = std::min(y0, w.y); y1 = std::max(y1, w.y);
      }
      const float da = v.x.angle - old.angle, ca = std::cos(da), sa = std::sin(da);
      const int i0 = std::max(0, (int)(x0 / C) - 1), i1 = std::min(cw, (int)(x1 / C) + 2);
      const int j0 = std::max(0, (int)(y0 / C) - 1), j1 = std::min(gcH_, (int)(y1 / C) + 2);
      auto carried = [&](V2 p, V2& vel) {
        const V2 l = ToLocal(v.x, p);
        if (!InsideLocal(v, l)) return false;
        const V2 q = ToWorld(old, l);
        const V2 o{SampleGu(q.x, q.y, gu2_.data()), SampleGv(q.x, q.y, gv2_.data())};
        vel = {o.x * ca - o.y * sa, o.x * sa + o.y * ca};
        return true;
      };
      V2 vel;
      for (int j = j0; j < std::min(j1, gcH_); j++)
        for (int i = i0; i <= i1; i++)
          if (carried({(float)(i * C), (j + 0.5f) * C}, vel)) gu_[(size_t)j * U1 + i] = vel.x;
      for (int j = j0; j <= j1; j++)
        for (int i = i0; i < std::min(i1, cw); i++)
          if (carried({(i + 0.5f) * C, (float)(j * C)}, vel)) gv_[(size_t)j * cw + i] = vel.y;
    }
  }

  // 1. THE LIQUID: the pixels a particle holds (NearestParticle's 0.6
  // spacing, what the gas has always read as liquid), and per cell the mean
  // velocity of the awake particles in it.
  for (int y = py0; y <= py1; y++) std::memset(&liqPx_[(size_t)y * W + px0], 0, (size_t)(px1 - px0 + 1));
  for (int j = by0; j <= by1; j++)
    for (int i = bx0; i <= bx1; i++) {
      liqCellV_[cell(i, j)] = V2{};
      liqCellN_[cell(i, j)] = 0;
    }
  {
    const float r = spacing_ * 0.6f, r2 = r * r;
    for (size_t pi = 0; pi < px_.size(); pi++) {
      if (!pw_[pi]) continue;
      const V2 p = px_[pi];
      if (p.x < px0 - 2 || p.x > px1 + 3 || p.y < py0 - 2 || p.y > py1 + 3) continue;
      for (int yy = std::max(py0, (int)std::floor(p.y - r)); yy <= std::min(py1, (int)std::floor(p.y + r)); yy++)
        for (int xx = std::max(px0, (int)std::floor(p.x - r)); xx <= std::min(px1, (int)std::floor(p.x + r)); xx++) {
          const float dx = xx + 0.5f - p.x, dy = yy + 0.5f - p.y;
          if (dx * dx + dy * dy < r2) liqPx_[(size_t)yy * W + xx] = 1;
        }
      const int ci = (int)std::floor(p.x) / C, cj = (int)std::floor(p.y) / C;
      if ((int)pi < nAct_ && p.x >= 0 && p.y >= 0 && ci >= bx0 && ci <= bx1 && cj >= by0 && cj <= by1) {
        liqCellV_[cell(ci, cj)] = liqCellV_[cell(ci, cj)] + pv_[pi];
        liqCellN_[cell(ci, cj)] += 1.0f;
      }
    }
  }

  // 2. THE CELLS: solid (glass, grain or liquid holds 3 of its 4 pixels) or
  // air; the air's frame (the vessel it is inside); the gas in it.
  // A cell with any GLASS in it is glass: at 2 px a cell, a majority rule
  // let a diagonal wall through as a chequer of air cells, and the solve
  // breathed across the glass.
  const float buoyK = cfg_.gasBuoyancy / (float)(C * C * R);
  const int T = kGasTile;
  for (int j = by0; j <= by1; j++)
    for (int i = bx0; i <= bx1; i++) {
      if (!gTileOn_[(size_t)(j / T) * gtlW_ + i / T]) continue;   // open air (lf 2)
      int solid = 0, freePx = 0, wallV = -1;
      bool liq = false;
      uint32_t gas = 0;
      float lift = 0;
      uint8_t frame = 0, anyFrame = 0;
      for (int dy = 0; dy < C; dy++)
        for (int dx = 0; dx < C; dx++) {
          const int x = i * C + dx, y = j * C + dy;
          if (x >= W || y >= H) { solid++; continue; }
          const size_t k = (size_t)y * W + x;
          if (!anyFrame) anyFrame = inside_[k];
          if (wall_[k]) { solid++; wallV = wall_[k] - 1; continue; }
          if (grid_[k]) { solid++; continue; }
          if (liqPx_[k]) { solid++; liq = true; continue; }
          freePx++;
          if (!frame) frame = inside_[k];
          if (gasAmt_[k]) {
            gas += gasAmt_[k];
            lift += (float)gasAmt_[k] * GasLift(subs_[gasSub_[k]]);
          }
        }
      const int c = cell(i, j);
      const V2 ctr{(i + 0.5f) * C, (j + 0.5f) * C};
      gcFrame_[c] = frame ? frame : anyFrame;
      gcDen_[c] = freePx ? (float)gas / (float)(freePx * R) : 0.0f;
      float b = lift * buoyK;
      if (wallV >= 0 || solid * 4 >= 3 * C * C) {
        lf[L(i, j)] = 1;
        V2 sv{};
        if (wallV >= 0 && wallV < (int)vessels_.size() && !vessels_[wallV].outline.empty()) sv = vesselVel(wallV, ctr);
        // Liquid in a vessel is a wall AT REST in the vessel's frame: its
        // slosh, rasterized into cell faces, does not add up to zero flux,
        // and the difference pumped air -- and vapour -- out of the mouth.
        // In the open (a pour falling through the air) it drags the air.
        else if (liq && anyFrame) sv = frameVel(anyFrame, ctr);
        else if (liq && liqCellN_[c] > 0) sv = liqCellV_[c] * (E / liqCellN_[c]);
        gcSolidV_[c] = sv;
        gp_[c] = 0;
      } else {
        lf[L(i, j)] = 0;
        // Hot glass heats the air against it, which rises.
        const int f = gcFrame_[c] - 1;
        if (f >= 0 && f < (int)vessels_.size() && vessels_[f].heat > 0.02f && !vessels_[f].outline.empty() &&
            vessels_[f].Near(ToLocal(vessels_[f].x, ctr)) == Vessel::kNear)
          b += cfg_.gasHeatLift * vessels_[f].heat;
      }
      gcBuoy_[c] = b;
    }

  // 3. FORCES, on faces between two air cells: buoyancy (vertical faces),
  // the frame's felt acceleration, vorticity confinement; then damping.
  float* gu = gu_.data();
  float* gv = gv_.data();
  auto uc = [&](int i, int j) { return 0.5f * (gu[j * U1 + i] + gu[j * U1 + i + 1]); };
  auto vc = [&](int i, int j) { return 0.5f * (gv[j * cw + i] + gv[(j + 1) * cw + i]); };
  std::vector<V2>& conf = gConf_;
  conf.assign((size_t)bw * bh, V2{});
  if (cfg_.gasVorticity > 0) {
    // The curl at each air cell (centred differences of face velocities
    // averaged to the centres; a solid or open neighbour mirrors its own).
    std::vector<float>& curl = gcCurl_;
    for (int j = by0; j <= by1; j++)
      for (int i = bx0; i <= bx1; i++) {
        const int l = L(i, j), c = cell(i, j);
        if (lf[l] != 0) { curl[c] = 0; continue; }
        const float v0 = vc(i, j), u0 = uc(i, j);
        const float vr = lf[l + 1] == 0 ? vc(i + 1, j) : v0, vl = lf[l - 1] == 0 ? vc(i - 1, j) : v0;
        const float ut = lf[l + LW] == 0 ? uc(i, j + 1) : u0, ub = lf[l - LW] == 0 ? uc(i, j - 1) : u0;
        curl[c] = 0.5f * (vr - vl) - 0.5f * (ut - ub);
      }
    // The confinement force, weighted by the gas in the cell (pure air is
    // left to calm: the curls worth keeping are the ones you can see).
    for (int j = by0; j <= by1; j++)
      for (int i = bx0; i <= bx1; i++) {
        const int l = L(i, j), c = cell(i, j);
        if (lf[l] != 0) continue;
        const float w = std::min(1.0f, gcDen_[c] * 3.0f);
        if (w <= 0) continue;
        const float a0 = std::fabs(curl[c]);
        auto ac = [&](int dl, int dc) { return lf[l + dl] == 0 ? std::fabs(curl[c + dc]) : a0; };
        const float gx = 0.5f * (ac(1, 1) - ac(-1, -1)), gy = 0.5f * (ac(LW, cw) - ac(-LW, -cw));
        const float len = std::sqrt(gx * gx + gy * gy) + 1e-6f;
        const float k = cfg_.gasVorticity * w * curl[c] / len;
        conf[(size_t)(j - by0) * bw + (i - bx0)] = {k * gy, -k * gx};
      }
  }
  const float keep = 1.0f - std::clamp(cfg_.gasDamping, 0.0f, 1.0f);
  // TURBULENCE: a small random push on faces where there is gas. Real gas
  // is never laminar, and a perfectly symmetric field has no way to start
  // an exchange -- a flask held mouth-down would hold its heavy vapour over
  // the neck for ever instead of glugging it out.
  const float jitA = cfg_.gasJitter;
  auto jit = [&](int ca, int cb) {
    if (jitA <= 0) return 0.0f;
    const float w = std::min(1.0f, 2.0f * (gcDen_[ca] + gcDen_[cb]));
    return w > 0 ? jitA * w * (float)(Rand01() - 0.5) : 0.0f;
  };
  // THE DRAUGHT (SimConfig::gasWind): air outside every vessel is pulled
  // toward the room's breeze, gasWindGrip of the difference a step. Its
  // sines per column and row of faces: [sin, cos] at the face line, then
  // [sin, cos] half a cell on.
  const float windA = cfg_.gasWind, grip = std::clamp(cfg_.gasWindGrip, 0.0f, 1.0f);
  const bool windOn = windA > 0 && grip > 0;
  if (windOn) {
    // (the phases: StepGas set them)
    const float k = kTwoPi / kWindEddy;
    windCol_.resize((size_t)(bw + 1) * 4);
    windRow_.resize((size_t)(bh + 1) * 5);
    for (int i = bx0; i <= bx1 + 1; i++) {
      float* w = &windCol_[(size_t)(i - bx0) * 4];
      w[0] = std::sin(k * i * C + windPhX_); w[1] = std::cos(k * i * C + windPhX_);
      w[2] = std::sin(k * (i + 0.5f) * C + windPhX_); w[3] = std::cos(k * (i + 0.5f) * C + windPhX_);
    }
    for (int j = by0; j <= by1 + 1; j++) {
      float* w = &windRow_[(size_t)(j - by0) * 5];
      w[0] = std::sin(k * j * C + windPhY_); w[1] = std::cos(k * j * C + windPhY_);
      w[2] = std::sin(k * (j + 0.5f) * C + windPhY_); w[3] = std::cos(k * (j + 0.5f) * C + windPhY_);
      w[4] = windU0_ * (0.7f + 0.3f * (j + 0.5f) * C / (float)H);
    }
  }
  for (int j = by0; j <= by1; j++)
    for (int i = bx0 + 1; i <= bx1; i++) {
      const int c0 = j * cw;
      const int l = L(i, j);
      if (lf[l - 1] != 0 || lf[l] != 0) continue;
      float& u = gu[j * U1 + i];
      const int fa = gcFrame_[cell(i - 1, j)], fb = gcFrame_[cell(i, j)];
      if (fa && fa == fb) u += 0.5f * (gcBuoy_[cell(i - 1, j)] + gcBuoy_[cell(i, j)]) * felt[fa].x;
      if (windOn && !fa && !fb) {
        const float* wc = &windCol_[(size_t)(i - bx0) * 4];
        const float* wr = &windRow_[(size_t)(j - by0) * 5];
        u += grip * (wr[4] + 0.6f * windA * wc[0] * wr[3] - u);
      }
      const size_t q = (size_t)(j - by0) * bw + (i - bx0);
      u = (u + 0.5f * (conf[q - 1].x + conf[q].x) + jit(c0 + i - 1, c0 + i)) * keep;
    }
  for (int j = by0 + 1; j <= by1; j++)
    for (int i = bx0; i <= bx1; i++) {
      const int l = L(i, j);
      if (lf[l - LW] != 0 || lf[l] != 0) continue;
      float& v = gv[j * cw + i];
      const int fa = gcFrame_[cell(i, j - 1)], fb = gcFrame_[cell(i, j)];
      const float b = 0.5f * (gcBuoy_[cell(i, j - 1)] + gcBuoy_[cell(i, j)]);
      if (fa && fa == fb) v += b * felt[fa].y;
      if (windOn && !fa && !fb) {
        const float* wc = &windCol_[(size_t)(i - bx0) * 4];
        const float* wr = &windRow_[(size_t)(j - by0) * 5];
        v += grip * (-0.6f * windA * wc[3] * wr[0] - v);
      }
      const size_t q = (size_t)(j - by0) * bw + (i - bx0);
      v += b;
      v = (v + 0.5f * (conf[q - bw].y + conf[q].y) + jit(cell(i, j - 1), cell(i, j))) * keep;
    }

  // 4. SELF-ADVECTION (semi-Lagrangian): each face takes the velocity from
  // where its air was a step ago.
  for (int j = by0; j <= by1; j++)
    for (int i = bx0; i <= bx1 + 1; i++) {
      const int l = L(i, j);
      if (lf[l - 1] != 0 && lf[l] != 0) continue;
      const float x = (float)(i * C), y = (j + 0.5f) * C;
      const float u = gu[j * U1 + i], v = SampleGv(x, y);
      gu2_[j * U1 + i] = SampleGu(x - u, y - v);
    }
  for (int j = by0; j <= by1 + 1; j++)
    for (int i = bx0; i <= bx1; i++) {
      const int l = L(i, j);
      if (lf[l - LW] != 0 && lf[l] != 0) continue;
      const float x = (i + 0.5f) * C, y = (float)(j * C);
      const float u = SampleGu(x, y), v = gv[j * cw + i];
      gv2_[j * cw + i] = SampleGv(x - u, y - v);
    }
  for (int j = by0; j <= by1; j++)
    for (int i = bx0; i <= bx1 + 1; i++) {
      const int l = L(i, j);
      if (lf[l - 1] != 0 && lf[l] != 0) continue;
      gu[j * U1 + i] = gu2_[j * U1 + i];
    }
  for (int j = by0; j <= by1 + 1; j++)
    for (int i = bx0; i <= bx1; i++) {
      const int l = L(i, j);
      if (lf[l - LW] != 0 && lf[l] != 0) continue;
      gv[j * cw + i] = gv2_[j * cw + i];
    }

  // 5. THE WALLS: a face between air and a solid moves with the solid, as
  // seen from the air's frame; between two solids (or on the table) it is still.
  auto wallFace = [&](int ai, int aj, int bi, int bj, int la, int lb, bool horiz, float& f) {
    const bool sa = lf[la] == 1, sb = lf[lb] == 1;
    if (!sa && !sb) return;
    if (sa && sb) { f = 0; return; }
    const int si = sa ? ai : bi, sj = sa ? aj : bj, fi = sa ? bi : ai, fj = sa ? bj : aj;
    if (sj < 0) { f = 0; return; }
    V2 sv = gcSolidV_[cell(si, sj)];
    if (lf[sa ? lb : la] == 0) sv = sv - frameVel(gcFrame_[cell(fi, fj)], {(fi + 0.5f) * C, (fj + 0.5f) * C});
    f = horiz ? sv.x : sv.y;
  };
  for (int j = by0; j <= by1; j++)
    for (int i = bx0; i <= bx1 + 1; i++) {
      const int l = L(i, j);
      wallFace(i - 1, j, i, j, l - 1, l, true, gu[j * U1 + i]);
    }
  for (int j = by0; j <= by1 + 1; j++)
    for (int i = bx0; i <= bx1; i++) {
      if (j == 0) { gv[i] = 0; continue; }
      const int l = L(i, j);
      wallFace(i, j - 1, i, j, l - LW, l, false, gv[j * cw + i]);
    }

  // 6. THE DIVERGENCE TO REMOVE, less what the gas's expansion wants.
  std::vector<float>& rhs = gRhs_;
  std::vector<float>& lp = gLp_;
  rhs.assign(LN, 0.0f);
  lp.assign(LN, 0.0f);
  // EXPANSION IS BIRTH. Gas made this step (Deposit: evaporation, a
  // reaction's gas) takes gasExpand / gasRest times its matter's room, and
  // that volume is a source in the air where it was born: fresh vapour
  // pushes the air before it -- up and out of an open mouth, so a flask
  // fills from its liquid up and then overflows. Gas that is merely DENSE
  // (pooled, packed by a carried flask, the transport's small compressions)
  // drives no air: it evens out locally (StepGas's excess diffusion). As a
  // source, every one of those pumped air out of an open mouth, and a
  // half-full flask blew its vapour out of its neck.
  const float birthK = cfg_.gasExpandRate / (float)(R * C);
  // Gas is mostly BORN IN MATTER -- a reaction at the acid's bed of sand,
  // an evaporating particle at the surface -- and a solid cell is no part of
  // the solve, so its volume was never pushed anywhere and never paid: the
  // gas bubbled up into a headspace that only compressed (past one
  // atmosphere, an open flask packed like a stoppered one) while the air
  // above it never moved and nothing went out of the mouth. The volume
  // rises with its bubbles: to the first air cell above it on its own side
  // of the glass (none -- under a shoulder of glass -- and it is dropped).
  for (int j = by0; j <= by1; j++)
    for (int i = bx0; i <= bx1; i++) {
      const int c = cell(i, j);
      if (gcBirth_[c] <= 0 || lf[L(i, j)] != 1) continue;
      for (int jj = j + 1; jj <= by1; jj++) {
        const uint8_t f = lf[L(i, jj)];
        if (f == 1) continue;
        if (f == 0 && gcFrame_[cell(i, jj)] == gcFrame_[c]) gcBirth_[cell(i, jj)] += gcBirth_[c];
        break;
      }
      gcBirth_[c] = 0;
    }
  for (int j = by0; j <= by1; j++)
    for (int i = bx0; i <= bx1; i++) {
      const int l = L(i, j);
      if (lf[l] != 0) continue;
      const int c = cell(i, j);
      const float div = gu[j * U1 + i + 1] - gu[j * U1 + i] + gv[(j + 1) * cw + i] - gv[j * cw + i];
      // At most a cell's own room a step: a particle's worth of vapour
      // (tens of natural volumes in one pixel) pushes for several steps --
      // what it has not pushed yet stays owed.
      const float src = std::min(1.0f, gcBirth_[c] * birthK);
      gcBirth_[c] = std::max(0.0f, gcBirth_[c] - src / std::max(1e-6f, birthK));
      rhs[l] = div - src;
      lp[l] = gp_[c];
    }
  // A CLOSED body of air (a stoppered flask, a pocket under liquid) can only
  // redistribute: its sources' mean comes out, so the solve is consistent
  // and the gas evens out through it.
  {
    std::vector<int>& comp = gcComp_;
    comp.assign(LN, -1);
    int nComp = 0;
    const int dl[4] = {1, -1, LW, -LW};
    for (int j = by0; j <= by1; j++)
      for (int i = bx0; i <= bx1; i++) {
        const int l0 = L(i, j);
        if (lf[l0] != 0 || comp[l0] >= 0) continue;
        gcQueue_.clear();
        gcQueue_.push_back(l0);
        comp[l0] = nComp;
        bool open = false;
        double sum = 0;
        for (size_t q = 0; q < gcQueue_.size(); q++) {
          const int cc = gcQueue_[q];
          sum += rhs[cc];
          for (int d : dl) {
            const int nl = cc + d;
            if (lf[nl] == 2) { open = true; continue; }
            if (lf[nl] != 0 || comp[nl] >= 0) continue;
            comp[nl] = nComp;
            gcQueue_.push_back(nl);
          }
        }
        if (!open) {
          const float mean = (float)(sum / (double)gcQueue_.size());
          for (int cc : gcQueue_) rhs[cc] -= mean;
        }
        nComp++;
      }
  }

  // 7. PRESSURE: red-black successive over-relaxation, warm-started. Solid
  // and open cells hold p = 0 in `lp`, so a neighbour's term is just its p;
  // only the count differs (a solid neighbour is not counted: Neumann).
  {
    std::vector<int>& red = gRed_;
    std::vector<float>& inv = gInv_;
    red.clear();
    size_t nRed = 0;
    for (int color = 0; color < 2; color++) {
      for (int j = by0; j <= by1; j++)
        for (int i = bx0 + ((bx0 + j + color) & 1); i <= bx1; i += 2) {
          const int l = L(i, j);
          if (lf[l] != 0) continue;
          red.push_back(l);
        }
      if (color == 0) nRed = red.size();
    }
    inv.resize(red.size());
    for (size_t q = 0; q < red.size(); q++) {
      const int l = red[q];
      const int n = (lf[l + 1] != 1) + (lf[l - 1] != 1) + (lf[l + LW] != 1) + (lf[l - LW] != 1);
      inv[q] = n ? 1.0f / (float)n : 0.0f;
    }
    const float w = std::clamp(cfg_.gasSor, 1.0f, 1.95f);
    float* P = lp.data();
    const float* B = rhs.data();
    for (int it = 0; it < cfg_.gasPressureIters; it++)
      for (int color = 0; color < 2; color++) {
        const size_t q0 = color ? nRed : 0, q1 = color ? red.size() : nRed;
        for (size_t q = q0; q < q1; q++) {
          const int l = red[q];
          const float pn = (P[l + 1] + P[l - 1] + P[l + LW] + P[l - LW] - B[l]) * inv[q];
          P[l] += w * (pn - P[l]);
        }
      }
  }

  // 8. PROJECT: subtract the pressure gradient from every face with air on
  // at least one side and no wall on either.
  const float vmax = cfg_.gasMaxSpeed;
  for (int j = by0; j <= by1; j++)
    for (int i = bx0; i <= bx1 + 1; i++) {
      const int l = L(i, j);
      const uint8_t a = lf[l - 1], b = lf[l];
      if (a == 1 || b == 1 || (a != 0 && b != 0)) continue;
      float& u = gu[j * U1 + i];
      u = std::clamp(u - (lp[l] - lp[l - 1]), -vmax, vmax);
    }
  for (int j = by0; j <= by1 + 1; j++)
    for (int i = bx0; i <= bx1; i++) {
      if (j == 0) continue;
      const int l = L(i, j);
      const uint8_t a = lf[l - LW], b = lf[l];
      if (a == 1 || b == 1 || (a != 0 && b != 0)) continue;
      float& v = gv[j * cw + i];
      v = std::clamp(v - (lp[l] - lp[l - LW]), -vmax, vmax);
    }
  for (int j = by0; j <= by1; j++)
    for (int i = bx0; i <= bx1; i++) {
      const int l = L(i, j);
      if (lf[l] == 0) gp_[cell(i, j)] = lp[l];
    }

  // 9. THE LOOK'S TEXTURE rides the air (look only: nothing reads it back),
  // on its own coarse grid (kTexCell px). Each texel's two coordinate
  // layers are fetched from where its air came from; its phase advances
  // with how far the air moved, so a still cloud's texture is still, and a
  // layer is reset to the texel's own position when its weight is zero
  // (RenderGas crossfades them by the phase).
  {
    const int T = kTexCell, tw = gtW_;
    std::vector<float>& t = gtc_;
    std::vector<float>& old = gtcOld_;
    old.assign(t.begin(), t.end());
    const int ti0 = px0 / T, ti1 = std::min(gtW_ - 1, px1 / T), tj0 = py0 / T, tj1 = std::min(gtH_ - 1, py1 / T);
    auto fetch = [&](float x, float y, float* o) {
      const float fx = std::clamp(x / T - 0.5f, (float)ti0, (float)ti1), fy = std::clamp(y / T - 0.5f, (float)tj0, (float)tj1);
      const int i0 = std::min((int)fx, std::max(ti0, ti1 - 1)), j0 = std::min((int)fy, std::max(tj0, tj1 - 1));
      const int i1 = std::min(i0 + 1, ti1), j1 = std::min(j0 + 1, tj1);
      const float tx = fx - i0, ty = fy - j0;
      const float* a = &old[((size_t)j0 * tw + i0) * 5];
      const float* b = &old[((size_t)j0 * tw + i1) * 5];
      const float* c = &old[((size_t)j1 * tw + i0) * 5];
      const float* d = &old[((size_t)j1 * tw + i1) * 5];
      for (int q = 0; q < 4; q++)
        o[q] = (a[q] + (b[q] - a[q]) * tx) * (1 - ty) + (c[q] + (d[q] - c[q]) * tx) * ty;
      // The phase is not interpolated across a wrap: the nearest texel's.
      o[4] = (tx < 0.5f ? (ty < 0.5f ? a : c) : (ty < 0.5f ? b : d))[4];
    };
    for (int j = tj0; j <= tj1; j++)
      for (int i = ti0; i <= ti1; i++) {
        const float x = (i + 0.5f) * T, y = (j + 0.5f) * T;
        const float u = SampleGu(x, y), v = SampleGv(x, y);
        const float sp = std::sqrt(u * u + v * v);
        if (sp < 1e-3f) continue;
        float* o = &t[((size_t)j * tw + i) * 5];
        fetch(x - u, y - v, o);
        // A layer lives ~40 px of travel.
        const float ph0 = o[4];
        float ph = ph0 + sp / 40.0f;
        if (ph >= 1.0f) { ph -= 1.0f; o[0] = x; o[1] = y; }
        if (ph0 < 0.5f && ph >= 0.5f) { o[2] = x; o[3] = y; }
        o[4] = ph;
      }
  }
  (void)H;
}

void FlaskSim::StepGas() {
  const int W = cfg_.gridW, H = cfg_.gridH, C = kGasCell;
  // Drop drained pixels from the list.
  {
    size_t w = 0;
    for (int k : gasList_) {
      if (gasAmt_[k]) gasList_[w++] = k;
      else { gasListed_[k] = 0; gasSub_[k] = 0xFF; gasAge_[k] = 0; }
    }
    gasList_.resize(w);
  }
  EnsureGasFlow();
  const int T = kGasTile, TP = kGasTile * C;
  // A tile let go of is still air again (its faces and its pressure).
  auto clearTile = [&](int ti, int tj) {
    const int i1 = std::min(gcW_, (ti + 1) * T) - 1, j1 = std::min(gcH_, (tj + 1) * T) - 1;
    for (int j = tj * T; j <= j1; j++)
      for (int i = ti * T; i <= i1; i++) {
        gu_[(size_t)j * (gcW_ + 1) + i] = gu_[(size_t)j * (gcW_ + 1) + i + 1] = 0;
        gv_[(size_t)j * gcW_ + i] = gv_[(size_t)(j + 1) * gcW_ + i] = 0;
        gp_[(size_t)j * gcW_ + i] = 0;
      }
  };
  gTileWas_.swap(gTileOn_);
  std::fill(gTileOn_.begin(), gTileOn_.end(), 0);
  if (gasList_.empty()) {
    // No gas: the air comes to rest.
    for (int tj = 0; tj < gtlH_; tj++)
      for (int ti = 0; ti < gtlW_; ti++)
        if (gTileWas_[(size_t)tj * gtlW_ + ti]) clearTile(ti, tj);
    gbx1_ = gby1_ = -1;
    return;
  }
  BucketChem();
  SetWindPhase();

  // THE ACTIVE TILES: every tile with gas in it and its eight neighbours (at
  // least a tile of air round any wisp), and the whole of every vessel
  // holding some (a closed flask must be solved whole, or its air leaks out
  // of the solve's open edge). The solve's box is their bounds; a cell of
  // the box on no active tile is open air at zero pressure, as the box's
  // outside always was -- so two flasks at the ends of the bench are two
  // small solves, not one bench-wide one.
  gasHolds_.assign(vessels_.size() + 1, 0);
  std::fill(gTileSeed_.begin(), gTileSeed_.end(), 0);
  for (int k : gasList_) {
    const int x = k % W, y = k / W;
    gTileSeed_[(size_t)(y / TP) * gtlW_ + x / TP] = 1;
    if (inside_[k] && inside_[k] <= vessels_.size()) gasHolds_[inside_[k]] = 1;
  }
  for (int tj = 0; tj < gtlH_; tj++)
    for (int ti = 0; ti < gtlW_; ti++) {
      if (!gTileSeed_[(size_t)tj * gtlW_ + ti]) continue;
      for (int b = std::max(0, tj - 1); b <= std::min(gtlH_ - 1, tj + 1); b++)
        for (int a = std::max(0, ti - 1); a <= std::min(gtlW_ - 1, ti + 1); a++) gTileOn_[(size_t)b * gtlW_ + a] = 1;
    }
  for (size_t vi = 0; vi < vessels_.size(); vi++) {
    if (!gasHolds_[vi + 1] || vessels_[vi].outline.empty()) continue;
    int x0 = W, x1 = -1, y0 = H, y1 = -1;
    for (V2 o : vessels_[vi].outline) {
      const V2 w = ToWorld(vessels_[vi].x, o);
      x0 = std::min(x0, (int)std::floor(w.x) - 4); x1 = std::max(x1, (int)std::ceil(w.x) + 4);
      y0 = std::min(y0, (int)std::floor(w.y) - 4); y1 = std::max(y1, (int)std::ceil(w.y) + 8);
    }
    for (int tj = std::max(0, y0) / TP; tj <= std::min(gtlH_ - 1, std::max(0, y1) / TP); tj++)
      for (int ti = std::max(0, x0) / TP; ti <= std::min(gtlW_ - 1, std::max(0, x1) / TP); ti++)
        gTileOn_[(size_t)tj * gtlW_ + ti] = 1;
  }
  int bx0 = gcW_, bx1 = -1, by0 = gcH_, by1 = -1;
  for (int tj = 0; tj < gtlH_; tj++)
    for (int ti = 0; ti < gtlW_; ti++) {
      const size_t t = (size_t)tj * gtlW_ + ti;
      if (gTileWas_[t] && !gTileOn_[t]) clearTile(ti, tj);
      if (!gTileOn_[t]) continue;
      bx0 = std::min(bx0, ti * T); bx1 = std::max(bx1, std::min(gcW_, (ti + 1) * T) - 1);
      by0 = std::min(by0, tj * T); by1 = std::max(by1, std::min(gcH_, (tj + 1) * T) - 1);
    }
  gbx0_ = bx0; gby0_ = by0; gbx1_ = bx1; gby1_ = by1;

  StepGasFlow(bx0, by0, bx1, by1);

  // ---- TRANSPORT -------------------------------------------------------------
  const uint32_t cap = (uint32_t)cfg_.gasPixelCap;
  const float D = std::clamp(cfg_.gasDiffuse, 0.0f, 0.2f);
  const float Dx = std::clamp(cfg_.gasExcessDiffuse, 0.0f, 0.24f), Rf = (float)GasR();
  gasMoves_.clear();
  auto stoch = [&](float f) -> uint32_t {
    if (f <= 0) return 0;
    const float fl = std::floor(f);
    return (uint32_t)fl + (Rand01() < (double)(f - fl) ? 1u : 0u);
  };
  auto vent = [&](int k, uint32_t n) {
    const int s = gasSub_[k];
    n = std::min<uint32_t>(n, gasAmt_[k]);
    if (!n) return;
    spilledUnits_[s] += BankGas(0, s, n);
    NoteExit((k % W) + 0.5f, n);
    gasAmt_[k] = (uint16_t)(gasAmt_[k] - n);
  };
  // THE MOUTH'S EXCHANGE (SimConfig::gasMouthExchange, gasWindMouth). A gas
  // leaves an open mouth by a counter-flow in the neck -- out along one
  // side, room air in along the other -- and a breeze over the mouth adds
  // the eddy it spins there and the turbulence at the lip. The 2-px grid
  // resolves neither in a 12-px neck: measured, the solve's own exchange
  // was a few px wide at under a px a step, so a light gas mixed through the
  // bulb reached the lip dilute and an open flask looked stoppered (and a
  // heavy vapour held mouth-down poured a fifth in 6 s); driving a cavity
  // eddy on the air only swapped the neck's air with the plume over it. So
  // it acts on the GAS (the units drift; the air is untouched): in the top
  // gasWindMouthDepth neck-widths of every open vessel holding gas (and
  // kMouthAbove px over the lip) the gas drifts out along the vessel's up at
  // gasMouthExchange + gasWindMouth x the breeze's speed at the mouth,
  // fading with depth, where the mouth faces the way that gas goes (the
  // transport below). Units only move: exact. A stopper is glass.
  draws_.clear();
  const float mouthX = std::max(0.0f, cfg_.gasMouthExchange);
  for (size_t vi = 0; vi < vessels_.size(); vi++) {
    const Vessel& vs = vessels_[vi];
    if (vs.outline.size() < 2 || vs.stoppered || vi + 1 >= gasHolds_.size() || !gasHolds_[vi + 1]) continue;
    MouthDraw md;
    md.x = vs.x;
    const V2 la = vs.outline.front(), lb = vs.outline.back();
    md.lm = (la + lb) * 0.5f;
    md.hw = 0.5f * std::hypot(lb.x - la.x, lb.y - la.y);
    if (md.hw < 1.0f) continue;
    const V2 o = ToWorld(vs.x, {0, 0});
    md.up = ToWorld(vs.x, {0, 1}) - o;
    const V2 w = cfg_.gasWind > 0 ? WindAt(ToWorld(vs.x, md.lm)) : V2{};
    md.speed = std::max(0.0f, cfg_.gasWindMouth) * std::hypot(w.x, w.y);
    if (md.speed + mouthX <= 0) continue;
    md.depth = std::max(2.0f, cfg_.gasWindMouthDepth * 2.0f * md.hw);
    md.x0 = md.y0 = 1e9f;
    md.x1 = md.y1 = -1e9f;
    for (V2 lp : {V2{md.lm.x - md.hw, md.lm.y + kMouthAbove}, V2{md.lm.x + md.hw, md.lm.y + kMouthAbove},
                  V2{md.lm.x - md.hw, md.lm.y - md.depth}, V2{md.lm.x + md.hw, md.lm.y - md.depth}}) {
      const V2 p = ToWorld(vs.x, lp);
      md.x0 = std::min(md.x0, p.x); md.x1 = std::max(md.x1, p.x);
      md.y0 = std::min(md.y0, p.y); md.y1 = std::max(md.y1, p.y);
    }
    draws_.push_back(md);
  }
  // Can gas of slot s go into pixel nk (before this step's moves)?
  auto openPx = [&](int nk, int s) {
    if (wall_[nk] || grid_[nk] || liqPx_[nk]) return false;
    return !gasAmt_[nk] || gasSub_[nk] == s;
  };
  const size_t nG = gasList_.size();
  for (size_t a = 0; a < nG; a++) {
    const int k = gasList_[a];
    if (!gasAmt_[k]) continue;
    const int x = k % W, y = k / W;
    const int s = gasSub_[k];
    const Substance& S = subs_[s];
    // IN GLASS OR A GRAIN (the glass swept over it, a grain landed on it):
    // out to the nearest free pixel on its own side.
    if (wall_[k] || grid_[k]) {
      const int side = (int)inside_[k] - 1;
      int best = -1;
      for (int r = 1; r <= 5 && best < 0; r++)
        for (int dy = -r; dy <= r && best < 0; dy++)
          for (int dx = -r; dx <= r && best < 0; dx++) {
            if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
            const int xx = x + dx, yy = y + dy;
            if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
            const int nk = yy * W + xx;
            if ((int)inside_[nk] - 1 != side || !openPx(nk, s)) continue;
            best = nk;
          }
      if (best >= 0) gasMoves_.push_back({k, best, gasAmt_[k], (uint8_t)s, gasAge_[k]});
      else {
        const uint32_t q = gasAmt_[k];
        gasAmt_[k] = 0;
        AddPool(side, s, q, {x + 0.5f, y + 0.5f});
      }
      continue;
    }
    const bool outside = inside_[k] == 0;
    if (outside) {
      if (gasAge_[k] < 255) gasAge_[k]++;
      // Off the top or the sides of the table: in the world.
      if (y >= H - 2 || x <= 0 || x >= W - 1) { vent(k, gasAmt_[k]); continue; }
      // Lingered: it thins into the room -- and a wisp too thin to see
      // (under a twelfth of its natural volume) is gone into it outright, so
      // a spill leaves no bench-wide haze to simulate.
      // A heavy vapour lingers twice as long: it is seen to fall to the table.
      if (gasAge_[k] > cfg_.gasVentSteps * (S.heavy ? 2 : 1)) {
        if (gasAmt_[k] * 12u < (uint32_t)GasR()) { vent(k, gasAmt_[k]); continue; }
        vent(k, stoch((float)gasAmt_[k] * (S.heavy ? cfg_.gasFadeHeavy : cfg_.gasFadeLight)));
        if (!gasAmt_[k]) continue;
      }
    } else {
      gasAge_[k] = 0;
    }
    const uint32_t amt = gasAmt_[k];
    // A BUBBLE: gas in liquid rises through it whole, heavy or not.
    if (liqPx_[k]) {
      const int sd = (Rand() & 1) ? 1 : -1;
      const int mv[3][2] = {{0, 1}, {sd, 1}, {-sd, 1}};
      for (auto& m : mv) {
        const int nx = x + m[0], ny = y + m[1];
        if (nx < 0 || nx >= W) continue;
        if (ny >= H) { vent(k, amt); break; }
        const int nk = ny * W + nx;
        if (wall_[nk] || (grid_[nk] && (Rand() & 7))) continue;
        if (gasAmt_[nk] && gasSub_[nk] != s) continue;
        gasMoves_.push_back({k, nk, (uint16_t)amt, (uint8_t)s, gasAge_[k]});
        break;
      }
      continue;
    }
    // THE FLOW: upwind through each face by its velocity, plus diffusion
    // down the difference.
    const float cx = x + 0.5f, cy = y + 0.5f;
    float vel[4] = {SampleGu(x + 1.0f, cy), -SampleGu((float)x, cy), SampleGv(cx, y + 1.0f),
                    -SampleGv(cx, (float)y)};
    // The breeze's draw at an open mouth: the gas (not the air) drifts up
    // and out through it.
    for (const MouthDraw& md : draws_) {
      if (cx < md.x0 || cx > md.x1 || cy < md.y0 || cy > md.y1) continue;
      // Only where the mouth faces the way this gas goes: up for a light
      // gas, down for a heavy one (a flask held mouth-down pours its
      // vapour; upright, the vapour lies in it; a light gas in an upturned
      // flask is a trapped bubble). Sideways, a little.
      const float face = std::clamp((S.heavy ? -md.up.y : md.up.y) + 0.3f, 0.0f, 1.0f);
      if (face <= 0) continue;
      const V2 l = ToLocal(md.x, {cx, cy});
      const float xi = (l.x - md.lm.x) / md.hw, d = md.lm.y - l.y;
      if (xi <= -1.0f || xi >= 1.0f || d > md.depth || d < -kMouthAbove) continue;
      const float sp = std::min(kMouthMaxDrift, (md.speed + mouthX) * face) * (d <= 0 ? 1.0f : 1.0f - d / md.depth);
      vel[0] += sp * md.up.x; vel[1] -= sp * md.up.x;
      vel[2] += sp * md.up.y; vel[3] -= sp * md.up.y;
    }
    static const int kD[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    float f[4];
    int to[4];
    float sum = 0;
    for (int d = 0; d < 4; d++) {
      f[d] = 0;
      to[d] = -1;
      const int nx = x + kD[d][0], ny = y + kD[d][1];
      if (nx < 0 || nx >= W || ny < 0) continue;
      if (ny >= H) {   // off the top: in the world
        to[d] = -2;
        f[d] = (float)amt * std::max(0.0f, vel[d]);
        sum += f[d];
        continue;
      }
      const int nk = ny * W + nx;
      if (!openPx(nk, s)) continue;
      const float b = gasAmt_[nk];
      // Diffusion down the difference, and -- far faster -- of the EXCESS
      // over the natural volume: a packed pixel shares into its neighbours
      // (it is under pressure), which is what spreads fresh vapour through
      // its pool and undoes a carried cloud's packing.
      const float ex = std::max(0.0f, (float)amt - Rf) - std::max(0.0f, b - Rf);
      f[d] = (float)amt * std::max(0.0f, vel[d]) + D * std::max(0.0f, (float)amt - b) + Dx * std::max(0.0f, ex);
      to[d] = nk;
      sum += f[d];
    }
    if (sum <= 0) continue;
    const float lim = 0.95f * (float)amt;
    const float sc = sum > lim ? lim / sum : 1.0f;
    uint32_t left = amt;
    for (int d = 0; d < 4 && left; d++) {
      if (to[d] == -1 || f[d] <= 0) continue;
      const uint32_t n = std::min(left, stoch(f[d] * sc));
      if (!n) continue;
      if (to[d] == -2) { vent(k, n); left -= n; continue; }
      gasMoves_.push_back({k, to[d], (uint16_t)n, (uint8_t)s, gasAge_[k]});
      left -= n;
    }
  }
  // Apply: every move leaves its pixel first, then arrives -- a pixel that
  // took two gases in one step keeps the first and sends the other back.
  for (const GasMove& m : gasMoves_) gasAmt_[m.from] = (uint16_t)(gasAmt_[m.from] - m.n);
  for (const GasMove& m : gasMoves_) {
    uint32_t q = m.n;
    int dst = m.to;
    if (gasAmt_[dst] && gasSub_[dst] != m.sub) dst = m.from;
    if (gasAmt_[dst] && gasSub_[dst] != m.sub) dst = -1;
    if (dst >= 0) {
      const uint32_t room = cap - std::min<uint32_t>(cap, gasAmt_[dst]);
      const uint32_t put = std::min(q, room);
      if (put) {
        if (!gasAmt_[dst]) gasAge_[dst] = m.age;
        else gasAge_[dst] = std::min(gasAge_[dst], m.age);
        gasSub_[dst] = m.sub;
        gasAmt_[dst] = (uint16_t)(gasAmt_[dst] + put);
        AddGasPixel(dst);
        q -= put;
      }
    }
    if (q) {
      const int side = (int)inside_[m.from] - 1;
      if (!AddGasAt(m.sub, q, m.from % W, m.from / W, side, 3))
        AddPool(side, m.sub, q, {(m.from % W) + 0.5f, (m.from / W) + 0.5f});
    }
  }
  for (const GasMove& m : gasMoves_)
    if (!gasAmt_[m.from]) gasSub_[m.from] = 0xFF;
  (void)H;
}

// A MOVING VESSEL CARRIES ITS GAS, as it carries its liquid: every cloud
// pixel inside it (judged in its previous pose) goes where the same point of
// the vessel is now -- and lands INSIDE it, never in the glass or outside
// (owner, 2026-09-27: heavy vapour fell out of a flask that was only moved).
// The old fallback put what did not fit on the nearest pixel anywhere --
// often across the glass -- or in a pool on the table; now it searches the
// vessel's own inside wider, and what still finds no room waits in a pool IN
// THE VESSEL (its frame; FlushPools puts it back inside). Gas leaves a moving
// vessel only the way liquid does: through the mouth, when the mouth is where
// the gas goes (tipped past level, a heavy vapour pours).
void FlaskSim::CarryGas() {
  if (gasList_.empty()) return;
  const int W = cfg_.gridW, H = cfg_.gridH;
  struct Mv { int from, to, vessel; };
  std::vector<Mv> moves;
  for (size_t vi = 0; vi < vessels_.size(); vi++) {
    const Vessel& v = vessels_[vi];
    if (v.outline.empty()) continue;
    if (v.x.pos.x == v.prevX.pos.x && v.x.pos.y == v.prevX.pos.y && v.x.angle == v.prevX.angle) continue;
    for (int k : gasList_) {
      if (!gasAmt_[k]) continue;
      const V2 p{(k % W) + 0.5f, (k / W) + 0.5f};
      const V2 l = ToLocal(v.prevX, p);
      if (!InsideLocal(v, l)) continue;
      const V2 w = ToWorld(v.x, l);
      const int nx = std::clamp((int)std::floor(w.x), 0, W - 1), ny = std::clamp((int)std::floor(w.y), 0, H - 1);
      const int nk = ny * W + nx;
      // A pixel that stays put and stays inside needs no move.
      if (nk != k || inside_[k] != (uint8_t)(vi + 1) || wall_[k]) moves.push_back({k, nk, (int)vi});
    }
  }
  if (moves.empty()) return;
  // Lift every moving pixel off the grid first, then set it down: a pixel
  // moving into one that is itself moving must not merge with it.
  struct Held { int to; uint8_t sub; uint16_t amt; uint8_t age; int vessel; };
  std::vector<Held> held;
  held.reserve(moves.size());
  for (const Mv& m : moves) {
    if (!gasAmt_[m.from]) continue;
    held.push_back({m.to, gasSub_[m.from], gasAmt_[m.from], gasAge_[m.from], m.vessel});
    gasAmt_[m.from] = 0;
    gasSub_[m.from] = 0xFF;
  }
  for (const Held& h : held) {
    uint32_t q = h.amt;
    const int x = h.to % W, y = h.to / W;
    if (!AddGasAt(h.sub, q, x, y, h.vessel) && !AddGasAt(h.sub, q, x, y, h.vessel, 10))
      AddPool(h.vessel, h.sub, q, {x + 0.5f, y + 0.5f});
    gasAge_[h.to] = h.age;
  }
}

// THE GAS'S LOOK. A cloud in the material's own colours whose alpha is how
// much is there (natural volumes, softly blurred) x the material's opacity,
// broken into wisps by a texture that RIDES THE AIR (StepGasFlow's advected
// coordinates): moving gas curls and streams, still gas is still -- nothing
// here reads the clock. Lit from above: a deep cloud is darker underneath.
// In liquid it is bubbles.
void FlaskSim::RenderGas(std::vector<uint32_t>& out) const {
  if (gasList_.empty() || gtc_.empty()) return;
  const int W = cfg_.gridW, H = cfg_.gridH, C = kGasCell;
  const float invR = 1.0f / (float)GasR();
  int x0 = W, x1 = -1, y0 = H, y1 = -1;
  for (int k : gasList_) {
    if (!gasAmt_[k]) continue;
    const int x = k % W, y = k / W;
    x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
  }
  if (x1 < x0) return;
  constexpr int kR = 2;
  x0 = std::max(0, x0 - kR); y0 = std::max(0, y0 - kR);
  x1 = std::min(W - 1, x1 + kR); y1 = std::min(H - 1, y1 + kR);
  const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
  const size_t n = (size_t)bw * bh;
  // Density in natural volumes, blurred by a separable 5-tap Gaussian: the
  // integer units' grain goes, the shape stays.
  static const float kG[5] = {0.0545f, 0.2442f, 0.4026f, 0.2442f, 0.0545f};
  rGas_.assign(n, 0.0f);
  std::vector<float>& tmp = rGasTmp_;
  tmp.assign(n, 0.0f);
  for (int yy = 0; yy < bh; yy++)
    for (int xx = 0; xx < bw; xx++) {
      const size_t k = (size_t)(yy + y0) * W + xx + x0;
      if (!gasAmt_[k]) continue;
      const float a = (float)gasAmt_[k] * invR;
      for (int t = -kR; t <= kR; t++) {
        const int x = xx + t;
        if (x >= 0 && x < bw) tmp[(size_t)yy * bw + x] += a * kG[t + kR];
      }
    }
  for (int yy = 0; yy < bh; yy++)
    for (int xx = 0; xx < bw; xx++) {
      const float a = tmp[(size_t)yy * bw + xx];
      if (a <= 0) continue;
      for (int t = -kR; t <= kR; t++) {
        const int y = yy + t;
        if (y >= 0 && y < bh) rGas_[(size_t)y * bw + xx] += a * kG[t + kR];
      }
    }
  // Which gas a pixel shows: its own, else the fullest within reach.
  rGasSub_.assign(n, 0xFF);
  for (int yy = 0; yy < bh; yy++)
    for (int xx = 0; xx < bw; xx++) {
      const size_t j = (size_t)yy * bw + xx;
      if (rGas_[j] <= 0.01f) continue;
      const int x = xx + x0, y = yy + y0;
      const size_t k = (size_t)y * W + x;
      if (gasAmt_[k]) { rGasSub_[j] = gasSub_[k]; continue; }
      uint16_t best = 0;
      for (int dy = -kR; dy <= kR; dy++)
        for (int dx = -kR; dx <= kR; dx++) {
          const int xx2 = x + dx, yy2 = y + dy;
          if (xx2 < 0 || yy2 < 0 || xx2 >= W || yy2 >= H) continue;
          const size_t k2 = (size_t)yy2 * W + xx2;
          if (gasAmt_[k2] > best) { best = gasAmt_[k2]; rGasSub_[j] = gasSub_[k2]; }
        }
    }
  // Light from above through the cloud: optical depth accumulated down each
  // column (a deep pool of vapour is shadowed at its bottom). Kept as the
  // depth; its exp is taken only where a pixel is drawn.
  std::vector<float>& shade = tmp;
  for (int xx = 0; xx < bw; xx++) {
    float acc = 0;
    for (int yy = bh - 1; yy >= 0; yy--) {
      const size_t j = (size_t)yy * bw + xx;
      acc = acc * 0.93f + rGas_[j];
      shade[j] = acc;
    }
  }
  // The advected texture, bilinear over cell centres.
  auto tex = [&](float px, float py) {
    const float T = (float)kTexCell;
    const float fx = std::clamp(px / T - 0.5f, 0.0f, (float)(gtW_ - 1)),
                fy = std::clamp(py / T - 0.5f, 0.0f, (float)(gtH_ - 1));
    const int i0 = std::min((int)fx, gtW_ - 2), j0 = std::min((int)fy, gtH_ - 2);
    const float tx = fx - i0, ty = fy - j0;
    const float* a = &gtc_[((size_t)j0 * gtW_ + i0) * 5];
    const float* b = &gtc_[((size_t)j0 * gtW_ + i0 + 1) * 5];
    const float* c = &gtc_[((size_t)(j0 + 1) * gtW_ + i0) * 5];
    const float* d = &gtc_[((size_t)(j0 + 1) * gtW_ + i0 + 1) * 5];
    float o[4];
    for (int q = 0; q < 4; q++) o[q] = (a[q] + (b[q] - a[q]) * tx) * (1 - ty) + (c[q] + (d[q] - c[q]) * tx) * ty;
    const float ph = (tx < 0.5f ? (ty < 0.5f ? a : c) : (ty < 0.5f ? b : d))[4];
    // Two octaves of value noise per layer, crossfaded by the phase (each
    // layer's weight is zero when it is reset), contrast kept.
    auto fbm = [](float x, float y) { return 0.65f * Noise2(x * 0.11f, y * 0.11f) + 0.35f * Noise2(x * 0.27f + 5.3f, y * 0.27f + 1.7f); };
    const float wA = 1.0f - std::fabs(2.0f * ph - 1.0f);
    const float nA = fbm(o[0], o[1]), nB = fbm(o[2] + 37.0f, o[3] + 11.0f);
    float v = nA * wA + nB * (1 - wA);
    v = 0.5f + (v - 0.5f) / std::sqrt(wA * wA + (1 - wA) * (1 - wA));
    return std::clamp(v, 0.0f, 1.0f);
  };
  auto put = [&](int x, int y, int r, int g, int b, int a) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    uint32_t& o = out[(size_t)(H - 1 - y) * W + x];
    o = Over(o, r, g, b, a);
  };
  const float t = (float)step_;
  for (int yy = 0; yy < bh; yy++)
    for (int xx = 0; xx < bw; xx++) {
      const size_t j = (size_t)yy * bw + xx;
      const float d = rGas_[j];
      if (d <= 0.01f || rGasSub_[j] == 0xFF) continue;
      const int x = xx + x0, y = yy + y0;
      const size_t k = (size_t)y * W + x;
      if (grid_[k]) continue;
      const Substance& S = subs_[rGasSub_[j]];
      if (rTotal_.size() == (size_t)W * H && rTotal_[k] > 0.28f) {
        // Bubbles in liquid: a bright rim, only where the gas is.
        if (gasAmt_[k] && ((x + y + (int)(t * 0.25f)) & 1)) {
          const uint32_t c = S.color[0];
          const int r = Enc(R8(c)), g = Enc(G8(c)), b = Enc(B8(c));
          put(x, y, r + (255 - r) / 2, g + (255 - g) / 2, b + (255 - b) / 2, 210);
        }
        continue;
      }
      const float op = std::max(0.02f, S.opacity / 255.0f);
      const float n1 = tex(x + 0.5f, y + 0.5f);
      // Thin gas breaks into wisps; a thick cloud only mottles.
      const float thin = std::clamp(1.0f - d * 0.5f, 0.0f, 1.0f);
      const float dd = d * (1.0f - thin * 0.75f + thin * 1.4f * (n1 - 0.2f));
      float a = (1.0f - std::exp(-std::max(0.0f, dd) * 1.5f * (0.3f + op))) * (0.8f + 0.2f * n1);
      // Colour: the palette blended by the same texture (so it moves with it).
      const float m = std::clamp(n1 * 1.6f - 0.3f, 0.0f, 1.0f);
      const uint32_t c0 = S.color[0], c1 = S.color[n1 < 0.5f ? 1 : 2];
      auto mix = [&](int p, int q) { return (int)(p + (q - p) * std::fabs(m - 0.5f) * 2.0f); };
      int r = Enc(mix(R8(c0), R8(c1))), g = Enc(mix(G8(c0), G8(c1))), b = Enc(mix(B8(c0), B8(c1)));
      // Smoke is lit: a dark gas still reads against the dark desk as a haze
      // of its own hue (a third of the way to a pale grey), shadowed below.
      r += (170 - r) / 3; g += (175 - g) / 3; b += (170 - b) / 3;
      const float lit = 0.62f + 0.38f * std::exp(-0.06f * shade[j]);
      r = (int)(r * lit); g = (int)(g * lit); b = (int)(b * lit);
      const float glow = S.emission / 255.0f;
      if (wall_[k]) a *= 0.5f;
      r += (int)((255 - r) * glow * 0.5f);
      g += (int)((255 - g) * glow * 0.5f);
      b += (int)((255 - b) * glow * 0.5f);
      put(x, y, r, g, b, (int)(std::min(0.92f, a + glow * 0.3f * std::min(1.0f, d)) * 255));
    }
}

// ---- the look ----------------------------------------------------------------

void FlaskSim::RenderChem(std::vector<uint32_t>& out) const {
  const int W = cfg_.gridW, H = cfg_.gridH;
  const float t = (float)step_;
  auto put = [&](int x, int y, int r, int g, int b, int a) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    uint32_t& o = out[(size_t)(H - 1 - y) * W + x];
    o = Over(o, r, g, b, a);
  };
  uint32_t& rng = lookRng_;
  auto rnd = [&rng] { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };

  // 1. DISSOLVED MATTER tints (and lights) its solvent by concentration.
  if (!chem_.solutes.empty() && !rOwner_.empty()) {
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        const size_t k = (size_t)y * W + x;
        if (grid_[k] || wall_[k] || rTotal_[k] <= 0.28f) continue;
        const int o = rOwner_[k];
        if (o < 0 || o >= (int)pmass_.size() || !pmass_[o]) continue;
        const ChemSolute* sp = chem_.Species(psol_[o]);
        if (!sp) continue;
        const float c = std::min(1.0f, (float)pmass_[o] * sp->yieldPerVoxel /
                                           std::max(1.0f, (float)pw_[o] * std::max(1u, sp->saturation)));
        const float f = c * sp->tintStrength / 255.0f;
        const float gl = c * sp->glow / 255.0f * (0.8f + 0.2f * std::sin(t * 0.03f + (pvar_[o] & 31)));
        uint32_t& px = out[(size_t)(H - 1 - y) * W + x];
        const int tr = Enc(sp->tint >> 16 & 255), tg = Enc(sp->tint >> 8 & 255), tb = Enc(sp->tint & 255);
        const int a = A8(px);
        int r = R8(px) + (int)((tr - R8(px)) * f), g = G8(px) + (int)((tg - G8(px)) * f),
            b = B8(px) + (int)((tb - B8(px)) * f);
        r += (int)((255 - r) * gl * 0.5f);
        g += (int)((255 - g) * gl * 0.5f);
        b += (int)((255 - b) * gl * 0.5f);
        px = Pack(r, g, b, std::max(a, (int)(a + gl * 60)));
        // Motes of undissolved sparkle where it glows.
        if (sp->glow && ((x * 7 + y * 13 + (int)(t * 0.2f)) % 97) == 0 && c > 0.2f) put(x, y, 255, 240, 255, 230);
      }
  }

  // 2. THE BURNER'S FLAME and the glass it heats.
  for (size_t vi = 0; vi < vessels_.size(); vi++) {
    const Vessel& v = vessels_[vi];
    if (v.outline.empty()) continue;
    if (v.heat > 0.02f) {
      // Hot glass glows a dull red.
      for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
          if (wall_[(size_t)y * W + x] != vi + 1) continue;
          uint32_t& px = out[(size_t)(H - 1 - y) * W + x];
          const float f = 0.45f * v.heat;
          px = Pack(R8(px) + (int)((255 - R8(px)) * f), G8(px) + (int)((110 - G8(px)) * f),
                    B8(px) + (int)((50 - B8(px)) * f), std::min(255, A8(px) + (int)(50 * v.heat)));
        }
    }
    if (!v.burner) continue;
    // The burner: a brass cup on the table under the vessel.
    const float bx = v.x.pos.x, by = cfg_.tableY;
    const bool lit = Burning((int)vi);
    for (int dx = -6; dx <= 6; dx++)
      for (int dy = -3; dy <= -1; dy++) {
        const bool rim = dy == -1 || std::abs(dx) == 6;
        put((int)bx + dx, (int)by + dy, rim ? 196 : 150, rim ? 150 : 108, rim ? 70 : 48, 255);
      }
    if (!lit) continue;
    // The flame: tongues licking up round the base -- blue at the root,
    // orange, yellow at the tips, flickering -- wrapping the bottom of the
    // glass (never drawn inside it), with a warm glow on the table.
    const float hw = std::max(v.shape.profile.front().x, v.shape.profile[1].x) * v.shape.width * 0.5f + 7.0f;
    for (int dx = -(int)hw - 6; dx <= (int)hw + 6; dx++)
      for (int dy = -1; dy < 5; dy++) {
        const float g = std::max(0.0f, 1.0f - std::fabs((float)dx) / (hw + 6)) * (1.0f - dy / 5.0f);
        put((int)bx + dx, (int)by + dy, 255, 170, 70, (int)(90 * g * (0.5f + 0.5f * v.heat)));
      }
    for (int dx = -(int)hw; dx <= (int)hw; dx++) {
      const float u = 1.0f - std::fabs((float)dx) / hw;
      const float fl = 0.55f + 0.45f * Noise2(dx * 0.35f, t * 0.12f);
      const int h = (int)(std::sqrt(u) * 16.0f * fl + 3);
      for (int dy = 0; dy < h; dy++) {
        const float f = (float)dy / std::max(1, h);
        int r, g, b, a;
        if (f < 0.3f) { r = 90; g = 150; b = 255; a = 200; }
        else if (f < 0.7f) { r = 255; g = 150; b = 40; a = 220; }
        else { r = 255; g = 230; b = 110; a = 170; }
        const int px = (int)bx + dx, py = (int)by + dy;
        if (px < 0 || py < 0 || px >= W || py >= H) continue;
        const size_t k = (size_t)py * W + px;
        // Over the glass the flame is in FRONT of the flask (a cross-section
        // of a flame wrapping its bottom): drawn there, thinner.
        const bool front = inside_.size() == (size_t)W * H && inside_[k] == vi + 1;
        if (front) a = a * 11 / 20;
        put(px, py, r, g, b, a);
      }
    }
  }

  // 3. THE GAS (RenderGas).
  RenderGas(out);

  // 4. THE ARC: an electrode over the mouth and a crackling bolt down into
  // the liquid, redrawn every picture.
  for (size_t vi = 0; vi < vessels_.size(); vi++) {
    const Vessel& v = vessels_[vi];
    if (v.outline.empty() || v.shock <= 0) continue;
    const V2 top = ToWorld(v.x, {0.0f, v.shape.height + 16.0f});
    const V2 tip = ToWorld(v.x, {0.0f, v.shape.height + 4.0f});
    const V2 dst = ToWorld(v.x, {0.0f, v.shape.height * 0.18f});
    // The electrode rod.
    for (int k = 0; k <= 14; k++) {
      const V2 p{top.x + (tip.x - top.x) * k / 14.0f, top.y + (tip.y - top.y) * k / 14.0f};
      put((int)p.x, (int)p.y, 120, 120, 132, 255);
      put((int)p.x + 1, (int)p.y, 70, 70, 80, 255);
    }
    auto bolt = [&](V2 a, V2 b, int segs, float jag, int bright) {
      V2 prev = a;
      const V2 d = VSub(b, a);
      const float l = std::max(1.0f, std::sqrt(Dot2(d, d)));
      const V2 nrm{-d.y / l, d.x / l};
      for (int s = 1; s <= segs; s++) {
        const float f = (float)s / segs;
        const float off = s == segs ? 0.0f : ((int)(rnd() % 1000) / 500.0f - 1.0f) * jag;
        const V2 p{a.x + d.x * f + nrm.x * off, a.y + d.y * f + nrm.y * off};
        const int n = (int)std::ceil(std::max(std::fabs(p.x - prev.x), std::fabs(p.y - prev.y)));
        for (int k = 0; k <= n; k++) {
          const float u = n ? (float)k / n : 0.0f;
          const int x = (int)(prev.x + (p.x - prev.x) * u), y = (int)(prev.y + (p.y - prev.y) * u);
          for (int oy = -1; oy <= 1; oy++)
            for (int ox = -1; ox <= 1; ox++)
              if (ox || oy) put(x + ox, y + oy, 110, 160, 255, bright / 3);
          put(x, y, 235, 244, 255, bright);
        }
        prev = p;
      }
    };
    bolt(tip, dst, 9, 4.5f, 255);
    if (rnd() & 1) {
      const V2 mid{tip.x + (dst.x - tip.x) * 0.5f, tip.y + (dst.y - tip.y) * 0.5f};
      const V2 br{mid.x + ((int)(rnd() % 21) - 10), mid.y - 6.0f - (rnd() % 8)};
      bolt(mid, br, 4, 2.5f, 200);
    }
  }

  // 5. SHARDS of burst glass, falling.
  if (!shards_.empty()) {
    const int el = (int)std::min<uint32_t>(64, step_ - shardStep_);
    shardStep_ = step_;
    size_t w = 0;
    for (Shard s : shards_) {
      for (int k = 0; k < el; k++) {
        s.vy -= cfg_.gravity * 2.0f;
        s.x += s.vx * 0.5f;
        s.y += s.vy * 0.5f;
        s.life--;
      }
      if (s.life <= 0 || s.y < -2 || s.x < -2 || s.x > W + 2) continue;
      shards_[w++] = s;
      const int a = std::min(230, 60 + s.life * 2);
      put((int)s.x, (int)s.y, 225, 240, 255, a);
      put((int)s.x + (s.vx > 0 ? 1 : -1), (int)s.y, 160, 200, 230, a / 2);
    }
    shards_.resize(w);
  }

  // 6. THE STOPPER: a cork in the mouth, turned with the vessel.
  for (size_t vi = 0; vi < vessels_.size(); vi++) {
    const Vessel& v = vessels_[vi];
    if (v.outline.empty() || !v.stoppered) continue;
    const float H0 = v.shape.height;
    const float mh = v.shape.profile.back().x * v.shape.width * 0.5f;
    const V2 cs[4] = {ToWorld(v.x, {-mh - 3, H0 - 5}), ToWorld(v.x, {mh + 3, H0 - 5}),
                      ToWorld(v.x, {-mh - 3, H0 + 7}), ToWorld(v.x, {mh + 3, H0 + 7})};
    float ax0 = 1e9f, ax1 = -1e9f, ay0 = 1e9f, ay1 = -1e9f;
    for (V2 c : cs) { ax0 = std::min(ax0, c.x); ax1 = std::max(ax1, c.x); ay0 = std::min(ay0, c.y); ay1 = std::max(ay1, c.y); }
    for (int y = std::max(0, (int)ay0); y <= std::min(H - 1, (int)ay1); y++)
      for (int x = std::max(0, (int)ax0); x <= std::min(W - 1, (int)ax1); x++) {
        const V2 l = ToLocal(v.x, {x + 0.5f, y + 0.5f});
        const float top = H0 + 6.0f, bot = H0 - 4.0f;
        if (l.y < bot || l.y > top) continue;
        // Tapered: narrower in the neck, a cap proud of the lip.
        const float half = l.y < H0 + 1.0f ? mh + 0.5f - (H0 + 1.0f - l.y) * 0.25f : mh + 2.0f;
        if (std::fabs(l.x) > half) continue;
        const bool rim = std::fabs(l.x) > half - 1.0f || l.y > top - 1.0f || l.y < bot + 1.0f;
        const float sp = Hash01((int)std::floor(l.x), (int)std::floor(l.y), 3);
        int r = 158, g = 106, b = 58;
        if (rim) { r = 104; g = 66; b = 36; }
        else if (sp > 0.8f) { r = 190; g = 140; b = 86; }
        else if (sp < 0.15f) { r = 128; g = 84; b = 44; }
        put(x, y, r, g, b, 255);
      }
  }
}

}  // namespace alchemy
