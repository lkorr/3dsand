#include "sim/voxload.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <unordered_map>

namespace {

// ---- little-endian cursor over the file bytes ------------------------------
struct Cursor {
  const uint8_t* p;
  size_t len, off = 0;
  bool ok = true;

  uint32_t U32() {
    if (off + 4 > len) { ok = false; return 0; }
    uint32_t v;
    std::memcpy(&v, p + off, 4);
    off += 4;
    return v;
  }
  int32_t I32() { return (int32_t)U32(); }
  std::string Str() {
    uint32_t n = U32();
    if (!ok || off + n > len) { ok = false; return {}; }
    std::string s((const char*)p + off, n);
    off += n;
    return s;
  }
  // DICT: count, then count * (STRING key, STRING value)
  std::map<std::string, std::string> Dict() {
    std::map<std::string, std::string> d;
    uint32_t n = U32();
    for (uint32_t i = 0; i < n && ok; i++) {
      std::string k = Str();
      d[k] = Str();
    }
    return d;
  }
  void Skip(size_t n) {
    if (off + n > len) { ok = false; return; }
    off += n;
  }
};

// ---- scene graph ------------------------------------------------------------
// 3x3 rotation with entries in {-1,0,1}, decoded from the .vox _r byte:
// bits 0..1 = column of the 1 in row 0, bits 2..3 = column in row 1,
// bits 4..6 = sign of rows 0..2. Row 2's column is the remaining one.
struct Rot {
  int8_t m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};

Rot DecodeRot(int byte) {
  Rot r{};
  std::memset(r.m, 0, sizeof(r.m));
  int c0 = byte & 3, c1 = (byte >> 2) & 3;
  int c2 = 3 - c0 - c1;
  if (c0 > 2 || c1 > 2 || c2 < 0 || c2 > 2 || c0 == c1) return Rot{};  // malformed: identity
  r.m[0][c0] = (byte >> 4) & 1 ? -1 : 1;
  r.m[1][c1] = (byte >> 5) & 1 ? -1 : 1;
  r.m[2][c2] = (byte >> 6) & 1 ? -1 : 1;
  return r;
}

IVec3 RotApply(const Rot& r, IVec3 v) {
  return {r.m[0][0] * v.x + r.m[0][1] * v.y + r.m[0][2] * v.z,
          r.m[1][0] * v.x + r.m[1][1] * v.y + r.m[1][2] * v.z,
          r.m[2][0] * v.x + r.m[2][1] * v.y + r.m[2][2] * v.z};
}

Rot RotCompose(const Rot& a, const Rot& b) {
  Rot r{};
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) {
      int s = 0;
      for (int k = 0; k < 3; k++) s += a.m[i][k] * b.m[k][j];
      r.m[i][j] = (int8_t)s;
    }
  return r;
}

struct RawModel {
  IVec3 size{};
  std::vector<uint8_t> xyzi;  // packed x,y,z,colorIndex quads
};

struct TrnNode {
  std::string name;
  int child = -1;
  IVec3 t{};
  Rot r{};
};
struct GrpNode {
  std::vector<int> children;
};
struct ShpNode {
  std::vector<int> modelIds;
};

// One placed model instance after flattening the graph.
struct Placed {
  std::string name;
  int modelId;
  IVec3 t{};   // scene-space translation of the model box CENTER
  Rot r{};
};

void Flatten(int nodeId, IVec3 t, Rot r, std::string name,
             const std::unordered_map<int, TrnNode>& trns,
             const std::unordered_map<int, GrpNode>& grps,
             const std::unordered_map<int, ShpNode>& shps,
             std::vector<Placed>& out, int depth = 0) {
  if (depth > 64) return;  // cyclic / hostile file
  auto it = trns.find(nodeId);
  if (it != trns.end()) {
    const TrnNode& n = it->second;
    // child transform composes as parent * (R, t)
    IVec3 rt = RotApply(r, n.t);
    Flatten(n.child, IVec3{t.x + rt.x, t.y + rt.y, t.z + rt.z},
            RotCompose(r, n.r), n.name.empty() ? name : n.name, trns, grps,
            shps, out, depth + 1);
    return;
  }
  auto ig = grps.find(nodeId);
  if (ig != grps.end()) {
    for (int c : ig->second.children)
      Flatten(c, t, r, name, trns, grps, shps, out, depth + 1);
    return;
  }
  auto is = shps.find(nodeId);
  if (is != shps.end()) {
    for (int m : is->second.modelIds) out.push_back({name, m, t, r});
  }
}

}  // namespace

