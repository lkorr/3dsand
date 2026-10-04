#pragma once
// THE CHARGE FIELD (docs/PLAN_electricity.md section 1, package E1; DESIGN.md
// "Electricity -- charge field").
//
// A TRANSIENT per-cell potential P (u16) over the conducting cells of the
// window, recomputed every CA-active tick by sim_elec.wgsl AFTER the CA:
//
//   P' = max(seed, max_nb6(P_nb - enter(cell, P_nb)), P - decay(P)), clamped >= 0
//   enter(cell, p) = resist(cell) + (p * spreadQ(n) >> 12)          (wave 2)
//   spreadQ(n) = max(0, n - sim.elecSpreadFree) * sim.elecSpreadLoss, < 4096
//   decay(P) = max(sim.elecDecay, P >> sim.elecDecayShift)
//
// n is the cell's count of CONDUCTING face neighbours. THE SPREADING LOSS
// (wave 2, 2026-10-04): a wire (n <= 2) costs only its resist, so copper still
// carries across the window; a cell in BULK (a pool, the sea, rain-wet ground,
// a plank wall) also loses a share of what it receives for every conducting
// neighbour past the free two -- the current divides there. Proportional, so
// lightning's 30,000 and a spark's 200 both stay local in bulk (reach ~ log P),
// where a pure per-cell cost let a strike into water run ~5,000 cells.
// Monotone in p (spreadQ < 4096), so the update is still max-plus-like: it only
// raises values, toward a least fixpoint, whatever the thread order.
//
// only in cells whose material conducts (materials.json "electric".resist, or
// a conducting coat on the cell -- the wet rule); an insulator holds P = 0
// unless it is itself a source. Max-plus relaxation: every update only RAISES
// a value toward the unique least fixpoint over its inputs, so the result does
// not depend on which thread got there first (rule 1). The decay is taken ONCE
// per tick, off every stored value, before the tick's rounds.
//
// Three buffers, addressed through the constants below. Each constant a shader
// reads is MIRRORED in that shader (sim_elec.wgsl; the accessor block also in
// sim_step.wgsl) and held to this file by scripts/check_invariants.py `elec`.
// Not in common.wgsl: only those shaders agree on the layout, and a
// common.wgsl edit misses the SPIR-V cache of every shader (CLAUDE.md).
//
//   elecPool    GPU-owned. kElecPoolPages pages of kElecPageWords, then the
//               per-tick CELL CACHE (kElecCacheBase, one kElecCacheWords block
//               per page) and its tick stamps (kElecStampBase). A page is
//               one window chunk's 4,096 cells as u16, two per word (the EVEN
//               chunk-local index in the low half), in TWO HALVES of
//               kElecHalfWords: half 0 is the settled field (what the CA and
//               every reader see, last tick's P), half 1 is round scratch.
//               Round r reads half (r & 1) and writes the other (Jacobi between
//               rounds: a chunk's halo is its neighbours' previous round), and
//               elecSettle copies the last round back into half 0 and zeroes
//               half 1. A page on the free stack is ALL ZERO, always (the
//               cache and stamp are not: a stamp != this tick means "rebuild").
//   elecMeta    GPU-owned. Header (list cursor, free stack, phase, stats,
//               indirect args), then per WINDOW slot the entry (HAS | page) and
//               owner chunk, the want bitset, the two live lists and the free
//               stack. All zero = an empty field, which is what the worldgen
//               and load-reset fill rows leave.
//   elecParams  CPU-written (Simulation::PrepareElec / UploadTables): the knobs
//               (mode, rounds, decay, the wet resist table) and four words per
//               material (resist | source, and E2's ignite / char words).
//
// THE TICK (pass_table.def "elec"), every CA-active tick after the heat rows:
//   elecAlloc   ONE workgroup, phase 0 (the head): re-key pages whose window
//               slot changed owner, page the chunks caMask's doorbell wanted
//               (a source material in a dirty chunk), in SLOT order
//   elecRound   x sim.elecRounds: one group per live chunk; fixpoint in shared
//               memory by axis sweeps, the halo from the neighbours' previous
//               round; a face whose charge can enter an unpaged conductor
//               wants that chunk -- elecAlloc pages it between rounds, so the
//               field crosses sim.elecRounds chunks a tick
//   elecSettle  the last round into half 0; a chunk with charge goes on the
//               next tick's list and is marked dirty (DIRTY_R_ELEC) when a chunk
//               of its 3x3x3 is dirty this tick (the CPU page-table mirror's
//               one-ring bound); an all-zero page goes back on the stack
//   elecAlloc   phase rounds (the tail): page the last round's wants onto the
//               next list, flip the lists -- unless NO chunk could be marked,
//               in which case the whole field is PURGED (elecPurge): charge the
//               CA cannot keep awake would otherwise evolve on however many
//               ticks the CPU takes to prove the world settled, which is a
//               readback-timing outcome (rule 1).
//
// Not hashed, not saved, not replicated: a pure function of the voxels and the
// tick inputs, reproduced by replaying them. Worldgen and load zero it.
#include <cstdint>
#include <string>
#include <vector>

