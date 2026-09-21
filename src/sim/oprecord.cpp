// oprecord.cpp — see oprecord.h for what this module is and why it exists.

#include "sim/oprecord.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sim/bytestream.h"

namespace sandvox {
namespace opstream {
namespace {

// ---- module state ---------------------------------------------------------
//
// Globals rather than an object threaded through SubmitTick, for the reason
// SetHarnessSnapshotDrain is a global: SubmitTick already has fifteen
// arguments and exactly one call site cares. Everything here is CPU-side and
// touched only from the tick body's fixed sequence, so it cannot introduce a
// scheduling dependency into the sim (rule 1).

// This tick's author ranges, one list per stream. Cleared once per tick by
// RecordFrame, so an op vector that is never framed cannot grow it forever.
struct AuthorRange {
  uint32_t begin, end;
  OpMeta meta;
};
std::vector<AuthorRange> g_ranges[(size_t)Stream::Count];
StreamCounts g_counts;        // clamps + dedupe, this run
std::FILE* g_rec = nullptr;   // open record file, or null
uint64_t g_recBytes = 0;
uint32_t g_recFrames = 0;
const Log* g_replay = nullptr;
uint32_t g_replayMismatch = 0, g_replayFirstTick = 0, g_replayFirstWord = 0;
// The gen list Stream reported for a tick that has not been framed yet.
uint32_t g_genTick = 0xFFFFFFFFu;
std::vector<uint32_t> g_genSlots;
// SANDVOX_RECORD_OPS, resolved once.
bool g_envChecked = false;
std::string g_envPath;

constexpr uint32_t kFileMagic = 0x53565250u;  // 'SVRP'
constexpr uint32_t kFrameMagic = 0x4F50544Bu; // 'OPTK'

uint32_t VoxelMetersBits() {
  uint32_t bits = 0;
  static_assert(sizeof(bits) == sizeof(kVoxelMeters), "float32 expected");
  std::memcpy(&bits, &kVoxelMeters, sizeof(bits));
  return bits;
}

// FNV-1a over the material NAMES, in id order. Names rather than the whole
// MaterialDef for the same reason worldio stores names: material IDS are baked
// into every op we record, so what has to match is the id -> name mapping, not
// the tuning of the materials behind it. Retuning stone's hardness must not
// invalidate a record; renaming id 1 must.
uint32_t HashNames(const std::vector<MaterialDef>& mats) {
  uint32_t h = 2166136261u;
  for (const MaterialDef& m : mats) {
    for (char ch : m.name) {
      h ^= (uint32_t)(uint8_t)ch;
      h *= 16777619u;
    }
    h ^= 0xFFu;
    h *= 16777619u;
  }
  return h;
}

bool WriteAll(const std::vector<uint8_t>& b) {
  if (!g_rec) return false;
  if (std::fwrite(b.data(), 1, b.size(), g_rec) != b.size()) {
    std::fprintf(stderr, "oprecord: short write, recording stopped\n");
    std::fclose(g_rec);
    g_rec = nullptr;
    return false;
  }
  g_recBytes += b.size();
  return true;
}

}  // namespace

// ---- author ---------------------------------------------------------------

const char* ProducerName(uint8_t p) {
  static const char* kNames[] = {"unknown", "brush",     "spell",
                                 "mob",     "avatar",    "debris",
                                 "editlayer", "worldgen", "lab"};
  return p < (uint8_t)Producer::Count ? kNames[p] : "?";
}

void NoteAuthorRange(Stream s, uint32_t begin, uint32_t end, Producer p,
                     uint32_t author) {
  if (end <= begin || s >= Stream::Count) return;
  AuthorRange r;
  r.begin = begin;
  r.end = end;
  r.meta.producer = (uint8_t)p;
  r.meta.author = author;
  g_ranges[(size_t)s].push_back(r);
}

std::vector<OpMeta> ResolveAuthors(Stream s, uint32_t count) {
  std::vector<OpMeta> out(count);
  if (s >= Stream::Count) return out;
  // Applied in registration order, so an inner scope (registered LAST, because
  // it is destroyed first) overwrites the outer one. That is the attribution
  // anyone would expect: the nearest producer to the push wins.
  for (const AuthorRange& r : g_ranges[(size_t)s])
    for (uint32_t i = r.begin; i < r.end && i < count; i++) out[i] = r.meta;
  return out;
}

void ClearAuthorRanges() {
  for (auto& v : g_ranges) v.clear();
}

// ---- canonicalization -----------------------------------------------------

// SANDVOX_OPS_NO_DEDUPE=1 turns the canonicalization OFF and puts the cell-op
// stream back to "whichever invocation the driver ran last wins".
//
// IT EXISTS FOR ATTRIBUTION, not for use. A gate that changes verdict when the
// dedupe lands has same-tick duplicate cell ops in its fixture, and the ONLY
// cheap way to establish that is to run the same binary with the rule off
// (CLAUDE.md rule 6: adding attribution to the reporter buys every hypothesis
// at once, A/B elimination buys one). Do not ship a measurement taken with it
// set — with it set the run is not deterministic.
static bool NoDedupe() {
  static const bool off = [] {
    const char* e = std::getenv("SANDVOX_OPS_NO_DEDUPE");
    return e && e[0] == '1';
  }();
  return off;
}

uint32_t CanonicalizeCells(const std::vector<CellOp>& in,
                           std::vector<CellOp>& out, uint32_t* firstDupeCell) {
  const size_t n = in.size();
  if (n < 2 || NoDedupe()) return 0;
  // Sort an INDEX PERMUTATION on a (cellIdx, pushIndex) key rather than the
  // payload, and compact the survivors back in push order. Two consequences,
  // both load-bearing:
  //   * a stream with no duplicates comes out byte-identical to what went in,
  //     so this cannot move the world hash except where a race existed;
  //   * the surviving op for a contested cell is the FIRST one pushed, which
  //     is the priority the CPU tick body already establishes.
  // The packed u64 key gives a total order with no ties, so plain std::sort is
  // as deterministic as a stable one and cheaper.
  static std::vector<uint64_t> keys;  // reused: this runs on the frame path
  keys.clear();
  keys.reserve(n);
  for (size_t i = 0; i < n; i++)
    keys.push_back(((uint64_t)in[i].cellIdx << 32) | (uint32_t)i);
  std::sort(keys.begin(), keys.end());
  uint32_t dupes = 0, firstCell = 0xFFFFFFFFu;
  static std::vector<uint8_t> drop;
  drop.assign(n, 0);
  for (size_t k = 1; k < n; k++) {
    if ((uint32_t)(keys[k] >> 32) != (uint32_t)(keys[k - 1] >> 32)) continue;
    if (dupes == 0) firstCell = (uint32_t)(keys[k] >> 32);
    drop[(uint32_t)(keys[k] & 0xFFFFFFFFu)] = 1;
    dupes++;
  }
  if (dupes == 0) return 0;
  out.clear();
  out.reserve(n - dupes);
  for (size_t i = 0; i < n; i++)
    if (!drop[i]) out.push_back(in[i]);
  if (firstDupeCell) *firstDupeCell = firstCell;
  return dupes;
}

// ---- counters -------------------------------------------------------------

const StreamCounts& Counts() { return g_counts; }
void ResetCounts() { g_counts = StreamCounts{}; }

void NoteTruncation(uint32_t brush, uint32_t exp, uint32_t cell, uint32_t spawn,
                    uint32_t fluid, uint32_t gas) {
  g_counts.brushTrunc += brush;
  g_counts.expTrunc += exp;
  g_counts.cellTrunc += cell;
  g_counts.spawnTrunc += spawn;
  g_counts.fluidTrunc += fluid;
  g_counts.gasTrunc += gas;
}

void NoteCellDupes(uint32_t tick, uint32_t dropped, uint32_t firstCellIdx) {
  if (dropped == 0) return;
  if (g_counts.cellDupes == 0) {
    g_counts.firstDupeTick = tick;
    if (firstCellIdx != 0xFFFFFFFFu) g_counts.firstDupeCell = firstCellIdx;
  }
  g_counts.cellDupes += dropped;
  g_counts.ticksWithDupes++;
}

// ---- recording ------------------------------------------------------------

uint32_t MaterialTableHash(const std::vector<MaterialDef>& mats) {
  return HashNames(mats);
}

bool StartRecording(const std::string& path, uint32_t seed,
                    const std::vector<MaterialDef>& mats, std::string& err) {
  StopRecording();
  g_rec = std::fopen(path.c_str(), "wb");
  if (!g_rec) {
    err = "cannot open " + path + " for writing";
    return false;
  }
  g_recBytes = 0;
  g_recFrames = 0;
  std::vector<uint8_t> b;
  ByteWriter w{b};
  w.U32(kFileMagic);
  w.U32(kRecordVersion);
  w.U32(kWorldN);
  w.U32(kChunk);
  w.U32(VoxelMetersBits());
  w.U32(seed);
  w.U32((uint32_t)mats.size());
  w.U32(HashNames(mats));
  w.U32((uint32_t)sizeof(TickParams));
  return WriteAll(b);
}

void StopRecording() {
  if (!g_rec) return;
  std::fclose(g_rec);
  g_rec = nullptr;
}

bool Recording() { return g_rec != nullptr; }
uint64_t RecordedBytes() { return g_recBytes; }
uint32_t RecordedFrames() { return g_recFrames; }

void NoteGenList(uint32_t tick, const std::vector<uint32_t>& slots) {
  if (!g_rec && !g_replay) return;
  g_genTick = tick;
  g_genSlots = slots;
}

void RecordFrame(const TickInputs& in, const TickParams& tp,
                 const std::vector<BrushOp>& ops,
                 const std::vector<ExplosionOp>& exps, const CellOp* cells,
                 uint32_t cellCount, const ParticleSpawn* spawns,
                 uint32_t spawnCount, const FluidSpawnOp* fluid,
                 uint32_t fluidCount, const std::vector<GasSpawnOp>& gas) {
  // ---- the replay half: COMPARE, do not overwrite ------------------------
  //
  // A replay does not feed TickParams back to the GPU; it feeds the recorded
  // INPUTS to the same SubmitTick and checks that the struct it rebuilt is the
  // one the recording built. That is the stronger claim and the cheaper code:
  // if the two ever differ, some per-tick knob is being read from somewhere
  // that is not the input stream, which is exactly the bug the record exists
  // to catch.
  // ...with ONE exemption, and the bar for adding another is the paragraph
  // world.h writes beside the field itself. `snapEpoch` is a monotone nonce
  // from a process-global atomic (NextSnapEpoch), not an input and not hashed
  // state: the repose prepass writes an epoch and the CA compares against the
  // one it wrote, so the sim is invariant to the VALUE and only depends on a
  // stamp from an earlier tick never reading as current. world.h says so in as
  // many words, and the `determinism` gate PROVES it every run — its two runs
  // sit at different epochs in the same process and agree bit for bit. A
  // record that carried the epoch would therefore be pinning a number that is
  // allowed to differ; comparing it would fail every replay for no defect.
  // Anything else that differs here IS the bug this loop exists to catch.
  const size_t kEpochWord = offsetof(TickParams, snapEpoch) / 4;
  if (g_replay) {
    for (const Frame& f : g_replay->frames) {
      if (f.in.tick != in.tick) continue;
      const uint32_t* a = (const uint32_t*)&f.tp;
      const uint32_t* c = (const uint32_t*)&tp;
      for (size_t i = 0; i < sizeof(TickParams) / 4; i++) {
        if (a[i] == c[i] || i == kEpochWord) continue;
        if (g_replayMismatch == 0) {
          g_replayFirstTick = in.tick;
          g_replayFirstWord = (uint32_t)i;
        }
        g_replayMismatch++;
      }
      break;
    }
  }
  // The author ranges belong to THIS tick and to no other: clear them whatever
  // happens below, or a producer whose ops were never framed would keep
  // claiming indices in every later tick.
  struct ClearOnExit {
    ~ClearOnExit() { ClearAuthorRanges(); }
  } clearRanges;
  if (!g_rec) return;

  std::vector<uint8_t> body;
  ByteWriter w{body};
  w.Pod(in);
  w.Bytes(&tp, sizeof(TickParams));
  w.PodVec(ops);
  w.PodVec(exps);
  w.U32(cellCount);
  if (cellCount) w.Bytes(cells, (size_t)cellCount * sizeof(CellOp));
  w.U32(spawnCount);
  if (spawnCount) w.Bytes(spawns, (size_t)spawnCount * sizeof(ParticleSpawn));
  w.U32(fluidCount);
  if (fluidCount) w.Bytes(fluid, (size_t)fluidCount * sizeof(FluidSpawnOp));
  w.PodVec(gas);
  // The gen list belongs to this tick only if Stream reported it for this
  // tick; a stale one from an earlier tick would put a plane in the wrong
  // frame, which is worse than an empty list.
  std::vector<uint32_t> gen;
  if (g_genTick == in.tick) gen = g_genSlots;
  w.PodVec(gen);
  // The author side table, expanded from this tick's ranges. Producers that
  // have not adopted a scope report Unknown/0, which the reader can tell apart
  // from "there were no ops".
  std::vector<OpMeta> opMeta = ResolveAuthors(Stream::Brush, (uint32_t)ops.size());
  std::vector<OpMeta> expMeta =
      ResolveAuthors(Stream::Explosion, (uint32_t)exps.size());
  w.PodVec(opMeta);
  w.PodVec(expMeta);

  std::vector<uint8_t> frame;
  ByteWriter fw{frame};
  fw.U32(kFrameMagic);
  fw.U32((uint32_t)body.size());
  fw.Bytes(body.data(), body.size());
  if (WriteAll(frame)) g_recFrames++;
}

// ---- replay ---------------------------------------------------------------

void SetReplay(const Log* log) { g_replay = log; }
const Log* Replay() { return g_replay; }
uint32_t ReplayParamMismatches() { return g_replayMismatch; }
uint32_t ReplayFirstMismatchTick() { return g_replayFirstTick; }
uint32_t ReplayFirstMismatchWord() { return g_replayFirstWord; }
void ResetReplayStats() {
  g_replayMismatch = 0;
  g_replayFirstTick = 0;
  g_replayFirstWord = 0;
}

void ReplaceGasIfReplaying(uint32_t tick, std::vector<GasSpawnOp>& gas) {
  if (!g_replay) return;
  for (const Frame& f : g_replay->frames) {
    if (f.in.tick != tick) continue;
    gas = f.gas;
    return;
  }
  gas.clear();
}

bool Log::Load(const std::string& path, const std::vector<MaterialDef>& mats,
               std::string& err) {
  frames.clear();
  std::FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) {
    err = "cannot open " + path;
    return false;
  }
  std::fseek(fp, 0, SEEK_END);
  long len = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);
  std::vector<uint8_t> buf((size_t)(len > 0 ? len : 0));
  if (!buf.empty() && std::fread(buf.data(), 1, buf.size(), fp) != buf.size()) {
    std::fclose(fp);
    err = "short read on " + path;
    return false;
  }
  std::fclose(fp);

