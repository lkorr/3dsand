#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "math3d.h"

// Layered skeletal animation runtime (docs/PLAN_voxel_editor.md §B).
//
// DETERMINISM BOUNDARY: everything in this header is CPU-float PRESENTATION
// state. Poses, IK, springs and gait phases are never hashed and never touch
// the grid. Mob code turns the resulting model-space transforms into Jolt
// kinematic targets; the only grid contact is the existing BrushOp/CellOp path
// in mob.cpp (blood, settle-back). See DESIGN.md §on bodies/debris.
//
// This file deliberately knows nothing about Jolt or the World — it is the
// schema-facing half so the editor (wave 2b) can share it, while mob.cpp keeps
// the physics plumbing.

// ---- math -------------------------------------------------------------------

struct Quat {
  float x = 0, y = 0, z = 0, w = 1;
};

Quat QuatMul(const Quat& a, const Quat& b);
Quat QuatConj(const Quat& q);
Quat QuatAxisAngle(Vec3 axis, float angle);
Quat QuatNormalize(const Quat& q);
Vec3 QuatRotate(const Quat& q, Vec3 v);
Vec3 QuatRotateInv(const Quat& q, Vec3 v);
float QuatDot(const Quat& a, const Quat& b);
// Shortest-arc nlerp. Sign-fixes `b` against `a` first; normalized result.
Quat QuatNlerp(const Quat& a, const Quat& b, float t);
// Shortest-arc slerp, falling back to nlerp when the arc is tiny.
Quat QuatSlerp(const Quat& a, const Quat& b, float t);
// Minimal rotation taking `from` to `to` (both need not be normalized).
Quat QuatFromTo(Vec3 from, Vec3 to);
// Euler DEGREES -> quat, applied X then Y then Z. The order is stated here
// once and shared by everything that authors a rotation as three numbers in
// JSON (rig sockets, item grips), because "which order were those Euler
// angles in" is otherwise decided independently at each call site and they
// disagree the first time an axis is non-zero.
Quat QuatFromEulerDeg(Vec3 deg);

struct Transform {
  Quat rot;
  Vec3 pos;
};

// ---- schema -----------------------------------------------------------------

enum class Ease : uint8_t {
  Instant, Linear, QuadIn, QuadOut, QuadInOut, CubicIn, CubicOut, CubicInOut
};
Ease ParseEase(const std::string& s);
float ApplyEase(Ease e, float t);

enum class ClipMode : uint8_t { Override, Additive };

// Fused keyframe: rotation and position sampled together (the plan doc is
// explicit that clips are fused quat+pos keys, not per-channel Euler tracks).
struct AnimKey {
  int32_t tMs = 0;
  Quat rot;
  Vec3 pos;
  bool hasRot = false;
  bool hasPos = false;
  Ease ease = Ease::Linear;
};

struct AnimTrack {
  int part = -1;                 // index into the def's part list
  std::vector<AnimKey> keys;     // sorted by tMs
};

struct AnimClip {
  std::string name;
  int32_t durationMs = 0;
  bool loop = false;
  ClipMode mode = ClipMode::Override;
  int32_t blendInMs = 0, blendOutMs = 0;
  std::vector<uint8_t> mask;     // per-part 0/1; empty = affects all parts
  std::vector<AnimTrack> tracks;
};

// Holden's closed-form spring (unconditionally stable at any dt). One per
// axis of a part's local rotation offset; a part is KEYED or JIGGLED, never
// both, so springs never fight a clip for the same channel.
struct SpringDef {
  float halflife = 0.15f;        // seconds to halve the error
  float gain = 1.0f;             // how strongly motion drives the goal
  float maxAngle = 0.7f;         // clamp (radians)
};

