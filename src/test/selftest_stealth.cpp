// selftest_stealth.cpp -- STEALTH AND DETECTION (game/stealth.h,
// docs/PLAN_stealth.md): the detection meter, its two channels, and what a
// creature does with what it half-knows.
//
//   stealth  Pure arbiter arms (no world: hand-built actors through
//            ai::Think, the ai-tactics shape). The watcher is the shipped
//            `guardian` with its rules stripped and its senses pinned
//            (`t_watch`); the intruder is one modelled actor ahead of it.
//
//            A. THE TERMS. Visibility falls lit-walking > lit-crouched-still
//               > dark-crouched-still > dark-prone-still; noise falls
//               sprint > walk > crouch-walk > crawl > still (0); worn metal
//               raises it, rain shrinks how far it carries; the cone is 0
//               behind a 200-degree field.
//            B. A LIT BODY WALKING `stealth.gap` voxels ahead is DETECTED
//               (a target) within `stealth.litTicks`.
//            C. THE SAME BODY dark, crouched and creeping is never noticed
//               (meter under cautionAt) in `stealth.longTicks`.
//            D. CREEPING UP BEHIND, dark and crouched, to `stealth.backGap`:
//               never detected -- ai::Detects false, a sneak attack.
//            E. SPRINTING BEHIND: heard, not seen -- the meter climbs to the
//               sound cap and no further (no target), and the creature
//               INVESTIGATES: it turns to face the noise.
//            F. A DISTRACTION (a noise nobody made: an arrow into stone):
//               far, it looks; near, it walks to it and looks round.
//            G. HARDER TO LOSE: detected lit, the body then goes dark,
//               crouched and still -- and is still held in sight (a fresh
//               creature would never see it, arm C). Behind a wall, it is
//               kept for alertDecayTicks, then SEARCHED for, then the
//               creature stands down ALERTED.
//            H. LEGACY: an unmodelled actor is acquired on the first tick.
//            I. STRUCK: a blade from an unseen attacker -> it knows at once
//               (a target, felt from behind); an arrow -> it searches.
//            J. THE ALARM: on acquiring, it cries once; an ally that hears
//               the cry walks toward the intruder.
//            K. REPLAY: arm B twice, meter trace bit for bit.
//            L. THE LIGHT (MobSystem::LightAt, the live world): open air at
//               noon is bright, at night moonlit, under the ground dark.
//
//   stealth-world  THE LIVE PATH, through THE tick (support::TickRig): a
//            `stalker` NPC (sword in hand) behind a `villager` facing away,
//            `stealth.worldGap` voxels apart on a stone strip. Within
//            `stealth.worldTicks`: the stalker STALKS (Intent::Stalk, the
//            body crouched -- Mob::AiCrouch), its stroke lands as a SNEAK
//            ATTACK (a MobSystem::SneakEvent, x stealth.sneakMelee) on a
//            villager that had not detected it, and the villager -- told by
//            the blow (NoteAttacked) and provoked by the hurt -- turns on it
//            within `stealth.answerTicks`. Both bodies publish a modelled
//            exposure (MobSystem::PublishStealth). The world is regenerated
//            on the way out.
//
// Thresholds in tests/baseline.json (`stealth.*`).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "game/ai_behavior.h"
#include "game/item.h"
#include "game/mob.h"
#include "game/session.h"
#include "game/stealth.h"
#include "phys/debris.h"
#include "phys/physics.h"
#include "sim/world.h"
#include "sim/worldgen_run.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

using namespace sandvox;

