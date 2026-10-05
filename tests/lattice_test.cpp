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

// ---- 7. THE FAST PATHS ARE THE OLD ANSWERS, BIT FOR BIT (fight64 round 3) ---
//
// DownsampleSkin's block index became a shift and SpallGrow's `live` set a
// bitmap. Both were meant to change the cost and nothing else, so the two
// pre-change implementations are kept here verbatim and every output compared
// field by field over random lattices that include negative coordinates, the
// int8 overflow band, duplicate cells and a ratio that is not a power of two.
std::vector<DebrisVoxel> RefDownsample(const std::vector<PrefabVoxel>& src,
                                       uint32_t ratio, bool* overflow) {
  const int s = (int)std::max(1u, ratio);
  const uint32_t full = (uint32_t)(s * s * s);
  constexpr int kMaxDistinct = 8;
  struct Blk {
    uint32_t count = 0;
    int n = 0;
    uint32_t pair[kMaxDistinct]{};
    uint32_t hits[kMaxDistinct]{};
    uint16_t stain = 0;
  };
  bool over = false;
  int lo[3] = {128, 128, 128}, hi[3] = {-1, -1, -1};
  for (const PrefabVoxel& v : src) {
    const int bx = (int)v.x / s, by = (int)v.y / s, bz = (int)v.z / s;
    if (bx < 0 || by < 0 || bz < 0 || bx > 127 || by > 127 || bz > 127) {
      over = true;
      continue;
    }
    lo[0] = std::min(lo[0], bx); hi[0] = std::max(hi[0], bx);
    lo[1] = std::min(lo[1], by); hi[1] = std::max(hi[1], by);
    lo[2] = std::min(lo[2], bz); hi[2] = std::max(hi[2], bz);
  }
  if (overflow) *overflow = over;
  std::vector<DebrisVoxel> out;
  if (hi[0] < 0) return out;
  const int ex = hi[0] - lo[0] + 1, ey = hi[1] - lo[1] + 1, ez = hi[2] - lo[2] + 1;
  std::vector<int32_t> at((size_t)ex * ey * ez, -1);
  std::vector<Blk> blocks;
  for (const PrefabVoxel& v : src) {
    const int bx = (int)v.x / s, by = (int)v.y / s, bz = (int)v.z / s;
    if (bx < 0 || by < 0 || bz < 0 || bx > 127 || by > 127 || bz > 127) continue;
    int32_t& slot =
        at[((size_t)(bz - lo[2]) * ey + (size_t)(by - lo[1])) * ex + (size_t)(bx - lo[0])];
    if (slot < 0) {
      slot = (int32_t)blocks.size();
      blocks.emplace_back();
    }
    const uint32_t pair = (uint32_t)v.material | ((uint32_t)v.color << 16);
    Blk& blk = blocks[(size_t)slot];
    blk.count++;
    int k = 0;
    for (; k < blk.n; k++)
      if (blk.pair[k] == pair) break;
    if (k == blk.n && blk.n < kMaxDistinct) blk.pair[blk.n++] = pair;
    if (k < kMaxDistinct) blk.hits[k]++;
    if (BodyStainAmt(v.stain) > BodyStainAmt(blk.stain)) blk.stain = v.stain;
  }
  size_t i = 0;
  for (int z = 0; z < ez; z++)
    for (int y = 0; y < ey; y++)
      for (int x = 0; x < ex; x++, i++) {
        const int32_t slot = at[i];
        if (slot < 0) continue;
        const Blk& blk = blocks[(size_t)slot];
        if (blk.count * 2 < full) continue;
        int best = 0;
        for (int k = 1; k < blk.n; k++)
          if (blk.hits[k] > blk.hits[best]) best = k;
        out.push_back({(int8_t)(x + lo[0]), (int8_t)(y + lo[1]), (int8_t)(z + lo[2]),
                       (uint8_t)(blk.pair[best] >> 16),
                       (uint16_t)(blk.pair[best] & 0xFFFFu), blk.stain});
      }
  return out;
}

