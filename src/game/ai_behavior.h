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
//     targetId     — who it is aimed at (0 = nobody; a player is in the
//                    kPlayerActorBase band, an NPC is its mob id)
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
  Flee,          // run from the threat (the target, else the nearest enemy seen)
  // ---- DEFENCE (2026-09-27) -----------------------------------------------
  // Both score only while the TARGET IS SWINGING AT US (Actor::action) and
  // only after this creature has REACTED to it (Defense: a per-blow roll
  // against `skill`, then `reactTicks` of delay). That is what keeps them fair:
  // a fast blow beats a slow reader, and an unskilled creature mostly eats it.
  Guard,         // raise the held weapon onto the incoming blade (the parry is
                 // the existing blade-on-blade test; this only puts one there)
  Dodge,         // step back and aside, out of the reach of the blow
  // ---- A VILLAGER'S DAY (2026-09-29, PLAN_world_editor P7) ----------------
  // One verb per schedule activity. They score ONLY while the resident layer
  // (world/refs_npc.h) has written a Routine onto the brain, and only the one
  // the current schedule row names; the profile's weights decide how easily a
  // fight or a fright takes over. All six share one actuator (walk to
  // Routine::goal over the local navigator, then hold the pose the routine
  // asks for) -- what differs between them is WHERE the resident layer sends
  // the body and what it does on arrival, and that is not the arbiter's
  // business.
  Sleep,         // lie on the bed anchor
  Work,          // stand at the work anchor facing its yaw
  Wander,        // stroll between points around an anchor
  Socialize,     // stand at a gathering place facing someone
  Eat,           // stand at home facing the hearth
  Goto,          // go there and stand
  // ---- MAGIC (2026-10-04, docs/PLAN_demons.md D4) --------------------------
  // Emit a CastRequest (CastTuning) and hold the facing through its commit,
  // the RequestAttack shape. WHICH spell, and whether it can fly at all, is the
  // owner's business (game/demon_cast.h, the creature spell kit): this layer
  // only decides "near enough, facing, and the cadence says now".
  Cast,
  Count,
};
// The activity verb for a schedule row's `do` ("sleep" -> Intent::Sleep), or
// Intent::Count for anything else.
Intent IntentForActivity(const std::string& act);

// What a combatant is doing with its weapon right now, as others see it.
// Published per actor (Actor::action) from the NPC stroke phase or the player's
// strike cursor; the defence verbs and the `target*` facts read it.
enum class Action : uint8_t {
  None = 0,   // weapon at rest / walking
  Guard,      // holding a guard (a blade raised, not swinging)
  Windup,     // a blow is coming: the telegraph
  Cut,        // the blow is live
  Recover,    // just swung (or was parried): the punish window
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
  // ---- WHO TO FIGHT, not just who is nearest (2026-09-27) ------------------
  // Target choice scores every perceived enemy by distance MINUS these, in
  // world voxels. Both default 0, which is plain "nearest" — the behaviour
  // every profile had before.
  //   stickiness — the current target counts as this much closer. Stops a
  //                creature swapping between two enemies a voxel apart,
  //                which reads as it having no idea who it is fighting.
  //   preferWeak — an enemy at 0 hp counts as this much closer (linear in
  //                missing life). A predator finishes the wounded one.
  float stickiness = 0.0f;
  float preferWeak = 0.0f;
  // ---- NEUTRAL: "fights back if struck" (2026-09-29, P7) -------------------
  // An `aggro: "neutral"` creature perceives nobody as a target UNTIL IT IS
  // HURT; for this many ticks after life last fell it acquires like a hostile
  // one (preferring whoever has named IT as their target -- the one hitting
  // it), and a target it holds is kept through the alert decay as usual.
  // There is no attacker attribution on a wound, so "the nearest stranger
  // when it hurt" is the honest approximation; see DESIGN.md §16.P7.
  uint32_t provokeTicks = 240;
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
  // Flee's drive, same units. Flee turns its back (it is a rout, not footwork:
  // HoldRange is the verb that gives ground facing the enemy).
  float fleeSpeed = 1.0f;
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
  // 0 = ASK THE BODY (SelfView::stepUpCells). These were a hard 2 and 5 in
  // behaviors.json, authored back when the drive used a fixed constant of its
  // own — two independent copies of "how big a ledge is a wall", in a JSON file
  // and in a C++ header, with nothing keeping them equal. Raising the drive's
  // budget without noticing this would have made the planner the new bottleneck
  // and looked exactly like the bug it was supposed to fix. A profile that
  // wants a timid climber still says so; a profile that says nothing inherits
  // whatever its rig can physically do.
  int maxStepUp = 0;
  int maxStepDown = 0;
  int headroom = 0;
  // Can this creature move its feet at ALL? A statue is data, not a code path:
  // `false` forces driveScale to 0 no matter which intent wins, so a profile
  // author cannot accidentally give a training dummy a shuffle.
  bool mobile = false;
  // ---- IDLE WANDER (2026-10-01; `movement.wander` in behaviors.json) -------
  // A creature with nothing to fight strolls round where it was born: legs of
  // a walk to a point drawn inside `wanderRadius` of its anchor (counter-based
  // on its id and the leg number), each followed by a pause of
  // `wanderPauseMin..Max` ticks standing still. Scored as the `wander` intent
  // (its weight is the profile's `intents.wander`), so a target, a fright or a
  // resident's schedule outbids it with no special case. 0 = off: the day
  // verbs then come only from a resident routine (world/refs_npc.cpp), which
  // always wins over this one.
  float wanderRadius = 0.0f;      // world voxels from the anchor
  float wanderSpeed = 0.5f;       // multiplier on the def's walk speed
  uint32_t wanderPauseMin = 60;   // ticks
  uint32_t wanderPauseMax = 180;  // ticks
  float wanderArrive = 2.0f;      // world voxels, planar: "there"
  // ---- TACTICAL FOOTWORK (2026-09-27) --------------------------------------
  // Dodge's drive, as a multiplier on walk speed (split between a back-step
  // and a side-step). Above 1 is a burst — a dodge at walking pace is not one.
  float dodgeSpeed = 1.3f;
  // 0..1. While circling, how strongly to orbit AWAY from the nearest ally
  // fighting the same target, so a pack spreads around its quarry instead of
  // queueing in a line. 0 = the orbit direction is a coin flip, as before.
  float flank = 0.0f;
  // KITING. < 0 = off. When this creature out-reaches its target, the band's
  // floor is lifted to (target's reach + half a body + keepOut): it stands
  // where it can hit and the target cannot. Ignored when it does NOT out-reach
  // (a floor above the ceiling is not a band). Rules may set it (`tune`).
  float keepOut = -1.0f;
};

