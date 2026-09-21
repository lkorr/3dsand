// selftest_net.cpp — gate `net-loopback`: the transport, the handshake and
// the lockstep pacer (docs/PLAN_multiplayer_m9.md §3, package A).
//
// CPU-ONLY. No world, no GPU, no assets, no fixtures — so `--gate
// net-loopback` alone is the whole iteration loop for the network layer, which
// is what CLAUDE.md's "authoring cheap-to-verify work" asks for. It ignores
// `Ctx` entirely, exactly as `tick-input` does.
//
// FOUR CLAIMS, and each is stated so it can fail:
//
//  (a) FRAMING SURVIVES PARTIAL DELIVERY. 1,000 messages of mixed sizes
//      (0 .. 70 KiB) through MakeLoopback(), which hands bytes over 1,500 at a
//      time — so a 70 KiB message crosses ~48 Poll()s and reassembly is
//      genuinely exercised. Compared BYTE FOR BYTE and IN ORDER: a length-and-
//      type check would pass while the payload was shifted by the header.
//  (b) THE REAL SOCKET. TcpLink listening on 127.0.0.1:0 and connecting to the
//      ephemeral port it reports, both ends in this process, 200 messages
//      including one 300 KiB (which forces the partial-WRITE path that the
//      memory pipe cannot reach), with TCP_NODELAY read back by getsockopt.
//      ADVISORY IF THE SOCKET CANNOT BE HAD: the plan's kill criterion says a
//      firewall-blocked loopback listen must not fail the gate, because (a)
//      covers the framing and the two-process smoke covers the socket. It is
//      reported loudly in the detail line either way.
//  (c) THE REFUSAL NAMES THE FIELD. Every compared field of Hello, perturbed
//      one at a time, must be the one FirstMismatch names — and two perturbed
//      at once must name the EARLIER of them, which is what makes the fixed
//      order a contract instead of an accident. `playerId` perturbed must
//      name nothing, since the two peers are supposed to differ there.
//  (d) THE PACER NEVER RUNS A TICK WITHOUT ITS BATCH. Driven with a scripted
//      peer, including a stall long enough to drain the D-tick buffer and then
//      stall the local side for exactly 10 ticks. The buffer absorbing exactly
//      D of the peer's missing ticks is the claim; it is what D is FOR.
//
// WHY THE LOOPBACK PAIR IS NOT A MOCK: it runs the same LinkBase reassembler
// and the same wire frame as TcpLink. If it had its own framing, (a) would be
// testing a second implementation and would keep passing while the shipped one
// was broken — the "fixture that measures itself" failure.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "net/link.h"
#include "net/protocol.h"
#include "test/selftest.h"

