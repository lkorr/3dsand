#include "game/player.h"

#include <algorithm>
#include <cmath>

#include "sim/tuning.h"

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
// The values themselves live in assets/materials/tuning.json and are edited
// with assets/tuner.html; these aliases keep the call sites below readable and
// keep the rationale comments attached to the thing they explain. `T()` is
// re-read every frame, so a tuning reload applies immediately.
namespace {
inline const Tuning::Player& T() { return CurrentTuning().player; }

// Step-up: walk over small ledges without jumping. Everything here is stated
// as a physical height and converted, so the *feel* of the terrain is fixed by
// how big a bump is in meters, not by how many voxels it happens to be made of.
// Player::kMaxStepUpVoxels is kStepUpM converted to voxels (see player.h).

// Speed shed per METER climbed, not per voxel. Per-voxel was the bug at small
// kVoxelMeters: a 20 cm curb is 2 voxels at 0.125 m but 4 at 0.05 m, so the
// same real ledge cost 2x the speed purely from the resolution change.
// -> tuning.json player.stepSpeedPenaltyPerM / player.minStepSpeedScale

// Bumps at or below this height are "floor roughness", not ledges: they are
// climbed with no speed penalty at all. This is what makes a noisily-placed
// single-voxel-deep surface feel like smooth ground once voxels are small.
// -> tuning.json player.smoothBump

// Upward speed above which we are unambiguously leaving the ground under our
// own power, so ground-snapping and step-up must both stand down. Quake 3 says
// "never step up when you still have up velocity"; Source spells the same rule
// NON_JUMP_VELOCITY. Without it the snap drags a jump back onto the bump it is
// trying to leave — which is exactly why jumping while crossing rough ground
// used to fail. Expressed in m/s so it is voxel-size independent.
// -> tuning.json player.nonJumpSpeed

// Grace windows, seconds. Coyote time keeps a jump legal just after walking off
// an edge; the buffer honours a jump pressed just before landing. Both exist
// because on noisy ground the true airborne/grounded boundary is genuinely
// ragged, and a player pressing jump "while running over gravel" is otherwise
// at the mercy of which frame the press lands on.
// -> tuning.json player.coyoteTime / player.jumpBufferTime
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

// DE-PENETRATION: lift the body out of ground it is already inside.
//
// Every sweep in this file is a hard VETO — SweepAxis refuses any substep that
// ENDS overlapping a solid. That is correct while the body starts outside the
// world, and it is a trap the moment it does not: once the AABB overlaps a
// voxel, the very first substep of every axis fails, including the upward ones,
// so nothing can move at all. The player is welded in place until they noclip
// out. Getting there is routine rather than exotic — the CA drops a powder into
// your feet, a step-down settle lands a fraction inside a face, a reaction grows
// a solid where you stand — which is why walking noisy ground gets stuck so
// often. No amount of tuning the sweeps helps, because the sweeps are working
// as designed; what was missing is a way back OUT.
//
// So: scan upward in half-voxel increments for the first position that is free
// and return how far up it is, or -1 if there is no free spot inside `maxRise`.
// Upward only, and capped: a player standing shin-deep in a drift is lifted
// clear, while one buried under a collapse stays buried (being entombed is a
// real state the world can put you in, and teleporting out of it would be a
// worse bug than being stuck). The cap is a caller's policy decision, not a
// constant here.
float UnstickRise(const Vec3& pos, float maxRise, const Player::KindFn& kindAt) {
  if (!Collides(pos, kindAt)) return -1.0f;  // not stuck: nothing to do
  const float kProbeStep = 0.5f;
  for (float rise = kProbeStep; rise <= maxRise + 1e-4f; rise += kProbeStep) {
    Vec3 test = pos;
    test.y += rise;
    if (!Collides(test, kindAt)) return rise;
  }
  return -1.0f;  // buried deeper than the cap allows: leave them in it
}

// WATER-EDGE JUMP: is there a bank in front of us that a jump would put us on?
//
// Swimming is drag-limited by design (liquidDrag), so the swim thrust alone
// tops out at a slow crawl and cannot climb out of anything — at a pool wall
// you bob against the edge indefinitely. Every engine that has water solves
// this the same way: while in liquid, a jump pressed INTO the bank becomes a
// real jump impulse rather than swim thrust (Quake/Source `waterjump`,
// Minecraft's horizontal-collision + step check). This is the predicate.
//
// THE FRAME OF REFERENCE IS THE WATERLINE, NOT THE BODY. That is the whole
// subtlety here, and the first version of this function got it wrong by
// probing the AABB the way the ground step-up does. A floating swimmer does
// not stand on anything: the body straddles the surface with its feet dangling
// however deep the equilibrium between swimUp and buoyancy puts them — in this
// engine about 9 of 17 voxels submerged. Measured against those feet, the lip
// of an ordinary pool is ~11 voxels up, three times the step budget, so an
// AABB-relative test refuses every real pool edge while the player is visibly
// bobbing right at it. The question that actually matters is about the water's
// edge — "is there a bank at the surface in front of me, with room above it" —
// and the answer must not change when the body floats a little higher or lower.
//
// `dir` is the horizontal direction the player is pressing, already normalized.
// `surfaceY` is the height of the first non-liquid cell above the body, i.e.
// the waterline. True when both hold:
//   (1) the cell just across the water's edge, at the waterline, is solid —
//       the "against a bank or a wall" part, and the reason a jump in open
//       water is unaffected;
//   (2) there is room for the body to stand on top of it. A bank you cannot
//       fit on is not a way out, it is an overhang; boosting into it just
//       bonks your head and drops you back in.
//
// Deliberately NOT gated on how deep the body is. Depth limits this on its own:
// the impulse is a fixed velocity fighting liquidDrag, so from the bottom of a
// deep lake it buys a fraction of a meter, while at the surface — where the
// body leaves the liquid and the drag stops applying — the same impulse carries
// the full jump. That falls out of the physics rather than needing a threshold.
// `out` receives the position the body would STAND at on top of the ledge —
// the same point the fit test below validates, handed back so the caller does
// not have to re-derive it (and cannot derive it differently).
bool WaterLedgeAhead(const Vec3& pos, const Vec3& dir, float surfaceY,
                     const Player::KindFn& kindAt, Vec3* out) {
  // How far ahead to probe: just past the AABB face, so we are asking about
  // the voxel we are pressed against, not one we are merely near.
  const float kProbeAhead = Player::kHalfXZ + 0.6f;
  const float ax = pos.x + dir.x * kProbeAhead;
  const float az = pos.z + dir.z * kProbeAhead;

  // (1) is the water's edge a wall? Sample AT the waterline — the cell the
  // surface runs into. A bank one voxel proud of the water and a cliff a
  // hundred voxels tall both answer yes here; (2) is what separates them.
  if (kindAt({ifloor(ax), ifloor(surfaceY), ifloor(az)}) != CellKind::Solid)
    return false;

  // (2) can the body stand on it? Place the AABB on top of that cell and ask
  // whether it fits. This is the test that refuses a sheer cliff: on a pool rim
  // the space above the lip is open and the body fits, whereas against a wall
  // that keeps going the same box is buried in rock. It also refuses an
  // overhang, since the ceiling is what the box collides with there.
  //
  // A LEDGE MAY BE SEVERAL VOXELS PROUD of the water, so scan upward rather
  // than testing only the cell directly above: a pool with a raised coping, or
  // a bank the terrain generator left two voxels high, is exactly the case a
  // player expects to climb. The scan is capped at the step budget so this
  // stays "a ledge you could have walked up had you been on land" rather than
  // a general-purpose wall climb.
  const float lipY = std::floor(surfaceY) + 1.0f;
  const float maxLip = lipY + (float)Player::kMaxStepUpVoxels;
  for (float top = lipY; top <= maxLip + 1e-4f; top += 1.0f) {
    Vec3 stand{ax, top + Player::kHalfY, az};
    if (!Collides(stand, kindAt)) {
      if (out) *out = stand;
      return true;
    }
  }
  return false;
}

// LEDGE GRAB: is there a lip within hand reach in the facing direction?
//
// The airborne sibling of WaterLedgeAhead, and the same kind of question asked
// in a different frame of reference. The water jump measures from the
// WATERLINE because a swimmer's body height above the surface is equilibrium
// noise; a jumper has no waterline, so this measures from the BODY — but from
// the HANDS, not the feet. The gesture is "arms up, facing the wall": anything
// whose top surface sits between the shoulders and the fingertips
// (ledgeReach above the head) is catchable, and anything below that is not a
// grab, it is a step — kMaxStepUpVoxels already owns that regime.
//
// `dir` is the horizontal FACING (the arms point where the camera points), not
// the pressed direction the water jump uses: mid-jump the player may have no
// horizontal input at all — momentum is carrying them — and the grab is what
// space is held FOR.
//
// Three probe columns (two hands and the centerline) at two depths, because a
// hand can catch a lip slightly to the side or slightly farther out than the
// face the body is pressed against. Each column is scanned TOP-DOWN and only
// its FIRST solid cell can be the lip: if that cell has solid above it the
// wall simply continues past reach and the column refuses — which is what
// keeps a sheer wall ungrabbable at any fall speed. Requiring two clear cells
// above the lip is hand room: a one-voxel slot between two slabs is a crack,
// not a ledge.
//
// The HIGHEST lip across all columns wins. On a noisy wall that is the natural
// climbing choice, and for a jump that fell short it is the surface the hands
// pass last.
struct LedgeHit {
  IVec3 lip;    // the solid voxel the hands land on
  Vec3 anchor;  // where the body settles while dangling (hands on lip)
  Vec3 stand;   // standing position on top of the lip, validated at pull-up
};
bool LedgeGrabAhead(const Vec3& pos, const Vec3& dir,
                    const Player::KindFn& kindAt, LedgeHit* out) {
  if (T().ledgeReach <= 0.0f) return false;  // knob at 0 disables grabbing
  const Vec3 perp{-dir.z, 0.0f, dir.x};
  const float reachUp = T().ledgeReach / kVoxelMeters;
  // Shoulders up to fingertips. The lower bound matters as much as the upper:
  // without it a chest-high lip "grabs" and the dangle settle yanks the body
  // a full arm-plus-torso downward, which reads as the wall swallowing you.
  const int yLo = ifloor(pos.y + 0.5f * Player::kHalfY);
  const int yHi = ifloor(pos.y + Player::kHalfY + reachUp);
  const float depths[2] = {Player::kHalfXZ + 0.6f, Player::kHalfXZ + 1.4f};
  const float lats[3] = {0.0f, -0.6f * Player::kHalfXZ, 0.6f * Player::kHalfXZ};

  bool found = false;
  IVec3 bestLip{};
  float bestAx = 0, bestAz = 0, bestLat = 0;
  for (float ahead : depths) {
    for (float lat : lats) {
      const float ax = pos.x + dir.x * ahead + perp.x * lat;
      const float az = pos.z + dir.z * ahead + perp.z * lat;
      const int cx = ifloor(ax), cz = ifloor(az);
      for (int y = yHi; y >= yLo; y--) {
        if (kindAt({cx, y, cz}) != CellKind::Solid) continue;
        // First solid from the top. A lip only if the hands have room on it.
        if (kindAt({cx, y + 1, cz}) != CellKind::Solid &&
            kindAt({cx, y + 2, cz}) != CellKind::Solid &&
            (!found || y > bestLip.y)) {
          found = true;
          bestLip = {cx, y, cz};
          bestAx = ax;
          bestAz = az;
          bestLat = lat;
        }
        break;  // solid with solid above: wall past reach, column refuses
      }
    }
  }
  if (!found) return false;
  if (out) {
    const float lipTop = (float)(bestLip.y + 1);
    out->lip = bestLip;
    // Dangle straight down from where the winning hand caught: same x/z as
    // the body (shifted to the hand's column), head ledgeHangDrop below the
    // lip. No pull toward the wall — the sweep-driven settle cannot penetrate
    // anything, and drifting the body sideways on latch reads as suction.
    out->anchor = {pos.x + perp.x * bestLat,
                   lipTop - T().ledgeHangDrop / kVoxelMeters - Player::kHalfY,
                   pos.z + perp.z * bestLat};
    // Standing on the lip, centered over the probe point — the same shape
    // WaterLedgeAhead hands back, consumed by the same mantle.
    out->stand = {bestAx, lipTop + Player::kHalfY, bestAz};
  }
  return true;
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
  ledgeGrabbed = false;  // one-frame flag; set again below if a grab latches

  // Decay the render-only step-smoothing offset toward zero (frame-rate
  // independent: a fixed half-life, so the eye covers half the remaining
  // distance every viewSmoothHalflife seconds at any FPS). Decayed BEFORE this
  // frame's snaps are accumulated so the frame a step lands on starts fully
  // compensated. See Player::ViewEyePos().
  {
    const float hl = T().viewSmoothHalflife;
    if (hl > 1e-4f)
      viewYOffset *= std::pow(0.5f, dt / hl);
    else
      viewYOffset = 0.0f;  // knob at 0 disables smoothing entirely
  }

  // ---- unstick: eject a body that is already inside solid ground ----
  //
  // Runs BEFORE anything else moves, because every sweep below is a veto that
  // fails outright from an overlapping start (see UnstickRise). Skipped in fly
  // mode, where passing through solids is the whole point.
  //
  // The lift is RATE-LIMITED rather than teleported: a whole step height in one
  // frame reads as a pop, and the cases this fires on are usually a voxel or two
  // deep, so at a few m/s the eye barely registers it. It also self-limits — the
  // moment the AABB is clear, UnstickRise returns -1 and this stops. The vertical
  // velocity is cancelled on the way out so a body that was falling into the
  // ground does not immediately drive itself back in, and the climb is banked
  // into viewYOffset exactly like a step-up so the camera glides rather than
  // jumps.
  if (!fly) {
    const float maxRise = T().unstickMaxDepth / kVoxelMeters;
    float rise = UnstickRise(pos, maxRise, kindAt);
    if (rise > 0.0f) {
      float step = std::min(rise, (T().unstickSpeed / kVoxelMeters) * dt);
      pos.y += step;
      viewYOffset -= step;
      if (vel.y < 0.0f) vel.y = 0.0f;
      // Being lifted out counts as standing on the thing you were inside:
      // without this the frame reports airborne, which strobes every
      // grounded-gated system for as long as the ejection takes.
      grounded = true;
      coyoteTimer = T().coyoteTime;
    }
  }

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
  // HOW MUCH of the body is under, 0..1 — not just whether any of it is.
  //
  // The count was already being taken here and then thrown away on a `> 0`,
  // which made every liquid effect all-or-nothing: a body with its toes in a
  // puddle got the same full drag, buoyancy and wade-speed penalty as one
  // fully submerged. That is what made the water-edge jump impossible to tune
  // rather than merely weak. A floating swimmer's feet sit below the waterline
  // by construction, so `inLiquid` stayed true for the entire jump arc and the
  // full liquidDrag ate the impulse ~6 voxels up, every time, no matter how
  // large the impulse was. Drag you cannot escape by rising is not drag, it is
  // a ceiling.
  //
  // Scaling by submersion removes the ceiling without special-casing the jump:
  // as the body clears the surface the drag naturally releases, which is also
  // the correct answer for wading, for a swimmer's head breaking the surface,
  // and for standing in shallow water — all of which previously read as "fully
  // in the sea".
  const float submersion =
      (float)liquidCells / (float)kLiquidSamples;

  // WATERLINE: the first non-liquid cell above the deepest liquid we are in.
  // The water-edge jump is measured against this rather than against the body
  // (see WaterLedgeAhead), so it needs the actual surface height, not a sample.
  //
  // Scanned from the feet UP, and only as far as a body height above the head:
  // the surface we care about is the one WE are floating in, and a submerged
  // swimmer under an air pocket should read the top of their own water column,
  // not some other surface far above. Bounded so a deep dive cannot turn this
  // into a long walk up the column every frame.
  float waterSurfaceY = 0.0f;
  bool haveSurface = false;
  if (inLiquid) {
    const int feet = ifloor(pos.y - kHalfY);
    const int ceiling = ifloor(pos.y + kHalfY) + (int)(2.0f * kHalfY);
    const int cx = ifloor(pos.x), cz = ifloor(pos.z);
    for (int y = feet; y <= ceiling; y++) {
      if (kindAt({cx, y, cz}) != CellKind::Liquid) {
        waterSurfaceY = (float)y;
        haveSurface = true;
        break;
      }
    }
  }

  // Timers run every frame regardless of mode so they never go stale in fly.
  if (coyoteTimer > 0.0f) coyoteTimer -= dt;
  if (jumpBuffer > 0.0f) jumpBuffer -= dt;
  if (in.jumpPressed) jumpBuffer = T().jumpBufferTime;

  // ---- water-edge mantle: drive the body onto the ledge it committed to ----
  //
  // A scripted climb, so it runs INSTEAD of the normal move rather than
  // alongside it: normal movement is what cannot get out of the water in the
  // first place (see the mantleTimer note in player.h), and letting gravity and
  // drag keep acting during the climb just fights it. Velocity stays zeroed and
  // position is driven straight at the validated target.
  //
  // It still moves through the SWEEPS, never by assignment. The target was
  // validated as free when the mantle latched, but the world is a live cellular
  // automaton — the bank can collapse, or a powder can pour into the spot,
  // between the latch and the arrival. Sweeping means the worst case is being
  // stopped short and dropped back in the water, which is recoverable, rather
  // than being teleported inside solid rock, which is the welded-in-place state
  // UnstickRise exists to dig out of.
  //
  // UP FIRST, then across. Reversed, the body drives into the wall it is
  // climbing and the mantle stalls against it every time.
  if (mantleTimer > 0.0f && !fly) {
    mantleTimer -= dt;
    Vec3 d = mantleTarget - pos;
    const float rise = (mantleSpeed / kVoxelMeters) * dt;
    float yBefore = pos.y;
    if (d.y > 1e-3f) {
      SweepAxis(pos, std::min(d.y, rise), 1, kindAt);
    } else {
      // At height: cross onto the bank. Only now, so the horizontal press
      // cannot start until there is somewhere to press onto.
      float remain = std::sqrt(d.x * d.x + d.z * d.z);
      if (remain > 1e-3f) {
        float s = std::min(1.0f, rise / remain);
        SweepAxis(pos, d.x * s, 0, kindAt);
        SweepAxis(pos, d.z * s, 2, kindAt);
      }
    }
    // Bank the climb into the view offset like a step-up, so the camera glides
    // out of the water instead of snapping up it.
    viewYOffset -= pos.y - yBefore;
    viewYOffset = std::clamp(viewYOffset, -(float)kMaxStepUpVoxels,
                             (float)kMaxStepUpVoxels);

    // Done when we arrive, or when the timer runs out — the timeout is what
    // stops a mantle that got blocked mid-climb (collapsed bank, a body shoved
    // into the way) from holding movement hostage forever.
    Vec3 left = mantleTarget - pos;
    if (left.len() < 0.35f || mantleTimer <= 0.0f) {
      mantleTimer = 0.0f;
      vel = Vec3{0, 0, 0};
      grounded = true;
      coyoteTimer = T().coyoteTime;
    }
    return;  // scripted: no gravity, no swim, no walk this frame
  }

  // ---- ledge grab: dangling from a lip by the hands ----
  //
  // The same shape as the water mantle above: a scripted state that replaces
  // the normal move while it holds. The latch itself happens at the END of the
  // walk branch (post-move, so it judges where the body actually ended up);
  // this block is everything that happens after it.
  //
  // Space held is the grip. Releasing it — or pressing crouch — lets go and
  // drops from rest; gravity resumes THIS frame by falling through to the
  // normal move. And because the world is a live cellular automaton, the lip
  // is re-validated every frame: the voxel the hands are on can burn away,
  // dissolve, or be sealed over between one frame and the next, and a grip on
  // a cell that no longer exists must open on its own.
  if (hanging && !fly) {
    const bool lipOk =
        kindAt(hangLip) == CellKind::Solid &&
        kindAt({hangLip.x, hangLip.y + 1, hangLip.z}) != CellKind::Solid;
    if (!in.up || in.down || !lipOk || inLiquid) {
      hanging = false;  // let go: fall through, gravity resumes this frame
    } else if (in.forward > 0.3f) {
      // Pull up. Two regimes, chosen by whether the body can actually STAND
      // on the lip — asked NOW, not at latch time, because the world may have
      // changed while we dangled:
      //
      // Room to stand -> the same committed, rate-limited mantle the water
      // edge uses (mantleTimer above). Reliable at any lip height, cannot
      // overshoot, ends standing on the thing you climbed.
      //
      // No room (a one-voxel ledge on a rough wall, an overhang above) -> a
      // ballistic ARM BOOST straight up. It cannot land you on this lip — a
      // dead-hang body is a full arm-plus-height below it and a jump's worth
      // of rise does not cover that — but it does not need to: near the apex
      // the hands are a boost higher than they were, the latch below runs
      // again, and the next lip up catches. Grab, boost, grab is how a noisy
      // wall becomes climbable, which is exactly what this feature is for.
      hanging = false;
      if (!Collides(hangStand, kindAt)) {
        mantleTarget = hangStand;
        mantleSpeed = T().ledgeMantleSpeed;
        mantleTimer = T().ledgeMantleTime;
        vel = Vec3{0, 0, 0};
        return;  // the mantle block above drives from the next frame
      }
      // Arms, not legs: deliberately not scaled by jumpScale — a wizard with
      // no legs can still do a pull-up.
      vel = Vec3{0, 0, 0};
      vel.y = T().ledgeBoostSpeed / kVoxelMeters;
      // fall through: the boost integrates through the normal move this frame
    } else {
      // Dangle. Velocity is zeroed and the body settles toward the anchor
      // (hands on the lip, head ledgeHangDrop below it) — rate-limited at
      // the mantle speed and moved through the SWEEPS, never by assignment,
      // for the same live-world reasons the mantle lists.
      vel = Vec3{0, 0, 0};
      grounded = false;
      coyoteTimer = 0.0f;
      Vec3 d = hangAnchor - pos;
      const float step = (T().ledgeMantleSpeed / kVoxelMeters) * dt;
      const float yBefore = pos.y;
      if (std::abs(d.y) > 1e-3f)
        SweepAxis(pos, std::clamp(d.y, -step, step), 1, kindAt);
      const float remain = std::sqrt(d.x * d.x + d.z * d.z);
      if (remain > 1e-3f) {
        const float s = std::min(1.0f, step / remain);
        SweepAxis(pos, d.x * s, 0, kindAt);
        SweepAxis(pos, d.z * s, 2, kindAt);
      }
      // Bank the settle into the view offset like every other scripted
      // vertical move, so the eye eases down to the dead hang.
      viewYOffset -= pos.y - yBefore;
      viewYOffset = std::clamp(viewYOffset, -(float)kMaxStepUpVoxels,
                               (float)kMaxStepUpVoxels);
      return;  // scripted: no gravity, no walk this frame
    }
  }

  if (fly) {
    float speed = (in.sprint ? T().flySprint : T().flySpeed) / kVoxelMeters;
    Vec3 wish = lookFwd * in.forward + right * in.strafe;
    if (in.up) wish += Vec3{0, 1, 0};
    if (in.down) wish += Vec3{0, -1, 0};
    vel = wish.len() > 1e-3f ? wish.normalized() * speed : Vec3{0, 0, 0};
    pos += vel * dt;
    grounded = false;
    hanging = false;  // no timer guards the grip, so fly must open it
    coyoteTimer = 0.0f;
    viewYOffset = 0.0f;  // fly motion is deliberate: never smooth it
  } else {
    const float nonJumpSpeed = T().nonJumpSpeed / kVoxelMeters;

    // ---- ground state, decided BEFORE the move (Source uses `oldground`) ----
    // Probe reach gets extended by the step height while we already believe we
    // are grounded. That hysteresis is the structural form of coyote time: it
    // stops `grounded` flickering as the body crests sub-voxel noise, and it
    // keeps us attached walking down a rough slope instead of bouncing off it.
    bool rising = vel.y > nonJumpSpeed;
    float reach = grounded ? 0.1f + (float)kMaxStepUpVoxels : 0.1f;
    float drop = rising ? -1.0f : GroundProbe(pos, reach, kindAt);
    bool onGround = drop >= 0.0f && !inLiquid;
    if (onGround) coyoteTimer = T().coyoteTime;

    const float gravity = T().gravity / kVoxelMeters;
    // Buoyancy lerps in with submersion rather than switching on at the first
    // sample: at 1/5 under you are barely lightened, fully under you get the
    // authored liquidGravityScale. Same value at full submersion as before, so
    // swimming proper is unchanged; what changes is the shallow end.
    float accel =
        inLiquid ? (1.0f + (T().liquidGravityScale - 1.0f) * submersion) : 1.0f;
    vel.y -= gravity * accel * dt;

    // Wade speed also scales with submersion: ankle-deep water should barely
    // slow you, chest-deep should be the authored liquidSpeedScale.
    float wade =
        inLiquid ? (1.0f + (T().liquidSpeedScale - 1.0f) * submersion) : 1.0f;
    float speed = ((in.sprint ? T().sprintSpeed : T().walkSpeed) / kVoxelMeters) *
                  wade * (speedScale > 0.0f ? speedScale : 0.0f);
    Vec3 wish = flatFwd * in.forward + right * in.strafe;
    wish.y = 0;
    if (wish.len() > 1e-3f) wish = wish.normalized() * speed;
    // Snappy ground control, floatier air control. Expressed as a per-SECOND
    // rate and converted with 1-exp(-rate*dt): lerping by a bare constant every
    // frame (what this used to do) made acceleration and water drag scale with
    // frame rate, so the same input felt different at 30 and 144 FPS.
    float rate = onGround ? T().groundAccel
                          : (inLiquid ? T().liquidAccel : T().airAccel);
    float blend = 1.0f - std::exp(-rate * dt);
    vel.x += (wish.x - vel.x) * blend;
    vel.z += (wish.z - vel.z) * blend;

    // ---- jump: buffered press + coyote window, both consumed on use ----
    bool jumped = false;
    waterJumped = false;
    if (inLiquid) {
      // Drag proportional to how much of the body is actually in the water
      // (frame-rate independent). This is the one that matters most: at full
      // submersion it is exactly the authored liquidDrag, so swimming feels as
      // tuned, but a body rising out of the water sheds it continuously
      // instead of dragging until the last sample pops clear. Without this the
      // water-edge jump cannot work at any impulse — see the note on
      // `submersion` above.
      vel.y *= std::exp(-T().liquidDrag * submersion * dt);

      // Water-edge mantle. Gated on the same canJump/jumpScale the dry jump is
      // — a wizard with no legs cannot pull themselves out of a pool either.
      //
      // The direction probed is the one the player is PRESSING, not the one
      // they are looking at: pressing into the edge is the gesture, and using
      // look direction instead would fire whenever you glanced at a nearby wall
      // while swimming past it. With no horizontal input there is nothing to
      // climb toward and the branch simply does not apply.
      Vec3 dir = wish;
      dir.y = 0;
      if (mantleTimer <= 0.0f && jumpBuffer > 0.0f && canJump &&
          jumpScale > 0.0f && haveSurface && dir.len() > 1e-3f) {
        dir = dir.normalized();
        Vec3 target;
        if (WaterLedgeAhead(pos, dir, waterSurfaceY, kindAt, &target)) {
          mantleTarget = target;
          mantleSpeed = T().waterMantleSpeed;
          mantleTimer = T().waterMantleTime;
          jumpBuffer = 0.0f;  // consume, or it re-fires every frame in contact
          waterJumped = true;
          vel = Vec3{0, 0, 0};  // the climb drives position, not velocity
        }
      }

      if (!waterJumped && mantleTimer <= 0.0f) {
        // Swim thrust scales with submersion for the same reason drag does:
        // you can only push against water you are actually in. This is not a
        // refinement, it is what keeps the surface a surface — swimUp is much
        // larger than gravity, so an unscaled thrust against a drag that fades
        // as you rise levitates the body clear out of the pool and leaves it
        // hovering with only its feet wet. Scaled, thrust and buoyancy fall off
        // together and the body settles AT the waterline, which is what
        // floating is.
        if (in.up) vel.y += (T().swimUp / kVoxelMeters) * submersion * dt;
        if (in.down) vel.y -= (T().swimDown / kVoxelMeters) * submersion * dt;
      }
    } else if (jumpBuffer > 0.0f && coyoteTimer > 0.0f && canJump &&
               jumpScale > 0.0f) {
      vel.y = (T().jumpSpeed / kVoxelMeters) * jumpScale;
      jumpBuffer = 0.0f;
      coyoteTimer = 0.0f;  // consume both, or one press pogos every frame
      onGround = false;    // no snapping or stepping on the frame we launch
      jumped = true;
    }
    const float vmax = T().maxFall / kVoxelMeters;
    vel.y = std::clamp(vel.y, -vmax, vmax);

    // ---- vertical move ----
    bool blockedY = SweepAxis(pos, vel.y * dt, 1, kindAt);
    if (blockedY) vel.y = 0;

    // ---- horizontal move, with step-up ----
    float climbed =
        onGround ? StepSlide(pos, vel.x * dt, vel.z * dt, kindAt) : 0.0f;
    // The climb is an instantaneous vertical snap of the BODY; cancel it in
    // the view offset so the eye stays put this frame and glides up as the
    // offset decays. Horizontal motion is untouched — stays 1:1.
    viewYOffset -= climbed;
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
        float yBefore = pos.y;
        SweepAxis(pos, -snap, 1, kindAt);
        // Downward twin of the step-up compensation: the snap teleports the
        // body onto the lower surface, so bank the drop (positive) into the
        // view offset and let the eye follow it down over the half-life.
        viewYOffset += yBefore - pos.y;
        if (vel.y < 0.0f) vel.y = 0.0f;
      }
    }

