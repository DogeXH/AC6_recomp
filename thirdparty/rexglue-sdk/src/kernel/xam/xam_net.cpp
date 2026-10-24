/**
 * ReXGlue runtime - AC6 Recompilation project
 * Copyright (c) 2026 Tom Clay. All rights reserved.
 * @modified    2026 - AC6 netplay: NetDll sockets routed to ac6net::vnet and XNet/QoS to
 *              ac6net::Netplay when netplay is enabled (REX_HAVE_AC6NET). Parts of the QoS,
 *              select and XNet behaviour follow Xenia Canary Netplay's xam_net.cc (BSD-3).
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#ifndef REX_HAVE_AC6NET
#define REX_HAVE_AC6NET 0
#endif

#include <algorithm>
#include <map>
#include <cstring>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/ppc/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xsocket.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

#if REX_PLATFORM_WIN32
// NOTE: must be included last as it expects windows.h to already be included.
#define _WINSOCK_DEPRECATED_NO_WARNINGS  // inet_addr
#include <winsock2.h>                    // NOLINT(build/include_order)
#elif REX_PLATFORM_LINUX
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

#if REX_HAVE_AC6NET
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <ac6net/log.h>
#include <ac6net/netplay.h>
#include <ac6net/vnet.h>

#include "netplay_net_marshal.h"
#include "netplay_net_qos.h"
#endif

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

// https://github.com/G91/TitanOffLine/blob/1e692d9bb9dfac386d08045ccdadf4ae3227bb5e/xkelib/xam/xamNet.h
enum {
  XNCALLER_INVALID = 0x0,
  XNCALLER_TITLE = 0x1,
  XNCALLER_SYSAPP = 0x2,
  XNCALLER_XBDM = 0x3,
  XNCALLER_TEST = 0x4,
  NUM_XNCALLER_TYPES = 0x4,
};

// https://github.com/pmrowla/hl2sdk-csgo/blob/master/common/xbox/xboxstubs.h
typedef struct {
  // FYI: IN_ADDR should be in network-byte order.
  in_addr ina;                    // IP address (zero if not static/DHCP)
  in_addr inaOnline;              // Online IP address (zero if not online)
  rex::be<uint16_t> wPortOnline;  // Online port
  uint8_t abEnet[6];              // Ethernet MAC address
  uint8_t abOnline[20];           // Online identification
} XNADDR;

typedef struct {
  rex::be<int32_t> status;
  rex::be<uint32_t> cina;
  in_addr aina[8];
} XNDNS;

typedef struct {
  uint8_t flags;
  uint8_t reserved;
  rex::be<uint16_t> probes_xmit;
  rex::be<uint16_t> probes_recv;
  rex::be<uint16_t> data_len;
  rex::be<uint32_t> data_ptr;
  rex::be<uint16_t> rtt_min_in_msecs;
  rex::be<uint16_t> rtt_med_in_msecs;
  rex::be<uint32_t> up_bits_per_sec;
  rex::be<uint32_t> down_bits_per_sec;
} XNQOSINFO;

typedef struct {
  rex::be<uint32_t> count;
  rex::be<uint32_t> count_pending;
  XNQOSINFO info[1];
} XNQOS;

struct Xsockaddr_t {
  rex::be<uint16_t> sa_family;
  char sa_data[14];
};

struct X_WSADATA {
  rex::be<uint16_t> version;
  rex::be<uint16_t> version_high;
  char description[256 + 1];
  char system_status[128 + 1];
  rex::be<uint16_t> max_sockets;
  rex::be<uint16_t> max_udpdg;
  rex::be<uint32_t> vendor_info_ptr;
};

struct XWSABUF {
  rex::be<uint32_t> len;
  rex::be<uint32_t> buf_ptr;
};

struct XWSAOVERLAPPED {
  rex::be<uint32_t> internal;
  rex::be<uint32_t> internal_high;
  union {
    struct {
      rex::be<uint32_t> low;
      rex::be<uint32_t> high;
    } offset;  // must be named to avoid GCC error
    rex::be<uint32_t> pointer;
  };
  rex::be<uint32_t> event_handle;
};

void LoadSockaddr(const uint8_t* ptr, sockaddr* out_addr) {
  out_addr->sa_family = memory::load_and_swap<uint16_t>(ptr + 0);
  switch (out_addr->sa_family) {
    case AF_INET: {
      auto in_addr = reinterpret_cast<sockaddr_in*>(out_addr);
      in_addr->sin_port = memory::load_and_swap<uint16_t>(ptr + 2);
      // Maybe? Depends on type.
      in_addr->sin_addr.s_addr = *(uint32_t*)(ptr + 4);
      break;
    }
    default:
      assert_unhandled_case(out_addr->sa_family);
      break;
  }
}

void StoreSockaddr(const sockaddr& addr, uint8_t* ptr) {
  switch (addr.sa_family) {
    case AF_UNSPEC:
      std::memset(ptr, 0, sizeof(addr));
      break;
    case AF_INET: {
      auto& in_addr = reinterpret_cast<const sockaddr_in&>(addr);
      memory::store_and_swap<uint16_t>(ptr + 0, in_addr.sin_family);
      memory::store_and_swap<uint16_t>(ptr + 2, in_addr.sin_port);
      // Maybe? Depends on type.
      memory::store_and_swap<uint32_t>(ptr + 4, in_addr.sin_addr.s_addr);
      break;
    }
    default:
      assert_unhandled_case(addr.sa_family);
      break;
  }
}

// https://github.com/joolswills/mameox/blob/master/MAMEoX/Sources/xbox_Network.cpp#L136
struct XNetStartupParams {
  uint8_t cfgSizeOfStruct;
  uint8_t cfgFlags;
  uint8_t cfgSockMaxDgramSockets;
  uint8_t cfgSockMaxStreamSockets;
  uint8_t cfgSockDefaultRecvBufsizeInK;
  uint8_t cfgSockDefaultSendBufsizeInK;
  uint8_t cfgKeyRegMax;
  uint8_t cfgSecRegMax;
  uint8_t cfgQosDataLimitDiv4;
  uint8_t cfgQosProbeTimeoutInSeconds;
  uint8_t cfgQosProbeRetries;
  uint8_t cfgQosSrvMaxSimultaneousResponses;
  uint8_t cfgQosPairWaitTimeInSeconds;
};

XNetStartupParams xnet_startup_params = {0};

#if REX_HAVE_AC6NET
//=============================================================================
// AC6 netplay glue (ARCHITECTURE.md 5.4)
//=============================================================================
// With ac6net::Netplay enabled, every guest socket is a virtual ac6net::vnet socket with the
// guest handle 0x1000 + id, and the XNet address/QoS calls go to ac6net::Netplay. With netplay
// disabled, none of this runs and the XSocket path below behaves exactly as before.
namespace {

namespace np = netplay_net;
namespace wsa = ac6net::wsa;
using ac6net::Netplay;
using ac6net::vnet::Endpoint;
using ac6net::vnet::SocketId;

bool NetplayOn() {
  return Netplay::Get().enabled();
}

ac6net::vnet::VNet& Vnet() {
  return Netplay::Get().vnet();
}

// Handles in the vnet range go to vnet only while netplay is on; otherwise they take the
// XSocket path, which reports WSAENOTSOCK for them as it always did.
bool IsVnetSocket(uint32_t handle) {
  return np::IsVnetHandle(handle) && NetplayOn();
}

// Winsock-style failure: the WSA code goes to the calling guest thread's last error, which
// NetDll_WSAGetLastError returns.
int WsaFail(int err) {
  if (!err) err = wsa::kInval;
  // Trace every failure the guest sees except the routine WOULDBLOCK (at most 30 per code).
  if (err != wsa::kWouldBlock && ac6net::TraceEnabled()) {
    static std::mutex mu;
    static std::map<int, int> counts;
    std::lock_guard<std::mutex> lk(mu);
    if (counts[err]++ < 30) {
      ac6net::Trace("net.err", "\"err\":" + std::to_string(err));
    }
  }
  XThread::SetLastError(static_cast<uint32_t>(err));
  return -1;
}

int WsaResult(int ret, int err) {
  return ret < 0 ? WsaFail(err) : ret;
}

uint8_t* GuestBytes(uint32_t guest_addr) {
  return REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(guest_addr);
}

template <typename T>
uint8_t* HostBytes(const ppc_ptr_t<T>& p) {
  return reinterpret_cast<uint8_t*>(p.host_address());
}

template <typename T>
bool GuestOk(const ppc_ptr_t<T>& p, uint64_t len) {
  return p && np::GuestRangeOk(p.guest_address(), len);
}

// Reads a guest socket address argument (bind, connect, sendto). Returns a WSA code.
template <typename T>
int ReadSockaddrArg(const ppc_ptr_t<T>& p, uint32_t len, Endpoint* out) {
  if (!GuestOk(p, np::kSockaddrInSize))
    return wsa::kFault;
  return np::ParseSockaddrIn(HostBytes(p), len, out);
}

// Validates an output socket address (accept, getsockname, getpeername, recvfrom) before the
// operation runs, so a bad pointer never loses a connection or a datagram.
template <typename T>
int CheckSockaddrOutArg(const ppc_ptr_t<T>& addr, const ppc_pu32_t& len) {
  if (len && !GuestOk(len, 4))
    return wsa::kFault;
  const uint32_t cap = len ? static_cast<uint32_t>(*len) : 0;
  int err = np::CheckSockaddrOut(static_cast<bool>(addr), static_cast<bool>(len), cap);
  if (err == wsa::kOk && addr && !GuestOk(addr, np::kSockaddrInSize))
    err = wsa::kFault;
  return err;
}

template <typename T>
void WriteSockaddrOut(const ppc_ptr_t<T>& addr, const ppc_pu32_t& len, const Endpoint& ep) {
  if (!addr)
    return;
  np::StoreSockaddrIn(HostBytes(addr), ep);
  *len = np::kSockaddrInSize;
}

// Wraps a freshly created vnet socket id as a guest handle.
u32 PublishSocket(SocketId id) {
  const uint32_t handle = np::HandleFromId(id);
  if (!handle) {
    int ignored = wsa::kOk;
    Vnet().Close(id, &ignored);
    return static_cast<u32>(WsaFail(wsa::kNoBufs));
  }
  return handle;
}

// Return addresses of the calling guest code as a JSON array of hex strings: ctx.lr, then one
// per frame of the r1 back chain (a frame's caller saved its LR at back_chain - 8). Diagnostics
// for the traces only; stops at the first implausible frame. The walk stays inside the calling
// thread's own guest stack (PCR stack_end..stack_base): the outermost frame's back chain points
// at stack_base, and the page there is the stack's no-access guard page, so reading it faults.
// (Windows build, 2026-10-23: the Main XThread died with an access violation at guest
// 0x70210000 when the game closed a socket at mission start / session delete.)
std::string GuestStackJson(int max_frames = 14) {
  std::string out = "[";
  if (!rex::runtime::ThreadState::Get())
    return out + "]";
  const PPCContext* ctx = rex::runtime::current_ppc_context();
  char buf[16];
  auto add = [&](uint32_t a) {
    if (out.size() > 1)
      out += ',';
    std::snprintf(buf, sizeof(buf), "\"%08X\"", a);
    out += buf;
  };
  add(static_cast<uint32_t>(ctx->lr));
  uint32_t stack_lo = 0, stack_hi = 0;
  if (XThread* thread = XThread::GetCurrentThread(); thread && thread->pcr_ptr() &&
                                                     np::GuestRangeOk(thread->pcr_ptr(),
                                                                      sizeof(X_KPCR))) {
    const auto* pcr = reinterpret_cast<const X_KPCR*>(GuestBytes(thread->pcr_ptr()));
    stack_hi = pcr->stack_base_ptr;
    stack_lo = pcr->stack_end_ptr;
  }
  if (stack_hi <= stack_lo)
    return out + "]";
  // [a, a + n) inside the thread's stack.
  auto in_stack = [&](uint32_t a, uint32_t n) {
    return a >= stack_lo && a <= stack_hi && n <= stack_hi - a;
  };
  uint32_t sp = ctx->r1.u32;
  for (int i = 0; i < max_frames; ++i) {
    if (!in_stack(sp, 4))
      break;
    const uint32_t back = memory::load_and_swap<uint32_t>(GuestBytes(sp));
    if (back <= sp || back - sp > 0x100000 || back < 8 || !in_stack(back - 8, 4))
      break;
    add(memory::load_and_swap<uint32_t>(GuestBytes(back - 8)));
    sp = back;
  }
  return out + "]";
}

void TraceEvent(const char* event, const std::string& fields) {
  if (ac6net::TraceEnabled())
    ac6net::Trace(event, fields);
}

std::string IpField(const char* name, uint32_t ip) {
  return std::string("\"") + name + "\":\"" + ac6net::Ipv4ToString(ip) + "\"";
}

// --- sockets ---------------------------------------------------------------

u32 VnetSocket(u32 af, u32 type, u32 protocol) {
  if (af != np::kAfInet)
    return static_cast<u32>(WsaFail(wsa::kAfNoSupport));
  int err = wsa::kOk;
  const SocketId id = Vnet().Socket(static_cast<int>(type), static_cast<int>(protocol), &err);
  if (id <= 0)
    return static_cast<u32>(WsaFail(err ? err : wsa::kNoBufs));
  return PublishSocket(id);
}

int VnetClose(u32 handle) {
  int err = wsa::kOk;
  return WsaResult(Vnet().Close(np::IdFromHandle(handle), &err), err);
}

int VnetShutdown(u32 handle, i32 how) {
  int err = wsa::kOk;
  return WsaResult(Vnet().Shutdown(np::IdFromHandle(handle), how, &err), err);
}

int VnetSetOption(u32 handle, u32 level, u32 option, const ppc_pvoid_t& value, u32 len) {
  const int lvl = static_cast<int>(level);
  const int opt = static_cast<int>(option);
  if (np::IsIgnoredXboxOption(lvl, opt)) {
    // There is no XNet security layer to opt out of; vnet traffic is protected by the overlay.
    return Vnet().IsSocket(np::IdFromHandle(handle)) ? 0 : WsaFail(wsa::kNotSock);
  }
  if (len > np::kMaxOptionBytes)
    return WsaFail(wsa::kInval);
  if (len && !GuestOk(value, len))
    return WsaFail(wsa::kFault);
  uint8_t host[np::kMaxOptionBytes] = {};
  if (len)
    np::SwapOptionValue(lvl, opt, HostBytes(value), host, len, /*guest_to_host=*/true);
  int err = wsa::kOk;
  return WsaResult(Vnet().SetOption(np::IdFromHandle(handle), lvl, opt, host, len, &err), err);
}

