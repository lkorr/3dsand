// A BODY THE WATER CARRIES (MobDef::SwimDef; DESIGN.md "Swimmers").
//
// The locomotion of every creature with a sidecar `swim` block: a shark, a
// shoal of piranha, and whatever swims next. It sits BESIDE the walk drive
// (MobSystem::DriveLocomotion / UpdateFall), at the same seam -- the
// behaviour layer still writes only a desired heading and a drive scale (plus,
// for a body that can leave the floor, an altitude: ai::IntentOut::altitude),
// and nothing here knows which creature it is moving.
//
// THE MODE IS A SCHMITT PAIR ON THE BODY'S OWN WETNESS (SenseWater): sampled
// down its axis off the chunk cache every tick, entered at kSwimEnter, left
// under kSwimLeave, so a fish whose back breaks the surface does not flicker
// between swimming and falling. Entering converts the walker's ballistic
// state into the swim velocity (a fish thrown into a pond keeps its splash),
// leaving converts it back (a breach is a real arc and lands where it lands).
//
// WHAT THE WATER DOES (DriveSwim), in the order it is applied:
//   1. THE HEIGHT IT WANTS becomes a pitch: atan(dy / run), bounded by the
//      waterline and the bed as soft walls a voxel early. (Or the layer names
//      the pitch outright -- IntentOut::pitch, a strike up a 3D line.)
//   2. IT FEELS AHEAD for rock -- and for AIR above the waterline, which to a
//      fish is a wall -- down its wanted line, and if that is blocked tries a
//      fan of nearby lines and takes the first clear one (a projection onto
//      the open set, the walk's Deflect rule in three dimensions).
//   3. STEER: yaw by the shared Steer (turn rate, ramp); pitch by its own
//      rate, here.
//   4. THRUST along the 3D axis toward drive x def.speed (accel up, brake
//      down), FINS bleed sideslip off at `sideGrip`, BUOYANCY cancels gravity
//      in the wet share of the body.
//   5. THE MOVE is resolved against samples down the body's own axis and
//      against other bodies (BlockedByMob), sliding along what refuses it;
//      the waterline is a hard ceiling except for a charge's `breach`.
//   6. IT BANKS into the turn: roll = the coordinated-turn lean
//      atan(v x omega / g), scaled by `bank`. Presentation (PoseInputs).
//
// OUT OF THE WATER (TickStranded): the walker's gravity and ground settle,
// no walking at all, the body eased onto a flank, a flop every
// flopMin..flopMax seconds that throws it toward the nearest water it can
// feel within flopReach (or anywhere, if none), and after breathSeconds the
// breath runs out at suffocateHp a second (DamageCause::Asphyxia: hp, no
// wound).
//
// DETERMINISM: CPU-float gameplay state like the heading and the walk drive
// (rule 1 is about the grid; this writes no voxel). Every draw is
// rng::Hash3 on (id, counter); nothing reads a clock.
#include <algorithm>
#include <climits>
#include <cmath>

#include "game/mob.h"
#include "sim/rng.h"
#include "sim/scale.h"
#include "sim/tuning.h"
#include "sim/world.h"

