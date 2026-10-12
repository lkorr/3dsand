// dungeon.cpp -- the dungeon shell: asset I/O, resolution, placement, the GPU
// block and the CPU twin of worldgen_dungeon.wgsli. See dungeon.h.
#include "sim/dungeon.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <tuple>

#include <nlohmann/json.hpp>

#include "sim/materials.h"
#include "sim/rng.h"
#include "sim/worldmap.h"

using nlohmann::json;

namespace dungeon {

// ============================================================================
// the shell
// ============================================================================
const std::string& Shell::Str(uint32_t i) const {
  static const std::string kNone;
  return i < strings.size() ? strings[i] : kNone;
}
uint32_t Shell::Intern(const std::string& s) {
  for (size_t i = 0; i < strings.size(); i++)
    if (strings[i] == s) return static_cast<uint32_t>(i);
  strings.push_back(s);
  return static_cast<uint32_t>(strings.size() - 1);
}

namespace {

const char* const kOpNames[] = {"carve", "fill", "skin", "keep"};
const char* const kShapeNames[] = {"box", "rbox", "ellipsoid", "cylinder", "capsule", "ramp", "vault", "tube"};
const char* const kKindNames[] = {"rubble", "stalagmite", "stalactite", "crystal", "moss", "puddle", "bones", "ore_vein"};
const char* const kSurfNames[] = {"floor", "ceil", "wall", "any"};
const char* const kAxisNames[] = {"x", "y", "z"};
const char* const kDirNames[] = {"+x", "-x", "+z", "-z"};

template <size_t N>
int IndexOf(const char* const (&names)[N], const std::string& s) {
  for (size_t i = 0; i < N; i++)
    if (s == names[i]) return static_cast<int>(i);
  return -1;
}

int64_t Num(const json& j, const char* k, int64_t d) {
  if (!j.contains(k)) return d;
  const json& v = j[k];
  if (v.is_number_integer()) return v.get<int64_t>();
  if (v.is_number()) return static_cast<int64_t>(std::llround(v.get<double>()));
  return d;
}
bool Vec3(const json& j, const char* k, std::array<int32_t, 3>& out) {
  if (!j.contains(k) || !j[k].is_array() || j[k].size() != 3) return false;
  for (int i = 0; i < 3; i++) {
    if (!j[k][i].is_number()) return false;
    out[i] = static_cast<int32_t>(j[k][i].is_number_integer() ? j[k][i].get<int64_t>()
                                                               : std::llround(j[k][i].get<double>()));
  }
  return true;
}

uint32_t FnvBytes(uint32_t h, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
  return h;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

}  // namespace

bool ParseShellJson(const std::string& text, Shell& s, std::string& err) {
  s = Shell{};
  json j;
  try {
    j = json::parse(text);
  } catch (const std::exception& e) {
    err = std::string("shell.json does not parse: ") + e.what();
    return false;
  }
  if (!j.is_object()) { err = "shell.json is not an object"; return false; }
  if (j.value("format", std::string()) != "sandvox-dungeon-shell") {
    err = "shell.json: \"format\" must be \"sandvox-dungeon-shell\"";
    return false;
  }
  s.version = static_cast<uint32_t>(Num(j, "version", 1));
  if (s.version != 1) { err = "shell.json: version " + std::to_string(s.version) + ", this build reads 1"; return false; }
  s.dungeon = j.value("dungeon", std::string());
  s.sourceHash = static_cast<uint32_t>(Num(j, "sourceHash", 0));
  s.naturalCaves = j.value("naturalCaves", false);
  if (j.contains("anchor") && !Vec3(j, "anchor", s.anchor)) { err = "shell.json: anchor must be [x, y, z]"; return false; }
  s.keepOut = static_cast<int32_t>(Num(j, "keepOut", 24));
  if (j.contains("materials")) {
    if (!j["materials"].is_object()) { err = "shell.json: materials must be an object slot -> name"; return false; }
    // Sorted by slot (nlohmann's object order), so the table is a function of content.
    for (auto it = j["materials"].begin(); it != j["materials"].end(); ++it) {
      if (!it.value().is_string()) { err = "shell.json: material slot \"" + it.key() + "\" must name a material"; return false; }
      s.materials.push_back({s.Intern(it.key()), s.Intern(it.value().get<std::string>())});
    }
  }
  auto str = [&](const json& o, const char* k) -> uint32_t {
    if (!o.contains(k) || !o[k].is_string()) return kNoStr;
    return s.Intern(o[k].get<std::string>());
  };
  const json prims = j.value("prims", json::array());
  if (!prims.is_array()) { err = "shell.json: prims must be an array"; return false; }
  for (size_t i = 0; i < prims.size(); i++) {
    const json& o = prims[i];
    const std::string who = "prim " + std::to_string(i) + ": ";
    if (!o.is_object()) { err = who + "not an object"; return false; }
    Prim p;
    const int op = IndexOf(kOpNames, o.value("op", std::string()));
    const int sh = IndexOf(kShapeNames, o.value("shape", std::string()));
    if (op < 0) { err = who + "op must be carve|fill|skin|keep"; return false; }
    if (sh < 0) { err = who + "unknown shape \"" + o.value("shape", std::string()) + "\""; return false; }
    p.op = static_cast<uint32_t>(op);
    p.shape = static_cast<uint32_t>(sh);
    p.mat = str(o, "mat");
    p.firm = str(o, "firm");
    p.piece = str(o, "piece");
    if (p.shape == kShapeCapsule) {
      if (!Vec3(o, "a", p.a) || !Vec3(o, "b", p.b)) { err = who + "a capsule needs a and b"; return false; }
      p.r = static_cast<int32_t>(Num(o, "r", 0));
    } else if (p.shape == kShapeTube) {
      if (!o.contains("pts") || !o["pts"].is_array() || !o.contains("r") || !o["r"].is_array() ||
          o["pts"].size() != o["r"].size() || o["pts"].size() < 2) {
        err = who + "a tube needs pts [[x,y,z],...] and r [...] of the same length (>= 2)";
        return false;
      }
      p.tubeFirst = static_cast<uint32_t>(s.tube.size());
      for (size_t k = 0; k < o["pts"].size(); k++) {
        const json& q = o["pts"][k];
        if (!q.is_array() || q.size() != 3 || !o["r"][k].is_number()) { err = who + "bad tube point"; return false; }
        TubePt t;
        t.x = static_cast<int32_t>(std::llround(q[0].get<double>()));
        t.y = static_cast<int32_t>(std::llround(q[1].get<double>()));
        t.z = static_cast<int32_t>(std::llround(q[2].get<double>()));
        t.r = static_cast<int32_t>(std::llround(o["r"][k].get<double>()));
        s.tube.push_back(t);
      }
      p.tubeCount = static_cast<uint32_t>(o["pts"].size());
    } else {
      if (!Vec3(o, "min", p.a) || !Vec3(o, "max", p.b)) { err = who + "needs min and max"; return false; }
    }
    if (p.shape == kShapeRBox) p.r = static_cast<int32_t>(Num(o, "r", 0));
    if (p.shape == kShapeVault) p.r = static_cast<int32_t>(Num(o, "rise", 0));
    if (p.shape == kShapeCylinder || p.shape == kShapeVault) {
      const int ax = IndexOf(kAxisNames, o.value("axis", std::string(p.shape == kShapeVault ? "x" : "y")));
      if (ax < 0) { err = who + "axis must be x|y|z"; return false; }
      p.axis = static_cast<uint32_t>(ax);
    }
    if (p.shape == kShapeRamp) {
      const int d = IndexOf(kDirNames, o.value("dir", std::string("+x")));
      if (d < 0) { err = who + "dir must be +x|-x|+z|-z"; return false; }
      p.axis = static_cast<uint32_t>(d);
      p.r = static_cast<int32_t>(Num(o, "stepH", 1));
      p.stepD = static_cast<int32_t>(Num(o, "stepD", 1));
      p.height = static_cast<int32_t>(Num(o, "height", 0));
      p.below = static_cast<int32_t>(Num(o, "below", 0));
    }
    if (o.contains("noise") && o["noise"].is_object()) {
      const json& n = o["noise"];
      p.noiseAmp = static_cast<int32_t>(Num(n, "amp", 0));
      p.noiseScale = static_cast<int32_t>(Num(n, "scale", 4));
      p.noiseOct = static_cast<int32_t>(Num(n, "octaves", 1));
      p.noiseSeed = static_cast<uint32_t>(Num(n, "seed", 0));
      if (p.noiseAmp == 0) p.noiseOct = 0;
    }
    p.blend = static_cast<int32_t>(Num(o, "blend", 0));
    p.skin = static_cast<int32_t>(Num(o, "skin", 0));
    if (o.contains("level") && o["level"].is_number()) p.level = static_cast<int32_t>(Num(o, "level", 0));
    p.openOnly = o.value("openOnly", false);
    p.mat2 = str(o, "mat2");
    p.checker = static_cast<int32_t>(Num(o, "checker", 0));
    if (o.contains("scatter")) {
      if (!o["scatter"].is_array()) { err = who + "scatter must be an array"; return false; }
      p.scatterFirst = static_cast<uint32_t>(s.scatter.size());
      for (const json& r : o["scatter"]) {
        Scatter sc;
        const int kd = IndexOf(kKindNames, r.value("kind", std::string()));
        if (kd < 0) { err = who + "unknown scatter kind \"" + r.value("kind", std::string()) + "\""; return false; }
        sc.kind = static_cast<uint32_t>(kd);
        const double dens = r.contains("density") && r["density"].is_number() ? r["density"].get<double>() : 0.0;
        sc.densityQ16 = static_cast<uint32_t>(std::clamp(std::floor(dens * 65536.0 + 0.5), 0.0, 65536.0));
        if (r.contains("size") && r["size"].is_array() && r["size"].size() == 2) {
          sc.sizeMin = static_cast<int32_t>(std::llround(r["size"][0].get<double>()));
          sc.sizeMax = static_cast<int32_t>(std::llround(r["size"][1].get<double>()));
        }
        const int sf = IndexOf(kSurfNames, r.value("surface", std::string(sc.kind == kScStalactite ? "ceil" : "floor")));
        if (sf < 0) { err = who + "scatter surface must be floor|ceil|wall|any"; return false; }
        sc.surface = static_cast<uint32_t>(sf);
        sc.mat = str(r, "mat");
        sc.seed = static_cast<uint32_t>(Num(r, "seed", 0));
        sc.spacing = static_cast<int32_t>(Num(r, "spacing", 0));
        s.scatter.push_back(sc);
      }
      p.scatterCount = static_cast<uint32_t>(s.scatter.size()) - p.scatterFirst;
    }
    s.prims.push_back(p);
  }
  if (j.contains("props")) {
    const json& pr = j["props"];
    for (const json& t : pr.value("templates", json::array())) {
      Template tp;
      tp.id = str(t, "id");
      if (!t.contains("size") || !t["size"].is_array() || t["size"].size() != 3) {
        err = "template " + std::to_string(s.templates.size()) + ": needs size [nx, ny, nz]";
        return false;
      }
      tp.nx = t["size"][0].get<int32_t>();
      tp.ny = t["size"][1].get<int32_t>();
      tp.nz = t["size"][2].get<int32_t>();
      for (const json& n : t.value("palette", json::array())) {
        if (!n.is_string()) { err = "template palette entries are material names"; return false; }
        tp.palette.push_back(s.Intern(n.get<std::string>()));
      }
      for (const json& v : t.value("voxels", json::array())) {
        if (!v.is_array() || v.size() != 4) { err = "template voxels are [x, y, z, paletteIndex]"; return false; }
        tp.voxels.push_back({v[0].get<int32_t>(), v[1].get<int32_t>(), v[2].get<int32_t>(), v[3].get<int32_t>()});
      }
      s.templates.push_back(std::move(tp));
    }
    for (const json& in : pr.value("instances", json::array())) {
      Instance ins;
      ins.tmpl = static_cast<uint32_t>(Num(in, "template", 0));
      std::array<int32_t, 3> pos{};
      if (!Vec3(in, "pos", pos)) { err = "instance " + std::to_string(s.instances.size()) + ": needs pos"; return false; }
      ins.x = pos[0];
      ins.y = pos[1];
      ins.z = pos[2];
      const int64_t yaw = Num(in, "yaw", 0);
      if (yaw % 90 != 0) { err = "instance " + std::to_string(s.instances.size()) + ": yaw must be a quarter turn"; return false; }
      ins.yawQ = static_cast<uint32_t>(((yaw / 90) % 4 + 4) % 4);
      ins.id = str(in, "id");
      s.instances.push_back(ins);
    }
  }
  return true;
}

// ---- the binary (PLAN §3.4.1 "shell.bin") ----------------------------------
namespace {
struct Rd {
  const uint8_t* p;
  size_t n, at = 0;
  bool ok = true;
  uint32_t U() {
    if (at + 4 > n) { ok = false; return 0; }
    uint32_t v;
    std::memcpy(&v, p + at, 4);
    at += 4;
    return v;
  }
  int32_t I() { return static_cast<int32_t>(U()); }
};
void W32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; i++) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
}  // namespace

bool ReadShellBin(const uint8_t* data, size_t n, Shell& s, std::string& err) {
  s = Shell{};
  Rd r{data, n};
  if (r.U() != kBinMagic) { err = "shell.bin: not an SVDG file"; return false; }
  s.version = r.U();
  if (s.version != kBinVersion) { err = "shell.bin: version " + std::to_string(s.version) + ", this build reads 1"; return false; }
  s.sourceHash = r.U();
  s.naturalCaves = (r.U() & 1u) != 0;
  for (int i = 0; i < 3; i++) s.anchor[i] = r.I();
  s.keepOut = r.I();
  const uint32_t ns = r.U();
  if (!r.ok || ns > (1u << 20)) { err = "shell.bin: bad string count"; return false; }
  for (uint32_t i = 0; i < ns && r.ok; i++) {
    const uint32_t len = r.U();
    if (len > (1u << 16) || r.at + len > n) { err = "shell.bin: bad string"; return false; }
    s.strings.emplace_back(reinterpret_cast<const char*>(data + r.at), len);
    r.at += (len + 3u) & ~3u;
  }
  auto sid = [&](uint32_t v) { return v == kNoStr || v < s.strings.size(); };
  const uint32_t nm = r.U();
  for (uint32_t i = 0; i < nm && r.ok; i++) {
    const uint32_t a = r.U(), b = r.U();
    if (!sid(a) || !sid(b) || a == kNoStr || b == kNoStr) { err = "shell.bin: bad material entry"; return false; }
    s.materials.push_back({a, b});
  }
  const uint32_t np = r.U();
  if (!r.ok || np > (1u << 20)) { err = "shell.bin: bad prim count"; return false; }
  for (uint32_t i = 0; i < np && r.ok; i++) {
    Prim p;
    p.op = r.U();
    p.shape = r.U();
    p.mat = r.U();
    for (int k = 0; k < 3; k++) p.a[k] = r.I();
    for (int k = 0; k < 3; k++) p.b[k] = r.I();
    p.r = r.I();
    p.axis = r.U();
    p.stepD = r.I();
    p.height = r.I();
    p.below = r.I();
    p.noiseAmp = r.I();
    p.noiseScale = r.I();
    p.noiseOct = r.I();
    p.noiseSeed = r.U();
    p.blend = r.I();
    p.skin = r.I();
    p.level = r.I();
    p.firm = r.U();
    p.tubeFirst = r.U();
    p.tubeCount = r.U();
    p.scatterFirst = r.U();
    p.scatterCount = r.U();
    if (p.op > 3 || p.shape > 7 || !sid(p.mat) || !sid(p.firm)) {
      err = "shell.bin: prim " + std::to_string(i) + " is malformed";
      return false;
    }
    s.prims.push_back(p);
  }
  const uint32_t nt = r.U();
  for (uint32_t i = 0; i < nt && r.ok; i++) {
    TubePt t;
    t.x = r.I();
    t.y = r.I();
    t.z = r.I();
    t.r = r.I();
    s.tube.push_back(t);
  }
  const uint32_t nsc = r.U();
  for (uint32_t i = 0; i < nsc && r.ok; i++) {
    Scatter c;
    c.kind = r.U();
    c.densityQ16 = r.U();
    c.sizeMin = r.I();
    c.sizeMax = r.I();
    c.surface = r.U();
    c.mat = r.U();
    c.seed = r.U();
    c.spacing = r.I();
    if (c.kind > 7 || c.surface > 3 || !sid(c.mat)) { err = "shell.bin: scatter " + std::to_string(i) + " is malformed"; return false; }
    s.scatter.push_back(c);
  }
  const uint32_t ntp = r.U();
  for (uint32_t i = 0; i < ntp && r.ok; i++) {
    Template t;
    t.id = r.U();
    t.nx = r.I();
    t.ny = r.I();
    t.nz = r.I();
    const uint32_t npal = r.U();
    if (npal > 4096) { err = "shell.bin: bad template palette"; return false; }
    for (uint32_t k = 0; k < npal && r.ok; k++) t.palette.push_back(r.U());
    const uint32_t nv = r.U();
    if (!r.ok || r.at + static_cast<size_t>(nv) * 16 > n) { err = "shell.bin: bad template voxels"; return false; }
    for (uint32_t k = 0; k < nv; k++) {
      const int32_t x = r.I(), y = r.I(), z = r.I(), pi = r.I();
      t.voxels.push_back({x, y, z, pi});
    }
    for (uint32_t k : t.palette)
      if (!sid(k)) { err = "shell.bin: template palette string out of range"; return false; }
    s.templates.push_back(std::move(t));
  }
  const uint32_t ni = r.U();
  for (uint32_t i = 0; i < ni && r.ok; i++) {
    Instance in;
    in.tmpl = r.U();
    in.x = r.I();
    in.y = r.I();
    in.z = r.I();
    in.yawQ = r.U() & 3u;
    in.id = r.U();
    s.instances.push_back(in);
  }
  // The PIECES block, then the end marker.
  const uint32_t npc = r.U();
  if (r.ok && npc != s.prims.size()) { err = "shell.bin: pieces block has " + std::to_string(npc) + " entries for " + std::to_string(s.prims.size()) + " prims"; return false; }
  for (uint32_t i = 0; i < npc && r.ok; i++) s.prims[i].piece = r.U();
  uint32_t tag = r.U();
  if (tag == kBinPrimExtras) {
    const uint32_t nx = r.U();
    if (r.ok && nx != s.prims.size()) { err = "shell.bin: PRMX block has " + std::to_string(nx) + " entries for " + std::to_string(s.prims.size()) + " prims"; return false; }
    for (uint32_t i = 0; i < nx && r.ok; i++) {
      const uint32_t fl = r.U();
      s.prims[i].openOnly = (fl & 1u) != 0;
      s.prims[i].mat2 = r.U();
      s.prims[i].checker = r.I();
      r.U();
      if (!sid(s.prims[i].mat2)) { err = "shell.bin: prim " + std::to_string(i) + " mat2 string out of range"; return false; }
    }
    tag = r.U();
  }
  if (tag != kBinEnd || !r.ok) { err = "shell.bin: truncated (no END! marker)"; return false; }
  for (const Prim& p : s.prims) {
    if (p.tubeFirst + p.tubeCount > s.tube.size() || p.scatterFirst + p.scatterCount > s.scatter.size() || !sid(p.piece)) {
      err = "shell.bin: a prim's tube / scatter / piece reference is out of range";
      return false;
    }
  }
  return true;
}

std::vector<uint8_t> WriteShellBin(const Shell& s) {
  std::vector<uint8_t> b;
  W32(b, kBinMagic);
  W32(b, kBinVersion);
  W32(b, s.sourceHash);
  W32(b, s.naturalCaves ? 1u : 0u);
  for (int i = 0; i < 3; i++) W32(b, static_cast<uint32_t>(s.anchor[i]));
  W32(b, static_cast<uint32_t>(s.keepOut));
  W32(b, static_cast<uint32_t>(s.strings.size()));
  for (const std::string& t : s.strings) {
    W32(b, static_cast<uint32_t>(t.size()));
    b.insert(b.end(), t.begin(), t.end());
    while (b.size() & 3u) b.push_back(0);
  }
  W32(b, static_cast<uint32_t>(s.materials.size()));
  for (const auto& m : s.materials) { W32(b, m.first); W32(b, m.second); }
  W32(b, static_cast<uint32_t>(s.prims.size()));
  for (const Prim& p : s.prims) {
    const uint32_t w[kPrimWordsBin] = {p.op, p.shape, p.mat,
                                       static_cast<uint32_t>(p.a[0]), static_cast<uint32_t>(p.a[1]), static_cast<uint32_t>(p.a[2]),
                                       static_cast<uint32_t>(p.b[0]), static_cast<uint32_t>(p.b[1]), static_cast<uint32_t>(p.b[2]),
                                       static_cast<uint32_t>(p.r), p.axis, static_cast<uint32_t>(p.stepD),
                                       static_cast<uint32_t>(p.height), static_cast<uint32_t>(p.below),
                                       static_cast<uint32_t>(p.noiseAmp), static_cast<uint32_t>(p.noiseScale),
                                       static_cast<uint32_t>(p.noiseOct), p.noiseSeed, static_cast<uint32_t>(p.blend),
                                       static_cast<uint32_t>(p.skin), static_cast<uint32_t>(p.level), p.firm,
                                       p.tubeFirst, p.tubeCount, p.scatterFirst, p.scatterCount};
    for (uint32_t v : w) W32(b, v);
  }
  W32(b, static_cast<uint32_t>(s.tube.size()));
  for (const TubePt& t : s.tube) {
    W32(b, static_cast<uint32_t>(t.x));
    W32(b, static_cast<uint32_t>(t.y));
    W32(b, static_cast<uint32_t>(t.z));
    W32(b, static_cast<uint32_t>(t.r));
  }
  W32(b, static_cast<uint32_t>(s.scatter.size()));
  for (const Scatter& c : s.scatter) {
    const uint32_t w[kScatterWordsBin] = {c.kind, c.densityQ16, static_cast<uint32_t>(c.sizeMin),
                                          static_cast<uint32_t>(c.sizeMax), c.surface, c.mat, c.seed,
                                          static_cast<uint32_t>(c.spacing)};
    for (uint32_t v : w) W32(b, v);
  }
  W32(b, static_cast<uint32_t>(s.templates.size()));
  for (const Template& t : s.templates) {
    W32(b, t.id);
    W32(b, static_cast<uint32_t>(t.nx));
    W32(b, static_cast<uint32_t>(t.ny));
    W32(b, static_cast<uint32_t>(t.nz));
    W32(b, static_cast<uint32_t>(t.palette.size()));
    for (uint32_t p : t.palette) W32(b, p);
    W32(b, static_cast<uint32_t>(t.voxels.size()));
    for (const auto& v : t.voxels)
      for (int k = 0; k < 4; k++) W32(b, static_cast<uint32_t>(v[k]));
  }
  W32(b, static_cast<uint32_t>(s.instances.size()));
  for (const Instance& in : s.instances) {
    W32(b, in.tmpl);
    W32(b, static_cast<uint32_t>(in.x));
    W32(b, static_cast<uint32_t>(in.y));
    W32(b, static_cast<uint32_t>(in.z));
    W32(b, in.yawQ);
    W32(b, in.id);
  }
  W32(b, static_cast<uint32_t>(s.prims.size()));
  for (const Prim& p : s.prims) W32(b, p.piece);
  // The extras block only when some prim uses one, so a shell without them is
  // byte-for-byte the v1 layout every writer emits.
  bool extras = false;
  for (const Prim& p : s.prims) extras = extras || p.HasExtras();
  if (extras) {
    W32(b, kBinPrimExtras);
    W32(b, static_cast<uint32_t>(s.prims.size()));
    for (const Prim& p : s.prims) {
      W32(b, p.openOnly ? 1u : 0u);
      W32(b, p.mat2);
      W32(b, static_cast<uint32_t>(p.checker));
      W32(b, 0u);
    }
  }
  W32(b, kBinEnd);
  return b;
}

std::string ShellToJson(const Shell& s) {
  json j;
  j["format"] = "sandvox-dungeon-shell";
  j["version"] = s.version;
  if (!s.dungeon.empty()) j["dungeon"] = s.dungeon;
  j["sourceHash"] = s.sourceHash;
  j["anchor"] = {s.anchor[0], s.anchor[1], s.anchor[2]};
  j["keepOut"] = s.keepOut;
  j["naturalCaves"] = s.naturalCaves;
  json mats = json::object();
  for (const auto& m : s.materials) mats[s.Str(m.first)] = s.Str(m.second);
  j["materials"] = mats;
  json prims = json::array();
  for (const Prim& p : s.prims) {
    json o;
    o["op"] = kOpNames[p.op & 3];
    o["shape"] = kShapeNames[p.shape & 7];
    if (p.mat != kNoStr) o["mat"] = s.Str(p.mat);
    if (p.firm != kNoStr) o["firm"] = s.Str(p.firm);
    if (p.piece != kNoStr) o["piece"] = s.Str(p.piece);
    if (p.shape == kShapeCapsule) {
      o["a"] = {p.a[0], p.a[1], p.a[2]};
      o["b"] = {p.b[0], p.b[1], p.b[2]};
      o["r"] = p.r;
    } else if (p.shape == kShapeTube) {
      json pts = json::array(), rs = json::array();
      for (uint32_t k = 0; k < p.tubeCount; k++) {
        const TubePt& t = s.tube[p.tubeFirst + k];
        pts.push_back({t.x, t.y, t.z});
        rs.push_back(t.r);
      }
      o["pts"] = pts;
      o["r"] = rs;
    } else {
      o["min"] = {p.a[0], p.a[1], p.a[2]};
      o["max"] = {p.b[0], p.b[1], p.b[2]};
    }
    if (p.shape == kShapeRBox) o["r"] = p.r;
    if (p.shape == kShapeVault) o["rise"] = p.r;
    if (p.shape == kShapeCylinder || p.shape == kShapeVault) o["axis"] = kAxisNames[std::min(p.axis, 2u)];
    if (p.shape == kShapeRamp) {
      o["dir"] = kDirNames[p.axis & 3];
      o["stepH"] = p.r;
      o["stepD"] = p.stepD;
      o["height"] = p.height;
      o["below"] = p.below;
    }
    if (p.noiseOct > 0 && p.noiseAmp > 0)
      o["noise"] = {{"amp", p.noiseAmp}, {"scale", p.noiseScale}, {"octaves", p.noiseOct}, {"seed", p.noiseSeed}};
    if (p.blend) o["blend"] = p.blend;
    if (p.skin) o["skin"] = p.skin;
    if (p.level != kNoLevel) o["level"] = p.level;
    if (p.openOnly) o["openOnly"] = true;
    if (p.mat2 != kNoStr) o["mat2"] = s.Str(p.mat2);
    if (p.checker) o["checker"] = p.checker;
    if (p.scatterCount) {
      json sc = json::array();
      for (uint32_t k = 0; k < p.scatterCount; k++) {
        const Scatter& c = s.scatter[p.scatterFirst + k];
        json r;
        r["kind"] = kKindNames[c.kind & 7];
        r["density"] = c.densityQ16 / 65536.0;
        r["size"] = {c.sizeMin, c.sizeMax};
        r["surface"] = kSurfNames[c.surface & 3];
        if (c.mat != kNoStr) r["mat"] = s.Str(c.mat);
        r["seed"] = c.seed;
        if (c.spacing) r["spacing"] = c.spacing;
        sc.push_back(r);
      }
      o["scatter"] = sc;
    }
    prims.push_back(o);
  }
  j["prims"] = prims;
  json tps = json::array(), ins = json::array();
  for (const Template& t : s.templates) {
    json o;
    if (t.id != kNoStr) o["id"] = s.Str(t.id);
    o["size"] = {t.nx, t.ny, t.nz};
    json pal = json::array();
    for (uint32_t p : t.palette) pal.push_back(s.Str(p));
    o["palette"] = pal;
    json vs = json::array();
    for (const auto& v : t.voxels) vs.push_back({v[0], v[1], v[2], v[3]});
    o["voxels"] = vs;
    tps.push_back(o);
  }
  for (const Instance& in : s.instances) {
    json o;
    o["template"] = in.tmpl;
    o["pos"] = {in.x, in.y, in.z};
    o["yaw"] = in.yawQ * 90;
    if (in.id != kNoStr) o["id"] = s.Str(in.id);
    ins.push_back(o);
  }
  j["props"] = {{"templates", tps}, {"instances", ins}};
  return j.dump(1);
}

bool SameContent(const Shell& a, const Shell& b, std::string* why) {
  auto fail = [&](const std::string& w) {
    if (why) *why = w;
    return false;
  };
  auto S = [](const Shell& s, uint32_t i) { return i == kNoStr ? std::string("\x01(none)") : s.Str(i); };
  if (a.version != b.version || a.sourceHash != b.sourceHash || a.naturalCaves != b.naturalCaves ||
      a.anchor != b.anchor || a.keepOut != b.keepOut)
    return fail("header (version / sourceHash / naturalCaves / anchor / keepOut)");
  {
    std::map<std::string, std::string> ma, mb;
    for (const auto& m : a.materials) ma[a.Str(m.first)] = a.Str(m.second);
    for (const auto& m : b.materials) mb[b.Str(m.first)] = b.Str(m.second);
    if (ma != mb) return fail("materials table");
  }
  if (a.prims.size() != b.prims.size()) return fail("prim count " + std::to_string(a.prims.size()) + " vs " + std::to_string(b.prims.size()));
  for (size_t i = 0; i < a.prims.size(); i++) {
    const Prim& p = a.prims[i];
    const Prim& q = b.prims[i];
    const std::string w = "prim " + std::to_string(i);
    if (p.op != q.op || p.shape != q.shape || S(a, p.mat) != S(b, q.mat) || S(a, p.firm) != S(b, q.firm) ||
        S(a, p.piece) != S(b, q.piece))
      return fail(w + ": op / shape / mat / firm / piece");
    if (p.shape != kShapeTube && (p.a != q.a || p.b != q.b)) return fail(w + ": geometry");
    if (p.r != q.r || p.axis != q.axis || p.stepD != q.stepD || p.height != q.height || p.below != q.below)
      return fail(w + ": shape parameters");
    if (p.noiseOct != q.noiseOct || (p.noiseOct && (p.noiseAmp != q.noiseAmp || p.noiseScale != q.noiseScale ||
                                                    p.noiseSeed != q.noiseSeed)))
      return fail(w + ": noise");
    if (p.blend != q.blend || p.skin != q.skin || p.level != q.level) return fail(w + ": blend / skin / level");
    if (p.openOnly != q.openOnly || S(a, p.mat2) != S(b, q.mat2) || p.checker != q.checker)
      return fail(w + ": openOnly / mat2 / checker");
    if (p.tubeCount != q.tubeCount) return fail(w + ": tube point count");
    for (uint32_t k = 0; k < p.tubeCount; k++) {
      const TubePt& t = a.tube[p.tubeFirst + k];
      const TubePt& u = b.tube[q.tubeFirst + k];
      if (t.x != u.x || t.y != u.y || t.z != u.z || t.r != u.r) return fail(w + ": tube point " + std::to_string(k));
    }
    if (p.scatterCount != q.scatterCount) return fail(w + ": scatter count");
    for (uint32_t k = 0; k < p.scatterCount; k++) {
      const Scatter& c = a.scatter[p.scatterFirst + k];
      const Scatter& d = b.scatter[q.scatterFirst + k];
      if (c.kind != d.kind || c.densityQ16 != d.densityQ16 || c.sizeMin != d.sizeMin || c.sizeMax != d.sizeMax ||
          c.surface != d.surface || S(a, c.mat) != S(b, d.mat) || c.seed != d.seed || c.spacing != d.spacing)
        return fail(w + ": scatter rule " + std::to_string(k));
    }
  }
  if (a.templates.size() != b.templates.size()) return fail("template count");
  for (size_t i = 0; i < a.templates.size(); i++) {
    const Template& t = a.templates[i];
    const Template& u = b.templates[i];
    if (S(a, t.id) != S(b, u.id) || t.nx != u.nx || t.ny != u.ny || t.nz != u.nz || t.voxels != u.voxels ||
        t.palette.size() != u.palette.size())
      return fail("template " + std::to_string(i));
    for (size_t k = 0; k < t.palette.size(); k++)
      if (S(a, t.palette[k]) != S(b, u.palette[k])) return fail("template " + std::to_string(i) + " palette");
  }
  if (a.instances.size() != b.instances.size()) return fail("instance count");
  for (size_t i = 0; i < a.instances.size(); i++) {
    const Instance& x = a.instances[i];
    const Instance& y = b.instances[i];
    if (x.tmpl != y.tmpl || x.x != y.x || x.y != y.y || x.z != y.z || x.yawQ != y.yawQ || S(a, x.id) != S(b, y.id))
      return fail("instance " + std::to_string(i));
  }
  return true;
}

bool SourceHashOfFile(const std::string& path, uint32_t& out) {
  std::vector<uint8_t> b;
  if (!ReadFile(path, b)) return false;
  uint32_t h = 2166136261u;
  for (uint8_t c : b)
    if (c != '\r') h = (h ^ c) * 16777619u;
  out = h;
  return true;
}

bool LoadShellPath(const std::string& path, Shell& out, std::string& err, std::string* used) {
  namespace fs = std::filesystem;
  const fs::path p(path);
  std::string target = path;
  std::error_code ec;
  if (p.filename() == "dungeon.json" || fs::is_directory(p, ec)) {
    const fs::path dir = fs::is_directory(p, ec) ? p : p.parent_path();
    target = fs::is_regular_file(dir / "shell.bin", ec) ? (dir / "shell.bin").string() : (dir / "shell.json").string();
  }
  std::vector<uint8_t> b;
  if (!ReadFile(target, b)) { err = "cannot read " + target; return false; }
  if (used) *used = target;
  const bool bin = b.size() >= 4 && b[0] == 'S' && b[1] == 'V' && b[2] == 'D' && b[3] == 'G';
  if (bin) {
    if (!ReadShellBin(b.data(), b.size(), out, err)) { err = target + ": " + err; return false; }
  } else {
    if (!ParseShellJson(std::string(b.begin(), b.end()), out, err)) { err = target + ": " + err; return false; }
  }
  if (out.dungeon.empty()) out.dungeon = fs::path(target).parent_path().filename().string();
  return true;
}

bool LoadShellAsset(const std::string& assetDir, const std::string& id, Shell& out, std::string& err,
                    std::string* used) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::path(assetDir) / "dungeons" / id;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) { err = "assets/dungeons/" + id + "/ does not exist"; return false; }
  if (!fs::is_regular_file(dir / "shell.bin", ec) && !fs::is_regular_file(dir / "shell.json", ec)) {
    err = "assets/dungeons/" + id + "/ has neither shell.bin nor shell.json (compile it: dungeongen.compile)";
    return false;
  }
  if (!LoadShellPath(dir.string(), out, err, used)) return false;
  out.dungeon = id;
  return true;
}

