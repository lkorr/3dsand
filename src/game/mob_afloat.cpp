// A WALKER THE WATER HOLDS UP (2026-10-11): NPC swimming for any rig that
// carries the swim family's clips (tread / swim / swim_back / swim_side /
// swim_up / swim_down -- every human and everything built on the human rig).
//
// Until now an NPC that walked into a lake walked along its BED: liquid is not
// in the ground probe, so the walk drive settled the body onto the rock under
// the water and it strolled about down there in its walking pose. The player
// has swum since 13a6381; this gives the NPC the same body in the water.
//
// IT IS NOT THE FISH'S DRIVE (game/mob_swim.cpp, MobDef::SwimDef). A fish is a
// volumetric body with no feet, steered in 3D by its behaviour profile's
// altitude; a walker is a ground creature whose AI keeps its ground verbs and
// never asks for a height. What the water changes for it is HOW IT MOVES and
// HOW IT IS POSED, so this sits at the same seam as the walk drive
// (DriveLocomotion / UpdateFall) and the AI never knows:
//
//   * THE MODE is Mob::swimming_, the same flag the fish uses ("the water
//     carries this body"), so everything already routed on it -- the driver
//     velocity a blow adds to (Mob::AddDriverVelocity), a lunge that becomes a
//     surge (Mob::Launch / SwimSurge), the velocity the AI's facts read -- is
//     right for a swimmer of either kind without a second flag. What is FISH
//     is gated on `def.swim.present` everywhere, never on swimming_ alone.
//     A Schmitt pair on the WATER DEPTH over the body's column, in shares of
//     its standing height: in at kAfloatEnter (the bed is out of reach), out
//     under kAfloatLeave (the feet find it), so a body at the edge of its depth
//     does not flicker between wading and swimming.
//   * THE BODY FLOATS with the waterline kAfloatShare up its standing height
//     (the swim clips' own kSwimWaterShare in pose.cpp, so the pose's waterline
//     lift has nothing left to correct), moving at the player's water speed
//     share (player.liquidSpeedScale) with the player's water acceleration
//     (player.liquidAccel). The walk's steering (Steer) and its forward / strafe
//     drive are kept, so a duelist circles in the water as it does on land.
//   * IT DIVES FOR WHAT IT IS FIGHTING. An engaged creature whose target is
//     under the surface below it swims down to the target's height; anything
//     else comes up to the float. There is no breath yet -- a creature holds
//     its dive as long as the fight is down there (a follow-up if wanted).
//   * IT CLIMBS OUT. A move into a bank that stands no more than a step above
//     the waterline hauls the body out onto it, the way the walk drive steps up
//     a block; anything taller is a wall and the body slides along it.
//
// THE POSE is the player's (Mob::PosePipeline, "the swim family"): the driver
// fills PoseInputs::swim and the velocity, and an UNDERWATER stroke gets its
// direction (PoseInputs::swimAim) from the swim velocity, so a dive pitches the
// body along it exactly as the player's look-steered stroke does.

#include <algorithm>
#include <cmath>

#include "game/ai_behavior.h"
#include "game/mob.h"
#include "game/mob_cellcache.h"
#include "sim/scale.h"
#include "sim/tuning.h"
#include "sim/world.h"

namespace {

// The Schmitt pair, in shares of the body's standing height of water over the
// bed at its column. In when the water is over the chin (anything shallower a
// man stands in -- the `aquatic` gate's chest-deep man still stands); out when
// it is back to where the floating soles almost touch. Both above
// kAfloatShare, so a floating body never hangs below its bed.
constexpr float kAfloatEnter = 0.92f;
constexpr float kAfloatLeave = 0.80f;
// Where the waterline crosses a floating body, as a share of its standing
// height above the soles: the tread's shoulders (pose.cpp kSwimWaterShare).
constexpr float kAfloatShare = 0.72f;
// How fast the body closes on the height it wants, per second of error (a
// critically-damped feel at the swim speeds involved), capped at the swim
// speed itself.
constexpr float kDepthGain = 2.5f;
// How far the waterline / bed scans reach, in standing heights.
constexpr float kScanHeights = 2.5f;
// The fall a body that left the water by walking off its depth may have to
// make before it is standing: the drive's own step rule takes it from there.
constexpr float kDiveMargin = 0.5f;

bool Engaged(ai::Intent it) {
  switch (it) {
    case ai::Intent::FaceTarget:
    case ai::Intent::Approach:
    case ai::Intent::HoldRange:
    case ai::Intent::CircleStrafe:
    case ai::Intent::RequestAttack:
    case ai::Intent::Guard:
    case ai::Intent::Dodge:
      return true;
    default:
      return false;
  }
}

}  // namespace

bool MobSystem::CanSwimAfloat(const Mob& mob, const MobDef& def) const {
  // The tread is the family's anchor (pose.cpp gates the whole family on it).
  // A body on a serpent's tail (SlitherDef::upright, the naga) is a walker
  // that swims: its tail kicks with the slither's water wave, under the swim
  // family's strokes. Only a body that IS its chain (the snake) is not.
  return !def.swim.present &&
         (!def.slither.present || def.slither.upright) &&
         mob.Pose().swimClip[0] >= 0;
}

