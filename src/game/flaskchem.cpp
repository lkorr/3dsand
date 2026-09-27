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
inline int Enc(int v) { return (int)std::lround(255.0 * std::pow(std::clamp(v, 0, 255) / 255.0, 1.0 / 1.5)); }

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
    // GAS: into the vessel's free inside pixels, top down, a little at a time
    // round and round so it starts as a cloud rather than a slab.
    uint32_t left = c.p[i].eighths * upe;
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
    for (int pass = 0; left && !px.empty() && pass < 64; pass++)
      for (int k : px) {
        if (!left) break;
        const uint32_t room = cap - std::min<uint32_t>(cap, gasAmt_[k]);
        const uint32_t put = std::min({left, room, 6u});
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
  return (float)GasUnits(vi) / free;
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
      e.count++;
      e.amount += fx.amount;
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
    if (p >= cfg_.burstAt || v.heat > 0.5f) {
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
  for (Pool& p : pools_)
    if (p.vessel == vi) {
      const V2 w = ToWorld(v.x, {(float)(p.sx / std::max(1e-9, p.w)), (float)(p.sy / std::max(1e-9, p.w))});
      p.sx = (double)w.x * p.w;
      p.sy = (double)w.y * p.w;
      p.vessel = -1;
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

bool FlaskSim::AuditUnits(std::string* why) const {
  const size_t S = subs_.size();
  std::vector<int64_t> live(S, 0);
  for (size_t i = 0; i < px_.size(); i++) {
    live[psub_[i]] += pw_[i];
    if (pmass_[i]) {
      const ChemSolute* sp = chem_.Species(psol_[i]);
      if (sp && sp->from >= 0) live[sp->from] += pmass_[i];
    }
  }
  for (const Grain& g : grains_) live[g.sub] += 1;
  for (int k : gasList_)
    if (gasAmt_[k]) live[gasSub_[k]] += gasAmt_[k];
  for (const Pool& p : pools_) live[p.sub] += p.units;
  bool ok = true;
  for (size_t s = 0; s < S; s++) {
    const int64_t have = live[s] + spilledUnits_[s] + removed_[s] + drained_[s];
    const int64_t want = seeded_[s] + produced_[s] - consumed_[s];
    if (have != want) {
      ok = false;
      if (why) {
        char b[160];
        std::snprintf(b, sizeof b, "mat %u: have %lld units (live %lld spill %u off %lld drained %lld), ledger says %lld; ",
                      (unsigned)subs_[s].mat, (long long)have, (long long)live[s], spilledUnits_[s],
                      (long long)removed_[s], (long long)drained_[s], (long long)want);
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

bool FlaskSim::AddGasAt(int sub, uint32_t& q, int x, int y) {
  const int W = cfg_.gridW, H = cfg_.gridH;
  const uint32_t cap = (uint32_t)cfg_.gasPixelCap;
  for (int r = 0; r <= 3 && q; r++)
    for (int dy = -r; dy <= r && q; dy++)
      for (int dx = -r; dx <= r && q; dx++) {
        if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
        const int xx = x + dx, yy = y + dy;
        if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
        const int k = yy * W + xx;
        if (wall_[k]) continue;
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
    if (!AddGasAt(sub, q, x, y)) AddPool(hv, sub, q, at);
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
      if (!PlaceGrain(g, x, y)) break;
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
      AddGasAt(p.sub, p.units, (int)std::floor(at.x), (int)std::floor(at.y));
    } else if (S.powder) {
      const int x = std::clamp((int)std::floor(at.x), 0, cfg_.gridW - 1);
      const int y = std::clamp((int)std::floor(at.y), 0, cfg_.gridH - 1);
      while (p.units) {
        Grain g{};
        g.sub = (uint8_t)p.sub;
        g.variant = (uint8_t)(Rand() % 3);
        g.home = (int8_t)hv;
        g.chem = (uint8_t)chemStep_;
        if (!PlaceGrain(g, x, y)) break;
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
  if (type == NbGas && to >= 0 && subs_[to].gas && q >= gasAmt_[idx]) {
    gasSub_[idx] = (uint8_t)to;
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
    // directions -- no glass, no grain, no gas, no other liquid there.
    static const int kD[4][2] = {{0, 1}, {1, 0}, {-1, 0}, {0, -1}};
    for (auto& d : kD) {
      const float qx = p.x + d[0] * spacing_ * 0.9f, qy = p.y + d[1] * spacing_ * 0.9f;
      const int xx = (int)std::floor(qx), yy = (int)std::floor(qy);
      if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
      const int k = yy * W + xx;
      if (wall_[k] || grid_[k] || gasAmt_[k]) continue;
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
    if (r.kind == kChemDecay) {
      uint32_t chance = r.chance;
      if (ChemScaleArmed(r.cond)) {
        uint32_t count = 0;
        for (int k = 0; k < nn && count < 6; k++)
          count += matches(r, nb[k]) != ChemScaleInverted(r.cond);
        chance = ChemScaledChance(r.chance, r.cond, count, chem_.chanceDen);
      }
      if (!chance || Rand01() * den >= chance * scale) continue;
      if (r.fx >= 0) RaiseEvent(chem_.effects[r.fx], hv, at, subs_[slot].mat, 0, r);
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
      const int q = std::min(UnitsOf(selfType, selfIdx), cfg_.unitsPerParticle);
      if (r.prodNbr >= 0) {
        produced_[r.prodNbr] += q;
        Deposit(r.prodNbr, (uint32_t)q, nb[hit].at, hv);
      }
      if (r.fx >= 0) RaiseEvent(chem_.effects[r.fx], hv, at, subs_[slot].mat, 0, r);
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
    const int uS = UnitsOf(selfType, selfIdx);
    const int uN = virt ? INT_MAX : UnitsOf(n.type, n.idx);
    const int q = std::max(1, std::min(uS, uN));
    const uint16_t nm = nbMat(n);
    if (r.fx >= 0) RaiseEvent(chem_.effects[r.fx], hv, at, subs_[slot].mat, nm, r);
    if (r.prodNbr != kChemKeep) {
      if (!virt) {
        ConvertEnt(n.type, n.idx, q, r.prodNbr, hv);
      } else if (r.prodNbr >= 0) {
        // A virtual neighbour's product is MATERIALISED where it touched
        // (the world's spark voxel becomes chlorine; ours leaves chlorine).
        produced_[r.prodNbr] += q;
        Deposit(r.prodNbr, (uint32_t)q, n.at, hv);
      }
    }
    if (r.prodSelf != kChemKeep) ConvertEnt(selfType, selfIdx, q, r.prodSelf, hv);
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
      if (can) { activeSlot_[s] = 1; break; }
    }
    anyActive |= activeSlot_[s] != 0;
  }
  const bool anySolute = !chem_.solutes.empty();

  // PARTICLES: their rules, then what is dissolved in them.
  const int n0 = (int)px_.size();
  for (int i = 0; i < n0 && firedThisStep_ < cfg_.chemMaxFires; i++) {
    if (!pw_[i]) continue;
    const int s = psub_[i];
    const bool sol = anySolute && psol_[i] != 0;
    if (!activeSlot_[s] && !sol) continue;
    int hv = phome_[i];
    if (hv >= (int)vessels_.size() || (hv >= 0 && vessels_[hv].outline.empty())) hv = -1;
    GatherParticleNbrs(i, hv, nbScratch_);
    if (activeSlot_[s] && TryRules(NbParticle, i, s, nbScratch_, hv, px_[i], scale)) continue;
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
    if (activeSlot_[s]) TryRules(NbGrain, gi, s, nbScratch_, hv, {g.x + 0.5f, g.y + 0.5f}, scale);
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
        phome_[w] = phome_[i]; pvar_[w] = pvar_[i]; panc_[w] = panc_[i]; pheat_[w] = pheat_[i];
        psol_[w] = psol_[i]; pmass_[w] = pmass_[i];
      }
      w++;
    }
    nAct_ = act;
    px_.resize(w); pv_.resize(w); pprev_.resize(w); psub_.resize(w); pw_.resize(w); mbar_.resize(w);
    calm_.resize(w); phome_.resize(w); pvar_.resize(w); panc_.resize(w); pheat_.resize(w);
    psol_.resize(w); pmass_.resize(w);
    needPartition_ = true;
  }
}

// ---- the gas phase -----------------------------------------------------------
//
// A pixel CA of clouds. Each gas pixel holds up to gasPixelCap units of ONE
// gas. A step, every cloud pixel tries to move along its buoyancy -- up for a
// gas lighter than air, down for a heavy one (the world's density: chlorine
// 6 pools, hydrogen 1 races up) -- then diagonally, then sideways, and
// SPREADS as it goes (half of it moves, half stays), so a puff becomes a
// cloud that fills the headspace. Glass and a stopper stop it; a grain
// mostly does; liquid does not (gas under liquid rises through it as
// bubbles). Outside every vessel it lingers `gasVentSteps` and is then IN THE
// WORLD: counted as spilled, which the bench streams out at the lip of the
// flask in the hand.
void FlaskSim::StepGas() {
  if (gasList_.empty()) return;
  const int W = cfg_.gridW, H = cfg_.gridH;
  // Drop drained pixels from the list.
  {
    size_t w = 0;
    for (int k : gasList_) {
      if (gasAmt_[k]) gasList_[w++] = k;
      else { gasListed_[k] = 0; gasSub_[k] = 0xFF; gasAge_[k] = 0; }
    }
    gasList_.resize(w);
  }
  if (gasList_.empty()) return;
  if (gasMark_.size() != (size_t)W * H) gasMark_.assign((size_t)W * H, 0);
  const uint32_t mark = ++gasStamp_;
  gasOrder_ = gasList_;
  // Top rows first for a rising gas (so a column moves as a column), and the
  // row order alternates so a cloud does not lean.
  std::sort(gasOrder_.begin(), gasOrder_.end(), [&](int a, int b) {
    if (a / W != b / W) return a / W > b / W;
    return (step_ & 2) ? a > b : a < b;
  });
  BucketChem();
  const uint32_t cap = (uint32_t)cfg_.gasPixelCap;
  for (int k : gasOrder_) {
    if (!gasAmt_[k] || gasMark_[k] == mark) continue;
    const int x = k % W, y = k / W;
    const int s = gasSub_[k];
    const bool outside = inside_[k] == 0 && !wall_[k];
    if (outside) {
      if (gasAge_[k] < 255) gasAge_[k]++;
      if (gasAge_[k] > cfg_.gasVentSteps || y >= H - 2) {
        spilledUnits_[s] += gasAmt_[k];
        NoteExit(x + 0.5f, gasAmt_[k]);
        gasAmt_[k] = 0;
        gasSub_[k] = 0xFF;
        continue;
      }
    } else {
      gasAge_[k] = 0;
    }
    const Substance& S = subs_[s];
    const bool heavy = S.density >= 5;
    const int vdir = heavy ? -1 : 1;
    const float pRise = heavy ? 0.3f : std::clamp((6.0f - (float)S.density) / 5.0f, 0.3f, 1.0f);
    const bool inLiquid = NearestParticle(x + 0.5f, y + 0.5f, spacing_ * 0.6f) >= 0;
    // Candidate moves, in order.
    int cand[6][2];
    int nc = 0;
    const int side = (Rand() & 1) ? 1 : -1;
    if (Rand01() < pRise) cand[nc][0] = 0, cand[nc][1] = vdir, nc++;
    cand[nc][0] = side, cand[nc][1] = vdir, nc++;
    cand[nc][0] = -side, cand[nc][1] = vdir, nc++;
    if (!inLiquid) {
      cand[nc][0] = side, cand[nc][1] = 0, nc++;
      cand[nc][0] = -side, cand[nc][1] = 0, nc++;
    }
    for (int c = 0; c < nc; c++) {
      const int nx = x + cand[c][0], ny = y + cand[c][1];
      if (nx < 0 || nx >= W || ny < 0) continue;
      if (ny >= H) {   // off the top of the table: in the world
        spilledUnits_[s] += gasAmt_[k];
        NoteExit(x + 0.5f, gasAmt_[k]);
        gasAmt_[k] = 0;
        gasSub_[k] = 0xFF;
        break;
      }
      const int nk = ny * W + nx;
      if (wall_[nk]) continue;
      // Grains stop gas, except that a rising bubble can work its way up
      // through a bed now and then; a heavy gas never sinks into one.
      if (grid_[nk] && (heavy || cand[c][1] <= 0 || (Rand() & 7))) continue;
      const bool vertical = cand[c][1] != 0;
      if (!gasAmt_[nk]) {
        // Into a free pixel. A PUFF rises whole (a bubble always does), so a
        // wisp climbs as a wisp; a dense one leaves half behind as it goes,
        // and sideways a cloud only bleeds a third -- it spreads, slowly.
        const uint32_t a = gasAmt_[k];
        const uint32_t mv = inLiquid ? a : vertical ? (a > 24 ? (a + 1) / 2 : a) : (a >= 3 ? a / 3 : 0);
        if (!mv) continue;
        gasSub_[nk] = (uint8_t)s;
        gasAmt_[nk] = (uint16_t)mv;
        gasAge_[nk] = gasAge_[k];
        gasMark_[nk] = mark;
        AddGasPixel(nk);
        gasAmt_[k] = (uint16_t)(a - mv);
        if (!gasAmt_[k]) gasSub_[k] = 0xFF;
        break;
      }
      if (gasSub_[nk] == s) {
        // Into its own cloud: even out (with the buoyant side favoured).
        const uint32_t a = gasAmt_[k], b = gasAmt_[nk];
        uint32_t mv = a > b ? (a - b + (vertical ? 1 : 0)) / 2 : 0;
        if (inLiquid) mv = a;
        mv = std::min(mv, cap - std::min(cap, b));
        if (!mv) continue;
        gasAmt_[nk] = (uint16_t)(b + mv);
        gasAmt_[k] = (uint16_t)(a - mv);
        gasMark_[nk] = mark;
        if (!gasAmt_[k]) gasSub_[k] = 0xFF;
        break;
      }
      // Another gas: the lighter rises through the heavier (they trade places).
      if (vertical && ((vdir > 0 && subs_[gasSub_[nk]].density > S.density) ||
                       (vdir < 0 && subs_[gasSub_[nk]].density < S.density))) {
        std::swap(gasSub_[k], gasSub_[nk]);
        std::swap(gasAmt_[k], gasAmt_[nk]);
        std::swap(gasAge_[k], gasAge_[nk]);
        gasMark_[nk] = mark;
        break;
      }
    }
  }
}

// A MOVING VESSEL CARRIES ITS GAS: every cloud pixel inside it (judged in its
// previous pose) goes where the same point of the vessel is now. Without
// this the glass swept through a carried flask's headspace and a stoppered
// flask leaked its gas at every step.
void FlaskSim::CarryGas() {
  if (gasList_.empty()) return;
  const int W = cfg_.gridW, H = cfg_.gridH;
  struct Mv { int from, to; };
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
      const int nx = (int)std::floor(w.x), ny = (int)std::floor(w.y);
      if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
      const int nk = ny * W + nx;
      if (nk != k) moves.push_back({k, nk});
    }
  }
  if (moves.empty()) return;
  // Lift every moving pixel off the grid first, then set it down: a pixel
  // moving into one that is itself moving must not merge with it.
  struct Held { int to; uint8_t sub; uint16_t amt; uint8_t age; };
  std::vector<Held> held;
  held.reserve(moves.size());
  for (const Mv& m : moves) {
    if (!gasAmt_[m.from]) continue;
    held.push_back({m.to, gasSub_[m.from], gasAmt_[m.from], gasAge_[m.from]});
    gasAmt_[m.from] = 0;
    gasSub_[m.from] = 0xFF;
  }
  for (const Held& h : held) {
    uint32_t q = h.amt;
    if (!AddGasAt(h.sub, q, h.to % W, h.to / W)) AddPool(-1, h.sub, q, {(h.to % W) + 0.5f, (h.to / W) + 0.5f});
    gasAge_[h.to] = h.age;
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
      const int h = (int)(std::sqrt(u) * 20.0f * fl + 2);
      for (int dy = 0; dy < h; dy++) {
        const float f = (float)dy / std::max(1, h);
        int r, g, b, a;
        if (f < 0.3f) { r = 90; g = 150; b = 255; a = 200; }
        else if (f < 0.7f) { r = 255; g = 150; b = 40; a = 220; }
        else { r = 255; g = 230; b = 110; a = 170; }
        const int px = (int)bx + dx, py = (int)by + dy;
        if (px < 0 || py < 0 || px >= W || py >= H) continue;
        const size_t k = (size_t)py * W + px;
        if (inside_.size() == (size_t)W * H && inside_[k] == vi + 1 && !wall_[k]) continue;   // not inside the glass
        put(px, py, r, g, b, a);
      }
    }
  }

  // 3. THE GAS: a soft cloud in the material's own colours, alpha from how
  // much is there and the material's opacity, wisped by drifting noise. In
  // liquid it is bubbles.
  if (!gasList_.empty()) {
    int x0 = W, x1 = -1, y0 = H, y1 = -1;
    for (int k : gasList_) {
      if (!gasAmt_[k]) continue;
      const int x = k % W, y = k / W;
      x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
    }
    if (x1 >= x0) {
      // Each unit is splatted as a soft round puff (radius 3, weights summing
      // to one), so a thin trail of single units reads as a wisp of smoke
      // rather than as scattered dots.
      constexpr int kR = 3;
      static const std::array<float, (2 * kR + 1) * (2 * kR + 1)> kW = [] {
        std::array<float, (2 * kR + 1) * (2 * kR + 1)> w{};
        float sum = 0;
        for (int dy = -kR; dy <= kR; dy++)
          for (int dx = -kR; dx <= kR; dx++) {
            const float v = std::exp(-(dx * dx + dy * dy) / 3.5f);
            w[(dy + kR) * (2 * kR + 1) + dx + kR] = v;
            sum += v;
          }
        for (float& v : w) v /= sum;
        return w;
      }();
      x0 = std::max(0, x0 - kR); y0 = std::max(0, y0 - kR);
      x1 = std::min(W - 1, x1 + kR); y1 = std::min(H - 1, y1 + kR);
      const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
      rGas_.assign((size_t)bw * bh, 0.0f);
      rGasSub_.assign((size_t)bw * bh, 0xFF);
      std::vector<float> best((size_t)bw * bh, 0.0f);
      for (int k : gasList_) {
        if (!gasAmt_[k]) continue;
        const int x = k % W - x0, y = k / W - y0;
        const float a = (float)gasAmt_[k];
        for (int dy = -kR; dy <= kR; dy++)
          for (int dx = -kR; dx <= kR; dx++) {
            const int xx = x + dx, yy = y + dy;
            if (xx < 0 || yy < 0 || xx >= bw || yy >= bh) continue;
            const float w = kW[(dy + kR) * (2 * kR + 1) + dx + kR];
            const size_t j = (size_t)yy * bw + xx;
            rGas_[j] += a * w;
            if (a * w > best[j]) { best[j] = a * w; rGasSub_[j] = gasSub_[k]; }
          }
      }
      for (int yy = 0; yy < bh; yy++)
        for (int xx = 0; xx < bw; xx++) {
          const size_t j = (size_t)yy * bw + xx;
          if (rGas_[j] <= 0.02f || rGasSub_[j] == 0xFF) continue;
          const int x = xx + x0, y = yy + y0;
          const size_t k = (size_t)y * W + x;
          if (grid_[k]) continue;
          const Substance& S = subs_[rGasSub_[j]];
          const float op = std::max(0.02f, S.opacity / 255.0f);
          const float wisp = 0.55f + 0.45f * Noise2(x * 0.18f, y * 0.18f - t * 0.02f);
          float a = (1.0f - std::exp(-rGas_[j] * 7.0f * (0.3f + op))) * wisp;
          const float n2 = Noise2(x * 0.3f + 11.0f, y * 0.3f - t * 0.03f);
          const uint32_t c = S.color[n2 < 0.4f ? 1 : n2 < 0.75f ? 0 : 2];
          // Smoke is lit: a dark gas still reads against the dark desk as a
          // haze of its own hue (a third of the way to a pale grey).
          int r = Enc(R8(c)), g = Enc(G8(c)), b = Enc(B8(c));
          r += (170 - r) / 3; g += (175 - g) / 3; b += (170 - b) / 3;
          const float glow = S.emission / 255.0f;
          if (rTotal_.size() == (size_t)W * H && rTotal_[k] > 0.28f) {
            // Bubbles in liquid: a bright rim, only where the gas is.
            if (gasAmt_[k] && ((x + y + (int)(t * 0.25f)) & 1))
              put(x, y, r + (255 - r) / 2, g + (255 - g) / 2, b + (255 - b) / 2, 210);
            continue;
          }
          if (wall_[k]) a *= 0.5f;
          r += (int)((255 - r) * glow * 0.5f);
          g += (int)((255 - g) * glow * 0.5f);
          b += (int)((255 - b) * glow * 0.5f);
          put(x, y, r, g, b, (int)(std::min(0.92f, a + glow * 0.3f * std::min(1.0f, rGas_[j])) * 255));
        }
    }
  }

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

  // 5. THE STOPPER: a cork in the mouth, turned with the vessel.
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