// ---- HOW A CREATURE DEFENDS ITSELF (2026-09-27) ---------------------------
//
// The Guard and Dodge verbs above only become available once the creature has
// REACTED to a blow, and this block is the reaction. Per incoming blow (keyed
// on the tick the target's windup began) one counter-based roll decides
// whether it reacts at all (`skill`), and when (`reactTicks` + a jitter). The
// guard/dodge WEIGHTS then decide which answer it picks; a creature that
// cannot guard (nothing in its hand) can only dodge.
//
// Defaults are a creature that never reacts, so an old profile is unchanged.
struct Defense {
  float skill = 0.0f;           // 0..1: chance of reacting to a given blow
  uint32_t reactTicks = 8;      // delay from the windup's first tick
  uint32_t reactJitter = 6;     // + [0, jitter) per blow
  // World voxels past the attacker's reach that still count as threatened.
  // The stand-off test is centre-to-centre against a reach that is itself an
  // estimate, so a zero margin leaves a creature ignoring a blow that lands.
  float margin = 2.5f;
  // Raw score Guard holds with NO blow incoming, while standing inside an
  // ARMED target's reach and not ready to attack: the "keep your guard up"
  // stance. 0 = only ever guard a real blow. A rule can raise the WEIGHT to
  // make a hurt creature turtle.
  float guardStance = 0.0f;
  // How far out along the arm the guard holds the blade, 0..1.
  float guardReach = 0.85f;
  // Ticks a defence is held after the blow ends, so a guard does not drop in
  // the very tick the edge passes and eat the recoil.
  uint32_t holdTicks = 4;
};

// ---- HOW A CREATURE CASTS (2026-10-04, docs/PLAN_demons.md D4) -------------
//
// The `cast` block of a profile. Like AttackTuning::styles, the SPELL NAMES are
// passed through untouched: they name entries of the creature spell kit
// (assets/demons/spells/, game/demon_cast.h), and this layer must not know
// what is in it. A repeated name weighs the draw. Defaults are a creature
// that never casts (no spells, a zero range), so every older profile is
// unchanged.
struct CastTuning {
  std::vector<std::string> spells;
  // Centre-to-centre, world voxels: "near enough to try". Each kit spell then
  // has its own range, checked by the owner when it draws one.
  float rangeMin = 0.0f;
  float rangeMax = 0.0f;
  // Ticks between casts + a hash-RNG jitter in [0, jitter): SPARING by default.
  uint32_t cadenceTicks = 150;
  uint32_t jitterTicks = 60;
  // Ticks after FIRST seeing a target before the first cast (a creature that
  // opens with a fireball the tick it spots you is unfair, not cunning).
  uint32_t firstDelayTicks = 45;
  // Facing held for the cast, like AttackTuning::commitTicks.
  uint32_t commitTicks = 8;
  float aimTolerance = 0.6f;
  // The creature's own mana pool (game/spell.h CasterState), read by the owner
  // the first time this creature casts.
  int32_t mana = 100;
  int32_t regenPerMille = 220;   // per-mille of a mana point per tick
  // Flight carriers this creature may have in the air at once (rule 2: 64
  // casters stay bounded however eager their cadence).
  int32_t maxLive = 2;
};

