// ONE POSE PIPELINE FOR EVERY CREATURE (game/pose.h; rule-unification W2-L).
//
// Moved here from PlayerAvatar::UpdateAnimation / UpdateGait / UpdateAirPose /
// UpdateAirDrive / ApplyAirArms / SyncStrideClock (avatar.cpp) and
// MobSystem::UpdateAnimation / UpdateGait (mob.cpp), which were two copies of
// one pipeline. The player's copy is the base: every one of its departures from
// the NPC copy was a documented bug fix (the stride budget, the frozen swing
// target, the landing snap, the per-call velocity blend, the two clocks, the IK
// switch), so the NPCs adopt them and the player's pose does not move. What an
// NPC kept is what is genuinely forced by who owns its body — the height and
// the tilt, which are PoseInputs fields — plus the NPC-only stages that are
// data-gated anyway (the chase clip, the legacy swingAmp layer, flipbooks).
// DESIGN.md "One pose pipeline" is the account.
#include "game/mob.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "game/player.h"   // Player::kHalfXZ: the ledge-hang palm setback
#include "sim/scale.h"
#include "sim/tuning.h"

namespace {

inline Quat AxisAngle(Vec3 axis, float a) { return QuatAxisAngle(axis, a); }
inline Quat Mul(const Quat& a, const Quat& b) { return QuatMul(a, b); }
inline Vec3 Rotate(const Quat& q, Vec3 v) { return QuatRotate(q, v); }
inline Vec3 RotateInv(const Quat& q, Vec3 v) { return QuatRotateInv(q, v); }
// Smoothstep of an already-normalised parameter, clamped to 0..1.
inline float Smooth01(float x) {
  x = std::clamp(x, 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
}
// Smooth01 over the span [a, b] of `x`.
inline float SmoothSpan(float a, float b, float x) {
  return Smooth01((x - a) / std::max(b - a, 1e-4f));
}

// Ceiling on the gait's velocity lookahead, in leg lengths. `leadTime` is a
// DURATION, so the unclamped offset grows linearly with speed and blows past
// what a two-bone chain can reach — see the note at the clamp in StepGait.
// Just under 1 keeps the planted foot inside the leg's reach annulus even at a
// full-lean sprint, so the IK poses a bent leg instead of a straight pointer.
constexpr float kMaxLeadLegLengths = 0.9f;

// Floor on the speed scaling of the swing, and an absolute floor on the swing
// itself. Together they stop a near-stationary step from swinging for seconds
// and a sprint from snapping the foot across with no visible arc.
constexpr float kMinSwingScale = 0.35f;
constexpr float kMinSwingSeconds = 0.09f;

// THE STRIDE BUDGET — why the feet trailed no matter how the gait was tuned.
//
// Only ONE leg may swing at a time, so during a swing the body advances
// `speed * swingDuration` while the swinging foot advances at most its reach:
// the stance point plus the capped lead, i.e. ~kMaxLeadLegLengths * legLength
// ahead of where the body will be. If the body's travel exceeds that, the foot
// lands BEHIND where it lifted off relative to the body, every single step. The
// error is cumulative and unbounded, so the planted foot ratchets backwards
// until the leg is straight and pointing away — and since a foot can then never
// get back out in front, the legs sit permanently behind the character instead
// of alternating fore and aft.
//
// The numbers on this rig at kVoxelMeters 0.10 (walk 35 world vox/s, legLength
// ~5.79 vox): the body covers 35 * 0.171 = 6.00 voxels per swing against a
// 5.21-voxel lead cap. Net -0.79 voxels PER STEP. No cadence, threshold or
// lead-time value can fix that, because the budget itself is negative — which
// is why tuning it repeatedly changed nothing.
//
// It is the same bug the NPC gait had and never named: `ai-slope` found a foot
// "welded to the bottom of the hill" while the body climbed, because a flat
// `stepDuration` swing at two voxels a tick loses ground every step. The NPCs
// adopted this bound with the rest of the player's gait in W2-L.
//
// So bound the swing by the budget instead of hoping a constant fits: the
// duration is whatever keeps the body's travel inside the distance the foot can
// actually gain. Expressed as a fraction so the foot lands with margin rather
// than exactly at full extension (a foot that lands at maximum reach is a
// straight, locked leg — the pose we are trying to avoid).
constexpr float kSwingTravelFrac = 0.7f;

// THE STANCE RESERVE — how much of the leg's reach is kept in hand.
//
// A two-bone chain asked for exactly L1 + L2 is a locked, straight leg, and
// AnimSolveTwoBone clamps at dMax = L1 + L2 - eps, so a target at or past full
// extension produces the SAME pose for every target beyond it. Solving at 97%
// leaves a real bend at the knee in the neutral stance and keeps the solve away
// from the numerically nasty end of the acos.
constexpr float kStanceReachFrac = 0.97f;

// Ceiling on the stance crouch, in leg lengths. The crouch below is derived
// from the geometry and is well-behaved, but `strideBias` is authored data —
// a rig that asks for a stride longer than its own leg would otherwise drive
// the pelvis into the floor rather than simply failing to reach.
constexpr float kMaxCrouchLegLengths = 0.30f;

// Half-life on the stance crouch. Short on purpose: the crouch demand
// oscillates once per STEP now (see the stance note in StepGait), and a
// half-life anywhere near the step period averages that oscillation away and
// gives back the permanent half-crouch it replaced.
constexpr float kCrouchHalflife = 0.05f;

// ...and what it becomes at a full stump drag, where that same oscillation is
// the hop rather than the walk. Longer than any step period this rig can run,
// so what reaches the pelvis is the crouch's AVERAGE and not its swing. See the
// commit site in StepGait for why a one-legged pump is not a gait.
constexpr float kDragCrouchHalflife = 0.55f;

// ...and how far the STANDING knee bends to let the stump reach the floor, in
// leg lengths, at a full drag. Under the kMaxCrouchLegLengths ceiling above on
// purpose — this is a limp, not a squat. A FLOOR under the walking crouch when
// the driver pins the height; a sink after the ground-authority clamp when the
// feet derive it. The two drivers' copies were both 0.15 and are now this one.
constexpr float kDragSinkLegLengths = 0.15f;

// The HELD crouch (Ctrl, tuning.json player.crouchKneeDrop): its ceiling in
// leg lengths and its half-life. The ceiling keeps an authored drop from
// folding the leg past what the two-bone solve can pose; the half-life is
// longer than the stance crouch's because this is a deliberate change of
// pose, not a per-step oscillation — it should read as sinking into the
// knees, not snapping.
constexpr float kMaxHeldCrouchLegLengths = 0.55f;
constexpr float kHeldCrouchHalflife = 0.08f;

// Stride-clock lock. `kStrideSyncGain` is how much of the phase error is taken
// out at each touchdown: 1 snaps (and pops the bob), 0 never locks. Half
// converges inside two steps while keeping every correction sub-visible once
// locked. `kStepPeriodHalflife` smooths the measured step period so one long
// stride over a ledge does not slew the whole clock.
constexpr float kStrideSyncGain = 0.5f;
constexpr float kStepPeriodHalflife = 0.25f;
// A step slower than this is not a gait — the clock parks rather than crawling.
constexpr float kMaxStepPeriod = 1.2f;

// ---- THE AIRBORNE POSE, IN LEG LENGTHS AND ARM REACHES ---------------------
//
// Four shapes, blended by one number. `avatar.airPose` in tuning.h says why the
// number is `vel.y` and not a clock; these are the shapes it interpolates.
//
// AUTHORED AS RATIOS, DELIBERATELY (sim/scale.h, "ratios and relative
// measures"): a foot target is a fraction of THAT rig's leg and a hand target a
// fraction of THAT rig's arm, so the same table poses the wizard, mina and a
// four-armed thing nobody has drawn yet without a per-def pose block. The gait
// above is authored the same way and for the same reason — and it is why the
// NPCs could be given the air pose in W2-L without a single authored key.
//
// The FOOT frame is prefab space TILTED BY THE LEAN: +Y up, +Z forward, `Out`
// away from the midline on that leg's own side. `footFwd` is signed per leg by
// the scissor split so the legs pass each other, and `footLead` is the part
// both feet share (what a landing wants, since you land on two of them).
//
// Every foot offset stays under 1.0 in magnitude: a two-bone chain asked for its
// full span is a straight, locked limb and AnimSolveTwoBone clamps to its reach
// annulus, so the table would collapse to one pose at the extremes.
//
// `lean` is radians of FORWARD pitch — a positive rotation about model +X,
// which takes +Z to -Y (verified with scripts/geometry.py), i.e. nose down.
//
// THE LEGS ARE SOLVED; THE ARMS ARE POSED. Two shapes, two mechanisms, and the
// split is not arbitrary:
//
//   A LEG has to arrive at a PLACE — under the hip, out in front, down on the
//   ground the probe found — and its chain exists to hit a point. IK.
//
//   AN ARM only has to have an ANGLE, and this rig's elbow is an authored
//   HINGE (`armL.*` poseLimit, axis -X, 0..130, hinge true). AnimClampPoseLimits
//   DISCARDS whatever off-hinge swing a solver produces (anim.h says so at the
//   flag), and a hand target even slightly off the arm's sagittal plane makes
//   the solver bend in a plane the hinge does not share — so the clamp throws
//   the bend away and the forearm ends up pointing somewhere nobody asked for.
//   Photographed with --shot-jump, both forearms stuck out sideways like
//   paddles at every phase.
//
//   Posing the SHOULDER and the ELBOW directly cannot fail that way: those are
//   exactly the degrees of freedom the rig authors limits about, so the clamp
//   has nothing to discard. It is also less work than a two-bone solve.
//
// AND THE ARMS OPPOSE THE LEGS. `armSwing` is signed against this arm's own
// leg, so the left arm leads while the left leg trails — the way it does in
// every stride the gait already runs. Without it both arms do the same thing at
// once and the body reads as a mannequin held in the air.
//        footDown footFwd footLead footOut  armPitch armSwing armOut armFlex lean

// TUCK — the launch. Knees come up and the arms finish the swing that threw the
// body off the ground: the lead arm drives up in front, the trailing one is
// still behind, elbows well bent. The chest opens a little, because a jump
// starts with the back arching rather than folding. The LEG scissor is small —
// both knees rise, one merely leads — while the ARM swing is the widest of the
// four, since this is the instant the arms were actually driving.
constexpr AirKeyPose kAirTuck{0.46f, 0.17f, 0.00f, 0.06f,
                              0.42f, 0.70f, 0.14f, 0.95f, -0.07f};
// FLOAT — the apex. Nothing is driving any more, so the shape relaxes rather
// than opening: legs part-extended and scissored, arms carried low and still
// swinging, but half as far. This is the pose a short hop spends nearly all of
// its air time in, so it is deliberately the quietest of the four — an ordinary
// hop should not look like an event.
constexpr AirKeyPose kAirFloat{0.71f, 0.26f, 0.00f, 0.11f,
                               0.20f, 0.48f, 0.12f, 0.55f, 0.01f};
// REACH — a committed fall. The legs run down toward whatever is coming and the
// arms come up to brace: elbows folded, forearms near head height, the
// counter-swing fading as the two arms converge on the same job. Not a flail —
// the wide arms-out shape the old clip had at full weight read as comedy on a
// two-metre drop, and this only arrives at speeds a two-metre drop never
// reaches.
constexpr AirKeyPose kAirReach{0.92f, 0.16f, 0.03f, 0.14f,
                               0.85f, 0.28f, 0.22f, 1.15f, 0.11f};
// PREPARE — the ground is close. You land on TWO FEET, so the scissor nearly
// vanishes and what replaces it is `footLead`, which both legs share: the feet
// come down and forward together, the knees keep more bend than the reach does
// (0.79 against 0.92 — there has to be travel left to absorb with), the arms
// stop counter-swinging and reach forward for balance, and the body folds into
// the landing. This is the shape the `land` clip's squash arrives from, and
// having it is most of why a fall reads as a fall rather than as a floating
// pose that stops. Without the shared lead it was the reach shape with slightly
// different numbers, and photographed as the same picture twice.
constexpr AirKeyPose kAirPrepare{0.79f, 0.04f, 0.19f, 0.17f,
                                 0.62f, 0.10f, 0.18f, 0.80f, 0.19f};

inline float MixF(float a, float b, float t) { return a + (b - a) * t; }
inline AirKeyPose MixAir(const AirKeyPose& a, const AirKeyPose& b, float t) {
  return AirKeyPose{
      MixF(a.footDown, b.footDown, t), MixF(a.footFwd, b.footFwd, t),
      MixF(a.footLead, b.footLead, t), MixF(a.footOut, b.footOut, t),
      MixF(a.armPitch, b.armPitch, t), MixF(a.armSwing, b.armSwing, t),
      MixF(a.armOut, b.armOut, t),     MixF(a.armFlex, b.armFlex, t),
      MixF(a.lean, b.lean, t)};
}

// How far above the SOLE the gait's foot probe and the air pose's landing
// probe start, in cells: the avatar's historical 2 cells, stated in metres.
constexpr int kFootProbeLift = MetresToCellsI(0.20f);

inline float HalfLifeK(float dt, float hl) {
  return hl > 1e-4f ? 1.0f - std::pow(0.5f, dt / hl) : 1.0f;
}

}  // namespace

// ---- ONLY LEGS WALK --------------------------------------------------------
//
// `sk.chains` holds every IK chain the rig publishes, and on every current
// humanoid that is two legs AND TWO ARMS. The NPC gait once treated all four as
// feet: the hands were handed ground contact points, dragged into the body-
// height average, and folded into the foot plane the body's tilt was derived
// from — the "an NPC standing on a ramp is rotated 45 degrees" bug.
//
// The fallback is deliberate: a rig that tags NO chain "leg" is a legacy rig
// whose chains are all legs by convention, and silently giving it no gait at
// all would be a worse failure than the one being fixed. ONE predicate for the
// gait, the stride clock's leg count, the leg IK and the air pose's scissor, so
// the four cannot disagree about which chains are legs (the avatar's copies
// used the bare tag and the NPC's the fallback until W2-L).
bool Mob::IsLegChain(const AnimSkeleton& sk, size_t c) {
  if (c >= sk.chains.size()) return false;
  if (sk.chains[c].tag == "leg") return true;
  for (const IkChain& ch : sk.chains)
    if (ch.tag == "leg") return false;
  return true;
}

// ---- THE PIPELINE ------------------------------------------------------------

void Mob::PosePipeline(const PoseInputs& in, float dt, World& world,
                       uint32_t tick) {
  const AnimSkeleton& sk = skel_;
  AnimState& st = anim_;
  if (sk.parts.empty() || def_ == nullptr) return;
  const MobDef& def = *def_;
  st.partAlive.resize(sk.parts.size(), 1);
  st.springs.resize(sk.parts.size(), SpringState{});

  // ---- THE VELOCITY: the driver's, smoothed on a HALF-LIFE -----------------
  //
  // The driver supplies it (pose.h PoseInputs::velocity) because only the
  // driver knows which integrator owns the body. On the avatar the old
  // measurement — differencing our own origin over `kTickDt` — was garbage:
  // Player::Update runs once per FRAME at real dt, this runs 0..4 times a
  // frame inside the fixed-tick loop, so the delta measured the wrong interval
  // every frame and slammed between double the truth and zero (the "character
  // spazzes, hands move like crazy" report). An NPC moves itself one step per
  // tick, so for it the difference IS exact, and the NPC driver still takes it.
  //
  // The SMOOTHING is the player's, and it is a half-life rather than the NPC's
  // old per-call `0.7/0.3` blend: a per-call blend's time constant scales with
  // how many ticks fire, the same class of bug as the measurement above. At the
  // NPC's fixed 30 Hz the old blend was a 0.065 s half-life; it is
  // `avatar.velocityHalflife` (0.08 s) now, for every creature.
  st.lastPos = origin_;  // other code reads it as "where we were"
  const Vec3 planar{in.velocity.x, 0, in.velocity.z};
  st.velocity = st.velocity +
                (planar - st.velocity) *
                    HalfLifeK(dt, CurrentTuning().avatar.velocityHalflife);
  speedNow_ = Vec3{st.velocity.x, 0, st.velocity.z}.len();
  const float speedFactor =
      std::clamp(speedNow_ / std::max(def.speed, 0.01f), 0.0f, 1.5f);

  // ---- ONE CLOCK, AND THE FEET OWN IT --------------------------------------
  // See SyncStrideClock for why a `cadence * speedFactor` oscillator ran at
  // 2.6x the real footfall rate on the avatar and read as a fast, jittery sway.
  // The rate is measured between touchdowns; the phase is corrected at each
  // one. Until the first two steps have been timed `strideRate` is zero and the
  // pelvis simply holds still, which is the honest answer for a body that has
  // not taken a stride yet.
  //
  // THE NPCS NOW RUN IT TOO. The NPC path kept the free oscillator
  // deliberately ("its swing is a flat stepDuration at mob speeds, where the
  // mismatch is small"), with the note that anyone unifying these should port
  // the sync, not the oscillator — which is what this is. With the stride
  // budget bounding their swing as well, the two clocks would disagree on an
  // NPC exactly as they did on the player.
  //
  // A rig with NO leg chains (dummy.json) keeps the oscillator: it never
  // plants a foot, so there is nothing to lock to. `phase_` below is that
  // rig's legacy swingAmp drive.
  const GaitDef& g = sk.gait;
  bool haveLegs = false;
  for (size_t c = 0; c < sk.chains.size(); c++)
    if (IsLegChain(sk, c)) haveLegs = true;
  if (haveLegs) {
    // Park the clock when the feet stop reporting: a body standing still takes
    // no steps, and a rate left running would keep the (speed-scaled, so
    // invisible) bob accumulating phase to land on an arbitrary value the
    // moment it moves again.
    if (pose_.sinceTouchdown > kMaxStepPeriod)
      pose_.strideRate *= std::pow(0.5f, dt / 0.15f);
    st.gaitPhase += dt * pose_.strideRate;
  } else {
    st.gaitPhase += dt * (g.present ? g.cadence : 2.2f) * speedFactor;
  }
  st.gaitPhase -= std::floor(st.gaitPhase);
  phase_ = st.gaitPhase * 6.2831853f;

  // ---- dismemberment locomotion state ----
  // Polled every tick rather than only on Sever: partAlive changes in several
  // places (Sever, recursive DetachLimb, Die), and this is a handful of
  // comparisons. On a transition the outgoing clip blends out while the new
  // one blends in over its own blendInMs — a crossfade for free.
  {
    const int want = AnimSelectState(sk, st);
    if (want != st.locoState) {
      if (st.locoState >= 0 && !sk.states[st.locoState].clip.empty()) {
        const int old = sk.FindClip(sk.states[st.locoState].clip);
        for (ClipInstance& inst : st.clips)
          if (inst.clip == old) inst.stopping = true;
      }
      st.locoState = want;
      if (want >= 0) {
        const AnimStateRule& rule = sk.states[want];
        if (!rule.clip.empty()) PlayClip(rule.clip);
        // A foot frozen mid-swing would report "swinging" forever once the
        // gait stops running; land everything where it stands.
        if (rule.disableGait)
          for (FootState& f : st.feet) f.swinging = false;
      }
    }
  }
  const AnimStateRule* loco =
      st.locoState >= 0 ? &sk.states[st.locoState] : nullptr;
  const bool clipOwnsPose = loco && loco->disableGait;

  // ---- A CRAWLER THAT IS NOT GOING ANYWHERE IS NOT CRAWLING ----------------
  // The prone clip is a locomotion cycle, and it used to loop at full rate
  // whatever the body was doing, so a legless creature lying still stroked the
  // ground forever. Its playhead now advances with the body's own speed
  // against the speed the state is authored for: a stop freezes the stroke
  // where it is (the body just lies there), creeping plays it slowly, and a
  // full crawl plays it as authored. `speedNow_` is already low-passed
  // (avatar.velocityHalflife), which is what makes the stop an ease and not a
  // snap. Only the playhead is scaled (ClipInstance::rate), never the blend.
  if (loco != nullptr && loco->groundAlign > 0.0f && !loco->clip.empty()) {
    const int ci = sk.FindClip(loco->clip);
    const float ref =
        std::max(def.speed * std::max(loco->speedScale, 0.05f), 0.5f);
    float rate = std::clamp(speedNow_ / ref, 0.0f, 1.5f);
    if (rate < 0.05f) rate = 0.0f;
    for (ClipInstance& inst : st.clips)
      if (inst.clip == ci && !inst.stopping) inst.rate = rate;
  }

  // ---- the chase pose: arms out while the dead come for you ----------------
  // `MobDef::chaseClip` (zombie.json names `reach`). Held while the driver
  // says the creature is engaged (PoseInputs::chaseWant — the NPC brain's
  // "has a target, not idle, not mid-strike"), dropped otherwise. A
  // dismemberment state that owns the pose outright (crawl, squirm) wins for
  // the same reason it beats the gait swing. The clip is ADDITIVE, so leaving
  // it on top of an authored punch or bite would add 80 degrees of shoulder
  // to every blow — which is why the driver drops `chaseWant` for a strike.
  if (!def.chaseClip.empty()) {
    const int ci = sk.FindClip(def.chaseClip);
    if (ci >= 0) {
      const bool want = in.chaseWant && !clipOwnsPose;
      // Seconds to reach the pose, and to give it up. Fast enough that the
      // arms are already down by the time a punch's own clip has blended in
      // (0.09 s on the library strokes) without the snap a hard switch gives.
      constexpr float kChasePoseRampS = 0.18f;
      const float step = dt / kChasePoseRampS;
      chasePose_ = want ? std::min(1.0f, chasePose_ + step)
                        : std::max(0.0f, chasePose_ - step);
      if (chasePose_ > 0.0f) {
        // REVIVE BEFORE REQUESTING. PlayClipIndex skips an instance already
        // marked `stopping` and appends a second one beside it, so a target
        // re-acquired inside the blend-out would leave two of this clip
        // stacked. Clearing the flag first makes the request a no-op on the
        // instance we already own.
        for (ClipInstance& inst : st.clips)
          if (inst.clip == ci) {
            inst.stopping = false;
            inst.weight = chasePose_;
          }
        PlayClipIndex(ci);   // no-op once the instance is running
        for (ClipInstance& inst : st.clips)
          if (inst.clip == ci) inst.weight = chasePose_;
      } else {
        // Retire it at zero rather than leaving a weightless instance to be
        // sampled every tick for the rest of the creature's life.
        for (ClipInstance& inst : st.clips)
          if (inst.clip == ci) inst.stopping = true;
      }
    }
  }

  // ---- stages 1-3: sample active clips, blend, apply additives ----
  AnimSampleAndBlend(sk, st, dt);

  // ---- the legless body's locomotion: lateral undulation -------------------
  // Straight after the clips, so a clip that OWNS a segment (the strike's
  // coil) is read from the blend it just made, and before everything that
  // rotates a part about its joint (aim, hit react) so those compose on top
  // of the wave rather than being overwritten by it. Data-gated on the def's
  // `slither` block: one test on every other rig.
  if (def.slither.present) ApplySlither(in, dt);

  // ---- procedural layer: the legacy phase swing ----------------------------
  // dummy.json has swingAmp/swingPhase and no chains; running it HERE rather
  // than as a separate code path means the fallback and the new rig share one
  // pipeline. Suppressed while a disableGait loco state is active: a crawl clip
  // keys the same parts the walk swing drives. No current def with legs
  // authors swingAmp, so on those this loop costs one test per part.
  for (size_t i = 0; !clipOwnsPose && i < sk.parts.size(); i++) {
    const AnimPart& p = sk.parts[i];
    if (p.swingAmp == 0) continue;
    // progressive phase lag up the hierarchy: each level down the chain
    // trails its parent slightly, which is what makes a walk look like it
    // propagates through the body instead of moving as one rigid piece.
    int depth = 0;
    for (int k = p.parent; k >= 0; k = sk.parts[k].parent) depth++;
    const float lag = g.present ? g.phaseLag * (float)depth * 6.2831853f : 0.0f;
    const float swing =
        p.swingAmp * std::sin(phase_ - lag + p.swingPhase * 3.14159265f);
    // when a gait is present the legs are IK-driven, so the swing only
    // survives on parts no chain owns (arms, head)
    bool inChain = false;
    for (const IkChain& ch : sk.chains)
      for (int cp : ch.parts) inChain |= (cp == (int)i);
    if (inChain) continue;
    st.local[i].rot =
        QuatNormalize(Mul(st.local[i].rot, AxisAngle(p.axis, swing)));
  }

  // ---- pelvis bob / sway / spine counter-rotation --------------------------
  // Suppressed while airborne (the sway is a walk rhythm and has no meaning in
  // the air — the NPC copy ran it through a fall until W2-L) or while an
  // authored clip owns the pose (a crawl keys the same parts these drive).
  // Pelvis bob at 2x step frequency (one rise per FOOTFALL), sway and roll at
  // 1x, on the ONE clock above.
  if (g.present && !clipOwnsPose && in.grounded && def.rootLimb >= 0 &&
      def.rootLimb < (int)sk.parts.size()) {
    Transform& root = st.local[def.rootLimb];
    root.pos.y += g.bobAmp *
                  std::sin(g.bobFreqMul * 6.2831853f * st.gaitPhase) *
                  speedFactor;
    root.pos.x += g.swayAmp * std::sin(6.2831853f * st.gaitPhase) * speedFactor;
    const float roll =
        g.rollAmp * std::sin(6.2831853f * st.gaitPhase) * speedFactor;
    root.rot = QuatNormalize(Mul(root.rot, AxisAngle({0, 0, 1}, roll)));
    for (size_t i = 0; i < sk.parts.size(); i++) {
      if (sk.parts[i].tag != "spine") continue;
      st.local[i].rot = QuatNormalize(
          Mul(st.local[i].rot, AxisAngle({0, 1, 0}, -g.spineCounter * roll)));
    }
  }

  // ---- springs: a part is KEYED or JIGGLED, never both ----
  // The goal is driven by velocity NORMALIZED against the def's own top speed,
  // so `gain` means "radians of lag at full speed" for every def regardless of
  // how fast that def moves (kSpringVelScale, anim.h).
  const float speedRef = std::max(def.speed, 0.01f);
  for (size_t i = 0; i < sk.parts.size(); i++) {
    const AnimPart& p = sk.parts[i];
    if (!p.hasSpring) continue;
    Vec3 goal{-st.velocity.z / speedRef * p.spring.gain * kSpringVelScale, 0,
              st.velocity.x / speedRef * p.spring.gain * kSpringVelScale};
    goal.x = std::clamp(goal.x, -p.spring.maxAngle, p.spring.maxAngle);
    goal.z = std::clamp(goal.z, -p.spring.maxAngle, p.spring.maxAngle);
    AnimSpringStep(p.spring, st.springs[i], goal, dt);
    const Vec3& s = st.springs[i].x;
    const Quat jiggle =
        Mul(AxisAngle({1, 0, 0}, s.x),
            Mul(AxisAngle({0, 1, 0}, s.y), AxisAngle({0, 0, 1}, s.z)));
    st.local[i].rot = QuatNormalize(Mul(st.local[i].rot, jiggle));
  }

  // ---- head look (the LOOK TARGET input) ------------------------------------
  // THE HEAD LEADS, THE BODY FOLLOWS: the head points where the driver says
  // it is looking, so a glance reads as a glance rather than as the whole
  // character strafing with a fixed stare. Eased on avatar.headLookHalflife
  // toward the goal — a glance is a slow thing — and back to straight ahead
  // when the driver has no look to give.
  //
  // APPLIED BEFORE THE FLATTEN: `local[i].rot` is a joint rotation and the
  // head has children, so composing it at the joint carries them along. The
  // angle arithmetic (the spine's share, the pitch negation, the yaw sign
  // from the BODY's heading convention) is Mob::ApplyAimPart's.
  {
    const auto& av = CurrentTuning().avatar;
    const float k = HalfLifeK(dt, av.headLookHalflife);
    const float goalYaw = in.haveLook ? in.lookYaw : 0.0f;
    const float goalPitch = in.haveLook ? in.lookPitch : 0.0f;
    pose_.lookYaw += (goalYaw - pose_.lookYaw) * k;
    pose_.lookPitch += (goalPitch - pose_.lookPitch) * k;
    int head = -1;
    for (size_t i = 0; i < sk.parts.size(); i++)
      if (sk.parts[i].name == "head") {
        head = (int)i;
        break;
      }
    // A severed head does not look at anything.
    const bool haveHead = head >= 0 && head < (int)st.local.size() &&
                          (head >= (int)st.partAlive.size() || st.partAlive[head]);
    if (haveHead && (std::fabs(pose_.lookYaw) > 1e-4f ||
                     std::fabs(pose_.lookPitch) > 1e-4f))
      ApplyAimPart(sk, st, head, pose_.lookYaw, pose_.lookPitch, 1.0f,
                   in.lookSpineShare);
    // A held pose that follows the look up and down (AnimClip::pitchFollow,
    // e.g. spell_ready's arms): a share of the same eased pitch, about the
    // part's own X, sign as ApplyAimPart's (pitch positive UP).
    if (std::fabs(pose_.lookPitch) > 1e-4f)
      for (size_t i = 0; i < st.pitchFollow.size() && i < st.local.size(); i++) {
        const float f = st.pitchFollow[i];
        if (f == 0.0f || (i < st.partAlive.size() && !st.partAlive[i])) continue;
        st.local[i].rot = QuatNormalize(
            Mul(st.local[i].rot, AxisAngle({1, 0, 0}, -pose_.lookPitch * f)));
      }
  }

  // ---- stage 3.5: the torso serves a live swing (anim.h) ----
  // Composes additively with the head-look's share above — both are bounded
  // small rotations on the same spine locals. The pose's torso fields are
  // already weight-scaled by the melee driver, so this is a no-op outside a
  // stroke and fades exactly with the arm claim.
  AnimApplySpineTwist(sk, st, weapon_.torsoTwist, weapon_.torsoPitch,
                      def.rootLimb);

  // ---- stage 3.6: the AIM TARGET — the strike aim, or a look at a target ----
  // PRE-FLATTEN, because it rotates a part about its joint and shares the yaw
  // with the spine above it. See Mob::ApplyStrikeAim. A no-op on a body with no
  // Aim-mode effector live and no `aimLook_` — which is every player today.
  if (in.strikeAim) ApplyStrikeAim(sk, st);

  // ---- the airborne pose: phase, landing probe and lean ---------------------
  // Resolved BEFORE the flatten, because the lean is a spine rotation and the
  // parts hanging off the spine have to inherit it. Every creature now: an NPC
  // goes airborne off a lunge, a blast before it goes limp, a lift and a ledge,
  // and until W2-L it did all of them in its walking pose.
  UpdateAirDrive(in, dt, world, clipOwnsPose);
  ApplyAirLean(sk, st);
  // The airborne arms, at their own joints and in the same layer as the lean
  // they hang off — see ApplyAirArms for why they are posed rather than solved.
  ApplyAirArms(sk, st);
  // The ledge climb's torso lean, in the same pre-flatten layer as the air
  // lean; its legs are solved after the leg IK below, its arms are the hang
  // palms kept on the lip.
  UpdateClimbDrive(in, dt);
  ApplyClimbLean(sk, st);

  // ---- stage 3.7: the body answers a blow (mob.h Mob::HitReact) ----
  // After the aim and the twist, so a creature mid-bite that gets hit rocks
  // on top of the pose its stroke is holding; pre-flatten, because it writes
  // the same `st.local` channel the bob, the lean and the twist do.
  ApplyHitReact(sk, st, dt);

  // ---- stage 3.55: the one-footed drag (mob.h Mob::TickStumpDrag) ---------
  // `grounded` is the driver's debounced view deliberately — the drag must not
  // drop out for the tick a bump crest costs — and it is also how a HOP works:
  // leaving the ground eases the drag out on the way up and back in on landing.
  {
    const AnimStump stump = TickStumpDrag(dt, in.grounded, clipOwnsPose);
    TrackStumpContact(world, stump, dt);
  }

  if (loco && loco->groundAlign > 0.0f)
    AnimDragDeadLegs(sk, st, speedNow_, st.gaitPhase);

  // ---- stage 4: flatten to model space ----
  AnimFlatten(sk, st);

  // `grounded` is part of the gate, not just an input to it: a gait with no
  // floor under it has no meaningful foot target (see ParkGaitForAir).
  const bool gaitActive = g.present && !clipOwnsPose && in.grounded;
  const bool driverHeight = in.height == PoseInputs::Height::FromDriver;

  // ---- the held crouch (a DRIVER EXTRA) ----
  // Eased toward the authored knee drop while the driver asks for it, toward 0
  // otherwise, and off entirely while a clip owns the pelvis (crawl, squirm):
  // those key the same pelvis and composing the two would double-drop it.
  // Capped in leg lengths off the rig's own chain so an authored metre value
  // cannot ask more bend than the solve can pose. Runs in the air too — a
  // crouch-jump keeps its tuck, and the landing does not have to re-sink.
  {
    float legLen = 0.0f;
    for (const FootState& f : st.feet) legLen = std::max(legLen, f.legLength);
    float want = (in.crouch && !clipOwnsPose)
                     ? MetresToCells(CurrentTuning().player.crouchKneeDrop)
                     : 0.0f;
    if (legLen > 0.0f) want = std::min(want, kMaxHeldCrouchLegLengths * legLen);
    pose_.crouchHold += (want - pose_.crouchHold) * HalfLifeK(dt, kHeldCrouchHalflife);
    if (pose_.crouchHold < 1e-3f) pose_.crouchHold = 0.0f;
  }

  // ---- THE IK FADES IN AND OUT; IT DOES NOT SWITCH -------------------------
  //
  // `grounded` is genuinely ragged crossing a bumpy incline, and a gate that
  // turned it into a HARD switch snapped the limbs between the IK-solved pose
  // and the rest hang the air branch leaves behind — the avatar's "arms shoot
  // up straight walking uphill". AnimSolveTwoBone already takes a WEIGHT and
  // blends its result against the incoming pose, so the weight is driven
  // continuously instead. This is NOT blending two IK results together (a pose
  // satisfying neither foot); it fades a single solve against the flattened
  // animation pose, which is what the weight parameter is for.
  //
  // THE AIR POSE IS THE SAME AUTHORITY, NOT A COMPETING ONE: `airFrac` counts
  // as gait for this weight. Without that a take-off would fade this to zero
  // while the air pose faded up, and their product leaves a window mid-jump
  // where neither owns the legs and the rest hang shows through. What
  // crossfades is the TARGET, in the solve below.
  //
  // The NPC copy switched its leg IK on `!airborne_` as a bool until W2-L.
  {
    const float want = (gaitActive || pose_.airFrac > 0.0f) ? 1.0f : 0.0f;
    pose_.gaitWeight += (want - pose_.gaitWeight) *
                        HalfLifeK(dt, CurrentTuning().avatar.ikBlendHalflife);
    if (pose_.gaitWeight < 1e-3f) pose_.gaitWeight = 0.0f;
    if (pose_.gaitWeight > 0.999f) pose_.gaitWeight = 1.0f;
  }

  if (!in.grounded && !clipOwnsPose) {
    ParkGaitForAir(in, dt);
  } else if (gaitActive) {
    StepGait(in, dt, world, tick);
  } else {
    // An authored clip owns the pose (crawl, a hop), or the rig has no gait:
    // it keys the same pelvis the crouch moves, so unwind the crouch rather
    // than composing the two. Mob::SettleClipOwnedBody is the whole of "lying
    // on the ground" (and of an upright clip-owned state's offset), including
    // the tilt ease. What differs is the HEIGHT ease, and it follows who owns
    // the height: a driver-pinned body takes the historical +-0.4 per tick; a
    // feet-derived one takes MobSystem::EaseBodyY's bounded lag.
    pose_.stanceCrouch *= std::pow(0.5f, dt / 0.12f);
    float targetY = origin_.y;
    SettleClipOwnedBody(world, loco, dt, targetY);
    if (driverHeight)
      bodyY_ += std::clamp(targetY - bodyY_, -0.4f, 0.4f);
    else
      MobSystem::EaseBodyY(*this, targetY, dt);
    footInit_ = true;
    // ...and then each segment follows ITS stretch of ground rather than the
    // one plane (Mob::ConformProneSegments). After the height is final, since
    // the probes are taken from where the joints are actually drawn.
    if (loco != nullptr && loco->groundAlign > 0.0f)
      ConformProneSegments(world, dt);
    else
      proneConform_.clear();
  }
  if (!clipOwnsPose) proneConform_.clear();
  pose_.bodyPlaced = true;

  // ---- stage 5: leg IK, strictly a POST-PROCESS on the flattened pose -------
  // IK is never a blended layer: blending two IK results gives a pose that
  // satisfies neither end-effector. Runs whenever the weight is non-zero, not
  // only while the gait is active: the ticks that need the fade are exactly
  // the ones just after the gait switched off. `f.planted` is still the last
  // real foot target during those ticks — ParkGaitForAir parks the SWING but
  // deliberately leaves `planted` alone — so the legs ease out of their last
  // stance instead of snapping to the rest hang.
  //
  // IN THE AIR THE SAME SOLVE IS AIMED SOMEWHERE ELSE. The airborne shape is a
  // body-relative foot target (kAirTuck and friends), arriving as a blend of
  // the TARGET rather than as a second solve.
  if (!sk.chains.empty() && pose_.gaitWeight > 0.0f) {
    const Quat yaw = AxisAngle({0, 1, 0}, heading_);
    const Vec3 pivot{def.worldSize.x * 0.5f, 0, def.worldSize.z * 0.5f};
    const Vec3 bodyOrigin{origin_.x, bodyY_, origin_.z};
    const Quat leanQ = Mul(AxisAngle({1, 0, 0}, pose_.airLeanPitch),
                           AxisAngle({0, 0, 1}, pose_.airLeanRoll));
    // Which leg leads the scissor. An ORDINAL among the leg chains, not the
    // chain index: a rig whose arms are interleaved with its legs must still
    // split its legs fore and aft, the same argument SyncStrideClock makes.
    int legOrdinal = 0;
    for (size_t c = 0; c < sk.chains.size() && c < st.feet.size(); c++) {
      if (!IsLegChain(sk, c)) continue;
      const FootState& f = st.feet[c];
      const float split = (legOrdinal++ & 1) ? -1.0f : 1.0f;
      const float weight = f.valid ? sk.chains[c].weight * pose_.gaitWeight : 0.0f;
      if (weight <= 0) continue;
      // The airborne target, in the leaned body's own frame off this leg's hip.
      const int hip = sk.chains[c].parts.empty() ? -1 : sk.chains[c].parts[0];
      const bool haveAir =
          pose_.airFrac > 0.0f && hip >= 0 && hip < (int)st.model.size();
      Vec3 airPt{};
      if (haveAir) {
        const float L = f.legLength;
        const float side = sk.parts[hip].anchorLocal.x >= pivot.x ? 1.0f : -1.0f;
        const AirKeyPose& key = pose_.airKey;
        const Vec3 off{side * key.footOut * L, -key.footDown * L,
                       (split * key.footFwd + key.footLead) * L};
        airPt = st.model[hip].pos + Rotate(leanQ, off);
      }
      const Vec3 rel = f.planted - bodyOrigin - pivot;
      // A STALE PLANT MUST NOT BE REACHED FOR. `planted` is a WORLD point, and
      // in a real fall the body drops away from it until it sits above the hip
      // — at which point the solver aims the legs UP and folds them through the
      // pelvis (the leg-inversion failure ParkGaitForAir exists to prevent).
      // Once the target is further than the leg can reach there is nothing
      // sensible to solve for, and the remaining fade is dropped rather than
      // pointed at a ghost; with the air pose live, the chain is handed to it.
      const bool haveGait = rel.len() <= f.legLength * 1.6f;
      if (!haveGait && !haveAir) continue;
      const Vec3 prefabPt = RotateInv(yaw, rel) + pivot;
      // THE TARGET IS PREFAB-ABSOLUTE, AND SO IS THE HIP IT IS SOLVED AGAINST.
      // Nothing is rebased here. AnimFlatten seeds the root with its own
      // rest.pos, which IS rootAnchor, so every st.model[].pos already carries
      // the root offset — including st.model[hip].pos, which AnimSolveTwoBone
      // reads as its chain root. Subtracting rootAnchor from the target while
      // the hip keeps it moved the two ends into different frames and skewed
      // (target - root) by exactly rootAnchor (the avatar's 11.2-voxel ghost
      // target and the NPC's both-legs-splayed-one-way were the same mistake).
      // Same convention as the submit path, which likewise does not re-add
      // rootAnchor to modelPos. `airPt` is already in this frame.
      const Vec3 target =
          !haveGait ? airPt
                    : (haveAir ? prefabPt + (airPt - prefabPt) * pose_.airFrac
                               : prefabPt);
      AnimSolveTwoBone(sk, st, sk.chains[c], target, weight);
    }
  }

  // ---- the ledge climb's legs: the knee onto the lip, then the stand ----
  ApplyClimbLegs(sk, st);

  // ---- stage 5.5: the weapon arm (game/melee.h) ----
  // THE SAME CALL FOR EVERY CREATURE: a stroke is driven by aiming the WEAPON
  // ARM's IK chain at a point derived from the stroke driver, a post-process
  // like the legs, faded by `weaponWeight_`. The notes about the mirrored-
  // authoring x flip, the pole, and why nothing may rotate the held part after
  // the flatten live in Mob::ApplyWeaponArm.
  PoseAxisOverride weaponHinge;
  ApplyWeaponArm(sk, st, weaponHinge);
  // ...and the style's per-joint brakes on what it solved (melee.h ArmSmooth).
  // Before the clamp, like every other solver on this path.
  SmoothWeaponArm(sk, st, dt);

  // ---- ledge-hang palms (a DRIVER EXTRA) ----
  ApplyHangArms(in, dt, sk, st);

  // ---- anything else the driver solves, clamped with the rest ----
  if (in.extraIk) in.extraIk(sk, st);

  // ---- stage 6: the pose has to be anatomically possible ------------------
  //
  // LAST, after every solver on this path — the leg gait, the weapon arm and
  // the ledge hang all write st.model[] and any of them can push a joint past
  // its range. Clamping between them would let a later solve undo the clamp.
  // The weapon arm hands in ONE exception while a stroke is live: the elbow's
  // hinge PLANE is steered into the plane of the cut (anim.h PoseAxisOverride).
  // FORGETTING THE OVERRIDE HERE IS SILENT AND EXPENSIVE — the solve reports
  // `ikMiss 0.00` and the clamp discards the off-plane component; that is why
  // RecordWeaponClamp sits on the same line.
  AnimClampPoseLimits(sk, st, &weaponHinge, 1);
  ReapplyKeyedArm(sk, st);
  RecordWeaponClamp(sk, st);
  // A held vessel tilted to what the alchemy bench shows (SetHeldAim): after
  // the clamp, because the tilt is the bench's fact, not the wrist's choice.
  ApplyHeldAim(sk, st);

  // ---- flipbooks: integer frame index from elapsed ms ----
  // Data-gated like the chase clip: a def with no live flipbook pays one test.
  if (st.flipbook.book >= 0 && st.flipbook.book < (int)sk.flipbooks.size()) {
    const Flipbook& fb = sk.flipbooks[st.flipbook.book];
    st.flipbook.elapsedMs += (int32_t)std::lround(dt * 1000.0);
    const int fr = AnimFlipbookFrame(fb, st.flipbook.elapsedMs);
    if (fr != st.flipbook.frame) {
      st.flipbook.frame = fr;
      // re-point the affected limb at the frame's model; instances rebuild
      for (const FlipbookFrame& ff : fb.frames) {
        if (ff.part < 0 || ff.part >= (int)limbs_.size()) continue;
        limbs_[ff.part].flipbookModel = -1;
      }
      if (fr >= 0 && fr < (int)fb.frames.size()) {
        const FlipbookFrame& ff = fb.frames[fr];
        if (ff.part >= 0 && ff.part < (int)limbs_.size()) {
          limbs_[ff.part].flipbookModel = ff.model;
          MarkInstancesDirty();   // bounded: only on an actual frame change
        }
      }
    }
    if (!fb.loop && st.flipbook.frame == (int)fb.frames.size() - 1)
      st.flipbook.book = -1;
  }
}

// ---- LATERAL UNDULATION -------------------------------------------------------
//
// THE WAVE IS FIXED IN THE WORLD, NOT IN THE BODY. A travelling sine of yaw
// along the chain, if its phase ran on a clock, would be a wave sliding along
// a body that slides along the ground: the curve would move sideways under
// every segment and the creature would skate, which is exactly what reads as
// fake. So the phase is the ODOMETER -- the distance covered along the facing
// -- times the wavenumber. A point `s` voxels behind the snout then sits at
// lateral offset A*sin(k*(d - s)), where d - s is (to first order) the world
// distance along the path at which that point is standing: every segment
// passes through the same S the head drew, nothing slips sideways, and forward
// speed and wave frequency are tied by construction (f = v / lambda). A body
// that stops keeps its curve, which is what a resting snake does.
//
// THE CURVE IS POSED, NOT SIMULATED. Each segment is yawed to the path's
// tangent at its own middle (psi = atan(dX/dz), dz = -ds along a body laid out
// along +Z), as a delta on its parent's yaw so the flatten builds the polyline;
// the root, whose joint is the rig's origin, is also SHIFTED to the path's
// lateral offset at its joint, because a rotation alone cannot move the point
// the rig hangs from.
//
// A TURN BENDS THE WAVE. A body turning at w while moving at v is on a path of
// curvature kappa = w / v; laying the whole chain on that arc
// (X += kappa/2 * (s - sPivot)^2, a parabola about the yaw pivot) means the
// tail follows the head round the corner instead of being swung sideways by
// the yaw. A floored speed and a ceiling on kappa keep a turn on the spot from
// coiling the body into a knot, and the bend eases on its own half-life.
//
// THE HEAD STEADIES. A real snake holds its head nearly on line while the body
// swings behind it; the envelope runs from `ampHead` at the snout to full
// amplitude `ampRamp` of the body back. It costs a little exactness at the
// front, which is where a real head slips too.
//
// A CLIP THAT OWNS A SEGMENT WINS IT. The strike's coil is an override clip on
// the front segments; the wave yields to it there by the clip's blended weight,
// and the child of a yielded segment is laid relative to what that segment
// ACTUALLY got, so the boundary does not kink.
//
// Presentation only (pose.h): CPU float, never hashed, never on the grid.
namespace {
std::atomic<uint64_t> g_slitherCalls{0}, g_slitherNanos{0};
struct SlitherTimer {
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  ~SlitherTimer() {
    g_slitherCalls.fetch_add(1, std::memory_order_relaxed);
    g_slitherNanos.fetch_add(
        (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0)
            .count(),
        std::memory_order_relaxed);
  }
};
}  // namespace

uint64_t SlitherPoseCalls() { return g_slitherCalls.load(); }
uint64_t SlitherPoseNanos() { return g_slitherNanos.load(); }

void Mob::ApplySlither(const PoseInputs& in, float dt) {
  if (def_ == nullptr) return;
  const SlitherTimer timer;   // the `snake` gate's cost readout
  const MobDef::SlitherDef& sd = def_->slither;
  const AnimSkeleton& sk = skel_;
  AnimState& st = anim_;
  const size_t n = std::min(sk.parts.size(), st.local.size());
  if (!sd.present || sd.length <= 0.0f || n == 0) return;

  // ---- the odometer and the turn -----------------------------------------
  const Vec3 fwd{std::sin(heading_), 0, std::cos(heading_)};
  const float vFwd = in.velocity.x * fwd.x + in.velocity.z * fwd.z;
  if (!pose_.slitherInit) {
    pose_.slitherInit = true;
    pose_.slitherHeading = heading_;
    pose_.slitherKappa = 0.0f;
  }
  const float lambda = std::max(0.5f, sd.wavelength * sd.length);
  const float k = 6.2831853f / lambda;
  pose_.slitherOdo += vFwd * dt;
  // Wrapped to keep the float precise over a long life; the wave is periodic.
  if (std::fabs(pose_.slitherOdo) > 64.0f * lambda)
    pose_.slitherOdo = std::fmod(pose_.slitherOdo, lambda);
  float dh = heading_ - pose_.slitherHeading;
  while (dh > 3.14159265f) dh -= 6.28318531f;
  while (dh <= -3.14159265f) dh += 6.28318531f;
  pose_.slitherHeading = heading_;
  {
    const float vRef =
        std::max(std::fabs(vFwd), 0.5f * std::max(def_->speed, 0.5f));
    float want = (dh / std::max(dt, 1e-4f)) / vRef * sd.turnBend;
    want = std::clamp(want, -sd.maxCurvature, sd.maxCurvature);
    pose_.slitherKappa +=
        (want - pose_.slitherKappa) * HalfLifeK(dt, sd.bendHalfLife);
  }

  const float A = sd.amplitude * sd.length;
  const float phase = k * pose_.slitherOdo;
  const float ramp = std::max(1e-3f, sd.ampRamp * sd.length);
  const float sPivot = sd.zTip - def_->worldSize.z * 0.5f;
  const float kappa = pose_.slitherKappa;
  auto lateral = [&](float s) {
    float t = std::clamp(s / ramp, 0.0f, 1.0f);
    t = t * t * (3.0f - 2.0f * t);
    const float env = sd.ampHead + (1.0f - sd.ampHead) * t;
    const float u = s - sPivot;
    return env * A * std::sin(phase - k * s) + 0.5f * kappa * u * u;
  };
  // psi(s) = atan(dX/dz) = atan(-dX/ds): the yaw that takes the segment's +Z
  // onto the path's forward tangent (a positive turn about +Y takes +Z toward
  // +X, scripts/geometry.py).
  auto yawAt = [&](float s) {
    const float h = 0.25f;
    return std::atan(-(lateral(s + h) - lateral(s - h)) / (2.0f * h));
  };

  // ---- how much of each part the wave may have ----------------------------
  // 1 minus the blended weight of every live OVERRIDE clip whose mask names
  // it. Additive clips compose with the wave and take nothing from it.
  static thread_local std::vector<float> own;
  own.assign(n, 1.0f);
  for (const ClipInstance& inst : st.clips) {
    if (inst.clip < 0 || inst.clip >= (int)sk.clips.size()) continue;
    const AnimClip& clip = sk.clips[inst.clip];
    if (clip.mode != ClipMode::Override) continue;
    const float w = std::clamp(inst.weight * inst.fade, 0.0f, 1.0f);
    if (w <= 0.0f) continue;
    for (size_t i = 0; i < n; i++)
      if (clip.mask.empty() || (i < clip.mask.size() && clip.mask[i]))
        own[i] = std::max(0.0f, own[i] - w);
  }

  // ---- target yaw per chain part, then deltas down the hierarchy ----------
  static thread_local std::vector<float> yawAbs;
  static thread_local std::vector<uint8_t> inChain;
  yawAbs.assign(n, 0.0f);
  inChain.assign(n, 0);
  for (size_t c = 0; c < sd.parts.size() && c < sd.sMid.size(); c++) {
    const int p = sd.parts[c];
    if (p < 0 || (size_t)p >= n) continue;
    inChain[(size_t)p] = 1;
    yawAbs[(size_t)p] = own[(size_t)p] * yawAt(sd.sMid[c]);
  }
  // Parents are stored before children, so one ascending pass sees every
  // parent's absolute yaw before its child is laid relative to it.
  for (size_t i = 0; i < n; i++) {
    if (!inChain[i]) continue;
    const int par = sk.parts[i].parent;
    const float parentYaw =
        (par >= 0 && inChain[(size_t)par]) ? yawAbs[(size_t)par] : 0.0f;
    const float delta = yawAbs[i] - parentYaw;
    if (std::fabs(delta) > 1e-5f)
      st.local[i].rot = QuatNormalize(
          QuatMul(AxisAngle({0, 1, 0}, delta), st.local[i].rot));
    if ((int)i == def_->rootLimb)
      st.local[i].pos.x += own[i] * lateral(sd.sRootJoint);
  }
}


// ---- THE GAIT ----------------------------------------------------------------
//
// ONE solver. The player's version was the base (every one of its departures
// was a measured bug fix: the double-counted root anchor, the frozen swing
// target, PlayClip rewinding, the stride budget, the landing snap, the ankle-
// not-sole effector, the neediest-foot award). The NPC version contributed:
// group awards (a quadruped trot is a diagonal PAIR stepping together), the
// legacy-rig leg fallback (IsLegChain), and — because it is forced by who owns
// the body, not by taste — the feet-derived height with its ground authority
// and the four-probe ground tilt, selected by PoseInputs.
void Mob::ApplyHeldAim(const AnimSkeleton& sk, AnimState& st) const {
  const size_t n = std::min(sk.parts.size(), st.model.size());
  for (int k = 0; k < kHands; k++) {
    const HeldHand& hh = held_[k];
    const float w = std::clamp(hh.aimWeight, 0.0f, 1.0f);
    if (w <= 0.0f || hh.slot < 0 || (size_t)hh.slot >= n) continue;
    const int hand = sk.parts[hh.slot].parent;
    if (hand < 0 || (size_t)hand >= n) continue;
    const float al = hh.aimAxis.len();
    if (al < 1e-4f) continue;
    // THE WHOLE ORIENTATION, NOT JUST THE LONG AXIS. model.rot takes the
    // item's lattice into model space (LimbTargetFor), so its long axis is
    // model.rot * +X. This used to be QuatFromTo(that axis now, aim): the
    // minimal turn, which keeps whatever ROLL the forearm happened to give
    // the fist. With the arm raised that roll swings with every small change
    // of the solve, and the hand, the flask and (through the bench's closed
    // loop on the flask) the arm with it flipped from tick to tick. So the
    // item's frame is set outright: lattice +X along the aim and lattice +Y
    // toward the body's back (-Z, where the hold clip already has it for both
    // hands), taken off the aim — a function of the aim alone.
    const Vec3 ax = hh.aimAxis * (1.0f / al);
    Vec3 by = Vec3{0, 0, -1} - ax * (-ax.z);
    if (by.len() < 0.2f) by = Vec3{0, -1, 0} - ax * (-ax.y);   // aim along Z
    by = by.normalized();
    const Quat want = QuatFromBasis(ax, by, ax.cross(by));
    const Quat full = QuatMul(want, QuatConj(st.model[hh.slot].rot));
    const Quat q = QuatNormalize(QuatSlerp(Quat{}, full, w));
    // The hand and everything on it, turned about the WRIST — the point the
    // arm's IK placed — so the hand stays on the forearm and where the flask
    // ends up is a smooth function of the arm's command.
    const Vec3 pivot = st.model[hand].pos;
    static thread_local std::vector<uint8_t> moved;
    moved.assign(n, 0);
    for (size_t i = (size_t)hand; i < n; i++) {
      const int par = sk.parts[i].parent;
      if ((int)i != hand && (par < 0 || !moved[par])) continue;
      moved[i] = 1;
      st.model[i].pos = pivot + QuatRotate(q, st.model[i].pos - pivot);
      st.model[i].rot = QuatNormalize(QuatMul(q, st.model[i].rot));
    }
  }
}

void Mob::StepGait(const PoseInputs& in, float dt, World& world, uint32_t tick) {
  const AnimSkeleton& sk = skel_;
  const GaitDef& g = sk.gait;
  if (sk.chains.empty() || def_ == nullptr) return;
  const MobDef& def = *def_;
  AnimState& st = anim_;
  const bool driverHeight = in.height == PoseInputs::Height::FromDriver;

  const float speedFactor =
      std::clamp(speedNow_ / std::max(def.speed, 0.01f), 0.0f, 1.5f);

  // Stride clock: the time since the last touchdown, which the sync at the
  // bottom of the swing turns into a measured step period. Advanced here so it
  // keeps counting through a stride where no foot happens to land.
  pose_.sinceTouchdown += dt;

  // ---- THE STANCE CROUCH: where the stride's reach actually comes from -----
  //
  // When the DRIVER owns the height (PoseInputs::Height::FromDriver) the hip
  // sits at a FIXED height above the ground. That is the whole stride budget,
  // and on a rig authored standing it is essentially zero: the human's hip is
  // 7.50 above the min corner and its ankle 0.75, a 6.75 span against a
  // 6.79-voxel chain. A foot placed on the ground `s` voxels fore or aft of the
  // hip is sqrt(6.75^2 + s^2) away, so the leg runs out of reach at s = 0.7
  // VOXELS, and every longer step lands on AnimSolveTwoBone's reach clamp —
  // one identical straight-leg pose for every target beyond it. A real walker
  // buys the reach by bending the knees, so do that.
  //
  // BUT ONLY WHILE THE LEG IS ACTUALLY OUT AT THE END OF ITS STRIDE: the demand
  // is measured per FOOT, per tick, against where that foot actually is — the
  // highest hip that can still reach it, taken over the legs. A walking pelvis
  // is LOWEST at double support and HIGHEST at midstance; holding the double-
  // support height through the cycle is "walks on flat ground in a kneeling
  // pose" (0.77 voxels of permanent crouch, measured).
  //
  // Exactly ONE thing is taken from `f.planted`: its HORIZONTAL offset from the
  // hip, which descends from origin_.x/z and the velocity lead and is not a
  // function of bodyY_ — so this is not the height feedback path.
  //
  // Measured in both height modes (the readout means the same thing), applied
  // only in FromDriver: a feet-derived body's hip height is already the foot
  // average plus the rig's authored `rideHeight` stance.
  float crouchNeed = 0.0f;
  float crouchLegLength = 0.0f;

  // Exactly ONE gait group may swing at a time — that single constraint IS the
  // gait state machine. It generalizes to any leg count (two singleton groups =
  // a biped alternating, diagonal pairs = a quadruped trot) with no per-gait
  // table, and it degrades gracefully when a leg is severed.
  int swingingGroup = -1;
  for (size_t c = 0; c < st.feet.size() && c < sk.chains.size(); c++) {
    if (!st.feet[c].swinging) continue;
    for (size_t gi = 0; gi < g.groups.size(); gi++)
      for (int p : g.groups[gi])
        if (p == sk.chains[c].effector || p == sk.chains[c].parts[0])
          swingingGroup = (int)gi;
    if (swingingGroup < 0) swingingGroup = (int)c;  // ungrouped: its own group
  }

  // THE SWING SLOT IS AWARDED, NOT CLAIMED. See the note where the bids are
  // resolved, below the loop: whoever wants it most gets it.
  struct StepBid {
    size_t chain = 0;
    int group = -1;
    float drift = 0;
    Vec3 to{};
    // The surface under `to`, taken by the probe that chose it: the sound and
    // the print name the ground the foot was AIMED at (anim.h swingMat).
    uint32_t mat = 0;
  };
  StepBid bids[8];
  int nBids = 0;

  // The planted (not swinging) feet, for the FEET-DERIVED height below.
  float plantedSum = 0;
  int nPlanted = 0;
  const Vec3 pivot{def.worldSize.x * 0.5f, 0, def.worldSize.z * 0.5f};
  const Quat yaw = AxisAngle({0, 1, 0}, heading_);
  // THE IK EFFECTOR IS THE ANKLE, NOT THE SOLE.
  //
  // GroundHeightAt returns the SURFACE (the top face of the solid, `y + 1`),
  // which is the plane the min corner rests on. The chain the goal becomes a
  // target for ends at the ankle JOINT, `restSoleY_` above the min corner in
  // the authored pose. Handing the solver the surface asks for the full hip-to-
  // min-corner drop — on the human 7.50 voxels out of a 6.79-voxel chain — so
  // the solve clamped to its reach annulus on every tick of every walk. The
  // NPC copy handed it the surface and compensated in the body height instead
  // (`- restSoleY_`, below); that compensation is kept for the feet-derived
  // height so an NPC stands exactly where it stood, and the goal is now the
  // same point for everyone. `footTrim` rides here too — the probe and the
  // pose have to agree about where the foot is.
  const float ankleRise = restSoleY_ + CurrentTuning().avatar.footTrim;

  for (size_t c = 0; c < sk.chains.size() && c < st.feet.size(); c++) {
    const IkChain& ch = sk.chains[c];
    FootState& f = st.feet[c];
    // Not a leg: no contact point, and `valid = false` is also what makes the
    // IK pass give this chain zero weight, so an arm falls through to whatever
    // the clips and the weapon-arm override pose it as.
    if (!IsLegChain(sk, c)) {
      f.valid = false;
      f.swinging = false;
      continue;
    }
    // limb loss: stop scheduling this leg's steps entirely.
    bool alive = true;
    for (int p : ch.parts)
      if (p >= 0 && p < (int)st.partAlive.size() && !st.partAlive[p])
        alive = false;
    if (!alive) {
      f.valid = false;
      f.swinging = false;
      continue;
    }
    const bool wasValid = f.valid;
    f.valid = true;

    // Rest stance position for this foot, in world space. restLocal is ALREADY
    // the foot's anchor in PREFAB coordinates: each rest.pos is a joint anchor
    // measured from its parent's anchor, so the sum up through the root
    // telescopes to the effector's absolute prefab anchor. Adding the root
    // anchor on top double-counted it and slid both feet 2.5 voxels forward —
    // the "naruto run" (gotcha-gait-speed-vs-leg-length).
    Vec3 restLocal = sk.parts[ch.effector].rest.pos;
    for (int par = sk.parts[ch.effector].parent; par >= 0;
         par = sk.parts[par].parent)
      restLocal = restLocal + sk.parts[par].rest.pos;
    const Vec3 stance =
        Vec3{origin_.x, bodyY_, origin_.z} + pivot + Rotate(yaw, restLocal - pivot);
    // This chain's HIP, in world XZ (anchorLocal is prefab-absolute). Only the
    // HORIZONTAL is used — see the crouch block at the bottom of the loop.
    const Vec3 hipXZ =
        origin_ + pivot + Rotate(yaw, sk.parts[ch.parts[0]].anchorLocal - pivot);
    // Lead the target by the current velocity so the foot lands where the body
    // is GOING, not where it was.
    //
    // THE LEAD IS CLAMPED TO THE LEG'S REACH. `leadTime` is a duration, so the
    // raw offset grows without bound with speed; past the leg's reach the solve
    // clamps to its annulus and the leg simply points at the target — a
    // straight limb trailing the body. Capping at a fraction of leg length
    // keeps the same lean at walk pace and degrades to a reachable stride at a
    // sprint. (The NPC copy's `fwd * strideBias * legLength * speedFactor +
    // vel * leadTime` was unclamped: 6.0 voxels ahead at a human's walk against
    // a 6.1 cap, i.e. on the edge of reach every step.)
    Vec3 lead = Vec3{st.velocity.x, 0, st.velocity.z} *
                (g.leadTime + g.strideBias * 0.1f);
    const float maxLead = kMaxLeadLegLengths * f.legLength;
    const float leadLen = lead.len();
    if (leadLen > maxLead) lead = lead * (maxLead / leadLen);
    Vec3 goal = stance + lead;
    // Probe from just above the SOLE (origin_.y) and fall back to the sole when
    // the probe misses — never from bodyY_, which is what let the avatar's
    // height feed itself (gotcha-avatar-gait-height-feedback). kFootProbeLift
    // is the avatar's 2 cells written in metres; the NPC copy started 0.30 m
    // up. GroundHeightAt climbs out of matter it starts inside, so a step is
    // found from either start; the lower one only differs under an overhang
    // three cells up, where it finds the floor and the higher one the roof.
    int gy = 0;
    uint32_t gmat = 0;
    if (GroundHeightAt(world, ifloor(goal.x), ifloor(goal.z),
                       ifloor(origin_.y) + kFootProbeLift, gy, &gmat))
      goal.y = (float)gy + ankleRise;
    else
      goal.y = origin_.y + ankleRise;

    // First frame, a landing (ParkGaitForAir clears footInit_) or a leg that
    // just came back: plant where the goal is, and do not step off a plant
    // left over from before.
    if (!footInit_ || !wasValid) {
      f.planted = goal;
      f.swinging = false;
    }
    if (f.swinging) {
      // SWING TIME IS BOUNDED BY THE STRIDE BUDGET (kSwingTravelFrac): the
      // speed-scaled duration is the DESIRED swing and the budget caps it, so
      // the swing never lasts longer than it takes the body to consume the
      // ground the foot is able to gain. At low speed the budget is enormous
      // and the speed scaling governs, so a slow walk is unaffected.
      const float durScale = std::clamp(speedFactor, kMinSwingScale, 1.5f);
      float dur = std::max(g.stepDuration / durScale, kMinSwingSeconds);
      const float reach = kMaxLeadLegLengths * f.legLength;
      if (speedNow_ > 0.01f) dur = std::min(dur, kSwingTravelFrac * reach / speedNow_);
      dur = std::max(dur, kMinSwingSeconds);
      f.swingT += dt / dur;
      if (f.swingT >= 1.0f) {
        // Land ON the target: at t = 1 the arc's lift term is sin(pi) = 0 (and
        // the land ease below has already shed it), so assigning swingTo is
        // continuous with the previous sample rather than a drop — the 45-48
        // degree leg snap per step the avatar measured going uphill.
        f.swingT = 0;
        f.swinging = false;
        f.planted = f.swingTo;
        // THE FOOTFALL: the gait's own touchdown moment, so a step is heard
        // exactly when the art shows the foot land, and a severed leg stops
        // producing steps for free.
        Footfall ff;
        ff.posVox = f.swingTo;
        ff.mat = f.swingMat;
        ff.speed = speedNow_;
        ff.foot = (int)c;
        if (ff.mat != 0) PushFootfall(ff);
        // ...AND WHAT IS ON THE FOOT COMES OFF ON THE FLOOR. Tick-side, at the
        // plant itself: a print is world state, and keying it on the tick is
        // what keeps a bloody walk the same at 30 fps and at 200.
        ShedCoat(ch.effector, f.planted, tick, world);
        // ...and the stride clock's only input.
        SyncStrideClock((int)c);
      } else {
        // RE-TARGET THE SWING WHILE IT IS IN THE AIR. Freezing swingTo at
        // lift-off starved the gait: the body travels further during a swing
        // than the frozen lead, so every step lost ground. Weighted by t so the
        // early swing keeps its committed direction and the late swing homes
        // in on where the body actually is.
        f.swingTo = f.swingTo * (1.0f - f.swingT) + goal * f.swingT;
        // Parabolic arc between lift-off and touch-down, the lift EASED OUT
        // over the last quarter so the foot arrives flat on the target
        // whatever t the tick lands on.
        const float t = f.swingT;
        Vec3 flat = f.swingFrom * (1.0f - t) + f.swingTo * t;
        float lift = std::sin(t * 3.14159265f) * g.stepHeight * f.legLength;
        const float kLandEase = 0.75f;   // lift is fully shed by t = 1
        if (t > kLandEase)
          lift *= std::max(0.0f, (1.0f - t) / (1.0f - kLandEase));
        flat.y += lift;
        f.planted = flat;  // `planted` doubles as the current foot target
      }
    } else {
      // This foot WANTS to step. Whether it gets to is decided after the loop,
      // once every leg has said how badly it needs the slot. A body at a
      // standstill does not step off a sub-threshold creep (`speedFactor`).
      const float drift = Vec3{goal.x - f.planted.x, 0, goal.z - f.planted.z}.len();
      if (drift > g.stepThreshold * f.legLength && speedFactor > 0.05f &&
          nBids < 8) {
        int myGroup = -1;
        for (size_t gi = 0; gi < g.groups.size(); gi++)
          for (int p : g.groups[gi])
            if (p == ch.effector || p == ch.parts[0]) myGroup = (int)gi;
        bids[nBids++] = StepBid{c, myGroup >= 0 ? myGroup : (int)c, drift, goal, gmat};
      }
      plantedSum += f.planted.y;
      nPlanted++;
    }
    // ---- this leg's share of the stance crouch (see the note at the top) ----
    // How far the hip must come down to reach THIS foot at THIS excursion: the
    // horizontal separation is spent out of the leg's reach, and what is left
    // over is the vertical the hip may keep. Whichever leg wants the lowest hip
    // wins. ONLY THE HORIZONTAL COMES FROM THE FOOT; the vertical is the rig's
    // own span (a foot height would put bodyY_ downstream of the probe).
    {
      const float reach = f.legLength * kStanceReachFrac;
      const float dx = hipXZ.x - f.planted.x;
      const float dz = hipXZ.z - f.planted.z;
      const float vert =
          std::sqrt(std::max(reach * reach - (dx * dx + dz * dz), 0.0f));
      crouchNeed = std::max(crouchNeed, (restHipY_ - restSoleY_) - vert);
      crouchLegLength = std::max(crouchLegLength, f.legLength);
    }
  }

  // ---- WHO GETS TO STEP -----------------------------------------------------
  //
  // Exactly one GROUP may swing at a time. The slot is AWARDED TO THE NEEDIEST
  // LEG — the largest drift past threshold wins — not claimed by whichever
  // chain the loop reached first: that kept chain 0 re-claiming it forever
  // (a leg re-bids the instant it lands), one foot welded to the bottom of a
  // hill while the other climbed (NPC, `ai-slope`) and one leg raking out
  // behind the body (avatar, "naruto run"). Self-balancing, any leg count,
  // ties keep list order so it stays deterministic. Every member of the
  // winning group steps together: that is what makes a diagonal pair a trot.
  if (nBids > 0) {
    if (swingingGroup < 0) {
      const StepBid* best = &bids[0];
      for (int i = 1; i < nBids; i++)
        if (bids[i].drift > best->drift) best = &bids[i];
      swingingGroup = best->group;
    }
    for (int i = 0; i < nBids; i++) {
      const StepBid& b = bids[i];
      if (b.group != swingingGroup) continue;
      FootState& f = st.feet[b.chain];
      f.swinging = true;
      f.swingT = 0;
      f.swingFrom = f.planted;
      f.swingTo = b.to;
      f.swingMat = b.mat;
    }
  }

  // ---- commit the crouch ---------------------------------------------------
  // ---- THE GOOD KNEE PAYS FOR THE STUMP'S CLEARANCE ----------------------
  // Rolling the pelvis into the short side drops that hip by (hip half-width) *
  // sin(lean) — under a third of a voxel against a whole foot of missing leg.
  // The rest comes from BENDING THE STANDING KNEE, which is what the stance
  // crouch already is: a FLOOR under the walking demand, not a term added to it
  // (the stride's own crouch already oscillates up to the same clamp).
  if (dragW_ > 0.0f && crouchLegLength > 0.0f)
    crouchNeed = std::max(crouchNeed, dragW_ * crouchLegLength * kDragSinkLegLengths);
  crouchNeed = std::clamp(crouchNeed, 0.0f, kMaxCrouchLegLengths * crouchLegLength);
  // Eased on a SHORT half-life, which the per-foot form above makes safe (it is
  // continuous by construction) and which must stay short: the demand
  // oscillates ONCE PER STEP, and a half-life near the step period would
  // average that back into the constant crouch it replaced — EXCEPT while a
  // stump is dragging. On one leg the surviving foot takes every step, the
  // period doubles, the excursion is the whole stride's reach, and a 0.05 s
  // filter passes all of it — "it hops endlessly" (project-stump-drag). A
  // dragging body is carried by a leg it never picks up, so the pump is
  // low-passed away in proportion to how committed to the drag it is.
  const float crouchHl =
      kCrouchHalflife + (kDragCrouchHalflife - kCrouchHalflife) * dragW_;
  pose_.stanceCrouch += (crouchNeed - pose_.stanceCrouch) * HalfLifeK(dt, crouchHl);

  if (driverHeight) {
    // BODY HEIGHT COMES FROM THE DRIVER, NOT FROM THE FEET. The player's AABB
    // already resolved against the terrain this tick, so origin_.y IS the sole
    // of the boot, and re-deriving it from feet whose goal falls back to that
    // same height climbs ~9.5 voxels a tick ("the wizard is 100 feet above the
    // player"). The feet supply only the things the AABB cannot know. The
    // CROUCH is subtracted here and nowhere else — the leg IK targets are
    // WORLD points, so lowering the pelvis pushes the bend into the knees and
    // leaves the feet exactly where the gait put them.
    bodyY_ = origin_.y - pose_.stanceCrouch - pose_.crouchHold;
  } else if (nPlanted > 0) {
    // ---- BODY FROM FEET ------------------------------------------------------
    // Height is DERIVED from where the feet actually are, so a creature walking
    // up a voxel staircase rises correctly with no slope code, and the drawn
    // body eases over a step its ground-snapped `origin_` climbs in one tick.
    //
    // bodyY is the prefab MIN CORNER, so the foot average is converted out of
    // the ankle frame back to the surface (`- ankleRise`) and then by the rig's
    // rest sole height (`- restSoleY_`) — the NPC's historical placement, which
    // `rideHeight` (a CROUCH/STRETCH about that stance, 1.0 = authored height)
    // was tuned against on every current def. A LEG's length, not chain zero's.
    float legLen = st.feet[0].legLength;
    for (size_t c = 0; c < sk.chains.size() && c < st.feet.size(); c++)
      if (IsLegChain(sk, c)) {
        legLen = st.feet[c].legLength;
        break;
      }
    const float stance = (g.rideHeight - 1.0f) * legLen;
    float targetY =
        plantedSum / (float)nPlanted - ankleRise - restSoleY_ + stance;
    // ---- THE FEET ARE A DETAIL, THE GROUND IS THE TRUTH ---------------------
    // On a slope every voxel of foot staleness is a voxel of height; the
    // average of one fresh foot and one a stride behind put the DRAWN body ten
    // voxels under the surface while the collider rode the slope correctly
    // ("NPCs walk into hills"). So the average is BOUNDED against where the
    // body would stand on the ground its own footprint reports — asymmetrically:
    // a foot on a HIGHER step legitimately lifts the body (up to the drift that
    // triggers a step); below the ground is not a pose, it is the bug.
    const float groundTarget = origin_.y - restSoleY_ + stance;
    const float upAuthority =
        std::max(1.0f, legLen * std::max(0.15f, g.stepThreshold));
    const float downAuthority = std::max(0.5f, legLen * 0.08f);
    targetY = std::clamp(targetY, groundTarget - downAuthority,
                         groundTarget + upAuthority);
    // THE GOOD KNEE PAYS FOR THE STUMP'S CLEARANCE here too, AFTER the
    // authority clamp on purpose: a deliberate crouch, not the stale-foot error
    // that clamp exists to bound.
    if (dragW_ > 0.0f) targetY -= dragW_ * legLen * kDragSinkLegLengths;
    // SANDVOX_GAIT_DEBUG=1: where the body height is actually coming from —
    // "the drawn body is underground" has three causes that look identical from
    // outside (the ease lagging, the feet not stepping, the rig offsets wrong).
    static const bool kGaitDebug = std::getenv("SANDVOX_GAIT_DEBUG") != nullptr;
    if (kGaitDebug) {
      std::printf("gait: origin %.2f body %.2f target %.2f | feet %d/%zu avg "
                  "%.2f sole %.2f stance %.2f legLen %.2f |",
                  origin_.y, bodyY_, targetY, nPlanted, st.feet.size(),
                  plantedSum / (float)nPlanted, restSoleY_, stance, legLen);
      for (size_t c2 = 0; c2 < sk.chains.size() && c2 < st.feet.size(); c2++)
        std::printf(" [%s%s%s y%.1f d%.2f]", sk.chains[c2].tag.c_str(),
                    st.feet[c2].valid ? "" : "!", st.feet[c2].swinging ? "~" : "",
                    st.feet[c2].planted.y, st.feet[c2].legLength * g.stepThreshold);
      std::printf("\n");
    }
    // Snapped only the FIRST time this body is ever placed. `footInit_` is
    // also cleared by every landing (ParkGaitForAir, so the feet re-plant),
    // and snapping the drawn height there would pop the body by the stance
    // offset the tick it touched down; the NPC copy only ever cleared it at
    // spawn, and eased every landing.
    if (!pose_.bodyPlaced) bodyY_ = targetY;
    else MobSystem::EaseBodyY(*this, targetY, dt);
  } else {
    MobSystem::EaseBodyY(*this, origin_.y, dt);
  }

  // ---- BODY TILT ------------------------------------------------------------
  Vec3 targetUp{0, 1, 0};
  if (in.tiltFromGround) {
    // FROM THE GROUND, IN THE BODY'S OWN FRAME, CLAMPED. A tilt fitted through
    // the planted feet never fired on a biped (two contacts), jittered with a
    // quadruped's swing, admitted 53 degrees of pure roll and laid the body
    // parallel to the hill. Four probes fore/aft and left/right of the
    // footprint give a pitch and a roll directly, in the body's own frame,
    // under ONE ceiling on the total lean (LocomotionDef::tiltMaxDeg) —
    // clamping each axis separately lets a corner combine both into sqrt(2)x.
    const Vec3 fwd{std::sin(heading_), 0, std::cos(heading_)};
    const Vec3 rgt{std::cos(heading_), 0, -std::sin(heading_)};
    const float span =
        std::max(1.5f, std::max(def.worldSize.x, def.worldSize.z) * 0.5f);
    const float cx = origin_.x + def.worldSize.x * 0.5f;
    const float cz = origin_.z + def.worldSize.z * 0.5f;
    const int yFrom = ifloor(origin_.y) + kMobProbeLiftCells;
    // The GRADIENT under the body as a rise over a run. Unknown ground is FLAT
    // ground here, not a cliff — the rule the whole locomotion layer lives by.
    auto grade = [&](Vec3 dir) {
      int hi = 0, lo = 0;
      const bool okHi = GroundHeightAt(world, ifloor(cx + dir.x * span),
                                       ifloor(cz + dir.z * span), yFrom, hi);
      const bool okLo = GroundHeightAt(world, ifloor(cx - dir.x * span),
                                       ifloor(cz - dir.z * span), yFrom, lo);
      return (okHi && okLo) ? (float)(hi - lo) / (2.0f * span) : 0.0f;
    };
    // Negated because `up` leans AWAY from the rise.
    Vec3 lean = fwd * -grade(fwd) + rgt * -grade(rgt);
    const float leanMax = std::tan(
        std::clamp(skel_.loco.tiltMaxDeg, 0.0f, 60.0f) * 0.0174532925f);
    const float leanLen = lean.len();
    if (leanLen > leanMax) lean = lean * (leanMax / std::max(leanLen, 1e-6f));
    targetUp = (Vec3{0, 1, 0} + lean).normalized();
    if (targetUp.y < 0.5f) targetUp = Vec3{0, 1, 0};
  }
  // Upright otherwise. A player's torso does not lean into the grade today —
  // the avatar never had a slope tilt — and giving it one is a visible change
  // to the player that W2-L was not asked to make; `tiltFromGround` is the
  // switch when somebody wants it.
  // Eased toward the target so stepping onto a new block does not snap.
  bodyUp_ = (bodyUp_ * 0.85f + targetUp * 0.15f).normalized();
  if (bodyUp_.len() < 0.5f) bodyUp_ = {0, 1, 0};
  footInit_ = true;
}

// ---- the stride clock ------------------------------------------------------
//
// TWO CLOCKS IS THE BUG. The feet step on a DRIFT THRESHOLD while the pelvis
// bob, sway and roll ran off `gaitPhase += dt * cadence * speedFactor`, a free-
// running oscillator that knows nothing about the feet. On the stock human at
// walk pace the runtime's own step model gives 3.09 footfalls/s against 8.13 Hz
// of bob — under four samples a cycle at a 30 Hz tick, i.e. "the body sways
// left and right really fast and it's jittery". It cannot be tuned out, because
// the two clocks disagree by a ratio that itself moves with speed.
//
// So there is one clock and the FEET own it. The phase still advances smoothly
// (the bob must not step), but its RATE is the measured stride rate and its
// PHASE is pulled onto a half-turn boundary at every touchdown. bobFreqMul 2
// then means "once per footfall" by construction. A partial correction rather
// than a snap: once locked the residual is a few milliseconds and invisible.
void Mob::SyncStrideClock(int chain) {
  const AnimSkeleton& sk = skel_;
  // Which leg is this, among the leg chains? Ordinal, not chain index: a rig
  // whose arm chains are interleaved with its legs must still split the stride
  // evenly between the legs that actually step.
  int legOrdinal = 0, nLegs = 0, mine = 0;
  for (size_t c = 0; c < sk.chains.size(); c++) {
    if (!IsLegChain(sk, c)) continue;
    if ((int)c == chain) mine = legOrdinal;
    legOrdinal++;
    nLegs++;
  }
  if (nLegs <= 0) return;

  const float elapsed = pose_.sinceTouchdown;
  pose_.sinceTouchdown = 0.0f;
  // A first touchdown, or one after a stop/jump, has no period to measure —
  // adopt the boundary outright and wait for the next step to time the rate.
  const bool haveStep =
      pose_.lastFootDown >= 0 && elapsed > 1e-3f && elapsed < kMaxStepPeriod;
  pose_.lastFootDown = chain;
  if (haveStep) {
    const float k = 1.0f - std::pow(0.5f, elapsed / kStepPeriodHalflife);
    pose_.stepPeriod += (elapsed - pose_.stepPeriod) * k;
    // A stride is nLegs steps (two, for a biped): every leg lands once.
    const float stride = pose_.stepPeriod * (float)nLegs;
    if (stride > 1e-3f) pose_.strideRate = 1.0f / stride;
  } else {
    pose_.stepPeriod = 0.0f;
  }
  // Pull the phase onto this leg's boundary, the short way round.
  const float want = (float)mine / (float)nLegs;
  float err = want - anim_.gaitPhase;
  err -= std::floor(err + 0.5f);
  anim_.gaitPhase += err * (haveStep ? kStrideSyncGain : 1.0f);
  anim_.gaitPhase -= std::floor(anim_.gaitPhase);
}

// THE LEGS MUST NOT BE IK-DRIVEN AT A STALE PLANT IN MID-AIR.
//
// `f.planted` is a WORLD-SPACE point. On the ground that is exactly right; in
// the air it is a trap: nothing re-plants the foot, `planted` stays where the
// ground USED to be while the body plummets away, and within a few ticks that
// point is ABOVE the hip — the solver aims the legs up and folds them through
// the pelvis ("legs invert entirely and fall upside down inside the model").
// There is no sensible foot target from the ground in the air, so we do not
// invent one: the swings are parked, the air pose (if eligible) takes the
// chains, and the first grounded tick re-plants from scratch.
void Mob::ParkGaitForAir(const PoseInputs& in, float dt) {
  // The crouch is a WALKING pose; ease it out while there is no ground under
  // the feet, so landing does not have to unwind a stance the air never used.
  pose_.stanceCrouch *= std::pow(0.5f, dt / 0.12f);
  for (FootState& f : anim_.feet) {
    f.swinging = false;
    f.swingT = 0;
  }
  // Force a fresh plant on landing: the first grounded tick would otherwise
  // measure drift against a plant left over from before take-off — anywhere in
  // the world — and immediately fire a bogus step (and a bogus footstep).
  footInit_ = false;
  // A landing re-times the stride from scratch: the elapsed time across a jump
  // is not a step period.
  pose_.lastFootDown = -1;
  pose_.sinceTouchdown = 0.0f;
  if (in.height == PoseInputs::Height::FromDriver)
    bodyY_ = origin_.y - pose_.stanceCrouch - pose_.crouchHold;
  else
    MobSystem::EaseBodyY(*this, origin_.y, dt);
  bodyUp_ = (bodyUp_ * 0.85f + Vec3{0, 1, 0} * 0.15f).normalized();
  if (bodyUp_.len() < 0.5f) bodyUp_ = {0, 1, 0};
}

// ---- the airborne pose: drive ----------------------------------------------
//
// THE PHASE OF A JUMP IS `vel.y`, NOT A CLOCK. See avatar.airPose in tuning.h
// for the whole argument; the short form is that the old air pose was a 900 ms
// LOOPING clip whose keyframes are ten degrees apart, so a jump, a hop and a
// hundred-metre drop all played the same slow sway. This one signed number
// selects between four shapes, driven through the SAME leg and arm chains the
// gait and the ledge hang use, so no rig needs an authored keyframe for it.
void Mob::UpdateAirDrive(const PoseInputs& in, float dt, World& world,
                         bool clipOwnsPose) {
  const auto& av = CurrentTuning().avatar;
  // `grounded` is the driver's debounced view: the air pose must not start on
  // the flicker a bump crest produces.
  const bool active = av.airPose && !in.grounded && !clipOwnsPose &&
                      in.airPoseEligible && alive_ && !Ragdolled();

  const float hl = av.ikBlendHalflife;
  pose_.airFrac += ((active ? 1.0f : 0.0f) - pose_.airFrac) * HalfLifeK(dt, hl);
  if (pose_.airFrac < 1e-3f) pose_.airFrac = 0.0f;
  if (pose_.airFrac > 0.999f) pose_.airFrac = 1.0f;
  if (pose_.airFrac <= 0.0f) {
    // Parked, not merely unused: airVy is fed the live velocity so the FIRST
    // airborne tick already carries the launch spike. Seeding it from zero
    // would spend the whole rise easing up to a value the body only has at
    // take-off, and the tuck would never appear on a short hop.
    pose_.airVy = in.velocity.y;
    pose_.airLandW = 0.0f;
    pose_.airLeanPitch = pose_.airLeanRoll = 0.0f;
    pose_.airKey = AirKeyPose{};
    return;
  }

  // Lightly smoothed, on a half-life a third of the velocity filter's: the
  // launch spike is the most expressive thing in a jump and must survive.
  {
    const float vhl = std::max(av.velocityHalflife, 0.0f) * 0.33f;
    pose_.airVy += (in.velocity.y - pose_.airVy) * HalfLifeK(dt, vhl);
  }

  // ---- phase: rise / float / fall -----------------------------------------
  const float vRise = MetresPerSecToCells(av.airPoseRiseSpeed);
  const float vFall = MetresPerSecToCells(av.airPoseFallSpeed);
  const float rise = std::clamp(pose_.airVy / vRise, 0.0f, 1.0f);
  const float fall = std::clamp(-pose_.airVy / vFall, 0.0f, 1.0f);
  AirKeyPose key = rise > 0.0f ? MixAir(kAirFloat, kAirTuck, rise)
                               : MixAir(kAirFloat, kAirReach, fall);

  // ---- the ground coming up -----------------------------------------------
  // ONE probe per tick, and only while descending. A probe that finds nothing
  // (the CPU mirror is 3x3x3 chunks and a fast fall outruns it) leaves the
  // weight where it was — no answer is not the answer "no".
  const float landReach = MetresToCells(av.airPoseLandHeight);
  float landWant = 0.0f;
  if (landReach > 1e-3f && pose_.airVy < 0.0f && def_) {
    const Vec3 pivot{def_->worldSize.x * 0.5f, 0, def_->worldSize.z * 0.5f};
    int gy = 0;
    if (GroundHeightAt(world, ifloor(origin_.x + pivot.x),
                       ifloor(origin_.z + pivot.z),
                       ifloor(origin_.y) + kFootProbeLift, gy)) {
      // GroundHeightAt returns the SURFACE and origin_.y is the sole, so this
      // is the gap the feet still have to fall. SQUARED so the prepare bites
      // near the ground instead of tinting the whole descent.
      const float gap = origin_.y - (float)gy;
      const float t = std::clamp(1.0f - gap / landReach, 0.0f, 1.0f);
      landWant = t * t;
    } else {
      landWant = pose_.airLandW;
    }
  }
  // Eased on the IK half-life; rising quicker than falling, for the reason a
  // landing must commit and a take-off may drift out.
  {
    const float lhl =
        std::max(hl, 1e-4f) * (landWant > pose_.airLandW ? 0.6f : 1.4f);
    pose_.airLandW += (landWant - pose_.airLandW) * HalfLifeK(dt, lhl);
  }
  if (pose_.airLandW > 1e-3f) key = MixAir(key, kAirPrepare, pose_.airLandW);

  // ---- lean into the travel ------------------------------------------------
  // A running jump tips forward and banks into a sideways drift; a standing
  // one does not, because the lean is scaled by the velocity that earns it.
  const float leanMax = av.airPoseLean * 3.14159265f / 180.0f;
  float fwdFrac = 0.0f, latFrac = 0.0f;
  if (leanMax > 1e-4f && def_ && def_->speed > 0.01f) {
    const float c = std::cos(heading_), s = std::sin(heading_);
    // heading 0 = +Z; model +X is the character's LEFT.
    const Vec3 fwd{s, 0, c}, left{c, 0, -s};
    const Vec3 v = anim_.velocity;
    const float ref = def_->speed;
    fwdFrac = std::clamp((v.x * fwd.x + v.z * fwd.z) / ref, -1.0f, 1.0f);
    latFrac = std::clamp((v.x * left.x + v.z * left.z) / ref, -1.0f, 1.0f);
  }
  pose_.airLeanPitch = (key.lean + fwdFrac * leanMax) * pose_.airFrac;
  // Positive rotation about model +Z lifts the LEFT side — a lean to the
  // RIGHT — so travelling left must lean left, hence the negation.
  pose_.airLeanRoll = -latFrac * leanMax * 0.6f * pose_.airFrac;
  pose_.airKey = key;
}

// The air lean, HALF AT THE PELVIS, HALF UP THE BACK. All on the root tilts the
// rig as one plank; all on the spine leaves level hips under a folded chest.
void Mob::ApplyAirLean(const AnimSkeleton& sk, AnimState& st) {
  if (pose_.airFrac <= 0.0f || def_ == nullptr) return;
  if (std::fabs(pose_.airLeanPitch) <= 1e-4f && std::fabs(pose_.airLeanRoll) <= 1e-4f)
    return;
  const Quat halfLean = Mul(AxisAngle({1, 0, 0}, pose_.airLeanPitch * 0.5f),
                            AxisAngle({0, 0, 1}, pose_.airLeanRoll * 0.5f));
  const int root = def_->rootLimb;
  if (root >= 0 && root < (int)sk.parts.size() &&
      (root >= (int)st.partAlive.size() || st.partAlive[root]))
    st.local[root].rot = QuatNormalize(Mul(st.local[root].rot, halfLean));
  // Split across however many spine joints the rig has above the root, so a
  // three-segment back leans the same TOTAL as a single torso.
  int nSpine = 0;
  for (size_t i = 0; i < sk.parts.size(); i++)
    if (sk.parts[i].tag == "spine" && (int)i != root) nSpine++;
  if (nSpine > 0) {
    const Quat per =
        Mul(AxisAngle({1, 0, 0}, pose_.airLeanPitch * 0.5f / nSpine),
            AxisAngle({0, 0, 1}, pose_.airLeanRoll * 0.5f / nSpine));
    for (size_t i = 0; i < sk.parts.size(); i++) {
      if (sk.parts[i].tag != "spine" || (int)i == root) continue;
      if (i < st.partAlive.size() && !st.partAlive[i]) continue;
      st.local[i].rot = QuatNormalize(Mul(st.local[i].rot, per));
    }
  }
}

// ---- the airborne pose: the arms -------------------------------------------
//
// COMPOSED ONTO THE LOCAL POSE, BEFORE THE FLATTEN — not solved as IK like the
// legs (the key table's "legs are solved, arms are posed" note). Runs before
// the weapon arm and the ledge hang, both of which are post-flatten IK and
// therefore override it outright: a swing and a grab are things the driver
// asked for, and the air pose is what the arms do when nothing else has an
// opinion.
void Mob::ApplyAirArms(const AnimSkeleton& sk, AnimState& st) {
  if (pose_.airFrac <= 0.0f || !def_ || sk.chains.empty()) return;
  const Vec3 pivot{def_->worldSize.x * 0.5f, 0, def_->worldSize.z * 0.5f};
  const AirKeyPose& key = pose_.airKey;
  int armOrdinal = 0;
  for (size_t c = 0; c < sk.chains.size(); c++) {
    const IkChain& ch = sk.chains[c];
    if (ch.tag != "arm" || ch.parts.size() < 2) continue;
    // OPPOSITE the leg of the same ordinal (the leg scissor is
    // `(ordinal & 1) ? -1 : +1`), written against an ORDINAL for the reason
    // SyncStrideClock counts legs the same way.
    const float swing = (armOrdinal++ & 1) ? 1.0f : -1.0f;
    const int sh = ch.parts[0], el = ch.parts[1];
    if (sh >= (int)st.local.size() || el >= (int)st.local.size()) continue;
    // A lost arm's chain has gone silent; so must this.
    bool alive = true;
    for (int p : ch.parts)
      if (p >= 0 && p < (int)st.partAlive.size() && !st.partAlive[p])
        alive = false;
    if (!alive) continue;
    const float side = sk.parts[sh].anchorLocal.x >= pivot.x ? 1.0f : -1.0f;
    const float w = ch.weight * pose_.airFrac;
    // SIGNS, VERIFIED AGAINST THE RIG'S OWN AUTHORED LIMITS: a positive
    // rotation about model +X swings a hanging limb BACKWARD on these rigs, so
    // forward is -X; a positive rotation about +Z carries it toward +X (the
    // character's LEFT), so abducting AWAY from the midline is `side`.
    const float pitch = (key.armPitch + swing * key.armSwing) * w;
    const Quat shoulder = Mul(AxisAngle({1, 0, 0}, -pitch),
                              AxisAngle({0, 0, 1}, side * key.armOut * w));
    st.local[sh].rot = QuatNormalize(Mul(st.local[sh].rot, shoulder));
    // The elbow only flexes, and only one way: a hinge about -X.
    st.local[el].rot = QuatNormalize(
        Mul(st.local[el].rot, AxisAngle({1, 0, 0}, -key.armFlex * w)));
  }
}

// ---- ledge-hang arms: pin the PALMS to the held lip -------------------------
//
// The hang clip gets the arms into the neighbourhood; this solve makes the
// contact EXACT, on any rig out of the box: chain lengths come from the
// skeleton, the hand spread from each chain's own shoulder anchor, and the
// contact point is the ITEM SOCKET — the frame a sword hangs from — so "where
// does this rig's palm sit inside its hand part" is answered by the rig.
//
// POST-PROCESS, LIKE THE LEGS AND THE WEAPON ARM, and faded through
// hangIkWeight for the reason gaitWeight exists: the ticks around a grab and a
// release must blend. Targets are WORLD points un-yawed the way the legs
// un-yaw `planted`, so if the body yaws inside its hold cone the hands stay on
// the lip and the shoulders turn under them.
void Mob::ApplyHangArms(const PoseInputs& in, float dt, const AnimSkeleton& sk,
                        AnimState& st) {
  {
    // A ledge climb keeps the palms on the lip through the muscle-up: the
    // body rises past hands that stay put, which IS the arms travelling from
    // overhead, through the pull, to pressing down at the sides. Once the
    // knee has the weight they let go and ease back to the clip's (idle)
    // arms — the drift down to the sides.
    float want = in.hangActive ? 1.0f : 0.0f;
    if (!in.hangActive && in.climbActive)
      want = 1.0f - Smooth01((in.climbRise - ledgeclimb::kHKneeOn) /
                             (ledgeclimb::kHKneel + 0.18f - ledgeclimb::kHKneeOn));
    pose_.hangIkWeight += (want - pose_.hangIkWeight) *
                          HalfLifeK(dt, CurrentTuning().avatar.ikBlendHalflife);
    if (pose_.hangIkWeight < 1e-3f) pose_.hangIkWeight = 0.0f;
    if (pose_.hangIkWeight > 0.999f) pose_.hangIkWeight = 1.0f;
  }
  if (pose_.hangIkWeight <= 0.0f || sk.chains.empty() || def_ == nullptr) return;
  const Quat yaw = AxisAngle({0, 1, 0}, heading_);
  const Vec3 pivot{def_->worldSize.x * 0.5f, 0, def_->worldSize.z * 0.5f};
  const Vec3 bodyOrigin{origin_.x, bodyY_, origin_.z};
  // The body's centre column in world, and the wall geometry off the lip.
  const float cx = origin_.x + pivot.x, cz = origin_.z + pivot.z;
  const Vec3 dir = in.hangDir;
  // Model +X is the character's LEFT on these rigs; facing `dir`, that
  // lateral maps to (dir.z, 0, -dir.x) in world.
  const Vec3 leftW{dir.z, 0.0f, -dir.x};
  const float lipTopY = (float)(in.hangLip.y + 1);
  // How far ahead the lip actually is, measured to the held cell, aimed at its
  // NEAR TOP CORNER plus a finger's width onto the surface. Metres on both
  // bounds (a mixed-unit clamp halved the setback at 5 cm and the palm stopped
  // landing on the lip).
  const float aheadRaw = ((float)in.hangLip.x + 0.5f - cx) * dir.x +
                         ((float)in.hangLip.z + 0.5f - cz) * dir.z;
  const float ahead = std::clamp(aheadRaw - MetresToCells(0.055f),
                                 Player::kHalfXZ * 0.6f,
                                 Player::kHalfXZ + MetresToCells(0.12f));
  for (size_t c = 0; c < sk.chains.size(); c++) {
    const IkChain& ch = sk.chains[c];
    if (ch.tag != "arm" || ch.parts.empty() || ch.effector < 0) continue;
    const float weight = ch.weight * pose_.hangIkWeight;
    if (weight <= 0) continue;
    const Vec3 shoulder = sk.parts[ch.parts[0]].anchorLocal;
    const float latM = shoulder.x - pivot.x;  // this arm's own spread
    // Palm rest point on the lip: fingers just over the top surface.
    const Vec3 palmW{cx + leftW.x * latM + dir.x * ahead,
                     lipTopY + MetresToCells(0.02f),
                     cz + leftW.z * latM + dir.z * ahead};
    const Vec3 rel = palmW - bodyOrigin - pivot;
    const Vec3 palmPrefab = RotateInv(yaw, rel) + pivot;
    // The solver places the EFFECTOR's joint (the wrist). The grip point is the
    // hand's item socket — aim the wrist short of the lip by exactly that. No
    // socket on THIS hand: borrow another ARM's hand's and mirror its lateral
    // (a socket on anything that is not a hand must not be borrowed).
    Vec3 sockLocal{};
    bool found = false, mirrored = false;
    for (const MobSocketDef& sock : def_->sockets) {
      if (sock.partIndex < 0) continue;
      if (sock.partIndex == ch.effector) {
        sockLocal = sock.offset - limbs_[sock.partIndex].restOffset;
        found = true;
        mirrored = false;
        break;
      }
      if (found) continue;
      for (const IkChain& other : sk.chains) {
        if (other.tag == "arm" && other.effector == sock.partIndex) {
          sockLocal = sock.offset - limbs_[sock.partIndex].restOffset;
          found = true;
          mirrored = true;
          break;
        }
      }
    }
    if (mirrored) sockLocal.x = -sockLocal.x;
    if (ch.parts.size() < 2) continue;
    const int i0 = ch.parts[0], i1 = ch.parts[1], ie = ch.effector;
    if (ie >= (int)st.model.size() || i1 >= (int)st.model.size()) continue;
    if (!st.partAlive.empty() && (!st.partAlive[i0] || !st.partAlive[i1]))
      continue;  // limb lost: the chain has gone silent, so must this

    // Rough wrist target from the pre-solve hand rotation; refined by the
    // second pass below (the flatten restarts from the clips every tick, so a
    // single pass never converges).
    Vec3 palmOff = QuatRotate(st.model[ie].rot, sockLocal);
    const Vec3 wristT = palmPrefab - palmOff;

    const Vec3 root = st.model[i0].pos;
    const float L1 = (st.model[i1].pos - root).len();
    const float L2 = ie == i1 ? sk.parts[i1].rest.pos.len()
                              : (st.model[ie].pos - st.model[i1].pos).len();
    const float armReach = (L1 + L2) * 0.98f;
    const Vec3 toT = wristT - root;
    const float shortBy = toT.len() - armReach;
    // Ghost guard, same geometry rule as the legs — but only while FADING
    // OUT: a live hang is allowed to be short (the shrug covers it).
    const bool gripping = in.hangActive || in.climbActive;
    if (!gripping && shortBy > 3.0f) continue;

    // ---- SHOULDER SHRUG: the stretch that makes short arms reach -------
    // Two-bone IK clamps to its annulus, so a chibi rig whose arms cannot span
    // shoulder->lip lands its hands the shortfall below it. The whole chain
    // TRANSLATES toward the target by the (capped, 25 cm) shortfall instead —
    // shoulders pulled up by the body's weight. ZERO on a rig whose arms reach.
    // Only ever UP: a climber pressing down on the lip whose arms have run
    // out of length lifts the hands off it; the shoulders never drop to it.
    if (shortBy > 0.0f && toT.len() > 1e-4f && toT.y > 0.0f) {
      const float kShrugCap = MetresToCells(0.25f);
      const Vec3 shift = toT * (std::min(shortBy, kShrugCap) / toT.len() * weight);
      st.model[i0].pos += shift;
      st.model[i1].pos += shift;
      if (ie != i1) st.model[ie].pos += shift;
    }

    // Pass 1 settles the chain onto the rough target; pass 2 re-measures the
    // palm from the solved hand rotation and corrects.
    AnimSolveTwoBone(sk, st, ch, wristT, weight);
    palmOff = QuatRotate(st.model[ie].rot, sockLocal);
    AnimSolveTwoBone(sk, st, ch, palmPrefab - palmOff, weight);
  }
}

// ---- the ledge climb: a muscle-up, a knee onto the lip, the stand -----------
//
// The body's RISE is authored in Player (player.h ledgeclimb): a pull that
// gets the chest over the hands, a stop, a slow heave of the waist to the lip,
// a lag while one knee swings on, the stand. Everything here is keyed on that
// rise — how far the body actually is from the dead hang — never on a clock,
// so the limbs cannot run ahead of a body the world has stalled, and a
// network ghost poses from nothing but its position and the lip.
//
// Three layers, each where its kind of pose already lives:
//   - the ARMS are ApplyHangArms with the palms kept on the lip: the body
//     rising past hands that stay put is the whole muscle-up, overhead ->
//     pulling -> pressing down at the sides; then they let go and ease back
//     to the clip's arms (see the weight there);
//   - the TORSO leans over the lip (pre-flatten, like the air lean);
//   - the LEGS are two-bone targets in the WALL'S frame (post-flatten, like
//     the gait): hanging, then the lead knee onto the lip, then both feet up
//     onto it.
void Mob::UpdateClimbDrive(const PoseInputs& in, float dt) {
  const bool on = in.climbActive && alive_ && !Ragdolled();
  if (on) {
    // Held through the fade-out: the last pose the climb had is what blends
    // into the gait (a finished climb) or back into the hang (a cancelled one).
    pose_.climbRise = std::clamp(in.climbRise, 0.0f, 1.0f);
    pose_.climbLip = in.hangLip;
    pose_.climbDir = in.hangDir;
  }
  pose_.climbW += ((on ? 1.0f : 0.0f) - pose_.climbW) *
                  HalfLifeK(dt, CurrentTuning().avatar.ikBlendHalflife);
  if (pose_.climbW < 1e-3f) pose_.climbW = 0.0f;
  if (pose_.climbW > 0.999f) pose_.climbW = 1.0f;
}

namespace {
// Forward lean of the torso over the lip, radians, at rise `h`: none in the
// hang, the chest thrown over the hands by the end of the pull, deepest while
// the waist comes up and the knee swings on, upright again standing.
float ClimbLeanAt(float h) {
  constexpr float kDeg = 3.14159265f / 180.0f;
  struct K {
    float h, deg;
  };
  const K keys[] = {{0.0f, 0.0f},
                    {ledgeclimb::kHChestOver, 26.0f},
                    {ledgeclimb::kHWaist, 38.0f},
                    {ledgeclimb::kHKneel, 30.0f},
                    {1.0f, 0.0f}};
  for (size_t i = 0; i + 1 < sizeof(keys) / sizeof(keys[0]); i++)
    if (h <= keys[i + 1].h)
      return (keys[i].deg + (keys[i + 1].deg - keys[i].deg) *
                                SmoothSpan(keys[i].h, keys[i + 1].h, h)) *
             kDeg;
  return 0.0f;
}
}  // namespace

// Mostly up the BACK, some at the pelvis: a climber folds at the waist over
// the lip while the hips hang under it.
void Mob::ApplyClimbLean(const AnimSkeleton& sk, AnimState& st) {
  if (pose_.climbW <= 0.0f || def_ == nullptr) return;
  const float pitch = ClimbLeanAt(pose_.climbRise) * pose_.climbW;
  if (std::fabs(pitch) <= 1e-4f) return;
  constexpr float kRootShare = 0.35f;
  const int root = def_->rootLimb;
  if (root >= 0 && root < (int)sk.parts.size() &&
      (root >= (int)st.partAlive.size() || st.partAlive[root]))
    st.local[root].rot = QuatNormalize(
        Mul(st.local[root].rot, AxisAngle({1, 0, 0}, pitch * kRootShare)));
  int nSpine = 0;
  for (size_t i = 0; i < sk.parts.size(); i++)
    if (sk.parts[i].tag == "spine" && (int)i != root) nSpine++;
  if (nSpine == 0) return;
  const Quat per = AxisAngle({1, 0, 0}, pitch * (1.0f - kRootShare) / nSpine);
  for (size_t i = 0; i < sk.parts.size(); i++) {
    if (sk.parts[i].tag != "spine" || (int)i == root) continue;
    if (i < st.partAlive.size() && !st.partAlive[i]) continue;
    st.local[i].rot = QuatNormalize(Mul(st.local[i].rot, per));
  }
}

void Mob::ApplyClimbLegs(const AnimSkeleton& sk, AnimState& st) {
  if (pose_.climbW <= 0.0f || sk.chains.empty() || def_ == nullptr) return;
  const float h = pose_.climbRise;
  const Quat yaw = AxisAngle({0, 1, 0}, heading_);
  const Vec3 pivot{def_->worldSize.x * 0.5f, 0, def_->worldSize.z * 0.5f};
  const Vec3 bodyOrigin{origin_.x, bodyY_, origin_.z};
  auto toWorld = [&](Vec3 p) { return bodyOrigin + pivot + Rotate(yaw, p - pivot); };
  auto toPrefab = [&](Vec3 w) { return RotateInv(yaw, w - bodyOrigin - pivot) + pivot; };
  const Vec3 up{0, 1, 0};
  const Vec3 dir = pose_.climbDir;
  const Vec3 leftW{dir.z, 0.0f, -dir.x};  // model +X is the rig's left
  const Vec3 centre{origin_.x + pivot.x, 0.0f, origin_.z + pivot.z};
  const float lipTopY = (float)(pose_.climbLip.y + 1);
  // The wall face under the lip, as a distance along `dir` from the body's
  // centre column. Every target below is placed against it, so they are
  // fixed in the WORLD: the body stepping across onto the lip at the end
  // walks its hips over feet that stay where they were put.
  const float face = ((float)pose_.climbLip.x + 0.5f - centre.x) * dir.x +
                     ((float)pose_.climbLip.z + 0.5f - centre.z) * dir.z - 0.5f;
  const float ankleH = MetresToCells(0.08f);

  // Phases, 0..1 each, on the rise.
  const float swing = SmoothSpan(ledgeclimb::kHWaist - 0.05f,
                                 ledgeclimb::kHKneeOn, h);        // knee comes up
  const float leadUp = SmoothSpan(ledgeclimb::kHKneel, 1.0f, h);  // onto the foot
  const float trailLift = SmoothSpan(ledgeclimb::kHKneel, 0.86f, h);
  const float trailOver = SmoothSpan(0.80f, 1.0f, h);

  int legOrdinal = 0;
  for (size_t c = 0; c < sk.chains.size(); c++) {
    if (!IsLegChain(sk, c)) continue;
    const IkChain& ch = sk.chains[c];
    const bool lead = legOrdinal++ == 0;
    if (ch.parts.size() < 2 || ch.effector < 0) continue;
    const int i0 = ch.parts[0], i1 = ch.parts[1], ie = ch.effector;
    if (i0 >= (int)st.model.size() || i1 >= (int)st.model.size() ||
        ie >= (int)st.model.size())
      continue;
    if (!st.partAlive.empty() && (!st.partAlive[i0] || !st.partAlive[i1]))
      continue;  // a lost leg's chain has gone silent; so must this
    const float weight = ch.weight * pose_.climbW;
    if (weight <= 0.0f) continue;
    const float L1 = (st.model[i1].pos - st.model[i0].pos).len();
    const float L2 = (st.model[ie].pos - st.model[i1].pos).len();
    if (L1 < 1e-3f || L2 < 1e-3f) continue;
    const float L = L1 + L2;

    const Vec3 hipW = toWorld(st.model[i0].pos);
    const Vec3 hipRel{hipW.x - centre.x, 0.0f, hipW.z - centre.z};
    const float lat = hipRel.dot(leftW);
    const float hipAlong = hipRel.dot(dir);
    // A point in this leg's column: `along` from the centre, at height y.
    auto at = [&](float along, float y) {
      return Vec3{centre.x + leftW.x * lat + dir.x * along, y,
                  centre.z + leftW.z * lat + dir.z * along};
    };
    // Dangling: straight down, knees soft, a touch back off the wall.
    const Vec3 hangF = hipW - up * (0.93f * L) - dir * (0.10f * L);

    Vec3 target = hangF;
    if (lead) {
      // THE KNEE ON THE LIP: on its top surface, a thigh's length from the
      // hip, at least a hand past the edge. The shin trails back over the
      // edge from it — a knee on a ledge, not a foot.
      const float kneeY = lipTopY + MetresToCells(0.07f);
      const float dy = hipW.y - kneeY;
      const float minAlong = face + MetresToCells(0.12f);
      const float reach = std::fabs(dy) < L1 ? std::sqrt(L1 * L1 - dy * dy) : -1.0f;
      Vec3 kneeW;
      if (reach >= 0.0f && hipAlong + reach >= minAlong) {
        kneeW = at(hipAlong + reach, kneeY);
      } else {
        const Vec3 d = at(minAlong, kneeY) - hipW;
        kneeW = hipW + d * (L1 / std::max(d.len(), 1e-4f));
      }
      const Vec3 shinDir = (dir * -0.9f - up * 0.44f).normalized();
      const Vec3 kneelF = kneeW + shinDir * L2;
      // The swing: the thigh rotates up from hanging to the lip and the shin
      // folds under it, so the foot rides an arc behind the knee instead of
      // being dragged straight through the wall.
      const Vec3 thighHang = (up * -0.97f - dir * 0.24f).normalized();
      const Vec3 thighKneel = (kneeW - hipW) * (1.0f / L1);
      Vec3 thigh = thighHang + (thighKneel - thighHang) * swing;
      thigh = thigh.len() > 1e-4f ? thigh.normalized() : thighKneel;
      Vec3 shin = up * -1.0f + (shinDir + up) * swing;
      shin = shin.len() > 1e-4f ? shin.normalized() : shinDir;
      const Vec3 swungF = hipW + thigh * L1 + shin * L2;
      target = hangF + (swungF - hangF) * swing;
      // ...then up onto that foot: planted on the lip ahead, carried over
      // the edge on a small arc so it does not scrape through the corner.
      const Vec3 standF = at(face + MetresToCells(0.16f), lipTopY + ankleH);
      if (leadUp > 0.0f)
        target = kneelF + (standF - kneelF) * leadUp +
                 up * (std::sin(leadUp * 3.14159265f) * MetresToCells(0.15f));
    } else if (trailLift > 0.0f || trailOver > 0.0f) {
      // The trailing leg comes up last: lifted clear of the edge while it is
      // still behind it, then brought over to stand beside the lead foot.
      const float clearY = lipTopY + ankleH + MetresToCells(0.10f);
      Vec3 f = hangF;
      f.y = hangF.y + (std::max(hangF.y, clearY) - hangF.y) * trailLift;
      const Vec3 standF = at(face + MetresToCells(0.06f), lipTopY + ankleH);
      target = f + (standF - f) * trailOver;
    }
    AnimSolveTwoBone(sk, st, ch, toPrefab(target), weight);
  }
}

// ---- ONE GO-LIMP RULE --------------------------------------------------------
//
// Long enough in the air to go limp (sim/tuning.h Ragdoll): the same rule for
// every creature. It was written twice — PlayerAvatar::PreTick with the
// player's exemptions, MobSystem::UpdateFall with none — and the NPC copy
// handed the limbs `{0, fallVel_, 0}`, dropping the horizontal half of a lunge
// or a blast mid-flight on the tick the body went limp.
bool Mob::ShouldGoLimp(float airTime, const LimpExemptions& ex) const {
  if (!alive_ || Ragdolled()) return false;
  if (airTime < CurrentTuning().ragdoll.fallSeconds) return false;
  // Fly mode is not falling; a hang is held by the hands; a blind fall is the
  // controller HOLDING the drop because the CPU mirror cannot see the ground
  // yet (Player::blindFall) — the body is not moving, however long the clock
  // has run.
  return !(ex.fly || ex.hanging || ex.blindFall);
}

void Mob::GoLimpFromFall() {
  // The limbs take the body's WHOLE velocity with them, from whichever
  // integrator owned it: MoveKinematic gave them one tick's worth of the
  // animated pose, not the drop.
  const Vec3 v = BodyVelocity();
  StartRagdoll(CurrentTuning().ragdoll.minSeconds, "fall");
  SetLimbVelocities(v);
}

// ---- ONE BODY VELOCITY, ROUTED TO WHOEVER OWNS THE BODY ------------------------
//
// "The body's velocity" is three different integrators depending on the
// moment, and every caller used to pick one by hand: a Jolt impulse when the
// rig is limp, the ballistic state UpdateFall integrates for a live NPC, and
// `player.vel` for the avatar (session.cpp wrote `player.vel.y += ...` itself).
// `AddLift` existed only because of that split.
Vec3 Mob::BodyVelocity() const {
  if (Ragdolled() && ragdoll_ == RagdollPhase::Limp && phys_ != nullptr) {
    const int root = def_ != nullptr ? def_->rootLimb : -1;
    Vec3 lin{}, ang{};
    if (root >= 0 && root < (int)limbs_.size() && limbs_[root].body &&
        phys_->GetBodyVelocities(limbs_[root].body, lin, ang))
      return lin;
  }
  return DriverVelocity();
}

Vec3 Mob::DriverVelocity() const {
  // An NPC in the air is its ballistic state; on the ground its measured walk.
  if (airborne_) return Vec3{airVel_.x, fallVel_, airVel_.z};
  return anim_.velocity;
}

void Mob::AddBodyVelocity(Vec3 vps) {
  if (vps.x == 0.0f && vps.y == 0.0f && vps.z == 0.0f) return;
  WakeDead();   // a corpse lifted by `float aura` is a corpse in motion
  // ---- LIMP: the rig belongs to Jolt --------------------------------------
  // One impulse per live limb, sized mass x dv so every limb gains the SAME
  // speed (a uniform rig velocity, which is why SetLimbVelocities exists), and
  // applied AT the centre of mass so a lift lifts instead of spinning. Worn
  // shells and held limbs are excluded: their velocity is somebody else's.
  if (Ragdolled() && phys_) {
    const float dvM = CellsToMetres(vps.len());
    if (dvM <= 0.0f) return;
    for (MobLimb& l : limbs_) {
      if (!l.body || l.holdSeconds > 0 || l.wornHost >= 0) continue;
      const float m = phys_->BodyMass(l.body);
      if (m <= 0.0f) continue;
      Vec3 com;
      if (!phys_->BodyCenterOfMass(l.body, com)) continue;
      phys_->ApplyImpulseAt(l.body, vps, m * dvM, com);
    }
    return;
  }
  if (!alive_) return;
  AddDriverVelocity(vps);
}

void Mob::AddDriverVelocity(Vec3 vps) {
  // ---- LIVE NPC: the ballistic state UpdateFall integrates ----------------
  // A LIFT IS NOT A FALL: an upward push must not land the body on the ground
  // it is rising off this tick (the `launched_` latch) and must not go limp for
  // having been held up a while (`airTime_`). Both are reset only for an
  // UPWARD push, so `heavy aura` still drives a body down onto the floor.
  if (!airborne_) {
    airborne_ = true;
    fallVel_ = 0.0f;
    airVel_ = Vec3{};
    airTime_ = 0.0f;
  }
  fallVel_ += vps.y;
  airVel_.x += vps.x;
  airVel_.z += vps.z;
  if (vps.y > 0.0f) {
    launched_ = true;
    airTime_ = 0.0f;
  }
}