struct GaitDef {
  bool present = false;
  float cadence = 2.2f;          // stride frequency multiplier
  float strideBias = 0.35f;      // forward foot lead, in leg lengths
  float leadTime = 0.2f;         // seconds of velocity lookahead
  float stepThreshold = 0.6f;    // drift (in leg lengths) that unplants a foot
  float stepDuration = 0.22f;    // seconds of swing
  float stepHeight = 0.25f;      // arc peak, in leg lengths
  float rideHeight = 0.9f;       // body above the foot plane, in leg lengths
  float bobAmp = 0.06f, bobFreqMul = 2.0f;
  float swayAmp = 0.05f, rollAmp = 0.09f;
  float spineCounter = 0.7f;
  float phaseLag = 0.05f;        // seconds of lag per hierarchy level
  // Leg groups: only ONE group may be swinging at a time. This single
  // constraint IS the gait state machine — it generalizes to any leg count
  // (biped = two singleton groups, quadruped = diagonal pairs) without a
  // per-gait table, and it degrades gracefully when a leg is severed.
  std::vector<std::vector<int>> groups;  // part indices
};

// How a body STEERS, as opposed to how its feet land (GaitDef). The split is
// deliberate: gait is a property of the leg rig and is mirrored by the editor's
// preview, whereas these are the physical limits an AI's steering output is
// clamped against. A future behaviour tree writes a desired heading and a
// desired speed; nothing above this layer ever gets to set body facing
// directly, which is what keeps "turn instantly to face the player" from
// becoming possible by accident.
struct LocomotionDef {
  // Peak yaw rate, radians/sec. The default is ~206 deg/s: brisk enough that a
  // mob does not feel sluggish, slow enough that the turn reads as a turn.
  float turnRate = 3.6f;
  // Yaw acceleration, radians/sec^2. Rate is ramped rather than stepped so a
  // reversal eases out of the old direction instead of snapping to full rate,
  // which is the difference between a creature turning and a turret slewing.
  // <= 0 means "no ramp": jump straight to turnRate (cheap mobs, insects).
  float turnAccel = 18.0f;
  // Facing error (radians) beyond which forward drive is scaled down, and the
  // error at which it reaches zero. A mob that needs to turn 180 deg should
  // pivot roughly in place rather than carving a wide arc through the wall it
  // just bounced off; a mob 10 deg off course should not slow at all.
  float driveAlignFull = 0.5f;   // <= this: full speed
  float driveAlignZero = 2.0f;   // >= this: pivot in place
  // Turn rate multiplier while at full forward speed. Real bodies turn tighter
  // when slow; 1.0 disables the coupling.
  float turnRateMoving = 0.55f;
};

// Locomotion state selected by DISMEMBERMENT: each rule pairs a predicate over
// the severed parts with how the survivor keeps moving (a looping clip, a speed
// penalty, whether the gait still owns the legs). Rules are evaluated in
// authored order and the FIRST match wins, so a sidecar lists the most-maimed
// state first ("both legs gone -> crawl" before "a leg gone -> limp"). No match
// = normal locomotion. All predicate fields are AND-ed; an empty predicate
// never matches (it would otherwise shadow every rule after it).
struct AnimStateRule {
  std::string name;
  std::vector<int> missingAll;    // every one of these parts is severed
  std::vector<int> missingAnyOf;  // at least one of these parts is severed
  int minChainsLost = 0;          // >= N IK chains disabled ("legs of use lost")
  std::string clip;               // looping loco clip, crossfaded on entry
  float speedScale = 1.0f;        // walk-drive speed multiplier
  // The clip owns the pose: suppress gait scheduling, IK, the legacy phase
  // swing and the pelvis bob while this state is active. A crawl keyed on the
  // torso fights all four otherwise.
  bool disableGait = false;
  // Animated body height relative to the walk drive's ground (world voxels)
  // while the gait's foot-derived height is suppressed.
  float bodyYOffset = 0.0f;
};

enum class IkSolver : uint8_t { TwoBone };

struct IkChain {
  std::string tag;
  std::vector<int> parts;        // root..tip, parents-first
  int effector = -1;
  Vec3 pole{0, 0, 1};            // explicit bend-plane hint (never derived)
  IkSolver solver = IkSolver::TwoBone;
  float weight = 1.0f;           // driven to 0 when the chain loses a part
};

