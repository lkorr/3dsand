// World chemistry (docs/PLAN_alchemy_chemistry.md, package A): the new
// materials behaving right IN THE 3D WORLD, and reactions with EFFECTS.
//
//   chem-sodium        sodium dropped into a stone basin of water EXPLODES:
//                      the GPU reports the firing, the tick authors a real
//                      ExplosionOp (the grenade path), a crater is cut in the
//                      basin, lye / hydrogen / fire appear -- and the whole
//                      run is reproduced bit for bit a second time.
//   chem-acid-fumes    acid on stone and iron eats them and FUMES noxious
//                      gas; the glass box it sits in and a glass block in the
//                      pool are untouched.
//   chem-electrolysis  salt on lava melts to molten salt; molten salt shocked
//                      with sparks splits into sodium and chlorine.
//   chem-toxic         a creature standing in chlorine is chemically burnt:
//                      skin blisters to flesh_cooked through the inbound pass
//                      and the burnt fraction (which caps health) rises.
//
// Every gate ticks THE tick (support::TickCursor -> TickAuthority): the
// explosion path under test lives in game/session.cpp's phase K and the body
// path in the mob phase, so a sim-only ticker would test nothing. Each builds
// its own sealed fixture in open air over the harness terrain (FixtureYOver,
// never an absolute Y), and regenerates the world on the way out.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "game/mob.h"
#include "game/session.h"
#include "sim/worldgen_run.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

using namespace sandvox;

namespace selftest {
namespace {

int MatId(Ctx& c, const char* n) {
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == n) return (int)i;
  return -1;
}

struct Box {
  IVec3 lo, hi;  // inclusive, world cells
  bool Has(int x, int y, int z) const {
    return x >= lo.x && x <= hi.x && y >= lo.y && y <= hi.y && z >= lo.z && z <= hi.z;
  }
};

// Material histogram over a box, read through the page table (ReadVoxelsSync
// synthesizes sentinel chunks). `hash` gets an FNV over every word with the
// stamp + excite scratch masked, which is what the world hash covers too.
std::vector<uint32_t> Census(Ctx& c, const Box& b, uint32_t* hash = nullptr) {
  std::vector<uint32_t> h(c.mats.size() + 1, 0);
  std::vector<uint32_t> buf(kChunkVol);
  uint32_t fnv = 2166136261u;
  for (int cz = b.lo.z >> 4; cz <= (b.hi.z >> 4); cz++)
    for (int cy = b.lo.y >> 4; cy <= (b.hi.y >> 4); cy++)
      for (int cx = b.lo.x >> 4; cx <= (b.hi.x >> 4); cx++) {
        ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex({cx, cy, cz}), 1,
                       buf.data(), "chemCensus");
        for (uint32_t k = 0; k < kChunkVol; k++) {
          const int x = (int)(k % 16) + cx * 16, y = (int)((k / 16) % 16) + cy * 16,
                    z = (int)(k / 256) + cz * 16;
          if (!b.Has(x, y, z)) continue;
          const uint32_t m = buf[k] & 0xFFFu;
          h[m < c.mats.size() ? m : c.mats.size()]++;
          fnv = (fnv ^ (buf[k] & ~0x00FF0000u)) * 16777619u;
        }
      }
  if (hash) *hash = fnv;
  return h;
}

// A box of `wall` material: floor + four walls, open top, interior cleared to
// air (the fixture sits in open air, but clearing costs nothing and makes the
// claim independent of what the terrain put there).
//
// STANDING ON A FOUNDATION of `footing` (stone) from below the terrain up to
// the shell's floor. A box floating in open air is an ISLAND, and the island
// scan cuts it loose as a debris body -- the first cut of these gates read
// every census as zero at the end because the whole fixture had fallen off
// the grid ("debris: island of 1049 voxels -> 1 body shard").
void Vessel(std::vector<CellOp>& ops, const Box& inner, int wallTop, uint32_t wall,
            uint32_t footing) {
  for (int z = inner.lo.z - 1; z <= inner.hi.z + 1; z++)
    for (int x = inner.lo.x - 1; x <= inner.hi.x + 1; x++)
      for (int y = World::TerrainHeight(x, z, kDefaultSeed) - 2; y < inner.lo.y - 1; y++)
        ops.push_back({World::SlotCellIndex({x, y, z}), PackVoxNew(footing, 0)});
  for (int z = inner.lo.z - 1; z <= inner.hi.z + 1; z++)
    for (int x = inner.lo.x - 1; x <= inner.hi.x + 1; x++)
      for (int y = inner.lo.y - 1; y <= wallTop; y++) {
        const bool shell = x < inner.lo.x || x > inner.hi.x || z < inner.lo.z ||
                           z > inner.hi.z || y < inner.lo.y;
        ops.push_back({World::SlotCellIndex({x, y, z}), shell ? PackVoxNew(wall, 0) : 0u});
      }
}

void Fill(std::vector<CellOp>& ops, const Box& b, uint32_t mat, uint32_t state) {
  for (int z = b.lo.z; z <= b.hi.z; z++)
    for (int y = b.lo.y; y <= b.hi.y; y++)
      for (int x = b.lo.x; x <= b.hi.x; x++)
        ops.push_back({World::SlotCellIndex({x, y, z}), PackVoxNew(mat, state)});
}

// A site inside the residency window, over the harness terrain (never an
// absolute X/Z either: `streaming` may have moved the origin in a full run).
IVec3 Site(Ctx& c, int inset, int halfW, int above) {
  const IVec3 o = c.world.WindowOrigin();
  const int x = o.x * (int)kChunk + inset, z = o.z * (int)kChunk + inset;
  const int y = FixtureYOver(x - halfW - 2, z - halfW - 2, x + halfW + 2,
                             z + halfW + 2, kDefaultSeed, above);
  return {x, y, z};
}

void Regenerate(Ctx& c) {
  c.mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
}

// ---------------------------------------------------------------------------
// chem-sodium
// ---------------------------------------------------------------------------
struct SodiumRun {
  std::vector<ExplosionOp> blasts;
  uint32_t firstBlastTick = 0;  // ticks after the sodium went in; 0 = never
  uint64_t events = 0, refused = 0, aftermath = 0;
  uint32_t stone0 = 0, stoneEnd = 0;
  uint32_t lyeMax = 0, hydrogenMax = 0, fireMax = 0, sodiumEnd = 0;
  uint32_t hash = 0;
  Box basin{};
};

