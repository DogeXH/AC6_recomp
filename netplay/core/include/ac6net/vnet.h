// ac6net - virtual BSD sockets for the guest (ARCHITECTURE.md 5.4/5.5).
//
// All addresses and ports are host order. Every call returns the Winsock-style result
// (>= 0 success, -1 error) and writes the WSA error code to *err (wsa::kOk on success).
// The glue copies *err into the calling guest thread's last-error slot.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "ac6net/types.h"

namespace ac6net {
class PeerLink;  // src/link.h
}

namespace ac6net::vnet {

using SocketId = int32_t;  // > 0 valid

inline constexpr int kSockStream = 1;
inline constexpr int kSockDgram = 2;
inline constexpr int kProtoTcp = 6;
inline constexpr int kProtoUdp = 17;
inline constexpr int kProtoVdp = 254;

// Option constants (guest/Winsock values).
inline constexpr int kSolSocket = 0xFFFF;
inline constexpr int kSoReuseAddr = 0x0004;
inline constexpr int kSoKeepAlive = 0x0008;
inline constexpr int kSoBroadcast = 0x0020;
inline constexpr int kSoLinger = 0x0080;
inline constexpr int kSoSndBuf = 0x1001;
inline constexpr int kSoRcvBuf = 0x1002;
inline constexpr int kSoSndTimeo = 0x1005;
inline constexpr int kSoRcvTimeo = 0x1006;
inline constexpr int kSoError = 0x1007;
inline constexpr int kSoType = 0x1008;
inline constexpr int kIpprotoTcp = 6;
inline constexpr int kTcpNoDelay = 0x0001;

inline constexpr int kSdReceive = 0;
inline constexpr int kSdSend = 1;
inline constexpr int kSdBoth = 2;

inline constexpr uint32_t kInaddrAny = 0;
inline constexpr uint32_t kInaddrBroadcast = 0xFFFFFFFF;

struct Endpoint {
  uint32_t ip = 0;  // host order
  uint16_t port = 0;
};

// Select request/response. On return each vector holds only the ready sockets (Winsock
// semantics: the sets are rewritten). Returns the total number of ready sockets, 0 on timeout.
struct SelectSets {
  std::vector<SocketId> read;
  std::vector<SocketId> write;
  std::vector<SocketId> except;
};

struct Stats {
  uint64_t dgrams_sent = 0, dgrams_recv = 0, dgrams_dropped = 0;
  uint64_t stream_bytes_sent = 0, stream_bytes_recv = 0;
  uint32_t sockets_open = 0, streams_open = 0;
};

class VNet {
 public:
  // `link` delivers datagrams/stream segments to peers and reports the own vIP (which may change
  // from 0 to a real value after registration). Destinations equal to the own vIP or 127.0.0.1
  // are delivered in-process without touching the link.
  explicit VNet(PeerLink* link);
  ~VNet();
  VNet(const VNet&) = delete;
  VNet& operator=(const VNet&) = delete;

  SocketId Socket(int type, int protocol, int* err);
  int Close(SocketId s, int* err);
  int Bind(SocketId s, Endpoint local, int* err);
  int Connect(SocketId s, Endpoint remote, int* err);
  int Listen(SocketId s, int backlog, int* err);
  SocketId Accept(SocketId s, Endpoint* remote, int* err);
  int Send(SocketId s, const void* data, size_t len, int flags, int* err);
  int Recv(SocketId s, void* data, size_t len, int flags, int* err);
  int SendTo(SocketId s, const void* data, size_t len, int flags, Endpoint to, int* err);
  int RecvFrom(SocketId s, void* data, size_t len, int flags, Endpoint* from, int* err);
  int Shutdown(SocketId s, int how, int* err);
  int SetNonBlocking(SocketId s, bool non_blocking, int* err);
  int Available(SocketId s, uint32_t* bytes, int* err);  // FIONREAD
  int SetOption(SocketId s, int level, int option, const void* value, size_t len, int* err);
  // `value`/`len` in/out; integers are written host-endian (the glue byte-swaps for the guest).
  int GetOption(SocketId s, int level, int option, void* value, size_t* len, int* err);
  int GetSockName(SocketId s, Endpoint* local, int* err);
  int GetPeerName(SocketId s, Endpoint* remote, int* err);
  // timeout_us < 0 = infinite. Unknown ids produce -1/kNotSock.
  int Select(SelectSets* sets, int64_t timeout_us, int* err);
  bool IsSocket(SocketId s) const;

  // Called by the overlay (network thread) for channels 0x01-0x03. Not for guest use.
  void OnLinkData(uint32_t from_vip, uint8_t channel, const uint8_t* data, size_t len);
  void OnPeerLost(uint32_t vip);
  // Called by the network thread every ~5 ms (KCP update, connect timeouts).
  void Tick(uint64_t now_ms);

  Stats stats() const;
  void CloseAll();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ac6net::vnet
