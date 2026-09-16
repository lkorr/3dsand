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
#include <filesystem>
#include <unordered_set>
#include <string>
#include <vector>

#include "game/avatar.h"
#include "game/bodyreg.h"
#include "game/equipment.h"
#include "game/item.h"
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
    // ---- the shared clip library (assets/anims/*.json) -------------------
    // Data-driven: every file there must have compiled onto the human rig
    // under its file stem (mob.cpp "THE SHARED CLIP LIBRARY"), because that
    // is the name an attack style's `clip` field uses. Zero files is a pass
    // that says so; a file the human does not have is the pipeline broken
    // between the tuner's "-> library" button and Mob::PlayClip.
    {
      const MobDef* human = nullptr;
      for (const MobDef& md : mobs.Defs())
        if (md.name == "human") human = &md;
      int files = 0, missing = 0;
      std::string missingNames;
      std::error_code ec;
      for (auto& e : std::filesystem::directory_iterator(
               AssetDir() + "/anims", ec)) {
        if (!e.is_regular_file() || e.path().extension() != ".json") continue;
        files++;
        const std::string stem = e.path().stem().string();
        if (human == nullptr || human->skel.FindClip(stem) < 0) {
          missing++;
          missingNames += (missingNames.empty() ? "" : ", ") + stem;
        }
      }
      const bool libOk = missing == 0;
      std::printf("mob clip library: %s (%d file(s) in assets/anims, %d not on "
                  "the human%s%s)\n",
                  libOk ? "PASS" : "FAIL", files, missing,
                  missing ? ": " : "", missingNames.c_str());
      mobOk = mobOk && libOk;

      // ---- natural weapons (mob.h MobNaturalWeaponDef; PLAN §3) ----------
      // EVERY `natural` ENTRY ON EVERY DEF, not just the human's: the block
      // is inherited through `extends`, so a zombie carries the human's jaws
      // and a rig that renames a part would break the inheritor rather than
      // the original. Two claims, and they are the two ways the block can be
      // wrong without anything else noticing — a weapon on a part this rig
      // has not got never resolves (the loader says so and drops it, so the
      // survivors must all resolve), and a weapon with a zero-length edge has
      // no segment for the sweep and no direction for the driver, which reads
      // downstream as "the creature punches and never connects".
      {
        int weapons = 0, bad = 0;
        std::string badNames;
        for (const MobDef& md : mobs.Defs())
          for (const MobNaturalWeaponDef& nw : md.natural) {
            weapons++;
            const bool livePart =
                nw.partIndex >= 0 && nw.partIndex < (int)md.limbs.size() &&
                md.limbs[nw.partIndex].name == nw.part;
            const bool realEdge = (nw.edgeTo - nw.edgeFrom).len() > 1e-3f &&
                                  nw.edgeHalfWidth > 0.0f;
            if (livePart && realEdge) continue;
            bad++;
            badNames += (badNames.empty() ? "" : ", ") + md.name + "/" +
                        nw.name + (livePart ? " (degenerate edge)"
                                            : " (no such part)");
          }
        const bool natOk = bad == 0;
        std::printf("mob natural weapons: %s (%d across %d def(s), %d "
                    "broken%s%s)\n",
                    natOk ? "PASS" : "FAIL", weapons, (int)mobs.Defs().size(),
                    bad, bad ? ": " : "", badNames.c_str());
        mobOk = mobOk && natOk;
      }
    }
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
        // WHETHER THIS PROBE CAN RUN AT ALL depends on the voxel size.
        // A limb only gets a micro brick when its def's skinScale > 1
        // (mob.cpp), and skinScale is now DERIVED as artVoxelsPerMetre /
        // kVoxelsPerMetre (DESIGN.md 3b). The critter's art is 20 vox/m, so
        // in a 20 vox/m world it is exactly 1:1 with the grid, takes the
        // ordinary cube path, and has no bricks to render — the mob is the
        // right physical size, it simply has no sub-cell detail left.
        //
        // That is a legitimate state, not a rendering bug, so it SKIPS —
        // but only when expected. An empty list while the def still says
        // skinScale > 1 means the bricks went missing, which is exactly
        // what this probe exists to catch, and still fails.
        uint32_t critterSkin = 1;
        for (const MobDef& md : mobs.Defs())
          if (md.name == "critter") critterSkin = md.skinScale;
        const bool microExpected = critterSkin > 1;
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
        if (!microInsts.empty()) {
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

        const bool microSkipped = microInsts.empty() && !microExpected;
        bool microOk = microSkipped ||
                       (!microInsts.empty() && badDirs == 0 && soloBad == 0);
        if (microSkipped)
          std::printf("micro body render: SKIPPED (critter skinScale 1 at "
                      "%d world vox/m - art is 1:1 with the grid, so it "
                      "takes the cube path and has no bricks to probe)\n",
                      kVoxelsPerMetre);
        else
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
        // BLOOD IS COSMETIC FOR THIS BLOCK. Since Gore §F (2026-09-02) an
        // amputation opens a stump that drains hp until death, and a dummy
        // with four stumps bleeds out inside the 300-odd ticks below — which
        // turns "prone and still" into a ragdoll drifting 200 voxels and
        // reads as a locomotion failure. The claim here is which loco state
        // a maimed rig selects, not whether it survives its wounds
        // (`bleed-out` owns that), so the rate is parked at zero and put
        // back afterwards.
        const Tuning savedGore = CurrentTuning();
        {
          Tuning tt = savedGore;
          tt.gore.bleedHpPerVoxel = 0.0f;
          SetCurrentTuning(tt);
        }
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
        SetCurrentTuning(savedGore);
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
        // ---- the rear release band (avatar.headLookReleaseYaw) ----
        // Third person orbits the camera all the way round a body that faces
        // its travel direction, so the look delta reaches 180. Clamping alone
        // makes every angle past the cone the same pose — head pinned at its
        // stop — and dead behind is exactly where you want the character's own
        // forward pose instead. Assert the SHAPE of the band, not its width:
        // still craning outside it, near neutral at 180, and symmetric across
        // the wrap (both signs of 175 must agree, or the goal snaps as the
        // camera crosses straight-behind).
        auto lookSettle = [&](float deg) {
          avatar.SetLook(deg / 57.29578f, 0.0f);
          for (int i = 0; i < 40; i++) avTick();
          return relHeadYaw() - headRest;
        };
        const float headOutside = lookSettle(120.0f);  // band starts at 130
        const float headBehind = lookSettle(179.0f);
        const float headBehindNeg = lookSettle(-179.0f);
        // RELATIVE to the craning pose, not an absolute "near zero". The idle
        // rig carries a standing bias of several degrees at this joint — the
        // +-60 pair above recovers +51 / -63 for the same 60 degrees asked —
        // so an absolute neutral bound would be gating that bias as much as
        // the release. A fraction of the crane is the actual claim: dead
        // behind, the head is holding the body's pose and not the camera's.
        bool releaseOk = std::fabs(headOutside) > 25.0f &&
                         std::fabs(headBehind) < 0.35f * std::fabs(headOutside) &&
                         std::fabs(headBehindNeg) < 0.35f * std::fabs(headOutside);
        bool lookOk = lookTurns && hipsHeld && releaseOk;
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
        // ---- ...AND THE AIRBORNE POSE MUST BE A FUNCTION OF vel.y ----------
        //
        // `legsNotInverted` above is a SAFETY bound: it says the legs did not
        // fold through the pelvis. It passes just as happily on a body that
        // does nothing at all in the air, which is what the old pose was —
        // a 900 ms looping clip swaying the arms ten degrees over the rest
        // hang the leg IK had faded out of ("just wobbles left and right
        // slightly"). Nothing in it knew which way the body was going.
        //
        // So assert the DIFFERENCE, which is the whole claim of avatar.airPose:
        // rising and falling are different shapes, in the right direction, by
        // an amount you could see. Two arms of one fixture, ~20 ticks apart —
        // the cheapest possible form of "it responds to the phase", and the
        // one thing no amount of extra clip authoring could fake.
        //
        //   rising  -> knees TUCKED (foot close under the hip), arms driven UP
        //   falling -> legs REACHING down, arms out WIDE to brace
        //
        // Measured in model space off each limb's own joint, so the body's
        // world position and the fixture's teleporting have no vote.
        float tuckDrop = 0, reachDrop = 0;    // hip.y - foot.y, mean of 2 legs
        float tuckHandUp = 0, reachHandUp = 0;   // hand.y - shoulder.y, mean
        // THE COUNTER-SWING, as the fore/aft GAP between the two hands. This
        // is the one number that says the arms are doing opposite things:
        // averages cannot see it (they cancel), and a per-arm height cannot
        // either. It is also the assertion that would fail if `armSwing` ever
        // stopped being signed per arm, which is the way this regresses.
        float tuckHandSplit = 0, reachHandSplit = 0;  // |hand.z(L) - hand.z(R)|
        float restDrop = 0;                   // the same drop, standing
        {
          const int hipParts[2] = {avatar.PartIndex("legU.L"),
                                   avatar.PartIndex("legU.R")};
          const int feet[2] = {avatar.PartIndex("foot.L"),
                               avatar.PartIndex("foot.R")};
          const int shoulders[2] = {avatar.PartIndex("armU.L"),
                                    avatar.PartIndex("armU.R")};
          const int hands[2] = {avatar.PartIndex("hand.L"),
                                avatar.PartIndex("hand.R")};
          auto sampleLegs = [&](float& drop) {
            drop = 0;
            int n = 0;
            for (int s = 0; s < 2; s++) {
              Vec3 hp, fp;
              Quat hq, fq;
              if (hipParts[s] < 0 || feet[s] < 0) continue;
              if (!avatar.PartModelTransform(hipParts[s], hp, hq)) continue;
              if (!avatar.PartModelTransform(feet[s], fp, fq)) continue;
              drop += hp.y - fp.y;
              n++;
            }
            if (n) drop /= (float)n;
          };
          auto sampleArms = [&](float& up, float& split) {
            up = split = 0;
            int n = 0;
            float z[2] = {0, 0};
            for (int s = 0; s < 2; s++) {
              Vec3 sp, hp;
              Quat sq, hq;
              if (shoulders[s] < 0 || hands[s] < 0) continue;
              if (!avatar.PartModelTransform(shoulders[s], sp, sq)) continue;
              if (!avatar.PartModelTransform(hands[s], hp, hq)) continue;
              up += hp.y - sp.y;
              z[s] = hp.z;
              n++;
            }
            if (n == 2) split = std::fabs(z[0] - z[1]);
            if (n) up /= (float)n;
          };
          // The standing reference, taken BEFORE anything leaves the ground:
          // every bound below is a fraction of this rig's own hanging leg, so
          // the thresholds follow the art the way the gait's do.
          sampleLegs(restDrop);

          // ---- arm 1: RISING ----
          // A real launch, not a teleport: the player's own jumpSpeed, applied
          // as both the velocity the pose reads and the travel it implies, so
          // the body is genuinely going up while it claims to be. The pose is
          // driven by vel.y alone, but a fixture whose position disagreed with
          // its velocity would be lying to every OTHER rule in the avatar (the
          // gait's coyote distance, the landing probe) and the failure would
          // land somewhere unrelated.
          pl.grounded = false;
          pl.vel.x = 0;
          pl.vel.z = 0;
          // Put the body back where the rise started before handing over. Every
          // fixture after this one inherits pl.pos, and a jump at this rig's
          // real jumpSpeed covers ~77 voxels in 22 ticks — enough to leave the
          // fall below, the ramp and the bumpy-incline pass all measuring a
          // body suspended somewhere new. Restoring it keeps this arm additive.
          const float yBeforeRise = pl.pos.y;
          const float kRise =
              CurrentTuning().player.jumpSpeed / kVoxelMeters;
          for (int i = 0; i < 22; i++) {
            pl.vel.y = kRise;
            pl.pos.y += kRise * kTickDt;
            avTick();
          }
          sampleLegs(tuckDrop);
          sampleArms(tuckHandUp, tuckHandSplit);

          // ---- arm 2: FALLING, AND FAR ENOUGH UP TO STILL BE FALLING ------
          //
          // MEASURED FROM THE TOP OF THE JUMP, not from the drop below. The
          // first version of this sampled 12 ticks into the existing fall loop,
          // which starts AT the shelf the body walked off — and avatar.cpp's
          // landing probe folds in the PREPARE shape over the last
          // airPoseLandHeight (1.6 m, so 32 voxels at 5 cm) before contact. The
          // body was inside that the whole way down, so the arm labelled
          // "falling" was in fact the landing pose: it reported hands 0.6
          // voxels above the shoulder against the reach shape's authored 0.40
          // of an arm, and the lateral spread came out NARROWER than the tuck's
          // instead of much wider. The fixture was wrong, not the pose — but a
          // fixture that cannot tell "falling" from "about to land" is not
          // evidence for either, and it would have failed the arm assertion
          // while the thing under test was working.
          //
          // Up here the ground is ~70 voxels down, well past the probe's reach,
          // so this is the reach shape and nothing else.
          // At the tuning's OWN full fall speed, so this is the reach shape at
          // full commitment rather than some fraction of it — the phase is
          // `vel.y / airPoseFallSpeed`, so naming the same knob the pose reads
          // is what keeps the arm at 1.0 if that knob is ever retuned.
          const float kFall =
              CurrentTuning().avatar.airPoseFallSpeed / kVoxelMeters;
          for (int i = 0; i < 14; i++) {
            pl.vel.y = -kFall;
            pl.pos.y -= kFall * kTickDt * 0.1f;  // stay high; vel.y is the pose
            avTick();
          }
          sampleLegs(reachDrop);
          sampleArms(reachHandUp, reachHandSplit);

          pl.pos.y = yBeforeRise;
          // Land for a beat before handing over to the fall fixture below. Not
          // cosmetic: the air CLOCK is what sends a body limp
          // (ragdoll.fallSeconds, 3 s), and 36 ticks of jump plus the 45 the
          // next loop spends falling would put this fixture within a few ticks
          // of a ragdoll it is not trying to test.
          pl.grounded = true;
          pl.vel.y = 0;
          for (int i = 0; i < 5; i++) avTick();
          avatar.ClearFootfalls();   // the landing is not one of the gait's
        }
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

        // THE TWO SHAPES MUST DIFFER, AND IN THE RIGHT DIRECTION.
        //
        // Bounds are fractions of THIS rig's own standing leg (restDrop), never
        // voxel literals: the pose table is authored in leg lengths for exactly
        // that reason, and a hardcoded voxel bound would silently mean a
        // different pose at a different kVoxelMeters or on a different rig.
        //
        // The margins are wide on purpose. The claim is "the body answers the
        // phase", not "the tuck is 0.46 of a leg" — pinning the authored value
        // here would make every future tweak of the shapes a test edit, which
        // is the closed-ended-system failure DESIGN.md warns about. What it
        // catches is the thing that actually regresses: the drive coming
        // unhooked (both arms identical), or its sign inverting.
        const bool haveAirRef = restDrop > 1e-3f;
        const bool tucks = haveAirRef && tuckDrop < restDrop * 0.80f;
        const bool reaches = haveAirRef && reachDrop > tuckDrop * 1.25f;
        // The arms: driven UP at the launch, out WIDE on the way down. Measured
        // against each other rather than against a standing pose, because the
        // idle arms sit at whatever the rig authored and that is not a fact
        // this test should own.
        //
        // TWO AXES OF THE SAME DRIVE, and each catches a failure the other
        // cannot: the arms COUNTER-SWING widest at the launch (that is the
        // instant they were actually driving) and are carried HIGHER on the way
        // down (the brace). A swing that stopped being signed per arm collapses
        // the split without touching the mean height; an arm pose that stopped
        // reading the phase at all holds both.
        const bool armsDrive =
            tuckHandSplit > reachHandSplit + restDrop * 0.05f &&
            reachHandUp > tuckHandUp + restDrop * 0.05f;
        // With avatar.airPose OFF the shapes are the clip's, and the clip has
        // no idea which way the body is moving — so this whole block is the
        // air pose's own gate and is skipped rather than failed when it is off.
        const bool airPoseOn = CurrentTuning().avatar.airPose;
        const bool airPoseOk = !airPoseOn || (tucks && reaches && armsDrive);
        std::printf(
            "avatar air pose: %s (rest leg %.2f vox | tuck drop %.2f, reach "
            "drop %.2f | hand up %.2f -> %.2f, counter-swing %.2f -> %.2f)\n",
            !airPoseOn ? "off" : (airPoseOk ? "PASS" : "FAIL"), restDrop,
            tuckDrop, reachDrop, tuckHandUp, reachHandUp, tuckHandSplit,
            reachHandSplit);

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
            //
            // WITH avatar.airPose ON THERE ARE NO AIR CLIPS TO COUNT. The pose
            // is driven from vel.y through the IK chains and neither clip is
            // ever started, so a check phrased against them is green by
            // construction — the coverage has to move to the thing that poses
            // the body now. `AirPoseWeight` is the same claim about the same
            // event: cresting a step must not convince the rig it is airborne.
            if (avatar.ClipActive("jump")) rampJumpTicks++;
            if (avatar.ClipWeight("fall") > 0.05f) rampFallTicks++;
            if (avatar.AirPoseWeight() > 0.05f) rampFallTicks++;
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
                    rampWalks && rampNoAirClips && airPoseOk;
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
            "hips held %.1f deg; rear release: 120 -> %+.1f still craning, "
            "179 -> %+.1f, -179 -> %+.1f both back to forward)\n",
            lookOk ? "PASS" : "FAIL", headPos, headNeg, hipsYaw, headOutside,
            headBehind, headBehindNeg);
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

        // ---- swing aim policy (ResolveSwingYaw / ResolveSwingBasis) ----
        // The sword's copy of the neck rule. Same shape as the head-look
        // assertions above: inside the cone the camera is the basis; past it
        // the offset pins at the cone; across the rear band it releases to
        // the body's forward from BOTH signs, so the +180/-180 wrap is
        // seamless. Pure, so it is driven directly. Tuned values: melee.aimYaw
        // 70, aimReleaseYaw 50 (band begins at 130).
        {
          const float kRad = 1.0f / 57.29578f;
          const float swIn = degOf(ResolveSwingYaw(30.0f * kRad));
          const float swPin = degOf(ResolveSwingYaw(100.0f * kRad));
          const float swEdge = degOf(ResolveSwingYaw(125.0f * kRad));
          const float swNegPin = degOf(ResolveSwingYaw(-100.0f * kRad));
          const float swBehind = degOf(ResolveSwingYaw(179.0f * kRad));
          const float swBehindNeg = degOf(ResolveSwingYaw(-179.0f * kRad));
          // 200 deg unwrapped must read as -160, on the far side of the band.
          const float swUnwrapped = degOf(ResolveSwingYaw(200.0f * kRad));
          const bool yawOk = std::fabs(swIn - 30.0f) < 0.05f &&
                             std::fabs(swPin - 70.0f) < 0.05f &&
                             std::fabs(swEdge - 70.0f) < 0.05f &&
                             std::fabs(swNegPin + 70.0f) < 0.05f &&
                             std::fabs(swBehind) < 1.0f &&
                             std::fabs(swBehindNeg) < 1.0f &&
                             swUnwrapped < -20.0f && swUnwrapped > -70.05f;

          // The basis: a pitched camera 100 deg round from the body. The
          // output must face body+70 in yaw, keep the camera's pitch exactly,
          // and stay an orthonormal right-handed frame; inside the cone the
          // inputs come back bit-for-bit.
          auto headingBasis = [](float h, float pitch, Vec3& r, Vec3& u,
                                 Vec3& f) {
            const float cp = std::cos(pitch);
            f = Vec3{std::sin(h) * cp, std::sin(pitch), std::cos(h) * cp};
            r = f.cross(Vec3{0, 1, 0}).normalized();
            u = r.cross(f).normalized();
          };
          const float body = 0.4f, pitch = -0.3f;
          Vec3 cr, cu, cf, r, u, f;
          headingBasis(body + 100.0f * kRad, pitch, cr, cu, cf);
          ResolveSwingBasis(body + 100.0f * kRad, body, cr, cu, cf, r, u, f);
          const float outHeadErr =
              std::fabs(wrapDeg(degOf(std::atan2(f.x, f.z) - body) - 70.0f));
          const float pitchErr = std::fabs(f.y - cf.y);
          // |r x u . f| = 1, not r x u = f: Camera::Right is fwd x up, so the
          // engine's basis has right x up = -forward, and a rigid rotation
          // must preserve whichever handedness it was given.
          const float orthoErr = std::fabs(r.dot(f)) + std::fabs(u.dot(f)) +
                                 std::fabs(r.dot(u)) +
                                 std::fabs(std::fabs(r.cross(u).dot(f)) - 1.0f);
          Vec3 r2, u2, f2;
          headingBasis(body + 20.0f * kRad, pitch, cr, cu, cf);
          ResolveSwingBasis(body + 20.0f * kRad, body, cr, cu, cf, r2, u2, f2);
          const bool identity = r2.x == cr.x && r2.y == cr.y && r2.z == cr.z &&
                                u2.x == cu.x && u2.y == cu.y && u2.z == cu.z &&
                                f2.x == cf.x && f2.y == cf.y && f2.z == cf.z;
          const bool basisOk = outHeadErr < 0.1f && pitchErr < 1e-5f &&
                               orthoErr < 1e-4f && identity;
          const bool swingOk = yawOk && basisOk;
          std::printf(
              "avatar swing cone: %s (yaw 30 -> %+.1f, 100 -> %+.1f, 125 -> "
              "%+.1f, -100 -> %+.1f pinned; rear release 179 -> %+.1f, -179 "
              "-> %+.1f, 200 unwrapped -> %+.1f; basis 100 deg round: faces "
              "body+70 err %.2f deg, pitch err %.1e, ortho err %.1e, "
              "in-cone identity %s)\n",
              swingOk ? "PASS" : "FAIL", swIn, swPin, swEdge, swNegPin,
              swBehind, swBehindNeg, swUnwrapped, outHeadErr, pitchErr,
              orthoErr, identity ? "yes" : "NO");
          mobOk = mobOk && swingOk;
        }

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

            // ---- THE ARM IS AN ARM, AT EVERY TARGET THE MOUSE CAN NAME ----
            //
            // The weapon arm is driven by the MOUSE through a two-bone IK
            // chain, so unlike the legs it is aimed at arbitrary points on the
            // sphere — including behind the shoulder and across the chest. The
            // solver has no opinion about anatomy: handed such a point it
            // reaches it, raking the upper arm through the torso and opening
            // the elbow sideways. `poseLimit` on armU/armL is what forbids
            // that (anim.h PoseBallLimit / AnimPart::poseHinge), and this is
            // the only thing between it and the screen.
            //
            // WHY A FAN AND NOT ONE POSE. A single hostile target proves one
            // clamp fires. The failure being gated is a REGION of the input
            // space, and the two worst corners (down-and-back, up-and-across)
            // are not reachable by any one offset — the earlier version of the
            // ball clamp handled the pure-back case correctly and let the
            // back-and-across corner through, which no single-target test
            // could have seen.
            //
            // The bounds are READ OFF THE RIG, never restated here, exactly as
            // the hip/knee assertions above do it: a limit is authored data and
            // a test carrying its own copy is a second source of truth that
            // passes while measuring nothing.
            {
              const int shoulderPart = avatar.PartIndex("armU.R");
              const int elbowPart = avatar.PartIndex("armL.R");
              const float kDeg = 57.29578f;
              // Slack absorbs the ONE tick of lag between the clamp and this
              // readback plus the IK blend weight ramping; it is not a licence
              // for the joint to be a little bit illegal.
              const float kSlack = 2.0f;
              PoseBallLimit ball;
              bool haveBall = false, haveHinge = false;
              float hingeLo = 0, hingeHi = 0;
              Vec3 hingeAxis{1, 0, 0};
              if (shoulderPart >= 0 && shoulderPart < avatar.LimbCount()) {
                const MobLimbDef& ld = avatar.LimbDefAt(shoulderPart);
                ball = ld.poseBall;
                haveBall = ball.has && ball.reachCount == 2;
              }
              bool hingeEnforced = false;
              if (elbowPart >= 0 && elbowPart < avatar.LimbCount()) {
                const MobLimbDef& ld = avatar.LimbDefAt(elbowPart);
                // MEASURE whenever a limit exists; REQUIRE the hinge flag
                // separately. Keeping those apart is what makes the control
                // runnable without a rebuild: flip `hinge` to false in the
                // sidecar and this still reports how far out of plane the
                // solver takes the forearm, while correctly failing.
                haveHinge = ld.hasPoseLimit;
                hingeEnforced = ld.poseHinge;
                hingeAxis = ld.poseAxis.normalized();
                hingeLo = ld.poseMin * kDeg;
                hingeHi = ld.poseMax * kDeg;
              }
              // The part's rotation relative to its PARENT, which is the frame
              // a poseLimit is stated in (the rests on these rigs are identity,
              // so parent-relative and rest-relative coincide; measuring
              // parent-relative is what makes this independent of the clamp).
              auto relOf = [&](int part, int parent, Quat& out) {
                Vec3 p, pp;
                Quat q, pq;
                if (part < 0 || parent < 0) return false;
                if (!avatar.PartModelTransform(part, p, q)) return false;
                if (!avatar.PartModelTransform(parent, pp, pq)) return false;
                out = QuatNormalize(QuatMul(QuatConj(pq), q));
                return true;
              };
              // Hostile hand targets, shoulder-relative world voxels, in the
              // frame SetWeaponPose takes: +Z forward, +X the camera's right,
              // +Y up. Every one of these is a place a fast mouse flick
              // genuinely puts the target — behind, across, and the corners.
              const Vec3 kFan[] = {
                  {0, 0, -8},    {0, -6, -6},  {0, 6, -6},   {-9, 0, -4},
                  {9, 0, -4},    {-9, -5, 0},  {-9, 5, 2},   {-10, 0, 1},
                  {-7, -7, -5},  {-7, 7, -5},  {2, -9, -3},  {2, 9, -3},
                  {0, 0, 9},     {6, 6, 6},
              };
              // SCALED TO THE RIG'S OWN ARM, not left as world voxels.
              //
              // Two reasons, and the second is the one that cost a run. A
              // literal 9-voxel offset means a different fraction of an arm at
              // every kVoxelMeters, so this would have quietly become an
              // out-of-reach test the next time the voxel size moved. And an
              // out-of-reach target is not a hostile target at all: the
              // two-bone solver clamps to its reach annulus, and a clamped
              // two-bone solve is a DEAD STRAIGHT limb by construction (the
              // same trap the leg IK note above documents). The elbow then
              // reads ~0 at every target and the hinge assertion measures
              // nothing. 0.72 of full reach keeps the elbow honestly bent
              // while still putting the HAND well outside the shoulder's legal
              // cone, which is the input this is about.
              float armLen = 0.0f;
              {
                Vec3 ps, pe, ph;
                Quat qs, qe, qh;
                const int handPart2 = avatar.PartIndex("hand.R");
                if (avatar.PartModelTransform(shoulderPart, ps, qs) &&
                    avatar.PartModelTransform(elbowPart, pe, qe) &&
                    handPart2 >= 0 &&
                    avatar.PartModelTransform(handPart2, ph, qh))
                  armLen = (pe - ps).len() + (ph - pe).len();
              }
              const float fanScale =
                  armLen > 1e-3f ? armLen * 0.72f / 9.0f : 1.0f;
              // ABSOLUTE degrees past each plane, not the overshoot. The
              // overshoot alone reads 0.0 both when the clamp is holding the
              // joint exactly on its stop and when the limits have been opened
              // right out — the same number for "working" and "measuring" — so
              // the absolute is what the control run below actually needs.
              float worstBehind = -90.0f, worstAcross = -90.0f;
              float worstElbowLo = 999.0f, worstElbowHi = -999.0f;
              float worstOffHinge = 0.0f;
              int samples = 0;
              for (const Vec3& raw : kFan) {
                const Vec3 tgt = raw * fanScale;
                avatar.SetWeaponPose(tgt, Vec3{0, 0, 1}, Vec3{0, 1, 0}, 1.0f);
                for (int i = 0; i < 6; i++) avTick();
                Quat rel;
                if (haveBall && relOf(shoulderPart, avatar.PartIndex("torso"),
                                      rel)) {
                  // WHERE THE BONE POINTS, which is what the limit bounds —
                  // an angle about one axis cannot see "the arm is behind"
                  // once the shoulder has also abducted.
                  const Vec3 d = QuatRotate(rel, ball.bone);
                  for (int k = 0; k < 2; k++) {
                    const float past =
                        std::asin(std::clamp(d.dot(ball.reachNormal[k]), -1.0f,
                                             1.0f)) * kDeg;
                    float& worst = k == 0 ? worstBehind : worstAcross;
                    worst = std::max(worst, past);
                  }
                  samples++;
                }
                if (haveHinge && relOf(elbowPart, shoulderPart, rel)) {
                  // A HINGE HAS TO BE MEASURED AS A HINGE. The angle alone
                  // would pass on a forearm swung 40 degrees out of its plane
                  // with a legal in-plane component, which is exactly the
                  // sideways-elbow pose. So measure both: the bend, and how
                  // far the bone left the plane it is allowed to move in.
                  const Vec3 bone{0, -1, 0};
                  const Vec3 d = QuatRotate(rel, bone);
                  worstOffHinge =
                      std::max(worstOffHinge, std::fabs(d.dot(hingeAxis)) );
                  // Signed bend in the hinge plane, in the authored axis'
                  // own sense (positive = the direction min/max count in).
                  const Vec3 ref = bone;
                  const Vec3 perp = hingeAxis.cross(ref);
                  const float ang =
                      std::atan2(d.dot(perp), d.dot(ref)) * kDeg;
                  worstElbowLo = std::min(worstElbowLo, ang);
                  worstElbowHi = std::max(worstElbowHi, ang);
                }
              }
              avatar.SetWeaponPose(Vec3{}, Vec3{0, 0, 1}, Vec3{0, 1, 0}, 0.0f);
              for (int i = 0; i < 4; i++) avTick();

              const float behindLim =
                  haveBall ? std::asin(ball.reachSin[0]) * kDeg : 0.0f;
              const float acrossLim =
                  haveBall ? std::asin(ball.reachSin[1]) * kDeg : 0.0f;
              const bool armLimited =
                  haveBall && haveHinge && hingeEnforced &&
                  samples == (int)(sizeof(kFan) / sizeof(kFan[0])) &&
                  worstBehind <= behindLim + kSlack &&
                  worstAcross <= acrossLim + kSlack &&
                  worstElbowLo >= hingeLo - kSlack &&
                  worstElbowHi <= hingeHi + kSlack &&
                  // sin(2 degrees) — the forearm stays in its plane.
                  worstOffHinge <= 0.035f;
              std::printf(
                  "arm-limits: %s (%d targets at 0.72 of a %.1f vox arm; "
                  "shoulder reached %.1f deg behind vs stop %.0f, %.1f across "
                  "the midline vs stop %.0f; elbow %.1f..%.1f deg vs authored "
                  "%.0f..%.0f hinged=%d, worst off-hinge %.3f)\n",
                  armLimited ? "PASS" : "FAIL", samples, armLen, worstBehind,
                  behindLim, worstAcross, acrossLim, worstElbowLo,
                  worstElbowHi, hingeLo, hingeHi, hingeEnforced ? 1 : 0,
                  worstOffHinge);
              mobOk = mobOk && armLimited;

              // ---- arm-readback: SetWeaponPose's inverse actually inverts --
              //
              // Mob::WeaponArmPose reports where the weapon hand IS in the
              // frame SetWeaponPose SPEAKS, and the mouse-directed swing uses
              // it to take control of the arm without moving it (game/melee.h).
              // The yaw half of that conversion is invisible on a rig standing
              // at heading 0 facing the camera — exactly the case anybody
              // would eyeball. NOTE this is a ROUND TRIP, so it can only
              // assert the two directions AGREE, not that either is right:
              // the toRig x-mirror (removed 2026-09-01, see mob.cpp) passed
              // here for a month because its inverse rode in WeaponArmPose.
              //
              // So this asserts against a target the test chose, not against
              // the value under test: drive the hand to a known legal offset,
              // let it settle, and require the readback to agree. A dropped
              // yaw or a flipped x mirrors the answer and this fails by
              // roughly twice the offset.
              if (armLen > 1e-3f) {
                // Comfortably legal and comfortably reachable: forward, out to
                // the weapon side and a little down, at 0.6 of the arm. Nothing
                // here should be on a pose stop, or the settle below would
                // converge somewhere other than the target for a legitimate
                // reason and the comparison would mean nothing.
                const Vec3 want =
                    Vec3{0.35f, -0.45f, 0.60f}.normalized() * (armLen * 0.6f);
                avatar.SetWeaponPose(want, Vec3{0, 0, 1}, Vec3{0, 1, 0}, 1.0f);
                for (int i = 0; i < 12; i++) avTick();
                Vec3 got{};
                float gotReach = 0;
                const bool read = avatar.WeaponArmPose(got, gotReach);
                // Generous, and it has to be: the pose limits and one tick of
                // lag both move the hand a little off an unconstrained solve.
                // A sign error is a WHOLE-ARM error, so this catches those with
                // room to spare while never asserting the IK is exact.
                const float err = (got - want).len();
                const float tol = std::max(0.35f * armLen, 0.5f);
                const bool reachOk =
                    std::fabs(gotReach - armLen) < 0.05f * armLen;
                const bool roundTrip = read && err < tol && reachOk;
                std::printf(
                    "arm-readback: %s (asked (%.2f,%.2f,%.2f), read "
                    "(%.2f,%.2f,%.2f), err %.2f vox vs tol %.2f; reach %.2f vs "
                    "the rig's own %.2f)\n",
                    roundTrip ? "PASS" : "FAIL", want.x, want.y, want.z, got.x,
                    got.y, got.z, err, tol, gotReach, armLen);
                mobOk = mobOk && roundTrip;
                avatar.SetWeaponPose(Vec3{}, Vec3{0, 0, 1}, Vec3{0, 1, 0},
                                     0.0f);
                for (int i = 0; i < 4; i++) avTick();
              }
            }
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
                 mCloth = matId("cloth"),
                 mClothBurn = matId("cloth_burning"),
                 mClothChar = matId("cloth_charred"), mSkin = matId("skin"),
                 mCooked = matId("flesh_cooked"),
                 mCharred = matId("flesh_charred"),
                 mBurning = matId("flesh_burning"), mBlood = matId("blood"),
                 mCinder = matId("flesh_cinder"), mUnder = matId("linen"),
                 mUnderBurn = matId("linen_burning"),
                 mUnderChar = matId("linen_charred"),
                 // The anatomy layers under the skin (assets/editor/anatomy.js):
                 // body mass the fire has to get through, and flesh that can
                 // char. Bone is deliberately in neither census -- it has no
                 // fire rules, so counting it would only dilute both ratios.
                 mFlesh = matId("flesh"), mMuscle = matId("muscle"),
                 // Heat doing something that is NOT spreading fire, for the
                 // spread knob's exclusion probe in I.4.
                 mWater = matId("water"), mSteam = matId("steam");
  if (!mFire || !mAcid || !mCloth || !mSkin || !mCooked || !mBurning) {
    detail = "body-reactivity materials missing from materials.json";
    return Status::Fail;
  }
  if (!mCinder || !mUnder || !mUnderBurn || !mUnderChar) {
    detail = "linen / flesh_cinder missing from materials.json";
    return Status::Fail;
  }
  if (!mFlesh || !mMuscle) {
    detail = "anatomy materials flesh / muscle missing from materials.json";
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

  // ---- I. the three claims that live entirely in the COMPILED TABLE --------
  // No fixture, no ticks, no GPU: each of these is a statement about what
  // reactions.json compiled to, and asserting it here costs microseconds where
  // reproducing it through a burning creature costs hundreds of ticks and
  // could only ever show it happening rather than show it being impossible.
  {
    // ---- I.1 the burn-duration multiplier reaches the compiler -------------
    // combustion.burnDurationPct is folded into the authored chance at LOAD
    // (sim/materials.cpp), so nothing downstream can see it and --sweep — which
    // only reloads shaders — cannot reach it either. The differential is
    // therefore taken where the knob is spent: compile the same two files
    // twice, at 100% and at 200%, and compare the tables.
    //
    // Two halves, and the second is the one worth having. That SOMETHING moved
    // proves the knob is wired; that everything which moved moved by EXACTLY
    // the factor proves it did not also quietly rescale an ignition or an emit
    // rule, which is the failure that would look like a working feature and
    // play like a different game.
    const std::string mp = AssetDir() + "/materials/materials.json";
    const std::string rp = AssetDir() + "/materials/reactions.json";
    const Tuning saved = CurrentTuning();
    std::vector<MaterialDef> m1, m2;
    std::vector<ReactionGpu> r1, r2;
    std::string e1, e2;
    Tuning t1 = saved;
    t1.combustion.burnDurationPct = 100;
    SetCurrentTuning(t1);
    const bool load1 = LoadAssets(mp, rp, m1, r1, e1);
    Tuning t2 = saved;
    t2.combustion.burnDurationPct = 200;
    SetCurrentTuning(t2);
    const bool load2 = LoadAssets(mp, rp, m2, r2, e2);
    SetCurrentTuning(saved);

    uint32_t scaled = 0, wrongFactor = 0;
    bool sameShape = load1 && load2 && r1.size() == r2.size();
    if (sameShape) {
      for (size_t i = 0; i < r1.size(); i++) {
        // Everything but the chance must be untouched: this knob rewrites a
        // rate, never a rule.
        if (r1[i].packed != r2[i].packed || r1[i].prodSelf != r2[i].prodSelf ||
            r1[i].prodNbr != r2[i].prodNbr || r1[i].cond != r2[i].cond) {
          sameShape = false;
          break;
        }
        if (r1[i].chance == r2[i].chance) continue;
        scaled++;
        // Halved, up to the round-half-up the compiler applies once.
        const uint32_t want = (r1[i].chance + 1u) / 2u;
        if (r2[i].chance + 1u < want || r2[i].chance > want + 1u) wrongFactor++;
      }
    }
    const bool i1 = sameShape && scaled > 0 && wrongFactor == 0;
    std::printf("  burn duration knob: %s (%u rules halved at 200%%, %u by the "
                "wrong factor; live setting %d%%)\n",
                i1 ? "PASS" : "FAIL", scaled, wrongFactor,
                saved.combustion.burnDurationPct);
    if (!i1 && (!load1 || !load2))
      std::printf("    reload failed: %s%s\n", e1.c_str(), e2.c_str());
    std::fflush(stdout);
    ok = ok && i1;

    // ---- I.4 the SPREAD knob scales ignition and ONLY ignition -------------
    //
    // combustion.spreadPct is burnDurationPct's twin -- that one is how long a
    // lit voxel stays lit, this one is how readily the voxel beside it catches
    // -- and it is folded into the authored chance by the same compiler at the
    // same point, so it is provable the same way and for microseconds: compile
    // the table twice and compare.
    //
    // THE EXCLUSIONS ARE THE HALF WORTH HAVING, exactly as in I.1. That some
    // chance moved proves the knob is wired. What matters is that no EMIT rule
    // moved (a slower fire must not also be a dimmer one), that nothing marked
    // burnDuration moved (each rule has exactly one owner among the two knobs,
    // or the retire/relight loop gets scaled twice by two sliders that look
    // independent in the tuner), and that HEAT'S OTHER JOBS did not move --
    // water steaming against a flame is a hot neighbour doing something that
    // is not spreading fire, and it is the case that separates "scale the
    // spread" from "scale everything a hot cell touches".
    //
    // THE SEAR MUST MOVE WITH THE REST, and this asserts that rather than the
    // reverse, which is the correction that cost a run. skin -> flesh_cooked
    // produces a material that is not a heat source, so the first draft of the
    // knob left it at the authored rate -- and a body then browned at four
    // times the rate the cloth over it caught, inverting the "cloth catches at
    // about eight times flesh's rate" comparison the whole body-burn section
    // of reactions.json is built on. The B subtest caught it as skin halving
    // at t+31 against cloth's t+124. Pinned here as well as there because this
    // costs microseconds and that costs 150 ticks.
    {
      const Tuning saved2 = CurrentTuning();
      std::vector<MaterialDef> ma, mb;
      std::vector<ReactionGpu> ra, rb;
      std::string ea, eb;
      Tuning ta = saved2;
      ta.combustion.spreadPct = 100;
      SetCurrentTuning(ta);
      const bool loadA = LoadAssets(mp, rp, ma, ra, ea);
      Tuning tb = saved2;
      tb.combustion.spreadPct = 50;
      SetCurrentTuning(tb);
      const bool loadB = LoadAssets(mp, rp, mb, rb, eb);
      SetCurrentTuning(saved2);

      uint32_t moved = 0, wrongFactor = 0, emitMoved = 0, durMoved = 0;
      bool shape = loadA && loadB && ra.size() == rb.size();
      // Both probe rules are found by what they DO rather than by an index: an
      // index would silently follow the wrong rule the first time anyone
      // inserts one above it. The sear is the rule in skin's bucket whose
      // product is flesh_cooked; the steam rule is the one in water's bucket
      // whose product is steam.
      int searA = -1, searB = -1, steamA = -1, steamB = -1;
      auto findRule = [](const std::vector<MaterialDef>& mm,
                         const std::vector<ReactionGpu>& rr, uint32_t self,
                         uint32_t prod) {
        if (self == 0 || self >= mm.size()) return -1;
        const MaterialGpu& g = mm[self].gpu;
        for (uint32_t i = 0; i < g.reactCount; i++)
          if ((rr[g.reactOffset + i].prodSelf & 0xFFFu) == prod)
            return (int)(g.reactOffset + i);
        return -1;
      };
      if (shape) {
        searA = findRule(ma, ra, mSkin, mCooked);
        searB = findRule(mb, rb, mSkin, mCooked);
        steamA = findRule(ma, ra, mWater, mSteam);
        steamB = findRule(mb, rb, mWater, mSteam);
        for (size_t i = 0; i < ra.size(); i++) {
          if (ra[i].packed != rb[i].packed || ra[i].prodSelf != rb[i].prodSelf ||
              ra[i].prodNbr != rb[i].prodNbr || ra[i].cond != rb[i].cond) {
            shape = false;
            break;
          }
          if (ra[i].chance == rb[i].chance) continue;
          moved++;
          if ((ra[i].packed & 3u) == kReactEmit) emitMoved++;
          const uint32_t want = (ra[i].chance + 1u) / 2u;
          if (rb[i].chance + 1u < want || rb[i].chance > want + 1u) wrongFactor++;
        }
        // Nothing the OTHER knob owns may move with this one. Taken by
        // re-compiling at a different burnDurationPct and checking the rules
        // this knob moved are disjoint from the rules that one moves.
        std::vector<MaterialDef> mc;
        std::vector<ReactionGpu> rc;
        std::string ec;
        Tuning tc = saved2;
        tc.combustion.spreadPct = 100;
        tc.combustion.burnDurationPct = saved2.combustion.burnDurationPct * 2;
        SetCurrentTuning(tc);
        const bool loadC = LoadAssets(mp, rp, mc, rc, ec);
        SetCurrentTuning(saved2);
        if (loadC && rc.size() == ra.size())
          for (size_t i = 0; i < ra.size(); i++)
            if (ra[i].chance != rb[i].chance && ra[i].chance != rc[i].chance)
              durMoved++;
      }
      const bool searScaled = searA >= 0 && searB >= 0 &&
                              ra[searA].chance != rb[searB].chance;
      // Water is allowed to be missing from a stripped material set; what is
      // not allowed is for it to be there and to have moved.
      const bool steamHeld = steamA < 0 || steamB < 0 ||
                             ra[steamA].chance == rb[steamB].chance;
      const bool i4 = shape && moved > 0 && wrongFactor == 0 &&
                      emitMoved == 0 && durMoved == 0 && searScaled && steamHeld;
      std::printf(
          "  spread knob: %s (%u ignition rules halved at 50%%, %u by the "
          "wrong factor, %u emit, %u shared with burn duration; sear %s, steam "
          "%s; live setting %d%%)\n",
          i4 ? "PASS" : "FAIL", moved, wrongFactor, emitMoved, durMoved,
          searScaled ? "scaled" : "HELD", steamHeld ? "held" : "MOVED",
          saved2.combustion.spreadPct);
      if (!i4 && (!loadA || !loadB))
        std::printf("    reload failed: %s%s\n", ea.c_str(), eb.c_str());
      std::fflush(stdout);
      ok = ok && i4;
    }

    // ---- I.5 the FLAME knob scales the exception and only the exception ----
    //
    // combustion.flamePct is the global strength over the per-rule
    // neighborChance exceptions reactions.json authors for the drifting flame.
    // Provable in the compiled table for microseconds, like its two
    // neighbours: halve the knob and every fire-exception rule must halve,
    // while the rule it is an exception TO -- the tag rule the coals match --
    // must not move at all. That second half is the one worth having. The
    // exception is compiled as a synthetic hot-minus-fire tag plus an exact
    // fire rule, and a knob that moved both would not be making the flame
    // weaker, it would be making the whole fire weaker and duplicating
    // spreadPct under a second name.
    {
      const Tuning saved3 = CurrentTuning();
      std::vector<MaterialDef> mf1, mf2;
      std::vector<ReactionGpu> rf1, rf2;
      std::string ef1, ef2;
      Tuning tf1 = saved3;
      tf1.combustion.flamePct = 100;
      SetCurrentTuning(tf1);
      const bool loadF1 = LoadAssets(mp, rp, mf1, rf1, ef1);
      Tuning tf2 = saved3;
      tf2.combustion.flamePct = 50;
      SetCurrentTuning(tf2);
      const bool loadF2 = LoadAssets(mp, rp, mf2, rf2, ef2);
      SetCurrentTuning(saved3);

      uint32_t flameMoved = 0, flameWrong = 0, coalsMoved = 0;
      bool shapeF = loadF1 && loadF2 && rf1.size() == rf2.size();
      if (shapeF) {
        const uint32_t mFire = matId("fire");
        for (size_t i = 0; i < rf1.size(); i++) {
          if (rf1[i].packed != rf2[i].packed ||
              rf1[i].prodSelf != rf2[i].prodSelf || rf1[i].cond != rf2[i].cond) {
            shapeF = false;
            break;
          }
          if (rf1[i].chance == rf2[i].chance) continue;
          // An exact-fire-neighbour rule is the exception; anything else that
          // moved is the knob reaching past what it owns.
          if (rf1[i].nbrMat == mFire && rf1[i].nbrTags == 0) {
            flameMoved++;
            const uint32_t want = (rf1[i].chance + 1u) / 2u;
            if (rf2[i].chance + 1u < want || rf2[i].chance > want + 1u)
              flameWrong++;
          } else {
            coalsMoved++;
          }
        }
      }
      const bool i5 = shapeF && flameMoved > 0 && flameWrong == 0 &&
                      coalsMoved == 0;
      std::printf("  flame knob: %s (%u fire-exception rules halved at 50%%, "
                  "%u by the wrong factor, %u non-exception rules moved; live "
                  "setting %d%%)\n",
                  i5 ? "PASS" : "FAIL", flameMoved, flameWrong, coalsMoved,
                  saved3.combustion.flamePct);
      if (!i5 && (!loadF1 || !loadF2))
        std::printf("    reload failed: %s%s\n", ef1.c_str(), ef2.c_str());
      std::fflush(stdout);
      ok = ok && i5;
    }

    // ---- I.2 A BURNT CHARACTER IS NEVER A NAKED ONE ------------------------
    // The base human's linen is a material, not a paint colour, precisely so
    // that burning it cannot expose skin — every reaction that rewrites a body
    // voxel clears its art colour, so a painted-on garment burns into whatever
    // the underlying material chars to and reads as bare flesh.
    //
    // Asserted as a REACHABILITY claim over the whole chain rather than as
    // "linen_burning has no air branch", because the hole could be opened
    // by any of the three materials, or by a fourth added between them later.
    // Nothing in the closure may produce air (prodSelf 0), and the closure must
    // terminate at linen_charred, which authors no rules at all.
    {
      std::vector<uint32_t> stack{mUnder}, seen{mUnder};
      bool leaks = false, closed = true;
      while (!stack.empty() && closed) {
        const uint32_t id = stack.back();
        stack.pop_back();
        const MaterialGpu& g = mats[id].gpu;
        for (uint32_t i = 0; i < g.reactCount; i++) {
          const ReactionGpu& rr = c.reactions[g.reactOffset + i];
          const uint32_t kind = rr.packed & 3u;
          // An EMIT rule's prodNbr goes into a NEIGHBOURING air cell (the
          // flame licking upward) and is not this voxel becoming anything —
          // only prodSelf is the chain.
          if (kind == kReactEmit && rr.prodSelf == kProdKeep) continue;
          if (rr.prodSelf == 0u) { leaks = true; break; }
          if (rr.prodSelf == kProdKeep) continue;
          const uint32_t p = rr.prodSelf & 0xFFFu;
          if (std::find(seen.begin(), seen.end(), p) != seen.end()) continue;
          seen.push_back(p);
          stack.push_back(p);
        }
      }
      // Every state the linen can reach must be one of its own three, or the
      // chain has escaped into a material with different (consumable) rules.
      for (uint32_t s : seen)
        if (s != mUnder && s != mUnderBurn && s != mUnderChar) closed = false;
      const bool terminal = mats[mUnderChar].gpu.reactCount == 0;
      const bool i2 = !leaks && closed && terminal;
      std::printf("  linen chars, never bares: %s (%zu states reachable, "
                  "air branch %s, terminus %s)\n",
                  i2 ? "PASS" : "FAIL", seen.size(), leaks ? "PRESENT" : "none",
                  terminal ? "inert" : "still reactive");
      std::fflush(stdout);
      ok = ok && i2;
    }

    // ---- I.3 deep char exists and ENDS ------------------------------------
    // flesh_cinder is the stage past flesh_charred, and the reason it is a
    // material rather than a palette jitter is that it has no rules — a torched
    // corpse is permanently black AND permanently settled. A cinder that could
    // re-ignite would be a corpse whose chunk never sleeps (rule 2), so the
    // "authors nothing" half is the load-bearing one.
    {
      bool reachable = false;
      const MaterialGpu& g = mats[mCharred].gpu;
      for (uint32_t i = 0; i < g.reactCount; i++)
        if ((c.reactions[g.reactOffset + i].prodSelf & 0xFFFu) == mCinder)
          reachable = true;
      const bool inert = mats[mCinder].gpu.reactCount == 0;
      const bool i3 = reachable && inert;
      std::printf("  deep char: %s (charred -> cinder %s, cinder %s)\n",
                  i3 ? "PASS" : "FAIL", reachable ? "authored" : "MISSING",
                  inert ? "inert" : "STILL REACTIVE");
      std::fflush(stdout);
      ok = ok && i3;
    }
  }

  int wizDef = -1;
  for (size_t i = 0; i < mobs.Defs().size(); i++)
    if (mobs.Defs()[i].name == "wizard") wizDef = (int)i;
  if (wizDef < 0) {
    detail = "no wizard def (the fixture: cloth over skin on one rig)";
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

  // ---- FIRE FIXTURES ARE SIZED IN TICKS, AND THE SPREAD KNOB IS A CLOCK ----
  //
  // Every window below was chosen against the ignition chances reactions.json
  // authors, i.e. against combustion.spreadPct = 100. That knob multiplies
  // exactly those chances, so at the shipped 12% a fire needs about eight
  // times as long to travel the same distance and every fixed window silently
  // becomes a deadline that tightens as the knob comes down. It failed that
  // way on the first run at 12%: the player's 90-tick immersion scored 6.7%
  // past the sear against a 15% floor, which reads as "the body barely burns"
  // and is really "you gave an eight-times-slower fire the same wall clock".
  //
  // The same correction kQuiet already makes for burnDurationPct, for the same
  // reason and with the same shape. A claim about how far a burn GETS is a
  // claim about the burn, not about the setting of a slider.
  const int spreadScale =
      std::max(1, 100 / std::max(1, CurrentTuning().combustion.spreadPct));

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

    // Split by material, because "13 voxels still alight" is a bare count and
    // a bare count buys one hypothesis per run (CLAUDE.md rule 6). WHICH
    // material is still lit says which relight loop failed to converge: cloth
    // and flesh have separate charred -> burning rules with different chances
    // and different minCounts, and the combustion clock scales both. It paid
    // for itself on its first run: "13 still alight" at 200% was 13 CLOTH and 0
    // flesh, and still falling — a longer tail from a longer burn, not the
    // relight loop refusing to converge, which is what the bare count had
    // looked like and would have been diagnosed as.
    auto alightCloth = [&]() {
      return (mobs.IsAlive(id) ? census(id, mClothBurn) : 0u) +
             debris.TotalBodyMaterial(mClothBurn);
    };
    auto alightFlesh = [&]() {
      return (mobs.IsAlive(id) ? census(id, mBurning) : 0u) +
             debris.TotalBodyMaterial(mBurning);
    };
    auto alightInWorld = [&]() { return alightCloth() + alightFlesh(); };

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
    // THE QUIET WINDOW IS A DURATION, SO IT SCALES WITH THE BURN CLOCK. 630
    // ticks was chosen against the chances reactions.json authored, and
    // combustion.burnDurationPct multiplies exactly those — so a literal here
    // is a deadline that silently tightens every time the slider goes up. It
    // failed that way on the first run at 200%: 13 voxels still alight, which
    // reads as "the fire never went out" and is really "the fire takes twice
    // as long to go out and you gave it the same wall clock". The claim being
    // asserted is that the burn TERMINATES, and terminating twice as slowly is
    // the feature working, not a regression.
    const int kQuiet = 630 * CurrentTuning().combustion.burnDurationPct / 100;
    for (int i = 0; i < kQuiet; i++) {
      burnTick(id, 0, 0);
      peakAlight = std::max(peakAlight, alightInWorld());
      if (deathTick < 0 && !mobs.IsAlive(id)) deathTick = i;
    }
    countBlood = false;
    const bool died = deathTick >= 0;
    const float worstSever = mobs.WorstSeverFraction();
    const std::string worstName = mobs.WorstSeverLimb();

    // ---- TERMINATION IS CONVERGENCE, NOT A DEADLINE ------------------------
    //
    // The window above is a duration and already scales with
    // combustion.burnDurationPct, for the reason recorded at kQuiet. It now
    // has a SECOND clock over it: combustion.spreadPct decides how fast the
    // front travels, so at the shipped 12% the fire spends about eight times
    // as long crawling over the body before there is nothing left to reach.
    // Measured at 12%: 172 cloth voxels still alight at the deadline and zero
    // 630 ticks later -- the burn was working exactly as authored and the
    // deadline was the thing that was wrong.
    //
    // Scaling the window by both knobs would make it ~10,000 ticks and this
    // subtest the most expensive thing in the suite, to assert something it
    // can assert directly instead. What rule 2 actually requires is that the
    // process TERMINATES, and the difference between "slow" and "never" is
    // whether the count is still falling -- which the attribution branch below
    // was already computing, after the fact, to explain the failure. So it is
    // the assertion now: keep giving the fire more quiet ticks while it is
    // strictly converging, and fail only on a PLATEAU, which is a relight loop
    // whose gain has reached 1 and would never converge at any window size.
    uint32_t extraTicks = 0;
    {
      const int kRound = 315;   // a quarter of the base window
      const int kRounds = 8;    // hard cap: bounded like everything else here
      uint32_t prev = alightInWorld();
      for (int rd = 0; rd < kRounds && prev > 0; rd++) {
        for (int i = 0; i < kRound; i++) burnTick(id, 0, 0);
        extraTicks += (uint32_t)kRound;
        const uint32_t now = alightInWorld();
        if (now >= prev) break;  // plateau: stop and let the assertion fail
        prev = now;
      }
    }
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
        "  burn leaves char: %s (%u lit, peak %u alight -> %u after %d quiet "
        "ticks + %u converging, %u charred; worst sever %s at %.0f%% of spawn "
        "volume, floor 50%%; %s)\n",
        hOk ? "PASS" : "FAIL", lit, peakAlight, stillAlight, kQuiet, extraTicks,
        charred,
        worstSever < 0.0f ? "(nothing severed)" : worstName.c_str(),
        worstSever < 0.0f ? 0.0f : worstSever * 100.0f,
        died ? ("creature died at t+" + std::to_string(deathTick)).c_str()
             : "creature survived");
    // Attribution at the point of failure: name the material that would not go
    // out, and say whether the remainder is still FALLING or has settled into a
    // steady state. Those are different bugs — a long tail is a clock that
    // wants more ticks, a plateau is a relight loop whose gain has reached 1
    // and will never converge (CLAUDE.md rule 2) — and the count alone cannot
    // tell them apart, which is what cost this line a run.
    if (!out) {
      const uint32_t stuckCloth = alightCloth(), stuckFlesh = alightFlesh();
      for (int i = 0; i < kQuiet / 2; i++) burnTick(0, 0, 0);
      std::printf("    still alight: %u cloth + %u flesh; %u after %d further "
                  "quiet ticks (%s)\n",
                  stuckCloth, stuckFlesh, alightInWorld(), kQuiet / 2,
                  alightInWorld() < stillAlight ? "still falling"
                                                : "PLATEAU — loop gain >= 1");
    }
    std::fflush(stdout);
    ok = ok && hOk;

    // ---- claim 3: FIRE CAUTERISES ------------------------------------------
    //
    // A creature burning to death, for the whole quiet window, must not
    // produce ONE drop of
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
        "  fire does not bleed: %s (%u blood droplets, %u of %d ticks with an "
        "open wound)\n",
        dryOk ? "PASS" : "FAIL", burnBloodDrops, burnBleedTicks, kQuiet);
    std::fflush(stdout);
    ok = ok && dryOk;
    mobs.Reset();
    debris.Reset();
  }


  // ---- J. HEAT CROSSES A JOINT --------------------------------------------
  //
  // Owner report, 2026-09-03: "setting a mob on fire leads to their legs never
  // catching on fire". The cause was structural, not a rate: a creature's
  // limbs are separate lattices that cannot see each other and a mob is not in
  // the grid, so the only channel from a burning hip to the thigh under it was
  // the `fire` gas the hip emits -- and `fire` rises with probability 1 in
  // calm air, so a flame emitted downward floats back up through the limb that
  // made it. Mob::BuildCrossLimbHeat gives every limb the world cells its
  // SIBLINGS are alight in, and combustion.crossLimbPct is what a rule armed
  // only by one of those pays.
  //
  // A TWO-ARM DIFFERENTIAL, because the single-arm version of this test is
  // worthless: "the legs burned" passes just as well if the world fire, the
  // emitted flame or the fixture's own ground lit them. The control arm sets
  // crossLimbPct to 0, which is precisely the old behaviour, and the claim is
  // the DIFFERENCE. Both arms are the same seed, the same rig, the same
  // ignition and the same tick count, so nothing else can account for it.
  //
  // NO WORLD FIRE AT ALL. The soak box the earlier subtests use would light
  // the legs directly and there would be nothing left to measure; the whole
  // point is a fire that starts on ONE limb and has to travel.
  //
  // The knob is read LIVE by the burn pass, not folded in at load like its
  // neighbour burnDurationPct, so flipping CurrentTuning between the arms is
  // enough -- no material recompile, no second binary.
  {
    int hipLimb = -1;
    std::vector<int> legLimbs;
    for (int li = 0; li < nLimbs; li++) {
      const std::string& n = mobs.Defs()[wizDef].limbs[li].name;
      if (n == "hips") hipLimb = li;
      // The thighs ONLY. A foot is two joints away, so including it would
      // measure how far the fire travels rather than whether it crosses at
      // all, and would fail for a reason the differential cannot name.
      if (n == "legU.L" || n == "legU.R") legLimbs.push_back(li);
    }
    if (hipLimb < 0 || legLimbs.empty()) {
      std::printf("  heat across a joint: FAIL (rig has no hips/legU.* to "
                  "measure; found %d limbs)\n", nLimbs);
      std::fflush(stdout);
      ok = false;
    } else {
      // RAW material on the thighs: everything fire has not touched yet.
      // Counting what is LEFT rather than what appeared is what makes the
      // measure blind to which way the chain went -- cloth to cloth_burning,
      // skin to flesh_cooked, or a voxel burnt clean away all count the same,
      // and a future material in the chain needs no edit here.
      auto legRaw = [&](uint64_t id) {
        uint32_t n = 0;
        for (int li : legLimbs)
          n += mobs.LimbMaterialCount(id, li, mCloth) +
               mobs.LimbMaterialCount(id, li, mSkin) +
               mobs.LimbMaterialCount(id, li, mUnder);
        return n;
      };
      auto legAlight = [&](uint64_t id) {
        uint32_t n = 0;
        for (int li : legLimbs) n += mobs.LimbBurningCount(id, li);
        return n;
      };

      const Tuning savedT = CurrentTuning();
      // A SUSTAINED SOURCE, not a longer window, and the distinction is what
      // keeps this subtest affordable. One flash of ignition on the hips is
      // not a fire a thigh can catch from at the shipped spread rate: 120 lit
      // cloth voxels each burn out in ~50 ticks and light well under one
      // neighbour apiece on the way, so the source is gone before the joint
      // has had many rolls. Measured that way at spreadPct 12: 2 thigh voxels
      // touched against the control arm's 5, i.e. pure noise. Re-lighting the
      // hips on a fixed cadence models what a creature actually on fire looks
      // like -- something keeps the torso burning -- and holds the SOURCE
      // constant so the thing being measured is the CROSSING. Applied
      // identically in both arms, so it cannot manufacture the differential.
      const int kCrossTicks = 300;
      const int kRelight = 30;
      struct Arm {
        uint32_t touched = 0, alight = 0, lit = 0, raw0 = 0;
        uint32_t cells = 0, faces = 0, scaled = 0;
      };
      Arm arms[2];
      const int pcts[2] = {savedT.combustion.crossLimbPct, 0};
      for (int a = 0; a < 2; a++) {
        Tuning t = savedT;
        t.combustion.crossLimbPct = pcts[a];
        // THE DEATH KNOT IS PARKED, the same way the player immersion below
        // parks it and for the same reason: this fixture holds a fire on the
        // hips for three hundred ticks and the wizard dies of it, at which
        // point the mob is gone and every per-limb census reads zero. Both
        // arms then scored "all 558 thigh voxels touched" and the differential
        // said 1x -- a measurement of the corpse, not of the crossing. Death
        // by burns has its own gate (`burn-cap`); this one needs the body to
        // stay to be counted.
        t.gore.burnDeathFraction = 1.0f;
        SetCurrentTuning(t);
        debris.Reset();
        mobs.Reset();
        SubmitWorldgen(ctx, world, sim, kDefaultSeed);
        ctx.WaitIdle();
        const IVec3 site = fixture(260);
        const int h = World::TerrainHeight(site.x, site.z, kDefaultSeed);
        pchunk = IVec3{site.x >> 4, h >> 4, site.z >> 4};
        const uint64_t id = mobs.Spawn(wizDef, {site.x, h + 1, site.z});
        if (!id) continue;
        for (int i = 0; i < 12; i++) burnTick(id, 0, 0);  // settle onto ground
        arms[a].raw0 = legRaw(id);
        // Light the HIPS and nothing else. Cloth first because skin cannot be
        // lit directly at all -- its rule products (flesh_cooked) carry no
        // tag:hot, so IgnitedForm refuses it, which is the sear being a colour
        // change rather than combustion.
        arms[a].lit = mobs.IgniteLimb(id, hipLimb, 120u, mCloth);
        if (arms[a].lit == 0) arms[a].lit = mobs.IgniteLimb(id, hipLimb, 120u);
        mobs.ResetBurnStats();
        // SAMPLED EVERY TICK AND KEPT AT ITS MINIMUM, never read at the end.
        // With the knot parked the creature should survive, but "should" is
        // not a thing to build a measurement on: if it dies anyway the census
        // goes to zero and an end-state read reports the maximum possible
        // score for the worst possible reason. The most-burnt state actually
        // observed while the body was there to observe is the honest number,
        // and it is the same reasoning the B subtest gives for sampling the
        // halving tick rather than the end state.
        uint32_t peakAlight = 0, minRaw = arms[a].raw0;
        for (int i = 0; i < kCrossTicks; i++) {
          if (i % kRelight == 0) mobs.IgniteLimb(id, hipLimb, 120u, mCloth);
          burnTick(id, 0, 0);
          if (!mobs.IsAlive(id)) break;
          peakAlight = std::max(peakAlight, legAlight(id));
          minRaw = std::min(minRaw, legRaw(id));
        }
        arms[a].touched = arms[a].raw0 > minRaw ? arms[a].raw0 - minRaw : 0u;
        arms[a].alight = peakAlight;
        const MobSystem::BurnStats& bs = mobs.Burn();
        arms[a].cells = bs.crossCells;
        arms[a].faces = bs.crossFaces;
        arms[a].scaled = bs.crossOnly;
      }
      SetCurrentTuning(savedT);
      mobs.Reset();
      debris.Reset();

      // The claim is the DIFFERENCE, and it is stated as a multiple rather
      // than as an absolute so it does not have to be retuned every time a
      // combustion number moves. The control arm is allowed to be non-zero:
      // the hips still emit fire into the grid and a little of it can drift
      // back onto a thigh, which is the very path that was too weak to matter
      // and is the reason this feature exists.
      // A DIFFERENTIAL IS ONLY MEANINGFUL WHILE THE CONTROL IS A CONTROL.
      //
      // The control arm is "the old behaviour": the only heat that reaches a
      // thigh is the grid's, through the flame the hips emit. At the authored
      // spread rate that path is far too weak to matter, which is the entire
      // owner report -- it measured 0 of 558. But combustion.spreadPct is a
      // live knob with a range up to 400%, and at the top of it the ordinary
      // grid path is fast enough to burn the legs off by itself: measured at
      // 400%, the control arm took 442 of 558 and the differential collapsed
      // to 1x. That is not the crossing failing, it is the experiment losing
      // its control, and reporting it as a failed assertion would be a lie
      // about which of the two the run found.
      //
      // So it is reported as UNMEASURABLE and the subtest declines to assert,
      // loudly and with the number that made it so. It still FAILS in the case
      // that matters -- a control that stays near zero while the live arm does
      // nothing is exactly the bug, and no setting of any knob hides that.
      const bool saturated = arms[1].touched * 2 >= arms[1].raw0;
      const bool crossOk =
          arms[0].lit > 0 && arms[0].touched > 0 &&
          arms[0].touched >= arms[1].touched * 4 + 8;
      std::printf(
          "  heat across a joint: %s (%u lit on hips; thigh voxels touched "
          "%u/%u at crossLimbPct %d vs %u/%u at 0, %ux; peak alight %u vs %u"
          "%s)\n",
          saturated ? "UNMEASURABLE" : crossOk ? "PASS" : "FAIL", arms[0].lit,
          arms[0].touched, arms[0].raw0, pcts[0], arms[1].touched,
          arms[1].raw0,
          arms[1].touched ? arms[0].touched / arms[1].touched
                          : arms[0].touched,
          arms[0].alight, arms[1].alight,
          saturated ? "; the CONTROL arm already burnt half the thigh through "
                      "the grid, so this differential has no control at this "
                      "combustion.spreadPct -- not asserted"
                    : "");
      // Attribution at the point of failure, not elimination afterwards: the
      // three counters say which link broke. No cells means the fronts were
      // empty (the hips never caught, or the index was dropped every tick); no
      // faces means the thighs never had a face pointing into one (a pose or
      // widening problem); faces but no scaled rolls means the rules never
      // matched; all three non-zero with nothing touched means the roll itself
      // is refusing, i.e. the percentage is too low.
      if (!crossOk)
        std::printf("    cross heat: %u cells, %u faces took one, %u rules "
                    "scaled by it (control arm: %u / %u / %u)\n",
                    arms[0].cells, arms[0].faces, arms[0].scaled,
                    arms[1].cells, arms[1].faces, arms[1].scaled);
      std::fflush(stdout);
      ok = ok && (crossOk || saturated);
    }
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
    // subtest must not assume the player is dressed in any particular thing.
    // `human`, the stock base body, is flesh plus one garment: a linen
    // linen on the hips and upper thighs, which is its own material rather
    // than a paint colour so that burning it chars instead of exposing skin. A
    // cloth census on it is legitimately 0, and asserting robe cloth here
    // would fail a perfectly correct rig for the crime of not wearing a robe
    // (gotcha-gate-hardcodes-asset-cast). The claim this subtest owns is
    // narrower than it looks: that the avatar's OWN BODY is wired into the same
    // burn pass mobs use. So it measures WHATEVER body mass the rig has —
    // flesh, robe and linen — and the cloth-goes-FIRST ordering is a separate
    // claim, asserted on the wizard above.
    uint32_t idleFront = 0, body0 = 0, cloth0 = 0, peakAlight = 0, peakChar = 0;
    float bodyLost = 0;
    auto avBody = [&]() {
      return avCensus(mCloth) + avCensus(mUnder) + avCensus(mSkin) +
             avCensus(mFlesh) + avCensus(mMuscle);
    };
    // THE STAGE CENSUS IS TAKEN ON THE LAST LIVE TICK, not after the blaze.
    // Since the burn cap (sim/tuning.h Gore §G, 2026-09-02) a body whose
    // surface is 70% burnt dies of it, and this bonfire gets a human there
    // inside its 90 ticks. Die() clears the skin lattice, so a census taken
    // on the corpse falls through to the collider — which was never burnt —
    // and reported "0.0% charred of 2528 flesh" against a body that had in
    // fact charred through. What is being asserted is how far the chain got
    // while there was a body to get it; that is a live reading.
    struct StageCensus {
      uint32_t skin = 0, flesh = 0, muscle = 0, cooked = 0, burning = 0,
               charred = 0, cinder = 0, under = 0, underBurn = 0, underChar = 0;
    } live;
    auto takeCensus = [&]() {
      live.skin = avCensus(mSkin);
      live.flesh = avCensus(mFlesh);
      live.muscle = avCensus(mMuscle);
      live.cooked = avCensus(mCooked);
      live.burning = avCensus(mBurning);
      live.charred = avCensus(mCharred);
      live.cinder = avCensus(mCinder);
      live.under = avCensus(mUnder);
      live.underBurn = avCensus(mUnderBurn);
      live.underChar = avCensus(mUnderChar);
    };
    int avDeathTick = -1;
    if (spawned) {
      // THE DEATH KNOT IS PARKED AT 100% FOR THIS FIXTURE. The claim here is
      // how far the burn chain gets across a body over 90 spread-scaled
      // ticks (see spreadScale) in a
      // bonfire; with the knot at its shipped 70% the human died at t+50
      // and the chain was measured over half the exposure it was written
      // against (14.9% past the sear, against a floor of 15% set between
      // 9.8% and 23.7% at 90 ticks). Death by burns has its own gate
      // (`burn-cap`); this one wants the body to stay to be counted. The cap
      // itself still applies, so hp is still clamped under it here.
      const Tuning savedT = CurrentTuning();
      {
        Tuning tt = savedT;
        tt.gore.burnDeathFraction = 1.0f;
        SetCurrentTuning(tt);
      }
      for (int i = 0; i < 10; i++) avTick(0);
      idleFront = avBurning();            // an idle player must cost nothing
      mobs.ResetBurnStats();  // count only what the BLAZE below asked of it
      cloth0 = avCensus(mCloth) + avCensus(mUnder);
      body0 = avBody();
      uint32_t lastBody = body0;
      takeCensus();
      for (int i = 0; i < 90 * spreadScale && avatar.Spawned() &&
                      avatar.IsAlive();
           i++) {
        avTick(mFire);
        if (!avatar.IsAlive()) {
          avDeathTick = i;
          break;
        }
        takeCensus();
        const uint32_t bd = avBody();
        if (bd) lastBody = bd;
        peakAlight = std::max(peakAlight, avCensus(mClothBurn) +
                                              avCensus(mUnderBurn) +
                                              avCensus(mBurning));
        peakChar = std::max(peakChar, avCensus(mClothChar) +
                                          avCensus(mUnderChar) +
                                          avCensus(mCooked) +
                                          avCensus(mCharred) +
                                          avCensus(mCinder));
      }
      bodyLost = body0 ? (float)(body0 - lastBody) / (float)body0 : 0.0f;
      SetCurrentTuning(savedT);
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
    // HOW FAR DOWN THE CHAIN THE FIRE ACTUALLY GOT, and why it stopped there.
    //
    // `peakChar` above lumps flesh_cooked in with flesh_charred, and those are
    // two completely different outcomes: cooked is a SEAR (one hot face, no
    // combustion) and charred is what is left after the voxel burned. A body
    // whose whole surface seared and never ignited scores exactly the same on
    // that number as one that burned through, which is what let "the player
    // only chars at the limb outlines" sit under a green gate. Split, and
    // reported next to the ramp histogram that says whether the threshold or
    // the fire is what is missing (MobSystem::BurnStats).
    if (spawned) {
      const MobSystem::BurnStats& bs = mobs.Burn();
      std::printf("    stages (last live tick%s): skin %u cooked %u burning "
                  "%u charred %u cinder %u | linen %u burning %u charred %u\n",
                  avDeathTick >= 0
                      ? (", died of the burns at t+" +
                         std::to_string(avDeathTick))
                            .c_str()
                      : "",
                  live.skin, live.cooked, live.burning, live.charred,
                  live.cinder, live.under, live.underBurn, live.underChar);
      // ---- FLESH REACHES CHAR, and this is the line that says so ----------
      //
      // The claim above is "the player is wired into the burn pass", and it
      // was true while the character stood in a bonfire for three seconds and
      // came out PINK. Everything that made that visible was lumped together:
      // `peakChar` counts flesh_cooked, which is a SEAR at one hot face and
      // not combustion at all, so a body that only ever seared scored the same
      // as one that burned. Measured before the world-pitch fix in
      // MobSystem::BurnOneLimb, 9.8% of flesh reached a char state and it was
      // all on the convex edges of the limbs; after, 23.7%, spread over the
      // surface. The floor is set between those two, so this fails if the ramp
      // ever stops reaching a body's flat faces again.
      const uint32_t flesh = live.skin + live.flesh + live.muscle +
                             live.cooked + live.burning + live.charred +
                             live.cinder;
      const uint32_t past = live.charred + live.cinder;
      const double charPct = flesh ? 100.0 * (double)past / (double)flesh : 0.0;
      const double floorPct = BaselineNumber("mobBurnFleshCharPctMin", 15.0);
      const bool charOk = charPct >= floorPct;
      std::printf("    flesh past the sear: %s (%.1f%% charred/cinder of %u "
                  "flesh, floor %.1f%%)\n",
                  charOk ? "PASS" : "FAIL", charPct, flesh, floorPct);
      ok = ok && charOk;
      std::printf("    ramp: %u rolls, %u refused by minCount, %u widened at "
                  "world pitch | hot faces 0:%u 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u\n",
                  bs.rampRolls, bs.rampRefused, bs.rampWidened,
                  bs.hotFaces[0], bs.hotFaces[1],
                  bs.hotFaces[2], bs.hotFaces[3], bs.hotFaces[4],
                  bs.hotFaces[5], bs.hotFaces[6]);
      std::printf("    exposure: %u candidates | world-facing faces "
                  "0:%u 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u\n",
                  bs.candidates, bs.exposed[0], bs.exposed[1], bs.exposed[2],
                  bs.exposed[3], bs.exposed[4], bs.exposed[5], bs.exposed[6]);
      std::fflush(stdout);
    }
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

// ============================================================================
// NPC AI gates (game/ai_behavior.h, game/ai_nav.h)
//
// WHAT THESE ASSERT, AND WHY EACH IS THE ASSERTION IT IS.
//
// The AI is a utility arbiter that writes a desired heading and a local drive
// vector. Almost every wrong implementation of that still produces a mob that
// "moves toward the player and turns", so "it got closer" and "its heading
// changed" are worthless claims — the same trap the steering gate documents.
// What is actually falsifiable:
//
//   ai-dummy    A profile that authorizes nothing must produce EXACTLY zero
//               motion and EXACTLY zero turn, with a target standing in plain
//               sight. This is the whole "behaviour is data" claim in one
//               number: the dummy shares every line of code with the duelist
//               and differs only in JSON.
//   ai-face     A static swordsman's heading must CONVERGE on the bearing to a
//               target moved to arbitrary, non-quantized angles around it —
//               and its feet must not move while it does. Convergence to a
//               free angle is what separates "faces the target" from "snaps to
//               one of eight probe directions".
//   ai-approach A duelist must cross a wall it cannot walk through (so the
//               navigator, not the steering, is what is being tested), settle
//               INSIDE its authored band, STAY there, and never occupy the
//               target's space. Reaching the target is easy; staying at arm's
//               length without dithering is the thing that is hard and the
//               thing that reads as good.
//
// FIXTURE NOTES. These gates run late (after mob-burn) and the last of them
// writes real voxels, so it regenerates the world on the way out — the gates
// after it place fixtures by absolute coordinate (CLAUDE.md rule 7). Thresholds
// live in tests/baseline.json, not here.
// ============================================================================

// The humanoid rig the AI gates drive. Chosen by CAPABILITY first — it has to
// publish a `held_right` socket or the sword cannot be equipped and the whole
// fixture is silently testing an unarmed mob — and the tie goes to `human`.
//
// IT USED TO GO TO `mina`, AND THAT QUIETLY MADE EVERY AI GATE A TEST OF A
// CREATURE NOBODY FIGHTS. It is not a cosmetic difference: mina walks 60
// voxels a second against the human's 31.5, which is two columns a tick
// instead of one, and the locomotion failures that matter are all about whether
// the body can keep up with the ground it is crossing. Measured on the slope
// fixture, the two rigs disagreed about the SIZE of every number this file
// asserts. A gate should drive what is in play; when that changes, this
// function is the one line to change.
int AiHumanoidDef(const MobSystem& mobs) {
  int best = -1;
  for (size_t i = 0; i < mobs.Defs().size(); i++) {
    if (mobs.Defs()[i].FindSocket("held_right") < 0) continue;
    if (best < 0 || mobs.Defs()[i].name == "human") best = (int)i;
  }
  return best;
}

// The flattest patch of terrain within `search` voxels of (cx,cz), sampled on a
// coarse grid over a `half`-voxel half-extent.
//
// Terrain is procedural and this gate must not be pinned to a hand-picked
// coordinate that a worldgen tweak silently puts on a cliff — the fixture asks
// the SAME height function the world is built from where the flat ground is,
// and reports the relief it settled for so a future failure says "the terrain
// moved" instead of "the AI broke".
IVec3 AiFlatSpot(int cx, int cz, int search, int half, uint32_t seed,
                 int& outRelief) {
  int bestX = cx, bestZ = cz, bestRelief = INT32_MAX;
  for (int oz = -search; oz <= search; oz += 8) {
    for (int ox = -search; ox <= search; ox += 8) {
      int lo = INT32_MAX, hi = INT32_MIN;
      for (int dz = -half; dz <= half; dz += 3)
        for (int dx = -half; dx <= half; dx += 3) {
          const int h = World::TerrainHeight(cx + ox + dx, cz + oz + dz, seed);
          lo = std::min(lo, h);
          hi = std::max(hi, h);
        }
      const int relief = hi - lo;
      if (relief < bestRelief) {
        bestRelief = relief;
        bestX = cx + ox;
        bestZ = cz + oz;
      }
    }
  }
  outRelief = bestRelief;
  return IVec3{bestX, World::TerrainHeight(bestX, bestZ, seed), bestZ};
}

// Where an AI fixture goes: the CENTRE OF THE RESIDENCY WINDOW, never a fixed
// world coordinate.
//
// This is the trap selftest.h names in as many words, and it cost this gate one
// full-suite run to relearn: `streaming` leaves the window origin ~20 chunks
// out, so a mob spawned at an absolute (140,140) lands outside it and
// MobSystem::PreTick despawns it on the first tick as out-of-window. The
// symptom is not "the AI misbehaved" — it is a target 134 voxels away, zero
// ticks in any intent and a null Brain, because there is no creature at all.
// Every one of these gates passed standalone and failed in the suite.
IVec3 AiFixtureCentre(const World& world) {
  const IVec3 o = world.WindowOrigin();
  return IVec3{(o.x + (int)kNChunk / 2) * (int)kChunk, 0,
               (o.z + (int)kNChunk / 2) * (int)kChunk};
}

// Everything an AI gate needs to advance one tick. The mob systems run in the
// same order PreTick does in the game, so a behaviour cannot pass here by
// depending on an ordering the frame loop does not have.
struct AiTicker {
  Ctx& c;
  uint32_t tick = 7000;
  IVec3 playerChunk{8, 8, 8};

  void operator()(const std::vector<BrushOp>& ops = {},
                  const std::vector<CellOp>& cells = {}) {
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps = cells;
    std::vector<BrushOp> myOps = ops;
    c.mobs.PreTick(tick + 1, c.world, myOps, cellOps, spawns);
    c.debris.QueueSupportEvents(c.world.Snap());
    c.debris.PreTick(tick + 1, c.world, cellOps, spawns);
    ++tick;
    SubmitTick(c.ctx, c.world, c.sim, tick, kDefaultSeed, myOps, {}, cellOps,
               false, playerChunk, true, false, spawns);
    c.ctx.WaitIdle();
    c.ctx.ProcessEvents();
    c.phys.Step(kTickDt);
    c.debris.PostStep();
    c.mobs.PostStep();
  }
};

Vec3 AiMobCentre(const MobSystem& mobs, uint64_t id, const MobDef& def) {
  const Vec3 o = mobs.MobOrigin(id);
  return Vec3{o.x + def.worldSize.x * 0.5f, o.y + def.worldSize.y * 0.5f,
              o.z + def.worldSize.z * 0.5f};
}

float AiPlanar(Vec3 a, Vec3 b) {
  const float dx = a.x - b.x, dz = a.z - b.z;
  return std::sqrt(dx * dx + dz * dz);
}

// Spawn one AI-driven humanoid with a sword, on the flat spot, with `profile`
// applied. Returns 0 on any failure, with the reason already in `why`.
uint64_t AiSpawn(Ctx& c, int defIndex, IVec3 at, const char* profile,
                 std::string& why) {
  const uint64_t id = c.mobs.Spawn(defIndex, at);
  if (id == 0) {
    why = "Spawn refused";
    return 0;
  }
  if (!c.mobs.SetMobBehavior(id, profile)) {
    why = std::string("no behaviour profile \"") + profile +
          "\" in assets/mobs/behaviors.json";
    return 0;
  }
  // A mob with a sword is one EquipItem call (mob.h). Not decorative here: an
  // attack request that goes out from an unarmed creature is a different claim.
  const ItemDef* sword = c.items.At(c.items.Find("sword"));
  if (sword != nullptr) c.mobs.EquipItem(id, sword);
  return id;
}

// ---- crowd -----------------------------------------------------------------
//
// Owner report: creatures chasing one target "bunch up into the same space and
// overlap". Four duelists are pointed at one actor from four sides and the
// claim is that they end up AROUND it rather than inside each other.
//
// TWO ARMS, AND THE SECOND ONE IS THE POINT. "No two bodies overlapped" is
// vacuously true of a fixture that never crowded them in the first place — a
// spawn spread too wide, a target nobody reached, an AI that never closed. So
// the control arm runs the SAME fixture with `spacingMul` zeroed in the def and
// must overlap badly. A DATA-only control arm (no rebuild, no second binary,
// no code path that exists for the test), which is the cheapest honest way to
// prove a fixture measures its subject.
Status GateCrowd(Ctx& c, std::string& detail) {
  c.debris.Reset();
  c.mobs.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  const int defIndex = AiHumanoidDef(c.mobs);
  if (defIndex < 0) {
    detail = "no mob def publishes a held_right socket";
    return Status::Fail;
  }
  if (c.mobs.Behaviors().profiles.empty()) {
    detail = "assets/mobs/behaviors.json loaded no profiles";
    return Status::Fail;
  }
  // The pristine defs, kept so the control arm can be handed a modified copy
  // and the gate can put the originals back on the way out. Gates share one
  // World and one MobSystem (selftest.h kOrder), so leaving a def edited here
  // would silently retune every NPC gate after this one.
  const std::vector<MobDef> pristine = c.mobs.Defs();

  int relief = 0;
  const IVec3 anchor = AiFixtureCentre(c.world);
  const IVec3 spot =
      AiFlatSpot(anchor.x, anchor.z, 96, 16, kDefaultSeed, relief);

  // One arm: spawn four duelists on the points of a compass around a target
  // they all want, and report the closest two bodies ever got.
  struct Arm {
    float minGap = 1e9f;    // closest centre-to-centre distance seen, voxels
    float minSep = 0.0f;    // the distance at which those two bodies touch
    int overlapTicks = 0;   // ticks with ANY pair interpenetrating
    int alive = 0;
    // ---- WHY ONE OF THEM IS NOT ALIVE (CLAUDE.md rule 6) ----------------
    // "3/4 alive" is a bare number with two very different causes -- they cut
    // each other, or one fell over and bled out -- and a spacing gate that
    // silently became a knife fight would read exactly the same either way.
    int requests = 0;       // attack requests issued over the arm
    int cutTicks = 0;       // ticks any of them was in a committed cut
    float minHp = 1e9f;     // the worst-off survivor
  };
  auto runArm = [&](Arm& out) {
    c.debris.Reset();
    c.mobs.Reset();
    const MobDef& def = c.mobs.Defs()[defIndex];
    // Radius the engine itself uses, mirrored here rather than guessed: the
    // gate must assert against the same geometry the feature enforces, or it
    // is measuring a number of its own invention.
    const float r = std::max(0.25f, 0.25f * (def.worldSize.x + def.worldSize.z));
    out.minSep = 2.0f * r;
    // Far enough out that they must CONVERGE to crowd (so the fixture tests
    // approach, not just spawn placement), close enough to arrive inside the
    // tick budget.
    const int ring = std::max(8, (int)(out.minSep * 4.0f));
    const int dxs[4] = {1, -1, 0, 0};
    const int dzs[4] = {0, 0, 1, -1};
    std::vector<uint64_t> ids;
    std::string why;
    for (int k = 0; k < 4; k++) {
      const IVec3 at{spot.x + dxs[k] * ring, spot.y + 1, spot.z + dzs[k] * ring};
      // ---- `crowder`, NOT `duelist` (2026-09-15) ------------------------
      //
      // A duelist is armed and swings, and four of them converging on one
      // point are inside each other's arcs BY CONSTRUCTION -- the spacing
      // feature under test is exactly what holds them 3.6 voxels apart with
      // 8-voxel swords. That was harmless only while blades quietly failed to
      // connect; with the sweep's probe no longer dying inside its own
      // wielder, the same fixture became a knife fight (11 attack requests, 54
      // cut ticks, one of the four dead) while every spacing number it exists
      // to measure kept passing. `crowder` wants the target and walks at it
      // and has no attack intent at all, so this gate measures its own
      // subject; friendly fire is `duel`'s, and `duel` asserts it.
      const uint64_t id = AiSpawn(c, defIndex, at, "crowder", why);
      if (id != 0) ids.push_back(id);
    }
    // ONE target for all four: that is the whole scenario. Placed at the
    // fixture centre so every duelist wants the same point.
    c.mobs.SetPlayerActor(Vec3{(float)spot.x, (float)spot.y + 8.0f,
                               (float)spot.z},
                          3.0f, 17.0f, true);
    AiTicker tick{c, 7000, {spot.x >> 4, spot.y >> 4, spot.z >> 4}};
    const int ticks = (int)BaselineNumber("crowdTicks", 240);
    for (int t = 0; t < ticks; t++) {
      tick();
      out.requests += (int)c.mobs.AttackRequests().size();
      c.mobs.ClearAttackRequests();
      for (uint64_t id : ids) {
        if (!c.mobs.IsAlive(id)) continue;
        const NpcStroke* s = c.mobs.MobStroke(id);
        if (s != nullptr && s->Cutting()) out.cutTicks++;
      }
      for (size_t a = 0; a < ids.size(); a++) {
        if (!c.mobs.IsAlive(ids[a])) continue;
        for (size_t b = a + 1; b < ids.size(); b++) {
          if (!c.mobs.IsAlive(ids[b])) continue;
          const Vec3 pa = c.mobs.MobOrigin(ids[a]);
          const Vec3 pb = c.mobs.MobOrigin(ids[b]);
          // Origins are MIN CORNERS of identical bodies, so the centre offset
          // cancels and the origin delta IS the centre delta.
          const float dx = pa.x - pb.x, dz = pa.z - pb.z;
          const float d = std::sqrt(dx * dx + dz * dz);
          out.minGap = std::min(out.minGap, d);
          if (d < out.minSep) out.overlapTicks++;
        }
      }
    }
    for (uint64_t id : ids)
      if (c.mobs.IsAlive(id)) {
        out.alive++;
        out.minHp = std::min(out.minHp, c.mobs.TotalHp(id));
      }
    c.mobs.ClearPlayerActor();
    c.mobs.ClearAttackRequests();
  };

  Arm on{};
  runArm(on);

  // ---- the control arm: the same fixture with spacing switched OFF --------
  Arm off{};
  {
    std::vector<MobDef> noSpacing = pristine;
    for (MobDef& d : noSpacing) d.skel.loco.spacingMul = 0.0f;
    c.mobs.SetDefs(std::move(noSpacing));
    runArm(off);
    c.mobs.SetDefs(std::vector<MobDef>(pristine));
  }

  // A tolerance, not equality: the drive resolves in discrete steps and may
  // end a tick a hair inside the touching distance before the next one pushes
  // back out. Anything approaching a body's own radius is a real overlap.
  const double slack = BaselineNumber("crowdOverlapSlackFrac", 0.12);
  const float floorOn = on.minSep * (float)(1.0 - slack);
  const bool spaced = on.alive == 4 && on.minGap >= floorOn;
  // THE FIXTURE MUST ACTUALLY CROWD. If the control arm also stays apart, this
  // gate is measuring nothing and says so rather than passing.
  const bool crowdedWithout = off.minGap < on.minSep;
  const bool better = on.minGap > off.minGap;

  RecordObserved("crowdMinGapOn", on.minGap);
  RecordObserved("crowdMinGapOff", off.minGap);
  RecordObserved("crowdOverlapTicksOff", off.overlapTicks);

  const bool ok = spaced && crowdedWithout && better;
  detail = Format(
      "touching at %.2f vox | ON min gap %.2f (>= %.2f), %d overlap ticks, "
      "%d/4 alive (%d requests, %d cut ticks, worst hp %.0f) | OFF min gap "
      "%.2f, %d overlap ticks | spaced %d, fixture crowds %d, improved %d",
      on.minSep, on.minGap, floorOn, on.overlapTicks, on.alive, on.requests,
      on.cutTicks, on.minHp >= 1e8f ? -1.0f : on.minHp, off.minGap,
      off.overlapTicks, spaced ? 1 : 0, crowdedWithout ? 1 : 0,
      better ? 1 : 0);

  c.debris.Reset();
  c.mobs.Reset();
  return ok ? Status::Pass : Status::Fail;
}

// ---- undead ----------------------------------------------------------------
//
// One gate for the whole of "a zombie is a human, differently" (mob.h
// MobDef::undead / MobRotDef, assets/mobs/zombie.json). Four claims, and the
// first two are the ones that would otherwise rot silently:
//
//   A. THE VARIANT WEARS THE ORIGINAL'S BODY. `extends` and `model` exist so
//      that human.vox has exactly one copy in the repo. If somebody later
//      "fixes" the loader by pairing .json to .vox by filename again, the
//      zombie simply stops existing — which nothing else here would notice,
//      because every other claim is about a def that failed to load.
//   B. A LIVING CREATURE IS BORN WHOLE. The control arm, and the reason the
//      rot claims mean anything: "voxels are missing" is not a result unless
//      the same measurement on a human comes back zero.
//   C. THE DAMAGE IS REAL AND IT IS CHARGED. Voxels gone from the
//      authoritative lattice, hp down with them, nothing severed, and — the
//      easy one to lose — NO BLOOD, because these holes are old.
//   D. NO TWO ARE ALIKE. Per-limb loss differs between two spawns of the same
//      def. A per-DEF seed would pass every claim above and produce an army of
//      identical corpses.
//
// No ticks at all: rot happens inside Spawn, so everything here is a count
// taken the instant after it.
Status GateUndead(Ctx& c, std::string& detail) {
  c.debris.Reset();
  c.mobs.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  int zi = -1, hi = -1;
  for (int i = 0; i < (int)c.mobs.Defs().size(); i++) {
    if (c.mobs.Defs()[i].name == "zombie") zi = i;
    if (c.mobs.Defs()[i].name == "human") hi = i;
  }
  if (zi < 0 || hi < 0) {
    detail = zi < 0 ? "no \"zombie\" mob def (assets/mobs/zombie.json)"
                    : "no \"human\" mob def to compare against";
    return Status::Fail;
  }
  const MobDef& zd = c.mobs.Defs()[zi];
  const MobDef& hd = c.mobs.Defs()[hi];

  // ---- A: the variant wears the original's art and rig ---------------------
  const bool sameArt = zd.prefab.size.x == hd.prefab.size.x &&
                       zd.prefab.size.y == hd.prefab.size.y &&
                       zd.prefab.size.z == hd.prefab.size.z &&
                       zd.skinScale == hd.skinScale &&
                       zd.limbs.size() == hd.limbs.size() &&
                       zd.prefab.artColors.size() == hd.prefab.artColors.size();
  // ...and the overrides on top of it actually landed.
  // ...and it must still be ARMABLE. The NPC AI panel's creature list is every
  // def publishing a `held_right` socket, because the panel arms what it
  // spawns — so a variant that lost the inherited socket would silently
  // vanish from that dropdown with nothing else going wrong.
  const bool overrides = zd.undead && !zd.woundHeals && hd.woundHeals &&
                         zd.rot.enabled && zd.speed < hd.speed &&
                         zd.speed > 0.0f &&
                         zd.FindSocket("held_right") >= 0;

  // The palette is a FILTER, so the claim is about the shape of the change and
  // not about any particular colour: greyer (chroma down) and no darker.
  auto meanOf = [](const std::vector<uint32_t>& cs, bool chroma) {
    double sum = 0;
    int n = 0;
    for (uint32_t v : cs) {
      if (v == 0) continue;  // unused slot (voxload zero-fills all 128)
      const int r = (int)((v >> 16) & 0xFF), g = (int)((v >> 8) & 0xFF),
                b = (int)(v & 0xFF);
      sum += chroma ? (double)(std::max({r, g, b}) - std::min({r, g, b}))
                    : 0.2126 * r + 0.7152 * g + 0.0722 * b;
      n++;
    }
    return n ? sum / n : 0.0;
  };
  const double zChroma = meanOf(zd.prefab.artColors, true);
  const double hChroma = meanOf(hd.prefab.artColors, true);
  const double zLuma = meanOf(zd.prefab.artColors, false);
  const double hLuma = meanOf(hd.prefab.artColors, false);
  const double chromaMax = BaselineNumber("undeadChromaFracMax", 0.75);
  const bool paler = hChroma > 0.0 && zChroma < hChroma * chromaMax &&
                     zLuma >= hLuma;

  // ---- spawn: one human, two zombies, same ground -------------------------
  int relief = 0;
  const IVec3 anchor = AiFixtureCentre(c.world);
  const IVec3 spot =
      AiFlatSpot(anchor.x, anchor.z, 96, 16, kDefaultSeed, relief);
  const int step = std::max(8, (int)hd.worldSize.x * 3);
  const uint64_t hId = c.mobs.Spawn(hi, {spot.x, spot.y + 1, spot.z});
  const uint64_t zA = c.mobs.Spawn(zi, {spot.x + step, spot.y + 1, spot.z});
  const uint64_t zB = c.mobs.Spawn(zi, {spot.x + 2 * step, spot.y + 1, spot.z});
  Mob* mh = c.mobs.FindMobById(hId);
  Mob* ma = c.mobs.FindMobById(zA);
  Mob* mb = c.mobs.FindMobById(zB);
  if (!mh || !ma || !mb) {
    detail = "Spawn refused (human, zombie A or zombie B)";
    c.debris.Reset();
    c.mobs.Reset();
    return Status::Fail;
  }

  // Counts on the AUTHORITATIVE lattice — the one LimbVoxelsAtSpawn counted.
  // Mixing it with the collider's would scale every fraction below by
  // (skinScale/physScale)^3 and make a 20% loss read as 0.3%.
  auto tally = [&](uint64_t id, const Mob* m, std::vector<uint32_t>& perLimb,
                   uint32_t& at0, uint32_t& now, uint32_t& spurs) {
    at0 = now = spurs = 0;
    perLimb.clear();
    for (int i = 0; i < m->LimbCount(); i++) {
      const uint32_t a = c.mobs.LimbVoxelsAtSpawn(id, i);
      const uint32_t n = c.mobs.LimbArtVoxelCount(id, i);
      at0 += a;
      now += n;
      perLimb.push_back(a > n ? a - n : 0u);
      // Voxels left with four or more open faces: what SPECKLE leaves behind
      // and a torn chunk does not (LimbOpenFaceCount's own note). This is the
      // "largely in groups" half of the owner's ask expressed as a number —
      // a count of missing voxels alone cannot tell the two apart.
      spurs += c.mobs.LimbOpenFaceCount(id, i, 4);
    }
  };
  std::vector<uint32_t> lh, la, lb;
  uint32_t hAt0 = 0, hNow = 0, hSpur = 0, aAt0 = 0, aNow = 0, aSpur = 0;
  uint32_t bAt0 = 0, bNow = 0, bSpur = 0;
  tally(hId, mh, lh, hAt0, hNow, hSpur);
  tally(zA, ma, la, aAt0, aNow, aSpur);
  tally(zB, mb, lb, bAt0, bNow, bSpur);

  // ---- B: the living control arm ------------------------------------------
  const bool humanWhole = hAt0 > 0 && hNow == hAt0;

  // ---- C: the damage is real, bounded, charged, and dry -------------------
  const double lossA = aAt0 ? (double)(aAt0 - aNow) / (double)aAt0 : 0.0;
  const double lossMin = BaselineNumber("undeadLossFracMin", 0.01);
  // The authored ceiling plus slack for the cap's own estimate of how much of
  // a bite sphere leaves (Mob::RotAtSpawn kRotSphereFill). If this trips, the
  // cap is not holding and a limb is one bad draw from collapsing.
  const double lossMax = BaselineNumber("undeadLossFracMax", 0.30);
  const bool bitten = lossA >= lossMin && lossA <= lossMax && aNow < aAt0;
  const float hpH = c.mobs.TotalHp(hId), hpA = c.mobs.TotalHp(zA);
  const bool hurt = hpA > 0.0f && hpH > 0.0f && hpA < hpH;
  // Rot must never be what takes a limb off: no sever, no death, same rig.
  const bool intact = c.mobs.IsAlive(zA) && c.mobs.IsAlive(zB) &&
                      ma->LimbCount() == mh->LimbCount() &&
                      mb->LimbCount() == mh->LimbCount();
  // THE HOLES ARE OLD. Without Mob::inSpawnRot_ every bite tops up a drip
  // budget and the creature arrives haemorrhaging.
  const bool dry = c.mobs.BloodLost(zA) <= 0.0f;
  // Chunks, not speckle: spurs per voxel lost, against the same body's own
  // baseline roughness (the human's spur count is what the ART already has).
  const uint32_t lostA = aAt0 - aNow;
  const double spurPerLost =
      lostA ? (double)(aSpur > hSpur ? aSpur - hSpur : 0u) / (double)lostA : 0.0;
  const double spurMax = BaselineNumber("undeadMaxSpurPerLost", 0.20);
  const bool chunky = spurPerLost <= spurMax;

  // ---- D: no two are alike -------------------------------------------------
  const bool varies = la.size() == lb.size() && la != lb;

  // ---- E: a rot hole is a WOUND, and it looks like one --------------------
  // Owner report: a bite showed "just pale underneath, which doesn't look
  // great". The soak has two halves and they are different mechanisms, so both
  // are counted separately -- a regression that killed one would otherwise hide
  // behind the other:
  //   * WOUND-MATERIAL voxels: flesh rewritten to blood, the red in the hole.
  //   * STAINED voxels: a tint laid OVER what the hole exposed, bone included
  //     (which the rewrite refuses on purpose), so a bite through the skull
  //     does not read as clean bone.
  // Measured on the authoritative lattice through LimbLattice, and once again
  // against the LIVING CONTROL ARM: an unrotted human must have neither, or
  // "there is blood on it" is not a result.
  auto gore = [&](uint64_t id, const Mob* m, uint32_t& wound, uint32_t& stain) {
    wound = stain = 0;
    for (int i = 0; i < m->LimbCount(); i++)
      for (const PrefabVoxel& v : c.mobs.LimbLattice(id, i)) {
        if (zd.woundMat && (uint32_t)(v.material & 0xFFFu) == zd.woundMat)
          wound++;
        if (v.stain) stain++;
      }
  };
  uint32_t aWound = 0, aStain = 0, hWound = 0, hStain = 0;
  gore(zA, ma, aWound, aStain);
  gore(hId, mh, hWound, hStain);
  // Fractions of the body, so the floor is a look rather than a voxel count
  // that drifts with the art's resolution.
  const double woundFrac = aAt0 ? (double)aWound / (double)aAt0 : 0.0;
  const double stainFrac = aAt0 ? (double)aStain / (double)aAt0 : 0.0;
  const double woundMin = BaselineNumber("undeadWoundFracMin", 0.02);
  const double stainMin = BaselineNumber("undeadStainFracMin", 0.02);
  // ---- THE CONTROL ARM IS NOT ZERO FOR THE REWRITE, AND MUST NOT BE --------
  //
  // A LIVING HUMAN ALREADY CONTAINS BLOOD VOXELS: its `anatomy` speckles the
  // muscle layer with `blood` at fraction 0.06 (assets/mobs/human.json), baked
  // into the .vox. Measured here at 182 of 26,494. So "the zombie has wound
  // material and the human has none" is a FALSE claim that this gate asserted
  // and failed on first run -- correctly, because the claim was wrong rather
  // than the feature.
  //
  // The honest version is a RATIO against that baseline. The two halves of the
  // soak therefore get different tests, which is right because they are
  // different mechanisms: the rewrite must clear the anatomy's own blood by a
  // wide margin, while the SMEAR is only ever applied by damage, so zero on an
  // unhurt creature is a real and checkable claim for it.
  const double hWoundFrac = hAt0 ? (double)hWound / (double)hAt0 : 0.0;
  const double woundOverBase = BaselineNumber("undeadWoundOverAnatomy", 3.0);
  const bool bloody = zd.rot.stainScale > 0.0f && woundFrac >= woundMin &&
                      woundFrac >= hWoundFrac * woundOverBase &&
                      stainFrac >= stainMin && hStain == 0;

  // ---- F: NOT EVERY HOLE BLED (2026-09-15, MobRotDef::dryFraction) ---------
  //
  // Claim E above says there IS blood, which the first version of the soak
  // satisfied by making every hole on the body equally bloody -- and a body
  // whose wounds are all the same age reads as uniform however right any one
  // of them looks. The owner's word for what it should be is a MISHMASH. So
  // this claim is the other end of the same spectrum: a real share of the
  // holes must be DRY, showing the flesh and bone the anatomy baked under the
  // skin with no blood over it at all.
  //
  // WHAT COUNTS AS "IN A HOLE", WITHOUT A RECIPE LOOKUP. A hole's wall is an
  // EXPOSED voxel (one with an empty 6-neighbour on this limb's own lattice)
  // whose material is one the intact body never shows -- muscle, fat, bone,
  // whatever the anatomy put under the skin. The set of materials a whole body
  // DOES show is not hardcoded here: it is measured off the LIVING CONTROL ARM
  // this gate already spawns, so the claim keeps working when the human's art,
  // its anatomy recipe or its clothing changes, and cannot be broken by a
  // material rename.
  //
  // Voxels already rewritten to the wound material are excluded: they are
  // blood by construction, so counting them either way would be answering a
  // different question. What is left is "of the tissue showing through the
  // holes, how much of it has nothing on it".
  //
  // Both ENDS are asserted, because either alone is satisfied by a uniform:
  // all-dry is the bloodless rot this replaced and all-wet is what it replaced
  // it with.
  std::vector<uint32_t> skinMats;  // what a WHOLE body shows, measured
  auto exposedInner = [&](uint64_t id, const Mob* m,
                          std::vector<uint32_t>* mats, uint32_t& wall,
                          uint32_t& clean) {
    wall = clean = 0;
    for (int i = 0; i < m->LimbCount(); i++) {
      const std::vector<PrefabVoxel>& lat = c.mobs.LimbLattice(id, i);
      std::unordered_set<uint64_t> occ;
      occ.reserve(lat.size() * 2);
      auto key = [](int x, int y, int z) {
        return (uint64_t)(uint32_t)(x + 32768) |
               ((uint64_t)(uint32_t)(y + 32768) << 16) |
               ((uint64_t)(uint32_t)(z + 32768) << 32);
      };
      for (const PrefabVoxel& v : lat)
        if (v.material) occ.insert(key(v.x, v.y, v.z));
      static const int d6[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                   {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
      for (const PrefabVoxel& v : lat) {
        const uint32_t mat = (uint32_t)(v.material & 0xFFFu);
        if (!mat) continue;
        bool open = false;
        for (const auto& dd : d6)
          if (!occ.count(key(v.x + dd[0], v.y + dd[1], v.z + dd[2]))) {
            open = true;
            break;
          }
        if (!open) continue;
        if (mats) {  // control arm: RECORD what a whole body shows
          mats->push_back(mat);
          continue;
        }
        if (zd.woundMat && mat == zd.woundMat) continue;  // is blood already
        if (std::find(skinMats.begin(), skinMats.end(), mat) != skinMats.end())
          continue;
        wall++;
        if (!v.stain) clean++;
      }
    }
  };
  uint32_t wall = 0, wallClean = 0, ignA = 0, ignB = 0;
  exposedInner(hId, mh, &skinMats, ignA, ignB);
  std::sort(skinMats.begin(), skinMats.end());
  skinMats.erase(std::unique(skinMats.begin(), skinMats.end()), skinMats.end());
  exposedInner(zA, ma, nullptr, wall, wallClean);
  const double dryWallFrac = wall ? (double)wallClean / (double)wall : 0.0;
  const double dryWallMin = BaselineNumber("undeadDryWallFracMin", 0.20);
  const double dryWallMax = BaselineNumber("undeadDryWallFracMax", 0.85);
  // The floor on `wall` is the vacuity guard: "0 of 0 wall voxels are clean"
  // would otherwise pass or fail on a division rather than on the feature.
  const bool mishmash = wall >= 200 && dryWallFrac >= dryWallMin &&
                        dryWallFrac <= dryWallMax;

  RecordObserved("undeadLossFrac", lossA);
  RecordObserved("undeadSpurPerLost", spurPerLost);
  RecordObserved("undeadChromaFrac", hChroma > 0 ? zChroma / hChroma : 0.0);
  RecordObserved("undeadWoundFrac", woundFrac);
  RecordObserved("undeadStainFrac", stainFrac);
  RecordObserved("undeadDryWallFrac", dryWallFrac);

  const bool ok = sameArt && overrides && paler && humanWhole && bitten &&
                  hurt && intact && dry && chunky && varies && bloody &&
                  mishmash;
  detail = Format(
      "art/rig shared %d, overrides %d, palette chroma %.2f -> %.2f luma %.1f "
      "-> %.1f (%d), human whole %u/%u (%d), zombie %u/%u lost %.3f (%.2f..%.2f"
      ", %d), hp %.1f -> %.1f (%d), intact %d, dry %d, spurs/lost %.3f <= %.3f "
      "(%d), varies %d, blood: wound %.3f (>= %.3f and >= %.1fx anatomy "
      "%.3f) stain %.3f (>= %.3f), human wound/stain %u/%u (%d), dry hole "
      "walls %u/%u = %.3f (%.2f..%.2f, %d)",
      sameArt ? 1 : 0, overrides ? 1 : 0, hChroma, zChroma, hLuma, zLuma,
      paler ? 1 : 0, hNow, hAt0, humanWhole ? 1 : 0, aNow, aAt0, lossA, lossMin,
      lossMax, bitten ? 1 : 0, hpH, hpA, hurt ? 1 : 0, intact ? 1 : 0,
      dry ? 1 : 0, spurPerLost, spurMax, chunky ? 1 : 0, varies ? 1 : 0,
      woundFrac, woundMin, woundOverBase, hWoundFrac, stainFrac, stainMin,
      hWound, hStain, bloody ? 1 : 0, wallClean, wall, dryWallFrac, dryWallMin,
      dryWallMax, mishmash ? 1 : 0);

  c.debris.Reset();
  c.mobs.Reset();
  return ok ? Status::Pass : Status::Fail;
}

// ---- ai-dummy --------------------------------------------------------------
Status GateAiDummy(Ctx& c, std::string& detail) {
  c.debris.Reset();
  c.mobs.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  const int defIndex = AiHumanoidDef(c.mobs);
  if (defIndex < 0) {
    detail = "no mob def publishes a held_right socket";
    return Status::Fail;
  }
  if (c.mobs.Behaviors().profiles.empty()) {
    detail = "assets/mobs/behaviors.json loaded no profiles";
    return Status::Fail;
  }
  const MobDef& def = c.mobs.Defs()[defIndex];

  int relief = 0;
  const IVec3 anchor = AiFixtureCentre(c.world);
  const IVec3 spot =
      AiFlatSpot(anchor.x, anchor.z, 96, 16, kDefaultSeed, relief);
  std::string why;
  const uint64_t id =
      AiSpawn(c, defIndex, {spot.x, spot.y + 1, spot.z}, "dummy", why);
  if (id == 0) {
    detail = why;
    return Status::Fail;
  }

  // A target in PLAIN SIGHT, close enough to attack. The claim is not "an
  // unprovoked dummy stands still" — that is trivially true of a mob with no
  // AI at all. It is "a dummy standing next to something a duelist would
  // charge does not move a millimetre", which is only true if the profile, and
  // nothing else, is what authorizes motion.
  c.mobs.SetPlayerActor(
      Vec3{(float)spot.x + 9.0f, (float)spot.y + 8.0f, (float)spot.z}, 3.0f,
      17.0f, true);

  // Arithmetic shift, not divide: a residency window that has streamed to
  // negative world coordinates makes `/ 16` round toward zero and name the
  // wrong chunk. The player chunk is also the fixture's own, so the tick
  // stream does not shift the window out from under the mob mid-gate.
  AiTicker tick{c, 7000, {spot.x >> 4, spot.y >> 4, spot.z >> 4}};
  tick();   // one tick so the rig settles onto the ground before measuring
  const Vec3 p0 = c.mobs.MobOrigin(id);
  const float h0 = c.mobs.MobHeading(id);
  const int ticks = (int)BaselineNumber("aiDummyTicks", 90);
  float maxDrift = 0, maxTurn = 0;
  for (int i = 0; i < ticks; i++) {
    tick();
    const Vec3 p = c.mobs.MobOrigin(id);
    maxDrift = std::max(maxDrift, AiPlanar(p, p0));
    maxTurn = std::max(maxTurn,
                       std::abs(std::remainder(c.mobs.MobHeading(id) - h0,
                                               6.2831853f)));
  }
  const ai::Brain* br = c.mobs.MobBrain(id);
  const bool sawNothing = br != nullptr && !br->hasTarget;
  // A despawned mob has zero drift and zero turn too. Without this line the two
  // assertions above are vacuously true for exactly the failure that is easiest
  // to cause — a fixture placed outside the residency window (see
  // AiFixtureCentre) — which is how this gate reported PASS-shaped numbers on a
  // creature that had not existed for ninety ticks.
  const bool stillHere = c.mobs.IsAlive(id) && br != nullptr;

  // EXACT zero, not "small": the dummy profile authorizes no drive at all, so
  // any motion is a code path that ignored the data. The bounds live in
  // baseline.json only so a future gait change that legitimately jitters the
  // min corner can be re-pinned without a rebuild.
  const float driftMax = (float)BaselineNumber("aiDummyMaxDriftVox", 0.001);
  const float turnMax = (float)BaselineNumber("aiDummyMaxTurnRad", 0.001);
  const bool ok =
      stillHere && maxDrift <= driftMax && maxTurn <= turnMax && sawNothing;
  RecordObserved("aiFlatSpotRelief", (double)relief);
  detail = Format(
      "%d ticks, alive %d, drift %.5f vox (<= %.3f), turn %.5f rad (<= %.3f), "
      "blind %d, terrain relief %d",
      ticks, stillHere ? 1 : 0, maxDrift, driftMax, maxTurn, turnMax,
      sawNothing ? 1 : 0, relief);
  c.mobs.ClearPlayerActor();
  c.mobs.ClearAttackRequests();
  c.debris.Reset();
  c.mobs.Reset();
  return ok ? Status::Pass : Status::Fail;
}

// ---- ai-face ---------------------------------------------------------------
Status GateAiFace(Ctx& c, std::string& detail) {
  c.debris.Reset();
  c.mobs.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  const int defIndex = AiHumanoidDef(c.mobs);
  if (defIndex < 0) {
    detail = "no mob def publishes a held_right socket";
    return Status::Fail;
  }
  const MobDef& def = c.mobs.Defs()[defIndex];
  int relief = 0;
  const IVec3 anchor = AiFixtureCentre(c.world);
  const IVec3 spot =
      AiFlatSpot(anchor.x, anchor.z, 96, 18, kDefaultSeed, relief);

  std::string why;
  const uint64_t id = AiSpawn(c, defIndex, {spot.x, spot.y + 1, spot.z},
                              "swordsman_static", why);
  if (id == 0) {
    detail = why;
    return Status::Fail;
  }

  // Arithmetic shift, not divide: a residency window that has streamed to
  // negative world coordinates makes `/ 16` round toward zero and name the
  // wrong chunk. The player chunk is also the fixture's own, so the tick
  // stream does not shift the window out from under the mob mid-gate.
  AiTicker tick{c, 7200, {spot.x >> 4, spot.y >> 4, spot.z >> 4}};
  tick();
  const Vec3 p0 = c.mobs.MobOrigin(id);

  // Deliberately NOT multiples of 45 or 90 degrees. The terrain fan has eight
  // directions and the legacy avoidance aims at 0.6 of a probe offset; a
  // quantized implementation lands on those, and a gate that tested them would
  // pass on exactly the bug it exists to catch.
  const float kBearings[] = {0.9f, 2.6f, -1.7f, -3.0f, 0.35f};
  const float radius = (float)BaselineNumber("aiFaceRadiusVox", 16);
  const int settle = (int)BaselineNumber("aiFaceSettleTicks", 70);
  const float tol = (float)BaselineNumber("aiFaceToleranceRad", 0.12);

  float worstErr = 0, maxDrift = 0;
  int perceived = 0;
  int attacks = 0;
  for (float b : kBearings) {
    const int tx = spot.x + (int)std::lround(std::sin(b) * radius);
    const int tz = spot.z + (int)std::lround(std::cos(b) * radius);
    const int th = World::TerrainHeight(tx, tz, kDefaultSeed);
    c.mobs.SetPlayerActor(Vec3{(float)tx, (float)th + 8.0f, (float)tz}, 3.0f,
                          17.0f, true);
    for (int i = 0; i < settle; i++) {
      tick();
      maxDrift = std::max(maxDrift, AiPlanar(c.mobs.MobOrigin(id), p0));
    }
    const ai::Brain* br = c.mobs.MobBrain(id);
    if (br != nullptr && br->hasTarget) perceived++;
    // The bearing the mob SHOULD hold is the one to the target's centre from
    // its own centre, which is not the same as `b` once the rig's min corner
    // and its half-extent are accounted for. Deriving it rather than restating
    // `b` is what stops this being a circular assertion.
    const Vec3 me = AiMobCentre(c.mobs, id, def);
    const float want =
        std::atan2((float)tx - me.x, (float)tz - me.z);
    const float err =
        std::abs(std::remainder(c.mobs.MobHeading(id) - want, 6.2831853f));
    worstErr = std::max(worstErr, err);
    attacks += (int)c.mobs.AttackRequests().size();
    c.mobs.ClearAttackRequests();
  }

  const float driftMax = (float)BaselineNumber("aiFaceMaxDriftVox", 0.001);
  // Same reason as ai-dummy's: a despawned mob has a foot drift of zero.
  const bool stillHere = c.mobs.IsAlive(id);
  const int nB = (int)(sizeof(kBearings) / sizeof(float));
  const bool ok = stillHere && worstErr <= tol && maxDrift <= driftMax &&
                  perceived == nB;
  detail = Format(
      "%d bearings, alive %d, worst facing error %.3f rad (<= %.3f), foot drift "
      "%.5f vox (<= %.3f), perceived %d/%d, %d attack requests, relief %d",
      nB, stillHere ? 1 : 0, worstErr, tol, maxDrift, driftMax, perceived, nB,
      attacks, relief);
  c.mobs.ClearPlayerActor();
  c.mobs.ClearAttackRequests();
  c.debris.Reset();
  c.mobs.Reset();
  return ok ? Status::Pass : Status::Fail;
}

// ---- ai-approach -----------------------------------------------------------
// ---- ai-reach --------------------------------------------------------------
//
// Owner report, 2026-09-16: NPCs "circle and stay out of reach", a mace swings
// rarely, a dagger never swings, and a zombie does not bite. Four symptoms, one
// cause, and NOTHING IN THE SUITE COULD SEE ANY OF IT — which is the first
// thing this gate is for.
//
// The hole was structural rather than an oversight: every MOBILE duelist
// fixture in the suite equips a sword (AiSpawn does it unasked), and the only
// unarmed fixture is `swordsman_static`, which is immobile and hand-placed at
// four voxels. So the suite tested exactly the one configuration that worked.
// The band and the profile floor are BOTH sword numbers — `duelist` authors
// reach 10 and a band of 7..11 — and nothing that ever held anything shorter
// was allowed to move its own feet. Measured: `duel`, `ai-approach`,
// `npc-styles`, `lunge` and `bite-target` are byte-identical across the fix.
//
// THE SUBJECT IS THE COUPLING BETWEEN A WEAPON'S REACH AND A CREATURE'S FEET,
// so the fixture is the same duelist four times over holding four different
// lengths of steel, allowed to walk. The claim is that each one closes to
// somewhere it can actually hit from and then HITS — not that it issues
// requests, which is what `ai-approach` asserts and which stayed perfectly
// green through the whole bug. A request that `BeginStroke` drops is
// indistinguishable from a request that lands unless you count the cuts.
//
// The dagger arm is the one that fails without the fix: reach about 3.5 against
// a band floor of 7, so every swing is refused for being out of its own reach
// and the creature orbits forever at sword distance. The sword arm is the
// control in the other direction — it must NOT move, because the band's mid
// (9.0) already sits on what a sword lands at, and a fix that "improved" the
// armed case would be retuning every other NPC gate behind this one's back.
Status GateAiReach(Ctx& c, std::string& detail) {
  c.debris.Reset();
  c.mobs.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  const int defIndex = AiHumanoidDef(c.mobs);
  if (defIndex < 0) {
    detail = "no mob def publishes a held_right socket";
    return Status::Fail;
  }
  const MobDef& def = c.mobs.Defs()[defIndex];
  if (c.mobs.Behaviors().Find("duelist") < 0 ||
      c.mobs.Behaviors().Find("training_dummy") < 0) {
    detail = "behaviors.json is missing \"duelist\" or \"training_dummy\"";
    return Status::Fail;
  }

  int relief = 0;
  const IVec3 anchor = AiFixtureCentre(c.world);
  const IVec3 spot = AiFlatSpot(anchor.x, anchor.z, 96, 22, kDefaultSeed, relief);
  const int gapVox = (int)BaselineNumber("aiReachGapVox", 18);
  const int ticks = (int)BaselineNumber("aiReachTicks", 420);

  // THE PLAYER IS PUT OUT OF THE WAY. `SetPlayerActor` publishes an actor of
  // faction "player", and a hostile duelist takes the NEAREST target of another
  // faction — so a player left at the origin (or left over from an earlier
  // gate, which is the real hazard in a shared-World suite) silently becomes
  // the subject and the dummy is never approached at all.
  c.mobs.ClearPlayerActor();

  struct Arm {
    const char* name;
    const char* item;   // nullptr = bare fists
    // nullptr = the `duelist` humanoid. A named def/profile pair is how the
    // ZOMBIE arm gets in: its whole repertoire is natural weapons, so it is the
    // case where "the band follows what you fight with" is not about steel at
    // all — and it is the one the owner actually reported.
    const char* defName = nullptr;
    const char* profile = nullptr;
    float reach = 0;    // StrikeReachOf, world voxels
    int requests = 0;
    int cutTicks = 0;
    int hits = 0;
    float settled = 0;  // mean centre-to-centre over the last third of the fight
    uint32_t lost = 0;  // flesh voxels taken off the dummy
    float hp0 = 0, hp1 = 0;
    int killTick = -1;  // >= 0 = the quarry died and the run stopped there
  };
  // Longest to shortest, which is also most-likely-to-work to least: the
  // ordering is not load-bearing but it makes the printed line read as a ramp.
  Arm arms[] = {{"sword", "sword"},
                {"mace", "mace"},
                {"dagger", "dagger"},
                {"fists", nullptr},
                // THE BITE. Owner report, verbatim: "the zombies also aren't
                // biting at all". Same defect seen from the other side — its
                // list is 50/50 between a 23-voxel lunging bite and a 3.3-voxel
                // standing one, and the draw was blind to distance, so every
                // roll that landed on the short one was thrown away by
                // `BeginStroke` with the 48-tick cadence already spent.
                {"zombie-bite", nullptr, "zombie", "zombie"}};

  AiTicker tick{c, 7800, {spot.x >> 4, spot.y >> 4, spot.z >> 4}};

  for (Arm& a : arms) {
    c.mobs.Reset();
    c.debris.Reset();

    // The quarry first and standing still: `training_dummy` is blind, passive,
    // immobile and of faction "quarry", so it is a target that never fights
    // back and never walks out of the measurement.
    const int tz = spot.z + gapVox;
    const int th = World::TerrainHeight(spot.x, tz, kDefaultSeed);
    std::string why;
    const uint64_t target = c.mobs.Spawn(
        defIndex, {spot.x - (int)(def.worldSize.x * 0.5f), th + 1,
                   tz - (int)(def.worldSize.z * 0.5f)});
    if (target == 0 || !c.mobs.SetMobBehavior(target, "training_dummy")) {
      detail = std::string("could not place the quarry for arm ") + a.name;
      return Status::Fail;
    }

    // ...and the subject, ARMED BEFORE IT IS TICKED. `StrikeReachOf` asks the
    // LIVE weapon how long it is, so a creature ticked before it is handed its
    // mace spends those ticks placing its feet for a fist.
    int attDef = defIndex;
    if (a.defName != nullptr) {
      attDef = -1;
      for (size_t k = 0; k < c.mobs.Defs().size(); k++)
        if (c.mobs.Defs()[k].name == a.defName) attDef = (int)k;
      if (attDef < 0) {
        detail = std::string("no mob def named \"") + a.defName + "\"";
        return Status::Fail;
      }
    }
    const MobDef& adef = c.mobs.Defs()[attDef];
    const uint64_t att = c.mobs.Spawn(
        attDef, {spot.x - (int)(adef.worldSize.x * 0.5f), spot.y + 1,
                 spot.z - (int)(adef.worldSize.z * 0.5f)});
    if (att == 0 ||
        !c.mobs.SetMobBehavior(att, a.profile ? a.profile : "duelist")) {
      detail = std::string("could not place the attacker for arm ") + a.name;
      return Status::Fail;
    }
    if (a.item != nullptr) {
      const ItemDef* it = c.items.At(c.items.Find(a.item));
      if (it == nullptr || !c.mobs.EquipItem(att, it)) {
        detail = std::string("could not equip \"") + a.item + "\"";
        return Status::Fail;
      }
    }
    c.mobs.SetHeading(att, 0.0f);

    // A few ticks to settle the rig onto the ground BEFORE the reach is read:
    // the arm's bone lengths come off a live pose.
    for (int i = 0; i < 4; i++) tick();
    if (Mob* m = c.mobs.FindMobById(att)) a.reach = c.mobs.StrikeReachOf(*m);

    const uint32_t flesh0 = [&] {
      uint32_t n = 0;
      Mob* m = c.mobs.FindMobById(target);
      if (m == nullptr) return n;
      for (int i = 0; i < m->AppendedBase(); i++)
        if (c.mobs.LimbBody(target, i)) n += c.mobs.LimbVoxelCount(target, i);
      return n;
    }();

    a.hp0 = c.mobs.TotalHp(target);
    // ---- ONLY WHILE THERE IS SOMETHING TO STAND OFF FROM -------------------
    //
    // Sampled into a list and truncated at the kill rather than averaged over a
    // fixed tail, because the tail is not always a fight. Measured: the mace
    // arm beat the dummy to death around tick 250, the duelist lost its target
    // (`Perceive` drops a dead one), went idle and drifted — and a last-third
    // mean reported it "standing off" at 17.7 voxels, which is a reading of a
    // creature with nobody to fight. A stand-off measured after the duel is
    // over is not a stand-off.
    std::vector<float> dists;
    dists.reserve((size_t)ticks);
    int prevHit = 0;
    for (int i = 0; i < ticks; i++) {
      tick();
      a.requests += (int)c.mobs.AttackRequests().size();
      c.mobs.ClearAttackRequests();
      // CONTACT EVENTS, NOT THE WIDEST SWEEP. `NpcStroke::bodiesHit` is a
      // per-stroke counter cleared when the stroke resets, so the `max` the
      // other gates take is the breadth of ONE swing — 20 for a sword crossing
      // a torso, 1 for a punch — and comparing that number between weapons
      // measures the shape of the sweep rather than how often the creature
      // connected. Counting the RISES totals the contacts across every stroke.
      if (const NpcStroke* s = c.mobs.MobStroke(att)) {
        if (s->Cutting()) a.cutTicks++;
        if (s->bodiesHit > prevHit) a.hits += s->bodiesHit - prevHit;
        prevHit = s->bodiesHit;
      } else {
        prevHit = 0;
      }
      if (!c.mobs.IsAlive(target)) {
        a.killTick = i;
        break;
      }
      // THE LIVE POSITIONS OF BOTH. Measured against a SNAPSHOT of the target's
      // centre first, which quietly turned "how far apart did they fight" into
      // "how far has the target been knocked from where it started": the mace
      // arm shoves the dummy across the ground, and the gate reported a
      // stand-off of 15.3 voxels for two bodies that were touching.
      dists.push_back(
          AiPlanar(AiMobCentre(c.mobs, att, adef), AiMobCentre(c.mobs, target, def)));
    }
    a.hp1 = c.mobs.IsAlive(target) ? c.mobs.TotalHp(target) : 0.0f;
    // The last third of the LIVE ticks: the first two are the walk in, and
    // averaging an 18-voxel approach into the stand-off reports a number the
    // creature never stood at.
    if (!dists.empty()) {
      const size_t from = dists.size() - dists.size() / 3;
      double s = 0;
      for (size_t k = from; k < dists.size(); k++) s += dists[k];
      a.settled = (float)(s / (double)(dists.size() - from));
    }
    const uint32_t flesh1 = [&] {
      uint32_t n = 0;
      Mob* m = c.mobs.FindMobById(target);
      if (m == nullptr) return n;
      for (int i = 0; i < m->AppendedBase(); i++)
        if (c.mobs.LimbBody(target, i)) n += c.mobs.LimbVoxelCount(target, i);
      return n;
    }();
    a.lost = flesh0 > flesh1 ? flesh0 - flesh1 : 0u;
  }

  c.mobs.Reset();
  c.debris.Reset();

  // ---- the claims ---------------------------------------------------------
  //
  // ONE PER ARM, AND IT IS ABOUT CUTS RATHER THAN REQUESTS. See the header: the
  // whole defect lived in the gap between deciding to attack and a swing
  // existing, so a gate that counts decisions measures the half that was never
  // broken.
  //
  // AND "LANDED" IS NOT "HIT ONCE". The first run of this gate passed on one
  // body-hit and zero voxels for three of its four arms, which is a creature
  // that grazed something once in fourteen seconds — the band had moved but
  // was still placed for a punch the armed creature would never throw
  // (MobSystem::StrikeReachOf). A fluke contact is exactly what a
  // marginally-too-far fighter produces, so the floor has to be above one.
  const int minHits = (int)BaselineNumber("aiReachMinHits", 3);
  bool allSwung = true, allLanded = true;
  for (const Arm& a : arms) {
    if (a.cutTicks <= 0) allSwung = false;
    if (a.hits < minHits) allLanded = false;
  }
  // ...AND THE BLOWS MATTERED. Measured as hp rather than as voxels, because
  // voxels are not a fair common currency here: a mace authors `cut` 0 and a
  // dagger's kerf is small enough on purpose ("it will not take an arm off and
  // it was never going to") that 21 landed hits removed nothing measurable,
  // while the sword stripped the target's whole 3328. hp is what every one of
  // the four is trying to do, so hp is the claim they can all be held to.
  bool allHurt = true;
  for (const Arm& a : arms)
    if (!(a.killTick >= 0 || a.hp1 < a.hp0)) allHurt = false;
  // ...AND THE FEET FOLLOWED THE STEEL. A dagger fighter must stand closer than
  // a swordsman; without that the arms above could all pass on a fixture that
  // simply spawned everything within arm's reach. Compared between arms rather
  // than against a literal, so re-proportioning the rig or re-authoring a blade
  // moves both sides together.
  const Arm& sword = arms[0];
  const Arm& dagger = arms[2];
  const bool closer = dagger.settled < sword.settled;
  // The sword arm is the NO-OP CONTROL. `duelist`'s band is 7..11 and its mid
  // is 9.0; a sword lands at about the same, so the shift is zero and the arm
  // must still stand in the authored band. A fix that dragged the armed case
  // inward would be silently retuning `duel`, `crowd` and `ai-approach`.
  const float tol = (float)BaselineNumber("aiReachBandTolVox", 2.5);
  const bool swordUnmoved =
      sword.settled >= 7.0f - tol && sword.settled <= 11.0f + tol;

  RecordObserved("aiReachSwordSettled", (double)sword.settled);
  RecordObserved("aiReachDaggerSettled", (double)dagger.settled);
  RecordObserved("aiReachFistCutTicks", (double)arms[3].cutTicks);
  RecordObserved("aiReachDaggerCutTicks", (double)dagger.cutTicks);

  const bool ok = allSwung && allLanded && allHurt && closer && swordUnmoved;
  std::string per;
  for (const Arm& a : arms)
    per += Format(
        "%s(reach %.1f: %d req, %d cut, %d hit, hp %.0f->%.0f%s, %u vox, stood "
        "%.1f) ",
        a.name, a.reach, a.requests, a.cutTicks, a.hits, a.hp0, a.hp1,
        a.killTick >= 0 ? Format(" KILLED t%d", a.killTick).c_str() : "",
        a.lost, a.settled);
  detail = Format(
      "%d ticks from %d vox, band [7,11]: %s| every arm swung=%d landed>=%d=%d, "
      "hurt it=%d, dagger closer than sword=%d, sword still in band=%d",
      ticks, gapVox, per.c_str(), allSwung ? 1 : 0, minHits,
      allLanded ? 1 : 0, allHurt ? 1 : 0,
      closer ? 1 : 0, swordUnmoved ? 1 : 0);
  return ok ? Status::Pass : Status::Fail;
}

Status GateAiApproach(Ctx& c, std::string& detail) {
  c.debris.Reset();
  c.mobs.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  const int defIndex = AiHumanoidDef(c.mobs);
  if (defIndex < 0) {
    detail = "no mob def publishes a held_right socket";
    return Status::Fail;
  }
  const MobDef& def = c.mobs.Defs()[defIndex];
  const int prof = c.mobs.Behaviors().Find("duelist");
  if (prof < 0) {
    detail = "no \"duelist\" profile in assets/mobs/behaviors.json";
    return Status::Fail;
  }
  const ai::Profile& dp = c.mobs.Behaviors().profiles[prof];

  int relief = 0;
  const IVec3 anchor = AiFixtureCentre(c.world);
  const IVec3 spot =
      AiFlatSpot(anchor.x, anchor.z, 96, 22, kDefaultSeed, relief);
  const int gapVox = (int)BaselineNumber("aiApproachGapVox", 20);
  const int tx = spot.x, tz = spot.z + gapVox;
  const int th = World::TerrainHeight(tx, tz, kDefaultSeed);

  // Arithmetic shift, not divide: a residency window that has streamed to
  // negative world coordinates makes `/ 16` round toward zero and name the
  // wrong chunk. The player chunk is also the fixture's own, so the tick
  // stream does not shift the window out from under the mob mid-gate.
  AiTicker tick{c, 7400, {spot.x >> 4, spot.y >> 4, spot.z >> 4}};

  // ---- the wall --------------------------------------------------------
  // Stone spheres straddling the straight line, at each column's own terrain
  // height so undulation cannot bury or float them. Seven voxels tall above the
  // ground: a body cannot step it (SenseGround refuses a rise past 2), and a
  // ray from the mob's eye down to the target's chest clears it — so the
  // duelist can SEE its target the whole time and the only thing being tested
  // is whether it can WALK to it. A wall that also broke line of sight would
  // make a perception failure look like a navigation failure.
  //
  // CELL OPS, NOT A BRUSH — A SPHERE IS A RAMP. The brush only paints spheres,
  // and a fence of them is climbable: near a sphere's edge the surface rises
  // about two voxels per column, which is exactly the planner's step limit. So
  // A* cheerfully routed OVER the "wall" while the drive — whose probe looks
  // several voxels ahead and refuses anything more than two up from where it
  // stands — refused to walk the route it was given, and the mob stood at the
  // foot of the rubble for 600 ticks. The column census made it unambiguous:
  // 0 unknown columns, 66 steep ones, and a waypoint 27 voxels dead ahead.
  //
  // A real wall is vertical. Written cell by cell, one voxel thick, so the rise
  // from the ground in front of it to its top happens in ONE column step and
  // neither the planner nor the drive can talk itself up it.
  const uint32_t stone = [&] {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == "stone") return (uint32_t)i;
    return 1u;
  }();

  // ---- THE CREATURE FIRST, AND PINNED ------------------------------------
  //
  // Two reasons, both learned the hard way, and both about the fixture rather
  // than about the AI.
  //
  //   * The wall's height has to be measured against THIS BODY'S STRIDE, and
  //     the only honest source for that is the body (Mob::StepUpCells, resolved
  //     from the rig). A literal was fine while every mob stepped 0.20 m and
  //     became a test of nothing the day they got the player's 0.58 m.
  //   * A mob with no profile WANDERS. Every tick spent writing the wall and
  //     pulling it into the mirror was a tick the subject spent walking, and it
  //     arrived at the measured loop wherever it happened to get to — once
  //     already past the barrier. `dummy` is the authored profile whose whole
  //     content is `mobile: false`, so the creature stands still until the
  //     fixture is built and the duel is switched on below.
  //
  // Spawn so the mob's CENTRE lands on the flat spot: Spawn takes the prefab's
  // min corner, and a humanoid's box is wide enough that ignoring that puts the
  // creature half a body off the line the wall is built across.
  std::string why;
  const uint64_t id =
      AiSpawn(c, defIndex,
              {spot.x - (int)(def.worldSize.x * 0.5f), spot.y + 1,
               spot.z - (int)(def.worldSize.z * 0.5f)},
              "dummy", why);
  if (id == 0) {
    detail = why;
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    return Status::Fail;
  }
  const int stepUp = c.mobs.MobStepUpCells(id);
  const float mobStand = (float)(spot.y + 1);
  // THE WALL'S HEIGHT IS RELATIVE TO THE CREATURE, NOT A LITERAL 8.
  //
  // It was eight voxels above EACH COLUMN'S OWN TERRAIN, which is two
  // assumptions that both went stale the moment mobs were given the player's
  // stride (0.58 m, five cells) instead of their old 0.20 m:
  //
  //   * eight is only a wall to a body that cannot step five, and
  //   * "above its own column" is not "above the approach" on undulating
  //     ground — with three voxels of relief the mob walked up to a barrier
  //     whose top was four voxels above ITS feet and stepped onto it, exactly
  //     as it would a kerb.
  //
  // The gate then reported "crossed the wall plane, 0.2 voxels from centre",
  // which reads like walking through solid rock and was in fact a legal climb
  // over a fixture that had quietly stopped being a wall. So: one ABSOLUTE top,
  // derived from the tallest ground anywhere near the approach plus twice any
  // sane step budget, and every column filled up to it. A fixture that measures
  // itself against the thing under test cannot rot when that thing is retuned.
  const int kWallHalfX = 16;
  int wallTop = 0, wallRise = 0;
  {
    const int wz = spot.z + gapVox / 2;
    int highest = th;
    for (int wx = spot.x - kWallHalfX; wx <= spot.x + kWallHalfX; wx++)
      for (int dz = -2; dz <= 3; dz++)
        highest = std::max(highest,
                           World::TerrainHeight(wx, wz + dz, kDefaultSeed));
    highest = std::max(highest, World::TerrainHeight(spot.x, spot.z, kDefaultSeed));
    // TWO CELLS PAST WHAT THIS BODY CAN STEP, measured from where it STANDS —
    // and no taller, because the gate also needs the duelist to SEE its target
    // over the barrier the whole time (a wall that breaks line of sight makes a
    // perception failure look like a navigation failure, which is the one
    // confusion this fixture exists to avoid). Also never below the local
    // terrain, or undulation punches a hole in it.
    wallTop = std::max(highest + 1, (int)mobStand + stepUp + 2);
    wallRise = wallTop - (int)mobStand;
    std::vector<CellOp> wall;
    for (int wx = spot.x - kWallHalfX; wx <= spot.x + kWallHalfX; wx++) {
      const int wh = World::TerrainHeight(wx, wz, kDefaultSeed);
      // Two voxels thick: the ground probe samples columns, and a one-voxel
      // sheet can be stepped across diagonally between two column centres.
      for (int dz = 0; dz < 2; dz++)
        for (int y = wh + 1; y <= wallTop; y++) {
          const IVec3 cell{wx, y, wz + dz};
          if (!c.world.CellInWindow(cell)) continue;
          wall.push_back(CellOp{World::SlotCellIndex(cell), stone});
        }
    }
    // Several thousand cells now; feed the queue in slices rather than assuming
    // one submission swallows them all.
    const size_t kSlice = 4000;
    for (size_t i = 0; i < wall.size(); i += kSlice)
      tick({}, std::vector<CellOp>(
                   wall.begin() + (ptrdiff_t)i,
                   wall.begin() + (ptrdiff_t)std::min(i + kSlice, wall.size())));
  }
  // ---- get the wall into the mirror the NAVIGATOR reads --------------------
  //
  // Two different CPU mirrors exist and they are not interchangeable.
  // `World::KindAt` reads the 3x3x3 SNAPSHOT; the ground probe, and therefore
  // every question this planner asks, reads the CHUNK CACHE (`World::Cached`).
  // The cache is only refreshed when a chunk is fetched — and a chunk another
  // gate already pulled in stays at that version forever unless something asks
  // again. So this gate PASSED standalone and FAILED in the suite: an earlier
  // gate had cached these chunks before the wall existed, the navigator saw
  // open ground, never planned a detour, and the mob strolled through a wall
  // its own drive probe could not see either.
  //
  // Fetch explicitly, and then assert against the CACHE rather than against
  // KindAt — record at the point the failure would actually happen.
  auto cachedSolid = [&](IVec3 cell) {
    const CachedChunk* cc = c.world.Cached({cell.x >> 4, cell.y >> 4, cell.z >> 4});
    if (cc == nullptr || cc->voxels.size() != kChunkVol) return false;
    const uint32_t m =
        cc->voxels[(((uint32_t)cell.z & 15u) * kChunk + ((uint32_t)cell.y & 15u)) *
                       kChunk +
                   ((uint32_t)cell.x & 15u)] &
        0xFFFu;
    return m != 0;
  };
  {
    const int wz = spot.z + gapVox / 2;
    for (int i = 0; i < 60; i++) {
      bool all = true;
      for (int wx = spot.x - kWallHalfX; wx <= spot.x + kWallHalfX; wx += 4) {
        const IVec3 cell{wx, wallTop, wz};
        if (cachedSolid(cell)) continue;
        all = false;
        c.world.RequestChunkFetch({cell.x >> 4, cell.y >> 4, cell.z >> 4});
        c.world.RequestChunkFetch({cell.x >> 4, cell.y >> 4, (cell.z + 1) >> 4});
      }
      if (all) break;
      tick();
    }
  }

  // ---- THE SIGHT LINE RISES WITH THE WALL --------------------------------
  // The target used to sit a flat eight voxels over its own ground, which
  // cleared a seven-voxel barrier and nothing taller. Solve for it instead:
  // the ray from the mob's chest to the target's passes over the wall plane at
  // the midpoint of the gap, so putting the target where that midpoint clears
  // `wallTop` by three keeps the duelist's eyes on it for ANY wall this gate
  // decides it needs — which is the property the fixture actually wants, rather
  // than a number that happened to work once.
  const float mobChest = mobStand + def.worldSize.y * 0.5f;
  const float targetY =
      std::max((float)th + 8.0f, 2.0f * ((float)wallTop + 3.0f) - mobChest);
  c.mobs.SetPlayerActor(Vec3{(float)tx, targetY, (float)tz}, 3.0f, 17.0f, true);
  // ...and NOW it is a duelist. Everything above ran with the creature pinned.
  if (!c.mobs.SetMobBehavior(id, "duelist")) {
    detail = "no behaviour profile \"duelist\"";
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    return Status::Fail;
  }
  // THE CROSSING DETECTOR HAS TO WATCH THE WARM-UP TICKS TOO.
  //
  // It did not, and the failure that taught this is the exact one `ai-slope`
  // hit from the other direction: these twenty ticks are a creature walking
  // with nobody looking, and the fastest rig in the pack covers a voxel a tick.
  // When it happened to be past the wall by the first measured sample, the gate
  // recorded "crossed at tick 0, 0.2 voxels from the wall's centre" — which
  // reads exactly like walking THROUGH the barrier and was in fact a detour
  // completed off-camera. Two runs went into telling those apart, and the same
  // fixture ran GREEN standalone and RED in-suite purely because the residency
  // window put it on different terrain and changed how far it got.
  //
  // So the crossing is recorded from the tick the creature exists, and a
  // crossing during the warm-up is reported with a NEGATIVE tick rather than
  // being silently attributed to the first observed frame.
  const float wallZ = (float)(spot.z + gapVox / 2);
  const float wallHalfX = (float)kWallHalfX;
  float crossX = 0, crossY = 0;
  int crossTick = INT32_MIN;
  int stepNo = -20;
  auto noteCross = [&]() {
    if (crossTick != INT32_MIN) return;
    const Vec3 ctr = AiMobCentre(c.mobs, id, def);
    if (ctr.z <= wallZ) return;
    crossTick = stepNo;
    crossX = std::abs(ctr.x - (float)spot.x);
    crossY = c.mobs.MobOrigin(id).y;   // OVER the wall and AROUND it differ
  };
  for (int i = 0; i < 20; i++) {   // anchors fetch the chunks around the mob
    tick();
    noteCross();
    stepNo++;
  }

  // Does the CHUNK CACHE have the wall, and is it CONTINUOUS? Every column, at
  // the ground+2 the step-up limit cares about, read out of the same mirror the
  // ground probe reads. Not KindAt: see the note above — the snapshot reported
  // a complete wall the navigator could not see at all.
  int wallSeen = 0, wallCols = 0;
  {
    const int wz = spot.z + gapVox / 2;
    for (int wx = spot.x - kWallHalfX; wx <= spot.x + kWallHalfX; wx++) {
      wallCols++;
      // AT THE TOP, not at ground+2. The claim that matters is "the barrier is
      // as tall as this gate built it, everywhere" — a mirror that has the
      // bottom of the wall and not the top is precisely the state in which a
      // creature steps over it.
      if (cachedSolid(IVec3{wx, wallTop, wz})) wallSeen++;
    }
  }

  // ---- the run ---------------------------------------------------------
  const int budget = (int)BaselineNumber("aiApproachTicks", 400);
  const int holdTicks = (int)BaselineNumber("aiApproachHoldTicks", 240);
  const float bandTol = (float)BaselineNumber("aiApproachBandTolVox", 2.5);
  const float minClear = (float)BaselineNumber("aiApproachMinClearVox", 3.0);

  const Vec3 targetC{(float)tx, targetY, (float)tz};
  const float startDist = AiPlanar(AiMobCentre(c.mobs, id, def), targetC);
  int arriveTick = -1;
  float minDist = 1e9f;
  int attacks = 0;
  bool everPathed = false;
  // ATTRIBUTION, not a bare number. "Arrived at tick 161" is exactly the kind
  // of count CLAUDE.md warns about: it invites turning features off one at a
  // time. Ticks-per-intent plus ground actually covered says WHERE the time
  // went in one line — the first version of this gate reported only the tick
  // and the cause (an orbit the neck could not follow, so every tick was spent
  // at a fraction of drive) was invisible.
  int intentTicks[(int)ai::Intent::Count] = {};
  float travelled = 0;
  // WHERE it crossed the wall's plane, not merely that it got past. A detour is
  // a crossing at |x| beyond the wall's end; walking THROUGH one is a crossing
  // at x ~ 0, and the two are indistinguishable from "arrived at tick N".
  Vec3 prevPos = c.mobs.MobOrigin(id);
  auto step = [&]() {
    tick();
    const Vec3 now = c.mobs.MobOrigin(id);
    travelled += AiPlanar(now, prevPos);
    prevPos = now;
    noteCross();
    stepNo++;
    const ai::Brain* br = c.mobs.MobBrain(id);
    if (br != nullptr) {
      intentTicks[(int)br->intent]++;
      if (br->path.valid) everPathed = true;
    }
    attacks += (int)c.mobs.AttackRequests().size();
    c.mobs.ClearAttackRequests();
    const float d = AiPlanar(AiMobCentre(c.mobs, id, def), targetC);
    minDist = std::min(minDist, d);
    // A trace, not a bare verdict. Where the mob actually WAS, what it was
    // doing and whether it had a plan, sampled thinly enough to read: a gate
    // that only reports "did not arrive" makes the next reader guess between
    // "the planner found nothing", "it found something and could not follow
    // it" and "it oscillated". Cheap and off by default in spirit — 12 lines
    // over a 600-tick run.
    if (br != nullptr && (stepNo % 50) == 0) {
      const Vec3 ctr = AiMobCentre(c.mobs, id, def);
      const bool haveWp = !br->path.Done();
      const Vec3 wp = haveWp ? br->path.Current() : Vec3{};
      std::printf(
          "    ai-approach t%3d: at (%+6.1f,%+6.1f) d %5.1f  %-9s path %2zu/%2zu"
          " wp (%+6.1f,%+6.1f)%s  replans %u  cols %u probed / %u unknown / %u "
          "blocked / %u steep\n",
          stepNo, ctr.x - (float)spot.x, ctr.z - (float)spot.z, d,
          ai::IntentName(br->intent), br->path.cursor, br->path.pts.size(),
          haveWp ? wp.x - (float)spot.x : 0.0f,
          haveWp ? wp.z - (float)spot.z : 0.0f,
          br->navFailed ? "  NAVFAIL" : "", br->replans, br->path.colsProbed,
          br->path.colsUnknown, br->path.colsBlocked, br->path.colsSteep);
    }
    return d;
  };
  for (int i = 0; i < budget && arriveTick < 0; i++) {
    const float d = step();
    if (d >= dp.movement.rangeMin - bandTol && d <= dp.movement.rangeMax + bandTol)
      arriveTick = i;
  }

  // ...and then STAYS there. Arriving is easy; a mob that overshoots into the
  // target, or oscillates across the band edge, or drifts back out because its
  // circle-strafe walks a widening polygon, all pass an arrival-only test and
  // all read as broken.
  int outOfBand = 0;
  float worstOut = 0;
  if (arriveTick >= 0) {
    for (int i = 0; i < holdTicks; i++) {
      const float d = step();
      const float over = std::max(0.0f, d - (dp.movement.rangeMax + bandTol));
      const float under = std::max(0.0f, (dp.movement.rangeMin - bandTol) - d);
      if (over > 0 || under > 0) {
        outOfBand++;
        worstOut = std::max(worstOut, std::max(over, under));
      }
    }
  }

  const int outAllowed = (int)BaselineNumber("aiApproachOutOfBandTicks", 12);
  // The fixture has to be a wall FOR THIS CREATURE. Asserted rather than
  // assumed: a rig re-authored with a longer stride would otherwise turn this
  // gate into a test of nothing, silently and in the passing direction.
  const bool wallIsAWall = wallRise > stepUp;
  // A duel with no swings in it is not a duel. This is the one assertion that
  // ties the whole chain — perceive, path, hold range, aim — to the seam Phase
  // C consumes: the arbiter can score perfectly and still never fire if the
  // footwork keeps the target off the nose, which is exactly the bug the first
  // run of this gate found.
  const int minAttacks = (int)BaselineNumber("aiApproachMinAttacks", 2);
  // The crossing must be a DETOUR: past the end of the wall, not through it.
  // Without this the gate is satisfied by a mob that walks straight over a
  // 6-voxel step the locomotion is supposed to refuse, which would make it a
  // test of nothing.
  const bool detoured =
      crossTick != INT32_MIN && crossX > wallHalfX * 0.6f;
  const bool ok = wallSeen == wallCols && wallIsAWall && arriveTick >= 0 &&
                  everPathed && detoured &&
                  minDist >= minClear && outOfBand <= outAllowed &&
                  attacks >= minAttacks;
  RecordObserved("aiApproachArriveTick", (double)arriveTick);
  RecordObserved("aiApproachAttacks", (double)attacks);
  detail = Format(
      "wall %d/%d columns in mirror (half-width %.0f, rises %d vox over the "
      "approach vs a %d-cell stride: wall %d), start %.1f vox, crossed the wall "
      "plane at tick %d, |x| %.1f from centre at y %+.1f (detour %d), arrived "
      "tick %d/%d "
      "(%.0f vox walked), pathed %d, band [%.1f,%.1f]+-%.1f: %d/%d ticks out "
      "(worst %.2f), closest %.2f (>= %.1f), %d attacks (>= %d), %u replans, "
      "intents idle/face/appr/hold/circ/atk %d/%d/%d/%d/%d/%d, relief %d",
      wallSeen, wallCols, wallHalfX, wallRise, stepUp, wallIsAWall ? 1 : 0,
      startDist, crossTick, crossX,
      crossY - (float)spot.y, detoured ? 1 : 0,
      arriveTick, budget, travelled, everPathed ? 1 : 0, dp.movement.rangeMin,
      dp.movement.rangeMax, bandTol, outOfBand, holdTicks, worstOut, minDist,
      minClear, attacks, minAttacks,
      c.mobs.MobBrain(id) != nullptr ? c.mobs.MobBrain(id)->replans : 0u,
      intentTicks[0], intentTicks[1], intentTicks[2], intentTicks[3],
      intentTicks[4], intentTicks[5], relief);

  c.mobs.ClearPlayerActor();
  c.mobs.ClearAttackRequests();
  c.debris.Reset();
  c.mobs.Reset();
  // The wall is real grid state and the gates after this one place fixtures by
  // absolute coordinate (CLAUDE.md rule 7).
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  return ok ? Status::Pass : Status::Fail;
}

// ---- ai-slope ---------------------------------------------------------------
//
// THE TWO WAYS AN NPC MOVES WRONG ON A HILL, AS TWO NUMBERS.
//
// Everything above this gate tests navigation on FLAT ground: `ai-approach`
// builds a vertical wall precisely so the creature has to go AROUND it, and a
// vertical wall is the one obstacle whose failure mode is visible in a planar
// distance. That left the whole sloped-terrain regime — which is most of the
// world — asserted by nothing at all, and both bugs this gate pins lived there
// for as long as the AI has existed:
//
//   depthVox  How far the body's own min corner ever got BELOW the surface it
//             was standing on. The walk drive settled the origin toward the
//             probed ground at 3 cm a tick while a humanoid walks 3.15 m/s, so
//             on anything steeper than about thirty degrees the body outran its
//             own settle and sank. Once it was under the surface the ground
//             probe — which only ever scanned DOWNWARD — reported the rock
//             beneath the buried body as the floor, every one of the eight
//             sense probes read flat against that fiction, and the creature
//             walked the rest of the way through the hill. Must stay at zero:
//             this is not a tolerance, it is "the body is not inside the
//             ground".
//   tiltDeg   How far the body ever leaned off vertical. The lean came from a
//             plane fitted through the "planted feet", except that the gait
//             counted every IK chain as a foot and a humanoid publishes two
//             legs AND TWO ARMS — so the plane was fitted through two feet and
//             two hands, which on a slope are nowhere near coplanar with the
//             ground. Bounded only by "the normal's y is above 0.6", i.e. 53
//             degrees, in any direction including pure roll. That is the
//             "standing on a ramp and rotated 45 degrees for no reason"
//             complaint, and it is why it only ever showed up on slopes.
//
// A THIRD CLAIM, and the reason the fixture is a ramp rather than a cliff:
// the creature must actually GET UP IT. A body that refuses every slope passes
// both numbers above trivially by standing still, and that was very nearly the
// old behaviour — the mob's step-up budget was a flat 0.20 m against the
// player's 0.58 m, so a kerb the player strides over read as a wall, the
// forward probe went blocked, and the steering deflected sideways. `climbedVox`
// is what separates "does not sink" from "does not move".
//
// THE FIXTURE IS A STAIRCASE, NOT A SMOOTH WEDGE. Each column is one voxel
// higher than the one behind it, so every individual step is comfortably inside
// any sane budget and the gate is testing the SETTLE RATE rather than the step
// rule. A smooth 45-degree wedge would confound the two.
Status GateAiSlope(Ctx& c, std::string& detail) {
  c.debris.Reset();
  c.mobs.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  const int defIndex = AiHumanoidDef(c.mobs);
  if (defIndex < 0) {
    detail = "no mob def publishes a held_right socket";
    return Status::Fail;
  }
  const MobDef& def = c.mobs.Defs()[defIndex];
  if (c.mobs.Behaviors().Find("duelist") < 0) {
    detail = "no \"duelist\" profile in assets/mobs/behaviors.json";
    return Status::Fail;
  }

  int relief = 0;
  const IVec3 anchor = AiFixtureCentre(c.world);
  const IVec3 spot = AiFlatSpot(anchor.x, anchor.z, 96, 26, kDefaultSeed, relief);
  const int h0 = World::TerrainHeight(spot.x, spot.z, kDefaultSeed);

  AiTicker tick{c, 7600, {spot.x >> 4, spot.y >> 4, spot.z >> 4}};

  const uint32_t stone = [&] {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == "stone") return (uint32_t)i;
    return 1u;
  }();

  // ---- the ramp ----------------------------------------------------------
  // Rises from `rampZ0` toward +Z at one voxel per column, then a flat crest to
  // stand the target on. Wide enough (+-kHalfX) that walking round it is not a
  // shortcut inside the planner's radius, so the creature is genuinely being
  // asked to climb.
  // THE RAMP STARTS WELL CLEAR OF THE SPAWN, and that gap is load-bearing
  // rather than tidy. The first version put it four voxels away, and the
  // fastest rig in the pack walks a voxel a tick: the creature had climbed the
  // entire thing during the warm-up ticks, before the measuring loop began, and
  // the gate reported "climbed 0.0" about a mob standing serenely on the crest.
  // A fixture whose subject finishes the test before the test starts is the
  // worst kind of green.
  // The whole fixture has to fit inside the duelist's 46-voxel sight range,
  // measured PLANAR from the spawn — a ramp long enough to be interesting and a
  // crest deep enough to hold on adds up fast, and a creature that never
  // perceives its target reports the same "climbed 0.0" as one that refused the
  // slope. The gate says which (`targeted`) rather than leaving the next reader
  // to guess.
  const int kHalfX = 20, kRampLen = 16, kCrestLen = 18;
  const int rampZ0 = spot.z + 8;
  // ONE VOXEL OF RISE PER VOXEL OF RUN: a 45-degree hill, which is steep and
  // walkable. Tried at 3 first, on the reasoning that `terrain` reports this
  // world at "slope max 3" — and 3 per voxel of run is a 71-degree cliff, which
  // the drive correctly REFUSES after a few columns and the player could not
  // walk either. That is a fixture measuring the step rule, not the thing this
  // gate is for; the rise-per-column number in a terrain report is a maximum
  // over a whole window and includes its cliff faces.
  const int kRampRise = 1;
  const int crestY = h0 + kRampLen * kRampRise;
  {
    std::vector<CellOp> ops;
    for (int dz = 0; dz < kRampLen + kCrestLen; dz++) {
      const int wz = rampZ0 + dz;
      const int topY = h0 + std::min(dz, kRampLen) * kRampRise;
      for (int wx = spot.x - kHalfX; wx <= spot.x + kHalfX; wx++) {
        // From each column's OWN terrain height, so undulation cannot leave the
        // ramp floating over a dip or buried in a rise.
        const int base = World::TerrainHeight(wx, wz, kDefaultSeed);
        // A column whose own terrain already stands above the ramp profile is
        // filled to the terrain, not skipped: `for (y = base; y <= topY)` with
        // base > topY writes nothing at all, and a hole in the middle of the
        // ramp turns this into a test of something else entirely.
        for (int y = base; y <= std::max(topY, base); y++) {
          const IVec3 cell{wx, y, wz};
          if (!c.world.CellInWindow(cell)) continue;
          ops.push_back(CellOp{World::SlotCellIndex(cell), stone});
        }
      }
    }
    // The queue is bounded per tick; feed it in slices rather than assuming one
    // submission swallows a few thousand cells.
    const size_t kSlice = 4000;
    for (size_t i = 0; i < ops.size(); i += kSlice) {
      std::vector<CellOp> slice(ops.begin() + (ptrdiff_t)i,
                                ops.begin() + (ptrdiff_t)std::min(i + kSlice,
                                                                 ops.size()));
      tick({}, slice);
    }
  }

  // ---- get the ramp into the mirror the LOCOMOTION reads -----------------
  // Same trap `ai-approach` documents at length: two CPU mirrors exist and the
  // ground probe reads the CHUNK CACHE, which holds whatever version was last
  // fetched. A chunk an earlier gate pulled in before the ramp existed stays
  // that way until something asks again, and the creature then walks through a
  // ramp its own probe cannot see — which would make this gate pass for the
  // wrong reason.
  auto cachedSolid = [&](IVec3 cell) {
    const CachedChunk* cc =
        c.world.Cached({cell.x >> 4, cell.y >> 4, cell.z >> 4});
    if (cc == nullptr || cc->voxels.size() != kChunkVol) return false;
    const uint32_t m =
        cc->voxels[(((uint32_t)cell.z & 15u) * kChunk +
                    ((uint32_t)cell.y & 15u)) *
                       kChunk +
                   ((uint32_t)cell.x & 15u)] &
        0xFFFu;
    return m != 0;
  };
  // The topmost solid at or below `from` in this column, +1 — the surface a
  // body would stand on, read out of the same mirror the mob reads.
  auto surfaceAt = [&](int wx, int wz, int from) {
    for (int y = from; y > from - 64; y--)
      if (cachedSolid(IVec3{wx, y, wz})) return y + 1;
    return INT32_MIN;
  };
  // EVERY COLUMN, not every third. The loop sampled a stride of three and the
  // census below checked all of them, so it broke out as soon as its OWN
  // samples were cached while columns in between — in chunks nothing had asked
  // for — were still missing. That reported "ramp 8/16 columns in mirror" in
  // one suite run and 16/16 in the next, from identical code: a check whose
  // exit condition is weaker than its assertion is a flaky check, not a
  // tolerant one.
  for (int i = 0; i < 120; i++) {
    bool all = true;
    for (int dz = 0; dz < kRampLen; dz++) {
      const IVec3 cell{spot.x, h0 + dz * kRampRise, rampZ0 + dz};
      if (cachedSolid(cell)) continue;
      all = false;
      c.world.RequestChunkFetch({cell.x >> 4, cell.y >> 4, cell.z >> 4});
    }
    if (all) break;
    tick();
  }
  int rampSeen = 0, rampCols = 0;
  for (int dz = 0; dz < kRampLen; dz++) {
    rampCols++;
    if (cachedSolid(IVec3{spot.x, h0 + dz * kRampRise, rampZ0 + dz})) rampSeen++;
  }

  // ---- the run -----------------------------------------------------------
  // Target on the crest, so "walk to the thing" IS "climb the ramp". The mob
  // starts on the flat below it.
  // FOURTEEN VOXELS ONTO THE CREST, not at its lip. The duelist's stand-off
  // band is 7..11 voxels and it is measured PLANAR — the intent layer has no
  // opinion about height at all — so a target parked at the top of the slope is
  // satisfied by a creature standing part-way up it, and "climbed 16 of 22"
  // would have been a pass for a body that never reached the top. Putting the
  // target deep enough onto the flat means the band can only be met from the
  // crest itself.
  const int targetZ = rampZ0 + kRampLen + 12;
  // ---- THE TARGET HAS TO BE VISIBLE FROM THE BOTTOM OF THE RAMP ----------
  // A creature standing below a plateau cannot see anything standing ON it:
  // the sight ray grazes the crest lip, which is solid, and `requireLos` then
  // reports no target at all. The gate duly said "climbed 0.0" about a mob that
  // was idle because it had nothing to walk towards — a PERCEPTION failure
  // wearing a locomotion failure's clothes, which is the exact confusion
  // ai-approach documents at length about its wall.
  //
  // So the target is lifted until the ray clears the lip. It floats, and that
  // is fine: everything the duelist decides with it -- the stand-off band, the
  // attack reach -- is measured PLANAR, so the height changes nothing except
  // whether the creature can see the carrot it is being asked to climb towards.
  // `targeted` is reported, so if this ever stops working it names itself.
  const float mobChest = (float)(h0 + 1) + def.worldSize.y * 0.5f;
  const float lipT = (float)(rampZ0 + kRampLen - spot.z) /
                     std::max(1.0f, (float)(targetZ - spot.z));
  const float targetY =
      std::max((float)crestY + 8.0f,
               mobChest + ((float)crestY + 3.0f - mobChest) / std::max(lipT, 0.1f));
  c.mobs.SetPlayerActor(Vec3{(float)spot.x, targetY, (float)targetZ}, 3.0f,
                        17.0f, true);

  std::string why;
  const uint64_t id =
      AiSpawn(c, defIndex,
              {spot.x - (int)(def.worldSize.x * 0.5f), h0 + 1,
               spot.z - (int)(def.worldSize.z * 0.5f)},
              "duelist", why);
  if (id == 0) {
    detail = why;
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    return Status::Fail;
  }
  // NO WARM-UP TICKS, and that is the second half of the fixture lesson above.
  // A warm-up is ground the creature covers with nobody watching, and on a rig
  // that walks a voxel a tick it was enough to climb most of the ramp before
  // the first depth sample was taken — so the one measurement this gate exists
  // for was being skipped over exactly where it mattered. The ramp chunks are
  // already in the mirror (the fetch loop above proved it column by column) and
  // the mob's own anchors fetch what it stands on within a tick or two; until
  // they do the drive simply declines to walk, which costs a few ticks of the
  // budget and nothing else.
  const int budget = (int)BaselineNumber("aiSlopeTicks", 700);
  // The SPAWN height, so `climbed` is the whole climb rather than whatever was
  // left of it when the measuring started.
  const float startY = (float)(h0 + 1);
  float maxDepth = 0, maxDrawnDepth = 0, maxTiltDeg = 0, topY = startY;
  int buriedTicks = 0, deepestTick = -1;
  int drawnBuriedTicks = 0, drawnDeepestTick = -1;
  Vec3 worstAt{};
  int ranTicks = 0;
  int intentTicks[(int)ai::Intent::Count] = {};
  bool everTargeted = false, everPathed = false;
  float nearestZ = 1e9f;
  for (int i = 0; i < budget; i++) {
    tick();
    ranTicks = i + 1;
    const Vec3 o = c.mobs.MobOrigin(id);
    if (o.x == 0 && o.y == 0 && o.z == 0) break;   // despawned
    topY = std::max(topY, o.y);
    const float drawnY = c.mobs.MobBodyY(id);
    // ATTRIBUTION, not a bare "climbed 0". A creature that never saw its
    // target, one that saw it and refused the slope, and one that climbed and
    // slid back down all report the same zero, and only the first is not a
    // locomotion bug at all.
    const ai::Brain* br = c.mobs.MobBrain(id);
    if (br != nullptr) {
      intentTicks[(int)br->intent]++;
      if (br->hasTarget) everTargeted = true;
      if (br->path.valid) everPathed = true;
    }
    nearestZ = std::min(nearestZ, (float)targetZ - o.z);
    // Dense over the first ticks and sparse after: the climb IS the first
    // twenty ticks on a rig that walks two columns a tick, and an every-100
    // sample steps straight over the only part of the run under test.
    if ((i < 20 || (i % 100) == 0) && br != nullptr)
      std::printf(
          "    ai-slope t%3d: at (%+6.1f,%+6.1f,%+6.1f) drawn%+7.2f dz %5.1f  "
          "%-9s tgt %d "
          " path %2zu/%2zu  cols %u probed / %u unknown / %u blocked / %u "
          "steep\n",
          i, o.x - (float)spot.x, o.y - startY, o.z - (float)spot.z,
          drawnY - o.y, (float)targetZ - o.z, ai::IntentName(br->intent),
          br->hasTarget ? 1 : 0, br->path.cursor, br->path.pts.size(),
          br->path.colsProbed, br->path.colsUnknown, br->path.colsBlocked,
          br->path.colsSteep);

    // HOW DEEP, not merely "is it inside". A bare "buried" boolean tells the
    // next reader nothing about whether this was a float-epsilon graze on a
    // step-up or a body ten voxels into a hillside, and those are different
    // bugs. Measured at the footprint's own columns, from the SAME mirror the
    // locomotion reads (see the note on cachedSolid above).
    //
    // AND MEASURED TWICE, AT BOTH HEIGHTS THE CREATURE HAS. `MobOrigin` is the
    // COLLIDER; `MobBodyY` is where the body is DRAWN, and every limb transform
    // is built from the second one. The first version of this gate asserted
    // only on the collider, reported a clean 0.00, and was reported from the
    // game as having changed nothing — because it had not: the collision origin
    // rode the slope correctly while the visible creature stayed buried in it,
    // held back by a body-height ease fifteen times too slow to follow the
    // ground. A gate that measures the half of a fix that is easy to measure is
    // worse than no gate, because it certifies the bug.
    const float hx = std::max(0.0f, def.worldSize.x * 0.5f - 0.5f);
    const float hz = std::max(0.0f, def.worldSize.z * 0.5f - 0.5f);
    const float cx = o.x + def.worldSize.x * 0.5f;
    const float cz = o.z + def.worldSize.z * 0.5f;
    float depth = 0, drawnDepth = 0;
    for (int iz = -1; iz <= 1; iz++)
      for (int ix = -1; ix <= 1; ix++) {
        const int wx = ifloor(cx + hx * (float)ix);
        const int wz = ifloor(cz + hz * (float)iz);
        const int s = surfaceAt(wx, wz, ifloor(o.y) + 4);
        if (s == INT32_MIN) continue;
        depth = std::max(depth, (float)s - o.y);
        drawnDepth = std::max(drawnDepth, (float)s - drawnY);
      }
    if (depth > maxDepth) {
      maxDepth = depth;
      deepestTick = i;
      worstAt = o;
    }
    if (drawnDepth > maxDrawnDepth) {
      maxDrawnDepth = drawnDepth;
      drawnDeepestTick = i;
    }
    if (depth > 0.01f) buriedTicks++;
    if (drawnDepth > 0.01f) drawnBuriedTicks++;

    const Vec3 up = c.mobs.MobBodyUp(id);
    const float dot = std::clamp(up.y / std::max(up.len(), 1e-4f), -1.0f, 1.0f);
    maxTiltDeg = std::max(maxTiltDeg, std::acos(dot) * 57.2957795f);
  }

  const float climbed = topY - startY;
  const float minClimb = (float)BaselineNumber("aiSlopeMinClimbVox", 16.0);
  const float maxDepthAllowed = (float)BaselineNumber("aiSlopeMaxDepthVox", 0.5);
  // THE RIG'S OWN CEILING PLUS A SLACK, not an independent number. The claim is
  // "the lean is clamped to what the rig authored" (anim.h
  // LocomotionDef::tiltMaxDeg); pinning a literal here would assert something
  // else, and would go quietly stale the day somebody re-authors the cap.
  // The clamp is on the TOTAL lean (one angle off vertical, not one per axis),
  // so this is tight on purpose: a slack wide enough to absorb a corner would
  // also absorb the 53-degree roll this gate exists to refuse. Measured, the
  // pre-fix code reached 17.7 degrees on this exact ramp.
  const float tiltCap = c.mobs.MobTiltMaxDeg(id);
  const float maxTiltAllowed =
      tiltCap + (float)BaselineNumber("aiSlopeTiltSlackDeg", 1.0);
  const int stepUp = c.mobs.MobStepUpCells(id);
  const int minStepUp = (int)BaselineNumber("aiSlopeMinStepUpCells", 4);

  // TWO ALLOWANCES, because the two heights are making different promises.
  // The COLLIDER must be exactly on the surface — "do not be inside the ground"
  // is not a preference, and it measures 0.00. The DRAWN body may dip a little
  // as a foot plants on lower ground, which is what a stride looks like; what
  // it may not do is follow feet that have gone stale down into a hillside.
  // A tenth of a metre on a 1.7 m figure is the sole grazing, not a creature
  // walking through a hill.
  const float maxDrawnAllowed =
      (float)BaselineNumber("aiSlopeMaxDrawnDepthVox", 1.2);
  const bool ok = rampSeen == rampCols && climbed >= minClimb &&
                  maxDepth <= maxDepthAllowed &&
                  maxDrawnDepth <= maxDrawnAllowed &&
                  maxTiltDeg <= maxTiltAllowed && stepUp >= minStepUp;
  RecordObserved("aiSlopeClimbedVox", (double)climbed);
  RecordObserved("aiSlopeMaxDepthObservedVox", (double)maxDepth);
  RecordObserved("aiSlopeMaxDrawnDepthObservedVox", (double)maxDrawnDepth);
  RecordObserved("aiSlopeMaxTiltObservedDeg", (double)maxTiltDeg);
  detail = Format(
      "ramp %d/%d columns in mirror (%d vox/col over %d, crest +%d), climbed "
      "%.1f vox of %d (>= %.1f), collider deepest below surface %.2f vox "
      "(<= %.2f) at tick %d (%d/%d ticks in the ground), DRAWN body deepest "
      "%.2f vox (<= %.2f) at tick %d (%d/%d ticks in the ground), max body tilt "
      "%.1f deg (<= %.1f), step budget %d cells (>= %d), relief %d, %d ticks "
      "run, targeted %d, pathed %d, closed to dz %.1f, intents "
      "idle/face/appr/hold/circ/atk %d/%d/%d/%d/%d/%d",
      rampSeen, rampCols, kRampRise, kRampLen, kRampLen * kRampRise, climbed,
      kRampLen * kRampRise, minClimb,
      maxDepth, maxDepthAllowed, deepestTick, buriedTicks, budget,
      maxDrawnDepth, maxDrawnAllowed, drawnDeepestTick, drawnBuriedTicks,
      budget, maxTiltDeg,
      maxTiltAllowed, stepUp, minStepUp, relief, ranTicks,
      everTargeted ? 1 : 0, everPathed ? 1 : 0, nearestZ, intentTicks[0],
      intentTicks[1], intentTicks[2], intentTicks[3], intentTicks[4],
      intentTicks[5]);
  if (!ok && maxDepth > maxDepthAllowed)
    detail += Format("; deepest at (%.1f,%.1f,%.1f)", worstAt.x, worstAt.y,
                     worstAt.z);

  // LEAVE THE SUITE EXACTLY AS `ai-approach` DOES. A gate in a shared-World run
  // owns its teardown, and the omission here was not the voxels — it was the
  // PLAYER ACTOR. Leaving one registered hands every later mob gate a phantom
  // enemy standing on this gate's crest, and `corpse-burn` (fifteen gates
  // downstream) duly reported its corpse evaporating instead of burning.
  c.mobs.ClearPlayerActor();
  c.mobs.ClearAttackRequests();
  c.debris.Reset();
  c.mobs.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  return ok ? Status::Pass : Status::Fail;
}

// ---- crawl-slope ------------------------------------------------------------
//
// A CREATURE WITH NO LEGS LIES ON THE GROUND, AND IT DOES NOT TWITCH BETWEEN
// TWO SLOPES WHILE IT DOES IT.
//
// THE FIXTURE IS A RAMP OF TWO GRADES, MIXED. Every second column rises, by one
// voxel or by two on a fixed hash, so the local grade under any short baseline
// is either 1:2 or 1:1 and the average over a body length is neither. That is
// the shape the complaint is about: real ground is a mix of grades, and an
// estimator with no redundancy reports whichever one it happened to land on. A
// smooth wedge could not tell the two estimators apart, because on a plane they
// agree exactly.
//
// Four numbers, because four different things can be wrong:
//
//   floatVox   How far the lowest point the creature actually DRAWS ever got
//              above the ground UNDER THAT POINT, over the ramp's interior. A crawl clip pitches the
//              ROOT, and a root rotation happens about the hip joint, so the
//              torso swings out horizontally AT HIP HEIGHT and stays there — a
//              legless humanoid slid along nine voxels in the air. The old
//              answer was a hand-authored `bodyYOffset` per state per rig,
//              which is that rig's hip height written down by a person, and it
//              was a third of the way there on the human and half on the
//              wizard. Measured against the ramp profile THIS GATE WROTE and
//              at the CONTACT POINT's own column — the engine's ground fit is
//              the thing under test, so a probe that asked the subject where
//              the floor is would prove nothing, and a reference taken under
//              the body's MIDDLE is metres away from its nose on a slope.
//   tiltDeg    The angle the body is drawn at, against atan(the ramp's true
//              mean grade). This is the "crawl along the normal of roughly the
//              slope" claim stated directly: avg within `TiltErr` of the mean
//              grade, and — the part that is actually about jitter — a RANGE
//              (max minus min) under `TiltRange`. A body snapping between the
//              fixture's two component grades would swing about 18 degrees.
//   sdFit      Standard deviation of the fitted forward grade over the run,
//              against `sdFoot` from THE ESTIMATOR THIS REPLACED — UpdateGait's
//              two probes over the footprint span, which is what a crawl would
//              have inherited had the walker's slope lean simply been switched
//              on for it. The body-length two-probe is reported alongside but
//              not asserted on, because it separates the two halves of the fix:
//              most of the win is measuring over the body's own LENGTH at all,
//              and the plane fit takes the rest. EXCURSION, not total
//              variation: fifteen columns
//              crossing a voxel boundary at fifteen different moments make
//              more, smaller steps than two columns do, so per-tick change
//              punishes the smoother signal. What the eye reads as "jumping
//              between two slopes" is how far the estimate swings, which is
//              what a standard deviation is.
//
// Plus two guards that stop any of them being satisfied by doing nothing:
// `crawled` (it has to travel up the ramp) and a floor under `sdTwo` (the
// fixture has to actually present a mixed grade, or the comparison is vacuous).
// Thresholds are in tests/baseline.json, so retuning them costs no rebuild.
Status GateCrawlSlope(Ctx& c, std::string& detail) {
  c.debris.Reset();
  c.mobs.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  const int defIndex = AiHumanoidDef(c.mobs);
  if (defIndex < 0) {
    detail = "no mob def publishes a held_right socket";
    return Status::Fail;
  }
  const MobDef& def = c.mobs.Defs()[defIndex];
  if (c.mobs.Behaviors().Find("duelist") < 0) {
    detail = "no \"duelist\" profile in assets/mobs/behaviors.json";
    return Status::Fail;
  }
  // The rig has to HAVE legs to lose, or this gate is measuring a walker.
  auto partIndex = [&](const char* nm) {
    for (size_t i = 0; i < def.limbs.size(); i++)
      if (def.limbs[i].name == nm) return (int)i;
    return -1;
  };
  const int legL = partIndex("legU.L"), legR = partIndex("legU.R");
  if (legL < 0 || legR < 0) {
    detail = Format("%s has no legU.L/legU.R to sever", def.name.c_str());
    return Status::Fail;
  }

  int relief = 0;
  const IVec3 anchor = AiFixtureCentre(c.world);
  const IVec3 spot = AiFlatSpot(anchor.x, anchor.z, 96, 30, kDefaultSeed, relief);
  const int h0 = World::TerrainHeight(spot.x, spot.z, kDefaultSeed);
  AiTicker tick{c, 7600, {spot.x >> 4, spot.y >> 4, spot.z >> 4}};

  const uint32_t stone = [&] {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == "stone") return (uint32_t)i;
    return 1u;
  }();

  // ---- the mixed ramp ----------------------------------------------------
  // Rises on EVERY SECOND column, by one voxel or by two. Half a voxel per
  // column of run against one per column: 27 degrees against 45, averaging
  // near 37 — steep enough to be a slope, shallow enough that the walk drive
  // will carry a body up it (a 1:1-and-2:1 mix averages 56 degrees, and the
  // drive correctly refuses most of that, so the creature never moved).
  // THE WHOLE FIXTURE HAS TO FIT INSIDE THE DUELIST'S 46-VOXEL SIGHT RANGE,
  // measured PLANAR from the spawn — ramp start + length + the stand-off the
  // target needs past the crest. `ai-slope` documents this at length and this
  // gate relearned it: a 40-column ramp put the target 54 voxels out, the
  // creature perceived nothing, and "crawled 0.0" was a PERCEPTION failure
  // wearing a locomotion failure's clothes. `targeted` below says which.
  const int kHalfX = 22, kRampLen = 28, kCrestLen = 10;
  const int rampZ0 = spot.z + 6;
  auto riseAt = [](int dz) {
    if ((dz & 1) != 0) return 0;         // the flat half of each pair
    // Cheap fixed hash on the PAIR index, so the fixture is identical every
    // run and the pattern is not a period-2 alternation a symmetric two-point
    // probe would average out by construction.
    uint32_t h = (uint32_t)(dz >> 1) * 2654435761u;
    h ^= h >> 13;
    return ((h >> 5) & 1u) ? 2 : 1;
  };
  std::vector<int> top(kRampLen + kCrestLen + 1, h0);
  int totalRise = 0;
  for (int dz = 1; dz < (int)top.size(); dz++) {
    const int r = dz <= kRampLen ? riseAt(dz) : 0;
    totalRise += r;
    top[dz] = top[dz - 1] + r;
  }
  const float meanGrade = (float)totalRise / (float)kRampLen;
  {
    std::vector<CellOp> ops;
    for (int dz = 0; dz < (int)top.size(); dz++) {
      const int wz = rampZ0 + dz;
      for (int wx = spot.x - kHalfX; wx <= spot.x + kHalfX; wx++) {
        const int base = World::TerrainHeight(wx, wz, kDefaultSeed);
        for (int y = base; y <= std::max(top[dz], base); y++) {
          const IVec3 cell{wx, y, wz};
          if (!c.world.CellInWindow(cell)) continue;
          ops.push_back(CellOp{World::SlotCellIndex(cell), stone});
        }
      }
    }
    const size_t kSlice = 4000;
    for (size_t i = 0; i < ops.size(); i += kSlice) {
      std::vector<CellOp> slice(
          ops.begin() + (ptrdiff_t)i,
          ops.begin() + (ptrdiff_t)std::min(i + kSlice, ops.size()));
      tick({}, slice);
    }
  }
  // The surface THIS GATE wrote, from its own table — the independent truth the
  // float is measured against (see the header note). Valid at every column,
  // on the ramp and off it.
  auto fixtureSurface = [&](int wx, int wz) {
    const int dz = wz - rampZ0;
    const int base = World::TerrainHeight(wx, wz, kDefaultSeed);
    const bool onRamp = dz >= 0 && dz < (int)top.size() &&
                        wx >= spot.x - kHalfX && wx <= spot.x + kHalfX;
    return (float)((onRamp ? std::max(top[dz], base) : base) + 1);
  };

  // Get the ramp into the chunk cache the LOCOMOTION reads — the same trap
  // ai-approach and ai-slope both document at length.
  auto cachedSolid = [&](IVec3 cell) {
    const CachedChunk* cc =
        c.world.Cached({cell.x >> 4, cell.y >> 4, cell.z >> 4});
    if (cc == nullptr || cc->voxels.size() != kChunkVol) return false;
    return (cc->voxels[(((uint32_t)cell.z & 15u) * kChunk +
                        ((uint32_t)cell.y & 15u)) *
                           kChunk +
                       ((uint32_t)cell.x & 15u)] &
            0xFFFu) != 0u;
  };
  for (int i = 0; i < 160; i++) {
    bool all = true;
    for (int dz = 0; dz < kRampLen; dz++) {
      const IVec3 cell{spot.x, top[dz], rampZ0 + dz};
      if (cachedSolid(cell)) continue;
      all = false;
      c.world.RequestChunkFetch({cell.x >> 4, cell.y >> 4, cell.z >> 4});
    }
    if (all) break;
    tick();
  }
  int rampSeen = 0;
  for (int dz = 0; dz < kRampLen; dz++)
    if (cachedSolid(IVec3{spot.x, top[dz], rampZ0 + dz})) rampSeen++;

  // ---- the crawler -------------------------------------------------------
  // BLEEDING OFF, for `mob dismember states`' reason: an amputation opens a
  // stump that drains hp until death, and a creature that bleeds out mid-run
  // turns into a ragdoll and stops being a crawl at all. The claim here is
  // where a crawling body is DRAWN, not whether it survives its wounds.
  const Tuning savedGore = CurrentTuning();
  {
    Tuning tt = savedGore;
    tt.gore.bleedHpPerVoxel = 0.0f;
    SetCurrentTuning(tt);
  }
  const int targetZ = rampZ0 + kRampLen + 8;
  const float mobChest = (float)(h0 + 1) + def.worldSize.y * 0.5f;
  const float lipT = (float)(rampZ0 + kRampLen - spot.z) /
                     std::max(1.0f, (float)(targetZ - spot.z));
  const float targetY = std::max(
      (float)top[kRampLen] + 8.0f,
      mobChest + ((float)top[kRampLen] + 3.0f - mobChest) / std::max(lipT, 0.1f));
  c.mobs.SetPlayerActor(Vec3{(float)spot.x, targetY, (float)targetZ}, 3.0f,
                        17.0f, true);

  std::string why;
  auto bail = [&](const std::string& d) {
    detail = d;
    SetCurrentTuning(savedGore);
    c.mobs.ClearPlayerActor();
    c.mobs.ClearAttackRequests();
    c.debris.Reset();
    c.mobs.Reset();
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    return Status::Fail;
  };
  const uint64_t id =
      AiSpawn(c, defIndex,
              {spot.x - (int)(def.worldSize.x * 0.5f), h0 + 1,
               spot.z - (int)(def.worldSize.z * 0.5f)},
              "duelist", why);
  if (id == 0) return bail(why);
  for (int i = 0; i < 8; i++) tick();     // stand up, find the target
  c.mobs.Sever(id, legL);
  c.mobs.Sever(id, legR);
  for (int i = 0; i < 12; i++) tick();    // the crawl clip blends in
  const int stateNow = c.mobs.LocoState(id);
  const float startZ = c.mobs.MobOrigin(id).z;

  const int budget = (int)BaselineNumber("crawlSlopeTicks", 1200.0);
  const float spanFwd = std::max(2.0f, def.worldSize.y);
  const float spanSide =
      std::max(1.0f, std::max(def.worldSize.x, def.worldSize.z));
  const float half = spanFwd * 0.5f;
  float floatMax = 0, floatAny = 0, sinkMax = 0, worstFloatAt = 0;
  float tiltMin = 1e9f, tiltMax = -1e9f;
  double tiltSum = 0, fitSum = 0, fitSq = 0, twoSum = 0, twoSq = 0;
  double footSum = 0, footSq = 0, roughSum = 0;
  // ---- the HEAVE: how still does the body hold while it crawls? ----------
  // REPORTED, NOT ASSERTED, and the reason is the fixture. `heave` is the body
  // height above the fixture's own surface under its centre column, so its
  // spread carries that surface's per-column quantisation (this ramp rises 0,
  // 1 or 2 whole voxels per column) as well as any motion of the body — a
  // threshold on it would be a threshold on the fixture. `pose` is the posed
  // low point's offset from the body frame, which is the stroke as it enters
  // the placement and is clean of terrain. Together they say WHERE a heave
  // came from: a large `pose` is the clip rocking the body, a large `heave`
  // with a small `pose` is the ground read under it stepping.
  double heaveSum = 0, heaveSq = 0, poseSum = 0, poseSq = 0;
  int heaved = 0;
  int onRamp = 0, graded = 0, placed = 0;
  float prevTwo = 0, prevFoot = 0;
  bool everTargeted = false;
  for (int i = 0; i < budget; i++) {
    tick();
    const Vec3 o = c.mobs.MobOrigin(id);
    if (o.x == 0 && o.y == 0 && o.z == 0) break;             // despawned
    Mob* m = c.mobs.FindMobById(id);
    if (m == nullptr) break;
    // ATTRIBUTION, not a bare "crawled 0". A creature that never SAW its
    // target and one that saw it and could not climb report the same zero, and
    // only the second is a locomotion bug (ai-slope's note).
    const ai::Brain* br = c.mobs.MobBrain(id);
    if (br != nullptr && br->hasTarget) everTargeted = true;
    const float cx = o.x + def.worldSize.x * 0.5f;
    const float cz = o.z + def.worldSize.z * 0.5f;
    // Past the foot of the ramp: before that the creature is on the flat and
    // there is no slope claim to make about it.
    if (cz < (float)(rampZ0 + 2)) continue;
    onRamp++;

    // THE RAMP'S INTERIOR, clear of its foot and its crest by half a body.
    // Those are genuine CONVEX AND CONCAVE BREAKS in the grade, and a rigid
    // body as long as the creature is tall cannot touch on both sides of one —
    // it bridges, exactly as a plank does. Asserting a contact there would be
    // asserting that a rigid body is not rigid. Everything outside the window
    // is still measured and reported (`floatAny`), it is simply not a claim.
    const bool interior = cz >= (float)rampZ0 + half &&
                          cz <= (float)(rampZ0 + kRampLen) - half;

    // ---- is it ON the ground? ----
    // At the CONTACT POINT's own column, against the fixture's own profile.
    float lowY = 0;
    Vec3 at{};
    if (m->DrawnLowY(lowY, &at)) {
      const float surf = fixtureSurface(ifloor(at.x), ifloor(at.z));
      placed++;
      floatAny = std::max(floatAny, lowY - surf);
      if (interior) {
        const float heave =
            m->BodyY() - fixtureSurface(ifloor(cx), ifloor(cz));
        const float pose = lowY - m->BodyY();
        heaveSum += heave;
        heaveSq += (double)heave * heave;
        poseSum += pose;
        poseSq += (double)pose * pose;
        heaved++;
        if (lowY - surf > floatMax) {
          floatMax = lowY - surf;
          worstFloatAt = cz - (float)rampZ0;
        }
        sinkMax = std::max(sinkMax, surf - lowY);
      }
    }

    // ---- is it lying at the RIGHT angle, and does that angle hold still? ----
    if (!interior) continue;
    const Vec3 up = c.mobs.MobBodyUp(id);
    const float tilt =
        std::acos(std::clamp(up.y, -1.0f, 1.0f)) * 57.29577951f;
    tiltSum += tilt;
    tiltMin = std::min(tiltMin, tilt);
    tiltMax = std::max(tiltMax, tilt);

    // ---- three estimators, same tick, same heading -----------------------
    // `foot` IS THE ESTIMATOR THIS REPLACED, not a strawman: UpdateGait's
    // slope lean is two probes over `max(1.5, max(worldSize.x, z) * 0.5)`,
    // which is the right instrument for a walker standing on two small feet
    // and is what a crawl would have inherited. `two` is the same two-probe
    // shape over the PRONE baseline, and it is reported rather than asserted
    // on because it separates the two halves of the fix: how much of the win
    // is measuring over the body's length at all, and how much is fitting a
    // plane instead of differencing two columns.
    const Mob::GroundPlane gp =
        m->FitGroundPlane(c.world, cx, cz, o.y, spanFwd, spanSide, 5, 3);
    const Vec3 face = c.mobs.MobFacing(id);
    const int yFrom = ifloor(o.y) + 3;
    auto twoProbe = [&](float reach, float fallback) {
      int hi = 0, lo = 0;
      const bool okHi = m->GroundHeightAt(c.world, ifloor(cx + face.x * reach),
                                          ifloor(cz + face.z * reach), yFrom, hi);
      const bool okLo = m->GroundHeightAt(c.world, ifloor(cx - face.x * reach),
                                          ifloor(cz - face.z * reach), yFrom, lo);
      return (okHi && okLo) ? (float)(hi - lo) / (2.0f * reach) : fallback;
    };
    const float footSpan =
        std::max(1.5f, std::max(def.worldSize.x, def.worldSize.z) * 0.5f);
    const float two = twoProbe(half, prevTwo);
    const float foot = twoProbe(footSpan, prevFoot);
    prevTwo = two;
    prevFoot = foot;
    if (!gp.valid) continue;
    fitSum += gp.gradeFwd;
    fitSq += (double)gp.gradeFwd * gp.gradeFwd;
    twoSum += two;
    twoSq += (double)two * two;
    footSum += foot;
    footSq += (double)foot * foot;
    roughSum += gp.roughness;
    graded++;
  }
  const float crawled = c.mobs.MobOrigin(id).z - startZ;
  auto sd = [](double sum, double sq, int n) {
    if (n < 2) return 0.0f;
    const double mean = sum / n;
    return (float)std::sqrt(std::max(0.0, sq / n - mean * mean));
  };
  const float sdHeave = sd(heaveSum, heaveSq, heaved);
  const float sdPose = sd(poseSum, poseSq, heaved);
  const float sdFit = sd(fitSum, fitSq, graded);
  const float sdTwo = sd(twoSum, twoSq, graded);
  const float sdFoot = sd(footSum, footSq, graded);
  const float rough = graded > 0 ? (float)(roughSum / graded) : 0.0f;
  const float tiltAvg = graded > 0 ? (float)(tiltSum / graded) : 0.0f;
  const float tiltRange = graded > 0 ? tiltMax - tiltMin : 0.0f;
  const float wantTilt = std::atan(meanGrade) * 57.29577951f;

  const float floatAllowed = (float)BaselineNumber("crawlSlopeFloatVox", 2.5);
  const float sinkAllowed = (float)BaselineNumber("crawlSlopeSinkVox", 2.5);
  const float tiltErr = (float)BaselineNumber("crawlSlopeTiltErrDeg", 8.0);
  const float tiltSpan = (float)BaselineNumber("crawlSlopeTiltRangeDeg", 12.0);
  const float sdRatio = (float)BaselineNumber("crawlSlopeSdRatio", 0.7);
  const float minCrawl = (float)BaselineNumber("crawlSlopeMinCrawlVox", 10.0);
  const int minSamples = (int)BaselineNumber("crawlSlopeMinSamples", 25.0);

  const bool moved = crawled >= minCrawl && graded >= minSamples && placed > 0;
  const bool grounded = floatMax <= floatAllowed && sinkMax <= sinkAllowed;
  const bool lying =
      std::fabs(tiltAvg - wantTilt) <= tiltErr && tiltRange <= tiltSpan;
  // `sdFoot` near zero would mean the fixture never presented a mixed grade to
  // the estimator under comparison, so that is a failure OF THE FIXTURE and is
  // reported as one rather than passing trivially.
  const bool steady = sdFoot > 0.03f && sdFit <= sdFoot * sdRatio;
  const bool ok = moved && grounded && lying && steady && stateNow >= 0;

  detail = Format(
      "ramp %d/%d cols in mirror, rises 0/1/2 per col, true mean grade %.3f "
      "(%.1f deg); state %d, targeted %d, crawled %.1f vox (min %.1f) over %d ramp ticks, "
      "%d graded; float %.2f vox (max %.2f) at dz %.0f, sink %.2f (max %.2f), "
      "float anywhere incl. crest %.2f; "
      "tilt avg %.1f vs %.1f deg (err max %.1f), range %.1f (max %.1f); grade "
      "sd fit %.4f vs footprint-probe %.4f = %.2fx (max %.2f), body-length "
      "two-probe %.4f; fit residual %.2f vox; body-height sd %.2f vox about "
      "its own column, posed-clearance sd %.2f",
      rampSeen, kRampLen, meanGrade, wantTilt, stateNow, everTargeted ? 1 : 0,
      crawled, minCrawl,
      onRamp, graded, floatMax, floatAllowed, worstFloatAt, sinkMax,
      sinkAllowed, floatAny, tiltAvg, wantTilt, tiltErr, tiltRange, tiltSpan, sdFit,
      sdFoot, sdFoot > 0 ? sdFit / sdFoot : 0.0f, sdRatio, sdTwo, rough,
      sdHeave, sdPose);

  SetCurrentTuning(savedGore);
  c.mobs.ClearPlayerActor();
  c.mobs.ClearAttackRequests();
  c.debris.Reset();
  c.mobs.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  return ok ? Status::Pass : Status::Fail;
}

// ---- LIVE RAGDOLL (DESIGN.md "A creature knocked down gets back up") --------
//
// Six claims, each a number this gate prints, so `--gate ragdoll` alone is
// the whole iteration loop:
//   A. a blast beside a standing creature knocks it LIMP (phase 1) and moves
//      its pelvis at least ragdollBlastMinTravel voxels — and no more than
//      ragdollBlastMaxTravel ("across the room, not across the map");
//   B. it gets back UP: phase returns to 0 within ragdollGetUpMaxTicks, still
//      alive, every limb still its own (LimbBodyCount unchanged, no body
//      adopted by DebrisSystem), standing on the ground where it landed and
//      still standing 60 ticks later;
//   C. dropped from a height it FALLS under gravity (origin_.y descends, the
//      old code hung it in the air), goes limp once the fall has lasted
//      ragdoll.fallSeconds — shortened for the fixture, the harness mirror is
//      only 2.4 m deep — and gets back up on the ground.
//   D. through main.cpp's WHOLE explosion block (crater, debris damage, the
//      carve, the per-body debris impulse, then the rig launch) a standing
//      wizard and then the SAME wizard already limp never have a limb faster
//      than ragdollRepeatBlastMaxSpeed. The per-body impulse used to reach a
//      limp rig's limbs one at a time — impulse / 0.3 kg on a hand — and the
//      launch stacked on top of it: "bodies zoom across the map".
//   E. the PLAYER on fire, wearing cloth, with the capsule proxy in the world:
//      what burns off the body (gobbets, shed cloth) may not shove the player
//      through PlayerPushOut by more than ragdollBurnMaxPushVox in a tick, nor
//      move them more than ragdollBurnMaxTravel over the burn. Measured before
//      the fix: 62 voxels in one tick, 432 voxels before death.
//   F. the TUMBLE: the same charge at the same distance, once at the ankles
//      and once over the head, spins the rig in OPPOSITE directions (|w.z| at
//      least ragdollBlastMinSpin either way) and tips it opposite ways 12
//      ticks later (the up axis leans by at least ragdollBlastMinTiltGap
//      between the two arms). The differential stays modest — no limb leaves
//      the blast more than ragdollBlastMaxLimbSpeedRatio times faster than
//      the slowest, and the spin is under ragdoll.blastMaxSpin. Before this,
//      every limb took the same velocity and a body floated away from an
//      explosion still standing to attention.
// Thresholds are in tests/baseline.json (BaselineNumber), so retuning what
// counts as "across the room" is a JSON edit, not a rebuild.
Status GateRagdoll(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  DebrisSystem& debris = c.debris;
  MobSystem& mobs = c.mobs;
  Physics& phys = c.phys;
  debris.Reset();
  mobs.Reset();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  int dummyDef = -1, wizDef = -1;
  for (size_t i = 0; i < mobs.Defs().size(); i++) {
    if (mobs.Defs()[i].name == "dummy") dummyDef = (int)i;
    if (mobs.Defs()[i].name == kAvatarDefName) wizDef = (int)i;
  }
  if (dummyDef < 0 || wizDef < 0) {
    detail = "no dummy / avatar mob def";
    return Status::Fail;
  }
  const int nLimbs = (int)mobs.Defs()[dummyDef].limbs.size();
  // The AI gates' fixture: a flat spot at the CENTRE OF THE RESIDENCY
  // WINDOW, never an absolute coordinate — an earlier gate leaves the window
  // elsewhere and a mob spawned outside it is despawned on its first tick
  // (AiFixtureCentre's note; this gate relearned it in-suite).
  int relief = 0;
  const IVec3 anchor = AiFixtureCentre(world);
  const IVec3 spot = AiFlatSpot(anchor.x, anchor.z, 96, 16, kDefaultSeed, relief);
  const int h = spot.y;
  AiTicker ticker{c, 9000, {spot.x >> 4, spot.y >> 4, spot.z >> 4}};
  auto tickOnce = [&]() { ticker(); };
  const double minTravel = BaselineNumber("ragdollBlastMinTravel", 6.0);
  const double maxTravel = BaselineNumber("ragdollBlastMaxTravel", 120.0);
  const int getUpMaxTicks = (int)BaselineNumber("ragdollGetUpMaxTicks", 400.0);
  bool ok = true;

  // ---- A. the blast --------------------------------------------------------
  uint64_t id = mobs.Spawn(dummyDef, {spot.x, h + 1, spot.z});
  if (id == 0) {
    detail = "spawn failed";
    return Status::Fail;
  }
  std::printf("  ragdoll fixture: spot (%d, %d, %d), relief %d\n", spot.x, h,
              spot.z, relief);
  for (int i = 0; i < 45; i++) tickOnce();  // stand, settle the feet
  const uint32_t debrisBefore = debris.BodyCount();
  const Vec3 p0 = mobs.MobRootPos(id);
  const int phaseBefore = mobs.RagdollPhaseOf(id);
  // The X-detonate charge (tools.detonateRadius/Power) 6 voxels to the side,
  // at pelvis height, through the same scale main.cpp's explosion loop uses.
  const auto& tune = CurrentTuning();
  const Vec3 ec{p0.x - 6.0f, p0.y + 2.0f, p0.z};
  const float reach = (float)tune.tools.detonateRadius * tune.ragdoll.blastRadiusScale;
  const float impulse = (float)tune.tools.detonatePower * tune.ragdoll.blastImpulseScale;
  const int knocked = mobs.BlastMobsRadial(ec, reach, impulse);
  const int phaseAfter = mobs.RagdollPhaseOf(id);
  for (int i = 0; i < 45; i++) tickOnce();  // 1.5 s of flight
  const Vec3 p1 = mobs.MobRootPos(id);
  const float travel = Vec3{p1.x - p0.x, 0, p1.z - p0.z}.len();
  const bool blastOk = knocked == 1 && phaseBefore == 0 && phaseAfter == 1 &&
                       mobs.IsAlive(id) && travel >= (float)minTravel &&
                       travel <= (float)maxTravel;
  std::printf("  ragdoll blast: %s (knocked %d, phase %d->%d, pelvis travelled "
              "%.1f vox in 1.5 s, band %.0f..%.0f, alive %d)\n",
              blastOk ? "PASS" : "FAIL", knocked, phaseBefore, phaseAfter, travel,
              minTravel, maxTravel, (int)mobs.IsAlive(id));
  ok = ok && blastOk;

  // ---- B. the get-up -------------------------------------------------------
  int upAt = -1;
  for (int i = 0; i < getUpMaxTicks; i++) {
    tickOnce();
    if (mobs.RagdollPhaseOf(id) == 0) {
      upAt = i + 1;
      break;
    }
  }
  const Vec3 up = mobs.MobOrigin(id);
  const int gh = World::TerrainHeight(ifloor(up.x + 1.5f), ifloor(up.z + 1.5f),
                                      kDefaultSeed);
  for (int i = 0; i < 60; i++) tickOnce();  // ...and stays up, walking
  const Vec3 later = mobs.MobOrigin(id);
  const bool upOk = upAt > 0 && mobs.IsAlive(id) &&
                    mobs.LimbBodyCount() == (uint32_t)nLimbs &&
                    debris.BodyCount() == debrisBefore &&
                    std::fabs(up.y - (float)(gh + 1)) < 16.0f &&
                    mobs.RagdollPhaseOf(id) == 0 &&
                    std::fabs(later.y - (float)(gh + 1)) < 16.0f;
  std::printf("  ragdoll get-up: %s (up after %d ticks, alive %d, limbs %u/%d, "
              "debris +%d, stood at y %.1f vs ground %d, 60 ticks later y %.1f "
              "phase %d)\n",
              upOk ? "PASS" : "FAIL", upAt, (int)mobs.IsAlive(id),
              mobs.LimbBodyCount(), nLimbs, (int)(debris.BodyCount() - debrisBefore),
              up.y, gh + 1, later.y, mobs.RagdollPhaseOf(id));
  ok = ok && upOk;

  // ---- C. the fall ---------------------------------------------------------
  // 2.2 m up, inside the harness mirror's 2.4 m ground scan, and a
  // fixture-short fallSeconds so the limp fires before the landing.
  mobs.Reset();
  debris.Reset();
  const Tuning saved = CurrentTuning();
  {
    Tuning tt = saved;
    tt.ragdoll.fallSeconds = 0.3f;
    SetCurrentTuning(tt);
  }
  uint64_t fid =
      mobs.Spawn(dummyDef, {spot.x, h + 1 + MetresToCellsI(2.2f), spot.z});
  const Vec3 f0 = mobs.MobOrigin(fid);
  for (int i = 0; i < 5; i++) tickOnce();
  const Vec3 f1 = mobs.MobOrigin(fid);
  int limpAt = -1;
  for (int i = 0; i < 30; i++) {
    tickOnce();
    if (mobs.RagdollPhaseOf(fid) == 1) {
      limpAt = i + 6;
      break;
    }
  }
  int fUpAt = -1;
  for (int i = 0; i < getUpMaxTicks; i++) {
    tickOnce();
    if (mobs.RagdollPhaseOf(fid) == 0) {
      fUpAt = i + 1;
      break;
    }
  }
  const Vec3 f2 = mobs.MobOrigin(fid);
  const bool fallOk = fid != 0 && f1.y < f0.y - 0.5f && limpAt > 0 &&
                      fUpAt > 0 && mobs.IsAlive(fid) &&
                      std::fabs(f2.y - (float)(h + 1)) < 16.0f;
  std::printf("  ragdoll fall: %s (spawned %.0f up, dropped %.2f vox in 5 ticks, "
              "limp at tick %d, up after %d more, final y %.1f vs ground %d, "
              "alive %d)\n",
              fallOk ? "PASS" : "FAIL", f0.y - (float)(h + 1), f0.y - f1.y,
              limpAt, fUpAt, f2.y, h + 1, (int)mobs.IsAlive(fid));
  ok = ok && fallOk;
  SetCurrentTuning(saved);

  // ---- D. the second blast ---------------------------------------------------
  // main.cpp's explosion block, verbatim in shape: destruction event, debris
  // damage, the mob carve, the per-body debris impulse WITH the rig skip
  // list, then the rig launch. A gate that calls BlastMobsRadial alone (A
  // above) never saw the per-body impulse reach a limp rig.
  {
    mobs.Reset();
    debris.Reset();
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    uint32_t tick = ticker.tick;
    const IVec3 pchunk{spot.x >> 4, spot.y >> 4, spot.z >> 4};
    auto explodeTick = [&](const ExplosionOp* e) {
      std::vector<BrushOp> ops;
      std::vector<CellOp> cellOps;
      std::vector<ParticleSpawn> spawns;
      std::vector<ExplosionOp> exps;
      mobs.PreTick(tick + 1, world, ops, cellOps, spawns);
      debris.QueueSupportEvents(world.Snap());
      debris.PreTick(tick + 1, world, cellOps, spawns);
      if (e) {
        exps.push_back(*e);
        debris.AddDestructionEvent(tick + 1,
                                   {e->x - e->radius, e->y - e->radius, e->z - e->radius},
                                   {e->x + e->radius, e->y + e->radius, e->z + e->radius});
        const Vec3 ec{(float)e->x + 0.5f, (float)e->y + 0.5f, (float)e->z + 0.5f};
        const float edr = (float)e->radius * tune.physics.explosionBodyDamageScale;
        debris.DamageBodiesRadial(ec, edr, world, spawns);
        mobs.CarveMobsRadial(ec, edr, world, spawns);
        std::vector<uint64_t> rig;
        mobs.AppendLiveLimbBodies(rig);
        std::sort(rig.begin(), rig.end());
        phys.ApplyRadialImpulse(Vec3{(float)e->x, (float)e->y, (float)e->z},
                                (float)e->radius * tune.physics.explosionImpulseRadiusScale,
                                (float)e->power * tune.physics.explosionImpulseScale, &rig);
        mobs.BlastMobsRadial(ec, (float)e->radius * tune.ragdoll.blastRadiusScale,
                             (float)e->power * tune.ragdoll.blastImpulseScale);
      }
      ++tick;
      SubmitTick(ctx, world, sim, tick, kDefaultSeed, ops, exps, cellOps, false,
                 pchunk, true, true, spawns);
      ctx.WaitIdle();
      ctx.ProcessEvents();
      phys.Step(kTickDt);
      debris.PostStep();
      mobs.PostStep();
    };
    const int nWiz = (int)mobs.Defs()[wizDef].limbs.size();
    // The fastest limb of the rig this tick, m/s. Every limb, not the pelvis:
    // the per-body impulse hit the LIGHT limbs hardest.
    auto fastestLimb = [&](uint64_t id) {
      float best = 0.0f;
      for (int l = 0; l < nWiz; l++) {
        const uint64_t hb = mobs.LimbBody(id, l);
        Vec3 lin{}, ang{};
        if (hb && phys.GetBodyVelocities(hb, lin, ang))
          best = std::max(best, lin.len() * kVoxelMeters);
      }
      return best;
    };
    // The LAST POSITION THE CREATURE WAS ALIVE AT, because a dead mob's
    // position probe is not a position: the husk leaves mobs_ on the next
    // PreTick sweep and MobRootPos then answers from nothing, which is how
    // "the pelvis moved 583 voxels in 1.3 s" (56 m/s, twice the speed cap
    // that exists to prevent exactly that) got reported for a body that had
    // in fact stopped. Travel measured to here is a real distance either way.
    Vec3 lastAlive{};
    int diedAt = -1;
    auto flight = [&](uint64_t id, int ticks, const ExplosionOp& e) {
      float peak = 0.0f;
      for (int i = 0; i < ticks; i++) {
        explodeTick(i == 0 ? &e : nullptr);
        peak = std::max(peak, fastestLimb(id));
        if (mobs.IsAlive(id)) lastAlive = mobs.MobRootPos(id);
        else if (diedAt < 0) diedAt = i;
      }
      return peak;
    };
    const double maxSpeed = BaselineNumber("ragdollRepeatBlastMaxSpeed", 21.0);
    const uint64_t wid = mobs.Spawn(wizDef, {spot.x, h + 1, spot.z});
    for (int i = 0; i < 45; i++) explodeTick(nullptr);
    // HOW MUCH WIZARD IS LEFT BEFORE THE FIRST CHARGE, because this arm's
    // subject does not always arrive intact and that was invisible: it
    // settles for 45 ticks in whatever world the gates before it left, and
    // reaches the blast at 15 of 15 limbs under `--gate ragdoll` but only 12
    // of 15 inside a full `--selftest` (44.9 kg against 52.4). A charge 6
    // voxels away is close to lethal for the smaller one, which makes "it
    // survived to take a second blast" a knife-edge claim rather than a
    // property of the launch — read this number first when the arm moves.
    const uint32_t limbsAtBlast = mobs.LimbBodyCount();
    const Vec3 w0 = mobs.MobRootPos(wid);
    const ExplosionOp e1{ifloor(w0.x) - 6, ifloor(w0.y) + 2, ifloor(w0.z),
                         tune.tools.detonateRadius, tune.tools.detonatePower, 0, 0, 0};
    const float peak1 = flight(wid, 20, e1);
    const int phase1 = mobs.RagdollPhaseOf(wid);
    const Vec3 w1 = mobs.MobRootPos(wid);
    const ExplosionOp e2{ifloor(w1.x) - 6, ifloor(w1.y) + 2, ifloor(w1.z),
                         tune.tools.detonateRadius, tune.tools.detonatePower, 0, 0, 0};
    const float peak2 = flight(wid, 40, e2);
    const float travel2 = (lastAlive - w1).len();
    const bool repeatOk = wid != 0 && phase1 == 1 && mobs.IsAlive(wid) &&
                          peak1 <= (float)maxSpeed && peak2 <= (float)maxSpeed &&
                          travel2 <= (float)maxTravel;
    std::printf("  ragdoll repeat blast: %s (fastest limb %.1f m/s standing, %.1f m/s "
                "already limp, ceiling %.0f; second blast moved the pelvis %.1f vox "
                "in 1.3 s, band ..%.0f; phase after first %d, alive %d, died at flight "
                "tick %d of \"%s\"; subject had %u of %d limbs before the first "
                "charge)\n",
                repeatOk ? "PASS" : "FAIL", peak1, peak2, maxSpeed, travel2, maxTravel,
                phase1, (int)mobs.IsAlive(wid), diedAt, mobs.DeathCause(wid),
                limbsAtBlast, nWiz);
    ok = ok && repeatOk;
    ticker.tick = tick;
  }

  // ---- E. the player on fire -------------------------------------------------
  // The avatar with the capsule proxy in the world (PlayerPushOut needs one),
  // wearing the stock cloth, every part alight. What comes off — burn
  // gobbets, a robe burnt through — is born beside or on top of the capsule,
  // gets swept into it by the avatar's own kinematic limbs, and used to be
  // read by PlayerPushOut as one hit per voxel box, summed.
  {
    mobs.Reset();
    debris.Reset();
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    uint32_t tick = ticker.tick;
    const IVec3 pchunk{spot.x >> 4, spot.y >> 4, spot.z >> 4};
    PlayerAvatar avatar;
    avatar.Init(&phys, &world, &debris, c.mats, &mobs);
    avatar.SetDefs(&mobs.Defs(), kAvatarDefName);
    Player pl;
    pl.fly = false;
    pl.grounded = true;
    pl.pos = Vec3{(float)spot.x + 0.5f, (float)(h + 2) + Player::kHalfY, (float)spot.z + 0.5f};
    const bool spawned = avatar.Spawn(pl, 0.0f);
    const uint64_t proxy = phys.CreatePlayerBody(Player::kHalfXZ, Player::kHalfY);
    const char* wear[3] = {"hood", "robe", "boots"};
    const int slots[3] = {0, 1, 3};  // EquipSlotId Head, Chest, Boots
    int worn = 0;
    for (int k = 0; k < 3; k++) {
      const ItemDef* d = c.items.At(c.items.Find(wear[k]));
      if (d && avatar.WearItem(d, slots[k])) worn++;
    }
    auto avTick = [&]() {
      std::vector<BrushOp> ops;
      std::vector<ParticleSpawn> spawns;
      std::vector<CellOp> cellOps;
      phys.MovePlayerBody(proxy, pl.pos, kTickDt);
      avatar.PreTick(tick + 1, pl, 0.0f, kTickDt, world, ops, cellOps, spawns);
      debris.QueueSupportEvents(world.Snap());
      debris.PreTick(tick + 1, world, cellOps, spawns);
      ++tick;
      SubmitTick(ctx, world, sim, tick, kDefaultSeed, ops, {}, cellOps, false,
                 pchunk, true, true, spawns);
      ctx.WaitIdle();
      ctx.ProcessEvents();
      phys.Step(kTickDt);
      debris.PostStep();
      avatar.PostStep();
    };
    for (int i = 0; i < 30; i++) avTick();
    for (int l = 0; l < avatar.PartCount(); l++) avatar.IgnitePart(l, 200);
    const double maxPush = BaselineNumber("ragdollBurnMaxPushVox", 0.5);
    const double maxMoved = BaselineNumber("ragdollBurnMaxTravel", 6.0);
    const Vec3 start = pl.pos;
    float peakPush = 0.0f;
    int pushTicks = 0;
    uint32_t gobbets = 0;
    // WHAT pushed hardest, not just how hard: mass, depth, the tick it
    // happened on, and whether that body is one of the avatar's OWN limbs
    // (a corpse's arm handed to the world inside the capsule is a different
    // bug from a gobbet swept into it, and a bare "6.25 vox" cannot tell
    // them apart — CLAUDE.md rule 6).
    Physics::PushSource worst{};
    int worstTick = -1, worstAlive = -1, worstOwn = -1, worstLayer = -1;
    uint32_t worstBodies = 0;
    size_t worstPending = 0;
    // The handles the avatar's parts hold while ALIVE. Die() hands each one to
    // DebrisSystem and then zeroes `limb.body`, so asking the avatar "is this
    // yours" AFTER the death tick always answers no — the probe has to
    // remember. (A fixture that measures itself: the first version of this
    // line read PartBody() at the time of the push and could not have
    // reported 1 whatever happened.)
    std::vector<uint64_t> ownBodies;
    for (int l = 0; l < avatar.PartCount(); l++)
      if (avatar.PartBody(l)) ownBodies.push_back(avatar.PartBody(l));
    // THE CLAIM IS ABOUT A LIVING PLAYER, so the sample window ends at death.
    // The loop tests IsAlive() before the tick, so the last push it collected
    // was always taken AFTER the avatar died in that very tick — a corpse's
    // own limbs going to the world under the capsule, which is a different
    // event from "what burns off me shoves me" and has its own failure mode
    // (see the post-mortem line below; it is reported, never asserted on).
    float deathPush = 0.0f;
    Physics::PushSource deathSrc{};
    Vec3 livingPos = pl.pos;
    for (int i = 0; i < 400 && avatar.IsAlive(); i++) {
      avTick();
      Physics::PushSource src{};
      Vec3 push = phys.PlayerPushOut(proxy, pl.pos, &src);
      // Player::ApplyPush's clamp, without its terrain sweeps.
      const float len = push.len();
      const float kMaxPush = 2.0f * Player::kHalfXZ;
      if (len > kMaxPush) push = push * (kMaxPush / len);
      Vec3 follow;
      if (avatar.RagdollFollow(follow)) pl.pos = follow;
      else pl.pos += push;
      gobbets = std::max(gobbets, debris.BodyCount());
      if (!avatar.IsAlive()) {  // died during this tick: post-mortem, not ours
        deathPush = len;
        deathSrc = src;
        break;
      }
      livingPos = pl.pos;
      if (len > peakPush) {
        peakPush = len;
        worst = src;
        worstTick = i;
        worstAlive = 1;
        worstBodies = debris.BodyCount();
        worstLayer = phys.BodyObjectLayer(src.body);
        worstPending = phys.PendingReleaseCount();
        worstOwn = 0;
        for (uint64_t b : ownBodies)
          if (b == src.body) worstOwn = 1;
      }
      if (len > 0.05f) pushTicks++;
    }
    const float moved = (livingPos - start).len();
    const bool burnOk = spawned && worn == 3 && peakPush <= (float)maxPush &&
                        moved <= (float)maxMoved;
    std::printf("  ragdoll player on fire: %s (peak push %.2f vox/tick, ceiling %.2f; "
                "%d ticks pushed; moved %.1f vox, ceiling %.0f; up to %u pieces off; "
                "worn %d/3, alive %d)\n",
                burnOk ? "PASS" : "FAIL", peakPush, maxPush, pushTicks, moved, maxMoved,
                gobbets, worn, (int)avatar.IsAlive());
    std::printf("    worst pusher: body %llu, %.2f kg, %.2f vox deep, layer %d "
                "(1=MOVING, 3=AVATAR), at burn tick %d (avatar alive %d, was its "
                "own limb %d, %u debris bodies, %zu still pending release)\n",
                (unsigned long long)worst.body, worst.massKg, worst.depthVox,
                worstLayer, worstTick, worstAlive, worstOwn, worstBodies,
                worstPending);
    // OPEN FINDING, reported every run and asserted on by nothing (2026-09-11).
    // On the tick the avatar dies, Die() hands its limbs to DebrisSystem and
    // queues them through ReleaseToWorldWhenClear — and at some poses one of
    // them is on Layers::MOVING, deep inside the capsule, on that same tick.
    // Caught here at a blast-launch retune that shifted this arm's tick phase:
    // a 12.83 kg limb 3.25 voxels in, one shove of 6.25 voxels, while five
    // other limbs were still correctly pending. It is phase-dependent (three
    // neighbouring phases push 0.00), it is NOT the burn, and it is above the
    // 5%-of-player-mass filter by design — a corpse's torso is meant to be
    // able to shove you, just not by a third of a metre in one tick.
    int deathOwn = 0;
    for (uint64_t b : ownBodies)
      if (b == deathSrc.body) deathOwn = 1;
    std::printf("    post-mortem (not asserted): death-tick push %.2f vox from "
                "body %llu, %.2f kg, %.2f vox deep, layer %d, own limb %d\n",
                deathPush, (unsigned long long)deathSrc.body, deathSrc.massKg,
                deathSrc.depthVox, phys.BodyObjectLayer(deathSrc.body), deathOwn);
    ok = ok && burnOk;
    avatar.Despawn();
    phys.RemoveBody(proxy);
    ticker.tick = tick;
  }

  // ---- F. the tumble ---------------------------------------------------------
  // BOTH ARMS INSIDE ONE GATE, and the only thing that differs between them is
  // the HEIGHT of the charge: same rig, same spot, same reach, same distance
  // from the pelvis (the charge is placed symmetrically about the point
  // BlastRadial measures the knockdown at, so the launch speed is the same and
  // only the geometry moves). A blast at the ankles must roll the body one way
  // and a blast over the head the other; a comparison against a number from a
  // different fixture could not tell "the spin works" from "this rig happens
  // to fall over".
  {
    const double minSpin = BaselineNumber("ragdollBlastMinSpin", 0.4);
    const double maxRatio = BaselineNumber("ragdollBlastMaxLimbSpeedRatio", 3.0);
    const double minTiltGap = BaselineNumber("ragdollBlastMinTiltGap", 0.25);
    const auto& rgt = CurrentTuning().ragdoll;
    const int nWiz = (int)mobs.Defs()[wizDef].limbs.size();
    const int rootLimb = mobs.Defs()[wizDef].rootLimb;
    float spinZ[2] = {0, 0}, spinLen[2] = {0, 0}, ratio[2] = {0, 0}, upX[2] = {0, 0};
    int knockedF[2] = {0, 0};
    for (int arm = 0; arm < 2; arm++) {
      mobs.Reset();
      debris.Reset();
      SubmitWorldgen(ctx, world, sim, kDefaultSeed);
      ctx.WaitIdle();
      const uint64_t wid = mobs.Spawn(wizDef, {spot.x, h + 1, spot.z});
      if (wid == 0) break;
      for (int i = 0; i < 45; i++) tickOnce();  // stand, settle the feet
      // BlastRadial measures the knockdown 0.4 m above the pelvis body's
      // origin; put the charge 8 voxels below that for the ankle arm and 8
      // above it for the overhead one, 6 to the side either way.
      const Vec3 w0 = mobs.MobRootPos(wid);
      const float probeY = w0.y + 0.4f / kVoxelMeters;
      const Vec3 ec2{w0.x - 6.0f, probeY + (arm == 0 ? -8.0f : 8.0f), w0.z};
      knockedF[arm] = mobs.BlastMobsRadial(ec2, reach, impulse);
      // Read the rig's motion the instant the launch is set: every limb was
      // given the same spin (one rigid motion), and the linear speeds differ
      // only by omega x r.
      float fast = 0.0f, slow = 1e9f;
      for (int l = 0; l < nWiz; l++) {
        const uint64_t hb = mobs.LimbBody(wid, l);
        Vec3 lin{}, ang{};
        if (!hb || !phys.GetBodyVelocities(hb, lin, ang)) continue;
        fast = std::max(fast, lin.len());
        slow = std::min(slow, lin.len());
        if (l == rootLimb) {
          spinZ[arm] = ang.z;
          spinLen[arm] = ang.len();
        }
      }
      ratio[arm] = slow > 1e-3f ? fast / slow : 0.0f;
      // ...and the EFFECT, a third of a second later: a spin Jolt cancelled
      // against the joints on the first step would set the numbers above and
      // change nothing about the body. Rotation about +z tips the pelvis's up
      // axis toward -x, so the two arms must straddle.
      for (int i = 0; i < 12; i++) tickOnce();
      BodyTransform xf{};
      if (rootLimb >= 0 && phys.GetTransform(mobs.LimbBody(wid, rootLimb), xf)) {
        const Quat q{xf.quat[0], xf.quat[1], xf.quat[2], xf.quat[3]};
        upX[arm] = QuatRotate(q, Vec3{0, 1, 0}).x;
      }
    }
    const bool spinOk =
        knockedF[0] == 1 && knockedF[1] == 1 && spinZ[0] >= (float)minSpin &&
        spinZ[1] <= -(float)minSpin &&
        spinLen[0] <= rgt.blastMaxSpin + 1e-3f &&
        spinLen[1] <= rgt.blastMaxSpin + 1e-3f && ratio[0] <= (float)maxRatio &&
        ratio[1] <= (float)maxRatio && (upX[1] - upX[0]) >= (float)minTiltGap;
    std::printf("  ragdoll blast spin: %s (ankle charge %.2f rad/s about z, "
                "overhead %.2f, floor %.2f, cap %.1f; limb speed spread %.2fx / "
                "%.2fx, ceiling %.1fx; pelvis up.x %.2f vs %.2f, gap %.2f needs "
                "%.2f; knocked %d/%d)\n",
                spinOk ? "PASS" : "FAIL", spinZ[0], spinZ[1], minSpin,
                rgt.blastMaxSpin, ratio[0], ratio[1], maxRatio, upX[0], upX[1],
                upX[1] - upX[0], minTiltGap, knockedF[0], knockedF[1]);
    ok = ok && spinOk;
  }

  mobs.Reset();
  debris.Reset();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  if (!ok) detail = "see the ragdoll lines above";
  return ok ? Status::Pass : Status::Fail;
}

// ---- ragdoll-dress ---------------------------------------------------------
//
// A DRESSED BODY GOING LIMP STAYS ONE OBJECT.
//
// Owner report, 2026-09-13: "when the human mob is falling, and is wearing
// clothes, when they go into the ragdoll state the clothes will decouple from
// the body when moving at high speeds... all of the clothes separate from limbs
// and it just becomes a crazy tangled mess ball of limbs and clothes that
// aren't actually connected properly. Seems like when falling the limbs will be
// moving at slightly different velocities/resistances/move to different noisily
// placed locations on different frames."
//
// TWO MECHANISMS, BOTH ABOUT A RIG BEING TREATED AS N INDEPENDENT BODIES, and
// this gate measures one number for each:
//
//   * A worn shell used to be a DYNAMIC body on a Fixed constraint to the limb
//     it covers, geometrically inside it and an order of magnitude heavier or
//     lighter. That is the textbook energy source for a sequential-impulse
//     solver, and even when it behaves it LAGS under acceleration. It is now a
//     kinematic follower with no constraint at all (MobLimb::wornHost,
//     Mob::DriveWornShells), so its offset from its host is not solved for, it
//     is derived — and therefore CONSTANT. That is what `shell drift` and
//     `shell twist` assert, and constant is a far stronger claim than small.
//
//   * DebrisSystem::UntunnelBody clamped each body of the rig SEPARATELY
//     against the collision patches, teleporting whichever ones had outrun the
//     streaming back to their own last vouched sample while their neighbours
//     kept travelling — a fresh multi-voxel violation at every joint, every
//     tick of the starvation window, which the solver then closes by force.
//     UntunnelRig clamps the rig on one common fraction of the step instead.
//     `limb stretch` is what sees this: a joint held open by 30 voxels is not a
//     pose, it is a tear.
//
// THE TWO ARMS ARE THE TWO REGIMES:
//
//   A. THE FALL. A dressed human made limp `fallLift` voxels up (as high as the
//      residency window allows, capped at 200 ≈ 50 m) and dropped under Jolt's
//      own gravity, so it arrives at ~30 m/s having covered ground no collision
//      patch existed over. This is the arm that outruns the streaming, and the
//      untunnel counters are reported beside it so a number that moves has its
//      cause attached (CLAUDE.md rule 6) rather than needing a bisection.
//   B. THE SLAM. The same rig limp on the spot and handed 30 m/s straight down.
//      No streaming starvation, so it isolates the constraint: whatever drift
//      shows here is the garment lagging its limb under pure acceleration and
//      impact.
//
// WHY BOTH ARMS START LIMP RATHER THAN FALLING INTO IT. An NPC's kinematic fall
// (MobSystem::UpdateFall) cannot get anywhere near these speeds and by design
// never will: gravity WAITS on a column the CPU mirror has not delivered
// (GroundSense::groundUnknown), and the mirror is 3x3x3 chunks, so a creature
// with 50 m of air under it does not fall at all — it hovers. A long fall in the
// game is therefore always the same two beats: a short kinematic drop while the
// ground is still in probe range, then ragdoll.fallSeconds fires and JOLT owns
// the rest of the descent. The second beat is the whole of what the report is
// about and it is the only beat that reaches 30 m/s, so the fixture starts
// there. A first version of this gate spawned the creature 200 voxels up and
// waited for its air clock: 400 ticks, limp never fired, peak limb speed 0.0
// m/s, and every number below it a clean zero measured on a body standing still
// in the sky — a fixture measuring itself.
//
// Bounds are in tests/baseline.json, so retuning what counts as "attached" is a
// JSON edit and not a rebuild. Measured with SANDVOX_NO_RIGWELD=1 (the A/B arm
// that restores the old jointed-shell + per-body-clamp behaviour) the same
// fixture is what says the thresholds mean anything at all.
Status GateRagdollDress(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  DebrisSystem& debris = c.debris;
  MobSystem& mobs = c.mobs;
  Physics& phys = c.phys;

  int wizDef = -1;
  for (size_t i = 0; i < mobs.Defs().size(); i++)
    if (mobs.Defs()[i].name == kAvatarDefName) wizDef = (int)i;
  if (wizDef < 0) {
    detail = "no avatar mob def to dress";
    return Status::Fail;
  }
  const int baseLimbs = (int)mobs.Defs()[wizDef].limbs.size();

  int relief = 0;
  const IVec3 anchor = AiFixtureCentre(world);
  const IVec3 spot = AiFlatSpot(anchor.x, anchor.z, 96, 16, kDefaultSeed, relief);
  const int h = spot.y;
  // HOW HIGH THE FALL MAY START: inside the residency window with the margin
  // MobSystem::PreTick despawns past, never an absolute coordinate (the trap
  // AiFixtureCentre exists for — an earlier gate leaves the window elsewhere).
  const IVec3 wlo = world.WindowOrigin();
  const float fallLift =
      std::min(200.0f, std::max(0.0f, (float)(wlo.y + kWorldN - 24 - (h + 1))));

  // One shell's rigid relationship to the limb it is strapped to, captured
  // while the creature is intact and upright.
  struct Strap {
    int shell = -1, host = -1;
    Vec3 relPos{};   // shell origin in the host's frame
    Quat relRot{};   // shell rotation relative to the host's
  };
  // ...and one JOINT's, for the same reason. `anchorInParent` is the child's own
  // joint anchor expressed in the PARENT's frame, which is precisely the point a
  // ball or hinge constraint welds and therefore the one number that is
  // invariant under any pose the rig can legally take. (The obvious measure —
  // how far the two body origins sit apart — is not: a thigh pivoting about a
  // held hip moves its own origin, and the first version of this gate duly
  // reported 2.40 voxels of "stretch" on an upper arm that was simply rotating.)
  struct Link {
    int child = -1, parent = -1;
    Vec3 anchorInParent{};
  };

  struct Arm {
    int shells = 0, links = 0, ticks = 0;
    float drift = 0.0f, twist = 0.0f, stretch = 0.0f;
    float peakSpeed = 0.0f;   // fastest limb seen, m/s
    int driftTick = -1;
    std::string driftName, stretchName;
    int limpAt = -1;
    float lowest = 1.0e30f;
    bool spawned = false;
    uint32_t holds = 0, rigHolds = 0, released = 0;
  };

  auto run = [&](float liftVox, float hitVox, int ticks) -> Arm {
    Arm a;
    mobs.Reset();
    debris.Reset();
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    AiTicker ticker{c, 21000, {spot.x >> 4, spot.y >> 4, spot.z >> 4}};
    const uint64_t id =
        mobs.Spawn(wizDef, {spot.x, h + 1 + (int)liftVox, spot.z});
    if (id == 0) return a;
    Mob* mob = mobs.FindMobById(id);
    if (mob == nullptr) return a;
    a.spawned = true;
    // DRESS IT IN EVERYTHING THE LIBRARY SHIPS, counted and never named — the
    // same construction `corpse-armor` uses, and for the same reason: a tree
    // with no armour content still runs this gate, it simply runs it on a naked
    // body and the detail line says so.
    for (const ItemDef& it : c.items.items) {
      if (!ItemKindIsWorn(it.kind)) continue;
      int home = -1;
      for (int s = 0; s < kEquipSlotCount; s++)
        if (EquipSlotAccepts(s, it.kind)) { home = s; break; }
      if (home >= 0) mob->WearItem(&it, home);
    }
    // ONE TICK BEFORE THE REFERENCE IS TAKEN: WearItem places each shell itself,
    // so the pair is already rigid, but a reference read off the placement would
    // be measuring the placement. One full tick means the pose pipeline and
    // PostStep have both run and the number below is what the ENGINE maintains.
    ticker();
    auto xfOf = [&](int limb, BodyTransform& out) {
      const uint64_t b = mobs.LimbBody(id, limb);
      return b != 0 && phys.GetTransform(b, out);
    };
    std::vector<Strap> straps;
    for (int s = baseLimbs; s < mob->LimbCount(); s++) {
      // By DEF PARENT NAME rather than through MobLimb::wornHost: the A/B arm
      // (SANDVOX_NO_RIGWELD) does not set that field, and an arm that could not
      // find the shells would report a clean zero for the behaviour it exists
      // to demonstrate — a fixture measuring itself.
      if (mob->LimbDefAt(s).tag != "worn") continue;
      const std::string& parent = mob->LimbDefAt(s).parent;
      int host = -1;
      for (int p = 0; p < baseLimbs; p++)
        if (mob->LimbDefAt(p).name == parent) { host = p; break; }
      BodyTransform hs{}, ss{};
      if (host < 0 || !xfOf(host, hs) || !xfOf(s, ss)) continue;
      const Quat hq{hs.quat[0], hs.quat[1], hs.quat[2], hs.quat[3]};
      const Quat sq{ss.quat[0], ss.quat[1], ss.quat[2], ss.quat[3]};
      Strap st;
      st.shell = s;
      st.host = host;
      st.relPos = QuatRotateInv(hq, ss.pos - hs.pos);
      st.relRot = QuatMul(QuatConj(hq), sq);
      straps.push_back(st);
    }
    std::vector<Link> links;
    for (int i = 0; i < baseLimbs && i < mob->LimbCount(); i++) {
      const std::string& parent = mob->LimbDefAt(i).parent;
      if (parent.empty()) continue;
      int pi = -1;
      for (int p = 0; p < baseLimbs; p++)
        if (mob->LimbDefAt(p).name == parent) { pi = p; break; }
      BodyTransform pp{};
      if (pi < 0 || mobs.LimbBody(id, i) == 0 || !xfOf(pi, pp)) continue;
      const Quat pq{pp.quat[0], pp.quat[1], pp.quat[2], pp.quat[3]};
      Link l;
      l.child = i;
      l.parent = pi;
      l.anchorInParent =
          QuatRotateInv(pq, mobs.LimbAnchorPos(id, i) - pp.pos);
      links.push_back(l);
    }
    a.shells = (int)straps.size();
    a.links = (int)links.size();
    debris.ResetUntunnelProbe();
    // Limp from here, with whatever head start this arm asked for. minSeconds is
    // long enough to cover the whole descent: a get-up part-way down would put
    // the rig back on the pose pipeline and end the measurement early.
    mob->StartRagdoll(12.0f, "gate");
    mob->SetLimbVelocities(Vec3{0.0f, -hitVox, 0.0f});
    for (int t = 0; t < ticks; t++) {
      ticker();
      a.ticks = t + 1;
      if (!mobs.IsAlive(id)) break;        // splatted on landing: measured to here
      if (mobs.RagdollPhaseOf(id) == 1 && a.limpAt < 0) a.limpAt = t;
      // Only while LIMP: the claim is about the phase Jolt owns. A kinematic rig
      // is posed by the animation and its shells by DriveWornShells off the same
      // transforms, which is the case every other mob gate already covers.
      if (mobs.RagdollPhaseOf(id) != 1) continue;
      for (const Strap& st : straps) {
        BodyTransform hs{}, ss{};
        if (!xfOf(st.host, hs) || !xfOf(st.shell, ss)) continue;
        const Quat hq{hs.quat[0], hs.quat[1], hs.quat[2], hs.quat[3]};
        const Quat sq{ss.quat[0], ss.quat[1], ss.quat[2], ss.quat[3]};
        const float d = (QuatRotateInv(hq, ss.pos - hs.pos) - st.relPos).len();
        const Quat rel = QuatMul(QuatConj(hq), sq);
        const Quat err = QuatMul(QuatConj(st.relRot), rel);
        const float twist =
            2.0f * std::acos(std::min(1.0f, std::fabs(err.w))) * 57.29578f;
        if (d > a.drift) {
          a.drift = d;
          a.driftTick = t;
          a.driftName = mob->LimbDefAt(st.shell).name;
        }
        a.twist = std::max(a.twist, twist);
      }
      for (const Link& l : links) {
        BodyTransform pp{};
        if (mobs.LimbBody(id, l.child) == 0 || !xfOf(l.parent, pp)) continue;
        const Quat pq{pp.quat[0], pp.quat[1], pp.quat[2], pp.quat[3]};
        const float gap =
            (QuatRotateInv(pq, mobs.LimbAnchorPos(id, l.child) - pp.pos) -
             l.anchorInParent).len();
        if (gap > a.stretch) {
          a.stretch = gap;
          a.stretchName = mob->LimbDefAt(l.child).name;
        }
      }
      for (int i = 0; i < baseLimbs; i++) {
        const uint64_t b = mobs.LimbBody(id, i);
        Vec3 lin{}, ang{};
        if (b && phys.GetBodyVelocities(b, lin, ang))
          a.peakSpeed = std::max(a.peakSpeed, lin.len() * kVoxelMeters);
      }
      a.lowest = std::min(a.lowest, mobs.MobRootPos(id).y);
    }
    const DebrisSystem::UntunnelProbe& up = debris.Untunnel();
    a.holds = up.holds;
    a.rigHolds = up.rigHolds;
    a.released = up.released;
    return a;
  };

  const double maxDrift = BaselineNumber("ragdollDressMaxShellDriftVox", 0.05);
  const double maxTwist = BaselineNumber("ragdollDressMaxShellTwistDeg", 0.5);
  const double maxStretch = BaselineNumber("ragdollDressMaxLimbStretchVox", 2.0);

  // 200 voxels under gravity is ~3.2 s = 195 ticks; 300 leaves room for the
  // landing and for a shorter drop if the window is tight.
  const Arm fall = run(fallLift, 0.0f, 300);
  const Arm slam = run(0.0f, 30.0f / kVoxelMeters, 90);

  bool ok = true;
  auto judge = [&](const char* what, const Arm& a, bool needLimp) {
    const bool armOk = a.spawned && (!needLimp || a.limpAt >= 0) &&
                       a.drift <= (float)maxDrift &&
                       a.twist <= (float)maxTwist &&
                       a.stretch <= (float)maxStretch;
    std::printf(
        "  ragdoll dress %s: %s (%d shells on %d links, limp at tick %d, %d "
        "ticks, peak limb %.1f m/s; shell drift %.3f vox (cap %.2f, worst "
        "\"%s\" at tick %d), twist %.2f deg (cap %.2f), limb stretch %.2f vox "
        "(cap %.2f, worst \"%s\"); untunnel %u body-holds / %u rig-holds / %u "
        "released)\n",
        what, armOk ? "PASS" : "FAIL", a.shells, a.links, a.limpAt, a.ticks,
        a.peakSpeed, a.drift, maxDrift, a.driftName.c_str(), a.driftTick,
        a.twist, maxTwist, a.stretch, maxStretch, a.stretchName.c_str(),
        a.holds, a.rigHolds, a.released);
    ok = ok && armOk;
  };
  judge("fall", fall, /*needLimp=*/true);
  judge("slam", slam, /*needLimp=*/true);

  // A NAKED SUBJECT IS NOT A PASS. Every number above is a maximum over the
  // shells, so with no worn content in the tree they are all zero and the gate
  // would be green having measured nothing. `armor-wear` is what asserts the
  // content exists; this says out loud that it did not find any.
  if (fall.shells == 0 && slam.shells == 0) {
    detail = "no worn item in the library fits " + std::string(kAvatarDefName) +
             ": nothing was measured";
    mobs.Reset();
    debris.Reset();
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    return Status::Skip;
  }

  detail = Format(
      "fall %.0f vox: %d shells, drift %.3f / twist %.2f deg / stretch %.2f "
      "vox at up to %.0f m/s (%u rig-holds); slam 30 m/s: drift %.3f / twist "
      "%.2f / stretch %.2f; caps %.2f / %.2f / %.2f; fixture (%d,%d,%d) relief %d",
      fallLift, fall.shells, fall.drift, fall.twist, fall.stretch,
      fall.peakSpeed, fall.rigHolds, slam.drift, slam.twist, slam.stretch,
      maxDrift, maxTwist, maxStretch, spot.x, h, spot.z, relief);
  mobs.Reset();
  debris.Reset();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  std::printf("ragdoll dress: %s\n", ok ? "PASS" : "FAIL");
  return ok ? Status::Pass : Status::Fail;
}

// ---- ragdoll-falldamage -------------------------------------------------
//
// A LIMP BODY STILL HITS THE GROUND — AND A LIMP BODY IN FLIGHT DOES NOT.
//
// Three arms over one fixture, and the SET is the claim. Impact damage is
// billed off Player::impactDeltaV, which is velocity the CONTROLLER'S SWEEP
// refused; a ragdoll has no controller (main.cpp teleports the capsule onto the
// pelvis with its velocity zeroed every tick), so the sweep never refuses
// anything and a limp landing used to be free. That made a long fall SAFER than
// a short one: fall 2.9 s and the sweep splatters you, fall 3.1 s and
// ragdoll.fallSeconds flips you limp first and you land for nothing.
// Mob::TakeRagdollImpact is the same measurement taken off the solver instead.
//
// The other two arms are the ones that stop the fix being worse than the bug.
// The signal is a velocity DIFFERENCE, and two things change a limp body's
// velocity without anything hitting it: gravity, every tick, forever; and a
// blast, which SETS it outright. Either read as an impact would kill the player
// for being in the air. Deceleration-only + peak-hold + the reseed on
// SetLimbVelocities are what prevent it, and arms (b) and (c) are what say so.
//
// A gate, not a `ragdoll` arm: that gate's arms inherit each other's tick phase
// and two of them are knife-edge on it.
Status GateRagdollFallDamage(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  DebrisSystem& debris = c.debris;
  MobSystem& mobs = c.mobs;
  Physics& phys = c.phys;
  const auto& pt = CurrentTuning().player;

  int relief = 0;
  const IVec3 anchor = AiFixtureCentre(world);
  const IVec3 spot = AiFlatSpot(anchor.x, anchor.z, 96, 16, kDefaultSeed, relief);
  const int h = spot.y;
  const IVec3 pchunk{spot.x >> 4, spot.y >> 4, spot.z >> 4};

  // One run of the fixture: spawn the avatar `liftVox` above the flat spot,
  // settle it, flip it limp, hand the whole rig `hitVox` of downward velocity
  // and tick. Returns the health it lost and whether it survived.
  struct Run {
    int32_t before = 0, after = 0;
    bool alive = false, landed = false;
    float lowest = 0.0f;
  };
  auto run = [&](float liftVox, float hitVox, int ticks) {
    mobs.Reset();
    debris.Reset();
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    uint32_t tick = 12000;
    PlayerAvatar avatar;
    avatar.Init(&phys, &world, &debris, c.mats, &mobs);
    avatar.SetDefs(&mobs.Defs(), kAvatarDefName);
    Player pl;
    pl.fly = false;
    pl.grounded = true;
    pl.pos = Vec3{(float)spot.x + 0.5f, (float)(h + 2) + Player::kHalfY + liftVox,
                  (float)spot.z + 0.5f};
    Run r;
    if (!avatar.Spawn(pl, 0.0f)) return r;
    const uint64_t proxy = phys.CreatePlayerBody(Player::kHalfXZ, Player::kHalfY);
    auto avTick = [&]() {
      std::vector<BrushOp> ops;
      std::vector<ParticleSpawn> spawns;
      std::vector<CellOp> cellOps;
      phys.MovePlayerBody(proxy, pl.pos, kTickDt);
      avatar.PreTick(tick + 1, pl, 0.0f, kTickDt, world, ops, cellOps, spawns);
      debris.QueueSupportEvents(world.Snap());
      debris.PreTick(tick + 1, world, cellOps, spawns);
      ++tick;
      SubmitTick(ctx, world, sim, tick, kDefaultSeed, ops, {}, cellOps, false,
                 pchunk, true, true, spawns);
      ctx.WaitIdle();
      ctx.ProcessEvents();
      phys.Step(kTickDt);
      debris.PostStep();
      avatar.PostStep();
      Vec3 follow;
      if (avatar.RagdollFollow(follow)) pl.pos = follow;
    };
    // Standing still long enough for the feet to plant and, at lift > 0, for
    // the collision patches around the body to exist: an arm about what a
    // LANDING costs must not be measuring a streaming stutter.
    for (int i = 0; i < 40; i++) avTick();
    r.before = avatar.TotalHealth();
    avatar.StartRagdoll(2.0f, "gate");
    avatar.SetLimbVelocities(Vec3{0, -hitVox, 0});
    r.lowest = 1e30f;
    for (int i = 0; i < ticks && avatar.IsAlive(); i++) {
      avTick();
      r.lowest = std::min(r.lowest, avatar.RootWorldPos().y);
    }
    r.after = avatar.TotalHealth();
    r.alive = avatar.IsAlive();
    // The body is ON the ground, not through it. Generous: the pelvis sits
    // about a metre up a standing rig and this is about tunnelling, not pose.
    r.landed = r.lowest > (float)h - 8.0f;
    avatar.Despawn();
    phys.RemoveBody(proxy);
    return r;
  };

  // (a) A SUB-LETHAL LANDING COSTS HEALTH. 15 m/s is comfortably over
  //     fallDamageSpeed (8) and under fallSplatSpeed (25), so the assertion is
  //     "hurt, not killed" — a threshold that moved either way would show.
  const float subVox = 15.0f / kVoxelMeters;
  const Run sub = run(0.0f, subVox, 40);
  const bool subOk = sub.before > 0 && sub.after < sub.before && sub.alive &&
                     sub.landed;
  std::printf("  ragdoll fall damage: %s (limp landing at 15.0 m/s cost %d of "
              "%d hp, alive %d, pelvis never below y=%.1f vs ground %d)\n",
              subOk ? "PASS" : "FAIL", sub.before - sub.after, sub.before,
              (int)sub.alive, sub.lowest, h);

  // (b) A LETHAL ONE KILLS. Above fallSplatSpeed, which is the branch that
  //     carves the body apart rather than spending health.
  const float lethalVox = (pt.fallSplatSpeed + 8.0f) / kVoxelMeters;
  const Run lethal = run(0.0f, lethalVox, 40);
  const bool lethalOk = !lethal.alive && lethal.landed;
  std::printf("  ragdoll splat: %s (limp landing at %.1f m/s vs splat speed "
              "%.1f: alive %d, pelvis never below y=%.1f vs ground %d)\n",
              lethalOk ? "PASS" : "FAIL", lethalVox * kVoxelMeters,
              pt.fallSplatSpeed, (int)lethal.alive, lethal.lowest, h);

  // (c) FLIGHT IS FREE. The same lethal velocity, but aimed UP from 24 voxels
  //     off the ground and sampled only while the body is still rising: no
  //     contact, so gravity is the only thing changing the velocity and the
  //     arrest must read nothing. This is the arm that fails if the
  //     deceleration test, the peak-hold or the SetLimbVelocities reseed goes.
  //     12 ticks is 0.4 s — the body is still above where it started.
  const Run flight = run(24.0f, -lethalVox, 12);
  const bool flightOk = flight.before > 0 && flight.after == flight.before &&
                        flight.alive;
  std::printf("  ragdoll flight free: %s (launched UP at %.1f m/s, 12 ticks "
              "with nothing touched: %d of %d hp left, alive %d)\n",
              flightOk ? "PASS" : "FAIL", lethalVox * kVoxelMeters,
              flight.after, flight.before, (int)flight.alive);

  const bool ok = subOk && lethalOk && flightOk;
  detail = Format(
      "sub-lethal 15 m/s: -%d of %d hp alive %d; splat %.0f m/s: alive %d; "
      "0.4 s of upward flight at %.0f m/s: -%d hp; fixture (%d,%d,%d) relief %d",
      sub.before - sub.after, sub.before, (int)sub.alive,
      lethalVox * kVoxelMeters, (int)lethal.alive, lethalVox * kVoxelMeters,
      flight.before - flight.after, spot.x, h, spot.z, relief);
  mobs.Reset();
  debris.Reset();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  std::printf("ragdoll falldamage: %s\n", ok ? "PASS" : "FAIL");
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
      // A creature that is a recolour of another one, born already bitten,
      // whose cuts do not close. Counts only, and no ticks at all — rot
      // happens inside Spawn.
      {"undead", "mob", {}, false, GateUndead, /*needsRender=*/false},
      // Creatures give each other room instead of piling into one point.
      // Two arms in one gate, the second with spacing zeroed IN THE DEF, so
      // the fixture has to prove it crowds before "they did not overlap"
      // means anything.
      {"crowd", "mob", {}, false, GateCrowd, /*needsRender=*/false},
      // NPC AI. No render either: every claim is a distance, an angle or a
      // count, which is what makes them iterable with `--gate` alone.
      {"ai-dummy", "mob", {}, false, GateAiDummy, /*needsRender=*/false},
      {"ai-face", "mob", {}, false, GateAiFace, /*needsRender=*/false},
      {"ai-approach", "mob", {}, false, GateAiApproach, /*needsRender=*/false},
      // The coupling between a weapon's LENGTH and where a creature puts its
      // feet -- the one configuration every fixture above happens to avoid,
      // because each of them is either armed with a sword or nailed down.
      {"ai-reach", "mob", {}, false, GateAiReach, /*needsRender=*/false},
      // The sloped-terrain regime the three above never touch: a real ramp,
      // asserting the body stays ON it (never inside it) and does not lean
      // like furniture while climbing.
      {"ai-slope", "mob", {}, false, GateAiSlope, /*needsRender=*/false},
      // A body with no legs lies ON the slope and stops re-aiming every voxel.
      {"crawl-slope", "mob", {}, false, GateCrawlSlope, /*needsRender=*/false},
      // Live ragdoll: blast knockdown, get-up, NPC gravity. Counts only.
      {"ragdoll", "mob", {}, false, GateRagdoll, /*needsRender=*/false},
      // ...and what a limp landing COSTS. Its own gate rather than another
      // `ragdoll` arm: that gate's arms inherit each other's tick phase.
      {"ragdoll-falldamage", "mob", {}, false, GateRagdollFallDamage,
       /*needsRender=*/false},
      // ...and whether the thing that landed is still ONE body. Dressed, limp,
      // 50 m of fall: the clothes may not leave the limbs and the joints may not
      // be torn open by the anti-tunnel clamp.
      {"ragdoll-dress", "mob", {}, false, GateRagdollDress,
       /*needsRender=*/false},
  };
  return g;
}

}  // namespace selftest