// ---- geometry ---------------------------------------------------------------
// Restated so materials.h can include this header; elec.cpp static_asserts
// them against world.h (as heat.h does).
constexpr uint32_t kElecChunk = 16;
constexpr uint32_t kElecWindowChunks = 32768;   // world.h kNumChunks
constexpr uint32_t kElecChunkVol = kElecChunk * kElecChunk * kElecChunk;   // 4,096
constexpr uint32_t kElecHalfWords = kElecChunkVol / 2;      // u16 cells, two a word
constexpr uint32_t kElecPageWords = 2 * kElecHalfWords;     // 4,096 words = 16 KiB
// 2,048 pages = 32 MiB. A copper wire is one page per chunk it crosses; a
// charged lake one per chunk of water within reach (water resist 6: a spark's
// 200 reaches 33 cells, three chunks). A refused page is COUNTED
// (kEmRefused, last_run.json elec.refused) and the chunk stays an uncharged
// gap until the next round asks again -- never an abort.
constexpr uint32_t kElecPoolPages = 2048;
// THE CELL CACHE (wave 2): one u16 per cell of a page's chunk, two per word
// (the even cell low), built by the chunk's FIRST round of a tick and read by
// every later one -- the voxels do not change between the elec rows, so the
// cache is exact, and rounds 1.. skip the voxel / material / stain / solute
// reads and the face-neighbour scan. Per cell: bits 0..11 resist
// (kElecResistInsulator = none), 12..14 conducting face neighbours (0..6),
// bit 15 the cell is a SOURCE (its seed is re-read from its voxel). The stamp
// is the tick + 1 the block was built on (0 never matches a live tick + 1).
// Indexed by PAGE, like the page. 16 MiB beside the pool's 32.
constexpr uint32_t kElecCacheWords = kElecHalfWords;
constexpr uint32_t kElecCacheBase = kElecPoolPages * kElecPageWords;
constexpr uint32_t kElecStampBase = kElecCacheBase + kElecPoolPages * kElecCacheWords;
constexpr uint32_t kElecPoolWords = kElecStampBase + kElecPoolPages;
// The most rounds a tick can run (sim.elecRounds clamps to it): the pass table
// carries one alloc / copy / round triple per round past the first, each under
// its own condition (pass_table.h Cond::ElecR1..ElecR7).
constexpr uint32_t kElecRoundsMax = 8;
// Sweep iterations a round may spend reaching its chunk's fixpoint: one
// iteration is an x, a y and a z sweep (each line forward and back), so a
// straight or L-shaped conductor converges in one or two. A serpentine longer
// than the cap simply carries on next round -- every iteration is a pure
// function of the round's inputs, so the cap is deterministic.
constexpr uint32_t kElecIterCap = 12;
// The cell's resist as the kernel sees it: 1..4094, kElecResistInsulator = no
// conduction (the authored 0 / no "electric" block). TWELVE bits since wave 2
// (2026-10-04; it was 8, 1..254): resistivity spans orders of magnitude, and a
// byte could not say "dry wood is nearly an insulator" -- at 60 one bolt ran
// ~500 cells of connected timber; the widest a byte allowed (254) still ~118.
constexpr uint32_t kElecResistInsulator = 4095;
constexpr uint32_t kElecResistMask = 0xFFFu;
constexpr uint32_t kElecPMax = 65535;

