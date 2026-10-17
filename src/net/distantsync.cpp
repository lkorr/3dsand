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

#include "game/distant.h"   // D7b: the edge records DistantEdgeWire carries

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


// ============================================================================
// D7b: THE EDGE ACROSS A PEER'S WINDOW
// ============================================================================
namespace {

void WriteStrike(ByteWriter& w, const StrikeProfile& s) {
  w.F32(s.cut);
  w.F32(s.blunt);
  w.F32(s.bluntCarve);
  w.F32(s.armorBreak);
  w.F32(s.bite);
  w.F32(s.biteRadius);
  w.F32(s.biteStain);
  w.F32(s.biteMouthful);
  w.F32(s.biteForce);
  w.U32(s.biteCarrion ? 1u : 0u);
  w.F32(s.push);
  w.F32(s.biteSwallow);
  w.U32((uint32_t)s.infectMat | ((uint32_t)s.infectStain << 16));
}
void ReadStrike(ByteReader& r, StrikeProfile& s) {
  r.F32(s.cut);
  r.F32(s.blunt);
  r.F32(s.bluntCarve);
  r.F32(s.armorBreak);
  r.F32(s.bite);
  r.F32(s.biteRadius);
  r.F32(s.biteStain);
  r.F32(s.biteMouthful);
  r.F32(s.biteForce);
  uint32_t b = 0;
  r.U32(b);
  s.biteCarrion = b != 0;
  r.F32(s.push);
  r.F32(s.biteSwallow);
  uint32_t inf = 0;
  r.U32(inf);
  s.infectMat = (uint16_t)(inf & 0xFFFFu);
  s.infectStain = (uint16_t)(inf >> 16);
}

void WriteKit(ByteWriter& w, const distant::DistantKit& k) {
  w.Pod(k.id);
  w.Str(k.def);
  w.Str(k.profile);
  w.U32((uint32_t)k.styles.size());
  for (const distant::DistantStyle& s : k.styles) {
    w.U32((uint32_t)(int32_t)s.style);
    w.U32((uint32_t)(int32_t)s.form);
    w.Str(s.name);
    w.Str(s.weapon);
    w.U32((s.natural ? 1u : 0u) | (s.held ? 2u : 0u) | (s.fallback ? 4u : 0u) |
          ((uint32_t)s.kind << 8));
    w.F32(s.reach);
    w.U32((uint32_t)(int32_t)s.impactTicks);
    w.U32((uint32_t)(int32_t)s.totalTicks);
    WriteStrike(w, s.strike);
  }
  w.U32((uint32_t)(int32_t)k.closeStyle);
  w.F32(k.closeRange);
  w.F32(k.slack);
  w.U32((uint32_t)k.guard | (k.bleeds ? 0x100u : 0u) | (k.absorbRuined ? 0x200u : 0u) |
        (k.ranged ? 0x400u : 0u));
  w.F32(k.arrowDamage);
  w.F32(k.arrowSpeed);
  w.U32((uint32_t)(int32_t)k.shots);
  w.Str(k.ammo);
}
// The styles of a kit are a handful; a hostile count must not reserve a heap.
constexpr uint32_t kMaxKitStyles = 64;
constexpr uint32_t kMaxEdgeRecords = 4096;
void ReadKit(ByteReader& r, distant::DistantKit& k) {
  r.Pod(k.id);
  r.Str(k.def);
  r.Str(k.profile);
  uint32_t n = 0;
  r.U32(n);
  if (n > kMaxKitStyles) {
    r.ok = false;
    return;
  }
  k.styles.resize(n);
  for (distant::DistantStyle& s : k.styles) {
    uint32_t v = 0;
    r.U32(v);
    s.style = (int)(int32_t)v;
    r.U32(v);
    s.form = (int)(int32_t)v;
    r.Str(s.name);
    r.Str(s.weapon);
    uint32_t bits = 0;
    r.U32(bits);
    s.natural = (bits & 1u) != 0;
    s.held = (bits & 2u) != 0;
    s.fallback = (bits & 4u) != 0;
    const uint32_t kind = (bits >> 8) & 0xFFu;
    s.kind = kind < (uint32_t)distant::StrikeKind::Count ? (distant::StrikeKind)kind
                                                          : distant::StrikeKind::Cut;
    r.F32(s.reach);
    r.U32(v);
    s.impactTicks = (int)(int32_t)v;
    r.U32(v);
    s.totalTicks = (int)(int32_t)v;
    ReadStrike(r, s.strike);
  }
  uint32_t v = 0;
  r.U32(v);
  k.closeStyle = (int)(int32_t)v;
  r.F32(k.closeRange);
  r.F32(k.slack);
  uint32_t bits = 0;
  r.U32(bits);
  const uint32_t g = bits & 0xFFu;
  k.guard = g < (uint32_t)distant::GuardClass::Count ? (distant::GuardClass)g
                                                       : distant::GuardClass::None;
  k.bleeds = (bits & 0x100u) != 0;
  k.absorbRuined = (bits & 0x200u) != 0;
  k.ranged = (bits & 0x400u) != 0;
  r.F32(k.arrowDamage);
  r.F32(k.arrowSpeed);
  r.U32(v);
  k.shots = (int)(int32_t)v;
  r.Str(k.ammo);
  k.limbs.clear();
}

void WriteEdgeActor(ByteWriter& w, const ai::Actor& a) {
  w.Pod(a.id);
  DistantViewActor v;
  v.centre = a.centre;
  v.radius = a.radius;
  v.height = a.height;
  v.hpFrac = a.hpFrac;
  v.heading = a.heading;
  v.immersion = a.immersion;
  v.reach = a.reach;
  v.faction = a.faction;
  v.targetId = a.targetId;
  v.velocity = a.velocity;
  v.armed = a.armed ? 1 : 0;
  v.hostile = a.hostile ? 1 : 0;
  v.bleeding = a.bleeding ? 1 : 0;
  v.action = (uint8_t)a.action;
  WriteActor(w, v);
}
void ReadEdgeActor(ByteReader& r, ai::Actor& a) {
  a = ai::Actor{};
  r.Pod(a.id);
  DistantViewActor v;
  ReadActor(r, v);
  // ApplyDistantView's rule for a view actor: these fields, the rest at their
  // "unknown and harmless" defaults.
  a.centre = v.centre;
  a.radius = v.radius;
  a.height = v.height;
  a.faction = v.faction;
  a.alive = true;
  a.armed = v.armed != 0;
  a.hpFrac = v.hpFrac;
  a.heading = v.heading;
  a.targetId = v.targetId;
  a.hostile = v.hostile != 0;
  a.immersion = v.immersion;
  a.velocity = v.velocity;
  a.reach = v.reach;
  a.bleeding = v.bleeding != 0;
  a.action = v.action <= (uint8_t)ai::Action::Recover ? (ai::Action)v.action : ai::Action::None;
}

template <class T, class F>
void WriteList(ByteWriter& w, const std::vector<T>& v, F one) {
  w.U32((uint32_t)v.size());
  for (const T& x : v) one(w, x);
}
template <class T, class F>
void ReadList(ByteReader& r, std::vector<T>& v, F one) {
  uint32_t n = 0;
  r.U32(n);
  if (!r.ok || n > kMaxEdgeRecords) {
    r.ok = false;
    return;
  }
  v.resize(n);
  for (T& x : v) {
    one(r, x);
    if (!r.ok) return;
  }
}
void Fx3(ByteWriter& w, const int32_t (&v)[3]) {
  for (int c = 0; c < 3; c++) w.U32((uint32_t)v[c]);
}
void Fx3(ByteReader& r, int32_t (&v)[3]) {
  for (int c = 0; c < 3; c++) {
    uint32_t u = 0;
    r.U32(u);
    v[c] = (int32_t)u;
  }
}

}  // namespace