struct AttackTuning {
  // AUTHORED STYLE IDS, passed through to the stroke system untouched. THIS
  // LAYER MUST NOT KNOW WHAT STYLES EXIST — that is the stroke system's
  // vocabulary (game/strokes.h), and baking a list here is exactly the
  // closed-ended system rule 4 forbids.
  //
  // A LIST, not one id, and that is a character decision rather than a
  // convenience: a creature that answers every opening with the same cut is a
  // creature the player solves once. One is drawn per attack by
  // `PickAttackStyle`, counter-based on (mobId, tick), so ten swings vary and
  // the sequence still replays. `"style": "x"` and `"styles": ["x"]` are the
  // same profile — the loader folds the singular into the list — so a profile
  // written before styles were plural is unchanged.
  std::vector<std::string> styles;
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
  //
  // IT IS SUSPENDED AGAINST A TARGET THAT IS ALREADY LEAVING — see the
  // `holdGround` note in Think. Stepping off from someone who is running away
  // is not footwork, it is a second retreat, and two of them is a creature
  // that never fights again.
  uint32_t disengageTicks = 22;

  // ---- STRIKE WHILE CLOSING (2026-09-16) ----------------------------------
  //
  // Fraction of the creature's own walk speed it may spend keeping the target
  // in range WHILE a swing is committed. 0 restores the original behaviour,
  // which was to plant the feet from the instant of the decision until the
  // commit window expired.
  //
  // Owner report: "enemy mobs need to be able to hit you if you're
  // crouchwalking away from them". Planting the feet is the whole of why they
  // could not. The sequence was: decide to attack (RequestAttack writes a
  // heading and NO drive), then `commitUntil` pins the mob to FaceTarget
  // (which also writes no drive) for `commitTicks`, then `disengageUntil`
  // lifts the band floor to its ceiling for `disengageTicks` more. A `duelist`
  // therefore stood still for 10 ticks and then actively gave ground for 26,
  // 36 ticks out of a 34-tick cadence — against a target walking away at even
  // half its speed, every one of those ticks is gap it has to win back before
  // it is allowed to try again. It never was.
  //
  // A SWING IS A STEP FORWARD. This is the number that says how much of one.
  float pursueSpeed = 0.6f;

  // A target opening the range faster than this FRACTION OF OUR OWN WALK SPEED
  // cancels the `disengageTicks` window — see the `holdGround` note in Think.
  //
  // IT IS A NOISE FLOOR, NOT A JUDGEMENT. The first version of this was 0.30,
  // reasoned as "a third of walk speed is where a retreat stops being a
  // shuffle", and it was wrong for the only case it was written for: the
  // reported scenario is a CROUCHWALKING player, which is
  // `player.walkSpeed` 1.6 m/s x `crouchSpeedScale` 0.5 = 8 world voxels a
  // second, against a `human` mob that walks at 31.5 — a ratio of 0.25, under
  // the threshold, so the suspension never fired once in a 360-tick fight that
  // was entirely about it (`ai-pursue` reported `held 0`). Sizing a floor by
  // reasoning about what "counts" as running away is how that happens.
  //
  // The honest rule is that ANY target opening the range is one you do not step
  // away from, and the only thing a threshold is needed for is to stop a gait's
  // bob and yaw — a fraction of a voxel per tick, surviving the low pass as a
  // voxel or two a second — from reading as flight. Hence a floor just above
  // that, and no opinion about pace.
  //
  // A LARGE VALUE RESTORES THE ORIGINAL BEHAVIOUR (step off from anyone,
  // always), which is what `duelist_flatfooted` uses to give the `ai-pursue`
  // gate a repro arm that costs no rebuild and no second binary.
  float holdGroundFrac = 0.08f;

