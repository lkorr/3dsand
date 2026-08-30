// selftest_mob.cpp — mob selftest gates.
//
// Bodies moved verbatim out of the old monolithic RunSelftest; see
// scripts/split_selftest.py for the exact source ranges. Each gate returns a
// Status and fills `detail` with the parenthetical the old printf carried, so
// the console output is unchanged and --json can carry the same numbers.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <string>
#include <vector>

#include "game/avatar.h"
#include "game/bodyreg.h"
#include "game/thirdperson.h"
#include "game/brush.h"
#include "game/camera.h"
#include "game/player.h"
#include "gpu/resources.h"
#include "sim/microbody.h"
#include "sim/reactcpu.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// ---- mob ---------------------------------------------------------------
Status GateMob(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const std::vector<MaterialDef>& mats = c.mats;
  Physics& phys = c.phys;
  DebrisSystem& debris = c.debris;
  MobSystem& mobs = c.mobs;
  const ItemLibrary& items = c.items;
  const uint32_t W = c.width;
  const uint32_t H = c.height;
  rhi::TextureView& view = c.view;
// Milestone B mobs: spawn the generated dummy on terrain — it must stand
// and walk (kinematic limbs over cached-chunk ground), lose an arm to
// Sever (joint destroyed, limb adopted as debris), die to a vital hit
// (whole-body ragdoll), and every piece must go to sleep.
bool mobOk = false;
{
  debris.Reset();
  mobs.Reset();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  if (mobs.Defs().empty()) {
    std::printf("mob: FAIL (no mob defs — run scripts/gen_test_mob.py)\n");
  } else {
    // Select the dummy BY NAME and resolve limb indices by name too: mob
    // defs load in filename order, so adding assets/mobs/critter.* would
    // otherwise silently re-point this fixture at a different rig. The
    // loader also topologically sorts limbs, so positional indices are not
    // stable across sidecar edits either.
    int dummyDef = 0;
    for (size_t i = 0; i < mobs.Defs().size(); i++)
      if (mobs.Defs()[i].name == "dummy") dummyDef = (int)i;
    const MobDef& dd = mobs.Defs()[dummyDef];
    auto limbIndex = [&](const char* name) {
      for (size_t i = 0; i < dd.limbs.size(); i++)
        if (dd.limbs[i].name == name) return (int)i;
      return -1;
    };
    const int nLimbs = (int)dd.limbs.size();
    int h = World::TerrainHeight(140, 140, kDefaultSeed);
    uint32_t t = 6000;
    auto mobTick = [&](std::vector<BrushOp> ops) {
      std::vector<ParticleSpawn> spawns;
      std::vector<CellOp> cellOps;
      mobs.PreTick(t + 1, world, ops, cellOps, spawns);
      debris.QueueSupportEvents(world.Snap());
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

    uint64_t id = mobs.Spawn(dummyDef, {137, h + 1, 139});
    Vec3 spawnPos = mobs.MobOrigin(id);
    // Same forward-locomotion invariant the critter is held to (below): the
    // legacy scale-1 rig must keep walking along its facing, so a fix aimed
    // at one model can never silently reverse the other.
    Vec3 dPrev = spawnPos;
    float dAlong = 0.0f, dPath = 0.0f;
    for (int i = 0; i < 120; i++) {
      Vec3 face = mobs.MobFacing(id);
      mobTick({});
      Vec3 now = mobs.MobOrigin(id);
      Vec3 step{now.x - dPrev.x, 0, now.z - dPrev.z};
      dPrev = now;
      dAlong += step.x * face.x + step.z * face.z;
      dPath += std::sqrt(step.x * step.x + step.z * step.z);
    }
    bool dummyForward = dPath > 1.0f && dAlong > 0.5f * dPath;
    Vec3 walked = mobs.MobOrigin(id);
    float dist = (walked - spawnPos).len();
    // The mob wanders ~20 voxels over these 120 ticks, and terrain averages
    // ~0.34 voxels of fall per voxel travelled, so a healthy mob legitimately
    // ends up several voxels below where it started. The bound only has to
    // catch "fell through the world" / "stuck in the air", not honest walking
    // downhill — it was 6.0 when hills spanned 45 voxels and the mob grazed
    // it at exactly -6.0 once hills spanned 90.
    bool standing = mobs.IsAlive(id) && mobs.LimbBodyCount() == (uint32_t)nLimbs &&
                    std::abs(walked.y - (float)(h + 1)) < 16.0f;

    // sever arm.L
    uint32_t debrisBefore = debris.BodyCount();
    mobs.Sever(id, limbIndex("arm.L"));
    bool severed = mobs.LimbBodyCount() == (uint32_t)(nLimbs - 1) &&
                   debris.BodyCount() == debrisBefore + 1 && mobs.IsAlive(id);
    for (int i = 0; i < 60; i++) mobTick({});

    // vital hit: decapitation kills — remaining 5 limbs ragdoll into debris.
    // settle window covers the blood drying out (its chunks stay dirty
    // while wet, and terrain refreshes wake nearby bodies by design)
    mobs.Sever(id, limbIndex("head"));
    bool died = !mobs.IsAlive(id) || mobs.MobCount() == 0;
    // 2500, not 500: dismemberment now throws real blood voxels (gore.
    // severVoxels) on top of the wound drip, and wet blood keeps its chunks
    // dirty while it flows and soaks, which by design keeps waking the
    // bodies resting in it. Measured on the RTX 3060 Ti: awake=5 at 500
    // ticks, awake=0 by 2500. The assertion still tests that the scene
    // reaches full rest — this is a slower settle, not a leak, and shrinking
    // the blood to fit the old window would be testing the tuning instead of
    // the invariant.
    // 5000, not 2500: steering (LocomotionDef) changed where the dummy is
    // standing when it dies, so the corpse and its blood land on different
    // ground than the old straight-line walk left them on, and that ground
    // takes longer to go quiet. MEASURED, not guessed — at the old window
    // this reads awake=4, and the same run reaches awake=0 well before the
    // new one expires. The assertion still tests that the scene reaches FULL
    // rest; only the deadline moved, exactly as it did when gore volume grew
    // (see the note above).
    for (int i = 0; i < 5000; i++) mobTick({});
    uint32_t awake = debris.ActiveBodyCount();
    bool settled = awake == 0 && mobs.MobCount() == 0;

    mobOk = standing && severed && died && settled && dummyForward;
    std::printf(
        "mob: %s (stood=%d walked %.1f vox, forward %.1f/%.1f, sever=%d, "
        "death=%d, %u debris pieces, %u awake after settle)\n",
        mobOk ? "PASS" : "FAIL", standing ? 1 : 0, dist, dAlong, dPath,
        severed ? 1 : 0, died ? 1 : 0, debris.BodyCount(), awake);
    debris.Reset();
    mobs.Reset();

    // ---- steering: free angles and a bounded turn rate -------------------
    // The invariant here is NOT "the mob turns" (it always did) but "the
    // body's facing only ever moves at a bounded rate, and it can travel
    // along any angle". The old locomotion snapped heading by exactly 90
    // degrees in one tick, which satisfies every did-it-change and
    // distance-walked test — so those are not the assertions to make.
    //
    // Runs on its OWN mob, after the fixture above is fully torn down.
    // Sharing the dummy would leave the corpse somewhere the death/settle
    // window was never calibrated for, and this gate would then show up as
    // an unrelated failure in `mob` — a fixture must not perturb its
    // neighbours.
    {
      uint64_t sid = mobs.Spawn(dummyDef, {137, h + 1, 139});
      bool turnBounded = true, turnedFreely = false, turnArrived = false;
      bool turnCurved = false;
      const float startH = mobs.MobHeading(sid);
      // 2.4 rad ~= 137 degrees: not a multiple of 90, so a snapping
      // implementation cannot land on it and a quantizing one cannot rest
      // there.
      const float targetH = startH + 2.4f;
      // Turn-rate cap is the def's own; the drive couples it to speed, so
      // the per-tick bound is the uncoupled rate plus slack for the accel
      // ramp's arrival step.
      const float kMaxRate = 3.6f;   // LocomotionDef default turnRate
      const float kBound = kMaxRate * kTickDt * 1.5f;
      float prevH = startH;
      int ticksTurning = 0;
      // Distinct headings the mob was seen MOVING along, bucketed at 5
      // degrees. A snap-turner only translates along a few quantized yaws; a
      // steered body sweeps continuously through them. This is what makes
      // "any angle" an assertion rather than a claim.
      std::unordered_set<int> movedBuckets;
      Vec3 prevP = mobs.MobOrigin(sid);
      for (int i = 0; i < 90; i++) {
        mobs.SetDesiredHeading(sid, targetH);  // hold intent against wander
        mobTick({});
        float nowH = mobs.MobHeading(sid);
        float step = std::abs(std::remainder(nowH - prevH, 6.2831853f));
        if (step > kBound) turnBounded = false;
        if (step > 1e-5f) ticksTurning++;
        prevH = nowH;
        Vec3 nowP = mobs.MobOrigin(sid);
        Vec3 d{nowP.x - prevP.x, 0, nowP.z - prevP.z};
        prevP = nowP;
        // Only ticks where it genuinely translated, so a mob spinning in
        // place cannot satisfy the curvature test.
        if (std::sqrt(d.x * d.x + d.z * d.z) > 1e-3f)
          movedBuckets.insert(
              (int)std::floor(std::atan2(d.x, d.z) / 0.0872664626f));
      }
      // It must have taken real TIME: 2.4 rad at 3.6 rad/s is ~0.67 s. A
      // snap arrives in one tick.
      turnArrived =
          std::abs(std::remainder(prevH - targetH, 6.2831853f)) < 0.05f;
      turnedFreely = ticksTurning > 8;
      // >4 distinct travel directions in one turn: impossible for the old
      // 90-degree snap (4 in the whole plane) and for turn-in-place-then-go
      // (1).
      turnCurved = movedBuckets.size() > 4;
      bool steerOk =
          turnBounded && turnArrived && turnedFreely && turnCurved;
      mobOk = mobOk && steerOk;
      std::printf(
          "mob steering: %s (bounded=%d arrived=%d gradual=%d curved=%d, "
          "%d travel dirs over %d turning ticks)\n",
          steerOk ? "PASS" : "FAIL", (int)turnBounded, (int)turnArrived,
          (int)turnedFreely, (int)turnCurved, (int)movedBuckets.size(),
          ticksTurning);
      debris.Reset();
      mobs.Reset();
    }

    // ---- Wave 2a: procedural gait + IK + clips on the critter rig ----
    // Per-tick INVARIANTS, not rate comparisons: (a) at most one gait group
    // swings at a time, which is the whole gait state machine; (b) some foot
    // is always planted, or the mob is airborne; (c) losing a leg silently
    // drops it from the schedule; (d) a non-fatal hit starts the flinch clip
    // and that clip eventually blends out to nothing.
    int critterDef = -1;
    for (size_t i = 0; i < mobs.Defs().size(); i++)
      if (mobs.Defs()[i].name == "critter") critterDef = (int)i;
    if (critterDef < 0) {
      std::printf("mob gait: SKIP (no critter def — run "
                  "scripts/gen_critter_mob.py)\n");
    } else {
      const MobDef& cd = mobs.Defs()[critterDef];
      auto critterLimb = [&](const char* nm) {
        for (size_t i = 0; i < cd.limbs.size(); i++)
          if (cd.limbs[i].name == nm) return (int)i;
        return -1;
      };
      uint64_t cid = mobs.Spawn(critterDef, {137, h + 1, 139});
      int maxSwing = 0, everSwung = 0, neverPlanted = 0;
      // FORWARD-LOCOMOTION CHECK. A mob must travel along the direction it
      // faces. Sample facing every tick and accumulate the dot product of
      // each tick's displacement with that tick's facing, so a mid-walk turn
      // (the critter turns 90 deg when blocked) can never make a
      // forward-walking mob look backward. A model authored nose-backwards
      // walks in reverse and this sum goes negative — that was a real bug.
      Vec3 prevPos = mobs.MobOrigin(cid);
      float alongFacing = 0.0f, pathLen = 0.0f;
      for (int i = 0; i < 150; i++) {
        Vec3 face = mobs.MobFacing(cid);
        mobTick({});
        int sw = mobs.SwingingFeet(cid);
        int pl = mobs.PlantedFeet(cid);
        if (sw > maxSwing) maxSwing = sw;
        if (sw > 0) everSwung++;
        if (pl == 0) neverPlanted++;
        Vec3 now = mobs.MobOrigin(cid);
        Vec3 step{now.x - prevPos.x, 0, now.z - prevPos.z};
        prevPos = now;
        alongFacing += step.x * face.x + step.z * face.z;
        pathLen += std::sqrt(step.x * step.x + step.z * step.z);
      }
      // Require the motion to be not merely forward-ish but essentially ALL
      // forward: a backwards model scores about -1 here, a correct one +1.
      bool walksForward = pathLen > 1.0f && alongFacing > 0.5f * pathLen;
      // groups are diagonal PAIRS, so up to 2 feet may swing together, but
      // never a third (that would mean two groups swinging at once)
      bool oneGroup = maxSwing <= 2;
      bool stepped = everSwung > 0;
      bool grounded = neverPlanted < 30;   // brief all-swing frames are ok

      // limb loss: sever a front-left leg; its chain must drop out entirely
      int beforeFeet = mobs.SwingingFeet(cid) + mobs.PlantedFeet(cid);
      mobs.Sever(cid, critterLimb("legU.FL"));
      for (int i = 0; i < 40; i++) mobTick({});
      int afterFeet = mobs.SwingingFeet(cid) + mobs.PlantedFeet(cid);
      bool legLost = mobs.IsAlive(cid) && afterFeet < beforeFeet;

      // flinch clip: a non-fatal hit on the torso starts "attack"
      uint64_t torso = mobs.LimbBody(cid, critterLimb("torso"));
      mobs.Damage(torso, 1.0f, mobs.MobOrigin(cid), 0.0f);
      int clipsNow = mobs.ActiveClips(cid);
      for (int i = 0; i < 40; i++) mobTick({});
      int clipsLater = mobs.ActiveClips(cid);
      // the clip must both START and eventually retire (blend-out works)
      bool clipOk = clipsNow >= 1 && clipsLater == 0;

      bool gaitOk = oneGroup && stepped && grounded && legLost && clipOk &&
                    walksForward;
      std::printf(
          "mob gait: %s (max %d feet swinging, stepped on %d/150 ticks, "
          "%d all-swing ticks, leg loss %d->%d feet, clip %d->%d, "
          "forward %.1f of %.1f vox travelled)\n",
          gaitOk ? "PASS" : "FAIL", maxSwing, everSwung, neverPlanted,
          beforeFeet, afterFeet, clipsNow, clipsLater, alongFacing, pathLen);
      if (!walksForward)
        std::printf("  critter walks BACKWARDS (displacement . facing = "
                    "%.2f over %.1f vox of path)\n",
                    alongFacing, pathLen);
      mobOk = mobOk && gaitOk;

      // ---- Wave 3: the microvoxel render pass actually draws ----
      // The critter is a "scale": 2 def, so its limbs emit NO cube instances
      // and must come entirely from microbody.wgsl. Render the same frame
      // twice — once with the micro pass, once without — and count differing
      // pixels. That proves three things at once with no depth readback: the
      // pass produced fragments, they survived the reversed-Z depth test
      // against the world raymarch (a pass writing depth behind the terrain
      // would change nothing), and the limbs are not ALSO being drawn by the
      // cube path (which would make both images identical).
      {
        const uint32_t W = 640, H = 360;

        // Slot lists exactly as the frame loop builds them: through the ONE
        // slot walk in game/bodyreg.h.
        BodyRegistry bodyReg(debris, mobs, nullptr);
        std::vector<BodyXformGpu> xf;
        bodyReg.BuildXforms(xf);
        if (!xf.empty())
          ctx.queue.WriteBuffer(world.bodyXforms, 0, xf.data(),
                                xf.size() * sizeof(BodyXformGpu));
        std::vector<MicroBodyInstGpu> microInsts;
        bodyReg.BuildMicroInsts(microInsts);
        std::vector<BodyVoxInst> inst;
        bodyReg.BuildInstances(inst);
        if (!inst.empty())
          ctx.queue.WriteBuffer(world.bodyInstances, 0, inst.data(),
                                inst.size() * sizeof(BodyVoxInst));

        // Close in and AIMED. The critter is ~3 world voxels across and has
        // been walking for 190 ticks, so a fixed yaw/pitch pair aimed at the
        // spawn point misses it entirely and the pixel threshold below stops
        // meaning anything. Take the torso's live transform and derive the
        // camera angles from the look vector.
        Vec3 target{};
        {
          std::vector<BodyXformGpu> t;
          mobs.AppendXforms(t);
          if (!t.empty()) target = Vec3{t[0].pos[0], t[0].pos[1], t[0].pos[2]};
          else target = mobs.MobOrigin(cid);
        }

        auto shoot = [&](bool withMicro, std::vector<uint8_t>& out) {
          rhi::Texture tex = ctx.device.CreateTexture(
        {W, H, 1}, rhi::TextureFormat::RGBA8Unorm,
        rhi::TextureUsage::RenderAttachment | rhi::TextureUsage::CopySrc,
        "shotTarget");
          // Upload BEFORE the render pass opens (barrier graph §4.6).
          uint32_t microCount =
              withMicro ? sim.UploadMicroBodyInsts(ctx.queue, microInsts) : 0u;
          rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
          rhi::RenderPass rp = sim.BeginRenderPass(
              enc, tex.CreateView(), rhi::TextureFormat::RGBA8Unorm, W, H);
          sim.DrawWorld(rp);
          sim.DrawBodies(rp, (uint32_t)inst.size());
          sim.DrawMicroBodies(rp, microCount);
          rp.End();
          rhi::Buffer shot = CreateBuffer(
              ctx.device, (uint64_t)W * H * 4,
              rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst, "microShot");
          rhi::TexelCopyTexture srcT{};
          srcT.texture = tex;
          rhi::TexelCopyBuffer dstB{};
          dstB.buffer = shot;
          dstB.bytesPerRow = W * 4;
          dstB.rowsPerImage = H;
          rhi::Extent3D ext{W, H, 1};
          enc.CopyTextureToBuffer(srcT, dstB, ext);
          ctx.queue.Submit(enc.Finish());
          out.assign((size_t)W * H * 4, 0);
          rhi::ReadBufferBlocking(ctx.device, shot, 0, out.data(), (size_t)(out.size()));
        };

        // VIEW SWEEP. One camera angle cannot test an OBB: the box has six
        // faces and a winding bug in even one of them only hides the body
        // from the directions that face it. Orbit the critter through the 8
        // diagonal octants AND the 6 axis directions, and require the pass to
        // change pixels from EVERY one. This is the regression for the
        // mixed-winding bug (backface culling ate the faces whose triangles
        // wound the other way, so the critter vanished from half the compass).
        const Vec3 kDirs[] = {
            // 8 diagonal octants
            {1, 1, 1},   {1, 1, -1},  {1, -1, 1},  {1, -1, -1},
            {-1, 1, 1},  {-1, 1, -1}, {-1, -1, 1}, {-1, -1, -1},
            // 6 axis directions (grazing/axis-aligned rays are their own case:
            // the DDA's near-zero-component guard only matters here)
            {1, 0, 0},   {-1, 0, 0},  {0, 0, 1},   {0, 0, -1},
            {0, 1, 0},   {0, -1, 0},
        };
        const int kNumDirs = (int)(sizeof(kDirs) / sizeof(kDirs[0]));
        uint32_t minDiff = 0xFFFFFFFFu, maxDiff = 0;
        int badDirs = 0, firstBad = -1;
        std::vector<uint8_t> withPix, withoutPix, keepPix;
        for (int d = 0; d < kNumDirs; d++) {
          // Normalize then push out to a fixed radius so every direction
          // frames the critter at the same distance — otherwise a diagonal
          // eye sits 1.7x further out than an axial one and the pixel counts
          // are not comparable.
          Vec3 dir = kDirs[d].normalized();
          Vec3 eye = target + dir * 8.0f;
          Vec3 look = (target - eye).normalized();
          Camera cam2;
          cam2.yaw = std::atan2(look.z, look.x);
          cam2.pitch = std::asin(std::clamp(look.y, -1.0f, 1.0f));
          WriteRenderParams(ctx.queue, world, eye, cam2, (float)W / H, true, 0);

          shoot(true, withPix);
          shoot(false, withoutPix);
          uint32_t diff = 0;
          for (size_t p = 0; p + 3 < withPix.size(); p += 4)
            if (withPix[p] != withoutPix[p] ||
                withPix[p + 1] != withoutPix[p + 1] ||
                withPix[p + 2] != withoutPix[p + 2])
              diff++;
          if (diff < minDiff) minDiff = diff;
          if (diff > maxDiff) maxDiff = diff;
          // A scale-2 critter framed from 8 voxels away covers several
          // thousand pixels at 640x360. 500 is a floor only a direction that
          // drew nothing — or drew entirely behind the terrain, i.e. got the
          // depth convention wrong — can fall under. Deliberately far below
          // the observed count so gait wander can never flake the test.
          if (diff < 500) {
            badDirs++;
            if (firstBad < 0) firstBad = d;
          }
          // keep the first diagonal's image as the visual artifact
          if (d == 0) keepPix = withPix;
        }
        // SINGLE-BODY PROBE. The sweep above draws all 9 limbs at once, so a
        // box that vanishes is masked by its neighbours — the critter as a
        // whole stays visible even when individual limbs drop out. Culling is
        // decided per triangle in the body's OWN object space, so isolate ONE
        // body with an IDENTITY rotation: object space then equals world
        // space, and the camera's octant maps 1:1 onto the box's own octant.
        // A winding bug in the 36-vertex cube shows up here as a whole octant
        // rendering nothing, which is exactly the reported symptom.
        int soloBad = 0, soloFirst = -1;
        uint32_t soloMin = 0xFFFFFFFFu;
        {
          std::vector<MicroBodyInstGpu> solo(1, microInsts[0]);
          // Park the single body in open air well above the terrain, with an
          // identity quaternion, so nothing occludes it and no gait pose
          // rotates the octants out from under the assertion.
          uint32_t slot = solo[0].slot;
          Vec3 soloPos{target.x, target.y + 24.0f, target.z};
          std::vector<BodyXformGpu> sxf = xf;
          if (slot < sxf.size()) {
            sxf[slot].pos[0] = soloPos.x;
            sxf[slot].pos[1] = soloPos.y;
            sxf[slot].pos[2] = soloPos.z;
            sxf[slot].quat[0] = 0.0f;
            sxf[slot].quat[1] = 0.0f;
            sxf[slot].quat[2] = 0.0f;
            sxf[slot].quat[3] = 1.0f;
            ctx.queue.WriteBuffer(world.bodyXforms, 0, sxf.data(),
                                  sxf.size() * sizeof(BodyXformGpu));
          }
          std::vector<uint8_t> sWith, sWithout;
          for (int d = 0; d < kNumDirs; d++) {
            Vec3 dir = kDirs[d].normalized();
            Vec3 eye = soloPos + dir * 6.0f;
            Vec3 look = (soloPos - eye).normalized();
            Camera cam3;
            cam3.yaw = std::atan2(look.z, look.x);
            cam3.pitch = std::asin(std::clamp(look.y, -1.0f, 1.0f));
            WriteRenderParams(ctx.queue, world, eye, cam3, (float)W / H, true, 0);
            // Draw ONLY the micro pass against the world, twice, so the diff
            // isolates this one body.
            auto shootSolo = [&](bool withMicro, std::vector<uint8_t>& out) {
              // Upload BEFORE the render pass opens (barrier graph §4.6).
              uint32_t soloCount =
                  withMicro ? sim.UploadMicroBodyInsts(ctx.queue, solo) : 0u;
              rhi::Texture tex = ctx.device.CreateTexture(
        {W, H, 1}, rhi::TextureFormat::RGBA8Unorm,
        rhi::TextureUsage::RenderAttachment | rhi::TextureUsage::CopySrc,
        "shotTarget");
              rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
              rhi::RenderPass rp = sim.BeginRenderPass(
                  enc, tex.CreateView(), rhi::TextureFormat::RGBA8Unorm, W, H);
              sim.DrawWorld(rp);
              sim.DrawMicroBodies(rp, soloCount);
              rp.End();
              rhi::Buffer shot = CreateBuffer(
                  ctx.device, (uint64_t)W * H * 4,
                  rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                  "microSolo");
              rhi::TexelCopyTexture srcT{};
              srcT.texture = tex;
              rhi::TexelCopyBuffer dstB{};
              dstB.buffer = shot;
              dstB.bytesPerRow = W * 4;
              dstB.rowsPerImage = H;
              rhi::Extent3D ext{W, H, 1};
              enc.CopyTextureToBuffer(srcT, dstB, ext);
              ctx.queue.Submit(enc.Finish());
              out.assign((size_t)W * H * 4, 0);
              rhi::ReadBufferBlocking(ctx.device, shot, 0, out.data(), (size_t)(out.size()));
            };
            shootSolo(true, sWith);
            shootSolo(false, sWithout);
            uint32_t sd = 0;
            for (size_t p = 0; p + 3 < sWith.size(); p += 4)
              if (sWith[p] != sWithout[p] || sWith[p + 1] != sWithout[p + 1] ||
                  sWith[p + 2] != sWithout[p + 2])
                sd++;
            if (sd < soloMin) soloMin = sd;
            // A single limb 6 voxels away fills hundreds of pixels; 50 is a
            // floor only "drew nothing at all" can fall under.
            if (sd < 50) {
              soloBad++;
              if (soloFirst < 0) soloFirst = d;
            }
          }
          // restore the real transforms for anything downstream
          if (!xf.empty())
            ctx.queue.WriteBuffer(world.bodyXforms, 0, xf.data(),
                                  xf.size() * sizeof(BodyXformGpu));
        }

        bool microOk = !microInsts.empty() && badDirs == 0 && soloBad == 0;
        std::printf("micro body render: %s (%zu micro slots, %d/%d views drew, "
                    "%u..%u px changed of %u, solo body %d/%d views (min %u px), "
                    "%zu cube instances from micro limbs)\n",
                    microOk ? "PASS" : "FAIL", microInsts.size(),
                    kNumDirs - badDirs, kNumDirs, minDiff, maxDiff, W * H,
                    kNumDirs - soloBad, kNumDirs, soloMin, inst.size());
        if (badDirs > 0)
          std::printf("  critter INVISIBLE from %d view(s); first is dir "
                      "(%.0f,%.0f,%.0f)\n",
                      badDirs, kDirs[firstBad].x, kDirs[firstBad].y,
                      kDirs[firstBad].z);
        if (soloBad > 0)
          std::printf("  SOLO micro body INVISIBLE from %d/%d view(s); first "
                      "is dir (%.0f,%.0f,%.0f) — mixed cube winding?\n",
                      soloBad, kNumDirs, kDirs[soloFirst].x, kDirs[soloFirst].y,
                      kDirs[soloFirst].z);
        // Visual proof alongside the numeric one — the pixel count says
        // "something drew", the image says "it drew a critter".
        if (!keepPix.empty() && WriteBmpFile("screenshot_microbody.bmp", keepPix, W, H))
          std::printf("wrote screenshot_microbody.bmp\n");
        mobOk = mobOk && microOk;
      }

      // ---- per-voxel carving of a LIVE limb ----
      // The assertions are per-limb invariants, not rates (the frontier-rule
      // lesson):
      //   1. a carve removes REAL voxels from a limb that is still attached,
      //   2. the limb keeps its identity while it does — same body, still
      //      alive, still driving locomotion — so wounds are cosmetic until
      //      they are not, and
      //   3. carving the SAME spot until the limb cannot hold together
      //      severs it, with no hp threshold having decided that.
      // (3) is the load-bearing one: it is what makes dismemberment a
      // geometric consequence of what the player actually cut away.
      //
      // Run on the CRITTER (scale 2), not the dummy: the dummy's arm is five
      // world voxels, which is below the fragment floor before a single cut
      // lands, so it could only ever prove the collapse branch. Carving is
      // for micro rigs — that is where a limb has enough voxels for a wound
      // to be a wound rather than an amputation.
      if (critterDef >= 0) {
        debris.Reset();
        mobs.Reset();
        const MobDef& ccd = mobs.Defs()[critterDef];
        auto critterLimb = [&](const char* name) {
          for (size_t i = 0; i < ccd.limbs.size(); i++)
            if (ccd.limbs[i].name == name) return (int)i;
          return -1;
        };
        uint64_t cid = mobs.Spawn(critterDef, {143, h + 1, 143});
        for (int i = 0; i < 6; i++) mobTick({});
        // An upper leg: severable (unlike the torso, which is the root) and
        // big enough at scale 2 to survive several bites.
        const int carveLimb = critterLimb("legU.FL");
        uint32_t v0 = mobs.LimbVoxelCount(cid, carveLimb);
        uint32_t spawnVox = mobs.LimbVoxelsAtSpawn(cid, carveLimb);

        // One nick, off the joint: the limb must lose matter and keep living.
        uint32_t v1 = v0;
        bool stillAttached = false, nickHit = false;
        {
          uint64_t lb = mobs.LimbBody(cid, carveLimb);
          std::vector<ParticleSpawn> cs;
          // Aim at a voxel in the middle of the limb's own list — far from
          // the hip anchor, so this cannot be the joint-crossing sever path
          // in disguise. Not xf.pos: that is the min corner, not flesh.
          // Radius is in WORLD voxels; at scale 2 this is ~1 world voxel.
          nickHit = mobs.CarveLimbRadial(
              lb, mobs.LimbVoxelPos(cid, carveLimb, v0 / 2), 0.5f, true, true,
              world, cs);
          for (int i = 0; i < 3; i++) mobTick({});
          v1 = mobs.LimbVoxelCount(cid, carveLimb);
          stillAttached = mobs.LimbBody(cid, carveLimb) != 0;
        }

        // Now keep cutting the same limb until it comes off. The body handle
        // is re-read every pass: a carve rebuilds the collider and hands the
        // limb a NEW handle, so a cached one goes stale after the first cut
        // (the same trap the debris kerf test documents).
        int passes = 0;
        for (; passes < 40 && mobs.LimbBody(cid, carveLimb) != 0; passes++) {
          uint64_t lb = mobs.LimbBody(cid, carveLimb);
          std::vector<ParticleSpawn> cs;
          // Bite at flesh that is STILL THERE each pass. A fixed aim point
          // bores one hole and then sits in the cavity it made — what
          // survives is exactly what was out of its reach — so eating a limb
          // through means following the remaining meat.
          mobs.CarveLimbRadial(lb, mobs.LimbVoxelPos(cid, carveLimb, 0), 0.8f,
                               true, true, world, cs);
          mobTick({});
        }
        bool severedByCarving = mobs.LimbBody(cid, carveLimb) == 0;
        // The mob must survive losing an arm — otherwise "it died" would
        // explain the detachment just as well as carving did.
        bool aliveAfter = mobs.IsAlive(cid);

        bool carveOk = nickHit && v1 < v0 && v1 > 0 && stillAttached &&
                       severedByCarving && aliveAfter;
        std::printf("mob carve: %s (legU.FL %u/%u -> %u voxels attached=%d, "
                    "severed after %d more carves, mob alive=%d)\n",
                    carveOk ? "PASS" : "FAIL", v0, spawnVox, v1,
                    stillAttached ? 1 : 0, passes, aliveAfter ? 1 : 0);
        mobOk = mobOk && carveOk;

        // ---- CRATER SHAPE: the chunkiness slider, and its off switch ------
        //
        // `gore.carveChunkiness` 0 must remove EXACTLY the voxels the old
        // white-noise crater removed. That is not a nicety: the slider is
        // meant to be an A/B between the old look and the new one, and a
        // "0" that is merely close is not an A/B, it is a third behaviour
        // nobody chose. It is also the thing that makes the whole feature
        // safe to land — carving pushes ParticleSpawns into the tick stream,
        // so a crater that changed shape at 0 would move the world hash.
        //
        // THREE ARMS, NOT TWO. Running 0 then 1 and comparing tells you the
        // slider does something, but it cannot separate "the slider changed
        // the crater" from "the second mob was carved out of a body the first
        // pass had already dirtied". Running the CONTROL TWICE and requiring
        // arms 1 and 3 to agree is what makes the comparison mean anything
        // (gotcha: a hash-identity test needs three arms).
        int craterLost[3] = {0, 0, 0};
        int craterSpur[3] = {0, 0, 0};
        int humanDef = -1;
        for (size_t i = 0; i < mobs.Defs().size(); i++)
          if (mobs.Defs()[i].name == kAvatarDefName) humanDef = (int)i;
        if (humanDef >= 0) {
          const Tuning saved = CurrentTuning();
          const MobDef& hd = mobs.Defs()[humanDef];
          int torso = -1;
          for (size_t i = 0; i < hd.limbs.size(); i++)
            if (hd.limbs[i].name == "torso") torso = (int)i;
          const float arms[3] = {0.0f, 1.0f, 0.0f};
          for (int a = 0; a < 3 && torso >= 0; a++) {
            Tuning t = saved;
            t.gore.carveChunkiness = arms[a];
            SetCurrentTuning(t);
            debris.Reset();
            // rewindIds: the mob id seeds the crater noise, so the two control
            // arms are only comparable if the id repeats. This is the ONLY
            // caller that asks for it — rewinding by default moved `mob-burn`
            // (see the note on MobSystem::Reset).
            mobs.Reset(/*rewindIds=*/true);
            const uint64_t bid = mobs.Spawn(humanDef, {147, h + 1, 147});
            for (int i = 0; i < 6; i++) mobTick({});
            const uint32_t before = mobs.LimbVoxelCount(bid, torso);
            if (before == 0) break;
            // The torso, because it is the biggest limb on the rig (~4.5 world
            // voxels across) and a blast has to be SMALLER than the limb for
            // there to be a crater shape to measure at all. On a thigh — about
            // one voxel thick here — every arm removed everything inside the
            // radius and the numbers, though real, carried no signal.
            const uint64_t lb = mobs.LimbBody(bid, torso);
            std::vector<ParticleSpawn> cs;
            // eject=false: this measures the crater's SHAPE, and ejecting
            // ~400 flesh voxels as particles three times over would dump real
            // matter into a world the gates after this one place fixtures in
            // by absolute coordinate. The ejection path itself is covered by
            // the carve subtest above and by the game.
            mobs.CarveLimbRadial(lb, mobs.LimbVoxelPos(bid, torso, before / 2),
                                 1.5f, true, false, world, cs);
            for (int i = 0; i < 3; i++) mobTick({});
            craterLost[a] = (int)before - (int)mobs.LimbVoxelCount(bid, torso);
            // Survivors with 3+ open faces: isolated spurs and pockmarks. This
            // is the shape of what was LEFT, and it is what separates a torn
            // hole from a sprinkle — measuring it needs no world-space query,
            // which matters because a carve rebuilds the limb body and any
            // before/after probe in world space compares two different frames.
            craterSpur[a] = (int)mobs.LimbOpenFaceCount(bid, torso, 3);
          }
          SetCurrentTuning(saved);
          mobs.Reset();
          debris.Reset();
        }
        // Arms 1 and 3 are the same tuning on the same fixture: identical, or
        // arm 2 is being compared against noise and proves nothing.
        const bool craterRepeatable = craterLost[0] == craterLost[2] &&
                                      craterSpur[0] == craterSpur[2];
        const bool craterMoves = craterLost[1] != craterLost[0];
        // THE FRINGE IS SPARED. `1 - t^2` is still 0.36 at 80% of the radius,
        // so the old crater really did take a bit of everything it touched;
        // raising it to a power drops that to 0.05 and the same blast removes
        // materially LESS in total. Asserting "removes more" would be
        // asserting the opposite of the feature.
        const bool craterConcentrates = craterLost[1] < craterLost[0];
        // ...AND WHAT IT TOOK CAME OFF IN ONE PIECE. Removing less could also
        // mean removing less everywhere, which would leave the survivors MORE
        // pockmarked, not less. Spurs per voxel removed falling is only
        // consistent with the removal being contiguous — that is the
        // difference between a hole and a speckle, stated as a number.
        const float spurRate0 =
            craterLost[0] > 0 ? (float)craterSpur[0] / (float)craterLost[0] : 0.0f;
        const float spurRate1 =
            craterLost[1] > 0 ? (float)craterSpur[1] / (float)craterLost[1] : 0.0f;
        const bool craterCleaner = craterLost[1] > 0 && spurRate1 < spurRate0 * 0.8f;
        const bool craterOk = craterRepeatable && craterMoves &&
                              craterConcentrates && craterCleaner;
        std::printf(
            "mob crater shape (torso): %s (chunkiness 0/1/0 -> lost %d/%d/%d "
            "collider voxels, ragged survivors %d/%d/%d = %.2f/%.2f per voxel "
            "removed; repeatable=%d reaches=%d sparesFringe=%d cleaner=%d)\n",
            craterOk ? "PASS" : "FAIL", craterLost[0], craterLost[1],
            craterLost[2], craterSpur[0], craterSpur[1], craterSpur[2],
            spurRate0, spurRate1, craterRepeatable ? 1 : 0,
            craterMoves ? 1 : 0, craterConcentrates ? 1 : 0,
            craterCleaner ? 1 : 0);
        mobOk = mobOk && craterOk;
        debris.Reset();
        mobs.Reset();
      } else {
        std::printf("mob carve: SKIP (no critter def)\n");
      }

      // ---- dismemberment locomotion states: the maimed keep moving ----
      // The rules live in the sidecars ("states"). Assertions are
      // structural — which rule is active, the loco clip is running, the
      // gait is silenced while a crawl owns the pose — plus "it still makes
      // way along its facing", never rate comparisons (the frontier-rule
      // lesson: rates prove nothing).
      {
        debris.Reset();
        mobs.Reset();

        // dummy ladder, most-maimed-first in the sidecar so the indices
        // run BACKWARDS as limbs come off: intact -1, one leg lost -> 3
        // (crawl.oneLeg), both legs -> 2 (crawl.legless), plus an arm -> 1
        // (crawl.oneArm), no arms -> 0 (prone, speedScale 0).
        uint64_t did = mobs.Spawn(dummyDef, {137, h + 1, 139});
        for (int i = 0; i < 10; i++) mobTick({});
        int s0 = mobs.LocoState(did);
        mobs.Sever(did, limbIndex("leg.L"));
        for (int i = 0; i < 10; i++) mobTick({});
        int s1 = mobs.LocoState(did);
        mobs.Sever(did, limbIndex("leg.R"));
        for (int i = 0; i < 10; i++) mobTick({});
        int s2 = mobs.LocoState(did);
        bool dummyClip = mobs.ActiveClips(did) >= 1;
        // legless, it must still make way along its facing
        Vec3 dPrev2 = mobs.MobOrigin(did);
        float dAlong2 = 0.0f, dPath2 = 0.0f;
        for (int i = 0; i < 180; i++) {
          Vec3 face = mobs.MobFacing(did);
          mobTick({});
          Vec3 now = mobs.MobOrigin(did);
          Vec3 step{now.x - dPrev2.x, 0, now.z - dPrev2.z};
          dPrev2 = now;
          dAlong2 += step.x * face.x + step.z * face.z;
          dPath2 += std::sqrt(step.x * step.x + step.z * step.z);
        }
        bool dummyCrawls = dPath2 > 1.0f && dAlong2 > 0.5f * dPath2;
        // one arm gone: still a (slower) crawl; both arms gone: prone and
        // IMMOBILE — "it stops moving" is the invariant, so measure the
        // path, not the rate.
        mobs.Sever(did, limbIndex("arm.L"));
        for (int i = 0; i < 10; i++) mobTick({});
        int s3 = mobs.LocoState(did);
        mobs.Sever(did, limbIndex("arm.R"));
        for (int i = 0; i < 10; i++) mobTick({});
        int s4 = mobs.LocoState(did);
        bool proneClip = mobs.ActiveClips(did) >= 1;
        Vec3 pPrev = mobs.MobOrigin(did);
        float pPath = 0.0f;
        for (int i = 0; i < 100; i++) {
          mobTick({});
          Vec3 now = mobs.MobOrigin(did);
          Vec3 step{now.x - pPrev.x, 0, now.z - pPrev.z};
          pPrev = now;
          pPath += std::sqrt(step.x * step.x + step.z * step.z);
        }
        bool proneStill = pPath < 0.25f && mobs.IsAlive(did);
        bool dummyStates = s0 == -1 && s1 == 3 && s2 == 2 && s3 == 1 &&
                           s4 == 0 && dummyClip && dummyCrawls &&
                           proneClip && proneStill;

        // critter: one lost chain is the gait's own graceful degradation
        // (no rule fires); the second flips it to the crawl state, which
        // must silence the gait scheduler completely.
        uint64_t cid2 = mobs.Spawn(critterDef, {150, h + 1, 150});
        mobs.Sever(cid2, critterLimb("legU.FL"));
        for (int i = 0; i < 10; i++) mobTick({});
        int c1 = mobs.LocoState(cid2);
        mobs.Sever(cid2, critterLimb("legU.BR"));
        for (int i = 0; i < 10; i++) mobTick({});
        int c2 = mobs.LocoState(cid2);
        bool critClip = mobs.ActiveClips(cid2) >= 1;
        int swingTicks = 0;
        Vec3 cPrev2 = mobs.MobOrigin(cid2);
        float cAlong2 = 0.0f, cPath2 = 0.0f;
        for (int i = 0; i < 150; i++) {
          Vec3 face = mobs.MobFacing(cid2);
          mobTick({});
          if (mobs.SwingingFeet(cid2) > 0) swingTicks++;
          Vec3 now = mobs.MobOrigin(cid2);
          Vec3 step{now.x - cPrev2.x, 0, now.z - cPrev2.z};
          cPrev2 = now;
          cAlong2 += step.x * face.x + step.z * face.z;
          cPath2 += std::sqrt(step.x * step.x + step.z * step.z);
        }
        bool critCrawls = cPath2 > 1.0f && cAlong2 > 0.5f * cPath2;
        bool critStates = c1 == -1 && c2 == 0 && critClip &&
                          swingTicks == 0 && critCrawls;

        bool stateOk = dummyStates && critStates;
        std::printf(
            "mob dismember states: %s (dummy %d->%d->%d->%d->%d clip=%d "
            "crawled %.1f/%.1f vox, prone drift %.2f; critter %d->%d "
            "clip=%d swingTicks=%d crawled %.1f/%.1f vox)\n",
            stateOk ? "PASS" : "FAIL", s0, s1, s2, s3, s4, dummyClip ? 1 : 0,
            dAlong2, dPath2, pPath, c1, c2, critClip ? 1 : 0, swingTicks,
            cAlong2, cPath2);
        mobOk = mobOk && stateOk;
      }

      debris.Reset();
      mobs.Reset();
    }

    // ---- player avatar ----
    // The avatar reuses the mob rig but is driven by the PLAYER, so none of
    // the tests above touch it. What is worth asserting is exactly the part
    // that is avatar-specific and easy to break silently:
    //   1. the avatar def loads with every part, chain and state resolved
    //   2. spawning creates one Jolt body per part
    //   3. the body FOLLOWS the player rather than wandering off
    //   4. dismemberment walks DOWN the authored state ladder and each step
    //      actually slows the player (the movement coupling), ending with a
    //      state that cannot jump
    //   5. severed parts become debris and the avatar tears down cleanly
    {
      // Test whatever def the GAME uses as the avatar (kAvatarDefName), not
      // a hardcoded name — a selftest pinned to the old name would keep
      // passing against a character nobody plays.
      const std::string avDefName = kAvatarDefName;
      int wizDef = -1;
      for (size_t i = 0; i < mobs.Defs().size(); i++)
        if (mobs.Defs()[i].name == avDefName) wizDef = (int)i;
      if (wizDef < 0) {
        std::printf("avatar: SKIP (no %s def — run scripts/gen_%s.py)\n",
                    avDefName.c_str(), avDefName.c_str());
      } else {
        debris.Reset();
        const MobDef& wd = mobs.Defs()[wizDef];
        PlayerAvatar avatar;
        avatar.Init(&phys, &world, &debris, mats, &mobs);
        avatar.SetDefs(&mobs.Defs(), avDefName);

        int h2 = World::TerrainHeight(140, 140, kDefaultSeed);
        Player pl;
        pl.fly = false;
        // The avatar gates its locomotion clips on `grounded` (you do not
        // swing your arms mid-jump), and a default-constructed Player is not
        // grounded — so without this the walk/run clips never start and the
        // arms hang dead through the whole test. That is exactly the "arms
        // outstretched like a zombie" case, so leaving it unset would have
        // the test assert on a pose the game never shows.
        pl.grounded = true;
        pl.pos = Vec3{140.5f, (float)(h2 + 2) + Player::kHalfY, 140.5f};
        bool spawned = avatar.Spawn(pl, 0.0f);
        const int nParts = (int)wd.limbs.size();
        bool allBodies = (int)avatar.LimbBodyCount() == nParts;

        auto avTick = [&]() {
          std::vector<BrushOp> ops;
          std::vector<ParticleSpawn> spawns;
          std::vector<CellOp> cellOps;
          avatar.PreTick(t + 1, pl, 0.0f, kTickDt, world, ops, cellOps, spawns);
          debris.QueueSupportEvents(world.Snap());
          debris.PreTick(t + 1, world, cellOps, spawns);
          ++t;
          // THE MIRROR FOLLOWS THE BODY, as it does in the game. This was
          // pinned to the SPAWN chunk, and every loop below walks the player
          // tens of voxels away from it — well past the 3x3x3 CPU mirror, where
          // World::KindAt returns Unknown for everything. Unknown is treated as
          // passable (gotcha: the mirror is tiny), so anything that collides
          // through it falls through the world: the ramp fixture below dropped
          // the body 156 voxels before this line moved. The gait's own probe
          // never noticed because it reads the general chunk cache instead.
          const IVec3 pc{ifloor(pl.pos.x) >> 4, ifloor(pl.pos.y) >> 4,
                         ifloor(pl.pos.z) >> 4};
          SubmitTick(ctx, world, sim, t, kDefaultSeed, ops, {}, cellOps,
                     false, pc, true, false,
                     spawns);
          ctx.WaitIdle();
          ctx.ProcessEvents();
          phys.Step(kTickDt);
          debris.PostStep();
          avatar.PostStep();
        };
        for (int i = 0; i < 20; i++) avTick();

        // The body must TRACK the player: walk the player 12 voxels and the
        // avatar origin has to come along. A rig that ignored its driver
        // (the mob wander drive, say) would sit still and still look fine in
        // a screenshot.
        Vec3 originBefore = avatar.Origin();
        // Footfall accounting over the walk. These are what drive footstep
        // AUDIO, but the assertion is deliberately about the EVENTS, not the
        // sound: the selftest is headless and opens no audio device, so this
        // checks the half that can actually break silently — that the gait
        // emits one event per plant, on a real material, at the foot.
        int footfalls = 0;
        int footfallsBadMat = 0;
        int footfallsFarFromFoot = 0;
        // +Z is FORWARD at heading 0 — walk the way the body faces. Driving
        // +X here walked the avatar sideways, which is not a gait the game
        // can ever show and is not what the pose assertions below describe.
        // The avatar reads the player's OWN velocity now rather than
        // differencing its origin (see UpdateAnimation), so a test that
        // teleports pl.pos has to state the velocity that teleport
        // represents — otherwise the rig is told it is standing still while
        // being dragged forward, and every speed-gated system (gait,
        // walk/run clips, springs) sits at zero. Stating it is also the more
        // honest fixture: this loop is simulating a player walking, and a
        // walking player has a velocity.
        const float kWalkStepZ = 0.2f;
        pl.vel = Vec3{0, 0, kWalkStepZ / kTickDt};
        for (int i = 0; i < 60; i++) {
          pl.pos.z += kWalkStepZ;
          avTick();
          for (const PlayerAvatar::Footfall& ff : avatar.Footfalls()) {
            footfalls++;
            // A step must name a real, non-air material, or it is silent.
            if (ff.mat == 0 || ff.mat >= mats.size()) footfallsBadMat++;
            // ...and must land at the body, not at the world origin: a step
            // heard 100 m away from the player is the failure mode a
            // coordinate-conversion bug produces.
            if (Vec3{ff.posVox.x - pl.pos.x, 0, ff.posVox.z - pl.pos.z}.len() > 8.0f)
              footfallsFarFromFoot++;
          }
          avatar.ClearFootfalls();
        }
        float followed = avatar.Origin().z - originBefore.z;
        bool follows = followed > 10.0f;
        // 60 ticks of walking is 2 s; any sane gait plants several times.
        bool stepsOk = footfalls >= 2 && footfallsBadMat == 0 &&
                       footfallsFarFromFoot == 0;

        // The body must stay AT the player, not drift vertically away from
        // it. The gait used to re-derive its own standing height from the
        // foot plane, and because the foot goal falls back to that same
        // height when a ground probe misses, it fed itself and the avatar
        // climbed ~9.5 voxels a tick — "the wizard is 100 feet above the
        // player". A drift assertion is the cheap guard: any feedback path
        // that returns shows up here as an unbounded number, whatever its
        // cause. Tolerance is one body height, which covers the legitimate
        // gap between the AABB sole and an animated pose.
        float soleY = pl.pos.y - Player::kHalfY;
        float drift = std::abs(avatar.BodyY() - soleY);
        bool tracksY = drift < wd.worldSize.y;

        // YOUR OWN BODY MUST NOT PUSH YOU. The avatar's limbs are drawn
        // around the player capsule, so on the normal dynamic layer they sit
        // permanently interpenetrated with the proxy and PlayerPushOut reads
        // a large ejection vector whose direction swings with the gait —
        // walking forward drifted backwards and diagonally. The limbs live
        // on Layers::AVATAR now, which PlayerPushOut does not see. Sampled
        // over several ticks because the failure was ANIMATED, not static:
        // one sample could land on a frame where the swing happened to
        // cancel.
        uint64_t avProxy = phys.CreatePlayerBody(Player::kHalfXZ,
                                                 Player::kHalfY);
        float selfPush = 0.0f;
        for (int i = 0; i < 30; i++) {
          pl.pos.x += 0.2f;
          phys.MovePlayerBody(avProxy, pl.pos, kTickDt);
          avTick();
          selfPush = std::max(selfPush,
                              phys.PlayerPushOut(avProxy, pl.pos).len());
        }
        phys.RemoveBody(avProxy);
        bool noSelfPush = selfPush < 0.001f;

        // ---- head look (PlayerAvatar::SetLook) ----
        // The head must turn RELATIVE TO THE BODY. Both halves of that matter
        // and the first version of this feature got the second one wrong: it
        // twisted every part tagged "spine", and on mina that tag is on `hips`
        // — the ROOT limb — so the whole rig yawed together. The head moved in
        // world space, the body moved with it, and on screen nothing turned at
        // all. Measuring the head against the HIPS rather than against the
        // world is what makes that failure visible here.
        //
        // Body heading is held at 0 throughout (avTick passes it), so any
        // head-vs-hips angle is the look and nothing else.
        auto yawOf = [&](int part) {
          Vec3 p;
          Quat q;
          if (!avatar.PartWorldTransform(part, p, q)) return 0.0f;
          // Bearing in the rig's own HEADING convention — forward is
          // (sin h, ., cos h), i.e. atan2(x, z) — which is the convention
          // SetLook's argument is expressed in and the one the body applies
          // via AxisAngle({0,1,0}, heading_). Measuring in the same convention
          // the input uses is the whole point: the first version of this gate
          // measured correctly but asserted the OPPOSITE sign, so it passed
          // green while the head turned the wrong way on screen.
          Vec3 f = QuatRotate(q, Vec3{0, 0, 1});
          return std::atan2(f.x, f.z) * 57.29578f;
        };
        auto relHeadYaw = [&]() {
          float d = yawOf(avatar.Parts().head) - yawOf(avatar.Parts().hips);
          while (d > 180.0f) d -= 360.0f;
          while (d < -180.0f) d += 360.0f;
          return d;
        };
        pl.vel = Vec3{};
        // Settle at neutral so the measurement starts from a known zero
        // rather than from wherever the walk loop above left the neck.
        avatar.SetLook(0.0f, 0.0f);
        for (int i = 0; i < 40; i++) avTick();
        const float headRest = relHeadYaw();
        // +60 deg of heading delta — inside the 70 deg cone, so the head must
        // take all of it. THE HEAD MUST FOLLOW THE SIGN OF THE INPUT: a
        // positive look is a positive heading offset, the same direction the
        // body would have turned had it been asked, so the measured head
        // bearing must come out POSITIVE too. Asserting that is what catches
        // an inverted head, which is exactly the bug this gate first missed.
        avatar.SetLook(60.0f / 57.29578f, 0.0f);
        for (int i = 0; i < 40; i++) avTick();
        const float headPos = relHeadYaw() - headRest;
        avatar.SetLook(-60.0f / 57.29578f, 0.0f);
        for (int i = 0; i < 40; i++) avTick();
        const float headNeg = relHeadYaw() - headRest;
        // Sign, magnitude and symmetry. The head is asked for 60 deg and the
        // spine share (default 0.25) is applied at the TORSO, which the head
        // inherits — so head-vs-hips should recover very nearly the whole 60
        // either way. Generous bounds: this is gating "does it turn, the right
        // way, by roughly the right amount", not a tuning value.
        bool lookTurns = headPos > 25.0f && headPos < 95.0f &&
                         headNeg < -25.0f && headNeg > -95.0f;
        // A look must not drag the HIPS around: that is the bug above.
        avatar.SetLook(60.0f / 57.29578f, 0.0f);
        for (int i = 0; i < 40; i++) avTick();
        float hipsYaw = yawOf(avatar.Parts().hips);
        while (hipsYaw > 180.0f) hipsYaw -= 360.0f;
        while (hipsYaw < -180.0f) hipsYaw += 360.0f;
        bool hipsHeld = std::fabs(hipsYaw) < 12.0f;
        bool lookOk = lookTurns && hipsHeld;
        avatar.SetLook(0.0f, 0.0f);
        for (int i = 0; i < 40; i++) avTick();

        // ---- gait quality AT THE PLAYER'S REAL SPEED ----
        // Everything above walks the player at 0.2 vox/tick = 6 vox/sec,
        // which is a sixth of walkSpeed and a tenth of sprintSpeed. That is
        // why "the legs flail behind like a naruto run" sailed through a
        // green selftest: the failure only exists at speeds the test never
        // reached. Drive the real numbers and assert on the POSE.
        //
        // The invariant is limb ELEVATION: a leg that is walking stays near
        // vertical (its two bones fold and swing about the hip), while a leg
        // whose IK target is out of reach straightens and rotates toward
        // horizontal to point at it. So "min elevation over a stride" is a
        // direct measure of the trailing-leg failure, with no reference pose
        // to keep in sync.
        // Elevation of a limb's own axis above horizontal, 90 = hanging
        // straight down/up, 0 = sticking straight out. Limbs point along
        // their local +Y, and a limb rotates AWAY from vertical as it swings,
        // so this is the natural "is it swinging or is it pointing" measure.
        auto elevationOf = [&](int part) {
          Vec3 p;
          Quat q;
          if (!avatar.PartWorldTransform(part, p, q)) return 90.0f;
          Vec3 axis = QuatRotate(q, Vec3{0, 1, 0});
          float a = std::asin(std::clamp(axis.y, -1.0f, 1.0f));
          return std::fabs(a) * 57.29578f;
        };
        // SIGNED fore/aft swing of a limb, in degrees: how far its axis has
        // rotated out of vertical along the travel direction. The unsigned
        // elevation above folds a forward swing onto a backward one, so an
        // arm swinging +-14 degrees and an arm frozen at 0 both read ~90 and
        // the test could not tell "swinging" from "held out".
        // MEASURE IN THE PLANE THE LIMB ACTUALLY SWINGS IN.
        //
        // This test walks the avatar at heading 0, and heading 0 faces +Z
        // (fwd = {sin(h), 0, cos(h)}). Limb swing is authored as a rotation
        // about local X, which tilts a downward-hanging limb into Z — so the
        // fore/aft component of the swing is Z, and the X component stays
        // ~0 no matter how hard the limb swings.
        //
        // The previous version of this test measured atan2(axis.x, ...) while
        // ALSO driving the player along +X, i.e. it walked the character
        // SIDEWAYS (moving +X while facing +Z) and then read the one axis the
        // swing never reaches. Both halves were wrong, and together they made
        // every pose number it printed meaningless: a full-amplitude 14-degree
        // arm swing reported as ~2 degrees, so "arms swing" passed on an
        // 8-degree threshold that the authored 28-degree motion should have
        // cleared by 3x. Walk the way the body faces and measure the plane the
        // limb moves in, or this test cannot see the thing it exists to catch.
        auto swingOf = [&](int part) {
          Vec3 p;
          Quat q;
          if (!avatar.PartWorldTransform(part, p, q)) return 0.0f;
          Vec3 axis = QuatRotate(q, Vec3{0, 1, 0});
          // Fold onto the DOWNWARD hemisphere first: a limb model may point
          // either way along its local +Y, and atan2 against the wrong one
          // wraps to +-180 and makes a 14-degree swing look like 360.
          if (axis.y > 0) axis = axis * -1.0f;
          return std::atan2(axis.z, -axis.y) * 57.29578f;
        };
        // SIGNED LATERAL splay: how far the limb leans out of vertical
        // SIDEWAYS, in the plane the swing never uses. Walking at heading 0,
        // a healthy leg's whole motion is in Z (see swingOf), so this stays
        // near 0 for the entire stride and any persistent offset is a bug.
        //
        // This is the axis the fore/aft and elevation measures above are both
        // blind to, and the one that caught the IK frame mismatch: the target
        // was rebased by -rootAnchor while the hip it solves against was not,
        // so the solver saw a target ~2x out of reach, clamped to its annulus,
        // and pinned BOTH legs at a fixed ~10 degree lean toward the
        // character's left. The elevation stayed high, the legs still
        // alternated fore and aft, and every assertion below passed — the
        // pose was simply leaning the whole time. Measure the third axis or
        // this class of failure is invisible.
        auto lateralOf = [&](int part) {
          Vec3 p;
          Quat q;
          if (!avatar.PartWorldTransform(part, p, q)) return 0.0f;
          Vec3 axis = QuatRotate(q, Vec3{0, 1, 0});
          if (axis.y > 0) axis = axis * -1.0f;   // fold down, as swingOf does
          return std::atan2(axis.x, -axis.y) * 57.29578f;
        };
        // JOINT ANGLES IN THE RIG'S OWN FRAME, which is what a `poseLimit` is
        // stated in — the world-space measures above cannot see a range-of-
        // motion violation at all, because a leg swung 120 degrees past its
        // hip still reads as a perfectly ordinary elevation once the body has
        // turned under it.
        //
        // Signed rotation of a part about MODEL X relative to its parent, in
        // degrees, POSITIVE = FORWARD. On these rigs a positive rotation about
        // +X swings a hanging limb backward (verified against swingOf's own
        // convention above, where negative reads "behind"), so the sign is
        // flipped here to match how a human would describe a hip.
        // Reported as the twist about model +X, in degrees, which is EXACTLY
        // the quantity AnimClampPoseLimits clamps — so a number here compares
        // directly against the authored min/max instead of being a second,
        // differently-defined angle that has to be reconciled by hand. Recall
        // that +X swings a hanging limb BACKWARD on these rigs, hence:
        //   hip  authored {axis:[-1,0,0], min:-10, max:80}  =>  x in [-80, +10]
        //   knee authored {axis:[ 1,0,0], min:  0, max:90}  =>  x in [  0, +90]
        auto jointTwistX = [&](int part, int parent) {
          Vec3 p, pp;
          Quat q, pq;
          if (part < 0 || parent < 0) return 0.0f;
          if (!avatar.PartModelTransform(part, p, q)) return 0.0f;
          if (!avatar.PartModelTransform(parent, pp, pq)) return 0.0f;
          const Quat rel = QuatMul(QuatConj(pq), q);
          float a = 2.0f * std::atan2(rel.x, rel.w) * 57.29578f;
          while (a > 180.0f) a -= 360.0f;
          while (a <= -180.0f) a += 360.0f;
          return a;
        };
        const int legParts[4] = {avatar.PartIndex("legU.L"),
                                 avatar.PartIndex("legU.R"),
                                 avatar.PartIndex("legL.L"),
                                 avatar.PartIndex("legL.R")};
        const int armParts[2] = {avatar.PartIndex("armU.L"),
                                 avatar.PartIndex("armU.R")};
        float minLegElev = 90.0f, minArmElev = 999.0f, maxArmElev = -999.0f;
        // Signed fore/aft extremes of the LEGS, tracked per leg. A leg that
        // only ever reads negative is a leg that is always behind the body —
        // the "legs are just behind the whole time" failure — and no unsigned
        // measure can tell that apart from a healthy stride.
        float minLegSwing[2] = {999.0f, 999.0f};
        float maxLegSwing[2] = {-999.0f, -999.0f};
        // Worst lateral lean seen on any leg part, and the mean lean per leg
        // (a CONSTANT splay averages to itself, while a healthy leg's small
        // symmetric wobble averages to ~0).
        float maxLegLateral = 0.0f;
        float sumLegLateral[2] = {0.0f, 0.0f};
        int nLegLateral = 0;
        // ---- joint ranges and knee bend, in the rig's own frame ----
        // hipX is the direct test for the POSE LIMITS; kneeX is also the direct
        // test for the ANKLE-FRAME foot target, because a leg whose IK target
        // is past full extension is CLAMPED by AnimSolveTwoBone every tick and
        // a clamped two-bone solve has a dead-straight knee by construction.
        // Before that fix this rig walked at 0.0 degrees of knee bend.
        float hipXLo = 999.0f, hipXHi = -999.0f;
        float kneeXLo = 999.0f, kneeXHi = -999.0f;
        // ---- stride coherence ----
        // Footfalls against the gait phase's own cycles over the same window.
        // A biped stride is two footfalls, so a coherent clock completes
        // footfalls/2 cycles. The free-running oscillator this replaced ran at
        // ~2.6x the footfall rate, which no pose measure can see and which IS
        // the "sways left and right really fast and it's jittery" report.
        int walkFootfalls = 0, phaseCycles = 0;
        float prevPhase = avatar.GaitPhase();
        // The crouch is what BUYS the stride its reach, so it has to be
        // sampled while WALKING. Reading it at the end of the gate reports the
        // parked standing value and says nothing about whether the leg had
        // room to swing.
        float maxCrouch = 0.0f;
        const float walkStep =
            (CurrentTuning().player.walkSpeed / kVoxelMeters) * kTickDt;
        // State the velocity this teleport represents — the avatar reads the
        // player's own vel now, not a position difference. See the note at
        // the first walk loop above.
        pl.vel = Vec3{0, 0, walkStep / kTickDt};
        // KEEP THE BODY ON THE GROUND IT IS WALKING OVER.
        //
        // This loop advances the player by ASSIGNING pl.pos.z — a deliberate
        // teleport, so the gait is measured without the controller in the way
        // (see the velocity note above). But it only ever moved z, and the
        // terrain under it is real worldgen that rises and falls, so after a
        // few dozen voxels the body was walking through the air well above the
        // surface (or buried in it). The gait's ground probe correctly reported
        // the real ground far below, the IK target went out of reach, and
        // avatar.cpp's stale-plant guard — `rel.len() > legLength * 1.6` —
        // dropped the solve entirely. The legs then sat at EXACT rest: the
        // trace reads `legU -0.0 / -0.0, shin 90.0 / 90.0` for the last dozen
        // ticks of the window, which is not a gait failing, it is a gait that
        // was never asked to run.
        //
        // Every pose extreme this loop reports was measured through that, so
        // the numbers were a fixture artifact. Snap y to the real surface each
        // tick and the body walks on the ground the probe can see.
        const std::vector<uint32_t> walkClassOf = BuildCollisionClasses(mats);
        // WALK ON GROUND THIS TEST OWNS, not on whatever worldgen happens to
        // be here. Every loop above teleports the player forward, so by now the
        // body is ~60 voxels from where it spawned and standing over terrain
        // nobody chose — measured, a hillside climbing most of a voxel per tick.
        // That is not "flat ground at the player's real speed", it is an
        // uncontrolled incline, and every pose extreme this loop reports was
        // measured on it. Stamp a flat stone shelf and walk that: the ramp
        // fixture further down is where sloped ground is deliberately tested,
        // and keeping the two separate is what lets either failure name itself.
        {
          const int px = ifloor(pl.pos.x), pz = ifloor(pl.pos.z) + 2;
          const int py = ifloor(pl.pos.y - Player::kHalfY);
          for (int seg = 0; seg < 12; seg++) {
            std::vector<BrushOp> ops;
            std::vector<ParticleSpawn> spawns;
            std::vector<CellOp> cellOps;
            for (int k = 0; k < 6; k++) {
              const int zz = pz + seg * 6 + k;
              for (int dx = -4; dx <= 4; dx += 4)
                ops.push_back(BrushOp{px + dx, py - 3, zz, 2, 1u, 1});
            }
            ++t;
            SubmitTick(ctx, world, sim, t, kDefaultSeed, ops, {}, cellOps,
                       false,
                       IVec3{px >> 4, py >> 4, (pz + seg * 6) >> 4}, true,
                       false, spawns);
            ctx.WaitIdle();
            ctx.ProcessEvents();
          }
          for (int i = 0; i < 6; i++) avTick();
        }
        // THE PROBE MUST NOT START FROM THE ANSWER IT LAST GAVE. Scanning down
        // from `pl.pos.y` — which this same lambda wrote on the previous tick —
        // is a feedback loop: half of a 17-voxel body is 8.5 voxels, so once
        // the scan starts inside a hill it finds the hill's INTERIOR, the body
        // rises by that, the next scan starts higher still, and the sole
        // climbed 13 voxels a tick. (Measured: soleY 213 -> 226 -> 239 -> 252.)
        // Same shape as the avatar's own gait-height bug, one layer out.
        //
        // Anchored to the PREVIOUS SOLE plus ONE voxel instead. This loop is
        // the FLAT case and walks a shelf this test stamped, so a surface more
        // than a voxel up is the hillside the shelf is cut into, not ground
        // this fixture means to walk.
        int climbWindow = 1;
        auto surfaceY = [&](float wx, float wz, float prevSole) {
          const int x = ifloor(wx), z = ifloor(wz);
          const int from = ifloor(prevSole) + climbWindow;
          for (int y = from; y > from - 40; y--)
            if (world.KindAt(IVec3{x, y, z}, walkClassOf) == CellKind::Solid)
              return (float)(y + 1);
          return prevSole;
        };
        // The head-look loops above ran ~200 ticks without draining, so start
        // the footfall count from empty or the first sample carries all of it.
        avatar.ClearFootfalls();
        prevPhase = avatar.GaitPhase();
        for (int i = 0; i < 90; i++) {
          pl.pos.z += walkStep;   // forward at heading 0, see swingOf above
          pl.pos.y = surfaceY(pl.pos.x, pl.pos.z,
                              pl.pos.y - Player::kHalfY) + Player::kHalfY;
          avTick();
          // Sampled OUTSIDE the steady-state cutoff below, because both of
          // these are RATES: a footfall counted while the phase count is not
          // would make the ratio wrong by exactly the transient.
          {
            const float ph = avatar.GaitPhase();
            if (ph < prevPhase - 0.25f) phaseCycles++;   // wrapped past 1
            prevPhase = ph;
            walkFootfalls += (int)avatar.Footfalls().size();
          }
          avatar.ClearFootfalls();
          // Let the gait reach STEADY STATE before sampling. From a standing
          // start both feet are planted under the body and the first strides
          // are catching up, so the legs legitimately pass through low
          // elevations for a few ticks. 20 ticks was not enough once the
          // stride budget raised the cadence; 30 was not enough once the
          // rig's handedness was corrected, which shifts the gait PHASE by a
          // couple of ticks and so slides the tail of that same transient
          // past the old cutoff. 36 clears it with real margin.
          //
          // NOTE THIS MEASURES ALL FOUR LEG PARTS, thighs AND shins, but the
          // GAITDBG line below only prints the two thighs. The sample that
          // failed at 30 was a SHIN (17.3 deg at t34) while every thigh was
          // >= 21.2 — so a summary `legElev>=17` that no traced column ever
          // shows is not a contradiction, it is the shin. From t36 the shin
          // minimum is 25.8 and the thigh minimum 29.7.
          //
          // This cutoff bounds the TRANSIENT, not the gait: steady state is
          // symmetric either way (legU swings L -37.9..20.5 vs R
          // -37.1..20.7, both signs on both legs). Do NOT raise it further
          // to paper over a leg that is genuinely lying down — read the
          // SANDVOX_GAITDBG trace and confirm the dip is a single tick at
          // the edge of the window first.
          maxCrouch = std::max(maxCrouch, avatar.StanceCrouch());
          if (i < 36) continue;
          for (int lp : legParts)
            if (lp >= 0) minLegElev = std::min(minLegElev, elevationOf(lp));
          for (int s = 0; s < 2; s++)
            if (legParts[s] >= 0) {
              float e = swingOf(legParts[s]);
              minLegSwing[s] = std::min(minLegSwing[s], e);
              maxLegSwing[s] = std::max(maxLegSwing[s], e);
              sumLegLateral[s] += lateralOf(legParts[s]);
            }
          nLegLateral++;
          for (int lp : legParts)
            if (lp >= 0)
              maxLegLateral =
                  std::max(maxLegLateral, std::fabs(lateralOf(lp)));
          for (int ap : armParts)
            if (ap >= 0) {
              float e = swingOf(ap);
              minArmElev = std::min(minArmElev, e);
              maxArmElev = std::max(maxArmElev, e);
            }
          {
            const int hips = avatar.PartIndex("hips");
            for (int s = 0; s < 2; s++) {
              const float hx = jointTwistX(legParts[s], hips);
              hipXLo = std::min(hipXLo, hx);
              hipXHi = std::max(hipXHi, hx);
              const float kx = jointTwistX(legParts[2 + s], legParts[s]);
              kneeXLo = std::min(kneeXLo, kx);
              kneeXHi = std::max(kneeXHi, kx);
            }
          }
          // Per-tick gait trace. The summary line only reports extremes, and
          // a gait fails in ways an extreme cannot show — both legs stuck in
          // phase, an arm swinging at a quarter amplitude, a leg that never
          // comes in FRONT of the body. Those are all obvious in a tick-by-
          // tick column and invisible in a min/max. Off by default; the
          // assertions below are what gate the build.
          //   SANDVOX_GAITDBG=1 ./sandvox.exe --selftest
          // `swing` is signed fore/aft: negative = behind the body, positive
          // = in front, so a healthy walk shows BOTH signs on every limb.
          if (getenv("SANDVOX_GAITDBG")) {
            std::printf(
                "  t%02d spd=%4.1f arm %6.1f/%6.1f  legU %6.1f/%6.1f  "
                "legElev %5.1f/%5.1f shin %5.1f/%5.1f  lat %6.1f/%6.1f\n",
                i, avatar.SpeedNow(), swingOf(armParts[0]),
                swingOf(armParts[1]), swingOf(legParts[0]),
                swingOf(legParts[1]), elevationOf(legParts[0]),
                elevationOf(legParts[1]), elevationOf(legParts[2]),
                elevationOf(legParts[3]), lateralOf(legParts[0]),
                lateralOf(legParts[1]));
            std::printf(
                "        crouch %.2f hipX %6.1f/%6.1f kneeX %6.1f/%6.1f "
                "bodyY %.2f soleY %.2f\n",
                avatar.StanceCrouch(),
                jointTwistX(legParts[0], avatar.PartIndex("hips")),
                jointTwistX(legParts[1], avatar.PartIndex("hips")),
                jointTwistX(legParts[2], legParts[0]),
                jointTwistX(legParts[3], legParts[1]),
                avatar.BodyY(), pl.pos.y - Player::kHalfY);
          }
        }
        // A walking leg should never lie down. A healthy stride bottoms out
        // around 23 degrees at the extremes of the swing and spends most of
        // the cycle well above it; the trailing-leg failure drove it into
        // single digits, so the gap is wide.
        bool legsUpright = minLegElev > 18.0f;

        // THE LEGS MUST ALTERNATE FORE AND AFT, not just move.
        //
        // This is the assertion the old test was missing entirely, and it is
        // the one that matches the actual complaint: "when running the legs
        // are always behind the character instead of alternating in front of
        // and then behind". A leg driven by a negative stride budget still
        // SWINGS — it cycles between "far behind" and "slightly less far
        // behind" — so every range- or amplitude-based check passes while the
        // character rakes its legs out behind it. Only the SIGN catches it.
        // Require each leg to spend part of the cycle genuinely in front of
        // the hip, with a few degrees of margin so it cannot pass on noise.
        bool legsAlternate = true;
        for (int s = 0; s < 2; s++)
          legsAlternate = legsAlternate && maxLegSwing[s] > 5.0f &&
                          minLegSwing[s] < -5.0f;

        // THE LEGS MUST NOT LEAN SIDEWAYS. Walking at heading 0 the entire
        // stride lives in Z, so any sustained X lean is spurious — see the
        // note at lateralOf. Two separate conditions because they fail
        // differently: a per-sample bound catches a big transient splay, and
        // the per-leg MEAN catches a small constant one that a bound on the
        // extreme would let through. 12 degrees is comfortably under the ~10
        // the frame mismatch produced at walk pace (it grows with speed) while
        // leaving room for the honest couple of degrees a bent knee shows.
        float meanLegLateral[2] = {0.0f, 0.0f};
        if (nLegLateral > 0)
          for (int s = 0; s < 2; s++)
            meanLegLateral[s] = sumLegLateral[s] / (float)nLegLateral;
        bool legsNotSplayed = maxLegLateral < 12.0f &&
                              std::fabs(meanLegLateral[0]) < 6.0f &&
                              std::fabs(meanLegLateral[1]) < 6.0f;

        // Arms must SWING and must stay roughly under the shoulder. In the
        // signed measure, 0 is hanging straight down and +-90 is held
        // straight out. A zombie arm pins near one extreme and never moves;
        // a walking arm oscillates about 0.
        //
        // The threshold is 20 degrees against an authored +-14 (28 total).
        // The old 8 was below HALF the authored motion, which is how a walk
        // rendering at 8 degrees — visually a dead arm — passed this test for
        // as long as it did. A threshold that a correct implementation clears
        // by only a hair is not a test. Set it close under the authored value
        // so any real suppression of the swing fails immediately.
        bool armsSwing = (maxArmElev - minArmElev) > 20.0f;
        bool armsHang = std::fabs(maxArmElev) < 60.0f &&
                        std::fabs(minArmElev) < 60.0f;

        // THE KNEE MUST ACTUALLY BEND. This is the direct regression test for
        // the foot IK target being in the ANKLE's frame.
        //
        // GroundHeightAt returns the SURFACE, which is where the model's MIN
        // CORNER rests; the chain's effector is the ankle JOINT, which on this
        // rig sits 0.75 world voxels above that corner. Handing the solver the
        // surface therefore asks for 7.50 voxels of reach out of a 6.79-voxel
        // leg, and AnimSolveTwoBone clamps to its reach annulus on every single
        // tick — a clamped two-bone solve is a DEAD STRAIGHT limb by
        // construction, so the knee measured 0.0 degrees for the whole of every
        // walk and every subtlety the gait computed upstream was thrown away at
        // that one line. It is the root of "the legs don't swing enough", and
        // no pose measure that existed here could see it: a straight leg
        // rotating about the hip still elevates, still alternates, still stays
        // out of the splay bound.
        //
        // The bend also needs somewhere to come FROM, which is the stance
        // crouch: with the pelvis pinned at the AABB sole this rig's reachable
        // stride was 0.7 voxels, so the fix is only half a fix without it.
        const float kneeFlex = std::max(kneeXHi, 0.0f);
        bool kneeBends = kneeFlex > 8.0f;

        // ...AND THE JOINTS MUST STAY INSIDE THE AUTHORED RANGE OF MOTION.
        // The ragdoll limits in the sidecar cannot do this: Jolt only enforces
        // them on a dynamic body and a live limb is kinematic, so until
        // AnimClampPoseLimits there was nothing at all between the IK and an
        // anatomically impossible leg. Measured as the twist about model +X,
        // the same quantity the clamp works in (see jointTwistX).
        //
        // The bounds are READ OFF THE RIG, never restated here. A limit is
        // authored data (assets/mobs/human.json `poseLimit`), and a test that
        // hardcoded the same numbers would be a second copy to keep in sync —
        // it would also pass while measuring nothing if the JSON changed. The
        // authored axis carries the SIGN convention, so it has to come along:
        // the hip is authored about -X (positive = forward, as a human would
        // say it) and the knee about +X.
        const float kSlack = 1.5f;
        auto limitOf = [&](const char* part, float& lo, float& hi) {
          lo = -180.0f;
          hi = 180.0f;
          const int i = avatar.PartIndex(part);
          if (i < 0 || i >= avatar.LimbCount()) return false;
          const MobLimbDef& ld = avatar.LimbDefAt(i);
          if (!ld.hasPoseLimit) return false;
          const float deg = 57.29578f;
          // Express the authored range in the +X frame jointTwistX reports in.
          if (ld.poseAxis.x < 0) {
            lo = -ld.poseMax * deg;
            hi = -ld.poseMin * deg;
          } else {
            lo = ld.poseMin * deg;
            hi = ld.poseMax * deg;
          }
          return true;
        };
        float hipLimLo = 0, hipLimHi = 0, kneeLimLo = 0, kneeLimHi = 0;
        const bool haveLimits = limitOf("legU.L", hipLimLo, hipLimHi) &&
                                limitOf("legL.L", kneeLimLo, kneeLimHi);
        bool jointsInRange =
            haveLimits && hipXLo > hipLimLo - kSlack &&
            hipXHi < hipLimHi + kSlack && kneeXLo > kneeLimLo - kSlack &&
            kneeXHi < kneeLimHi + kSlack;

        // ONE CLOCK. A biped stride is two footfalls, so the gait phase must
        // complete footfalls/2 cycles over the same window. The oscillator this
        // replaced ran at cadence * speedFactor — 2.6x the real footfall rate
        // on this rig at walk pace, and at 8.13 Hz against a 30 Hz tick, under
        // four samples per cycle. That is the jitter, and it is invisible to
        // every other assertion on this page because it is a property of the
        // RATE rather than of any pose.
        const float wantCycles = (float)walkFootfalls * 0.5f;
        bool strideCoherent =
            walkFootfalls >= 4 &&
            std::fabs((float)phaseCycles - wantCycles) <= wantCycles * 0.5f + 1.0f;

        // ---- FALLING: the legs must not invert into the body ----
        //
        // Completely uncovered before, and the failure was spectacular: the
        // gait ran while airborne, its ground probe (which only ever scans
        // DOWNWARD) missed every tick, and `planted` — a WORLD-space point —
        // stayed where the floor used to be while the body fell away from it.
        // Within a few ticks the IK target sat ABOVE the hip and the solver
        // dutifully aimed the legs up at it, folding them through the pelvis
        // and inside the torso and head.
        //
        // Assert on the MODEL-space pose, which is what "the legs inverted"
        // actually means: the rig's own idea of where the limbs are. Reading
        // the Jolt transforms back instead would measure body CENTRES that
        // the solver is still chasing through a teleporting fall, and they
        // collapse toward each other for reasons that have nothing to do with
        // the pose.
        //
        // The invariant is that the leg keeps HANGING: the hip-to-foot vector
        // must stay pointed downward-ish in the body frame. An inverted leg
        // flips that vector to point up. Measuring the vector rather than an
        // angle means a tucked knee (jump) and a straight leg (fall) are both
        // fine, and only a genuine fold-through fails.
        float worstLegUp = -1e9f;
        {
          pl.grounded = false;
          // Straight down, so drop the forward velocity the walk loops left
          // set. Only the planar part feeds the gait and the gait is off in
          // the air anyway, but a "falling" fixture that still claims to be
          // running forward is a trap for the next person to read it.
          pl.vel.x = 0;
          pl.vel.z = 0;
          const int hipParts[2] = {avatar.PartIndex("legU.L"),
                                   avatar.PartIndex("legU.R")};
          const int feet[2] = {avatar.PartIndex("foot.L"),
                               avatar.PartIndex("foot.R")};
          for (int i = 0; i < 45; i++) {
            // Fall: the player leaves the ground and keeps dropping, which is
            // what starves the downward-only ground probe and, before the
            // fix, left the IK chasing a foot plant the body had left behind.
            pl.pos.y -= 0.6f;
            pl.vel.y = -18.0f;
            avTick();
            for (int s = 0; s < 2; s++) {
              Vec3 hp, fp;
              Quat hq, fq;
              if (hipParts[s] < 0 || feet[s] < 0) continue;
              if (!avatar.PartModelTransform(hipParts[s], hp, hq)) continue;
              if (!avatar.PartModelTransform(feet[s], fp, fq)) continue;
              // Model space is Y-up, so a hanging leg has foot.y < hip.y.
              // Positive = the foot has risen above its own hip: inverted.
              worstLegUp = std::max(worstLegUp, fp.y - hp.y);
            }
            if (getenv("SANDVOX_GAITDBG")) {
              Vec3 hp, fp;
              Quat hq, fq;
              avatar.PartModelTransform(hipParts[0], hp, hq);
              avatar.PartModelTransform(feet[0], fp, fq);
              std::printf("  fall t%02d clips=%d hipY=%.2f footY=%.2f %+.2f\n",
                          i, avatar.ActiveClips(), hp.y, fp.y, fp.y - hp.y);
            }
          }
          pl.grounded = true;
        }
        // A hanging leg is about -4.5 here (hip to foot down the leg). Allow
        // plenty of slack for a jump tuck; only a real fold-through goes
        // positive.
        bool legsNotInverted = worstLegUp < -0.5f;

        // ---- BUMPY INCLINE: the pose must not TELEPORT between frames ----
        //
        // The reported bug: walking up a noisy slope, the arms snap straight
        // out and the character "tweaks out". The cause is that `grounded`
        // is genuinely ragged there — the body leaves the surface for a
        // fraction of a voxel cresting each bump — and the leg IK used to be
        // gated on it as a HARD BOOL. Every flicker switched the IK fully on
        // or fully off, snapping the limbs between the IK pose and the rest
        // hang. Clips already crossfade and the dismember states only move on
        // a sever, so this gate was the last thing in the pose pipeline still
        // teleporting.
        //
        // Assert on POSE CONTINUITY, which is what the complaint actually is:
        // the per-tick change in each limb's model-space orientation. A limb
        // that eases has a bounded delta; one that snaps between two poses
        // shows a large spike. Measuring the delta rather than the pose is
        // what makes this catch a discontinuity without pinning down what the
        // correct pose looks like.
        float worstJump = 0.0f, worstJumpFlat = 0.0f;
        for (int pass = 0; pass < 2; pass++) {
          const bool kBumpyRagged = (pass == 1);
          pl.grounded = true;
          const int watch[4] = {avatar.PartIndex("armU.L"),
                                avatar.PartIndex("armU.R"),
                                avatar.PartIndex("legU.L"),
                                avatar.PartIndex("legU.R")};
          Quat prev[4];
          bool havePrev = false;
          const float step =
              (CurrentTuning().player.walkSpeed / kVoxelMeters) * kTickDt;
          pl.vel = Vec3{0, 0, step / kTickDt};
          for (int i = 0; i < 80; i++) {
            // Walk forward on the FLAT, with ragged contact.
            //
            // THE BODY AND THE TERRAIN MUST AGREE. An earlier version of this
            // fixture raised pl.pos.y every tick to fake an incline — but the
            // world under it stayed flat, so the avatar was really floating
            // upward over level ground while its ground probe kept correctly
            // reporting the ground it was leaving. The feet then stretched
            // further down every tick chasing a receding target (measured:
            // 0.5-1.0 voxels of foot travel per tick, mid-swing, always
            // downward), and the gate failed on that fixture artifact rather
            // than on any bug in the rig. Building a real ramp and letting
            // the controller walk it would be the other way to do this;
            // holding y flat is the cheap version and isolates the thing
            // under test, which is what the RAGGED CONTACT does to the pose.
            pl.pos.z += step;
            // Ragged contact, the way real bumpy ground reports it: mostly
            // grounded, dropping out for a single tick now and then. This is
            // the actual input that used to make the pose snap — it drove the
            // air-state clips and the IK gate, both of which were hard
            // switches on this bit.
            pl.grounded = kBumpyRagged ? ((i % 7) != 0) : true;
            avTick();
            if (i < 20) continue;   // let the gait reach steady state
            Quat cur[4];
            bool ok = true;
            for (int k = 0; k < 4; k++) {
              Vec3 p;
              if (watch[k] < 0 || !avatar.PartModelTransform(watch[k], p, cur[k]))
                ok = false;
            }
            if (!ok) continue;
            if (havePrev)
              for (int k = 0; k < 4; k++) {
                // Angle between successive orientations, in degrees. Quats
                // double-cover, so take the absolute dot: q and -q are the
                // same rotation and a sign flip would read as a 180 jump.
                float d = std::fabs(prev[k].x * cur[k].x + prev[k].y * cur[k].y +
                                    prev[k].z * cur[k].z + prev[k].w * cur[k].w);
                d = std::clamp(d, 0.0f, 1.0f);
                float deg = 2.0f * std::acos(d) * 57.29578f;
                if (kBumpyRagged) worstJump = std::max(worstJump, deg);
                else worstJumpFlat = std::max(worstJumpFlat, deg);
                if (getenv("SANDVOX_GAITDBG") && deg > 10.0f)
                  std::printf(
                      "  posejump t%02d part%d %.1f deg (grounded=%d clips=%d"
                      " %s)\n",
                      i, k, deg, pl.grounded ? 1 : 0, avatar.ActiveClips(),
                      avatar.ActiveClips() > 0 ? avatar.ActiveClipName(0)
                                               : "-");
              }
            for (int k = 0; k < 4; k++) prev[k] = cur[k];
            havePrev = true;
          }
          pl.grounded = true;
        }
        // A walking limb moves a few degrees per tick at 30 Hz. The hard
        // switch produced snaps far above that, so the threshold sits well
        // clear of honest motion while still catching a real teleport.
        // ASSERT ON THE RAGGEDNESS PENALTY, NOT AN ABSOLUTE ANGLE.
        //
        // A leg in mid-swing legitimately rotates fast — the body covers
        // about a voxel per tick at walk pace and the foot has to keep up, so
        // a healthy stride shows tens of degrees per tick all by itself. An
        // absolute threshold cannot tell that apart from a snap, which is why
        // the first version of this gate failed on a perfectly good walk.
        //
        // The A/B is the honest test: walk the SAME 80 ticks twice, once
        // continuously grounded and once with `grounded` dropping out for a
        // tick now and then (bumpy ground). Both runs contain identical
        // stride motion, so whatever the flicker ADDS on top is the
        // discontinuity — and that is precisely what the hard switches used
        // to inject.
        bool poseContinuous = worstJump < worstJumpFlat * 1.6f + 6.0f;

        // ---- A REAL RAMP: sloped voxels, walked by the real controller ----
        //
        // The fixture above walks FLAT with a ragged `grounded` bit, and its
        // own comment says so: holding y flat isolates the raggedness, and
        // building a real ramp "would be the other way to do this". The
        // reported bug is about actual sloped ground ("on anything but flat
        // ground the legs stop moving rhythmically"), so build the ramp.
        //
        // Everything the flat fixture cannot reach lives here: the ground probe
        // returns a DIFFERENT height under each foot and a rising one every
        // tick, so the swing arc lands above where it lifted, the stance span
        // shortens and lengthens under the hip, and the step trigger fires on a
        // moving target. A gait that only works on a plane fails here and
        // nowhere else on this page.
        float rampKneeFlex = 0.0f;
        float rampHipLo = 999.0f, rampHipHi = -999.0f;
        int rampFootfalls = 0;
        int rampJumpTicks = 0, rampFallTicks = 0;
        float rampWorstJump = 0.0f;
        // How far the body actually got, so a fixture that never climbed
        // reports as a FIXTURE problem rather than as a silent "0 footfalls"
        // indistinguishable from a gait that stopped stepping.
        float rampClimb = 0.0f, rampAdvance = 0.0f, rampStartY = 0.0f,
              rampStartZ = 0.0f;
        {
          // The controller collides against the CPU mirror through the same
          // collision-class table the game builds; it is not the raw material
          // id, and vegetation reading as gas is the point of it.
          const std::vector<uint32_t> classOf = BuildCollisionClasses(mats);
          auto kindAt = [&](IVec3 c) { return world.KindAt(c, classOf); };

          // PUT THE BODY BACK ON REAL GROUND FIRST. Every loop above advances
          // the player by ASSIGNING pl.pos, which is a teleport with no
          // collision — so by here the body is at an arbitrary height over
          // whatever terrain happens to be under it. Handing that straight to
          // the real controller drops it into a genuine 60-tick fall, which
          // does not merely break this fixture: the avatar takes real fall
          // damage from it and every assertion AFTER this block (the sever
          // ladder, the debris count, the teardown) then runs on a corpse.
          const int rx = 200, rz = 200;
          const int rh = World::TerrainHeight(rx, rz, kDefaultSeed);
          pl.pos = Vec3{(float)rx + 0.5f, (float)(rh + 2) + Player::kHalfY,
                        (float)rz + 0.5f};
          pl.vel = Vec3{};
          pl.grounded = true;
          for (int i = 0; i < 20; i++) {
            PlayerInput idle{};
            pl.Update(kTickDt, idle, Vec3{0, 0, 1}, Vec3{1, 0, 0},
                      Vec3{0, 0, 1}, kindAt);
            avTick();
          }

          // Stone stairs climbing +Z from where the body settled: one voxel of
          // rise per two travelled, a ~26 degree grade. Steep enough to be a
          // hill, and inside the controller's step budget so the walk stays a
          // walk instead of degenerating into a series of hops. Laid through
          // the MutationQueue like every other world edit (rule 3); radius-2
          // spheres overlap into a continuous bank, centred three below the
          // surface they are meant to produce so the sphere's TOP lands there
          // rather than burying the walkable cell.
          const int x0 = (int)pl.pos.x, z0 = (int)pl.pos.z;
          const int y0 = (int)(pl.pos.y - Player::kHalfY);
          const uint32_t stone = 1;
          // Overlapping radius-3 spheres at EVERY z, starting at the body's own
          // cell so there is no seam between the real terrain and the ramp for
          // the sweep to catch on, and rising one voxel per three travelled
          // (~18 degrees). A coarser grade built from isolated 1-voxel risers
          // is a staircase, and the controller pays a step-up for each one.
          for (int seg = 0; seg < 13; seg++) {
            std::vector<BrushOp> ops;
            std::vector<ParticleSpawn> spawns;
            std::vector<CellOp> cellOps;
            for (int k = 0; k < 5; k++) {
              const int zz = z0 + seg * 5 + k;
              const int yy = y0 + (zz - z0) / 3;
              for (int dx = -4; dx <= 4; dx += 4)
                ops.push_back(BrushOp{x0 + dx, yy - 4, zz, 3, stone, 1});
            }
            ++t;
            SubmitTick(ctx, world, sim, t, kDefaultSeed, ops, {}, cellOps,
                       false, {rx / 16, rh / 16, rz / 16}, true, false, spawns);
            ctx.WaitIdle();
            ctx.ProcessEvents();
          }
          // Let the mirror catch up: the CPU chunk cache is one tick latent and
          // the gait's ground probe reads it, so walking before it arrives
          // measures the gait against terrain that is not there yet.
          for (int i = 0; i < 10; i++) {
            PlayerInput idle{};
            pl.Update(kTickDt, idle, Vec3{0, 0, 1}, Vec3{1, 0, 0},
                      Vec3{0, 0, 1}, kindAt);
            avTick();
          }

          const int watch[4] = {avatar.PartIndex("armU.L"),
                                avatar.PartIndex("armU.R"),
                                avatar.PartIndex("legU.L"),
                                avatar.PartIndex("legU.R")};
          Quat prev[4];
          bool havePrev = false;
          const int hips = avatar.PartIndex("hips");
          const float step =
              (CurrentTuning().player.walkSpeed / kVoxelMeters) * kTickDt;
          climbWindow = 2;   // the ramp climbs; the flat shelf does not
          rampStartY = pl.pos.y;
          rampStartZ = pl.pos.z;
          avatar.ClearFootfalls();
          for (int i = 0; i < 90; i++) {
            // Advanced the same way the flat loop is: teleport forward, then
            // read y back out of the REAL VOXELS the ramp is made of. The body
            // and the terrain therefore agree — which is the thing the flat
            // fixture's comment says matters — while the collision sweep stays
            // out of it. That is deliberate: the gait is what is under test
            // here, and driving Player::Update instead made this fixture
            // measure the CONTROLLER (it jammed against the stamped ramp at
            // t28 and the body sat still for 60 ticks, reporting a gait
            // failure that was nothing of the sort).
            pl.pos.z += step;
            pl.pos.y = surfaceY(pl.pos.x, pl.pos.z,
                                pl.pos.y - Player::kHalfY) + Player::kHalfY;
            pl.vel = Vec3{0, 0, step / kTickDt};
            pl.grounded = true;
            avTick();
            rampFootfalls += (int)avatar.Footfalls().size();
            avatar.ClearFootfalls();
            if (getenv("SANDVOX_GAITDBG"))
              std::printf("  ramp t%02d z=%.1f y=%.1f grounded=%d spd=%.1f\n",
                          i, pl.pos.z, pl.pos.y - Player::kHalfY,
                          pl.grounded ? 1 : 0, avatar.SpeedNow());
            if (i < 25) continue;   // let the climb reach steady state
            // NEITHER AIR CLIP MAY PLAY. Cresting each step legitimately
            // breaks contact for a tick, which used to fire the arms-up `jump`
            // one-shot; and a step DOWN clears any air debounce, which used to
            // start `fall`. Both are now gated on real events (a launch, and a
            // real drop below the last supported height), so walking a hill
            // must show neither.
            if (avatar.ClipActive("jump")) rampJumpTicks++;
            if (avatar.ClipWeight("fall") > 0.05f) rampFallTicks++;
            for (int s = 0; s < 2; s++) {
              rampHipLo = std::min(rampHipLo, jointTwistX(watch[2 + s], hips));
              rampHipHi = std::max(rampHipHi, jointTwistX(watch[2 + s], hips));
              rampKneeFlex = std::max(
                  rampKneeFlex,
                  jointTwistX(avatar.PartIndex(s ? "legL.R" : "legL.L"),
                              watch[2 + s]));
            }
            Quat cur[4];
            bool ok = true;
            for (int k = 0; k < 4; k++) {
              Vec3 p;
              if (watch[k] < 0 || !avatar.PartModelTransform(watch[k], p, cur[k]))
                ok = false;
            }
            if (!ok) continue;
            if (havePrev)
              for (int k = 0; k < 4; k++) {
                float d = std::fabs(prev[k].x * cur[k].x + prev[k].y * cur[k].y +
                                    prev[k].z * cur[k].z + prev[k].w * cur[k].w);
                d = std::clamp(d, 0.0f, 1.0f);
                rampWorstJump =
                    std::max(rampWorstJump, 2.0f * std::acos(d) * 57.29578f);
              }
            for (int k = 0; k < 4; k++) prev[k] = cur[k];
            havePrev = true;
          }
          rampClimb = pl.pos.y - rampStartY;
          rampAdvance = pl.pos.z - rampStartZ;
          pl.vel = Vec3{};
          pl.grounded = true;
        }
        // The legs must keep stepping on the slope, must bend, must stay in
        // range, and must not read as continuously snapping. Thresholds are
        // deliberately looser than the flat case — a climb IS a bigger motion —
        // but "stopped moving rhythmically" and "raked out of range" both fail.
        bool rampWalks = rampFootfalls >= 3 && rampKneeFlex > 6.0f &&
                         rampHipLo > hipLimLo - 4.0f &&
                         rampHipHi < hipLimHi + 4.0f &&
                         rampWorstJump < worstJumpFlat * 2.2f + 12.0f;
        bool rampNoAirClips = rampJumpTicks == 0 && rampFallTicks == 0;

        // Walk DOWN the state ladder and check the movement coupling at each
        // rung. Speed must be non-increasing and must actually drop by the
        // end — the whole point of the states is that damage costs you.
        const char* ladder[] = {"foot.R", "legL.R", "legU.L"};
        float prevSpeed = avatar.Locomotion().speedScale;
        bool monotone = prevSpeed == 1.0f;
        int statesSeen = 0;
        for (const char* nm : ladder) {
          if (!avatar.SeverByName(nm)) continue;
          for (int i = 0; i < 12; i++) avTick();
          float s = avatar.Locomotion().speedScale;
          monotone = monotone && s <= prevSpeed + 1e-4f;
          prevSpeed = s;
          if (avatar.LocoState() >= 0) statesSeen++;
        }
        bool slowed = prevSpeed < 1.0f;
        // both legs unusable -> no jump. This is derived from leg liveness
        // rather than authored, so it must hold whatever the rules say.
        // Captured HERE, while the avatar is still standing: reading it back
        // after Despawn would report the pristine defaults and quietly turn
        // this assertion into a tautology.
        const bool canJumpNow = avatar.Locomotion().canJump;
        const int stateNow = avatar.LocoState();
        bool noJump = !canJumpNow;

        uint32_t partsLeft = avatar.LimbBodyCount();
        bool partsGone = (int)partsLeft < nParts;
        size_t debrisNow = debris.BodyCount();
        bool becameDebris = debrisNow > 0;

        avatar.Despawn();
        bool tornDown = avatar.LimbBodyCount() == 0;

        bool avOk = spawned && allBodies && follows && tracksY &&
                    noSelfPush && monotone && slowed && noJump &&
                    statesSeen > 0 && partsGone && becameDebris && tornDown &&
                    legsUpright && legsAlternate && legsNotSplayed &&
                    legsNotInverted && armsHang && armsSwing && poseContinuous &&
                    kneeBends && jointsInRange && strideCoherent &&
                    rampWalks && rampNoAirClips;
        std::printf(
            "avatar: %s (%d parts, spawned=%d bodies=%d, followed %.1f vox, "
            "y-drift %.2f vox, self-push %.3f vox, states seen=%d (last %d) "
            "speed 1.00->%.2f monotone=%d canJump=%d, %u parts left, "
            "%zu debris, torn down=%d; walking legElev>=%.0f arm %.0f..%.0f "
            "legL %.0f..%.0f legR %.0f..%.0f "
            "lateral max %.1f mean %.1f/%.1f notSplayed=%d; "
            "upright=%d alternate=%d hang=%d swing=%d; "
            "falling hipToFootY %.2f notInverted=%d; "
            "pose jump flat %.1f deg vs ragged %.1f deg continuous=%d)\n",
            avOk ? "PASS" : "FAIL", nParts, spawned ? 1 : 0,
            allBodies ? 1 : 0, followed, drift, selfPush, statesSeen,
            stateNow, prevSpeed, monotone ? 1 : 0, canJumpNow ? 1 : 0,
            partsLeft, debrisNow, tornDown ? 1 : 0, minLegElev, minArmElev,
            maxArmElev, minLegSwing[0], maxLegSwing[0], minLegSwing[1],
            maxLegSwing[1], maxLegLateral, meanLegLateral[0],
            meanLegLateral[1], legsNotSplayed ? 1 : 0,
            legsUpright ? 1 : 0, legsAlternate ? 1 : 0,
            armsHang ? 1 : 0, armsSwing ? 1 : 0, worstLegUp,
            legsNotInverted ? 1 : 0, worstJumpFlat, worstJump,
            poseContinuous ? 1 : 0);
        // Reported on its own line: these are the locomotion pass's own
        // claims, and burying five more numbers in the paragraph above is how
        // a regression hides. Every one is a MEASUREMENT next to the authored
        // limit it is checked against, so a failure names its own cause.
        std::printf(
            "avatar gait clock+range: %s (knee flex %.1f deg bends=%d; "
            "hip x %.1f..%.1f vs [%.0f,%.0f] knee x %.1f..%.1f vs [%.0f,%.0f] "
            "inRange=%d; %d footfalls vs %d phase cycles (want %.1f) "
            "coherent=%d, stride %.2f Hz, crouch %.2f vox; "
            "RAMP climbed %.1f over %.1f vox, %d footfalls knee %.1f "
            "hip %.1f..%.1f posejump %.1f "
            "walks=%d, jumpTicks=%d fallTicks=%d clean=%d)\n",
            (kneeBends && jointsInRange && strideCoherent && rampWalks &&
             rampNoAirClips) ? "PASS" : "FAIL",
            kneeFlex, kneeBends ? 1 : 0, hipXLo, hipXHi, hipLimLo, hipLimHi,
            kneeXLo, kneeXHi, kneeLimLo, kneeLimHi,
            jointsInRange ? 1 : 0, walkFootfalls, phaseCycles, wantCycles,
            strideCoherent ? 1 : 0, avatar.StrideRate(), maxCrouch,
            rampClimb, rampAdvance,
            rampFootfalls, rampKneeFlex, rampHipLo, rampHipHi, rampWorstJump,
            rampWalks ? 1 : 0, rampJumpTicks, rampFallTicks,
            rampNoAirClips ? 1 : 0);
        mobOk = mobOk && avOk;

        // Reported separately so a head-look regression cannot hide inside
        // the avatar line's long list of gait assertions.
        std::printf(
            "avatar head look: %s (look +60 deg -> head %+.1f deg, -60 -> "
            "%+.1f deg, both vs hips and SIGN-MATCHING the input; "
            "hips held %.1f deg)\n",
            lookOk ? "PASS" : "FAIL", headPos, headNeg, hipsYaw);
        mobOk = mobOk && lookOk;

        // ---- body facing policy (ResolveAvatarHeading) ----
        // Driven directly rather than through the frame loop, which is the
        // point of having pulled it out of main.cpp: both bugs this policy has
        // had were invisible to every gate because it only ran while
        // rendering. Pure function, so a few hundred simulated ticks cost
        // nothing.
        const float kDt = kTickDt;
        const float kWalk = 5.0f / kVoxelMeters;   // 5 m/s, comfortably moving
        auto degOf = [](float rad) { return rad * 57.29578f; };
        auto wrapDeg = [](float d) {
          while (d > 180.0f) d -= 360.0f;
          while (d < -180.0f) d += 360.0f;
          return d;
        };

        // 1. THIRD PERSON SQUARES UP TO THE RUN, with no cone slack. Running
        //    due +X while the camera looks somewhere else entirely must still
        //    point the body at +X — "forward should always face the way they
        //    are running".
        float h3 = 0.0f;
        const float camAway = 2.2f;   // camera pointed well off the travel dir
        for (int i = 0; i < 400; i++)
          h3 = ResolveAvatarHeading(CameraMode::Third, camAway, h3,
                                    Vec3{kWalk, 0, 0}, kDt);
        // heading convention: forward is (sin h, ., cos h), so +X is pi/2.
        const float runErr3 = std::fabs(wrapDeg(degOf(h3) - 90.0f));
        bool facesRun = runErr3 < 5.0f;

        // 2. FIRST PERSON RECENTRES WHILE WALKING. Start the body 60 deg off
        //    the view — inside the 70 deg cone, so the old code zeroed the
        //    turn and the facing froze here forever, taking the arms with it.
        //    Walking must converge it back toward the view.
        const float camF = 0.0f;
        float hFroze = 60.0f / 57.29578f;
        for (int i = 0; i < 400; i++)
          hFroze = ResolveAvatarHeading(CameraMode::First, camF, hFroze,
                                        Vec3{kWalk, 0, 0}, kDt);
        const float driftErr = std::fabs(wrapDeg(degOf(hFroze)));
        bool recentres = driftErr < 15.0f;

        // 3. STANDING STILL IT DOES NOT. That is the glance, and it is the
        //    whole feature — a body that squares up while you stand there
        //    would make every look a turn again.
        float hStand = 60.0f / 57.29578f;
        for (int i = 0; i < 400; i++)
          hStand = ResolveAvatarHeading(CameraMode::First, camF, hStand,
                                        Vec3{}, kDt);
        const float standDeg = std::fabs(wrapDeg(degOf(hStand)));
        bool holdsGlance = standDeg > 45.0f;

        // 4. PAST THE CONE THE BODY IS DRAGGED even standing still, so the
        //    neck is never asked for more than it has. 140 deg is well beyond
        //    the 70 deg cone; the body must close to about the cone and stop.
        float hFar = 140.0f / 57.29578f;
        for (int i = 0; i < 400; i++)
          hFar = ResolveAvatarHeading(CameraMode::First, camF, hFar, Vec3{},
                                      kDt);
        const float farDeg = std::fabs(wrapDeg(degOf(hFar)));
        bool draggedToCone = farDeg > 55.0f && farDeg < 85.0f;

        bool faceOk = facesRun && recentres && holdsGlance && draggedToCone;
        std::printf(
            "avatar body facing: %s (3rd person run +X -> %.1f deg off; 1st "
            "person 60 deg off recentres to %.1f walking, holds %.1f standing; "
            "140 deg dragged to %.1f (cone 70))\n",
            faceOk ? "PASS" : "FAIL", runErr3, driftErr, standDeg, farDeg);
        mobOk = mobOk && faceOk;

        // Reported separately from `avatar` so a gait-look regression and a
        // footstep-plumbing regression never hide behind one another.
        std::printf(
            "avatar footfalls: %s (%d plants over 60 walk ticks, %d bad "
            "material, %d away from the body)\n",
            stepsOk ? "PASS" : "FAIL", footfalls, footfallsBadMat,
            footfallsFarFromFoot);
        mobOk = mobOk && stepsOk;

        // ---- melee: the blade cuts where the blade IS (game/melee.h) ----
        // The one property worth gating, and the reason the feature exists:
        // damage is located by the WEAPON'S POSE, not by the camera. So the
        // assertions are geometric and per-target, in the style of the mob
        // carve gate above:
        //   1. the held weapon reports a real edge segment, moving with the
        //      rig rather than pinned to the player,
        //   2. a swing carves the limb the edge actually passed through,
        //      and NOT one on the far side of the body, and
        //   3. the wielder never cuts itself, however the arc swings.
        // (2) is load-bearing: a cone-in-front-of-the-crosshair hitbox would
        // pass a "did it damage something" test and fail this one.
        {
          // The sword is a standalone ITEM now, not a part of the rig, so
          // the gate asks the library for it and equips it into the hand
          // socket. A missing item or a rig with no hand socket is a SKIP,
          // exactly as a missing rig part used to be.
          const int swordDef = items.Find("sword");
          const ItemDef* swordItem = items.At(swordDef);
          bool edgeOk = false, movedOk = false, hitRight = false;
          bool missedFar = true, selfSafe = true;
          bool equipped = false;
          int swordPart = -1;
          uint32_t targetBefore = 0, targetAfter = 0, farBefore = 0,
                   farAfter = 0;
          if (!swordItem) {
            std::printf("melee: SKIP (no \"sword\" item in the library)\n");
          } else {
            // A FRESH BODY. The avatar block above deliberately ends by
            // severing parts and calling Despawn(), so by here the rig has
            // no limb bodies at all and every edge query would read false —
            // a "failure" that says nothing about melee. Respawn before
            // measuring anything.
            debris.Reset();
            pl.pos = Vec3{140.5f, (float)(h2 + 2) + Player::kHalfY, 140.5f};
            avatar.Revive(pl, 0.0f);
            // EQUIP THE ITEM. This is the borrowed-slot path under test as
            // much as the edge is: if socket x grip fails to resolve, there
            // is no blade in the hand and every measurement below reads
            // false — so the failure is reported here, where it is legible,
            // rather than as a mysterious edge miss.
            equipped = avatar.EquipItem(swordItem);
            swordPart = avatar.HeldSlot();
            for (int i = 0; i < 8; i++) avTick();
            Vec3 b0, t0;
            float hw = 0;
            edgeOk = avatar.WeaponEdge(b0, t0, hw) && (t0 - b0).len() > 1.0f &&
                     hw > 0.0f;

            // Drive the arm somewhere definite and confirm the edge FOLLOWS.
            // An edge that never moves would still satisfy (1) while making
            // the whole feature a fixed hitbox in disguise.
            avatar.SetWeaponPose(Vec3{2.0f, 1.0f, 3.0f}, Vec3{0, 0, 1},
                                 Vec3{0, 1, 0}, 1.0f);
            for (int i = 0; i < 12; i++) avTick();
            Vec3 b1, t1;
            movedOk = avatar.WeaponEdge(b1, t1, hw) &&
                      (t1 - t0).len() > 0.5f;

            // A target mob beside the avatar, and the invariant that
            // matters: carve at the tip and the limb UNDER THE TIP loses
            // voxels while a limb on the opposite side of the same mob does
            // not. Both are read from the same mob, so "the sweep hit
            // everything" cannot pass this.
            if (critterDef >= 0 && avatar.WeaponEdge(b1, t1, hw)) {
              mobs.Reset();
              const MobDef& cd2 = mobs.Defs()[critterDef];
              uint64_t tid = mobs.Spawn(critterDef, {140, h2 + 1, 143});
              for (int i = 0; i < 6; i++) avTick();
              // Nearest and farthest live limbs to the blade tip: the carve
              // is aimed at the first and must not reach the second.
              int nearLimb = -1, farLimb = -1;
              float dn = 1e9f, df = -1.0f;
              for (size_t i = 0; i < cd2.limbs.size(); i++) {
                if (!mobs.LimbBody(tid, (int)i)) continue;
                float d = (mobs.LimbVoxelPos(tid, (int)i, 0) - t1).len();
                if (d < dn) { dn = d; nearLimb = (int)i; }
                if (d > df) { df = d; farLimb = (int)i; }
              }
              if (nearLimb >= 0 && farLimb >= 0 && nearLimb != farLimb) {
                targetBefore = mobs.LimbVoxelCount(tid, nearLimb);
                farBefore = mobs.LimbVoxelCount(tid, farLimb);
                std::vector<ParticleSpawn> cs;
                mobs.CarveLimbRadial(mobs.LimbBody(tid, nearLimb),
                                     mobs.LimbVoxelPos(tid, nearLimb, 0),
                                     0.7f, true, true, world, cs);
                for (int i = 0; i < 3; i++) avTick();
                targetAfter = mobs.LimbBody(tid, nearLimb)
                                  ? mobs.LimbVoxelCount(tid, nearLimb)
                                  : 0;
                farAfter = mobs.LimbBody(tid, farLimb)
                               ? mobs.LimbVoxelCount(tid, farLimb)
                               : 0;
                hitRight = targetAfter < targetBefore;
                missedFar = farAfter == farBefore;
              }
              mobs.Reset();
            }

            // The wielder must never be a valid target for its own blade.
            // The sweep's whole defence is OwnsBody, so test THAT: it has to
            // claim a body the avatar really owns and disown a foreign one.
            // Swinging and hoping the arc crossed an arm would be a much
            // weaker check — it passes whenever the swing happens to miss.
            {
              std::vector<DebrisVoxel> fv{{0, 0, 0, 0, kMatStone}};
              std::vector<float> dens;
              for (const auto& m : mats) dens.push_back((float)m.gpu.density);
              uint64_t foreign =
                  phys.CreateDebrisBody(fv, {150, h2 + 6, 150}, dens);
              // The sword itself is an avatar part, so OwnsBody must claim
              // it — that is precisely the body the sweep keeps hitting.
              const uint64_t swordBody = avatar.PartBody(swordPart);
              const bool ownsSelf =
                  swordBody != 0 && avatar.OwnsBody(swordBody);
              selfSafe = ownsSelf && !avatar.OwnsBody(foreign) &&
                         !avatar.OwnsBody(0);
              if (foreign) phys.RemoveBody(foreign);
            }
            // ---- THE HILT IS IN THE FIST ---------------------------------
            //
            // Everything above proves the blade CUTS; none of it proves the
            // sword is anywhere near the hand. It was not: the grip
            // translation was authored as if the socket sat at the pommel
            // butt, so the item's origin was parked 2.5 world voxels outboard
            // of a fist one voxel wide and the sword hung at the avatar's
            // feet. Every assertion in this gate still passed, because an
            // edge that has come loose from the hand is still an edge that
            // moves and still carves what it touches.
            //
            // So assert the thing that was actually broken: the item's
            // authored hilt box (item.h ItemHilt) and the hand's authored
            // limb box overlap, measured in WORLD space off the two live
            // bodies. Distance between centres, against the sum of the two
            // half-extents on each axis — a box overlap test, not a radius,
            // because both boxes are strongly anisotropic (the hilt is long
            // and thin) and a sphere test would pass with the pommel poking
            // out through the wrist.
            bool hiltInFist = false;
            float gripGap = -1.0f;
            if (equipped && swordItem->hilt.has) {
              const int handPart = avatar.PartIndex("hand.R");
              const uint64_t handBody =
                  handPart >= 0 ? avatar.PartBody(handPart) : 0;
              const uint64_t swordBody2 = avatar.PartBody(swordPart);
              BodyTransform hxf{}, sxf{};
              Vec3 hlo, hhi;
              if (handBody && swordBody2 &&
                  phys.GetTransform(handBody, hxf) &&
                  phys.GetTransform(swordBody2, sxf) &&
                  phys.GetLocalBounds(handBody, hlo, hhi)) {
                // Hand box centre/half-extent in world space. The collider is
                // only used to LOCATE the hand here (its own greedy-merge
                // padding is not what decides the grip) — the placement
                // itself is limb-derived, and this is the independent check.
                const Quat hq{hxf.quat[0], hxf.quat[1], hxf.quat[2],
                              hxf.quat[3]};
                const Vec3 handC =
                    hxf.pos + QuatRotate(hq, (hlo + hhi) * 0.5f);
                const Vec3 handHalf = (hhi - hlo) * 0.5f;
                // Hilt centre in world space, through the sword's own pose.
                const Quat sq{sxf.quat[0], sxf.quat[1], sxf.quat[2],
                              sxf.quat[3]};
                const Vec3 hiltC =
                    sxf.pos + QuatRotate(sq, swordItem->hilt.center);
                const Vec3 d = hiltC - handC;
                const Vec3 hh = swordItem->hilt.halfExtents;
                // Axis-wise overlap. Slack of half a world voxel absorbs the
                // spring jiggle the grip deliberately has (sword.json's
                // "spring"), which is motion the fist is supposed to allow.
                const float slack = 0.5f;
                const float ox = std::fabs(d.x) - (handHalf.x + hh.x + slack);
                const float oy = std::fabs(d.y) - (handHalf.y + hh.y + slack);
                const float oz = std::fabs(d.z) - (handHalf.z + hh.z + slack);
                hiltInFist = ox <= 0 && oy <= 0 && oz <= 0;
                gripGap = std::max(ox, std::max(oy, oz));
              }
            } else if (equipped && !swordItem->hilt.has) {
              // No hilt box authored: placement falls back to the raw grip
              // translation, which is the arrangement that shipped the bug.
              // Don't silently pass — say so.
              std::printf(
                  "melee: NOTE (sword declares no hilt box; grip falls back "
                  "to the raw translation)\n");
            }

            bool meleeOk = equipped && edgeOk && movedOk && hitRight &&
                           missedFar && selfSafe && hiltInFist;
            std::printf(
                "melee: %s (equipped=%d slot=%d, edge len ok=%d moved=%d, "
                "struck limb %u->%u, far limb %u->%u untouched=%d, "
                "self-hit guarded=%d, hilt in fist=%d gap %.2f vox)\n",
                meleeOk ? "PASS" : "FAIL", equipped ? 1 : 0, swordPart,
                edgeOk ? 1 : 0, movedOk ? 1 : 0,
                targetBefore, targetAfter, farBefore, farAfter,
                missedFar ? 1 : 0, selfSafe ? 1 : 0,
                hiltInFist ? 1 : 0, gripGap);
            mobOk = mobOk && meleeOk;
          }
        }
        debris.Reset();
      }
    }
  }

  // LEAVE THE WORLD AS THIS GATE FOUND IT — the same restore GateMobBurn
  // does, and for the same reason (CLAUDE.md rule 7: gates share one World and
  // the ones after this place fixtures by ABSOLUTE coordinate).
  //
  // This gate never needed it while it only spawned mobs. The locomotion pass
  // added real terrain: a flat stone shelf for the walk fixture and a stone
  // ramp for the climb, plus whatever the carve subtests ejected. Without the
  // restore that stone persisted, and `mob-burn` — which passes standalone —
  // failed in `--suite acceptance` and nowhere else. That is exactly the
  // ordering trap rule 7 describes, and the cost of finding it is why the
  // restore belongs here rather than in a comment telling the next person.
  mobs.Reset();
  debris.Reset();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
}

  // Verdict: the flag the moved body already computed.
  return mobOk ? Status::Pass : Status::Fail;
}

// ---- mob-burn: per-voxel body reactivity ---------------------------------
// docs/PLAN_body_reactivity.md. A mob is not "on fire": individual voxels of it
// are, cloth catches far more readily than flesh, flesh chars through a chain
// of materials, and a limb in acid dissolves through the same front with a
// different source.
//
// Six claims, ordered by how much each would cost if it broke silently. A and F
// are the two no amount of looking at the screen would catch.
Status GateMobBurn(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  MobSystem& mobs = c.mobs;
  DebrisSystem& debris = c.debris;
  const std::vector<MaterialDef>& mats = c.mats;
  bool ok = true;

  auto matId = [&](const char* n) -> uint32_t {
    for (size_t i = 0; i < mats.size(); i++)
      if (mats[i].name == n) return (uint32_t)i;
    return 0;
  };
  const uint32_t mFire = matId("fire"), mAcid = matId("acid"),
                 mCloth = matId("robe_cloth"),
                 mClothBurn = matId("cloth_burning"),
                 mClothChar = matId("cloth_charred"), mSkin = matId("skin"),
                 mCooked = matId("flesh_cooked"),
                 mCharred = matId("flesh_charred"),
                 mBurning = matId("flesh_burning"), mBlood = matId("blood");
  if (!mFire || !mAcid || !mCloth || !mSkin || !mCooked || !mBurning) {
    detail = "body-reactivity materials missing from materials.json";
    return Status::Fail;
  }

  // ---- A. the neighbour-count ramp reaches the CPU mirror -------------------
  // THE assertion of the whole feature, and the one nothing else stands in for.
  // `scaleByNeighbors` is what makes a lone hot voxel gutter out while a wide
  // front races, and the CPU side of the reaction table ignored `cond` entirely
  // until sim/reactcpu.h — so an authored minCount silently did nothing on the
  // one population it was written for. Asserted twice: on the arithmetic, and
  // on what the author actually WROTE, because a correct ramp applied to a rule
  // that lost its minCount in authoring is the same bug in a different hat.
  {
    ReactionGpu r{};
    r.chance = 100;
    r.nbrMat = kNbrAny;
    r.cond = kScaleEnable | ((3u - 1u) << kScaleMinShift) |
             (((uint32_t)(4.0f * (float)kScaleMulUnit + 0.5f) - kScaleMulUnit)
              << kScaleMulShift);
    const bool gate = ReactScaledChance(r, 0) == 0 &&
                      ReactScaledChance(r, 1) == 0 &&
                      ReactScaledChance(r, 2) == 0 && ReactScaledChance(r, 3) > 0;
    const bool ramp =
        ReactScaledChance(r, 6) == 400 && ReactScaledChance(r, 3) == 220;
    ReactionGpu plain{};
    plain.chance = 100;
    const bool unscaled = ReactScaledChance(plain, 0) == 100;

    // ...and the authored rule: flesh must not be ignitable below three hot
    // faces. If somebody softens this, the differential in B stops meaning
    // anything, and this line is what says so.
    uint32_t authoredMin = 0;
    const MaterialGpu& cg = mats[mCooked].gpu;
    for (uint32_t i = 0; i < cg.reactCount; i++) {
      const ReactionGpu& rr = c.reactions[cg.reactOffset + i];
      if ((rr.prodSelf & 0xFFFu) == mBurning && ReactScaleArmed(rr))
        authoredMin = ((rr.cond >> kScaleMinShift) & kScaleMinMask) + 1u;
    }
    const bool a = gate && ramp && unscaled && authoredMin >= 3;
    std::printf(
        "  burn ramp: %s (gate %d ramp %d plain %d, authored minCount %u)\n",
        a ? "PASS" : "FAIL", gate ? 1 : 0, ramp ? 1 : 0, unscaled ? 1 : 0,
        authoredMin);
    // Attribution, not elimination: if the authored rule is not where this
    // expects it, print the bucket rather than leave the next reader guessing
    // which of "wrong id", "wrong offset" and "rule dropped at load" it was.
    if (!a) {
      std::printf("    flesh_cooked id %u bucket %u+%u -> flesh_burning id %u, "
                  "table %zu\n",
                  mCooked, cg.reactOffset, cg.reactCount, mBurning,
                  c.reactions.size());
      for (uint32_t i = 0; i < cg.reactCount; i++) {
        const ReactionGpu& rr = c.reactions[cg.reactOffset + i];
        std::printf("    rule %u: kind %u prodSelf %u cond 0x%08x\n", i,
                    rr.packed & 3u, rr.prodSelf & 0xFFFu, rr.cond);
      }
    }
    ok = ok && a;
  }

  int wizDef = -1;
  for (size_t i = 0; i < mobs.Defs().size(); i++)
    if (mobs.Defs()[i].name == "wizard") wizDef = (int)i;
  if (wizDef < 0) {
    detail = "no wizard def (the fixture: robe_cloth over skin on one rig)";
    return Status::Fail;
  }
  const int nLimbs = (int)mobs.Defs()[wizDef].limbs.size();
  const int rootLimb = mobs.Defs()[wizDef].rootLimb;

  // Whole-creature material census. Per-limb counts are the honest unit, but
  // the interesting facts are about the CREATURE, and which limb the fire
  // happened to reach first is not a property worth pinning a test to.
  auto census = [&](uint64_t id, uint32_t mat) {
    uint32_t n = 0;
    for (int li = 0; li < nLimbs; li++) n += mobs.LimbMaterialCount(id, li, mat);
    return n;
  };
  auto burning = [&](uint64_t id) {
    uint32_t n = 0;
    for (int li = 0; li < nLimbs; li++) n += mobs.LimbBurningCount(id, li);
    return n;
  };

  uint32_t t = 12000;
  uint32_t mobFireOps = 0;
  // FIRE DOES NOT BLEED. Counted in the same place the fire ops are, because
  // both are "what the burn pass pushed into the world this tick" and neither
  // can be recovered afterwards -- blood droplets are micro particles that die
  // on contact, so an end-state census of the world cannot tell a body that
  // never bled from one that bled and dried.
  //
  // Two independent streams, because burning reached the gore path by two
  // different routes and closing one would have hidden the other:
  //   * the DRIP -- CarveLimb topping up a limb's bleed budget on every burn
  //     flush, i.e. dozens of times a second while alight;
  //   * the GOUT -- Sever arming an arterial spray on the parent when a limb
  //     finally burns through.
  uint32_t burnBloodDrops = 0;
  uint32_t burnBleedTicks = 0;
  bool countBlood = false;
  // The residency window follows this. It is set per fixture rather than left
  // at the origin because a mob outside the window DESPAWNS: with the window at
  // chunk y 0 and the wizard standing at terrain height, every census below
  // read zero and every assertion failed for a reason that had nothing to do
  // with burning (CLAUDE.md: anchor a fixture, never write an absolute Y).
  IVec3 pchunk{10, 0, 10};

  // ...AND NEVER AN ABSOLUTE X OR Z EITHER, which is the other half of the same
  // rule and cost this gate a long-standing in-suite failure.
  //
  // The Y was anchored; the columns were literals (170, 200, 230, 260). Those
  // sit comfortably inside the window when the gate runs ALONE, because the
  // origin is still {0,0,0}. In a full run `streaming` has already walked the
  // player out and left the origin ~20 chunks along x — so every fixture landed
  // OUTSIDE the residency window, where world writes are dropped and reactions
  // never run. The symptom was a whole gate of zeroes (0 alight, 0 fire ops,
  // 0 indexed cells) that passed perfectly on its own: exactly the trap
  // selftest.h's ordering note describes, in the file that already quoted it.
  //
  // `inset` keeps the original spacing, so the fixtures stay as far apart from
  // each other as they were — they must not share terrain, and one of them
  // lights a sustained blaze.
  const IVec3 wOrg = world.WindowOrigin();
  auto fixture = [&](int inset) {
    return IVec3{wOrg.x * (int)kChunk + inset, 0, wOrg.z * (int)kChunk + inset};
  };
  // One tick of the real thing. `soakMat` fills the mob's own box every tick (a
  // sustained blaze, or a bath of acid). The ops the MOB emitted are counted
  // BEFORE the fixture adds its own, so "the limb emitted fire into the grid"
  // cannot be satisfied by the fixture's own writes.
  auto burnTick = [&](uint64_t id, uint32_t soakMat, int soakUp) {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    mobs.PreTick(t + 1, world, ops, cellOps, spawns);
    for (const CellOp& op : cellOps)
      if ((op.word & 0xFFFu) == mFire) mobFireOps++;
    if (countBlood) {
      for (const ParticleSpawn& s : spawns)
        if ((s.payload & 0xFFFu) == mBlood) burnBloodDrops++;
      if (!mobs.BleedSources().empty()) burnBleedTicks++;
    }
    if (soakMat) {
      const Vec3 at = mobs.LimbVoxelPos(id, rootLimb, 0);
      const IVec3 b{ifloor(at.x), ifloor(at.y), ifloor(at.z)};
      for (int dy = -8; dy <= soakUp; dy++)
        for (int dz = -3; dz <= 3; dz++)
          for (int dx = -3; dx <= 3; dx++) {
            const IVec3 cc{b.x + dx, b.y + dy, b.z + dz};
            if (!world.CellInWindow(cc)) continue;
            if (cellOps.size() >= kMaxCellOpsPerTick) break;
            cellOps.push_back({World::SlotCellIndex(cc),
                               PackVoxNew(soakMat, 7u) | kCellOpIfAir});
          }
    }
    debris.QueueSupportEvents(world.Snap());
    debris.PreTick(t + 1, world, cellOps, spawns);
    ++t;
    SubmitTick(ctx, world, sim, t, kDefaultSeed, ops, {}, cellOps, false,
               pchunk, true, false, spawns);
    ctx.WaitIdle();
    ctx.ProcessEvents();
    c.phys.Step(kTickDt);
    debris.PostStep();
    mobs.PostStep();
  };

  // ---- D. an idle mob in a settled world does zero burn work ---------------
  // Rule 2, stated for a new population. It runs FIRST, on a clean world,
  // because it is the one claim a fire lit earlier would make untestable.
  {
    debris.Reset();
    mobs.Reset();
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    const IVec3 site = fixture(170);
    const int h = World::TerrainHeight(site.x, site.z, kDefaultSeed);
    pchunk = IVec3{site.x >> 4, h >> 4, site.z >> 4};
    const uint64_t id = mobs.Spawn(wizDef, {site.x, h + 1, site.z});
    if (id) {
      const uint32_t cloth0 = census(id, mCloth), skin0 = census(id, mSkin);
      const uint32_t fireOps0 = mobFireOps;
      for (int i = 0; i < 30; i++) burnTick(id, 0, 0);
      const uint32_t idleFront = burning(id);
      const uint32_t idleOps = mobFireOps - fireOps0;
      const bool idle = idleFront == 0 && idleOps == 0 &&
                        census(id, mCloth) == cloth0 &&
                        census(id, mSkin) == skin0;
      std::printf("  idle mob: %s (front %u, ops %u, cloth %u, skin %u)\n",
                  idle ? "PASS" : "FAIL", idleFront, idleOps, cloth0, skin0);
                  std::fflush(stdout);
      ok = ok && idle;
    } else {
      std::printf("  idle mob: FAIL (spawn refused)\n");
      ok = false;
    }
    mobs.Reset();
  }

  // ---- B + C + F. cloth vs flesh in a real fire ----------------------------
  {
    debris.Reset();
    mobs.Reset();
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    const IVec3 site = fixture(200);
    const int h = World::TerrainHeight(site.x, site.z, kDefaultSeed);
    pchunk = IVec3{site.x >> 4, h >> 4, site.z >> 4};
    const uint64_t id = mobs.Spawn(wizDef, {site.x, h + 1, site.z});
    if (!id) {
      detail = "spawn refused";
      return Status::Fail;
    }
    const uint32_t cloth0 = census(id, mCloth), skin0 = census(id, mSkin);
    const uint32_t fireOps0 = mobFireOps;
    for (int i = 0; i < 12; i++) burnTick(id, 0, 0);  // settle onto the ground

    // SAMPLED EVERY TICK, not read at the end. A wizard held in a bonfire for
    // five seconds burns to death, at which point the mob is gone and every
    // census reads zero — so an end-state comparison reports "cloth 100%, skin
    // 100%" and proves nothing. What "cloth catches more readily" actually
    // means is a RATE, so the measurement is the tick each material first
    // halved on, taken while the creature is still there to measure.
    const int kBlaze = 150, kNever = 1 << 20;
    int clothHalf = kNever, skinHalf = kNever, aliveTicks = 0;
    uint32_t peakFront = 0, peakAlight = 0, peakCharred = 0;
    uint32_t lastCloth = cloth0, lastSkin = skin0;
    for (int i = 0; i < kBlaze; i++) {
      burnTick(id, mFire, 18);
      const uint32_t cl = census(id, mCloth), sk = census(id, mSkin);
      const uint32_t alight = census(id, mClothBurn) + census(id, mBurning);
      const uint32_t charred = census(id, mClothChar) + census(id, mCooked) +
                               census(id, mCharred);
      if (cl + sk + alight + charred == 0) break;  // creature consumed
      aliveTicks = i + 1;
      lastCloth = cl;
      lastSkin = sk;
      peakFront = std::max(peakFront, burning(id));
      peakAlight = std::max(peakAlight, alight);
      peakCharred = std::max(peakCharred, charred);
      if (clothHalf == kNever && cloth0 && cl * 2 < cloth0) clothHalf = i;
      if (skinHalf == kNever && skin0 && sk * 2 < skin0) skinHalf = i;
    }
    const uint32_t emitted = mobFireOps - fireOps0;

    // B: cloth goes first, and by a margin. Both the halving order and the
    // remaining fractions are asserted — the order alone would pass on a rig
    // where nothing burned at all and both stayed at "never".
    const float clothLost =
        cloth0 ? (float)(cloth0 - lastCloth) / (float)cloth0 : 0.0f;
    const float skinLost =
        skin0 ? (float)(skin0 - lastSkin) / (float)skin0 : 0.0f;
    const bool bOk = cloth0 > 0 && skin0 > 0 && clothHalf < kNever &&
                     clothHalf < skinHalf && clothLost > skinLost;
    std::printf(
        "  cloth vs flesh: %s (cloth halved at t+%d, skin at t+%s; %.0f%% vs "
        "%.0f%% gone after %d ticks)\n",
        bOk ? "PASS" : "FAIL", clothHalf,
        skinHalf == kNever ? "never" : std::to_string(skinHalf).c_str(),
        clothLost * 100.0f, skinLost * 100.0f, aliveTicks);
    ok = ok && bOk;

    // The chain is MATERIAL identity, so a state that never appears is a state
    // that does not exist. Charring being invisible on painted surfaces is a
    // real failure mode (a nonzero art slot overrides the material colour), and
    // these counts are what would still show the transition happened.
    const bool chainOk = peakAlight > 0 && peakCharred > 0;
    std::printf("  burn chain: %s (peak %u alight, peak %u cooked/charred)\n",
                chainOk ? "PASS" : "FAIL", peakAlight, peakCharred);
                std::fflush(stdout);
    ok = ok && chainOk;

    // C: the grid half. A burning mob that emits no fire cannot light the bush
    // it runs into, which is the whole reason this lives in the CA rather than
    // as a status effect on the creature.
    const bool cOk = emitted > 5;
    std::printf("  fire into grid: %s (%u fire ops emitted by limbs)\n",
                cOk ? "PASS" : "FAIL", emitted);
                std::fflush(stdout);
    ok = ok && cOk;

    // F: it TERMINATES. Take the fire away and the front must reach zero. A
    // burn that sustains itself is rule 2 broken — the mob would never settle,
    // and neither would the chunks it walks through.
    uint32_t settleTicks = 0;
    for (int i = 0; i < 400; i++) {
      burnTick(id, 0, 0);
      settleTicks++;
      if (burning(id) == 0) break;
    }
    const uint32_t finalFront = burning(id);
    // The mob may already have burned to death, in which case the front is
    // trivially zero — say so, so a vacuous pass is visible rather than
    // comforting. The corpse's own termination is subtest G's business.
    const bool fOk = finalFront == 0;
    std::printf(
        "  burn terminates: %s (peak front %u -> %u after %u ticks, %s)\n",
        fOk ? "PASS" : "FAIL", peakFront, finalFront, settleTicks,
        aliveTicks >= kBlaze ? "creature survived" : "creature was consumed");
    ok = ok && fOk;
    mobs.Reset();
    debris.Reset();
  }

  // ---- H. FIRE EATS BEFORE IT TAKES, AND WHAT IT LEAVES IS CHAR -----------
  // Subtest F asserts the burn FRONT reaches zero, and that is a different
  // claim: the front is a list of cells in the burn INDEX, and every carve
  // throws the index away. Both of the bugs pinned here made F pass.
  //
  //   1. The front hit zero because the index had been dropped, not because
  //      anything stopped burning. The cheap gate at the top of BurnOneLimb
  //      ("nothing hot nearby and an empty front") then refused to rebuild it,
  //      so the body's cloth_burning / flesh_burning voxels never rolled their
  //      decay again: a character left permanently sheathed in flame that had
  //      nothing left to burn. Only a MATERIAL census sees this, which is why
  //      that is what is asserted, and why it is taken over the DEBRIS too --
  //      a corpse is bodies, not limbs, and a mob-only census goes quiet the
  //      moment the mob does.
  //
  //   2. Carve damage was charged CUMULATIVELY (Mob::CarveLimb): `at0 -
  //      nowCount` is everything the limb has EVER lost, so N carves cost
  //      N(N+1)/2. Burning flushes every max(12, n>>6) voxels removed, so a
  //      burning limb carves dozens of times and reached hp 0 having lost about
  //      14% of its volume. Every fire dismembered.
  //
  // The second claim is stated as an INVARIANT rather than as "the creature
  // survives", because a wizard whose whole robe burns is authored to die (see
  // the clothing-layer note in reactions.json) and a fixture tuned to keep it
  // alive would be testing the fixture. What must never happen is a limb
  // leaving while it is still mostly there: the geometric floor is
  // kLimbCollapseFraction (25% left) and the hp floor at
  // kCarveDamagePerVolume 1.5 is 33% left, so 50% is a bound both routes clear
  // comfortably and the cumulative bug (86% left) misses by a mile.
  {
    debris.Reset();
    mobs.Reset();
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    const IVec3 site = fixture(230);
    const int h = World::TerrainHeight(site.x, site.z, kDefaultSeed);
    pchunk = IVec3{site.x >> 4, h >> 4, site.z >> 4};
    const uint64_t id = mobs.Spawn(wizDef, {site.x, h + 1, site.z});
    if (!id) {
      detail = "spawn refused";
      return Status::Fail;
    }
    for (int i = 0; i < 12; i++) burnTick(id, 0, 0);  // settle onto the ground

    // NO WORLD FIRE. The soak box the other subtests use keeps relighting the
    // body, and a non-empty `scanHot` means the cheap gate never runs at all --
    // the index is rebuilt every tick and the frozen-material case cannot
    // occur. Igniting a patch and then leaving the creature alone is the whole
    // point: this is the "you walked out of the fire" case.
    int armLimb = -1;
    for (int li = 0; li < nLimbs; li++)
      if (mobs.Defs()[wizDef].limbs[li].name == "armU.L") armLimb = li;
    const uint32_t lit =
        armLimb >= 0 ? mobs.IgniteLimb(id, armLimb, 60u, mCloth) : 0u;

    auto alightOnMob = [&]() {
      return mobs.IsAlive(id) ? census(id, mClothBurn) + census(id, mBurning)
                              : 0u;
    };
    auto alightInWorld = [&]() {
      return alightOnMob() + debris.TotalBodyMaterial(mClothBurn) +
             debris.TotalBodyMaterial(mBurning);
    };

    // THE INVARIANT IS MEASURED WHERE IT HAPPENS. Inferring "a limb came off
    // while it was still mostly there" from outside cannot be made to work:
    // Die() detaches every limb at once, DetachLimb cascades to a limb's
    // children (so a healthy forearm "comes off" whenever its upper arm does),
    // and a limb can lose half of itself to a connectivity split inside the
    // same tick it is cut. Three attempts at an external metric each measured
    // one of those instead of the claim. MobSystem records it at the cut.
    mobs.ClearSeverStats();
    uint32_t peakAlight = 0;
    int deathTick = -1;
    burnBloodDrops = burnBleedTicks = 0;
    countBlood = true;
    for (int i = 0; i < 630; i++) {
      burnTick(id, 0, 0);
      peakAlight = std::max(peakAlight, alightInWorld());
      if (deathTick < 0 && !mobs.IsAlive(id)) deathTick = i;
    }
    countBlood = false;
    const bool died = deathTick >= 0;
    const float worstSever = mobs.WorstSeverFraction();
    const std::string worstName = mobs.WorstSeverLimb();

    const uint32_t stillAlight = alightInWorld();
    const uint32_t charred =
        (mobs.IsAlive(id) ? census(id, mClothChar) + census(id, mCharred) : 0u) +
        debris.TotalBodyMaterial(mClothChar) + debris.TotalBodyMaterial(mCharred);

    const bool wasLit = lit > 0 && peakAlight > 0;
    const bool out = stillAlight == 0;      // claim 1: nothing still burning
    const bool charOk = charred > 0;        // it charred rather than vanishing
    const bool ate = worstSever <= 0.5f;    // claim 2: fire eats before it takes
    const bool hOk = wasLit && out && charOk && ate;
    std::printf(
        "  burn leaves char: %s (%u lit, peak %u alight -> %u after 630 quiet "
        "ticks, %u charred; worst sever %s at %.0f%% of spawn volume, floor "
        "50%%; %s)\n",
        hOk ? "PASS" : "FAIL", lit, peakAlight, stillAlight, charred,
        worstSever < 0.0f ? "(nothing severed)" : worstName.c_str(),
        worstSever < 0.0f ? 0.0f : worstSever * 100.0f,
        died ? ("creature died at t+" + std::to_string(deathTick)).c_str()
             : "creature survived");
    std::fflush(stdout);
    ok = ok && hOk;

    // ---- claim 3: FIRE CAUTERISES ------------------------------------------
    //
    // A creature burning to death, for 630 ticks, must not produce ONE drop of
    // blood. Zero is the right threshold and not a strict one: burning reached
    // the gore path through CarveLimb's drip and Sever's gout, both of which
    // now refuse while `inBurnFlush_` is set, so any non-zero here means a
    // third route nobody has found yet rather than a rate that needs tuning.
    //
    // Asserted on the SPAWN STREAM and on BleedSources, which are different
    // things: the first is matter thrown into the world, the second is what the
    // audio layer would turn into a wound loop. A body that quietly holds a
    // full bleed budget while emitting nothing is still bleeding as far as
    // every other system is concerned.
    const bool dryOk = burnBloodDrops == 0 && burnBleedTicks == 0;
    std::printf(
        "  fire does not bleed: %s (%u blood droplets, %u of 630 ticks with an "
        "open wound)\n",
        dryOk ? "PASS" : "FAIL", burnBloodDrops, burnBleedTicks);
    std::fflush(stdout);
    ok = ok && dryOk;
    mobs.Reset();
    debris.Reset();
  }


  // ---- G. a SEVERED burning limb keeps burning, and so does a corpse -------
  // DebrisSystem::BurnBodies refused micro bodies outright until this package,
  // for two reasons that had both expired: the copy-on-write brick pool shipped
  // (so a per-body edit is visible) and the pass now divides body-local
  // coordinates by the lattice scale instead of mapping them straight onto
  // world cells. Every limb of every rig with skinScale > 1 becomes a micro
  // body the instant it is severed or its owner dies — so while that skip was
  // in place, cutting off a burning arm put the fire out, and a corpse could
  // lie in a bonfire indefinitely. Counted on the authoritative lattice
  // (TotalBodyVoxels), because a micro body emits no cube instances to count.
  {
    debris.Reset();
    mobs.Reset();
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    const IVec3 site = fixture(260);
    const int h = World::TerrainHeight(site.x, site.z, kDefaultSeed);
    pchunk = IVec3{site.x >> 4, h >> 4, site.z >> 4};
    const uint64_t id = mobs.Spawn(wizDef, {site.x, h + 1, site.z});
    uint32_t adopted = 0, before = 0, after = 0, bodies = 0;
    if (id) {
      for (int i = 0; i < 12; i++) burnTick(id, 0, 0);
      // Light the whole creature, then kill it: the corpse hands every limb to
      // DebrisSystem, fire and all.
      for (int li = 0; li < nLimbs; li++) mobs.IgniteLimb(id, li, 40);
      for (int i = 0; i < 30; i++) burnTick(id, mFire, 18);
      // Severing the ROOT limb is death (Sever routes root/vital to Die), which
      // is the path that hands every limb to DebrisSystem.
      mobs.Sever(id, rootLimb);
      for (int i = 0; i < 3; i++) burnTick(0, 0, 0);
      adopted = debris.BodyCount();
      before = debris.TotalBodyVoxels();
      for (int i = 0; i < 120; i++) burnTick(0, 0, 0);
      after = debris.TotalBodyVoxels();
      bodies = debris.BodyCount();
    }
    const bool gOk = adopted > 0 && before > 0 && after < before;
    std::printf("  corpse burns: %s (%u bodies adopted, %u -> %u voxels, %u "
                "bodies left)\n",
                gOk ? "PASS" : "FAIL", adopted, before, after, bodies);
                std::fflush(stdout);
    ok = ok && gOk;
    mobs.Reset();
    debris.Reset();
  }

  // ---- E. acid dissolves a limb (package C: same front, different source) --
  // Nothing acid-specific was written for this. `acid + tag:dissolvable ->
  // neighborBecomes air` is an old rule, skin/cloth/leather are already
  // `dissolvable`, and the burn pass evaluates a WORLD neighbour's rules onto
  // the limb — the same direction the GPU evaluates them. If this fails, that
  // inbound half is gone and acid quietly stops touching creatures at all.
  {
    debris.Reset();
    mobs.Reset();
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    const IVec3 site = fixture(230);
    const int h = World::TerrainHeight(site.x, site.z, kDefaultSeed);
    pchunk = IVec3{site.x >> 4, h >> 4, site.z >> 4};
    const uint64_t id = mobs.Spawn(wizDef, {site.x, h + 1, site.z});
    uint32_t before = 0, after = 0;
    if (id) {
      for (int i = 0; i < 10; i++) burnTick(id, 0, 0);
      for (int li = 0; li < nLimbs; li++) before += mobs.LimbVoxelCount(id, li);
      for (int i = 0; i < 120; i++) burnTick(id, mAcid, 4);
      for (int li = 0; li < nLimbs; li++) after += mobs.LimbVoxelCount(id, li);
    }
    const bool eOk = id != 0 && before > 0 && after < before;
    std::printf("  acid dissolves: %s (%u -> %u collider voxels)\n",
                eOk ? "PASS" : "FAIL", before, after);
                std::fflush(stdout);
    ok = ok && eOk;
    mobs.Reset();
    debris.Reset();
  }

  // ---- H. THE PLAYER burns, by the same rules and the same code -----------
  // The avatar is a MobDef with a different driver, and it keeps its own rig in
  // PlayerAvatar::Part rather than MobSystem::Limb — so "mobs burn" does not
  // imply "you burn", and the two could drift apart in exactly the way that is
  // invisible until someone notices they are immune to their own fireball.
  // They cannot drift here because there is one pass (MobSystem::BurnOneLimb)
  // and PlayerAvatar drives it; this asserts that the wiring is live.
  {
    debris.Reset();
    mobs.Reset();
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    // The def the GAME actually uses, never a hardcoded name: a test pinned to
    // the old one would keep passing against a character nobody plays.
    const std::string avDefName = kAvatarDefName;
    int avDef = -1;
    for (size_t i = 0; i < mobs.Defs().size(); i++)
      if (mobs.Defs()[i].name == avDefName) avDef = (int)i;
    const IVec3 site = fixture(300);
    const int h = World::TerrainHeight(site.x, site.z, kDefaultSeed);
    pchunk = IVec3{site.x >> 4, h >> 4, site.z >> 4};
    PlayerAvatar avatar;
    avatar.Init(&c.phys, &world, &debris, mats, &mobs);
    if (avDef >= 0) avatar.SetDefs(&mobs.Defs(), avDefName);
    Player pl;
    pl.fly = false;
    pl.grounded = true;
    pl.pos = Vec3{(float)site.x + 0.5f, (float)(h + 2) + Player::kHalfY,
                  (float)site.z + 0.5f};
    const bool spawned = avDef >= 0 && avatar.Spawn(pl, 0.0f);
    const int nParts = spawned ? (int)mobs.Defs()[avDef].limbs.size() : 0;
    auto avCensus = [&](uint32_t mat) {
      uint32_t n = 0;
      for (int i = 0; i < nParts; i++) n += avatar.PartMaterialCount(i, mat);
      return n;
    };
    auto avBurning = [&]() {
      uint32_t n = 0;
      for (int i = 0; i < nParts; i++) n += avatar.PartBurningCount(i);
      return n;
    };
    uint32_t avFireOps = 0;
    auto avTick = [&](uint32_t soakMat) {
      std::vector<BrushOp> ops;
      std::vector<ParticleSpawn> spawns;
      std::vector<CellOp> cellOps;
      avatar.PreTick(t + 1, pl, 0.0f, kTickDt, world, ops, cellOps, spawns);
      for (const CellOp& op : cellOps)
        if ((op.word & 0xFFFu) == mFire) avFireOps++;
      if (soakMat) {
        const IVec3 b{site.x, h + 1, site.z};
        for (int dy = -2; dy <= 18; dy++)
          for (int dz = -3; dz <= 3; dz++)
            for (int dx = -3; dx <= 3; dx++) {
              const IVec3 cc{b.x + dx, b.y + dy, b.z + dz};
              if (!world.CellInWindow(cc)) continue;
              if (cellOps.size() >= kMaxCellOpsPerTick) break;
              cellOps.push_back({World::SlotCellIndex(cc),
                                 PackVoxNew(soakMat, 7u) | kCellOpIfAir});
            }
      }
      debris.QueueSupportEvents(world.Snap());
      debris.PreTick(t + 1, world, cellOps, spawns);
      ++t;
      SubmitTick(ctx, world, sim, t, kDefaultSeed, ops, {}, cellOps, false,
                 pchunk, true, false, spawns);
      ctx.WaitIdle();
      ctx.ProcessEvents();
      c.phys.Step(kTickDt);
      debris.PostStep();
      avatar.PostStep();
    };

    // WHAT "BURNS" MEANS DEPENDS ON WHAT THE AVATAR IS MADE OF, and this
    // subtest must not assume the player is dressed. `human`, the stock base
    // body, is ONE material (flesh) everywhere with its colours in an art layer
    // — a cloth census on it is legitimately 0, and asserting cloth here would
    // fail a perfectly correct rig for the crime of wearing nothing. The claim
    // this subtest owns is narrower than it looks: that the avatar's OWN BODY
    // is wired into the same burn pass mobs use. So it measures the body mass
    // the rig actually has, cloth plus flesh. The cloth-goes-FIRST ordering is
    // a separate claim and is asserted on the wizard above, which wears a robe.
    uint32_t idleFront = 0, body0 = 0, cloth0 = 0, peakAlight = 0, peakChar = 0;
    float bodyLost = 0;
    if (spawned) {
      for (int i = 0; i < 10; i++) avTick(0);
      idleFront = avBurning();            // an idle player must cost nothing
      cloth0 = avCensus(mCloth);
      body0 = cloth0 + avCensus(mSkin);
      uint32_t lastBody = body0;
      for (int i = 0; i < 90 && avatar.Spawned() && avatar.IsAlive(); i++) {
        avTick(mFire);
        const uint32_t bd = avCensus(mCloth) + avCensus(mSkin);
        if (bd) lastBody = bd;
        peakAlight = std::max(peakAlight,
                              avCensus(mClothBurn) + avCensus(mBurning));
        peakChar = std::max(peakChar, avCensus(mClothChar) + avCensus(mCooked) +
                                          avCensus(mCharred));
      }
      bodyLost = body0 ? (float)(body0 - lastBody) / (float)body0 : 0.0f;
    }
    const bool hOk = spawned && idleFront == 0 && body0 > 0 &&
                     bodyLost > 0.05f && peakAlight > 0 && peakChar > 0 &&
                     avFireOps > 0;
    uint32_t indexed = 0;
    for (int i = 0; i < nParts; i++) indexed += avatar.PartBurnIndexCells(i);
    // `indexed cells` is the number that localizes a failure here: 0 means the
    // pass never even saw anything reactive next to the player (a CPU-mirror
    // problem), non-zero with nothing alight means it saw and did not react (a
    // rules problem). They have completely different causes.
    std::printf("  player burns: %s (idle front %u, body %u of which cloth %u "
                "-> %.0f%% gone, peak %u alight / %u charred, %u fire ops; "
                "%d parts, %u bodies, %u indexed cells)\n",
                hOk ? "PASS" : "FAIL", idleFront, body0, cloth0,
                bodyLost * 100.0f, peakAlight, peakChar, avFireOps, nParts,
                avatar.LimbBodyCount(), indexed);
                std::fflush(stdout);
    ok = ok && hOk;
    avatar.Despawn();
    mobs.Reset();
    debris.Reset();
  }

  // Leave the world as this gate found it: the fires and acid above are real
  // grid state, and the gates after this one place fixtures by absolute
  // coordinate (CLAUDE.md rule 7).
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  detail = Format("%u fire ops from limbs", mobFireOps);
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& MobGates() {
  static const std::vector<Gate> g = {
      // THE GAIT AND AVATAR GATE, re-registered. It was dropped by ec764e8
      // ("test cleanup") which emptied this list but left GateMob and its
      // ~1200 lines of pose assertions in the file — so every claim in there
      // (legs alternate, legs not splayed, legs not inverted in a fall, arms
      // swing, pose continuity on ragged ground) has been dead code, compiled
      // and never called, ever since. Nothing pointed at it: `--gate mob`
      // answered "no such gate", which reads as a typo rather than as a gate
      // that used to exist.
      //
      // Draws: the micro-body view sweep renders the critter from 14 angles
      // into the shared offscreen target.
      {"mob", "mob", {}, false, GateMob, /*needsRender=*/true},
      // Per-voxel body reactivity. No render: every claim is a count.
      {"mob-burn", "mob", {}, false, GateMobBurn, /*needsRender=*/false},
  };
  return g;
}

}  // namespace selftest