template <class Vox, class OnLost>
size_t RefSpall(std::vector<Vox>& cells, const SpallParams& p, OnLost onLost) {
  if (p.rounds <= 0 || p.strength <= 0.0f || p.radius <= 0.0f) return 0;
  const float r2 = p.radius * p.radius;
  auto key = [](int x, int y, int z) -> uint64_t {
    return ((uint64_t)(uint32_t)(x + 32768) << 42) |
           ((uint64_t)(uint32_t)(y + 32768) << 21) |
           (uint64_t)(uint32_t)(z + 32768);
  };
  const float reach = p.radius + 2.0f;
  const float reach2 = reach * reach;
  auto nearBlow = [&](const Vox& v) {
    const float dx = (float)v.x + 0.5f - p.centre.x,
                dy = (float)v.y + 0.5f - p.centre.y,
                dz = (float)v.z + 0.5f - p.centre.z;
    return dx * dx + dy * dy + dz * dz < reach2;
  };
  std::unordered_set<uint64_t> live;
  auto rebuild = [&] {
    live.clear();
    for (const Vox& v : cells)
      if (nearBlow(v)) live.insert(key(v.x, v.y, v.z));
  };
  rebuild();
  constexpr int kMinOpenFaces = 3;
  size_t total = 0;
  for (int round = 0; round < p.rounds; round++) {
    auto doomed = [&](int x, int y, int z) {
      const Vec3 d{(float)x + 0.5f - p.centre.x, (float)y + 0.5f - p.centre.y,
                   (float)z + 0.5f - p.centre.z};
      const float d2 = d.dot(d);
      if (d2 >= r2) return false;
      int open = 0;
      static const int kN[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                   {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
      for (const auto& n : kN)
        if (!live.count(key(x + n[0], y + n[1], z + n[2]))) open++;
      if (open < kMinOpenFaces) return false;
      const float t = std::sqrt(d2 / r2);
      const float chance = p.strength * (1.0f - t) * ((float)open / 6.0f);
      const uint32_t h =
          rng::Hash3(p.seed + 0x5BF03635u * (uint32_t)(round + 1),
                     (uint32_t)(x * 73856093) ^ (uint32_t)(y * 19349663),
                     (uint32_t)(z * 83492791));
      return (float)(h & 0xFFFFu) / 65535.0f < chance;
    };
    const size_t before = cells.size();
    cells.erase(std::remove_if(cells.begin(), cells.end(),
                               [&](const Vox& v) {
                                 if (!doomed(v.x, v.y, v.z)) return false;
                                 onLost(v);
                                 return true;
                               }),
                cells.end());
    const size_t took = before - cells.size();
    total += took;
    if (took == 0) break;
    rebuild();
  }
  return total;
}

bool SameVox(const DebrisVoxel& a, const DebrisVoxel& b) {
  return a.x == b.x && a.y == b.y && a.z == b.z && a.color == b.color &&
         a.payload == b.payload && a.stain == b.stain;
}

void TestFastPathsMatchReference() {
  uint32_t st = 12345u;
  auto rnd = [&]() {
    st = st * 1664525u + 1013904223u;
    return st >> 8;
  };
  const uint32_t ratios[] = {1, 2, 3, 4, 8};
  for (int trial = 0; trial < 400; trial++) {
    const uint32_t ratio = ratios[trial % 5];
    std::vector<PrefabVoxel> skin;
    // A blobby solid plus noise: offsets that reach below zero and past the
    // int8 band, a duplicated cell now and then, two materials, two colours.
    const int n = 4 + (int)(rnd() % 24);
    const int off = (trial % 7 == 0) ? -3 : (trial % 11 == 0 ? 120 * (int)ratio : 0);
    for (int z = 0; z < n; z++)
      for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
          if (rnd() % 5 == 0) continue;
          PrefabVoxel v{(int16_t)(x + off), (int16_t)(y + off / 2), (int16_t)(z + off),
                        (uint16_t)(1 + rnd() % 2)};
          v.color = (uint8_t)(rnd() % 3);
          v.stain = (uint16_t)(rnd() % 4 == 0 ? (rnd() & 0xFFFFu) : 0u);
          skin.push_back(v);
          if (rnd() % 37 == 0) skin.push_back(v);
        }
    bool o1 = false, o2 = false;
    const auto a = DownsampleSkin(skin, ratio, &o1);
    const auto b = RefDownsample(skin, ratio, &o2);
    bool same = o1 == o2 && a.size() == b.size();
    for (size_t i = 0; same && i < a.size(); i++) same = SameVox(a[i], b[i]);
    CHECK(same, "DownsampleSkin must match the reference implementation exactly");

    SpallParams sp;
    sp.centre = Vec3{(float)(rnd() % (n + 4)) - 2.0f + 0.3f,
                     (float)(rnd() % (n + 4)) - 2.0f + 0.7f,
                     (float)(rnd() % (n + 4)) - 2.0f + 0.1f} +
                Vec3{(float)off, (float)(off / 2), (float)off};
    sp.radius = 1.0f + (float)(rnd() % 90) * 0.1f;
    sp.strength = 0.3f + (float)(rnd() % 70) * 0.01f;
    sp.rounds = 1 + (int)(rnd() % 4);
    sp.seed = rnd();
    std::vector<PrefabVoxel> s1 = skin, s2 = skin;
    std::vector<PrefabVoxel> l1, l2;
    const size_t t1 = SpallGrow(s1, sp, [&](const PrefabVoxel& v) { l1.push_back(v); });
    const size_t t2 = RefSpall(s2, sp, [&](const PrefabVoxel& v) { l2.push_back(v); });
    bool sameS = t1 == t2 && s1.size() == s2.size() && l1.size() == l2.size();
    for (size_t i = 0; sameS && i < s1.size(); i++)
      sameS = s1[i].x == s2[i].x && s1[i].y == s2[i].y && s1[i].z == s2[i].z;
    for (size_t i = 0; sameS && i < l1.size(); i++)
      sameS = l1[i].x == l2[i].x && l1[i].y == l2[i].y && l1[i].z == l2[i].z;
    CHECK(sameS, "SpallGrow must match the reference implementation exactly");
  }
}

}  // namespace

int main() {
  TestFastPathsMatchReference();
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