int VnetGetOption(u32 handle, u32 level, u32 option, const ppc_pvoid_t& value,
                  const ppc_pu32_t& len_ptr) {
  if (!GuestOk(len_ptr, 4))
    return WsaFail(wsa::kFault);
  const uint32_t cap = std::min<uint32_t>(*len_ptr, np::kMaxOptionBytes);
  if (!GuestOk(value, cap ? cap : 1))
    return WsaFail(wsa::kFault);
  const int lvl = static_cast<int>(level);
  const int opt = static_cast<int>(option);
  uint8_t host[np::kMaxOptionBytes] = {};
  size_t len = cap;
  int err = wsa::kOk;
  const int ret = Vnet().GetOption(np::IdFromHandle(handle), lvl, opt, host, &len, &err);
  if (ret < 0)
    return WsaFail(err);
  len = std::min<size_t>(len, cap);
  np::SwapOptionValue(lvl, opt, host, HostBytes(value), len, /*guest_to_host=*/false);
  *len_ptr = static_cast<uint32_t>(len);
  return 0;
}

int VnetIoctl(u32 handle, u32 cmd, const ppc_pvoid_t& arg) {
  if (!GuestOk(arg, 4))
    return WsaFail(wsa::kFault);
  const SocketId id = np::IdFromHandle(handle);
  int err = wsa::kOk;
  switch (cmd) {
    case np::kFionbio: {
      const bool non_blocking = np::LoadBe32(HostBytes(arg)) != 0;
      return WsaResult(Vnet().SetNonBlocking(id, non_blocking, &err), err);
    }
    case np::kFionread: {
      uint32_t available = 0;
      const int ret = Vnet().Available(id, &available, &err);
      if (ret < 0)
        return WsaFail(err);
      np::StoreBe32(HostBytes(arg), available);
      return 0;
    }
    default:
      REXKRNL_WARN("NetDll_ioctlsocket: unsupported command {:08X} on a netplay socket", cmd);
      return WsaFail(wsa::kInval);
  }
}

int VnetBind(u32 handle, const ppc_ptr_t<XSOCKADDR_IN>& name, u32 name_len) {
  Endpoint local;
  const int perr = ReadSockaddrArg(name, name_len, &local);
  if (perr != wsa::kOk)
    return WsaFail(perr);
  int err = wsa::kOk;
  return WsaResult(Vnet().Bind(np::IdFromHandle(handle), local, &err), err);
}

int VnetConnect(u32 handle, const ppc_ptr_t<XSOCKADDR>& name, u32 name_len) {
  Endpoint remote;
  const int perr = ReadSockaddrArg(name, name_len, &remote);
  if (perr != wsa::kOk)
    return WsaFail(perr);
  int err = wsa::kOk;
  return WsaResult(Vnet().Connect(np::IdFromHandle(handle), remote, &err), err);
}

int VnetListen(u32 handle, i32 backlog) {
  int err = wsa::kOk;
  return WsaResult(Vnet().Listen(np::IdFromHandle(handle), backlog, &err), err);
}

u32 VnetAccept(u32 handle, const ppc_ptr_t<XSOCKADDR>& addr, const ppc_pu32_t& addr_len) {
  const int cerr = CheckSockaddrOutArg(addr, addr_len);
  if (cerr != wsa::kOk)
    return static_cast<u32>(WsaFail(cerr));
  Endpoint remote;
  int err = wsa::kOk;
  const SocketId id = Vnet().Accept(np::IdFromHandle(handle), &remote, &err);
  if (id <= 0)
    return static_cast<u32>(WsaFail(err));
  const u32 new_handle = PublishSocket(id);
  if (new_handle != static_cast<u32>(-1))
    WriteSockaddrOut(addr, addr_len, remote);
  return new_handle;
}

int VnetRecv(u32 handle, const ppc_pvoid_t& buf, u32 len, u32 flags) {
  if (len && !GuestOk(buf, len))
    return WsaFail(wsa::kFault);
  int err = wsa::kOk;
  return WsaResult(
      Vnet().Recv(np::IdFromHandle(handle), buf.host_address(), len, static_cast<int>(flags), &err),
      err);
}

int VnetRecvFrom(u32 handle, const ppc_pvoid_t& buf, u32 len, u32 flags,
                 const ppc_ptr_t<XSOCKADDR_IN>& from, const ppc_pu32_t& from_len) {
  if (len && !GuestOk(buf, len))
    return WsaFail(wsa::kFault);
  const int cerr = CheckSockaddrOutArg(from, from_len);
  if (cerr != wsa::kOk)
    return WsaFail(cerr);
  Endpoint source;
  int err = wsa::kOk;
  const int ret = Vnet().RecvFrom(np::IdFromHandle(handle), buf.host_address(), len,
                                  static_cast<int>(flags), &source, &err);
  if (ret < 0)
    return WsaFail(err);
  WriteSockaddrOut(from, from_len, source);
  return ret;
}

int VnetSend(u32 handle, const void* data, u32 len, u32 flags) {
  int err = wsa::kOk;
  return WsaResult(Vnet().Send(np::IdFromHandle(handle), data, len, static_cast<int>(flags), &err),
                   err);
}

template <typename T>
int VnetSendTo(u32 handle, const void* data, u32 len, u32 flags, const ppc_ptr_t<T>& to,
               u32 to_len) {
  if (!to)
    return VnetSend(handle, data, len, flags);  // connected socket
  Endpoint remote;
  const int perr = ReadSockaddrArg(to, to_len, &remote);
  if (perr != wsa::kOk)
    return WsaFail(perr);
  int err = wsa::kOk;
  return WsaResult(
      Vnet().SendTo(np::IdFromHandle(handle), data, len, static_cast<int>(flags), remote, &err),
      err);
}

template <typename T>
int VnetGetName(u32 handle, const ppc_ptr_t<T>& addr, const ppc_pu32_t& addr_len, bool peer) {
  if (!addr || !addr_len)
    return WsaFail(wsa::kFault);
  const int cerr = CheckSockaddrOutArg(addr, addr_len);
  if (cerr != wsa::kOk)
    return WsaFail(cerr);
  Endpoint ep;
  int err = wsa::kOk;
  const SocketId id = np::IdFromHandle(handle);
  const int ret = peer ? Vnet().GetPeerName(id, &ep, &err) : Vnet().GetSockName(id, &ep, &err);
  if (ret < 0)
    return WsaFail(err);
  WriteSockaddrOut(addr, addr_len, ep);
  return 0;
}

// WSABUF arrays (WSASendTo / WSARecvFrom). Overlapped I/O is not supported on vnet sockets.
constexpr uint32_t kMaxWsaBuffers = 64;
constexpr uint64_t kMaxWsaBytes = 1u << 20;

struct GuestBuffer {
  uint8_t* data;
  uint32_t len;
};

int ReadWsaBuffers(const ppc_ptr_t<XWSABUF>& buffers, u32 count, std::vector<GuestBuffer>* out,
                   uint64_t* total) {
  out->clear();
  *total = 0;
  if (count > kMaxWsaBuffers)
    return wsa::kInval;
  if (count && !GuestOk(buffers, uint64_t{count} * sizeof(XWSABUF)))
    return wsa::kFault;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t len = buffers[i].len;
    const uint32_t ptr = buffers[i].buf_ptr;
    if (!np::GuestRangeOk(ptr, len))
      return wsa::kFault;
    *total += len;
    if (*total > kMaxWsaBytes)
      return wsa::kNoBufs;
    out->push_back({len ? GuestBytes(ptr) : nullptr, len});
  }
  return wsa::kOk;
}

