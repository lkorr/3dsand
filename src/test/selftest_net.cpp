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

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "game/avatar.h"
#include "game/session.h"
#include "net/authority.h"
#include "net/link.h"
#include "net/opsync.h"
#include "net/protocol.h"
#include "net/storesync.h"
#include "sim/oprecord.h"
#include "sim/worldio.h"
#include "test/selftest.h"
// ops-exchange submits real ticks, so it needs the harness's sim plumbing
// (SubmitWorldgen / SubmitTick / ReadHashSync / FixtureY). The two CPU-only
// gates above still touch none of it.
#include "test/support.h"

// Same as every other gate TU that submits ticks (selftest_sim.cpp line 28):
// the harness plumbing lives in namespace sandvox and is called unqualified.
using namespace sandvox;

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

// ---- ops-exchange: two machines submit the SAME ops (M9.3-B) -------------
//
// docs/PLAN_multiplayer_m9.md M9.3-B. The claim this gate exists to make is
// one sentence: TWO MACHINES THAT AUTHOR DIFFERENT EDITS SUBMIT THE SAME OP
// VECTORS UNDER THE SAME TICK LABEL. Everything below is that claim split
// into pieces that can each fail on their own.
//
//  (A) THE MERGE IS CANONICAL. The same script is played twice against the
//      same world: once from the HOST's seat (local player 0, the client's
//      ops arriving over a loopback link) and once from the CLIENT's seat
//      (local player 1, the host's ops arriving). The merged batch of every
//      tick is serialized and compared BYTE FOR BYTE between the two arms,
//      and the world hashes must match at every probe. The two arms differ in
//      which batch is "mine", so a merge with any notion of "mine first"
//      fails here — which is the single thing that makes the exchange sound.
//  (B) THE ORDER IS AUTHOR-MAJOR. Player 0's ops lead on both machines. Read
//      off the merged indices of the REMOTE brush ops: from the host's seat
//      they sit AFTER the local ones, from the client's seat they start at 0.
//      (A) would catch a swap too, but not say which way round it went.
//  (C) DELAY. Ops authored at T are submitted at T + D by both sides. The
//      first D ticks therefore submit NOTHING, and the queue says so: D
//      merges with no local batch and D with no remote one.
//  (D) RESIDENCY. A remote CellOp carries a SLOT index, which is global, so
//      the chunk it means depends on the SENDER's window origin. Fed a peer
//      whose window is four chunks along, the cells that land in a chunk this
//      machine also holds are kept and the ones that do not are dropped — and
//      the first drop names its chunk, because "8 cells dropped" on its own
//      is the bare count CLAUDE.md rule 6 is about.
//  (E) THE COPIED ARITHMETIC AGREES. net::SlotChunkUnderOrigin duplicates
//      World::SlotToWorldChunk (the World member reads the LOCAL origin and
//      there is no World holding the peer's). A copy that drifts paints the
//      wrong chunk silently, so the two are compared over a spread of slots.
//  (F) THE WIRE BLOB ROUND-TRIPS, all five vectors, and REFUSES a truncated
//      or wrong-version one whole rather than half-decoding it.
//  (G) THE RECORD REPLAYS. The host arm is recorded and replayed through the
//      same drive loop `ops-replay` uses; same hash series. That is what says
//      the merged stream is still a complete description of the tick.
//
// IT NEEDS THE GPU (it submits) and it re-worldgens between arms, exactly as
// `ops-replay` does, so `--gate ops-exchange` alone is a complete run.
namespace opsexch {

constexpr int kTicks = 200;
constexpr uint32_t kProbeEvery = 25;

// THE SCRIPT, A PURE FUNCTION OF THE TICK. Both arms play the same one, so
// the only difference between them is which seat the merge is done from —
// which is the whole point of (A). Two authors, both of them editing.
struct Script {
  uint32_t seed = 0;
  int ox = 0, oz = 0;       // window origin in world CELLS
  IVec3 origin{0, 0, 0};    // window origin in world CHUNKS
  int hx = 0, hy = 0, hz = 0;  // the host's stroke
  IVec3 cellBase{0, 0, 0};     // the client's cell box, in a resident chunk

  // PLAYER 0. A stone stroke every 10 ticks and one blast at tick 100.
  OpBatch Host(uint32_t t) const {
    OpBatch b;
    if (t % 10 == 0)
      b.ops.push_back({hx, hy, hz, 5, kMatStone, 1u, 0, 0});
    if (t == 100)
      b.exps.push_back({hx, hy, hz, 10, 300, 0, 0, 0});
    return b;
  }

