#include "game/container.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "game/mob.h"  // SplatterEvent
#include "game/worlditems.h"
#include "phys/debris.h"
#include "phys/physics.h"
#include "sim/rng.h"
#include "sim/tuning.h"

bool ContainerSnapWord(const World& world, IVec3 c, uint32_t& word) {
  const WorldSnapshot& s = world.Snap();
  if (!s.valid || s.mirror.empty()) return false;
  const int cx = (c.x >> 4) - s.mirrorBase.x;
  const int cy = (c.y >> 4) - s.mirrorBase.y;
  const int cz = (c.z >> 4) - s.mirrorBase.z;
  if (cx < 0 || cy < 0 || cz < 0 || cx >= 3 || cy >= 3 || cz >= 3) return false;
  const int lx = c.x & 15, ly = c.y & 15, lz = c.z & 15;
  const size_t idx = (size_t)((cz * 3 + cy) * 3 + cx) * kChunkVol +
                     (size_t)((lz * (int)kChunk + ly) * (int)kChunk + lx);
  if (idx >= s.mirror.size()) return false;
  word = s.mirror[idx];
  return true;
}

int ContainerCellUnits(uint32_t word, const MaterialDef& m) {
  if (m.gpu.klass == CLASS_LIQUID)
    return (int)((word >> 12) & 7u) + 1;   // fullness code 0..7 = 1..8 eighths
  return kContainerUnitsPerCell;
}

bool ContainerAccepts(const ItemDef& def, const ItemStack& st, uint32_t mat,
                      const std::vector<MaterialDef>& mats, const char** why) {
  const char* dummy = nullptr;
  const char*& w = why ? *why : dummy;
  if (!def.IsContainer()) {
    w = "that is not a vessel";
    return false;
  }
  if (mat == 0 || mat >= mats.size()) {
    w = "nothing there to take up";
    return false;
  }
  const uint32_t klass = mats[mat].gpu.klass;
  if (klass > 31 || ((def.container.holds >> klass) & 1u) == 0) {
    w = klass == CLASS_LIQUID   ? "this will not hold a liquid"
        : klass == CLASS_POWDER ? "this will not hold loose grains"
                                : "you cannot scoop that";
    return false;
  }
  if (st.Filled() && st.fillMat != mat) {
    w = "it already holds something else";
    return false;
  }
  if (st.fillAmt >= def.container.capacity) {
    w = "it is full";
    return false;
  }
  return true;
}

int ContainerScoop(const ItemDef& def, ItemStack& st, IVec3 hit,
                   const ContainerWordFn& wordAt, const World& window,
                   const std::vector<MaterialDef>& mats,
                   std::vector<CellOp>& ops, const char** why,
                   ContainerScoopMemo* memo, uint32_t tick) {
  if (memo) memo->Prune(tick);
  const char* dummy = nullptr;
  const char*& w = why ? *why : dummy;
  uint32_t hitWord = 0;
  if (!wordAt(hit, hitWord)) {
    w = "too far away";
    return 0;
  }
  // The struck cell decides the material when the vessel is empty; a vessel
  // that already holds something -- or has a scoop of it still in flight --
  // only ever takes more of the same.
  const int pending = memo ? memo->Pending() : 0;
  const uint32_t mat = st.Filled()   ? st.fillMat
                       : pending > 0 ? memo->PendingMat()
                                     : (hitWord & 0xFFFu);
  if (!ContainerAccepts(def, st, hitWord & 0xFFFu, mats, &w)) return 0;
  if ((hitWord & 0xFFFu) != mat) {
    w = "it already holds something else";
    return 0;
  }

  // Candidates: every cell of `mat` within a small ball of the struck one.
  // Radius 2 is a cupped hand's worth: enough that a held button empties a
  // puddle rather than boring a one-cell shaft, small enough that the scoop
  // is where you are looking.
  struct Cand {
    IVec3 c;
    int d2;
    uint32_t word;
  };
  std::vector<Cand> cands;
  constexpr int kR = 2;
  for (int dz = -kR; dz <= kR; dz++)
    for (int dy = -kR; dy <= kR; dy++)
      for (int dx = -kR; dx <= kR; dx++) {
        const int d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > kR * kR) continue;
        const IVec3 c{hit.x + dx, hit.y + dy, hit.z + dz};
        uint32_t word = 0;
        if (!wordAt(c, word) || (word & 0xFFFu) != mat) continue;
        if (!window.CellInWindow(c)) continue;
        if (memo && memo->Has(c)) continue;   // taken; the snapshot is behind
        cands.push_back({c, d2, word});
      }
  // Nearest first; at equal distance the HIGHER cell first (skim the surface
  // of a pond, take the top of a heap), then a fixed coordinate order so the
  // choice is a pure function of the grid.
  std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
    if (a.d2 != b.d2) return a.d2 < b.d2;
    if (a.c.y != b.c.y) return a.c.y > b.c.y;
    if (a.c.z != b.c.z) return a.c.z < b.c.z;
    return a.c.x < b.c.x;
  });
  int taken = 0, asked = 0;
  const int cap = def.container.capacity;
  for (const Cand& cd : cands) {
    if (taken >= def.container.scoopPerTick) break;
    const int units = ContainerCellUnits(cd.word, mats[mat]);
    // A cell is taken WHOLE or not at all: a partial take would need an
    // unconditional write of the remainder, which is the overwrite the
    // conditional clear exists to avoid. What is still in flight counts
    // against the room, or four ticks of claims could overfill it.
    if (st.fillAmt + pending + asked + units > cap) continue;
    ops.push_back({World::SlotCellIndex(cd.c), CellOpClearIfMat(mat)});
    asked += units;
    if (memo) memo->cells.push_back({cd.c, tick});
    taken++;
  }
  if (taken > 0) {
    if (memo) {
      // A CLAIM, paid by ContainerSettle once the GPU has said what it took.
      memo->claims.push_back({tick, (uint16_t)mat, asked});
    } else {
      // No ledger to settle against (the pure gate): pay the ask.
      st.fillMat = (uint16_t)mat;
      st.fillAmt = (uint16_t)(st.fillAmt + asked);
    }
  }
  if (taken == 0)
    w = st.fillAmt + pending >= cap ? "it is full"
                                    : "there is not enough left to fill it";
  return taken;
}

