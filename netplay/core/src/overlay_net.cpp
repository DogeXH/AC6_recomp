// ac6net - UDP socket layer (see overlay_net.h).
#include "overlay_net.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include "ac6net/log.h"

#if defined(_WIN32)
#include <iphlpapi.h>
#include <mstcpip.h>
#include <mswsock.h>  // SIO_UDP_CONNRESET / SIO_UDP_NETRESET
#include <mmsystem.h>  // timeBeginPeriod (winmm)
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#ifndef SIO_UDP_NETRESET
#define SIO_UDP_NETRESET _WSAIOW(IOC_VENDOR, 15)
#endif
#ifndef WSA_FLAG_NO_HANDLE_INHERIT
#define WSA_FLAG_NO_HANDLE_INHERIT 0x80
#endif
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace ac6net::net {

namespace {

#if defined(_WIN32)
int LastError() { return WSAGetLastError(); }
bool WouldBlock(int e) { return e == WSAEWOULDBLOCK; }
void CloseHandle_(SocketHandle s) { closesocket(s); }
#else
int LastError() { return errno; }
bool WouldBlock(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
void CloseHandle_(SocketHandle s) { ::close(s); }
#endif

std::string ErrorText(const char* what) {
  return std::string(what) + " failed (error " + std::to_string(LastError()) + ")";
}

bool IsUsableV6(const in6_addr& a) {
  const uint8_t* b = a.s6_addr;
  bool all_zero = true;
  for (int i = 0; i < 16; ++i)
    if (b[i]) all_zero = false;
  if (all_zero) return false;                                // ::
  if (b[0] == 0xFF) return false;                            // multicast
  if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return false;   // fe80::/10 link-local
  if (b[0] == 0xFE && (b[1] & 0xC0) == 0xC0) return false;   // fec0::/10 site-local
  if ((b[0] & 0xFE) == 0xFC) return false;                   // fc00::/7 ULA
  bool loopback = b[15] == 1;
  for (int i = 0; i < 15; ++i)
    if (b[i]) loopback = false;
  if (loopback) return false;                                // ::1
  static const uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
  if (std::memcmp(b, kMapped, 12) == 0) return false;        // v4-mapped
  return true;
}

bool IsUsableV4(uint32_t ip_host) {
  if (ip_host == 0) return false;
  if ((ip_host >> 24) == 127) return false;              // loopback
  if ((ip_host & 0xFFFF0000u) == 0xA9FE0000u) return false;  // 169.254/16
  if ((ip_host >> 28) >= 0xE) return false;              // multicast / reserved
  return true;
}

}  // namespace

bool Addr::IsLoopback() const {
  if (IsV4()) {
    const auto* sin = reinterpret_cast<const sockaddr_in*>(&ss);
    return (ntohl(sin->sin_addr.s_addr) >> 24) == 127;
  }
  if (IsV6()) {
    const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(&ss);
    static const uint8_t kLoop[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    return std::memcmp(sin6->sin6_addr.s6_addr, kLoop, 16) == 0;
  }
  return false;
}

uint16_t Addr::Port() const {
  if (IsV4()) return ntohs(reinterpret_cast<const sockaddr_in*>(&ss)->sin_port);
  if (IsV6()) return ntohs(reinterpret_cast<const sockaddr_in6*>(&ss)->sin6_port);
  return 0;
}

std::string Addr::Ip() const {
  char buf[INET6_ADDRSTRLEN] = {};
  if (IsV4()) {
    inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(&ss)->sin_addr, buf, sizeof(buf));
  } else if (IsV6()) {
    inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(&ss)->sin6_addr, buf, sizeof(buf));
  }
  return buf;
}

std::string Addr::ToString() const {
  if (IsV6()) return "[" + Ip() + "]:" + std::to_string(Port());
  if (IsV4()) return Ip() + ":" + std::to_string(Port());
  return "-";
}

bool Addr::SameIp(const Addr& o) const {
  if (!valid() || !o.valid() || family() != o.family()) return false;
  if (IsV4()) {
    return reinterpret_cast<const sockaddr_in*>(&ss)->sin_addr.s_addr ==
           reinterpret_cast<const sockaddr_in*>(&o.ss)->sin_addr.s_addr;
  }
  return std::memcmp(&reinterpret_cast<const sockaddr_in6*>(&ss)->sin6_addr,
                     &reinterpret_cast<const sockaddr_in6*>(&o.ss)->sin6_addr, 16) == 0;
}

bool Addr::operator==(const Addr& o) const { return SameIp(o) && Port() == o.Port(); }

bool Addr::FromIpPort(const std::string& ip, uint16_t port, Addr* out) {
  Addr a;
  sockaddr_in sin{};
  if (inet_pton(AF_INET, ip.c_str(), &sin.sin_addr) == 1) {
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
#if defined(__APPLE__)
    sin.sin_len = sizeof(sin);
#endif
    std::memcpy(&a.ss, &sin, sizeof(sin));
    a.len = sizeof(sin);
    *out = a;
    return true;
  }
  sockaddr_in6 sin6{};
  if (inet_pton(AF_INET6, ip.c_str(), &sin6.sin6_addr) == 1) {
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = htons(port);
#if defined(__APPLE__)
    sin6.sin6_len = sizeof(sin6);
#endif
    std::memcpy(&a.ss, &sin6, sizeof(sin6));
    a.len = sizeof(sin6);
    *out = a;
    return true;
  }
  return false;
}

bool NetInit() {
#if defined(_WIN32)
  static std::once_flag once;
  static bool ok = false;
  std::call_once(once, [] {
    WSADATA data;
    ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  });
  return ok;
#else
  return true;
#endif
}

bool UdpSocket::Open(int family, uint16_t port, std::string* error) {
  Close();
  if (!NetInit()) {
    if (error) *error = "socket library initialisation failed";
    return false;
  }
#if defined(_WIN32)
  // Not inheritable (the POSIX branch sets FD_CLOEXEC): a child process started with handle
  // inheritance would otherwise keep our port bound after the game exits. Overlapped like
  // ::socket(). Windows 7 before SP1 rejects the flag; fall back to an inheritable socket.
  SocketHandle s = WSASocketW(family, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0,
                              WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  if (s == kInvalidSocket) s = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
#else
  SocketHandle s = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
#endif
  if (s == kInvalidSocket) {
    if (error) *error = ErrorText("socket");
    return false;
  }
#if defined(_WIN32)
  // Without SO_EXCLUSIVEADDRUSE another program may bind the same port on a specific local
  // address (or with SO_REUSEADDR) and receive our datagrams. Must precede bind().
  {
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
               sizeof(exclusive));
  }
#endif
  if (family == AF_INET6) {
    // Separate IPv4 and IPv6 sockets, so v6-only on every platform (the Windows default is
    // already TRUE, Linux's is FALSE).
    int on = 1;
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&on), sizeof(on));
  }
  // Generous buffers: bursts of game traffic arrive while the network thread is busy.
  int buf = 1 << 20;
  setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf), sizeof(buf));
  setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buf), sizeof(buf));

  sockaddr_storage ss{};
  socklen_t len = 0;
  if (family == AF_INET) {
    auto* sin = reinterpret_cast<sockaddr_in*>(&ss);
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = htonl(INADDR_ANY);
    sin->sin_port = htons(port);
    len = sizeof(sockaddr_in);
  } else {
    auto* sin6 = reinterpret_cast<sockaddr_in6*>(&ss);
    sin6->sin6_family = AF_INET6;
    sin6->sin6_addr = in6addr_any;
    sin6->sin6_port = htons(port);
    len = sizeof(sockaddr_in6);
  }
  if (::bind(s, reinterpret_cast<sockaddr*>(&ss), len) != 0) {
    if (error) {
      *error = ErrorText("bind");
#if defined(_WIN32)
      // The two usual Windows causes, spelled out for players reading the log.
      const int e = LastError();
      if (e == WSAEADDRINUSE) {
        *error += ": the port is in use by another program or game instance (netplay_port = 0 "
                  "picks a free one)";
      } else if (e == WSAEACCES) {
        *error += ": the port is reserved by Windows (Hyper-V/WSL/Docker excluded port range; "
                  "see 'netsh interface ipv4 show excludedportrange protocol=udp') or held "
                  "exclusively by another program; choose another netplay_port or 0";
      }
#endif
    }
    CloseHandle_(s);
    return false;
  }