namespace {

// The Schmitt pair on the body's wet share (Mob::swimming_).
constexpr float kSwimEnter = 0.55f;
constexpr float kSwimLeave = 0.30f;
// The longest piece of one tick's swim move resolved at once, cells (the
// body samples sit ~5 apart down a shark; a body is ~3 across), and the most
// pieces a tick is cut into.
constexpr float kMaxSubstep = 2.0f;
constexpr int kMaxSubsteps = 8;
// How far the waterline / bed scans reach from the body's centre, cells.
constexpr int kColumnScan = 24;
// A stranded body lies this far over on its flank (radians).
constexpr float kFlankRoll = 1.45f;

inline float HalfLifeK(float dt, float hl) {
  return hl > 1e-4f ? 1.0f - std::pow(0.5f, dt / hl) : 1.0f;
}

// THE MIRROR FIRST, THEN ONE CHUNK MEMO over the on-demand fetch cache. The
// 3x3x3 mirror round the player is refreshed every tick, so near the player
// (where a fish is being watched, and where it is hunting him) the answer is
// the water as it is NOW -- a pool dug this second, a lake drained. Past the
// mirror, the fetch cache the walk drive's ground probe reads
// (Mob::GroundHeightAt): a fish forty voxels from the camera must still know
// where the water is. A cache miss asks for the chunk and answers Unknown;
// outside the residency window is Solid, the sim's own rule.
struct CellCache {
  World& world;
  const std::vector<uint32_t>& cls;
  // Ask for a missing chunk? A swimmer's own sense does: it needs the water.
  // A WATCHER's question about somebody else (BodyImmersion, every actor every
  // tick) does not -- it answers from what is already known, and must not put
  // a single request into the one FIFO every ground probe in the world waits
  // on (World::FetchSource: a flood from one consumer is starvation for all).
  bool fetch = true;
  const CachedChunk* cc = nullptr;
  int kx = INT_MIN, ky = INT_MIN, kz = INT_MIN;
  CellKind At(IVec3 c) {
    if (!world.CellInWindow(c)) return CellKind::Solid;
    if (const CellKind m = world.KindAt(c, cls); m != CellKind::Unknown) return m;
    const int x = c.x >> 4, y = c.y >> 4, z = c.z >> 4;
    if (x != kx || y != ky || z != kz) {
      kx = x;
      ky = y;
      kz = z;
      cc = world.Cached({x, y, z});
      if (cc != nullptr && cc->voxels.size() != kChunkVol) cc = nullptr;
      if (cc == nullptr && fetch)
        world.RequestChunkFetch({x, y, z}, World::FetchSource::Mob);
    }
    if (cc == nullptr) return CellKind::Unknown;
    const uint32_t li = ((uint32_t)(c.z & 15) * kChunk + (uint32_t)(c.y & 15)) * kChunk +
                        (uint32_t)(c.x & 15);
    return World::KindOfWord(cc->voxels[li], cls);
  }
  CellKind At(Vec3 p) { return At(IVec3{ifloor(p.x), ifloor(p.y), ifloor(p.z)}); }
};

Vec3 DirOf(float heading, float pitch) {
  const float cp = std::cos(pitch);
  return Vec3{std::sin(heading) * cp, std::sin(pitch), std::cos(heading) * cp};
}

}  // namespace

// ---------------------------------------------------------------------------
// A launch while swimming: a surge (Mob::Launch routes here; the style's
// lunge calls it directly with a 3D direction).
// ---------------------------------------------------------------------------
void Mob::SwimSurge(Vec3 vel) {
  swimVel_ = swimVel_ + vel;
  // Capped at the body's own burst ceiling: a surge is a kick of the tail,
  // not a cannon.
  if (def_ != nullptr) {
    const float cap = def_->speed * std::max(def_->swim.burst, 1.0f) * 1.25f;
    const float m = swimVel_.len();
    if (m > cap && m > 1e-4f) swimVel_ = swimVel_ * (cap / m);
  }
}

const Mob* MobSystem::SwimStrikeTarget(const Mob& mob) const {
  if (!mob.swimming_ || !mob.stroke_.Active()) return nullptr;
  if (mob.stroke_.phase != NpcStroke::Phase::Windup &&
      mob.stroke_.phase != NpcStroke::Phase::Cut)
    return nullptr;
  // FindCombatantById is a lookup; it does not change the system.
  return const_cast<MobSystem*>(this)->FindCombatantById(mob.stroke_.targetId);
}

bool MobSystem::SwimBlockedAt(const Mob& mob, IVec3 cell) const {
  if (world_ == nullptr) return false;
  CellCache cache{*world_, mob.ClassOf()};
  return cache.At(cell) == CellKind::Solid;
}

