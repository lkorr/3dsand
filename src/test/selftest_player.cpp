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

#include "game/player.h"
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
  };
  return g;
}

}  // namespace selftest
