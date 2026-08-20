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

// Skin width, in voxels. The AABB is queried very slightly shrunk so that
// resting *flush* against an axis-aligned voxel face does not read as an
// overlap. Without this, a box whose face lands exactly on a voxel boundary
// (constantly, on an axis-aligned grid — the coordinates compare bit-equal)
// reports a collision at zero penetration every frame: the move resolves to
// zero length and the player welds itself to the surface. Luanti solves this
// by defining touching as non-intersecting; the shrink is the same fix.
constexpr float kSkin = 1.0f / 512.0f;

// Cosine of the steepest surface that still counts as standable ground. On a
// voxel grid every face is axis-aligned so this is really a "is the blocking
// face a floor, not a wall" test, but keeping the Quake threshold (0.7) means
// the rule reads the same as everywhere else.
constexpr float kMinWalkNormalY = 0.7f;

// Does the AABB centered at p overlap any solid voxel?
bool Collides(const Vec3& p, const Player::KindFn& kindAt) {
  const float hx = Player::kHalfXZ - kSkin, hy = Player::kHalfY - kSkin;
  int x0 = ifloor(p.x - hx), x1 = ifloor(p.x + hx);
  int y0 = ifloor(p.y - hy), y1 = ifloor(p.y + hy);
  int z0 = ifloor(p.z - hx), z1 = ifloor(p.z + hx);
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

// Horizontal distance squared travelled from `from` to `to`. Vertical gain is
// deliberately excluded: a step-up attempt that climbs a lot but advances
// little must not beat a flat slide that actually made progress.
float FlatDist2(const Vec3& from, const Vec3& to) {
  float dx = to.x - from.x, dz = to.z - from.z;
  return dx * dx + dz * dz;
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

// Upward speed above which we are unambiguously leaving the ground under our
// own power, so ground-snapping and step-up must both stand down. Quake 3 says
// "never step up when you still have up velocity"; Source spells the same rule
// NON_JUMP_VELOCITY. Without it the snap drags a jump back onto the bump it is
// trying to leave — which is exactly why jumping while crossing rough ground
// used to fail. Expressed in m/s so it is voxel-size independent.
constexpr float kNonJumpSpeedM = 0.5f;

// Grace windows, seconds. Coyote time keeps a jump legal just after walking off
// an edge; the buffer honours a jump pressed just before landing. Both exist
// because on noisy ground the true airborne/grounded boundary is genuinely
// ragged, and a player pressing jump "while running over gravel" is otherwise
// at the mercy of which frame the press lands on.
constexpr float kCoyoteTime = 0.12f;
constexpr float kJumpBufferTime = 0.12f;
}  // namespace

namespace {

// Positional ground probe: is there standable ground within `reach` voxels
// below the AABB? Returns the (positive) drop to the surface, or -1 for none.
//
// This replaces the old "did this frame's downward sweep get blocked" test,
// which is the single worst bug on noisy terrain: crossing rough ground means
// genuinely leaving the surface for a fraction of a voxel on most frames, so a
// per-frame blocked test makes `grounded` strobe. Everything gated on grounded
// — jumping, step-up, ground friction — then strobes with it. Asking the
// positional question instead ("is there floor under me right now") is stable
// because it does not care whether this particular frame happened to touch.
float GroundProbe(const Vec3& pos, float reach, const Player::KindFn& kindAt) {
  Vec3 test = pos;
  if (!SweepAxis(test, -reach, 1, kindAt)) return -1.0f;  // fell the whole way
  return pos.y - test.y;
}

// Try to advance horizontally by (dx, dz) using the classic Quake/Source
// three-attempt step move, and take whichever attempt travelled farther
// horizontally. Returns the height climbed IN VOXELS (0 if the flat move won).
//
// The three attempts are:
//   (A) slide flat from the original position;
//   (B) lift by the step height, slide from up there, then press back down;
//   and the winner is decided by horizontal distance, not by "did (A) block".
//
// The naive alternative — "if blocked, lift and retry the blocked axis" — is
// what used to be here, and it fails on rough ground in three separate ways:
// it commits to the lift even when the lift makes things worse (raised, the
// AABB can foul a *different* voxel that the flat slide would have slid past,
// and the flat result no longer exists to fall back on); it never validates
// that it landed on something standable, so it will climb the side of a
// one-voxel spike; and by retrying only the blocked axis it drops wall-sliding
// at the exact moment it steps, which reads as catching on every corner.
float StepSlide(Vec3& pos, float dx, float dz, const Player::KindFn& kindAt) {
  const Vec3 start = pos;

  // (A) the flat slide. Each axis is swept independently, so a blocked X still
  // permits the full Z — that is the sliding behaviour, and it must happen
  // before any decision about stepping.
  Vec3 flat = start;
  bool blockedX = SweepAxis(flat, dx, 0, kindAt);
  bool blockedZ = SweepAxis(flat, dz, 2, kindAt);
  if (!blockedX && !blockedZ) {  // nothing in the way: no step needed at all
    pos = flat;
    return 0.0f;
  }

  // (B) lift, slide, settle.
  const float lift = (float)Player::kMaxStepUpVoxels;
  Vec3 up = start;
  SweepAxis(up, lift, 1, kindAt);  // partial lift is fine (low ceiling)
  float lifted = up.y - start.y;
  float climbed = -1.0f;
  if (lifted > 1e-4f) {
    SweepAxis(up, dx, 0, kindAt);
    SweepAxis(up, dz, 2, kindAt);
    // Press back down by the distance actually achieved, not the nominal step
    // height (Quake 3's stepSize fix — matters under a low ceiling).
    bool landed = SweepAxis(up, -lifted, 1, kindAt);
    // The settle must land on real floor. If we fell the whole way back down
    // we merely hopped over nothing; if we ended above where we started
    // without landing, we are wedged. Either way the step is not valid.
    if (landed) {
      float c = up.y - start.y;
      if (c >= -1e-4f) climbed = std::max(0.0f, c);
    }
  }

  // Take whichever attempt actually made horizontal progress. This comparison
  // is the whole point of the pattern: stepping is only better if it moved you
  // farther, and on noisy ground the flat slide frequently wins.
  if (climbed >= 0.0f && FlatDist2(start, up) > FlatDist2(start, flat) + 1e-6f) {
    pos = up;
    return climbed;
  }
  pos = flat;
  return 0.0f;
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

  // Timers run every frame regardless of mode so they never go stale in fly.
  if (coyoteTimer > 0.0f) coyoteTimer -= dt;
  if (jumpBuffer > 0.0f) jumpBuffer -= dt;
  if (in.jumpPressed) jumpBuffer = kJumpBufferTime;

  if (fly) {
    float speed = (in.sprint ? kFlySprintM : kFlySpeedM) / kVoxelMeters;
    Vec3 wish = lookFwd * in.forward + right * in.strafe;
    if (in.up) wish += Vec3{0, 1, 0};
    if (in.down) wish += Vec3{0, -1, 0};
    vel = wish.len() > 1e-3f ? wish.normalized() * speed : Vec3{0, 0, 0};
    pos += vel * dt;
    grounded = false;
    coyoteTimer = 0.0f;
  } else {
    const float nonJumpSpeed = kNonJumpSpeedM / kVoxelMeters;

    // ---- ground state, decided BEFORE the move (Source uses `oldground`) ----
    // Probe reach gets extended by the step height while we already believe we
    // are grounded. That hysteresis is the structural form of coyote time: it
    // stops `grounded` flickering as the body crests sub-voxel noise, and it
    // keeps us attached walking down a rough slope instead of bouncing off it.
    bool rising = vel.y > nonJumpSpeed;
    float reach = grounded ? 0.1f + (float)kMaxStepUpVoxels : 0.1f;
    float drop = rising ? -1.0f : GroundProbe(pos, reach, kindAt);
    bool onGround = drop >= 0.0f && !inLiquid;
    if (onGround) coyoteTimer = kCoyoteTime;

    const float gravity = kGravityM / kVoxelMeters;
    float accel = inLiquid ? 0.25f : 1.0f;
    vel.y -= gravity * accel * dt;

    float speed = ((in.sprint ? kSprintSpeedM : kWalkSpeedM) / kVoxelMeters) *
                  (inLiquid ? 0.55f : 1.0f);
    Vec3 wish = flatFwd * in.forward + right * in.strafe;
    wish.y = 0;
    if (wish.len() > 1e-3f) wish = wish.normalized() * speed;
    // snappy ground control, floatier air control
    float blend = onGround ? 0.35f : (inLiquid ? 0.15f : 0.06f);
    vel.x += (wish.x - vel.x) * blend;
    vel.z += (wish.z - vel.z) * blend;

    // ---- jump: buffered press + coyote window, both consumed on use ----
    bool jumped = false;
    if (inLiquid) {
      vel.y *= 0.92f;  // drag
      if (in.up) vel.y += (kSwimUpM / kVoxelMeters) * dt;  // swim
      if (in.down) vel.y -= (kSwimDownM / kVoxelMeters) * dt;
    } else if (jumpBuffer > 0.0f && coyoteTimer > 0.0f) {
      vel.y = kJumpSpeedM / kVoxelMeters;
      jumpBuffer = 0.0f;
      coyoteTimer = 0.0f;  // consume both, or one press pogos every frame
      onGround = false;    // no snapping or stepping on the frame we launch
      jumped = true;
    }
    const float vmax = kMaxFallM / kVoxelMeters;
    vel.y = std::clamp(vel.y, -vmax, vmax);

    // ---- vertical move ----
    bool blockedY = SweepAxis(pos, vel.y * dt, 1, kindAt);
    if (blockedY) vel.y = 0;

    // ---- horizontal move, with step-up ----
    float climbed =
        onGround ? StepSlide(pos, vel.x * dt, vel.z * dt, kindAt) : 0.0f;
    if (!onGround) {
      // Airborne: plain slide, no stepping (both Quake and Source refuse to
      // step while off the ground). Zeroing the blocked component here is
      // wrong for the same reason it was wrong before — against a voxel
      // staircase a diagonal run alternates X and Z blocks and would lose both
      // components — so a blocked axis just stops advancing this frame and
      // keeps its velocity for the next.
      SweepAxis(pos, vel.x * dt, 0, kindAt);
      SweepAxis(pos, vel.z * dt, 2, kindAt);
    }

    // ---- stay on ground: snap back down onto the surface after moving ----
    // Source calls this at the end of every WalkMove. It is what turns walking
    // *down* rough ground from a series of little falls into contact motion,
    // and it is why grounded stays true across noise. Skipped while rising.
    if (onGround && !jumped && vel.y <= nonJumpSpeed) {
      float snap = GroundProbe(pos, 0.1f + (float)kMaxStepUpVoxels, kindAt);
      if (snap > 0.0f) {
        SweepAxis(pos, -snap, 1, kindAt);
        if (vel.y < 0.0f) vel.y = 0.0f;
      }
    }

    // Re-probe after the move so `grounded` reported to the rest of the frame
    // reflects where we ended up, not where we started.
    grounded = !rising && !inLiquid &&
               GroundProbe(pos, 0.1f, kindAt) >= 0.0f;
    if (grounded) coyoteTimer = kCoyoteTime;

    // Surface roughness is free; only real ledges cost speed. Without this
    // exemption, fine voxels turn every noisy floor into a constant drag.
    float climbedM = climbed * kVoxelMeters;
    if (climbedM > kSmoothBumpM) {
      float scale = std::max(kMinStepSpeedScale,
                             1.0f - (climbedM - kSmoothBumpM) * kStepSpeedPenaltyPerM);
      vel.x *= scale;
      vel.z *= scale;
    }
  }

  // No world-bounds clamp: the world is infinite (toroidal streaming follows
  // the player). The residency-window faces read as Solid through kindAt, so
  // collision alone stops the player in the rare case a face is ever reached
  // (the window recenters ~6 chunks before that).
}

void Player::ApplyPush(Vec3 push, const KindFn& kindAt) {
  float len = push.len();
  if (len < 1e-4f) return;
  // A body can shove at most one body-width per tick: a deeply interpenetrated
  // state (debris spawned around the player) resolves over a few ticks instead
  // of ejecting the camera across the map in one frame.
  const float kMaxPush = 2.0f * kHalfXZ;
  if (len > kMaxPush) push = push * (kMaxPush / len);
  SweepAxis(pos, push.x, 0, kindAt);
  SweepAxis(pos, push.y, 1, kindAt);
  SweepAxis(pos, push.z, 2, kindAt);
  if (push.y > 0.01f && vel.y < 0.0f) {
    // supported from below by a body: standing (and jumping) on debris works
    // even though the voxel ground probe can't see rigidbodies
    vel.y = 0.0f;
    grounded = true;
    coyoteTimer = kCoyoteTime;
  }
}
