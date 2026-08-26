#include "sim/windprim.h"

#include <algorithm>
#include <cmath>

namespace {

// Round-half-away-from-zero into Q16.16, written out by hand rather than taken
// from std::lround for the reason sim/wind.h gives at its own quantiser: the
// rounding MODE is part of the value the sim sees, and one written here cannot
// be changed by a compiler flag.
int32_t Q16(double v) {
  double x = v * 65536.0;
  double r = x >= 0.0 ? (x + 0.5) : (x - 0.5);
  if (r > 2147483000.0) r = 2147483000.0;
  if (r < -2147483000.0) r = -2147483000.0;
  return (int32_t)r;
}

int32_t ClampI(int32_t v, int32_t lo, int32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Chunks a primitive's footprint spans, from its extents alone. Position does
// not enter: a moving primitive's box TRANSLATES, it does not grow, which is
// what lets Spawn decide once and for all whether this primitive is allowed to
// carry the entrainment licence.
uint64_t FootprintChunks(const WindPrim& p) {
  const int32_t ext = std::max(p.radius, p.kind == kWindPrimBurst ? 0 : p.reach);
  // +2 chunks: one because the box is inclusive and unaligned, one because the
  // page table dilates op targets by a ring anyway.
  const uint64_t n = (uint64_t)((2 * ext) / (int)kChunk + 3);
  return n * n * n;
}

}  // namespace

void WindPrimAim(WindPrim& p, Vec3 dir, float speedMs) {
  float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
  if (!(len > 1e-6f)) {
    // A fan pointing nowhere is an authoring mistake, and normalising zero
    // would put a NaN axis into every sample inside its radius — which does
    // not look like a bad direction, it looks like the wind system is broken.
    dir = Vec3{0.0f, 1.0f, 0.0f};
    len = 1.0f;
  }
  p.dirX = Q16(dir.x / len);
  p.dirY = Q16(dir.y / len);
  p.dirZ = Q16(dir.z / len);
  // m/s -> world cells/s, the same single conversion WindWeather performs, so
  // "8 m/s" means the same thing whether it came from the weather or a fan.
  const float cells = speedMs / kVoxelMeters;
  p.strengthQ = ClampI(Q16(cells), -kWindPrimMaxSpeed * 65536,
                       kWindPrimMaxSpeed * 65536);
}

int32_t WindPrimEnvelope(const WindPrim& p, uint32_t tick) {
  if (tick < p.spawnTick) return 0;
  const uint32_t age = tick - p.spawnTick;
  const bool forever = (p.ttl == kWindPrimForever);
  if (!forever && age >= p.ttl) return 0;

  // Attack and release are clamped to a quarter of the life each, so a 6-tick
  // gust still has a rise and a fall rather than being all attack.
  const uint32_t quarter = forever ? kWindPrimAttack : (p.ttl / 4 + 1);
  const uint32_t attack = std::min(kWindPrimAttack, quarter);
  int32_t env = 1024;
  if (attack > 0 && age < attack)
    env = (int32_t)((age * 1024u) / attack);
  if (!forever) {
    const uint32_t release = std::min(kWindPrimRelease, quarter);
    const uint32_t left = p.ttl - age;
    if (release > 0 && left < release)
      env = std::min(env, (int32_t)((left * 1024u) / release));
  }
  return ClampI(env, 0, 1024);
}

WindPrimGpu WindPrimResolve(const WindPrim& p, uint32_t tick) {
  WindPrimGpu g{};
  const uint32_t age = (tick >= p.spawnTick) ? (tick - p.spawnTick) : 0u;

  // POSITION IS ANALYTIC IN TIME (research doc §4.3): origin + vel * age, not
  // an accumulator the CPU steps. That is what makes a primitive pure input
  // data — a replay that re-issues the spawn op reproduces the whole flight,
  // and there is no per-tick mutation to get wrong.
  //
  // The multiply is done in 64 bits and shifted once: vel is Q16.16 cells per
  // tick and age is unbounded for a fan, so vel * age in i32 wraps after ~9
  // minutes of a slow drift and the fan jumps to the other side of the world.
  auto travel = [&](int32_t v) -> int32_t {
    const int64_t d = ((int64_t)v * (int64_t)age) >> 16;
    return (int32_t)ClampI((int32_t)std::clamp<int64_t>(d, -(1 << 24), 1 << 24),
                           -(1 << 24), 1 << 24);
  };
  const int32_t px = p.x + travel(p.velX);
  const int32_t py = p.y + travel(p.velY);
  const int32_t pz = p.z + travel(p.velZ);

  const int32_t env = WindPrimEnvelope(p, tick);
  // Strength carries the envelope already. Dividing before multiplying keeps a
  // storm-strength primitive (Q16.16 of 400 cells/s = 2^24.6) inside i32.
  const int32_t sQ = (p.strengthQ / 1024) * env;

  const int32_t radius = ClampI(p.radius, 1, kWindPrimMaxExtent);
  const int32_t reach = ClampI(p.reach, 1, kWindPrimMaxExtent);

  g.w[0] = px;
  g.w[1] = py;
  g.w[2] = pz;
  // kind in bits 0..3, flags in bits 4..15 — WPRIM_KIND_MASK / WPRIM_F_SHIFT
  // in common.wgsl.
  g.w[3] = (int32_t)((p.kind & 0xFu) | ((p.flags & 0xFFFu) << 4));
  g.w[4] = p.dirX;
  g.w[5] = p.dirY;
  g.w[6] = p.dirZ;
  g.w[7] = sQ;
  g.w[8] = radius;
  g.w[9] = reach;
  g.w[10] = p.swirlQ;
  g.w[11] = p.riseQ;
  return g;
}

void WindPrimBounds(const WindPrim& p, uint32_t tick, IVec3& lo, IVec3& hi) {
  const WindPrimGpu g = WindPrimResolve(p, tick);
  const int32_t px = g.w[0], py = g.w[1], pz = g.w[2];
  const int32_t radius = g.w[8], reach = g.w[9];

  if (p.kind == kWindPrimCone) {
    // The swept box of the axis segment, fattened by the radius. A 32-cell fan
    // aimed along +X then declares a 32x8x8 box instead of a 64^3 cube, which
    // is the difference between spending the wake budget on the surface it is
    // pointed at and spending it on sky.
    const int32_t tx = px + (int32_t)(((int64_t)g.w[4] * reach) >> 16);
    const int32_t ty = py + (int32_t)(((int64_t)g.w[5] * reach) >> 16);
    const int32_t tz = pz + (int32_t)(((int64_t)g.w[6] * reach) >> 16);
    lo = {std::min(px, tx) - radius, std::min(py, ty) - radius,
          std::min(pz, tz) - radius};
    hi = {std::max(px, tx) + radius, std::max(py, ty) + radius,
          std::max(pz, tz) + radius};
    return;
  }
  if (p.kind == kWindPrimVortex) {
    // A cylinder about the axis: radius across, reach along, both ways (a
    // tornado's column extends up from where it was placed and down to the
    // ground, and which end the author anchored is not this file's business).
    const int32_t ax = (int32_t)std::abs(((int64_t)g.w[4] * reach) >> 16);
    const int32_t ay = (int32_t)std::abs(((int64_t)g.w[5] * reach) >> 16);
    const int32_t az = (int32_t)std::abs(((int64_t)g.w[6] * reach) >> 16);
    lo = {px - radius - ax, py - radius - ay, pz - radius - az};
    hi = {px + radius + ax, py + radius + ay, pz + radius + az};
    return;
  }
  lo = {px - radius, py - radius, pz - radius};
  hi = {px + radius, py + radius, pz + radius};
}

bool WindPrimSystem::Spawn(const WindPrim& in) {
  // Budget charged BEFORE emission (CLAUDE.md): a caller that overruns learns
  // its gust did not happen rather than silently evicting someone's fan.
  if (live_.size() >= kWindPrimCap) return false;

  WindPrim p = in;
  p.radius = ClampI(p.radius, 1, kWindPrimMaxExtent);
  p.reach = ClampI(p.reach, 1, kWindPrimMaxExtent);
  p.strengthQ = ClampI(p.strengthQ, -kWindPrimMaxSpeed * 65536,
                       kWindPrimMaxSpeed * 65536);
  p.swirlQ = ClampI(p.swirlQ, -4 * 65536, 4 * 65536);
  p.riseQ = ClampI(p.riseQ, -4 * 65536, 4 * 65536);
  if ((p.flags & (kWindPrimAir | kWindPrimWater)) == 0) p.flags |= kWindPrimAir;

  // THE ENTRAINMENT LICENCE IS BOUNDED AT SPAWN, not trimmed at wake time.
  // A primitive that carries it must dirty-mark its whole footprint every tick
  // it lives, and a footprint of a hundred thousand chunks is a rule-2
  // violation no per-tick budget can rescue — trimming it would just make
  // entrainment work in an arbitrary corner of the blast. So the licence is
  // refused outright and the primitive still blows; it simply cannot pick
  // settled matter up. The extents are constant for life, so deciding once
  // here is deciding for good.
  if ((p.flags & kWindPrimEntrain) != 0 &&
      FootprintChunks(p) > kWindWakeMaxChunks) {
    p.flags &= ~kWindPrimEntrain;
  }

  live_.push_back(p);
  return true;
}

void WindPrimSystem::RetireOwner(uint64_t ownerId) {
  if (ownerId == 0) return;
  live_.erase(std::remove_if(live_.begin(), live_.end(),
                             [ownerId](const WindPrim& p) {
                               return p.ownerId == ownerId;
                             }),
              live_.end());
}

void WindPrimSystem::Clear() {
  live_.clear();
  resolved_.clear();
  lo_ = {0, 0, 0};
  hi_ = {-1, -1, -1};
}

void WindPrimSystem::Tick(uint32_t tick) {
  tick_ = tick;
  // Expire first, so a primitive whose last tick was the previous one is gone
  // before anything can sample its zero-strength envelope.
  live_.erase(std::remove_if(live_.begin(), live_.end(),
                             [tick](const WindPrim& p) {
                               if (p.ttl == kWindPrimForever) return false;
                               return tick >= p.spawnTick + p.ttl;
                             }),
              live_.end());

  resolved_.clear();
  // EMPTY BOX CONVENTION, the fluid render AABB's: lo > hi on every axis means
  // "no primitives anywhere" and every sample rejects before the loop. With no
  // primitives the shader's early-out is exact and the world hash cannot move.
  lo_ = {1, 1, 1};
  hi_ = {0, 0, 0};
  if (live_.empty()) return;

  bool any = false;
  for (const WindPrim& p : live_) {
    resolved_.push_back(WindPrimResolve(p, tick));
    IVec3 a{}, b{};
    WindPrimBounds(p, tick, a, b);
    if (!any) {
      lo_ = a;
      hi_ = b;
      any = true;
    } else {
      lo_ = {std::min(lo_.x, a.x), std::min(lo_.y, a.y), std::min(lo_.z, a.z)};
      hi_ = {std::max(hi_.x, b.x), std::max(hi_.y, b.y), std::max(hi_.z, b.z)};
    }
  }
}

void WindPrimSystem::BuildWake(const World& world,
                               const std::vector<uint32_t>& occupancy,
                               uint32_t budget,
                               std::vector<uint32_t>& out) const {
  out.clear();
  wakeRefused_ = 0;
  wakeLast_ = 0;
  if (live_.empty()) return;
  if (budget > kWindWakeCap) budget = kWindWakeCap;

  // Dedup by slot. A bitset over the window's chunk slots costs 4 KiB of stack
  // and one clear; the alternative (sorting the output) would make the wake
  // order depend on the sort, and the wake list is an input to the sim.
  static constexpr size_t kWords = (size_t)kNumChunks / 64;
  std::vector<uint64_t> seen(kWords, 0ull);

  const bool haveOcc = occupancy.size() >= (size_t)kNumChunks;

  for (const WindPrim& p : live_) {
    if ((p.flags & kWindPrimEntrain) == 0) continue;  // no licence, no wake
    IVec3 lo{}, hi{};
    // `tick_` is the tick the resolved list was built at, so the box a
    // travelling gust declares is the box it currently occupies rather than
    // the one it was spawned in. Tick() is the only thing that advances the
    // system, so the two cannot describe different instants.
    WindPrimBounds(p, tick_, lo, hi);

    for (int cz = lo.z >> 4; cz <= (hi.z >> 4); cz++) {
      for (int cy = lo.y >> 4; cy <= (hi.y >> 4); cy++) {
        for (int cx = lo.x >> 4; cx <= (hi.x >> 4); cx++) {
          const IVec3 wc{cx, cy, cz};
          if (!world.ChunkInWindow(wc)) continue;  // out of window is inert
          const uint32_t slot = World::SlotChunkIndex(wc);
          // THE OCCUPANCY FILTER. Entrainment moves matter that is already
          // there, so a chunk of pure air has nothing to pick up and waking it
          // is a dispatch that provably does nothing. This is what turns a
          // fan's footprint from "a cube of sky" into "the surface it is aimed
          // at", and it is why the budget below is rarely reached.
          //
          // The snapshot is one tick latent, which is the safe direction: a
          // chunk that just gained matter is already dirty from the op that
          // put it there.
          if (haveOcc && occupancy[slot] == 0) continue;
          const uint64_t bit = 1ull << (slot & 63u);
          if (seen[slot >> 6] & bit) continue;
          if (out.size() >= budget) {
            wakeRefused_++;
            continue;
          }
          seen[slot >> 6] |= bit;
          out.push_back(slot);
          wakeLast_ = (uint32_t)out.size();
        }
      }
    }
  }
}

WindPrimSystem& WindPrims() {
  // Function-local static, like Celestial()'s: constructed on first use, so
  // there is no static-initialisation-order question between this and the
  // tuning tables a primitive's defaults are read from.
  static WindPrimSystem sys;
  return sys;
}
