// worldwater.cpp -- map.json rivers[] and lakes[] in the engine
// (docs/PLAN_worldbuilder.md W1): the spline, the parse, and THE BAKE.
//
// WHY RIVERS CAN EXIST AT ALL. A flowing CA river never sleeps (CLAUDE.md
// rule 2; docs/PLAN_water_master.md lists them as a non-goal). So a river here
// is not a current: it is a STAIRCASE OF STILL POOLS laid along a line. The
// bake makes the water surface monotone non-increasing source -> mouth and
// quantizes it into flat REACHES; inside a reach the surface is one Y, so the
// water is at rest the moment worldgen writes it, and between reaches a WEIR
// (the lower reach's first RIVER_LIP voxels below the step's cut line, held
// at the upper surface) keeps the upper pool from spilling. A step that
// drops more than kRiverWaterfall is a waterfall: recorded, drawn as a step.
// The look of flow is the renderer's business, never mass transport.
//
// THE RIVER BAKE, precisely (E2's JS preview ports this; the numbers are
// integer voxels unless said):
//   1. Knots [x, z, w?, d?]: a knot without w / d takes them linearly by arc
//      length (along the tessellated curve) between its nearest knots that
//      have them (the first and last must).
//   2. The centreline: SplineTessellate (smooth) or PolylineResample at step
//      16; per output point w / d = lerp(knot seg, knot seg + 1, f). Rounded
//      to integer voxels, consecutive duplicates dropped.
//   3. Samples every 4 voxels along that polyline, as roads sample: per
//      segment len = round(|b - a|), n = max(1, ceil(len / 4)), sample k at
//      a + (b - a) * k / n (integer division toward zero), arc s += len * k / n,
//      w / d lerped by k / n; the last point appended. hw = max(1, round(w / 2)),
//      d = round(d).
//   4. g_i = BareGroundHeight(sample) -- the ground with no river, road or pad.
//   5. SOURCE IN A LAKE (the first sample inside a lake polygon): every sample
//      up to arc (first sample outside) + Lf is S = level, Lf = hw + bank +
//      band + RIVER_LIP + 4 there. After it the running state starts at
//      G0 = level + 1, R = level.
//   6. Otherwise, running min m_i = min(m_{i-1}, g_i) from m_0 = g_0; the
//      REACH: G0 = m_0, R = G0 - 1; whenever m_i <= G0 - stepVox, a new reach
//      G0 = m_i, R = m_i - 1. S_i = R.
//   7. The level it drains to, E: a JOINED river's surface at the parent
//      sample nearest the mouth (the parent baked first), else the level of
//      the lake the mouth lies in, else seaLevelY. Every S_i = max(S_i, E);
//      and the TAIL is forced to E: a join's last Lf = hwParent + hw + bank +
//      RIVER_LIP + 4 of arc, a lake mouth's samples from (first sample of the
//      trailing run inside the polygon) - Lf (Lf as in 5). So the water that
//      meets the parent or the lake is at its level, and any drop is a step
//      (with its weir) well clear of the other body.
//   8. Reaches = runs of equal S. A step between samples i-1 and i is cut at
//      sample i with the cut normal = normalize(p[i+1] - p[i-1]) * 1024
//      (rounded), SHARED by both pieces, so the two sides partition the plane
//      exactly. Within a reach, pieces split Douglas-Peucker style while a
//      sample strays > 1 voxel from the chord, the chord is > kRiverPieceMax
//      a side, or hw / d stray > 1 from their lerp.
//   9. Per piece: bank = map's `bank`, else round(1.5 * 2 * max hw), clamped
//      to kRiverBankMax and hw + bank <= 1536.
// The column-side rules (channel, dam, weir, bank, ford) are worldgen.wgsl
// waterApply's comment.
//
// LAKES: the outline (spline-closed or the polygon), Douglas-Peucker 0.75,
// edges split to <= kLakeEdgeMax; one kSiteLakeEdge per edge; one
// kSiteLakeCell per site cell an edge is listed in (its row parity bitset) or
// that lies wholly inside (a flag). lakeTest's comment is the predicate.
#include "sim/worldwater.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "sim/biomes.h"
#include "sim/materials.h"