u32 VnetWsaSendTo(u32 handle, const ppc_ptr_t<XWSABUF>& buffers, u32 count,
                  const ppc_pu32_t& bytes_sent, u32 flags, const ppc_ptr_t<XSOCKADDR_IN>& to,
                  u32 to_len, bool overlapped) {
  if (overlapped)
    return static_cast<u32>(WsaFail(wsa::kOpNotSupp));
  std::vector<GuestBuffer> parts;
  uint64_t total = 0;
  const int berr = ReadWsaBuffers(buffers, count, &parts, &total);
  if (berr != wsa::kOk)
    return static_cast<u32>(WsaFail(berr));
  std::vector<uint8_t> combined;
  combined.reserve(static_cast<size_t>(total));
  for (const GuestBuffer& part : parts) {
    combined.insert(combined.end(), part.data, part.data + part.len);
  }
  const int ret =
      VnetSendTo(handle, combined.data(), static_cast<u32>(combined.size()), flags, to, to_len);
  if (ret < 0)
    return static_cast<u32>(-1);
  if (GuestOk(bytes_sent, 4))
    *bytes_sent = static_cast<uint32_t>(ret);
  return 0;
}

u32 VnetWsaRecvFrom(u32 handle, const ppc_ptr_t<XWSABUF>& buffers, u32 count,
                    const ppc_pu32_t& bytes_recv, const ppc_pu32_t& flags_ptr,
                    const ppc_ptr_t<XSOCKADDR_IN>& from, const ppc_pu32_t& from_len,
                    bool overlapped) {
  if (overlapped)
    return static_cast<u32>(WsaFail(wsa::kOpNotSupp));
  std::vector<GuestBuffer> parts;
  uint64_t total = 0;
  const int berr = ReadWsaBuffers(buffers, count, &parts, &total);
  if (berr != wsa::kOk)
    return static_cast<u32>(WsaFail(berr));
  if (flags_ptr && !GuestOk(flags_ptr, 4))
    return static_cast<u32>(WsaFail(wsa::kFault));
  const int cerr = CheckSockaddrOutArg(from, from_len);
  if (cerr != wsa::kOk)
    return static_cast<u32>(WsaFail(cerr));
  const int flags = flags_ptr ? static_cast<int>(static_cast<uint32_t>(*flags_ptr)) : 0;
  std::vector<uint8_t> staging(static_cast<size_t>(total));
  Endpoint source;
  int err = wsa::kOk;
  const int ret = Vnet().RecvFrom(np::IdFromHandle(handle), staging.data(), staging.size(), flags,
                                  &source, &err);
  if (ret < 0)
    return static_cast<u32>(WsaFail(err));
  size_t offset = 0;
  for (const GuestBuffer& part : parts) {
    if (offset >= static_cast<size_t>(ret))
      break;
    const size_t n = std::min<size_t>(part.len, static_cast<size_t>(ret) - offset);
    std::memcpy(part.data, staging.data() + offset, n);
    offset += n;
  }
  WriteSockaddrOut(from, from_len, source);
  if (GuestOk(bytes_recv, 4))
    *bytes_recv = static_cast<uint32_t>(ret);
  if (flags_ptr)
    *flags_ptr = 0u;
  return 0;
}

// --- QoS -------------------------------------------------------------------

class KernelGuestHeap final : public np::GuestHeap {
 public:
  uint32_t Alloc(uint32_t size) override { return REX_KERNEL_MEMORY()->SystemHeapAlloc(size); }
  void Free(uint32_t guest_addr) override { REX_KERNEL_MEMORY()->SystemHeapFree(guest_addr); }
  uint8_t* Translate(uint32_t guest_addr) override { return GuestBytes(guest_addr); }
};

KernelGuestHeap g_guest_heap;

// Live XNQOS blocks handed out by XNetQosLookup / XNetQosServiceLookup, by guest address.
// Only guest threads take this lock; the network thread touches QosResults objects only.
std::mutex g_qos_mutex;
std::unordered_map<uint32_t, std::shared_ptr<np::QosResults>> g_qos_results;

void SignalGuestEvent(uint32_t event_handle) {
  if (!event_handle)
    return;
  auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
  if (ev)
    ev->Set(0, false);
}

std::shared_ptr<np::QosResults> NewQosResults(uint32_t count, uint32_t targets,
                                              uint32_t event_handle) {
  const uint32_t addr = g_guest_heap.Alloc(np::XnQosBytes(count));
  if (!addr)
    return nullptr;
  np::QosResults::Signal signal;
  if (event_handle)
    signal = [event_handle] { SignalGuestEvent(event_handle); };
  auto results =
      std::make_shared<np::QosResults>(&g_guest_heap, addr, count, targets, std::move(signal));
  results->Init();
  std::lock_guard<std::mutex> lock(g_qos_mutex);
  g_qos_results[addr] = results;
  return results;
}

// Stops the lookup, then frees the XNQOS and every data blob it points to. No glue lock is
// held while the core is called.
void ReleaseQosResults(const std::shared_ptr<np::QosResults>& results) {
  const uint64_t lookup_id = results->lookup_id();
  const std::vector<uint32_t> allocations = results->Release();
  if (lookup_id)
    Netplay::Get().QosCancel(lookup_id);
  for (uint32_t addr : allocations)
    g_guest_heap.Free(addr);
}

std::shared_ptr<np::QosResults> TakeQosResults(uint32_t addr) {
  std::lock_guard<std::mutex> lock(g_qos_mutex);
  auto it = g_qos_results.find(addr);
  if (it == g_qos_results.end())
    return nullptr;
  auto results = std::move(it->second);
  g_qos_results.erase(it);
  return results;
}

uint32_t QosDataLimit() {
  const uint32_t div4 = xnet_startup_params.cfgQosDataLimitDiv4;
  return div4 ? div4 * 4 : 256;  // XDK default cfgQosDataLimitDiv4 = 64
}

}  // namespace
#endif  // REX_HAVE_AC6NET

