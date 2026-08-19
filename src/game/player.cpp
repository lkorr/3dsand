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

void Player::Update(float dt, const PlayerInput& in, const Vec3& flatFwd,
                    const Vec3& right, const Vec3& lookFwd, const KindFn& kindAt) {
  dt = std::min(dt, 0.05f);

  // how much of the body is in liquid?
  int liquidCells = 0, sampled = 0;
  for (int dy = -1; dy <= 1; dy++) {
    IVec3 c{ifloor(pos.x), ifloor(pos.y + dy * 4.0f), ifloor(pos.z)};
    if (kindAt(c) == CellKind::Liquid) liquidCells++;
    sampled++;
  }
  inLiquid = liquidCells > 0;

  if (fly) {
    float speed = in.sprint ? 260.0f : 110.0f;
    Vec3 wish = lookFwd * in.forward + right * in.strafe;
    if (in.up) wish += Vec3{0, 1, 0};
    if (in.down) wish += Vec3{0, -1, 0};
    vel = wish.len() > 1e-3f ? wish.normalized() * speed : Vec3{0, 0, 0};
    pos += vel * dt;
  } else {
    const float gravity = 78.5f;  // 9.8 m/s^2 in voxels
    float accel = inLiquid ? 0.25f : 1.0f;
    vel.y -= gravity * accel * dt;

    float speed = (in.sprint ? 64.0f : 36.0f) * (inLiquid ? 0.55f : 1.0f);
    Vec3 wish = flatFwd * in.forward + right * in.strafe;
    wish.y = 0;
    if (wish.len() > 1e-3f) wish = wish.normalized() * speed;
    // snappy ground control, floatier air control
    float blend = grounded ? 0.35f : (inLiquid ? 0.15f : 0.06f);
    vel.x += (wish.x - vel.x) * blend;
    vel.z += (wish.z - vel.z) * blend;

    if (inLiquid) {
      vel.y *= 0.92f;  // drag
      if (in.up) vel.y += 140.0f * dt;  // swim
      if (in.down) vel.y -= 60.0f * dt;
    } else if (in.jumpPressed && grounded) {
      vel.y = 42.0f;
    }
    vel.y = std::clamp(vel.y, -240.0f, 240.0f);

    bool blockedY = SweepAxis(pos, vel.y * dt, 1, kindAt);
    grounded = blockedY && vel.y < 0;
    if (blockedY) vel.y = 0;
    if (SweepAxis(pos, vel.x * dt, 0, kindAt)) vel.x = 0;
    if (SweepAxis(pos, vel.z * dt, 2, kindAt)) vel.z = 0;
  }

  // keep inside the world with margin
  float lim = (float)kWorldN - 2.0f;
  pos.x = std::clamp(pos.x, 2.0f, lim);
  pos.y = std::clamp(pos.y, (float)kHalfY + 1.0f, lim);
  pos.z = std::clamp(pos.z, 2.0f, lim);
}
