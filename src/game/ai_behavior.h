#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "game/ai_nav.h"
#include "math3d.h"

// ============================================================================
// NPC BEHAVIOUR — perception, a utility arbiter over named intents, and the
// attack REQUEST seam.
//
// This is the foundation the whole cast is meant to be built on: guards,
// grazers, fleeing critters, archers, pack hunters. So the first question the
// design answers is not "how does a duelist fight" but "what has to be DATA so
// that the next creature is a JSON entry instead of a C++ patch".
//
// ---------------------------------------------------------------------------
// THE SPLIT: VOCABULARY IS CODE, CHARACTER IS DATA
//
// An INTENT is a verb the engine knows how to perform — Approach, HoldRange,
// CircleStrafe. Each is a scorer ("how badly do I want this right now") plus an
// actuator ("write desiredHeading and driveScale"). That is code, and it has to
// be: "circle the target at the outer edge of my band" is geometry, not
// content.
//
// A PROFILE is a named creature character: which verbs it is allowed at all,
// how much it wants each one, how far it can see, what range it likes, how
// often it swings. That is DATA — `assets/mobs/behaviors.json`, hot-reloaded on
// R with materials, glyphs and items. A mob sidecar opts in with one key:
//
//     "behavior": "duelist"
//
// CLAUDE.md design rule 4 ("no closed-ended systems") is the reason there is no
// `enum MobKind` and no `switch (mob.type)` anywhere in this file. A profile
// with `"approach": 0` cannot approach; a profile that never lists `circle`
// never circles. A passive grazer is a profile with `aggro: "passive"` and a
// wander weight; a fleeing critter is that plus one new intent implementation.
// **Adding a creature must be a JSON edit. Adding a VERB is one enum entry, one
// scorer and one actuator, and every existing profile is unaffected.**
//
// ---------------------------------------------------------------------------
// THE ARBITER: UTILITY WITH HYSTERESIS
//
// Every enabled intent is scored 0..1 each tick and multiplied by its authored
// weight; the highest wins. Pure argmax over continuous scores is a machine for
// producing twitching creatures — two intents within a hair of each other swap
// every tick and the mob dances on the spot — so three dampers sit on top, and
// all three are authored:
//
//   * `hysteresis`   — a flat bonus the INCUMBENT intent gets. It must lose by
//                      a real margin, not by a rounding error, to be replaced.
//   * `minDwellTicks`— an intent cannot be dropped before this many ticks
//                      unless its own score has fallen to zero (the "my target
//                      died" escape hatch). This is what makes a step BACK read
//                      as a step back rather than as a stutter.
//   * `cooldownTicks`— after an intent ends it cannot be re-picked for this
//                      long. Circle-strafe uses it so footwork has a rhythm.
//
// ---------------------------------------------------------------------------
// DETERMINISM (CLAUDE.md rule 1)
//
// Everything here is CPU-float gameplay state, exactly like the gait and the
// melee pose it sits beside. The AI never writes a voxel; it writes
// `desiredHeading` and `driveScale`, and the body moves because the SAME
// locomotion pipeline every mob already had moves it.
//
// But it still must be REPRODUCIBLE, because a replay of the same seed and the
// same inputs has to produce the same fight. So:
//
//   * The AI runs in the fixed 30 Hz tick step, inside MobSystem::PreTick, and
//     nowhere else. The frame loop runs that 0..4 times per frame; anything
//     that sampled per frame would multiply-count.
//   * Every random draw is `rng::Hash3(mobId ^ salt, tick, index)` — stateless
//     and counter-based, the CPU mirror of the shader's hash3. No `rand()`, no
//     wall clock, no `std::chrono`, no accumulating engine state.
//   * No decision reads a Jolt float. Positions come from `Mob::origin_`, which
//     the kinematic drive owns.
//
// ---------------------------------------------------------------------------
// THE ATTACK SEAM (what Phase C consumes)
//
// This layer decides WHEN to attack and emits an `AttackRequest`. It does NOT
// swing: no clip is played, no pose is set, no damage is dealt. The stroke
// system owns that. A request carries exactly what a stroke needs and nothing
// about how a stroke works:
//
//     style        — an authored id from the profile ("slash", "thrust", ...)
//     targetPoint  — where the blow is aimed, world voxels
//     targetId     — who it is aimed at (0 = the player)
//     tick         — when it was issued
//     commitTicks  — how long the AI has promised to hold still for it
//
// While a request is live the arbiter locks the mob to FaceTarget for
// `commitTicks`, so a stroke system can assume the body is not pirouetting
// mid-swing. Until Phase C lands, the requests are drained and logged/drawn.
// ============================================================================

