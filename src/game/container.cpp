#include "game/container.h"

#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "game/benchchem.h"  // ChemGateOpen (pocket chemistry)
#include "game/mob.h"  // SplatterEvent
#include "game/worlditems.h"
#include "phys/debris.h"
#include "phys/physics.h"
#include "sim/reactcpu.h"
#include "sim/rng.h"
#include "sim/solutes.h"
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
  if (m.gpu.klass == CLASS_POWDER && (m.gpu.flags & kMatFlagWander) == 0u)
    return (int)PowderMassOfState((word >> 12) & 0xFu);   // powder mass, eighths
  return kContainerUnitsPerCell;
}

// The state nibble a poured/spilled cell of `eighths` (1..8) carries: a
// liquid's fullness code, or a powder's mass code (world.h POWDER MASS; full
// = variant 0). Powder needs no MEASURED flag: sim_particle.wgsl lands a
// non-liquid's nibble verbatim.
static uint32_t PouredState(bool liquid, int eighths) {
  if (liquid) return (uint32_t)(eighths - 1);
  return eighths >= (int)kPowderFull ? 0u : (uint32_t)eighths + 2u;
}

bool ContainerAccepts(const ItemDef& def, const ItemStack& st, uint32_t mat,
                      const std::vector<MaterialDef>& mats, const char** why) {
  const char* dummy = nullptr;
  const char*& w = why ? *why : dummy;
  if (!def.IsContainer()) {
    w = "that is not a vessel";
    return false;
  }
  if (st.stoppered) {
    w = "it is stoppered";
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
  if (st.contents.AmountOf((uint16_t)mat) == 0 &&
      st.contents.n >= alchemy::kMaxSubstances) {
    w = "it already holds too many things";
    return false;
  }
  if ((int)st.FillTotal() >= def.container.capacity) {
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
  // The struck cell decides the material. A vessel that already holds
  // something takes it as another portion (contents mix, up to
  // kMaxSubstances); each claim in flight carries its own material.
  const int pending = memo ? memo->Pending() : 0;
  const uint32_t mat = hitWord & 0xFFFu;
  if (!ContainerAccepts(def, st, mat, mats, &w)) return 0;

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
    if ((int)st.FillTotal() + pending + asked + units > cap) continue;
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
      st.contents.Add((uint16_t)mat, (uint32_t)asked);
    }
  }
  if (taken == 0)
    w = (int)st.FillTotal() + pending >= cap ? "it is full"
                                    : "there is not enough left to fill it";
  return taken;
}

void ContainerLedgerObserve(ContainerScoopLedger& L, uint32_t snapTick,
                            uint32_t ledger, int landingClaims) {
  // The same snapshot again (no new one arrived this tick): its pot is
  // already being drawn down, and observing it twice would refill it.
  if (L.have && snapTick == L.tick) return;
  // What THIS snapshot's tick removed, if the previous snapshot was the tick
  // before and the ledger did not go backwards (a reset zeroes it).
  L.known = L.have && snapTick == L.tick + 1 && ledger >= L.eighths;
  L.left = L.known ? (int)(ledger - L.eighths) : 0;
  L.landing = std::max(0, landingClaims);
  L.have = true;
  L.tick = snapTick;
  L.eighths = ledger;
}

int ContainerDeposit(const ItemDef& def, ItemStack& st, uint16_t mat, int units) {
  if (units <= 0 || mat == 0 || st.Empty() || !def.IsContainer()) return 0;
  if (st.count != 1) return 0;   // one object's fill, never a stack's
  const int room = std::max(0, def.container.capacity - (int)st.FillTotal());
  const int put = std::min(units, room);
  if (put <= 0) return 0;
  if (!st.contents.Add(mat, (uint32_t)put)) return 0;   // a 17th substance
  return put;
}

void ContainerSoluteObserve(ContainerSoluteLedger& L, uint32_t snapTick,
                            const uint32_t (&scoopedBy)[kSolScoopSpecies]) {
  if (L.have && L.tick == snapTick) return;
  for (uint32_t k = 0; k < kSolScoopSpecies; k++) {
    if (L.have && scoopedBy[k] >= L.seen[k]) L.pot[k] += scoopedBy[k] - L.seen[k];
    L.seen[k] = scoopedBy[k];
  }
  L.have = true;
  L.tick = snapTick;
}

int ContainerSoluteTake(ContainerSoluteLedger& L, uint32_t species, uint32_t eighthUnits) {
  if (species == 0 || species > kSolScoopSpecies || eighthUnits == 0) return 0;
  uint32_t& pot = L.pot[species - 1];
  const uint32_t n = pot / eighthUnits;
  pot -= n * eighthUnits;
  return (int)n;
}