// ============================================================================
// resolution
// ============================================================================
bool ResolveShell(const Shell& s, const std::vector<std::string>& matNames, const std::vector<uint32_t>& matClass,
                  Resolved& out, std::string& err) {
  out = Resolved{};
  std::map<std::string, std::string> slots;
  for (const auto& m : s.materials) slots[s.Str(m.first)] = s.Str(m.second);
  auto idOf = [&](const std::string& ref, uint32_t& id) {
    auto it = slots.find(ref);
    const std::string& name = it != slots.end() ? it->second : ref;
    for (size_t i = 1; i < matNames.size(); i++)
      if (matNames[i] == name) { id = static_cast<uint32_t>(i); return true; }
    return false;
  };
  auto klass = [&](uint32_t id) { return id < matClass.size() ? matClass[id] : static_cast<uint32_t>(CLASS_SOLID); };
  auto byDefault = [&](const char* slot, const char* fallback, uint32_t& id) {
    if (slots.count(slot) && idOf(slot, id)) return true;
    return idOf(fallback, id);
  };
  auto bad = [&](size_t i, const std::string& why) {
    err = "prim " + std::to_string(i) + (s.prims[i].piece != kNoStr ? " (piece " + s.Str(s.prims[i].piece) + ")" : "") +
          ": " + why;
    return false;
  };
  constexpr int kCoordMax = 1 << 20;
  auto coordOk = [&](const std::array<int32_t, 3>& v) {
    return std::abs(v[0]) <= kCoordMax && std::abs(v[1]) <= kCoordMax && std::abs(v[2]) <= kCoordMax;
  };
  for (size_t i = 0; i < s.prims.size(); i++) {
    const Prim& p = s.prims[i];
    uint32_t mat = 0, firm = 0;
    if (p.mat != kNoStr && !idOf(s.Str(p.mat), mat)) return bad(i, "material \"" + s.Str(p.mat) + "\" is neither a slot nor in materials.json");
    if (p.op == kOpCarve) {
      if (mat != 0 && klass(mat) != CLASS_LIQUID) return bad(i, "a carve's mat is its LIQUID (air when absent); \"" + s.Str(p.mat) + "\" is not a liquid");
      if (p.firm != kNoStr) {
        if (!idOf(s.Str(p.firm), firm)) return bad(i, "firm material \"" + s.Str(p.firm) + "\" is unknown");
      } else if (!byDefault("wall", "stone", firm)) {
        return bad(i, "no firm material (no `wall` slot and no `stone`)");
      }
      if (klass(firm) != CLASS_SOLID) return bad(i, "the firm shell must be a SOLID (a powder shell would spill on its birth tick)");
    } else if (p.op == kOpFill || p.op == kOpSkin) {
      if (mat == 0) return bad(i, std::string(p.op == kOpFill ? "fill" : "skin") + " needs a mat");
    }
    if (p.op == kOpSkin && (p.skin < 1 || p.skin > 64)) return bad(i, "skin thickness must be 1..64 voxels");
    uint32_t mat2 = 0;
    if (p.mat2 != kNoStr && !idOf(s.Str(p.mat2), mat2)) return bad(i, "mat2 \"" + s.Str(p.mat2) + "\" does not resolve");
    if (p.openOnly && p.op != kOpFill) return bad(i, "openOnly is a fill's flag");
    if ((p.mat2 != kNoStr || p.checker != 0) && p.op != kOpFill && p.op != kOpSkin)
      return bad(i, "mat2 / checker pattern a fill or a skin");
    if (p.checker < 0 || p.checker > 256 || (p.checker > 0 && p.mat2 == kNoStr))
      return bad(i, "checker is 1..256 cells and needs a mat2");
    if (p.noiseOct < 0 || p.noiseOct > 3 || p.noiseAmp < 0 || p.noiseAmp > 64 ||
        (p.noiseOct > 0 && (p.noiseScale < 1 || p.noiseScale > 10)))
      return bad(i, "noise amp 0..64, scale 1..10, octaves 0..3");
    if (p.blend < 0 || p.blend > 256) return bad(i, "blend must be 0..256 voxels");
    if (p.shape == kShapeTube || p.shape == kShapeCapsule) {
      if (p.r < 0 || p.r > 512) return bad(i, "radius must be 0..512");
      if (p.shape == kShapeCapsule && (!coordOk(p.a) || !coordOk(p.b))) return bad(i, "a coordinate is past 2^20");
      for (uint32_t k = 0; k < p.tubeCount; k++) {
        const TubePt& t = s.tube[p.tubeFirst + k];
        if (t.r < 0 || t.r > 512 || !coordOk({t.x, t.y, t.z})) return bad(i, "tube point " + std::to_string(k) + " out of range");
      }
    } else {
      if (!coordOk(p.a) || !coordOk(p.b)) return bad(i, "a coordinate is past 2^20");
      for (int k = 0; k < 3; k++)
        if (p.b[k] < p.a[k] || p.b[k] - p.a[k] + 1 > 2048) return bad(i, "box axis " + std::to_string(k) + " must be 1..2048 cells (min <= max)");
      if (p.shape == kShapeRBox && (p.r < 0 || p.r > 512)) return bad(i, "rbox r must be 0..512 (it is clamped to half the shortest side)");
      if (p.shape == kShapeVault) {
        if (p.axis != 0 && p.axis != 2) return bad(i, "a vault runs along x or z");
        if (p.r < 0 || p.r > p.b[1] - p.a[1] + 1) return bad(i, "vault rise must be 0..the box height");
      }
      if (p.shape == kShapeCylinder && p.axis > 2) return bad(i, "cylinder axis must be x|y|z");
      if (p.shape == kShapeRamp) {
        if (p.axis > 3 || p.stepD < 1 || p.r < 0 || p.r > 2048 || p.height < 0 || p.below < 0 || p.height > 65535 ||
            p.below > 65535 || p.height + p.below < 1)
          return bad(i, "ramp: dir +x|-x|+z|-z, stepD >= 1, stepH 0..2048, height/below >= 0 and not both 0");
      }
    }
    if (p.scatterCount > 4) return bad(i, "at most 4 scatter rules per prim");
    out.primMat.push_back(mat);
    out.primFirm.push_back(firm);
    out.primMat2.push_back(mat2);
  }
  for (size_t i = 0; i < s.scatter.size(); i++) {
    const Scatter& c = s.scatter[i];
    if (c.sizeMin < 1 || c.sizeMax < c.sizeMin || c.sizeMax > 32 || c.densityQ16 > 65536 || c.spacing < 0 || c.spacing > 1024) {
      err = "scatter rule " + std::to_string(i) + ": size 1..32 (min <= max), density 0..1, spacing 0..1024";
      return false;
    }
    uint32_t id = 0;
    bool ok = false;
    if (c.mat != kNoStr) {
      ok = idOf(s.Str(c.mat), id);
    } else {
      switch (c.kind) {
        case kScRubble: ok = byDefault("rubble", "gravel", id); break;
        case kScStalagmite: case kScStalactite: ok = byDefault("wall", "stone", id); break;
        case kScCrystal: ok = byDefault("crystal", "crystal", id); break;
        case kScMoss: ok = byDefault("moss", "moss_patch", id); break;
        case kScPuddle: ok = byDefault("water", "water", id); break;
        case kScBones: ok = byDefault("bone", "bone", id); break;
        default: ok = byDefault("ore", "iron", id); break;
      }
    }
    if (!ok || id == 0) { err = "scatter rule " + std::to_string(i) + ": its material does not resolve"; return false; }
    if (c.kind == kScPuddle && klass(id) != CLASS_LIQUID) { err = "scatter rule " + std::to_string(i) + ": a puddle's mat must be a liquid"; return false; }
    if (c.kind != kScPuddle && klass(id) == CLASS_LIQUID) { err = "scatter rule " + std::to_string(i) + ": only a puddle may be a liquid"; return false; }
    out.scatterMat.push_back(id);
  }
  for (size_t i = 0; i < s.templates.size(); i++) {
    const Template& t = s.templates[i];
    const std::string w = "template " + std::to_string(i) + (t.id != kNoStr ? " (" + s.Str(t.id) + ")" : "");
    if (t.nx < 1 || t.ny < 1 || t.nz < 1 || t.nx > 256 || t.ny > 256 || t.nz > 256) { err = w + ": size must be 1..256 per axis"; return false; }
    std::vector<uint32_t> pal;
    for (uint32_t ps : t.palette) {
      const std::string& n = s.Str(ps);
      uint32_t id = 0;
      if (n == "@carve") { pal.push_back(kCarveMat); continue; }
      if (!idOf(n, id) || id == 0) { err = w + ": palette entry \"" + n + "\" does not resolve"; return false; }
      pal.push_back(id);
    }
    for (const auto& v : t.voxels)
      if (v[0] < 0 || v[1] < 0 || v[2] < 0 || v[0] >= t.nx || v[1] >= t.ny || v[2] >= t.nz || v[3] < 0 ||
          v[3] >= static_cast<int32_t>(pal.size())) {
        err = w + ": a voxel is outside the template or names no palette entry";
        return false;
      }
    out.templatePalette.push_back(std::move(pal));
  }
  for (size_t i = 0; i < s.instances.size(); i++)
    if (s.instances[i].tmpl >= s.templates.size()) { err = "instance " + std::to_string(i) + ": template index out of range"; return false; }
  return true;
}

