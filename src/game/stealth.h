#pragma once
// stealth.h — HOW HARD A BODY IS TO NOTICE, AND HOW A CREATURE NOTICES IT
// (docs/PLAN_stealth.md, DESIGN.md "Stealth and detection").
//
// The model is The Elder Scrolls' (Skyrim's detection, Oblivion's skills) with
// two Splinter Cell: Chaos Theory ideas on top, and it replaces nothing for a
// creature that does not ask for it: a profile with no `stealth` block, and
// any actor that does not publish an Exposure (a gate's hand-built list),
// keeps the instant, binary perception ai_behavior.cpp always had.
//
// ---------------------------------------------------------------------------
// TWO CHANNELS, ONE METER
//
// Every creature that senses keeps, per body it is tracking, an AWARENESS
// level 0..1 (Brain::aware). It is not a roll and not a switch: it is a meter
// that FILLS while the body is sensed and DRAINS while it is not, exactly as
// Skyrim's detection level climbs over successive checks. 1 is DETECTED -- the
// body becomes a target the ordinary arbiter fights; below that the creature
// is only SUSPICIOUS (it turns to look at `cautionAt`, walks over to look at
// `investigateAt`). The meter is fed by two channels:
//
//   SIGHT  — needs the cone and the line. Its rate is the target's
//            VISIBILITY (light on the body x stance x motion x size x its
//            sneak skill) against the viewer's reach (sightRange x its own
//            awareness skill), falling off with distance and toward the edge
//            of the cone. Light is the biggest term on purpose: a body in
//            shadow, crouched and still is all but invisible past arm's
//            length; the same body in torchlight is seen across a room.
//   SOUND  — needs neither cone nor line (a wall only muffles it). Every
//            body that moves makes NOISE, an audible radius in world voxels
//            (stance, speed, worn metal, the surface underfoot, its sneak
//            skill), and discrete EVENTS add more (a blow, a landing, a
//            scream, an arrow striking stone). Noise is heard against the
//            AMBIENT floor (rain, a storm): Chaos Theory's sound meter -- a
//            sound quieter than the world around the listener is no sound.
//            Sound alone stops at `soundCap`: it tells a creature WHERE to
//            look, and it must still SEE you to fight you -- unless it is
//            already fighting you, when hearing you run is enough to follow.
//
// ---------------------------------------------------------------------------
// HARDER TO LOSE THAN TO AVOID
//
// Once a creature is fighting a body (it is the target) the same body is
// judged by kinder rules for the viewer: its sight reach is multiplied by
// `combatSightMul`, the light on the body cannot read darker than
// `combatLightFloor` (it is staring at you; the shadow you were hiding in is
// no longer enough), and a sound from the body refreshes where it was last
// known. Losing it takes breaking the line for the profile's alertDecayTicks;
// then the creature SEARCHES (Intent::Search) round where it lost you, with
// its meter for you held high and filling faster, and only after that does it
// stand down -- still ALERTED for `alertedTicks`, its gains multiplied by
// `alertedGainMul` ("must have been the wind... but I am watching").
//
// ---------------------------------------------------------------------------
// DETERMINISM (CLAUDE.md rule 1)
//
// All of this is CPU float gameplay state beside the arbiter it feeds, under
// the same rules: it runs in the 30 Hz tick, its only randomness is the
// counter RNG keyed on (id, tick), and its inputs are the tick's published
// actors and the tick's noise events. Light is computed from the tick's day
// phase and rain word and the CPU's chunk caches -- the same stores the line
// of sight already reads -- never from a render target or the camera.

#include <cstdint>

#include "math3d.h"

struct Tuning;

