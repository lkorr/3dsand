#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "game/melee.h"
#include "math3d.h"

// ============================================================================
// ATTACK STYLES — an NPC swing, authored (assets/mobs/attack_styles.json).
//
// THE ONE IDEA. `game/melee.h` says the stroke driver "never sees a mouse: it
// consumes ABSTRACT CONTROL DELTAS, and an NPC attack is the same driver fed an
// AUTHORED CURVE instead". This file is that curve, and it is DATA rather than
// a table of C++ because CLAUDE.md design rule 4 applies to a swing exactly as
// it applies to a creature: adding an attack must be a JSON edit.
//
// So there is no `enum SwingKind`, no `switch (style)` and no list of style
// names anywhere in the engine. `ai_behavior.h` already refuses to know what
// styles exist — its `AttackTuning::styles` is a list of opaque authored ids —
// and a profile that names a style this file has never heard of gets a loud
// skip and its first available style, never a crash and never a silent no-op.
//
// ---------------------------------------------------------------------------
// THE SCHEMA, and why a stroke is three segments
//
//   {
//     "name":    "horizontal_r",
//     "label":   "Horizontal cut, weapon side across",
//     "windup":  { "ticks": 12, "az":  0.30, "el": 0.10, "reach": -0.05 },
//     "cut":     { "ticks":  7, "az": -2.30, "el": 0.00, "reach":  0.10 },
//     "recover": { "ticks": 10 },
//     "jitter":  { "az": 0.20, "el": 0.08, "tempo": 0.25 }
//   }
//
// WINDUP is a POSE, in the mob's own facing basis, expressed RELATIVE TO THE
// AIM (see below): azimuth 0 straight ahead and positive to the mob's right,
// elevation 0 level, `reach` a fraction of the arm's own reach band so a lunge
// is the same lunge on a long arm and a short one. It is driven closed-loop and
// deliberately SLOWLY — under `MeleeTuning::commitSpeed`, so the driver stays in
// Guard and no cut fires. Its length is the whole telegraph: 12 ticks is 0.40 s
// of visible blade raise, and there is no UI indicator by design.
//
// CUT is a TRAVEL, not a pose: how far the point goes and how fast. The deltas
// are divided by `cut.ticks` and delivered per tick at whatever pixel rate that
// implies, which is what commits the driver's own Slash and gives the sweep the
// tip speed that scales the damage (melee.h note 2, "SPEED IS THE DAMAGE").
//
// RECOVER is a hold with no input: the driver's own follow-through unwinds and
// the arm is handed back to the walk cycle over `PoseWeight`'s ramp.
//
// ---------------------------------------------------------------------------
// THE CUT IS CENTRED ON THE AIM, AND THE AIM IS TAKEN ONCE
//
// A committed cut does not home. The aim — the target's bearing and elevation
// about this mob's own shoulder — is resolved at the END OF THE WINDUP and
// never again, so a target that steps offline after the blade has started
// moving is missed, which is the entire point of a telegraph. The windup's own
// target is then `aim - cut/2 + windup`: the blade ends up half a cut short of
// the aim so that the MIDDLE of the travel passes through it, rather than the
// beginning. A stroke aimed at its own start point cuts the air behind the
// target every time.
//
// ---------------------------------------------------------------------------
// VARIATION IS DETERMINISTIC (CLAUDE.md rule 1)
//
// Ten swings must not look stamped, and they must still replay. Every draw is
// `rng::Hash3(mobId ^ salt, tick, index)` — the same counter-based hash the
// shader and the AI use — so the variation is a pure function of who swung and
// when. `jitter.az`/`el` bow the start pose; `jitter.tempo` scales the windup
// and cut tick counts, which is what stops two duelists beating time together.
// Nothing here reads a clock, an accumulator or a Jolt float.
//
// The whole thing is CPU float presentation state, exactly like the melee state
// and the gait it drives. Damage reaches the world only through
// MeleeSweepDamage's ordinary MutationQueue paths.
// ============================================================================

// One segment of a stroke program. `reach` is a FRACTION of the arm's reach
// band rather than voxels, for the kVoxelMeters reason every other length in
// this engine is derived: a bare "3 cells" halves in metres the moment the
// world scale moves.
struct StrokeSegment {
  int ticks = 8;
  float az = 0;      // windup: pose, relative to the aim. cut: travel.
  float el = 0;
  float reach = 0;
};

struct StrokeJitter {
  float az = 0;      // radians of start-azimuth bow, +-
  float el = 0;
  float tempo = 0;   // fraction of the tick counts, +-
};

