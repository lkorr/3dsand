#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The MESSAGE half of M9.2 package A: what the two peers say to each other,
// and the pacer that decides when they may say it.
//
// SCOPE. Everything here is pure value types plus encode/decode. No sockets
// (net/link.h), no game state (package B owns PlayerState, M9.3 owns ops), no
// files. That is what lets the `net-loopback` gate drive the whole protocol
// and the whole pacer on the CPU with no World, no GPU and no assets.
//
// ENDIANNESS AND ABI. The payloads are raw little-endian PODs through
// sim/bytestream.h, whose own header says it is "not a wire format". That
// caveat is about two DIFFERENT machine classes; this is two builds of this
// exe on x64 Windows talking on a LAN, and `Hello` refuses anything that is
// not bit-for-bit the same build's idea of the world. A cross-architecture
// port would replace the encoders, not the protocol.
namespace net {

// Scoped, so `MsgType::Hello` and `struct Hello` can both exist. An unscoped
// enum with a `Hello` enumerator would collide with the struct of the same
// name and force one of them to be renamed into something less obvious.
enum class MsgType : uint16_t {
  Hello = 1,
  HelloAck,
  HelloRefuse,
  TickBatch,
  ChunkSync,       // M9.3
  EntityState,     // M9.4
  ChunkPut,        // M9.5
  ChunkGet,        // M9.5
  ChunkManifest,   // M9.5
  // ---- M9.3 package C, the convergence half (net/chunksync.h) ------------
  // ADDITIVE, and deliberately appended rather than slotted beside ChunkSync:
  // the enumerator VALUES are on the wire, so inserting one in the middle
  // would renumber every message after it and make two builds of this exe
  // disagree about what a `5` means. Appending cannot.
  HashBlocks,      // per-4^3-block digest sums, published on a hash tick
  HashDrill,       // "your sum for this block differs; send me the 64"
  HashChunks,      // the 64 chunk digests of one block
  ChunkRequest,    // "send me your copy of this chunk" (to its authority)
  ChunkBusy,       // "not now" - not resident, or not quiet enough to ship
};

// Bumped when any message's layout or meaning changes. It is the FIRST field
// Hello compares, so a version skew is reported as "protocolVersion" rather
// than as whichever struct happened to move.
constexpr uint16_t kProtocolVersion = 1;

// D, the lockstep delay, in ticks. Fixed rather than adaptive: §4 finding 5
// takes "no deadlock" from a constant that both sides agree on before they
// exchange a byte, and an adaptive D is a negotiation that can fail. Four
// ticks at kTickDt is ~67 ms of tolerated one-way jitter, which is a LAN with
// room to spare.
constexpr uint32_t kOpDelayTicks = 4;

// ---- Hello --------------------------------------------------------------
//
// EVERY FACT THAT MUST BE IDENTICAL FOR TWO MACHINES TO SIMULATE THE SAME
// WORLD, in one message. This is the same identity the op record already
// refuses a replay on (sim/oprecord.h Header), plus the asset stamps that
// record does not carry because a record is replayed against the tree that
// made it and a PEER is not.
//
// The u32 block is a table (HelloU32Fields) rather than a hand-written
// comparison chain, so FirstMismatch's order and the gate's enumeration of
// fields come from ONE list. A field added to the struct but not the table
// would be silently unchecked — the static_assert in protocol.cpp on
// offsetof(Hello, playerId) is what makes that a build error instead.
struct Hello {
  // --- version ---
  uint32_t protocolVersion = kProtocolVersion;
  uint32_t recordVersion = 0;     // opstream::kRecordVersion
  // --- world constants ---
  uint32_t worldN = 0;            // kWorldN
  uint32_t chunk = 0;             // kChunk
  uint32_t voxelMetersBits = 0;   // bit pattern of kVoxelMeters
  uint32_t tickParamsBytes = 0;   // sizeof(TickParams)
  // --- seed ---
  uint32_t seed = 0;
  // --- material table ---
  uint32_t matCount = 0;
  uint32_t matHash = 0;           // FNV-1a over material names, in id order
  // --- tuning + environment stamps ---
  uint32_t tuningHash = 0;        // sandvox::TuningStamp
  uint32_t materialsHash = 0;
  uint32_t reactionsHash = 0;
  uint32_t envMap = 0;            // biomes::EnvironmentStamp
  uint32_t envBiomes = 0;
  uint32_t envTrees = 0;
  // --- input contract ---
  uint32_t tickInputVersion = 0;  // kTickInputVersion
  // NOT COMPARED, and it must be last in the u32 block (see the static_assert
  // in protocol.cpp): the two peers are supposed to have DIFFERENT player ids,
  // so comparing this would refuse every healthy connection.
  uint32_t playerId = 0;
  // Compared, after the env hashes: a different map NAME is the mismatch a
  // human can act on, and the hash that goes with it is unreadable.
  std::string mapName;

