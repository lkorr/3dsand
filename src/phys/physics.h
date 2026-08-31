#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "math3d.h"

// Thin ownership wrapper around Jolt (DESIGN.md §7): debris rigidbodies +
// static marching-cubes terrain patches. All public APIs use VOXEL units for
// positions/sizes; conversion to meters (kVoxelMeters) happens inside so the
// rest of the engine stays in one coordinate system. Fixed 30 Hz stepping,
// same tick as the CA.
//
// Jolt is CPU float physics: its results never feed the deterministic grid
// directly — bodies only re-enter the grid through MutationQueue ops.

namespace JPH {
class PhysicsSystem;
class TempAllocatorImpl;
class JobSystemThreadPool;
class BodyInterface;
}  // namespace JPH

struct BodyTransform {
  Vec3 pos;        // voxel units (body center of mass)
  float quat[4];   // x, y, z, w
};

// One voxel of a debris body, in body-local voxel coordinates.
//
// `color` is a 1-BASED MERGED art index (0 = unpainted), independent
// of the material in `payload`: a creature is one material all over and
// painted per voxel, and a limb that lands back in the world grid becomes its
// MATERIAL, never its colour. 0 = unpainted, i.e. use the material's colour.
// It occupies what was explicit padding, so the struct is still 4 bytes.
struct DebrisVoxel {
  int8_t x, y, z;
  uint8_t color = 0;
  uint16_t payload;  // material | state<<12
};

// One sub-shape of a compound collider, in body-local VOXEL coordinates.
// Used by the collision-box debug overlay to draw tight-fitting boxes.
struct SubShapeBox {
  Vec3 center;       // body-local, voxels
  Vec3 halfExtents;  // along the sub-shape's own axes, voxels
  float quat[4];     // x, y, z, w — sub-shape orientation in body-local space
};

class Physics {
 public:
  Physics();
  ~Physics();  // out-of-line: members are incomplete types here
  bool Init();
  void Shutdown();
  void Step(float dt);

  // Debris body from island voxels (local coords relative to `originVoxel`).
  // Boxes are greedy-merged; mass comes from per-voxel material density
  // (kg/m^3). Returns an opaque handle (0 = failure).
  // allowKinematic: body may later switch motion type (mob limbs animate
  // kinematically while alive, go dynamic as ragdoll — PLAN §B4).
  //
  // `voxelPitch` is the size of ONE of the supplied voxels, in world voxels: 1
  // for ordinary debris, 1/scale for a microvoxel mob limb whose coordinates
  // are in micro units (docs/PLAN_voxel_editor.md §C). It scales the collider
  // AND the per-voxel volume that feeds mass, so a limb of the same physical
  // size weighs the same regardless of the resolution it was drawn at.
  uint64_t CreateDebrisBody(const std::vector<DebrisVoxel>& voxels,
                            IVec3 originVoxel,
                            const std::vector<float>& densityOfMat,
                            bool allowKinematic = false,
                            float voxelPitch = 1.0f);
  // Same, but at an arbitrary transform (laser splits inherit the parent
  // body's pose mid-tumble — PLAN §C2).
  uint64_t CreateDebrisBodyXf(const std::vector<DebrisVoxel>& voxels,
                              const BodyTransform& xf,
                              const std::vector<float>& densityOfMat,
                              bool allowKinematic = false,
                              float voxelPitch = 1.0f);
  // Analytic sphere collider — a greedy-boxed voxel ball can never roll
  // smoothly, so rolling objects get a true Jolt sphere. Mass = density *
  // sphere volume. The voxel ball that renders it is the caller's business
  // (DebrisSystem::AdoptBody). `originOffsetVox` is the vector from the BODY
  // ORIGIN to the sphere's centre: zero for a plain centered body, (r,r,r)
  // when the render model is a min-corner-origin microvoxel brick (the micro
  // march runs [0..dims] from the origin, so collider and art must agree on
  // where the origin sits — microbody.wgsl).
  uint64_t CreateSphereBody(Vec3 centerVoxel, float radiusVoxels,
                            float densityKgM3, Vec3 originOffsetVox = Vec3{});
  // Linear/angular velocity in voxel units (split halves keep momentum).
  bool GetBodyVelocities(uint64_t handle, Vec3& lin, Vec3& angRadPerSec) const;
  void SetBodyVelocities(uint64_t handle, Vec3 lin, Vec3 angRadPerSec);