namespace selftest {
namespace {

// A tiny deterministic LCG. Not the sim's RNG and deliberately not hash3:
// nothing here is hashed state, and the gate wants a repeatable size/payload
// script that is obviously independent of anything the sim does.
struct Lcg {
  uint32_t s;
  uint32_t Next() { return s = s * 1664525u + 1013904223u; }
  uint32_t Below(uint32_t n) { return n ? Next() % n : 0u; }
};

// Message `i`'s payload, built from its index alone so the receiver's copy can
// be checked against a freshly generated one without keeping 35 MB of sent
// bytes alive. Every byte depends on BOTH i and the offset, so a payload that
// arrived shifted, truncated or swapped with a neighbour's fails.
void MakePayload(uint32_t i, size_t n, std::vector<uint8_t>& out) {
  out.resize(n);
  uint32_t x = i * 2654435761u + 1u;
  for (size_t k = 0; k < n; k++) {
    x = x * 1664525u + 1013904223u;
    out[k] = (uint8_t)((x >> 17) ^ (uint8_t)(i + k));
  }
}

// Sizes: mostly small (a TickBatch is ~150 B), every sixteenth large enough to
// cross many Poll chunks. "Mixed 0..70 KiB" with the small case dominating is
// the real traffic shape, and it keeps the gate at milliseconds.
size_t ScriptSize(uint32_t i, Lcg& rng) {
  if (i == 0) return 0;                       // the empty frame, explicitly
  if (i % 16 == 15) return 40960 + rng.Below(30000);
  return rng.Below(512);
}

// ---- (a) loopback framing ----------------------------------------------

bool SubtestLoopback(int& sent, int& got, int& firstBad, std::string& why) {
  net::LoopbackPair lb = net::MakeLoopback();
  constexpr uint32_t kCount = 1000;
  Lcg rng{0x5EEDBEEFu};
  std::vector<size_t> sizes(kCount);
  for (uint32_t i = 0; i < kCount; i++) sizes[i] = ScriptSize(i, rng);

  // A frame over the ceiling must be REFUSED and must queue nothing. Checked
  // first, on a clean link: if a refused send had left a header in the stream
  // every assertion below would fail for the wrong reason.
  std::vector<uint8_t> huge(1);
  if (lb.a->Send(1, 1, huge.data(), (size_t)net::kMaxFrameBytes + 1)) {
    why = "oversized Send was accepted";
    return false;
  }
  if (lb.a->Stats().bytesOut != 0) {
    why = "refused Send queued bytes";
    return false;
  }

  std::vector<uint8_t> pay, want;
  uint32_t nextSend = 0, nextRecv = 0;
  firstBad = -1;
  // Interleave sending and polling rather than queueing all 1,000 first: that
  // is how the frame loop will use it, and it keeps both directions' buffers
  // at realistic sizes instead of one 3 MB blob that would never exercise the
  // "frame straddles two reads" case at every size.
  int guard = 0;
  while (nextRecv < kCount && guard++ < 2000000) {
    if (nextSend < kCount && nextSend < nextRecv + 8) {
      MakePayload(nextSend, sizes[nextSend], pay);
      // type/ver vary so a reassembler that read the header at a fixed offset
      // (or dropped it) cannot pass by always reporting the same pair.
      if (!lb.a->Send((uint16_t)(nextSend % 9 + 1), (uint16_t)(nextSend % 3),
                      pay.data(), pay.size())) {
        why = "Send refused a legal message";
        return false;
      }
      nextSend++;
    }
    lb.a->Poll();
    lb.b->Poll();
    net::Msg m;
    while (lb.b->Recv(m)) {
      MakePayload(nextRecv, sizes[nextRecv], want);
      const bool ok = m.type == (uint16_t)(nextRecv % 9 + 1) &&
                      m.ver == (uint16_t)(nextRecv % 3) &&
                      m.payload.size() == want.size() &&
                      (want.empty() || std::memcmp(m.payload.data(), want.data(),
                                                   want.size()) == 0);
      if (!ok && firstBad < 0) firstBad = (int)nextRecv;
      nextRecv++;
    }
  }
  sent = (int)nextSend;
  got = (int)nextRecv;
  if (nextRecv != kCount) {
    why = "loopback did not deliver every message";
    return false;
  }
  if (firstBad >= 0) {
    why = "loopback payload mismatch";
    return false;
  }
  // THE REVERSE DIRECTION, so "a pair" means a pair. One message is enough:
  // the framing is already proven, what is unproven is that b's tx pipe is
  // wired to a's rx pipe and not to its own.
  MakePayload(4242, 77, pay);
  lb.b->Send(7, 2, pay.data(), pay.size());
  for (int k = 0; k < 8; k++) { lb.b->Poll(); lb.a->Poll(); }
  net::Msg back;
  if (!lb.a->Recv(back) || back.type != 7 || back.ver != 2 ||
      back.payload != pay) {
    why = "loopback reverse direction failed";
    return false;
  }
  return true;
}

// ---- (b) the real socket ------------------------------------------------

bool SubtestTcp(int& got, bool& available, bool& noDelay, std::string& why) {
  available = false;
  noDelay = false;
  got = 0;
  net::TcpLink host, client;
  if (!host.Listen(0)) {
    why = "listen(127.0.0.1:0) failed: " + host.Error();
    return false;
  }
  const uint16_t port = host.ListenPort();
  if (port == 0) {
    why = "getsockname returned port 0";
    return false;
  }
  if (!client.Connect("127.0.0.1", port)) {
    why = "connect failed: " + client.Error();
    return false;
  }
  // Bounded spin, not a sleep loop: on loopback the handshake completes in the
  // first few polls, and a timeout here is the firewall case the kill
  // criterion is about rather than something to wait out.
  int spins = 0;
  while ((!host.Connected() || !client.Connected()) && spins++ < 200000) {
    host.Poll();
    client.Poll();
    if (!host.Error().empty() || !client.Error().empty()) break;
  }
  if (!host.Connected() || !client.Connected()) {
    why = "handshake did not complete (host '" + host.Error() + "', client '" +
          client.Error() + "')";
    return false;
  }
  available = true;
  // TCP_NODELAY read BACK, on both ends. setsockopt's return value is not the
  // claim — the claim is that the option is live on the socket that carries
  // the per-tick batches.
  noDelay = host.NoDelay() && client.NoDelay();

  constexpr uint32_t kCount = 200;
  Lcg rng{0xC0FFEE11u};
  std::vector<size_t> sizes(kCount);
  for (uint32_t i = 0; i < kCount; i++) sizes[i] = rng.Below(400);
  // ONE 300 KiB MESSAGE. The memory pipe never short-writes, so this is the
  // only thing in the gate that reaches TcpLink's partial-WRITE path — the
  // one whose failure mode is a silently truncated frame and a peer that
  // waits forever.
  sizes[kCount / 2] = 300 * 1024;

  std::vector<uint8_t> pay, want;
  uint32_t nextSend = 0, nextRecv = 0;
  int guard = 0;
  int bad = -1;
  while (nextRecv < kCount && guard++ < 4000000) {
    if (nextSend < kCount && nextSend < nextRecv + 8) {
      MakePayload(nextSend, sizes[nextSend], pay);
      if (!client.Send((uint16_t)(nextSend % 5 + 1), (uint16_t)(nextSend % 2),
                       pay.data(), pay.size())) {
        why = "tcp Send refused a legal message";
        return false;
      }
      nextSend++;
    }
    client.Poll();
    host.Poll();
    if (!client.Error().empty() || !host.Error().empty()) {
      why = "tcp link failed mid-stream (client '" + client.Error() +
            "', host '" + host.Error() + "')";
      return false;
    }
    net::Msg m;
    while (host.Recv(m)) {
      MakePayload(nextRecv, sizes[nextRecv], want);
      const bool ok = m.type == (uint16_t)(nextRecv % 5 + 1) &&
                      m.ver == (uint16_t)(nextRecv % 2) &&
                      m.payload.size() == want.size() &&
                      (want.empty() || std::memcmp(m.payload.data(), want.data(),
                                                   want.size()) == 0);
      if (!ok && bad < 0) bad = (int)nextRecv;
      nextRecv++;
    }
  }
  got = (int)nextRecv;
  if (nextRecv != kCount) {
    why = "tcp did not deliver every message";
    return false;
  }
  if (bad >= 0) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "tcp payload mismatch at %d", bad);
    why = buf;
    return false;
  }
  if (!noDelay) {
    why = "TCP_NODELAY not set on the live sockets";
    return false;
  }
  return true;
}