  void Encode(std::vector<uint8_t>& out) const;
  bool Decode(const uint8_t* p, size_t n);

  // The name of the first field on which `mine` and `theirs` disagree, or
  // nullptr when they agree on everything that matters.
  //
  // THE ORDER IS PART OF THE CONTRACT: version, then world constants, then
  // seed, then the material table, then the tuning/environment stamps, then
  // tickInputVersion. A build skew shows up as "protocolVersion" and not as
  // "matHash", so the refusal a user reads names the thing they should fix
  // rather than the first consequence of it.
  static const char* FirstMismatch(const Hello& mine, const Hello& theirs);
};

// The compared u32 fields, in FirstMismatch's order. Exposed so the gate can
// perturb each one by name without a second hand-maintained list that could
// drift from this one.
struct HelloU32Field {
  const char* name;
  uint32_t Hello::*member;
};
const std::vector<HelloU32Field>& HelloU32Fields();

// Fill in everything this build knows about itself. The caller supplies only
// what this header cannot see without dragging the sim in: the seed, the
// material table's count+hash, the two asset stamps, the map name and its own
// player id. Everything else (protocol/record version, kWorldN, kChunk,
// kVoxelMeters, sizeof(TickParams), kTickInputVersion) is compiled in, which
// is the point — those are exactly the fields a hand-filled Hello would get
// subtly wrong on one side.
Hello LocalHello(uint32_t seed, uint32_t matCount, uint32_t matHash,
                 uint32_t tuningHash, uint32_t materialsHash,
                 uint32_t reactionsHash, uint32_t envMap, uint32_t envBiomes,
                 uint32_t envTrees, const std::string& mapName,
                 uint32_t playerId);

// ---- HelloAck / HelloRefuse --------------------------------------------

// THE HOST'S TICK COUNTER IS THE CLOCK (§3, "Decisions that bind all three").
// `startTick` is where the client begins; it is not a suggestion, and the
// pacer is Reset() to it.
struct HelloAck {
  uint32_t startTick = 0;
  uint32_t yourPlayerId = 0;
  void Encode(std::vector<uint8_t>& out) const;
  bool Decode(const uint8_t* p, size_t n);
};

// The FIELD NAME from FirstMismatch, sent back so the refused side can print
// what to fix. A refusal carrying only "incompatible" is the message that
// makes a user re-copy their whole assets directory to fix a tuning.json.
struct HelloRefuse {
  std::string field;
  void Encode(std::vector<uint8_t>& out) const;
  bool Decode(const uint8_t* p, size_t n);
};

// ---- TickBatch ----------------------------------------------------------
//
// ONE MESSAGE PER DIRECTION PER TICK, EMPTY OR NOT. §4 finding 5: "no ops" and
// "not arrived" must be different observations, and they are only different if
// the empty case still puts a frame on the wire.
struct TickBatchHeader {
  uint32_t tick = 0;
  uint32_t playerId = 0;
  // The sender's residency window origin AS OF THIS TICK. Carried because
  // finding 3 forbids interpreting a remote CellOp without it (cellIdx is a
  // SLOT index, so an aliased chunk would be painted silently) and finding 1
  // needs both origins to decide which chunks are comparable at all.
  int32_t windowOrigin[3] = {0, 0, 0};
  float timeScale = 1.0f;
  uint32_t vizActive = 0;
  uint32_t flags = 0;     // reserved; zero on the wire today
};

// The two blobs are OPAQUE at this layer. `playerState` is package B's
// business and `ops` is M9.3's; encoding them here would put three packages'
// schemas in one file and make every one of them a protocol version bump.
struct TickBatchWire {
  TickBatchHeader h;
  std::vector<uint8_t> playerState;
  std::vector<uint8_t> ops;
  void Encode(std::vector<uint8_t>& out) const;
  bool Decode(const uint8_t* p, size_t n);
};

// ---- LockstepPacer ------------------------------------------------------
//
// A PURE VALUE TYPE. No sockets, no clock, no allocation: it is told what was
// sent and what arrived, and it answers whether the local tick may run. That
// is what lets the gate drive a 10-tick peer stall deterministically in
// microseconds, and it is why the rule "never run a tick whose batch has not
// arrived" can be a proof rather than an observation.
//
// THE THREE INVARIANTS, which package C wires into the frame loop:
//
//  1. SEND BEFORE WAIT. The batch for T+D goes out before the local side
//     blocks on the peer's batch for T. Both sides doing the reverse is the
//     deadlock §4 finding 5 is about — each waiting for a message the other
//     will only send after it stops waiting.
//  2. A BATCH EVERY TICK, empty or not. `ShouldSend()` is true exactly once
//     per tick attempt, so the peer's contiguity check has no holes in it and
//     silence unambiguously means "not arrived".
//  3. PRE-SEND D AT CONNECT. There is no separate connect path: Reset() puts
//     `sent` behind by D+1 and the SAME `while (ShouldSend())` loop emits
//     T0..T0+D before the first tick runs. A hand-written pre-send is a
//     second implementation of the send rule and drifts from it the first
//     time D moves.
//
// The steady state is then lag == D: the peer's batches run D ticks ahead of
// the tick being simulated, and D ticks of jitter are absorbed without a
// stall. That is what D BUYS, and it is asserted directly by the gate.
struct LockstepPacer {
  uint32_t D = kOpDelayTicks;