    // Re-probe after the move so `grounded` reported to the rest of the frame
    // reflects where we ended up, not where we started.
    grounded = !rising && !inLiquid &&
               GroundProbe(pos, 0.1f, kindAt) >= 0.0f;
    if (grounded) coyoteTimer = T().coyoteTime;

    // ---- ledge grab latch (see the hanging block above) ----
    //
    // Airborne, space held — the same reach-up gesture swimming uses — and
    // not powering upward: the vel.y gate is nonJumpSpeed, the constant that
    // already means "unambiguously leaving the ground under own power". While
    // a jump or an arm boost is still driving up, the hands stay off the
    // wall; that is also what stops a boost from instantly re-latching the
    // lip it just left. Judged AFTER the move, against where the body
    // actually ended the frame.
    if (!hanging && !grounded && !inLiquid && mantleTimer <= 0.0f && in.up &&
        !in.down && vel.y <= nonJumpSpeed) {
      Vec3 dir = flatFwd;
      dir.y = 0.0f;
      if (dir.len() > 1e-3f) {
        dir = dir.normalized();
        LedgeHit hit;
        if (LedgeGrabAhead(pos, dir, kindAt, &hit)) {
          hanging = true;
          ledgeGrabbed = true;
          hangLip = hit.lip;
          hangAnchor = hit.anchor;
          hangStand = hit.stand;
          hangDir = dir;
          vel = Vec3{0, 0, 0};  // the grip arrests the fall
        }
      }
    }

    // Surface roughness is free; only real ledges cost speed. Without this
    // exemption, fine voxels turn every noisy floor into a constant drag.
    float climbedM = climbed * kVoxelMeters;
    if (climbedM > T().smoothBump) {
      float scale = std::max(T().minStepSpeedScale,
                             1.0f - (climbedM - T().smoothBump) * T().stepSpeedPenaltyPerM);
      vel.x *= scale;
      vel.z *= scale;
    }

    // Cap the view offset at one step height. Anything bigger than a step is
    // not a step — a long fall resolved by the ground snap, a spawn, a shove —
    // and smearing the camera across it reads as lag, not smoothness.
    const float maxOff = (float)kMaxStepUpVoxels;
    viewYOffset = std::clamp(viewYOffset, -maxOff, maxOff);
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
    coyoteTimer = T().coyoteTime;
  }
}