// ---------------------------------------------------------------------------
// SenseAfloat: the waterline and the bed over the body's column, the wet share
// of the body, and the mode.
// ---------------------------------------------------------------------------
void MobSystem::SenseAfloat(Mob& mob, const MobDef& def, World& world) {
  const float H = std::max(def.worldSize.y, 1.0f);
  const int cx = ifloor(mob.origin_.x + def.worldSize.x * 0.5f);
  const int cz = ifloor(mob.origin_.z + def.worldSize.z * 0.5f);
  const int y0 = ifloor(mob.origin_.y);
  const int reach = (int)std::ceil(kScanHeights * H);
  mobwater::CellCache cache{world, mob.ClassOf()};

  const CellKind sole = cache.At(IVec3{cx, y0, cz});
  if (sole == CellKind::Unknown) return;   // not fetched yet: keep the mode
  bool wet = sole == CellKind::Liquid;
  // A body standing ON the bed has its sole cell at the bed top, which is
  // water; one whose origin sank a cell into a powder bed asks the cell above.
  if (!wet && sole == CellKind::Solid) {
    const CellKind up = cache.At(IVec3{cx, y0 + 1, cz});
    wet = up == CellKind::Liquid;
  }
  mob.surfaceKnown_ = mob.bedKnown_ = false;
  if (!wet) {
    mob.immersion_ = 0.0f;
    if (mob.swimming_) LeaveAfloat(mob, def);
    return;
  }
  for (int y = y0 + 1; y <= y0 + reach; y++) {
    const CellKind k = cache.At(IVec3{cx, y, cz});
    if (k == CellKind::Unknown) return;
    if (k != CellKind::Liquid) {
      mob.surfaceKnown_ = true;
      mob.surfaceY_ = (float)y;
      break;
    }
  }
  for (int y = y0; y >= y0 - reach; y--) {
    const CellKind k = cache.At(IVec3{cx, y, cz});
    if (k == CellKind::Unknown) return;
    if (k != CellKind::Liquid) {
      mob.bedKnown_ = true;
      mob.bedY_ = (float)(y + 1);   // the bed's TOP: where a sole would stand
      break;
    }
  }
  // Deeper than the scan either way is still deep water: a lake whose bed is
  // out of reach is swum, not walked.
  const float surf = mob.surfaceKnown_ ? mob.surfaceY_ : (float)(y0 + reach);
  const float bed = mob.bedKnown_ ? mob.bedY_ : (float)(y0 - reach);
  mob.immersion_ = std::clamp((surf - mob.origin_.y) / H, 0.0f, 1.0f);
  const float depth = (surf - bed) / H;

  if (!mob.swimming_ && depth >= kAfloatEnter) {
    // INTO THE WATER. A body that fell in keeps a share of its fall (the
    // splash takes the rest); one that waded in keeps its walk.
    mob.swimming_ = true;
    mob.swimVel_ =
        mob.airborne_
            ? Vec3{mob.airVel_.x, mob.fallVel_, mob.airVel_.z} * 0.6f
            : Vec3{mob.anim_.velocity.x, 0.0f, mob.anim_.velocity.z};
    mob.airborne_ = false;
    mob.launched_ = false;
    mob.fallVel_ = 0.0f;
    mob.airVel_ = Vec3{};
    mob.airTime_ = 0.0f;
  } else if (mob.swimming_ && depth < kAfloatLeave) {
    LeaveAfloat(mob, def);
  }
}

// Out of the water (the feet found the bed, the body climbed out, or the water
// went): the walker's own state takes over. A body still above its footing
// falls the rest of the way under UpdateFall, carrying its swim velocity.
void MobSystem::LeaveAfloat(Mob& mob, const MobDef& def) {
  (void)def;
  mob.swimming_ = false;
  mob.airborne_ = false;
  mob.fallVel_ = 0.0f;
  mob.airVel_ = Vec3{};
  mob.airTime_ = 0.0f;
  mob.launched_ = false;
  mob.swimVel_ = Vec3{};
}