// ---- elecMeta ---------------------------------------------------------------
constexpr uint32_t kEmHdrWords = 64;
constexpr uint32_t kEmCur = 0;         // which list (0 / 1) is THIS tick's live list
constexpr uint32_t kEmFreeTop = 1;     // pages on the free stack
constexpr uint32_t kEmNextFresh = 2;   // pages never handed out start here
constexpr uint32_t kEmPhase = 3;       // elecAlloc calls this tick (round index + 1)
constexpr uint32_t kEmMarked = 4;      // chunks elecSettle marked dirty this tick
constexpr uint32_t kEmCount0 = 5;      // list 0's count
constexpr uint32_t kEmCount1 = 6;      // list 1's count
// Monotonic: chunks caMask's doorbell asked a page for (wave 2). The CPU reads
// it off the snapshot (World::ElecMayBeLive): a ring it has not seen means a
// source is standing in the window, so the elec rows must run (the fast path).
constexpr uint32_t kEmDoorbells = 7;
// Stats, words 16..31 (the snapshot a gate / last_run.json reads).
constexpr uint32_t kEmPagesPeak = 16;
constexpr uint32_t kEmRefused = 17;    // monotonic: wanted chunks the pool could not page
constexpr uint32_t kEmAllocs = 18;
constexpr uint32_t kEmFrees = 19;
constexpr uint32_t kEmPurges = 20;     // ticks whose field was purged (nothing markable)
constexpr uint32_t kEmStranded = 21;   // monotonic: charged chunk-ticks that could not be marked
constexpr uint32_t kEmLivePeak = 22;   // most chunks one tick's rounds ran over
constexpr uint32_t kEmRekeyed = 23;    // pages zeroed because their slot changed owner
constexpr uint32_t kEmPPeak = 24;      // the highest P any round wrote, monotonic
constexpr uint32_t kEmRoundChunks = 25;  // chunk-rounds run, monotonic (the cost)
// ATTRIBUTION (diagnostic, never read by the sim): the highest P any round
// wrote into a CONDUCTING cell (as opposed to kEmPPeak, which a lone source
// sets on its own), and chunk-rounds run per round index 0..3 (index 3 counts
// every round past the third) -- "seeds seen, nothing conducted" and "only
// round 0 ran" are different bugs, and a bare P peak cannot tell them apart.
constexpr uint32_t kEmCondPeak = 26;
constexpr uint32_t kEmRoundHist = 27;    // 27..30
constexpr uint32_t kEmSnapWords = 32;
// Indirect args: two 16-byte records, copied to elecArgs by copy_elecArgs.
//   record 0: one group per live-list chunk (elecRound, elecSettle)
//   record 1: one group per chunk to purge (elecPurge)
constexpr uint32_t kEmArgs = 32;
constexpr uint32_t kElecArgRound = 0, kElecArgPurge = 1, kElecArgRecords = 2;
constexpr uint32_t kElecArgsBytes = kElecArgRecords * 16;
static_assert(kEmArgs * 4 == 128, "pass_table.def copy_elecArgs reads byte 128");
static_assert(kElecArgsBytes == 32, "pass_table.def copy_elecArgs copies 32 bytes");
static_assert(kEmArgs + kElecArgRecords * 4 <= kEmHdrWords, "the args fit the header");