struct FlipbookFrame {
  int part = -1;
  int model = 0;                 // .vox model index to swap in
  int32_t durationMs = 100;
};

struct Flipbook {
  std::string name;
  bool loop = true;
  std::vector<FlipbookFrame> frames;
};

// ---- ball-joint pose limit (the shoulder) -----------------------------------
//
// WHAT A SHOULDER ACTUALLY IS. The one-axis clamp below (`poseAxis`/`poseMin`/
// `poseMax`) is the right shape for a knee and the wrong shape for a shoulder:
// it bounds ONE component of the rotation and leaves the other two free, so a
// mouse-driven arm could still be raked straight back or folded across the
// chest and through the ribs as long as its X component stayed legal. A cone
// is not right either — the reachable set of a real shoulder is very nearly
// "the whole sphere MINUS the region behind the torso MINUS a wedge across the
// midline", and no cone centred anywhere describes that without also refusing
// the arm-straight-out-to-the-side pose, which is ordinary.
//
// So the limit is stated as what it is: HALF-SPACES ON WHERE THE BONE MAY
// POINT, plus a bound on how far the bone may roll about itself.
//
//   reachNormal[k] / reachSin[k]:  (bone direction) . normal <= sin(limit)
//                                  "no more than <limit> degrees past this
//                                   plane", authored in degrees.
//   twistMin/twistMax:             roll about the bone itself. Load-bearing
//                                  even though the IK barely twists on its
//                                  own: the shoulder's roll is what aims the
//                                  ELBOW's hinge plane, so an unbounded roll
//                                  puts a correctly-hinged forearm into the
//                                  ribs.
//
// EXACTLY TWO NORMALS, AND THEY MUST BE PERPENDICULAR. The nearest legal point
// on the sphere has a closed form when the constraint normals are orthonormal
// (clamp each component, complete the third from the unit-length identity) and
// does not when they are not — sequential projection onto two tilted planes
// re-violates the plane it just left. Two perpendicular planes say "not behind"
// and "not across", which is the whole anatomical claim; the loader rejects a
// non-perpendicular pair rather than silently solving the wrong problem.
struct PoseBallLimit {
  bool has = false;
  Vec3 bone{0, -1, 0};           // bone direction in the part's own rest frame
  int reachCount = 0;            // 0..2
  Vec3 reachNormal[2]{};         // unit, rest frame, mutually perpendicular
  float reachSin[2]{};           // sin of the authored degrees
  bool hasTwist = false;
  float twistMin = 0, twistMax = 0;  // radians about `bone`, 0 = rest
};

// One rigged part. Mirrors MobLimbDef's rig half; mob.cpp owns the Jolt half.
struct AnimPart {
  std::string name;
  std::string tag;               // "leg", "arm", ... — chains/gait query by tag
  int parent = -1;               // MUST be < own index (parent-before-child)
  Transform rest;                // local rest transform
  Vec3 anchorLocal{};            // joint pivot in this part's own local frame
  bool hasSpring = false;
  SpringDef spring;
  // legacy no-IK fallback (dummy.json): sinusoidal swing about `axis`
  Vec3 axis{1, 0, 0};
  float swingAmp = 0, swingPhase = 0;
  // ---- POSE-SPACE joint limit (optional; sidecar "poseLimit") --------------
  // A RANGE OF MOTION FOR THE ANIMATION, which is a different thing from the
  // ragdoll limits next to it in MobLimbDef. `minAngle`/`maxAngle` and the
  // swing-twist cone are Jolt constraints: they bound a DYNAMIC body, and a
  // live limb is KINEMATIC — the pose pipeline writes its transform every tick
  // and the solver never sees a constraint at all. So nothing whatsoever
  // stopped the IK from raking a thigh out behind the body or folding it up
  // through the pelvis; the only guard was a selftest noticing afterwards.
  //
  // This clamps the SOLVED pose instead, about the part's own REST frame, so
  // an anatomically impossible leg is unrepresentable rather than merely
  // untested. Authored in degrees, stored in radians. Applied after the IK and
  // before the pose is submitted (AnimClampPoseLimits).
  bool hasPoseLimit = false;
  Vec3 poseAxis{1, 0, 0};        // in the part's own rest frame
  float poseMin = -3.14159265f;  // radians, about poseAxis, 0 = rest
  float poseMax = 3.14159265f;
  // HINGE: one degree of freedom, not one BOUNDED degree of freedom.
  //
  // Without this the clamp above bounds the component about `poseAxis` and
  // leaves the swing off that axis untouched — which is what a knee wants (the
  // solver never puts much there and a hard projection would fight it) and
  // emphatically not what an elbow wants. The two-bone IK bends about an axis
  // it derives in MODEL space from the chain's pole vector, and that axis only
  // coincides with the forearm's own hinge axis while the shoulder is in pure
  // flexion or pure abduction; at any blend of the two the solver opens the
  // elbow sideways, which is the "arm folds through itself" pose.
  //
  // With it, the part's rotation relative to its parent is forced to be a pure
  // rotation about `poseAxis` in [poseMin, poseMax] — the off-axis swing is
  // DISCARDED, not clamped. The hand then misses the IK target by however much
  // the solver's plane disagreed with the joint's, which is the correct trade:
  // an elbow is a hinge, and a reach it cannot make is a reach it cannot make.
  bool poseHinge = false;
  PoseBallLimit poseBall;
};