namespace worldmap {

using nlohmann::json;

// ---- THE SPLINE -----------------------------------------------------------------
namespace {

double Dist(const std::array<double, 2>& a, const std::array<double, 2>& b) {
  const double dx = b[0] - a[0], dz = b[1] - a[1];
  return std::sqrt(dx * dx + dz * dz);
}

std::array<double, 2> BgEval(const std::array<double, 2>& p0, const std::array<double, 2>& p1,
                             const std::array<double, 2>& p2, const std::array<double, 2>& p3, double t0,
                             double t1, double t2, double t3, double t) {
  std::array<double, 2> out{};
  for (int c = 0; c < 2; c++) {
    const double a1 = (t1 - t) / (t1 - t0) * p0[c] + (t - t0) / (t1 - t0) * p1[c];
    const double a2 = (t2 - t) / (t2 - t1) * p1[c] + (t - t1) / (t2 - t1) * p2[c];
    const double a3 = (t3 - t) / (t3 - t2) * p2[c] + (t - t2) / (t3 - t2) * p3[c];
    const double b1 = (t2 - t) / (t2 - t0) * a1 + (t - t0) / (t2 - t0) * a2;
    const double b2 = (t3 - t) / (t3 - t1) * a2 + (t - t1) / (t3 - t1) * a3;
    out[c] = (t2 - t) / (t2 - t1) * b1 + (t - t1) / (t2 - t1) * b2;
  }
  return out;
}

std::vector<SplinePt> Tessellate(const std::vector<std::array<double, 2>>& P, bool closed, double step, bool smooth) {
  std::vector<SplinePt> out;
  const int n = static_cast<int>(P.size());
  if (n == 0) return out;
  if (n == 1) { out.push_back({P[0][0], P[0][1], 0, 0.0}); return out; }
  auto ctrl = [&](int i) -> std::array<double, 2> {
    if (closed) return P[((i % n) + n) % n];
    if (i < 0) return {2 * P[0][0] - P[1][0], 2 * P[0][1] - P[1][1]};
    if (i >= n) return {2 * P[n - 1][0] - P[n - 2][0], 2 * P[n - 1][1] - P[n - 2][1]};
    return P[i];
  };
  const int nseg = closed ? n : n - 1;
  for (int i = 0; i < nseg; i++) {
    const std::array<double, 2> p0 = ctrl(i - 1), p1 = ctrl(i), p2 = ctrl(i + 1), p3 = ctrl(i + 2);
    std::vector<std::array<double, 2>> dense;
    if (!smooth) {
      dense = {p1, p2};
    } else {
      const double d01 = std::max(Dist(p0, p1), 1e-4);
      const double d12 = std::max(Dist(p1, p2), 1e-4);
      const double d23 = std::max(Dist(p2, p3), 1e-4);
      const double t0 = 0.0, t1 = std::sqrt(d01), t2 = t1 + std::sqrt(d12), t3 = t2 + std::sqrt(d23);
      const int nd = std::min(std::max(static_cast<int>(std::ceil(d12 / 2.0)), 8), 4096);
      dense.push_back(p1);
      for (int k = 1; k < nd; k++) {
        const double t = t1 + (t2 - t1) * k / nd;
        dense.push_back(BgEval(p0, p1, p2, p3, t0, t1, t2, t3, t));
      }
      dense.push_back(p2);
    }
    std::vector<double> cum(dense.size(), 0.0);
    for (size_t k = 1; k < dense.size(); k++) cum[k] = cum[k - 1] + Dist(dense[k - 1], dense[k]);
    const double L = cum.back();
    const int m = std::max(1, static_cast<int>(std::ceil(L / step)));
    out.push_back({dense[0][0], dense[0][1], i, 0.0});
    size_t j = 0;
    for (int k = 1; k < m; k++) {
      const double s = L * k / m;
      while (j + 2 < dense.size() && cum[j + 1] < s) j++;
      const double span = cum[j + 1] - cum[j];
      const double f = span > 0 ? (s - cum[j]) / span : 0.0;
      const std::array<double, 2>& a = dense[j];
      const std::array<double, 2>& b = dense[j + 1];
      out.push_back({a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, i, static_cast<double>(k) / m});
    }
  }
  if (!closed) out.push_back({P[n - 1][0], P[n - 1][1], n - 2, 1.0});
  return out;
}

// Douglas-Peucker on integer points, open polyline, tolerance in voxels: the
// kept indices (first and last always).
void DpKeep(const std::vector<std::pair<int, int>>& p, size_t a, size_t b, double tol, std::vector<char>& keep) {
  if (b <= a + 1) return;
  const double ax = p[a].first, az = p[a].second, vx = p[b].first - ax, vz = p[b].second - az;
  const double vl = std::sqrt(vx * vx + vz * vz);
  double best = -1;
  size_t bi = a;
  for (size_t k = a + 1; k < b; k++) {
    const double qx = p[k].first - ax, qz = p[k].second - az;
    const double d = vl > 0 ? std::fabs(qx * vz - qz * vx) / vl : std::sqrt(qx * qx + qz * qz);
    if (d > best) { best = d; bi = k; }
  }
  if (best > tol) {
    keep[bi] = 1;
    DpKeep(p, a, bi, tol, keep);
    DpKeep(p, bi, b, tol, keep);
  }
}

std::vector<std::pair<int, int>> RoundDedupe(const std::vector<SplinePt>& t) {
  std::vector<std::pair<int, int>> out;
  for (const SplinePt& q : t) {
    const std::pair<int, int> v{static_cast<int>(std::lround(q.x)), static_cast<int>(std::lround(q.z))};
    if (!out.empty() && out.back() == v) continue;
    out.push_back(v);
  }
  return out;
}

std::vector<std::pair<int, int>> Simplify(const std::vector<std::pair<int, int>>& p, double tol) {
  if (p.size() < 3) return p;
  std::vector<char> keep(p.size(), 0);
  keep.front() = keep.back() = 1;
  DpKeep(p, 0, p.size() - 1, tol, keep);
  std::vector<std::pair<int, int>> out;
  for (size_t i = 0; i < p.size(); i++)
    if (keep[i]) out.push_back(p[i]);
  return out;
}

uint32_t ResolveMat(const biomes::BiomeSet& set, const std::string& name, const char* fallback,
                    const std::string& who, const WaterWarn& warn) {
  auto find = [&](const std::string& n) -> uint32_t {
    for (size_t i = 1; i < set.matNames.size(); i++)
      if (set.matNames[i] == n) return static_cast<uint32_t>(i);
    return 0u;
  };
  uint32_t id = find(name);
  if (id != 0u && id < set.matClass.size() && set.matClass[id] != CLASS_SOLID && set.matClass[id] != CLASS_POWDER) {
    warn(who + "\"" + name + "\" is not a solid or a powder; using " + fallback);
    id = 0u;
  } else if (id == 0u) {
    warn(who + "\"" + name + "\" is not a material in materials.json; using " + fallback);
  }
  if (id == 0u) id = find(fallback);
  if (id == 0u) id = find("stone");
  return id;
}

int PackCut(double tx, double tz) {
  const double l = std::sqrt(tx * tx + tz * tz);
  if (l <= 0) return static_cast<int>(1024u);
  const int cx = static_cast<int>(std::lround(tx / l * 1024.0));
  const int cz = static_cast<int>(std::lround(tz / l * 1024.0));
  return static_cast<int>((static_cast<uint32_t>(cx) & 0xFFFFu) | ((static_cast<uint32_t>(cz) & 0xFFFFu) << 16));
}

// floor(a / b) for b > 0, exact in i64.
int64_t FloorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b) != 0 && ((a < 0) != (b < 0))) q -= 1;
  return q;
}
// The lake predicate's kx (worldgen.wgsl lakeTest): the largest integer x
// strictly left of edge (a, b)'s crossing of row z, or false when the edge
// does not straddle z (half-open: (az > z) != (bz > z)).
bool CrossK(int ax, int az, int bx, int bz, int z, int64_t& k) {
  if ((az > z) == (bz > z)) return false;
  int64_t den = bz - az;
  int64_t num = static_cast<int64_t>(ax) * den + static_cast<int64_t>(z - az) * (bx - ax);
  if (den < 0) { den = -den; num = -num; }
  k = FloorDiv(num - 1, den);
  return true;
}

}  // namespace

