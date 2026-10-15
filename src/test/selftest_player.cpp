// selftest_player.cpp — player selftest gates.
//
// Bodies moved verbatim out of the old monolithic RunSelftest; see
// scripts/split_selftest.py for the exact source ranges. Each gate returns a
// Status and fills `detail` with the parenthetical the old printf carried, so
// the console output is unchanged and --json can carry the same numbers.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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
#include "sim/biomes.h"
#include "sim/currentprim.h"
#include "sim/materials.h"
#include "sim/stream.h"
#include "sim/treeatlas.h"
#include "sim/tuning.h"
#include "sim/worldgen_run.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

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

// player-current — A RIVER CARRIES A SWIMMER, NOT A WADER (2026-10-14).
//
// One stream primitive running +X through a synthetic pool (the lambda
// fixture, for the reason over GatePlayerWaterJump). Two bodies, no movement
// input on either:
//   WADE  waist-deep with feet on the bed. The legs hold against the flow,
//         so the body must not move and Player::currentVel must stay zero.
//   TREAD 30 deep, Space held at the surface. The water carries it
//         downstream, and the velocity the avatar's swim pose reads
//         (vel - currentVel, avatar.cpp) must be near zero -- the pose treads
//         while it drifts instead of crawling the way the river goes.
Status GatePlayerCurrent(Ctx&, std::string& detail) {
  const int kFloor = 100, kWadeTop = 108, kDeepTop = 130;
  CurrentPrims().Clear();
  CurrentPrim st;
  st.kind = kCurrentPrimStream;
  st.flags = kCurrentPrimSim;
  st.x = 128; st.y = 115; st.z = 140;
  st.radius = 256;
  st.reach = 256;
  st.decayTicks = kCurrentPrimForever;
  st.ownerId = 0x7E58u;
  CurrentPrimAim(st, Vec3{1.0f, 0.0f, 0.0f}, 2.0f);
  const bool spawned = CurrentPrims().Spawn(st);
  CurrentPrims().Tick(64);
  const Vec3 field = CurrentAtCpu(Vec3{128.5f, 115.5f, 140.5f});

  const Vec3 fwd{1, 0, 0}, right{0, 0, 1};
  const float dt = 1.0f / 60.0f;
  auto pool = [&](int top) {
    return Player::KindFn([=](IVec3 c) {
      if (c.y < kFloor) return CellKind::Solid;
      return c.y < top ? CellKind::Liquid : CellKind::Air;
    });
  };

  Player::KindFn wadeFn = pool(kWadeTop);
  Player wade;
  wade.fly = false;
  wade.pos = Vec3{128.5f, kFloor + Player::kHalfY, 140.5f};
  for (int i = 0; i < 30; i++) wade.Update(dt, TickInput{}, fwd, right, fwd, wadeFn);
  const float wadeX0 = wade.pos.x;
  bool wadeHeld = true;
  for (int i = 0; i < 120; i++) {
    wade.Update(dt, TickInput{}, fwd, right, fwd, wadeFn);
    wadeHeld &= wade.currentVel.x == 0.0f && wade.currentVel.z == 0.0f;
  }
  const bool wadeOk = wade.inLiquid && wade.grounded && !wade.swimming &&
                      wadeHeld && std::fabs(wade.pos.x - wadeX0) < 0.5f;

  Player::KindFn deepFn = pool(kDeepTop);
  Player tread;
  tread.fly = false;
  tread.pos = Vec3{100.5f, (float)kDeepTop - 1.0f, 140.5f};
  TickInput up;
  up.SetHeld(TB_JUMP, true);
  for (int i = 0; i < 30; i++) tread.Update(dt, up, fwd, right, fwd, deepFn);
  const float treadX0 = tread.pos.x;
  for (int i = 0; i < 120; i++) tread.Update(dt, up, fwd, right, fwd, deepFn);
  const float drift = tread.pos.x - treadX0;
  const float relH = std::hypot(tread.vel.x - tread.currentVel.x,
                                tread.vel.z - tread.currentVel.z);
  const float curH = std::hypot(tread.currentVel.x, tread.currentVel.z);
  const bool treadOk = tread.inLiquid && !tread.grounded && drift > 5.0f &&
                       curH > 1.0f && relH < 0.25f * curH;
  CurrentPrims().Clear();

  const bool ok = spawned && field.x > 1.0f && wadeOk && treadOk;
  char buf[320];
  std::snprintf(buf, sizeof(buf),
                "field %.1f vox/s | wade wet=%d ground=%d swim=%d dx %.2f "
                "held=%d | tread dx %.1f current %.1f rel %.2f",
                field.x, wade.inLiquid ? 1 : 0, wade.grounded ? 1 : 0,
                wade.swimming ? 1 : 0, wade.pos.x - wadeX0, wadeHeld ? 1 : 0,
                drift, curH, relH);
  detail = buf;
  return ok ? Status::Pass : Status::Fail;
}