  // ---- joints (PLAN §B1) ----
  enum class JointType { Fixed, Hinge, Ball };

  // Everything a joint needs, in one struct.
  //
  // TWO KINDS OF GEOMETRY LIVE HERE AND THEY ARE NOT IN THE SAME FRAME.
  // `anchorVoxel` is a POSITION, in world voxels, at the moment of creation.
  // Every other vector is a DIRECTION in the rig's REST pose, and CreateJoint
  // rotates it into each body's current pose before handing it to Jolt.
  //
  // That split is the whole point. A Jolt constraint captures its reference
  // frame from the bodies as they stand when it is created, so a joint built
  // from world-space directions puts angle zero at whatever pose the bodies
  // happen to be in. At spawn that is the rest pose and it reads correctly;
  // from MobSystem::RebuildLimbBody — which re-creates a carved limb's joints
  // from the LIVE pose, mid-ragdoll — it silently re-centres every limit on a
  // bent corpse, and the limb is then free to bend that far again. Passing
  // rest-frame directions makes the limits mean the same thing wherever the
  // joint is built from.
  struct JointDesc {
    JointType type = JointType::Ball;
    Vec3 anchorVoxel{};          // WORLD voxels, the pivot
    // ---- Hinge ----
    Vec3 axis{1, 0, 0};          // rest-frame hinge axis
    float minAngle = -1.2f, maxAngle = 1.2f;  // radians, about `axis`
    // ---- Ball ----
    // A swing-twist cone (the joint Jolt's own Ragdoll class uses), NOT a bare
    // point constraint. A point constraint has no angular limit whatsoever,
    // which is what let a corpse fold its thigh 180 degrees up through its own
    // pelvis and its torso back through its hips: nothing in the scene objects,
    // because a mob's limbs are excluded from colliding with each other
    // (DisableCollisionsAmong) precisely so they cannot fight their own joints.
    // The angular limit is therefore the ONLY thing keeping a rig's parts out
    // of each other, and it was missing.
    //
    // `boneAxis` is the direction this limb points at rest — from the joint
    // anchor toward the limb's own centre — and is the cone's CENTRE LINE. It
    // has to come from the rig (mob.cpp derives it from the same anchor and
    // model box the pose pipeline uses); a cone centred on anything else parks
    // the limb against its own limit at rest.
    Vec3 boneAxis{0, -1, 0};
    // Half-angles, radians. `coneFwd` is swing in the limb's fore/aft plane
    // (a thigh stepping, a shoulder reaching), `coneSide` is swing out of it
    // (abduction). Split because a hip that should allow a full stride must
    // not also allow the splits, and one symmetric cone cannot say that.
    float coneFwd = 1.5707963f;   // 90 degrees
    float coneSide = 1.5707963f;
    float twist = 1.0471976f;     // roll about `boneAxis`, +-60 degrees
    // Joint friction as a FRACTION OF THE LIMB'S OWN WEIGHT TORQUE about this
    // anchor, so one number means the same thing on a finger and on a thigh
    // (CreateJoint multiplies by mass * g * lever arm). Below 1 a limb still
    // falls under its own weight, just not like wet rope — which is the
    // difference between a corpse and a rubber toy. 0 disables it.
    float friction = 0.15f;
  };

  // Returns an opaque handle (0 = failure). Joints attached to a body are
  // destroyed automatically when that body is removed.
  uint64_t CreateJoint(uint64_t bodyA, uint64_t bodyB, const JointDesc& desc);
  void DestroyJoint(uint64_t joint);  // <- this is dismemberment
  // How far body B currently sits from the REST direction its ball joint was
  // built around, in radians (0 when the joint is not a ball joint or is
  // dead). The limit test in selftest_mob.cpp asks this rather than
  // re-deriving the cone frame, which would be a second implementation of it.
  bool JointSwingAngle(uint64_t joint, float& outRadians) const;