ppc_u32_result_t NetDll_XNetStartup_entry(ppc_u32_t caller, ppc_ptr_t<XNetStartupParams> params) {
  if (params) {
    assert_true(params->cfgSizeOfStruct == sizeof(XNetStartupParams));
    std::memcpy(&xnet_startup_params, params, sizeof(XNetStartupParams));
  }

  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");

  /*
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return 0;
}

ppc_u32_result_t NetDll_XNetCleanup_entry(ppc_u32_t caller, ppc_pvoid_t params) {
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  // auto xnet = xam->xnet();
  // xam->set_xnet(nullptr);

  // TODO: Shut down and delete.
  // delete xnet;

  return 0;
}

ppc_u32_result_t NetDll_XNetGetOpt_entry(ppc_u32_t one, ppc_u32_t option_id, ppc_pvoid_t buffer_ptr,
                                         ppc_pu32_t buffer_size) {
  assert_true(one == 1);
  switch (option_id) {
    case 1:
      if (*buffer_size < sizeof(XNetStartupParams)) {
        *buffer_size = sizeof(XNetStartupParams);
        return 0x2738;  // WSAEMSGSIZE
      }
      std::memcpy(buffer_ptr, &xnet_startup_params, sizeof(XNetStartupParams));
      return 0;
    default:
      REXKRNL_ERROR("NetDll_XNetGetOpt: option {} unimplemented", option_id);
      return 0x2726;  // WSAEINVAL
  }
}

ppc_u32_result_t NetDll_XNetRandom_entry(ppc_u32_t caller, ppc_pvoid_t buffer_ptr,
                                         ppc_u32_t length) {
#if REX_HAVE_AC6NET
  if (NetplayOn()) {
    if (!length)
      return 0;
    if (!GuestOk(buffer_ptr, length))
      return wsa::kFault;
    Netplay::Get().Random(HostBytes(buffer_ptr), length);
    return 0;
  }
#endif
  // For now, constant values.
  // This makes replicating things easier.
  std::memset(buffer_ptr, 0xBB, length);

  return 0;
}

ppc_u32_result_t NetDll_WSAStartup_entry(ppc_u32_t caller, ppc_u16_t version,
                                         ppc_ptr_t<X_WSADATA> data_ptr) {
// TODO(benvanik): abstraction layer needed.
#if REX_PLATFORM_WIN32
  WSADATA wsaData;
  ZeroMemory(&wsaData, sizeof(WSADATA));
  int ret = WSAStartup(version, &wsaData);

  auto data_out = REX_KERNEL_MEMORY()->TranslateVirtual(data_ptr.guest_address());

  if (data_ptr) {
    data_ptr->version = wsaData.wVersion;
    data_ptr->version_high = wsaData.wHighVersion;
    std::memcpy(&data_ptr->description, wsaData.szDescription, 0x100);
    std::memcpy(&data_ptr->system_status, wsaData.szSystemStatus, 0x80);
    data_ptr->max_sockets = wsaData.iMaxSockets;
    data_ptr->max_udpdg = wsaData.iMaxUdpDg;

    // Some games (5841099F) want this value round-tripped - they'll compare if
    // it changes and bugcheck if it does.
    uint32_t vendor_ptr = memory::load_and_swap<uint32_t>(data_out + 0x190);
    memory::store_and_swap<uint32_t>(data_out + 0x190, vendor_ptr);
  }
#else
  int ret = 0;
  if (data_ptr) {
    // Guess these values!
    data_ptr->version = version.value();
    data_ptr->description[0] = '\0';
    data_ptr->system_status[0] = '\0';
    data_ptr->max_sockets = 100;
    data_ptr->max_udpdg = 1024;
  }
#endif

  // DEBUG
  /*
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return ret;
}

ppc_u32_result_t NetDll_WSACleanup_entry(ppc_u32_t caller) {
  // This does nothing. Xenia needs WSA running.
  return 0;
}

ppc_u32_result_t NetDll_WSAGetLastError_entry() {
  return XThread::GetLastError();
}

// WSARecvFrom(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags, lpFrom, lpFromlen,
// lpOverlapped, lpCompletionRoutine). The fromlen argument was missing from the original
// signature; the XSocket path ignores every argument, so adding it changes nothing there.
ppc_u32_result_t NetDll_WSARecvFrom_entry(ppc_u32_t caller, ppc_u32_t socket,
                                          ppc_ptr_t<XWSABUF> buffers_ptr, ppc_u32_t buffer_count,
                                          ppc_pu32_t num_bytes_recv, ppc_pu32_t flags_ptr,
                                          ppc_ptr_t<XSOCKADDR_IN> from_addr, ppc_pu32_t fromlen_ptr,
                                          ppc_ptr_t<XWSAOVERLAPPED> overlapped_ptr,
                                          ppc_pvoid_t completion_routine_ptr) {
#if REX_HAVE_AC6NET
  if (IsVnetSocket(socket)) {
    return VnetWsaRecvFrom(socket, buffers_ptr, buffer_count, num_bytes_recv, flags_ptr, from_addr,
                           fromlen_ptr, overlapped_ptr || completion_routine_ptr);
  }
#endif
  if (overlapped_ptr) {
    // auto evt = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(
    //    overlapped_ptr->event_handle);

    // if (evt) {
    //  //evt->Set(0, false);
    //}
  }

  // we're not going to be receiving packets any time soon
  // return error so we don't wait on that - Cancerous
  return -1;
}

// If the socket is a VDP socket, buffer 0 is the game data length, and buffer 1
// is the unencrypted game data.
ppc_u32_result_t NetDll_WSASendTo_entry(ppc_u32_t caller, ppc_u32_t socket_handle,
                                        ppc_ptr_t<XWSABUF> buffers, ppc_u32_t num_buffers,
                                        ppc_pu32_t num_bytes_sent, ppc_u32_t flags,
                                        ppc_ptr_t<XSOCKADDR_IN> to_ptr, ppc_u32_t to_len,
                                        ppc_ptr_t<XWSAOVERLAPPED> overlapped,
                                        ppc_pvoid_t completion_routine) {
#if REX_HAVE_AC6NET
  if (IsVnetSocket(socket_handle)) {
    return VnetWsaSendTo(socket_handle, buffers, num_buffers, num_bytes_sent, flags, to_ptr, to_len,
                         overlapped || completion_routine);
  }
#endif
  assert(!overlapped);
  assert(!completion_routine);

  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  // Our sockets implementation doesn't support multiple buffers, so we need
  // to combine the buffers the game has given us!
  std::vector<uint8_t> combined_buffer_mem;
  uint32_t combined_buffer_size = 0;
  uint32_t combined_buffer_offset = 0;
  for (uint32_t i = 0; i < num_buffers; i++) {
    combined_buffer_size += buffers[i].len;
    combined_buffer_mem.resize(combined_buffer_size);
    uint8_t* combined_buffer = combined_buffer_mem.data();

    std::memcpy(combined_buffer + combined_buffer_offset,
                REX_KERNEL_MEMORY()->TranslateVirtual(buffers[i].buf_ptr), buffers[i].len);
    combined_buffer_offset += buffers[i].len;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  socket->SendTo(combined_buffer_mem.data(), combined_buffer_size, flags, &native_to, to_len);

  // TODO: Instantly complete overlapped

  return 0;
}

ppc_u32_result_t NetDll_WSAWaitForMultipleEvents_entry(ppc_u32_t num_events, ppc_pu32_t events,
                                                       ppc_u32_t wait_all, ppc_u32_t timeout,
                                                       ppc_u32_t alertable) {
  if (num_events > 64) {
    XThread::SetLastError(87);  // ERROR_INVALID_PARAMETER
    return ~0u;
  }

  uint64_t timeout_wait = (uint64_t)timeout;

  X_STATUS result = 0;
  do {
    result = xboxkrnl::xeNtWaitForMultipleObjectsEx(num_events, events, wait_all, 1, alertable,
                                                    timeout != -1 ? &timeout_wait : nullptr);
  } while (result == X_STATUS_ALERTED);

  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return ~0u;
  }
  return 0;
}

ppc_u32_result_t NetDll_WSACreateEvent_entry() {
  XEvent* ev = new XEvent(REX_KERNEL_STATE());
  ev->Initialize(true, false);
  return ev->handle();
}

ppc_u32_result_t NetDll_WSACloseEvent_entry(ppc_u32_t event_handle) {
  X_STATUS result = REX_KERNEL_OBJECTS()->ReleaseHandle(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

ppc_u32_result_t NetDll_WSAResetEvent_entry(ppc_u32_t event_handle) {
  X_STATUS result = xboxkrnl::xeNtClearEvent(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

ppc_u32_result_t NetDll_WSASetEvent_entry(ppc_u32_t event_handle) {
  X_STATUS result = xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

struct XnAddrStatus {
  // Address acquisition is not yet complete
  static const uint32_t XNET_GET_XNADDR_PENDING = 0x00000000;
  // XNet is uninitialized or no debugger found
  static const uint32_t XNET_GET_XNADDR_NONE = 0x00000001;
  // Host has ethernet address (no IP address)
  static const uint32_t XNET_GET_XNADDR_ETHERNET = 0x00000002;
  // Host has statically assigned IP address
  static const uint32_t XNET_GET_XNADDR_STATIC = 0x00000004;
  // Host has DHCP assigned IP address
  static const uint32_t XNET_GET_XNADDR_DHCP = 0x00000008;
  // Host has PPPoE assigned IP address
  static const uint32_t XNET_GET_XNADDR_PPPOE = 0x00000010;
  // Host has one or more gateways configured
  static const uint32_t XNET_GET_XNADDR_GATEWAY = 0x00000020;
  // Host has one or more DNS servers configured
  static const uint32_t XNET_GET_XNADDR_DNS = 0x00000040;
  // Host is currently connected to online service
  static const uint32_t XNET_GET_XNADDR_ONLINE = 0x00000080;
  // Network configuration requires troubleshooting
  static const uint32_t XNET_GET_XNADDR_TROUBLESHOOT = 0x00008000;
};

ppc_u32_result_t NetDll_XNetGetTitleXnAddr_entry(ppc_u32_t caller, ppc_ptr_t<XNADDR> addr_ptr) {
#if REX_HAVE_AC6NET
  if (NetplayOn()) {
    // AC6 busy-waits on the game thread while this returns PENDING, so it never does. The
    // first call gives a first-ever registration a bounded chance to finish.
    Netplay& netplay = Netplay::Get();
    static std::once_flag first_call;
    std::call_once(first_call,
                   [&netplay] { netplay.WaitRegistered(netplay.config().first_connect_wait_ms); });
    const ac6net::Identity identity = netplay.identity();
    const ac6net::XnAddr xnaddr = netplay.LocalXnAddr();
    if (GuestOk(addr_ptr, np::kXnAddrSize))
      np::WriteXnAddr(HostBytes(addr_ptr), xnaddr);
    uint32_t status = np::kXnAddrStatusOffline;  // loopback XNADDR, no identity yet
    if (identity.valid && identity.vip) {
      // XNADDR from ARCHITECTURE.md section 3. It stays the same while the server is
      // unreachable; only the ONLINE bit tracks the connection.
      status = netplay.state() == ac6net::NetState::kOffline
                   ? (np::kXnAddrStatusOnline & ~np::kXnAddrOnline)
                   : np::kXnAddrStatusOnline;
    }
    // Report changes only: the game calls this from several places.
    static std::atomic<uint64_t> last_reported{~uint64_t{0}};
    const uint64_t report = (uint64_t{xnaddr.ina} << 32) | status;
    if (last_reported.exchange(report) != report) {
      REXKRNL_INFO("XNetGetTitleXnAddr: netplay {} (status {:02X})",
                   ac6net::Ipv4ToString(xnaddr.ina), status);
      TraceEvent("xnet.title_xnaddr",
                 IpField("ina", xnaddr.ina) + ",\"status\":" + std::to_string(status));
    }
    return status;
  }
#endif
  // Just return a loopback address atm.
  addr_ptr->ina.s_addr = htonl(INADDR_LOOPBACK);
  addr_ptr->inaOnline.s_addr = 0;
  addr_ptr->wPortOnline = 0;

  // TODO(gibbed): A proper mac address.
  // RakNet's 360 version appears to depend on abEnet to create "random" 64-bit
  // numbers. A zero value will cause RakPeer::Startup to fail. This causes
  // 58411436 to crash on startup.
  // The 360-specific code is scrubbed from the RakNet repo, but there's still
  // traces of what it's doing which match the game code.
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L382
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L4527
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L4467
  // "Mac address is a poor solution because you can't have multiple connections
  // from the same system"
  std::memset(addr_ptr->abEnet, 0xCC, 6);

  std::memset(addr_ptr->abOnline, 0, 20);

  return XnAddrStatus::XNET_GET_XNADDR_STATIC;
}

ppc_u32_result_t NetDll_XNetGetDebugXnAddr_entry(ppc_u32_t caller, ppc_ptr_t<XNADDR> addr_ptr) {
  addr_ptr.Zero();

  // XNET_GET_XNADDR_NONE causes caller to gracefully return.
  return XnAddrStatus::XNET_GET_XNADDR_NONE;
}

ppc_u32_result_t NetDll_XNetXnAddrToMachineId_entry(ppc_u32_t caller, ppc_ptr_t<XNADDR> addr_ptr,
                                                    ppc_pu32_t id_ptr) {
  // Tell the caller we're not signed in to live (non-zero ret)
  return 1;
}

void NetDll_XNetInAddrToString_entry(ppc_u32_t caller, ppc_u32_t in_addr, ppc_pchar_t string_out,
                                     ppc_u32_t string_size) {
  rex::string::rex_strcpy(string_out, string_size, "666.666.666.666");
}

// This converts a XNet address to an IN_ADDR. The IN_ADDR is used for
// subsequent socket calls (like a handle to a XNet address)
ppc_u32_result_t NetDll_XNetXnAddrToInAddr_entry(ppc_u32_t caller, ppc_ptr_t<XNADDR> xn_addr,
                                                 ppc_pvoid_t xid, ppc_pvoid_t in_addr) {
#if REX_HAVE_AC6NET
  if (NetplayOn()) {
    if (!GuestOk(xn_addr, np::kXnAddrSize) || !GuestOk(in_addr, 4))
      return wsa::kInval;
    if (xid && !GuestOk(xid, np::kXnKidSize))
      return wsa::kInval;
    const ac6net::XnAddr addr = np::ReadXnAddr(HostBytes(xn_addr));
    ac6net::XnKid kid;
    if (xid)
      kid = np::ReadXnKid(HostBytes(xid));
    uint32_t vip = 0;
    const int err = Netplay::Get().XnAddrToInAddr(addr, xid ? &kid : nullptr, &vip);
    TraceEvent("xnet.xnaddr_to_inaddr", IpField("ina_online", addr.ina_online) + "," +
                                            IpField("vip", vip) +
                                            ",\"err\":" + std::to_string(err));
    if (err != wsa::kOk) {
      REXKRNL_WARN("XNetXnAddrToInAddr: {} not resolvable ({})",
                   ac6net::Ipv4ToString(addr.ina_online ? addr.ina_online : addr.ina), err);
      return static_cast<u32>(err);
    }
    np::StoreBe32(HostBytes(in_addr), vip);  // IN_ADDR: network order == big-endian
    return 0;
  }
#endif
  return 1;
}

// Does the reverse of the above: XNetInAddrToXnAddr(const IN_ADDR ina, XNADDR* pxna,
// XNKID* pxnkid). The 4-byte IN_ADDR is passed by value, so the register already holds the
// address in host numeric form (10.64.0.7 == 0x0A400007).
ppc_u32_result_t NetDll_XNetInAddrToXnAddr_entry(ppc_u32_t caller, ppc_u32_t in_addr,
                                                 ppc_ptr_t<XNADDR> xn_addr, ppc_pvoid_t xid) {
#if REX_HAVE_AC6NET
  if (NetplayOn()) {
    if (xn_addr && !GuestOk(xn_addr, np::kXnAddrSize))
      return wsa::kInval;
    if (xid && !GuestOk(xid, np::kXnKidSize))
      return wsa::kInval;
    ac6net::XnAddr addr;
    ac6net::XnKid kid;
    const int err = Netplay::Get().InAddrToXnAddr(in_addr, &addr, &kid);
    TraceEvent("xnet.inaddr_to_xnaddr",
               IpField("vip", in_addr) + ",\"err\":" + std::to_string(err));
    if (err != wsa::kOk) {
      if (xn_addr)
        xn_addr.Zero();
      REXKRNL_WARN("XNetInAddrToXnAddr: {} unknown ({})", ac6net::Ipv4ToString(in_addr), err);
      return static_cast<u32>(err);
    }
    if (xn_addr)
      np::WriteXnAddr(HostBytes(xn_addr), addr);
    if (xid)
      np::WriteXnKid(HostBytes(xid), kid);
    return 0;
  }
#endif
  return 1;
}

// https://www.google.com/patents/WO2008112448A1?cl=en
// Reserves a port for use by system link
ppc_u32_result_t NetDll_XNetSetSystemLinkPort_entry(ppc_u32_t caller, ppc_u32_t port) {
  return 1;
}

// https://github.com/ILOVEPIE/Cxbx-Reloaded/blob/master/src/CxbxKrnl/EmuXOnline.h#L39
struct XEthernetStatus {
  static const uint32_t XNET_ETHERNET_LINK_ACTIVE = 0x01;
  static const uint32_t XNET_ETHERNET_LINK_100MBPS = 0x02;
  static const uint32_t XNET_ETHERNET_LINK_10MBPS = 0x04;
  static const uint32_t XNET_ETHERNET_LINK_FULL_DUPLEX = 0x08;
  static const uint32_t XNET_ETHERNET_LINK_HALF_DUPLEX = 0x10;
};

ppc_u32_result_t NetDll_XNetGetEthernetLinkStatus_entry(ppc_u32_t caller) {
  return 0;
}

ppc_u32_result_t NetDll_XNetDnsLookup_entry(ppc_u32_t caller, ppc_pchar_t host,
                                            ppc_u32_t event_handle, ppc_pu32_t pdns) {
  // TODO(gibbed): actually implement this
  if (pdns) {
    auto dns_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNDNS));
    auto dns = REX_KERNEL_MEMORY()->TranslateVirtual<XNDNS*>(dns_guest);
    dns->status = 1;  // non-zero = error
    *pdns = dns_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}

ppc_u32_result_t NetDll_XNetDnsRelease_entry(ppc_u32_t caller, ppc_ptr_t<XNDNS> dns) {
  if (!dns) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(dns.guest_address());
  return 0;
}

ppc_u32_result_t NetDll_XNetQosServiceLookup_entry(ppc_u32_t caller, ppc_u32_t flags,
                                                   ppc_u32_t event_handle, ppc_pu32_t pqos) {
#if REX_HAVE_AC6NET
  if (NetplayOn()) {
    if (!GuestOk(pqos, 4))
      return wsa::kInval;
    auto results = NewQosResults(1, 1, event_handle);
    if (!results)
      return wsa::kNoBufs;
    *pqos = results->guest_address();
    uint32_t rtt_ms = 0, up_bps = 0, dn_bps = 0;
    const bool ok = Netplay::Get().QosServiceLookup(&rtt_ms, &up_bps, &dn_bps);
    // One completed entry; the nominal 10 Mbit/s (when the core reports none) gives tier 5.
    results->OnResult(0, np::MakeServiceResult(ok, rtt_ms, up_bps, dn_bps), true);
    TraceEvent("qos.service_lookup", std::string("\"ok\":") + (ok ? "true" : "false") +
                                         ",\"rtt_ms\":" + std::to_string(rtt_ms));
    return 0;
  }
#endif
  // Set pqos as some games will try accessing it despite non-successful result
  if (pqos) {
    auto qos_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNQOS));
    auto qos = REX_KERNEL_MEMORY()->TranslateVirtual<XNQOS*>(qos_guest);
    qos->count = qos->count_pending = 0;
    *pqos = qos_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}

ppc_u32_result_t NetDll_XNetQosRelease_entry(ppc_u32_t caller, ppc_ptr_t<XNQOS> qos) {
  if (!qos) {
    return X_STATUS_INVALID_PARAMETER;
  }
#if REX_HAVE_AC6NET
  if (auto results = TakeQosResults(qos.guest_address())) {
    ReleaseQosResults(results);  // cancels the lookup, frees the XNQOS and its data blobs
    return 0;
  }
  if (NetplayOn()) {
    // Not one of ours (or already released): freeing it could corrupt the heap.
    REXKRNL_WARN("XNetQosRelease: unknown or already released XNQOS {:08X}", qos.guest_address());
    return 0;
  }
#endif
  REX_KERNEL_MEMORY()->SystemHeapFree(qos.guest_address());
  return 0;
}

// XNetQosListen(const XNKID* pxnkid, const BYTE* pb, UINT cb, DWORD dwBitsPerSec,
//               DWORD dwFlags)
ppc_u32_result_t NetDll_XNetQosListen_entry(ppc_u32_t caller, ppc_pvoid_t id, ppc_pvoid_t data,
                                            ppc_u32_t data_size, ppc_u32_t r7, ppc_u32_t flags) {
#if REX_HAVE_AC6NET
  if (NetplayOn()) {
    if (!GuestOk(id, np::kXnKidSize))
      return wsa::kInval;
    const ac6net::XnKid kid = np::ReadXnKid(HostBytes(id));
    const uint8_t* bytes = nullptr;
    size_t len = 0;
    if (data && data_size) {
      if (data_size > QosDataLimit())
        return wsa::kMsgSize;
      if (!GuestOk(data, data_size))
        return wsa::kFault;
      bytes = HostBytes(data);
      len = data_size;
    }
    Netplay::Get().QosListen(kid, bytes, len, r7, flags);
    TraceEvent("qos.listen", "\"xnkid\":\"" + ac6net::HexEncode(kid.b.data(), kid.b.size()) +
                                 "\",\"len\":" + std::to_string(len) +
                                 ",\"flags\":" + std::to_string(flags));
    return 0;
  }
#endif
  return X_ERROR_FUNCTION_FAILED;
}

// XNetQosLookup(UINT cxna, const XNADDR* apxna[], const XNKID* apxnkid[],
//               const XNKEY* apxnkey[], UINT cina, const IN_ADDR aina[],
//               const DWORD adwServiceId[], UINT cProbes, DWORD dwBitsPerSec, DWORD dwFlags,
//               WSAEVENT hEvent, XNQOS** ppxnqos)
// The XNQOS is handed back at once with cxnqosPending = cxna; the ac6net callback fills each
// entry (network thread) and drops cxnqosPending to 0 when all are done.
u32 NetDll_XNetQosLookup_entry(u32 caller, u32 num_remote, u32 xnaddr_ptrs, u32 xnkid_ptrs,
                               u32 xnkey_ptrs, u32 num_gateways, u32 gateway_addrs, u32 service_ids,
                               u32 num_probes, u32 bits_per_sec, u32 flags, u32 event_handle,
                               ppc_pu32_t pxnqos) {
#if REX_HAVE_AC6NET
  if (NetplayOn()) {
    if (!GuestOk(pxnqos, 4))
      return wsa::kInval;
    const uint64_t count = uint64_t{num_remote} + num_gateways;
    if (count == 0 || count > np::kMaxQosEntries)
      return wsa::kInval;
    if (num_remote && !np::GuestRangeOk(xnaddr_ptrs, uint64_t{num_remote} * 4)) {
      return wsa::kInval;
    }
    if (xnkid_ptrs && !np::GuestRangeOk(xnkid_ptrs, uint64_t{num_remote} * 4)) {
      return wsa::kInval;
    }

    // Unreadable entries become zero addresses, which the core completes without contact.
    std::vector<ac6net::XnAddr> addrs(num_remote);
    std::vector<ac6net::XnKid> kids(num_remote);
    for (uint32_t i = 0; i < num_remote; ++i) {
      const uint32_t addr_ptr = np::LoadBe32(GuestBytes(xnaddr_ptrs + 4 * i));
      if (np::GuestRangeOk(addr_ptr, np::kXnAddrSize) && addr_ptr) {
        addrs[i] = np::ReadXnAddr(GuestBytes(addr_ptr));
      }
      if (xnkid_ptrs) {
        const uint32_t kid_ptr = np::LoadBe32(GuestBytes(xnkid_ptrs + 4 * i));
        if (np::GuestRangeOk(kid_ptr, np::kXnKidSize) && kid_ptr) {
          kids[i] = np::ReadXnKid(GuestBytes(kid_ptr));
        }
      }
    }

    auto results = NewQosResults(static_cast<uint32_t>(count), num_remote, event_handle);
    if (!results)
      return wsa::kNoBufs;
    *pxnqos = results->guest_address();
    if (num_gateways) {
      REXKRNL_WARN("XNetQosLookup: {} security gateway target(s) not supported", num_gateways);
    }
    if (!num_remote) {
      // Only gateway entries, all complete already.
      SignalGuestEvent(event_handle);
      return 0;
    }

    // With no probes the lookup still needs one round trip to fetch the listener's data.
    const uint32_t probes = std::clamp<uint32_t>(num_probes, 1, np::kMaxQosProbes);
    const uint64_t lookup_id = Netplay::Get().QosLookup(
        addrs, kids, probes,
        [results](size_t index, const ac6net::QosTargetResult& r, bool all_done) {
          results->OnResult(index, r, all_done);
          if (all_done) {
            TraceEvent("qos.lookup.done", std::string("\"contacted\":") +
                                              (r.contacted ? "true" : "false") +
                                              ",\"data\":" + std::to_string(r.data.size()) +
                                              ",\"rtt_med_ms\":" + std::to_string(r.rtt_med_ms));
          }
        });
    if (!lookup_id) {
      TakeQosResults(results->guest_address());
      ReleaseQosResults(results);
      *pxnqos = 0u;
      REXKRNL_WARN("XNetQosLookup: core refused the lookup");
      return wsa::kNetDown;
    }
    results->set_lookup_id(lookup_id);
    TraceEvent("qos.lookup.start", IpField("target", addrs[0].ina_online) +
                                       ",\"n\":" + std::to_string(num_remote) +
                                       ",\"probes\":" + std::to_string(probes));
    return 0;
  }
#endif
  REXKRNL_WARN("{} STUB", "__imp__NetDll_XNetQosLookup");
  return caller;  // the former REX_EXPORT_STUB left r3 (= caller) as the return value
}

// XNetCreateKey(XNKID* pxnkid, XNKEY* pxnkey): random system-link XNKID (type byte 0x00) and
// a random key, as Xenia's GenerateSessionId(XNKID_SYSTEM_LINK).
u32 NetDll_XNetCreateKey_entry(u32 caller, ppc_pvoid_t xnkid, ppc_pvoid_t xnkey) {
#if REX_HAVE_AC6NET
  if (NetplayOn()) {
    if (!GuestOk(xnkid, np::kXnKidSize) || !GuestOk(xnkey, np::kXnKeySize)) {
      return wsa::kInval;
    }
    ac6net::XnKid kid;
    ac6net::XnKey key;
    Netplay::Get().Random(kid.b.data(), kid.b.size());
    Netplay::Get().Random(key.b.data(), key.b.size());
    np::MakeSystemLinkKid(&kid);
    np::WriteXnKid(HostBytes(xnkid), kid);
    std::memcpy(HostBytes(xnkey), key.b.data(), key.b.size());
    return 0;
  }
#endif
  REXKRNL_WARN("{} STUB", "__imp__NetDll_XNetCreateKey");
  return caller;  // the former REX_EXPORT_STUB left r3 (= caller) as the return value
}

// XNetRegisterKey(const XNKID*, const XNKEY*): nothing to register, vnet has no security
// associations.
u32 NetDll_XNetRegisterKey_entry(u32 caller, ppc_pvoid_t xnkid, ppc_pvoid_t xnkey) {
#if REX_HAVE_AC6NET
  if (NetplayOn())
    return 0;
#endif
  REXKRNL_WARN("{} STUB", "__imp__NetDll_XNetRegisterKey");
  return caller;  // the former REX_EXPORT_STUB left r3 (= caller) as the return value
}

ppc_u32_result_t NetDll_inet_addr_entry(ppc_pchar_t addr_ptr) {
  if (!addr_ptr) {
    return -1;
  }

  uint32_t addr = inet_addr(addr_ptr);
  // https://docs.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-inet_addr#return-value
  // Based on console research it seems like x360 uses old version of inet_addr
  // In case of empty string it return 0 instead of -1
  if (addr == -1 && !addr_ptr.value().length()) {
    return 0;
  }

  return rex::byte_swap(addr);
}

ppc_u32_result_t NetDll_socket_entry(ppc_u32_t caller, ppc_u32_t af, ppc_u32_t type,
                                     ppc_u32_t protocol) {
#if REX_HAVE_AC6NET
  if (NetplayOn())
    return VnetSocket(af, type, protocol);
#endif
  XSocket* socket = new XSocket(REX_KERNEL_STATE());
  X_STATUS result =
      socket->Initialize(XSocket::AddressFamily((uint32_t)af), XSocket::Type((uint32_t)type),
                         XSocket::Protocol((uint32_t)protocol));

  if (XFAILED(result)) {
    socket->Release();

    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return -1;
  }

  return socket->handle();
}

ppc_u32_result_t NetDll_closesocket_entry(ppc_u32_t caller, ppc_u32_t socket_handle) {
#if REX_HAVE_AC6NET
  if (IsVnetSocket(socket_handle)) {
    if (ac6net::TraceEnabled())  // the stack walk only feeds the trace
      TraceEvent("net.close", "\"h\":" + std::to_string(uint32_t(socket_handle)) +
                                  ",\"stack\":" + GuestStackJson());
    return static_cast<u32>(VnetClose(socket_handle));
  }
#endif
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  // TODO: Absolutely delete this object. It is no longer valid after calling
  // closesocket.
  socket->Close();
  socket->ReleaseHandle();
  return 0;
}

ppc_i32_result_t NetDll_shutdown_entry(ppc_u32_t caller, ppc_u32_t socket_handle, ppc_i32_t how) {
#if REX_HAVE_AC6NET
  if (IsVnetSocket(socket_handle)) {
    if (ac6net::TraceEnabled())  // the stack walk only feeds the trace
      TraceEvent("net.shutdown", "\"h\":" + std::to_string(uint32_t(socket_handle)) +
                                     ",\"how\":" + std::to_string(int32_t(how)) +
                                     ",\"stack\":" + GuestStackJson());
    return VnetShutdown(socket_handle, how);
  }
#endif
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  auto ret = socket->Shutdown(how);
  if (ret == -1) {
#if REX_PLATFORM_WIN32
    uint32_t error_code = WSAGetLastError();
    XThread::SetLastError(error_code);
#else
    XThread::SetLastError(0x0);
#endif
  }
  return ret;
}

ppc_u32_result_t NetDll_setsockopt_entry(ppc_u32_t caller, ppc_u32_t socket_handle, ppc_u32_t level,
                                         ppc_u32_t optname, ppc_pvoid_t optval_ptr,
                                         ppc_u32_t optlen) {
#if REX_HAVE_AC6NET
  if (IsVnetSocket(socket_handle)) {
    return static_cast<u32>(VnetSetOption(socket_handle, level, optname, optval_ptr, optlen));
  }
#endif
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->SetOption(level, optname, optval_ptr, optlen);
  return XSUCCEEDED(status) ? 0 : -1;
}

ppc_u32_result_t NetDll_ioctlsocket_entry(ppc_u32_t caller, ppc_u32_t socket_handle, ppc_u32_t cmd,
                                          ppc_pvoid_t arg_ptr) {
#if REX_HAVE_AC6NET
  if (IsVnetSocket(socket_handle))
    return static_cast<u32>(VnetIoctl(socket_handle, cmd, arg_ptr));
#endif
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->IOControl(cmd, arg_ptr);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  // TODO
  return 0;
}

ppc_u32_result_t NetDll_bind_entry(ppc_u32_t caller, ppc_u32_t socket_handle,
                                   ppc_ptr_t<XSOCKADDR_IN> name, ppc_u32_t namelen) {
#if REX_HAVE_AC6NET
  if (IsVnetSocket(socket_handle))
    return static_cast<u32>(VnetBind(socket_handle, name, namelen));
#endif
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_name(name);
  X_STATUS status = socket->Bind(&native_name, namelen);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

ppc_u32_result_t NetDll_connect_entry(ppc_u32_t caller, ppc_u32_t socket_handle,
                                      ppc_ptr_t<XSOCKADDR> name, ppc_u32_t namelen) {
#if REX_HAVE_AC6NET
  if (IsVnetSocket(socket_handle)) {
    return static_cast<u32>(VnetConnect(socket_handle, name, namelen));
  }
#endif
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_name(name);
  X_STATUS status = socket->Connect(&native_name, namelen);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

ppc_u32_result_t NetDll_listen_entry(ppc_u32_t caller, ppc_u32_t socket_handle, ppc_i32_t backlog) {
#if REX_HAVE_AC6NET
  if (IsVnetSocket(socket_handle))
    return static_cast<u32>(VnetListen(socket_handle, backlog));
#endif
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->Listen(backlog);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

ppc_u32_result_t NetDll_accept_entry(ppc_u32_t caller, ppc_u32_t socket_handle,
                                     ppc_ptr_t<XSOCKADDR> addr_ptr, ppc_pu32_t addrlen_ptr) {
#if REX_HAVE_AC6NET
  if (IsVnetSocket(socket_handle))
    return VnetAccept(socket_handle, addr_ptr, addrlen_ptr);
#endif
  if (!addr_ptr) {
    // WSAEFAULT
    XThread::SetLastError(0x271E);
    return -1;
  }

  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_addr(addr_ptr);
  int native_len = *addrlen_ptr;
  auto new_socket = socket->Accept(&native_addr, &native_len);
  if (new_socket) {
    addr_ptr->address_family = native_addr.address_family;
    std::memcpy(addr_ptr->sa_data, native_addr.sa_data, *addrlen_ptr - 2);
    *addrlen_ptr = native_len;

    return new_socket->handle();
  } else {
    return -1;
  }
}

struct x_fd_set {
  rex::be<uint32_t> fd_count;
  rex::be<uint32_t> fd_array[64];
};

struct host_set {
  uint32_t count;
  object_ref<XSocket> sockets[64];

  void Load(const x_fd_set* guest_set) {
    assert_true(guest_set->fd_count < 64);
    this->count = guest_set->fd_count;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket_handle = static_cast<X_HANDLE>(guest_set->fd_array[i]);
      if (socket_handle == -1) {
        this->count = i;
        break;
      }
      // Convert from Xenia -> native
      auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
      assert_not_null(socket);
      this->sockets[i] = socket;
    }
  }

  void Store(x_fd_set* guest_set) {
    guest_set->fd_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      guest_set->fd_array[guest_set->fd_count++] = socket->handle();
    }
  }

  void Store(fd_set* native_set) {
    FD_ZERO(native_set);
    for (uint32_t i = 0; i < this->count; ++i) {
      FD_SET(this->sockets[i]->native_handle(), native_set);
    }
  }

  void UpdateFrom(fd_set* native_set) {
    uint32_t new_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      if (FD_ISSET(socket->native_handle(), native_set)) {
        this->sockets[new_count++] = socket;
      }
    }
    this->count = new_count;
  }
};

#if REX_HAVE_AC6NET
namespace {

// select on vnet sockets: read/write/except x_fd_sets, guest nfds ignored, timeval* NULL =
// infinite, sets rewritten to their ready members. Returns false, without touching anything,
// when the call belongs to the XSocket path (netplay off, or only XSocket handles).
bool TryVnetSelect(ppc_ptr_t<x_fd_set> readfds, ppc_ptr_t<x_fd_set> writefds,
                   ppc_ptr_t<x_fd_set> exceptfds, const ppc_pvoid_t& timeout_ptr, i32* result) {
  if (!NetplayOn())
    return false;

  // A set passed twice (4B4D07E7 passes one set as read, write and except) counts once, as
  // the first of them; treating it as writable would report every socket ready (Xenia).
  const ppc_ptr_t<x_fd_set> args[3] = {readfds, writefds, exceptfds};
  uint8_t* sets[3] = {};
  for (int i = 0; i < 3; ++i) {
    if (!args[i])
      continue;
    bool aliased = false;
    for (int j = 0; j < i; ++j) {
      aliased |= args[j] && args[j].guest_address() == args[i].guest_address();
    }
    if (aliased)
      continue;
    if (!GuestOk(args[i], np::kFdSetBytes)) {
      *result = WsaFail(wsa::kFault);
      return true;
    }
    sets[i] = HostBytes(args[i]);
  }

  const np::SelectPlan plan = np::PlanSelect(sets);
  if (!plan.vnet)
    return false;
  if (plan.err != wsa::kOk) {
    *result = WsaFail(plan.err);
    return true;
  }

  int64_t timeout_us = -1;
  if (timeout_ptr) {
    if (!GuestOk(timeout_ptr, np::kTimevalBytes)) {
      *result = WsaFail(wsa::kFault);
      return true;
    }
    int32_t sec = static_cast<int32_t>(np::LoadBe32(HostBytes(timeout_ptr)));
    int32_t usec = static_cast<int32_t>(np::LoadBe32(HostBytes(timeout_ptr) + 4));
    chrono::Clock::ScaleGuestDurationTimeval(&sec, &usec);
    const int err = np::TimevalToMicros(sec, usec, &timeout_us);
    if (err != wsa::kOk) {
      *result = WsaFail(err);
      return true;
    }
  }

  ac6net::vnet::SelectSets ready = plan.query;
  int err = wsa::kOk;
  if (Vnet().Select(&ready, timeout_us, &err) < 0) {
    *result = WsaFail(err);
    return true;
  }
  *result = np::FinishSelect(sets, plan, ready);
  return true;
}

}  // namespace
#endif  // REX_HAVE_AC6NET

ppc_i32_result_t NetDll_select_entry(ppc_i32_t caller, ppc_i32_t nfds, ppc_ptr_t<x_fd_set> readfds,
                                     ppc_ptr_t<x_fd_set> writefds, ppc_ptr_t<x_fd_set> exceptfds,
                                     ppc_pvoid_t timeout_ptr) {
#if REX_HAVE_AC6NET
  i32 vnet_result = 0;
  if (TryVnetSelect(readfds, writefds, exceptfds, timeout_ptr, &vnet_result)) {
    return vnet_result;
  }
#endif
  host_set host_readfds = {0};
  fd_set native_readfds = {0};
  if (readfds) {
    host_readfds.Load(readfds);
    host_readfds.Store(&native_readfds);
  }
  host_set host_writefds = {0};
  fd_set native_writefds = {0};
  if (writefds) {
    host_writefds.Load(writefds);
    host_writefds.Store(&native_writefds);
  }
  host_set host_exceptfds = {0};
  fd_set native_exceptfds = {0};
  if (exceptfds) {
    host_exceptfds.Load(exceptfds);
    host_exceptfds.Store(&native_exceptfds);
  }
  timeval* timeout_in = nullptr;
  timeval timeout;
  if (timeout_ptr) {
    timeout = {static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[0]),
               static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[1])};
    chrono::Clock::ScaleGuestDurationTimeval(reinterpret_cast<int32_t*>(&timeout.tv_sec),
                                             reinterpret_cast<int32_t*>(&timeout.tv_usec));
    timeout_in = &timeout;
  }
  int ret = select(nfds, readfds ? &native_readfds : nullptr, writefds ? &native_writefds : nullptr,
                   exceptfds ? &native_exceptfds : nullptr, timeout_in);
  if (readfds) {
    host_readfds.UpdateFrom(&native_readfds);
    host_readfds.Store(readfds);
  }
  if (writefds) {
    host_writefds.UpdateFrom(&native_writefds);
    host_writefds.Store(writefds);
  }
  if (exceptfds) {
    host_exceptfds.UpdateFrom(&native_exceptfds);
    host_exceptfds.Store(exceptfds);
  }

  // TODO(gibbed): modify ret to be what's actually copied to the guest fd_sets?
  return ret;
}

ppc_u32_result_t NetDll_recv_entry(ppc_u32_t caller, ppc_u32_t socket_handle, ppc_pvoid_t buf_ptr,
                                   ppc_u32_t buf_len, ppc_u32_t flags) {
#if REX_HAVE_AC6NET
  if (IsVnetSocket(socket_handle)) {
    return static_cast<u32>(VnetRecv(socket_handle, buf_ptr, buf_len, flags));
  }
#endif
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  return socket->Recv(buf_ptr, buf_len, flags);
}

ppc_u32_result_t NetDll_recvfrom_entry(ppc_u32_t caller, ppc_u32_t socket_handle,
                                       ppc_pvoid_t buf_ptr, ppc_u32_t buf_len, ppc_u32_t flags,
                                       ppc_ptr_t<XSOCKADDR_IN> from_ptr, ppc_pu32_t fromlen_ptr) {
#if REX_HAVE_AC6NET
  if (IsVnetSocket(socket_handle)) {
    return static_cast<u32>(
        VnetRecvFrom(socket_handle, buf_ptr, buf_len, flags, from_ptr, fromlen_ptr));
  }
#endif
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_from;
  if (from_ptr) {
    native_from = *from_ptr;
  }
  uint32_t native_fromlen = fromlen_ptr ? fromlen_ptr.value() : 0;
  int ret =
      socket->RecvFrom(buf_ptr, buf_len, flags, &native_from, fromlen_ptr ? &native_fromlen : 0);

  if (from_ptr) {
    from_ptr->sin_family = native_from.sin_family;
    from_ptr->sin_port = native_from.sin_port;
    from_ptr->sin_addr = native_from.sin_addr;
    std::memset(from_ptr->x_sin_zero, 0, sizeof(from_ptr->x_sin_zero));
  }
  if (fromlen_ptr) {
    *fromlen_ptr = native_fromlen;
  }

  if (ret == -1) {
// TODO: Better way of getting the error code
#if REX_PLATFORM_WIN32
    uint32_t error_code = WSAGetLastError();
    XThread::SetLastError(error_code);
#else
    XThread::SetLastError(0x0);
#endif
  }

  return ret;
}

ppc_u32_result_t NetDll_send_entry(ppc_u32_t caller, ppc_u32_t socket_handle, ppc_pvoid_t buf_ptr,
                                   ppc_u32_t buf_len, ppc_u32_t flags) {
#if REX_HAVE_AC6NET
  if (IsVnetSocket(socket_handle)) {
    if (buf_len && !GuestOk(buf_ptr, buf_len))
      return static_cast<u32>(WsaFail(wsa::kFault));
    return static_cast<u32>(VnetSend(socket_handle, buf_ptr.host_address(), buf_len, flags));
  }
#endif
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  return socket->Send(buf_ptr, buf_len, flags);
}

ppc_u32_result_t NetDll_sendto_entry(ppc_u32_t caller, ppc_u32_t socket_handle, ppc_pvoid_t buf_ptr,
                                     ppc_u32_t buf_len, ppc_u32_t flags,
                                     ppc_ptr_t<XSOCKADDR_IN> to_ptr, ppc_u32_t to_len) {
#if REX_HAVE_AC6NET
  if (IsVnetSocket(socket_handle)) {
    if (buf_len && !GuestOk(buf_ptr, buf_len))
      return static_cast<u32>(WsaFail(wsa::kFault));
    return static_cast<u32>(
        VnetSendTo(socket_handle, buf_ptr.host_address(), buf_len, flags, to_ptr, to_len));
  }
#endif
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  return socket->SendTo(buf_ptr, buf_len, flags, &native_to, to_len);
}

// getsockname / getpeername / getsockopt: implemented for vnet sockets. On the XSocket path
// they keep the behaviour of the former REX_EXPORT_STUBs (log, return r3 = caller).
u32 NetDll_getsockname_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name,
                             ppc_pu32_t namelen_ptr) {
#if REX_HAVE_AC6NET
  if (IsVnetSocket(socket_handle)) {
    return static_cast<u32>(VnetGetName(socket_handle, name, namelen_ptr, /*peer=*/false));
  }