int ContainerSettle(ContainerScoopMemo& memo, uint32_t snapTick, uint32_t ledger,
                    const ItemDef* def, ItemStack* st) {
  // What THIS snapshot's tick removed, if the previous snapshot was the tick
  // before and the ledger did not go backwards (a reset zeroes it).
  const bool known = memo.haveLedger && snapTick == memo.ledgerTick + 1 &&
                     ledger >= memo.ledgerEighths;
  int removed = known ? (int)(ledger - memo.ledgerEighths) : 0;
  if (!memo.haveLedger || snapTick != memo.ledgerTick) {
    memo.haveLedger = true;
    memo.ledgerTick = snapTick;
    memo.ledgerEighths = ledger;
  }
  int paid = 0;
  size_t w = 0;
  // How many claims land on THIS tick: the last of them takes whatever the
  // ledger has left, so the total paid is exactly what the tick removed. A
  // cell that filled up between the snapshot and the clear gave the GPU MORE
  // than was asked, and that is still water that left the world.
  int landing = 0;
  for (const ContainerScoopMemo::Claim& c : memo.claims)
    if (c.tick == snapTick) landing++;
  for (const ContainerScoopMemo::Claim& c : memo.claims) {
    if (c.tick > snapTick) {
      memo.claims[w++] = c;   // not landed yet
      continue;
    }
    int pay = c.units;
    if (c.tick == snapTick && known) {
      // ONE ledger for every vessel in the world: two scoops landing on the
      // same tick share it. Each but the last is paid at most its claim.
      pay = --landing > 0 ? std::min(c.units, removed) : removed;
      removed -= pay;
    }
    if (pay > 0 && st && def && def->IsContainer() &&
        (!st->Filled() || st->fillMat == c.mat)) {
      const int room = def->container.capacity - st->fillAmt;
      pay = std::min(pay, std::max(0, room));
      st->fillMat = c.mat;
      st->fillAmt = (uint16_t)(st->fillAmt + pay);
      paid += pay;
    }
  }
  memo.claims.resize(w);
  return paid;
}

bool ContainerInReach(const ItemDef& def, Vec3 mouth, Vec3 target) {
  return (target - mouth).len() <= std::max(1.0f, def.container.pourRange);
}

