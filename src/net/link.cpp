#include "net/link.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// ORDER IS LOAD-BEARING: <winsock2.h> must come before anything that pulls in
// <windows.h>, or windows.h drags in the winsock 1.1 header and every socket
// type is redefined. telemetry.cpp has the same block for the same reason.
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
constexpr SOCKET INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;
#endif

namespace net {
namespace {

#ifdef _WIN32
// One WSAStartup for the process, refcounted the way telemetry.cpp's
// EnsureWSA does it. Both may run in the same process (a --telemetry host),
// and WSAStartup is itself refcounted by the OS, so a second call is harmless;
// the static here just keeps it to one call per module.
bool g_wsaInit = false;
void EnsureWSA() {
  if (g_wsaInit) return;
  WSADATA wd;
  WSAStartup(MAKEWORD(2, 2), &wd);
  g_wsaInit = true;
}
void SetNonBlocking(SOCKET s) {
  u_long mode = 1;
  ioctlsocket(s, FIONBIO, &mode);
}
void CloseSocket(SOCKET s) { closesocket(s); }
bool WouldBlock() {
  const int e = WSAGetLastError();
  return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
}
int LastErr() { return WSAGetLastError(); }
#else
void EnsureWSA() {}
void SetNonBlocking(int s) { fcntl(s, F_SETFL, O_NONBLOCK); }
void CloseSocket(int s) { close(s); }
bool WouldBlock() {
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
}
int LastErr() { return errno; }
#endif

// TCP_NODELAY, set on every peer socket the moment we own one. See the class
// comment in link.h: without it a 150-byte-per-tick stream Nagle-coalesces and
// the lockstep pacer stalls on a LAN that has no real latency at all.
void SetNoDelay(SOCKET s) {
  int one = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
}

std::string Errno(const char* what) {
  char buf[96];
  std::snprintf(buf, sizeof buf, "%s failed (err %d)", what, LastErr());
  return buf;
}

// The in-memory byte pipe behind MakeLoopback(). Single-threaded by
// construction — both ends live on the frame/gate thread — so there is no
// mutex and no atomic here, and adding one would be the only thing in this
// file that could introduce a scheduling-dependent outcome.
struct Pipe {
  std::vector<uint8_t> buf;
  size_t off = 0;
  bool closed = false;

  void Put(const uint8_t* p, size_t n) { buf.insert(buf.end(), p, p + n); }
  size_t Take(uint8_t* dst, size_t max) {
    const size_t avail = buf.size() - off;
    const size_t n = max < avail ? max : avail;
    if (n) std::memcpy(dst, buf.data() + off, n);
    off += n;
    // Compact only once the consumed prefix dominates, so a long stream is
    // O(total) rather than O(total^2) in memmove.
    if (off > 65536 && off * 2 > buf.size()) {
      buf.erase(buf.begin(), buf.begin() + (ptrdiff_t)off);
      off = 0;
    }
    return n;
  }
};

class LoopbackLink : public LinkBase {
 public:
  LoopbackLink(std::shared_ptr<Pipe> tx, std::shared_ptr<Pipe> rx)
      : tx_(std::move(tx)), rx_(std::move(rx)) {}

  void Poll() override {
    if (failed_) return;
    // Write half: hand everything queued to the peer's pipe. A memory pipe
    // never short-writes, so this is where TcpLink's partial-write logic has
    // no analogue — which is exactly why the TCP arm of the gate exists.
    if (out_.size() > outSent_) {
      tx_->Put(out_.data() + outSent_, out_.size() - outSent_);
      outSent_ = out_.size();
      out_.clear();
      outSent_ = 0;
    }
    // Read half: BOUNDED, so reassembly across partial reads is genuinely
    // exercised (link.h, kLoopbackChunkBytes).
    uint8_t tmp[kLoopbackChunkBytes];
    const size_t n = rx_->Take(tmp, sizeof tmp);
    if (n) Ingest(tmp, n);
    if (rx_->closed && rx_->off >= rx_->buf.size() && !closed_)
      Fail("peer closed");
  }
  bool Connected() const override { return !closed_ && !failed_; }
  void Close() override {
    if (closed_) return;
    closed_ = true;
    tx_->closed = true;
  }