bool LoadVoxFromMemory(const uint8_t* data, size_t len, size_t materialCount,
                       Prefab& out, std::string& errors, std::string& warnings) {
  out.models.clear();
  out.size = {0, 0, 0};

  Cursor c{data, len};
  if (len < 8 || std::memcmp(data, "VOX ", 4) != 0) {
    errors += "not a .vox file (bad magic)\n";
    return false;
  }
  c.Skip(4);
  c.U32();  // version (150/200) — chunk layout is compatible for what we read

  std::vector<RawModel> models;
  IVec3 pendingSize{};
  bool haveSize = false;
  std::unordered_map<int, TrnNode> trns;
  std::unordered_map<int, GrpNode> grps;
  std::unordered_map<int, ShpNode> shps;

  // MAIN wrapper then a flat run of child chunks; unknown chunks are skipped
  // by their declared sizes (MATL, LAYR, rOBJ, rCAM, NOTE, IMAP, RGBA ...).
  while (c.ok && c.off + 12 <= c.len) {
    char id[5] = {};
    std::memcpy(id, c.p + c.off, 4);
    c.Skip(4);
    uint32_t content = c.U32();
    c.U32();  // childBytes: MAIN declares its children here; we walk linearly
    size_t next = c.off + (std::strcmp(id, "MAIN") == 0 ? 0 : content);
    if (next > c.len) { errors += "truncated chunk "; errors += id; errors += "\n"; return false; }

    if (std::strcmp(id, "SIZE") == 0) {
      pendingSize = {c.I32(), c.I32(), c.I32()};
      haveSize = true;
    } else if (std::strcmp(id, "XYZI") == 0) {
      uint32_t n = c.U32();
      if (!haveSize) { errors += "XYZI before SIZE\n"; return false; }
      if (c.off + (size_t)n * 4 > c.len) { errors += "truncated XYZI\n"; return false; }
      RawModel m;
      m.size = pendingSize;
      m.xyzi.assign(c.p + c.off, c.p + c.off + (size_t)n * 4);
      c.Skip((size_t)n * 4);
      models.push_back(std::move(m));
      haveSize = false;
    } else if (std::strcmp(id, "nTRN") == 0) {
      int nodeId = c.I32();
      auto attrs = c.Dict();
      TrnNode n;
      auto nm = attrs.find("_name");
      if (nm != attrs.end()) n.name = nm->second;
      n.child = c.I32();
      c.I32();  // reserved
      c.I32();  // layer
      int frames = c.I32();
      for (int f = 0; f < frames && c.ok; f++) {
        auto d = c.Dict();
        if (f != 0) continue;  // frame 0 only (no animation import)
        auto rt = d.find("_r");
        if (rt != d.end()) n.r = DecodeRot(std::atoi(rt->second.c_str()));
        auto tt = d.find("_t");
        if (tt != d.end())
          std::sscanf(tt->second.c_str(), "%d %d %d", &n.t.x, &n.t.y, &n.t.z);
      }
      trns[nodeId] = std::move(n);
    } else if (std::strcmp(id, "nGRP") == 0) {
      int nodeId = c.I32();
      c.Dict();
      int n = c.I32();
      GrpNode g;
      for (int i = 0; i < n && c.ok; i++) g.children.push_back(c.I32());
      grps[nodeId] = std::move(g);
    } else if (std::strcmp(id, "nSHP") == 0) {
      int nodeId = c.I32();
      c.Dict();
      int n = c.I32();
      ShpNode s;
      for (int i = 0; i < n && c.ok; i++) {
        s.modelIds.push_back(c.I32());
        c.Dict();  // per-model attributes (frame index)
      }
      shps[nodeId] = std::move(s);
    }
    c.off = next;
  }
  if (!c.ok) { errors += "malformed .vox (read past end)\n"; return false; }
  if (models.empty()) { errors += "no XYZI models\n"; return false; }

  // flatten the scene graph; files without one get every model at the origin
  std::vector<Placed> placed;
  if (!trns.empty()) Flatten(0, {0, 0, 0}, Rot{}, "", trns, grps, shps, placed);
  if (placed.empty())
    for (size_t i = 0; i < models.size(); i++)
      placed.push_back({"", (int)i, {0, 0, 0}, Rot{}});

  // scene-space voxels per instance, then one global Z-up -> Y-up conversion.
  // Chirality-preserving: engine = (scene.x, scene.z, -scene.y). The whole
  // prefab is rebased afterwards so relative limb placement survives exactly.
  struct EngModel {
    std::string name;
    std::vector<IVec3> cells;
    std::vector<uint16_t> mats;
    IVec3 mn{INT32_MAX, INT32_MAX, INT32_MAX}, mx{INT32_MIN, INT32_MIN, INT32_MIN};
  };
  std::vector<EngModel> eng;
  std::vector<uint32_t> badIndex(256, 0);

  for (size_t pi = 0; pi < placed.size(); pi++) {
    const Placed& pl = placed[pi];
    if (pl.modelId < 0 || pl.modelId >= (int)models.size()) continue;
    const RawModel& rm = models[pl.modelId];
    EngModel em;
    em.name = pl.name.empty() ? "model" + std::to_string(pi) : pl.name;
    // MagicaVoxel rotates about the model box center (floor(size/2)),
    // matching ogt_vox and the editor's behaviour.
    IVec3 pivot{rm.size.x >> 1, rm.size.y >> 1, rm.size.z >> 1};
    em.cells.reserve(rm.xyzi.size() / 4);
    for (size_t i = 0; i + 3 < rm.xyzi.size(); i += 4) {
      uint8_t ci = rm.xyzi[i + 3];
      if (ci == 0) continue;  // index 0 = empty by convention
      if (ci >= materialCount) { badIndex[ci]++; }
      IVec3 local{rm.xyzi[i], rm.xyzi[i + 1], rm.xyzi[i + 2]};
      IVec3 s = RotApply(pl.r, {local.x - pivot.x, local.y - pivot.y, local.z - pivot.z});
      s = {s.x + pl.t.x, s.y + pl.t.y, s.z + pl.t.z};
      IVec3 e{s.x, s.z, -s.y};  // Z-up -> Y-up, chirality preserved
      em.cells.push_back(e);
      em.mats.push_back(ci);
      em.mn.x = std::min(em.mn.x, e.x); em.mn.y = std::min(em.mn.y, e.y); em.mn.z = std::min(em.mn.z, e.z);
      em.mx.x = std::max(em.mx.x, e.x); em.mx.y = std::max(em.mx.y, e.y); em.mx.z = std::max(em.mx.z, e.z);
    }
    if (!em.cells.empty()) eng.push_back(std::move(em));
  }
  if (eng.empty()) { errors += "no non-empty models\n"; return false; }

  for (int ci = 0; ci < 256; ci++)
    if (badIndex[ci])
      warnings += out.name + ": palette index " + std::to_string(ci) + " (" +
                  std::to_string(badIndex[ci]) +
                  " voxels) has no material — skipped at placement\n";

  IVec3 pmn{INT32_MAX, INT32_MAX, INT32_MAX}, pmx{INT32_MIN, INT32_MIN, INT32_MIN};
  for (const EngModel& em : eng) {
    pmn.x = std::min(pmn.x, em.mn.x); pmn.y = std::min(pmn.y, em.mn.y); pmn.z = std::min(pmn.z, em.mn.z);
    pmx.x = std::max(pmx.x, em.mx.x); pmx.y = std::max(pmx.y, em.mx.y); pmx.z = std::max(pmx.z, em.mx.z);
  }
  out.size = {pmx.x - pmn.x + 1, pmx.y - pmn.y + 1, pmx.z - pmn.z + 1};

  for (EngModel& em : eng) {
    PrefabModel pm;
    pm.name = std::move(em.name);
    pm.offset = {em.mn.x - pmn.x, em.mn.y - pmn.y, em.mn.z - pmn.z};
    pm.size = {em.mx.x - em.mn.x + 1, em.mx.y - em.mn.y + 1, em.mx.z - em.mn.z + 1};
    pm.voxels.reserve(em.cells.size());
    for (size_t i = 0; i < em.cells.size(); i++) {
      pm.voxels.push_back({(int16_t)(em.cells[i].x - em.mn.x),
                           (int16_t)(em.cells[i].y - em.mn.y),
                           (int16_t)(em.cells[i].z - em.mn.z),
                           (uint16_t)(em.mats[i] & 0xFFF)});
    }
    out.models.push_back(std::move(pm));
  }
  return true;
}

