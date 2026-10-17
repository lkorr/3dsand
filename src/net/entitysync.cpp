#include "net/entitysync.h"

#include <algorithm>
#include <cmath>

#include "game/mob.h"
#include "game/worlditems.h"
#include "phys/debris.h"
#include "sim/bytestream.h"

namespace net {

// ============================================================================
// THE ENVELOPE'S CODEC
// ============================================================================
//
// Nothing clever: a version, the label, the sender, then each vector as a
// count followed by that many records through the codecs packages B and C
// already own. The two families have different signatures — mobsync takes a
// `ByteWriter&`, debrissync takes the `std::vector<uint8_t>&` directly —
// because they were written a day apart by different packages. Both write to
// the SAME vector (`ByteWriter` is a reference wrapper, sim/bytestream.h), so
// the mismatch costs one local and no copy, and neither header had to move.

void EntityBatch::Clear() {
  mobHandoffs.clear();
  mobAnnounces.clear();
  mobStates.clear();
  mobPoses.clear();
  mobGones.clear();
  bodyHandoffs.clear();
  bodyAnnounces.clear();
  bodyPoses.clear();
  bodyGones.clear();
  itemTakes.clear();
  itemGrants.clear();
  meleeHits.clear();
  playerBodies.clear();
  distantHandOuts.clear();
  distantDones.clear();
  distantHandIns.clear();
  distantView.entries.clear();
  distantView.gones.clear();
  livingFree = kNoLivingFree;
  distantEdge.Clear();
  tick = 0;
  playerId = 0;
}

void EntityBatch::Encode(std::vector<uint8_t>& out) const {
  ByteWriter w{out};
  w.U32(kEntityBatchVersion);
  w.U32(tick);
  w.U32(playerId);
  // The apply order (see the header): handoffs, announces, poses, gones, then
  // the item pair. Written in that order so a DECODER that reads the fields in
  // declaration order and an APPLIER that walks them in declaration order
  // cannot disagree — there is no separate ordering rule to keep in sync.
  w.U32((uint32_t)mobHandoffs.size());
  for (const MobHandoff& r : mobHandoffs) ::net::Encode(w, r);
  w.U32((uint32_t)mobAnnounces.size());
  for (const MobAnnounce& r : mobAnnounces) ::net::Encode(w, r);
  w.U32((uint32_t)mobStates.size());
  for (const MobState& r : mobStates) ::net::Encode(w, r);
  w.U32((uint32_t)mobPoses.size());
  for (const MobPose& r : mobPoses) ::net::Encode(w, r);
  w.U32((uint32_t)mobGones.size());
  for (const MobGone& r : mobGones) ::net::Encode(w, r);

  w.U32((uint32_t)bodyHandoffs.size());
  for (const BodyHandoff& r : bodyHandoffs) ::net::Encode(out, r);
  w.U32((uint32_t)bodyAnnounces.size());
  for (const BodyAnnounce& r : bodyAnnounces) ::net::Encode(out, r);
  w.U32((uint32_t)bodyPoses.size());
  for (const BodyPose& r : bodyPoses) ::net::Encode(out, r);
  w.U32((uint32_t)bodyGones.size());
  for (const BodyGone& r : bodyGones) ::net::Encode(out, r);

  w.U32((uint32_t)itemTakes.size());
  for (const ItemTake& r : itemTakes) ::net::Encode(out, r);
  w.U32((uint32_t)itemGrants.size());
  for (const ItemGrant& r : itemGrants) ::net::Encode(out, r);

  w.U32((uint32_t)meleeHits.size());
  for (const MeleeHit& r : meleeHits) EncodeMeleeHit(w, r);

  w.U32((uint32_t)playerBodies.size());
  for (const PlayerBody& r : playerBodies) ::net::Encode(w, r);

  // v5 (D7a): the distant tier across machines, in apply order.
  w.U32((uint32_t)distantHandOuts.size());
  for (const DistantHandOut& r : distantHandOuts) ::net::Encode(w, r);
  w.U32((uint32_t)distantDones.size());
  for (const DistantHandInDone& r : distantDones) ::net::Encode(w, r);
  w.U32((uint32_t)distantHandIns.size());
  for (const DistantHandIn& r : distantHandIns) ::net::Encode(w, r);
  ::net::Encode(w, distantView);
  w.U32(livingFree);
  // v6 (D7b): the edge across the peer's window.
  ::net::Encode(w, distantEdge);
}

namespace {
// A count that a short or hostile payload cannot turn into a 4-billion-entry
// reserve. Nothing legitimate approaches it: the interest set is bounded by
// the peer's window and `kMaxBodies` is in the low thousands.
constexpr uint32_t kMaxRecordsPerKind = 65536;

template <typename T, typename F>
bool ReadVec(::ByteReader& r, std::vector<T>& v, F&& decodeOne) {
  uint32_t n = 0;
  if (!r.U32(n) || n > kMaxRecordsPerKind) {
    r.ok = false;
    return false;
  }
  v.resize(n);
  for (uint32_t i = 0; i < n; i++)
    if (!decodeOne(r, v[i])) return false;
  return true;
}
}  // namespace

bool EntityBatch::Decode(const uint8_t* p, size_t n) {
  Clear();
  ::ByteReader r{p, n};
  uint32_t ver = 0;
  if (!r.U32(ver) || ver != kEntityBatchVersion) return false;
  if (!r.U32(tick) || !r.U32(playerId)) return false;
  auto mobRec = [](auto& rr, auto& rec) { return ::net::Decode(rr, rec); };
  auto bodyRec = [](auto& rr, auto& rec) { return ::net::Decode(rr, rec); };
  if (!ReadVec(r, mobHandoffs, mobRec)) return false;
  if (!ReadVec(r, mobAnnounces, mobRec)) return false;
  if (!ReadVec(r, mobStates, mobRec)) return false;
  if (!ReadVec(r, mobPoses, mobRec)) return false;
  if (!ReadVec(r, mobGones, mobRec)) return false;
  if (!ReadVec(r, bodyHandoffs, bodyRec)) return false;
  if (!ReadVec(r, bodyAnnounces, bodyRec)) return false;
  if (!ReadVec(r, bodyPoses, bodyRec)) return false;
  if (!ReadVec(r, bodyGones, bodyRec)) return false;
  if (!ReadVec(r, itemTakes, bodyRec)) return false;
  if (!ReadVec(r, itemGrants, bodyRec)) return false;
  auto hitRec = [](::ByteReader& rr, MeleeHit& rec) {
    return DecodeMeleeHit(rr, rec);
  };
  if (!ReadVec(r, meleeHits, hitRec)) return false;
  if (!ReadVec(r, playerBodies, mobRec)) return false;
  // v5 (D7a).
  auto distRec = [](auto& rr, auto& rec) { return ::net::Decode(rr, rec); };
  if (!ReadVec(r, distantHandOuts, distRec)) return false;
  if (!ReadVec(r, distantDones, distRec)) return false;
  if (!ReadVec(r, distantHandIns, distRec)) return false;
  if (!::net::Decode(r, distantView)) return false;
  r.U32(livingFree);
  // v6 (D7b).
  if (!::net::Decode(r, distantEdge)) return false;
  return r.ok;
}

// ============================================================================
// LIFETIME
// ============================================================================

void EntitySync::Connect(uint32_t me, uint32_t peer) {
  connected_ = true;
  me_ = me;
  peer_ = peer;
  // EVERYTHING ELSE IS THIS CONNECTION'S AND NO OTHER. An incumbent from a
  // previous session names a playerId that is not on the wire any more; an
  // "announced" id refers to a peer that never heard it; a queued batch names
  // a label the host's restarted clock will reuse for a different tick.
  mobMem_.clear();
  bodyMem_.clear();
  bodyChunkMem_.clear();
  chunkMem_.clear();
  // The closures capture `this` and name THIS connection's ids, so they are
  // re-installed rather than inherited from the last one.
  ownershipBound_ = false;
  announcedMobs_.clear();
  announcedBodies_.clear();
  mobPoseSent_.clear();
  mobStateSent_.clear();
  mobGearSent_.clear();
  ghostMobSeen_.clear();
  ghostBodySeen_.clear();
  pendingMobHandoffs_.clear();
  pendingBodyHandoffs_.clear();
  inbox_.clear();
  localGrants_.clear();
  gearKeySent_ = 0;
  gearLabelSent_ = 0;
  gearEverSent_ = false;
  havePeerBody_ = false;
  peerCount_ = 0;
  // D7a: a view a previous peer was sent, a room it reported, a report this
  // side made: none of it describes this connection.
  viewSent_.clear();
  peerLivingFree_ = 0;
  havePeerFree_ = false;
  freeSent_ = kNoLivingFree;
  freeLabel_ = 0;
}

void EntitySync::Disconnect() {
  connected_ = false;
  peerCount_ = 0;
  inbox_.clear();
  pendingMobHandoffs_.clear();
  pendingBodyHandoffs_.clear();
  announcedMobs_.clear();
  announcedBodies_.clear();
  ghostMobSeen_.clear();
  ghostBodySeen_.clear();
  mobMem_.clear();
  bodyMem_.clear();
  bodyChunkMem_.clear();
  chunkMem_.clear();
  // D7a: the view's memory and the room reports go with the peer (the
  // caller's MobSystem::DistantNetDisconnect restores what was in transit).
  viewSent_.clear();
  peerLivingFree_ = 0;
  havePeerFree_ = false;
  freeSent_ = kNoLivingFree;
  freeLabel_ = 0;
  // NOT unbound here: the caller replaces the two closures with an
  // "everything is mine" pair (main.cpp's netDrop), and clearing the flag is
  // what lets a later Connect install ours again.
  ownershipBound_ = false;
  // `localGrants_` is deliberately KEPT: a grant that arrived before the peer
  // dropped is an item that is already gone from the peer's world, and
  // throwing it away here would destroy the sword on both machines.
}

void EntitySync::SetPeers(const PeerView& mine, const PeerView* peer) {
  peers_[0] = mine;
  if (peer != nullptr) {
    peers_[1] = *peer;
    peerCount_ = 2;
  } else {
    peerCount_ = 1;
  }
}

// ============================================================================
// THE TWO OWNERSHIP CLOSURES
// ============================================================================
//
// QUERY THEN COMMIT, which is authority.h's separation: the query is pure and
// the `Remember` is what moves the hysteresis boundary. These are the only
// callers that commit, so "who owns this" is decided exactly once per entity
// per tick no matter how many places ask.

uint32_t EntitySync::MobOwner(uint64_t mobId, Vec3 feetVox) {
  if (peerCount_ < 2) return me_;   // no second candidate: everything is mine
  const uint32_t who = EntityAuthority(mobId, feetVox, Peers(), mobMem_);
  // kNoAuthority means NOBODY has the chunk with margin. For a creature that
  // is a real answer but not a usable one — somebody has to step it or it
  // freezes mid-stride — and the machine that currently holds it is the only
  // one that can. So an unowned mob stays where it is rather than being
  // orphaned, and the incumbent is left alone so the boundary does not move
  // on an answer nobody won.
  if (who == kNoAuthority) {
    const auto it = mobMem_.find(mobId);
    return it != mobMem_.end() ? it->second : me_;
  }
  Remember(mobMem_, mobId, who);
  return who;
}

// ---- A BODY, KEYED ON THE BODY (M9.4-E) ------------------------------------
//
// M9.4-C's `SetOwnershipFn` passed a position and nothing else, so this had to
// key its incumbent on the CHUNK — and a chunk-keyed incumbent is ONE memory
// slot shared by every body standing in that chunk, which is not hysteresis
// for any of them: a sword jittering across a boundary moved the crate beside
// it as well, and two bodies crossing in opposite directions overwrote each
// other's incumbent every tick (the flapping case authority.h names). With the
// global id in hand this is exactly `MobOwner` with a different table.
uint32_t EntitySync::BodyOwner(uint64_t globalBodyId, Vec3 posVox) {
  if (peerCount_ < 2) return me_;   // no second candidate: everything is mine
  if (globalBodyId == 0) return BodyOwner(posVox);   // no identity to key on
  const uint32_t who = EntityAuthority(globalBodyId, posVox, Peers(), bodyMem_);
  // kNoAuthority: nobody's window holds it with margin. Unlike a chunk (which
  // is simply not scanned), a body has to be stepped by SOMEBODY or it freezes
  // in mid-air, so the machine that currently holds it keeps it and the
  // incumbent is left alone — `MobOwner`'s rule, for the same reason.
  if (who == kNoAuthority) {
    const auto it = bodyMem_.find(globalBodyId);
    return it != bodyMem_.end() ? it->second : me_;
  }
  Remember(bodyMem_, globalBodyId, who);
  return who;
}

uint32_t EntitySync::BodyOwner(Vec3 posVox) {
  if (peerCount_ < 2) return me_;
  const IVec3 wc = ChunkOfFeet(posVox);
  const uint64_t key = World::PackChunkKey(wc);
  const uint32_t who = ChunkAuthority(wc, Peers(), bodyChunkMem_);
  if (who == kNoAuthority) {
    const auto it = bodyChunkMem_.find(key);
    return it != bodyChunkMem_.end() ? it->second : me_;
  }
  Remember(bodyChunkMem_, key, who);
  return who;
}

// ---- and the one place both closures are installed --------------------------
void EntitySync::BindOwnership(MobSystem& mobs, DebrisSystem& debris) {
  if (ownershipBound_) return;
  mobs.SetOwnershipFn(
      [this](uint64_t id, Vec3 feet) { return MobOwner(id, feet); });
  debris.SetOwnershipFn(
      [this](uint64_t id, Vec3 pos) { return BodyOwner(id, pos); });
  debris.SetChunkOwnedFn([this](IVec3 wc) { return ChunkOwned(wc); });
  ownershipBound_ = true;
}

bool EntitySync::ChunkOwned(IVec3 wc) {
  if (peerCount_ < 2) return true;
  const uint32_t who = ChunkAuthority(wc, Peers(), chunkMem_);
  // A CHUNK NOBODY HAS IS NOT SCANNED. Unlike a mob, an unscanned region of
  // grid is not a thing that freezes: the island scan is idempotent and runs
  // again the moment somebody's window covers it with margin. Claiming it
  // would mean scanning grid whose neighbours are SOLID window edges, which
  // is §4 finding 1's non-comparable case and would drop matter the peer
  // still sees supported.
  if (who == kNoAuthority) return false;
  Remember(chunkMem_, World::PackChunkKey(wc), who);
  return who == me_;
}

// ============================================================================
// HANDOFFS — BEFORE THE TICK
// ============================================================================
//
// WHY THE LOSING SIDE SENDS, AND WHY THE WINNING SIDE DOES NOT WAIT.
// Ownership is DERIVED (authority.h): both machines compute the same answer
// from the same two `PlayerState`s, so both observe the flip on the same
// tick. The RECEIVING machine therefore promotes its ghost to local
// immediately, through the ordinary `RefreshOwnership` inside PreTick, with
// nothing to wait for. What the handoff message carries is not permission,
// it is STATE: the per-mob save record (wounds, carve holes, rig geometry),
// the brain's target, the gear by name — everything a pose stream cannot
// express. It arrives D+1 ticks later and is overlaid in place
// (`MobSystem::ApplyHandoff` keeps the rig), so the creature is stepped from
// its ghost pose for four ticks and then gains its damage back.
//
// That four-tick window is the honest cost of fixed-latency lockstep and it
// is visible only as a creature whose wounds appear a tenth of a second after
// it changes hands. The alternative — stall the entity until its record
// arrives — is a visible freeze on every boundary crossing.
//
// BEFORE THE TICK, because `TakeHandoff` / `BuildHandoff` refuse an entity
// that is already a ghost, and `RefreshOwnership` (the first thing both
// systems' PreTick does) is what makes it one.
void EntitySync::ScanHandoffs(MobSystem& mobs, DebrisSystem& debris,
                              uint32_t tick) {
  (void)tick;
  if (!connected_) return;
  // D7a, before anything below asks MobOwner: forget the incumbents of the
  // ids that entered a tier, and hand the host's tier the peer's window.
  PrepareDistant(mobs);
  if (peerCount_ < 2) return;
  // Once per connection, and free after that. It is here rather than only at
  // connect time so that a caller which installed the older position-only
  // debris closure still ends up with the body-keyed hysteresis M9.4-E added
  // — one binding site, whoever called it.
  BindOwnership(mobs, debris);

  // ---- creatures ---------------------------------------------------------
  //
  // Ids first, then act: `TakeHandoff` writes into `mobs_` and a later
  // package could make it remove an entry, and an index walk that outlived
  // that would be a use-after-move with no symptom until a crash.
  std::vector<uint64_t> flipping;
  for (uint32_t i = 0; i < mobs.MobCount(); i++) {
    const Mob* m = mobs.MobAt(i);
    if (m == nullptr || m->Def() == nullptr) continue;
    if (m->IsGhost()) continue;   // not mine to give away
    // THE DEAD ARE HANDED OVER TOO (P3): the record is MOBS v6, which says
    // "dead", and MobSystem::ApplyHandoff enters the dead state from it.
    // The FEET, the same point MobSystem::RefreshOwnership asks about — a
    // different anchor here would compute a different owner than the system
    // is about to, and the handoff would name the wrong machine.
    const Vec3 origin = m->Origin();
    const Vec3 feet{origin.x + m->Def()->worldSize.x * 0.5f, origin.y,
                    origin.z + m->Def()->worldSize.z * 0.5f};
    const uint32_t want = MobOwner(m->Id(), feet);
    if (want != me_ && want != kNoAuthority) flipping.push_back(m->Id());
  }
  for (uint64_t id : flipping) {
    MobHandoff h;
    // `want` is re-asked rather than carried: `MobOwner` is memoised through
    // the incumbent table, so the second call is the same answer and this
    // costs a map lookup instead of a parallel vector.
    const uint32_t want = mobMem_.count(id) ? mobMem_[id] : peer_;
    if (!mobs.TakeHandoff(id, want, h)) continue;
    pendingMobHandoffs_.push_back(std::move(h));
    // The handoff CARRIES an announce, so the peer no longer needs one from
    // us and we are no longer the thing that announces it.
    announcedMobs_.erase(id);
    mobPoseSent_.erase(id);
    mobGearSent_.erase(id);
    c_.handoffsOut++;
  }

  // ---- loose matter ------------------------------------------------------
  std::vector<uint64_t> handles;
  for (uint32_t i = 0; i < debris.BodyCount(); i++) {
    const uint64_t h = debris.BodyHandle(i);
    if (h == 0 || debris.IsGhost(h)) continue;
    // BY THE BODY'S OWN ID, the same question `RefreshOwnership` asks a tick
    // later: a chunk-keyed answer here and a body-keyed one there would name
    // two different machines for one crate.
    if (BodyOwner(debris.GlobalIdOf(h), debris.BodyPosition(i)) != me_)
      handles.push_back(h);
  }
  for (uint64_t h : handles) {
    BodyHandoff bh;
    if (!debris.BuildHandoff(h, peer_, bh)) continue;
    announcedBodies_.erase(bh.announce.globalId);
    pendingBodyHandoffs_.push_back(std::move(bh));
    c_.handoffsOut++;
  }
}

// ============================================================================
// D7a: THE DISTANT TIER ACROSS MACHINES (net/distantsync.h; game/distant.h)
// ============================================================================
//
// OWNERSHIP'S MEMORY FORGETS A CREATURE THAT ENTERED A TIER. `mobMem_` holds
// the incumbent net::EntityAuthority's hysteresis reads, keyed by mob id. A
// creature that went distant (a host demotion, a hand-out sent or received, a
// hand-in received, a transit restored) keeps its id, and when it comes back
// near -- a promotion, on either machine, maybe minutes later and on the
// other side of the map -- a stale incumbent from before it left would win
// the hysteresis for the wrong machine and ghost it there. Forgotten here, its
// first answer after promotion is the arithmetic's alone, which both
// machines compute alike.
//
// THE HOST'S TIER IS TOLD W. The peer's window (the very origin the authority
// arithmetic reads, aged as it is: PeerView) and the room it last reported go
// to MobSystem::SetDistantPeers every tick before the tick -- empty until the
// peer has spoken, so a host whose peer has not yet said where it is runs the
// single-player rules.
void EntitySync::PrepareDistant(MobSystem& mobs) {
  std::vector<uint64_t> entered;
  mobs.TakeTierEntries(entered);
  for (const uint64_t id : entered) mobMem_.erase(id);
  if (mobs.GetDistantNetRole() != DistantNetRole::Host) return;
  if (peerCount_ < 2) {
    mobs.SetDistantPeers({});
    return;
  }
  DistantPeerWindow p;
  p.playerId = peers_[1].playerId;
  p.windowOrigin = peers_[1].windowOrigin;
  p.livingFree = peerLivingFree_;
  p.haveFree = havePeerFree_;
  mobs.SetDistantPeers(std::span<const DistantPeerWindow>(&p, 1));
}

namespace {
// FNV-1a over everything a view entry SAYS -- the statue, the band actor, the
// box, the shape -- so "changed" means "the client would draw or see it
// differently", bit for bit (no float tolerance: a creature that moved a
// thousandth of a voxel moved).
uint64_t ViewEntryHash(const DistantViewEntry& e) {
  uint64_t h = 1469598103934665603ull;
  auto bytes = [&h](const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) {
      h ^= b[i];
      h *= 1099511628211ull;
    }
  };
  auto f = [&bytes](float v) { bytes(&v, sizeof v); };
  auto u = [&bytes](uint64_t v) { bytes(&v, sizeof v); };
  u(e.flags);
  for (char ch : e.defName) u((uint8_t)ch);
  u(e.look.full);
  u(e.look.pristine);
  u(e.look.bare);
  u(e.look.def);
  f(e.origin.x);
  f(e.origin.y);
  f(e.origin.z);
  f(e.heading);
  f(e.gaitPhase);
  u(e.state);
  if (e.flags & DistantViewEntry::kBand) {
    const DistantViewActor& a = e.actor;
    f(a.centre.x);
    f(a.centre.y);
    f(a.centre.z);
    f(a.radius);
    f(a.height);
    f(a.hpFrac);
    f(a.heading);
    f(a.immersion);
    f(a.reach);
    u(a.faction);
    u(a.targetId);
    f(a.velocity.x);
    f(a.velocity.y);
    f(a.velocity.z);
    u(((uint64_t)a.armed) | ((uint64_t)a.hostile << 8) | ((uint64_t)a.bleeding << 16) |
      ((uint64_t)a.action << 24));
  }
  if (e.flags & DistantViewEntry::kBox) {
    f(e.boxLo.x);
    f(e.boxLo.y);
    f(e.boxLo.z);
    f(e.boxHi.x);
    f(e.boxHi.y);
    f(e.boxHi.z);
  }
  return h;
}
}  // namespace

