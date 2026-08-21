#pragma once
#include <cstdint>

// Minimal localhost-only WebSocket server for streaming per-tick telemetry to
// the sandvox tuner's Engine tab.  Zero cost when disabled (--telemetry off).
//
// The wire format is one JSON object per tick:
//   {"tick":N,"stages":{"ca":{"ms":0.18},"render":{"ms":2.66},...}}
//
// Stage keys match the data-eng-stage attributes in tuner.html, so adding a
// stage to the engine needs no page changes beyond a new <details> block.

struct TelemetryStage {
  const char* name;
  double ms;
};

class Telemetry {
 public:
  bool Start(uint16_t port = 8080);
  void Broadcast(uint32_t tick, const TelemetryStage* stages, int count);
  void Poll();
  void Shutdown();
  bool Active() const { return listen_ != kInvalid; }

 private:
  void Accept();
  bool Handshake(intptr_t fd);
  void Send(intptr_t fd, const char* data, int len);
  void Drop(intptr_t fd);

  static constexpr intptr_t kInvalid = -1;
  intptr_t listen_ = kInvalid;
  static constexpr int kMaxClients = 4;
  intptr_t clients_[kMaxClients] = {kInvalid, kInvalid, kInvalid, kInvalid};
  char recvBuf_[4096] = {};
};
