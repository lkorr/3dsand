// boltfx.cpp — see boltfx.h. Render-only.
#include "sim/boltfx.h"

#include <algorithm>
#include <cmath>

#include "gpu/rhi.h"

namespace boltfx {
namespace {

// ---- render-only hash (never the sim's rng: nothing here may be hashed) ----
uint32_t H(uint32_t a, uint32_t b) {
  uint32_t x = a * 0x9E3779B9u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu;
  x ^= x >> 16; x *= 0x7FEB352Du;
  x ^= x >> 15; x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}
float U(uint32_t h) { return (float)(h >> 8) * (1.0f / 16777216.0f); }   // [0,1)
float S(uint32_t h) { return U(h) * 2.0f - 1.0f; }                        // [-1,1)

struct Seg {
  Vec3 a, b;
  uint8_t level;   // 0 channel, 1 branch / fork, 2 splash
  float width;     // core radius, fine voxels
  float gain;      // brightness along a branch: tapers toward its tip
};

struct Bolt {
  float t0 = 0.0f;
  uint32_t seed = 0;
  bool channel = false;          // false = a shock: splash only
  std::vector<Seg> segs;
  std::vector<Vec3> lights;      // light points, foot first
  float strokeT[4] = {0, 0, 0, 0};
  float strokeA[4] = {0, 0, 0, 0};
  int strokes = 1;
  float life = 0.3f;
};

std::vector<Bolt> gBolts;
const rhi::Buffer* gBuf = nullptr;
bool gTableEmpty = false;        // the GPU table is known to say "no bolt"
std::vector<float> gRows;        // staging, kRows * 4

// Core radii (fine voxels = 0.1 m). The raymarch widens a core that would be
// thinner than a pixel and scales its brightness down to match, so a bolt
// 100 m off is still a one-pixel line rather than a dotted one.
constexpr float kWidthChannel = 0.30f;
constexpr float kWidthBranch = 0.16f;
constexpr float kWidthSplash = 0.045f;
// How far the channel is continued above the planned bolt: out of the window
// toward the cloud deck, render-only. 900 fine voxels = 90 m.
constexpr float kSkyRise = 900.0f;
constexpr float kSkyNear = 300.0f;

// Midpoint displacement: every segment of `pts` gets a displaced midpoint,
// `levels` times; the offset is perpendicular to the segment, `amp` of its
// length. A cell path (one cell a step, a kick every few) becomes a jagged
// line with structure below the cell.
void Jag(std::vector<Vec3>& pts, float amp, uint32_t seed, int levels) {
  for (int lev = 0; lev < levels; lev++) {
    if (pts.size() < 2) return;
    std::vector<Vec3> out;
    out.reserve(pts.size() * 2);
    for (size_t i = 0; i + 1 < pts.size(); i++) {
      const Vec3 a = pts[i], b = pts[i + 1];
      const Vec3 d = b - a;
      const float L = d.len();
      out.push_back(a);
      if (L < 0.6f) continue;
      const Vec3 dir = d * (1.0f / L);
      const Vec3 ref = std::fabs(dir.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
      const Vec3 u = dir.cross(ref).normalized();
      const Vec3 v = dir.cross(u);
      const uint32_t h = H(seed, (uint32_t)(lev * 4099 + (int)i));
      const Vec3 off = (u * S(h) + v * S(H(h, 7u))) * (amp * L);
      out.push_back(a + d * (0.4f + 0.2f * U(H(h, 11u))) + off);
    }
    out.push_back(pts.back());
    pts.swap(out);
  }
}

Vec3 Centre(const IVec3& c) { return {(float)c.x + 0.5f, (float)c.y + 0.5f, (float)c.z + 0.5f}; }

// `taper` > 0: brightness and width fall from 1 to (1 - taper) along the line
// (a branch dies out toward its tip).
void AddLine(Bolt& b, const std::vector<Vec3>& pts, uint8_t level, float width,
             float taper = 0.0f) {
  const size_t n = pts.size() > 1 ? pts.size() - 1 : 1;
  for (size_t i = 0; i + 1 < pts.size(); i++) {
    const float g = 1.0f - taper * (float)i / (float)n;
    b.segs.push_back({pts[i], pts[i + 1], level, width * (0.5f + 0.5f * g), g});
  }
}

// A render-only branch: down and away from `from`, `steps` legs.
void Branch(Bolt& b, Vec3 from, uint32_t seed, int steps, float legLen) {
  std::vector<Vec3> pts{from};
  const float ang = U(H(seed, 1u)) * 6.2831853f;
  Vec3 dir{std::cos(ang), -0.7f - 0.5f * U(H(seed, 2u)), std::sin(ang)};
  Vec3 p = from;
  for (int k = 0; k < steps; k++) {
    const uint32_t h = H(seed, 16u + (uint32_t)k);
    const Vec3 jit{S(h) * 0.5f, S(H(h, 3u)) * 0.3f, S(H(h, 5u)) * 0.5f};
    p += (dir + jit).normalized() * (legLen * (0.7f + 0.6f * U(H(h, 9u))));
    pts.push_back(p);
  }
  Jag(pts, 0.2f, H(seed, 77u), 3);
  AddLine(b, pts, 1, kWidthBranch, 0.8f);
  // One sub-branch off its first half, shorter and fainter still.
  if (steps >= 3 && (H(seed, 91u) & 1u)) {
    const size_t at = 1 + (size_t)(H(seed, 92u) % (uint32_t)std::max<size_t>(1, pts.size() / 2));
    std::vector<Vec3> sub{pts[at]};
    Vec3 q = pts[at];
    const float a2 = U(H(seed, 93u)) * 6.2831853f;
    const Vec3 d2{std::cos(a2), -0.9f, std::sin(a2)};
    for (int k = 0; k < 2; k++) {
      q += d2.normalized() * (legLen * (0.5f + 0.4f * U(H(seed, 94u + (uint32_t)k))));
      sub.push_back(q);
    }
    Jag(sub, 0.2f, H(seed, 95u), 3);
    for (size_t i = 0; i + 1 < sub.size(); i++) {
      const float g = 0.45f * (1.0f - 0.8f * (float)i / (float)(sub.size() - 1));
      b.segs.push_back({sub[i], sub[i + 1], 1, kWidthBranch * 0.6f, g});
    }
  }
}

// Position at arc length `s` from the start of polyline `pts`.
Vec3 AlongPolyline(const std::vector<Vec3>& pts, float s) {
  for (size_t i = 0; i + 1 < pts.size(); i++) {
    const float L = (pts[i + 1] - pts[i]).len();
    if (s <= L && L > 0.0f) return pts[i] + (pts[i + 1] - pts[i]) * (s / L);
    s -= L;
  }
  return pts.empty() ? Vec3{} : pts.back();
}

// The envelope of one level of one bolt at age t (seconds): the strokes, the
// continuing current, a per-frame flicker, the fade.
float Envelope(const Bolt& b, float t, int level) {
  if (t < 0.0f || t > b.life) return 0.0f;
  float stroke = 0.0f;
  for (int i = 0; i < b.strokes; i++) {
    const float dt = t - b.strokeT[i];
    if (dt < 0.0f) continue;
    float a = b.strokeA[i];
    // Re-strikes run down the CHANNEL; the branches light on the first stroke
    // (and a faint echo after), the splash on the first only.
    if (i > 0 && level == 1) a *= 0.18f;
    if (i > 0 && level == 2) a *= 0.35f;
    if (level == 2) a *= 0.55f;   // the splash: thin crackle, not a second bolt
    stroke += a * std::exp(-dt / (level == 2 ? 0.05f : 0.035f));
  }
  const float glow = (level == 0 ? 0.16f : level == 1 ? 0.05f : 0.0f) * std::exp(-t / 0.22f);
  // Flicker re-rolled at 60 Hz of bolt age: the same picture for the same t.
  const uint32_t fq = (uint32_t)(t * 60.0f);
  const float fl = level == 2 ? 0.45f + 0.55f * U(H(b.seed ^ 0x5A5Au, fq))
                              : 0.78f + 0.22f * U(H(b.seed, fq));
  const float fade = std::clamp((b.life - t) / 0.12f, 0.0f, 1.0f);
  return (stroke + glow) * fl * fade;
}

}  // namespace

void SetBuffer(const rhi::Buffer* buf) {
  gBuf = buf;
  gTableEmpty = false;
}

void Clear() { gBolts.clear(); }

void NoteStrike(const std::vector<StrikePath>& paths, float time, uint32_t seed) {
  if (paths.empty()) return;
  Bolt b;
  b.t0 = time;
  b.seed = H(seed, 0xB017u);
  const uint32_t s0 = b.seed;

  std::vector<Vec3> channel;   // sky -> foot, once built
  for (const StrikePath& p : paths) {
    if (p.kind != StrikePath::Channel || p.cells.size() < 2) continue;
    b.channel = true;
    // Keypoints every 3 cells of the plan's walk, the last cell kept, then
    // down to the struck top's surface (the walk ends in the foot cell, the
    // air just above it).
    std::vector<Vec3> pts;
    for (size_t i = 0; i < p.cells.size(); i += 3) pts.push_back(Centre(p.cells[i]));
    if ((p.cells.size() - 1) % 3 != 0) pts.push_back(Centre(p.cells.back()));
    const IVec3 f = p.cells.back();
    pts.push_back({(float)f.x + 0.5f, (float)f.y + 0.02f, (float)f.z + 0.5f});
    // Continue UP out of the plan toward the deck, wandering sideways more
    // the higher it goes. Two scales: short legs over the first kSkyNear cells (what a camera near
    // the strike sees, jagged three levels deep), long ones above (far, and
    // mostly out of frame, two levels).
    std::vector<Vec3> near{pts.front()}, far;
    Vec3 q = pts.front();
    int leg = 0;
    while (q.y - pts.front().y < kSkyRise && leg < 48) {
      const uint32_t h = H(s0, 0x5C00u + (uint32_t)leg);
      const float up = q.y - pts.front().y;
      const bool isNear = up < kSkyNear;
      const float wander = isNear ? 7.0f : 14.0f + 0.02f * up;
      const float rise = isNear ? 16.0f + 12.0f * U(H(h, 1u)) : 45.0f + 30.0f * U(H(h, 1u));
      q += Vec3{S(h) * wander, rise, S(H(h, 2u)) * wander};
      (isNear ? near : far).push_back(q);
      leg++;
    }
    Jag(near, 0.2f, H(s0, 0x50u), 3);
    if (!far.empty()) {
      far.insert(far.begin(), near.back());
      Jag(far, 0.2f, H(s0, 0x51u), 2);
    }
    // Top down: far, near, then the planned part.
    std::vector<Vec3> sky(far.rbegin(), far.rend());
    if (!sky.empty()) sky.pop_back();   // near.back() again
    sky.insert(sky.end(), near.rbegin(), near.rend());
    Jag(pts, 0.24f, H(s0, 0x52u), 2);
    channel = sky;
    channel.insert(channel.end(), pts.begin() + 1, pts.end());
    AddLine(b, channel, 0, kWidthChannel);
    // Render-only side branches off the sky part, dying out on the way down.
    const int nb = 7 + (int)(H(s0, 0xB4u) % 4u);
    const size_t skyN = sky.size();
    for (int k = 0; k < nb && skyN > 8; k++) {
      const uint32_t hb = H(s0, 0xB500u + (uint32_t)k);
      // Biased low (sky is top-down): u^2 from the bottom end.
      const float u = U(hb);
      const size_t at = skyN - 3 - (size_t)((float)(skyN - 6) * u * u);
      // Branches nearer the ground are shorter (a camera at the strike sees
      // those); high ones reach further.
      const float up = sky[at].y - pts.front().y;
      const float leg = std::min(12.0f + 0.05f * up, 40.0f) * (0.7f + 0.6f * U(H(hb, 4u)));
      Branch(b, sky[at], hb, 3 + (int)(H(hb, 3u) % 3u), leg);
    }
    break;
  }
  for (const StrikePath& p : paths) {
    if (p.kind == StrikePath::Channel || p.cells.size() < 2) continue;
    std::vector<Vec3> pts;
    for (const IVec3& c : p.cells) pts.push_back(Centre(c));
    const bool fork = p.kind == StrikePath::Fork;
    Jag(pts, fork ? 0.3f : 0.32f, H(s0, (uint32_t)pts.size() * 31u + (uint32_t)b.segs.size()), 2);
    AddLine(b, pts, fork ? 1 : 2, fork ? kWidthBranch : kWidthSplash, fork ? 0.6f : 0.5f);
  }
  if (b.segs.empty()) return;

  // Light points: along the channel from the foot up (the lower channel is
  // what lights the ground), or the splash's origin for a shock.
  if (b.channel) {
    std::vector<Vec3> up(channel.rbegin(), channel.rend());
    static const float kAt[8] = {0.5f, 8.0f, 24.0f, 56.0f, 110.0f, 200.0f, 340.0f, 540.0f};
    for (float s : kAt) b.lights.push_back(AlongPolyline(up, s));
    // (every light point at the channel's envelope: the shader's falloff
    // already makes the lower ones dominate what they light)
  } else {
    b.lights.push_back(b.segs.front().a);
  }

  // The strokes: the return stroke at 0, then 1..3 re-strikes 40..150 ms
  // apart, each a little weaker than the first.
  b.strokes = b.channel ? 2 + (int)(H(s0, 0x57u) % 3u) : 1;
  b.strokeT[0] = 0.0f;
  b.strokeA[0] = 1.0f;
  for (int i = 1; i < b.strokes; i++) {
    const uint32_t h = H(s0, 0x5700u + (uint32_t)i);
    b.strokeT[i] = b.strokeT[i - 1] + 0.04f + 0.11f * U(h);
    b.strokeA[i] = 0.55f + 0.4f * U(H(h, 1u));
  }
  b.life = b.strokeT[b.strokes - 1] + (b.channel ? 0.32f : 0.16f);

  if (gBolts.size() >= kMaxBolts) gBolts.erase(gBolts.begin());
  gBolts.push_back(std::move(b));
}

bool Active(float time) {
  for (const Bolt& b : gBolts)
    if (time - b.t0 >= 0.0f && time - b.t0 <= b.life) return true;
  return false;
}

void Upload(const rhi::Queue& queue, float time) {
  if (gBuf == nullptr) return;
  // Retire what has burnt out (a bolt from the future -- the render clock
  // jumped back, a shot rig -- is kept until its time comes).
  gBolts.erase(std::remove_if(gBolts.begin(), gBolts.end(),
                              [&](const Bolt& b) { return time - b.t0 > b.life; }),
               gBolts.end());
  const bool any = Active(time);
  if (!any && gTableEmpty) return;
  gRows.assign((size_t)kRows * 4, 0.0f);
  uint32_t nSeg = 0, nLight = 0;
  if (any) {
    for (const Bolt& b : gBolts) {
      const float t = time - b.t0;
      if (t < 0.0f || t > b.life) continue;
      const float e[3] = {Envelope(b, t, 0), Envelope(b, t, 1), Envelope(b, t, 2)};
      for (const Seg& s : b.segs) {
        if (nSeg >= kMaxSegs) break;
        const float in = e[s.level] * s.gain;
        if (in <= 1e-3f) continue;
        float* r = &gRows[(size_t)(kRowSegs + 2 * nSeg) * 4];
        r[0] = s.a.x; r[1] = s.a.y; r[2] = s.a.z; r[3] = in;
        r[4] = s.b.x; r[5] = s.b.y; r[6] = s.b.z; r[7] = s.width;
        nSeg++;
      }
      const float li = b.channel ? e[0] : e[2] * 0.15f;
      for (const Vec3& p : b.lights) {
        if (nLight >= kMaxLights || li <= 1e-3f) break;
        float* r = &gRows[(size_t)(kRowLights + nLight) * 4];
        r[0] = p.x; r[1] = p.y; r[2] = p.z; r[3] = li;
        nLight++;
      }
    }
  }
  // Groups of kGroupSegs consecutive segments (consecutive along a path, so a
  // group is a compact box), each with its AABB of endpoints +- width.
  const uint32_t nGroup = (nSeg + kGroupSegs - 1) / kGroupSegs;
  for (uint32_t g = 0; g < nGroup; g++) {
    const uint32_t first = g * kGroupSegs, cnt = std::min(kGroupSegs, nSeg - first);
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = first; i < first + cnt; i++) {
      const float* r = &gRows[(size_t)(kRowSegs + 2 * i) * 4];
      for (int k = 0; k < 3; k++) {
        lo[k] = std::min(lo[k], std::min(r[k], r[4 + k]) - r[7]);
        hi[k] = std::max(hi[k], std::max(r[k], r[4 + k]) + r[7]);
      }
    }
    float* r = &gRows[(size_t)(kRowGroups + 2 * g) * 4];
    r[0] = lo[0]; r[1] = lo[1]; r[2] = lo[2]; r[3] = (float)first;
    r[4] = hi[0]; r[5] = hi[1]; r[6] = hi[2]; r[7] = (float)cnt;
  }
  gRows[0] = (float)nSeg;
  gRows[1] = (float)nGroup;
  gRows[2] = (float)nLight;
  gRows[3] = 0.0f;
  // Only the rows in use (the header says how many): a frame with no bolt
  // writes 16 bytes.
  const size_t usedRows = nSeg == 0 && nLight == 0 ? 1 : (size_t)kRows;
  queue.WriteBuffer(*gBuf, 0, gRows.data(), usedRows * 16);
  gTableEmpty = nSeg == 0 && nLight == 0;
}

}  // namespace boltfx
