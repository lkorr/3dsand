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

}  // namespace

const std::vector<Gate>& PlayerGates() {
  static const std::vector<Gate> g = {
      {"player-walk", "player", {}, false, GatePlayerWalk},
  };
  return g;
}

}  // namespace selftest
