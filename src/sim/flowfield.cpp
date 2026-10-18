#include "sim/flowfield.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "gpu/rhi.h"
#include "sim/currentprim.h"
#include "sim/tuning.h"
#include "sim/worldmap.h"

namespace sandvox {
namespace {

int FloorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// SOR sweeps per block. The analytic boundary potential is also the starting
// guess, so the sweeps only have to carry the correction the SHAPE makes --
// the narrows, the bars, the lake outline -- across a 129-node domain.
constexpr int kFlowSweeps = 260;
constexpr float kFlowOmega = 1.88f;
// A lake (or sea) node farther than this from every inlet and outlet of its
// body is pinned to the body's analytic potential. Without the pin a big lake
// fed by one river has nothing for the inflow to leave by inside the block,
// and the solve would stagnate the river feeding it; with it the river's fan
// spreads into the lake and fades into the far field the analytic sum gives.
constexpr float kFlowAnchor = 320.0f;
// Two neighbouring nodes whose surfaces differ by more than this are not the
// same water (a pond beside a river, a pool above a bank) unless both were
// laid by the map (a weir step inside one river is still one river).
constexpr int kFlowSameWater = 2;
// Cells of arc at each end of a river that are pinned to its own potential
// (see the classification below).
constexpr float kFlowPinArc = 24.0f;

// The depth the solve sees is floored: a lip one cell deep (a weir crest, the
// sill where a river meets its lake) is real, but at one cell its h^(5/3)
// throttles the whole reach through a single node row and the h^(2/3) in the
// velocity cannot undo it -- a 3 m/s stripe at the 0.4 m/s river's mouth.
constexpr float kFlowMinDepth = 3.0f;
float Tr(float h) { return std::pow(std::max(h, kFlowMinDepth), 5.0f / 3.0f); }

// A velocity (cells/s) as the grids' i16, 1/kFlowVelQ cells/s.
uint32_t QVel(float v) {
  const float s = v * (float)kFlowVelQ;
  return (uint32_t)(uint16_t)(int16_t)std::clamp((int)std::lround(s), -32767, 32767);
}

}  // namespace

// ---------------------------------------------------------------------------
// THE NETWORK: per river its gradient G, discharge Q and potential constant C;
// per lake (and the sea) the analytic potential of its inlets and outlets.
// A pure function of (map, scale), built once per map.
struct WaterFlowField::Net {
  struct Term {
    float x, z, coef, r0;   // phi += coef * ln(max(r, r0) / r0)
  };
  struct Body {
    std::vector<Term> terms;
    float phi0 = 0.0f;
    int state = 0;   // 0 unresolved, 1 resolving, 2 done
  };
  struct River {
    bool ok = false;
    float G = 0, C = 0, Q = 0, coef = 0, r0 = 0;
    int src = 0, mouth = 0, maxHw = 1;
    int joinSample = -1;   // the parent's line sample nearest our mouth
    std::vector<float> arc;
    int x0 = 0, z0 = 0, x1 = 0, z1 = 0;   // line bbox, padded by maxHw
    int state = 0;
  };
  std::vector<River> rivers;
  std::vector<Body> bodies;   // lakes, then the sea at index lakes.size()
  int sea = 0;

