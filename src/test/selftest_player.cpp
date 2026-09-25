// selftest_player.cpp — player selftest gates.
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

#include <span>

#include "game/ai_behavior.h"
#include "game/avatar.h"
#include "game/mob.h"
#include "game/player.h"
#include "game/remoteplayer.h"  // the ghost seams: `remote-ghost` calls the
                               // SAME free functions session.cpp's phases do
#include "game/session.h"   // PrefetchChunksAround -- the gate and the real
                            // tick must call ONE definition of it
#include "sim/materials.h"
#include "sim/tuning.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// ---- player-walk -------------------------------------------------------
Status GatePlayerWalk(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const std::vector<MaterialDef>& mats = c.mats;
// player walk test: drop onto terrain through the real async-mirror path
bool walkOk = false;
{
  // Same COLLISION table the game builds, so a gate can never pass against
  // collision behaviour the player does not actually have (passable
  // vegetation reads as gas — sim/materials.h).
  std::vector<uint32_t> classOf = BuildCollisionClasses(mats);
  Player player;
  player.fly = false;
  int h = World::TerrainHeight(140, 140, kDefaultSeed);
  player.pos = Vec3{140.5f, (float)(h + 30), 140.5f};
  auto kindAt = [&](IVec3 c) { return world.KindAt(c, classOf); };
  uint32_t t = 200;
  for (int i = 0; i < 240; i++) {
    IVec3 pc{ifloor(player.pos.x) / (int)kChunk, ifloor(player.pos.y) / (int)kChunk,
             ifloor(player.pos.z) / (int)kChunk};
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false, pc, true, false);
    ctx.WaitIdle();
    ctx.ProcessEvents();  // deliver the mirror
    player.Update(1.0f / 30.0f, TickInput{}, Vec3{1, 0, 0}, Vec3{0, 0, 1},
                  Vec3{1, 0, 0}, kindAt);
    if (player.grounded) break;
  }
  float feet = player.pos.y - Player::kHalfY;
  walkOk = player.grounded && std::abs(feet - (float)(h + 1)) < 4.0f;
  std::printf("player walk: %s (grounded=%d feet y=%.1f, terrain h=%d)\n",
              walkOk ? "PASS" : "FAIL", player.grounded ? 1 : 0, feet, h);

  // ---- UNSTICK: a body inside solid ground must climb back out -----------
  //
  // Every collision sweep in player.cpp is a hard VETO — it refuses any
  // substep that ENDS overlapping a solid. That is correct from outside the
  // world and a trap from inside it: once the AABB overlaps a voxel, the
  // FIRST substep of every axis fails, including the upward ones, so the
  // player cannot move at all and has to noclip out. On noisy terrain you get
  // there constantly (a powder settles into your feet, a step-down lands a
  // fraction inside a face), which is what made walking rough ground feel
  // like it kept swallowing you.
  //
  // Two halves, and the second matters as much as the first: shallow burial
  // must eject, and DEEP burial must NOT — being entombed is a real state the
  // world can put you in, and teleporting out of it would be the worse bug.
  // Testing only the ejection would pass with an unconditional escape hatch.
  bool unstickOk = false;
  if (walkOk) {
    const Vec3 standing = player.pos;
    // (a) shallow: sink the body two voxels into the floor it is standing on
    // and require it to rise clear within a second of ticks.
    player.pos.y = standing.y - 2.0f;
    player.vel = Vec3{0, 0, 0};
    for (int i = 0; i < 30; i++)
      player.Update(1.0f / 30.0f, TickInput{}, Vec3{1, 0, 0}, Vec3{0, 0, 1},
                    Vec3{1, 0, 0}, kindAt);
    // Clear means the AABB no longer overlaps: the surest statement of that
    // from out here is that the body got back to roughly where it was
    // standing, rather than staying where we buried it.
    float roseTo = player.pos.y;
    bool shallowFreed = roseTo > standing.y - 0.75f;

    // (b) deep: drop the body below the surface far enough that the ejection
    // cap refuses it, and require it to STAY there.
    //
    // DEPTH IS BOUNDED BY THE CPU MIRROR, not just by the cap. The mirror is
    // 3x3x3 chunks around the player's last SubmitTick position, and past it
    // KindAt returns Unknown — which Collides() does not treat as blocking.
    // So a body dropped 25 voxels down (the first version of this fixture)
    // is not entombed at all: it is in Unknown space, free to move, and
    // "did not move" passed for entirely the wrong reason. 14 voxels is
    // comfortably past unstickMaxDepth (0.9 m = 9 voxels) while staying
    // inside the one chunk below the player that the mirror actually holds.
    player.pos = Vec3{standing.x, standing.y - 14.0f, standing.z};
    player.vel = Vec3{0, 0, 0};
    float buriedAt = player.pos.y;
    // ...and CONFIRM the fixture actually buries it. "Did not move" passes
    // trivially if the body is somewhere it was never stuck — a cave, or
    // (the trap that caught the first version of this gate) Unknown space
    // outside the CPU mirror, which Collides() does not treat as blocking.
    //
    // Sample INSIDE the AABB, not around it: the question is whether the
    // capsule is packed in rock. Only Solid counts, so an Unknown reading
    // fails the fixture rather than silently standing in for rock.
    // The precondition to assert is the DIRECT one — is the body overlapping
    // solid? — not a proxy for it. Counting surrounding rock invites an
    // arbitrary threshold that real terrain (caves, pockets) fails for
    // reasons that have nothing to do with the behaviour under test. Sweep a
    // zero-length move instead: SweepAxis returns blocked exactly when the
    // AABB it lands in overlaps a solid, which is the same test the movement
    // code itself is stuck on. Probing every cell the box spans is what
    // Collides does, so a hit anywhere in the span is a real overlap.
    int solidAround = 0, sampled = 0;
    {
      const float hx = Player::kHalfXZ, hy = Player::kHalfY;
      for (int y = ifloor(player.pos.y - hy); y <= ifloor(player.pos.y + hy);
           y++)
        for (int z = ifloor(player.pos.z - hx);
             z <= ifloor(player.pos.z + hx); z++)
          for (int x = ifloor(player.pos.x - hx);
               x <= ifloor(player.pos.x + hx); x++) {
            sampled++;
            if (kindAt({x, y, z}) == CellKind::Solid) solidAround++;
          }
    }
    // For this half to mean anything the body must really be overlapping —
    // otherwise the sweeps were never vetoing anything and "did not move"
    // says nothing. Whether the cap then refuses the lift is what the drift
    // assertion below measures; both together are the statement that a
    // too-deep body stays put BECAUSE it is too deep.
    bool reallyBuried = solidAround > 0;
    for (int i = 0; i < 30; i++)
      player.Update(1.0f / 30.0f, TickInput{}, Vec3{1, 0, 0}, Vec3{0, 0, 1},
                    Vec3{1, 0, 0}, kindAt);
    float buriedDrift = std::abs(player.pos.y - buriedAt);
    bool deepStaysBuried = buriedDrift < 2.0f;

    unstickOk = shallowFreed && deepStaysBuried && reallyBuried;
    std::printf(
        "player unstick: %s (sunk 2.0 vox -> rose %.2f, buried 14 vox in "
        "%d/%d solid -> moved %.2f)\n",
        unstickOk ? "PASS" : "FAIL", roseTo - (standing.y - 2.0f),
        solidAround, sampled, buriedDrift);
  } else {
    std::printf("player unstick: SKIP (walk gate failed first)\n");
  }
  walkOk = walkOk && unstickOk;
}

  // Verdict: the flag the moved body already computed.
  return walkOk ? Status::Pass : Status::Fail;
}

// ---- player-waterjump --------------------------------------------------
//
// Getting OUT of a pool. Swim thrust is drag-limited by design, so it cannot
// climb anything on its own — before the water-edge jump, pressing into the rim
// of a pool left you bobbing against it indefinitely. The mechanic under test
// turns a jump pressed INTO a climbable ledge while in liquid into a real jump
// impulse (player.cpp WaterLedgeAhead).
//
// The world here is SYNTHETIC — a pure kindAt lambda, no GPU, no terrain. That
// is deliberate on two counts. The geometry this needs (a pool with a rim at a
// known height, and a sheer wall with no rim) does not occur at a known
// location in generated terrain, and gates run in sequence with the streaming
// gate leaving the window origin ~20 chunks out, so anything anchored to a
// world position is fragile. A lambda states the fixture exactly and the code
// under test cannot tell the difference.
//
// Four assertions, and the negative ones carry the weight: an unconditional
// "jump works in water" would pass the first alone. The fourth is the other
// side of the same coin — WADING is not swimming, and a body standing in
// ankle-deep water still jumps like any other body standing on the ground.
Status GatePlayerWaterJump(Ctx&, std::string& detail) {
  // Pool: solid floor at y < 100, water in 100..129 for x < 140, and a solid
  // bank from x >= 140 rising to y = 134. So the water surface is y=130 and the
  // lip is 4 voxels above it.
  //
  // TWO NUMBERS HERE ARE LOAD-BEARING, and both were wrong in earlier versions
  // of this fixture in ways that made it test nothing:
  //
  // The pool is 30 voxels deep against a 17-voxel body so the player actually
  // SWIMS. At 10 voxels the body just stood on the bottom with its head out,
  // which is wading, not swimming.
  //
  // The bank is 4 voxels proud of the water because kMaxStepUpVoxels is ~6:
  // a 2-voxel lip is an ordinary STEP, and a floating body whose feet have
  // risen above the waterline simply walks over it with no water-edge logic
  // involved at all. The gate passed that way once and was measuring the step
  // code. It must be tall enough that only the mantle can clear it, and low
  // enough to still be a bank you would expect to climb.
  const float kWater = 130.0f, kRim = 134.0f;
  auto poolKind = [&](IVec3 c) {
    if (c.y < 100) return CellKind::Solid;
    if (c.x >= 140) return c.y < (int)kRim ? CellKind::Solid : CellKind::Air;
    return c.y < (int)kWater ? CellKind::Liquid : CellKind::Air;
  };
  // Sheer wall: same pool, but the barrier runs up forever. Nothing to land on,
  // so the boost must NOT fire — otherwise you climb any wall from any depth.
  auto cliffKind = [&](IVec3 c) {
    if (c.y < 100) return CellKind::Solid;
    if (c.x >= 140) return CellKind::Solid;
    return c.y < (int)kWater ? CellKind::Liquid : CellKind::Air;
  };

  const Vec3 fwd{1, 0, 0}, right{0, 0, 1};
  const float dt = 1.0f / 60.0f;

  // Float the body at the surface, right up against the rim, and run it until
  // the swim/drag state settles so the measurement starts from equilibrium
  // rather than from whatever the drop-in left behind.
  //
  // `up` is held for the whole settle, and that is not incidental: buoyancy is
  // only a gravity SCALE (liquidGravityScale 0.25), so a body that stops
  // paddling sinks to the pool floor. Treading water at the surface is what a
  // player about to climb out is actually doing, and it is the only state in
  // which the rim is within a step. The first version of this fixture pressed
  // forward alone, settled on the bottom 12 voxels down, and read as a failure
  // of the mechanic when it was really a failure to be at the surface.
  auto settleAtRim = [&](const Player::KindFn& kindAt) {
    Player p;
    p.fly = false;
    // Start clear of the wall and let the swim press close the gap. The AABB is
    // kHalfXZ wide, so spawning at the wall face (139.4, the first version of
    // this fixture) puts the box INSIDE the rim: every sweep then vetoes from
    // an overlapping start, the body cannot move on any axis, and the gate
    // reads as "the mechanic did nothing" when really nothing was ever able to
    // move. Two body-widths back is comfortably clear.
    p.pos = Vec3{140.0f - 3.0f * Player::kHalfXZ, kWater - 1.0f, 140.5f};
    TickInput swim;
    swim.forward = 1.0f;  // press into the rim the whole time
    swim.SetHeld(TB_JUMP, true);       // tread water, or buoyancy alone sinks us
    for (int i = 0; i < 120; i++) p.Update(dt, swim, fwd, right, fwd, kindAt);
    return p;
  };

  // (a) POOL: pressing into the rim and jumping must put the feet ABOVE the lip.
  // The assertion is positional, not "did velocity go up": a boost that lifts
  // you a little and drops you back in is the bug this exists to catch, so the
  // only statement worth making is that the body ends up out of the water and
  // on top of the rim.
  Player::KindFn poolFn = poolKind;
  Player pool = settleAtRim(poolFn);
  float floatFeet = pool.pos.y - Player::kHalfY;
  // Precondition, asserted not assumed: treading water at the SURFACE, clear
  // of the pool floor at y=100. Standing on the bottom would make every
  // negative assertion below pass for the wrong reason (nothing is within a
  // step of the bottom, so nothing would fire regardless of the mechanic).
  bool wasSwimming = pool.inLiquid && floatFeet > 105.0f;
  {
    TickInput jump;
    jump.forward = 1.0f;
    jump.SetPressed(TB_JUMP, true);
    jump.SetHeld(TB_JUMP, true);
    pool.Update(dt, jump, fwd, right, fwd, poolFn);
  }
  bool fired = pool.waterJumped;
  // Let the climb play out and then some. Input keeps pressing forward
  // (jumpPressed is a single frame, exactly as main.cpp delivers it). The extra
  // frames past the mantle timeout matter: they are what catches a climb that
  // reaches the lip and then slides back into the pool, which is the failure
  // this whole mechanic exists to prevent.
  {
    TickInput hold;
    hold.forward = 1.0f;
    for (int i = 0; i < 180; i++) pool.Update(dt, hold, fwd, right, fwd, poolFn);
  }
  float outFeet = pool.pos.y - Player::kHalfY;
  // OUT means standing on the bank: feet at the rim top, body past the wall,
  // and no longer in the water. All three, because each alone has a way to be
  // true while still stuck — hanging on the lip, or clipped into the rim.
  // Past the wall means the whole BOX is over the bank, not just the centre —
  // the AABB is kHalfXZ wide, so a centre barely past 140 is still hanging over
  // the water.
  bool climbedOut = outFeet >= kRim - 0.5f &&
                    pool.pos.x > 140.0f + Player::kHalfXZ && !pool.inLiquid;

  // (b) SHEER WALL: same press, same jump, no ledge. Must not fire.
  Player::KindFn cliffFn = cliffKind;
  Player cliff = settleAtRim(cliffFn);
  {
    TickInput jump;
    jump.forward = 1.0f;
    jump.SetPressed(TB_JUMP, true);
    jump.SetHeld(TB_JUMP, true);
    cliff.Update(dt, jump, fwd, right, fwd, cliffFn);
  }
  bool cliffFired = cliff.waterJumped;

  // (c) OPEN WATER: pressing away from the rim, into nothing. Must not fire —
  // this is what keeps ordinary swimming unchanged.
  Player open = settleAtRim(poolFn);
  bool openFired = false;
  {
    TickInput jump;
    jump.forward = -1.0f;  // away from the rim
    jump.SetPressed(TB_JUMP, true);
    jump.SetHeld(TB_JUMP, true);
    open.Update(dt, jump, fwd, right, fwd, poolFn);
    openFired = open.waterJumped;
  }

  // (d) SHALLOW WATER: ankle-deep over a solid floor is WADING, not swimming.
  // A body there is standing on the ground and must keep everything a grounded
  // body has — above all the jump.
  //
  // This is the arm the mechanic lost for a long time and nothing noticed:
  // `inLiquid` was a bare "any of the five body samples is wet", it zeroed
  // `onGround`, and the jump lived in the `else` of it, so ONE voxel of water
  // underfoot took jumping away entirely. The swim thrust that remained is
  // scaled by submersion — 0.2 here — and cannot lift anybody. Every arm above
  // floats in 30 voxels of water, so all three passed throughout.
  //
  // The floor is at y=100 and the water is two voxels deep (100..101), which
  // wets the feet sample and nothing else: submersion 0.2, the shallowest
  // state that is still liquid, and therefore the hardest case for the rule.
  const float kFloor = 100.0f, kShallowTop = 102.0f;
  Player::KindFn wadeFn = [&](IVec3 c) {
    if (c.y < (int)kFloor) return CellKind::Solid;
    return c.y < (int)kShallowTop ? CellKind::Liquid : CellKind::Air;
  };
  Player wade;
  wade.fly = false;
  wade.pos = Vec3{128.0f, kFloor + Player::kHalfY, 140.5f};
  for (int i = 0; i < 60; i++)
    wade.Update(dt, TickInput{}, fwd, right, fwd, wadeFn);
  // Preconditions, asserted not assumed: actually in the water, and actually
  // read as standing in it rather than swimming in it. Without the first, the
  // jump below would prove nothing; without the second, it would be testing
  // the dry path.
  bool wadeWet = wade.inLiquid && !wade.swimming && wade.grounded;
  float wadeFeet0 = wade.pos.y - Player::kHalfY;
  {
    TickInput jump;
    jump.SetPressed(TB_JUMP, true);
    wade.Update(dt, jump, fwd, right, fwd, wadeFn);
  }
  float wadePeak = wadeFeet0;
  for (int i = 0; i < 90; i++) {
    wade.Update(dt, TickInput{}, fwd, right, fwd, wadeFn);
    wadePeak = std::max(wadePeak, wade.pos.y - Player::kHalfY);
  }
  // A real jump clears ~14 voxels from a dry floor; ankle-deep drag trims that
  // a little. 5 voxels is far above anything buoyancy or a bob can produce and
  // far below the true arc, so the assertion survives a jumpSpeed retune.
  bool wadeJumped = wadePeak >= wadeFeet0 + 5.0f;

  bool ok = wasSwimming && fired && climbedOut && !cliffFired && !openFired &&
            wadeWet && wadeJumped;
  char buf[320];
  std::snprintf(buf, sizeof(buf),
                "floated feet y=%.1f -> out y=%.1f x=%.1f (rim %.0f), "
                "fired=%d cliff=%d open=%d | wade wet=%d feet %.1f -> peak %.1f",
                floatFeet, outFeet, pool.pos.x, kRim, fired ? 1 : 0,
                cliffFired ? 1 : 0, openFired ? 1 : 0, wadeWet ? 1 : 0,
                wadeFeet0, wadePeak);
  detail = buf;
  std::printf("player waterjump: %s (%s)\n", ok ? "PASS" : "FAIL", buf);
  return ok ? Status::Pass : Status::Fail;
}