  // PLAYER 1. A 4^3 box of exact cells every 7 ticks, plus its own brush op
  // every 13 — the second brush author is what makes (B) readable: with only
  // one brush author the merged vector would be the same whatever the order.
  OpBatch Client(uint32_t t) const {
    OpBatch b;
    if (t % 7 == 0) {
      for (int dz = 0; dz < 4; dz++)
        for (int dy = 0; dy < 4; dy++)
          for (int dx = 0; dx < 4; dx++)
            b.cells.push_back(
                {World::SlotCellIndex({cellBase.x + dx, cellBase.y + dy,
                                       cellBase.z + dz}),
                 PackVoxNew(kMatWood, 0)});
    }
    if (t % 13 == 0)
      b.ops.push_back({hx + 40, hy, hz + 40, 4, kMatSand, 1u, 0, 0});
    return b;
  }
};

// ONE MESSAGE ACROSS A REAL LOOPBACK LINK, not a direct call into the queue.
// The loopback pair hands bytes over 1,500 at a time through the same
// reassembler TcpLink uses (net/link.h), so the 2 KiB cell batches genuinely
// cross several Poll()s and this arm covers the framing as well as the merge.
// False if the frame never arrives or the blob is refused.
bool ExchangeOne(net::Link& tx, net::Link& rx, const OpBatch& b, IVec3 origin,
                 uint32_t label, uint32_t author, net::OpDelayQueue& q) {
  std::vector<uint8_t> buf;
  net::OpsWire::Encode(b, origin, label, buf);
  if (!tx.Send((uint16_t)net::MsgType::TickBatch, net::kProtocolVersion,
               buf.data(), buf.size()))
    return false;
  net::Msg m;
  for (int i = 0; i < 8192; i++) {
    tx.Poll();
    rx.Poll();
    if (rx.Recv(m)) break;
    if (i == 8191) return false;
  }
  OpBatch rb;
  IVec3 rorigin{0, 0, 0};
  uint32_t rlabel = 0;
  if (!net::OpsWire::Decode(m.payload.data(), m.payload.size(), rb, rorigin,
                            rlabel) ||
      rlabel != label)
    return false;
  q.NoteRemote(rlabel, author, rorigin, std::move(rb));
  return true;
}

// The merged batch of one tick, as bytes, for the arm-to-arm memcmp. Reusing
// OpsWire is deliberate: it is one encoder, it covers every field of every op
// struct, and a comparison that walked the vectors by hand would be a second
// definition of "the same batch".
void BlobOf(const OpBatch& b, std::vector<uint8_t>& out) {
  net::OpsWire::Encode(b, IVec3{0, 0, 0}, 0, out);
}

// THE TICK ON WHICH BOTH AUTHORS HAVE A BRUSH OP, which is the only tick that
// can tell the two orderings apart. The script gives player 0 a stroke every
// 10 script-ticks and player 1 one every 13, so they coincide at script tick
// 130 — and a merge at tick T carries what was authored at T - D, so the
// merged tick to look at is 130 + D.
constexpr uint32_t kBothAuthorsTick = 130 + net::kOpLabelAhead;

struct ArmResult {
  std::vector<uint32_t> hashes;
  std::vector<std::vector<uint8_t>> blobs;   // one per tick
  // At kBothAuthorsTick: how many brush ops the merge produced, and the
  // merged index of the first REMOTE one. Together they say who led.
  int bothOps = -1;
  int bothRemoteIdx = -1;
  uint64_t missingLocal = 0, missingRemote = 0, mergedMax = 0;
  bool wireOk = true;
};

// Play the script from ONE seat. `localId` is which player this machine is.
void RunArm(Ctx& c, const Script& sc, uint32_t localId, ArmResult& r) {
  SubmitWorldgen(c.ctx, c.world, c.sim, sc.seed);
  c.ctx.WaitIdle();

  net::LoopbackPair lb = net::MakeLoopback();
  net::OpDelayQueue q;
  q.SetLocalId(localId);
  const uint32_t D = q.D();

  for (uint32_t t = 1; t <= (uint32_t)kTicks; t++) {
    const OpBatch host = sc.Host(t), client = sc.Client(t);
    const uint32_t label = t + D;
    // The peer's batch travels; ours is pushed straight in, which is exactly
    // what session.cpp phase N does on a real machine.
    if (localId == 0) {
      q.PushLocal(t, host, sc.origin);
      if (!ExchangeOne(*lb.b, *lb.a, client, sc.origin, label, 1, q))
        r.wireOk = false;
    } else {
      q.PushLocal(t, client, sc.origin);
      if (!ExchangeOne(*lb.b, *lb.a, host, sc.origin, label, 0, q))
        r.wireOk = false;
    }

    net::MergeStats ms;
    std::vector<uint32_t> remoteBrush;
    OpBatch merged = q.Merge(t, c.world, ms, &remoteBrush);
    if (t == kBothAuthorsTick) {
      r.bothOps = (int)merged.ops.size();
      r.bothRemoteIdx = remoteBrush.empty() ? -1 : (int)remoteBrush[0];
    }
    r.blobs.emplace_back();
    BlobOf(merged, r.blobs.back());

    SubmitTick(c.ctx, c.world, c.sim, t, sc.seed, merged.ops, merged.exps,
               merged.cells, true, {8, 3, 8}, false, t >= 100, merged.spawns,
               0, merged.fluid);
    if (t % kProbeEvery == 0 || t == (uint32_t)kTicks)
      r.hashes.push_back(ReadHashSync(c.ctx, c.world));
  }
  r.missingLocal = q.missingLocal;
  r.missingRemote = q.missingRemote;
  r.mergedMax = q.mergedMax;
}

}  // namespace opsexch