// ============================================================================
// placement and bounds
// ============================================================================
namespace {
struct Box { std::array<int, 3> lo{}, hi{}; };
// One tube segment, local cells.
struct Seg { std::array<int, 3> a{}, b{}; int ra = 0, rb = 0; };

std::vector<Seg> SegmentsOf(const Shell& s, const Prim& p) {
  std::vector<std::array<int, 4>> pts;
  if (p.shape == kShapeCapsule) {
    pts.push_back({p.a[0], p.a[1], p.a[2], p.r});
    pts.push_back({p.b[0], p.b[1], p.b[2], p.r});
  } else {
    for (uint32_t k = 0; k < p.tubeCount; k++) {
      const TubePt& t = s.tube[p.tubeFirst + k];
      pts.push_back({t.x, t.y, t.z, t.r});
    }
  }
  std::vector<Seg> out;
  for (size_t k = 0; k + 1 < pts.size(); k++) {
    const auto& A = pts[k];
    const auto& B = pts[k + 1];
    const int span = std::max({std::abs(B[0] - A[0]), std::abs(B[1] - A[1]), std::abs(B[2] - A[2]), 1});
    // No axis may span more than 256 cells (dgSegD's products).
    const int n = (span + 255) / 256;
    auto at = [&](int i, int c) { return A[c] + ((B[c] - A[c]) * i) / n; };
    for (int i = 0; i < n; i++) {
      Seg sg;
      for (int c = 0; c < 3; c++) {
        sg.a[c] = at(i, c);
        sg.b[c] = at(i + 1, c);
      }
      sg.ra = at(i, 3);
      sg.rb = at(i + 1, 3);
      out.push_back(sg);
    }
  }
  return out;
}

// The prim's inflation in cells: everything that can make dgCell's answer for
// a cell outside the shape differ from "untouched" (PLAN §3.4.1).
int InflationOf(const Shell& s, const Prim& p) {
  int e = 3 + (p.noiseOct > 0 ? 2 * p.noiseAmp : 0);
  if (p.op == kOpCarve) e += p.blend + 2;   // the smooth-min fillet; the firm shell
  int band = 0;
  for (uint32_t k = 0; k < p.scatterCount; k++) {
    const Scatter& c = s.scatter[p.scatterFirst + k];
    if (c.kind == kScOre) band = std::max(band, c.sizeMax + 1);
    if (c.kind == kScMoss || c.kind == kScPuddle) band = std::max(band, 3);
  }
  e += band;
  // An elongated ellipse under-reads its distance by the axis ratio (dgEll):
  // widen by it, so the shape cannot reach past its listed cells.
  if (p.shape == kShapeEllipsoid || p.shape == kShapeCylinder || p.shape == kShapeVault) {
    int lo = INT_MAX, hi = 0;
    for (int k = 0; k < 3; k++) {
      if (p.shape == kShapeCylinder && static_cast<uint32_t>(k) == p.axis) continue;
      if (p.shape == kShapeVault && (static_cast<uint32_t>(k) == p.axis || k == 1)) continue;
      const int h = p.b[k] - p.a[k] + 1;
      lo = std::min(lo, h);
      hi = std::max(hi, h);
    }
    if (p.shape == kShapeVault) { lo = std::min(lo, std::max(p.r, 1)); hi = std::max(hi, std::max(p.r, 1)); }
    if (lo != INT_MAX && lo > 0) e = std::min(256, e * ((hi + lo - 1) / lo));
  }
  return e;
}

// The prim's local index boxes (one, or one per tube segment), inflated.
std::vector<Box> LocalBoxes(const Shell& s, const Prim& p) {
  const int e = InflationOf(s, p);
  std::vector<Box> out;
  if (p.shape == kShapeCapsule || p.shape == kShapeTube) {
    for (const Seg& g : SegmentsOf(s, p)) {
      const int r = std::max(g.ra, g.rb) + e;
      Box b;
      for (int c = 0; c < 3; c++) {
        b.lo[c] = std::min(g.a[c], g.b[c]) - r;
        b.hi[c] = std::max(g.a[c], g.b[c]) + r;
      }
      out.push_back(b);
    }
    return out;
  }
  Box b;
  for (int c = 0; c < 3; c++) {
    b.lo[c] = p.a[c] - e;
    b.hi[c] = p.b[c] + e;
  }
  if (p.shape == kShapeRamp) b.lo[1] = p.a[1] - p.below - e;
  out.push_back(b);
  return out;
}

Box ToWorld(const Box& l, int q, const std::array<int, 3>& o) {
  const int x0 = RotX(q, l.lo[0], l.lo[2]) + o[0], z0 = RotZ(q, l.lo[0], l.lo[2]) + o[2];
  const int x1 = RotX(q, l.hi[0], l.hi[2]) + o[0], z1 = RotZ(q, l.hi[0], l.hi[2]) + o[2];
  Box w;
  w.lo = {std::min(x0, x1), l.lo[1] + o[1], std::min(z0, z1)};
  w.hi = {std::max(x0, x1), l.hi[1] + o[1], std::max(z0, z1)};
  return w;
}

// A template's footprint dims after `q` quarter turns.
void RotDims(const Template& t, int q, int& nx, int& nz) {
  nx = (q & 1) ? t.nz : t.nx;
  nz = (q & 1) ? t.nx : t.nz;
}
Box InstanceLocalBox(const Shell& s, const Instance& in) {
  const Template& t = s.templates[in.tmpl];
  int nx, nz;
  RotDims(t, static_cast<int>(in.yawQ), nx, nz);
  Box b;
  b.lo = {in.x, in.y, in.z};
  b.hi = {in.x + nx - 1, in.y + t.ny - 1, in.z + nz - 1};
  return b;
}
}  // namespace

