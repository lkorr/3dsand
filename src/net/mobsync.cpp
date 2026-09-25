// mobsync.cpp — the encode/decode half of net/mobsync.h. No sockets, no tick,
// no dependency on the mob system: given a record, produce bytes; given bytes,
// produce a record or a poisoned reader.
//
// SYMMETRY IS THE WHOLE CONTRACT. Every Encode/Decode pair below writes and
// reads the same fields in the same order, and the `mob-handoff` gate proves
// it the only way that cannot rot: encode → decode → encode and compare the
// two byte vectors. A field added to one side and forgotten on the other
// shifts every field after it, which that comparison catches on the first run.

#include "net/mobsync.h"

// GLOBAL `net`, not `sandvox::net`: game/mob.h and every header it includes
// (math3d.h, sim/bytestream.h, game/equipment.h) are in the global namespace,
// and this record set is consumed from there. Qualify as `::net::` at call
// sites inside `namespace sandvox` so a sibling `sandvox::net` cannot make the
// name ambiguous.
namespace net {
namespace {

// The gear list. Each entry is an ItemInstance (game/iteminstance.h's shared
// shape, so the wire, the MOBS record and the ground-item records cannot
// disagree about what an item is) plus where it sits on the rig.
void WriteGear(ByteWriter& w, const std::vector<WireGear>& gear) {
  w.U32((uint32_t)gear.size());
  for (const WireGear& g : gear) {
    WriteItemInstance(w, g);
    w.Pod(g.equipSlot);
    w.U32(g.held);
  }
}

bool ReadGear(ByteReader& r, std::vector<WireGear>& gear) {
  uint32_t n = 0;
  if (!r.U32(n)) return false;
  gear.clear();
  for (uint32_t i = 0; i < n && r.ok; i++) {
    WireGear g;
    if (!ReadItemInstance(r, g)) break;
    r.Pod(g.equipSlot);
    r.U32(g.held);
    if (!r.ok) break;
    gear.push_back(std::move(g));
  }
  return r.ok;
}

// Every Decode opens with this. A version mismatch poisons the reader rather
// than returning a "clean" false, so a caller that reads several records out
// of one buffer cannot accidentally carry on past the first bad one.
bool ReadVersion(ByteReader& r) {
  uint32_t v = 0;
  if (!r.U32(v)) return false;
  if (v != kMobSyncVersion) {
    r.ok = false;
    return false;
  }
  return true;
}

}  // namespace

void Encode(ByteWriter& w, const MobAnnounce& a) {
  w.U32(kMobSyncVersion);
  w.Pod(a.id);
  w.Str(a.defName);
  w.U32(a.owner);
  WriteGear(w, a.gear);
}

bool Decode(ByteReader& r, MobAnnounce& a) {
  if (!ReadVersion(r)) return false;
  r.Pod(a.id);
  r.Str(a.defName);
  r.U32(a.owner);
  return ReadGear(r, a.gear) && r.ok;
}

void Encode(ByteWriter& w, const MobPose& p) {
  w.U32(kMobSyncVersion);
  w.Pod(p.id);
  w.U32(p.tick);
  w.Pod(p.origin);
  w.F32(p.heading);
  w.U32(p.alive);
  // The one record on the hot path — one per owned mob per tick — so the limb
  // array travels as a single POD run rather than field by field.
  w.PodVec(p.limbs);
}

bool Decode(ByteReader& r, MobPose& p) {
  if (!ReadVersion(r)) return false;
  r.Pod(p.id);
  r.U32(p.tick);
  r.Pod(p.origin);
  r.F32(p.heading);
  r.U32(p.alive);
  r.PodVec(p.limbs);
  return r.ok;
}

void Encode(ByteWriter& w, const MobHandoff& h) {
  w.U32(kMobSyncVersion);
  // The announce is nested WHOLE, version word and all: it is the same record
  // a peer gets on first interest, and building it twice in two shapes is how
  // the two would stop agreeing about what identity means.
  Encode(w, h.announce);
  w.Str(h.brain.profile);
  w.Pod(h.brain.targetId);
  w.U32(h.brain.hasTarget);
  w.Pod(h.brain.targetPos);
  w.Pod(h.brain.lastSeenPos);
  w.U32(h.brain.lastSeenTick);
  w.U32(h.recordVersion);
  w.PodVec(h.record);
}

bool Decode(ByteReader& r, MobHandoff& h) {
  if (!ReadVersion(r)) return false;
  if (!Decode(r, h.announce)) return false;
  r.Str(h.brain.profile);
  r.Pod(h.brain.targetId);
  r.U32(h.brain.hasTarget);
  r.Pod(h.brain.targetPos);
  r.Pod(h.brain.lastSeenPos);
  r.U32(h.brain.lastSeenTick);
  r.U32(h.recordVersion);
  r.PodVec(h.record);
  return r.ok;
}

void Encode(ByteWriter& w, const MobGone& g) {
  w.U32(kMobSyncVersion);
  w.Pod(g.id);
  w.U32(g.reason);
}

bool Decode(ByteReader& r, MobGone& g) {
  if (!ReadVersion(r)) return false;
  r.Pod(g.id);
  r.U32(g.reason);
  return r.ok;
}

void Encode(ByteWriter& w, const MobState& s) {
  w.U32(kMobSyncVersion);
  // The announce nested whole, as the handoff nests it (same reason).
  Encode(w, s.announce);
  w.U32(s.recordVersion);
  w.PodVec(s.record);
}

bool Decode(ByteReader& r, MobState& s) {
  if (!ReadVersion(r)) return false;
  if (!Decode(r, s.announce)) return false;
  r.U32(s.recordVersion);
  r.PodVec(s.record);
  return r.ok;
}

}  // namespace net