namespace ai {

// How a creature treats a stranger. Authored as a string; resolved at load.
enum class Aggro : uint8_t {
  Passive,   // never picks a target at all — grazers, props, training dummies
  Neutral,   // perceives and tracks, but does not close or attack unprovoked
  Hostile,   // closes with and attacks anything of another faction
};

// THE VERB SET. Adding one here is the ONLY C++ a new behaviour costs; every
// profile that does not name it is untouched, because an unlisted intent has
// weight 0 and can never win.
//
// Ordering is stable and is used as a deterministic tie-break, so new entries
// go at the END, before Count.
enum class Intent : uint8_t {
  Idle = 0,      // stand. The floor: always available, always scores just above 0
  FaceTarget,    // turn to the target without moving the feet
  Approach,      // close the distance, via the navigator when the way is not clear
  HoldRange,     // maintain the engagement band: back off when close, close when far
  CircleStrafe,  // sidestep around the target at the current radius
  RequestAttack, // emit an AttackRequest and hold the facing through the commit
  Count,
};

const char* IntentName(Intent i);
Intent IntentFromName(const std::string& s);

// ---- profile schema --------------------------------------------------------
// Every field here is authored in behaviors.json and live-editable from the dev
// panel. Defaults are chosen so that an EMPTY profile is a harmless statue:
// blind, weightless, immobile. A creature only does what its JSON asks for.

struct Perception {
  // World voxels. 0 = blind, which is what makes `dummy` a training target
  // rather than a special case in code.
  float sightRange = 0.0f;
  // Full cone width in DEGREES, centred on the mob's heading. >= 360 is
  // omnidirectional; a guard wants ~150, a paranoid beast 360.
  float fovDegrees = 360.0f;
  // Must the line to the target be clear of solid voxels? A creature that can
  // see through a hill is the classic "how did it know" complaint; a creature
  // that instantly forgets you behind a sapling is the opposite one. `alertDecay`
  // is the answer to both.
  bool requireLos = true;
  Aggro aggro = Aggro::Passive;
  // Ticks a target stays "known" after it is no longer perceived. The mob keeps
  // heading for `lastSeenPos` through this window, which is what turns a broken
  // line of sight into a pursuit instead of an instant shrug.
  uint32_t alertDecayTicks = 90;
  // Once alerted, sight range is multiplied by this before the LOSE test. Plain
  // hysteresis on perception itself: without it a target standing exactly at
  // the range boundary flickers in and out of existence every tick.
  float keepRangeScale = 1.4f;
};

struct Movement {
  // The ENGAGEMENT BAND, world voxels, measured centre-to-centre. Everything
  // about footwork is expressed against these two numbers: inside `min` the mob
  // wants out, past `max` it wants in, between them it is content and free to
  // circle. A pikeman is a wide band far out; a brawler is a narrow one close.
  float rangeMin = 0.0f;
  float rangeMax = 0.0f;
  // Deadband, world voxels. Applied INSIDE both edges so a mob sitting on the
  // boundary is not alternately advancing and retreating — the single most
  // common way a range-keeping AI looks broken.
  float bandSlack = 1.5f;
  // Multipliers on the def's own walk speed, so one profile serves a fast
  // creature and a slow one.
  float approachSpeed = 1.0f;
  float strafeSpeed = 0.55f;
  float retreatSpeed = 0.8f;
  // 0 = never sidesteps, 1 = circles whenever it is content. Reversal is a
  // hash-RNG draw on a cadence, so two duelists do not orbit in lockstep.
  float circleTendency = 0.0f;
  uint32_t circleHoldTicks = 24;   // ticks before the orbit direction may flip
  // Replan cadence. Paths are planned on a clock, never per tick (CLAUDE.md
  // rule 2) — and the path is also dropped early when the terrain under it
  // changes or the target walks off the end of it.
  uint32_t repathTicks = 12;
  // Navigator shape. `navRadius` bounds the search; it is pointless past the
  // CPU mirror's reach and expensive below it.
  float navRadius = 22.0f;
  int maxStepUp = 2;      // must agree with what DriveLocomotion will climb
  int maxStepDown = 5;
  // Can this creature move its feet at ALL? A statue is data, not a code path:
  // `false` forces driveScale to 0 no matter which intent wins, so a profile
  // author cannot accidentally give a training dummy a shuffle.
  bool mobile = false;
};

struct AttackTuning {
  // Authored style id, passed through to the stroke system untouched. THIS
  // LAYER MUST NOT KNOW WHAT STYLES EXIST — that is Phase C's vocabulary, and
  // baking a list here is exactly the closed-ended system rule 4 forbids.
  std::string style = "slash";
  // World voxels, centre-to-centre, inside which a blow can land. Distinct from
  // rangeMax on purpose: a creature that only attacks at the very edge of its
  // footwork band feels timid, one that attacks from anywhere in the band feels
  // reckless, and that difference is a character choice.
  float reach = 8.0f;
  // Facing error (radians) the mob will accept before committing. A swing at a
  // target 90 degrees off the nose is a whiff nobody asked for.
  float aimTolerance = 0.45f;
  // Ticks between attacks, plus a per-attack hash-RNG jitter in [0, jitter).
  // The jitter is not garnish: a metronome is the single most robot-like thing
  // a melee NPC can do, and it makes two of them perfectly synchronised.
  uint32_t cadenceTicks = 40;
  uint32_t jitterTicks = 18;
  // How long the mob holds its facing for the stroke system. Phase C receives
  // this on the request and may use it as its swing window.
  uint32_t commitTicks = 10;
  // Ticks after a commit during which the mob prefers to give ground. Reading
  // as "hit and step off" rather than "stand in the blender" is most of what
  // makes a duel feel like a duel.
  uint32_t disengageTicks = 22;
};

// Per-intent authored knobs. An intent absent from the JSON keeps weight 0 and
// is therefore disabled — that is the enable flag, deliberately not a separate
// boolean nobody would keep in sync with the weight.
struct IntentTuning {
  float weight = 0.0f;
  uint32_t cooldownTicks = 0;
  uint32_t minDwellTicks = 0;
};

struct Profile {
  std::string name;          // the id a sidecar and the panel refer to
  std::string label;         // human text for the debug readout
  uint32_t color = 0xC0FFFFFFu;   // 0xAABBGGRR, matching DebugBox
  // Who this creature considers "us". Targets are actors of a DIFFERENT
  // faction. A string resolved to an id at load, so adding "bandit" is content.
  std::string faction = "monster";
  Perception perception;
  Movement movement;
  AttackTuning attack;
  IntentTuning intents[(int)Intent::Count];
  // Flat score bonus the current intent keeps. See the arbiter note above.
  float hysteresis = 0.22f;
};

struct Library {
  std::vector<Profile> profiles;
  int Find(const std::string& n) const {
    for (size_t i = 0; i < profiles.size(); i++)
      if (profiles[i].name == n) return (int)i;
    return -1;
  }
  const Profile* At(int i) const {
    return (i >= 0 && i < (int)profiles.size()) ? &profiles[i] : nullptr;
  }
  Profile* At(int i) {
    return (i >= 0 && i < (int)profiles.size()) ? &profiles[i] : nullptr;
  }
};

// Load assets/mobs/behaviors.json. Follows every other loader in this engine: a
// bad entry is skipped LOUDLY into `log` and never fatal, and an unknown key is
// ignored so a newer authored file still loads on an older binary.
bool LoadBehaviors(const std::string& path, Library& out, std::string& log);
// Write the library back, whole. Machine-owned formatting (the emitter shape
// `WorldgenDefaultsJson` uses) rather than the text-surgery patcher, because
// the dev panel can ADD and REMOVE profiles, which surgery cannot express.
bool SaveBehaviors(const std::string& path, const Library& lib, std::string& err);

// ---- the world, as the AI sees it -----------------------------------------

// One thing that can be perceived or targeted. The player and every live mob
// are both actors, which is the entire reason mob-vs-mob combat is a data
// change later rather than a second code path: target selection scans this list
// and never asks what KIND of thing an entry is.
struct Actor {
  uint64_t id = 0;          // mob id; 0 is reserved for the player
  Vec3 centre{};            // world voxels, body centre
  float radius = 1.0f;      // horizontal half-extent, for stand-off distance
  float height = 2.0f;
  uint32_t faction = 0;
  bool alive = true;
};

// Everything Think() may read about the outside world.
struct WorldView {
  const std::vector<Actor>* actors = nullptr;
  NavProbe probe;            // ground/headroom queries, bound by the caller
  // Line of sight, bound by the caller so the AI shares ONE implementation of
  // "is there rock between these two points" with everything else that asks.
  bool (*lineOfSight)(void* ctx, Vec3 from, Vec3 to) = nullptr;
  void* losCtx = nullptr;
};

// The mob, as the AI sees itself. A view rather than a Mob& so this file has no
// dependency on the rig, the physics or the animation — which is what lets the
// gates drive it and what will let a future non-Mob agent use it.
struct SelfView {
  uint64_t id = 0;
  Vec3 origin{};        // prefab MIN CORNER, world voxels (Mob::origin_'s frame)
  Vec3 size{};          // worldSize
  float heading = 0;
  float speed = 4.0f;   // def.speed, world voxels/sec
  // The yaw rate this body can actually sustain WHILE MOVING (rad/s), from its
  // rig's LocomotionDef. The behaviour layer needs it for one thing and it is
  // not cosmetic: an orbit of radius r walked at v induces an angular rate v/r,
  // and a creature whose neck cannot follow that spends the whole circle
  // looking behind itself — it never gets its nose on the target, so it never
  // attacks, and its drive is scaled down by an alignment it can never reach.
  // Measured: mina circling at the authored 0.5 x 60 vox/s needed 3.3 rad/s
  // against a 2.8 rad/s cap and issued ZERO attacks in 240 ticks of holding.
  float turnRate = 3.6f;
  uint32_t faction = 0;
  Vec3 Centre() const {
    return Vec3{origin.x + size.x * 0.5f, origin.y + size.y * 0.5f,
                origin.z + size.z * 0.5f};
  }
  Vec3 Foot() const {
    return Vec3{origin.x + size.x * 0.5f, origin.y, origin.z + size.z * 0.5f};
  }
};

// The 8-way terrain fan the locomotion layer already probes, copied in so this
// file does not depend on MobSystem's private nested type. Probe 0 is dead
// ahead, in the mob's own frame.
struct GroundView {
  static constexpr int kProbes = 8;
  bool haveGround = false;
  int groundY = 0;
  bool clear[kProbes] = {};
  int stepUp[kProbes] = {};
};

// ---- the attack seam -------------------------------------------------------

// One NPC attack request. See "THE ATTACK SEAM" above for the contract.
struct AttackRequest {
  uint64_t mobId = 0;
  uint64_t targetId = 0;
  std::string style;
  Vec3 targetPoint{};       // world voxels, where the blow is aimed
  uint32_t tick = 0;
  uint32_t commitTicks = 0;
  float distance = 0;       // centre-to-centre at the moment of the decision
};

// ---- per-creature runtime state -------------------------------------------

// Everything the arbiter remembers between ticks. Lives on the Mob (one per
// creature) and is pure presentation/gameplay state — never saved, never
// hashed, rebuilt from nothing on spawn.
struct Brain {
  int profile = -1;              // index into Library, -1 = no AI (legacy wander)