// ---- (c) the handshake matrix ------------------------------------------

net::Hello SampleHello(uint32_t playerId) {
  // Plausible but arbitrary values: FirstMismatch compares, it does not
  // validate, so what matters is only that no two fields start out equal by
  // accident (a perturbation that collided with a neighbour's value would
  // make the wrong field the first mismatch and the test would be lying).
  return net::LocalHello(/*seed*/ 1337, /*matCount*/ 211, /*matHash*/ 0xAB12CD34u,
                         /*tuning*/ 0x11111111u, /*materials*/ 0x22222222u,
                         /*reactions*/ 0x33333333u, /*envMap*/ 0x44444444u,
                         /*envBiomes*/ 0x55555555u, /*envTrees*/ 0x66666666u,
                         "harness", playerId);
}

bool SubtestHello(int& fieldsChecked, std::string& why) {
  const net::Hello mine = SampleHello(1);
  fieldsChecked = 0;

  // Two peers, same build, different player ids: no mismatch.
  if (net::Hello::FirstMismatch(mine, SampleHello(2)) != nullptr) {
    why = "identical builds refused each other";
    return false;
  }
  // playerId alone must NOT be a mismatch — it is the one field that is
  // supposed to differ, and comparing it would refuse every real connection.
  {
    net::Hello t = mine;
    t.playerId = 999;
    if (net::Hello::FirstMismatch(mine, t) != nullptr) {
      why = "playerId was compared";
      return false;
    }
  }

  const auto& fields = net::HelloU32Fields();
  for (size_t i = 0; i < fields.size(); i++) {
    net::Hello t = mine;
    t.*fields[i].member ^= 0xA5A5A5A5u;
    const char* got = net::Hello::FirstMismatch(mine, t);
    if (!got || std::strcmp(got, fields[i].name) != 0) {
      why = std::string("perturbing ") + fields[i].name + " named '" +
            (got ? got : "<null>") + "'";
      return false;
    }
    fieldsChecked++;
  }
  // mapName, the one non-u32 compared field.
  {
    net::Hello t = mine;
    t.mapName = "somewhere else";
    const char* got = net::Hello::FirstMismatch(mine, t);
    if (!got || std::strcmp(got, "mapName") != 0) {
      why = std::string("perturbing mapName named '") + (got ? got : "<null>") + "'";
      return false;
    }
    fieldsChecked++;
  }
  // THE ORDER IS THE CONTRACT. Perturb the last u32 AND the first, and the
  // FIRST must be named: that is what makes "a version skew reads as
  // protocolVersion" true rather than incidental.
  if (fields.size() >= 2) {
    net::Hello t = mine;
    t.*fields.front().member ^= 1u;
    t.*fields.back().member ^= 1u;
    const char* got = net::Hello::FirstMismatch(mine, t);
    if (!got || std::strcmp(got, fields.front().name) != 0) {
      why = "FirstMismatch did not report the EARLIER field";
      return false;
    }
  }

  // Encode/Decode round trip, and a truncated payload must be refused rather
  // than read past the end (ByteReader's sticky `ok` is what does it).
  std::vector<uint8_t> enc;
  mine.Encode(enc);
  net::Hello dec;
  if (!dec.Decode(enc.data(), enc.size())) {
    why = "Hello round trip failed to decode";
    return false;
  }
  if (net::Hello::FirstMismatch(mine, dec) != nullptr ||
      dec.playerId != mine.playerId || dec.mapName != mine.mapName) {
    why = "Hello round trip changed a field";
    return false;
  }
  net::Hello trunc;
  if (trunc.Decode(enc.data(), enc.size() - 3)) {
    why = "truncated Hello decoded as ok";
    return false;
  }

  // The other two handshake messages, same round trip. Cheap, and HelloAck
  // carries the host's clock — a field that silently decoded as zero would
  // start every client at tick 0.
  std::vector<uint8_t> b;
  net::HelloAck ack{12345, 2}, ack2;
  ack.Encode(b);
  if (!ack2.Decode(b.data(), b.size()) || ack2.startTick != 12345 ||
      ack2.yourPlayerId != 2) {
    why = "HelloAck round trip failed";
    return false;
  }
  b.clear();
  net::HelloRefuse ref{"matHash"}, ref2;
  ref.Encode(b);
  if (!ref2.Decode(b.data(), b.size()) || ref2.field != "matHash") {
    why = "HelloRefuse round trip failed";
    return false;
  }
  // TickBatchWire, with both blobs non-empty and one of them empty — the
  // empty-ops case is the COMMON one (a batch goes out every tick whether or
  // not there are ops), so it is the one that must not be mis-decoded.
  b.clear();
  net::TickBatchWire tb;
  tb.h.tick = 777;
  tb.h.playerId = 2;
  tb.h.windowOrigin[0] = -512;
  tb.h.windowOrigin[1] = 96;
  tb.h.windowOrigin[2] = 1024;
  tb.h.timeScale = 0.25f;
  tb.h.vizActive = 1;
  tb.playerState = {1, 2, 3, 4, 5};
  tb.Encode(b);
  net::TickBatchWire tb2;
  if (!tb2.Decode(b.data(), b.size()) ||
      std::memcmp(&tb.h, &tb2.h, sizeof tb.h) != 0 ||
      tb2.playerState != tb.playerState || !tb2.ops.empty()) {
    why = "TickBatchWire round trip failed";
    return false;
  }
  return true;
}