bool LoadVoxFile(const std::string& path, size_t materialCount, Prefab& out,
                 std::string& errors, std::string& warnings) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) { errors += path + ": cannot open\n"; return false; }
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> buf((size_t)std::max(n, 0L));
  size_t got = buf.empty() ? 0 : std::fread(buf.data(), 1, buf.size(), f);
  std::fclose(f);
  out.name = std::filesystem::path(path).stem().string();
  return LoadVoxFromMemory(buf.data(), got, materialCount, out, errors, warnings);
}

bool LoadPrefabDir(const std::string& dir, size_t materialCount,
                   std::vector<Prefab>& out, std::string& log) {
  out.clear();
  std::error_code ec;
  std::vector<std::string> paths;
  for (auto& e : std::filesystem::directory_iterator(dir, ec))
    if (e.is_regular_file() && e.path().extension() == ".vox")
      paths.push_back(e.path().string());
  if (ec) return true;  // no prefab dir: fine, nothing to load
  std::sort(paths.begin(), paths.end());
  bool any = paths.empty();
  for (const std::string& p : paths) {
    Prefab pf;
    std::string err, warn;
    if (LoadVoxFile(p, materialCount, pf, err, warn)) {
      out.push_back(std::move(pf));
      any = true;
    } else {
      log += p + ": " + err;
    }
    log += warn;
  }
  return any;
}
