#include "net/protocol.h"

#include <cstddef>
#include <cstring>

#include "sim/bytestream.h"
#include "sim/oprecord.h"    // sandvox::opstream::kRecordVersion
#include "sim/tickinput.h"   // kTickInputVersion
#include "sim/world.h"       // kWorldN, kChunk, kVoxelMeters, TickParams

namespace net {
namespace {

// The compared u32 block, in the order FirstMismatch walks it. ONE list: it is
// both the comparison order and what the gate enumerates, so a field cannot be
// checked here and forgotten there.
const std::vector<HelloU32Field> kHelloU32 = {
    // version first — a build skew must be reported as itself, not as the
    // first world constant it happens to have moved.
    {"protocolVersion", &Hello::protocolVersion},
    {"recordVersion", &Hello::recordVersion},
    // world constants
    {"worldN", &Hello::worldN},
    {"chunk", &Hello::chunk},
    {"voxelMetersBits", &Hello::voxelMetersBits},
    {"tickParamsBytes", &Hello::tickParamsBytes},
    // seed
    {"seed", &Hello::seed},
    // material table
    {"matCount", &Hello::matCount},
    {"matHash", &Hello::matHash},
    // tuning + environment stamps
    {"tuningHash", &Hello::tuningHash},
    {"materialsHash", &Hello::materialsHash},
    {"reactionsHash", &Hello::reactionsHash},
    {"envMap", &Hello::envMap},
    {"envBiomes", &Hello::envBiomes},
    {"envTrees", &Hello::envTrees},
    // the input contract, last
    {"tickInputVersion", &Hello::tickInputVersion},
};

// A NEW u32 IN Hello THAT IS NOT IN kHelloU32 IS A BUILD ERROR, not a silent
// hole in the handshake. Every compared u32 precedes `playerId` in the struct
// and none of them needs padding between them, so playerId's offset IS the
// table's length in bytes. Add a field without adding its row and this fails
// to compile with the message that says what to do.
static_assert(offsetof(Hello, playerId) == sizeof(uint32_t) * 16,
              "add the new Hello u32 to kHelloU32 (and to Encode/Decode), and "
              "keep playerId last in the u32 block");

uint32_t VoxelMetersBits() {
  // The same bit pattern sim/oprecord.cpp writes into its record header. It is
  // re-derived rather than exported because oprecord.cpp's helper is
  // file-local, and this is three lines with a static_assert guarding it.
  uint32_t bits = 0;
  static_assert(sizeof(bits) == sizeof(kVoxelMeters), "float32 expected");
  std::memcpy(&bits, &kVoxelMeters, sizeof bits);
  return bits;
}

}  // namespace

const std::vector<HelloU32Field>& HelloU32Fields() { return kHelloU32; }

void Hello::Encode(std::vector<uint8_t>& out) const {
  ByteWriter w{out};
  for (const HelloU32Field& f : kHelloU32) w.U32(this->*f.member);
  w.U32(playerId);
  w.Str(mapName);
}

bool Hello::Decode(const uint8_t* p, size_t n) {
  ByteReader r{p, n};
  for (const HelloU32Field& f : kHelloU32) r.U32(this->*f.member);
  r.U32(playerId);
  r.Str(mapName);
  return r.ok;
}

const char* Hello::FirstMismatch(const Hello& mine, const Hello& theirs) {
  for (const HelloU32Field& f : kHelloU32)
    if (mine.*f.member != theirs.*f.member) return f.name;
  // After the env hashes on purpose: if the maps differ at all the hashes
  // already said so, and this is the version of that answer a human can act
  // on. playerId is deliberately absent — the peers are SUPPOSED to differ.
  if (mine.mapName != theirs.mapName) return "mapName";
  return nullptr;
}

Hello LocalHello(uint32_t seed, uint32_t matCount, uint32_t matHash,
                 uint32_t tuningHash, uint32_t materialsHash,
                 uint32_t reactionsHash, uint32_t envMap, uint32_t envBiomes,
                 uint32_t envTrees, const std::string& mapName,
                 uint32_t playerId) {
  Hello h;
  h.protocolVersion = kProtocolVersion;
  h.recordVersion = sandvox::opstream::kRecordVersion;
  h.worldN = kWorldN;
  h.chunk = kChunk;
  h.voxelMetersBits = VoxelMetersBits();
  h.tickParamsBytes = (uint32_t)sizeof(TickParams);
  h.seed = seed;
  h.matCount = matCount;
  h.matHash = matHash;
  h.tuningHash = tuningHash;
  h.materialsHash = materialsHash;
  h.reactionsHash = reactionsHash;
  h.envMap = envMap;
  h.envBiomes = envBiomes;
  h.envTrees = envTrees;
  h.tickInputVersion = kTickInputVersion;
  h.playerId = playerId;
  h.mapName = mapName;
  return h;
}

void HelloAck::Encode(std::vector<uint8_t>& out) const {
  ByteWriter w{out};
  w.U32(startTick);
  w.U32(yourPlayerId);
}

bool HelloAck::Decode(const uint8_t* p, size_t n) {
  ByteReader r{p, n};
  r.U32(startTick);
  r.U32(yourPlayerId);
  return r.ok;
}

void HelloRefuse::Encode(std::vector<uint8_t>& out) const {
  ByteWriter w{out};
  w.Str(field);
}

bool HelloRefuse::Decode(const uint8_t* p, size_t n) {
  ByteReader r{p, n};
  r.Str(field);
  return r.ok;
}

void TickBatchWire::Encode(std::vector<uint8_t>& out) const {
  ByteWriter w{out};
  // Field by field rather than one Pod of the struct. The struct happens to
  // have no padding today (nine 4-byte members), but a single `Pod` would make
  // the WIRE LAYOUT a function of the compiler's struct layout — add one
  // uint16_t flag later and the format silently gains two bytes of
  // uninitialised stack, with no version bump and no test that could see it.
  w.U32(h.tick);
  w.U32(h.playerId);
  for (int i = 0; i < 3; i++) w.Pod(h.windowOrigin[i]);
  w.F32(h.timeScale);
  w.U32(h.vizActive);
  w.U32(h.flags);
  w.PodVec(playerState);
  w.PodVec(ops);
}

bool TickBatchWire::Decode(const uint8_t* p, size_t n) {
  ByteReader r{p, n};
  r.U32(h.tick);
  r.U32(h.playerId);
  for (int i = 0; i < 3; i++) r.Pod(h.windowOrigin[i]);
  r.F32(h.timeScale);
  r.U32(h.vizActive);
  r.U32(h.flags);
  r.PodVec(playerState);
  r.PodVec(ops);
  return r.ok;
}

}  // namespace net
