#include "game/container.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "game/mob.h"  // SplatterEvent
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
