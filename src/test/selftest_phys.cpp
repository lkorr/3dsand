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
  // DAWN ONLY: this is a diagnostic bitmap, not the verdict — debrisOk was
  // computed above from compute + readback, which is why the GATE is not
  // declared needsRender and still runs its assertions on --backend vulkan.
  if (debris.BodyCount() > 0 && c.ctx.backendKind == rhi::BackendKind::Dawn) {
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

}  // namespace

const std::vector<Gate>& BodyGates() {
  static const std::vector<Gate> g = {
      {"debris", "phys", {}, false, GateDebris},
      {"settle-back", "phys", {}, false, GateSettleBack},
      {"player-body", "phys", {}, false, GatePlayerBody},
  };
  return g;
}

}  // namespace selftest
