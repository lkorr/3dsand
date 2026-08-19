#include "game/player.h"

#include <algorithm>
#include <cmath>

namespace {

// The CPU mirror is 3x3x3 chunks, so collision only works while the player's
// AABB fits inside it. Past that the body straddles the window, out-of-mirror
// cells read Unknown, Collides() treats them as air, and the player quietly
// falls through the world. Catch it at startup rather than in play.
static_assert(2.0f * Player::kHalfY < 3.0f * kChunk - 2.0f &&
                  2.0f * Player::kHalfXZ < 3.0f * kChunk - 2.0f,
              "Player AABB exceeds the 3x3x3 CPU mirror: kVoxelMeters is too "
              "small for this player size. Either raise kVoxelMeters or widen "
              "the mirror (World::Snap/mirrorBase) before going finer.");

// Does the AABB centered at p overlap any solid voxel?
bool Collides(const Vec3& p, const Player::KindFn& kindAt) {
  int x0 = ifloor(p.x - Player::kHalfXZ), x1 = ifloor(p.x + Player::kHalfXZ);
  int y0 = ifloor(p.y - Player::kHalfY), y1 = ifloor(p.y + Player::kHalfY);
  int z0 = ifloor(p.z - Player::kHalfXZ), z1 = ifloor(p.z + Player::kHalfXZ);
  // Feet upward: ground is the overwhelmingly common blocker, and the body
  // spans kHalfY*2/kVoxelMeters rows (34 at 0.05 m voxels), so finding the hit
  // on the first row instead of the last is most of the cost.
  for (int y = y0; y <= y1; y++)
    for (int z = z0; z <= z1; z++)
      for (int x = x0; x <= x1; x++)
        if (kindAt({x, y, z}) == CellKind::Solid) return true;
  return false;
}

// Move along one axis, clamping against solids. Returns true if blocked.
//
// The substep is sub-voxel so the sweep cannot tunnel through a one-voxel wall.
// It is also capped in count: at small kVoxelMeters a frame's motion is many
// voxels long (fly-sprint at 0.05 m voxels is ~11 vox/frame), and an uncapped
// voxel-sized substep makes the AABB test count blow up with 1/kVoxelMeters.
// Past the cap we take longer strides — tunneling there is bounded by the
// same physical distance regardless of voxel size.
bool SweepAxis(Vec3& pos, float delta, int axis, const Player::KindFn& kindAt) {
  if (delta == 0) return false;
  float* c = axis == 0 ? &pos.x : axis == 1 ? &pos.y : &pos.z;
  float target = *c + delta;
  constexpr int kMaxSubsteps = 8;
  float dist = std::abs(delta);
  int n = (int)std::ceil(dist / 0.45f);
  if (n > kMaxSubsteps) n = kMaxSubsteps;
  if (n < 1) n = 1;
  float step = (delta > 0 ? dist : -dist) / (float)n;
  for (int i = 0; i < n; i++) {
    float prev = *c;
    float next = (i == n - 1) ? target : prev + step;
    *c = next;
    if (Collides(pos, kindAt)) {
      *c = prev;
      return true;
    }
  }
  return false;
}

}  // namespace

// Physical tuning, in meters (and m/s, m/s^2). Converted to voxel units below.
namespace {
constexpr float kFlySpeedM = 13.75f, kFlySprintM = 32.5f;
constexpr float kWalkSpeedM = 4.5f, kSprintSpeedM = 8.0f;
constexpr float kGravityM = 9.81f;
constexpr float kJumpSpeedM = 5.25f;
constexpr float kSwimUpM = 17.5f, kSwimDownM = 7.5f;
constexpr float kMaxFallM = 30.0f;

// Step-up: walk over small ledges without jumping. Everything here is stated
// as a physical height and converted, so the *feel* of the terrain is fixed by
// how big a bump is in meters, not by how many voxels it happens to be made of.
// Player::kMaxStepUpVoxels is kStepUpM converted to voxels (see player.h).

// Speed shed per METER climbed, not per voxel. Per-voxel was the bug at small
// kVoxelMeters: a 20 cm curb is 2 voxels at 0.125 m but 4 at 0.05 m, so the
// same real ledge cost 2x the speed purely from the resolution change.
constexpr float kStepSpeedPenaltyPerM = 0.35f / 0.125f;  // was 0.35/voxel @12.5cm
constexpr float kMinStepSpeedScale = 0.20f;  // climbing never fully stalls

// Bumps at or below this height are "floor roughness", not ledges: they are
// climbed with no speed penalty at all. This is what makes a noisily-placed
// single-voxel-deep surface feel like smooth ground once voxels are small.
constexpr float kSmoothBumpM = 0.12f;
}  // namespace