float MobSystem::BodyImmersion(const Mob& m) const {
  if (world_ == nullptr || m.def_ == nullptr) return 0.0f;
  const Vec3 ws = m.def_->worldSize;
  CellCache cache{*world_, m.ClassOf(), /*fetch=*/false};
  const float x = m.origin_.x + ws.x * 0.5f, z = m.origin_.z + ws.z * 0.5f;
  int wet = 0;
  for (float f : {0.15f, 0.5f, 0.85f})
    if (cache.At(Vec3{x, m.origin_.y + ws.y * f, z}) == CellKind::Liquid) wet++;
  return (float)wet / 3.0f;
}

float MobSystem::PointImmersion(const Mob& m, Vec3 centre) const {
  if (world_ == nullptr) return 0.0f;
  CellCache cache{*world_, m.ClassOf(), /*fetch=*/false};
  int wet = 0;
  for (float dy : {-1.5f, 0.0f, 1.5f})
    if (cache.At(Vec3{centre.x, centre.y + dy, centre.z}) == CellKind::Liquid)
      wet++;
  return (float)wet / 3.0f;
}

// ---------------------------------------------------------------------------
// CARRION: how much meat is left on a corpse (ai::Perception::carrion).
// ---------------------------------------------------------------------------
// Every anatomy limb (worn shells and hair are not food), the fine skin
// lattice where the limb has one (it is the truth there; the coarse lattice
// is its collider), a tombstone (material 0) is a cell already gone. The
// ratio is edible over everything else -- bone, mostly -- so a skeleton reads
// near 0 and a fresh human reads several; a boneless body with meat on it
// reads 1e9.
void MobSystem::RefreshCarrion(Mob& m) const {
  m.carrionLimb_.assign(m.limbs_.size(), 0u);
  uint64_t edible = 0, other = 0;
  const size_t nMat = matEdible_.size();
  for (int i = 0; i < m.AppendedBase() && i < (int)m.limbs_.size(); i++) {
    const MobLimb& L = m.limbs_[(size_t)i];
    if (L.body == 0 || m.IsBloodless(i)) continue;
    uint32_t e = 0, o = 0;
    auto count = [&](uint32_t mat) {
      if (mat == 0) return;
      if (mat < nMat && matEdible_[mat] != 0)
        e++;
      else
        o++;
    };
    if (L.HasFineSkin()) {
      for (const PrefabVoxel& v : L.skinVoxels) count(v.material);
    } else {
      for (const DebrisVoxel& v : L.voxels) count(v.payload & 0xFFFu);
    }
    m.carrionLimb_[(size_t)i] = e;
    edible += e;
    other += o;
  }
  m.fleshRatio_ = edible == 0  ? 0.0f
                  : other == 0 ? 1e9f
                               : (float)((double)edible / (double)other);
  m.carrionKnown_ = true;
}