// The whole rig, shared by all instances of a def (immutable after load).
struct AnimSkeleton {
  std::vector<AnimPart> parts;   // stored parent-before-child (FLATTEN needs it)
  std::vector<AnimClip> clips;
  std::vector<IkChain> chains;
  std::vector<Flipbook> flipbooks;
  std::vector<AnimStateRule> states;  // dismemberment locomotion, first match wins
  GaitDef gait;
  LocomotionDef loco;
  int FindPart(const std::string& name) const;
  int FindClip(const std::string& name) const;
  // True when the storage order guarantees parent index < child index.
  bool ParentsFirst() const;
};

// ---- per-instance runtime state --------------------------------------------

struct ClipInstance {
  int clip = -1;
  float timeMs = 0;              // playhead; WRAPS for a looping clip
  // Time since this instance started, which does NOT wrap. The blend-in reads
  // this rather than timeMs so a looping clip fades in once instead of
  // re-fading at the top of every cycle — see ClipFade in anim.cpp.
  float ageMs = 0;
  float weight = 1.0f;           // requested weight before blend in/out
  bool stopping = false;         // blending out, remove at weight 0
  float fade = 0;                // current blend-in/out factor 0..1
  // PLAYBACK RATE, multiplied into the playhead only — never into `ageMs`.
  //
  // A clip's authored period is right for exactly ONE speed. The walk and run
  // arm swings are derived from the runtime's step model at walk pace and at
  // sprint pace (scripts/gen_human.py arm_cycle_ms), which leaves every speed
  // BETWEEN them — most of the speeds actually played — with arms cycling at a
  // rate the feet do not share, so they drift in and out of phase. The avatar
  // sets this from the live stride rate so one authored cycle spans one stride
  // at any pace.
  //
  // `ageMs` is deliberately excluded: it is the blend-in's clock and a blend is
  // measured in real seconds, not in stride fractions.
  float rate = 1.0f;
};

struct FootState {
  bool valid = false;            // false when the leg is gone
  bool swinging = false;
  Vec3 planted{};                // world-space planted position
  Vec3 swingFrom{}, swingTo{};
  float swingT = 0;              // 0..1 through the step
  float legLength = 1.0f;
  // Material under swingTo, captured by the ground probe when the target was
  // chosen. Presentation only (footstep sounds): sampled at probe time rather
  // than re-read at touchdown so the sound matches the surface the foot was
  // actually aimed at, even if the world changed underneath mid-swing.
  uint32_t swingMat = 0;
};

struct SpringState {
  Vec3 x{}, v{};                 // current offset (radians per axis) + velocity
};