// THE VIEW, AS A DIFFERENCE. MobSystem builds every entry whole (the tier
// creatures within distant.radius of the peer's window centre, ascending id);
// what goes is: first sight (with the shape), a change (without it), a
// keyframe every kDistantViewKeyframeTicks (with it -- the asleep creature's
// whole cost, and the client's way back if it ever lost its mirror), and a
// GONE for every id sent before that is not in the set now (it walked out of
// the radius, was handed in, promoted, retired). Gones in ascending id: the
// bytes are a pure function of the tier and the peer's window.
void EntitySync::BuildDistantView(MobSystem& mobs, uint32_t label, DistantView& out) {
  thread_local std::vector<DistantViewEntry> now;
  thread_local std::vector<uint64_t> ids;
  mobs.BuildDistantView(peers_[1].windowOrigin, label, now);
  ids.clear();
  for (const DistantViewEntry& e : now) ids.push_back(e.id);   // ascending
  for (DistantViewEntry& e : now) {
    const uint64_t h = ViewEntryHash(e);
    auto it = viewSent_.find(e.id);
    const bool first = it == viewSent_.end();
    const bool keyframe = !first && label - it->second.label >= kDistantViewKeyframeTicks;
    const bool changed = !first && it->second.hash != h;
    if (!first && !changed && !keyframe) continue;
    viewSent_[e.id] = ViewSent{h, label};
    if (!first && !keyframe) {
      e.flags &= ~DistantViewEntry::kShape;
      e.defName.clear();
      e.look = WireLook{};
    } else {
      c_.viewShapesOut++;
    }
    out.entries.push_back(std::move(e));
    c_.viewEntriesOut++;
  }
  // The gones: every id remembered that is not in this tick's set (`ids` is
  // ascending, so a binary search answers membership).
  for (auto it = viewSent_.begin(); it != viewSent_.end();) {
    const uint64_t id = it->first;
    const bool still = std::binary_search(ids.begin(), ids.end(), id);
    if (still) {
      ++it;
      continue;
    }
    out.gones.push_back(id);
    it = viewSent_.erase(it);
  }
  std::sort(out.gones.begin(), out.gones.end());
  c_.viewGonesOut += out.gones.size();
  if (!out.Empty()) {
    c_.viewBatchesOut++;
    c_.viewBytesOut += EncodedSize(out);
  }
}