// ---- player-ledgegrab --------------------------------------------------
//
// Ledge grabbing: airborne with space held, arms facing a voxel lip within
// hand reach, the body latches on and dangles; W pulls it up (player.cpp
// LedgeGrabAhead + the hanging block). The gesture this exists for: jump a
// gap, fall a bit short, catch the lip, pull up — and on a wall of small
// ledges, chain grab/boost/grab to scale it.
//
// Synthetic kindAt fixtures for the same reasons the waterjump gate lists.
// The geometry is chosen so the jump ALONE always falls short: jumpSpeed
// 5.25 m/s buys 1.40 m of rise against a plateau 1.5 m above the feet, so any
// body that ends up on top provably went through the grab. The negative
// fixtures carry the weight — a sheer wall must never latch at any fall
// speed, and without space held nothing may latch at all.
Status GatePlayerLedgeGrab(Ctx&, std::string& detail) {
  // Launch floor for x<140 (top y=100), a pit floor at y=60 under the gap so
  // a missed grab lands somewhere provable, and the grab wall at x>=150.
  // (a) plateau: wall top y=115 -> lip cell y=114, open sky above.
  auto plateauKind = [](IVec3 c) {
    if (c.y < 60) return CellKind::Solid;  // pit floor
    if (c.x < 140) return c.y < 100 ? CellKind::Solid : CellKind::Air;
    if (c.x >= 150) return c.y < 115 ? CellKind::Solid : CellKind::Air;
    return CellKind::Air;
  };
  // (b) sheer wall: same launch, the wall runs far past any hand reach.
  auto cliffKind = [](IVec3 c) {
    if (c.y < 60) return CellKind::Solid;
    if (c.x < 140) return c.y < 100 ? CellKind::Solid : CellKind::Air;
    if (c.x >= 150) return c.y < 200 ? CellKind::Solid : CellKind::Air;
    return CellKind::Air;
  };
  // (d) noisy wall: the face column (x=150) stops at the same y=114 lip, but
  // more wall rises right behind it (x>=151 up to y=127). There is no room to
  // stand on that one-voxel ledge, so the pull-up must become the arm boost,
  // and the NEXT lip (y=127) must catch on the way down from the boost apex —
  // the chain. The tier height leaves headroom between the apex (anchor
  // y=108 at ledgeHangDrop -0.15 m, + 14 voxels of jumpSpeed rise = ~122)
  // and lip2's latch window (bottom y=113), so a modest jumpSpeed or
  // hang-drop retune does not silently break the gate.
  auto noisyKind = [](IVec3 c) {
    if (c.y < 60) return CellKind::Solid;
    if (c.x < 140) return c.y < 100 ? CellKind::Solid : CellKind::Air;
    if (c.x >= 151) return c.y < 128 ? CellKind::Solid : CellKind::Air;
    if (c.x == 150) return c.y < 115 ? CellKind::Solid : CellKind::Air;
    return CellKind::Air;
  };

  const Vec3 fwd{1, 0, 0}, right{0, 0, 1};
  const float dt = 1.0f / 60.0f;

  // Sprint at the wall, jump just before the edge, then fly with W (and space,
  // when `space`) pressed into it — exactly the inputs a player makes. When
  // `pauseAtHang`, W is released for half a second once the latch fires, so
  // the DANGLE itself is what is being measured: the grip must hold with no
  // input but space, and must not drift. Then W again pulls up, and keeps
  // pressing past the top (same reasoning as the waterjump's extra frames — a
  // climb that slides back off the lip must read as a failure).
  struct RunResult {
    Player p;
    bool everHung = false, droppedWhileWaiting = false;
    int firstHangFrames = 0;      // dangle length of the first latch
    bool firstHangEnded = false;
    float shimmyDz = 0.0f;        // lateral travel during the measured dangle
    float zAtHang = 0.0f;
  };
  auto run = [&](const Player::KindFn& kindAt, bool space, bool pauseAtHang,
                 int frames) {
    RunResult r;
    r.p.fly = false;
    r.p.pos = Vec3{132.0f, 100.0f + Player::kHalfY, 200.5f};
    int stage = 0, hangHeld = 0;
    for (int i = 0; i < frames; i++) {
      // Stage transitions are judged on the player's CURRENT state, before
      // this frame's input is built. Folded into the input branches (the
      // first version of this loop), the latch frame was followed by one more
      // W frame and the pull-up fired before the dangle was ever measured.
      if (stage == 1 && r.p.hanging) {
        r.everHung = true;
        r.zAtHang = r.p.pos.z;
        stage = pauseAtHang ? 2 : 3;
      } else if (stage == 2) {
        if (!r.p.hanging) r.droppedWhileWaiting = true;
        r.shimmyDz = r.p.pos.z - r.zAtHang;
        if (++hangHeld >= 30) stage = 3;
      } else if (stage == 3 && r.p.hanging) {
        r.everHung = true;  // chained re-grabs during the climb
      }
      // Length of the FIRST hang, in frames. This is what proves the settle
      // delay: with W and space both held straight through the latch, the
      // grab must still dangle for ledgePullDelay before the pull-up honours
      // the held W — the instant-mantle regression made the catch invisible.
      if (r.everHung && !r.firstHangEnded) {
        if (r.p.hanging) r.firstHangFrames++;
        else r.firstHangEnded = true;
      }
      TickInput in;
      in.SetHeld(TB_SPRINT, true);
      if (stage == 0) {  // run-up
        in.forward = 1.0f;
        if (r.p.grounded && r.p.pos.x > 135.0f) {
          in.SetPressed(TB_JUMP, true);
          in.SetHeld(TB_JUMP, space);
          stage = 1;
        }
      } else if (stage == 1) {  // flight, pressed into the wall
        in.forward = 1.0f;
        in.SetHeld(TB_JUMP, space);
      } else if (stage == 2) {  // dangle: hands only, W released
        in.SetHeld(TB_JUMP, space);
        // Shimmy while dangling: D held the whole half-second. The wall runs
        // forever in z here, so the traverse must actually travel — the
        // assertion below is what proves the hands slide AND the grip holds.
        in.strafe = 1.0f;
      } else {  // pull up and keep walking over the top
        in.forward = 1.0f;
        in.SetHeld(TB_JUMP, space);
      }
      r.p.Update(dt, in, fwd, right, fwd, kindAt);
    }
    return r;
  };

  // (a) the headline move: grab, hold a half-second dead hang (shimmying
  // right the whole time), pull up, stand. The shimmy assertion carries two
  // statements at once: the hands traverse (dz well past noise) and the grip
  // survives the traverse (droppedWhileWaiting stays false through it).
  RunResult a = run(plateauKind, true, true, 600);
  float aFeet = a.p.pos.y - Player::kHalfY;
  bool aOk = a.everHung && !a.droppedWhileWaiting && a.p.grounded &&
             !a.p.hanging && std::abs(aFeet - 115.0f) < 0.75f &&
             a.p.pos.x > 150.0f + Player::kHalfXZ && a.shimmyDz > 2.0f;

  // (b) sheer wall: same press, same fall, nothing within reach ends in air
  // with room above it. Must never latch — otherwise every wall in the world
  // is climbable — and the body must therefore end on the pit floor, which is
  // also the proof the fixture gave it nothing else to land on.
  RunResult b = run(cliffKind, true, false, 400);
  float bFeet = b.p.pos.y - Player::kHalfY;
  bool bOk = !b.everHung && bFeet < 62.0f;

  // (c) space never held: the identical jump at the grabbable plateau must
  // sail past it into the pit. Space IS the grip; W alone must not climb.
  RunResult c = run(plateauKind, false, false, 400);
  float cFeet = c.p.pos.y - Player::kHalfY;
  bool cOk = !c.everHung && cFeet < 62.0f;

  // (d) chained climb up the noisy wall: first lip unstandable -> arm boost
  // -> second lip -> mantle onto the top. Everything after the jump is just
  // "hold W and space", which is the point of the feature — and BECAUSE W is
  // held straight through the latch, the first hang's length is the settle
  // assertion: it must last most of ledgePullDelay (10 frames ~ 0.17 s of the
  // 0.25 s default) before the held W is honoured. An instant mantle here is
  // the "hanging doesn't work" regression.
  RunResult d = run(noisyKind, true, false, 900);
  float dFeet = d.p.pos.y - Player::kHalfY;
  bool dOk = d.everHung && d.p.grounded && !d.p.hanging &&
             std::abs(dFeet - 128.0f) < 0.75f && d.firstHangFrames >= 10;

  // (e) the pull-up is a HOLD, and letting go of the grip converts to a jump.
  // One scripted body: latch, serve the delay, TAP W (climb must start),
  // release W with space still held (the climb must CANCEL and lower back to
  // the hang, not run to completion), then release space and re-tap it (the
  // coyote window must turn the press into a full jump off the wall).
  bool eLatched = false, eClimbStarted = false, eBackToHang = false,
       eJumped = false;
  {
    Player p;
    p.fly = false;
    p.pos = Vec3{132.0f, 100.0f + Player::kHalfY, 200.5f};
    Player::KindFn kf = plateauKind;
    auto frames = [&](TickInput in, int n) {
      for (int i = 0; i < n; i++) p.Update(dt, in, fwd, right, fwd, kf);
    };
    {  // run-up and jump at the wall, space held from the jump on
      TickInput in;
      in.SetHeld(TB_SPRINT, true);
      in.forward = 1.0f;
      int guard = 0;
      while (!(p.grounded && p.pos.x > 135.0f) && ++guard < 400)
        p.Update(dt, in, fwd, right, fwd, kf);
      in.SetPressed(TB_JUMP, true);
      in.SetHeld(TB_JUMP, true);
      p.Update(dt, in, fwd, right, fwd, kf);
      in.SetPressed(TB_JUMP, false);
      guard = 0;
      while (!p.hanging && ++guard < 400)
        p.Update(dt, in, fwd, right, fwd, kf);
      eLatched = p.hanging;
    }
    {  // serve the pull delay, then tap W for 8 frames
      TickInput in;
      in.SetHeld(TB_JUMP, true);
      frames(in, 30);
      in.forward = 1.0f;
      frames(in, 8);
      eClimbStarted = p.mantleTimer > 0.0f;
      in.forward = 0.0f;  // W released mid-climb, space still gripping
      frames(in, 60);
      eBackToHang = p.hanging && p.mantleTimer <= 0.0f && !p.grounded &&
                    p.pos.y - Player::kHalfY < 110.0f;
    }
    {  // parkour: release space one frame, re-press the next
      TickInput in;
      frames(in, 1);
      in.SetPressed(TB_JUMP, true);
      in.SetHeld(TB_JUMP, true);
      frames(in, 1);
      eJumped = p.vel.y > 0.8f * (5.25f / kVoxelMeters);
    }
  }
  bool eOk = eLatched && eClimbStarted && eBackToHang && eJumped;

  bool ok = aOk && bOk && cOk && dOk && eOk;
  char buf[320];
  std::snprintf(buf, sizeof(buf),
                "plateau: hung=%d dropped=%d feet=%.1f x=%.1f (want 115, "
                ">153) shimmy=%.1f (want>2); cliff hung=%d feet=%.1f; "
                "no-space hung=%d feet=%.1f; chain hung=%d feet=%.1f (want "
                "128) firstHang=%df (want>=10); hold: latch=%d climb=%d "
                "cancel->hang=%d retap-jump=%d",
                a.everHung ? 1 : 0, a.droppedWhileWaiting ? 1 : 0, aFeet,
                a.p.pos.x, a.shimmyDz, b.everHung ? 1 : 0, bFeet,
                c.everHung ? 1 : 0, cFeet, d.everHung ? 1 : 0, dFeet,
                d.firstHangFrames, eLatched ? 1 : 0, eClimbStarted ? 1 : 0,
                eBackToHang ? 1 : 0, eJumped ? 1 : 0);
  detail = buf;
  std::printf("player ledgegrab: %s (%s)\n", ok ? "PASS" : "FAIL", buf);
  return ok ? Status::Pass : Status::Fail;
}

// ---- player-plants -----------------------------------------------------
//
// Soft vegetation must not stop a moving body. Pond weed, reeds and kelp are
// ordinary solid voxels — the CA runs on them, they burn, the brush and the
// laser remove them — but the player has to swim straight through, and before
// kMatFlagPassable a reed bed was a wall of solid cubes you could stand on in
// the middle of a pond.
//
// THE NEGATIVE HALF IS THE POINT. "The player moved" passes trivially if
// collision is broken outright, so this walks the SAME body the SAME distance
// into a wall built from the same fixture and requires it to be stopped. One
// assertion without the other proves nothing.
//
// The material table is the REAL one (c.mats), not a synthetic flag: the thing
// under test is that the shipped materials.json actually authors `passable` on
// the plants and that BuildCollisionClasses honours it. A hand-made table here
// would pass while the game still walled the player out of every pond.
Status GatePlayerPlants(Ctx& c, std::string& detail) {
  const std::vector<MaterialDef>& mats = c.mats;
  std::vector<uint32_t> classOf = BuildCollisionClasses(mats);

  // Look up the shipped plant materials and a known-solid control by NAME.
  auto idOf = [&](const char* n) -> uint32_t {
    for (size_t i = 0; i < mats.size(); i++)
      if (mats[i].name == n) return (uint32_t)i;
    return 0;
  };
  const uint32_t kReed = idOf("reed");
  const uint32_t kKelp = idOf("kelp");
  const uint32_t kPad = idOf("lilypad");
  const uint32_t kStone = idOf("stone");
  const bool found = kReed && kKelp && kPad && kStone;

  // Every plant must classify as non-blocking, and stone must not. This is the
  // table-level assertion; the sweep below is the behavioural one.
  auto passable = [&](uint32_t m) {
    return m < classOf.size() && classOf[m] != CLASS_SOLID &&
           classOf[m] != CLASS_POWDER;
  };
  const bool tableOk = found && passable(kReed) && passable(kKelp) &&
                       passable(kPad) && !passable(kStone);

  // Synthetic fixture, same reasoning as the waterjump gate above: floor at
  // y<100, and a slab of FILL from x>=140 that the player walks into. Running
  // it twice — once filled with reed, once with stone — isolates the material
  // as the only variable.
  auto runInto = [&](uint32_t fill) {
    auto kindAt = [&](IVec3 p) {
      if (p.y < 100) return CellKind::Solid;
      if (p.x >= 140 && p.y < 130) {
        uint32_t k = fill < classOf.size() ? classOf[fill] : (uint32_t)CLASS_SOLID;
        if (k == CLASS_SOLID || k == CLASS_POWDER) return CellKind::Solid;
        if (k == CLASS_LIQUID) return CellKind::Liquid;
        return CellKind::Gas;
      }
      return CellKind::Air;
    };
    Player p;
    p.fly = false;
    p.pos = Vec3{130.0f, 100.0f + Player::kHalfY, 130.5f};
    TickInput in{};
    in.forward = 1.0f;  // +x, straight at the slab
    const Vec3 fwd{1, 0, 0}, right{0, 0, 1};
    for (int i = 0; i < 200; i++) p.Update(1.0f / 30.0f, in, fwd, right, fwd, kindAt);
    return p.pos.x;
  };

  const float reedX = runInto(kReed);
  const float stoneX = runInto(kStone);
  // Through the reeds: well past the x=140 face. Into the stone: stopped at
  // it (the capsule half-width keeps the centre just short of 140).
  const bool sweptThrough = reedX > 150.0f;
  const bool stoppedByStone = stoneX < 141.0f;

  const bool ok = tableOk && sweptThrough && stoppedByStone;
  char buf[224];
  std::snprintf(buf, sizeof(buf),
                "table ok=%d (reed/kelp/pad passable, stone not); walked into "
                "reeds x=%.1f (through=%d), into stone x=%.1f (stopped=%d)",
                tableOk ? 1 : 0, reedX, sweptThrough ? 1 : 0, stoneX,
                stoppedByStone ? 1 : 0);
  detail = buf;
  std::printf("player plants: %s (%s)\n", ok ? "PASS" : "FAIL", buf);
  return ok ? Status::Pass : Status::Fail;
}

