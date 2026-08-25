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
#include "sim/materials.h"
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
  for (int i = 0; i < 4; i++)
    tick({{px, slabY, pz, 6, kMatStone, 1, 0, 0}});
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
  debris.ClearImpactEvents();
  for (int i = 0; i < 150; i++) tick({});
  const size_t afterSettle = debris.ImpactEvents().size();

  const bool fired = landed > 0;
  // The struck surface, not the striker. Both happen to be stone here, so the
  // real assertion is that it resolved to SOMETHING in the material table
  // rather than to 0 (which would mean "we could not tell what was hit" and
  // would leave the cue silent in play).
  const bool matOk = fired && first.material == kMatStone;
  // Energy in (0,1]: a 26-voxel fall is well past the gate but the mapping
  // must still be a ramp and not a constant.
  const bool energyOk = fired && first.energy > 0.0f && first.energy <= 1.0f;
  // Within a body-length of the slab surface, in the column we dropped into.
  const bool posOk = fired && std::abs(first.posVoxel.x - (float)px) < 10.0f &&
                     std::abs(first.posVoxel.z - (float)pz) < 10.0f &&
                     first.posVoxel.y > (float)slabY - 12.0f &&
                     first.posVoxel.y < (float)dropY;
  const bool quietOk = afterSettle == 0;

  const bool ok = fired && matOk && energyOk && posOk && quietOk;
  detail = Format(
      "%zu impact(s) on landing at tick %d (mat %u, energy %.2f, y %.1f vs "
      "slab %d); %zu during 150 settled ticks",
      landed, landTick, first.material, first.energy, first.posVoxel.y, slabY,
      afterSettle);
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
    detail = "no mob defs — run scripts/gen_test_mob.py";
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
    mobs.PreTick(t + 1, world, ops, spawns);
    std::vector<CellOp> cellOps;
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

}  // namespace

const std::vector<Gate>& AudioGates() {
  static const std::vector<Gate> g = {
      {"audio-impact", "audio", {}, false, GateAudioImpact},
      {"audio-mob-voice", "audio", {}, false, GateAudioMobVoice},
      {"audio-ambience", "audio", {}, false, GateAudioAmbience},
  };
  return g;
}

}  // namespace selftest
