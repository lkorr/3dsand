// lightning.cpp — see lightning.h.
#include "game/lightning.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <string>

#include "game/spell.h"
#include "sim/rng.h"

namespace {

// A strike prefers a GOOD conductor: materials.json `electric.resist` at or
// under this (the charge field's own number, src/sim/elec.h -- metals 1..4,
// water 6, blood / coolant 8). Wood (60) and flesh (20) carry charge but are
// not what a bolt seeks out.
constexpr uint32_t kStrikeConductMaxResist = 8;

// -1, 0 or +1 from two bits of a hash: 1/4 each way, 1/2 none.
int Kick(uint32_t h) {
  const uint32_t b = h & 3u;
  return b == 0 ? 1 : b == 1 ? -1 : 0;
}

void Push(StrikePlan& p, const World& world, IVec3 c, uint32_t mat, uint32_t h) {
  if (p.cells.size() >= kStrikeMaxCells) return;
  if (!world.CellInWindow(c)) return;
  p.cells.push_back({World::SlotCellIndex(c), PackVoxNew(mat, (h >> 28) % 3u) | kCellOpIfAir});
}

uint32_t StrikeMatAt(void* ctx, int32_t x, int32_t y, int32_t z, bool& known) {
  const World& world = *(const World*)ctx;
  known = false;
  const IVec3 wc{x >> 4, y >> 4, z >> 4};
  if (!world.ChunkInWindow(wc)) return 0;
  const int lx = x & 15, ly = y & 15, lz = z & 15;
  const size_t li = (size_t)((lz * (int)kChunk + ly) * (int)kChunk + lx);
  const WorldSnapshot& s = world.Snap();
  if (s.valid) {
    const int cx = wc.x - s.mirrorBase.x, cy = wc.y - s.mirrorBase.y,
              cz = wc.z - s.mirrorBase.z;
    if (cx >= 0 && cy >= 0 && cz >= 0 && cx < 3 && cy < 3 && cz < 3) {
      const size_t idx = (size_t)((cz * 3 + cy) * 3 + cx) * kChunkVol + li;
      if (idx < s.mirror.size()) {
        known = true;
        return s.mirror[idx] & 0xFFFu;
      }
    }
  }
  const CachedChunk* cc = world.Cached(wc);
  if (cc != nullptr && cc->voxels.size() == kChunkVol) {
    known = true;
    return cc->voxels[li] & 0xFFFu;
  }
  return 0;
}

}  // namespace

SpellProbe WorldStrikeProbe(const World& world) {
  SpellProbe p;
  p.matAt = StrikeMatAt;
  p.ctx = (void*)&world;
  return p;
}

void StrikeMats::Resolve(const std::vector<MaterialDef>& mats) {
  lightning = arc = 0;
  conductive.assign(mats.size(), 0);
  passable.assign(mats.size(), 0);
  for (size_t i = 0; i < mats.size(); i++) {
    const MaterialDef& m = mats[i];
    if (m.name == "lightning") lightning = (uint32_t)i;
    if (m.name == "arc") arc = (uint32_t)i;
    if (i == 0 || m.gpu.klass == CLASS_GAS) passable[i] = 1;
    if (m.elec.resist != 0 && m.elec.resist <= kStrikeConductMaxResist) conductive[i] = 1;
  }
  resolvedFor = mats.size();
}

StrikeSpec StrikeSpecFromGlyph(const GlyphStrike& gs, IVec3 aim, int32_t scaleMille,
                               uint32_t key) {
  const int64_t sc = std::max(scaleMille, 1);
  StrikeSpec spec;
  spec.aim = aim;
  spec.searchRadius = gs.search;
  spec.height = (int32_t)std::min<int64_t>((int64_t)gs.height * sc / 1000, kStrikeMaxHeight);
  spec.conductBonus = gs.conductBonus;
  spec.boltMat = gs.boltMat;
  spec.splashMat = gs.splashMat;
  spec.splashRadius = gs.splash;
  spec.arcs = (int32_t)std::clamp<int64_t>((int64_t)gs.arcs * sc / 1000, gs.arcs > 0 ? 1 : 0,
                                           kStrikeMaxArcs);
  spec.forks = gs.forks;
  spec.atAim = gs.height == 0 || gs.boltMat == 0;
  spec.key = key;
  return spec;
}

