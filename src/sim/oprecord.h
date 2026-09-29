// oprecord.h — the MutationQueue as a RECORD, and the hygiene that makes a
// record replayable (docs/PLAN_multiplayer_now.md package N3).
//
// CLAUDE.md rule 3 has always said the op stream "is also save format, replay
// log, and future network stream". Until this file, none of those three
// existed: the queue was six per-tick std::vectors that were uploaded and
// forgotten, the ops carried no author and no sequence, and two ops landing on
// one cell in one tick had no defined winner at all — sim_mutate.wgsl's two
// entry points were plain voxStores and whichever invocation the driver ran
// last owned the cell.
//
// This module supplies the four missing pieces:
//
//   1. AUTHOR (CPU-side only). Who emitted an op — an entity id plus a
//      producer enum. NOT uploaded: the GPU does not need it and the voxel
//      word has no room for it. The record needs it and the network layer will
//      (an op stream with no author cannot be validated by a host).
//   2. THE RECORD. One framed record per tick holding the whole tick INPUT:
//      the scalar arguments SubmitTick was called with, the entire TickParams
//      struct (many sim.* knobs ride it per tick precisely "so a replay
//      reproduces the stream" — world.h), all six op vectors, the gen list the
//      streamer dispatched, and the OpMeta side table.
//   3. CANONICALIZATION. Cell ops are deduped CPU-side, keep-FIRST in push
//      order, before upload. Brush ops dedupe shader-side (sim_mutate.wgsl).
//   4. CLAMPS. Every one of the six streams is clamped to its cap at the ONE
//      choke point and every truncation is COUNTED, so "the op you pushed did
//      not happen" is a number in build/last_run.json instead of silence.
//
// WHAT A RECORD IS NOT. It is not a save: saves are still chunk snapshots
// (sim/worldio.cpp), and a record replays only from the worldgen seed its
// header names. It is not compressed and not a wire format — the framing is
// raw little-endian PODs, same as sim/bytestream.h's other users, and the
// header refuses a mismatched build the way worldio's meta.svm does.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sim/materials.h"
#include "sim/tickinput.h"
#include "sim/world.h"

// ReplaceChunksIfReplaying hands a recorded chunk-replace back to the ONE
// function allowed to install it (Stream::ReplaceChunk, sim/stream.h). By name
// only: stream.cpp already includes THIS header for NoteGenList, so including
// stream.h here would be a cycle. oprecord.cpp includes the real one.
class Stream;

namespace sandvox {
namespace opstream {

// ---- 1. AUTHOR ------------------------------------------------------------

// WHICH SYSTEM emitted an op. Not a material, not a mode — those describe what
// the op does; this describes who asked. A host validating a client's stream
// needs both ("this client may paint, it may not detonate").
enum class Producer : uint8_t {
  Unknown = 0,
  Brush,      // the held tool (game/brush.cpp)
  Spell,      // game/spell.cpp emissions
  Mob,        // game/mob.cpp — bleed, burn, limb carve
  Avatar,     // game/avatar.cpp — the player's own body
  Debris,     // phys/debris.cpp reinsertion
  EditLayer,  // sim/worldedit.cpp authored layers
  Worldgen,   // stream refill patch-ups
  Lab,        // harness / gate fixtures
  // An op that arrived from a PEER and was replayed here (M9.3-B's merge).
  // It is its own producer and not the producer the sender used, because the
  // question the receiver asks about it is different: the sender's ownership
  // was already checked on the sender, and the ONE thing the receiver must
  // never do is re-emit it as if it were local (net::Owns returns false for
  // Remote, and that is the whole point of the value existing).
  //
  // APPENDED, never inserted: every earlier enumerator keeps its numeric
  // value, so a record written before this line still decodes to the same
  // producers and OpMeta's layout is untouched (it stores a uint8_t).
  Remote,
  Count
};
const char* ProducerName(uint8_t p);

// The side table entry. 8 bytes, CPU-side only, never uploaded.
struct OpMeta {
  uint32_t author = 0;   // entity id; 0 = the local player
  uint8_t producer = 0;  // Producer
  uint8_t pad0 = 0, pad1 = 0, pad2 = 0;
};
static_assert(sizeof(OpMeta) == 8, "OpMeta is a record field");

// A RANGE SCOPE rather than an extra argument on every producer signature.
//
// The alternative — a parallel std::vector<OpMeta>& threaded beside every
// std::vector<BrushOp>& — widens MobSystem::PreTick, Mob::BleedTick,
// PlayerAvatar::ApplyFallDamage, SpellSystem::Tick and their headers, and a
// producer that pushed an op and forgot the meta would silently desynchronize
// the two vectors. A range scope cannot desynchronize: it reads the vector's
// size when it is built and again when it dies, and claims exactly the ops
// that appeared in between. One RAII line per producer, no signature changes,
// and a producer that has not adopted it reports Unknown — which is honest,
// and distinguishable from "there were no ops".
//
// AN AMBIENT "current author" WOULD NOT WORK, and it is worth saying why:
// producers run early in the tick body and SubmitTick runs at the end, so an
// ambient value has already been unwound by the time the recorder reads it.
// The author has to be attached to the OPS, and the ops are in a vector, so
// the attachment is a range in that vector.
//
// Deterministic by construction: ranges are recorded on the CPU in the tick
// body's fixed sequence, are consumed and cleared once per tick, and never
// reach a shader — nothing here can move the world hash.
enum class Stream : uint8_t { Brush = 0, Explosion = 1, Count };
void NoteAuthorRange(Stream s, uint32_t begin, uint32_t end, Producer p,
                     uint32_t author);
// Expand this tick's ranges over `count` ops. Later (inner) ranges win, so a
// spell emitting through a mob's helper is attributed to the spell.
std::vector<OpMeta> ResolveAuthors(Stream s, uint32_t count);
void ClearAuthorRanges();

struct BrushAuthorScope {
  BrushAuthorScope(const std::vector<BrushOp>& v, Producer p,
                   uint32_t author = 0)
      : v_(v), begin_((uint32_t)v.size()), p_(p), a_(author) {}
  ~BrushAuthorScope() {
    NoteAuthorRange(Stream::Brush, begin_, (uint32_t)v_.size(), p_, a_);
  }
  BrushAuthorScope(const BrushAuthorScope&) = delete;
  BrushAuthorScope& operator=(const BrushAuthorScope&) = delete;