  // How far AHEAD of the target the blow is aimed, in ticks. **-1 = use
  // `commitTicks`** (the default); 0 = no lead at all, which is what this
  // engine did before 2026-09-16 and what the gate's repro arm asks for.
  //
  // THE AIM POINT IS FROZEN AT THE REQUEST AND NEVER REFRESHED — `StartStroke`
  // copies `AttackRequest::targetPoint` onto the stroke and `StepStroke`
  // re-derives the bearing from that same stored point every tick (mob.cpp,
  // "THE AIM: the target's bearing about THIS mob's shoulder"). So a stroke
  // aimed at where the target stood when the decision was taken is still aimed
  // there fifteen ticks later, when the edge actually arrives. That is correct
  // for the TELEGRAPH — a committed cut must not home, and `strokes.h` says so
  // — but it means the point the AI hands over has to be where the target WILL
  // BE, not where it is. Aiming at the present against a body in motion is a
  // guaranteed miss dressed up as a design principle.
  //
  // The right value is the delay from the decision to the middle of the cut:
  // `windup.ticks + cut.ticks/2` of the styles this profile draws
  // (assets/mobs/attack_styles.json — the NPC sword cuts are 12..14 + 5..7, so
  // about 15; the punches and the bite are about 12). Tempo jitter moves it, so
  // it is an estimate and is authored as one.
  int32_t leadTicks = -1;

  // ---- MIND GAMES (2026-09-27) ---------------------------------------------
  //
  // FEINT: chance (0..1) that a committed swing is a bluff. The stroke starts
  // exactly as a real one does — same windup, same telegraph — and is pulled
  // out `feintTicks` into the commit, before the cut. The real blow then comes
  // `feintFollowTicks` later, while the target is still answering the fake. A
  // creature that never feints is one the player learns to read in a minute.
  float feintChance = 0.0f;
  uint32_t feintTicks = 6;
  uint32_t feintFollowTicks = 10;
  // RIPOSTE: after a guard or dodge that saw a blow through, the next attack
  // may come this many ticks later, whatever the cadence says. -1 = off (the
  // cadence rules as before). The punish for a swing that was answered.
  int32_t riposteTicks = -1;
};

// ---- TUNE: what a holding RULE may do to the fighter, besides weights -------
//
// Intent weights say WHAT a creature does; these say HOW. A rule's `tune` map
// names any of them, so "wounded: fight from further out, swing less, guard
// more" is one rule and not a second profile. Scales multiply across every
// holding rule, adds sum, `keepOut` is last-wins. Names are the lower-case
// keys used in JSON (TuneName).
enum class Tune : uint8_t {
  Cadence = 0,  // scale on the time between attacks (< 1 = swings more often)
  Speed,        // scale on approach / strafe / retreat / pursuit / dodge
  Band,         // ADD (voxels) to both edges of the footwork band, after the
                // weapon has placed it: + stands further out, - crowds in
  KeepOut,      // SET Movement::keepOut (kite outside their reach)
  Disengage,    // scale on the post-swing step-off
  Circle,       // scale on circleTendency
  Skill,        // scale on Defense::skill
  React,        // scale on Defense::reactTicks (> 1 = slower to answer)
  Feint,        // scale on AttackTuning::feintChance
  Aim,          // scale on aimTolerance (> 1 = swings from a sloppier angle)
  Count,
};
const char* TuneName(Tune t);
Tune TuneFromName(const std::string& s);
enum class TuneMode : uint8_t { Scale, Add, Set };
TuneMode TuneModeOf(Tune t);

// Per-intent authored knobs. An intent absent from the JSON keeps weight 0 and
// is therefore disabled — that is the enable flag, deliberately not a separate
// boolean nobody would keep in sync with the weight.
struct IntentTuning {
  float weight = 0.0f;
  uint32_t cooldownTicks = 0;
  uint32_t minDwellTicks = 0;
};

