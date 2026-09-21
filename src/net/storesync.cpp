#include "net/storesync.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "sim/bytestream.h"

namespace net {
namespace {

// ---- the five payloads ----------------------------------------------------
//
// Every one of them opens with `kStoreSyncVersion` so a build skew is a
// dropped message with a counted reason rather than a misread coordinate.
// `IVec3` goes on the wire as three i32 and not as a Pod of the struct: the
// struct's layout is this build's business, and writing the three fields is
// the same three instructions with none of the padding risk.

void PutWc(ByteWriter& w, IVec3 wc) {
  w.Pod(wc.x);
  w.Pod(wc.y);
  w.Pod(wc.z);
}
bool GetWc(ByteReader& r, IVec3& wc) {
  return r.Pod(wc.x) && r.Pod(wc.y) && r.Pod(wc.z);
}

// A chunk blob: the RLE exactly as `ChunkStore` keeps it and the save format
// writes it (sim/stream.h `RleEncodeChunk`), so a chunk crossing the wire is
// never re-serialized anywhere in the engine.
void PutRle(ByteWriter& w, const std::vector<uint32_t>& rle) {
  w.PodVec(rle);
}
bool GetRle(ByteReader& r, std::vector<uint32_t>& rle) {
  return r.PodVec(rle);
}

bool ReadHeader(ByteReader& r) {
  uint32_t v = 0;
  return r.U32(v) && v == kStoreSyncVersion;
}

}  // namespace

// ---- lifecycle ------------------------------------------------------------

void StoreSync::Connect(Link* link, bool host, ChunkStore* store, uint32_t me,
                        uint32_t peer) {
  link_ = link;
  host_ = host;
  store_ = store;
  me_ = me;
  peer_ = peer;
  havePeer_ = false;
  // The manifest mirror is the HOST's state as of the last connection and it
  // means nothing now: a reconnect is answered with a fresh full manifest
  // (SendFullManifest below), and until it lands the client must not claim to
  // know what the host holds. Clearing it is what makes `ManifestReady`
  // honest across a rejoin.
  if (!host_) {
    manifest_.clear();
    manifestReady_ = false;
    manifestExpected_ = manifestGot_ = 0;
  } else {
    manifestReady_ = true;  // the host is its own manifest; nothing to wait for
  }
  // Nothing may be in flight on a link that did not exist a line ago. These
  // were already MISSED by Disconnect(), so dropping them here loses nothing.
  pendingGets_.clear();
  inFlight_.clear();
  chunkMem_.clear();

  // ---- THE RECONNECT'S ONE JOB (plan step 3) -----------------------------
  //
  // "the client's unacked puts persist in RAM for a rejoin in the same
  // process". This is that rejoin. Every chunk this machine evicted while it
  // was the authority, and whose arrival the host never confirmed, goes back
  // out — in one burst, because there are at most a few of them (a put is
  // acked within a round trip in the steady state, so the list is only
  // non-empty across a drop).
  if (!host_ && !unacked_.empty()) {
    for (const auto& [key, u] : unacked_) {
      std::vector<uint8_t> b;
      ByteWriter w{b};
      w.U32(kStoreSyncVersion);
      PutWc(w, u.wc);
      w.U32(u.tick);
      PutRle(w, u.rle);
      Send(MsgType::ChunkPut, b);
      c_.putsSent++;
      c_.reoffered++;
    }
  }
  c_.unacked = unacked_.size();
}

void StoreSync::SetDelivery(DeliverFn deliver, MissFn miss) {
  deliver_ = std::move(deliver);
  miss_ = std::move(miss);
}

void StoreSync::Disconnect() {
  // ---- A HELD SLOT WAITS FOREVER, SO A DISCONNECT MUST ANSWER ------------
  //
  // `Stream` holds a wanted slot INERT indefinitely (sim/stream.h): a
  // PT_EMPTY sentinel that reads as air, wakes nothing and is never
  // generated, until `DeliverRemote` or `DeliverMiss` lands. That is the
  // right behaviour while a peer exists — a few ticks of sky six chunks away
  // beats showing an edit being undone and redone. With the peer GONE it is
  // a permanent hole in the world that no future event can fill, because
  // nothing re-asks a held slot.
  //
  // So every outstanding request is missed here, which puts its slot back in
  // the gen list on the next `Stream::Update` and regenerates it from the
  // seed. Regenerated is worse than the peer's copy and infinitely better
  // than air.
  for (const auto& [key, wc] : inFlight_) {
    if (miss_) miss_(wc);
    c_.abandoned++;
  }
  for (IVec3 wc : pendingGets_) {
    if (miss_) miss_(wc);
    c_.abandoned++;
  }
  inFlight_.clear();
  pendingGets_.clear();
  link_ = nullptr;
  havePeer_ = false;
  chunkMem_.clear();
  // unacked_ and manifest_ are KEPT: see Connect's reconnect block. Neither
  // can do harm while disconnected — nothing consults the manifest without a
  // peer (Wanted returns false on `!link_`) and nothing sends.
  c_.unacked = unacked_.size();
  c_.manifestEntries = manifest_.size();
}

void StoreSync::SetPeers(const PeerView& mine, const PeerView* theirs) {
  mine_ = mine;
  if (theirs) {
    theirs_ = *theirs;
    havePeer_ = true;
  } else {
    havePeer_ = false;
  }
}

// ---- authority ------------------------------------------------------------

// ---- WHY EVICTION DOES NOT USE ChunkAuthority, AND MUST NOT --------------
//
// THIS IS THE BUG THAT WOULD HAVE MADE THE WHOLE PACKAGE A NO-OP, so it is
// written out in full.
//
// `ChunkAuthority` requires a candidate to hold the chunk RESIDENT WITH
// MARGIN (authority.h rule 1: "authority must be resident", §4 finding 6).
// That rule is right for the question it was written for — "who STEPS this
// chunk's mobs?" — because a machine without the chunk in memory cannot step
// anything in it.
//
// An EVICTION is the opposite moment. `OnEvicted` fires because the chunk has
// just LEFT the window: at the synchronous sentinel site it is on the plane
// the shift is discarding, and at the async `CompleteOldest` site it left
// several ticks ago and the origin has moved on. So `ResidentWithMargin(wc,
// myOrigin, 1)` is FALSE for every chunk this hook is ever called with, on
// every machine, always. Asking `ChunkAuthority(wc) == me` there returns
// `kNoAuthority` every single time: no chunk is ever put, no manifest row is
// ever written, and the gate would still pass because a gate can call
// `OnEvicted` with a view that has not moved.
//
// THE RULE THAT IS ACTUALLY WANTED is "is my copy the live one?", and at an
// eviction the answer is yes unless somebody else still has it:
//
//   I am the authority for these bytes UNLESS the peer still holds the chunk
//   resident (with margin, so its copy is a real simulation and not a window
//   edge) AND is nearer to it than I am.
//
// Both halves are load-bearing. Without the residency test a peer twenty
// chunks away would veto my put and the chunk would be lost by both. Without
// the distance test two overlapping windows would both stay silent, each
// deferring to the other. And when BOTH machines evict the same chunk in the
// same few ticks — which overlapping windows make possible — both put, and
// the tick tag arbitrates ("newer wins", `ChunkPut`'s handler). That is not a
// race: it is exactly what the tag is for.
bool StoreSync::IAmAuthority(IVec3 wc) const {
  // Alone in the world: my copy is the only copy, so it is the world's.
  if (!havePeer_) return true;
  if (!ResidentWithMargin(wc, theirs_.windowOrigin, kAuthorityMargin))
    return true;  // the peer does not have it; nobody else can speak for it
  // The peer has it AND is nearer: its copy is the live simulation and mine
  // is a cache of something it is still stepping. Chebyshev, because the
  // window is a cube — authority.h's `ChunkChebyshev` says why.
  return ChunkChebyshev(wc, theirs_.chunk) >= ChunkChebyshev(wc, mine_.chunk);
}

bool StoreSync::PeerIsAuthority(IVec3 wc) const {
  if (!havePeer_) return false;
  const PeerView views[2] = {mine_, theirs_};
  return ChunkAuthority(wc, std::span<const PeerView>(views, 2), chunkMem_) ==
         peer_;
}

// ---- ChunkExchange --------------------------------------------------------

void StoreSync::OnEvicted(IVec3 wc, uint32_t tick,
                          const std::vector<uint32_t>& rle) {
  if (!link_) return;
  // ONLY THE AUTHORITY SPEAKS (rule 1 of the header). `Stream` reports EVERY
  // eviction and deliberately does not know who owns the chunk; this is where
  // that decision is made. `IAmAuthority` is the EVICTION form of the rule
  // and not `ChunkAuthority` — the long comment at its definition says why
  // the two cannot be the same function. A machine that defers says nothing
  // and loses nothing: `Stream` Puts the bytes into its own store either way,
  // immediately after this returns, as a cache of something the other machine
  // is still simulating.
  if (!IAmAuthority(wc)) return;

  if (host_) {
    // THE HOST IS THE STORE, and the tag is the only thing this line adds.
    // `Stream` is about to Put these exact bytes one statement after this
    // returns — untagged, because M9.5-A left every existing Put caller's
    // signature alone. So the bytes are not in question; the TICK is, and an
    // untagged Put ERASES a tag (chunkstore.h). Writing the tag here and
    // nowhere else would therefore lose it immediately, which is why the
    // chunk is also queued for the deferred re-assert in `Pump` (the full
    // argument is there). The Put here is still worth doing: it is what makes
    // the tag correct for the window between now and that Pump, and it is
    // what the `store-sync` gate observes when it drives OnEvicted directly
    // with no Stream behind it.
    store_->Put(wc, rle, tick);
    pendingTags_.push_back({wc, tick});
    c_.putsLocal++;
    // ...and tell the client, one row. A full manifest per eviction would be
    // §4 finding 7's "1,024 ChunkGets per plane shift" in manifest clothing.
    std::vector<uint8_t> b;
    ByteWriter w{b};
    w.U32(kStoreSyncVersion);
    PutWc(w, wc);
    w.U32(tick);
    Send(MsgType::ManifestDelta, b);
    c_.manifestSent++;
    return;
  }

  // THE CLIENT ASKS. Kept until acked; a second eviction of the same chunk
  // REPLACES the first (see Unacked's declaration).
  SendPut(wc, tick, rle);
}

void StoreSync::SendPut(IVec3 wc, uint32_t tick,
                        const std::vector<uint32_t>& rle) {
  const uint64_t key = World::PackChunkKey(wc);
  Unacked& u = unacked_[key];
  u.wc = wc;
  u.tick = tick;
  u.rle = rle;
  c_.unacked = unacked_.size();

  std::vector<uint8_t> b;
  ByteWriter w{b};
  w.U32(kStoreSyncVersion);
  PutWc(w, wc);
  w.U32(tick);
  PutRle(w, rle);
  Send(MsgType::ChunkPut, b);
  c_.putsSent++;
  if (everPut_.insert(key).second) c_.putsDistinct++;
}

bool StoreSync::Wanted(IVec3 wc) {
  // No peer, no opinion: with nobody to ask, procgen is the only answer and
  // holding the slot would be holding it forever.
  if (!link_ || !havePeer_) return false;

  if (host_) {
    // THE HOST'S QUESTION IS ARITHMETIC, NOT A TABLE. My store missed, so
    // nothing I have ever been told about this chunk is relevant; what
    // matters is whether the CLIENT is the machine that owns it and is
    // therefore the machine whose copy is the live one. `ChunkAuthority`
    // already refuses a candidate that does not hold the chunk with margin
    // (authority.h rule 1), so "is the peer the authority" IS "does the peer
    // have it resident, with slack" — there is no second test to write, and
    // writing one would be a second copy of the residency rule to keep in
    // step with the first.
    if (!PeerIsAuthority(wc)) return false;
    c_.wanted++;
    return true;
  }

  // THE CLIENT'S QUESTION IS THE MIRROR. A manifest hit means the host holds
  // bytes for this chunk that are not procgen — somebody edited it and it was
  // evicted — and `Stream` only asks after its OWN store missed, so the tag
  // comparison below is against 0 in every call the engine makes today. It is
  // written out anyway because it is the RULE ("a tag newer than my store's",
  // plan step 2) and the day a caller asks with a copy in hand it has to be
  // right then too.
  //
  // A manifest row is not required to carry a non-zero tick: `ChunkStore::
  // Manifest` reports 0 for a chunk stored before M9.5 or by an untagged
  // Put, and those chunks are still real edits. Hence `>=` and not `>`.
  const auto it = manifest_.find(World::PackChunkKey(wc));
  if (it == manifest_.end()) return false;
  if (it->second < store_->TickOf(wc)) return false;
  c_.wanted++;
  return true;
}

void StoreSync::Request(IVec3 wc) {
  // QUEUED, NOT SENT. `Request` is called from inside `Stream::Update`, once
  // per wanted slot, and a window shift refills a 1,024-chunk plane in one
  // call — so the send site is `Pump`, where the in-flight cap can bound it
  // (kMaxInFlightGets). A held slot is patient by construction, which is what
  // makes deferring free.
  if (!link_) {
    if (miss_) miss_(wc);
    return;
  }
  const uint64_t key = World::PackChunkKey(wc);
  if (inFlight_.count(key)) return;  // already asked; one answer is enough
  pendingGets_.push_back(wc);
}

// ---- the wire -------------------------------------------------------------

void StoreSync::Send(MsgType t, const std::vector<uint8_t>& b) {
  if (!link_) return;
  link_->Send((uint16_t)t, kProtocolVersion, b.data(), b.size());
}

size_t StoreSync::SendFullManifest() {
  if (!link_ || !host_ || !store_) return 0;
  std::vector<std::pair<IVec3, uint32_t>> rows;
  store_->Manifest(rows);
  const uint32_t total = (uint32_t)rows.size();
  // ONE MESSAGE EVEN WHEN EMPTY. A fresh host has stored nothing, and the
  // client's `ManifestReady` has to become true anyway or the late-join
  // ReloadWindow below would wait for a slice that is never coming. An empty
  // manifest is a real and common answer, not the absence of one.
  uint32_t first = 0;
  do {
    const uint32_t count = std::min<uint32_t>(kManifestRowsPerSlice, total - first);
    std::vector<uint8_t> b;
    ByteWriter w{b};
    w.U32(kStoreSyncVersion);
    w.U32(total);
    w.U32(first);
    w.U32(count);
    for (uint32_t i = 0; i < count; i++) {
      PutWc(w, rows[first + i].first);
      w.U32(rows[first + i].second);
    }
    Send(MsgType::ChunkManifest, b);
    c_.manifestSent += count;
    first += count;
  } while (first < total);
  return total;
}

void StoreSync::AnswerGet(IVec3 wc) {
  // THE SYMMETRIC HALF. Both roles run this: the client answers the host's
  // pull of a chunk the client owns out of the client's own store, and the
  // host answers the client's pull out of the truth. Identical code, which is
  // the whole argument for having no relay in a two-peer world (header).
  const std::vector<uint32_t>* rle = store_ ? store_->Get(wc) : nullptr;
  std::vector<uint8_t> b;
  ByteWriter w{b};
  w.U32(kStoreSyncVersion);
  PutWc(w, wc);
  if (!rle) {
    Send(MsgType::ChunkMiss, b);
    c_.missesSent++;
    return;
  }
  w.U32(store_->TickOf(wc));
  PutRle(w, *rle);
  Send(MsgType::ChunkData, b);
  c_.dataSent++;
}

bool StoreSync::OnMessage(MsgType t, const uint8_t* p, size_t n) {
  ByteReader r{p, n};
  switch (t) {
    // ---- HOST INBOX: the client's eviction ------------------------------
    case MsgType::ChunkPut: {
      IVec3 wc{};
      uint32_t tick = 0;
      std::vector<uint32_t> rle;
      if (!ReadHeader(r) || !GetWc(r, wc) || !r.U32(tick) || !GetRle(r, rle))
        return true;  // malformed: dropped whole, never half-applied
      // NEWER WINS, AND OLDER IS STILL ACKED. The host's tag is the world's
      // opinion of how fresh the chunk is; a put that lost the race resolves
      // the client's obligation exactly as completely as one that won, and
      // NOT acking it would leave the client re-offering stale bytes on every
      // reconnect for the rest of the session.
      if (store_) {
        if (tick >= store_->TickOf(wc)) {
          store_->Put(wc, std::move(rle), tick);
          c_.putsRecv++;
        } else {
          c_.putsRefusedOld++;
        }
      }
      std::vector<uint8_t> b;
      ByteWriter w{b};
      w.U32(kStoreSyncVersion);
      PutWc(w, wc);
      w.U32(tick);
      Send(MsgType::ChunkPutAck, b);
      return true;
    }
    // ---- CLIENT INBOX: it landed ----------------------------------------
    case MsgType::ChunkPutAck: {
      IVec3 wc{};
      uint32_t tick = 0;
      if (!ReadHeader(r) || !GetWc(r, wc) || !r.U32(tick)) return true;
      const uint64_t key = World::PackChunkKey(wc);
      const auto it = unacked_.find(key);
      // TICK-MATCHED, not merely chunk-matched. The client may have evicted
      // the same chunk a second time while the first ack was in flight; that
      // replaced the entry (see Unacked), and erasing on the OLD ack would
      // forget a put that is still owed.
      if (it != unacked_.end() && it->second.tick == tick) {
        unacked_.erase(it);
        c_.putsAcked++;
      }
      // The host now holds it at this tag, so the mirror should say so —
      // otherwise the client would re-ask for its own bytes after a window
      // round trip.
      manifest_[key] = tick;
      c_.unacked = unacked_.size();
      c_.manifestEntries = manifest_.size();
      return true;
    }
    // ---- CLIENT INBOX: the full manifest, in slices ----------------------
    case MsgType::ChunkManifest: {
      uint32_t total = 0, first = 0, count = 0;
      if (!ReadHeader(r) || !r.U32(total) || !r.U32(first) || !r.U32(count))
        return true;
      // A slice that starts at 0 is a NEW manifest, not a continuation: a
      // reconnect re-sends the whole thing and the old mirror described a
      // store the host may since have grown.
      if (first == 0) {
        manifest_.clear();
        manifestGot_ = 0;
        manifestExpected_ = total;
        manifestReady_ = false;
      }
      for (uint32_t i = 0; i < count; i++) {
        IVec3 wc{};
        uint32_t tick = 0;
        if (!GetWc(r, wc) || !r.U32(tick)) return true;  // truncated: stop
        manifest_[World::PackChunkKey(wc)] = tick;
        c_.manifestRecv++;
      }
      manifestGot_ += count;
      if (manifestGot_ >= manifestExpected_) manifestReady_ = true;
      c_.manifestEntries = manifest_.size();
      return true;
    }
    // ---- CLIENT INBOX: one row ------------------------------------------
    case MsgType::ManifestDelta: {
      IVec3 wc{};
      uint32_t tick = 0;
      if (!ReadHeader(r) || !GetWc(r, wc) || !r.U32(tick)) return true;
      manifest_[World::PackChunkKey(wc)] = tick;
      c_.manifestRecv++;
      c_.manifestEntries = manifest_.size();
      return true;
    }
    // ---- EITHER INBOX: somebody wants a chunk I may have ----------------
    case MsgType::ChunkGet: {
      IVec3 wc{};
      if (!ReadHeader(r) || !GetWc(r, wc)) return true;
      c_.getsRecv++;
      AnswerGet(wc);
      return true;
    }
    // ---- EITHER INBOX: the answer ---------------------------------------
    case MsgType::ChunkData: {
      IVec3 wc{};
      uint32_t tick = 0;
      std::vector<uint32_t> rle;
      if (!ReadHeader(r) || !GetWc(r, wc) || !r.U32(tick) || !GetRle(r, rle))
        return true;
      inFlight_.erase(World::PackChunkKey(wc));
      c_.dataRecv++;
      // Straight through to the delivery hook, which in the game is
      // `Stream::DeliverRemote` — it stores the bytes TAGGED (so a later
      // re-entry needs no second round trip) and installs them through the
      // M9.3-C replace door. Nothing here decodes a voxel.
      if (deliver_) deliver_(wc, tick, rle);
      return true;
    }
    case MsgType::ChunkMiss: {
      IVec3 wc{};
      if (!ReadHeader(r) || !GetWc(r, wc)) return true;
      inFlight_.erase(World::PackChunkKey(wc));
      c_.missesRecv++;
      if (miss_) miss_(wc);
      return true;
    }
    default:
      return false;
  }
}

void StoreSync::Pump() {
  if (!link_) return;
  // ---- THE HOST'S DEFERRED RE-TAG --------------------------------------
  //
  // `Stream::OnEvicted` fires immediately BEFORE `store_.Put(wc, rle)` —
  // untagged, because M9.5-A deliberately left every existing caller's Put
  // signature alone and `Stream` does not know a tick tag is wanted. An
  // untagged Put DROPS an existing tag (chunkstore.h says why: "unknown" is
  // the honest answer for bytes whose freshness nobody vouched for), so the
  // tag this object wrote inside OnEvicted is erased one line later, every
  // time, and the host's store would end a session with zero tagged chunks.
  //
  // MEASURED CONSEQUENCE IF THIS IS MISSING: the smoke's saved world has the
  // client's puts tagged (those go through ChunkPut and never meet Stream's
  // Put) and every chunk the HOST evicted untagged — so `manifest.svt` under-
  // reports the world by exactly the host's own edits.
  //
  // Re-asserting on the next Pump is correct rather than merely convenient:
  // Pump runs once a frame from the frame loop, strictly after the tick that
  // did the eviction, so Stream's untagged Put has certainly landed. Re-Put
  // is not needed — only the tag — and the tag is re-applied by Putting the
  // bytes the store already holds back with the tick. `Get` returns a pointer
  // into the region map, so the copy is one vector; the list is at most one
  // window plane long and is usually empty.
  if (host_ && store_ && !pendingTags_.empty()) {
    for (const auto& [wc, tick] : pendingTags_) {
      const std::vector<uint32_t>* cur = store_->Get(wc);
      if (!cur) continue;  // evicted from the store entirely; nothing to tag
      std::vector<uint32_t> copy = *cur;
      store_->Put(wc, std::move(copy), tick);
    }
    pendingTags_.clear();
  }

  // ...then the queued gets, bounded.
  while (!pendingGets_.empty() && inFlight_.size() < kMaxInFlightGets) {
    const IVec3 wc = pendingGets_.front();
    pendingGets_.pop_front();
    const uint64_t key = World::PackChunkKey(wc);
    if (inFlight_.count(key)) continue;
    inFlight_[key] = wc;
    std::vector<uint8_t> b;
    ByteWriter w{b};
    w.U32(kStoreSyncVersion);
    PutWc(w, wc);
    Send(MsgType::ChunkGet, b);
    c_.getsSent++;
  }
}

uint32_t StoreSync::ManifestTickOf(IVec3 wc) const {
  const auto it = manifest_.find(World::PackChunkKey(wc));
  return it == manifest_.end() ? 0u : it->second;
}

}  // namespace net