Vec3 ContainerPourPoint(const ItemDef& def, Vec3 from, Vec3 head, Vec3 fwd,
                        const std::function<CellKind(IVec3)>& kindAt,
                        const Physics& phys,
                        const std::vector<uint64_t>& ignore) {
  const float fl = fwd.len();
  const Vec3 dir = fl > 1e-4f ? fwd * (1.0f / fl) : Vec3{0, -1, 0};
  float dist = std::max(0.0f, (head - from).dot(dir)) +
               std::max(0.1f, def.container.aimDist);
  // The grid, by a voxel DDA from the render eye (Amanatides-Woo): stop in
  // front of the first solid or liquid cell, so a pour aimed at the ground
  // lands on it rather than inside it. Unknown (past the mirror) is passable,
  // as it is for projectiles; at this range it never comes up.
  if (kindAt) {
    IVec3 c{ifloor(from.x), ifloor(from.y), ifloor(from.z)};
    const int step[3] = {dir.x >= 0 ? 1 : -1, dir.y >= 0 ? 1 : -1,
                         dir.z >= 0 ? 1 : -1};
    const float d[3] = {dir.x, dir.y, dir.z};
    const float o[3] = {from.x, from.y, from.z};
    int* ci[3] = {&c.x, &c.y, &c.z};
    float tMax[3], tDelta[3];
    for (int a = 0; a < 3; a++) {
      if (std::fabs(d[a]) < 1e-6f) {
        tMax[a] = tDelta[a] = 1e30f;
        continue;
      }
      const float edge = (float)*ci[a] + (step[a] > 0 ? 1.0f : 0.0f);
      tMax[a] = (edge - o[a]) / d[a];
      tDelta[a] = std::fabs(1.0f / d[a]);
    }
    for (int guard = 0; guard < 512; guard++) {
      const int a = tMax[0] < tMax[1] ? (tMax[0] < tMax[2] ? 0 : 2)
                                      : (tMax[1] < tMax[2] ? 1 : 2);
      const float t = tMax[a];
      if (t >= dist) break;
      *ci[a] += step[a];
      tMax[a] += tDelta[a];
      const CellKind k = kindAt(c);
      if (k == CellKind::Solid || k == CellKind::Liquid) {
        // Just short of the face the ray entered through.
        dist = std::max(0.0f, t - 0.05f);
        break;
      }
    }
  }
  // A body: the grid cannot see a creature, and pouring on one is half of
  // what a vessel is for.
  float frac = 1.0f;
  if (dist > 0.0f && phys.CastRayBody(from, dir, dist, frac, ignore) != 0)
    dist *= frac;
  return from + dir * dist;
}