// ---- player-crouch -----------------------------------------------------
//
// The collision box is NOT the figure (Player::Box): a small box on the
// figure's sole, and Ctrl shrinks it. Same synthetic-lambda world as the
// ledge gate — the fixture is a floor at y=100 and, from x=150 on, a
// corridor with an exact number of free rows under a solid ceiling. Every
// height here is read off the live tuning so the gate follows the knobs.
//
// Four claims, and the first one is the reason the box exists: a corridor
// exactly as tall as the STANDING box admits the body even though the
// 17-row figure would not fit. Then: a corridor as tall as the CROUCH box
// refuses the standing body, admits it crouched, keeps it crouched when
// Ctrl is released under the ceiling, and stands it up once it walks out.
// Then the crouch walk is slower by crouchSpeedScale, and the eye drops with
// the box and never rises above it.
Status GatePlayerCrouch(Ctx&, std::string& detail) {
  const Tuning::Player& tp = CurrentTuning().player;
  const int standRows = (int)std::ceil(tp.collisionHeight / kVoxelMeters - 1e-3f);
  const int crouchRows = (int)std::ceil(tp.crouchHeight / kVoxelMeters - 1e-3f);
  const int figureRows = (int)std::lround(2.0f * Player::kHalfY);
  auto corridor = [](int headroom) -> Player::KindFn {
    return [headroom](IVec3 c) {
      if (c.y < 100) return CellKind::Solid;
      if (c.x >= 150 && c.y >= 100 + headroom) return CellKind::Solid;
      return CellKind::Air;
    };
  };
  const Vec3 fwd{1, 0, 0}, right{0, 0, 1};
  const float dt = 1.0f / 60.0f;
  auto make = [&]() {
    Player p;
    p.fly = false;
    p.pos = Vec3{140.0f, 100.0f + Player::kHalfY, 200.5f};
    return p;
  };
  auto frames = [&](Player& p, const Player::KindFn& k, TickInput in,
                    int n) {
    for (int i = 0; i < n; i++) p.Update(dt, in, fwd, right, fwd, k);
  };
  TickInput walk;
  walk.forward = 1.0f;
  TickInput crawl = walk;
  crawl.SetHeld(TB_CROUCH, true);
  TickInput back;
  back.forward = -1.0f;
  TickInput rest;

  // (a) a corridor exactly standRows tall admits the standing body.
  const Player::KindFn tall = corridor(standRows);
  Player a = make();
  frames(a, tall, walk, 240);
  const bool aIn = a.pos.x > 156.0f && !a.crouching;
  const bool aTighterThanFigure = standRows < figureRows;

  // (b) a corridor crouchRows tall: refused standing, admitted crouched,
  // crouch sticks under the ceiling, stands up after walking out.
  const Player::KindFn low = corridor(crouchRows);
  Player b = make();
  frames(b, low, walk, 180);
  const float bBlockedX = b.pos.x;
  const bool bRefused = bBlockedX < 150.0f;
  frames(b, low, crawl, 300);
  const float bInX = b.pos.x;
  const bool bAdmitted = b.crouching && bInX > 156.0f;
  frames(b, low, rest, 30);
  const bool bStuck = b.crouching;  // Ctrl released, ceiling still there
  frames(b, low, back, 420);
  const bool bStood = !b.crouching && b.pos.x < 148.0f;

  // (c) crouch speed on open floor: distance over the same window.
  const Player::KindFn open = corridor(64);
  Player c1 = make(), c2 = make();
  frames(c1, open, walk, 90);
  const float x1 = c1.pos.x;
  frames(c1, open, walk, 60);
  const float standDist = c1.pos.x - x1;
  frames(c2, open, crawl, 90);
  const float x2 = c2.pos.x;
  frames(c2, open, crawl, 60);
  const float crouchDist = c2.pos.x - x2;
  const float ratio = standDist > 1e-3f ? crouchDist / standDist : -1.0f;
  const bool cOk = std::abs(ratio - tp.crouchSpeedScale) < 0.1f;

  // (d) the eye drops with the box and stays inside it.
  const float eyeStand = c1.EyePos().y - (c1.pos.y - Player::kHalfY);
  const float eyeCrouch = c2.EyePos().y - (c2.pos.y - Player::kHalfY);
  const bool dOk = c2.crouching && eyeStand - eyeCrouch > 1.0f &&
                   eyeCrouch < c2.BoxHeight() && eyeStand < c1.BoxHeight();

  const bool ok = aIn && aTighterThanFigure && bRefused && bAdmitted &&
                  bStuck && bStood && cOk && dOk;
  char buf[360];
  std::snprintf(buf, sizeof(buf),
                "rows stand=%d crouch=%d figure=%d; tall corridor x=%.1f "
                "(want>156); low: standing x=%.1f (want<150) crouched x=%.1f "
                "(want>156) stuck=%d stood=%d x=%.1f (want<148); speed ratio "
                "%.2f (want %.2f); eye stand=%.1f crouch=%.1f",
                standRows, crouchRows, figureRows, a.pos.x, bBlockedX, bInX,
                bStuck ? 1 : 0, bStood ? 1 : 0, b.pos.x, ratio,
                tp.crouchSpeedScale, eyeStand, eyeCrouch);
  detail = buf;
  std::printf("player crouch: %s (%s)\n", ok ? "PASS" : "FAIL", buf);
  return ok ? Status::Pass : Status::Fail;
}


// ---- player-fastfall ---------------------------------------------------
//
// A LONG DROP MUST END ON THE FLOOR, NOT UNDER THE WORLD.
//
// Both arms run at dt = 0.05 s, which is not an arbitrary slow frame: it is
// the clamp Player::Update applies (a longer frame is integrated as 50 ms), so
// it is the WORST step the controller can ever be handed, and it is what a
// shader-compile or streaming hitch actually delivers. maxFall is 535 vox/s at
// kVoxelMeters 0.10, so one such step is 26.8 voxels of travel. Neither claim
// is visible in a gate that drops the body thirty voxels.
//
//  (a) THE SWEEP RESOLVES A FULL-SPEED FALL onto a ONE-VOXEL slab. SweepAxis
//      caps its substep COUNT, so its stride grows with the distance asked of
//      it; today the longest available is 3.4 voxels against a 15-row box,
//      which cannot step over anything. This pins that margin rather than a
//      past bug: raising maxFall, lowering kVoxelMeters or shrinking the
//      collision box eats it, and this is where that shows up.
//
//  (b) THE BLIND FALL, which IS a fixed bug. KindAt reports Unknown outside
//      the 3x3x3 CPU mirror and Collides has to treat Unknown as air.
//      `staleMirror` is that mirror exactly: the same chunk cube MirrorBaseFor
//      picks, centred on where the body was kLatency frames ago, because the
//      cube is built from a readback that old. It holds 16-32 voxels below the
//      feet while the body covers 26.8 a frame, so the snapshot describes a
//      cube the feet have left, the sweep finds no floor, and the fall feeds
//      itself — the body used to sail through the ground and keep going. It
//      must now stop on the floor inside that cube, and Player::blindFall must
//      have fired, or the arm proved nothing about the clamp.
//
//      The hold is a CLAMP, not a veto, and the frame count is what says so:
//      one full step is longer than the mirror's whole below-feet margin, so a
//      veto would answer "no" forever and the body would hover. `held` close
//      to the frame total with the body still arriving on the ground is the
//      shape of a descent rate-limited by the readback.
Status GatePlayerFastFall(Ctx&, std::string& detail) {
  const Vec3 fwd{1, 0, 0}, right{0, 0, 1};
  const float dt = 0.05f;       // the dt clamp: the worst step Update can see
  const int kFloor = 100;       // top solid row is kFloor - 1 in arm (b)
  const float kStart = 1700.0f; // 160 m up: terminal velocity on arrival
  const float terminal = CurrentTuning().player.maxFall / kVoxelMeters;

  // (a) a one-voxel floor, fully known. Nothing may pass it.
  Player::KindFn slab = [&](IVec3 c) {
    return c.y == kFloor ? CellKind::Solid : CellKind::Air;
  };
  Player thin;
  thin.fly = false;
  thin.pos = Vec3{140.5f, kStart, 140.5f};
  float thinPeak = 0.0f;
  for (int i = 0; i < 400 && !thin.grounded; i++) {
    thin.Update(dt, TickInput{}, fwd, right, fwd, slab);
    thinPeak = std::max(thinPeak, -thin.vel.y);
  }
  const float thinFeet = thin.pos.y + thin.CurrentBox().yLo;
  // Terminal velocity must actually have been reached, or the fixture is a
  // short drop wearing a long one's name.
  const bool fastEnough = thinPeak > 0.95f * terminal;
  const bool thinOk = thin.grounded &&
                      std::abs(thinFeet - (float)(kFloor + 1)) < 0.5f;

  // (b) the same fall against a mirror that is kLatency frames stale.
  constexpr int kLatency = 3;
  float centreHist[kLatency] = {kStart, kStart, kStart};
  float mirrorCentre = kStart;
  Player::KindFn staleMirror = [&](IVec3 c) {
    const int mc = ifloor(mirrorCentre) >> 4;   // the mirror's centre chunk
    const int cc = c.y >> 4;
    if (cc < mc - 1 || cc > mc + 1) return CellKind::Unknown;
    return c.y < kFloor ? CellKind::Solid : CellKind::Air;
  };
  Player blind;
  blind.fly = false;
  blind.pos = Vec3{140.5f, kStart, 140.5f};
  int heldFrames = 0;
  int frames = 0;
  for (; frames < 2000 && !blind.grounded; frames++) {
    // The snapshot delivered THIS frame was encoded kLatency frames ago.
    mirrorCentre = centreHist[frames % kLatency];
    centreHist[frames % kLatency] = blind.pos.y;
    blind.Update(dt, TickInput{}, fwd, right, fwd, staleMirror);
    if (blind.blindFall) heldFrames++;
  }
  const float blindFeet = blind.pos.y + blind.CurrentBox().yLo;
  const bool blindOk = blind.grounded && heldFrames > 0 &&
                       std::abs(blindFeet - (float)kFloor) < 1.0f;

  const bool ok = thinOk && fastEnough && blindOk;
  char buf[320];
  std::snprintf(buf, sizeof buf,
                "1-vox slab: grounded=%d feet %.2f (want %d) peak %.0f of "
                "terminal %.0f vox/s | %d-frame stale mirror: grounded=%d "
                "feet %.2f (want %d) held %d of %d frames",
                thin.grounded ? 1 : 0, thinFeet, kFloor + 1, thinPeak, terminal,
                kLatency, blind.grounded ? 1 : 0, blindFeet, kFloor, heldFrames,
                frames);
  detail = buf;
  std::printf("player fastfall: %s (%s)\n", ok ? "PASS" : "FAIL", buf);
  return ok ? Status::Pass : Status::Fail;
}

// ---- tick-input --------------------------------------------------------
//
// THE CONTROLLER IS A FUNCTION OF THE COMMAND STREAM, AND OF NOTHING ELSE.
//
// This is package N2's proof (docs/PLAN_multiplayer_now.md). Before it, the
// player integrated on the FRAME clock: Player::Update ran once per frame with
// a frame dt, exp/pow-smoothed everything it owned by that dt, and aged the
// coyote/buffer/hang timers by it — while every other gameplay system stepped
// at kTickDt inside the fixed-tick loop. Two players at 30 and 144 fps
// therefore walked different distances from the same keys, and no gate could
// see it because --selftest never runs the game loop.
//
// The claim, stated so it can fail: FEED THE SAME PER-TICK COMMANDS THROUGH A
// DIFFERENT FRAME SCHEDULE AND GET THE SAME TRAJECTORY TO THE BIT. Arm A runs
// one tick per frame; arm B runs four. Same 300 ticks, same commands, and the
// position/velocity/state stream must be bit-identical, not close.
//
// HOW THE SCRIPT IS WRITTEN, AND WHY IT IS WRITTEN THAT WAY. The frame layer's
// contract (sim/tickinput.h TickInputFeeder) is asymmetric on purpose: HELD
// state is a sample, broadcast to every tick of a multi-tick frame, while a
// pressed EDGE and the accumulated look delta go to exactly ONE tick. So a
// script that changed a held bit or pressed a key at an arbitrary tick index
// would deliver DIFFERENT commands under the two schedules and the comparison
// would be meaningless — it would be testing the script, not the controller.
// The script is therefore authored in BLOCKS of kTicksPerBlock ticks: held
// state and the camera basis change only on a block boundary, and every edge
// and every pixel of look is scheduled on the block's first tick. Under both
// schedules tick i then consumes an identical TickInput, which is checked
// directly (test 2) rather than assumed.
//
// WHAT IT DOES NOT TEST: ops. The plan asks for "identical op vectors per
// tick", but the controller in this fixture authors no ops at all — the ops a
// player produces come from the brush, the spells and the strike runner, none
// of which exist without a World, a GPU and an avatar. The COMMAND stream is
// the honest analogue and the one the network layer actually ships, so that is
// what is compared; `player-styles` and `swing` cover the strike side.
constexpr int kTickInputTicks = 300;
constexpr int kTicksPerBlock = 4;   // = the deep arm's ticks per frame

// One tick, as it came out of the controller. Compared with memcmp: "to the
// bit" is the claim, so a tolerance would silently pass the exact bug this
// gate exists to catch.
struct TickInputSample {
  Vec3 pos, vel;
  float viewYOffset;
  uint32_t flags;   // grounded | crouching<<1 | jumped<<2 | inLiquid<<3
};

// The scripted command for the block a tick belongs to. Held state and the
// basis are functions of the BLOCK; edges and look pixels are returned
// separately and are delivered only on the block's first tick.
TickInput TickInputScriptBlock(int block, uint32_t& edges, float& lookDx) {
  TickInput in;
  edges = 0;
  lookDx = 0.0f;
  const int firstTick = block * kTicksPerBlock;
  // WALK: forward from the third block, so the body has landed first.
  if (firstTick >= 8) in.forward = 1.0f;
  // STRAFE: a right-hand arc through the middle of the run.
  if (firstTick >= 120 && firstTick < 200) in.strafe = 1.0f;
  // SPRINT: held across a stretch, so the speed law changes mid-trajectory.
  in.SetHeld(TB_SPRINT, firstTick >= 40 && firstTick < 120);
  // CROUCH: held near the end, which also moves the collision box and the eye.
  in.SetHeld(TB_CROUCH, firstTick >= 240 && firstTick < 280);
  // JUMP: three presses, plus space HELD around each one (the ledge and swim
  // paths read the held bit, the buffer reads the edge).
  const bool jumpBlock = firstTick == 48 || firstTick == 96 || firstTick == 144;
  in.SetHeld(TB_JUMP, jumpBlock);
  if (jumpBlock) edges |= TB_JUMP;
  // STRIKE: one attack press, and a flick of mouse travel to pick its
  // direction with. Neither reaches the controller — they ride along to prove
  // the edge and the look delta are delivered to exactly one tick.
  if (firstTick == 200) {
    edges |= TB_ATTACK;
    lookDx = 240.0f;
  }
  // TURN: the camera basis yaws by a fixed amount per block, so the walk
  // direction is a different vector on almost every block and a controller
  // that had cached a basis would drift apart between the arms.
  const float yaw = (float)block * 0.02f;
  in.flatFwd = Vec3{std::cos(yaw), 0.0f, std::sin(yaw)};
  in.right = Vec3{-std::sin(yaw), 0.0f, std::cos(yaw)};
  in.lookFwd = in.flatFwd;
  return in;
}