// world = base + R(q) (local - anchor) = R(q) local + (base - R(q) anchor).
std::array<int, 3> OriginFor(const Shell& s, int q, std::array<int, 3> base) {
  return {base[0] - RotX(q, s.anchor[0], s.anchor[2]), base[1] - s.anchor[1],
          base[2] - RotZ(q, s.anchor[0], s.anchor[2])};
}

void PlacedBounds(Placed& pl) {
  const Shell& s = *pl.shell;
  bool any = false;
  Box u;
  auto add = [&](const Box& w) {
    if (!any) { u = w; any = true; return; }
    for (int c = 0; c < 3; c++) {
      u.lo[c] = std::min(u.lo[c], w.lo[c]);
      u.hi[c] = std::max(u.hi[c], w.hi[c]);
    }
  };
  for (const Prim& p : s.prims)
    for (const Box& b : LocalBoxes(s, p)) add(ToWorld(b, pl.yawQ, pl.origin));
  for (const Instance& in : s.instances) add(ToWorld(InstanceLocalBox(s, in), pl.yawQ, pl.origin));
  if (!any) {
    u.lo = pl.origin;
    u.hi = pl.origin;
  }
  pl.boxMin = u.lo;
  pl.boxMax = u.hi;
  if (s.naturalCaves) {
    pl.keepMin = {1, 1, 1};
    pl.keepMax = {0, 0, 0};
  } else {
    const int k = std::max(0, s.keepOut);
    for (int c = 0; c < 3; c++) {
      pl.keepMin[c] = u.lo[c] - k;
      pl.keepMax[c] = u.hi[c] + k;
    }
  }
}

// ============================================================================
// the GPU block
// ============================================================================
namespace {
uint32_t U(int v) { return static_cast<uint32_t>(v); }
uint32_t KeyHash(int cx, int cy, int cz) {
  return (U(cx) * 73856093u) ^ (U(cy) * 19349663u) ^ (U(cz) * 83492791u);
}
// The template rotated by q quarter turns, as stamp run-lists (worldmap.h
// kStamp_*), offsets RELATIVE to the block's first word.
std::vector<uint32_t> PackTemplate(const Template& t, const std::vector<uint32_t>& pal, int q) {
  int nx, nz;
  RotDims(t, q, nx, nz);
  const int ny = t.ny;
  std::vector<uint32_t> grid(static_cast<size_t>(nx) * nz * ny, 0u);   // 0 = template air
  for (const auto& v : t.voxels) {
    int x = v[0], z = v[2];
    int rx = x, rz = z;
    switch (q & 3) {
      case 1: rx = z; rz = t.nx - 1 - x; break;
      case 2: rx = t.nx - 1 - x; rz = t.nz - 1 - z; break;
      case 3: rx = t.nz - 1 - z; rz = x; break;
      default: break;
    }
    grid[(static_cast<size_t>(rz) * nx + rx) * ny + v[1]] = pal[static_cast<size_t>(v[3])];
  }
  std::vector<uint32_t> W(4 + static_cast<size_t>(nx) * nz * 2, 0u);
  W[0] = U(nx);
  W[1] = U(ny);
  W[2] = U(nz);
  W[3] = 4u;
  for (int cz = 0; cz < nz; cz++)
    for (int cx = 0; cx < nx; cx++) {
      const size_t col = static_cast<size_t>(cz) * nx + cx;
      const uint32_t runOff = static_cast<uint32_t>(W.size());
      uint32_t count = 0;
      int y = 0;
      while (y < ny) {
        const uint32_t m = grid[col * ny + y];
        if (m == 0) { y++; continue; }
        int len = 1;
        while (y + len < ny && len < 31 && grid[col * ny + y + len] == m) len++;
        W.push_back((m & 0xFFFu) | (U(y) << 16) | (U(len) << 27));
        count++;
        y += len;
      }
      W[4 + col * 2] = runOff;
      W[4 + col * 2 + 1] = count;
    }
  return W;
}
}  // namespace

