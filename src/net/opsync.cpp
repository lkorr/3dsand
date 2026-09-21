#include "net/opsync.h"

#include <algorithm>

#include "sim/bytestream.h"

// M9.3-B. The header carries the argument; this file is the mechanism.

namespace net {
namespace {

using sandvox::opstream::OpMeta;
using sandvox::opstream::Producer;

// The five vectors, in ONE order, written once. Encode and Decode both go
// through it, so a vector added to OpBatch cannot be encoded and forgotten on
// the read side (the compiler will not catch that; a single list will).
template <typename W, typename B>
void WalkBatch(W&& w, B& b) {
  w(b.ops);
  w(b.exps);
  w(b.cells);
  w(b.spawns);
  w(b.fluid);
}

}  // namespace

// ---- the wire blob ------------------------------------------------------

void OpsWire::Encode(const OpBatch& b, IVec3 origin, uint32_t label,
                     std::vector<uint8_t>& out) {
  out.clear();
  ByteWriter w{out};
  w.U32(kOpsWireVersion);
  w.U32(label);
  w.Pod(origin.x);
  w.Pod(origin.y);
  w.Pod(origin.z);
  // WalkBatch is templated on the batch's constness, so the encode side walks
  // a `const OpBatch&` and the decode side a mutable one through ONE list.
  WalkBatch([&](const auto& v) { w.PodVec(v); }, b);
}

bool OpsWire::Decode(const uint8_t* p, size_t n, OpBatch& b, IVec3& origin,
                     uint32_t& label) {
  b.Clear();
  ByteReader r{p, n};
  uint32_t ver = 0;
  if (!r.U32(ver) || ver != kOpsWireVersion) return false;
  if (!r.U32(label)) return false;
  if (!r.Pod(origin.x) || !r.Pod(origin.y) || !r.Pod(origin.z)) return false;
  WalkBatch([&](auto& v) { r.PodVec(v); }, b);
  // ByteReader's `ok` is sticky, so one short read poisons everything after
  // it. A half-decoded batch is worse than none: the pacer would run the tick
  // believing the peer's whole intent had arrived.
  if (!r.ok) {
    b.Clear();
    return false;
  }
  return true;
}

// ---- the queue ----------------------------------------------------------

void OpDelayQueue::PushLocal(uint32_t producedAt, const OpBatch& b,
                             IVec3 origin) {
  const uint32_t label = producedAt + d_;
  Label& L = labels_[label];
  L.haveLocal = true;
  L.local.batch = b;   // a copy: `out` is reused by the next tick
  L.local.origin = origin;
  L.local.author = localId_;
  L.local.opMeta.clear();
  L.local.expMeta.clear();
  localBatches++;
}

void OpDelayQueue::NoteLocalMeta(uint32_t label, std::vector<OpMeta> opMeta,
                                 std::vector<OpMeta> expMeta) {
  auto it = labels_.find(label);
  if (it == labels_.end() || !it->second.haveLocal) return;
  it->second.local.opMeta = std::move(opMeta);
  it->second.local.expMeta = std::move(expMeta);
}

bool OpDelayQueue::AppendLocalBrush(uint32_t label, const BrushOp& op,
                                    Producer p) {
  auto it = labels_.find(label);
  if (it == labels_.end() || !it->second.haveLocal) return false;
  Entry& e = it->second.local;
  e.batch.ops.push_back(op);
  // Keep the side table the same length as the vector it describes, or the
  // re-note in Merge would attribute this op to whatever followed it. A
  // shorter table is the normal case here (the paint switch runs before the
  // metadata is noted on some ticks), and resize fills the gap with
  // Producer::Unknown, which is honest.
  e.opMeta.resize(e.batch.ops.size());
  OpMeta m;
  m.producer = (uint8_t)p;
  m.author = localId_;
  e.opMeta.back() = m;
  return true;
}

void OpDelayQueue::NoteRemote(uint32_t label, uint32_t author,
                              IVec3 remoteOrigin, OpBatch b) {
  // A LABEL ALREADY MERGED CANNOT BE APPLIED. With the pacer holding the tick
  // until the peer's batch has arrived this must stay 0; a non-zero count is
  // the pacing bug's first symptom, which is why it is counted rather than
  // silently ignored.
  if (everMerged_ && label <= mergedUpTo_) {
    lateRemote++;
    return;
  }
  Label& L = labels_[label];
  Entry e;
  e.batch = std::move(b);
  e.origin = remoteOrigin;
  e.author = author;
  // Replace a batch for the same author (a resend), else insert SORTED by
  // author so Merge walks the canonical order without re-sorting.
  auto it = std::find_if(L.remote.begin(), L.remote.end(),
                         [&](const Entry& x) { return x.author == author; });
  if (it != L.remote.end()) {
    *it = std::move(e);
  } else {
    auto at = std::lower_bound(
        L.remote.begin(), L.remote.end(), author,
        [](const Entry& x, uint32_t a) { return x.author < a; });
    L.remote.insert(at, std::move(e));
  }
  remoteBatches++;
}

bool OpDelayQueue::HaveLocal(uint32_t label) const {
  auto it = labels_.find(label);
  return it != labels_.end() && it->second.haveLocal;
}

bool OpDelayQueue::HaveRemote(uint32_t label) const {
  auto it = labels_.find(label);
  return it != labels_.end() && !it->second.remote.empty();
}

bool OpDelayQueue::Ready(uint32_t label) const {
  return HaveLocal(label) && HaveRemote(label);
}

const OpBatch& OpDelayQueue::Outgoing(uint32_t label, IVec3* origin) const {
  static const OpBatch kEmpty;
  auto it = labels_.find(label);
  if (it == labels_.end() || !it->second.haveLocal) {
    if (origin) *origin = IVec3{0, 0, 0};
    return kEmpty;
  }
  if (origin) *origin = it->second.local.origin;
  return it->second.local.batch;
}

OpBatch OpDelayQueue::Merge(uint32_t label, const World& world, MergeStats& st,
                            std::vector<uint32_t>* remoteBrushIdx) {
  st = MergeStats{};
  if (remoteBrushIdx) remoteBrushIdx->clear();
  OpBatch out;

  auto it = labels_.find(label);
  if (it == labels_.end()) {
    missingLocal++;
    missingRemote++;
  } else {
    Label& L = it->second;
    st.haveLocal = L.haveLocal;
    st.haveRemote = !L.remote.empty();
    if (!L.haveLocal) missingLocal++;
    if (L.remote.empty()) missingRemote++;

    // ---- THE CANONICAL ORDER: author ascending, push order within --------
    //
    // Built as a list of sources rather than by "local then remote" or
    // "remote then local", because both of those are a notion of "mine",
    // and the whole point is that neither machine has one. The host is
    // player 0, so a host's ops lead on BOTH machines.
    std::vector<const Entry*> srcs;
    srcs.reserve(L.remote.size() + 1);
    for (const Entry& e : L.remote) srcs.push_back(&e);
    if (L.haveLocal) {
      auto at = std::lower_bound(
          srcs.begin(), srcs.end(), L.local.author,
          [](const Entry* x, uint32_t a) { return x->author < a; });
      srcs.insert(at, &L.local);
    }
    st.authors = (uint32_t)srcs.size();

    for (const Entry* e : srcs) {
      const bool isLocal = (e == &L.local);
      const size_t brushBegin = out.ops.size();
      const size_t expBegin = out.exps.size();

      if (isLocal) {
        // THE LOCAL BATCH, VERBATIM. Its ops were produced D ticks ago and
        // its author ranges were recorded THEN, against a vector that started
        // at index 0 — but under the merge they may sit after a peer's ops.
        // So the ranges were resolved to a flat side table at production time
        // (NoteLocalMeta) and are re-noted here at the MERGED indices. Without
        // this the client (player 1, whose ops follow the host's) would
        // attribute every one of its own ops to the host's.
        out.ops.insert(out.ops.end(), e->batch.ops.begin(), e->batch.ops.end());
        out.exps.insert(out.exps.end(), e->batch.exps.begin(),
                        e->batch.exps.end());
        out.cells.insert(out.cells.end(), e->batch.cells.begin(),
                         e->batch.cells.end());
        out.spawns.insert(out.spawns.end(), e->batch.spawns.begin(),
                          e->batch.spawns.end());
        out.fluid.insert(out.fluid.end(), e->batch.fluid.begin(),
                         e->batch.fluid.end());
        // Re-note as RUNS of equal (producer, author): one range per run
        // reproduces exactly what ResolveAuthors would have expanded, at a
        // fraction of the ranges.
        auto renote = [&](sandvox::opstream::Stream s,
                          const std::vector<OpMeta>& meta, size_t base) {
          size_t i = 0;
          while (i < meta.size()) {
            size_t j = i + 1;
            while (j < meta.size() && meta[j].producer == meta[i].producer &&
                   meta[j].author == meta[i].author)
              j++;
            if (meta[i].producer != (uint8_t)Producer::Unknown)
              sandvox::opstream::NoteAuthorRange(
                  s, (uint32_t)(base + i), (uint32_t)(base + j),
                  (Producer)meta[i].producer, meta[i].author);
            i = j;
          }
        };
        renote(sandvox::opstream::Stream::Brush, e->opMeta, brushBegin);
        renote(sandvox::opstream::Stream::Explosion, e->expMeta, expBegin);
        continue;
      }

      // ---- A PEER'S BATCH ---------------------------------------------
      //
      // Producer::Remote, author = the peer's playerId, for every op of it.
      // The receiver must never re-emit a peer's op as if it were its own
      // (net::Owns returns false for Remote, and that is what the value
      // exists for), and the record has to be able to say which machine an
      // op came from.
      {
        sandvox::opstream::BrushAuthorScope bs(out.ops, Producer::Remote,
                                               e->author);
        for (const BrushOp& b : e->batch.ops) {
          if (remoteBrushIdx) remoteBrushIdx->push_back((uint32_t)out.ops.size());
          out.ops.push_back(b);
          st.remoteBrush++;
        }
      }
      {
        sandvox::opstream::ExpAuthorScope es(out.exps, Producer::Remote,
                                             e->author);
        for (const ExplosionOp& x : e->batch.exps) {
          out.exps.push_back(x);
          st.remoteExps++;
        }
      }
      // CELLS: the one kind that needs the sender's origin (see the header's
      // point 3). Dropped ops are COUNTED and the first one's chunk is kept,
      // because "12 cells dropped" with nothing attached is the bare-count
      // failure of CLAUDE.md rule 6.
      for (const CellOp& c : e->batch.cells) {
        const uint32_t slot = c.cellIdx / kChunkVol;
        IVec3 wc{0, 0, 0};
        bool keep = World::IsWindowSlot(slot);
        if (keep) {
          wc = SlotChunkUnderOrigin(slot, e->origin);
          keep = world.ChunkInWindow(wc);
        }
        if (!keep) {
          st.remoteCellsDropped++;
          droppedCells++;
          if (!st.haveFirstDrop) {
            st.haveFirstDrop = true;
            st.firstDropChunk[0] = wc.x;
            st.firstDropChunk[1] = wc.y;
            st.firstDropChunk[2] = wc.z;
          }
          continue;
        }
        out.cells.push_back(c);
        st.remoteCells++;
      }
      // Particle and fluid spawns are absolute world coords (24.8 and Q16.16
      // respectively) and need no translation; a spawn outside the window is
      // the kernel's problem and it already refuses it.
      for (const ParticleSpawn& s : e->batch.spawns) {
        out.spawns.push_back(s);
        st.remoteSpawns++;
      }
      for (const FluidSpawnOp& f : e->batch.fluid) {
        out.fluid.push_back(f);
        st.remoteFluid++;
      }
    }
  }

  st.merged = (uint32_t)(out.ops.size() + out.exps.size() + out.cells.size() +
                         out.spawns.size() + out.fluid.size());
  mergedMax = std::max<uint64_t>(mergedMax, st.merged);
  mergedUpTo_ = label;
  everMerged_ = true;
  // Labels are consumed in tick order and nothing asks for a past one twice.
  labels_.erase(labels_.begin(), labels_.upper_bound(label));
  return out;
}

}  // namespace net