// Run `kTickInputTicks` ticks with `ticksPerFrame` of them per frame, through
// the REAL frame-layer accumulator (TickInputFeeder), and record both what the
// controller did and what command each tick actually consumed.
void RunTickInputArm(int ticksPerFrame, std::vector<TickInputSample>& out,
                     std::vector<TickInput>& cmds, int& jumpEdgesSeen) {
  // Flat ground at y=100 with a step up at x>=150, so the run crosses a ledge
  // and exercises the step-up sweep and the view-smoothing decay too.
  auto kindAt = [](IVec3 c) {
    const int ground = c.x >= 150 ? 101 : 100;
    return c.y < ground ? CellKind::Solid : CellKind::Air;
  };
  Player p;
  p.fly = false;
  p.pos = Vec3{140.0f, 100.0f + Player::kHalfY + 2.0f, 140.0f};
  p.SnapRender();
  TickInputFeeder feeder;
  out.clear();
  cmds.clear();
  jumpEdgesSeen = 0;
  int tickIdx = 0;
  while (tickIdx < kTickInputTicks) {
    // ---- the FRAME half: one sample of the keyboard, then N ticks --------
    const int block = tickIdx / kTicksPerBlock;
    uint32_t edges = 0;
    float lookDx = 0.0f;
    const TickInput blk = TickInputScriptBlock(block, edges, lookDx);
    feeder.SetAxes(blk.forward, blk.strafe);
    feeder.SetHeldMask(blk.held);
    // Edges and look pixels only on the block's first tick — see the note
    // above on why the script is written in blocks.
    if (tickIdx % kTicksPerBlock == 0) {
      feeder.Press(edges);
      feeder.Look(lookDx, 0.0f);
    }
    for (int k = 0; k < ticksPerFrame && tickIdx < kTickInputTicks; k++) {
      const TickInput ti = feeder.Consume(blk.flatFwd, blk.right, blk.lookFwd);
      if (ti.Pressed(TB_JUMP)) jumpEdgesSeen++;
      cmds.push_back(ti);
      p.Update(kTickDt, ti, kindAt);
      TickInputSample s{};
      s.pos = p.pos;
      s.vel = p.vel;
      s.viewYOffset = p.viewYOffset;
      s.flags = (p.grounded ? 1u : 0u) | (p.crouching ? 2u : 0u) |
                (p.jumped ? 4u : 0u) | (p.inLiquid ? 8u : 0u);
      out.push_back(s);
      p.jumped = false;   // main.cpp drains it at the end of every tick
      tickIdx++;
    }
  }
}

Status GateTickInput(Ctx& c, std::string& detail) {
  (void)c;
  std::vector<TickInputSample> a, b;
  std::vector<TickInput> ca, cb;
  int jumpsA = 0, jumpsB = 0;
  RunTickInputArm(1, a, ca, jumpsA);
  RunTickInputArm(kTicksPerBlock, b, cb, jumpsB);

  // (1) THE COMMAND STREAM. If the two schedules did not deliver identical
  // commands, nothing below means anything — so this is checked first and
  // reported separately, and it is also the assertion on the feeder's own
  // contract (held broadcast, edge to exactly one tick).
  int cmdDiff = 0, firstCmdDiff = -1;
  for (size_t i = 0; i < ca.size() && i < cb.size(); i++) {
    if (std::memcmp(&ca[i], &cb[i], sizeof(TickInput)) == 0) continue;
    if (firstCmdDiff < 0) firstCmdDiff = (int)i;
    cmdDiff++;
  }
  const bool cmdOk = ca.size() == cb.size() &&
                     ca.size() == (size_t)kTickInputTicks && cmdDiff == 0;

  // (2) THE EDGE LAW: exactly one tick per scheduled press, under either
  // schedule. Zero would mean an edge was dropped, two would mean a tick batch
  // re-delivered it — the two failures the ad-hoc latches could not tell apart.
  const int jumpsScripted = 3;
  const bool edgeOk = jumpsA == jumpsScripted && jumpsB == jumpsScripted;

  // (3) THE TRAJECTORY, to the bit.
  int diff = 0, firstDiff = -1;
  float worst = 0.0f;
  for (size_t i = 0; i < a.size() && i < b.size(); i++) {
    if (std::memcmp(&a[i], &b[i], sizeof(TickInputSample)) == 0) continue;
    if (firstDiff < 0) firstDiff = (int)i;
    diff++;
    worst = std::max(worst, (a[i].pos - b[i].pos).len());
  }
  const bool trajOk = a.size() == b.size() && diff == 0;

  // (4) THE BODY ACTUALLY WENT SOMEWHERE. A controller that refused every
  // input would pass (1)-(3) perfectly, which is the "absolute zero is a rate
  // claim" trap: the arms agreeing is only interesting if there was motion.
  const Vec3 start{140.0f, 100.0f + Player::kHalfY + 2.0f, 140.0f};
  const Vec3 end = a.empty() ? start : a.back().pos;
  const float travelled = (Vec3{end.x, 0, end.z} - Vec3{start.x, 0, start.z}).len();
  const bool movedOk = travelled > 20.0f;

  // (5) THE PIN. The final position, recorded so --rebaseline can move it in
  // one step when a player.* tuning value legitimately changes the walk. A
  // pin that differs while (1)-(4) hold is a rebaselinable difference, not a
  // regression — MarkPinnedOnly is what says so.
  RecordObserved("tickInput.endX", (double)end.x);
  RecordObserved("tickInput.endY", (double)end.y);
  RecordObserved("tickInput.endZ", (double)end.z);
  const double pinX = BaselineNumber("tickInput.endX", (double)end.x);
  const double pinY = BaselineNumber("tickInput.endY", (double)end.y);
  const double pinZ = BaselineNumber("tickInput.endZ", (double)end.z);
  const double pinTol = BaselineNumber("tickInput.endTolVox", 0.05);
  const bool pinOk = std::abs(pinX - (double)end.x) <= pinTol &&
                     std::abs(pinY - (double)end.y) <= pinTol &&
                     std::abs(pinZ - (double)end.z) <= pinTol;

  const bool core = cmdOk && edgeOk && trajOk && movedOk;
  if (core && !pinOk) MarkPinnedOnly();
  const bool ok = core && pinOk;
  char buf[400];
  std::snprintf(buf, sizeof(buf),
                "%d ticks at 1/frame vs %d/frame: cmds differ %d (first %d), "
                "traj differ %d (first %d, worst %.6f vox), jump edges %d/%d "
                "(want %d), travelled %.2f vox, end (%.4f, %.4f, %.4f) pin "
                "(%.4f, %.4f, %.4f) tol %.3f",
                kTickInputTicks, kTicksPerBlock, cmdDiff, firstCmdDiff, diff,
                firstDiff, worst, jumpsA, jumpsB, jumpsScripted, travelled,
                end.x, end.y, end.z, pinX, pinY, pinZ, pinTol);
  detail = buf;
  std::printf("tick input: %s (%s)\n", ok ? "PASS" : "FAIL", buf);
  return ok ? Status::Pass : Status::Fail;
}


// ---- view-smooth -----------------------------------------------------------
//
// WALKING UP A HILL IS ONE CONTINUOUS MOTION, NOT THIRTY TELEPORTS A SECOND.
//
// The body climbs a ledge in ONE tick — `StepSlide` is an instantaneous
// vertical snap, by design — so both render corrections have to hide it:
//   * `viewYOffset`, which cancels the snap and decays over
//     player.viewSmoothHalflife, and
//   * the N2 tick interpolation, which rides prevPos -> pos across the 33 ms.
// They are not independent, and that is what this gate exists to pin. From
// 2026-09-10 (N2) to 2026-09-21 the two DOUBLE-COUNTED each other: the offset
// subtracted the whole step in one lump at the tick boundary while the lerp
// added it back linearly over the tick, so the eye dropped a full step height
// on the first frame of every tick and climbed back over the next 33 ms. At a
// 30 Hz tick and a voxel per step that is a 10 cm sawtooth at 30 Hz for the
// whole climb — visually indistinguishable from no smoothing at all, which is
// exactly how it was reported ("it snaps up one voxel at a time").
// `Player::BankVerticalSnap` moves prevPos with the offset so the lerp only
// ever covers the CONTINUOUS part of the tick's travel.
//
// THE MEASUREMENT IS PER FRAME, NOT PER TICK, because the artifact only exists
// between ticks: sampled once per tick the broken version and the fixed one
// return the same numbers. So the fixture runs the frame loop's real shape —
// N frames per tick, `alpha` sweeping 0..1 — and looks at the biggest
// single-frame change in the drawn eye height.
//
// THREE CLAIMS, each of which fails on its own:
//   (A) no frame moves the eye more than `viewSmooth.maxFrameJumpVox`. The
//       staircase's riser is one voxel, so a per-frame jump near 1.0 IS the
//       sawtooth and a well-smoothed climb is an order of magnitude under it.
//   (B) the eye never goes DOWN while the body is climbing. A dip is the
//       signature of the double-count specifically (the lump lands before the
//       lerp makes it back), and it is not caught by (A) alone.
//   (C) the BODY and the EYE move as one. The art is posed once per tick
//       around `pos` (Mob::SetRenderOffset / Player::RenderBodyOffset); if it
//       does not get the same two corrections, third person shows a figure
//       stair-stepping under a camera that glides. With no crouch in the
//       script the gap between them is a constant, so its VARIATION is the
//       error — measured in voxels, asserted at ~0.
constexpr int kViewSmoothTicks = 240;
constexpr int kViewSmoothFramesPerTick = 5;   // 150 fps against a 30 Hz tick
constexpr int kViewSmoothSettleTicks = 20;   // land first, then measure

Status GateViewSmooth(Ctx& c, std::string& detail) {
  (void)c;
  // A staircase: one voxel up every 4 voxels of +x, starting at x=150. Four
  // voxels is about one tick of walking, so the run crosses a riser every few
  // ticks and the climb is the steady state rather than a single event.
  auto kindAt = [](IVec3 cell) {
    int ground = 100;
    if (cell.x >= 150) ground = 100 + (cell.x - 150) / 4 + 1;
    return cell.y < ground ? CellKind::Solid : CellKind::Air;
  };
  Player p;
  p.fly = false;
  p.pos = Vec3{140.0f, 100.0f + Player::kHalfY + 2.0f, 140.0f};
  p.SnapRender();

  TickInput in;
  in.forward = 1.0f;
  in.flatFwd = Vec3{1, 0, 0};
  in.right = Vec3{0, 0, -1};
  in.lookFwd = in.flatFwd;

  float prevEye = 0.0f, prevGap = 0.0f;
  bool have = false;
  float maxJump = 0.0f, worstDip = 0.0f, maxGapDrift = 0.0f;
  // Attribution, not a bare count (CLAUDE.md rule 6): WHEN the worst frame
  // happened and what the body was doing on it. A number with no cause
  // attached costs a diagnosis run; these cost nothing.
  int jumpAt[2] = {-1, -1}, dipAt[2] = {-1, -1};
  float jumpWhy[5] = {0, 0, 0, 0, 0}, dipWhy[5] = {0, 0, 0, 0, 0};
  float climbed = 0.0f;
  float startY = p.pos.y;
  for (int t = 0; t < kViewSmoothTicks; t++) {
    p.Update(kTickDt, in, kindAt);
    // The fixture spawns two voxels up so the body lands on its own feet
    // rather than starting interpenetrating; those ticks are a FALL, which is
    // fast continuous motion and would set the worst-frame numbers to
    // something that has nothing to do with stepping. (Measured before this
    // skip existed: the worst 'dip' was tick 4 at vel.y = -16.4 vox/s, i.e.
    // gravity working correctly.) The claim is about a WALK, so the walk is
    // what is sampled.
    if (t < kViewSmoothSettleTicks) {
      have = false;
      startY = p.pos.y;   // the climb is measured from where the walk begins
      continue;
    }
    for (int f = 0; f < kViewSmoothFramesPerTick; f++) {
      const float alpha = (float)f / (float)kViewSmoothFramesPerTick;
      const float eyeY = p.RenderEyePos(alpha).y;
      // Where the ART is drawn, in the same units: the avatar is posed around
      // `pos` and translated by RenderBodyOffset (game/mob.cpp AppendXforms).
      const float bodyY = p.pos.y + p.RenderBodyOffset(alpha).y;
      const float gap = eyeY - bodyY;
      if (have) {
        const float d = eyeY - prevEye;
        if (std::abs(d) > maxJump) {
          maxJump = std::abs(d);
          jumpAt[0] = t; jumpAt[1] = f;
          jumpWhy[0] = p.pos.y; jumpWhy[1] = p.prevPos.y;
          jumpWhy[2] = p.viewYOffset; jumpWhy[3] = p.grounded ? 1.f : 0.f;
          jumpWhy[4] = p.vel.y;
        }
        if (d < worstDip) {
          worstDip = d;
          dipAt[0] = t; dipAt[1] = f;
          dipWhy[0] = p.pos.y; dipWhy[1] = p.prevPos.y;
          dipWhy[2] = p.viewYOffset; dipWhy[3] = p.grounded ? 1.f : 0.f;
          dipWhy[4] = p.vel.y;
        }
        maxGapDrift = std::max(maxGapDrift, std::abs(gap - prevGap));
      }
      prevEye = eyeY;
      prevGap = gap;
      have = true;
    }
  }
  climbed = p.pos.y - startY;

  // (0) THE BODY ACTUALLY CLIMBED. Every bound below is satisfied perfectly by
  // a player that never left the flat, which is the "absolute zero is a rate
  // claim" trap — so the climb is asserted first and the rest means nothing
  // without it. The staircase rises well past 10 voxels over the run.
  const bool climbedOk = climbed > 8.0f;

  const double maxJumpAllow = BaselineNumber("viewSmooth.maxFrameJumpVox", 0.25);
  const double maxDipAllow = BaselineNumber("viewSmooth.maxFrameDipVox", 0.03);
  const double maxGapAllow = BaselineNumber("viewSmooth.maxGapDriftVox", 0.001);
  RecordObserved("viewSmooth.maxFrameJumpVox", (double)maxJump);
  RecordObserved("viewSmooth.maxFrameDipVox", (double)-worstDip);
  RecordObserved("viewSmooth.maxGapDriftVox", (double)maxGapDrift);

  const bool jumpOk = maxJump <= (float)maxJumpAllow;
  const bool dipOk = -worstDip <= (float)maxDipAllow;
  const bool gapOk = maxGapDrift <= (float)maxGapAllow;
  const bool ok = climbedOk && jumpOk && dipOk && gapOk;

  char buf[360];
  std::snprintf(buf, sizeof(buf),
                "%d ticks x %d frames up a 1-in-4 staircase: climbed %.2f vox "
                "(want >8), max frame jump %.4f vox (allow %.4f)%s, worst frame "
                "dip %.4f vox (allow %.4f)%s, eye-vs-body drift %.6f vox "
                "(allow %.6f)%s",
                kViewSmoothTicks, kViewSmoothFramesPerTick, climbed, maxJump,
                maxJumpAllow, jumpOk ? "" : " FAIL", -worstDip, maxDipAllow,
                dipOk ? "" : " FAIL", maxGapDrift, maxGapAllow,
                gapOk ? "" : " FAIL");
  if (!jumpOk || !dipOk)
    std::printf("view smooth: worst jump at tick %d frame %d "
                "(pos.y %.4f prev.y %.4f off %.4f ground %.0f vel.y %.3f); "
                "worst dip at tick %d frame %d "
                "(pos.y %.4f prev.y %.4f off %.4f ground %.0f vel.y %.3f)\n",
                jumpAt[0], jumpAt[1], jumpWhy[0], jumpWhy[1], jumpWhy[2],
                jumpWhy[3], jumpWhy[4], dipAt[0], dipAt[1], dipWhy[0],
                dipWhy[1], dipWhy[2], dipWhy[3], dipWhy[4]);
  detail = buf;
  std::printf("view smooth: %s (%s)\n", ok ? "PASS" : "FAIL", buf);
  return ok ? Status::Pass : Status::Fail;
}


