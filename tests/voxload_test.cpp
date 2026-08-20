// voxload_test — CPU-only .vox parser harness (no GPU, no window).
// Builds synthetic .vox binaries in memory and asserts the loader's coordinate
// conversion (Z-up -> Y-up, chirality), scene-graph flattening (names,
// translations, rotations), rebasing, and palette warnings. These are exactly
// the properties that silently corrupt art if they regress ("every model is
// on its side").

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sim/voxload.h"

namespace {

int failures = 0;

#define CHECK(cond, msg)                                        \
  do {                                                          \
    if (!(cond)) {                                              \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
      failures++;                                               \
    }                                                           \
  } while (0)

// ---- .vox binary builder ----------------------------------------------------
struct VoxBuilder {
  std::vector<uint8_t> b;

  void U32(uint32_t v) {
    b.insert(b.end(), (uint8_t*)&v, (uint8_t*)&v + 4);
  }
  void I32(int32_t v) { U32((uint32_t)v); }
  void Raw(const void* p, size_t n) {
    b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n);
  }
  void Str(const std::string& s) {
    U32((uint32_t)s.size());
    Raw(s.data(), s.size());
  }
  void Dict(const std::vector<std::pair<std::string, std::string>>& kv) {
    U32((uint32_t)kv.size());
    for (auto& [k, v] : kv) { Str(k); Str(v); }
  }
  // chunk with content produced by `fill`
  template <typename F>
  void Chunk(const char* id, F fill) {
    Raw(id, 4);
    size_t sizeAt = b.size();
    U32(0);  // content bytes (patched)
    U32(0);  // child bytes
    size_t start = b.size();
    fill();
    uint32_t n = (uint32_t)(b.size() - start);
    std::memcpy(&b[sizeAt], &n, 4);
  }

  static VoxBuilder Begin() {
    VoxBuilder v;
    v.Raw("VOX ", 4);
    v.U32(150);
    v.Raw("MAIN", 4);
    v.U32(0);
    size_t at = v.b.size();
    v.U32(0);  // MAIN childBytes — loader walks linearly, patch anyway
    v.mainChildAt = at;
    return v;
  }
  size_t mainChildAt = 0;
  void End() {
    uint32_t n = (uint32_t)(b.size() - mainChildAt - 4);
    std::memcpy(&b[mainChildAt], &n, 4);
  }

  void Size(int x, int y, int z) {
    Chunk("SIZE", [&] { I32(x); I32(y); I32(z); });
  }
  void Xyzi(const std::vector<std::array<uint8_t, 4>>& vox) {
    Chunk("XYZI", [&] {
      U32((uint32_t)vox.size());
      for (auto& v : vox) Raw(v.data(), 4);
    });
  }
  void Trn(int id, const std::string& name, int child, IVec3 t, int rotByte = -1) {
    Chunk("nTRN", [&] {
      I32(id);
      if (name.empty()) Dict({});
      else Dict({{"_name", name}});
      I32(child);
      I32(-1);  // reserved
      I32(0);   // layer
      I32(1);   // frames
      std::vector<std::pair<std::string, std::string>> f;
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%d %d %d", t.x, t.y, t.z);
      f.push_back({"_t", buf});
      if (rotByte >= 0) f.push_back({"_r", std::to_string(rotByte)});
      Dict(f);
    });
  }
  void Grp(int id, const std::vector<int>& children) {
    Chunk("nGRP", [&] {
      I32(id);
      Dict({});
      I32((int)children.size());
      for (int ch : children) I32(ch);
    });
  }
  void Shp(int id, int modelId) {
    Chunk("nSHP", [&] {
      I32(id);
      Dict({});
      I32(1);
      I32(modelId);
      Dict({});
    });
  }
};

const PrefabModel* FindModel(const Prefab& p, const std::string& name) {
  for (const auto& m : p.models)
    if (m.name == name) return &m;
  return nullptr;
}

bool HasVoxel(const PrefabModel& m, int x, int y, int z, uint16_t mat) {
  for (const auto& v : m.voxels)
    if (v.x == x && v.y == y && v.z == z && v.material == mat) return true;
  return false;
}

void TestMinimal() {
  // two voxels, no scene graph: pure axis-conversion check.
  // scene (x,y,z) -> engine (x, z, -y), rebased to min corner 0.
  VoxBuilder v = VoxBuilder::Begin();
  v.Size(2, 3, 4);
  v.Xyzi({{{0, 0, 0, 1}}, {{1, 2, 3, 2}}});
  v.End();

  Prefab p;
  std::string err, warn;
  bool ok = LoadVoxFromMemory(v.b.data(), v.b.size(), 32, p, err, warn);
  CHECK(ok, ("minimal parse failed: " + err).c_str());
  if (!ok) return;
  CHECK(p.models.size() == 1, "one model");
  const PrefabModel& m = p.models[0];
  // engine: (0,0,0) -> (0,0,0); (1,2,3) -> (1,3,-2); rebase by min (0,0,-2)
  CHECK(HasVoxel(m, 0, 0, 2, 1), "voxel A converted");
  CHECK(HasVoxel(m, 1, 3, 0, 2), "voxel B converted");
  CHECK(p.size.x == 2 && p.size.y == 4 && p.size.z == 3, "prefab size (2,4,3)");
  CHECK(warn.empty(), "no warnings for valid palette indices");
}

