#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

// The TRANSPORT half of M9.2 (docs/PLAN_multiplayer_m9.md §3, package A).
//
// One peer per link. That is not a simplification waiting to be generalised —
// M9 is two players by decision (§0), and "one peer" is what lets the whole
// thing be a polled, single-threaded, allocation-light object with no
// connection table, no id space and no fan-out. A third player would be a
// second Link, not a wider one.
//
// NO THREADS, and `Poll()` is the ONLY place a socket is touched. This is the
// same thread model `src/telemetry.cpp` has used since it landed, for the same
// reason: the frame loop is the only thing allowed to observe network state,
// so nothing here can introduce a scheduling-dependent outcome and rule 1
// (bit-deterministic simulation) survives contact with the network. `Send`
// appends to an outbound byte buffer; `Poll` flushes as much of it as the
// socket will take (a non-blocking `send` may take a PARTIAL write — the
// remainder stays queued, which is the bug that eats the tail of a 300 KiB
// ChunkSync if you assume otherwise); `Recv` pops one fully reassembled frame.
//
// THE WIRE FRAME is `{u32 payloadLen, u16 type, u16 ver}` then `payloadLen`
// bytes. `len` counts the PAYLOAD only, so a zero-length message is a legal
// 8-byte frame — and an empty TickBatch is sent every tick (§4 finding 5: "no
// ops" must be distinguishable from "not arrived"), so the zero case is the
// common case rather than an edge one. A frame claiming more than
// kMaxFrameBytes is a protocol error and closes the link: without that ceiling
// a corrupt length word makes the reassembler wait forever for bytes that are
// never coming, which reads as a hang rather than as a disconnect.
namespace net {

// Seconds since the first call, from a monotonic clock. Exported because the
// 3-second-silence disconnect rule (§4 finding 5) belongs to the frame loop
// (package C) and it must measure against the same clock `LinkStats` does.
double NowSeconds();

struct Msg {
  uint16_t type = 0;
  uint16_t ver = 0;
  std::vector<uint8_t> payload;
};

struct LinkStats {
  uint64_t bytesIn = 0, bytesOut = 0, msgsIn = 0, msgsOut = 0;
  // NowSeconds() at the last byte that arrived, or 0 if nothing ever has.
  // The silence timer reads this; it is BYTES and not messages on purpose, so
  // a peer that is mid-way through a large ChunkSync is not declared dead.
  double lastRecvSeconds = 0.0;
};

// Frame header: u32 len + u16 type + u16 ver.
constexpr size_t kFrameHeaderBytes = 8;
// 4 MiB. §4 finding 8 puts the worst expected tick at 64-128 KiB and slices
// ChunkSync at 4 KiB, so this is two orders of magnitude of headroom and is a
// sanity ceiling rather than a budget.
constexpr uint32_t kMaxFrameBytes = 4u * 1024u * 1024u;

class Link {
 public:
  virtual ~Link();
  // Queue one message. False when the link has already failed, or when `n`
  // exceeds kMaxFrameBytes (nothing is queued in that case — a refused send
  // must not leave half a frame in the stream).
  virtual bool Send(uint16_t type, uint16_t ver, const uint8_t* p, size_t n) = 0;
  // Pop one fully reassembled message. False when none is ready. Non-blocking
  // and does NOT touch the socket: everything comes out of what Poll() read.
  virtual bool Recv(Msg& out) = 0;
  // Accept / connect-completion / read / write-flush. The only socket call
  // site in the engine's network path.
  virtual void Poll() = 0;
  virtual bool Connected() const = 0;
  virtual void Close() = 0;
  virtual const LinkStats& Stats() const = 0;
  // Empty while healthy; the reason the link failed once it has. A string and
  // not an enum because every consumer of it prints it.
  virtual const std::string& Error() const = 0;
};

// Shared implementation of everything that is NOT the transport: the outbound
// byte queue, the reassembler, the stats and the error latch. TcpLink and the
// loopback pair therefore run the SAME framing code, which is what makes the
// loopback arm of the `net-loopback` gate a real test of the wire format and
// not a test of a second implementation of it.
class LinkBase : public Link {
 public:
  bool Send(uint16_t type, uint16_t ver, const uint8_t* p, size_t n) override;
  bool Recv(Msg& out) override;
  const LinkStats& Stats() const override { return stats_; }
  const std::string& Error() const override { return err_; }

 protected:
  // Feed bytes that arrived from the transport; cuts complete frames off the
  // front and queues them. Latches a protocol error on an oversized frame.
  void Ingest(const uint8_t* p, size_t n);
  void Fail(const std::string& why);
  // Bytes waiting to go out (queued minus already handed to the transport).
  size_t Pending() const { return out_.size() - outSent_; }

  std::vector<uint8_t> out_;   // queued, not yet handed to the transport
  size_t outSent_ = 0;         // bytes of out_ the transport has taken
  std::vector<uint8_t> in_;    // received, not yet framed
  size_t inOff_ = 0;
  std::deque<Msg> rx_;
  LinkStats stats_;
  std::string err_;
  bool failed_ = false;
};

// Direct-IP TCP, non-blocking, TCP_NODELAY. The only real transport.
//
// TCP_NODELAY IS NOT OPTIONAL HERE and telemetry.cpp is not the precedent for
// it: a TickBatch is ~150 B (§4 finding 8) and Nagle holds a small write until
// the previous one is ACKed, so a stream of one small message per tick
// coalesces into bursts and the lockstep pacer stalls for the delay. The gate
// reads the flag back with getsockopt rather than trusting the setsockopt
// return, because a failed set on a not-yet-connected socket is silent.
class TcpLink : public LinkBase {
 public:
  TcpLink();
  ~TcpLink() override;
  TcpLink(const TcpLink&) = delete;
  TcpLink& operator=(const TcpLink&) = delete;

  // Bind + listen on 127.0.0.1. `port == 0` asks the OS for an ephemeral one;
  // read it back with ListenPort(). Accepts exactly ONE peer — a second
  // connection is accepted and immediately closed rather than left pending, so
  // a stray client gets a reset instead of a silent hang.
  bool Listen(uint16_t port);
  uint16_t ListenPort() const { return listenPort_; }
  bool Listening() const { return listen_ != -1; }

  // Non-blocking connect; completion arrives in a later Poll().
  bool Connect(const std::string& ip, uint16_t port);

  void Poll() override;
  bool Connected() const override { return connected_; }
  void Close() override;

  // getsockopt(IPPROTO_TCP, TCP_NODELAY) on the live peer socket. False when
  // there is no peer. Exists for the gate; costs one syscall.
  bool NoDelay() const;

 private:
  void Accept();
  void PollConnecting();
  void ReadPeer();
  void FlushOut();
  void DropPeer(const std::string& why);

  intptr_t listen_ = -1;
  intptr_t peer_ = -1;
  bool connecting_ = false;
  bool connected_ = false;
  uint16_t listenPort_ = 0;
};

// Two Links wired to each other through in-memory byte pipes, for gates and
// for a future in-process two-player harness. Not a mock: it carries the same
// framed bytes through the same reassembler, and it hands them over a BOUNDED
// number at a time (kLoopbackChunkBytes) so a 70 KiB message genuinely crosses
// dozens of Poll()s. A loopback that delivered whole messages atomically would
// pass while the partial-read path was broken, which is the one thing the
// loopback arm exists to cover.
constexpr size_t kLoopbackChunkBytes = 1500;

struct LoopbackPair {
  std::unique_ptr<Link> a, b;
};
LoopbackPair MakeLoopback();

}  // namespace net