int ContainerPour(const ItemDef& def, ItemStack& st, Vec3 mouth, Vec3 fwd,
                  const Vec3* target, int partGravity, uint32_t tick,
                  uint32_t seed, std::vector<ParticleSpawn>& spawns,
                  SplatterEvent* splat) {
  if (!def.IsContainer() || !st.Filled()) return 0;
  const float speed = std::max(1.0f, def.container.pourSpeed);   // vox/s
  const float gTick = (float)std::max(0, partGravity) / 256.0f;
  Vec3 v0;
  int n = 0;        // ticks of flight to the target (aimed) or a guess (tipped)
  float dist = 0.0f;
  if (target && ContainerInReach(def, mouth, *target)) {
    // AIMED. Flight time from the launch speed, in whole ticks, because the
    // kernel moves a particle once per tick and the arc is solved in its own
    // units: after n ticks of `v -= g; p += v` a particle has travelled
    // n*v0 - g*n(n+1)/2, so v0 = delta/n + g(n+1)/2 lands it on the target.
    const Vec3 delta = *target - mouth;
    dist = delta.len();
    n = std::clamp((int)std::lround(dist / speed * 30.0f), 3, 30);
    v0 = delta * (1.0f / (float)n);
    v0.y += gTick * (float)(n + 1) * 0.5f;
  } else {
    // TIPPED: out along the look at the gentle speed; gravity does the rest.
    const float fl = fwd.len();
    const Vec3 dir = fl > 1e-4f ? fwd * (1.0f / fl) : Vec3{0, -1, 0};
    v0 = dir * (speed / 30.0f);
    dist = def.container.pourRange;
    n = 30;
  }

  int poured = 0;
  const uint32_t mat = st.fillMat;
  for (int k = 0; k < def.container.pourPerTick && st.Filled(); k++) {
    if (spawns.size() >= kMaxParticleSpawnsPerTick) break;
    // A little spread so the stream is a stream and not one voxel column:
    // +-4% of the launch velocity and +-0.3 cell at the lip, both hashed from
    // (seed, tick, k) so a replayed pour lands the same cells.
    const uint32_t h = rng::Hash3(seed, tick, (uint32_t)k * 0x9E3779B9u + 1u);
    auto jit = [&](uint32_t salt) {
      return ((float)(rng::Pcg(h ^ salt) & 0xFFFFu) / 65535.0f) * 2.0f - 1.0f;
    };
    const float vl = v0.len();
    const Vec3 v{v0.x + jit(0x11u) * vl * 0.04f, v0.y + jit(0x22u) * vl * 0.04f,
                 v0.z + jit(0x33u) * vl * 0.04f};
    const Vec3 p{mouth.x + jit(0x44u) * 0.3f, mouth.y + jit(0x55u) * 0.3f,
                 mouth.z + jit(0x66u) * 0.3f};
    ParticleSpawn s{};
    s.px = (int32_t)std::lround(p.x * 256.0f);
    s.py = (int32_t)std::lround(p.y * 256.0f);
    s.pz = (int32_t)std::lround(p.z * 256.0f);
    s.vx = (int32_t)std::lround(v.x * 256.0f);
    s.vy = (int32_t)std::lround(v.y * 256.0f);
    s.vz = (int32_t)std::lround(v.z * 256.0f);
    // Full state: the kernel reinserts a liquid particle as a full cell
    // whatever it carries (sim_particle.wgsl), so the last few eighths of a
    // flask come out as one whole cell. That is the one rounding in the
    // system and it is at most 7/8 of a cell per emptying.
    s.payload = (mat & 0xFFFu) | (7u << 12);
    // CALM: a stream, not spray -- the wind does not carry it off.
    s.flags = kPFlagAlive | kPFlagCalm;
    spawns.push_back(s);
    const int spend = std::min<int>(kContainerUnitsPerCell, st.fillAmt);
    st.fillAmt = (uint16_t)(st.fillAmt - spend);
    if (st.fillAmt == 0) st.fillMat = 0;
    poured++;
  }
  if (poured > 0 && splat) {
    const Tuning& tune = CurrentTuning();
    SplatterEvent& e = *splat;
    e = SplatterEvent{};
    e.origin = mouth;
    e.axis = v0;
    e.cone = 0.04f;
    e.speed = v0.len() * 30.0f;
    e.life = std::clamp(n + 4, 1, 255);
    e.reach = std::min(tune.gore.splatterReach, std::max(dist, 8.0f) + 4.0f);
    e.count = poured;
    e.mat = mat;
    e.amount = (uint32_t)std::max(1, tune.gore.splatterAmount);
    e.sourceLimb = -1;
    e.tick = tick;
    e.seed = rng::Hash3(seed, tick, 0x5F1A5Cu);
  }
  return poured;
}

bool ContainerPoursAsFluid(const MaterialDef& m) {
  return m.gpu.klass == CLASS_LIQUID && m.gpu.moveEvery <= 1u;
}

namespace {
// Q16.16 fixed point, the FluidSpawnOp unit for both position (world cells)
// and velocity (cells/tick).
int32_t Q16(float v) { return (int32_t)std::lround(v * 65536.0f); }
}  // namespace