void TestSceneGraph() {
  // torso at origin, head translated +10 scene-Z (up) => +10 engine-Y.
  VoxBuilder v = VoxBuilder::Begin();
  v.Size(2, 2, 2);
  v.Xyzi({{{0, 0, 0, 1}}, {{1, 1, 1, 1}}});  // model 0: head
  v.Size(2, 2, 2);
  v.Xyzi({{{0, 0, 0, 2}}, {{1, 1, 1, 2}}});  // model 1: torso
  v.Trn(0, "", 1, {0, 0, 0});
  v.Grp(1, {2, 4});
  v.Trn(2, "head", 3, {0, 0, 10});
  v.Shp(3, 0);
  v.Trn(4, "torso", 5, {0, 0, 0});
  v.Shp(5, 1);
  v.End();

  Prefab p;
  std::string err, warn;
  bool ok = LoadVoxFromMemory(v.b.data(), v.b.size(), 32, p, err, warn);
  CHECK(ok, ("scene parse failed: " + err).c_str());
  if (!ok) return;
  CHECK(p.models.size() == 2, "two models");
  const PrefabModel* head = FindModel(p, "head");
  const PrefabModel* torso = FindModel(p, "torso");
  CHECK(head && torso, "named models found");
  if (!head || !torso) return;
  // identical boxes, head 10 scene-Z above torso -> offset.y differs by 10
  CHECK(head->offset.y - torso->offset.y == 10, "head 10 above torso in engine Y");
  CHECK(head->offset.x == torso->offset.x && head->offset.z == torso->offset.z,
        "head/torso aligned in XZ");
  CHECK(torso->offset.x == 0 && torso->offset.y == 0 && torso->offset.z == 0,
        "prefab rebased to min corner 0");
}

void TestRotation() {
  // rot byte 17: row0=(0,-1,0), row1=(1,0,0), row2=(0,0,1) — 90° about scene Z.
  // asymmetric model so the rotation is observable: voxels (0,0,0) and (2,0,0),
  // size (3,1,1), pivot (1,0,0).
  VoxBuilder v = VoxBuilder::Begin();
  v.Size(3, 1, 1);
  v.Xyzi({{{0, 0, 0, 1}}, {{2, 0, 0, 1}}});
  v.Trn(0, "rotated", 1, {0, 0, 0}, 17);
  v.Shp(1, 0);
  v.End();

  Prefab p;
  std::string err, warn;
  bool ok = LoadVoxFromMemory(v.b.data(), v.b.size(), 32, p, err, warn);
  CHECK(ok, ("rotation parse failed: " + err).c_str());
  if (!ok) return;
  // local - pivot: (-1,0,0) and (1,0,0); R*: (0,-1,0)->( 0,-1,0)? no:
  // R*(-1,0,0) = (0,-1,0); R*(1,0,0) = (0,1,0). scene span is along Y ->
  // engine span along Z (negated). The model must be 1x1x3 in engine axes.
  const PrefabModel& m = p.models[0];
  CHECK(m.size.x == 1 && m.size.y == 1 && m.size.z == 3,
        "90° scene-Z rotation turns X-run into engine Z-run");
}

void TestPaletteWarning() {
  VoxBuilder v = VoxBuilder::Begin();
  v.Size(1, 1, 1);
  v.Xyzi({{{0, 0, 0, 200}}});  // index 200 with 32 materials
  v.End();

  Prefab p;
  std::string err, warn;
  bool ok = LoadVoxFromMemory(v.b.data(), v.b.size(), 32, p, err, warn);
  CHECK(ok, "parse succeeds despite bad palette index");
  CHECK(!warn.empty(), "warning emitted for unmapped palette index");
  CHECK(warn.find("200") != std::string::npos, "warning names the index");
}

void TestGarbage() {
  std::vector<uint8_t> junk(64, 0xAB);
  Prefab p;
  std::string err, warn;
  CHECK(!LoadVoxFromMemory(junk.data(), junk.size(), 32, p, err, warn),
        "garbage rejected");
  std::vector<uint8_t> truncated = {'V', 'O', 'X', ' ', 150, 0, 0, 0};
  CHECK(!LoadVoxFromMemory(truncated.data(), truncated.size(), 32, p, err, warn),
        "truncated header rejected");
}

}  // namespace

int main() {
  TestMinimal();
  TestSceneGraph();
  TestRotation();
  TestPaletteWarning();
  TestGarbage();
  if (failures == 0) {
    std::printf("voxload_test: PASS\n");
    return 0;
  }
  std::printf("voxload_test: %d FAILURES\n", failures);
  return 1;
}