namespace stealth {

// ---- A BODY'S STANCE ------------------------------------------------------
enum class Stance : uint8_t { Stand = 0, Crouch = 1, Prone = 2 };

// ---- HOW HIDDEN A BODY IS RIGHT NOW (published on every ai::Actor) --------
//
// Filled by whoever publishes the actor (MobSystem::PublishStealth for an NPC
// and for the players' avatars). `modelled` false -- the default, and what a
// gate's hand-built actor carries -- means "perceive it the old way": a
// stealth-enabled creature sees it at once inside range, cone and line, so a
// fixture written before this file existed still fights the way it did.
struct Exposure {
  bool modelled = false;
  // The body's own SNEAK skill, 0..100 (sidecar `stealth.sneak`, else the
  // profile's; a player's body with none, stealth.playerSneak).
  float sneak = 0.0f;
  Stance stance = Stance::Stand;
  // Planar speed over the body's own WALK speed: 0 still, 1 walking, ~1.8 a
  // sprint. A body's own walk, so a fast creature walking is not "running".
  float moveFrac = 0.0f;
  // Light ON the body, 0 (black) .. 1 (noon sun or a bonfire's face).
  float light = 1.0f;
  // Its MOVEMENT NOISE this tick: the radius, world voxels, at which a
  // listener of awareness 50 hears it in silence. 0 = silent (still).
  float noise = 0.0f;
  // Burning: the body is a light source itself (and a loud one).
  float burning = 0.0f;
};

// ---- WHAT ONE CREATURE CAN SENSE (behaviors.json profile `stealth`) -------
//
// Authored per profile; a sidecar's own `stealth` block overrides the two
// skills per character (MobDef::stealthSneak / stealthAwareness), so one
// "villager" profile can hold a keen old watchman and a dozy farmhand.
struct Senses {
  // false = this creature perceives the legacy way (no meter at all).
  bool enabled = false;
  // DETECTION SKILL 0..100 -- Oblivion's "the detector's own Sneak", Skyrim's
  // perception. 50 is an ordinary person; it scales both reaches.
  float awareness = 50.0f;
  // This creature's OWN sneak skill, 0..100: how hard IT is to notice.
  float sneak = 20.0f;
  // World voxels at which a fully VISIBLE body (lit, standing, walking, the
  // size of a person, no sneak skill) is just detectable by an awareness-50
  // creature. 0 = perception.sightRange.
  float sightRange = 0.0f;
  // Multiplier on every sound's audible radius. 0 = deaf.
  float hearing = 1.0f;
  // 0..1: the light a body reads as, at least. A cat, a demon, the undead.
  float darkVision = 0.0f;
  // The field of view for the meter, full width in degrees; 0 = the
  // profile's perception.fovDegrees. Skyrim's is 190: the blind spot behind
  // a guard is what makes walking up on one possible at all.
  float fovDegrees = 0.0f;
  // The sharp CENTRAL cone, full width in degrees; outside it (still inside
  // perception.fovDegrees) the gain falls to `peripheral` at the cone's edge.
  float centralDeg = 70.0f;
  float peripheral = 0.35f;
  // SEARCHING: how long (ticks) and how wide (world voxels) a creature hunts
  // round the place it lost a target before standing down.
  uint32_t searchTicks = 360;
  float searchRadius = 40.0f;
  // INVESTIGATING: how long (ticks) after the last stimulus it keeps looking.
  uint32_t investigateTicks = 240;
  // Walk speed (x the def's) while investigating / searching / stalking.
  float investigateSpeed = 0.45f;
  float stalkSpeed = 0.4f;
  // THE ALARM: entering a fight, it cries out -- a noise event this many
  // voxels loud, pointing at its target. Its allies hear it as a stimulus
  // (they come to look) and their meter for the intruder jumps by
  // stealth.alarmGain. 0 = a silent creature.
  float alarm = 0.0f;
};

// ---- THE GLOBAL COEFFICIENTS (tuning.json `stealth.*`) ---------------------
// Every number here has ONE row in sim/tuning_params.def, which is the only
// place its default is written: FromTuning copies the live values, and
// Defaults() is FromTuning of a default-constructed Tuning -- so a gate that
// builds no Tuning still runs the shipped model, and nothing here can drift
// from the .def. A Params made any other way is zeros: use one of the two.
// (The comments name what each term does; the values live in the .def.)
struct Params {
  // -- the meter --
  float cautionAt{};      // turns to look
  float investigateAt{};  // walks over to look
  float soundCap{};        // sound alone stops here
  float sightRate{};       // /s at point blank, full visibility
  float soundRate{};       // /s at the source
  float decay{};          // /s, calm
  float searchDecay{};    // /s while searching / for a fighting target
  float searchHold{};     // a lost target's meter at the search start
  float searchGainMul{};
  float alertedGainMul{};
  uint32_t alertedTicks{};
  // -- sight --
  float sightFalloff{};    // exponent on (1 - d/reach)
  float nearAlways{};     // voxels: inside this, light hardly matters
  float lightFloor{};     // the darkest a body reads
  float lightGamma{};
  float crouchVis{};
  float proneVis{};
  float stillVis{};       // a body that does not move
  float runVis{};         // added per walk-speed past a walk
  float sneakVisCut{};     // sneak 100 removes this share of visibility
  float awarenessReach{};  // reach x (0.6 + this x awareness/100)
  float refHeight{};      // voxels: the size a body counts as 1
  float combatSightMul{};
  float combatLightFloor{};
  float keepGain{};       // a fighting target is "seen" above this
  // -- sound --
  float walkNoise{};      // voxels, a person walking on stone
  float crouchNoise{};
  float proneNoise{};
  float runNoisePow{};
  float metalNoise{};      // a full suit of metal adds this share
  float sneakNoiseCut{};
  float splashNoise{};    // wading / swimming, per walk-speed
  float muffled{};         // through a wall
  float rainMask{};        // ambient at the hardest rain
  float alarmGain{};
  float landNoise{};      // a landing at landRefSpeed
  float landRefSpeed{};  // vox/s
  float strikeNoise{};   // a blow landing, at full power
  float voiceNoise{};    // a cry of pain
  float impactNoise{};    // an arrow / thrown thing striking matter
  float twangNoise{};     // a bow loosed
  // -- light --
  float nightLight{};
  float roofedDay{};      // daylight under a roof (doorways, windows)
  float overcastDim{};    // in rain
  float emitterRadius{};  // voxels: a glowing cell's reach
  float emitterGain{};
  float burningLight{};
  // -- sneak attacks --
  float sneakMelee{};
  float sneakRanged{};
  bool sneakNeedsCrouch{};
  // -- the player --
  float playerSneak{};
};

Params FromTuning(const Tuning& t);
const Params& Defaults();

// ---- ONE TRACKED BODY, AS ONE CREATURE KNOWS IT ----------------------------
struct Awareness {
  uint64_t id = 0;          // 0 = free slot
  float level = 0.0f;       // the meter, 0..1
  // DETECTED, with hysteresis: set when the meter reaches 1, kept until it
  // falls under `investigateAt` (or a search resets it: the body has to be
  // FOUND again). The arbiter's candidates and ai::Detects read this, not the
  // level -- otherwise a single tick's drain would un-detect a body the
  // creature was stabbed by a tick ago.
  bool detected = false;
  uint32_t lastTick = 0;    // last tick it was sensed at all
  Vec3 lastPos{};           // where, as sensed (sight: the body; sound: the noise)
  bool seen = false;        // by sight THIS tick
  bool heard = false;       // by sound THIS tick
  float sightGain = 0.0f;   // this tick's rates, /s (the panel and the gates)
  float soundGain = 0.0f;
};

// ---- A POINT OF INTEREST ----------------------------------------------------
// What an un-targeted creature is looking at or walking to: a body it is
// unsure of (kind Body), a noise it cannot put a body to (Noise), an ally's
// alarm (Alarm), or the place it lost its target (Lost: a search).
enum class StimKind : uint8_t { None = 0, Body, Noise, Alarm, Lost };
struct Stimulus {
  StimKind kind = StimKind::None;
  Vec3 pos{};
  float level = 0.0f;       // how sure, 0..1 (>= investigateAt: go and look)
  uint64_t source = 0;      // the body it is about, when known
  uint32_t tick = 0;        // last refreshed
  uint32_t until = 0;       // forgotten after
};

// ---- A NOISE EVENT (MobSystem::PostNoise) ------------------------------------
enum class NoiseKind : uint8_t { Step = 0, Land, Strike, Voice, Impact, Loose, Alarm, Blast };
struct Noise {
  Vec3 pos{};               // where it is heard FROM
  Vec3 point{};             // what it points at (an alarm: the intruder)
  float radius = 0.0f;      // audible radius at awareness 50 in silence, voxels
  uint64_t source = 0;      // the actor it betrays (0 = nobody: a distraction)
  uint32_t faction = ~0u;   // the crier's faction (an alarm is for its allies)
  NoiseKind kind = NoiseKind::Step;
};

// ---- THE ALERT STATE (what the HUD and the dev panel name) -----------------
enum class Alert : uint8_t { Calm = 0, Suspicious, Searching, Combat };
const char* AlertName(Alert a);

// ---- THE MATH -----------------------------------------------------------------
// Pure. Every term is named so a gate can assert it and the panel can print it.

// How VISIBLE a body is, independent of who looks or from where: light,
// stance, motion, size and its sneak skill. ~1 for a lit person walking.
// `lightFloor` is the viewer's (dark vision, a fight).
float Visibility(const Params& p, const Exposure& e, float heightVox,
                 float lightFloor);
// The sight reach, world voxels: where that visibility is just detectable by
// this viewer (its sightRange x awareness).
float SightReach(const Params& p, float sightRange, float awareness, float vis);
// Gain off the cone: 1 inside the central cone, down to `peripheral` at the
// fov's edge, 0 past it. `offAxis` radians, `fovDeg`/`centralDeg` full widths.
float ConeGain(float offAxis, float fovDeg, float centralDeg, float peripheral);
// Sight gain per second at distance `d` of a reach (0 at or past it).
float SightGain(const Params& p, float d, float reach, float cone);
// The movement noise of a body, world voxels of audible radius.
// `metal` 0..1 (worn metal), `surface` x (the ground underfoot), `wading`
// 0..1 (liquid at the legs).
float MovementNoise(const Params& p, Stance st, float moveFrac, float metal,
                    float surface, float wading, float sneak);
// The ambient floor this tick, 0 (silence) .. ~1 (a downpour), from the rain
// word's amount (0..255).
float Ambient(const Params& p, uint32_t rainAmount);
// Sound gain per second for a noise of `radius` at distance `d`, heard by a
// creature with `hearing` and `awareness`, `blocked` = through matter.
float SoundGain(const Params& p, float radius, float d, float hearing,
                float awareness, float ambient, bool blocked);
// The same as a one-shot (an event): how far the meter jumps.
float SoundImpulse(const Params& p, float radius, float d, float hearing,
                   float awareness, float ambient, bool blocked);
// The ambient light model's pieces, for MobSystem::LightAt: daylight 0..1
// from DaylightStrengthCpu's 0..255, an open-sky fraction, the rain amount,
// and the emitter sum (already distance-weighted, in glowing-cell units).
float SkyLight(const Params& p, uint32_t daylight255, float openFrac,
               uint32_t rainAmount);
float EmitterLight(const Params& p, float weightedCells);

}  // namespace stealth