struct FlipbookState {
  int book = -1;
  int32_t elapsedMs = 0;
  int frame = -1;
};

// Everything one animated instance needs between frames.
struct AnimState {
  std::vector<ClipInstance> clips;
  std::vector<Transform> local;   // stage 1-3 output
  std::vector<Transform> model;   // stage 4-5 output (model space)
  std::vector<uint8_t> partAlive; // 0 = severed; IK weight and gait skip it
  std::vector<FootState> feet;    // parallel to skeleton.chains
  std::vector<SpringState> springs;  // parallel to skeleton.parts
  FlipbookState flipbook;
  int locoState = -1;             // index into skeleton.states, -1 = normal
  float gaitPhase = 0;
  Vec3 lastPos{};
  Vec3 velocity{};
  bool initialized = false;
};

// ---- pipeline ---------------------------------------------------------------

// Dismemberment state selection: the first rule in sk.states whose predicate
// holds against st.partAlive (and the chain liveness derived from it), or -1
// for none. Pure query — the caller owns reacting to a change (starting and
// stopping loco clips lives in mob.cpp, next to the rest of clip control).
int AnimSelectState(const AnimSkeleton& sk, const AnimState& st);

// Stage 1-3. Samples every active clip, blends OVERRIDE layers with
// weight-normalized nlerp (rest-pose fallback below kBlendEpsilon), then
// applies ADDITIVE layers on top of the normalized result. Advances clip
// times by dt and retires finished non-looping clips.
void AnimSampleAndBlend(const AnimSkeleton& sk, AnimState& st, float dt);

// Stage 4. One linear parent-before-child pass; requires sk.ParentsFirst().
void AnimFlatten(const AnimSkeleton& sk, AnimState& st);

// THE TORSO SERVES THE SWING (stage 3.5 — writes st.local, so it must run
// BEFORE AnimFlatten). Twists the spine `yawRight` radians toward the rig's
// own RIGHT and raises the chest by `pitchUp`, split across the parts tagged
// "spine" excluding `rootLimb` — the head-look's distribution law exactly,
// and for the head-look's reason (rotating the root yaws the whole rig, legs
// and all; see avatar.cpp's mina note). Both drivers call it with the
// WeaponPose's torso fields, which the melee driver has already scaled by the
// pose weight, so 0/0 — every fabricated gate pose — is a no-op. The
// weapon-arm solve downstream picks the moved shoulder up for free because it
// solves against the LIVE chain-root joint.
void AnimApplySpineTwist(const AnimSkeleton& sk, AnimState& st, float yawRight,
                         float pitchUp, int rootLimb);

// Stage 5. Two-bone analytic IK in MODEL space, run as a POST-PROCESS on the
// flattened pose — never as a blended layer, because blending IK results
// destroys the exact end-effector placement that is the whole point of IK.
// `targetModel` is the desired effector position in model space.
void AnimSolveTwoBone(const AnimSkeleton& sk, AnimState& st, const IkChain& chain,
                      Vec3 targetModel, float weight);

// A HINGE WHOSE PLANE IS STEERED, for the duration of one solve.
//
// `AnimPart::poseAxis` is a fact about the joint stated in the part's own rest
// frame, and for a knee that is the whole story: the leg IK's bend plane is
// fixed by an authored pole and the two agree by construction. A MOUSE-DIRECTED
// arm is the case that breaks it. The stroke driver (game/melee.h) rotates the
// weapon arm's bend plane into the plane of the cut, so the plane the solver
// actually bends in moves every tick — and the authored axis, being fixed in
// the upper arm's frame, no longer coincides with it. The hinge clamp then does
// exactly what it promises and DISCARDS the off-axis swing, which throws away
// most of the solve and leaves the hand short of the target: a horizontal cut
// collapses back into a forward jab.
//
// The anatomically honest fix is to roll the SHOULDER until the elbow's own
// hinge plane contains the cut — a real arm aims its elbow that way, and the
// PoseBallLimit note above says so. Rolling the upper bone about its own axis
// does not move the elbow, but it does change the two-bone solution (the bend
// plane is determined by the pole), so doing it after the solve means resolving
// the chain against a moving constraint. This is the same thing expressed as
// data instead: the joint stays a ONE-DEGREE-OF-FREEDOM hinge with its authored
// 0..130 degree range — what changes is which plane that degree of freedom
// lives in, which is precisely what shoulder roll buys and which is invisible
// on a near-cylindrical upper arm. The RANGE, the part of the limit that keeps
// the pose humanly possible, is never touched.
//
// `blend` fades the override toward the authored axis so a take-over and a
// hand-back do not pop; 0 is "authored axis", 1 is "the solved plane".
struct PoseAxisOverride {
  int part = -1;        // index into sk.parts; -1 = inactive
  Vec3 axis{1, 0, 0};   // replacement hinge axis, in the part's REST frame
  float blend = 0.0f;   // 0..1 toward `axis`
};

