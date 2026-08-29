#include "sim/waterbody.h"

#include <algorithm>
#include <cstring>

#include "sim/tuning.h"

namespace sandvox {

namespace {

// Floor division, the same shape worldgen.wgsl's `fdiv` has. C's `/` truncates
// toward zero, so -1 / 448 is 0 and a column one voxel west of the origin would
// be assigned to tile 0 alongside a column 447 voxels east of it — the tile
// grid would have a double-width column through x = 0.
int fdiv(int a, int b) { return a >= 0 ? a / b : -(((-a) + b - 1) / b); }

// Integer square root by Newton, exact for the whole i32 range. No std::sqrt:
// this feeds a cell COUNT that a mass ledger will one day be compared against,
// and a double that rounds 4489.0 to 4488.9999 turns a 67-cell row into a
// 66-cell row on one machine and not another (rule 1).
uint32_t isqrt(uint64_t n) {
  if (n == 0) return 0;
  uint64_t x = n, y = (x + 1) / 2;
  while (y < x) {
    x = y;
    y = (x + n / x) / 2;
  }
  return (uint32_t)x;
}

// Lattice points inside a disc: #{(dx,dz) integer : dx*dx + dz*dz <= m}.
//
// EXACT, not pi*r*r. At r = 68 the closed form is 14,527.4 and the true count is
// 14,545 — 17 cells, which is 136 eighths, which is the difference between a
// conservation gate that passes and one that reports an unattributable leak the
// moment this number is allowed to become a mass. It is O(r) and it is called
// once per basin per height, so ~3,500 isqrt calls for the largest natural
// pond. That is nothing, and it is paid once at registry build.
uint32_t discCells(int64_t m) {
  if (m < 0) return 0;
  const int64_t rmax = (int64_t)isqrt((uint64_t)m);
  uint32_t n = 0;
  for (int64_t dx = -rmax; dx <= rmax; dx++) {
    const int64_t rem = m - dx * dx;
    n += 2u * isqrt((uint64_t)rem) + 1u;
  }
  return n;
}

// The basin's cross-section at height `y`, as an inclusive squared-radius
// bound. Negative means "the basin does not reach this height".
//
// This is the ONE place the bowl's shape is turned into a number, and it is the
// exact algebraic inverse of `pondAt`'s parabola rather than a resampling of
// it. pondAt fills (floor, surf] where
//
//     depth(d2) = rimDepth + ((r^2 - d2) * (centreDepth - rimDepth)) / r^2
//     floor(d2) = surf - depth(d2)                       [integer division]
//
// so a cell at height y is inside iff floor(d2) < y, i.e. depth(d2) >= k+1
// where k = surf - y. Since the division truncates and every term is
// non-negative, `floor(a/b) >= m` is exactly `a >= m*b`, which rearranges to a
// bound on d2 with no floating point and no off-by-one guessing:
//
//     d2 <= r^2 - ceil((k + 1 - rimDepth) * r^2 / (centreDepth - rimDepth))
int64_t crossSectionD2(const WaterBasin& b, int y) {
  if (y <= b.floorY || y > b.spillY) return -1;
  // ABOVE THE AUTHORED SURFACE the container is the berm's inner core, which is
  // WIDER than the disc. Reported as the disc — the parabola's own limit at
  // y = surfY — because the curve's job is to pace a surface that is DESCENDING
  // and a body above its own fill level is overflowing, which component 5
  // refuses to adopt anyway. Under-reporting here can only make a full body
  // read as slightly fuller, never as holding water it does not have.
  if (y > b.surfY) return b.discD2Max;
  if (b.kind == WaterBasinKind::FlatDisc) return b.discD2Max;

  const int64_t R2 = (int64_t)b.radius * (int64_t)b.radius;
  const int64_t A = b.centreDepth - b.rimDepth;
  const int64_t k = b.surfY - y;
  const int64_t m = k + 1 - b.rimDepth;
  if (m <= 0) return b.discD2Max;      // shallower than the rim: the whole disc
  if (A <= 0) return -1;               // degenerate flat bowl, already handled
  const int64_t need = (m * R2 + A - 1) / A;   // ceil
  const int64_t maxD2 = R2 - need;
  if (maxD2 < 0) return -1;
  return std::min<int64_t>(maxD2, b.discD2Max);
}

}  // namespace

// ---- component 2, the analytic half ---------------------------------------

WaterBasinCurve WaterBasinBuildCurve(const WaterBasin& b) {
  WaterBasinCurve c;
  c.floorY = b.floorY;
  const int top = b.spillY;
  if (top <= b.floorY) return c;
  c.area.reserve((size_t)(top - b.floorY));
  c.prefix.reserve((size_t)(top - b.floorY));
  uint64_t running = 0;
  for (int y = b.floorY + 1; y <= top; y++) {
    const uint32_t a = discCells(crossSectionD2(b, y));
    // EIGHTHS, because that is the unit of the CA's state nibble and therefore
    // of the whole ledger. One full cell is 8; lowering the surface of a body
    // with `area` surface cells by one eighth costs exactly `area` eighths.
    // Integer against integer, no scaling, anywhere in this design.
    running += (uint64_t)a * 8u;
    c.area.push_back(a);
    c.prefix.push_back(running);
  }
  return c;
}

uint32_t WaterBasinAreaAt(const WaterBasinCurve& c, int y) {
  const int i = y - c.floorY - 1;
  if (i < 0 || i >= (int)c.area.size()) return 0;
  return c.area[(size_t)i];
}

uint64_t WaterBasinVolumeEighths(const WaterBasinCurve& c, int level) {
  if (c.prefix.empty()) return 0;
  int i = level - c.floorY - 1;
  if (i < 0) return 0;
  if (i >= (int)c.prefix.size()) i = (int)c.prefix.size() - 1;
  return c.prefix[(size_t)i];
}

int WaterBasinLevelFor(const WaterBasinCurve& c, uint64_t eighths,
                       uint32_t* remainder) {
  if (remainder) *remainder = 0;
  if (c.prefix.empty() || eighths == 0) return c.floorY;
  // Binary search of the prefix sum — the plan's `level(volume)`. `lower_bound`
  // finds the first level whose cumulative volume REACHES the requested one, so
  // a volume that lands exactly on a level boundary gives that level and a zero
  // remainder rather than the level above it with a full step of leftover.
  const auto it = std::lower_bound(c.prefix.begin(), c.prefix.end(), eighths);
  if (it == c.prefix.end()) {
    // More water than the container holds to its spill elevation. The excess is
    // NOT silently dropped: it is reported as the remainder so a caller that
    // sums remainders into a conservation total still balances, and so the
    // overflow is visible to component 5 as "this body is over its spill".
    if (remainder) *remainder = (uint32_t)std::min<uint64_t>(
        eighths - c.prefix.back(), 0xFFFFFFFFull);
    return c.floorY + (int)c.prefix.size();
  }
  const size_t i = (size_t)(it - c.prefix.begin());
  const uint64_t below = i == 0 ? 0 : c.prefix[i - 1];
  const uint64_t into = eighths - below;
  const uint64_t full = (uint64_t)c.area[i] * 8u;
  if (into >= full) {
    if (remainder) *remainder = 0;
    return c.floorY + 1 + (int)i;
  }
  // Partway into a level: the surface sits at the level BELOW and `into` eighths
  // are spread across it. That leftover is plan §3.3's legitimate divergence
  // between the ledger and the voxel sum, and it is stored rather than implied.
  if (remainder) *remainder = (uint32_t)into;
  return c.floorY + (int)i;
}

// ---- the system ------------------------------------------------------------

void WaterBodySystem::Reset() {
  basins_.clear();
  curves_.clear();
  bodies_.clear();
  chunkBody_.assign(kNumChunks, 0u);
  builtOrigin_ = IVec3{1 << 30, 1 << 30, 1 << 30};
  builtSeed_ = 0;
  straddles_ = 0;
  outOfWindow_ = 0;
  gpu_.bodies.clear();
  gpu_.chunks.clear();
  gpu_.bodyCount = 0;
  gpu_.writesThisTick = false;
}

void WaterBodySystem::RebuildBasins(const World& world, uint32_t seed) {
  basins_.clear();
  curves_.clear();

  const auto& wg = CurrentTuning().worldgen;
  const IVec3 o = world.WindowOrigin();
  const int lox = o.x * (int)kChunk, loz = o.z * (int)kChunk;
  const int hix = lox + (int)kWorldN - 1, hiz = loz + (int)kWorldN - 1;

  // The three authored pools first, so their ids stay 1..3 whatever the window
  // is. `pond68` — the scene both fluid benches measure — is the first of them.
  World::AuthoredPool pools[World::kAuthoredPools];
  World::AuthoredPoolList(pools);
  for (int i = 0; i < World::kAuthoredPools; i++) {
    const World::AuthoredPool& p = pools[i];
    WaterBasin b;
    b.id = (uint32_t)(i + 1);
    b.cx = p.cx;
    b.cz = p.cz;
    b.radius = p.r;
    // A column is inside an authored pool iff d2 < r*r (TerrainHeight's test),
    // one cell tighter than pondAt's `d2 <= r*r`. At r = 68 that ring is 428
    // cells; carrying the BOUND rather than the radius is what stops the two
    // conventions from quietly disagreeing by a ring of 2*pi*r cells.
    b.discD2Max = p.r > 0 ? p.r * p.r - 1 : -1;
    b.surfY = p.waterY;
    b.floorY = p.floorY;
    b.centreDepth = p.waterY - p.floorY;
    b.rimDepth = b.centreDepth;
    b.spillY = p.rimY;
    b.kind = WaterBasinKind::FlatDisc;
    b.matName = p.mat;
    basins_.push_back(std::move(b));
  }

  // Tarns: one scan of the pond tiles the window touches. A disc never leaves
  // its own tile (pondInfo's inset), so this finds every one that can reach in.
  const int tile = World::PondTileSize();
  if (tile > 0) {
    for (int tz = fdiv(loz, tile); tz <= fdiv(hiz, tile); tz++) {
      for (int tx = fdiv(lox, tile); tx <= fdiv(hix, tile); tx++) {
        const World::PondDisc d = World::PondTile(tx, tz, seed);
        if (!d.present) continue;
        if ((uint32_t)basins_.size() >= kWaterBodyCap) {
          // Rule 2: budgets are charged BEFORE emission. A window holding more
          // tarns than the cap simply leaves the extras unregistered, which
          // means "simulated the way they are today" — the same safe
          // degradation refusing adoption gives.
          continue;
        }
        WaterBasin b;
        // Stable identity from WHERE it is, not from discovery order: a
        // descriptor that renamed itself when the player walked away would
        // re-adopt every window move, and every re-adoption is a seam crossing.
        b.id = 0x80000000u | (((uint32_t)tx & 0xFFFFu) << 16) |
               ((uint32_t)tz & 0xFFFFu);
        b.cx = d.cx;
        b.cz = d.cz;
        b.radius = d.r;
        b.discD2Max = d.r * d.r;
        b.surfY = d.surf;
        b.centreDepth = wg.pondDepth;
        b.rimDepth = wg.pondDepthRim;
        b.floorY = d.surf - wg.pondDepth;
        b.spillY = d.surf + wg.pondBerm;
        b.kind = WaterBasinKind::ParabolicBowl;
        b.matName = "water";
        basins_.push_back(std::move(b));
      }
    }
  }

  curves_.reserve(basins_.size());
  for (const WaterBasin& b : basins_) curves_.push_back(WaterBasinBuildCurve(b));

  builtOrigin_ = o;
  builtSeed_ = seed;
}

void WaterBodySystem::Relabel(const World& world) {
  chunkBody_.assign(kNumChunks, 0u);
  straddles_ = 0;
  bodies_.assign(basins_.size(), WaterBodyDesc{});

  for (size_t i = 0; i < basins_.size(); i++) {
    const WaterBasin& b = basins_[i];
    WaterBodyDesc& d = bodies_[i];
    d.basinId = b.id;
    d.lo = IVec3{b.cx - b.radius, b.floorY + 1, b.cz - b.radius};
    d.hi = IVec3{b.cx + b.radius, b.surfY, b.cz + b.radius};
    if (d.hi.y < d.lo.y) continue;

    // Chunks the body's WATER occupies. Not the whole basin AABB: the berm and
    // the rim above the fill level hold no water, and labelling them would drag
    // a ring of dry chunks into the quiescence test that a passing footstep
    // could then keep permanently un-quiet.
    for (int cz = d.lo.z >> 4; cz <= (d.hi.z >> 4); cz++) {
      for (int cx = d.lo.x >> 4; cx <= (d.hi.x >> 4); cx++) {
        // Reject chunk columns whose nearest corner is outside the disc, so a
        // 137-cell disc costs ~81 chunk columns rather than the 81 of its
        // bounding square plus the corners nothing is in.
        const int nx = std::clamp(b.cx, cx * 16, cx * 16 + 15);
        const int nz = std::clamp(b.cz, cz * 16, cz * 16 + 15);
        const int64_t ddx = nx - b.cx, ddz = nz - b.cz;
        if (ddx * ddx + ddz * ddz > b.discD2Max) continue;
        for (int cy = d.lo.y >> 4; cy <= (d.hi.y >> 4); cy++) {
          const IVec3 wc{cx, cy, cz};
          if (!world.ChunkInWindow(wc)) continue;
          const uint32_t slot = World::SlotChunkIndex(wc);
          const uint32_t held = chunkBody_[slot];
          if (held != 0 && held != (uint32_t)(i + 1)) {
            // THE STRADDLE. Two basins at two levels in one chunk: a shave keyed
            // off the chunk's body would drop one of them at the other's level.
            // Refuse BOTH — falling back to the CA is a safe degradation and the
            // detection is this branch, where per-cell labelling to "fix" it
            // would cost the aux layer's entire reason for existing.
            //
            // The chunk keeps the FIRST body's label. That is not a preference:
            // both bodies are refused, so the label reaches nothing that acts,
            // and clearing it would mean walking the earlier body's chunk list
            // to keep the two views consistent for no consumer.
            d.straddle = true;
            bodies_[held - 1].straddle = true;
            straddles_++;
            continue;
          }
          chunkBody_[slot] = (uint32_t)(i + 1);
          d.chunks.push_back(slot);
        }
      }
    }
  }
}

// The MATERIAL a basin holds, by name. Material ids are fixed by materials.json
// ORDER and world.h states them (kMatWater/kMatOil/kMatLava) -- the same
// contract worldgen.wgsl already leans on when it writes `M_WATER : u32 = 5u`.
// Resolving here through those constants rather than a fresh literal keeps the
// count of places that know a water is 5 at one.
static uint32_t WaterMatId(const std::string& name) {
  if (name == "water") return kMatWater;
  if (name == "oil") return kMatOil;
  if (name == "lava") return kMatLava;
  return kMatAir;   // unknown: the shave would match nothing, so refuse instead
}

void WaterBodySystem::Classify(const World& world, uint32_t tick) {
  (void)tick;
  const auto& s = CurrentTuning().sim;

  // ---- pass 1: refresh every descriptor from its curve ---------------------
  // Separate from the ladder because the ladder's ORDER depends on the volumes,
  // and a sort whose key is filled in by the loop it orders is a sort on zeroes.
  for (size_t i = 0; i < bodies_.size(); i++) {
    WaterBodyDesc& d = bodies_[i];
    // The ANALYTIC prediction, and from M2 that is all it is. The GPU ledger
    // carries the live level and the live surface area, measured from the
    // voxels by the adoption reduce and then by every shave. What the CPU
    // publishes here is the SEED and the gate's reference value.
    d.level = basins_[i].surfY;
    d.surfaceArea = WaterBasinAreaAt(curves_[i], d.level);
    d.volumeEighths = WaterBasinVolumeEighths(curves_[i], d.level);
    d.matId = WaterMatId(basins_[i].matName);
  }

  // ---- pass 2: the ladder, biggest body first -----------------------------
  // Proposal order is a property of the WORLD (volume, then basin id), never of
  // iteration order, so "which body loses at the cap" is reproducible -- plan
  // section 3.4: a schedule is a function of the tick, never of what the CPU
  // reached first. Biggest first because the cap should refuse the body whose
  // honest simulation costs least.
  std::vector<size_t> order(bodies_.size());
  for (size_t i = 0; i < order.size(); i++) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t c) {
    if (bodies_[a].volumeEighths != bodies_[c].volumeEighths)
      return bodies_[a].volumeEighths > bodies_[c].volumeEighths;
    return basins_[a].id < basins_[c].id;
  });

  outOfWindow_ = 0;
  uint32_t proposed = 0;
  uint32_t chunkBudget = kWaterChunkCap;
  for (size_t oi = 0; oi < order.size(); oi++) {
    const size_t i = order[oi];
    const WaterBasin& b = basins_[i];
    WaterBodyDesc& d = bodies_[i];
    d.gpuSlot = kNoGpuSlot;

    // ---- component 5's ladder, enter side, DETERMINISTIC HALF ------------
    // Every test below is a pure function of (seed, window origin, tuning).
    // The one that is not -- quiescence -- is measured on the GPU now; see the
    // authority split at the top of waterbody.h. Ordered cheapest-and-most-
    // disqualifying first, and each refusal is NAMED, because "0 proposed" with
    // no reason attached is the bare count CLAUDE.md rule 6 says costs a dozen
    // elimination runs to un-ask.
    WaterBodyRefusal why = WaterBodyRefusal::None;
    if (d.straddle) {
      why = WaterBodyRefusal::Straddle;
    } else if (d.chunks.empty() ||
               !world.ChunkInWindow(IVec3{d.lo.x >> 4, d.lo.y >> 4, d.lo.z >> 4}) ||
               !world.ChunkInWindow(IVec3{d.hi.x >> 4, d.hi.y >> 4, d.hi.z >> 4})) {
      why = WaterBodyRefusal::OutOfWindow;
      outOfWindow_++;
    } else if (d.level >= b.spillY) {
      // Not overflowing is not flowing. A body at or over its spill elevation
      // is a stream with a wide bit in the middle, and the entire hydrostatic
      // assumption underneath a single `level` is false for it.
      why = WaterBodyRefusal::Overflowing;
    } else if (d.matId == kMatAir) {
      // A basin whose liquid this build has no id for. Refusing is the safe
      // degradation; proposing it would ship the shave a material id of AIR.
      why = WaterBodyRefusal::TooSmall;
    } else if (d.volumeEighths < (uint64_t)std::max(s.waterBodyMinVolume, 0)) {
      // Small ponds are cheap to simulate honestly and the model's error is
      // relatively largest there, so the threshold is a correctness argument
      // and not only a performance one.
      why = WaterBodyRefusal::TooSmall;
    } else if (proposed >= (uint32_t)std::clamp(s.waterBodyMaxCount, 0,
                                                (int)kWaterBodyCap)) {
      // At the cap, refuse. `order` put the biggest first, so what is refused
      // is the smallest candidate -- the one whose honest simulation costs
      // least.
      why = WaterBodyRefusal::AtCap;
    } else if (d.chunks.size() > chunkBudget) {
      // Rule 2 charges the budget BEFORE emission. See the note on
      // WaterBodyRefusal::NoChunkBudget for why a truncated list is worse than
      // no list at all.
      why = WaterBodyRefusal::NoChunkBudget;
    }

    if (why != WaterBodyRefusal::None) {
      d.refusal = why;
      // ---- the exit side, and the gap that makes it hysteresis ----------
      // A proposed body is NOT withdrawn by an ENTER test failing. It is
      // withdrawn by an EXIT test failing, and the exit thresholds sit
      // meaningfully clear of the enter ones. A body parked on one boundary
      // would otherwise flip representation every tick, and plan section 5 is
      // blunt about what that is: every flip is a seam crossing where mass can
      // be lost.
      if (d.state == WaterBodyState::Proposed) {
        const bool exitTrip =
            why == WaterBodyRefusal::Straddle ||
            why == WaterBodyRefusal::OutOfWindow ||
            why == WaterBodyRefusal::Overflowing ||
            why == WaterBodyRefusal::AtCap ||
            why == WaterBodyRefusal::NoChunkBudget ||
            (why == WaterBodyRefusal::TooSmall &&
             d.volumeEighths < (uint64_t)std::max(s.waterBodyExitVolume, 0));
        // TooSmall between the exit and enter thresholds is the hysteresis
        // BAND: the body stays proposed, which is the whole point of the gap.
        if (exitTrip) d.state = WaterBodyState::Releasing;
      }
      // A releasing body still needs a GPU slot: release must be MASS-EXACT,
      // and the ledger cannot pay off an outstanding debit for a body it has
      // been told nothing about (sim_waterbody.wgsl's WB_RELEASING). Straddle
      // and out-of-window are the two cases where it cannot get one, and both
      // mean the footprint is no longer addressable -- there is nothing to
      // shave with either way.
      if (d.state == WaterBodyState::Releasing && !d.chunks.empty() &&
          d.matId != kMatAir && d.chunks.size() <= chunkBudget &&
          proposed < kWaterBodyCap) {
        d.gpuSlot = proposed++;
        chunkBudget -= (uint32_t)d.chunks.size();
      } else if (d.state == WaterBodyState::Releasing) {
        d.state = WaterBodyState::Candidate;
      }
      continue;
    }

    d.refusal = WaterBodyRefusal::None;
    d.state = WaterBodyState::Proposed;
    d.gpuSlot = proposed++;
    chunkBudget -= (uint32_t)d.chunks.size();
  }
}

