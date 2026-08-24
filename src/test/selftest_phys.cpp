// selftest_phys.cpp — phys selftest gates.
//
// Bodies moved verbatim out of the old monolithic RunSelftest; see
// scripts/split_selftest.py for the exact source ranges. Each gate returns a
// Status and fills `detail` with the parenthetical the old printf carried, so
// the console output is unchanged and --json can carry the same numbers.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "game/bodyreg.h"
#include "game/brush.h"
#include "game/camera.h"
#include "game/player.h"
#include "gpu/resources.h"
#include "sim/voxload.h"  // kArtPaletteBase
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// ---- debris ------------------------------------------------------------
Status GateDebris(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  Physics& phys = c.phys;
  DebrisSystem& debris = c.debris;
  const uint32_t W = c.width;
  const uint32_t H = c.height;
// M6 debris: build a stone arm held up by one pillar, blast the pillar,
// and require the arm to (a) get detected as an island, (b) fall as a Jolt
// body onto marching-cubes terrain, (c) go to sleep.
bool debrisOk = false;
{
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  int h = World::TerrainHeight(60, 60, kDefaultSeed);
  uint32_t t = 2000;
  uint32_t bodiesSeen = 0;

  for (int i = 0; i < 420; i++) {
    std::vector<BrushOp> ops;
    if (i < 8) {
      // pillar: stacked stone spheres; arm: a bar of spheres at the top
      ops.push_back({60, h + 2 + i * 3, 60, 2, kMatStone, 1, 0, 0});
      if (i < 3) ops.push_back({60 + 6 * (i + 1), h + 22, 60, 3, kMatStone, 1, 0, 0});
      ops.push_back({60, h + 22, 60, 3, kMatStone, 1, 0, 0});
    }
    std::vector<ExplosionOp> exps;
    if (i == 40) {
      exps.push_back({60, h + 10, 60, 7, 500, 0, 0, 0});
      debris.AddDestructionEvent(t + 1, {50, h, 50}, {84, h + 26, 70});
    }

    debris.QueueSupportEvents(world.Snap());
    std::vector<CellOp> cellOps;
    std::vector<ParticleSpawn> spawns;
    debris.PreTick(t + 1, world, cellOps, spawns);
    ++t;
    SubmitTick(ctx, world, sim, t, kDefaultSeed, ops, exps, cellOps, false,
               {3, (h + 10) / 16, 3}, true, i >= 40 && i < 380, spawns);
    ctx.WaitIdle();
    ctx.ProcessEvents();
    phys.Step(kTickDt);
    debris.PostStep();
    bodiesSeen = std::max(bodiesSeen, debris.BodyCount());
  }
  uint32_t awake = debris.ActiveBodyCount();
  debrisOk = bodiesSeen >= 1 && awake == 0;
  std::printf("debris: %s (%u bodies spawned, %u awake after settling, "
              "%u events pending)\n",
              debrisOk ? "PASS" : "FAIL", bodiesSeen, awake,
              debris.PendingEvents());

  // visual proof: render the settled debris field to screenshot_debris.bmp
  // (through the ONE slot walk — game/bodyreg.h — like every render path).
  // Both backends since phase 4b; the verdict above is still compute-only.
  if (debris.BodyCount() > 0) {
    BodyRegistry bodyReg(debris, c.mobs, nullptr);
    std::vector<BodyVoxInst> inst;
    bodyReg.BuildInstances(inst);
    ctx.queue.WriteBuffer(world.bodyInstances, 0, inst.data(),
                          inst.size() * sizeof(BodyVoxInst));
    std::vector<BodyXformGpu> xf;
    bodyReg.BuildXforms(xf);
    ctx.queue.WriteBuffer(world.bodyXforms, 0, xf.data(),
                          xf.size() * sizeof(BodyXformGpu));

    const uint32_t W = 1280, H = 720;
    rhi::Texture tex = ctx.device.CreateTexture({W, H, 1}, rhi::TextureFormat::RGBA8Unorm, rhi::TextureUsage::RenderAttachment | rhi::TextureUsage::CopySrc, "offscreen");
    Camera cam2;
    cam2.yaw = -2.356f;
    cam2.pitch = -0.32f;
    Vec3 eye{60.0f + 34, (float)h + 26, 60.0f + 34};
    WriteRenderParams(ctx.queue, world, eye, cam2, (float)W / H, true, 0);
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    rhi::RenderPass rp = sim.BeginRenderPass(
        enc, tex.CreateView(), rhi::TextureFormat::RGBA8Unorm, W, H);
    sim.DrawWorld(rp);
    sim.DrawParticles(rp);
    sim.DrawBodies(rp, (uint32_t)inst.size());
    rp.End();
    rhi::Buffer shot = CreateBuffer(ctx.device, (uint64_t)W * H * 4,
                                     rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                                     "debrisShot");
    rhi::TexelCopyTexture srcT{};
    srcT.texture = tex;
    rhi::TexelCopyBuffer dstB{};
    dstB.buffer = shot;
    dstB.bytesPerRow = W * 4;
    dstB.rowsPerImage = H;
    rhi::Extent3D ext{W, H, 1};
    enc.CopyTextureToBuffer(srcT, dstB, ext);
    ctx.queue.Submit(enc.Finish());
    std::vector<uint8_t> pixels(W * H * 4);
    bool got = false;
    got = rhi::ReadBufferBlocking(ctx.device, shot, 0, pixels.data(), (size_t)(pixels.size()));
    if (got && WriteBmpFile("screenshot_debris.bmp", pixels, W, H))
      std::printf("wrote screenshot_debris.bmp\n");
  }
}

  // Verdict: the flag the moved body already computed.
  return debrisOk ? Status::Pass : Status::Fail;
}