struct AttackStyle {
  std::string name;    // the id a behaviour profile refers to
  std::string label;   // human text for the dev readout
  StrokeSegment windup;
  StrokeSegment cut;
  int recoverTicks = 10;
  StrokeJitter jitter;
  // AN AUTHORED BODY ANIMATION TO PLAY WITH THE STROKE, by name (empty = none).
  // The stroke program drives the WEAPON ARM through the melee driver; this is
  // everything else — the step, the shoulder drop, the off hand — keyframed in
  // the tuner's clip lane and saved to assets/anims/<name>.json, from where
  // LoadMobDefs compiles it onto every rig whose part names it fits (mob.cpp,
  // "THE SHARED CLIP LIBRARY"). Started by Mob::PlayClip at BeginStroke, so it
  // runs on the ordinary clip layer (mask, blend, mode) and the driver's arm
  // claim still wins on the arm. A name no rig knows is a loud loader line,
  // not a crash: PlayClip no-ops on a miss.
  std::string clip;
};

// THE PLAYER'S FLICK COMPASS (the `player` block of attack_styles.json): a
// screen-space direction per style, quantized by max dot at the attack press.
// Indices, not names — resolved against the library at load time (a sector
// naming an unknown style is skipped LOUDLY, the loader's convention), and
// rebuilt with the library on every R so hot-reload renumbering cannot bite.
// It lives ON the StyleLibrary because it is meaningless apart from one.
struct PlayerStrikeMap {
  struct Sector {
    float x = 0, y = 0;   // unit-ish, screen space: +x right, +y DOWN
    int style = -1;
  };
  std::vector<Sector> sectors;
  int neutral[2] = {-1, -1};   // the directionless click alternates these
  bool Usable() const { return !sectors.empty() || neutral[0] >= 0; }
};

struct StyleLibrary {
  std::vector<AttackStyle> styles;
  PlayerStrikeMap player;
  int Find(const std::string& n) const {
    for (size_t i = 0; i < styles.size(); i++)
      if (styles[i].name == n) return (int)i;
    return -1;
  }
  const AttackStyle* At(int i) const {
    return (i >= 0 && i < (int)styles.size()) ? &styles[i] : nullptr;
  }
  bool empty() const { return styles.empty(); }
};

// The flick (screen space, +y down) -> a style index by max dot over the
// sectors; -1 when the map has none. The caller decides what a -1 means
// (fall back to NeutralStrike, or don't swing).
int QuantizeStrike(const StyleLibrary& lib, float dx, float dy);
// The directionless click: one of the two neutral entries, `right` picking
// which. Returns the other one when the asked-for side is unresolved, and -1
// when neither is.
int NeutralStrike(const StyleLibrary& lib, bool right);

// Load assets/mobs/attack_styles.json. Follows every other loader here: a bad
// entry is skipped LOUDLY into `log` and is never fatal, and an unknown key is
// ignored so a newer authored file still loads on an older binary.
bool LoadAttackStyles(const std::string& path, StyleLibrary& out,
                      std::string& log);

// ---------------------------------------------------------------------------
// THE PROGRAM STATE of one live stroke, split from the NPC's swing (below)
// because TWO callers now replay authored programs through a MeleeState: a
// Mob's NpcStroke, and main.cpp's discrete player attacks (melee.controlMode
// 0), which own a bare cursor beside the player's own MeleeState. The split is
// exactly "what the stroke runner needs" — targets, edge memory and damage
// bookkeeping stay on NpcStroke, because the player's caller already owns
// those concerns its own way (main.cpp's sweep block and lastEdge* memory).
struct StrokeCursor {
  // GUARD is a WINDUP THAT NEVER ENDS: the same closed-loop, under-commitSpeed
  // drive to a stated pose, held indefinitely. It exists because "hold your
  // sword across this line" is a POSE and not an attack, and there was no way
  // to ask a creature for one — which is exactly what a defender in the block
  // gate, and a scripted encounter later, needs. Its target is ABSOLUTE
  // (`wantAz`/`wantEl` in the mob's own basis) rather than relative to an aim,
  // because a guard is not aimed at anything.
  enum class Phase : uint8_t { Idle = 0, Guard, Windup, Cut, Recover };

  Phase phase = Phase::Idle;
  int style = -1;
  int phaseTick = 0;         // ticks spent in the current phase
  int windupTicks = 0;       // after tempo jitter
  int cutTicks = 0;
  int recoverTicks = 0;
  uint32_t seed = 0;         // wielder ^ salt ^ startTick; every draw keys off it
  // The aim, resolved ONCE at the end of the windup and never refreshed.
  float aimAz = 0, aimEl = 0;
  bool aimed = false;
  // Where the windup is steering to, in the wielder's basis.
  float wantAz = 0, wantEl = 0, wantReach = 0;