// ---- THE BODY QUERY (package E4: shocks reach bodies; wave 2 package B) ----
// Up to kElecQueryMax world-cell BOXES a tick, ONE PER BODY (the union of the
// body's limb, shell and held-item boxes under its current pose:
// MobSystem::QueueShockQueries), each answered by one workgroup of
// sim_elec.wgsl elecQuery with the settled field's max P over the box, the
// cells with P > 0, their P sum and the cells scanned -- and, since wave 2, a
// GRID: the box's every cell as a u16 (P clamped to kElecQueryGridPMax, bit
// kElecQueryGridAir = the cell is air), two cells a word, x fastest, at the
// word offset the CPU gave the box (box word 6; kElecQueryNoGrid = none). The
// CPU conducts the charge THROUGH the body from that grid
// (mob_shock.cpp: every body cell takes its material's resist, the world's
// max-plus rule). The boxes ride elecParams' tail (CPU-written,
// Simulation::PrepareElecQueries), the answers and grids elecMeta's tail
// (GPU-written), and both reach the CPU on the snapshot ring at
// World::kSnapshotLatency, exactly like every other gameplay readback: a
// decision at tick T reads the boxes of tick T - K - 1, never "whenever the
// copy landed". A box is inclusive and at most kElecQueryAxisMax cells along
// each axis (the CPU clamps it).
//
// SIZED FROM THE CREATURE CAP. kElecQueryMax holds every living creature
// (world.h kMaxLiveMobs = MobSystem::kMaxMobs, restated here as
// kElecLiveMobs) plus the players' bodies; elec.cpp static_asserts the
// restatement and mob_shock.cpp the sum, so raising the cap without this
// fails the build instead of silently refusing the last bodies (the wave-1 query asked one box per LIMB and stopped at ~8
// bodies). The grid budget, kElecQueryGridWordsMax, holds kElecQueryMax
// human-sized boxes (a human's box is ~7 x 19 x 7 cells, ~470 words); a box
// past the budget is refused and counted (World::ElecQueryCounters), the
// players' never (they queue first).
// world.h kMaxLiveMobs, restated (materials.h includes this header, so it
// cannot include world.h) and static_asserted against it in elec.cpp.
constexpr uint32_t kElecLiveMobs = 64;
constexpr uint32_t kElecQueryPlayerReserve = 16;
constexpr uint32_t kElecQueryMax = kElecLiveMobs + kElecQueryPlayerReserve;   // 80
constexpr uint32_t kElecQueryBoxWords = 8;   // lo.xyz, hi.xyz (i32, inclusive), grid word offset, 0
constexpr uint32_t kElecQueryResWords = 4;   // maxP, charged cells, sum P, cells scanned
constexpr uint32_t kElecQueryAxisMax = 32;
constexpr uint32_t kElecQueryResBytes = kElecQueryMax * kElecQueryResWords * 4;
constexpr uint32_t kElecQueryGridWordsMax = 49152;   // 192 KiB a tick, at most
constexpr uint32_t kElecQueryGridBytes = kElecQueryGridWordsMax * 4;
constexpr uint32_t kElecQueryGridPMax = 0x7FFFu;
constexpr uint32_t kElecQueryGridAir = 0x8000u;
constexpr uint32_t kElecQueryNoGrid = 0xFFFFFFFFu;

constexpr uint32_t kEmEntry = kEmHdrWords;                       // HAS | page
constexpr uint32_t kEmOwner = kEmEntry + kElecWindowChunks;       // packed world chunk
constexpr uint32_t kEmWant = kEmOwner + kElecWindowChunks;        // bitset
constexpr uint32_t kEmList0 = kEmWant + kElecWindowChunks / 32;
constexpr uint32_t kEmList1 = kEmList0 + kElecPoolPages;
constexpr uint32_t kEmStack = kEmList1 + kElecPoolPages;
// The body query's answers, kElecQueryResWords per box (elecQuery writes them;
// the snapshot ring copies the first `count` boxes' worth on a tick that ran).
constexpr uint32_t kEmQuery = kEmStack + kElecPoolPages;
// ...and their grids (kElecQueryGridWordsMax words; each box's at its offset).
constexpr uint32_t kEmQueryGrid = kEmQuery + kElecQueryMax * kElecQueryResWords;
constexpr uint32_t kEmWords = kEmQueryGrid + kElecQueryGridWordsMax;

constexpr uint32_t kElecEntryHas = 0x80000000u;
constexpr uint32_t kElecEntryPage = 0x00FFFFFFu;
static_assert(kElecPoolPages <= kElecEntryPage, "page index must fit the entry word");