// ---- settle-back -------------------------------------------------------
Status GateSettleBack(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const std::vector<MaterialDef>& mats = c.mats;
  Physics& phys = c.phys;
  DebrisSystem& debris = c.debris;
  Stream& stream = c.stream;
// B6 settle-back: a dropped stone block must sleep, snap to the lattice,
// convert back into grid voxels through the op stream, and free its body —
// closing the grid -> body -> grid loop.
bool settleOk = false;
{
  debris.Reset();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  int h = World::TerrainHeight(80, 80, kDefaultSeed);
  std::vector<float> dens;
  for (const auto& m : mats) dens.push_back((float)m.gpu.density);
  std::vector<DebrisVoxel> vox;
  for (int z = 0; z < 3; z++)
    for (int y = 0; y < 3; y++)
      for (int x = 0; x < 3; x++)
        // PAINTED, deliberately: art colour is presentation only, so this
        // block must land in the grid as plain stone. See the check below.
        vox.push_back({(int8_t)x, (int8_t)y, (int8_t)z,
                       (uint8_t)kArtPaletteBase, kMatStone});
  uint64_t bh = phys.CreateDebrisBody(vox, {80, h + 4, 80}, dens);
  BodyTransform bxf{};
  bxf.pos = Vec3{80, (float)(h + 4), 80};
  bxf.quat[3] = 1;
  debris.AdoptBody(bh, vox, bxf);

  // Art colour must never reach a world cell: it is presentation state on a
  // body's skin, while the grid is hashed sim state (rule 1). A painted limb
  // that is severed and settles back has to become its plain MATERIAL — which
  // is checked here, on the ops themselves, because that is the one place the
  // colour could leak across. A failure here would mean every painted mob
  // silently desyncs multiplayer the first time a limb hits the ground.
  bool artStayedOut = true;
  uint32_t t = 8000;
  for (int i = 0; i < 360 && debris.BodyCount() > 0; i++) {
    std::vector<CellOp> cellOps;
    std::vector<ParticleSpawn> spawns;
    debris.PreTick(t + 1, world, cellOps, spawns);
    for (const CellOp& op : cellOps)
      if ((op.word & 0xFFFu) != kMatStone) artStayedOut = false;
    ++t;
    SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, cellOps, false,
               {5, h / 16, 5}, true, false, spawns);
    ctx.WaitIdle();
    ctx.ProcessEvents();
    phys.Step(kTickDt);
    debris.PostStep();
  }
  settleOk = debris.BodyCount() == 0 && debris.SettledBack() >= 1 && artStayedOut;
  std::printf("settle-back: %s (%u bodies converted to grid, %u still "
              "bodies, painted body settled as plain material=%d)\n",
              settleOk ? "PASS" : "FAIL", debris.SettledBack(),
              debris.BodyCount(), (int)artStayedOut);

  // C2 body split: a 3x3x9 bar cut through the middle must become two
  // independent bodies (no stepping needed — pure partition + respawn)
  std::vector<DebrisVoxel> bar;
  for (int z = 0; z < 9; z++)
    for (int y = 0; y < 3; y++)
      for (int x = 0; x < 3; x++)
        bar.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 0, kMatStone});
  uint64_t barBody = phys.CreateDebrisBody(bar, {500, 500, 500}, dens);
  BodyTransform barXf{};
  barXf.pos = Vec3{500, 500, 500};
  barXf.quat[3] = 1;
  debris.AdoptBody(barBody, bar, barXf);
  bool splitOk = debris.SplitBody(barBody, Vec3{501.5f, 501.5f, 504.5f},
                                  Vec3{0, 0, 1}) &&
                 debris.BodyCount() == 2;
  std::printf("body split: %s (%u bodies after cut)\n",
              splitOk ? "PASS" : "FAIL", debris.BodyCount());
  settleOk = settleOk && splitOk;
  debris.Reset();

  // body blast: an explosion must take VOXELS OFF a body, not just shove it.
  // A blast centred on the waist of a dumbbell has to cut it in two, so the
  // gate is both "lost voxels" and "ended up as more than one body" — the
  // second is what separates real damage from a cosmetic crater.
  {
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    int bh3 = World::TerrainHeight(120, 120, kDefaultSeed);
    // two 5x5x2 plates joined by a 1x1x3 neck: blasting the neck separates
    // them, and each plate is far above the 8-voxel body floor
    std::vector<DebrisVoxel> bar;
    for (int z = 0; z < 2; z++)
      for (int y = 0; y < 5; y++)
        for (int x = 0; x < 5; x++) {
          bar.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 0, kMatStone});
          bar.push_back({(int8_t)x, (int8_t)y, (int8_t)(z + 5), 0, kMatStone});
        }
    for (int z = 2; z < 5; z++)
      bar.push_back({2, 2, (int8_t)z, 0, kMatStone});
    uint32_t barVox = (uint32_t)bar.size();
    Vec3 barAt{120, (float)(bh3 + 8), 120};
    uint64_t bb = phys.CreateDebrisBody(bar, {120, bh3 + 8, 120}, dens);
    BodyTransform bxf{};
    bxf.pos = barAt;
    bxf.quat[3] = 1;
    debris.AdoptBody(bb, bar, bxf);

    std::vector<ParticleSpawn> bspawns;
    // centred on the neck (local 2,2,3 -> world), radius covers it only
    debris.DamageBodiesRadial(barAt + Vec3{2.5f, 2.5f, 3.5f}, 2.5f, world,
                              bspawns);
    uint32_t after = 0;
    {
      // Counted through the registry (no mobs exist in this gate, so the count
      // is the body's surviving voxels) — no slot-space list is built by hand.
      std::vector<BodyVoxInst> bi2;
      BodyRegistry(debris, c.mobs, nullptr).BuildInstances(bi2);
      after = (uint32_t)bi2.size();
    }
    bool blastOk = after < barVox && debris.BodyCount() >= 2;
    std::printf("body blast: %s (%u -> %u voxels, %u bodies, %zu ejecta)\n",
                blastOk ? "PASS" : "FAIL", barVox, after, debris.BodyCount(),
                bspawns.size());
    settleOk = settleOk && blastOk;
    debris.Reset();
  }

  // laser kerf on a body: repeated melts at one spot must bore through and
  // eventually sever it. Unlike the old plane split this removes matter, so
  // the gate is "voxels went away AND the body came apart" — a cut that only
  // separated (without eating a channel) would be the old behaviour back.
  {
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    int bh4 = World::TerrainHeight(150, 150, kDefaultSeed);
    std::vector<DebrisVoxel> rod;  // 3x3x11 rod: cut across the middle
    for (int z = 0; z < 11; z++)
      for (int y = 0; y < 3; y++)
        for (int x = 0; x < 3; x++)
          rod.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 0, kMatStone});
    uint32_t rodVox = (uint32_t)rod.size();
    Vec3 rodAt{150, (float)(bh4 + 8), 150};
    uint64_t rb = phys.CreateDebrisBody(rod, {150, bh4 + 8, 150}, dens);
    BodyTransform rxf{};
    rxf.pos = rodAt;
    rxf.quat[3] = 1;
    debris.AdoptBody(rb, rod, rxf);

    // Hold the beam on the middle of the rod for a few ticks. The handle is
    // re-read every iteration because each melt rebuilds the collider and
    // hands the body a new one — `rb` is stale after the first pass.
    std::vector<ParticleSpawn> lspawns;
    for (int i = 0; i < 12 && debris.BodyCount() < 2; i++)
      debris.MeltBodyAt(debris.BodyHandle(0), rodAt + Vec3{1.5f, 1.5f, 5.5f},
                        2.0f, world, lspawns);
    uint32_t lafter = 0;
    {
      std::vector<BodyVoxInst> bi3;
      BodyRegistry(debris, c.mobs, nullptr).BuildInstances(bi3);
      lafter = (uint32_t)bi3.size();
    }
    bool kerfOk = lafter < rodVox && debris.BodyCount() >= 2;
    std::printf("laser kerf: %s (%u -> %u voxels, %u bodies)\n",
                kerfOk ? "PASS" : "FAIL", rodVox, lafter, debris.BodyCount());
    settleOk = settleOk && kerfOk;
    debris.Reset();
  }

  // body burn: a rigidbody carrying embers must KEEP burning — embers decay
  // away (voxels leave the body) and emit real fire into the grid so nearby
  // flammables can catch. This is the fix for detached islands freezing
  // mid-flame forever.
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  int bh2 = World::TerrainHeight(90, 90, kDefaultSeed);
  std::vector<DebrisVoxel> plank;
  for (int z = 0; z < 5; z++)
    for (int y = 0; y < 5; y++)
      for (int x = 0; x < 5; x++)
        plank.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 0,
                         (uint16_t)(y == 4 ? kMatEmber : kMatWood)});
  uint32_t plankVoxels = (uint32_t)plank.size();
  uint64_t pb = phys.CreateDebrisBody(plank, {90, bh2 + 4, 90}, dens);
  BodyTransform pxf{};
  pxf.pos = Vec3{90, (float)(bh2 + 4), 90};
  pxf.quat[3] = 1;
  debris.AdoptBody(pb, plank, pxf);
  uint32_t fireOps = 0;
  t = 9000;
  for (int i = 0; i < 90 && debris.BodyCount() > 0; i++) {
    std::vector<CellOp> cellOps;
    std::vector<ParticleSpawn> spawns;
    debris.PreTick(t + 1, world, cellOps, spawns);
    for (const CellOp& op : cellOps)
      if ((op.word & 0xFFFu) == kMatFire) fireOps++;
    ++t;
    SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, cellOps, false,
               {5, bh2 / 16, 5}, true, false, spawns);
    ctx.WaitIdle();
    ctx.ProcessEvents();
    phys.Step(kTickDt);
    debris.PostStep();
  }
  std::vector<BodyVoxInst> burnInst;
  BodyRegistry(debris, c.mobs, nullptr).BuildInstances(burnInst);
  bool burnOk = fireOps > 5 && (uint32_t)burnInst.size() < plankVoxels;
  std::printf("body burn: %s (%u fire ops emitted, %u -> %zu voxels)\n",
              burnOk ? "PASS" : "FAIL", fireOps, plankVoxels,
              burnInst.size());
  settleOk = settleOk && burnOk;
  debris.Reset();

  // body shatter: burn through a dumbbell's ember bridge and the small
  // clump must disconnect and re-enter the world as ballistic particles
  // (the big plate keeps the body). Spin the body so it never settles.
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  std::vector<DebrisVoxel> bell;
  // The plate is 5x5, not 3x3, and that margin is the point. The ember does
  // not merely burn the bridge: it ignites the wood it touches, so the plate
  // is losing voxels the whole time the bridge is burning through. A 3x3
  // plate (9 voxels) erodes past the 8-voxel dissolve floor BEFORE the
  // connectivity check ever separates the clump, so the whole dumbbell went
  // to particles and the test measured erosion instead of shattering.
  // 25 voxels outlast the bridge with room to spare.
  for (int y = 0; y < 5; y++)  // 5x5x1 plate at z=0
    for (int x = 0; x < 5; x++)
      bell.push_back({(int8_t)x, (int8_t)y, 0, 0, kMatWood});
  bell.push_back({2, 2, 1, 0, kMatEmber});  // bridge, centred on the plate
  bell.push_back({2, 2, 2, 0, kMatWood});   // 4-voxel clump beyond it
  bell.push_back({1, 2, 2, 0, kMatWood});
  bell.push_back({3, 2, 2, 0, kMatWood});
  bell.push_back({2, 1, 2, 0, kMatWood});
  uint64_t db = phys.CreateDebrisBody(bell, {90, bh2 + 6, 90}, dens);
  BodyTransform dxf{};
  dxf.pos = Vec3{90, (float)(bh2 + 6), 90};
  dxf.quat[3] = 1;
  debris.AdoptBody(db, bell, dxf);
  phys.SetBodyVelocities(db, Vec3{0, 0, 0}, Vec3{0.4f, 1.2f, 0.3f});
  uint32_t spawnsSeen = 0;
  t = 10000;
  for (int i = 0; i < 400 && debris.BodyCount() > 0 && spawnsSeen < 3; i++) {
    std::vector<CellOp> cellOps;
    std::vector<ParticleSpawn> spawns;
    debris.PreTick(t + 1, world, cellOps, spawns);
    spawnsSeen += (uint32_t)spawns.size();
    ++t;
    SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, cellOps, false,
               {5, bh2 / 16, 5}, true, true, spawns);
    ctx.WaitIdle();
    ctx.ProcessEvents();
    phys.Step(kTickDt);
    debris.PostStep();
  }
  bool shatterOk = spawnsSeen >= 3 && debris.BodyCount() == 1;
  std::printf("body shatter: %s (%u fragment voxels -> particles, %u bodies)\n",
              shatterOk ? "PASS" : "FAIL", spawnsSeen, debris.BodyCount());
  settleOk = settleOk && shatterOk;
  debris.Reset();
}

  // Verdict: the flag the moved body already computed.
  return settleOk ? Status::Pass : Status::Fail;
}

