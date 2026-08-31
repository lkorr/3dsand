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
};

struct StyleLibrary {
  std::vector<AttackStyle> styles;
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

// Load assets/mobs/attack_styles.json. Follows every other loader here: a bad
// entry is skipped LOUDLY into `log` and is never fatal, and an unknown key is
// ignored so a newer authored file still loads on an older binary.
bool LoadAttackStyles(const std::string& path, StyleLibrary& out,
                      std::string& log);

// ---------------------------------------------------------------------------
// THE RUNTIME: one live swing.
//
// Owned by the Mob (one per creature), pure presentation state, never saved and
// never hashed. It holds its own MeleeState because that IS the stroke driver —
// an NPC that computed its own poses would be a second implementation of the
// feel, which is the thing melee.h's input surface exists to prevent.
struct NpcStroke {
  // GUARD is a WINDUP THAT NEVER ENDS: the same closed-loop, under-commitSpeed
  // drive to a stated pose, held indefinitely. It exists because "hold your
  // sword across this line" is a POSE and not an attack, and there was no way
  // to ask a creature for one — which is exactly what a defender in the block
  // gate, and a scripted encounter later, needs. Its target is ABSOLUTE
  // (`wantAz`/`wantEl` in the mob's own basis) rather than relative to an aim,
  // because a guard is not aimed at anything.
  enum class Phase : uint8_t { Idle = 0, Guard, Windup, Cut, Recover };

  MeleeState melee;
  Phase phase = Phase::Idle;
  int style = -1;
  int phaseTick = 0;         // ticks spent in the current phase
  int windupTicks = 0;       // after tempo jitter
  int cutTicks = 0;
  int recoverTicks = 0;
  uint64_t targetId = 0;
  Vec3 targetPoint{};        // world voxels, where the blow was aimed
  uint32_t seed = 0;         // mobId ^ salt ^ startTick; every draw keys off it
  // The aim, resolved ONCE at the end of the windup and never refreshed.
  float aimAz = 0, aimEl = 0;
  bool aimed = false;
  // Where the windup is steering to, in the mob's basis.
  float wantAz = 0, wantEl = 0, wantReach = 0;
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

  bool Active() const { return phase != Phase::Idle; }
  bool Cutting() const { return phase == Phase::Cut; }
  void Reset() { *this = NpcStroke{}; }
};

// Pick a style for one attack out of a profile's authored list. Counter-based
// on (mobId, tick) so the sequence replays; returns -1 when the list is empty
// or names nothing the library knows.
int PickAttackStyle(const StyleLibrary& lib,
                    const std::vector<std::string>& names, uint64_t mobId,
                    uint32_t tick);

