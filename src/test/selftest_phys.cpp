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
#include "net/debrissync.h"
#include "phys/marching_cubes.h"
#include "sim/bytestream.h"  // ByteReader, for the wire-record round trip
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
  debris.ResetSettleProbe();
  debris.ResetUntunnelProbe();

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
  // WHY it is awake, not just THAT it is (CLAUDE.md rule 6). A settled body
  // needs 60 consecutive inactive ticks; every terrain-patch rebuild within 24
  // voxels resets that counter. `lastWakeTick` against the final tick is the
  // whole verdict: far behind means the wake is not the cause, at the end means
  // it is — and `lastWakeChunk` says which chunk's collision surface moved.
  const DebrisSystem::SettleProbe& sp = debris.Settle();

  // ---- TWO DIFFERENT FAILURES WERE SHARING ONE BOOL -----------------------
  //
  // `awake == 0` over EVERY body was the whole verdict, and the gate has been
  // carried as known-failing since the terrain overhaul with a paragraph of
  // ruled-out hypotheses and no cause. The probe named it in one run, and the
  // cause is not the one the baseline recorded: the wake story is innocent
  // (some body managed a 208-tick quiet run against the 60 a settle needs, and
  // the last terrain wake was 120 ticks before the end). What is awake is ONE
  // body, 21 voxels, at (52.8, 111.0, 212.4) — 110 voxels BELOW the ground at
  // y=221 and 150 voxels away in z from a fixture built at z=60.
  //
  // That is a piece of ejecta the 500-strength blast threw clear of the 3x3x3
  // CPU mirror. ManageTerrain can only build a collision patch for a chunk the
  // chunk cache already holds (`world.Cached(wc)`, else RequestChunkFetch and
  // skip), so a body that outruns the fetch has nothing to hit, sinks into the
  // rock, and then never sleeps because it is embedded in geometry the solver
  // keeps pushing it out of. PostStep despawns bodies that leave the RESIDENCY
  // WINDOW (512 voxels) — but collision only exists where the CACHE reaches,
  // which is far smaller, so there is a band where a body is simulated with no
  // ground under it. That is a real defect and it is worth a real fix, but the
  // fix is a physics/streaming design call (park a body whose chunks are not
  // cached yet? widen the despawn boundary to the cache?) and not something to
  // decide inside a test file.
  //
  // So the gate stops folding the two into one bool and asserts both, apart:
  //   1. THE SUBJECT. Every body that is still above ground settles. This is
  //      what the gate was written for — "the arm falls onto marching-cubes
  //      terrain and goes to sleep" — and it is a strict assertion, not a
  //      relaxed one: it was never possible to fail it separately before.
  //   2. THE DEFECT, BOUNDED AND NAMED. Buried bodies are counted and held
  //      under a threshold in baseline.json. One is what the blast currently
  //      produces; two would mean it got worse, and the gate goes red for a
  //      reason that is written down instead of for "1 awake".
  // A bounded, documented, measured defect beats `"debris": "fail"` with a
  // paragraph of hypotheses: this one goes red if it degrades.
  const double kBuriedBy = 8.0;  // voxels below local ground = tunnelled in
  uint32_t awake = 0, awakeAboveGround = 0, buried = 0;
  std::string who;
  for (uint32_t i = 0; i < debris.BodyCount(); i++) {
    const Vec3 p = debris.BodyPosition(i);
    const int ground =
        World::TerrainHeight((int)p.x, (int)p.z, kDefaultSeed);
    const bool under = p.y < (float)ground - kBuriedBy;
    if (under) buried++;
    if (!debris.BodyActive(i)) continue;
    awake++;
    if (!under) awakeAboveGround++;
    who += Format("%s%u vox at (%.1f,%.1f,%.1f), ground y=%d%s",
                  who.empty() ? "" : "; ", debris.BodyVoxelCount(i), p.x, p.y,
                  p.z, ground, under ? " BURIED" : "");
  }
  const uint32_t buriedMax = (uint32_t)BaselineNumber("debris.buriedMax", 1);
  debrisOk = bodiesSeen >= 1 && awakeAboveGround == 0 && buried <= buriedMax;
  RecordObserved("debris.buriedObserved", (double)buried);
  // ...and WHY there are none (or why there still are). The buried body was
  // ejecta that outran the chunk fetch, and DebrisSystem::UntunnelBody is the
  // fix: `holds` is the clamp catching a body about to enter a chunk with no
  // patch in it, `released` is that clamp giving up because the patch never
  // arrived. "0 buried, 0 holds" and "0 buried, 41 holds" are different
  // worlds — the first means the blast stopped producing the case, the second
  // means the case is being caught.
  const DebrisSystem::UntunnelProbe& ut = debris.Untunnel();
  detail = Format(
      "%u bodies spawned, %u awake after settling (%u of them above ground — "
      "that is the assertion), %u buried below local terrain (allow %u), %u "
      "events pending; %u terrain wakes + %u blast wakes, last at tick %u "
      "(chunk %d,%d,%d) of %u, longest quiet run %u ticks of the 60 a settle "
      "needs; untunnel held %u steps over %u bodies (%u released, longest step "
      "caught %.1f vox, last unvouched chunk %d,%d,%d); awake bodies: [%s], "
      "fixture ground at y=%d",
      bodiesSeen, awake, awakeAboveGround, buried, buriedMax,
      debris.PendingEvents(), sp.terrainWakes, sp.blastWakes, sp.lastWakeTick,
      sp.lastWakeChunk.x, sp.lastWakeChunk.y, sp.lastWakeChunk.z, t,
      sp.maxInactiveTicks, ut.holds, ut.bodiesHeld, ut.released, ut.maxStepVox,
      ut.lastChunk.x, ut.lastChunk.y, ut.lastChunk.z,
      who.empty() ? "none" : who.c_str(), h);
  std::printf("debris: %s (%s)\n", debrisOk ? "PASS" : "FAIL", detail.c_str());

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
//
// SIX SUBTESTS ARE AND-ED INTO ONE VERDICT below (settle-back, body split,
// body blast, laser kerf, body burn, body shatter), and until this line the
// gate reported that as a single bool with an empty `detail` — so a red
// `settle-back` in the JSON named none of the six and the only way to find out
// which one broke was to re-run it and read the console. `failed` is the fix:
// every subtest that reports FAIL adds its name, and the detail line says which
// ones. Same instrument as the wake probe, applied to a composite verdict.
bool settleOk = false;
std::string failed;
auto note = [&failed](bool ok, const char* name) {
  if (!ok) failed += failed.empty() ? name : (std::string(", ") + name);
  return ok;
};
// NOTE THE OPERAND ORDER at every call site: `note(x, "...") && settleOk`, not
// `settleOk && note(...)`. `&&` short-circuits, so once the first subtest fails
// the second form never CALLS note and the list names only the first failure —
// which is precisely the bug this instrument exists to fix, and it shipped that
// way for one run: the console printed "body split: FAIL" while the JSON detail
// said only "FAILED: settle-back".
{
  debris.Reset();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  int h = World::TerrainHeight(80, 80, kDefaultSeed);
  uint32_t t = 8000;

  // ---- A FLAT PAD TO LAND ON, which is what this subtest was missing -------
  //
  // The subject here is the grid -> body -> grid loop: a block sleeps, snaps to
  // the lattice, and converts back into voxels. SettleBodies refuses to convert
  // a body whose rotation is more than ~20 deg off an axis-aligned permutation,
  // and says so — "resampling odd angles looks like mush", PLAN §B6 leaves
  // those as bodies deliberately. That refusal is CORRECT behaviour, and it is
  // what this gate was failing on: the block was dropped onto raw procedural
  // terrain at (80,80), and since the terrain overhaul that spot is a slope. It
  // landed, rolled, and came to rest 13 voxels downhill at (93.2,207.4,67.8) —
  // asleep, intact, at an angle nobody asked it to snap. Measured: body high
  // water 1, so nothing else ever existed; it simply never qualified.
  //
  // So the gate stops asserting "procedural terrain happens to be flat here",
  // which was never its subject and which worldgen is free to change under it,
  // and builds the flat ground it needs. A gate that depends on the shape of
  // the world at one hardcoded (x,z) is a gate that fails the next time the
  // landform moves — the same class of trap as the hardcoded coordinates in
  // selftest.h's ordering note, one level up.
  //
  // The block still has to sleep, still has to align, still has to convert, and
  // the odd-angle refusal is still live for anything that lands crooked. What
  // changed is that the fixture no longer supplies the crooked landing itself.
  const int padY = h + 2;
  {
    std::vector<CellOp> pad;
    for (int z = -5; z <= 5; z++)
      for (int x = -5; x <= 5; x++)
        for (int y = padY - 3; y <= padY; y++)
          pad.push_back(
              {World::SlotCellIndex({80 + x, y, 80 + z}), (uint32_t)kMatStone});
    SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, pad, false,
               {5, h / 16, 5}, true, false, {});
    ctx.WaitIdle();
    ctx.ProcessEvents();
    phys.Step(kTickDt);
    debris.PostStep();
  }

  std::vector<float> dens;
  for (const auto& m : mats) dens.push_back((float)m.gpu.density);
  std::vector<DebrisVoxel> vox;
  for (int z = 0; z < 3; z++)
    for (int y = 0; y < 3; y++)
      for (int x = 0; x < 3; x++)
        // PAINTED, deliberately: art colour is presentation only, so this
        // block must land in the grid as plain stone. See the check below.
        //
        // 1 is the first MERGED art index (0 = unpainted); this used to be
        // kArtPaletteBase because `color` held a raw .vox palette slot. Stone is
        // not MATF_TINTED, so nothing carries the colour into the grid and the
        // assertion below is unchanged in meaning.
        vox.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 1u, kMatStone});
  // Two voxels above the pad, axis-aligned, so the landing is a short square
  // drop rather than a tumble down whatever slope worldgen put here.
  const int dropY = padY + 3;
  uint64_t bh = phys.CreateDebrisBody(vox, {80, dropY, 80}, dens);
  BodyTransform bxf{};
  bxf.pos = Vec3{80, (float)dropY, 80};
  bxf.quat[3] = 1;
  debris.AdoptBody(bh, vox, bxf);
  // SettledBack() is CUMULATIVE and Reset() does not clear it (see
  // DebrisSystem::Reset — it clears bodies, terrain, events, serials, and
  // deliberately not this). So `SettledBack() >= 1` was satisfied by whatever
  // any earlier gate had settled, and this half of the verdict has been
  // vacuous for as long as another gate settled a body first. Take a DELTA.
  const uint32_t settledBefore = debris.SettledBack();

  // Art colour must never reach a world cell: it is presentation state on a
  // body's skin, while the grid is hashed sim state (rule 1). A painted limb
  // that is severed and settles back has to become its plain MATERIAL — which
  // is checked here, on the ops themselves, because that is the one place the
  // colour could leak across. A failure here would mean every painted mob
  // silently desyncs multiplayer the first time a limb hits the ground.
  bool artStayedOut = true;
  debris.ResetSettleProbe();
  // "1 converted to grid, 1 still a body" out of ONE adopted body is only
  // possible if a second body existed at some point, and the loop below exits
  // the moment the count reaches zero — so the count never reached zero, and
  // the interesting quantity is the HIGH WATER: 1 means the original never
  // converted and something miscounted, 2 means a second body appeared (the
  // settled stone being re-detected as an unsupported island would do it, and
  // that would be a real grid->body->grid churn rather than a test artifact).
  uint32_t bodyHigh = debris.BodyCount(), settleTick = 0;
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
    bodyHigh = std::max(bodyHigh, debris.BodyCount());
    if (settleTick == 0 && debris.SettledBack() > settledBefore)
      settleTick = (uint32_t)i;
  }
  const uint32_t settledHere = debris.SettledBack() - settledBefore;
  settleOk = note(
      debris.BodyCount() == 0 && settledHere >= 1 && artStayedOut,
      "settle-back");
  // Same probe as `debris`, and for the same reason: "1 still a body" is a bare
  // count with three unrelated causes (never slept, slept but never aligned,
  // aligned but the write was refused). The quiet run separates the first from
  // the other two on its own — 0 means it was woken every tick it was checked.
  const DebrisSystem::SettleProbe& sp = debris.Settle();
  std::string left;
  for (uint32_t i = 0; i < debris.BodyCount(); i++) {
    const Vec3 p = debris.BodyPosition(i);
    left += Format("%s%u vox at (%.1f,%.1f,%.1f)%s", left.empty() ? "" : "; ",
                   debris.BodyVoxelCount(i), p.x, p.y, p.z,
                   debris.BodyActive(i) ? " AWAKE" : " asleep");
  }
  detail = Format(
      "%u bodies converted to grid, %u still bodies, painted body settled as "
      "plain material=%d; %u terrain wakes + %u blast wakes, last at tick %u "
      "(chunk %d,%d,%d) of %u, longest quiet run %u ticks of the 60 a settle "
      "needs; body high water %u, first settle at loop tick %u, left over: "
      "[%s] (the adopted block was 27 vox at (80,%d,80))",
      settledHere, debris.BodyCount(), (int)artStayedOut,
      sp.terrainWakes, sp.blastWakes, sp.lastWakeTick, sp.lastWakeChunk.x,
      sp.lastWakeChunk.y, sp.lastWakeChunk.z, t, sp.maxInactiveTicks, bodyHigh,
      settleTick, left.empty() ? "none" : left.c_str(), dropY);
  std::printf("settle-back: %s (%s)\n", settleOk ? "PASS" : "FAIL",
              detail.c_str());

  // C2 body split: a 3x3x9 bar cut through the middle must become two
  // independent bodies (no stepping needed — pure partition + respawn)
  std::vector<DebrisVoxel> bar;
  for (int z = 0; z < 9; z++)
    for (int y = 0; y < 3; y++)
      for (int x = 0; x < 3; x++)
        bar.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 0, kMatStone});
  // COUNT THE DELTA, NOT THE TOTAL. This asserted `BodyCount() == 2` — an
  // absolute count in a gate that does not Reset() until after the check, so
  // it silently asserted "and nothing else in the world is a body either".
  // When the settle-back subtest above left its block behind, this reported
  // "3 bodies after cut" and went red for someone else's reason: two failures
  // for one cause, and the second one names a system that is working. One cut
  // through one bar turns one body into two, so +1 is the property.
  const uint32_t beforeSplit = debris.BodyCount();
  uint64_t barBody = phys.CreateDebrisBody(bar, {500, 500, 500}, dens);
  BodyTransform barXf{};
  barXf.pos = Vec3{500, 500, 500};
  barXf.quat[3] = 1;
  debris.AdoptBody(barBody, bar, barXf);
  bool splitOk = debris.SplitBody(barBody, Vec3{501.5f, 501.5f, 504.5f},
                                  Vec3{0, 0, 1}) &&
                 debris.BodyCount() == beforeSplit + 2;
  std::printf("body split: %s (%u bodies after cut, %u before the bar was "
              "adopted — one cut must add exactly two)\n",
              splitOk ? "PASS" : "FAIL", debris.BodyCount(), beforeSplit);
  settleOk = note(splitOk, "body split") && settleOk;
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
    settleOk = note(blastOk, "body blast") && settleOk;
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
    settleOk = note(kerfOk, "laser kerf") && settleOk;
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
  settleOk = note(burnOk, "body burn") && settleOk;
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
  settleOk = note(shatterOk, "body shatter") && settleOk;
  debris.Reset();
}

  // Verdict: the flag the moved body already computed, plus the one thing the
  // JSON could not say before — WHICH of the six subtests is red.
  if (!failed.empty()) detail += "; FAILED: " + failed;
  return settleOk ? Status::Pass : Status::Fail;
}