 private:
  std::shared_ptr<Pipe> tx_, rx_;
  bool closed_ = false;
};

}  // namespace

double NowSeconds() {
  using clock = std::chrono::steady_clock;
  static const clock::time_point t0 = clock::now();
  return std::chrono::duration<double>(clock::now() - t0).count();
}

Link::~Link() = default;

// ---- LinkBase: framing, queues, stats ----------------------------------

bool LinkBase::Send(uint16_t type, uint16_t ver, const uint8_t* p, size_t n) {
  if (failed_) return false;
  if (n > kMaxFrameBytes) {
    // REFUSE WITHOUT QUEUEING. Writing the header and then discovering the
    // payload is too big would leave a length word in the stream with no
    // bytes behind it and desynchronise the peer's reassembler forever.
    return false;
  }
  const uint32_t len = (uint32_t)n;
  uint8_t hdr[kFrameHeaderBytes];
  std::memcpy(hdr + 0, &len, 4);
  std::memcpy(hdr + 4, &type, 2);
  std::memcpy(hdr + 6, &ver, 2);
  out_.insert(out_.end(), hdr, hdr + kFrameHeaderBytes);
  if (n) out_.insert(out_.end(), p, p + n);
  stats_.msgsOut++;
  stats_.bytesOut += kFrameHeaderBytes + n;
  return true;
}

bool LinkBase::Recv(Msg& out) {
  if (rx_.empty()) return false;
  out = std::move(rx_.front());
  rx_.pop_front();
  return true;
}

void LinkBase::Ingest(const uint8_t* p, size_t n) {
  if (!n || failed_) return;
  in_.insert(in_.end(), p, p + n);
  stats_.bytesIn += n;
  stats_.lastRecvSeconds = NowSeconds();
  for (;;) {
    const size_t avail = in_.size() - inOff_;
    if (avail < kFrameHeaderBytes) break;
    uint32_t len = 0;
    uint16_t type = 0, ver = 0;
    std::memcpy(&len, in_.data() + inOff_ + 0, 4);
    std::memcpy(&type, in_.data() + inOff_ + 4, 2);
    std::memcpy(&ver, in_.data() + inOff_ + 6, 2);
    if (len > kMaxFrameBytes) {
      char buf[96];
      std::snprintf(buf, sizeof buf, "frame length %u exceeds %u", len,
                    kMaxFrameBytes);
      Fail(buf);
      return;
    }
    if (avail < kFrameHeaderBytes + len) break;  // wait for the rest
    Msg m;
    m.type = type;
    m.ver = ver;
    m.payload.assign(in_.data() + inOff_ + kFrameHeaderBytes,
                     in_.data() + inOff_ + kFrameHeaderBytes + len);
    inOff_ += kFrameHeaderBytes + len;
    stats_.msgsIn++;
    rx_.push_back(std::move(m));
  }
  // Same compaction rule as Pipe::Take, and for the same reason.
  if (inOff_ > 65536 && inOff_ * 2 > in_.size()) {
    in_.erase(in_.begin(), in_.begin() + (ptrdiff_t)inOff_);
    inOff_ = 0;
  }
}

void LinkBase::Reset() {
  failed_ = false;
  err_.clear();
  // Every byte of the dead peer's conversation goes with it: a half-received
  // frame reassembled against the NEXT peer's bytes is a protocol error at
  // best and a silently mis-typed message at worst.
  out_.clear();
  outSent_ = 0;
  in_.clear();
  inOff_ = 0;
  rx_.clear();
}

void LinkBase::Fail(const std::string& why) {
  if (failed_) return;   // FIRST reason wins: it is the one with a cause
  failed_ = true;
  err_ = why;
}

// ---- TcpLink ------------------------------------------------------------

TcpLink::TcpLink() { EnsureWSA(); }
TcpLink::~TcpLink() { Close(); }

bool TcpLink::Listen(uint16_t port) {
  if (listen_ != -1) return true;
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) {
    Fail(Errno("socket"));
    return false;
  }
  // NO SO_REUSEADDR, for the reason telemetry.cpp spells out: on Windows it
  // means "bind even though a live socket already holds this port", so two
  // hosts would share one port and which one a client reached would be
  // indeterminate. A refusal can be printed; a share cannot.
  SetNonBlocking(s);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (bind(s, (sockaddr*)&addr, sizeof addr) != 0) {
    Fail(Errno("bind"));
    CloseSocket(s);
    return false;
  }
  if (::listen(s, 2) != 0) {
    Fail(Errno("listen"));
    CloseSocket(s);
    return false;
  }
  // READ THE PORT BACK. With `port == 0` the OS picked one and the caller has
  // no other way to learn it; the gate needs it, and so does anything that
  // ever prints "joinable at ...". getsockname is the only source of truth
  // here even when a port WAS requested, since a bind can be redirected.
  sockaddr_in bound{};
  socklen_t blen = sizeof bound;
  if (getsockname(s, (sockaddr*)&bound, &blen) == 0)
    listenPort_ = ntohs(bound.sin_port);
  else
    listenPort_ = port;
  listen_ = (intptr_t)s;
  return true;
}

