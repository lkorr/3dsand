// selftest_sim.cpp — sim selftest gates.
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
#include <tuple>
#include <vector>

#include "game/prefab.h"
#include "gpu/resources.h"
#include "test/selftest.h"
#include "test/support.h"

#include "net/chunksync.h"  // HashTree + Comparable/ChunkAuthority (chunk-resync)
#include "sim/oprecord.h"  // the op record + replay (ops-replay gate)
#include "sim/pagetable.h"
#include "sim/rng.h"       // rng::Pcg — the CPU twin of the digest fold
#include "sim/rng_simd.h"  // rng::Pcg8 / JitterStateInRow8 (the simd gate)
#include "sim/scan.h"      // scan::FirstIndexWhereMasked   (the simd gate)
#include "sim/weather.h"  // SetOverride / SimRainWord (rain-fire gate)
#include "sim/rainexpo.h"  // rain-lean: the fall-line lattice, CPU side
#include "sim/stream.h"  // RleEncodeChunk / RleEncodeSentinelChunk (fusion gate)

using namespace sandvox;

namespace selftest {
namespace {

// ---- determinism: DIVERGENCE ATTRIBUTION (SANDVOX_DET_PROBE=<t>[,<t>...]) --
// CLAUDE.md rule 6: "first divergence at tick 85" is a bare number. At each
// probe tick both runs capture the per-slot voxel digests, the whole solute
// meta record, every solute-carrying slot's cells and the voxel words of those
// slots and their 26-rings (plus any slot named in SANDVOX_DET_SLOTS), and run
// 2 prints WHICH slot differs and in WHICH field: voxel material / state /
// stain / stamp+excite, solute cell value, solute meta word. Off by default:
// it is a set of blocking readbacks at the probe ticks only.
struct DetProbe {
  std::vector<uint32_t> chunkHash, solMeta, solTable;
  std::map<uint32_t, std::vector<uint16_t>> solCells;  // slot -> 4096 values
  std::map<uint32_t, std::vector<uint32_t>> vox;       // slot -> 4096 words
  uint32_t gas[kGasSpHdr] = {};
};

static std::vector<uint32_t> DetProbeTicks(const char* env) {
  std::vector<uint32_t> v;
  const char* s = std::getenv(env);
  while (s && *s) {
    char* end = nullptr;
    const unsigned long t = std::strtoul(s, &end, 10);
    if (end == s) break;
    v.push_back((uint32_t)t);
    s = (*end == ',') ? end + 1 : end;
  }
  return v;
}

static void DetProbeCapture(GpuContext& ctx, World& world, DetProbe& p) {
  auto rb = [&](const rhi::Buffer& b, uint64_t off, void* dst, size_t n,
                const char* label) {
    rhi::ReadbackBlocking(ctx.device, ctx.queue, b, off, dst, n, label);
  };
  p.chunkHash.assign(kChunkHashWords, 0u);
  rb(world.chunkHash, 0, p.chunkHash.data(), kChunkHashBytes, "detChunkHash");
  p.solMeta.assign(kSolMetaWords, 0u);
  rb(world.solMeta, 0, p.solMeta.data(), (size_t)kSolMetaWords * 4, "detSolMeta");
  p.solTable.assign(kNumSlots, 0u);
  rb(world.solTable, 0, p.solTable.data(), (size_t)kNumSlots * 4, "detSolTable");
  std::vector<uint32_t> page(kSolWordsPerPage);
  std::vector<uint32_t> want;
  for (uint32_t s = 0; s < kNumSlots; s++) {
    const uint32_t e = p.solTable[s];
    if (e == 0u) continue;
    std::vector<uint16_t>& cells = p.solCells[s];
    cells.assign(kChunkVol, 0);
    if (e & kSolPageBit) {
      rb(world.solPool, (uint64_t)(e & kSolPageMask) * kSolWordsPerPage * 4,
         page.data(), (size_t)kSolWordsPerPage * 4, "detSolPage");
      for (uint32_t i = 0; i < kChunkVol; i++)
        cells[i] = (uint16_t)((page[i >> 1] >> ((i & 1u) * 16u)) & 0xFFFFu);
    } else {
      for (uint32_t i = 0; i < kChunkVol; i++) cells[i] = (uint16_t)(e & 0xFFFFu);
    }
    const IVec3 wc = world.SlotToWorldChunk(s);
    for (int dz = -1; dz <= 1; dz++)
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
          const IVec3 n{wc.x + dx, wc.y + dy, wc.z + dz};
          if (world.ChunkInWindow(n)) want.push_back(World::SlotChunkIndex(n));
        }
  }
  for (uint32_t s : DetProbeTicks("SANDVOX_DET_SLOTS"))
    if (s < kNumSlots) want.push_back(s);
  std::sort(want.begin(), want.end());
  want.erase(std::unique(want.begin(), want.end()), want.end());
  if (want.size() > 1024) want.resize(1024);
  for (uint32_t s : want) {
    std::vector<uint32_t>& w = p.vox[s];
    w.assign(kChunkVol, 0u);
    ReadVoxelsSync(ctx, world, s, 1, w.data(), "detVox");
  }
  ReadGasStatsSync(ctx, world, p.gas);
}

static void DetProbeCompare(World& world, uint32_t tick, const DetProbe& a,
                            const DetProbe& b) {
  auto wcs = [&](uint32_t s) {
    const IVec3 c = world.SlotToWorldChunk(s);
    return Format("slot %u (%d,%d,%d)", s, c.x, c.y, c.z);
  };
  std::printf("det-probe tick %u:\n", tick);
  // Voxel digests (sim_occupancy's per-slot chunkHash: material+state+stain).
  uint32_t nVox = 0;
  for (uint32_t s = 0; s < kNumSlots; s++) {
    if (a.chunkHash[s] == b.chunkHash[s]) continue;
    if (nVox++ < 16)
      std::printf("  voxel digest differs: %s  %08x vs %08x\n", wcs(s).c_str(),
                  a.chunkHash[s], b.chunkHash[s]);
  }
  std::printf("  voxel digests: %u slots differ (table tick %u vs %u)\n", nVox,
              a.chunkHash[kNumSlots], b.chunkHash[kNumSlots]);
  // Solute meta header: the counters and the free depth.
  for (uint32_t w = 0; w < kSolMetaHdrWords; w++)
    if (a.solMeta[w] != b.solMeta[w])
      std::printf("  solMeta hdr[%u]: %u vs %u\n", w, a.solMeta[w], b.solMeta[w]);
  struct Region { const char* name; uint32_t base, n; };
  const Region regs[] = {{"wantFlag", kSolMWantFlag, kNumSlots},
                         {"reqFlag", kSolMReqFlag, kNumSlots},
                         {"stall", kSolMStall, kNumSlots},
                         {"agg", kSolMAgg, kNumSlots}};
  for (const Region& r : regs) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < r.n; i++) {
      if (a.solMeta[r.base + i] == b.solMeta[r.base + i]) continue;
      if (n++ < 8)
        std::printf("  solMeta %s: %s  %08x vs %08x\n", r.name, wcs(i).c_str(),
                    a.solMeta[r.base + i], b.solMeta[r.base + i]);
    }
    if (n) std::printf("  solMeta %s: %u slots differ\n", r.name, n);
  }
  // Solute cells, by CONTENT (the page index is an address, not state).
  std::vector<uint32_t> slots;
  for (auto& kv : a.solCells) slots.push_back(kv.first);
  for (auto& kv : b.solCells) slots.push_back(kv.first);
  std::sort(slots.begin(), slots.end());
  slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
  static const std::vector<uint16_t> kNone(kChunkVol, 0);
  uint32_t solDiffSlots = 0;
  for (uint32_t s : slots) {
    auto ia = a.solCells.find(s), ib = b.solCells.find(s);
    const std::vector<uint16_t>& ca = ia != a.solCells.end() ? ia->second : kNone;
    const std::vector<uint16_t>& cb = ib != b.solCells.end() ? ib->second : kNone;
    uint32_t n = 0, first = 0;
    for (uint32_t i = 0; i < kChunkVol; i++)
      if (ca[i] != cb[i]) { if (!n) first = i; n++; }
    const bool kindDiff = ((a.solTable[s] & kSolPageBit) != 0) !=
                          ((b.solTable[s] & kSolPageBit) != 0);
    if (!n && !kindDiff) continue;
    if (solDiffSlots++ < 16)
      std::printf("  solute differs: %s  %u cells (first local %u: %04x vs %04x)"
                  " entry %08x vs %08x\n",
                  wcs(s).c_str(), n, first, ca[first], cb[first], a.solTable[s],
                  b.solTable[s]);
  }
  std::printf("  solute: %u slots differ (%zu vs %zu carrying)\n", solDiffSlots,
              a.solCells.size(), b.solCells.size());
  // Voxel words, by field, where both runs captured the slot.
  uint32_t vShown = 0;
  for (auto& kv : a.vox) {
    auto ib = b.vox.find(kv.first);
    if (ib == b.vox.end()) continue;
    uint32_t nm = 0, ns = 0, nst = 0, nstamp = 0, first = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < kChunkVol; i++) {
      const uint32_t x = kv.second[i], y = ib->second[i];
      if (x == y) continue;
      if (first == 0xFFFFFFFFu) first = i;
      if ((x & 0xFFFu) != (y & 0xFFFu)) nm++;
      if (((x >> 12) & 0xFu) != ((y >> 12) & 0xFu)) ns++;
      if ((x & 0x7F000000u) != (y & 0x7F000000u)) nst++;
      if ((x & 0x00FF0000u) != (y & 0x00FF0000u)) nstamp++;
    }
    if (first == 0xFFFFFFFFu) continue;
    if (vShown++ < 16)
      std::printf("  voxels differ: %s  material %u, state %u, stain %u, "
                  "stamp/excite %u (first local %u: %08x vs %08x)\n",
                  wcs(kv.first).c_str(), nm, ns, nst, nstamp, first,
                  kv.second[first], ib->second[first]);
  }
  std::printf("  voxel words: %u of %zu captured slots differ\n", vShown,
              a.vox.size());
  for (uint32_t i = 0; i < kGasSpHdr; i++)
    if (a.gas[i] != b.gas[i])
      std::printf("  gas stat[%u]: %u vs %u\n", i, a.gas[i], b.gas[i]);
}

// Cross-BOOT attribution: SANDVOX_DET_DUMP=<prefix> writes run 1's probes (and
// its per-tick hashes) to files; SANDVOX_DET_REF=<prefix> reads a previous
// boot's and compares run 1 against them. Two boots that end on different
// hashes are then diffed exactly like the two runs of one boot.
static void DetProbeSave(const std::string& path, const DetProbe& p) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  auto vec = [&](const std::vector<uint32_t>& v) {
    const uint32_t n = (uint32_t)v.size();
    std::fwrite(&n, 4, 1, f);
    std::fwrite(v.data(), 4, n, f);
  };
  vec(p.chunkHash); vec(p.solMeta); vec(p.solTable);
  uint32_t n = (uint32_t)p.solCells.size();
  std::fwrite(&n, 4, 1, f);
  for (auto& kv : p.solCells) {
    std::fwrite(&kv.first, 4, 1, f);
    std::fwrite(kv.second.data(), 2, kChunkVol, f);
  }
  n = (uint32_t)p.vox.size();
  std::fwrite(&n, 4, 1, f);
  for (auto& kv : p.vox) {
    std::fwrite(&kv.first, 4, 1, f);
    std::fwrite(kv.second.data(), 4, kChunkVol, f);
  }
  std::fwrite(p.gas, 4, kGasSpHdr, f);
  std::fclose(f);
}

static bool DetProbeLoad(const std::string& path, DetProbe& p) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  bool ok = true;
  auto rd = [&](void* d, size_t sz, size_t n) {
    if (std::fread(d, sz, n, f) != n) ok = false;
  };
  auto vec = [&](std::vector<uint32_t>& v) {
    uint32_t n = 0;
    rd(&n, 4, 1);
    v.assign(n, 0u);
    if (n) rd(v.data(), 4, n);
  };
  vec(p.chunkHash); vec(p.solMeta); vec(p.solTable);
  uint32_t n = 0;
  rd(&n, 4, 1);
  for (uint32_t i = 0; i < n && ok; i++) {
    uint32_t s = 0;
    rd(&s, 4, 1);
    std::vector<uint16_t>& c = p.solCells[s];
    c.assign(kChunkVol, 0);
    rd(c.data(), 2, kChunkVol);
  }
  n = 0;
  rd(&n, 4, 1);
  for (uint32_t i = 0; i < n && ok; i++) {
    uint32_t s = 0;
    rd(&s, 4, 1);
    std::vector<uint32_t>& w = p.vox[s];
    w.assign(kChunkVol, 0u);
    rd(w.data(), 4, kChunkVol);
  }
  rd(p.gas, 4, kGasSpHdr);
  std::fclose(f);
  return ok;
}

// ---- determinism -------------------------------------------------------
Status GateDeterminism(Ctx& c, std::string& detail) {
  constexpr int kTicks = 200;
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

// determinism: two identical runs must produce identical hash sequences
std::vector<uint32_t> hashes[2];
// ---- GAS PARTICLES ARE NOT IN THE WORLD HASH, AND CANNOT BE ---------------
// A parcel outside the residency window touches no voxel, so the hash the
// occupancy pass folds cannot see it at all. It becomes visible only when it
// re-enters and lands, which is potentially a hundred ticks after the motion
// that decided where. That is a real hole: a scheduling-dependent gas step
// out there would reproduce a matching hash sequence for the whole run.
//
// So the gas population carries its OWN digest (kGasSpDigest) — the sum of
// every surviving parcel's particlePriority, order-independent because the
// pool's append order is not. Read ONCE per run rather than per tick: it is a
// blocking readback, the failure it catches is persistent (a divergence does
// not heal), and 400 extra stalls for a claim two make is exactly the
// verification budget CLAUDE.md is about.
uint32_t gasDigest[2] = {}, gasLiveEnd[2] = {};
const std::vector<uint32_t> probeTicks = DetProbeTicks("SANDVOX_DET_PROBE");
std::map<uint32_t, DetProbe> probe[2];
for (int run = 0; run < 2; run++) {
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  for (uint32_t t = 1; t <= kTicks; t++) {
    SubmitTick(ctx, world, sim, t, kDefaultSeed, SelftestOps(t, kDefaultSeed),
               SelftestExps(t, kDefaultSeed), {}, true, {8, 3, 8}, false,
               SelftestParticlesActive(t));
    hashes[run].push_back(ReadHashSync(ctx, world));
    if (std::find(probeTicks.begin(), probeTicks.end(), t) != probeTicks.end()) {
      DetProbeCapture(ctx, world, probe[run][t]);
      if (run == 1) DetProbeCompare(world, t, probe[0][t], probe[1][t]);
      if (run == 0) {
        const char* dump = std::getenv("SANDVOX_DET_DUMP");
        const char* ref = std::getenv("SANDVOX_DET_REF");
        if (dump && *dump)
          DetProbeSave(Format("%s.t%u.bin", dump, t), probe[0][t]);
        DetProbe rp;
        if (ref && *ref && DetProbeLoad(Format("%s.t%u.bin", ref, t), rp)) {
          std::printf("det-probe vs REFERENCE BOOT (%s):\n", ref);
          DetProbeCompare(world, t, rp, probe[0][t]);
        }
      }
    }
  }
  if (run == 0) {
    if (const char* dump = std::getenv("SANDVOX_DET_DUMP"); dump && *dump) {
      if (FILE* f = std::fopen(Format("%s.hashes.txt", dump).c_str(), "w")) {
        for (uint32_t h : hashes[0]) std::fprintf(f, "%08x\n", h);
        std::fclose(f);
      }
    }
    if (const char* ref = std::getenv("SANDVOX_DET_REF"); ref && *ref) {
      if (FILE* f = std::fopen(Format("%s.hashes.txt", ref).c_str(), "r")) {
        unsigned h = 0;
        int i = 0;
        while (i < kTicks && std::fscanf(f, "%x", &h) == 1) {
          if (h != hashes[0][i]) {
            std::printf("det-probe: first divergence FROM THE REFERENCE BOOT at "
                        "tick %d: %08x (ref) vs %08x\n", i + 1, h, hashes[0][i]);
            break;
          }
          i++;
        }
        std::fclose(f);
      }
    }
  }
  uint32_t gs[kGasSpHdr] = {};
  ReadGasStatsSync(ctx, world, gs);
  gasDigest[run] = gs[kGasSpDigest];
  gasLiveEnd[run] = GasAliveSync(ctx, world, sim);
}
const bool gasSame =
    gasDigest[0] == gasDigest[1] && gasLiveEnd[0] == gasLiveEnd[1];
bool deterministic = hashes[0] == hashes[1] && gasSame;
std::printf("determinism: gas %u parcels alive, digest %08x (%s)\n",
            gasLiveEnd[0], gasDigest[0],
            gasSame ? "reproduced" : "DIVERGED between the two runs");

// ---- THE TWO CHECKS ARE DIFFERENT CLAIMS. DO NOT CONFLATE THEM. ------------
//
// `deterministic` above is THE INVARIANT (CLAUDE.md rule 1): the same
// seed+tick+inputs reproduce bit-identically. If that fails, something is
// scheduling-dependent and the sim is broken. Stop and report.
//
// `goldenOk` below is a CHANGE DETECTOR, and nothing more. Twice-run equality
// proves the sim reproduces itself; it does NOT prove it still simulates the
// same world. A change that quietly makes the sim do less stays perfectly
// self-consistent and sails through — the phase-2b Vulkan-port work found
// exactly that, a build where the mutate and explode passes dispatched ZERO
// workgroups with the full suite green. Pinning the final hash is what converts
// "the sim agrees with itself" into "the sim agrees with what we recorded".
//
// A MOVED PIN IS NOT A BROKEN SIM. Every intentional change to hashed state
// moves it — a reaction chance, a material, a sim.* value, a re-baked asset.
// The correct response to an EXPECTED move is `--selftest --rebaseline` in the
// same commit, and no investigation whatsoever: there is nothing to diagnose,
// and treating the old number as a target to restore is how a five-minute data
// change turns into an afternoon. Only an UNEXPECTED move is information.
//
// An absent key means "not pinned" and only reports — a checkout predating the
// key still behaves as before. See tests/BASELINE.md.
char got[16];
std::snprintf(got, sizeof(got), "%08x", hashes[0].back());
const std::string& golden = GoldenDeterminismHash();
bool goldenOk = golden.empty() || golden == got;

// The status word names WHICH claim failed, because they mean opposite things:
// "determinism FAILED" is a broken sim, "pin moved" is usually just a change
// that has not been rebaselined yet.
std::printf("determinism: %s (final hash %s over %d ticks%s)\n",
            (deterministic && goldenOk) ? "PASS"
            : !deterministic            ? "FAIL"
                                        : "PIN MOVED",
            got, kTicks,
            golden.empty() ? ", not pinned"
            : goldenOk     ? ", matches baseline"
                           : ", sim reproduces itself; only the recorded value "
                             "differs - rebaseline if you meant it");
if (!deterministic) {
  for (int i = 0; i < kTicks; i++) {
    if (hashes[0][i] != hashes[1][i]) {
      std::printf("  first divergence at tick %d: %08x vs %08x\n", i + 1,
                  hashes[0][i], hashes[1][i]);
      break;
    }
  }
}
if (!goldenOk) {
  std::printf(
      "  PINNED HASH MOVED: baseline says %s, this build produced %s.\n"
      "  THE SIM IS NOT BROKEN. Determinism itself PASSED (two runs of the\n"
      "  same seed agreed bit for bit) — this line only says the world you\n"
      "  simulate differs from the one last recorded.\n"
      "    * Did you MEAN to change hashed state (a material, a reaction\n"
      "      chance, a sim.* value, a re-baked asset)? Then this is expected.\n"
      "      Run --selftest --rebaseline, commit the new value alongside your\n"
      "      change, and move on. Do NOT investigate it and do NOT try to get\n"
      "      %s back; there is nothing here to diagnose.\n"
      "    * Did you NOT expect it? Then this is the finding: something\n"
      "      changed behaviour that you did not intend. See tests/BASELINE.md\n"
      "      and the escalation ladder in CLAUDE.md.\n",
      golden.c_str(), got, golden.c_str());
}

  // Says PIN MOVED, not MISMATCH: this string is what lands in last_run.json
  // and in the regression summary, and "mismatch" reads as a broken sim when
  // the sim in fact reproduced itself perfectly two lines above.
  std::string goldenNote = golden.empty() ? ", golden hash not pinned"
                           : goldenOk     ? ", matches golden"
                                          : ", PIN MOVED from baseline " +
                                                golden +
                                                " (determinism itself passed; "
                                                "rebaseline if intended)";
  detail = Format("final hash %s over %d ticks%s", got, kTicks,
                  goldenNote.c_str());

  // Verdict: self-consistent AND simulating the recorded world.
  //
  // Those two are different failures and only one of them is rebaselinable.
  // `deterministic` is the twice-run comparison -- the invariant this gate
  // exists to protect, and never something a baseline edit may excuse.
  // `goldenOk` is a PINNED VALUE. A run that is self-consistent but simulates a
  // different world than the one recorded is exactly the case --rebaseline is
  // for, so say so rather than reporting an undifferentiated FAIL that the
  // rebaseline path then refuses to act on.
  if (deterministic && !goldenOk) MarkPinnedOnly();
  return (deterministic && goldenOk) ? Status::Pass : Status::Fail;
}

// ---- sleep -------------------------------------------------------------
Status GateSleep(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const std::vector<MaterialDef>& mats = c.mats;
// sleep: a settled world must go (nearly) fully idle — the M2 exit
// criterion, and the guard against reaction rules that never stop matching.
// Includes an explosion: every ejected particle must reinsert and die.
uint32_t sleepActive = 0;
uint32_t particlesLeft = 0;
uint32_t faLive1 = 0;  // MLS-MPM particles still alive; read below, verdict at the end
uint32_t particlesEnd = 0;  // ejecta re-read AFTER the quiet window (see below)
int settled = 0;  // tick at which the world went quiet (or the cap)
{
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  uint32_t t = 0;
  // Settle budget is ADAPTIVE: 500 fixed ticks was tuned for the 256^3
  // window; the 512^3 window holds 8x the content (every pond and lava
  // pocket in 32 m of world), and freshly generated liquid legitimately
  // takes longer to equalize. Tick until the world is quiet, hard-capped —
  // the cap is what still catches never-sleeping content (rule 2).
  uint32_t quiet = kNumSlots;
  for (int i = 0; i < 3000; i++) {
    std::vector<ExplosionOp> exps;
    if (i == 30) exps.push_back({110, 76, 110, 12, 350, 0, 0, 0});  // wood slab
    // A FIXTURE THAT OUTLASTS WHAT IT MEASURES. This used to close at i < 460,
    // which was fine while the world settled in ~500 ticks: the explosion at 30
    // threw ejecta, the window covered their flight, and they were all dead
    // before it shut. The MPM seam now sheds splash droplets throughout
    // settling, and settling can run to the 3000-tick cap — so droplets emitted
    // after 460 were never stepped again and therefore could never die. The
    // gate reported 262,144 particles alive (exactly kParticleCap) and blamed
    // the engine for a pool the FIXTURE had frozen.
    //
    // Still a pure function of the tick, which is the determinism requirement
    // this flag carries (see SelftestParticlesActive and the note in
    // simulation.h): the caller must derive it only from tick-deterministic
    // inputs, and "have we reached tick 30" is one.
    bool pactive = i >= 30;
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, exps, {}, false, {8, 3, 8},
               false, pactive);
    if (i >= 500 && i % 100 == 0) {
      ctx.WaitIdle();
      quiet = ReadActiveChunksSync(ctx, world, sim);
      settled = i;
      if (quiet < 32) break;
    }
  }
  ctx.WaitIdle();
  uint32_t counts[2] = {};
  ReadCountsSync(ctx, world, counts);
  particlesLeft = std::min(counts[sim.Page()], kParticleCap);
  double s0 = NowSeconds();  // settled-world cost: the whole point of dirty dispatch
  for (int i = 0; i < 100; i++)
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false, {8, 3, 8},
               false, false);
  ctx.WaitIdle();
  std::printf("sim settled: %.3f ms/tick\n", (NowSeconds() - s0) * 1000.0 / 100.0);

  // ---- THE MPM HALF OF "SETTLED", WHICH THIS GATE NEVER LOOKED AT --------
  //
  // `particlesLeft` above reads world.particleCounts — the EJECTA/debris
  // system. The MLS-MPM water particles live in world.fluidArgsStage and had
  // no assertion anywhere in the suite, which is how a lake could hold ~7,700
  // of them alive forever while this gate printed "0 particles alive" and
  // PASSED. Measured 2026-09-08 by `--perf --scenario idle`: activeChunks 0
  // (so ab6ce9c's CA fix is genuinely working) and fluidLive 7,680 flat for
  // 240 ticks, costing 3.7 ms/frame of solver that rule 2 says must sleep.
  //
  // WHY A PROBE WINDOW AND NOT ONE READ. FA_LIVE is state, but every settle
  // counter is ZEROED per tick (seam_fill_settle), so a single sample says
  // nothing about a steady state. Twenty ticks with a read after each turns
  // "nothing settled" into WHICH of the three opposite causes it was:
  //   blocks 0                  -> never went calm (settleJudge/settleScan)
  //   blocks > 0, refused high  -> column arithmetic did not fit (geometry)
  //   blocks > 0, unstable high -> settleCheck's excite-stability veto
  //   settled > 0, live flat    -> converting and being re-excited (a loop)
  // That is the whole ladder, bought in one run instead of one hypothesis per
  // run (CLAUDE.md "When to run what", rule 6).
  // Slot indices are common.wgsl's FA_* map, spelled as raw subscripts with the
  // name in a comment because that is how every other fluid gate reads this
  // buffer (selftest_ca.cpp:471, selftest_water.cpp:1008) — one convention,
  // not a fourth mirror of the same table.
  uint32_t faLive0 = 0;
  uint64_t faBlocks = 0, faRefused = 0, faUnstable = 0, faSettled = 0,
           faExcited = 0, faExSeen = 0, faExCandid = 0, faCeil = 0, faFloor = 0,
           faForced = 0, faSealed = 0;
  {
    uint32_t fa[kFluidArgsWords] = {};  // ReadFluidArgsSync fills the whole map
    ReadFluidArgsSync(ctx, world, fa);
    faLive0 = fa[7];                    // FA_LIVE
    for (int i = 0; i < 20; i++) {
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false,
                 {8, 3, 8}, false, false);
      ctx.WaitIdle();
      ReadFluidArgsSync(ctx, world, fa);
      faBlocks += fa[13];               // FA_SETBLOCKS
      faRefused += fa[25];              // FA_SETREFUSED
      faUnstable += fa[26];             // FA_SETUNSTABLE
      faSettled += fa[10];              // FA_SETTLED
      faExcited += fa[11];              // FA_EXCITED
      faExSeen += fa[27];               // FA_EXSEEN
      faExCandid += fa[28];             // FA_EXCANDID
      faCeil += fa[30];                 // FA_SETCEIL  (sim_fluid_seam.wgsl)
      faFloor += fa[31];                // FA_SETFLOOR (sim_fluid_seam.wgsl)
      faForced += fa[32];               // FA_FORCED   (sim_fluid_seam.wgsl)
      faSealed += fa[33];               // FA_SEALED   (sim_fluid_seam.wgsl)
    }
    faLive1 = fa[7];                    // FA_LIVE
  }
  // THE EJECTA COUNT, RE-READ AT THE END. `particlesLeft` above is sampled the
  // instant the settle loop exits, which is the WRONG MOMENT for a claim about
  // a settled world: a busy settle leaves a backlog of splash droplets that the
  // next hundred ticks retire perfectly well, and reading it early cannot tell
  // that backlog apart from a leak. The verdict uses this one; the early value
  // is kept and printed beside it because the DIFFERENCE is the diagnosis.
  {
    uint32_t counts2[2] = {};
    ReadCountsSync(ctx, world, counts2);
    particlesEnd = std::min(counts2[sim.Page()], kParticleCap);
  }
  std::printf("sleep: MPM %u -> %u particles over 20 quiet ticks | picked %llu "
              "blocks, refused %llu infeasible (columns: %llu no-room-at-ceiling, "
              "%llu no-floor/trapped), %llu unstable | forced %llu, sealed %llu"
              " | settled %llu eighths, excited %llu (seen %llu, candidates "
              "%llu)\n",
              faLive0, faLive1, (unsigned long long)faBlocks,
              (unsigned long long)faRefused, (unsigned long long)faCeil,
              (unsigned long long)faFloor, (unsigned long long)faUnstable,
              (unsigned long long)faForced, (unsigned long long)faSealed,
              (unsigned long long)faSettled, (unsigned long long)faExcited,
              (unsigned long long)faExSeen, (unsigned long long)faExCandid);

  rhi::Buffer staging = CreateBuffer(ctx.device, kNumSlots * 4,
                                      rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                                      "dirtyRead");
  rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  enc.CopyBufferToBuffer(sim.DirtyActive(), 0, staging, 0, kNumSlots * 4);
  ctx.queue.Submit(enc.Finish());
  std::vector<uint32_t> awake;
  {
    std::vector<uint32_t> d_((kNumSlots * 4) / 4, 0);
    rhi::ReadBufferBlocking(ctx.device, staging, 0, d_.data(), (size_t)(kNumSlots * 4));
    const uint32_t* d = d_.data();

          // ---- WHY are they awake (common.wgsl's DIRTY_R_* / sim_step's
          // DIRTY_M_*) ---------------------------------------------------
          //
          // The dirty word is a reason BITMASK, not the literal 1, so the
          // buffer this loop already read names the rule that asked for each
          // chunk. Printed unconditionally and on PASS as well as FAIL: "5
          // chunks active" and "5 chunks active | MOVE 5 | powder 5" cost the
          // same run and only one of them is a measurement.
          //
          // This exists because the alternative was measured. Diagnosing a
          // never-sleeping pond from the awake COUNT took ~13 ten-minute
          // world-probe runs and a bespoke sampler that had to be debugged
          // three times; the reason bits answered it in one. That is
          // CLAUDE.md rule 6 with a price tag on it.
          //
          // Bit order must match kName in world.cpp's snapshot fold.
          {
            // The names come from kDirtyReasonName in world.h. They used to be
            // a private copy here, with a comment on each of the two saying it
            // must match the other; the first bit added after that comment was
            // written broke it, and a histogram that silently drops its last
            // bits is worse than no histogram (rule 6).
            // ONE pass, and it is the SAME pass that fills `awake`. An earlier
            // revision counted the reasons in a second loop over the same
            // array and printed "write 128 ... wbody 32761" next to "0 / 32768
            // chunks active" — two readings of one buffer that cannot both be
            // true. Whatever the cause, the fix that makes it unable to happen
            // again is not to have two loops.
            // kNumSlots, not kNumChunks: the dirty buffer is STORAGE, indexed
            // by slot, so a ticket slot's reason bits are in it too (tickets
            // P0, class (b)).
            uint32_t why[kDirtyReasonBits] = {0};
            for (uint32_t i = 0; i < kNumSlots; i++) {
              const uint32_t w = d[i];
              if (w == 0) continue;
              sleepActive++;
              awake.push_back(i);
              for (int b = 0; b < kDirtyReasonBits; b++)
                if (w & (1u << b)) why[b]++;
            }
            std::printf("sleep: awake by reason (%u chunks):", sleepActive);
            bool any = false;
            for (int b = 0; b < kDirtyReasonBits; b++)
              if (why[b]) {
                std::printf(" %s %u", kDirtyReasonName[b], why[b]);
                any = true;
              }
            std::printf("%s\n", any ? "" : " none - fully quiet");
          }
  }

  // ---- DO THE AWAKE CHUNKS ACTUALLY WRITE ANYTHING? ----------------------
  //
  // The owner's report, from the live game: F6 shows a handful of chunks lit
  // permanently, and the "active voxels" overlay — which draws a red wireframe
  // on every voxel the CA WROTE this tick — shows nothing inside them. Those
  // two claims cannot both be right, and until now this gate could not say
  // which was lying: it printed a COUNT and a reason histogram, and a reason
  // bit records that markDirty was CALLED, not that a word changed.
  //
  // So diff the words, the way selftest_terrain.cpp's move pass already does
  // for the same question. WHICH FIELD moved is the whole point: a pond soaking
  // into its bed changes stain, a film creeping changes fullness, and a rule
  // that re-stamps without writing changes nothing at all — three different
  // bugs that all read as "N chunks awake".
  //
  // RUNS ON PASS TOO, unlike the block below it. "8 chunks are awake and every
  // one of them is genuinely working" and "8 chunks are awake and not one word
  // is moving" are opposite findings, and the second is a FALSE WAKE that the
  // <32 threshold would hide forever.
  if (!awake.empty()) {
    const size_t n = std::min<size_t>(awake.size(), 16);
    std::vector<std::vector<uint32_t>> before(n);
    for (size_t i = 0; i < n; i++) {
      before[i].assign(kChunkVol, 0);
      ReadVoxelsSync(ctx, world, awake[i], 1, before[i].data(), "sleepMove0");
    }
    for (int i = 0; i < 20; i++)
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false,
                 {8, 3, 8}, false, false);
    ctx.WaitIdle();
    uint32_t words = 0, matCh = 0, stateCh = 0, stainCh = 0, stampOnly = 0;
    std::vector<uint32_t> now(kChunkVol, 0);
    for (size_t i = 0; i < n; i++) {
      ReadVoxelsSync(ctx, world, awake[i], 1, now.data(), "sleepMove1");
      for (size_t v = 0; v < kChunkVol; v++) {
        const uint32_t a = before[i][v], b = now[v];
        if (a == b) continue;
        words++;
        const bool m = (a & 0xFFFu) != (b & 0xFFFu);
        const bool s = ((a >> 12) & 0xFu) != ((b >> 12) & 0xFu);
        const bool st = (a & 0x7F000000u) != (b & 0x7F000000u);
        if (m) matCh++;
        if (s) stateCh++;
        if (st) stainCh++;
        // Only the tick stamp (bits 16..18) and/or the excite scratch moved:
        // the cell was VISITED and re-stamped without its content changing,
        // which is what "awake but the overlay is empty" looks like from here.
        if (!m && !s && !st) stampOnly++;
      }
    }
    std::printf("sleep: awake chunks over 20 more ticks: %u words changed in "
                "%zu chunks (material %u, fullness %u, stain %u, STAMP-ONLY "
                "%u)%s\n",
                words, n, matCh, stateCh, stainCh, stampOnly,
                words == 0 ? "  <-- FALSE WAKE: nothing is writing" : "");

    // ---- AND WHAT DOES THE GEOMETRY LOOK LIKE THERE? ---------------------
    //
    // CLAUDE.md rule 6, one rung further. "27 words changed, 21 of them back
    // where they started" says a rule is CYCLING; it does not say which rule or
    // in what shape, and the liquid rules are told apart by exactly that — a
    // film against one riser is the terrace tread the rule was written for, a
    // film between two facing risers is the 2-cycle it cannot see. Those differ
    // only in cells the shader itself is forbidden to read, so the only place
    // the distinction can be made is HERE, on the CPU, with no lattice bound.
    //
    // Measured cost of not having this: the first repair of the never-sleeping
    // shoreline was aimed at the riser branch on an inference from
    // `fullness 0` — plausible, cheap to make, and it left 14 chunks awake
    // instead of 8. One printout of the neighbourhood would have aimed it.
    //
    // Prints a 5x5 plan view at the changed cell's own level and the level
    // below it (the floor), reading a 3x3x3 SLOT-chunk block so a cell on a
    // chunk border still gets a true neighbourhood instead of a wall of
    // out-of-buffer. '#' is anything solid the CA cannot enter, '.' air,
    // '1'-'8' this liquid's fullness in eighths, '*' some other material.
    if (words != 0) {
      uint32_t waterId = 0;
      for (size_t mi = 0; mi < c.mats.size(); mi++)
        if (c.mats[mi].name == "water") waterId = (uint32_t)mi;
      // The first awake chunk that actually moved something. One chunk is
      // enough — these come in families, and six cells of one is a shape.
      for (size_t i = 0; i < n; i++) {
        ReadVoxelsSync(ctx, world, awake[i], 1, now.data(), "sleepShape");
        std::vector<uint32_t> chg;
        for (size_t v = 0; v < kChunkVol; v++)
          if (before[i][v] != now[v]) chg.push_back((uint32_t)v);
        if (chg.empty()) continue;

        // 3x3x3 slot chunks around this one -> a 48^3 block, centre at +16.
        const int ccx = (int)(awake[i] % kNChunk),
                  ccy = (int)((awake[i] / kNChunk) % kNChunk),
                  ccz = (int)(awake[i] / (kNChunk * kNChunk));
        std::vector<uint32_t> blk((size_t)48 * 48 * 48, 0);
        std::vector<uint32_t> cbuf((size_t)kChunkVol);
        for (int dz = -1; dz <= 1; dz++)
          for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
              const int nx = ((ccx + dx) % (int)kNChunk + (int)kNChunk) % (int)kNChunk;
              const int ny = ((ccy + dy) % (int)kNChunk + (int)kNChunk) % (int)kNChunk;
              const int nz = ((ccz + dz) % (int)kNChunk + (int)kNChunk) % (int)kNChunk;
              ReadVoxelsSync(ctx, world,
                             (uint32_t)(nz * (int)kNChunk * (int)kNChunk +
                                        ny * (int)kNChunk + nx),
                             1, cbuf.data(), "sleepShapeN");
              for (uint32_t k = 0; k < kChunkVol; k++)
                blk[(size_t)((dz + 1) * 16 + (int)(k / 256)) * 48 * 48 +
                    (size_t)((dy + 1) * 16 + (int)((k / 16) % 16)) * 48 +
                    (size_t)((dx + 1) * 16 + (int)(k % 16))] = cbuf[k];
            }
        auto glyph = [&](int bx, int by, int bz) -> char {
          if (bx < 0 || bx >= 48 || by < 0 || by >= 48 || bz < 0 || bz >= 48)
            return '?';
          const uint32_t w = blk[(size_t)bz * 48 * 48 + (size_t)by * 48 + bx];
          const uint32_t m = w & 0xFFFu;
          if (m == 0) return '.';
          if (m == waterId) return (char)('1' + ((w >> 12) & 7u));
          if (m >= c.mats.size()) return '*';
          const uint32_t k = c.mats[m].gpu.klass;
          return (k == CLASS_SOLID || k == CLASS_POWDER) ? '#' : '*';
        };
        std::printf("sleep: shape of the movers in awake chunk (%d,%d,%d) "
                    "[rows are z-2..z+2, columns x-2..x+2]\n",
                    ccx, ccy, ccz);
        for (size_t j = 0; j < chg.size() && j < 6; j++) {
          const int lx = (int)(chg[j] % 16), ly = (int)((chg[j] / 16) % 16),
                    lz = (int)(chg[j] / 256);
          const int bx = 16 + lx, by = 16 + ly, bz = 16 + lz;
          const uint32_t a = before[i][chg[j]], b = now[chg[j]];
          std::printf("  local(%2d,%2d,%2d) %04x/f%u -> %04x/f%u   own level | "
                      "the floor under it\n",
                      lx, ly, lz, a & 0xFFFu, ((a >> 12) & 7u) + 1u,
                      b & 0xFFFu, ((b >> 12) & 7u) + 1u);
          for (int rz = -2; rz <= 2; rz++) {
            std::printf("    ");
            for (int rx = -2; rx <= 2; rx++) std::printf("%c", glyph(bx + rx, by, bz + rz));
            std::printf("   ");
            for (int rx = -2; rx <= 2; rx++) std::printf("%c", glyph(bx + rx, by - 1, bz + rz));
            std::printf("%s\n", rz == 0 ? "   <- the cell's row" : "");
          }
        }
        break;
      }
    }
  }

  // diagnosis on failure: where are the awake chunks, and what's in them?
  if (sleepActive >= 32 && !awake.empty()) {
    for (size_t i = 0; i < awake.size() && i < 12; i++) {
      uint32_t ci = awake[i];
      std::printf("  awake chunk (%u,%u,%u)", ci % kNChunk, (ci / kNChunk) % kNChunk,
                  ci / (kNChunk * kNChunk));
      if (i % 4 == 3) std::printf("\n");
    }
    std::printf("\n");
    // material histogram of the first awake chunks
    uint32_t hist[64] = {};
    {
      // Through the CPU seam (§2.1a) — one call per awake slot, since they are
      // arbitrary slot indices rather than a contiguous range.
      std::vector<uint32_t> v_((size_t)kChunkVol * 4, 0);
      for (int k = 0; k < 4 && k < (int)awake.size(); k++)
        ReadVoxelsSync(ctx, world, awake[k], 1,
                       v_.data() + (size_t)k * kChunkVol, "voxRead");
      const uint32_t* v = v_.data();
      for (uint32_t i = 0; i < kChunkVol * 4; i++) hist[std::min(v[i] & 0xFFFu, 63u)]++;
    }
    std::printf("  first-4-chunk contents:");
    for (uint32_t m = 1; m < 64; m++)
      if (hist[m]) std::printf(" %s=%u", m < mats.size() ? mats[m].name.c_str() : "?", hist[m]);
    std::printf("\n");
  }
}
// ---- THE HYDROSTATIC INVARIANT, at the authored tarn --------------------
//
// "An eighth is a SURFACE thing; anything with the same liquid on top of it is
// full." A partial cell with water standing on it is both physically wrong and
// the fuel the lateral levelling rules used to burn forever, so a settled world
// should contain none of them.
//
// Measured HERE rather than in a probe of its own because this gate already
// generates the world, already settles it, and the harness map already
// carries its fixture lake at (420,420) — the question costs one readback of the
// chunk column over it instead of a ten-minute parked flight. (It was a parked
// flight first. Nine of them, and the sampler kept missing the water.)
//
// The top row of each chunk is skipped: its neighbour is in the chunk above,
// which this box may not have read. That costs 1/16 of the population and no
// accuracy in an is-it-zero question.
uint64_t subPartial = 0, subTotal = 0;
{
  std::vector<uint32_t> v((size_t)kChunkVol, 0);
  for (int cy = 11; cy <= 14; cy++)
    for (int cz = 24; cz <= 28; cz++)
      for (int cx = 24; cx <= 28; cx++) {
        const uint32_t slot = World::SlotChunkIndex({cx, cy, cz});
        ReadVoxelsSync(ctx, world, slot, 1, v.data(), "tarnRead");
        for (uint32_t z = 0; z < kChunk; z++)
          for (uint32_t y = 0; y + 1 < kChunk; y++)
            for (uint32_t x = 0; x < kChunk; x++) {
              const uint32_t w = v[(z * kChunk + y) * kChunk + x];
              const uint32_t m = w & 0xFFFu;
              if (m == 0 || m >= mats.size()) continue;
              if (mats[m].gpu.klass != CLASS_LIQUID) continue;
              if ((v[(z * kChunk + y + 1) * kChunk + x] & 0xFFFu) != m) continue;
              subTotal++;
              if (((w >> 12) & 0xFu) != 7u) subPartial++;  // state = f - 1
            }
      }
}
std::printf("sleep: hydrostatic at the tarn: %llu of %llu submerged liquid "
            "cells are PARTIAL\n",
            (unsigned long long)subPartial, (unsigned long long)subTotal);

// THREE conditions now, and the third is the one this gate was missing: the
// MPM population must reach zero, not merely stop growing. Rule 2 is about
// COST, and a settled world that still holds particles pays the whole 9-substep
// solver table every tick forever (measured 3.7 ms/frame at the authored
// home_lake). `particlesLeft` is the EJECTA system; `faLive1` is the water.
bool sleepOk = sleepActive < 32 && particlesEnd == 0 && faLive1 == 0;
std::printf("sleep: %s (%u / %u chunks active, %u particles alive (%u at "
            "settle), %u MPM particles alive, quiet after ~%d settle ticks, "
            "%llu/%llu submerged cells partial)\n",
            sleepOk ? "PASS" : "FAIL", sleepActive, kNumSlots, particlesEnd,
            particlesLeft, faLive1, settled, (unsigned long long)subPartial,
            (unsigned long long)subTotal);

  // Verdict: the flag the moved body already computed.
  return sleepOk ? Status::Pass : Status::Fail;
}

// ---- evaporation -------------------------------------------------------
Status GateEvaporation(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const std::vector<MaterialDef>& mats = c.mats;
// ---- the sun dries spread water, not bodies of water -----------------------
// The mirror of the pond-freeze test above, and it asserts the thing that
// actually went wrong: evaporation used to be a flat chance on any sunlit
// surface cell, so a pond boiled off from its whole top face in seconds.
// The fix is scaleByNeighbors with minCount 4 — a cell must have >= 4
// non-water face neighbours before the sun can take it.
//
// A rate comparison would be weak here for the same reason it was for
// freezing, so this asserts the two ENDS of the rule as hard invariants on
// the final state:
//
//   1. A pond does NOT shrink. Its surface counts 1 non-water neighbour
//      (the air above), which is below minCount, so after thousands of
//      sunlit ticks every last surface cell must still be water. One
//      missing cell means the gate is not being applied.
//   2. Isolated droplets DO go. A single water voxel sitting on stone
//      counts 5-6 non-water neighbours and must evaporate.
//
// Together they pin the rule from both sides: (1) alone passes a rule that
// never fires, (2) alone passes the old always-fires rule.
bool evapOk = false;
{
  auto matId = [&](const char* n) {
    for (size_t i = 0; i < mats.size(); i++)
      if (mats[i].name == n) return (int)i;
    return -1;
  };
  const int wi = matId("water"), si = matId("stone");
  // Pin the cycle at noon so the day-gated rule is at full strength every
  // tick (minLight 120 needs a high sun).
  Tuning noon = CurrentTuning();
  noon.dayNight.freeze = 1;
  noon.dayNight.freezePhase = 32768;  // 32768 = noon
  Tuning saved = CurrentTuning();
  SetCurrentTuning(noon);

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // Left: a walled pond, 3 deep, open to the sky. Right: a row of single
  // water voxels on a stone shelf, spaced 3 apart so none touches another
  // (spacing matters — two adjacent droplets would each count a water
  // neighbour and drop toward the gate).
  const int px = 80, pz = 96, R = 8, kDepth = 3;
  const int dx = 120, kDrops = 12;
  // Terrain-relative, over the WHOLE footprint the pond and the droplet
  // shelf occupy: at a literal y120 this pond was inside bedrock after the
  // datum moved, which reads as "no evaporation in 2500 noon ticks"
  // because `needsSky` is false 200 voxels down, not as a buried fixture.
  const int py = FixtureYOver(px - R - 1, pz - R - 1,
                              dx + kDrops * 3, pz + R + 1, kDefaultSeed, 8);
  std::vector<CellOp> scene;
  auto put = [&](int x, int y, int z, int m) {
    uint32_t state = (m == wi) ? 7u : 0u;  // liquids are born full
    scene.push_back({World::SlotCellIndex({x, y, z}),
                     (uint32_t)((m & 0xFFF) | (state << 12))});
  };
  for (int z = -R - 1; z <= R + 1; z++)
    for (int x = -R - 1; x <= R + 1; x++) {
      put(px + x, py - 1, pz + z, si);  // floor
      bool rim = (x < -R || x > R || z < -R || z > R);
      for (int y = 0; y < kDepth; y++) put(px + x, py + y, pz + z, rim ? si : wi);
      for (int y = kDepth; y < kDepth + 3; y++) put(px + x, py + y, pz + z, 0);
    }
  for (int i = 0; i < kDrops; i++) {
    const int x = dx + i * 3;
    put(x, py - 1, pz, si);            // shelf under the droplet
    put(x, py, pz, wi);                // the droplet itself
    for (int y = 1; y < 4; y++) put(x, py + y, pz, 0);  // open sky above
  }
  uint32_t et = 1;
  SubmitTick(ctx, world, sim, et, kDefaultSeed, {}, {}, scene, false,
             {6, 7, 6}, false, false);
  ctx.WaitIdle();

  // Long enough that a droplet at ~2-3 per-mille is overwhelmingly likely to
  // have gone (P(survive) < 1e-3 at 2500 ticks), and long enough that the
  // old flat 2 per-mille rule would have stripped the pond surface many
  // times over.
  for (uint32_t t = 2; t <= 2500; t++)
    SubmitTick(ctx, world, sim, ++et, kDefaultSeed, {}, {}, {}, false,
               {6, 7, 6}, false, false);
  ctx.WaitIdle();

  std::vector<uint32_t> vox(kNumSlots * (size_t)kChunkVol);
  {
    ReadVoxelsSync(ctx, world, 0, kNumSlots, vox.data(), "evapRead");  // §2.1a
  }
  auto readCell = [&](int x, int y, int z) {
    return vox[World::SlotCellIndex({x, y, z})] & 0xFFFu;
  };

  // (1) The pond's surface layer must be intact — every cell still water.
  const int surf = py + kDepth - 1;
  uint32_t surfN = 0, surfWater = 0;
  for (int z = -R; z <= R; z++)
    for (int x = -R; x <= R; x++) {
      surfN++;
      if (readCell(px + x, surf, pz + z) == (uint32_t)wi) surfWater++;
    }
  // (2) The droplets must be gone.
  uint32_t dropsLeft = 0;
  for (int i = 0; i < kDrops; i++)
    if (readCell(dx + i * 3, py, pz) == (uint32_t)wi) dropsLeft++;

  evapOk = surfWater == surfN && dropsLeft == 0;
  std::printf("evaporation: %s (pond surface %u/%u water after 2500 noon "
              "ticks, %u/%d isolated droplets left)\n",
              evapOk ? "PASS" : "FAIL", surfWater, surfN, dropsLeft, kDrops);
  SetCurrentTuning(saved);
}

  // Verdict: the flag the moved body already computed.
  return evapOk ? Status::Pass : Status::Fail;
}

// ---- blood-stain -------------------------------------------------------
Status GateBloodStain(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const std::vector<MaterialDef>& mats = c.mats;
// ---- blood stains what it touches, and then goes to sleep ------------------
// Staining writes the voxel word's spare bits (kStain*, world.h) from the
// liquid movement path in sim_step.wgsl. Three separate things have to hold,
// and none of them is visible to the hash test:
//
//   1. It HAPPENS — stone under a pool of blood ends up carrying a stain of
//      the right type. (A rule that silently never fires still hashes fine.)
//   2. It TERMINATES — the chunk goes back to sleep. This is the rule-2 risk
//      and the one that would not show up until a level is full of gore: a
//      pool of blood sits on stone forever, so a naive "keep me awake while
//      I'm touching something stainable" would pin those chunks awake for
//      the rest of the session. doStaining only holds the chunk while there
//      is UNSTAINED surface left in reach, and this is what proves it.
//   3. It stays BOUNDED — stain amounts saturate at kStainAmtMax rather than
//      overflowing into the neighbouring bits of the word (which would
//      corrupt the tick-stamp and, one bit further, the material id).
bool stainOk = false;
{
  auto matId = [&](const char* n) {
    for (size_t i = 0; i < mats.size(); i++)
      if (mats[i].name == n) return (int)i;
    return -1;
  };
  const int bi = matId("blood"), si = matId("stone");
  const uint32_t stainType = mats[bi].gpu.stainPack & kStainPackTypeMask;

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // A stone basin with blood poured into it, in open air well clear of the
  // terrain. The floor and walls are what should end up stained.
  const int px = 96, py = 120, pz = 96, R = 6;
  std::vector<CellOp> pool;
  auto put = [&](int x, int y, int z, int m) {
    uint32_t state = (m == bi) ? 7u : 0u;  // liquids are born full
    pool.push_back({World::SlotCellIndex({x, y, z}),
                    (uint32_t)((m & 0xFFF) | (state << 12))});
  };
  for (int z = -R - 1; z <= R + 1; z++)
    for (int x = -R - 1; x <= R + 1; x++) {
      put(px + x, py - 1, pz + z, si);  // floor
      bool rim = (x < -R || x > R || z < -R || z > R);
      for (int y = 0; y < 2; y++) put(px + x, py + y, pz + z, rim ? si : bi);
      for (int y = 2; y < 5; y++) put(px + x, py + y, pz + z, 0);
    }
  uint32_t st = 1;
  SubmitTick(ctx, world, sim, st, kDefaultSeed, {}, {}, pool, false,
             {6, 7, 6}, false, false);
  ctx.WaitIdle();
  // Run until the blood has DRIED AWAY, not merely until it has finished
  // staining. Blood carries an unconditional decay rule ("blood dries away",
  // reactions.json) at 8 per-mille, which correctly holds its chunks awake
  // for as long as any blood is left — so a shorter run would measure a pool
  // that is still mid-evaporation and say nothing about sleep.
  //
  // Waiting for the dry-out is the stronger test anyway: it asserts that the
  // STAIN OUTLIVES THE LIQUID. That is the whole point of putting stain in
  // the voxel word rather than deriving it from what is standing there — the
  // mark on the floor has to survive the blood evaporating off it, and it
  // has to do so without keeping the chunk awake.
  // 8 per-mille gives a half-life of ~87 ticks; 4000 is ~46 half-lives.
  const uint32_t kDryTicks = 4000;
  for (uint32_t t = 2; t <= kDryTicks; t++)
    SubmitTick(ctx, world, sim, ++st, kDefaultSeed, {}, {}, {}, false,
               {6, 7, 6}, false, false);
  ctx.WaitIdle();

  std::vector<uint32_t> vox(kNumSlots * (size_t)kChunkVol);
  {
    ReadVoxelsSync(ctx, world, 0, kNumSlots, vox.data(), "stainRead");  // §2.1a
  }

  // Count stained floor voxels, and check every stain in the world is
  // well-formed: right type, amount within the field, and never on air.
  uint32_t stainedFloor = 0, floorN = 0, badStain = 0, consumed = 0;
  for (int z = -R; z <= R; z++)
    for (int x = -R; x <= R; x++) {
      floorN++;
      uint32_t w = vox[World::SlotCellIndex({px + x, py - 1, pz + z})];
      if ((w & 0xFFFu) == 0u) { consumed++; continue; }  // eaten by the stain
      if (VoxStainType(w) == stainType && VoxStainAmt(w) > 0) stainedFloor++;
    }
  for (size_t i = 0; i < vox.size(); i++) {
    uint32_t w = vox[i];
    uint32_t type = VoxStainType(w), amt = VoxStainAmt(w);
    if (type == 0 && amt == 0) continue;
    // A stain must have both halves, name a registered type, fit the field,
    // and sit on actual matter.
    //
    // Water now wets absorbent ground, and worldgen paints ponds on grass and
    // sand by the thousand, so a world-wide scan legitimately sees tens of
    // thousands of "wet" stains. Asserting they were blood reported them as
    // malformed packing — a broken test, not a broken sim. What is actually
    // invariant is that every stain is WELL-FORMED; the blood-specific
    // assertion is `stainedFloor` above, which looks only at the test's floor.
    if (type > kStainTypeMax || amt == 0 || amt > kStainAmtMax ||
        (w & 0xFFFu) == 0u) {
      badStain++;
    }
  }

  // How much blood is left? The sleep assertion only means anything once the
  // pool has actually dried, so report it rather than assuming.
  uint32_t bloodLeft = 0;
  for (size_t i = 0; i < vox.size(); i++)
    if ((vox[i] & 0xFFFu) == (uint32_t)bi) bloodLeft++;

  uint32_t stainActive = ReadActiveChunksSync(ctx, world, sim);
  // Four assertions, each covering a different way this could be broken:
  //   covered   — the stain HAPPENED (a rule that never fires still hashes)
  //   badStain  — every stain is well-formed and none landed on air, so the
  //               packing never overflowed into the stamp or material bits
  //   bloodLeft — the pool really did dry, so the sleep check below is
  //               measuring a settled world and not a mid-reaction one
  //   active    — and the stained floor SLEEPS. This is the rule-2 half: a
  //               stain is permanent, so if it held its chunk awake the way
  //               a naive "am I touching something stainable" rule would,
  //               every gore-soaked chunk would stay awake for the session.
  //
  // bloodLeft is checked against a small threshold, not zero. A handful of
  // isolated voxels can settle in a chunk that then goes to sleep with no
  // neighbour left to wake it, and a sleeping chunk does not run reactions —
  // so their decay is simply paused until something disturbs them. That is
  // the sleep rule working as designed (cost scales with activity), not a
  // stuck reaction, and it is exactly why `active` is the assertion that
  // matters here rather than a demand that every last voxel evaporate.
  bool covered = stainedFloor + consumed > floorN / 2 && stainedFloor > 0;
  const uint32_t kBloodDregs = 16;  // isolated voxels in sleeping chunks
  stainOk = covered && badStain == 0 && bloodLeft <= kBloodDregs &&
            stainActive < 32;
  std::printf("blood stain: %s (%u/%u floor stained, %u consumed, %u malformed, "
              "%u blood left, %u chunks active after %u ticks)\n",
              stainOk ? "PASS" : "FAIL", stainedFloor, floorN, consumed,
              badStain, bloodLeft, stainActive, kDryTicks);
}

  // Verdict: the flag the moved body already computed.
  return stainOk ? Status::Pass : Status::Fail;
}

// ---- flung-liquid ------------------------------------------------------
Status GateFlungLiquid(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const std::vector<MaterialDef>& mats = c.mats;
bool fullOk = false;
// ---- flung liquid lands FULL (the anti-gelatin invariant) --------------
// A liquid that rejoins the grid from flight must be born at full fullness,
// exactly like one a brush paints (sim_mutate.wgsl: "liquids are born
// full"). This is a LOOK invariant with no visible symptom in any other
// check: the state nibble is fullness, the renderer builds blood's smooth
// surface from that as a density field, and a spawn that leaves the nibble
// at 0 lands at 1/8 density. The hash still matches, the world still
// settles, nothing fails — the blood just renders as separately shaded
// translucent cubes (the "gelatin" look shadeViscous exists to avoid).
//
// So this asserts the nibble directly rather than trusting a screenshot.
{
  auto matId = [&](const char* n) {
    for (size_t i = 0; i < mats.size(); i++)
      if (mats[i].name == n) return (int)i;
    return -1;
  };
  const int bloodMat = matId("blood");

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // A stone slab in open air, and blood particles dropped onto it — the
  // same coordinates and window the stain test uses, which are known to sit
  // inside the residency window with nothing else going on around them.
  // Terrain-relative: at a literal y120 the slab and the drops above it
  // were both inside the hillside after the datum moved, and the gate
  // reported it as "0 blood voxels landed".
  const int px = 96, pz = 96;
  const int py = FixtureY(px, pz, kDefaultSeed, 24);
  // The PLAYER CHUNK below has to follow the slab. It was a literal {6, 7, 6}
  // (y112..127), which was fine while the terrain band was y32..y86 and is 100
  // voxels under the fixture now — and the player chunk is what centres the
  // 3x3x3 CPU mirror and the particle materialization ring, so a stale one
  // reads as "nothing landed" rather than as a mis-aimed harness.
  // A WELL, not a bare plate. The assertion is that a particle rejoining the
  // grid is born at FULL fullness, and on an open plate 25 drops sheet out over
  // 49 cells within the 40 ticks and the maximum reads 5/7 — a pass that
  // depended on the drops happening to pile up. One voxel of rim makes the
  // pooling structural: 25 drops into a 5x5 well is one full cell each.
  std::vector<CellOp> slab;
  for (int z = -3; z <= 3; z++)
    for (int x = -3; x <= 3; x++) {
      slab.push_back({World::SlotCellIndex({px + x, py, pz + z}),
                      (uint32_t)(matId("stone") & 0xFFF)});
      if (std::abs(x) == 3 || std::abs(z) == 3)
        for (int wy = 1; wy <= 5; wy++)
          slab.push_back({World::SlotCellIndex({px + x, py + wy, pz + z}),
                          (uint32_t)(matId("stone") & 0xFFF)});
    }

  std::vector<ParticleSpawn> drops;
  for (int i = 0; i < 25 && bloodMat > 0; i++) {
    ParticleSpawn s{};
    // ONE DROP PER CELL over the 5x5 floor of the well, and it has to stay that
    // way: stacking 25 drops into a 3x3 does not pile them three deep, it lands
    // them in the same cells and reads 2/7 — worse than the open plate this
    // replaced. The WELL is what fixes the original 5/7, not the packing: 25
    // full voxels in 25 cells with a wall around them have nowhere to flow, so
    // "born full" survives the 40 ticks the gate waits.
    s.px = (px - 2 + (i % 5)) * 256 + 128;
    // DROPPED FROM INSIDE THE WELL, two voxels up, not six. sim.windMode is 1,
    // so a particle in flight takes wind drag — over six voxels of fall that is
    // enough lateral drift to put some of the 25 drops on the rim or outside
    // it, and the gate then measures a sheet spreading on a plate instead of
    // the thing it asserts. The fall still exercises the deposit path.
    s.py = (py + 3) * 256 + 128;
    s.pz = (pz - 2 + (i / 5)) * 256 + 128;
    s.vx = 0; s.vy = -128; s.vz = 0;
    s.payload = (uint32_t)bloodMat;  // state nibble deliberately left 0
    s.flags = kPFlagAlive;
    drops.push_back(s);
  }
  // THE BIRTH, not the 40th tick. The well's floor layer is read EVERY tick
  // and the fullest blood cell ever seen there is what the claim is about:
  // a drop that rejoined the grid full and then lost an eighth to an authored
  // sink (blood dries; since the rule-unification W1-B1 matter rules a liquid
  // pays for staining the floor it wets) is still a drop born full. Reading
  // only tick 40 made the claim "born full AND nothing has touched it since".
  uint32_t maxEver = 0;
  int maxEverTick = -1;
  std::vector<uint32_t> wellBuf((size_t)kChunkVol);
  uint32_t ft = 20000;
  for (int i = 0; i < 40; i++) {
    SubmitTick(ctx, world, sim, ++ft, kDefaultSeed, {}, {},
               i == 0 ? slab : std::vector<CellOp>{}, false, {6, py >> 4, 6},
               /*wantReadback=*/false, /*particlesActive=*/true,
               i == 1 ? drops : std::vector<ParticleSpawn>{});
    ctx.WaitIdle();
    ctx.ProcessEvents();
    for (int cz = (pz - 2) >> 4; cz <= (pz + 2) >> 4; cz++)
      for (int cy = (py + 1) >> 4; cy <= (py + 3) >> 4; cy++)
        for (int cx = (px - 2) >> 4; cx <= (px + 2) >> 4; cx++) {
          ReadVoxelsSync(ctx, world, World::SlotChunkIndex({cx, cy, cz}), 1,
                         wellBuf.data(), "fullWell");
          for (uint32_t k = 0; k < kChunkVol; k++) {
            const uint32_t w = wellBuf[k];
            if ((w & 0xFFFu) != (uint32_t)bloodMat) continue;
            const uint32_t st = (w >> 12) & 0xFu;
            if (st > maxEver) { maxEver = st; maxEverTick = i; }
          }
        }
  }

  std::vector<uint32_t> fv(kNumSlots * (size_t)kChunkVol);
  {
    ReadVoxelsSync(ctx, world, 0, kNumSlots, fv.data(), "fullRead");  // §2.1a
  }
  // Count landed blood and how much of it is at less than full fullness.
  // Blood FLOWS once it lands, and flowing splits a cell's fullness across
  // its neighbours, so partial cells are expected at the spreading edge —
  // the bug being caught is "everything lands at the 1/8 floor", so the
  // assertion is that the maximum reached full, not that every cell did.
  uint32_t landed = 0, maxState = 0;
  for (size_t i = 0; i < fv.size(); i++) {
    if ((fv[i] & 0xFFFu) != (uint32_t)bloodMat) continue;
    landed++;
    maxState = std::max(maxState, (fv[i] >> 12) & 0xFu);
  }
  fullOk = bloodMat > 0 && landed > 0 && maxEver == 7;
  std::printf("flung liquid fullness: %s (%u blood voxels landed, fullest "
              "ever %u/7 at tick %d, %u/7 at tick 40 - 0 would render as "
              "gelatin cubes)\n",
              fullOk ? "PASS" : "FAIL", landed, maxEver, maxEverTick, maxState);
  detail = Format("%u blood voxels landed, fullest ever %u/7 at tick %d, "
                  "fullest at tick 40 %u/7", landed, maxEver, maxEverTick,
                  maxState);
}

  // Verdict: the flag the moved body already computed.
  return fullOk ? Status::Pass : Status::Fail;
}

// ---- fluid-det -----------------------------------------------------------
// The MLS-MPM prototype's determinism spike (docs/PLAN_mpm_fluids.md Phase 0,
// run in-engine): drop a block of fluid particles into a stone basin, run the
// solver, and require the ENTIRE particle buffer to hash identically across
// two from-worldgen runs. This is the gate on the plan's central bet — that
// fixed-point integer-atomic P2G accumulation makes a GPU MPM scatter
// scheduling-independent. It also asserts basic physical sanity (the basin
// held the fluid; velocities and J stayed inside the solver's clamps) and
// that the WORLD hash is identical across runs too — the fluid must never
// leak into CA state (it writes no voxels by construction).
Status GateFluidDet(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  const int kTicks = 80;
  // Window-local basin placement (slot space via SlotCellIndex, same as the
  // pond gates — each run re-worldgens, so these are stable).
  const int px = 96, py = 120, pz = 96, R = 8, H = 8;

  // The particle's settled identity: what the settle converter writes back as
  // voxels. A zero mat is a DEAD particle to the seam's compaction, so this
  // is load-bearing, not cosmetic.
  uint32_t waterId = 0;
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == "water") { waterId = (uint32_t)i; break; }
  if (waterId == 0) {
    detail = "no 'water' material";
    return Status::Fail;
  }

  // The spawn block: 4^3 cells x 8 particles on the half-cell lattice with a
  // hash jitter — a pure function of the index, so both runs see identical
  // ops (the twice-run comparison's precondition, like SelftestOps).
  auto detSpawns = [&]() {
    std::vector<FluidSpawnOp> fs;
    for (int cz = -2; cz < 2; cz++)
      for (int cy = 0; cy < 4; cy++)
        for (int cx = -2; cx < 2; cx++)
          for (int s = 0; s < 8; s++) {
            uint32_t h = ((uint32_t)fs.size() * 6271u + 12345u) * 747796405u +
                         2891336453u;
            FluidSpawnOp op{};
            op.px = ((px + cx) << 16) + ((s & 1) ? 49152 : 16384) +
                    (int32_t)(h % 8192u) - 4096;
            op.py = ((py + 3 + cy) << 16) + ((s & 2) ? 49152 : 16384) +
                    (int32_t)((h >> 13) % 8192u) - 4096;
            op.pz = ((pz + cz) << 16) + ((s & 4) ? 49152 : 16384) +
                    (int32_t)((h >> 19) % 8192u) - 4096;
            op.mat = waterId;
            fs.push_back(op);
          }
    return fs;
  };

  uint64_t partHash[2] = {0, 0};
  uint32_t worldHash[2] = {0, 0};
  uint32_t spawned = 0;
  uint32_t live = 0;
  std::vector<uint32_t> last;  // run-2 live particle words, for the sanity sweep
  uint32_t settledEighths = 0;
  for (int run = 0; run < 2; run++) {
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();

    std::vector<CellOp> basin;
    auto put = [&](int x, int y, int z, uint32_t m) {
      basin.push_back({World::SlotCellIndex({x, y, z}),
                       (uint32_t)((m & 0xFFFu))});
    };
    for (int z = -R - 1; z <= R + 1; z++)
      for (int x = -R - 1; x <= R + 1; x++) {
        put(px + x, py - 1, pz + z, kMatStone);
        bool rim = (x < -R || x > R || z < -R || z > R);
        for (int y = 0; y < H; y++)
          put(px + x, py + y, pz + z, rim ? kMatStone : kMatAir);
      }

    uint32_t fluidN = 0;
    uint32_t ft = 30000;
    for (int i = 0; i < kTicks; i++) {
      std::vector<FluidSpawnOp> fs;
      if (i == 1) fs = detSpawns();
      SubmitTick(ctx, world, sim, ++ft, kDefaultSeed, {}, {},
                 i == 0 ? basin : std::vector<CellOp>{}, false, {6, 7, 6},
                 /*wantReadback=*/false, /*particlesActive=*/false,
                 {}, 0, fs, fluidN);
      fluidN += (uint32_t)fs.size();
      ctx.WaitIdle();
      ctx.ProcessEvents();
    }
    spawned = fluidN;

    // The live count is GPU-owned now (the seam's compaction — settle may
    // have converted some or all of the pool back to voxels inside the run).
    uint32_t fa[16] = {};
    rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0, fa,
                          64, "fluidDetArgs");
    live = std::min(fa[7], kFluidCap);

    // Hash the LIVE particles only (kFluidParticleWords stride): slots past
    // the live count are compaction leftovers — deterministic garbage within
    // a run but stale across runs, so they must not enter the hash.
    std::vector<uint32_t> buf((size_t)live * kFluidParticleWords);
    if (live > 0 &&
        !rhi::ReadbackBlocking(ctx.device, ctx.queue,
                               world.fluidParticles[sim.Page()], 0, buf.data(),
                               buf.size() * 4, "fluidDet")) {
      detail = "fluid particle readback failed";
      std::printf("fluid det: FAIL (readback failed)\n");
      return Status::Fail;
    }
    uint64_t h = 1469598103934665603ull;  // FNV-1a over the raw words
    for (uint32_t w : buf) {
      h ^= w;
      h *= 1099511628211ull;
    }
    partHash[run] = h ^ ((uint64_t)live << 32);
    worldHash[run] = HashWorldNow(ctx, world, sim, kDefaultSeed);
    if (run == 1) last.swap(buf);

    // Settled water in the basin: mass that left the particle pool through
    // the settle converter. Counted in eighths from the voxel words — the
    // seam's whole claim is that this plus the live pool equals the spawn.
    if (run == 1) {
      settledEighths = 0;
      std::vector<uint32_t> cbuf((size_t)kChunkVol);
      for (int cy = (py - 2) / 16; cy <= (py + H + 8) / 16; cy++)
        for (int cz2 = (pz - R - 2) / 16; cz2 <= (pz + R + 2) / 16; cz2++)
          for (int cx2 = (px - R - 2) / 16; cx2 <= (px + R + 2) / 16; cx2++) {
            uint32_t slot =
                World::SlotChunkIndex({cx2, cy, cz2});
            ReadVoxelsSync(ctx, world, slot, 1, cbuf.data(), "fluidDetVox");
            for (uint32_t i = 0; i < kChunkVol; i++) {
              int lx = (int)(i % 16) + cx2 * 16, ly = (int)((i / 16) % 16) + cy * 16,
                  lz = (int)(i / 256) + cz2 * 16;
              if (lx < px - R || lx > px + R || lz < pz - R || lz > pz + R ||
                  ly < py || ly > py + H)
                continue;
              uint32_t w = cbuf[i];
              if ((w & 0xFFFu) == waterId)
                settledEighths += ((w >> 12) & 0xFu) + 1u;
            }
          }
    }
  }

  // Physical sanity on the final live pool: the basin held (positions inside
  // the walls, nothing tunneled through the floor), and every particle
  // respects the solver's own clamps. Bounds are deliberately slack — this is
  // "the solver did not explode", not a look test.
  uint32_t escaped = 0, badV = 0, badJ = 0;
  uint32_t liveEighths = 0;
  for (uint32_t i = 0; i < live; i++) {
    const int32_t* p = (const int32_t*)&last[(size_t)i * kFluidParticleWords];
    int x = p[0] >> 16, y = p[1] >> 16, z = p[2] >> 16;
    if (x < px - R - 2 || x > px + R + 2 || y < py - 2 || y > py + H + 8 ||
        z < pz - R - 2 || z > pz + R + 2)
      escaped++;
    for (int a = 3; a < 6; a++)
      if (p[a] < -200000 || p[a] > 200000) { badV++; break; }
    if (p[15] < 30000 || p[15] > 100000) badJ++;
    liveEighths += ((uint32_t)p[18] >> 12) & 0x7u;  // attr fullness
  }

  bool det = partHash[0] == partHash[1];
  bool worldOk = worldHash[0] == worldHash[1];
  // Mass conservation across the seam: every spawned particle is either still
  // live (its fullness eighths) or settled into basin voxels. Exact integer
  // accounting — the seam's core claim.
  bool massOk = liveEighths + settledEighths == spawned;
  bool sane = spawned > 0 && escaped == 0 && badV == 0 && badJ == 0;
  bool ok = det && worldOk && sane && massOk;
  std::printf(
      "fluid det: %s (%u spawned -> %u live + %u settled eighths, %d ticks: "
      "particle hash %016llx %s, world hash %s, %u escaped, %u bad vel, "
      "%u bad J)\n",
      ok ? "PASS" : "FAIL", spawned, live, settledEighths, kTicks,
      (unsigned long long)partHash[0], det ? "matches" : "DIVERGED",
      worldOk ? "matches" : "DIVERGED", escaped, badV, badJ);
  detail = Format("%u spawned, %u live, %u settled, hash %016llx, det %s, "
                  "world %s, mass %s",
                  spawned, live, settledEighths,
                  (unsigned long long)partHash[0], det ? "ok" : "DIVERGED",
                  worldOk ? "ok" : "DIVERGED", massOk ? "ok" : "LOST");
  return ok ? Status::Pass : Status::Fail;
}

// ---- fluid-identity ------------------------------------------------------
// W1-B2: an MPM particle's ONE identity is its material. The part of that a
// gate can see without a camera is DENSITY: the solver used to weigh every
// particle the same (and group them by a four-slot `species` that made acid
// and water one liquid), so a heavier liquid poured over a lighter one just
// sat there until the settle converter handed it to the CA. Now momentum is
// mass-weighted by materials[].density and the grid divides by mass, so the
// same pressure field pushes the light liquid up through the heavy one.
//
// Fixture: fluid-det's basin (taller, and GLASS: since W1-B1 excited acid
// runs its own rules, and acid eats stone (-> gravel -> sand) and anything
// tag:dissolvable, so a stone basin spent the acid on its walls and both arms
// lost live particles alike — measured on the wave-1 tree, A 1.04 vs B 1.09.
// Glass carries only `meltable`; no acid or water rule names it, and acid and
// water have no rule against each other, so every particle's fate here is
// the solver's) filled wall to wall with two
// liquids, `L` cells of each (fluidIdentityLayerCells), spawned one cell-row
// per tick, bottom first. The interface is NOT flat: under a central 7x7
// patch it sits `D` cells lower (fluidIdentityDipCells), so the top liquid
// starts with a finger pushed into the bottom one. That seed matters: at grid
// resolution a mixed node carries one velocity, so two liquids only move
// relative to each other between nodes of different composition — thin flat
// layers are all interface and cannot overturn, which the first version of
// this gate measured (acid 4x water's density, 2+2 cell layers: no overturn
// in 25 ticks).
//   A: ACID (1200 kg/m^3) over WATER (1000) — unstable, the finger sinks;
//   B: WATER over ACID — stable, the same finger (now of water) is pushed
//      back up. Same volumes, same ticks, same jitter.
// The measure is by MATERIAL, never by spawn order (the seam re-excites
// settled water with a fresh birth tick): sep = mean height of live acid
// particles minus that of live water particles, in cells. Compression and
// numerical mixing shrink |sep| in BOTH arms alike (measured: A +1.25 ->
// +1.05, B -1.30 -> -1.11 over 25 ticks); density shrinks A's faster, because
// in A the heavy liquid is going down and in B it is already there. The
// verdict is that difference, at the last checkpoint where both arms are
// still mostly particles (after the settle converter the CA's own density
// swaps would pass this for the wrong reason): A's loss of separation must
// exceed B's by fluidIdentityMarginCells. The OLD solver weighed both liquids
// the same, so A and B were mirror images and the difference was ~0.
// Arm A runs twice: the non-water mass path (massScale, nodeMass, the
// composition words) is new code and gets its own twice-run particle + world
// hash — fluid-det only ever pours water.
Status GateFluidIdentity(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  uint32_t waterId = 0, acidId = 0;
  for (size_t i = 0; i < c.mats.size(); i++) {
    if (c.mats[i].name == "water") waterId = (uint32_t)i;
    if (c.mats[i].name == "acid") acidId = (uint32_t)i;
  }
  if (waterId == 0 || acidId == 0) {
    detail = "needs 'water' and 'acid' materials";
    return Status::Fail;
  }

  const int L = std::clamp((int)BaselineNumber("fluidIdentityLayerCells", 4.0), 1, 6);
  const int D = std::clamp((int)BaselineNumber("fluidIdentityDipCells", 2.0), 0, L - 1);
  const double margin = BaselineNumber("fluidIdentityMarginCells", 0.25);
  const int px = 96, py = 120, pz = 96, R = 8, H = 2 * L + 6;
  const int kSpawnTicks = 2 * L;             // one cell-row per tick
  static const int kChecks[] = {15, 20, 25, 30, 35, 40, 45};
  constexpr int kNumChecks = (int)(sizeof(kChecks) / sizeof(kChecks[0]));
  const int kLastTick = kChecks[kNumChecks - 1];

  // Interface height of column (cx, cz): L, or L - D under the central 7x7.
  auto iface = [&](int cx, int cz) {
    return (std::abs(cx) <= 3 && std::abs(cz) <= 3) ? L - D : L;
  };
  // Row `row` of the pool: the bottom liquid below the interface, the top one
  // above it, 8 particles per cell on the half-cell lattice with hash jitter.
  auto spawnRow = [&](int row, uint32_t bottomMat, uint32_t topMat) {
    std::vector<FluidSpawnOp> fs;
    for (int cz = -R; cz <= R; cz++)
      for (int cx = -R; cx <= R; cx++)
        for (int s = 0; s < 8; s++) {
          uint32_t h = ((uint32_t)fs.size() * 6271u + 777u + (uint32_t)row * 131u) *
                           747796405u + 2891336453u;
          FluidSpawnOp op{};
          op.px = ((px + cx) << 16) + ((s & 1) ? 49152 : 16384) +
                  (int32_t)(h % 8192u) - 4096;
          op.py = ((py + row) << 16) + ((s & 2) ? 49152 : 16384) +
                  (int32_t)((h >> 13) % 8192u) - 4096;
          op.pz = ((pz + cz) << 16) + ((s & 4) ? 49152 : 16384) +
                  (int32_t)((h >> 19) % 8192u) - 4096;
          op.mat = row < iface(cx, cz) ? bottomMat : topMat;
          fs.push_back(op);
        }
    return fs;
  };

  struct Arm {
    double sep[kNumChecks] = {};     // acid mean y - water mean y, cells
    uint32_t acidN[kNumChecks] = {}, waterN[kNumChecks] = {};
    uint32_t acidSpawned = 0, waterSpawned = 0;
    uint64_t hash = 0;
    uint32_t worldHash = 0;
  };
  auto runArm = [&](uint32_t bottomMat, uint32_t topMat) -> Arm {
    Arm a;
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    std::vector<CellOp> basin;
    auto put = [&](int x, int y, int z, uint32_t m) {
      basin.push_back({World::SlotCellIndex({x, y, z}), (uint32_t)(m & 0xFFFu)});
    };
    for (int z = -R - 1; z <= R + 1; z++)
      for (int x = -R - 1; x <= R + 1; x++) {
        put(px + x, py - 1, pz + z, kMatGlass);
        bool rim = (x < -R || x > R || z < -R || z > R);
        for (int y = 0; y < H; y++)
          put(px + x, py + y, pz + z, rim ? kMatGlass : kMatAir);
      }
    uint32_t fluidN = 0;
    uint32_t ft = 40000;
    std::vector<uint32_t> buf;
    int check = 0;
    for (int i = 0; i <= kLastTick; i++) {
      std::vector<FluidSpawnOp> fs;
      if (i >= 1 && i <= kSpawnTicks) {
        fs = spawnRow(i - 1, bottomMat, topMat);
        for (const FluidSpawnOp& op : fs)
          (op.mat == acidId ? a.acidSpawned : a.waterSpawned)++;
      }
      SubmitTick(ctx, world, sim, ++ft, kDefaultSeed, {}, {},
                 i == 0 ? basin : std::vector<CellOp>{}, false, {6, 7, 6},
                 /*wantReadback=*/false, /*particlesActive=*/false, {}, 0, fs,
                 fluidN);
      fluidN += (uint32_t)fs.size();
      ctx.WaitIdle();
      ctx.ProcessEvents();
      if (check >= kNumChecks || i != kChecks[check]) continue;
      uint32_t fa[16] = {};
      rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0, fa,
                            64, "fluidIdArgs");
      const uint32_t live = std::min(fa[7], kFluidCap);
      buf.assign((size_t)live * kFluidParticleWords, 0u);
      if (live > 0)
        rhi::ReadbackBlocking(ctx.device, ctx.queue,
                              world.fluidParticles[sim.Page()], 0, buf.data(),
                              buf.size() * 4, "fluidId");
      double ay = 0, wy = 0;
      for (uint32_t k = 0; k < live; k++) {
        const uint32_t* p = &buf[(size_t)k * kFluidParticleWords];
        const uint32_t m = p[18] & 0xFFFu;
        const double y = (double)(int32_t)p[1] / 65536.0;
        if (m == acidId) { ay += y; a.acidN[check]++; }
        else if (m == waterId) { wy += y; a.waterN[check]++; }
      }
      if (a.acidN[check] && a.waterN[check])
        a.sep[check] = ay / a.acidN[check] - wy / a.waterN[check];
      check++;
    }
    uint64_t h = 1469598103934665603ull;   // the LAST checkpoint's live pool
    for (uint32_t w : buf) { h ^= w; h *= 1099511628211ull; }
    a.hash = h ^ ((uint64_t)(buf.size() / kFluidParticleWords) << 32);
    a.worldHash = HashWorldNow(ctx, world, sim, kDefaultSeed);
    return a;
  };

  const Arm over1 = runArm(waterId, acidId);   // acid poured onto water
  const Arm over2 = runArm(waterId, acidId);
  const Arm under = runArm(acidId, waterId);   // water poured onto acid

  // Mostly particles: at least half of each liquid, in BOTH arms.
  auto mostlyLive = [&](const Arm& a, int k) {
    return a.acidN[k] * 2 >= a.acidSpawned && a.waterN[k] * 2 >= a.waterSpawned;
  };
  std::string traj;
  int verdictAt = -1;   // the last checkpoint both arms are mostly particles
  for (int k = 0; k < kNumChecks; k++) {
    traj += Format(" t%d %+.2f/%+.2f (%u+%u)", kChecks[k], over1.sep[k],
                   under.sep[k], over1.acidN[k], over1.waterN[k]);
    if (mostlyLive(over1, k) && mostlyLive(under, k)) verdictAt = k;
  }
  // How much separation each arm has LOST since the first checkpoint. The
  // symmetric processes take the same from both; density takes more from A.
  double lossA = 0, lossB = 0;
  if (verdictAt > 0) {
    lossA = std::abs(over1.sep[0]) - std::abs(over1.sep[verdictAt]);
    lossB = std::abs(under.sep[0]) - std::abs(under.sep[verdictAt]);
  }
  const bool det = over1.hash == over2.hash && over1.worldHash == over2.worldHash;
  const bool orderOk = over1.sep[0] > 0.0 && under.sep[0] < 0.0;  // measure sane
  const bool sank = verdictAt > 0 && lossA > lossB + margin;
  const bool ok = det && orderOk && sank;
  std::printf(
      "fluid identity: %s (L %d, dip %d; sep acid-over-water / water-over-acid,"
      " cells [A live acid+water]:%s; by t%d A lost %.2f cells of separation, "
      "B %.2f, needs A > B + %.2f; acid particle hash %016llx %s, world %s)\n",
      ok ? "PASS" : "FAIL", L, D, traj.c_str(),
      verdictAt >= 0 ? kChecks[verdictAt] : -1, lossA, lossB, margin,
      (unsigned long long)over1.hash,
      over1.hash == over2.hash ? "matches" : "DIVERGED",
      over1.worldHash == over2.worldHash ? "matches" : "DIVERGED");
  detail = Format("loss A %.2f vs B %.2f (t%d), order %s, det %s", lossA, lossB,
                  verdictAt >= 0 ? kChecks[verdictAt] : -1,
                  orderOk ? "ok" : "WRONG", det ? "ok" : "DIVERGED");
  return ok ? Status::Pass : Status::Fail;
}

// ---- fluid-settle --------------------------------------------------------
// The settle converter in isolation (plan §7, Phase 2): pour MPM water into a
// stone basin, stop, and require the WHOLE pool to convert back to fullness
// voxels within a bounded tick count — zero live particles, zero active fluid
// blocks, and exact integer mass (spawned eighths == basin voxel eighths).
// Twice-run: the end-state world hash must match across two from-worldgen
// runs (the seam is inside the hashed domain now, so this is the determinism
// gate for its writes).
Status GateFluidSettle(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  uint32_t waterId = 0;
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == "water") { waterId = (uint32_t)i; break; }
  if (waterId == 0) { detail = "no 'water' material"; return Status::Fail; }

  // Pin the cycle at DIM DAWN (daylight ~15 of 255): freezing needs night
  // (day == 0) and evaporation needs minLight 120, so BOTH authored water
  // sinks are off and the mass audit is exact. Noon was the first attempt —
  // it blocked freezing but left evaporation live, and `seesSky` probes only
  // ONE cell up, so a roof does not stop it: this gate caught the sun
  // sipping 2-3 eighths of settled rim film per run before the phase moved
  // here. The seam's own flow counters (binned == settled == died ==
  // poured) were exact throughout — that is the separation this gate's two
  // different checks exist to make.
  Tuning dawn = CurrentTuning();
  dawn.dayNight.freeze = 1;
  dawn.dayNight.freezePhase = (int)(kDaySunrise + 1024u);
  Tuning saved = CurrentTuning();
  SetCurrentTuning(dawn);

  const int px = 96, py = 120, pz = 96, R = 8, H = 8;
  const int kMaxTicks = 400;
  uint32_t worldHash[2] = {0, 0};
  uint32_t spawned = 0, live = ~0u, blocks = ~0u;
  int settledAt = -1;
  uint32_t basinEighths = 0;
  uint32_t settledSum = 0, deadSum = 0, excitedSum = 0, binnedSum = 0;
  uint32_t pickedSum = 0, infeasibleSum = 0, unstableSum = 0;
  for (int run = 0; run < 2; run++) {
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    std::vector<CellOp> basin;
    auto put = [&](int x, int y, int z, uint32_t m) {
      basin.push_back({World::SlotCellIndex({x, y, z}), m & 0xFFFu});
    };
    // TWO-cell shell everywhere. MPM's separate-BC nodes live at cell
    // centres; a hard splash can push a particle fractionally through a
    // 1-cell wall at a corner (measured: 2 of 1280 eighths ended up outside
    // the audit box over a 130-tick settle). Two cells of stone is beyond
    // any single-substep reach. The roof also blocks needsSky reactions.
    for (int z = -R - 2; z <= R + 2; z++)
      for (int x = -R - 2; x <= R + 2; x++) {
        put(px + x, py - 1, pz + z, kMatStone);
        put(px + x, py - 2, pz + z, kMatStone);
        put(px + x, py + H, pz + z, kMatStone);
        put(px + x, py + H + 1, pz + z, kMatStone);
        bool rim = (x < -R || x > R || z < -R || z > R);
        for (int y = 0; y < H; y++)
          put(px + x, py + y, pz + z, rim ? kMatStone : kMatAir);
      }
    auto pourOps = [&](uint32_t t) {
      std::vector<FluidSpawnOp> fs;
      for (int cz = -2; cz < 2; cz++)
        for (int cy = 0; cy < 2; cy++)
          for (int cx = -2; cx < 2; cx++)
            for (int s = 0; s < 8; s++) {
              uint32_t h = ((t * 131u + (uint32_t)fs.size()) * 6271u + 12345u) *
                               747796405u + 2891336453u;
              FluidSpawnOp op{};
              op.px = ((px + cx) << 16) + ((s & 1) ? 49152 : 16384) +
                      (int32_t)(h % 8192u) - 4096;
              op.py = ((py + 4 + cy) << 16) + ((s & 2) ? 49152 : 16384) +
                      (int32_t)((h >> 13) % 8192u) - 4096;
              op.pz = ((pz + cz) << 16) + ((s & 4) ? 49152 : 16384) +
                      (int32_t)((h >> 19) % 8192u) - 4096;
              op.vy = -19661;
              op.mat = waterId;
              fs.push_back(op);
            }
      return fs;
    };

    // The interior mass audit, callable mid-run (see the timing probe below).
    // Fills `cells` with every interior water cell's packed (coord, word) so
    // consecutive audits can be diffed to the exact voxel when mass moves.
    auto auditBasin = [&](std::map<uint64_t, uint32_t>* cells) {
      uint32_t eighths = 0;
      if (cells) cells->clear();
      std::vector<uint32_t> cbuf2((size_t)kChunkVol);
      for (int cy = (py - 2) / 16; cy <= (py + H + 8) / 16; cy++)
        for (int cz2 = (pz - R - 2) / 16; cz2 <= (pz + R + 2) / 16; cz2++)
          for (int cx2 = (px - R - 2) / 16; cx2 <= (px + R + 2) / 16; cx2++) {
            ReadVoxelsSync(ctx, world, World::SlotChunkIndex({cx2, cy, cz2}),
                           1, cbuf2.data(), "settleVox");
            for (uint32_t i = 0; i < kChunkVol; i++) {
              int lx = (int)(i % 16) + cx2 * 16,
                  ly = (int)((i / 16) % 16) + cy * 16,
                  lz = (int)(i / 256) + cz2 * 16;
              if (lx < px - R || lx > px + R || lz < pz - R || lz > pz + R ||
                  ly < py || ly > py + H)
                continue;
              if ((cbuf2[i] & 0xFFFu) == waterId) {
                eighths += ((cbuf2[i] >> 12) & 0xFu) + 1u;
                if (cells)
                  (*cells)[((uint64_t)lx << 40) | ((uint64_t)ly << 20) |
                           (uint64_t)lz] = cbuf2[i];
              }
            }
          }
      return eighths;
    };

    uint32_t ft = 40000;
    uint32_t liveEst = 0;
    spawned = 0;
    settledAt = -1;
    settledSum = 0;
    deadSum = 0;
    excitedSum = 0;
    binnedSum = 0;
    pickedSum = 0;
    infeasibleSum = 0;
    unstableSum = 0;
    uint32_t lastCount = 0;
    std::map<uint64_t, uint32_t> prevCells;
    for (int i = 0; i < kMaxTicks; i++) {
      std::vector<FluidSpawnOp> fs;
      if (i >= 1 && i < 6) fs = pourOps((uint32_t)i);  // 5 ticks x 256
      SubmitTick(ctx, world, sim, ++ft, kDefaultSeed, {}, {},
                 i == 0 ? basin : std::vector<CellOp>{}, false, {6, 7, 6},
                 false, false, {}, 0, fs, liveEst);
      spawned += (uint32_t)fs.size();
      ctx.WaitIdle();
      ctx.ProcessEvents();
      if (i >= 6) {
        // Per-tick: the FA event counters clear at the top of every fluid
        // tick, so mass-flow bookkeeping (settled/dead/excited sums — the
        // audit that localizes any leak) must not skip a tick.
        uint32_t fa[32] = {};
        rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0,
                              fa, 128, "settleArgs");
        live = std::min(fa[7], kFluidCap);
        blocks = fa[3];
        settledSum += fa[10];
        deadSum += fa[8];
        excitedSum += fa[11];
        binnedSum += fa[15];
        // WP3's settle diagnosis, and why this gate can now say WHY it failed
        // rather than only that it did: blocks the scan picked, and how many
        // settleCheck turned down for an infeasible column (FA_SETREFUSED)
        // versus an excite-unstable result (FA_SETUNSTABLE). "0 settled" has
        // two opposite causes and this separates them.
        pickedSum += fa[13];
        infeasibleSum += fa[25];
        unstableSum += fa[26];
        liveEst = live;
        if (live == 0 && blocks == 0 && settledAt < 0) {
          settledAt = i;
          if (run == 1) {
            lastCount = auditBasin(&prevCells);
            std::printf("  at quiet (t%d): %u eighths standing\n", i,
                        lastCount);
          }
        }
        // Leak hunt: once quiet, diff the pool cell-by-cell every tick and
        // print the exact voxel transitions whenever the count moves — the
        // mass is provably particle-free at this point, so whatever changes
        // is the CA acting alone.
        if (run == 1 && settledAt >= 0 && i > settledAt) {
          std::map<uint64_t, uint32_t> cells;
          uint32_t n = auditBasin(&cells);
          if (n != lastCount) {
            std::printf("  t%d: %u -> %u eighths; diffs:\n", i, lastCount, n);
            for (auto& [k, w] : prevCells) {
              auto it = cells.find(k);
              uint32_t nw2 = it == cells.end() ? 0u : it->second;
              if (nw2 != w)
                std::printf("    (%d,%d,%d) %08x -> %08x\n",
                            (int)(k >> 40), (int)((k >> 20) & 0xFFFFF),
                            (int)(k & 0xFFFFF), w, nw2);
            }
            for (auto& [k, w] : cells)
              if (!prevCells.count(k))
                std::printf("    (%d,%d,%d) 00000000 -> %08x\n",
                            (int)(k >> 40), (int)((k >> 20) & 0xFFFFF),
                            (int)(k & 0xFFFFF), w);
            lastCount = n;
          }
          prevCells.swap(cells);
        }
        if (settledAt >= 0 && i >= settledAt + 20) break;  // +CA calm margin
      } else {
        liveEst = spawned;  // conservative until the first readback
      }
    }
    worldHash[run] = HashWorldNow(ctx, world, sim, kDefaultSeed);

    // Basin sweep: every spawned eighth must be standing water now.
    basinEighths = auditBasin(nullptr);
  }
  SetCurrentTuning(saved);

  bool settled = settledAt >= 0 && live == 0 && blocks == 0;
  bool massOk = basinEighths == spawned;
  bool det = worldHash[0] == worldHash[1];
  bool ok = settled && massOk && det && spawned > 0;
  std::printf(
      "fluid settle: %s (%u eighths poured -> %u settled voxel eighths, "
      "quiet at tick %d, %u live / %u blocks at end, flow: %u binned / "
      "%u settled / %u died / %u re-excited, picks: %u = %u infeasible + "
      "%u unstable + %u committed, world hash %s)\n",
      ok ? "PASS" : "FAIL", spawned, basinEighths, settledAt, live, blocks,
      binnedSum, settledSum, deadSum, excitedSum, pickedSum, infeasibleSum,
      unstableSum, pickedSum - infeasibleSum - unstableSum,
      det ? "matches" : "DIVERGED");
  detail = Format("%u poured, %u settled, quiet@%d, det %s", spawned,
                  basinEighths, settledAt, det ? "ok" : "DIVERGED");
  return ok ? Status::Pass : Status::Fail;
}

// ---- fluid-excite --------------------------------------------------------
// The excite converter (plan §7): a SEALED two-chamber stone box — settled
// water on an upper floor, an empty catch chamber below — has its floor plug
// carved out with sim.fluidExciteMode on. The unsupported water must convert
// to MPM particles (hydrostatically pre-compressed: J < 1 in the early
// drain), drain through the hole, and re-settle in the catch chamber, with
// exact mass and a twice-run-identical world hash. Sealed + noon-pinned so no
// authored reaction can eat water out of the audit.
Status GateFluidExcite(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  uint32_t waterId = 0;
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == "water") { waterId = (uint32_t)i; break; }
  if (waterId == 0) { detail = "no 'water' material"; return Status::Fail; }

  Tuning t = CurrentTuning();
  t.dayNight.freeze = 1;
  t.dayNight.freezePhase = (int)(kDaySunrise + 1024u);  // both water sinks off
  t.sim.fluidExciteMode = 1;  // the disturbance trigger under test
  // A SEALED box is adversarial for settling: with the default damping of 0
  // the drained pool rings between the walls indefinitely (measured: max
  // particle speed still ~15 vox/s after 200 ticks — nothing radiates out of a
  // closed chamber). Real damping is a look knob; the gate turns it up so the
  // drain's END STATE is reachable in a bounded run.
  t.sim.fluidDamping = 0.9f;
  // Pin every sim.fluid* parameter to its tuning_params.def default so the
  // gate is hermetic — its outcome must not depend on whatever the user last
  // dragged in the tuner. "Simply stock" meant "whatever tuning.json says",
  // and a hot-reloaded slider change broke the gate (BASELINE.md: cohesion
  // 32.9, attractDiff -1.08 from a tuner session).
  //
  // PINNING IS NOT OVERRIDING, and the difference is the plan's success
  // signal. THE OVERRIDE SET — parameters set to something OTHER than the
  // default because the gate cannot pass at stock — is shrinking: 7 before
  // WP2, 3 after it, 1 now. WP2 made stock CFL-honest and zero-tension, which
  // retired the stiffness/cohesion/attract overrides; WP3 retired the settle
  // trio's other two, because settleEps 6.0 and wakeSpeed 24.0 ARE stock now
  // (the at-rest speed floor scales with gravity and the owner's is 900).
  // Only fluidDamping above is left, and it is a property of the SEALED
  // geometry — nothing radiates out of a closed box — not a defect in the
  // defaults. Every line below is a pin at the .def value, not an override;
  // if one of them starts disagreeing with tuning_params.def, that is the
  // drift this block exists to catch.
  t.sim.fluidStiffness = 14000.0f;
  t.sim.fluidGravity = 900.0f;
  t.sim.fluidRestDensity = 8.0f;
  t.sim.fluidEosPower = 4;
  t.sim.fluidCohesion = 0.0f;
  t.sim.fluidAttractSame = 0.0f;
  t.sim.fluidAttractDiff = 0.0f;
  t.sim.fluidViscosity = 0.0f;
  t.sim.fluidFriction = 0.0f;
  t.sim.fluidSplashRate = 4.0f;
  t.sim.fluidSplashSpeed = 18.0f;
  t.sim.fluidSplashMaxDensity = 0.7f;
  t.sim.fluidSplashLife = 1.1f;
  t.sim.fluidSplashScaleIdx = 2;
  t.sim.fluidFoamRate = 90.0f;
  t.sim.fluidFoamCrestRate = 120.0f;
  t.sim.fluidTrappedMin = 1.5f;
  t.sim.fluidTrappedMax = 11.0f;
  t.sim.fluidCrestMin = 0.25f;
  t.sim.fluidCrestMax = 2.0f;
  t.sim.fluidFoamEnergyMin = 8.0f;
  t.sim.fluidFoamEnergyMax = 260.0f;
  t.sim.fluidFoamLife = 2.2f;
  t.sim.fluidFoamLifeMin = 0.5f;
  t.sim.fluidBubbleBuoyancy = 1.6f;
  t.sim.fluidFoamDrag = 0.72f;
  t.sim.fluidBubbleDensity = 1.05f;
  t.sim.fluidSprayDensity = 0.42f;
  t.sim.fluidFoamScaleIdx = 3;
  // WP3's knobs, pinned like the rest: the CFL substep budget, and the settle
  // trio whose values the WP3 sweep re-derived at the owner's gravity.
  t.sim.fluidSubsteps = 9;
  t.sim.fluidSettleEps = 6.0f;
  t.sim.fluidWakeSpeed = 24.0f;
  t.sim.fluidSettleTicks = 24;
  Tuning saved = CurrentTuning();
  SetCurrentTuning(t);
  // fluidDamping is a WGSL const (folded into the kernels at compile time —
  // the sim.fluid* human-unit exception), so unlike the CPU-read knobs above
  // it only takes effect through a shader reload: the F5 path, run here for
  // the same reason F5 exists.
  sim.ReloadShaders(ctx.device);

  // Box: interior x,z in [-6,6] around (96,·,96); DOUBLE outer shell (the
  // settle gate's wall-leak lesson), catch chamber 110..119, upper floor
  // y=120 with a 4x4 plug at the centre, water 121..123 (3 deep, full),
  // double roof 126..127.
  // upperY names the LOWER of the two internal-floor layers (119, 120): a
  // 1-cell internal floor let particles embed in it and quiver forever.
  const int px = 96, pz = 96, RB = 8;
  const int floorY = 109, upperY = 119, roofY = 126;
  const int kCarveTick = 30, kMaxTicks = 340;
  const uint32_t kWaterEighths = 13u * 13u * 3u * 8u;  // 4056

  uint32_t worldHash[2] = {0, 0};
  uint32_t excitedSum = 0, live = ~0u, blocks = ~0u, endEighths = 0;
  uint32_t compressed = 0, sampled = 0, liveEighths = 0;
  uint32_t setBlocksSum = 0;  // settle picks over the run (refusal-loop probe)
  uint32_t infeasibleSum = 0, unstableSum = 0;  // and why they were refused
  uint32_t exBinned = 0, exSettled = 0, exDied = 0;  // seam flow ledger
  uint32_t endWide = 0;   // water voxels anywhere in the box, walls included
  int32_t endMaxS2 = -1;      // max (v>>8)^2 at end (never-calm probe)
  for (int run = 0; run < 2; run++) {
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    std::vector<CellOp> box;
    auto put = [&](int x, int y, int z, uint32_t m, uint32_t state = 0) {
      box.push_back({World::SlotCellIndex({x, y, z}),
                     (m & 0xFFFu) | (state << 12)});
    };
    for (int y = floorY - 1; y <= roofY + 1; y++)
      for (int z = -RB; z <= RB; z++)
        for (int x = -RB; x <= RB; x++) {
          bool shell = x <= -RB + 1 || x >= RB - 1 || z <= -RB + 1 ||
                       z >= RB - 1 || y <= floorY || y >= roofY ||
                       y == upperY || y == upperY + 1;
          if (shell) {
            put(px + x, y, pz + z, kMatStone);
          } else if (y >= upperY + 2 && y <= upperY + 4) {
            put(px + x, y, pz + z, waterId, 7u);  // full water
          } else {
            put(px + x, y, pz + z, kMatAir);
          }
        }
    std::vector<CellOp> carve;
    for (int z = -2; z < 2; z++)
      for (int x = -2; x < 2; x++) {
        carve.push_back({World::SlotCellIndex({px + x, upperY, pz + z}), 0u});
        carve.push_back(
            {World::SlotCellIndex({px + x, upperY + 1, pz + z}), 0u});
      }

    uint32_t ft = 50000;
    uint32_t liveEst = 0;
    excitedSum = 0;
    compressed = 0;
    sampled = 0;
    for (int i = 0; i < kMaxTicks; i++) {
      std::vector<CellOp> cops;
      if (i == 0) cops = box;
      else if (i == kCarveTick) cops = carve;
      SubmitTick(ctx, world, sim, ++ft, kDefaultSeed, {}, {}, cops, false,
                 {6, 7, 6}, false, false, {}, 0, {}, liveEst);
      ctx.WaitIdle();
      ctx.ProcessEvents();
      if (i >= kCarveTick) {
        uint32_t fa[32] = {};
        rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0,
                              fa, 128, "exciteArgs");
        live = std::min(fa[7], kFluidCap);
        blocks = fa[3];
        excitedSum += fa[11];
        setBlocksSum += fa[13];
        // WP3's refusal split (FA_SETREFUSED / FA_SETUNSTABLE): "picked but
        // nothing settled" has two opposite causes and this is the only thing
        // that can tell them apart.
        infeasibleSum += fa[25];
        unstableSum += fa[26];
        // The seam flow ledger, which localises a mass discrepancy: binned
        // != settled means the column walk dropped it, settled != the voxel
        // sweep means the writes were lost.
        exBinned += fa[15]; exSettled += fa[10]; exDied += fa[8];
        if (run == 1 && i % 40 == 0)
          std::printf("  excite t%d: live %u, blocks %u, excited+ %u, "
                      "picks+ %u (%u infeasible, %u unstable)\n",
                      i, live, blocks, fa[11], fa[13], fa[25], fa[26]);
        // Exact one-tick-stale count; the exciteMode predicate keeps the
        // seam recording even at zero while the CA is active.
        liveEst = live;
        if (i > kCarveTick + 60 && live == 0 && blocks == 0) break;
        // Hydrostatic check, early in the drain: excited particles seeded
        // below a submerged surface must carry J < 1 (pre-compression).
        if (i == kCarveTick + 2 && live > 0 && run == 1) {
          uint32_t n = std::min(live, 256u);
          std::vector<uint32_t> pbuf((size_t)n * kFluidParticleWords);
          rhi::ReadbackBlocking(ctx.device, ctx.queue,
                                world.fluidParticles[sim.Page()], 0,
                                pbuf.data(), pbuf.size() * 4, "exciteJ");
          for (uint32_t k = 0; k < n; k++) {
            int32_t j = (int32_t)pbuf[k * kFluidParticleWords + 15];
            sampled++;
            if (j < 65536 - 1024) compressed++;
          }
        }
      } else {
        liveEst = 0;
      }
    }
    worldHash[run] = HashWorldNow(ctx, world, sim, kDefaultSeed);

    // End-state particle sweep: the live pool's fullness (for the exact mass
    // audit) plus the residual-churn diagnostics.
    liveEighths = 0;
    if (live > 0) {
      uint32_t n = std::min(live, kFluidCap);
      std::vector<uint32_t> pbuf((size_t)n * kFluidParticleWords);
      rhi::ReadbackBlocking(ctx.device, ctx.queue,
                            world.fluidParticles[sim.Page()], 0, pbuf.data(),
                            pbuf.size() * 4, "exciteEndV");
      endMaxS2 = 0;
      uint32_t fast = 0;
      // THE AT-REST FLOOR PROBE (WP3 item 4, how the settle trio is derived).
      // Report the residual speed BOTH ways: raw, and with the free-surface
      // gravity bias stripped exactly as `seamRestVy` strips it in the shader.
      // A weakly-compressible free surface carries one substep of gravity
      // forever, so the RAW number is a property of `gravity/substeps` and
      // tells you nothing about whether the water is moving; the STRIPPED one
      // is the quantity `settleEps` is supposed to be compared against, and it
      // is what makes the threshold mean the same thing at any gravity.
      const int32_t gSub =
          (int32_t)std::lround(t.sim.fluidGravity * 65536.0 / 900.0) /
          std::max(1, t.sim.fluidSubsteps);
      int32_t restMaxS2 = 0;
      uint32_t restFast = 0;
      for (uint32_t k = 0; k < n; k++) {
        const int32_t* p = (const int32_t*)&pbuf[k * kFluidParticleWords];
        liveEighths += ((uint32_t)p[18] >> 12) & 0x7u;
        int32_t sx = p[3] >> 8, sy = p[4] >> 8, sz = p[5] >> 8;
        int32_t s2 = sx * sx + sy * sy + sz * sz;
        if (s2 > 49) fast++;
        endMaxS2 = std::max(endMaxS2, s2);
        int32_t ry = std::min(std::abs(p[4] + gSub), std::abs(p[4])) >> 8;
        int32_t r2 = sx * sx + ry * ry + sz * sz;
        if (r2 > 49) restFast++;
        restMaxS2 = std::max(restMaxS2, r2);
      }
      if (run == 1)
        std::printf("  end: %u live carrying %u eighths; raw max %.2f vox/s "
                    "(%u above 0.9), rest-frame max %.2f vox/s (%u above "
                    "0.9), one substep of g = %.2f vox/s\n",
                    n, liveEighths, std::sqrt((double)endMaxS2) * 30.0 / 256.0,
                    fast, std::sqrt((double)restMaxS2) * 30.0 / 256.0, restFast,
                    (double)(gSub >> 8) * 30.0 / 256.0);
    }

    // Mass audit over the whole sealed interior (both chambers): standing
    // water eighths at the end must equal the eighths placed at the start.
    endEighths = 0;
    endWide = 0;
    std::vector<uint32_t> cbuf((size_t)kChunkVol);
    for (int cy = (floorY - 1) / 16; cy <= (roofY + 1) / 16; cy++)
      for (int cz2 = (pz - RB) / 16; cz2 <= (pz + RB) / 16; cz2++)
        for (int cx2 = (px - RB) / 16; cx2 <= (px + RB) / 16; cx2++) {
          ReadVoxelsSync(ctx, world, World::SlotChunkIndex({cx2, cy, cz2}), 1,
                         cbuf.data(), "exciteVox");
          for (uint32_t i = 0; i < kChunkVol; i++) {
            int lx = (int)(i % 16) + cx2 * 16,
                ly = (int)((i / 16) % 16) + cy * 16,
                lz = (int)(i / 256) + cz2 * 16;
            // WIDE sweep first: every water voxel anywhere in the box, walls
            // included. If the interior audit comes up short but this does
            // not, the mass was written somewhere the interior test does not
            // look -- an audit-scope artifact, not a destroyed eighth.
            if ((cbuf[i] & 0xFFFu) == waterId)
              endWide += ((cbuf[i] >> 12) & 0xFu) + 1u;
            if (lx < px - RB + 2 || lx > px + RB - 2 || lz < pz - RB + 2 ||
                lz > pz + RB - 2 || ly <= floorY || ly >= roofY)
              continue;
            if ((cbuf[i] & 0xFFFu) == waterId)
              endEighths += ((cbuf[i] >> 12) & 0xFu) + 1u;
          }
        }
  }
  SetCurrentTuning(saved);
  sim.ReloadShaders(ctx.device);  // restore the default-tuning kernels

  // What Phase 2 must prove here: the disturbance excited a real drain with
  // hydrostatic seeding, the mass account is EXACT across every conversion
  // (standing voxels + live fullness == the water placed), MOST of the water
  // made it back to settled voxels, and the whole story is twice-run
  // identical. Full quiescence of a sealed box is deliberately NOT asserted:
  // a ~1% residual fountain near the hole is solver churn (documented in
  // DESIGN.md), and the zero-live end state is already gated by fluid-settle
  // and fluid-det on gentler geometry.
  bool excited = excitedSum > 3000;        // the basin genuinely drained
  bool hydro = sampled > 0 && compressed > 0;
  bool massOk = endEighths + liveEighths == kWaterEighths;
  bool resettled = endEighths > kWaterEighths * 3u / 4u && live < 800;
  bool det = worldHash[0] == worldHash[1];
  bool ok = excited && hydro && massOk && resettled && det;
  std::printf("  seam flow: %u binned / %u settled / %u died; interior %u + "
              "live %u = %u of %u (wide sweep %u)\n",
              exBinned, exSettled, exDied, endEighths, liveEighths,
              endEighths + liveEighths, kWaterEighths, endWide);
  std::printf(
      "fluid excite: %s (%u eighths excited over the drain, %u/%u sampled "
      "particles pre-compressed, %u standing + %u live eighths of %u, "
      "%u live / %u blocks at end, %u settle picks (%u infeasible, "
      "%u unstable), end max s2 %d, world hash %s)\n",
      ok ? "PASS" : "FAIL", excitedSum, compressed, sampled, endEighths,
      liveEighths, kWaterEighths, live, blocks, setBlocksSum, infeasibleSum,
      unstableSum, endMaxS2, det ? "matches" : "DIVERGED");
  detail = Format("%u excited, %u/%u compressed, mass %u+%u/%u, det %s",
                  excitedSum, compressed, sampled, endEighths, liveEighths,
                  kWaterEighths, det ? "ok" : "DIVERGED");
  return ok ? Status::Pass : Status::Fail;
}

// ---- fluid-onwater -------------------------------------------------------
// PARTICLE WATER MEETS SETTLED WATER — the one question none of the five lab
// pour scenes can ask, because every one of them pours into an EMPTY basin.
//
// Up to WP4 the answer was "they ignore each other". sim_fluid.wgsl's
// fluidSolid() blocks solids and powders only, and a fullness voxel scatters
// nothing into the node grid because it has no particles, so MPM water poured
// onto a basin filled to the rim fell straight to the BED as if the basin were
// empty. sim.fluidSettledMass is the fix: a settled cell seeds its node with
// fullness/8 * restDensity of zero-velocity mass, so the pool has density and
// the pour floats on it.
//
// THE ASSERTION IS A DIFFERENTIAL, not an absolute depth, and that is what
// makes it hard to fool. A sealed box, a bed of stone, eight full layers of
// SETTLED water above it, and a burst of particles dropped in from the air
// pocket at the top. The gate runs the same pour TWICE — once with
// fluidSettledMass 0 (WP4's pass-through, the control) and once at the shipped
// 1.0 — and requires that the pool measurably holds the pour UP.
//
// An absolute threshold was tried first and is the wrong instrument: this is a
// weakly-compressible solver, the jet arrives near its own CFL ceiling, and the
// static boundary cannot be shoved aside the way real water would be, so a fast
// pour legitimately punches a deep crater before pressure throws it back. What
// must never happen — and what WP4 did every time — is the pour reaching the
// BED as though the basin were empty. The control arm measures exactly that, in
// the same geometry, on the same build.
//
// exciteMode is pinned OFF, and note what that does NOT switch off: the seam's
// WAKE trigger is unconditional (sim_fluid_seam.wgsl exciteDetect — only the
// fall and perch triggers are behind fluidExciteEnable), because a disturbance
// reaching settled water has to be able to propagate whatever mode the world is
// in. So the pour can still excite the pool it lands on, and the CONTROL ARM
// shows why that mattered so much: with no boundary mass the pour falls THROUGH
// the pool, every cell it passes has a fast node face-adjacent to it, and the
// wake cascades down the whole column — the measured control converts 8,136 of
// the box's 8,144 eighths, i.e. the entire basin, which is the reported "a
// waterfall turns the whole lake into fluid". The shipped arm converts ~1,300:
// a crater at the point of impact, which is what a splash should do.
//
// SECOND ASSERTION, and it is the sharper one: the pool must still BE a pool
// afterwards. `peakLive` bounds how much of the box was ever particles at once.
//
// (The control arm's deepest-particle reading lands below the bed — a fully
// converted, over-pressured sealed box leaks particles through a 2-cell shell.
// That is a pre-existing containment weakness under pathological pressure, the
// same one the fluid-excite gate's double shell exists to keep out of ITS
// audit; here it is only the control, whose mass is deliberately not audited.)
Status GateFluidOnWater(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  uint32_t waterId = 0;
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == "water") { waterId = (uint32_t)i; break; }
  if (waterId == 0) { detail = "no 'water' material"; return Status::Fail; }

  Tuning t = CurrentTuning();
  t.dayNight.freeze = 1;
  t.dayNight.freezePhase = (int)(kDaySunrise + 1024u);  // both water sinks off
  t.sim.fluidExciteMode = 0;   // see the header: the pool must stay settled
  t.sim.fluidSubsteps = 9;
  t.sim.fluidStiffness = 14000.0f;
  t.sim.fluidGravity = 900.0f;
  t.sim.fluidRestDensity = 8.0f;
  t.sim.fluidEosPower = 4;
  t.sim.fluidCohesion = 0.0f;
  t.sim.fluidViscosity = 0.0f;
  t.sim.fluidDamping = 0.0f;
  t.sim.fluidFriction = 0.0f;
  Tuning saved = CurrentTuning();

  // Box: interior x,z in [-5,5] around (96,·,96), double stone shell. Bed at
  // bedY, settled water bedY+1 .. bedY+8 (8 full layers), air above it, roof.
  const int px = 96, pz = 96, RB = 7;
  const int bedY = 110, roofY = 127;
  const int waterTop = bedY + 8;      // 118: last settled layer
  const int pourY = 124;              // the air pocket, 6 cells clear of it
  const int kPourTick = 4, kMaxTicks = 150;
  const int kIn = RB - 2;             // interior half-extent (5)
  const uint32_t kSettledEighths = (uint32_t)((2 * kIn + 1) * (2 * kIn + 1)) * 8u * 8u;

  uint32_t worldHash[2] = {0, 0};
  uint32_t poured = 0, live = 0, liveEighths = 0, endEighths = 0;
  int minParticleY = 1 << 30;     // deepest particle seen after the pour lands
  int minSampleTick = -1;
  // run 0 = the CONTROL (fluidSettledMass 0, i.e. WP4's pass-through);
  // runs 1 and 2 = the shipped 1.0, twice, so the hash comparison still has
  // two matching arms to compare.
  int controlMinY = 1 << 30;
  uint32_t peakLive = 0, controlPeakLive = 0;
  for (int run = 0; run < 3; run++) {
    t.sim.fluidSettledMass = run == 0 ? 0.0f : 1.0f;
    SetCurrentTuning(t);
    // sim.fluid* are WGSL consts folded in at compile time (the human-unit
    // exception in CLAUDE.md), so the arm switch is a shader reload — the F5
    // path, used here for the reason F5 exists.
    sim.ReloadShaders(ctx.device);
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    std::vector<CellOp> box;
    auto put = [&](int x, int y, int z, uint32_t m, uint32_t state = 0) {
      box.push_back({World::SlotCellIndex({x, y, z}),
                     (m & 0xFFFu) | (state << 12)});
    };
    for (int y = bedY - 1; y <= roofY + 1; y++)
      for (int z = -RB; z <= RB; z++)
        for (int x = -RB; x <= RB; x++) {
          const bool shell = x <= -RB + 1 || x >= RB - 1 || z <= -RB + 1 ||
                             z >= RB - 1 || y <= bedY || y >= roofY;
          if (shell) put(px + x, y, pz + z, kMatStone);
          else if (y <= waterTop) put(px + x, y, pz + z, waterId, 7u);
          else put(px + x, y, pz + z, kMatAir);
        }

    // The pour: a 5x5x2 block of cells' worth of particles at the 2x2x2
    // sub-cell lattice, the same seeding exciteEmit uses, dropped with a
    // downward kick so they arrive with real momentum rather than drifting on.
    std::vector<FluidSpawnOp> pour;
    for (int y = 0; y < 2; y++)
      for (int z = -2; z <= 2; z++)
        for (int x = -2; x <= 2; x++)
          for (int k = 0; k < 8; k++) {
            FluidSpawnOp op{};
            op.px = ((px + x) << 16) + ((k & 1) ? 49152 : 16384);
            op.py = ((pourY + y) << 16) + ((k & 2) ? 49152 : 16384);
            op.pz = ((pz + z) << 16) + ((k & 4) ? 49152 : 16384);
            op.vx = 0;
            op.vy = -65536;   // 1 cell/tick down = 30 vox/s
            op.vz = 0;
            op.mat = waterId;
            pour.push_back(op);
          }
    poured = (uint32_t)pour.size();

    uint32_t ft = 50000;
    uint32_t liveEst = 0;
    minParticleY = 1 << 30;
    peakLive = 0;
    for (int i = 0; i < kMaxTicks; i++) {
      std::vector<CellOp> cops;
      if (i == 0) cops = box;
      std::vector<FluidSpawnOp> sp;
      if (i == kPourTick) sp = pour;
      SubmitTick(ctx, world, sim, ++ft, kDefaultSeed, {}, {}, cops, false,
                 {6, 7, 6}, false, false, {}, 0, sp, liveEst);
      ctx.WaitIdle();
      ctx.ProcessEvents();
      if (i < kPourTick) { liveEst = 0; continue; }
      uint32_t fa[32] = {};
      rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0, fa,
                            128, "onWaterArgs");
      live = std::min(fa[7], kFluidCap);
      liveEst = live;
      peakLive = std::max(peakLive, live);
      // The depth probe. Sampled from the tick the pour has had time to reach
      // the surface (a 6-cell fall at 1 cell/tick plus gravity) to the end.
      if (i >= kPourTick + 10 && live > 0) {
        uint32_t n = std::min(live, kFluidCap);
        std::vector<uint32_t> pbuf((size_t)n * kFluidParticleWords);
        rhi::ReadbackBlocking(ctx.device, ctx.queue,
                              world.fluidParticles[sim.Page()], 0, pbuf.data(),
                              pbuf.size() * 4, "onWaterP");
        uint32_t alive = 0, belowBed = 0;
        int tickMin = 1 << 30;
        for (uint32_t k = 0; k < n; k++) {
          const uint32_t* p = &pbuf[k * kFluidParticleWords];
          if (((p[18] & 0xFFFu) == 0u) || (((p[18] >> 12) & 0x7u) == 0u))
            continue;  // fpAlive
          alive++;
          const int y = (int)((int32_t)p[1] >> 16);
          if (y < bedY) belowBed++;
          if (y < tickMin) tickMin = y;
          if (y < minParticleY) { minParticleY = y; minSampleTick = i; }
        }
        if (i % 30 == 0)
          std::printf("  on-water arm%d t%d: live %u alive %u minY %d "
                      "belowBed %u\n",
                      run, i, live, alive, tickMin, belowBed);
      }
    }
    if (run == 0) {
      controlMinY = minParticleY;
      controlPeakLive = peakLive;
      continue;
    }
    worldHash[run - 1] = HashWorldNow(ctx, world, sim, kDefaultSeed);

    liveEighths = 0;
    if (live > 0) {
      uint32_t n = std::min(live, kFluidCap);
      std::vector<uint32_t> pbuf((size_t)n * kFluidParticleWords);
      rhi::ReadbackBlocking(ctx.device, ctx.queue,
                            world.fluidParticles[sim.Page()], 0, pbuf.data(),
                            pbuf.size() * 4, "onWaterEnd");
      for (uint32_t k = 0; k < n; k++)
        liveEighths += (pbuf[k * kFluidParticleWords + 18] >> 12) & 0x7u;
    }

    // Standing water over the sealed interior, for the mass audit.
    endEighths = 0;
    std::vector<uint32_t> cbuf((size_t)kChunkVol);
    for (int cy = (bedY - 1) / 16; cy <= (roofY + 1) / 16; cy++)
      for (int cz2 = (pz - RB) / 16; cz2 <= (pz + RB) / 16; cz2++)
        for (int cx2 = (px - RB) / 16; cx2 <= (px + RB) / 16; cx2++) {
          ReadVoxelsSync(ctx, world, World::SlotChunkIndex({cx2, cy, cz2}), 1,
                         cbuf.data(), "onWaterVox");
          for (uint32_t j = 0; j < kChunkVol; j++)
            if ((cbuf[j] & 0xFFFu) == waterId)
              endEighths += ((cbuf[j] >> 12) & 0xFu) + 1u;
        }
  }
  SetCurrentTuning(saved);
  sim.ReloadShaders(ctx.device);

  // TWO conditions on the depth, and they fail for different reasons.
  //   * `held`   — the pool must hold the pour measurably higher than no pool
  //     at all. This is the mechanism test, and the control arm supplies its
  //     own reference on this build, this GPU, this tuning.
  //   * `offBed` — and it must not reach the bed. The control arm lands ON the
  //     bed, so this is the user-visible statement: water poured into a full
  //     basin does not end up underneath it.
  const int kFloor = bedY + 2;
  const uint32_t kTotal = kSettledEighths + poured;
  bool held = minParticleY > controlMinY;
  bool offBed = minParticleY >= kFloor;
  // The pool must survive as a pool: less than half the box may ever be
  // particles at once. Measured 1,274 of 8,144 here, against a control of
  // 8,136 — the whole basin.
  bool stayedPool = peakLive * 2u < kTotal;
  bool massOk = endEighths + liveEighths == kTotal;
  bool det = worldHash[0] == worldHash[1];
  bool ok = held && offBed && stayedPool && massOk && det;
  std::printf(
      "fluid on-water: %s (%u particles poured onto %u settled eighths; "
      "deepest particle y %d at t%d vs control (settledMass 0) y %d, bed %d, "
      "floor %d; peak live %u vs control %u of %u total; %u standing + %u live "
      "= %u of %u; world hash %s)\n",
      ok ? "PASS" : "FAIL", poured, kSettledEighths, minParticleY,
      minSampleTick, controlMinY, bedY, kFloor, peakLive, controlPeakLive,
      kTotal, endEighths, liveEighths, endEighths + liveEighths, kTotal,
      det ? "matches" : "DIVERGED");
  detail =
      Format("deepest y %d vs control %d (bed %d), peak live %u vs %u of %u, "
             "mass %u+%u/%u, det %s",
             minParticleY, controlMinY, bedY, peakLive, controlPeakLive, kTotal,
             endEighths, liveEighths, kTotal, det ? "ok" : "DIVERGED");
  return ok ? Status::Pass : Status::Fail;
}

// ---- debris-float --------------------------------------------------------
// DEBRIS MEETS A POND (docs/PLAN_debris_buoyancy.md). Owner report, 2026-09-13:
// explode a tree over water and the chips build a structure ON the surface —
// they neither sink nor float, they stop dead on the film and stack.
//
// The fixture is a sealed basin with eight settled layers of water, and it runs
// BOTH populations against it in one boot, because the interesting property is
// that they part ways:
//
//   * IRON (7600) must end up UNDER the waterline, on the bed. Reaching the bed
//     is the half that needs the "may displace a liquid it is denser than" rule
//     in resolve — the cell a sinking voxel comes to rest in is water, not air,
//     and without that rule it proposes a cell it can never be given, forever.
//   * WOOD (600) must end up AT the waterline and nowhere else. Two thresholds,
//     not one: too deep means buoyancy is not holding it up, and too HIGH is
//     the reported bug — a chip that stopped on the film, or a second chip that
//     stacked on the first.
//
// Both arms come from the same `density` field with nothing authored per
// material, which is the design claim as much as it is the fixture's
// convenience. The particle count at the end is the rule-2 half: a floater that
// never settles is a live particle forever, so "the ring drained" is the
// statement that the bob terminates.
//
// The BODY arm asks the same question of the other representation. A felled
// trunk is not particles — it is one Jolt body, and liquids are not in the
// collider, so a wooden body used to sink through a lake exactly like an iron
// one. The iron cube is the control, and in this fixture it is a STARK one:
// the basin is written as CellOps into a harness world that never builds a
// terrain collider under it, so the iron cube falls through the bed, through
// the floor and out of the bottom of the world (measured: y -31 after 66
// ticks). The wood cube, dropped beside it in the same tick from the same
// height, stops at y 116. Nothing in this fixture can hold either of them up
// except the water, which makes the differential exactly the claim: the only
// thing between the wood and the same fate is Archimedes.
//
// "NOTHING CAN HOLD THEM UP" IS A PROPERTY OF THE DROP COLUMNS, not of the
// harness, and the sentence above read as the second until 2026-09-14, when a
// raft of settled wood CHIPS caught the iron cube and held it at y120.7. Where
// the cubes are dropped is load-bearing; see the note over the `cube` calls.
//
// Both thresholds also hold in the other regime, so a future harness that DOES
// build collision here does not silently invert the test — the iron would rest
// on the bed at 111, still under `bedY + 4`.
Status GateDebrisFloat(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  uint32_t waterId = 0, woodId = 0, ironId = 0;
  for (size_t i = 0; i < c.mats.size(); i++) {
    if (c.mats[i].name == "water") waterId = (uint32_t)i;
    else if (c.mats[i].name == "wood") woodId = (uint32_t)i;
    else if (c.mats[i].name == "iron") ironId = (uint32_t)i;
  }
  if (!waterId || !woodId || !ironId) {
    detail = "need materials water/wood/iron";
    return Status::Fail;
  }

  const int px = 96, pz = 96, RB = 7;
  const int bedY = 110, roofY = 127;
  const int waterTop = bedY + 8;   // 118: the topmost settled layer
  const int dropY = 124;           // the air pocket above it
  const int kSpawnTick = 6, kTicks = 320;

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  std::vector<CellOp> box;
  auto put = [&](int x, int y, int z, uint32_t m, uint32_t state = 0) {
    box.push_back({World::SlotCellIndex({x, y, z}),
                   (m & 0xFFFu) | (state << 12)});
  };
  for (int y = bedY - 1; y <= roofY + 1; y++)
    for (int z = -RB; z <= RB; z++)
      for (int x = -RB; x <= RB; x++) {
        const bool shell = x <= -RB + 1 || x >= RB - 1 || z <= -RB + 1 ||
                           z >= RB - 1 || y <= bedY || y >= roofY;
        if (shell) put(px + x, y, pz + z, kMatStone);
        else if (y <= waterTop) put(px + x, y, pz + z, waterId, 7u);
        else put(px + x, y, pz + z, kMatAir);
      }

  // One voxel per column, columns kept apart, so a landing site is never
  // contested: this gate is about where matter COMES TO REST, and two chips
  // arguing over one cell is the claim system's business, tested elsewhere.
  std::vector<ParticleSpawn> drop;
  auto chip = [&](int x, int z, uint32_t mat) {
    ParticleSpawn s{};
    s.px = ((px + x) << 8) + 128;
    s.py = (dropY << 8) + 128;
    s.pz = ((pz + z) << 8) + 128;
    s.vy = -64;  // a quarter voxel a tick: arriving, not drifting
    s.payload = mat & 0xFFFu;
    s.flags = kPFlagAlive;
    drop.push_back(s);
  };
  int nIron = 0, nWood = 0;
  for (int z = -4; z <= -2; z++)
    for (int x = -4; x <= -2; x++) { chip(x, z, ironId); nIron++; }
  for (int z = 2; z <= 4; z++)
    for (int x = 2; x <= 4; x++) { chip(x, z, woodId); nWood++; }

  uint32_t t = 60000;
  for (int i = 0; i < kTicks; i++) {
    std::vector<CellOp> cops;
    if (i == 0) cops = box;
    std::vector<ParticleSpawn> sp;
    if (i == kSpawnTick) sp = drop;
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, cops, false,
               {6, 7, 6}, false, i >= kSpawnTick, sp);
    ctx.WaitIdle();
    ctx.ProcessEvents();
  }

  uint32_t counts[2] = {};
  ReadCountsSync(ctx, world, counts);
  const uint32_t liveEnd = std::min(counts[sim.Page()], kParticleCap);

  // Where did each population come to rest?
  int ironLo = 1 << 30, ironHi = -(1 << 30), woodLo = 1 << 30,
      woodHi = -(1 << 30);
  int ironCells = 0, woodCells = 0;
  std::vector<uint32_t> cbuf((size_t)kChunkVol);
  for (int cy = (bedY - 1) / 16; cy <= (roofY + 1) / 16; cy++)
    for (int cz = (pz - RB) / 16; cz <= (pz + RB) / 16; cz++)
      for (int cx = (px - RB) / 16; cx <= (px + RB) / 16; cx++) {
        ReadVoxelsSync(ctx, world, World::SlotChunkIndex({cx, cy, cz}), 1,
                       cbuf.data(), "debrisFloatVox");
        for (uint32_t j = 0; j < kChunkVol; j++) {
          const uint32_t m = cbuf[j] & 0xFFFu;
          if (m != woodId && m != ironId) continue;
          const int y = cy * 16 + (int)((j / 16) % 16);
          if (m == ironId) {
            ironCells++;
            ironLo = std::min(ironLo, y);
            ironHi = std::max(ironHi, y);
          } else {
            woodCells++;
            woodLo = std::min(woodLo, y);
            woodHi = std::max(woodHi, y);
          }
        }
      }

  // ---- the body arm --------------------------------------------------------
  // Two 3x3x3 cubes dropped into the same basin, one of each material. The
  // debris system has to be TICKED for this: PreTick is what pulls the basin
  // into the CPU mirror, and FloatBodies reads the waterline out of that
  // mirror. Without the warm-up loop below there is no water as far as the
  // buoyancy probe is concerned and BOTH cubes fall through.
  //
  // SAMPLED PER TICK, not at the end, and that is not a style choice: a body
  // that comes to rest on the bed is asleep 60 ticks later and SettleBodies
  // converts it back to voxels, at which point its handle is gone and asking
  // where it is answers "nowhere". The first cut of this arm read both centres
  // after the loop, got two dead handles, and reported the same -1 for a body
  // that had sunk and a body that had never been made.
  uint64_t hWood = 0, hIron = 0;
  float woodLowest = 1e9f, ironLowest = 1e9f;
  int woodSeen = 0, ironSeen = 0;
  auto tickBodies = [&](int n) {
    for (int i = 0; i < n; i++) {
      std::vector<CellOp> cellOps;
      std::vector<ParticleSpawn> spawns;
      c.debris.PreTick(++t, world, cellOps, spawns);
      SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, cellOps, false,
                 {6, 7, 6}, false, false, spawns);
      ctx.WaitIdle();
      ctx.ProcessEvents();
      c.phys.Step(kTickDt);
      c.debris.PostStep();
      Vec3 com{};
      if (hWood && c.phys.BodyCenterOfMass(hWood, com)) {
        woodSeen++;
        woodLowest = std::min(woodLowest, com.y);
      }
      if (hIron && c.phys.BodyCenterOfMass(hIron, com)) {
        ironSeen++;
        ironLowest = std::min(ironLowest, com.y);
      }
    }
  };
  tickBodies(40);
  std::vector<float> density(c.mats.size(), 1000.0f);
  for (size_t i = 0; i < c.mats.size(); i++)
    density[i] = std::max(1.0f, (float)c.mats[i].gpu.density);
  auto cube = [&](uint32_t mat, int ox, int oz, uint64_t& handle) {
    std::vector<DebrisVoxel> vox;
    for (int8_t z = 0; z < 3; z++)
      for (int8_t y = 0; y < 3; y++)
        for (int8_t x = 0; x < 3; x++)
          vox.push_back(DebrisVoxel{x, y, z, 0, (uint16_t)mat});
    handle = c.phys.CreateDebrisBody(vox, {px + ox, dropY, pz + oz}, density);
    if (!handle) return false;
    BodyTransform xf{};
    xf.pos = Vec3{(float)(px + ox), (float)dropY, (float)(pz + oz)};
    xf.quat[3] = 1;
    c.debris.AdoptBody(handle, vox, xf);
    return true;
  };
  // z -1 puts both cubes in the band BETWEEN the two chip zones (chips occupy
  // z -4..-2 and z 2..4; a cube spans three cells from its corner, so -1 is
  // z -1..1 and touches neither). They used to be dropped at z 0, which is
  // z 0..2 and clips the wood chips' z 2 column -- and on 2026-09-14, the tick
  // floating debris stopped being blown about by the wind, that stopped being
  // harmless. The chips had previously come to rest scattered over y118..120
  // and the iron cube fell between them; once they settled into a proper flat
  // raft at the waterline the cube came to rest ON IT, at a centre of mass of
  // 120.7 = the raft's top at 119 plus half a cube. The arm went red while
  // reporting a number that had nothing to do with buoyancy.
  //
  // So this is a constraint on the fixture and not a tuning of it: THE BODY
  // ARM MUST BE DROPPED INTO A COLUMN THE PARTICLE ARM NEVER LANDS IN. The
  // whole claim below is "the only thing between the wood and the iron's fate
  // is Archimedes", and anything else a cube can rest on makes that a
  // measurement of the something else.
  const bool madeBodies = cube(woodId, -3, -1, hWood) && cube(ironId, 3, -1, hIron);
  tickBodies(200);
  const DebrisSystem::FloaterProbe& fp = c.debris.Floaters();

  // The four claims, each failing for its own reason.
  const bool sank = ironCells >= nIron - 1 && ironHi <= waterTop;
  const bool floated = woodCells >= nWood - 1 && woodLo >= waterTop &&
                       woodHi <= waterTop + 2;
  const bool drained = liveEnd == 0;
  // The body arm's thresholds are deliberately loose — it is asking "did these
  // two end up in DIFFERENT places", not "how deep". The measure is the LOWEST
  // each body ever got: a floating body that is later stamped back into the
  // grid still never dipped, and a sinking one is not rescued by whatever
  // happens to it afterwards. 5 voxels of daylight over the bed is a body on
  // the water rather than under it.
  const bool bodiesParted = madeBodies && woodSeen > 0 && ironSeen > 0 &&
                            ironLowest <= (float)(bedY + 4) &&
                            woodLowest >= (float)(bedY + 5);

  const bool ok = sank && floated && drained && bodiesParted;
  std::printf("debris-float: %s (iron %d/%d cells y%d..%d, bed %d | wood %d/%d "
              "cells y%d..%d, waterline %d | live %u | bodies lowest wood "
              "y%.1f (%d ticks) iron y%.1f (%d ticks), impulses %u, probes "
              "%u/%u wet, surface y%.0f)\n",
              ok ? "PASS" : "FAIL", ironCells, nIron, ironLo, ironHi, bedY + 1,
              woodCells, nWood, woodLo, woodHi, waterTop + 1, liveEnd,
              woodSeen ? woodLowest : -1.0f, woodSeen,
              ironSeen ? ironLowest : -1.0f, ironSeen, fp.floatedBodies,
              fp.floatProbes, fp.floatProbesWet, fp.floatLastSurfaceY);
  detail = Format("iron y%d..%d (bed %d), wood y%d..%d (waterline %d), live %u,"
                  " bodies lowest wood %.1f / iron %.1f",
                  ironLo, ironHi, bedY + 1, woodLo, woodHi, waterTop + 1,
                  liveEnd, woodSeen ? woodLowest : -1.0f,
                  ironSeen ? ironLowest : -1.0f);
  return ok ? Status::Pass : Status::Fail;
}

// ---- fluid-stain ---------------------------------------------------------
// Staining parity across the seam (plan §6.2, task 4a): water voxels placed
// CARRYING a foreign stain (type 3 — "blood-water") are excited, drain
// through the box, and must (a) stain the solid surfaces the particles touch
// with that SAME carried type (the CA's own staining would apply water's
// authored "wet" type, so type 3 on a wall can only have come through the
// particle attr word), and (b) re-settle still carrying the stain bits.
// Twice-run world-hash equality as always.
Status GateFluidStain(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  uint32_t waterId = 0;
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == "water") { waterId = (uint32_t)i; break; }
  if (waterId == 0) { detail = "no 'water' material"; return Status::Fail; }
  const uint32_t kSType = 3u, kSAmt = 12u;

  Tuning t = CurrentTuning();
  t.dayNight.freeze = 1;
  t.dayNight.freezePhase = (int)(kDaySunrise + 1024u);
  t.sim.fluidExciteMode = 1;
  // The excite gate's sealed-box overrides. settleEps/wakeSpeed used to be
  // here too and are stock now (WP3) — a sealed chamber still needs the
  // damping and the softer stiffness.
  t.sim.fluidDamping = 0.9f;
  t.sim.fluidStiffness = 2400.0f;
  Tuning saved = CurrentTuning();
  SetCurrentTuning(t);
  sim.ReloadShaders(ctx.device);

  const int px = 96, pz = 96, RB = 8;
  const int floorY = 109, upperY = 119, roofY = 126;
  const int kCarveTick = 30, kMaxTicks = 260;
  uint32_t worldHash[2] = {0, 0};
  uint32_t stainedWalls = 0, stainedWater = 0, appliedSum = 0;
  for (int run = 0; run < 2; run++) {
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    std::vector<CellOp> box;
    auto put = [&](int x, int y, int z, uint32_t w) {
      box.push_back({World::SlotCellIndex({x, y, z}), w});
    };
    const uint32_t stainedWaterWord =
        waterId | (7u << 12) | (kSAmt << 24) | (kSType << 28);
    for (int y = floorY - 1; y <= roofY + 1; y++)
      for (int z = -RB; z <= RB; z++)
        for (int x = -RB; x <= RB; x++) {
          bool shell = x <= -RB + 1 || x >= RB - 1 || z <= -RB + 1 ||
                       z >= RB - 1 || y <= floorY || y >= roofY ||
                       y == upperY || y == upperY + 1;
          if (shell) put(px + x, y, pz + z, kMatStone);
          else if (y >= upperY + 2 && y <= upperY + 3)
            put(px + x, y, pz + z, stainedWaterWord);
          else put(px + x, y, pz + z, 0u);
        }
    std::vector<CellOp> carve;
    for (int z = -2; z < 2; z++)
      for (int x = -2; x < 2; x++) {
        carve.push_back({World::SlotCellIndex({px + x, upperY, pz + z}), 0u});
        carve.push_back(
            {World::SlotCellIndex({px + x, upperY + 1, pz + z}), 0u});
      }

    uint32_t ft = 60000;
    uint32_t liveEst = 0;
    for (int i = 0; i < kMaxTicks; i++) {
      std::vector<CellOp> cops;
      if (i == 0) cops = box;
      else if (i == kCarveTick) cops = carve;
      SubmitTick(ctx, world, sim, ++ft, kDefaultSeed, {}, {}, cops, false,
                 {6, 7, 6}, false, false, {}, 0, {}, liveEst);
      ctx.WaitIdle();
      ctx.ProcessEvents();
      if (i >= kCarveTick) {
        uint32_t fa[32] = {};
        rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0,
                              fa, 128, "stainArgs");
        liveEst = std::min(fa[7], kFluidCap);
        appliedSum += fa[17];  // FA_STAINED
        // Wall sweep MID-DRAIN: the settled pool later WASHES the walls it
        // touches (water's authored `washes: true` rinsing the foreign
        // type — the same behaviour that lets CA water clean blood off
        // stone), so the deposited stains must be observed while the flow
        // is live, not at the washed end state.
        if (i == kCarveTick + 50) {
          stainedWalls = 0;
          std::vector<uint32_t> cb2((size_t)kChunkVol);
          for (int cy = (floorY - 1) / 16; cy <= (roofY + 1) / 16; cy++)
            for (int cz2 = (pz - RB) / 16; cz2 <= (pz + RB) / 16; cz2++)
              for (int cx2 = (px - RB) / 16; cx2 <= (px + RB) / 16; cx2++) {
                ReadVoxelsSync(ctx, world,
                               World::SlotChunkIndex({cx2, cy, cz2}), 1,
                               cb2.data(), "stainMid");
                for (uint32_t k = 0; k < kChunkVol; k++) {
                  uint32_t w = cb2[k];
                  if (((w >> 28) & 0x7u) == kSType &&
                      ((w >> 24) & 0xFu) != 0 && (w & 0xFFFu) == kMatStone)
                    stainedWalls++;
                }
              }
        }
      }
    }
    worldHash[run] = HashWorldNow(ctx, world, sim, kDefaultSeed);

    stainedWater = 0;
    std::vector<uint32_t> cbuf((size_t)kChunkVol);
    for (int cy = (floorY - 1) / 16; cy <= (roofY + 1) / 16; cy++)
      for (int cz2 = (pz - RB) / 16; cz2 <= (pz + RB) / 16; cz2++)
        for (int cx2 = (px - RB) / 16; cx2 <= (px + RB) / 16; cx2++) {
          ReadVoxelsSync(ctx, world, World::SlotChunkIndex({cx2, cy, cz2}), 1,
                         cbuf.data(), "stainVox");
          for (uint32_t i = 0; i < kChunkVol; i++) {
            uint32_t w = cbuf[i];
            if (((w >> 28) & 0x7u) == kSType && ((w >> 24) & 0xFu) != 0 &&
                (w & 0xFFFu) == waterId)
              stainedWater++;
          }
        }
  }
  SetCurrentTuning(saved);
  sim.ReloadShaders(ctx.device);

  bool det = worldHash[0] == worldHash[1];
  // stainedWalls is a STEADY-STATE snapshot: application (~27%/tick/cell)
  // races the pool's wash (~26%/tick/contact), so only a couple of wall
  // cells are visibly stained at any instant. The volume claim lives in
  // appliedSum; the snapshot just proves the bits land on real voxels.
  bool ok = appliedSum >= 50 && stainedWalls >= 1 && stainedWater >= 20 && det;
  std::printf(
      "fluid stain: %s (%u contact stains applied, %u wall cells carried the "
      "excited type mid-drain, %u settled water cells kept it to the end, "
      "world hash %s)\n",
      ok ? "PASS" : "FAIL", appliedSum, stainedWalls, stainedWater,
      det ? "matches" : "DIVERGED");
  detail = Format("%u applied, %u walls, %u water, det %s", appliedSum,
                  stainedWalls, stainedWater, det ? "ok" : "DIVERGED");
  return ok ? Status::Pass : Status::Fail;
}

// ---- fluid-react ---------------------------------------------------------
// CA reactions consume EXCITED fluid (plan §6.2, task 4b): plants next to
// water grow into it (`neighborBecomes: plant` — a water-consuming rule).
// A plant bed on the catch floor eats from the drained pool: consumption
// must occur while the water is PARTICLES (the doReactions synthesis + the
// seam's consume flags), and the mass account must stay exact:
// placed == standing water + live fullness + consumed eighths.
Status GateFluidReact(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  uint32_t waterId = 0, plantId = 0;
  for (size_t i = 0; i < c.mats.size(); i++) {
    if (c.mats[i].name == "water") waterId = (uint32_t)i;
    if (c.mats[i].name == "plant") plantId = (uint32_t)i;
  }
  if (waterId == 0 || plantId == 0) {
    detail = "no water/plant material";
    return Status::Fail;
  }

  Tuning t = CurrentTuning();
  t.dayNight.freeze = 1;
  t.dayNight.freezePhase = (int)(kDaySunrise + 1024u);
  // Pin every sim.fluid* parameter to its tuning_params.def default so this
  // gate is hermetic — its outcome must not depend on the user's tuning.json.
  // Only THREE are real overrides now (exciteMode, damping, stiffness);
  // settleEps and wakeSpeed became stock in WP3 and are pins like the rest.
  t.sim.fluidExciteMode = 1;
  t.sim.fluidDamping = 0.9f;       // sealed box: nothing radiates out of it
  t.sim.fluidStiffness = 2400.0f;  // softer than stock so the drain ends
  t.sim.fluidSettleEps = 6.0f;     // == stock (WP3)
  t.sim.fluidWakeSpeed = 24.0f;    // == stock (WP3)
  t.sim.fluidSubsteps = 9;
  t.sim.fluidGravity = 900.0f;
  t.sim.fluidRestDensity = 8.0f;
  t.sim.fluidEosPower = 4;
  t.sim.fluidCohesion = 0.0f;
  t.sim.fluidAttractSame = 0.0f;
  t.sim.fluidAttractDiff = 0.0f;
  t.sim.fluidViscosity = 0.0f;
  t.sim.fluidFriction = 0.0f;
  // SPLASH IS NOT THE MISSING MASS, and the correction is worth writing down
  // because the wrong answer was already in this file.
  //
  // The gate comes up 37 of 2704 eighths short (1.4%), deterministically. The
  // first diagnosis was splash: sim_fluid's g2p sheds droplets into the
  // ballistic particle system at sim.fluidSplashRate, a droplet is in none of
  // the three counted destinations, and the reporter dutifully said "short 37,
  // 10783 droplets in flight" -- a real number next to the failure, which is
  // exactly what makes a coincidence convincing. So the rate was pinned to 0.
  //
  // Read the emitter (sim_fluid.wgsl, "splash: fast free-surface particles
  // shed micro droplets"): it fills a Particle and appends it, and it NEVER
  // touches the parent's fullness. A splash droplet is a PFLAG_MICRO visual
  // that deposits a stain on contact -- it carries no eighths, so it cannot
  // remove any. The pin proved it: droplets fell 10783 -> 10264 and the
  // shortfall stayed at exactly 37. A number that does not move when you
  // remove its supposed cause was never the cause.
  //
  // So the rate goes back to the .def default like every other line here, and
  // the question goes to the seam ledger below, which is the instrument the
  // other two fluid gates already use for exactly this.
  t.sim.fluidSplashRate = 4.0f;
  t.sim.fluidSplashSpeed = 18.0f;
  t.sim.fluidSplashMaxDensity = 0.7f;
  t.sim.fluidSplashLife = 1.1f;
  t.sim.fluidSplashScaleIdx = 2;
  t.sim.fluidFoamRate = 90.0f;
  t.sim.fluidFoamCrestRate = 120.0f;
  t.sim.fluidTrappedMin = 1.5f;
  t.sim.fluidTrappedMax = 11.0f;
  t.sim.fluidCrestMin = 0.25f;
  t.sim.fluidCrestMax = 2.0f;
  t.sim.fluidFoamEnergyMin = 8.0f;
  t.sim.fluidFoamEnergyMax = 260.0f;
  t.sim.fluidFoamLife = 2.2f;
  t.sim.fluidFoamLifeMin = 0.5f;
  t.sim.fluidBubbleBuoyancy = 1.6f;
  t.sim.fluidFoamDrag = 0.72f;
  t.sim.fluidBubbleDensity = 1.05f;
  t.sim.fluidSprayDensity = 0.42f;
  t.sim.fluidFoamScaleIdx = 3;
  t.sim.fluidSettleTicks = 24;
  Tuning saved = CurrentTuning();
  SetCurrentTuning(t);
  sim.ReloadShaders(ctx.device);

  const int px = 96, pz = 96, RB = 8;
  const int floorY = 109, upperY = 119, roofY = 126;
  const int kCarveTick = 30, kMaxTicks = 260;
  const uint32_t kWaterEighths = 13u * 13u * 2u * 8u;  // 2 deep this time
  uint32_t worldHash[2] = {0, 0};
  uint32_t consumedSum = 0, standing = 0, liveEighths = 0, plantsEnd = 0;
  // Attribution for a mass account that does not close — see the census below.
  uint32_t strayWater = 0, endParticles = 0;
  // THE SEAM LEDGER, which is what `ca-slope` and `fluid-excite` reach for when
  // their own accounts do not close, and what this gate was missing. Water
  // crossing between voxels and particles passes through these counters, so a
  // shortfall that is invisible in the three-way account is usually loud in
  // here: excited != emitted means the excite dropped it, emitted != settled +
  // live means a particle died carrying mass, binned is settle refusing a
  // column. Seven u32 adds a tick against a readback the loop already does.
  uint32_t exExcited = 0, exEmitted = 0, exSettled = 0, exDead = 0;
  uint32_t exBinned = 0, exRefused = 0;
  const uint32_t kPlantsStart = 5 * 5;
  for (int run = 0; run < 2; run++) {
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    std::vector<CellOp> box;
    auto put = [&](int x, int y, int z, uint32_t w) {
      box.push_back({World::SlotCellIndex({x, y, z}), w});
    };
    for (int y = floorY - 1; y <= roofY + 1; y++)
      for (int z = -RB; z <= RB; z++)
        for (int x = -RB; x <= RB; x++) {
          bool shell = x <= -RB + 1 || x >= RB - 1 || z <= -RB + 1 ||
                       z >= RB - 1 || y <= floorY || y >= roofY ||
                       y == upperY || y == upperY + 1;
          if (shell) put(px + x, y, pz + z, kMatStone);
          else if (y >= upperY + 2 && y <= upperY + 3)
            put(px + x, y, pz + z, waterId | (7u << 12));
          else if (y == floorY + 1 && std::abs(x) <= 2 && std::abs(z) <= 2)
            put(px + x, y, pz + z, plantId);  // the plant bed (5x5)
          else put(px + x, y, pz + z, 0u);
        }
    std::vector<CellOp> carve;
    for (int z = -2; z < 2; z++)
      for (int x = -2; x < 2; x++) {
        carve.push_back({World::SlotCellIndex({px + x, upperY, pz + z}), 0u});
        carve.push_back(
            {World::SlotCellIndex({px + x, upperY + 1, pz + z}), 0u});
      }

    uint32_t ft = 70000;
    uint32_t liveEst = 0;
    consumedSum = 0;
    exExcited = exEmitted = exSettled = exDead = exBinned = exRefused = 0;
    for (int i = 0; i < kMaxTicks; i++) {
      std::vector<CellOp> cops;
      if (i == 0) cops = box;
      else if (i == kCarveTick) cops = carve;
      SubmitTick(ctx, world, sim, ++ft, kDefaultSeed, {}, {}, cops, false,
                 {6, 7, 6}, false, false, {}, 0, {}, liveEst);
      ctx.WaitIdle();
      ctx.ProcessEvents();
      // FROM TICK 0, not from the carve. The old loop started accumulating at
      // kCarveTick on the reasoning that nothing can happen before the floor
      // opens — but that is an assumption about the sim stated in the test,
      // and it is the kind that produces a small constant shortfall if it is
      // wrong (the water sits on a sealed floor for 30 ticks, and "sealed"
      // is what the reaction rules get to decide, not this file). Reading the
      // counters every tick costs the same readback and removes the
      // assumption. If ticks 0..29 really do contribute nothing, the ledger
      // below will say so in zeroes.
      uint32_t fa[32] = {};
      rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0, fa,
                            128, "reactArgs");
      liveEst = std::min(fa[7], kFluidCap);
      consumedSum += fa[16];  // FA_CONSUMED
      exDead += fa[8];        // FA_DEAD
      exEmitted += fa[9];     // FA_EMITTED
      exSettled += fa[10];    // FA_SETTLED
      exExcited += fa[11];    // FA_EXCITED
      exRefused += fa[12];    // FA_REFUSED
      exBinned += fa[15];     // FA_BINNED
    }
    worldHash[run] = HashWorldNow(ctx, world, sim, kDefaultSeed);

    // Final live fullness for the mass equation.
    liveEighths = 0;
    uint32_t fa[32] = {};
    rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0, fa,
                          128, "reactArgsEnd");
    uint32_t live = std::min(fa[7], kFluidCap);
    if (live > 0) {
      std::vector<uint32_t> pbuf((size_t)live * kFluidParticleWords);
      rhi::ReadbackBlocking(ctx.device, ctx.queue,
                            world.fluidParticles[sim.Page()], 0, pbuf.data(),
                            pbuf.size() * 4, "reactEndP");
      for (uint32_t k = 0; k < live; k++)
        liveEighths += (pbuf[k * kFluidParticleWords + 18] >> 12) & 0x7u;
    }

    standing = 0;
    plantsEnd = 0;
    strayWater = 0;
    std::vector<uint32_t> cbuf((size_t)kChunkVol);
    for (int cy = (floorY - 1) / 16; cy <= (roofY + 1) / 16; cy++)
      for (int cz2 = (pz - RB) / 16; cz2 <= (pz + RB) / 16; cz2++)
        for (int cx2 = (px - RB) / 16; cx2 <= (px + RB) / 16; cx2++) {
          ReadVoxelsSync(ctx, world, World::SlotChunkIndex({cx2, cy, cz2}), 1,
                         cbuf.data(), "reactVox");
          for (uint32_t i = 0; i < kChunkVol; i++) {
            int lx = (int)(i % 16) + cx2 * 16,
                ly = (int)((i / 16) % 16) + cy * 16,
                lz = (int)(i / 256) + cz2 * 16;
            const bool inside =
                !(lx < px - RB + 2 || lx > px + RB - 2 || lz < pz - RB + 2 ||
                  lz > pz + RB - 2 || ly <= floorY || ly >= roofY);
            uint32_t m = cbuf[i] & 0xFFFu;
            // WHERE ELSE COULD THE MASS BE. The account below is exact by
            // construction, so when it does not close the only useful next
            // question is which of the OTHER destinations took the
            // difference — and answering that by turning features off one at
            // a time is the mistake CLAUDE.md rule 6 is about. Water outside
            // the interior box (leaked into the shell, or through it) is one
            // destination and is counted here; droplets in flight are the
            // other and are read from the snapshot below.
            if (m == waterId && !inside) {
              strayWater += ((cbuf[i] >> 12) & 0xFu) + 1u;
              continue;
            }
            if (!inside) continue;
            if (m == waterId) standing += ((cbuf[i] >> 12) & 0xFu) + 1u;
            if (m == plantId) plantsEnd++;
          }
        }
    // Droplets: sim_fluid's g2p sheds spray into the BALLISTIC particle system
    // (tuning sim.fluidSplashRate), which is a fourth destination for water
    // that neither the grid census nor the fluid-particle census can see.
    endParticles = world.Snap().valid ? world.Snap().particleCount : 0u;
  }
  SetCurrentTuning(saved);
  sim.ReloadShaders(ctx.device);

  bool det = worldHash[0] == worldHash[1];
  bool consumed = consumedSum > 0;
  bool grew = plantsEnd > kPlantsStart;
  const int shortBy = (int)kWaterEighths - (int)(standing + liveEighths +
                                                 consumedSum);

  // THE ACCOUNT HAS FOUR DESTINATIONS, NOT THREE, and the fourth is the thing
  // the gate is named after.
  //
  // `standing + live + consumed == placed` was exact by construction only if
  // every eighth a reaction eats passes through the seam. It does not.
  // FA_CONSUMED is a fluidArgs counter: it records eighths eaten off EXCITED
  // fluid, which is the specific claim this gate exists to make. But the
  // authored rule is `{self: plant, neighbor: water, neighborBecomes: plant}`
  // (assets/materials/reactions.json), and the CA runs it on SETTLED water
  // voxels too — a water cell beside a plant simply becomes plant, in the
  // grid, with no particle and no seam event. Those eighths are not lost; they
  // are standing in the world as the 198 new plant cells the gate itself
  // counts and calls a pass.
  //
  // Measured: 37 of 2704 (1.4%), and the seam ledger says it is not seam-side
  // (2432 excited -> 2432 emitted, nothing refused). 37 eighths against 198
  // new plants is under a quarter of an eighth per plant, which is what
  // eating mostly-empty rim cells looks like.
  //
  // So the assertion becomes the property that actually matters, and it is
  // still a leak detector — it just knows about the fourth destination:
  //   1. NO WATER IS CREATED. shortBy >= 0, unconditionally.
  //   2. Everything missing is accounted for by plants that exist. A water
  //      cell holds at most 8 eighths and each conversion consumes at most
  //      one cell, so the gap can never exceed 8 * the plants that grew.
  //   3. And it stays SMALL. Bound 2 alone is loose (8 * 198 = 1584), so the
  //      fraction is pinned in baseline.json where a threshold belongs. Water
  //      vanishing with no plants to show for it, or vanishing faster than the
  //      plant bed can eat, fails here exactly as the equality used to.
  const uint32_t newPlants =
      plantsEnd > kPlantsStart ? plantsEnd - kPlantsStart : 0u;
  const double gapPct = 100.0 * (double)shortBy / (double)kWaterEighths;
  const double gapPctMax = BaselineNumber("fluidReactCaGapPctMax", 3.0);
  bool massOk = shortBy >= 0 && (uint32_t)shortBy <= 8u * newPlants &&
                gapPct <= gapPctMax;
  RecordObserved("fluidReactCaGapPct", gapPct);
  bool ok = consumed && grew && massOk && det;
  std::printf("  react seam: %u excited -> %u emitted, %u settled, %u dead, "
              "%u refused, %u binned\n",
              exExcited, exEmitted, exSettled, exDead, exRefused, exBinned);
  std::printf(
      "fluid react: %s (%u eighths consumed by reactions, plants %u -> %u, "
      "%u standing + %u live + %u consumed of %u placed"
      " [gap %d = %.2f%% (allow %.2f%%), under the %u eighths %u new plants"
      " could have eaten; %u stray outside the box, %u droplets in flight],"
      " world hash %s)\n",
      ok ? "PASS" : "FAIL", consumedSum, kPlantsStart, plantsEnd, standing,
      liveEighths, consumedSum, kWaterEighths, shortBy, gapPct, gapPctMax,
      8u * newPlants, newPlants, strayWater, endParticles,
      det ? "matches" : "DIVERGED");
  detail = Format("%u consumed, plants %u->%u, mass %u+%u+%u/%u (gap %d ="
                  " %.2f%% of %.2f%% allowed, vs %u eighths %u new plants"
                  " could eat; stray %u, droplets %u; seam %u excited -> %u"
                  " emitted, %u settled, %u dead, %u refused, %u binned),"
                  " det %s",
                  consumedSum, kPlantsStart, plantsEnd, standing, liveEighths,
                  consumedSum, kWaterEighths, shortBy, gapPct, gapPctMax,
                  8u * newPlants, newPlants, strayWater, endParticles,
                  exExcited, exEmitted, exSettled, exDead, exRefused, exBinned,
                  det ? "ok" : "DIVERGED");
  return ok ? Status::Pass : Status::Fail;
}

// ---- fluid-self-react ------------------------------------------------------
// EXCITED FLUID RUNS ITS OWN RULES (rule-unification W1-B1). fluid-react proves
// the NEIGHBOUR side: a voxel's rule consumes the excited water beside it. This
// gate proves the SELF side, which did not exist before -- an air cell holding
// MPM particles runs the particles' material bucket (sim_step excitedReact),
// woken by the seam's particleTick when one of its ungated rules has a partner.
// Three sealed glass boxes (glass: acid's rules skip it, nothing hot, not
// absorbent), each poured with 1024 particles and run with settle held off so
// every eighth stays EXCITED for the whole window -- the fixture then asserts
// that nothing settled, so no voxel liquid can have done the work:
//   A  acid on a floor half stone, half an inert organic: acid's own PAIR
//      rules (neighborBecomes gravel / air) must eat both. Stone has no rule
//      of its own and the organic is picked inert, so only acid can.
//   B  water on a lava floor: water's own rule `water + tag:hot -> steam`
//      must make steam. Lava's rule makes STONE (neighbour kept), so any steam
//      is the water's, and FA_CONSUMED -- the bins its self product took --
//      must be non-zero.
//   C  water on an inert ABSORBENT floor: the donor spend (stainStep's
//      STAIN_SPEND paid by particleTick's bid). Exact: the wet-stain levels on
//      the floor equal the eighths consumed, and spawned == live + consumed.
// Dim dawn pinned (evaporation and freezing off), twice-run world hash.
Status GateFluidSelfReact(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  auto idOf = [&](const char* n) -> uint32_t {
    for (size_t i = 1; i < c.mats.size(); i++)
      if (c.mats[i].name == n) return (uint32_t)i;
    return 0u;
  };
  const uint32_t waterId = idOf("water"), acidId = idOf("acid"),
                 lavaId = idOf("lava"), glassId = idOf("glass"),
                 steamId = idOf("steam");
  if (!waterId || !acidId || !lavaId || !glassId || !steamId) {
    detail = "missing one of water / acid / lava / glass / steam";
    return Status::Fail;
  }
  // The organic and the absorbent floors are picked BY PROPERTY, first in
  // table order, and INERT (no rule of their own) so the only thing that can
  // change them is the fluid (memory: gotcha-gate-hardcodes-asset-cast).
  uint32_t organicId = 0, absorbId = 0;
  for (uint32_t i = 1; i < (uint32_t)c.mats.size(); i++) {
    const MaterialDef& m = c.mats[i];
    if (m.gpu.klass != CLASS_SOLID || m.gpu.reactCount != 0) continue;
    bool organic = false;
    for (const std::string& t : m.tags) if (t == "organic") organic = true;
    if (!organicId && organic) organicId = i;
    if (!absorbId && !organic && ((m.gpu.stainPack >> 27) & 0xFu) != 0)
      absorbId = i;
  }
  if (!organicId || !absorbId) {
    detail = Format("no inert solid %s in the table",
                    !organicId ? "organic" : "absorbent non-organic");
    return Status::Fail;
  }
  const uint32_t wetType = c.mats[waterId].gpu.stainPack & 0x7u;

  Tuning t = CurrentTuning();
  t.dayNight.freeze = 1;
  t.dayNight.freezePhase = (int)(kDaySunrise + 1024u);
  t.sim.fluidExciteMode = 0;     // nothing voxel becomes a particle...
  t.sim.fluidSettleTicks = 600;  // ...and no particle becomes a voxel
  t.sim.fluidDamping = 0.9f;     // the sealed-box pins fluid-react uses
  t.sim.fluidStiffness = 2400.0f;
  Tuning saved = CurrentTuning();
  SetCurrentTuning(t);
  sim.ReloadShaders(ctx.device);
  // WET DOES NOT DRY during the arms. Since f039607 (2026-09-25) water's wet
  // stain comes off the ground by itself (stain.dries, 15 per mille per tick
  // per level, sim_step.wgsl stainDry) whenever no water VOXEL touches the
  // cell -- and in arm C every eighth is an excited particle, so nothing
  // touches it and the roofed fixture's floor dries while it drinks: measured
  // 893 drunk vs 831 levels standing, ~60 levels dried over 75 cells in 60
  // ticks. The claim here is the ABSORB ledger (each eighth drunk is one level
  // of wet), so the drying is parked for the gate, exactly as the dawn pin
  // parks evaporation; drying has its own gate (rain-stain, "wet dries").
  {
    std::vector<MaterialDef> noDry = c.mats;
    noDry[waterId].stainDries = 0;
    sim.UploadTables(ctx.queue, noDry, c.reactions);
  }

  const int px = 96, pz = 96, RB = 7;
  const int floorY = 109, roofY = 126, padY = floorY + 1;
  const int kTicks = (int)BaselineNumber("fluidSelfReactTicks", 60);

  struct Arm {
    uint32_t floorStart[2] = {0, 0};  // A: stone / organic; B: lava; C: absorbent
    uint32_t floorEnd[2] = {0, 0};
    uint32_t steam = 0, stainLevels = 0, stainedCells = 0, fluidVox = 0;
    uint32_t consumed = 0, settled = 0, dead = 0, live = 0, liveEighths = 0;
    uint32_t spawned = 0, hash = 0;
  };
  // arm: 0 = acid on stone|organic, 1 = water on lava, 2 = water on absorbent
  auto runArm = [&](int arm, uint32_t tick0) -> Arm {
    Arm r;
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    auto floorMat = [&](int x) -> uint32_t {
      if (arm == 0) return x < 0 ? kMatStone : organicId;
      return arm == 1 ? lavaId : absorbId;
    };
    std::vector<CellOp> box;
    for (int y = floorY - 1; y <= roofY + 1; y++)
      for (int z = -RB; z <= RB; z++)
        for (int x = -RB; x <= RB; x++) {
          const bool shell = x <= -RB + 1 || x >= RB - 1 || z <= -RB + 1 ||
                             z >= RB - 1 || y <= floorY || y >= roofY;
          uint32_t w = 0u;
          if (shell) w = glassId;
          else if (y == padY) w = floorMat(x);
          if (w == lavaId) w |= 7u << 12;  // a FULL liquid cell
          box.push_back({World::SlotCellIndex({px + x, y, pz + z}), w});
        }
    const uint32_t fluidMat = arm == 0 ? acidId : waterId;
    std::vector<FluidSpawnOp> pour;
    for (int cz = -4; cz < 4; cz++)
      for (int cy = 0; cy < 2; cy++)
        for (int cx = -4; cx < 4; cx++)
          for (int s = 0; s < 8; s++) {
            FluidSpawnOp op{};
            op.px = ((px + cx) << 16) + ((s & 1) ? 49152 : 16384);
            op.py = ((padY + 3 + cy) << 16) + ((s & 2) ? 49152 : 16384);
            op.pz = ((pz + cz) << 16) + ((s & 4) ? 49152 : 16384);
            op.mat = fluidMat;
            pour.push_back(op);
          }
    r.spawned = (uint32_t)pour.size();  // one eighth per spawned particle

    auto census = [&](uint32_t out[2]) {
      std::vector<uint32_t> cb((size_t)kChunkVol);
      out[0] = out[1] = 0;
      r.steam = r.stainLevels = r.stainedCells = r.fluidVox = 0;
      for (int cy = (floorY - 1) / 16; cy <= (roofY + 1) / 16; cy++)
        for (int cz = (pz - RB) / 16; cz <= (pz + RB) / 16; cz++)
          for (int cx = (px - RB) / 16; cx <= (px + RB) / 16; cx++) {
            ReadVoxelsSync(ctx, world, World::SlotChunkIndex({cx, cy, cz}), 1,
                           cb.data(), "selfReact");
            for (uint32_t i = 0; i < kChunkVol; i++) {
              const int lx = (int)(i % 16) + cx * 16 - px,
                        ly = (int)((i / 16) % 16) + cy * 16,
                        lz = (int)(i / 256) + cz * 16 - pz;
              if (lx <= -RB + 1 || lx >= RB - 1 || lz <= -RB + 1 ||
                  lz >= RB - 1 || ly <= floorY || ly >= roofY)
                continue;  // shell or outside: not the fixture's interior
              const uint32_t w = cb[i], m = w & 0xFFFu;
              if (m == steamId) r.steam++;
              if (m == fluidMat) r.fluidVox++;
              if (ly == padY) {
                if (m == floorMat(lx)) out[lx < 0 || arm != 0 ? 0 : 1]++;
                if (arm == 2 && m == absorbId && ((w >> 28) & 0x7u) == wetType) {
                  r.stainLevels += (w >> 24) & 0xFu;
                  if ((w >> 24) & 0xFu) r.stainedCells++;
                }
              }
            }
          }
    };

    uint32_t ft = tick0, liveEst = 0, fluidN = 0;
    for (int i = 0; i < kTicks; i++) {
      std::vector<FluidSpawnOp> fs;
      if (i == 2) fs = pour;
      SubmitTick(ctx, world, sim, ++ft, kDefaultSeed, {}, {},
                 i == 0 ? box : std::vector<CellOp>{}, false, {6, 7, 6},
                 false, false, {}, 0, fs, std::max(liveEst, fluidN));
      fluidN += (uint32_t)fs.size();
      ctx.WaitIdle();
      ctx.ProcessEvents();
      if (i == 1) census(r.floorStart);
      uint32_t fa[32] = {};
      rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0, fa,
                            128, "selfReactArgs");
      liveEst = std::min(fa[7], kFluidCap);
      if (i >= 2) {
        r.consumed += fa[16];  // FA_CONSUMED: self products + absorb spends
        r.settled += fa[10];   // FA_SETTLED: must stay 0 (fixture validity)
        r.dead += fa[8];
      }
    }
    census(r.floorEnd);
    r.hash = HashWorldNow(ctx, world, sim, kDefaultSeed);
    r.live = liveEst;
    if (r.live > 0) {
      std::vector<uint32_t> pbuf((size_t)r.live * kFluidParticleWords);
      rhi::ReadbackBlocking(ctx.device, ctx.queue,
                            world.fluidParticles[sim.Page()], 0, pbuf.data(),
                            pbuf.size() * 4, "selfReactP");
      for (uint32_t k = 0; k < r.live; k++)
        r.liveEighths += (pbuf[k * kFluidParticleWords + 18] >> 12) & 0x7u;
    }
    return r;
  };

  Arm arms[2][3];
  for (int run = 0; run < 2; run++)
    for (int a = 0; a < 3; a++)
      arms[run][a] = runArm(a, 80000u + 1000u * (uint32_t)a);
  SetCurrentTuning(saved);
  sim.ReloadShaders(ctx.device);
  sim.UploadTables(ctx.queue, c.mats, c.reactions);

  bool det = true;
  for (int a = 0; a < 3; a++)
    det = det && arms[0][a].hash == arms[1][a].hash &&
          arms[0][a].consumed == arms[1][a].consumed;
  const Arm& A = arms[1][0];
  const Arm& B = arms[1][1];
  const Arm& C = arms[1][2];
  const uint32_t minEat = (uint32_t)BaselineNumber("fluidSelfReactMinEaten", 2);
  const uint32_t minSteam = (uint32_t)BaselineNumber("fluidSelfReactMinSteam", 3);
  const uint32_t stoneEaten = A.floorStart[0] - std::min(A.floorStart[0], A.floorEnd[0]);
  const uint32_t orgEaten = A.floorStart[1] - std::min(A.floorStart[1], A.floorEnd[1]);
  // Fixture validity: every eighth stayed excited (nothing settled, no voxel
  // of the poured liquid anywhere in the box), so the CA's VOXEL rules for
  // the fluid never had anything to run on.
  const bool excitedOnly = A.settled == 0 && B.settled == 0 && C.settled == 0 &&
                           A.fluidVox == 0 && C.fluidVox == 0;
  const bool acidAte = stoneEaten >= minEat && orgEaten >= minEat;
  const bool steamed = B.steam >= minSteam && B.consumed > 0;
  const bool absorbed = C.consumed > 0 && C.stainLevels == C.consumed &&
                        C.spawned == C.liveEighths + C.consumed;
  RecordObserved("fluidSelfReactStoneEaten", (double)stoneEaten);
  RecordObserved("fluidSelfReactOrganicEaten", (double)orgEaten);
  RecordObserved("fluidSelfReactSteam", (double)B.steam);
  RecordObserved("fluidSelfReactAbsorbed", (double)C.consumed);
  const bool ok = det && excitedOnly && acidAte && steamed && absorbed;
  std::printf(
      "fluid self-react: %s (A acid: stone %u->%u, %s %u->%u, %u eighths "
      "consumed | B water on lava: %u steam cells, %u eighths consumed | C "
      "water on %s: %u eighths drunk, %u wet levels on %u cells, spawned %u = "
      "live %u + drunk %u | settled A/B/C %u/%u/%u, poured-liquid voxels "
      "A/C %u/%u | world hash %s)\n",
      ok ? "PASS" : "FAIL", A.floorStart[0], A.floorEnd[0],
      c.mats[organicId].name.c_str(), A.floorStart[1], A.floorEnd[1],
      A.consumed, B.steam, B.consumed, c.mats[absorbId].name.c_str(),
      C.consumed, C.stainLevels, C.stainedCells, C.spawned, C.liveEighths,
      C.consumed, A.settled, B.settled, C.settled, A.fluidVox, C.fluidVox,
      det ? "matches" : "DIVERGED");
  detail = Format(
      "acid ate stone %u / %s %u (min %u); water on lava made %u steam "
      "(min %u), consumed %u; absorb %u drunk vs %u wet levels, spawned %u vs "
      "live %u + drunk; excited-only %s; det %s",
      stoneEaten, c.mats[organicId].name.c_str(), orgEaten, minEat, B.steam,
      minSteam, B.consumed, C.consumed, C.stainLevels, C.spawned,
      C.liveEighths, excitedOnly ? "yes" : "NO (fixture settled)",
      det ? "ok" : "DIVERGED");
  return ok ? Status::Pass : Status::Fail;
}

// ---- prefab ------------------------------------------------------------
Status GatePrefab(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const std::vector<MaterialDef>& mats = c.mats;
// Milestone A prefab placement: a 48^3 stone cube (110,592 voxels) must
// spread across ticks under the 16384/tick placer budget and land with
// ZERO dropped voxels (exact count via chunk fetches).
bool prefabOk = false;
{
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  Prefab pf;
  pf.name = "testcube";
  PrefabModel pm;
  pm.name = "cube";
  pm.size = {48, 48, 48};
  for (int z = 0; z < 48; z++)
    for (int y = 0; y < 48; y++)
      for (int x = 0; x < 48; x++)
        pm.voxels.push_back({(int16_t)x, (int16_t)y, (int16_t)z,
                             (uint16_t)kMatStone});
  pf.size = pm.size;
  pf.models.push_back(std::move(pm));

  PrefabPlacer placer;
  // Terrain-relative, and the COUNT BOX below follows it: a fixed box that
  // now contains a hillside counts the hillside, which is how "110,592
  // voxels placed" was reported as 257,391 landed.
  IVec3 lo{100, FixtureYOver(100, 100, 147, 147, kDefaultSeed, 40, 96), 100},
      hi;
  placer.Place(pf, lo, 0, true, mats, lo, hi);
  uint32_t t = 4000;
  int drainTicks = 0;
  while (placer.PendingCount() > 0 && drainTicks < 32) {
    std::vector<CellOp> cellOps;
    placer.PreTick(world, cellOps);
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, cellOps, false,
               {8, 10, 8}, false, false);
    drainTicks++;
  }
  ctx.WaitIdle();

  // count the stamped voxels back out through the async fetch path
  uint32_t placedTick = t;
  uint64_t stone = 0;
  bool allFresh = false;
  for (int tries = 0; tries < 90 && !allFresh; tries++) {
    allFresh = true;
    stone = 0;
    for (int cz = 100 >> 4; cz <= 147 >> 4; cz++)
      for (int cy = lo.y >> 4; cy <= (lo.y + 47) >> 4; cy++)
        for (int cx = 100 >> 4; cx <= 147 >> 4; cx++) {
          const CachedChunk* cc = world.Cached({cx, cy, cz});
          if (!cc || cc->version < placedTick) {
            world.RequestChunkFetch({cx, cy, cz});
            allFresh = false;
            continue;
          }
          for (uint32_t w : cc->voxels)
            if ((w & 0xFFFu) == kMatStone) stone++;
        }
    if (!allFresh) {
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false,
                 {8, 10, 8}, true, false);
      ctx.WaitIdle();
      ctx.ProcessEvents();
    }
  }
  prefabOk = allFresh && stone == 48ull * 48 * 48 && drainTicks >= 6;
  std::printf("prefab: %s (%llu / %llu voxels landed over %d ticks)\n",
              prefabOk ? "PASS" : "FAIL", (unsigned long long)stone,
              48ull * 48 * 48, drainTicks);
}

  // Verdict: the flag the moved body already computed.
  return prefabOk ? Status::Pass : Status::Fail;
}

// ---- perf --------------------------------------------------------------
Status GatePerf(Ctx& c, std::string& detail) {
  // Advisory. The numbers track kVoxelMeters more than they track correctness,
  // so this reports MARGINAL and never turns the run red (Gate::advisory).
  //
  // THE BUDGETS LIVE IN baseline.json, which is CLAUDE.md's own rule ("put
  // thresholds and expected values in tests/baseline.json, not in C++ — a
  // threshold that lives in source costs a rebuild to tune") and which this
  // gate was the last one violating. 8.0 and 16.0 were literals here, written
  // at a smaller kVoxelMeters, and they have been unreachable since the voxel
  // size changed: the gate has sat red in the baseline ever since, asserting
  // an aspiration rather than detecting a regression. A permanently-red
  // advisory gate is worse than no gate, because it trains everyone to skip
  // the line.
  //
  // So: budgets are data, the measured values are pushed through
  // RecordObserved so `--rebaseline` writes back what the machine actually
  // does, and the fallbacks below are the historical literals for a checkout
  // whose baseline.json predates the keys.
  const double simBudget = BaselineNumber("perf.simMsMax", 8.0);
  const double frameBudget = BaselineNumber("perf.frameMsMax", 16.0);
  bool perfOk = c.simMs < simBudget && c.bestFrameMs < frameBudget;
  RecordObserved("perf.simMsObserved", c.simMs);
  RecordObserved("perf.frameMsObserved", c.bestFrameMs);
  std::printf("perf: %s\n", perfOk ? "PASS" : "MARGINAL (see numbers above)");
  detail = Format("sim %.2f ms/tick (budget %.2f), best frame %.2f ms "
                  "(budget %.2f)",
                  c.simMs, simBudget, c.bestFrameMs, frameBudget);
  // The overall verdict moved to the harness (selftest.cpp), which diffs every
  // gate against tests/baseline.json. The old aggregate AND-ed a hand-kept list
  // of flags that had already drifted out of step with the gates that existed.
  return perfOk ? Status::Pass : Status::Fail;
}


// ---- page-roundtrip (PLAN_page_table.md §4.4 Gate B) ---------------------
//
// THE gate that reads THROUGH the translation rather than around it: every
// other gate goes via ReadVoxelsSync, which synthesizes sentinels so a gate
// testing sim behaviour sees a dense-looking snapshot. This one asserts the
// page table itself.
//
// Anchored to world.WindowOrigin(), never a fixed world position — gates run
// in kOrder sequence and the streaming gate leaves the origin ~20 chunks out,
// which is the documented trap that made the spell gate detonate on tick 1.
Status GatePageRoundtrip(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  const bool paged = world.residency == World::Residency::Paged;
  PageTable& pt = *world.pages;

  // ---- step 0: the SENTINEL RLE FUSION is byte-identical (CPU only) --------
  //
  // RleEncodeSentinelChunk computes the RLE a sentinel chunk would produce
  // WITHOUT materializing its 4,096 words — it fuses SynthWordAt's synthesis
  // loop into RleEncodeChunk's run scan, and (for the EMPTY/UNIFORM shape)
  // skips the loop entirely. That is only legitimate if it is a fusion rather
  // than a second encoding, so assert the equality directly against the
  // definition: synthesize with SynthWordAt, encode with RleEncodeChunk, and
  // demand the same bytes. The save format depends on this (§4.2), and a
  // divergence here would corrupt evicted chunks silently and permanently.
  //
  // Covers all three sentinel shapes and NEGATIVE world-chunk coordinates —
  // the two's-complement bitcast is the documented trap in the jitter formula.
  {
    std::vector<uint32_t> words(kChunkVol), refRle, fusedRle;
    const uint32_t seed = pt.WorldSeed();
    const uint32_t entries[] = {
        kPtEmpty,
        kPtSentinelBit | kMatStone,
        kPtSentinelBit | kPtJitterBit | kMatStone,
        kPtSentinelBit | kPtJitterBit | kMatSand,
    };
    const IVec3 coords[] = {{0, 0, 0}, {3, 5, 7}, {-4, -1, -9}, {130, -7, 22}};
    for (uint32_t e : entries)
      for (const IVec3& wc : coords) {
        const int bx = wc.x * (int)kChunk, by = wc.y * (int)kChunk,
                  bz = wc.z * (int)kChunk;
        for (uint32_t k = 0; k < kChunkVol; k++)
          words[k] = SynthWordAt(e, bx + (int)(k % kChunk),
                                 by + (int)((k / kChunk) % kChunk),
                                 bz + (int)(k / (kChunk * kChunk)), seed);
        RleEncodeChunk(words.data(), refRle);
        RleEncodeSentinelChunk(e, wc, seed, fusedRle);
        if (refRle != fusedRle) {
          detail = "sentinel RLE fusion diverged: entry " + std::to_string(e) +
                   " at chunk " + std::to_string(wc.x) + "," +
                   std::to_string(wc.y) + "," + std::to_string(wc.z) +
                   " (" + std::to_string(refRle.size()) + " vs " +
                   std::to_string(fusedRle.size()) + " words)";
          return Status::Fail;
        }
      }
  }

  // Standalone (--gate) the world is the untouched identity map: every CHUNK
  // SLOT holds a real page (ResetIdentity claims kNumSlots of them) and there
  // is no sentinel anywhere, so the sky walk below finds no EMPTY chunk. This
  // used to compare against PoolPages(), which was kNumSlots until
  // 2026-08-30; the pool is kNumSlots + kPageRetireCeiling since, so the
  // test never fired and the gate failed standalone with "no EMPTY chunk in
  // the column" — the identity map, not a page-pool problem. In-suite the
  // previous gates have long since generated and demoted, so this never
  // fires there.
  if (paged && pt.PagesInUse() >= kNumSlots) {
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
  }

  // ---- FIND the empty sky; do not assume where it is ----------------------
  //
  // This used to be `origin.y + 24 chunks` on the theory that 384 voxels up is
  // above any terrain. It is not a theory this gate can hold: `page-roundtrip`
  // runs AFTER `streaming`, which flies the player out past x = 600, so the
  // window has moved and the "provably empty" column sits in open procedural
  // terrain that neither this file nor the constant knows the height of. Any
  // better constant has the same defect — it would be a second, unowned copy of
  // the terrain band, and the terrain overhaul moves that band by an order of
  // magnitude.
  //
  // So ask the page table, which already knows: walk DOWN the chunk column from
  // the top of the window for the first EMPTY sentinel. That is what "empty sky"
  // means, stated as the property instead of as a coordinate. Failing when the
  // whole column has none is itself a new and meaningful assertion — a residency
  // window with no empty chunk anywhere above the player is a page-pool problem
  // this gate would otherwise have painted over.
  const IVec3 o = world.WindowOrigin();
  const int scx = o.x + (int)kNChunk / 2;
  const int scz = o.z + (int)kNChunk / 2;
  int skyCy = -1;
  for (int cy = o.y + (int)kNChunk - 1; cy >= o.y; cy--) {
    const uint32_t s = World::SlotChunkIndex({scx, cy, scz});
    if (world.PageEntryOfSlot(s) == kPtEmpty) { skyCy = cy; break; }
  }
  if (skyCy < 0) {
    detail = Format(
        "no EMPTY chunk anywhere in the column above (%d,%d): the residency "
        "window has no open sky to paint into, which is a page-pool or "
        "worldgen-height problem, not a paging one",
        scx, scz);
    return Status::Fail;
  }
  const IVec3 sky{scx * (int)kChunk + 8, skyCy * (int)kChunk + 8,
                  scz * (int)kChunk + 8};
  const uint32_t skySlot =
      World::SlotChunkIndex({sky.x >> 4, sky.y >> 4, sky.z >> 4});

  uint32_t t = 900000;  // past any other gate's tick range
  auto tick = [&](const std::vector<BrushOp>& ops) {
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, ops, {}, {}, false,
               {sky.x >> 4, sky.y >> 4, sky.z >> 4}, true, false);
    ctx.WaitIdle();
  };

  // ---- step 1: ALLOC -----------------------------------------------------
  // The target chunk must start as a sentinel in paged mode: it is open sky.
  const bool wasSentinel =
      (world.PageEntryOfSlot(skySlot) & kPtSentinelBit) != 0u;
  const uint32_t before = pt.PagesInUse();
  tick({{sky.x, sky.y, sky.z, 4, kMatSand, 1, 0, 0}});
  const bool nowResident =
      (world.PageEntryOfSlot(skySlot) & kPtSentinelBit) == 0u;
  const uint32_t afterAlloc = pt.PagesInUse();

  // ---- step 2: CONTENT ---------------------------------------------------
  // Read the chunk back THROUGH the translation and assert the paint landed.
  // This is the assertion that a write into a chunk that was a sentinel one
  // tick earlier actually reached memory rather than being no-oped.
  std::vector<uint32_t> chunk(kChunkVol, 0);
  ReadVoxelsSync(ctx, world, skySlot, 1, chunk.data(), "prtRead");
  uint32_t painted = 0;
  for (uint32_t w : chunk)
    if ((w & 0xFFFu) == kMatSand) painted++;

  // ---- step 8: CPU/GPU SYNTHESIS AGREEMENT (§4.4 step 8) -----------------
  // SynthWord (world.h) and synthWord (common.wgsl) are the two halves of one
  // contract — the hash contract. EMPTY's half is the strong one and it is
  // checkable directly: synthWord(PT_EMPTY) must be exactly 0, which is why a
  // materialized EMPTY page is a plain fillBuffer(0).
  const bool synthEmptyZero = SynthWord(kPtEmpty) == 0u;
  bool synthAgrees = true;
  {
    // An untouched sky slot next door is still a sentinel; read it through the
    // seam (which synthesizes CPU-side) and assert every word matches the rule.
    const uint32_t nbrSlot = World::SlotChunkIndex(
        {(sky.x >> 4) + 2, sky.y >> 4, sky.z >> 4});
    std::vector<uint32_t> nbr(kChunkVol, 0);
    ReadVoxelsSync(ctx, world, nbrSlot, 1, nbr.data(), "prtSynth");
    const uint32_t entry = world.PageEntryOfSlot(nbrSlot);
    if ((entry & kPtSentinelBit) != 0u) {
      const uint32_t want = SynthWord(entry);
      for (uint32_t w : nbr)
        if (w != want) { synthAgrees = false; break; }
    }
  }

  // ---- step 3: FREE WITH HYSTERESIS -------------------------------------
  // Erase the ball, then tick past the threshold. The hysteresis is the thing
  // under test, so assert the page did NOT come back before it — a gate that
  // only checked the end state would pass with hysteresis removed.
  tick({{sky.x, sky.y, sky.z, 6, kMatAir, 1, 0, 0}});
  bool heldThroughHysteresis = true;
  const int kFreeTicks = 8;  // mirrors kPageFreeTicks (pagetable.h)
  for (int i = 0; i < kFreeTicks + 6; i++) {
    tick({});
    if (i < kFreeTicks - 2 && paged &&
        (world.PageEntryOfSlot(skySlot) & kPtSentinelBit) != 0u)
      heldThroughHysteresis = false;  // freed too early
  }
  const uint32_t afterFree = pt.PagesInUse();
  // The free assertion is SLOT-LEVEL: the painted chunk's entry is a sentinel
  // again once hysteresis has run. It was originally a global count
  // (afterFree <= afterAlloc), which is the wrong invariant on a live world:
  // this gate runs after the spells gate, whose fires are still spreading, so
  // background materialization legitimately outpaces the one freed page and
  // the count RISES while the roundtrip under test works perfectly. The
  // slot-level form is also strictly stronger for the property under test —
  // the global count could pass with this chunk never freed at all, carried
  // by unrelated demotions. (Same class as gotcha "a world-wide sweep must
  // assert invariants, not that every stain is blood".)
  //
  // The wait is BOUNDED, not fixed: free probes are capped per tick
  // (kMaxFreeProbesPerTick), so on a live world this chunk queues behind
  // whatever demotion backlog the previous gates left, and a fixed 6-tick
  // grace reads a working mechanism as a leak. The early-free assertion
  // above already pinned the hysteresis floor; this loop just gives the
  // capped drain time to reach our slot.
  bool freedAtEnd = (world.PageEntryOfSlot(skySlot) & kPtSentinelBit) != 0u;
  for (int i = 0; i < 400 && paged && !freedAtEnd; i++) {
    tick({});
    freedAtEnd = (world.PageEntryOfSlot(skySlot) & kPtSentinelBit) != 0u;
  }

  // ---- step 7: pageFaults == 0 ------------------------------------------
  // The assertion that turns §2.4's structural claim into evidence. Read
  // directly rather than via the snapshot so it covers this gate's own ticks.
  uint32_t faults = 0;
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.pageFaults, 0, &faults, 4,
                        "prtFaults");

  // Verdict. In DENSE mode there are no sentinels by construction, so the
  // alloc/free assertions are vacuous and only the content, synthesis and
  // fault assertions apply — which is right: dense is the oracle, and what it
  // must prove is that the translation path produces the same voxels.
  const bool allocOk =
      !paged || (wasSentinel && nowResident && afterAlloc > before);
  const bool freeOk = !paged || freedAtEnd;
  const bool ok = allocOk && freeOk && painted > 0 && faults == 0 &&
                  synthEmptyZero && synthAgrees && heldThroughHysteresis;

  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "%s: sky slot %u sentinel->resident %d->%d, pages %u->%u->%u, "
                "%u sand voxels painted through the table, hysteresis held=%d "
                "freed at end=%d, synthWord(EMPTY)==0 %d, CPU/GPU synth agree "
                "%d, pageFaults %u",
                paged ? "paged" : "dense", skySlot, (int)wasSentinel,
                (int)nowResident, before, afterAlloc, afterFree, painted,
                (int)heldThroughHysteresis, (int)(!paged || freedAtEnd),
                (int)synthEmptyZero, (int)synthAgrees, faults);
  detail = buf;
  return ok ? Status::Pass : Status::Fail;
}

// ---- daylight-boundary, Gate D (PLAN_page_table.md §3.2a / §4.4) ---------
//
// THE SUITE STRUCTURALLY CANNOT DO WITHOUT THIS, and it is new value even
// without paging: Simulation::EncodeWakeAll has ZERO test coverage today.
// Every day/night gate PINS the phase (dayNight.freeze = 1 at midnight and at
// noon), so wasDay != isDay is never true anywhere in the suite and the
// wake-all path — which sets all 32,768 dirty flags — has never once run under
// test.
//
// It is also the case that decides whether §3.8's fatal abort is reachable in
// normal play: without the "intersect nonSentinel" filter on the
// materialization set, a wake-all would demand 32,768 pages from an
// 8,192-page pool and crash TWICE PER IN-GAME DAY.
Status GateDaylightBoundary(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  PageTable& pt = *world.pages;
  const bool paged = world.residency == World::Residency::Paged;

  // Run the cycle UNFROZEN — the whole point — and short, so a boundary is
  // guaranteed inside the tick budget.
  const Tuning saved = CurrentTuning();
  Tuning tun = saved;
  tun.dayNight.freeze = 0;
  tun.dayNight.cycleMinutes = 1;   // 1800 ticks per day at 30 Hz
  SetCurrentTuning(tun);

  const uint32_t tpd = TicksPerDay(tun);
  uint32_t t = 800000;
  uint32_t crossings = 0;
  uint32_t peakPages = pt.PagesInUse();
  bool everExhausted = false;

  bool prevDay = DaylightStrengthCpu(DayPhaseForTick(t, tpd, false, 0)) > 0;
  for (uint32_t i = 0; i < tpd; i++) {
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false,
               {8, 3, 8}, true, false);
    const bool isDay =
        DaylightStrengthCpu(DayPhaseForTick(t, tpd, false, 0)) > 0;
    if (isDay != prevDay) crossings++;
    prevDay = isDay;
    peakPages = std::max(peakPages, pt.PagesInUse());
    // Only meaningful in PAGED mode: dense is the identity map, so
    // pagesInUse_ == PoolPages() by construction and always has been.
    if (paged && pt.PagesInUse() >= pt.PoolPages()) everExhausted = true;
    if (crossings >= 1 && i > 64) break;   // a boundary plus settle time
  }
  ctx.WaitIdle();

  // (a) the hash matches a dense run — carried by the suite running in both
  //     residency modes rather than re-derived here.
  // (b) pagesInUse_ < kPoolPages THROUGHOUT: the abort must stay unreachable.
  // (c) pageFaults == 0: nothing the wake made dirty was written unmaterialized.
  // (d) chunks return to sleep afterwards.
  uint32_t faults = 0;
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.pageFaults, 0, &faults, 4,
                        "dayFaults");
  const uint32_t active = ReadActiveChunksSync(ctx, world, sim);
  const uint32_t hash = HashWorldNow(ctx, world, sim, kDefaultSeed);

  SetCurrentTuning(saved);

  const bool ok = crossings >= 1 && !everExhausted && faults == 0;
  char buf[400];
  std::snprintf(buf, sizeof(buf),
                "%s: %u daylight crossing(s) with freeze OFF (EncodeWakeAll "
                "fired), peak pages %u / %u, exhausted=%d, pageFaults %u, "
                "%u chunks active after, hash %08x",
                paged ? "paged" : "dense",
                crossings, peakPages, pt.PoolPages(), (int)everExhausted,
                faults, active, hash);
  detail = buf;
  return ok ? Status::Pass : Status::Fail;
}

// ---- simd: the derived vector forms equal their scalar definitions --------
//
// sim/scan.h and sim/rng_simd.h are strength reductions of loops that classify
// and verify whole chunks (PageTable::Classify, the hysteresis free probe).
// They are guarded by #if defined(__AVX2__), and A `#if` PROVES NOTHING ABOUT
// THE BRANCH IT DID NOT COMPILE — so this gate compares the two IN THE SAME
// BINARY rather than trusting that two build configurations agree.
//
// Two things here are genuinely new arithmetic rather than a mechanical
// substitution, and they are the two this sweeps hardest:
//   - Pcg8's vpsrlvd, which agrees with C++ `>>` only because (s>>28)+4 is
//     always < 32. Every one of the 16 possible (s>>28) values is exercised.
//   - Mod3_8's magic-number division by 3.
//
// It also exercises FirstIndexWhereMasked's SCALAR TAIL, which no production
// call site can reach: every real caller passes n == kChunkVol (a multiple of
// 8), so without this the tail would be untested code that ships.
Status GateSimd(Ctx&, std::string& detail) {
  uint32_t checked = 0, bad = 0;
  std::string firstBad;

  // ---- (a) FirstIndexWhereMasked vs its scalar definition ----------------
  // Lengths straddling the 8-wide block, and a mismatch planted at EVERY
  // index in turn (plus the no-mismatch case), against several masks.
  {
    const uint32_t masks[] = {0xFFFFFFFFu, 0xFF000FFFu, 0x00000FFFu, 0u};
    const size_t lens[] = {0, 1, 7, 8, 9, 15, 16, 17, 31, 32, 33, 4096};
    std::vector<uint32_t> buf(4096);
    for (uint32_t mask : masks)
      for (size_t n : lens) {
        // planted == n means "no mismatch anywhere".
        for (size_t planted = 0; planted <= n; planted++) {
          for (size_t i = 0; i < n; i++) buf[i] = 0xAAAA5555u;
          const uint32_t want = 0xAAAA5555u & mask;
          if (planted < n) buf[planted] = 0xAAAA5555u ^ 0x1001u;
          const size_t got =
              scan::FirstIndexWhereMasked(buf.data(), n, mask, want);
          const size_t exp =
              scan::ScalarFirstIndexWhereMasked(buf.data(), n, mask, want);
          checked++;
          if (got != exp) {
            bad++;
            if (firstBad.empty()) {
              char b[192];
              std::snprintf(b, sizeof(b),
                            "scan mask %08x n %zu planted %zu: got %zu want %zu",
                            mask, n, planted, got, exp);
              firstBad = b;
            }
          }
          if (n > 64 && planted > 40) break;  // 4096 x 4096 is not the point
        }
      }
  }

#if defined(__AVX2__)
  // ---- (b) Pcg8 vs rng::Pcg ---------------------------------------------
  // Edge words, then a stride that walks the whole u32 range so every value of
  // the (s>>28) shift selector is hit many times over.
  {
    std::vector<uint32_t> probe = {0u, 1u, 2u, 7u, 0xFFFFFFFFu, 0xFFFFFFFEu,
                                   0x80000000u, 0x7FFFFFFFu, 0xC0FFEEu,
                                   747796405u, 2891336453u, 277803737u};
    // Force each of the 16 (s>>28) selectors: invert Pcg's first step so s
    // lands in a chosen top nibble. s = v*A + B  =>  v = (s - B) * A^-1.
    // A^-1 mod 2^32 for A = 747796405 is 3425556037.
    for (uint32_t nib = 0; nib < 16; nib++) {
      const uint32_t s = (nib << 28) | 0x0123456u;
      probe.push_back((uint32_t)((s - 2891336453u) * 3425556037u));
    }
    for (uint64_t v = 0; v < 0x100000000ull; v += 0x3B9ACAull)  // ~1,100 samples
      probe.push_back((uint32_t)v);
    for (size_t i = 0; i + 8 <= probe.size(); i += 8) {
      const __m256i in = _mm256_loadu_si256((const __m256i*)(probe.data() + i));
      uint32_t out[8];
      _mm256_storeu_si256((__m256i*)out, rng::Pcg8(in));
      for (int k = 0; k < 8; k++) {
        const uint32_t exp = rng::Pcg(probe[i + k]);
        checked++;
        if (out[k] != exp) {
          bad++;
          if (firstBad.empty()) {
            char b[160];
            std::snprintf(b, sizeof(b), "Pcg8(%08x): got %08x want %08x",
                          probe[i + k], out[k], exp);
            firstBad = b;
          }
        }
      }
    }
  }

  // ---- (c) JitterStateInRow8 vs JitterStateInRow (covers Mod3_8) ---------
  // Real row seeds at real coordinates, including negative ones — the world
  // is signed and the (uint32_t) cast of a negative coord is the documented
  // two's-complement path both forms must take.
  {
    const int ys[] = {-4097, -16, -1, 0, 1, 15, 16, 4096};
    const int zs[] = {-4097, -16, -1, 0, 1, 15, 16, 4096};
    const int xs[] = {-4096, -17, 0, 16, 1024};
    const uint32_t seeds[] = {0u, 1u, 0xC0FFEEu, 0xDEADBEEFu};
    const __m256i lane = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    for (uint32_t seed : seeds)
      for (int y : ys)
        for (int z : zs)
          for (int xb : xs) {
            const uint32_t rowSeed = JitterRowSeed(y, z, seed);
            uint32_t out[8];
            _mm256_storeu_si256(
                (__m256i*)out,
                rng::JitterStateInRow8(
                    rowSeed, _mm256_add_epi32(_mm256_set1_epi32(xb), lane),
                    seed));
            for (int k = 0; k < 8; k++) {
              const uint32_t exp = JitterStateInRow(rowSeed, xb + k, seed);
              checked++;
              if (out[k] != exp) {
                bad++;
                if (firstBad.empty()) {
                  char b[192];
                  std::snprintf(b, sizeof(b),
                                "jitter seed %08x y %d z %d x %d: got %u want %u",
                                seed, y, z, xb + k, out[k], exp);
                  firstBad = b;
                }
              }
            }
          }
  }
#endif

  char buf[320];
  std::snprintf(buf, sizeof(buf),
                "%s path: %u derived==definition checks, %u mismatch%s%s%s",
                scan::kHaveAvx2 ? "AVX2" : "SCALAR-ONLY (no __AVX2__)", checked,
                bad, bad == 1 ? "" : "es", firstBad.empty() ? "" : " | first: ",
                firstBad.c_str());
  detail = buf;
  return bad == 0 ? Status::Pass : Status::Fail;
}

// ---- fire-down: can fire travel DOWNWARD at all? ---------------------------
//
// THE BUG THIS EXISTS FOR. Nothing in the world had a downward heat path.
// `fire` is a gas with rise probability 1024/1024 in calm air (gasIntent,
// sim_step.wgsl) and every emit rule in reactions.json was `dir: up`, so the
// only way heat ever reached a voxel from above was a STATIONARY tag:hot solid
// touching it. Wood had one (`ember`); foliage did not, because
// `leaves + tag:hot` produced `fire` directly — i.e. a burnt leaf converted
// itself into the one thing that immediately leaves the neighbourhood. Owner
// report, two symptoms and one cause: a lit tree cooks the top of its crown
// and never touches the bottom layer of leaves, and a character whose torso
// burns through never lights his own legs.
//
// KNOWN-FAILING SINCE 2026-09-03, by the owner's choice: fire became a weak
// igniter (neighborChance, gate weak-flame) and arm B fell from saturating
// to ~9% with it, because the flame emitted downward IS fire and was the only
// path across a gap. A falling non-floating product would restore the arm;
// the owner declined one. Arm A (conduction) still saturates.
//
// TWO ARMS, because the fix is two mechanisms and either one alone would let a
// one-armed gate pass while half the bug survived:
//
//   A. CONDUCTION, through solid fuel. A leaf slab lit across its TOP face
//      must burn all the way to its BOTTOM layer. This is the `leaf_burning`
//      stage: a stationary tag:hot solid that cooks all six of its faces for
//      the length of its burn. Note there is no flame in this arm at all —
//      RK_EMIT only fires into an AIR neighbour, and the interior of a slab
//      has none — so it measures conduction and nothing else.
//
//   B. EMISSION, one cell down through air. A second slab sits below the first
//      with ONE cell of air between them, and must catch. Nothing conducts
//      across air, so this arm can only pass through the `dir: down` emit
//      siblings, and it is the exact analogue of the body case: MobSystem's
//      limb lattices cannot see each other, so a burning torso reaches the
//      legs ONLY by putting fire into the world cell between them.
//
// WHY ONE CELL AND NOT TWO. Because one cell is the whole claim, and a gate
// that asserted more would be asserting a feature nothing here implements.
// Measured with a two-cell gap: 0%, and the arithmetic says why rather than
// leaving it a mystery. A flame RISES every tick it lives, so it gets exactly
// one roll at each altitude on its way out; giving `fire` its own downward
// emit therefore buys ~0.05 cells of descent, not the ~0.33 a stationary
// emitter would get, and no survivable chance closes a two-cell gap (the
// self-replication ceiling is chance 138, where a fire never goes out at all).
// Heat crossing OPEN AIR at range is radiant transfer, which this engine has
// never modelled and which is not what either reported symptom needed.
//
// WHY A SLAB AND NOT A COLUMN. A 1-wide column of leaves is a 1-D chain with
// one link per voxel, and at any survivable per-link probability it dies out —
// under the fix as surely as without it. A slab gives an interior voxel the
// six neighbours the authored chances were written against, which is the
// geometry a canopy actually has.
//
// FLOATED IN CLEAR AIR, well above the terrain, so the arms measure the fuel
// and not whatever the ground under the fixture happens to be, and the world
// is regenerated on the way out (rule 7: the gates after this one still expect
// pristine terrain).
Status GateFireDown(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  auto matId = [&](const char* n) -> uint32_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == n) return (uint32_t)i;
    return 0;
  };
  const uint32_t mLeaf = matId("leaves"), mEmber = matId("ember"),
                 mBurn = matId("leaf_burning");
  if (!mLeaf || !mEmber || !mBurn) {
    detail = "leaves, ember or leaf_burning missing from materials.json";
    return Status::Fail;
  }

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // Geometry. Two slabs, W x W in plan, kDeep tall, separated by kGap of air.
  // The igniter is a layer of `ember` laid ON TOP of the upper slab: a
  // stationary heat source, so the arms measure how far the fire TRAVELS
  // rather than how long the match was held.
  const int W = 9, kDeep = 8, kGap = 1;
  const int cx = 200, cz = 200, R = W / 2;
  const int baseY = FixtureYOver(cx - R - 1, cz - R - 1, cx + R + 1, cz + R + 1,
                                 kDefaultSeed, 24);
  const int loTop = baseY + kDeep - 1;              // top layer of the LOWER slab
  const int hiBot = baseY + kDeep + kGap;           // bottom layer of the UPPER slab
  const int hiTop = hiBot + kDeep - 1;

  std::vector<CellOp> scene;
  auto put = [&](int x, int y, int z, uint32_t m) {
    scene.push_back({World::SlotCellIndex({x, y, z}), m & 0xFFFu});
  };
  for (int z = -R; z <= R; z++)
    for (int x = -R; x <= R; x++) {
      for (int y = 0; y < kDeep; y++) {            // lower slab
        put(cx + x, baseY + y, cz + z, mLeaf);
        put(cx + x, hiBot + y, cz + z, mLeaf);     // upper slab
      }
      for (int y = 0; y < kGap; y++)               // the gap must be AIR
        put(cx + x, baseY + kDeep + y, cz + z, 0u);
      put(cx + x, hiTop + 1, cz + z, mEmber);      // the match
    }

  uint32_t t = 1;
  SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, scene, false,
             {12, 12, 12}, false, false);
  ctx.WaitIdle();

  // Long enough for a front to cross both slabs and the gap at the authored
  // rates, and short enough to stay a ~10 s gate. A layer transition is order
  // 20 ticks (a leaf catches at 25 per-mille per hot face and leaf_burning
  // exposes it for ~12.5), so 18 layers of travel wants several hundred.
  const uint32_t kTicks = 900;
  for (uint32_t i = 2; i <= kTicks; i++)
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false,
               {12, 12, 12}, false, false);
  ctx.WaitIdle();

  std::vector<uint32_t> vox(kNumSlots * (size_t)kChunkVol);
  ReadVoxelsSync(ctx, world, 0, kNumSlots, vox.data(), "fireDownRead");
  auto at = [&](int x, int y, int z) {
    return vox[World::SlotCellIndex({x, y, z})] & 0xFFFu;
  };

  // "Touched by fire" is `no longer leaves`. Leaves are consumed to air (or
  // are mid-burn as leaf_burning); nothing else in the fixture can remove
  // them, and counting the leaves LEFT rather than the products is what keeps
  // the assertion independent of which decay branch won.
  auto layerBurnt = [&](int y) {
    uint32_t gone = 0;
    for (int z = -R; z <= R; z++)
      for (int x = -R; x <= R; x++)
        if (at(cx + x, y, cz + z) != mLeaf) gone++;
    return gone;
  };
  const uint32_t cells = (uint32_t)(W * W);
  const uint32_t hiTopGone = layerBurnt(hiTop), hiBotGone = layerBurnt(hiBot);
  const uint32_t loTopGone = layerBurnt(loTop), loBotGone = layerBurnt(baseY);

  const double floorPct = BaselineNumber("fireDownLayerPctMin", 60.0);
  auto pct = [&](uint32_t n) { return 100.0 * (double)n / (double)cells; };

  // Arm A fails as "the front stalled inside the slab", arm B as "the front
  // never crossed the gap" — and the top layer is asserted too, so a fixture
  // that never lit at all reports THAT rather than looking like a spread bug.
  const bool lit = pct(hiTopGone) >= floorPct;
  const bool armA = pct(hiBotGone) >= floorPct;
  const bool armB = pct(loTopGone) >= floorPct;
  const bool armBDeep = pct(loBotGone) >= floorPct;

  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "%s: lit %.0f%% | A conduct (slab bottom) %.0f%% | B emit "
                "(down %d cell of air) %.0f%% | lower slab bottom %.0f%% "
                "[floor %.0f%%, %u ticks, %dx%d slabs %d deep]",
                (lit && armA && armB && armBDeep) ? "PASS" : "FAIL",
                pct(hiTopGone), pct(hiBotGone), kGap, pct(loTopGone),
                pct(loBotGone), floorPct, kTicks, W, W, kDeep);
  detail = buf;
  std::printf("fire-down: %s\n", buf);

  // Rule 7: leave pristine terrain for the gates after this one.
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  return (lit && armA && armB && armBDeep) ? Status::Pass : Status::Fail;
}

// ---- rain-fire: the weather reaches the world through ONE integer ---------
//
// weather::SimRainWord puts rain (and the ground wetness it leaves) on the tick
// stream; reactions authored "rain" douse a burning cell the rain can reach and
// reactions authored "rainDamped" slow an exposed ignition. This gate lights
// the SAME fire under three pinned skies and asserts the ordering the feature
// promises: a storm puts it out, a drizzle slows it, a clear sky does neither.
//
// The fuel is a ONE-VOXEL-THICK sheet of leaves floating in clear air, and the
// thinness is the point: every leaf has air above it, so every leaf is
// rain-exposed (sim_step.wgsl rainExposed). A solid slab would measure its own
// interior — which the rain correctly cannot reach — rather than the rain.
// The match is a 3x3 pad of ember on the sheet's centre.
//
// Also asserted: the word itself — no rain and no wetness under a clear pin, rain > 0 under a storm,
// and a drizzle's wetness above its rain (the leaky integral is what keeps
// ground slow to catch after a shower). The pin is restored on the way out,
// and the world regenerated (rule 7).
Status GateRainFire(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  auto matId = [&](const char* n) -> uint32_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == n) return (uint32_t)i;
    return 0;
  };
  const uint32_t mLeaf = matId("leaves"), mEmber = matId("ember");
  if (!mLeaf || !mEmber) {
    detail = "leaves or ember missing from materials.json";
    return Status::Fail;
  }
  const std::string prevPin = weather::Override();
  const Tuning& tun = CurrentTuning();

  const int W = 25, R = W / 2, cx = 200, cz = 200;
  const uint32_t kTicks = (uint32_t)BaselineNumber("rainFireTicks", 400.0);

  struct Arm { const char* sky; uint32_t word; uint32_t burnt; };
  Arm arms[3] = {{"clear", 0, 0}, {"drizzle", 0, 0}, {"storm", 0, 0}};
  for (Arm& a : arms) {
    weather::SetOverride(a.sky);
    a.word = weather::SimRainWord(tun, kDefaultSeed, 1);
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    const int y = FixtureYOver(cx - R - 1, cz - R - 1, cx + R + 1, cz + R + 1,
                               kDefaultSeed, 24);
    std::vector<CellOp> scene;
    for (int z = -R; z <= R; z++)
      for (int x = -R; x <= R; x++) {
        scene.push_back({World::SlotCellIndex({cx + x, y, cz + z}), mLeaf});
        if (std::abs(x) <= 1 && std::abs(z) <= 1)
          scene.push_back({World::SlotCellIndex({cx + x, y + 1, cz + z}), mEmber});
      }
    uint32_t t = 1;
    SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, scene, false,
               {12, 12, 12}, false, false);
    for (uint32_t i = 2; i <= kTicks; i++)
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false,
                 {12, 12, 12}, false, false);
    ctx.WaitIdle();
    std::vector<uint32_t> vox(kNumSlots * (size_t)kChunkVol);
    ReadVoxelsSync(ctx, world, 0, kNumSlots, vox.data(), "rainFireRead");
    for (int z = -R; z <= R; z++)
      for (int x = -R; x <= R; x++)
        if ((vox[World::SlotCellIndex({cx + x, y, cz + z})] & 0xFFFu) != mLeaf)
          a.burnt++;
  }
  weather::SetOverride(prevPin);
  weather::Snap();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  const uint32_t cells = (uint32_t)(W * W);
  auto pct = [&](uint32_t n) { return 100.0 * (double)n / (double)cells; };
  auto rain = [](uint32_t w) { return w & 0xFFu; };
  auto wet = [](uint32_t w) { return (w >> 16) & 0xFFu; };
  const Arm &clr = arms[0], &drz = arms[1], &stm = arms[2];
  const double clearMin = BaselineNumber("rainFireClearPctMin", 60.0);
  const double stormMaxFrac = BaselineNumber("rainFireStormMaxFrac", 0.5);
  const bool wordOk = rain(clr.word) == 0 && wet(clr.word) == 0 &&
                      rain(stm.word) > rain(drz.word) &&
                      rain(drz.word) > 0 && wet(drz.word) >= rain(drz.word);
  const bool lit = pct(clr.burnt) >= clearMin;
  const bool slowed = drz.burnt < clr.burnt;
  const bool doused = (double)stm.burnt <= stormMaxFrac * (double)clr.burnt;
  const bool ok = wordOk && lit && slowed && doused;

  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "%s: burnt after %u ticks — clear %.0f%% | drizzle %.0f%% | "
                "storm %.0f%% [clear >= %.0f%%, drizzle < clear, storm <= "
                "%.2fx clear] | word rain/wet: clear %u/%u drizzle %u/%u storm "
                "%u/%u%s",
                ok ? "PASS" : "FAIL", kTicks, pct(clr.burnt), pct(drz.burnt),
                pct(stm.burnt), clearMin, stormMaxFrac, rain(clr.word),
                wet(clr.word), rain(drz.word), wet(drz.word), rain(stm.word),
                wet(stm.word), wordOk ? "" : " (WORD WRONG)");
  detail = buf;
  std::printf("rain-fire: %s\n", buf);
  return ok ? Status::Pass : Status::Fail;
}

// ---- weak-flame: the floating flame ignites at a fraction of the coals' rate --
//
// Every ignition rule in reactions.json is `neighbor: tag:hot`, and `fire` --
// the gas that rises off every burning voxel and drifts through the air --
// carries that tag beside the stationary heat sources, so one drifting flame
// used to light a tree or a robe at the same rate as a bed of coals pressed
// against it. `"neighborChance": { "fire": 0.125 }` on the ignition rules says
// otherwise, and the loader compiles it AWAY (ExpandNeighborChance,
// sim/materials.cpp) into a base rule on a synthetic hot-minus-fire tag plus
// an exact fire rule at the scaled chance. Nothing on the GPU knows the field
// exists, which is exactly why this gate reads the COMPILED table: a typo in
// the field name, a rule the expansion refused, or a synthetic tag that leaked
// onto fire would each leave the world burning at the old rate under a green
// determinism gate (the hash would simply move to a wrong world).
//
// Pure CPU over c.mats / c.reactions: no world, no GPU, nothing left behind.
// Asserted per fuel, for a spread of fuels (wood, the three foliage families,
// grass, cloth): the base rule must match ember and lava and NOT fire, the
// fire rule must exist at base/ratio, and the two must sit in that order (a
// voxel touching both rolls the full rate first). And the two things the
// exception must NOT have touched: water still steams against fire, skin is
// still seared by it.
Status GateWeakFlame(Ctx& c, std::string& detail) {
  auto matId = [&](const char* n) -> uint32_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == n) return (uint32_t)i;
    return 0;
  };
  const uint32_t mFire = matId("fire"), mEmber = matId("ember"),
                 mLava = matId("lava");
  if (!mFire || !mEmber || !mLava) {
    detail = "fire / ember / lava missing from materials.json";
    return Status::Fail;
  }
  // THE EXPECTED RATIO TRACKS THE KNOB. `weakFlame.ratio` is the ratio
  // reactions.json authors (the reciprocal of its neighborChance multiplier for
  // fire, 0.0625 = 16), and combustion.flamePct is the global strength over
  // that authoring -- so at 200% the flame is twice as dangerous and the ratio
  // is 8, not 16. Restating the authored number here and calling it the answer
  // would make this gate fail the moment anyone moved the slider it exists to
  // protect, which is the opposite of what it is for: it checks that the
  // exception COMPILED, not that nobody touched the tuning.
  const int flamePct = CurrentTuning().combustion.flamePct;
  const double ratio =
      BaselineNumber("weakFlame.ratio", 8.0) * 100.0 / (double)std::max(1, flamePct);
  const uint32_t fireTags = c.mats[mFire].gpu.tagMask;

  struct Fuel { const char* self; const char* product; };
  const Fuel fuels[] = {{"wood", "ember"},
                        {"leaves", "leaf_burning"},
                        {"pine_needles", "pine_burning"},
                        {"autumn_leaves", "autumn_burning"},
                        {"grass", "fire"},
                        {"cloth", "cloth_burning"}};
  std::string fails;
  int checked = 0;
  for (const Fuel& f : fuels) {
    const uint32_t self = matId(f.self), prod = matId(f.product);
    if (!self || !prod) {
      fails += std::string(" ") + f.self + ":missing";
      continue;
    }
    const MaterialGpu& mg = c.mats[self].gpu;
    int baseAt = -1, flameAt = -1;
    for (uint32_t i = 0; i < mg.reactCount; i++) {
      const ReactionGpu& r = c.reactions[mg.reactOffset + i];
      if ((r.packed & 3u) != kReactPair || r.prodSelf != prod) continue;
      if (r.nbrMat == kNbrAny && r.nbrTags != 0 && baseAt < 0) baseAt = (int)i;
      if (r.nbrMat == mFire && flameAt < 0) flameAt = (int)i;
    }
    if (baseAt < 0 || flameAt < 0) {
      fails += std::string(" ") + f.self + ":" +
               (baseAt < 0 ? "no-tag-rule" : "no-fire-rule");
      continue;
    }
    const ReactionGpu& base = c.reactions[mg.reactOffset + baseAt];
    const ReactionGpu& flame = c.reactions[mg.reactOffset + flameAt];
    const bool excludesFire = (fireTags & base.nbrTags) == 0;
    const bool keepsEmber = (c.mats[mEmber].gpu.tagMask & base.nbrTags) != 0;
    const bool keepsLava = (c.mats[mLava].gpu.tagMask & base.nbrTags) != 0;
    const double got = flame.chance > 0
                           ? (double)base.chance / (double)flame.chance : 0.0;
    const bool scaled = std::fabs(got - ratio) < 0.05;
    const bool ordered = baseAt < flameAt;
    if (!(excludesFire && keepsEmber && keepsLava && scaled && ordered)) {
      char b[160];
      std::snprintf(b, sizeof(b), " %s:{fire-in-mask %d, ember %d, lava %d, "
                    "ratio %.2f, order %d}", f.self, !excludesFire, keepsEmber,
                    keepsLava, got, ordered);
      fails += b;
    }
    checked++;
  }

  // The untouched rules: a tag:hot pair rule that must STILL match fire.
  struct Keep { const char* self; const char* product; };
  const Keep keeps[] = {{"water", "steam"}, {"skin", "flesh_cooked"}};
  for (const Keep& k : keeps) {
    const uint32_t self = matId(k.self), prod = matId(k.product);
    if (!self || !prod) { fails += std::string(" ") + k.self + ":missing"; continue; }
    const MaterialGpu& mg = c.mats[self].gpu;
    bool ok = false;
    for (uint32_t i = 0; i < mg.reactCount; i++) {
      const ReactionGpu& r = c.reactions[mg.reactOffset + i];
      if ((r.packed & 3u) != kReactPair || r.prodSelf != prod) continue;
      if (r.nbrTags != 0 && (fireTags & r.nbrTags) != 0) ok = true;
    }
    if (!ok) fails += std::string(" ") + k.self + ":no-longer-reacts-to-fire";
  }

  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "%s: %d fuels ignite from fire at 1/%.0f of the coals' rate "
                "(flamePct %d%%)%s%s",
                fails.empty() ? "PASS" : "FAIL", checked, ratio, flamePct,
                fails.empty() ? "" : " |", fails.c_str());
  detail = buf;
  std::printf("weak-flame: %s\n", buf);
  return fails.empty() ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------- support-flag
// EVERY REMOVAL PATH RAISES THE SUPPORT FLAG, not just the CA.
//
// Solids never move in the CA (sim_step returns early for CLASS_SOLID), so the
// only thing that can ever drop unsupported rock is island detection — and the
// only thing that summons island detection is a per-chunk support-loss flag.
// Until the chokepoint landed, `flagSupportLoss` was called from sim_step
// alone: a spell or an exact-cell op that carved a pillar out from under a
// ledge left the ledge hanging with nothing in the engine aware of it.
//
// WHY THE FIXTURE IS ALL STONE. A powder fixture would prove nothing: the CA
// itself flags support loss when a grain slides, so a flag would appear whether
// or not the mutation path raised one. With stone above and stone below and a
// single cell erased between them, the CA has nothing it can do — sim_step
// visits the chunk and returns. Any flag observed here therefore came from
// sim_mutate, which is exactly the claim.
//
// WHY THE ERASED CELL SITS ON A CHUNK BOUNDARY. The second half of the fix:
// flagSupportLoss used to `return` after the FIRST solid neighbour it found,
// so a cell vacating between two solids in DIFFERENT chunks flagged one and
// left the other's matter floating. The erased cell here has a solid directly
// ABOVE it (same chunk) and a solid to its -X (the neighbouring chunk), and
// faceDir's order visits +Y before -X — so the old code flagged the home chunk
// and stopped. Asserting BOTH chunks is what makes this gate see the early
// return rather than just the missing call.
Status GateSupportFlag(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const std::vector<MaterialDef>& mats = c.mats;
  int si = -1;
  for (size_t i = 0; i < mats.size(); i++)
    if (mats[i].name == "stone") si = (int)i;
  if (si < 0) {
    detail = "FAIL: no stone material";
    return Status::Fail;
  }

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // The gap cell sits at a chunk boundary on X so its -X neighbour is in the
  // chunk next door. Anchored over the terrain, never a literal Y (support.h):
  // at a fixed height this fixture was inside bedrock once the datum moved.
  const int gx = 96;                       // 96 = 6*16, a chunk's low X face
  const int gz = 96;
  const int gy = FixtureYOver(gx - 4, gz - 4, gx + 4, gz + 4, kDefaultSeed, 12);

  std::vector<CellOp> scene;
  auto put = [&](int x, int y, int z, int m) {
    scene.push_back({World::SlotCellIndex({x, y, z}),
                     (uint32_t)(m & 0xFFF)});
  };
  // A stone post with a cap, standing clear of the ground, plus one stone
  // voxel hanging off the gap cell's -X face in the neighbouring chunk.
  for (int y = 0; y < 6; y++) put(gx, gy + y, gz, si);   // the post
  put(gx - 1, gy + 2, gz, si);                            // the -X arm
  const int kGapY = gy + 2;                               // the cell to erase

  uint32_t t = 1;
  SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, scene, false,
             {gx >> 4, kGapY >> 4, gz >> 4}, false, false);
  ctx.WaitIdle();

  // Settle, then READ BACK to consume and clear every flag worldgen and the
  // fixture placement itself raised. Support flags accumulate until a readback
  // takes them (world.cpp: copy-then-fill), so without this the count below
  // would include someone else's tick.
  for (int i = 0; i < 8; i++)
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false,
               {gx >> 4, kGapY >> 4, gz >> 4}, i == 7, false);
  ctx.WaitIdle();

  // HOW THE FLAGS ARE COLLECTED, and it is the whole difficulty of testing
  // this buffer.
  //
  // Support flags are ONE-SHOT: world.cpp's readback copies them out and
  // immediately fills the buffer with zeroes. And under paged residency — the
  // default — the harness forces a snapshot readback every <= 4 ticks whether
  // or not the caller asked for one (support.cpp's needSnapshotForPaging: the
  // page mirror starves without it). So a gate CANNOT read world.support
  // directly and expect to find anything: the drain cadence is not the gate's
  // to control, and an earlier draft of this one measured 0 flagged chunks
  // world-wide with a provably intact fixture for exactly that reason.
  //
  // The flags are therefore collected the way their real consumer collects
  // them (DebrisSystem::QueueSupportEvents): union of snap.supportFlags over a
  // window of ticks, so whichever snapshot happens to drain them is caught.
  std::vector<uint8_t> seen(kNumSlots, 0);
  auto collect = [&](int ticks) {
    for (int i = 0; i < ticks; i++) {
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false,
                 {gx >> 4, kGapY >> 4, gz >> 4}, true, false);
      ctx.WaitIdle();
      const WorldSnapshot& sn = world.Snap();
      if (!sn.valid || sn.supportFlags.size() != kNumSlots) continue;
      for (uint32_t ci = 0; ci < kNumSlots; ci++)
        if (sn.supportFlags[ci]) seen[ci] = 1;
    }
  };
  // Baseline window: drain and record everything worldgen and the fixture
  // placement raised, so the assertion below can require that the erase
  // flagged something NEW rather than merely that a flag exists somewhere.
  collect(12);
  std::vector<uint8_t> before = seen;

  const uint32_t homeChunk = World::SlotChunkIndex({gx >> 4, kGapY >> 4, gz >> 4});
  const uint32_t westChunk =
      World::SlotChunkIndex({(gx - 1) >> 4, kGapY >> 4, gz >> 4});
  if (homeChunk == westChunk) {
    detail = "FAIL: fixture is not astride a chunk boundary (gx must be 16-aligned)";
    return Status::Fail;
  }

  // FIXTURE INTEGRITY IS ESTABLISHED HERE, BEFORE THE ERASE, and that ordering
  // is load-bearing since 2026-09-04. The -X arm is a single stone voxel whose
  // only neighbour is the gap cell, so the moment the erase lands the arm IS a
  // one-voxel island — and `soloSolid` in sim_step.wgsl now drops those. Read
  // afterwards, as this census used to be, a correctly-behaving engine reports
  // "the -X arm never landed (fixture missing)" and the gate blames the fixture
  // for the feature it is standing next to. What the gate actually claims (an
  // exact-cell erase between two solids flags BOTH chunks) is untouched; only
  // the moment at which "the fixture was really there" is a meaningful question
  // has moved, from after the mutation to before it.
  std::vector<uint32_t> voxPre(kNumSlots * (size_t)kChunkVol);
  ReadVoxelsSync(ctx, world, 0, kNumSlots, voxPre.data(), "supportVoxPre");
  auto matPre = [&](int x, int y, int z) {
    return voxPre[World::SlotCellIndex({x, y, z})] & 0xFFFu;
  };
  const bool capThere = (int)matPre(gx, gy + 5, gz) == si;
  const bool armThere = (int)matPre(gx - 1, kGapY, gz) == si;

  // THE MUTATION UNDER TEST: one exact-cell op erasing the gap. This is the
  // path island removal, settle-back and the spell VM all write through.
  std::fill(seen.begin(), seen.end(), (uint8_t)0);
  std::vector<CellOp> erase{{World::SlotCellIndex({gx, kGapY, gz}), 0u}};
  SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, erase, false,
             {gx >> 4, kGapY >> 4, gz >> 4}, true, false);
  ctx.WaitIdle();
  {
    const WorldSnapshot& sn = world.Snap();
    if (sn.valid && sn.supportFlags.size() == kNumSlots)
      for (uint32_t ci = 0; ci < kNumSlots; ci++)
        if (sn.supportFlags[ci]) seen[ci] = 1;
  }
  collect(12);
  std::vector<uint8_t> after = seen;

  const bool homeFlagged = after[homeChunk] != 0;
  const bool westFlagged = after[westChunk] != 0;
  // ATTRIBUTION, not a bare pass/fail. A zero here has several very different
  // causes — the fixture never landed, the erase never landed, or the flag was
  // genuinely not raised — and "home=0" alone cannot tell them apart. So the
  // verdict line carries the fixture census and both windows' flag counts.
  uint32_t nBefore = 0, nAfter = 0;
  for (uint32_t i = 0; i < kNumSlots; i++) {
    nBefore += before[i] ? 1u : 0u;
    nAfter += after[i] ? 1u : 0u;
  }
  std::vector<uint32_t> vox(kNumSlots * (size_t)kChunkVol);
  ReadVoxelsSync(ctx, world, 0, kNumSlots, vox.data(), "supportVox");
  auto matAt = [&](int x, int y, int z) {
    return vox[World::SlotCellIndex({x, y, z})] & 0xFFFu;
  };
  // Only the ERASE is read after the fact; `capThere` / `armThere` were taken
  // before it, for the reason given at that read.
  const bool gapOpen = matAt(gx, kGapY, gz) == 0u;

  std::string fails;
  if (!capThere) fails += " the post never landed (fixture missing);";
  if (!armThere) fails += " the -X arm never landed (fixture missing);";
  if (!gapOpen) fails += " the erase op never landed (gap still solid);";
  if (capThere && armThere && gapOpen) {
    if (!homeFlagged)
      fails += " the post's own chunk was not flagged (the mutation path raised nothing);";
    if (!westFlagged)
      fails += " the -X arm's chunk was not flagged (flagSupportLoss stopped at the first neighbour);";
  }

  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "%s: one exact-cell erase between two solids flagged home=%d "
                "west=%d (chunks %u/%u); fixture cap=%d arm=%d gap=%d; "
                "flagged chunks baseline %u -> after-erase %u%s%s",
                fails.empty() ? "PASS" : "FAIL", homeFlagged ? 1 : 0,
                westFlagged ? 1 : 0, homeChunk, westChunk, capThere ? 1 : 0,
                armThere ? 1 : 0, gapOpen ? 1 : 0, nBefore, nAfter,
                fails.empty() ? "" : " |", fails.c_str());
  detail = buf;
  std::printf("support-flag: %s\n", buf);
  return fails.empty() ? Status::Pass : Status::Fail;
}


// ---- snapshot-latency ---------------------------------------------------
//
// THE CLAIM: a gameplay decision made at tick T reads the world at tick
// T - World::kSnapshotLatency, on every machine and at every frame rate.
//
// Before docs/PLAN_multiplayer_now.md N1 that was false in two directions at
// once (RESEARCH_multiplayer_readiness.md L1 / L1'). The game published
// "whatever the last MapAsync callback delivered", so the AGE of the world a
// brush stroke, a mob's ground probe or a spell's ladder read was a function of
// how fast this machine's GPU came back -- one tick on a fast one, several on a
// loaded one, and older still whenever the readback ring saturated and declined
// a copy outright. The harness hid it: SetHarnessSnapshotDrain blocked for the
// map every tick, so `--selftest` pinned a hash for a fixed ONE-tick latency
// that the shipped game never ran at.
//
// So this gate is a DIFFERENTIAL on GPU pacing, which is the only thing that
// can catch the failure it exists for:
//
//   pass A  the harness ordering (drain on): every submit retires before the
//           next tick is encoded. The GPU is never behind.
//   pass B  no drain: ticks pile into the queue exactly as they do in the
//           game's kMaxTicksPerFrame catch-up burst, and the publish is the
//           only thing that ever waits on a fence.
//
// Both must report Snap().tick == t - K on EVERY tick and must produce the
// SAME world hash over the same 64 ticks. A pipeline whose latency is a timing
// property cannot do that; one whose latency is a constant cannot do anything
// else.
Status GateSnapshotLatency(Ctx& c, std::string& detail) {
  constexpr uint32_t kTicks = 64;
  const uint32_t K = World::kSnapshotLatency;
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  // Pinned in tests/baseline.json so K itself and the drained arm's wait budget
  // are tunable without a rebuild (CLAUDE.md: thresholds live in JSON).
  const uint32_t pinnedK = (uint32_t)BaselineNumber("snapshotLatencyK", (double)K);
  const uint64_t drainedWaitBudget =
      (uint64_t)BaselineNumber("snapshotDrainedMaxWaits", 0.0);

  const bool hadDrain = HarnessSnapshotDrain();
  uint32_t finalHash[2] = {};
  uint32_t badCount[2] = {};
  uint32_t firstBadTick[2] = {};
  uint32_t firstBadGot[2] = {};
  World::SnapshotPipeStats pipe[2];

  for (int pass = 0; pass < 2; pass++) {
    // Pass B is the whole point: with the drain off nothing waits for the GPU
    // except the publish itself, so by tick K+1 there are K ticks in the queue
    // -- the kMaxTicksPerFrame case, reproduced without a frame loop.
    SetHarnessSnapshotDrain(pass == 0);
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    ctx.ProcessEvents();
    world.TakeSnapshotPipe();  // zero the counters for this arm
    for (uint32_t t = 1; t <= kTicks; t++) {
      SubmitTick(ctx, world, sim, t, kDefaultSeed, SelftestOps(t, kDefaultSeed),
                 SelftestExps(t, kDefaultSeed), {}, true, {8, 3, 8}, false,
                 SelftestParticlesActive(t));
      // THE INVARIANT, ON EVERY TICK. SubmitWorldgen invalidated the pipeline,
      // so the first readback is tick 1's and the first publishable target is
      // tick 1 -- reached at t = K + 1. Before that Snap() must be INVALID and
      // not merely old: "no snapshot for the first K ticks after a reset" is
      // itself a constant, and a leftover snapshot of the previous gate's world
      // showing through here would be the exact bug this gate exists for.
      const WorldSnapshot& sn = world.Snap();
      const bool wantValid = t >= K + 1;
      const bool ok = wantValid ? (sn.valid && sn.tick == t - K) : !sn.valid;
      if (!ok) {
        if (badCount[pass] == 0) {
          firstBadTick[pass] = t;
          firstBadGot[pass] = sn.valid ? sn.tick : 0xFFFFFFFFu;
        }
        badCount[pass]++;
      }
    }
    pipe[pass] = world.TakeSnapshotPipe();
    finalHash[pass] = ReadHashSync(ctx, world);
  }
  SetHarnessSnapshotDrain(hadDrain);

  std::string fails;
  auto fail = [&](const std::string& m) {
    if (!fails.empty()) fails += "; ";
    fails += m;
  };
  if (K != pinnedK)
    fail(Format("K is %u, baseline pins %u", K, pinnedK));
  for (int pass = 0; pass < 2; pass++) {
    const char* nm = pass == 0 ? "drained" : "deep-queue";
    if (badCount[pass])
      fail(Format("%s: %u of %u ticks read the wrong snapshot (first at tick "
                  "%u: got %d, wanted %d)",
                  nm, badCount[pass], kTicks, firstBadTick[pass],
                  (int)firstBadGot[pass],
                  (int)(firstBadTick[pass] - K)));
    // A DECLINE BREAKS THE CONSTANT, so it is a failure and not a statistic:
    // the tick that got no copy has no snapshot to hand over K ticks later.
    if (pipe[pass].declines)
      fail(Format("%s: ring declined %llu readbacks", nm,
                  (unsigned long long)pipe[pass].declines));
    if (pipe[pass].missing > K)
      fail(Format("%s: %llu ticks had no snapshot at their target (expected %u,"
                  " the post-reset warm-up)",
                  nm, (unsigned long long)pipe[pass].missing, K));
  }
  // The drained arm waits for the whole device after every submit, so a wait
  // here means the publish could not find a snapshot that had provably already
  // landed -- a pipeline bug, not a slow GPU.
  if (pipe[0].waits + pipe[0].slotWaits > drainedWaitBudget)
    fail(Format("drained arm blocked %llu times (budget %llu)",
                (unsigned long long)(pipe[0].waits + pipe[0].slotWaits),
                (unsigned long long)drainedWaitBudget));
  // THE DIFFERENTIAL. Same seed, same ops, same 64 ticks, two different GPU
  // pacings. Equal hashes is the property "the world does not depend on how far
  // behind the GPU is", which is what a replicated sim needs and what a
  // freshest-wins snapshot cannot give.
  if (finalHash[0] != finalHash[1])
    fail(Format("pacing changed the world: drained %08x vs deep-queue %08x",
                finalHash[0], finalHash[1]));

  RecordObserved("snapshotLatencyK", (double)K);
  RecordObserved("snapshotDrainedMaxWaits",
                 (double)(pipe[0].waits + pipe[0].slotWaits));

  // The deep-queue arm's wait count is REPORTED, never asserted: a headless
  // loop submits a tick and immediately needs the snapshot from K ticks ago
  // with no frame of real work in between, so it blocks by construction. The
  // wait budget that matters is the GAME's, and it is measured with
  // `--frames 600 --autofly-surface` (package N1's kill criterion), not here.
  detail = Format(
      "%s (K=%u, %u ticks x2 | drained: %llu waits, %llu slot-waits | "
      "deep-queue: %llu waits, %llu slot-waits | hash %08x both%s%s)",
      fails.empty() ? "PASS" : "FAIL", K, kTicks,
      (unsigned long long)pipe[0].waits, (unsigned long long)pipe[0].slotWaits,
      (unsigned long long)pipe[1].waits, (unsigned long long)pipe[1].slotWaits,
      finalHash[0], fails.empty() ? "" : " | ", fails.c_str());
  std::printf("snapshot-latency: %s\n", detail.c_str());

  // Leave pristine terrain: this gate regenerated twice and then ran 64 ticks
  // of the selftest op stream over it (CLAUDE.md rule 7 / the kOrder note).
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  return fails.empty() ? Status::Pass : Status::Fail;
}

// ---- ops-replay --------------------------------------------------------
//
// THE TWO CLAIMS, and they are different claims (docs/PLAN_multiplayer_now.md
// N3):
//
//   A. A RECORDED SESSION REPLAYS TO THE SAME HASH. The op stream plus the
//      per-tick TickParams is a COMPLETE description of a tick's input: feed
//      the record back into the same SubmitTick and the world walks the same
//      path. CLAUDE.md rule 3 has claimed this for the op stream since the
//      beginning; nothing tested it, and nothing could, because the queue was
//      uploaded and forgotten.
//   B. TWO OPS ON ONE CELL IN ONE TICK HAVE A DEFINED WINNER. Overlapping
//      brush ops used to race inside sim_mutate's single dispatch, and
//      duplicate cell ops raced inside `cells`. The rule is now the one
//      sim_explode already used — lowest op index that would write the cell
//      owns it — enforced in the shader for brushes and at the CPU choke point
//      for cell ops.
//
// Claim B is checked by CONSTRUCTION, not by a statistic: the scene aims two
// overlapping same-tick brush ops of different materials at one place and asks
// what is there afterwards. Before the dedupe landed the answer was whatever
// the driver scheduled last, so this probe is capable of failing.
//
// SELF-CONTAINED (no deps): it runs its own worldgen twice, exactly as
// `determinism` does, so `--gate ops-replay` alone is a complete run.
namespace opsreplay {

constexpr int kTicks = 200;
constexpr uint32_t kProbeEvery = 15;

// The scripted scene. A PURE FUNCTION OF THE TICK, like SelftestOps, so the
// record and the replay are comparing the same world rather than two arbitrary
// ones — and so the L5 fixture below lands at a known place.
struct Scene {
  uint32_t seed;
  int ox, oz;  // window-relative origin in world cells
  // The L5 fixture: two mode-1 (overwrite) ops of DIFFERENT materials whose
  // spheres intersect. Op index 0 is stone, index 1 is wood, and the rule says
  // stone owns every cell they share.
  int ax, ay, az;  // op 0 centre

  std::vector<BrushOp> Ops(uint32_t t) const {
    std::vector<BrushOp> ops;
    // a sand column into air, and a water pour: the ordinary brush traffic
    if (t >= 5 && t < 120) {
      ops.push_back({ox + 100, FixtureY(ox + 100, oz + 100, seed, 110), oz + 100,
                     6, kMatSand, 0, 0, 0});
      ops.push_back({ox + 176, FixtureY(ox + 176, oz + 176, seed, 90), oz + 176,
                     5, kMatWater, 0, 0, 0});
    }
    // THE OVERLAP. Repeated over five ticks so a single scheduling accident
    // cannot decide the gate; idempotent, since op 0 rewrites its own stone.
    if (t >= 40 && t < 45) {
      ops.push_back({ax, ay, az, 5, kMatStone, 1u, 0, 0});
      ops.push_back({ax + 4, ay, az, 5, kMatWood, 1u, 0, 0});
    }
    // fire on the far side, for reaction coverage
    if (t >= 70 && t < 100) {
      ops.push_back({ox + 110, FixtureY(ox + 110, oz + 110, seed, 20), oz + 110,
                     3, kMatFire, 0, 0, 0});
    }
    return ops;
  }

  std::vector<ExplosionOp> Exps(uint32_t t) const {
    std::vector<ExplosionOp> exps;
    if (t == 60) {
      const int h = World::TerrainHeight(ox + 100, oz + 100, seed);
      // `author` is the renamed pad0: zero is the local player, which is what
      // every explosion in a single-player build is.
      exps.push_back({ox + 100, h, oz + 100, 14, 400, 0, 0, 0});
    }
    return exps;
  }

  // The exact-cell arm, standing in for a prefab stamp: an 8^3 box of stone
  // written through the `cells` entry, WITH A DELIBERATE DUPLICATE. The last
  // op repeats the box's first cell with water in it, so CanonicalizeCells has
  // something to drop and keep-first is observable at CellProbe() below.
  std::vector<CellOp> Cells(uint32_t t) const {
    std::vector<CellOp> cells;
    if (t != 20) return cells;
    const IVec3 b = BoxOrigin();
    for (int dz = 0; dz < 8; dz++)
      for (int dy = 0; dy < 8; dy++)
        for (int dx = 0; dx < 8; dx++)
          cells.push_back({World::SlotCellIndex({b.x + dx, b.y + dy, b.z + dz}),
                           PackVoxNew(kMatStone, 0)});
    cells.push_back({World::SlotCellIndex(b), PackVoxNew(kMatWater, 8)});
    return cells;
  }

  IVec3 BoxOrigin() const {
    return {ox + 60, FixtureY(ox + 60, oz + 60, seed, 40), oz + 60};
  }
  // The contested brush cell: two from op 0's centre, two from op 1's, so both
  // spheres cover it.
  IVec3 BrushProbe() const { return {ax + 2, ay, az}; }
  // A cell only op 0 covers, as the control: if this is not stone the fixture
  // never landed and the contested probe means nothing.
  IVec3 BrushControl() const { return {ax - 3, ay, az}; }
  IVec3 CellProbe() const { return BoxOrigin(); }
};

uint32_t MatAt(GpuContext& ctx, World& world, IVec3 cell) {
  std::vector<uint32_t> chunk(kChunkVol);
  const IVec3 wc{cell.x >> 4, cell.y >> 4, cell.z >> 4};
  ReadVoxelsSync(ctx, world, World::SlotChunkIndex(wc), 1, chunk.data(),
                 "opsReplayProbe");
  const uint32_t k = (uint32_t)((cell.z & 15) * 256 + (cell.y & 15) * 16 +
                                (cell.x & 15));
  return chunk[k] & 0xFFFu;
}

// Drive one pass of the scene, collecting a hash every kProbeEvery ticks.
void RunScene(Ctx& c, const Scene& sc, std::vector<uint32_t>& hashes) {
  SubmitWorldgen(c.ctx, c.world, c.sim, sc.seed);
  c.ctx.WaitIdle();
  hashes.clear();
  for (uint32_t t = 1; t <= (uint32_t)kTicks; t++) {
    SubmitTick(c.ctx, c.world, c.sim, t, sc.seed, sc.Ops(t), sc.Exps(t),
               sc.Cells(t), true, {8, 3, 8}, false, t >= 60);
    if (t % kProbeEvery == 0 || t == (uint32_t)kTicks)
      hashes.push_back(ReadHashSync(c.ctx, c.world));
  }
}

}  // namespace opsreplay

Status GateOpsReplay(Ctx& c, std::string& detail) {
  using namespace opsreplay;
  namespace ops = sandvox::opstream;

  const uint32_t seed = kDefaultSeed;
  const IVec3 wo = c.world.WindowOrigin();
  Scene sc;
  sc.seed = seed;
  sc.ox = wo.x * (int)kChunk;
  sc.oz = wo.z * (int)kChunk;
  sc.ax = sc.ox + 140;
  sc.az = sc.oz + 140;
  sc.ay = FixtureY(sc.ax, sc.az, seed, 40);

  const std::string path = "build/ops_replay.svops";
  if (ops::Recording())
    std::printf("ops-replay: taking over the recorder (a SANDVOX_RECORD_OPS "
                "recording ends here)\n");
  std::string err;
  if (!ops::StartRecording(path, seed, c.mats, err)) {
    detail = "cannot record: " + err;
    std::printf("ops-replay: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }

  // ---- pass A: play the scene with the recorder on ------------------------
  std::vector<uint32_t> hashA;
  RunScene(c, sc, hashA);
  const uint32_t matBrushA = MatAt(c.ctx, c.world, sc.BrushProbe());
  const uint32_t matCtrlA = MatAt(c.ctx, c.world, sc.BrushControl());
  const uint32_t matCellA = MatAt(c.ctx, c.world, sc.CellProbe());
  const uint64_t bytes = ops::RecordedBytes();
  const uint32_t frames = ops::RecordedFrames();
  ops::StopRecording();

  // ---- pass B: replay the file --------------------------------------------
  ops::Log log;
  if (!log.Load(path, c.mats, err)) {
    detail = "record refused on load: " + err;
    std::printf("ops-replay: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  // RAII, because every early return below has to disarm the replay or the
  // NEXT gate would have its gas spawns replaced out from under it.
  struct ReplayArm {
    explicit ReplayArm(const ops::Log* l) {
      ops::ResetReplayStats();
      ops::SetReplay(l);
    }
    ~ReplayArm() { ops::SetReplay(nullptr); }
  } arm(&log);

  SubmitWorldgen(c.ctx, c.world, c.sim, log.header.seed);
  c.ctx.WaitIdle();
  std::vector<uint32_t> hashB;
  for (const ops::Frame& f : log.frames) {
    // ---- M9.3-C: THE PHASE-B POSITION OF THIS DRIVE LOOP ----------------
    //
    // A chunk resync is a per-tick INPUT that is not an op (oprecord.h's
    // Frame::chunkReplaces), so a replay that only fed ops back would diverge
    // at the first sync. It goes here for the same reason session.cpp phase B
    // puts it before stream.Update: before the tick's submit, so the CA of
    // THIS tick runs on the corrected chunk. A no-op — one pointer test — for
    // every record this gate itself makes, since the scene never syncs.
    ops::ReplaceChunksIfReplaying(f.in.tick, c.stream);
    SubmitTick(c.ctx, c.world, c.sim, f.in.tick, f.in.seed, f.ops, f.exps,
               f.cells, f.in.hashEnable != 0,
               {f.in.playerChunk[0], f.in.playerChunk[1], f.in.playerChunk[2]},
               f.in.wantReadback != 0, f.in.particlesActive != 0, f.spawns,
               f.in.farCount, f.fluid, f.in.fluidLive,
               f.in.vizActive != 0);
    if (f.in.tick % kProbeEvery == 0 || f.in.tick == (uint32_t)kTicks)
      hashB.push_back(ReadHashSync(c.ctx, c.world));
  }
  const uint32_t matBrushB = MatAt(c.ctx, c.world, sc.BrushProbe());
  const uint32_t matCellB = MatAt(c.ctx, c.world, sc.CellProbe());
  const uint32_t paramMiss = ops::ReplayParamMismatches();

  // ---- the verdict --------------------------------------------------------
  std::string fails;
  auto fail = [&](const std::string& s) {
    if (!fails.empty()) fails += "; ";
    fails += s;
  };
  if ((uint32_t)log.frames.size() != (uint32_t)kTicks)
    fail(Format("record has %zu frames, scene ran %d ticks", log.frames.size(),
                kTicks));
  size_t firstDiff = hashA.size();
  for (size_t i = 0; i < hashA.size() && i < hashB.size(); i++)
    if (hashA[i] != hashB[i]) { firstDiff = i; break; }
  if (hashA.size() != hashB.size() || firstDiff < hashA.size())
    fail(Format("replay diverged at probe %zu (tick %zu): %08x vs %08x",
                firstDiff, (firstDiff + 1) * kProbeEvery,
                firstDiff < hashA.size() ? hashA[firstDiff] : 0u,
                firstDiff < hashB.size() ? hashB[firstDiff] : 0u));
  if (paramMiss != 0)
    fail(Format("%u TickParams words rebuilt differently (first at tick %u, "
                "word %u) - a per-tick knob is being read from outside the "
                "input stream",
                paramMiss, ops::ReplayFirstMismatchTick(),
                ops::ReplayFirstMismatchWord()));
  // L5: the contested brush cell belongs to op index 0 (stone), not op 1
  // (wood), and it says so on BOTH runs.
  if (matCtrlA != kMatStone)
    fail(Format("fixture never landed: op-0-only cell is mat %u, not stone",
                matCtrlA));
  if (matBrushA != kMatStone)
    fail(Format("contested brush cell is mat %u, not stone (op 0 must win)",
                matBrushA));
  if (matBrushA != matBrushB)
    fail(Format("contested brush cell differs between record and replay: %u "
                "vs %u",
                matBrushA, matBrushB));
  // The cell-op arm: keep-FIRST, so the duplicate's water never lands.
  if (matCellA != kMatStone)
    fail(Format("duplicated cell op is mat %u, not stone (keep-first)",
                matCellA));
  if (matCellA != matCellB)
    fail(Format("duplicated cell op differs on replay: %u vs %u", matCellA,
                matCellB));

  // Informational pins. Byte size is not a correctness property — it moves
  // whenever TickParams grows — so it REPORTS rather than fails, the way an
  // absent baseline key is treated everywhere else in this harness.
  RecordObserved("opsReplayBytes", (double)bytes);
  RecordObserved("opsReplayFrames", (double)frames);
  const double pinnedBytes = BaselineNumber("opsReplayBytes", 0.0);
  char note[128] = "";
  if (pinnedBytes > 0.0 && (double)bytes != pinnedBytes)
    std::snprintf(note, sizeof(note), ", record %llu B (pin says %.0f)",
                  (unsigned long long)bytes, pinnedBytes);
  else
    std::snprintf(note, sizeof(note), ", record %llu B", (unsigned long long)bytes);

  const ops::StreamCounts& sccount = ops::Counts();
  char buf[640];
  std::snprintf(buf, sizeof(buf),
                "%s (%u frames, %zu hash probes reproduced%s | contested brush "
                "cell -> mat %u on both runs | cell dupes dropped %u over %u "
                "ticks | clamps b%u e%u c%u s%u f%u%s%s)",
                fails.empty() ? "PASS" : "FAIL", frames, hashA.size(), note,
                matBrushA, sccount.cellDupes, sccount.ticksWithDupes,
                sccount.brushTrunc, sccount.expTrunc, sccount.cellTrunc,
                sccount.spawnTrunc, sccount.fluidTrunc,
                fails.empty() ? "" : " | ", fails.c_str());
  detail = buf;
  std::printf("ops-replay: %s\n", buf);
  return fails.empty() ? Status::Pass : Status::Fail;
}

// ---- chunk-hash: the per-chunk digest and the quiet streak (M9.3-A) -----
//
// docs/PLAN_multiplayer_m9.md M9.3-A. Two machines cannot compare 16 KiB per
// chunk to find out whose world drifted, so every chunk carries a DIGEST of
// its 4,096 words, written by sim_occupancy.wgsl's full entry point beside the
// world hash. The world hash keys each cell on its SLOT-GLOBAL index and is
// therefore a fold of "this window"; the digest keys on the CHUNK-LOCAL index,
// so it is a pure function of the chunk's contents and of nothing else. Four
// claims, and each is a way the digest could be useless while looking fine:
//
//   A  TWICE-RUN. Two identical worlds produce identical digest tables. If
//      this fails the fold itself is non-deterministic and nothing below
//      means anything.
//   B  ONE VOXEL, ONE SLOT. Flipping one cell's palette nibble moves exactly
//      ONE slot's digest, and it is that cell's. A digest that moved for
//      other chunks would make a resync ship the whole world; one that did
//      not move for this chunk would make it ship nothing.
//   C  CONTENT-KEYED, AND SENTINEL == MATERIALIZED. Un-painting the cell
//      returns the digest to the value it had while the chunk was still a
//      JITTER sentinel with no page at all — the page table is derived data
//      (PLAN_page_table.md), so a digest that could tell a sentinel from its
//      materialized twin would report a difference every time a peer happened
//      to allocate a page. And the digest the GPU produced equals one this
//      gate folds on the CPU from the chunk's words with the chunk-local key,
//      which is what "content-keyed" means as a measurement rather than as a
//      claim: a slot-keyed fold would disagree with it for every slot but 0.
//   D  THE QUIET STREAK. World::QuietTicks grows by one per published tick on
//      an untouched chunk, resets to 0 when Stream::MarkModifiedBox names it,
//      and resets when the chunk is edited through the op stream.
//
// The whole gate runs with hashEnable on every tick: the full occupancy pass
// is what writes the table, and a gate that had to guess which ticks ran it
// would be measuring the tick schedule instead of the digest.
Status GateChunkHash(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  Stream& stream = c.stream;

  // Tunable without a rebuild (CLAUDE.md "put thresholds in baseline.json").
  const uint32_t kSettle = (uint32_t)BaselineNumber("chunkHashSettleTicks", 30);
  const uint32_t kQuietCap = (uint32_t)BaselineNumber("chunkHashQuietCap", 60);
  const IVec3 pc{8, 3, 8};

  std::string fails;
  auto fail = [&](const std::string& m) {
    if (!fails.empty()) fails += "; ";
    fails += m;
  };

  uint32_t t = 0;
  auto tick = [&](const std::vector<CellOp>& cells) {
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, cells,
               /*hashEnable=*/true, pc, /*wantReadback=*/false,
               /*particlesActive=*/false);
  };
  auto readTable = [&](std::vector<uint32_t>& out) {
    out.assign(kChunkHashWords, 0u);
    ctx.WaitIdle();
    rhi::ReadbackBlocking(ctx.device, ctx.queue, world.chunkHash, 0, out.data(),
                          kChunkHashBytes, "chunkhash");
  };
  auto diffSlots = [](const std::vector<uint32_t>& a,
                      const std::vector<uint32_t>& b,
                      std::vector<uint32_t>& out) {
    out.clear();
    for (uint32_t i = 0; i < kNumSlots; i++)
      if (a[i] != b[i]) out.push_back(i);
  };

  // ---- A: twice-run ------------------------------------------------------
  std::vector<uint32_t> run[2];
  for (int r = 0; r < 2; r++) {
    stream.OnRegen();
    world.SetWindowOrigin({0, 0, 0});
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    t = 0;
    for (uint32_t i = 0; i < kSettle; i++) tick({});
    readTable(run[r]);
  }
  std::vector<uint32_t> twiceDiff;
  diffSlots(run[0], run[1], twiceDiff);
  if (!twiceDiff.empty())
    fail(Format("twice-run: %zu of %u slots disagree (first slot %u: %08x vs "
                "%08x)",
                twiceDiff.size(), kNumSlots, twiceDiff[0],
                run[0][twiceDiff[0]], run[1][twiceDiff[0]]));
  // The freshness stamp is the table's own answer to "which pass wrote this",
  // and a consumer that trusted a stale table would compare a peer's tick-30
  // world against its own tick-90 one and call the difference a desync.
  if (run[1][kChunkHashTickWord] != t)
    fail(Format("table stamp is tick %u, last full pass was tick %u",
                run[1][kChunkHashTickWord], t));

  // ---- quiesce, so B's "exactly one slot" is a claim about the paint ------
  // A world still settling moves digests on its own, and "exactly 1 changed"
  // measured against a moving background is not a measurement. Tick until two
  // consecutive full passes agree everywhere, and REPORT how long that took
  // rather than asserting a settle time this gate does not own.
  std::vector<uint32_t> prev = run[1], cur;
  uint32_t quiesceTicks = 0;
  std::vector<uint32_t> moved;
  for (; quiesceTicks < kQuietCap; quiesceTicks++) {
    tick({});
    readTable(cur);
    diffSlots(prev, cur, moved);
    prev.swap(cur);
    if (moved.empty()) break;
  }
  if (!moved.empty())
    fail(Format("world never quiesced: %zu slots still moving after %u ticks",
                moved.size(), kQuietCap));

  // ---- the subject chunk: a JITTER SENTINEL, FOUND not assumed ------------
  // Claim C needs a chunk that has no page at all, and the gate must not
  // assume where one is: the first cut picked "deep stone under the harness
  // player chunk" by TerrainHeight and drew slot 8520, which the mirror's
  // materialization ring had already made a real page — so the round trip
  // measured nothing and said so. Ask the page table instead, and take the
  // first slot (ascending, so the choice is deterministic) that is a JITTER
  // sentinel of non-air matter: that is exactly the buried-bulk case §9 of
  // PLAN_page_table.md compresses, and the one a resync will meet most.
  IVec3 cell{0, 0, 0};
  uint32_t slot = 0;
  bool foundSentinel = false;
  // SLOT 0 IS EXCLUDED, and that is the difference between a measurement and a
  // fixture that cannot fail. The world hash keys on `wg.x * CHUNK_VOL + i`;
  // at slot 0 that IS `i`, so C2's CPU fold would agree with a slot-keyed
  // digest too and the one claim the whole package rests on would be
  // untestable. The first run picked slot 0 and passed for that reason.
  for (uint32_t s = 1; s < kNumSlots && !foundSentinel; s++) {
    const uint32_t e = world.PageEntryOfSlot(s);
    if ((e & kPtSentinelBit) == 0u) continue;
    if ((e & kPtJitterBit) == 0u) continue;       // uniform: no palette to flip
    if ((e & kPtMatMask) == kMatAir) continue;    // air hashes to nothing
    const IVec3 wc = world.SlotToWorldChunk(s);
    if (!world.ChunkInWindow(wc)) continue;
    slot = s;
    cell = {wc.x * (int)kChunk + 8, wc.y * (int)kChunk + 8,
            wc.z * (int)kChunk + 8};
    foundSentinel = true;
  }
  if (!foundSentinel)
    fail("no JITTER sentinel chunk in the window to round-trip");
  const uint32_t localIdx = World::SlotCellIndex(cell) % kChunkVol;
  const uint32_t entryBefore = world.PageEntryOfSlot(slot);
  const bool sentinelBefore = (entryBefore & kPtSentinelBit) != 0u;
  const uint32_t digBefore = prev[slot];
  if (!world.CellInWindow(cell))
    fail(Format("subject cell (%d,%d,%d) is outside the window", cell.x, cell.y,
                cell.z));
  // The word that is there NOW, whichever form the chunk is in. A sentinel has
  // no page to read, so its word comes from the same synthesis the CPU mirror
  // and the eviction encoder use; a real page is read back verbatim.
  uint32_t origWord = 0;
  if (sentinelBefore) {
    origWord = SynthWordAt(entryBefore, cell.x, cell.y, cell.z,
                           world.pages->WorldSeed());
  } else {
    const uint64_t off = world.PageOffsetOfSlot(slot);
    rhi::ReadbackBlocking(ctx.device, ctx.queue, world.voxels,
                          off + (uint64_t)localIdx * 4, &origWord, 4, "chword");
  }
  // Flip ONE bit of the palette-jitter nibble. It changes the hashed value
  // (bits 0..15 are in `v`) and changes nothing else: same material, same
  // stain, so no reaction can fire and no neighbour can be woken into moving.
  // Painting air here would also work and would additionally move occupancy,
  // which is not this gate's subject and would add a second cause for a digest
  // to move.
  const uint32_t paintWord = origWord ^ (1u << 12);

  // ---- B: one voxel moves exactly one slot's digest -----------------------
  tick({CellOp{World::SlotCellIndex(cell), paintWord}});
  readTable(cur);
  std::vector<uint32_t> painted;
  diffSlots(prev, cur, painted);
  const size_t changedSlots = painted.size();
  if (changedSlots != 1 || painted[0] != slot) {
    std::string got;
    for (size_t i = 0; i < painted.size() && i < 6; i++)
      got += Format("%s%u", i ? "," : "", painted[i]);
    fail(Format("paint moved %zu slot(s) [%s], wanted exactly slot %u",
                changedSlots, got.c_str(), slot));
  }
  const uint32_t digPainted = cur[slot];
  const bool materialized =
      (world.PageEntryOfSlot(slot) & kPtSentinelBit) == 0u;

  // ---- C: un-paint returns the SENTINEL-ERA digest ------------------------
  prev.swap(cur);
  tick({CellOp{World::SlotCellIndex(cell), origWord}});
  readTable(cur);
  if (cur[slot] != digBefore)
    fail(Format("un-paint left digest %08x, sentinel-era value was %08x "
                "(sentinel before: %s, materialized after: %s)",
                cur[slot], digBefore, sentinelBefore ? "yes" : "no",
                materialized ? "yes" : "no"));
  // The gate is only worth its runtime if the round trip actually crossed the
  // sentinel boundary. Say so rather than passing quietly on a chunk that was
  // a real page the whole time.
  if (!sentinelBefore)
    fail(Format("slot %u was already a real page (entry %08x); the "
                "sentinel-vs-materialized round trip was not exercised",
                slot, entryBefore));
  if (!materialized)
    fail(Format("slot %u never materialized under the paint (entry %08x)",
                slot, world.PageEntryOfSlot(slot)));

  // ---- C2: the digest is the CPU's content fold, not a slot fold ----------
  // The discriminating measurement for "chunk-local key". A slot-keyed fold
  // (what the world hash does) would disagree with this for every slot but 0.
  uint32_t cpuDigest = 0;
  bool readWords = false;
  {
    std::vector<uint32_t> words(kChunkVol, 0u);
    const uint64_t off = world.PageOffsetOfSlot(slot);
    if (off != World::kNoPage)
      readWords =
          rhi::ReadbackBlocking(ctx.device, ctx.queue, world.voxels, off,
                                words.data(), (size_t)kChunkVol * 4, "chwords");
    if (readWords) {
      for (uint32_t i = 0; i < kChunkVol; i++) {
        const uint32_t w = words[i];
        if ((w & 0xFFFu) == 0u) continue;  // air contributes nothing, as in WGSL
        const uint32_t v = (w & 0xFFFFu) | ((w & kStainBits) >> 8u);
        cpuDigest += rng::Pcg(i ^ (v * 0x9E3779B9u));
      }
      if (cpuDigest != cur[slot])
        fail(Format("CPU content fold %08x != GPU digest %08x for slot %u "
                    "(the key is not chunk-local)",
                    cpuDigest, cur[slot], slot));
    } else {
      fail("could not read the slot's words back for the CPU content fold");
    }
  }

  // ---- D: the quiet streak -----------------------------------------------
  // The paint above was two ticks ago and the snapshot is K ticks latent, so
  // walk the streak forward from wherever it is now and assert the SHAPE: one
  // per published tick, zero on an explicit touch, zero after an edit.
  const uint32_t q0 = world.QuietTicks(slot);
  tick({});
  const uint32_t q1 = world.QuietTicks(slot);
  tick({});
  const uint32_t q2 = world.QuietTicks(slot);
  if (!(q1 == q0 + 1 && q2 == q1 + 1))
    fail(Format("quiet streak on an untouched chunk went %u -> %u -> %u, "
                "wanted +1 per tick",
                q0, q1, q2));
  // Stream::MarkModifiedBox is the CPU-edit path: it must break the streak
  // IMMEDIATELY, not kSnapshotLatency ticks later, because a CPU edit is
  // activity the dirty flags have not reported yet.
  const IVec3 lo{cell.x, cell.y, cell.z};
  stream.MarkModifiedBox(lo, lo);
  const uint32_t qTouched = world.QuietTicks(slot);
  if (qTouched != 0)
    fail(Format("MarkModifiedBox left the quiet streak at %u, wanted 0",
                qTouched));
  // ...and an edit through the op stream breaks it too, by way of the dirty
  // flags, which is the path every GPU-side change takes. Give it the publish
  // latency plus one and require that it was reset somewhere in there.
  for (uint32_t i = 0; i < World::kSnapshotLatency + 2; i++) tick({});
  const uint32_t qGrown = world.QuietTicks(slot);
  tick({CellOp{World::SlotCellIndex(cell), paintWord}});
  uint32_t qMin = 0xFFFFFFFFu;
  for (uint32_t i = 0; i <= World::kSnapshotLatency + 1; i++) {
    tick({});
    qMin = std::min(qMin, world.QuietTicks(slot));
  }
  if (qMin != 0)
    fail(Format("an op-stream edit never reset the quiet streak (min %u over "
                "K+2 ticks; it had grown to %u)",
                qMin, qGrown));

  // ---- the published table matches the GPU's, through the readback ring ---
  // The accessors are the product, not the buffer: M9.3-C will read
  // World::ChunkHashOfSlot, not world.chunkHash. Settle first, so the ring's
  // latency cannot be mistaken for a parse bug.
  tick({CellOp{World::SlotCellIndex(cell), origWord}});
  for (uint32_t i = 0; i < World::kSnapshotLatency + 3; i++) tick({});
  readTable(cur);
  uint32_t ringDiff = 0, firstRingSlot = 0;
  for (uint32_t i = 0; i < kNumSlots; i++)
    if (world.ChunkHashOfSlot(i) != cur[i]) {
      if (ringDiff == 0) firstRingSlot = i;
      ringDiff++;
    }
  if (ringDiff)
    fail(Format("snapshot digest table differs from the GPU's in %u slots "
                "(first %u: %08x vs %08x, snapshot tick %u / table tick %u)",
                ringDiff, firstRingSlot, world.ChunkHashOfSlot(firstRingSlot),
                cur[firstRingSlot], world.ChunkHashTick(),
                cur[kChunkHashTickWord]));
  if (world.ChunkHashTick() == 0)
    fail("the snapshot never carried a digest-table tick");
  // A page fault is a DROPPED STORE, which would silently make every digest
  // above a description of the wrong world.
  if (world.Snap().valid && world.Snap().pageFaults != 0)
    fail(Format("%u page faults", world.Snap().pageFaults));

  const uint32_t pinnedChanged =
      (uint32_t)BaselineNumber("chunkHashChangedSlots", 1);
  if (changedSlots != pinnedChanged)
    fail(Format("changedSlots %zu, baseline pins %u", changedSlots,
                pinnedChanged));

  // Leave the world regenerated: this gate painted, un-painted and ran ~110
  // ticks, and the gates after it inherit whatever it leaves (selftest.h's
  // ordering note).
  stream.OnRegen();
  world.SetWindowOrigin({0, 0, 0});
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  char buf[768];
  std::snprintf(buf, sizeof(buf),
                "%s (%u slots hashed, twice-run identical | quiesced in %u "
                "ticks | paint moved %zu slot (%u) %08x -> %08x -> %08x | "
                "sentinel round trip %s | CPU fold %08x | quiet %u -> %u%s%s)",
                fails.empty() ? "PASS" : "FAIL", kNumSlots, quiesceTicks,
                changedSlots, slot, digBefore, digPainted, cur[slot],
                (sentinelBefore && materialized) ? "yes" : "NO", cpuDigest, q0,
                qGrown, fails.empty() ? "" : " | ", fails.c_str());
  detail = buf;
  std::printf("chunk-hash: %s\n", buf);
  return fails.empty() ? Status::Pass : Status::Fail;
}

// ---- chunk-resync: the hash tree, the authority rule, and the repair -----
//
// docs/PLAN_multiplayer_m9.md M9.3-C. Package A gave every chunk a digest;
// this gate is about what two machines DO with the digests when they differ.
// Three arms, and each one tests a claim the others cannot:
//
//   A  PURE. `net::Comparable`, `net::ChunkAuthority` and `net::HashTree`'s
//      block arithmetic are functions of two window origins and two player
//      chunks and nothing else — which is the whole reason ownership can be a
//      DERIVED fact rather than a negotiated one (net/authority.h). They are
//      testable with no World at all, so they are tested with no World at all:
//      a truth table, including the two cases that cost a review finding (a
//      nearest machine that does not HOLD the chunk must lose, and a chunk on
//      the window's outermost plane is not comparable at any distance).
//   B  THE REPAIR. Two arms of the SAME 80-tick scene. The control never
//      drifts. The subject paints five cells of a buried chunk (a divergence
//      manufactured exactly the way a dropped op would leave one), watches the
//      digest move, then installs the control's copy of that chunk through
//      `Stream::ReplaceChunk` at the phase-B position — and both arms must end
//      on the SAME WORLD HASH and the same digest.
//
//      THE WORLD HASH IS THE CLAIM, not the digest. A digest that came back
//      would only prove the 4,096 words were restored; the world hash proves
//      that every side effect a refill has — occupancy, sub-occupancy, the far
//      indices, the dirty wake, the settled-skip latch — was reproduced too,
//      which is the plan's kill criterion in one number.
//   C  THE RECORD. The drift-and-repair run is recorded and replayed. A chunk
//      replace is a per-tick input that is NOT an op (oprecord.h), so a replay
//      that did not carry it would diverge at the replace; that this one does
//      not is what makes a networked session's record worth keeping.
//
// The subject chunk is FOUND, not assumed: the gate scans for the deepest
// buried column at the harness site and reports the chunk's air fraction, so a
// fixture that stopped being buried says so instead of passing quietly.
namespace resync {

constexpr uint32_t kSettle = 40;    // ticks before anything happens
constexpr uint32_t kDriftAt = 41;   // the five drifting cells land here
constexpr uint32_t kReplaceAt = 56; // ...and the repair here
constexpr uint32_t kTotal = 80;     // both arms run exactly this many ticks
constexpr uint32_t kDriftCells = 5;

struct ArmResult {
  uint32_t finalHash = 0;
  std::vector<uint32_t> tableAfterSettle;  // digest table at kSettle
  std::vector<uint32_t> tableBeforeFix;    // ...just before kReplaceAt
  std::vector<uint32_t> tableFinal;        // ...at kTotal
  std::vector<uint32_t> capturedWords;     // the chunk's words at kSettle
};

// The five cells the drift writes, and the palette variant each gets. A
// PALETTE VARIANT and not a new material: the state nibble is hashed (world.h
// word layout), so the digest moves, while the material is unchanged so no
// reaction can fire and no neighbour can be woken into moving. The divergence
// stays the five voxels it was asked to be, which is what lets arm B's world
// hash be an equality rather than a tolerance.
std::vector<CellOp> DriftCells(IVec3 wc, const std::vector<uint32_t>& base) {
  std::vector<CellOp> out;
  for (uint32_t i = 0; i < kDriftCells; i++) {
    const uint32_t li = 1000u + i * 137u;  // scattered, deterministic
    const IVec3 cell{wc.x * (int)kChunk + (int)(li % kChunk),
                     wc.y * (int)kChunk + (int)((li / kChunk) % kChunk),
                     wc.z * (int)kChunk + (int)(li / (kChunk * kChunk))};
    const uint32_t w = base[li];
    // XOR one bit of the state nibble: guaranteed to differ from whatever is
    // there, whichever palette variant worldgen chose, so the fixture cannot
    // accidentally write the word that was already present.
    out.push_back({World::SlotCellIndex(cell), w ^ (1u << 13)});
  }
  return out;
}

}  // namespace resync

Status GateChunkResync(Ctx& c, std::string& detail) {
  using namespace resync;
  namespace ops = sandvox::opstream;
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  Stream& stream = c.stream;
  const IVec3 pc{8, 3, 8};

  std::string fails;
  auto fail = [&](const std::string& m) {
    if (!fails.empty()) fails += "; ";
    fails += m;
  };

  // ======== ARM A: the pure rules ========================================
  //
  // No World, no GPU, no fixture. Every one of these is a claim about the
  // arithmetic two machines must agree on without talking.
  {
    const int n = (int)kNChunk;
    const IVec3 o0{0, 0, 0};
    const IVec3 o1{1, 0, 0};  // a peer one chunk to the +x
    // Comparable: kComparableMargin (2) inside BOTH windows.
    if (!net::Comparable({10, 10, 10}, o0, o1))
      fail("Comparable said no for a chunk deep inside both windows");
    // The outermost plane of MY window. Not comparable at any distance —
    // §4 finding 1: its CA sees SOLID on one side and real neighbours on the
    // other, so the two machines are not even SUPPOSED to agree there.
    if (net::Comparable({0, 10, 10}, o0, o1))
      fail("Comparable said yes on my window's outermost plane");
    if (net::Comparable({1, 10, 10}, o0, o1))
      fail("Comparable said yes one chunk inside the plane (margin is 2)");
    // Inside mine with margin, but on the PEER's edge.
    if (net::Comparable({2, 10, 10}, o0, o1))
      fail("Comparable ignored the peer's margin");
    if (!net::Comparable({3, 10, 10}, o0, o1))
      fail("Comparable said no two chunks inside both windows");
    // ...and past the far face of the peer's window.
    if (net::Comparable({n - 1, 10, 10}, o0, o1))
      fail("Comparable said yes past the peer's far face");

    // ChunkAuthority: nearest RESIDENT wins. Peer 1 is nearer in chunks but
    // does NOT hold the chunk (its window is 40 chunks away), so peer 0 —
    // farther, but resident — must own it. This is §4 finding 6 and it is the
    // case plain nearest-player gets wrong.
    net::AuthorityMemory mem;
    std::vector<net::PeerView> peers = {
        {0, {16, 16, 16}, {0, 0, 0}, true},
        {1, {60, 16, 16}, {40, 0, 0}, true},
    };
    if (net::ChunkAuthority({26, 16, 16}, peers, mem) != 0)
      fail("ChunkAuthority gave a chunk to a peer that does not hold it");
    // Both resident: nearest wins.
    peers[1].windowOrigin = {0, 0, 0};
    peers[1].chunk = {20, 16, 16};
    if (net::ChunkAuthority({21, 16, 16}, peers, mem) != 1)
      fail("ChunkAuthority did not pick the nearer resident peer");
    // A tie goes to the lower id, on BOTH machines, which is what makes the
    // answer symmetric without a message.
    peers[1].chunk = {16, 16, 16};
    if (net::ChunkAuthority({18, 16, 16}, peers, mem) != 0)
      fail("ChunkAuthority broke a tie against the lower id");
    // Nobody can reach it -> kNoAuthority, and that is a real answer: a chunk
    // between two distant players is nobody's and nothing steps it.
    peers[0].windowOrigin = {0, 0, 0};
    peers[1].windowOrigin = {0, 0, 0};
    if (net::ChunkAuthority({500, 16, 16}, peers, mem) != net::kNoAuthority)
      fail("ChunkAuthority named an owner for a chunk nobody holds");

    // HashTree block arithmetic. The negative case is the whole reason it is a
    // SHIFT: -1 / 4 is 0 in C++ and would put chunk -1 in block 0, beside
    // chunk 0, so two blocks would overlap and their sums would be meaningless.
    if (net::HashTree::BlockOfChunk({-1, -4, 7}).x != -1 ||
        net::HashTree::BlockOfChunk({-1, -4, 7}).y != -1 ||
        net::HashTree::BlockOfChunk({-1, -4, 7}).z != 1)
      fail("HashTree::BlockOfChunk is not an arithmetic shift");
    // ...and BlockChunk is its inverse over the 64, in the canonical order
    // (x fastest) both ends index HashChunksMsg::digest with.
    uint32_t seen = 0;
    bool roundTrip = true;
    for (uint32_t i = 0; i < net::kChunksPerBlock; i++) {
      const IVec3 wc = net::HashTree::BlockChunk({-1, 2, 0}, i);
      const IVec3 b = net::HashTree::BlockOfChunk(wc);
      if (b.x != -1 || b.y != 2 || b.z != 0) roundTrip = false;
      seen++;
    }
    if (!roundTrip || seen != net::kChunksPerBlock)
      fail("HashTree::BlockChunk left its own block");
    if (net::HashTree::BlockChunk({0, 0, 0}, 1).x != 1 ||
        net::HashTree::BlockChunk({0, 0, 0}, 4).y != 1 ||
        net::HashTree::BlockChunk({0, 0, 0}, 16).z != 1)
      fail("HashTree::BlockChunk is not x-fastest");
  }

  // ======== find the subject chunk (measured, not assumed) ===============
  //
  // The deepest column at the harness site, two chunks under its surface. Two
  // chunks and not one: the chunk the surface is IN is where the CA still has
  // work to do, and the resync's whole premise is quiet matter.
  int bestH = -1, bestX = 0, bestZ = 0;
  for (int cz = 6; cz <= 12; cz++)
    for (int cx = 6; cx <= 12; cx++) {
      const int wx = cx * (int)kChunk + 8, wz = cz * (int)kChunk + 8;
      const int h = World::TerrainHeight(wx, wz, kDefaultSeed);
      if (h > bestH) { bestH = h; bestX = wx; bestZ = wz; }
    }
  const IVec3 subject{bestX >> 4, (bestH >> 4) - 2, bestZ >> 4};
  if (subject.y < 2) {
    detail = Format("no buried chunk at the harness site (best terrain height "
                    "%d -> chunk y %d)", bestH, subject.y);
    std::printf("chunk-resync: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  const uint32_t slot = World::SlotChunkIndex(subject);

  // ======== ARM B: the repair ============================================

  auto readTable = [&](std::vector<uint32_t>& out) {
    out.assign(kChunkHashWords, 0u);
    ctx.WaitIdle();
    rhi::ReadbackBlocking(ctx.device, ctx.queue, world.chunkHash, 0, out.data(),
                          kChunkHashBytes, "resyncHash");
  };

  // ONE SCENE, TWO ARMS. `replaceWith` null = the control (no drift, no
  // repair); non-null = the subject, which drifts at kDriftAt and installs
  // those words at kReplaceAt. Identical tick counts, identical hash schedule,
  // identical everything else — which is what makes the final hashes
  // comparable at all (CLAUDE.md rule 7's "run both arms at the same scope").
  auto runArm = [&](const std::vector<uint32_t>* replaceWith,
                    ArmResult& r) {
    stream.OnRegen();
    world.SetWindowOrigin({0, 0, 0});
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    std::vector<CellOp> drift;
    for (uint32_t t = 1; t <= kTotal; t++) {
      // THE PHASE-B POSITION: before this tick's submit, exactly where
      // session.cpp installs a live sync, so the CA of tick kReplaceAt runs on
      // the repaired chunk. Recorded the way phase B records it, which is what
      // arm C replays.
      if (replaceWith && t == kReplaceAt) {
        if (!stream.ReplaceChunk(subject, replaceWith->data())) {
          fail("ReplaceChunk refused a resident chunk");
        } else {
          std::vector<uint32_t> rle;
          RleEncodeChunk(replaceWith->data(), rle);
          ops::NoteChunkReplace(t, subject, rle);
        }
      }
      std::vector<CellOp> cells;
      if (replaceWith && t == kDriftAt) cells = drift;
      SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, cells,
                 /*hashEnable=*/true, pc, /*wantReadback=*/false,
                 /*particlesActive=*/false);
      if (t == kSettle) {
        readTable(r.tableAfterSettle);
        // The authority's copy: the chunk as the control has it, read through
        // the same synthesis path a sentinel chunk needs (support.h), so a
        // chunk with no page at all is captured correctly.
        r.capturedWords.assign(kChunkVol, 0u);
        ReadVoxelsSync(ctx, world, slot, 1, r.capturedWords.data(), "resyncCap");
        if (replaceWith == nullptr) drift.clear();
      }
      if (t == kDriftAt - 1 && replaceWith != nullptr)
        drift = DriftCells(subject, r.capturedWords);
      if (t == kReplaceAt - 1) readTable(r.tableBeforeFix);
    }
    readTable(r.tableFinal);
    r.finalHash = ReadHashSync(ctx, world);
  };

  // Control first: it is what supplies the "authority copy" the subject arm
  // installs, and running it first means the subject arm cannot have tainted
  // the words it is repaired with.
  ArmResult ctrl;
  runArm(nullptr, ctrl);

  // How buried is the subject, really? A number, not an assumption — a
  // fixture that stopped being deep matter would still pass arm B and would
  // be testing a quiet AIR chunk, which is not the case the resync is for.
  uint32_t solid = 0;
  for (uint32_t i = 0; i < kChunkVol; i++)
    if ((ctrl.capturedWords[i] & 0xFFFu) != 0u) solid++;
  if (solid * 100u < kChunkVol * 90u)
    fail(Format("subject chunk (%d,%d,%d) is only %.0f%% solid; the fixture is "
                "not buried matter",
                subject.x, subject.y, subject.z,
                100.0 * solid / (double)kChunkVol));

  // ...and the subject arm, recorded, so arm C has a file.
  const std::string path = "build/chunk_resync.svops";
  std::string err;
  const bool recording = ops::StartRecording(path, kDefaultSeed, c.mats, err);
  if (!recording)
    fail("cannot record the repair run: " + err);
  ArmResult subj;
  subj.capturedWords = ctrl.capturedWords;  // the authority's copy
  runArm(&ctrl.capturedWords, subj);
  const uint32_t recFrames = ops::RecordedFrames();
  ops::StopRecording();

  // B1: the drift MOVED the digest, and moved exactly the subject's slot.
  // Without this the repair below could be repairing nothing.
  std::vector<uint32_t> moved;
  for (uint32_t i = 0; i < kNumSlots; i++)
    if (subj.tableBeforeFix[i] != ctrl.tableBeforeFix[i]) moved.push_back(i);
  if (moved.size() != 1 || moved[0] != slot) {
    std::string got;
    for (size_t i = 0; i < moved.size() && i < 6; i++)
      got += Format("%s%u", i ? "," : "", moved[i]);
    fail(Format("before the repair the two arms differed in %zu slot(s) [%s]; "
                "wanted exactly the subject's slot %u",
                moved.size(), got.c_str(), slot));
  }
  // B2: THE REPAIR. The digest is back...
  if (subj.tableFinal[slot] != ctrl.tableFinal[slot])
    fail(Format("after the repair the subject's digest is %08x, the control's "
                "is %08x",
                subj.tableFinal[slot], ctrl.tableFinal[slot]));
  // ...and so is every other slot, which is the claim that ReplaceChunk did
  // not wake something into moving that the control never moved.
  uint32_t stillDiff = 0, firstDiff = 0;
  for (uint32_t i = 0; i < kNumSlots; i++)
    if (subj.tableFinal[i] != ctrl.tableFinal[i]) {
      if (stillDiff == 0) firstDiff = i;
      stillDiff++;
    }
  if (stillDiff)
    fail(Format("%u slots still differ after the repair (first %u: %08x vs "
                "%08x)",
                stillDiff, firstDiff, subj.tableFinal[firstDiff],
                ctrl.tableFinal[firstDiff]));
  // B3: THE KILL CRITERION. The whole world hash, not just the chunk: if this
  // fails while B2 passes, the replace path is missing one of FillSlots's side
  // effects (occupancy, sub-occupancy, the wake, the far indices) and the
  // plan says find which, not widen the test.
  if (subj.finalHash != ctrl.finalHash)
    fail(Format("world hash after the repair %08x != control %08x — a "
                "FillSlots side effect is missing from ReplaceChunk",
                subj.finalHash, ctrl.finalHash));
  if (stream.ChunkReplacesApplied() == 0)
    fail("Stream reported no chunk replaces at all");

  // ======== ARM C: the record replays ====================================
  //
  // A chunk replace is a per-tick input that is not an op. Feed the record
  // back and require the same final hash — which can only happen if
  // ReplaceChunksIfReplaying re-applied the replace at the same point, since
  // without it the replay runs the drift and never repairs it.
  uint32_t replayHash = 0, replayReplaces = 0;
  uint32_t replayFrames = 0;
  if (recording) {
    ops::Log log;
    if (!log.Load(path, c.mats, err)) {
      fail("the repair record refused on load: " + err);
    } else {
      struct ReplayArm {
        explicit ReplayArm(const ops::Log* l) {
          ops::ResetReplayStats();
          ops::SetReplay(l);
        }
        ~ReplayArm() { ops::SetReplay(nullptr); }
      } arm(&log);
      stream.OnRegen();
      world.SetWindowOrigin({0, 0, 0});
      SubmitWorldgen(ctx, world, sim, log.header.seed);
      ctx.WaitIdle();
      for (const ops::Frame& f : log.frames) {
        replayReplaces += ops::ReplaceChunksIfReplaying(f.in.tick, stream);
        SubmitTick(ctx, world, sim, f.in.tick, f.in.seed, f.ops, f.exps,
                   f.cells, f.in.hashEnable != 0,
                   {f.in.playerChunk[0], f.in.playerChunk[1],
                    f.in.playerChunk[2]},
                   f.in.wantReadback != 0, f.in.particlesActive != 0, f.spawns,
                   f.in.farCount, f.fluid, f.in.fluidLive,
                   f.in.vizActive != 0);
      }
      replayFrames = (uint32_t)log.frames.size();
      replayHash = ReadHashSync(ctx, world);
      if (replayReplaces != 1)
        fail(Format("the replay applied %u chunk replaces, the record has 1",
                    replayReplaces));
      if (ops::ReplayChunkReplaceRefusals() != 0)
        fail(Format("the replay refused %u chunk replaces as non-resident",
                    ops::ReplayChunkReplaceRefusals()));
      if (replayHash != subj.finalHash)
        fail(Format("replay hash %08x != the run it recorded %08x", replayHash,
                    subj.finalHash));
    }
  }

  // Informational pins, the way ops-replay treats its byte size: they REPORT
  // rather than fail, because a block count moves with the window's contents.
  RecordObserved("chunkResyncSolidPct",
                 100.0 * solid / (double)kChunkVol);
  RecordObserved("chunkResyncFrames", (double)recFrames);

  // Leave the world regenerated: this gate ran 240 ticks over three arms and
  // the gates after it inherit whatever it leaves (selftest.h's ordering note).
  stream.OnRegen();
  world.SetWindowOrigin({0, 0, 0});
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  char buf[832];
  std::snprintf(buf, sizeof(buf),
                "%s (subject chunk (%d,%d,%d) slot %u, %.0f%% solid | drift "
                "moved %zu slot | repaired: digest %08x -> %08x (control "
                "%08x), world hash %08x == control | replay %u frames, %u "
                "replace, hash %08x | Stream applied %llu refused %llu%s%s)",
                fails.empty() ? "PASS" : "FAIL", subject.x, subject.y,
                subject.z, slot, 100.0 * solid / (double)kChunkVol,
                moved.size(), subj.tableBeforeFix[slot], subj.tableFinal[slot],
                ctrl.tableFinal[slot], subj.finalHash, replayFrames,
                replayReplaces, replayHash,
                (unsigned long long)stream.ChunkReplacesApplied(),
                (unsigned long long)stream.ChunkReplacesRefused(),
                fails.empty() ? "" : " | ", fails.c_str());
  detail = buf;
  std::printf("chunk-resync: %s\n", buf);
  return fails.empty() ? Status::Pass : Status::Fail;
}

}  // namespace

// ---- stain-react: a coat on the ground is matter that reacts ---------------
//
// DESIGN.md §6 "A coat is a co-located virtual neighbour" (rule-unification
// W2-J1; sim_step.wgsl coatReact). Five strips of ground on one stone slab in
// open air, each beside a row of heat (lava or ember) or of stone, all at once:
//
//   oil    lava  | dirt wearing oil (6)     -- dirt is not flammable; its oil is
//   clean  lava  | clean dirt               -- the control: heat does nothing
//   dry    ember | grass
//   wet    ember | grass wearing water (12)
//   inert  stone | dirt wearing oil (6)     -- a coat with no partner is inert
//   oilgr  ember | grass wearing oil (6)    -- clause 2c: the flash is made ON
//                                              the grass, which catches
//
// Claims, each against its arm's control:
//   IGNITES    flame over the oiled dirt, sampled every tick, exceeds the clean
//              dirt's (lava makes no flame of its own)
//   BURNS OUT  the oiled dirt's oil is all spent, and the dirt is still dirt
//   WET        wet grass burnt by tick stainReactWetTicks is at most
//              stainReactWetMaxFrac of dry grass burnt, and dry burnt at least
//              stainReactDryMinPct -- wet resists, it is not merely slower luck
//   CLEAN      the clean dirt is unchanged: dirt, unstained
//   INERT      the partnerless oil is untouched, level for level
//   CATCHES    oiled grass catches faster than dry grass beside the same ember:
//              grass-cell-ticks still unburnt over the first stainReactCatchTicks
//              ticks are at most stainReactCatchMaxFrac of the dry arm's
//              (clause 2c: the oil's flash lights its wearer, at oil's chance)
//   SLEEPS     heat replaced by stone, 1000 ticks later every chunk the fixture
//              occupies is asleep with the inert oil still on it (rule 2: a coat
//              without a partner marks nothing)
// Weather pinned clear (rain would damp the ignition rules); world regenerated
// on the way out (rule 7).
Status GateStainReact(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  auto matId = [&](const char* n) -> uint32_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == n) return (uint32_t)i;
    return 0;
  };
  const uint32_t mDirt = matId("dirt"), mGrass = matId("grass"),
                 mEmber = matId("ember"), mStone = matId("stone"),
                 mOil = matId("oil"), mWater = matId("water"),
                 mFire = matId("fire"), mLava = matId("lava");
  if (!mDirt || !mGrass || !mEmber || !mStone || !mOil || !mWater || !mFire ||
      !mLava) {
    detail = "dirt/grass/ember/stone/oil/water/fire/lava missing from materials.json";
    return Status::Fail;
  }
  const uint32_t oilType = c.mats[mOil].gpu.stainPack & kStainPackTypeMask;
  const uint32_t wetType = c.mats[mWater].gpu.stainPack & kStainPackTypeMask;
  if (!oilType || !wetType) {
    detail = "oil or water has no ground stain type";
    return Status::Fail;
  }
  const std::string prevPin = weather::Override();
  weather::SetOverride("clear");
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  enum { kOil, kClean, kDry, kWet, kInert, kOilGrass, kArms };
  const int L = 12, x0 = 150, z0 = 300, kPitch = 6;
  const int zSpan = kPitch * kArms;
  const int y = FixtureYOver(x0 - 4, z0 - 4, x0 + L + 4, z0 + zSpan + 4,
                             kDefaultSeed, 24);
  auto heatZ = [&](int a) { return z0 + a * kPitch; };
  auto fuelZ = [&](int a) { return z0 + a * kPitch + 1; };

  // One word per cell, decided once (two ops on one cell: the lower index
  // wins, so the box is assembled here rather than pushed in layers).
  std::map<std::tuple<int, int, int>, uint32_t> cells;
  for (int x = x0 - 4; x <= x0 + L + 3; x++)
    for (int z = z0 - 4; z <= z0 + zSpan + 3; z++) {
      cells[{x, y - 1, z}] = mStone;                      // the slab
      for (int yy = y; yy <= y + 6; yy++) cells[{x, yy, z}] = 0;  // clear air
    }
  for (int a = 0; a < kArms; a++) {
    // Heat that does not go out for the dirt arms: a walled channel of lava
    // (no rule of lava's touches dirt). Embers decay, and a first version with
    // embers left 14 levels of oil beside the three that had gone out, which
    // measured the heat, not the coat. The GRASS arms keep embers: lava's own
    // `lava + tag:organic -> fire` rewrites grass whatever it wears (only the
    // wearer sees its coat), which is not the claim.
    uint32_t heat = mEmber;
    if (a == kOil || a == kClean) heat = mLava | (7u << 12);  // born full
    if (a == kInert) heat = mStone;
    uint32_t fuel = mDirt;
    if (a == kOil || a == kInert) fuel = mDirt | PackStain(oilType, 6);
    if (a == kDry) fuel = mGrass;
    if (a == kOilGrass) fuel = mGrass | PackStain(oilType, 6);
    if (a == kWet) fuel = mGrass | PackStain(wetType, 12);
    for (int x = x0; x < x0 + L; x++) {
      cells[{x, y, heatZ(a)}] = heat;
      cells[{x, y, fuelZ(a)}] = fuel;
      cells[{x, y, fuelZ(a) + 1}] = mStone;  // guard: dirt cannot slide off
    }
    for (int dz = -1; dz < 3; dz++) {        // end caps, same reason
      cells[{x0 - 1, y, heatZ(a) + dz}] = mStone;
      cells[{x0 + L, y, heatZ(a) + dz}] = mStone;
    }
    for (int x = x0; x < x0 + L; x++)        // the lava channel's back wall
      cells[{x, y, heatZ(a) - 1}] = mStone;
  }
  std::vector<CellOp> scene;
  for (const auto& [p, wd] : cells)
    scene.push_back({World::SlotCellIndex({std::get<0>(p), std::get<1>(p),
                                           std::get<2>(p)}),
                     wd});

  // Readback of just the fixture's chunks.
  std::map<uint32_t, std::vector<uint32_t>> chunkBuf;
  for (const auto& [p, wd] : cells)
    chunkBuf[World::SlotCellIndex({std::get<0>(p), std::get<1>(p),
                                   std::get<2>(p)}) / kChunkVol];
  auto readFixture = [&](const char* label) {
    ctx.WaitIdle();
    for (auto& [slot, buf] : chunkBuf) {
      buf.resize(kChunkVol);
      ReadVoxelsSync(ctx, world, slot, 1, buf.data(), label);
    }
  };
  auto at = [&](int x, int yy, int z) -> uint32_t {
    const uint32_t ci = World::SlotCellIndex({x, yy, z});
    auto it = chunkBuf.find(ci / kChunkVol);
    return it == chunkBuf.end() ? 0u : it->second[ci % kChunkVol];
  };
  struct ArmStat {
    uint32_t fuelMat = 0, stainType = 0, levels = 0, flame = 0, heat = 0;
  };
  auto armStat = [&](int a) {
    ArmStat s;
    for (int x = x0; x < x0 + L; x++) {
      const uint32_t hm = at(x, y, heatZ(a)) & 0xFFFu;
      if (hm == mEmber || hm == mLava) s.heat++;
      const uint32_t w = at(x, y, fuelZ(a));
      const uint32_t want =
          (a == kDry || a == kWet || a == kOilGrass) ? mGrass : mDirt;
      if ((w & 0xFFFu) == want) s.fuelMat++;
      if (VoxStainAmt(w)) s.stainType |= 1u << VoxStainType(w);
      s.levels += VoxStainAmt(w);
      for (int yy = y + 1; yy <= y + 2; yy++)
        if ((at(x, yy, fuelZ(a)) & 0xFFFu) == mFire) s.flame++;
    }
    return s;
  };

  uint32_t t = 1;
  const IVec3 pc{(x0 + L / 2) >> 4, y >> 4, (z0 + zSpan / 2) >> 4};
  SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, scene, false, pc, false,
             false);
  const uint32_t kWetTicks = (uint32_t)BaselineNumber("stainReactWetTicks", 40.0);
  const uint32_t kBurnTicks = (uint32_t)BaselineNumber("stainReactBurnTicks", 200.0);
  uint32_t flameOil = 0, flameClean = 0;
  const uint32_t kCatchTicks =
      (uint32_t)BaselineNumber("stainReactCatchTicks", 10.0);
  uint32_t unburntDry = 0, unburntOilGrass = 0;  // grass-cell-ticks
  ArmStat wetAt[kArms];
  // The oil arm's burn, as a trace: t:levels/embers left. Attribution for a
  // BURNS OUT failure (a coat that stops reacting vs heat that went out).
  std::string oilTrace;
  for (uint32_t i = 2; i <= kBurnTicks; i++) {
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false, pc, false,
               false);
    // Flame over the fuel rows, every tick while the oil can burn.
    if (i <= 40) {
      readFixture("stainReactFlame");
      flameOil += armStat(kOil).flame;
      flameClean += armStat(kClean).flame;
      if (i <= kCatchTicks) {
        unburntDry += armStat(kDry).fuelMat;
        unburntOilGrass += armStat(kOilGrass).fuelMat;
      }
    }
    if (i == 5 || i == 10 || i == 20 || i == 40 || i == 100 || i == kBurnTicks) {
      if (i > 40) readFixture("stainReactTrace");
      const ArmStat o = armStat(kOil);
      oilTrace += " t" + std::to_string(i) + ":" + std::to_string(o.levels) +
                  "/" + std::to_string(o.heat);
    }
    if (i == kWetTicks) {
      readFixture("stainReactWet");
      for (int a = 0; a < kArms; a++) wetAt[a] = armStat(a);
    }
  }
  readFixture("stainReactBurn");
  ArmStat burnt[kArms];
  for (int a = 0; a < kArms; a++) burnt[a] = armStat(a);

  // Quench: the heat rows become stone, then let everything that was burning
  // go out, and ask whether the fixture's chunks sleep with coats on them.
  std::vector<CellOp> quench;
  for (int a = 0; a < kArms; a++)
    for (int x = x0; x < x0 + L; x++)
      quench.push_back({World::SlotCellIndex({x, y, heatZ(a)}), mStone});
  SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, quench, false, pc,
             false, false);
  const uint32_t kRestTicks = (uint32_t)BaselineNumber("stainReactRestTicks", 1000.0);
  for (uint32_t i = 0; i < kRestTicks; i++)
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false, pc, false,
               false);
  ctx.WaitIdle();
  readFixture("stainReactRest");
  const ArmStat inertEnd = armStat(kInert);
  std::vector<uint32_t> dirty(kNumSlots, 0);
  rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DirtyActive(), 0, dirty.data(),
                        kNumSlots * 4, "stainReactDirty");
  uint32_t awake = 0;
  for (const auto& [slot, buf] : chunkBuf)
    if (dirty[slot] != 0) awake++;

  weather::SetOverride(prevPin);
  weather::Snap();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  const uint32_t Lu = (uint32_t)L;
  const double wetMaxFrac = BaselineNumber("stainReactWetMaxFrac", 0.5);
  const double dryMinPct = BaselineNumber("stainReactDryMinPct", 75.0);
  const uint32_t dryBurnt = Lu - wetAt[kDry].fuelMat;
  const uint32_t wetBurnt = Lu - wetAt[kWet].fuelMat;
  const bool ignites = flameOil > flameClean;
  const bool burnsOut = burnt[kOil].levels == 0 && burnt[kOil].fuelMat == Lu;
  const bool wetOk = 100.0 * dryBurnt >= dryMinPct * Lu &&
                     (double)wetBurnt <= wetMaxFrac * (double)dryBurnt &&
                     wetAt[kWet].levels < 12u * Lu;
  const bool cleanOk = burnt[kClean].fuelMat == Lu && burnt[kClean].levels == 0;
  const bool inertOk = burnt[kInert].levels == 6u * Lu &&
                       inertEnd.levels == 6u * Lu &&
                       inertEnd.stainType == (1u << oilType);
  const bool sleeps = awake == 0;
  const double catchMaxFrac = BaselineNumber("stainReactCatchMaxFrac", 0.6);
  const bool catches = unburntDry > 0 &&
                       (double)unburntOilGrass <= catchMaxFrac * (double)unburntDry;
  const bool ok = ignites && burnsOut && wetOk && cleanOk && inertOk && sleeps &&
                  catches;

  char buf[1536];
  std::snprintf(
      buf, sizeof(buf),
      "%s: IGNITES %s (flame-ticks over oiled dirt %u vs clean %u, first 40 "
      "ticks) | BURNS OUT %s (oil levels %u -> %u by t%u, dirt %u/%u; "
      "levels/heat%s) | WET %s "
      "(t%u: dry burnt %u/%u, wet burnt %u/%u, wet levels %u/%u) | CLEAN %s "
      "(dirt %u/%u, levels %u) | INERT %s (oil levels %u, %u after rest) | "
      "SLEEPS %s (%u of %zu fixture chunks awake %u ticks after quench) | "
      "CATCHES %s (grass-cell-ticks unburnt over t2..t%u: oiled %u vs dry %u)",
      ok ? "PASS" : "FAIL", ignites ? "ok" : "FAIL", flameOil, flameClean,
      burnsOut ? "ok" : "FAIL", 6u * Lu, burnt[kOil].levels, kBurnTicks,
      burnt[kOil].fuelMat, Lu, oilTrace.c_str(), wetOk ? "ok" : "FAIL",
      kWetTicks, dryBurnt, Lu,
      wetBurnt, Lu, wetAt[kWet].levels, 12u * Lu, cleanOk ? "ok" : "FAIL",
      burnt[kClean].fuelMat, Lu, burnt[kClean].levels, inertOk ? "ok" : "FAIL",
      burnt[kInert].levels, inertEnd.levels, sleeps ? "ok" : "FAIL", awake,
      chunkBuf.size(), kRestTicks, catches ? "ok" : "FAIL", kCatchTicks,
      unburntOilGrass, unburntDry);
  RecordObserved("stainReact.catchUnburntOilGrass", (double)unburntOilGrass);
  RecordObserved("stainReact.catchUnburntDry", (double)unburntDry);
  RecordObserved("stainReact.flameOil", (double)flameOil);
  detail = buf;
  std::printf("stain-react: %s\n", buf);
  return ok ? Status::Pass : Status::Fail;
}

// ---- rain-stain: the newest stain wins, rain wets, wet dries -----------------
//
// 2026-09-25 (the owner: "pouring oil over blood-stained sand should rewrite
// it to oil stains; rain should wet the ground and the wet should go away on
// its own, which removes the blood"). Four strips on a stone slab in open air,
// each walled so a liquid poured on it stays on it:
//
//   oilOver  dirt wearing blood (12), a layer of OIL poured on it
//   open     dirt wearing blood (12)                     -- the storm's subject
//   stone    stone wearing blood (1), no dirt            -- a free mark, same claim
//   roofed   dirt wearing blood (12) under a stone roof  -- the control
//
// Claims:
//   OIL OVER  (clear sky, rainStainOilTicks) the oiled strip's dirt wears oil,
//             not blood: blood cells 0, oil cells >= rainStainOilMinFrac
//   WASHES    (storm, rainStainStormTicks, the oil lifted off first) every
//             cell on the open, stone and oilOver strips wears WET, none blood
//             or oil -- rain replaced them through stainStep (sim_mutate.wgsl
//             rainFall), it did not delete them
//   ROOFED    the roofed strip keeps all its blood (rain comes from the sky)
//   DRIES     (clear again, rainStainDryTicks) the wet levels on the three
//             rained strips fall to at most rainStainDryMaxFrac of their peak
//   SLEEPS    at the end no fixture chunk is awake: drying a top surface is
//             the sampler's job and holds nothing awake (rule 2)
// Weather pinned and restored; world regenerated on the way out (rule 7).
Status GateRainStain(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  auto matId = [&](const char* n) -> uint32_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == n) return (uint32_t)i;
    return 0;
  };
  const uint32_t mDirt = matId("dirt"), mStone = matId("stone"),
                 mOil = matId("oil"), mWater = matId("water"),
                 mBlood = matId("blood");
  if (!mDirt || !mStone || !mOil || !mWater || !mBlood) {
    detail = "dirt/stone/oil/water/blood missing from materials.json";
    return Status::Fail;
  }
  const uint32_t oilType = c.mats[mOil].gpu.stainPack & kStainPackTypeMask;
  const uint32_t wetType = c.mats[mWater].gpu.stainPack & kStainPackTypeMask;
  const uint32_t bloodType = c.mats[mBlood].gpu.stainPack & kStainPackTypeMask;
  if (!oilType || !wetType || !bloodType) {
    detail = "oil, water or blood has no ground stain type";
    return Status::Fail;
  }
  const std::string prevPin = weather::Override();
  // THE WIND IS PINNED CALM (2026-09-30): the rain falls along the tick's
  // slope now (rain-lean), and this fixture's strips are one-voxel trenches
  // walled a voxel above their floor -- a slanted line meets the wall top or
  // the trench's edge before its floor, so under the auto wind the strips
  // would be judged on the geometry of the lean, not on the stain rules this
  // gate is about. The slant is rain-lean's; the stains are this gate's.
  const Tuning savedTune = CurrentTuning();
  {
    Tuning t = savedTune;
    t.wind.weatherAuto = false;
    t.wind.windSpeed = 0.0f;
    SetCurrentTuning(t);
  }
  weather::SetOverride("clear");
  weather::Snap();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  enum { kOilOver, kOpen, kStone, kRoofed, kArms };
  const int L = 12, x0 = 150, z0 = 300, kPitch = 4;
  const int zSpan = kPitch * kArms;
  const int y = FixtureYOver(x0 - 4, z0 - 4, x0 + L + 4, z0 + zSpan + 4,
                             kDefaultSeed, 24);
  auto stripZ = [&](int a) { return z0 + a * kPitch + 1; };
  // One word per cell, decided once (the lower op index wins a cell).
  std::map<std::tuple<int, int, int>, uint32_t> cells;
  for (int x = x0 - 4; x <= x0 + L + 3; x++)
    for (int z = z0 - 4; z <= z0 + zSpan + 3; z++) {
      cells[{x, y - 1, z}] = mStone;
      for (int yy = y; yy <= y + 6; yy++) cells[{x, yy, z}] = 0;
    }
  for (int a = 0; a < kArms; a++) {
    const int z = stripZ(a);
    for (int x = x0 - 1; x <= x0 + L; x++)       // the wall around the strip
      for (int dz = -1; dz <= 1; dz++)
        for (int yy = y; yy <= y + 1; yy++) cells[{x, yy, z + dz}] = mStone;
    for (int x = x0; x < x0 + L; x++) {
      cells[{x, y, z}] = a == kStone ? (mStone | PackStain(bloodType, 1))
                                     : (mDirt | PackStain(bloodType, 12));
      cells[{x, y + 1, z}] = a == kOilOver ? (mOil | (7u << 12)) : 0u;
    }
    if (a == kRoofed)
      for (int x = x0 - 1; x <= x0 + L; x++)
        for (int dz = -1; dz <= 1; dz++) cells[{x, y + 3, z + dz}] = mStone;
  }
  std::vector<CellOp> scene;
  std::map<uint32_t, std::vector<uint32_t>> chunkBuf;
  for (const auto& [p, wd] : cells) {
    const uint32_t ci = World::SlotCellIndex({std::get<0>(p), std::get<1>(p),
                                              std::get<2>(p)});
    scene.push_back({ci, wd});
    chunkBuf[ci / kChunkVol];
  }
  auto readFixture = [&](const char* label) {
    ctx.WaitIdle();
    for (auto& [slot, buf] : chunkBuf) {
      buf.resize(kChunkVol);
      ReadVoxelsSync(ctx, world, slot, 1, buf.data(), label);
    }
  };
  auto at = [&](int x, int yy, int z) -> uint32_t {
    const uint32_t ci = World::SlotCellIndex({x, yy, z});
    auto it = chunkBuf.find(ci / kChunkVol);
    return it == chunkBuf.end() ? 0u : it->second[ci % kChunkVol];
  };
  struct Strip { uint32_t blood = 0, oil = 0, wet = 0, wetLevels = 0, ground = 0; };
  auto strip = [&](int a) {
    Strip s;
    for (int x = x0; x < x0 + L; x++) {
      const uint32_t w = at(x, y, stripZ(a));
      const uint32_t want = a == kStone ? mStone : mDirt;
      if ((w & 0xFFFu) == want) s.ground++;
      if (!VoxStainAmt(w)) continue;
      const uint32_t t = VoxStainType(w);
      if (t == bloodType) s.blood++;
      if (t == oilType) s.oil++;
      if (t == wetType) { s.wet++; s.wetLevels += VoxStainAmt(w); }
    }
    return s;
  };
  auto fmt = [](const Strip& s) {
    char b[96];
    std::snprintf(b, sizeof(b), "blood %u oil %u wet %u (lv %u) ground %u",
                  s.blood, s.oil, s.wet, s.wetLevels, s.ground);
    return std::string(b);
  };

  uint32_t t = 1;
  const IVec3 pc{(x0 + L / 2) >> 4, y >> 4, (z0 + zSpan / 2) >> 4};
  auto run = [&](uint32_t n, const std::vector<CellOp>& first) {
    for (uint32_t i = 0; i < n; i++)
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {},
                 i == 0 ? first : std::vector<CellOp>{}, false, pc, false, false);
  };
  const uint32_t kOilTicks = (uint32_t)BaselineNumber("rainStainOilTicks", 120.0);
  const uint32_t kStormTicks = (uint32_t)BaselineNumber("rainStainStormTicks", 900.0);
  const uint32_t kDryTicks = (uint32_t)BaselineNumber("rainStainDryTicks", 2400.0);
  const double oilMinFrac = BaselineNumber("rainStainOilMinFrac", 0.75);
  const double dryMaxFrac = BaselineNumber("rainStainDryMaxFrac", 0.05);

  // 1. Oil over blood, clear sky.
  run(kOilTicks, scene);
  readFixture("rainStainOil");
  const Strip oiled = strip(kOilOver);
  // 2. Lift what is left of the oil off, then a storm.
  std::vector<CellOp> lift;
  for (int x = x0; x < x0 + L; x++)
    for (int yy = y + 1; yy <= y + 2; yy++)
      lift.push_back({World::SlotCellIndex({x, yy, stripZ(kOilOver)}), 0u});
  weather::SetOverride("storm");
  weather::Snap();
  const uint32_t stormWord = weather::SimRainWord(CurrentTuning(), kDefaultSeed, t + 1);
  run(kStormTicks, lift);
  readFixture("rainStainStorm");
  Strip storm[kArms];
  for (int a = 0; a < kArms; a++) storm[a] = strip(a);
  // 3. Clear again: the wet dries.
  weather::SetOverride("clear");
  weather::Snap();
  run(kDryTicks, {});
  readFixture("rainStainDry");
  Strip dry[kArms];
  for (int a = 0; a < kArms; a++) dry[a] = strip(a);
  std::vector<uint32_t> dirty(kNumSlots, 0);
  rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DirtyActive(), 0, dirty.data(),
                        kNumSlots * 4, "rainStainDirty");
  uint32_t awake = 0;
  for (const auto& [slot, buf] : chunkBuf)
    if (dirty[slot] != 0) awake++;

  weather::SetOverride(prevPin);
  weather::Snap();
  SetCurrentTuning(savedTune);
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  const uint32_t Lu = (uint32_t)L;
  const bool oilOk = oiled.blood == 0 && oiled.oil >= oilMinFrac * Lu;
  bool washes = (stormWord & kRainAmountMask) != 0;
  uint32_t peak = 0, left = 0;
  for (int a : {(int)kOilOver, (int)kOpen, (int)kStone}) {
    washes = washes && storm[a].blood == 0 && storm[a].oil == 0 &&
             storm[a].wet == storm[a].ground && storm[a].ground > 0;
    peak += storm[a].wetLevels;
    left += dry[a].wetLevels;
  }
  const bool roofed = storm[kRoofed].blood == Lu && dry[kRoofed].blood == Lu;
  const bool dries = peak > 0 && left <= dryMaxFrac * peak;
  const bool sleeps = awake == 0;
  const bool ok = oilOk && washes && roofed && dries && sleeps;
  RecordObserved("rainStain.peakWetLevels", (double)peak);
  RecordObserved("rainStain.leftWetLevels", (double)left);

  char buf[1024];
  std::snprintf(
      buf, sizeof(buf),
      "%s: OIL OVER %s (t%u: %s) | WASHES %s (storm rain %u, %u ticks: oilOver "
      "%s | open %s | stone %s) | ROOFED %s (%s) | DRIES %s (wet levels %u -> "
      "%u after %u clear ticks, want <= %.2f) | SLEEPS %s (%u of %zu fixture "
      "chunks awake)",
      ok ? "PASS" : "FAIL", oilOk ? "ok" : "FAIL", kOilTicks, fmt(oiled).c_str(),
      washes ? "ok" : "FAIL", stormWord & kRainAmountMask, kStormTicks,
      fmt(storm[kOilOver]).c_str(), fmt(storm[kOpen]).c_str(),
      fmt(storm[kStone]).c_str(), roofed ? "ok" : "FAIL",
      fmt(dry[kRoofed]).c_str(), dries ? "ok" : "FAIL", peak, left, kDryTicks,
      dryMaxFrac, sleeps ? "ok" : "FAIL", awake, chunkBuf.size());
  detail = buf;
  std::printf("rain-stain: %s\n", buf);
  return ok ? Status::Pass : Status::Fail;
}

// ---- rain-lean: the rain falls ALONG ITS SLOPE, and only where it lands ------
//
// 2026-09-30 (DESIGN.md §9.w "Where the rain lands"). The sim's rain follows a
// slope on the tick stream (weather::SimRain -> TickParams rainSlopeQx/Qz): the
// ground sampler (sim_mutate rainFall) walks the slanted fall line, and a rain
// DOUSE reaches a cell only if the rain exposure map (sim_rain_expo, built
// before the CA) says it is the first blocker on its fall line. This gate
// builds one floating stone fixture -- a pad, a roofed hut with a doorway in
// its -Z wall -- and asserts, under a pinned wind blowing +Z (into the door):
//
//   rain, no wind (slope 0): embers on the open pad are doused, embers on the
//     hut floor just inside the door are NOT (the roof is over them);
//   storm, 30 m/s (slope at the 70-degree cap): embers on the open pad ARE
//     doused, the same embers inside the door ARE (the rain comes in at 70
//     degrees), embers behind the hut's lee wall are NOT;
//   stains after the storm: the windward (-Z) wall's outer face wet, the lee
//     (+Z) face dry;
//   the CPU mirror (MobSystem::RainExposedCpu's walk, rainexpo.h
//     ExposedWalkUp over the read-back grid) agrees with the GPU map at every
//     sampled cell of the fixture, before any fire exists to move under it.
//
// An ember becomes WOOD only by a douse (its `rain` rule or an extinguisher,
// and there is none): so "doused" is a wood cell at the site, and a sheltered
// site must show exactly none. The pin, the tuning and the world are restored
// on the way out (rule 7).
Status GateRainLean(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  auto matId = [&](const char* n) -> uint32_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == n) return (uint32_t)i;
    return 0;
  };
  const uint32_t mStone = matId("stone"), mEmber = matId("ember"),
                 mWood = matId("wood"), mWater = matId("water");
  if (!mStone || !mEmber || !mWood || !mWater) {
    detail = "stone/ember/wood/water missing from materials.json";
    return Status::Fail;
  }
  const uint32_t wetType = c.mats[mWater].gpu.stainPack & kStainPackTypeMask;
  const std::string prevPin = weather::Override();
  const Tuning saved = CurrentTuning();
  const uint32_t kTicks = (uint32_t)BaselineNumber("rainLeanTicks", 240.0);

  // ---- the fixture, relative to (cx, y0, cz); y0 is the pad's top ----
  const int cx = 200, cz = 200;
  const int kPadX = 30, kPadZ = 44;          // pad half-extents
  const int kHx = 10, kHz = 12, kH = 10;     // hut half-extents, wall height
  const int kDoorX = 4, kDoorH = 8;          // doorway half-width, height
  const int y0 = FixtureYOver(cx - kPadX, cz - kPadZ, cx + kPadX, cz + kPadZ,
                              kDefaultSeed, 24);
  std::map<std::tuple<int, int, int>, uint32_t> fixture;
  for (int x = -kPadX; x <= kPadX; x++)
    for (int z = -kPadZ; z <= kPadZ; z++) {
      fixture[{cx + x, y0, cz + z}] = mStone;
      for (int y = 1; y <= kH + 6; y++) fixture[{cx + x, y0 + y, cz + z}] = 0u;
    }
  for (int x = -kHx; x <= kHx; x++)
    for (int z = -kHz; z <= kHz; z++) {
      fixture[{cx + x, y0 + kH + 1, cz + z}] = mStone;   // roof, two thick
      fixture[{cx + x, y0 + kH + 2, cz + z}] = mStone;
    }
  for (int y = 1; y <= kH; y++) {
    for (int x = -kHx; x <= kHx; x++) {
      const bool door = std::abs(x) <= kDoorX && y <= kDoorH;
      if (!door) fixture[{cx + x, y0 + y, cz - kHz}] = mStone;
      fixture[{cx + x, y0 + y, cz + kHz}] = mStone;
    }
    for (int z = -kHz; z <= kHz; z++) {
      fixture[{cx - kHx, y0 + y, cz + z}] = mStone;
      fixture[{cx + kHx, y0 + y, cz + z}] = mStone;
    }
  }
  std::vector<CellOp> fixtureOps;
  for (const auto& [k, w] : fixture)
    fixtureOps.push_back(
        {World::SlotCellIndex({std::get<0>(k), std::get<1>(k), std::get<2>(k)}), w});

  // ---- the ember sites (on the pad / hut floor, y0 + 1) ----
  enum Site { kOpen, kInside, kLee, kSites };
  const char* kSiteName[kSites] = {"open", "inside", "lee"};
  std::vector<IVec3> sites[kSites];
  for (int x = -12; x <= 12; x += 3) sites[kOpen].push_back({cx + x, y0 + 1, cz - 32});
  for (int x : {-3, -1, 1, 3})
    for (int d : {3, 5, 7}) sites[kInside].push_back({cx + x, y0 + 1, cz - kHz + d});
  for (int x : {-6, -2, 2, 6})
    for (int d : {4, 6, 8}) sites[kLee].push_back({cx + x, y0 + 1, cz + kHz + d});

  struct Run {
    const char* preset; float wind;
    weather::TickRain rain;
    uint32_t doused[kSites] = {};
    uint32_t windwardWet = 0, windwardCells = 0, leeWet = 0, leeCells = 0;
    uint32_t mirrorChecked = 0, mirrorMismatch = 0;
    IVec3 firstMismatch{0, 0, 0};
  };
  Run runs[2] = {{"rain", 0.0f, {}}, {"storm", 30.0f, {}}};
  std::vector<uint32_t> vox(kNumSlots * (size_t)kChunkVol);
  for (Run& r : runs) {
    Tuning t = saved;
    t.wind.weatherAuto = false;
    t.wind.windDirDeg = 0.0f;      // heading 0 = downwind +Z: into the door
    t.wind.windSpeed = r.wind;
    t.wind.gustStrength = 0.0f;
    t.weather.rainTouchesWorld = true;
    SetCurrentTuning(t);
    weather::SetOverride(r.preset);
    weather::Snap();
    r.rain = weather::SimRain(t, kDefaultSeed, 1);
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    uint32_t tick = 1;
    SubmitTick(ctx, world, sim, tick, kDefaultSeed, {}, {}, fixtureOps, false,
               {12, 12, 12}, false, false);
    SubmitTick(ctx, world, sim, ++tick, kDefaultSeed, {}, {}, {}, false,
               {12, 12, 12}, false, false);
    ctx.WaitIdle();

    // ---- THE CPU MIRROR vs THE GPU MAP, on the static fixture ----
    if (r.wind > 0.0f) {
      ReadVoxelsSync(ctx, world, 0, kNumSlots, vox.data(), "rainLeanRead");
      std::vector<uint32_t> map((size_t)rainlat::kExpoMaxAxis * rainlat::kExpoMaxAxis);
      rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.RainExpoBuffer(), 0, map.data(),
                            map.size() * 4, "rainLeanMap");
      const IVec3 o = world.WindowOrigin();
      rainlat::Box b;
      b.lo[0] = o.x * (int32_t)kChunk;
      b.lo[1] = o.y * (int32_t)kChunk;
      b.lo[2] = o.z * (int32_t)kChunk;
      for (int a = 0; a < 3; a++) b.hi[a] = b.lo[a] + (int32_t)kWorldN - 1;
      const int32_t nx = rainlat::N(r.rain.slopeQx), nz = rainlat::N(r.rain.slopeQz);
      int32_t lo[2], ext[2];
      rainlat::ExpoDomain(b, nx, nz, lo, ext);
      auto blocks = [&](int32_t x, int32_t y, int32_t z) -> int {
        const uint32_t m = vox[World::SlotCellIndex({x, y, z})] & 0xFFFu;
        if (m == 0 || m >= c.mats.size()) return 0;
        const MaterialGpu& g = c.mats[m].gpu;
        if ((g.flags & kMatFlagMicro) != 0) return 0;
        return (g.klass == CLASS_SOLID || g.klass == CLASS_POWDER ||
                (g.klass == CLASS_LIQUID && (g.flags & kMatFlagOpaque) != 0)) ? 1 : 0;
      };
      for (int x = -kPadX + 2; x <= kPadX - 2; x += 2)
        for (int z = -kPadZ + 2; z <= kPadZ - 2; z += 2)
          for (int y = 1; y <= kH + 4; y += 3) {
            const IVec3 p{cx + x, y0 + y, cz + z};
            const int32_t kx = p.x + rainlat::Drift(nx, p.y);
            const int32_t kz = p.z + rainlat::Drift(nz, p.y);
            const int32_t tx = (kx >> rainlat::kExpoTexShift) - lo[0];
            const int32_t tz = (kz >> rainlat::kExpoTexShift) - lo[1];
            if (tx < 0 || tz < 0 || tx >= ext[0] || tz >= ext[1]) continue;
            const int32_t h = (int32_t)map[(size_t)tz * ext[0] + tx];
            const bool gpu = h == rainlat::kExpoOpen || p.y >= h - rainlat::kExpoMargin;
            const bool cpu = rainlat::ExposedWalkUp(b, nx, nz, p.x, p.y, p.z, blocks);
            r.mirrorChecked++;
            if (gpu != cpu) {
              if (r.mirrorMismatch == 0) r.firstMismatch = p;
              r.mirrorMismatch++;
            }
          }
    }

    // ---- the fire, then the rain on it ----
    std::vector<CellOp> embers;
    for (int s = 0; s < kSites; s++)
      for (const IVec3& p : sites[s]) embers.push_back({World::SlotCellIndex(p), mEmber});
    SubmitTick(ctx, world, sim, ++tick, kDefaultSeed, {}, {}, embers, false,
               {12, 12, 12}, false, false);
    for (uint32_t i = 0; i < kTicks; i++)
      SubmitTick(ctx, world, sim, ++tick, kDefaultSeed, {}, {}, {}, false,
                 {12, 12, 12}, false, false);
    ctx.WaitIdle();
    ReadVoxelsSync(ctx, world, 0, kNumSlots, vox.data(), "rainLeanRead");
    for (int s = 0; s < kSites; s++)
      for (const IVec3& p : sites[s])
        if ((vox[World::SlotCellIndex(p)] & 0xFFFu) == mWood) r.doused[s]++;
    // The walls' OUTER faces: -Z (windward) and +Z (lee), doorway excluded.
    for (int y = 1; y <= kH; y++)
      for (int x = -kHx; x <= kHx; x++) {
        if (!(std::abs(x) <= kDoorX && y <= kDoorH)) {
          const uint32_t w = vox[World::SlotCellIndex({cx + x, y0 + y, cz - kHz})];
          r.windwardCells++;
          if (((w >> 28) & 7u) == wetType) r.windwardWet++;
        }
        const uint32_t w = vox[World::SlotCellIndex({cx + x, y0 + y, cz + kHz})];
        r.leeCells++;
        if (((w >> 28) & 7u) == wetType) r.leeWet++;
      }
  }
  SetCurrentTuning(saved);
  weather::SetOverride(prevPin);
  weather::Snap();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  const Run& calm = runs[0];
  const Run& storm = runs[1];
  const double minFrac = BaselineNumber("rainLeanDousedMinFrac", 0.25);
  const double windwardMin = BaselineNumber("rainLeanWindwardWetMinFrac", 0.5);
  auto frac = [&](uint32_t n, int s) { return (double)n / (double)sites[s].size(); };
  const bool slopeOk = calm.rain.slopeQx == 0 && calm.rain.slopeQz == 0 &&
                       storm.rain.slopeQx == 0 &&
                       storm.rain.slopeQz == weather::kRainSlopeMaxN * 4096 &&
                       (calm.rain.word & 0xFFu) != 0 && (storm.rain.word & 0xFFu) != 0;
  const bool calmOk = frac(calm.doused[kOpen], kOpen) >= minFrac && calm.doused[kInside] == 0;
  const bool stormOk = frac(storm.doused[kOpen], kOpen) >= minFrac &&
                       frac(storm.doused[kInside], kInside) >= minFrac &&
                       storm.doused[kLee] == 0;
  const bool stainOk =
      (double)storm.windwardWet >= windwardMin * (double)storm.windwardCells &&
      storm.leeWet == 0;
  const bool mirrorOk = storm.mirrorChecked > 0 && storm.mirrorMismatch == 0;
  const bool ok = slopeOk && calmOk && stormOk && stainOk && mirrorOk;

  char buf[768];
  std::snprintf(
      buf, sizeof(buf),
      "%s: slope calm %d/%d storm %d/%d sixteenths (%s) | doused after %u ticks "
      "(wood / sites): calm open %u/%zu inside %u/%zu (%s: open >= %.0f%%, "
      "inside 0) | storm open %u/%zu inside %u/%zu lee %u/%zu (%s: open, "
      "inside >= %.0f%%, lee 0) | stains after the storm: windward face %u/%u "
      "wet, lee face %u/%u (%s: windward >= %.0f%%, lee 0) | CPU mirror vs GPU "
      "map: %u/%u cells agree%s",
      ok ? "PASS" : "FAIL", rainlat::N(calm.rain.slopeQx), rainlat::N(calm.rain.slopeQz),
      rainlat::N(storm.rain.slopeQx), rainlat::N(storm.rain.slopeQz),
      slopeOk ? "ok" : "FAIL", kTicks, calm.doused[kOpen], sites[kOpen].size(),
      calm.doused[kInside], sites[kInside].size(), calmOk ? "ok" : "FAIL",
      100.0 * minFrac, storm.doused[kOpen], sites[kOpen].size(),
      storm.doused[kInside], sites[kInside].size(), storm.doused[kLee],
      sites[kLee].size(), stormOk ? "ok" : "FAIL", 100.0 * minFrac,
      storm.windwardWet, storm.windwardCells, storm.leeWet, storm.leeCells,
      stainOk ? "ok" : "FAIL", 100.0 * windwardMin,
      storm.mirrorChecked - storm.mirrorMismatch, storm.mirrorChecked,
      mirrorOk ? "" : " (MIRROR DISAGREES)");
  if (storm.mirrorMismatch)
    std::snprintf(buf + std::strlen(buf), sizeof(buf) - std::strlen(buf),
                  "; first at (%d,%d,%d)", storm.firstMismatch.x,
                  storm.firstMismatch.y, storm.firstMismatch.z);
  (void)kSiteName;
  detail = buf;
  std::printf("rain-lean: %s\n", buf);
  return ok ? Status::Pass : Status::Fail;
}

// ---- stamp-sleep: a matched cell keeps its chunk awake whatever the stamp says
//
// Rule-unification W2-R. sim_step.wgsl's substep gate skips a cell whose tick
// stamp equals this substep's stampFor(). The field cycles 1..7 (common.wgsl
// STAMP_CYCLE), so a cell that MOVED and then sits still carries a stale stamp
// that equals the current one once every 7 ticks: substep 0 aliases 4 ticks
// after a substep-1 write, 7 after a substep-0 one. Reactions (and staining)
// only run on substep 0, so on that tick a matched-but-unfired cell never
// reaches its keepAwake mark -- and if it was the only mark in its chunk, the
// chunk sleeps with the reaction still pending, forever. W2-J1 met it as 2 of
// 72 oil levels left unburnt beside live lava and worked around it for coats.
//
// Fixture, no coats: kArms steel shafts, one per chunk (acid's REACT mark is
// own-chunk only, so each arm sleeps or wakes on its own), each with an IRON
// floor cell. One full acid voxel is dropped 1 or 2 cells down the shaft, arm
// a at tick 2 + a, so each arrives carrying a LIVE stamp (a longer drop lands
// STAMP_NEVER -- measured: drops of 3..8 all did, so they never alias and
// test nothing) from a different tick/substep phase, and rests on the iron: `acid + iron -> air` matches at 10 per mille, nothing else
// in the chunk has a rule, and steel has no rule of acid's. Weather pinned
// clear (rain in the shaft would wake it); regenerated on the way out.
//
// Claims:
//   AWAKE   for every tick of the watch window, every arm whose acid rests on
//           its iron has its chunk dirty for the next tick. The REPORTER, per
//           violation: the tick t the chunk went unmarked, the acid's stamp,
//           and stampFor(t, 0) -- equal is the alias, attributed on one line.
//   EATS    by stampSleepTicks every arm's iron is gone (p = 1 - e^-15 each)
//   SLEEPS  and every arm's chunk is asleep (acid on steel matches nothing)
//   STAMPED precondition: the acid came to rest carrying a LIVE stamp in at
//           least one arm, so the fixture exercises the case at all
Status GateStampSleep(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  auto matId = [&](const char* n) -> uint32_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == n) return (uint32_t)i;
    return 0;
  };
  const uint32_t mAcid = matId("acid"), mIron = matId("iron"),
                 mSteel = matId("steel");
  if (!mAcid || !mIron || !mSteel) {
    detail = "acid/iron/steel missing from materials.json";
    return Status::Fail;
  }
  // common.wgsl stampFor(); the CPU never WRITES one (world.h kStampNever),
  // this only names the alias in the report.
  auto stampFor = [](uint32_t tick, uint32_t sub) {
    return ((tick * 2u + sub) % 7u) + 1u;
  };
  const std::string prevPin = weather::Override();
  weather::SetOverride("clear");
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  constexpr int kArms = 8;
  const int cz = 20 * 16 + 8;  // chunk-centred: marks never fan to a neighbour
  struct Arm {
    int x = 0, yIron = 0;
    uint32_t slot = 0;          // the acid's (and iron's) chunk slot
    int sleptAt = -1;           // first tick the chunk went unmarked while matched
    uint32_t sleptStamp = 0;    // acid's stamp then
    bool alias = false;
    bool rested = false;
    uint32_t restStamp = 0;     // acid's stamp the first tick it rests on iron
    int eatenAt = -1;
  };
  Arm arms[kArms];
  std::map<std::tuple<int, int, int>, uint32_t> cells;
  for (int a = 0; a < kArms; a++) {
    Arm& A = arms[a];
    A.x = (12 + 2 * a) * 16 + 8;  // every other chunk: no shared faces either
    int hMin = INT32_MAX, hMax = INT32_MIN;
    for (int dx = -1; dx <= 1; dx++)
      for (int dz = -1; dz <= 1; dz++) {
        const int h = World::TerrainHeight(A.x + dx, cz + dz, kDefaultSeed);
        hMin = std::min(hMin, h);
        hMax = std::max(hMax, h);
      }
    // Iron at chunk-local y 2 of the first chunk wholly above the ground, so
    // the acid's chunk holds nothing but this shaft; the steel runs down into
    // the terrain so the column is anchored (not a floating island).
    A.yIron = ((hMax + 2 + 15) & ~15) + 2;
    const int top = A.yIron + 10;
    for (int yy = hMin - 2; yy <= top; yy++)
      for (int dx = -1; dx <= 1; dx++)
        for (int dz = -1; dz <= 1; dz++) {
          uint32_t wd = mSteel;
          if (dx == 0 && dz == 0) {
            if (yy == A.yIron) wd = mIron;
            else if (yy > A.yIron) wd = 0;
          }
          cells[{A.x + dx, yy, cz + dz}] = wd;
        }
    for (int yy = top + 1; yy <= top + 3; yy++)  // clear air over the mouth
      for (int dx = -1; dx <= 1; dx++)
        for (int dz = -1; dz <= 1; dz++) cells[{A.x + dx, yy, cz + dz}] = 0;
    A.slot = World::SlotCellIndex({A.x, A.yIron, cz}) / kChunkVol;
  }
  std::vector<CellOp> scene;
  for (const auto& [p, wd] : cells)
    scene.push_back({World::SlotCellIndex({std::get<0>(p), std::get<1>(p),
                                           std::get<2>(p)}),
                     wd});

  std::vector<uint32_t> buf(kChunkVol);
  std::vector<uint32_t> dirty(kNumSlots, 0);
  auto wordAt = [&](const Arm& A, int yy) {
    return buf[World::SlotCellIndex({A.x, yy, cz}) % kChunkVol];
  };

  uint32_t t = 1;
  const IVec3 pc{arms[kArms / 2].x >> 4, arms[0].yIron >> 4, cz >> 4};
  SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, scene, false, pc, false,
             false);
  const uint32_t kWatch = (uint32_t)BaselineNumber("stampSleepWatchTicks", 300.0);
  const uint32_t kTicks = (uint32_t)BaselineNumber("stampSleepTicks", 1500.0);
  uint32_t matchedTicks = 0;  // arm-ticks observed matched (the denominator)
  for (uint32_t i = 2; i <= kTicks; i++) {
    std::vector<CellOp> drop;
    if (i - 2 < (uint32_t)kArms) {
      const Arm& A = arms[i - 2];
      drop.push_back({World::SlotCellIndex({A.x, A.yIron + 2 + (int)((i - 2) & 1u), cz}),
                      mAcid | (7u << 12)});  // a full voxel, 1 or 2 cells up
    }
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, drop, false, pc,
               false, false);
    if (i > kWatch) continue;
    ctx.WaitIdle();
    rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DirtyActive(), 0,
                          dirty.data(), kNumSlots * 4, "stampSleepDirty");
    for (Arm& A : arms) {
      if (A.eatenAt >= 0) continue;
      ReadVoxelsSync(ctx, world, A.slot, 1, buf.data(), "stampSleepArm");
      const uint32_t floor = wordAt(A, A.yIron), acid = wordAt(A, A.yIron + 1);
      if ((floor & 0xFFFu) != mIron) { A.eatenAt = (int)t; continue; }
      if ((acid & 0xFFFu) != mAcid) continue;  // still falling
      if (!A.rested) { A.rested = true; A.restStamp = VoxStamp(acid); }
      matchedTicks++;
      if (dirty[A.slot] == 0 && A.sleptAt < 0) {
        // Nothing marked the chunk during tick t, with the acid matched.
        A.sleptAt = (int)t;
        A.sleptStamp = VoxStamp(acid);
        A.alias = A.sleptStamp == stampFor(t, 0);
      }
    }
  }
  ctx.WaitIdle();
  rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DirtyActive(), 0,
                        dirty.data(), kNumSlots * 4, "stampSleepDirtyEnd");
  uint32_t eaten = 0, awakeEnd = 0, slept = 0, aliased = 0, stamped = 0;
  std::string rep;
  for (int a = 0; a < kArms; a++) {
    Arm& A = arms[a];
    ReadVoxelsSync(ctx, world, A.slot, 1, buf.data(), "stampSleepEnd");
    const bool ironGone = (wordAt(A, A.yIron) & 0xFFFu) != mIron;
    if (ironGone) eaten++;
    if (dirty[A.slot] != 0) awakeEnd++;
    if (A.restStamp != kStampNever) stamped++;
    if (A.sleptAt >= 0) {
      slept++;
      if (A.alias) aliased++;
      char line[160];
      std::snprintf(line, sizeof(line),
                    " [arm %d: chunk unmarked at t%d with acid on iron, acid "
                    "stamp %u, stampFor(t%d,0)=%u -> %s; iron %s]",
                    a, A.sleptAt, A.sleptStamp, A.sleptAt,
                    stampFor((uint32_t)A.sleptAt, 0), A.alias ? "ALIAS" : "other",
                    ironGone ? "eaten later" : "NEVER EATEN");
      rep += line;
    }
  }

  weather::SetOverride(prevPin);
  weather::Snap();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  const bool awakeOk = slept == 0;
  const bool eatsOk = eaten == (uint32_t)kArms;
  const bool sleepsOk = awakeEnd == 0;
  const bool stampedOk = stamped > 0;
  const bool ok = awakeOk && eatsOk && sleepsOk && stampedOk;
  char head[512];
  std::snprintf(head, sizeof(head),
                "%s: AWAKE %s (%u of %d arms' chunks went unmarked while "
                "matched, %u by stamp alias; %u matched arm-ticks watched over "
                "%u ticks) | EATS %s (iron eaten %u/%d by t%u) | SLEEPS %s (%u "
                "arm chunks awake at the end) | STAMPED %s (%u/%d arms rested "
                "with a live stamp)",
                ok ? "PASS" : "FAIL", awakeOk ? "ok" : "FAIL", slept, kArms,
                aliased, matchedTicks, kWatch, eatsOk ? "ok" : "FAIL", eaten,
                kArms, t, sleepsOk ? "ok" : "FAIL", awakeEnd,
                stampedOk ? "ok" : "FAIL", stamped, kArms);
  detail = std::string(head) + rep;
  std::printf("stamp-sleep: %s\n", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

const std::vector<Gate>& SimGates() {
  static const std::vector<Gate> g = {
      {"simd", "sim", {}, false, GateSimd},
      {"weak-flame", "sim", {}, false, GateWeakFlame},
      {"determinism", "sim", {}, false, GateDeterminism},
      {"ops-replay", "sim", {}, false, GateOpsReplay},
      {"chunk-hash", "sim", {}, false, GateChunkHash},
      {"chunk-resync", "sim", {}, false, GateChunkResync},
      {"sleep", "sim", {}, false, GateSleep},
      {"evaporation", "sim", {}, false, GateEvaporation},
      {"blood-stain", "sim", {}, false, GateBloodStain},
      {"flung-liquid", "sim", {}, false, GateFlungLiquid},
      {"fluid-det", "sim", {}, false, GateFluidDet},
      {"fluid-identity", "sim", {}, false, GateFluidIdentity},
      {"fluid-settle", "sim", {}, false, GateFluidSettle},
      {"fluid-excite", "sim", {}, false, GateFluidExcite},
      {"fluid-onwater", "sim", {}, false, GateFluidOnWater},
      {"debris-float", "sim", {}, false, GateDebrisFloat},
      {"fluid-stain", "sim", {}, false, GateFluidStain},
      {"fluid-react", "sim", {}, false, GateFluidReact},
      {"fluid-self-react", "sim", {}, false, GateFluidSelfReact},
      {"prefab", "sim", {}, false, GatePrefab},
      {"page-roundtrip", "sim", {}, false, GatePageRoundtrip},
      {"fire-down", "sim", {}, false, GateFireDown},
      {"rain-fire", "sim", {}, false, GateRainFire},
      {"stain-react", "sim", {}, false, GateStainReact},
      {"rain-stain", "sim", {}, false, GateRainStain},
      {"rain-lean", "sim", {}, false, GateRainLean},
      {"stamp-sleep", "sim", {}, false, GateStampSleep},
      {"daylight-boundary", "sim", {}, false, GateDaylightBoundary},
      {"support-flag", "sim", {}, false, GateSupportFlag},
      {"snapshot-latency", "sim", {}, false, GateSnapshotLatency},
      // No draw of its own, but its verdict reads bestFrameMs, which only the
      // screenshots gate sets — so it needs the render path transitively.
      {"perf", "sim", {"screenshots"}, true, GatePerf, /*needsRender=*/true},
  };
  return g;
}

}  // namespace selftest