// ---- (d) the pacer ------------------------------------------------------

// The scripted run: a peer that supplies one batch per iteration, with a
// silence long enough to drain the buffer AND stall the local side.
constexpr int kPacerIters = 200;
constexpr int kPacerStallFrom = 60;
constexpr int kWantStalls = 10;
// D of the peer's missing ticks are absorbed by the delay buffer before the
// local side stalls at all — that is exactly what D buys, so the silence must
// be D + kWantStalls long for the stall count to come out at kWantStalls.
constexpr int kPeerSilence = (int)net::kOpDelayTicks + kWantStalls;

bool SubtestPacer(int& stalls, int& ran, int& sent, int& maxLag,
                  std::string& why) {
  net::LockstepPacer p;
  const uint32_t kStart = 1000;
  p.Reset(kStart);
  uint32_t peerNext = kStart;
  maxLag = -1;
  bool ranWithoutBatch = false;
  uint32_t firstRan = 0;
  bool anyRan = false;
  // THE INDEPENDENT ORACLE. Asserting `localTick <= p.peerUpTo` inside
  // `if (p.CanRun())` would be circular — that IS CanRun's definition, so it
  // could only fail if the compiler miscompiled it. This is the test's OWN
  // record of which batch ticks the scripted peer actually handed over, kept
  // without asking the pacer anything.
  bool sawAnyDelivery = false;
  uint32_t deliveredUpTo = 0;

  for (int it = 0; it < kPacerIters; it++) {
    // INVARIANT 1+2+3, in one loop and with no special case for connect: at
    // it == 0 this emits T0..T0+D (the pre-send), and exactly one batch per
    // iteration after.
    while (p.ShouldSend()) p.NoteSent(p.NextToSend());

    const bool silent = it >= kPacerStallFrom && it < kPacerStallFrom + kPeerSilence;
    if (!silent) {
      // The peer runs the same rule, so at it == 0 it has D+1 batches ready.
      const int supply = (it == 0) ? (int)net::kOpDelayTicks + 1 : 1;
      for (int k = 0; k < supply; k++) {
        deliveredUpTo = peerNext;
        sawAnyDelivery = true;
        if (!p.NotePeerBatch(peerNext++)) {
          why = "NotePeerBatch rejected a contiguous batch";
          return false;
        }
      }
    }

    if (p.CanRun()) {
      // THE CLAIM, checked at the moment of running and against the test's own
      // delivery log (see above): this tick's batch really did arrive.
      if (!sawAnyDelivery || p.localTick > deliveredUpTo) ranWithoutBatch = true;
      if (!anyRan) { firstRan = p.localTick; anyRan = true; }
      if (p.Lag() > maxLag) maxLag = p.Lag();
      p.NoteRan();
    } else {
      p.NoteStall();
    }
  }

  stalls = (int)p.stalls;
  ran = (int)p.ticksRun;
  sent = (int)p.batchesSent;

  if (ranWithoutBatch) {
    why = "a tick ran without its peer batch";
    return false;
  }
  if (!anyRan || firstRan != kStart) {
    why = "the pacer did not start at HelloAck::startTick";
    return false;
  }
  if (stalls != kWantStalls) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "stalls %d, want %d", stalls, kWantStalls);
    why = buf;
    return false;
  }
  // A BATCH EVERY TICK, D AHEAD. After running K ticks the highest batch sent
  // is T0+K-1+D, so exactly K+D batches went out. A pacer that skipped a send
  // while stalled, or double-sent on resume, fails here and nowhere else.
  if (sent != ran + (int)net::kOpDelayTicks) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "sent %d, want ran+D = %d", sent,
                  ran + (int)net::kOpDelayTicks);
    why = buf;
    return false;
  }
  // The steady-state lag IS D. Less would mean the delay buffer is not being
  // filled; more would mean the local side is falling behind for free.
  if (maxLag != (int)net::kOpDelayTicks) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "max lag %d, want D = %d", maxLag,
                  (int)net::kOpDelayTicks);
    why = buf;
    return false;
  }

  // OUT OF ORDER IS A PROTOCOL ERROR, on its own pacer so the counters above
  // stay clean. TCP cannot reorder, so a gap means a batch was dropped or
  // duplicated and running past it would simulate a tick with no inputs.
  net::LockstepPacer q;
  q.Reset(0);
  q.NotePeerBatch(0);
  if (q.NotePeerBatch(2) || q.outOfOrder != 1 || q.peerUpTo != 0) {
    why = "a non-contiguous peer batch was accepted";
    return false;
  }
  if (q.CanRun(1)) {
    why = "CanRun accepted a tick past the contiguous frontier";
    return false;
  }
  return true;
}