  float PhiBody(int b, float x, float z) {
    Body& B = bodies[(size_t)b];
    float phi = Phi0(b);
    for (const Term& t : B.terms) {
      const float dx = x - t.x, dz = z - t.z;
      const float r = std::sqrt(dx * dx + dz * dz);
      phi += t.coef * std::log(std::max(r, t.r0) / t.r0);
    }
    return phi;
  }
  float Phi0(int b);
  float C(int r);
};

float WaterFlowField::Net::Phi0(int b) {
  Body& B = bodies[(size_t)b];
  if (B.state == 2) return B.phi0;
  if (B.state == 1) return 0.0f;   // a cycle in the map's routing: break it
  B.state = 1;
  float phi0 = 0.0f;
  // A lake's level is set by its OUTLET: the potential at the outlet's source
  // must equal that river's own potential there.
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  if (b < (int)m.lakes.size()) {
    for (size_t r = 0; r < rivers.size(); r++) {
      if (!rivers[r].ok || m.rivers[r].lakeIn != b) continue;
      const River& R = rivers[r];
      const auto& p = m.rivers[r].line[(size_t)R.src];
      const float atSrc = C((int)r) - R.G * R.arc[(size_t)R.src];
      float terms = 0.0f;
      for (const Term& t : B.terms) {
        const float dx = (float)p[0] - t.x, dz = (float)p[1] - t.z;
        const float rr = std::sqrt(dx * dx + dz * dz);
        terms += t.coef * std::log(std::max(rr, t.r0) / t.r0);
      }
      phi0 = atSrc - terms;
      break;
    }
  }
  B.phi0 = phi0;
  B.state = 2;
  return phi0;
}

float WaterFlowField::Net::C(int r) {
  River& R = rivers[(size_t)r];
  if (R.state == 2) return R.C;
  if (R.state == 1) return 0.0f;
  R.state = 1;
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  const worldmap::WorldMapData::River& rv = m.rivers[(size_t)r];
  const auto& mp = rv.line[(size_t)R.mouth];
  float down = 0.0f;
  if (rv.lakeOut >= 0) {
    down = PhiBody(rv.lakeOut, (float)mp[0], (float)mp[1]);
  } else if (rv.joinIdx >= 0 && rivers[(size_t)rv.joinIdx].ok && R.joinSample >= 0) {
    const River& P = rivers[(size_t)rv.joinIdx];
    down = C(rv.joinIdx) - P.G * P.arc[(size_t)R.joinSample];
  } else {
    down = PhiBody(sea, (float)mp[0], (float)mp[1]);
  }
  R.C = down + R.G * R.arc[(size_t)R.mouth];
  R.state = 2;
  return R.C;
}

namespace {

std::shared_ptr<WaterFlowField::Net> BuildNet(float scale) {
  auto net = std::make_shared<WaterFlowField::Net>();
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  net->sea = (int)m.lakes.size();
  net->bodies.resize(m.lakes.size() + 1);
  net->rivers.resize(m.rivers.size());
  for (size_t r = 0; r < m.rivers.size(); r++) {
    const auto& rv = m.rivers[r];
    auto& R = net->rivers[r];
    const auto& L = rv.line;
    if (L.size() < 2) continue;
    R.arc.assign(L.size(), 0.0f);
    for (size_t i = 1; i < L.size(); i++) {
      const float ex = (float)(L[i][0] - L[i - 1][0]), ez = (float)(L[i][1] - L[i - 1][1]);
      R.arc[i] = R.arc[i - 1] + std::sqrt(ex * ex + ez * ez);
    }
    // The river proper runs between the lakes it leaves and enters: the baked
    // line starts and ends INSIDE them.
    int src = 0, mouth = (int)L.size() - 1;
    if (rv.lakeIn >= 0) {
      const auto& lk = m.lakes[(size_t)rv.lakeIn];
      while (src + 1 < mouth && lk.Inside(L[(size_t)src][0], L[(size_t)src][1])) src++;
    }
    if (rv.lakeOut >= 0) {
      const auto& lk = m.lakes[(size_t)rv.lakeOut];
      while (mouth - 1 > src && lk.Inside(L[(size_t)mouth][0], L[(size_t)mouth][1])) mouth--;
    }
    R.src = src;
    R.mouth = mouth;
    double hwSum = 0.0;
    int maxHw = 1;
    for (int i = src; i <= mouth; i++) {
      hwSum += L[(size_t)i][3];
      maxHw = std::max(maxHw, L[(size_t)i][3]);
    }
    for (const auto& p : L) maxHw = std::max(maxHw, p[3]);
    const float hwMean = (float)(hwSum / (double)(mouth - src + 1));
    double dSum = 0.0;
    int dN = 0;
    for (const auto& k : rv.knots)
      if (k[3] > 0) { dSum += k[3]; dN++; }
    const float dMean = dN > 0 ? (float)(dSum / dN) : 20.0f;
    // The U profile's mean depth is two thirds of its centre depth.
    const float hRef = std::max(2.0f, 0.67f * dMean);
    const float run = R.arc[(size_t)mouth] - R.arc[(size_t)src];
    const float fall = (float)(L[(size_t)src][2] - L[(size_t)mouth][2]);
    const float slope = run > 1.0f ? std::max(0.0f, fall / run) : 0.0f;
    // Chezy on the river's own fall and depth, floored: a river the map
    // authored flows even down a reach the bake laid flat (most of them).
    const float vms = std::clamp(scale * std::sqrt(slope * hRef * kVoxelMeters),
                                 kRiverMinFlowMs, kRiverMaxFlowMs);
    const float vC = vms / kVoxelMeters;
    // v = h^(2/3) |grad phi|, so the reference depth flows at exactly vC.
    R.G = vC / std::pow(hRef, 2.0f / 3.0f);
    const float Q = vC * 2.0f * hwMean * hRef;
    R.Q = Q;
    R.coef = Q / (3.14159265f * Tr(hRef));
    R.r0 = std::max(hwMean, 4.0f);
    R.maxHw = maxHw;
    int x0 = INT32_MAX, z0 = INT32_MAX, x1 = INT32_MIN, z1 = INT32_MIN;
    for (const auto& p : L) {
      x0 = std::min(x0, p[0]); x1 = std::max(x1, p[0]);
      z0 = std::min(z0, p[1]); z1 = std::max(z1, p[1]);
    }
    R.x0 = x0 - maxHw; R.x1 = x1 + maxHw;
    R.z0 = z0 - maxHw; R.z1 = z1 + maxHw;
    R.ok = scale > 0.0f;
  }
  // Inlets are sources of their mouth's body, outlets sinks of their source's;
  // a tributary's mouth is its parent's channel, not a body.
  for (size_t r = 0; r < m.rivers.size(); r++) {
    auto& R = net->rivers[r];
    if (!R.ok) continue;
    const auto& rv = m.rivers[r];
    const auto& mp = rv.line[(size_t)R.mouth];
    const auto& sp = rv.line[(size_t)R.src];
    if (rv.lakeOut >= 0) {
      net->bodies[(size_t)rv.lakeOut].terms.push_back({(float)mp[0], (float)mp[1], -R.coef, R.r0});
    } else if (rv.joinIdx >= 0 && net->rivers[(size_t)rv.joinIdx].ok) {
      const auto& PL = m.rivers[(size_t)rv.joinIdx].line;
      int64_t best = INT64_MAX;
      for (size_t k = 0; k < PL.size(); k++) {
        const int64_t dx = PL[k][0] - mp[0], dz = PL[k][1] - mp[1];
        if (dx * dx + dz * dz < best) { best = dx * dx + dz * dz; R.joinSample = (int)k; }
      }
    } else {
      net->bodies[(size_t)net->sea].terms.push_back({(float)mp[0], (float)mp[1], -R.coef, R.r0});
    }
    if (rv.lakeIn >= 0)
      net->bodies[(size_t)rv.lakeIn].terms.push_back({(float)sp[0], (float)sp[1], R.coef, R.r0});
  }
  for (size_t r = 0; r < net->rivers.size(); r++)
    if (net->rivers[r].ok) net->C((int)r);
  for (size_t b = 0; b < net->bodies.size(); b++) net->Phi0((int)b);
  return net;
}

}  // namespace

// ---------------------------------------------------------------------------
// ONE BLOCK: probe, classify, relax, differentiate.
struct WaterFlowField::Block {
  std::vector<float> vx, vz;
  std::vector<int16_t> surf, bed;   // surf == kFlowDry: land
};

namespace {

std::shared_ptr<WaterFlowField::Block> SolveBlock(WaterFlowField::Net& net, int bx, int bz,
                                                  uint32_t seed) {
  constexpr int N = kFlowBlockNodes;
  const int x0 = bx * kFlowBlock + kFlowBlock / 2 - kFlowBlock;
  const int z0 = bz * kFlowBlock + kFlowBlock / 2 - kFlowBlock;
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  const int seaY = m.seaLevelY;
  auto blk = std::make_shared<WaterFlowField::Block>();
  blk->vx.assign((size_t)N * N, 0.0f);
  blk->vz.assign((size_t)N * N, 0.0f);
  blk->surf.assign((size_t)N * N, (int16_t)kFlowDry);
  blk->bed.assign((size_t)N * N, 0);

  // 1. PROBE: the column as worldgen composes it. Land is kFlowDry.
  std::vector<float> depth((size_t)N * N, 0.0f);
  std::vector<uint8_t> wet((size_t)N * N, 0);
  for (int j = 0; j < N; j++)
    for (int i = 0; i < N; i++) {
      const int x = x0 + i * kFlowStep, z = z0 + j * kFlowStep;
      const worldmap::WaterColumn wc = worldmap::WaterColumnAt(x, z, seed);
      int s = kFlowDry;
      if (wc.water > wc.h) s = wc.water;
      else if (wc.h < seaY) s = seaY;
      if (s == kFlowDry) continue;
      const size_t k = (size_t)j * N + i;
      blk->surf[k] = (int16_t)std::clamp(s, -32767, 32767);
      blk->bed[k] = (int16_t)std::clamp(wc.h, -32767, 32767);
      depth[k] = (float)std::max(s - wc.h, 1);
      wet[k] = wc.wet ? 1 : 0;
    }

  // 2. CLASSIFY + the analytic starting potential. A river sample bucket over
  // the domain, so the nearest-centreline query is local.
  constexpr int kBk = 64;
  const int span = (N - 1) * kFlowStep;
  const int nb = span / kBk + 1;
  struct Smp {
    int r, k;
  };
  std::vector<std::vector<Smp>> bucket((size_t)nb * nb);
  int reachHw = 1;
  for (size_t r = 0; r < net.rivers.size(); r++) {
    const auto& R = net.rivers[r];
    if (!R.ok || R.x1 < x0 || R.x0 > x0 + span || R.z1 < z0 || R.z0 > z0 + span) continue;
    const auto& L = m.rivers[r].line;
    for (int k = R.src; k <= R.mouth; k++) {
      const int px = L[(size_t)k][0] - x0, pz = L[(size_t)k][1] - z0;
      const int hw = L[(size_t)k][3];
      if (px < -hw || pz < -hw || px > span + hw || pz > span + hw) continue;
      const int bi = std::clamp(FloorDiv(px, kBk), 0, nb - 1);
      const int bj = std::clamp(FloorDiv(pz, kBk), 0, nb - 1);
      bucket[(size_t)bj * nb + bi].push_back({(int)r, k});
      reachHw = std::max(reachHw, hw);
    }
  }
  const int bR = (reachHw + 8) / kBk + 1;
  std::vector<int> lakeHere;
  for (size_t l = 0; l < m.lakes.size(); l++) {
    const auto& lk = m.lakes[l];
    if (lk.x1 < x0 || lk.x0 > x0 + span || lk.z1 < z0 || lk.z0 > z0 + span) continue;
    lakeHere.push_back((int)l);
  }

  std::vector<float> phi((size_t)N * N, 0.0f);
  std::vector<uint8_t> fixed((size_t)N * N, 0);
  // Per node: the river it was classified into (-1 = none) and its arc.
  std::vector<int> rivOf((size_t)N * N, -1);
  std::vector<int> bodyOf((size_t)N * N, -1);
  std::vector<float> arcOf((size_t)N * N, 0.0f);
  for (int j = 0; j < N; j++)
    for (int i = 0; i < N; i++) {
      const size_t k = (size_t)j * N + i;
      if (blk->surf[k] == kFlowDry) continue;
      const int x = x0 + i * kFlowStep, z = z0 + j * kFlowStep;
      const bool ring = i == 0 || j == 0 || i == N - 1 || j == N - 1;
      int body = -1;
      for (int l : lakeHere)
        if (m.lakes[(size_t)l].Inside(x, z)) { body = l; break; }
      if (body < 0 && !wet[k] && blk->bed[k] < seaY) body = net.sea;
      if (body < 0) {
        // The nearest river sample, among those whose channel covers us.
        int64_t best = INT64_MAX;
        int br = -1, bk = -1;
        const int ci = std::clamp(FloorDiv(x - x0, kBk), 0, nb - 1);
        const int cj = std::clamp(FloorDiv(z - z0, kBk), 0, nb - 1);
        for (int jj = std::max(cj - bR, 0); jj <= std::min(cj + bR, nb - 1); jj++)
          for (int ii = std::max(ci - bR, 0); ii <= std::min(ci + bR, nb - 1); ii++)
            for (const Smp& s : bucket[(size_t)jj * nb + ii]) {
              const auto& p = m.rivers[(size_t)s.r].line[(size_t)s.k];
              const int64_t dx = p[0] - x, dz = p[1] - z;
              const int64_t d2 = dx * dx + dz * dz;
              const int64_t lim = (int64_t)(p[3] + 8) * (p[3] + 8);
              if (d2 <= lim && d2 < best) { best = d2; br = s.r; bk = s.k; }
            }
        if (br >= 0) {
          const auto& R = net.rivers[(size_t)br];
          const float s = R.arc[(size_t)bk];
          phi[k] = R.C - R.G * s;
          rivOf[k] = br;
          arcOf[k] = s;
        }
        // else: a pond, or water the map does not route. Starts at 0 and is
        // never pinned, so unless the solve connects it to a river it stays
        // uniform -- still.
        continue;
      }
      phi[k] = net.PhiBody(body, (float)x, (float)z);
      bodyOf[k] = body;
      float dMin = 1e30f;
      for (const auto& t : net.bodies[(size_t)body].terms) {
        const float dx = (float)x - t.x, dz = (float)z - t.z;
        dMin = std::min(dMin, std::sqrt(dx * dx + dz * dz));
      }
      fixed[k] = (ring || dMin > kFlowAnchor) ? 1 : 0;
    }

  // 3. RELAX. Conductance between neighbours is the harmonic mean of their
  // transmissivities, so a node beside the bank (shallow, T small) passes
  // little and the bank itself (no water) passes nothing: no-flux walls.
  std::vector<float> kE((size_t)N * N, 0.0f), kS((size_t)N * N, 0.0f);
  auto conn = [&](size_t a, size_t b) {
    if (blk->surf[a] == kFlowDry || blk->surf[b] == kFlowDry) return 0.0f;
    if (std::abs(blk->surf[a] - blk->surf[b]) > kFlowSameWater && !(wet[a] && wet[b])) return 0.0f;
    const float ta = Tr(depth[a]), tb = Tr(depth[b]);
    return 2.0f * ta * tb / (ta + tb);
  };
  for (int j = 0; j < N; j++)
    for (int i = 0; i < N; i++) {
      const size_t k = (size_t)j * N + i;
      if (i + 1 < N) kE[k] = conn(k, k + 1);
      if (j + 1 < N) kS[k] = conn(k, k + N);
    }
  // THE RIVERS' BOUNDARY CONDITIONS, per connected piece of water. A river's
  // water inside the domain is not always one piece: its head is a spring on
  // dry land, a fall's lip bakes a dry row across the channel, it leaves the
  // domain and comes back. In every piece, for every river in it:
  //   * its UPSTREAM end (the most-upstream kFlowPinArc of it) takes the
  //     river's discharge Q as an INFLOW, shared by transmissivity -- unless it
  //     is the river's true source and the lake that feeds it is in the piece;
  //   * its DOWNSTREAM end is PINNED to the river's own potential -- unless it
  //     is the true mouth and what it drains into (its lake, the sea, its
  //     parent river) is in the piece, where the water simply carries on.
  // Discharge, not potential, is what is prescribed: a river's flow is set by
  // its catchment, not by how hard its channel is to push through, so a deep
  // pool runs slow and the lip of a weir runs fast with the same Q -- which a
  // potential drop pinned at both ends got backwards (the shallow lip throttled
  // the whole reach).
  std::vector<float> src((size_t)N * N, 0.0f);
  {
    std::vector<int> comp((size_t)N * N, -1);
    std::vector<uint32_t> stack, members;
    int nComp = 0;
    for (size_t k0 = 0; k0 < comp.size(); k0++) {
      if (blk->surf[k0] == kFlowDry || comp[k0] >= 0) continue;
      members.clear();
      stack.assign(1, (uint32_t)k0);
      comp[k0] = nComp;
      while (!stack.empty()) {
        const uint32_t k = stack.back();
        stack.pop_back();
        members.push_back(k);
        const int i = (int)(k % N);
        auto push = [&](size_t q) {
          if (comp[q] < 0) { comp[q] = nComp; stack.push_back((uint32_t)q); }
        };
        if (i + 1 < N && kE[k] > 0.0f) push(k + 1);
        if (i > 0 && kE[k - 1] > 0.0f) push(k - 1);
        if (k + N < (size_t)N * N && kS[k] > 0.0f) push(k + N);
        if (k >= (uint32_t)N && kS[k - N] > 0.0f) push(k - N);
      }
      nComp++;
      std::map<int, std::pair<float, float>> span;
      std::vector<int> bodies;
      bool anyFixed = false;
      for (uint32_t k : members) {
        anyFixed = anyFixed || fixed[k];
        if (bodyOf[k] >= 0 && std::find(bodies.begin(), bodies.end(), bodyOf[k]) == bodies.end())
          bodies.push_back(bodyOf[k]);
        if (rivOf[k] < 0) continue;
        auto it = span.find(rivOf[k]);
        if (it == span.end()) span[rivOf[k]] = {arcOf[k], arcOf[k]};
        else {
          it->second.first = std::min(it->second.first, arcOf[k]);
          it->second.second = std::max(it->second.second, arcOf[k]);
        }
      }
      auto has = [&](int b) { return std::find(bodies.begin(), bodies.end(), b) != bodies.end(); };
      bool anySource = false;
      for (const auto& [r, sp] : span) {
        const auto& R = net.rivers[(size_t)r];
        const auto& rv = m.rivers[(size_t)r];
        const bool trueSrc = sp.first - R.arc[(size_t)R.src] < kFlowPinArc;
        const bool trueMouth = R.arc[(size_t)R.mouth] - sp.second < kFlowPinArc;
        const bool fed = trueSrc && rv.lakeIn >= 0 && has(rv.lakeIn);
        bool drains = false;
        if (trueMouth) {
          if (rv.lakeOut >= 0) drains = has(rv.lakeOut);
          else if (rv.joinIdx >= 0) drains = span.count(rv.joinIdx) > 0;
          else drains = has(net.sea);
        }
        float tUp = 0.0f;
        for (uint32_t k : members) {
          if (rivOf[k] != r) continue;
          if (!drains && arcOf[k] >= sp.second - kFlowPinArc) { fixed[k] = 1; anyFixed = true; }
          else if (!fed && arcOf[k] <= sp.first + kFlowPinArc) tUp += Tr(depth[k]);
        }
        if (fed || tUp <= 0.0f) continue;
        for (uint32_t k : members)
          if (rivOf[k] == r && !fixed[k] && arcOf[k] <= sp.first + kFlowPinArc) {
            src[k] += R.Q * Tr(depth[k]) / tUp;
            anySource = true;
          }
      }
      // Water that comes in must leave: a piece with an inflow and no pinned
      // node anywhere (a terminal lake, a river ending in a pool the map does
      // not route) pins every river's downstream end after all.
      if (anySource && !anyFixed)
        for (uint32_t k : members) {
          if (rivOf[k] < 0) continue;
          const auto& sp = span[rivOf[k]];
          if (arcOf[k] >= sp.second - kFlowPinArc) { fixed[k] = 1; src[k] = 0.0f; }
        }
    }
  }

  std::vector<uint32_t> freeNodes;
  for (int j = 0; j < N; j++)
    for (int i = 0; i < N; i++) {
      const size_t k = (size_t)j * N + i;
      if (blk->surf[k] != kFlowDry && !fixed[k]) freeNodes.push_back((uint32_t)k);
    }
  for (int it = 0; it < kFlowSweeps; it++) {
    for (uint32_t k : freeNodes) {
      const int i = (int)(k % N);
      float sk = 0.0f, sp = 0.0f;
      if (i + 1 < N && kE[k] > 0.0f) { sk += kE[k]; sp += kE[k] * phi[k + 1]; }
      if (i > 0 && kE[k - 1] > 0.0f) { sk += kE[k - 1]; sp += kE[k - 1] * phi[k - 1]; }
      if (k + N < (size_t)N * N && kS[k] > 0.0f) { sk += kS[k]; sp += kS[k] * phi[k + N]; }
      if (k >= (uint32_t)N && kS[k - N] > 0.0f) { sk += kS[k - N]; sp += kS[k - N] * phi[k - N]; }
      if (sk <= 0.0f) continue;
      phi[k] += kFlowOmega * ((sp + src[k]) / sk - phi[k]);
    }
  }

  // 4. VELOCITY: v = -h^(2/3) grad(phi), across connected neighbours only
  // (one-sided at a bank), capped where a weir's potential step would read as
  // a jet.
  const float vCap = kRiverMaxFlowMs / kVoxelMeters;
  for (int j = 0; j < N; j++)
    for (int i = 0; i < N; i++) {
      const size_t k = (size_t)j * N + i;
      if (blk->surf[k] == kFlowDry) continue;
      auto grad = [&](bool hasP, bool hasM, float pP, float pM) {
        if (hasP && hasM) return (pP - pM) / (2.0f * kFlowStep);
        if (hasP) return (pP - phi[k]) / (float)kFlowStep;
        if (hasM) return (phi[k] - pM) / (float)kFlowStep;
        return 0.0f;
      };
      const bool e = i + 1 < N && kE[k] > 0.0f, w = i > 0 && kE[k - 1] > 0.0f;
      const bool s = j + 1 < N && kS[k] > 0.0f, n = j > 0 && kS[k - N] > 0.0f;
      const float gx = grad(e, w, e ? phi[k + 1] : 0.0f, w ? phi[k - 1] : 0.0f);
      const float gz = grad(s, n, s ? phi[k + N] : 0.0f, n ? phi[k - N] : 0.0f);
      const float h23 = std::pow(std::max(depth[k], kFlowMinDepth), 2.0f / 3.0f);
      float vx = -h23 * gx, vz = -h23 * gz;
      const float sp = std::sqrt(vx * vx + vz * vz);
      if (sp > vCap) { vx *= vCap / sp; vz *= vCap / sp; }
      blk->vx[k] = vx;
      blk->vz[k] = vz;
    }
  return blk;
}

}  // namespace

const WaterFlowField::Block& WaterFlowField::BlockAt(int bx, int bz, uint32_t seed) {
  auto it = blocks_.find({bx, bz});
  if (it != blocks_.end()) return *it->second;
  for (auto* pend : {&pending_, &farPending_}) {
    auto pit = pend->find({bx, bz});
    if (pit != pend->end()) {
      auto b = pit->second.get();
      pend->erase(pit);
      return *blocks_.emplace(std::make_pair(bx, bz), std::move(b)).first->second;
    }
  }
  const auto t0 = std::chrono::steady_clock::now();
  auto b = SolveBlock(*net_, bx, bz, seed);
  solved_++;
  if (std::getenv("SANDVOX_FLOW_TRACE")) {
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    int wetN = 0;
    for (int16_t s : b->surf) wetN += s != kFlowDry;
    std::fprintf(stderr, "flow: block (%d,%d) solved in %.1f ms, %d water nodes\n", bx, bz, ms, wetN);
  }
  return *blocks_.emplace(std::make_pair(bx, bz), std::move(b)).first->second;
}

size_t WaterFlowField::Harvest() {
  size_t landed = 0;
  for (auto* pend : {&pending_, &farPending_}) {
    for (auto it = pend->begin(); it != pend->end();) {
      if (it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        blocks_[it->first] = it->second.get();
        it = pend->erase(it);
        landed++;
      } else {
        ++it;
      }
    }
  }
  return landed;
}

void WaterFlowField::Quiesce() {
  for (auto* pend : {&pending_, &farPending_}) {
    for (auto& [key, f] : *pend) blocks_[key] = f.get();
    pend->clear();
  }
}

void WaterFlowField::Prefetch(int ox, int oz, uint32_t seed) {
  constexpr size_t kFlowMaxPending = 2;
  const int far = ox + (kFlowWinNodes - 1) * kFlowStep;
  const int farZ = oz + (kFlowWinNodes - 1) * kFlowStep;
  const int h = kFlowBlock / 2, c = kFlowBlock / 2;
  const int bx0 = FloorDiv(ox - h - c, kFlowBlock), bx1 = FloorDiv(far + h - c, kFlowBlock) + 1;
  const int bz0 = FloorDiv(oz - h - c, kFlowBlock), bz1 = FloorDiv(farZ + h - c, kFlowBlock) + 1;
  for (int bz = bz0; bz <= bz1; bz++)
    for (int bx = bx0; bx <= bx1; bx++) {
      if (pending_.size() >= kFlowMaxPending) return;
      if (blocks_.count({bx, bz}) || pending_.count({bx, bz}) || farPending_.count({bx, bz}))
        continue;
      std::shared_ptr<Net> net = net_;
      pending_.emplace(std::make_pair(bx, bz),
                       std::async(std::launch::async, [net, bx, bz, seed] {
                         return SolveBlock(*net, bx, bz, seed);
                       }));
    }
}

void WaterFlowField::Clear() {
  Quiesce();
  words_.clear();
  blocks_.clear();
  net_.reset();
  built_ = false;
  farBuilt_ = false;
  farMissing_ = 0;
  farLanded_ = 0;
  version_++;
  farVersion_++;
  solved_ = 0;
  scale_ = -1.0f;
}

void WaterFlowField::Update(IVec3 originChunks, uint32_t seed) {
  UpdateAt(originChunks.x * (int)kChunk, originChunks.z * (int)kChunk, seed);
}

void WaterFlowField::UpdateAt(int baseX, int baseZ, uint32_t seed) {
  const float scale = CurrentTuning().sim.currentStreamScale;
  const uint32_t mapHash = worldmap::CurrentWorldMap().contentHash;
  const bool netStale = !net_ || mapHash != mapHash_ || scale != scale_ || seed != seed_;
  farLanded_ += Harvest();
  if (netStale) {
    Quiesce();
    blocks_.clear();
    net_ = BuildNet(scale);
    mapHash_ = mapHash;
    scale_ = scale;
    seed_ = seed;
    built_ = false;
    farBuilt_ = false;
  }
  const int ox = baseX - kFlowStep, oz = baseZ - kFlowStep;
  const int far = ox + (kFlowWinNodes - 1) * kFlowStep;
  const int farZ = oz + (kFlowWinNodes - 1) * kFlowStep;
  (void)far;
  (void)farZ;
  if (built_ && baseX == baseX_ && baseZ == baseZ_) {
    // Holding still: keep the background solves of the ring the window
    // would need half a block from here going, so the move finds them.
    Prefetch(ox, oz, seed);
    UpdateFar(seed);
    return;
  }
  baseX_ = baseX;
  baseZ_ = baseZ;
  built_ = true;

  // Evict what the window has left well behind. The far grid's blocks reach
  // kFlowFarHalf + kFlowFarSnap + one tent block past the window's centre:
  // kEvictBlocks keeps all of them (and evicts nothing the window needs).
  constexpr int kEvictBlocks = (kFlowFarHalf + kFlowFarSnap) / kFlowBlock + 2;
  static_assert(kEvictBlocks >= 3, "the window's own ring needs +-3");
  const int cbx = FloorDiv(baseX + (int)kWorldN / 2, kFlowBlock);
  const int cbz = FloorDiv(baseZ + (int)kWorldN / 2, kFlowBlock);
  for (auto it = blocks_.begin(); it != blocks_.end();) {
    if (std::abs(it->first.first - cbx) > kEvictBlocks ||
        std::abs(it->first.second - cbz) > kEvictBlocks)
      it = blocks_.erase(it);
    else
      ++it;
  }
  Prefetch(ox, oz, seed);

  // The window's part only: the far grid (kFlowFarBase on) is UpdateFar's.
  if (words_.size() != kFlowWords) words_.assign(kFlowWords, 0u);
  std::fill(words_.begin(), words_.begin() + kFlowNearWords, 0u);
  words_[0] = (uint32_t)ox;
  words_[1] = (uint32_t)oz;
  words_[2] = (uint32_t)kFlowWinNodes;
  constexpr int NB = kFlowBlockNodes;
  for (int j = 0; j < kFlowWinNodes; j++) {
    const int z = oz + j * kFlowStep;
    const int bz = FloorDiv(z - kFlowBlock / 2, kFlowBlock);
    const int cz = bz * kFlowBlock + kFlowBlock / 2;
    const float tz = (float)(z - cz) / (float)kFlowBlock;
    for (int i = 0; i < kFlowWinNodes; i++) {
      const int x = ox + i * kFlowStep;
      const int bx = FloorDiv(x - kFlowBlock / 2, kFlowBlock);
      const int cx = bx * kFlowBlock + kFlowBlock / 2;
      const float tx = (float)(x - cx) / (float)kFlowBlock;
      float vx = 0.0f, vz = 0.0f;
      int surf = kFlowDry, bed = 0;
      // The tent blend of the (up to) four blocks whose centres surround the
      // node. A zero weight never asks for its block, so a window does not
      // solve blocks it cannot see.
      for (int dz = 0; dz < 2; dz++)
        for (int dx = 0; dx < 2; dx++) {
          const float w = (dx ? tx : 1.0f - tx) * (dz ? tz : 1.0f - tz);
          if (w <= 0.0f) continue;
          const Block& b = BlockAt(bx + dx, bz + dz, seed);
          const int ccx = cx + dx * kFlowBlock, ccz = cz + dz * kFlowBlock;
          const int ii = (x - (ccx - kFlowBlock)) / kFlowStep;
          const int jj = (z - (ccz - kFlowBlock)) / kFlowStep;
          const size_t k = (size_t)jj * NB + ii;
          vx += w * b.vx[k];
          vz += w * b.vz[k];
          surf = b.surf[k];   // the same probe in every block
          bed = b.bed[k];
        }
      uint32_t* n = &words_[kFlowHeaderWords + 2 * ((size_t)j * kFlowWinNodes + i)];
      n[0] = surf == kFlowDry ? 0u : (QVel(vx) | (QVel(vz) << 16));
      n[1] = (uint32_t)(uint16_t)(int16_t)surf | ((uint32_t)(uint16_t)(int16_t)bed << 16);
    }
  }
  version_++;
  // SANDVOX_FLOW_DUMP=<file>: the composed words, raw, for scripts/flow_view.py.
  if (const char* dp = std::getenv("SANDVOX_FLOW_DUMP")) {
    char name[512];
    std::snprintf(name, sizeof name, "%s_%d_%d.bin", dp, baseX, baseZ);
    if (FILE* f = std::fopen(name, "wb")) {
      std::fwrite(words_.data(), 4, kFlowNearWords, f);
      std::fclose(f);
    }
  }
  if (std::getenv("SANDVOX_FLOW_TRACE"))
    std::fprintf(stderr, "flow: window (%d,%d) composed, %zu blocks cached, %u solved\n", baseX,
                 baseZ, blocks_.size(), solved_);
  UpdateFar(seed);
}

// THE FAR GRID (flowfield.h kFlowFarBase). Recentred on the window when it has
// moved more than kFlowFarSnap cells (hysteresis: a window stepping back and
// forth over one chunk does not re-upload the grid each time); re-composed
// when blocks it composed dry have landed, at most every kFarRecomposeTicks
// updates so the startup fill is a handful of uploads, not one per block.
void WaterFlowField::UpdateFar(uint32_t seed) {
  constexpr uint32_t kFarRecomposeTicks = 8;
  const int wcx = baseX_ + (int)kWorldN / 2, wcz = baseZ_ + (int)kWorldN / 2;
  const bool recentre = !farBuilt_ || std::abs(wcx - farCx_) > kFlowFarSnap ||
                        std::abs(wcz - farCz_) > kFlowFarSnap;
  if (recentre) {
    farCx_ = FloorDiv(wcx + kFlowFarSnap / 2, kFlowFarSnap) * kFlowFarSnap;
    farCz_ = FloorDiv(wcz + kFlowFarSnap / 2, kFlowFarSnap) * kFlowFarSnap;
  }
  farLanded_ += Harvest();
  farTicks_++;
  if (recentre || (farMissing_ > 0 && farLanded_ > 0 && farTicks_ >= kFarRecomposeTicks))
    ComposeFar();
  if (farMissing_ == 0 || farPending_.size() >= kFlowFarPending) return;
  // The blocks the far grid lacks, nearest its centre first: the river under
  // the camera fills in before the one on the horizon.
  const int h = kFlowBlock / 2;
  const int bx0 = FloorDiv(farCx_ - kFlowFarHalf - h, kFlowBlock);
  const int bx1 = FloorDiv(farCx_ + kFlowFarHalf - h, kFlowBlock) + 1;
  const int bz0 = FloorDiv(farCz_ - kFlowFarHalf - h, kFlowBlock);
  const int bz1 = FloorDiv(farCz_ + kFlowFarHalf - h, kFlowBlock) + 1;
  std::vector<std::pair<long long, std::pair<int, int>>> want;
  for (int bz = bz0; bz <= bz1; bz++)
    for (int bx = bx0; bx <= bx1; bx++) {
      const std::pair<int, int> key{bx, bz};
      if (blocks_.count(key) || pending_.count(key) || farPending_.count(key)) continue;
      const long long dx = (long long)bx * kFlowBlock + h - farCx_;
      const long long dz = (long long)bz * kFlowBlock + h - farCz_;
      want.push_back({dx * dx + dz * dz, key});
    }
  std::sort(want.begin(), want.end());
  for (const auto& w : want) {
    if (farPending_.size() >= kFlowFarPending) break;
    std::shared_ptr<Net> net = net_;
    const int bx = w.second.first, bz = w.second.second;
    farPending_.emplace(w.second, std::async(std::launch::async, [net, bx, bz, seed] {
                          return SolveBlock(*net, bx, bz, seed);
                        }));
  }
}

// The window composition's arithmetic over the far lattice, from the cached
// blocks only: a node whose blend wants a block that is not here yet is
// composed dry and counted in farMissing_.
void WaterFlowField::ComposeFar() {
  if (words_.size() != kFlowWords) words_.assign(kFlowWords, 0u);
  const int ox = farCx_ - kFlowFarHalf, oz = farCz_ - kFlowFarHalf;
  uint32_t* hdr = &words_[kFlowFarBase];
  hdr[0] = (uint32_t)ox;
  hdr[1] = (uint32_t)oz;
  hdr[2] = (uint32_t)kFlowFarNodes;
  hdr[3] = (uint32_t)kFlowFarStep;
  // The blocks the lattice can ask for, looked up once.
  const int h = kFlowBlock / 2;
  const int bx0 = FloorDiv(ox - h, kFlowBlock), bz0 = FloorDiv(oz - h, kFlowBlock);
  const int nbx = FloorDiv(ox + 2 * kFlowFarHalf - h, kFlowBlock) + 2 - bx0;
  const int nbz = FloorDiv(oz + 2 * kFlowFarHalf - h, kFlowBlock) + 2 - bz0;
  std::vector<const Block*> blk((size_t)nbx * nbz, nullptr);
  for (int j = 0; j < nbz; j++)
    for (int i = 0; i < nbx; i++) {
      auto it = blocks_.find({bx0 + i, bz0 + j});
      if (it != blocks_.end()) blk[(size_t)j * nbx + i] = it->second.get();
    }
  constexpr int NB = kFlowBlockNodes;
  uint32_t missing = 0;
  for (int j = 0; j < kFlowFarNodes; j++) {
    const int z = oz + j * kFlowFarStep;
    const int bz = FloorDiv(z - h, kFlowBlock);
    const int cz = bz * kFlowBlock + h;
    const float tz = (float)(z - cz) / (float)kFlowBlock;
    for (int i = 0; i < kFlowFarNodes; i++) {
      const int x = ox + i * kFlowFarStep;
      const int bx = FloorDiv(x - h, kFlowBlock);
      const int cx = bx * kFlowBlock + h;
      const float tx = (float)(x - cx) / (float)kFlowBlock;
      float vx = 0.0f, vz = 0.0f;
      int surf = kFlowDry, bed = 0;
      bool ok = true;
      for (int dz = 0; dz < 2 && ok; dz++)
        for (int dx = 0; dx < 2; dx++) {
          const float w = (dx ? tx : 1.0f - tx) * (dz ? tz : 1.0f - tz);
          if (w <= 0.0f) continue;
          const Block* b = blk[(size_t)(bz + dz - bz0) * nbx + (bx + dx - bx0)];
          if (b == nullptr) {
            ok = false;
            break;
          }
          const int ccx = cx + dx * kFlowBlock, ccz = cz + dz * kFlowBlock;
          const int ii = (x - (ccx - kFlowBlock)) / kFlowStep;
          const int jj = (z - (ccz - kFlowBlock)) / kFlowStep;
          const size_t k = (size_t)jj * NB + ii;
          vx += w * b->vx[k];
          vz += w * b->vz[k];
          surf = b->surf[k];   // the same probe in every block
          bed = b->bed[k];
        }
      if (!ok) {
        missing++;
        surf = kFlowDry;
      }
      uint32_t* n = &words_[kFlowFarBase + kFlowHeaderWords + 2 * ((size_t)j * kFlowFarNodes + i)];
      n[0] = surf == kFlowDry ? 0u : (QVel(vx) | (QVel(vz) << 16));
      n[1] = (uint32_t)(uint16_t)(int16_t)surf | ((uint32_t)(uint16_t)(int16_t)bed << 16);
    }
  }
  farMissing_ = missing;
  farLanded_ = 0;
  farTicks_ = 0;
  farBuilt_ = true;
  farVersion_++;
  if (std::getenv("SANDVOX_FLOW_TRACE"))
    std::fprintf(stderr, "flow: far grid (%d,%d) composed, %u nodes waiting on a block, %zu solving\n",
                 farCx_, farCz_, missing, farPending_.size());
}

// TRANSCRIBED in common.wgsl flowGridAt, line for line: same words, same
// bilinear weights, same vertical gate and profile. Change both together.
Vec3 WaterFlowField::Sample(Vec3 p) const {
  if (words_.size() < kFlowWords || words_[2] == 0u) return Vec3{0.0f, 0.0f, 0.0f};
  const int n = (int)words_[2];
  const float fx = (p.x - (float)(int32_t)words_[0]) / (float)kFlowStep;
  const float fz = (p.z - (float)(int32_t)words_[1]) / (float)kFlowStep;
  const int ix = (int)std::floor(fx), iz = (int)std::floor(fz);
  if (ix < 0 || iz < 0 || ix >= n - 1 || iz >= n - 1) return Vec3{0.0f, 0.0f, 0.0f};
  const float tx = fx - (float)ix, tz = fz - (float)iz;
  float vx = 0.0f, vz = 0.0f, surf = -1e9f, bed = 1e9f;
  for (int c = 0; c < 4; c++) {
    const int dx = c & 1, dz = c >> 1;
    const size_t k = kFlowHeaderWords + 2 * ((size_t)(iz + dz) * n + (ix + dx));
    const int s = (int16_t)(words_[k + 1] & 0xFFFFu);
    if (s == kFlowDry) continue;
    const float w = (dx ? tx : 1.0f - tx) * (dz ? tz : 1.0f - tz);
    vx += w * (float)(int16_t)(words_[k] & 0xFFFFu);
    vz += w * (float)(int16_t)(words_[k] >> 16);
    surf = std::max(surf, (float)s);
    bed = std::min(bed, (float)(int16_t)(words_[k + 1] >> 16));
  }
  if (surf < -1e8f || p.y > surf + 1.5f || p.y < bed - 0.5f) return Vec3{0.0f, 0.0f, 0.0f};
  // Faster at the surface than at the bed: a log-law profile, flattened.
  const float hz = std::clamp((p.y - bed) / std::max(surf - bed, 1.0f), 0.0f, 1.0f);
  const float prof = (0.75f + 0.25f * std::sqrt(hz)) / (float)kFlowVelQ;
  return Vec3{vx * prof, 0.0f, vz * prof};
}

WaterFlowField& WaterFlow() {
  static WaterFlowField f;
  return f;
}

void WaterFlowQuiesce() { WaterFlow().Quiesce(); }

namespace flowfield {
namespace {
const rhi::Buffer* gBuf = nullptr;
uint32_t gUploaded = 0xFFFFFFFFu;
uint32_t gUploadedFar = 0xFFFFFFFFu;
}  // namespace

void SetBuffer(const rhi::Buffer* buf) {
  gBuf = buf;
  gUploaded = 0xFFFFFFFFu;
  gUploadedFar = 0xFFFFFFFFu;
}

Vec3 SampleMeasured(const uint32_t* grid, Vec3 p) {
  return SampleMeasuredSpan(grid, p, p.y, p.y);
}

Vec3 SampleMeasuredSpan(const uint32_t* grid, Vec3 p, float yLo, float yHi) {
  const Vec3 zero{0.0f, 0.0f, 0.0f};
  if (grid == nullptr || grid[2] == 0u) return zero;
  const int n = (int)grid[2];
  const float fx = (p.x - (float)(int32_t)grid[0]) / (float)kFlowStep;
  const float fz = (p.z - (float)(int32_t)grid[1]) / (float)kFlowStep;
  const int ix = (int)std::floor(fx), iz = (int)std::floor(fz);
  if (ix < 0 || iz < 0 || ix >= n - 1 || iz >= n - 1) return zero;
  const float tx = fx - (float)ix, tz = fz - (float)iz;
  float vx = 0.0f, vz = 0.0f, top = -1e9f, bot = 1e9f;
  for (int c = 0; c < 4; c++) {
    const int dx = c & 1, dz = c >> 1;
    const size_t k = kFlowHeaderWords + 2 * ((size_t)(iz + dz) * n + (ix + dx));
    const int t = (int16_t)(grid[k + 1] & 0xFFFFu);
    if (t == kFlowDry) continue;
    const float w = (dx ? tx : 1.0f - tx) * (dz ? tz : 1.0f - tz);
    vx += w * (float)(int16_t)(grid[k] & 0xFFFFu);
    vz += w * (float)(int16_t)(grid[k] >> 16);
    top = std::max(top, (float)t);
    bot = std::min(bot, (float)(int16_t)(grid[k + 1] >> 16));
  }
  // The band gate, for a span: zero only when the WHOLE of [yLo, yHi] is
  // outside the band (+-1.5 cells). A point (yLo == yHi) is flowMeasAt's gate.
  if (top < -1e8f || yLo > top + 1.5f || yHi < bot - 1.5f) return zero;
  vx /= (float)kFlowVelQ;
  vz /= (float)kFlowVelQ;
  // THE VERTICAL: common.wgsl flowMeasRise, line for line, with the span's
  // nearer end standing in for the point.
  float rise = 0.0f;
  const float vh = std::sqrt(vx * vx + vz * vz);
  if (vh > 0.0f) {
    const size_t vbase = kFlowHeaderWords + 2 * (size_t)n * n;
    float f = 0.0f;
    for (int c = 0; c < 4; c++) {
      const int dx = c & 1, dz = c >> 1;
      const size_t node = (size_t)(iz + dz) * n + (ix + dx);
      if ((int)(int16_t)(grid[kFlowHeaderWords + 2 * node + 1] & 0xFFFFu) == kFlowDry) continue;
      const uint32_t vw = grid[vbase + node];
      const float on = (float)(vw >> 28);
      if (on == 0.0f) continue;
      const float h = (float)(int16_t)(vw & 0xFFFFu);
      const float hw = (float)((vw >> 16) & 63u);
      const float d = std::max((float)(((vw >> 22) & 63u) * 2u), 4.0f);
      float dy = 0.0f;
      if (yHi < h - hw) dy = (h - hw) - yHi;
      if (yLo > h + hw) dy = (h + hw) - yLo;
      const float w = (dx ? tx : 1.0f - tx) * (dz ? tz : 1.0f - tz);
      f += w * (on / 15.0f) * std::clamp(dy / d, -1.5f, 1.5f);
    }
    rise = vh * f;
  }
  return Vec3{vx, rise, vz};
}

namespace {
// The published snapshot's GRID copy (PublishMeasured). A shared pointer to
// the snapshot's own immutable table, so installing it is a reference count,
// not a 36 KiB copy, and a later snapshot sharing the same table (no fold ran
// in between) installs the same pointer.
std::shared_ptr<const std::vector<uint32_t>> gMeasured;
}  // namespace

void PublishMeasured(std::shared_ptr<const std::vector<uint32_t>> grid) {
  gMeasured = std::move(grid);
}

const uint32_t* PublishedMeasured() {
  if (!gMeasured || gMeasured->size() < (size_t)kFlowMeasGridWords) return nullptr;
  return gMeasured->data();
}

Vec3 BlendMeasured(Vec3 solved, Vec3 measured) {
  const float m = std::sqrt(measured.x * measured.x + measured.z * measured.z);
  const float keep = 1.0f - std::clamp(m / kFlowMeasTakeover, 0.0f, 1.0f);
  return Vec3{solved.x * keep + measured.x, solved.y * keep + measured.y,
              solved.z * keep + measured.z};
}

void Upload(const rhi::Queue& queue) {
  if (gBuf == nullptr) return;
  const WaterFlowField& f = WaterFlow();
  const bool have = f.Words().size() >= kFlowWords;
  const uint32_t zero[kFlowHeaderWords] = {0u, 0u, 0u, 0u};
  // The window grid and the far grid upload apart: the window's moves every
  // chunk, the far one every kFlowFarSnap cells.
  if (f.Version() != gUploaded) {
    gUploaded = f.Version();
    if (have) queue.WriteBuffer(*gBuf, 0, f.Words().data(), (size_t)kFlowNearWords * 4);
    else queue.WriteBuffer(*gBuf, 0, zero, sizeof zero);
  }
  if (f.FarVersion() != gUploadedFar) {
    gUploadedFar = f.FarVersion();
    if (have) {
      queue.WriteBuffer(*gBuf, (uint64_t)kFlowFarBase * 4, f.Words().data() + kFlowFarBase,
                        (size_t)kFlowFarWords * 4);
    } else {
      queue.WriteBuffer(*gBuf, (uint64_t)kFlowFarBase * 4, zero, sizeof zero);
    }
  }
}
}  // namespace flowfield

}  // namespace sandvox
