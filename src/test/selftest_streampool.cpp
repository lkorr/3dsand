// selftest_streampool.cpp -- `stream-pool`: a stream from a FINITE source
// sleeps while it runs (docs/PLAN_running_water.md phase 3, THE CONDUIT;
// tickets 2026-10-16_11-16-43 and 11-14-41).
//
// THE CLAIM. A pour running down a hill used to keep every chunk along it
// awake until its source was empty: only a stream fed by the map's water (an
// infinite reservoir) was adopted as held. Now sim_stream.wgsl adopts a
// stream rooted on a finite source as a POOL stream and sim_step.wgsl moves
// its water through a LEDGER: what the source pours into the head is
// deposited, what the mouth spills is withdrawn, and the middle rests.
//
// THE FIXTURE, built from cell ops on the harness pad (no synthetic map: the
// whole structure is stone and air written over the ground, `ca-slope`'s
// way), all of it baseline-tunable (streamPool.*):
//   a RESERVOIR (resX x resZ, resH layers of water) on a stone pillar, its
//   downstream wall two cells thick; a CHANNEL chanW wide and chanLen long
//   out of that wall, its floor falling one voxel every chanRun columns; a
//   BASIN (basinX x basinZ) basinDepth under the channel's last floor. No
//   roof (sim_stream's surface walk stops at a wall over the column). Dim dawn
//   pinned (evaporation and freezing are authored mass sinks), sky clear.
// Then the GATE -- the reservoir wall's cells across the channel -- opens and:
//   A. RUNS + ADOPTED: the channel's columns are adopted as POOL streams
//      (>= streamPool.adoptMin of them at the best sample).
//   B. MIDDLE ASLEEP WHILE THE SOURCE DRAINS: the channel's middle chunks
//      (past headCols, before mouthCols) awake <= middleAwakeMax for
//      middleSleepSamples samples in a row while the reservoir still loses
//      water between them.
//   C. MASS: grid eighths over the audit box + MPM particles + the ledger's
//      live balance == the poured eighths within massTol, at every sample.
//   D. RUN OUT: once the reservoir holds under emptyFrac of the pour, after
//      settleTicks more: no POOL column left in the fixture, the fixture's
//      chunks asleep (<= endAwakeMax), and the ledger's stranded balance
//      |live| <= strandMax.
//   E. DETERMINISM: the same build + open twice from a fresh worldgen at the
//      same tick numbers, detTicks ticks on, hashed (fixture voxels, the
//      fixture's still-table words, the ledger): equal.
// Every sample prints tick : reservoir / channel / basin eighths, MPM, the
// ledger (live, snapshot), adopted (POOL / map) channel columns, awake
// head / middle / mouth chunks and the largest flood count, so a failure says
// which part did not happen (CLAUDE.md verification rule 6).
// Leaves a freshly generated world behind (the `lake-settle` exit).

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sim/tuning.h"
#include "sim/waterbody.h"
#include "sim/weather.h"
#include "sim/world.h"
#include "sim/worldgen_run.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