int ContainerSettle(ContainerScoopLedger& L, ContainerScoopMemo& memo,
                    uint32_t snapTick,
                    const std::function<int(uint16_t mat, int units)>& deposit,
                    std::vector<ContainerUnpaid>* unpaid) {
  const bool pot = L.have && L.known && L.tick == snapTick;
  int paid = 0;
  size_t w = 0;
  for (const ContainerScoopMemo::Claim& c : memo.claims) {
    if (c.tick > snapTick) {
      memo.claims[w++] = c;   // not landed yet
      continue;
    }
    int pay = c.units;
    if (c.tick == snapTick && pot) {
      // ONE ledger for every vessel in the world: every claim landing on this
      // tick, in every session, draws on the same pot. Each but the last is
      // paid at most its claim; the LAST takes whatever is left, so the total
      // paid is exactly what the tick removed -- a cell that filled up
      // between the snapshot and the clear gave the GPU MORE than was asked,
      // and that is still matter that left the world.
      pay = --L.landing > 0 ? std::min(c.units, L.left) : L.left;
      pay = std::max(0, pay);
      L.left -= pay;
    }
    if (pay <= 0) continue;
    const int put = deposit ? std::clamp(deposit(c.mat, pay), 0, pay) : 0;
    paid += put;
    if (put < pay && unpaid) unpaid->push_back({c.mat, pay - put});
  }
  memo.claims.resize(w);
  return paid;
}

int ContainerSettle(ContainerScoopMemo& memo, uint32_t snapTick, uint32_t ledger,
                    const ItemDef* def, ItemStack* st,
                    std::vector<ContainerUnpaid>* unpaid) {
  int landing = 0;
  for (const ContainerScoopMemo::Claim& c : memo.claims)
    if (c.tick == snapTick) landing++;
  ContainerLedgerObserve(memo.ledger, snapTick, ledger, landing);
  return ContainerSettle(
      memo.ledger, memo, snapTick,
      [&](uint16_t mat, int units) {
        return def && st ? ContainerDeposit(*def, *st, mat, units) : 0;
      },
      unpaid);
}