// Stage 6. Clamp every part carrying a `poseLimit` back inside its authored
// range of motion, then re-flatten the affected subtrees so children follow.
// Runs AFTER all IK, because the IK is what puts a joint outside its range;
// running it before would clamp a pose the solver is about to overwrite.
//
// Works purely in st.model[], the frame the IK writes, and deliberately does
// NOT write st.local[] — stage 1 reseeds every local from the rest pose on the
// next frame, so anything stored there would be discarded unread. This is the
// same reason AnimSolveTwoBone leaves local alone.
//
// Parts with no limit are untouched, so this is a no-op on every rig that
// authors none.
void AnimClampPoseLimits(const AnimSkeleton& sk, AnimState& st,
                         const PoseAxisOverride* overrides = nullptr,
                         int overrideCount = 0);

// THE SIGNED ANGLE OF `q` ABOUT `axis`, WRAPPED INTO (-pi, pi]. False when
// there is no recoverable angle (q is a half turn about something
// perpendicular to `axis`).
//
// PUBLIC BECAUSE TWO PLACES MUST AGREE ABOUT IT, which is the "two places that
// must agree" rule in CLAUDE.md with a bug attached. The hinge clamp reads a
// joint's bend with it; `Mob::ApplyWeaponArm` reads the SAME bend to decide
// which sense of the steered hinge axis makes the authored [min, max] describe
// it. Those were two copies of the arithmetic and they differed in exactly one
// respect: this one wraps and the other did not.
//
// A quaternion in the far hemisphere (`w < 0`) makes `2 * atan2(...)` come out
// above pi, so the un-wrapped copy read a bend of +4.89 rad as POSITIVE and
// kept the axis, while the clamp wrapped the same rotation to -1.39 rad, found
// it below the elbow's authored `min` of 0, and clamped it straight — throwing
// the forearm 1.39 rad off the solve on exactly the ticks a committed
// horizontal cut passed through. Measured in `swing-plane` A: the DRIVER'S arc
// was planar to 1.09 voxels while the posed sword was 10.58 voxels off it and
// 1.52 rad out of level, which reads as "the swing wanders" and is really one
// missing wrap.
bool AnimHingeAngleAbout(const Quat& q, const Vec3& axis, float* out);

// Holden spring integration for one part's local rotation offset.
void AnimSpringStep(const SpringDef& def, SpringState& s, Vec3 goal, float dt);

// Integer-tick flipbook frame lookup (render-only, but kept integer so it
// stays reproducible frame-to-frame). Returns the frame index or -1.
int AnimFlipbookFrame(const Flipbook& fb, int32_t elapsedMs);

constexpr float kBlendEpsilon = 0.1f;

// Spring goal scale, applied to a body's velocity AFTER it has been divided by
// that def's own top speed. Shared by both animation drivers (mob.cpp and
// avatar.cpp) so `SpringDef::gain` means the same thing — radians of lag at
// full speed — no matter which one is driving the rig. Against raw voxels per
// second it does not: the player avatar moves an order of magnitude faster than
// a critter, and the same authored gain pinned its head at maxAngle forever.
constexpr float kSpringVelScale = 0.6f;