bool TcpLink::Connect(const std::string& ip, uint16_t port) {
  if (peer_ != -1) return true;
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) {
    Fail(Errno("socket"));
    return false;
  }
  SetNonBlocking(s);
  // Before connect, not after: on a connected socket a failed setsockopt is
  // silent, and on Windows the option survives the handshake either way.
  SetNoDelay(s);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
    Fail("bad ip: " + ip);
    CloseSocket(s);
    return false;
  }
  const int r = ::connect(s, (sockaddr*)&addr, sizeof addr);
  if (r != 0 && !WouldBlock()) {
    Fail(Errno("connect"));
    CloseSocket(s);
    return false;
  }
  peer_ = (intptr_t)s;
  // A non-blocking connect that returned 0 is already up (loopback often
  // does); otherwise Poll() watches for writability.
  connecting_ = (r != 0);
  connected_ = (r == 0);
  return true;
}

void TcpLink::Poll() {
  if (failed_) return;
  if (listen_ != -1 && peer_ == -1) Accept();
  if (connecting_) PollConnecting();
  if (peer_ == -1 || !connected_) return;
  // FLUSH BEFORE READ. The pacer's first invariant is send-before-wait, and
  // the frame loop's only chance to honour it is that Poll() pushes this
  // tick's outbound batch out before it looks for the peer's — a Poll that
  // read first would add a whole frame of latency to every batch.
  FlushOut();
  ReadPeer();
}

void TcpLink::Accept() {
  SOCKET c = accept((SOCKET)listen_, nullptr, nullptr);
  if (c == INVALID_SOCKET) return;
  if (peer_ != -1) {
    // ONE PEER PER LINK. Close it now rather than leaving it in the backlog:
    // a second client then gets a reset it can report instead of a connection
    // that appears to succeed and never carries a byte.
    CloseSocket(c);
    return;
  }
  SetNonBlocking(c);
  SetNoDelay(c);
  peer_ = (intptr_t)c;
  connected_ = true;
  connecting_ = false;
  stats_.lastRecvSeconds = NowSeconds();
}