std::vector<SplinePt> SplineTessellate(const std::vector<std::array<double, 2>>& pts, bool closed, double step) {
  return Tessellate(pts, closed, step, true);
}
std::vector<SplinePt> PolylineResample(const std::vector<std::array<double, 2>>& pts, bool closed, double step) {
  return Tessellate(pts, closed, step, false);
}

std::vector<std::pair<int, int>> SmoothRoadPoints(const std::vector<std::pair<int, int>>& pts) {
  std::vector<std::array<double, 2>> p;
  for (const auto& [x, z] : pts) p.push_back({static_cast<double>(x), static_cast<double>(z)});
  return Simplify(RoundDedupe(SplineTessellate(p, false, 16.0)), 0.75);
}

bool WorldMapData::Lake::Inside(int x, int z) const {
  bool in = false;
  const size_t n = poly.size();
  for (size_t i = 0, k = n - 1; i < n; k = i++) {
    int64_t kx;
    if (CrossK(poly[k].first, poly[k].second, poly[i].first, poly[i].second, z, kx) && x <= kx) in = !in;
  }
  return in;
}

int WaterIndexReach(const WorldMapData::StampSite& st, int treeMaxReach) {
  if (st.kind == kSiteRiver)
    return std::max(st.radius, st.hwB) +
           std::max({st.padMargin, treeMaxReach, static_cast<int>(kSiteRoadFarTint)}) + static_cast<int>(kRiverLip) + 1;
  if (st.kind == kSiteLakeEdge) return st.padMargin + std::max(treeMaxReach, 0) + 1;
  return 0;   // a lake cell: its own cell
}

// ---- PARSE -------------------------------------------------------------------------
bool ParseRiversLakes(const json& j, const biomes::BiomeSet& set, WorldMapData& out, const std::string& at,
                      std::string& log, const WaterWarn& warn) {
  auto num = [](const json& v, double& d) {
    if (!v.is_number()) return false;
    d = v.get<double>();
    return std::isfinite(d) && std::fabs(d) <= (1 << 24);
  };
  auto clampWarn = [&](const std::string& who, const char* key, int v, int lo, int hi) {
    const int c = std::clamp(v, lo, hi);
    if (c != v)
      warn(who + key + " " + std::to_string(v) + " clamped to " + std::to_string(c) + " (" + std::to_string(lo) + ".." +
           std::to_string(hi) + ")");
    return c;
  };
  if (j.contains("rivers")) {
    if (!j["rivers"].is_array()) { log += at + "rivers must be an array\n"; return false; }
    std::set<std::string> ids;
    for (const json& r : j["rivers"]) {
      if (!r.is_object() || !r.contains("id") || !r["id"].is_string() || r["id"].get<std::string>().empty()) {
        log += at + "a rivers[] entry needs an id\n";
        return false;
      }
      WorldMapData::River rv;
      rv.id = r["id"].get<std::string>();
      const std::string who = at + "river \"" + rv.id + "\": ";
      const std::string wwho = "river \"" + rv.id + "\": ";
      if (!ids.insert(rv.id).second) { log += who + "a second river with this id\n"; return false; }
      rv.name = r.contains("name") && r["name"].is_string() ? r["name"].get<std::string>() : std::string();
      if (!r.contains("points") || !r["points"].is_array()) {
        log += who + "needs points: [[x, z, w, d], ...] in world voxels, source first\n";
        return false;
      }
      for (const json& p : r["points"]) {
        double v[4] = {0, 0, -1, -1};
        const bool shape = p.is_array() && (p.size() == 2 || p.size() == 4);
        bool ok = shape;
        for (size_t k = 0; ok && k < p.size(); k++) ok = num(p[k], v[k]);
        if (!ok) { log += who + "a point that is not [x, z] or [x, z, w, d] within +-16M voxels\n"; return false; }
        std::array<int, 4> q{static_cast<int>(std::lround(v[0])), static_cast<int>(std::lround(v[1])), -1, -1};
        if (p.size() == 4) {
          q[2] = clampWarn(wwho, "w", static_cast<int>(std::lround(v[2])), kRiverWidthMin, kRiverWidthMax);
          q[3] = clampWarn(wwho, "d", static_cast<int>(std::lround(v[3])), kRiverDepthMin, kRiverDepthMax);
        }
        if (!rv.knots.empty() && rv.knots.back()[0] == q[0] && rv.knots.back()[1] == q[1]) continue;
        rv.knots.push_back(q);
      }
      if (rv.knots.size() < 2) { log += who + "needs at least two distinct points\n"; return false; }
      if (rv.knots.front()[2] < 0 || rv.knots.back()[2] < 0) {
        log += who + "the first and last points must carry [x, z, w, d] (width and depth)\n";
        return false;
      }
      rv.smooth = r.contains("smooth") && r["smooth"].is_boolean() && r["smooth"].get<bool>();
      rv.bedName = r.contains("bed") && r["bed"].is_string() ? r["bed"].get<std::string>() : std::string("gravel");
      rv.bedId = ResolveMat(set, rv.bedName, "gravel", wwho + "bed ", warn);
      if (r.contains("joins") && r["joins"].is_string()) rv.joins = r["joins"].get<std::string>();
      if (r.contains("stepVox") && r["stepVox"].is_number())
        rv.stepVox = clampWarn(wwho, "stepVox", static_cast<int>(std::lround(r["stepVox"].get<double>())), 1, kRiverStepMax);
      if (r.contains("bank") && r["bank"].is_number())
        rv.bank = clampWarn(wwho, "bank", static_cast<int>(std::lround(r["bank"].get<double>())), 0, kRiverBankMax);
      if (r.contains("profile") && r["profile"].is_string()) {
        rv.profile = r["profile"].get<std::string>();
        if (rv.profile != "U" && rv.profile != "V") {
          warn(wwho + "profile \"" + rv.profile + "\" is not U or V; using U");
          rv.profile = "U";
        }
      }
      for (const auto& k : rv.knots)
        if (!out.InExtent(k[0], k[1])) { warn(wwho + "a point is outside the map's extent"); break; }
      out.rivers.push_back(std::move(rv));
    }
    // Joins resolve once every river is read.
    for (WorldMapData::River& rv : out.rivers) {
      if (rv.joins.empty()) continue;
      for (size_t k = 0; k < out.rivers.size(); k++)
        if (out.rivers[k].id == rv.joins) rv.joinIdx = static_cast<int>(k);
      if (rv.joinIdx < 0) {
        log += at + "river \"" + rv.id + "\": joins \"" + rv.joins + "\", which is not a river on this map\n";
        return false;
      }
      if (&out.rivers[static_cast<size_t>(rv.joinIdx)] == &rv) {
        log += at + "river \"" + rv.id + "\": joins itself\n";
        return false;
      }
    }
  }
  if (j.contains("lakes")) {
    if (!j["lakes"].is_array()) { log += at + "lakes must be an array\n"; return false; }
    std::set<std::string> ids;
    for (const json& r : j["lakes"]) {
      if (!r.is_object() || !r.contains("id") || !r["id"].is_string() || r["id"].get<std::string>().empty()) {
        log += at + "a lakes[] entry needs an id\n";
        return false;
      }
      WorldMapData::Lake lk;
      lk.id = r["id"].get<std::string>();
      const std::string who = at + "lake \"" + lk.id + "\": ";
      const std::string wwho = "lake \"" + lk.id + "\": ";
      if (!ids.insert(lk.id).second) { log += who + "a second lake with this id\n"; return false; }
      lk.name = r.contains("name") && r["name"].is_string() ? r["name"].get<std::string>() : std::string();
      double lv = 0;
      if (!r.contains("level") || !num(r["level"], lv) || std::fabs(lv) > kHeightLimit) {
        log += who + "needs level: the water surface Y in voxels (within +-" + std::to_string(kHeightLimit) + ")\n";
        return false;
      }
      lk.level = static_cast<int>(std::lround(lv));
      if (!r.contains("points") || !r["points"].is_array()) {
        log += who + "needs points: [[x, z], ...] in world voxels\n";
        return false;
      }
      for (const json& p : r["points"]) {
        double x = 0, z = 0;
        if (!p.is_array() || p.size() != 2 || !num(p[0], x) || !num(p[1], z)) {
          log += who + "a point that is not [x, z] within +-16M voxels\n";
          return false;
        }
        const std::pair<int, int> q{static_cast<int>(std::lround(x)), static_cast<int>(std::lround(z))};
        if (!lk.points.empty() && lk.points.back() == q) continue;
        lk.points.push_back(q);
      }
      if (lk.points.size() > 1 && lk.points.front() == lk.points.back()) lk.points.pop_back();
      if (lk.points.size() < 3) { log += who + "a polygon needs at least three distinct points\n"; return false; }
      lk.smooth = r.contains("smooth") && r["smooth"].is_boolean() && r["smooth"].get<bool>();
      lk.bedName = r.contains("bed") && r["bed"].is_string() ? r["bed"].get<std::string>() : std::string("shore_mud");
      lk.shoreName = r.contains("shore") && r["shore"].is_string() ? r["shore"].get<std::string>() : std::string("sand");
      lk.bedId = ResolveMat(set, lk.bedName, "shore_mud", wwho + "bed ", warn);
      lk.shoreId = ResolveMat(set, lk.shoreName, "sand", wwho + "shore ", warn);
      if (r.contains("band") && r["band"].is_number())
        lk.band = clampWarn(wwho, "band", static_cast<int>(std::lround(r["band"].get<double>())), 4, kLakeBandMax);
      for (const auto& q : lk.points)
        if (!out.InExtent(q.first, q.second)) { warn(wwho + "a point is outside the map's extent"); break; }
      out.lakes.push_back(std::move(lk));
    }
  }
  return true;
}