bool PackDungeons(const std::vector<Placed>& ds, std::vector<uint32_t>& W, std::string& log, PackStats* stats,
                  bool evalOff) {
  PackStats st;
  if (W.size() <= worldmap::kHDungeon) W.resize(worldmap::kHeaderWords, 0u);
  W[worldmap::kHDungeon] = 0u;
  if (ds.empty()) {
    if (stats) *stats = st;
    return true;
  }
  const uint32_t base = static_cast<uint32_t>(W.size());
  W.resize(base + kDgH_Words, 0u);
  W[base + kDgH_Count] = U(static_cast<int>(ds.size()));
  W[base + kDgH_Flags] = evalOff ? static_cast<uint32_t>(kDgFlagEvalOff) : 0u;
  // ---- the dungeon records ----
  const uint32_t recs = static_cast<uint32_t>(W.size());
  W[base + kDgH_Recs] = recs;
  W.resize(recs + ds.size() * kDgD_Words, 0u);
  Box gen, keep;
  bool keepAny = false;
  for (size_t d = 0; d < ds.size(); d++) {
    const Placed& pl = ds[d];
    uint32_t* r = W.data() + recs + d * kDgD_Words;
    r[kDgD_OriginX] = U(pl.origin[0]);
    r[kDgD_OriginY] = U(pl.origin[1]);
    r[kDgD_OriginZ] = U(pl.origin[2]);
    r[kDgD_Yaw] = U(pl.yawQ & 3);
    for (int c = 0; c < 3; c++) {
      r[kDgD_Box + c] = U(pl.boxMin[c]);
      r[kDgD_Box + 3 + c] = U(pl.boxMax[c]);
      r[kDgD_Keep + c] = U(pl.keepMin[c]);
      r[kDgD_Keep + 3 + c] = U(pl.keepMax[c]);
    }
    for (int c = 0; c < 3; c++) {
      gen.lo[c] = d == 0 ? pl.boxMin[c] : std::min(gen.lo[c], pl.boxMin[c]);
      gen.hi[c] = d == 0 ? pl.boxMax[c] : std::max(gen.hi[c], pl.boxMax[c]);
    }
    if (pl.keepMax[0] >= pl.keepMin[0]) {
      for (int c = 0; c < 3; c++) {
        keep.lo[c] = keepAny ? std::min(keep.lo[c], pl.keepMin[c]) : pl.keepMin[c];
        keep.hi[c] = keepAny ? std::max(keep.hi[c], pl.keepMax[c]) : pl.keepMax[c];
      }
      keepAny = true;
    }
  }
  for (int c = 0; c < 3; c++) {
    W[base + kDgH_GenX0 + c] = U(gen.lo[c]);
    W[base + kDgH_GenX1 + c] = U(gen.hi[c]);
    W[base + kDgH_KeepX0 + c] = U(keepAny ? keep.lo[c] : 1);
    W[base + kDgH_KeepX1 + c] = U(keepAny ? keep.hi[c] : 0);
  }
  // ---- prims, scatter, tube segments, templates, instances; and the cells ----
  using Key = std::tuple<int, int, int>;   // (cz, cy, cx): a fixed order
  std::map<Key, std::pair<std::vector<uint32_t>, std::vector<uint32_t>>> cells;   // prims, instances
  auto touch = [&](const Box& w, uint32_t off, bool inst) {
    const int s = kDgCellLog2;
    for (int cz = w.lo[2] >> s; cz <= (w.hi[2] >> s); cz++)
      for (int cy = w.lo[1] >> s; cy <= (w.hi[1] >> s); cy++)
        for (int cx = w.lo[0] >> s; cx <= (w.hi[0] >> s); cx++) {
          auto& e = cells[{cz, cy, cx}];
          std::vector<uint32_t>& v = inst ? e.second : e.first;
          if (v.empty() || v.back() != off) v.push_back(off);
        }
  };
  std::map<std::vector<uint32_t>, uint32_t> blocks;   // a packed template (relative) -> its offset
  for (size_t d = 0; d < ds.size(); d++) {
    const Placed& pl = ds[d];
    const Shell& s = *pl.shell;
    const Resolved& rs = *pl.res;
    const uint32_t drec = recs + static_cast<uint32_t>(d) * kDgD_Words;
    for (size_t i = 0; i < s.prims.size(); i++) {
      const Prim& p = s.prims[i];
      // Scatter rules first (the prim record points at them).
      const uint32_t scat = static_cast<uint32_t>(W.size());
      for (uint32_t k = 0; k < p.scatterCount; k++) {
        const Scatter& c = s.scatter[p.scatterFirst + k];
        const size_t at = W.size();
        W.resize(at + kDgS_Words, 0u);
        W[at + kDgS_Kind] = c.kind | (c.surface << 8);
        W[at + kDgS_Dens] = c.densityQ16;
        W[at + kDgS_Size] = U(c.sizeMin) | (U(c.sizeMax) << 16);
        W[at + kDgS_Mat] = rs.scatterMat[p.scatterFirst + k];
        W[at + kDgS_Seed] = c.seed;
        W[at + kDgS_Spacing] = U(c.spacing > 0 ? c.spacing : 2 * c.sizeMax * 3 + 4);
      }
      // Tube segments (capsule = one segment), Q2 local, with their boxes.
      uint32_t seg0 = 0, segN = 0;
      const bool tube = p.shape == kShapeCapsule || p.shape == kShapeTube;
      if (tube) {
        const int e = InflationOf(s, p);
        seg0 = static_cast<uint32_t>(W.size());
        for (const Seg& g : SegmentsOf(s, p)) {
          const size_t at = W.size();
          W.resize(at + kDgT_Words, 0u);
          const int rq = (std::max(g.ra, g.rb) + e) * 4;
          for (int c = 0; c < 3; c++) {
            W[at + c] = U(g.a[c] * 4 + 2);
            W[at + 3 + c] = U(g.b[c] * 4 + 2);
            W[at + 8 + c] = U(std::min(g.a[c], g.b[c]) * 4 + 2 - rq);
            W[at + 11 + c] = U(std::max(g.a[c], g.b[c]) * 4 + 2 + rq);
          }
          W[at + 6] = U(g.ra * 4);
          W[at + 7] = U(g.rb * 4);
          segN++;
        }
      }
      const uint32_t off = static_cast<uint32_t>(W.size());
      W.resize(off + kDgP_Words, 0u);
      uint32_t* r = W.data() + off;
      const uint32_t shape = tube ? static_cast<uint32_t>(kShapeCapsule) : p.shape;
      const uint32_t oct = static_cast<uint32_t>(std::clamp(p.noiseOct, 0, 3));
      r[kDgP_Flags] = (p.op & 3u) | (shape << 4) | ((p.axis & 3u) << 8) | ((p.scatterCount & 15u) << 12) |
                      (oct << 16) | ((static_cast<uint32_t>(std::clamp(p.noiseScale, 1, 15)) & 15u) << 20) |
                      (p.openOnly ? static_cast<uint32_t>(kDgPF_OpenOnly) : 0u);
      r[kDgP_Pat] = (rs.primMat2[i] & 0xFFFu) | (U(p.checker) << 16);
      r[kDgP_Mats] = (rs.primMat[i] & 0xFFFu) | ((rs.primFirm[i] & 0xFFFu) << 16);
      if (tube) {
        r[kDgP_P0] = seg0;
        r[kDgP_P1] = segN;
      } else if (p.shape == kShapeRamp) {
        for (int c = 0; c < 3; c++) {
          r[kDgP_AX + c] = U(p.a[c]);
          r[kDgP_BX + c] = U(p.b[c]);
        }
        r[kDgP_P0] = U(p.r);
        r[kDgP_P1] = U(p.stepD);
        r[kDgP_P2] = U(p.height) | (U(p.below) << 16);
      } else {
        for (int c = 0; c < 3; c++) {
          r[kDgP_AX + c] = U((p.a[c] + p.b[c] + 1) * 2);
          r[kDgP_BX + c] = U((p.b[c] - p.a[c] + 1) * 2);
        }
        // rbox r (clamped to half the shortest side: past it the rounding is a
        // capsule-ish box, which is what an author asking for more meant) / vault rise, Q2.
        const int minHalfQ2 = std::min({(p.b[0] - p.a[0] + 1) * 2, (p.b[1] - p.a[1] + 1) * 2, (p.b[2] - p.a[2] + 1) * 2});
        r[kDgP_P0] = U(p.shape == kShapeRBox ? std::min(p.r * 4, minHalfQ2) : p.r * 4);
      }
      int level = p.level;
      if (p.op == kOpCarve && rs.primMat[i] != 0 && level == kNoLevel) level = INT_MAX;
      r[kDgP_Level] = U(level);
      r[kDgP_NAmp] = U(oct > 0 ? p.noiseAmp * 4 : 0);
      r[kDgP_NSeed] = p.noiseSeed;
      r[kDgP_Blend] = (U(p.op == kOpCarve ? p.blend * 4 : 0) & 0xFFFFu) | (U(p.op == kOpSkin ? p.skin * 4 : 0) << 16);
      r[kDgP_Scat] = p.scatterCount ? scat : 0u;
      r[kDgP_DRec] = drec;
      for (const Box& b : LocalBoxes(s, p)) touch(ToWorld(b, pl.yawQ, pl.origin), off, false);
      st.prims++;
    }
    for (size_t i = 0; i < s.instances.size(); i++) {
      const Instance& in = s.instances[i];
      const int qt = static_cast<int>((in.yawQ + static_cast<uint32_t>(pl.yawQ)) & 3u);
      const std::vector<uint32_t> blk = PackTemplate(s.templates[in.tmpl], rs.templatePalette[in.tmpl], qt);
      uint32_t boff;
      auto it = blocks.find(blk);
      if (it != blocks.end()) {
        boff = it->second;
      } else {
        boff = static_cast<uint32_t>(W.size());
        std::vector<uint32_t> abs = blk;
        abs[3] += boff;
        const size_t cols = static_cast<size_t>(abs[0]) * abs[2];
        for (size_t c = 0; c < cols; c++) abs[4 + c * 2] += boff;
        W.insert(W.end(), abs.begin(), abs.end());
        blocks.emplace(blk, boff);
        st.templates++;
      }
      const Box wb = ToWorld(InstanceLocalBox(s, in), pl.yawQ, pl.origin);
      const uint32_t ir = static_cast<uint32_t>(W.size());
      W.push_back(boff);
      W.push_back(U(wb.lo[0]));
      W.push_back(U(wb.lo[1]));
      W.push_back(U(wb.lo[2]));
      touch(wb, ir, true);
      st.instances++;
    }
  }
  // ---- the lists (identical lists shared) ----
  std::map<std::vector<uint32_t>, uint32_t> lists;
  std::vector<std::pair<Key, uint32_t>> entries;
  Box cr;
  bool crAny = false;
  for (const auto& [key, v] : cells) {
    if (v.first.size() > kDgListMax || v.second.size() > kDgInstMax) {
      log += "dungeons: the 64^3 index cell (" + std::to_string(std::get<2>(key)) + "," + std::to_string(std::get<1>(key)) +
             "," + std::to_string(std::get<0>(key)) + ") is touched by " + std::to_string(v.first.size()) + " prims and " +
             std::to_string(v.second.size()) + " props (limits " + std::to_string(kDgListMax) + " / " +
             std::to_string(kDgInstMax) + "): split the piece, or move clutter into scatter rules\n";
      return false;
    }
    std::vector<uint32_t> lw;
    lw.push_back(U(static_cast<int>(v.first.size())));
    lw.insert(lw.end(), v.first.begin(), v.first.end());
    lw.push_back(U(static_cast<int>(v.second.size())));
    lw.insert(lw.end(), v.second.begin(), v.second.end());
    uint32_t loff;
    auto it = lists.find(lw);
    if (it != lists.end()) {
      loff = it->second;
    } else {
      loff = static_cast<uint32_t>(W.size());
      W.insert(W.end(), lw.begin(), lw.end());
      lists.emplace(lw, loff);
      st.listWords += static_cast<uint32_t>(lw.size());
    }
    st.worstList = std::max<uint32_t>(st.worstList, static_cast<uint32_t>(v.first.size()));
    entries.push_back({key, loff});
    const std::array<int, 3> c{std::get<2>(key), std::get<1>(key), std::get<0>(key)};
    for (int k = 0; k < 3; k++) {
      cr.lo[k] = crAny ? std::min(cr.lo[k], c[k]) : c[k];
      cr.hi[k] = crAny ? std::max(cr.hi[k], c[k]) : c[k];
    }
    crAny = true;
  }
  st.indexCells = static_cast<uint32_t>(entries.size());
  for (int c = 0; c < 3; c++) {
    W[base + kDgH_CellX0 + c] = U(crAny ? cr.lo[c] : 1);
    W[base + kDgH_CellX1 + c] = U(crAny ? cr.hi[c] : 0);
  }
  // ---- the hash table: at most half full, keys inserted in (cz, cy, cx) order ----
  uint32_t cap = 16;
  while (cap < 2 * entries.size()) cap <<= 1;
  const uint32_t tab = static_cast<uint32_t>(W.size());
  W.resize(tab + static_cast<size_t>(cap) * kDgE_Words, 0u);
  uint32_t probeMax = 0;
  for (const auto& [key, loff] : entries) {
    const int cx = std::get<2>(key), cy = std::get<1>(key), cz = std::get<0>(key);
    uint32_t i = KeyHash(cx, cy, cz) & (cap - 1);
    uint32_t probes = 0;
    while (W[tab + i * kDgE_Words + 3] != 0u) {
      i = (i + 1) & (cap - 1);
      probes++;
    }
    W[tab + i * kDgE_Words + 0] = U(cx);
    W[tab + i * kDgE_Words + 1] = U(cy);
    W[tab + i * kDgE_Words + 2] = U(cz);
    W[tab + i * kDgE_Words + 3] = loff;
    probeMax = std::max(probeMax, probes);
  }
  W[base + kDgH_Hash] = tab;
  W[base + kDgH_HashMask] = cap - 1;
  W[base + kDgH_ProbeMax] = probeMax;
  W[worldmap::kHDungeon] = base;
  st.hashCap = cap;
  st.probeMax = probeMax;
  st.words = static_cast<uint32_t>(W.size()) - base;
  if (stats) *stats = st;
  return true;
}