// ---- the CPU->GPU payload --------------------------------------------------
//
// Rebuilt from scratch every tick rather than carried, for the reason
// PageTable::UpdateSpawnRing is: a carried set is a set that can be stale, and
// a stale entry here is a shave aimed at a chunk the page table was never told
// about. Everything it reads is a pure function of the tick.
void WaterBodySystem::BuildGpu(int testDrain) {
  gpu_.bodies.assign(kWaterBodyScalars, 0);
  gpu_.chunks.clear();
  gpu_.bodyCount = 0;
  gpu_.writesThisTick = false;

  // Slot order, not registry order: the GPU indexes by slot and the ledger it
  // carries belongs to whatever body held that slot last tick.
  const WaterBodyDesc* bySlot[kWaterBodyCap] = {};
  for (const WaterBodyDesc& d : bodies_) {
    if (d.gpuSlot < kWaterBodyCap) bySlot[d.gpuSlot] = &d;
  }
  uint32_t n = 0;
  while (n < kWaterBodyCap && bySlot[n] != nullptr) n++;   // dense by Classify
  gpu_.bodyCount = n;
  if (n == 0) return;

  gpu_.chunks.reserve(kWaterChunkCap);
  for (uint32_t k = 0; k < n; k++) {
    const WaterBodyDesc& d = *bySlot[k];
    const WaterBasin* b = Basin(d.basinId);
    if (!b) continue;
    int32_t* row = &gpu_.bodies[(size_t)k * kWaterBodyWords];
    row[0] = b->cx;
    row[1] = b->cz;
    row[2] = b->discD2Max;
    row[3] = (int32_t)d.matId;
    row[4] = b->floorY;
    row[5] = d.level;                    // the analytic fill height: a SEED
    row[6] = (int32_t)d.surfaceArea;     // the analytic area: a SCHEDULE
    // Bit 0 = propose, bit 1 = release. Must match WBF_* in sim_waterbody.wgsl.
    // A releasing body carries BOTH: it is still governed (so the shave keeps
    // paying off its debit) and it is on its way out.
    row[7] = d.state == WaterBodyState::Releasing ? 3
                                                  : (d.state == WaterBodyState::Proposed ? 1 : 0);
    for (uint32_t slot : d.chunks) {
      if (gpu_.chunks.size() >= kWaterChunkCap) break;   // Classify charged this
      gpu_.chunks.push_back((k << 16) | (slot & 0xFFFFu));
    }
  }
  // WHEN THE FOOTPRINT MUST BE DECLARED TO THE PAGE TABLE. M2's only drain
  // source is the test tap, and it is CPU-known, so "a shave could fire" is a
  // question the CPU can answer exactly. M3 replaces this with "any body has a
  // hole", which is equally CPU-known (holes appear when someone digs, and
  // digging is a mutation op).
  //
  // Keeping it exact rather than conservative is what makes idle cost ZERO: a
  // still lake declares no op targets, materializes no pages and wakes no
  // chunks, so `--gate waterbody` pass E is a property of the design and not of
  // a threshold.
  gpu_.writesThisTick = testDrain > 0 && !gpu_.chunks.empty();
}

