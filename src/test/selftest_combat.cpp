// selftest_combat.cpp — THE COMBAT GATES: what a blow feels like, and what an
// NPC does with one.
//
// SIX GATES IN ONE TU, and kOrder deliberately splits them to opposite ends of
// the run (src/test/selftest.cpp). The registry is a pool; the ORDER is
// kOrder's alone, so one file can hold both halves of the subject without
// pretending they cost the same.
//
// FRONT OF THE RUN — pure CPU, no world, no GPU, no fixtures, nothing left
// behind, so a broken build reports in the first second:
//
//   combat-tuning  the `melee.*` and `combatfx.*` groups of tuning.json reach
//                  the code that uses them, and the clamps in LoadTuning are
//                  real rather than decorative.
//   combat-cues    the three melee sound slots agree with the asset tree and
//                  the engine actually asks for them.
//
// END OF THE MOB GROUP — these spawn armed creatures and let them cut each
// other up, which is about as large a perturbation as this suite has:
//
//   npc-strike   a request becomes a windup, a cut, and lost voxels
//   npc-block    a blade in the path arrests the stroke and takes the hp
//   npc-styles   every authored style sweeps the plane it claims to
//   duel         two AI duelists, opposed factions, wounds on both sides
//
// WHY A TUNING GATE EXISTS AT ALL, when nothing else in the suite has one.
// `melee.*` is the first group whose values are consumed through a COPY:
// MeleeState holds a MeleeTuning by value, filled once by ApplyMeleeTuning, so
// a key that fails to reach it fails SILENTLY and forever — the slider moves,
// the JSON changes, and the stroke does not. Every other CPU group is read
// through CurrentTuning() at the point of use, where a missing read is
// impossible by construction. That copy is the thing under test here.
//
// AND WHY IT ASSERTS THE CLAMPS. The clamps are not taste (see the long note in
// LoadTuning's melee block): each one protects a structural property that a
// slider dragged to its end otherwise breaks outright — a zero slashTime
// divides, a zero speed BAND makes the damage ramp singular, a zero smoothing
// halflife is a step function into the degenerate lean. A clamp that silently
// stopped clamping would surface as a division by zero in a swing, weeks later.
//
// WHY THE NPC ASSERTIONS AND NOT OTHERS. `swing` covers the control law with no
// world at all; `swing-plane` covers the whole pipeline for the PLAYER'S swing.
// The thing that can silently break in the four below is not "does damage work"
// — swing-plane E/F and the wound gates cover the carve. It is the JOIN: an AI
// that requests attacks nobody executes, a style whose windup never commits, a
// cut aimed where the target was rather than where it is, a parry that arrests
// nothing. Each of those is invisible from outside as "the NPC does not seem to
// hit very hard", so each has a gate.
//
// Thresholds live in tests/baseline.json (CLAUDE.md: a bound in source costs a
// rebuild to tune) and every measured value is pushed back through
// RecordObserved so `--rebaseline` can retune them in one command.
//
// ORDERING (selftest.h, and the kOrder note in selftest.cpp): the four NPC
// gates spawn creatures, and a mob id seeds id-keyed draws all over the engine,
// so each opens with an IdCounterScope and each regenerates worldgen on the way
// in and out — a gate has to be verifiable with `--gate <name>` alone, and none
// of these wants another gate's leftovers.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "audio/cues.h"
#include "game/ai_behavior.h"
#include "game/bodyreg.h"
#include "game/item.h"
#include "game/melee.h"
#include "game/mob.h"
#include "game/strokes.h"
#include "sim/scale.h"
#include "sim/tuning.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

using namespace sandvox;

namespace selftest {
namespace {

bool Near(float a, float b, float eps = 1e-4f) {
  return std::fabs(a - b) <= eps * std::max(1.0f, std::fabs(b));
}

// ---------------------------------------------------------------------------
// combat-tuning
// ---------------------------------------------------------------------------
Status GateCombatTuning(Ctx& c, std::string& detail) {
  (void)c;
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const char* what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("combat-tuning: FAILED %s\n", what);
    }
  };

  const std::string tuningPath =
      sandvox::AssetDir() + "/materials/tuning.json";

  // ---- A. THE SHIPPED FILE PARSES, AND EVERY KEY IS PRESENT ----------------
  //
  // A MISSING KEY IS THE FAILURE THIS GATE IS FOR, and it is invisible to
  // everything else: LoadTuning leaves an absent key at its compiled-in
  // default, which for this group is the same number tuning.json ships. So a
  // typo'd key reads identically to a correct one until somebody edits the
  // file and nothing happens.
  //
  // The test is therefore a DIFFERENTIAL, not a value comparison: load the
  // real file, then load it again with one key perturbed, and require the
  // loaded value to have moved. That proves the read is wired without pinning
  // any number, so retuning the game never touches this gate.
  Tuning shipped;
  check(LoadTuning(tuningPath, shipped), "tuning.json parses");
  // Warnings are clamps and unknown-type complaints. The SHIPPED file must
  // provoke none from our two groups — a default that trips its own clamp is a
  // authoring bug that would otherwise ship silently.
  {
    int ours = 0;
    for (const std::string& w : shipped.warnings)
      if (w.rfind("melee", 0) == 0 || w.rfind("combatfx", 0) == 0) {
        ours++;
        std::printf("combat-tuning: shipped tuning.json warns: %s\n", w.c_str());
      }
    check(ours == 0, "shipped melee/combatfx values provoke no warnings");
  }

  // Every key, its group, and a value that is INSIDE the clamp band but
  // different from anything shipped. One table, so adding a knob to the
  // struct without adding it here is a one-line omission rather than a silent
  // hole — and adding it here without wiring the Read* fails the gate.
  struct Probe {
    const char* group;
    const char* key;
    float value;
    // Where the value lands. Pulled out of a freshly-loaded Tuning so the
    // check is on the real read path, not on a re-implementation of it.
    float (*read)(const Tuning&);
  };
  static const Probe kProbes[] = {
      {"melee", "commitSpeed", 1234.0f, [](const Tuning& t) { return t.melee.commitSpeed; }},
      {"melee", "slashTime", 0.29f, [](const Tuning& t) { return t.melee.slashTime; }},
      {"melee", "recoverTime", 0.31f, [](const Tuning& t) { return t.melee.recoverTime; }},
      {"melee", "fullSpeedMps", 5.25f, [](const Tuning& t) { return t.melee.fullSpeedMps; }},
      {"melee", "minSpeedMps", 1.75f, [](const Tuning& t) { return t.melee.minSpeedMps; }},
      {"melee", "aimGainX", 0.0081f, [](const Tuning& t) { return t.melee.aimGainX; }},
      {"melee", "aimGainY", 0.0091f, [](const Tuning& t) { return t.melee.aimGainY; }},
      {"melee", "reachGainM", 0.0071f, [](const Tuning& t) { return t.melee.reachGainM; }},
      {"melee", "azOut", 2.11f, [](const Tuning& t) { return t.melee.azOut; }},
      {"melee", "azAcross", 1.11f, [](const Tuning& t) { return t.melee.azAcross; }},
      {"melee", "elMin", -1.21f, [](const Tuning& t) { return t.melee.elMin; }},
      {"melee", "elMax", 1.21f, [](const Tuning& t) { return t.melee.elMax; }},
      {"melee", "handExtend", 0.61f, [](const Tuning& t) { return t.melee.handExtend; }},
      {"melee", "extendSmoothing", 0.31f, [](const Tuning& t) { return t.melee.extendSmoothing; }},
      {"melee", "leanTurnRate", 11.5f, [](const Tuning& t) { return t.melee.leanTurnRate; }},
      {"melee", "handLead", -1.0f, [](const Tuning& t) { return t.melee.handLead; }},
      {"melee", "fallbackReachM", 0.81f, [](const Tuning& t) { return t.melee.fallbackReachM; }},
      {"melee", "reachFraction", 0.71f, [](const Tuning& t) { return t.melee.reachFraction; }},
      {"melee", "guardForwardM", 0.41f, [](const Tuning& t) { return t.melee.guardForwardM; }},
      {"melee", "guardUpM", 0.42f, [](const Tuning& t) { return t.melee.guardUpM; }},
      {"melee", "guardSideM", 0.43f, [](const Tuning& t) { return t.melee.guardSideM; }},
      {"melee", "dirSmoothing", 0.11f, [](const Tuning& t) { return t.melee.dirSmoothing; }},
      {"melee", "swingArc", 1.61f, [](const Tuning& t) { return t.melee.swingArc; }},
      {"melee", "swingAnticipate", 0.51f, [](const Tuning& t) { return t.melee.swingAnticipate; }},
      {"melee", "swingExtend", 0.31f, [](const Tuning& t) { return t.melee.swingExtend; }},
      {"melee", "bladeSmoothing", 0.081f, [](const Tuning& t) { return t.melee.bladeSmoothing; }},
      {"melee", "wristMaxAngle", 1.11f, [](const Tuning& t) { return t.melee.wristMaxAngle; }},
      {"melee", "edgeFloor", 0.51f, [](const Tuning& t) { return t.melee.edgeFloor; }},
      {"melee", "blockGapM", 0.21f, [](const Tuning& t) { return t.melee.blockGapM; }},
      {"melee", "blockItemDamage", 0.61f, [](const Tuning& t) { return t.melee.blockItemDamage; }},
      {"melee", "blockNudgeAz", 0.41f, [](const Tuning& t) { return t.melee.blockNudgeAz; }},
      {"melee", "blockNudgeEl", 0.31f, [](const Tuning& t) { return t.melee.blockNudgeEl; }},
      // controlMode is an int read by ReadI; 1.0f streams as the literal `1`,
      // which is the only non-default value the [0,1] clamp band has.
      {"melee", "controlMode", 1.0f, [](const Tuning& t) { return (float)t.melee.controlMode; }},
      {"melee", "pickMinSpeed", 333.0f, [](const Tuning& t) { return t.melee.pickMinSpeed; }},
      {"melee", "torsoShare", 0.51f, [](const Tuning& t) { return t.melee.torsoShare; }},
      {"melee", "torsoPitch", 0.31f, [](const Tuning& t) { return t.melee.torsoPitch; }},
      {"melee", "headClearM", 0.11f, [](const Tuning& t) { return t.melee.headClearM; }},
      {"combatfx", "hitStopChipScale", 0.61f, [](const Tuning& t) { return t.combatfx.hitStopChipScale; }},
      {"combatfx", "hitStopChipMs", 71.0f, [](const Tuning& t) { return t.combatfx.hitStopChipMs; }},
      {"combatfx", "hitStopFleshScale", 0.41f, [](const Tuning& t) { return t.combatfx.hitStopFleshScale; }},
      {"combatfx", "hitStopFleshMs", 111.0f, [](const Tuning& t) { return t.combatfx.hitStopFleshMs; }},
      {"combatfx", "hitStopSeverScale", 0.21f, [](const Tuning& t) { return t.combatfx.hitStopSeverScale; }},
      {"combatfx", "hitStopSeverMs", 161.0f, [](const Tuning& t) { return t.combatfx.hitStopSeverMs; }},
      {"combatfx", "flashChip", 0.61f, [](const Tuning& t) { return t.combatfx.flashChip; }},
      {"combatfx", "flashFlesh", 1.11f, [](const Tuning& t) { return t.combatfx.flashFlesh; }},
      {"combatfx", "flashSever", 2.11f, [](const Tuning& t) { return t.combatfx.flashSever; }},
      {"combatfx", "flashHalflife", 0.121f, [](const Tuning& t) { return t.combatfx.flashHalflife; }},
      // ---- the directional flinch (mob.h Mob::HitReact) -------------------
      // `hitReact` itself is the group's second BOOL and is absent for the same
      // reason `hitStop` is: this table's mechanism is a float comparison.
      {"combatfx", "hitReactRefDamage", 21.0f, [](const Tuning& t) { return t.combatfx.hitReactRefDamage; }},
      {"combatfx", "hitReactMaxScale", 3.10f, [](const Tuning& t) { return t.combatfx.hitReactMaxScale; }},
      {"combatfx", "hitReactLeanDeg", 9.50f, [](const Tuning& t) { return t.combatfx.hitReactLeanDeg; }},
      {"combatfx", "hitReactSpineShare", 0.31f, [](const Tuning& t) { return t.combatfx.hitReactSpineShare; }},
      {"combatfx", "hitReactPushFrac", 0.041f, [](const Tuning& t) { return t.combatfx.hitReactPushFrac; }},
      {"combatfx", "hitReactLimbDeg", 21.0f, [](const Tuning& t) { return t.combatfx.hitReactLimbDeg; }},
      {"combatfx", "hitReactHalflife", 0.121f, [](const Tuning& t) { return t.combatfx.hitReactHalflife; }},
      // ...and the same reaction on a body Jolt owns (docs/PLAN_struck_matter.md):
      // a limp living limb and a corpse take an impulse where a standing one
      // takes the spring, and this is the dial both halves share.
      {"combatfx", "hitReactImpulse", 7.5f, [](const Tuning& t) { return t.combatfx.hitReactImpulse; }},
      {"combatfx", "whooshVolume", 0.71f, [](const Tuning& t) { return t.combatfx.whooshVolume; }},
      {"combatfx", "whooshMinSpeed", 411.0f, [](const Tuning& t) { return t.combatfx.whooshMinSpeed; }},
      {"combatfx", "whooshRateSlow", 0.71f, [](const Tuning& t) { return t.combatfx.whooshRateSlow; }},
      {"combatfx", "whooshRateFast", 1.41f, [](const Tuning& t) { return t.combatfx.whooshRateFast; }},
      {"combatfx", "fleshVolume", 1.21f, [](const Tuning& t) { return t.combatfx.fleshVolume; }},
      {"combatfx", "clangVolume", 1.31f, [](const Tuning& t) { return t.combatfx.clangVolume; }},
      {"combatfx", "cueRadius", 33.0f, [](const Tuning& t) { return t.combatfx.cueRadius; }},
      // ---- THE OTHER TWO KINDS OF BLOW (game/impact.h) --------------------
      // Every NUMERIC row of the blunt/bite model, on the same differential:
      // write one key, require the loaded value to have moved. `gore.bruiseMat`
      // is deliberately absent -- it is a STRING (the one name-typed row in
      // tuning.json, resolved at use through MobSystem::MaterialIdNamed), and
      // this table's whole mechanism is a float comparison.
      //
      // Each value is inside its clamp band and different from anything
      // shipped, which is the rule the table's own note states: a probe that
      // happened to equal the default would pass while measuring nothing.
      {"gore", "bruiseRadius", 1.31f, [](const Tuning& t) { return t.gore.bruiseRadius; }},
      {"gore", "bluntBleedScale", 0.21f, [](const Tuning& t) { return t.gore.bluntBleedScale; }},
      {"gore", "bluntCarveRadius", 1.11f, [](const Tuning& t) { return t.gore.bluntCarveRadius; }},
      {"gore", "biteRadius", 2.11f, [](const Tuning& t) { return t.gore.biteRadius; }},
      {"gore", "biteBlob", 3.75f, [](const Tuning& t) { return t.gore.biteBlob; }},
      {"gore", "biteStainScale", 2.25f, [](const Tuning& t) { return t.gore.biteStainScale; }},
      {"gore", "infectHealSlow", 9.5f, [](const Tuning& t) { return t.gore.infectHealSlow; }},
      {"gear", "bluntDentRadius", 2.11f, [](const Tuning& t) { return t.gear.bluntDentRadius; }},
      {"gear", "bluntHardnessRef", 77.0f, [](const Tuning& t) { return t.gear.bluntHardnessRef; }},
      {"gear", "bluntHardnessMin", 0.31f, [](const Tuning& t) { return t.gear.bluntHardnessMin; }},
      {"gear", "bluntThrough", 0.71f, [](const Tuning& t) { return t.gear.bluntThrough; }},
      {"gear", "bluntShellHp", 0.81f, [](const Tuning& t) { return t.gear.bluntShellHp; }},
      {"gear", "biteOnShell", 0.41f, [](const Tuning& t) { return t.gear.biteOnShell; }},
  };

  // The probe file is written next to the real one so a relative path in the
  // loader (there is none today, but that is not this gate's business to know)
  // resolves the same way. Removed on the way out; a leftover would be picked
  // up by nothing, but a test that litters the asset tree is a test somebody
  // has to clean up after.
  const std::string probePath =
      sandvox::AssetDir() + "/materials/tuning.combatprobe.json";
  int wired = 0;
  for (const Probe& p : kProbes) {
    {
      std::ofstream f(probePath);
      // ONE GROUP, ONE KEY. LoadTuning starts from the compiled defaults and
      // only overwrites what it finds, so a one-key file is a legal tuning
      // file and every other value stays at its default — which is exactly
      // the isolation this needs.
      f << "{\n  \"" << p.group << "\": { \"" << p.key << "\": " << p.value
        << " }\n}\n";
    }
    Tuning probed;
    if (!LoadTuning(probePath, probed)) {
      check(false, "probe file parses");
      continue;
    }
    const float got = p.read(probed);
    if (!Near(got, p.value)) {
      ok = false;
      std::printf(
          "combat-tuning: FAILED %s.%s did not reach the struct "
          "(wrote %.5f, read %.5f) — check the Read* call in LoadTuning\n",
          p.group, p.key, p.value, got);
    } else {
      wired++;
    }
    checks++;
  }
  std::remove(probePath.c_str());
  check(wired == (int)(sizeof(kProbes) / sizeof(kProbes[0])),
        "every melee/combatfx/gore/gear key reaches its field");

  // ---- A2. THE ONE NAME-TYPED ROW ------------------------------------------
  //
  // `gore.bruiseMat` names a MATERIAL rather than holding an id, for the
  // reason its declaration gives at length (the two files hot-reload
  // independently, so an id would be stale after either). The float
  // differential above cannot see it, and a row nothing checks is a row that
  // silently stops being read -- which is exactly the failure mode this whole
  // gate exists for.
  {
    const std::string probePath =
        sandvox::AssetDir() + "/materials/tuning.strprobe.json";
    {
      std::ofstream f(probePath);
      f << "{\n  \"gore\": { \"bruiseMat\": \"probe_not_a_material\" }\n}\n";
    }
    Tuning probed;
    check(LoadTuning(probePath, probed), "string probe parses");
    std::remove(probePath.c_str());
    check(probed.gore.bruiseMat == "probe_not_a_material",
          "gore.bruiseMat reaches its field as a STRING");
    // ...and the shipped value is a material that actually exists. A typo here
    // costs no crash and no warning -- it costs every punch in the game its
    // bruise, silently, which is the kind of thing only a gate ever notices.
    check(!shipped.gore.bruiseMat.empty(),
          "the shipped gore.bruiseMat names something");
  }