void TcpLink::PollConnecting() {
  fd_set wr, ex;
  FD_ZERO(&wr);
  FD_ZERO(&ex);
  FD_SET((SOCKET)peer_, &wr);
  FD_SET((SOCKET)peer_, &ex);
  timeval tv{0, 0};
  const int n = select((int)peer_ + 1, nullptr, &wr, &ex, &tv);
  if (n <= 0) return;
  if (FD_ISSET((SOCKET)peer_, &ex)) {
    DropPeer("connect refused");
    return;
  }
  if (!FD_ISSET((SOCKET)peer_, &wr)) return;
  // Writable is necessary but not sufficient on every stack: SO_ERROR is the
  // portable "did the handshake actually succeed" answer.
  int soErr = 0;
  socklen_t elen = sizeof soErr;
  if (getsockopt((SOCKET)peer_, SOL_SOCKET, SO_ERROR, (char*)&soErr, &elen) == 0 &&
      soErr != 0) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "connect failed (so_error %d)", soErr);
    DropPeer(buf);
    return;
  }
  connecting_ = false;
  connected_ = true;
  SetNoDelay((SOCKET)peer_);
  stats_.lastRecvSeconds = NowSeconds();
}

void TcpLink::ReadPeer() {
  uint8_t tmp[16384];
  for (;;) {
    const int n = recv((SOCKET)peer_, (char*)tmp, (int)sizeof tmp, 0);
    if (n > 0) {
      Ingest(tmp, (size_t)n);
      if (failed_) return;
      if (n < (int)sizeof tmp) return;   // drained
      continue;
    }
    if (n == 0) {
      DropPeer("peer closed");
      return;
    }
    if (!WouldBlock()) DropPeer(Errno("recv"));
    return;
  }
}

void TcpLink::FlushOut() {
  while (out_.size() > outSent_) {
    const size_t left = out_.size() - outSent_;
    // Cap one send at 64 KiB: a 4 MiB write on a non-blocking socket returns
    // a partial count anyway, and a smaller call keeps the loop's bookkeeping
    // in the cheap path.
    const int want = (int)(left < 65536 ? left : 65536);
    const int n = send((SOCKET)peer_, (const char*)out_.data() + outSent_, want, 0);
    if (n > 0) {
      // THE PARTIAL WRITE. `n` may be less than `want` with no error at all;
      // the remainder stays queued for the next Poll. Treating a short write
      // as a full one silently truncates the frame and the peer's reassembler
      // then waits forever for bytes that were never sent.
      outSent_ += (size_t)n;
      continue;
    }
    if (n == 0 || WouldBlock()) break;
    DropPeer(Errno("send"));
    return;
  }
  if (outSent_ && outSent_ == out_.size()) {
    out_.clear();
    outSent_ = 0;
  } else if (outSent_ > 65536) {
    out_.erase(out_.begin(), out_.begin() + (ptrdiff_t)outSent_);
    outSent_ = 0;
  }
}

void TcpLink::DropPeer(const std::string& why) {
  if (peer_ != -1) CloseSocket((SOCKET)peer_);
  peer_ = -1;
  connected_ = false;
  connecting_ = false;
  Fail(why);
}

bool TcpLink::NoDelay() const {
  if (peer_ == -1) return false;
  int v = 0;
  socklen_t len = sizeof v;
  if (getsockopt((SOCKET)peer_, IPPROTO_TCP, TCP_NODELAY, (char*)&v, &len) != 0)
    return false;
  return v != 0;
}

void TcpLink::Close() {
  if (peer_ != -1) CloseSocket((SOCKET)peer_);
  if (listen_ != -1) CloseSocket((SOCKET)listen_);
  peer_ = -1;
  listen_ = -1;
  connected_ = false;
  connecting_ = false;
}

// ---- loopback -----------------------------------------------------------

LoopbackPair MakeLoopback() {
  auto ab = std::make_shared<Pipe>();
  auto ba = std::make_shared<Pipe>();
  LoopbackPair p;
  p.a = std::make_unique<LoopbackLink>(ab, ba);
  p.b = std::make_unique<LoopbackLink>(ba, ab);
  return p;
}

}  // namespace net