#if defined(_WIN32)
  u_long nb = 1;
  if (ioctlsocket(s, FIONBIO, &nb) != 0) {
    // A blocking socket would stall the network thread in recvfrom forever.
    if (error) *error = ErrorText("ioctlsocket(FIONBIO)");
    CloseHandle_(s);
    return false;
  }
  // Do not let ICMP port-unreachable (SIO_UDP_CONNRESET) or TTL-expired (SIO_UDP_NETRESET)
  // surface as WSAECONNRESET / WSAENETRESET on this unconnected socket: a peer that went away
  // must not make the next recvfrom fail. RecvFrom still treats both as skippable in case the
  // ioctl is unsupported (older Wine, layered service providers).
  BOOL report = FALSE;
  DWORD bytes = 0;
  if (WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &bytes, nullptr,
               nullptr) != 0) {
    LogF(LogLevel::kDebug, "net: SIO_UDP_CONNRESET not supported (error %d)", LastError());
  }
  WSAIoctl(s, SIO_UDP_NETRESET, &report, sizeof(report), nullptr, 0, &bytes, nullptr, nullptr);
#else
  const int flags = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, flags | O_NONBLOCK);
  fcntl(s, F_SETFD, FD_CLOEXEC);
#if defined(SO_NOSIGPIPE)
  int on = 1;
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
#endif
  sockaddr_storage bound{};
  socklen_t bound_len = sizeof(bound);
  if (getsockname(s, reinterpret_cast<sockaddr*>(&bound), &bound_len) != 0) {
    if (error) *error = ErrorText("getsockname");
    CloseHandle_(s);
    return false;
  }
  port_ = family == AF_INET ? ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port)
                            : ntohs(reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port);
  fd_ = s;
  family_ = family;
  return true;
}