  // ---- perception ----
  uint64_t targetId = 0;
  bool hasTarget = false;
  bool visible = false;          // perceived THIS tick (vs. remembered)
  Vec3 targetPos{};              // live position when visible, else lastSeenPos
  Vec3 lastSeenPos{};
  uint32_t lastSeenTick = 0;
  float targetDist = 0;
  float bearingError = 0;        // radians, target bearing minus heading

  // ---- arbiter ----
  Intent intent = Intent::Idle;
  uint32_t intentSince = 0;
  uint32_t cooldownUntil[(int)Intent::Count] = {};
  float score[(int)Intent::Count] = {};   // last tick's scores, for the panel

  // ---- attack clock ----
  uint32_t nextAttackTick = 0;
  uint32_t commitUntil = 0;      // facing is locked while tick < this
  uint32_t disengageUntil = 0;
  uint32_t lastAttackTick = 0;   // sticky; the dev panel reads it
  uint32_t attacksIssued = 0;

  // ---- footwork ----
  int circleSign = 0;            // -1 / +1, redrawn on a cadence
  uint32_t circleUntil = 0;

  // ---- navigation ----
  NavPath path;
  uint32_t nextRepathTick = 0;
  Vec3 pathTarget{};             // the goal the live path was planned for
  bool navFailed = false;        // last plan failed; steering direct
  // How many A* searches this creature has actually run. A DIAGNOSTIC, and a
  // load-bearing one: a replan count that climbs with the cadence rather than
  // with the number of obstacles means the planner is re-deciding a symmetric
  // route every few ticks, which reads as a mob dithering in place.
  uint32_t replans = 0;
  // Progress along the CURRENT waypoint, and how long there has been none. A
  // path is retired by being walked or by stalling, never by the agent drifting
  // off the straight line it was planned along — see UpdatePath.
  float lastWaypointDist = 1e9f;
  uint32_t stuckTicks = 0;