bool RunSodium(Ctx& c, SodiumRun& R, std::string& why) {
  const int mStone = MatId(c, "stone"), mWater = MatId(c, "water"),
            mSodium = MatId(c, "sodium"), mLye = MatId(c, "lye"),
            mHyd = MatId(c, "hydrogen"), mFire = MatId(c, "fire");
  if (mStone < 0 || mWater < 0 || mSodium < 0 || mLye < 0 || mHyd < 0 || mFire < 0) {
    why = "missing one of stone/water/sodium/lye/hydrogen/fire";
    return false;
  }
  Regenerate(c);
  const int half = 6;
  const IVec3 s = Site(c, 200, half, 3);
  const Box inner{{s.x - half, s.y + 1, s.z - half}, {s.x + half, s.y + 8, s.z + half}};
  // Two cells of water: the lump floats at s.y + 3, so the blast (r5) reaches
  // the floor at s.y through two cells of water and cuts a crater in it.
  const int waterTop = s.y + 2;
  // What is watched: the basin, its shell and 12 cells of sky over it (the
  // hydrogen rises, the fire and smoke are laid over the crater).
  R.basin = Box{{inner.lo.x - 1, s.y - 2, inner.lo.z - 1},
                {inner.hi.x + 1, inner.hi.y + 12, inner.hi.z + 1}};
  std::vector<CellOp> build, water, lump;
  Vessel(build, inner, inner.hi.y, (uint32_t)mStone, (uint32_t)mStone);
  // A thicker floor, so the crater has rock to cut and cannot hole the basin.
  Fill(build, Box{{inner.lo.x - 1, s.y - 2, inner.lo.z - 1}, {inner.hi.x + 1, s.y - 1, inner.hi.z + 1}},
       (uint32_t)mStone, 0);
  Fill(water, Box{{inner.lo.x, inner.lo.y, inner.lo.z}, {inner.hi.x, waterTop, inner.hi.z}},
       (uint32_t)mWater, 7u);
  // A 2x1x2 lump on the surface, in the middle. Sodium (970) floats.
  Fill(lump, Box{{s.x, waterTop + 1, s.z}, {s.x + 1, waterTop + 1, s.z + 1}},
       (uint32_t)mSodium, 0);

  uint32_t t = 71000;
  support::TickCursor tick{c, t, IVec3{s.x >> 4, s.y >> 4, s.z >> 4}};
  tick(std::vector<BrushOp>{}, build);
  tick(std::vector<BrushOp>{}, water);
  for (int i = 0; i < 20; i++) tick();  // the water settles
  R.stone0 = Census(c, R.basin)[mStone];
  auto& rf = tick.Rig().Authority().reactFx;
  const uint64_t b0 = rf.blasts, e0 = rf.events, r0 = rf.refused, a0 = rf.aftermathCells;
  const size_t recent0 = rf.recent.size();
  (void)recent0;
  tick(std::vector<BrushOp>{}, lump);
  const int kTicks = 90;
  for (int i = 1; i <= kTicks; i++) {
    const uint64_t before = rf.blasts;
    tick();
    if (rf.blasts > before) {
      if (!R.firstBlastTick) R.firstBlastTick = (uint32_t)i;
      for (uint64_t k = before; k < rf.blasts; k++)
        R.blasts.push_back(rf.recent[rf.recent.size() - (size_t)(rf.blasts - k)]);
    }
    const std::vector<uint32_t> h = Census(c, R.basin);
    R.lyeMax = std::max(R.lyeMax, h[mLye]);
    R.hydrogenMax = std::max(R.hydrogenMax, h[mHyd]);
    R.fireMax = std::max(R.fireMax, h[mFire]);
  }
  const std::vector<uint32_t> end = Census(c, R.basin, &R.hash);
  R.stoneEnd = end[mStone];
  R.sodiumEnd = end[mSodium];
  R.events = rf.events - e0;
  R.refused = rf.refused - r0;
  R.aftermath = rf.aftermathCells - a0;
  (void)b0;
  return true;
}