StrikeSpec WeatherStrikeSpec(const StrikeMats& mats, IVec3 aim, uint32_t key) {
  StrikeSpec spec;
  spec.aim = aim;
  spec.searchRadius = 8;
  spec.height = 56;
  spec.conductBonus = 6;
  spec.boltMat = mats.lightning;
  spec.splashMat = mats.arc;
  spec.splashRadius = 3;
  spec.arcs = 6;
  spec.forks = 3;
  spec.key = key;
  return spec;
}

StrikePlan PlanStrike(const StrikeSpec& spec, const StrikeMats& mats,
                      const SpellProbe* probe, const World& world) {
  StrikePlan p;
  const uint32_t key = spec.key;
  const IVec3 aim = spec.aim;
  p.target = aim;

  // ---- 1. TARGET ------------------------------------------------------------
  // Scan every column of the disc down through the mirror to its first
  // non-gas cell. Column order is fixed (z then x), and the score's ties break
  // on distance and then on a hash, so the choice is a pure function of the
  // mirror and the key.
  if (!spec.atAim && probe && probe->matAt) {
    const int r = std::clamp(spec.searchRadius, 0, kStrikeMaxSearch);
    bool any = false;
    int64_t best = INT64_MIN;
    for (int dz = -r; dz <= r; dz++)
      for (int dx = -r; dx <= r; dx++) {
        if (dx * dx + dz * dz > r * r) continue;
        const int x = aim.x + dx, z = aim.z + dz;
        int topY = INT32_MIN;
        uint32_t topMat = 0;
        bool known = true;
        for (int y = aim.y + kStrikeScanUp; y >= aim.y - kStrikeScanDown; y--) {
          bool k = false;
          const uint32_t m = probe->matAt(probe->ctx, x, y, z, k);
          if (!k) {
            // Above the mirror is unknown sky: keep descending. Below a known
            // cell it means the column leaves the mirror before it hits
            // anything, and the column does not compete.
            if (y < aim.y) { known = false; break; }
            continue;
          }
          if (mats.Passable(m)) continue;
          topY = y;
          topMat = m;
          break;
        }
        if (!known || topY == INT32_MIN) continue;
        p.columnsScanned++;
        const bool cond = mats.Conductive(topMat);
        // Score: height in cells, the conductor bonus, then -distance, then a
        // hash, packed so one integer compare orders all four.
        const int64_t d2 = (int64_t)dx * dx + (int64_t)dz * dz;
        const int64_t score = ((int64_t)topY + (cond ? spec.conductBonus : 0)) * (1ll << 32) -
                              d2 * (1ll << 16) +
                              (int64_t)(rng::Hash3(key, (uint32_t)x, (uint32_t)z) & 0xFFFFu);
        if (!any || score > best) {
          any = true;
          best = score;
          p.target = IVec3{x, topY, z};
          p.targetMat = topMat;
          p.targetConductive = cond;
        }
      }
    p.targetKnown = any;
  }
  if (spec.atAim && probe && probe->matAt) {
    bool k = false;
    p.targetMat = probe->matAt(probe->ctx, aim.x, aim.y, aim.z, k);
    p.targetKnown = k;
    p.targetConductive = k && mats.Conductive(p.targetMat);
  }

  // The FOOT: the cell the bolt arrives in, just above the struck top. A shock
  // goes off at the aim itself.
  const IVec3 foot = spec.atAim ? aim : IVec3{p.target.x, p.target.y + 1, p.target.z};

  // ---- 2. BOLT ----------------------------------------------------------------
  const int h = std::clamp(spec.height, 0, kStrikeMaxHeight);
  if (h > 0 && spec.boltMat != 0) {
    // Start offset: up to h/6 (max 6) cells off the column, hashed.
    const int span = std::min(6, std::max(1, h / 6));
    const uint32_t h0 = rng::Hash3(key, 0xB0170000u, 0u);
    int ox = (int)(h0 % (uint32_t)(2 * span + 1)) - span;
    int oz = (int)((h0 >> 8) % (uint32_t)(2 * span + 1)) - span;
    // Fork points: up to `forks` steps on the upper two thirds of the bolt.
    const int nForks = std::clamp(spec.forks, 0, 3);
    // Render geometry (StrikePath): the channel first, its forks after it.
    p.paths.push_back({StrikePath::Channel, {}});
    p.paths[0].cells.reserve((size_t)h);
    for (int st = h; st >= 1; st--) {
      // `st` steps above the foot: y = foot.y + st - 1 ... foot.y.
      const uint32_t hs = rng::Hash3(key, 0xB0180000u, (uint32_t)st);
      int kx = Kick(hs), kz = Kick(hs >> 2);
      // Pull back so the bolt can always arrive: |offset| <= steps left.
      const int left = st - 1;
      if (std::abs(ox + kx) > left) kx = ox > 0 ? -1 : ox < 0 ? 1 : 0;
      if (std::abs(oz + kz) > left) kz = oz > 0 ? -1 : oz < 0 ? 1 : 0;
      ox += kx;
      oz += kz;
      const IVec3 c{foot.x + ox, foot.y + st - 1, foot.z + oz};
      Push(p, world, c, spec.boltMat, hs);
      p.boltCells++;
      p.paths[0].cells.push_back(c);
      // A fork: a short diagonal run down and away from the main channel.
      for (int f = 0; f < nForks; f++) {
        const uint32_t hf = rng::Hash3(key, 0xB0190000u, (uint32_t)f);
        const int at = h / 3 + (int)(hf % (uint32_t)std::max(1, (2 * h) / 3));
        if (at != st || st < 4) continue;
        const int fx = (hf >> 12) & 1u ? 1 : -1, fz = (hf >> 13) & 1u ? 1 : -1;
        const int len = 3 + (int)((hf >> 16) % 4u);
        IVec3 fc = c;
        StrikePath fork{StrikePath::Fork, {c}};
        for (int k = 0; k < len; k++) {
          const uint32_t hk = rng::Hash3(key ^ hf, 0xB01A0000u, (uint32_t)k);
          fc.x += (hk & 1u) ? fx : 0;
          fc.z += (hk & 2u) ? fz : 0;
          fc.y -= 1;
          Push(p, world, fc, spec.boltMat, hk);
          p.boltCells++;
          fork.cells.push_back(fc);
        }
        p.paths.push_back(std::move(fork));
      }
    }
  }

  // ---- 3. SPLASH ----------------------------------------------------------------
  // Jagged walks out of the foot (ReactFxAftermath's arc walk, biased outward
  // and down so the crackle runs over the ground round the struck thing).
  if (spec.splashMat != 0) {
    const int nArc = std::clamp(spec.arcs, 0, kStrikeMaxArcs);
    const int len = 2 * std::clamp(spec.splashRadius, 1, kStrikeMaxSplash);
    for (int k = 0; k < nArc; k++) {
      const uint32_t hd = rng::Hash3(key, 0xA2C60000u, (uint32_t)k);
      // An outward heading round the compass (8 directions), never straight up.
      static const int kDir[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1},
                                     {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
      const int di = (int)((hd + (uint32_t)k * 3u) % 8u);
      const int hx = kDir[di][0], hz = kDir[di][1];
      IVec3 c = foot;
      StrikePath walk{StrikePath::Splash, {foot}};
      for (int st = 0; st < len; st++) {
        const uint32_t hs = rng::Hash3(key ^ (uint32_t)k * 0x9E3779B9u, 0xA2C70000u, (uint32_t)st);
        c.x += ((hs >> 4) & 1u) ? hx : Kick(hs);
        c.z += ((hs >> 5) & 1u) ? hz : Kick(hs >> 2);
        // Down one in three steps, up one in eight: it hugs and runs down.
        const uint32_t v = (hs >> 8) % 24u;
        c.y += v < 8 ? -1 : v < 11 ? 1 : 0;
        Push(p, world, c, spec.splashMat, hs);
        p.splashCells++;
        walk.cells.push_back(c);
      }
      p.paths.push_back(std::move(walk));
    }
  }
  return p;
}

bool EmitStrike(const StrikePlan& plan, StrikeBudget& budget, std::vector<CellOp>& cellOps) {
  const uint32_t n = (uint32_t)plan.cells.size();
  if (n == 0) return false;
  // Charged BEFORE emission, whole or not at all (CLAUDE.md: budgets are
  // charged before emission, an op refused if it does not fit).
  if (budget.used + n > budget.cap || cellOps.size() + n > kMaxCellOpsPerTick) {
    budget.refused++;
    return false;
  }
  budget.used += n;
  cellOps.insert(cellOps.end(), plan.cells.begin(), plan.cells.end());
  budget.emitted++;
  budget.cells += n;
  return true;
}

bool LightningStrike(const StrikeSpec& spec, const StrikeMats& mats, const SpellProbe* probe,
                     const World& world, StrikeBudget& budget, std::vector<CellOp>& cellOps,
                     StrikePlan* planOut) {
  StrikePlan p = PlanStrike(spec, mats, probe, world);
  const bool ok = EmitStrike(p, budget, cellOps);
  if (planOut) *planOut = std::move(p);
  return ok;
}