  void Reset() {
    *this = Brain{profile};
  }
  Brain() = default;
  explicit Brain(int p) : profile(p) {}
};

// ---- the tick --------------------------------------------------------------

// What Think() is allowed to produce. Deliberately the same two fields the
// existing AI seam may write (`DESIGN.md`, "Mob steering: intent vs
// actuation") plus the attack request — a behaviour STRUCTURALLY cannot
// teleport a facing or move a body.
struct IntentOut {
  float desiredHeading = 0;
  // Forward drive, SIGNED, as a multiplier on the def's walk speed. Negative is
  // a back-pedal — a duelist that turns its back to give ground reads as a rout
  // rather than as footwork, and circling is impossible without the lateral
  // term below, so the drive stage takes a 2D local velocity rather than a
  // scalar. `Steer` is still the only writer of heading; this only changes
  // WHICH DIRECTION the body translates relative to that heading.
  float driveScale = 0;
  float driveStrafe = 0;   // lateral, + = the mob's own right
  bool attack = false;
  AttackRequest request;
};

// One tick of AI for one creature. Returns false when the mob has no profile,
// which the caller reads as "fall through to the legacy wander-and-avoid" —
// so an un-authored mob behaves exactly as it did before this system existed.
//
// MUST be called from the fixed tick step, once per tick, with a monotonically
// increasing `tick`.
bool Think(Brain& brain, const Library& lib, const SelfView& self,
           const GroundView& ground, const WorldView& view, uint32_t tick,
           float dt, IntentOut& out);

// Resolve a faction name to an id, interning as it goes. Names are content;
// ids are runtime. Shared by profile load and by whoever labels the player.
uint32_t FactionId(const std::string& name);
const char* FactionName(uint32_t id);

}  // namespace ai
