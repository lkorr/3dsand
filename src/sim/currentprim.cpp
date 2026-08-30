#include "sim/currentprim.h"

#include <algorithm>
#include <cmath>

#include "sim/rng.h"
#include "sim/tuning.h"
#include "sim/waterbody.h"

namespace sandvox {
namespace {

// Round-half-away-from-zero into Q16.16, written out by hand for the reason
// windprim.cpp gives at its own quantiser: the rounding MODE is part of the
// value the sim sees, and one written here cannot be changed by a compiler flag.
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

}  // namespace

void CurrentPrimAim(CurrentPrim& p, Vec3 dir, float speedMs) {
  float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
  if (!(len > 1e-6f)) {
    // A whirlpool with no axis is an authoring mistake, and normalising zero
    // would put a NaN axis into every sample in range — which does not read as
    // a bad direction, it reads as the water system being broken. +Y is what a
    // drain wants anyway.
    dir = Vec3{0.0f, 1.0f, 0.0f};
    len = 1.0f;
  }
  p.dirX = Q16(dir.x / len);
  p.dirY = Q16(dir.y / len);
  p.dirZ = Q16(dir.z / len);
  // m/s -> world cells/s, the same single conversion WindPrimAim performs, so
  // "2 m/s" means the same thing whether it is a breeze or a river.
  const float cells = speedMs / kVoxelMeters;
  p.strengthQ = ClampI(Q16(cells), -kCurrentPrimMaxSpeed * 65536,
                       kCurrentPrimMaxSpeed * 65536);
}

float CurrentGammaToCoreMs(float gammaM2s, int radiusCells) {
  // v_theta = Gamma / (2 pi r), and `strength` is defined at the core radius,
  // which currentPrimEvalF fixes at radius/8. Metres, because Gamma is authored
  // in m^2/s and the conversion to cells happens in CurrentPrimAim.
  const float coreM =
      std::max(1.0f, (float)std::max(radiusCells / 8, 1)) * kVoxelMeters;
  return gammaM2s / (6.2831853f * coreM);
}

void CurrentPrimBounds(const CurrentPrim& p, IVec3& lo, IVec3& hi) {
  // A sink or a source is a ball of `radius`; a vortex and a stream also reach
  // `reach` along their axis. Tighter than pos +- max(radius, reach) for the
  // radial kinds, which is what the shader's whole-loop reject is spent on.
  const int32_t r = ClampI(p.radius, 1, kCurrentPrimMaxExtent);
  const bool axial = (p.kind == kCurrentPrimVortex ||
                      p.kind == kCurrentPrimStream);
  const int32_t l = axial ? ClampI(p.reach, 1, kCurrentPrimMaxExtent) : 0;
  // The axis is Q16.16 and may point any way, so the axial reach is added on
  // every component it has a share of. Bounding it by |dir| per axis rather
  // than by `l` on all three keeps a horizontal river's box flat.
  const int32_t ax = (int32_t)(((int64_t)std::abs(p.dirX) * l) >> 16);
  const int32_t ay = (int32_t)(((int64_t)std::abs(p.dirY) * l) >> 16);
  const int32_t az = (int32_t)(((int64_t)std::abs(p.dirZ) * l) >> 16);
  lo = {p.x - r - ax, p.y - r - ay, p.z - r - az};
  hi = {p.x + r + ax, p.y + r + ay, p.z + r + az};
}

int32_t CurrentPrimEnvelope(const CurrentPrim& p, uint32_t tick) {
  if (tick < p.spawnTick) return 0;
  // ATTACK, from spawnTick. Measured from the spawn rather than from the last
  // sighting so that a seeder refreshing a live primitive every tick does not
  // restart the ramp — which would pin the envelope at its first step forever
  // and make a drain's whirlpool permanently one sixth of its strength.
  int32_t env = 1024;
  const uint32_t age = tick - p.spawnTick;
  if (kCurrentPrimAttack > 0 && age < kCurrentPrimAttack)
    env = (int32_t)((age * 1024) / kCurrentPrimAttack);
  // RELEASE, from seenTick. This is the plan's "Gamma must decay when flow
  // stops": while the seeder keeps asserting the primitive, `since` is 0 and
  // the release term is 1; the tick the drain stops, the swirl starts winding
  // down and reaches nothing `decayTicks` later.
  if (p.decayTicks != kCurrentPrimForever) {
    const uint32_t since = tick > p.seenTick ? tick - p.seenTick : 0u;
    const uint32_t d = std::max(p.decayTicks, 1u);
    if (since >= d) return 0;
    env = std::min(env, (int32_t)(((d - since) * 1024) / d));
  }
  return ClampI(env, 0, 1024);
}

CurrentPrimGpu CurrentPrimResolve(const CurrentPrim& p, uint32_t tick) {
  CurrentPrimGpu g;
  const int32_t env = CurrentPrimEnvelope(p, tick);
  g.w[0] = p.x;
  g.w[1] = p.y;
  g.w[2] = p.z;
  g.w[3] = (int32_t)((p.kind & 0xFu) | ((p.flags & 0xFu) << 4));
  g.w[4] = p.dirX;
  g.w[5] = p.dirY;
  g.w[6] = p.dirZ;
  // The envelope is applied HERE, once, so the shader never has to know when a
  // primitive started — the same reason WindPrimResolve does it.
  g.w[7] = (int32_t)(((int64_t)p.strengthQ * env) >> 10);
  g.w[8] = ClampI(p.radius, 1, kCurrentPrimMaxExtent);
  g.w[9] = ClampI(p.reach, 1, kCurrentPrimMaxExtent);
  g.w[10] = p.swirlQ;
  g.w[11] = p.riseQ;
  (void)tick;
  return g;
}

bool CurrentPrimSystem::Spawn(const CurrentPrim& p) {
  // REFRESH IN PLACE when an owner is already flowing. A per-tick seeder calls
  // this thirty times a second for the same whirlpool, and appending each time
  // would blow the cap in one second and restart the attack every tick.
  if (p.ownerId != 0) {
    for (CurrentPrim& q : live_) {
      if (q.ownerId != p.ownerId) continue;
      const uint32_t spawn = q.spawnTick;   // keep the attack running
      q = p;
      q.spawnTick = spawn;
      return true;
    }
  }
  if (live_.size() >= kCurrentPrimCap) {
    dropped_++;
    return false;
  }
  live_.push_back(p);
  return true;
}

void CurrentPrimSystem::RetireOwner(uint64_t ownerId) {
  if (ownerId == 0) return;
  live_.erase(std::remove_if(live_.begin(), live_.end(),
                             [&](const CurrentPrim& p) {
                               return p.ownerId == ownerId;
                             }),
              live_.end());
}

void CurrentPrimSystem::Clear() {
  live_.clear();
  resolved_.clear();
  lo_ = {1, 1, 1};
  hi_ = {0, 0, 0};
  dropped_ = 0;
  streamOrigin_ = {1 << 30, 1 << 30, 1 << 30};
}

void CurrentPrimSystem::Tick(uint32_t tick) {
  tick_ = tick;
  // Drop everything whose envelope has reached zero. Expiry is a tick
  // comparison, never a timer, so a replay reproduces the list exactly.
  live_.erase(std::remove_if(live_.begin(), live_.end(),
                             [&](const CurrentPrim& p) {
                               return CurrentPrimEnvelope(p, tick) <= 0;
                             }),
              live_.end());
  resolved_.clear();
  // `lo > hi` on any axis is the EMPTY convention (the fluid render box's, and
  // the wind primitives'), and it is what makes a world with no currents cost
  // exactly one compare per sample.
  IVec3 lo{1, 1, 1}, hi{0, 0, 0};
  bool any = false;
  for (const CurrentPrim& p : live_) {
    if (resolved_.size() >= kCurrentPrimCap) break;
    resolved_.push_back(CurrentPrimResolve(p, tick));
    IVec3 a, b;
    CurrentPrimBounds(p, a, b);
    if (!any) {
      lo = a;
      hi = b;
      any = true;
    } else {
      lo = {std::min(lo.x, a.x), std::min(lo.y, a.y), std::min(lo.z, a.z)};
      hi = {std::max(hi.x, b.x), std::max(hi.y, b.y), std::max(hi.z, b.z)};
    }
  }
  lo_ = lo;
  hi_ = hi;
}

// ---------------------------------------------------------------------------
// THE STREAM ARM (plan component 8)
//
// Manning/Chezy: v proportional to sqrt(slope * depth), direction from the BED
// gradient. Genuinely independent of components 1-7 — no descriptor is needed
// and this would work in a world where the ledger did not exist.
//
// THE SLOPE TRAP, and it is a repeat of one this repo has already paid for.
// `World::Column::slope` is `Land.slope`, which worldgen.wgsl:550 states is
// `g2` — accumulated through the HILL octave and deliberately not through
// detail or grain, because d(slope)/dcolumn through the grain octave is 96 Q8,
// the whole of a gate's range in ONE column. A current built on the fine
// gradient is per-voxel noise. So the MAGNITUDE comes from that field directly.
//
// The DIRECTION needs a signed gradient, which `slope` (an absolute sum) does
// not carry and which is not exposed — the signed g2 pair lives inside the
// block check_invariants.py token-compares against the shader, and widening it
// would be a change to the mirror rather than to this system. So the direction
// is a CENTRAL DIFFERENCE OF THE GROUND HEIGHT OVER A WIDE BASELINE, which is
// the same low-pass by another route: over +-32 voxels the grain octave (cell 8
// voxels, amplitude 4) can contribute at most 4/64 = 0.06 voxel/voxel to the
// answer, against a hill octave whose gradient is the thing being measured.
// A +-1 difference — the actual trap — would give it 2.0.
void CurrentPrimSystem::SeedStreams(const World& world, uint32_t seed,
                                    uint32_t tick) {
  const IVec3 origin = world.WindowOrigin();
  // Scheduled by the WINDOW, not by convenience: the answer is a pure function
  // of (seed, window, tuning) and cannot change between window moves, so
  // re-probing it every tick would be ~1,600 terrain hashes a tick for a
  // result that is already on the list.
  if (origin.x == streamOrigin_.x && origin.y == streamOrigin_.y &&
      origin.z == streamOrigin_.z && seed == streamSeed_) {
    // Keep the existing stream primitives alive: they are terrain, not events,
    // so their release ramp must never start while the window still holds them.
    for (CurrentPrim& p : live_) {
      if (p.kind == kCurrentPrimStream) p.seenTick = tick;
    }
    return;
  }
  streamOrigin_ = origin;
  streamSeed_ = seed;
  RetireOwner(kStreamOwnerId);

  const Tuning& tun = CurrentTuning();
  const float scale = tun.sim.currentStreamScale;
  if (!(scale > 0.0f)) return;

  // A coarse lattice over the residency window. `kStreamProbeStride` is 64
  // voxels — a stream primitive's own radius, so the probes tile the window
  // without the primitives overlapping into a wall of flow.
  const int base = origin.x * (int)kChunk;
  const int baseZ = origin.z * (int)kChunk;
  const int span = (int)kWorldN;
  int emitted = 0;
  for (int oz = kStreamProbeStride / 2; oz < span; oz += kStreamProbeStride) {
    for (int ox = kStreamProbeStride / 2; ox < span; ox += kStreamProbeStride) {
      if (emitted >= kStreamMaxPrims) return;
      const int x = base + ox, z = baseZ + oz;
      const World::Column c = World::TerrainColumn(x, z, seed);
      if (c.water == INT32_MIN) continue;          // no standing water here
      const int depth = c.water - c.h;             // voxels
      if (depth <= 0) continue;
      // The landform slope, Q8. Below the threshold the water is a pond, not a
      // stream, and a pond has no bed-driven current at all.
      if (c.slope < tun.sim.currentStreamMinSlope) continue;

      // Direction: downhill, from a WIDE central difference (see the note
      // above). Y is dropped — a bed current runs along the bed.
      const int S = kStreamGradBaseline;
      const int hxp = World::TerrainHeight(x + S, z, seed);
      const int hxm = World::TerrainHeight(x - S, z, seed);
      const int hzp = World::TerrainHeight(x, z + S, seed);
      const int hzm = World::TerrainHeight(x, z - S, seed);
      float dx = (float)(hxm - hxp);   // +x is downhill when h falls with x
      float dz = (float)(hzm - hzp);
      const float dl = std::sqrt(dx * dx + dz * dz);
      if (!(dl > 1e-3f)) continue;
      dx /= dl;
      dz /= dl;

      // Chezy: v = C * sqrt(R * S). R ~ depth in metres, S = slope (Q8 -> 1).
      const float slope01 = (float)c.slope / 256.0f;
      const float depthM = (float)depth * kVoxelMeters;
      const float vms = scale * std::sqrt(slope01 * depthM);
      if (!(vms > 0.02f)) continue;

      CurrentPrim p;
      p.kind = kCurrentPrimStream;
      // THE SIM LICENCE: a stream's parameters are a pure function of (seed,
      // window, tuning), so this one is entitled to it.
      p.flags = kCurrentPrimSim;
      p.x = x;
      p.y = c.water;
      p.z = z;
      p.radius = kStreamProbeStride;
      p.reach = kStreamProbeStride;
      p.decayTicks = kCurrentPrimForever;
      p.spawnTick = tick;
      p.seenTick = tick;
      p.ownerId = kStreamOwnerId;
      CurrentPrimAim(p, Vec3{dx, 0.0f, dz}, vms);
      if (!Spawn(p)) return;
      emitted++;
    }
  }
}

// ---------------------------------------------------------------------------
// THE DRAIN ARM (plan components 6 + 8)
//
// While a governed body's drain window is live, a SINK at the hole plus a
// VORTEX about the vertical through it.
//
// WHERE THE HOLE IS, AND WHY IT IS NOT READ BACK. The GPU ledger knows exactly
// where the orifice is (WBS_HOLEKEY) and exactly whether it is flowing
// (WBS_EMIT), and neither is available to the CPU except through a readback
// that arrives on a schedule set by fence retirement. Seeding from that would
// be M1's §1.1 correction 2 all over again — a scheduling-dependent input to a
// field a kernel reads. What the CPU DOES know on the tick stream is the
// MUTATION that made the hole: holes appear when someone digs or explodes, and
// every one of those arrives through the mutation queue. So the sink and the
// vortex sit where the dig was, and they wind down `sim.currentVortexDecay`
// seconds after the digging stops — which is also the honest answer to "when
// does the swirl end", because the CPU genuinely cannot see the last eighth
// leave.
//
// GAMMA AND CHIRALITY FROM hash3 OF THE HOLE POSITION. This is physically
// legitimate rather than a fudge: a real bathtub vortex is not created by the
// drain, it is residual ambient circulation being concentrated as fluid moves
// inward. Gamma is conserved, so v_theta = Gamma/2*pi*r blows up as r shrinks
// — the swirl is an INITIAL CONDITION. Drawing it from the position means not
// every drain in the world spins the same way, which is the giveaway a single
// constant would produce.
void CurrentPrimSystem::SeedDrains(const WaterBodySystem& water, uint32_t seed,
                                   uint32_t tick) {
  const Tuning& tun = CurrentTuning();
  const uint32_t decay =
      (uint32_t)std::max(1, (int)(tun.sim.currentVortexDecay * 30.0f + 0.5f));
  for (const WaterBodyDesc& b : water.Bodies()) {
    if (b.state != WaterBodyState::Proposed) continue;
    const WaterBodyHole h = water.HoleHint(b.basinId);
    if (!h.valid) continue;

    // One hash keys both the strength and the handedness, so a given drain
    // spins the same way every time the world is rebuilt from the same seed.
    const uint32_t r = rng::Hash3(seed ^ 0x5EEDu, (uint32_t)(h.x ^ (h.z << 12)),
                                  (uint32_t)b.basinId);
    // 0.55x .. 1.45x of the authored circulation.
    const float gamma =
        tun.sim.currentVortexGamma * (0.55f + (float)(r & 0xFFu) / 283.0f);
    const float chir = (r & 0x100u) != 0 ? 1.0f : -1.0f;

    const int radius = std::max(8, tun.sim.currentVortexRadius);

    // THE VORTEX — the far-reaching, visible, navigable part.
    {
      CurrentPrim p;
      p.kind = kCurrentPrimVortex;
      p.flags = kCurrentPrimSim;
      p.x = h.x;
      p.y = b.level;
      p.z = h.z;
      p.radius = radius;
      // Axial reach: from the surface down to the hole, plus a margin. A
      // whirlpool is a column, not a ball.
      p.reach = std::max(8, b.level - h.y + 8);
      p.swirlQ = Q16(chir);
      p.riseQ = 0;
      p.decayTicks = decay;
      p.spawnTick = tick;
      p.seenTick = tick;
      p.ownerId = kDrainOwnerBase + b.basinId * 2u;
      CurrentPrimAim(p, Vec3{0.0f, -1.0f, 0.0f},
                     CurrentGammaToCoreMs(gamma, radius));
      if (!Spawn(p)) return;
    }
    // THE SINK — violent, and only a couple of voxels wide at any realistic
    // discharge. That asymmetry against the vortex above is not a compromise,
    // it is why real whirlpools look enormous while the actual suction is a
    // small throat.
    {
      CurrentPrim p;
      p.kind = kCurrentPrimSink;
      p.flags = kCurrentPrimSim;
      p.x = h.x;
      p.y = h.y;
      p.z = h.z;
      p.radius = std::max(4, radius / 3);
      p.reach = p.radius;
      p.decayTicks = decay;
      p.spawnTick = tick;
      p.seenTick = tick;
      p.ownerId = kDrainOwnerBase + b.basinId * 2u + 1u;
      CurrentPrimAim(p, Vec3{0.0f, -1.0f, 0.0f}, tun.sim.currentSinkSpeed);
      if (!Spawn(p)) return;
    }
  }
}

CurrentPrimSystem& CurrentPrims() {
  static CurrentPrimSystem s;
  return s;
}

// ---- component 9: the impact-ripple ring -----------------------------------

void WaveImpactRing::Add(float xVox, float zVox, float timeSec, float ampM) {
  if (!(ampM > 0.0f)) return;
  ring_[next_] = WaveImpact{xVox, zVox, timeSec, ampM};
  next_ = (next_ + 1) % kWaveImpactCap;
  if (count_ < kWaveImpactCap) count_++;
}

void WaveImpactRing::Clear() {
  for (uint32_t i = 0; i < kWaveImpactCap; i++) ring_[i] = WaveImpact{};
  count_ = 0;
  next_ = 0;
}

WaveImpactRing& WaveImpacts() {
  static WaveImpactRing s;
  return s;
}

// ---- the field, on the CPU (see the header's note on the third copy) -------
//
// Transcribed from currentPrimEvalF in assets/shaders/common.wgsl, line for
// line, in the same order and with the same clamps. Read the two side by side
// when changing either.
Vec3 CurrentAtCpu(Vec3 posVox) {
  const CurrentPrimSystem& cp = CurrentPrims();
  const uint32_t n = std::min(cp.Count(), kCurrentPrimCap);
  if (n == 0) return Vec3{0.0f, 0.0f, 0.0f};
  // The union AABB, the same whole-loop reject the shaders take.
  const IVec3 lo = cp.BoundsLo(), hi = cp.BoundsHi();
  const int px = (int)std::floor(posVox.x), py = (int)std::floor(posVox.y),
            pz = (int)std::floor(posVox.z);
  if (px < lo.x || py < lo.y || pz < lo.z || px > hi.x || py > hi.y ||
      pz > hi.z)
    return Vec3{0.0f, 0.0f, 0.0f};

  Vec3 acc{0.0f, 0.0f, 0.0f};
  for (uint32_t i = 0; i < n; i++) {
    const int32_t* w = cp.Resolved()[i].w;
    const uint32_t kind = (uint32_t)w[3] & 0xFu;
    const Vec3 pos{(float)w[0], (float)w[1], (float)w[2]};
    Vec3 dir{(float)w[4] / 65536.0f, (float)w[5] / 65536.0f,
             (float)w[6] / 65536.0f};
    const float s = (float)w[7] / 65536.0f;
    const float rad = std::max((float)w[8], 1.0f);
    const float len = std::max((float)w[9], 1.0f);
    const float swirl = (float)w[10] / 65536.0f;
    const float rise = (float)w[11] / 65536.0f;
    const float core = std::max(rad * 0.125f, 1.0f);

    const Vec3 d{posVox.x - pos.x, posVox.y - pos.y, posVox.z - pos.z};
    const float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
    const float rr = rad * rad;

    if (kind == kCurrentPrimSink || kind == kCurrentPrimSource) {
      if (d2 > rr) continue;
      const float rc = std::max(std::sqrt(d2), core);
      const float edge = 1.0f - d2 / rr;
      const float mag = s * (core * core) / (rc * rc) * edge *
                        (kind == kCurrentPrimSink ? -1.0f : 1.0f);
      acc.x += d.x / rc * mag;
      acc.y += d.y / rc * mag;
      acc.z += d.z / rc * mag;
      continue;
    }

    const float ax = d.x * dir.x + d.y * dir.y + d.z * dir.z;
    const float r2 = std::max(0.0f, d2 - ax * ax);
    if (r2 > rr) continue;
    if (std::abs(ax) > len) continue;
    const float radW = 1.0f - r2 / rr;
    const float axW = 1.0f - std::abs(ax) / len;

    if (kind == kCurrentPrimVortex) {
      const Vec3 perp{d.x - dir.x * ax, d.y - dir.y * ax, d.z - dir.z * ax};
      const float rpc = std::max(std::sqrt(r2), core);
      const Vec3 tang{dir.y * perp.z - dir.z * perp.y,
                      dir.z * perp.x - dir.x * perp.z,
                      dir.x * perp.y - dir.y * perp.x};
      const float vt = s * core / rpc * radW * axW;
      const float lift = rise * s * radW * axW;
      acc.x += tang.x / rpc * (vt * swirl) - perp.x / rpc * (vt * 0.22f) +
               dir.x * lift;
      acc.y += tang.y / rpc * (vt * swirl) - perp.y / rpc * (vt * 0.22f) +
               dir.y * lift;
      acc.z += tang.z / rpc * (vt * swirl) - perp.z / rpc * (vt * 0.22f) +
               dir.z * lift;
      continue;
    }

    // kCurrentPrimStream.
    const float m = s * radW * axW;
    acc.x += dir.x * m;
    acc.y += dir.y * m;
    acc.z += dir.z * m;
  }
  return acc;
}

uint32_t CurrentDebugArrowsPerAxis() {
  const Tuning::Render& r = CurrentTuning().render;
  int half = (int)(r.dbgCurrentRadius / std::max(r.dbgCurrentSpacing, 1.0f));
  if (half < 0) half = 0;
  if (half > 24) half = 24;   // 49^3 = 117,649 arrows is already absurd
  return (uint32_t)(2 * half + 1);
}

uint32_t CurrentDebugArrowCount() {
  const uint32_t n = CurrentDebugArrowsPerAxis();
  return n * n * n;
}

}  // namespace sandvox