namespace selftest {

Status GateStreamPool(Ctx& c, std::string& detail) {
  using sandvox::kDefaultSeed;
  using sandvox::ReadVoxelsSync;
  using sandvox::SubmitWorldgen;
  std::vector<std::string> problems;
  auto fail = [&](const std::string& why) {
    detail = why;
    std::printf("stream-pool: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  };
  uint32_t mWater = 0;
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == "water") { mWater = static_cast<uint32_t>(i); break; }
  if (mWater == 0) return fail("no `water` material");

  auto num = [](const char* k, double d) { return static_cast<int>(BaselineNumber(k, d)); };
  const int resX = num("streamPool.resX", 32), resZ = num("streamPool.resZ", 32), resH = num("streamPool.resH", 4);
  const int chanLen = num("streamPool.chanLen", 80), chanW = num("streamPool.chanW", 3);
  const int chanRun = std::max(1, num("streamPool.chanRun", 4));
  const int basinX = num("streamPool.basinX", 40), basinZ = num("streamPool.basinZ", 40);
  const int basinDepth = num("streamPool.basinDepth", 10);
  const int headCols = num("streamPool.headCols", 12), mouthCols = num("streamPool.mouthCols", 12);
  const int sampleTicks = std::max(10, num("streamPool.sampleTicks", 150));
  const int maxTicks = num("streamPool.maxTicks", 9000);
  const int settleTicks = num("streamPool.settleTicks", 1500);
  const int detTicks = num("streamPool.detTicks", 1500);
  const int middleAwakeMax = num("streamPool.middleAwakeMax", 0);
  const int middleSleepSamples = num("streamPool.middleSleepSamples", 3);
  const long long massTol = num("streamPool.massTol", 64);
  const long long strandMax = num("streamPool.strandMax", 600);
  const int endAwakeMax = num("streamPool.endAwakeMax", 0);
  const double adoptMin = BaselineNumber("streamPool.adoptMin", 0.5);
  const double emptyFrac = BaselineNumber("streamPool.emptyFrac", 0.08);
  const double spentFrac = BaselineNumber("streamPool.spentFrac", 0.005);
  const int siteX = num("streamPool.siteX", 130), siteZ = num("streamPool.siteZ", 282);

  // ---- geometry (x runs downstream) ----
  const int xr0 = siteX;                       // reservoir interior
  const int xg0 = xr0 + resX;                  // the gate: 2 wall columns
  const int xc0 = xg0 + 2;                     // channel column k = 0
  const int xb0 = xc0 + chanLen;               // basin interior
  const int xEnd = xb0 + basinX + 1;           // last wall column
  const int xBeg = xr0 - 2;
  const int zc = siteZ;
  const int zr0 = zc - resZ / 2, zr1 = zr0 + resZ - 1;
  const int zch0 = zc - chanW / 2, zch1 = zch0 + chanW - 1;
  const int zb0 = zc - basinZ / 2, zb1 = zb0 + basinZ - 1;
  const int zBeg = std::min({zr0, zch0, zb0}) - 2, zEnd = std::max({zr1, zch1, zb1}) + 2;
  // The pad's ground over the footprint, and that it is DRY ground (a sea or
  // lake column holds a map target the stream could never be adopted over).
  int gMin = INT_MAX, gMax = INT_MIN, wet = 0;
  for (int z = zBeg; z <= zEnd; z++)
    for (int x = xBeg; x <= xEnd; x++) {
      const worldmap::WaterColumn wc = worldmap::WaterColumnAt(x, z, kDefaultSeed);
      if (wc.water >= 0) wet++;
      gMin = std::min(gMin, wc.h);
      gMax = std::max(gMax, wc.h);
    }
  if (wet > 0) return fail(Format("the fixture's footprint has %d map-water columns -- move streamPool.siteX/Z", wet));
  const int drop = (chanLen - 1) / chanRun;
  // As LOW as the ground allows: the structure is cut into the hill (every
  // footprint cell is written, stone or air), its basin floor just over the
  // lowest ground. NOT stood on the highest ground: the first runs put the
  // reservoir at y246, over this map's snowline (terrain.treeline 228), and the
  // heat layer froze the water -- the channel iced shut and the pour "lost"
  // 7,584 eighths to ice the census did not count.
  const int yBf = gMin + 2;                              // basin floor (stone top)
  const int yRf = yBf + basinDepth + drop;               // reservoir floor (stone top)
  const int wallTop = yRf + resH + 2;
  const int yBase = gMin - 3;
  const int snowline = worldmap::CurrentWorldMap().terrain.treeline - 1;
  if (wallTop + 6 >= snowline)
    return fail(Format("the fixture's top (y%d) reaches the map's snowline (y%d): its water would freeze -- lower "
                       "streamPool.resH / basinDepth / chanLen or move siteX/Z to lower ground",
                       wallTop + 6, snowline));
  const IVec3 org = c.world.WindowOrigin();
  const int wx0 = org.x * static_cast<int>(kChunk), wy0 = org.y * static_cast<int>(kChunk),
            wz0 = org.z * static_cast<int>(kChunk), W = static_cast<int>(kWorldN);
  if (xBeg - 8 < wx0 || xEnd + 8 >= wx0 + W || zBeg - 8 < wz0 || zEnd + 8 >= wz0 + W || yBase < wy0 ||
      wallTop + 12 >= wy0 + W)
    return fail(Format("the fixture (%d..%d, %d..%d, %d..%d) is not inside the window at (%d,%d,%d)", xBeg, xEnd, yBase,
                       wallTop, zBeg, zEnd, wx0, wy0, wz0));
  auto floorAt = [&](int k) { return yRf - k / chanRun; };
  // Kind of a footprint column: 0 outside, 1 wall, 2 reservoir, 3 gate, 4 channel, 5 basin.
  auto kindAt = [&](int x, int z) -> int {
    if (x >= xr0 && x < xg0 && z >= zr0 && z <= zr1) return 2;
    if (x >= xg0 && x < xc0 && z >= zch0 && z <= zch1) return 3;
    if (x >= xc0 && x < xb0 && z >= zch0 && z <= zch1) return 4;
    if (x >= xb0 && x < xb0 + basinX && z >= zb0 && z <= zb1) return 5;
    // walls: within 2 of any interior
    for (int dz = -2; dz <= 2; dz++)
      for (int dx = -2; dx <= 2; dx++) {
        const int xx = x + dx, zz = z + dz;
        if ((xx >= xr0 && xx < xg0 && zz >= zr0 && zz <= zr1) || (xx >= xg0 && xx < xb0 && zz >= zch0 && zz <= zch1) ||
            (xx >= xb0 && xx < xb0 + basinX && zz >= zb0 && zz <= zb1))
          return 1;
      }
    return 0;
  };
  const uint32_t stoneW = kMatStone, waterW = (mWater & 0xFFFu) | (7u << 12);

  // ---- the audit box and the chunk sets ----
  const int ax0 = xBeg - 8, ax1 = xEnd + 8, az0 = zBeg - 8, az1 = zEnd + 8, ay0 = yBase, ay1 = wallTop + 10;
  std::vector<IVec3> boxChunks;
  for (int cz = az0 >> 4; cz <= (az1 >> 4); cz++)
    for (int cy = ay0 >> 4; cy <= (ay1 >> 4); cy++)
      for (int cx = ax0 >> 4; cx <= (ax1 >> 4); cx++) boxChunks.push_back({cx, cy, cz});
  // HEAD: every chunk over x < xc0 + headCols; MOUTH: x >= xb0 - mouthCols;
  // MIDDLE: the chunks holding the channel's water band (floor .. floor + 3)
  // between them that are neither.
  std::set<uint32_t> headSet, mouthSet, middleSet, boxSet;
  for (const IVec3& ch : boxChunks) {
    const uint32_t s = World::SlotChunkIndex(ch);
    boxSet.insert(s);
    if (ch.x * 16 < xc0 + headCols) headSet.insert(s);
    if (ch.x * 16 + 15 >= xb0 - mouthCols) mouthSet.insert(s);
  }
  for (int k = headCols; k < chanLen - mouthCols; k++)
    for (int z = zch0; z <= zch1; z++)
      for (int y = floorAt(k) + 1; y <= floorAt(k) + 3; y++) {
        const uint32_t s = World::SlotChunkIndex({(xc0 + k) >> 4, y >> 4, z >> 4});
        if (!headSet.count(s) && !mouthSet.count(s)) middleSet.insert(s);
      }

  // ---- tuning + weather pins ----
  const Tuning saved = CurrentTuning();
  const std::string prevPin = weather::Override();
  auto restore = [&] {
    SetCurrentTuning(saved);
    weather::SetOverride(prevPin);
  };
  auto pinTuning = [&](int exciteMode, int relax) {
    Tuning dawn = saved;
    dawn.dayNight.freeze = 1;
    dawn.dayNight.freezePhase = static_cast<int>(kDaySunrise + 1024u);
    if (exciteMode >= 0) dawn.sim.fluidExciteMode = exciteMode;
    // WATER-BODY DISCOVERY OFF (sim.waterDiscoverMinEighths 0, its own off
    // switch). A sealed reservoir is exactly what W-D adopts as a body, and a
    // draining body's DISCHARGE (DESIGN.md 5b.5) throws part of the drain out
    // as MPM particles: measured, the first diagnosed run put 1,536 eighths in
    // particles and, in the determinism arm, 3,535 eighths of water outside the
    // structure. That is the body system's behaviour, gated by its own gates;
    // this gate's claims are the conduit's.
    dawn.sim.waterDiscoverMinEighths = 0;
    // THE CONDUIT'S ROOTING is off by default since 2026-10-17 (the owner's
    // rule: only streams fed from the map's water are held); this gate is the
    // conduit's, so it turns it on.
    dawn.sim.streamPoolRoot = 1;
    if (relax >= 0) dawn.sim.stillWaterRelax = relax;
    SetCurrentTuning(dawn);
  };
  weather::SetOverride("clear");

  // ---- build ops: top down (nothing unwritten can fall into what is written) ----
  std::vector<CellOp> build;
  for (int y = wallTop + 6; y >= yBase; y--)
    for (int z = zBeg; z <= zEnd; z++)
      for (int x = xBeg; x <= xEnd; x++) {
        const int kd = kindAt(x, z);
        if (kd == 0) continue;
        int top = wallTop;   // stone up to here
        if (kd == 2) top = yRf;
        else if (kd == 4) top = floorAt(x - xc0);
        else if (kd == 5) top = yBf;
        // the gate stays a wall until it opens
        build.push_back({World::SlotCellIndex({x, y, z}), y <= top ? stoneW : 0u});
      }
  std::vector<CellOp> fill, open;
  for (int y = yRf + 1; y <= yRf + resH; y++)
    for (int z = zr0; z <= zr1; z++)
      for (int x = xr0; x < xg0; x++) fill.push_back({World::SlotCellIndex({x, y, z}), waterW});
  for (int y = yRf + 1; y <= wallTop + 6; y++)
    for (int z = zch0; z <= zch1; z++)
      for (int x = xg0; x < xc0; x++) open.push_back({World::SlotCellIndex({x, y, z}), 0u});

  // ---- the readers ----
  const int mm = static_cast<int>(kNChunk) - 1;
  struct Census {
    long long res = 0, chan = 0, basin = 0, other = 0, fluid = 0;
    int32_t ledger = 0, snap = 0;
    uint32_t dep = 0, wd = 0;   // the ledger's diagnostic totals: eighths deposited / withdrawn
    int adoptPool = 0, adoptMap = 0, resAdopt = 0, poolAny = 0, chanCols = 0;
    int tOver = 0, tAt = 0, tUnder = 0, tDry = 0;   // adopted columns' water vs target
    uint32_t floodMax = 0, midWhy = 0;
    int head = 0, middle = 0, mouth = 0, box = 0;
    std::map<uint32_t, long long> mats;   // voxel count per material over the box
    std::string lane;                     // the channel's centre lane (see census)
    // WATER BODIES over the box (sim/waterbody.h): the reservoir is exactly
    // the kind of still pool the body discovery adopts, and a body's drain
    // keeps mass outside the grid (its debit) -- reported so a mass gap can be
    // put on it or ruled out.
    int bodies = 0;
    long long bodyVol = 0, bodyDebit = 0;
    uint64_t hash = 0, hashVox = 0, hashStill = 0;   // all, the box's voxels, its still words
    long long Total() const { return res + chan + basin + other + fluid + ledger; }
  };
  const rhi::Buffer& still = c.sim.StillWaterBuffer();
  auto census = [&](bool wantHash) -> Census {
    c.ctx.WaitIdle();
    Census s;
    uint64_t h = 1469598103934665603ull, hs = h;
    std::vector<uint32_t> cbuf(kChunkVol);
    std::map<std::pair<int, int>, int> colTop;   // each column's highest water surface, 8 y + f
    for (const IVec3& ch : boxChunks) {
      ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex(ch), 1, cbuf.data(), "streamPoolVox");
      for (uint32_t k = 0; k < kChunkVol; k++) {
        const uint32_t w = cbuf[k];
        if (wantHash) h = (h ^ (w & ~0x00FF0000u)) * 1099511628211ull;
        const uint32_t mid = w & 0xFFFu;
        if (mid != 0u) s.mats[mid]++;
        if (mid != mWater) continue;
        const int x = static_cast<int>(k % 16) + ch.x * 16, y = static_cast<int>((k / 16) % 16) + ch.y * 16,
                  z = static_cast<int>(k / 256) + ch.z * 16;
        if (x < ax0 || x > ax1 || z < az0 || z > az1 || y < ay0 || y > ay1) continue;
        const long long e = ((w >> 12) & 15u) + 1;
        const int kd = kindAt(x, z);
        if (kd == 2) s.res += e;
        else if (kd == 3 || kd == 4) s.chan += e;
        else if (kd == 5) s.basin += e;
        else s.other += e;
        int& top = colTop.try_emplace({x, z}, INT_MIN).first->second;
        top = std::max(top, 8 * y + static_cast<int>(e));
      }
    }
    s.fluid = c.world.Snap().fluidLive;
    std::vector<uint32_t> tab(size_t(kWorldN) * kWorldN * 4u + 4u);
    rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, still, 0, tab.data(), tab.size() * 4, "streamPoolStill");
    const size_t L = size_t(kWorldN) * kWorldN * 4u;
    s.ledger = static_cast<int32_t>(tab[L]);
    s.snap = static_cast<int32_t>(tab[L + 1]);
    s.dep = tab[L + 2];
    s.wd = tab[L + 3];
    for (int z = zBeg; z <= zEnd; z++)
      for (int x = xBeg; x <= xEnd; x++) {
        const size_t si = (size_t(z & int(kWorldN - 1)) * kWorldN + size_t(x & int(kWorldN - 1))) * 4u;
        if (wantHash) hs = (hs ^ tab[si + 2]) * 1099511628211ull, hs = (hs ^ tab[si + 3]) * 1099511628211ull;
        const int kd = kindAt(x, z);
        if (kd == 4) s.chanCols++;
        if (tab[si + 2] == 0u) continue;
        const bool pool = (tab[si + 3] & (1u << 24)) != 0u;
        if (pool) s.poolAny++;
        s.floodMax = std::max(s.floodMax, (tab[si + 3] >> 12) & 4095u);
        if (kd == 4) (pool ? s.adoptPool : s.adoptMap)++;
        else if (kd == 2 || kd == 3) s.resAdopt++;
        // Where each adopted column's water stands against its target.
        const int t8 = static_cast<int>(tab[si + 2] << 12) >> 12;
        auto it = colTop.find({x, z});
        const int top = it == colTop.end() ? INT_MIN : it->second;
        (top == INT_MIN ? s.tDry : top > t8 ? s.tOver : top < t8 ? s.tUnder : s.tAt)++;
      }
    // The channel's centre lane, gate to mouth, one character a column: '.'
    // dry, '~' water not adopted, '=' / '^' / 'v' a POOL column at / over /
    // under its target, 'M' a map-rooted one.
    for (int x = xg0; x < xb0; x++) {
      const size_t si = (size_t(zc & int(kWorldN - 1)) * kWorldN + size_t(x & int(kWorldN - 1))) * 4u;
      auto it = colTop.find({x, zc});
      const int top = it == colTop.end() ? INT_MIN : it->second;
      if (tab[si + 2] == 0u) { s.lane += top == INT_MIN ? '.' : '~'; continue; }
      if ((tab[si + 3] & (1u << 24)) == 0u) { s.lane += 'M'; continue; }
      const int t8 = static_cast<int>(tab[si + 2] << 12) >> 12;
      s.lane += top == INT_MIN ? '.' : top > t8 ? '^' : top < t8 ? 'v' : '=';
    }
    s.hashVox = h;
    s.hashStill = hs;
    s.hash = ((h ^ hs) * 1099511628211ull ^ tab[L]) * 1099511628211ull;
    for (const auto* list : {&sandvox::WaterBodies().Bodies(), &sandvox::WaterBodies().Children()})
      for (const sandvox::WaterBodyDesc& d : *list) {
        bool in = false;
        for (uint32_t sl : d.chunks) in = in || boxSet.count(sl) != 0;
        if (!in) continue;
        s.bodies++;
        s.bodyVol += static_cast<long long>(d.volumeEighths);
        s.bodyDebit += d.debitEighths;
      }
    std::vector<uint32_t> fl(kNumSlots, 0);
    rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.sim.DirtyActive(), 0, fl.data(), kNumSlots * 4, "streamPoolAwake");
    for (uint32_t sl : boxSet)
      if (sl < kNumSlots && fl[sl] != 0u) {
        s.box++;
        if (headSet.count(sl)) s.head++;
        if (mouthSet.count(sl)) s.mouth++;
        if (middleSet.count(sl)) {
          s.middle++;
          s.midWhy |= fl[sl];
        }
      }
    return s;
  };
  auto reasons = [](uint32_t bits) {
    std::string out;
    for (int i = 0; i < kDirtyReasonBits; i++)
      if ((bits >> i) & 1u) out += (out.empty() ? "" : "+") + std::string(kDirtyReasonName[i]);
    return out;
  };
  auto nameOf = [&](uint32_t id) { return id < c.mats.size() ? c.mats[id].name : Format("#%u", id); };
  // The materials whose voxel count in the box moved most against `ref`
  // (water included): where water that left the census went (ice, steam ...).
  auto matDelta = [&](const Census& ref, const Census& s) {
    std::map<uint32_t, long long> d;
    for (const auto& [id, n] : s.mats) d[id] += n;
    for (const auto& [id, n] : ref.mats) d[id] -= n;
    std::vector<std::pair<long long, uint32_t>> v;
    for (const auto& [id, n] : d)
      if (n != 0) v.push_back({-std::llabs(n), id});
    std::sort(v.begin(), v.end());
    std::string out;
    for (size_t i = 0; i < v.size() && i < 4; i++)
      out += Format(" %s%+lld", nameOf(v[i].second).c_str(), d[v[i].second]);
    return out;
  };
  auto line = [&](int tick, const Census& s) {
    return Format(" [%d: res %lld chan %lld basin %lld else %lld mpm %lld | ledger %d snap %d (in %u out %u) | adopted %d "
                  "pool + %d map (+%d at the source; over %d at %d under %d dry %d) flood %u | awake head %d mid %d%s%s "
                  "mouth %d of %d | bodies %d vol %lld debit %lld]",
                  tick, s.res, s.chan, s.basin, s.other, s.fluid, s.ledger, s.snap, s.dep, s.wd, s.adoptPool, s.adoptMap,
                  s.resAdopt, s.tOver, s.tAt, s.tUnder, s.tDry, s.floodMax, s.head, s.middle, s.middle ? " " : "",
                  s.middle ? reasons(s.midWhy).c_str() : "", s.mouth, s.box, s.bodies, s.bodyVol, s.bodyDebit);
  };

  // Fresh world, the fixture built and filled, `settle` ticks; the gate not
  // yet open. Returns the census after.
  uint32_t t = 96000;
  const IVec3 fixture{(xc0 + chanLen / 2) >> 4, floorAt(chanLen / 2) >> 4, zc >> 4};
  auto setup = [&](int exciteMode, int relax = -1) -> Census {
    pinTuning(exciteMode, relax);
    c.stream.OnRegen();   // the `determinism` gate's regen: the streamer forgets the old world too
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    sandvox::support::TickCursor tick{c, t, fixture};
    const size_t per = 60000;
    for (size_t i = 0; i < build.size(); i += per) {
      sandvox::support::TickOps ops;
      ops.cells.assign(build.begin() + i, build.begin() + std::min(build.size(), i + per));
      tick(ops);
    }
    sandvox::support::TickOps f;
    f.cells = fill;
    tick(f);
    for (int k = 0; k < 60; k++) tick();
    return census(false);
  };

  // ---- the main arm ----
  // MPM excitation OFF, as in every CA liquid gate (ca-gutter): opening a
  // full-height gate on a deep reservoir excited ~9,400 eighths into
  // particles that never settled back (first run), and the claim here is the
  // CA conduit, not the splash.
  const Census before = setup(0);
  const long long poured = before.Total();
  const long long pourOps = static_cast<long long>(fill.size()) * 8;
  std::printf("stream-pool: fixture: reservoir %dx%dx%d at x%d..%d z%d..%d floor y%d; gate x%d..%d; channel %d x %d, "
              "floor y%d -> y%d (1 in %d); basin %dx%d floor y%d; ground y%d..%d; %zu build ops; poured %lld eighths "
              "(%lld by the ops); head x < %d, mouth x >= %d; %zu head / %zu middle / %zu mouth / %zu box chunks\n",
              resX, resZ, resH, xr0, xg0 - 1, zr0, zr1, yRf, xg0, xc0 - 1, chanLen, chanW, floorAt(0),
              floorAt(chanLen - 1), chanRun, basinX, basinZ, yBf, gMin, gMax, build.size(), poured, pourOps,
              xc0 + headCols, xb0 - mouthCols, headSet.size(), middleSet.size(), mouthSet.size(), boxSet.size());
  if (before.res < pourOps * 9 / 10) {
    restore();
    return fail(Format("the reservoir holds %lld of %lld poured eighths before it opens -- the fixture leaks", before.res,
                       pourOps));
  }
  {
    sandvox::support::TickCursor tick{c, t, fixture};
    sandvox::support::TickOps o;
    o.cells = open;
    tick(o);
    std::string curve;
    long long massErrMax = 0;
    int bestPool = 0, sleepRun = 0, sleepBest = 0, sleepAt = -1, emptyAt = -1, done = 1;
    long long prevRes = before.res;
    std::vector<long long> resHist;
    Census s;
    for (; done < maxTicks;) {
      for (int k = 0; k < sampleTicks; k++) tick();
      done += sampleTicks;
      s = census(false);
      curve += "\n " + line(done, s) + " materials vs the pour:" + matDelta(before, s) + "\n    lane |" + s.lane + "|";
      massErrMax = std::max(massErrMax, std::llabs(s.Total() - poured));
      bestPool = std::max(bestPool, s.adoptPool);
      const bool draining = s.res < prevRes;
      prevRes = s.res;
      if (draining && s.middle <= middleAwakeMax && s.adoptPool > 0) {
        if (++sleepRun > sleepBest) sleepBest = sleepRun;
        if (sleepRun >= middleSleepSamples && sleepAt < 0) sleepAt = done;
      } else {
        sleepRun = 0;
      }
      // SPENT: under emptyFrac of the pour, or STOPPED -- it lost under
      // spentFrac of the pour over the last two samples (the reservoir floor
      // is level with the channel's first tread, so a shallow pool stays
      // behind that no stream can carry).
      resHist.push_back(s.res);
      const size_t nh = resHist.size();
      const bool stopped = nh >= 3 && resHist[nh - 3] - s.res < static_cast<long long>(spentFrac * poured);
      if (emptyAt < 0 && (s.res < static_cast<long long>(emptyFrac * poured) || stopped)) emptyAt = done;
      if (emptyAt >= 0 && done - emptyAt >= settleTicks) break;
    }
    const Census end = s;
    std::printf("stream-pool: curve (tick: reservoir / channel / basin / elsewhere eighths, MPM particles | the ledger | "
                "adopted channel columns | awake chunks):%s\n",
                curve.c_str());
    const bool aOk = s.chanCols > 0 && bestPool >= static_cast<int>(adoptMin * s.chanCols);
    std::printf("stream-pool: A adopted: %s (at best %d of the channel's %d columns POOL-adopted, >= %.2f)\n",
                aOk ? "PASS" : "FAIL", bestPool, s.chanCols, adoptMin);
    if (!aOk) problems.push_back(Format("A: %d of %d channel columns adopted at best", bestPool, s.chanCols));
    const bool bOk = sleepAt >= 0;
    std::printf("stream-pool: B middle asleep while the source drains: %s (%d samples in a row with <= %d of the %zu "
                "middle chunks awake while the reservoir lost water, >= %d needed; first at tick %d)\n",
                bOk ? "PASS" : "FAIL", sleepBest, middleAwakeMax, middleSet.size(), middleSleepSamples, sleepAt);
    if (!bOk) problems.push_back(Format("B: the middle slept %d samples in a row while draining (need %d)", sleepBest,
                                        middleSleepSamples));
    const bool cOk = massErrMax <= massTol;
    std::printf("stream-pool: C mass: %s (voxels + MPM + ledger vs %lld poured: worst |error| %lld eighths <= %lld)\n",
                cOk ? "PASS" : "FAIL", poured, massErrMax, massTol);
    if (!cOk) problems.push_back(Format("C: mass off by up to %lld eighths of %lld", massErrMax, poured));
    const bool dOk = emptyAt >= 0 && end.poolAny == 0 && end.box <= endAwakeMax &&
                     std::llabs(static_cast<long long>(end.ledger)) <= strandMax;
    std::printf("stream-pool: D run out: %s (source spent (under %.2f of the pour, or stopped) at tick %d; %d ticks on: %d POOL columns "
                "left, %d fixture chunks awake (<= %d), ledger stranded %d (|.| <= %lld); window %u awake)\n",
                dOk ? "PASS" : "FAIL", emptyFrac, emptyAt, emptyAt >= 0 ? done - emptyAt : -1, end.poolAny, end.box,
                endAwakeMax, end.ledger, strandMax, sandvox::ReadActiveChunksSync(c.ctx, c.world, c.sim));
    if (!dOk)
      problems.push_back(Format("D: empty at %d, %d POOL columns left, %d awake, ledger %d", emptyAt, end.poolAny,
                                end.box, end.ledger));
    RecordObserved("streamPool.observedAdopted", static_cast<double>(bestPool));
    RecordObserved("streamPool.observedMassErr", static_cast<double>(massErrMax));
    RecordObserved("streamPool.observedStranded", static_cast<double>(end.ledger));
    RecordObserved("streamPool.observedEmptyAt", static_cast<double>(emptyAt));
  }

  // ---- E. determinism: THREE runs from a fresh worldgen at the same tick numbers ----
  // MPM excitation off: a particle still in flight at the end of one run would
  // otherwise be the next run's starting state. THREE, not two (memory note
  // gotcha-hash-identity-needs-three-arms): run 1 follows the long main arm,
  // runs 2 and 3 follow an identical E run each, so state that survives a
  // worldgen (the main arm's leftovers) shows as 1 != 2 == 3, and only 2 != 3
  // is a rule-1 failure. Every run hashes the fixture each sampleTicks, so a
  // divergence names its first sample.
  if (detTicks > 0) {
    // Runs 0..2: the conduit (as above). Runs 3..4: the CONTROL -- the same
    // build, open and ticks with sim.stillWaterRelax 0 (no held water, no
    // streams, no ledger: the CA alone). If the control pair differs too, what
    // differs is not the conduit's (state that survives a worldgen, or a
    // nondeterminism elsewhere), and the line says so.
    const uint32_t tE = t;
    std::vector<uint64_t> seq[5], seqV[5], seqS[5];
    Census last[5];
    for (int r = 0; r < 5; r++) {
      t = tE;
      setup(0, r < 3 ? -1 : 0);
      sandvox::support::TickCursor tick{c, t, fixture};
      sandvox::support::TickOps o;
      o.cells = open;
      tick(o);
      for (int k = 1; k < detTicks; k++) {
        tick();
        if ((k + 1) % sampleTicks == 0 || k + 1 == detTicks) {
          last[r] = census(true);
          seq[r].push_back(last[r].hash);
          seqV[r].push_back(last[r].hashVox);
          seqS[r].push_back(last[r].hashStill);
        }
      }
    }
    // The first sample at which runs a and b differ, and in what.
    auto firstDiff = [&](int a, int b, std::string& what) -> int {
      for (size_t i = 0; i < seq[a].size() && i < seq[b].size(); i++)
        if (seq[a][i] != seq[b][i]) {
          what = std::string(seqV[a][i] != seqV[b][i] ? " voxels" : "") + (seqS[a][i] != seqS[b][i] ? " still-words" : "") +
                 (seqV[a][i] == seqV[b][i] && seqS[a][i] == seqS[b][i] ? " ledger" : "");
          return static_cast<int>(std::min<size_t>((i + 1) * sampleTicks, detTicks));
        }
      return -1;
    };
    std::string w12, w23, wC;
    const int d12 = firstDiff(0, 1, w12), d23 = firstDiff(1, 2, w23), dC = firstDiff(3, 4, wC);
    // A FAIL is the conduit's only when the control pair agrees.
    const bool eOk = d23 < 0 || dC >= 0;
    std::printf("stream-pool: E determinism: %s (%d ticks after opening; conduit runs 1/2/3 %016llx / %016llx / %016llx: "
                "2 vs 3 (same pre-history) first differ at tick %d%s, 1 vs 2 at tick %d%s; CONTROL (stillWaterRelax 0) "
                "twice: %016llx / %016llx, first differ at tick %d%s%s)\n",
                d23 < 0 ? "PASS" : (dC >= 0 ? "INCONCLUSIVE" : "FAIL"), detTicks,
                static_cast<unsigned long long>(seq[0].back()), static_cast<unsigned long long>(seq[1].back()),
                static_cast<unsigned long long>(seq[2].back()), d23, w23.c_str(), d12, w12.c_str(),
                static_cast<unsigned long long>(seq[3].back()), static_cast<unsigned long long>(seq[4].back()), dC, wC.c_str(),
                d23 >= 0 && dC >= 0 ? " -- the control differs too: not the conduit's" : "");
    RecordObserved("streamPool.observedDetDiffTick", static_cast<double>(d23));
    RecordObserved("streamPool.observedDetControlDiffTick", static_cast<double>(dC));
    if (!eOk)
      problems.push_back(Format("E: two runs of the same pour differ from tick %d (%s) and the control does not -- RULE 1",
                                d23, w23.c_str()));
  }

  // ---- a fresh world back ----
  restore();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  DrainFullRefill(c.ctx, c.world, c.sim, IVec3{108 >> 4, 122 >> 4, 108 >> 4});
  if (!problems.empty()) {
    detail = problems[0];
    for (size_t i = 1; i < problems.size(); i++) detail += "; " + problems[i];
    std::printf("stream-pool: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  detail = "pool-fed stream adopted, middle asleep while it drains, mass exact, released when empty, twice-run identical";
  std::printf("stream-pool: PASS\n");
  return Status::Pass;
}

const std::vector<Gate>& StreamPoolGates() {
  static const std::vector<Gate> g = {
      {"stream-pool", "sim", {}, false, GateStreamPool},
  };
  return g;
}

}  // namespace selftest