Status GateOpsExchange(Ctx& c, std::string& detail) {
  using namespace opsexch;
  namespace ops = sandvox::opstream;
  Table t;

  const uint32_t seed = kDefaultSeed;
  const IVec3 wo = c.world.WindowOrigin();
  Script sc;
  sc.seed = seed;
  sc.origin = wo;
  sc.ox = wo.x * (int)kChunk;
  sc.oz = wo.z * (int)kChunk;
  sc.hx = sc.ox + 120;
  sc.hz = sc.oz + 120;
  sc.hy = FixtureY(sc.hx, sc.hz, seed, 30);
  // The client's cell box, in a chunk well inside the window so both peers
  // hold it (arm (D) below is where non-residency is tested, deliberately, so
  // that (A)'s two arms compare like with like).
  sc.cellBase = {sc.ox + 70, FixtureY(sc.ox + 70, sc.oz + 70, seed, 30),
                 sc.oz + 70};

  // ---- (F) the wire blob, before anything expensive ----------------------
  {
    OpBatch b;
    b.ops.push_back({1, 2, 3, 4, kMatStone, 1u, 0, 0});
    b.exps.push_back({5, 6, 7, 8, 99, 3, 0, 0});
    b.cells.push_back({1234u, PackVoxNew(kMatWood, 2)});
    b.spawns.push_back({1, 2, 3, 4, 5, 6, 7u, 1u});
    b.fluid.push_back({1, 2, 3, 4, 5, 6, 2u, kMatWater});
    std::vector<uint8_t> buf;
    net::OpsWire::Encode(b, IVec3{9, -8, 7}, 4242u, buf);
    OpBatch out;
    IVec3 origin{0, 0, 0};
    uint32_t label = 0;
    t.True("wire: a full batch decodes",
           net::OpsWire::Decode(buf.data(), buf.size(), out, origin, label));
    t.Eq("wire: label survives", (long long)label, 4242ll);
    t.Eq("wire: origin survives",
         (long long)(origin.x * 100 + origin.y * 10 + origin.z), 827ll);
    t.Eq("wire: the five vectors keep their sizes",
         (long long)(out.ops.size() + out.exps.size() * 10 +
                     out.cells.size() * 100 + out.spawns.size() * 1000 +
                     out.fluid.size() * 10000),
         11111ll);
    t.Eq("wire: a brush op survives field for field",
         std::memcmp(&out.ops[0], &b.ops[0], sizeof(BrushOp)), 0);
    t.Eq("wire: an explosion keeps its author",
         (long long)out.exps[0].author, 3ll);
    t.Eq("wire: a cell op survives",
         std::memcmp(&out.cells[0], &b.cells[0], sizeof(CellOp)), 0);
    // A TRUNCATED BLOB IS REFUSED WHOLE. Half a peer's tick is worse than
    // none of it: the pacer would run the tick believing everything arrived.
    OpBatch half;
    t.False("wire: a truncated blob is refused",
            net::OpsWire::Decode(buf.data(), buf.size() / 2, half, origin,
                                 label));
    t.Eq("wire: ...and leaves nothing behind", (long long)half.ops.size(), 0ll);
    std::vector<uint8_t> bad = buf;
    bad[0] ^= 0xFFu;   // the version word
    t.False("wire: a wrong version is refused",
            net::OpsWire::Decode(bad.data(), bad.size(), half, origin, label));
  }

  // ---- (E) the copied slot arithmetic ------------------------------------
  {
    int agree = 0;
    for (uint32_t s = 0; s < kNumChunks; s += 977u) {
      const IVec3 a = net::SlotChunkUnderOrigin(s, wo);
      const IVec3 b = c.world.SlotToWorldChunk(s);
      if (a.x == b.x && a.y == b.y && a.z == b.z) agree++;
    }
    t.Eq("slot arithmetic: the copy agrees with World over every probed slot",
         agree, (int)((kNumChunks + 976u) / 977u));
  }

  // ---- (D) residency: a peer whose window is four chunks along -----------
  //
  // Two CellOp boxes from the peer: one in a chunk both windows hold, one in
  // a chunk only the PEER holds (x = origin + 33, past this window's 32). The
  // slot index is the same arithmetic either way — what separates them is the
  // origin the peer computed them under, which is the whole reason that
  // origin is on the wire.
  {
    const IVec3 peerOrigin{wo.x + 4, wo.y, wo.z};
    const IVec3 shared{wo.x + 8, wo.y + 1, wo.z + 1};
    const IVec3 peerOnly{wo.x + 33, wo.y + 1, wo.z + 1};
    t.True("residency fixture: the shared chunk IS resident here",
           c.world.ChunkInWindow(shared));
    t.False("residency fixture: the peer-only chunk is NOT",
            c.world.ChunkInWindow(peerOnly));
    OpBatch peer;
    auto box = [&](IVec3 wc) {
      for (int k = 0; k < 8; k++)
        peer.cells.push_back({World::SlotChunkIndex(wc) * kChunkVol + (uint32_t)k,
                              PackVoxNew(kMatStone, 0)});
    };
    box(shared);
    box(peerOnly);
    net::OpDelayQueue q;
    q.SetLocalId(0);
    OpBatch mine;   // the local side authors nothing this tick
    q.PushLocal(0, mine, wo);
    q.NoteRemote(q.D(), 1, peerOrigin, peer);
    net::MergeStats ms;
    OpBatch merged = q.Merge(q.D(), c.world, ms);
    t.Eq("residency: the shared chunk's cells are kept",
         (long long)ms.remoteCells, 8ll);
    t.Eq("residency: the peer-only chunk's cells are dropped",
         (long long)ms.remoteCellsDropped, 8ll);
    t.Eq("residency: ...and the merged batch carries only the kept ones",
         (long long)merged.cells.size(), 8ll);
    t.True("residency: the drop names its chunk", ms.haveFirstDrop);
    t.Eq("residency: ...and it is the peer-only one",
         (long long)(ms.firstDropChunk[0] - wo.x), 33ll);
  }

  // ---- (A)(B)(C)(G) the two seats ----------------------------------------
  const std::string path = "build/ops_exchange.svops";
  std::string err;
  const bool rec = ops::StartRecording(path, seed, c.mats, err);
  if (!rec) std::printf("ops-exchange: not recording (%s)\n", err.c_str());

  ArmResult host;
  RunArm(c, sc, 0, host);
  const uint32_t frames = ops::RecordedFrames();
  if (rec) ops::StopRecording();

  ArmResult client;
  RunArm(c, sc, 1, client);

  t.True("wire: every batch crossed the loopback link (host seat)",
         host.wireOk);
  t.True("wire: every batch crossed the loopback link (client seat)",
         client.wireOk);

  // (A) the merged vectors, byte for byte, every tick.
  size_t firstBlobDiff = (size_t)-1;
  for (size_t i = 0; i < host.blobs.size() && i < client.blobs.size(); i++) {
    if (host.blobs[i].size() != client.blobs[i].size() ||
        std::memcmp(host.blobs[i].data(), client.blobs[i].data(),
                    host.blobs[i].size()) != 0) {
      firstBlobDiff = i;
      break;
    }
  }
  t.Eq("canonical: both seats merged the same number of ticks",
       (long long)host.blobs.size(), (long long)client.blobs.size());
  t.Eq("canonical: the merged batch is byte-identical on both seats "
       "(first differing tick, or -1)",
       (long long)(firstBlobDiff == (size_t)-1 ? -1 : (long long)firstBlobDiff + 1),
       -1ll);
  // ...and therefore the two worlds are the same world.
  size_t firstHashDiff = (size_t)-1;
  for (size_t i = 0; i < host.hashes.size() && i < client.hashes.size(); i++)
    if (host.hashes[i] != client.hashes[i]) { firstHashDiff = i; break; }
  t.Eq("canonical: the world hash agrees at every probe",
       (long long)(firstHashDiff == (size_t)-1 ? -1
                                               : (long long)firstHashDiff),
       -1ll);
  t.True("canonical: the arms actually did something (a non-empty merge)",
         host.mergedMax > 0);

  // (B) author-major order, on the one tick that can tell it apart: both
  // players authored a brush op, so the merge has two and the question is
  // which is at index 0. Player 0's, on BOTH seats — so the REMOTE op is at
  // index 1 from the host's seat and at index 0 from the client's. A merge
  // that put "mine" first would swap exactly these two numbers.
  t.Eq("order: the both-authors tick merged two brush ops (host seat)",
       (long long)host.bothOps, 2ll);
  t.Eq("order: the both-authors tick merged two brush ops (client seat)",
       (long long)client.bothOps, 2ll);
  t.Eq("order: from the HOST's seat the peer's op is second",
       (long long)host.bothRemoteIdx, 1ll);
  t.Eq("order: from the CLIENT's seat the peer's (player 0's) op is first",
       (long long)client.bothRemoteIdx, 0ll);

  // (C) the delay's warm-up: D merges with no local batch and D with no
  // remote one, because nothing was labelled for the first D ticks.
  t.Eq("delay: the first D+1 ticks merged no local batch",
       (long long)host.missingLocal, (long long)net::kOpLabelAhead);
  t.Eq("delay: ...and no remote one either",
       (long long)host.missingRemote, (long long)net::kOpLabelAhead);

  // (G) the record of the host seat replays to the same hash series.
  std::vector<uint32_t> replay;
  bool replayRan = false;
  if (rec) {
    ops::Log log;
    if (log.Load(path, c.mats, err)) {
      struct ReplayArm {
        explicit ReplayArm(const ops::Log* l) {
          ops::ResetReplayStats();
          ops::SetReplay(l);
        }
        ~ReplayArm() { ops::SetReplay(nullptr); }
      } arm(&log);
      SubmitWorldgen(c.ctx, c.world, c.sim, log.header.seed);
      c.ctx.WaitIdle();
      for (const ops::Frame& f : log.frames) {
        SubmitTick(c.ctx, c.world, c.sim, f.in.tick, f.in.seed, f.ops, f.exps,
                   f.cells, f.in.hashEnable != 0,
                   {f.in.playerChunk[0], f.in.playerChunk[1],
                    f.in.playerChunk[2]},
                   f.in.wantReadback != 0, f.in.particlesActive != 0, f.spawns,
                   f.in.farCount, f.fluid, f.in.fluidLive,
                   f.in.vizActive != 0);
        if (f.in.tick % kProbeEvery == 0 || f.in.tick == (uint32_t)kTicks)
          replay.push_back(ReadHashSync(c.ctx, c.world));
      }
      replayRan = true;
      t.Eq("replay: the record has one frame per tick",
           (long long)log.frames.size(), (long long)kTicks);
      size_t d = (size_t)-1;
      for (size_t i = 0; i < host.hashes.size() && i < replay.size(); i++)
        if (host.hashes[i] != replay[i]) { d = i; break; }
      t.Eq("replay: the merged stream reproduces the hash series",
           (long long)(d == (size_t)-1 ? -1 : (long long)d), -1ll);
    } else {
      t.True("replay: the record loads", false);
    }
  }

  if (t.bad) {
    char b[448];
    std::snprintf(b, sizeof b, "%d/%d rows FAILED; first: %s", t.bad, t.rows,
                  t.why.c_str());
    detail = b;
    std::printf("ops-exchange: FAIL (%s)\n", b);
    return Status::Fail;
  }

  RecordObserved("net.opsExchangeRows", (double)t.rows);
  RecordObserved("net.opsExchangeMergedMax", (double)host.mergedMax);
  const double pin = BaselineNumber("net.opsExchangeRows", (double)t.rows);
  const bool pinOk = (double)t.rows == pin;
  if (!pinOk) MarkPinnedOnly();

  char b[640];
  std::snprintf(b, sizeof b,
                "%d/%.0f rows: %d ticks merged from BOTH seats, byte-identical "
                "batches and equal hashes at %zu probes; largest merge %llu "
                "ops; delay warm-up %llu ticks; residency dropped 8 of 16 peer "
                "cells (chunk x+33); record %u frames %s",
                t.rows, pin, kTicks, host.hashes.size(),
                (unsigned long long)host.mergedMax,
                (unsigned long long)host.missingLocal, frames,
                replayRan ? "replayed to the same hashes" : "NOT replayed");
  detail = b;
  std::printf("ops-exchange: %s\n", b);
  return pinOk ? Status::Pass : Status::Fail;
}