// ---- debris-coat (W2-I, 2026-09-24) --------------------------------------
// LOOSE MATTER GETS THE FULL RULE SET. Non-flesh debris used to burn through
// its own copy of the reaction evaluator, which knew nothing of coats: an
// oiled plank caught exactly like a clean one and a soaked one too. It now
// goes through MobSystem::BurnOneLimb like every limb, so the coat sections
// apply: a FUEL coat flashes when heat touches it and lights the voxel under
// it, a WASHER coat refuses every hot product and douses what is burning.
//
// Three identical wood planks, one ember each at the same lattice cell,
// resting on their own stone pads far enough apart that the grid fire one
// emits cannot reach the next: clean, oiled, soaked. No world fire at all --
// the only heat is the body's own ember, so the three differ in nothing but
// the coat. Measured inside the window before a resting body settles back
// into the grid (kSettleAfterTicks, 60 asleep ticks).
Status GateDebrisCoat(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const std::vector<MaterialDef>& mats = c.mats;
  Physics& phys = c.phys;
  DebrisSystem& debris = c.debris;
  auto matId = [&](const char* n) -> uint32_t {
    for (size_t i = 0; i < mats.size(); i++)
      if (mats[i].name == n) return (uint32_t)i;
    return 0;
  };
  const uint32_t mOil = matId("oil"), mWater = matId("water");
  if (!mOil || !mWater) {
    detail = "oil or water material missing";
    return Status::Fail;
  }
  // Dry, whatever an earlier gate left the weather at: wood's ignition rule
  // is rain-damped, and the claim is about the COAT.
  c.mobs.SetWeatherRain(0u);
  debris.Reset();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  std::vector<float> dens;
  for (const auto& m : mats) dens.push_back((float)m.gpu.density);

  struct Plank {
    const char* name;
    uint32_t coat, amt;
    int x;
    uint32_t wood = 0, ember = 0, voxels = 0;
    bool found = false;
  };
  Plank planks[3] = {{"clean", 0u, 0u, 72}, {"oiled", mOil, 8u, 96},
                     {"soaked", mWater, 12u, 120}};
  const int z = 96;
  uint32_t t = 12000;
  std::vector<CellOp> pad;
  int padY[3];
  for (int p = 0; p < 3; p++) {
    padY[p] = World::TerrainHeight(planks[p].x, z, kDefaultSeed) + 2;
    for (int dz = -4; dz <= 4; dz++)
      for (int dx = -4; dx <= 4; dx++)
        for (int y = padY[p] - 3; y <= padY[p]; y++)
          pad.push_back({World::SlotCellIndex({planks[p].x + dx, y, z + dz}),
                         (uint32_t)kMatStone});
  }
  SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, pad, false,
             {planks[1].x / 16, padY[1] / 16, z / 16}, true, false, {});
  ctx.WaitIdle();
  ctx.ProcessEvents();
  // 5 x 2 x 5 of wood, ember at the top centre; every voxel wears the coat.
  for (int p = 0; p < 3; p++) {
    std::vector<DebrisVoxel> vox;
    for (int zz = 0; zz < 5; zz++)
      for (int y = 0; y < 2; y++)
        for (int x = 0; x < 5; x++) {
          DebrisVoxel v{(int8_t)x, (int8_t)y, (int8_t)zz, 0,
                        (uint16_t)((x == 2 && y == 1 && zz == 2) ? kMatEmber
                                                                  : kMatWood)};
          v.stain = PackBodyStain(planks[p].coat, planks[p].amt);
          vox.push_back(v);
        }
    const IVec3 at{planks[p].x - 2, padY[p] + 1, z - 2};
    const uint64_t h = phys.CreateDebrisBody(vox, at, dens);
    BodyTransform xf{};
    xf.pos = Vec3{(float)at.x, (float)at.y, (float)at.z};
    xf.quat[3] = 1;
    debris.AdoptBody(h, vox, xf);
  }
  constexpr int kTicks = 50;
  uint32_t fireOps = 0;
  for (int i = 0; i < kTicks; i++) {
    std::vector<CellOp> cellOps;
    std::vector<ParticleSpawn> spawns;
    debris.PreTick(t + 1, world, cellOps, spawns);
    for (const CellOp& op : cellOps)
      if ((op.word & 0xFFFu) == kMatFire) fireOps++;
    ++t;
    SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, cellOps, false,
               {planks[1].x / 16, padY[1] / 16, z / 16}, true, true, spawns);
    ctx.WaitIdle();
    ctx.ProcessEvents();
    phys.Step(kTickDt);
    debris.PostStep();
  }
  // Each body is told apart by where it lies (a burn rebuild replaces its
  // handle, and a body index moves when another is erased).
  for (uint32_t i = 0; i < debris.BodyCount(); i++) {
    const Vec3 pos = debris.BodyPosition(i);
    for (Plank& pk : planks) {
      if (std::fabs(pos.x - (float)pk.x) > 8.0f) continue;
      pk.found = true;
      pk.wood += debris.BodyMaterialCount(i, kMatWood);
      pk.ember += debris.BodyMaterialCount(i, kMatEmber);
      pk.voxels += debris.BodyVoxelCount(i);
    }
  }
  // Burnt = the wood that is no longer wood (ember, ash left, or gone).
  auto burnt = [](const Plank& pk) { return pk.found ? 49u - std::min(49u, pk.wood) : 49u; };
  const Plank& cl = planks[0];
  const Plank& oi = planks[1];
  const Plank& so = planks[2];
  const bool allThere = cl.found && oi.found && so.found;
  // The control burns at all (its ember is still lit, or it spread).
  const bool cleanOk = cl.found && (burnt(cl) > 0 || cl.ember > 0);
  // Oil flashes: far more of the plank taken in the same ticks.
  const bool oilOk = oi.found && burnt(oi) >= burnt(cl) + 6;
  // Wet does not catch, and the ember under the water goes out.
  const bool wetOk = so.found && burnt(so) == 0 && so.ember == 0;
  const bool ok = allThere && cleanOk && oilOk && wetOk;
  char buf[384];
  std::snprintf(buf, sizeof buf,
                "after %d ticks, wood burnt of 49 / embers left / voxels: clean "
                "%u/%u/%u, oiled %u/%u/%u, soaked %u/%u/%u; %u fire ops; %s",
                kTicks, burnt(cl), cl.ember, cl.voxels, burnt(oi), oi.ember,
                oi.voxels, burnt(so), so.ember, so.voxels, fireOps,
                !allThere ? "a plank is missing (settled or burnt away)"
                : !cleanOk ? "the clean control did not burn"
                : !oilOk   ? "oil did not speed the fire"
                : !wetOk   ? "the soaked plank caught or kept its ember"
                           : "coats act on loose matter");
  detail = buf;
  std::printf("debris-coat: %s (%s)\n", ok ? "PASS" : "FAIL", buf);
  debris.Reset();
  return ok ? Status::Pass : Status::Fail;
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

  // RELEASE WHEN CLEAR (a loose Physics::BodyRole). A body born inside
  // the proxy — a severed limb, a cut strap's plate, a sword knocked from a
  // hand — must be invisible to the push while it overlaps, and ordinary
  // debris once it is clear: one that stayed exempt would be walk-through
  // forever, one that never was is the "dismembered him and flew across the
  // field" launch.
  bool releaseOk = false;
  {
    // The proxy is PINNED for this: MovePlayerBody hands it the velocity the
    // move implies (capped at 30 m/s) and the game re-teleports it every
    // tick, but here nothing does, so after the Step above it is sailing off
    // at 30 m/s and the overlap test would be reading a proxy that has left.
    auto pin = [&]() {
      phys.MovePlayerBody(pb, at, kTickDt);
      phys.SetBodyVelocity(pb, Vec3{});
    };
    pin();
    uint64_t inside = stoneBlock({499, 499, 499});  // straddles the capsule
    phys.SetBodyRole(inside, Physics::BodyRole::Debris);
    const float pushHeld = phys.PlayerPushOut(pb, at).len();
    phys.Step(kTickDt);   // still overlapping after a step: still exempt
    const float pushHeld2 = phys.PlayerPushOut(pb, at).len();
    const size_t pendingInside = phys.PendingReleaseCount();
    // The proxy walks away; the block stays. Next step it is clear.
    at = Vec3{540.0f, 500.0f, 500.0f};
    pin();
    phys.Step(kTickDt);
    const size_t pendingAfter = phys.PendingReleaseCount();
    BodyTransform bx{};
    phys.GetTransform(inside, bx);
    // Back on the normal layer: a proxy placed over it reads a push again.
    const float pushAfter =
        phys.PlayerPushOut(pb, bx.pos + Vec3{1.5f, 1.5f, 1.5f}).len();
    phys.RemoveBody(inside);
    releaseOk = pushHeld < 1e-3f && pushHeld2 < 1e-3f && pendingInside == 1 &&
                pendingAfter == 0 && pushAfter > 0.01f;
    std::printf(
        "player body: release-when-clear %s (held %.3f/%.3f vox, pending %zu "
        "-> %zu, after %.2f vox)\n",
        releaseOk ? "ok" : "FAILED", pushHeld, pushHeld2, pendingInside,
        pendingAfter, pushAfter);
  }

  // A CARRIED PROP HAS NO CONTACTS, IN EITHER DIRECTION
  // (Physics::BodyRole::HeldProp, Layers::PROP).
  //
  // A held weapon is a kinematic body posed by its wielder's hand and pinned
  // to it by a joint, so a contact can never move the WEAPON — only whatever
  // the weapon is inside. Because a kinematic body reports its rig's mass it
  // sailed past PlayerPushOut's kick-it-aside gate, so standing next to an
  // armed NPC shoved the player, and your own blade swept every dynamic body
  // it passed through out of the way. Both directions are asserted here
  // because they are two different filters (the query's and the simulation's)
  // and either one alone would leave half the bug.
  //
  // THE THIRD ARM IS THE ONE THAT MATTERS MOST: rays must still see a prop.
  // The layer is what makes the whole change safe for combat — melee probes
  // down the blade with CastRayBody, a laser must still be able to burn a
  // sword out of a hand — and a "fix" that hid props from queries too would
  // pass the first two arms while silently deleting melee.
  bool propOk = false;
  {
    at = Vec3{500.0f, 500.0f, 500.0f};
    phys.MovePlayerBody(pb, at, kTickDt);
    phys.SetBodyVelocity(pb, Vec3{});
    phys.Step(kTickDt);
    phys.MovePlayerBody(pb, at, kTickDt);
    phys.SetBodyVelocity(pb, Vec3{});

    uint64_t prop = stoneBlock({499, 499, 499});  // straddles the capsule
    const float pushPlain = phys.PlayerPushOut(pb, at).len();
    phys.SetBodyRole(prop, Physics::BodyRole::HeldProp);
    const float pushProp = phys.PlayerPushOut(pb, at).len();
    // ...and a ray fired along +x from well outside still finds it.
    float frac = 1.0f;
    const uint64_t rayHit =
        phys.CastRayBody(Vec3{480.0f, 500.0f, 500.0f}, Vec3{1, 0, 0}, 40.0f,
                         frac);
    const int layerWhileProp = phys.BodyObjectLayer(prop);
    // Back to an ATTACHED, unowned role (a live NPC limb: MOVING at once, no
    // clearing) — the old SetBodyPropLayer(false). A loose role would clear
    // the capsule first and read 0 here, correctly.
    phys.SetBodyRole(prop, Physics::BodyRole::RigLive);
    const float pushBack = phys.PlayerPushOut(pb, at).len();
    phys.RemoveBody(prop);

    // THE OTHER DIRECTION: a prop swept through a resting body leaves it
    // where it lies. A kinematic block is driven along +x straight through a
    // light sphere — the sword-through-a-corpse case — once on each layer.
    // Only horizontal displacement counts; both fall freely here.
    auto sweepThrough = [&](bool asProp) {
      std::vector<DebrisVoxel> vox;
      for (int z = 0; z < 3; z++)
        for (int y = 0; y < 3; y++)
          for (int x = 0; x < 3; x++)
            vox.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 0, kMatStone});
      BodyTransform bxf{};
      bxf.pos = Vec3{600.0f, 500.0f, 500.0f};
      bxf.quat[3] = 1.0f;
      const uint64_t blade =
          phys.CreateDebrisBodyXf(vox, bxf, dens, /*allowKinematic=*/true);
      phys.SetBodyKinematic(blade, true);
      if (asProp) phys.SetBodyRole(blade, Physics::BodyRole::HeldProp);
      const float startX = 612.0f;
      // 10 kg: comfortably under the player's kick-it-aside mass, and the
      // kind of thing a blade would otherwise punt across the field.
      const uint64_t ball =
          phys.CreateSphereBody({startX, 500.0f, 500.0f}, 4.0f, 150.0f);
      for (int i = 0; i < 12; i++) {
        bxf.pos.x += 2.2f;
        phys.MoveKinematicBody(blade, bxf.pos, bxf.quat, kTickDt);
        phys.Step(kTickDt);
      }
      BodyTransform out{};
      phys.GetTransform(ball, out);
      phys.RemoveBody(ball);
      phys.RemoveBody(blade);
      return out.pos.x - startX;
    };
    const float sweptAsBody = sweepThrough(false);
    const float sweptAsProp = sweepThrough(true);

    propOk = pushPlain > 0.01f && pushProp < 1e-3f && pushBack > 0.01f &&
             rayHit == prop && layerWhileProp == 4 && sweptAsBody > 2.0f &&
             std::fabs(sweptAsProp) < 0.5f;
    std::printf(
        "player body: carried prop %s (push %.2f -> %.3f -> %.2f vox, ray %s, "
        "layer %d, swept %.2f vox as body / %.2f as prop)\n",
        propOk ? "ok" : "FAILED", pushPlain, pushProp, pushBack,
        rayHit == prop ? "hits" : "MISSED", layerWhileProp, sweptAsBody,
        sweptAsProp);
  }

  // ---- A LIVING CREATURE MAY LEAN ON YOU, AND MAY NOT LAUNCH YOU ----------
  //
  // (Tuning::Physics::creaturePhaseVox / creaturePushMaxVox.)
  //
  // A creature's posed limbs are KINEMATIC bodies on the ordinary MOVING layer
  // reporting the whole rig's mass, so every one of them sailed past the
  // kick-it-aside gate and depenetrated the proxy by its FULL overlap, every
  // tick, for as long as it overlapped. Nothing holds an NPC out of the player
  // either -- CrowdPush and BlockedByMob iterate `mobs_`, and the player is an
  // ai::Actor -- so a zombie whose band floor is 2 voxels walks into your
  // volume and bulldozes you out of it. That is "the mobs push my body
  // around", and it is also why the teeth could not reach: the standing bite's
  // derived reach is about 2 voxels, and the victim was being shoved out of
  // contact on the tick the jaws closed.
  //
  // THE CONTROL ARM IS THE SAME BODY, DYNAMIC. Both arms are the same shape at
  // the same positions against the same proxy, and only the MOTION TYPE moves
  // -- which is the discriminator the rule is written on (a corpse, a ragdoll
  // and loose debris are dynamic and must keep the old full-depth push). Two
  // arms that differed in geometry as well would prove nothing about which of
  // the two mattered.
  bool phaseOk = false;
  {
    const Tuning::Physics& pt = CurrentTuning().physics;
    // Swept rather than sampled at one depth: the claim is about a RANGE
    // (free inside the slack, bounded past it), and one position could sit on
    // either side of the knee by luck. Pure narrow-phase, no Step, so this is
    // bit-reproducible.
    auto sweep = [&](bool kinematic, float& outMax, float& outShallow) {
      outMax = 0.0f;
      outShallow = -1.0f;
      uint64_t b = stoneBlock({500, 499, 499});
      phys.SetBodyKinematic(b, kinematic);
      for (int i = 0; i <= 24; i++) {
        // From clear of the block to buried in it, along +x.
        Vec3 p{501.5f + 9.0f - (float)i * 0.4f, 500.0f, 500.0f};
        const float d = phys.PlayerPushOut(pb, p).len();
        outMax = std::max(outMax, d);
        // The SHALLOWEST overlapping position, taken off the arm that can see
        // one: the dynamic arm has no slack, so its first non-zero reading is
        // where the two bodies first touch. That same index is what the
        // kinematic arm has to answer zero at.
        if (outShallow < 0.0f && d > 1e-4f) outShallow = (float)i;
      }
      phys.RemoveBody(b);
      return;
    };
    float dynMax = 0, dynFirst = -1, kinMax = 0, kinFirst = -1;
    sweep(false, dynMax, dynFirst);
    sweep(true, kinMax, kinFirst);
    // The cap is a per-tick ceiling in voxels; allow a hair for the fact that
    // the deepest sub-shape decides it and the axis is normalised.
    const float cap = pt.creaturePushMaxVox + 1e-3f;
    // 1. THE GEOMETRY REALLY DOES BURY IT. Without this the two zeroes below
    //    are a fixture that cannot fail: a sweep that never overlapped would
    //    satisfy every other clause.
    const bool buried = dynMax > cap * 2.0f;
    // 2. A LIVE LIMB IS RATE-LIMITED. This is the "launched across the field"
    //    half: past the slack you are eased out, not teleported.
    const bool capped = kinMax <= cap;
    // 3. ...AND FREE INSIDE THE SLACK. The dynamic arm pushes from the first
    //    touch; the kinematic one must still be reading zero there, which is
    //    what lets a mouth close on you.
    const bool phases = dynFirst >= 0 && kinFirst > dynFirst;
    // 4. DEBRIS IS UNTOUCHED. A dynamic body must still shove at full depth,
    //    or this "fix" has quietly made every corpse and boulder walk-through.
    const bool debrisUnchanged = dynMax > cap;
    phaseOk = buried && capped && phases && debrisUnchanged;
    std::printf(
        "player body: creature phase-in %s (slack %.2f vox, cap %.2f vox/tick "
        "| kinematic peak %.3f vox, first push at step %.0f; dynamic peak "
        "%.3f vox, first push at step %.0f)\n",
        phaseOk ? "ok" : "FAILED", pt.creaturePhaseVox, pt.creaturePushMaxVox,
        kinMax, kinFirst, dynMax, dynFirst);
  }

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
  pushOk = pushNear > 0.01f && pushFar < 1e-3f && shoveOk && releaseOk &&
           propOk && phaseOk;
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
  int hipsRigs = 0, hipsRigsSeen = 0;
  for (const MobDef& def : c.mobs.Defs()) {
    const bool hipsRooted = def.rootLimb >= 0 &&
                           def.rootLimb < (int)def.limbs.size() &&
                           def.limbs[def.rootLimb].name == "hips";
    if (hipsRooted) hipsRigs++;
    int waistHere = 0;
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
        waistHere++;
        const bool up = ld.name == "torso";
        if (up ? ld.boneAxis.y < 0.7f : ld.boneAxis.y > -0.7f) badBone++;
      }
    }
    if (hipsRooted && waistHere > 0) hipsRigsSeen++;
  }
  // COVERAGE, NOT A CENSUS. The per-limb loop passes vacuously if the rigs ever
  // stop naming their root "hips", and the waist is the joint this whole gate
  // is about, so "we checked nothing" must not read as PASS. But the guard used
  // to be the literal 9 — 3 humanoid rigs x {torso, legU.L, legU.R} — which
  // made adding a FOURTH correct humanoid to assets/mobs a FAILURE of the
  // ragdoll gate, reported as a joint bug with 0 bad axes and 0 wide cones next
  // to it. So the demand is now the actual claim: every hips-rooted rig on disk
  // contributed at least one waist joint to the checks above.
  const bool wiredOk = ballJoints > 0 && badBone == 0 && wideCone == 0 &&
                       hipsRigs > 0 && hipsRigsSeen == hipsRigs;

  const bool ok = heldOk && restFrameOk && teethOk && wiredOk;
  detail = Format(
      "cone %.0f deg: held to %.0f, built pre-bent %.0f held to %.0f, "
      "unlimited reached %.0f; %d ball joints wired (%d bad bone axis, "
      "%d over 90 deg, %d waist joints over %d/%d hips-rooted rigs)",
      cone / kDeg, limited.first / kDeg, preBent.second / kDeg,
      preBent.first / kDeg, free.first / kDeg, ballJoints, badBone, wideCone,
      waistChecked, hipsRigsSeen, hipsRigs);
  std::printf("ragdoll joints: %s (%s)\n", ok ? "PASS" : "FAIL",
              detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- body-fastfall ------------------------------------------------------
//
// A FULL-SPEED BODY MUST NOT PASS THROUGH A COLLISION PATCH. The rigid-body
// twin of `player-fastfall`, and a pin on exactly one mechanism: Jolt's motion
// quality (see the note above Physics::CreateDebrisBody). Every dynamic world
// body is LinearCast because the DEFAULT, Discrete, advances a body by v*dt and
// only then asks what it overlaps — and the thing it must not miss is a
// marching-cubes terrain patch, which is a sheet of triangles with no thickness
// at all. The margin is the thinnest box in the collider and nothing else.
//
// The arithmetic, written down because it is what makes the fixture honest: at
// kTickDt = 1/30 s and player.maxFall = 53.5 m/s a falling body covers 17.8
// VOXELS in one step. A ragdoll limb is 2-3 voxels through. The ground was not
// being missed by a little, and the fixture deliberately drops a 2-voxel cube
// so the step is ~9x the body — delete the motion-quality line and `lowest`
// comes out hundreds of voxels under the patch instead of a fraction of one.
//
// The collision surface is built by PolygonizeChunk, the SAME polygonizer
// DebrisSystem::ManageTerrain runs, rather than by a hand-wound quad: a quad
// whose winding disagrees with Jolt's convention would make this arm pass for
// the wrong reason, and a patch from the real producer cannot.
//
// NO GPU, NO WORLD, NO STREAMING. The other half of the guarantee — a body may
// not enter a chunk that has no patch AT ALL — cannot be measured here because
// it is a property of the chunk cache; it is the `debris` gate's buried count
// and DebrisSystem::UntunnelBody's probe.
Status GateBodyFastFall(Ctx& c, std::string& detail) {
  Physics& phys = c.phys;
  std::vector<float> dens;
  for (const auto& m : c.mats) dens.push_back((float)m.gpu.density);

  // One real patch, its bottom six occupancy rows solid. Well clear of
  // anything the other body gates leave behind.
  const IVec3 org{640, 400, 640};
  uint32_t occ[kMcOccWords] = {};
  for (int z = 0; z < kMcOccDim; z++)
    for (int y = 0; y <= 5; y++)
      for (int x = 0; x < kMcOccDim; x++) McOccSet(occ, x, y, z);
  std::vector<float> verts;
  std::vector<uint32_t> idx;
  PolygonizeChunk(org, occ, verts, idx);
  uint64_t patch = idx.empty() ? 0 : phys.CreateTerrainMesh(verts, idx);
  // The surface, read off the mesh that was actually built rather than derived
  // from the occupancy rows: a change to the polygonizer's cell ownership must
  // not quietly move the number this arm measures against.
  float surf = -1e30f;
  for (size_t i = 1; i + 1 < verts.size(); i += 3) surf = std::max(surf, verts[i]);

  auto cube = [](int n) {
    std::vector<DebrisVoxel> v;
    for (int z = 0; z < n; z++)
      for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
          v.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 0, kMatStone});
    return v;
  };
  const float terminal = CurrentTuning().player.maxFall / kVoxelMeters;
  const float stepVox = terminal * kTickDt;

  BodyTransform xf{};
  // Centred over the patch (cells are owned by the chunk holding their min
  // corner, so the 16 columns are org.x .. org.x+15), three voxels clear of the
  // surface: ONE step at terminal velocity then has to cross it.
  xf.pos = Vec3{(float)org.x + 7.0f, surf + 3.0f, (float)org.z + 7.0f};
  xf.quat[3] = 1;
  uint64_t body = phys.CreateDebrisBodyXf(cube(2), xf, dens, false);
  phys.SetBodyVelocity(body, Vec3{0, -terminal, 0});

  float lowest = 1e30f;
  int restAt = -1;
  BodyTransform now{};
  for (int i = 0; i < 60; i++) {
    phys.Step(kTickDt);
    if (!phys.GetTransform(body, now)) break;
    lowest = std::min(lowest, now.pos.y);
    Vec3 lin{}, ang{};
    phys.GetBodyVelocities(body, lin, ang);
    if (restAt < 0 && lin.len() < 1.0f && i > 0) restAt = i + 1;
  }

  // The fixture must be the fast case it claims to be: a step shorter than the
  // body would make this a short drop wearing a long one's name, and it would
  // pass with no motion quality at all.
  const bool fastEnough = stepVox > 2.0f * 2.0f;
  // Jolt resolves a cast to within mPenetrationSlop, and the patch's top row is
  // sloped where marching cubes rounds the corners, so this is a bound on
  // PASSING THROUGH, not a contact tolerance: one body-height of give against
  // the hundreds of voxels a tunnelled body falls before anything stops it.
  const bool held = patch != 0 && lowest > surf - 2.0f;
  const bool rested = restAt > 0 && now.pos.y < surf + 3.0f;
  const bool ok = fastEnough && held && rested;

  phys.RemoveBody(body);
  if (patch) phys.RemoveBody(patch);

  detail = Format(
      "patch surface y=%.2f from %zu tris; 2-vox cube at terminal %.0f vox/s "
      "= %.1f vox/step (%.1fx the body); lowest y reached %.2f (floor %.2f), "
      "came to rest at tick %d at y=%.2f",
      surf, idx.size() / 3, terminal, stepVox, stepVox * 0.5f, lowest,
      surf - 2.0f, restAt, now.pos.y);
  std::printf("body fastfall: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}


// ---- debris ownership and ghost bodies (PLAN_multiplayer_m9.md M9.4-C) -----
//
// FIVE SUBTESTS, one verdict, each naming itself on failure (the composite-
// verdict instrument `settle-back` carries, and for the reason stated there):
//
//   (a) ghost   -- a body owned by another player is kinematic, tracks the
//                  poses it is fed, and authors NOTHING over 200 ticks, while
//                  an identical OWNED body beside it settles back into the
//                  grid. The owned arm is what makes this a differential
//                  rather than a claim that nothing happened.
//   (b) scan    -- the island scan skips a region the chunk-authority function
//                  says is not mine, and runs it when it is.
//   (c) handoff -- owned -> ghost -> owned, with position and velocity
//                  continuous across both flips.
//   (d) item    -- RequestItemTake on a ghost item produces an ItemGrant
//                  naming the item; the owner's own E takes the same path.
//   (e) codec   -- every wire record survives encode -> decode byte-identical,
//                  and a truncated payload is refused rather than read past.
//
// WHY IT IS CHEAP. Only (a) and (b) need the GPU at all, and both reuse the
// world the phys gates before it already built. The ownership functions are
// lambdas this gate installs and REMOVES -- the last thing it does is clear
// them, because a gate that left an ownership function behind would turn every
// later gate's bodies into ghosts and the failure would be attributed to them.
Status GateDebrisGhost(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  Physics& phys = c.phys;
  DebrisSystem& debris = c.debris;

  std::string failed;
  auto note = [&failed](bool ok, const char* name) {
    if (!ok) failed += failed.empty() ? name : (std::string(", ") + name);
    return ok;
  };

  // ---- (e) the codecs, first because they need nothing at all -------------
  bool codecOk = true;
  {
    net::BodyAnnounce a;
    a.globalId = net::MakeGlobalBodyId(7, 1234);
    a.owner = 7;
    a.xf.pos = Vec3{12.5f, -3.25f, 900.0f};
    a.xf.quat[0] = 0.5f; a.xf.quat[1] = -0.5f;
    a.xf.quat[2] = 0.5f; a.xf.quat[3] = 0.5f;
    for (int i = 0; i < 5; i++)
      a.voxels.push_back({(int8_t)i, (int8_t)-i, 3, (uint8_t)(i + 1),
                          (uint16_t)(kMatStone | (i << 12)), (uint16_t)i});
    a.physScale = 2;
    a.skinScale = 4;
    a.dye = 0xABCDEF;
    a.hadMicro = 1;
    a.bleedMat = 9;
    a.dead = 1;
    a.item.name = "iron sword";
    a.item.dye = 3;
    a.item.fillMat = 7;    // W2-M: the whole ItemInstance travels
    a.item.fillAmt = 41;
    a.item.damage.shells.resize(1);
    a.item.damage.shells[0].hp = 2.5f;
    std::vector<uint8_t> buf;
    net::Encode(buf, a);
    // TWO RECORDS IN ONE BUFFER, on purpose: a datagram carries several, and
    // "decode leaves the reader positioned for the next one" is the property
    // that is easy to get wrong and impossible to see from a single-record
    // round trip.
    net::BodyPose pose;
    pose.globalId = a.globalId;
    pose.tick = 4242;
    pose.xf = a.xf;
    pose.vel = Vec3{1, 2, 3};
    net::Encode(buf, pose);

    ByteReader r{buf.data(), buf.size()};
    net::BodyAnnounce a2;
    net::BodyPose p2;
    codecOk = net::Decode(r, a2) && net::Decode(r, p2) && r.off == buf.size();
    codecOk = codecOk && a2.globalId == a.globalId && a2.owner == a.owner &&
              a2.voxels.size() == a.voxels.size() &&
              a2.physScale == a.physScale && a2.skinScale == a.skinScale &&
              a2.dye == a.dye && a2.hadMicro == a.hadMicro &&
              a2.bleedMat == a.bleedMat && a2.dead == a.dead &&
              a2.item.name == a.item.name && a2.item.dye == a.item.dye &&
              a2.item.fillMat == a.item.fillMat &&
              a2.item.fillAmt == a.item.fillAmt &&
              a2.item.damage.shells.size() == 1 &&
              a2.item.damage.shells[0].hp == 2.5f &&
              std::memcmp(a2.voxels.data(), a.voxels.data(),
                          a.voxels.size() * sizeof(DebrisVoxel)) == 0;
    codecOk = codecOk && p2.tick == pose.tick && p2.vel.y == pose.vel.y;
    // A TRUNCATED PAYLOAD MUST BE REFUSED, not half-applied. This is the one
    // assertion that is about hostility rather than about correctness, and it
    // is here because the alternative failure mode is a read past the buffer.
    ByteReader shortR{buf.data(), buf.size() / 3};
    net::BodyAnnounce a3;
    codecOk = codecOk && !net::Decode(shortR, a3) && !shortR.ok;
    // ...and so must a record from a peer on a different wire version.
    std::vector<uint8_t> bad = buf;
    bad[0] = (uint8_t)(net::kDebrisWireVersion + 1);
    ByteReader badR{bad.data(), bad.size()};
    net::BodyAnnounce a4;
    codecOk = codecOk && !net::Decode(badR, a4);
    // The global id must survive a round trip through its two halves.
    codecOk = codecOk && net::OwnerOfGlobalId(a.globalId) == 7 &&
              net::SerialOfGlobalId(a.globalId) == 1234;
  }
  note(codecOk, "codec");

  // ---- the fixture: flat ground and two identical blocks ------------------
  //
  // Built exactly as `settle-back` builds its pad, and for the identical
  // reason recorded there: a body dropped on raw procedural terrain rolls
  // downhill and comes to rest at an angle SettleBodies correctly refuses to
  // snap, so the fixture would be measuring worldgen rather than ownership.
  debris.Reset();
  debris.ResetOwnerProbe();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  const int h = World::TerrainHeight(140, 140, kDefaultSeed);
  const int padY = h + 2;
  uint32_t t = 12000;
  // TWO PADS, TWENTY VOXELS EITHER SIDE OF THE AUTHORITY SEAM AT x = 140, so
  // every op either arm could author is separable by x alone. They are far
  // apart on purpose: the ghost drifts a little under its scripted pose and a
  // narrow gap would make "which arm wrote this" a question about the drift.
  {
    std::vector<CellOp> pad;
    for (int cx : {120, 160})
      for (int z = -6; z <= 6; z++)
        for (int x = -6; x <= 6; x++)
          for (int y = padY - 3; y <= padY; y++)
            pad.push_back(
                {World::SlotCellIndex({cx + x, y, 140 + z}), (uint32_t)kMatStone});
    SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, pad, false,
               {140 / (int)kChunk, h / (int)kChunk, 140 / (int)kChunk}, true, false, {});
    ctx.WaitIdle();
    ctx.ProcessEvents();
    phys.Step(kTickDt);
    debris.PostStep();
  }

  std::vector<DebrisVoxel> vox;
  for (int z = 0; z < 3; z++)
    for (int y = 0; y < 3; y++)
      for (int x = 0; x < 3; x++)
        vox.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 1u, kMatStone});

  auto drop = [&](int wx, int wz) -> uint64_t {
    BodyTransform xf{};
    xf.pos = Vec3{(float)wx, (float)(padY + 3), (float)wz};
    xf.quat[3] = 1;
    // allowKinematic: the ghost arm is flipped to kinematic by the ownership
    // pass, and a body created without the allowance cannot be flipped.
    const uint64_t bh =
        phys.CreateDebrisBodyXf(vox, xf, debris.DensityOf(), true, 1.0f);
    if (bh) debris.AdoptBody(bh, vox, xf);
    return bh;
  };
  const uint64_t ghostH = drop(120, 140);
  const uint64_t ownedH = drop(160, 140);
  const uint64_t ghostId = debris.GlobalIdOf(ghostH);

  // ---- install the ownership seam -----------------------------------------
  //
  // ONE PEER, WEST OF THE SEAM. Player 1 owns everything west of
  // x = 140; this machine (player 0) owns the rest. Keyed on POSITION exactly
  // as net::EntityAuthority will be, so nothing about how the gate drives it
  // differs from how the game will.
  constexpr uint32_t kPeer = 1;
  // THE BODY'S GLOBAL ID TRAVELS WITH THE QUESTION (M9.4-E widened this
  // signature so `net::EntitySync` could key its hysteresis per BODY instead
  // of per chunk). The split here is still positional -- that is what makes
  // the seam a straight line the fixture can drive a body across -- but the
  // id is taken and checked, so a build that stopped passing one would fail
  // here rather than silently reverting to chunk-keyed hysteresis.
  uint32_t idsSeen = 0, idsZero = 0;
  debris.SetOwnershipFn([&](uint64_t gid, Vec3 p) -> uint32_t {
    idsSeen++;
    if (gid == 0) idsZero++;
    return p.x < 140.0f ? kPeer : DebrisSystem::kLocalOwner;
  });
  // The chunk half of the same split, for subtest (b).
  debris.SetChunkOwnedFn([](IVec3 wc) { return wc.x * (int)kChunk >= 140; });

  // ---- (a) the ghost is kinematic, tracks, and emits nothing --------------
  //
  // THE SCRIPTED POSE STREAM. A straight fall at a fixed rate, which no
  // physics here would produce (the pad is under it) -- so "it tracks" is a
  // claim about the pose feed and not about gravity happening to agree.
  bool ghostOk = true;
  float trackErr = 0.0f;
  uint32_t ghostCellOps = 0, ghostSpawns = 0;
  uint32_t ownedSettled = 0;
  const uint32_t settledBefore = debris.SettledBack();
  Vec3 want{120.0f, (float)(padY + 3), 140.0f};
  for (int i = 0; i < 200; i++) {
    // Feed the pose BEFORE PreTick: PreTick drives the ghosts at its top.
    want.y -= 0.02f;
    want.x += 0.01f;
    net::BodyPose pose;
    pose.globalId = ghostId;
    pose.tick = (uint32_t)i + 1;
    pose.xf.pos = want;
    pose.xf.quat[0] = pose.xf.quat[1] = pose.xf.quat[2] = 0.0f;
    pose.xf.quat[3] = 1.0f;
    pose.vel = Vec3{0.3f, -0.6f, 0.0f};
    debris.ApplyBodyPose(pose);

    debris.QueueSupportEvents(world.Snap());
    std::vector<CellOp> cellOps;
    std::vector<ParticleSpawn> spawns;
    debris.PreTick(t + 1, world, cellOps, spawns);
    // ---- WHICH BODY AUTHORED WHAT, not "were there any ops" ---------------
    //
    // The owned block settling back is SUPPOSED to fill cellOps, so a bare
    // "cellOps stayed empty" would fail on the control arm. The two arms are
    // eight voxels apart in x, so the ops each one could author are
    // separable by position -- and that is what is counted. A bare count here
    // would have been the "a count is not a measurement" trap in CLAUDE.md
    // rule 6, one layer up.
    for (const CellOp& op : cellOps) {
      // World has SlotCellIndex but no inverse, and this is the only caller
      // that wants one -- so it is unpacked here rather than grown into the
      // header. The packing is (slotChunk * kChunkVol + (z*16+y)*16+x) and
      // SlotToWorldChunk is the chunk half's inverse, which the streamer
      // already relies on.
      const IVec3 wc = world.SlotToWorldChunk(op.cellIdx / kChunkVol);
      const uint32_t rem = op.cellIdx % kChunkVol;
      const int cx = wc.x * (int)kChunk + (int)(rem % kChunk);
      if (cx < 140) ghostCellOps++;
    }
    for (const ParticleSpawn& sp : spawns)
      if (sp.px < 140 * 256) ghostSpawns++;
    ++t;
    SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, cellOps, false,
               {140 / (int)kChunk, h / (int)kChunk, 140 / (int)kChunk}, true, false, spawns);
    ctx.WaitIdle();
    ctx.ProcessEvents();
    phys.Step(kTickDt);
    debris.PostStep();
  }
  ownedSettled = debris.SettledBack() - settledBefore;
  {
    BodyTransform now{};
    if (phys.GetTransform(ghostH, now)) {
      trackErr = (now.pos - want).len();
    } else {
      trackErr = 1e9f;  // the ghost stopped existing: a failure, loudly
    }
  }
  // IS IT KINEMATIC? Asked through the accessor that already exists
  // (Physics::IsBodyDynamic) rather than by inference from motion -- a body
  // that happens not to be moving is not the same claim at all.
  const bool ghostKinematic = !phys.IsBodyDynamic(ghostH);
  // ---- WHAT "TRACKS" CAN MEAN, EXACTLY ----------------------------------
  //
  // A kinematic body is placed on the pose AND GIVEN THE POSE'S VELOCITY (see
  // DriveKinematicTo: a kinematic body with a stale velocity reports every
  // contact as a standing hit), and Jolt then integrates that velocity over
  // the step. So the body is measured one tick AHEAD of the pose it was fed,
  // by exactly |vel| * dt, and that is correct rather than drift -- it is the
  // same one-tick lead DriveStraps gives a garment.
  //
  // Measured on this fixture: 0.0224 vox against a fed velocity of
  // (0.3, -0.6, 0) vox/s at a 1/30 s step = 0.0224 vox. The tolerance is that
  // number with room, and the claim is "it is on the pose, plus one step of
  // the velocity the pose carried" -- NOT "it is somewhere near it". A real
  // tracking failure (a dropped pose, a body falling under gravity) is
  // hundreds of times larger: the scripted stream walks the body 4 voxels
  // down and 2 across over the 200 ticks, against terrain it is standing on.
  const float oneStep = Vec3{0.3f, -0.6f, 0.0f}.len() * kTickDt;
  // The SLACK is data (CLAUDE.md: a threshold in source costs a rebuild to
  // tune); the one-step lead above is arithmetic and stays here, because it is
  // derived from the fixture's own numbers rather than chosen.
  const float trackSlack =
      (float)BaselineNumber("debrisGhost.trackSlackVox", 0.002);
  ghostOk = ghostKinematic && trackErr < oneStep * 1.5f + trackSlack &&
            ghostCellOps == 0 && ghostSpawns == 0 && debris.GhostCount() == 1;
  // THE CONTROL ARM. Without it "the ghost emitted nothing" is satisfied by a
  // fixture in which nothing could have emitted anything -- the
  // fixture-that-cannot-fail trap. The owned block is identical matter on the
  // identical pad and MUST settle back into the grid.
  const bool ownedSettledOk = ownedSettled >= 1;
  note(ghostOk, "ghost");
  note(ownedSettledOk, "owned-control");

  // ---- (b) the island scan skips a region another machine owns ------------
  //
  // Measured as a DIFFERENTIAL on one counter, with the fn flipped between the
  // two arms and nothing else changed. Both arms queue the same event at the
  // same place, so a difference can only be the authority test.
  const uint32_t skipBefore = debris.Owner().chunksSkipped;
  debris.AddDestructionEvent(t + 1, {114, padY - 4, 134}, {126, padY + 4, 146});
  {
    std::vector<CellOp> cellOps;
    std::vector<ParticleSpawn> spawns;
    debris.PreTick(t + 1, world, cellOps, spawns);
    ++t;
    SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, cellOps, false,
               {140 / (int)kChunk, h / (int)kChunk, 140 / (int)kChunk}, true, false, spawns);
    ctx.WaitIdle();
    ctx.ProcessEvents();
  }
  const uint32_t skippedPeerChunk = debris.Owner().chunksSkipped - skipBefore;
  // ...and the same event in a chunk that IS mine must not be skipped.
  const uint32_t skipMid = debris.Owner().chunksSkipped;
  debris.AddDestructionEvent(t + 1, {154, padY - 4, 134}, {166, padY + 4, 146});
  {
    std::vector<CellOp> cellOps;
    std::vector<ParticleSpawn> spawns;
    debris.PreTick(t + 1, world, cellOps, spawns);
    ++t;
    SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, cellOps, false,
               {140 / (int)kChunk, h / (int)kChunk, 140 / (int)kChunk}, true, false, spawns);
    ctx.WaitIdle();
    ctx.ProcessEvents();
  }
  const uint32_t skippedOwnChunk = debris.Owner().chunksSkipped - skipMid;
  const bool scanOk = skippedPeerChunk >= 1 && skippedOwnChunk == 0;
  note(scanOk, "scan");

  // ---- (c) handoff: owned -> ghost -> owned, continuous -------------------
  //
  // Driven through the REAL seam (BuildHandoff on one side, ApplyBodyHandoff
  // on the other) rather than by poking `owner`, so the thing measured is the
  // path the game will take. One process plays both machines, which is exactly
  // what makes the continuity assertion meaningful: the same Jolt body is on
  // both ends, so any discontinuity is the handoff's and not a second solver's.
  bool handoffOk = false;
  float posJump = 0.0f, velJump = 0.0f;
  // "Within one tick of Jolt drift", as data. Nothing steps between the two
  // flips here, so the honest number is zero and this is the allowance for a
  // solver that is free to normalise a quaternion or clamp a velocity on the
  // way through -- not a tolerance for a handoff that loses momentum.
  const float handoffTol = (float)BaselineNumber("debrisGhost.handoffTolVox", 0.05);
  {
    // ---- ITS OWN BODY, AND THE FIRST RUN SAYS WHY -------------------------
    //
    // This started out reusing the owned block from (a) and reported
    // `handoff pos jump 0.0000 vel jump 0.0000 (out 0 in 0)` -- three zeros
    // that look like success and are the signature of a handle that no longer
    // exists. The owned block's whole JOB in (a) is to settle back into the
    // grid, and settling back FREES THE BODY (that is what `owned control
    // settled 1` on the same line was reporting). So (c) was handing off a
    // corpse of a handle and measuring the transform of nothing.
    //
    // A fresh body, in the air, east of the seam: (c) runs no ticks at all,
    // so nothing can settle or fall out from under it.
    const uint64_t hoH = drop(160, 160);
    BodyTransform before{};
    Vec3 linBefore{}, angBefore{};
    phys.GetTransform(hoH, before);
    phys.GetBodyVelocities(hoH, linBefore, angBefore);
    // Give it a real velocity, or "velocity is continuous" is the trivial
    // claim that zero equals zero (the fixture-that-cannot-fail trap again).
    phys.SetBodyVelocities(hoH, Vec3{3.0f, 1.5f, -2.0f}, Vec3{});
    phys.GetBodyVelocities(hoH, linBefore, angBefore);

    net::BodyHandoff ho;
    const bool built = debris.BuildHandoff(hoH, kPeer, ho);
    const bool becameGhost = debris.IsGhost(hoH) && !phys.IsBodyDynamic(hoH);
    // ...and back. The far machine would send this; here it is the same
    // record fed straight back in, which is the tightest possible test of the
    // round trip and needs no transport.
    net::BodyHandoff back = ho;
    back.newOwner = DebrisSystem::kLocalOwner;
    back.announce.owner = DebrisSystem::kLocalOwner;
    const uint64_t got = debris.ApplyBodyHandoff(back);
    BodyTransform after{};
    Vec3 linAfter{}, angAfter{};
    phys.GetTransform(got, after);
    phys.GetBodyVelocities(got, linAfter, angAfter);
    posJump = (after.pos - before.pos).len();
    velJump = (linAfter - linBefore).len();
    handoffOk = built && becameGhost && got == hoH &&
                !debris.IsGhost(hoH) && phys.IsBodyDynamic(hoH) &&
                posJump < handoffTol && velJump < handoffTol &&
                debris.Owner().handoffsOut >= 1 &&
                debris.Owner().handoffsIn >= 1;
  }
  note(handoffOk, "handoff");

  // ---- (d) ItemTake on a ghost item yields a named ItemGrant --------------
  //
  // The registry is a layer above debris, so the two callbacks are what the
  // game binds and what this drives. The "take" here just destroys the body,
  // which is what DropItemToWorld's inverse does.
  bool itemOk = false;
  std::string grantedName;
  {
    // The GHOST arm is the interesting one: E on a body another machine owns.
    // The WHOLE item (W2-M): a grant used to carry name, dye and a damage word
    // nothing wrote, and a flask picked up across the wire arrived empty.
    debris.SetItemLookupFn([&](uint64_t bh, ItemInstance& it) {
      if (bh != ghostH) return false;
      it.name = "iron sword";
      it.dye = 5;
      it.fillMat = 9;
      it.fillAmt = 12;
      it.damage.shells.resize(1);
      it.damage.shells[0].hp = 12.0f;
      return true;
    });
    // A PICKUP TAKES THE THING OFF THE GROUND. The first version of this
    // just returned true, and the refusal assertion below then measured
    // nothing: asking a second time found the body still lying there and was
    // granted again. DestroyBody is what DropItemToWorld's inverse does, and
    // it is the seam that fires OnBodyGone and clears the registry.
    debris.SetItemTakeFn(
        [&](uint64_t bh) { return bh == ghostH && debris.DestroyBody(bh); });

    // 1. E on the ghost queues a request and grants NOTHING yet -- the whole
    //    point is that the non-owner may not invent the item.
    const bool queued = debris.RequestItemTake(ghostId);
    net::ItemGrant early;
    const bool noEarlyGrant = !debris.PopItemGrant(early);
    net::ItemTake out;
    const bool sent = debris.PopItemTake(out) && out.globalId == ghostId &&
                      out.byPlayer == DebrisSystem::kLocalOwner;

    // 2. The owner's side of the same exchange, played here: ApplyItemTake
    //    resolves it. The body must be MINE for that, so the ownership fn is
    //    dropped for the length of the exchange and the body reclaimed --
    //    which is also, exactly, what a handoff would have done.
    debris.ClearOwnershipFn();
    net::BodyHandoff toMe;
    toMe.announce.globalId = ghostId;
    toMe.announce.owner = DebrisSystem::kLocalOwner;
    // The pose has to be real: ApplyBodyHandoff puts the body ON the
    // announce's transform (that is the whole point of carrying one), so a
    // default-constructed record would teleport the sword to the origin.
    phys.GetTransform(ghostH, toMe.announce.xf);
    debris.ApplyBodyHandoff(toMe);
    debris.ApplyItemTake(out);
    net::ItemGrant g{};
    const bool gotGrant = debris.PopItemGrant(g);
    grantedName = g.item.name;
    // ...and it survives the wire: the requester sees what the owner took.
    std::vector<uint8_t> gbuf;
    net::Encode(gbuf, g);
    ByteReader gr{gbuf.data(), gbuf.size()};
    net::ItemGrant gw{};
    const bool wired = net::Decode(gr, gw) && gr.off == gbuf.size();
    itemOk = queued && noEarlyGrant && sent && gotGrant && g.granted == 1 &&
             wired && gw.item.name == "iron sword" && gw.item.dye == 5 &&
             gw.item.fillMat == 9 && gw.item.fillAmt == 12 &&
             gw.item.damage.shells.size() == 1 &&
             gw.item.damage.shells[0].hp == 12.0f && gw.globalId == ghostId;
    // A REFUSAL IS A REPLY, and it has to be distinguishable. Asking again for
    // the body that has now been taken must come back granted == 0 rather
    // than silently producing nothing.
    net::ItemTake again{ghostId, kPeer};
    debris.ApplyItemTake(again);
    net::ItemGrant g2{};
    itemOk = itemOk && debris.PopItemGrant(g2) && g2.granted == 0;
  }
  note(itemOk, "item");

  // THE WIDENED SIGNATURE IS ACTUALLY CARRYING AN IDENTITY (M9.4-E). A build
  // that passed 0 for every body would look exactly like this gate passing:
  // the positional split still answers correctly, and the hysteresis would
  // have quietly gone back to being chunk-keyed. So the id is asserted, not
  // just accepted.
  note(idsSeen > 0 && idsZero == 0, "bodyid");

  // THE NUMBERS COME OUT BEFORE THE TEARDOWN. Reset() clears the probe along
  // with the bodies it counted, and the first run of this gate reported
  // `poses driven 0 refused 0, emitters skipped 0, (out 0 in 0)` for exactly
  // that reason -- five instruments reading zero because the detail string was
  // built after the reset. A probe you zero before you print it is not a probe.
  const DebrisSystem::OwnerProbe probe = debris.Owner();

  // ---- PUT THE SEAM BACK ---------------------------------------------------
  //
  // THE LAST THING THIS GATE DOES, and it is load-bearing: gates share one
  // DebrisSystem (see kOrder's note), so an ownership function left installed
  // would turn every later gate's bodies into ghosts that emit nothing, and
  // the failures would be attributed to those gates. The callbacks go too.
  debris.ClearOwnershipFn();
  debris.SetChunkOwnedFn(nullptr);
  debris.SetItemLookupFn(nullptr);
  debris.SetItemTakeFn(nullptr);
  debris.Reset();

  const bool ok = failed.empty();
  detail = Format(
      "ghost: kinematic=%d track err %.4f vox, ops authored %u cell / %u spawn "
      "(owned control settled %u); scan skipped %u peer chunk(s), %u of mine; "
      "handoff pos jump %.4f vel jump %.4f (out %u in %u); item grant '%s'; "
      "poses driven %u refused %u, emitters skipped %u; owner fn asked %u "
      "time(s), %u without an id%s%s",
      ghostKinematic ? 1 : 0, trackErr, ghostCellOps, ghostSpawns, ownedSettled,
      skippedPeerChunk, skippedOwnChunk, posJump, velJump,
      probe.handoffsOut, probe.handoffsIn,
      grantedName.c_str(), probe.ghostsDriven, probe.posesRefused,
      probe.emittersSkipped, idsSeen, idsZero,
      failed.empty() ? "" : "; FAILED: ", failed.c_str());
  std::printf("debris ghost: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- big-body-collider -----------------------------------------------------
//
// A FELLED TREE MUST NOT COST 100 ms A STEP. The owner report was "cut a tree
// down, or split a big body, and it runs at 3 fps for a few seconds"; the
// measured cause (physics.cpp, kDiscreteMinExtentVox) was a crown collider of
// 1024 one-voxel boxes that Jolt LinearCast on every step, because a compound's
// inner radius is its smallest box's. This pins the three rules that fixed it,
// structurally, so no timing has to be trusted:
//
//   A. a sparse crown stays within Physics::ColliderBoxBudget() boxes AND every
//      one of its voxels is inside a box (the old 1024 cap covered a sixth);
//   B. its solid trunk survives as ONE exact box (the fat part is kept, only
//      the leaves go coarse), and a thick body steps discretely;
//   C. a small, thin body is untouched: one box, still LinearCast -- the
//      `body-fastfall` guarantee is for exactly these.
//
// Pure Jolt, no world, no terrain: the bodies are made, read and removed.
Status GateBigBodyCollider(Ctx& c, std::string& detail) {
  Physics& phys = c.phys;
  std::vector<float> dens;
  for (const auto& m : c.mats) dens.push_back((float)m.gpu.density);

  // The crown: a radius-28 ball at 35% dither (the tree-fell fixture's rim
  // density), on a 6x60x6 trunk listed FIRST so the greedy merge meets it as
  // one run, the way an island's flood meets a trunk.
  std::vector<DebrisVoxel> tree;
  const int R = 28, T = 60;
  for (int z = 0; z < 6; z++)
    for (int y = 0; y < T; y++)
      for (int x = 0; x < 6; x++)
        tree.push_back({(int8_t)(R - 3 + x), (int8_t)y, (int8_t)(R - 3 + z), 0,
                        kMatStone});
  for (int z = 0; z <= 2 * R; z++)
    for (int y = 0; y <= 2 * R; y++)
      for (int x = 0; x <= 2 * R; x++) {
        const int dx = x - R, dy = y - R, dz = z - R;
        if (dx * dx + dy * dy + dz * dz > R * R) continue;
        const bool inTrunk = x >= R - 3 && x < R + 3 && z >= R - 3 && z < R + 3 &&
                             y + T - R < T;
        if (inTrunk) continue;
        uint32_t h = (uint32_t)(x * 73856093) ^ (uint32_t)(y * 19349663) ^
                     (uint32_t)(z * 83492791);
        h ^= h >> 13; h *= 0x5BD1E995u; h ^= h >> 15;
        if (h % 100u >= 35u) continue;
        tree.push_back({(int8_t)x, (int8_t)(y + T - R), (int8_t)z, 0, kMatStone});
      }
  BodyTransform xf{};
  xf.pos = Vec3{900.0f, 700.0f, 900.0f};  // far from every other fixture
  xf.quat[3] = 1;
  const uint64_t big = phys.CreateDebrisBodyXf(tree, xf, dens, false);
  std::vector<SubShapeBox> boxes;
  const size_t nBoxes = big ? phys.GetSubShapeBoxes(big, boxes, 1u << 16) : 0;
  size_t uncovered = 0;
  // The trunk, exactly: a 6 x 60 x 6 box whose min corner is the trunk's.
  bool trunkExact = false;
  for (const SubShapeBox& b : boxes)
    trunkExact = trunkExact ||
                 (std::fabs(b.halfExtents.x - 3.0f) < 1e-3f &&
                  std::fabs(b.halfExtents.y - 0.5f * T) < 1e-3f &&
                  std::fabs(b.halfExtents.z - 3.0f) < 1e-3f &&
                  std::fabs(b.center.x - (float)R) < 1e-3f &&
                  std::fabs(b.center.y - 0.5f * T) < 1e-3f &&
                  std::fabs(b.center.z - (float)R) < 1e-3f);
  for (const DebrisVoxel& v : tree) {
    const Vec3 p{(float)v.x + 0.5f, (float)v.y + 0.5f, (float)v.z + 0.5f};
    bool in = false;
    for (const SubShapeBox& b : boxes) {
      if (std::fabs(p.x - b.center.x) <= b.halfExtents.x + 1e-3f &&
          std::fabs(p.y - b.center.y) <= b.halfExtents.y + 1e-3f &&
          std::fabs(p.z - b.center.z) <= b.halfExtents.z + 1e-3f) {
        in = true;
        break;
      }
    }
    if (!in) uncovered++;
  }
  const bool bigCast = phys.UsesLinearCast(big);
  if (big) phys.RemoveBody(big);

  // C: the small thin body.
  std::vector<DebrisVoxel> plank;
  for (int z = 0; z < 3; z++)
    for (int x = 0; x < 12; x++)
      plank.push_back({(int8_t)x, 0, (int8_t)z, 0, kMatStone});
  xf.pos = Vec3{960.0f, 700.0f, 900.0f};
  const uint64_t small = phys.CreateDebrisBodyXf(plank, xf, dens, false);
  std::vector<SubShapeBox> sboxes;
  const size_t nSmall = small ? phys.GetSubShapeBoxes(small, sboxes, 64) : 0;
  const bool smallCast = phys.UsesLinearCast(small);
  if (small) phys.RemoveBody(small);

  const bool a = big != 0 && (int)nBoxes <= Physics::ColliderBoxBudget() &&
                 uncovered == 0;
  const bool b = trunkExact && !bigCast;
  const bool cOk = small != 0 && nSmall == 1 && smallCast;
  const bool ok = a && b && cOk;
  char buf[512];
  std::snprintf(buf, sizeof buf,
      "A crown %zu vox -> %zu boxes (budget %d), %zu voxels outside every box | "
      "B trunk %s, %s (discrete from %.0f vox thick) | "
      "C plank -> %zu box, %s",
      tree.size(), nBoxes, Physics::ColliderBoxBudget(), uncovered,
      trunkExact ? "kept as one exact 6x60x6 box" : "NOT kept exact",
      bigCast ? "LinearCast" : "discrete",
      Physics::DiscreteMinExtentVox(), nSmall,
      smallCast ? "LinearCast" : "discrete");
  detail = buf;
  std::printf("big-body-collider: %s (%s)\n", ok ? "PASS" : "FAIL",
              detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& BodyGates() {
  static const std::vector<Gate> g = {
      {"debris", "phys", {}, false, GateDebris},
      {"settle-back", "phys", {}, false, GateSettleBack},
      {"debris-coat", "phys", {}, false, GateDebrisCoat},
      {"player-body", "phys", {}, false, GatePlayerBody},
      {"ragdoll-joints", "phys", {}, false, GateRagdollJoints},
      {"body-fastfall", "phys", {}, false, GateBodyFastFall},
      {"big-body-collider", "phys", {}, false, GateBigBodyCollider},
      {"debris-ghost", "phys", {}, false, GateDebrisGhost},
  };
  return g;
}

}  // namespace selftest