// ---- player-body -------------------------------------------------------
Status GatePlayerBody(Ctx& c, std::string& detail) {
  const std::vector<MaterialDef>& mats = c.mats;
  Physics& phys = c.phys;
  DebrisSystem& debris = c.debris;
// player↔body (deferred from M6): the kinematic player proxy must register
// debris overlap as a depenetration push, and read clear when separated.
// Pure narrow-phase — no stepping between spawn and query, so deterministic.
bool pushOk = false;
{
  std::vector<float> dens;
  for (const auto& m : mats) dens.push_back((float)m.gpu.density);
  auto stoneBlock = [&](IVec3 origin) {
    std::vector<DebrisVoxel> vox;
    for (int z = 0; z < 3; z++)
      for (int y = 0; y < 3; y++)
        for (int x = 0; x < 3; x++)
          vox.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 0, kMatStone});
    return phys.CreateDebrisBody(vox, origin, dens);
  };
  uint64_t pb = phys.CreatePlayerBody(Player::kHalfXZ, Player::kHalfY);
  Vec3 at{500.0f, 500.0f, 500.0f};  // far from the debris-test terrain
  phys.MovePlayerBody(pb, at, kTickDt);
  phys.Step(kTickDt);  // proxy reaches its target
  uint64_t nearBody = stoneBlock({499, 499, 499});  // straddles the capsule
  float pushNear = phys.PlayerPushOut(pb, at).len();
  phys.RemoveBody(nearBody);
  uint64_t farBody = stoneBlock({520, 500, 500});
  float pushFar = phys.PlayerPushOut(pb, at).len();
  phys.RemoveBody(farBody);

  // mass-relative shove: the proxy is dynamic with a real mass, so walking
  // into a light sphere must move it far more than the same walk into a
  // heavy one (both fall freely — only horizontal displacement counts).
  auto walkInto = [&](float density) {
    at = Vec3{500.0f, 500.0f, 500.0f};
    phys.MovePlayerBody(pb, at, kTickDt);
    phys.Step(kTickDt);
    const float startX = 509.5f;  // just clear of capsule(4.8) + sphere(4)
    uint64_t s = phys.CreateSphereBody({startX, 500.0f, 500.0f}, 4.0f, density);
    for (int i = 0; i < 12; i++) {
      at.x += 2.2f;  // ~4.2 m/s walk
      phys.MovePlayerBody(pb, at, kTickDt);
      phys.Step(kTickDt);
    }
    BodyTransform xf{};
    phys.GetTransform(s, xf);
    phys.RemoveBody(s);
    return xf.pos.x - startX;
  };
  float lightMoved = walkInto(150.0f);    // ~10 kg beach ball
  float heavyMoved = walkInto(12000.0f);  // ~780 kg lead sphere
  phys.RemoveBody(pb);
  bool shoveOk = lightMoved > 2.0f && lightMoved > 3.0f * heavyMoved;
  pushOk = pushNear > 0.01f && pushFar < 1e-3f && shoveOk;
  std::printf(
      "player body: %s (overlap push %.2f vox, clear push %.3f vox, "
      "shove light %.1f vox vs heavy %.1f vox)\n",
      pushOk ? "PASS" : "FAIL", pushNear, pushFar, lightMoved, heavyMoved);
}

  // Verdict: the flag the moved body already computed.
  return pushOk ? Status::Pass : Status::Fail;
}