// ---- store-sync (M9.5 package B) -----------------------------------------
//
// THE HOST STORE ON THE WIRE. Two `net::StoreSync` ends over `MakeLoopback()`
// and two `ChunkStore`s, driven directly: no `Stream`, no `World`, no GPU and
// no assets for the whole protocol half. That is deliberate and it is the
// cheap-to-verify rule from CLAUDE.md — `Stream`'s side of the exchange is
// already covered by `chunk-exchange`, so re-driving it here would pay a
// worldgen per arm to re-assert somebody else's claim.
//
// The `Stream` is faked by a RECORDING STUB: `StoreSync` reaches it through
// two std::functions (`SetDelivery`) precisely so this is possible, and the
// stub records what was delivered so the bytes can be compared rather than
// merely counted.
//
// SEVEN CLAIMS, each stated so it can fail:
//
//  (A) THE CLIENT'S EVICTION LANDS IN THE HOST'S STORE, with its tick tag,
//      and the put leaves the unacked list.
//  (B) NEWER WINS AND OLDER IS STILL ACKED. An older `ChunkPut` must not
//      overwrite, and must not stay unacked either — an unacked stale put is
//      re-offered on every reconnect forever.
//  (C) A NON-AUTHORITY SAYS NOTHING. The client evicting a chunk the host
//      still holds resident and is nearer to emits no message at all. This
//      is the arm that would catch the eviction-authority rule being written
//      as `ChunkAuthority(wc) == me`, which is false for EVERY eviction
//      (net/storesync.cpp says why at length).
//  (D) MANIFEST -> WANTED -> GET -> DATA -> DELIVER, end to end, with the
//      delivered BYTES compared against the host's copy. A chunk in no
//      manifest is not wanted, which is the other half of the claim: a
//      `Wanted` that always says yes would hold every slot in the world.
//  (E) A MISS IS A MESSAGE. The host pulling a chunk the client does not
//      have gets `ChunkMiss` and its miss hook fires — silence would leave
//      the slot inert forever.
//  (F) AN UNACKED PUT SURVIVES A DROP AND IS RE-OFFERED. Plan step 3.
//  (G) A DISCONNECT ANSWERS EVERY OUTSTANDING REQUEST with a local miss.
//
// ...and then two FILE claims, which do need the GPU because `SaveWorld` is
// the only writer of `meta.svm` and faking it would test a second
// implementation:
//
//  (H) meta.svm ROUND-TRIPS tick+seed under SVM5.
//  (I) AN SVM4 FILE STILL LOADS, and reports the pair as UNKNOWN rather than
//      as zero. The fixture makes a real SVM4 file by rewriting the magic of
//      a real SVM5 one and dropping its two appended words, which is exactly
//      what an old build's writer would have produced.
Status GateStoreSync(Ctx& c, std::string& detail) {
  Table t;

  // ---- the fixture's geography -----------------------------------------
  //
  // The host stands at chunk 16 with its window at the origin (covering
  // chunks 0..31 per axis); the client stands at chunk 60 with its window at
  // 44 (covering 44..75). The two windows do not overlap AT ALL, which is
  // the M9 case: kWorldN = 512 is a 51.2 m cube and two players walking
  // independently leave each other's window in seconds.
  const net::PeerView hostView = Peer(0, {16, 16, 16}, {0, 0, 0});
  const net::PeerView clientView = Peer(1, {60, 16, 16}, {44, 1, 1});
  // Deep inside the CLIENT's window and nowhere near the host's.
  const IVec3 wcClient{60, 16, 16};
  const IVec3 wcClient2{58, 16, 16};
  const IVec3 wcClient3{62, 20, 16};
  const IVec3 wcClient4{63, 12, 20};
  // Deep inside the HOST's window.
  const IVec3 wcHost{10, 16, 16};
  // In NEITHER window: nobody can be the authority for it.
  const IVec3 wcNowhere{200, 16, 16};

  // Chunk payloads. Legal RLE (pairs of word + run length summing to
  // kChunkVol), and DIFFERENT per chunk so a delivery that arrived with the
  // wrong bytes fails rather than passing on a length check.
  auto MakeRle = [](uint32_t word) {
    return std::vector<uint32_t>{word, (uint32_t)kChunkVol};
  };
  const std::vector<uint32_t> rleA = MakeRle(0x0000'1001u);
  const std::vector<uint32_t> rleB = MakeRle(0x0000'2002u);
  const std::vector<uint32_t> rleC = MakeRle(0x0000'3003u);

  ChunkStore hostStore, clientStore;
  net::StoreSync hostSync, clientSync;

  // THE FAKE STREAM. Two recorders, one per end.
  struct Recorder {
    std::vector<std::pair<IVec3, uint32_t>> delivered;
    std::vector<std::vector<uint32_t>> bytes;
    std::vector<IVec3> missed;
  };
  Recorder hostRec, clientRec;
  auto Bind = [](net::StoreSync& s, Recorder& r) {
    s.SetDelivery(
        [&r](IVec3 wc, uint32_t tick, const std::vector<uint32_t>& rle) {
          r.delivered.push_back({wc, tick});
          r.bytes.push_back(rle);
        },
        [&r](IVec3 wc) { r.missed.push_back(wc); });
  };
  Bind(hostSync, hostRec);
  Bind(clientSync, clientRec);

  // The pair is re-seated by claim (F), so the pump reads it through a
  // pointer rather than capturing one link.
  net::LoopbackPair lp = net::MakeLoopback();
  auto Pump = [&](int rounds) {
    for (int i = 0; i < rounds; i++) {
      lp.a->Poll();
      lp.b->Poll();
      net::Msg m;
      while (lp.a->Recv(m))
        hostSync.OnMessage((net::MsgType)m.type, m.payload.data(),
                           m.payload.size());
      while (lp.b->Recv(m))
        clientSync.OnMessage((net::MsgType)m.type, m.payload.data(),
                             m.payload.size());
      hostSync.Pump();
      clientSync.Pump();
    }
  };
  auto Wire = [&]() {
    hostSync.Connect(lp.a.get(), /*host=*/true, &hostStore, 0, 1);
    clientSync.Connect(lp.b.get(), /*host=*/false, &clientStore, 1, 0);
    hostSync.SetPeers(hostView, &clientView);
    clientSync.SetPeers(clientView, &hostView);
  };
  Wire();

  // ---- (A) the client's eviction lands in the host's store --------------
  clientSync.OnEvicted(wcClient, 4242, rleA);
  t.Eq("A: the put is outstanding until the ack comes back",
       (long long)clientSync.UnackedCount(), 1ll);
  Pump(8);
  {
    const std::vector<uint32_t>* got = hostStore.Get(wcClient);
    t.True("A: the host's store has the client's chunk", got != nullptr);
    t.True("A: ...with the client's bytes", got && *got == rleA);
    t.Eq("A: ...and the client's tick tag",
         (long long)hostStore.TickOf(wcClient), 4242ll);
    t.Eq("A: the ack cleared the unacked list",
         (long long)clientSync.UnackedCount(), 0ll);
    t.Eq("A: the host counted one accepted put",
         (long long)hostSync.Stats().putsRecv, 1ll);
  }

  // ---- (B) newer wins; older is refused AND acked -----------------------
  clientSync.OnEvicted(wcClient, 100, rleB);
  Pump(8);
  {
    const std::vector<uint32_t>* got = hostStore.Get(wcClient);
    t.True("B: an older put does not overwrite", got && *got == rleA);
    t.Eq("B: ...and the tag does not move",
         (long long)hostStore.TickOf(wcClient), 4242ll);
    t.Eq("B: the refusal is counted",
         (long long)hostSync.Stats().putsRefusedOld, 1ll);
    // The point of the claim: a refused put must still be RESOLVED, or the
    // client re-offers stale bytes on every reconnect for the rest of the
    // session.
    t.Eq("B: a refused put is still acked away",
         (long long)clientSync.UnackedCount(), 0ll);
  }
  clientSync.OnEvicted(wcClient, 5000, rleB);
  Pump(8);
  {
    const std::vector<uint32_t>* got = hostStore.Get(wcClient);
    t.True("B: a newer put does overwrite", got && *got == rleB);
    t.Eq("B: ...and carries the newer tag",
         (long long)hostStore.TickOf(wcClient), 5000ll);
  }

  // ---- (C) a non-authority says nothing ---------------------------------
  //
  // `wcHost` is inside the host's window with margin and the host is 6
  // chunks from it against the client's 50, so the host's copy is the live
  // one and the client's is a cache. Nothing may go out.
  {
    const uint64_t before = clientSync.Stats().putsSent;
    clientSync.OnEvicted(wcHost, 9000, rleC);
    Pump(8);
    t.Eq("C: evicting a chunk the peer still owns sends nothing",
         (long long)(clientSync.Stats().putsSent - before), 0ll);
    t.True("C: ...and the host's store never sees it",
           hostStore.Get(wcHost) == nullptr);
  }
  // ...and the mirror image, which is what proves (C) is a rule and not a
  // link that stopped working: a chunk in NOBODY's window is still the
  // evictor's to speak for, because nobody else has a copy at all.
  {
    const uint64_t before = clientSync.Stats().putsSent;
    clientSync.OnEvicted(wcNowhere, 9100, rleC);
    Pump(8);
    t.Eq("C: evicting a chunk nobody else holds DOES send",
         (long long)(clientSync.Stats().putsSent - before), 1ll);
    t.Eq("C: ...and the host stores it", (long long)hostStore.TickOf(wcNowhere),
         9100ll);
  }

  // ---- (D) manifest -> wanted -> get -> data -> deliver ------------------
  //
  // A chunk that is in the HOST's store and has never been near the client.
  // It sits in the host's window, so it is the host's to own — which is the
  // realistic case: the host edited it, evicted it, and now the client walks
  // over that ground for the first time.
  hostStore.Put(wcHost, rleC, 777);
  {
    const size_t rows = hostSync.SendFullManifest();
    Pump(16);
    t.True("D: the client received a complete manifest",
           clientSync.ManifestReady());
    t.True("D: ...covering every row the host sent",
           clientSync.ManifestSize() == rows);
    t.Eq("D: ...with the host's tick tag on the edited chunk",
         (long long)clientSync.ManifestTickOf(wcHost), 777ll);
  }
  t.True("D: a manifest hit is WANTED", clientSync.Wanted(wcHost));
  // The other half. Without it, a `Wanted` hard-wired to `true` would pass
  // every line above and hold every slot in the world.
  t.False("D: a chunk in no manifest is not wanted",
          clientSync.Wanted(wcClient3));
  {
    const size_t before = clientRec.delivered.size();
    clientSync.Request(wcHost);
    Pump(16);
    t.Eq("D: exactly one chunk was delivered",
         (long long)(clientRec.delivered.size() - before), 1ll);
    if (clientRec.delivered.size() > before) {
      const auto& d = clientRec.delivered.back();
      t.True("D: ...for the chunk that was asked for",
             d.first.x == wcHost.x && d.first.y == wcHost.y &&
                 d.first.z == wcHost.z);
      t.Eq("D: ...carrying the host's tag", (long long)d.second, 777ll);
      t.True("D: ...and the host's BYTES", clientRec.bytes.back() == rleC);
    }
  }

  // ---- (E) a miss is a message ------------------------------------------
  //
  // The host wants a chunk deep in the client's window (so the client is its
  // authority by `ChunkAuthority` — the refill form of the rule, where
  // residency IS the question). The client has never stored it, so the honest
  // answer is `ChunkMiss`, and the host's slot must be released rather than
  // held forever.
  t.True("E: the host wants a chunk the client is the authority for",
         hostSync.Wanted(wcClient2));
  {
    const size_t before = hostRec.missed.size();
    hostSync.Request(wcClient2);
    Pump(16);
    t.Eq("E: the miss came back as a message, not as silence",
         (long long)(hostRec.missed.size() - before), 1ll);
    t.Eq("E: ...and nothing was delivered for it",
         (long long)hostSync.Stats().dataRecv, 0ll);
  }

  // ---- (G) a disconnect answers every outstanding request ---------------
  //
  // Run BEFORE (F) because (F) re-seats the link and this claim is about the
  // link that is currently up. `Request` only queues, so not pumping leaves
  // it outstanding; `Disconnect` must miss it, or the slot behind it reads as
  // air for the rest of the process.
  {
    const size_t before = clientRec.missed.size();
    clientSync.Request(wcClient4);
    clientSync.Disconnect();
    t.Eq("G: a disconnect misses the request it cannot answer",
         (long long)(clientRec.missed.size() - before), 1ll);
    t.True("G: ...and counts it", clientSync.Stats().abandoned >= 1);
  }
  hostSync.Disconnect();

  // ---- (F) an unacked put survives a drop and is re-offered -------------
  //
  // The put goes out onto a link that is then thrown away before anything
  // could ack it, which is a dropped peer. The bytes are gone; the
  // OBLIGATION is not, and it lives in RAM across the gap (plan step 3).
  Wire();  // reconnect on the same (now reset) pair — nothing was in flight
  clientSync.OnEvicted(wcClient3, 6000, rleC);
  t.Eq("F: the put is outstanding", (long long)clientSync.UnackedCount(), 1ll);
  clientSync.Disconnect();
  hostSync.Disconnect();
  t.Eq("F: a disconnect does NOT forget an unacked put",
       (long long)clientSync.UnackedCount(), 1ll);
  {
    // A genuinely new connection: a fresh pair, so not one byte of the old
    // one can be responsible for what follows.
    lp = net::MakeLoopback();
    const uint64_t before = clientSync.Stats().reoffered;
    Wire();
    t.Eq("F: reconnecting re-offers it",
         (long long)(clientSync.Stats().reoffered - before), 1ll);
    Pump(8);
    t.Eq("F: ...and the host finally stores it",
         (long long)hostStore.TickOf(wcClient3), 6000ll);
    t.Eq("F: ...and the obligation is discharged",
         (long long)clientSync.UnackedCount(), 0ll);
  }

  // ---- (H) + (I) meta.svm ------------------------------------------------
  //
  // The only part of this gate that touches the GPU, and only because
  // `SaveWorld` is the sole writer of the file. Everything above ran in
  // microseconds on the CPU.
  const char* kDir = "selftest_storesync.svd";
  bool svm5Ok = false, svm4Ok = false;
  {
    GpuContext& ctx = c.ctx;
    std::filesystem::remove_all(kDir);
    const WorldStamp wrote{123456u, (uint32_t)kDefaultSeed, true};
    const bool saved =
        SaveWorld(ctx, c.world, c.stream, kDir, c.mats, nullptr, wrote);
    t.True("H: the world saved", saved);
    WorldStamp read{};
    const bool loaded =
        LoadWorld(ctx, c.world, c.sim, c.stream, kDir, c.mats, nullptr, &read);
    t.True("H: ...and loaded", loaded);
    t.True("H: the SVM5 pair is reported as KNOWN", read.known);
    t.Eq("H: the tick round-trips", (long long)read.tick, 123456ll);
    t.Eq("H: the seed round-trips", (long long)read.seed,
         (long long)kDefaultSeed);
    svm5Ok = saved && loaded && read.known && read.tick == 123456u;

    // ---- make a real SVM4 file out of the real SVM5 one -----------------
    //
    // Rewrite the magic and drop the two appended words: byte for byte, that
    // is what a pre-M9.5 build's writer produced. Patching the file rather
    // than hand-rolling one is what keeps this a test of the READER and not
    // of a second writer that could drift from the shipped one.
    const std::string metaPath = std::string(kDir) + "/meta.svm";
    std::vector<uint8_t> meta;
    if (FILE* fp = std::fopen(metaPath.c_str(), "rb")) {
      std::fseek(fp, 0, SEEK_END);
      const long len = std::ftell(fp);
      std::fseek(fp, 0, SEEK_SET);
      meta.resize(len > 0 ? (size_t)len : 0);
      if (!meta.empty())
        (void)std::fread(meta.data(), 1, meta.size(), fp);
      std::fclose(fp);
    }
    t.True("I: the saved meta.svm is long enough to demote", meta.size() > 8);
    if (meta.size() > 8) {
      const uint32_t v4 = 0x344D5653u;  // 'SVM4'
      std::memcpy(meta.data(), &v4, 4);
      meta.resize(meta.size() - 8);     // drop the appended tick + seed
      if (FILE* fp = std::fopen(metaPath.c_str(), "wb")) {
        (void)std::fwrite(meta.data(), 1, meta.size(), fp);
        std::fclose(fp);
      }
    }
    WorldStamp old{123u, 456u, true};  // pre-poisoned: the reader must clear it
    const bool loadedV4 =
        LoadWorld(ctx, c.world, c.sim, c.stream, kDir, c.mats, nullptr, &old);
    t.True("I: an SVM4 world still loads", loadedV4);
    t.False("I: ...and reports its tick/seed as UNKNOWN, not as zero",
            old.known);
    t.Eq("I: ...with the out-param cleared rather than left stale",
         (long long)old.tick, 0ll);
    svm4Ok = loadedV4 && !old.known;

    // Detach before deleting, exactly as `save-load` does: the store stays
    // bound to a directory for its lifetime otherwise, and the next gate
    // that saves would be refused ("one world dir per session").
    c.stream.Store().Unbind();
    std::filesystem::remove_all(kDir);
    // ...and put the world back where the suite expects it. `LoadWorld` left
    // the grid restored from a file; the gates after this one assume the
    // ordinary worldgen at the origin (the same courtesy `chunk-exchange`
    // pays, for the same reason).
    SubmitWorldgen(ctx, c.world, c.sim, kDefaultSeed);
    ctx.WaitIdle();
  }

  if (t.bad) {
    char b[448];
    std::snprintf(b, sizeof b, "%d/%d rows FAILED; first: %s", t.bad, t.rows,
                  t.why.c_str());
    detail = b;
    std::printf("store-sync: FAIL (%s)\n", b);
    return Status::Fail;
  }

  RecordObserved("net.storeSyncRows", (double)t.rows);
  const double pin = BaselineNumber("net.storeSyncRows", (double)t.rows);
  const bool pinOk = (double)t.rows == pin;
  if (!pinOk) MarkPinnedOnly();

  const net::StoreSync::Counters& hs = hostSync.Stats();
  const net::StoreSync::Counters& cs = clientSync.Stats();
  char b[640];
  std::snprintf(b, sizeof b,
                "%d/%.0f rows: puts sent %llu / recv %llu (1 refused as "
                "older, 1 re-offered after a drop); manifest %llu rows "
                "mirrored; gets %llu -> data %llu + misses %llu; meta.svm "
                "SVM5 %s, SVM4 %s",
                t.rows, pin, (unsigned long long)cs.putsSent,
                (unsigned long long)hs.putsRecv,
                (unsigned long long)clientSync.ManifestSize(),
                (unsigned long long)(cs.getsSent + hs.getsSent),
                (unsigned long long)(cs.dataRecv + hs.dataRecv),
                (unsigned long long)(cs.missesRecv + hs.missesRecv),
                svm5Ok ? "round-tripped" : "FAILED",
                svm4Ok ? "still loads (unknown pair)" : "FAILED");
  detail = b;
  std::printf("store-sync: %s\n", b);
  return pinOk ? Status::Pass : Status::Fail;
}

// ---- blast-players ----------------------------------------------------------
//
// ONE PLAYER'S GRENADE HITS THE OTHER PLAYER (W1-F, 2026-09-24; the owner's
// half at wave-1 integration).
//
// Phase K used to carve and launch THE SESSION'S OWN avatar beside the NPC
// calls, so with two players the thrower's blast reached the thrower and
// nobody else: B could stand on A's grenade untouched. The body pass is now
// `ExplosionHitsBodies` (session.h) and every player is reached through
// MobSystem's registered avatars. Over the wire the author's machine can only
// carve B's GHOST; B's machine applies A's blast to B's real body in phase N
// (`RemoteExplosionsHitOwnAvatars`), from the explosion indices
// `OpDelayQueue::Merge` reports as remote. The gate calls those functions --
// the code phases K and N run -- rather than restating them. CPU + Jolt only:
// no tick, no worldgen, nothing pinned.
//
//   (a) TWO LOCAL PLAYERS. A's blast at B's chest: B loses voxels AND goes
//       limp; A, 80 voxels off, loses nothing and stays up (the reach is the
//       blast's, not "every avatar"). B's limbs are in the impulse skip list.
//   (b) B IS A PEER'S GHOST (`SetAvatars(.., localCount = 1)`), A's machine,
//       phase K: the same blast carves it (the melee rule: presentation), does
//       NOT launch it -- a ghost's position is the wire's -- and emits NO gore
//       (the owner authors that; emitting it here too doubles it).
//   (c) B'S MACHINE, phase N. A's blast arrives as a REMOTE batch; the merge
//       names it remote; B's own avatar is carved AND launched and its gore
//       lands in the carry vector. A's ghost, standing in the same blast on
//       the far side, is untouched: A's machine already hit A in phase K.
//   (d) A'S MACHINE, phase N. The same blast is A's LOCAL op in A's merge, so
//       phase N applies nothing -- A and B's ghost, both in reach, are
//       untouched. Together with (b) that is "never applied twice".
namespace blastp {

enum class Arm { Local, Ghost, OwnerSide, AuthorSide };

struct BlastArm {
  uint32_t lostA = 0, lostB = 0;
  bool limpA = false, limpB = false;
  bool bSkipped = false;  // B's limb bodies were in AppendLiveLimbBodies
  size_t spawns = 0;      // gore into the batch (or phase N's carry vector)
  size_t remoteExps = 0;  // (c)/(d): what the merge reported as remote
  uint32_t applied = 0;   // (c)/(d): RemoteExplosionsHitOwnAvatars' count
  std::string why;
};

uint32_t AvatarVoxels(const PlayerAvatar& av) {
  uint32_t n = 0;
  for (int i = 0; i < av.PartCount(); i++) n += av.PartVoxelCount(i);
  return n;
}

// One blast through phase N's two calls, on the machine whose local id is
// `localId`. `authorId` authored the blast; the other id sends an empty batch.
uint32_t PhaseNOnce(Ctx& c, uint32_t localId, uint32_t authorId,
                    const ExplosionOp& e, BlastArm& r,
                    std::vector<ParticleSpawn>& gore) {
  net::OpDelayQueue q;
  q.SetLocalId(localId);
  const uint32_t t = 1, label = t + q.D();
  const IVec3 origin = c.world.WindowOrigin();
  OpBatch withBlast, empty;
  withBlast.exps.push_back(e);
  const uint32_t peerId = localId == 0 ? 1u : 0u;
  q.PushLocal(t, localId == authorId ? withBlast : empty, origin);
  q.NoteRemote(label, peerId, origin, peerId == authorId ? withBlast : empty);
  net::MergeStats ms;
  std::vector<uint32_t> remoteBrush, remoteExp;
  const OpBatch merged = q.Merge(label, c.world, ms, &remoteBrush, &remoteExp);
  r.remoteExps = remoteExp.size();
  // Merge noted author ranges for the merged vector; this gate submits
  // nothing, so drop them rather than hand them to the next gate's tick.
  sandvox::opstream::ClearAuthorRanges();
  return RemoteExplosionsHitOwnAvatars(merged.exps, remoteExp, c.world, c.phys,
                                       c.debris, c.mobs, gore);
}

void RunBlastArm(Ctx& c, Arm arm, BlastArm& r) {
  c.debris.Reset();
  c.mobs.Reset(true);
  const IVec3 wo = c.world.WindowOrigin();
  const int cx = (wo.x + (int)kNChunk / 2) * (int)kChunk;
  const int cz = (wo.z + (int)kNChunk / 2) * (int)kChunk;
  const int gy = World::TerrainHeight(cx, cz, c.world.WorldSeed());
  // Well above the ground: nothing but the blast may touch either body.
  const float y = (float)(gy + 40) + Player::kHalfY;
  // 12 voxels to B's side at chest height: close enough that the crater
  // bites B's near flank, far enough that it does not kill (a grenade AT the
  // chest takes the whole body, and a dead body is not launched), and well
  // inside the launch reach (radius x ragdoll.blastRadiusScale).
  const int off = 12;
  // (a)/(b): A 80 voxels off, out of reach. (c)/(d): A on the FAR side of the
  // blast, as close to it as B, so "A untouched" is a claim about who the
  // pass applies to and not about distance.
  const bool aInReach = arm == Arm::OwnerSide || arm == Arm::AuthorSide;
  Player pa, pb;
  pa.fly = pb.fly = true;
  pb.pos = Vec3{(float)cx + 0.5f, y, (float)cz + 0.5f};
  pa.pos = aInReach ? Vec3{(float)(cx - 2 * off) + 0.5f, y, (float)cz + 0.5f}
                    : Vec3{(float)cx + 80.5f, y, (float)cz + 0.5f};
  pa.SnapRender();
  pb.SnapRender();
  PlayerAvatar a(0x5A11EDU), b(0x5A11EDU + 1);
  a.Init(&c.phys, &c.world, &c.debris, c.mats, &c.mobs);
  b.Init(&c.phys, &c.world, &c.debris, c.mats, &c.mobs);
  a.SetDefs(&c.mobs.Defs(), kAvatarDefName);
  b.SetDefs(&c.mobs.Defs(), kAvatarDefName);
  if (!a.HasDef() || !a.Spawn(pa, 0.0f) || !b.Spawn(pb, 0.0f)) {
    r.why = std::string("could not spawn two \"") + kAvatarDefName + "\" avatars";
    a.Despawn();
    b.Despawn();
    return;
  }
  // The avatar list as the machine in question holds it: its own sessions
  // first, then the peer's ghost (RemotePlayersSyncAvatars' order).
  if (arm == Arm::OwnerSide) {
    Mob* avs[2] = {&b, &a};  // B's machine: B is mine, A a ghost
    c.mobs.SetAvatars(std::span<Mob* const>(avs, 2), 1);
  } else {
    Mob* avs[2] = {&a, &b};  // A's machine (or both local in (a))
    c.mobs.SetAvatars(std::span<Mob* const>(avs, 2), arm == Arm::Local ? 2 : 1);
  }

  std::vector<uint64_t> skip;
  c.mobs.AppendLiveLimbBodies(skip);
  const uint64_t bBody = b.PartBody(0);
  r.bSkipped = bBody != 0 &&
               std::find(skip.begin(), skip.end(), bBody) != skip.end();

  const uint32_t a0 = AvatarVoxels(a), b0 = AvatarVoxels(b);
  const auto& g = CurrentTuning().grenade;
  const ExplosionOp e{ifloor(pb.pos.x) - off, ifloor(pb.pos.y),
                      ifloor(pb.pos.z), g.blastRadius, g.blastPower, 0, 0, 0};
  std::vector<ParticleSpawn> spawns;
  if (arm == Arm::OwnerSide)
    r.applied = PhaseNOnce(c, 1, 0, e, r, spawns);
  else if (arm == Arm::AuthorSide)
    r.applied = PhaseNOnce(c, 0, 0, e, r, spawns);
  else
    ExplosionHitsBodies(e, c.world, c.phys, c.debris, c.mobs, spawns);
  const uint32_t a1 = AvatarVoxels(a), b1 = AvatarVoxels(b);
  r.lostA = a0 > a1 ? a0 - a1 : 0;
  r.lostB = b0 > b1 ? b0 - b1 : 0;
  r.limpA = a.Ragdoll() == Mob::RagdollPhase::Limp;
  r.limpB = b.Ragdoll() == Mob::RagdollPhase::Limp;
  r.spawns = spawns.size();

  // Leave nothing behind: limb bodies, severed limbs adopted by debris, and
  // the avatar list are all shared with the next gate.
  c.mobs.SetAvatars({});
  a.Despawn();
  b.Despawn();
  c.debris.Reset();
  c.mobs.Reset(true);
}

}  // namespace blastp

Status GateBlastPlayers(Ctx& c, std::string& detail) {
  using blastp::Arm;
  blastp::BlastArm local, ghost, owner, author;
  blastp::RunBlastArm(c, Arm::Local, local);
  if (local.why.empty()) blastp::RunBlastArm(c, Arm::Ghost, ghost);
  if (ghost.why.empty()) blastp::RunBlastArm(c, Arm::OwnerSide, owner);
  if (owner.why.empty()) blastp::RunBlastArm(c, Arm::AuthorSide, author);
  const std::string why = !local.why.empty()   ? local.why
                          : !ghost.why.empty() ? ghost.why
                          : !owner.why.empty() ? owner.why
                                               : author.why;
  if (!why.empty()) {
    detail = why;
    std::printf("blast-players: FAIL (%s)\n", why.c_str());
    return Status::Fail;
  }
  const bool localOk = local.lostB > 0 && local.limpB && local.lostA == 0 &&
                       !local.limpA && local.bSkipped;
  const bool ghostOk = ghost.lostB > 0 && !ghost.limpB && ghost.bSkipped &&
                       ghost.spawns == 0;
  const bool ownerOk = owner.remoteExps == 1 && owner.applied == 1 &&
                       owner.lostB > 0 && owner.limpB && owner.spawns > 0 &&
                       owner.lostA == 0 && !owner.limpA;
  const bool authorOk = author.remoteExps == 0 && author.applied == 0 &&
                        author.lostA == 0 && !author.limpA &&
                        author.lostB == 0 && !author.limpB &&
                        author.spawns == 0;
  char buf[720];
  std::snprintf(buf, sizeof buf,
                "(a) local B: -%u vox, %s, in skip list %d | A (80 vox off): "
                "-%u vox, %s | (b) ghost B: -%u vox, %s, gore %zu (want "
                "carved, NOT launched, 0 gore) | (c) B's machine: remote %zu, "
                "applied %u, B -%u vox %s gore %zu, ghost A -%u vox %s (want "
                "1/1, carved+launched, ghost untouched) | (d) A's machine: "
                "remote %zu, applied %u, A -%u %s, ghost B -%u %s (want 0, "
                "untouched)",
                local.lostB, local.limpB ? "launched" : "NOT LAUNCHED",
                local.bSkipped ? 1 : 0, local.lostA,
                local.limpA ? "LAUNCHED" : "standing", ghost.lostB,
                ghost.limpB ? "LAUNCHED" : "not launched", ghost.spawns,
                owner.remoteExps, owner.applied, owner.lostB,
                owner.limpB ? "launched" : "NOT LAUNCHED", owner.spawns,
                owner.lostA, owner.limpA ? "LAUNCHED" : "standing",
                author.remoteExps, author.applied, author.lostA,
                author.limpA ? "LAUNCHED" : "standing", author.lostB,
                author.limpB ? "LAUNCHED" : "standing");
  detail = buf;
  const bool ok = localOk && ghostOk && ownerOk && authorOk;
  std::printf("blast-players: %s (%s)\n", ok ? "PASS" : "FAIL", buf);
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& NetGates() {
  static const std::vector<Gate> g = {
      {"net-loopback", "net", {}, false, GateNetLoopback},
      // M9.4-A. No deps and no Ctx use: it reads net/authority.h and nothing
      // else, so it cannot be affected by what an earlier gate left behind.
      {"authority", "net", {}, false, GateAuthority},
      // M9.3-B. Unlike the two above this one SUBMITS: it worldgens three
      // times and runs 200 ticks per arm, which is why it is last in the
      // file and last in the group. No deps -- it builds its own world.
      {"ops-exchange", "net", {}, false, GateOpsExchange},
      // M9.5-B. Almost entirely CPU — two StoreSync ends over MakeLoopback,
      // no Stream and no world — except for the two meta.svm claims at the
      // end, which need SaveWorld because it is the only writer of the file.
      // It regenerates at the origin on the way out, so the gate after it
      // starts where it always did.
      {"store-sync", "net", {}, false, GateStoreSync},
      // W1-F + integration. CPU + Jolt only: two avatars, ExplosionHitsBodies
      // (phase K) and one OpDelayQueue merge into RemoteExplosionsHitOwnAvatars
      // (phase N) per side.
      // Resets debris and mobs on the way in and out.
      {"blast-players", "net", {}, false, GateBlastPlayers},
  };
  return g;
}

}  // namespace selftest