int ContainerPourFluid(const ItemDef& def, ItemStack& st, Vec3 mouth, Vec3 fwd,
                       const Vec3* target, uint32_t tick, uint32_t seed,
                       uint32_t room, std::vector<FluidSpawnOp>& out,
                       SplatterEvent* splat) {
  if (!def.IsContainer() || !st.Filled()) return 0;
  const Tuning& tune = CurrentTuning();
  const float speed = std::max(1.0f, def.container.pourSpeed);   // vox/s
  // The solver's own integration: per substep v.y -= g/S, then p += v/S. After
  // n ticks (m = nS substeps) that is n*v0 - g*n^2/2 - g*n/(2S), so
  // v0.y = dy/n + g*(n + 1/S)/2 lands on the target.
  const float S = (float)std::max(1, tune.sim.fluidSubsteps);
  const float g = std::max(0.0f, tune.sim.fluidGravity) / 900.0f;  // cells/tick^2
  const float vmax = 0.45f * S * 0.9f;  // FLUID_VMAX, with headroom for jitter
  Vec3 v0;
  float dist = 0.0f;
  int n = 0;
  if (target && ContainerInReach(def, mouth, *target)) {
    // AIMED. The flight time the pour speed asks for, else the nearest one
    // whose launch fits under the CFL cap (spawnAppend clamps anything over
    // it, which would bend the arc short).
    const Vec3 delta = *target - mouth;
    dist = delta.len();
    const int want = std::clamp((int)std::lround(dist / speed * 30.0f), 1, 30);
    auto launch = [&](int k) {
      Vec3 v = delta * (1.0f / (float)k);
      v.y += g * ((float)k + 1.0f / S) * 0.5f;
      return v;
    };
    auto fits = [&](const Vec3& v) {
      return std::fabs(v.x) <= vmax && std::fabs(v.y) <= vmax &&
             std::fabs(v.z) <= vmax;
    };
    n = want;
    for (int d = 0; d < 30; d++) {
      if (want - d >= 1 && fits(launch(want - d))) { n = want - d; break; }
      if (want + d <= 30 && fits(launch(want + d))) { n = want + d; break; }
    }
    v0 = launch(n);
  } else {
    // TIPPED: out along the look at the gentle speed; gravity does the rest.
    const float fl = fwd.len();
    const Vec3 dir = fl > 1e-4f ? fwd * (1.0f / fl) : Vec3{0, -1, 0};
    v0 = dir * (speed / 30.0f);
    dist = def.container.pourRange;
    n = 30;
  }

  const uint32_t mat = st.fillMat;
  const uint32_t species = (mat - 1u) & 3u;   // exciteEmit's rule
  const int want = std::min<int>(
      {def.container.pourPerTick * kContainerUnitsPerCell, (int)st.fillAmt,
       (int)std::min<uint32_t>(room, (uint32_t)kMaxFluidSpawnsPerTick)});
  const float vl = v0.len();
  int poured = 0;
  for (int k = 0; k < want; k++) {
    // A blob about a cell across at the lip, eight particles to the cell on
    // the solver's half-cell lattice, with +-4% on the launch so the stream
    // spreads a little instead of flying as one rigid clump. All of it hashed
    // from (seed, tick, k): a replayed pour is the same pour.
    const uint32_t h = rng::Hash3(seed, tick, (uint32_t)k * 0x9E3779B9u + 1u);
    auto jit = [&](uint32_t salt) {
      return ((float)(rng::Pcg(h ^ salt) & 0xFFFFu) / 65535.0f) * 2.0f - 1.0f;
    };
    const int s = k & 7;
    const Vec3 p{mouth.x + ((s & 1) ? 0.25f : -0.25f) + jit(0x44u) * 0.1f,
                 mouth.y + ((s & 2) ? 0.25f : -0.25f) + jit(0x55u) * 0.1f,
                 mouth.z + ((s & 4) ? 0.25f : -0.25f) + jit(0x66u) * 0.1f};
    const Vec3 v{v0.x + jit(0x11u) * vl * 0.04f, v0.y + jit(0x22u) * vl * 0.04f,
                 v0.z + jit(0x33u) * vl * 0.04f};
    FluidSpawnOp op{};
    op.px = Q16(p.x); op.py = Q16(p.y); op.pz = Q16(p.z);
    op.vx = Q16(v.x); op.vy = Q16(v.y); op.vz = Q16(v.z);
    op.species = species;
    op.mat = mat;
    out.push_back(op);
    poured++;
  }
  st.fillAmt = (uint16_t)(st.fillAmt - poured);
  if (st.fillAmt == 0) st.fillMat = 0;
  if (poured > 0 && splat) {
    SplatterEvent& e = *splat;
    e = SplatterEvent{};
    e.origin = mouth;
    e.axis = v0;
    e.cone = 0.04f;
    e.speed = v0.len() * 30.0f;
    e.life = std::clamp(n + 4, 1, 255);
    e.reach = std::min(tune.gore.splatterReach, std::max(dist, 8.0f) + 4.0f);
    e.count = (poured + kContainerUnitsPerCell - 1) / kContainerUnitsPerCell;
    e.mat = mat;
    e.amount = (uint32_t)std::max(1, tune.gore.splatterAmount);
    e.sourceLimb = -1;
    e.tick = tick;
    e.seed = rng::Hash3(seed, tick, 0x5F1A5Cu);
  }
  return poured;
}

