// lattice_test — CPU-only harness for the skin/collider lattice bridge.
//
// A micro body may carry a render SKIN finer than its physics COLLIDER
// (phys/lattice.h). The two are related by exactly one function, and the whole
// design rests on properties that are cheap to assert here and expensive to
// notice in a screenshot:
//
//   - a carve at skin resolution keeps skin detail the collider cannot express
//   - the derived collider still AGREES with the skin about where the body is
//   - the int8 collider bound is enforced by dropping, never by wrapping
//
// That last one is the reason this file exists. The pre-split code round-tripped
// block indices through (uint8_t)/(int8_t) and was correct only because a limb
// was bounded at +-120 elsewhere; at 8x skin that assumption stops holding, and
// a silent wrap would teleport part of a limb to the opposite side of the body.

#include <cmath>
#include <cstdio>
#include <vector>

#include "phys/lattice.h"

namespace {

int failures = 0;

#define CHECK(cond, msg)                                        \
  do {                                                          \
    if (!(cond)) {                                              \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
      failures++;                                               \
    }                                                           \
  } while (0)

// A solid box of skin voxels, [0,n)^3, all one material.
std::vector<PrefabVoxel> SolidBox(int n, uint16_t mat) {
  std::vector<PrefabVoxel> v;
  for (int z = 0; z < n; z++)
    for (int y = 0; y < n; y++)
      for (int x = 0; x < n; x++)
        v.push_back({(int16_t)x, (int16_t)y, (int16_t)z, mat});
  return v;
}

bool HasVoxel(const std::vector<DebrisVoxel>& v, int x, int y, int z) {
  for (const DebrisVoxel& d : v)
    if (d.x == x && d.y == y && d.z == z) return true;
  return false;
}

// ---- 1. a solid box downsamples to a solid box ------------------------------
void TestSolid() {
  bool over = false;
  // 16^3 skin at ratio 4 -> 4^3 collider, every block completely full.
  auto out = DownsampleSkin(SolidBox(16, 7), 4, &over);
  CHECK(!over, "solid 16^3 should not overflow the collider bound");
  CHECK(out.size() == 64, "16^3 at ratio 4 should yield 4^3 = 64 voxels");
  for (const DebrisVoxel& d : out) {
    CHECK(d.x >= 0 && d.x < 4 && d.y >= 0 && d.y < 4 && d.z >= 0 && d.z < 4,
          "collider voxel outside the expected 4^3 box");
    CHECK(d.payload == 7, "solid single-material box should keep its material");
  }
}

// ---- 2. majority-fill: under half solid becomes air --------------------------
void TestMajorityFill() {
  bool over = false;
  // One 4x4x4 block with 31 of 64 voxels solid: just under half -> air.
  std::vector<PrefabVoxel> v;
  for (int i = 0; i < 31; i++)
    v.push_back({(int16_t)(i % 4), (int16_t)((i / 4) % 4), (int16_t)(i / 16), 3});
  CHECK(DownsampleSkin(v, 4, &over).empty(),
        "a block under half full must read as air");

  // 32 of 64 is exactly half -> solid (the rule is `count*2 < full` drops).
  v.push_back({(int16_t)3, (int16_t)3, (int16_t)1, 3});
  CHECK(DownsampleSkin(v, 4, &over).size() == 1,
        "a block exactly half full must survive");
}

// ---- 3. plurality material wins ---------------------------------------------
void TestPluralityMaterial() {
  bool over = false;
  std::vector<PrefabVoxel> v;
  // A full 2x2x2 block: 5 of material 9, 3 of material 4. 9 should win.
  int made = 0;
  for (int z = 0; z < 2; z++)
    for (int y = 0; y < 2; y++)
      for (int x = 0; x < 2; x++)
        v.push_back({(int16_t)x, (int16_t)y, (int16_t)z,
                     (uint16_t)(made++ < 5 ? 9 : 4)});
  auto out = DownsampleSkin(v, 2, &over);
  CHECK(out.size() == 1, "one full 2^3 block -> one collider voxel");
  CHECK(!out.empty() && out[0].payload == 9,
        "the plurality material (9) should win the block");
}

// ---- 4. the int8 bound DROPS rather than wraps -------------------------------
void TestOverflowDrops() {
  bool over = false;
  std::vector<PrefabVoxel> v;
  // Block index 128 is one past the int8 bound. At ratio 2 that is skin x=256.
  // A wrap would put this at collider x = -128, i.e. the far side of the body.
  for (int z = 0; z < 2; z++)
    for (int y = 0; y < 2; y++)
      for (int x = 0; x < 2; x++)
        v.push_back({(int16_t)(256 + x), (int16_t)y, (int16_t)z, 5});
  auto out = DownsampleSkin(v, 2, &over);
  CHECK(over, "a block past +-127 must set the overflow flag");
  CHECK(out.empty(), "an out-of-range block must be dropped, never wrapped");
  for (const DebrisVoxel& d : out)
    CHECK(d.x >= 0, "no collider voxel may land at a negative coordinate");
}

// ---- 5. THE POINT: a carve keeps detail the collider cannot express ----------
//
// Carve a small sphere out of a 32^3 skin at skin 8 / collider 2 (ratio 4) and
// assert that (a) the skin keeps voxels the collider had to round away, and
// (b) the collider still agrees with the skin about the body's extent.
void TestCarveKeepsDetail() {
  const int n = 32;
  auto skin = SolidBox(n, 6);
  const size_t before = skin.size();

  // A sphere of radius 3.4 skin voxels — smaller than ONE collider voxel
  // (ratio 4), so a collider-resolution carve could not represent it at all.
  const float cx = 8.5f, cy = 8.5f, cz = 8.5f, r = 3.4f;
  auto keep = [&](const PrefabVoxel& v) {
    float dx = (float)v.x + 0.5f - cx, dy = (float)v.y + 0.5f - cy,
          dz = (float)v.z + 0.5f - cz;
    return dx * dx + dy * dy + dz * dz >= r * r;
  };
  std::vector<PrefabVoxel> carved;
  for (const PrefabVoxel& v : skin)
    if (keep(v)) carved.push_back(v);

  CHECK(carved.size() < before, "the carve must actually remove skin voxels");
  CHECK(before - carved.size() > 100,
        "a r=3.4 sphere should remove ~160 skin voxels");

  bool over = false;
  auto collider = DownsampleSkin(carved, 4, &over);
  CHECK(!over, "a 32^3 skin at ratio 4 fits the collider bound");

  // (a) Detail survives: the skin has a real cavity...
  int skinHoleVoxels = 0;
  for (int z = 6; z < 12; z++)
    for (int y = 6; y < 12; y++)
      for (int x = 6; x < 12; x++) {
        bool present = false;
        for (const PrefabVoxel& v : carved)
          if (v.x == x && v.y == y && v.z == z) { present = true; break; }
        if (!present) skinHoleVoxels++;
      }
  CHECK(skinHoleVoxels > 50, "the skin should show a genuine cavity");

  // ...while the collider is far coarser about it. The crater spans 6-7 skin
  // voxels across, and one collider voxel is 4 of those, so the collider can
  // only ever say "this 4x4x4 block is mostly gone" — it renders the cavity as
  // at most a couple of missing blocks where the skin shows a smooth bowl.
  //
  // Counted rather than named: exactly WHICH blocks drop depends on where the
  // sphere centre falls against the block grid (a centre on a block corner
  // splits its mass eight ways and can drop none of them). The property that
  // matters is that the collider loses FEWER voxels than the skin did, i.e.
  // detail exists that physics is not paying for.
  const size_t skinLost = before - carved.size();
  const size_t colliderLost = (size_t)(n / 4) * (n / 4) * (n / 4) - collider.size();
  CHECK(colliderLost * 4 < skinLost,
        "the collider must lose far fewer voxels than the skin (coarser)");
  CHECK(collider.size() > 100,
        "most of the collider must survive a cavity this small");

  // (b) Agreement: the collider's extent still matches the skin's, within one
  // collider voxel. This is the invariant that replaces vigilance about drift.
  int16_t sxMax = 0, syMax = 0, szMax = 0;
  for (const PrefabVoxel& v : carved) {
    sxMax = std::max(sxMax, v.x);
    syMax = std::max(syMax, v.y);
    szMax = std::max(szMax, v.z);
  }
  int8_t cxMax = 0, cyMax = 0, czMax = 0;
  for (const DebrisVoxel& d : collider) {
    cxMax = std::max(cxMax, d.x);
    cyMax = std::max(cyMax, d.y);
    czMax = std::max(czMax, d.z);
  }
  CHECK(std::abs((sxMax / 4) - cxMax) <= 1, "x extent must agree within 1");
  CHECK(std::abs((syMax / 4) - cyMax) <= 1, "y extent must agree within 1");
  CHECK(std::abs((szMax / 4) - czMax) <= 1, "z extent must agree within 1");
}

// ---- 6. carving away everything yields an empty collider, not a ghost --------
void TestFullyCarved() {
  bool over = false;
  CHECK(DownsampleSkin({}, 4, &over).empty(),
        "an empty skin must produce an empty collider");
  CHECK(!over, "an empty skin does not overflow");
}

}  // namespace

int main() {
  TestSolid();
  TestMajorityFill();
  TestPluralityMaterial();
  TestOverflowDrops();
  TestCarveKeepsDetail();
  TestFullyCarved();
  if (failures == 0) {
    std::printf("lattice_test: PASS\n");
    return 0;
  }
  std::printf("lattice_test: FAIL (%d)\n", failures);
  return 1;
}
