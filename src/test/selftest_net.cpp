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

#include "net/authority.h"
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

// ===========================================================================
// gate `authority` — WHO STEPS THIS? (docs/PLAN_multiplayer_m9.md M9.4-A)
// ===========================================================================
//
// CPU-ONLY, like `net-loopback` above and for the same reason: `net/authority`
// is pure arithmetic over two `PlayerState`s' worth of facts, so the gate
// needs no World, no GPU, no assets and no fixture, and `--gate authority`
// alone is the whole iteration loop.
//
// It is a TRUTH TABLE, not a simulation. Each row states one claim in words
// and then checks it, so a failure prints the sentence that stopped being true
// rather than "expected 2, got 1". The rows exist because each is a way the
// naive rule ("nearest player owns it") is wrong:
//
//   nearest-wins         the base case, so the table fails loudly if the
//                        metric breaks entirely.
//   resident-beats-near  §4 finding 6: a machine can be NEARER and not have
//                        the chunk. It must lose. Without this row the engine
//                        would hand a mob to a peer with no memory of the
//                        ground it stands on.
//   margin-is-why        that same peer IS in-window by `ChunkInWindow` and is
//                        excluded only by the margin — so the row above is
//                        testing the margin rule and not a typo in an origin.
//   tie-lower-id         ties are the COMMON case at a boundary (two players
//                        walking abreast), not an edge case.
//   nobody               a chunk between two distant players has no owner, and
//                        that is an answer rather than a failure.
//   disconnected         a peer that dropped is not a candidate and cannot
//                        keep what it held.
//   hysteresis-hold /    one chunk nearer is not enough; two is.
//   hysteresis-release
//   hysteresis-*-evicts  hysteresis protects an INCUMBENT, not a ghost: losing
//                        residency or the connection releases it at once.
//   entity-keyed         two entities in ONE chunk with different incumbents
//                        get different answers. A chunk-keyed memory would
//                        give them the same one — the flapping bug.
//   feet-floor           a mob at a negative coordinate must not be pulled
//                        into the chunk above it by C++ integer division.
//   comparable           §4 finding 1's two-chunk depth, in BOTH windows.
//   owns-*               the producer table: every producer, in a chunk this
//                        machine owns and in one it does not.
//
// WHY THE FIXTURES ARE HAND-BUILT NUMBERS. Deriving peer positions from a
// World would make the gate depend on the window a previous gate left behind
// (CLAUDE.md: "a `--gate X` subset is not a small `--selftest`"), and every
// claim here is about arithmetic, not about any particular world.