// ---- ragdoll-joints ----------------------------------------------------
//
// A ball joint used to be a bare point constraint: position welded, rotation
// completely free. That never showed while a mob was alive — limbs are
// kinematic then, and a constraint cannot move infinite mass — but a corpse
// could fold its thigh 180 degrees up through its own pelvis, and since one
// mob's limbs are deliberately excluded from colliding with each other
// (DisableCollisionsAmong), nothing else in the scene ever objected.
//
// Three claims, and the mob gate can make none of them: it loses every joint
// handle at death (MobSystem::Die hands the bodies to DebrisSystem), so the
// fixture has to be built here out of the primitive itself.
//
//   1. a limited joint CONTAINS a whipped limb,
//   2. an unlimited one does not — so claim 1 has teeth rather than measuring
//      a limb that was never pushed hard enough to reach its limit,
//   3. the limit is measured from the rig's REST pose even when the joint is
//      BUILT at a bent one. That is not hypothetical: MobSystem::Rebuild-
//      LimbBody re-creates a carved limb's joints from the live pose, so a
//      pose-relative frame would quietly re-centre every cone on a corpse
//      mid-fold and let it bend that far again from there.
Status GateRagdollJoints(Ctx& c, std::string& detail) {
  Physics& phys = c.phys;
  const std::vector<MaterialDef>& mats = c.mats;
  std::vector<float> dens;
  for (const auto& m : mats) dens.push_back((float)m.gpu.density);

  auto box = [](int sx, int sy, int sz) {
    std::vector<DebrisVoxel> v;
    for (int z = 0; z < sz; z++)
      for (int y = 0; y < sy; y++)
        for (int x = 0; x < sx; x++)
          v.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 0, kMatStone});
    return v;
  };
  // Well clear of the terrain the earlier phys gates leave behind: the thigh
  // has to be held by its joint, not caught by the floor.
  const Vec3 kHip{600.0f, 400.0f, 600.0f};
  const Vec3 kThighAnchorLocal{2.0f, 10.0f, 2.0f};  // top centre of a 4x10x4

  // Hang a "thigh" off a pinned "pelvis", whip it about every axis in turn,
  // and report the furthest it ever gets from its REST direction (straight
  // down). `preBend` rotates the child about the anchor BEFORE the joint is
  // created — the RebuildLimbBody case.
  auto whip = [&](float coneRad, float preBendRad) {
    const float s = std::sin(preBendRad), co = std::cos(preBendRad);
    // Rotate the local anchor about Z so the body origin lands where the
    // anchor still coincides with the hip: a constraint built on two points
    // that do not already agree yanks the bodies together on the first step,
    // and that transient is not what this is measuring.
    const Vec3 a = kThighAnchorLocal;
    const Vec3 rotA{a.x * co - a.y * s, a.x * s + a.y * co, a.z};

    BodyTransform hipXf{};
    hipXf.pos = kHip - Vec3{3.0f, 0.0f, 3.0f};
    hipXf.quat[3] = 1;
    uint64_t pelvis = phys.CreateDebrisBodyXf(box(6, 5, 6), hipXf, dens, true);
    phys.SetBodyKinematic(pelvis, true);  // pinned: the parent frame is world

    BodyTransform thighXf{};
    thighXf.pos = kHip - rotA;
    thighXf.quat[2] = std::sin(preBendRad * 0.5f);
    thighXf.quat[3] = std::cos(preBendRad * 0.5f);
    uint64_t thigh = phys.CreateDebrisBodyXf(box(4, 10, 4), thighXf, dens, true);
    phys.DisableCollisionsAmong({pelvis, thigh});  // as a real rig does

    Physics::JointDesc jd;
    jd.type = Physics::JointType::Ball;
    jd.anchorVoxel = kHip;
    jd.boneAxis = Vec3{0, -1, 0};  // the thigh's rest direction
    jd.coneFwd = jd.coneSide = coneRad;
    jd.twist = 0.35f;
    jd.friction = 0.0f;  // friction would flatter the limit; test it bare
    uint64_t joint = phys.CreateJoint(pelvis, thigh, jd);

    // Four one-shot whips, one per swing axis and both signs, each given time
    // to be resolved. A one-shot velocity is the honest load — it is what an
    // explosion impulse looks like — whereas re-setting the velocity every
    // step would just be overwriting the solver's answer and would beat any
    // constraint ever written.
    const Vec3 kWhips[4] = {{22, 0, 0}, {0, 0, 22}, {-22, 0, 0}, {0, 0, -22}};
    float worst = 0, atCreate = 0;
    phys.JointSwingAngle(joint, atCreate);
    for (int w = 0; w < 4; w++) {
      phys.SetBodyVelocities(thigh, Vec3{}, kWhips[w]);
      for (int i = 0; i < 45; i++) {
        phys.Step(kTickDt);
        float ang = 0;
        if (phys.JointSwingAngle(joint, ang)) worst = std::max(worst, ang);
      }
    }
    phys.DestroyJoint(joint);
    phys.RemoveBody(thigh);
    phys.RemoveBody(pelvis);
    return std::pair<float, float>{worst, atCreate};
  };

  constexpr float kDeg = 3.14159265f / 180.0f;
  // 45 degrees: tight enough that a limb reaching it is unmistakably past
  // square to its parent, loose enough that the solver is not permanently
  // saturated.
  const float cone = 45 * kDeg;
  auto limited = whip(cone, 0.0f);
  auto preBent = whip(cone, 30 * kDeg);
  auto free = whip(179 * kDeg, 0.0f);

  // MEASURED, not guessed: the solver clamps the velocity at the limit before
  // it integrates, so the overshoot is one sub-step of residual — 5 degrees on
  // the RTX 3060 Ti (held to 50 and 47 against a 45 degree cone). 14 leaves
  // most of the gap to the 90 the assertion actually cares about, and it is a
  // bound rather than a tolerance: losing the limit does not creep past it,
  // it goes straight to the 102 the unlimited arm below reports.
  const float kSlack = 14 * kDeg;
  const bool heldOk = limited.first <= cone + kSlack;
  // The pre-bent joint must be held to the SAME cone about the rest
  // direction, not to cone + 30 about wherever it was built.
  const bool restFrameOk = preBent.first <= cone + kSlack &&
                           std::abs(preBent.second - 30 * kDeg) < 2 * kDeg;
  const bool teethOk = free.first > 90 * kDeg;

  // ---- and the rigs are actually wired to it ------------------------------
  //
  // The three claims above are about the primitive. This is about the DATA
  // reaching it, which is the other half and fails differently: a bone axis
  // that came out inverted centres the cone 180 degrees from the rest pose, so
  // the limb is pinned at its limit while standing and free exactly where it
  // should be held — the worst outcome available here, and one that no angle
  // bound would catch because the angles would all be legal.
  //
  // Asserted on the loaded defs, with no simulation: it is a wiring check, and
  // an assertion that spends 90 ticks of physics to read a load-time constant
  // is a slow way to test nothing extra.
  int ballJoints = 0, badBone = 0, wideCone = 0, waistChecked = 0;
  for (const MobDef& def : c.mobs.Defs()) {
    for (size_t i = 0; i < def.limbs.size(); i++) {
      const MobLimbDef& ld = def.limbs[i];
      if ((int)i == def.rootLimb || ld.joint != Physics::JointType::Ball) continue;
      ballJoints++;
      if (std::abs(ld.boneAxis.len() - 1.0f) > 1e-3f) badBone++;
      // The user-facing rule: no ball joint may reach past square to its
      // parent, or the limb ends up inside it.
      if (ld.coneFwd > 90 * kDeg + 1e-4f || ld.coneSide > 90 * kDeg + 1e-4f)
        wideCone++;
      // The two the corpses broke on, by NAME and by direction: the torso
      // hangs UP off the waist and the thighs hang DOWN off it. Anything else
      // means the bone was derived from the wrong pair of boxes.
      if (ld.parent == "hips") {
        waistChecked++;
        const bool up = ld.name == "torso";
        if (up ? ld.boneAxis.y < 0.7f : ld.boneAxis.y > -0.7f) badBone++;
      }
    }
  }
  // 9 = 3 humanoid rigs (wizard, asha, mina) x {torso, legU.L, legU.R}. A
  // count, not just a per-limb loop, because the loop passes vacuously if the
  // rigs ever stop naming their root "hips" — and the waist is the joint the
  // whole change is about, so "we checked nothing" must not read as PASS.
  const bool wiredOk = ballJoints > 0 && badBone == 0 && wideCone == 0 &&
                       waistChecked == 9;

  const bool ok = heldOk && restFrameOk && teethOk && wiredOk;
  detail = Format(
      "cone %.0f deg: held to %.0f, built pre-bent %.0f held to %.0f, "
      "unlimited reached %.0f; %d ball joints wired (%d bad bone axis, "
      "%d over 90 deg, %d waist joints)",
      cone / kDeg, limited.first / kDeg, preBent.second / kDeg,
      preBent.first / kDeg, free.first / kDeg, ballJoints, badBone, wideCone,
      waistChecked);
  std::printf("ragdoll joints: %s (%s)\n", ok ? "PASS" : "FAIL",
              detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& BodyGates() {
  static const std::vector<Gate> g = {
      {"debris", "phys", {}, false, GateDebris},
      {"settle-back", "phys", {}, false, GateSettleBack},
      {"player-body", "phys", {}, false, GatePlayerBody},
      {"ragdoll-joints", "phys", {}, false, GateRagdollJoints},
  };
  return g;
}

}  // namespace selftest