 private:
  const std::vector<BrushOp>& v_;
  uint32_t begin_;
  Producer p_;
  uint32_t a_;
};

struct ExpAuthorScope {
  ExpAuthorScope(const std::vector<ExplosionOp>& v, Producer p,
                 uint32_t author = 0)
      : v_(v), begin_((uint32_t)v.size()), p_(p), a_(author) {}
  ~ExpAuthorScope() {
    NoteAuthorRange(Stream::Explosion, begin_, (uint32_t)v_.size(), p_, a_);
  }
  ExpAuthorScope(const ExpAuthorScope&) = delete;
  ExpAuthorScope& operator=(const ExpAuthorScope&) = delete;

 private:
  const std::vector<ExplosionOp>& v_;
  uint32_t begin_;
  Producer p_;
  uint32_t a_;
};

// ---- 3. CANONICALIZATION --------------------------------------------------

// Deduplicate `in` by cellIdx, KEEPING THE FIRST op in push order, and leaving
// the survivors in push order.
//
// WHY KEEP-FIRST AND WHY PUSH ORDER. CPU push order is already deterministic
// (a fixed sequence in the tick body; the mob burn rotates by a tick-derived
// start; WorldEdits::Drain walks an ordered vector) — the shader simply did
// not honour it, because `cells` is one dispatch and two invocations writing
// one cell race. Priority therefore goes to the op the CPU emitted first, and
// the survivors keep their positions so a tick with NO duplicates uploads a
// byte-identical buffer. That is what makes this change hash-neutral except
// exactly where a latent race existed.
//
// Returns the number of ops DROPPED. Zero means `in` was already canonical and
// `out` is untouched — the caller keeps using `in`, so the common path
// allocates nothing. `firstDupeCell` (optional) gets one contested cell index,
// so a report can name the cell instead of printing a bare count.
uint32_t CanonicalizeCells(const std::vector<CellOp>& in,
                           std::vector<CellOp>& out,
                           uint32_t* firstDupeCell = nullptr);

// ---- 4. CLAMPS, COUNTED ---------------------------------------------------

// Ops REFUSED at the choke point this run, per stream, plus the cell ops
// dropped as duplicates. A bare "it did not happen" is the failure mode
// CLAUDE.md rule 6 is about; these are surfaced in build/last_run.json.
struct StreamCounts {
  uint32_t brushTrunc = 0;
  uint32_t expTrunc = 0;
  uint32_t cellTrunc = 0;
  uint32_t spawnTrunc = 0;
  uint32_t fluidTrunc = 0;
  uint32_t gasTrunc = 0;
  // Solute pour ops (world.h CellOpSolute) refused at the choke point: past
  // kMaxSolutePourOpsPerTick, or squeezed out of kMaxCellOpsPerTick. The
  // ops, and the solute UNITS they carried -- dissolved mass a vessel was
  // already debited for, so the units are the loss.
  uint32_t solPourTrunc = 0;
  uint32_t solPourUnitsTrunc = 0;
  uint32_t cellDupes = 0;  // dropped by CanonicalizeCells
  uint32_t ticksWithDupes = 0;
  uint32_t firstDupeCell = 0xFFFFFFFFu;  // a cell that lost a duplicate
  uint32_t firstDupeTick = 0;
  // ---- M9.4-A: the SINGLE-PRODUCER ledger --------------------------------
  //
  // Counted only on a CONNECTED run (net::Hook() is null otherwise), so every
  // gate, both smokes and single-player leave these at zero and nothing about
  // the uploaded stream changes.
  //
  // TWO counters, not one, because they are two different bugs:
  //   authorityViolations  a system emitted a world-authored op into a chunk
  //                        another machine owns — a DUPLICATED op, the thing
  //                        §4 finding 4's single-producer rule exists to stop.
  //   unknownProducers     an op carried Producer::Unknown, i.e. some producer
  //                        never adopted an author scope. Not a trespass; a
  //                        missing annotation, and fixable in a different file.
  // Folding them together would make "we shipped a new emitter without a
  // scope" indistinguishable from "we double-stepped a chunk".
  uint32_t authorityViolations = 0;
  uint32_t unknownProducers = 0;
  // ATTRIBUTION FOR THE FIRST VIOLATION (CLAUDE.md rule 6: a bare count is not
  // a measurement). Producer, world chunk, and who DID own it — enough to name
  // the emitter and the boundary on one line without a second run.
  // firstViolProducer == Producer::Count means "a CellOp", which carries no
  // OpMeta and so has no producer to name.
  uint32_t firstViolProducer = 0xFFFFFFFFu;
  uint32_t firstViolOwner = 0xFFFFFFFFu;  // net::kNoAuthority = nobody
  uint32_t firstViolTick = 0;
  int32_t firstViolChunk[3] = {0, 0, 0};
  uint32_t Total() const {
    return brushTrunc + expTrunc + cellTrunc + spawnTrunc + fluidTrunc +
           gasTrunc + solPourTrunc;
  }
};
const StreamCounts& Counts();
void ResetCounts();
void NoteTruncation(uint32_t brush, uint32_t exp, uint32_t cell, uint32_t spawn,
                    uint32_t fluid, uint32_t gas);
void NoteCellDupes(uint32_t tick, uint32_t dropped, uint32_t firstCellIdx);
void NoteSolutePourTrunc(uint32_t ops, uint32_t units);
// One op emitted into a chunk this machine does not own. `producer` is a
// Producer, or Producer::Count for a CellOp (which has no OpMeta). Only the
// FIRST call fills the attribution fields; the rest only bump the count.
void NoteAuthorityViolation(uint32_t tick, uint32_t producer, int32_t wcx,
                            int32_t wcy, int32_t wcz, uint32_t owner);
// One op whose producer never adopted an author scope.
void NoteUnknownProducer();

// ---- 2. THE RECORD --------------------------------------------------------

// Everything SubmitTick is told that is NOT an op vector. A replay that
// rebuilt these from the game would be replaying a different tick.
struct TickInputs {
  uint32_t tick = 0;
  uint32_t seed = 0;
  uint32_t hashEnable = 0;
  uint32_t wantReadback = 0;
  uint32_t particlesActive = 0;
  uint32_t vizActive = 0;
  int32_t playerChunk[3] = {0, 0, 0};
  uint32_t farCount = 0;
  uint32_t fluidLive = 0;
};

// One tick, as it reached the GPU.
struct Frame {
  TickInputs in;
  // THE PLAYER'S COMMAND FOR THIS TICK (package N2, sim/tickinput.h). The
  // other half of "the whole tick input": `in` above is what SubmitTick was
  // told, this is what the HUMAN asked for, and a replay that has the first
  // without the second can reproduce the world but not the player. Zeroed
  // (version 0) on any tick nobody called NoteTickInput for -- the gates, the
  // smokes and the lab all drive SubmitTick with no controller attached, and
  // "there was no command" has to be distinguishable from "the command was
  // all zeroes".
  TickInput cmd{};
  TickParams tp{};
  std::vector<BrushOp> ops;
  std::vector<ExplosionOp> exps;
  std::vector<CellOp> cells;
  std::vector<ParticleSpawn> spawns;
  std::vector<FluidSpawnOp> fluid;
  std::vector<GasSpawnOp> gas;
  std::vector<uint32_t> genList;  // slots worldgen refilled on this tick
  std::vector<OpMeta> opMeta;     // parallel to `ops`
  std::vector<OpMeta> expMeta;    // parallel to `exps`
  // ---- THE THIRD PER-TICK INPUT THAT IS NOT AN OP (M9.3-C) ---------------
  //
  // A chunk resync replaces 4,096 voxel words wholesale (net/chunksync.h). It
  // belongs with CLAUDE.md rule 3's snapshot-restore exceptions — it does not
  // go through the MutationQueue, because "the peer's copy of this chunk" is
  // not expressible as a cell at a time — and it is therefore INVISIBLE to a
  // replay that only feeds ops back. A record of a networked session that did
  // not carry it would diverge at the first sync with nothing in the file to
  // say why.
  //
  // Same shape and same reason as `genList`: stashed by the producer through
  // NoteChunkReplace, folded into this tick's frame by RecordFrame, applied on
  // replay by ReplaceChunksIfReplaying at the phase-B position. The payload is
  // the SAME RLE the store and the wire use (sim/stream.h), so the record
  // costs what the wire cost and decodes through one function.
  struct ChunkReplace {
    int32_t wc[3] = {0, 0, 0};
    std::vector<uint32_t> rle;  // (word, runLength) pairs, kPersistMask'd
  };
  std::vector<ChunkReplace> chunkReplaces;
};

// The header, checked on load the way worldio's meta.svm is: a record made by
// a build with a different window size, chunk size, voxel scale or material
// table describes a world this binary cannot reproduce, and saying so beats
// replaying it into a hash mismatch nobody can attribute.
struct Header {
  uint32_t version = 0;
  uint32_t worldN = 0;
  uint32_t chunk = 0;
  uint32_t voxelMetersBits = 0;
  uint32_t seed = 0;
  uint32_t matCount = 0;
  uint32_t matHash = 0;
  uint32_t tickParamsBytes = 0;
};
// 2: Frame carries the player's TickInput (package N2).
// 3: Frame carries chunkReplaces, the chunk resyncs installed on that tick
//    (M9.3-C). A version-2 record has none, and could in principle be read
//    with a default-empty vector — but a frame body is a flat byte run with no
//    per-field tags, so a reader that expected the field would run off the end
//    of every old frame. The bump turns that into one clear refusal.
// 4: TickInputs lost hasSplashMat + fluidSplashMat[4] (W1-B2: a fluid
//    particle splashes as its own material, so the per-species splash table
//    is gone). TickInputs is written as one POD, so the frame shrank.
// 5: TickInput grew `useRef` (72 -> 76 bytes, the use verb, PLAN_world_
//    editor.md P1); the frame's command POD grew with it.
constexpr uint32_t kRecordVersion = 5;

// ---- recording ----
// Start appending frames to `path`. `mats` is the loaded material table: its
// names are hashed into the header. Returns false and fills `err` if the file
// cannot be opened.
//
// SANDVOX_RECORD_OPS=<path> starts one automatically at the first SubmitTick
// of ANY run — the game, --selftest, --shot, the smokes — which is what makes
// the recorder useful before main.cpp grows a --record-ops flag.
bool StartRecording(const std::string& path, uint32_t seed,
                    const std::vector<MaterialDef>& mats, std::string& err);
void StopRecording();
bool Recording();
uint64_t RecordedBytes();
uint32_t RecordedFrames();

// Called by SubmitTick, once per tick, after TickParams is complete and every
// stream has been clamped. A no-op (one bool test) when nothing is recording
// and nothing is replaying.
void RecordFrame(const TickInputs& in, const TickParams& tp,
                 const std::vector<BrushOp>& ops,
                 const std::vector<ExplosionOp>& exps, const CellOp* cells,
                 uint32_t cellCount, const ParticleSpawn* spawns,
                 uint32_t spawnCount, const FluidSpawnOp* fluid,
                 uint32_t fluidCount, const std::vector<GasSpawnOp>& gas);

// The gen list Stream dispatched for this tick (sim/stream.cpp). Stashed and
// folded into the next frame: worldgen's second TickParams write is a separate
// submit, and a replay that did not know which planes were regenerated cannot
// reproduce them.
void NoteGenList(uint32_t tick, const std::vector<uint32_t>& slots);

// ONE CHUNK RESYNC, stashed into this tick's frame (M9.3-C). Called by phase B
// immediately AFTER Stream::ReplaceChunk succeeds — after, so a refused
// replace (the window shifted between the request and the reply) does not put
// a replace in the record that a replay would then apply. Several per tick
// accumulate; the stash is cleared when the tick number moves, exactly the way
// the gen list's is, so a stale one cannot land in the wrong frame.
void NoteChunkReplace(uint32_t tick, IVec3 wc, const std::vector<uint32_t>& rle);

// The player's command for this tick (main.cpp's frame layer, package N2).
// Stashed the same way the gen list is and for the same reason: it is produced
// at the TOP of the tick body and SubmitTick runs at the bottom, so threading
// it through the submit signature would widen a call four harnesses share to
// carry a value only the game has. A tick nobody calls this for records a
// zeroed command with version 0.
void NoteTickInput(uint32_t tick, const TickInput& cmd);

// ---- replay ----
// A loaded record. Parsing lives here; the DRIVE LOOP lives in the caller
// (selftest_sim.cpp's `ops-replay` gate, and main.cpp's --replay-ops when the
// flag lands), because feeding frames means calling SubmitTick and sim/ must
// not depend on test/.
struct Log {
  Header header;
  std::vector<Frame> frames;
  bool Load(const std::string& path, const std::vector<MaterialDef>& mats,
            std::string& err);
};

// Arm replay mode: while a log is set, SubmitTick takes the tick's gas spawns
// from the record instead of from World's CPU queue, and every rebuilt
// TickParams is COMPARED against the recorded one. The comparison is the
// interesting half — it asserts that the whole per-tick knob stream (wind, day
// phase, fluid gates, water bodies) is a pure function of the recorded inputs,
// which is what "a replay reproduces the stream" actually means.
void SetReplay(const Log* log);
const Log* Replay();
// While replaying, swap `gas` for the recorded tick's list. No-op otherwise.
void ReplaceGasIfReplaying(uint32_t tick, std::vector<GasSpawnOp>& gas);
// While replaying, the recorded tick's TickParams::weatherRain (true + `word`);
// false otherwise. The rain word carries the dev-panel weather PIN, a human
// input no other recorded field reproduces, so a replay takes it from the
// record rather than rebuilding it from whatever the replaying process has
// pinned (weather::TakeTickRain).
bool RecordedWeatherRain(uint32_t tick, uint32_t& word);

// While replaying, re-apply this tick's recorded chunk replaces through
// `stream`. A no-op (one pointer test) otherwise, so the drive loops call it
// unconditionally and single-player pays nothing.
//
// WHERE: THE PHASE-B POSITION — before Stream::Update and before the tick's
// submit, the same point session.cpp installs a live sync at. A replace
// applied after the submit would land one tick late and every hash probe from
// there on would describe a world one tick of CA away from the recording's.
//
// Returns how many it applied, so a drive loop can report "3 replaces"
// instead of leaving a silent difference between record and replay.
// ::Stream, GLOBALLY QUALIFIED.  is this namespace's op-kind
// enum (Brush / Explosion / ...) and would win the lookup, which is a compile
// error that reads like a missing include.
uint32_t ReplaceChunksIfReplaying(uint32_t tick, ::Stream& stream);
// ...and how many the replay REFUSED (chunk not resident), since the last
// ResetReplayStats. Must be 0 on a faithful replay: the recording's window was
// in the same place at the same tick, so a refusal means the replay's
// streaming diverged before the replace did.
uint32_t ReplayChunkReplaceRefusals();
// TickParams words that differed from the record since the last reset, and the
// first tick at which one did.
uint32_t ReplayParamMismatches();
uint32_t ReplayFirstMismatchTick();
uint32_t ReplayFirstMismatchWord();
// The two VALUES behind that first word, and every DISTINCT word index that
// ever differed. CLAUDE.md rule 6: a bare "416 words rebuilt differently" is a
// count and buys one hypothesis per run; "words 8,9,10,34,35,36 — word 8 was
// 1714 in the record, 1712 on replay" names the fields (here: TickParams
// origin[3] and mirrorBase[3], i.e. the residency window, which no recorded
// input carries).
uint32_t ReplayFirstMismatchRecorded();
uint32_t ReplayFirstMismatchRebuilt();
const std::vector<uint32_t>& ReplayMismatchWords();
void ResetReplayStats();

// The material NAME table hash the header carries. Exposed so a caller can
// report the two values rather than "hash mismatch".
uint32_t MaterialTableHash(const std::vector<MaterialDef>& mats);

// SANDVOX_RECORD_OPS, or empty. Read once. The harness that owns both a
// material table and the tick loop (selftest.cpp's Run) honours it; main.cpp's
// --record-ops flag will call StartRecording directly when the file is free to
// edit. Empty string = do not record.
const std::string& EnvRecordPath();

}  // namespace opstream
}  // namespace sandvox