// ---- RULES: CHARACTER THAT DEPENDS ON THE BODY'S STATE -------------------
//
// A profile's weights say what a creature wants when nothing is wrong with it.
// A RULE says how that changes when something is: "on fire -> run", "down to a
// third of its blood -> stop attacking", "alone -> hang back". Authored in the
// profile as
//
//     "rules": [
//       { "note": "burning: run",
//         "when":  { "burning": ">0" },
//         "weight": { "flee": 3.0 },
//         "scale":  { "attack": 0 } }
//     ]
//
// `when` is a conjunction of comparisons against named FACTS (below); an empty
// `when` always holds. While a rule holds, `weight` REPLACES an intent's
// authored weight (so a rule can switch on a verb the profile leaves at 0) and
// `scale` then multiplies it. Rules apply in order, so a later rule's `weight`
// wins over an earlier one's and every holding rule's `scale` compounds.
//
// The facts are the vocabulary, and they are code for the same reason intents
// are: "how much of my body is alight" is a question about the rig. Adding one
// is one enum entry, one name and one line in Think's fact table; a rule that
// names an unknown fact is reported at load and dropped whole, never half-
// applied.
enum class Fact : uint8_t {
  Hp = 0,       // life left, 0..1 of the authored total (blood is health)
  Burning,      // fraction of the body's limbs with fire on them, 0..1
  LimbsLost,    // authored limbs no longer attached (severed or never spawned)
  SinceHurt,    // ticks since life last fell (a blow, a burn, a bleed); 1e9 = never
  HasTarget,    // 0/1
  Visible,      // 0/1: the target is perceived this tick, not remembered
  TargetDist,   // centre-to-centre, world voxels; 1e9 with no target
  Allies,       // live actors of OUR faction within sightRange (self excluded)
  Enemies,      // live actors of another faction within sightRange
  // ---- WHAT I AM HOLDING, AND WHAT I AM UP AGAINST (2026-09-27) -----------
  // All of the `target*` facts read the TARGET's published Actor, and are 0
  // with no target (reach/hp 0 too, so "targetHp < 0.3" needs hasTarget).
  Armed,            // 0/1: something in MY hand (not fists or teeth)
  MyReach,          // what my usable styles land at, world voxels
  TargetReach,      // what the target's weapon lands at
  ReachAdv,         // MyReach - TargetReach: + = I out-reach it
  TargetArmed,      // 0/1
  TargetHp,         // 0..1
  HpAdv,            // Hp - TargetHp: + = I am the healthier one
  TargetAttacking,  // 0/1: it is winding up or cutting RIGHT NOW
  TargetRecovering, // 0/1: it just swung — the punish window
  TargetGuarding,   // 0/1: it is holding a guard
  InTheirReach,     // 0/1: I stand inside what its weapon lands at
  TargetFacingMe,   // -1..1: cos of its facing off the line to me (1 = square)
  EngagedAllies,    // allies of mine fighting the same target
  PressRank,        // how many of those are NEARER it than me (0 = I am the
                    // front of the queue). The press-limit rule reads this.
  // ---- (2026-09-29, P7) ----------------------------------------------------
  Hostiles,         // live actors of another faction within sightRange whose
                    // own profile is `aggro: "hostile"` (Actor::hostile): a
                    // villager runs from the zombie, not from the player
  // ---- (2026-10-03, PLAN_electricity E4) ------------------------------------
  Shocked,          // ticks of shock stun left (0 = not stunned). While > 0
                    // the body's intent is overridden (no move, no attack);
                    // a rule may still read it (e.g. flee the water after)
  Count,
};
const char* FactName(Fact f);
Fact FactFromName(const std::string& s);

enum class CmpOp : uint8_t { Lt, Le, Gt, Ge, Eq, Ne };

struct Condition {
  Fact fact = Fact::Hp;
  CmpOp op = CmpOp::Lt;
  float value = 0.0f;
};

