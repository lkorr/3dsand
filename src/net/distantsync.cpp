// distantsync.cpp — the encode/decode half of net/distantsync.h. No sockets,
// no tick, no MobSystem: given a record, produce bytes; given bytes, produce a
// record or a poisoned reader.
//
// SYMMETRY IS THE WHOLE CONTRACT (mobsync.cpp's): every Encode/Decode pair
// writes and reads the same fields in the same order. `distant-mp` carries
// every record it exchanges as encoded BYTES both ways, so a field added to
// one side and forgotten on the other shifts everything after it and fails
// the gate's first claim.

#include "net/distantsync.h"

namespace net {
namespace {

// mobsync.cpp's rule: a version mismatch POISONS the reader rather than
// returning a clean false, so a caller reading several records out of one
// buffer cannot carry on past the first bad one.
bool ReadVersion(ByteReader& r) {
  uint32_t v = 0;
  if (!r.U32(v)) return false;
  if (v != kDistantSyncVersion) {
    r.ok = false;
    return false;
  }
  return true;
}

// The brain travels exactly as a MobHandoff carries it (mobsync.cpp), field
// for field; restated here rather than shared because mobsync.cpp keeps it
// inline in the handoff's codec.
void WriteBrain(ByteWriter& w, const MobBrainWire& b) {
  w.Str(b.profile);
  w.Pod(b.targetId);
  w.U32(b.hasTarget);
  w.Pod(b.targetPos);
  w.Pod(b.lastSeenPos);
  w.U32(b.lastSeenTick);
}
void ReadBrain(ByteReader& r, MobBrainWire& b) {
  r.Str(b.profile);
  r.Pod(b.targetId);
  r.U32(b.hasTarget);
  r.Pod(b.targetPos);
  r.Pod(b.lastSeenPos);
  r.U32(b.lastSeenTick);
}

void WriteLook(ByteWriter& w, const WireLook& l) {
  w.Pod(l.full);
  w.Pod(l.pristine);
  w.Pod(l.bare);
  w.Pod(l.def);
}
void ReadLook(ByteReader& r, WireLook& l) {
  r.Pod(l.full);
  r.Pod(l.pristine);
  r.Pod(l.bare);
  r.Pod(l.def);
}

void WriteSteer(ByteWriter& w, const WireSteer& s) {
  w.F32(s.heading);
  w.F32(s.desiredHeading);
  w.F32(s.turnVel);
  w.F32(s.speedNow);
}
void ReadSteer(ByteReader& r, WireSteer& s) {
  r.F32(s.heading);
  r.F32(s.desiredHeading);
  r.F32(s.turnVel);
  r.F32(s.speedNow);
}

// The actor field by field, not as one POD: DistantViewActor has padding
// between its u8 flags and the next field, and padding bytes are whatever the
// stack held -- two encodes of one actor would differ, and the gate's
// twice-run digest over the wire bytes would see it.
void WriteActor(ByteWriter& w, const DistantViewActor& a) {
  w.Pod(a.centre);
  w.F32(a.radius);
  w.F32(a.height);
  w.F32(a.hpFrac);
  w.F32(a.heading);
  w.F32(a.immersion);
  w.F32(a.reach);
  w.U32(a.faction);
  w.Pod(a.targetId);
  w.Pod(a.velocity);
  const uint32_t bits = (uint32_t)a.armed | ((uint32_t)a.hostile << 1) |
                        ((uint32_t)a.bleeding << 2) | ((uint32_t)a.action << 8);
  w.U32(bits);
}
void ReadActor(ByteReader& r, DistantViewActor& a) {
  r.Pod(a.centre);
  r.F32(a.radius);
  r.F32(a.height);
  r.F32(a.hpFrac);
  r.F32(a.heading);
  r.F32(a.immersion);
  r.F32(a.reach);
  r.U32(a.faction);
  r.Pod(a.targetId);
  r.Pod(a.velocity);
  uint32_t bits = 0;
  r.U32(bits);
  a.armed = (uint8_t)(bits & 1u);
  a.hostile = (uint8_t)((bits >> 1) & 1u);
  a.bleeding = (uint8_t)((bits >> 2) & 1u);
  a.action = (uint8_t)((bits >> 8) & 0xFFu);
}

// A count a short or hostile payload cannot turn into a 4-billion-entry
// reserve (entitysync.cpp's kMaxRecordsPerKind): the tier holds 512.
constexpr uint32_t kMaxViewEntries = 65536;

}  // namespace

void Encode(ByteWriter& w, const DistantHandOut& h) {
  w.U32(kDistantSyncVersion);
  w.Pod(h.id);
  WriteBrain(w, h.brain);
  w.U32(h.recordVersion);
  w.PodVec(h.record);
  WriteSteer(w, h.steer);
  w.Pod(h.vel);
  w.U32(h.airborne);
  w.F32(h.status.bleedRate);
  w.Pod(h.status.bleedLimb);
  w.U32(h.status.burnTicksLeft);
  w.F32(h.status.burnFrac);
  w.U32(h.status.stumps);
  WriteLook(w, h.look);
}

bool Decode(ByteReader& r, DistantHandOut& h) {
  if (!ReadVersion(r)) return false;
  r.Pod(h.id);
  ReadBrain(r, h.brain);
  r.U32(h.recordVersion);
  r.PodVec(h.record);
  ReadSteer(r, h.steer);
  r.Pod(h.vel);
  r.U32(h.airborne);
  r.F32(h.status.bleedRate);
  r.Pod(h.status.bleedLimb);
  r.U32(h.status.burnTicksLeft);
  r.F32(h.status.burnFrac);
  r.U32(h.status.stumps);
  ReadLook(r, h.look);
  return r.ok;
}

void Encode(ByteWriter& w, const DistantHandIn& h) {
  w.U32(kDistantSyncVersion);
  w.Pod(h.id);
  WriteBrain(w, h.brain);
  w.U32(h.recordVersion);
  w.PodVec(h.record);
  WriteSteer(w, h.steer);
  w.Pod(h.vel);
  w.U32(h.flags);
  WriteLook(w, h.look);
}

bool Decode(ByteReader& r, DistantHandIn& h) {
  if (!ReadVersion(r)) return false;
  r.Pod(h.id);
  ReadBrain(r, h.brain);
  r.U32(h.recordVersion);
  r.PodVec(h.record);
  ReadSteer(r, h.steer);
  r.Pod(h.vel);
  r.U32(h.flags);
  ReadLook(r, h.look);
  return r.ok;
}

void Encode(ByteWriter& w, const DistantHandInDone& d) {
  w.U32(kDistantSyncVersion);
  w.Pod(d.id);
  w.U32(d.outcome);
}

bool Decode(ByteReader& r, DistantHandInDone& d) {
  if (!ReadVersion(r)) return false;
  r.Pod(d.id);
  r.U32(d.outcome);
  return r.ok;
}

void Encode(ByteWriter& w, const DistantView& v) {
  w.U32(kDistantSyncVersion);
  w.U32((uint32_t)v.entries.size());
  for (const DistantViewEntry& e : v.entries) {
    w.Pod(e.id);
    w.U32(e.flags);
    if (e.flags & DistantViewEntry::kShape) {
      w.Str(e.defName);
      WriteLook(w, e.look);
    }
    w.Pod(e.origin);
    w.F32(e.heading);
    w.F32(e.gaitPhase);
    w.U32(e.state);
    if (e.flags & DistantViewEntry::kBand) WriteActor(w, e.actor);
    if (e.flags & DistantViewEntry::kBox) {
      w.Pod(e.boxLo);
      w.Pod(e.boxHi);
    }
  }
  w.PodVec(v.gones);
}

bool Decode(ByteReader& r, DistantView& v) {
  v.entries.clear();
  v.gones.clear();
  if (!ReadVersion(r)) return false;
  uint32_t n = 0;
  if (!r.U32(n) || n > kMaxViewEntries) {
    r.ok = false;
    return false;
  }
  v.entries.resize(n);
  for (uint32_t i = 0; i < n && r.ok; i++) {
    DistantViewEntry& e = v.entries[i];
    r.Pod(e.id);
    r.U32(e.flags);
    if (e.flags & DistantViewEntry::kShape) {
      r.Str(e.defName);
      ReadLook(r, e.look);
    }
    r.Pod(e.origin);
    r.F32(e.heading);
    r.F32(e.gaitPhase);
    r.U32(e.state);
    if (e.flags & DistantViewEntry::kBand) ReadActor(r, e.actor);
    if (e.flags & DistantViewEntry::kBox) {
      r.Pod(e.boxLo);
      r.Pod(e.boxHi);
    }
  }
  r.PodVec(v.gones);
  if (v.gones.size() > kMaxViewEntries) r.ok = false;
  return r.ok;
}

size_t EncodedSize(const DistantView& v) {
  std::vector<uint8_t> tmp;
  ByteWriter w{tmp};
  Encode(w, v);
  return tmp.size();
}

}  // namespace net