// ---- two-players -----------------------------------------------------------
//
// TWO INDEPENDENT BODIES IN ONE WORLD, AND ONLY ONE OF THEM HAS A MIRROR.
//
// This is M9.1 package P3's proof (docs/PLAN_multiplayer_m9.md §2). What made a
// second player impossible was not the tick — package P1 split that into
// per-player phases — it was the COLLISION SOURCE. `World::KindAt` reads the
// snapshot's 3x3x3 chunk mirror, and that cube is centred on ONE player's chunk
// (phase H submits with session 0's). A second body twelve chunks away reads
// Unknown for every cell; `Collides` treats Unknown as air on purpose and
// `KnownDrop` clamps the descent to the last row the mirror vouches for, so
// such a body does not fall through the world — IT HOVERS, indefinitely,
// wherever it left the cube. (The engine map said Unknown cells are SOLID for
// the player. They are not: the clamp is upstream of the sweep, not inside it.)
//
// So player 0 walks on `KindAt` with the mirror centred on it — the game's
// arrangement, unchanged — and player 1 walks on `World::KindAtCached` with
// `PrefetchChunksAround` keeping its chunk cache warm. Those two functions
// are all of P3's engine change, and this is what asserts them. The claims, in
// the order they are checked:
//
//   1. BOTH trajectories are pinned. A second player that hovers, sinks, stalls
//      or teleports moves a pin.
//   2. Player 1 never drops below its own analytic ground, checked EVERY TICK.
//      At the end only, a body that fell and was caught later reads fine.
//   3. The cache actually takes over. Player 1's answers are counted per tick
//      and the analytic fallback has to stop being used and stay stopped — a
//      `KindAtCached` that never read a fetched voxel would satisfy claims 1
//      and 2 perfectly, which is the "fixture that cannot fail" trap.
//   4. A hostile creature targets the NEARER of the two players BY ITS BANDED
//      ACTOR ID (`ai::kPlayerActorBase + i`, package P2), and
//      `MobSystem::FindCombatantById` resolves that id to the right avatar.
//      With one player both of those are vacuous; with two they are the point.
//   5. The twice-run contract: the whole 300 ticks run twice with a fresh
//      worldgen between, and the world-hash sequences must agree. Determinism
//      is the invariant (CLAUDE.md rule 1) and a second player is new input.
//
// FIXTURE PLACEMENT PINS THE WINDOW ITSELF, the way `pond-shore` does.
//
// The obvious thing — anchor to `world.WindowOrigin()` like the AI gates —
// makes the end positions a function of WHICH GATES RAN FIRST: `streaming`
// leaves the origin ~20 chunks out and `pond-shore` stands it somewhere else
// entirely, so a pin measured under `--gate two-players` is a different number
// from the same pin measured under `--selftest`. That is CLAUDE.md rule 7
// ("a `--gate X` subset is not a small `--selftest`") arriving as a pinned
// value instead of as a verdict, and it costs a wrong conclusion either way
// round. So the gate SETS the origin to (0,0,0), regenerates, and restores the
// caller's origin and pristine worldgen before it returns — the fixture is the
// same world at every scope.
//
// Within that window both sites then move to the flattest patch nearby, because
// the kill criterion for this package is "if player 1 falls through EVEN WITH
// the analytic fallback, MOVE THE SITE" — an overhang, a cave roof or a ruin
// floor is a column `TerrainHeight` cannot describe, and widening the tolerance
// would hide exactly that.
//
// THE TWO BODIES WALK APART, NOT TOGETHER. The package text says "player 0
// walks +X, player 1 walks −X"; player 0 is therefore the +X site and player 1
// the −X one, so those headings INCREASE the separation. Placed the other way
// round the two would converge — at ~30 Hz over 300 ticks a walking body covers
// far more than the 192 voxels between them — and player 1 would spend the back
// half of the run inside player 0's mirror, which is the one condition the gate
// exists to avoid. Walking is also confined to ticks 8..128 so neither body
// leaves the residency window; the rest of the run is standing and one jump.
constexpr int kTwoPlayerTicks = 300;
constexpr int kTwoPlayerChunkGap = 12;   // chunks between the bodies, in X
constexpr int kTwoPlayerWalkEnd = 128;   // last tick with forward held
constexpr int kTwoPlayerJumpTick = 160;  // the one jump, while standing
// The window this fixture owns. (0,0,0) is where most of the suite sits, which
// makes world and window-local chunk coordinates coincide and removes the
// SlotChunkIndex trap `pond-shore` documents.
constexpr IVec3 kTwoPlayerOrigin{0, 0, 0};

// The flattest patch within `search` voxels of (cx,cz). Same shape as the AI
// gates' AiFlatSpot (selftest_mob.cpp) and for the same reason: terrain is
// procedural, so a hand-picked coordinate becomes a cliff the next time
// worldgen is tuned and the failure reads as "the player broke". Restated here
// rather than shared because the two live in different TUs' anonymous
// namespaces; a third copy should move it to selftest.h.
IVec3 TwoPlayerFlatSpot(int cx, int cz, int search, uint32_t seed,
                        int& outRelief) {
  int bestX = cx, bestZ = cz, bestRelief = INT32_MAX;
  for (int oz = -search; oz <= search; oz += 8)
    for (int ox = -search; ox <= search; ox += 8) {
      int lo = INT32_MAX, hi = INT32_MIN;
      for (int dz = -8; dz <= 8; dz += 4)
        for (int dx = -8; dx <= 8; dx += 4) {
          const int h = World::TerrainHeight(cx + ox + dx, cz + oz + dz, seed);
          lo = std::min(lo, h);
          hi = std::max(hi, h);
        }
      if (hi - lo < bestRelief) {
        bestRelief = hi - lo;
        bestX = cx + ox;
        bestZ = cz + oz;
      }
    }
  outRelief = bestRelief;
  return IVec3{bestX, World::TerrainHeight(bestX, bestZ, seed), bestZ};
}

// The scripted command for one tick, in BLOCKS of kTicksPerBlock — the same
// discipline `tick-input` uses above and for the same reason: held state is a
// sample and an edge belongs to exactly one tick, so anything that changes
// mid-block is testing the script rather than the controller. `dir` is +1 for
// the body walking +X and -1 for the one walking -X.
TickInput TwoPlayerScript(int tick, int dir) {
  TickInput in;
  const int firstTick = (tick / kTicksPerBlock) * kTicksPerBlock;
  if (firstTick >= 8 && firstTick < kTwoPlayerWalkEnd) in.forward = 1.0f;
  const bool jumpBlock = firstTick == kTwoPlayerJumpTick;
  in.SetHeld(TB_JUMP, jumpBlock);
  if (jumpBlock && tick == firstTick) in.SetPressed(TB_JUMP, true);
  in.flatFwd = Vec3{(float)dir, 0, 0};
  in.right = Vec3{0, 0, (float)dir};
  in.lookFwd = in.flatFwd;
  return in;
}

// What one arm of the gate measured. Two arms prove the twice-run contract;
// arm 0 is what the pins are read against.
struct TwoPlayerArm {
  Vec3 start0{}, start1{}, end0{}, end1{};
  std::vector<uint32_t> hashes;
  int worstSinkTick = -1;        // first tick player 1 was below its ground
  float worstSink = 0.0f;        // how far below, in voxels
  int lastFallbackTick = -1;     // last tick answered from TerrainHeight
  uint64_t fallbacks = 0;        // analytic answers for player 1, all ticks
  uint64_t cacheHits = 0;        // answers that read a fetched voxel
  uint64_t mobId = 0;
  uint64_t mobTarget = 0;
  bool mobBrain = false;
  bool combatantOk = false;      // FindCombatantById(base+1) == &avatar 1
  float mobToP0 = 0, mobToP1 = 0;
  uint64_t fetchReq = 0, fetchCoalesced = 0;
  std::string why;               // non-empty = the arm could not be built
};

// Run the whole 300-tick fixture once, from a fresh worldgen.
void RunTwoPlayerArm(Ctx& c, TwoPlayerArm& a) {
  c.debris.Reset();
  // rewindIds: mob ids seed gore variance and blast noise, so the second arm
  // must start its id counter where the first did or the two runs are not the
  // same experiment.
  c.mobs.Reset(true);
  c.mobs.ClearRisings();
  // THE WINDOW IS PART OF THE FIXTURE (see the note above). Set before the
  // worldgen, because SubmitWorldgen generates the window that is there.
  c.world.SetWindowOrigin(kTwoPlayerOrigin);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  c.world.ResetFetchProbe();

  // The COLLISION table the game builds, not a copy of the material classes:
  // passable vegetation reads as gas and a gate that forgot it would pass
  // against collision the player does not have (sim/materials.h).
  const std::vector<uint32_t> classOf = BuildCollisionClasses(c.mats);

  const int cx = (kTwoPlayerOrigin.x + (int)kNChunk / 2) * (int)kChunk;
  const int cz = (kTwoPlayerOrigin.z + (int)kNChunk / 2) * (int)kChunk;
  const int half = kTwoPlayerChunkGap * (int)kChunk / 2;  // 96 voxels
  int relief0 = 0, relief1 = 0;
  const IVec3 site0 =
      TwoPlayerFlatSpot(cx + half, cz, 24, kDefaultSeed, relief0);
  const IVec3 site1 =
      TwoPlayerFlatSpot(cx - half, cz, 24, kDefaultSeed, relief1);

  Player p0, p1;
  p0.fly = false;
  p1.fly = false;
  // Feet exactly one voxel above the ground column: pos.y − kHalfY is the sole
  // (the `player-walk` gate uses the same arithmetic), and TerrainHeight names
  // the topmost GROUND voxel, so h + 1 is the first free row.
  p0.pos = Vec3{(float)site0.x + 0.5f, (float)(site0.y + 1) + Player::kHalfY,
                (float)site0.z + 0.5f};
  p1.pos = Vec3{(float)site1.x + 0.5f, (float)(site1.y + 1) + Player::kHalfY,
                (float)site1.z + 0.5f};
  p0.SnapRender();
  p1.SnapRender();
  a.start0 = p0.pos;
  a.start1 = p1.pos;

  // ---- the two collision sources, and the whole difference between them ----
  auto kind0 = [&](IVec3 cell) { return c.world.KindAt(cell, classOf); };
  // Player 1's, WITH A PROBE AROUND IT. `KindAtCached` does not report which
  // arm it took, and "58 page faults" is the shape of report CLAUDE.md rule 6
  // forbids: the gate therefore reproduces the function's own hit test here and
  // counts the two arms, so a failure says "still answering from TerrainHeight
  // at tick 240" rather than "player 1 is in the wrong place".
  uint64_t hits = 0, misses = 0;
  auto kind1 = [&](IVec3 cell) {
    const IVec3 wc{cell.x >> 4, cell.y >> 4, cell.z >> 4};
    if (c.world.ChunkInWindow(wc)) {
      const CachedChunk* cc = c.world.Cached(wc);
      if (cc != nullptr && cc->voxels.size() == kChunkVol)
        hits++;
      else
        misses++;
    }
    return c.world.KindAtCached(cell, classOf);
  };

  // ---- two avatars with DISTINCT mob ids ----------------------------------
  // 0x5A11ED is baked into every pinned world hash and every gate's expected
  // gore spread, so session 0 keeps it exactly; session i is + i (P2).
  PlayerAvatar av0(0x5A11EDU), av1(0x5A11EDU + 1);
  av0.Init(&c.phys, &c.world, &c.debris, c.mats, &c.mobs);
  av1.Init(&c.phys, &c.world, &c.debris, c.mats, &c.mobs);
  av0.SetDefs(&c.mobs.Defs(), kAvatarDefName);
  av1.SetDefs(&c.mobs.Defs(), kAvatarDefName);
  if (!av0.HasDef() || !av1.HasDef()) {
    a.why = std::string("no mob def named \"") + kAvatarDefName + "\"";
    return;
  }
  av0.Spawn(p0, 0.0f);
  av1.Spawn(p1, 3.14159265f);
  Mob* avatars[2] = {&av0, &av1};
  c.mobs.SetAvatars(std::span<Mob* const>(avatars, 2));

  // Two kinematic proxies. `Physics::playerBodies_` is a LIST since P2; with
  // one entry it was bit-identical to the single handle it replaced, and this
  // is the first fixture that puts two in it.
  const uint64_t pb0 = c.phys.CreatePlayerBody(Player::kHalfXZ, Player::kHalfY);
  const uint64_t pb1 = c.phys.CreatePlayerBody(Player::kHalfXZ, Player::kHalfY);

  // ---- one hostile creature, and it stands next to PLAYER ONE -------------
  // "crowder" is mobile and hostile and WANTS a target, with no attack intent
  // and no styles: exactly a targeting fixture, with none of the knife-fight
  // noise an armed duelist brings (its profile comment in behaviors.json says
  // so in as many words). Spawned beside player 1 so "the nearer actor" has an
  // unambiguous answer — the other player is 192 voxels away.
  int defIndex = -1;
  for (size_t i = 0; i < c.mobs.Defs().size(); i++) {
    if (c.mobs.Defs()[i].FindSocket("held_right") < 0) continue;
    if (defIndex < 0 || c.mobs.Defs()[i].name == "human") defIndex = (int)i;
  }
  if (defIndex < 0) {
    a.why = "no mob def publishes a held_right socket";
    return;
  }
  const int mx = site1.x + 14, mz = site1.z;
  a.mobId = c.mobs.Spawn(defIndex,
                         {mx, World::TerrainHeight(mx, mz, kDefaultSeed) + 1, mz});
  if (a.mobId == 0) {
    a.why = "Spawn refused for the hostile fixture";
    return;
  }
  if (!c.mobs.SetMobBehavior(a.mobId, "crowder")) {
    a.why = "no behaviour profile \"crowder\" in assets/mobs/behaviors.json";
    return;
  }

  uint32_t cursor1 = 0;   // player 1's prefetch round-robin (PlayerSession's)
  uint32_t tick = 9000;
  for (int i = 0; i < kTwoPlayerTicks; i++) {
    // ---- PHASE A, per player. The prefetch is the one line P3 adds to the
    // real tick, and it is gated there on `index > 0` — player 0 does not get
    // one because the mirror follows it.
    PrefetchChunksAround(c.world, p1.pos, cursor1);
    const uint64_t hits0 = hits, misses0 = misses;
    p0.Update(kTickDt, TwoPlayerScript(i, +1), kind0);
    p1.Update(kTickDt, TwoPlayerScript(i, -1), kind1);
    if (misses > misses0) a.lastFallbackTick = i;
    (void)hits0;

    // ---- PHASE H (world): the actor list, then the NPCs. Order copied from
    // session.cpp — SetPlayerActors publishes the positions mobs.PreTick then
    // perceives, and an avatar posed before that would be perceived a tick
    // late.
    const MobSystem::PlayerActorDesc actors[2] = {
        {Vec3{p0.pos.x, p0.pos.y, p0.pos.z}, Player::kHalfXZ,
         2.0f * Player::kHalfY, true},
        {Vec3{p1.pos.x, p1.pos.y, p1.pos.z}, Player::kHalfXZ,
         2.0f * Player::kHalfY, true},
    };
    c.mobs.SetPlayerActors(std::span<const MobSystem::PlayerActorDesc>(actors, 2));

    std::vector<BrushOp> ops;
    std::vector<CellOp> cellOps;
    std::vector<ParticleSpawn> spawns;
    c.mobs.PreTick(tick + 1, c.world, ops, cellOps, spawns);
    // ---- PHASE I, per player.
    av0.PreTick(tick + 1, p0, 0.0f, kTickDt, c.world, ops, cellOps, spawns);
    av1.PreTick(tick + 1, p1, 3.14159265f, kTickDt, c.world, ops, cellOps,
                spawns);
    // ---- PHASE J (world).
    c.debris.QueueSupportEvents(c.world.Snap());
    c.debris.PreTick(tick + 1, c.world, cellOps, spawns);

    ++tick;
    c.phys.MovePlayerBody(pb0, p0.pos, kTickDt);
    c.phys.MovePlayerBody(pb1, p1.pos, kTickDt);
    // The mirror is centred on PLAYER 0 — that is the whole asymmetry under
    // test, and it is the same `playerChunk` phase H hands SubmitTick in the
    // game. Arithmetic shift, not divide: a window streamed to negative world
    // coordinates makes `/ 16` round toward zero and name the wrong chunk.
    const IVec3 pc{ifloor(p0.pos.x) >> 4, ifloor(p0.pos.y) >> 4,
                   ifloor(p0.pos.z) >> 4};
    // The hash is read six times per arm rather than 300: it is a blocking
    // readback, and a divergence does not heal, so a sparse sequence catches
    // the same failure for a fiftieth of the stalls (CLAUDE.md's budget).
    const bool wantHash = (i % 60) == 59 || i == kTwoPlayerTicks - 1;
    SubmitTick(c.ctx, c.world, c.sim, tick, kDefaultSeed, ops, {}, cellOps,
               wantHash, pc, true, false, spawns);
    c.ctx.WaitIdle();
    c.ctx.ProcessEvents();
    if (wantHash) a.hashes.push_back(ReadHashSync(c.ctx, c.world));
    c.phys.Step(kTickDt);
    c.debris.PostStep();
    c.mobs.PostStep();
    av0.PostStep();
    av1.PostStep();

    // ---- THE FALL-THROUGH ASSERTION, every tick. A body caught on the way
    // down reads perfectly at tick 300, so the worst moment is what is kept.
    // The bar is player 1's OWN analytic ground minus one voxel: below that it
    // is inside terrain the fallback claims is solid, which is the failure the
    // whole package exists to prevent.
    const int gh = World::TerrainHeight(ifloor(p1.pos.x), ifloor(p1.pos.z),
                                        c.world.WorldSeed());
    const float sole = p1.pos.y - Player::kHalfY;
    const float sink = (float)(gh - 1) - sole;
    if (sink > 0.0f && sink > a.worstSink) {
      a.worstSink = sink;
      if (a.worstSinkTick < 0) a.worstSinkTick = i;
    }
  }

  a.end0 = p0.pos;
  a.end1 = p1.pos;
  a.fallbacks = misses;
  a.cacheHits = hits;

  const ai::Brain* br = c.mobs.MobBrain(a.mobId);
  a.mobBrain = br != nullptr;
  if (br != nullptr) a.mobTarget = br->hasTarget ? br->targetId : 0;
  // The band resolves to the RIGHT avatar. Checked separately from the target
  // id: "the mob named player 1" and "player 1's id resolves to player 1's
  // body" are two claims, and P2's band only works if both hold.
  a.combatantOk =
      c.mobs.FindCombatantById(ai::kPlayerActorBase + 1) == (Mob*)&av1 &&
      c.mobs.FindCombatantById(ai::kPlayerActorBase + 0) == (Mob*)&av0;
  const Vec3 mo = c.mobs.MobOrigin(a.mobId);
  a.mobToP0 = std::sqrt((mo.x - a.end0.x) * (mo.x - a.end0.x) +
                        (mo.z - a.end0.z) * (mo.z - a.end0.z));
  a.mobToP1 = std::sqrt((mo.x - a.end1.x) * (mo.x - a.end1.x) +
                        (mo.z - a.end1.z) * (mo.z - a.end1.z));

  const World::FetchProbe& fp = c.world.Fetches();
  for (int i = 0; i < World::FetchProbe::kSources; i++) {
    a.fetchReq += fp.requests[i];
    a.fetchCoalesced += fp.coalesced[i];
  }

  // Leave nothing behind: the avatars own limb bodies, the proxies are Jolt
  // bodies, and the mob list is shared with every later gate.
  av0.Despawn();
  av1.Despawn();
  c.mobs.SetAvatars({});
  c.mobs.ClearPlayerActors();
  c.phys.RemoveBody(pb0);
  c.phys.RemoveBody(pb1);
  c.debris.Reset();
  c.mobs.Reset(true);
  c.mobs.ClearRisings();
}