struct Rule {
  std::string note;                 // the author's words, kept by SaveBehaviors
  std::vector<Condition> when;      // ALL must hold; empty = always
  float setWeight[(int)Intent::Count];   // < 0 = leave the weight alone
  float scale[(int)Intent::Count];       // 1 = leave it alone
  // The `tune` map (see Tune). `tuneSet[k]` false = the rule does not name it.
  float tune[(int)Tune::Count];
  bool tuneSet[(int)Tune::Count];
  Rule() {
    for (int i = 0; i < (int)Intent::Count; i++) {
      setWeight[i] = -1.0f;
      scale[i] = 1.0f;
    }
    for (int k = 0; k < (int)Tune::Count; k++) {
      tune[k] = 0.0f;
      tuneSet[k] = false;
    }
  }
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
  Defense defense;
  CastTuning cast;   // D4: magic (empty = never casts)
  IntentTuning intents[(int)Intent::Count];
  // Flat score bonus the current intent keeps. See the arbiter note above.
  float hysteresis = 0.22f;
  // State-dependent weight changes, applied in order. See "RULES" above.
  std::vector<Rule> rules;
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
// THE PLAYERS' RESERVED ACTOR ID BAND. Player i is `kPlayerActorBase + i`.
//
// WHY A HIGH BAND AND NOT THE LOW INDICES (M9.1 P2, 2026-09-20). Until now the
// player's actor id was literally 0 and the list index WAS the id, which broke
// twice over the moment a second session existed:
//
//   * mob ids start at 1 (MobSystem::nextId_) and are restored from saves by
//     SetNextIdCounter, so player 1 collided with the first mob ever spawned in
//     the world. Reserving a LOW band would have meant moving mob id
//     allocation, and mob ids are in the save format.
//   * 0 was doing two jobs. `Brain::targetId = 0` is how "I have no target" is
//     written (ai_behavior.cpp Perceive) and `ForceAttack`'s `targetId`
//     defaults to 0 meaning "no victim in mind" -- yet `FindCombatantById(0)`
//     resolved to avatars_[0], the player. A scripted swing that named nobody
//     drew the player's limb. See the memory note "targetId 0 resolves to
//     nobody".
//
// 1<<62 is above every id either space can reach (mob ids are a monotonic
// counter; a world would have to spawn 4.6e18 creatures) and below the sign
// bit, so an accidental signed compare still orders sanely. The band is NOT
// saved: player actor ids are rebuilt every tick by SetPlayerActors and the AI
// brain is not in SaveState, so changing it costs no migration.
//
// A band member still means "a creature that is not in `mobs_`": the avatar is
// a Mob owned by a PlayerSession, so `FindMobById` comes back empty for it.
// Anything resolving a target id to a CREATURE must say so explicitly;
// `MobSystem::FindCombatantById` is that function, and this band is why it has
// to exist.
constexpr uint64_t kPlayerActorBase = 1ull << 62;
// Player 0 -- the symbol every one-player comparison keeps using, so nothing
// that means "the local player" has to know the band arithmetic.
constexpr uint64_t kPlayerActorId = kPlayerActorBase;
// Is this actor id a player's? The ONE test; do not open-code the band.
constexpr bool IsPlayerActorId(uint64_t id) { return id >= kPlayerActorBase; }

struct Actor {
  uint64_t id = 0;          // mob id, or kPlayerActorBase + player index
  Vec3 centre{};            // world voxels, body centre
  float radius = 1.0f;      // horizontal half-extent, for stand-off distance
  float height = 2.0f;
  uint32_t faction = 0;
  bool alive = true;
  // ---- WHAT IT IS FIGHTING WITH, AS OTHERS CAN SEE IT (2026-09-27) --------
  // Everything a watching creature could read off the body: what is in its
  // hand and how far that lands, how hurt it is, which way it faces, what its
  // weapon is doing and where the point is. Filled by MobSystem::PreTick from
  // the rig (and, for a player, the strike cursor the session publishes).
  // Defaults are "unknown and harmless", which is what a gate that builds its
  // own actor list gets.
  float reach = 0.0f;       // world voxels, centre-to-centre; 0 = unknown
  bool armed = false;       // a held weapon, not fists or teeth
  float hpFrac = 1.0f;
  float heading = 0.0f;     // 0 = +Z, like everything else
  Action action = Action::None;
  bool haveTip = false;     // weapon point known this tick
  Vec3 tip{};               // world voxels
  uint64_t targetId = 0;    // who IT is fighting (NPCs; 0 = nobody/unknown)
  // Its profile is `aggro: "hostile"` -- it attacks strangers unprovoked.
  // What Fact::Hostiles counts and what a fleeing villager runs from.
  bool hostile = false;
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
  // ---- THE BODY'S OWN TERRAIN BUDGETS, in world voxels -------------------
  // The planner and the walk drive must refuse the SAME wall. When they do
  // not, the failure is the one ai_nav.h's `climbPenalty` note describes: A*
  // routes over a rise the drive then declines to walk, and the creature
  // stands at the foot of it forever insisting the way is clear. These are
  // resolved once per rig from its authored metres (anim.h LocomotionDef) and
  // handed across so there is exactly one number, not two that agree today.
  //
  // A profile MAY override them (Movement::maxStepUp and friends, > 0), which
  // is how a cautious creature is authored — but the default is silence, and
  // silence means "ask the body".
  int stepUpCells = 2;
  int stepDownCells = 5;
  int headroomCells = 3;
  // ---- HOW FAR THIS BODY CAN REACH, THIS TICK (plan §5) ------------------
  //
  // `max(profile.reach, the longest reach among the styles it can still USE)`,
  // computed by MobSystem::AttackReachOf and handed in. 0 = "ask the profile",
  // which is what every caller that has no style library says.
  //
  // IT IS AN INPUT AND NOT A LOOKUP, and that is the whole point. This layer
  // refuses to know what styles exist (AttackTuning::styles says so at
  // length), but a zombie that lunges 22 voxels and a duelist that cuts at 10
  // cannot share one authored number — and putting 22 in the zombie's profile
  // would have it stand off at 22 with a sword in its hand too. So the stroke
  // system, which knows both the library and the rig, answers the question and
  // the arbiter simply uses the answer.
  float attackReach = 0.0f;
  // ---- ...AND HOW FAR IT REACHES WITH NO PROFILE FLOOR UNDER IT ----------
  //
  // `attackReach` above is `max(profile.reach, the styles)` — a FLOOR, so that
  // a creature commits from the distance its author had in mind. THE FOOTWORK
  // BAND CANNOT USE THAT NUMBER, because a floor is exactly the wrong shape
  // for "where should I stand": it is the one term that does not shrink when
  // the weapon does.
  //
  // Measured 2026-09-16, and it is the whole of "the AI has gone passive": the
  // `duelist` profile authors reach 10 and a band of 7..11 — both of them
  // SWORD numbers, because a sword is what it was tuned holding. Give the same
  // creature a dagger and every one of those numbers is still 10, 7 and 11,
  // while `BeginStroke` refuses any style the target is outside the reach OF
  // (mob.cpp, "ITS OWN REACH, BEFORE ANYTHING ELSE"). So it walks to 7..11,
  // holds there, commits on the profile's floor of 10, draws a dagger cut that
  // lands at about 3.5 — and the swing is dropped, silently, with the cadence
  // already spent. A mace lands at about 6 and squeaks through at the very
  // inner edge; a fist lands at 5 and does not; a dagger never swings at all.
  //
  // So this is the SAME sum WITHOUT the floor: the longest reach among the
  // styles this creature can actually use right now, 0 when none resolves
  // (which reads as "no opinion — keep the authored band"). `Think` pulls the
  // band in onto it, and only ever INWARD; see the band-geometry note there.
  float strikeReach = 0.0f;
  // ---- THE BODY'S CONDITION, for the rule facts (Fact::Hp and friends) -----
  // Filled by MobSystem::DecideIntent from the rig (Mob::BodyFacts). The
  // defaults are an unhurt creature, so a caller that knows nothing about a
  // body (a gate driving Think directly) gets rules that see a healthy one.
  float hpFrac = 1.0f;
  float burningFrac = 0.0f;
  int limbsLost = 0;
  // Ticks of shock stun left (Mob::StunTicksLeft; Fact::Shocked). 0 = none.
  float shocked = 0.0f;
  // Something is in this body's hand (Fact::Armed), and it can hold a guard
  // with it — a guard is a BLADE across a line, which is what the parry test
  // meets, so fists and teeth cannot.
  bool armed = false;
  bool canGuard = false;
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

// One NPC CAST request (D4): the attack seam's shape for magic. Drained by the
// owner (game/demon_cast.h MobCastTick), which draws the spell from the
// profile's CastTuning::spells, prices it against the creature's own mana and
// casts it through the spell VM. Nothing here knows what a spell is.
struct CastRequest {
  uint64_t mobId = 0;
  uint64_t targetId = 0;
  Vec3 targetPoint{};       // the target's centre, world voxels, at the decision
  Vec3 targetVel{};         // its low-passed velocity, voxels/sec (Brain::targetVel)
  uint32_t tick = 0;
  float distance = 0;       // centre-to-centre at the decision
};

// ---- THE ROUTINE: what the resident layer asks of the body (P7) -----------
//
// Written EVERY TICK by world/refs_npc.cpp (the resident controller, which
// runs in TickRefs before mobs.PreTick) for an NPC that has a schedule, and
// read by Think. The split is the arbiter's own: the resident layer decides
// WHERE (the schedule row, the anchor, the waynode route, the door it is
// waiting at) and this struct carries only the next place to put the feet and
// the pose to hold there. `active` false = no routine (every creature that is
// not an authored villager), and the six activity verbs cannot score.
struct Routine {
  bool active = false;
  Intent verb = Intent::Goto;   // which activity verb scores (Sleep..Goto)
  Vec3 goal{};                  // where the FEET go next, world voxels
  bool final = false;           // goal is the anchor itself (slow into it)
  float arriveRadius = 3.0f;    // world voxels, planar
  float speed = 0.5f;           // multiplier on the def's walk speed
  bool hold = false;            // stand still here (a closed door, a talk)
  bool faceHeadingSet = false;  // at the goal / holding: turn to this heading
  float faceHeading = 0.0f;
  bool facePointSet = false;    // ...or to look at this point (wins)
  Vec3 facePoint{};
  // OUT, written by Think: the feet are within arriveRadius of a final goal.
  bool arrived = false;
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