void UdpSocket::Close() {
  if (fd_ != kInvalidSocket) CloseHandle_(fd_);
  fd_ = kInvalidSocket;
  port_ = 0;
  family_ = 0;
}

int UdpSocket::RecvFrom(uint8_t* buf, size_t cap, Addr* from) {
  if (!valid()) return 0;
  from->len = sizeof(from->ss);
#if defined(_WIN32)
  const int n = ::recvfrom(fd_, reinterpret_cast<char*>(buf), static_cast<int>(cap), 0,
                           reinterpret_cast<sockaddr*>(&from->ss), &from->len);
  if (n < 0) {
    const int e = LastError();
    if (WouldBlock(e)) return 0;
    // Windows reports a datagram longer than `cap` as WSAEMSGSIZE with the first `cap` bytes
    // filled in (POSIX silently truncates); hand the truncated bytes on like POSIX does.
    // WSAECONNRESET / WSAENETRESET (ICMP for an earlier send), WSAEINTR and anything else:
    // skip this one and keep draining.
    return e == WSAEMSGSIZE ? static_cast<int>(cap) : -1;
  }
#else
  const ssize_t n = ::recvfrom(fd_, buf, cap, 0, reinterpret_cast<sockaddr*>(&from->ss),
                               &from->len);
  if (n < 0) return WouldBlock(LastError()) ? 0 : -1;
#endif
  if (n == 0) return -1;  // an empty datagram is never valid; report it as skippable
  return static_cast<int>(n);
}

bool UdpSocket::SendTo(const Addr& to, const uint8_t* data, size_t len) {
  if (!valid() || !to.valid() || to.family() != family_) return false;
#if defined(_WIN32)
  const int n = ::sendto(fd_, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
                         reinterpret_cast<const sockaddr*>(&to.ss), to.len);
#else
  int flags = 0;
#if defined(MSG_NOSIGNAL)
  flags |= MSG_NOSIGNAL;
#endif
  const ssize_t n = ::sendto(fd_, data, len, flags, reinterpret_cast<const sockaddr*>(&to.ss),
                             to.len);
#endif
  return n == static_cast<decltype(n)>(len);
}

