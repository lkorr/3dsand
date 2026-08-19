#include "game/player.h"

#include <algorithm>
#include <cmath>

namespace {

// Does the AABB centered at p overlap any solid voxel?
bool Collides(const Vec3& p, const Player::KindFn& kindAt) {
  int x0 = ifloor(p.x - Player::kHalfXZ), x1 = ifloor(p.x + Player::kHalfXZ);
  int y0 = ifloor(p.y - Player::kHalfY), y1 = ifloor(p.y + Player::kHalfY);
  int z0 = ifloor(p.z - Player::kHalfXZ), z1 = ifloor(p.z + Player::kHalfXZ);
  for (int y = y0; y <= y1; y++)
    for (int z = z0; z <= z1; z++)
      for (int x = x0; x <= x1; x++)
        if (kindAt({x, y, z}) == CellKind::Solid) return true;
  return false;
}

// Move along one axis, clamping against solids. Returns true if blocked.
bool SweepAxis(Vec3& pos, float delta, int axis, const Player::KindFn& kindAt) {
  if (delta == 0) return false;
  float* c = axis == 0 ? &pos.x : axis == 1 ? &pos.y : &pos.z;
  float target = *c + delta;
  // step in sub-voxel increments to avoid tunneling at high speed
  float step = delta > 0 ? 0.45f : -0.45f;
  int n = (int)std::ceil(std::abs(delta) / 0.45f);
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
constexpr float kLiquidSampleM = 0.5f;  // body sample spacing for liquid check

// Step-up: walk over small ledges without jumping. Heights are in voxels
// (not meters) since terrain relief is voxel-quantized.
constexpr int kMaxStepUpVoxels = 1;  // tallest ledge climbed automatically
// Fraction of horizontal speed shed per voxel climbed in a frame; steeper
// terrain triggers step-ups more often (and taller ones), so it compounds.
constexpr float kStepSpeedPenaltyPerVoxel = 0.35f;
constexpr float kMinStepSpeedScale = 0.20f;  // climbing never fully stalls
}  // namespace

namespace {

// After a blocked horizontal sweep, try lifting the AABB by 1..kMaxStepUpVoxels,
// redoing the move, then settling back down onto the ledge. Returns voxels
// actually climbed, or -1 if no step height clears the obstacle.
int TryStepUp(Vec3& pos, float delta, int axis, const Player::KindFn& kindAt) {
  for (int h = 1; h <= kMaxStepUpVoxels; h++) {
    Vec3 test = pos;
    if (SweepAxis(test, (float)h, 1, kindAt)) continue;   // no headroom
    if (SweepAxis(test, delta, axis, kindAt)) continue;   // still blocked
    SweepAxis(test, -(float)h, 1, kindAt);                // settle onto ledge
    int climbed = std::max(0, (int)std::lround(test.y - pos.y));
    pos = test;
    return climbed;
  }
  return -1;
}

}  // namespace

void Player::Update(float dt, const PlayerInput& in, const Vec3& flatFwd,
                    const Vec3& right, const Vec3& lookFwd, const KindFn& kindAt) {
  dt = std::min(dt, 0.05f);

  // how much of the body is in liquid?
  int liquidCells = 0, sampled = 0;
  for (int dy = -1; dy <= 1; dy++) {
    IVec3 c{ifloor(pos.x), ifloor(pos.y + dy * (kLiquidSampleM / kVoxelMeters)),
            ifloor(pos.z)};
    if (kindAt(c) == CellKind::Liquid) liquidCells++;
    sampled++;
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

    int climbedVoxels = 0;
    for (int axis : {0, 2}) {
      float& v = axis == 0 ? vel.x : vel.z;
      float start = axis == 0 ? pos.x : pos.z;
      if (!SweepAxis(pos, v * dt, axis, kindAt)) continue;
      // blocked: on the ground, try to climb the ledge with the unconsumed
      // part of the move instead of stopping dead
      float remaining = start + v * dt - (axis == 0 ? pos.x : pos.z);
      int climbed = grounded ? TryStepUp(pos, remaining, axis, kindAt) : -1;
      if (climbed < 0) v = 0; else climbedVoxels += climbed;
    }
    if (climbedVoxels > 0) {
      float scale = std::max(kMinStepSpeedScale,
                             1.0f - climbedVoxels * kStepSpeedPenaltyPerVoxel);
      vel.x *= scale;
      vel.z *= scale;
    }
  }

  // keep inside the world with margin
  float lim = (float)kWorldN - 2.0f;
  pos.x = std::clamp(pos.x, 2.0f, lim);
  pos.y = std::clamp(pos.y, (float)kHalfY + 1.0f, lim);
  pos.z = std::clamp(pos.z, 2.0f, lim);
}
