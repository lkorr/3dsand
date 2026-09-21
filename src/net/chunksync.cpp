#include "net/chunksync.h"

#include <algorithm>
#include <cstring>

#include "sim/bytestream.h"
#include "sim/stream.h"  // RleEncodeChunk / RleDecodeChunk, kPersistMask

namespace net {

// ---- message codecs -------------------------------------------------------
//
// Raw little-endian PODs through sim/bytestream.h, like every other message in
// this protocol. Each one carries its own `version` FIRST, so a skew is caught
// before a single field is read at the wrong offset; `Decode` returns false and
// main.cpp drops the message rather than acting on half of it.

void HashBlocksMsg::Encode(std::vector<uint8_t>& out) const {
  ByteWriter w{out};
  w.U32(version);
  w.U32(hashTick);
  w.Bytes(origin, sizeof(origin));
  w.PodVec(blocks);
}

bool HashBlocksMsg::Decode(const uint8_t* p, size_t n) {
  ByteReader r{p, n};
  r.U32(version);
  if (version != kChunkSyncVersion) return false;
  r.U32(hashTick);
  r.Bytes(origin, sizeof(origin));
  r.PodVec(blocks);
  return r.ok;
}

void HashDrillMsg::Encode(std::vector<uint8_t>& out) const {
  ByteWriter w{out};
  w.U32(version);
  w.U32(hashTick);
  w.Bytes(b, sizeof(b));
}

bool HashDrillMsg::Decode(const uint8_t* p, size_t n) {
  ByteReader r{p, n};
  r.U32(version);
  if (version != kChunkSyncVersion) return false;
  r.U32(hashTick);
  r.Bytes(b, sizeof(b));
  return r.ok;
}

void HashChunksMsg::Encode(std::vector<uint8_t>& out) const {
  ByteWriter w{out};
  w.U32(version);
  w.U32(hashTick);
  w.Bytes(b, sizeof(b));
  w.Bytes(digest, sizeof(digest));
}

bool HashChunksMsg::Decode(const uint8_t* p, size_t n) {
  ByteReader r{p, n};
  r.U32(version);
  if (version != kChunkSyncVersion) return false;
  r.U32(hashTick);
  r.Bytes(b, sizeof(b));
  r.Bytes(digest, sizeof(digest));
  return r.ok;
}

void ChunkRequestMsg::Encode(std::vector<uint8_t>& out) const {
  ByteWriter w{out};
  w.U32(version);
  w.U32(hashTick);
  w.Bytes(wc, sizeof(wc));
}

bool ChunkRequestMsg::Decode(const uint8_t* p, size_t n) {
  ByteReader r{p, n};
  r.U32(version);
  if (version != kChunkSyncVersion) return false;
  r.U32(hashTick);
  r.Bytes(wc, sizeof(wc));
  return r.ok;
}

void ChunkBusyMsg::Encode(std::vector<uint8_t>& out) const {
  ByteWriter w{out};
  w.U32(version);
  w.Bytes(wc, sizeof(wc));
  w.U32(reason);
}

bool ChunkBusyMsg::Decode(const uint8_t* p, size_t n) {
  ByteReader r{p, n};
  r.U32(version);
  if (version != kChunkSyncVersion) return false;
  r.Bytes(wc, sizeof(wc));
  r.U32(reason);
  return r.ok;
}

void ChunkSyncMsg::Encode(std::vector<uint8_t>& out) const {
  ByteWriter w{out};
  w.U32(version);
  w.U32(tick);
  w.Bytes(wc, sizeof(wc));
  w.U32(slice);
  w.U32(slices);
  w.U32(pairs);
  w.PodVec(bytes);
}

bool ChunkSyncMsg::Decode(const uint8_t* p, size_t n) {
  ByteReader r{p, n};
  r.U32(version);
  if (version != kChunkSyncVersion) return false;
  r.U32(tick);
  r.Bytes(wc, sizeof(wc));
  r.U32(slice);
  r.U32(slices);
  r.U32(pairs);
  r.PodVec(bytes);
  return r.ok;
}

// ---- HashTree -------------------------------------------------------------

bool HashTree::ChunkSyncable(const World& world, IVec3 wc, IVec3 myOrigin,
                             IVec3 peerOrigin) {
  // Comparable first: it is the cheap pure test and it is the one that decides
  // whether the CA was even SUPPOSED to agree here (§4 finding 1).
  if (!Comparable(wc, myOrigin, peerOrigin)) return false;
  // ...then resident under the origin we are actually holding right now. The
  // comparable test used the origin AS OF the hash tick, which may be one or
  // two chunks behind the live window; a chunk that has since left cannot be
  // read at all.
  if (!world.ChunkInWindow(wc)) return false;
  // ...and quiet. The three things this buys are in chunksync.h's header.
  return world.QuietTicks(World::SlotChunkIndex(wc)) >= kSyncQuietTicks;
}

bool HashTree::BlockDigests(const World& world, IVec3 b, IVec3 myOrigin,
                            IVec3 peerOrigin, uint32_t out[kChunksPerBlock]) {
  for (uint32_t i = 0; i < kChunksPerBlock; i++) {
    const IVec3 wc = BlockChunk(b, i);
    if (!ChunkSyncable(world, wc, myOrigin, peerOrigin)) return false;
    out[i] = world.ChunkHashOfSlot(World::SlotChunkIndex(wc));
  }
  return true;
}

void HashTree::Build(const World& world, IVec3 myOrigin, IVec3 peerOrigin,
                     std::vector<HashBlockEntry>& out) {
  out.clear();
  // WALK BLOCKS, NOT CHUNKS. The window is kNChunk^3 chunks; the blocks that
  // can be wholly inside it are the ones whose 4x4x4 span is. Starting from
  // the FLOOR of the origin's block and running one past the end is what makes
  // a window whose origin is not a multiple of 4 still enumerate every block
  // it partly holds — BlockDigests then rejects the partial ones, so the
  // arithmetic here can be generous.
  const IVec3 b0 = BlockOfChunk(myOrigin);
  const int span = ((int)kNChunk >> kHashBlockShift) + 1;
  uint32_t dig[kChunksPerBlock];
  for (int dz = 0; dz < span; dz++)
    for (int dy = 0; dy < span; dy++)
      for (int dx = 0; dx < span; dx++) {
        const IVec3 b{b0.x + dx, b0.y + dy, b0.z + dz};
        if (!BlockDigests(world, b, myOrigin, peerOrigin, dig)) continue;
        // Wrapping sum. Order-independent by construction, so neither end has
        // to agree on a traversal — see HashBlockEntry's comment.
        uint32_t sum = 0;
        for (uint32_t i = 0; i < kChunksPerBlock; i++) sum += dig[i];
        out.push_back({b.x, b.y, b.z, sum});
      }
}

// ---- ChunkSync ------------------------------------------------------------

void ChunkSync::Connect(uint32_t localId, uint32_t peerId) {
  Disconnect();
  localId_ = localId;
  peerId_ = peerId;
  connected_ = true;
}

void ChunkSync::Disconnect() {
  connected_ = false;
  // EVERY LABEL AND EVERY REQUEST BELONGS TO THE CONNECTION THAT MADE IT. A
  // rejoin restarts the host's clock (protocol.h), so a tick number from the
  // previous session names a different moment; and a half-reassembled chunk
  // from a peer that is gone must never be installed.
  lastPublished_ = 0;
  everPublished_ = false;
  ownRing_.clear();
  peerRing_.clear();
  wanted_.clear();
  inFlight_.clear();
  outbox_.clear();
  pending_.clear();
  chunkMem_.clear();
}

void ChunkSync::NoteOwnTick(uint32_t tick, IVec3 origin, IVec3 playerChunk) {
  ownRing_[tick] = {origin, playerChunk};
  while (ownRing_.size() > kOriginRingTicks) ownRing_.erase(ownRing_.begin());
}

void ChunkSync::NotePeerState(uint32_t tick, IVec3 origin, IVec3 playerChunk) {
  peerRing_[tick] = {origin, playerChunk};
  while (peerRing_.size() > kOriginRingTicks) peerRing_.erase(peerRing_.begin());
}

bool ChunkSync::OriginsAt(uint32_t tick, IVec3& own, IVec3& peer) const {
  const auto a = ownRing_.find(tick);
  const auto b = peerRing_.find(tick);
  if (a == ownRing_.end() || b == peerRing_.end()) return false;
  own = a->second.origin;
  peer = b->second.origin;
  return true;
}

std::vector<PeerView> ChunkSync::Peers(IVec3 own, IVec3 peer) const {
  // The authority arithmetic reads only what a PlayerState carries
  // (authority.h): both players' feet chunk and both window origins. The feet
  // chunks come from the same ring entries the origins did, so the whole query
  // is "as of the hash tick" and not a mix of two moments.
  std::vector<PeerView> v;
  v.push_back({localId_, own, own, true});
  v.push_back({peerId_, peer, peer, true});
  return v;
}

void ChunkSync::QueueWant(IVec3 wc, uint32_t hashTick) {
  const uint64_t k = World::PackChunkKey(wc);
  if (inFlight_.count(k)) return;  // already asked
  for (const Want& w : wanted_)
    if (World::PackChunkKey(w.wc) == k) return;
  wanted_.push_back({wc, hashTick});
}

void ChunkSync::IssueRequests(const World& world, uint32_t nowTick,
                              std::vector<Out>& out) {
  if (wanted_.empty()) return;
  // NEAREST TO THE LOCAL PLAYER FIRST. A divergence under the player's feet is
  // one they can walk into this second; one at the window's edge is fourteen
  // chunks of travel away. With a cap of four in flight the ORDER is the whole
  // policy, so it is a sort and not a heuristic.
  const IVec3 me = ownRing_.empty() ? IVec3{0, 0, 0}
                                    : ownRing_.rbegin()->second.chunk;
  std::sort(wanted_.begin(), wanted_.end(),
            [&](const Want& a, const Want& b) {
              const int da = ChunkChebyshev(a.wc, me);
              const int db = ChunkChebyshev(b.wc, me);
              if (da != db) return da < db;
              // Deterministic tie-break, so two runs of the same state issue
              // the same requests in the same order.
              return World::PackChunkKey(a.wc) < World::PackChunkKey(b.wc);
            });
  while (!wanted_.empty() && inFlight_.size() < kMaxChunksInFlight) {
    const Want w = wanted_.front();
    wanted_.erase(wanted_.begin());
    if (!world.ChunkInWindow(w.wc)) continue;  // the window moved; re-detect
    ChunkRequestMsg m;
    m.hashTick = w.hashTick;
    m.wc[0] = w.wc.x;
    m.wc[1] = w.wc.y;
    m.wc[2] = w.wc.z;
    Out o{MsgType::ChunkRequest, {}};
    m.Encode(o.payload);
    out.push_back(std::move(o));
    Inbound f;
    f.askedTick = nowTick;
    inFlight_[World::PackChunkKey(w.wc)] = f;
    requestsSent++;
  }
}

void ChunkSync::SendBusy(IVec3 wc, uint32_t reason, std::vector<Out>& out) {
  ChunkBusyMsg m;
  m.wc[0] = wc.x;
  m.wc[1] = wc.y;
  m.wc[2] = wc.z;
  m.reason = reason;
  Out o{MsgType::ChunkBusy, {}};
  m.Encode(o.payload);
  out.push_back(std::move(o));
  busySent++;
}

void ChunkSync::ShipChunk(const World& world, IVec3 wc, const uint32_t* words,
                          uint32_t nowTick, std::vector<Out>& out) {
  // THE SAME ENCODER THE SAVE FORMAT AND THE EVICTION PATH USE (stream.h). Not
  // a second encoding: the receiver decodes with RleDecodeChunk, which is what
  // FillSlots's store-hit branch feeds ReplaceChunk, so the resync's bytes
  // travel the identical road a refill's do. `kPersistMask` strips the tick
  // stamp on the way in (RleEncodeChunk does it), and the decode re-stamps
  // kStampNever, which is exactly right for a chunk arriving from elsewhere:
  // it has never acted on THIS machine's clock.
  std::vector<uint32_t> rle;
  RleEncodeChunk(words, rle);
  const size_t total = rle.size() * sizeof(uint32_t);
  const uint32_t slices =
      (uint32_t)((total + kChunkSyncSliceBytes - 1) / kChunkSyncSliceBytes);
  const uint8_t* raw = (const uint8_t*)rle.data();
  for (uint32_t s = 0; s < slices; s++) {
    const size_t off = (size_t)s * kChunkSyncSliceBytes;
    const size_t len = std::min(kChunkSyncSliceBytes, total - off);
    ChunkSyncMsg m;
    m.tick = nowTick;
    m.wc[0] = wc.x;
    m.wc[1] = wc.y;
    m.wc[2] = wc.z;
    m.slice = s;
    m.slices = slices;
    m.pairs = (uint32_t)(rle.size() / 2);
    m.bytes.assign(raw + off, raw + off + len);
    Out o{MsgType::ChunkSync, {}};
    m.Encode(o.payload);
    out.push_back(std::move(o));
    slicesSent++;
    bytesShipped += len;
  }
  (void)world;
}

void ChunkSync::Pump(World& world, uint32_t nowTick, std::vector<Out>& out) {
  if (!connected_) return;

  // ---- 1. PUBLISH, ONCE PER DIGEST TABLE ---------------------------------
  //
  // The trigger is the TABLE advancing, not a tick schedule. `ChunkHashTick()`
  // only moves when a full occupancy pass has been read back, which is every
  // 15th tick plus the readback ring's K ticks of latency — and the frame loop
  // has no way to know which frame that lands on. Publishing on "the table
  // moved" makes the schedule the GPU's, which is the only clock that knows
  // when the digests are real.
  const uint32_t ht = world.ChunkHashTick();
  if (ht != 0 && (!everPublished_ || ht != lastPublished_)) {
    IVec3 own{}, peer{};
    // No origins for that tick means the peer's state for it has not arrived
    // (or has aged out of the ring). SKIP the publish rather than guess: a
    // comparison run against the wrong origin reports a divergence that is
    // really a window edge, and a skipped publish costs one hash period.
    if (OriginsAt(ht, own, peer)) {
      HashBlocksMsg m;
      m.hashTick = ht;
      m.origin[0] = own.x;
      m.origin[1] = own.y;
      m.origin[2] = own.z;
      HashTree::Build(world, own, peer, m.blocks);
      Out o{MsgType::HashBlocks, {}};
      m.Encode(o.payload);
      out.push_back(std::move(o));
      hashBlocksSent++;
      blockEntriesSent += m.blocks.size();
      lastPublished_ = ht;
      everPublished_ = true;
    }
  }

  // ---- 2. SERVICE THE OUTBOX (we are somebody's authority) ---------------
  //
  // A request was accepted, a fetch was pushed, and the cache lands 1-2 ticks
  // plus K later (world.h). Poll it here rather than blocking: a synchronous
  // readback on the frame path is forbidden (CLAUDE.md rule 3's last line) and
  // would stall the peer's tick batches behind it.
  for (size_t i = 0; i < outbox_.size();) {
    Outbox& ob = outbox_[i];
    const CachedChunk* cc = world.Cached(ob.wc);
    if (cc != nullptr && cc->voxels.size() == kChunkVol &&
        cc->version >= ob.askedTick) {
      ShipChunk(world, ob.wc, cc->voxels.data(), nowTick, out);
      outbox_.erase(outbox_.begin() + (long)i);
      continue;
    }
    if (nowTick - ob.askedTick > kRequestTimeoutTicks) {
      // The fetch never landed (the chunk streamed out, or the queue was
      // starved). Tell the requester so it stops waiting; it re-detects on the
      // next publish, which costs one hash period and no state.
      SendBusy(ob.wc, /*reason=*/0, out);
      outbox_.erase(outbox_.begin() + (long)i);
      continue;
    }
    // Re-ask every tick until it lands: RequestChunkFetch COALESCES duplicates
    // (world.h) and counts them, so this is a counter increment, not a queue
    // push, for every tick after the first.
    world.RequestChunkFetch(ob.wc, World::FetchSource::Other);
    i++;
  }

  // ---- 3. EXPIRE OUR OWN STALE REQUESTS ----------------------------------
  for (auto it = inFlight_.begin(); it != inFlight_.end();) {
    if (nowTick - it->second.askedTick > kRequestTimeoutTicks) {
      requestsExpired++;
      it = inFlight_.erase(it);
    } else {
      ++it;
    }
  }

  // ---- 4. ISSUE UP TO THE CAP --------------------------------------------
  IssueRequests(world, nowTick, out);
}

void ChunkSync::OnMessage(MsgType t, const uint8_t* p, size_t n, World& world,
                          uint32_t nowTick, std::vector<Out>& out) {
  if (!connected_) return;
  switch (t) {
    // ---- the peer published; compare -------------------------------------
    case MsgType::HashBlocks: {
      HashBlocksMsg m;
      if (!m.Decode(p, n)) return;
      hashBlocksRecv++;
      IVec3 own{}, peer{};
      if (!OriginsAt(m.hashTick, own, peer)) {
        // We cannot reconstruct which chunks were comparable at that tick, so
        // we cannot reproduce the sender's block SELECTION either — and a
        // comparison over two different selections is not a comparison. Record
        // the point with zero compared so the series says "we skipped one"
        // rather than "there was nothing wrong".
        series.push_back({m.hashTick, 0, 0});
        return;
      }
      uint32_t compared = 0, bad = 0;
      uint32_t dig[kChunksPerBlock];
      for (const HashBlockEntry& e : m.blocks) {
        const IVec3 b{e.bx, e.by, e.bz};
        // Our own side of the all-or-nothing rule. A block the peer could
        // publish and we cannot is not a mismatch; it is a block one of us has
        // activity in, and it will be compared on a later publish.
        if (!HashTree::BlockDigests(world, b, own, peer, dig)) continue;
        compared++;
        uint32_t sum = 0;
        for (uint32_t i = 0; i < kChunksPerBlock; i++) sum += dig[i];
        if (sum == e.sum) continue;
        bad++;
        HashDrillMsg d;
        d.hashTick = m.hashTick;
        d.b[0] = b.x;
        d.b[1] = b.y;
        d.b[2] = b.z;
        Out o{MsgType::HashDrill, {}};
        d.Encode(o.payload);
        out.push_back(std::move(o));
        drillsSent++;
      }
      blockEntriesCompared += compared;
      blockMismatches += bad;
      series.push_back({m.hashTick, bad, compared});
      return;
    }
    // ---- somebody wants the 64 --------------------------------------------
    case MsgType::HashDrill: {
      HashDrillMsg m;
      if (!m.Decode(p, n)) return;
      drillsRecv++;
      IVec3 own{}, peer{};
      if (!OriginsAt(m.hashTick, own, peer)) return;
      HashChunksMsg r;
      r.hashTick = m.hashTick;
      r.b[0] = m.b[0];
      r.b[1] = m.b[1];
      r.b[2] = m.b[2];
      // If the block is no longer wholly syncable here the reply would be a
      // fold over a different set, so say nothing. The requester's own
      // in-flight bookkeeping has no entry for a drill, so a dropped reply
      // costs one hash period and leaks nothing.
      if (!HashTree::BlockDigests(world, {m.b[0], m.b[1], m.b[2]}, own, peer,
                                  r.digest))
        return;
      Out o{MsgType::HashChunks, {}};
      r.Encode(o.payload);
      out.push_back(std::move(o));
      return;
    }
    // ---- the 64 came back; name the chunks --------------------------------
    case MsgType::HashChunks: {
      HashChunksMsg m;
      if (!m.Decode(p, n)) return;
      IVec3 own{}, peer{};
      if (!OriginsAt(m.hashTick, own, peer)) return;
      const IVec3 b{m.b[0], m.b[1], m.b[2]};
      uint32_t mine[kChunksPerBlock];
      if (!HashTree::BlockDigests(world, b, own, peer, mine)) return;
      const std::vector<PeerView> peers = Peers(own, peer);
      for (uint32_t i = 0; i < kChunksPerBlock; i++) {
        if (mine[i] == m.digest[i]) continue;
        chunkMismatches++;
        const IVec3 wc = HashTree::BlockChunk(b, i);
        // WHEN THE LOCAL MACHINE IS THE AUTHORITY, THE LOCAL COPY WINS and
        // nothing is requested. Both machines run this same pure function over
        // the same two PeerViews, so exactly one of them asks and exactly one
        // answers — there is no negotiation to fail, and no case where both
        // sides overwrite each other into a ping-pong.
        const uint32_t owner = ChunkAuthority(wc, peers, chunkMem_);
        if (owner != peerId_) {
          notMine++;
          continue;
        }
        QueueWant(wc, m.hashTick);
      }
      return;
    }
    // ---- we are the authority; answer if the chunk is quiet ---------------
    case MsgType::ChunkRequest: {
      ChunkRequestMsg m;
      if (!m.Decode(p, n)) return;
      requestsRecv++;
      const IVec3 wc{m.wc[0], m.wc[1], m.wc[2]};
      if (!world.ChunkInWindow(wc)) {
        SendBusy(wc, /*reason=*/0, out);
        return;
      }
      // THE QUIET GATE, CHECKED BY THE AUTHORITY AND NOT BY THE REQUESTER.
      // The requester's view of our quiet streak is a digest, not a streak —
      // it cannot know whether our copy is settled. This is the check that
      // earns the KNOWN GAP's argument (chunksync.h): a chunk with no dirty
      // flag for K + D published ticks has no in-flight particles, gas or MPM
      // matter in it, so shipping its voxel words is shipping all of it.
      if (world.QuietTicks(World::SlotChunkIndex(wc)) < kSyncQuietTicks) {
        SendBusy(wc, /*reason=*/1, out);
        return;
      }
      // Ask for the words and answer when they land (Pump step 2). The fetch
      // cache is the only door to a chunk's contents that does not stall the
      // frame, and it is the one the plan names.
      world.RequestChunkFetch(wc, World::FetchSource::Other);
      outbox_.push_back({wc, nowTick});
      return;
    }
    // ---- "not now" ---------------------------------------------------------
    case MsgType::ChunkBusy: {
      ChunkBusyMsg m;
      if (!m.Decode(p, n)) return;
      busyRecv++;
      // Drop the in-flight entry and DO NOT re-queue here. The next publish
      // re-detects the same mismatch (the chunk is still different) and the
      // want is created again from scratch — which is the retry the plan asks
      // for, without a retry counter that could spin on a chunk that will
      // never be quiet.
      inFlight_.erase(World::PackChunkKey({m.wc[0], m.wc[1], m.wc[2]}));
      return;
    }
    // ---- the authority's copy, one slice at a time -------------------------
    case MsgType::ChunkSync: {
      ChunkSyncMsg m;
      if (!m.Decode(p, n)) return;
      slicesRecv++;
      const IVec3 wc{m.wc[0], m.wc[1], m.wc[2]};
      const auto it = inFlight_.find(World::PackChunkKey(wc));
      // A slice for a chunk we are not waiting for is a late reply to a
      // request that expired, or a duplicate. Dropped: installing a chunk
      // nobody asked for would overwrite whatever the window holds there NOW.
      if (it == inFlight_.end()) return;
      Inbound& f = it->second;
      if (m.slices == 0 || m.slice >= m.slices) return;
      if (f.slices == 0) {
        f.slices = m.slices;
        f.pairs = m.pairs;
        f.bytes.assign((size_t)m.pairs * 2 * sizeof(uint32_t), 0u);
      }
      // A peer that changed its mind mid-transfer (a re-request crossing the
      // wire) would produce two different sizes; take the abort rather than
      // splicing two chunks together.
      if (f.slices != m.slices || f.pairs != m.pairs) {
        inFlight_.erase(it);
        return;
      }
      const size_t off = (size_t)m.slice * kChunkSyncSliceBytes;
      if (off + m.bytes.size() > f.bytes.size()) {
        inFlight_.erase(it);
        return;
      }
      std::memcpy(f.bytes.data() + off, m.bytes.data(), m.bytes.size());
      f.have++;
      if (f.have < f.slices) return;
      // COMPLETE. Decode to 4,096 words and hand it to phase B, which is the
      // only place allowed to install it (it has the Stream, and it runs
      // before stream.Update so the tick's CA sees the corrected chunk).
      PendingReplace pr;
      pr.wc = wc;
      pr.words.assign(kChunkVol, 0u);
      const bool okDecode = RleDecodeChunk((const uint32_t*)f.bytes.data(),
                                           f.pairs, pr.words.data());
      inFlight_.erase(it);
      if (okDecode) pending_.push_back(std::move(pr));
      return;
    }
    default:
      return;
  }
}

std::vector<ChunkSync::PendingReplace> ChunkSync::TakePending() {
  std::vector<PendingReplace> v;
  v.swap(pending_);
  return v;
}

}  // namespace net