uint32_t ContainerScoopStream(IVec3 cell, uint32_t mat, Vec3 mouth, int life,
                              uint32_t seed, uint32_t tick, uint32_t room,
                              std::vector<FluidSpawnOp>& out) {
  life = std::clamp(life, 2, 255);
  const uint32_t species = ((mat - 1u) & 3u) | kFluidOpGhost |
                           ((uint32_t)life << kFluidOpLifeShift);
  uint32_t n = 0;
  for (int s = 0; s < 8 && n < room; s++) {
    const uint32_t h =
        rng::Hash3(seed, tick, (uint32_t)(s + 1) * 0x9E3779B9u);
    const Vec3 p{cell.x + ((s & 1) ? 0.75f : 0.25f),
                 cell.y + ((s & 2) ? 0.75f : 0.25f),
                 cell.z + ((s & 4) ? 0.75f : 0.25f)};
    // The velocity CARRIES THE TARGET: spawnAppend reads p + v * life as the
    // homing point before it clamps v to the CFL cap, so a far mouth is still
    // reached, just not at launch speed. A little jitter on the mouth end
    // keeps eight particles from converging on one point.
    const Vec3 aim = mouth + Vec3{(float)((h & 0xFFu) / 255.0f - 0.5f) * 0.4f,
                                  (float)(((h >> 8) & 0xFFu) / 255.0f - 0.5f) * 0.4f,
                                  (float)(((h >> 16) & 0xFFu) / 255.0f - 0.5f) * 0.4f};
    const Vec3 v = (aim - p) * (1.0f / (float)life);
    FluidSpawnOp op{};
    op.px = Q16(p.x); op.py = Q16(p.y); op.pz = Q16(p.z);
    op.vx = Q16(v.x); op.vy = Q16(v.y); op.vz = Q16(v.z);
    op.species = species;
    op.mat = mat;
    out.push_back(op);
    n++;
  }
  return n;
}

float PourBrushCellsPerSec(const ItemDef& def, float radius) {
  const float k = std::max(0.0f, radius) / kPourBrushRefRadius;
  return (float)def.container.applyCells * k * k;
}

bool ContainerThrowable(const ItemDef& def) {
  return def.IsContainer() && def.container.throwSpeed > 0.0f;
}

float ContainerThrowCharge(const ItemDef& def, int heldTicks) {
  const float full = std::max(1.0f, def.container.throwChargeSec * 30.0f);
  return std::clamp((float)std::max(0, heldTicks - 1) / full, 0.0f, 1.0f);
}

float ContainerThrowSpeed(const ItemDef& def, int heldTicks) {
  const float k = ContainerThrowCharge(def, heldTicks);
  // Eased in: the first half of the wind-up buys less than the second, so a
  // quick flick lobs it a few metres and only a full draw hurls it.
  const float e = k * k * (3.0f - 2.0f * k);
  return def.container.throwMinSpeed +
         (def.container.throwSpeed - def.container.throwMinSpeed) * e;
}

bool ContainerShouldBreak(const ItemDef& def, float contactSpeed, Vec3 dv) {
  const float b = def.container.breakSpeed;
  if (!def.IsContainer() || b <= 0.0f) return false;
  return contactSpeed >= b || dv.len() >= b;
}

