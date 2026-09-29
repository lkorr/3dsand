#pragma once
#include <cstdint>
#include <memory>
#include <unordered_map>
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

// ---- THE ANTI-TUNNEL A/B ARM, IN ONE BINARY --------------------------------
//
// "Nothing passes through the ground" is three mechanisms that landed together
// (DESIGN.md, and the notes above Physics::CreateDebrisBody and
// DebrisSystem::UntunnelBody), and all three change where a rigid body ends up
// — so the next unexplained body-settling number is going to want to ask
// "was it this?". A differential measured across two BUILDS measures the
// builds too (CLAUDE.md), so it is an environment variable, exactly as
// SANDVOX_TERRAIN_BUILDS_PER_TICK is for the patch budget:
//
//   SANDVOX_NO_ANTITUNNEL=1          all three off (Jolt's Discrete default,
//                                    no lookahead, no clamp) = pre-2026-09-12
//   SANDVOX_NO_ANTITUNNEL=ccd        Discrete motion quality only
//   SANDVOX_NO_ANTITUNNEL=lookahead  ManageTerrain stops asking ahead
//   SANDVOX_NO_ANTITUNNEL=clamp      UntunnelBody becomes a no-op
//
// Read once and cached; unset (the normal case) costs one predictable branch.
enum class AntiTunnel { Ccd, Lookahead, Clamp };
bool AntiTunnelOff(AntiTunnel part);

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
  // Body coat word, same encoding as PrefabVoxel::stain (sim/voxload.h
  // BodyStain*): the MATERIAL on this voxel in bits 0..11, how much of it in
  // bits 12..15. The one field that grew the struct (6 -> 8 bytes with
  // alignment; widening it from a byte to the coat word costs nothing, since
  // the byte was followed by a byte of padding), and it is here rather than
  // folded into `payload`'s state nibble because that nibble is what a carved
  // voxel takes into the grid as a liquid's fullness.
  uint16_t stain = 0;
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
  // Mass in kg, 0 for a dead handle. Read off the motion properties, which a
  // kinematic body also carries (mob limbs are kinematic while animated), so
  // a rig's total mass is known BEFORE it goes dynamic — which is when the
  // blast launch needs it (Mob::BlastRadial).
  float BodyMass(uint64_t handle) const;
  // Where the body's MASS is, in world voxels — not `GetTransform`'s pos,
  // which is the voxel lattice's origin corner. A blast that must know how
  // far each limb of a rig is from the charge has to measure from the mass
  // (Mob::BlastRadial): a min corner puts a thigh's "position" at its knee.
  bool BodyCenterOfMass(uint64_t handle, Vec3& outVoxel) const;
  // Linear/angular velocity in voxel units (split halves keep momentum).
  bool GetBodyVelocities(uint64_t handle, Vec3& lin, Vec3& angRadPerSec) const;
  void SetBodyVelocities(uint64_t handle, Vec3 lin, Vec3 angRadPerSec);

  // ---- buoyancy (docs/PLAN_debris_buoyancy.md phase 3) ----
  // One tick of Archimedes for a body crossing a flat liquid surface at
  // `surfaceYVoxel`. Jolt computes the submerged volume from the COLLIDER —
  // exactly, per sub-shape — which is what buys the tilt and the bob: the
  // upward impulse lands at the centre of the submerged part, not at the centre
  // of mass, so a log with one end out of the water rights itself.
  //
  // `buoyancy` is rhoFluid / rhoBody, the ratio Jolt's own parameter means (1 =
  // neutral, >1 floats, <1 sinks). It is the SAME ratio sim_particle.wgsl
  // computes for a voxel in flight, off the same materials.json `density`, so a
  // chip blown off a log and the log itself agree about which way is up.
  //
  // Does NOT wake a sleeping body, which is why it takes the Body rather than
  // the BodyInterface overload: a raft that has come to rest is meant to stay
  // asleep, and re-impulsing it every tick would mean nothing on water ever
  // sleeps again (CLAUDE.md rule 2).
  bool ApplyBuoyancy(uint64_t handle, float surfaceYVoxel, float buoyancy,
                     float linearDrag, float angularDrag, float dt);

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
    // ---- Hinge motor (a door, world/refs_doors.h) ----
    // Max torque the POSITION motor may apply, N*m. 0 = no motor (every rig
    // joint). With a motor the hinge drives toward the target angle set by
    // SetJointMotorTarget (radians from the pose at creation, about `axis`,
    // right-handed), as a spring of `motorFreq` Hz, critically damped.
    float motorTorque = 0.0f;
    float motorFreq = 2.0f;
  };

  // Returns an opaque handle (0 = failure). Joints attached to a body are
  // destroyed automatically when that body is removed.
  //
  // `bodyA == 0` is THE WORLD (Jolt's Body::sFixedToWorld): a door leaf hung
  // on a hinge that nothing can move. Such a joint survives ReplaceBody on its
  // one body like any other (the world side "did not move"), and dies with it.
  uint64_t CreateJoint(uint64_t bodyA, uint64_t bodyB, const JointDesc& desc);
  // A motored hinge's target (radians, see JointDesc::motorTorque); wakes the
  // body. False for a joint that is not a motored hinge or is dead.
  bool SetJointMotorTarget(uint64_t joint, float radians);
  // A hinge's current angle, radians from its creation pose. False if dead
  // or not a hinge.
  bool JointHingeAngle(uint64_t joint, float& outRadians) const;
  bool JointAlive(uint64_t joint) const;
  void DestroyJoint(uint64_t joint);  // <- this is dismemberment
  // Everything that referenced `oldHandle` now references `newHandle`, then
  // the old body is removed. Every joint on the old body is rebuilt against
  // the new one under the SAME joint handle, with the same rest-frame limits,
  // anchored where the OTHER body still says the anchor is; the collision
  // group (one mob's exclusion set) and the object layer (the avatar's
  // exemption) are copied across.
  //
  // For collider rebuilds. RemoveBody destroys attached joints because Jolt
  // asserts otherwise, and every rebuild path — DebrisSystem::RebuildCollider,
  // the burn shrink, the fragment parent — did CreateBody + RemoveBody, so a
  // corpse's torso that lost a sword's worth of voxels lost its neck, both
  // shoulders and both hips in the same call (gate `corpse-intact`). A body
  // comes apart only where something cuts it apart.
  void ReplaceBody(uint64_t oldHandle, uint64_t newHandle);
  // The layer half of ReplaceBody on its own, for a rebuild that re-makes its
  // joints itself (Mob::RebuildLimbBody) and for a SPLIT, where the parent
  // stays. `to` takes `from`'s ROLE, owner and object layer (SetBodyRole), and
  // if `from` was still CLEARING, `to` is added to the list (never in place of
  // `from`: a dead parent is forgotten by the next Step, a live one keeps its
  // own entry). Without this a body rebuilt or split INSIDE the player came
  // back on the plain MOVING layer and shoved them — a burning gobbet that
  // shrinks (DebrisSystem's burn rebuild) did exactly that, every rebuild.
  void CarryLayer(uint64_t from, uint64_t to);
  // WHERE A REBUILT COLLIDER WENT. Every rebuild (ReplaceBody, a limb's carve
  // rebuild, a split's pieces) passes through CarryLayer, which records
  // from -> to in a small ring. Something holding a handle across a hit — the
  // physics grab (game/grab.h) — asks this when its handle dies instead of
  // letting go. Several successors (a split) answer the heaviest one still
  // alive. A successor that was itself rebuilt again is still returned, so the
  // caller can walk the chain; 0 when the record has aged out of the ring.
  // Read by nothing that is hashed: the grab is player input.
  uint64_t Successor(uint64_t handle) const;
  // Joints currently attached to one body / alive in the whole system.
  uint32_t JointCount(uint64_t handle) const;
  uint32_t JointCount() const;
  // ---- WHERE A BODY IS HELD, SO A CUT CAN DECIDE TO LET GO (2026-09-20) ---
  //
  // A corpse is a set of bodies that `Mob::Die` left JOINTED, and until now
  // nothing could take one apart: severing on the loose side is connectivity
  // INSIDE a single body, so a blade could part a corpse's neck completely and
  // the head stayed on. Deciding that needs two things this class already
  // knows and never published — which joints are on a body, and WHERE on that
  // body each one is anchored — so that the flesh at the anchor can be asked
  // whether there is anything left to hold.
  //
  // `anchorLocalVox` is in the body's OWN frame, world voxels (the lattice
  // scale is the caller's business). `other` is the body at the far end.
  struct BodyJoint {
    uint64_t joint = 0;
    uint64_t other = 0;
    Vec3 anchorLocalVox{};
  };
  void JointsOn(uint64_t handle, std::vector<BodyJoint>& out) const;
  // How far body B currently sits from the REST direction its ball joint was
  // built around, in radians (0 when the joint is not a ball joint or is
  // dead). The limit test in selftest_mob.cpp asks this rather than
  // re-deriving the cone frame, which would be a second implementation of it.
  bool JointSwingAngle(uint64_t joint, float& outRadians) const;

  // ---- WHO IS DRIVING THIS BODY (docs/PLAN_struck_matter.md) --------------
  //
  // RIG-POSED (kinematic) or SOLVER-OWNED (dynamic). This is the question that
  // decides what a body does when something hits it — a pose spring for the
  // first, an impulse for the second — and it is asked HERE rather than of
  // `Mob` on purpose: a limp living limb and a corpse limb give the same
  // answer, and neither the melee sweep nor anything else should have to
  // recognise which of the two it is holding. False for a dead handle.
  bool IsBodyDynamic(uint64_t handle) const;
  // ONE BLOW, AT A POINT. `impulse` is kg*m/s along `dir` (need not be unit —
  // it is normalised here), applied at `atVoxel` in world voxels, so a hit off
  // the centre of mass SPINS the body as well as shoving it. That is the whole
  // reason this is not SetBodyVelocity: a sword across a corpse's shoulder
  // should turn it over.
  //
  // Speed-capped by mass exactly as ApplyRadialImpulse is, and for the same
  // reason stated there: impulse/mass on a 0.3 kg hand is a rocket. Silently
  // does nothing to a body that is not dynamic — a kinematic body's transform
  // is written by whoever poses it, so an impulse on one is not merely
  // ineffective, it is a claim about ownership that is false.
  // Returns true if an impulse actually landed.
  bool ApplyImpulseAt(uint64_t handle, Vec3 dir, float impulse, Vec3 atVoxel);

  // ---- mob locomotion plumbing (PLAN §B4) ----
  // Requires the body to have been created with allowKinematic.
  void SetBodyKinematic(uint64_t handle, bool kinematic);
  // Drive a kinematic body toward a pose over dt (gives it real velocity).
  void MoveKinematicBody(uint64_t handle, Vec3 posVoxel, const float quat[4],
                         float dt);
  void SetBodyVelocity(uint64_t handle, Vec3 velVoxelsPerSec);

  // Teleport a body, keeping its rotation and both velocities. A TELEPORT
  // SKIPS COLLISION, so this is not a way to move anything: the one caller is
  // DebrisSystem::UntunnelBody, which is undoing a step that ended somewhere
  // no collider could have stopped it. Returns false if the handle is dead.
  bool SetBodyPosition(uint64_t handle, Vec3 posVoxel);

  // Teleport a body's POSITION AND ROTATION, keeping both velocities. Same
  // "skips collision" caveat as SetBodyPosition, and the same reason to exist:
  // a body whose pose is DERIVED from another body's has no pose of its own to
  // solve for. Mob::DriveWornShells uses it to put a garment exactly on the
  // limb it is strapped to, every tick — MoveKinematicBody could not, because
  // it aims a body at a pose it reaches at the END of the next step, and one
  // tick of lag on a limb falling at 40 m/s is four voxels of daylight between
  // a hood and the head inside it.
  bool SetBodyTransform(uint64_t handle, Vec3 posVoxel, const float quat[4]);

  // ---- A BODY'S COLLISION LAYER IS DERIVED FROM ITS ROLE (W2-N) -----------
  //
  // Callers say WHAT a body is; this class alone decides which Jolt object
  // layer that means (ResolveLayer) and which pairs of layers meet (the pair
  // filter in physics.cpp). Until 2026-09-24 the layer was set imperatively
  // at ~24 sites (SetBodyAvatarLayer, SetBodyPropLayer,
  // ReleaseToWorldWhenClear) and every missed site was its own bug: a rebuilt
  // blade back on MOVING, a thrown flask bursting on the thrower's head, a
  // get-up sword knocked back to MOVING later by a release still pending.
  //
  // THE ROLES, and the layer each resolves to (`owner` = a player proxy
  // handle, kNoOwner, or kAnyPlayer):
  //
  //   role          owner none   owner = proxy P       owner any / unknown
  //   ------------  -----------  --------------------  -------------------
  //   RigLive       MOVING       OWNED(P)              EXEMPT
  //   WornShell     MOVING       OWNED(P)              EXEMPT
  //   Carried       MOVING       OWNED(P)              EXEMPT
  //   HeldProp      PROP         PROP                  PROP
  //   SeveredHold   EXEMPT       EXEMPT                EXEMPT
  //   Debris        MOVING, entered through CLEARING
  //   RigLimp       MOVING, entered through CLEARING
  //   RigDead       MOVING, entered through CLEARING
  //   Thrown        THROWN(P) / THROWN while clearing, then Debris on MOVING
  //   Door          DOOR: bodies + every capsule, never terrain (open leaf)
  //
  // OWNED(P) is the OWNER-SCOPED exemption: exactly MOVING, except that it
  // never meets player P's capsule and P's PlayerPushOut never sees it. Every
  // OTHER player's capsule does meet it: your own body cannot shove you, and
  // somebody else's can. (The old AVATAR layer exempted a body from EVERY
  // player's capsule, which is wrong the moment there are two.) EXEMPT is that
  // old layer, kept for what really is exempt from everyone: a piece inside
  // somebody, and an avatar whose proxy it has not been told
  // (Mob::SetCollisionOwner). PROP meets nothing and is seen by every query.
  // THROWN meets terrain and ordinary bodies only; THROWN(P) also meets bodies
  // and capsules that are not P's.
  //
  // WHY OWNED EXISTS. The player's avatar is drawn around the player's own
  // capsule, so its limbs are permanently interpenetrated with the proxy. On
  // the normal layer that produced a large depenetration push every tick whose
  // direction swung with the gait animation — the player's own body steering
  // them backwards and sideways. Rays and other queries still see these
  // bodies, so laser/damage hits on the avatar are unaffected.
  //
  // CLEARING is the one transition rule, in one place (was
  // ReleaseToWorldWhenClear). Everything that leaves a creature — a severed
  // limb, a cut strap's pauldron, a sword knocked from a hand, a carved-off
  // gobbet, a corpse's limbs, an item dropped from the pack — is created
  // exactly where the creature is, which for the player means INSIDE the
  // capsule proxy. On MOVING that is a deep penetration the solver cannot
  // resolve, and PlayerPushOut turns it into a shove of up to a body-width per
  // tick for as long as the overlap lasts: "my arm came off and I was launched
  // across the field". So a LOOSE role (Debris, RigLimp, RigDead, Thrown)
  // always enters by waiting on EXEMPT — THROWN for a throw, because EXEMPT
  // meets EXEMPT and a flask thrown from the hand hit your own head and burst
  // — and every Step each waiting body whose world AABB is clear of EVERY live
  // proxy (M9.1 P2) settles on its own layer: ordinary debris that can be
  // stood on, kicked and picked up. The check is an AABB test (one lock per
  // body), bounded by kMaxPendingRelease; past that the oldest settles
  // unconditionally rather than the list growing. With no live proxy there is
  // nobody to protect and it settles at once. Re-setting a loose role re-arms
  // the wait. An ATTACHED role (the other five) applies at once — except that
  // a body still clearing, given an attached role some player could feel,
  // keeps clearing and settles on that role's layer when clear.
  //
  // The role and owner live in the Jolt body's user data, so ReplaceBody and
  // CarryLayer take them to a rebuilt collider with no caller involvement. A
  // body this class creates starts as Debris on MOVING, not clearing.
  enum class BodyRole : uint8_t {
    Debris = 0,
    RigLive,
    RigLimp,
    RigDead,
    WornShell,
    HeldProp,
    Carried,
    SeveredHold,
    Thrown,
    // An OPEN DOOR LEAF (world/refs_doors.h): dynamic, hinged to the world,
    // meets bodies and capsules but never terrain (Layers::DOOR).
    Door,
    Count,
  };
  static constexpr uint64_t kNoOwner = 0;
  static constexpr uint64_t kAnyPlayer = (1ull << 56) - 1;
  static bool IsLooseRole(BodyRole r) {
    return r == BodyRole::Debris || r == BodyRole::RigLimp ||
           r == BodyRole::RigDead || r == BodyRole::Thrown;
  }
  static const char* RoleName(BodyRole r);
  void SetBodyRole(uint64_t handle, BodyRole role, uint64_t owner = kNoOwner);
  BodyRole BodyRoleOf(uint64_t handle) const;
  uint64_t BodyOwnerOf(uint64_t handle) const;
  // True while the body waits to be clear of every player (see CLEARING).
  bool BodyClearing(uint64_t handle) const;
  // THE ONE MAPPING, pure. `ownerSlot`: -2 no owner, -1 any/unknown, else the
  // owning proxy's slot (PlayerSlotOf). Returns a Jolt object layer number.
  static int ResolveLayer(BodyRole role, int ownerSlot, bool clearing);
  // The owner-scoping slot a player proxy was given (0..kMaxPlayerProxies-1),
  // or -1 for a dead handle or a proxy past the slot count (which then
  // behaves like the old single PLAYER layer: every OWNED body meets it).
  int PlayerSlotOf(uint64_t proxy) const;
  // Would the SIMULATION let these two bodies' layers meet? The pair filter
  // asked directly (a rig's group exclusions not included). For gates.
  bool LayersCollide(uint64_t a, uint64_t b) const;

  // ---- HeldProp: A HELD WEAPON IS CARRIED, NOT SIMULATED ----
  //
  // The PROP layer generates contacts with NOTHING — no terrain, no debris,
  // no creature, no player proxy — while staying fully visible to ray casts
  // and every other query.
  //
  // WHY THIS EXISTS. A sword in a fist is a kinematic body posed by its
  // wielder's hand every tick and pinned to it by a Fixed joint. Contacts can
  // therefore never move the WEAPON; the only thing they can do is move
  // whatever the weapon is inside. Because a kinematic body reports its rig's
  // mass, an NPC's drawn sword sailed straight past PlayerPushOut's
  // kick-it-aside mass gate and depenetrated the player's capsule every tick
  // it overlapped — standing near an armed NPC shoved you off your feet, and
  // a swing dragged you with it. In the other direction your own blade, swept
  // at swing speed, scattered every dynamic body it passed through: corpses,
  // ragdolls and loose debris flung out of the way by a weapon you were only
  // carrying. Reported as "fights feel clunky and weird".
  //
  // IT COSTS COMBAT NOTHING, because no part of combat was ever routed
  // through these contacts and melee applies no impulses at all:
  //   * the swing's hit detection ray-casts down the blade's own axis
  //     (game/melee.cpp MeleeSweepDamage -> CastRayBody), and rays see this
  //     layer;
  //   * a parry is decided GEOMETRICALLY, segment against segment, by
  //     MobSystem::FindParry — which the comment there says outright, because
  //     a blade collider is a quarter of a voxel thick and the probes could
  //     never answer it;
  //   * Mob::WeaponEdge needs the body's TRANSFORM, which is why this moves a
  //     body between layers rather than removing it.
  //
  // A prop that stops being held — dropped, thrown, knocked loose, severed
  // with the arm, or dynamic under a ragdoll — must come off this layer, or
  // it will fall through the floor. Every one of those paths sets the role it
  // becomes (RigLive / RigLimp / RigDead / SeveredHold / Debris), and a role
  // is a whole layer: there is no "clear the prop flag" left to forget.
  static constexpr size_t kMaxPendingRelease = 256;
  // How many player proxies TickPendingReleases will test a piece against. A
  // bound, not a player cap: it sizes the stack array of AABBs gathered once
  // per tick (rule 2 — the per-piece loop must not grow with the world). LAN
  // co-op is a handful of players; if a proxy past this ever exists, a piece
  // inside it is released a tick early rather than the sweep getting slower.
  static constexpr int kMaxPlayerProxies = 8;
  // How many bodies are still waiting to be released. For the selftest.
  size_t PendingReleaseCount() const { return pendingRelease_.size(); }

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
  // Same, skipping `ignore`. For a ray that STARTS INSIDE a body: Jolt reports
  // a convex shape the origin is in as a hit at fraction 0, so a look ray cast
  // from the player's eye sees the avatar's own head before anything else
  // unless the rig's limbs are excluded (the E look-at prompt).
  uint64_t CastRayBody(Vec3 fromVoxel, Vec3 dirNormalized, float maxDistVoxels,
                       float& fraction,
                       const std::vector<uint64_t>& ignore) const;

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
  //
  // `outWorst` names WHAT pushed hardest, because a bare push length is not a
  // measurement (CLAUDE.md rule 6): the `ragdoll` gate's avatar-on-fire arm
  // reports a number, and telling "a 0.06 kg gobbet summed eight times" from
  // "one 9 kg corpse limb released inside the capsule" by turning features off
  // costs a run per hypothesis. Optional and free when null.
  struct PushSource {
    uint64_t body = 0;
    float massKg = 0;
    float depthVox = 0;
  };
  // Which object layer a body is on: 0 STATIC, 1 MOVING, 2 PLAYER (a proxy
  // with no slot), 3 EXEMPT (from every player; was AVATAR), 4 PROP,
  // 5 THROWN, 8+s PLAYER slot s, 16+s OWNED by player s, 24+s THROWN by
  // player s, -1 dead. The layer is the whole of whether a body can shove the
  // player, so a push that should have been impossible is answered by this
  // (and BodyRoleOf says WHY it is on it).
  int BodyObjectLayer(uint64_t handle) const;
  Vec3 PlayerPushOut(uint64_t handle, Vec3 centerVoxel,
                     PushSource* outWorst = nullptr) const;

  // ---- contact reporting (audio; DESIGN.md §12b) --------------------------
  //
  // One NEW contact from the last Step. Jolt calls OnContactAdded only when a
  // manifold first appears, which is exactly the LANDING moment — a body
  // resting in a pile re-reports through OnContactPersisted, which this
  // deliberately does not listen to, so a settled heap costs nothing.
  //
  // The list is filtered inside the listener before it is stored, because the
  // filter is what bounds it (CLAUDE.md rule 2): contacts below the speed gate,
  // and anything touching a PLAYER proxy, an OWNED body or an EXEMPT one,
  // never become entries. Those layers are excluded by name — your own
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
  // The contacts the filter above keeps OUT of that list because one side is
  // a player's own body (OWNED or EXEMPT layer) or a player CAPSULE — reported
  // here when the other side is a LOOSE body (MOVING / THROWN; not terrain,
  // and not another player-side body). Same speed gate, same step lifetime,
  // own cap. Read only by
  // MobSystem::ApplyContactDamage, so a thrown rock hurts the player the way it
  // hurts an NPC (W2-K) without the player's own footsteps reaching the mixer.
  // Reporting only: the collision response is the pair filter's, unchanged.
  const std::vector<ContactImpact>& OwnedBodyImpacts() const;
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
  // Wake one body. The counterpart to DeactivateBody, and the reason it exists
  // is SettleBodies: a body that is refused a settle for want of ground under
  // it must also be given the chance to FALL, or the refusal just parks it as
  // a permanent sleeping body. WakeNear would do it, but it wakes every body
  // in a radius to solve a question about one.
  void ActivateBody(uint64_t handle);
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
  // `skipSorted` (ascending handles, optional) names bodies the impulse must
  // NOT touch: the limbs a living creature still owns. Those are launched as
  // ONE rig by Mob::BlastRadial — a per-body impulse/mass on a 0.3 kg hand is
  // 170 m/s, and the joints drag the rest of the body after it.
  void ApplyRadialImpulse(Vec3 centerVoxel, float radiusVoxels, float impulse,
                          const std::vector<uint64_t>* skipSorted = nullptr);
  // Wake dynamic bodies whose AABB intersects the given voxel-space sphere
  // (terrain changed under them).
  void WakeNear(Vec3 centerVoxel, float radiusVoxels);

  uint32_t NumActiveBodies() const;

  // ---- THE RUNAWAY-RIG NET (the long note is in physics.cpp) ---------------
  //
  // 46e3848 closed the case where a body's velocity became GARBAGE. This is
  // the case where it stays entirely legal and is still a bug: a jointed rig
  // whose solver gains energy every step until every piece sits AT Jolt's own
  // clamp, spinning at 47.1 rad/s and never stopping. Nothing reports it,
  // because 47.1 rad/s is a number physics is allowed to produce.
  //
  // What the caller gets is the account, so "the corpse exploded" can be
  // answered with numbers instead of a bisect: how many bodies were at their
  // ceiling on the last step, how many the net had to slow down, how many it
  // had to cut free, and the worst single Update in wall clock.
  struct RunawayProbe {
    uint32_t hot = 0;       // bodies AT their own clamp on the last step
    uint32_t damped = 0;    // ...that stayed there long enough to be slowed
    uint32_t cut = 0;       // ...and then long enough to have their joints cut
    uint32_t repaired = 0;  // transforms put back inside a finite world
    float peakSpeedVox = 0.0f;  // fastest and fastest-spinning body seen since
    float peakSpinRad = 0.0f;   // the probe was last reset
    double worstStepMs = 0.0;   // longest single Update, wall clock
  };
  const RunawayProbe& Runaway() const { return runaway_; }
  // The last Update: its wall clock and what the narrow phase produced.
  struct StepStats {
    double ms = 0.0;
    uint32_t manifoldsDyn = 0, pointsDyn = 0;        // body vs body
    uint32_t manifoldsStatic = 0, pointsStatic = 0;  // body vs terrain/static
  };
  const StepStats& LastStep() const { return lastStep_; }
  void ResetRunawayProbe() { runaway_ = RunawayProbe{}; }
  // The ceilings every dynamic body this class creates is born with, so a test
  // can assert against the engine's number rather than a copy of it.
  static float MaxBodySpeedVox();
  static float MaxBodySpinRad();
  // ...and the collider rules CreateDebrisBodyXf builds under, for the
  // `big-body-collider` gate: the most sub-shapes a debris body gets, and the
  // thinnest extent (world voxels) at which a body steps discretely.
  static int ColliderBoxBudget();
  static float DiscreteMinExtentVox();
  // True when Jolt shape-casts this body's steps (EMotionQuality::LinearCast).
  bool UsesLinearCast(uint64_t handle) const;

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
  // EVERY live player proxy (CreatePlayerBody), so CLEARING can ask "is this
  // body still inside A player" without the caller threading the handle
  // through every mob. Empty in a world with no player. Each proxy also holds
  // an owner-scoping SLOT, encoded in its object layer (PlayerSlotOf).
  //
  // A LIST, NOT A SLOT (M9.1 P2). It was one handle, and the newest proxy won:
  // a second session's capsule would have silently stolen the protection from
  // the first, and a severed limb inside player 0 would have been dropped into
  // the MOVING layer the instant player 1 spawned -- which reads as a piece of
  // somebody shoving them across the room. With ONE entry the behaviour is
  // bit-identical to the single handle it replaces: the two readers below tested
  // one proxy and now test every proxy, and "every" over a list of one is the
  // same test. RemoveBody erases, so a selftest that makes and removes several
  // proxies (selftest_phys/selftest_mob do) does not grow this without bound.
  std::vector<uint64_t> playerBodies_;
  std::vector<uint64_t> pendingRelease_;
  // CarryLayer's from -> to record (Successor). A ring, so it never grows:
  // 1024 rebuilds is far more than can happen between two grab servo ticks,
  // even with a forest fire rebuilding burning bodies.
  struct Rebuilt { uint64_t from = 0, to = 0; };
  std::vector<Rebuilt> rebuilt_ = std::vector<Rebuilt>(1024);
  uint32_t rebuiltHead_ = 0;
  // World-space AABB of a live body, metres. False for a dead handle.
  bool WorldBounds(uint64_t handle, float outMin[3], float outMax[3]) const;
  void TickPendingReleases();
  // SetBodyRole's halves: the owner's slot (-2 none, -1 any/unknown), whether
  // any live proxy exists, and the layer a clearing body settles on.
  int OwnerSlot(uint64_t owner) const;
  bool AnyLivePlayer() const;
  void SettleBody(uint64_t handle);
  void AddPending(uint64_t handle);
  // ---- the FP-overflow net (see the long note in physics.cpp) ----
  // Called on every DYNAMIC body the moment it is added: floors a principal
  // moment of inertia that a decomposition left at (or below) zero, which is
  // the only way omega can reach 1e19 rad/s in a single solver step.
  // `what` names the creator for the report. No-op in the normal case.
  void GuardBodyInertia(uint32_t bodyIndexAndSeq, const char* what);
  // True when both vectors are finite and small enough that squaring them
  // cannot overflow. False reports once (rate-limited) and the caller drops
  // the write.
  bool VelocityIsSane(Vec3 linVox, Vec3 angRad, const char* what);
  // Called from Step() before Update(): walks the ACTIVE rigid bodies and
  // neutralises (and names) any whose stored velocity is already past what
  // Jolt's own clamp could have produced. Zero cost when nothing is wrong.
  void SweepInsaneVelocities();
  // Called from Step() straight after the sweep above: finds the bodies that
  // are sitting AT their velocity ceiling rather than passing through it,
  // slows them, and — if they are still there a moment later — cuts the joints
  // that are driving them and puts them to sleep. See the note in physics.cpp.
  void SweepRunawayRigs();
  RunawayProbe runaway_{};
  StepStats lastStep_{};
  // Jolt body INDEX -> consecutive-ish steps spent at the ceiling. Climbs by
  // one per hot step and falls by one per quiet one, so a body that is being
  // DRIVEN escalates while a body that was merely thrown hard decays back to
  // nothing. Bounded by the active list; entries are dropped at zero.
  std::unordered_map<uint32_t, uint16_t> hotSteps_;
  int runawayReports_ = 0;  // rate limit on the three reporters above
  // SANDVOX_PHYS_FAULT: the deliberate blow-up that proves the two above
  // (and the Jolt FP-exception setting) actually do something. No-op unset.
  void InjectPhysFault();
  int insaneReports_ = 0;  // rate limit on the two reporters above
  int faultStep_ = 0;      // SANDVOX_PHYS_FAULT step counter
  // ReplaceBody's per-joint step: rebuild `joint` with `newBody` standing in
  // for `oldBody` on whichever side it was. False if nothing was rebuilt.
  bool RetargetJoint(uint64_t joint, uint64_t oldBody, uint64_t newBody);
};