void Encode(ByteWriter& w, const ::DistantEdgeWire& e) {
  w.U32(kDistantSyncVersion);
  WriteList(w, e.actors, WriteEdgeActor);
  WriteList(w, e.blows, [](ByteWriter& w, const NearEdgeBlow& b) {
    w.Pod(b.attacker);
    w.Pod(b.target);
    w.U32(b.tick);
    w.Pod(b.from);
    WriteKit(w, b.kit);
    w.U32((uint32_t)(int32_t)b.style);
    w.F32(b.dist);
  });
  WriteList(w, e.arrowsOut, [](ByteWriter& w, const NearArrowOut& o) {
    w.Pod(o.shooter);
    w.Pod(o.target);
    w.U32(o.tick);
    w.Pod(o.from);
    w.Pod(o.exit);
    w.F32(o.speed);
    w.F32(o.damage);
    w.Str(o.ammo);
  });
  WriteList(w, e.boxHits, [](ByteWriter& w, const NearArrowBoxHit& h) {
    w.Pod(h.shooter);
    w.Pod(h.target);
    w.U32(h.tick);
    w.Pod(h.hit.id);
    w.F32(h.hit.t);
    w.Pod(h.hit.at);
    for (int c = 0; c < 3; c++) w.F32(h.hit.local[c]);
    for (int c = 0; c < 3; c++) w.F32(h.hit.dir[c]);
    w.F32(h.damage);
    w.Str(h.ammo);
  });
  WriteList(w, e.shotsOut, [](ByteWriter& w, const NearShotOut& s) {
    w.Pod(s.shooter);
    w.U32(s.tick);
    Fx3(w, s.pos);
    Fx3(w, s.vel);
    w.U32(s.seed);
    w.U32((uint32_t)(int32_t)s.life);
    w.F32(s.damage);
    w.Str(s.ammo);
  });
  WriteList(w, e.arrowsIn, [](ByteWriter& w, const DistantArrowIn& a) {
    w.Pod(a.shooter);
    w.Pod(a.target);
    w.U32(a.tick);
    Fx3(w, a.pos);
    Fx3(w, a.vel);
    w.U32(a.seed);
    w.Str(a.ammo);
    w.U32((uint32_t)(int32_t)a.life);
  });
}