// ============================================================================
// THE OUTGOING BATCH — AFTER THE TICK
// ============================================================================

void EntitySync::Build(MobSystem& mobs, DebrisSystem& debris, uint32_t label,
                       EntityBatch& out) {
  out.Clear();
  if (!connected_) return;
  out.tick = label;
  out.playerId = me_;

  // Staged by ScanHandoffs at the top of this tick. They go out whether or
  // not we have a peer view — a handoff is addressed to a specific machine
  // and does not depend on interest.
  out.mobHandoffs.swap(pendingMobHandoffs_);
  out.bodyHandoffs.swap(pendingBodyHandoffs_);

  // ---- item traffic, which is also interest-independent ------------------
  //
  // Requests I made of the peer go out; replies come out of the SAME queue on
  // both machines (debris.h's note on PopItemGrant) and are split here by who
  // they are addressed to. That split is the only reason this drain is not
  // simply `while (PopItemGrant) send`.
  {
    ItemTake t{};
    while (debris.PopItemTake(t)) {
      out.itemTakes.push_back(t);
      c_.takesOut++;
    }
    ItemGrant g{};
    while (debris.PopItemGrant(g)) {
      if (g.toPlayer == me_) {
        localGrants_.push_back(g);
      } else {
        out.itemGrants.push_back(g);
        c_.grantsOut++;
      }
    }
  }

  // ---- blows on the peer's bodies (game/meleehit.h), interest-independent:
  // a record names its target, and the peer applies it only if it owns it.
  {
    MeleeHit h;
    while (mobs.PopMeleeHit(h)) {
      out.meleeHits.push_back(std::move(h));
      c_.meleeOut++;
    }
  }

  // ---- MY PLAYER'S BODY (net::PlayerBody) --------------------------------
  //
  // Every tick while it stands: the peer draws its ghost of me from this.
  // Interest-independent for the same reason a PlayerState is -- the peer
  // always has my player. The gear rides when it changed (a key over the
  // rig-slot gear list, MobSystem::RedressFromWire's comparison) or on the
  // keyframe.
  if (const Mob* av = mobs.Avatar();
      av != nullptr && !av->IsGhost() && av->Def() != nullptr &&
      av->LimbBodyCount() > 0) {
    PlayerBody pb;
    pb.playerId = me_;
    std::vector<WireGear> gear;
    av->CaptureGear(gear);
    uint64_t key = 1469598103934665603ull;
    auto mix = [&key](uint64_t v) {
      for (int b = 0; b < 8; b++) {
        key ^= (v >> (b * 8)) & 0xFFu;
        key *= 1099511628211ull;
      }
    };
    mix(gear.size());
    for (const WireGear& g : gear) {
      for (char ch : g.name) mix((uint8_t)ch);
      mix((uint64_t)(int64_t)g.equipSlot);
      mix(g.held);
      mix(g.dye);
    }
    if (!gearEverSent_ || key != gearKeySent_ ||
        label - gearLabelSent_ >= kPlayerGearKeyframeTicks) {
      pb.hasGear = 1;
      pb.gear = std::move(gear);
      gearKeySent_ = key;
      gearLabelSent_ = label;
      gearEverSent_ = true;
      c_.gearOut++;
    }
    MobSystem::BuildPoseOf(*av, label, pb.pose);
    out.playerBodies.push_back(std::move(pb));
    c_.bodiesOut++;
  }

  // ---- D7a: THE DISTANT TIER ACROSS MACHINES (net/distantsync.h) ----------
  //
  // Addressed records, interest-independent: what this tick's tier queued
  // for the peer -- a client's hand-outs and its answers to hand-ins, a
  // host's hand-ins (the outboxes are empty without a role). Then a CLIENT's
  // room, on change and on the keyframe (the host serves its remote
  // promotions under it), and a HOST's view of its tier for the peer, which
  // needs the peer's window.
  {
    mobs.TakeDistantOutgoing(out.distantHandOuts, out.distantHandIns, out.distantDones);
    c_.handOutsOut += out.distantHandOuts.size();
    c_.handInsOut += out.distantHandIns.size();
    c_.donesOut += out.distantDones.size();
    if (mobs.GetDistantNetRole() == DistantNetRole::Client) {
      const uint32_t free = mobs.DistantLivingFree();
      if (free != freeSent_ || label - freeLabel_ >= kDistantViewKeyframeTicks) {
        out.livingFree = free;
        freeSent_ = free;
        freeLabel_ = label;
        c_.freeReportsOut++;
      }
    }
    if (mobs.GetDistantNetRole() == DistantNetRole::Host && peerCount_ >= 2)
      BuildDistantView(mobs, label, out.distantView);
    // D7b: THE EDGE ACROSS THE PEER'S WINDOW -- whatever this tick's tier
    // queued for the other side (game/distant.h DistantEdgeWire). Empty
    // without a role.
    mobs.TakeDistantEdge(out.distantEdge);
  }

  // ---- interest, which needs to know where the peer is -------------------
  if (peerCount_ < 2) return;
  const IVec3 peerOrigin = peers_[1].windowOrigin;

  // ---- creatures ---------------------------------------------------------
  //
  // One pass over my mobs building the "inside the peer's interest set" set,
  // then a difference against what was announced last tick. The set is small
  // (a window holds tens of creatures, not thousands), so a flat vector +
  // sort would be faster in principle and unreadable in practice; this is not
  // on the frame's hot path — it runs once per TICK on the CPU beside a
  // socket write.
  std::unordered_set<uint64_t> visibleMobs;
  for (uint32_t i = 0; i < mobs.MobCount(); i++) {
    const Mob* m = mobs.MobAt(i);
    if (m == nullptr || m->Def() == nullptr || m->IsGhost()) continue;
    // A RELEASED RIG IS A HUSK: its limbs are DebrisSystem's now and travel
    // as bodies, so it leaves the visible set and gets its MobGone below. A
    // corpse that still has its rig is streamed like the living (P2c).
    if (m->RigReleased()) continue;
    const Vec3 origin = m->Origin();
    const Vec3 feet{origin.x + m->Def()->worldSize.x * 0.5f, origin.y,
                    origin.z + m->Def()->worldSize.z * 0.5f};
    if (!ResidentWithMargin(ChunkOfFeet(feet), peerOrigin,
                            -kInterestMarginChunks))
      continue;
    const uint64_t id = m->Id();
    visibleMobs.insert(id);
    // RULE 1: the shape first, and only once.
    if (announcedMobs_.insert(id).second) {
      MobAnnounce a;
      if (mobs.BuildAnnounce(id, a)) {
        out.mobAnnounces.push_back(std::move(a));
        c_.announcesOut++;
        mobGearSent_[id] = mobs.GearKeyOf(id);
      } else {
        announcedMobs_.erase(id);   // could not describe it; try again later
        continue;
      }
    } else if (m->Alive()) {
      // RULE 1b: A LIVING CREATURE'S GEAR CHANGED (MobSystem::GearKeyOf) --
      // a shield broken, a blade knocked from the hand, a cuirass cut loose,
      // a piece put on. The announce is the one record that carries the
      // gear, so it goes again; the peer's ApplyAnnounce re-dresses a ghost
      // whose list differs. A CORPSE's gear rides its MobState (below).
      // Before the pose, for the corpse's reason: the pose is indexed by the
      // rig slots the re-dress lays out.
      const uint64_t gk = mobs.GearKeyOf(id);
      auto sent = mobGearSent_.find(id);
      if (sent == mobGearSent_.end() || sent->second != gk) {
        MobAnnounce a;
        if (mobs.BuildAnnounce(id, a)) {
          out.mobAnnounces.push_back(std::move(a));
          c_.announcesOut++;
          c_.gearReannounces++;
          mobGearSent_[id] = gk;
        }
      }
    }
    // THE CORPSE'S SHAPE (P2c), before its pose: a MobState re-dresses the
    // ghost, and the pose is indexed by the rig slots that re-dress lays out.
    // Keyed, not timed: a corpse nobody touches sends one state (at death, or
    // at first sight) and never another.
    if (!m->Alive()) {
      const uint64_t key = mobs.StateKey(id);
      auto sent = mobStateSent_.find(id);
      const bool changed = sent == mobStateSent_.end() || sent->second.key != key;
      const bool due = sent == mobStateSent_.end() ||
                       label - sent->second.label >= kDeadStateMinTicks;
      if (changed && due && out.mobStates.size() < kMaxDeadStatesPerBatch) {
        MobState st;
        if (mobs.BuildState(id, st)) {
          out.mobStates.push_back(std::move(st));
          mobStateSent_[id] = DeadSent{key, label};
          c_.statesOut++;
        }
      }
    }
    // RULE 2: and the pose, every tick — except an ASLEEP corpse, which gets
    // one keyframe per kDeadPoseKeyframeTicks (the header says why any).
    auto last = mobPoseSent_.find(id);
    if (m->DeadAsleep() && last != mobPoseSent_.end() &&
        label - last->second < kDeadPoseKeyframeTicks) {
      c_.posesAsleep++;
      continue;
    }
    MobPose p;
    if (mobs.BuildPose(id, label, p)) {
      out.mobPoses.push_back(std::move(p));
      mobPoseSent_[id] = label;
      c_.posesOut++;
    }
  }
  // RULE 3: everything the peer was told about that is no longer mine and
  // inside its window. A DEATH IS NOT HERE ANY MORE (P2c): a corpse stays in
  // the visible set and its pose says alive = 0. What is here is a creature
  // that left the window, was parked or handed away, and a corpse whose rig
  // decayed to debris (released, or swept as a husk) — the peer drops its
  // ghost Mob, and the limbs come across as ghost bodies through the debris
  // half below, exactly as a severed part always has. The reason is
  // diagnostic only.
  for (auto it = announcedMobs_.begin(); it != announcedMobs_.end();) {
    if (visibleMobs.count(*it)) {
      ++it;
      continue;
    }
    const Mob* fm = mobs.FindMobById(*it);
    const bool wasCorpse = mobStateSent_.count(*it) != 0;
    const bool decayed = (fm != nullptr && fm->RigReleased()) ||
                         (fm == nullptr && wasCorpse);
    MobGone g{*it, decayed ? kGoneEvicted : kGoneDespawn};
    out.mobGones.push_back(g);
    c_.gonesOut++;
    mobPoseSent_.erase(*it);
    mobStateSent_.erase(*it);
    mobGearSent_.erase(*it);
    it = announcedMobs_.erase(it);
  }

  // ---- loose matter ------------------------------------------------------
  //
  // `OwnedBodiesNear` is C's interest query and it already applies the
  // sleeping-body discount, so a settled corpse drops out of this list on its
  // own. That makes "left the peer's window" and "went to sleep" the same
  // observation here, and the Gone below would fire on a body that merely
  // settled — which would delete it on the peer. So the difference is taken
  // against a SECOND, margin-only test rather than against this list.
  std::vector<uint64_t> near;
  debris.OwnedBodiesNear(peerOrigin, kInterestMarginChunks, near);
  for (uint64_t gid : near) {
    const uint64_t h = debris.HandleOfGlobalId(gid);
    if (h == 0) continue;
    if (announcedBodies_.insert(gid).second) {
      BodyAnnounce a;
      if (debris.BuildAnnounce(h, a)) {
        out.bodyAnnounces.push_back(std::move(a));
        c_.announcesOut++;
      } else {
        announcedBodies_.erase(gid);
        continue;
      }
    }
    BodyPose p;
    if (debris.BuildPose(h, label, p)) {
      out.bodyPoses.push_back(p);
      c_.posesOut++;
    }
  }
  // A body the peer knows about that this machine no longer owns or no longer
  // HAS. Deliberately NOT "is not in `near`": a settled body is absent from
  // that list and must keep existing on the peer.
  for (auto it = announcedBodies_.begin(); it != announcedBodies_.end();) {
    const uint64_t h = debris.HandleOfGlobalId(*it);
    if (h != 0 && !debris.IsGhost(h)) {
      ++it;
      continue;
    }
    out.bodyGones.push_back(BodyGone{*it, 0});
    c_.gonesOut++;
    it = announcedBodies_.erase(it);
  }

  if (!out.Empty()) c_.batchesOut++;
}