// player-swim-entry — THE WATER CARRIES A BODY ONLY WHERE IT IS NECK DEEP
// (ticket 2026-10-14_18-18-29, owner: "entering shallow water from certain
// elevations can look extremely goofy ... swimming animation should only start
// if floating fully in water at the neck").
//
// The bug: a body that DROPPED into water it could stand in -- off a bank, a
// jump -- was afloat (wet, no footing) for the frames of the drop, the implied
// tread caught it at the surface float, and it never touched the bed again:
// it swam, and the avatar played the stroke, in waist-deep water. Walking in
// from the same bank at bed level never showed it, which is why it was "from
// certain elevations".
//
// One synthetic pool (the lambda fixture, as player-waterjump), bed at y=100,
// the body dropped from six voxels over the surface with no input:
//   WAIST    9 deep  (0.53 of the 1.7 m figure): ends standing on the bed,
//                     wading -- grounded, not swimming, not swimWater.
//   SHOULDER 13 deep (0.76): the same -- shoulder deep is still a wade (the
//                     old 0.75-sample swim line took the footing here).
//   ...and WALKING OFF THE BANK into the waist-deep pool, W held: the same.
//   NECK     16 deep (0.94): swimWater, afloat at the surface (the tread).
//   HELD     ...then the bed under the floating body rises to 13 deep: still
//                     swimWater (it lets go below the chest, not the neck);
//                     and to 10 deep: let go, and the body sinks to the bed.
// Also asserted per tick on the shallow arms: not afloat-and-held -- no tick
// of the last two seconds (of five) in liquid without footing.
Status GatePlayerSwimEntry(Ctx&, std::string& detail) {
  const int kBed = 100;
  const float dt = 1.0f / 30.0f;
  const Vec3 fwd{1, 0, 0}, right{0, 0, 1};
  struct Arm {
    bool grounded = false, swimming = false, swimWater = false, inLiquid = false;
    float sole = 0.0f, depth = 0.0f;
    int afloatTicks = 0;   // wet with no footing, in the run's last 60 ticks
  };
  // bedAt: the bed's top (cells below it are solid); top: the waterline.
  // x >= bankX is a bank whose top is `bankTop` (walk-off arm only).
  auto run = [&](int bedAt, int top, Vec3 start, const TickInput& in, int ticks,
                 Player* keep = nullptr, int bankX = 1 << 20, int bankTop = 0) {
    Player::KindFn fn = [=](IVec3 c) {
      if (c.x >= bankX) return c.y < bankTop ? CellKind::Solid : CellKind::Air;
      if (c.y < bedAt) return CellKind::Solid;
      return c.y < top ? CellKind::Liquid : CellKind::Air;
    };
    Player local;
    Player& p = keep ? *keep : local;
    if (!keep) {
      p.fly = false;
      p.pos = start;
    }
    Arm a;
    for (int i = 0; i < ticks; i++) {
      p.Update(dt, in, fwd, right, fwd, fn);
      if (i >= ticks - 60 && p.inLiquid && !p.grounded) a.afloatTicks++;
    }
    a.grounded = p.grounded;
    a.swimming = p.swimming;
    a.swimWater = p.swimWater;
    a.inLiquid = p.inLiquid;
    a.sole = p.pos.y - Player::kHalfY;
    a.depth = p.waterDepth;
    return a;
  };
  auto dropAt = [&](int top) {
    return Vec3{120.5f, (float)top + 6.0f + Player::kHalfY, 140.5f};
  };
  const TickInput idle{};
  const Arm waist = run(kBed, kBed + 9, dropAt(kBed + 9), idle, 150);
  const Arm shoulder = run(kBed, kBed + 13, dropAt(kBed + 13), idle, 150);
  // Off a bank whose top is 4 over the water, walking toward -x into the pool.
  TickInput walk;
  walk.forward = -1.0f;
  const Arm bank = run(kBed, kBed + 9,
                       Vec3{141.5f, (float)(kBed + 13) + Player::kHalfY, 140.5f},
                       walk, 150, nullptr, 140, kBed + 13);
  auto wading = [&](const Arm& a) {
    return a.inLiquid && a.grounded && !a.swimming && !a.swimWater &&
           std::fabs(a.sole - (float)kBed) < 0.6f && a.afloatTicks == 0;
  };
  Player deep;
  deep.fly = false;
  deep.pos = dropAt(kBed + 16);
  const Arm neck = run(kBed, kBed + 16, Vec3{}, idle, 150, &deep);
  // The bed rises under the floater; the surface stays where it was.
  const Arm held = run(kBed + 3, kBed + 16, Vec3{}, idle, 30, &deep);
  const Arm gone = run(kBed + 6, kBed + 16, Vec3{}, idle, 150, &deep);
  const bool neckOk = neck.inLiquid && neck.swimWater && !neck.grounded &&
                      neck.sole > (float)kBed + 3.0f;
  const bool heldOk = held.swimWater && !held.grounded;
  const bool goneOk = !gone.swimWater && gone.grounded && !gone.swimming &&
                      std::fabs(gone.sole - (float)(kBed + 6)) < 0.6f;
  const bool ok = wading(waist) && wading(shoulder) && wading(bank) && neckOk &&
                  heldOk && goneOk;
  auto line = [](const char* n, const Arm& a) {
    return Format("%s: ground %d swim %d swimWater %d depth %.0f sole %.2f "
                  "afloat-ticks %d",
                  n, a.grounded ? 1 : 0, a.swimming ? 1 : 0, a.swimWater ? 1 : 0,
                  a.depth, a.sole, a.afloatTicks);
  };
  detail = line("waist", waist) + " | " + line("shoulder", shoulder) + " | " +
           line("bank walk-off", bank) + " | " + line("neck", neck) + " | " +
           line("bed to 13", held) + " | " + line("bed to 10", gone);
  std::printf("player swim-entry: %s (%s)\n", ok ? "PASS" : "FAIL",
              detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- player-swim-floor -------------------------------------------------
//
// A swimmer pressed into the lake bed keeps going and keeps its heading
// (player.cpp SwimSlide, 2026-10-10). Before it, an afloat body took the
// airborne slide: the box's SOLE caught each voxel step of the floor, that
// axis lost its velocity for a tick, travel snapped onto the other axis and
// the third-person body (which faces its travel) twitched toward it -- at a
// rate set by the swim angle to the grid -- and a rising bed stopped it dead.
//
// Fixture: water to y=160 over a bed that rises one voxel per six in x with a
// one-voxel checker of bumps on top. The stroke runs 15 degrees off +x (so it
// crosses both grid families) with the look pitched 0.4 rad down into the
// bed. The control is the same stroke in the same water with no bed in reach.
// Asserted: progress >= 60% of the control's, the per-tick travel direction
// never more than 20 degrees off the stroke, and the body ended up riding
// the bed rather than inside it.
Status GatePlayerSwimFloor(Ctx&, std::string& detail) {
  auto bedTop = [](int x, int z) {
    return 100 + std::max(0, x - 100) / 6 + (((x >> 1) + (z >> 1)) & 1);
  };
  auto bedKind = [&](IVec3 c) {
    if (c.y < bedTop(c.x, c.z)) return CellKind::Solid;
    return c.y < 160 ? CellKind::Liquid : CellKind::Air;
  };
  auto openKind = [](IVec3 c) {
    if (c.y < 0) return CellKind::Solid;
    return c.y < 160 ? CellKind::Liquid : CellKind::Air;
  };
  const float dt = 1.0f / 30.0f;
  const float yaw = 0.2618f, pitch = 0.4f;
  const Vec3 fwd{std::cos(yaw), 0, std::sin(yaw)};
  const Vec3 right{-fwd.z, 0, fwd.x};
  const Vec3 look{fwd.x * std::cos(pitch), -std::sin(pitch), fwd.z * std::cos(pitch)};

  struct Run {
    float dist = 0, worstDeg = 0, endSole = 0, endBed = 0;
    bool swam = true;
  };
  auto run = [&](const Player::KindFn& kindAt, const Vec3& lookFwd, float startY) {
    Player p;
    p.fly = false;
    p.pos = Vec3{100.5f, startY, 100.5f};
    TickInput in;
    in.forward = 1.0f;
    Run r;
    const Vec3 start = p.pos;
    for (int i = 0; i < 120; i++) {
      const Vec3 before = p.pos;
      p.Update(dt, in, fwd, right, lookFwd, kindAt);
      r.swam &= p.inLiquid;
      const float dx = p.pos.x - before.x, dz = p.pos.z - before.z;
      const float l = std::sqrt(dx * dx + dz * dz);
      if (i >= 10 && l > 0.02f) {
        const float c = std::clamp((dx * fwd.x + dz * fwd.z) / l, -1.0f, 1.0f);
        r.worstDeg = std::max(r.worstDeg, std::acos(c) * 57.2958f);
      }
    }
    r.dist = std::sqrt((p.pos.x - start.x) * (p.pos.x - start.x) +
                       (p.pos.z - start.z) * (p.pos.z - start.z));
    r.endSole = p.pos.y - Player::kHalfY;
    r.endBed = (float)bedTop((int)std::floor(p.pos.x), (int)std::floor(p.pos.z));
    return r;
  };
  // Start with the sole two voxels over the bed, so the stroke meets it at once.
  const Run bed = run(bedKind, look, 103.0f + Player::kHalfY);
  const Run open = run(openKind, fwd, 130.0f);
  const bool progress = open.dist > 1.0f && bed.dist >= 0.6f * open.dist;
  const bool steady = bed.worstDeg <= 20.0f;
  const bool riding = bed.endSole >= bed.endBed - 0.01f;
  const bool ok = progress && steady && riding && bed.swam;
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "bed %.1f vox vs open %.1f (%.0f%%), worst heading %.1f deg, "
                "end sole %.2f over bed %.0f, wet %d",
                bed.dist, open.dist, open.dist > 0 ? 100.0f * bed.dist / open.dist : 0.0f,
                bed.worstDeg, bed.endSole, bed.endBed, bed.swam ? 1 : 0);
  detail = buf;
  std::printf("player swim-floor: %s (%s)\n", ok ? "PASS" : "FAIL", buf);
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

  // (f) A SHIMMY ON A WALL GRABBED AT AN ANGLE travels both ways without
  // snapping back (2026-10-08). The grab used to keep the CAMERA facing as the
  // wall direction, so the slide ran diagonally: one way peeled the body off
  // the face until only a side probe column held the lip, the anchor took that
  // column's offset, and the settle dragged the body back a voxel -- forever,
  // in that direction only. Looking 30 degrees off the wall's normal, each
  // direction must cover real ground with no frame moving against the input.
  bool fLatched = false, fSquared = false;
  float fTravel[2] = {0, 0}, fWorstBack[2] = {0, 0};
  {
    Player p;
    p.fly = false;
    p.pos = Vec3{132.0f, 100.0f + Player::kHalfY, 200.5f};
    Player::KindFn kf = plateauKind;
    const Vec3 look = Vec3{0.866f, 0.0f, 0.5f};
    const Vec3 lookR{-look.z, 0.0f, look.x};
    TickInput in;
    in.SetHeld(TB_SPRINT, true);
    in.forward = 1.0f;
    int guard = 0;
    while (!(p.grounded && p.pos.x > 135.0f) && ++guard < 400)
      p.Update(dt, in, look, lookR, look, kf);
    in.SetPressed(TB_JUMP, true);
    in.SetHeld(TB_JUMP, true);
    p.Update(dt, in, look, lookR, look, kf);
    in.SetPressed(TB_JUMP, false);
    guard = 0;
    while (!p.hanging && ++guard < 400) p.Update(dt, in, look, lookR, look, kf);
    fLatched = p.hanging;
    fSquared = std::abs(p.hangDir.x - 1.0f) < 1e-3f && std::abs(p.hangDir.z) < 1e-3f;
    TickInput hold;
    hold.SetHeld(TB_JUMP, true);
    for (int i = 0; i < 20; i++) p.Update(dt, hold, look, lookR, look, kf);
    for (int dirI = 0; dirI < 2 && p.hanging; dirI++) {
      const float sgn = dirI == 0 ? 1.0f : -1.0f;
      TickInput sh = hold;
      sh.strafe = sgn;
      const float z0 = p.pos.z;
      for (int i = 0; i < 90 && p.hanging; i++) {
        const float zb = p.pos.z;
        p.Update(dt, sh, look, lookR, look, kf);
        fWorstBack[dirI] = std::max(fWorstBack[dirI], -(p.pos.z - zb) * sgn);
      }
      fTravel[dirI] = (p.pos.z - z0) * sgn;
    }
  }
  const bool fOk = fLatched && fSquared && fTravel[0] > 2.0f &&
                   fTravel[1] > 2.0f && fWorstBack[0] < 0.05f &&
                   fWorstBack[1] < 0.05f;
  std::printf("player ledgegrab (f) angled shimmy: latch=%d squared=%d "
              "travel %.2f / %.2f (want >2), worst back-step %.3f / %.3f "
              "(want <0.05)\n",
              fLatched ? 1 : 0, fSquared ? 1 : 0, fTravel[0], fTravel[1],
              fWorstBack[0], fWorstBack[1]);

  bool ok = aOk && bOk && cOk && dOk && eOk && fOk;
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

// ---- player-climb-fuzz ---------------------------------------------------
//
// THE CLIMB MUST NEVER END A TICK INSIDE THE WORLD (2026-10-09, owner report:
// "the climbing up animation can inject the player inside voxels and get them
// stuck inside a wall"). player-ledgegrab proves the climb WORKS on three clean
// walls; this proves it is SAFE on hundreds of rough ones. Every fixture is a
// static kindAt, so the world never moves into the body: any tick that ends
// with the live box overlapping a solid cell when the tick before did not is
// the controller putting it there, and the report names the state it was in.
//
// Fixtures (seeded, deterministic): the player-ledgegrab launch pad and pit,
// then a rough wall from x=150 -- a lip at a random height, per-column noise
// on the top, terraces stepping back, knobs on the face, sometimes a roof slab
// over the lip. Inputs: sprint, jump, then space held with W held, W pulsed
// (the cancel-and-resume path) or W plus a strafe (the shimmy) -- at the real
// 30 Hz tick.
//
// The controller sees the world through a model of the CPU mirror (a lagged
// 3x3x3 chunk cube, Unknown outside), because that is where the reported bug
// lived: the mirror's centre used a truncating divide, so at negative
// coordinates the cube sat a chunk off and the climb's step onto the lip
// crossed cells the mirror had not delivered -- read as air, solid a few
// ticks later. Half the seeds reproduce that off-by-one cube on purpose; the
// controller must hold rather than CLIMB into what it cannot see. Plain
// walking and jumping are not defended against a mis-centred mirror -- the
// centring is what defends them -- so on shifted seeds only an entry made
// while hanging or mantling counts; on centred seeds every entry does.
Status GatePlayerClimbFuzz(Ctx&, std::string& detail) {
  struct Rng {
    uint32_t s;
    uint32_t Next() {
      s ^= s << 13; s ^= s >> 17; s ^= s << 5;
      return s;
    }
    int Range(int lo, int hi) { return lo + (int)(Next() % (uint32_t)(hi - lo + 1)); }
  };
  auto cellHash = [](uint32_t seed, int x, int y, int z) {
    uint32_t h = seed * 0x9E3779B1u ^ (uint32_t)x * 0x85EBCA77u ^
                 (uint32_t)y * 0xC2B2AE3Du ^ (uint32_t)z * 0x27D4EB2Fu;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return h;
  };
  const int kSeeds = 400;
  const int kTicks = 240;
  const float dt = kTickDt;
  int climbs = 0, entered = 0, endedInside = 0, unseen[2] = {0, 0};
  int walkEntriesShifted = 0;  // reported, not asserted (see the header)
  std::string first;
  for (int seed = 1; seed <= kSeeds; seed++) {
    Rng r{(uint32_t)seed * 2654435761u | 1u};
    const int lipTop = r.Range(110, 118);
    const int noiseP = r.Range(0, 3);          // in 8ths: per-column top noise
    const int terraceAt = r.Range(1, 6);       // columns back to the next tier
    const int terraceUp = r.Range(0, 3) == 0 ? 0 : r.Range(1, 12);
    const bool roof = r.Range(0, 2) == 0;
    const int roofLo = lipTop + r.Range(4, 20);
    const int roofOver = r.Range(-2, 4);       // roof's front edge, from x=150
    const int knobP = r.Range(0, 2);           // in 8ths: knobs on the face
    const uint32_t hs = (uint32_t)seed;
    // Where the wall falls against the 16-cell chunk grid, so the mirror cube
    // below meets it at every alignment.
    const int ox = r.Range(0, 15);
    auto top = [=](int x, int z) {             // exclusive top of column x>=150
      int t = lipTop + (x >= 150 + terraceAt ? terraceUp : 0);
      if ((int)(cellHash(hs, x, 0, z) & 7u) < noiseP)
        t += (int)(cellHash(hs, x, 1, z) % 3u) - 1;
      return t;
    };
    Player::KindFn kf = [=](IVec3 c) {
      c.x -= ox;
      if (c.y < 60) return CellKind::Solid;
      if (c.x < 140) return c.y < 100 ? CellKind::Solid : CellKind::Air;
      if (roof && c.x >= 150 + roofOver && c.y >= roofLo && c.y < roofLo + 3)
        return CellKind::Solid;
      if (c.x >= 150) return c.y < top(c.x, c.z) ? CellKind::Solid : CellKind::Air;
      if (c.x == 149 && c.y > 100 && c.y < lipTop &&
          (int)(cellHash(hs, c.x, c.y, c.z) & 7u) < knobP)
        return CellKind::Solid;
      return CellKind::Air;
    };
    // THE CPU MIRROR, as the game has it: KindAt answers only inside a 3x3x3
    // chunk cube centred on where the body was World::kSnapshotLatency ticks
    // ago, Unknown past it. Odd seeds shift the cube one chunk back along x,
    // the geometry the truncating chunk divide in session.cpp produced at
    // negative coordinates: the body on the cube's +x edge, the wall it faces
    // a few cells past it. `truth` stays the oracle for "inside".
    const bool shifted = (seed & 1) != 0;
    std::vector<IVec3> chunkHist;
    IVec3 cubeLo{};
    Player::KindFn truth = kf;
    Player::KindFn mirror = [&cubeLo, truth](IVec3 c) {
      const IVec3 ch{c.x >> 4, c.y >> 4, c.z >> 4};
      if (ch.x < cubeLo.x || ch.y < cubeLo.y || ch.z < cubeLo.z ||
          ch.x > cubeLo.x + 2 || ch.y > cubeLo.y + 2 || ch.z > cubeLo.z + 2)
        return CellKind::Unknown;
      return truth(c);
    };
    auto inside = [&](const Player& p) {
      const Player::Box b = p.CurrentBox();
      const float hx = b.hx - 0.01f;
      for (int y = ifloor(p.pos.y + b.yLo + 0.01f); y <= ifloor(p.pos.y + b.yHi - 0.01f); y++)
        for (int z = ifloor(p.pos.z - hx); z <= ifloor(p.pos.z + hx); z++)
          for (int x = ifloor(p.pos.x - hx); x <= ifloor(p.pos.x + hx); x++)
            if (kf({x, y, z}) == CellKind::Solid) return true;
      return false;
    };

    const int mode = r.Range(0, 2);  // 0 hold W, 1 pulse W, 2 W + strafe
    const float strafe = r.Range(0, 1) ? 1.0f : -1.0f;
    const float zStart = 200.0f + (float)r.Range(0, 9) * 0.1f;
    Player p;
    p.fly = false;
    p.pos = Vec3{132.0f + (float)ox, 100.0f + Player::kHalfY, zStart};
    const Vec3 fwd{1, 0, 0}, right{0, 0, 1};
    bool jumped = false, climbed = false, wasInside = false, unseenThisSeed = false;
    for (int t = 0; t < kTicks; t++) {
      TickInput in;
      in.SetHeld(TB_SPRINT, !jumped);
      in.forward = 1.0f;
      if (!jumped) {
        if (p.grounded && p.pos.x > 135.0f + (float)ox) {
          in.SetPressed(TB_JUMP, true);
          in.SetHeld(TB_JUMP, true);
          jumped = true;
        }
      } else {
        in.SetHeld(TB_JUMP, true);
        if (mode == 1) in.forward = ((t / r.Range(3, 9)) & 1) ? 1.0f : 0.0f;
        if (mode == 2) in.strafe = strafe;
      }
      const bool hangB = p.hanging, mantleB = p.mantleTimer > 0.0f;
      const Vec3 before = p.pos;
      chunkHist.push_back({ifloor(p.pos.x) >> 4, ifloor(p.pos.y) >> 4, ifloor(p.pos.z) >> 4});
      const IVec3 seen = chunkHist[chunkHist.size() > World::kSnapshotLatency
                                       ? chunkHist.size() - 1 - World::kSnapshotLatency
                                       : 0];
      cubeLo = IVec3{seen.x - 1 - (shifted ? 1 : 0), seen.y - 1, seen.z - 1};
      // Did the mirror hide part of the spot a pull-up would stand on? That
      // is the situation the climb used to walk into rock from.
      if (p.hanging && !unseenThisSeed) {
        const Player::Box sb = p.CurrentBox();
        const Vec3 s = p.hangStand;
        for (int y = ifloor(s.y + sb.yLo + 0.01f); y <= ifloor(s.y + sb.yHi - 0.01f) && !unseenThisSeed; y++)
          for (int z = ifloor(s.z - sb.hx); z <= ifloor(s.z + sb.hx) && !unseenThisSeed; z++)
            for (int x = ifloor(s.x - sb.hx); x <= ifloor(s.x + sb.hx); x++)
              if (mirror({x, y, z}) == CellKind::Unknown) {
                unseenThisSeed = true;
                break;
              }
      }
      p.Update(dt, in, fwd, right, fwd, mirror);
      if (p.mantleFromHang && p.mantleTimer > 0.0f) climbed = true;
      const bool now = inside(p);
      const bool climbState = hangB || mantleB || p.hanging || p.mantleTimer > 0.0f;
      if (now && !wasInside && shifted && !climbState) walkEntriesShifted++;
      if (now && !wasInside && (!shifted || climbState)) {
        entered++;
        if (first.empty()) {
          char b[400];
          std::snprintf(b, sizeof(b),
                        "seed %d mode %d tick %d: (%.2f,%.2f,%.2f)->(%.2f,%.2f,%.2f) "
                        "hang %d->%d mantle %d->%d fromHang %d clock %.2f lip (%d,%d,%d) "
                        "stand (%.2f,%.2f,%.2f) | lipTop %d terrace +%d@%d roof %d@%d over %d "
                        "noise %d knob %d",
                        seed, mode, t, before.x, before.y, before.z, p.pos.x, p.pos.y,
                        p.pos.z, hangB, p.hanging, mantleB, p.mantleTimer > 0.0f,
                        p.mantleFromHang, p.climbClock, p.hangLip.x, p.hangLip.y,
                        p.hangLip.z, p.hangStand.x, p.hangStand.y, p.hangStand.z, lipTop,
                        terraceUp, terraceAt, roof, roofLo, roofOver, noiseP, knobP);
          first = b;
        }
      }
      wasInside = now;
    }
    if (climbed) climbs++;
    if (wasInside && !shifted) endedInside++;
    if (unseenThisSeed) unseen[shifted ? 1 : 0]++;
  }
  char buf[700];
  std::snprintf(buf, sizeof(buf),
                "%d fixtures, %d climbed; %d entries into solid, %d ended inside; "
                "stand spot partly unseen by the mirror in %d centred / %d shifted; "
                "%d walk entries on shifted seeds (not asserted)%s%s",
                kSeeds, climbs, entered, endedInside, unseen[0], unseen[1],
                walkEntriesShifted,
                first.empty() ? "" : " | first: ", first.c_str());
  detail = buf;
  // A fuzz that never climbs proves nothing: most fixtures must reach the climb.
  // And the shifted cube must actually hide stand spots, or the mirror half
  // of this gate is not exercising the case it exists for.
  const bool ok = entered == 0 && climbs >= kSeeds / 4 && unseen[1] > 0;
  std::printf("player climb-fuzz: %s (%s)\n", ok ? "PASS" : "FAIL", buf);
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
// Five claims, and the first one is the reason the box exists: a corridor
// exactly as tall as the STANDING box admits the body even though the
// 17-row figure would not fit. Then: a corridor as tall as the CROUCH box
// refuses the standing body, admits it crouched (a Ctrl TAP latches the
// crouch), keeps it crouched when a second tap asks to stand under the
// ceiling, and stands it up once it walks out. Then the crouch walk is slower
// by crouchSpeedScale, and the eye drops with the box and never rises above
// it. Last, THE STANCE MACHINE (Player::UpdateStance, 2026-10-12): a HOLD of
// proneHoldTime goes prone, the prone box fits a corridor the crouch box does
// not, a tap rises one stage (to the crouch) only where it fits, and Space
// stands a prone body up instead of jumping it.
Status GatePlayerCrouch(Ctx&, std::string& detail) {
  const Tuning::Player& tp = CurrentTuning().player;
  const int standRows = (int)std::ceil(tp.collisionHeight / kVoxelMeters - 1e-3f);
  const int crouchRows = (int)std::ceil(tp.crouchHeight / kVoxelMeters - 1e-3f);
  const int proneRows = (int)std::ceil(tp.proneHeight / kVoxelMeters - 1e-3f);
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
  // A Ctrl TAP: one frame down (the press edge), released the next.
  auto tap = [&](Player& p, const Player::KindFn& k, TickInput in) {
    TickInput down = in;
    down.SetHeld(TB_CROUCH, true);
    down.SetPressed(TB_CROUCH, true);
    frames(p, k, down, 1);
    frames(p, k, in, 1);
  };
  // A Ctrl HOLD of `n` frames, then released.
  auto hold = [&](Player& p, const Player::KindFn& k, TickInput in, int n) {
    TickInput down = in;
    down.SetHeld(TB_CROUCH, true);
    down.SetPressed(TB_CROUCH, true);
    frames(p, k, down, 1);
    down.SetPressed(TB_CROUCH, false);
    frames(p, k, down, n - 1);
    frames(p, k, in, 1);
  };
  TickInput walk;
  walk.forward = 1.0f;
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
  tap(b, low, walk);
  frames(b, low, walk, 300);
  const float bInX = b.pos.x;
  const bool bAdmitted = b.crouching && !b.prone && bInX > 156.0f;
  tap(b, low, rest);  // ask to stand: the ceiling is still there
  frames(b, low, rest, 30);
  const bool bStuck = b.crouching;
  frames(b, low, back, 420);
  const bool bStood = !b.crouching && b.pos.x < 148.0f;

  // (c) crouch speed on open floor: distance over the same window.
  const Player::KindFn open = corridor(64);
  Player c1 = make(), c2 = make();
  frames(c1, open, walk, 90);
  const float x1 = c1.pos.x;
  frames(c1, open, walk, 60);
  const float standDist = c1.pos.x - x1;
  frames(c2, open, rest, 2);
  tap(c2, open, walk);
  frames(c2, open, walk, 86);
  const float x2 = c2.pos.x;
  frames(c2, open, walk, 60);
  const float crouchDist = c2.pos.x - x2;
  const float ratio = standDist > 1e-3f ? crouchDist / standDist : -1.0f;
  const bool cOk = std::abs(ratio - tp.crouchSpeedScale) < 0.1f;

  // (d) the eye drops with the box and stays inside it.
  const float eyeStand = c1.EyePos().y - (c1.pos.y - Player::kHalfY);
  const float eyeCrouch = c2.EyePos().y - (c2.pos.y - Player::kHalfY);
  const bool dOk = c2.crouching && eyeStand - eyeCrouch > 1.0f &&
                   eyeCrouch < c2.BoxHeight() && eyeStand < c1.BoxHeight();

  // (e) the stance machine. A hold released before proneHoldTime leaves the
  // crouch the press took (it was not a tap from the crouch, so nothing
  // toggles back); the next tap stands.
  const int holdFrames = (int)std::ceil(tp.proneHoldTime / dt);
  Player e = make();
  frames(e, open, rest, 30);
  hold(e, open, rest, std::max(2, holdFrames / 2));
  const bool eShortCrouch = e.crouching && !e.prone;
  tap(e, open, rest);
  const bool eTapStood = !e.crouching && !e.prone;
  // ...a full hold goes prone, with the prone box...
  hold(e, open, rest, holdFrames + 6);
  const bool eProne = e.prone && !e.crouching &&
                      std::fabs(e.BoxHeight() - (float)proneRows) < 1.01f;
  // ...a prone body gets under a ceiling the crouch box will not fit...
  const Player::KindFn flat = corridor(std::max(proneRows, crouchRows - 3));
  Player f = make();
  frames(f, flat, rest, 30);
  hold(f, flat, rest, holdFrames + 6);
  frames(f, flat, walk, 900);
  const bool fUnder = f.prone && f.pos.x > 156.0f;
  // ...a tap under it does not rise (the crouch box is refused there)...
  tap(f, flat, rest);
  frames(f, flat, rest, 10);
  const bool fStaysDown = f.prone;
  // ...and once back out on open floor it finishes rising to the crouch it
  // was asked for. Space from prone stands up and does NOT jump.
  frames(f, flat, back, 900);
  const bool fRoseToCrouch = f.crouching && !f.prone && f.pos.x < 148.0f;
  Player g = make();
  frames(g, open, rest, 30);
  hold(g, open, rest, holdFrames + 6);
  const bool gWasProne = g.prone;
  TickInput jump = rest;
  jump.SetHeld(TB_JUMP, true);
  jump.SetPressed(TB_JUMP, true);
  const float gY = g.pos.y;
  frames(g, open, jump, 1);
  frames(g, open, rest, 20);
  const bool gStood = gWasProne && !g.prone && !g.crouching &&
                      std::fabs(g.pos.y - gY) < 0.5f;
  const bool eOk = eShortCrouch && eTapStood && eProne && fUnder &&
                   fStaysDown && fRoseToCrouch && gStood;

  const bool ok = aIn && aTighterThanFigure && bRefused && bAdmitted &&
                  bStuck && bStood && cOk && dOk && eOk;
  char buf[640];
  std::snprintf(buf, sizeof(buf),
                "rows stand=%d crouch=%d prone=%d figure=%d; tall corridor "
                "x=%.1f (want>156); low: standing x=%.1f (want<150) crouched "
                "x=%.1f (want>156) stuck=%d stood=%d x=%.1f (want<148); speed "
                "ratio %.2f (want %.2f); eye stand=%.1f crouch=%.1f; stance: "
                "short-hold crouch=%d tap-stand=%d prone=%d (box %.1f) "
                "under=%d x=%.1f stays-down=%d rose-to-crouch=%d space-stood=%d",
                standRows, crouchRows, proneRows, figureRows, a.pos.x,
                bBlockedX, bInX, bStuck ? 1 : 0, bStood ? 1 : 0, b.pos.x, ratio,
                tp.crouchSpeedScale, eyeStand, eyeCrouch, eShortCrouch ? 1 : 0,
                eTapStood ? 1 : 0, eProne ? 1 : 0, e.BoxHeight(),
                fUnder ? 1 : 0, f.pos.x, fStaysDown ? 1 : 0,
                fRoseToCrouch ? 1 : 0, gStood ? 1 : 0);
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
  // Since 2026-10-12 the press EDGE is what crouches and the hold, kept up
  // past player.proneHoldTime, goes prone — so this exercises both.
  in.SetHeld(TB_CROUCH, firstTick >= 240 && firstTick < 280);
  if (firstTick == 240) edges |= TB_CROUCH;
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

// ---- freefall --------------------------------------------------------------
//
// A 150 M FALL IS ONE CONTINUOUS MOTION, ON THE REAL TICK.
//
// Reported 2026-10-08: a body in freefall stutters -- it glides, jerks back,
// glides -- and the game lags while it falls. `player-fastfall` cannot see it:
// it runs Player::Update against a synthetic KindFn, so it has no snapshot
// latency the tick did not script, no streaming, no avatar and no pose. This
// gate drops the REAL player -- non-fly, avatar spawned, CPU mirror centred on
// the body the way the game does it, residency window FOLLOWING the body
// (TickAuthorityCtx::Harness::pinWindow off) -- from 150 m above the harness
// map's ground, through TickAuthority, and watches every tick of it.
//
// WHAT "CLEAN" MEANS, each its own claim and each attributed to the tick and
// the state it failed in (CLAUDE.md rule 6):
//   (A) THE BODY'S FALL IS GRAVITY. The per-tick drop grows by g*dt^2 a tick
//       until maxFall clamps it, then stays put. A tick whose drop differs
//       from that by more than `freefall.maxDropJerkVox` is a stutter.
//       `Player::blindFall` (a descent held by a mirror the body outran,
//       KnownDrop) is counted separately and must be ZERO: it is the named
//       cause, so it is reported by name.
//   (B) THE DRAWN BODY NEVER GOES BACK UP. Sampled per FRAME (four a tick,
//       the alpha sweep the frame loop does), the rendered eye and body must
//       descend monotonically while airborne.
//   (C) THE AVATAR RIDES THE BODY. The hips' physics transform relative to
//       the player position must not jump tick to tick by more than
//       `freefall.maxRigSlipVox`, and must never move UP mid-fall -- a rig
//       left behind by its driver and snapped back is the "glitching back" a
//       camera sees.
//   (D) THE ANIMATION IS ONE FALL. The air pose weight, once in, stays in
//       until touchdown (no flicker out and back), and the loco state does not
//       churn mid-air.
//   (E) IT LANDS. Grounded, on the surface, inside the tick budget.
// It also REPORTS the tick wall time (max / p50) and the window shifts -- the
// "lag" half of the report; the bound is `freefall.maxTickMs`.
// SANDVOX_FREEFALL_DUMP=1 prints every tick's row.
constexpr int kFreefallDropVox = 1500;     // 150 m at kVoxelMeters 0.10
constexpr int kFreefallMaxTicks = 900;     // 30 s; the fall takes ~6
constexpr int kFreefallFrames = 4;         // frames per tick for (B)

Status GateFreefall(Ctx& c, std::string& detail) {
  const IVec3 savedOrigin = c.world.WindowOrigin();
  const float terminal = CurrentTuning().player.maxFall / kVoxelMeters;
  const float g = CurrentTuning().player.gravity / kVoxelMeters;

  // The site: the flattest patch near the window centre, so the landing claim
  // is about the fall and not about an overhang TerrainHeight cannot see.
  const int cx = (savedOrigin.x + (int)kNChunk / 2) * (int)kChunk;
  const int cz = (savedOrigin.z + (int)kNChunk / 2) * (int)kChunk;
  int relief = 0;
  const IVec3 site = TwoPlayerFlatSpot(cx, cz, 48, kDefaultSeed, relief);
  const float startY = (float)(site.y + kFreefallDropVox) + Player::kHalfY;
  const IVec3 startChunk{site.x >> 4, ifloor(startY) >> 4, site.z >> 4};

  c.debris.Reset();
  c.debris.ResetUntunnelProbe();
  c.mobs.Reset(true);
  c.mobs.ClearRisings();
  // The window starts CENTRED on the body, 150 m up, the way it is after a
  // player has flown there: what follows is the streaming a real fall drives.
  c.world.SetWindowOrigin({startChunk.x - (int)kNChunk / 2,
                           startChunk.y - (int)kNChunk / 2,
                           startChunk.z - (int)kNChunk / 2});
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  struct TickRow {
    float y, vy, drop, hipsRel, hipsDy, air, ms;
    int blind, grounded, loco, winY, mirY, limp;
    // Chunks between the body and the nearest window face on Y: the window
    // the sim, the colliders and the mirror all live in.
    int winMargin;
    // The chunk one below the body's: what the ragdoll's anti-tunnel clamp
    // waits on (DebrisSystem::ColliderVouched). held = rig-steps clamped this
    // tick; built = its patch's mirror version (0 = never polygonized).
    int held, belowCached, belowSky;
    uint32_t belowBuilt;
    // The body's lowest point above the site's ground, in voxels: the sole
    // while live, the pelvis less a margin while limp (the capsule follows the
    // pelvis then, RagdollFollow).
    float gap;
    // The below chunk's NeedRec (DebrisSystem::NeedProbe), as ticks RELATIVE
    // to this one (-1 = never): listed, requested, cached, built; and the
    // pass-over counts.
    std::string need;
  };
  std::vector<TickRow> rows;
  c.debris.EnableNeedProbe(true);
  std::vector<float> frameEye, frameBody;   // per frame, airborne only
  bool spawned = false;
  int landTick = -1;
  float landFeet = 0.0f;
  // THE CROSSING (CLAUDE.md rule 6): the first tick the body is below the
  // ground it should have landed on, and what every collision source said
  // about that ground at that moment.
  std::string crossing;
  {
    support::TickRig rig(c, 91000u, startChunk);
    // THE GAME'S ARRANGEMENT, not the harness's: the window follows the body
    // and the mirror is centred on it (session.cpp phase L's `pc`). The far
    // field stays inert (Harness::farInert): a rig never Init's one.
    rig.Authority().harness.pinWindow = false;
    rig.Authority().harness.haveSubmitChunk = false;
    PlayerSession& s = rig.Session();
    s.avatar.Init(&c.phys, &c.world, &c.debris, c.mats, &c.mobs);
    s.avatar.SetDefs(&c.mobs.Defs(), kAvatarDefName);
    c.mobs.SetAvatar(&s.avatar);
    s.player.fly = false;
    s.player.grounded = false;
    s.player.vel = Vec3{0, 0, 0};
    s.player.pos = Vec3{(float)site.x + 0.5f, startY, (float)site.z + 0.5f};
    s.player.SnapRender();
    const int hips = s.avatar.PartIndex("hips");

    float prevY = s.player.pos.y, prevHipsY = 0.0f;
    uint32_t prevRigHolds = c.debris.Untunnel().rigHolds;
    bool haveHips = false;
    for (int i = 0; i < kFreefallMaxTicks; i++) {
      const double t0 = NowSeconds();
      support::RunTicks(rig, 1);
      const double ms = (NowSeconds() - t0) * 1000.0;
      const Player& p = s.player;
      spawned = spawned || s.avatar.Spawned();
      TickRow r{};
      r.y = p.pos.y;
      r.vy = p.vel.y;
      r.drop = prevY - p.pos.y;
      r.blind = p.blindFall ? 1 : 0;
      r.grounded = p.grounded ? 1 : 0;
      r.ms = (float)ms;
      r.winY = c.world.WindowOrigin().y;
      r.mirY = c.world.Snap().valid ? c.world.Snap().mirrorBase.y : -99999;
      r.limp = s.avatar.Spawned() && s.avatar.Ragdolled() ? 1 : 0;
      r.gap = (r.limp ? p.pos.y - 4.0f : p.pos.y + p.CurrentBox().yLo) - (float)site.y;
      {
        const uint32_t h = c.debris.Untunnel().rigHolds;
        r.held = (int)(h - prevRigHolds);
        prevRigHolds = h;
        const IVec3 bw{ifloor(p.pos.x) >> 4, (ifloor(p.pos.y) >> 4) - 1, ifloor(p.pos.z) >> 4};
        r.belowBuilt = c.debris.TerrainBuiltVersion(bw);
        r.belowCached = c.world.Cached(bw) != nullptr ? 1 : 0;
        r.belowSky = c.debris.SkyKnown(bw) ? 1 : 0;
        if (const DebrisSystem::NeedRec* nr = c.debris.NeedProbe(bw)) {
          const uint32_t now = rig.tick;
          auto ago = [&](uint32_t t) { return t == 0 ? -1 : (int)(now - t); };
          r.need = Format("need -%d rank %u req -%d cached -%d built -%d | ceil %u budget %u "
                          "sky %u gather %u build %u",
                          ago(nr->firstNeeded), nr->firstRank, ago(nr->firstRequested),
                          ago(nr->firstCached), ago(nr->firstBuilt), nr->ceilingCut,
                          nr->budgetDenied, nr->skyskipped, nr->gatherDeferred,
                          nr->buildDeferred);
        } else {
          r.need = Format("never listed (%d,%d,%d) pos (%.1f,%.1f,%.1f) map %zu", bw.x, bw.y,
                          bw.z, p.pos.x, p.pos.y, p.pos.z, c.debris.NeedProbeSize());
        }
        for (const auto& an : c.debris.ProbeAnchors())
          r.need += Format(" | anchor (%.0f,%.0f,%.0f) v (%.0f,%.0f,%.0f)", an.first.x,
                           an.first.y, an.first.z, an.second.x, an.second.y, an.second.z);
      }
      {
        const int bc = ifloor(p.pos.y) >> 4;
        r.winMargin = std::min(bc - r.winY, r.winY + (int)kNChunk - 1 - bc);
      }
      r.loco = s.avatar.Spawned() ? s.avatar.LocoState() : -1;
      r.air = s.avatar.Spawned() ? s.avatar.AirPoseWeight() : 0.0f;
      Vec3 hp{};
      Quat hq{};
      if (hips >= 0 && s.avatar.Spawned() &&
          s.avatar.PartWorldTransform(hips, hp, hq)) {
        r.hipsRel = hp.y - p.pos.y;
        r.hipsDy = haveHips ? hp.y - prevHipsY : 0.0f;
        prevHipsY = hp.y;
        haveHips = true;
      }
      rows.push_back(r);
      if (crossing.empty() && p.pos.y + p.CurrentBox().yLo < (float)site.y - 2.0f) {
        const IVec3 gc{site.x >> 4, site.y >> 4, site.z >> 4};
        const uint64_t hb = hips >= 0 && s.avatar.Spawned() ? s.avatar.PartBody(hips) : 0;
        int solidCol = 0, unknownCol = 0;
        for (int y = site.y - 4; y <= site.y + 4; y++) {
          const CellKind k = s.kindAt({site.x, y, site.z});
          solidCol += k == CellKind::Solid ? 1 : 0;
          unknownCol += k == CellKind::Unknown ? 1 : 0;
        }
        crossing = Format(
            "crossed ground at tick %d: limp %d, ground chunk (%d,%d,%d) patch version %u "
            "vouched %d (chunk below: %u), mirror column y%d..%d: %d solid %d unknown, "
            "hips body %llu linearCast %d",
            i, r.limp, gc.x, gc.y, gc.z, c.debris.TerrainBuiltVersion(gc),
            c.debris.ColliderVouched(gc) ? 1 : 0,
            c.debris.TerrainBuiltVersion({gc.x, gc.y - 1, gc.z}), site.y - 4, site.y + 4,
            solidCol, unknownCol, (unsigned long long)hb,
            hb != 0 && c.phys.UsesLinearCast(hb) ? 1 : 0);
      }
      prevY = p.pos.y;
      if (!p.grounded)
        for (int f = 0; f < kFreefallFrames; f++) {
          const float a = (float)f / (float)kFreefallFrames;
          frameEye.push_back(p.RenderEyePos(a).y);
          frameBody.push_back(p.pos.y + p.RenderBodyOffset(a).y);
        }
      if (p.grounded && landTick < 0) {
        landTick = i;
        landFeet = p.pos.y + p.CurrentBox().yLo;
      }
      if (landTick >= 0 && i - landTick >= 15) break;
    }
    c.mobs.SetAvatar(nullptr);
    if (s.avatar.Spawned()) s.avatar.Despawn();
  }
  c.debris.EnableNeedProbe(false);
  const bool landed = landTick >= 0;
  // Put the window back and regenerate: every gate after this in kOrder
  // assumes worldgen it did not move (selftest.h's ordering note).
  c.world.SetWindowOrigin(savedOrigin);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  // ---- the analysis --------------------------------------------------------
  // The fall proper: from the first tick that moved to the tick before the
  // landing. Ticks before that are the body waiting for a first snapshot.
  int first = -1;
  for (size_t i = 0; i < rows.size(); i++)
    if (rows[i].drop > 1e-3f) { first = (int)i; break; }
  // ...to the tick before IMPACT: the first tick the body could not fall its
  // full step because the ground was in the way. What follows -- the limp
  // body's bounce, the get-up, the landing settle -- is a collision, not a
  // fall, and the claims below are about the fall.
  int impact = -1;
  if (first >= 0)
    for (int i = first + 1; i < (int)rows.size(); i++)
      if (rows[i].gap < rows[i - 1].drop + 1.0f) { impact = i; break; }
  const int last = impact >= 0 ? impact - 1 : landed ? landTick - 1 : (int)rows.size() - 1;
  // The drawn frames are sampled only while airborne and in tick order; cut
  // them at the same tick.
  {
    size_t keep = 0;
    for (int i = 0; i <= last && i < (int)rows.size(); i++)
      if (!rows[i].grounded) keep += kFreefallFrames;
    if (keep < frameEye.size()) { frameEye.resize(keep); frameBody.resize(keep); }
  }
  const float gStep = g * kTickDt * kTickDt;
  float worstJerk = 0.0f, peakDrop = 0.0f, worstSlip = 0.0f, worstHipsUp = 0.0f;
  int jerkAt = -1, slipAt = -1, hipsUpAt = -1, blindTicks = 0, firstBlind = -1;
  int airDrops = 0, locoChanges = 0, limpAt = -1, minMargin = 1 << 20, marginAt = -1;
  float airPeak = 0.0f;
  if (first >= 0)
    for (int i = first + 1; i <= last; i++) {
      const TickRow& r = rows[i];
      const TickRow& q = rows[i - 1];
      peakDrop = std::max(peakDrop, r.drop);
      // Expected: d_t = d_{t-1} + g dt^2, capped at terminal * dt while the
      // controller owns the body. A LIMP body (ragdoll.fallSeconds into the
      // fall the avatar goes limp and the capsule follows the pelvis) is
      // Jolt's, with no maxFall: only the gravity step is expected of it. The
      // tick it goes limp is a hand-over and is measured like any other.
      const float want = r.limp ? q.drop + gStep
                                : std::min(q.drop + gStep, terminal * kTickDt);
      const float jerk = std::abs(r.drop - want);
      if (jerk > worstJerk) { worstJerk = jerk; jerkAt = i; }
      if (r.blind) { blindTicks++; if (firstBlind < 0) firstBlind = i; }
      const float slip = std::abs(r.hipsRel - q.hipsRel);
      if (i > first + 1 && slip > worstSlip) { worstSlip = slip; slipAt = i; }
      if (r.hipsDy > worstHipsUp) { worstHipsUp = r.hipsDy; hipsUpAt = i; }
      // The air pose is the LIVE body's; a limp one is the ragdoll's.
      if (!r.limp && r.air < airPeak - 0.05f && airPeak > 0.9f) airDrops++;
      if (r.limp && limpAt < 0) limpAt = i;
      if (r.winMargin < minMargin) { minMargin = r.winMargin; marginAt = i; }
      airPeak = std::max(airPeak, r.air);
      if (r.loco != q.loco && i > first + 5 && !r.limp) locoChanges++;
    }
  float worstFrameUpEye = 0.0f, worstFrameUpBody = 0.0f;
  for (size_t i = 1; i < frameEye.size(); i++) {
    worstFrameUpEye = std::max(worstFrameUpEye, frameEye[i] - frameEye[i - 1]);
    worstFrameUpBody = std::max(worstFrameUpBody, frameBody[i] - frameBody[i - 1]);
  }
  std::vector<float> ms;
  for (const TickRow& r : rows) ms.push_back(r.ms);
  std::sort(ms.begin(), ms.end());
  const float msMax = ms.empty() ? 0.0f : ms.back();
  const float msP50 = ms.empty() ? 0.0f : ms[ms.size() / 2];
  const int winShift = rows.empty() ? 0 : rows.front().winY - rows.back().winY;

  const double jerkAllow = BaselineNumber("freefall.maxDropJerkVox", 0.25);
  const double slipAllow = BaselineNumber("freefall.maxRigSlipVox", 1.0);
  const double upAllow = BaselineNumber("freefall.maxFrameRiseVox", 0.01);
  const double msAllow = BaselineNumber("freefall.maxTickMs", 1.0e9);
  RecordObserved("freefall.maxDropJerkVox", (double)worstJerk);
  RecordObserved("freefall.blindTicks", (double)blindTicks);
  RecordObserved("freefall.maxRigSlipVox", (double)worstSlip);
  RecordObserved("freefall.maxFrameRiseVox",
                 (double)std::max(worstFrameUpEye, worstFrameUpBody));
  RecordObserved("freefall.maxTickMs", (double)msMax);
  RecordObserved("freefall.p50TickMs", (double)msP50);

  std::string fails;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) fails += (fails.empty() ? "" : "; ") + what;
  };
  auto rowAt = [&](int i) -> std::string {
    if (i < 0 || i >= (int)rows.size()) return "-";
    const TickRow& r = rows[i];
    return Format("tick %d: y %.1f vy %.1f drop %.2f blind %d gnd %d limp %d held %d "
                  "[below: built %u cached %d sky %d] win.y %d mirror.y %d (body chunk %d) "
                  "hipsRel %.2f hipsDy %.2f air %.2f loco %d %.1f ms {%s}",
                  i, r.y, r.vy, r.drop, r.blind, r.grounded, r.limp, r.held, r.belowBuilt,
                  r.belowCached, r.belowSky, r.winY, r.mirY, ifloor(r.y) >> 4, r.hipsRel,
                  r.hipsDy, r.air, r.loco, r.ms, r.need.c_str());
  };
  check(spawned, "the avatar never spawned");
  check(first >= 0, "the body never moved");
  // 0.9, not ~1: the avatar goes limp (ragdoll.fallSeconds) a little before
  // the live body reaches maxFall, and the limp rig then accelerates to about
  // 0.92 of it before the ground. What this guards is "a long fall", not a cap.
  check(peakDrop > 0.9f * terminal * kTickDt,
        Format("peak drop %.2f vox/tick never reached terminal %.2f: not a long fall",
               peakDrop, terminal * kTickDt));
  check(landed, Format("never landed in %d ticks (%s)", (int)rows.size(),
                       rowAt((int)rows.size() - 1).c_str()));
  check(!landed || std::abs(landFeet - (float)site.y) < 6.0f,
        Format("landed with feet at %.2f, ground %d (relief %d)", landFeet, site.y, relief));
  check(blindTicks == 0, Format("%d ticks held by a starved mirror (Player::blindFall), "
                                "first [%s]", blindTicks, rowAt(firstBlind).c_str()));
  check(worstJerk <= (float)jerkAllow,
        Format("drop jerk %.2f vox (allow %.2f) at [%s] after [%s]", worstJerk, jerkAllow,
               rowAt(jerkAt).c_str(), rowAt(jerkAt - 1).c_str()));
  check(worstFrameUpEye <= (float)upAllow && worstFrameUpBody <= (float)upAllow,
        Format("a drawn frame went UP while airborne: eye %.3f body %.3f vox (allow %.3f)",
               worstFrameUpEye, worstFrameUpBody, upAllow));
  check(worstSlip <= (float)slipAllow,
        Format("hips slipped %.2f vox against the body in one tick (allow %.2f) at [%s]",
               worstSlip, slipAllow, rowAt(slipAt).c_str()));
  check(worstHipsUp <= 0.5f, Format("hips moved UP %.2f vox in a tick mid-fall at [%s]",
                                    worstHipsUp, rowAt(hipsUpAt).c_str()));
  check(airDrops == 0, Format("air pose flickered out %d times mid-fall", airDrops));
  check(locoChanges <= 1, Format("loco state changed %d times mid-fall", locoChanges));
  check(msMax <= (float)msAllow, Format("tick took %.1f ms (allow %.1f)", msMax, msAllow));
  // THE BODY STAYS IN THE WINDOW. Past its faces nothing is simulated, the
  // mirror cannot be centred and every collider vouches: a body that leaves
  // it falls through the world (Stream kCatchUpLag).
  check(minMargin >= 2, Format("body came within %d chunks of the window face at [%s]",
                               minMargin, rowAt(marginAt).c_str()));
  const auto& ut = c.debris.Untunnel();
  RecordObserved("freefall.rigHolds", (double)ut.rigHolds);
  check(ut.released == 0, Format("%u bodies released into unvouched space", ut.released));

  detail = Format("%d vox drop over %d ticks (landed tick %d, feet %.1f / ground %d), "
                  "peak %.2f vox/tick of %.2f, jerk %.3f, blind %d, frame rise %.3f, "
                  "rig slip %.2f, air peak %.2f, window shifted %d chunks, "
                  "tick %.1f ms p50 / %.1f max, limp at tick %d, impact tick %d, min window margin %d, "
                  "untunnel holds %u rig %u released %u",
                  kFreefallDropVox, (int)rows.size(), landTick, landFeet, site.y, peakDrop,
                  terminal * kTickDt, worstJerk, blindTicks,
                  std::max(worstFrameUpEye, worstFrameUpBody), worstSlip, airPeak, winShift,
                  msP50, msMax, limpAt, impact, minMargin, ut.holds, ut.rigHolds, ut.released);
  if (!crossing.empty()) detail += " | " + crossing;
  if (!fails.empty()) detail += " | " + fails;
  const bool ok = fails.empty();
  std::printf("freefall: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  if (std::getenv("SANDVOX_FREEFALL_DUMP")) {
    for (size_t i = 0; i < rows.size(); i++) std::printf("  ff %s\n", rowAt((int)i).c_str());
    std::printf("  ff %s\n", c.world.FetchReport().c_str());
  }
  return ok ? Status::Pass : Status::Fail;
}


// ---- freefall-slope ----------------------------------------------------------
//
// A LIMP BODY SKIPPING DOWN A MOUNTAINSIDE (bug report 2026-10-09_15-16-57).
//
// The owner jumped off a ~2 km mountain on the default map: the fall was
// smooth while it touched nothing, glitchy while the body grazed and skipped
// down the face, and SOMETIMES THE BODY HIT NOTHING IN THE AIR AND BURST.
// `freefall` drops onto flat ground through open sky; this drops the real
// player (non-fly, avatar spawned, window and mirror following the body, the
// same arrangement) beside the `peaks` map's 300 m cliff band
// (scripts/gen_map_peaks.py: a smoothstep over 420 voxels, ~10 voxels of rise
// per voxel at its steepest) with a little speed INTO the face, so the limp rig
// meets the face partway down and slides and tumbles the rest of it -- the
// window shifting on two axes the whole way, which is what puts patches at its
// faces.
//
// Claims, each attributed to the tick it failed on (CLAUDE.md rule 6):
//   (A) NO RIG HOLDS. UntunnelRig holds a rig whose step ends in a chunk with
//       no collider yet; a held rig hangs in the air for a tick. None on a
//       chunk the lookahead listed at least kSnapshotLatency + 1 ticks ahead
//       (a budget failure); a hold on a chunk listed later than that (the rig
//       bounced off the face into a chunk nothing pointed at) is bounded by
//       `freefallSlope.maxLateHolds` -- only the fetch round trip separates
//       that one from its patch.
//   (B) NOTHING RELEASED into unvouched space.
//   (C) NO ARREST IN OPEN AIR. A tick in which the pelvis lost more than
//       `freefallSlope.airArrestMs` m/s (beyond gravity) while the nearest
//       landform cell (World::TerrainHeight over a grid round it) is more than
//       `freefallSlope.airClearVox` voxels away is a collision with nothing.
//       Each is reported with what the colliders round the pelvis were built
//       on (TerrainEntry outMask = a neighbour that was outside the window).
//   (D) IT GETS DOWN: the body descends at least half the face.
//   (E) NO LINEAR RUNAWAY-NET ACTION. Physics::SweepRunawayRigs slows a
//       jointed body that sits at 90% of its velocity ceiling; a rig that is
//       merely FALLING fast must never trip it (its limbs damped one at a time
//       read as a 27 m/s arrest in open air and splatted the body there).
//       Trips at the ANGULAR bar (a limb the solver keeps at 47.1 rad/s after
//       an impact -- the net's own case) are recorded with the part, not
//       failed: `spinDamps` in the report.
//
// TWO ARMS (round 2 of the ticket, 2026-10-11). The first round's single arm
// started 60 m above the face's middle and met it at ~45 m/s; the owner's real
// mountain (`default`, 2 km) is a different regime, reproduced with
// SANDVOX_FREEFALL_SITE below: the rig reached 75 m/s in open air, the runaway
// net fired on every limb and the body burst with "manifolds static 0 dyn 0",
// and grazing the face at 60 m/s it was held 22 ticks waiting for patches.
//   graze:    the round-1 arm (start 60 m above the face's middle).
//   terminal: 300 m higher, barely drifting toward the face, so the rig is at
//             terminal velocity long before it meets it. Adds (F): the limp
//             rig's speed stays within `freefallSlope.terminalSlack` of
//             player.maxFall (Mob::ApplyRigAirDrag -- the controller's own
//             terminal velocity, so going limp does not speed the fall up).
// SANDVOX_FREEFALL_DUMP=1 prints every tick's row.
constexpr int kSlopeMaxTicks = 900;  // 30 s at 30 Hz

Status GateFreefallSlope(Ctx& c, std::string& detail) {
  const std::string dir = AssetDir();
  auto fail = [&](const std::string& why) {
    detail = why;
    std::printf("freefall-slope: FAIL (%s)\n", why.c_str());
    return Status::Fail;
  };
  biomes::BiomeSet set;
  std::string log;
  if (!biomes::LoadBiomeSet(dir, c.mats, set, log)) return fail("biome files did not load: " + log);
  TreeAtlas atlas;
  if (!LoadTreeAtlas(dir + "/trees", c.mats, set, atlas, log))
    return fail("tree atlas did not load: " + log);
  const worldmap::WorldMapData real = worldmap::CurrentWorldMap();  // a COPY
  if (!real.Loaded()) return fail("no world map loaded");
  // SANDVOX_FREEFALL_SITE=<map>,x,y,z,vx,vy,vz (voxels, voxels/s): drop the
  // body there instead -- the owner's real mountain, e.g. the ticket's
  // `default,5700,19830,-69483,0,0,40`. A diagnostic arm: the PASS/FAIL
  // claims are the gate's, the face numbers (zMid, hFoot) the site's column.
  struct Site { char map[64] = "peaks"; float x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0; bool set = false; } site;
  if (const char* e = std::getenv("SANDVOX_FREEFALL_SITE")) {
    char m[64] = {};
    if (std::sscanf(e, "%63[^,],%f,%f,%f,%f,%f,%f", m, &site.x, &site.y, &site.z, &site.vx, &site.vy,
                    &site.vz) == 7) {
      std::snprintf(site.map, sizeof site.map, "%s", m);
      site.set = true;
    } else {
      return fail(std::string("SANDVOX_FREEFALL_SITE unreadable: ") + e);
    }
  }
  worldmap::WorldMapData pk;
  std::vector<uint32_t> words;
  if (!worldmap::LoadWorldMap(dir, site.map, set, c.mats.size(), kDefaultSeed, pk, log) ||
      !worldmap::PackWorldMap(set, pk, words, log))
    return fail(std::string("the ") + site.map + " map did not load: " + log);
  const IVec3 savedOrigin = c.world.WindowOrigin();
  c.ctx.WaitIdle();
  worldmap::SetCurrentWorldMap(pk);
  c.sim.UploadEnvironment(c.ctx.device, c.ctx.queue, atlas, words);

  // The face at x = 0: the band's centreline is z = -8000 there and the land
  // to +z stands 300 m higher. Find the steepest column pair, then start the
  // body off the low side of it, 60 m above the face's middle.
  const int fx = 0;
  int zMid = -7790, steepest = 0;
  for (int z = -8100; z <= -7500; z += 4) {
    const int d = World::TerrainHeight(fx, z + 4, kDefaultSeed) -
                  World::TerrainHeight(fx, z, kDefaultSeed);
    if (d > steepest) { steepest = d; zMid = z; }
  }
  const int hMid = World::TerrainHeight(fx, zMid, kDefaultSeed);
  const int hTop = World::TerrainHeight(fx, zMid + 400, kDefaultSeed);
  const int hFoot = World::TerrainHeight(fx, zMid - 400, kDefaultSeed);
  const float startX = site.set ? site.x : (float)fx + 0.5f;
  const float startZ = site.set ? site.z : (float)(zMid - 40) + 0.5f;
  const float startY = site.set ? site.y : (float)(hMid + 600) + Player::kHalfY;
  if (site.set) {
    // The column profile along the push, so a dump says what the body met.
    std::printf("freefall-slope: site %s (%.1f,%.1f,%.1f) v (%.1f,%.1f,%.1f) vox/s; ground under it %d\n",
                site.map, site.x, site.y, site.z, site.vx, site.vy, site.vz,
                World::TerrainHeight(ifloor(site.x), ifloor(site.z), kDefaultSeed));
    const float hl = std::sqrt(site.vx * site.vx + site.vz * site.vz);
    const float ux = hl > 0 ? site.vx / hl : 0.0f, uz = hl > 0 ? site.vz / hl : 1.0f;
    std::string prof;
    for (int d = -40; d <= 2400; d += 80)
      prof += Format(" %d:%d", d, World::TerrainHeight(ifloor(site.x + ux * d), ifloor(site.z + uz * d),
                                                       kDefaultSeed));
    std::printf("freefall-slope: ground along the push (dist:height)%s\n", prof.c_str());
  }
  const float g = CurrentTuning().player.gravity / kVoxelMeters;
  const double airArrestMs = BaselineNumber("freefallSlope.airArrestMs", 6.0);
  const double airClearVox = BaselineNumber("freefallSlope.airClearVox", 24.0);

  // The nearest landform cell to p, from the CPU height function over a grid
  // round it (cells inside the ground count 0). The landform is what
  // TerrainHeight answers; the band is bare rock, so it is the face itself.
  auto clearance = [&](Vec3 p) {
    float best = 1e9f;
    for (int dz = -40; dz <= 40; dz += 4)
      for (int dx = -40; dx <= 40; dx += 4) {
        const int x = ifloor(p.x) + dx, z = ifloor(p.z) + dz;
        const float h = (float)World::TerrainHeight(x, z, kDefaultSeed);
        const float up = std::max(0.0f, p.y - h);
        best = std::min(best, std::sqrt((float)(dx * dx + dz * dz) + up * up));
      }
    return best;
  };

  struct Row {
    Vec3 hips, hv;
    int limp = 0, held = 0, alive = 1, bills = 0;
    float clear = 0.0f, lost = 0.0f;
    int outPatches = 0;  // patches round the pelvis built against an out-of-window neighbour
    float ms = 0.0f;
    // Where the tick's physics went (attribution for a slow graze/splat):
    // the Jolt step, its CCD and collide spans, contacts against terrain,
    // and the terrain colliders built (count, shape + add wall us).
    float stepMs = 0.0f, ccdMs = 0.0f, findMs = 0.0f, solveMs = 0.0f;
    uint32_t statMan = 0, ccdBodies = 0, meshes = 0, tris = 0;
    float meshUs = 0.0f;
    uint32_t damped = 0;  // runaway-net damps this tick (Physics::Runaway)
    float spin = 0.0f;    // the fastest-spinning part's rad/s, and which
    std::string spinPart;
    float span = 0.0f;    // the furthest part's centre from the pelvis, voxels
    int needAgo = -1;     // a held tick: how long before it the chunk was first listed
    std::string why;     // a held tick: the unvouched chunk and its NeedProbe
  };
  const float vTerminal = CurrentTuning().player.maxFall / kVoxelMeters;
  const double terminalSlack = BaselineNumber("freefallSlope.terminalSlack", 1.1);
  const double maxLateHolds = BaselineNumber("freefallSlope.maxLateHolds", 6.0);
  std::string fails;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) fails += (fails.empty() ? "" : "; ") + what;
  };
  std::string armsDetail;
  float splatMsWorst = 0.0f;

  // ONE ARM: drop the player from (sx,sy,sz) with v0, follow it down, check.
  auto runArm = [&](const char* arm, float sx, float sy, float sz, Vec3 v0, uint32_t tick0,
                    bool graze) {
    const IVec3 armChunk{ifloor(sx) >> 4, ifloor(sy) >> 4, ifloor(sz) >> 4};
    c.debris.Reset();
    c.debris.ResetUntunnelProbe();
    c.phys.ResetRunawayProbe();
    c.world.ResetFetchProbe();
    c.mobs.Reset(true);
    c.mobs.ClearRisings();
    c.stream.OnRegen();
    c.world.SetWindowOrigin({armChunk.x - (int)kNChunk / 2, armChunk.y - (int)kNChunk / 2,
                             armChunk.z - (int)kNChunk / 2});
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();

    std::vector<Row> rows;
    std::string crossing;
    uint32_t bordersBefore = c.debris.Settle().terrainBorderStale;
    const uint32_t urgentBefore = c.debris.Settle().terrainUrgentBuilds;
    c.debris.EnableNeedProbe(true);
    {
      support::TickRig rig(c, tick0, armChunk);
      rig.Authority().harness.pinWindow = false;
      rig.Authority().harness.haveSubmitChunk = false;
      PlayerSession& s = rig.Session();
      s.avatar.Init(&c.phys, &c.world, &c.debris, c.mats, &c.mobs);
      s.avatar.SetDefs(&c.mobs.Defs(), kAvatarDefName);
      c.mobs.SetAvatar(&s.avatar);
      s.player.fly = false;
      s.player.grounded = false;
      // A step toward the face (+z), the way a jump off a ledge carries one.
      s.player.vel = v0;
      s.player.pos = Vec3{sx, sy, sz};
      s.player.SnapRender();
      const int hips = s.avatar.PartIndex("hips");
      uint32_t prevHolds = c.debris.Untunnel().rigHolds;
      uint32_t prevBills = 0, prevDamped = 0;
      Vec3 prevHv{};
      bool havePrev = false;
      int stillTicks = 0;
      for (int i = 0; i < kSlopeMaxTicks; i++) {
        const Physics::MeshStats m0 = c.phys.Meshes();
        const double t0 = NowSeconds();
        support::RunTicks(rig, 1);
        Row r;
        r.ms = (float)((NowSeconds() - t0) * 1000.0);
        {
          const Physics::StepStats& st = c.phys.LastStep();
          r.stepMs = (float)st.ms;
          r.ccdMs = st.phaseWallMs[(int)Physics::StepPhase::Ccd];
          r.findMs = st.phaseWallMs[(int)Physics::StepPhase::FindCollisions];
          r.solveMs = st.phaseWallMs[(int)Physics::StepPhase::SolveVelocity] +
                      st.phaseWallMs[(int)Physics::StepPhase::SolvePosition];
          r.statMan = st.manifoldsStatic;
          for (int k = 0; k < Physics::kRoleCols; k++) r.ccdBodies += st.ccdBodies[k];
          const Physics::MeshStats& m1 = c.phys.Meshes();
          r.meshes = (uint32_t)(m1.meshes - m0.meshes);
          r.tris = (uint32_t)(m1.tris - m0.tris);
          r.meshUs = (float)((m1.shapeUs - m0.shapeUs) + (m1.addUs - m0.addUs) +
                             (m1.removeUs - m0.removeUs));
          const uint32_t dmp = c.phys.Runaway().damped + c.phys.Runaway().cut;
          r.damped = dmp - prevDamped;
          prevDamped = dmp;
        }
        const uint32_t h = c.debris.Untunnel().rigHolds;
        r.held = (int)(h - prevHolds);
        prevHolds = h;
        if (r.held) {
          const IVec3 uc = c.debris.Untunnel().lastChunk;
          const uint32_t now = rig.tick;
          auto ago = [&](uint32_t t) { return t == 0 ? -1 : (int)(now - t); };
          if (const DebrisSystem::NeedRec* nr = c.debris.NeedProbe(uc)) {
            r.needAgo = ago(nr->firstNeeded);
            r.why = Format("unvouched (%d,%d,%d): need -%d rank %u req -%d cached -%d built -%d "
                           "| ceil %u budget %u sky %u gather %u build %u",
                           uc.x, uc.y, uc.z, ago(nr->firstNeeded), nr->firstRank,
                           ago(nr->firstRequested), ago(nr->firstCached), ago(nr->firstBuilt),
                           nr->ceilingCut, nr->budgetDenied, nr->skyskipped, nr->gatherDeferred,
                           nr->buildDeferred);
          } else {
            r.why = Format("unvouched (%d,%d,%d): never listed", uc.x, uc.y, uc.z);
          }
          // WHO and WHERE (UntunnelRig's own record, taken at the hold): the
          // limb whose step ended in the chunk, its step, how far that end is
          // from the anchor the sweep before it listed chunks round (the
          // pelvis), and the vouch verdict's own reasons.
          const auto& un = c.debris.Untunnel();
          std::string part = "?";
          if (s.avatar.Spawned())
            for (int pi = 0; pi < s.avatar.PartCount(); pi++)
              if (s.avatar.PartBody(pi) == un.lastHandle && s.avatar.PartName(pi))
                part = s.avatar.PartName(pi);
          float anchorDist = -1.0f;
          Vec3 anchorVel{};
          for (const auto& [ap, av] : c.debris.ProbeAnchors()) {
            const float d = (un.lastTo - ap).len();
            if (anchorDist < 0.0f || d < anchorDist) { anchorDist = d; anchorVel = av; }
          }
          r.why += Format(" | limb %s step (%.1f,%.1f,%.1f)->(%.1f,%.1f,%.1f) %.1f vox, %.1f vox "
                          "from the anchor (v %.0f vox/s) | %s",
                          part.c_str(), un.lastFrom.x, un.lastFrom.y, un.lastFrom.z, un.lastTo.x,
                          un.lastTo.y, un.lastTo.z, (un.lastTo - un.lastFrom).len(), anchorDist,
                          anchorVel.len(), un.lastWhy.c_str());
        }
        r.limp = s.avatar.Spawned() && s.avatar.Ragdolled() ? 1 : 0;
        r.alive = s.avatar.Spawned() && s.avatar.Alive() ? 1 : 0;
        const uint32_t bills = s.avatar.Spawned() ? s.avatar.FallsBilled() : prevBills;
        r.bills = (int)(bills - prevBills);
        prevBills = bills;
        Vec3 hp = s.player.pos;
        Quat hq{};
        if (hips >= 0 && s.avatar.Spawned()) s.avatar.PartWorldTransform(hips, hp, hq);
        r.hips = hp;
        Vec3 lin{}, ang{};
        const uint64_t hb = hips >= 0 && s.avatar.Spawned() ? s.avatar.PartBody(hips) : 0;
        if (hb != 0 && c.phys.GetBodyVelocities(hb, lin, ang)) r.hv = lin;
        else r.hv = s.player.vel;
        r.clear = clearance(hp);
        if (s.avatar.Spawned())
          for (int pi = 0; pi < s.avatar.PartCount(); pi++) {
            const uint64_t pb = s.avatar.PartBody(pi);
            Vec3 pl{}, pa{};
            if (pb == 0 || !c.phys.GetBodyVelocities(pb, pl, pa)) continue;
            BodyTransform pxf{};
            if (c.phys.GetTransform(pb, pxf)) r.span = std::max(r.span, (pxf.pos - hp).len());
            if (pa.len() > r.spin) {
              r.spin = pa.len();
              r.spinPart = s.avatar.PartName(pi) ? s.avatar.PartName(pi) : "?";
            }
          }
        if (havePrev) {
          const Vec3 expect = prevHv + Vec3{0.0f, -g * kTickDt, 0.0f};
          // (At terminal velocity Mob::ApplyRigAirDrag takes back one tick of
          // gravity, ~0.33 m/s: far under airArrestMs, so it needs no term.)
          r.lost = (expect - r.hv).len() * kVoxelMeters;  // m/s
        }
        {
          const IVec3 pc{ifloor(hp.x) >> 4, ifloor(hp.y) >> 4, ifloor(hp.z) >> 4};
          for (int dz = -1; dz <= 1; dz++)
            for (int dy = -1; dy <= 1; dy++)
              for (int dx = -1; dx <= 1; dx++) {
                uint32_t om = 0, mm = 0;
                if (c.debris.TerrainBorder({pc.x + dx, pc.y + dy, pc.z + dz}, om, mm) && om != 0)
                  r.outPatches++;
              }
        }
        // THE CROSSING (as `freefall` reports it): the first tick the pelvis is
        // inside the landform, and what every collision source said about the
        // chunks round it at that moment.
        if (crossing.empty() && hp.y < (float)World::TerrainHeight(ifloor(hp.x), ifloor(hp.z),
                                                                   kDefaultSeed) - 8.0f) {
          const IVec3 pc{ifloor(hp.x) >> 4, ifloor(hp.y) >> 4, ifloor(hp.z) >> 4};
          crossing = Format("%s: crossed the ground at tick %d (pelvis chunk (%d,%d,%d), linearCast %d):",
                            arm, i, pc.x, pc.y, pc.z, hb != 0 && c.phys.UsesLinearCast(hb) ? 1 : 0);
          const WorldSnapshot& sn = c.world.Snap();
          for (int dy = 1; dy >= -1; dy--) {
            const IVec3 q{pc.x, pc.y + dy, pc.z};
            uint32_t om = 0, mm = 0;
            const bool body = c.debris.TerrainBorder(q, om, mm);
            const uint32_t occ = sn.valid && sn.occupancy.size() >= kNumChunks
                                     ? sn.occupancy[World::SlotChunkIndex(q)] : 0xFFFFFFFFu;
            crossing += Format(" [y%+d built %u body %d out %x miss %x vouched %d cached %d sky %d occ %u]",
                               dy, c.debris.TerrainBuiltVersion(q), body ? 1 : 0, om, mm,
                               c.debris.ColliderVouched(q) ? 1 : 0, c.world.Cached(q) ? 1 : 0,
                               c.debris.SkyKnown(q) ? 1 : 0, occ);
          }
        }
        // AN ARREST WITH NOTHING NEAR: what the body could have met. The Jolt
        // step's manifolds (terrain vs body-body, by role pair), the runaway
        // net's damps, and every terrain patch with a body round the pelvis --
        // its border guesses and how much matter its own cached chunk holds.
        if (r.lost > (float)airArrestMs && r.clear > (float)airClearVox) {
          const Physics::StepStats& st = c.phys.LastStep();
          r.why += Format(" AIR: manifolds static %u dyn %u, runaway damps %u;", st.manifoldsStatic,
                          st.manifoldsDyn, r.damped);
          for (int a = 0; a < Physics::kRoleCols; a++)
            for (int b = a; b < Physics::kRoleCols; b++)
              if (const uint32_t n = st.rolePairs[a * Physics::kRoleCols + b])
                r.why += Format(" pair %d-%d x%u;", a, b, n);
          const IVec3 pc{ifloor(hp.x) >> 4, ifloor(hp.y) >> 4, ifloor(hp.z) >> 4};
          for (int dz = -1; dz <= 1; dz++)
            for (int dy = -1; dy <= 1; dy++)
              for (int dx = -1; dx <= 1; dx++) {
                const IVec3 q{pc.x + dx, pc.y + dy, pc.z + dz};
                uint32_t om = 0, mm = 0;
                if (!c.debris.TerrainBorder(q, om, mm)) continue;
                const CachedChunk* cc = c.world.Cached(q);
                int solid = -1;
                if (cc && cc->voxels.size() == kChunkVol) {
                  solid = 0;
                  for (uint32_t w : cc->voxels) {
                    const uint32_t m = w & 0xFFFu;
                    if (m != 0u && m < c.mats.size() && c.mats[m].gpu.klass == CLASS_SOLID) solid++;
                  }
                }
                r.why += Format(" patch(%d,%d,%d) v%u out %x miss %x inWin %d solid %d;", dx, dy, dz,
                                c.debris.TerrainBuiltVersion(q), om, mm,
                                c.world.ChunkInWindow(q) ? 1 : 0, solid);
              }
        }
        prevHv = r.hv;
        havePrev = true;
        rows.push_back(r);
        // Settled: a limp (or dead) body that has stopped for two seconds.
        if (r.limp && r.hv.len() * kVoxelMeters < 0.5f) stillTicks++;
        else stillTicks = 0;
        if (stillTicks >= 60) break;
      }
      c.mobs.SetAvatar(nullptr);
      if (s.avatar.Spawned()) s.avatar.Despawn();
    }
    c.debris.EnableNeedProbe(false);
    const uint32_t borders = c.debris.Settle().terrainBorderStale - bordersBefore;
    const uint32_t urgentBuilds = c.debris.Settle().terrainUrgentBuilds - urgentBefore;
    // WHAT THE HELD-FOR CHUNKS HELD, read from the mirror cache now that they
    // have landed: the material in each, by count (attribution for (A)).
    std::string heldWhat;
    {
      std::vector<IVec3> seen;
      for (const Row& r : rows) {
        if (!r.held || !r.alive) continue;
        int ux, uy, uz;
        if (std::sscanf(r.why.c_str(), "unvouched (%d,%d,%d)", &ux, &uy, &uz) != 3) continue;
        const IVec3 uc{ux, uy, uz};
        bool dup = false;
        for (const IVec3& q : seen) dup = dup || (q.x == uc.x && q.y == uc.y && q.z == uc.z);
        if (dup || seen.size() >= 6) continue;
        seen.push_back(uc);
        const CachedChunk* cc = c.world.Cached(uc);
        if (!cc || cc->voxels.size() != kChunkVol) {
          heldWhat += Format(" (%d,%d,%d): not cached;", uc.x, uc.y, uc.z);
          continue;
        }
        std::map<uint32_t, int> n;
        for (uint32_t w : cc->voxels)
          if ((w & 0xFFFu) != 0u) n[w & 0xFFFu]++;
        heldWhat += Format(" (%d,%d,%d):", uc.x, uc.y, uc.z);
        if (n.empty()) heldWhat += " empty";
        for (const auto& [m, k] : n)
          heldWhat += Format(" %s(class %u) x%d", m < c.mats.size() ? c.mats[m].name.c_str() : "?",
                             m < c.mats.size() ? c.mats[m].gpu.klass : 99u, k);
        heldWhat += ";";
      }
    }
    const auto ut = c.debris.Untunnel();
    const auto rw = c.phys.Runaway();
    // The engine's fetch FIFO over the arm (World::FetchReport): a held chunk
    // that was requested in time and never cached was queued behind the
    // per-tick readback cap, or dropped -- this says which.
    const std::string fetchRep = c.world.FetchReport();

    // ---- the analysis ---------------------------------------------------------
    int holds = 0, firstHold = -1, airArrests = 0, firstAir = -1, contacts = 0, firstContact = -1;
    int airBills = 0, outAtAir = 0, splatAway = 0, splatAwayAt = -1, damped = 0, firstDamp = -1;
    int splatAt = -1, budgetHolds = 0, lateHolds = 0, firstLate = -1, spinDamped = 0;
    int firstSpin = -1;
    float worstAir = 0.0f, peakLimp = 0.0f;
    int worstAirAt = -1, peakLimpAt = -1;
    std::string billList;
    for (int i = 0; i < (int)rows.size(); i++) {
      const Row& r = rows[i];
      // A HOLD IS ONE OF TWO THINGS, and the needAgo of its chunk says which.
      // Listed at least kSnapshotLatency + 1 ticks ahead, the patch had time
      // to be fetched and built and was not: the budgets' fault, and the
      // sustained "skipping down the face" (22 in a row at 60 m/s on the real
      // mountain before the in-flight and urgent-lane fixes). Listed later --
      // the rig BOUNCED and turned into a chunk no lookahead pointed at --
      // only the fetch round trip separates it from its patch: counted and
      // bounded (`freefallSlope.maxLateHolds`), not zero.
      // While the body is ALIVE: UntunnelRig's counter is the world's, and
      // after a splat it is the radial impulse's pieces flying out in
      // directions no lookahead pointed at (measured on the real mountain:
      // 12 of 16 holds were the corpse's pieces in the 20 ticks after it).
      if (r.held && r.alive) {
        holds++;
        if (r.needAgo >= (int)World::kSnapshotLatency + 1) {
          budgetHolds++;
          if (firstHold < 0) firstHold = i;
        } else {
          lateHolds++;
          if (firstLate < 0) firstLate = i;
        }
      }
      // While the body is ALIVE: after the splat the net is on the pieces a
      // radial impulse flung (its own job, not this claim's). And split by
      // which bar tripped: a LINEAR trip is a fast rig misread as a driven
      // one (this ticket); an ANGULAR one is a limb the solver keeps
      // spinning at the 47.1 rad/s clamp -- the net's own case, recorded
      // here with the part that spun.
      if (r.damped && r.alive) {
        // (The previous row too: the step that CUTS a spinning limb zeroes it,
        // so the cut tick no longer shows the spin that caused it.)
        const float spinBar = 0.9f * Physics::MaxBodySpinRad();
        if (r.spin >= spinBar || (i > 0 && rows[i - 1].spin >= spinBar)) {
          spinDamped += (int)r.damped;
          if (firstSpin < 0) firstSpin = i;
        } else {
          damped += (int)r.damped;
          if (firstDamp < 0) firstDamp = i;
        }
      }
      if (r.clear < 6.0f && firstContact < 0) firstContact = i;
      if (r.clear < 6.0f) contacts++;
      if (r.limp && r.alive && r.clear > (float)airClearVox && r.hv.len() > peakLimp) {
        peakLimp = r.hv.len();
        peakLimpAt = i;
      }
      // A bill is made in the tick's PreTick, from the arrest the PREVIOUS
      // step produced, at the pelvis as that step left it: the previous row's.
      const float billClear = i > 0 ? rows[i - 1].clear : r.clear;
      if (r.bills)
        billList += Format(" t%d(clear %.0f%s)", i, billClear, r.alive ? "" : " dead");
      // THE SPLAT IS WHERE THE BODY HIT. A lethal bill (the tick the body dies
      // of a fall) with the pelvis clear of every landform cell is a body that
      // burst away from what killed it -- after bouncing off it, or lying still
      // beside it (Mob::TakeRagdollImpact's lethal exception).
      if (r.bills && !r.alive && i > 0 && rows[i - 1].alive) {
        splatAt = i;
        if (billClear > 8.0f) {
          splatAway++;
          if (splatAwayAt < 0) splatAwayAt = i;
        }
      }
      // Skip the tick the avatar goes limp (a hand-over from the controller's
      // velocity to the solver's) and the death tick (a splat reseeds the rig).
      const bool handover = i > 0 && (rows[i - 1].limp != r.limp || rows[i - 1].alive != r.alive);
      if (i == 0 || handover) continue;
      if (billClear > (float)airClearVox && r.bills) airBills++;
      if (r.lost > (float)airArrestMs && r.clear > (float)airClearVox) {
        airArrests++;
        if (firstAir < 0) firstAir = i;
        outAtAir += r.outPatches;
        if (r.lost > worstAir) { worstAir = r.lost; worstAirAt = i; }
      }
    }
    // THE SPLAT'S COST: the tick the body died on and the one after it (the
    // pieces' first step), wall clock. Recorded, not asserted (a machine-load
    // number); the report names it so a hitch has a number beside it.
    float splatMs = 0.0f;
    for (int i = splatAt; i >= 0 && i <= splatAt + 1 && i < (int)rows.size(); i++)
      splatMs = std::max(splatMs, rows[i].ms);
    splatMsWorst = std::max(splatMsWorst, splatMs);
    const float startHipsY = rows.empty() ? 0.0f : rows.front().hips.y;
    float lowest = startHipsY;
    for (const Row& r : rows) lowest = std::min(lowest, r.hips.y);
    const float descended = startHipsY - lowest;
    auto rowAt = [&](int i) -> std::string {
      if (i < 0 || i >= (int)rows.size()) return "-";
      const Row& r = rows[i];
      return Format("tick %d: hips (%.1f,%.1f,%.1f) v (%.1f,%.1f,%.1f) m/s lost %.1f m/s clear %.1f "
                    "limp %d alive %d held %d bills %d damped %u spin %.1f %s span %.1f outPatches %d %.1f ms [step %.2f "
                    "ccd %.2f find %.2f solve %.2f static %u ccdB %u meshes %u tris %u %.2f ms] %s",
                    i, r.hips.x, r.hips.y, r.hips.z, r.hv.x * kVoxelMeters, r.hv.y * kVoxelMeters,
                    r.hv.z * kVoxelMeters, r.lost, r.clear, r.limp, r.alive, r.held, r.bills,
                    r.damped, r.spin, r.spinPart.c_str(), r.span, r.outPatches, r.ms, r.stepMs, r.ccdMs, r.findMs, r.solveMs, r.statMan,
                    r.ccdBodies, r.meshes, r.tris, r.meshUs * 0.001f, r.why.c_str());
    };
    // EVERY hold while alive, one line each (there are a handful at most): the
    // first one in the failure text is not enough to attribute a COUNT.
    for (int i = 0; i < (int)rows.size(); i++)
      if (rows[i].held && rows[i].alive)
        std::printf("freefall-slope: %s hold [%s]\n", arm, rowAt(i).c_str());
    const std::string pre = std::string("freefallSlope.") + arm + ".";
    RecordObserved((pre + "rigHolds").c_str(), (double)holds);
    RecordObserved((pre + "lateHolds").c_str(), (double)lateHolds);
    RecordObserved((pre + "spinDamps").c_str(), (double)spinDamped);
    RecordObserved((pre + "airArrests").c_str(), (double)airArrests);
    RecordObserved((pre + "descendedVox").c_str(), (double)descended);
    RecordObserved((pre + "runawayDamps").c_str(), (double)damped);
    RecordObserved((pre + "peakLimpMs").c_str(), (double)(peakLimp * kVoxelMeters));
    RecordObserved((pre + "splatTickMs").c_str(), (double)splatMs);

    auto armCheck = [&](bool ok, const std::string& what) {
      check(ok, std::string(arm) + ": " + what);
    };
    if (graze) armCheck(firstContact >= 0, "the body never came within 6 voxels of the face");
    armCheck(budgetHolds == 0,
             Format("%d rig-held ticks (UntunnelRig) on a chunk listed in time, first [%s]",
                    budgetHolds, rowAt(firstHold).c_str()));
    armCheck(lateHolds <= (int)maxLateHolds,
             Format("%d rig-held ticks on chunks listed too late to fetch (a bounce), over "
                    "freefallSlope.maxLateHolds %.0f, first [%s]",
                    lateHolds, maxLateHolds, rowAt(firstLate).c_str()));
    armCheck(ut.released == 0, Format("%u bodies released into unvouched space", ut.released));
    armCheck(airArrests == 0 && airBills == 0,
             Format("%d arrests and %d billed impacts in open air (worst %.1f m/s at [%s]; %d "
                    "patches round the pelvis built against an out-of-window neighbour at those "
                    "ticks)",
                    airArrests, airBills, worstAir, rowAt(worstAirAt).c_str(), outAtAir));
    armCheck(splatAway == 0, Format("the lethal splat went off %d time(s) clear of the ground, "
                                    "first [%s]", splatAway, rowAt(splatAwayAt).c_str()));
    armCheck(damped == 0,
             Format("the runaway net slowed or cut %d bodies at their LINEAR bar while the body "
                    "was alive (run peak %.0f vox/s, %.1f rad/s, %u cut), first [%s]", damped,
                    rw.peakSpeedVox, rw.peakSpinRad, rw.cut, rowAt(firstDamp).c_str()));
    if (graze)
      armCheck(site.set || descended >= 0.5f * (float)(hMid - hFoot),
               Format("descended only %.0f voxels of a face %d tall (last [%s])", descended,
                      hTop - hFoot, rowAt((int)rows.size() - 1).c_str()));
    else
      armCheck(peakLimp <= (float)terminalSlack * vTerminal && peakLimp >= 0.8f * vTerminal,
               Format("limp rig peaked at %.1f m/s in open air, terminal velocity (player.maxFall) "
                      "%.1f m/s, slack x%.2f [%s]",
                      peakLimp * kVoxelMeters, vTerminal * kVoxelMeters, terminalSlack,
                      rowAt(peakLimpAt).c_str()));
    if (!crossing.empty()) check(false, crossing);
    armsDetail += Format(" | %s: start y%.0f, %d ticks, first contact tick %d, %d ticks within 6 "
                         "vox of the face, descended %.0f vox, peak limp %.1f m/s, rig holds %d "
                         "(%d late, released %u), open-air arrests %d, runaway damps %d linear / "
                         "%d spin (first spin [%s]), border re-gathers %u, urgent builds %u, splat "
                         "tick %.1f ms, fall bills:%s | held-for chunks:%s%s%s",
                         arm, sy, (int)rows.size(), firstContact, contacts, descended,
                         peakLimp * kVoxelMeters, holds, lateHolds, ut.released, airArrests, damped,
                         spinDamped, firstSpin >= 0 ? rowAt(firstSpin).substr(0, 160).c_str() : "-",
                         borders, urgentBuilds, splatMs,
                         billList.empty() ? " none" : billList.c_str(),
                         heldWhat.empty() ? " none" : heldWhat.c_str(),
                         holds > 0 ? " | " : "", holds > 0 ? fetchRep.c_str() : "");
    if (std::getenv("SANDVOX_FREEFALL_DUMP"))
      for (size_t i = 0; i < rows.size(); i++)
        std::printf("  fs %s %s\n", arm, rowAt((int)i).c_str());
  };

  if (site.set) {
    runArm("site", startX, startY, startZ, Vec3{site.vx, site.vy, site.vz}, 92000u, true);
  } else {
    runArm("graze", startX, startY, startZ, Vec3{0.0f, 0.0f, 30.0f}, 92000u, true);
    // 300 m higher and barely drifting toward the face: terminal velocity is
    // reached in open air, then the rig meets the face at it.
    runArm("terminal", startX, startY + 3000.0f, startZ - 20.0f, Vec3{0.0f, 0.0f, 8.0f}, 94000u,
           false);
  }

  // ---- the pristine world back (as `altitude` does) --------------------------
  {
    biomes::EnvironmentStamp stamp;
    std::string l;
    if (!ReloadEnvironment(c.ctx, c.sim, c.mats, stamp, l)) {
      worldmap::SetCurrentWorldMap(real);
      check(false, "ReloadEnvironment REFUSED afterwards: " + l);
    }
    c.stream.OnRegen();
    c.world.SetWindowOrigin(savedOrigin);
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    DrainFullRefill(c.ctx, c.world, c.sim, IVec3{108 >> 4, 122 >> 4, 108 >> 4});
  }
  check(site.set || steepest >= 16,
        Format("the face is not steep at x %d (steepest %d over 4 voxels)", fx, steepest));
  RecordObserved("freefallSlope.splatTickMs", (double)splatMsWorst);
  detail = Format("face x %d z %d (mid y%d, foot y%d, top y%d, steepest %d/4 vox)%s", fx, zMid, hMid,
                  hFoot, hTop, steepest, armsDetail.c_str());
  if (!fails.empty()) detail += " || " + fails;
  const bool ok = fails.empty();
  std::printf("freefall-slope: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& PlayerGates() {
  static const std::vector<Gate> g = {
      {"player-walk", "player", {}, false, GatePlayerWalk},
      {"player-waterjump", "player", {}, false, GatePlayerWaterJump},
      {"player-swim-floor", "player", {}, false, GatePlayerSwimFloor},
      {"player-current", "player", {}, false, GatePlayerCurrent},
      {"player-swim-entry", "player", {}, false, GatePlayerSwimEntry},
      {"player-ledgegrab", "player", {}, false, GatePlayerLedgeGrab},
      {"player-climb-fuzz", "player", {}, false, GatePlayerClimbFuzz},
      {"player-crouch", "player", {}, false, GatePlayerCrouch},
      {"player-plants", "player", {}, false, GatePlayerPlants},
      {"player-fastfall", "player", {}, false, GatePlayerFastFall},
      {"freefall", "player", {}, false, GateFreefall},
      {"freefall-slope", "player", {}, false, GateFreefallSlope},
      {"tick-input", "player", {}, false, GateTickInput},
      {"view-smooth", "player", {}, false, GateViewSmooth},
      {"two-players", "player", {}, false, GateTwoPlayers},
      {"remote-ghost", "player", {}, false, GateRemoteGhost},
      {"layer-roles", "player", {}, false, GateLayerRoles},
  };
  return g;
}

}  // namespace selftest
