// horizon.cpp -- the far horizon's height pyramid (horizon.h says what and why).
#include "sim/horizon.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "sim/world.h"
#include "sim/worldmap.h"

namespace horizon {

namespace {

// The authored height at a POST-LATTICE point (x, z): the post itself inside
// the extent, and outside it the edge post eased to the plane's default over
// edgeFadeVox -- worldgen.wgsl mapHeightAt's fade, in float, on the post
// nearest the clamped point. The horizon is a picture of the map, so the
// Catmull-Rom between posts is not reproduced: every sample point lands ON a
// post (the sample lattice is a multiple of the post spacing from the extent's
// corner), which is where the spline equals the post exactly.
int PostHeightAt(const worldmap::WorldMapData& m, int x, int z) {
  const int l = m.heightLog2;
  const int cx = std::clamp(x, m.extentX0, m.extentX0 + m.extentW);
  const int cz = std::clamp(z, m.extentZ0, m.extentZ0 + m.extentH);
  const int e = m.HeightPost((cx - m.extentX0) >> l, (cz - m.extentZ0) >> l);
  const int d = std::max(std::abs(x - cx), std::abs(z - cz));
  if (d == 0) return e;
  const int fade = std::max(m.edgeFadeVox, 1);
  if (d >= fade) return m.heightDefault;
  const float t = (float)d / (float)fade;
  const float s = t * t * (3.0f - 2.0f * t);
  return e + (int)std::lround((float)(m.heightDefault - e) * s);
}

int16_t ClampI16(int v) { return (int16_t)std::clamp(v, -32768, 32767); }
uint32_t Pack(int maxV, int avgV) {
  return (uint32_t)(uint16_t)ClampI16(maxV) | ((uint32_t)(uint16_t)ClampI16(avgV) << 16);
}
int UnMax(uint32_t w) { return (int)(int16_t)(w & 0xFFFFu); }
int UnAvg(uint32_t w) { return (int)(int16_t)(w >> 16); }

}  // namespace

uint64_t LightWords(const std::vector<uint32_t>& w) {
  if (w.size() <= kHNZ || w[kHValid] == 0u) return kLightHeaderWords;
  return kLightHeaderWords + 2ull * (uint64_t)w[kHNX] * (uint64_t)w[kHNZ];
}

void Build(const worldmap::WorldMapData& m, const std::vector<uint32_t>& wm, uint32_t seed,
           std::vector<uint32_t>& out) {
  out.assign(kHeaderWords, 0u);
  if (!m.Loaded() || m.height.empty()) return;   // kHValid stays 0

  const int S = 1 << kSampleLog2;
  // The grid covers the extent plus the edge fade, rounded out to a sample;
  // the extent's corner is a multiple of the site cell (>= a sample), so every
  // sample sits on the post lattice.
  const int margin = ((std::max(m.edgeFadeVox, 0) + S - 1) / S) * S;
  const int x0 = m.extentX0 - margin;
  const int z0 = m.extentZ0 - margin;
  const int nx = (m.extentW + 2 * margin) / S + 1;
  const int nz = (m.extentH + 2 * margin) / S + 1;
  const int sea = m.seaLevelY;
  const int outer = std::max(m.heightDefault, sea);

  // ---- the samples: MAX effective and AVG ground over the footprint posts ----
  // The footprint is the sample's own cell of the lattice, [-S/2, +S/2] about
  // it, at the post spacing (5 x 5 posts at 64-voxel posts; every post of a
  // finer plane would be 33 x 33, so the step is capped to S/4 -- a ridge is
  // still caught to within a quarter sample).
  const int step = std::max(1 << m.heightLog2, S / 4);
  const size_t ns = (size_t)nx * nz;
  std::vector<uint32_t> samples(ns);
  std::vector<uint8_t> biome(ns);
  // The column's own treeline (map v2 A1): worldgen caps a column with snow
  // when its ground reaches TreelineAt, which varies by biome and carries a
  // seeded jitter -- one map-wide Y painted all of `peaks` white.
  std::vector<int16_t> tline(ns);
  int maxY = outer;
  for (int j = 0; j < nz; j++) {
    for (int i = 0; i < nx; i++) {
      const int x = x0 + i * S, z = z0 + j * S;
      int hi = INT32_MIN;
      int64_t sum = 0;
      int n = 0;
      for (int dz = -S / 2; dz <= S / 2; dz += step) {
        for (int dx = -S / 2; dx <= S / 2; dx += step) {
          const int h = PostHeightAt(m, x + dx, z + dz);
          hi = std::max(hi, std::max(h, sea));
          sum += h;
          n++;
        }
      }
      const int avg = (int)(sum / std::max(n, 1));
      samples[(size_t)j * nx + i] = Pack(hi, avg);
      biome[(size_t)j * nx + i] = m.BiomeCellAt(x, z);
      tline[(size_t)j * nx + i] = ClampI16(worldmap::TreelineAt(x, z, seed));
      maxY = std::max(maxY, hi);
    }
  }

  // ---- THE WATER (W1): map lakes and river channels, per sample ----
  // A lake: the share of a 4 x 4 subsample grid over the footprint inside its
  // polygon with the post ground under its level; at least half wet lifts the
  // sample's MAX and AVG to the level, so the horizon draws the lake flat. A
  // river: every baked centreline sample (4 voxels of channel, 2 hw wide) adds
  // its area to the sample under it -- a 4 m stream is a faint line at 20 km,
  // a 40 m river a solid one; its geometry stays the ground's.
  std::vector<uint32_t> water;
  if (!m.lakes.empty() || !m.rivers.empty()) {
    water.assign(ns, 0u);
    std::vector<int> cov(ns, 0), lvl(ns, INT32_MIN);
    auto idxOf = [&](int x, int z, size_t* k) {
      const int i = (int)std::lround((double)(x - x0) / S), j = (int)std::lround((double)(z - z0) / S);
      if (i < 0 || j < 0 || i >= nx || j >= nz) return false;
      *k = (size_t)j * nx + i;
      return true;
    };
    for (const worldmap::WorldMapData::Lake& lk : m.lakes) {
      const int i0 = std::max(0, (lk.x0 - x0) / S - 1), i1 = std::min(nx - 1, (lk.x1 - x0) / S + 1);
      const int j0 = std::max(0, (lk.z0 - z0) / S - 1), j1 = std::min(nz - 1, (lk.z1 - z0) / S + 1);
      for (int j = j0; j <= j1; j++)
        for (int i = i0; i <= i1; i++) {
          int wet = 0;
          for (int b = 0; b < 4; b++)
            for (int a = 0; a < 4; a++) {
              const int x = x0 + i * S - S / 2 + S / 8 + a * S / 4, z = z0 + j * S - S / 2 + S / 8 + b * S / 4;
              // The authored ground (no noise): a lake is a few metres deep
              // and the posts' floor is coarse, so allow it 16 voxels.
              if (lk.Inside(x, z) && worldmap::AuthoredHeightAt(x, z) < lk.level + 16) wet++;
            }
          if (wet == 0) continue;
          const size_t k = (size_t)j * nx + i;
          cov[k] = std::max(cov[k], wet * 255 / 16);
          lvl[k] = std::max(lvl[k], lk.level);
          if (wet >= 8) {
            const int hi = std::max(UnMax(samples[k]), lk.level), avg = std::max(UnAvg(samples[k]), lk.level);
            samples[k] = Pack(hi, avg);
            maxY = std::max(maxY, hi);
          }
        }
    }
    for (const worldmap::WorldMapData::River& rv : m.rivers)
      for (const auto& p : rv.line) {
        size_t k;
        if (!idxOf(p[0], p[1], &k)) continue;
        cov[k] = std::min(255, cov[k] + (4 * 2 * p[3] * 255 + S * S / 2) / (S * S));
        lvl[k] = std::max(lvl[k], p[2]);
      }
    for (size_t k = 0; k < ns; k++)
      if (cov[k] > 0)
        water[k] = (uint32_t)std::min(cov[k], 255) | ((uint32_t)(uint16_t)ClampI16(lvl[k]) << 16);
  }

  // ---- the cell pyramid ----
  // Level 0: (nx-1) x (nz-1) cells between samples. Each level halves (rounding
  // up) until one cell covers everything.
  std::vector<std::vector<uint32_t>> mips;
  int cw = std::max(nx - 1, 1), ch = std::max(nz - 1, 1);
  {
    std::vector<uint32_t> l0((size_t)cw * ch);
    for (int j = 0; j < ch; j++) {
      for (int i = 0; i < cw; i++) {
        int hi = INT32_MIN, sum = 0;
        for (int c = 0; c < 4; c++) {
          const int si = std::min(i + (c & 1), nx - 1);
          const int sj = std::min(j + (c >> 1), nz - 1);
          const uint32_t w = samples[(size_t)sj * nx + si];
          hi = std::max(hi, UnMax(w));
          sum += UnAvg(w);
        }
        l0[(size_t)j * cw + i] = Pack(hi, sum / 4);
      }
    }
    mips.push_back(std::move(l0));
  }
  while ((cw > 1 || ch > 1) && (int)mips.size() < (int)kMaxLevels) {
    const int pw = cw, ph = ch;
    cw = (cw + 1) / 2;
    ch = (ch + 1) / 2;
    const std::vector<uint32_t>& p = mips.back();
    std::vector<uint32_t> l((size_t)cw * ch);
    for (int j = 0; j < ch; j++) {
      for (int i = 0; i < cw; i++) {
        int hi = INT32_MIN, sum = 0, n = 0;
        for (int c = 0; c < 4; c++) {
          const int pi = 2 * i + (c & 1), pj = 2 * j + (c >> 1);
          if (pi >= pw || pj >= ph) continue;
          const uint32_t w = p[(size_t)pj * pw + pi];
          hi = std::max(hi, UnMax(w));
          sum += UnAvg(w);
          n++;
        }
        l[(size_t)j * cw + i] = Pack(hi, sum / std::max(n, 1));
      }
    }
    mips.push_back(std::move(l));
  }

  // ---- pack ----
  out[kHValid] = 1u;
  out[kHX0] = (uint32_t)x0;
  out[kHZ0] = (uint32_t)z0;
  out[kHNX] = (uint32_t)nx;
  out[kHNZ] = (uint32_t)nz;
  out[kHLog2] = (uint32_t)kSampleLog2;
  out[kHLevels] = (uint32_t)mips.size();
  out[kHSeaY] = (uint32_t)sea;
  out[kHOuterY] = (uint32_t)outer;
  out[kHTreeline] = (uint32_t)m.terrain.treeline;
  out[kHMaxY] = (uint32_t)maxY;
  out[kHMatStone] = kMatStone;
  out[kHMatSnow] = kMatSnow;
  out[kHMatWater] = kMatWater;
  // Skin per biome id, from the packed records (the ids worldgen lays).
  if (wm.size() > worldmap::kHBiomeRecords) {
    const uint32_t count = std::min<uint32_t>(wm[worldmap::kHBiomeCount], kSkinSlots);
    const uint32_t recs = wm[worldmap::kHBiomeRecords];
    for (uint32_t b = 0; b < count; b++) {
      const size_t w = (size_t)recs + (size_t)b * worldmap::kBiomeRecWords + worldmap::kB_Skin;
      out[kHSkin + b] = w < wm.size() ? wm[w] : kMatStone;
    }
  }
  out[kHSamples] = (uint32_t)out.size();
  out.insert(out.end(), samples.begin(), samples.end());
  out[kHBiome] = (uint32_t)out.size();
  {
    const size_t bw = (ns + 3) / 4;
    const size_t at = out.size();
    out.resize(at + bw, 0u);
    std::memcpy(out.data() + at, biome.data(), ns);
  }
  for (size_t l = 0; l < mips.size(); l++) {
    out[kHMip + l] = (uint32_t)out.size();
    out.insert(out.end(), mips[l].begin(), mips[l].end());
  }
  out[kHTlPlane] = (uint32_t)out.size();
  {
    const size_t tw = (ns + 1) / 2;
    const size_t at = out.size();
    out.resize(at + tw, 0u);
    std::memcpy(out.data() + at, tline.data(), ns * sizeof(int16_t));
  }
  if (!water.empty()) {
    out[kHWater] = (uint32_t)out.size();
    out.insert(out.end(), water.begin(), water.end());
  }
}

}  // namespace horizon