#endif
  REXKRNL_WARN("{} STUB", "__imp__NetDll_getsockname");
  return caller;
}

u32 NetDll_getpeername_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name,
                             ppc_pu32_t namelen_ptr) {
#if REX_HAVE_AC6NET
  if (IsVnetSocket(socket_handle)) {
    return static_cast<u32>(VnetGetName(socket_handle, name, namelen_ptr, /*peer=*/true));
  }
#endif
  REXKRNL_WARN("{} STUB", "__imp__NetDll_getpeername");
  return caller;
}

u32 NetDll_getsockopt_entry(u32 caller, u32 socket_handle, u32 level, u32 optname,
                            ppc_pvoid_t optval_ptr, ppc_pu32_t optlen_ptr) {
#if REX_HAVE_AC6NET
  if (IsVnetSocket(socket_handle)) {
    return static_cast<u32>(VnetGetOption(socket_handle, level, optname, optval_ptr, optlen_ptr));
  }
#endif
  REXKRNL_WARN("{} STUB", "__imp__NetDll_getsockopt");
  return caller;
}

ppc_u32_result_t NetDll___WSAFDIsSet_entry(ppc_u32_t socket_handle, ppc_ptr_t<x_fd_set> fd_set) {
  const uint8_t max_fd_count = std::min((uint32_t)fd_set->fd_count, uint32_t(64));
  for (uint8_t i = 0; i < max_fd_count; i++) {
    if (fd_set->fd_array[i] == socket_handle) {
      return 1;
    }
  }
  return 0;
}