bool PollReadable(UdpSocket* const* socks, size_t n, int timeout_ms, bool* readable) {
#if defined(_WIN32)
  WSAPOLLFD fds[4];
#else
  pollfd fds[4];
#endif
  size_t map[4];
  size_t count = 0;
  for (size_t i = 0; i < n && count < 4; ++i) {
    readable[i] = false;
    if (!socks[i] || !socks[i]->valid()) continue;
    fds[count].fd = socks[i]->handle();
    fds[count].events = POLLIN;
    fds[count].revents = 0;
    map[count++] = i;
  }
  if (count == 0) return false;
#if defined(_WIN32)
  const int r = WSAPoll(fds, static_cast<ULONG>(count), timeout_ms);
  if (r < 0) {
    // WSAPoll fails at once (no EINTR-style retry semantics); wait out the timeout so a
    // persistent error cannot turn the network loop into a busy spin.
    if (timeout_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
    return false;
  }
#else
  const int r = ::poll(fds, static_cast<nfds_t>(count), timeout_ms);
#endif
  if (r <= 0) return false;
  bool any = false;
  for (size_t k = 0; k < count; ++k) {
    if (fds[k].revents & (POLLIN | POLLERR | POLLHUP)) {
      readable[map[k]] = true;
      any = true;
    }
  }
  return any;
}

bool ResolveHostPort(const std::string& host_port, Addr* out, std::string* error) {
  if (!NetInit()) {
    if (error) *error = "socket library initialisation failed";
    return false;
  }
  std::string host, port;
  if (!host_port.empty() && host_port[0] == '[') {
    const size_t close = host_port.find(']');
    if (close == std::string::npos || close + 1 >= host_port.size() ||
        host_port[close + 1] != ':') {
      if (error) *error = "bad server address: " + host_port;
      return false;
    }
    host = host_port.substr(1, close - 1);
    port = host_port.substr(close + 2);
  } else {
    const size_t colon = host_port.rfind(':');
    if (colon == std::string::npos || host_port.find(':') != colon) {
      if (error) *error = "bad server address (want host:port or [v6]:port): " + host_port;
      return false;
    }
    host = host_port.substr(0, colon);
    port = host_port.substr(colon + 1);
  }
  if (host.empty() || port.empty() || port.size() > 5 ||
      port.find_first_not_of("0123456789") != std::string::npos || std::stoul(port) == 0 ||
      std::stoul(port) > 65535) {
    if (error) *error = "bad server address: " + host_port;
    return false;
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  addrinfo* res = nullptr;
  const int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
  if (rc != 0 || !res) {
    if (error) *error = "cannot resolve " + host + " (getaddrinfo " + std::to_string(rc) + ")";
    return false;
  }
  const addrinfo* pick = nullptr;
  for (const addrinfo* ai = res; ai; ai = ai->ai_next) {
    if (ai->ai_family == AF_INET) {
      pick = ai;
      break;
    }
    if (ai->ai_family == AF_INET6 && !pick) pick = ai;
  }
  bool ok = false;
  if (pick && pick->ai_addrlen <= sizeof(out->ss)) {
    std::memset(&out->ss, 0, sizeof(out->ss));
    std::memcpy(&out->ss, pick->ai_addr, pick->ai_addrlen);
    out->len = static_cast<socklen_t>(pick->ai_addrlen);
    ok = true;
  }
  freeaddrinfo(res);
  if (!ok && error) *error = "no usable address for " + host;
  return ok;
}

std::vector<Addr> LocalAddresses(bool want_v4, bool want_v6) {
  std::vector<Addr> out;
  auto add_unique = [&out](const Addr& a) {
    for (const auto& e : out)
      if (e.SameIp(a)) return;
    out.push_back(a);
  };
#if defined(_WIN32)
  // Windows lists IPv6 addresses before IPv4 per adapter, keeps deprecated temporary IPv6
  // addresses for days, and adds virtual adapters (Hyper-V/WSL, VPN, Teredo). The overlay
  // offers only a handful of candidates, so order them by usefulness: IPv4 on adapters with a
  // default gateway (the real LAN), other IPv4, stable IPv6, temporary IPv6. Tunnel adapters and
  // addresses that are not in the "preferred" DAD state (tentative, duplicate, deprecated) are
  // skipped.
  ULONG size = 16 * 1024;
  std::vector<uint8_t> buf(size);
  ULONG rc = ERROR_BUFFER_OVERFLOW;
  for (int tries = 0; tries < 3 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
    buf.resize(size);
    rc = GetAdaptersAddresses(AF_UNSPEC,
                              GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                  GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME |
                                  GAA_FLAG_INCLUDE_GATEWAYS,
                              nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
  }
  if (rc != NO_ERROR) return out;
  std::vector<Addr> tiers[4];  // v4 + gateway, v4, v6 stable, v6 temporary
  for (auto* ad = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); ad; ad = ad->Next) {
    if (ad->OperStatus != IfOperStatusUp) continue;
    if (ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK || ad->IfType == IF_TYPE_TUNNEL) continue;
    const bool has_gateway = ad->FirstGatewayAddress != nullptr;
    for (auto* ua = ad->FirstUnicastAddress; ua; ua = ua->Next) {
      if (ua->DadState != IpDadStatePreferred) continue;
      const sockaddr* sa = ua->Address.lpSockaddr;
      if (!sa) continue;
      Addr a;
      if (sa->sa_family == AF_INET && want_v4) {
        const auto* sin = reinterpret_cast<const sockaddr_in*>(sa);
        if (!IsUsableV4(ntohl(sin->sin_addr.s_addr))) continue;
        sockaddr_in clean{};
        clean.sin_family = AF_INET;
        clean.sin_addr = sin->sin_addr;
        std::memcpy(&a.ss, &clean, sizeof(clean));
        a.len = sizeof(clean);
        tiers[has_gateway ? 0 : 1].push_back(a);
      } else if (sa->sa_family == AF_INET6 && want_v6) {
        const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(sa);
        if (!IsUsableV6(sin6->sin6_addr)) continue;
        sockaddr_in6 clean{};
        clean.sin6_family = AF_INET6;
        clean.sin6_addr = sin6->sin6_addr;
        std::memcpy(&a.ss, &clean, sizeof(clean));
        a.len = sizeof(clean);
        tiers[ua->SuffixOrigin == IpSuffixOriginRandom ? 3 : 2].push_back(a);
      }
    }
  }
  for (const auto& tier : tiers)
    for (const Addr& a : tier) add_unique(a);
#else
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return out;
  for (ifaddrs* ifa = list; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || !(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
    Addr a;
    if (ifa->ifa_addr->sa_family == AF_INET && want_v4) {
      const auto* sin = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);
      if (!IsUsableV4(ntohl(sin->sin_addr.s_addr))) continue;
      sockaddr_in clean{};
      clean.sin_family = AF_INET;
      clean.sin_addr = sin->sin_addr;
      std::memcpy(&a.ss, &clean, sizeof(clean));
      a.len = sizeof(clean);
      add_unique(a);
    } else if (ifa->ifa_addr->sa_family == AF_INET6 && want_v6) {
      const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(ifa->ifa_addr);
      if (!IsUsableV6(sin6->sin6_addr)) continue;
      sockaddr_in6 clean{};
      clean.sin6_family = AF_INET6;
      clean.sin6_addr = sin6->sin6_addr;
      std::memcpy(&a.ss, &clean, sizeof(clean));
      a.len = sizeof(clean);
      add_unique(a);
    }
  }
  freeifaddrs(list);
#endif
  return out;
}

#if defined(_WIN32)
namespace {
// Windows 11 stops honouring timeBeginPeriod for a process whose windows are all minimised or
// occluded, which would put the network loop back on the 15.6 ms tick while a host is
// alt-tabbed. Opt out of that while the loop runs; restore the system default afterwards.
void SetIgnoreTimerResolutionThrottling(bool keep_resolution) {
#if defined(PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION)
  PROCESS_POWER_THROTTLING_STATE state{};
  state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  state.ControlMask = keep_resolution ? PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION : 0;
  state.StateMask = 0;
  // Fails harmlessly before Windows 11 and on Wine.
  SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
#else
  (void)keep_resolution;
#endif
}
std::mutex g_throttle_mu;
int g_throttle_users = 0;
}  // namespace

// The throttling opt-out is process-wide and several overlays (tests) may run at once.
ScopedTimerResolution::ScopedTimerResolution() : active_(timeBeginPeriod(1) == TIMERR_NOERROR) {
  if (!active_) return;
  std::lock_guard<std::mutex> lock(g_throttle_mu);
  if (g_throttle_users++ == 0) SetIgnoreTimerResolutionThrottling(true);
}
ScopedTimerResolution::~ScopedTimerResolution() {
  if (!active_) return;
  {
    std::lock_guard<std::mutex> lock(g_throttle_mu);
    if (--g_throttle_users == 0) SetIgnoreTimerResolutionThrottling(false);
  }
  timeEndPeriod(1);
}
#else
ScopedTimerResolution::ScopedTimerResolution() = default;
ScopedTimerResolution::~ScopedTimerResolution() = default;
#endif

}  // namespace ac6net::net