uint32_t ContainerParticleRoom(bool snapValid, uint32_t snapParticleCount,
                               size_t queuedThisTick) {
  if (!snapValid) return 0u;   // nothing known: pour nothing rather than guess
  // Every CPU spawn stream since the snapshot, at its cap: the snapshot shows
  // tick T - kSnapshotLatency and the streams of the ticks after it (this one
  // included, whose queue is counted exactly below) are not in it.
  const uint64_t since =
      (uint64_t)kMaxParticleSpawnsPerTick * World::kSnapshotLatency;
  const uint64_t used = (uint64_t)std::min(snapParticleCount, kParticleCap) +
                        since + (uint64_t)queuedThisTick;
  return used >= kParticleCap ? 0u : (uint32_t)(kParticleCap - used);
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

namespace {
// Is what this vessel holds a LIQUID (land at measured fullness) or not (a
// whole grain or nothing)? The material table when the caller has one; else
// the vessel's own `holds`: a vessel that can hold only liquids holds one.
// Unknown counts as NOT liquid, the direction that can only lose a partial
// cell, never mint one.
bool ContainerContentIsLiquid(const ItemDef& def, uint32_t mat,
                              const std::vector<MaterialDef>* mats) {
  if (mats) return mat < mats->size() && (*mats)[mat].gpu.klass == CLASS_LIQUID;
  // No table to ask: a vessel that can hold liquid is taken to (the gates
  // that pour without one pour water).
  return (def.container.holds & (1u << CLASS_LIQUID)) != 0;
}
}  // namespace

int ContainerPour(const ItemDef& def, ItemStack& st, Vec3 mouth, Vec3 fwd,
                  const Vec3* target, int partGravity, uint32_t tick,
                  uint32_t seed, std::vector<ParticleSpawn>& spawns,
                  SplatterEvent* splat, uint32_t partRoom,
                  const std::vector<MaterialDef>* mats,
                  std::vector<ContainerSolutePour>* sol) {
  // A stoppered vessel pours nothing (ContainerTopMat is 0 for it too).
  if (!def.IsContainer() || !st.Filled() || st.count != 1 || st.stoppered) return 0;
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
  // One portion per tick: the top layer (ContainerTopMat).
  const uint32_t mat = ContainerTopMat(st, mats);
  const bool liquid = ContainerContentIsLiquid(def, mat, mats);
  const uint32_t liquidBefore = ContainerLiquidEighths(st.contents, mats);
  uint32_t liquidOut = 0;
  for (int k = 0; k < def.container.pourPerTick && st.contents.AmountOf((uint16_t)mat) > 0; k++) {
    if (spawns.size() >= kMaxParticleSpawnsPerTick) break;
    if ((uint32_t)k >= partRoom) break;   // charged only for what the ring takes
    // A powder leaves in GRAINS of def.container.pourGrain eighths each (a
    // pouch: 1/8-voxel grains that merge into partial cells where they land);
    // a liquid in whole cells plus its measured remainder, as before.
    const int unit = liquid ? kContainerUnitsPerCell : def.container.pourGrain;
    const int spend = std::min<int>(unit, (int)st.contents.AmountOf((uint16_t)mat));
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
    // A cell carries EXACTLY what it was charged: a liquid as its fullness
    // code plus the MEASURED bit that tells the kernel to land it at that
    // fullness rather than full, a powder as its mass code (the last few
    // eighths of a pouch land as a partial cell of grains).
    s.payload = (mat & 0xFFFu) | (PouredState(liquid, spend) << 12);
    // CALM: a stream, not spray -- the wind does not carry it off.
    s.flags = kPFlagAlive | kPFlagCalm | (liquid ? kPFlagMeasured : 0u);
    spawns.push_back(s);
    st.contents.Take((uint16_t)mat, (uint32_t)spend);
    if (liquid) liquidOut += (uint32_t)spend;
    poured++;
  }
  // What was dissolved in the liquid that left goes with it (contract 2.5):
  // in solution, landing where the stream lands, when it lands -- the arc
  // above is solved to reach the target in `n` ticks (a tipped pour's
  // landing is the same arc's point after `n`, a guess the GPU's surface
  // search forgives).
  if (liquidOut && mats) {
    const alchemy::Composition d = ContainerTakeDissolvedShare(st.contents, liquidOut, liquidBefore);
    Vec3 land = mouth + v0 * (float)n;
    land.y -= gTick * (float)n * (float)(n + 1) * 0.5f;
    if (target && ContainerInReach(def, mouth, *target)) land = *target;
    const IVec3 lc{(int)std::floor(land.x), (int)std::floor(land.y), (int)std::floor(land.z)};
    uint32_t charged = (uint32_t)poured;
    for (int i = 0; i < d.n; i++) {
      // The grain fallback (no solute layer / no species) is charged
      // against what is left of the particle room the pour itself respected.
      const uint32_t roomLeft = partRoom == 0xFFFFFFFFu ? partRoom
                                : partRoom - std::min<uint32_t>(partRoom, charged);
      const size_t before = spawns.size();
      const int out = ContainerDissolvedToWorld(d.p[i].mat, (int)d.p[i].eighths, mouth, v0,
                                                seed ^ 0xD155u, tick, *mats, spawns, roomLeft,
                                                sol, lc, tick + (uint32_t)n);
      charged += (uint32_t)(spawns.size() - before);
      if (out < (int)d.p[i].eighths) st.contents.Add(d.p[i].mat, d.p[i].eighths - (uint32_t)out);
    }
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
                       SplatterEvent* splat, const std::vector<MaterialDef>* mats,
                       std::vector<ContainerSolutePour>* sol) {
  // One vessel's fill: a stack does not pour (isolate one first); a stoppered
  // one pours nothing.
  if (!def.IsContainer() || !st.Filled() || st.count != 1 || st.stoppered) return 0;
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

  const uint32_t mat = ContainerTopMat(st, mats);
  const uint32_t liquidBefore = ContainerLiquidEighths(st.contents, mats);
  const int want = std::min<int>(
      {def.container.pourPerTick * kContainerUnitsPerCell,
       (int)st.contents.AmountOf((uint16_t)mat),
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
    op.mat = mat;
    out.push_back(op);
    poured++;
  }
  st.contents.Take((uint16_t)mat, (uint32_t)poured);
  // The dissolved share of what left GOES WITH IT (container.h: as a solute
  // pour aimed where the fluid is aimed, landing when it lands -- not on the
  // particles, and why not). Only the FALLBACK (no `sol`, or the layer off)
  // keeps the old answer: the fluid road has no grid-particle stream to put
  // powder in, so the share goes back into the vessel as UNDISSOLVED powder
  // there and pours as grains after the liquid.
  if (poured > 0) {
    const alchemy::Composition d = ContainerTakeDissolvedShare(st.contents, (uint32_t)poured, liquidBefore);
    Vec3 land = mouth + v0 * (float)n;
    land.y -= g * (float)n * (float)n * 0.5f;
    if (target && ContainerInReach(def, mouth, *target)) land = *target;
    const IVec3 lc{(int)std::floor(land.x), (int)std::floor(land.y), (int)std::floor(land.z)};
    for (int i = 0; i < d.n; i++) {
      const int q = sol ? ContainerDissolvedToSolution(d.p[i].mat, (int)d.p[i].eighths, lc,
                                                       tick + (uint32_t)n, *sol)
                        : 0;
      if (q < (int)d.p[i].eighths)
        st.contents.Add(alchemy::BaseMat(d.p[i].mat), d.p[i].eighths - (uint32_t)q);
    }
  }
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
  const uint32_t flags = kFluidOpGhost | ((uint32_t)life << kFluidOpLifeShift);
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
    op.flags = flags;
    op.mat = mat;
    out.push_back(op);
    n++;
  }
  return n;
}

uint32_t ContainerApplyStream(Vec3 mouth, Vec3 target, uint32_t mat, int life,
                              int n, uint32_t seed, uint32_t tick, uint32_t room,
                              std::vector<FluidSpawnOp>& out) {
  life = std::clamp(life, 2, 255);
  const uint32_t flags = kFluidOpGhost | ((uint32_t)life << kFluidOpLifeShift);
  auto jit = [](uint32_t h, int sh, float span) {
    return ((float)((h >> sh) & 0xFFu) / 255.0f - 0.5f) * span;
  };
  uint32_t k = 0;
  for (int s = 0; s < n && k < room; s++) {
    const uint32_t h = rng::Hash3(seed, tick, (uint32_t)(s + 1) * 0x9E3779B9u);
    const uint32_t g = rng::Hash3(h, tick, 0xA9917u);
    // A tight cluster at the lip, spread a little wider where it lands: a
    // stream leaving a flask's neck and splashing on skin.
    const Vec3 p = mouth + Vec3{jit(h, 0, 0.3f), jit(h, 8, 0.3f), jit(h, 16, 0.3f)};
    const Vec3 aim = target + Vec3{jit(g, 0, 0.6f), jit(g, 8, 0.6f), jit(g, 16, 0.6f)};
    // The velocity carries the target (see ContainerScoopStream).
    const Vec3 v = (aim - p) * (1.0f / (float)life);
    FluidSpawnOp op{};
    op.px = Q16(p.x); op.py = Q16(p.y); op.pz = Q16(p.z);
    op.vx = Q16(v.x); op.vy = Q16(v.y); op.vz = Q16(v.z);
    op.flags = flags;
    op.mat = mat;
    out.push_back(op);
    k++;
  }
  return k;
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
                       std::vector<ParticleSpawn>& parts, SplatterEvent* splat,
                       uint32_t partRoom, std::vector<GasSpawnOp>* gas, uint32_t gasRoom,
                       std::vector<ContainerSolutePour>* sol) {
  // The next portion when this one is out (a mixed vessel spills them in
  // turn). A portion whose material is gone from the table is dropped.
  while (sp.units <= 0 && !sp.rest.Empty()) {
    sp.mat = sp.rest.p[0].mat;
    sp.units = (int)sp.rest.Take(sp.mat, sp.rest.p[0].eighths);
    if (alchemy::BaseMat(sp.mat) == 0 || alchemy::BaseMat(sp.mat) >= mats.size()) sp.units = 0;
  }
  if (sp.units <= 0 || alchemy::BaseMat(sp.mat) == 0 || alchemy::BaseMat(sp.mat) >= mats.size()) {
    sp.units = 0;
    return 0;
  }
  // DISSOLVED: the solute seam. In solution, into whatever the spill's liquid
  // lands in under the vessel, a few ticks on (the burst ball's fall; the
  // GPU's surface search finds the ground or the water under the cell).
  if (alchemy::IsDissolved(sp.mat)) {
    const uint32_t room = partRoom == 0xFFFFFFFFu ? 0xFFFFFFFFu : partRoom;
    const IVec3 lc{(int)std::floor(sp.at.x), (int)std::floor(sp.at.y), (int)std::floor(sp.at.z)};
    const int out = ContainerDissolvedToWorld(sp.mat, sp.units, sp.at, sp.vel * (1.0f / 30.0f),
                                              sp.seed, tick, mats, parts, room, sol, lc,
                                              tick + kSolutePourSpillTicks);
    sp.units -= out;
    return out;
  }
  // GAS: parcels on the CPU gas stream, eight eighths a voxel.
  if (mats[sp.mat].gpu.klass == CLASS_GAS) {
    if (!gas) return 0;   // no gas stream this call: it waits
    int emitted = 0;
    uint32_t made = 0;
    while (sp.units > 0 && made < gasRoom) {
      const int spend = std::min(sp.units, kContainerUnitsPerCell);
      // A partial last voxel: whole with the chance its eighths say (the
      // hash is the tick's and the spill's, so a replay rounds the same).
      const uint32_t h = rng::Hash3(sp.seed, tick, (uint32_t)sp.units * 0x9E3779B9u + 3u);
      const bool whole = spend >= kContainerUnitsPerCell ||
                         (rng::Pcg(h) % (uint32_t)kContainerUnitsPerCell) < (uint32_t)spend;
      if (whole) {
        auto u = [&](uint32_t salt) { return ((float)(rng::Pcg(h ^ salt) & 0xFFFFu) / 65535.0f) * 2.0f - 1.0f; };
        const float r = sp.pour ? 0.6f : 1.5f;
        gas->push_back(MakeGasSpawn((int32_t)std::floor(sp.at.x + u(0x11u) * r),
                                    (int32_t)std::floor(sp.at.y + std::fabs(u(0x22u)) * r),
                                    (int32_t)std::floor(sp.at.z + u(0x33u) * r), sp.mat));
        made++;
      }
      sp.units -= spend;
      emitted += spend;
    }
    return emitted;
  }
  const Tuning& tune = CurrentTuning();
  const bool asFluid = ContainerPoursAsFluid(mats[sp.mat]);
  // THE BURST'S SIZE: a ball about the volume of what was in it, so 128 cells
  // of water do not appear packed into the one cell the glass occupied (the
  // solver's pressure would fire them off like a charge). Nudged off the
  // surface it broke on so none of it is born inside the wall.
  const float cells = (float)sp.units / (float)kContainerUnitsPerCell;
  const float r = sp.pour ? 0.35f
                         : std::clamp(std::cbrt(cells * 3.0f / (4.0f * 3.14159265f)),
                                      0.5f, 3.5f);
  const Vec3 c = sp.at + sp.away * (r * 0.6f);
  // Outward at a couple of metres a second, plus a third of what the vessel
  // was carrying: a flask smashed against a wall splashes along it rather than
  // stopping dead, and one dropped on its base splashes round its feet.
  const float burst = sp.pour ? MetresToCells(0.15f) / 30.0f
                              : MetresToCells(2.5f) / 30.0f;  // cells/tick
  const Vec3 carry = sp.vel * ((sp.pour ? 1.0f : 0.3f) / 30.0f);
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
    if (!sp.pour) v.y += burst * 0.35f;
  };

  int emitted = 0;
  if (asFluid) {
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
      op.mat = sp.mat;
      fluid.push_back(op);
      emitted++;
    }
    sp.units -= emitted;
  } else {
    // A cell per particle, MEASURED like the pour's (ContainerPour): a
    // partial last cell lands at its fullness (liquid) or mass (powder).
    const bool liquid = mats[sp.mat].gpu.klass == CLASS_LIQUID;
    uint32_t made = 0;
    while (sp.units > 0 && parts.size() < kMaxParticleSpawnsPerTick &&
           made < partRoom) {
      // A powder spills as one-eighth GRAINS (world.h POWDER ENTERS THE WORLD
      // AS GRAINS): matter conserved, split, and whatever the budget cannot
      // take this tick waits for the next.
      const int spend = std::min(liquid ? kContainerUnitsPerCell : 1, sp.units);
      Vec3 p, v;
      sample((uint32_t)sp.units, p, v);
      ParticleSpawn ps{};
      ps.px = (int32_t)std::lround(p.x * 256.0f);
      ps.py = (int32_t)std::lround(p.y * 256.0f);
      ps.pz = (int32_t)std::lround(p.z * 256.0f);
      ps.vx = (int32_t)std::lround(v.x * 256.0f);
      ps.vy = (int32_t)std::lround(v.y * 256.0f);
      ps.vz = (int32_t)std::lround(v.z * 256.0f);
      ps.payload = ((uint32_t)sp.mat & 0xFFFu) | (PouredState(liquid, spend) << 12);
      ps.flags = kPFlagAlive | (liquid ? kPFlagMeasured : 0u);
      parts.push_back(ps);
      made++;
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
    const ItemDef* def = items.Of(wi);
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
    sp.rest = wi->contents;
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
    if (!sp.Done()) spills.push_back(sp);
  }
  return n;
}

int ContainerSpend(ItemStack& st, int cells) {
  if (!st.Filled() || cells <= 0 || st.count != 1 || st.stoppered) return 0;
  // What is spent is the MIX: every portion in proportion, so a salve of two
  // parts honey to one of ash stays two to one as the flask empties.
  // Remainders go to the largest portions first, which keeps the total exact.
  const int want = cells * kContainerUnitsPerCell;
  const uint32_t total = st.FillTotal();
  const int spend = std::min<int>(want, (int)total);
  if (spend <= 0) return 0;
  alchemy::Composition before = st.contents;
  uint32_t taken = 0;
  for (int i = 0; i < before.n; i++) {
    const uint32_t part = (uint32_t)((uint64_t)before.p[i].eighths * (uint32_t)spend / total);
    taken += st.contents.Take(before.p[i].mat, part);
  }
  while (taken < (uint32_t)spend && st.Filled()) {
    uint16_t m = ContainerMainMat(st.contents);
    taken += st.contents.Take(m, 1);
  }
  return (int)taken;
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
  // Whole cells, rounded UP: a flask with one eighth left is not "0 water".
  auto cellsOf = [](uint32_t e) {
    return (int)((e + kContainerUnitsPerCell - 1) / kContainerUnitsPerCell);
  };
  auto nameOf = [&](uint16_t m) {
    const uint16_t b = alchemy::BaseMat(m);
    if (b >= mats.size()) return std::string("?");
    return alchemy::IsDissolved(m) ? mats[b].name + " (dissolved)" : mats[b].name;
  };
  char b[96];
  const char* cork = st.stoppered ? " (stoppered)" : "";
  if (st.contents.n == 1) {
    std::snprintf(b, sizeof b, "%s %d/%d%s", nameOf(st.contents.p[0].mat).c_str(),
                  cellsOf(st.contents.p[0].eighths), capCells, cork);
    return b;
  }
  // A mixture: the main portion by name, the count of the others, the total.
  std::snprintf(b, sizeof b, "%s +%d more %d/%d%s",
                nameOf(ContainerMainMat(st.contents)).c_str(), st.contents.n - 1,
                cellsOf(st.FillTotal()), capCells, cork);
  return b;
}

uint32_t ContainerFillSwatch(const ItemStack& st,
                             const std::vector<MaterialDef>& mats) {
  const uint16_t m = ContainerMainMat(st.contents);
  if (m == 0 || m >= mats.size()) return 0;
  return 0xFF000000u | (mats[m].gpu.color0 & 0x00FFFFFFu);
}

alchemy::Composition ContainerParseFillSpec(const std::string& spec, int capacity,
                                            const std::vector<MaterialDef>& mats) {
  alchemy::Composition out;
  size_t at = 0;
  while (at <= spec.size()) {
    const size_t plus = spec.find('+', at);
    const std::string part = spec.substr(at, plus == std::string::npos ? std::string::npos : plus - at);
    at = plus == std::string::npos ? spec.size() + 1 : plus + 1;
    if (part.empty()) continue;
    const size_t colon = part.find(':');
    const std::string name = part.substr(0, colon);
    const float frac = colon == std::string::npos ? 1.0f : (float)std::atof(part.c_str() + colon + 1);
    uint16_t mat = 0;
    for (size_t m = 1; m < mats.size(); m++)
      if (mats[m].name == name) mat = (uint16_t)m;
    const int room = capacity - (int)out.Total();
    const int amt = std::clamp((int)std::lround(frac * (float)capacity), 0, std::max(0, room));
    if (mat && amt > 0) out.Add(mat, (uint32_t)amt);
  }
  return out;
}

uint16_t ContainerMainMat(const alchemy::Composition& c) {
  // A dissolved portion is not a material a flask can show (it has no layer).
  uint32_t best = 0;
  uint16_t m = 0;
  for (int i = 0; i < c.n; i++)
    if (!alchemy::IsDissolved(c.p[i].mat) && c.p[i].eighths > best) { best = c.p[i].eighths; m = c.p[i].mat; }
  return m;
}

uint16_t ContainerTopMat(const ItemInstance& st, const std::vector<MaterialDef>* mats) {
  const alchemy::Composition& c = st.contents;
  // A stoppered vessel has nothing at its mouth: every road out (pour, apply,
  // the brush) reads this and finds nothing.
  if (c.Empty() || st.stoppered) return 0;
  // Dissolved matter has no layer (it rides the liquid, ContainerTakeDissolvedShare)
  // and gas is not poured (an open vessel's gas vented on the bench).
  int best = -1;
  int32_t bestD = 0;
  for (int i = 0; i < c.n; i++) {
    const uint16_t m = c.p[i].mat;
    if (alchemy::IsDissolved(m)) continue;
    if (!mats) { if (best < 0) best = i; continue; }
    if (m < mats->size() && (*mats)[m].gpu.klass == CLASS_GAS) continue;
    const int32_t d = m < mats->size() ? (*mats)[m].gpu.density : 0;
    if (best < 0 || d < bestD) { best = i; bestD = d; }
  }
  return best < 0 ? 0 : c.p[best].mat;
}

bool ContainerPocketExplosion(const alchemy::Composition& c, const std::vector<MaterialDef>& mats,
                              const std::vector<ReactionGpu>& reactions, ReactionEffect& out,
                              uint16_t with) {
  std::vector<MaterialGpu> gpu;   // ReactNbrMatches reads the gpu rows
  for (int a = 0; a < c.n; a++) {
    const uint16_t ma = c.p[a].mat;
    if (alchemy::IsDissolved(ma) || ma == 0 || ma >= mats.size()) continue;
    const MaterialDef& A = mats[ma];
    if (A.ruleFx.empty()) continue;
    for (uint32_t k = 0; k < A.gpu.reactCount && A.gpu.reactOffset + k < reactions.size(); k++) {
      if (k >= A.ruleFx.size() || A.ruleFx[k].effects.empty()) continue;
      const ReactionGpu& r = reactions[A.gpu.reactOffset + k];
      if ((r.packed & 3u) != kReactPair) continue;
      if (!alchemy::ChemGateOpen(r.cond, 0u) || RuleNeedsSolute(A, k)) continue;
      const ReactionEffect* ex = nullptr;
      for (const ReactionEffect& e : A.ruleFx[k].effects)
        if (e.kind == "explode") { ex = &e; break; }
      if (!ex) continue;
      if (gpu.empty()) {
        gpu.reserve(mats.size());
        for (const MaterialDef& m : mats) gpu.push_back(m.gpu);
      }
      for (int b = 0; b < c.n; b++) {
        const uint16_t mb = c.p[b].mat;
        if (b == a || alchemy::IsDissolved(mb) || mb == 0 || mb >= mats.size()) continue;
        if (with != 0 && ma != with && mb != with) continue;
        if (ReactNbrMatches(r, mb, gpu)) {
          out = *ex;
          return true;
        }
      }
    }
  }
  return false;
}

uint32_t ContainerVolume(const alchemy::Composition& c, const std::vector<MaterialDef>& mats) {
  uint32_t v = 0;
  for (int i = 0; i < c.n; i++) {
    const uint16_t m = c.p[i].mat;
    if (alchemy::IsDissolved(m) || (m < mats.size() && mats[m].gpu.klass == CLASS_GAS)) continue;
    v += c.p[i].eighths;
  }
  return v;
}

uint32_t ContainerLiquidEighths(const alchemy::Composition& c, const std::vector<MaterialDef>* mats) {
  uint32_t v = 0;
  for (int i = 0; i < c.n; i++) {
    const uint16_t m = c.p[i].mat;
    if (alchemy::IsDissolved(m)) continue;
    if (mats && (m >= mats->size() || (*mats)[m].gpu.klass != CLASS_LIQUID)) continue;
    v += c.p[i].eighths;
  }
  return v;
}

alchemy::Composition ContainerTakeDissolvedShare(alchemy::Composition& c, uint32_t eighths,
                                                 uint32_t liquidBefore) {
  alchemy::Composition out;
  if (!eighths) return out;
  alchemy::Composition before = c;
  for (int i = 0; i < before.n; i++) {
    const uint16_t m = before.p[i].mat;
    if (!alchemy::IsDissolved(m)) continue;
    // With the last of the liquid, all of it; else its share, rounded down
    // (the remainder leaves with a later pour).
    const uint32_t share = eighths >= liquidBefore
                               ? before.p[i].eighths
                               : (uint32_t)((uint64_t)before.p[i].eighths * eighths / std::max(1u, liquidBefore));
    const uint32_t got = c.Take(m, share);
    if (got) out.Add(m, got);
  }
  return out;
}

namespace {
uint64_t g_solutePoursDropped = 0;
}  // namespace

int ContainerDissolvedToSolution(uint16_t mat, int eighths, IVec3 cell, uint32_t landTick,
                                 std::vector<ContainerSolutePour>& out) {
  if (eighths <= 0 || !alchemy::IsDissolved(mat)) return 0;
  if (CurrentTuning().sim.soluteMode == 0) return 0;
  const SoluteDef* sd = SoluteFromPowder(CurrentSolutes(), alchemy::BaseMat(mat));
  if (!sd || sd->species == 0 || sd->species > kCellOpSoluteSpeciesMax) return 0;
  const uint32_t y8 = std::max<uint32_t>(1u, sd->yieldPerVoxel / 8u);
  // One op's worth at most per entry: the GPU's unit field and the powder a
  // pour with nowhere to dissolve leaves both bound it.
  const uint32_t per = std::max<uint32_t>(
      1u, std::min<uint32_t>(kSolutePourMaxEighths, kCellOpSoluteUnitsMax / y8));
  for (uint32_t left = (uint32_t)eighths; left > 0;) {
    const uint32_t e = std::min(left, per);
    out.push_back({landTick, cell, sd->species, e * y8});
    left -= e;
  }
  return eighths;
}

int ContainerSolutePoursDue(std::vector<ContainerSolutePour>& pending, uint32_t tick,
                            const World& world, std::vector<CellOp>& cells) {
  int sent = 0;
  size_t w = 0;
  for (size_t i = 0; i < pending.size(); i++) {
    ContainerSolutePour& p = pending[i];
    if (p.units == 0) continue;   // merged into an earlier one below
    if (p.tick > tick || sent >= (int)kMaxSolutePoursPerTick) {
      pending[w++] = p;   // not landed yet, or the tick is full: next tick
      continue;
    }
    if (!world.CellInWindow(p.cell)) {
      g_solutePoursDropped += p.units;
      std::fprintf(stderr, "solute pour: %u units of species %u landed outside the window at "
                   "(%d,%d,%d) and are lost\n", p.units, (unsigned)p.species, p.cell.x,
                   p.cell.y, p.cell.z);
      continue;
    }
    // Merge every later due pour of the same cell and species into this op
    // (a stream lands on one cell tick after tick), up to the op's field.
    // Merged no further than ONE entry's worth (kSolutePourMaxEighths): the
    // GPU's powder fallback searches room for exactly that much, so a merged
    // op over dry ground would lose the rest (SOLM_POUR_LOST).
    const SoluteDef* msd = CurrentSoluteById(p.species);
    const uint32_t my8 = std::max<uint32_t>(1u, msd ? msd->yieldPerVoxel / 8u : 32u);
    const uint32_t mergeCap = std::min<uint32_t>(kCellOpSoluteUnitsMax, kSolutePourMaxEighths * my8);
    uint32_t units = p.units;
    for (size_t j = i + 1; j < pending.size(); j++) {
      ContainerSolutePour& q = pending[j];
      if (q.units == 0 || q.tick > tick || q.species != p.species || q.cell.x != p.cell.x ||
          q.cell.y != p.cell.y || q.cell.z != p.cell.z || units + q.units > mergeCap)
        continue;
      units += q.units;
      q.units = 0;
    }
    cells.push_back({World::SlotCellIndex(p.cell), CellOpSolute(p.species, units)});
    sent++;
  }
  pending.resize(w);
  return sent;
}

uint64_t ContainerSolutePoursDropped() { return g_solutePoursDropped; }

int ContainerDissolvedToWorld(uint16_t mat, int eighths, Vec3 at, Vec3 vel, uint32_t seed,
                              uint32_t tick, const std::vector<MaterialDef>& mats,
                              std::vector<ParticleSpawn>& parts, uint32_t partRoom,
                              std::vector<ContainerSolutePour>* sol, IVec3 land,
                              uint32_t landTick) {
  // ---- IN SOLUTION, when there is a pour queue and a solute layer. --------
  if (sol && eighths > 0) {
    const int q = ContainerDissolvedToSolution(mat, eighths, land, std::max(landTick, tick), *sol);
    if (q >= eighths) return q;
  }
  // ---- THE FALLBACK: its powder. ------------------------------------------
  const uint16_t powder = alchemy::BaseMat(mat);
  if (powder == 0 || powder >= mats.size() || eighths <= 0) return 0;
  const bool liquid = mats[powder].gpu.klass == CLASS_LIQUID;
  int emitted = 0;
  uint32_t made = 0;
  while (emitted < eighths && parts.size() < kMaxParticleSpawnsPerTick && made < partRoom) {
    const int spend = liquid ? std::min(kContainerUnitsPerCell, eighths - emitted) : 1;
    const uint32_t h = rng::Hash3(seed, tick, (uint32_t)(eighths - emitted) * 0x9E3779B9u + 5u);
    auto u = [&](uint32_t salt) { return ((float)(rng::Pcg(h ^ salt) & 0xFFFFu) / 65535.0f) * 2.0f - 1.0f; };
    ParticleSpawn ps{};
    ps.px = (int32_t)std::lround((at.x + u(0x11u) * 0.3f) * 256.0f);
    ps.py = (int32_t)std::lround((at.y + u(0x22u) * 0.3f) * 256.0f);
    ps.pz = (int32_t)std::lround((at.z + u(0x33u) * 0.3f) * 256.0f);
    ps.vx = (int32_t)std::lround((vel.x * (1.0f + u(0x44u) * 0.05f)) * 256.0f);
    ps.vy = (int32_t)std::lround(vel.y * 256.0f);
    ps.vz = (int32_t)std::lround((vel.z * (1.0f + u(0x55u) * 0.05f)) * 256.0f);
    ps.payload = ((uint32_t)powder & 0xFFFu) | (PouredState(liquid, spend) << 12);
    ps.flags = kPFlagAlive | kPFlagCalm | (liquid ? kPFlagMeasured : 0u);
    parts.push_back(ps);
    made++;
    emitted += spend;
  }
  return emitted;
}

ContainerHeldFill ContainerHeldFillFrom(const ItemDef& def, const alchemy::Composition& c,
                                        bool stoppered) {
  ContainerHeldFill f;
  if (!def.IsContainer()) return f;
  if (!stoppered) f.open = def.container.stopperSlices;
  const uint16_t mat = ContainerMainMat(c);
  const uint32_t amt = c.Total();
  if (mat == 0 || amt == 0) return f;
  f.mat = mat;
  f.slices = def.container.fillSlices;
  f.frac = std::clamp((float)amt / (float)std::max(1, def.container.capacity),
                      0.0f, 1.0f);
  f.dims = def.container.fillDims;
  f.cells = def.container.fillCells;
  return f;
}

ContainerHeldFill ContainerHeldFillOf(const ItemDef& def, const ItemInstance& st) {
  return ContainerHeldFillFrom(def, st.contents, st.stoppered);
}

float ContainerFillGlow(const ItemInstance& st,
                        const std::vector<MaterialDef>& mats) {
  // The brightest portion: a drop of lava in water still glows.
  uint32_t e = 0;
  for (int i = 0; i < st.contents.n; i++) {
    const uint16_t m = st.contents.p[i].mat;
    if (m < mats.size()) e = std::max(e, mats[m].gpu.emission);
  }
  return (float)e / 255.0f;
}
