#include "telemetry.h"

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

static bool g_wsaInit = false;
static void EnsureWSA() {
  if (g_wsaInit) return;
  WSADATA wd;
  WSAStartup(MAKEWORD(2, 2), &wd);
  g_wsaInit = true;
}
static void SetNonBlocking(SOCKET s) {
  u_long mode = 1;
  ioctlsocket(s, FIONBIO, &mode);
}
static void CloseSocket(SOCKET s) { closesocket(s); }
static bool WouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }

#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
static void EnsureWSA() {}
static void SetNonBlocking(int s) { fcntl(s, F_SETFL, O_NONBLOCK); }
static void CloseSocket(int s) { close(s); }
static bool WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
typedef int SOCKET;
constexpr SOCKET INVALID_SOCKET = -1;
#endif

// SHA-1 for the WebSocket handshake — minimal, correct, not for crypto.
namespace {
struct SHA1 {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  uint8_t buf[64];
  int bufLen = 0;
  uint64_t totalBits = 0;

  static uint32_t rotl(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

  void ProcessBlock(const uint8_t* blk) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
      w[i] = (uint32_t)blk[i * 4] << 24 | (uint32_t)blk[i * 4 + 1] << 16 |
             (uint32_t)blk[i * 4 + 2] << 8 | blk[i * 4 + 3];
    for (int i = 16; i < 80; i++)
      w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6; }
      uint32_t t = rotl(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rotl(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }

  void Update(const void* data, int len) {
    auto* p = (const uint8_t*)data;
    totalBits += (uint64_t)len * 8;
    while (len > 0) {
      int n = 64 - bufLen;
      if (n > len) n = len;
      std::memcpy(buf + bufLen, p, n);
      bufLen += n; p += n; len -= n;
      if (bufLen == 64) { ProcessBlock(buf); bufLen = 0; }
    }
  }

  void Final(uint8_t out[20]) {
    buf[bufLen++] = 0x80;
    if (bufLen > 56) {
      std::memset(buf + bufLen, 0, 64 - bufLen);
      ProcessBlock(buf); bufLen = 0;
    }
    std::memset(buf + bufLen, 0, 56 - bufLen);
    for (int i = 0; i < 8; i++) buf[56 + i] = (uint8_t)(totalBits >> (56 - i * 8));
    ProcessBlock(buf);
    for (int i = 0; i < 5; i++) {
      out[i * 4] = (uint8_t)(h[i] >> 24); out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
      out[i * 4 + 2] = (uint8_t)(h[i] >> 8); out[i * 4 + 3] = (uint8_t)h[i];
    }
  }
};

static const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64(const uint8_t* d, int n) {
  std::string r;
  for (int i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)d[i] << 16;
    if (i + 1 < n) v |= (uint32_t)d[i + 1] << 8;
    if (i + 2 < n) v |= d[i + 2];
    r += kB64[(v >> 18) & 63];
    r += kB64[(v >> 12) & 63];
    r += (i + 1 < n) ? kB64[(v >> 6) & 63] : '=';
    r += (i + 2 < n) ? kB64[v & 63] : '=';
  }
  return r;
}
}  // namespace

bool Telemetry::Start(uint16_t port) {
  if (listen_ != kInvalid) return true;
  EnsureWSA();

  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) {
    std::fprintf(stderr, "telemetry: socket() failed\n");
    return false;
  }
  int opt = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
  SetNonBlocking(s);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
    std::fprintf(stderr, "telemetry: bind(:%d) failed\n", port);
    CloseSocket(s);
    return false;
  }
  listen(s, 4);
  listen_ = (intptr_t)s;
  std::printf("telemetry -> ws://127.0.0.1:%d/\n", port);
  return true;
}

void Telemetry::Poll() {
  if (listen_ == kInvalid) return;
  Accept();
}

void Telemetry::Accept() {
  for (;;) {
    SOCKET c = accept((SOCKET)listen_, nullptr, nullptr);
    if (c == INVALID_SOCKET) return;
    SetNonBlocking(c);
    if (!Handshake((intptr_t)c)) { CloseSocket(c); continue; }
    bool placed = false;
    for (int i = 0; i < kMaxClients; i++) {
      if (clients_[i] == kInvalid) { clients_[i] = (intptr_t)c; placed = true; break; }
    }
    if (!placed) CloseSocket(c);
  }
}