// ---------------------------------------------------------------------------
// DriveAfloat: one tick of a walker swimming. The file header has the account.
// ---------------------------------------------------------------------------
void MobSystem::DriveAfloat(Mob& mob, const MobDef& def, World& world,
                            float dt) {
  const auto& pt = CurrentTuning().player;
  const float H = std::max(def.worldSize.y, 1.0f);
  const float hx = def.worldSize.x * 0.5f, hz = def.worldSize.z * 0.5f;
  const float align = Steer(mob, def, dt);

  // ---- THE WISH: the walk's drive at water speed, plus a height ----------
  const float swimSpeed = def.speed * pt.liquidSpeedScale;
  const Vec3 fwd{std::sin(mob.heading_), 0, std::cos(mob.heading_)};
  const Vec3 rgt{std::cos(mob.heading_), 0, -std::sin(mob.heading_)};
  Vec3 wish = fwd * (swimSpeed * mob.driveScale_ * align) +
              rgt * (swimSpeed * mob.driveStrafe_);
  const float surf =
      mob.surfaceKnown_ ? mob.surfaceY_ : mob.origin_.y + kAfloatShare * H;
  const float floatY = surf - kAfloatShare * H;
  float wantY = floatY;
  // Diving for what it is fighting: the target's centre under the surface
  // and below this body's float by more than half a body.
  if (mob.ai_.hasTarget && Engaged(mob.ai_.intent) &&
      mob.ai_.targetPos.y < surf - kDiveMargin * H &&
      mob.ai_.targetPos.y - 0.5f * H < floatY - kDiveMargin * H)
    wantY = mob.ai_.targetPos.y - 0.5f * H;
  if (mob.bedKnown_) wantY = std::max(wantY, mob.bedY_);
  wish.y = std::clamp((wantY - mob.origin_.y) * kDepthGain, -swimSpeed,
                      swimSpeed);
  const float blend = 1.0f - std::exp(-pt.liquidAccel * dt);
  mob.swimVel_ = mob.swimVel_ + (wish - mob.swimVel_) * blend;

  // ---- THE MOVE, resolved like the walk's (DriveLocomotion `fits`) ------
  // A body is a wall, a fence is a wall, rock more than a step above the
  // body's soles is a wall, and footing under it lifts it (the bed rising to
  // a shore lifts a swimmer onto it until the feet take over).
  const float stepUp = (float)mob.StepUpCells();
  auto fits = [&](float nx, float nz, float fromY, float& outFloorY) {
    if (BlockedByMob(mob, def, nx + hx, nz + hz)) return false;
    if (!fence_.Allows(mob.id_, MobFence::Move, mob.origin_.x + hx,
                       mob.origin_.z + hz, nx + hx, nz + hz))
      return false;
    const Mob::Footing f =
        mob.FootprintFooting(world, def, nx + hx, nz + hz, fromY);
    outFloorY = -1.0e9f;
    if (!f.known) return true;    // unknown is open (ai_nav.h rule 1)
    if (f.wall && !MoveNoDeeper(mob, def, f)) return false;
    if (!f.fits) return false;
    outFloorY = (float)f.groundY;
    return true;
  };
  const Vec3 d = mob.swimVel_ * dt;
  float ny = mob.origin_.y + d.y;
  // Never out of the top: a swimmer rises to its float and no higher (a
  // surge may carry it a little over; the water takes it back).
  ny = std::min(ny, std::max(floatY, mob.origin_.y));
  float floorY = -1.0e9f;
  Vec3 applied{};
  if (d.x != 0.0f || d.z != 0.0f) {
    if (fits(mob.origin_.x + d.x, mob.origin_.z + d.z, ny, floorY)) {
      applied = Vec3{d.x, 0, d.z};
    } else {
      // CLIMBING OUT: the same move, judged from the waterline. A bank that
      // stands within a step of the surface is somewhere to stand; the body
      // hauls out onto it and the walker takes over.
      float bankY = 0.0f;
      if (mob.origin_.y + H > surf &&
          fits(mob.origin_.x + d.x, mob.origin_.z + d.z, surf, bankY) &&
          bankY > ny && bankY <= surf + stepUp) {
        mob.origin_.x += d.x;
        mob.origin_.z += d.z;
        mob.origin_.y = bankY;
        LeaveAfloat(mob, def);
        return;
      }
      // WALL SLIDE: each axis alone, the longer first.
      const bool xFirst = std::fabs(d.x) >= std::fabs(d.z);
      for (int pass = 0; pass < 2; pass++) {
        const bool doX = (pass == 0) == xFirst;
        const float dx = doX ? d.x : 0.0f, dz = doX ? 0.0f : d.z;
        if (dx == 0.0f && dz == 0.0f) continue;
        if (!fits(mob.origin_.x + dx, mob.origin_.z + dz, ny, floorY)) continue;
        applied = Vec3{dx, 0, dz};
        break;
      }
      // What the wall refused, the water stops.
      if (applied.x == 0.0f) mob.swimVel_.x = 0.0f;
      if (applied.z == 0.0f) mob.swimVel_.z = 0.0f;
    }
  }
  if (applied.x == 0.0f && applied.z == 0.0f) {
    float here = -1.0e9f;
    if (fits(mob.origin_.x, mob.origin_.z, ny, here)) floorY = here;
  }
  mob.origin_.x += applied.x;
  mob.origin_.z += applied.z;
  // The floor under the body holds it up (a swimmer brushing the bed, or one
  // carried onto a shelf), and so does the bed the sense measured.
  if (mob.bedKnown_) floorY = std::max(floorY, mob.bedY_);
  if (floorY > -1.0e8f && ny < floorY) {
    ny = floorY;
    mob.swimVel_.y = std::max(mob.swimVel_.y, 0.0f);
  }
  mob.origin_.y = ny;
  // The pose's stroke clock is its own (the swim family's playhead); the walk
  // phase is left where it was so the stride resumes where it stopped.
}
