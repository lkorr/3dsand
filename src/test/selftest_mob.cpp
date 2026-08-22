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
      mobs.PreTick(t + 1, world, ops, spawns);
      debris.QueueSupportEvents(world.Snap());
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
        avatar.Init(&phys, &world, &debris, mats);
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
          avatar.PreTick(t + 1, pl, 0.0f, kTickDt, world, ops, spawns);
          debris.QueueSupportEvents(world.Snap());
          std::vector<CellOp> cellOps;
          debris.PreTick(t + 1, world, cellOps, spawns);
          ++t;
          SubmitTick(ctx, world, sim, t, kDefaultSeed, ops, {}, cellOps,
                     false, {140 / 16, h2 / 16, 140 / 16}, true, false,
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
        const float walkStep =
            (CurrentTuning().player.walkSpeed / kVoxelMeters) * kTickDt;
        // State the velocity this teleport represents — the avatar reads the
        // player's own vel now, not a position difference. See the note at
        // the first walk loop above.
        pl.vel = Vec3{0, 0, walkStep / kTickDt};
        for (int i = 0; i < 90; i++) {
          pl.pos.z += walkStep;   // forward at heading 0, see swingOf above
          avTick();
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
                    legsNotInverted && armsHang && armsSwing && poseContinuous;
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
}

  // Verdict: the flag the moved body already computed.
  return mobOk ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& MobGates() {
  static const std::vector<Gate> g = {
      {"mob", "mob", {}, false, GateMob},
  };
  return g;
}

}  // namespace selftest