// ---------------------------------------------------------------------------
// SenseWater: the wet share, the mode, and the waterline / bed over the body.
// ---------------------------------------------------------------------------
void MobSystem::SenseWater(Mob& mob, const MobDef& def, World& world) {
  if (!def.swim.present) return;
  const Vec3 ws = def.worldSize;
  const Vec3 c = mob.origin_ + ws * 0.5f;
  const Vec3 f = mob.Forward3();
  const float halfLen = ws.z * 0.5f;
  CellCache cache{world, mob.ClassOf()};
  // Four samples down the axis and one on the back: a body with its dorsal
  // fin out is still swimming, one with half its length on the bank is not.
  int wet = 0, known = 0;
  auto sample = [&](Vec3 p) {
    const CellKind k = cache.At(p);
    // Rock is neither wet nor dry: a nose against the bank is still a fish
    // in the water.
    if (k == CellKind::Unknown || k == CellKind::Solid) return;
    known++;
    if (k == CellKind::Liquid) wet++;
  };
  for (float t : {-0.8f, -0.3f, 0.3f, 0.8f}) sample(c + f * (t * halfLen));
  sample(c + Vec3{0.0f, def.swim.trunkHalfH * 0.8f, 0.0f});
  // Nothing known (the column has not been fetched yet): keep what it was.
  if (known > 0) mob.immersion_ = (float)wet / (float)known;

  // ---- THE MODE --------------------------------------------------------------
  if (!mob.swimming_ && mob.immersion_ >= kSwimEnter) {
    // INTO THE WATER: the walker's ballistic state is the entry velocity,
    // and the water takes a share of it at once (the splash).
    mob.swimming_ = true;
    mob.swimVel_ = mob.airborne_
                       ? Vec3{mob.airVel_.x, mob.fallVel_, mob.airVel_.z} * 0.6f
                       : Vec3{};
    mob.airborne_ = false;
    mob.launched_ = false;
    mob.fallVel_ = 0.0f;
    mob.airVel_ = Vec3{};
    mob.airTime_ = 0.0f;
  } else if (mob.swimming_ && mob.immersion_ < kSwimLeave) {
    // OUT OF IT: a breach is a real arc. The latch keeps the first fall tick
    // from landing it on the water it just left (Mob::launched_).
    mob.swimming_ = false;
    mob.airborne_ = true;
    mob.launched_ = true;
    mob.fallVel_ = mob.swimVel_.y;
    mob.airVel_ = Vec3{mob.swimVel_.x, 0.0f, mob.swimVel_.z};
    mob.airTime_ = 0.0f;
    mob.swimVel_ = Vec3{};
    // Which flank it will land on, drawn once per stranding.
    mob.rollSide_ =
        (rng::Hash3((uint32_t)mob.id_ ^ 0xF1A9u, mob.flops_, 3u) & 1u) ? 1.0f : -1.0f;
  }

  // ---- THE WATERLINE AND THE BED, over the centre column -------------------
  // The first cell above that is not liquid (air -- or a cave roof, which is
  // a ceiling all the same) and the first below. Only while swimming: on the
  // bank neither is asked.
  mob.surfaceKnown_ = mob.bedKnown_ = false;
  if (!mob.swimming_) return;
  const int cx = ifloor(c.x), cz = ifloor(c.z), cy = ifloor(c.y);
  for (int y = cy; y <= cy + kColumnScan; y++) {
    const CellKind k = cache.At(IVec3{cx, y, cz});
    if (k == CellKind::Unknown) break;
    if (k != CellKind::Liquid) {
      mob.surfaceKnown_ = true;
      mob.surfaceY_ = (float)y;
      break;
    }
  }
  for (int y = cy - 1; y >= cy - kColumnScan; y--) {
    const CellKind k = cache.At(IVec3{cx, y, cz});
    if (k == CellKind::Unknown) break;
    if (k != CellKind::Liquid) {
      mob.bedKnown_ = true;
      mob.bedY_ = (float)y;
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// DriveSwim: one tick of the water carrying the body. The header has the
// order and why.
// ---------------------------------------------------------------------------
void MobSystem::DriveSwim(Mob& mob, const MobDef& def, World& world, float dt) {
  const MobDef::SwimDef& sw = def.swim;
  const Vec3 ws = def.worldSize;
  const float halfLen = ws.z * 0.5f;
  const float halfH = sw.trunkHalfH;
  const Vec3 c = mob.origin_ + ws * 0.5f;
  const float g = MetresToCells(CurrentTuning().physics.gravity);
  CellCache cache{world, mob.ClassOf()};
  const float speed = mob.swimVel_.len();

  // ---- 1. THE HEIGHT IT WANTS, as a pitch -----------------------------------
  // The run is at least a body length: a nose does not stand on end to rise
  // a voxel.
  float wantPitch = 0.0f;
  if (mob.altitudeSet_) {
    const float dy = mob.altitudeWant_ - c.y;
    wantPitch = std::atan2(dy, std::max(ws.z, speed * 0.6f));
  }
  // ...unless the behaviour layer named the angle itself (a strike up a 3D
  // line, ai::OrbitTuning::climb): that law's long run is right for a body
  // drifting to a depth and flattens a strike to a few degrees at speed.
  if (mob.pitchSet_) wantPitch = mob.pitchWant_;
  // The waterline and the bed are SOFT walls a voxel early: the pitch is
  // bent away before the hard limits in step 5 have to act.
  const float allowUp = mob.breachOk_ ? sw.breach : -sw.surfaceGap;
  if (mob.surfaceKnown_) {
    const float ceil = mob.surfaceY_ + allowUp - halfH;   // the highest centre
    const float over = c.y + 1.0f - ceil;
    if (over > 0.0f) wantPitch = std::min(wantPitch, -std::atan2(over, ws.z));
  }
  if (mob.bedKnown_) {
    const float floor = mob.bedY_ + 1.0f + sw.bedGap + halfH;   // the lowest
    const float under = floor + 1.0f - c.y;
    if (under > 0.0f) wantPitch = std::max(wantPitch, std::atan2(under, ws.z));
  }
  wantPitch = std::clamp(wantPitch, -sw.maxPitch, sw.maxPitch);

  // ---- 2. IT FEELS AHEAD ------------------------------------------------------
  // Down the WANTED line from the snout, `feel` voxels plus a third of a
  // second of travel. Rock is a wall; so is air above the waterline unless
  // the run may breach. Unknown is open (the mirror's edge is not a wall).
  // Blocked: a fan of lines about the wanted one, nearest first, turning the
  // way the body is already turning first so it does not dither between two
  // equal escapes; none clear = turn round.
  {
    const Vec3 snout = c + mob.Forward3() * halfLen;
    // ...no further than a strike's line runs (ai::IntentOut::feelReach):
    // what lies past the point it bites is not in its way.
    float reach = sw.feel + speed * 0.33f;
    if (mob.feelReach_ > 0.0f) reach = std::min(reach, mob.feelReach_);
    auto clear = [&](float h, float p) {
      const Vec3 d = DirOf(h, p);
      for (float s = 1.0f; s <= reach; s += 1.0f) {
        const CellKind k = cache.At(snout + d * s);
        if (k == CellKind::Solid) return false;
        if ((k == CellKind::Air || k == CellKind::Gas) && !mob.breachOk_)
          return false;
      }
      return true;
    };
    const float h0 = mob.desiredHeading_;
    if (reach > 0.5f && !clear(h0, wantPitch)) {
      const float side = mob.turnVel_ >= 0.0f ? 1.0f : -1.0f;
      struct Try { float dh, dp; };
      static constexpr Try kFan[] = {
          {0.5f, 0.0f},  {-0.5f, 0.0f}, {0.0f, 0.45f}, {0.0f, -0.45f},
          {1.0f, 0.0f},  {-1.0f, 0.0f}, {0.6f, 0.4f},  {-0.6f, 0.4f},
          {0.6f, -0.4f}, {-0.6f, -0.4f}, {1.6f, 0.0f}, {-1.6f, 0.0f},
          {0.0f, 0.9f},  {0.0f, -0.9f}, {2.4f, 0.0f}, {-2.4f, 0.0f}};
      bool found = false;
      for (const Try& t : kFan) {
        const float h = h0 + t.dh * side;
        const float p = std::clamp(wantPitch + t.dp, -sw.maxPitch, sw.maxPitch);
        if (!clear(h, p)) continue;
        mob.desiredHeading_ = h;
        wantPitch = p;
        found = true;
        break;
      }
      if (!found) mob.desiredHeading_ = mob.heading_ + 3.14159265f * side;
      mob.swimFelt_++;
    }
  }

  // ---- 3. STEER: the yaw is the walker's own; the pitch is the body's -------
  const float align = Steer(mob, def, dt);
  {
    const float step = sw.pitchRate * dt;
    mob.pitch_ += std::clamp(wantPitch - mob.pitch_, -step, step);
    mob.pitch_ = std::clamp(mob.pitch_, -sw.maxPitch, sw.maxPitch);
  }

  // ---- 4. THRUST, FINS, BUOYANCY --------------------------------------------
  // A fish cannot turn on the spot: the drive keeps a share of itself while
  // the nose is off the heading it wants, so a turn is a curve.
  const Vec3 fwd = mob.Forward3();
  const float drive = std::clamp(mob.driveScale_, -0.3f, sw.burst);
  const float target = def.speed * drive * (0.35f + 0.65f * align);
  float vAl = mob.swimVel_.dot(fwd);
  Vec3 lat = mob.swimVel_ - fwd * vAl;
  {
    const float dv = target - vAl;
    const bool speeding = target >= 0.0f ? dv > 0.0f : dv < 0.0f;
    const float rate = (speeding ? sw.accel : sw.brake) * dt;
    vAl += std::clamp(dv, -rate, rate);
  }
  lat = lat * std::exp(-sw.sideGrip * dt);
  // The crowd's push (ApplyCrowdSpacing's driveStrafe_): a sidestep of the fins.
  const Vec3 right{std::cos(mob.heading_), 0.0f, -std::sin(mob.heading_)};
  lat = lat + right * (def.speed * mob.driveStrafe_ * std::min(1.0f, sw.sideGrip * dt));
  Vec3 v = fwd * vAl + lat;
  v.y -= g * (1.0f - sw.buoyancy * mob.immersion_) * dt;

  // ---- 5. THE MOVE ------------------------------------------------------------
  // Samples down the body's own axis (nose to tail) and a body-height above and
  // below its middle three: the box it would be is not allowed into rock --
  // except that a body ALREADY partly in it (its turn swung the nose into the
  // bank: the yaw is not collision-checked, as the walker's is not) may move
  // so long as no MORE of it goes in (MobSystem::MoveNoDeeper's rule).
  const Vec3 upB = right.cross(fwd).normalized();
  auto rockAt = [&](Vec3 nc) {
    int n = 0;
    for (float t : {-0.9f, -0.45f, 0.0f, 0.45f, 0.9f}) {
      const Vec3 p = nc + fwd * (t * halfLen);
      if (cache.At(p) == CellKind::Solid) n++;
      if (std::fabs(t) < 0.6f) {
        if (cache.At(p + upB * (halfH * 0.8f)) == CellKind::Solid) n++;
        if (cache.At(p - upB * (halfH * 0.8f)) == CellKind::Solid) n++;
      }
    }
    return n;
  };
  // SUBSTEPPED: the box is only sampled where a step ENDS, so a step longer
  // than the gap between samples could pass through a thin wall or a body. A
  // charge at 5x a fast cruise covers six voxels a tick; the move is cut into
  // pieces of at most kMaxSubstep, each resolved (and glanced) on its own.
  const int nSub = std::clamp(
      (int)std::ceil((v * dt).len() / kMaxSubstep), 1, kMaxSubsteps);
  const float subDt = dt / (float)nSub;
  Vec3 nc = c;
  for (int sub = 0; sub < nSub; sub++) {
    const Vec3 p0 = nc;
    const int rockNow = rockAt(p0);
    auto fits = [&](Vec3 q) {
      if (BlockedByMob(mob, def, q.x, q.z)) return false;
      const int n = rockAt(q);
      return n == 0 || n <= rockNow - (rockNow > 0 ? 0 : 1);
    };
    const Vec3 step = v * subDt;
    Vec3 np = p0 + step;
    // GLANCE OFF: a refused step is first tried turned aside -- a fish that
    // meets a wall or a body at speed slides past it with most of its speed,
    // it does not stop dead (a charge goes past its victim, not into it). The
    // side it is already turning to first; the velocity turns with the step.
    if (!fits(np)) {
      const float side = mob.turnVel_ >= 0.0f ? 1.0f : -1.0f;
      for (float a : {0.35f, -0.35f, 0.7f, -0.7f, 1.05f, -1.05f, 1.4f, -1.4f}) {
        const float r = a * side;
        const float cr = std::cos(r), sr = std::sin(r);
        const Vec3 st{step.x * cr + step.z * sr, step.y, -step.x * sr + step.z * cr};
        if (!fits(p0 + st)) continue;
        np = p0 + st;
        v = Vec3{v.x * cr + v.z * sr, v.y, -v.x * sr + v.z * cr} * 0.92f;
        mob.swimGlances_++;
        break;
      }
    }
    if (!fits(np)) {
      // Each axis alone, longest first, and what refuses its axis loses that
      // velocity: a fish grazing a wall slides along it.
      np = p0;
      float comp[3] = {step.x, step.y, step.z};
      int order[3] = {0, 1, 2};
      std::sort(order, order + 3, [&](int a, int b) {
        return std::fabs(comp[a]) > std::fabs(comp[b]);
      });
      for (int a : order) {
        if (comp[a] == 0.0f) continue;
        Vec3 trial = np;
        (a == 0 ? trial.x : a == 1 ? trial.y : trial.z) += comp[a];
        if (fits(trial)) {
          np = trial;
        } else {
          (a == 0 ? v.x : a == 1 ? v.y : v.z) = 0.0f;
          mob.swimBumps_++;
        }
      }
    }
    nc = np;
  }
  // THE WATERLINE IS A CEILING: a body does not swim out of the water by
  // asking. Its back may just break the surface; a charge's run may lift it
  // `breach` voxels over. Never pulled DOWN by this (a body already above it
  // -- a breach coming back -- sinks on its own).
  if (mob.surfaceKnown_) {
    const float cap = mob.surfaceY_ - halfH * 0.4f +
                      (mob.breachOk_ ? sw.breach : 0.0f);
    if (nc.y > cap && nc.y > c.y) {
      nc.y = std::max(c.y, cap);
      v.y = std::min(v.y, 0.0f);
    }
  }
  mob.swimVel_ = v;
  mob.origin_ = nc - ws * 0.5f;
  mob.airborne_ = false;
  mob.fallVel_ = 0.0f;
  mob.airTime_ = 0.0f;
  mob.phase_ += (nc - c).len() * 0.6f;

  // ---- 6. THE BANK -------------------------------------------------------------
  // The coordinated-turn lean, atan(v w / g): a positive turn rate is a turn
  // to the body's LEFT (heading grows toward +X, its left), and a lean left is
  // a NEGATIVE turn about model +Z (+Z rolls the left side up).
  {
    const float lean = std::atan(vAl * mob.turnVel_ / std::max(g, 1.0f));
    const float want = std::clamp(-lean * sw.bank, -sw.maxRoll, sw.maxRoll);
    mob.roll_ += (want - mob.roll_) * HalfLifeK(dt, 0.18f);
  }
  // The water gives the breath back, twice as fast as the bank took it.
  mob.drySeconds_ = std::max(0.0f, mob.drySeconds_ - 2.0f * dt);
  mob.suffocateOwed_ = 0.0f;
  mob.flopKick_ *= std::exp(-dt / 0.25f);
}

// ---------------------------------------------------------------------------
// TickStranded: out of the water. The header has the account.
// ---------------------------------------------------------------------------
void MobSystem::TickStranded(Mob& mob, const MobDef& def, World& world,
                             const GroundSense& sense, uint32_t tick, float dt) {
  const MobDef::SwimDef& sw = def.swim;
  // It cannot steer and it cannot walk: the AI's opinion is overruled here,
  // once, so no verb can give a fish on the bank a stroll.
  mob.desiredHeading_ = mob.heading_;
  mob.turnVel_ = 0.0f;
  mob.driveScale_ = 0.0f;
  mob.driveStrafe_ = 0.0f;
  const bool falling = UpdateFall(mob, def, sense, dt);
  if (!mob.alive_ || mob.Ragdolled()) return;
  if (!falling) DriveLocomotion(mob, def, sense, 0.0f, dt);   // settle: drive 0

  // ---- ONTO A FLANK -----------------------------------------------------------
  {
    const float k = HalfLifeK(dt, 0.12f);
    mob.pitch_ += (0.0f - mob.pitch_) * k;
    mob.roll_ += (mob.rollSide_ * kFlankRoll - mob.roll_) * k;
  }

  // ---- THE BREATH -------------------------------------------------------------
  mob.drySeconds_ += dt;
  if (mob.drySeconds_ > sw.breathSeconds && sw.suffocateHp > 0.0f) {
    mob.suffocateOwed_ += sw.suffocateHp * dt;
    if (mob.suffocateOwed_ >= 1.0f) {
      const float amt = std::floor(mob.suffocateOwed_);
      mob.suffocateOwed_ -= amt;
      mob.SpendHp(amt, DamageCtx(DamageCause::Asphyxia));
      if (!mob.alive_) {
        if (mob.deathCause_ == nullptr || mob.deathCause_[0] == 0)
          mob.deathCause_ = "suffocated";
        return;
      }
    }
  }

  // ---- THE FLOP -----------------------------------------------------------------
  // On the ground, between flops: a kick that throws it up and along -- toward
  // the nearest liquid it can feel from where it lies (eight bearings, out to
  // flopReach, a voxel above the ground it is on), else anywhere -- and may
  // land it on its other flank. Stunned, it only lies there.
  mob.flopKick_ *= std::exp(-dt / 0.25f);
  if (mob.airborne_ || tick < mob.nextFlopTick_ || mob.Stunned(tick)) return;
  const uint32_t key = (uint32_t)mob.id_ ^ 0xF10Bu;
  const uint32_t h1 = rng::Hash3(key, mob.flops_, 1u);
  const uint32_t h2 = rng::Hash3(key, mob.flops_, 2u);
  const float u1 = (float)(h1 & 0xFFFFu) / 65535.0f;
  const float u2 = (float)((h1 >> 16) & 0xFFFFu) / 65535.0f;
  const float u3 = (float)(h2 & 0xFFFFu) / 65535.0f;
  const float u4 = (float)((h2 >> 16) & 0xFFFFu) / 65535.0f;
  const Vec3 ws = def.worldSize;
  const Vec3 c = mob.origin_ + ws * 0.5f;
  CellCache cache{world, mob.ClassOf()};
  float bestD = 1e9f, bestBearing = 0.0f;
  // Feeling for water: along each bearing at the height the body lies at,
  // and DOWN from there -- a fish on a ledge or a bank feels the water below
  // the edge it is lying beside. A wall at body height ends the bearing.
  if (sw.flopReach >= 1.0f) {
    const int y = ifloor(mob.origin_.y) + 1;
    for (int i = 0; i < 8; i++) {
      const float b = (float)i * (6.2831853f / 8.0f);
      const float sx = std::sin(b), sz = std::cos(b);
      for (float s = 1.0f; s <= sw.flopReach && s < bestD; s += 1.0f) {
        const int cx2 = ifloor(c.x + sx * s), cz2 = ifloor(c.z + sz * s);
        const CellKind k = cache.At(IVec3{cx2, y, cz2});
        if (k == CellKind::Solid || k == CellKind::Unknown) break;
        bool wet = k == CellKind::Liquid;
        for (int dy = 1; dy <= 4 && !wet; dy++) {
          const CellKind kd = cache.At(IVec3{cx2, y - dy, cz2});
          if (kd == CellKind::Liquid) wet = true;
          if (kd == CellKind::Solid || kd == CellKind::Unknown) break;
        }
        if (wet) {
          bestD = s;
          bestBearing = b;
          break;
        }
      }
    }
  }
  const bool towardWater = bestD < 1e8f;
  const float bearing =
      towardWater ? bestBearing : u1 * 6.2831853f;
  const float along = MetresPerSecToCells(sw.flopSpeed) * (towardWater ? 1.0f : 0.5f);
  const float rise = MetresPerSecToCells(sw.flopRise) * (0.7f + 0.6f * u2);
  mob.Launch(Vec3{std::sin(bearing) * along, rise, std::cos(bearing) * along});
  mob.flops_++;
  mob.flopKick_ = 1.0f;
  if (u3 < 0.35f) mob.rollSide_ = -mob.rollSide_;
  // The body swings round with the kick (toward the water when it felt some).
  {
    float turn = (u4 * 2.0f - 1.0f) * 0.9f;
    if (towardWater)
      turn = std::remainder(bestBearing - mob.heading_, 6.2831853f) * 0.5f;
    mob.heading_ = std::remainder(mob.heading_ + turn, 6.2831853f);
    mob.desiredHeading_ = mob.heading_;
  }
  const float wait = sw.flopMin + (sw.flopMax - sw.flopMin) * u4;
  mob.nextFlopTick_ = tick + (uint32_t)std::lround(wait * 30.0f);
}