  // ---- HOW THE TARGET IS MOVING (2026-09-16) -------------------------------
  //
  // World voxels/sec, low-passed, and the ONLY thing in this struct that is
  // about the target's future rather than its present. Three decisions read it
  // and none of them can be made without it: where to aim a blow that lands
  // fifteen ticks from now, whether the range will still be closed by then, and
  // whether giving ground after a swing is footwork or surrender.
  //
  // Sampled ONLY between consecutive visible ticks of the SAME target
  // (`velSampleTick`), because the two other ways a remembered position moves
  // are both lies: `Perceive` freezes `targetPos` at `lastSeenPos` while the
  // alert decays, so a differenced position reports a creature standing still,
  // and the tick it is re-acquired it reports one that teleported.
  Vec3 targetVel{};
  Vec3 prevTargetPos{};
  uint32_t velSampleTick = 0;
  uint64_t velTargetId = 0;      // whose position prevTargetPos is
  bool haveTargetVel = false;
  // `targetVel` projected onto the line from us to it: + = opening the range.
  // The sign is the whole question — a target closing with us needs no pursuit
  // and no lead, and one that is leaving needs both.
  float targetRadial = 0;
  // Centre-to-centre distance PREDICTED for the moment the edge arrives, which
  // is what the commit test and the style draw are made against. A diagnostic
  // as much as a value: `targetDist` and this disagreeing by six voxels is the
  // difference between a creature that swings at you and one that swings at
  // where you were (CLAUDE.md rule 6 — record it, do not re-derive it later).
  float leadDist = 0;

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
  // The forward drive the pursuit term added on top of whatever the winning
  // intent asked for, this tick, and whether the disengage window was SKIPPED
  // because the target was already leaving. Both are diagnostics: "the mob did
  // not hit me" has four causes (never committed / committed and stood still /
  // closed but aimed behind / landed and did nothing) and from outside they are
  // one bare zero.
  float pursueDrive = 0;
  bool heldGround = false;