  // The tick about to run (or being waited for).
  uint32_t localTick = 0;
  // Highest CONTIGUOUS peer batch tick received. Contiguous and not merely
  // highest: TCP cannot reorder, so a gap means a dropped or duplicated batch
  // and running past it would be simulating a tick whose inputs never came.
  uint32_t peerUpTo = 0;
  bool havePeer = false;
  // Highest batch WE have sent. `haveSent` distinguishes "nothing sent" from
  // "sent tick 0", which matters because startTick may legitimately be 0.
  uint32_t sentUpTo = 0;
  bool haveSent = false;

  // Counters, for the gate and for a HUD.
  uint32_t stalls = 0;       // tick attempts that could not run
  uint32_t ticksRun = 0;
  uint32_t batchesSent = 0;
  uint32_t outOfOrder = 0;   // peer batches that broke contiguity

  // Begin at the host's clock (HelloAck::startTick). Everything is derived
  // from this one call; there is nothing else to initialise.
  void Reset(uint32_t startTick) {
    localTick = startTick;
    peerUpTo = 0;
    havePeer = false;
    sentUpTo = 0;
    haveSent = false;
    stalls = ticksRun = batchesSent = outOfOrder = 0;
  }

  // The label the next outgoing batch carries. Before anything has been sent
  // that is the start tick itself (the first of the D+1 pre-sends); after, it
  // is one past the last one — which in the steady state equals localTick + D.
  uint32_t NextToSend() const { return haveSent ? sentUpTo + 1 : localTick; }

  // Invariant 2+3 in one predicate: keep sending while we are not yet D ahead
  // of the tick we are about to run. True D+1 times at connect and exactly
  // once per tick after, and FALSE while stalled — a stalled peer must not
  // make us run our own send ahead of the world.
  bool ShouldSend() const { return NextToSend() <= localTick + D; }

  void NoteSent(uint32_t t) {
    sentUpTo = t;
    haveSent = true;
    batchesSent++;
  }

  // Record a peer batch. False (and counted) when it breaks contiguity.
  bool NotePeerBatch(uint32_t t) {
    if (!havePeer) {
      havePeer = true;
      peerUpTo = t;
      return true;
    }
    if (t != peerUpTo + 1) {
      outOfOrder++;
      return false;
    }
    peerUpTo = t;
    return true;
  }

  bool CanRun(uint32_t t) const { return havePeer && t <= peerUpTo; }
  bool CanRun() const { return CanRun(localTick); }

  void NoteRan() {
    localTick++;
    ticksRun++;
  }
  void NoteStall() { stalls++; }

  // How many ticks of peer batches are buffered ahead of the local tick. -1
  // before the first batch arrives, so "no peer yet" is not confused with
  // "peer exactly level".
  int Lag() const {
    if (!havePeer) return -1;
    return (int)((int64_t)peerUpTo - (int64_t)localTick);
  }
};

}  // namespace net
