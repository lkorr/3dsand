#include "ui/item_stage.h"

#include <algorithm>
#include <cmath>

#include "game/dye.h"
#include "sim/materials.h"
#include "sim/tuning.h"

namespace itemstage {

namespace {

Vec3 Rot(const Vec3& v, const Vec3& axis, float a) {
  // Rodrigues, `axis` unit.
  const float c = std::cos(a), s = std::sin(a);
  return v * c + axis.cross(v) * s + axis * (axis.dot(v) * (1.0f - c));
}

Vec3 Col(uint32_t rgba) {   // MaterialGpu colour: R in the low byte
  return Vec3{(float)(rgba & 0xFFu), (float)((rgba >> 8) & 0xFFu),
              (float)((rgba >> 16) & 0xFFu)} * (1.0f / 255.0f);
}

Vec3 ArtCol(uint32_t trgb) {   // an art colour: 0xTTRRGGBB
  return Vec3{(float)((trgb >> 16) & 0xFFu), (float)((trgb >> 8) & 0xFFu),
              (float)(trgb & 0xFFu)} * (1.0f / 255.0f);
}

Vec3 Mix(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
Vec3 Mul(const Vec3& a, const Vec3& b) { return Vec3{a.x * b.x, a.y * b.y, a.z * b.z}; }

uint32_t Hash(int x, int y, int z) {
  uint32_t h = (uint32_t)x * 0x8DA6B343u ^ (uint32_t)y * 0xD8163841u ^ (uint32_t)z * 0xCB1AB31Fu;
  h ^= h >> 13;
  h *= 0x5BD1E995u;
  h ^= h >> 15;
  return h;
}
float HashF(int x, int y, int z) { return (float)(Hash(x, y, z) & 0xFFFFu) / 65535.0f; }

// common.wgsl valueNoise's shape (a smoothstepped trilinear lattice noise of
// period `scale`); the hash is this file's own, which is all a mottle needs.
float ValueNoise(Vec3 p, float scale) {
  const Vec3 q = p * (1.0f / std::max(scale, 1e-3f));
  const int ix = (int)std::floor(q.x), iy = (int)std::floor(q.y), iz = (int)std::floor(q.z);
  float fx = q.x - ix, fy = q.y - iy, fz = q.z - iz;
  fx = fx * fx * (3 - 2 * fx);
  fy = fy * fy * (3 - 2 * fy);
  fz = fz * fz * (3 - 2 * fz);
  auto L = [](float a, float b, float t) { return a + (b - a) * t; };
  const float x00 = L(HashF(ix, iy, iz), HashF(ix + 1, iy, iz), fx);
  const float x10 = L(HashF(ix, iy + 1, iz), HashF(ix + 1, iy + 1, iz), fx);
  const float x01 = L(HashF(ix, iy, iz + 1), HashF(ix + 1, iy, iz + 1), fx);
  const float x11 = L(HashF(ix, iy + 1, iz + 1), HashF(ix + 1, iy + 1, iz + 1), fx);
  return L(L(x00, x10, fy), L(x01, x11, fy), fz);
}

// The albedo of one voxel, before light: art or material palette, the dye,
// then the coat (microbody.wgsl's order: dye BEFORE the stain, because the
// coat goes on the cloth). Returns the coat's emission in `glow`.
Vec3 Albedo(const PrefabVoxel& v, const Look& look, float& glow) {
  glow = 0.0f;
  const std::vector<MaterialDef>& mats = *look.mats;
  const uint32_t m = v.material & 0xFFFu;
  Vec3 a{0.6f, 0.6f, 0.6f};
  if (v.color && look.artColors && (size_t)(v.color - 1) < look.artColors->size()) {
    a = ArtCol((*look.artColors)[v.color - 1]);
  } else if (m < mats.size()) {
    const uint32_t j = (uint32_t)(v.x * 7 + v.y * 13 + v.z * 29) % 3u;
    const MaterialGpu& g = mats[m].gpu;
    a = Col(j == 0 ? g.color0 : j == 1 ? g.color1 : g.color2);
  }
  if (DyeSet(look.dye)) {
    float d[3];
    DyeUnpack(look.dye, d);
    const float lum = 0.2126f * a.x + 0.7152f * a.y + 0.0722f * a.z;
    a = Vec3{d[0], d[1], d[2]} * (lum / kDyeRef);
  }
  const uint32_t amtI = BodyStainAmt(v.stain);
  const uint32_t cm = BodyStainMat(v.stain);
  if (amtI && cm < mats.size()) {
    const auto& R = CurrentTuning().render;
    const uint32_t packed = mats[cm].gpu.stainColor;
    const float opacity = (float)(packed >> 24) / 255.0f;
    const float amt = (float)amtI / (float)kBodyStainAmtMax;
    const float mottle = ValueNoise(Vec3{(float)v.x, (float)v.y, (float)v.z},
                                    R.stainMottleScale * (float)std::max(1u, look.scale));
    const float cover =
        opacity * std::clamp((amt * (1.0f + R.stainMottle) - mottle * R.stainMottle) *
                                 R.stainCoverage,
                             0.0f, 1.0f);
    if (cover > 0.0f) {
      const Vec3 sc = Col(packed);
      const Vec3 soaked = Mul(a, Mix(Vec3{1, 1, 1}, sc * R.stainDarken, cover));
      a = Mix(soaked, sc, cover * R.stainOpacity);
      glow = (float)(mats[cm].gpu._r3 & 0xFFu) / 255.0f * cover;
    }
  }
  return a;
}

uint8_t U8(float f) { return (uint8_t)std::clamp((int)std::lround(f * 255.0f), 0, 255); }

}  // namespace

void StageCamera::Ray(float px, float py, Vec3& ro, Vec3& rd) const {
  const float x = (px / (float)w * 2.0f - 1.0f) * halfW;
  const float y = (1.0f - py / (float)h * 2.0f) * halfH;
  ro = centre + right * x + up * y + back * depth;
  rd = back * -1.0f;
}

void StageCamera::Project(Vec3 p, float& px, float& py) const {
  const Vec3 d = p - centre;
  px = (d.dot(right) / halfW + 1.0f) * 0.5f * (float)w;
  py = (1.0f - d.dot(up) / halfH) * 0.5f * (float)h;
}

StageCamera MakeCamera(const LatticeGrid& g, const View& v, int w, int h) {
  StageCamera c;
  c.w = std::max(1, w);
  c.h = std::max(1, h);
  c.centre = g.Centre();
  const float rad = std::max(1.0f, g.Radius());
  // Longest axis across, middle axis up, thinnest toward the viewer.
  const int ext[3] = {g.dim.x, g.dim.y, g.dim.z};
  int ord[3] = {0, 1, 2};
  std::stable_sort(ord, ord + 3, [&](int a, int b) { return ext[a] > ext[b]; });
  const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  Vec3 right = axes[ord[0]], up = axes[ord[1]];
  Vec3 back = right.cross(up);
  // The orbit: yaw about the screen's up, then pitch about the new right.
  right = Rot(right, up, v.yaw);
  back = Rot(back, up, v.yaw);
  up = Rot(up, right, v.pitch);
  back = Rot(back, right, v.pitch);
  c.right = right.normalized();
  c.up = up.normalized();
  c.back = back.normalized();
  // THE ITEM FILLS THE PICTURE AT ZOOM 1, as laid in the base view: its
  // longest extent across the width, its middle one up the height, a margin
  // for the outline. (It was the bounding SPHERE on the shorter side, which
  // drew a sword -- long and thin -- across 40% of the picture.) Turned on
  // end it can overrun the frame; the wheel zooms out.
  const float aspect = (float)c.w / (float)c.h;
  const float needH = std::max((float)ext[ord[1]] * 0.5f,
                               (float)ext[ord[0]] * 0.5f / std::max(0.2f, aspect));
  const float fit = std::max(needH * 1.2f, rad * 0.45f) / std::max(0.05f, v.zoom);
  c.halfH = fit;
  c.halfW = fit * aspect;
  c.depth = rad * 2.0f + 4.0f;
  return c;
}

int Pick(const LatticeGrid& g, const StageCamera& cam, float px, float py) {
  Vec3 ro, rd;
  cam.Ray(px, py, ro, rd);
  const Hit h = Raycast(g, ro, rd);
  return h.hit ? h.voxel : -1;
}

void Render(const LatticeGrid& g, const std::vector<PrefabVoxel>& lat,
            const StageCamera& cam, const Look& look, std::vector<uint8_t>& rgba) {
  const int W = cam.w, H = cam.h;
  rgba.assign((size_t)W * H * 4, 0);
  if (g.Empty() || !look.mats) return;
  std::vector<float> depth((size_t)W * H, -1.0f);
  struct VoxelLook {
    bool done = false, hasGrad = false;
    Vec3 grad{}, albedo{};
    float glow = 0.0f;
  };
  std::vector<VoxelLook> cache(lat.size());
  // Studio light, fixed to the CAMERA so it turns with the orbit: a warm key
  // from the upper left, a cool fill from the right, a rim from behind.
  const Vec3 key = (cam.right * -0.55f + cam.up * 0.70f + cam.back * 0.55f).normalized();
  const Vec3 fill = (cam.right * 0.75f + cam.up * -0.15f + cam.back * 0.45f).normalized();
  const Vec3 keyCol{1.00f, 0.95f, 0.86f}, fillCol{0.55f, 0.62f, 0.78f};
  const Vec3 rimCol{0.95f, 0.85f, 0.62f};
  // Only the pixels the grid's box can cover are shot: the box's eight
  // corners projected, padded a pixel. A sword laid across covers a band a
  // quarter of the picture tall, and every ray outside it would miss.
  int bx0 = W, by0 = H, bx1 = -1, by1 = -1;
  for (int k = 0; k < 8; k++) {
    const Vec3 corner{(float)(g.lo.x + ((k & 1) ? g.dim.x : 0)),
                      (float)(g.lo.y + ((k & 2) ? g.dim.y : 0)),
                      (float)(g.lo.z + ((k & 4) ? g.dim.z : 0))};
    float qx, qy;
    cam.Project(corner, qx, qy);
    bx0 = std::min(bx0, (int)std::floor(qx) - 1);
    by0 = std::min(by0, (int)std::floor(qy) - 1);
    bx1 = std::max(bx1, (int)std::ceil(qx) + 1);
    by1 = std::max(by1, (int)std::ceil(qy) + 1);
  }
  bx0 = std::max(bx0, 0);
  by0 = std::max(by0, 0);
  bx1 = std::min(bx1, W - 1);
  by1 = std::min(by1, H - 1);
  for (int py = by0; py <= by1; py++) {
    for (int px = bx0; px <= bx1; px++) {
      Vec3 ro, rd;
      cam.Ray((float)px + 0.5f, (float)py + 0.5f, ro, rd);
      const Hit h = Raycast(g, ro, rd);
      if (!h.hit) continue;
      const PrefabVoxel& v = lat[(size_t)h.voxel];
      // The face normal, blended toward the occupancy gradient over the
      // 3x3x3 neighbourhood (microbody.wgsl BODY_SMOOTH_N): rounded parts
      // read round, a flat blade keeps its facets.
      Vec3 nFace = cam.back;
      if (h.axis >= 0) {
        const float s = (float)-h.sign;
        nFace = h.axis == 0 ? Vec3{s, 0, 0} : h.axis == 1 ? Vec3{0, s, 0} : Vec3{0, 0, s};
      }
      // The gradient and the albedo belong to the VOXEL, and a voxel covers
      // several pixels: each is worked out once per redraw (it was 26 grid
      // reads and the whole coat model per pixel).
      VoxelLook& vl = cache[(size_t)h.voxel];
      if (!vl.done) {
        Vec3 grad{};
        for (int dz = -1; dz <= 1; dz++)
          for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
              if (!dx && !dy && !dz) continue;
              if (g.At(h.cell.x + dx, h.cell.y + dy, h.cell.z + dz) >= 0) continue;
              const float inv = 1.0f / std::sqrt((float)(dx * dx + dy * dy + dz * dz));
              grad += Vec3{(float)dx, (float)dy, (float)dz} * inv;
            }
        vl.hasGrad = grad.len() > 1e-4f;
        vl.grad = vl.hasGrad ? grad.normalized() : Vec3{};
        vl.albedo = Albedo(v, look, vl.glow);
        vl.done = true;
      }
      Vec3 n = nFace;
      if (vl.hasGrad) n = Mix(nFace, vl.grad, 0.55f).normalized();
      const float glow = vl.glow;
      const Vec3 alb = vl.albedo;
      // Banded key light: four steps, so the shading reads as pixel art.
      const float kd = std::max(0.0f, n.dot(key));
      const float kb = std::floor(kd * 4.0f + 0.35f) / 4.0f;
      const float fd = std::max(0.0f, n.dot(fill));
      const float facing = std::max(0.0f, n.dot(cam.back));
      const float rim = std::pow(1.0f - facing, 3.0f) * 0.55f;
      Vec3 light = Vec3{0.24f, 0.23f, 0.27f} + keyCol * (kb * 0.85f) +
                   fillCol * (fd * 0.30f) + rimCol * rim;
      Vec3 c = Mul(alb, light) + alb * glow;
      // A cool lift on the shadow side, so dark metal does not go to black.
      c = c + Vec3{0.02f, 0.025f, 0.04f};
      const size_t o = ((size_t)py * W + px) * 4;
      rgba[o + 0] = U8(c.x);
      rgba[o + 1] = U8(c.y);
      rgba[o + 2] = U8(c.z);
      rgba[o + 3] = 255;
      depth[(size_t)py * W + px] = h.t;
    }
  }
  // THE INK: a one-pixel outline round the silhouette, and a darker pixel
  // where the depth jumps inside it (a crossguard in front of the blade).
  const uint8_t ink[3] = {22, 17, 26};
  std::vector<uint8_t> out = rgba;
  for (int py = 0; py < H; py++) {
    for (int px = 0; px < W; px++) {
      const size_t i = (size_t)py * W + px;
      const float d = depth[i];
      const int nx[4] = {px - 1, px + 1, px, px};
      const int ny[4] = {py, py, py - 1, py + 1};
      bool edge = false, crease = false;
      for (int k = 0; k < 4; k++) {
        if (nx[k] < 0 || ny[k] < 0 || nx[k] >= W || ny[k] >= H) continue;
        const float dn = depth[(size_t)ny[k] * W + nx[k]];
        if (d < 0.0f && dn >= 0.0f) edge = true;
        if (d >= 0.0f && dn >= 0.0f && d - dn > 2.5f) crease = true;
      }
      if (edge) {
        out[i * 4 + 0] = ink[0];
        out[i * 4 + 1] = ink[1];
        out[i * 4 + 2] = ink[2];
        out[i * 4 + 3] = 255;
      } else if (crease) {
        for (int ch = 0; ch < 3; ch++)
          out[i * 4 + ch] = (uint8_t)((out[i * 4 + ch] * 3 + ink[ch] * 2) / 5);
      }
    }
  }
  rgba.swap(out);
}

}  // namespace itemstage