  bool Active() const { return phase != Phase::Idle; }
  bool Cutting() const { return phase == Phase::Cut; }
  void Reset() { *this = StrokeCursor{}; }
};

// ---------------------------------------------------------------------------
// THE RUNTIME: one live NPC swing.
//
// Owned by the Mob (one per creature), pure presentation state, never saved and
// never hashed. It holds its own MeleeState because that IS the stroke driver —
// an NPC that computed its own poses would be a second implementation of the
// feel, which is the thing melee.h's input surface exists to prevent.
struct NpcStroke : StrokeCursor {
  MeleeState melee;
  uint64_t targetId = 0;
  Vec3 targetPoint{};        // world voxels, where the blow was aimed
  // The blade's edge as it was last tick, for the damage sweep. Not valid on
  // the first cut tick — there is no previous position to sweep from, and
  // inventing one is a free hit at the start of every swing.
  Vec3 edgeBase{}, edgeTip{};
  bool edgeValid = false;
  // Set when a parry arrested this stroke (game/melee.h EdgeSweepResult), so
  // the dev readout and the gates can tell "it finished" from "it was stopped".
  bool arrested = false;
  // ---- WHAT THIS SWING DID, recorded at the point it happened -------------
  //
  // "The NPC does not seem to hit very hard" has at least four causes — the
  // sweep never ran, it ran and the blade was too slow to do anything
  // (MeleeTuning::minSpeed), it was fast enough and passed through nothing, or
  // it hit and the wound was small — and from outside all four are the same
  // bare zero (CLAUDE.md rule 6). These three separate them for the cost of
  // three words per stroke, and they are what the gates assert on.
  int sweeps = 0;        // ticks a damage sweep actually ran
  int bodiesHit = 0;     // summed over those ticks
  float topTipSpeed = 0; // fastest the edge went, world voxels/sec

  // Shadows StrokeCursor::Reset on purpose: an NPC reset clears the whole
  // swing (melee state, edge memory, damage tallies), not just the program.
  void Reset() { *this = NpcStroke{}; }
};

// Pick a style for one attack out of a profile's authored list. Counter-based
// on (mobId, tick) so the sequence replays; returns -1 when the list is empty
// or names nothing the library knows.
int PickAttackStyle(const StyleLibrary& lib,
                    const std::vector<std::string>& names, uint64_t mobId,
                    uint32_t tick);

// ---------------------------------------------------------------------------
// THE SHARED STROKE-PROGRAM RUNNER.
//
// The phase machine that turns an authored AttackStyle into per-tick
// StrokeSamples, extracted from MobSystem::StepStroke so the player's discrete
// attacks (main.cpp, melee.controlMode 0) replay the same programs through
// their own MeleeState. Neither caller owns a copy of the feel: this is the
// only place a windup, a cut or a recover is stepped, exactly as melee.cpp is
// the only place a stroke is integrated.

// Fill a cursor's program fields for one swing: style index, seed, the
// tempo-jittered tick counts, phase = Windup. The CALLER resets its own
// container first (an NpcStroke also clears its target/edge/tally fields; the
// player's bare cursor has nothing else) and owns re-seeding/re-tuning the
// MeleeState the program will drive — see MobSystem::BeginStroke for why the
// tuning is re-applied per swing.
void BeginStrokeProgram(StrokeCursor& cur, const AttackStyle& sty,
                        int styleIndex, uint32_t seed);

// What one tick of the program did, so the caller knows whether to push a
// pose. Split three ways rather than a bool because the two "no pose" cases
// are different acts: Idle stepped nothing, Finished is the one tick where
// the caller must drop its pose claim (the NPC resets and pushes an empty
// WeaponPose; the player lets its MeleeState idle out).
enum class StrokeStepResult : uint8_t { Idle = 0, Live, Finished };

// One tick: synthesize the StrokeSample the current phase wants and Step the
// driver with it. `sty` may be null only for a Guard (a pose has no style).
// `liveAz`/`liveEl` are the aim's CURRENT bearing about the wielder's shoulder
// in the given basis — the NPC re-derives them from its target every tick and
// the windup tracks them until the commit freezes the aim; a player attack
// passes (0, 0), because the camera IS the aim. `dt` is the caller's tick.
StrokeStepResult StepStrokeProgram(StrokeCursor& cur, const AttackStyle* sty,
                                   MeleeState& m, float liveAz, float liveEl,
                                   float dt, const Vec3& right, const Vec3& up,
                                   const Vec3& fwd);

// An authored reach offset -> a radius the arm can actually serve. Public
// because the gates state their expectations in the same band positions the
// styles are authored in.
float StrokeReachIn(const MeleeState& m, float offset);