Status GateTwoPlayers(Ctx& c, std::string& detail) {
  const IVec3 savedOrigin = c.world.WindowOrigin();
  TwoPlayerArm a, b;
  RunTwoPlayerArm(c, a);
  if (a.why.empty()) RunTwoPlayerArm(c, b);
  // Put the window back where the caller had it and regenerate: this gate moved
  // the origin and regenerated the world twice, and every gate after it in
  // kOrder assumes worldgen it did not move (selftest.h's ordering note).
  c.world.SetWindowOrigin(savedOrigin);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  if (!a.why.empty()) {
    detail = a.why;
    std::printf("two players: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }

  // (1) BOTH BODIES WENT SOMEWHERE. Stated first because every assertion below
  // is vacuously true of two players that never moved — "absolute zero is a
  // rate claim".
  const float moved0 =
      (Vec3{a.end0.x, 0, a.end0.z} - Vec3{a.start0.x, 0, a.start0.z}).len();
  const float moved1 =
      (Vec3{a.end1.x, 0, a.end1.z} - Vec3{a.start1.x, 0, a.start1.z}).len();
  const bool movedOk = moved0 > 10.0f && moved1 > 10.0f;

  // (2) NO FALL-THROUGH, at any tick.
  const bool sinkOk = a.worstSinkTick < 0;

  // (3) THE CACHE TOOK OVER, and stayed taken over. `lastFallbackTick` is the
  // last tick that answered from TerrainHeight; it must be early (the fetch is
  // 1-2 ticks plus kSnapshotLatency, so a handful of ticks is expected) and
  // there must be real cache reads to have taken over WITH.
  const int settleBy = (int)BaselineNumber("twoPlayers.fallbackSettledBy", 60);
  const bool cacheOk =
      a.cacheHits > 0 && a.lastFallbackTick >= 0 && a.lastFallbackTick < settleBy;

  // (4) THE BAND. The creature stands beside player 1 and names player 1, and
  // both banded ids resolve to the right avatar.
  const bool nearer = a.mobToP1 < a.mobToP0;
  const bool targetOk = a.mobBrain && a.combatantOk && nearer &&
                        a.mobTarget == ai::kPlayerActorBase + 1;

  // (5) THE TWICE-RUN CONTRACT. Not a pin — a reproduction. Both arms ran the
  // same 300 ticks from the same worldgen and must agree bit for bit; this is
  // the only claim in the file whose failure is a stop-and-report bug rather
  // than a rebaselinable number.
  const bool detOk = !a.hashes.empty() && a.hashes == b.hashes &&
                     std::abs(a.end1.x - b.end1.x) == 0.0f &&
                     std::abs(a.end0.x - b.end0.x) == 0.0f;

  // (6) THE PINS.
  RecordObserved("twoPlayers.p0EndX", (double)a.end0.x);
  RecordObserved("twoPlayers.p0EndY", (double)a.end0.y);
  RecordObserved("twoPlayers.p0EndZ", (double)a.end0.z);
  RecordObserved("twoPlayers.p1EndX", (double)a.end1.x);
  RecordObserved("twoPlayers.p1EndY", (double)a.end1.y);
  RecordObserved("twoPlayers.p1EndZ", (double)a.end1.z);
  // Informational, recorded so a future failure can say which way the fixture
  // moved rather than only that it did (CLAUDE.md rule 6).
  RecordObserved("twoPlayers.lastFallbackTick", (double)a.lastFallbackTick);
  RecordObserved("twoPlayers.fallbackAnswers", (double)a.fallbacks);
  RecordObserved("twoPlayers.cacheAnswers", (double)a.cacheHits);
  RecordObserved("twoPlayers.fetchRequests", (double)a.fetchReq);
  RecordObserved("twoPlayers.fetchCoalesced", (double)a.fetchCoalesced);
  const double tol = BaselineNumber("twoPlayers.endTolVox", 0.05);
  const double pin[6] = {BaselineNumber("twoPlayers.p0EndX", (double)a.end0.x),
                         BaselineNumber("twoPlayers.p0EndY", (double)a.end0.y),
                         BaselineNumber("twoPlayers.p0EndZ", (double)a.end0.z),
                         BaselineNumber("twoPlayers.p1EndX", (double)a.end1.x),
                         BaselineNumber("twoPlayers.p1EndY", (double)a.end1.y),
                         BaselineNumber("twoPlayers.p1EndZ", (double)a.end1.z)};
  const double got[6] = {a.end0.x, a.end0.y, a.end0.z,
                         a.end1.x, a.end1.y, a.end1.z};
  bool pinOk = true;
  for (int i = 0; i < 6; i++)
    if (std::abs(pin[i] - got[i]) > tol) pinOk = false;

  const bool core = movedOk && sinkOk && cacheOk && targetOk && detOk;
  if (core && !pinOk) MarkPinnedOnly();
  const bool ok = core && pinOk;
  char buf[720];
  std::snprintf(
      buf, sizeof buf,
      "%d ticks x2, %d chunks apart: p0 (%.3f, %.3f, %.3f) moved %.1f | p1 "
      "(%.3f, %.3f, %.3f) moved %.1f | pin tol %.3f %s | sink worst %.2f vox "
      "at tick %d | p1 answers %llu cached / %llu analytic, last analytic tick "
      "%d (want < %d) | fetches %llu req %llu coalesced | mob %llu targets "
      "%llx (want %llx), d(p1)=%.1f d(p0)=%.1f, band resolves %d | hashes "
      "%zu %s",
      kTwoPlayerTicks, kTwoPlayerChunkGap, a.end0.x, a.end0.y, a.end0.z, moved0,
      a.end1.x, a.end1.y, a.end1.z, moved1, tol, pinOk ? "ok" : "MOVED",
      a.worstSink, a.worstSinkTick, (unsigned long long)a.cacheHits,
      (unsigned long long)a.fallbacks, a.lastFallbackTick, settleBy,
      (unsigned long long)a.fetchReq, (unsigned long long)a.fetchCoalesced,
      (unsigned long long)a.mobId, (unsigned long long)a.mobTarget,
      (unsigned long long)(ai::kPlayerActorBase + 1), a.mobToP1, a.mobToP0,
      a.combatantOk ? 1 : 0, a.hashes.size(),
      detOk ? "reproduced" : "DIVERGED between the two runs");
  detail = buf;
  std::printf("two players: %s (%s)\n", ok ? "PASS" : "FAIL", buf);
  return ok ? Status::Pass : Status::Fail;
}

// ---- remote-ghost -----------------------------------------------------------
//
// A PEER'S BODY, DRIVEN FROM ITS OUTCOME, AUTHORING NOTHING.
//
// This is M9.2 package B's proof (docs/PLAN_multiplayer_m9.md). `two-players`
// above asserted that two LOCAL bodies can share one world; this asserts the
// other shape — a body whose controller ran on another machine, which arrives
// here as a `PlayerState` and is never re-simulated. The claims, in order:
//
//   1. A ghost fed a state stream SPAWNS, and does so on the first state
//      (by tick 2, since the stream starts at tick 0).
//   2. IT GOES WHERE IT IS TOLD, every tick it is alive: the rig's root,
//      reconstructed from `Mob::Origin()` and the def's box, is within half a
//      voxel of the state's `pos`. A ghost that lagged, drifted or hovered
//      moves this number, and the WORST over all ticks is kept rather than an
//      end-of-run sample — a body that drifted and was corrected later reads
//      perfectly at tick 300.
//   3. THE WIRE OWNS ITS LIFE. `alive=false` in the stream despawns it and
//      `alive=true` brings it back. Local damage does not decide this.
//   4. IT IS A TARGET, by its BANDED actor id. The band is positional
//      (`ai::kPlayerActorBase + i` is entry i of the actor list), so a ghost
//      appended after one session is `+ 1`; a hostile creature standing beside
//      it must name that id, and `FindCombatantById` must resolve the id to
//      the GHOST's avatar and not to the local player's.
//   5. IT AUTHORS NOTHING. Two claims, and both are needed: the ghost's avatar
//      DID try to author (the throw-away vectors were non-empty at least once
//      — without that half the next line is a fixture that cannot fail), and
//      NONE of it reached the real batch (the real op vectors do not grow
//      across `RemotePlayersPreTick`, checked every tick).
//   6. The twice-run contract, as `two-players` states it.
//
// EVERY ONE OF THOSE RUNS THROUGH THE FREE FUNCTIONS session.cpp's PHASES CALL
// (game/remoteplayer.h). The gate does not reimplement the per-ghost sequence:
// a fixture carrying its own copy would keep passing while the real phase
// drifted out from under it, which is the "fixture that measures itself" trap.
//
// THE WINDOW IS PART OF THE FIXTURE, for the reason `two-players` spells out
// at length: anchoring to `world.WindowOrigin()` makes every pinned number a
// function of which gates ran first, and that is CLAUDE.md rule 7 arriving as
// a pinned value instead of as a verdict. It reuses `kTwoPlayerOrigin` and
// `TwoPlayerFlatSpot` above — same TU, same anonymous namespace, so this is
// one definition and not the third copy that note warns about.
constexpr int kGhostTicks = 300;
constexpr int kGhostWalkStart = 8;     // first tick the ghost is walking
constexpr int kGhostWalkEnd = 88;      // last; 80 ticks x 2 vox = 160 voxels
constexpr int kGhostStepVox = 2;       // the stream's walk speed, vox/tick
constexpr int kGhostJumpTick = 60;     // the one jump edge
constexpr int kGhostCrouchFrom = 100;
constexpr int kGhostCrouchTo = 140;
// ONE HARD LANDING, AND IT IS NOT DECORATION. A healthy walking avatar
// authors NOTHING — footfalls are events, not ops, and bleeding needs a
// wound — so without this the gate's claim 5 has no first half and the
// "leaked 0" half becomes a fixture that cannot fail. Measured: 300 ticks of
// a walking, jumping, crouching ghost produced 0 ops. `impactDeltaV` is the
// wire's one-tick edge for exactly this, so the script delivers one landing
// and the avatar's fall-damage path wounds it and bleeds for the rest of the
// run. 120 vox/s at kVoxelMeters = 0.10 is 12 m/s: over player.fallDamageSpeed
// (8) so it wounds, well under player.fallSplatSpeed (25) so it does not kill
// — killing it here would make claim 3 ("the wire owns its life") a lie.
constexpr int kGhostLandTick = 150;
constexpr float kGhostLandDeltaV = 120.0f;  // voxels/s, downward
constexpr int kGhostDieTick = 200;     // alive=false from here...
constexpr int kGhostReviveTick = 240;  // ...alive again from here
// Read the creature's target BEFORE the scripted death: a despawned actor is
// not a target and the creature correctly drops it, so sampling at tick 300
// would be asserting the re-acquisition timer rather than the band.
constexpr int kGhostTargetProbeTick = 190;
// ONE THROW, as the owner's session reports it: drawing, then the release's
// swing (3 ticks, PlayerSession::throwLaunchIn). Probed a few ticks into each
// level, past the clip's blend-in start.
constexpr int kGhostThrowDrawFrom = 20;
constexpr int kGhostThrowSwingFrom = 32;
constexpr int kGhostThrowSwingTo = 35;
constexpr int kGhostThrowDrawProbe = 28;
constexpr int kGhostThrowSwingProbe = 33;

// What one arm measured.
struct GhostArm {
  int spawnTick = -1;         // first tick the ghost's avatar was spawned
  float worstPosErr = 0;      // rig root vs state pos, voxels, over all ticks
  int worstPosErrTick = -1;
  // How many ticks the comparison above actually RAN on. A perfectly placed
  // rig reports 0.0000 vox forever, which is indistinguishable from a
  // comparison that never executed — the gate asserts this instead of the
  // worst-case tick index, which is -1 in the healthy case.
  int posSamples = 0;
  bool deadWhileDead = true;  // despawned for the whole scripted death
  bool aliveAfterRevive = false;
  uint64_t discarded = 0;     // ops the ghost authored and we threw away
  int firstDiscardTick = -1;
  uint64_t leaked = 0;        // ops that reached the REAL batch. Must be 0.
  uint64_t mobId = 0, mobTarget = 0;
  bool mobBrain = false;
  bool combatantOk = false;
  float mobToGhost = 0, mobToLocal = 0;
  float windupW = 0, throwW = 0;        // the ghost's clip weights at the probes
  float windupAfterRelease = -1;        // wind-up weight at the swing probe
  std::vector<uint32_t> hashes;
  std::string why;
};

// The scripted outcome for one tick, as it would have arrived off the wire.
// A pure function of the tick: the whole point of `PlayerState` is that it is
// a RESULT, so the fixture writes results rather than driving a controller.
PlayerState GhostScript(int tick, const IVec3& site, uint32_t seed) {
  PlayerState st;
  st.tick = (uint32_t)tick;
  st.playerId = 7;  // the id is the WIRE's, not an index: deliberately not 0/1
  const int walked = kGhostStepVox * std::clamp(tick - kGhostWalkStart, 0,
                                                kGhostWalkEnd - kGhostWalkStart);
  const int x = site.x + walked;
  // The ground under the ghost, analytically. The OWNER resolved this against
  // its own mirror; here it is just a number that arrived, which is exactly
  // the fiction `PlayerState` encodes.
  const int h = World::TerrainHeight(x, site.z, seed);
  st.pos = Vec3{(float)x + 0.5f, (float)(h + 1) + Player::kHalfY,
                (float)site.z + 0.5f};
  const bool walking = tick >= kGhostWalkStart && tick < kGhostWalkEnd;
  st.vel = Vec3{walking ? (float)kGhostStepVox / kTickDt : 0.0f, 0, 0};
  st.heading = 1.5707963f;  // facing +X: a mob's forward is (sin h, ., cos h)
  st.lookYaw = 0;
  st.lookPitch = 0;
  st.Set(PlayerState::kGrounded, true);
  st.Set(PlayerState::kAlive, tick < kGhostDieTick || tick >= kGhostReviveTick);
  // ONE TICK, not a block: `jumped` is an EDGE and the avatar's jump clip keys
  // on its rising edge, so a flag held for two ticks would be testing the
  // script rather than the drain.
  st.Set(PlayerState::kJumped, tick == kGhostJumpTick);
  st.Set(PlayerState::kCrouching,
         tick >= kGhostCrouchFrom && tick < kGhostCrouchTo);
  // The other one-tick edge, same rule: one tick only.
  if (tick == kGhostLandTick) st.impactDeltaV = Vec3{0, -kGhostLandDeltaV, 0};
  st.Set(PlayerState::kThrowDraw,
         tick >= kGhostThrowDrawFrom && tick < kGhostThrowSwingFrom);
  st.Set(PlayerState::kThrowSwing,
         tick >= kGhostThrowSwingFrom && tick < kGhostThrowSwingTo);
  st.health = 100;
  return st;
}

void RunGhostArm(Ctx& c, GhostArm& a) {
  c.debris.Reset();
  // rewindIds: mob ids seed gore variance and blast noise, so the second arm
  // must start its id counter where the first did or the two runs are not the
  // same experiment.
  c.mobs.Reset(true);
  c.mobs.ClearRisings();
  c.world.SetWindowOrigin(kTwoPlayerOrigin);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  const std::vector<uint32_t> classOf = BuildCollisionClasses(c.mats);
  const int cx = (kTwoPlayerOrigin.x + (int)kNChunk / 2) * (int)kChunk;
  const int cz = (kTwoPlayerOrigin.z + (int)kNChunk / 2) * (int)kChunk;
  int reliefL = 0, reliefG = 0;
  // The LOCAL player stands well clear in +X and never moves. The only thing
  // it has to be is unambiguously the FARTHER of the two actors, so that "the
  // creature named the ghost" is a claim about the band and not a coin flip.
  const IVec3 siteLocal =
      TwoPlayerFlatSpot(cx + 176, cz, 24, kDefaultSeed, reliefL);
  // The ghost's path starts here and runs +X for 160 voxels, ending ~112
  // voxels short of the local player and comfortably inside the window.
  const IVec3 siteGhost =
      TwoPlayerFlatSpot(cx - 96, cz, 24, kDefaultSeed, reliefG);

  Player local;
  local.fly = false;
  local.pos = Vec3{(float)siteLocal.x + 0.5f,
                   (float)(siteLocal.y + 1) + Player::kHalfY,
                   (float)siteLocal.z + 0.5f};
  local.SnapRender();
  auto kindLocal = [&](IVec3 cell) { return c.world.KindAt(cell, classOf); };

  PlayerAvatar av0(0x5A11EDU);
  av0.Init(&c.phys, &c.world, &c.debris, c.mats, &c.mobs);
  av0.SetDefs(&c.mobs.Defs(), kAvatarDefName);
  if (!av0.HasDef()) {
    a.why = std::string("no mob def named \"") + kAvatarDefName + "\"";
    return;
  }
  av0.Spawn(local, 0.0f);
  const uint64_t pb0 = c.phys.CreatePlayerBody(Player::kHalfXZ, Player::kHalfY);

  // ---- the ghost ---------------------------------------------------------
  // `Upsert` only allocates. The first `Apply` + `RemotePlayersPreTick` pair
  // inside the loop is what spawns it, which is claim 1.
  RemotePlayers remotes;
  RemotePlayer& ghost = remotes.Upsert(7);

  // ---- one hostile creature, beside the ghost's path ---------------------
  // The same "crowder" fixture `two-players` uses and for the same reason:
  // mobile, hostile, wants a target, carries no attack intent and no styles,
  // so it is a targeting probe with none of a duelist's knife-fight noise.
  int defIndex = -1;
  for (size_t i = 0; i < c.mobs.Defs().size(); i++) {
    if (c.mobs.Defs()[i].FindSocket("held_right") < 0) continue;
    if (defIndex < 0 || c.mobs.Defs()[i].name == "human") defIndex = (int)i;
  }
  if (defIndex < 0) {
    a.why = "no mob def publishes a held_right socket";
    return;
  }
  const int mx = siteGhost.x + 14, mz = siteGhost.z;
  a.mobId = c.mobs.Spawn(
      defIndex, {mx, World::TerrainHeight(mx, mz, kDefaultSeed) + 1, mz});
  if (a.mobId == 0) {
    a.why = "Spawn refused for the hostile fixture";
    return;
  }
  if (!c.mobs.SetMobBehavior(a.mobId, "crowder")) {
    a.why = "no behaviour profile \"crowder\" in assets/mobs/behaviors.json";
    return;
  }

  // The local player holds nothing down for the whole run: this gate is about
  // the ghost, and a walking local body would only add noise to "which actor
  // is nearer".
  TickInput idle;
  idle.flatFwd = Vec3{1, 0, 0};
  idle.right = Vec3{0, 0, 1};
  idle.lookFwd = idle.flatFwd;

  uint32_t tick = 9000;
  for (int i = 0; i < kGhostTicks; i++) {
    // ---- the wire delivers one state ----------------------------------
    // Before the local tick, as the model requires: the batch labelled tick T
    // is applied before tick T runs, so every phase below sees ONE consistent
    // outcome for the peer.
    ghost.Apply(GhostScript(i, siteGhost, c.world.WorldSeed()));

    // ---- PHASE A / B (the local session, then the interest set) --------
    local.Update(kTickDt, idle, kindLocal);
    InterestSet interest;
    interest.chunks.push_back({ifloor(local.pos.x) >> 4,
                               ifloor(local.pos.y) >> 4,
                               ifloor(local.pos.z) >> 4});
    RemotePlayersAppendInterest(remotes, interest);
    interest.primary = 0;
    if (interest.chunks.size() != 2 && a.why.empty())
      a.why = "RemotePlayersAppendInterest did not add the ghost's chunk";

    // ---- PHASE H: the actor list, the avatar list, then the NPCs -------
    std::vector<MobSystem::PlayerActorDesc> actors;
    actors.push_back({local.pos, Player::kHalfXZ, Player::kHalfY * 2.0f, true});
    RemotePlayersAppendActors(remotes, actors);
    if (remotes.dirty) {
      Mob* sessionAvatars[1] = {&av0};
      RemotePlayersSyncAvatars(remotes, c.mobs,
                               std::span<Mob* const>(sessionAvatars, 1));
    }
    c.mobs.SetPlayerActors(std::span<const MobSystem::PlayerActorDesc>(
        actors.data(), actors.size()));

    // THE REAL BATCH. Everything the local session and the world author goes
    // here; the ghost's goes into `remotes.scratch*` and is counted, never
    // appended. Sizes are snapshotted around the ghost call below so a leak is
    // caught on the tick it happens rather than at the end of 300 of them.
    std::vector<BrushOp> ops;
    std::vector<CellOp> cellOps;
    std::vector<ParticleSpawn> spawns;
    c.mobs.PreTick(tick + 1, c.world, ops, cellOps, spawns);

    // ---- PHASE I: the local avatar, then the ghosts --------------------
    av0.PreTick(tick + 1, local, 0.0f, kTickDt, c.world, ops, cellOps, spawns);
    const size_t realBefore = ops.size() + cellOps.size() + spawns.size();
    const uint64_t discardBefore = remotes.discardedOps;
    RemotePlayersPreTick(remotes, tick + 1, kTickDt, c.world, c.phys, c.mobs,
                         c.debris, c.mats, kAvatarDefName);
    const size_t realAfter = ops.size() + cellOps.size() + spawns.size();
    a.leaked += (uint64_t)(realAfter - realBefore);
    if (remotes.discardedOps > discardBefore && a.firstDiscardTick < 0)
      a.firstDiscardTick = i;

    // ---- PHASE J (world).
    c.debris.QueueSupportEvents(c.world.Snap());
    c.debris.PreTick(tick + 1, c.world, cellOps, spawns);

    ++tick;
    c.phys.MovePlayerBody(pb0, local.pos, kTickDt);
    // The mirror is centred on the LOCAL player, as in the game: the ghost
    // does not move the window (RemotePlayersAppendInterest is information
    // only until M9.3 decides chunk authority).
    const IVec3 pc{ifloor(local.pos.x) >> 4, ifloor(local.pos.y) >> 4,
                   ifloor(local.pos.z) >> 4};
    // Six hash reads per arm rather than 300: a blocking readback, and a
    // divergence does not heal (CLAUDE.md's verification budget).
    const bool wantHash = (i % 60) == 59 || i == kGhostTicks - 1;
    SubmitTick(c.ctx, c.world, c.sim, tick, kDefaultSeed, ops, {}, cellOps,
               wantHash, pc, true, false, spawns);
    c.ctx.WaitIdle();
    c.ctx.ProcessEvents();
    if (wantHash) a.hashes.push_back(ReadHashSync(c.ctx, c.world));
    c.phys.Step(kTickDt);
    c.debris.PostStep();
    c.mobs.PostStep();
    av0.PostStep();
    // ---- PHASE O: the ghosts' settle.
    RemotePlayersPostStep(remotes);

    // ---- the assertions that have to be made EVERY tick ----------------
    const bool wantAlive = ghost.last.Has(PlayerState::kAlive);
    if (ghost.spawned && a.spawnTick < 0) a.spawnTick = i;
    if (i >= kGhostDieTick && i < kGhostReviveTick && ghost.spawned)
      a.deadWhileDead = false;
    if (i == kGhostTicks - 1) a.aliveAfterRevive = ghost.spawned;
    // WHERE THE RIG ACTUALLY IS. `Mob::Origin()` is the low corner of the
    // def's box (avatar.cpp's PreTick sets it from the player's pos), so the
    // root is recovered by adding back half the box in x/z and kHalfY in y.
    // Reconstructed from the RIG rather than read back off `ghost.ghost.pos`,
    // which would be comparing the fixture's own input against itself.
    if (wantAlive && ghost.spawned) {
      if (const MobDef* def = ghost.avatar.Def()) {
        const Vec3 o = ghost.avatar.Origin();
        const Vec3 root{o.x + def->worldSize.x * 0.5f, o.y + Player::kHalfY,
                        o.z + def->worldSize.z * 0.5f};
        const Vec3 d = root - ghost.last.pos;
        const float err = d.len();
        a.posSamples++;
        if (err > a.worstPosErr) {
          a.worstPosErr = err;
          a.worstPosErrTick = i;
        }
      }
    }
    if (i == kGhostThrowDrawProbe)
      a.windupW = ghost.avatar.ClipWeight("throw_windup");
    if (i == kGhostThrowSwingProbe) {
      a.throwW = ghost.avatar.ClipWeight("throw");
      // Stopping, not gone: a released wind-up blends out over the swing.
      a.windupAfterRelease = ghost.avatar.ClipWeight("throw_windup");
    }
    if (i == kGhostTargetProbeTick) {
      const ai::Brain* br = c.mobs.MobBrain(a.mobId);
      a.mobBrain = br != nullptr;
      if (br != nullptr) a.mobTarget = br->hasTarget ? br->targetId : 0;
      // The band resolves to the RIGHT body: base+1 is the GHOST and base+0 is
      // still the local session. Two claims, checked separately — a band that
      // resolved both to the same avatar would satisfy the first alone.
      a.combatantOk = c.mobs.FindCombatantById(ai::kPlayerActorBase + 1) ==
                          (Mob*)&ghost.avatar &&
                      c.mobs.FindCombatantById(ai::kPlayerActorBase + 0) ==
                          (Mob*)&av0;
      const Vec3 mo = c.mobs.MobOrigin(a.mobId);
      const float dgx = mo.x - ghost.last.pos.x, dgz = mo.z - ghost.last.pos.z;
      const float dlx = mo.x - local.pos.x, dlz = mo.z - local.pos.z;
      a.mobToGhost = std::sqrt(dgx * dgx + dgz * dgz);
      a.mobToLocal = std::sqrt(dlx * dlx + dlz * dlz);
    }
  }
  a.discarded = remotes.discardedOps;

  // Leave nothing behind: the avatars own limb bodies, the proxies are Jolt
  // bodies, and the mob list is shared with every later gate.
  remotes.Clear(c.phys);
  av0.Despawn();
  c.mobs.SetAvatars({});
  c.mobs.ClearPlayerActors();
  c.phys.RemoveBody(pb0);
  c.debris.Reset();
  c.mobs.Reset(true);
  c.mobs.ClearRisings();
}

Status GateRemoteGhost(Ctx& c, std::string& detail) {
  const IVec3 savedOrigin = c.world.WindowOrigin();
  GhostArm a, b;
  RunGhostArm(c, a);
  if (a.why.empty()) RunGhostArm(c, b);
  // Put the window back and regenerate: this gate moved the origin and
  // regenerated the world twice, and every gate after it in kOrder assumes
  // worldgen it did not move (selftest.h's ordering note).
  c.world.SetWindowOrigin(savedOrigin);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  if (!a.why.empty()) {
    detail = a.why;
    std::printf("remote ghost: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }

  // (1) SPAWNED ON THE FIRST STATE.
  const bool spawnOk = a.spawnTick >= 0 && a.spawnTick <= 2;
  // (2) WHERE IT WAS TOLD TO BE. Half a voxel: the rig's root is placed from
  // `pos` directly, so the only slack is the box reconstruction's rounding.
  const double posTol = BaselineNumber("remoteGhost.posTolVox", 0.5);
  // The sample count is half the claim: a tolerance test over zero samples
  // passes, and a rig placed exactly reports 0.0000 forever. Most of the 300
  // ticks are alive+spawned, so anything under 200 means the ghost spent the
  // run despawned and the position was never actually compared.
  const bool posOk = a.worstPosErr <= (float)posTol && a.posSamples >= 200;
  // (3) THE WIRE OWNS ITS LIFE.
  const bool lifeOk = a.deadWhileDead && a.aliveAfterRevive;
  // (4) THE BAND, and the creature that uses it.
  const bool nearer = a.mobToGhost < a.mobToLocal;
  const bool targetOk = a.mobBrain && a.combatantOk && nearer &&
                        a.mobTarget == ai::kPlayerActorBase + 1;
  // (5) IT TRIED TO AUTHOR, AND NOTHING GOT OUT. Both halves, or this is a
  // fixture that cannot fail.
  const bool triedOk = a.discarded > 0 && a.firstDiscardTick >= 0;
  const bool noLeakOk = a.leaked == 0;
  // (6) THE TWICE-RUN CONTRACT. Not a pin — a reproduction. This is the only
  // claim here whose failure is a stop-and-report bug rather than a number.
  const bool detOk = !a.hashes.empty() && a.hashes == b.hashes &&
                     a.discarded == b.discarded &&
                     a.posSamples == b.posSamples &&
                     a.worstPosErrTick == b.worstPosErrTick;

  // Recorded so a future failure can say WHICH WAY the fixture moved rather
  // than only that it did (CLAUDE.md rule 6).
  RecordObserved("remoteGhost.spawnTick", (double)a.spawnTick);
  RecordObserved("remoteGhost.worstPosErrVox", (double)a.worstPosErr);
  RecordObserved("remoteGhost.posSamples", (double)a.posSamples);
  RecordObserved("remoteGhost.discardedOps", (double)a.discarded);
  RecordObserved("remoteGhost.firstDiscardTick", (double)a.firstDiscardTick);
  RecordObserved("remoteGhost.leakedOps", (double)a.leaked);

  // (7) THE OTHER PLAYER SEES YOU THROW: the wire's throw levels drive the
  // same clips on the ghost's rig that session.cpp plays on the owner's.
  const bool throwOk = a.windupW > 0.0f && a.throwW > 0.0f;

  const bool ok = spawnOk && posOk && lifeOk && targetOk && triedOk &&
                  noLeakOk && detOk && throwOk;
  char buf[900];
  std::snprintf(
      buf, sizeof buf,
      "%d ticks x2: spawned tick %d (want <= 2) | worst root-vs-state %.4f vox "
      "at tick %d over %d samples (tol %.2f) | dead %d..%d held %d, revived %d "
      "| authored %llu ops, ALL discarded (first at tick %d), leaked %llu | "
      "mob %llu targets %llx (want %llx) at tick %d, d(ghost)=%.1f "
      "d(local)=%.1f, band resolves %d | throw clips on the ghost: wind-up "
      "%.2f, throw %.2f (wind-up %.2f after release) | hashes %zu %s",
      kGhostTicks, a.spawnTick, a.worstPosErr, a.worstPosErrTick, a.posSamples,
      posTol,
      kGhostDieTick, kGhostReviveTick, a.deadWhileDead ? 1 : 0,
      a.aliveAfterRevive ? 1 : 0, (unsigned long long)a.discarded,
      a.firstDiscardTick, (unsigned long long)a.leaked,
      (unsigned long long)a.mobId, (unsigned long long)a.mobTarget,
      (unsigned long long)(ai::kPlayerActorBase + 1), kGhostTargetProbeTick,
      a.mobToGhost, a.mobToLocal, a.combatantOk ? 1 : 0, a.windupW, a.throwW,
      a.windupAfterRelease, a.hashes.size(),
      detOk ? "reproduced" : "DIVERGED between the two runs");
  detail = buf;
  std::printf("remote ghost: %s (%s)\n", ok ? "PASS" : "FAIL", buf);
  return ok ? Status::Pass : Status::Fail;
}

// ---- layer-roles (W2-N) ------------------------------------------------------
//
// A body's collision layer is DERIVED from its role (Physics::BodyRole), and a
// player's own body is exempt from THAT player's capsule only. Four claims,
// each a separate arm so a failure names which:
//   A. the table: every role x owner kind resolves to the documented layer,
//      settled and clearing (the pure ResolveLayer, walked in full);
//   B. owner scoping with two avatars and two capsules: A's limbs do not meet
//      A's capsule and do not shove A (PlayerPushOut), and DO meet B's capsule
//      and DO shove B; an avatar never told its proxy is exempt from both
//      (the pre-W2-N AVATAR layer, the control);
//   C. a throw owned by A clears A (its capsule and its limbs) but meets B,
//      and settles to loose Debris on MOVING once clear of both;
//   D. the transitions: an avatar going limp keeps its owned layer, an NPC
//      going limp clears first then settles on MOVING, and an attached role
//      given to a body still clearing applies at once when no player could
//      feel it (PROP) and waits otherwise — the get-up sword that a stale
//      release used to knock back onto MOVING.
// CPU + Jolt only: no tick, no World mutation. Leaves nothing behind.
Status GateLayerRoles(Ctx& c, std::string& detail) {
  using R = Physics::BodyRole;
  Physics& phys = c.phys;
  std::vector<std::string> fails;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) fails.push_back(what);
  };
  char buf[256];

  // ---- A. the table ----------------------------------------------------------
  // Layer ids (physics.h BodyObjectLayer): 1 MOVING, 3 EXEMPT, 4 PROP,
  // 5 THROWN, 16+s OWNED by s, 24+s THROWN by s. Slot 2 stands in for "some
  // owner" so an off-by-one between the families shows up.
  struct Row {
    R role;
    int none, owned, any;           // settled: owner none / slot 2 / any
    int cNone, cOwned, cAny;        // clearing
  };
  const Row rows[] = {
      {R::Debris, 1, 1, 1, 3, 3, 3},
      {R::RigLive, 1, 18, 3, 3, 3, 3},
      {R::RigLimp, 1, 1, 1, 3, 3, 3},
      {R::RigDead, 1, 1, 1, 3, 3, 3},
      {R::WornShell, 1, 18, 3, 3, 3, 3},
      {R::HeldProp, 4, 4, 4, 4, 4, 4},
      {R::Carried, 1, 18, 3, 3, 3, 3},
      {R::SeveredHold, 3, 3, 3, 3, 3, 3},
      {R::Thrown, 1, 1, 1, 5, 26, 5},
  };
  int tableBad = 0;
  for (const Row& r : rows) {
    const int got[6] = {
        Physics::ResolveLayer(r.role, -2, false),
        Physics::ResolveLayer(r.role, 2, false),
        Physics::ResolveLayer(r.role, -1, false),
        Physics::ResolveLayer(r.role, -2, true),
        Physics::ResolveLayer(r.role, 2, true),
        Physics::ResolveLayer(r.role, -1, true)};
    const int want[6] = {r.none, r.owned, r.any, r.cNone, r.cOwned, r.cAny};
    for (int k = 0; k < 6; k++)
      if (got[k] != want[k]) {
        tableBad++;
        std::snprintf(buf, sizeof buf, "table %s[%d] = %d, want %d",
                      Physics::RoleName(r.role), k, got[k], want[k]);
        check(false, buf);
      }
  }

  // ---- fixture: two capsules, two avatars -----------------------------------
  const IVec3 wo = c.world.WindowOrigin();
  const int cx = wo.x + (int)kWorldN / 2, cz = wo.z + (int)kWorldN / 2;
  const float gy = (float)World::TerrainHeight(cx, cz, kDefaultSeed) + 1.0f;
  const uint64_t pb0 = phys.CreatePlayerBody(Player::kHalfXZ, Player::kHalfY);
  const uint64_t pb1 = phys.CreatePlayerBody(Player::kHalfXZ, Player::kHalfY);
  const int s0 = phys.PlayerSlotOf(pb0), s1 = phys.PlayerSlotOf(pb1);
  check(s0 >= 0 && s1 >= 0 && s0 != s1, "two capsules, two distinct slots");
  Player pA, pB, pC;
  pA.fly = pB.fly = pC.fly = true;
  pA.pos = Vec3{(float)cx + 0.5f, gy + Player::kHalfY, (float)cz + 0.5f};
  pB.pos = pA.pos + Vec3{40.0f, 0.0f, 0.0f};
  pC.pos = pA.pos + Vec3{-40.0f, 0.0f, 0.0f};
  pA.SnapRender();
  pB.SnapRender();
  pC.SnapRender();
  auto pin = [&](uint64_t pb, Vec3 at) {
    phys.MovePlayerBody(pb, at, kTickDt);
    phys.SetBodyVelocity(pb, Vec3{});
  };
  pin(pb0, pA.pos);
  pin(pb1, pB.pos);

  PlayerAvatar avA(0x5A11EDU + 20), avB(0x5A11EDU + 21), avC(0x5A11EDU + 22);
  for (PlayerAvatar* av : {&avA, &avB, &avC}) {
    av->Init(&c.phys, &c.world, &c.debris, c.mats, &c.mobs);
    av->SetDefs(&c.mobs.Defs(), kAvatarDefName);
  }
  if (!avA.HasDef()) {
    phys.RemoveBody(pb0);
    phys.RemoveBody(pb1);
    detail = std::string("no mob def named \"") + kAvatarDefName + "\"";
    std::printf("layer-roles: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  // A is told its capsule BEFORE it spawns (the main.cpp order), B AFTER
  // (the re-application path: SetCollisionOwner re-derives every live slot),
  // C never (the control).
  avA.SetCollisionOwner(pb0);
  avA.Spawn(pA, 0.0f);
  avB.Spawn(pB, 0.0f);
  avB.SetCollisionOwner(pb1);
  avC.Spawn(pC, 0.0f);
  std::vector<uint64_t> limbsA, limbsB, limbsC;
  avA.AppendBodyHandles(limbsA);
  avB.AppendBodyHandles(limbsB);
  avC.AppendBodyHandles(limbsC);
  check(!limbsA.empty() && !limbsB.empty() && !limbsC.empty(),
        "three avatars spawned with limb bodies");

  // ---- B. owner scoping -----------------------------------------------------
  int aWrongLayer = 0, aMeetsOwn = 0, aMissesOther = 0;
  for (uint64_t h : limbsA) {
    if (phys.BodyObjectLayer(h) != 16 + s0) aWrongLayer++;
    if (phys.LayersCollide(h, pb0)) aMeetsOwn++;
    if (!phys.LayersCollide(h, pb1)) aMissesOther++;
  }
  int bWrongLayer = 0;
  for (uint64_t h : limbsB)
    if (phys.BodyObjectLayer(h) != 16 + s1) bWrongLayer++;
  int cMeetsAny = 0, cWrongLayer = 0;
  for (uint64_t h : limbsC) {
    if (phys.BodyObjectLayer(h) != 3) cWrongLayer++;
    if (phys.LayersCollide(h, pb0) || phys.LayersCollide(h, pb1)) cMeetsAny++;
  }
  std::snprintf(buf, sizeof buf,
                "A's %zu limbs: %d off OWNED(%d), %d meet A's capsule, %d miss "
                "B's", limbsA.size(), aWrongLayer, s0, aMeetsOwn, aMissesOther);
  check(aWrongLayer == 0 && aMeetsOwn == 0 && aMissesOther == 0, buf);
  std::snprintf(buf, sizeof buf, "B (owner told after spawn): %d limbs off OWNED(%d)",
                bWrongLayer, s1);
  check(bWrongLayer == 0, buf);
  std::snprintf(buf, sizeof buf,
                "C (no owner): %d limbs off EXEMPT, %d meet a capsule",
                cWrongLayer, cMeetsAny);
  check(cWrongLayer == 0 && cMeetsAny == 0, buf);
  // The push itself — the claim a player feels. A standing in A's own body
  // is not shoved by it; B standing in the same place is, by one of A's.
  Physics::PushSource worstB{};
  const float pushOwn = phys.PlayerPushOut(pb0, pA.pos).len();
  const float pushOther = phys.PlayerPushOut(pb1, pA.pos, &worstB).len();
  const bool byA =
      std::find(limbsA.begin(), limbsA.end(), worstB.body) != limbsA.end();
  std::snprintf(buf, sizeof buf,
                "push inside A's body: A %.3f vox (want 0), B %.3f vox by %s "
                "(want > 0, by one of A's limbs)",
                pushOwn, pushOther, byA ? "A's limb" : "something else");
  check(pushOwn < 1e-3f && pushOther > 1e-3f && byA, buf);

  // ---- C. a throw owned by A --------------------------------------------------
  std::vector<DebrisVoxel> cube;
  for (int8_t z = 0; z < 2; z++)
    for (int8_t y = 0; y < 2; y++)
      for (int8_t x = 0; x < 2; x++)
        cube.push_back(DebrisVoxel{x, y, z, 0, (uint16_t)kMatStone});
  auto block = [&](Vec3 at) {
    BodyTransform xf{};
    xf.pos = at;
    xf.quat[3] = 1;
    const uint64_t h = phys.CreateDebrisBodyXf(cube, xf, c.debris.DensityOf(), true);
    if (h) {
      phys.SetBodyKinematic(h, true);  // stays where it is put
    }
    return h;
  };
  const uint64_t flask = block(pA.pos);
  phys.SetBodyRole(flask, R::Thrown, pb0);
  const bool thrownClearing = phys.BodyClearing(flask);
  const int thrownLayer = phys.BodyObjectLayer(flask);
  const bool fMeetsA = phys.LayersCollide(flask, pb0) ||
                       (!limbsA.empty() && phys.LayersCollide(flask, limbsA[0]));
  const bool fMeetsB = phys.LayersCollide(flask, pb1) &&
                       (!limbsB.empty() && phys.LayersCollide(flask, limbsB[0]));
  std::snprintf(buf, sizeof buf,
                "throw by A: layer %d (want %d), clearing %d, meets A %d, "
                "meets B's capsule+limb %d",
                thrownLayer, 24 + s0, thrownClearing ? 1 : 0, fMeetsA ? 1 : 0,
                fMeetsB ? 1 : 0);
  check(thrownLayer == 24 + s0 && thrownClearing && !fMeetsA && fMeetsB, buf);

  // ---- D. transitions ---------------------------------------------------------
  // A pending body given an ATTACHED role nobody can feel (HeldProp) stops
  // clearing at once; given one a player could feel (an unowned RigLive, i.e.
  // MOVING) it keeps clearing and settles on THAT role's layer when clear.
  const uint64_t sword = block(pA.pos);
  phys.SetBodyRole(sword, R::RigLimp);
  const bool swordWasClearing = phys.BodyClearing(sword);
  phys.SetBodyRole(sword, R::HeldProp);
  const uint64_t limb = block(pA.pos);
  phys.SetBodyRole(limb, R::RigLimp);
  phys.SetBodyRole(limb, R::RigLive);
  const bool limbStillClearing =
      phys.BodyClearing(limb) && phys.BodyObjectLayer(limb) == 3;
  // A goes limp: an avatar's limbs stay its own. An NPC-like loose limp role is
  // the `limb` above; the NPC path proper is exercised by `ragdoll`.
  avA.StartRagdoll(1.0f, "layer-roles");
  int limpOff = 0;
  std::vector<uint64_t> limbsA2;
  avA.AppendBodyHandles(limbsA2);
  for (uint64_t h : limbsA2)
    if (phys.BodyObjectLayer(h) != 16 + s0 || phys.BodyClearing(h)) limpOff++;
  // Everyone walks away; one Step runs the clearing sweep.
  const Vec3 far{pA.pos.x, pA.pos.y + 200.0f, pA.pos.z};
  pin(pb0, far);
  pin(pb1, far + Vec3{20.0f, 0.0f, 0.0f});
  phys.Step(kTickDt);
  const bool flaskSettled = !phys.BodyClearing(flask) &&
                            phys.BodyObjectLayer(flask) == 1 &&
                            phys.BodyRoleOf(flask) == R::Debris;
  const bool swordProp = !phys.BodyClearing(sword) &&
                         phys.BodyObjectLayer(sword) == 4;
  const bool limbSettled = !phys.BodyClearing(limb) &&
                           phys.BodyObjectLayer(limb) == 1 &&
                           phys.BodyRoleOf(limb) == R::RigLive;
  std::snprintf(buf, sizeof buf,
                "clear of both: throw settled %d (Debris on MOVING), "
                "pending->HeldProp %d (was clearing %d), pending->RigLive held "
                "%d then settled %d, avatar limp keeps OWNED: %d off",
                flaskSettled ? 1 : 0, swordProp ? 1 : 0,
                swordWasClearing ? 1 : 0, limbStillClearing ? 1 : 0,
                limbSettled ? 1 : 0, limpOff);
  check(flaskSettled && swordProp && swordWasClearing && limbStillClearing &&
            limbSettled && limpOff == 0,
        buf);

  // ---- leave nothing behind --------------------------------------------------
  for (uint64_t h : {flask, sword, limb})
    if (h) phys.RemoveBody(h);
  avA.Despawn();
  avB.Despawn();
  avC.Despawn();
  phys.RemoveBody(pb0);
  phys.RemoveBody(pb1);

  const bool ok = fails.empty();
  detail.clear();
  for (const std::string& f : fails) detail += (detail.empty() ? "" : " | ") + f;
  if (ok)
    detail = "table 9 roles x 6 walked; A/B owner-scoped both ways (push own "
             "0, other > 0); throw clears thrower only; clearing transitions ok";
  std::printf("layer-roles: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  (void)tableBad;
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& PlayerGates() {
  static const std::vector<Gate> g = {
      {"player-walk", "player", {}, false, GatePlayerWalk},
      {"player-waterjump", "player", {}, false, GatePlayerWaterJump},
      {"player-ledgegrab", "player", {}, false, GatePlayerLedgeGrab},
      {"player-crouch", "player", {}, false, GatePlayerCrouch},
      {"player-plants", "player", {}, false, GatePlayerPlants},
      {"player-fastfall", "player", {}, false, GatePlayerFastFall},
      {"tick-input", "player", {}, false, GateTickInput},
      {"view-smooth", "player", {}, false, GateViewSmooth},
      {"two-players", "player", {}, false, GateTwoPlayers},
      {"remote-ghost", "player", {}, false, GateRemoteGhost},
      {"layer-roles", "player", {}, false, GateLayerRoles},
  };
  return g;
}

}  // namespace selftest
