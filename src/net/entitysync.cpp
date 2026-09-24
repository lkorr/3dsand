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
  mobPoses.clear();
  mobGones.clear();
  bodyHandoffs.clear();
  bodyAnnounces.clear();
  bodyPoses.clear();
  bodyGones.clear();
  itemTakes.clear();
  itemGrants.clear();
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
  if (!ReadVec(r, mobPoses, mobRec)) return false;
  if (!ReadVec(r, mobGones, mobRec)) return false;
  if (!ReadVec(r, bodyHandoffs, bodyRec)) return false;
  if (!ReadVec(r, bodyAnnounces, bodyRec)) return false;
  if (!ReadVec(r, bodyPoses, bodyRec)) return false;
  if (!ReadVec(r, bodyGones, bodyRec)) return false;
  if (!ReadVec(r, itemTakes, bodyRec)) return false;
  if (!ReadVec(r, itemGrants, bodyRec)) return false;
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
  ghostMobSeen_.clear();
  ghostBodySeen_.clear();
  pendingMobHandoffs_.clear();
  pendingBodyHandoffs_.clear();
  inbox_.clear();
  localGrants_.clear();
  peerCount_ = 0;
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
  if (!connected_ || peerCount_ < 2) return;
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
    // A CORPSE IS NOT HANDED OVER (yet): the dead state is not on the wire
    // (PLAN_corpse_is_a_mob.md P2c), so a dead Mob stays with the machine it
    // died on.
    if (!m->Alive()) continue;
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
    // THE DEAD ARE NOT STREAMED YET (P2c): a creature that died leaves the
    // visible set exactly as it did when a death removed it from mobs_, so the
    // peer gets its MobGone below.
    if (!m->Alive()) continue;
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
      } else {
        announcedMobs_.erase(id);   // could not describe it; try again later
        continue;
      }
    }
    // RULE 2: and the pose, every tick.
    MobPose p;
    if (mobs.BuildPose(id, label, p)) {
      out.mobPoses.push_back(std::move(p));
      c_.posesOut++;
    }
  }
  // RULE 3: everything the peer was told about that is no longer mine and
  // inside its window. Death and despawn are the same message; the reason is
  // diagnostic. `FindMobById` distinguishes them: still here = it walked out
  // of range, gone = it died or was culled.
  for (auto it = announcedMobs_.begin(); it != announcedMobs_.end();) {
    if (visibleMobs.count(*it)) {
      ++it;
      continue;
    }
    // Still here and ALIVE = it walked out of range; here but dead, or gone,
    // = it died (a corpse is a Mob now, so presence alone no longer says).
    const Mob* fm = mobs.FindMobById(*it);
    MobGone g{*it, fm != nullptr && fm->Alive() ? kGoneDespawn : kGoneDeath};
    out.mobGones.push_back(g);
    c_.gonesOut++;
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
      if (items != nullptr && !a.item.empty())
        items->Add(h, a.item, a.itemDye, a.itemDamage);
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

bool EntitySync::PopLocalGrant(ItemGrant& out) {
  if (localGrants_.empty()) return false;
  out = localGrants_.front();
  localGrants_.pop_front();
  return true;
}

}  // namespace net