// ---- elecParams -------------------------------------------------------------
constexpr uint32_t kEpHdrWords = 32;
constexpr uint32_t kEpMode = 0;        // sim.elecMode
constexpr uint32_t kEpRounds = 1;      // sim.elecRounds (1..kElecRoundsMax; 1 when off)
constexpr uint32_t kEpDecay = 2;       // sim.elecDecay
constexpr uint32_t kEpIterCap = 3;     // kElecIterCap
// E2 (sim_step.wgsl, what charge does in the CA), from the sim.elec* knobs and
// the material table (Simulation::PrepareElec / UploadTables):
constexpr uint32_t kEpReactMin = 4;        // P that makes a cell a virtual tag:electric neighbour
constexpr uint32_t kEpReactFull = 5;       // P at which such a rule fires at full chance
constexpr uint32_t kEpIgniteQ = 6;         // sim.elecIgniteGain, x32 fixed point (chance = E * q >> 4)
constexpr uint32_t kEpCrackleQ = 7;        // sim.elecCrackle, the same scaling
constexpr uint32_t kEpCrackleLoP = 8;      // P threshold of the low crackle tier (kElecPOff = never)
constexpr uint32_t kEpCrackleLoMat = 9;    // what it emits (`spark`)
constexpr uint32_t kEpCrackleHiP = 10;     // the high tier (`arc`)
constexpr uint32_t kEpCrackleHiMat = 11;
constexpr uint32_t kEpElecTag = 12;        // the tagMask bit(s) of "electric" (0 = no such tag)
// The proportional decay (sim.elecDecayShift; 0 = linear kEpDecay only): a
// stored P loses max(kEpDecay, P >> shift) in round 0 of a tick.
constexpr uint32_t kEpDecayShift = 13;
// THE SPREADING LOSS (wave 2; sim.elecSpreadLoss / sim.elecSpreadFree): the
// share of the potential a cell loses on entry, in 1/4096, per conducting face
// neighbour past the free count -- see the header comment.
constexpr uint32_t kEpSpreadQ = 14;
constexpr uint32_t kEpSpreadFree = 15;
// A threshold no P reaches (P is a u16).
constexpr uint32_t kElecPOff = kElecPMax + 1;
// The wet table: the resist of a cell under a CONDUCTING COAT of stain amount
// a (1..15), from sim.elecWetResist (the resist of a full coat):
// min(254, ceil(wetResist * 15 / a)). Word 0 (amount 0) is the insulator.
constexpr uint32_t kEpWet = 16;        // 16 words, 16..31
constexpr uint32_t kEpMat = kEpHdrWords;   // 4 words per material
constexpr uint32_t kEpMatStride = 4;
constexpr uint32_t kElecMatMax = 4096;
// The body query's boxes (see kElecQueryMax): word 0 the box count, words
// 1..3 zero, then kElecQueryBoxWords per box.
constexpr uint32_t kEpQuery = kEpMat + kEpMatStride * kElecMatMax;
constexpr uint32_t kEpQueryBoxes = kEpQuery + 4;
constexpr uint32_t kEpWords = kEpQueryBoxes + kElecQueryMax * kElecQueryBoxWords;

// One material = four words at kEpMat + mat * 4:
//   w0  resist (bits 0..11; 0 = insulator) | source (bits 16..31)
//   w1  E2: ignite product (bits 0..11) | char product (bits 12..23)
//   w2  E2: ignite chance, in 1/kReactChanceDen per tick (heat's scaling)
//   w3  wave 2: DISSOLVED resist (bits 0..11; 0 = not an electrolyte) -- the
//       resist a conducting liquid falls to when it carries this material
//       dissolved at its species' saturation (solutes.json `from`); salt makes
//       brine. Read by the shared resist rule (MIRROR elec, elecResistRaw).
// MaterialGpu._r2 (heat.h owns bits 0..24): bit 25 = the material is a SOURCE
// (caMask's doorbell, one test on the Material the CA already holds), bit 26 =
// it CONDUCTS (E2's quick "can this cell carry charge" test).
constexpr uint32_t kElecR2Source = 1u << 25;
constexpr uint32_t kElecR2Conducts = 1u << 26;

// ---- the authored data (materials.json "electric") --------------------------
//   "electric": { "resist": N, "source": E,
//                 "ignite": { "into": "<mat>", "chance": per-mille },
//                 "char": "<mat>", "dissolved": R, "shock": per-mille }
// No block = an insulator. resist 1..4094 is the cost per cell entered; source
// 1..65535 is the potential the cell holds every tick it exists; dissolved
// 1..4094 is what a conducting liquid's resist falls to with this material
// dissolved in it at saturation (linear in concentration); shock (bodies only,
// wave 2 package B) is the per-mille of a body cell's P its creature feels.
struct ElecDef {
  uint32_t resist = 0;               // 0 = insulator
  uint32_t source = 0;
  uint32_t dissolved = 0;            // 0 = not an electrolyte
  std::string igniteInto, charInto;  // names, resolved after the table loads
  uint32_t igniteIntoId = 0, charIntoId = 0;
  double igniteChanceMille = 0.0;
  // BODIES ONLY (wave 2 package B, mob_shock.cpp): how much of a body cell's
  // P a creature made of this material FEELS -- the hp and the stun of a
  // shock. 1000 for tissue with nerves and muscle (skin, flesh, brain) and for
  // the circuitry an android runs on; 0 (the default) for matter that only
  // carries the current (bone, blood, wood, metal). The world field never
  // reads it.
  uint32_t shockMille = 0;
  bool Any() const { return resist != 0 || source != 0; }
};

