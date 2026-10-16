// mob_stealth.cpp -- MobSystem's half of stealth and detection (game/stealth.h,
// docs/PLAN_stealth.md): what a body looks like to a watcher (the light on
// it, the noise it makes, its skill), the noise events every brain hears one
// tick later, the sneak-attack test, and the HUD's report of who is watching.
//
// The METER itself lives with the AI (ai_behavior.cpp UpdateAwareness): this
// file only measures the world for it, and it measures from the same stores
// the line of sight reads -- the chunk caches and the tick's day phase and
// rain word -- so a replay of the same ticks measures the same light.

#include <algorithm>
#include <climits>
#include <cmath>

#include "game/ai_behavior.h"
#include "game/mob.h"
#include "game/stealth.h"
#include "sim/rng.h"
#include "sim/tuning.h"
#include "sim/world.h"

namespace {

// How often a body's light is re-measured (ticks), staggered by id: a player
// every other tick, so the HUD's light meter follows a step into a doorway's
// shadow; a creature every sixth. Worn metal every thirtieth.
constexpr uint32_t kLightEveryPlayer = 2;
constexpr uint32_t kLightEveryNpc = 6;
constexpr uint32_t kMetalEvery = 30;

// THE SHADOW RAY'S REACH, world voxels (25.6 m), cell by cell. The renderer's
// traceOpaque spends render.shadowSteps (384) steps, going COARSE (4^3 blocks)
// past the near field; the CPU walks cells to here and then asks the same
// question the renderer asks of a ray that ran out -- is its end under the
// ground? (shadowFromOpaqueHit: "out of steps underground is a far blocker").
constexpr float kShadowReach = 256.0f;
// How far DOWN a body looks for the ground its ambient is measured on:
// common.wgsl opennessAtBody's six 4^3 blocks (2.4 m). Past that it is over a
// hole and the ambient is unscaled, as there.
constexpr int kGroundWalk = 24;

float Lum3(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
float Smooth(float e0, float e1, float x) {
  const float t = std::clamp((x - e0) / std::max(e1 - e0, 1e-6f), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// EVERY CELL A LIGHT RAY CAN MEET, from the freshest store that holds it:
// the tick's snapshot MIRROR (the 27 chunks round the player, this tick's
// words), then the FETCH CACHE (what the creatures' own probes fetched, and
// what the mirror seeded as the player walked), then the snapshot's
// per-chunk OCCUPANCY (a chunk with no matter in it is all air: the sky, by
// far the most of what a light ray crosses) -- and for a cell nothing holds,
// the ANALYTIC GROUND (World::TerrainHeight, worldgen's own height contract)
// stands in, so a hill whose chunks were never fetched still casts its
// shadow and still roofs a cave. It asks no fetches. One resolve per chunk
// a ray enters, not per cell.
class LightCells {
 public:
  LightCells(const World& w, const std::vector<uint8_t>& blocks)
      : w_(w), snap_(w.Snap()), blocks_(blocks), seed_(w.WorldSeed()) {
    mirrorOk_ = snap_.valid && snap_.mirror.size() == 27u * kChunkVol;
    occOk_ = snap_.valid && snap_.occupancy.size() >= kNumChunks;
  }
  // 1 = stops light (common.wgsl isRayBlockerW), 0 = lets it through,
  // -1 = no store holds the word.
  int At(int x, int y, int z) {
    const int cx = x >> 4, cy = y >> 4, cz = z >> 4;
    if (cx != mx_ || cy != my_ || cz != mz_) Resolve(cx, cy, cz);
    if (words_ == nullptr) return kind_;
    return Word(words_[((uint32_t)(z & 15) * kChunk + (uint32_t)(y & 15)) * kChunk +
                       (uint32_t)(x & 15)]);
  }
  // Under the analytic ground. Memoised per column for the call's lifetime:
  // a ray walks many cells of few columns, and one answer is a map lookup
  // and a noise stack.
  bool Buried(int x, int y, int z) {
    for (const Col& c : cols_)
      if (c.x == x && c.z == z) return y <= c.h;
    Col c{x, z, World::TerrainHeight(x, z, seed_)};
    cols_[nextCol_++ & 15] = c;
    return y <= c.h;
  }

 private:
  void Resolve(int cx, int cy, int cz) {
    mx_ = cx;
    my_ = cy;
    mz_ = cz;
    words_ = nullptr;
    kind_ = -1;
    const IVec3 wc{cx, cy, cz};
    if (!w_.ChunkInWindow(wc)) return;
    if (mirrorOk_) {
      const int ax = cx - snap_.mirrorBase.x, ay = cy - snap_.mirrorBase.y,
                az = cz - snap_.mirrorBase.z;
      if (ax >= 0 && ay >= 0 && az >= 0 && ax < 3 && ay < 3 && az < 3) {
        words_ = snap_.mirror.data() + (size_t)((az * 3 + ay) * 3 + ax) * kChunkVol;
        return;
      }
    }
    const CachedChunk* cc = w_.Cached(wc);
    if (cc != nullptr && cc->voxels.size() == kChunkVol) {
      words_ = cc->voxels.data();
      return;
    }
    if (occOk_ && snap_.occupancy[World::SlotChunkIndex(wc)] == 0u) kind_ = 0;
  }
  int Word(uint32_t w) const {
    const uint32_t m = w & 0xFFFu;
    if (m == 0 || m >= blocks_.size()) return 0;
    const uint8_t b = blocks_[m];
    if (b == 2) return PowderMassOfState((w >> 12) & 0xFu) >= kPowderBlockMin ? 1 : 0;
    return b;
  }
  struct Col {
    int x = INT_MIN, z = INT_MIN, h = 0;
  };
  const World& w_;
  const WorldSnapshot& snap_;
  const std::vector<uint8_t>& blocks_;
  uint32_t seed_ = 0;
  bool mirrorOk_ = false, occOk_ = false;
  int mx_ = INT_MIN, my_ = INT_MIN, mz_ = INT_MIN;
  const uint32_t* words_ = nullptr;
  int kind_ = -1;
  Col cols_[16];
  uint32_t nextCol_ = 0;
};

// ONE RAY, CELL BY CELL (Amanatides & Woo): the distance to the first cell
// that stops light, or none within `maxT` -- and then whether the ray's END is
// under the analytic ground. A cell no store holds is asked of the ground
// every fourth step (a ridge is wider than four cells; a wall the caches do
// not know is not a ridge, and is open, as an unfetched chunk is everywhere
// in this engine).
struct LightRay {
  bool hit = false;
  float t = 0.0f;
  bool buriedEnd = false;
};
LightRay March(LightCells& L, Vec3 o, Vec3 d, float maxT) {
  LightRay r;
  int x = ifloor(o.x), y = ifloor(o.y), z = ifloor(o.z);
  const int sx = d.x >= 0.0f ? 1 : -1, sy = d.y >= 0.0f ? 1 : -1, sz = d.z >= 0.0f ? 1 : -1;
  auto inv = [](float v) { return std::abs(v) > 1e-6f ? 1.0f / std::abs(v) : 1e30f; };
  const float dx = inv(d.x), dy = inv(d.y), dz = inv(d.z);
  auto first = [](float o, int c, float dir, float invd) {
    if (std::abs(dir) <= 1e-6f) return 1e30f;
    return (dir > 0.0f ? ((float)(c + 1) - o) : (o - (float)c)) * invd;
  };
  float tx = first(o.x, x, d.x, dx), ty = first(o.y, y, d.y, dy), tz = first(o.z, z, d.z, dz);
  float t = 0.0f;
  int unknown = 0;
  while (t <= maxT) {
    const int b = L.At(x, y, z);
    if (b == 1) {
      r.hit = true;
      r.t = t;
      return r;
    }
    if (b < 0) {
      if ((unknown++ & 3) == 0 && L.Buried(x, y, z)) {
        r.hit = true;
        r.t = t;
        return r;
      }
    } else {
      unknown = 0;
    }
    if (tx < ty && tx < tz) {
      x += sx;
      t = tx;
      tx += dx;
    } else if (ty < tz) {
      y += sy;
      t = ty;
      ty += dy;
    } else {
      z += sz;
      t = tz;
      tz += dz;
    }
  }
  r.t = maxT;
  const Vec3 e = o + d * maxT;
  r.buriedEnd = L.Buried(ifloor(e.x), ifloor(e.y), ifloor(e.z));
  return r;
}

// THE RENDERER'S BODY SHADING, FOR A WHITE BODY, AS LUMINANCE: microbody.wgsl
// -> common.wgsl litColorSNoFog + glowLight, term for term, averaged over the
// five faces a standing body turns to the world (up and the four sides).
//   ambient   ambientHemi(n) (the overcast grey included) x openScale, plus
//             the ENCLOSED ambient x (1 - raw openness) -- ambientOpen;
//   key       keyCol x wrapDiffuse(n.L) x shadowLiftCap(sh, raw);
//   face      render.faceX / faceZ on the side faces (litColorSNoFog's tie
//             breaker between the horizontal axes);
//   glow      the field's luminance x openScale (the `ao` glowLight takes:
//             open.x) x render.glowStrength.
// `keyScale` is the rain deck's dimming of the key (stealth.rainKeyDim).
float BodyRadiance(const stealth::SkyLight& s, const Tuning::Render& r, float sh,
                   float openRaw, float glowLum, float keyScale) {
  const float strength = std::clamp(r.opennessStrength, 0.0f, 1.0f);
  const float openScale =
      openRaw < 0.0f ? 1.0f : 1.0f + (std::max(openRaw, r.opennessFloor) - 1.0f) * strength;
  float cap = sh;   // shadowLiftCap
  if (openRaw >= 0.0f) {
    const float full = Smooth(std::min(r.shadowLift, 0.99f), 1.0f, sh) * Smooth(0.0f, 0.05f, openRaw);
    cap = sh * (openRaw + (1.0f - openRaw) * full);
  }
  const float keyLum = Lum3(s.keyCol) * keyScale;
  const float enclosed =
      openRaw < 0.0f ? 0.0f : Lum3(r.enclosedAmbient) * strength * (1.0f - openRaw);
  static const float kN[5][3] = {{0, 1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
  static const float kGrey[3] = {0.96f, 0.99f, 1.04f};
  const float greyLum = Lum3(kGrey);
  const float ov = std::clamp(s.ambOvercast, 0.0f, 1.0f);
  float sum = 0.0f;
  for (const auto& n : kN) {
    const float face = std::abs(n[0]) > 0.5f ? r.faceX : std::abs(n[2]) > 0.5f ? r.faceZ : 1.0f;
    const float up = n[1] * 0.5f + 0.5f;
    float amb[3];
    for (int k = 0; k < 3; k++) amb[k] = s.ambGround[k] + (s.ambSky[k] - s.ambGround[k]) * up;
    float al = Lum3(amb);
    if (ov > 0.0f) {
      const float g = al * greyLum * (1.0f + (0.72f - 1.0f) * std::clamp(-n[1], 0.0f, 1.0f));
      al += (g - al) * ov;
    }
    const float ndl = n[0] * s.keyDir[0] + n[1] * s.keyDir[1] + n[2] * s.keyDir[2];
    const float lambert =
        std::clamp((ndl + r.diffuseWrap) / (1.0f + r.diffuseWrap), 0.0f, 1.0f) * cap;
    sum += face * (al * openScale + enclosed + keyLum * lambert);
  }
  return sum / 5.0f + glowLum * openScale * r.glowStrength;
}

// common.wgsl tonemapHdr on LUMINANCE (Reinhard with the white point, then
// the output gamma; the bleach is a hue effect and is left out): what the
// screen shows of a white body lit this much, 0..1.
float OnScreen(float lum, const Tuning::Render& r) {
  const float W = std::max(r.exposureWhite, 0.01f);
  const float l = std::max(lum, 0.0f);
  const float mapped = l * (1.0f + l / (W * W)) / (1.0f + l);
  return std::pow(mapped, 1.0f / std::max(1.0f, r.gamma));
}

}  // namespace

// ---------------------------------------------------------------------------
// THE TABLES (OnMaterialsReloaded)
//
// A FOOTSTEP'S LOUDNESS per material is DERIVED, not authored: from the sound
// set its footstep already plays (a material that crunches like dry branches
// is loud to a listener for the same reason it is loud to the player), and
// failing that from its class. Thief's rule -- moss quiet, metal loud -- falls
// out of the sound sets the materials already carry.
//
// WHAT A MATERIAL IS TO LIGHT is the renderer's two answers, copied: whether
// it stops a ray (common.wgsl isRayBlocker: not a micro material; a solid, a
// powder, or an OPAQUE liquid) and how bright an emitting cell of it is
// (sim_glow.wgsl glowBlockSum: the palette colour through burnTint at its
// mean weight, times emission / 255; render.emissiveStrength at the sum), as
// luminance. The burn tint is taken with the tuning of the moment; a material
// reload rebuilds.
void MobSystem::BuildStealthTables(const std::vector<MaterialDef>& mats) {
  matStepNoise_.assign(mats.size(), 1.0f);
  matMetal_.assign(mats.size(), 0);
  matGlowLum_.assign(mats.size(), 0.0f);
  matLightBlock_.assign(mats.size(), 0);
  const Tuning::Render& R = CurrentTuning().render;
  const float tintW = 0.5f * (R.burnTintMin + R.burnTintMax);
  for (size_t i = 0; i < mats.size(); i++) {
    const MaterialDef& m = mats[i];
    const MaterialGpu& g = m.gpu;
    bool metal = false;
    for (const std::string& t : m.tags) metal |= t == "metal";
    matMetal_[i] = metal ? 1 : 0;
    if ((g.flags & kMatFlagMicro) == 0u) {
      if (g.klass == CLASS_SOLID) matLightBlock_[i] = 1;
      else if (g.klass == CLASS_POWDER) matLightBlock_[i] = 2;
      else if (g.klass == CLASS_LIQUID && (g.flags & kMatFlagOpaque) != 0u) matLightBlock_[i] = 1;
    }
    if (g.emission > 0u) {
      // The three palette variants (paletteJitter) in equal shares: the
      // positional jitter spreads a mass of the material over all three.
      const bool tint = (g.flags & kMatFlagBurnTint) != 0u;
      float lum = 0.0f;
      for (uint32_t pc : {g.color0, g.color1, g.color2}) {
        float col[3] = {(float)(pc & 0xFFu) / 255.0f, (float)((pc >> 8) & 0xFFu) / 255.0f,
                        (float)((pc >> 16) & 0xFFu) / 255.0f};
        if (tint)
          for (int k = 0; k < 3; k++) col[k] += (R.burnTintColor[k] - col[k]) * tintW;
        lum += Lum3(col) / 3.0f;
      }
      const float emis = (float)g.emission / 255.0f * (tint ? tintW : 1.0f);
      // render.emissiveStrength is applied where the light is summed (LightAt),
      // so an F5 of it needs no material reload.
      matGlowLum_[i] = lum * emis;
    }
    const std::string& step = m.Sound("footstep");
    float k = 1.0f;
    if (step == "leaf") k = 0.7f;          // grass, moss, leaf litter
    else if (step == "branch") k = 1.5f;   // twigs snap
    else if (step == "metal") k = 1.4f;
    else if (step == "stone") k = 1.1f;
    else if (step == "path") k = 0.9f;     // packed earth
    else if (g.klass == CLASS_POWDER) k = 0.75f;   // sand, snow, ash
    else if (metal) k = 1.4f;
    matStepNoise_[i] = k;
  }
  emitters_.clear();   // their luminance was taken from the old table
}

// ---------------------------------------------------------------------------
// NOISE EVENTS
void MobSystem::PostNoise(const stealth::Noise& n) {
  if (!(n.radius > 0.0f)) return;
  if (noiseNext_.size() >= kMaxNoises) {
    // FULL: the quietest goes, this one only if it is louder. Push order is
    // the tick's own deterministic order, so which one goes is too.
    auto q = std::min_element(noiseNext_.begin(), noiseNext_.end(),
                              [](const stealth::Noise& a, const stealth::Noise& b) {
                                return a.radius < b.radius;
                              });
    noiseRefused_++;
    if (q->radius < n.radius) *q = n;
    return;
  }
  noiseNext_.push_back(n);
}

// ---------------------------------------------------------------------------
// THE LIGHT ON A BODY -- the renderer's body lighting, on the CPU
//
// What the watchers see is what the renderer draws: a white body's on-screen
// brightness at three heights, by the same law microbody.wgsl lights a limb
// with (BodyRadiance above), from the same inputs measured the same way:
//
//   KEY      the tick's sun or moon (SetSkyLight: the renderer's own frame
//            light resolve), through a SHADOW RAY toward it from each height
//            and the renderer's softening law (shadowFromOpaqueHit: a far
//            blocker lifts toward render.shadowLift, a ray that runs out
//            under the ground is a far blocker). A roof, a wall between you
//            and a low sun, a tree's crown: shade, as drawn.
//   SKY      the openness of the GROUND under the body (opennessAtBody: walk
//            down to the first matter, take its up face) by the renderer's
//            five rays -- straight up at weight 2, four at 45 degrees at
//            weight 1, render.opennessReach long. A cave, a room, an alley.
//   GLOW     the glow field (sim_glow.wgsl): each nearby chunk's SOURCE (the
//            mean radiance of its glowing cells times the saturating fill)
//            with the field's (1 - d / glowReach)^2 falloff from the chunk's
//            centre -- and, unlike the renderer's body path, ONLY if a line
//            reaches one of that chunk's glowing cells. A fire behind a wall
//            does not light you.
//   ITSELF   a burning body is a lamp (Exposure::burning, in Visibility).
//
// Then the renderer's tonemap, and the mean of the three heights over
// stealth.lightFull.
float MobSystem::LightAt(World& world, const Vec3& centre, float heightVox,
                         stealth::Stance stance, LightProbe* probe) {
  if (!skyLight_.valid || matLightBlock_.empty()) return 1.0f;
  const stealth::Params& P = stealthP_;
  const Tuning::Render& R = CurrentTuning().render;
  LightCells L(world, matLightBlock_);
  const float k = stance == stealth::Stance::Crouch ? 0.65f
                  : stance == stealth::Stance::Prone ? 0.3f
                                                     : 1.0f;
  const float foot = centre.y - heightVox * 0.5f;
  const float h = heightVox * k;
  const int cx = ifloor(centre.x), cz = ifloor(centre.z);

  // ---- SKY: the ground under the body, and the five rays over it ----
  float openRaw = -1.0f;
  for (int dy = 0; dy <= kGroundWalk; dy++) {
    const int y = ifloor(foot) - dy;
    const int b = L.At(cx, y, cz);
    if (b == 1 || (b < 0 && L.Buried(cx, y, cz))) {
      const Vec3 o{(float)cx + 0.5f, (float)y + 1.05f, (float)cz + 0.5f};
      const float reach = std::max(1.0f, R.opennessReach / kVoxelMeters);
      constexpr float kD = 0.70710678f;
      static const float kRays[5][4] = {{0, 1, 0, 2}, {kD, kD, 0, 1}, {-kD, kD, 0, 1},
                                        {0, kD, kD, 1}, {0, kD, -kD, 1}};
      float w = 0.0f;
      for (const auto& rr : kRays) {
        const LightRay ray = March(L, o, Vec3{rr[0], rr[1], rr[2]}, reach);
        if (!ray.hit && !ray.buriedEnd) w += rr[3];
      }
      openRaw = w / 6.0f;
      break;
    }
  }

  // ---- GLOW at the chest, behind lines of sight ----
  // The chunk's field weight first (a distance, no words read), so only the
  // handful of chunks whose centre is within render.glowReach is summarised.
  const Vec3 chest{centre.x, foot + h * 0.6f, centre.z};
  float glow = 0.0f;
  int glowChunks = 0, glowSeen = 0;
  {
    const float reach = std::max(R.glowReach / kVoxelMeters, 1.0f);
    const IVec3 c0{ifloor(chest.x) >> 4, ifloor(chest.y) >> 4, ifloor(chest.z) >> 4};
    for (int i = 0; i < 27; i++) {
      const IVec3 wc{c0.x + i % 3 - 1, c0.y + (i / 3) % 3 - 1, c0.z + i / 9 - 1};
      const Vec3 cc{(float)wc.x * 16.0f + 8.0f, (float)wc.y * 16.0f + 8.0f,
                    (float)wc.z * 16.0f + 8.0f};
      const float fw = std::max(0.0f, 1.0f - (chest - cc).len() / reach);
      if (fw <= 0.0f) continue;
      const EmitterSum* es = EmitterOf(world, wc);
      if (es == nullptr || es->lum <= 0.0f) continue;
      glowChunks++;
      bool seen = false;
      for (int s = 0; s < es->nSamples && !seen; s++) {
        const Vec3 tgt{(float)es->samples[s].x + 0.5f, (float)es->samples[s].y + 0.5f,
                       (float)es->samples[s].z + 0.5f};
        const Vec3 v = tgt - chest;
        const float len = v.len();
        // Stop a cell short: the glowing cell itself is matter.
        seen = len < 1.5f || !March(L, chest, v * (1.0f / len), len - 1.0f).hit;
      }
      if (!seen) continue;
      glowSeen++;
      glow += es->lum * fw * fw;
    }
    glow *= R.emissiveStrength;
  }

  // ---- KEY: a shadow ray from each height ----
  const uint32_t rain = weatherRain_ & kRainAmountMask;
  const float keyScale = 1.0f - P.rainKeyDim * std::clamp((float)rain / 128.0f, 0.0f, 1.0f);
  const Vec3 keyDir{skyLight_.keyDir[0], skyLight_.keyDir[1], skyLight_.keyDir[2]};
  const float heights[3] = {0.92f, 0.6f, 0.25f};
  float screen = 0.0f;
  for (int hi = 0; hi < 3; hi++) {
    const Vec3 p{centre.x, foot + h * heights[hi], centre.z};
    const LightRay ray = March(L, p + keyDir * 0.75f, keyDir, kShadowReach);
    float sh = 1.0f;
    if (ray.hit)
      sh = std::clamp(Smooth(R.shadowSoftNear, R.shadowSoftFar, (ray.t + 0.75f) * kVoxelMeters) *
                          R.shadowLift,
                      0.0f, 1.0f);
    else if (ray.buriedEnd)
      sh = R.shadowLift;
    if (probe != nullptr) probe->shadow[hi] = sh;
    screen += OnScreen(BodyRadiance(skyLight_, R, sh, openRaw, glow, keyScale), R);
  }
  screen /= 3.0f;
  if (probe != nullptr) {
    probe->openRaw = openRaw;
    probe->glow = glow;
    probe->glowChunks = glowChunks;
    probe->glowSeen = glowSeen;
    probe->screen = screen;
  }
  return std::clamp(screen / std::max(P.lightFull, 0.01f), 0.0f, 1.0f);
}

// THE GLOW FIELD'S SOURCE FOR ONE CHUNK, from the freshest words the CPU has
// (the tick's snapshot mirror, else the fetch cache): the mean luminance of
// its glowing cells times the saturating fill (sim_glow.wgsl glowStoreSrc:
// render.glowFill, saturating at 1, so a lava lake is no brighter than one
// lava voxel's surface), and one glowing cell per octant for the line of
// sight -- an EXPOSED one (a face on a cell light passes, or on the chunk's
// edge) where the octant has one: the bottom of a lava pool is lava under
// lava, and a line to it is stopped by the pool's own surface.
const MobSystem::EmitterSum* MobSystem::EmitterOf(World& world, IVec3 chunk) {
  if (!world.ChunkInWindow(chunk)) return nullptr;
  const uint32_t* words = nullptr;
  bool fromMirror = false;
  uint32_t version = 0;
  const WorldSnapshot& snap = world.Snap();
  if (snap.valid && snap.mirror.size() == 27u * kChunkVol) {
    const int mx = chunk.x - snap.mirrorBase.x, my = chunk.y - snap.mirrorBase.y,
              mz = chunk.z - snap.mirrorBase.z;
    if (mx >= 0 && my >= 0 && mz >= 0 && mx < 3 && my < 3 && mz < 3) {
      words = snap.mirror.data() + (size_t)((mz * 3 + my) * 3 + mx) * kChunkVol;
      fromMirror = true;
      version = snap.tick;
    }
  }
  if (words == nullptr) {
    const CachedChunk* cc = world.Cached(chunk);
    if (cc == nullptr || cc->voxels.size() != kChunkVol) return nullptr;
    words = cc->voxels.data();
    version = cc->version;
  }
  const uint64_t key = World::PackChunkKey(chunk);
  auto it = emitters_.find(key);
  if (it != emitters_.end() && it->second.fromMirror == fromMirror &&
      it->second.version == version)
    return &it->second;
  // Bounded like the cache it shadows (rule 2): past 4096 summaries the whole
  // table goes and refills from what is asked next.
  if (it == emitters_.end() && emitters_.size() >= 4096) emitters_.clear();
  auto blocks = [&](uint32_t w) {
    const uint32_t m = w & 0xFFFu;
    if (m == 0 || m >= matLightBlock_.size()) return false;
    const uint8_t b = matLightBlock_[m];
    return b == 2 ? PowderMassOfState((w >> 12) & 0xFu) >= kPowderBlockMin : b == 1;
  };
  // 2 = a face on a cell IN THIS CHUNK that light passes (known exposed),
  // 1 = a face on the chunk's edge only (the neighbour is another chunk's,
  // unread: maybe), 0 = buried in its own kind.
  auto exposure = [&](int x, int y, int z) {
    static const int kF[6][3] = {{0, 1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}};
    int best = 0;
    for (const auto& f : kF) {
      const int nx = x + f[0], ny = y + f[1], nz = z + f[2];
      if (nx < 0 || ny < 0 || nz < 0 || nx >= (int)kChunk || ny >= (int)kChunk ||
          nz >= (int)kChunk) {
        best = 1;
        continue;
      }
      if (!blocks(words[((uint32_t)nz * kChunk + (uint32_t)ny) * kChunk + (uint32_t)nx]))
        return 2;
    }
    return best;
  };
  EmitterSum es;
  es.fromMirror = fromMirror;
  es.version = version;
  float sum = 0.0f;
  uint32_t count = 0;
  int8_t octant[8];   // sample slot per octant; -1 none yet
  int octRank[8] = {};   // the exposure of the octant's sample so far
  for (int8_t& o : octant) o = -1;
  for (uint32_t z = 0; z < kChunk; z++)
    for (uint32_t y = 0; y < kChunk; y++)
      for (uint32_t x = 0; x < kChunk; x++) {
        const uint32_t m = words[(z * kChunk + y) * kChunk + x] & 0xFFFu;
        if (m == 0 || m >= matGlowLum_.size()) continue;
        const float gl = matGlowLum_[m];
        if (gl <= 0.0f) continue;
        sum += gl;
        count++;
        const int o = (x >= 8 ? 1 : 0) | (y >= 8 ? 2 : 0) | (z >= 8 ? 4 : 0);
        if (octant[o] >= 0 && octRank[o] == 2) continue;
        const int rank = exposure((int)x, (int)y, (int)z);
        if (octant[o] >= 0 && rank <= octRank[o]) continue;
        if (octant[o] < 0) octant[o] = (int8_t)es.nSamples++;
        octRank[o] = rank;
        es.samples[octant[o]] = IVec3{chunk.x * (int)kChunk + (int)x,
                                      chunk.y * (int)kChunk + (int)y,
                                      chunk.z * (int)kChunk + (int)z};
      }
  if (count > 0) {
    const float fill = std::clamp(
        (float)count / (float)kChunkVol * CurrentTuning().render.glowFill, 0.0f, 1.0f);
    es.lum = sum / (float)count * fill;
  }
  EmitterSum& slot = emitters_[key];
  slot = es;
  return &slot;
}

// ---------------------------------------------------------------------------
// NPC FOOTSTEPS (presentation: the frame loop voices them)
//
// How loud, against a plain walk at the same speed: the stealth model's own
// terms -- the stance, the sneak skill, worn metal -- so a stalker creeping in
// sounds as quiet to the player as it is to the creature it is stalking. The
// speed term is the footstep cue's own (Cues::Footstep), not repeated here.
void MobSystem::QueueNpcSteps(Mob& m) {
  if (m.footfalls_.empty()) return;
  const stealth::Params& P = stealthP_;
  const ai::Profile* pr = behaviors_.At(m.ai_.profile);
  float sneak = 0.0f;
  if (m.def_ != nullptr && m.def_->stealthSneak >= 0.0f) sneak = m.def_->stealthSneak;
  else if (pr != nullptr && pr->stealth.enabled) sneak = pr->stealth.sneak;
  const float stanceK = m.aiCrouch_ ? P.crouchNoise : 1.0f;
  const float gain = stanceK * (1.0f + P.metalNoise * std::clamp(m.wornMetal_, 0.0f, 1.0f)) *
                     (1.0f - P.sneakNoiseCut * std::clamp(sneak / 100.0f, 0.0f, 1.0f));
  for (const Mob::Footfall& ff : m.footfalls_) {
    if (npcSteps_.size() >= kMaxNpcSteps) {
      npcStepsDropped_++;
      continue;
    }
    NpcStep s;
    s.posVox = ff.posVox;
    s.mat = ff.mat;
    s.speed = ff.speed;
    s.foot = ff.foot;
    s.landing = ff.landing;
    s.fallSpeed = ff.fallSpeed;
    s.gain = gain;
    npcSteps_.push_back(s);
  }
  m.footfalls_.clear();
}

// ---------------------------------------------------------------------------
// WHO IS WHO
uint64_t MobSystem::ActorIdOf(const Mob& m) const {
  for (size_t i = 0; i < avatars_.size(); i++)
    if (avatars_[i] == &m) return ai::kPlayerActorBase + (uint64_t)i;
  return m.id_;
}

// The share of a body's dress that is METAL, 0..1, by PIECE: a piece is
// metal when most of its shells' voxels are, and a full harness is five of
// them (the stock iron set: helm, cuirass, gauntlets, greaves, sabatons).
// By piece rather than by voxel because a breastplate is louder than its
// voxel count says: it clanks against everything else.
float MobSystem::WornMetalOf(const Mob& m) const {
  if (matMetal_.empty()) return 0.0f;
  int pieces = 0;
  for (int p = 0; p < m.WornPieceCount(); p++) {
    uint64_t metal = 0, total = 0;
    for (int slot : m.WornSlotsAt(p)) {
      if (slot < 0 || slot >= (int)m.limbs_.size()) continue;
      const MobLimb& l = m.limbs_[(size_t)slot];
      auto count = [&](uint32_t mat) {
        total++;
        if (mat < matMetal_.size() && matMetal_[mat]) metal++;
      };
      if (l.HasFineSkin())
        for (const PrefabVoxel& v : l.skinVoxels) count(v.material);
      else
        for (const DebrisVoxel& v : l.voxels) count(v.payload & 0xFFFu);
    }
    if (total > 0 && metal * 2 > total) pieces++;
  }
  return std::clamp((float)pieces / 5.0f, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// HOW HIDDEN A BODY IS (PreTick, beside PublishCombatant)
void MobSystem::PublishStealth(World& world, Mob& m, ai::Actor& a, uint32_t tick,
                               const PlayerActorDesc* desc) {
  stealth::Exposure& e = a.stealth;
  const stealth::Params& P = stealthP_;
  // WHO TAKES PART. A player (on foot: the session says so) always does. An
  // NPC does when its profile authors a `stealth` block: stealth is a trait
  // of creatures that have one, in both directions -- a body with no sneak
  // to it (a training dummy, a legacy fixture, a `duelist`) is a plain body
  // every watcher sees at once, exactly as before stealth existed.
  const ai::Profile* pr = behaviors_.At(m.ai_.profile);
  const bool takesPart = desc != nullptr ? desc->stealth
                                         : pr != nullptr && pr->stealth.enabled;
  if (!takesPart) {
    e = stealth::Exposure{};
    return;
  }
  e.modelled = true;
  // THE SKILL: the character's own, else (an NPC) its profile's or (a
  // player) the player default.
  if (m.def_ != nullptr && m.def_->stealthSneak >= 0.0f) e.sneak = m.def_->stealthSneak;
  else if (desc != nullptr) e.sneak = P.playerSneak;
  else e.sneak = pr->stealth.sneak;
  // THE STANCE AND THE WALK.
  float wading = 0.0f;
  if (desc != nullptr) {
    e.stance = desc->stance <= (uint8_t)stealth::Stance::Prone ? (stealth::Stance)desc->stance
                                                              : stealth::Stance::Stand;
    e.moveFrac = std::max(0.0f, desc->moveFrac);
    wading = std::clamp(desc->wading, 0.0f, 1.0f);
  } else {
    e.stance = m.aiCrouch_ ? stealth::Stance::Crouch : stealth::Stance::Stand;
    const float walk = m.def_ != nullptr ? std::max(m.def_->speed, 1.0f) : 40.0f;
    e.moveFrac = std::max(0.0f, m.speedNow_) / walk;
    // A swimmer IS in the water; its noise is its own business (a shark is
    // not splashing). A walker's legs in liquid are.
    const bool swimmer = m.def_ != nullptr && m.def_->swim.present;
    wading = swimmer ? 0.0f : std::clamp(a.immersion * 2.0f, 0.0f, 1.0f);
  }
  // THE LIGHT, on a cadence (a player's every other tick: the meter on the
  // HUD follows a step into shadow).
  const uint32_t every = desc != nullptr ? kLightEveryPlayer : kLightEveryNpc;
  if (!m.stealthLightValid_ || (tick + (uint32_t)m.id_) % every == 0) {
    m.stealthLight_ = LightAt(world, a.centre, a.height, e.stance);
    m.stealthLightValid_ = true;
    m.stealthLightAt_ = tick;
  }
  e.light = m.stealthLight_;
  float hp = 1.0f, burn = 0.0f;
  int lost = 0;
  m.BodyFacts(hp, burn, lost);
  e.burning = burn;
  // THE NOISE: its footsteps (and the clank of its dress).
  if (!m.wornMetalValid_ || (tick + (uint32_t)m.id_) % kMetalEvery == 0) {
    m.wornMetal_ = WornMetalOf(m);
    m.wornMetalValid_ = true;
    m.wornMetalAt_ = tick;
  }
  const float surface = m.lastFootMat_ != 0 && m.lastFootMat_ < matStepNoise_.size()
                            ? matStepNoise_[m.lastFootMat_]
                            : 1.0f;
  e.noise = stealth::MovementNoise(P, e.stance, e.moveFrac, m.wornMetal_, surface,
                                   wading, e.sneak);
  // ...and a LANDING, as an event: a thump heard once.
  if (m.landNoise_ > 0.0f) {
    stealth::Noise n;
    n.pos = n.point = a.centre;
    n.radius = P.landNoise * std::clamp(m.landNoise_ / std::max(1.0f, P.landRefSpeed), 0.0f, 3.0f) *
               (1.0f - P.sneakNoiseCut * std::clamp(e.sneak / 100.0f, 0.0f, 1.0f)) *
               (e.stance == stealth::Stance::Stand ? 1.0f : P.crouchNoise);
    n.source = a.id;
    n.faction = a.faction;
    n.kind = stealth::NoiseKind::Land;
    PostNoise(n);
    m.landNoise_ = 0.0f;
  }
}

const stealth::Exposure& MobSystem::PlayerExposure(size_t i) const {
  static const stealth::Exposure kNone{};
  return i < playerExposure_.size() ? playerExposure_[i] : kNone;
}

// ---------------------------------------------------------------------------
// SNEAK ATTACKS
float MobSystem::SneakAttackMul(const Mob& victim, uint64_t attackerActorId, bool ranged,
                                float itemMul) const {
  if (attackerActorId == 0 || !victim.alive_ || victim.def_ == nullptr) return 1.0f;
  if (victim.PlayerControlled()) return 1.0f;   // a player has no meter to catch
  if (attackerActorId == victim.id_) return 1.0f;
  if (ai::Detects(victim.ai_, behaviors_, attackerActorId)) return 1.0f;
  // THE ATTACKER, as the actor list published it: a body nobody models (a
  // gate's fixture) never sneak attacks, and with sneakNeedsCrouch neither
  // does one standing up.
  const ai::Actor* att = nullptr;
  for (const ai::Actor& a : actors_)
    if (a.id == attackerActorId) {
      att = &a;
      break;
    }
  if (att == nullptr || !att->stealth.modelled) return 1.0f;
  if (stealthP_.sneakNeedsCrouch && att->stealth.stance == stealth::Stance::Stand)
    return 1.0f;
  if (itemMul > 0.0f) return std::max(1.0f, itemMul);
  return std::max(1.0f, ranged ? stealthP_.sneakRanged : stealthP_.sneakMelee);
}

void MobSystem::NoteAttacked(Mob& victim, uint64_t attackerActorId, Vec3 attackerPos,
                             bool ranged, uint32_t tick) {
  if (attackerActorId == 0 || !victim.alive_ || victim.PlayerControlled()) return;
  Vec3 where = attackerPos;
  if (ranged) {
    // IT DID NOT SEE THE ARCHER: it knows roughly which way the arrow came
    // from. Blurred by a fifth of the distance (and a body or so), drawn from
    // (victim, tick) so a replay searches the same spot.
    const Vec3 vc = victim.origin_ + (victim.def_ ? victim.def_->worldSize * 0.5f : Vec3{});
    const float d = (attackerPos - vc).len();
    const float blur = 0.2f * d + 6.0f;
    const uint32_t h = rng::Hash3((uint32_t)victim.id_ ^ 0x5E4A7u, tick, 9u);
    where.x += blur * rng::SignedUnit(h);
    where.z += blur * rng::SignedUnit(h >> 16);
  }
  ai::NoteAttacked(victim.ai_, behaviors_, attackerActorId, where, ranged, tick, stealthP_);
}

// ---------------------------------------------------------------------------
// THE HUD'S REPORT
MobSystem::StealthReport MobSystem::StealthOf(uint64_t actorId) const {
  StealthReport r;
  const float caution = stealthP_.cautionAt;
  for (const Mob& m : mobs_) {
    if (!m.alive_ || m.def_ == nullptr || m.PlayerControlled()) continue;
    const ai::Brain& b = m.ai_;
    float level = ai::AwarenessOf(b, actorId);
    const bool fighting = b.hasTarget && b.targetId == actorId;
    if (fighting) {
      r.fighting++;
      level = std::max(level, b.visible ? 1.0f : level);
    }
    if (level <= 0.0f) continue;
    r.maxLevel = std::max(r.maxLevel, level);
    if (level < caution && !fighting) continue;
    r.watchers++;
    if ((int)b.alert > (int)r.alert) r.alert = b.alert;
    StealthWatcher w;
    w.pos = m.origin_ + m.def_->worldSize * 0.5f;
    w.level = level;
    w.alert = fighting ? stealth::Alert::Combat : b.alert;
    // The strongest few, kept sorted.
    int at = r.topN;
    while (at > 0 && r.top[at - 1].level < w.level) at--;
    if (at < StealthReport::kTop) {
      for (int k = std::min(r.topN, StealthReport::kTop - 1); k > at; k--) r.top[k] = r.top[k - 1];
      r.top[at] = w;
      r.topN = std::min(r.topN + 1, StealthReport::kTop);
    }
  }
  return r;
}