void WaterBodySystem::Tick(const World& world, uint32_t seed, uint32_t tick,
                           int mode, int testDrain) {
  mode_ = mode;
  if (mode == 0) {
    // THE OFF SWITCH, and it is an early-out rather than a flag consulted
    // later. Nothing is built, nothing is labelled, nothing is classified — so
    // `sim.waterBodyMode = 0` costs one compare and one branch per tick, and
    // "the feature is off" and "the tick is bit-identical to a build without
    // this file" are the same statement rather than two that have to be argued.
    if (!basins_.empty() || !bodies_.empty()) Reset();
    gpu_.bodies.clear();
    gpu_.chunks.clear();
    gpu_.bodyCount = 0;
    gpu_.writesThisTick = false;
    return;
  }
  if (chunkBody_.size() != kNumChunks) chunkBody_.assign(kNumChunks, 0u);

  // Rebuild + relabel only when the WINDOW or the SEED moved. Labelling is
  // O(basins x footprint chunks) — a few hundred chunk slots for a default pond
  // — and paying it every tick for a world nobody has walked across would be
  // rule 2 with the sign flipped. The per-tick cost is the classify pass, which
  // is O(bodies) plus one dirty-flag read per labelled chunk.
  const IVec3 o = world.WindowOrigin();
  if (seed != builtSeed_ || o.x != builtOrigin_.x || o.y != builtOrigin_.y ||
      o.z != builtOrigin_.z || basins_.empty()) {
    const std::vector<WaterBodyState> was = [&] {
      std::vector<WaterBodyState> v;
      v.reserve(bodies_.size());
      for (const auto& d : bodies_) v.push_back(d.state);
      return v;
    }();
    const std::vector<uint32_t> wasId = [&] {
      std::vector<uint32_t> v;
      v.reserve(bodies_.size());
      for (const auto& d : bodies_) v.push_back(d.basinId);
      return v;
    }();
    RebuildBasins(world, seed);
    Relabel(world);
    // Carry the ladder state across a rebuild BY BASIN ID. Without this a
    // window shift silently returns every adopted body to Candidate and the
    // quiescence window restarts — which is not merely wasteful, it is the
    // representation flip plan §5 spends a paragraph banning, arriving through
    // the back door of "the registry was rebuilt".
    for (auto& d : bodies_) {
      for (size_t k = 0; k < wasId.size(); k++) {
        if (wasId[k] == d.basinId) { d.state = was[k]; break; }
      }
    }
  }
  Classify(world, tick);
  BuildGpu(testDrain);
}

uint32_t WaterBodySystem::ProposedCount() const {
  uint32_t n = 0;
  for (const auto& d : bodies_)
    if (d.state == WaterBodyState::Proposed) n++;
  return n;
}

const WaterBodyDesc* WaterBodySystem::Find(uint32_t basinId) const {
  for (const auto& d : bodies_)
    if (d.basinId == basinId) return &d;
  return nullptr;
}

const WaterBasin* WaterBodySystem::Basin(uint32_t basinId) const {
  for (const auto& b : basins_)
    if (b.id == basinId) return &b;
  return nullptr;
}

const WaterBasinCurve* WaterBodySystem::Curve(uint32_t basinId) const {
  for (size_t i = 0; i < basins_.size(); i++)
    if (basins_[i].id == basinId) return &curves_[i];
  return nullptr;
}

WaterBodySystem& WaterBodies() {
  static WaterBodySystem s;
  return s;
}

}  // namespace sandvox