  ByteReader r{buf.data(), buf.size()};
  uint32_t magic = 0;
  r.U32(magic);
  if (!r.ok || magic != kFileMagic) {
    err = "not an op record (bad magic)";
    return false;
  }
  r.U32(header.version);
  r.U32(header.worldN);
  r.U32(header.chunk);
  r.U32(header.voxelMetersBits);
  r.U32(header.seed);
  r.U32(header.matCount);
  r.U32(header.matHash);
  r.U32(header.tickParamsBytes);
  if (!r.ok) {
    err = "truncated header";
    return false;
  }
  // The refusals, in worldio's style: name the two values, never just "hash
  // mismatch". A record that describes a world this binary cannot build is a
  // configuration report, not a corruption.
  char msg[256];
  if (header.version != kRecordVersion) {
    std::snprintf(msg, sizeof(msg), "record version %u, this build reads %u",
                  header.version, kRecordVersion);
    err = msg;
    return false;
  }
  if (header.worldN != kWorldN || header.chunk != kChunk) {
    std::snprintf(msg, sizeof(msg),
                  "record is %u^3 world / %u chunk, this build is %u^3 / %u",
                  header.worldN, header.chunk, kWorldN, kChunk);
    err = msg;
    return false;
  }
  if (header.voxelMetersBits != VoxelMetersBits()) {
    std::snprintf(msg, sizeof(msg),
                  "record voxel size bits %08x, this build %08x (%.4f m)",
                  header.voxelMetersBits, VoxelMetersBits(), kVoxelMeters);
    err = msg;
    return false;
  }
  if (header.tickParamsBytes != (uint32_t)sizeof(TickParams)) {
    std::snprintf(msg, sizeof(msg),
                  "record TickParams is %u B, this build's is %u B",
                  header.tickParamsBytes, (uint32_t)sizeof(TickParams));
    err = msg;
    return false;
  }
  const uint32_t nowHash = HashNames(mats);
  if (header.matCount != (uint32_t)mats.size() || header.matHash != nowHash) {
    std::snprintf(msg, sizeof(msg),
                  "record material table %u names / %08x, this build %u / %08x "
                  "- material ids are baked into every op",
                  header.matCount, header.matHash, (uint32_t)mats.size(),
                  nowHash);
    err = msg;
    return false;
  }