// ============================================================================
// THE CPU TWIN of worldgen_dungeon.wgsli
// ============================================================================
namespace twin {
// What the shader reads from its bindings, set by the entry points below.
const uint32_t* worldMap = nullptr;
const std::vector<MaterialDef>* gMats = nullptr;
uint32_t gSeed = 0;

// ---- the shader's types and builtins, spelt so the mirror block is the same tokens ----
inline int min(int a, int b) { return a < b ? a : b; }
inline int max(int a, int b) { return a > b ? a : b; }
inline uint32_t min(uint32_t a, uint32_t b) { return a < b ? a : b; }
inline uint32_t max(uint32_t a, uint32_t b) { return a > b ? a : b; }
inline int abs(int a) { return a < 0 ? -a : a; }
inline int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int select(int f, int t, bool cond) { return cond ? t : f; }   // WGSL's argument order
inline uint32_t firstLeadingBit(uint32_t v) {
  uint32_t b = 0;
  while (v >>= 1) b++;
  return b;
}
inline uint32_t hash3(uint32_t a, uint32_t b, uint32_t c) { return rng::Hash3(a, b, c); }
// worldgen.wgsl's Q14 noise primitives (the `noise` mirror) and fdiv.
inline int vsmooth(int t) {
  const int t2 = (t * t) >> 15;
  const int t3 = (t2 * t) >> 15;
  return 3 * t2 - 2 * t3;
}
inline int q15frac(int f, uint32_t csl) {
  if (csl <= 15u) return f << (15u - csl);
  return f >> (csl - 15u);
}
inline int vbilerp(int c00, int c10, int c01, int c11, int sx, int sz) {
  const int a = c00 + (((c10 - c00) * sx) >> 15);
  const int b = c01 + (((c11 - c01) * sx) >> 15);
  return a + (((b - a) * sz) >> 15);
}
inline int fdiv(int a, int b) {
  int q = a / b;
  if ((a % b) != 0 && ((a < 0) != (b < 0))) q -= 1;
  return q;
}
uint32_t matKlass(uint32_t m) { return m < gMats->size() ? (*gMats)[m].gpu.klass : static_cast<uint32_t>(CLASS_SOLID); }
int reposeRunOf(uint32_t mat) {
  const uint32_t w = mat < gMats->size() ? (*gMats)[mat].gpu.repose : 0u;
  const uint32_t a = (w >> kMatReposeCodeAShift) & kMatReposeCodeAMask;
  uint32_t b = a;
  if (((w >> kMatReposeBlendShift) & kMatReposeBlendMask) != 0u) b = (w >> kMatReposeCodeBShift) & kMatReposeCodeBMask;
  if (a == kRepose3To1 || b == kRepose3To1) return 3;
  if (a == kRepose2To1 || b == kRepose2To1) return 2;
  return 1;
}

constexpr uint32_t WM_H_DUNGEON = worldmap::kHDungeon;
constexpr uint32_t DG_H_COUNT = kDgH_Count, DG_H_RECS = kDgH_Recs, DG_H_HASH = kDgH_Hash,
                   DG_H_HASH_MASK = kDgH_HashMask, DG_H_PROBE_MAX = kDgH_ProbeMax, DG_H_CELL_X0 = kDgH_CellX0,
                   DG_H_CELL_Y0 = kDgH_CellY0, DG_H_CELL_Z0 = kDgH_CellZ0, DG_H_CELL_X1 = kDgH_CellX1,
                   DG_H_CELL_Y1 = kDgH_CellY1, DG_H_CELL_Z1 = kDgH_CellZ1;
// Read only by the shader's lookups outside the mirror (dgColIn / dgColTop /
// dgCaveKeep); bound here so check_invariants.py holds every DG_ const.
constexpr uint32_t DG_H_GEN_X0 = kDgH_GenX0, DG_H_GEN_Y0 = kDgH_GenY0, DG_H_GEN_Z0 = kDgH_GenZ0,
                   DG_H_GEN_X1 = kDgH_GenX1, DG_H_GEN_Y1 = kDgH_GenY1, DG_H_GEN_Z1 = kDgH_GenZ1,
                   DG_H_KEEP_X0 = kDgH_KeepX0, DG_H_KEEP_Y0 = kDgH_KeepY0, DG_H_KEEP_Z0 = kDgH_KeepZ0,
                   DG_H_KEEP_X1 = kDgH_KeepX1, DG_H_KEEP_Y1 = kDgH_KeepY1, DG_H_KEEP_Z1 = kDgH_KeepZ1,
                   DG_H_WORDS = kDgH_Words, DG_D_BOX = kDgD_Box, DG_D_KEEP = kDgD_Keep, DG_D_WORDS = kDgD_Words,
                   DG_P_WORDS = kDgP_Words, DG_I_WORDS = kDgI_Words, DG_H_FLAGS = kDgH_Flags,
                   DG_FLAG_EVAL_OFF = kDgFlagEvalOff;
constexpr uint32_t DG_D_ORIGIN_X = kDgD_OriginX, DG_D_ORIGIN_Y = kDgD_OriginY, DG_D_ORIGIN_Z = kDgD_OriginZ,
                   DG_D_YAW = kDgD_Yaw;
constexpr uint32_t DG_P_FLAGS = kDgP_Flags, DG_P_MATS = kDgP_Mats, DG_P_AX = kDgP_AX, DG_P_AY = kDgP_AY,
                   DG_P_AZ = kDgP_AZ, DG_P_BX = kDgP_BX, DG_P_BY = kDgP_BY, DG_P_BZ = kDgP_BZ, DG_P_P0 = kDgP_P0,
                   DG_P_P1 = kDgP_P1, DG_P_P2 = kDgP_P2, DG_P_LEVEL = kDgP_Level, DG_P_NAMP = kDgP_NAmp,
                   DG_P_NSEED = kDgP_NSeed, DG_P_BLEND = kDgP_Blend, DG_P_SCAT = kDgP_Scat, DG_P_DREC = kDgP_DRec,
                   DG_P_PAT = kDgP_Pat, DG_PF_OPEN_ONLY = kDgPF_OpenOnly;
constexpr uint32_t DG_S_KIND = kDgS_Kind, DG_S_DENS = kDgS_Dens, DG_S_SIZE = kDgS_Size, DG_S_MAT = kDgS_Mat,
                   DG_S_SEED = kDgS_Seed, DG_S_SPACING = kDgS_Spacing, DG_S_WORDS = kDgS_Words;
constexpr uint32_t DG_T_WORDS = kDgT_Words, DG_I_BLK = kDgI_Blk, DG_I_X = kDgI_X, DG_I_Y = kDgI_Y, DG_I_Z = kDgI_Z,
                   DG_E_WORDS = kDgE_Words, DG_CELL_LOG2 = kDgCellLog2, DG_LIST_MAX = kDgListMax;
constexpr uint32_t DG_OP_CARVE = kOpCarve, DG_OP_FILL = kOpFill, DG_OP_SKIN = kOpSkin, DG_OP_KEEP = kOpKeep;
constexpr uint32_t DG_SHAPE_BOX = kShapeBox, DG_SHAPE_RBOX = kShapeRBox, DG_SHAPE_ELLIPSOID = kShapeEllipsoid,
                   DG_SHAPE_CYLINDER = kShapeCylinder, DG_SHAPE_CAPSULE = kShapeCapsule, DG_SHAPE_RAMP = kShapeRamp,
                   DG_SHAPE_VAULT = kShapeVault;
constexpr uint32_t DG_SC_RUBBLE = kScRubble, DG_SC_STALAGMITE = kScStalagmite, DG_SC_STALACTITE = kScStalactite,
                   DG_SC_CRYSTAL = kScCrystal, DG_SC_MOSS = kScMoss, DG_SC_PUDDLE = kScPuddle, DG_SC_BONES = kScBones,
                   DG_SC_ORE = kScOre;
constexpr uint32_t DG_SURF_FLOOR = kSurfFloor, DG_SURF_CEIL = kSurfCeil, DG_SURF_WALL = kSurfWall;
constexpr uint32_t DG_CARVE_MAT = kCarveMat, DG_PROP_NONE = 0xFFFFFFFFu;
constexpr int DG_BIG = 1048576, DG_FIRM_Q2 = 8, DG_Q2_CLAMP = 8192;

inline int dgI(uint32_t i) { return static_cast<int>(worldMap[i]); }
bool dgFlowy(uint32_t m) {
  const uint32_t k = matKlass(m);
  return m != 0u && (k == CLASS_POWDER || k == CLASS_LIQUID);
}
uint32_t dgWord(uint32_t m, int x, int y, int z) {
  if (m == 0u) return 0u;
  const uint32_t rnd = hash3(gSeed ^ 0xC0FFEEu, static_cast<uint32_t>(x) ^ (static_cast<uint32_t>(z) << 12u),
                             static_cast<uint32_t>(y));
  uint32_t state = rnd % 3u;
  if (matKlass(m) == CLASS_LIQUID) state = 7u;   // LIQ_FULL_STATE
  return (m & 0xFFFu) | ((state & 0xFu) << 12u);   // packVox(m, state, STAMP_NEVER)
}
uint32_t dgListOfCell(int cx, int cy, int cz) {
  const uint32_t b = worldMap[WM_H_DUNGEON];
  if (b == 0u || (worldMap[b + DG_H_FLAGS] & DG_FLAG_EVAL_OFF) != 0u) return 0u;
  if (cx < dgI(b + DG_H_CELL_X0) || cy < dgI(b + DG_H_CELL_Y0) || cz < dgI(b + DG_H_CELL_Z0) ||
      cx > dgI(b + DG_H_CELL_X1) || cy > dgI(b + DG_H_CELL_Y1) || cz > dgI(b + DG_H_CELL_Z1))
    return 0u;
  const uint32_t tab = worldMap[b + DG_H_HASH];
  const uint32_t mask = worldMap[b + DG_H_HASH_MASK];
  const uint32_t pmax = worldMap[b + DG_H_PROBE_MAX];
  uint32_t i = KeyHash(cx, cy, cz) & mask;
  for (uint32_t k = 0; k <= pmax; k++) {
    const uint32_t e = tab + i * DG_E_WORDS;
    const uint32_t lst = worldMap[e + 3u];
    if (lst == 0u) return 0u;
    if (dgI(e) == cx && dgI(e + 1u) == cy && dgI(e + 2u) == cz) return lst;
    i = (i + 1u) & mask;
  }
  return 0u;
}
uint32_t dgListAt(int x, int y, int z) {
  return dgListOfCell(x >> DG_CELL_LOG2, y >> DG_CELL_LOG2, z >> DG_CELL_LOG2);
}

// MIRROR-BEGIN dungeon
// Integer square root, floor: Newton from above (the start is a power of two
// over the root, so the iterates fall monotonically to it).
uint32_t dgIsqrt(uint32_t v) {
  if (v < 2u) { return v; }
  uint32_t x = 1u << ((firstLeadingBit(v) >> 1u) + 1u);
  for (uint32_t i = 0u; i < 8u; i++) {
    const uint32_t y = (x + v / x) >> 1u;
    if (y >= x) { break; }
    x = y;
  }
  return x;
}
int dgClampQ(int v) { return clamp(v, -DG_Q2_CLAMP, DG_Q2_CLAMP); }
// Box distance from the per-axis outside distances q (Q2): exact.
int dgBox3(int qx, int qy, int qz) {
  const int ox = min(max(qx, 0), DG_Q2_CLAMP);
  const int oy = min(max(qy, 0), DG_Q2_CLAMP);
  const int oz = min(max(qz, 0), DG_Q2_CLAMP);
  return int(dgIsqrt(uint32_t(ox * ox + oy * oy + oz * oz))) + min(max(qx, max(qy, qz)), 0);
}
int dgBox2(int qa, int qb) {
  const int oa = min(max(qa, 0), 16384);
  const int ob = min(max(qb, 0), 16384);
  return int(dgIsqrt(uint32_t(oa * oa + ob * ob))) + min(max(qa, qb), 0);
}
// A coordinate over a semi-axis, Q12 (4096 = on the surface), clamped at 4x.
int dgNorm(int d, int h) { return clamp((d * 4096) / max(h, 1), -16384, 16384); }
// Ellipsoid / elliptic-section distance: the normalised radius scaled by the
// shortest semi-axis -- never more than the true distance (the packer inflates
// an elongated shape's index bounds by the ratio for that reason).
int dgEll(int k, int h) { return ((k - 4096) * h) >> 12u; }

// Quarter-turn rotation of cell coordinates (PLAN §3.4.1 / §3.7 R), linear:
// q1 (x, z) -> (z, -x). world = R(yaw) local + origin.
int dgRotX(uint32_t q, int x, int z) {
  if (q == 1u) { return z; }
  if (q == 2u) { return -x; }
  if (q == 3u) { return -z; }
  return x;
}
int dgRotZ(uint32_t q, int x, int z) {
  if (q == 1u) { return -x; }
  if (q == 2u) { return -z; }
  if (q == 3u) { return x; }
  return z;
}
// A world cell in a prim's dungeon's LOCAL frame (the inverse turn).
int dgLocalX(uint32_t dr, int x, int z) {
  const uint32_t qi = (4u - worldMap[dr + DG_D_YAW]) & 3u;
  return dgRotX(qi, x - dgI(dr + DG_D_ORIGIN_X), z - dgI(dr + DG_D_ORIGIN_Z));
}
int dgLocalZ(uint32_t dr, int x, int z) {
  const uint32_t qi = (4u - worldMap[dr + DG_D_YAW]) & 3u;
  return dgRotZ(qi, x - dgI(dr + DG_D_ORIGIN_X), z - dgI(dr + DG_D_ORIGIN_Z));
}

// 3D value noise, Q14 0..16383, on a 2^csl lattice of LOCAL cells.
int dgNoise3(int x, int y, int z, uint32_t csl, uint32_t seed) {
  const int gx = x >> csl;
  const int gy = y >> csl;
  const int gz = z >> csl;
  const int mask = int((1u << csl) - 1u);
  const int sx = vsmooth(q15frac(x & mask, csl));
  const int sy = vsmooth(q15frac(y & mask, csl));
  const int sz = vsmooth(q15frac(z & mask, csl));
  const uint32_t s0 = seed ^ (uint32_t(gy) * 0x9E3779B1u);
  const uint32_t s1 = seed ^ (uint32_t(gy + 1) * 0x9E3779B1u);
  const int a00 = int(hash3(s0, uint32_t(gx), uint32_t(gz)) & 0x3FFFu);
  const int a10 = int(hash3(s0, uint32_t(gx + 1), uint32_t(gz)) & 0x3FFFu);
  const int a01 = int(hash3(s0, uint32_t(gx), uint32_t(gz + 1)) & 0x3FFFu);
  const int a11 = int(hash3(s0, uint32_t(gx + 1), uint32_t(gz + 1)) & 0x3FFFu);
  const int b00 = int(hash3(s1, uint32_t(gx), uint32_t(gz)) & 0x3FFFu);
  const int b10 = int(hash3(s1, uint32_t(gx + 1), uint32_t(gz)) & 0x3FFFu);
  const int b01 = int(hash3(s1, uint32_t(gx), uint32_t(gz + 1)) & 0x3FFFu);
  const int b11 = int(hash3(s1, uint32_t(gx + 1), uint32_t(gz + 1)) & 0x3FFFu);
  const int n0 = vbilerp(a00, a10, a01, a11, sx, sz);
  const int n1 = vbilerp(b00, b10, b01, b11, sx, sz);
  return n0 + (((n1 - n0) * sy) >> 15u);
}
// The surface displacement, Q2, positive = the solid shrinks. Octave o has
// half the amplitude and half the cell of octave o - 1.
int dgDisp(uint32_t p, int lx, int ly, int lz) {
  const uint32_t fl = worldMap[p + DG_P_FLAGS];
  const uint32_t oct = (fl >> 16u) & 3u;
  if (oct == 0u) { return 0; }
  uint32_t csl = (fl >> 20u) & 15u;
  int amp = dgI(p + DG_P_NAMP);
  const uint32_t seed = worldMap[p + DG_P_NSEED];
  int d = 0;
  for (uint32_t o = 0u; o < oct; o++) {
    d += ((dgNoise3(lx, ly, lz, csl, seed + o * 0x632BE5ABu) - 8192) * amp) >> 13u;
    amp = amp >> 1u;
    if (csl > 1u) { csl -= 1u; }
  }
  return d;
}

// The stair profile's solid, in CELLS, as an L-infinity distance in Q2.
int dgRampD(uint32_t p, int lx, int ly, int lz) {
  const uint32_t dir = (worldMap[p + DG_P_FLAGS] >> 8u) & 3u;
  const int x0 = dgI(p + DG_P_AX);
  const int y0 = dgI(p + DG_P_AY);
  const int z0 = dgI(p + DG_P_AZ);
  const int x1 = dgI(p + DG_P_BX);
  const int y1 = dgI(p + DG_P_BY);
  const int z1 = dgI(p + DG_P_BZ);
  int t = lx - x0;
  int span = x1 - x0;
  if (dir == 1u) { t = x1 - lx; }
  if (dir == 2u) { t = lz - z0; span = z1 - z0; }
  if (dir == 3u) { t = z1 - lz; span = z1 - z0; }
  const int prof = y0 + (clamp(t, 0, span) / max(dgI(p + DG_P_P1), 1)) * dgI(p + DG_P_P0);
  const uint32_t hb = worldMap[p + DG_P_P2];
  const int lo = prof - int(hb >> 16u);
  const int hi = min(prof + int(hb & 0xFFFFu) - 1, y1);
  const int px = lx * 4 + 2;
  const int py = ly * 4 + 2;
  const int pz = lz * 4 + 2;
  const int dx = max(x0 * 4 - px, px - (x1 + 1) * 4);
  const int dy = max(lo * 4 - py, py - (hi + 1) * 4);
  const int dz = max(z0 * 4 - pz, pz - (z1 + 1) * 4);
  return max(dx, max(dy, dz));
}
// One tapered segment of a tube (Q2): the closest point of a-b by a Q8
// fraction, the radius lerped by the same fraction. The packer splits a
// segment so no axis spans more than 256 cells (the products stay < 2^31).
int dgSegD(uint32_t s, int px, int py, int pz) {
  const int ax = dgI(s);
  const int ay = dgI(s + 1u);
  const int az = dgI(s + 2u);
  const int bax = dgI(s + 3u) - ax;
  const int bay = dgI(s + 4u) - ay;
  const int baz = dgI(s + 5u) - az;
  const int pax = dgClampQ(px - ax);
  const int pay = dgClampQ(py - ay);
  const int paz = dgClampQ(pz - az);
  const int len2 = bax * bax + bay * bay + baz * baz;
  const int t = clamp(pax * bax + pay * bay + paz * baz, 0, len2);
  const int tq = (t << 8u) / max(len2, 1);
  const int ex = pax - ((bax * tq) >> 8u);
  const int ey = pay - ((bay * tq) >> 8u);
  const int ez = paz - ((baz * tq) >> 8u);
  const int ra = dgI(s + 6u);
  const int rb = dgI(s + 7u);
  return int(dgIsqrt(uint32_t(ex * ex + ey * ey + ez * ez))) - (ra + (((rb - ra) * tq) >> 8u));
}
// A capsule or a tube: the HARD minimum over its segments (so a densified
// spline has no smooth-min bulge at every joint), each segment skipped unless
// the cell is inside its inflated box -- outside it the segment cannot decide
// anything, exactly as a prim outside a cell's list cannot.
int dgTubeD(uint32_t p, int px, int py, int pz) {
  const uint32_t s0 = worldMap[p + DG_P_P0];
  const uint32_t n = worldMap[p + DG_P_P1];
  int d = DG_BIG;
  for (uint32_t k = 0u; k < n; k++) {
    const uint32_t s = s0 + k * DG_T_WORDS;
    if (px < dgI(s + 8u) || py < dgI(s + 9u) || pz < dgI(s + 10u) || px > dgI(s + 11u) ||
        py > dgI(s + 12u) || pz > dgI(s + 13u)) {
      continue;
    }
    d = min(d, dgSegD(s, px, py, pz));
  }
  return d;
}
// The shape's signed distance at local cell (lx, ly, lz), Q2, negative inside.
int dgShapeD(uint32_t p, int lx, int ly, int lz) {
  const uint32_t fl = worldMap[p + DG_P_FLAGS];
  const uint32_t shape = (fl >> 4u) & 15u;
  const uint32_t axis = (fl >> 8u) & 3u;
  if (shape == DG_SHAPE_RAMP) { return dgRampD(p, lx, ly, lz); }
  const int px = lx * 4 + 2;
  const int py = ly * 4 + 2;
  const int pz = lz * 4 + 2;
  if (shape == DG_SHAPE_CAPSULE) { return dgTubeD(p, px, py, pz); }
  const int dx = dgClampQ(px - dgI(p + DG_P_AX));
  const int dy = dgClampQ(py - dgI(p + DG_P_AY));
  const int dz = dgClampQ(pz - dgI(p + DG_P_AZ));
  const int hx = dgI(p + DG_P_BX);
  const int hy = dgI(p + DG_P_BY);
  const int hz = dgI(p + DG_P_BZ);
  if (shape == DG_SHAPE_BOX) { return dgBox3(abs(dx) - hx, abs(dy) - hy, abs(dz) - hz); }
  if (shape == DG_SHAPE_RBOX) {
    const int r = dgI(p + DG_P_P0);
    return dgBox3(abs(dx) - (hx - r), abs(dy) - (hy - r), abs(dz) - (hz - r)) - r;
  }
  if (shape == DG_SHAPE_ELLIPSOID) {
    const int nx = dgNorm(dx, hx);
    const int ny = dgNorm(dy, hy);
    const int nz = dgNorm(dz, hz);
    return dgEll(int(dgIsqrt(uint32_t(nx * nx + ny * ny + nz * nz))), min(hx, min(hy, hz)));
  }
  if (shape == DG_SHAPE_CYLINDER) {
    int du = dy;
    int dv = dz;
    int hu = hy;
    int hv = hz;
    int da = dx;
    int ha = hx;
    if (axis == 1u) { du = dx; hu = hx; da = dy; ha = hy; }
    if (axis == 2u) { dv = dy; hv = hy; da = dz; ha = hz; du = dx; hu = hx; }
    const int nu = dgNorm(du, hu);
    const int nv = dgNorm(dv, hv);
    const int dr = dgEll(int(dgIsqrt(uint32_t(nu * nu + nv * nv))), min(hu, hv));
    return dgBox2(dr, abs(da) - ha);
  }
  // DG_SHAPE_VAULT: walls up to the springing line, a half-ellipse over it.
  const int rise = dgI(p + DG_P_P0);
  int du = dz;
  int hu = hz;
  int da = dx;
  int ha = hx;
  if (axis == 2u) { du = dx; hu = hx; da = dz; ha = hz; }
  const int wall = hy - rise;
  const int rect = dgBox2(abs(du) - hu, max(-hy - dy, dy - wall));
  const int nu = dgNorm(du, hu);
  const int nv = dgNorm(dy - wall, rise);
  const int ell = max(dgEll(int(dgIsqrt(uint32_t(nu * nu + nv * nv))), min(hu, max(rise, 1))), wall - dy);
  return dgBox2(max(min(rect, ell), -hy - dy), abs(da) - ha);
}
// One prim's distance at a WORLD cell: its shape in its dungeon's frame, plus
// the noise.
int dgPrimD(uint32_t p, int x, int y, int z) {
  const uint32_t dr = worldMap[p + DG_P_DREC];
  const int lx = dgLocalX(dr, x, z);
  const int ly = y - dgI(dr + DG_D_ORIGIN_Y);
  const int lz = dgLocalZ(dr, x, z);
  return dgShapeD(p, lx, ly, lz) + dgDisp(p, lx, ly, lz);
}
// Polynomial smooth minimum, radius k (Q2): exact min where |a - b| >= k.
int dgSmin(int a, int b, int k) {
  if (k <= 0) { return min(a, b); }
  const int h = max(k - abs(a - b), 0);
  return min(a, b) - (h * h) / (4 * k);
}
// The carve field C at a world cell, from THAT cell's own list (so C is a
// function of the cell alone, whichever neighbour asks).
int dgCarveIn(uint32_t lst, int x, int y, int z) {
  const uint32_t n = min(worldMap[lst], DG_LIST_MAX);
  int c = DG_BIG;
  for (uint32_t k = 0u; k < n; k++) {
    const uint32_t p = worldMap[lst + 1u + k];
    if ((worldMap[p + DG_P_FLAGS] & 3u) != DG_OP_CARVE) { continue; }
    c = dgSmin(c, dgPrimD(p, x, y, z), int(worldMap[p + DG_P_BLEND] & 0xFFFFu));
  }
  return c;
}
int dgCarveAt(int x, int y, int z) {
  const uint32_t lst = dgListAt(x, y, z);
  if (lst == 0u) { return DG_BIG; }
  return dgCarveIn(lst, x, y, z);
}
// What a carved cell holds: air, or the carve's liquid at or under its level.
uint32_t dgCarveWord(uint32_t p, int x, int y, int z) {
  const uint32_t lm = worldMap[p + DG_P_MATS] & 0xFFFu;
  if (lm == 0u) { return 0u; }
  if (y - dgI(worldMap[p + DG_P_DREC] + DG_D_ORIGIN_Y) > dgI(p + DG_P_LEVEL)) { return 0u; }
  return dgWord(lm, x, y, z);
}
// The cell of a prop instance's template at world (x, y, z): its material,
// DG_CARVE_MAT, or DG_PROP_NONE (template air: leave the world alone). The
// stamp run-list format (worldmap.h kStamp_*), any y.
uint32_t dgPropCell(uint32_t ir, int x, int y, int z) {
  const uint32_t blk = worldMap[ir + DG_I_BLK];
  const int lx = x - dgI(ir + DG_I_X);
  const int ly = y - dgI(ir + DG_I_Y);
  const int lz = z - dgI(ir + DG_I_Z);
  const int nx = int(worldMap[blk]);
  if (lx < 0 || ly < 0 || lz < 0 || lx >= nx || ly >= int(worldMap[blk + 1u]) || lz >= int(worldMap[blk + 2u])) {
    return DG_PROP_NONE;
  }
  const uint32_t ci = worldMap[blk + 3u] + uint32_t(lz * nx + lx) * 2u;
  const uint32_t runOff = worldMap[ci];
  const uint32_t cnt = worldMap[ci + 1u];
  for (uint32_t k = 0u; k < cnt; k++) {
    const uint32_t r = worldMap[runOff + k];
    const int y0 = int((r >> 16u) & 2047u);
    if (ly < y0) { return DG_PROP_NONE; }
    if (ly < y0 + int((r >> 27u) & 31u)) { return r & 0xFFFu; }
  }
  return DG_PROP_NONE;
}
// One scatter rule of prim p over the cell (word w so far, terrain word tw,
// the prim's own distance dOwn, the carve field c). PLAN §3.4.1 "Scatter".
uint32_t dgScatter(uint32_t s, uint32_t p, int dOwn, int c, uint32_t w, uint32_t tw, int x, int y, int z) {
  const uint32_t kind = worldMap[s + DG_S_KIND] & 255u;
  const uint32_t surf = (worldMap[s + DG_S_KIND] >> 8u) & 255u;
  const uint32_t dens = worldMap[s + DG_S_DENS];
  const int smin = int(worldMap[s + DG_S_SIZE] & 0xFFFFu);
  const int smax = int(worldMap[s + DG_S_SIZE] >> 16u);
  const uint32_t mat = worldMap[s + DG_S_MAT];
  const uint32_t seed = worldMap[s + DG_S_SEED];
  const int sp = max(int(worldMap[s + DG_S_SPACING]), 4);
  const uint32_t dr = worldMap[p + DG_P_DREC];
  const int lx = dgLocalX(dr, x, z);
  const int ly = y - dgI(dr + DG_D_ORIGIN_Y);
  const int lz = dgLocalZ(dr, x, z);
  // THE SOLID BAND kinds (moss, ore, puddle) work on untouched, non-air
  // terrain just behind this prim's surface; the others on an air cell inside
  // it. Every kind's lattice feature, if it has one, is one disc per cell of a
  // `sp` lattice in the local XZ plane.
  const bool band = kind == DG_SC_MOSS || kind == DG_SC_ORE || kind == DG_SC_PUDDLE;
  if (band) {
    if (w != tw || c < 0 || (tw & 0xFFFu) == 0u || dOwn < 0) { return w; }
  } else {
    if (w != 0u || dOwn >= 0) { return w; }
  }
  int n3 = 0;
  if (kind == DG_SC_MOSS || kind == DG_SC_ORE) { n3 = dgNoise3(lx, ly, lz, 3u, seed); }
  if (kind == DG_SC_ORE) {
    if (dOwn <= smax * 4 && n3 > 16383 - int(dens >> 2u)) { return dgWord(mat, x, y, z); }
    return w;
  }
  if (band && dOwn > DG_FIRM_Q2) { return w; }
  int nProbe = 6;
  bool down = true;
  if (kind != DG_SC_MOSS) {
    const int gx = fdiv(lx, sp);
    const int gz = fdiv(lz, sp);
    const uint32_t h = hash3(seed ^ 0xD6A1u, uint32_t(gx), uint32_t(gz));
    if ((h & 0xFFFFu) >= dens) { return w; }
    const int size = smin + int((h >> 16u) % uint32_t(max(smax - smin + 1, 1)));
    int run = 1;
    int r = max(size / 3, 1);
    if (kind == DG_SC_RUBBLE) { run = reposeRunOf(mat); r = size * run; }
    if (kind == DG_SC_CRYSTAL) { r = max(size / 2, 1); }
    if (kind == DG_SC_BONES || kind == DG_SC_PUDDLE) { r = size; }
    r = min(r, (sp - 1) / 2);
    const uint32_t h2 = hash3(seed ^ 0x51A7u, uint32_t(gx), uint32_t(gz));
    const int span = max(sp - 2 * r, 1);
    const int ax = abs(lx - (gx * sp + r + int(h2 % uint32_t(span))));
    const int az = abs(lz - (gz * sp + r + int((h2 >> 16u) % uint32_t(span))));
    const int od = max(max(ax, az), (3 * (ax + az)) / 4);
    if (od > r) { return w; }
    if (!band) {
      int hh = 1;
      if (kind == DG_SC_RUBBLE) { hh = size - od / run; }
      else if (kind != DG_SC_BONES) { hh = (size * (r - od)) / max(r, 1); }
      if (hh < 1) { return w; }
      if (kind == DG_SC_BONES && (hash3(seed ^ 0xB0E5u, uint32_t(lx), uint32_t(lz)) & 1u) == 0u) { return w; }
      if (kind == DG_SC_STALACTITE) { down = false; }
      if (kind == DG_SC_CRYSTAL) {
        if (surf == DG_SURF_CEIL) { down = false; }
        else if (surf != DG_SURF_FLOOR && ((h2 >> 31u) & 1u) == 1u) { down = false; }
      }
      nProbe = hh;
    }
  }
  // THE PROBES, as ONE rolled loop (one inlined copy of the carve field): the
  // band kinds ask the six face neighbours (bit 0 up, 1 down, 2..5 the
  // sides); a floor / ceiling feature walks down (up) its own height for the
  // first solid cell it can grow from -- every cell between is carved, so the
  // column is contiguous.
  uint32_t mask = 0u;
  bool found = false;
  for (int i = 0; i < nProbe; i++) {
    int dx = 0;
    int dy = 0;
    int dz = 0;
    if (band) {
      if (i == 0) { dy = 1; }
      else if (i == 1) { dy = -1; }
      else if (i == 2) { dx = 1; }
      else if (i == 3) { dx = -1; }
      else if (i == 4) { dz = 1; }
      else { dz = -1; }
    } else {
      dy = select(i + 1, -1 - i, down);
    }
    if (dgCarveAt(x + dx, y + dy, z + dz) < 0) { mask = mask | (1u << uint32_t(i)); }
    else if (!band) { found = true; break; }
  }
  if (!band) {
    if (found) { return dgWord(mat, x, y, z); }
    return w;
  }
  if (kind == DG_SC_PUDDLE) {
    // A floor cell under air whose four sides and underside are solid: one
    // cell of liquid, contained by construction (PLAN: "puddle").
    if ((mask & 1u) != 0u && (mask & 62u) == 0u) { return dgWord(mat, x, y, z); }
    return w;
  }
  // MOSS: the first solid cell behind the surface on the authored side(s), in
  // patches.
  bool face = mask != 0u;
  if (surf == DG_SURF_FLOOR) { face = (mask & 1u) != 0u; }
  else if (surf == DG_SURF_CEIL) { face = (mask & 2u) != 0u; }
  else if (surf == DG_SURF_WALL) { face = (mask & 60u) != 0u; }
  if (face && n3 < int(dens >> 2u)) { return dgWord(mat, x, y, z); }
  return w;
}

// A fill's / skin's material at a world cell: `mat`, or the checker of
// `tile`-cell squares in the dungeon's LOCAL XZ plane alternating mat / mat2.
uint32_t dgFillMat(uint32_t p, int x, int z) {
  const uint32_t m = worldMap[p + DG_P_MATS] & 0xFFFu;
  const uint32_t pat = worldMap[p + DG_P_PAT];
  const int tile = int(pat >> 16u);
  if (tile == 0) { return m; }
  const uint32_t dr = worldMap[p + DG_P_DREC];
  const int cx = fdiv(dgLocalX(dr, x, z), tile);
  const int cz = fdiv(dgLocalZ(dr, x, z), tile);
  if (((cx + cz) & 1) != 0) { return pat & 0xFFFu; }
  return m;
}
// THE CELL (PLAN §3.4.1 "Op semantics"): terrain word tw in, the word out.
uint32_t dgCell(uint32_t lst, int x, int y, int z, uint32_t tw) {
  const uint32_t nAll = worldMap[lst];
  const uint32_t n = min(nAll, DG_LIST_MAX);
  int dd[DG_LIST_MAX];
  int c = DG_BIG;
  int own = DG_BIG;
  uint32_t ownP = 0u;
  for (uint32_t k = 0u; k < n; k++) {
    const uint32_t p = worldMap[lst + 1u + k];
    const int d = dgPrimD(p, x, y, z);
    dd[k] = d;
    if ((worldMap[p + DG_P_FLAGS] & 3u) == DG_OP_CARVE) {
      c = dgSmin(c, d, int(worldMap[p + DG_P_BLEND] & 0xFFFFu));
      if (d < own) { own = d; ownP = p; }
    }
  }
  const uint32_t tm = tw & 0xFFFu;
  uint32_t w = tw;
  // 1. the carve; 2. the firm shell round it.
  if (c < 0) {
    w = dgCarveWord(ownP, x, y, z);
  } else if (c < DG_FIRM_Q2 && ownP != 0u && dgFlowy(tm)) {
    w = dgWord(worldMap[ownP + DG_P_MATS] >> 16u, x, y, z);
  }
  // 3. the ordered pass, later wins; a kept cell is not re-cut.
  bool kept = false;
  for (uint32_t k = 0u; k < n; k++) {
    if (dd[k] >= 0) { continue; }
    const uint32_t p = worldMap[lst + 1u + k];
    const uint32_t op = worldMap[p + DG_P_FLAGS] & 3u;
    if (op == DG_OP_FILL) {
      if ((worldMap[p + DG_P_FLAGS] & DG_PF_OPEN_ONLY) == 0u || (w & 0xFFFu) == 0u) {
        w = dgWord(dgFillMat(p, x, z), x, y, z);
      }
    } else if (op == DG_OP_KEEP) {
      w = tw;
      kept = true;
    } else if (op == DG_OP_SKIN) {
      if (c >= 0 && c <= int(worldMap[p + DG_P_BLEND] >> 16u) && tm != 0u) {
        w = dgWord(dgFillMat(p, x, z), x, y, z);
      }
    } else if (!kept) {
      w = dgCarveWord(p, x, y, z);
    }
  }
  // 4. scatter, prim by prim.
  for (uint32_t k = 0u; k < n; k++) {
    const uint32_t p = worldMap[lst + 1u + k];
    const uint32_t ns = (worldMap[p + DG_P_FLAGS] >> 12u) & 15u;
    for (uint32_t j = 0u; j < ns; j++) {
      w = dgScatter(worldMap[p + DG_P_SCAT] + j * DG_S_WORDS, p, dd[k], c, w, tw, x, y, z);
    }
  }
  // 5. the props, later wins: CARVE clears, template air leaves alone.
  const uint32_t ni = worldMap[lst + 1u + nAll];
  for (uint32_t k = 0u; k < ni; k++) {
    const uint32_t m = dgPropCell(worldMap[lst + 2u + nAll + k], x, y, z);
    if (m == DG_CARVE_MAT) { w = 0u; }
    else if (m != DG_PROP_NONE) { w = dgWord(m, x, y, z); }
  }
  return w;
}
// MIRROR-END dungeon
}  // namespace twin

uint32_t TwinListAt(const std::vector<uint32_t>& W, int x, int y, int z) {
  if (W.size() <= worldmap::kHDungeon || W[worldmap::kHDungeon] == 0u) return 0u;
  twin::worldMap = W.data();
  return twin::dgListAt(x, y, z);
}
uint32_t TwinCell(const std::vector<uint32_t>& W, const std::vector<MaterialDef>& mats, uint32_t seed, int x, int y,
                  int z, uint32_t tw) {
  const uint32_t lst = TwinListAt(W, x, y, z);
  if (lst == 0u) return tw;
  twin::worldMap = W.data();
  twin::gMats = &mats;
  twin::gSeed = seed;
  return twin::dgCell(lst, x, y, z, tw);
}
int TwinCarveAt(const std::vector<uint32_t>& W, const std::vector<MaterialDef>& mats, uint32_t seed, int x, int y,
                int z) {
  if (W.size() <= worldmap::kHDungeon || W[worldmap::kHDungeon] == 0u) return twin::DG_BIG;
  twin::worldMap = W.data();
  twin::gMats = &mats;
  twin::gSeed = seed;
  return twin::dgCarveAt(x, y, z);
}

}  // namespace dungeon
