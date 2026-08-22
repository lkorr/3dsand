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
  std::vector<uint32_t> classOf;
  for (auto& m : mats) classOf.push_back(m.gpu.klass);
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

}  // namespace

const std::vector<Gate>& PlayerGates() {
  static const std::vector<Gate> g = {
      {"player-walk", "player", {}, false, GatePlayerWalk},
      {"player-waterjump", "player", {}, false, GatePlayerWaterJump},
  };
  return g;
}

}  // namespace selftest
