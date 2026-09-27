// debrissync.cpp — the codecs for the debris wire records (M9.4-C).
//
// Pure serialization. No sockets, no state, no dependency on the rest of
// src/net/ — see the header for why the records live apart from DebrisSystem.
//
// EVERY RECORD IS VERSION-PREFIXED, not just the stream. A stream-level version
// works when one process writes the whole file at once (worldio.h); it does not
// when records from several senders are interleaved in one datagram and any one
// of them may be a kind the reader has never seen. One u32 per record buys the
// property that a decode failure is CONTAINED: the reader stops, `ok` goes
// false, and the caller drops that datagram rather than mis-parsing the rest of
// it as body geometry.

#include "net/debrissync.h"

#include "sim/bytestream.h"

namespace net {
namespace {

// The vectors go through PodVec, which writes a count and then the raw bytes.
// That is the same layout DebrisSystem::SaveState uses for the same two
// lattices and it is sound for the same stated reason: LAN play between two
// builds of one binary. It is the ONE assumption in this file, and it is
// written down here rather than left implicit so that the day a peer is a
// different build, this is the line that has to change.
void WriteXf(ByteWriter& w, const BodyTransform& xf) {
  w.F32(xf.pos.x);
  w.F32(xf.pos.y);
  w.F32(xf.pos.z);
  for (float q : xf.quat) w.F32(q);
}
bool ReadXf(ByteReader& r, BodyTransform& xf) {
  r.F32(xf.pos.x);
  r.F32(xf.pos.y);
  r.F32(xf.pos.z);
  for (float& q : xf.quat) r.F32(q);
  return r.ok;
}
void WriteVec(ByteWriter& w, const Vec3& v) {
  w.F32(v.x);
  w.F32(v.y);
  w.F32(v.z);
}
bool ReadVec(ByteReader& r, Vec3& v) {
  r.F32(v.x);
  r.F32(v.y);
  r.F32(v.z);
  return r.ok;
}

// The announce body, without its own version word: BodyHandoff embeds a whole
// announce and must not pay (or re-check) the version twice.
void WriteAnnounce(ByteWriter& w, const BodyAnnounce& a) {
  w.Pod(a.globalId);
  w.U32(a.owner);
  WriteXf(w, a.xf);
  w.PodVec(a.voxels);
  w.PodVec(a.skinVoxels);
  w.U32(a.physScale);
  w.U32(a.skinScale);
  w.U32(a.dye);
  w.U32(a.hadMicro);
  w.U32(a.bleedMat);
  w.U32(a.dead);
  WriteItemInstance(w, a.item, kItemFmtStopper);
}
bool ReadAnnounce(ByteReader& r, BodyAnnounce& a) {
  r.Pod(a.globalId);
  r.U32(a.owner);
  ReadXf(r, a.xf);
  r.PodVec(a.voxels);
  r.PodVec(a.skinVoxels);
  r.U32(a.physScale);
  r.U32(a.skinScale);
  r.U32(a.dye);
  r.U32(a.hadMicro);
  r.U32(a.bleedMat);
  r.U32(a.dead);
  ReadItemInstance(r, a.item, kItemFmtStopper);
  // A zero pitch would divide by zero in every world-space conversion the
  // receiving side makes off this lattice. Clamped here rather than asserted
  // because a hostile or corrupt payload is a thing to survive, not to crash
  // on — the same posture ByteReader's sticky `ok` takes.
  if (a.physScale == 0) a.physScale = 1;
  if (a.skinScale == 0) a.skinScale = 1;
  return r.ok;
}

// Version gate, shared by every Decode below.
bool CheckVersion(ByteReader& r) {
  uint32_t v = 0;
  if (!r.U32(v)) return false;
  if (v != kDebrisWireVersion) {
    r.ok = false;  // poison the reader: the rest of this datagram is not ours
    return false;
  }
  return true;
}

}  // namespace

void Encode(std::vector<uint8_t>& out, const BodyAnnounce& rec) {
  ByteWriter w{out};
  w.U32(kDebrisWireVersion);
  WriteAnnounce(w, rec);
}
bool Decode(ByteReader& r, BodyAnnounce& out) {
  if (!CheckVersion(r)) return false;
  return ReadAnnounce(r, out);
}

void Encode(std::vector<uint8_t>& out, const BodyPose& rec) {
  ByteWriter w{out};
  w.U32(kDebrisWireVersion);
  w.Pod(rec.globalId);
  w.U32(rec.tick);
  WriteXf(w, rec.xf);
  WriteVec(w, rec.vel);
}
bool Decode(ByteReader& r, BodyPose& out) {
  if (!CheckVersion(r)) return false;
  r.Pod(out.globalId);
  r.U32(out.tick);
  ReadXf(r, out.xf);
  ReadVec(r, out.vel);
  return r.ok;
}

void Encode(std::vector<uint8_t>& out, const BodyHandoff& rec) {
  ByteWriter w{out};
  w.U32(kDebrisWireVersion);
  WriteAnnounce(w, rec.announce);
  WriteVec(w, rec.vel);
  WriteVec(w, rec.angVel);
  w.U32(rec.newOwner);
}
bool Decode(ByteReader& r, BodyHandoff& out) {
  if (!CheckVersion(r)) return false;
  ReadAnnounce(r, out.announce);
  ReadVec(r, out.vel);
  ReadVec(r, out.angVel);
  r.U32(out.newOwner);
  return r.ok;
}

void Encode(std::vector<uint8_t>& out, const BodyGone& rec) {
  ByteWriter w{out};
  w.U32(kDebrisWireVersion);
  w.Pod(rec.globalId);
  w.U32(rec.reason);
}
bool Decode(ByteReader& r, BodyGone& out) {
  if (!CheckVersion(r)) return false;
  r.Pod(out.globalId);
  r.U32(out.reason);
  return r.ok;
}

void Encode(std::vector<uint8_t>& out, const ItemTake& rec) {
  ByteWriter w{out};
  w.U32(kDebrisWireVersion);
  w.Pod(rec.globalId);
  w.U32(rec.byPlayer);
}
bool Decode(ByteReader& r, ItemTake& out) {
  if (!CheckVersion(r)) return false;
  r.Pod(out.globalId);
  r.U32(out.byPlayer);
  return r.ok;
}

void Encode(std::vector<uint8_t>& out, const ItemGrant& rec) {
  ByteWriter w{out};
  w.U32(kDebrisWireVersion);
  w.Pod(rec.globalId);
  w.U32(rec.toPlayer);
  WriteItemInstance(w, rec.item, kItemFmtStopper);
  w.U32(rec.granted);
}
bool Decode(ByteReader& r, ItemGrant& out) {
  if (!CheckVersion(r)) return false;
  r.Pod(out.globalId);
  r.U32(out.toPlayer);
  ReadItemInstance(r, out.item, kItemFmtStopper);
  r.U32(out.granted);
  return r.ok;
}

}  // namespace net