// ---- the gate -----------------------------------------------------------

Status GateNetLoopback(Ctx& c, std::string& detail) {
  (void)c;
  char buf[900];
  std::string why;

  int lbSent = 0, lbGot = 0, lbBad = -1;
  if (!SubtestLoopback(lbSent, lbGot, lbBad, why)) {
    std::snprintf(buf, sizeof buf,
                  "(a) loopback FAILED: %s (sent %d, got %d, first bad %d)",
                  why.c_str(), lbSent, lbGot, lbBad);
    detail = buf;
    return Status::Fail;
  }

  int tcpGot = 0;
  bool tcpUp = false, noDelay = false;
  std::string tcpWhy;
  const bool tcpOk = SubtestTcp(tcpGot, tcpUp, noDelay, tcpWhy);
  // THE KILL CRITERION (plan §3.A): a loopback listen/connect this machine
  // will not grant is ADVISORY, not a failure — (a) covers the framing and the
  // two-process smoke covers the socket. A failure AFTER the link came up is
  // a real failure and is treated as one.
  const bool tcpAdvisory = !tcpOk && !tcpUp;
  if (!tcpOk && tcpUp) {
    std::snprintf(buf, sizeof buf, "(b) tcp FAILED after connect: %s (got %d)",
                  tcpWhy.c_str(), tcpGot);
    detail = buf;
    return Status::Fail;
  }

  int helloFields = 0;
  if (!SubtestHello(helloFields, why)) {
    std::snprintf(buf, sizeof buf, "(c) hello FAILED: %s", why.c_str());
    detail = buf;
    return Status::Fail;
  }

  int stalls = 0, ran = 0, sent = 0, maxLag = -1;
  if (!SubtestPacer(stalls, ran, sent, maxLag, why)) {
    std::snprintf(buf, sizeof buf, "(d) pacer FAILED: %s", why.c_str());
    detail = buf;
    return Status::Fail;
  }

  // THE PINS. Counts, not thresholds: they say what the gate covered, so a
  // silently shrunk subtest (a script that stopped generating large messages,
  // a Hello field that stopped being compared) is visible as a moved number
  // rather than as a still-green gate.
  RecordObserved("net.loopbackMsgs", (double)lbGot);
  RecordObserved("net.tcpMsgs", (double)tcpGot);
  RecordObserved("net.helloFields", (double)helloFields);
  const double pinLb = BaselineNumber("net.loopbackMsgs", (double)lbGot);
  const double pinTcp = BaselineNumber("net.tcpMsgs", (double)tcpGot);
  const double pinFields = BaselineNumber("net.helloFields", (double)helloFields);
  // The tcp pin is only asserted when the arm actually ran: pinning it through
  // an advisory skip would turn the kill criterion back into a failure.
  const bool pinOk = (double)lbGot == pinLb &&
                     (double)helloFields == pinFields &&
                     (tcpAdvisory || (double)tcpGot == pinTcp);
  if (!pinOk) MarkPinnedOnly();

  std::snprintf(
      buf, sizeof buf,
      "(a) loopback %d/%.0f msgs byte-exact through %zu-byte reads; "
      "(b) tcp %s %d/%.0f msgs incl 300 KiB, TCP_NODELAY %s; "
      "(c) hello %d/%.0f fields each named by FirstMismatch, order pinned; "
      "(d) pacer D=%u: ran %d, sent %d (= ran+D), stalls %d after a %d-tick "
      "peer silence, max lag %d",
      lbGot, pinLb, net::kLoopbackChunkBytes,
      tcpAdvisory ? "ADVISORY-UNAVAILABLE" : "up", tcpGot, pinTcp,
      noDelay ? "verified" : (tcpAdvisory ? "n/a" : "MISSING"), helloFields,
      pinFields, net::kOpDelayTicks, ran, sent, stalls, kPeerSilence, maxLag);
  detail = buf;
  if (tcpAdvisory) detail += " | tcp arm skipped: " + tcpWhy;
  return pinOk ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& NetGates() {
  static const std::vector<Gate> g = {
      {"net-loopback", "net", {}, false, GateNetLoopback},
  };
  return g;
}

}  // namespace selftest
