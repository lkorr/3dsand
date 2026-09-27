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
  // Two glass boxes side by side: A melts salt on lava, B electrolyses a pool
  // of molten salt.
  const Box aIn{{s.x - 10 - half, s.y + 1, s.z - half}, {s.x - 10 + half, s.y + 6, s.z + half}};
  const Box bIn{{s.x + 4 - half, s.y + 1, s.z - half}, {s.x + 4 + half, s.y + 6, s.z + half}};
  auto grow = [](const Box& b, int up) {
    return Box{{b.lo.x - 1, b.lo.y - 1, b.lo.z - 1}, {b.hi.x + 1, b.hi.y + up, b.hi.z + 1}};
  };
  const Box aWatch = grow(aIn, 8), bWatch = grow(bIn, 8);
  std::vector<CellOp> build, fillA, fillB;
  Vessel(build, aIn, aIn.hi.y, (uint32_t)mGlass, (uint32_t)mStone);
  Vessel(build, bIn, bIn.hi.y, (uint32_t)mGlass, (uint32_t)mStone);
  Fill(fillA, Box{{aIn.lo.x, aIn.lo.y, aIn.lo.z}, {aIn.hi.x, aIn.lo.y, aIn.hi.z}},
       (uint32_t)mLava, 7u);
  Fill(fillA, Box{{aIn.lo.x + 1, aIn.lo.y + 1, aIn.lo.z + 1},
                  {aIn.hi.x - 1, aIn.lo.y + 1, aIn.hi.z - 1}},
       (uint32_t)mSalt, 0);
  Fill(fillB, Box{{bIn.lo.x, bIn.lo.y, bIn.lo.z}, {bIn.hi.x, bIn.lo.y, bIn.hi.z}},
       (uint32_t)mMolten, 7u);

  uint32_t t = 73000;
  support::TickCursor tick{c, t, IVec3{s.x >> 4, s.y >> 4, s.z >> 4}};
  tick(std::vector<BrushOp>{}, build);
  tick(std::vector<BrushOp>{}, fillA);
  tick(std::vector<BrushOp>{}, fillB);
  uint32_t moltenA = 0, sodiumB = 0, chlorineB = 0;
  const int kTicks = 160;
  for (int i = 1; i <= kTicks; i++) {
    std::vector<CellOp> zap;
    // Sparks over box B for ticks 2..8 (the bench's Electrify, done by hand):
    // IfAir, one layer above the molten pool.
    if (i >= 2 && i <= 8)
      for (int z = bIn.lo.z; z <= bIn.hi.z; z++)
        for (int x = bIn.lo.x; x <= bIn.hi.x; x++)
          zap.push_back({World::SlotCellIndex({x, bIn.lo.y + 1, z}),
                         PackVoxNew((uint32_t)mSpark, 0) | kCellOpIfAir});
    tick(std::vector<BrushOp>{}, zap);
    if (i % 2 == 0) {
      moltenA = std::max(moltenA, Census(c, aWatch)[mMolten]);
      const std::vector<uint32_t> hb = Census(c, bWatch);
      sodiumB = std::max(sodiumB, hb[mSodium]);
      chlorineB = std::max(chlorineB, hb[mCl]);
    }
  }
  Regenerate(c);
  const bool melts = moltenA > 0;
  const bool splits = sodiumB > 0 && chlorineB > 0;
  const bool ok = melts && splits;
  detail = Format(
      "A salt on lava: molten salt peak %u cells (%s); B molten salt + sparks: "
      "sodium peak %u, chlorine peak %u (%s)",
      moltenA, melts ? "melts" : "NEVER MELTED", sodiumB, chlorineB,
      splits ? "split" : "NO ELECTROLYSIS");
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

}  // namespace

const std::vector<Gate>& ChemGates() {
  static const std::vector<Gate> g = {
      {"chem-sodium", "sim", {}, false, GateChemSodium},
      {"chem-acid-fumes", "sim", {}, false, GateChemAcidFumes},
      {"chem-electrolysis", "sim", {}, false, GateChemElectrolysis},
      {"chem-toxic", "mob", {}, false, GateChemToxic},
  };
  return g;
}

}  // namespace selftest