// ---- THE BAKE ------------------------------------------------------------------------
bool BakeRiversLakes(WorldMapData& out, uint32_t seed, std::vector<WorldMapData::StampSite>& sites, std::string& log,
                     const std::string& at, const WaterWarn& warn) {
  const int sea = out.seaLevelY;
  // ---- lakes: outline, edges, cells ----
  const int cellLog2 = out.siteCellLog2;
  const int S = 1 << cellLog2;
  for (size_t li = 0; li < out.lakes.size(); li++) {
    WorldMapData::Lake& lk = out.lakes[li];
    std::vector<std::pair<int, int>> ring;
    if (lk.smooth) {
      std::vector<std::array<double, 2>> p;
      for (const auto& [x, z] : lk.points) p.push_back({static_cast<double>(x), static_cast<double>(z)});
      ring = RoundDedupe(SplineTessellate(p, true, 16.0));
    } else {
      ring = lk.points;
    }
    while (ring.size() > 1 && ring.front() == ring.back()) ring.pop_back();
    // Simplify the closed ring as an open one from vertex 0 round to vertex 0.
    if (ring.size() >= 4) {
      std::vector<std::pair<int, int>> open = ring;
      open.push_back(ring.front());
      open = Simplify(open, 0.75);
      open.pop_back();
      if (open.size() >= 3) ring = open;
    }
    lk.poly.clear();
    for (size_t i = 0; i < ring.size(); i++) {
      const auto [ax, az] = ring[i];
      const auto [bx, bz] = ring[(i + 1) % ring.size()];
      const int len = std::max(std::abs(bx - ax), std::abs(bz - az));
      const int n = std::max(1, (len + static_cast<int>(kLakeEdgeMax) - 1) / static_cast<int>(kLakeEdgeMax));
      for (int k = 0; k < n; k++)
        lk.poly.push_back({ax + static_cast<int>(static_cast<int64_t>(bx - ax) * k / n),
                           az + static_cast<int>(static_cast<int64_t>(bz - az) * k / n)});
    }
    if (lk.poly.size() < 3) { log += at + "lake \"" + lk.id + "\": its outline collapses to under three points\n"; return false; }
    lk.x0 = lk.x1 = lk.poly[0].first;
    lk.z0 = lk.z1 = lk.poly[0].second;
    for (const auto& [x, z] : lk.poly) {
      lk.x0 = std::min(lk.x0, x); lk.x1 = std::max(lk.x1, x);
      lk.z0 = std::min(lk.z0, z); lk.z1 = std::max(lk.z1, z);
    }
    const size_t e0 = sites.size();
    for (size_t i = 0; i < lk.poly.size(); i++) {
      WorldMapData::StampSite st;
      st.id = "lake " + lk.id + " edge " + std::to_string(i);
      st.kind = kSiteLakeEdge;
      st.x = lk.poly[i].first; st.z = lk.poly[i].second;
      st.x1 = lk.poly[(i + 1) % lk.poly.size()].first; st.z1 = lk.poly[(i + 1) % lk.poly.size()].second;
      st.padMargin = lk.band;
      st.surf = lk.level;
      st.bed = lk.bedId;
      st.shore = lk.shoreId;
      st.water = static_cast<int>(li);
      sites.push_back(std::move(st));
    }
    lk.edges = static_cast<int>(lk.poly.size());
    // Which cells list an edge: the index lists an edge over its box grown by
    // its reach (WaterIndexReach with the widest crown -- the caller's
    // buildIndex uses the same function), so a cell an edge PASSES THROUGH
    // always lists it. Grown here by the band + 1 + 512, a superset: a cell
    // listing no edge but marked here only costs its record.
    std::set<std::pair<int, int>> edgeCells;
    const int er = lk.band + 512 + 1;
    for (size_t i = e0; i < sites.size(); i++) {
      const WorldMapData::StampSite& e = sites[i];
      int c0x, c0z, c1x, c1z;
      out.CellOf(std::min(e.x, e.x1) - er, std::min(e.z, e.z1) - er, &c0x, &c0z);
      out.CellOf(std::max(e.x, e.x1) + er, std::max(e.z, e.z1) + er, &c1x, &c1z);
      for (int cz = c0z; cz <= c1z; cz++)
        for (int cx = c0x; cx <= c1x; cx++) edgeCells.insert({cx, cz});
    }
    // The cells round the outline: per cell, per row, the parity of the edges
    // crossing the row right of the cell (kx >= the cell's max x).
    int c0x, c0z, c1x, c1z;
    out.CellOf(lk.x0 - er, lk.z0 - er, &c0x, &c0z);
    out.CellOf(lk.x1 + er, lk.z1 + er, &c1x, &c1z);
    const int words = (S + 31) / 32;
    std::map<std::pair<int, int>, std::vector<uint32_t>> bits;
    std::vector<int64_t> ks;
    for (int cz = c0z; cz <= c1z; cz++) {
      const int Z0 = (cz - out.siteOriginZ) << cellLog2;
      for (int r = 0; r < S; r++) {
        const int z = Z0 + r;
        if (z < lk.z0 || z > lk.z1) continue;   // no edge straddles it: parity 0 everywhere
        ks.clear();
        const size_t n = lk.poly.size();
        for (size_t i = 0, k = n - 1; i < n; k = i++) {
          int64_t kx;
          if (CrossK(lk.poly[k].first, lk.poly[k].second, lk.poly[i].first, lk.poly[i].second, z, kx)) ks.push_back(kx);
        }
        if (ks.empty()) continue;
        std::sort(ks.begin(), ks.end());
        for (int cx = c0x; cx <= c1x; cx++) {
          const int64_t X1 = static_cast<int64_t>((cx - out.siteOriginX) << cellLog2) + S - 1;
          const size_t right = static_cast<size_t>(ks.end() - std::lower_bound(ks.begin(), ks.end(), X1));
          if ((right & 1u) == 0u) continue;
          std::vector<uint32_t>& b = bits[{cx, cz}];
          if (b.empty()) b.assign(static_cast<size_t>(words), 0u);
          b[static_cast<size_t>(r) >> 5] |= 1u << (r & 31);
        }
      }
    }
    int cells = 0;
    for (int cz = c0z; cz <= c1z; cz++)
      for (int cx = c0x; cx <= c1x; cx++) {
        if (!out.Inside(cx, cz)) continue;
        const bool listed = edgeCells.count({cx, cz}) != 0;
        auto it = bits.find({cx, cz});
        const bool anyIn = it != bits.end();
        if (!listed && !anyIn) continue;
        WorldMapData::StampSite st;
        st.id = "lake " + lk.id + " cell " + std::to_string(cx) + "," + std::to_string(cz);
        st.kind = kSiteLakeCell;
        st.x = (cx - out.siteOriginX) << cellLog2;
        st.z = (cz - out.siteOriginZ) << cellLog2;
        st.x1 = st.x + S - 1;
        st.z1 = st.z + S - 1;
        st.padMargin = lk.band;
        st.surf = lk.level;
        st.bed = lk.bedId;
        st.shore = lk.shoreId;
        st.water = static_cast<int>(li);
        if (listed) {
          st.bits = anyIn ? it->second : std::vector<uint32_t>(static_cast<size_t>(words), 0u);
        } else {
          // No edge passes through: every row has the same parity (it is
          // inside); a flag, no bitset.
          st.wflags = kLakeCellInside;
        }
        sites.push_back(std::move(st));
        cells++;
      }
    lk.cells = cells;
    // A lake that holds no water at its outline's centroid-ish samples is a
    // likely mistake; say how much of its bounding box is wet at 64-voxel steps.
    int wetN = 0, inN = 0;
    for (int z = lk.z0; z <= lk.z1; z += 64)
      for (int x = lk.x0; x <= lk.x1; x += 64)
        if (lk.Inside(x, z)) {
          inN++;
          if (BareGroundHeight(x, z, seed) < lk.level) wetN++;
        }
    if (inN > 0 && wetN == 0)
      warn("lake \"" + lk.id + "\": its ground is at or above level " + std::to_string(lk.level) +
           " everywhere sampled inside it -- it holds no water");
    if (lk.level < sea) warn("lake \"" + lk.id + "\": level " + std::to_string(lk.level) + " is under the sea (" +
                             std::to_string(sea) + ")");
  }

  // ---- rivers, parents before tributaries ----
  const size_t nr = out.rivers.size();
  std::vector<int> order, state(nr, 0);
  std::string cycle;
  std::function<bool(size_t)> visit = [&](size_t i) -> bool {
    if (state[i] == 2) return true;
    if (state[i] == 1) { cycle = out.rivers[i].id; return false; }
    state[i] = 1;
    if (out.rivers[i].joinIdx >= 0 && !visit(static_cast<size_t>(out.rivers[i].joinIdx))) return false;
    state[i] = 2;
    order.push_back(static_cast<int>(i));
    return true;
  };
  for (size_t i = 0; i < nr; i++)
    if (!visit(i)) {
      log += at + "rivers' joins form a cycle through \"" + cycle + "\"\n";
      return false;
    }
  auto lakeAt = [&](int x, int z) -> int {
    for (size_t k = 0; k < out.lakes.size(); k++) {
      const WorldMapData::Lake& lk = out.lakes[k];
      if (x >= lk.x0 && x <= lk.x1 && z >= lk.z0 && z <= lk.z1 && lk.Inside(x, z)) return static_cast<int>(k);
    }
    return -1;
  };
  for (int ri : order) {
    WorldMapData::River& rv = out.rivers[static_cast<size_t>(ri)];
    const std::string wwho = "river \"" + rv.id + "\": ";
    // 1. knots: the tessellation, and each knot's arc along it.
    std::vector<std::array<double, 2>> kp;
    for (const auto& k : rv.knots) kp.push_back({static_cast<double>(k[0]), static_cast<double>(k[1])});
    const std::vector<SplinePt> tp = rv.smooth ? SplineTessellate(kp, false, 16.0) : PolylineResample(kp, false, 16.0);
    const size_t nk = rv.knots.size();
    std::vector<double> karc(nk, 0.0);
    {
      double arc = 0;
      for (size_t i = 0; i < tp.size(); i++) {
        if (i > 0) arc += std::hypot(tp[i].x - tp[i - 1].x, tp[i].z - tp[i - 1].z);
        if (tp[i].f == 0.0 && tp[i].seg >= 0 && static_cast<size_t>(tp[i].seg) < nk) karc[static_cast<size_t>(tp[i].seg)] = arc;
        if (i + 1 == tp.size()) karc[nk - 1] = arc;
      }
    }
    std::vector<double> kw(nk), kd(nk);
    for (int a = 0; a < 2; a++) {
      std::vector<double>& dst = a == 0 ? kw : kd;
      size_t prev = 0;
      for (size_t i = 0; i < nk; i++) {
        if (rv.knots[i][2 + a] < 0) continue;
        dst[i] = rv.knots[i][2 + a];
        for (size_t q = prev + 1; q < i; q++) {
          const double span = karc[i] - karc[prev];
          const double f = span > 0 ? (karc[q] - karc[prev]) / span : 0.0;
          dst[q] = dst[prev] + (dst[i] - dst[prev]) * f;
        }
        prev = i;
      }
    }
    // 2. the integer centreline with its width and depth.
    struct V { int x, z; double w, d; };
    std::vector<V> cl;
    for (const SplinePt& q : tp) {
      const size_t s0 = static_cast<size_t>(std::clamp(q.seg, 0, static_cast<int>(nk) - 1));
      const size_t s1 = std::min(s0 + 1, nk - 1);
      const V v{static_cast<int>(std::lround(q.x)), static_cast<int>(std::lround(q.z)), kw[s0] + (kw[s1] - kw[s0]) * q.f,
                kd[s0] + (kd[s1] - kd[s0]) * q.f};
      if (!cl.empty() && cl.back().x == v.x && cl.back().z == v.z) continue;
      cl.push_back(v);
    }
    if (cl.size() < 2) { log += at + "river \"" + rv.id + "\": its centreline collapses to one point\n"; return false; }
    // 3. samples every 4 voxels.
    struct Smp { int x, z, s, hw, d, g, S, T8; };
    std::vector<Smp> sm;
    {
      int arc = 0;
      for (size_t i = 0; i + 1 < cl.size(); i++) {
        const int64_t dx = cl[i + 1].x - cl[i].x, dz = cl[i + 1].z - cl[i].z;
        const int len = std::max(1, static_cast<int>(std::llround(std::sqrt(static_cast<double>(dx * dx + dz * dz)))));
        const int n = std::max(1, (len + 3) / 4);
        for (int k = 0; k < n; k++) {
          const double f = static_cast<double>(k) / n;
          Smp s{};
          s.x = cl[i].x + static_cast<int>(dx * k / n);
          s.z = cl[i].z + static_cast<int>(dz * k / n);
          s.s = arc + len * k / n;
          s.hw = std::max(1, static_cast<int>(std::lround((cl[i].w + (cl[i + 1].w - cl[i].w) * f) / 2.0)));
          s.d = static_cast<int>(std::lround(cl[i].d + (cl[i + 1].d - cl[i].d) * f));
          sm.push_back(s);
        }
        arc += len;
      }
      Smp s{};
      s.x = cl.back().x; s.z = cl.back().z; s.s = arc;
      s.hw = std::max(1, static_cast<int>(std::lround(cl.back().w / 2.0)));
      s.d = static_cast<int>(std::lround(cl.back().d));
      sm.push_back(s);
    }
    const int N = static_cast<int>(sm.size());
    auto bankAt = [&](int hw) {
      int b = rv.bank >= 0 ? rv.bank : static_cast<int>(std::lround(1.5 * 2.0 * hw));
      b = std::min({b, static_cast<int>(kRiverBankMax), 1536 - hw});
      return std::max(b, 0);
    };
    // 4. the bare ground.
    for (Smp& s : sm) s.g = BareGroundHeight(s.x, s.z, seed);
    // 5. a source in a lake.
    rv.lakeIn = lakeAt(sm.front().x, sm.front().z);
    rv.lakeOut = lakeAt(sm.back().x, sm.back().z);
    int lead = -1, leadLevel = 0;
    if (rv.lakeIn >= 0) {
      const WorldMapData::Lake& lk = out.lakes[static_cast<size_t>(rv.lakeIn)];
      int j = 0;
      while (j < N && lk.Inside(sm[static_cast<size_t>(j)].x, sm[static_cast<size_t>(j)].z)) j++;
      const int jj = std::min(j, N - 1);
      const Smp& sj = sm[static_cast<size_t>(jj)];
      const int Lf = sj.hw + bankAt(sj.hw) + lk.band + static_cast<int>(kRiverLip) + 4;
      lead = 0;
      while (lead + 1 < N && sm[static_cast<size_t>(lead + 1)].s <= sj.s + Lf) lead++;
      leadLevel = lk.level;
    }
    // 6. running min, quantized into reaches.
    {
      int m = 0, G0 = 0, R = 0;
      for (int i = 0; i < N; i++) {
        Smp& s = sm[static_cast<size_t>(i)];
        if (i <= lead) {
          s.S = leadLevel;
          m = G0 = leadLevel + 1;
          R = leadLevel;
          continue;
        }
        if (i == 0) {
          m = G0 = s.g;
          R = G0 - 1;
        } else {
          m = std::min(m, s.g);
          if (m <= G0 - rv.stepVox) { G0 = m; R = m - 1; }
        }
        s.S = R;
      }
    }
    // 7. the level it drains to, and the tail.
    int E = sea, tailFrom = N;
    if (rv.joinIdx >= 0) {
      const WorldMapData::River& par = out.rivers[static_cast<size_t>(rv.joinIdx)];
      int64_t best = INT64_MAX;
      size_t bi = 0;
      for (size_t k = 0; k < par.line.size(); k++) {
        const int64_t dx = par.line[k][0] - sm.back().x, dz = par.line[k][1] - sm.back().z;
        if (dx * dx + dz * dz < best) { best = dx * dx + dz * dz; bi = k; }
      }
      if (!par.line.empty()) {
        E = par.line[bi][2];
        const int hwP = par.line[bi][3];
        const double dist = std::sqrt(static_cast<double>(best));
        if (dist > hwP + 2)
          warn(wwho + "its mouth is " + std::to_string(static_cast<int>(dist)) + " voxels from \"" + par.id +
               "\"'s centreline, outside its channel (half-width " + std::to_string(hwP) + ")");
        const Smp& se = sm.back();
        const int Lf = hwP + se.hw + bankAt(se.hw) + static_cast<int>(kRiverLip) + 4;
        tailFrom = N - 1;
        while (tailFrom > 0 && sm[static_cast<size_t>(tailFrom - 1)].s >= se.s - Lf) tailFrom--;
        // The "loader warns and clamps" of the format: a tributary whose own
        // ground runs below the parent's surface at the junction is raised to it.
        int below = 0;
        for (int i = tailFrom; i < N; i++) below = std::max(below, E - sm[static_cast<size_t>(i)].S);
        if (below > 0)
          warn(wwho + "its surface at the junction was up to " + std::to_string(below) + " voxels below \"" + par.id +
               "\"'s (" + std::to_string(E) + "); clamped to it");
      }
    } else if (rv.lakeOut >= 0) {
      const WorldMapData::Lake& lk = out.lakes[static_cast<size_t>(rv.lakeOut)];
      E = lk.level;
      int j = N - 1;
      while (j > 0 && lk.Inside(sm[static_cast<size_t>(j - 1)].x, sm[static_cast<size_t>(j - 1)].z)) j--;
      const Smp& sj = sm[static_cast<size_t>(j)];
      const int Lf = sj.hw + bankAt(sj.hw) + lk.band + static_cast<int>(kRiverLip) + 4;
      tailFrom = j;
      while (tailFrom > 0 && sm[static_cast<size_t>(tailFrom - 1)].s >= sj.s - Lf) tailFrom--;
    }
    if (rv.lakeIn >= 0 && leadLevel < E)
      warn(wwho + "its source lake's level " + std::to_string(leadLevel) + " is below the level it drains to (" +
           std::to_string(E) + ")");
    for (int i = 0; i < N; i++) {
      Smp& s = sm[static_cast<size_t>(i)];
      s.S = std::max(s.S, E);
      if (i >= tailFrom) s.S = E;
    }
    rv.endLevel = E;
    // 8. THE RUNNING SURFACE (2026-10-16): the staircase becomes a slope.
    // Every step of S that drops at most kRiverWaterfall is SOFT: the surface
    // ramps down to it linearly in arc, in EIGHTHS, over at most
    // kRiverRampVox of arc per voxel of drop and never before the previous
    // step (nor inside a source lake's lead). A step that drops more is a
    // WATERFALL and stays HARD: a cut, a crest and a held curtain (worldgen
    // waterApply). T8 is the water surface in eighths: the column holds full
    // cells to (T8 - 1) >> 3 and the top one holds T8 - 8 * that eighths.
    // T8 <= 8 S + 8 everywhere, so the ramp is under the staircase the
    // ground was cut for, never over it. Integer only (the JS port matches).
    for (Smp& s : sm) s.T8 = 8 * s.S + 8;
    {
      int prev = std::max(lead, 0);
      for (int i = 1; i < N; i++) {
        const Smp& sp = sm[static_cast<size_t>(i - 1)];
        const Smp& si = sm[static_cast<size_t>(i)];
        if (si.S == sp.S) continue;
        const int D = sp.S - si.S;
        if (D <= static_cast<int>(kRiverWaterfall)) {
          const int s0 = std::max(sm[static_cast<size_t>(prev)].s, si.s - D * static_cast<int>(kRiverRampVox));
          const int A8 = 8 * sp.S + 8, drop8 = 8 * D, span = std::max(si.s - s0, 1);
          for (int j = prev; j < i; j++) {
            Smp& sj = sm[static_cast<size_t>(j)];
            if (sj.s <= s0) continue;
            sj.T8 = A8 - static_cast<int>((static_cast<int64_t>(drop8) * (sj.s - s0)) / span);
          }
        }
        prev = i;
      }
    }
    // 9. reaches (runs between HARD steps), the waterfalls, pieces.
    std::vector<int> starts{0};
    rv.falls.clear();
    for (int i = 1; i < N; i++) {
      const int drop = sm[static_cast<size_t>(i - 1)].S - sm[static_cast<size_t>(i)].S;
      if (drop > static_cast<int>(kRiverWaterfall)) {
        starts.push_back(i);
        rv.falls.push_back({sm[static_cast<size_t>(i)].x, sm[static_cast<size_t>(i)].z, drop});
      }
    }
    rv.reaches = static_cast<int>(starts.size());
    auto cutAt = [&](int i) {
      const Smp& a = sm[static_cast<size_t>(std::max(i - 1, 0))];
      const Smp& b = sm[static_cast<size_t>(std::min(i + 1, N - 1))];
      return PackCut(b.x - a.x, b.z - a.z);
    };
    int q = 0;
    for (size_t r = 0; r < starts.size(); r++) {
      const int a0 = starts[r];
      const int b0 = r + 1 < starts.size() ? starts[r + 1] : N - 1;
      if (b0 <= a0) continue;
      std::vector<std::pair<int, int>> stack{{a0, b0}}, done;
      while (!stack.empty()) {
        const auto [a, b] = stack.back();
        stack.pop_back();
        const Smp& A = sm[static_cast<size_t>(a)];
        const Smp& B = sm[static_cast<size_t>(b)];
        const double vx = B.x - A.x, vz = B.z - A.z, vl = std::max(std::sqrt(vx * vx + vz * vz), 1e-9);
        double dev = 0;
        int farK = -1;
        for (int k = a + 1; k < b; k++) {
          const Smp& K = sm[static_cast<size_t>(k)];
          const double qx = K.x - A.x, qz = K.z - A.z;
          double d = std::fabs(qx * vz - qz * vx) / vl;
          const double f = B.s > A.s ? static_cast<double>(K.s - A.s) / (B.s - A.s) : 0.0;
          d = std::max(d, std::fabs(K.hw - (A.hw + (B.hw - A.hw) * f)));
          d = std::max(d, std::fabs(K.d - (A.d + (B.d - A.d) * f)));
          // The surface, in eighths: a piece is one straight ramp, so a
          // sample more than 2 eighths off the chord's lerp splits it.
          d = std::max(d, std::fabs(K.T8 - (A.T8 + (B.T8 - A.T8) * f)) / 2.0);
          if (d > dev) { dev = d; farK = k; }
        }
        const bool tooLong = std::max(std::abs(B.x - A.x), std::abs(B.z - A.z)) > static_cast<int>(kRiverPieceMax);
        if (b - a > 1 && (tooLong || dev > 1.0)) {
          const int mid = (dev <= 1.0 || farK < 0) ? (a + b) / 2 : farK;
          stack.push_back({mid, b});
          stack.push_back({a, mid});
          continue;
        }
        done.push_back({a, b});
      }
      for (size_t pi = 0; pi < done.size(); pi++) {
        const auto [a, b] = done[pi];
        const Smp& A = sm[static_cast<size_t>(a)];
        const Smp& B = sm[static_cast<size_t>(b)];
        WorldMapData::StampSite st;
        st.id = "river " + rv.id + " #" + std::to_string(++q);
        st.kind = kSiteRiver;
        st.water = ri;
        st.x = A.x; st.z = A.z; st.x1 = B.x; st.z1 = B.z;
        st.radius = std::min(A.hw, static_cast<int>(kRiverWidthMax) / 2);
        st.hwB = std::min(B.hw, static_cast<int>(kRiverWidthMax) / 2);
        st.padMargin = bankAt(std::max(st.radius, st.hwB));
        st.depthA = A.d;
        st.depthB = B.d;
        // The piece's surface, eighths, at each end (worldmap.h kS_RivSurf /
        // kS_RivLipY): waterApply lerps it along the chord.
        st.surf = A.T8;
        st.surfB = B.T8;
        st.bed = rv.bedId;
        st.len = static_cast<int>(std::lround(std::hypot(B.x - A.x, B.z - A.z)));
        st.wflags = rv.profile == "V" ? kRivProfileV : 0;
        st.lipY = (A.T8 - 1) >> 3;
        if (pi == 0 && r > 0) {
          st.wflags |= kRivFlatA;
          st.cutA = cutAt(a);
          st.lipY = sm[static_cast<size_t>(a0 - 1)].S;
        }
        if (pi + 1 == done.size() && r + 1 < starts.size()) {
          st.wflags |= kRivFlatB;
          st.cutB = cutAt(b);
        }
        sites.push_back(std::move(st));
      }
    }
    rv.pieces = q;
    // The baked line (the horizon, the gate, a tributary's junction) and the
    // reported profile.
    // line[2] and the profile's y are the top WATER CELL's Y, (T8 - 1) >> 3
    // (the staircase S where no ramp lowered it).
    rv.line.clear();
    for (const Smp& s : sm) rv.line.push_back({s.x, s.z, (s.T8 - 1) >> 3, s.hw});
    rv.profS.clear(); rv.profY.clear(); rv.profG.clear();
    const int pk = std::max(1, (N + kRoadProfileMax - 2) / (kRoadProfileMax - 1));
    for (int i = 0; i < N; i += pk) {
      rv.profS.push_back(sm[static_cast<size_t>(i)].s);
      rv.profY.push_back((sm[static_cast<size_t>(i)].T8 - 1) >> 3);
      rv.profG.push_back(sm[static_cast<size_t>(i)].g);
    }
    if ((N - 1) % pk != 0) {
      rv.profS.push_back(sm.back().s); rv.profY.push_back((sm.back().T8 - 1) >> 3); rv.profG.push_back(sm.back().g);
    }
    std::printf("world map: river '%s': %d samples, %d reaches, %d pieces, %zu waterfalls, %d -> %d (drains to %d)\n",
                rv.id.c_str(), N, rv.reaches, rv.pieces, rv.falls.size(), sm.front().S, sm.back().S, E);
  }
  return true;
}

}  // namespace worldmap