namespace selftest {
namespace {

constexpr uint64_t kIntruder = 500;
constexpr uint32_t kT0 = 9000;

bool gLosClear = true;
bool LosFn(void*, Vec3, Vec3) { return gLosClear; }

// The watcher's body: centre (0, 8.5, 0), facing +Z.
ai::SelfView WatcherSelf(const ai::Library& lib, int prof, uint64_t id = 77) {
  ai::SelfView self;
  self.id = id;
  self.origin = Vec3{-2.0f, 0.0f, -2.0f};
  self.size = Vec3{4.0f, 17.0f, 4.0f};
  self.heading = 0.0f;
  self.speed = 30.0f;
  self.faction = ai::FactionId(lib.At(prof)->faction);
  self.armed = true;
  self.canGuard = true;
  self.strikeReach = 9.0f;
  self.attackReach = 10.0f;
  return self;
}

// The intruder: a person `z` voxels along +Z (negative = behind), with the
// exposure the arm asks for. Its noise is the model's own for that exposure.
ai::Actor Intruder(float z, float light, stealth::Stance st, float moveFrac,
                   float sneak = 30.0f) {
  ai::Actor a;
  a.id = kIntruder;
  a.centre = Vec3{0.0f, 8.5f, z};
  a.radius = 3.0f;
  a.height = 17.0f;
  a.faction = ai::FactionId("player");
  a.alive = true;
  a.stealth.modelled = true;
  a.stealth.sneak = sneak;
  a.stealth.stance = st;
  a.stealth.moveFrac = moveFrac;
  a.stealth.light = light;
  a.stealth.noise = stealth::MovementNoise(stealth::Defaults(), st, moveFrac, 0.0f,
                                           1.0f, 0.0f, sneak);
  return a;
}

struct Run {
  ai::Brain brain;
  int detectedAt = -1;           // first tick the intruder was the target
  float maxLevel = 0.0f;
  int investigateTicks = 0, searchTicks = 0, visibleTicks = 0;
  int firstInvestigate = -1, firstSearch = -1;
  int alarms = 0;
  float lastHeading = 0.0f, lastDrive = 0.0f;
  std::vector<float> trace;      // the intruder's meter, every tick
  uint32_t nextTick = 0;         // where a run continuing this brain starts
};

// One watcher for `ticks`, the script moving the world under it first.
using Script = std::function<void(int t, std::vector<ai::Actor>& actors,
                                  std::vector<stealth::Noise>& noises)>;
// A run that CONTINUES a brain (`start`) must continue its clock too (`t0` =
// the earlier run's nextTick): every timer on a brain is an absolute tick.
Run RunWatch(const ai::Library& lib, int prof, std::vector<ai::Actor> actors,
             int ticks, const Script& script, ai::Brain* start = nullptr,
             uint64_t selfId = 77, uint32_t t0 = kT0) {
  Run r;
  r.brain = start != nullptr ? *start : ai::Brain(prof);
  const ai::SelfView self = WatcherSelf(lib, prof, selfId);
  ai::GroundView g;
  std::vector<stealth::Noise> noises;
  ai::WorldView v;
  v.actors = &actors;
  v.noises = &noises;
  v.lineOfSight = &LosFn;
  const stealth::Params& P = stealth::Defaults();
  v.stealthParams = &P;
  for (int t = 0; t < ticks; t++) {
    noises.clear();
    if (script) script(t, actors, noises);
    ai::IntentOut out;
    ai::Think(r.brain, lib, self, g, v, t0 + (uint32_t)t, 1.0f / 30.0f, out);
    const float lv = ai::AwarenessOf(r.brain, kIntruder);
    r.trace.push_back(lv);
    r.maxLevel = std::max(r.maxLevel, lv);
    if (r.detectedAt < 0 && r.brain.hasTarget && r.brain.targetId == kIntruder)
      r.detectedAt = t;
    if (r.brain.hasTarget && r.brain.visible) r.visibleTicks++;
    if (r.brain.intent == ai::Intent::Investigate) {
      r.investigateTicks++;
      if (r.firstInvestigate < 0) r.firstInvestigate = t;
    }
    if (r.brain.intent == ai::Intent::Search) {
      r.searchTicks++;
      if (r.firstSearch < 0) r.firstSearch = t;
    }
    if (out.alarm) r.alarms++;
    r.lastHeading = out.desiredHeading;
    r.lastDrive = out.driveScale;
  }
  r.nextTick = t0 + (uint32_t)ticks;
  return r;
}

int Clone(ai::Library& lib, const std::string& base, const std::string& name) {
  const ai::Profile* b = lib.At(lib.Find(base));
  if (b == nullptr) return -1;
  ai::Profile p = *b;
  p.name = name;
  p.rules.clear();
  lib.profiles.push_back(p);
  return (int)lib.profiles.size() - 1;
}

float WrapPiF(float a) { return std::remainder(a, 6.2831853f); }

Status GateStealth(Ctx& c, std::string& detail) {
  std::string why;
  bool ok = true;
  auto fail = [&](const std::string& s) {
    ok = false;
    if (!why.empty()) why += "; ";
    why += s;
  };
  gLosClear = true;
  const stealth::Params& P = stealth::Defaults();

  // ---- A: the terms ---------------------------------------------------------
  {
    auto vis = [&](float light, stealth::Stance st, float mf) {
      stealth::Exposure e;
      e.modelled = true;
      e.sneak = 30.0f;
      e.stance = st;
      e.moveFrac = mf;
      e.light = light;
      return stealth::Visibility(P, e, 17.0f, 0.0f);
    };
    const float v1 = vis(1.0f, stealth::Stance::Stand, 1.0f);
    const float v2 = vis(1.0f, stealth::Stance::Crouch, 0.0f);
    const float v3 = vis(0.05f, stealth::Stance::Crouch, 0.0f);
    const float v4 = vis(0.05f, stealth::Stance::Prone, 0.0f);
    if (!(v1 > v2 && v2 > v3 && v3 > v4 && v4 > 0.0f))
      fail(Format("visibility order %.3f > %.3f > %.3f > %.3f broken", v1, v2, v3, v4));
    auto noise = [&](stealth::Stance st, float mf, float metal) {
      return stealth::MovementNoise(P, st, mf, metal, 1.0f, 0.0f, 30.0f);
    };
    const float sprint = noise(stealth::Stance::Stand, 1.8f, 0.0f);
    const float walk = noise(stealth::Stance::Stand, 1.0f, 0.0f);
    const float creep = noise(stealth::Stance::Crouch, 0.5f, 0.0f);
    const float crawl = noise(stealth::Stance::Prone, 0.5f, 0.0f);
    const float still = noise(stealth::Stance::Stand, 0.0f, 0.0f);
    const float plate = noise(stealth::Stance::Stand, 1.0f, 1.0f);
    if (!(sprint > walk && walk > creep && creep > crawl && crawl > still && still == 0.0f))
      fail(Format("noise order %.1f > %.1f > %.1f > %.1f > %.1f broken", sprint, walk,
                  creep, crawl, still));
    if (!(plate > walk)) fail(Format("worn metal %.1f not louder than %.1f", plate, walk));
    const float dry = stealth::SoundGain(P, walk, walk * 0.5f, 1.0f, 50.0f, 0.0f, false);
    const float wet = stealth::SoundGain(P, walk, walk * 0.5f, 1.0f, 50.0f,
                                         stealth::Ambient(P, 255), false);
    if (!(dry > wet)) fail(Format("rain does not mask: %.3f vs %.3f", dry, wet));
    if (stealth::ConeGain(3.14159f, 200.0f, 70.0f, 0.35f) != 0.0f ||
        stealth::ConeGain(0.0f, 200.0f, 70.0f, 0.35f) != 1.0f)
      fail("cone gains wrong (behind must be 0, ahead 1)");
    RecordObserved("stealth.visLitWalk", (double)v1);
    RecordObserved("stealth.visDarkProne", (double)v4);
    RecordObserved("stealth.noiseWalk", (double)walk);
  }

  // ---- the watcher -----------------------------------------------------------
  ai::Library lib;
  std::string log;
  if (!ai::LoadBehaviors(AssetDir() + "/mobs/behaviors.json", lib, log)) {
    detail = "behaviors.json did not load: " + log;
    return Status::Fail;
  }
  const int watch = Clone(lib, "guardian", "t_watch");
  if (watch < 0 || !lib.At(watch)->stealth.enabled) {
    detail = "the shipped guardian has no stealth block";
    return Status::Fail;
  }
  {
    stealth::Senses& s = lib.At(watch)->stealth;
    s.awareness = 50.0f;
    s.sightRange = 200.0f;
    s.fovDegrees = 200.0f;
    s.centralDeg = 70.0f;
    s.peripheral = 0.35f;
    s.hearing = 1.0f;
    s.darkVision = 0.0f;
    s.searchTicks = 150;
    s.searchRadius = 30.0f;
    s.investigateTicks = 120;
    s.alarm = 120.0f;
    lib.At(watch)->perception.alertDecayTicks = 60;
  }
  const float gap = (float)BaselineNumber("stealth.gap", 60.0);
  const int litTicks = (int)BaselineNumber("stealth.litTicks", 60.0);
  const int longTicks = (int)BaselineNumber("stealth.longTicks", 300.0);
  const float backGap = (float)BaselineNumber("stealth.backGap", 9.0);

  // ---- B: lit, walking: detected ---------------------------------------------
  const Run lit = RunWatch(lib, watch, {Intruder(gap, 1.0f, stealth::Stance::Stand, 1.0f)},
                           litTicks + 30, nullptr);
  if (lit.detectedAt < 0 || lit.detectedAt > litTicks)
    fail(Format("B: lit walker at %.0f detected at tick %d (want <= %d), meter %.2f", gap,
                lit.detectedAt, litTicks, lit.maxLevel));
  RecordObserved("stealth.litDetectTick", (double)lit.detectedAt);

  // ---- C: dark, crouched, creeping: never noticed -----------------------------
  const Run dark = RunWatch(lib, watch,
                            {Intruder(gap, 0.05f, stealth::Stance::Crouch, 0.4f)},
                            longTicks, nullptr);
  if (dark.detectedAt >= 0 || dark.maxLevel >= P.cautionAt)
    fail(Format("C: shadow creeper noticed (meter %.2f, detected at %d)", dark.maxLevel,
                dark.detectedAt));

  // ---- D: creeping up behind -------------------------------------------------
  {
    const int steps = 240;
    const Run back = RunWatch(
        lib, watch, {Intruder(-gap, 0.05f, stealth::Stance::Crouch, 0.4f)}, steps,
        [&](int t, std::vector<ai::Actor>& a, std::vector<stealth::Noise>&) {
          const float u = (float)t / (float)(steps - 1);
          a[0].centre.z = -(gap + (backGap - gap) * u);
        });
    if (back.detectedAt >= 0 || ai::Detects(back.brain, lib, kIntruder))
      fail(Format("D: a creeper behind was detected (meter %.2f)", back.maxLevel));
    RecordObserved("stealth.backMeter", (double)back.maxLevel);
  }

  // ---- E: sprinting behind: heard, not seen -----------------------------------
  {
    const Run loud = RunWatch(lib, watch,
                              {Intruder(-gap, 1.0f, stealth::Stance::Stand, 1.8f)}, 120,
                              nullptr);
    if (loud.detectedAt >= 0)
      fail(Format("E: heard-only body became a target at tick %d", loud.detectedAt));
    if (loud.maxLevel < P.investigateAt || loud.maxLevel > P.soundCap + 1e-4f)
      fail(Format("E: sound-only meter %.2f (want %.2f..%.2f)", loud.maxLevel,
                  P.investigateAt, P.soundCap));
    if (loud.firstInvestigate < 0) {
      fail("E: never investigated the footsteps behind it");
    } else if (std::abs(WrapPiF(loud.lastHeading - 3.14159265f)) > 0.6f) {
      fail(Format("E: investigating, but heading %.2f rad, not toward the noise (pi)",
                  loud.lastHeading));
    }
  }

  // ---- F: a distraction -------------------------------------------------------
  {
    auto distract = [&](float d) {
      return RunWatch(lib, watch, {}, 90,
                      [d](int t, std::vector<ai::Actor>&, std::vector<stealth::Noise>& n) {
                        if (t != 2) return;
                        stealth::Noise e;
                        e.pos = e.point = Vec3{d * 0.6f, 1.0f, d * 0.8f};
                        e.radius = stealth::Defaults().impactNoise;
                        e.kind = stealth::NoiseKind::Impact;
                        n.push_back(e);
                      });
    };
    // The gate's body cannot move (no locomotion runs), so "it walks over"
    // is the drive it asks for, not an arrival.
    const float r = P.impactNoise;
    const Run farD = distract(r * 0.6f);   // impulse ~0.4: turn and look
    const Run nearD = distract(r * 0.2f);  // impulse ~0.8: go and look
    if (farD.firstInvestigate < 0)
      fail("F: a far arrow strike was not even looked at");
    else if (farD.lastDrive > 0.0f)
      fail("F: a faint noise sent it walking");
    if (nearD.firstInvestigate < 0 || nearD.lastDrive <= 0.0f)
      fail(Format("F: a near arrow strike did not bring it over (investigate %d ticks, "
                  "drive %.2f)",
                  nearD.investigateTicks, nearD.lastDrive));
  }

  // ---- G: harder to lose; then the search -------------------------------------
  {
    ai::Brain b = lit.brain;   // detected and fighting it
    if (!b.hasTarget) {
      fail("G: no detected start (arm B failed)");
    } else {
      const Run kept = RunWatch(lib, watch,
                                {Intruder(gap, 0.05f, stealth::Stance::Crouch, 0.0f)}, 150,
                                nullptr, &b, 77, lit.nextTick);
      if (kept.visibleTicks < 140)
        fail(Format("G: a fought body in the dark was held %d / 150 ticks", kept.visibleTicks));
      ai::Brain b2 = kept.brain;
      gLosClear = false;
      const int alertDecay = (int)lib.At(watch)->perception.alertDecayTicks;
      const int searchT = (int)lib.At(watch)->stealth.searchTicks;
      const Run lost = RunWatch(lib, watch,
                                {Intruder(gap, 0.05f, stealth::Stance::Crouch, 0.0f)},
                                alertDecay + searchT + 40, nullptr, &b2, 77, kept.nextTick);
      gLosClear = true;
      if (lost.brain.searches != 1 || lost.firstSearch < alertDecay - 5)
        fail(Format("G: lost behind a wall: %u searches, first search tick %d (want 1, "
                    ">= %d)",
                    lost.brain.searches, lost.firstSearch, alertDecay - 5));
      if (lost.brain.standDowns != 1 || lost.brain.alertedUntil < lost.nextTick)
        fail(Format("G: the search did not stand down alerted (%u stand-downs)",
                    lost.brain.standDowns));
      if (lost.brain.hasTarget) fail("G: still holds a target it cannot see or hear");
    }
  }

  // ---- H: legacy --------------------------------------------------------------
  {
    ai::Actor old = Intruder(gap * 0.5f, 0.0f, stealth::Stance::Prone, 0.0f);
    old.stealth = stealth::Exposure{};   // not modelled
    const Run leg = RunWatch(lib, watch, {old}, 3, nullptr);
    if (leg.detectedAt != 0)
      fail(Format("H: an unmodelled actor was acquired at tick %d (want 0)", leg.detectedAt));
  }

  // ---- I: struck -------------------------------------------------------------
  {
    ai::Brain b(watch);
    ai::NoteAttacked(b, lib, kIntruder, Vec3{0.0f, 8.5f, -6.0f}, false, kT0, P);
    const Run felt = RunWatch(lib, watch,
                              {Intruder(-6.0f, 0.05f, stealth::Stance::Crouch, 0.0f)}, 4,
                              nullptr, &b);
    if (felt.detectedAt != 0 || !ai::Detects(felt.brain, lib, kIntruder))
      fail(Format("I: a blade from behind did not make a target (at %d)", felt.detectedAt));
    ai::Brain a(watch);
    ai::NoteAttacked(a, lib, kIntruder, Vec3{0.0f, 8.5f, -80.0f}, true, kT0, P);
    const Run shot = RunWatch(lib, watch,
                              {Intruder(-80.0f, 0.05f, stealth::Stance::Crouch, 0.0f)}, 30,
                              nullptr, &a);
    if (shot.detectedAt >= 0 || shot.brain.searches != 1 || shot.firstSearch < 0)
      fail(Format("I: an arrow from nowhere: detected %d, %u searches", shot.detectedAt,
                  shot.brain.searches));
  }

  // ---- J: the alarm ------------------------------------------------------------
  {
    if (lit.alarms != 1)
      fail(Format("J: %d alarms on acquiring (want exactly 1)", lit.alarms));
    // An ally 60 voxels to the side, the intruder out of its sight (behind
    // a wall), hears the cry pointing at the intruder.
    gLosClear = false;
    const Vec3 point{0.0f, 8.5f, gap};
    const Run ally = RunWatch(
        lib, watch, {Intruder(gap, 0.05f, stealth::Stance::Crouch, 0.0f)}, 60,
        [&](int t, std::vector<ai::Actor>&, std::vector<stealth::Noise>& n) {
          if (t != 1) return;
          stealth::Noise e;
          e.pos = Vec3{40.0f, 8.5f, 0.0f};
          e.point = point;
          e.radius = 120.0f;
          e.source = kIntruder;
          e.faction = ai::FactionId(lib.At(watch)->faction);
          e.kind = stealth::NoiseKind::Alarm;
          n.push_back(e);
        },
        nullptr, 78);
    gLosClear = true;
    if (ally.firstInvestigate < 0 || ally.lastDrive <= 0.0f)
      fail(Format("J: the ally did not come to look (investigate %d ticks, drive %.2f)",
                  ally.investigateTicks, ally.lastDrive));
  }

  // ---- K: replay ----------------------------------------------------------------
  {
    const Run again = RunWatch(lib, watch,
                               {Intruder(gap, 1.0f, stealth::Stance::Stand, 1.0f)},
                               litTicks + 30, nullptr);
    bool same = again.trace.size() == lit.trace.size();
    for (size_t i = 0; same && i < lit.trace.size(); i++)
      same = std::memcmp(&again.trace[i], &lit.trace[i], sizeof(float)) == 0;
    if (!same) fail("K: the meter did not replay bit for bit");
  }

  // ---- L: the light on a body, in the live world (MobSystem::LightAt) ------
  // Open air over the harness at noon is bright; the same column at night is
  // moonlight; a point well under the analytic ground is dark whatever the
  // caches hold. No tick runs: the day phase is put back to the harness's 0.
  float noon = 0.0f, night = 0.0f, buried = 0.0f;
  {
    const IVec3 wo = c.world.WindowOrigin();
    const int x = wo.x * (int)kChunk + (int)kWorldN / 2;
    const int z = wo.z * (int)kChunk + (int)kWorldN / 2;
    const int ylo = wo.y * (int)kChunk + 2, yhi = wo.y * (int)kChunk + (int)kWorldN - 60;
    const int gy = std::clamp(World::TerrainHeight(x, z, c.world.WorldSeed()), ylo + 40, yhi);
    const Vec3 above{(float)x + 0.5f, (float)gy + 30.0f, (float)z + 0.5f};
    const Vec3 under{(float)x + 0.5f, (float)gy - 36.0f, (float)z + 0.5f};
    c.mobs.SetDayPhase(kDayNoon);
    noon = c.mobs.LightAt(c.world, above, 17.0f);
    buried = c.mobs.LightAt(c.world, under, 17.0f);
    c.mobs.SetDayPhase(0u);
    night = c.mobs.LightAt(c.world, above, 17.0f);
    if (!(noon > 0.8f && night < 0.35f && buried < 0.25f))
      fail(Format("L: light noon %.2f (want > 0.8), night %.2f (< 0.35), buried %.2f (< 0.25)",
                  noon, night, buried));
    RecordObserved("stealth.lightNoon", (double)noon);
    RecordObserved("stealth.lightNight", (double)night);
  }

  detail = ok ? Format("lit walker detected at t%d; shadow creeper meter %.2f; sound-only "
                       "meter capped; light noon %.2f / night %.2f / buried %.2f",
                       lit.detectedAt, dark.maxLevel, noon, night, buried)
              : why;
  return ok ? Status::Pass : Status::Fail;
}

// ============================================================================
// stealth-world: the live path
// ============================================================================

uint32_t MatIdOf(const Ctx& c, const char* name) {
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == name) return (uint32_t)i;
  return 0;
}

Status GateStealthWorld(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  std::string fails;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) {
      fails += (fails.empty() ? "" : "; ") + what;
      std::printf("stealth-world: FAILED %s\n", what.c_str());
    }
  };
  const int gap = (int)BaselineNumber("stealth.worldGap", 32.0);
  const int maxTicks = (int)BaselineNumber("stealth.worldTicks", 600.0);
  const int answerTicks = (int)BaselineNumber("stealth.answerTicks", 60.0);
  const ItemDef* sword = c.items.Named("sword");
  const int hd = c.mobs.FindDef("human");
  if (sword == nullptr || hd < 0 || c.mobs.Behaviors().Find("stalker") < 0 ||
      c.mobs.Behaviors().Find("villager") < 0) {
    detail = "needs the human def, the sword and the stalker / villager profiles";
    return Status::Fail;
  }

  // A FRESH WORLD: bodies torn down, ids and Jolt history rewound, worldgen.
  c.mobs.Reset();
  c.debris.Reset();
  c.phys.ResetBirthOrdinals();
  c.phys.ResetBodyIdHistory();
  c.mobs.SetNextIdCounter(1);
  c.mobs.SetWeatherRain(0u);
  c.mobs.SetRainSlope(0, 0);
  c.stream.OnRegen();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  // A flat stone strip along +x with clear air over it.
  const IVec3 o = c.world.WindowOrigin();
  const int sx = o.x * (int)kChunk + 200, sz = o.z * (int)kChunk + 200;
  const int behind = 40, ahead = gap + 50, halfZ = 12;
  const int sy = FixtureYOver(sx - behind, sz - halfZ, sx + ahead, sz + halfZ, kDefaultSeed, 1);
  const uint32_t stone = PackVoxNew(MatIdOf(c, "stone"), 0);
  std::vector<CellOp> cells;
  for (int zz = sz - halfZ; zz <= sz + halfZ; zz++)
    for (int xx = sx - behind; xx <= sx + ahead; xx++) {
      for (int yy = World::TerrainHeight(xx, zz, kDefaultSeed) - 2; yy < sy; yy++)
        cells.push_back({World::SlotCellIndex({xx, yy, zz}), stone});
      for (int yy = sy; yy <= sy + 26; yy++)
        cells.push_back({World::SlotCellIndex({xx, yy, zz}), 0u});
    }
  auto rig = std::make_unique<support::TickRig>(c, 121000u, IVec3{sx >> 4, sy >> 4, sz >> 4});
  {
    const size_t step = kMaxCellOpsPerTick / 2;
    for (size_t i = 0; i < cells.size(); i += step) {
      const std::vector<CellOp> part(cells.begin() + (ptrdiff_t)i,
                                     cells.begin() + (ptrdiff_t)std::min(cells.size(), i + step));
      support::RunTicks(*rig, 1, [&](uint32_t, support::TickOps& op) { op.cells = part; });
    }
  }
  const Vec3 ws = c.mobs.Defs()[(size_t)hd].worldSize;
  auto spawnAt = [&](float cx) {
    return c.mobs.Spawn(hd, IVec3{(int)std::floor(cx - ws.x * 0.5f), sy,
                                  (int)std::floor((float)sz + 0.5f - ws.z * 0.5f)});
  };
  const uint64_t victim = spawnAt((float)sx + 0.5f + (float)gap);
  const uint64_t stalker = spawnAt((float)sx + 0.5f);
  if (victim == 0 || stalker == 0) {
    detail = "the two humans did not spawn";
    return Status::Fail;
  }
  // LET THEM LAND FIRST. A spawn drops a body onto the strip, and a landing
  // is a noise (MobSystem::PublishStealth): with brains already on, the
  // villager heard the stalker arrive and turned to look before the scene
  // began. Brainless for the settle; a noise is heard for one tick only.
  c.mobs.SetHeading(victim, 1.5707963f);    // facing +x: its back to the stalker
  c.mobs.SetHeading(stalker, 1.5707963f);   // facing +x: the villager's back
  support::RunTicks(*rig, 40, [](uint32_t, support::TickOps&) {});
  c.mobs.SetMobBehavior(victim, "villager");
  c.mobs.SetMobBehavior(stalker, "stalker");
  c.mobs.SetHeading(victim, 1.5707963f);
  c.mobs.SetHeading(stalker, 1.5707963f);
  c.mobs.EquipItem(stalker, sword);
  c.mobs.ClearSneakEvents();

  int stalkTicks = 0, crouchTicks = 0, hitTick = -1, answeredTick = -1;
  int seenTick = -1;   // the stalker first held the villager as its target
  float mul = 0.0f;
  bool detectedBeforeHit = false;
  bool exposed = false;
  // ATTRIBUTION (CLAUDE.md rule 6): the moment the villager first NOTICED the
  // stalker -- how far, by which sense, what each was doing -- so "it was
  // seen first" comes with its cause.
  std::string noticed = "never";
  float minGap = 1e9f;
  int intentHist[(int)ai::Intent::Count] = {};
  for (int t = 0; t < maxTicks; t++) {
    support::RunTicks(*rig, 1, [](uint32_t, support::TickOps&) {});
    const ai::Brain* sb = c.mobs.MobBrain(stalker);
    const ai::Brain* vb = c.mobs.MobBrain(victim);
    Mob* sm = c.mobs.FindMobById(stalker);
    if (sb == nullptr || vb == nullptr || sm == nullptr) break;
    if (seenTick < 0 && sb->hasTarget && sb->targetId == victim) seenTick = t;
    if (sb->intent == ai::Intent::Stalk) stalkTicks++;
    intentHist[(int)sb->intent]++;
    float gapNow = 0.0f;
    {
      Vec3 lo{}, hi{}, a{}, b{};
      if (c.mobs.MobBodyBox(stalker, lo, hi)) a = (lo + hi) * 0.5f;
      if (c.mobs.MobBodyBox(victim, lo, hi)) b = (lo + hi) * 0.5f;
      gapNow = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
      minGap = std::min(minGap, gapNow);
    }
    if (noticed == "never")
      for (const stealth::Awareness& e : vb->aware)
        if (e.id == stalker && e.level >= c.mobs.StealthParams().cautionAt) {
          const ai::Actor* sa = nullptr;
          for (const ai::Actor& a : c.mobs.ActorList())
            if (a.id == stalker) sa = &a;
          noticed = Format("t%d gap %.1f level %.2f sight %.3f/s sound %.3f/s seen %d heard %d | "
                           "stalker %s crouch %d noise %.1f light %.2f move %.2f | villager %s",
                           t, gapNow, e.level, e.sightGain, e.soundGain, (int)e.seen,
                           (int)e.heard, ai::IntentName(sb->intent), (int)sm->AiCrouch(),
                           sa ? sa->stealth.noise : -1.0f, sa ? sa->stealth.light : -1.0f,
                           sa ? sa->stealth.moveFrac : -1.0f, ai::IntentName(vb->intent));
        }
    if (sm->AiCrouch()) crouchTicks++;
    if (hitTick < 0) {
      if (ai::Detects(*vb, c.mobs.Behaviors(), stalker)) detectedBeforeHit = true;
      for (const MobSystem::SneakEvent& e : c.mobs.SneakEvents())
        if (e.attacker == stalker && e.victim == victim) {
          hitTick = t;
          mul = e.mul;
        }
    } else if (answeredTick < 0 && vb->hasTarget && vb->targetId == stalker) {
      answeredTick = t;
    }
    if (!exposed)
      for (const ai::Actor& a : c.mobs.ActorList())
        if (a.id == victim && a.stealth.modelled) exposed = true;
    if (answeredTick >= 0 || (hitTick >= 0 && t > hitTick + answerTicks)) break;
  }
  const float want = c.mobs.StealthParams().sneakMelee;
  check(exposed, "the villager was never published with a modelled exposure");
  check(seenTick >= 0, "the stalker never took the villager as its target");
  check(stalkTicks > 0 && crouchTicks > 0,
        Format("the stalker never stalked crouched (stalk %d ticks, crouched %d)", stalkTicks,
               crouchTicks));
  check(hitTick >= 0, Format("no sneak attack landed in %d ticks (stalk %d ticks, villager "
                             "detected the stalker first: %s)",
                             maxTicks, stalkTicks, detectedBeforeHit ? "yes" : "no"));
  check(hitTick < 0 || std::abs(mul - want) < 1e-3f,
        Format("the sneak attack landed x%.2f (want x%.2f)", mul, want));
  check(hitTick < 0 || (answeredTick >= 0 && answeredTick - hitTick <= answerTicks),
        Format("the villager did not turn on its attacker within %d ticks (at %d after a hit "
               "at %d)",
               answerTicks, answeredTick, hitTick));
  RecordObserved("stealth.worldHitTick", (double)hitTick);
  RecordObserved("stealth.worldSeenTick", (double)seenTick);

  rig.reset();
  c.mobs.Reset();
  c.debris.Reset();
  c.stream.OnRegen();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  std::string hist;
  for (int k = 0; k < (int)ai::Intent::Count; k++)
    if (intentHist[k] > 0) hist += Format(" %s %d", ai::IntentName((ai::Intent)k), intentHist[k]);
  detail = Format("stalker targeted at t%d, stalked %d ticks (crouched %d), sneak attack at t%d "
                  "x%.2f, villager answered at t%d | closest %.1f | villager noticed: %s | "
                  "stalker intents:%s",
                  seenTick, stalkTicks, crouchTicks, hitTick, mul, answeredTick, minGap,
                  noticed.c_str(), hist.c_str());
  if (!fails.empty()) detail += " | FAILED: " + fails;
  std::printf("stealth-world: %s (%s)\n", fails.empty() ? "PASS" : "FAIL", detail.c_str());
  return fails.empty() ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& StealthGates() {
  static const std::vector<Gate> g = {
      // Pure arbiter arms: no world, no render.
      {"stealth", "mob", {}, false, GateStealth, /*needsRender=*/false},
      // The live path: a stalker, a villager, THE tick. Regenerates on the
      // way out.
      {"stealth-world", "mob", {}, false, GateStealthWorld, /*needsRender=*/false},
  };
  return g;
}

}  // namespace selftest
