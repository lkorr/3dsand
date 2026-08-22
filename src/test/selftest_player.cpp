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
    player.Update(1.0f / 30.0f, PlayerInput{}, Vec3{1, 0, 0}, Vec3{0, 0, 1},
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
      player.Update(1.0f / 30.0f, PlayerInput{}, Vec3{1, 0, 0}, Vec3{0, 0, 1},
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
      player.Update(1.0f / 30.0f, PlayerInput{}, Vec3{1, 0, 0}, Vec3{0, 0, 1},
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
// Three assertions, and the negative ones carry the weight: an unconditional
// "jump works in water" would pass the first alone.
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
    PlayerInput swim;
    swim.forward = 1.0f;  // press into the rim the whole time
    swim.up = true;       // tread water, or buoyancy alone sinks us
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
    PlayerInput jump;
    jump.forward = 1.0f;
    jump.jumpPressed = true;
    jump.up = true;
    pool.Update(dt, jump, fwd, right, fwd, poolFn);
  }
  bool fired = pool.waterJumped;
  // Let the climb play out and then some. Input keeps pressing forward
  // (jumpPressed is a single frame, exactly as main.cpp delivers it). The extra
  // frames past the mantle timeout matter: they are what catches a climb that
  // reaches the lip and then slides back into the pool, which is the failure
  // this whole mechanic exists to prevent.
  {
    PlayerInput hold;
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
    PlayerInput jump;
    jump.forward = 1.0f;
    jump.jumpPressed = true;
    jump.up = true;
    cliff.Update(dt, jump, fwd, right, fwd, cliffFn);
  }
  bool cliffFired = cliff.waterJumped;

  // (c) OPEN WATER: pressing away from the rim, into nothing. Must not fire —
  // this is what keeps ordinary swimming unchanged.
  Player open = settleAtRim(poolFn);
  bool openFired = false;
  {
    PlayerInput jump;
    jump.forward = -1.0f;  // away from the rim
    jump.jumpPressed = true;
    jump.up = true;
    open.Update(dt, jump, fwd, right, fwd, poolFn);
    openFired = open.waterJumped;
  }

  bool ok = wasSwimming && fired && climbedOut && !cliffFired && !openFired;
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "floated feet y=%.1f -> out y=%.1f x=%.1f (rim %.0f), "
                "fired=%d cliff=%d open=%d",
                floatFeet, outFeet, pool.pos.x, kRim, fired ? 1 : 0,
                cliffFired ? 1 : 0, openFired ? 1 : 0);
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
  // the chain. The tier height leaves ~4 voxels between the apex (anchor
  // y=103 + 14 voxels of jumpSpeed rise) and the bottom of lip2's latch
  // window, so a modest jumpSpeed retune does not silently break the gate.
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
        stage = pauseAtHang ? 2 : 3;
      } else if (stage == 2) {
        if (!r.p.hanging) r.droppedWhileWaiting = true;
        if (++hangHeld >= 30) stage = 3;
      } else if (stage == 3 && r.p.hanging) {
        r.everHung = true;  // chained re-grabs during the climb
      }
      PlayerInput in;
      in.sprint = true;
      if (stage == 0) {  // run-up
        in.forward = 1.0f;
        if (r.p.grounded && r.p.pos.x > 135.0f) {
          in.jumpPressed = true;
          in.up = space;
          stage = 1;
        }
      } else if (stage == 1) {  // flight, pressed into the wall
        in.forward = 1.0f;
        in.up = space;
      } else if (stage == 2) {  // dangle: hands only, W released
        in.up = space;
      } else {  // pull up and keep walking over the top
        in.forward = 1.0f;
        in.up = space;
      }
      r.p.Update(dt, in, fwd, right, fwd, kindAt);
    }
    return r;
  };

  // (a) the headline move: grab, hold a half-second dead hang, pull up, stand.
  RunResult a = run(plateauKind, true, true, 600);
  float aFeet = a.p.pos.y - Player::kHalfY;
  bool aOk = a.everHung && !a.droppedWhileWaiting && a.p.grounded &&
             !a.p.hanging && std::abs(aFeet - 115.0f) < 0.75f &&
             a.p.pos.x > 150.0f + Player::kHalfXZ;

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
  // "hold W and space", which is the point of the feature.
  RunResult d = run(noisyKind, true, false, 900);
  float dFeet = d.p.pos.y - Player::kHalfY;
  bool dOk = d.everHung && d.p.grounded && !d.p.hanging &&
             std::abs(dFeet - 128.0f) < 0.75f;

  bool ok = aOk && bOk && cOk && dOk;
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "plateau: hung=%d dropped=%d feet=%.1f x=%.1f (want 115, "
                ">153); cliff hung=%d feet=%.1f; no-space hung=%d feet=%.1f; "
                "chain hung=%d feet=%.1f (want 128)",
                a.everHung ? 1 : 0, a.droppedWhileWaiting ? 1 : 0, aFeet,
                a.p.pos.x, b.everHung ? 1 : 0, bFeet, c.everHung ? 1 : 0,
                cFeet, d.everHung ? 1 : 0, dFeet);
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
    PlayerInput in{};
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

}  // namespace

const std::vector<Gate>& PlayerGates() {
  static const std::vector<Gate> g = {
      {"player-walk", "player", {}, false, GatePlayerWalk},
      {"player-waterjump", "player", {}, false, GatePlayerWaterJump},
      {"player-ledgegrab", "player", {}, false, GatePlayerLedgeGrab},
      {"player-plants", "player", {}, false, GatePlayerPlants},
  };
  return g;
}

}  // namespace selftest