// Packs one material's four GPU words. Pure; used by Simulation::UploadTables
// and the gate.
void PackElecMaterial(const ElecDef& e, uint32_t out[kEpMatStride]);
// The wet table entry for stain amount a (0..15) from sim.elecWetResist.
uint32_t ElecWetResist(uint32_t wetResist, uint32_t amount);
// The tagMask bit of the tag "electric": the bits every material carrying the
// tag has and no other material has (tags are assigned one bit each, so this
// is exactly that bit). 0 when no material carries it.
struct MaterialDef;
uint32_t ElecTagMask(const std::vector<MaterialDef>& mats);
// A crackle tier's threshold as uploaded: the knob, raised to the emitted
// material's own source + 1 (so a crackle cannot re-charge its emitter past
// the threshold that emits it), or kElecPOff when there is no such material.
uint32_t ElecCrackleThreshold(int knob, uint32_t emitMat, uint32_t emitSource);

// What build/last_run.json's `elec` block reports: peaks and totals over every
// elecMeta header a gate handed ElecNoteRun.
struct ElecRunStats {
  uint32_t runs = 0, pagesPeak = 0, refused = 0, purges = 0, stranded = 0, livePeak = 0;
  uint32_t pPeak = 0;
};
void ElecNoteRun(const uint32_t* header);   // elecMeta words 0..kEmSnapWords
const ElecRunStats& ElecRunTotals();
// ---- the body query, CPU side (World::QueueElecQuery, WorldSnapshot) --------
// One box a body asked about: world cells, inclusive, and WHO asked (the
// creature id and its rig slot) -- the tag stays on the CPU, in the readback
// slot that carries the box's answer, so the GPU never sees an id.
struct ElecQuery {
  int32_t lo[3] = {0, 0, 0}, hi[3] = {-1, -1, -1};
  uint64_t mobId = 0;
  int32_t limb = -1;   // -1: the whole body (wave 2); >= 0 a rig slot
  // The box's grid: its word offset in the grid area (World::QueueElecQuery
  // assigns it, kElecQueryNoGrid = none) and its word count.
  uint32_t gridOff = kElecQueryNoGrid, gridWords = 0;
};
// One answer, as the publish hands it over: the query's tag, the tick whose
// field it read, and kElecQueryResWords of result.
struct ElecHit {
  uint64_t mobId = 0;
  int32_t limb = -1;
  uint32_t tick = 0;      // the tick the box was asked (and the field read) on
  uint32_t maxP = 0;      // the highest settled P in the box
  uint32_t charged = 0;   // cells with P > 0
  uint32_t sumP = 0;      // their P, summed
  uint32_t cells = 0;     // cells scanned (the box's volume inside the window)
  // The box and its grid (empty when the box had none): cell (x, y, z) of the
  // box is grid[(z * dims[1] + y) * dims[0] + x], kElecQueryGridPMax | air.
  int32_t lo[3] = {0, 0, 0}, dims[3] = {0, 0, 0};
  std::vector<uint16_t> grid;
};
// The box's dims (clamped like the kernel) and grid words.
inline void ElecQueryDims(const ElecQuery& q, int32_t dims[3]) {
  for (int a = 0; a < 3; a++) {
    const int32_t d = q.hi[a] - q.lo[a] + 1;
    dims[a] = d < 0 ? 0 : (d > (int32_t)kElecQueryAxisMax ? (int32_t)kElecQueryAxisMax : d);
  }
}
inline uint32_t ElecQueryGridWords(const ElecQuery& q) {
  int32_t d[3];
  ElecQueryDims(q, d);
  return ((uint32_t)d[0] * (uint32_t)d[1] * (uint32_t)d[2] + 1u) / 2u;
}

// Packed world-chunk key for elecMeta's owner word (10 bits an axis, +1 so a
// zero word is never a valid owner).
inline uint32_t ElecOwnerKey(int x, int y, int z) {
  return (((uint32_t)x & 1023u) | (((uint32_t)y & 1023u) << 10) |
          (((uint32_t)z & 1023u) << 20)) + 1u;
}
