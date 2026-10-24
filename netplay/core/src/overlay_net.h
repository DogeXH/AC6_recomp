// ac6net - minimal portable UDP socket layer for the overlay (POSIX + Winsock).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace ac6net::net {

#if defined(_WIN32)
using SocketHandle = SOCKET;
inline const SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

// An IPv4 or IPv6 UDP endpoint.
struct Addr {
  sockaddr_storage ss{};
  socklen_t len = 0;

  bool valid() const { return len != 0; }
  int family() const { return ss.ss_family; }
  bool IsV4() const { return valid() && family() == AF_INET; }
  bool IsV6() const { return valid() && family() == AF_INET6; }
  bool IsLoopback() const;
  uint16_t Port() const;
  std::string Ip() const;        // "1.2.3.4" or "2001:db8::1"
  std::string ToString() const;  // "1.2.3.4:5" or "[2001:db8::1]:5"
  bool SameIp(const Addr& other) const;
  bool operator==(const Addr& other) const;  // family, ip and port

  // Parses an IPv4 or IPv6 literal (no brackets, no scope ids).
  static bool FromIpPort(const std::string& ip, uint16_t port, Addr* out);
};

// Process-wide socket library initialisation (WSAStartup on Windows; no-op elsewhere).
bool NetInit();

class UdpSocket {
 public:
  UdpSocket() = default;
  ~UdpSocket() { Close(); }
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  // Opens a non-blocking socket bound to the wildcard address of `family` at `port` (0 =
  // ephemeral). AF_INET6 sockets are IPV6_V6ONLY.
  bool Open(int family, uint16_t port, std::string* error);
  void Close();
  bool valid() const { return fd_ != kInvalidSocket; }
  SocketHandle handle() const { return fd_; }
  uint16_t port() const { return port_; }
  int family() const { return family_; }

  // > 0: datagram length; 0: nothing pending; -1: a transient error (skip and try again).
  // Datagrams longer than `cap` are truncated (the caller's AEAD check rejects them).
  int RecvFrom(uint8_t* buf, size_t cap, Addr* from);
  bool SendTo(const Addr& to, const uint8_t* data, size_t len);

 private:
  SocketHandle fd_ = kInvalidSocket;
  uint16_t port_ = 0;
  int family_ = 0;
};

// Waits up to `timeout_ms` until one of `socks` (invalid entries are ignored) is readable.
// readable[i] is set for each ready socket. Returns false if none became ready.
bool PollReadable(UdpSocket* const* socks, size_t n, int timeout_ms, bool* readable);

// Windows: raises the process timer resolution to 1 ms while alive (timeBeginPeriod), so the
// ~5 ms network loop does not round its poll timeout up to the 15.6 ms default tick. No-op on
// other platforms.
class ScopedTimerResolution {
 public:
  ScopedTimerResolution();
  ~ScopedTimerResolution();
  ScopedTimerResolution(const ScopedTimerResolution&) = delete;
  ScopedTimerResolution& operator=(const ScopedTimerResolution&) = delete;

 private:
  bool active_ = false;
};

// Resolves "host:port", "[v6]:port" or "v4:port". Prefers an IPv4 result.
bool ResolveHostPort(const std::string& host_port, Addr* out, std::string* error);

// Local unicast addresses (port 0): IPv4 excluding loopback and link-local; IPv6 global unicast
// only (no loopback, link-local, ULA or multicast).
std::vector<Addr> LocalAddresses(bool want_v4, bool want_v6);

}  // namespace ac6net::net
