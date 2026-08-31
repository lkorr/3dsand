// selftest_combat.cpp — the combat feel layer, asserted without a world.
//
// TWO GATES, both pure CPU, no GPU, no fixtures, nothing left behind. They sit
// at the FRONT of kOrder beside `simd`, `scale`, `player-kit` and `swing` for
// the reason that block gives: a gate that neither disturbs pristine worldgen
// nor is disturbed by it costs milliseconds and reports a broken build in the
// first second of a run.
//
//   combat-tuning  the `melee.*` and `combatfx.*` groups of tuning.json reach
//                  the code that uses them, and the clamps in LoadTuning are
//                  real rather than decorative.
//   combat-cues    the three melee sound slots agree with the asset tree and
//                  the engine actually asks for them.
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

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "audio/cues.h"
#include "game/melee.h"
#include "sim/scale.h"
#include "sim/tuning.h"
#include "test/selftest.h"
#include "test/support.h"

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
      {"combatfx", "whooshVolume", 0.71f, [](const Tuning& t) { return t.combatfx.whooshVolume; }},
      {"combatfx", "whooshMinSpeed", 411.0f, [](const Tuning& t) { return t.combatfx.whooshMinSpeed; }},
      {"combatfx", "whooshRateSlow", 0.71f, [](const Tuning& t) { return t.combatfx.whooshRateSlow; }},
      {"combatfx", "whooshRateFast", 1.41f, [](const Tuning& t) { return t.combatfx.whooshRateFast; }},
      {"combatfx", "fleshVolume", 1.21f, [](const Tuning& t) { return t.combatfx.fleshVolume; }},
      {"combatfx", "clangVolume", 1.31f, [](const Tuning& t) { return t.combatfx.clangVolume; }},
      {"combatfx", "cueRadius", 33.0f, [](const Tuning& t) { return t.combatfx.cueRadius; }},
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
        "every melee/combatfx key reaches its field");

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
    "flashHalflife": 0.0, "cueRadius": 0.0
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
    same("combatfx.hitStopSeverMs", shipped.combatfx.hitStopSeverMs,
         defaults.combatfx.hitStopSeverMs);
    same("combatfx.flashHalflife", shipped.combatfx.flashHalflife,
         defaults.combatfx.flashHalflife);
    check(shipped.combatfx.hitStop == defaults.combatfx.hitStop,
          "combatfx.hitStop matches the compiled default");
    check(drift == 0, "shipped tuning.json matches the compiled defaults");
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
  for (const char* slot : {"whoosh", "flesh", "clang"}) {
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
  };
  static const Slot kSlots[] = {
      {audio::Cues::CombatCue::Whoosh, "melee/whoosh"},
      {audio::Cues::CombatCue::Flesh, "melee/flesh"},
      {audio::Cues::CombatCue::Clang, "melee/clang"},
  };
  for (const Slot& s : kSlots) {
    checks++;
    const int id = cues.CombatSetId(s.cue);
    if (id < 0) {
      ok = false;
      std::printf(
          "combat-cues: FAILED '%s' resolves to nothing. The set is a FOLDER "
          "under assets/sounds — run `python scripts/gen_combat_sounds.py` for "
          "the placeholders, or add real takes through scripts/import_sounds.py\n",
          s.name);
    } else {
      resolved++;
    }
  }

  // ---- C. THE ENGINE ASKS FOR THEM -----------------------------------------
  // One call per cue; the counter must move by exactly three. `enabled_` is
  // false (no Init), so nothing is voiced and nothing is heard — which is the
  // point: this measures the REQUEST, which is the only half a headless run
  // has.
  {
    const uint32_t before = cues.GetStats().combat;
    cues.Combat(audio::Cues::CombatCue::Whoosh, Vec3{0, 0, 0}, 0.5f);
    cues.Combat(audio::Cues::CombatCue::Flesh, Vec3{0, 0, 0}, 1.0f);
    cues.Combat(audio::Cues::CombatCue::Clang, Vec3{0, 0, 0}, 0.0f);
    const uint32_t moved = cues.GetStats().combat - before;
    checks++;
    if (moved != 3) {
      ok = false;
      std::printf(
          "combat-cues: FAILED the request counter moved by %u, want 3. "
          "Stats::combat must increment BEFORE the enabled_ early-out or a "
          "headless run can assert nothing (audio/cues.h)\n",
          moved);
    }
  }

  detail = Format("%d checks, %d/3 sets resolved", checks, resolved);
  std::printf("combat-cues: %s (%d checks, %d/3 sets resolved)\n",
              ok ? "PASS" : "FAIL", checks, resolved);
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& CombatGates() {
  static const std::vector<Gate> g = {
      // No deps, no world, no GPU: both build their own inputs and neither
      // leaves anything behind, so they can be run alone and in any order.
      {"combat-tuning", "player", {}, false, GateCombatTuning},
      {"combat-cues", "player", {}, false, GateCombatCues},
  };
  return g;
}

}  // namespace selftest