  // ---- mob locomotion plumbing (PLAN §B4) ----
  // Requires the body to have been created with allowKinematic.
  void SetBodyKinematic(uint64_t handle, bool kinematic);
  // Drive a kinematic body toward a pose over dt (gives it real velocity).
  void MoveKinematicBody(uint64_t handle, Vec3 posVoxel, const float quat[4],
                         float dt);
  void SetBodyVelocity(uint64_t handle, Vec3 velVoxelsPerSec);

  // Move a body onto (or off) the PLAYER-AVATAR collision layer. Bodies there
  // behave exactly like normal dynamic bodies except that they never generate
  // contacts with the player proxy, and they are invisible to PlayerPushOut.
  //
  // WHY THIS EXISTS. The player's avatar is drawn around the player's own
  // capsule, so its limbs are permanently interpenetrated with the proxy. On
  // the normal layer that produced a large depenetration push every tick whose
  // direction swung with the gait animation — the player's own body steering
  // them backwards and sideways. Rays and other queries still see these
  // bodies, so laser/damage hits on the avatar are unaffected.
  //
  // Call with `false` when a limb is severed: the detached piece stops being
  // "you" and should be able to bump into you like any other debris.
  void SetBodyAvatarLayer(uint64_t handle, bool isAvatar);

  // Disable collisions among a set of bodies (one mob's limbs): adjacent limb
  // boxes otherwise fight their own joints — the push/pull jitter keeps the
  // ragdoll awake forever. Jolt's own Ragdoll class does exactly this.
  void DisableCollisionsAmong(const std::vector<uint64_t>& handles);
  // Undo the above for ONE body: drop it out of its mob's GroupFilterTable so
  // a severed limb can hit the corpse it came off. Without this the filter
  // suppresses those contacts forever and the arm falls through the torso.
  void ClearCollisionGroup(uint64_t handle);

  // First dynamic (MOVING-layer) body hit by the ray, or 0. `fraction` is the
  // hit position along the ray (0..1 of maxDistVoxels). Laser body cuts.
  uint64_t CastRayBody(Vec3 fromVoxel, Vec3 dirNormalized, float maxDistVoxels,
                       float& fraction) const;

  // Static terrain collision patch (triangles in voxel units, world space).
  uint64_t CreateTerrainMesh(const std::vector<float>& vertsXYZ,
                             const std::vector<uint32_t>& indices);

  // ---- player proxy (deferred from M6; DESIGN.md §8) ----
  // Capsule the debris collides against. Voxel terrain collision stays in the
  // AABB controller (player.cpp); this body only exists so rigidbodies can't
  // pass through the player and so a moving player shoves debris. It ignores
  // STATIC terrain meshes — colliding with both grids would double-resolve.
  //
  // The proxy is DYNAMIC (rotation-locked, zero gravity, tuned playerMassKg)
  // rather than kinematic: a kinematic body is infinite mass to the solver, so
  // a strolling player would launch a two-ton block exactly like a bucket.
  // With a real mass the solver splits every contact impulse by true mass
  // ratio — light bodies get shoved, heavy ones barely creep — and since body
  // mass comes from per-voxel material density, "how hard can I push it"
  // falls out of the material data with no extra code. MovePlayerBody
  // re-teleports it to the authoritative player position each tick, so
  // whatever the solver did to the proxy itself is discarded; the player only
  // ever moves via PlayerPushOut through its own terrain sweeps.
  uint64_t CreatePlayerBody(float halfXZVox, float halfYVox);
  // Snap the proxy to the player's AABB center and give it the velocity
  // implied by the move (contacts need real velocity to transfer momentum).
  void MovePlayerBody(uint64_t handle, Vec3 centerVoxel, float dt);
  // Depenetration vector (voxel units) to move the player out of any debris
  // bodies overlapping the proxy shape at centerVoxel. Zero when clear.
  // The caller applies it through its own terrain sweeps.
  Vec3 PlayerPushOut(uint64_t handle, Vec3 centerVoxel) const;