// ============================================================================
// INCOMING
// ============================================================================

void EntitySync::NoteRemote(EntityBatch&& b) {
  if (!connected_) return;
  c_.batchesIn++;
  inbox_.push_back(std::move(b));
}

void EntitySync::ApplyForTick(uint32_t tick, MobSystem& mobs,
                              DebrisSystem& debris, WorldItems* items) {
  if (!connected_) return;
  // EVERY BATCH AT OR BEFORE `tick`, not just the one labelled it. A batch
  // whose label this machine has already run describes a world it has stepped
  // past, but its records are still the freshest thing anybody has said about
  // those entities — dropping it would leave a ghost frozen for the rest of
  // the connection. Applying it late is strictly better than not at all, and
  // every record here is idempotent or newest-wins.
  for (auto it = inbox_.begin(); it != inbox_.end();) {
    if (it->tick > tick) {
      ++it;
      continue;
    }
    EntityBatch& b = *it;

    // ---- handoffs, first: they carry their own announce -----------------
    for (const MobHandoff& h : b.mobHandoffs) {
      if (mobs.ApplyHandoff(h) != nullptr) {
        c_.handoffsIn++;
        ghostMobSeen_.erase(h.announce.id);   // it is mine now, not a ghost
      }
    }
    for (const BodyHandoff& h : b.bodyHandoffs) {
      if (debris.ApplyBodyHandoff(h) != 0) {
        c_.handoffsIn++;
        ghostBodySeen_.erase(h.announce.globalId);
      }
    }

    // ---- announces ------------------------------------------------------
    //
    // A mob announce with no handoff behind it means "I own this creature and
    // you have never seen it" — a creature the peer has ALWAYS owned, which
    // until M9.4-E had nowhere to land: only `ApplyHandoff` spawned, and it
    // spawns a mob this machine then owns, so every pose for a creature the
    // peer merely KEPT was refused for an unknown id (the two-process smoke
    // counted `misses=144`). `MobSystem::ApplyAnnounce` builds it as a GHOST
    // from the def name and the gear, and the pose two fields down — same
    // batch, applied after this loop by the envelope's field order — is what
    // places it.
    //
    // The expiry-clock entry is set whether or not the spawn succeeded: an
    // announce for a def this build does not have is still the peer telling
    // us the id exists, and `ExpireGhosts` is what forgets it.
    for (const MobAnnounce& a : b.mobAnnounces) {
      c_.announcesIn++;
      mobs.ApplyAnnounce(a);
      ghostMobSeen_[a.id] = tick;
    }
    for (const BodyAnnounce& a : b.bodyAnnounces) {
      const uint64_t h = debris.ApplyBodyAnnounce(a);
      if (h == 0) continue;
      c_.announcesIn++;
      ghostBodySeen_[a.globalId] = tick;
      // A GHOST ITEM NEEDS AN IDENTITY OR E CANNOT SEE IT. `WorldItems` is a
      // layer above debris and the announce is the only place the name
      // travels; without this the sword on the peer's ground is a nameless
      // pile of voxels and `ground.Find()` returns null.
      if (items != nullptr && !a.item.name.empty()) items->Add(h, a.item);
    }

    // ---- states (P2c): a corpse's shape, before the pose that indexes it --
    for (const MobState& st : b.mobStates) {
      if (mobs.ApplyState(st)) {
        c_.statesIn++;
        ghostMobSeen_[st.announce.id] = tick;
      }
    }

    // ---- poses ----------------------------------------------------------
    // ---- WHY A POSE WAS REFUSED, not just that one was (M9.4-E) ----------
    //
    // `applyMisses` is documented as "poses for an id we do not hold" and
    // that is the only defect it can name: a creature the peer is drawing
    // that does not exist here. It used to count TWO other things as well,
    // and both of them are the system working:
    //
    //   MINE NOW. The receiving machine promotes a ghost to local the tick
    //   the derived authority flips (`ScanHandoffs`' long note) and does not
    //   wait for the handoff record, so the D+1 poses already in flight
    //   arrive for a creature this machine has correctly started stepping.
    //   `ApplyPose` refuses them, which is right, and counting the refusal
    //   as a miss made the fixed-latency window look like a lost entity.
    //
    //   STALE. A pose older than the one already latched, dropped rather
    //   than applied so a body cannot be rewound.
    //
    // Split at the point of failure rather than inferred later (CLAUDE.md
    // rule 6): the id is in hand here and `FindMobById` / `HandleOfGlobalId`
    // answer "do I hold it" exactly.
    for (const MobPose& p : b.mobPoses) {
      if (mobs.ApplyPose(p)) {
        c_.posesIn++;
        ghostMobSeen_[p.id] = tick;
      } else if (mobs.FindMobById(p.id) == nullptr) {
        c_.applyMisses++;
      } else if (mobs.MobOwner(p.id) == mobs.LocalPlayerId()) {
        c_.posesMine++;
      } else {
        c_.posesStale++;
      }
    }
    for (const BodyPose& p : b.bodyPoses) {
      if (debris.ApplyBodyPose(p)) {
        c_.posesIn++;
        ghostBodySeen_[p.globalId] = tick;
      } else {
        const uint64_t h = debris.HandleOfGlobalId(p.globalId);
        if (h == 0)
          c_.applyMisses++;
        else if (!debris.IsGhost(h))
          c_.posesMine++;
        else
          c_.posesStale++;
      }
    }

    // ---- gones ----------------------------------------------------------
    for (const MobGone& g : b.mobGones) {
      mobs.ApplyGone(g);
      ghostMobSeen_.erase(g.id);
      c_.gonesIn++;
    }
    for (const BodyGone& g : b.bodyGones) {
      debris.ApplyBodyGone(g);
      ghostBodySeen_.erase(g.globalId);
      c_.gonesIn++;
    }

    // ---- items ----------------------------------------------------------
    //
    // A take is a question about one of MY bodies; `ApplyItemTake` answers it
    // through the registry callbacks and pushes the reply onto the grant
    // queue, which the next `Build` drains and sends. A grant is the answer to
    // a question I asked: it destroys my ghost and re-queues itself so the
    // same `Build` routes it to `localGrants_` for the bag.
    for (const ItemTake& t : b.itemTakes) {
      debris.ApplyItemTake(t);
      c_.takesIn++;
    }
    for (const ItemGrant& g : b.itemGrants) {
      if (g.toPlayer != me_) continue;   // not addressed to this machine
      debris.ApplyItemGrant(g);
      c_.grantsIn++;
    }

    // ---- blows on my bodies (game/meleehit.h) --------------------------
    // Handed to MobSystem, NOT applied here: a blow carves, bleeds and
    // emits gore, which needs the tick's World and op batch. The tick
    // applies them at one fixed phase (session.cpp, ApplyRemoteMeleeHits).
    for (MeleeHit& h : b.meleeHits) {
      mobs.NoteRemoteMeleeHit(std::move(h));
      c_.meleeIn++;
    }

    // ---- the peer's player's body: kept for the caller -----------------
    // The newest pose wins; a gear list is kept until taken even when a
    // later body without one replaces the pose.
    for (PlayerBody& pb : b.playerBodies) {
      if (pb.playerId == me_) continue;
      c_.bodiesIn++;
      if (havePeerBody_ && pb.pose.tick < peerBody_.pose.tick) continue;
      if (havePeerBody_ && !pb.hasGear && peerBody_.hasGear) {
        pb.hasGear = 1;
        pb.gear = std::move(peerBody_.gear);
      }
      peerBody_ = std::move(pb);
      havePeerBody_ = true;
    }

    // ---- D7a: the distant tier across machines (the envelope's order) ----
    // A HOST takes the peer's hand-outs (after the gones above released the
    // ghost it drew of each) and its answers to the hand-ins it sent; a
    // CLIENT takes the hand-ins into its waiting room, then the view of the
    // host's tier its band and statues are drawn from. MobSystem refuses
    // what its role does not expect -- and a refusal never loses a creature
    // (a hand-out it cannot take is parked, a hand-in is given back).
    for (const DistantHandOut& h : b.distantHandOuts) {
      mobs.ApplyDistantHandOut(h, tick);
      c_.handOutsIn++;
    }
    for (const DistantHandInDone& d : b.distantDones) {
      mobs.ApplyDistantDone(d, tick);
      c_.donesIn++;
    }
    for (const DistantHandIn& h : b.distantHandIns) {
      mobs.ApplyDistantHandIn(h, tick);
      c_.handInsIn++;
    }
    if (!b.distantView.Empty()) {
      mobs.ApplyDistantView(b.distantView);
      c_.viewEntriesIn += b.distantView.entries.size();
      c_.viewGonesIn += b.distantView.gones.size();
    }
    if (b.livingFree != kNoLivingFree) {
      peerLivingFree_ = b.livingFree;
      havePeerFree_ = true;
    }
    // D7b: the edge across the window, into the tier's queues for this tick.
    if (!b.distantEdge.Empty()) mobs.ApplyDistantEdge(::DistantEdgeWire(b.distantEdge), tick);

    it = inbox_.erase(it);
  }

  c_.ghostsNow = mobs.GhostCount() + debris.GhostCount();
  c_.ghostsMax = std::max(c_.ghostsMax, c_.ghostsNow);
}