  // ---- rules ----
  // `prevHp` < 0 = no reading yet (the first think after spawn cannot have
  // been hurt). `facts` and `rulesHeld` are this tick's evaluation, kept for the
  // panel and the gates: "the creature did not flee" has two causes (the rule
  // never held / it held and lost the arbitration) and this separates them.
  float prevHp = -1.0f;
  uint32_t hurtTick = 0;
  bool everHurt = false;
  float facts[(int)Fact::Count] = {};
  uint32_t rulesHeld = 0;        // bit r = rule r held (the first 32 rules)
  bool hasThreat = false;        // what Flee runs from, when it runs
  Vec3 threatPos{};
  // This tick's `tune` values after every holding rule (identity when none).
  float tune[(int)Tune::Count] = {};

  // ---- READING THE TARGET'S BLOWS (Defense) --------------------------------
  // One incoming blow = one windup onset. The roll and the delay are taken
  // ONCE per onset, so a creature does not get a fresh chance to react every
  // tick of the same swing (which would make skill 0.1 a near-certainty).
  Action targetAction = Action::None;
  uint32_t blowOnset = 0;        // tick the current blow's windup was first seen
  uint64_t blowTarget = 0;       // whose blow it is (a target switch drops it)
  bool blowLive = false;         // a blow from the target is in the air
  uint32_t blowEndTick = 0;      // tick it stopped (for Defense::holdTicks)
  bool reacts = false;           // this blow's roll came up
  uint32_t reactAt = 0;          // ...and from when
  uint32_t defendedOnset = ~0u;  // the blow a guard/dodge actually answered
  int dodgeSign = 0;             // -1 / +1 sidestep, fixed for one dodge
  bool guardRaised = false;      // the AI owns the stroke's Guard phase
  // Target facts cached for the panel and the gates.
  float targetReach = 0, targetHp = 0;
  bool targetArmed = false;
  int pressRank = 0, engagedAllies = 0;

  // ---- attack pacing, with live `cadence` tuning ----
  uint32_t attackInterval = 0;   // cadence + jitter drawn at the last attack
  uint32_t riposteAt = 0;        // a riposte / feint follow-up may fire from here
  bool riposteArmed = false;
  uint32_t feintAt = 0;          // pull the live swing out at this tick
  bool feinting = false;

  // ---- counters (diagnostics; the gates assert on them) ----
  uint32_t guards = 0, dodges = 0, feints = 0, ripostes = 0;

  // ---- the cast clock (D4, CastTuning) ----
  // 0 = not armed yet: the first tick with a target books firstDelayTicks.
  uint32_t nextCastTick = 0;
  uint32_t lastCastTick = 0;
  uint32_t castsIssued = 0;

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

  // ---- the schedule's ask (P7) ----
  Routine routine;
  // ---- the profile's own idle wander (Movement::wanderRadius) ----
  // `wanderOwned`: the routine above was written by Think's generic wander
  // (not by a resident layer), so Think may also take it back.
  bool wanderOwned = false;
  bool wanderAnchorSet = false;
  Vec3 wanderAnchor{};
  Vec3 wanderGoal{};
  uint32_t wanderLeg = 0;         // legs walked; the goal's hash key
  uint32_t wanderPauseUntil = 0;  // standing still (coiled) until this tick
  uint32_t wanderLegSince = 0;    // the tick this leg began (stuck guard)

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
  // ---- the weapon arm, when not attacking (2026-09-27) ----
  // `guard`: hold the blade between us and `guardPoint` (world voxels — the
  // incoming weapon's point when it is known). The caller turns that into a
  // stroke Guard pose; false while `guardRaised` was set means lower it.
  bool guard = false;
  Vec3 guardPoint{};
  float guardReach = 0.85f;
  // A FEINT pulling out: abandon the live swing if it is still winding up.
  bool cancelSwing = false;
  // ---- magic (D4) ----
  bool cast = false;
  CastRequest castRequest;
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