  // ---- contact reporting (audio; DESIGN.md §12b) --------------------------
  //
  // One NEW contact from the last Step. Jolt calls OnContactAdded only when a
  // manifold first appears, which is exactly the LANDING moment — a body
  // resting in a pile re-reports through OnContactPersisted, which this
  // deliberately does not listen to, so a settled heap costs nothing.
  //
  // The list is filtered inside the listener before it is stored, because the
  // filter is what bounds it (CLAUDE.md rule 2): contacts below the speed gate,
  // and anything touching the PLAYER proxy or a PLAYER-AVATAR limb, never
  // become entries. Bodies on Layers::AVATAR are excluded by name — your own
  // body parts are permanently interpenetrated with your capsule and would
  // otherwise machine-gun the mixer with your own footsteps.
  //
  // Cleared at the head of every Step, so a caller reads the step it just ran.
  struct ContactImpact {
    uint64_t bodyA = 0, bodyB = 0;  // 0 on a side that is static terrain
    Vec3 posVoxel{};                // contact point, world voxels
    Vec3 normal{};                  // unit, points from bodyA toward bodyB
    float speedVoxPerSec = 0;       // |approach speed| along `normal`
  };
  const std::vector<ContactImpact>& ContactImpacts() const;
  // Contacts slower than this are not reported at all. Set from the game
  // thread; Step latches it into the listener before handing control to the
  // Jolt job threads, which must never read a game-side global themselves
  // (same contract as the audio thread — DESIGN.md §12b).
  void SetContactReportSpeed(float voxPerSec);

  void RemoveBody(uint64_t handle);
  bool GetTransform(uint64_t handle, BodyTransform& out) const;
  bool IsActive(uint64_t handle) const;
  // Put a body to sleep immediately. Used by world LOAD: recreated bodies
  // must start asleep so a settled pile reloads settled (CLAUDE.md rule 2)
  // instead of every piece re-simulating its rest on the first tick.
  void DeactivateBody(uint64_t handle);
  // Local-space bounds of a body's actual COLLIDER, in voxels, relative to its
  // own origin. For the collision-box debug overlay.
  //
  // Read off the Jolt shape rather than recomputed from the voxel list that
  // built it, and that distinction is the whole value of this call: the
  // collider is a greedy box merge of those voxels, capped at 1024 boxes and
  // inflated by a small convex radius, so a reconstruction would show what we
  // MEANT to build while this shows what is actually being collided against.
  // When those two disagree, the disagreement is the bug you are looking for.
  //
  // False when the handle is dead or not in the simulation.
  bool GetLocalBounds(uint64_t handle, Vec3& outMin, Vec3& outMax) const;
  // Individual sub-shapes of a compound collider, in body-local voxels.
  // Returns the count appended (0 for non-compound or dead bodies).
  size_t GetSubShapeBoxes(uint64_t handle,
                          std::vector<SubShapeBox>& out,
                          size_t limit) const;
  // Radial impulse (explosions). center/radius in voxels, impulse in kg*m/s
  // at the center, falling off linearly to zero at radius.
  void ApplyRadialImpulse(Vec3 centerVoxel, float radiusVoxels, float impulse);
  // Wake dynamic bodies whose AABB intersects the given voxel-space sphere
  // (terrain changed under them).
  void WakeNear(Vec3 centerVoxel, float radiusVoxels);

  uint32_t NumActiveBodies() const;

 private:
  std::unique_ptr<JPH::TempAllocatorImpl> tempAlloc_;
  std::unique_ptr<JPH::JobSystemThreadPool> jobs_;
  std::unique_ptr<JPH::PhysicsSystem> system_;
  struct LayerImpls;
  std::unique_ptr<LayerImpls> layers_;
  struct ContactImpls;  // Jolt ContactListener + its bounded buffer
  std::unique_ptr<ContactImpls> contacts_;
  std::vector<uint64_t> dynamicBodies_;  // handles of live debris bodies
  struct JointImpls;                     // Jolt constraint refs (impl detail)
  std::unique_ptr<JointImpls> joints_;
  uint64_t nextJointId_ = 1;
  uint32_t nextCollisionGroup_ = 1;
};