int ContainerSpillStep(ContainerSpill& sp, const std::vector<MaterialDef>& mats,
                       uint32_t tick, uint32_t fluidRoom,
                       std::vector<FluidSpawnOp>& fluid,
                       std::vector<ParticleSpawn>& parts, SplatterEvent* splat) {
  if (sp.units <= 0 || sp.mat == 0 || sp.mat >= mats.size()) {
    sp.units = 0;
    return 0;
  }
  const Tuning& tune = CurrentTuning();
  const bool asFluid = ContainerPoursAsFluid(mats[sp.mat]);
  // THE BURST'S SIZE: a ball about the volume of what was in it, so 128 cells
  // of water do not appear packed into the one cell the glass occupied (the
  // solver's pressure would fire them off like a charge). Nudged off the
  // surface it broke on so none of it is born inside the wall.
  const float cells = (float)sp.units / (float)kContainerUnitsPerCell;
  const float r = std::clamp(std::cbrt(cells * 3.0f / (4.0f * 3.14159265f)),
                             0.5f, 3.5f);
  const Vec3 c = sp.at + sp.away * (r * 0.6f);
  // Outward at a couple of metres a second, plus a third of what the vessel
  // was carrying: a flask smashed against a wall splashes along it rather than
  // stopping dead, and one dropped on its base splashes round its feet.
  const float burst = MetresToCells(2.5f) / 30.0f;  // cells/tick
  const Vec3 carry = sp.vel * (0.3f / 30.0f);
  const float S = (float)std::max(1, tune.sim.fluidSubsteps);
  const float vmax = 0.45f * S * 0.9f;  // FLUID_VMAX, as ContainerPourFluid
  auto sample = [&](uint32_t k, Vec3& p, Vec3& v) {
    const uint32_t h = rng::Hash3(sp.seed, tick, k * 0x9E3779B9u + 7u);
    auto u = [&](uint32_t salt) {
      return ((float)(rng::Pcg(h ^ salt) & 0xFFFFu) / 65535.0f) * 2.0f - 1.0f;
    };
    // A point in the unit ball by rejection-free cube-to-ball squash: the
    // direction and a cube-root radius, so the ball is evenly filled.
    Vec3 d{u(0x11u), u(0x22u), u(0x33u)};
    const float dl = d.len();
    d = dl > 1e-4f ? d * (1.0f / dl) : Vec3{0, 1, 0};
    const float rr = std::cbrt(0.5f * (u(0x44u) + 1.0f));
    p = c + d * (r * rr);
    v = carry + d * (burst * (0.6f + 0.4f * (u(0x55u) * 0.5f + 0.5f)));
    v.y += burst * 0.35f;
  };

  int emitted = 0;
  if (asFluid) {
    const uint32_t species = (sp.mat - 1u) & 3u;
    const int n = (int)std::min<uint32_t>((uint32_t)sp.units, fluidRoom);
    for (int k = 0; k < n; k++) {
      Vec3 p, v;
      sample((uint32_t)(sp.units - k), p, v);
      v.x = std::clamp(v.x, -vmax, vmax);
      v.y = std::clamp(v.y, -vmax, vmax);
      v.z = std::clamp(v.z, -vmax, vmax);
      FluidSpawnOp op{};
      op.px = Q16(p.x); op.py = Q16(p.y); op.pz = Q16(p.z);
      op.vx = Q16(v.x); op.vy = Q16(v.y); op.vz = Q16(v.z);
      op.species = species;
      op.mat = sp.mat;
      fluid.push_back(op);
      emitted++;
    }
    sp.units -= emitted;
  } else {
    // Whole cells: the kernel reinserts a particle as a full cell (see
    // ContainerPour), so a partial last cell comes out whole.
    while (sp.units > 0 && parts.size() < kMaxParticleSpawnsPerTick) {
      Vec3 p, v;
      sample((uint32_t)sp.units, p, v);
      ParticleSpawn ps{};
      ps.px = (int32_t)std::lround(p.x * 256.0f);
      ps.py = (int32_t)std::lround(p.y * 256.0f);
      ps.pz = (int32_t)std::lround(p.z * 256.0f);
      ps.vx = (int32_t)std::lround(v.x * 256.0f);
      ps.vy = (int32_t)std::lround(v.y * 256.0f);
      ps.vz = (int32_t)std::lround(v.z * 256.0f);
      ps.payload = ((uint32_t)sp.mat & 0xFFFu) | (7u << 12);
      ps.flags = kPFlagAlive;
      parts.push_back(ps);
      const int spend = std::min(kContainerUnitsPerCell, sp.units);
      sp.units -= spend;
      emitted += spend;
    }
  }
  if (emitted > 0 && splat && !sp.splatted) {
    sp.splatted = true;
    SplatterEvent& e = *splat;
    e = SplatterEvent{};
    e.origin = c;
    e.axis = Vec3{sp.away.x, sp.away.y + 1.0f, sp.away.z};
    e.cone = 1.0f;
    e.speed = burst * 30.0f;
    e.life = 12;
    e.reach = std::min(tune.gore.splatterReach, r + 8.0f);
    e.count = std::max(1, (emitted + kContainerUnitsPerCell - 1) /
                              kContainerUnitsPerCell);
    e.mat = sp.mat;
    e.amount = (uint32_t)std::max(1, tune.gore.splatterAmount);
    e.sourceLimb = -1;
    e.tick = tick;
    e.seed = rng::Hash3(sp.seed, tick, 0xB2EA4Bu);
  }
  return emitted;
}