Status GateChemSodium(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  SodiumRun A, B;
  std::string why;
  if (!RunSodium(c, A, why) || !RunSodium(c, B, why)) {
    Regenerate(c);
    detail = why;
    return Status::Fail;
  }
  Regenerate(c);
  bool blastIn = !A.blasts.empty();
  for (const ExplosionOp& e : A.blasts)
    blastIn = blastIn && A.basin.Has(e.x, e.y, e.z);
  bool same = A.blasts.size() == B.blasts.size() && A.hash == B.hash &&
              A.firstBlastTick == B.firstBlastTick;
  for (size_t i = 0; same && i < A.blasts.size(); i++)
    same = A.blasts[i].x == B.blasts[i].x && A.blasts[i].y == B.blasts[i].y &&
           A.blasts[i].z == B.blasts[i].z && A.blasts[i].radius == B.blasts[i].radius;
  const uint32_t maxWait = (uint32_t)BaselineNumber("chemSodium.firstBlastTicksMax", 30);
  const bool quick = A.firstBlastTick != 0 && A.firstBlastTick <= maxWait;
  const bool crater = A.stoneEnd < A.stone0;
  const bool products = A.lyeMax > 0 && A.hydrogenMax > 0 && A.fireMax > 0;
  // Rule 2: a lump of sodium is a few bangs, never a hundred.
  const bool bounded = A.blasts.size() <= 4u * 90u;
  RecordObserved("chemSodium.blasts", (double)A.blasts.size());
  RecordObserved("chemSodium.firstBlastTick", (double)A.firstBlastTick);
  const bool ok = quick && blastIn && crater && products && same && bounded;
  const ExplosionOp e0 = A.blasts.empty() ? ExplosionOp{0, 0, 0, 0, 0, 0, 0, 0} : A.blasts[0];
  detail = Format(
      "%zu blast(s), first %u ticks after the drop (allow %u) at (%d,%d,%d) r%d p%d "
      "%s; %llu effect firings drained, %llu merged/refused, %llu fire+smoke cells "
      "laid; crater: stone %u -> %u; seen lye %u, hydrogen %u, fire %u; sodium left "
      "%u; run twice: %s (hash %08x vs %08x)",
      A.blasts.size(), A.firstBlastTick, maxWait, e0.x, e0.y, e0.z, e0.radius, e0.power,
      blastIn ? "in the basin" : "OUTSIDE the basin", (unsigned long long)A.events,
      (unsigned long long)A.refused, (unsigned long long)A.aftermath, A.stone0,
      A.stoneEnd, A.lyeMax, A.hydrogenMax, A.fireMax, A.sodiumEnd,
      same ? "IDENTICAL" : "DIVERGED", A.hash, B.hash);
  std::printf("chem-sodium: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// chem-acid-fumes
// ---------------------------------------------------------------------------
Status GateChemAcidFumes(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  const int mGlass = MatId(c, "glass"), mStone = MatId(c, "stone"),
            mIron = MatId(c, "iron"), mAcid = MatId(c, "acid"),
            mNox = MatId(c, "noxious_gas");
  if (mGlass < 0 || mStone < 0 || mIron < 0 || mAcid < 0 || mNox < 0) {
    detail = "missing one of glass/stone/iron/acid/noxious_gas";
    return Status::Fail;
  }
  Regenerate(c);
  const int half = 5;
  const IVec3 s = Site(c, 260, half, 3);
  const Box inner{{s.x - half, s.y + 1, s.z - half}, {s.x + half, s.y + 7, s.z + half}};
  const Box watch{{inner.lo.x - 1, inner.lo.y - 1, inner.lo.z - 1},
                  {inner.hi.x + 1, inner.hi.y + 14, inner.hi.z + 1}};
  std::vector<CellOp> build, stuff, pool;
  Vessel(build, inner, inner.hi.y, (uint32_t)mGlass, (uint32_t)mStone);
  // A stone slab on the floor, an iron tile and a glass block standing on the
  // GLASS floor beside it. Both stand on the floor itself, not on the slab:
  // with the slab under them, the acid eating the slab away left them islands
  // and the island scan carried them off as debris -- which read as "acid ate
  // the glass" (the first cut of this gate: 517 -> 505, a 12-voxel shard).
  // The iron is one layer for the same reason: an iron column eaten from the
  // bottom drops its top as debris, not as dissolved iron.
  Fill(stuff, Box{{inner.lo.x + 1, inner.lo.y, inner.lo.z + 1},
                  {inner.lo.x + 2, inner.lo.y, inner.lo.z + 2}},
       (uint32_t)mIron, 0);
  Fill(stuff, Box{{inner.hi.x - 2, inner.lo.y, inner.hi.z - 2},
                  {inner.hi.x - 1, inner.lo.y + 1, inner.hi.z - 1}},
       (uint32_t)mGlass, 0);
  for (int z = inner.lo.z; z <= inner.hi.z; z++)
    for (int x = inner.lo.x; x <= inner.hi.x; x++)
      stuff.push_back({World::SlotCellIndex({x, inner.lo.y, z}),
                       PackVoxNew((uint32_t)mStone, 0) | kCellOpIfAir});
  // Acid three deep over the slab, IfAir so the blocks stay.
  for (int z = inner.lo.z; z <= inner.hi.z; z++)
    for (int y = inner.lo.y + 1; y <= inner.lo.y + 3; y++)
      for (int x = inner.lo.x; x <= inner.hi.x; x++)
        pool.push_back({World::SlotCellIndex({x, y, z}), PackVoxNew((uint32_t)mAcid, 7u) | kCellOpIfAir});

  uint32_t t = 72000;
  support::TickCursor tick{c, t, IVec3{s.x >> 4, s.y >> 4, s.z >> 4}};
  tick(std::vector<BrushOp>{}, build);
  tick(std::vector<BrushOp>{}, stuff);
  const std::vector<uint32_t> h0 = Census(c, watch);
  tick(std::vector<BrushOp>{}, pool);
  const int kTicks = 240;
  uint32_t noxMax = 0, noxSightings = 0;
  for (int i = 1; i <= kTicks; i++) {
    tick();
    if (i % 4 == 0) {
      const uint32_t n = Census(c, watch)[mNox];
      noxMax = std::max(noxMax, n);
      noxSightings += n;
    }
  }
  const std::vector<uint32_t> h1 = Census(c, watch);
  Regenerate(c);
  const bool fumes = noxMax > 0;
  const bool ateStone = h1[mStone] < h0[mStone];
  const bool ateIron = h1[mIron] < h0[mIron];
  const bool glassHeld = h1[mGlass] == h0[mGlass];
  // Fuming, not a flood: the gas at its peak is a fraction of what was eaten.
  const uint32_t eaten = (h0[mStone] - std::min(h0[mStone], h1[mStone])) +
                         (h0[mIron] - std::min(h0[mIron], h1[mIron]));
  RecordObserved("chemAcidFumes.noxMax", (double)noxMax);
  const bool ok = fumes && ateStone && ateIron && glassHeld;
  detail = Format(
      "over %d ticks: noxious gas peak %u cells (%u cell-samples), stone %u -> %u, "
      "iron %u -> %u (%u eaten in all), glass %u -> %u (%s)",
      kTicks, noxMax, noxSightings, h0[mStone], h1[mStone], h0[mIron], h1[mIron],
      eaten, h0[mGlass], h1[mGlass], glassHeld ? "untouched" : "EATEN");
  std::printf("chem-acid-fumes: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// chem-electrolysis
// ---------------------------------------------------------------------------
Status GateChemElectrolysis(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  const int mStone = MatId(c, "stone");
  const int mGlass = MatId(c, "glass"), mLava = MatId(c, "lava"),
            mSalt = MatId(c, "salt"), mMolten = MatId(c, "molten_salt"),
            mSpark = MatId(c, "spark"), mSodium = MatId(c, "sodium"),
            mCl = MatId(c, "chlorine");
  if (mStone < 0 || mGlass < 0 || mLava < 0 || mSalt < 0 || mMolten < 0 || mSpark < 0 ||
      mSodium < 0 || mCl < 0) {
    detail = "missing one of glass/lava/salt/molten_salt/spark/sodium/chlorine";
    return Status::Fail;
  }
  Regenerate(c);
  const int half = 3;
  const IVec3 s = Site(c, 320, 12, 3);
  // A melts salt on lava. B electrolyses a pool of molten salt, THREE TIMES:
  // pools whose floor sits at each of the three residues of world y mod 3.
  //
  // WHY THREE (2026-10-03, CLAUDE.md rule 6). This gate was red at origin and
  // green only at the window `streaming` used to leave behind; a pool at site
  // y 220/223 failed where 221/222 passed. That is the CA's colour lattice,
  // not a missed cell: sim_step runs the 27 phases in the order x, then y,
  // then z (simulation.cpp's phase table), and a SPARK is a gas that rises one
  // cell in its OWN phase on substep 0, right after its reactions.
  // Electrolysis is authored from the molten salt's side (`molten_salt` +
  // tag:electric), so a pool cell sees the spark only if its phase runs
  // BEFORE the spark's. With the pool at y = 2 (mod 3) the spark above it is
  // at y = 0 (mod 3), whose phase runs first in every column: the spark has
  // risen before the pool looks, every tick, at every cell. One pool height in
  // three can never be electrolysed -- in the game, not just here -- and a
  // fixture that tests one height measures where the site happened to land.
  // The claim is "molten salt shocked with sparks splits", so it must hold at
  // all three heights, and the detail names the one that does not.
  const Box aIn{{s.x - 10 - half, s.y + 1, s.z - half}, {s.x - 10 + half, s.y + 6, s.z + half}};
  Box bIn[3];
  for (int k = 0; k < 3; k++) {
    const int bz = s.z + (k - 1) * 10;
    bIn[k] = Box{{s.x + 4 - half, s.y + 1 + k, bz - half},
                 {s.x + 4 + half, s.y + 6 + k, bz + half}};
  }
  auto grow = [](const Box& b, int up) {
    return Box{{b.lo.x - 1, b.lo.y - 1, b.lo.z - 1}, {b.hi.x + 1, b.hi.y + up, b.hi.z + 1}};
  };
  const Box aWatch = grow(aIn, 8);
  std::vector<CellOp> build, fillA, fillB;
  Vessel(build, aIn, aIn.hi.y, (uint32_t)mGlass, (uint32_t)mStone);
  for (int k = 0; k < 3; k++)
    Vessel(build, bIn[k], bIn[k].hi.y, (uint32_t)mGlass, (uint32_t)mStone);
  Fill(fillA, Box{{aIn.lo.x, aIn.lo.y, aIn.lo.z}, {aIn.hi.x, aIn.lo.y, aIn.hi.z}},
       (uint32_t)mLava, 7u);
  Fill(fillA, Box{{aIn.lo.x + 1, aIn.lo.y + 1, aIn.lo.z + 1},
                  {aIn.hi.x - 1, aIn.lo.y + 1, aIn.hi.z - 1}},
       (uint32_t)mSalt, 0);
  for (int k = 0; k < 3; k++)
    Fill(fillB, Box{{bIn[k].lo.x, bIn[k].lo.y, bIn[k].lo.z},
                    {bIn[k].hi.x, bIn[k].lo.y, bIn[k].hi.z}},
         (uint32_t)mMolten, 7u);

  uint32_t t = 73000;
  support::TickCursor tick{c, t, IVec3{s.x >> 4, s.y >> 4, s.z >> 4}};
  tick(std::vector<BrushOp>{}, build);
  tick(std::vector<BrushOp>{}, fillA);
  tick(std::vector<BrushOp>{}, fillB);
  uint32_t moltenA = 0;
  // Per pool: what it made, and the attribution for one that never splits
  // (was the pool there, did the sparks land, what did it turn into instead).
  uint32_t sodiumB[3] = {}, chlorineB[3] = {}, moltenB[3] = {}, sparkB[3] = {},
           saltB[3] = {}, moltenB2[3] = {};
  const int kTicks = 160;
  for (int i = 1; i <= kTicks; i++) {
    std::vector<CellOp> zap;
    // Sparks over each B pool for ticks 2..8 (the bench's Electrify, done by
    // hand): IfAir, one layer above the molten pool.
    if (i >= 2 && i <= 8)
      for (int k = 0; k < 3; k++)
        for (int z = bIn[k].lo.z; z <= bIn[k].hi.z; z++)
          for (int x = bIn[k].lo.x; x <= bIn[k].hi.x; x++)
            zap.push_back({World::SlotCellIndex({x, bIn[k].lo.y + 1, z}),
                           PackVoxNew((uint32_t)mSpark, 0) | kCellOpIfAir});
    tick(std::vector<BrushOp>{}, zap);
    if (i % 2 == 0) {
      moltenA = std::max(moltenA, Census(c, aWatch)[mMolten]);
      for (int k = 0; k < 3; k++) {
        const std::vector<uint32_t> hb = Census(c, grow(bIn[k], 8));
        sodiumB[k] = std::max(sodiumB[k], hb[mSodium]);
        chlorineB[k] = std::max(chlorineB[k], hb[mCl]);
        if (i == 2) moltenB2[k] = hb[mMolten];
        moltenB[k] = std::max(moltenB[k], hb[mMolten]);
        sparkB[k] = std::max(sparkB[k], hb[mSpark]);
        saltB[k] = std::max(saltB[k], hb[mSalt]);
      }
    }
  }
  Regenerate(c);
  const bool melts = moltenA > 0;
  bool splits = true;
  std::string pools;
  for (int k = 0; k < 3; k++) {
    const int py = bIn[k].lo.y;
    const int res = ((py % 3) + 3) % 3, sres = (res + 1) % 3;
    const bool split = sodiumB[k] > 0 && chlorineB[k] > 0;
    splits = splits && split;
    // Same x and z column, so the spark's phase runs first exactly when its
    // y residue is the smaller one.
    pools += Format(
        "%s pool y %d (y%%3 %d, spark %d: spark phase %s): sodium %u, chlorine "
        "%u (%s) [molten %u at tick 2, peak %u; spark peak %u; salt peak %u]",
        k ? " |" : "", py, res, sres, sres < res ? "FIRST" : "after", sodiumB[k],
        chlorineB[k], split ? "split" : "NO ELECTROLYSIS", moltenB2[k], moltenB[k],
        sparkB[k], saltB[k]);
  }
  const bool ok = melts && splits;
  detail = Format(
      "A salt on lava: molten salt peak %u cells (%s); B molten salt + sparks at "
      "three lattice heights:%s; site (%d,%d,%d)",
      moltenA, melts ? "melts" : "NEVER MELTED", pools.c_str(), s.x, s.y, s.z);
  std::printf("chem-electrolysis: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// chem-toxic
// ---------------------------------------------------------------------------
Status GateChemToxic(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  const int mCl = MatId(c, "chlorine"), mSkin = MatId(c, "skin"),
            mCooked = MatId(c, "flesh_cooked");
  if (mCl < 0 || mSkin < 0 || mCooked < 0) {
    detail = "missing one of chlorine/skin/flesh_cooked";
    return Status::Fail;
  }
  int def = -1;
  for (size_t i = 0; i < c.mobs.Defs().size(); i++)
    if (c.mobs.Defs()[i].name == "human") def = (int)i;
  if (def < 0)
    for (size_t i = 0; i < c.mobs.Defs().size(); i++)
      if (c.mobs.Defs()[i].name == kAvatarDefName) def = (int)i;
  if (def < 0) {
    detail = "no human / avatar def";
    return Status::Fail;
  }
  const int nLimbs = (int)c.mobs.Defs()[def].limbs.size();
  const int root = c.mobs.Defs()[def].rootLimb;
  Regenerate(c);
  const IVec3 o = c.world.WindowOrigin();
  const int sx = o.x * (int)kChunk + 180, sz = o.z * (int)kChunk + 180;
  const int h = World::TerrainHeight(sx, sz, kDefaultSeed);
  const uint64_t id = c.mobs.Spawn(def, {sx, h + 1, sz});
  if (!id) {
    Regenerate(c);
    detail = "spawn refused";
    return Status::Fail;
  }
  auto census = [&](uint32_t mat) {
    uint32_t n = 0;
    for (int li = 0; li < nLimbs; li++) n += c.mobs.LimbMaterialCount(id, li, mat);
    return n;
  };
  uint32_t t = 74000;
  support::TickCursor tick{c, t, IVec3{sx >> 4, h >> 4, sz >> 4}};
  for (int i = 0; i < 10; i++) tick();  // stand up, settle
  const uint32_t skin0 = census((uint32_t)mSkin), cooked0 = census((uint32_t)mCooked);
  const float hp0 = c.mobs.TotalHp(id), frac0 = c.mobs.BurnFraction(id);
  const int kTicks = 150;
  for (int i = 0; i < kTicks; i++) {
    // A standing cloud of chlorine around the body, topped up every tick
    // (IfAir: it never overwrites the ground or the creature's own cell
    // content). Chlorine sinks, so the top-up is what keeps the head in it.
    support::TickOps pre;
    const Vec3 at = c.mobs.LimbVoxelPos(id, root, 0);
    const IVec3 b{ifloor(at.x), ifloor(at.y), ifloor(at.z)};
    for (int dy = -12; dy <= 14; dy++)
      for (int dz = -4; dz <= 4; dz++)
        for (int dx = -4; dx <= 4; dx++) {
          const IVec3 cc{b.x + dx, b.y + dy, b.z + dz};
          if (!c.world.CellInWindow(cc)) continue;
          pre.cells.push_back({World::SlotCellIndex(cc),
                               PackVoxNew((uint32_t)mCl, 0) | kCellOpIfAir});
        }
    tick.chunk = IVec3{b.x >> 4, b.y >> 4, b.z >> 4};
    tick(pre);
  }
  // Let the burnt-fraction recount (every 8 ticks while dirty) catch up.
  for (int i = 0; i < 10; i++) tick();
  const uint32_t skin1 = census((uint32_t)mSkin), cooked1 = census((uint32_t)mCooked);
  const float hp1 = c.mobs.TotalHp(id), frac1 = c.mobs.BurnFraction(id);
  const float cap1 = c.mobs.BurnHealthCap(id);
  Regenerate(c);
  const bool blistered = cooked1 > cooked0 && skin1 < skin0;
  const bool hurt = frac1 > frac0;
  const bool ok = blistered && hurt;
  RecordObserved("chemToxic.cooked", (double)(cooked1 - std::min(cooked0, cooked1)));
  detail = Format(
      "%d ticks in chlorine: skin %u -> %u, flesh_cooked %u -> %u; burnt fraction "
      "%.3f -> %.3f, health cap %.3f, hp %.1f -> %.1f",
      kTicks, skin0, skin1, cooked0, cooked1, frac0, frac1, cap1, hp0, hp1);
  std::printf("chem-toxic: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ===========================================================================
// PACKAGE E (docs/PLAN_alchemy_chemistry.md, the creative expansion): the
// headline recipes in the world. Same discipline as package A's gates above:
// a sealed fixture over the harness terrain, THE tick, a regenerated world on
// the way out. Thresholds live in tests/baseline.json (chemGunpowder.*,
// chemThermite.*, chemFrost.*, chemHoly.*).
// ===========================================================================

// The state nibble a freshly placed cell of `m` wants: a liquid is placed
// full-ish (7, as package A's gates do), anything else as variant 0.
uint32_t PlaceState(Ctx& c, int m) {
  return m > 0 && c.mats[(size_t)m].gpu.klass == CLASS_LIQUID ? 7u : 0u;
}

int ResolveAll(Ctx& c, std::initializer_list<std::pair<int*, const char*>> want,
               std::string& missing) {
  int bad = 0;
  for (const auto& w : want) {
    *w.first = MatId(c, w.second);
    if (*w.first < 0) {
      missing += std::string(missing.empty() ? "" : ", ") + w.second;
      bad++;
    }
  }
  return bad;
}

// ---------------------------------------------------------------------------
// chem-gunpowder: a bed of gunpowder in a stone basin, one ember laid on it.
// It must GO OFF (a real ExplosionOp inside the basin, soon), cut a crater,
// burn through most of the powder, and stay a handful of bangs (the cap).
// ---------------------------------------------------------------------------
Status GateChemGunpowder(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  int mStone, mGp, mEmber, mFire;
  std::string missing;
  if (ResolveAll(c, {{&mStone, "stone"}, {&mGp, "gunpowder"}, {&mEmber, "ember"}, {&mFire, "fire"}},
                 missing)) {
    detail = "missing " + missing;
    return Status::Fail;
  }
  Regenerate(c);
  const int half = 4;
  const IVec3 s = Site(c, 230, half, 3);
  const Box inner{{s.x - half, s.y + 1, s.z - half}, {s.x + half, s.y + 6, s.z + half}};
  const Box watch{{inner.lo.x - 1, s.y - 2, inner.lo.z - 1},
                  {inner.hi.x + 1, inner.hi.y + 12, inner.hi.z + 1}};
  std::vector<CellOp> build, powder, match;
  Vessel(build, inner, inner.hi.y, (uint32_t)mStone, (uint32_t)mStone);
  Fill(build, Box{{inner.lo.x - 1, s.y - 2, inner.lo.z - 1}, {inner.hi.x + 1, s.y - 1, inner.hi.z + 1}},
       (uint32_t)mStone, 0);
  Fill(powder, Box{inner.lo, {inner.hi.x, inner.lo.y + 1, inner.hi.z}}, (uint32_t)mGp, 0);
  match.push_back({World::SlotCellIndex({s.x, inner.lo.y + 2, s.z}),
                   PackVoxNew((uint32_t)mEmber, 0) | kCellOpIfAir});
  uint32_t t = 75000;
  support::TickCursor tick{c, t, IVec3{s.x >> 4, s.y >> 4, s.z >> 4}};
  tick(std::vector<BrushOp>{}, build);
  tick(std::vector<BrushOp>{}, powder);
  for (int i = 0; i < 8; i++) tick();
  const std::vector<uint32_t> h0 = Census(c, watch);
  auto& rf = tick.Rig().Authority().reactFx;
  const uint64_t b0 = rf.blasts, r0 = rf.refused;
  tick(std::vector<BrushOp>{}, match);
  const int kTicks = 120;
  uint32_t firstBlast = 0, fireMax = 0;
  std::vector<ExplosionOp> blasts;
  for (int i = 1; i <= kTicks; i++) {
    const uint64_t before = rf.blasts;
    tick();
    if (rf.blasts > before) {
      if (!firstBlast) firstBlast = (uint32_t)i;
      for (uint64_t k = before; k < rf.blasts; k++)
        blasts.push_back(rf.recent[rf.recent.size() - (size_t)std::min<uint64_t>(rf.blasts - k, rf.recent.size())]);
    }
    if (i % 3 == 0) fireMax = std::max(fireMax, Census(c, watch)[mFire]);
  }
  const std::vector<uint32_t> h1 = Census(c, watch);
  const uint64_t nBlasts = rf.blasts - b0, refused = rf.refused - r0;
  Regenerate(c);
  // The FIRST blast must be in the basin (that is the ember lighting the
  // bed). Later ones may not be: the bed blows its own basin open and flings
  // burning powder out, which then goes off where it lands -- counted, not
  // failed.
  const bool inBasin = !blasts.empty() && watch.Has(blasts[0].x, blasts[0].y, blasts[0].z);
  size_t outside = 0;
  for (const ExplosionOp& e : blasts) outside += watch.Has(e.x, e.y, e.z) ? 0u : 1u;
  const uint32_t maxWait = (uint32_t)BaselineNumber("chemGunpowder.firstBlastTicksMax", 40);
  const double leftMax = BaselineNumber("chemGunpowder.leftFracMax", 0.5);
  const bool quick = firstBlast != 0 && firstBlast <= maxWait;
  const bool crater = h1[mStone] < h0[mStone];
  const bool burnt = (double)h1[mGp] <= leftMax * (double)h0[mGp];
  // Rule 2: the per-tick cap is 4 reaction blasts; a bed of powder must read
  // as a string of bangs, never as a blast per grain.
  const bool bounded = nBlasts <= (uint64_t)BaselineNumber("chemGunpowder.blastsMax", 60);
  RecordObserved("chemGunpowder.blasts", (double)nBlasts);
  RecordObserved("chemGunpowder.firstBlastTick", (double)firstBlast);
  const bool ok = quick && inBasin && crater && burnt && bounded;
  detail = Format(
      "%llu blast(s) (allow %.0f), first %u ticks after the ember (allow %u), %s, %zu of the "
      "%zu recorded outside it (flung powder); %llu "
      "merged/refused; gunpowder %u -> %u (allow %.0f%% left); crater: stone %u -> %u; fire seen %u",
      (unsigned long long)nBlasts, BaselineNumber("chemGunpowder.blastsMax", 60), firstBlast, maxWait,
      inBasin ? "the first in the basin" : "THE FIRST NOT IN THE BASIN", outside, blasts.size(),
      (unsigned long long)refused, h0[mGp],
      h1[mGp], 100 * leftMax, h0[mStone], h1[mStone], fireMax);
  std::printf("chem-gunpowder: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// chem-thermite: thermite on an iron plate, lit by a drop of lava. It must
// burn to molten iron (and mostly be gone), FLASH (the world's flash effect
// laid glare), leave iron behind as the melt freezes -- and do all of it bit
// for bit twice (the flash path is new C++ in the tick).
// ---------------------------------------------------------------------------
struct ThermiteRun {
  uint32_t th0 = 0, thEnd = 0, moltenMax = 0, iron0 = 0, ironEnd = 0, glareMax = 0;
  uint64_t flashes = 0, flashCells = 0;
  uint32_t hash = 0;
};

bool RunThermite(Ctx& c, ThermiteRun& R, std::string& why) {
  int mStone, mTh, mIron, mMolten, mLava, mGlare;
  std::string missing;
  if (ResolveAll(c, {{&mStone, "stone"}, {&mTh, "thermite"}, {&mIron, "iron"}, {&mMolten, "molten_iron"},
                     {&mLava, "lava"}, {&mGlare, "glare"}},
                 missing)) {
    why = "missing " + missing;
    return false;
  }
  Regenerate(c);
  const int half = 3;
  const IVec3 s = Site(c, 290, half, 3);
  const Box inner{{s.x - half, s.y + 1, s.z - half}, {s.x + half, s.y + 7, s.z + half}};
  const Box watch{{inner.lo.x - 1, s.y - 1, inner.lo.z - 1},
                  {inner.hi.x + 1, inner.hi.y + 10, inner.hi.z + 1}};
  std::vector<CellOp> build, fill, light;
  Vessel(build, inner, inner.hi.y, (uint32_t)mStone, (uint32_t)mStone);
  Fill(fill, Box{inner.lo, {inner.hi.x, inner.lo.y, inner.hi.z}}, (uint32_t)mIron, 0);
  Fill(fill, Box{{inner.lo.x, inner.lo.y + 1, inner.lo.z}, {inner.hi.x, inner.lo.y + 2, inner.hi.z}},
       (uint32_t)mTh, 0);
  light.push_back({World::SlotCellIndex({s.x, inner.lo.y + 3, s.z}),
                   PackVoxNew((uint32_t)mLava, 7u) | kCellOpIfAir});
  uint32_t t = 76000;
  support::TickCursor tick{c, t, IVec3{s.x >> 4, s.y >> 4, s.z >> 4}};
  tick(std::vector<BrushOp>{}, build);
  tick(std::vector<BrushOp>{}, fill);
  for (int i = 0; i < 6; i++) tick();
  {
    const std::vector<uint32_t> h = Census(c, watch);
    R.th0 = h[mTh];
    R.iron0 = h[mIron];
  }
  auto& rf = tick.Rig().Authority().reactFx;
  const uint64_t f0 = rf.flashes, fc0 = rf.flashCells;
  tick(std::vector<BrushOp>{}, light);
  for (int i = 1; i <= 240; i++) {
    tick();
    if (i % 2 == 0) {
      const std::vector<uint32_t> h = Census(c, watch);
      R.moltenMax = std::max(R.moltenMax, h[mMolten]);
      R.glareMax = std::max(R.glareMax, h[mGlare]);
    }
  }
  const std::vector<uint32_t> end = Census(c, watch, &R.hash);
  R.thEnd = end[mTh];
  R.ironEnd = end[mIron];
  R.flashes = rf.flashes - f0;
  R.flashCells = rf.flashCells - fc0;
  return true;
}

Status GateChemThermite(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  ThermiteRun A, B;
  std::string why;
  if (!RunThermite(c, A, why) || !RunThermite(c, B, why)) {
    Regenerate(c);
    detail = why;
    return Status::Fail;
  }
  Regenerate(c);
  const double leftMax = BaselineNumber("chemThermite.leftFracMax", 0.5);
  const bool burns = A.moltenMax > 0 && (double)A.thEnd <= leftMax * (double)A.th0;
  const bool flashes = A.flashes > 0 && A.flashCells > 0;
  const bool freezes = A.ironEnd >= A.iron0;
  const bool same = A.hash == B.hash && A.flashes == B.flashes && A.flashCells == B.flashCells;
  RecordObserved("chemThermite.moltenMax", (double)A.moltenMax);
  RecordObserved("chemThermite.flashes", (double)A.flashes);
  const bool ok = burns && flashes && freezes && same;
  detail = Format(
      "thermite %u -> %u (allow %.0f%% left), molten iron peak %u; %llu flash(es), %llu glare "
      "cells laid, glare seen %u; iron %u -> %u (%s); run twice: %s (hash %08x vs %08x)",
      A.th0, A.thEnd, 100 * leftMax, A.moltenMax, (unsigned long long)A.flashes,
      (unsigned long long)A.flashCells, A.glareMax, A.iron0, A.ironEnd,
      freezes ? "the melt froze to iron" : "IRON LOST", same ? "IDENTICAL" : "DIVERGED", A.hash, B.hash);
  std::printf("chem-thermite: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// chem-frost: frost salt sprinkled on a pool FREEZES it (several voxels of ice
// a grain, the salt spent doing it) and, on a pool of lava, quenches it to
// stone.
// ---------------------------------------------------------------------------
Status GateChemFrost(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  int mStone, mGlass, mWater, mIce, mLava, mFrost;
  std::string missing;
  if (ResolveAll(c, {{&mStone, "stone"}, {&mGlass, "glass"}, {&mWater, "water"}, {&mIce, "ice"},
                     {&mLava, "lava"}, {&mFrost, "frost_salt"}},
                 missing)) {
    detail = "missing " + missing;
    return Status::Fail;
  }
  Regenerate(c);
  const int half = 3;
  const IVec3 s = Site(c, 330, 11, 3);
  const Box aIn{{s.x - 6 - half, s.y + 1, s.z - half}, {s.x - 6 + half, s.y + 5, s.z + half}};
  const Box bIn{{s.x + 6 - half, s.y + 1, s.z - half}, {s.x + 6 + half, s.y + 5, s.z + half}};
  const Box aWatch{{aIn.lo.x - 1, aIn.lo.y - 1, aIn.lo.z - 1}, {aIn.hi.x + 1, aIn.hi.y + 4, aIn.hi.z + 1}};
  std::vector<CellOp> build, fill, salt;
  Vessel(build, aIn, aIn.hi.y, (uint32_t)mStone, (uint32_t)mStone);
  Vessel(build, bIn, bIn.hi.y, (uint32_t)mGlass, (uint32_t)mStone);
  Fill(fill, Box{aIn.lo, {aIn.hi.x, aIn.lo.y + 2, aIn.hi.z}}, (uint32_t)mWater, 7u);
  Fill(fill, Box{bIn.lo, {bIn.hi.x, bIn.lo.y, bIn.hi.z}}, (uint32_t)mLava, 7u);
  for (int dz = -1; dz <= 1; dz++)
    for (int dx = -1; dx <= 1; dx++) {
      salt.push_back({World::SlotCellIndex({s.x - 6 + dx, aIn.lo.y + 4, s.z + dz}),
                      PackVoxNew((uint32_t)mFrost, 0) | kCellOpIfAir});
      salt.push_back({World::SlotCellIndex({s.x + 6 + dx, bIn.lo.y + 2, s.z + dz}),
                      PackVoxNew((uint32_t)mFrost, 0) | kCellOpIfAir});
    }
  uint32_t t = 77000;
  support::TickCursor tick{c, t, IVec3{s.x >> 4, s.y >> 4, s.z >> 4}};
  tick(std::vector<BrushOp>{}, build);
  tick(std::vector<BrushOp>{}, fill);
  for (int i = 0; i < 12; i++) tick();
  const uint32_t ice0 = Census(c, aWatch)[mIce];
  tick(std::vector<BrushOp>{}, salt);
  uint32_t iceMax = 0, stoneB = 0;
  for (int i = 1; i <= 150; i++) {
    tick();
    if (i % 3 == 0) {
      iceMax = std::max(iceMax, Census(c, aWatch)[mIce]);
      stoneB = std::max(stoneB, Census(c, bIn)[mStone]);  // inside the GLASS box: only a quench makes stone
    }
  }
  const std::vector<uint32_t> ha = Census(c, aWatch);
  Regenerate(c);
  const uint32_t iceMin = (uint32_t)BaselineNumber("chemFrost.iceMin", 12);
  const bool freezes = iceMax >= ice0 + iceMin;
  const bool spent = ha[mFrost] < 9;
  const bool quench = stoneB > 0;
  RecordObserved("chemFrost.ice", (double)(iceMax - std::min(iceMax, ice0)));
  const bool ok = freezes && spent && quench;
  detail = Format(
      "9 grains on a pool: ice %u -> peak %u (want +%u), frost salt left %u of 9 (%s); 9 grains on "
      "lava: stone %u in the glass box (%s)",
      ice0, iceMax, iceMin, ha[mFrost], spent ? "spent" : "NOT SPENT", stoneB,
      quench ? "quenched" : "NO QUENCH");
  std::printf("chem-frost: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// chem-holy-water: (grid) salt in enchanted water makes holy water; holy
// water on rotflesh chars it where plain water does nothing; (body) a zombie
// standing in holy water has its rot SEARED -- rotflesh down, flesh_charred up
// -- while a zombie standing in plain water beside it is not.
// ---------------------------------------------------------------------------
Status GateChemHolyWater(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  int mStone, mGlass, mEw, mSalt, mHoly, mWater, mRot, mChar;
  std::string missing;
  if (ResolveAll(c, {{&mStone, "stone"}, {&mGlass, "glass"}, {&mEw, "enchanted_water"}, {&mSalt, "salt"},
                     {&mHoly, "holy_water"}, {&mWater, "water"}, {&mRot, "rotflesh"},
                     {&mChar, "flesh_charred"}},
                 missing)) {
    detail = "missing " + missing;
    return Status::Fail;
  }
  int zdef = -1;
  for (size_t i = 0; i < c.mobs.Defs().size(); i++)
    if (c.mobs.Defs()[i].name == "zombie") zdef = (int)i;
  if (zdef < 0) {
    detail = "no zombie def";
    return Status::Fail;
  }
  // ---- grid ----
  Regenerate(c);
  const int half = 2;
  const IVec3 s = Site(c, 250, 16, 3);
  Box in[3];
  for (int k = 0; k < 3; k++)
    in[k] = Box{{s.x - 12 + 12 * k - half, s.y + 1, s.z - half}, {s.x - 12 + 12 * k + half, s.y + 5, s.z + half}};
  auto grow = [](const Box& b) {
    return Box{{b.lo.x - 1, b.lo.y - 1, b.lo.z - 1}, {b.hi.x + 1, b.hi.y + 4, b.hi.z + 1}};
  };
  std::vector<CellOp> build, fill, salt;
  for (int k = 0; k < 3; k++) Vessel(build, in[k], in[k].hi.y, (uint32_t)mGlass, (uint32_t)mStone);
  // A: enchanted water, salt sprinkled on. B: a rotflesh floor under holy
  // water. C: the same floor under plain water (the control).
  Fill(fill, Box{in[0].lo, {in[0].hi.x, in[0].lo.y + 1, in[0].hi.z}}, (uint32_t)mEw, 7u);
  Fill(fill, Box{in[1].lo, {in[1].hi.x, in[1].lo.y, in[1].hi.z}}, (uint32_t)mRot, 0);
  Fill(fill, Box{in[2].lo, {in[2].hi.x, in[2].lo.y, in[2].hi.z}}, (uint32_t)mRot, 0);
  std::vector<CellOp> pour;
  Fill(pour, Box{{in[1].lo.x, in[1].lo.y + 1, in[1].lo.z}, {in[1].hi.x, in[1].lo.y + 2, in[1].hi.z}},
       (uint32_t)mHoly, 7u);
  Fill(pour, Box{{in[2].lo.x, in[2].lo.y + 1, in[2].lo.z}, {in[2].hi.x, in[2].lo.y + 2, in[2].hi.z}},
       (uint32_t)mWater, 7u);
  for (int dz = -1; dz <= 1; dz++)
    for (int dx = -1; dx <= 1; dx++)
      salt.push_back({World::SlotCellIndex({s.x - 12 + dx, in[0].lo.y + 3, s.z + dz}),
                      PackVoxNew((uint32_t)mSalt, 0) | kCellOpIfAir});
  uint32_t t = 78000;
  uint32_t holyMade = 0, rotB0 = 0, rotC0 = 0, rotB1 = 0, rotC1 = 0, charB = 0, charC = 0;
  {
    support::TickCursor tick{c, t, IVec3{s.x >> 4, s.y >> 4, s.z >> 4}};
    tick(std::vector<BrushOp>{}, build);
    tick(std::vector<BrushOp>{}, fill);
    for (int i = 0; i < 4; i++) tick();
    rotB0 = Census(c, in[1])[mRot];
    rotC0 = Census(c, in[2])[mRot];
    tick(std::vector<BrushOp>{}, pour);
    tick(std::vector<BrushOp>{}, salt);
    for (int i = 1; i <= 120; i++) {
      tick();
      if (i % 4 == 0) holyMade = std::max(holyMade, Census(c, grow(in[0]))[mHoly]);
    }
    const std::vector<uint32_t> hb = Census(c, grow(in[1])), hc = Census(c, grow(in[2]));
    rotB1 = hb[mRot];
    rotC1 = hc[mRot];
    charB = hb[mChar];
    charC = hc[mChar];
  }
  const bool makes = holyMade > 0;
  const bool sears = rotB1 < rotB0 && charB > 0;
  const bool control = rotC1 == rotC0 && charC == 0;

  // ---- bodies ----
  Regenerate(c);
  const int ph = 4;
  const IVec3 p = Site(c, 200, 18, 3);
  const Box pit[2] = {
      Box{{p.x - 11 - ph, p.y + 1, p.z - ph}, {p.x - 11 + ph, p.y + 16, p.z + ph}},
      Box{{p.x + 11 - ph, p.y + 1, p.z - ph}, {p.x + 11 + ph, p.y + 16, p.z + ph}}};
  std::vector<CellOp> pits, pools;
  for (int k = 0; k < 2; k++) {
    Vessel(pits, pit[k], pit[k].hi.y, (uint32_t)mStone, (uint32_t)mStone);
    Fill(pools, Box{pit[k].lo, {pit[k].hi.x, pit[k].lo.y + 7, pit[k].hi.z}},
         (uint32_t)(k == 0 ? mHoly : mWater), 7u);
  }
  support::TickCursor tick{c, t, IVec3{p.x >> 4, p.y >> 4, p.z >> 4}};
  tick(std::vector<BrushOp>{}, pits);
  const uint64_t zA = c.mobs.Spawn(zdef, {p.x - 11, pit[0].lo.y, p.z});
  const uint64_t zB = c.mobs.Spawn(zdef, {p.x + 11, pit[1].lo.y, p.z});
  if (!zA || !zB) {
    Regenerate(c);
    detail = "zombie spawn refused";
    return Status::Fail;
  }
  const int nLimbs = (int)c.mobs.Defs()[zdef].limbs.size();
  auto census = [&](uint64_t id, int mat) {
    uint32_t n = 0;
    for (int li = 0; li < nLimbs; li++) n += c.mobs.LimbMaterialCount(id, li, (uint32_t)mat);
    return n;
  };
  for (int i = 0; i < 6; i++) tick();
  // A ZOMBIE IS NOT MADE OF ROT: its body is the human's tissue under a
  // palette filter, and rotflesh is what its BITE leaves in a victim (the
  // sidecar's bite.infect) -- the rot a turned corpse carries. So both are
  // bitten with it first, identically (same limbs, same points, same seed):
  // every limb whose centre is below the waterline, which is what the pool
  // will reach.
  std::vector<ParticleSpawn> spawns;
  int bitten = 0;
  for (int li = 0; li < nLimbs; li++) {
    for (int k = 0; k < 2; k++) {
      const uint64_t id = k == 0 ? zA : zB;
      const Vec3 at = c.mobs.LimbVoxelPos(id, li, 0);
      if (at.y > (float)(pit[k].lo.y + 6)) continue;
      const uint64_t lb = c.mobs.LimbBody(id, li);
      if (!lb) continue;
      ::BiteHit bt;
      bt.at = c.mobs.LimbVoxelPos(id, li, 4441u);
      bt.hp = 1.0f;
      bt.power = 0.7f;
      bt.infectMat = (uint16_t)mRot;
      bt.infectStain = c.mobs.Defs()[zdef].bite.infectStain;
      bt.seed = 0x40E7u + (uint32_t)li;
      if (c.mobs.BiteHit(lb, bt, c.world, spawns) && k == 0) bitten++;
    }
  }
  tick();
  const uint32_t aRot0 = census(zA, mRot), aChar0 = census(zA, mChar);
  const uint32_t bRot0 = census(zB, mRot), bChar0 = census(zB, mChar);
  tick(std::vector<BrushOp>{}, pools);
  for (int i = 0; i < 200; i++) tick();
  const uint32_t aRot1 = census(zA, mRot), aChar1 = census(zA, mChar);
  const uint32_t bRot1 = census(zB, mRot), bChar1 = census(zB, mChar);
  Regenerate(c);
  const bool bodySears = aChar1 > aChar0 && aRot1 < aRot0;
  const bool bodyControl = bChar1 <= bChar0;
  RecordObserved("chemHoly.zombieCharred", (double)(aChar1 - std::min(aChar1, aChar0)));
  const bool ok = makes && sears && control && bodySears && bodyControl;
  detail = Format(
      "salt in enchanted water: holy water peak %u (%s); rotflesh floor under holy water %u -> %u, "
      "charred %u (%s); under plain water %u -> %u, charred %u (%s); %d submerged limb(s) bitten with rot; ZOMBIE in holy water: rotflesh "
      "%u -> %u, flesh_charred %u -> %u (%s); zombie in plain water: rotflesh %u -> %u, charred %u -> %u (%s)",
      holyMade, makes ? "made" : "NONE", rotB0, rotB1, charB, sears ? "seared" : "NOT SEARED", rotC0,
      rotC1, charC, control ? "untouched" : "TOUCHED", bitten, aRot0, aRot1, aChar0, aChar1,
      bodySears ? "seared" : "NOT SEARED", bRot0, bRot1, bChar0, bChar1,
      bodyControl ? "not charred" : "CHARRED");
  std::printf("chem-holy-water: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// chem-recipes: EVERY pair recipe package E authored, in the world, at once.
// A grid of small glass boxes, each holding two reagents (checkered where both
// flow; a solid as the floor with the other on it), ticked together; each
// box must show its product. One row per rule family, so a renamed material
// or a rule that stopped matching is named, not averaged away. The explosive
// recipes are NOT here (a blast would wreck its neighbours): chem-gunpowder
// has gunpowder, and the fire-making rows sit at the far corner from the
// flammable ones.
// ---------------------------------------------------------------------------
struct Recipe {
  const char* a;
  const char* b;  // "air" = the other half of the checker is empty
  const char* product;
};
// Order is LAYOUT: index 0 is one corner of the grid and the last the far
// corner. Powders that burn or go off (black_mix, gunpowder, smoke powder,
// ether, hydrogen) are first; everything that makes fire or heat is last.
const Recipe kRecipes[] = {
    {"saltpeter", "charcoal", "black_mix"},
    {"black_mix", "sulfur", "gunpowder"},
    {"saltpeter", "sugar", "smoke_powder"},
    {"gunpowder", "water", "charcoal"},
    {"spirits", "acid", "ether"},
    {"ether", "air", "ether_vapour"},
    {"acid", "aluminium", "hydrogen"},
    {"rust", "aluminium", "thermite"},
    {"acid", "copper", "blue_vitriol"},
    {"acid", "saltpeter", "aqua_fortis"},
    {"aqua_fortis", "salt", "aqua_regia"},
    {"aqua_regia", "gold", "noxious_gas"},
    {"acid", "chalk", "choke_damp"},
    {"acid", "sugar", "charcoal"},
    {"slaked_lime", "choke_damp", "chalk"},
    {"quicklime", "water", "slaked_lime"},
    {"quicksilver", "sulfur", "cinnabar"},
    {"blue_vitriol", "iron", "copper"},
    {"salt", "enchanted_water", "holy_water"},
    {"holy_water", "rotflesh", "flesh_charred"},
    {"fairy_dust", "ichor", "slime"},
    {"slime", "salt", "water"},
    {"sunwater", "moonwater", "philosophers_stone"},
    {"philosophers_stone", "lead", "gold_dust"},
    {"luminous_spores", "moonwater", "glowcap"},
    {"syrup", "fungus", "spirits"},
    {"dragons_blood", "skin", "dragonhide"},
    {"frost_salt", "water", "ice"},
    {"molten_iron", "water", "iron"},
    {"chalk", "lava", "quicklime"},
    {"cinnabar", "lava", "quicksilver"},
    {"glowcap", "lava", "luminous_spores"},
    {"smoke_powder", "lava", "thick_smoke"},
    {"thermite", "lava", "molten_iron"},
    {"sulfur", "lava", "noxious_gas"},
    {"phosphorus", "air", "smoke"},
};

Status GateChemRecipes(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  const int n = (int)(sizeof(kRecipes) / sizeof(kRecipes[0]));
  const int mGlass = MatId(c, "glass"), mStone = MatId(c, "stone");
  std::vector<int> A(n), B(n), P(n);
  std::string missing;
  for (int i = 0; i < n; i++) {
    A[i] = MatId(c, kRecipes[i].a);
    B[i] = std::strcmp(kRecipes[i].b, "air") == 0 ? 0 : MatId(c, kRecipes[i].b);
    P[i] = MatId(c, kRecipes[i].product);
    if (A[i] < 0 || B[i] < 0 || P[i] < 0)
      missing += Format("%s%s+%s->%s", missing.empty() ? "" : ", ", kRecipes[i].a, kRecipes[i].b,
                        kRecipes[i].product);
  }
  if (mGlass < 0 || mStone < 0 || !missing.empty()) {
    detail = "unresolved: " + missing;
    return Status::Fail;
  }
  Regenerate(c);
  const int cols = 6, pitch = 10, w = 4;  // inner 4x3x4, 6 walls-to-walls apart
  const int rows = (n + cols - 1) / cols;
  const int span = cols * pitch;
  const IVec3 s = Site(c, 330, span / 2 + 2, 3);
  const int x0 = s.x - span / 2, z0 = s.z - (rows * pitch) / 2;
  const int y0 = s.y + 1;
  auto boxOf = [&](int i) {
    const int bx = x0 + (i % cols) * pitch + 2, bz = z0 + (i / cols) * pitch + 2;
    return Box{{bx, y0, bz}, {bx + w - 1, y0 + 2, bz + w - 1}};
  };
  std::vector<CellOp> build, fill;
  for (int i = 0; i < n; i++) {
    const Box b = boxOf(i);
    Vessel(build, b, b.hi.y + 1, (uint32_t)mGlass, (uint32_t)mStone);
    const bool aSolid = c.mats[(size_t)A[i]].gpu.klass == CLASS_SOLID;
    const bool bSolid = B[i] > 0 && c.mats[(size_t)B[i]].gpu.klass == CLASS_SOLID;
    for (int z = b.lo.z; z <= b.hi.z; z++)
      for (int y = b.lo.y; y <= b.lo.y + 1; y++)
        for (int x = b.lo.x; x <= b.hi.x; x++) {
          int m;
          if (aSolid || bSolid) {
            // The solid is the FLOOR (a floating solid cell is an island the
            // scan would cut loose); the other reagent lies on it.
            const int floorMat = aSolid ? A[i] : B[i], topMat = aSolid ? B[i] : A[i];
            m = y == b.lo.y ? floorMat : topMat;
          } else {
            m = ((x + y + z) & 1) ? A[i] : B[i];
          }
          fill.push_back({World::SlotCellIndex({x, y, z}),
                          m ? PackVoxNew((uint32_t)m, PlaceState(c, m)) : 0u});
        }
  }
  uint32_t t = 79000;
  support::TickCursor tick{c, t, IVec3{s.x >> 4, s.y >> 4, s.z >> 4}};
  tick(std::vector<BrushOp>{}, build);
  tick(std::vector<BrushOp>{}, fill);
  // One census of the whole grid a sample, bucketed per box (the box and 4
  // cells of air over it, inside its walls).
  std::vector<uint32_t> peak(n, 0);
  std::vector<uint32_t> buf(kChunkVol);
  const Box area{{x0, y0, z0}, {x0 + span - 1, y0 + 6, z0 + rows * pitch - 1}};
  auto sample = [&] {
    std::vector<uint32_t> cnt(n, 0);
    for (int cz = area.lo.z >> 4; cz <= (area.hi.z >> 4); cz++)
      for (int cy = area.lo.y >> 4; cy <= (area.hi.y >> 4); cy++)
        for (int cx = area.lo.x >> 4; cx <= (area.hi.x >> 4); cx++) {
          ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex({cx, cy, cz}), 1, buf.data(),
                         "chemRecipes");
          for (uint32_t k = 0; k < kChunkVol; k++) {
            const int x = (int)(k % 16) + cx * 16, y = (int)((k / 16) % 16) + cy * 16,
                      z = (int)(k / 256) + cz * 16;
            if (!area.Has(x, y, z)) continue;
            const int lx = (x - x0) % pitch - 2, lz = (z - z0) % pitch - 2;
            if (lx < 0 || lx >= w || lz < 0 || lz >= w) continue;
            const int i = ((z - z0) / pitch) * cols + (x - x0) / pitch;
            if (i >= n) continue;
            if ((int)(buf[k] & 0xFFFu) == P[i]) cnt[i]++;
          }
        }
    for (int i = 0; i < n; i++) peak[i] = std::max(peak[i], cnt[i]);
  };
  const int kTicks = (int)BaselineNumber("chemRecipes.ticks", 160);
  for (int i = 1; i <= kTicks; i++) {
    tick();
    if (i % 4 == 0) sample();
  }
  Regenerate(c);
  int made = 0;
  std::string fails, got;
  for (int i = 0; i < n; i++) {
    if (peak[i] > 0) made++;
    else
      fails += Format("%s%s+%s->%s", fails.empty() ? "" : ", ", kRecipes[i].a, kRecipes[i].b,
                      kRecipes[i].product);
    got += Format("%s%s %u", got.empty() ? "" : ", ", kRecipes[i].product, peak[i]);
  }
  RecordObserved("chemRecipes.made", (double)made);
  const bool ok = made == n;
  detail = Format("%d of %d recipes made their product in %d ticks%s%s [peaks: %s]", made, n, kTicks,
                  fails.empty() ? "" : "; NOT MADE: ", fails.c_str(), got.c_str());
  std::printf("chem-recipes: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& ChemGates() {
  static const std::vector<Gate> g = {
      {"chem-sodium", "sim", {}, false, GateChemSodium},
      {"chem-acid-fumes", "sim", {}, false, GateChemAcidFumes},
      {"chem-electrolysis", "sim", {}, false, GateChemElectrolysis},
      {"chem-toxic", "mob", {}, false, GateChemToxic},
      // Package E: the creative expansion's headline recipes.
      {"chem-gunpowder", "sim", {}, false, GateChemGunpowder},
      {"chem-thermite", "sim", {}, false, GateChemThermite},
      {"chem-frost", "sim", {}, false, GateChemFrost},
      {"chem-holy-water", "mob", {}, false, GateChemHolyWater},
      {"chem-recipes", "sim", {}, false, GateChemRecipes},
  };
  return g;
}

}  // namespace selftest