bool Decode(ByteReader& r, ::DistantEdgeWire& e) {
  e.Clear();
  if (!ReadVersion(r)) return false;
  ReadList(r, e.actors, ReadEdgeActor);
  ReadList(r, e.blows, [](ByteReader& r, NearEdgeBlow& b) {
    r.Pod(b.attacker);
    r.Pod(b.target);
    r.U32(b.tick);
    r.Pod(b.from);
    ReadKit(r, b.kit);
    uint32_t v = 0;
    r.U32(v);
    b.style = (int)(int32_t)v;
    r.F32(b.dist);
  });
  ReadList(r, e.arrowsOut, [](ByteReader& r, NearArrowOut& o) {
    r.Pod(o.shooter);
    r.Pod(o.target);
    r.U32(o.tick);
    r.Pod(o.from);
    r.Pod(o.exit);
    r.F32(o.speed);
    r.F32(o.damage);
    r.Str(o.ammo);
  });
  ReadList(r, e.boxHits, [](ByteReader& r, NearArrowBoxHit& h) {
    r.Pod(h.shooter);
    r.Pod(h.target);
    r.U32(h.tick);
    r.Pod(h.hit.id);
    r.F32(h.hit.t);
    r.Pod(h.hit.at);
    for (int c = 0; c < 3; c++) r.F32(h.hit.local[c]);
    for (int c = 0; c < 3; c++) r.F32(h.hit.dir[c]);
    r.F32(h.damage);
    r.Str(h.ammo);
  });
  ReadList(r, e.shotsOut, [](ByteReader& r, NearShotOut& s) {
    r.Pod(s.shooter);
    r.U32(s.tick);
    Fx3(r, s.pos);
    Fx3(r, s.vel);
    r.U32(s.seed);
    uint32_t v = 0;
    r.U32(v);
    s.life = (int)(int32_t)v;
    r.F32(s.damage);
    r.Str(s.ammo);
  });
  ReadList(r, e.arrowsIn, [](ByteReader& r, DistantArrowIn& a) {
    r.Pod(a.shooter);
    r.Pod(a.target);
    r.U32(a.tick);
    Fx3(r, a.pos);
    Fx3(r, a.vel);
    r.U32(a.seed);
    r.Str(a.ammo);
    uint32_t v = 0;
    r.U32(v);
    a.life = (int)(int32_t)v;
  });
  return r.ok;
}

}  // namespace net