int ContainerBreakPass(WorldItems& ground, const ItemLibrary& items,
                       Physics& phys, DebrisSystem& debris,
                       std::vector<std::pair<uint64_t, Vec3>>& lastVel,
                       std::vector<ContainerSpill>& spills) {
  std::vector<std::pair<uint64_t, Vec3>> nextVel;
  struct Broke {
    uint64_t body;
    Vec3 prevVel, away;
  };
  std::vector<Broke> broke;
  const std::vector<Physics::ContactImpact>& contacts = phys.ContactImpacts();
  for (const WorldItem& wi : ground.All()) {
    const ItemDef* def = items.At(items.Find(wi.item));
    if (!def || !def->IsContainer() || def->container.breakSpeed <= 0.0f)
      continue;
    if (debris.IsGhost(wi.body)) continue;
    Vec3 lin{}, ang{};
    if (!phys.GetBodyVelocities(wi.body, lin, ang)) continue;
    // A body seen for the first time (just thrown, just loaded) has no
    // "before": its launch is not a jump.
    Vec3 prev = lin;
    bool seen = false;
    for (const auto& pv : lastVel)
      if (pv.first == wi.body) {
        prev = pv.second;
        seen = true;
        break;
      }
    float hit = 0.0f;
    Vec3 away{};
    for (const Physics::ContactImpact& ci : contacts) {
      if (ci.bodyA != wi.body && ci.bodyB != wi.body) continue;
      if (ci.speedVoxPerSec <= hit) continue;
      hit = ci.speedVoxPerSec;
      // `normal` points A -> B, so off the other thing is -normal for A.
      away = ci.bodyA == wi.body ? ci.normal * -1.0f : ci.normal;
    }
    const Vec3 dv = seen ? lin - prev : Vec3{};
    if (ContainerShouldBreak(*def, hit, dv)) {
      if (hit <= 0.0f) {
        // No contact to say which way is off the surface: the jump's own
        // direction is the push it took.
        const float l = dv.len();
        away = l > 1e-4f ? dv * (1.0f / l) : Vec3{};
      }
      broke.push_back({wi.body, prev, away});
    } else {
      nextVel.push_back({wi.body, lin});
    }
  }
  lastVel.swap(nextVel);
  int n = 0;
  for (const Broke& b : broke) {
    const WorldItem* wi = ground.Find(b.body);
    if (!wi) continue;
    ContainerSpill sp;
    sp.mat = ItemFillMat(wi->fill);
    sp.units = ItemFillAmt(wi->fill);
    sp.vel = b.prevVel;
    sp.away = b.away;
    sp.seed = (uint32_t)(b.body ^ (b.body >> 32)) ^ 0xF1A5Bu;
    if (!phys.BodyCenterOfMass(b.body, sp.at)) {
      BodyTransform bx{};
      phys.GetTransform(b.body, bx);
      sp.at = bx.pos;
    }
    // AFTER every read of the entry: OnBodyGone erases it.
    debris.DestroyBody(b.body);
    n++;
    if (sp.units > 0 && sp.mat != 0) spills.push_back(sp);
  }
  return n;
}

int ContainerSpend(ItemStack& st, int cells) {
  if (!st.Filled() || cells <= 0) return 0;
  const int want = cells * kContainerUnitsPerCell;
  const int spend = std::min<int>(want, st.fillAmt);
  st.fillAmt = (uint16_t)(st.fillAmt - spend);
  if (st.fillAmt == 0) st.fillMat = 0;
  return spend;
}

bool ContainerIsolateOne(ItemStack* slots, int nSlots, int slot,
                         ItemStack* spill, int nSpill) {
  if (slot < 0 || slot >= nSlots) return false;
  ItemStack& s = slots[slot];
  if (s.Empty() || s.count <= 1) return true;
  ItemStack rest = s;
  rest.count = s.count - 1;
  auto park = [&](ItemStack* v, int n, int skip) {
    for (int i = 0; i < n; i++)
      if (i != skip && v[i].Empty()) {
        v[i] = rest;
        return true;
      }
    return false;
  };
  if (!park(slots, nSlots, slot) && !park(spill, nSpill, -1)) return false;
  s.count = 1;
  return true;
}

std::string ContainerFillText(const ItemDef& def, const ItemStack& st,
                              const std::vector<MaterialDef>& mats) {
  if (!def.IsContainer()) return std::string();
  const int capCells = def.container.capacity / kContainerUnitsPerCell;
  if (!st.Filled()) {
    char b[64];
    std::snprintf(b, sizeof b, "empty (holds %d)", capCells);
    return b;
  }
  const std::string name =
      st.fillMat < mats.size() ? mats[st.fillMat].name : std::string("?");
  // Whole cells, rounded UP: a flask with one eighth left is not "0 water".
  const int cells = (st.fillAmt + kContainerUnitsPerCell - 1) / kContainerUnitsPerCell;
  char b[96];
  std::snprintf(b, sizeof b, "%s %d/%d", name.c_str(), cells, capCells);
  return b;
}