void EntitySync::ExpireGhosts(uint32_t tick, MobSystem& mobs,
                              DebrisSystem& debris) {
  if (!connected_) return;
  for (auto it = ghostMobSeen_.begin(); it != ghostMobSeen_.end();) {
    if (tick < it->second + kGhostExpiryTicks) {
      ++it;
      continue;
    }
    // `ApplyGone` refuses a mob this machine OWNS, which is exactly right: a
    // creature that was handed to us stopped being posed because it stopped
    // being a ghost, and the stale clock entry is the only thing to clean up.
    if (mobs.ApplyGone(MobGone{it->first, kGoneUnowned})) c_.expired++;
    it = ghostMobSeen_.erase(it);
  }
  for (auto it = ghostBodySeen_.begin(); it != ghostBodySeen_.end();) {
    if (tick < it->second + kGhostExpiryTicks) {
      ++it;
      continue;
    }
    const uint64_t h = debris.HandleOfGlobalId(it->first);
    if (h != 0 && debris.IsGhost(h)) {
      debris.ApplyBodyGone(BodyGone{it->first, kGoneUnowned});
      c_.expired++;
    }
    it = ghostBodySeen_.erase(it);
  }
}

bool EntitySync::TakePeerBody(PlayerBody& out) {
  if (!havePeerBody_) return false;
  out = std::move(peerBody_);
  peerBody_ = PlayerBody{};
  havePeerBody_ = false;
  return true;
}

bool EntitySync::PopLocalGrant(ItemGrant& out) {
  if (localGrants_.empty()) return false;
  out = localGrants_.front();
  localGrants_.pop_front();
  return true;
}

}  // namespace net