void NetDll_WSASetLastError_entry(ppc_u32_t error_code) {
  XThread::SetLastError(error_code);
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

XAM_EXPORT(__imp__NetDll_XNetStartup, rex::kernel::xam::NetDll_XNetStartup_entry)
XAM_EXPORT(__imp__NetDll_XNetCleanup, rex::kernel::xam::NetDll_XNetCleanup_entry)
XAM_EXPORT(__imp__NetDll_XNetGetOpt, rex::kernel::xam::NetDll_XNetGetOpt_entry)
XAM_EXPORT(__imp__NetDll_XNetRandom, rex::kernel::xam::NetDll_XNetRandom_entry)
XAM_EXPORT(__imp__NetDll_WSAStartup, rex::kernel::xam::NetDll_WSAStartup_entry)
XAM_EXPORT(__imp__NetDll_WSACleanup, rex::kernel::xam::NetDll_WSACleanup_entry)
XAM_EXPORT(__imp__NetDll_WSAGetLastError, rex::kernel::xam::NetDll_WSAGetLastError_entry)
XAM_EXPORT(__imp__NetDll_WSARecvFrom, rex::kernel::xam::NetDll_WSARecvFrom_entry)
XAM_EXPORT(__imp__NetDll_WSASendTo, rex::kernel::xam::NetDll_WSASendTo_entry)
XAM_EXPORT(__imp__NetDll_WSAWaitForMultipleEvents,
           rex::kernel::xam::NetDll_WSAWaitForMultipleEvents_entry)
XAM_EXPORT(__imp__NetDll_WSACreateEvent, rex::kernel::xam::NetDll_WSACreateEvent_entry)
XAM_EXPORT(__imp__NetDll_WSACloseEvent, rex::kernel::xam::NetDll_WSACloseEvent_entry)
XAM_EXPORT(__imp__NetDll_WSAResetEvent, rex::kernel::xam::NetDll_WSAResetEvent_entry)
XAM_EXPORT(__imp__NetDll_WSASetEvent, rex::kernel::xam::NetDll_WSASetEvent_entry)
XAM_EXPORT(__imp__NetDll_XNetGetTitleXnAddr, rex::kernel::xam::NetDll_XNetGetTitleXnAddr_entry)
XAM_EXPORT(__imp__NetDll_XNetGetDebugXnAddr, rex::kernel::xam::NetDll_XNetGetDebugXnAddr_entry)
XAM_EXPORT(__imp__NetDll_XNetXnAddrToMachineId,
           rex::kernel::xam::NetDll_XNetXnAddrToMachineId_entry)
XAM_EXPORT(__imp__NetDll_XNetInAddrToString, rex::kernel::xam::NetDll_XNetInAddrToString_entry)
XAM_EXPORT(__imp__NetDll_XNetXnAddrToInAddr, rex::kernel::xam::NetDll_XNetXnAddrToInAddr_entry)
XAM_EXPORT(__imp__NetDll_XNetInAddrToXnAddr, rex::kernel::xam::NetDll_XNetInAddrToXnAddr_entry)
XAM_EXPORT(__imp__NetDll_XNetSetSystemLinkPort,
           rex::kernel::xam::NetDll_XNetSetSystemLinkPort_entry)
XAM_EXPORT(__imp__NetDll_XNetGetEthernetLinkStatus,
           rex::kernel::xam::NetDll_XNetGetEthernetLinkStatus_entry)
XAM_EXPORT(__imp__NetDll_XNetDnsLookup, rex::kernel::xam::NetDll_XNetDnsLookup_entry)
XAM_EXPORT(__imp__NetDll_XNetDnsRelease, rex::kernel::xam::NetDll_XNetDnsRelease_entry)
XAM_EXPORT(__imp__NetDll_XNetQosServiceLookup, rex::kernel::xam::NetDll_XNetQosServiceLookup_entry)
XAM_EXPORT(__imp__NetDll_XNetQosRelease, rex::kernel::xam::NetDll_XNetQosRelease_entry)
XAM_EXPORT(__imp__NetDll_XNetQosListen, rex::kernel::xam::NetDll_XNetQosListen_entry)
XAM_EXPORT(__imp__NetDll_inet_addr, rex::kernel::xam::NetDll_inet_addr_entry)
XAM_EXPORT(__imp__NetDll_socket, rex::kernel::xam::NetDll_socket_entry)
XAM_EXPORT(__imp__NetDll_closesocket, rex::kernel::xam::NetDll_closesocket_entry)
XAM_EXPORT(__imp__NetDll_shutdown, rex::kernel::xam::NetDll_shutdown_entry)
XAM_EXPORT(__imp__NetDll_setsockopt, rex::kernel::xam::NetDll_setsockopt_entry)
XAM_EXPORT(__imp__NetDll_ioctlsocket, rex::kernel::xam::NetDll_ioctlsocket_entry)
XAM_EXPORT(__imp__NetDll_bind, rex::kernel::xam::NetDll_bind_entry)
XAM_EXPORT(__imp__NetDll_connect, rex::kernel::xam::NetDll_connect_entry)
XAM_EXPORT(__imp__NetDll_listen, rex::kernel::xam::NetDll_listen_entry)
XAM_EXPORT(__imp__NetDll_accept, rex::kernel::xam::NetDll_accept_entry)
XAM_EXPORT(__imp__NetDll_select, rex::kernel::xam::NetDll_select_entry)
XAM_EXPORT(__imp__NetDll_recv, rex::kernel::xam::NetDll_recv_entry)
XAM_EXPORT(__imp__NetDll_recvfrom, rex::kernel::xam::NetDll_recvfrom_entry)
XAM_EXPORT(__imp__NetDll_send, rex::kernel::xam::NetDll_send_entry)
XAM_EXPORT(__imp__NetDll_sendto, rex::kernel::xam::NetDll_sendto_entry)
XAM_EXPORT(__imp__NetDll___WSAFDIsSet, rex::kernel::xam::NetDll___WSAFDIsSet_entry)
XAM_EXPORT(__imp__NetDll_WSASetLastError, rex::kernel::xam::NetDll_WSASetLastError_entry)
#if REX_HAVE_AC6NET
// Formerly weak XAM_EXPORT_STUBs; without ac6net the stubs below are kept unchanged.
XAM_EXPORT(__imp__NetDll_XNetQosLookup, rex::kernel::xam::NetDll_XNetQosLookup_entry)
XAM_EXPORT(__imp__NetDll_XNetCreateKey, rex::kernel::xam::NetDll_XNetCreateKey_entry)
XAM_EXPORT(__imp__NetDll_XNetRegisterKey, rex::kernel::xam::NetDll_XNetRegisterKey_entry)
XAM_EXPORT(__imp__NetDll_getsockname, rex::kernel::xam::NetDll_getsockname_entry)
XAM_EXPORT(__imp__NetDll_getpeername, rex::kernel::xam::NetDll_getpeername_entry)
XAM_EXPORT(__imp__NetDll_getsockopt, rex::kernel::xam::NetDll_getsockopt_entry)
#endif

XAM_EXPORT_STUB(__imp__NetDll_UpnpActionCalculateWorkBufferSize);
XAM_EXPORT_STUB(__imp__NetDll_UpnpActionCreate);
XAM_EXPORT_STUB(__imp__NetDll_UpnpActionGetResults);
XAM_EXPORT_STUB(__imp__NetDll_UpnpCleanup);
XAM_EXPORT_STUB(__imp__NetDll_UpnpCloseHandle);
XAM_EXPORT_STUB(__imp__NetDll_UpnpDescribeCreate);
XAM_EXPORT_STUB(__imp__NetDll_UpnpDescribeGetResults);
XAM_EXPORT_STUB(__imp__NetDll_UpnpDoWork);
XAM_EXPORT_STUB(__imp__NetDll_UpnpEventCreate);
XAM_EXPORT_STUB(__imp__NetDll_UpnpEventGetCurrentState);
XAM_EXPORT_STUB(__imp__NetDll_UpnpEventUnsubscribe);
XAM_EXPORT_STUB(__imp__NetDll_UpnpSearchCreate);
XAM_EXPORT_STUB(__imp__NetDll_UpnpSearchGetDevices);
XAM_EXPORT_STUB(__imp__NetDll_UpnpStartup);
XAM_EXPORT_STUB(__imp__NetDll_WSACancelOverlappedIO);
XAM_EXPORT_STUB(__imp__NetDll_WSAEventSelect);
XAM_EXPORT_STUB(__imp__NetDll_WSAGetOverlappedResult);
XAM_EXPORT_STUB(__imp__NetDll_WSARecv);
XAM_EXPORT_STUB(__imp__NetDll_WSASend);
XAM_EXPORT_STUB(__imp__NetDll_WSAStartupEx);
XAM_EXPORT_STUB(__imp__NetDll_XHttpCloseHandle);
XAM_EXPORT_STUB(__imp__NetDll_XHttpConnect);
XAM_EXPORT_STUB(__imp__NetDll_XHttpCrackUrl);
XAM_EXPORT_STUB(__imp__NetDll_XHttpCrackUrlW);
XAM_EXPORT_STUB(__imp__NetDll_XHttpCreateUrl);
XAM_EXPORT_STUB(__imp__NetDll_XHttpCreateUrlW);
XAM_EXPORT_STUB(__imp__NetDll_XHttpDoWork);
XAM_EXPORT_STUB(__imp__NetDll_XHttpGetPerfCounters);
XAM_EXPORT_STUB(__imp__NetDll_XHttpOpen);
XAM_EXPORT_STUB(__imp__NetDll_XHttpOpenRequest);
XAM_EXPORT_STUB(__imp__NetDll_XHttpOpenRequestUsingMemory);
XAM_EXPORT_STUB(__imp__NetDll_XHttpQueryAuthSchemes);
XAM_EXPORT_STUB(__imp__NetDll_XHttpQueryHeaders);
XAM_EXPORT_STUB(__imp__NetDll_XHttpQueryOption);
XAM_EXPORT_STUB(__imp__NetDll_XHttpReadData);
XAM_EXPORT_STUB(__imp__NetDll_XHttpReceiveResponse);
XAM_EXPORT_STUB(__imp__NetDll_XHttpResetPerfCounters);
XAM_EXPORT_STUB(__imp__NetDll_XHttpSendRequest);
XAM_EXPORT_STUB(__imp__NetDll_XHttpSetCredentials);
XAM_EXPORT_STUB(__imp__NetDll_XHttpSetOption);
XAM_EXPORT_STUB(__imp__NetDll_XHttpSetStatusCallback);
XAM_EXPORT_STUB(__imp__NetDll_XHttpShutdown);
XAM_EXPORT_STUB(__imp__NetDll_XHttpStartup);
XAM_EXPORT_STUB(__imp__NetDll_XHttpWriteData);
XAM_EXPORT_STUB(__imp__NetDll_XNetConnect);
#if !REX_HAVE_AC6NET
XAM_EXPORT_STUB(__imp__NetDll_XNetCreateKey);
#endif
XAM_EXPORT_STUB(__imp__NetDll_XNetDnsReverseLookup);
XAM_EXPORT_STUB(__imp__NetDll_XNetDnsReverseRelease);
XAM_EXPORT_STUB(__imp__NetDll_XNetGetBroadcastVersionStatus);
XAM_EXPORT_STUB(__imp__NetDll_XNetGetConnectStatus);
XAM_EXPORT_STUB(__imp__NetDll_XNetGetSystemLinkPort);
XAM_EXPORT_STUB(__imp__NetDll_XNetGetXnAddrPlatform);
XAM_EXPORT_STUB(__imp__NetDll_XNetInAddrToServer);
XAM_EXPORT_STUB(__imp__NetDll_XNetQosGetListenStats);
#if !REX_HAVE_AC6NET
XAM_EXPORT_STUB(__imp__NetDll_XNetQosLookup);
XAM_EXPORT_STUB(__imp__NetDll_XNetRegisterKey);
#endif
XAM_EXPORT_STUB(__imp__NetDll_XNetReplaceKey);
XAM_EXPORT_STUB(__imp__NetDll_XNetServerToInAddr);
XAM_EXPORT_STUB(__imp__NetDll_XNetSetOpt);
XAM_EXPORT_STUB(__imp__NetDll_XNetStartupEx);
XAM_EXPORT_STUB(__imp__NetDll_XNetTsAddrToInAddr);
XAM_EXPORT_STUB(__imp__NetDll_XNetUnregisterInAddr);
XAM_EXPORT_STUB(__imp__NetDll_XNetUnregisterKey);
XAM_EXPORT_STUB(__imp__NetDll_XmlDownloadContinue);
XAM_EXPORT_STUB(__imp__NetDll_XmlDownloadGetParseTime);
XAM_EXPORT_STUB(__imp__NetDll_XmlDownloadGetReceivedDataSize);
XAM_EXPORT_STUB(__imp__NetDll_XmlDownloadStart);
XAM_EXPORT_STUB(__imp__NetDll_XmlDownloadStop);
XAM_EXPORT_STUB(__imp__NetDll_XnpCapture);
XAM_EXPORT_STUB(__imp__NetDll_XnpConfig);
XAM_EXPORT_STUB(__imp__NetDll_XnpConfigUPnP);
XAM_EXPORT_STUB(__imp__NetDll_XnpConfigUPnPPortAndExternalAddr);
XAM_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptRecv);
XAM_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetCallbacks);
XAM_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetExtendedReceiveCallback);
XAM_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmit);
XAM_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmitAsIp);
XAM_EXPORT_STUB(__imp__NetDll_XnpGetActiveSocketList);
XAM_EXPORT_STUB(__imp__NetDll_XnpGetConfigStatus);
XAM_EXPORT_STUB(__imp__NetDll_XnpGetKeyList);
XAM_EXPORT_STUB(__imp__NetDll_XnpGetQosLookupList);
XAM_EXPORT_STUB(__imp__NetDll_XnpGetSecAssocList);
XAM_EXPORT_STUB(__imp__NetDll_XnpGetVlanXboxName);
XAM_EXPORT_STUB(__imp__NetDll_XnpLoadConfigParams);
XAM_EXPORT_STUB(__imp__NetDll_XnpLoadMachineAccount);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonClearChallenge);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonClearQEvent);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonGetChallenge);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonGetQFlags);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonGetQVals);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonGetStatus);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonSetChallengeResponse);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonSetPState);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonSetQEvent);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonSetQFlags);
XAM_EXPORT_STUB(__imp__NetDll_XnpLogonSetQVals);
XAM_EXPORT_STUB(__imp__NetDll_XnpNoteSystemTime);
XAM_EXPORT_STUB(__imp__NetDll_XnpPersistTitleState);
XAM_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetAggregateMeasurement);
XAM_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetEntries);
XAM_EXPORT_STUB(__imp__NetDll_XnpQosHistoryLoad);
XAM_EXPORT_STUB(__imp__NetDll_XnpQosHistorySaveMeasurements);
XAM_EXPORT_STUB(__imp__NetDll_XnpRegisterKeyForCallerType);
XAM_EXPORT_STUB(__imp__NetDll_XnpReplaceKeyForCallerType);
XAM_EXPORT_STUB(__imp__NetDll_XnpSaveConfigParams);
XAM_EXPORT_STUB(__imp__NetDll_XnpSaveMachineAccount);
XAM_EXPORT_STUB(__imp__NetDll_XnpSetVlanXboxName);
XAM_EXPORT_STUB(__imp__NetDll_XnpToolIpProxyInject);
XAM_EXPORT_STUB(__imp__NetDll_XnpToolSetCallbacks);
XAM_EXPORT_STUB(__imp__NetDll_XnpUnregisterKeyForCallerType);
XAM_EXPORT_STUB(__imp__NetDll_XnpUpdateConfigParams);
#if !REX_HAVE_AC6NET
XAM_EXPORT_STUB(__imp__NetDll_getpeername);
XAM_EXPORT_STUB(__imp__NetDll_getsockname);
XAM_EXPORT_STUB(__imp__NetDll_getsockopt);
#endif
