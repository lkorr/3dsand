// selftest_audio.cpp — the CUE LAYER gates.
//
// WHY THESE ASSERT ON EVENTS AND NEVER ON SOUND. `--selftest` returns before
// audio init and opens no device (DESIGN.md §12b "Headless is silent"), so
// there is nothing to listen to here and there never will be. What CAN break
// silently is the half above the mixer: whether the engine NOTICES that a rock
// landed, that a creature was hurt, that there is water nearby. Every gate
// below asserts on that half — the event, its slot, its position — which is
// exactly the half DESIGN.md said was "left as a hook rather than landed
// unverified".
//
// The other half of each gate is the IDLE property (CLAUDE.md rule 2): a
// settled pile must report nothing, a world with no water must cost nothing.
// A cue hook that fires is easy; a cue hook that stops firing is the one that
// takes a wall of debris and turns it into a machine gun.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "audio/cues.h"
#include "audio/world.h"
#include "game/camera.h"
#include "sim/materials.h"
#include "sim/tuning.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// ---- impact: a body landing on terrain --------------------------------------
//
// Fixture: a stone slab, a stone block dropped onto it from a height, and then
// a long quiet window. The two claims are symmetric and both matter —
//   (a) the landing produces an ImpactEvent naming the STRUCK material with
//       real energy at roughly the right place, and
//   (b) once the block has settled, no further impacts are produced at all,
//       however long the world runs.
// (b) is the reason DESIGN.md left this unwired: Jolt reports a contact for
// every resting face of every body on every step, and voicing those is a
// machine gun. The speed gate lives in the contact listener, so this is the
// only place it can be observed from.
//
// THE TABLE IS NOW ACTUALLY FLAT (2026-09-12), and that was never cosmetic.
// It was a radius-6 SPHERE brush described in this comment as "a flat stone
// table", which stopped mattering the day bodies got continuous collision.
// With Jolt's Discrete default the block sank into the dome and was shoved
// back out along a squared-up manifold normal, which read as a clean 6.4 m/s
// landing and left it resting on the apex. With LinearCast the cast reports
// the FIRST touch: a cube corner against a 56-degree facet, whose normal
// component is 3.5 m/s of the same 6.3 m/s fall -- correct, since the rest of
// that speed became sliding -- and the block then slid off the dome and landed
// on the hillside 13 ticks into what this gate was calling its quiet window.
// Neither assertion below was wrong; the fixture was, in the way `settle-back`
// documents next door: A FIXTURE MUST NOT SUPPLY THE AWKWARD LANDING ITSELF.
// The pad is exact CellOps now, and the landing-energy check is a floor
// derived from the drop height rather than "greater than zero", so a future
// change that makes real impacts report quietly fails here instead of passing.
Status GateAudioImpact(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  Physics& phys = c.phys;
  DebrisSystem& debris = c.debris;

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  debris.Reset();

  // Absolute coordinates, like the debris gate next door: this gate runs
  // before `streaming` moves the residency window, and it establishes its own
  // world with SubmitWorldgen above.
  const int px = 100, pz = 100;
  const int h = World::TerrainHeight(px, pz, kDefaultSeed);
  const int slabY = h + 6;   // a flat stone table above the hillside
  const int dropY = slabY + 26;
  uint32_t t = 3000;

  auto tick = [&](std::vector<BrushOp> ops) {
    std::vector<CellOp> cellOps;
    std::vector<ParticleSpawn> spawns;
    debris.PreTick(t + 1, world, cellOps, spawns);
    ++t;
    SubmitTick(ctx, world, sim, t, kDefaultSeed, ops, {}, cellOps, false,
               {px / 16, slabY / 16, pz / 16}, true, false, spawns);
    ctx.WaitIdle();
    ctx.ProcessEvents();
    phys.Step(kTickDt);
    debris.PostStep();
  };

  // Lay the slab and let the chunk readbacks catch up: DebrisSystem builds a
  // marching-cubes collision patch from the CACHED chunk, so a body dropped
  // before the cache has the slab falls straight through it.
  {
    std::vector<CellOp> pad;
    for (int z = -6; z <= 6; z++)
      for (int x = -6; x <= 6; x++)
        for (int y = slabY - 3; y <= slabY; y++)
          pad.push_back(
              {World::SlotCellIndex({px + x, y, pz + z}), (uint32_t)kMatStone});
    std::vector<ParticleSpawn> spawns;
    debris.PreTick(t + 1, world, pad, spawns);   // appends to `pad`
    ++t;
    SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, pad, false,
               {px / 16, slabY / 16, pz / 16}, true, false, spawns);
    ctx.WaitIdle();
    ctx.ProcessEvents();
    phys.Step(kTickDt);
    debris.PostStep();
  }
  for (int i = 0; i < 24; i++) tick({});

  // The falling block: a 3x3x3 stone cube, created in Jolt and handed to the
  // debris system exactly the way a severed limb is. AdoptBody is what puts it
  // in bodies_, which is what makes its contacts resolvable to a material —
  // and is why a LIVE mob limb (owned by MobSystem, not here) cannot fire one.
  std::vector<DebrisVoxel> vox;
  for (int8_t z = 0; z < 3; z++)
    for (int8_t y = 0; y < 3; y++)
      for (int8_t x = 0; x < 3; x++)
        vox.push_back(DebrisVoxel{x, y, z, 0, (uint16_t)kMatStone});
  std::vector<float> density(c.mats.size(), 1000.0f);
  for (size_t i = 0; i < c.mats.size(); i++)
    density[i] = std::max(1.0f, (float)c.mats[i].gpu.density);
  const uint64_t bh = phys.CreateDebrisBody(vox, {px, dropY, pz}, density);
  if (bh == 0) {
    detail = "Jolt refused the test body";
    std::printf("audio impact: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  BodyTransform xf{};
  xf.pos = Vec3{(float)px, (float)dropY, (float)pz};
  xf.quat[3] = 1;
  debris.AdoptBody(bh, vox, xf);

  // Fall + land. Impacts accumulate because nothing in the harness drains
  // them (main.cpp's ClearImpactEvents is a frame-loop concern), which is
  // precisely what lets a test count them.
  int landTick = -1;
  for (int i = 0; i < 90 && landTick < 0; i++) {
    tick({});
    if (!debris.ImpactEvents().empty()) landTick = i;
  }
  const size_t landed = debris.ImpactEvents().size();
  DebrisSystem::ImpactEvent first{};
  if (landed) first = debris.ImpactEvents()[0];

  // The quiet window. Everything reported from here on is a settling contact,
  // and there must be none of it.
  //
  // WHICH TICK, AND HOW HARD (CLAUDE.md rule 6). "1 during 150 settled ticks"
  // is a bare count and it cannot tell the two failures apart: the tail of a
  // landing arriving a tick late (the loop above breaks on the FIRST contact,
  // which is not the same event as the body coming to rest) is a fixture that
  // opened its window too early, and a contact at tick 90 of 150 is the
  // resting-contact machine gun this gate exists for. They want opposite fixes.
  debris.ClearImpactEvents();
  std::string late;
  size_t seen = 0;
  for (int i = 0; i < 150; i++) {
    tick({});
    for (size_t k = seen; k < debris.ImpactEvents().size(); k++) {
      const DebrisSystem::ImpactEvent& e = debris.ImpactEvents()[k];
      late += Format("%s+%d tick, energy %.2f, y %.1f", late.empty() ? "" : "; ",
                     i, e.energy, e.posVoxel.y);
    }
    seen = debris.ImpactEvents().size();
  }
  const size_t afterSettle = debris.ImpactEvents().size();

  const bool fired = landed > 0;
  // The struck surface, not the striker. Both happen to be stone here, so the
  // real assertion is that it resolved to SOMETHING in the material table
  // rather than to 0 (which would mean "we could not tell what was hit" and
  // would leave the cue silent in play).
  const bool matOk = fired && first.material == kMatStone;
  // Energy in (0,1] AND LOUD ENOUGH FOR THE DROP. "> 0" could not tell a
  // correct landing from one reported at half speed, which is exactly what a
  // grazing contact looks like -- so the floor is derived from the fixture:
  // free fall over (dropY - slabY) voxels, through the same ramp CollectImpacts
  // applies, at 60% to leave room for the block's last partial step and for the
  // tuning moving underneath. A cue that goes quiet fails here.
  const auto& ta = CurrentTuning().audio;
  const float wantMs = std::sqrt(2.0f * CurrentTuning().physics.gravity *
                                 (float)(dropY - slabY) * kVoxelMeters);
  const float full = std::max(ta.impactFullSpeed, ta.impactMinSpeed + 0.1f);
  const float wantEnergy = std::clamp(
      (wantMs - ta.impactMinSpeed) / (full - ta.impactMinSpeed), 0.0f, 1.0f);
  const bool energyOk = fired && first.energy <= 1.0f &&
                        first.energy >= 0.6f * wantEnergy;
  // Within a body-length of the slab surface, in the column we dropped into.
  const bool posOk = fired && std::abs(first.posVoxel.x - (float)px) < 10.0f &&
                     std::abs(first.posVoxel.z - (float)pz) < 10.0f &&
                     first.posVoxel.y > (float)slabY - 12.0f &&
                     first.posVoxel.y < (float)dropY;
  const bool quietOk = afterSettle == 0;

  const bool ok = fired && matOk && energyOk && posOk && quietOk;
  detail = Format(
      "%zu impact(s) on landing at tick %d (mat %u, energy %.2f of the %.2f a "
      "%.1f m/s drop implies (floor %.2f), y %.1f vs slab %d); %zu during 150 "
      "settled ticks%s%s%s",
      landed, landTick, first.material, first.energy, wantEnergy, wantMs,
      0.6f * wantEnergy, first.posVoxel.y, slabY,
      afterSettle, late.empty() ? "" : " [", late.c_str(),
      late.empty() ? "" : "]");
  std::printf("audio impact: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- mob voices: hurt and death ---------------------------------------------
//
// The two events DESIGN.md listed as having "no damage/death site calling
// them". Both are asserted at the MobSystem reporting layer, where they are
// produced — main.cpp's job is only to map VoiceKind onto Cues::MobEvent, and
// a test that went through Cues would need a sound device to see anything.
//
// The third claim here is the de-duplication: a burst of damage in one drain
// window is ONE cry. That is the property that keeps a laser held on a mob
// from queueing an unbounded pile of events on a machine with audio off.
Status GateAudioMobVoice(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  Physics& phys = c.phys;
  DebrisSystem& debris = c.debris;
  MobSystem& mobs = c.mobs;

  if (mobs.Defs().empty()) {
    detail = "no mob defs — assets/mobs/ has no loadable sidecar";
    std::printf("audio mob voice: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  // By name, for the reason the mob gate documents: defs load in filename
  // order, so a positional index silently re-points at another rig.
  int dummyDef = 0;
  for (size_t i = 0; i < mobs.Defs().size(); i++)
    if (mobs.Defs()[i].name == "dummy") dummyDef = (int)i;
  const MobDef& dd = mobs.Defs()[(size_t)dummyDef];
  auto limbIndex = [&](const char* name) {
    for (size_t i = 0; i < dd.limbs.size(); i++)
      if (dd.limbs[i].name == name) return (int)i;
    return -1;
  };

  const int h = World::TerrainHeight(150, 150, kDefaultSeed);
  uint32_t t = 7000;
  auto tick = [&]() {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    mobs.PreTick(t + 1, world, ops, cellOps, spawns);
    debris.PreTick(t + 1, world, cellOps, spawns);
    ++t;
    SubmitTick(ctx, world, sim, t, kDefaultSeed, ops, {}, cellOps, false,
               {8, h / 16, 8}, true, false, spawns);
    ctx.WaitIdle();
    ctx.ProcessEvents();
    phys.Step(kTickDt);
    debris.PostStep();
    mobs.PostStep();
  };

  const uint64_t id = mobs.Spawn(dummyDef, {147, h + 1, 149});
  if (id == 0) {
    detail = "mob spawn failed";
    std::printf("audio mob voice: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  for (int i = 0; i < 12; i++) tick();

  // ---- hurt -----------------------------------------------------------------
  mobs.ClearVoiceEvents();
  const int arm = limbIndex("arm.L");
  const uint64_t armBody = mobs.LimbBody(id, arm);
  Vec3 armPos = mobs.MobOrigin(id);
  // A scratch: well under the limb's hp, so it must NOT sever (a sever reports
  // through SeverEvents and would make this gate pass for the wrong reason).
  mobs.Damage(armBody, 1.0f, armPos);
  const size_t afterOne = mobs.VoiceEvents().size();
  // Two more hits in the same drain window. One cry, not three.
  mobs.Damage(armBody, 1.0f, armPos);
  mobs.Damage(armBody, 1.0f, armPos);
  const size_t afterThree = mobs.VoiceEvents().size();

  bool hurtOk = afterOne == 1 && afterThree == 1 && mobs.IsAlive(id);
  float hurtIntensity = 0.0f;
  if (afterOne == 1) {
    const MobSystem::VoiceEvent& v = mobs.VoiceEvents()[0];
    hurtIntensity = v.intensity;
    hurtOk = hurtOk && v.kind == MobSystem::VoiceKind::Hurt && v.mobId == id &&
             v.defIndex == dummyDef && v.intensity > 0.0f &&
             v.intensity <= 1.0f;
  }

  // ---- death ----------------------------------------------------------------
  mobs.ClearVoiceEvents();
  mobs.Sever(id, limbIndex("head"));  // vital: routes through Die()
  size_t deaths = 0;
  MobSystem::VoiceEvent death{};
  for (const MobSystem::VoiceEvent& v : mobs.VoiceEvents())
    if (v.kind == MobSystem::VoiceKind::Death) {
      deaths++;
      death = v;
    }
  // Exactly one, carrying the def index — the mob itself is gone by the time
  // a frame would drain this, which is why the index rides on the event.
  const bool deathOk = deaths == 1 && death.mobId == id &&
                       death.defIndex == dummyDef && !mobs.IsAlive(id);

  // A corpse says nothing more, however long it ragdolls.
  mobs.ClearVoiceEvents();
  for (int i = 0; i < 60; i++) tick();
  const size_t afterDeath = mobs.VoiceEvents().size();
  const bool quietOk = afterDeath == 0;

  const bool ok = hurtOk && deathOk && quietOk;
  detail = Format(
      "hurt: %zu event from 1 hit, %zu from 3 (intensity %.3f); death: %zu "
      "event (def %d); %zu voices from a corpse over 60 ticks",
      afterOne, afterThree, hurtIntensity, deaths, death.defIndex, afterDeath);
  std::printf("audio mob voice: %s (%s)\n", ok ? "PASS" : "FAIL",
              detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- ambience: the material bed's driver -------------------------------------
//
// Cues::ProbeAmbience is deliberately split out of the voice so it can be
// asserted with no audio device — this gate builds the Cues object but never
// calls Init(), so no device is opened and nothing plays. What is under test
// is the DRIVER: does a body of water near the player produce a position and a
// weight, and does a world without one produce nothing at all.
Status GateAudioAmbience(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  audio::Cues cues;
  // Material table only: resolves which materials AUTHOR an ambience slot,
  // which is all the probe reads. Init() (device, library) is never called.
  cues.RebuildMaterialTable(c.mats);
  if (!cues.AnyAmbienceMaterial()) {
    detail = "no material binds an \"ambience\" slot in materials.json";
    std::printf("audio ambience: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  const int px = 200, pz = 200;
  const int h = World::TerrainHeight(px, pz, kDefaultSeed);
  uint32_t t = 9000;
  auto tick = [&](std::vector<BrushOp> ops) {
    ++t;
    SubmitTick(ctx, world, sim, t, kDefaultSeed, ops, {}, {}, false,
               {px / 16, h / 16, pz / 16}, true, false);
    ctx.WaitIdle();
    ctx.ProcessEvents();
  };

  const Vec3 eye{(float)px, (float)h + 2.0f, (float)pz};

  // ---- dry: nothing nearby ---------------------------------------------------
  for (int i = 0; i < 6; i++) tick({});
  const audio::Cues::AmbienceProbe dry = cues.ProbeAmbience(world, eye);

  // ---- wet: a pond ------------------------------------------------------------
  // Water is painted into a stone bowl so it stays put long enough to be
  // sampled: an unconfined blob would spread over the hillside and out of the
  // mirror while the readback catches up.
  const int wy = h + 4;
  for (int i = 0; i < 3; i++) tick({{px, wy, pz, 11, kMatStone, 1, 0, 0}});
  for (int i = 0; i < 3; i++) tick({{px, wy + 2, pz, 8, kMatAir, 1, 0, 0}});
  for (int i = 0; i < 6; i++) tick({{px, wy + 2, pz, 7, kMatWater, 1, 0, 0}});
  for (int i = 0; i < 6; i++) tick({});
  const audio::Cues::AmbienceProbe wet = cues.ProbeAmbience(world, eye);

  const bool dryOk = dry.material == 0 && dry.cells == 0;
  const bool foundOk = wet.material == kMatWater;
  const bool weightOk = foundOk && wet.weight > 0.0f && wet.weight <= 1.0f;
  // The centroid must be ON the pond, not on the listener: an emitter that
  // tracks the player pans to nothing, which is the failure mode this gate
  // exists to catch.
  const bool posOk = foundOk && std::abs(wet.posVox.x - (float)px) < 12.0f &&
                     std::abs(wet.posVox.z - (float)pz) < 12.0f &&
                     std::abs(wet.posVox.y - (float)(wy + 2)) < 12.0f;

  const bool ok = dryOk && foundOk && weightOk && posOk;
  detail = Format(
      "dry world: mat %u / %d cells; pond: mat %u, %d cells, weight %.2f, "
      "centroid (%.0f,%.0f,%.0f) vs pond (%d,%d,%d)",
      dry.material, dry.cells, wet.material, wet.cells, wet.weight,
      wet.posVox.x, wet.posVox.y, wet.posVox.z, px, wy + 2, pz);
  std::printf("audio ambience: %s (%s)\n", ok ? "PASS" : "FAIL",
              detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- spatial: WHICH EAR THE SOUND COMES OUT OF -------------------------------
//
// THE ONE GATE IN THIS FILE THAT DOES LISTEN, and the note at the top of the
// file is why it had to be written: "these assert on events, never on sound"
// was true, and it left the entire listener transform — the axis map, the
// listener yaw, the sign of the pitch — with no coverage whatsoever. It shipped
// broken for a month (fixed 2026-09-19): a bare `(x, y_up, z) -> (x, z, y_up)`
// swizzle has determinant -1, so it REFLECTED the sound field rather than
// rotating it, and a sound dead ahead of the player came out of the right
// speaker at every yaw; pitch was negated on top, so looking up put a blade
// swinging at chest height overhead. Both were reported by ear, by the player,
// and no gate in the suite had an opinion.
//
// It needs no device: AudioWorld::Init only prepares the voice pool, and
// Render() is a pure function of the params and the sample. So this is real DSP
// output — twelve camera poses, a wideband burst placed two metres away along a
// direction taken from `Camera`'s OWN basis (never a hand-rolled one — the
// camera's is left-handed and that is exactly the trap this gate guards), and
// the assertion is on the energy that comes out of each ear.
//
// The claims are SIGNS, not magnitudes. "Right is louder on the right" survives
// any tuning of the head-shadow filters, the distance curve or the pinna EQ;
// "right is 3.4 dB louder" would have to be re-pinned every time somebody
// touched one, and that is how a gate becomes something people delete. The
// measured values are recorded as observations instead.

// One probe: a fresh world (so no voice reuses another probe's smoother state),
// one burst, a quarter second of render. Returns per-ear energy.
struct EarEnergy {
  double left = 0, right = 0;
  double hfSum = 0;   // high-frequency content of the sum, for elevation
};

EarEnergy RenderProbe(const Vec3& earVox, float yaw, float pitch,
                      const Vec3& srcVox, const std::vector<float>& burst) {
  audio::AudioWorld aw;
  audio::ListenerPose lp;
  lp.posVox = earVox;
  lp.yaw = yaw;
  lp.pitch = pitch;
  aw.Init(48000.0, lp);
  aw.SetMasterGain(1.0f);
  aw.Update(lp, nullptr);

  audio::VoiceConfig cfg;
  cfg.gain = 1.0f;
  cfg.audibleRadius = 40.0f;
  cfg.verbWet = 0.0f;   // reverb is diffuse by design and would blur the ILD
  cfg.doppler = false;
  if (!aw.PlayOneShot(&burst, srcVox, cfg)) return EarEnergy{};

  EarEnergy e;
  std::vector<float> out(2 * 512, 0.0f);
  float prevSum = 0.0f;
  for (int block = 0; block < 24; block++) {   // 24 x 512 = 0.26 s
    std::fill(out.begin(), out.end(), 0.0f);
    aw.Render(out.data(), 512);
    for (int i = 0; i < 512; i++) {
      const float l = out[2 * i], r = out[2 * i + 1];
      e.left += (double)l * l;
      e.right += (double)r * r;
      const float sum = l + r;
      const float d = sum - prevSum;   // one-pole difference = crude HF meter
      prevSum = sum;
      e.hfSum += (double)d * d;
    }
  }
  return e;
}

Status GateAudioSpatial(Ctx& c, std::string& detail) {
  (void)c;   // no world, no GPU: this gate is the mixer and nothing else

  // A wideband burst. Deterministic LCG rather than <random>, so the gate's
  // numbers are the same on every machine and in every run.
  std::vector<float> burst((size_t)(48000 * 0.18));
  uint32_t s = 0x1234567u;
  for (size_t i = 0; i < burst.size(); i++) {
    s = s * 1664525u + 1013904223u;
    const float n = (float)((int32_t)(s >> 8) % 20001 - 10000) / 10000.0f;
    // Short fades: a hard edge on a 180 ms burst is a click, and a click is
    // broadband energy in the wrong place for the HF meter.
    const float ramp = 0.006f * (float)burst.size();
    const float aIn = std::min(1.0f, (float)i / ramp);
    const float aOut = std::min(1.0f, (float)(burst.size() - i) / ramp);
    burst[i] = n * 0.5f * std::min(aIn, aOut);
  }

  const Vec3 ear{100.0f, 40.0f, 100.0f};
  const float distVox = 2.0f / kVoxelMeters;   // two metres out

  struct Pose { float yaw, pitch; };
  const Pose poses[] = {{0.0f, 0.0f},     {1.5708f, 0.0f}, {3.1416f, 0.0f},
                        {2.35f, 0.0f},    {0.7f, 1.2f},    {2.35f, 1.2f},
                        {0.7f, -1.2f},    {2.35f, -1.2f}};

  int fails = 0;
  std::string firstFail;
  double worstFrontSkew = 0.0, worstSideRatio = 1e9;
  for (const Pose& p : poses) {
    Camera cam;
    cam.yaw = p.yaw;
    cam.pitch = p.pitch;
    // THE CAMERA'S OWN BASIS. `Camera::Right()` is `Forward() x +Y`, which makes
    // the basis left-handed (right is -X when forward is +Z); building a
    // textbook right-handed one here would only test this gate's arithmetic
    // against itself, and the defect being guarded IS a handedness mistake.
    const Vec3 fwd = cam.Forward(), right = cam.Right();

    const EarEnergy front = RenderProbe(ear, p.yaw, p.pitch, ear + fwd * distVox, burst);
    const EarEnergy rt = RenderProbe(ear, p.yaw, p.pitch, ear + right * distVox, burst);
    const EarEnergy lf = RenderProbe(ear, p.yaw, p.pitch, ear - right * distVox, burst);

    // 1. STRAIGHT AHEAD IS CENTRED. This is the assertion that fails loudest on
    //    a reflected frame: the old code put a source dead ahead hard right at
    //    every yaw, which is precisely the bug report.
    const double fTot = front.left + front.right;
    const double frontSkew =
        fTot > 0 ? std::abs(front.left - front.right) / fTot : 1.0;
    // 2. THE RIGHT-HAND SIDE COMES OUT OF THE RIGHT EAR, and vice versa.
    const double rRatio = rt.left > 0 ? rt.right / rt.left : 0.0;
    const double lRatio = lf.right > 0 ? lf.left / lf.right : 0.0;

    worstFrontSkew = std::max(worstFrontSkew, frontSkew);
    worstSideRatio = std::min(worstSideRatio, std::min(rRatio, lRatio));

    const bool ok = frontSkew < 0.25 && rRatio > 1.15 && lRatio > 1.15;
    if (!ok) {
      fails++;
      if (firstFail.empty())
        firstFail = Format(
            "yaw %.2f pitch %.2f: front skew %.3f (want <0.25), right/left "
            "%.2f, left/right %.2f (want >1.15)",
            p.yaw, p.pitch, frontSkew, rRatio, lRatio);
    }
  }

  // 3. ELEVATION IS NOT UPSIDE DOWN — TESTED AS AN INVARIANCE, NOT AS A TIMBRE.
  //
  //    The obvious test ("overhead should sound brighter than underfoot") is a
  //    claim about the ENGINE's pinna model, not about this conversion, and it
  //    is not even true of this engine as measured: the floor bounce combs a
  //    delayed copy into anything low, which puts high-frequency energy back.
  //    A gate that asserts it is asserting somebody else's design.
  //
  //    What is genuinely ours is that PITCH ROTATES THE SOURCE THE RIGHT WAY.
  //    So: turn the head instead of moving the source, and check that two poses
  //    which put the source in the SAME place relative to the face measure the
  //    same. Looking up 1.5 rad, a sound directly overhead is nearly dead ahead
  //    of your face, and a sound level with you is nearly underfoot. Reverse the
  //    sign of the pitch and those two swap — so the test is that each pitched
  //    probe is nearer its own reference than the other one. Whatever the pinna
  //    model does with elevation, it does the same thing to both members of a
  //    pair, which is why this survives tuning and the brightness test does not.
  const Vec3 up{0.0f, 1.0f, 0.0f};
  Camera flat;
  flat.yaw = 0.7f;
  flat.pitch = 0.0f;
  const Vec3 flatFwd = flat.Forward();
  auto bright = [](const EarEnergy& e) {
    const double tot = e.left + e.right;
    return tot > 0 ? e.hfSum / tot : 0.0;
  };
  // References, head level: dead ahead, and straight underfoot.
  const double refAhead =
      bright(RenderProbe(ear, 0.7f, 0.0f, ear + flatFwd * distVox, burst));
  const double refUnder =
      bright(RenderProbe(ear, 0.7f, 0.0f, ear - up * distVox, burst));
  // The same two places relative to the FACE, reached by pitching up instead.
  const double upAhead =
      bright(RenderProbe(ear, 0.7f, 1.5f, ear + up * distVox, burst));
  const double upUnder =
      bright(RenderProbe(ear, 0.7f, 1.5f, ear + flatFwd * distVox, burst));

  // COMPARED AS A RANKING, NOT AS A DISTANCE. The two pitched probes sit at
  // different WORLD positions by construction, and the early-reflection stage
  // images the room from the world position, so each pitched measurement carries
  // a common-mode offset from its reference (measured: about -0.35 on this
  // metric, in the same direction for both). That offset sinks a nearest-match
  // test and leaves the ORDER untouched — so the claim is the order:
  //
  //   head level:      ahead is brighter than underfoot          (the reference)
  //   head pitched up: overhead is brighter than level-with-you  (must agree)
  //
  // Negate the pitch and it does not merely weaken, it INVERTS: overhead becomes
  // behind-you (rear shadow, dull) and level-with-you becomes the zenith (+5 dB
  // pinna peak, bright), so the sign flips. That is the whole defect, in one
  // comparison, with no dependence on what the pinna model does in absolute
  // terms.
  const double refGap = refAhead - refUnder;
  const double upGap = upAhead - upUnder;
  // Resolution check first: if the metric cannot separate the zenith from the
  // nadir at all, the ranking below means nothing and the gate must say so
  // rather than pass (a fixture that cannot fail measures nothing).
  if (std::abs(refGap) < 0.1) {
    fails++;
    if (firstFail.empty())
      firstFail = Format(
          "elevation metric has no resolution: ahead %.4f vs underfoot %.4f",
          refAhead, refUnder);
  } else if ((upGap > 0.0) != (refGap > 0.0) || std::abs(upGap) < 0.05) {
    fails++;
    if (firstFail.empty())
      firstFail = Format(
          "pitch rotates the wrong way: level head ranks ahead %.4f over "
          "underfoot %.4f (gap %+.4f), but pitched up 1.5 rad it ranks overhead "
          "%.4f over level-with-you %.4f (gap %+.4f)",
          refAhead, refUnder, refGap, upAhead, upUnder, upGap);
  }

  RecordObserved("audioSpatial.worstFrontSkew", worstFrontSkew);
  RecordObserved("audioSpatial.worstSideRatio", worstSideRatio);
  RecordObserved("audioSpatial.brightAhead", refAhead);
  RecordObserved("audioSpatial.brightUnder", refUnder);
  RecordObserved("audioSpatial.brightPitchedAhead", upAhead);
  RecordObserved("audioSpatial.brightPitchedUnder", upUnder);

  const bool ok = fails == 0;
  detail = ok ? Format(
                    "8 poses: worst front skew %.3f, worst side ratio %.2fx; "
                    "pitch invariance ahead %.4f~%.4f, under %.4f~%.4f",
                    worstFrontSkew, worstSideRatio, upAhead, refAhead, upUnder,
                    refUnder)
              : Format("%d of 9 checks failed; first: %s", fails,
                       firstFail.c_str());
  std::printf("audio spatial: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& AudioGates() {
  static const std::vector<Gate> g = {
      {"audio-impact", "audio", {}, false, GateAudioImpact},
      {"audio-mob-voice", "audio", {}, false, GateAudioMobVoice},
      {"audio-ambience", "audio", {}, false, GateAudioAmbience},
      {"audio-spatial", "audio", {}, false, GateAudioSpatial},
  };
  return g;
}

}  // namespace selftest
