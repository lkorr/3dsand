#pragma once
#include <cstdint>

#include "measure/perfnodes.h"

// Minimal localhost-only WebSocket server for streaming per-frame telemetry to
// the sandvox tuner's Engine and Performance tabs.  Zero cost when disabled
// (no --telemetry).
//
// TWO WIRE FORMATS, on one socket, because the two tabs want different things
// and neither should be able to break the other.
//
//   v1 (Broadcast):        {"tick":N,"stages":{"ca":{"ms":0.18},...}}
//                          The Engine tab's live chips. Stage keys match its
//                          data-eng-stage attributes, so adding a stage there
//                          needs no page changes beyond a new block.
//
//   v2 (BroadcastSample):  {"v":2,"tick":N,"frame":F,"wallMs":16.7,
//                           "cpu":{...},"gpu":{...},"gpuValid":true,
//                           "counters":{...},"stages":{...}}
//                          The Performance tab's live view. Keys come from
//                          measure/perfnodes.h, so a live session and a
//                          recorded --perf run are the SAME SHAPE and the page
//                          draws them with the same code.
//
// A v2 message carries a v1 `stages` object as well. That is not redundancy for
// its own sake: it means turning the Performance tab on cannot make the Engine
// tab stop updating, and there is exactly one send per frame rather than two
// competing ones.

struct TelemetryStage {
  const char* name;
  double ms;
};

class Telemetry {
 public:
  bool Start(uint16_t port = 8080);
  void Broadcast(uint32_t tick, const TelemetryStage* stages, int count);
  // The rich per-frame sample. `sample` is the same struct the --perf harness
  // records, so anything the recorded page can draw, the live page can too.
  void BroadcastSample(const sandvox::PerfSample& sample);
  void Poll();
  void Shutdown();
  bool Active() const { return listen_ != kInvalid; }
  // True only when a browser is actually attached. The frame loop checks this
  // before doing any of the work that exists only to feed the socket — with
  // --telemetry on and nobody watching, the cost is one pointer compare.
  bool HasClient() const;

 private:
  void Accept();
  bool Handshake(intptr_t fd);
  void Send(intptr_t fd, const char* data, int len);
  void SendAll(const char* json, int len);
  void Drop(intptr_t fd);

  static constexpr intptr_t kInvalid = -1;
  intptr_t listen_ = kInvalid;
  static constexpr int kMaxClients = 4;
  intptr_t clients_[kMaxClients] = {kInvalid, kInvalid, kInvalid, kInvalid};
  char recvBuf_[4096] = {};
};
