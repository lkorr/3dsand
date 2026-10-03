#pragma once
// THE CHARGE FIELD (docs/PLAN_electricity.md section 1, package E1; DESIGN.md
// "Electricity -- charge field").
//
// A TRANSIENT per-cell potential P (u16) over the conducting cells of the
// window, recomputed every CA-active tick by sim_elec.wgsl AFTER the CA:
//
//   P' = max(seed, max_nb6(P_nb - resist(cell)), P - decay), clamped >= 0
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
//   elecPool    GPU-owned. kElecPoolPages pages of kElecPageWords. A page is
//               one window chunk's 4,096 cells as u16, two per word (the EVEN
//               chunk-local index in the low half), in TWO HALVES of
//               kElecHalfWords: half 0 is the settled field (what the CA and
//               every reader see, last tick's P), half 1 is round scratch.
//               Round r reads half (r & 1) and writes the other (Jacobi between
//               rounds: a chunk's halo is its neighbours' previous round), and
//               elecSettle copies the last round back into half 0 and zeroes
//               half 1. A page on the free stack is ALL ZERO, always.
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
// The cell's resist as the kernel sees it: 1..254, kElecResistInsulator = no
// conduction (the authored 0 / no "electric" block).
constexpr uint32_t kElecResistInsulator = 255;
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

constexpr uint32_t kEmEntry = kEmHdrWords;                       // HAS | page
constexpr uint32_t kEmOwner = kEmEntry + kElecWindowChunks;       // packed world chunk
constexpr uint32_t kEmWant = kEmOwner + kElecWindowChunks;        // bitset
constexpr uint32_t kEmList0 = kEmWant + kElecWindowChunks / 32;
constexpr uint32_t kEmList1 = kEmList0 + kElecPoolPages;
constexpr uint32_t kEmStack = kEmList1 + kElecPoolPages;
constexpr uint32_t kEmWords = kEmStack + kElecPoolPages;

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
// A threshold no P reaches (P is a u16).
constexpr uint32_t kElecPOff = kElecPMax + 1;
// The wet table: the resist of a cell under a CONDUCTING COAT of stain amount
// a (1..15), from sim.elecWetResist (the resist of a full coat):
// min(254, ceil(wetResist * 15 / a)). Word 0 (amount 0) is the insulator.
constexpr uint32_t kEpWet = 16;        // 16 words, 16..31
constexpr uint32_t kEpMat = kEpHdrWords;   // 4 words per material
constexpr uint32_t kEpMatStride = 4;
constexpr uint32_t kElecMatMax = 4096;
constexpr uint32_t kEpWords = kEpMat + kEpMatStride * kElecMatMax;

// One material = four words at kEpMat + mat * 4:
//   w0  resist (bits 0..7; 0 = insulator) | source (bits 16..31)
//   w1  E2: ignite product (bits 0..11) | char product (bits 12..23)
//   w2  E2: ignite chance, in 1/kReactChanceDen per tick (heat's scaling)
//   w3  reserved (0)
// MaterialGpu._r2 (heat.h owns bits 0..24): bit 25 = the material is a SOURCE
// (caMask's doorbell, one test on the Material the CA already holds), bit 26 =
// it CONDUCTS (E2's quick "can this cell carry charge" test).
constexpr uint32_t kElecR2Source = 1u << 25;
constexpr uint32_t kElecR2Conducts = 1u << 26;

// ---- the authored data (materials.json "electric") --------------------------
//   "electric": { "resist": N, "source": E,
//                 "ignite": { "into": "<mat>", "chance": per-mille },
//                 "char": "<mat>" }
// No block = an insulator. resist 1..254 is the cost per cell entered; source
// 1..65535 is the potential the cell holds every tick it exists.
struct ElecDef {
  uint32_t resist = 0;               // 0 = insulator
  uint32_t source = 0;
  std::string igniteInto, charInto;  // names, resolved after the table loads
  uint32_t igniteIntoId = 0, charIntoId = 0;
  double igniteChanceMille = 0.0;
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
// Packed world-chunk key for elecMeta's owner word (10 bits an axis, +1 so a
// zero word is never a valid owner).
inline uint32_t ElecOwnerKey(int x, int y, int z) {
  return (((uint32_t)x & 1023u) | (((uint32_t)y & 1023u) << 10) |
          (((uint32_t)z & 1023u) << 20)) + 1u;
}