namespace {

// One peer, spelled out. `origin` is the low corner of its 32-chunk window.
net::PeerView Peer(uint32_t id, IVec3 chunk, IVec3 origin,
                   bool connected = true) {
  net::PeerView p;
  p.playerId = id;
  p.chunk = chunk;
  p.windowOrigin = origin;
  p.connected = connected;
  return p;
}

// A checker that carries its failure text, so every row is one line and the
// first failure names its own claim instead of a pair of integers.
struct Table {
  int rows = 0;
  int bad = 0;
  std::string why;
  void Eq(const char* claim, long long got, long long want) {
    rows++;
    if (got == want) return;
    bad++;
    if (why.empty()) {
      char b[256];
      std::snprintf(b, sizeof b, "%s: got %lld, expected %lld", claim, got,
                    want);
      why = b;
    }
  }
  void True(const char* claim, bool v) { Eq(claim, v ? 1 : 0, 1); }
  void False(const char* claim, bool v) { Eq(claim, v ? 1 : 0, 0); }
};

Status GateAuthority(Ctx& c, std::string& detail) {
  (void)c;  // pure CPU: no world, no GPU, no assets
  Table t;
  const long long kNone = (long long)net::kNoAuthority;
  const net::AuthorityMemory kEmpty;

  // ---- A. the base geometry ----------------------------------------------
  // P1 sits at chunk 16 with a window at 0; P2 sits at chunk 30 with a window
  // at 14. Both windows cover the band from chunk 18 to 28, so distance is the
  // only thing separating them there.
  {
    const std::vector<net::PeerView> peers = {
        Peer(1, {16, 16, 16}, {0, 0, 0}),
        Peer(2, {30, 16, 16}, {14, 1, 1}),
    };
    t.Eq("nearest-wins: chunk 18 is 2 from P1 and 12 from P2",
         net::ChunkAuthority({18, 16, 16}, peers, kEmpty), 1);
    t.Eq("nearest-wins: chunk 28 is 12 from P1 and 2 from P2",
         net::ChunkAuthority({28, 16, 16}, peers, kEmpty), 2);
  }

  // ---- B. §4 finding 6: resident beats near ------------------------------
  // P1 is FOURTEEN chunks from wc and P2 is SIXTEEN — but wc sits on the very
  // first plane of P1's window, the plane whose CA sees SOLID on one side, so
  // P1 is not a candidate at all and the farther machine owns it.
  {
    const IVec3 wc{46, 16, 16};
    const std::vector<net::PeerView> peers = {
        Peer(1, {60, 16, 16}, {46, 1, 1}),  // nearer (14), on its edge plane
        Peer(2, {30, 16, 16}, {16, 1, 1}),  // farther (16), well inside
    };
    t.Eq("resident-beats-near: the nearer machine lacks the chunk with margin",
         net::ChunkAuthority(wc, peers, kEmpty), 2);
    t.True("margin-is-why: the nearer machine IS in-window at margin 0",
           net::ResidentWithMargin(wc, {46, 1, 1}, 0));
    t.False("margin-is-why: ...and is NOT at margin 1",
            net::ResidentWithMargin(wc, {46, 1, 1}, net::kAuthorityMargin));
    t.True("nearest-metric: the loser really was the nearer machine",
           net::ChunkChebyshev(wc, {60, 16, 16}) <
               net::ChunkChebyshev(wc, {30, 16, 16}));
  }

  // ---- C. ties, and nobody ------------------------------------------------
  {
    const std::vector<net::PeerView> peers = {
        Peer(5, {16, 16, 16}, {1, 1, 1}),
        Peer(2, {20, 16, 16}, {1, 1, 1}),
    };
    t.Eq("tie-lower-id: both are 2 chunks away; id 2 takes it",
         net::ChunkAuthority({18, 16, 16}, peers, kEmpty), 2);
    t.Eq("nobody: a chunk in neither window has no owner",
         net::ChunkAuthority({200, 16, 16}, peers, kEmpty), kNone);
  }

  // ---- D. a disconnected peer is not a candidate -------------------------
  {
    const IVec3 wc{18, 16, 16};
    std::vector<net::PeerView> peers = {
        Peer(1, {16, 16, 16}, {1, 1, 1}),  // 2 away
        Peer(2, {28, 16, 16}, {1, 1, 1}),  // 10 away
    };
    t.Eq("connected: the near machine owns it while it is up",
         net::ChunkAuthority(wc, peers, kEmpty), 1);
    peers[0].connected = false;
    t.Eq("disconnected: a dropped peer is no candidate, however near",
         net::ChunkAuthority(wc, peers, kEmpty), 2);
  }

  // ---- E. hysteresis ------------------------------------------------------
  {
    const IVec3 wc{20, 16, 16};
    const uint64_t key = World::PackChunkKey(wc);
    net::AuthorityMemory mem;
    net::Remember(mem, key, 1);

    // One chunk nearer: not enough.
    const std::vector<net::PeerView> hold = {
        Peer(1, {24, 16, 16}, {1, 1, 1}),  // 4 away, the incumbent
        Peer(2, {17, 16, 16}, {1, 1, 1}),  // 3 away, the challenger
    };
    t.Eq("hysteresis-hold: with no memory the nearer machine takes it",
         net::ChunkAuthority(wc, hold, kEmpty), 2);
    t.Eq("hysteresis-hold: the incumbent keeps it against a 1-chunk lead",
         net::ChunkAuthority(wc, hold, mem), 1);

    // Two chunks nearer: enough.
    const std::vector<net::PeerView> release = {
        Peer(1, {24, 16, 16}, {1, 1, 1}),  // 4 away
        Peer(2, {18, 16, 16}, {1, 1, 1}),  // 2 away
    };
    t.Eq("hysteresis-release: a 2-chunk lead takes it from the incumbent",
         net::ChunkAuthority(wc, release, mem), 2);

    // An incumbent that lost the chunk out of its window keeps nothing, even
    // though it is still by far the nearest machine.
    const std::vector<net::PeerView> evicted = {
        Peer(1, {24, 16, 16}, {20, 1, 1}),  // 4 away, wc on its edge plane
        Peer(2, {30, 16, 16}, {1, 1, 1}),   // 10 away, resident
    };
    t.Eq("hysteresis-residency-evicts: an incumbent without the chunk loses it",
         net::ChunkAuthority(wc, evicted, mem), 2);

    // Same for a disconnect.
    std::vector<net::PeerView> gone = hold;
    gone[0].connected = false;
    t.Eq("hysteresis-disconnect-evicts: a dropped incumbent loses it",
         net::ChunkAuthority(wc, gone, mem), 2);

    // And Remember() must ERASE on kNoAuthority, or a stale claim would
    // outlive the disconnect and win unchallenged the moment somebody walks
    // back into range.
    net::Remember(mem, key, net::kNoAuthority);
    t.Eq("remember-erases: no-authority clears the incumbent",
         (long long)mem.count(key), 0);
  }

  // ---- F. entity memory is keyed on the ENTITY ---------------------------
  // Two mobs standing in ONE chunk with different incumbents. A chunk-keyed
  // memory would hand both to the same machine; the per-entity slot is what
  // stops a mob on a chunk edge from changing hands every few ticks.
  {
    const IVec3 wc{20, 16, 16};
    const std::vector<net::PeerView> peers = {
        Peer(1, {24, 16, 16}, {1, 1, 1}),  // 4 away
        Peer(2, {17, 16, 16}, {1, 1, 1}),  // 3 away — leads by only 1
    };
    net::AuthorityMemory mem;
    net::Remember(mem, 7ull, 1);
    net::Remember(mem, 8ull, 2);
    t.Eq("entity-keyed: entity 7's incumbent (P1) holds it",
         net::EntityAuthorityChunk(7ull, wc, peers, mem), 1);
    t.Eq("entity-keyed: entity 8's incumbent (P2) holds it",
         net::EntityAuthorityChunk(8ull, wc, peers, mem), 2);
    t.Eq("entity-keyed: the CHUNK key is untouched by either entity slot",
         net::ChunkAuthority(wc, peers, mem), 2);

    // The Vec3 entry point agrees with the chunk one, and floors.
    const Vec3 feet{20.0f * 16.0f + 8.0f, 16.0f * 16.0f + 1.0f,
                    16.0f * 16.0f + 8.0f};
    t.Eq("feet: the Vec3 entry point lands in the same chunk",
         net::EntityAuthority(7ull, feet, peers, mem), 1);
    const IVec3 below = net::ChunkOfFeet({-0.5f, -0.5f, -0.5f});
    t.Eq("feet-floor: a voxel below the origin is in chunk -1, not chunk 0",
         (long long)(below.x * 100 + below.y * 10 + below.z), -111ll);
  }

  // ---- G. Comparable (§4 finding 1) --------------------------------------
  {
    t.True("comparable: 2 chunks inside both windows",
           net::Comparable({10, 10, 10}, {8, 8, 8}, {6, 6, 6}));
    t.False("comparable: 1 chunk inside the second window is not enough",
            net::Comparable({10, 10, 10}, {8, 8, 8}, {9, 9, 9}));
    t.False("comparable: ...and it is symmetric",
            net::Comparable({10, 10, 10}, {9, 9, 9}, {8, 8, 8}));
    t.False("comparable: not in the second window at all",
            net::Comparable({10, 10, 10}, {8, 8, 8}, {100, 100, 100}));
  }

  // ---- H. Owns: every producer, an owned chunk and a foreign one ---------
  {
    const std::vector<net::PeerView> peers = {
        Peer(1, {16, 16, 16}, {0, 0, 0}),
        Peer(2, {30, 16, 16}, {14, 1, 1}),
    };
    const IVec3 mine{18, 16, 16};    // P1 owns it (2 vs 12)
    const IVec3 theirs{28, 16, 16};  // P2 owns it (12 vs 2)
    const uint32_t me = 1;
    using P = sandvox::opstream::Producer;
    struct Row {
      P p;
      bool mineOk;
      bool theirsOk;
      const char* name;
    };
    const Row rows[] = {
        {P::Unknown, false, false, "owns-unknown"},
        {P::Brush, true, true, "owns-brush"},
        {P::Spell, true, true, "owns-spell"},
        {P::Mob, true, false, "owns-mob"},
        {P::Avatar, true, true, "owns-avatar"},
        {P::Debris, true, false, "owns-debris"},
        {P::EditLayer, true, true, "owns-editlayer"},
        {P::Worldgen, true, false, "owns-worldgen"},
        {P::Lab, true, true, "owns-lab"},
        {P::Remote, false, false, "owns-remote"},
    };
    // The table must cover EVERY producer. A producer added later with no row
    // here would be silently unchecked — the "fixture that cannot fail"
    // failure — so the coverage is counted rather than assumed.
    t.Eq("owns-table-covers-every-producer",
         (long long)(sizeof rows / sizeof rows[0]), (long long)P::Count);
    for (const Row& r : rows) {
      char b[96];
      std::snprintf(b, sizeof b, "%s in MY chunk", r.name);
      t.Eq(b, net::Owns((uint8_t)r.p, mine, me, peers, kEmpty) ? 1 : 0,
           r.mineOk ? 1 : 0);
      std::snprintf(b, sizeof b, "%s in the PEER's chunk", r.name);
      t.Eq(b, net::Owns((uint8_t)r.p, theirs, me, peers, kEmpty) ? 1 : 0,
           r.theirsOk ? 1 : 0);
      // The classifier the SubmitTick audit walks with must agree with the
      // rule Owns implements — otherwise the audit checks a different set of
      // ops than the set the rule governs, and passes for the wrong reason.
      // "Chunk-gated" IS "the answer depends on which chunk", so the expected
      // value is derived from the two columns rather than restated.
      const bool chunkGated = r.mineOk != r.theirsOk;
      std::snprintf(b, sizeof b, "%s: classifier agrees with the rule", r.name);
      t.Eq(b, net::ProducerNeedsChunkAuthority((uint8_t)r.p) ? 1 : 0,
           chunkGated ? 1 : 0);
    }
  }

  if (t.bad) {
    char b[384];
    std::snprintf(b, sizeof b, "%d/%d rows FAILED; first: %s", t.bad, t.rows,
                  t.why.c_str());
    detail = b;
    return Status::Fail;
  }

  // THE PIN IS THE ROW COUNT. It cannot say the answers are right — the rows
  // above do that — but it catches the one thing they cannot: a row deleted or
  // a loop that stopped running, which leaves a green gate measuring less than
  // it did yesterday.
  RecordObserved("net.authorityRows", (double)t.rows);
  const double pin = BaselineNumber("net.authorityRows", (double)t.rows);
  const bool pinOk = (double)t.rows == pin;
  if (!pinOk) MarkPinnedOnly();

  char b[448];
  std::snprintf(
      b, sizeof b,
      "%d/%.0f truth-table rows: nearest-wins; resident-beats-near (finding 6) "
      "at margin %d; tie -> lower id; nobody; disconnect; hysteresis hold/"
      "release at %d chunks plus residency and disconnect evictions; "
      "per-entity memory; feet floor; Comparable at margin %d; and all %d "
      "producers of Owns in an owned and a foreign chunk",
      t.rows, pin, net::kAuthorityMargin, net::kAuthorityHysteresis,
      net::kComparableMargin, (int)sandvox::opstream::Producer::Count);
  detail = b;
  return pinOk ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& NetGates() {
  static const std::vector<Gate> g = {
      {"net-loopback", "net", {}, false, GateNetLoopback},
      // M9.4-A. No deps and no Ctx use: it reads net/authority.h and nothing
      // else, so it cannot be affected by what an earlier gate left behind.
      {"authority", "net", {}, false, GateAuthority},
  };
  return g;
}

}  // namespace selftest