  while (r.ok && r.off + 8 <= r.n) {
    uint32_t fmagic = 0, flen = 0;
    r.U32(fmagic);
    r.U32(flen);
    if (!r.ok || fmagic != kFrameMagic) {
      err = "frame framing lost";
      return false;
    }
    if (r.off + flen > r.n) {
      err = "truncated frame (recording interrupted?)";
      return false;
    }
    ByteReader fr{buf.data() + r.off, flen};
    r.off += flen;
    Frame f;
    fr.Pod(f.in);
    fr.Bytes(&f.tp, sizeof(TickParams));
    fr.PodVec(f.ops);
    fr.PodVec(f.exps);
    fr.PodVec(f.cells);
    fr.PodVec(f.spawns);
    fr.PodVec(f.fluid);
    fr.PodVec(f.gas);
    fr.PodVec(f.genList);
    fr.PodVec(f.opMeta);
    fr.PodVec(f.expMeta);
    if (!fr.ok) {
      err = "malformed frame payload";
      return false;
    }
    frames.push_back(std::move(f));
  }
  return true;
}

// ---- the env-var entry point ----------------------------------------------
//
// main.cpp owns argv and is claimed by another agent this package, so the two
// flags the plan names (--record-ops / --replay-ops) are not wired there yet.
// The env var costs nothing and works from every harness; when the flags land
// they call StartRecording/SetReplay directly and this stays as the way to
// record the GAME without one.
const std::string& EnvRecordPath() {
  if (!g_envChecked) {
    g_envChecked = true;
    if (const char* p = std::getenv("SANDVOX_RECORD_OPS")) g_envPath = p;
  }
  return g_envPath;
}

}  // namespace opstream
}  // namespace sandvox