namespace {

// After a blocked horizontal sweep, try lifting the AABB by 1..kMaxStepUpVoxels,
// redoing the move, then settling back down onto the ledge. Returns the height
// actually climbed IN VOXELS (fractional — the settle-down lands wherever the
// ledge is, which need not be the height we probed), or -1 if no step height
// clears the obstacle.
//
// The probe is a single lift to the max step height rather than a 1..N ladder:
// at small kVoxelMeters the ladder ran up to kStepUpM/kVoxelMeters iterations
// (9 at 0.05 m) of three sweeps each, and the settle-down finds the true ledge
// top anyway, so the intermediate heights only cost time.
float TryStepUp(Vec3& pos, float delta, int axis, const Player::KindFn& kindAt) {
  const float h = (float)Player::kMaxStepUpVoxels;
  Vec3 test = pos;
  if (SweepAxis(test, h, 1, kindAt)) {
    // Not enough headroom for a full-height lift. Retry with whatever vertical
    // room we actually got; if that is nothing, there is no step to take.
    float lifted = test.y - pos.y;
    if (lifted <= 1e-4f) return -1.0f;
  }
  if (SweepAxis(test, delta, axis, kindAt)) return -1.0f;  // still blocked
  SweepAxis(test, -h, 1, kindAt);                          // settle onto ledge
  float climbed = test.y - pos.y;
  if (climbed < -1e-4f) return -1.0f;  // settled below start: not a step up
  pos = test;
  return std::max(0.0f, climbed);
}

}  // namespace

void Player::Update(float dt, const PlayerInput& in, const Vec3& flatFwd,
                    const Vec3& right, const Vec3& lookFwd, const KindFn& kindAt) {
  dt = std::min(dt, 0.05f);

  // How much of the body is in liquid? Sample a fixed number of points spread
  // over the body's actual height, so coverage does not depend on voxel size.
  int liquidCells = 0;
  constexpr int kLiquidSamples = 5;
  for (int i = 0; i < kLiquidSamples; i++) {
    float t = (float)i / (float)(kLiquidSamples - 1);  // 0..1, feet to head
    IVec3 c{ifloor(pos.x), ifloor(pos.y + (t * 2.0f - 1.0f) * kHalfY),
            ifloor(pos.z)};
    if (kindAt(c) == CellKind::Liquid) liquidCells++;
  }
  inLiquid = liquidCells > 0;

  if (fly) {
    float speed = (in.sprint ? kFlySprintM : kFlySpeedM) / kVoxelMeters;
    Vec3 wish = lookFwd * in.forward + right * in.strafe;
    if (in.up) wish += Vec3{0, 1, 0};
    if (in.down) wish += Vec3{0, -1, 0};
    vel = wish.len() > 1e-3f ? wish.normalized() * speed : Vec3{0, 0, 0};
    pos += vel * dt;
  } else {
    const float gravity = kGravityM / kVoxelMeters;
    float accel = inLiquid ? 0.25f : 1.0f;
    vel.y -= gravity * accel * dt;

    float speed = ((in.sprint ? kSprintSpeedM : kWalkSpeedM) / kVoxelMeters) *
                  (inLiquid ? 0.55f : 1.0f);
    Vec3 wish = flatFwd * in.forward + right * in.strafe;
    wish.y = 0;
    if (wish.len() > 1e-3f) wish = wish.normalized() * speed;
    // snappy ground control, floatier air control
    float blend = grounded ? 0.35f : (inLiquid ? 0.15f : 0.06f);
    vel.x += (wish.x - vel.x) * blend;
    vel.z += (wish.z - vel.z) * blend;

    if (inLiquid) {
      vel.y *= 0.92f;  // drag
      if (in.up) vel.y += (kSwimUpM / kVoxelMeters) * dt;  // swim
      if (in.down) vel.y -= (kSwimDownM / kVoxelMeters) * dt;
    } else if (in.jumpPressed && grounded) {
      vel.y = kJumpSpeedM / kVoxelMeters;
    }
    const float vmax = kMaxFallM / kVoxelMeters;
    vel.y = std::clamp(vel.y, -vmax, vmax);

    bool blockedY = SweepAxis(pos, vel.y * dt, 1, kindAt);
    grounded = blockedY && vel.y < 0;
    if (blockedY) vel.y = 0;

    float climbedM = 0.0f;
    for (int axis : {0, 2}) {
      float& v = axis == 0 ? vel.x : vel.z;
      float start = axis == 0 ? pos.x : pos.z;
      if (!SweepAxis(pos, v * dt, axis, kindAt)) continue;
      // blocked: on the ground, try to climb the ledge with the unconsumed
      // part of the move instead of stopping dead
      float remaining = start + v * dt - (axis == 0 ? pos.x : pos.z);
      float climbed = grounded ? TryStepUp(pos, remaining, axis, kindAt) : -1.0f;
      if (climbed < 0.0f) {
        v = 0;
      } else {
        // Surface roughness is free; only real ledges cost speed. Without this
        // exemption, fine voxels turn every noisy floor into a constant drag.
        float m = climbed * kVoxelMeters;
        if (m > kSmoothBumpM) climbedM += m - kSmoothBumpM;
      }
    }
    if (climbedM > 0.0f) {
      float scale = std::max(kMinStepSpeedScale,
                             1.0f - climbedM * kStepSpeedPenaltyPerM);
      vel.x *= scale;
      vel.z *= scale;
    }
  }

  // No world-bounds clamp: the world is infinite (toroidal streaming follows
  // the player). The residency-window faces read as Solid through kindAt, so
  // collision alone stops the player in the rare case a face is ever reached
  // (the window recenters ~6 chunks before that).
}