bool Telemetry::Handshake(intptr_t fd) {
  int total = 0;
  for (int tries = 0; tries < 50; tries++) {
    int n = recv((SOCKET)fd, recvBuf_ + total, (int)sizeof(recvBuf_) - total - 1, 0);
    if (n > 0) {
      total += n;
      recvBuf_[total] = 0;
      if (std::strstr(recvBuf_, "\r\n\r\n")) break;
    } else if (n == 0) return false;
    else if (WouldBlock()) {
#ifdef _WIN32
      Sleep(1);
#else
      usleep(1000);
#endif
      continue;
    } else return false;
  }
  if (!std::strstr(recvBuf_, "\r\n\r\n")) return false;

  const char* keyHdr = std::strstr(recvBuf_, "Sec-WebSocket-Key:");
  if (!keyHdr) keyHdr = std::strstr(recvBuf_, "sec-websocket-key:");
  if (!keyHdr) return false;
  keyHdr += 18;
  while (*keyHdr == ' ') keyHdr++;
  const char* keyEnd = std::strstr(keyHdr, "\r\n");
  if (!keyEnd) return false;
  std::string key(keyHdr, keyEnd);
  key += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

  SHA1 sha;
  sha.Update(key.data(), (int)key.size());
  uint8_t hash[20];
  sha.Final(hash);

  std::string resp =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: " + Base64(hash, 20) + "\r\n\r\n";
  send((SOCKET)fd, resp.data(), (int)resp.size(), 0);
  return true;
}

void Telemetry::Send(intptr_t fd, const char* data, int len) {
  uint8_t hdr[10];
  int hdrLen;
  hdr[0] = 0x81;  // text frame, FIN
  if (len < 126) {
    hdr[1] = (uint8_t)len;
    hdrLen = 2;
  } else if (len < 65536) {
    hdr[1] = 126;
    hdr[2] = (uint8_t)(len >> 8);
    hdr[3] = (uint8_t)len;
    hdrLen = 4;
  } else {
    hdr[1] = 127;
    std::memset(hdr + 2, 0, 4);
    hdr[6] = (uint8_t)(len >> 24); hdr[7] = (uint8_t)(len >> 16);
    hdr[8] = (uint8_t)(len >> 8); hdr[9] = (uint8_t)len;
    hdrLen = 10;
  }
  send((SOCKET)fd, (const char*)hdr, hdrLen, 0);
  send((SOCKET)fd, data, len, 0);
}

void Telemetry::Drop(intptr_t fd) {
  CloseSocket((SOCKET)fd);
  for (int i = 0; i < kMaxClients; i++) {
    if (clients_[i] == fd) { clients_[i] = kInvalid; break; }
  }
}

void Telemetry::Broadcast(uint32_t tick, const TelemetryStage* stages, int count) {
  if (listen_ == kInvalid) return;
  bool anyClient = false;
  for (int i = 0; i < kMaxClients; i++) {
    if (clients_[i] != kInvalid) { anyClient = true; break; }
  }
  if (!anyClient) return;

  char json[2048];
  int pos = std::snprintf(json, sizeof(json), "{\"tick\":%u,\"stages\":{", tick);
  for (int i = 0; i < count && pos < (int)sizeof(json) - 64; i++) {
    if (i) json[pos++] = ',';
    pos += std::snprintf(json + pos, sizeof(json) - pos, "\"%s\":{\"ms\":%.3f}",
                         stages[i].name, stages[i].ms);
  }
  pos += std::snprintf(json + pos, sizeof(json) - pos, "}}");

  for (int i = 0; i < kMaxClients; i++) {
    if (clients_[i] == kInvalid) continue;
    char peek;
    int r = recv((SOCKET)clients_[i], &peek, 1, MSG_PEEK);
    if (r == 0) { Drop(clients_[i]); continue; }
    if (r < 0 && !WouldBlock()) { Drop(clients_[i]); continue; }
    Send(clients_[i], json, pos);
  }
}

void Telemetry::Shutdown() {
  for (int i = 0; i < kMaxClients; i++) {
    if (clients_[i] != kInvalid) { CloseSocket((SOCKET)clients_[i]); clients_[i] = kInvalid; }
  }
  if (listen_ != kInvalid) { CloseSocket((SOCKET)listen_); listen_ = kInvalid; }
}