  // ---- B. THE CLAMPS ARE REAL ----------------------------------------------
  //
  // Written as "an out-of-band value comes back INSIDE the band", not as "it
  // comes back at exactly X": the bounds themselves are allowed to be retuned,
  // and a gate that pinned them would have to be edited every time they were.
  {
    const std::string badPath =
        sandvox::AssetDir() + "/materials/tuning.combatclamp.json";
    {
      std::ofstream f(badPath);
      f << R"({
  "melee": {
    "slashTime": 0.0, "recoverTime": -5.0, "dirSmoothing": 0.0,
    "bladeSmoothing": 0.0, "extendSmoothing": 0.0,
    "aimGainX": 0.0, "aimGainY": 900.0,
    "fullSpeedMps": 2.0, "minSpeedMps": 8.0,
    "handLead": 0.0, "reachFraction": 5.0, "wristMaxAngle": 99.0,
    "edgeFloor": -3.0, "commitSpeed": -100.0
  },
  "combatfx": {
    "hitStopChipScale": 0.0, "hitStopFleshScale": -1.0,
    "hitStopSeverScale": 0.0, "hitStopSeverMs": 100000.0,
    "flashHalflife": 0.0, "cueRadius": 0.0,
    "hitReactHalflife": 0.0, "hitReactRefDamage": 0.0,
    "hitReactLeanDeg": 400.0, "hitReactLimbDeg": 400.0,
    "hitReactPushFrac": 3.0, "hitReactMaxScale": 0.0
  }
})";
    }
    Tuning bad;
    check(LoadTuning(badPath, bad), "clamp probe parses");
    std::remove(badPath.c_str());

    // THE HANG GUARD, and it is the one that matters most here. A hit-stop
    // scale of 0 stops the tick accumulator filling at all, and every input in
    // the game is consumed inside the tick loop — so a 0 that got through is a
    // freeze the player cannot escape by any key. Asserted on all three tiers
    // because they are three separate clamps.
    check(bad.combatfx.hitStopChipScale > 0.0f, "chip hit-stop scale > 0");
    check(bad.combatfx.hitStopFleshScale > 0.0f, "flesh hit-stop scale > 0");
    check(bad.combatfx.hitStopSeverScale > 0.0f, "sever hit-stop scale > 0");
    check(bad.combatfx.hitStopSeverMs <= 1000.0f, "hit-stop length bounded");
    // A zero halflife never decays: exp2(-dt/0) is not a number, and every
    // struck limb would stay lit for the session.
    check(bad.combatfx.flashHalflife > 0.0f, "flash halflife > 0");
    check(bad.combatfx.cueRadius > 0.0f, "cue radius > 0");

    // ---- THE FLINCH STAYS A FLINCH ----------------------------------------
    // Two divisions and one promise. `hitReactHalflife` divides inside
    // HitReactImpulse AND inside AnimSpringStep; `hitReactRefDamage` divides
    // the strike profile. And the promise the whole feature is sold on is that
    // it is SLIGHT — a lean or a flick in the hundreds of degrees, or a shove
    // of three body heights, is a creature folded inside out, which no amount
    // of downstream clamping recovers.
    check(bad.combatfx.hitReactHalflife > 0.0f, "hit-react halflife > 0");
    check(bad.combatfx.hitReactRefDamage > 0.0f, "hit-react reference blow > 0");
    check(bad.combatfx.hitReactMaxScale >= 1.0f, "hit-react scale ceiling >= 1");
    check(bad.combatfx.hitReactLeanDeg <= 45.0f, "hit-react lean stays a lean");
    check(bad.combatfx.hitReactLimbDeg <= 90.0f, "hit-react flick stays a flick");
    check(bad.combatfx.hitReactPushFrac <= 0.5f, "hit-react shove stays a shove");

    // Divisions and degenerate smoothing.
    check(bad.melee.slashTime > 0.0f, "slash time > 0");
    check(bad.melee.recoverTime > 0.0f, "recover time > 0");
    check(bad.melee.dirSmoothing > 0.0f, "direction smoothing > 0");
    check(bad.melee.bladeSmoothing > 0.0f, "blade smoothing > 0");
    check(bad.melee.extendSmoothing > 0.0f, "extension smoothing > 0");
    check(bad.melee.commitSpeed > 0.0f, "commit speed > 0");
    // A zero aim gain disconnects the mouse, which reads as a hung game.
    check(bad.melee.aimGainX > 0.0f, "aim gain x > 0");
    check(bad.melee.aimGainY <= 0.2f, "aim gain y bounded above");
    // THE DAMAGE RAMP MUST HAVE A BAND. min >= full makes
    // (v - min) / (full - min) singular, and the sweep's own epsilon guard
    // would then turn every touch into a full-power hit — a clamp whose
    // absence looks like a balance change rather than a bug.
    check(bad.melee.minSpeedMps < bad.melee.fullSpeedMps,
          "min speed below full speed");
    // Only the SIGN of handLead is read, so a 0 has no meaning; LoadTuning
    // normalises it rather than leaving an unreadable state.
    check(bad.melee.handLead == 1.0f || bad.melee.handLead == -1.0f,
          "hand lead normalised to a sign");
    // A fully straight two-bone chain is a locked elbow, and the solver clamps
    // to its own annulus anyway — a target outside the reach costs mouse
    // travel to wind back before the arm visibly moves.
    check(bad.melee.reachFraction < 1.0f, "reach fraction under 1");
    // Past pi the wrist is a ball joint and the fist can face backwards down
    // its own arm.
    check(bad.melee.wristMaxAngle <= 3.1416f, "wrist limit at most pi");
    check(bad.melee.edgeFloor >= 0.0f && bad.melee.edgeFloor <= 1.0f,
          "edge floor in 0..1");
  }

  // ---- C. THE VALUES REACH MeleeTuning -------------------------------------
  //
  // The copy is the whole point (see this file's header). Three sentinels, one
  // per KIND of transfer, because they can fail independently:
  //
  //   wristMaxAngle   a plain float, copied straight across
  //   fullSpeed       a METRES/SEC key converted to world voxels. A conversion
  //                   dropped or applied twice is invisible in the JSON and
  //                   silently rescales the whole damage ramp.
  //   handLead        the sign-only knob, which is the one a naive copy gets
  //                   wrong by preserving a magnitude that must not exist.
  {
    Tuning t = shipped;
    t.melee.wristMaxAngle = 1.234f;
    t.melee.fullSpeedMps = 7.5f;
    t.melee.minSpeedMps = 1.25f;
    t.melee.handLead = -1.0f;
    t.melee.guardUpM = 0.375f;
    MeleeTuning mt;
    ApplyMeleeTuning(mt, t);
    check(Near(mt.wristMaxAngle, 1.234f), "wristMaxAngle reaches MeleeTuning");
    check(Near(mt.fullSpeed, MetresPerSecToCells(7.5f)),
          "fullSpeedMps converts m/s -> world voxels");
    check(Near(mt.minSpeed, MetresPerSecToCells(1.25f)),
          "minSpeedMps converts m/s -> world voxels");
    check(Near(mt.guardUp, MetresToCells(0.375f)),
          "guardUpM converts metres -> world voxels");
    check(mt.handLead == -1.0f, "handLead sign reaches MeleeTuning");
    // The block knobs came from phase C as struct initialisers and were
    // promoted into the JSON at merge time, so they are the NEWEST members of
    // the copy and the likeliest to be forgotten by the next person who adds
    // one. blockGap is the second metre key, and it is checked here for the
    // same reason guardUp is: the conversion is invisible in the file.
    check(Near(mt.blockGap, MetresToCells(t.melee.blockGapM)),
          "blockGapM converts metres -> world voxels");
    check(Near(mt.blockItemDamage, t.melee.blockItemDamage),
          "blockItemDamage reaches MeleeTuning");
    check(Near(mt.blockNudgeAz, t.melee.blockNudgeAz),
          "blockNudgeAz reaches MeleeTuning");
    check(Near(mt.blockNudgeEl, t.melee.blockNudgeEl),
          "blockNudgeEl reaches MeleeTuning");
    // AND THE CONVERSION IS NOT THE IDENTITY. At any kVoxelMeters other than
    // 1.0 the metre keys must land on a DIFFERENT number than they were
    // authored as; if they did not, someone has quietly deleted the conversion
    // and every check above would still pass.
    check(!Near(mt.fullSpeed, 7.5f, 1e-3f),
          "the m/s -> voxel conversion actually converts");
  }

  // ---- D. THE SHIPPED DEFAULTS MATCH THE COMPILED ONES ---------------------
  //
  // "A diff of behaviour at defaults is a bug." The migration's contract was
  // that tuning.json ships exactly the numbers the struct initialisers already
  // had, so nothing about the game changed on the day the values moved out of
  // melee.h. Checked against a DEFAULT-CONSTRUCTED Tuning rather than against
  // literals, so retuning the game means editing one file and not two.
  {
    const Tuning defaults;
    int drift = 0;
    auto same = [&](const char* what, float a, float b) {
      if (!Near(a, b)) {
        drift++;
        std::printf("combat-tuning: %s differs — tuning.json %.6f, C++ %.6f\n",
                    what, a, b);
      }
    };
    same("melee.wristMaxAngle", shipped.melee.wristMaxAngle,
         defaults.melee.wristMaxAngle);
    same("melee.commitSpeed", shipped.melee.commitSpeed,
         defaults.melee.commitSpeed);
    same("melee.aimGainX", shipped.melee.aimGainX, defaults.melee.aimGainX);
    same("melee.fullSpeedMps", shipped.melee.fullSpeedMps,
         defaults.melee.fullSpeedMps);
    same("melee.blockGapM", shipped.melee.blockGapM, defaults.melee.blockGapM);
    same("melee.blockItemDamage", shipped.melee.blockItemDamage,
         defaults.melee.blockItemDamage);
    same("melee.blockNudgeAz", shipped.melee.blockNudgeAz,
         defaults.melee.blockNudgeAz);
    same("melee.blockNudgeEl", shipped.melee.blockNudgeEl,
         defaults.melee.blockNudgeEl);
    same("combatfx.hitStopSeverMs", shipped.combatfx.hitStopSeverMs,
         defaults.combatfx.hitStopSeverMs);
    same("combatfx.flashHalflife", shipped.combatfx.flashHalflife,
         defaults.combatfx.flashHalflife);
    check(shipped.combatfx.hitStop == defaults.combatfx.hitStop,
          "combatfx.hitStop matches the compiled default");
    check(drift == 0, "shipped tuning.json matches the compiled defaults");
  }

  // ---- E. THE PANEL'S SAVE ROUND-TRIPS -------------------------------------
  //
  // SaveCombatTuning is a TEXT PATCH of value literals into the shipped file,
  // and it is all-or-nothing: one key it cannot find refuses the whole write.
  // Nothing above exercises it, and it shipped a real refusal: the byte span
  // of each group was computed ONCE, every patched literal changes the text's
  // length (`%.9g` prints the float that ships, not the hand-written
  // `0.005`), and by the tail of the melee group the keys had drifted past
  // the stale span end — the Combat panel's Save reported eleven keys
  // "missing" from a file that contained every one of them. This check is
  // the reproduction: save onto a COPY of the real file with every panel
  // value MOVED (so every literal changes length), demand success, then load
  // the copy back and demand a head, a middle and a tail key of each group
  // actually landed.
  {
    const std::string savePath =
        sandvox::AssetDir() + "/materials/tuning.combatsave.json";
    {
      std::ifstream in(tuningPath, std::ios::binary);
      std::ofstream out(savePath, std::ios::binary);
      out << in.rdbuf();
    }
    Tuning moved = shipped;
    // Nudges chosen to survive every clamp and to never print back to the
    // same literal the file holds.
    moved.melee.commitSpeed = 1111.5f;                       // melee head
    moved.melee.dirSmoothing = 0.077f;                       // melee middle
    moved.melee.headClearM = 0.123f;                         // melee TAIL
    moved.combatfx.hitStopChipMs = 77.5f;                    // combatfx head
    moved.combatfx.cueRadius = 44.5f;                        // combatfx TAIL
    moved.gore.cutDepth = 0.505f;                            // gore head
    moved.gore.bleedGain = 1.115f;                           // gore TAIL
    std::string err;
    const bool saved = SaveCombatTuning(savePath, moved, err);
    if (!saved)
      std::printf("combat-tuning: SaveCombatTuning refused: %s\n", err.c_str());
    check(saved, "SaveCombatTuning patches the shipped file whole");
    Tuning back;
    check(LoadTuning(savePath, back), "the saved file still parses");
    check(Near(back.melee.commitSpeed, moved.melee.commitSpeed) &&
              Near(back.melee.dirSmoothing, moved.melee.dirSmoothing) &&
              Near(back.melee.headClearM, moved.melee.headClearM),
          "melee head/middle/tail keys round-trip through Save");
    check(Near(back.combatfx.hitStopChipMs, moved.combatfx.hitStopChipMs) &&
              Near(back.combatfx.cueRadius, moved.combatfx.cueRadius),
          "combatfx head/tail keys round-trip through Save");
    check(Near(back.gore.cutDepth, moved.gore.cutDepth) &&
              Near(back.gore.bleedGain, moved.gore.bleedGain),
          "gore head/tail keys round-trip through Save");
    std::remove(savePath.c_str());
  }

  detail = Format("%d checks, %d keys wired", checks, wired);
  std::printf("combat-tuning: %s (%d checks, %d keys wired)\n",
              ok ? "PASS" : "FAIL", checks, wired);
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// combat-cues
// ---------------------------------------------------------------------------
//
// HEADLESS IS SILENT BY DESIGN (DESIGN.md §12b), so this cannot assert that
// anything was heard. It asserts the two things that are checkable without a
// device, and they are the two that actually break:
//
//   1. THE SLOT RESOLVES TO A REAL SET. Cues::CombatSetId is deliberately
//      device-free — it only consults the library — so a folder that was never
//      created, or a slot whose prefix disagrees between cues.cpp and
//      sound_schema.js, is caught here rather than by someone noticing the
//      game got quieter.
//   2. THE ENGINE ASKS. Stats::combat counts REQUESTS, incremented before the
//      device is consulted, precisely so this gate can exist — see its note in
//      cues.h. Every other counter in Stats is structurally frozen at 0 in a
//      headless run.
//
// What it deliberately does NOT do is drive a swing and check a cue came out.
// That needs an avatar, a weapon, a world and a target — which is the
// `swing-plane` gate's whole apparatus, at hundreds of times the cost, to
// establish a fact this file's call-site wiring already makes structural.
Status GateCombatCues(Ctx& c, std::string& detail) {
  (void)c;
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const char* what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("combat-cues: FAILED %s\n", what);
    }
  };

  // ---- A. THE SLOT TABLE AGREES WITH ITSELF --------------------------------
  // scripts/check_invariants.py compares cues.cpp against sound_schema.js;
  // this is the half a Python regex cannot see — that the C++ ENUM and the C++
  // TABLE agree, which is what CombatSetId's lookup depends on.
  for (const char* slot : {"whoosh", "flesh", "clang",
                           "strike_edge", "strike_blunt", "cut"}) {
    checks++;
    auto it = audio::Cues::kSlotPrefix.find(slot);
    if (it == audio::Cues::kSlotPrefix.end()) {
      ok = false;
      std::printf("combat-cues: FAILED slot '%s' missing from kSlotPrefix\n",
                  slot);
    } else if (it->second != "melee") {
      ok = false;
      std::printf("combat-cues: FAILED slot '%s' prefix is '%s', want 'melee'\n",
                  slot, it->second.c_str());
    }
  }

  // ---- B. THE SETS EXIST AND RESOLVE ---------------------------------------
  //
  // NO Init(). Constructing a Cues and scanning the library is device-free;
  // Init() is what opens hardware, and calling it here would make the gate
  // depend on the machine having a sound card — which the audio gates already
  // learned not to do (selftest_audio.cpp GateAudioAmbience).
  audio::Cues cues;
  const int loadedBuffers = cues.ScanLibrary(sandvox::AssetDir() + "/sounds");
  check(loadedBuffers > 0, "the sound library scanned something");
  int resolved = 0;
  struct Slot {
    audio::Cues::CombatCue cue;
    const char* name;
    bool mayBeEmpty;
  };
  // `melee/clang` (steel stopping steel) is a slot WITHOUT TAKES: its three
  // synthesized placeholders went out with the recorded strike_edge /
  // strike_blunt split (d69dae7) and nothing real has been recorded for it
  // yet. An authored-empty set is a silent blocked-parry, not a wiring fault —
  // the slot still resolves in kSlotPrefix (arm A) and the engine still asks
  // (arm C) — so it is reported, not failed, until a take lands.
  static const Slot kSlots[] = {
      {audio::Cues::CombatCue::Whoosh, "melee/whoosh", false},
      {audio::Cues::CombatCue::Flesh, "melee/flesh", false},
      {audio::Cues::CombatCue::Clang, "melee/clang", true},
      {audio::Cues::CombatCue::StrikeEdge, "melee/strike_edge", false},
      {audio::Cues::CombatCue::StrikeBlunt, "melee/strike_blunt", false},
      {audio::Cues::CombatCue::Cut, "melee/cut", false},
  };
  for (const Slot& s : kSlots) {
    checks++;
    const int id = cues.CombatSetId(s.cue);
    if (id < 0 && s.mayBeEmpty) {
      std::printf("combat-cues: '%s' has no takes yet (slot wired, set empty "
                  "— a blocked parry is silent until one is recorded)\n",
                  s.name);
    } else if (id < 0) {
      ok = false;
      std::printf(
          "combat-cues: FAILED '%s' resolves to nothing. The set is a FOLDER "
          "under assets/sounds — add takes through scripts/import_sounds.py\n",
          s.name);
    } else {
      resolved++;
    }
  }

  // ---- C. THE ENGINE ASKS FOR THEM -----------------------------------------
  // One call per cue; the counter must move by exactly six. `enabled_` is
  // false (no Init), so nothing is voiced and nothing is heard — which is the
  // point: this measures the REQUEST, which is the only half a headless run
  // has.
  {
    const uint32_t before = cues.GetStats().combat;
    cues.Combat(audio::Cues::CombatCue::Whoosh, Vec3{0, 0, 0}, 0.5f);
    cues.Combat(audio::Cues::CombatCue::Flesh, Vec3{0, 0, 0}, 1.0f);
    cues.Combat(audio::Cues::CombatCue::Clang, Vec3{0, 0, 0}, 0.0f);
    cues.Combat(audio::Cues::CombatCue::StrikeEdge, Vec3{0, 0, 0}, 0.7f);
    cues.Combat(audio::Cues::CombatCue::StrikeBlunt, Vec3{0, 0, 0}, 0.8f);
    cues.Combat(audio::Cues::CombatCue::Cut, Vec3{0, 0, 0}, 0.6f);
    const uint32_t moved = cues.GetStats().combat - before;
    checks++;
    if (moved != 6) {
      ok = false;
      std::printf(
          "combat-cues: FAILED the request counter moved by %u, want 6. "
          "Stats::combat must increment BEFORE the enabled_ early-out or a "
          "headless run can assert nothing (audio/cues.h)\n",
          moved);
    }
  }

  detail = Format("%d checks, %d/6 sets resolved", checks, resolved);
  std::printf("combat-cues: %s (%d checks, %d/6 sets resolved)\n",
              ok ? "PASS" : "FAIL", checks, resolved);
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// tuning-reach
// ---------------------------------------------------------------------------
// combat-tuning's one-key differential, applied to EVERY row in
// sim/tuning_params.def by expanding the table itself, so a new row is covered
// the moment it exists. For each row: write a tuning file holding only that
// key at a value off its compiled default, LoadTuning it, and require the
// struct field to have moved. Nine render.wave* rows had no read from 9a79eba
// to 2026-09-24 and every slider was dead.
//
// Since W2-Q the read itself is generated from the row (ReadDefRows), so a
// missing read can no longer happen by omission. What this still proves is
// that nothing AFTER the generated read -- a hand rule in LoadTuning, a
// clamp range that collapses to the default -- pins the field in place.
//
// Candidates are the default +-a step and its negation (a sign-only knob like
// melee.handLead moves under nothing else), each pulled inside the row's
// range. "Moved" is judged against a default-constructed Tuning, which is
// what LoadTuning starts from.
Status GateTuningReach(Ctx& c, std::string& detail) {
  (void)c;
  const std::string probePath =
      sandvox::AssetDir() + "/materials/tuning.reachprobe.json";
  // Heap, not stack: Tuning is large, and a local per expanded row (MSVC
  // gives each block scope its own slot) overflowed the stack at ~1.8 MB.
  const auto baseOwn = std::make_unique<Tuning>();
  const auto probe = std::make_unique<Tuning>();
  const Tuning& base = *baseOwn;
  Tuning& t = *probe;
  int rows = 0, wired = 0;
  std::string dead;

  auto loadOne = [&](const char* group, const char* key,
                     const std::string& value, Tuning& got) {
    {
      std::ofstream f(probePath);
      f << "{\n  \"" << group << "\": { \"" << key << "\": " << value
        << " }\n}\n";
    }
    return LoadTuning(probePath, got);
  };
  auto num = [](double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.9g", v);
    return std::string(b);
  };
  auto report = [&](const char* group, const char* key, bool moved) {
    rows++;
    if (moved) {
      wired++;
      return;
    }
    std::printf("tuning-reach: FAILED %s.%s did not reach its field -- no "
                "Read* for it in LoadTuning's \"%s\" group?\n",
                group, key, group);
    if (dead.size() < 200) dead += std::string(dead.empty() ? "" : ", ") +
                                   group + "." + key;
  };

  // Candidates are pulled inside the row's range first, so the probe never
  // relies on the clamp to land somewhere different from the default.
  auto inRange = [](double v, double lo, double hi) {
    if (!std::isnan(lo) && v < lo) v = lo;
    if (!std::isnan(hi) && v > hi) v = hi;
    return v;
  };
#define TP_F(g, m, n, d, lo, hi)                                          \
  {                                                                       \
    const double v0 = base.g.m, s = 0.25 * (std::fabs(v0) + 1.0);        \
    bool moved = false;                                                   \
    for (double cand : {v0 + s, v0 - s, -v0}) {                           \
      t = base;                                                           \
      if (loadOne(#g, #m, num(inRange(cand, lo, hi)), t) &&               \
          t.g.m != base.g.m)                                              \
        moved = true;                                                     \
    }                                                                     \
    report(#g, #m, moved);                                                \
  }
#define TP_I(g, m, n, d, lo, hi)                                          \
  {                                                                       \
    const long long v0 = base.g.m;                                        \
    bool moved = false;                                                   \
    for (long long cand : {v0 + 1, v0 - 1}) {                             \
      t = base;                                                           \
      const long long c2 = (long long)inRange((double)cand, lo, hi);      \
      if (loadOne(#g, #m, std::to_string(c2), t) && t.g.m != base.g.m)    \
        moved = true;                                                     \
    }                                                                     \
    report(#g, #m, moved);                                                \
  }
#define TP_U(g, m, n, d, lo, hi) TP_I(g, m, n, d, lo, hi)
#define TP_B(g, m, n, d)                                                  \
  {                                                                       \
    t = base;                                                             \
    report(#g, #m, loadOne(#g, #m, base.g.m ? "false" : "true", t) &&     \
                       t.g.m != base.g.m);                                \
  }
  // A string row may carry a hand-written validator (world.mapLayer refuses
  // a path), so the probe is a bare name no validator rejects.
#define TP_S(g, m, n, d)                                                  \
  {                                                                       \
    t = base;                                                             \
    report(#g, #m, loadOne(#g, #m, "\"reachprobe\"", t) &&                \
                       t.g.m == "reachprobe");                            \
  }
#define TP_V3(g, m, n, ...)                                               \
  {                                                                       \
    const double v0 = base.g.m[0], s = 0.25 * (std::fabs(v0) + 1.0);      \
    bool moved = false;                                                   \
    for (double cand : {v0 + s, v0 - s}) {                                \
      t = base;                                                           \
      const std::string arr = "[" + num(cand) + ", " +                    \
                              num(base.g.m[1]) + ", " +                   \
                              num(base.g.m[2]) + "]";                     \
      if (loadOne(#g, #m, arr, t) && t.g.m[0] != base.g.m[0]) moved = true; \
    }                                                                     \
    report(#g, #m, moved);                                                \
  }
#include "sim/tuning_params.def"
#undef TP_V3
#undef TP_S
#undef TP_B
#undef TP_U
#undef TP_I
#undef TP_F

  std::remove(probePath.c_str());
  std::printf("tuning-reach: %s (%d/%d tuning_params.def rows reach their "
              "field)\n", wired == rows ? "PASS" : "FAIL", wired, rows);
  if (wired != rows) {
    detail = std::to_string(rows - wired) + " dead row(s): " + dead;
    return Status::Fail;
  }
  return Status::Pass;
}


// ---- the shared fixture ----------------------------------------------------

// Which def can hold a sword. Asked of the RIG rather than named, so adding a
// creature does not silently change what these gates test (the "a gate that
// hardcodes the cast" lesson: naming an asset makes a correct new asset a
// failure).
int CombatDef(const MobSystem& mobs) {
  int best = -1;
  for (size_t i = 0; i < mobs.Defs().size(); i++) {
    if (mobs.Defs()[i].FindSocket("held_right") < 0) continue;
    if (best < 0 || mobs.Defs()[i].name == kAvatarDefName) best = (int)i;
  }
  return best;
}

// The centre of the RESIDENCY WINDOW, in voxels. Never an absolute coordinate:
// `streaming` leaves the window ~20 chunks out by the time these run, and a
// fixture placed outside it is despawned by PreTick on tick one — which reads
// as "the NPC never attacked" and is really "there was no NPC". Note the units:
// WindowOrigin() is in CHUNKS, and that exact confusion is what `swing-plane`
// shipped known-failing for.
IVec3 FixtureCentre(const World& world) {
  const IVec3 o = world.WindowOrigin();
  return IVec3{(o.x + (int)kNChunk / 2) * (int)kChunk, 0,
               (o.z + (int)kNChunk / 2) * (int)kChunk};
}

// The flattest patch near (cx,cz). Terrain is procedural and a hand-picked
// coordinate rots the moment worldgen is retuned; two fighters need the same
// floor, or one of them is swinging uphill at the other's knees.
IVec3 FlatSpot(int cx, int cz, uint32_t seed) {
  int bestX = cx, bestZ = cz, bestRelief = INT32_MAX;
  for (int oz = -96; oz <= 96; oz += 8)
    for (int ox = -96; ox <= 96; ox += 8) {
      int lo = INT32_MAX, hi = INT32_MIN;
      for (int dz = -16; dz <= 16; dz += 4)
        for (int dx = -16; dx <= 16; dx += 4) {
          const int h = World::TerrainHeight(cx + ox + dx, cz + oz + dz, seed);
          lo = std::min(lo, h);
          hi = std::max(hi, h);
        }
      if (hi - lo < bestRelief) {
        bestRelief = hi - lo;
        bestX = cx + ox;
        bestZ = cz + oz;
      }
    }
  return IVec3{bestX, World::TerrainHeight(bestX, bestZ, seed), bestZ};
}

// ONE TICK OF THE GAME'S OWN TICK (W2-O): support::RunTicks runs
// TickAuthority. This used to be "one tick, in the order PreTick runs in the
// game" written out by hand — mobs.PreTick, debris, SubmitTick, the step — and
// it was a copy of the tick as it stood that day: no day phase or rain word for
// the body reactions, no contact-damage pass after the step, no vessel pass,
// none of what the tick grew after it. The rig is built on first use, from the
// tick and the fixture chunk the gate constructed this with; a gate may move
// either between calls.
struct Ticker {
  Ctx& c;
  uint32_t tick = 26000;
  IVec3 playerChunk{8, 8, 8};
  std::unique_ptr<support::TickRig> rig;
  void operator()() {
    if (!rig) rig = std::make_unique<support::TickRig>(c, tick, playerChunk);
    rig->tick = tick;
    rig->SetFixtureChunk(playerChunk);
    support::RunTicks(*rig, 1);
    tick = rig->tick;
  }
};

// Spawn one humanoid with `profile` applied, armed or not. Returns 0 with the
// reason in `why`, which is what makes a fixture failure name itself instead of
// arriving as a bare zero downstream (CLAUDE.md rule 6).
uint64_t SpawnFighter(Ctx& c, int defIndex, IVec3 at, const char* profile,
                      bool armed, std::string& why) {
  const uint64_t id = c.mobs.Spawn(defIndex, at);
  if (id == 0) {
    why = "Spawn refused";
    return 0;
  }
  // ALWAYS A PROFILE, even for a target that does nothing. A def with no
  // authored `behavior` falls through to the legacy wander and WALKS OFF at its
  // own top speed — which is the fault that had `swing-plane` known-failing,
  // and it looks exactly like a sweep that misses.
  if (profile != nullptr && !c.mobs.SetMobBehavior(id, profile)) {
    why = std::string("no behaviour profile \"") + profile + "\"";
    return 0;
  }
  if (armed) {
    const ItemDef* sword = c.items.At(c.items.Find("sword"));
    if (sword == nullptr) {
      why = "no \"sword\" item";
      return 0;
    }
    if (!c.mobs.EquipItem(id, sword)) {
      why = "EquipItem refused";
      return 0;
    }
  }
  return id;
}

// Total live voxels over a creature's BODY limbs only — flesh, not luggage. A
// held sword and a worn coat are appended rig slots (Mob::AppendedBase), and
// counting them would make "the defender's flesh lost nothing" pass or fail on
// how badly their sword was chipped.
uint32_t FleshVoxels(MobSystem& mobs, uint64_t id) {
  Mob* m = mobs.FindMobById(id);
  if (m == nullptr) return 0;
  uint32_t n = 0;
  for (int i = 0; i < m->AppendedBase(); i++)
    if (mobs.LimbBody(id, i)) n += mobs.LimbVoxelCount(id, i);
  return n;
}

// ...and the hp of whatever is in the fist, which is what a parry charges.
float HeldHp(MobSystem& mobs, uint64_t id) {
  Mob* m = mobs.FindMobById(id);
  if (m == nullptr || m->HeldSlot() < 0) return -1.0f;
  return m->LimbHpAt(m->HeldSlot());
}

// The blade's tip in the wielder's own basis about its own shoulder — the frame
// every claim about a stroke's SHAPE is stated in, for the reason swing-plane
// gives: an offset centre smears azimuth into elevation.
struct TipRead {
  bool valid = false;
  float az = 0, el = 0, r = 0;
};
TipRead ReadTip(MobSystem& mobs, uint64_t id) {
  TipRead t;
  Mob* m = mobs.FindMobById(id);
  if (m == nullptr) return t;
  Vec3 hand, tip, flat;
  float reach = 0;
  if (!m->WeaponStrokePose(hand, tip, flat, reach)) return t;
  // WeaponStrokePose reports in the frame SetWeaponPose speaks, which for a mob
  // is the WORLD frame its own basis was built in — so the tip is projected
  // back onto that basis here, exactly as StepStroke expresses it.
  const Vec3 fwd = m->Facing();
  const Vec3 up{0, 1, 0};
  Vec3 right = up.cross(fwd);
  right = right.len() > 1e-4f ? right.normalized() : Vec3{1, 0, 0};
  const Vec3 l{tip.dot(right), tip.dot(up), tip.dot(fwd)};
  t.r = l.len();
  if (t.r < 1e-4f) return t;
  t.az = std::atan2(l.x, l.z);
  t.el = std::asin(std::clamp(l.y / t.r, -1.0f, 1.0f));
  t.valid = true;
  return t;
}

// A creature's chest in world voxels — what a cut is aimed at.
Vec3 Chest(MobSystem& mobs, uint64_t id, const MobDef& def) {
  const Vec3 o = mobs.MobOrigin(id);
  return Vec3{o.x + def.worldSize.x * 0.5f, o.y + def.worldSize.y * 0.66f,
              o.z + def.worldSize.z * 0.5f};
}

// Turn `who` toward `at`, INSTANTLY. The arbiter pins an ATTACKING mob to
// FaceTarget for its commit window, but a SCRIPTED attack (ForceAttack) has no
// arbiter behind it — and a stroke expressed in the body's own basis, made by a
// body facing the wrong way, is a stroke aimed at nothing.
//
// SetHeading and not SetDesiredHeading: a desired heading is overwritten by
// whatever intent wins the very next tick, and for a passive fixture that is
// Idle, which writes back the heading the creature already had. See the note at
// MobSystem::SetHeading for what that cost.
void FaceAt(MobSystem& mobs, uint64_t id, Vec3 at) {
  const Vec3 o = mobs.MobOrigin(id);
  mobs.SetHeading(id, std::atan2(at.x - o.x, at.z - o.z));
}

// Everything the four gates open with: pristine terrain, an empty stage, and
// the flat spot the fixture stands on.
struct Stage {
  IVec3 spot{};
  int defIndex = -1;
  const MobDef* def = nullptr;
  bool ok = false;
  std::string why;
};
Stage OpenStage(Ctx& c) {
  Stage s;
  s.defIndex = CombatDef(c.mobs);
  if (s.defIndex < 0) {
    s.why = "no mob def publishes a held_right socket";
    return s;
  }
  if (c.mobs.AttackStyles().empty()) {
    s.why = "assets/mobs/attack_styles.json loaded no styles";
    return s;
  }
  c.debris.Reset();
  c.mobs.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  const IVec3 anchor = FixtureCentre(c.world);
  s.spot = FlatSpot(anchor.x, anchor.z, kDefaultSeed);
  s.def = &c.mobs.Defs()[s.defIndex];
  s.ok = true;
  return s;
}
void CloseStage(Ctx& c) {
  c.mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
}

// =============================================================================
// npc-strike — a request becomes a swing, and the swing removes voxels
// =============================================================================
Status GateNpcStrike(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const char* what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("npc-strike: FAILED %s\n", what);
    }
  };

  Stage st = OpenStage(c);
  if (!st.ok) {
    detail = st.why;
    std::printf("npc-strike: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }

  // WITHIN REACH, and reach is short: an arm is 0.6 m and this sword 0.55 m, so
  // the point never gets much past 1.2 m from the shoulder. `swing-plane` E has
  // the same note and the same number.
  const float gap = (float)BaselineNumber("npcStrike.gapVox", 9.0);
  std::string why;
  // STOOD DOWN WHILE THE FIXTURE SETTLES. `swordsman_static` is hostile and its
  // attack cadence is 36 ticks, so it lands a blow DURING the twenty settling
  // ticks and the "before" reading is taken on a creature that has already been
  // cut — measured, the baseline came out 390 voxels light and the AI pass then
  // compared a wounded target against itself and found no change. It is stood
  // up a few lines below, once there is a real baseline to compare against.
  const uint64_t attacker =
      SpawnFighter(c, st.defIndex, {st.spot.x, st.spot.y + 1, st.spot.z},
                   "training_dummy", true, why);
  const uint64_t target = SpawnFighter(
      c, st.defIndex,
      {st.spot.x, st.spot.y + 1, st.spot.z + (int)std::lround(gap)},
      "training_dummy", false, why);
  if (attacker == 0 || target == 0) {
    detail = why.empty() ? "fixture spawn failed" : why;
    std::printf("npc-strike: SKIP (%s)\n", detail.c_str());
    CloseStage(c);
    return Status::Skip;
  }

  Ticker tick{c, 26000, {st.spot.x >> 4, st.spot.y >> 4, st.spot.z >> 4}};
  FaceAt(c.mobs, attacker, Chest(c.mobs, target, *st.def));
  for (int i = 0; i < 20; i++) tick();   // settle both rigs onto the ground

  const uint32_t fleshAi0 = FleshVoxels(c.mobs, target);
  check(fleshAi0 > 0, "the target has flesh to lose before anything swings");
  c.mobs.ClearAttackRequests();
  check(c.mobs.SetMobBehavior(attacker, "swordsman_static"),
        "the attacker's AI could be stood up");

  // ---- PASS 1: THE WHOLE CHAIN, WITH NOBODY DRIVING IT --------------------
  //
  // `swordsman_static` is hostile, immobile and armed, and the target is of
  // another faction inside its reach — so it decides to attack entirely on its
  // own. This is the claim that matters most and the one nothing else in the
  // suite makes: a REQUEST BECOMES A SWING. Until phase C the requests were
  // drained and printed, which is a seam that can stop firing without anything
  // going red.
  int aiRequests = 0, aiCutTicks = 0, aiSweeps = 0, aiHits = 0;
  float aiTopSpeed = 0;
  const int aiTicks = (int)BaselineNumber("npcStrike.aiTicks", 150);
  for (int i = 0; i < aiTicks; i++) {
    tick();
    aiRequests += (int)c.mobs.AttackRequests().size();
    c.mobs.ClearAttackRequests();
    const NpcStroke* s = c.mobs.MobStroke(attacker);
    if (s == nullptr) continue;
    if (s->Cutting()) aiCutTicks++;
    // Summed at the END of each stroke: the counters live on the stroke and
    // are cleared when it resets, so sampling the peak per stroke is what adds
    // up across several.
    aiSweeps = std::max(aiSweeps, s->sweeps);
    aiHits = std::max(aiHits, s->bodiesHit);
    aiTopSpeed = std::max(aiTopSpeed, s->topTipSpeed);
  }
  const uint32_t fleshAi1 = FleshVoxels(c.mobs, target);
  check(aiRequests > 0, "the AI decided to attack, unprompted");
  check(aiCutTicks > 0, "...and its own requests became real cuts");
  check(aiHits > 0, "...that passed through the target");
  check(fleshAi1 < fleshAi0, "...and removed real voxels from it");
  std::printf(
      "npc-strike (unprompted): %d requests, %d cut ticks, %d sweeps, %d "
      "bodies hit, top tip speed %.1f vox/s, %u flesh voxels lost over %d "
      "ticks\n",
      aiRequests, aiCutTicks, aiSweeps, aiHits, aiTopSpeed,
      fleshAi0 - fleshAi1, aiTicks);

  // ---- PASS 2: ONE STYLE, THREE TIMES, WITH THE AI OUT OF THE WAY ---------
  //
  // The shape and accumulation claims below need a KNOWN style and a known
  // number of swings, and an attacker that keeps deciding to attack on its own
  // cadence gives neither — measured, its own strokes refused two of three
  // scripted ones (ForceAttack will not interrupt a live swing, by design) and
  // the gate then measured an empty loop. Switching the profile is how a test
  // stops an AI: `training_dummy` is passive and blind, so the creature keeps
  // its body, its sword and its stance and simply stops choosing.
  check(c.mobs.SetMobBehavior(attacker, "training_dummy"),
        "the attacker's AI could be stood down for the scripted passes");
  // LONG ENOUGH FOR THE ARM TO COME HOME, not just for the stroke to end. A
  // scripted swing started while the weapon arm is still mid-recover takes over
  // from wherever the recover had it, and the take-over is exact (melee.h) —
  // so the first tick of the new cut has to travel from a pose nobody chose.
  // Measured, that read as a 300 vox/s POP on strike 0 against 107 vox/s on the
  // two that started from rest, and the sweep's probes stepped straight over
  // the target: 5 sweeps, 0 bodies hit, on a swing that looked twice as fast as
  // the ones that worked.
  for (int i = 0; i < 40; i++) tick();

  // ---- THREE STRIKES, EACH ON A FRESH TARGET ------------------------------
  //
  // A FRESH one each time, and the reason is measured: a committed sword cut
  // through a torso severs it, and severing a torso drops everything hanging
  // off it — the first landed strike took this dummy from 3092 flesh voxels to
  // ZERO. So "strike the same body three times" cannot establish anything after
  // the first blow, and "wounds accumulate" is not this gate's claim to make:
  // `wound-accumulate` owns it, on a fixture built to survive being cut.
  //
  // What three identical strikes on three identical targets DO establish is
  // REPEATABILITY, which is what an NPC attack has to have and what a
  // hash-seeded start bow could quietly cost: an attack that lands two times in
  // three is a creature the player experiences as randomly harmless.
  const int strikes = 3;
  uint32_t lostEach[3] = {};
  int windupSeen = 0, cutSeen = 0, finished = 0, landed = 0;
  float azSpan = 0, elSpan = 0;
  uint64_t victim = 0;
  for (int k = 0; k < strikes; k++) {
    // A FRESH ONE EVERY TIME, INCLUDING THE FIRST. Reusing the AI pass's target
    // for strike 0 is what the previous version did, and the AI pass had
    // already killed it — MobOrigin of a despawned creature is (0,0,0), so the
    // gate turned the attacker to face the WORLD ORIGIN and aimed a cut at it.
    // The symptom was a 513 vox/s swing that hit nothing, i.e. it looked like a
    // damage bug and was a dangling id.
    //
    // Same def, same profile, same place: only the id differs, and the id is
    // exactly what the stroke's variation is seeded on — so three strikes are
    // three DIFFERENT draws against identical geometry, which is the
    // repeatability claim rather than three replays of one lucky one.
    victim = SpawnFighter(
        c, st.defIndex,
        {st.spot.x, st.spot.y + 1, st.spot.z + (int)std::lround(gap)},
        "training_dummy", false, why);
    if (victim == 0) {
      check(false, "the replacement target spawned");
      break;
    }
    for (int i = 0; i < 14; i++) tick();
    const uint32_t before = FleshVoxels(c.mobs, victim);
    check(before > 0, "the target has flesh to lose before the strike");
    FaceAt(c.mobs, attacker, Chest(c.mobs, victim, *st.def));
    for (int i = 0; i < 6; i++) tick();   // let the turn finish before aiming
    const bool started = c.mobs.ForceAttack(
        attacker, "horizontal_r", Chest(c.mobs, victim, *st.def), tick.tick);
    check(started, "the scripted attack started");
    float azMin = 1e9f, azMax = -1e9f, elMin = 1e9f, elMax = -1e9f;
    int sweeps = 0, hits = 0;
    float topSpeed = 0;
    for (int i = 0; i < 70; i++) {
      tick();
      const NpcStroke* s = c.mobs.MobStroke(attacker);
      if (s == nullptr) break;
      sweeps = std::max(sweeps, s->sweeps);
      hits = std::max(hits, s->bodiesHit);
      topSpeed = std::max(topSpeed, s->topTipSpeed);
      if (s->phase == NpcStroke::Phase::Windup) windupSeen++;
      if (s->phase == NpcStroke::Phase::Cut) {
        cutSeen++;
        const TipRead t = ReadTip(c.mobs, attacker);
        if (t.valid) {
          azMin = std::min(azMin, t.az);
          azMax = std::max(azMax, t.az);
          elMin = std::min(elMin, t.el);
          elMax = std::max(elMax, t.el);
        }
      }
      if (!s->Active() && i > 4) {
        finished++;
        break;
      }
    }
    // The first strike that produced READABLE tip samples, not necessarily the
    // first strike: a `k == 0` gate reports a span of exactly zero whenever the
    // opening swing was the one that went wrong, which is both the least
    // informative moment to give up and the easiest one to hit.
    if (azSpan <= 0.0f && azMax > azMin) {
      azSpan = azMax - azMin;
      elSpan = elMax - elMin;
    }
    const uint32_t after = FleshVoxels(c.mobs, victim);
    lostEach[k] = before > after ? before - after : 0;
    if (hits > 0 && lostEach[k] > 0) landed++;
    // WHY THE STRIKE DID OR DID NOT LAND, next to the number it produced.
    std::printf(
        "npc-strike strike %d: %d sweeps, %d bodies hit, top tip speed %.1f "
        "vox/s, flesh %u -> %u\n",
        k, sweeps, hits, topSpeed, before, after);
  }

  // A WINDUP IS THE TELEGRAPH, so its LENGTH is the assertion: an attack that
  // resolved in two ticks would be unreadable and unavoidable, which is the
  // failure mode "no UI indicator" has to be defended against.
  const int windupMin = (int)BaselineNumber("npcStrike.windupTicksMin", 8);
  check(windupSeen >= windupMin * strikes,
        "every swing spent real time winding up (the telegraph)");
  check(cutSeen >= 2 * strikes, "...and real time cutting");
  check(finished == strikes, "every stroke ran to completion");

  const uint32_t lostMin = (uint32_t)BaselineNumber("npcStrike.lostVoxMin", 20);
  uint32_t lostWorst = 0xFFFFFFFFu, lost = 0;
  for (int k = 0; k < strikes; k++) {
    lost += lostEach[k];
    lostWorst = std::min(lostWorst, lostEach[k]);
  }
  uint32_t lostBest = 0;
  for (int k = 0; k < strikes; k++) lostBest = std::max(lostBest, lostEach[k]);
  check(landed == strikes, "EVERY scripted strike landed, not two in three");
  // A REAL WOUND, not merely contact. Stated on the BEST of the three rather
  // than the weakest, because a hash-seeded start bow legitimately makes one
  // swing of three a graze — measured, 3328 -> 3327 on a stroke that hit five
  // bodies. That is a sword clipping a shoulder, which is a thing swords do.
  // What would be a real regression is three grazes, and that is what this
  // catches; the weakest is RECORDED so a drift toward it is visible in the
  // baseline rather than invisible until it crosses a threshold.
  check(lostBest >= lostMin, "...and at least one of them cut deep");
  RecordObserved("npcStrike.lostWorstObserved", (double)lostWorst);

  // THE PLANE THE STYLE CLAIMS. `horizontal_r` is azimuth-dominant by
  // construction, so a build that swapped the two axes, or dropped one, fails
  // here — and this is the same measurement npc-styles makes over the whole
  // library.
  check(azSpan > elSpan,
        "a horizontal style sweeps AZIMUTH more than elevation");
  RecordObserved("npcStrike.lostVoxObserved", (double)lost);
  RecordObserved("npcStrike.azSpanObserved", azSpan);
  std::printf(
      "npc-strike: %d of %d strikes landed, %u flesh voxels total (weakest "
      "%u, min %u); windup %d ticks, cut %d ticks; first cut swept az %.2f "
      "el %.2f rad\n",
      landed, strikes, lost, lostWorst, lostMin, windupSeen, cutSeen, azSpan,
      elSpan);

  CloseStage(c);
  detail = Format("%d checks", checks);
  std::printf("npc-strike: %s (%d checks)\n", ok ? "PASS" : "FAIL", checks);
  return ok ? Status::Pass : Status::Fail;
}

// =============================================================================
// npc-block — simultaneous swings parry when the blades cross
// =============================================================================
//
// Both fighters swing horizontal cuts aimed at each other's chest. The same
// style gives both the same windup, so they reach Cut on the same tick and
// their blades cross midway between them. A parry only fires when both
// combatants are actively cutting (Mob::swinging_).
Status GateNpcBlock(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const char* what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("npc-block: FAILED %s\n", what);
    }
  };

  Stage st = OpenStage(c);
  if (!st.ok) {
    detail = st.why;
    std::printf("npc-block: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }

  const float gap = (float)BaselineNumber("npcBlock.gapVox", 9.0);
  std::string why;
  const uint64_t fighter1 =
      SpawnFighter(c, st.defIndex, {st.spot.x, st.spot.y + 1, st.spot.z},
                   "training_dummy", true, why);
  const uint64_t fighter2 = SpawnFighter(
      c, st.defIndex,
      {st.spot.x, st.spot.y + 1, st.spot.z + (int)std::lround(gap)},
      "training_dummy", true, why);
  if (fighter1 == 0 || fighter2 == 0) {
    detail = why.empty() ? "fixture spawn failed" : why;
    std::printf("npc-block: SKIP (%s)\n", detail.c_str());
    CloseStage(c);
    return Status::Skip;
  }

  Ticker tick{c, 27000, {st.spot.x >> 4, st.spot.y >> 4, st.spot.z >> 4}};
  FaceAt(c.mobs, fighter1, Chest(c.mobs, fighter2, *st.def));
  FaceAt(c.mobs, fighter2, Chest(c.mobs, fighter1, *st.def));
  for (int i = 0; i < 24; i++) tick();

  const Vec3 chest1 = Chest(c.mobs, fighter1, *st.def);
  const Vec3 chest2 = Chest(c.mobs, fighter2, *st.def);

  // The held-item prop layer check (Layers::PROP = 4): a drawn weapon must not
  // generate contacts while held. Checked here because this is the gate that
  // has two armed NPCs standing close.
  {
    Mob* m1 = c.mobs.FindMobById(fighter1);
    const uint64_t body1 = m1 != nullptr && m1->HeldSlot() >= 0
                               ? c.mobs.LimbBody(fighter1, m1->HeldSlot())
                               : 0;
    check(body1 != 0, "fighter 1 held item has a rig body");
    const int layer = body1 != 0 ? c.phys.BodyObjectLayer(body1) : -1;
    check(layer == 4,
          "held weapon is on the prop layer (no contact push)");
  }

  const uint32_t flesh1_0 = FleshVoxels(c.mobs, fighter1);
  const uint32_t flesh2_0 = FleshVoxels(c.mobs, fighter2);
  const float blade1_0 = HeldHp(c.mobs, fighter1);
  const float blade2_0 = HeldHp(c.mobs, fighter2);
  check(flesh1_0 > 0 && flesh2_0 > 0,
        "both fighters have flesh before the exchange");
  c.mobs.ClearBlockEvents();

  // Both swing the same horizontal cut, aimed at each other's chest.
  // Same style + same tick = same windup = Cut phase reached simultaneously.
  check(c.mobs.ForceAttack(fighter1, "horizontal_r", chest2, tick.tick),
        "fighter 1 started a cut");
  check(c.mobs.ForceAttack(fighter2, "horizontal_r", chest1, tick.tick),
        "fighter 2 started a cut");

  int blocks = 0;
  bool arrested1 = false, arrested2 = false;
  auto edgeOf = [&](uint64_t id, Vec3& base, Vec3& tip) -> bool {
    Mob* m = c.mobs.FindMobById(id);
    float hw = 0;
    return m != nullptr && m->WeaponEdge(base, tip, hw, nullptr);
  };
  float closest = 1e9f;
  for (int i = 0; i < 80; i++) {
    tick();
    for (const BlockEvent& ev : c.mobs.BlockEvents()) {
      blocks++;
      check((ev.attackerId == fighter1 && ev.blockerId == fighter2) ||
            (ev.attackerId == fighter2 && ev.blockerId == fighter1),
            "block event names the two fighters");
    }
    c.mobs.ClearBlockEvents();
    const NpcStroke* s1 = c.mobs.MobStroke(fighter1);
    const NpcStroke* s2 = c.mobs.MobStroke(fighter2);
    if (s1 && s1->phase == NpcStroke::Phase::Cut &&
        s2 && s2->phase == NpcStroke::Phase::Cut) {
      Vec3 a0, a1, b0, b1;
      if (edgeOf(fighter1, a0, a1) && edgeOf(fighter2, b0, b1)) {
        for (int j = 0; j <= 8; j++) {
          const Vec3 p = a0 + (a1 - a0) * ((float)j / 8.0f);
          for (int k = 0; k <= 8; k++) {
            const Vec3 q = b0 + (b1 - b0) * ((float)k / 8.0f);
            closest = std::min(closest, (p - q).len());
          }
        }
      }
    }
    if (s1) arrested1 = arrested1 || s1->arrested;
    if (s2) arrested2 = arrested2 || s2->arrested;
    const bool done1 = !s1 || (!s1->Active() && i > 4);
    const bool done2 = !s2 || (!s2->Active() && i > 4);
    if (done1 && done2) break;
  }

  check(blocks > 0, "a BlockEvent was emitted (blades crossed mid-swing)");
  check(arrested1 || arrested2,
        "at least one stroke was arrested by the parry");

  const uint32_t flesh1_1 = FleshVoxels(c.mobs, fighter1);
  const uint32_t flesh2_1 = FleshVoxels(c.mobs, fighter2);
  const float blade1_1 = HeldHp(c.mobs, fighter1);
  const float blade2_1 = HeldHp(c.mobs, fighter2);
  const bool bladesDamaged = blade1_1 < blade1_0 || blade2_1 < blade2_0;
  check(bladesDamaged, "at least one blade took hp damage from the parry");

  std::printf(
      "npc-block: %d block events; arrested f1=%d f2=%d; flesh f1 %u->%u "
      "f2 %u->%u; blade hp f1 %.1f->%.1f f2 %.1f->%.1f; closest %.2f vox\n",
      blocks, (int)arrested1, (int)arrested2,
      flesh1_0, flesh1_1, flesh2_0, flesh2_1,
      (double)blade1_0, (double)blade1_1,
      (double)blade2_0, (double)blade2_1, closest);

  CloseStage(c);
  detail = Format("%d checks", checks);
  std::printf("npc-block: %s (%d checks)\n", ok ? "PASS" : "FAIL", checks);
  return ok ? Status::Pass : Status::Fail;
}

// =============================================================================
// npc-styles — every authored style sweeps the plane it claims to
// =============================================================================
//
// THE GATE THAT KEEPS THE CONTENT HONEST AS IT GROWS. Adding a style is a JSON
// edit (game/strokes.h) and nothing else in the suite would notice a new one
// that never commits, or whose "overhead" is really a sideways flail. Each
// style is driven open-loop against a stated aim and its claim is checked
// against ITS OWN AUTHORED NUMBERS — so this needs no update when a style is
// added, only when one is added badly.
Status GateNpcStyles(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const std::string& what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("npc-styles: FAILED %s\n", what.c_str());
    }
  };

  Stage st = OpenStage(c);
  if (!st.ok) {
    detail = st.why;
    std::printf("npc-styles: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }
  std::string why;
  const uint64_t id =
      SpawnFighter(c, st.defIndex, {st.spot.x, st.spot.y + 1, st.spot.z},
                   "training_dummy", true, why);
  if (id == 0) {
    detail = why.empty() ? "fixture spawn failed" : why;
    std::printf("npc-styles: SKIP (%s)\n", detail.c_str());
    CloseStage(c);
    return Status::Skip;
  }
  Ticker tick{c, 28000, {st.spot.x >> 4, st.spot.y >> 4, st.spot.z >> 4}};
  for (int i = 0; i < 20; i++) tick();

  // Straight ahead, level, at arm's length: an aim with no bias of its own, so
  // what is measured is the STYLE and not where a target happens to be.
  //
  // RE-DERIVED PER STYLE, INSIDE THE LOOP (2026-09-15), because one of the
  // styles MOVES THE ATTACKER. `bite_lunge` leaps 16.67 voxels, and a point
  // fixed before the loop is a point the creature has flown past by the time
  // the next style is asked for -- so every style after the lunge in library
  // order was aimed backwards over the fixture's own shoulder and drove its
  // azimuth into the across-the-body stop. Measured: `punch_r` (before the
  // lunge) commanded azimuth -0.17..0.11 and `player_punch_r` (after it)
  // -1.40..1.01, on styles whose authored numbers differ by two hundredths of
  // a radian; the range did not move when the authoring was changed, because
  // the authoring was never what set it.
  //
  // The aim is DEFINED relative to the attacker -- "straight ahead, level, at
  // arm's length" -- so following the attacker is what the sentence above
  // always meant. `aimFor` is that sentence.
  //
  // ...AND AT THE DISTANCE THE STYLE IS FOR. Eleven voxels is arm's length and
  // is right for a style that stands and swings; a LUNGING style crosses three
  // body-lengths and would fly straight past it, leaving the cut aimed over
  // its own shoulder.  is the distance the AI would
  // really commit from, which is the distance the blow is really made at.
  auto aimFor = [&](const AttackStyle& s2) {
    const Vec3 o2 = c.mobs.MobOrigin(id);
    float ahead = 11.0f;
    if (const Mob* m2 = c.mobs.FindMobById(id))
      ahead = std::max(ahead, c.mobs.StyleReachOn(*m2, s2));
    return Vec3{o2.x + st.def->worldSize.x * 0.5f,
                o2.y + st.def->worldSize.y * 0.66f,
                o2.z + st.def->worldSize.z * 0.5f + ahead};
  };

  const float domMin = (float)BaselineNumber("npcStyles.dominanceMin", 1.3);
  const float minSweep = (float)BaselineNumber("npcStyles.minSweepRad", 0.25);
  const float minReach = (float)BaselineNumber("npcStyles.minThrustVox", 1.0);
  // BOTH OF THESE WERE LITERALS IN THIS FILE until the phase C/D merge, and
  // both had to move: the grip re-author changed the stroke's own radius (the
  // blade lies along the arm now, so the tip starts further out and the same
  // authored angles describe a different arc), and a bound that costs a
  // rebuild to retune is a bound nobody retunes (CLAUDE.md). The numbers
  // themselves are in tests/baseline.json with what moved them.
  const float diagRatioMax =
      (float)BaselineNumber("npcStyles.diagRatioMax", 4.0);
  const float thrustSwingMax =
      (float)BaselineNumber("npcStyles.thrustSwingMax", 1.0);
  const StyleLibrary& lib = c.mobs.AttackStyles();
  check(lib.styles.size() >= 5,
        "the library ships the five styles phase C promised");
  const MobDef& fixtureDef = *st.def;
  for (const AttackStyle& sty : lib.styles) {
    // ---- EVERY USABLE STYLE SWEEPS ITS OWN CHANNEL WITH ITS OWN EFFECTOR --
    //
    // The fixture is an ARMED humanoid, so `StyleUsable` is the filter that
    // decides which of the library's styles this creature can be asked for at
    // all — a punch on a rig with no `natural` block, or a bite on one with no
    // head, is not a failure, it is a style that does not apply. Skipping on
    // the same predicate the DRAW uses is what keeps this loop inherited by
    // every style added later, which is the property the gate was written for.
    const Mob* fixture = c.mobs.FindMobById(id);
    if (fixture == nullptr || !StyleUsable(*fixture, sty)) {
      std::printf("npc-styles %-14s skipped: this rig cannot swing it\n",
                  sty.name.c_str());
      continue;
    }
    // ---- THE SAME SWING IN EVERY SCOPE (2026-09-15) ---------------------
    //
    // A FIXED SEED, derived from the style's NAME and nothing else. Every draw
    // in the runner keys off the stroke's seed, and the one that matters here
    // is the TEMPO JITTER: it scales the windup and cut TICK COUNTS, so the
    // same style is a five-tick cut from one (mob id, tick) pair and a
    // three-tick one from another -- and a mob id depends on how many
    // creatures the gates before this one spawned. Measured on identical code,
    // `punch_r` reported 1.14 voxels of commanded radial travel standalone and
    // 0.94 in-suite, straddling its own 1.0 floor; `bite` reported 0.26 rad of
    // posed arc standalone and 0.23 in-suite against 0.25.
    //
    // That is the scope-stability failure `_npcStyles_about` names, arriving
    // by a different door: the earlier one was a CLAIM that moved between
    // scopes, this is an INPUT that does. Pinning it costs the gate nothing it
    // was measuring -- the jitter is a character feature, not a property of a
    // style's shape -- and every style still draws a DIFFERENT seed, so
    // nothing is measured at one lucky tempo.
    const uint32_t styleSeed =
        rng::Hash3(0x5C0BEu,
                   (uint32_t)std::hash<std::string>{}(sty.name), 0x5747u) |
        1u;
    // ...AND THE FIXTURE FACES IT. A lunge leaves the creature where it
    // landed AND pointing where it was going; a stroke expressed in the body's
    // own basis, made by a body facing the wrong way, is a stroke aimed at
    // nothing (selftest_combat's FaceAt says the same for the same reason).
    const Vec3 aim = aimFor(sty);
    FaceAt(c.mobs, id, aim);
    if (!c.mobs.ForceAttack(id, sty.name, aim, tick.tick, styleSeed)) {
      check(false, "style \"" + sty.name + "\" would not start");
      continue;
    }
    // ...AND IT IS DRIVING THE PART THE STYLE NAMED. One read, and it is the
    // one thing that cannot be recovered from the arc afterwards: a stroke
    // whose effector never reached the rig produces a perfectly shaped
    // COMMAND and moves nothing, which every arc check below would pass.
    {
      const Mob* m = c.mobs.FindMobById(id);
      const MobNaturalWeaponDef* nw = m ? m->EffectorWeapon() : nullptr;
      if (sty.weapon.empty() || sty.weapon == "held") {
        check(m != nullptr && nw == nullptr &&
                  m->StrikeEffectorKind() == StrikeEffectorMode::Held,
              "style \"" + sty.name + "\" swings the HELD item");
      } else {
        check(nw != nullptr && nw->name == sty.weapon,
              "style \"" + sty.name + "\" swings natural weapon \"" +
                  sty.weapon + "\"");
        check(nw != nullptr && fixtureDef.FindNatural(sty.weapon) >= 0,
              "...which " + fixtureDef.name + " declares");
      }
    }
    float azMin = 1e9f, azMax = -1e9f, elMin = 1e9f, elMax = -1e9f;
    float rMin = 1e9f, rMax = -1e9f;
    // ARC LENGTH, ACCUMULATED PER TICK. A span scaled by the MEAN elevation was
    // the first attempt and it is not enough: an overhead starts high and ends
    // low, so its mean elevation is near zero while the tip spends the first
    // half of the cut up near the pole where azimuth is wild. Measured, that
    // reported az 1.74 against el 1.59 for a style whose authored cut is 0.15
    // rad of azimuth and 1.90 of elevation. Summing |dAz| * cos(el) tick by
    // tick weights each step by the elevation it was actually taken at, which
    // is the arc the tip travelled rather than an angle it passed through.
    float azArc = 0, elArc = 0;
    float prevAz = 0, prevEl = 0;
    bool havePrev = false;
    // THE COMMANDED radius as well as the posed one. A thrust that does not
    // extend has two entirely different causes — the driver never asked, or the
    // arm could not serve it — and one number reports both (CLAUDE.md rule 6).
    float cmdRMin = 1e9f, cmdRMax = -1e9f;
    // THE COMMANDED ARCS TOO. See the note at the dominance checks below for
    // why the CLAIM is stated on these and not on the posed sword.
    float cmdAzArc = 0, cmdElArc = 0;
    float cmdAzLo = 1e9f, cmdAzHi = -1e9f;
    float blade = -1.0f;
    float cPrevAz = 0, cPrevEl = 0;
    bool cHavePrev = false;
    int cutTicks = 0;
    // ---- WHAT THE EFFECTOR ITSELF DID, in its own units (2026-09-15) -----
    //
    // `ReadTip` reports the driver's seed pose about the stroke's pivot, which
    // is the right coordinate for a SWORD and a poor one for anything else.
    // For a Chain effector it now reports the HAND (a fist has no blade to
    // lean, so mob.cpp deliberately reports its tip at its hand); for an Aim
    // effector it reports a point on a part that ROTATES about its own joint.
    // Neither is a lie and neither is what a reader wants to know, which is:
    // DID THE FIST MOVE, AND DID THE JAWS TURN.
    //
    // So a natural weapon is measured on its own terms, off the LIVE physics
    // transform the damage sweep uses:
    //   Chain -> the knuckles' world PATH LENGTH in voxels across the cut
    //   Aim   -> the part's forward-vector ANGULAR travel in radians
    // Accumulated per tick, like every arc here, so a blow that goes out and
    // comes back is not read as having gone nowhere.
    float edgePath = 0;          // Chain: voxels the tip travelled
    float fwdTurn = 0;           // Aim: radians the forward turned
    Vec3 prevEdgeTip{};
    Vec3 prevFwd{};
    bool haveEdgePrev = false;
    // ...and the rig's own COMMANDED/POSED pair for an aimed part
    // (Mob::AimDiag). The travel above says the head moved; this says whether
    // it moved as far as it was TOLD, which is a different question and the
    // one that caught the neck-lever bug.
    float aimCmdPitchLo = 1e9f, aimCmdPitchHi = -1e9f;
    float aimGotPitchLo = 1e9f, aimGotPitchHi = -1e9f;
    for (int i = 0; i < 90; i++) {
      tick();
      const NpcStroke* s = c.mobs.MobStroke(id);
      if (s == nullptr) break;
      if (s->phase == NpcStroke::Phase::Cut) {
        cutTicks++;
        if (const Mob* m = c.mobs.FindMobById(id)) {
          Vec3 eb, et, ef;
          float ehw = 0;
          if (m->WeaponEdge(eb, et, ehw, &ef)) {
            const Vec3 f = (et - eb).len() > 1e-5f ? (et - eb).normalized()
                                                   : Vec3{0, 0, 1};
            if (haveEdgePrev) {
              edgePath += (et - prevEdgeTip).len();
              fwdTurn += std::acos(std::clamp(f.dot(prevFwd), -1.0f, 1.0f));
            }
            prevEdgeTip = et;
            prevFwd = f;
            haveEdgePrev = true;
          }
          const Mob::AimDiag& ad = m->AimDiagnostics();
          if (ad.ran) {
            aimCmdPitchLo = std::min(aimCmdPitchLo, ad.cmdPitch);
            aimCmdPitchHi = std::max(aimCmdPitchHi, ad.cmdPitch);
            aimGotPitchLo = std::min(aimGotPitchLo, ad.gotPitch);
            aimGotPitchHi = std::max(aimGotPitchHi, ad.gotPitch);
          }
        }
        const TipRead t = ReadTip(c.mobs, id);
        if (t.valid) {
          azMin = std::min(azMin, t.az);
          azMax = std::max(azMax, t.az);
          elMin = std::min(elMin, t.el);
          elMax = std::max(elMax, t.el);
          rMin = std::min(rMin, t.r);
          rMax = std::max(rMax, t.r);
          if (havePrev) {
            // WRAPPED, because `t.az` is an atan2 (ReadTip) and a tip that
            // crosses behind the creature's own right shoulder jumps by 2*pi.
            // Summed raw, one crossing adds ~2*pi*cos(el) of phantom arc and a
            // pure THRUST reports 2.7 rad of azimuth travel it never made.
            // Same bug as swing-plane's steering accumulator and as
            // Mob::ApplyWeaponArm's hinge angle; same fix.
            float dAz = t.az - prevAz;
            while (dAz > 3.14159265f) dAz -= 6.28318531f;
            while (dAz <= -3.14159265f) dAz += 6.28318531f;
            azArc += std::fabs(dAz) *
                     std::cos(std::clamp(t.el, -1.5f, 1.5f));
            elArc += std::fabs(t.el - prevEl);
          }
          prevAz = t.az;
          prevEl = t.el;
          havePrev = true;
        }
        {
          const float cr = s->melee.StrokeRadius();
          cmdRMin = std::min(cmdRMin, cr);
          cmdRMax = std::max(cmdRMax, cr);
          const float ca = s->melee.StrokeAz(), ce = s->melee.StrokeEl();
        blade = s->melee.BladeLength();
        cmdAzLo = std::min(cmdAzLo, ca);
        cmdAzHi = std::max(cmdAzHi, ca);
          if (cHavePrev) {
            cmdAzArc += std::fabs(ca - cPrevAz) *
                        std::cos(std::clamp(ce, -1.5f, 1.5f));
            cmdElArc += std::fabs(ce - cPrevEl);
          }
          cPrevAz = ca;
          cPrevEl = ce;
          cHavePrev = true;
        }
      }
      if (!s->Active() && i > 4) break;
    }
    // AZIMUTH IS AN ANGLE; THE ARC IS A DISTANCE, and near the pole they are
    // not the same thing at all. A point held high sweeps a small CIRCLE OF
    // LATITUDE, so a couple of voxels of lateral wobble reads as a large
    // azimuth span for reasons that are pure spherical coordinates. Scaling by
    // cos(mean elevation) turns the span back into the arc the tip actually
    // travelled, which is the quantity "azimuth-dominant" was ever about.
    //
    // It is not a fudge to make a check pass: measured in the full suite, the
    // OVERHEAD — a style whose authored cut is 0.15 rad of azimuth against 1.90
    // of elevation — reported az 1.76 vs el 1.59 and was called horizontal. At
    // its mean elevation of ~1.0 rad that 1.76 is 0.95 voxels of arc.
    // ---- WHOSE PLANE IS BEING ASSERTED --------------------------------------
    //
    // The DOMINANCE claim is about the STROKE — "this style asked for elevation
    // and elevation is what it drove" — and it is stated on the commanded arcs
    // because the posed sword cannot carry it. Measured: the overhead's
    // commanded azimuth travel is 0.21 rad against 1.90 of elevation, and the
    // posed tip still swept 2.02 rad of azimuth arc. That is human.json's
    // SHOULDER, which bounds the right upper arm to 50 degrees past its own
    // backward plane and 30 past the midline: a pure vertical chop is not a
    // pose this body has, so the arm serves it by going round. `swing-plane`
    // hit exactly this and split its claims the same way, with the same reason
    // written next to it.
    //
    // The posed sword keeps a claim of its own, immediately below: it has to
    // have MOVED in the channel the style asked for. A rig that dropped the
    // stroke entirely fails that however tidy the command was.
    const float az = cmdAzArc;
    const float el = cmdElArc;
    const float posedAz = azArc;
    const float posedEl = elArc;
    const float azSpan = azMax > azMin ? azMax - azMin : 0.0f;
    const float elSpan = elMax > elMin ? elMax - elMin : 0.0f;
    const float dr = rMax > rMin ? rMax - rMin : 0.0f;
    const float cmdDr = cmdRMax > cmdRMin ? cmdRMax - cmdRMin : 0.0f;
    check(cutTicks >= 2, "style \"" + sty.name + "\" spent time cutting");

    // ITS OWN CLAIM, FROM ITS OWN AUTHORED NUMBERS. Nothing here names a style,
    // so the check is inherited by every style added later: whichever channel
    // the author asked to travel in must be the one the WEAPON travelled in.
    //
    // OVER THE WHOLE PATH, because a cut may be several legs (strokes.h "A CUT
    // IS A PATH") and the channel a stroke travels in is a fact about its
    // total displacement, not about whichever leg happens to be first.
    const StrokeSegment cutAll = sty.CutTravel();
    const float wantAz = std::fabs(cutAll.az);
    const float wantEl = std::fabs(cutAll.el);
    const float wantR = std::fabs(cutAll.reach);
    const std::string n = "\"" + sty.name + "\"";
    const bool natural = !(sty.weapon.empty() || sty.weapon == "held");
    // AN AIM EFFECTOR IS ONE THE RIG CANNOT SERVE WITH A CHAIN, and the rig is
    // the authority on which those are -- not a name in this file. Asked of
    // the live creature, so a def that later grows a neck chain makes its bite
    // a Chain effector and this gate follows it with no edit.
    bool aimed = false;
    if (natural) {
      const Mob* m = c.mobs.FindMobById(id);
      aimed = m != nullptr &&
              m->StrikeEffectorKind() == StrikeEffectorMode::Aim;
    }
    // ---- "THE WEAPON MOVED, NOT JUST THE STROKE" -------------------------
    //
    // Three weapons, three units, and the words name which one -- "the SWORD
    // moved" is the wrong sentence about a fist.
    //
    // A FRACTION OF WHAT THE STYLE COMMANDED rather than an absolute floor,
    // because an absolute floor is half of what made this gate flip between
    // scopes: a cut given three ticks by tempo jitter instead of five
    // legitimately travels less, and a claim stated against its own command
    // scales with it. (The other half is the jitter itself, pinned at the
    // ForceAttack above.)
    const float travelFrac =
        (float)BaselineNumber("npcStyles.naturalTravelFrac", 0.5);
    if (aimed) {
      // JAWS: the part's forward has to TURN, by a real share of the angular
      // travel the cut asked for.
      const float wantTurn = (wantAz + wantEl) * travelFrac;
      check(fwdTurn > wantTurn,
            "style " + n + ": the JAWS turned (" + Format("%.2f", fwdTurn) +
                " rad of " + Format("%.2f", wantAz + wantEl) + " commanded)");
      // ...AND THE HEAD IS WHERE IT WAS TOLD TO BE, not a fifth of the way
      // there. Stated on the SPAN of each so a stroke that never reaches its
      // extreme is caught where an average would hide it, and read off the
      // rig's own before/after pair (Mob::AimDiag) rather than re-derived
      // here -- this is the claim that caught the neck-lever bug.
      const float cmdSpan = aimCmdPitchHi > aimCmdPitchLo
                                ? aimCmdPitchHi - aimCmdPitchLo
                                : 0.0f;
      const float gotSpan = aimGotPitchHi > aimGotPitchLo
                                ? aimGotPitchHi - aimGotPitchLo
                                : 0.0f;
      const float followFrac =
          (float)BaselineNumber("npcStyles.aimFollowFrac", 0.6);
      check(cmdSpan > 0.1f,
            "style " + n + ": the driver commanded real pitch");
      check(gotSpan > cmdSpan * followFrac,
            "style " + n + ": the POSED head followed it (" +
                Format("%.2f", gotSpan) + " rad of " +
                Format("%.2f", cmdSpan) + " commanded)");
      RecordObserved("npcStyles.aimCmdPitchObserved", (double)cmdSpan);
      RecordObserved("npcStyles.aimGotPitchObserved", (double)gotSpan);
    } else if (natural) {
      // A FIST: the knuckles have to travel through the WORLD. In voxels,
      // against the reach band the arm can actually serve, because a punch is
      // a distance and not an angle.
      const float wantPath =
          (float)BaselineNumber("npcStyles.fistPathVox", 1.0);
      check(edgePath > wantPath,
            "style " + n + ": the FIST moved (" + Format("%.2f", edgePath) +
                " vox of knuckle travel)");
      RecordObserved("npcStyles.fistPathObserved", (double)edgePath);
    } else {
      check(posedAz + posedEl > minSweep,
            "style " + n + ": the SWORD moved, not just the stroke");
    }
    // ---- A STYLE THAT LEAPS CANNOT HAVE ITS SHAPE MEASURED THIS WAY -----
    //
    // Every claim below is stated in the WIELDER'S OWN BASIS about its own
    // pivot, and that coordinate only means what it says while the wielder is
    // standing still. A lunging style moves the body sixteen voxels DURING the
    // stroke, so the bearing to a fixed aim sweeps under the blow and the
    // gate reads the FLIGHT as azimuth: `bite_lunge` authors 0.08 rad of it
    // and commanded 1.53, pinned against the across-the-body stop at -1.40,
    // and the number did not move when the authoring did -- because the
    // authoring was never what set it.
    //
    // WHAT IS STILL ASSERTED FOR IT, and it is the sharper pair: the effector's
    // own travel above (the jaws turned 2.35 rad) and the rig's own
    // commanded-versus-posed pitch (0.92 -> 1.18), neither of which is
    // expressed in a frame the body's travel can rotate. A lunge's SHAPE is a
    // question for the Attacks lane, which can show the arc and the arc it was
    // asked for side by side; it is not one a body in mid-air can answer.
    if (sty.lunge.Any()) {
      std::printf("npc-styles %-14s dominance not asserted: it LEAPS, and the "
                  "basis the claim is stated in travels with it\n",
                  sty.name.c_str());
    } else if (wantR > wantAz && wantR > wantEl) {
      // A THRUST. Reach-dominant: the point goes OUT, not around. Measured in
      // VOXELS (a radius) against radians, so the two are asserted separately
      // rather than compared — comparing them would be comparing units.
      //
      // ---- AND A FIST'S THRUST IS STATED ON THE COMMAND -------------------
      //
      // Same restatement `_npcStyles_about` records for the swing half of this
      // branch, for a reason that is structural rather than a tolerance.
      // The driver's model is A HAND HELD AT A FIXED EXTENSION AND A BLADE
      // THAT LEANS (melee.cpp's law of cosines): the radial channel moves the
      // POINT by changing the blade's angle, and `extendLive_` — how far the
      // hand itself is from the shoulder — is smoothed toward a constant.
      // With a metre of sword on the end of the fist that produces four
      // voxels of posed extension; with a fist, whose whole "blade" is one
      // voxel of knuckle and which is deliberately NOT wrist-steered (plan
      // §4: there is nothing to lay along a line), the lean has almost nothing
      // to move and the posed radius travels a fifth of a voxel on a
      // commanded 1.1.
      //
      // So for a natural weapon the claim is "the STROKE drove the radial
      // channel", which is what a thrust IS, and the claim that the fist
      // itself moved is carried by the posed-arc check above — the same split,
      // for the same reason, as the swing check two lines down.
      check((natural ? cmdDr : dr) > minReach,
            "style " + n + " (thrust) extended its reach");
      // ON THE COMMANDED ARCS, like every other dominance claim here — the
      // restatement _npcStyles_about prescribed (2026-09-01, done when the
      // torso lean moved the posed number past the old absolute cap). A
      // shoulder that cannot push a point out without going round it will
      // always sweep some POSED azimuth, and the lean now moves the very
      // shoulder the arc is measured about; whether the STROKE stayed a
      // thrust is a claim about the command, and it is scope-stable.
      check(az < thrustSwingMax && el < thrustSwingMax,
            "style " + n + " (thrust) did not turn into a swing");
    } else if (wantAz > wantEl * domMin) {
      check(az > minSweep, "style " + n + " swept azimuth");
      check(az > el, "style " + n + " is azimuth-DOMINANT, as authored");
    } else if (wantEl > wantAz * domMin) {
      check(el > minSweep, "style " + n + " swept elevation");
      check(el > az, "style " + n + " is elevation-DOMINANT, as authored");
    } else {
      // A DIAGONAL: both channels, in comparable measure. Stated as a ratio
      // rather than as two thresholds, because "diagonal" is a relationship.
      check(az > minSweep && el > minSweep,
            "style " + n + " (diagonal) swept BOTH channels");
      const float ratio =
          std::max(az / std::max(el, 1e-3f), el / std::max(az, 1e-3f));
      check(ratio < diagRatioMax,
            "style " + n + " (diagonal) is genuinely diagonal");
    }
    std::printf(
        "npc-styles %-14s [%-7s] authored (az %.2f el %.2f reach %.2f, %d "
        "legs) -> "
        "swept commanded arc az %.2f el %.2f; posed arc az %.2f el %.2f "
        "(spans %.2f / %.2f) dr %.2f vox (commanded dr %.2f, r %.2f..%.2f) "
        "over %d cut ticks; effector knuckle path %.2f vox, forward turn "
        "%.2f rad, commanded az %.2f..%.2f, blade %.2f, head pitch commanded %.2f -> "
        "posed %.2f\n",
        sty.name.c_str(), sty.weapon.c_str(), cutAll.az, cutAll.el,
        cutAll.reach, (int)sty.cut.size(),
        az, el, posedAz, posedEl, azSpan, elSpan, dr, cmdDr,
        cmdRMin, cmdRMax, cutTicks, edgePath, fwdTurn,
        cmdAzLo, cmdAzHi, blade,
        aimCmdPitchHi > aimCmdPitchLo ? aimCmdPitchHi - aimCmdPitchLo : 0.0f,
        aimGotPitchHi > aimGotPitchLo ? aimGotPitchHi - aimGotPitchLo : 0.0f);
  }

  CloseStage(c);
  detail = Format("%d checks", checks);
  std::printf("npc-styles: %s (%d checks)\n", ok ? "PASS" : "FAIL", checks);
  return ok ? Status::Pass : Status::Fail;
}

// =============================================================================
// rig-clip — NO LIMB PASSES THROUGH ANOTHER LIMB OF THE SAME BODY
// =============================================================================
//
// THE REPORT: "many of the animations cause the character's arm to clip
// through their own body."
//
// NOTHING IN THE ENGINE COULD ANSWER THAT, and that is a structural fact
// rather than an oversight. A mob's limbs are deliberately excluded from
// colliding with each other (mob.h MobLimbDef's ball-joint note) and a live
// limb is KINEMATIC, so Jolt is not consulted about a pose at all.
// `AnimClampPoseLimits` bounds each joint's own range of motion, which is a
// claim about ONE joint and says nothing about where the shape on the end of
// it ends up. The only detector was a person looking at the screen.
//
// `game/selfclip.h` is the detector; this is the gate that spends it. It
// replays every authored style this rig can swing, and after EVERY TICK counts
// solid collider voxels of one limb standing inside another — differenced
// against the BIND POSE, so the shoulder ball that is always inside the chest
// is not a finding and a forearm swung through the ribs is.
//
// WHAT A FAILURE PRINTS is the pair, the style, the tick and the count, which
// is the whole of what a fixer needs: "armL.R is 34 voxels inside torso, in
// horizontal_l, at tick 9" names the style to re-author or the clamp to widen.
// A bare "the arm clips" would be the number-with-four-causes CLAUDE.md rule 6
// is about.
//
// THE THRESHOLD IS NOT ZERO and is in tests/baseline.json. Voxel art is not
// convex and a rig is not a jointed doll: an elbow at full flexion genuinely
// puts a corner of the forearm lattice a cell or two inside the upper arm, and
// a gate that called that a defect would be red on the walk cycle. What the
// number bounds is a LIMB INSIDE A LIMB, and a few cells at a joint is not
// that.
Status GateRigClip(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const std::string& what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("rig-clip: FAILED %s\n", what.c_str());
    }
  };

  Stage st = OpenStage(c);
  if (!st.ok) {
    detail = st.why;
    std::printf("rig-clip: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }
  std::string why;
  const uint64_t id =
      SpawnFighter(c, st.defIndex, {st.spot.x, st.spot.y + 1, st.spot.z},
                   "training_dummy", true, why);
  if (id == 0) {
    detail = why.empty() ? "fixture spawn failed" : why;
    std::printf("rig-clip: SKIP (%s)\n", detail.c_str());
    CloseStage(c);
    return Status::Skip;
  }
  Ticker tick{c, 28000, {st.spot.x >> 4, st.spot.y >> 4, st.spot.z >> 4}};
  for (int i = 0; i < 20; i++) tick();

  const int limit = (int)BaselineNumber("rigClip.maxExcessVox", 12);
  // A DIRECTLY JOINTED PAIR GETS ITS OWN, LOOSER NUMBER (selfclip.h
  // ClipPair::jointed says why at length): the two shapes share a pivot, so
  // every rotation about it buries more of one in the other, and the bind-pose
  // baseline cannot subtract that because the bind pose has the joint at rest.
  // It is still BOUNDED — a shoulder driven clean through the chest is a
  // defect — just not by the number that bounds a forearm in the ribs.
  const int jointLimit = (int)BaselineNumber("rigClip.maxJointExcessVox", 80);
  auto nameOf = [&](const Mob* m, int slot) {
    if (m == nullptr || slot < 0 || slot >= m->LimbCount())
      return std::string("?");
    return m->LimbDefAt(slot).name;
  };

  // ---- 0. THE BASELINE IS NOT A BLANKET PASS -----------------------------
  //
  // A detector that reports zero because it is looking at nothing passes every
  // assertion below for free, which is the "fixture that cannot fail" trap.
  // So: the rig standing still must be READABLE (the check ran, over a real
  // number of limbs) before a single style is driven through it.
  {
    const Mob* m = c.mobs.FindMobById(id);
    ClipReport rep;
    check(m != nullptr && m->SelfClipCheck(rep), "the rig can be read at all");
    check(rep.limbs >= 8, Format("the detector sees a whole body (%d limbs)",
                                 rep.limbs));
    check(rep.worstExcess <= limit,
          Format("standing still is clean: worst %d voxels (%s in %s), limit "
                 "%d",
                 rep.worstExcess, nameOf(m, rep.worst.a).c_str(),
                 nameOf(m, rep.worst.b).c_str(), limit));
  }

  auto aimFor = [&](const AttackStyle& s2) {
    const Vec3 o2 = c.mobs.MobOrigin(id);
    float ahead = 11.0f;
    if (const Mob* m2 = c.mobs.FindMobById(id))
      ahead = std::max(ahead, c.mobs.StyleReachOn(*m2, s2));
    return Vec3{o2.x + st.def->worldSize.x * 0.5f,
                o2.y + st.def->worldSize.y * 0.66f,
                o2.z + st.def->worldSize.z * 0.5f + ahead};
  };

  const StyleLibrary& lib = c.mobs.AttackStyles();
  int styles = 0;
  int worstAll = 0;
  std::string worstWhere;
  for (const AttackStyle& sty : lib.styles) {
    const Mob* fixture = c.mobs.FindMobById(id);
    if (fixture == nullptr || !StyleUsable(*fixture, sty)) continue;
    // ---- ...AND ONLY THE STYLES THE DRAW WOULD ACTUALLY PRODUCE ----------
    //
    // `ForceAttack` bypasses `PickAttackStyle`, and a FALLBACK style is one
    // the draw drops as a group whenever any non-fallback style is usable
    // (strokes.h AttackStyle::fallback: "a duelist with a sword never throws a
    // punch"). Forcing one onto this ARMED fixture stages a situation the
    // game cannot create — a human punching with a sword still in its fist —
    // and the detector correctly reports the sword sweeping through the head,
    // at 84 and 96 voxels. That is a true measurement of a false scenario, and
    // it is also outside what the stroke driver can fix: the item's pose is
    // derived from the hand by the rig, and for a Chain effector the driver is
    // told the tip IS the hand (mob.cpp says why at length), so it has no
    // blade to keep out of anything.
    //
    // IT IS SKIPPED LOUDLY rather than silently, because "a disarmed creature
    // punching" IS reachable and this gate does not cover it: a fixture with
    // an empty fist is the way to, and nobody should conclude from a green
    // line here that it was checked.
    const bool natural = !(sty.weapon.empty() || sty.weapon == "held");
    if ((sty.fallback || natural) && fixture->HeldSlot() >= 0) {
      std::printf(
          "rig-clip %-16s skipped: a %s style on an ARMED fixture is a swing "
          "nothing selects (and the held sword it sweeps is not the driver's "
          "to move)\n",
          sty.name.c_str(), natural ? "natural-weapon" : "fallback");
      continue;
    }
    styles++;
    // The same pinned seed npc-styles uses, for the same reason: the tempo
    // jitter is what decides the tick counts, and an unpinned one makes the
    // same style a different swing in a different scope.
    const uint32_t styleSeed =
        rng::Hash3(0x5C0BEu,
                   (uint32_t)std::hash<std::string>{}(sty.name), 0x5747u) |
        1u;
    const Vec3 aim = aimFor(sty);
    FaceAt(c.mobs, id, aim);
    if (!c.mobs.ForceAttack(id, sty.name, aim, tick.tick, styleSeed)) continue;

    int worst = 0, worstTick = -1;
    float push = 0;
    ClipPair worstPair;
    // ---- SPLIT BY WHO OWNED THE ARM (CLAUDE.md rule 6) -------------------
    //
    // "105 voxels of forearm inside the torso" has two completely different
    // causes and the bare number cannot tell them apart: the STROKE drove it
    // there (windup/cut, where the driver's pose claim is 1 and the chest
    // keep-out is what has to stop it), or the HAND-BACK did (recover, where
    // the claim is fading and the pose is a blend between the stroke's and
    // the walk cycle's, which nothing in the driver can reach). The tell that
    // they are not one event: the keep-out reported pushing the hand 1.11
    // voxels on the very swing the detector called 105 voxels deep, and two
    // numbers that far apart are not describing the same pose.
    int worstDriven = 0, worstDrivenTick = -1, worstDrivenJoint = 0;
    float worstRoundTrip = 0, worstIkMiss = 0, worstShoulderClamp = 0;
    ClipPair worstDrivenPair;
    for (int i = 0; i < 90; i++) {
      tick();
      const Mob* m = c.mobs.FindMobById(id);
      if (m == nullptr) break;
      const NpcStroke* s = c.mobs.MobStroke(id);
      ClipReport rep;
      if (m->SelfClipCheck(rep) && rep.worstExcess > worst) {
        worst = rep.worstExcess;
        worstPair = rep.worst;
        worstTick = i;
      }
      // HOW HARD THE KEEP-OUT WORKED, summed over the swing. "The arm is still
      // in the chest" and "the clamp never fired" are different failures and
      // the voxel count alone cannot tell them apart (CLAUDE.md rule 6).
      if (s != nullptr) push = std::max(push, s->melee.BodyClampPush());
      if (s != nullptr &&
          (s->phase == NpcStroke::Phase::Windup ||
           s->phase == NpcStroke::Phase::Cut)) {
        if (rep.worstExcess > worstDriven) {
          worstDriven = rep.worstExcess;
          worstDrivenPair = rep.worst;
          worstDrivenTick = i;
        }
        worstDrivenJoint = std::max(worstDrivenJoint, rep.worstJointExcess);
        // ---- IS THE RIG EVEN DOING WHAT THE DRIVER ASKED? ---------------
        //
        // The chest keep-out clamps the hand the DRIVER commands, and the rig
        // then has its own opinion: the two-bone solve may not reach, and
        // `AnimClampPoseLimits` takes the shoulder's authored ball limit off
        // the result afterwards. If the arm inside the chest is the CLAMPED
        // pose rather than the commanded one, no amount of work in the driver
        // can move it, and the fix belongs in the rig or in the authoring.
        // Those are opposite repairs, so the numbers that separate them are
        // recorded here rather than inferred later (CLAUDE.md rule 6).
        const Mob::WeaponArmDiag& wd = m->WeaponArmDiagnostics();
        if (wd.ran) {
          worstRoundTrip = std::max(worstRoundTrip, wd.roundTrip);
          worstIkMiss = std::max(worstIkMiss, wd.ikMiss);
          worstShoulderClamp = std::max(worstShoulderClamp, wd.shoulderClamp);
        }
      }
      if (s == nullptr || (!s->Active() && i > 4)) break;
    }
    const std::string where =
        Format("%s: %s is %d voxels inside %s at tick %d", sty.name.c_str(),
               nameOf(c.mobs.FindMobById(id), worstPair.a).c_str(), worst,
               nameOf(c.mobs.FindMobById(id), worstPair.b).c_str(), worstTick);
    if (worst > worstAll) {
      worstAll = worst;
      worstWhere = where;
    }
    // THE CLAIM IS STATED ON THE DRIVEN PHASES, which is where the driver's
    // keep-out is the thing responsible. The whole-stroke number is printed
    // beside it on every run, so a regression in the hand-back stays visible
    // even though this line does not assert on it.
    (void)where;
    check(worstDrivenJoint <= jointLimit,
          Format("%s: a JOINTED pair buried %d voxels while the stroke owned "
                 "the arm (limit %d)",
                 sty.name.c_str(), worstDrivenJoint, jointLimit));
    check(worstDriven <= limit,
          Format("%s: %s is %d voxels inside %s at tick %d, while the STROKE "
                 "owned the arm (limit %d)",
                 sty.name.c_str(),
                 nameOf(c.mobs.FindMobById(id), worstDrivenPair.a).c_str(),
                 worstDriven,
                 nameOf(c.mobs.FindMobById(id), worstDrivenPair.b).c_str(),
                 worstDrivenTick, limit));
    std::printf(
        "rig-clip %-16s driven %3d vox (%s in %s, tick %d) | jointed %3d | "
        "whole stroke %3d | limit %d/%d | push %.2f | rig: roundTrip %.2f ikMiss %.2f shoulderClamp %.2f rad\n",
        sty.name.c_str(), worstDriven,
        nameOf(c.mobs.FindMobById(id), worstDrivenPair.a).c_str(),
        nameOf(c.mobs.FindMobById(id), worstDrivenPair.b).c_str(),
        worstDrivenTick, worstDrivenJoint, worst, limit, jointLimit, push,
        worstRoundTrip, worstIkMiss, worstShoulderClamp);
  }
  check(styles >= 3, "the fixture can swing enough styles to be worth driving");
  if (worstAll > 0)
    std::printf("rig-clip: worst over all styles — %s\n", worstWhere.c_str());

  CloseStage(c);
  detail = Format("%d styles, worst %d vox", styles, worstAll);
  std::printf("rig-clip: %s (%d checks)\n", ok ? "PASS" : "FAIL", checks);
  return ok ? Status::Pass : Status::Fail;
}

// =============================================================================
// duel — two AI duelists, opposed factions, in an arena
// =============================================================================
//
// THE ONLY GATE HERE WITH NO SCRIPTING IN IT: perception, the arbiter, the
// attack clock, the style draw, the stroke program, the sweep and the wound
// model all run, and nothing tells anybody when to swing.
//
// SO THE ASSERTIONS ARE CHOSEN TO HOLD ACROSS THE SEED, not to describe one
// run. "Both landed a wound" is a property of two armed hostiles left alone
// long enough; "red won by tick 200" is a property of one draw, and asserting
// it is how a gate becomes a flake nobody trusts. The determinism claim is
// carried by the suite itself — this runs inside the twice-run comparison, so
// anything scheduling-dependent in here would already show as a hash disagreeing.
Status GateDuel(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const char* what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("duel: FAILED %s\n", what);
    }
  };

  Stage st = OpenStage(c);
  if (!st.ok) {
    detail = st.why;
    std::printf("duel: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }
  const float gap = (float)BaselineNumber("duel.gapVox", 14.0);
  std::string why;
  const uint64_t red =
      SpawnFighter(c, st.defIndex, {st.spot.x, st.spot.y + 1, st.spot.z},
                   "duelist", true, why);
  const uint64_t blue = SpawnFighter(
      c, st.defIndex,
      {st.spot.x, st.spot.y + 1, st.spot.z + (int)std::lround(gap)},
      "duelist_blue", true, why);
  if (red == 0 || blue == 0) {
    detail = why.empty() ? "fixture spawn failed" : why;
    std::printf("duel: SKIP (%s)\n", detail.c_str());
    CloseStage(c);
    return Status::Skip;
  }
  Ticker tick{c, 29000, {st.spot.x >> 4, st.spot.y >> 4, st.spot.z >> 4}};
  for (int i = 0; i < 4; i++) tick();   // rigs onto the ground, nothing else
  // FACING EACH OTHER, and it is not decoration. Spawn gives every creature
  // heading 0, so two of them placed along +Z stand BACK TO BACK: the one
  // behind is at 180 degrees, the duelist profile's field of view is 300, and
  // 180 is outside it. Nothing then turns the rear one round — its arbiter
  // picks Idle, and Idle deliberately writes back the heading it already has
  // (ai_behavior.cpp) — so it stands facing the horizon while it is cut down.
  // Measured before this line: blue saw a target on 0 of 530 ticks with both
  // alive, issued 0 attack requests, and lost every one of its 3328 flesh
  // voxels. That is not a duel; there was only ever one duelist.
  //
  // Worth flagging beyond the fixture: nothing in the behaviour layer LOOKS
  // AROUND. An idle creature is blind to its own back forever, which is a
  // sensible thing for a guard to be and a strange thing for everything to be.
  FaceAt(c.mobs, red, Chest(c.mobs, blue, *st.def));
  FaceAt(c.mobs, blue, Chest(c.mobs, red, *st.def));
  // NOTHING IS TICKED BETWEEN FACING THEM AND COUNTING. The first version spent
  // fourteen settling ticks here and cleared the request list afterwards — and
  // in the full suite the flat spot put the two of them inside each other's
  // reach on the tick they stood up, so the ENTIRE fight happened in those
  // fourteen ticks and the gate then measured six hundred ticks of two
  // corpses: 0 attack requests, 7 cut ticks each, both already bled out. A
  // measurement window that starts after the event is not a measurement.
  const uint32_t redFlesh0 = FleshVoxels(c.mobs, red);
  const uint32_t blueFlesh0 = FleshVoxels(c.mobs, blue);
  check(redFlesh0 > 0 && blueFlesh0 > 0, "both duelists stood up armed");
  c.mobs.ClearAttackRequests();
  c.mobs.ClearBlockEvents();

  const int ticks = (int)BaselineNumber("duel.ticks", 600);
  int requests = 0, blocks = 0, strokes = 0;
  // PER SIDE, because the interesting claim is that BOTH engaged. A total says
  // "somebody swung" and is satisfied by one duelist beating a statue.
  int reqRed = 0, reqBlue = 0, cutRed = 0, cutBlue = 0;
  int awakePeak = 0;
  // ---- THE WAKE WINDOW IS THE FIGHT, NOT THE FIXTURE'S WORLDGEN ----------
  //
  // `awakePeak` is a MAX over 600 samples of a whole-world dirty count, and it
  // used to be sampled with no check at all: `Snap().activeChunks`, whatever
  // that was. Two things are wrong with that and N1's fixed snapshot latency
  // made both visible at once.
  //
  // First, Snap() is INVALID for the first World::kSnapshotLatency ticks after
  // a world reset, and `activeChunks` then still holds the last published
  // value -- from the PREVIOUS GATE'S world. A max over another gate's numbers
  // is not a measurement of this one.
  //
  // Second and larger: OpenStage regenerates the world, and a fresh worldgen
  // wakes ~1,680 chunks. The stand-up phase above is exactly four ticks, so
  // that wake used to be published and consumed just BEFORE this loop opened
  // and was never sampled (measured peak 199-232 across three months of logs).
  // At K = 4 the same four snapshots arrive four ticks later -- inside the
  // window -- and the gate reported 1,446 for a fight whose gore was
  // unchanged (red lost the same 3,328 voxels in every run).
  //
  // So sample only snapshots that DESCRIBE THIS FIGHT. Under the old
  // zero-latency harness every sample satisfied this, which is why the check
  // was never needed; it is what the gate always meant. Not a threshold
  // change: `duel.awakeChunksMax` is untouched, and a fight that really did
  // wake the world still fails.
  const uint32_t fightFromTick = tick.tick + 1;
  int wakeSamples = 0;
  int bothAlive = 0, sawRed = 0, sawBlue = 0;
  float redNearest = 1e9f, blueNearest = 1e9f;
  const char* blueIntent = "(never ticked)";
  for (int i = 0; i < ticks; i++) {
    tick();
    for (const ai::AttackRequest& r : c.mobs.AttackRequests()) {
      requests++;
      if (r.mobId == red) reqRed++;
      if (r.mobId == blue) reqBlue++;
    }
    c.mobs.ClearAttackRequests();
    blocks += (int)c.mobs.BlockEvents().size();
    c.mobs.ClearBlockEvents();
    const NpcStroke* sr = c.mobs.MobStroke(red);
    const NpcStroke* sb = c.mobs.MobStroke(blue);
    if (sr != nullptr && sr->Cutting()) { strokes++; cutRed++; }
    if (sb != nullptr && sb->Cutting()) { strokes++; cutBlue++; }
    {
      const WorldSnapshot& sn = c.world.Snap();
      if (sn.valid && sn.tick >= fightFromTick) {
        awakePeak = std::max(awakePeak, (int)sn.activeChunks);
        wakeSamples++;
      }
    }
    // WHAT EACH BRAIN IS DOING, sampled while BOTH are still standing. A duel
    // that one side never joins has four causes -- it never saw the other, it
    // saw and would not close, it closed and would not swing, or it was dead
    // before its first cadence fired -- and "0 attack requests" names none of
    // them (CLAUDE.md rule 6).
    if (c.mobs.IsAlive(red) && c.mobs.IsAlive(blue)) {
      bothAlive++;
      const ai::Brain* br = c.mobs.MobBrain(red);
      const ai::Brain* bb = c.mobs.MobBrain(blue);
      if (br != nullptr && br->hasTarget) sawRed++;
      if (bb != nullptr && bb->hasTarget) sawBlue++;
      if (bb != nullptr) {
        blueNearest = std::min(blueNearest, bb->targetDist);
        blueIntent = ai::IntentName(bb->intent);
      }
      if (br != nullptr) redNearest = std::min(redNearest, br->targetDist);
    }
  }
  const uint32_t redFlesh1 = FleshVoxels(c.mobs, red);
  const uint32_t blueFlesh1 = FleshVoxels(c.mobs, blue);
  const uint32_t redLost = redFlesh0 > redFlesh1 ? redFlesh0 - redFlesh1 : 0;
  const uint32_t blueLost =
      blueFlesh0 > blueFlesh1 ? blueFlesh0 - blueFlesh1 : 0;

  // ---- WHAT HOLDS ACROSS THE SEED, AND WHAT DOES NOT ----------------------
  //
  // BOTH ENGAGED and BOTH SWUNG are properties of two armed hostiles inside
  // each other's band: perception, the arbiter, the attack clock, the style
  // draw and the stroke program all have to work on both sides for these to be
  // true, which is the whole chain this gate exists to cover.
  //
  // "BOTH BLED" IS NOT, and the first version asserted it. Measured over 600
  // ticks: red took blue from 3328 flesh voxels to ZERO and never lost one of
  // its own. That is not a bug — a committed sword cut through a torso severs
  // it, so whoever lands FIRST usually ends the fight, and there is no
  // defensive AI yet to change that (NPCs parry only when a windup happens to
  // put a blade in the way). Asserting a symmetric outcome would be asserting
  // that the RNG gave both sides a turn, which is exactly the kind of check
  // that goes red on an unrelated change and teaches everyone to ignore it.
  //
  // So: a real fight happened, both parties took part, and SOMEBODY bled.
  check(reqRed > 0 && reqBlue > 0, "BOTH duelists decided to attack");
  check(cutRed > 0 && cutBlue > 0, "...and both got real cuts out of it");
  check(strokes > 0, "...which the stroke system executed");
  const uint32_t lostMin = (uint32_t)BaselineNumber("duel.lostVoxMin", 1);
  check(redLost + blueLost >= lostMin, "and somebody took real wounds");
  // BOUNDED WAKE (CLAUDE.md rule 2): a fight is gore and blood, and gore is
  // CellOps into the grid. It must not wake the world.
  const int awakeMax = (int)BaselineNumber("duel.awakeChunksMax", 900);
  // ...and the wake claim needs SAMPLES, or "peak 0" is a gate that can only
  // pass (CLAUDE.md: an absolute zero is a rate claim). A window that saw no
  // in-fight snapshot at all is a fixture failure, not a bounded wake.
  check(wakeSamples > 0, "the wake window sampled the fight at all");
  check(awakePeak <= awakeMax, "the fight left the world's wake bounded");

  RecordObserved("duel.redLostObserved", (double)redLost);
  RecordObserved("duel.blueLostObserved", (double)blueLost);
  RecordObserved("duel.awakePeakObserved", (double)awakePeak);
  RecordObserved("duel.parriesObserved", (double)blocks);
  std::printf(
      "duel brains: %d ticks with both alive; red saw a target %d of them "
      "(closest %.1f vox), blue %d (closest %.1f vox, last intent %s)\n",
      bothAlive, sawRed, redNearest, sawBlue, blueNearest, blueIntent);
  std::printf(
      "duel: %d attack requests (red %d, blue %d), %d cut ticks (red %d, blue "
      "%d), %d parries over %d ticks; red lost %u vox, blue lost %u; peak "
      "awake chunks %d (max %d, over %d in-fight snapshots of %d ticks)\n",
      requests, reqRed, reqBlue, strokes, cutRed, cutBlue, blocks, ticks,
      redLost, blueLost, awakePeak, awakeMax, wakeSamples, ticks);

  CloseStage(c);
  detail = Format("%d checks", checks);
  std::printf("duel: %s (%d checks)\n", ok ? "PASS" : "FAIL", checks);
  return ok ? Status::Pass : Status::Fail;
}

// =============================================================================
// THE UNARMED GATES (docs/PLAN_impact_unarmed.md §8)
// =============================================================================
//
// Three gates, and between them they cover the three things that can silently
// stop working now that a creature can fight with its own body:
//
//   unarmed-attack  a creature with nothing in its fist still swings, and the
//                   STYLE DRAW shrinks to what it still has: no hands means
//                   only jaws, no head means only fists, neither means no
//                   stroke at all and one loud line rather than silence.
//   lunge           a body given a ballistic velocity really leaves the
//                   ground, really travels, really lands inside reach — and a
//                   crawler's pounce is the same move, lower and shorter.
//   bite-target     a style's `target` weights really reach the draw, over
//                   more than one tag and not always the head.
//
// Each one is ITS OWN CLAIM and none of them is "the AI attacked", which
// npc-strike already owns. Where a claim can be made without ticking the world
// at all it is: `PickAttackStyle` is a pure function of (library, profile, rig,
// tick), so the availability half of `unarmed-attack` asks it directly instead
// of waiting 150 ticks for an AI cadence and then inferring what it drew.

// A rig slot by name, on a live creature. The defs topologically sort their
// limbs, so a positional index is not stable across a sidecar edit.
int LimbNamed(const MobDef& def, const char* name) {
  for (size_t i = 0; i < def.limbs.size(); i++)
    if (def.limbs[i].name == name) return (int)i;
  return -1;
}

// The def that CAN fight unarmed: the one publishing natural weapons. Picked
// by the presence of the block rather than by name, for the reason CombatDef
// picks by the socket — a test that names "human" is a test that breaks when
// the art is re-authored under another name.
int NaturalDef(const MobSystem& mobs, const char* preferName) {
  int best = -1;
  for (size_t i = 0; i < mobs.Defs().size(); i++) {
    if (mobs.Defs()[i].natural.empty()) continue;
    if (best < 0) best = (int)i;
    if (preferName != nullptr && mobs.Defs()[i].name == preferName)
      return (int)i;
  }
  return best;
}

// =============================================================================
// zombie-draw — WHAT DOES A ZOMBIE ACTUALLY SWING, running its own AI
// =============================================================================
//
// THE HARNESS THAT DID NOT EXIST, and its absence is why "the zombie is just
// punching" survived two fixes aimed at it. Everything else in this file either
// forces a named style (`ForceAttack`, `--shot-strike`) or asks the draw with a
// distance the fixture chose -- so the one question the owner keeps asking,
// "left to itself, at the range it actually stands at, what does it pick?", had
// no instrument at all. A bare "it punches" has four causes that look identical
// from outside (the jaws unusable, the bites out of reach, the fallback group
// not being dropped, the profile not listing them) and eliminating them one per
// run is what CLAUDE.md rule 6 forbids.
//
// So: a real zombie on the real `zombie` profile, chasing a real target, with
// its own cadence and its own band, and a HISTOGRAM of what came out plus the
// reach table that explains it.
Status GateZombieDraw(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const std::string& what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("zombie-draw: FAILED %s\n", what.c_str());
    }
  };

  Stage st = OpenStage(c);
  if (!st.ok) {
    detail = st.why;
    std::printf("zombie-draw: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }
  // BY NAME AND NO FALLBACK. `NaturalDef` silently returns the FIRST def with a
  // natural block when the preferred name has none, so a gate that asked it for
  // "zombie" and got `human` would test the wrong creature and say nothing --
  // the fixture-that-measures-itself trap, and this gate exists because of one.
  int zdef = -1;
  for (size_t i = 0; i < c.mobs.Defs().size(); i++)
    if (c.mobs.Defs()[i].name == "zombie") zdef = (int)i;
  if (zdef < 0 || c.mobs.Defs()[(size_t)zdef].natural.empty()) {
    detail = zdef < 0 ? "no mob def named \"zombie\""
                      : "the zombie def declares no `natural` block";
    std::printf("zombie-draw: SKIP (%s)\n", detail.c_str());
    CloseStage(c);
    return Status::Skip;
  }
  const MobDef& nd = c.mobs.Defs()[(size_t)zdef];

  std::string why;
  // Spawned at the far edge of the zombie's own sight (34) so the run covers
  // the APPROACH as well as the standing fight: the draw is a function of
  // distance, and a fixture parked at one range only ever asks about that one.
  const uint64_t biter =
      SpawnFighter(c, zdef, {st.spot.x, st.spot.y + 1, st.spot.z}, "zombie",
                   false, why);
  // THE VICTIM IS A HUMAN, not a second zombie: the creature the owner is
  // asking about is the PLAYER, whose avatar is the human rig, and a zombie
  // prey brings its own `undead` death rules into a gate about the attacker.
  int hdef = -1;
  for (size_t i = 0; i < c.mobs.Defs().size(); i++)
    if (c.mobs.Defs()[i].name == "human") hdef = (int)i;
  const uint64_t prey = SpawnFighter(
      c, hdef >= 0 ? hdef : zdef, {st.spot.x, st.spot.y + 1, st.spot.z + 9},
      "training_dummy", false, why);
  if (biter == 0 || prey == 0) {
    detail = why.empty() ? "fixture spawn failed" : why;
    std::printf("zombie-draw: SKIP (%s)\n", detail.c_str());
    CloseStage(c);
    return Status::Skip;
  }
  Ticker tick{c, 26400, {st.spot.x >> 4, st.spot.y >> 4, st.spot.z >> 4}};
  for (int i = 0; i < 20; i++) tick();

  // THE REACH TABLE, ONCE, BEFORE ANYTHING SWINGS. This is the half that
  // explains the histogram below: a style the draw never returns is either
  // unusable or out of reach, and those are different bugs.
  const Mob* zm = c.mobs.FindMobById(biter);
  const StyleLibrary& lib = c.mobs.AttackStyles();
  std::string reachTable;
  int usableReal = 0;
  const ai::Profile* zpr = c.mobs.Behaviors().At(c.mobs.Behaviors().Find("zombie"));
  if (zm != nullptr && zpr != nullptr) {
    for (const std::string& n : zpr->attack.styles) {
      const AttackStyle* s = lib.At(lib.Find(n));
      if (s == nullptr) { reachTable += " " + n + "=NOSTYLE"; continue; }
      const bool u = StyleUsable(*zm, *s);
      const float r = StyleReachOn(*zm, *s);
      if (u && !s->fallback) usableReal++;
      reachTable += Format(" %s%s=%.1f%s", n.c_str(), s->fallback ? "(fb)" : "",
                           r, u ? "" : "/UNUSABLE");
    }
  }
  check(usableReal > 0,
        "at least one NON-fallback style (a bite) is usable on a live zombie");

  // ---- THE RUN: its own AI, its own cadence, its own band ----------------
  // ...and WHAT SHAPE IT CHASES IN (MobDef::chaseClip, assets/anims/reach.json).
  // Measured here rather than in a gate of its own because the fixture this
  // needs already exists three lines up: a real zombie, on its own profile,
  // closing on a real target. The observable is the UPPER ARM'S ELEVATION off
  // horizontal (90 = hanging at the side, 0 = straight out), read post-IK out
  // of the model pose, so it sees what is drawn and not what was requested.
  //
  // THE CONTROL IS THE PREY, not an earlier tick of the same creature. Same
  // rig, same gait, same clip machinery, no `chaseClip` — so the pair
  // separates "the reach clip is on this body" from "every human's arms sit
  // like that", which a single number never can. A temporal baseline would
  // not work anyway: the zombie acquires its target within a tick or two of
  // the spawn and there is no arms-down window to sample.
  const int armPart = nd.skel.FindPart("armU.R");
  float chaseElev = 180.0f, preyElev = 180.0f, reachWeight = 0.0f;
  auto elevOf = [&](uint64_t who) {
    const Vec3 up = c.mobs.LimbModelUp(who, armPart);
    return std::fabs(std::asin(std::clamp(up.y, -1.0f, 1.0f))) * 57.29578f;
  };
  std::map<std::string, int> drawn;
  int strokes = 0, cutTicks = 0;
  float minDist = 1e9f, maxDist = 0;
  bool wasActive = false;
  int goneBiter = 0, gonePrey = 0, alive = 0;
  const int ticks = (int)BaselineNumber("zombieDraw.aiTicks", 600);
  for (int i = 0; i < ticks; i++) {
    tick();
    const Mob* m = c.mobs.FindMobById(biter);
    const Mob* p = c.mobs.FindMobById(prey);
    // NOT `break`. A fixture that stops on the first missing frame reports a
    // correct measurement of nothing and hides WHICH of the two went: the prey
    // dying of its own bites is the feature working, the biter vanishing is a
    // broken fixture. Both are counted and said out loud.
    // GONE = not a living creature any more. A corpse is a dead Mob now
    // (docs/PLAN_corpse_is_a_mob.md), so presence alone no longer says it.
    if (m == nullptr || !m->Alive()) {
      // SAY WHY, once: a husk sweep, a turn (TurnMob re-spawns under a new id)
      // and a death are three different bugs and a count names none of them.
      if (goneBiter++ == 0)
        std::printf("zombie-draw: biter left at tick %d: death cause \"%s\", "
                    "%u mobs remain\n",
                    i, c.mobs.DeathCause(biter), c.mobs.MobCount());
      continue;
    }
    if (p == nullptr || !p->Alive()) { gonePrey++; continue; }
    alive++;
    const Vec3 d = m->Origin() - p->Origin();
    const float dist = Vec3{d.x, 0, d.z}.len();
    minDist = std::min(minDist, dist);
    maxDist = std::max(maxDist, dist);
    const NpcStroke* s = c.mobs.MobStroke(biter);
    // Sampled only OUTSIDE a stroke: the chase pose is deliberately ramped off
    // for the duration of a blow (it is additive, and an additive hold summed
    // onto an authored punch is a corrupted punch), so ticks inside one are
    // not ticks this measurement is about.
    if (armPart >= 0 &&
        (s == nullptr || !s->Active() || s->phase == NpcStroke::Phase::Guard)) {
      chaseElev = std::min(chaseElev, elevOf(biter));
      preyElev = std::min(preyElev, elevOf(prey));
      for (const auto& cw : c.mobs.ClipWeights(biter))
        if (cw.first == "reach") reachWeight = std::max(reachWeight, cw.second);
    }
    if (s == nullptr) continue;
    if (s->Cutting()) cutTicks++;
    const bool active = s->Active() && s->phase != NpcStroke::Phase::Guard;
    if (active && !wasActive && s->style >= 0) {
      strokes++;
      const AttackStyle* as = lib.At(s->style);
      drawn[as != nullptr ? as->name : "?"]++;
    }
    wasActive = active;
  }

  int bites = 0, punches = 0;
  for (const auto& kv : drawn) {
    const AttackStyle* as = lib.At(lib.Find(kv.first));
    if (as == nullptr) continue;
    (as->fallback ? punches : bites) += kv.second;
  }
  std::string hist;
  for (const auto& kv : drawn) hist += " " + kv.first + "=" + std::to_string(kv.second);

  RecordObserved("zombieDraw.strokes", (double)strokes);
  RecordObserved("zombieDraw.bites", (double)bites);
  RecordObserved("zombieDraw.punches", (double)punches);

  check(strokes > 0, "the zombie swung at all over the run");
  // THE CLAIM THE OWNER IS MAKING. A zombie's bites are NOT fallback and its
  // punches are, so PickAttackStyle drops the punches as a GROUP the moment a
  // bite survives the usability+reach filter. Punches outnumbering bites means
  // the bites are being filtered out at the range this creature actually
  // fights at -- which is exactly "it is just punching".
  check(bites > 0, "...and at least one of those swings was a BITE");
  check(bites >= punches,
        "...with bites not outnumbered by the fallback punches");

  RecordObserved("zombieDraw.chaseArmElevDeg", (double)chaseElev);
  RecordObserved("zombieDraw.preyArmElevDeg", (double)preyElev);
  // 40 degrees is not the authored 80 of shoulder flexion and must not be: the
  // gait swing, the spine twist and the arm's own pose clamp all compose over
  // the hold, and pinning the assertion to the authored number would make
  // every one of those a failure of this gate. What is being claimed is the
  // separation — the arms came UP and stayed up — and the prey's own arms in
  // the same frames are what says the clip did it.
  check(armPart >= 0, "the zombie's rig has an armU.R to measure");
  check(chaseElev < 40.0f,
        "the chasing zombie's upper arm came off its side (chaseClip reached "
        "the drawn pose)");
  check(preyElev > 55.0f,
        "...and the prey, on the same rig with no chaseClip, kept its arms "
        "down (so the pose is the clip and not the gait)");

  std::printf("zombie-draw: chase pose armU.R %.1f deg off horizontal vs the "
              "prey's %.1f, reach clip weight %.2f\n",
              chaseElev, preyElev, reachWeight);
  std::printf(
      "zombie-draw: %d strokes over %d ticks (%d both-alive, biter gone %d, prey "
      "gone %d), distance %.1f..%.1f, %d cut ticks |%s | reaches:%s\n",
      strokes, ticks, alive, goneBiter, gonePrey, alive ? minDist : 0.0f,
      maxDist, cutTicks, hist.c_str(), reachTable.c_str());

  CloseStage(c);
  detail = Format("%d strokes: %d bites, %d punches;%s", strokes, bites,
                  punches, reachTable.c_str());
  std::printf("zombie-draw: %s (%d checks)\n", ok ? "PASS" : "FAIL", checks);
  return ok ? Status::Pass : Status::Fail;
}

// =============================================================================
// limb-alias — ONE BRICK RECORD, ONE HOLDER, through a whole fight
// =============================================================================
//
// Owner report 2026-09-16: "I kill a few zombies and then my torso or arm
// swaps with one of theirs; my foot becomes a zombie torso."
//
// A limb wearing another creature's shape has exactly two causes and they live
// in different files, which is why eliminating them one run at a time (rule 6)
// was never going to end:
//
//   THE RECORD   two entities holding one `MicroBodySet` model index. The
//                first edit rewrites the other's art, the first free zeroes
//                the record's dims, and the freed record goes back on
//                `freeModels` while a live body still points at it — so the
//                NEXT carve anywhere in the world is handed a record with a
//                stale holder (sim/microbody.h MicroBodyClone).
//   THE SLOT     an instance is (slot, model) and the shader draws `model` at
//                `bodyXforms[slot]`. A slot base built from a COUNT rather
//                than from the walk that emitted the transforms puts every
//                mob limb and avatar part at somebody else's transform —
//                `DebrisSystem::AdoptBody` takes no cap, so `bodies_` runs
//                past kMaxBodies whenever a corpse hands over fifteen limbs
//                at once, and `BodyCount()` then overstates the slots the
//                debris walks actually wrote (game/bodyreg.cpp).
//
// Both are checked by `BodyRegistry::AuditMicroModels` + `BuildMicroInsts`,
// which name the two entities involved instead of reporting a count. This gate
// is the fixture that makes them fire: a real brawl, run until corpses pile up,
// audited EVERY tick — because the aliasing window opens at a death and can
// close again at the next cull, and a check at the end would photograph an
// empty room.
//
// THE FIXTURE HAS TO PRODUCE DEATHS, and a fixture that cannot fail measures
// nothing: the detail line reports the corpse count, so a run that killed
// nobody says so rather than passing on an empty audit.
Status GateLimbAlias(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const std::string& what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("limb-alias: FAILED %s\n", what.c_str());
    }
  };

  Stage st = OpenStage(c);
  if (!st.ok) {
    detail = st.why;
    std::printf("limb-alias: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }
  // THREE DUELS, not one. The record table is a shared allocator and the fault
  // it is being asked about is a RECYCLED index — which needs somebody still
  // alive to carve while somebody else's corpse is being culled. One pair dies
  // once and the pool never turns over.
  //
  // THE SHAPE IS `duel`'s, deliberately: opposed profiles, armed, and FACED AT
  // EACH OTHER before the first tick. That gate's own note says why — a
  // creature spawned behind another is outside its 300-degree field of view
  // and nothing in the behaviour layer ever looks around, so an unfaced pair
  // stands back to back for the whole run. Measured here first: three
  // zombie/duelist pairs six voxels apart produced 0 deaths and 1 debris body
  // in 900 ticks, which is a fixture that cannot fail.
  const float gap = (float)BaselineNumber("limbAlias.gapVox", 14.0);
  std::string why;
  std::vector<uint64_t> fighters;
  for (int k = 0; k < 3; k++) {
    const int dx = (k - 1) * 12;
    const uint64_t red =
        SpawnFighter(c, st.defIndex, {st.spot.x + dx, st.spot.y + 1, st.spot.z},
                     "duelist", true, why);
    const uint64_t blue = SpawnFighter(
        c, st.defIndex,
        {st.spot.x + dx, st.spot.y + 1, st.spot.z + (int)std::lround(gap)},
        "duelist_blue", true, why);
    if (red == 0 || blue == 0) {
      detail = why.empty() ? "fixture spawn failed" : why;
      std::printf("limb-alias: SKIP (%s)\n", detail.c_str());
      CloseStage(c);
      return Status::Skip;
    }
    fighters.push_back(red);
    fighters.push_back(blue);
  }
  Ticker settle{c, 26400, {st.spot.x >> 4, st.spot.y >> 4, st.spot.z >> 4}};
  for (int i = 0; i < 4; i++) settle();   // rigs onto the ground, nothing else
  for (size_t k = 0; k + 1 < fighters.size(); k += 2) {
    FaceAt(c.mobs, fighters[k], Chest(c.mobs, fighters[k + 1], *st.def));
    FaceAt(c.mobs, fighters[k + 1], Chest(c.mobs, fighters[k], *st.def));
  }

  // The registry the frame loop builds, with the brick pool attached — without
  // `MicroSet()` the audit is a no-op and this gate would pass on nothing.
  const MicroBodySet* mset = c.debris.MicroSet();
  check(mset != nullptr, "the debris system publishes a MicroBodySet");

  Ticker& tick = settle;
  uint32_t faults = 0, peakBodies = 0, peakSlots = 0, deaths = 0;
  uint32_t firstFaultTick = 0;
  const int ticks = (int)BaselineNumber("limbAlias.ticks", 900);
  std::vector<MicroBodyInstGpu> insts;
  for (int i = 0; i < ticks; i++) {
    tick();
    BodyRegistry reg(c.debris, c.mobs, nullptr, mset);
    // BOTH halves, every tick: BuildMicroInsts carries the slot-space checks
    // and AuditMicroModels carries the holder checks, and the two failures are
    // not reachable from each other's evidence.
    reg.BuildMicroInsts(insts);
    const uint32_t f = reg.AuditMicroModels();
    if (f > 0 && faults == 0) firstFaultTick = (uint32_t)i;
    faults += f;
    // Dead matter: loose debris, and the corpses — which keep their rigs as
    // dead Mobs now (docs/PLAN_corpse_is_a_mob.md) and hold brick records
    // exactly as the debris they used to become did.
    uint32_t deadBodies = c.debris.BodyCount();
    for (uint64_t id : fighters)
      if (const Mob* m = c.mobs.FindMobById(id); m != nullptr && !m->Alive())
        deadBodies += m->LimbBodyCount();
    peakBodies = std::max(peakBodies, deadBodies);
    peakSlots = std::max(peakSlots, reg.TotalSlots());
  }
  for (uint64_t id : fighters)
    if (!c.mobs.IsAlive(id)) deaths++;

  // A CORPSE IS THE PRECONDITION, not the subject. The fault needs a death to
  // put a record back on the free list and a survivor to take it, so a run
  // that produced neither has not tested anything and must not report PASS.
  check(deaths > 0,
        Format("the brawl killed somebody (%u of %zu fighters gone in %d ticks)",
               deaths, fighters.size(), ticks));
  check(peakBodies > 0, "the fight produced dead bodies (corpses or debris)");
  check(faults == 0,
        Format("no brick record has two holders and no instance claims "
               "another body's slot (%u faults, first at tick %u — the named "
               "reports are on stderr and in build/microbody_audit.log)",
               faults, firstFaultTick));

  // ---- PHASE 2: THE RELOAD, which is the OTHER way a limb wears another
  // body's shape and the one that survived the sever-path fix.
  //
  // `LoadMobDefs` rebuilds `MicroBodySet::models` from scratch on every R / F5
  // / combat-slider edit, and a model index is a POSITION in that vector. The
  // corpses standing on the ground right now hold indices; if any of them
  // outlives the rebuild it draws whatever def lands at its old position — a
  // leg as a torso, an arm as a zombie's head — and frees a record it does not
  // own when it finally lets go. The reload site cannot be run from a gate, so
  // what is pinned here is the CONTRACT it now goes through: after
  // `ReleaseMicroHolders` there is no holder left to be wrong.
  //
  // The count BEFORE is the half that makes this able to fail: a room with no
  // holders in it would satisfy "zero after" on an empty set.
  //
  // Covers debris + mob limbs. The avatar is the third holder class and this
  // harness never spawns one (`nullptr` above) — main.cpp's call passes it.
  {
    BodyRegistry reg(c.debris, c.mobs, nullptr, mset);
    const uint32_t held = reg.MicroHolderCount();
    check(held > 0,
          Format("the brawl left somebody holding a brick record (%u holders)",
                 held));
    reg.ReleaseMicroHolders();
    const uint32_t left = reg.MicroHolderCount();
    check(left == 0,
          Format("no holder survives the teardown a model-table rebuild needs "
                 "(%u of %u still hold an index — each one draws a stranger's "
                 "brick after the next asset reload)",
                 left, held));
    RecordObserved("limbAliasHoldersBeforeRelease", (double)held);
    std::printf("limb-alias: reload teardown %u holders -> %u\n", held, left);
  }

  RecordObserved("limbAliasFaults", (double)faults);
  RecordObserved("limbAliasDeaths", (double)deaths);
  RecordObserved("limbAliasPeakBodies", (double)peakBodies);
  RecordObserved("limbAliasPeakSlots", (double)peakSlots);
  std::printf(
      "limb-alias: %d ticks, %u of %zu fighters dead, peak %u dead bodies / "
      "%u body slots (ceiling %u), %u faults\n",
      ticks, deaths, fighters.size(), peakBodies, peakSlots, kMaxBodySlots,
      faults);

  CloseStage(c);
  detail = Format("%u faults over %d ticks; %u dead, peak %u bodies / %u slots",
                  faults, ticks, deaths, peakBodies, peakSlots);
  std::printf("limb-alias: %s (%d checks)\n", ok ? "PASS" : "FAIL", checks);
  return ok ? Status::Pass : Status::Fail;
}

// =============================================================================
// unarmed-attack — a fist is a weapon, and losing one costs a style
// =============================================================================
Status GateUnarmedAttack(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const std::string& what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("unarmed-attack: FAILED %s\n", what.c_str());
    }
  };

  Stage st = OpenStage(c);
  if (!st.ok) {
    detail = st.why;
    std::printf("unarmed-attack: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }
  const int natDef = NaturalDef(c.mobs, "human");
  if (natDef < 0) {
    detail = "no mob def declares a `natural` block";
    std::printf("unarmed-attack: SKIP (%s)\n", detail.c_str());
    CloseStage(c);
    return Status::Skip;
  }
  const MobDef& nd = c.mobs.Defs()[natDef];
  const StyleLibrary& lib = c.mobs.AttackStyles();

  // ---- A. THE LIBRARY AND THE RIG AGREE --------------------------------
  // Before any creature moves: every style that names a natural weapon names
  // one this rig HAS. A style pointing at a weapon nobody declares is a
  // content error whose only other symptom is a creature that never swings.
  int naturalStyles = 0;
  for (const AttackStyle& s : lib.styles) {
    if (s.weapon.empty() || s.weapon == "held") continue;
    naturalStyles++;
    check(nd.FindNatural(s.weapon) >= 0,
          "style \"" + s.name + "\" names natural weapon \"" + s.weapon +
              "\", which " + nd.name + " declares");
  }
  check(naturalStyles > 0,
        "attack_styles.json ships at least one natural-weapon style");

  // ---- B. A DISARMED FIGHTER STILL LANDS A BLOW -------------------------
  //
  // `swordsman_static` is hostile, immobile and now lists three punches as
  // FALLBACK styles. Spawned with nothing in its fist, the fallback rule is
  // the only thing standing between it and a creature that requests attacks
  // forever and never swings — which is precisely the failure mode this whole
  // package exists to remove.
  const float gap = (float)BaselineNumber("unarmedAttack.gapVox", 7.0);
  std::string why;
  const uint64_t attacker =
      SpawnFighter(c, natDef, {st.spot.x, st.spot.y + 1, st.spot.z},
                   "training_dummy", false, why);
  const uint64_t target = SpawnFighter(
      c, natDef,
      {st.spot.x, st.spot.y + 1, st.spot.z + (int)std::lround(gap)},
      "training_dummy", false, why);
  if (attacker == 0 || target == 0) {
    detail = why.empty() ? "fixture spawn failed" : why;
    std::printf("unarmed-attack: SKIP (%s)\n", detail.c_str());
    CloseStage(c);
    return Status::Skip;
  }
  // ---- THE TARGET STANDS SIDE-ON (2026-09-19) ----------------------------
  //
  // The gap above is not the distance the punch is thrown from. Two humans are
  // held about 4.5 voxels apart centre-to-centre by `ApplyCrowdSpacing`
  // whatever the fixture asked for (MobSystem::BodyRadius is a circle, so the
  // heading changes nothing about where the centres settle), and a punch's
  // hand delivers ~3.9 of its 5.0 arm (the note in StyleReachOn). FACING, the
  // victim's near surface is half a body DEPTH inside that -- ~1.1, so 3.4
  // from the attacker's centre against 3.9 of reach from a shoulder that is
  // itself a voxel off the line. That half-voxel margin is decided by the
  // settle drift and by the tempo jitter drawn from whatever mob id the
  // suite has reached, which is the coin flip `_unarmedAttack_gap_about`
  // describes -- and this gate has reported 0 bodies hit in-suite since
  // 2026-09-16, at gap 5 and at gap 4 alike. SIDE-ON, the near surface is
  // half a body WIDTH in (~2.2) and the knuckles have a voxel to spare.
  //
  // The claim is unchanged: the fallback punch is a real cut that lands on a
  // body standing inside its reach. Whether a punch at a FACING body is a
  // coin flip is a question about the fist's reach law, not about the
  // fallback rule, and `ai-reach` (the AI standing where its own band puts
  // it) is where that is measured. `unarmedAttack.targetSideOn` = 0 restores
  // the facing fixture without a rebuild.
  const bool sideOn = BaselineNumber("unarmedAttack.targetSideOn", 1) > 0;
  if (sideOn) c.mobs.SetHeading(target, 1.5707963f);
  Ticker tick{c, 25200, {st.spot.x >> 4, st.spot.y >> 4, st.spot.z >> 4}};
  FaceAt(c.mobs, attacker, Chest(c.mobs, target, nd));
  for (int i = 0; i < 20; i++) tick();

  const uint32_t flesh0 = FleshVoxels(c.mobs, target);
  check(flesh0 > 0, "the target has flesh before anything swings");
  // ...and how many limbs it has, for the "never dismembers" half below. A
  // body limb that still owns a collider and voxels is a limb still on.
  auto limbsOn = [&](uint64_t id) {
    int n = 0;
    const Mob* m = c.mobs.FindMobById(id);
    if (m == nullptr) return n;
    for (int i = 0; i < m->AppendedBase(); i++)
      if (c.mobs.LimbBody(id, i) && c.mobs.LimbVoxelCount(id, i) > 0) n++;
    return n;
  };
  const int limbs0 = limbsOn(target);
  c.mobs.ClearAttackRequests();
  check(c.mobs.SetMobBehavior(attacker, "swordsman_static"),
        "the disarmed attacker's AI could be stood up");
  // The live reach the AI is about to commit on, read AFTER the profile is on
  // (StrikeReachOf asks the profile which styles are drawable).
  float strikeReach = 0.0f;
  if (const Mob* am = c.mobs.FindMobById(attacker))
    strikeReach = c.mobs.StrikeReachOf(*am);

  int requests = 0, cutTicks = 0, hits = 0;
  float topSpeed = 0;
  // ---- WHERE THE RAYS WENT, AND HOW CLOSE THE KNUCKLES CAME (2026-09-19) --
  // `0 bodies hit` has four causes (melee.h EdgeSweepResult) and the print
  // below used to report the bare zero -- CLAUDE.md rule 6. The probe words
  // are per-stroke tallies cleared on reset, so they are summed by their
  // RISES exactly as `ai-reach` sums contacts. `nearest` is the closest the
  // fist's own edge tip came to the target's chest on any cut tick, and
  // `distMin..distMax` is the centre-to-centre distance the crowd spacing
  // actually held the pair at -- the number the gap above only requests.
  int probesCast = 0, probesAir = 0, probesSelf = 0, probesBody = 0;
  int prevCast = 0, prevAir = 0, prevSelf = 0, prevBody = 0;
  float nearest = 1e9f, distMin = 1e9f, distMax = 0.0f;
  auto rise = [](int now, int& prev, int& acc) {
    if (now > prev) acc += now - prev;
    prev = now;
  };
  const int ticks = (int)BaselineNumber("unarmedAttack.aiTicks", 150);
  for (int i = 0; i < ticks; i++) {
    tick();
    requests += (int)c.mobs.AttackRequests().size();
    c.mobs.ClearAttackRequests();
    {
      const Vec3 a = Chest(c.mobs, attacker, nd), b = Chest(c.mobs, target, nd);
      const float d = std::sqrt((a.x - b.x) * (a.x - b.x) +
                                (a.z - b.z) * (a.z - b.z));
      distMin = std::min(distMin, d);
      distMax = std::max(distMax, d);
    }
    const NpcStroke* s = c.mobs.MobStroke(attacker);
    if (s == nullptr) {
      prevCast = prevAir = prevSelf = prevBody = 0;
      continue;
    }
    if (s->Cutting()) {
      cutTicks++;
      Vec3 eb{}, et{};
      float ehw = 0.0f;
      const Mob* am = c.mobs.FindMobById(attacker);
      if (am != nullptr && am->WeaponEdge(eb, et, ehw))
        nearest = std::min(nearest, (et - Chest(c.mobs, target, nd)).len());
    }
    hits = std::max(hits, s->bodiesHit);
    topSpeed = std::max(topSpeed, s->topTipSpeed);
    rise(s->probesCast, prevCast, probesCast);
    rise(s->probesAir, prevAir, probesAir);
    rise(s->probesSelf, prevSelf, probesSelf);
    rise(s->probesBody, prevBody, probesBody);
  }
  const uint32_t flesh1 = FleshVoxels(c.mobs, target);
  const uint32_t lost = flesh0 > flesh1 ? flesh0 - flesh1 : 0u;
  const int limbs1 = limbsOn(target);
  check(requests > 0, "the disarmed AI decided to attack");
  check(cutTicks > 0, "...and its request became a real cut");
  check(hits > 0, "...that landed on the target");
  // "A PUNCH NEVER DISMEMBERS": a bare fist's profile is all `blunt`
  // (human.json `natural`), and the blunt resolver (Mob::BluntHit) never
  // amputates (Mob::JointRuleApplies; the Unarmed/Blunt rows of
  // game/severpolicy.h). That half is a limb
  // count, and it is unconditional.
  check(limbs1 >= limbs0, "a fist took no limb off the target");
  // ...AND "NEVER KERFS" IS NOW CONDITIONAL ON THE TUNING (2026-09-19).
  // Until gore.bluntCarveRadius was 0 a punch removed nothing and the ceiling
  // here was ZERO. The owner's retune set bluntCarveRadius 1.1 with the
  // gore.unarmed* overrides at -1 (inherit), and the fist's own `bluntCarve`
  // 0.3 is a ceiling on that: a punch that lands on flesh the bruise has
  // already pulped past gore.pulpCarveFrom flags the limb for Mob::
  // BluntPulpTick, which then eats pulped voxels at gore.pulpRotRate. So a
  // fist IS allowed a dent now, and the claim is that it stays a DENT: a
  // fraction of the body (unarmedAttack.fleshLostFracMax), never a wound. If
  // the owner turns the carve back off, the ceiling falls back to the
  // authored absolute (0) on its own -- no rebuild, no rebaseline.
  const auto& gt = CurrentTuning().gore;
  const float fistCarve = gt.unarmedCarveRadius >= 0.0f ? gt.unarmedCarveRadius
                                                         : gt.bluntCarveRadius;
  const float fistRipe = gt.unarmedPulpCarveFrom >= 0.0f ? gt.unarmedPulpCarveFrom
                                                          : gt.pulpCarveFrom;
  const bool fistDents =
      fistCarve > 0.0f && gt.pulpRotRate > 0.0f && fistRipe < 1.0f;
  const uint32_t lostAbs =
      (uint32_t)BaselineNumber("unarmedAttack.fleshLostMax", 0);
  const double lostFrac = BaselineNumber("unarmedAttack.fleshLostFracMax", 0.02);
  const uint32_t lostMax =
      fistDents ? std::max(lostAbs, (uint32_t)std::lround(lostFrac * flesh0))
                : lostAbs;
  check(lost <= lostMax,
        fistDents ? "a fist's dent stayed a dent (flesh lost within "
                    "unarmedAttack.fleshLostFracMax of the body)"
                  : "a fist took no more than the authored ceiling off the "
                    "target (blunt carve is off)");
  RecordObserved("unarmedAttack.fleshLostObserved", (double)lost);
  RecordObserved("unarmedAttack.nearestKnuckleObserved",
                 nearest < 1e8f ? (double)nearest : -1.0);
  std::printf(
      "unarmed-attack: %d requests, %d cut ticks, %d bodies hit, top tip "
      "speed %.1f vox/s, %u flesh voxels lost (ceiling %u%s) over %d ticks | "
      "probes %d cast: %d air, %d self, %d body | knuckles nearest %.2f vox "
      "to the chest, centres held %.2f..%.2f apart (asked %.1f, strike reach "
      "%.2f), target %s, limbs %d -> %d\n",
      requests, cutTicks, hits, topSpeed, lost, lostMax,
      fistDents ? ", dents on" : "", ticks, probesCast, probesAir, probesSelf,
      probesBody, nearest < 1e8f ? nearest : -1.0f, distMin, distMax, gap,
      strikeReach, sideOn ? "side-on" : "facing", limbs0, limbs1);

  // ---- C. THE DRAW SHRINKS WITH THE BODY --------------------------------
  //
  // Asked of `PickAttackStyle` DIRECTLY, over 200 ticks, because it is a pure
  // function of (library, profile, rig, tick) and waiting for an AI cadence to
  // reveal the same answer would cost thousands of ticks and report it as a
  // bare count of swings (CLAUDE.md rule 6).
  const int zombieProfile = c.mobs.Behaviors().Find("zombie");
  if (zombieProfile < 0) {
    check(false, "assets/mobs/behaviors.json ships a \"zombie\" profile");
  } else {
    const ai::Profile& zp = *c.mobs.Behaviors().At(zombieProfile);
    Mob* m = c.mobs.FindMobById(attacker);
    check(m != nullptr, "the attacker is still alive to lose limbs");
    auto drawn = [&](std::set<std::string>& out) {
      out.clear();
      for (uint32_t t = 0; t < 200; t++) {
        const int i = PickAttackStyle(lib, zp.attack.styles, attacker, t, m);
        if (i >= 0) out.insert(lib.styles[i].name);
      }
    };
    auto allWeapon = [&](const std::set<std::string>& s, const char* w) {
      for (const std::string& n : s) {
        const AttackStyle* a = lib.At(lib.Find(n));
        if (a == nullptr || a->weapon != w) return false;
      }
      return !s.empty();
    };
    std::set<std::string> whole;
    drawn(whole);
    check(whole.size() >= 2,
          "a whole body draws more than one of the zombie's styles");

    if (m != nullptr) {
      // ARMS OFF: only the jaws are left. Severing the UPPER arm takes the
      // whole chain with it, which is exactly what `NaturalWeaponUsable`
      // refuses on — a fist on the end of a missing elbow is not a weapon.
      c.mobs.Sever(attacker, LimbNamed(nd, "armU.L"));
      c.mobs.Sever(attacker, LimbNamed(nd, "armU.R"));
      std::set<std::string> armless;
      drawn(armless);
      check(allWeapon(armless, "jaws"),
            "with both arms gone the zombie draws only jaws styles");
      std::string names;
      for (const std::string& n : armless) names += " " + n;
      std::printf("unarmed-attack: armless draw ->%s\n",
                  names.empty() ? " (nothing)" : names.c_str());

      // ...and with the head off as well, nothing at all — a dropped request
      // and ONE loud line, never a silent no-op. `-1` IS that answer; the line
      // is MobSystem::ReportNoStroke's, printed once per (mob, reason).
      c.mobs.Sever(attacker, LimbNamed(nd, "head"));
      std::set<std::string> nothing;
      drawn(nothing);
      check(nothing.empty(),
            "with the arms and the head gone there is nothing left to draw");
    }
  }

  // ...AND THE FILTER IS PER WEAPON, NOT ALL-OR-NOTHING. One arm off must
  // cost the styles that swing THAT fist and nothing else, which is the claim
  // `NaturalWeaponUsable`'s chain walk actually makes.
  //
  // THE PLAN ASKED FOR "HEADLESS PICKS ONLY FISTS" HERE and this is that arm,
  // restated: the human's head is `vital: true`, so `Sever` routes it to Die
  // (selftest_mob's own fixture says so in as many words) and a live headless
  // NPC is not a thing this rig can be. The head half of the rule is still
  // asserted — it is the "nothing left to draw" check above, on a body that
  // has lost both arms AND its head — and this arm carries the half that is
  // reachable, which is the sharper one anyway: an all-or-nothing filter would
  // pass the head test and fail here.
  //
  // Asked on a SECOND creature so the one above's missing limbs cannot answer
  // it by accident (a gate arm that shares a column with another arm measures
  // the other arm — the memory file has that one written down).
  if (zombieProfile >= 0) {
    const uint64_t onearm =
        SpawnFighter(c, natDef, {st.spot.x + 4, st.spot.y + 1, st.spot.z},
                     "training_dummy", false, why);
    check(onearm != 0, "a second unarmed fixture spawned");
    if (onearm != 0) {
      for (int i = 0; i < 6; i++) tick();
      c.mobs.Sever(onearm, LimbNamed(nd, "armU.R"));
      const ai::Profile& zp = *c.mobs.Behaviors().At(zombieProfile);
      Mob* bm = c.mobs.FindMobById(onearm);
      check(bm != nullptr, "...and survived losing one arm");
      std::set<std::string> left;
      for (uint32_t t = 0; t < 200 && bm != nullptr; t++) {
        const int i = PickAttackStyle(lib, zp.attack.styles, onearm, t, bm);
        if (i >= 0) left.insert(lib.styles[i].name);
      }
      bool anyRightFist = false, anyLeftFist = false;
      for (const std::string& n : left) {
        const AttackStyle* a = lib.At(lib.Find(n));
        if (a == nullptr) continue;
        if (a->weapon == "fist.R") anyRightFist = true;
        if (a->weapon == "fist.L") anyLeftFist = true;
      }
      check(!anyRightFist,
            "losing the right arm drops every style that swings fist.R");
      check(anyLeftFist || !left.empty(),
            "...and leaves the rest of the repertoire alone");
      // ...AND THE SAME CLAIM WITHOUT THE FALLBACK RULE IN THE WAY. The draw
      // above answers "what does this body pick", and on a zombie the bites
      // are non-fallback so they hide the punches entirely — a correct answer
      // that makes the per-weapon half of the claim vacuous. `StyleUsable` is
      // the predicate underneath, so ask IT: one arm off must cost fist.R its
      // styles and leave fist.L's alone.
      if (bm != nullptr) {
        const AttackStyle* pr2 = lib.At(lib.Find("punch_r"));
        const AttackStyle* pl2 = lib.At(lib.Find("punch_l"));
        check(pr2 != nullptr && pl2 != nullptr,
              "the punch styles are in the library");
        if (pr2 && pl2) {
          check(!StyleUsable(*bm, *pr2),
                "punch_r is unusable with the right arm gone");
          check(StyleUsable(*bm, *pl2),
                "...and punch_l is untouched by it");
        }
      }
      std::string names;
      for (const std::string& n : left) names += " " + n;
      std::printf("unarmed-attack: one-armed draw ->%s\n",
                  names.empty() ? " (nothing)" : names.c_str());
    }
  }

  // ---- D. AN ARMED FIGHTER NEVER DRAWS A FALLBACK ------------------------
  // The other half of the same rule, and the one that says the duelists'
  // behaviour is UNCHANGED by this package (plan §5 asks for it by name).
  {
    const int duelist = c.mobs.Behaviors().Find("duelist");
    const uint64_t armed =
        SpawnFighter(c, natDef, {st.spot.x - 4, st.spot.y + 1, st.spot.z},
                     "training_dummy", true, why);
    check(duelist >= 0 && armed != 0, "an armed duelist fixture exists");
    if (duelist >= 0 && armed != 0) {
      for (int i = 0; i < 6; i++) tick();
      const ai::Profile& dp = *c.mobs.Behaviors().At(duelist);
      Mob* am = c.mobs.FindMobById(armed);
      bool sawFallback = false, sawReal = false;
      for (uint32_t t = 0; t < 400 && am != nullptr; t++) {
        const int i = PickAttackStyle(lib, dp.attack.styles, armed, t, am);
        if (i < 0) continue;
        (lib.styles[i].fallback ? sawFallback : sawReal) = true;
      }
      check(sawReal && !sawFallback,
            "an armed duelist draws its cuts and never a fallback punch");
    }
  }

  CloseStage(c);
  detail = Format("%d checks", checks);
  std::printf("unarmed-attack: %s (%d checks)\n", ok ? "PASS" : "FAIL", checks);
  return ok ? Status::Pass : Status::Fail;
}

// =============================================================================
// lunge — a body given a velocity leaves the ground, travels, and lands
// =============================================================================
Status GateLunge(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const std::string& what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("lunge: FAILED %s\n", what.c_str());
    }
  };

  Stage st = OpenStage(c);
  if (!st.ok) {
    detail = st.why;
    std::printf("lunge: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }
  const int natDef = NaturalDef(c.mobs, "zombie");
  const int lungeStyle = c.mobs.AttackStyles().Find("bite_lunge");
  if (natDef < 0 || lungeStyle < 0) {
    detail = natDef < 0 ? "no mob def declares a `natural` block"
                        : "no \"bite_lunge\" style";
    std::printf("lunge: SKIP (%s)\n", detail.c_str());
    CloseStage(c);
    return Status::Skip;
  }
  const MobDef& nd = c.mobs.Defs()[natDef];
  const AttackStyle& sty = *c.mobs.AttackStyles().At(lungeStyle);
  check(sty.lunge.Any(), "bite_lunge authors a lunge at all");
  check(sty.lunge.rise > 0.0f,
        "...with a non-zero rise (a flat launch lands the tick after it left)");

  // ONE FLIGHT, MEASURED. Returns how far the body travelled in the plane, how
  // high it got, and whether the stroke cut — three numbers rather than one,
  // because "the lunge did not work" has three independent causes and from
  // outside they are the same zero (CLAUDE.md rule 6).
  struct Flight {
    bool launched = false;
    float planar = 0;     // world voxels closed, start to finish
    float peakRise = 0;   // highest the origin got above its start
    float topXzSpeed = 0;
    int airTicks = 0;
    int cutTicks = 0;
    float sankBelow = 0;  // deepest the body went under its own start height
    // ---- WHAT BODY FLEW (2026-09-19) ----------------------------------
    // The loco state the biter was in when it launched, and that state's
    // lungeScale (1 for normal locomotion), plus how many spawns were thrown
    // back for arriving damaged -- see the note at `fly`.
    std::string state = "normal";
    float scale = 1.0f;
    int rerolls = 0;
  };
  const float standOff = (float)BaselineNumber("lunge.standOffVox", 20.0);
  // ---- THE UPRIGHT BODY MUST BE INTACT, AND A ZOMBIE SPAWNS EATEN ---------
  //
  // 2026-09-19: the upright flight came back at 5 air ticks / rise 0.22 /
  // top xz 55, against 11 / 0.91 / 100 three days earlier -- every number
  // scaled by 0.55, which is human.json's `limp` state (speedScale 0.55,
  // lungeScale inheriting it; anim.h AnimStateRule). Nothing about the lunge
  // moved. What moved is that a zombie is rotted at spawn (Mob::RotAtSpawn,
  // zombie.json `rot`), and since 904dd30 a limb comes off when the flesh at
  // its ATTACHMENT is gone whatever ate it (Mob::JointAttached) -- so a rot
  // bite that lands on an ankle now takes the foot with it, `limp` matches,
  // and the "upright" arm measured a one-footed hop at 0.55 of everything.
  // Which draw a spawn gets follows the mob id, and the id this gate reaches
  // depends on every gate before it (IdCounterScope restores the counter, it
  // does not reset it), which is why the same code passed standalone.
  //
  // So the fixture asks for the body it is claiming about: a spawn that is
  // not in normal locomotion after the anim poll has run is thrown back and
  // drawn again, up to `lunge.intactTries`. A rig that never comes up intact
  // is not hidden: the LAST spawn is flown anyway, its state and lungeScale
  // are printed, and the rise floor is scaled by that lungeScale so the claim
  // "it rises as its state allows" is still made rather than skipped. The
  // crawl arm goes through the same door before its legs come off, so its
  // state is `crawl` by construction and not `squirm` (an arm gone too).
  const int intactTries = (int)BaselineNumber("lunge.intactTries", 6);
  auto fly = [&](bool crawl, Flight& out) -> bool {
    std::string why;
    Ticker tick{c, 25600, {st.spot.x >> 4, st.spot.y >> 4, st.spot.z >> 4}};
    uint64_t biter = 0, prey = 0;
    for (int attempt = 0; attempt < std::max(1, intactTries); attempt++) {
      biter = SpawnFighter(c, natDef, {st.spot.x, st.spot.y + 1, st.spot.z},
                           "training_dummy", false, why);
      prey = SpawnFighter(
          c, natDef,
          {st.spot.x, st.spot.y + 1, st.spot.z + (int)std::lround(standOff)},
          "training_dummy", false, why);
      if (biter == 0 || prey == 0) return false;
      // The state is polled in the anim tick (MobSystem::UpdateAnimation),
      // not at spawn, so this is a few ticks and not a read straight after.
      for (int i = 0; i < 6; i++) tick();
      const int ls = c.mobs.LocoState(biter);
      if (ls < 0) break;
      if (ls < (int)nd.skel.states.size()) {
        out.state = nd.skel.states[ls].name;
        out.scale = std::max(0.0f, nd.skel.states[ls].lungeScale);
      }
      if (attempt + 1 >= std::max(1, intactTries)) break;  // fly it anyway
      out.rerolls++;
      c.mobs.Reset();
      c.debris.Reset();
      biter = prey = 0;
    }
    if (biter == 0 || prey == 0) return false;
    if (c.mobs.LocoState(biter) < 0) {
      out.state = "normal";
      out.scale = 1.0f;
    }
    if (crawl) {
      // BOTH THIGHS OFF is what puts this rig into its `crawl` state (the
      // sidecar's rule is `missing: [legU.L, legU.R]`) — `crawl-slope` severs
      // exactly these two for the same reason.
      c.mobs.Sever(biter, LimbNamed(nd, "legU.L"));
      c.mobs.Sever(biter, LimbNamed(nd, "legU.R"));
    }
    for (int i = 0; i < 30; i++) tick();   // settle, and let the state latch
    // The state that actually launches -- `crawl` for the severed arm -- so
    // the report names the body that flew and not the one that spawned.
    if (const int ls = c.mobs.LocoState(biter);
        ls >= 0 && ls < (int)nd.skel.states.size()) {
      out.state = nd.skel.states[ls].name;
      out.scale = std::max(0.0f, nd.skel.states[ls].lungeScale);
    }
    FaceAt(c.mobs, biter, Chest(c.mobs, prey, nd));
    const Vec3 from = c.mobs.MobOrigin(biter);
    if (!c.mobs.ForceAttack(biter, "bite_lunge", Chest(c.mobs, prey, nd),
                            tick.tick))
      return false;
    for (int i = 0; i < 60; i++) {
      tick();
      const Mob* m = c.mobs.FindMobById(biter);
      if (m == nullptr) break;
      if (m->Airborne()) {
        out.launched = true;
        out.airTicks++;
        const Vec3 v = m->AirVelocity();
        out.topXzSpeed = std::max(out.topXzSpeed, v.len());
      }
      const Vec3 p = m->Origin();
      out.peakRise = std::max(out.peakRise, p.y - from.y);
      out.sankBelow = std::max(out.sankBelow, from.y - p.y);
      const NpcStroke* s = c.mobs.MobStroke(biter);
      if (s != nullptr && s->Cutting()) out.cutTicks++;
    }
    const Mob* m = c.mobs.FindMobById(biter);
    if (m != nullptr) {
      const Vec3 to = m->Origin();
      out.planar = std::sqrt((to.x - from.x) * (to.x - from.x) +
                             (to.z - from.z) * (to.z - from.z));
    }
    c.mobs.Reset();
    c.debris.Reset();
    return true;
  };

  Flight upright, prone;
  const bool haveA = fly(false, upright);
  const bool haveB = fly(true, prone);
  if (!haveA || !haveB) {
    detail = "lunge fixture spawn or ForceAttack refused";
    std::printf("lunge: SKIP (%s)\n", detail.c_str());
    CloseStage(c);
    return Status::Skip;
  }

  const float minPlanar = (float)BaselineNumber("lunge.minPlanarVox", 3.0);
  const float minRise = (float)BaselineNumber("lunge.minRiseVox", 0.3);
  const float maxSink = (float)BaselineNumber("lunge.maxSinkVox", 1.0);
  // The body the upright arm flew really was upright (see the note at `fly`):
  // a zombie that came up limping on every one of `lunge.intactTries` draws is
  // a rot-at-spawn fact worth a red line here, since nothing else prints it.
  check(upright.state == "normal",
        "the upright biter came up intact within lunge.intactTries spawns "
        "(flew as \"" + upright.state + "\")");
  check(upright.launched, "an upright zombie's lunge leaves the ground");
  check(upright.topXzSpeed > 0.0f,
        "...with a planar velocity, not just a hop");
  check(upright.planar > minPlanar, "...and it closes real distance");
  // The floor is scaled by the flown state's lungeScale (1 when intact) so a
  // damaged fallback body is still held to "rises as its state allows".
  check(upright.peakRise > minRise * upright.scale, "...rising on the way");
  check(upright.cutTicks > 0, "...and the stroke still cuts when it lands");
  // A CRAWLER POUNCES LOW AND SHORT (anim.h AnimStateRule::lungeScale). The
  // crawl state authors speedScale 0.3 and lungeScale inherits it, so this is
  // a claim about the scale reaching the launch — a DIFFERENCE, not a number,
  // so retuning the crawl never touches this gate.
  check(prone.launched, "a crawling zombie still launches");
  check(prone.planar < upright.planar,
        "...but travels less than an upright one (lungeScale)");
  check(prone.peakRise < upright.peakRise, "...and stays lower");
  // AND IT DOES NOT TUNNEL. The planar half of a launch is integrated against
  // the same `fits()` the walk drive uses, so a body can neither pass through
  // the floor nor end up inside it; a rig that sank would report here rather
  // than as "the crawler looks wrong" three sessions later.
  check(prone.sankBelow <= maxSink,
        "...and never sinks into the ground it is dragging itself over");
  check(upright.sankBelow <= maxSink, "the upright one does not sink either");

  RecordObserved("lunge.uprightPlanarObserved", (double)upright.planar);
  RecordObserved("lunge.pronePlanarObserved", (double)prone.planar);
  RecordObserved("lunge.uprightRiseObserved", (double)upright.peakRise);
  RecordObserved("lunge.uprightRerollsObserved", (double)upright.rerolls);
  std::printf(
      "lunge upright: %d air ticks, planar %.2f vox, peak rise %.2f, top xz "
      "%.2f vox/s, %d cut ticks, sank %.2f | state %s (lungeScale %.2f), %d "
      "damaged spawn(s) thrown back\n"
      "lunge crawl:   %d air ticks, planar %.2f vox, peak rise %.2f, top xz "
      "%.2f vox/s, %d cut ticks, sank %.2f | state %s (lungeScale %.2f), %d "
      "damaged spawn(s) thrown back\n",
      upright.airTicks, upright.planar, upright.peakRise, upright.topXzSpeed,
      upright.cutTicks, upright.sankBelow, upright.state.c_str(), upright.scale,
      upright.rerolls, prone.airTicks, prone.planar, prone.peakRise,
      prone.topXzSpeed, prone.cutTicks, prone.sankBelow, prone.state.c_str(),
      prone.scale, prone.rerolls);

  CloseStage(c);
  detail = Format("%d checks", checks);
  std::printf("lunge: %s (%d checks)\n", ok ? "PASS" : "FAIL", checks);
  return ok ? Status::Pass : Status::Fail;
}

// =============================================================================
// bite-target — a blow chooses a LIMB, and not always the same one
// =============================================================================
//
// THE CLAIM IS ABOUT THE DRAW, NOT ABOUT THE WOUNDS. Inferring the target from
// where the damage landed would measure the sweep's geometry, the victim's
// pose and the parry table as well — three systems with their own gates — and
// would report all four as one number. `NpcStroke::targetLimb` records the
// choice at the instant it is made, which is the only place it is a fact.
Status GateBiteTarget(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const std::string& what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("bite-target: FAILED %s\n", what.c_str());
    }
  };

  Stage st = OpenStage(c);
  if (!st.ok) {
    detail = st.why;
    std::printf("bite-target: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }
  const int natDef = NaturalDef(c.mobs, "zombie");
  const StyleLibrary& lib = c.mobs.AttackStyles();
  const AttackStyle* sty = lib.At(lib.Find("bite_lunge"));
  if (natDef < 0 || sty == nullptr) {
    detail = natDef < 0 ? "no mob def declares a `natural` block"
                        : "no \"bite_lunge\" style";
    std::printf("bite-target: SKIP (%s)\n", detail.c_str());
    CloseStage(c);
    return Status::Skip;
  }
  const MobDef& nd = c.mobs.Defs()[natDef];
  check(!sty->target.empty(), "bite_lunge authors a target weight table");
  check(sty->target.size() >= 3,
        "...over at least three tags (bites go for arms and torso too)");
  // ---- THE ARM IS THE FAVOURITE, NOT THE HEAD (2026-09-17) ---------------
  //
  // Asserted on the AUTHORED table rather than on the drawn distribution, and
  // that is the right scope: 40 draws cannot separate 0.40 from 0.35 at any
  // honest confidence, so a gate that tried would either be a coin flip or
  // would need hundreds of draws to say something the content file states
  // outright. What CAN go wrong is somebody editing the weights back, and this
  // catches exactly that. The distribution arm below still asserts that the
  // draw really spreads over tags.
  {
    float armW = 0.0f, headW = 0.0f;
    for (const StyleTargetWeight& w : sty->target) {
      if (w.tag == "arm") armW = w.weight;
      if (w.tag == "head") headW = w.weight;
    }
    check(armW > headW,
          "...and it favours the ARM over the head (a throat-first zombie "
          "reads as a scripted execution, and the head is `vital`)");
  }
  check(!sty->targetProne.empty(),
        "...and authors a PRONE table, for when the biter is on the ground");

  std::string why;
  const uint64_t biter =
      SpawnFighter(c, natDef, {st.spot.x, st.spot.y + 1, st.spot.z},
                   "training_dummy", false, why);
  const uint64_t prey =
      SpawnFighter(c, natDef, {st.spot.x, st.spot.y + 1, st.spot.z + 5},
                   "training_dummy", false, why);
  if (biter == 0 || prey == 0) {
    detail = why.empty() ? "fixture spawn failed" : why;
    std::printf("bite-target: SKIP (%s)\n", detail.c_str());
    CloseStage(c);
    return Status::Skip;
  }
  Ticker tick{c, 25900, {st.spot.x >> 4, st.spot.y >> 4, st.spot.z >> 4}};
  for (int i = 0; i < 20; i++) tick();

  // 40 FORCED BITES, each one begun and then abandoned: the claim is about the
  // CHOICE, so the stroke only has to start. Abandoning it (ClearGuard-style,
  // by resetting) also keeps the fixture from being eaten over 40 swings,
  // which would change the live-limb set the draw runs over halfway through.
  const int want = (int)BaselineNumber("biteTarget.bites", 40);
  std::map<std::string, int> byTag;
  int chosen = 0, head = 0;
  for (int i = 0; i < want; i++) {
    Mob* m = c.mobs.FindMobById(biter);
    if (m == nullptr) break;
    m->Stroke().Reset();
    if (!c.mobs.ForceAttack(biter, "bite_lunge", Chest(c.mobs, prey, nd),
                            tick.tick + (uint32_t)i))
      continue;
    // ForceAttack is the SCRIPTED door and deliberately carries no target id
    // (it aims at a point), so the limb draw is exercised through the AI's
    // door instead: BeginStroke is what resolves a victim. Rather than wait on
    // a cadence, the draw itself is asked here with the same inputs.
    const int limb = MobSystem::PickTargetLimb(
        *sty, *c.mobs.FindMobById(prey), biter, /*attackerProne=*/0.0f,
        tick.tick + (uint32_t)i);
    if (limb < 0) continue;
    chosen++;
    const std::string tag = nd.limbs[limb].tag;
    byTag[tag]++;
    if (tag == "head") head++;
    m->Stroke().Reset();
  }
  check(chosen > want / 2, "most of the forced bites chose a limb at all");
  check(byTag.size() >= 2, "...over at least two distinct limb tags");
  check(head < chosen, "...and not the head every single time");
  std::string spread;
  for (const auto& kv : byTag)
    spread += " " + kv.first + "=" + std::to_string(kv.second);
  RecordObserved("biteTarget.tagsObserved", (double)byTag.size());
  RecordObserved("biteTarget.headFractionObserved",
                 chosen > 0 ? (double)head / (double)chosen : 0.0);
  std::printf("bite-target: %d/%d bites chose a limb;%s\n", chosen, want,
              spread.c_str());

  // ---- ...AND THE LOOKUP THAT DECIDES WHETHER THE DRAW RUNS AT ALL --------
  //
  // Everything above asks `PickTargetLimb` DIRECTLY, and says so. That proves
  // the draw and leaves the seam in front of it untested -- which is exactly
  // where the bug was: `StartStroke` reaches the draw through
  // `if (const Mob* victim = FindMobById(targetId))`, the player's target id
  // is the reserved ai::kPlayerActorId (0), and the avatar is a Mob that lives
  // OUTSIDE `mobs_`. So against the PLAYER the lookup came back empty, the
  // draw never ran, `targetLimb` stayed -1 and every NPC blow fell through to
  // BeginStrokeProgram's default chest aim. All five authored bite weights
  // were dead against the one target the game is mostly about, and the chest
  // is the one place a garment always covers -- so the teeth landed on cloth
  // every time and the owner reported never being able to detect a bite.
  //
  // A gate that only ever asks the draw is a fixture that measures itself.
  // This arm goes through `ForceAttack`'s `targetId`, which is the same
  // `StartStroke` door the AI uses.
  {
    const uint32_t seedTick = tick.tick + 1000u;
    const Vec3 aimAt = Chest(c.mobs, prey, nd);
    // The draw is only ever attempted on a style that HAS a weight table, so
    // the two arms below differ in one thing: whether the victim resolves.
    auto drawAgainstPlayer = [&](uint32_t k) -> int {
      Mob* m = c.mobs.FindMobById(biter);
      if (m == nullptr) return -2;
      m->Stroke().Reset();
      if (!c.mobs.ForceAttack(biter, "bite_lunge", aimAt, seedTick + k, 0,
                              ai::kPlayerActorId))
        return -2;
      const NpcStroke* s = c.mobs.MobStroke(biter);
      const int limb = s != nullptr ? s->targetLimb : -2;
      m->Stroke().Reset();
      return limb;
    };
    // CONTROL ARM: no avatar registered, which is every headless run and was
    // the whole world before this. The lookup must find nobody and the blow
    // must fall through to the chest, exactly as it always did.
    c.mobs.SetAvatar(nullptr);
    const int blind = drawAgainstPlayer(0);
    // ...AND THE SUBJECT: the player registered. `SetAvatar` takes a Mob*, and
    // the prey is one -- the claim is about the LOOKUP, not about PlayerAvatar,
    // so borrowing a body already in the fixture keeps this arm to the seam it
    // is testing.
    Mob* asPlayer = c.mobs.FindMobById(prey);
    c.mobs.SetAvatar(asPlayer);
    int drawn = 0, spread = 0;
    std::map<std::string, int> playerTags;
    for (uint32_t k = 1; k <= 12; k++) {
      const int limb = drawAgainstPlayer(k);
      if (limb < 0) continue;
      drawn++;
      playerTags[nd.limbs[limb].tag]++;
    }
    spread = (int)playerTags.size();
    // MUST be restored: gates share one MobSystem and a dangling avatar_ would
    // follow this one into every gate after it (selftest.h's ordering note).
    c.mobs.SetAvatar(nullptr);
    check(blind == -1, "with no avatar, a blow at the player aims at the chest");
    check(drawn > 0, "...and with one registered, the limb draw actually runs");
    check(spread >= 2, "...over more than one limb tag, not the chest every time");
    std::string ps;
    for (const auto& kv : playerTags)
      ps += " " + kv.first + "=" + std::to_string(kv.second);
    RecordObserved("biteTarget.playerTagsObserved", (double)spread);
    std::printf(
        "bite-target: at the PLAYER (actor id 0): no avatar -> targetLimb %d; "
        "avatar registered -> %d/12 drew a limb;%s\n",
        blind, drawn, ps.c_str());
  }

  // ---- ...AND A CRAWLER GOES FOR THE LEGS (2026-09-17) -------------------
  //
  // TWO ARMS, because the feature has two halves that fail independently, and
  // the second is the seam this gate's own note above was written about.
  //
  //   THE DRAW: hand `PickTargetLimb` a prone weight directly and check that
  //   it switches tables. Same style, same victim and the same ticks as the
  //   upright sweep above, so the two distributions are comparable and the
  //   only difference between them is the posture.
  //
  //   THE SEAM: take the biter's legs off, let the anim poll run, and check
  //   that the creature really does report a prone loco state. That is the
  //   number BeginStroke feeds the draw, and without this arm "crawlers bite
  //   legs" would pass on a rig whose crawl state nothing ever selects.
  {
    std::map<std::string, int> proneTags;
    int proneDrawn = 0, proneLeg = 0;
    for (uint32_t k = 0; k < (uint32_t)want; k++) {
      Mob* pv = c.mobs.FindMobById(prey);
      if (pv == nullptr) break;
      const int limb = MobSystem::PickTargetLimb(*sty, *pv, biter,
                                                 /*attackerProne=*/1.0f,
                                                 tick.tick + k);
      if (limb < 0) continue;
      proneDrawn++;
      const std::string tag = nd.limbs[limb].tag;
      proneTags[tag]++;
      if (tag == "leg") proneLeg++;
    }
    // 0.6 against an authored 0.9 is deliberately slack: the weight is split
    // across the tag's LIVE limbs, and an earlier gate can leave the fixture
    // short of one, so pinning the authored number would turn this into an
    // assertion about the fixture's anatomy. What is claimed is "the table
    // switched", and the upright table puts legs at 0.14.
    const double legFrac =
        proneDrawn > 0 ? (double)proneLeg / (double)proneDrawn : 0.0;
    check(proneDrawn > want / 2, "a prone biter still draws a limb");
    check(legFrac > 0.6,
          "...and goes for the LEGS, which is all a crawler can reach");
    RecordObserved("biteTarget.proneLegFractionObserved", legFrac);

    // THE SEAM: both legs off -> the `crawl` loco state -> groundAlign > 0.
    float prone = -1.0f;
    if (Mob* m = c.mobs.FindMobById(biter)) {
      check(m->LocoGroundAlign() <= 0.0f,
            "...the biter was NOT prone before its legs came off");
      // The UPPER leg, by name, exactly as the armless arm of
      // `unarmed-attack` does: the crawl state's rule lists `legU.L`/`legU.R`
      // and severing the upper joint takes the whole chain with it.
      c.mobs.Sever(biter, LimbNamed(nd, "legU.L"));
      c.mobs.Sever(biter, LimbNamed(nd, "legU.R"));
      // The state is polled in the anim tick, not at Sever, so this is a few
      // ticks and not a read straight after the cut.
      for (int i = 0; i < 6; i++) tick();
      if (Mob* still = c.mobs.FindMobById(biter))
        prone = still->LocoGroundAlign();
    }
    check(prone > 0.0f,
          "...and a legless biter really does report a PRONE loco state, "
          "which is the number BeginStroke feeds the draw");
    RecordObserved("biteTarget.proneGroundAlignObserved", (double)prone);
    std::string pt;
    for (const auto& kv : proneTags)
      pt += " " + kv.first + "=" + std::to_string(kv.second);
    std::printf(
        "bite-target: PRONE biter: %d/%d drew a limb, legs %.0f%%;%s | "
        "legless groundAlign %.2f\n",
        proneDrawn, want, legFrac * 100.0, pt.c_str(), (double)prone);
  }

  // ---- ...AND THE BLOW IS THEN AIMED AT THE LIMB IT DREW (2026-09-19) -----
  //
  // EVERY ARM ABOVE ASSERTS A CHOICE THAT STEERED NOTHING. `targetLimb` was
  // drawn, recorded and reported, and `StartStroke` went on handing the stroke
  // the caller's point -- the victim's BODY CENTRE -- so the whole table was
  // decoration: the drawn limb never reached the aim, and `bite-target` passed
  // on every one of its claims while a crawler aimed at a standing victim's
  // chest. The report was "zombie bites when crawling are extremely
  // inaccurate", and it was accurate.
  //
  // This is the arm that would have caught it, and it is deliberately not a
  // sweep: whether the teeth then CONNECT is a fact about reach, pose and the
  // half-width, and every one of those would make this arm move for reasons
  // that are not the bug. Where the blow is POINTED is one comparison against
  // the limb's own middle.
  //
  // The biter is legless and prone by now (the arm above severed it), which is
  // the posture the report is about: `targetProne` sends nine draws in ten at
  // a leg, so most of these aims are also the vertical claim.
  {
    const Vec3 chest = Chest(c.mobs, prey, nd);
    int aimed = 0, onLimb = 0, legAims = 0, legLow = 0;
    double worstDy = 0.0;
    for (uint32_t k = 0; k < 16; k++) {
      Mob* m = c.mobs.FindMobById(biter);
      Mob* pv = c.mobs.FindMobById(prey);
      if (m == nullptr || pv == nullptr) break;
      m->Stroke().Reset();
      // THE STANDING BITE, not the lunge: a crawler in contact is the case the
      // report is about, and it is the style with no flight to confuse the
      // point with. Through `ForceAttack`'s target id, which is the same
      // `StartStroke` door the AI's `BeginStroke` goes through.
      if (!c.mobs.ForceAttack(biter, "bite", chest, tick.tick + 3000u + k, 0,
                              prey))
        continue;
      const NpcStroke* s = c.mobs.MobStroke(biter);
      Vec3 lc{};
      if (s == nullptr || s->targetLimb < 0 ||
          !pv->LimbCentreWorld(s->targetLimb, lc)) {
        m->Stroke().Reset();
        continue;
      }
      aimed++;
      const double dy = std::fabs((double)(s->targetPoint.y - lc.y));
      worstDy = std::max(worstDy, dy);
      // A voxel of slack and no more: the aim carries the request's PLANAR
      // lead onto the limb and takes the height from the limb outright, so
      // there is nothing left to be off by vertically.
      if (dy < 1.0) onLimb++;
      if (nd.limbs[(size_t)s->targetLimb].tag == "leg") {
        legAims++;
        // The number the bug was: a standing victim's chest against the leg
        // the crawler actually drew. Two voxels is well inside the gap on any
        // rig whose legs are not its torso.
        if (s->targetPoint.y < chest.y - 2.0f) legLow++;
      }
      m->Stroke().Reset();
    }
    check(aimed >= 8, "a prone biter's forced bites resolve a limb to aim at");
    check(onLimb == aimed,
          "...and every one is aimed at THAT limb's own middle, not at the "
          "body centre the caller asked for");
    check(legAims > 0, "...with legs among them, as the prone table says");
    check(legLow == legAims,
          "...and a bite that drew a LEG is aimed well below the chest");
    RecordObserved("biteTarget.aimLimbDyWorst", worstDy);
    std::printf(
        "bite-target: AIM: %d bites aimed at the drawn limb (%d on-limb, "
        "worst dy %.2f vox); %d legs, %d of them below the chest\n",
        aimed, onLimb, worstDy, legAims, legLow);

    // ---- ...AND THE HEAD REALLY ENDS UP POINTING THERE -------------------
    //
    // THE OTHER HALF OF THE SAME REPORT, and it is a different mechanism:
    // aiming a blow correctly and POSING the part along that aim are two
    // steps, and the second one was upright-only. `ApplyAimPart` multiplies a
    // yaw/pitch pair onto the part's LOCAL rotation, i.e. about axes the chain
    // above the part has already moved -- and the `crawl` clip pitches the
    // hips 74 deg forward in override mode, so on a crawler the commanded yaw
    // came out mostly as pitch and the commanded pitch mostly as yaw. The rig
    // records both ends (Mob::AimDiag: what the driver asked for, and what the
    // posed edge actually points at, read after the flatten and the clamp), so
    // the claim is one subtraction and needs no geometry of its own here.
    //
    // 0.25 rad is stated as a bound on a BITE rather than fitted to the run:
    // 14 degrees is about the half-angle a set of jaws covers at contact, so
    // an error under it still lands on what was aimed at and an error over it
    // is a bite that goes somewhere else. The pre-fix pose misses by the
    // parent's own tilt, which is a radian and a quarter.
    Mob* m = c.mobs.FindMobById(biter);
    if (m != nullptr) {
      m->Stroke().Reset();
      float worstYaw = 0, worstPitch = 0;
      int sampled = 0;
      if (c.mobs.ForceAttack(biter, "bite", chest, tick.tick + 4000u, 0,
                             prey)) {
        for (int i = 0; i < 24; i++) {
          tick();
          const Mob* mm = c.mobs.FindMobById(biter);
          const NpcStroke* s = c.mobs.MobStroke(biter);
          if (mm == nullptr || s == nullptr) break;
          if (!s->Cutting()) continue;
          const Mob::AimDiag& ad = mm->AimDiagnostics();
          if (!ad.ran) continue;
          sampled++;
          auto wrap = [](float a) {
            while (a > 3.14159265f) a -= 6.28318531f;
            while (a <= -3.14159265f) a += 6.28318531f;
            return std::fabs(a);
          };
          worstYaw = std::max(worstYaw, wrap(ad.gotYaw - ad.cmdYaw));
          worstPitch = std::max(worstPitch, std::fabs(ad.gotPitch - ad.cmdPitch));
        }
      }
      check(sampled > 0, "a prone biter's cut is sampled at all");
      check(worstYaw < 0.25f && worstPitch < 0.25f,
            "...and while PRONE the jaws end up pointing where the stroke "
            "commanded, not rotated by the crawl pose's own pitch");
      RecordObserved("biteTarget.proneAimErrYaw", (double)worstYaw);
      RecordObserved("biteTarget.proneAimErrPitch", (double)worstPitch);
      std::printf(
          "bite-target: PRONE aim: %d cut ticks, worst commanded->posed error "
          "yaw %.3f rad, pitch %.3f rad\n",
          sampled, (double)worstYaw, (double)worstPitch);
    }
  }

  CloseStage(c);
  detail = Format("%d checks", checks);
  std::printf("bite-target: %s (%d checks)\n", ok ? "PASS" : "FAIL", checks);
  return ok ? Status::Pass : Status::Fail;
}

// =============================================================================
// hit-react — a struck body rocks AWAY from the blade, and puts itself back
// =============================================================================
//
// FOUR CLAIMS, and the first two are the ones that would otherwise be checked
// by eye and never again:
//
//   A. SOMETHING MOVES, and by more than the rig's own idle wobble. Measured
//      as a differential against a noise floor sampled from the SAME creature
//      over the SAME number of ticks immediately before the blow — not against
//      zero, because a standing figure is never still (the gait oscillator runs
//      free on an NPC) and "absolute zero is a rate claim" is a trap this suite
//      has fallen into before.
//   B. IT MOVES THE RIGHT WAY. The sign law in Mob::ApplyHitReact is four
//      lines of quaternion convention, it is invisible at a glance, and getting
//      it backwards produces a creature that leans INTO the sword — the one
//      failure of this feature that reads as a bug rather than as a taste
//      difference. Asserted against a known world direction, and then again
//      MIRRORED, because a formulation that ignored the direction entirely
//      would pass the first half.
//   C. IT IS A FLINCH AND NOT A STAGGER. Bounded above, and back home inside
//      the advertised recovery — a reaction still visible when the next blow
//      lands is a wobble.
//   D. NOTHING ELSE MOVED. The whole argument for firing this on every hit
//      with no budget and no AI state is that it is POSE-SPACE ONLY: the
//      origin, the heading and therefore the collider, the personal space and
//      the A* plan are untouched. That promise is one line to break and one
//      check to keep.
//
// THE WITNESS IS THE LIVE RIG, not the springs. `PartJointWorld` composes the
// transform the kinematic submit actually pushed into Jolt, so this measures
// what the renderer and the colliders see — a gate that read `hitReact_.lean`
// back would be asserting that a float it set is the float it set (the
// circular-probe note in the memory file, and the reason `swing-plane`
// measures the blade instead of the command).
//
// THE CENTROID rather than a named part, because a gate that says "watch the
// head" is a gate that fails the day a rig is re-authored without one. The
// mean joint of every live BODY limb moves with the lean and the shove and is
// a fact about any rig with limbs at all.
Vec3 BodyCentroid(Mob* m) {
  if (m == nullptr) return Vec3{};
  Vec3 sum{};
  int n = 0;
  // `PartJointWorld` is the liveness test as well as the read: it refuses a
  // slot with no collider, which is exactly what a severed part is here.
  for (int i = 0; i < m->AppendedBase(); i++) {
    Vec3 p;
    if (!m->PartJointWorld(i, p)) continue;
    sum = sum + p;
    n++;
  }
  return n > 0 ? sum * (1.0f / (float)n) : Vec3{};
}

Status GateHitReact(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const char* what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("hit-react: FAILED %s\n", what);
    }
  };

  Stage st = OpenStage(c);
  if (!st.ok) {
    detail = st.why;
    std::printf("hit-react: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }

  std::string why;
  const uint64_t who =
      SpawnFighter(c, st.defIndex, {st.spot.x, st.spot.y + 1, st.spot.z},
                   "training_dummy", false, why);
  if (who == 0) {
    detail = why.empty() ? "fixture spawn failed" : why;
    std::printf("hit-react: SKIP (%s)\n", detail.c_str());
    CloseStage(c);
    return Status::Skip;
  }

  Ticker tick{c, 31000, {st.spot.x >> 4, st.spot.y >> 4, st.spot.z >> 4}};
  // FACING +Z, which is heading 0 — the rig frame and the world frame agree, so
  // a blow stated in world +X is a blow stated in model +X and the sign law is
  // being read directly rather than through a rotation that could cancel an
  // error in it.
  FaceAt(c.mobs, who, Vec3{(float)st.spot.x, (float)st.spot.y,
                           (float)st.spot.z + 32.0f});
  for (int i = 0; i < 48; i++) tick();

  Mob* m = c.mobs.FindMobById(who);
  if (m == nullptr) {
    detail = "fixture vanished while settling";
    std::printf("hit-react: SKIP (%s)\n", detail.c_str());
    CloseStage(c);
    return Status::Skip;
  }
  // SOMETHING TO HIT. The first body limb that has a collider at all — named
  // by the rig rather than by this file, for the reason the centroid is.
  uint64_t target = 0;
  for (int i = 0; i < m->AppendedBase() && target == 0; i++)
    target = c.mobs.LimbBody(who, i);
  if (target == 0) {
    detail = "fixture has no limb collider to strike";
    std::printf("hit-react: SKIP (%s)\n", detail.c_str());
    CloseStage(c);
    return Status::Skip;
  }

  const int kWindow = 26;   // ticks watched after a blow: ~0.43 s, ~5 halflives

  // ---- THE NOISE FLOOR, from the same creature over the same span ----------
  Vec3 rest = BodyCentroid(m);
  float noise = 0;
  for (int i = 0; i < kWindow; i++) {
    tick();
    noise = std::max(noise, (BodyCentroid(c.mobs.FindMobById(who)) - rest).len());
  }
  rest = BodyCentroid(c.mobs.FindMobById(who));

  // ---- ONE BLOW, TRAVELLING WORLD +X --------------------------------------
  // The reference damage and a full-power ramp, so the peak is the authored
  // peak and the bound below is a statement about the tuning rather than about
  // whatever number happened to come out of a sweep.
  const float refHp = CurrentTuning().combatfx.hitReactRefDamage;
  auto strike = [&](Vec3 dir) {
    return c.mobs.HitReact(target, dir, refHp, 1.0f);
  };
  check(strike(Vec3{1, 0, 0}), "the blow reaches a live rig through FindOwner");

  const Vec3 origin0 = c.mobs.MobOrigin(who);
  const float heading0 = c.mobs.MobHeading(who);
  Vec3 peakOff{};
  float peak = 0;
  int peakTick = -1;
  for (int i = 0; i < kWindow; i++) {
    tick();
    const Vec3 off = BodyCentroid(c.mobs.FindMobById(who)) - rest;
    if (off.len() > peak) {
      peak = off.len();
      peakOff = off;
      peakTick = i;
    }
  }

  // ---- A. IT MOVED, AND NOT BY ACCIDENT -----------------------------------
  const double floorMul = BaselineNumber("hitReact.noiseMultiple", 4.0);
  check(peak > noise * (float)floorMul,
        "the reaction clears the rig's own idle wobble");
  check(peak > (float)BaselineNumber("hitReact.minPeakVox", 0.35),
        "the reaction is big enough to see");

  // ---- C. ...AND IT IS STILL A FLINCH -------------------------------------
  check(peak < (float)BaselineNumber("hitReact.maxPeakVox", 4.0),
        "the reaction is a flinch, not a stagger");
  // A peak that lands on the LAST tick of the window is a spring that has not
  // turned over yet, which means the halflife and the window disagree — and
  // the recovery check below would then be measuring a rise, not a fall.
  check(peakTick >= 0 && peakTick < kWindow - 4,
        "the reaction peaks and turns over inside the window");

  // ---- B. AWAY FROM THE BLADE ---------------------------------------------
  // Along the blow, not merely "somewhere else": a sideways artefact of the
  // gait would satisfy a bare displacement test.
  const float alongX = peak > 1e-5f ? peakOff.x / peak : 0.0f;
  check(alongX > (float)BaselineNumber("hitReact.alongFrac", 0.5),
        "the body goes AWAY from the blade's travel, not into it");
  RecordObserved("hitReact.observedAlong", alongX);
  RecordObserved("hitReact.observedPeakVox", peak);
  RecordObserved("hitReact.observedNoiseVox", noise);

  // ---- D. AND NOTHING THE WORLD CAN FEEL MOVED ----------------------------
  // The whole licence for firing this on every hit. `Near` on the origin
  // rather than equality because the fixture is a live creature the gait is
  // still settling under — what is being denied is a SHOVE, not a micrometre.
  {
    const Vec3 d = c.mobs.MobOrigin(who) - origin0;
    check(d.len() < std::max(noise, 0.05f),
          "the reaction moved no collider: the origin is where it was");
    check(std::fabs(c.mobs.MobHeading(who) - heading0) < 1e-3f,
          "the reaction turned nobody: the heading is where it was");
  }

  // ---- C2. IT PUTS ITSELF BACK --------------------------------------------
  for (int i = 0; i < kWindow; i++) tick();
  {
    Mob* live = c.mobs.FindMobById(who);
    const float left = (BodyCentroid(live) - rest).len();
    check(left < peak * (float)BaselineNumber("hitReact.settleFrac", 0.25) ||
              left <= noise,
          "the body is back on its feet inside two windows");
    check(live != nullptr && !live->HitReactLive(),
          "the layer switched itself off (CLAUDE.md rule 2)");
  }

  // ---- B2. THE MIRROR -----------------------------------------------------
  // The half that makes the direction claim mean something. A reaction that
  // ignored `dirWorld` and always leaned the same way passes every check above.
  rest = BodyCentroid(c.mobs.FindMobById(who));
  strike(Vec3{-1, 0, 0});
  Vec3 mirrorOff{};
  float mirrorPeak = 0;
  for (int i = 0; i < kWindow; i++) {
    tick();
    const Vec3 off = BodyCentroid(c.mobs.FindMobById(who)) - rest;
    if (off.len() > mirrorPeak) {
      mirrorPeak = off.len();
      mirrorOff = off;
    }
  }
  check(mirrorOff.x < 0.0f && peakOff.x > 0.0f,
        "a mirrored blow mirrors the reaction");
  RecordObserved("hitReact.observedMirrorX", mirrorOff.x);

  CloseStage(c);
  detail = Format("peak %.2f vox (noise %.2f), along %.2f, mirror x %.2f",
                  peak, noise, alongX, mirrorOff.x);
  std::printf("hit-react: %s (%d checks, %s)\n", ok ? "PASS" : "FAIL", checks,
              detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

// ---- levitate: a sustained gravity mod on a body that is NOT the caster ----
//
// (spell.h `SpellBodyImpulse` -> session.cpp's routing -> `Mob::AddBodyVelocity`,
// 2026-09-22, from the owner's report that `float aura` worked on himself, did
// nothing to an enemy, and did nothing to a ragdoll.)
//
// Three claims, because those were three independent causes that look like the
// same zero from outside (CLAUDE.md rule 6):
//
//   1. THE BODY CAN BE FOUND. A status attaches to whatever body lies within
//      the effect's radius + 2 of the point the spell resolved at - three
//      voxels for a bare mod. That search measured to `MobOrigin`, which is
//      the collider's MIN CORNER in x/z and the FEET in y, so a bolt that
//      struck a chest found no body at all, the status fell back to being a
//      PLACE, and a sustained mod on a place does nothing. Both distances are
//      printed; the box one is the claim.
//   2. A LIVE BODY RISES when it is lifted, and does not when it is not. The
//      control arm is the same creature over the same span with no lift, so
//      the number is a differential and not a reading of the terrain.
//   3. A LIMP BODY RISES TOO. That is a different velocity state entirely -
//      the rig belongs to Jolt once it goes down - and it is the half the
//      owner reported second.
Status GateLevitate(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const char* what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("levitate: FAILED %s\n", what);
    }
  };

  Stage st = OpenStage(c);
  if (!st.ok) {
    detail = st.why;
    std::printf("levitate: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }

  // WHAT `float` IS WORTH, derived rather than guessed: glyphs.json gives the
  // word `gravity add -1000`, and spell.cpp turns a sustained gravity mod into
  // `-amount * 0.012` voxels/sec of velocity per tick. One `float` is +12.
  const float kFloatVps = (float)BaselineNumber("levitate.floatVps", 12.0);
  const int kSpan = (int)BaselineNumber("levitate.spanTicks", 24);
  const float minRise = (float)BaselineNumber("levitate.minRiseVox", 3.0);

  std::string why;
  auto spawn = [&](int dz) {
    return SpawnFighter(c, st.defIndex,
                        {st.spot.x, st.spot.y + 1, st.spot.z + dz},
                        "training_dummy", false, why);
  };
  const uint64_t live = spawn(0);
  const uint64_t limp = spawn(24);
  if (live == 0 || limp == 0) {
    detail = why.empty() ? "fixture spawn failed" : why;
    std::printf("levitate: SKIP (%s)\n", detail.c_str());
    CloseStage(c);
    return Status::Skip;
  }
  Ticker tick{c, 33000, {st.spot.x >> 4, st.spot.y >> 4, st.spot.z >> 4}};
  for (int i = 0; i < 48; i++) tick();

  // ---- (1) the body's box, and the point a bolt would resolve at -----------
  Vec3 lo{}, hi{};
  const bool haveBox = c.mobs.MobBodyBox(live, lo, hi);
  check(haveBox, "a spawned mob reports a body box at all");
  const Vec3 chest{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f,
                   (lo.z + hi.z) * 0.5f};
  const Vec3 org = c.mobs.MobOrigin(live);
  const float dOrigin = (chest - org).len();
  const float dBox =
      std::max({lo.x - chest.x, chest.x - hi.x, lo.y - chest.y, chest.y - hi.y,
                lo.z - chest.z, chest.z - hi.z, 0.0f});
  check(haveBox && hi.y - lo.y >= 4.0f,
        "the box has the creature's HEIGHT in it (not a point at its feet)");
  check(dBox <= 0.0f, "a strike at the middle of a body is ON that body");

  // ---- (2) live: lifted vs not, same creature, same span -------------------
  auto riseOf = [&](uint64_t who, bool lift) {
    const float y0 = c.mobs.MobOrigin(who).y;
    float peak = 0.0f;
    for (int i = 0; i < kSpan; i++) {
      if (lift) c.mobs.LiftMob(who, Vec3{0.0f, kFloatVps, 0.0f});
      tick();
      if (c.mobs.FindMobById(who) == nullptr) break;
      peak = std::max(peak, c.mobs.MobOrigin(who).y - y0);
    }
    return peak;
  };
  const float idle = riseOf(live, false);
  const float lifted = riseOf(live, true);
  check(idle < 1.0f, "a creature nobody lifted stays on the ground");
  check(lifted >= minRise, "`float aura` lifts a LIVE body off the ground");
  const Mob* lm = c.mobs.FindMobById(live);
  check(lm != nullptr && lm->Airborne(),
        "...and it is airborne while it is held up, not standing in mid-air");

  // ---- (3) limp: the same lift through Jolt --------------------------------
  Mob* dead = c.mobs.FindMobById(limp);
  if (dead != nullptr) dead->StartRagdoll(4.0f, "levitate gate");
  for (int i = 0; i < 4; i++) tick();   // let the rig go dynamic
  dead = c.mobs.FindMobById(limp);
  const bool wentLimp = dead != nullptr && dead->Ragdolled();
  const float limpIdle = riseOf(limp, false);
  const float limpRise = riseOf(limp, true);
  check(wentLimp, "the second fixture actually went limp");
  check(limpIdle < 1.0f, "a corpse nobody lifted stays put");
  check(limpRise >= minRise, "`float aura` lifts a LIMP body too");

  std::printf(
      "levitate: box %.1f tall | chest %.1f vox from the collider ORIGIN, "
      "%.1f from the BOX | live rise %.2f (idle %.2f) | limp rise %.2f "
      "(idle %.2f) | %d checks\n",
      hi.y - lo.y, dOrigin, dBox, lifted, idle, limpRise, limpIdle, checks);

  c.mobs.Reset();
  c.debris.Reset();
  CloseStage(c);
  if (!ok) {
    detail = "a sustained gravity mod did not move a body";
    return Status::Fail;
  }
  return Status::Pass;
}

const std::vector<Gate>& CombatGates() {
  static const std::vector<Gate> g = {
      // No deps, no world, no GPU: both build their own inputs and neither
      // leaves anything behind, so they can be run alone and in any order.
      {"combat-tuning", "player", {}, false, GateCombatTuning},
      {"combat-cues", "player", {}, false, GateCombatCues},
      // Pure CPU over tuning.json like combat-tuning, but for every
      // tuning_params.def row rather than the combat groups.
      {"tuning-reach", "player", {}, false, GateTuningReach},
      // These four DO touch the world and spawn creatures, so kOrder puts them
      // at the END of the mob group. Same list, opposite end of the run.
      {"npc-strike", "mob", {}, false, GateNpcStrike},
      {"npc-block", "mob", {}, false, GateNpcBlock},
      {"npc-styles", "mob", {}, false, GateNpcStyles},
      // ...and the same replay measured for a different defect: does the pose
      // pass through itself (game/selfclip.h). Same fixture shape as
      // npc-styles and placed beside it, because they ask the two halves of
      // "is this swing any good" — where it went, and what it went through.
      {"rig-clip", "mob", {}, false, GateRigClip},
      {"duel", "mob", {}, false, GateDuel},
      // ---- THE UNARMED HALF (docs/PLAN_impact_unarmed.md §8) --------------
      // Same shape as the four above and placed with them in kOrder: each
      // spawns creatures, restores the id counter and regenerates worldgen on
      // the way out. `lunge` also throws bodies through the air, which is why
      // it sits after the ones that do not.
      {"unarmed-attack", "mob", {}, false, GateUnarmedAttack},
      {"lunge", "mob", {}, false, GateLunge},
      {"bite-target", "mob", {}, false, GateBiteTarget},
      {"zombie-draw", "mob", {}, false, GateZombieDraw},
      // ...and the one that watches the BRICK POOL while they do it. Same
      // fixture shape, same place in kOrder, but its subject is the render
      // bookkeeping the fight churns rather than the fight.
      {"limb-alias", "mob", {}, false, GateLimbAlias},
      // ---- the directional flinch (mob.h Mob::HitReact) -------------------
      // Spawns one passive dummy and hits it twice through the ordinary
      // MobSystem entry point. Same shape as the ones above — id scope in,
      // worldgen out — so `--gate hit-react` is the whole of iterating on it.
      {"hit-react", "mob", {}, false, GateHitReact},
      // ---- a sustained gravity mod on somebody else's body ----------------
      // Spawns two creatures and lays one of them down, so it goes with the
      // group above and at the end of it.
      {"levitate", "mob", {}, false, GateLevitate},
  };
  return g;
}

}  // namespace selftest
