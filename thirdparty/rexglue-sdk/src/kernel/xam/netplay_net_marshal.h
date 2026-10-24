/**
 * @file        netplay_net_marshal.h
 * @brief       Guest <-> host marshalling for the AC6 netplay NetDll/XNet glue.
 *
 * Pure functions over raw guest bytes (big-endian, as they sit in guest memory) and the
 * host-side ac6net types. No rex runtime dependencies, so the logic is unit-testable
 * without a kernel (see netplay_net_test.cpp).
 *
 * Guest layouts follow Xenia (BSD-3, Copyright Ben Vanik and the Xenia Canary team):
 * src/xenia/kernel/xam/xam_net.cc and src/xenia/kernel/xnet.h.
 *
 * @license     BSD 3-Clause License
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <ac6net/types.h>
#include <ac6net/vnet.h>

namespace rex::kernel::xam::netplay_net {

// ---------------------------------------------------------------------------
// Socket handles. A vnet socket id `n` is exposed to the guest as 0x1000 + n. The XObject
// table hands out handles from 0xF8000000 upward, so this range never collides with it.
// ---------------------------------------------------------------------------
inline constexpr uint32_t kSocketHandleBase = 0x00001000;
inline constexpr uint32_t kSocketHandleEnd = 0x10000000;  // exclusive

inline bool IsVnetHandle(uint32_t handle) {
  return handle >= kSocketHandleBase && handle < kSocketHandleEnd;
}
// 0 if `id` cannot be represented as a guest handle.
inline uint32_t HandleFromId(ac6net::vnet::SocketId id) {
  if (id <= 0 || static_cast<uint32_t>(id) >= kSocketHandleEnd - kSocketHandleBase) {
    return 0;
  }
  return kSocketHandleBase + static_cast<uint32_t>(id);
}
// Only meaningful when IsVnetHandle(handle). Handle 0x1000 maps to id 0, which vnet rejects
// with WSAENOTSOCK.
inline ac6net::vnet::SocketId IdFromHandle(uint32_t handle) {
  return static_cast<ac6net::vnet::SocketId>(handle - kSocketHandleBase);
}

// ---------------------------------------------------------------------------
// Guest constants.
// ---------------------------------------------------------------------------
inline constexpr uint16_t kAfInet = 2;
inline constexpr uint32_t kSockaddrInSize = 16;  // {u16 family, u16 port, u32 addr, 8 zero}
inline constexpr uint32_t kXnAddrSize = 36;
inline constexpr uint32_t kXnKidSize = 8;
inline constexpr uint32_t kXnKeySize = 16;
inline constexpr uint32_t kFdSetSize = 64;                   // FD_SETSIZE on the 360
inline constexpr uint32_t kFdSetBytes = 4 + 4 * kFdSetSize;  // x_fd_set
inline constexpr uint32_t kTimevalBytes = 8;                 // {be i32 sec, be i32 usec}

inline constexpr uint32_t kFionbio = 0x8004667E;
inline constexpr uint32_t kFionread = 0x4004667F;

// Xbox-only socket options, accepted and ignored (there is no XNet security layer).
inline constexpr int kSoMarkInsecure = 0x5801;
inline constexpr int kSoPrivate = 0x5802;
inline constexpr int kSoGrantInsecure = 0x5803;

// XNetGetTitleXnAddr status bits.
inline constexpr uint32_t kXnAddrNone = 0x01;
inline constexpr uint32_t kXnAddrEthernet = 0x02;
inline constexpr uint32_t kXnAddrStatic = 0x04;
inline constexpr uint32_t kXnAddrGateway = 0x20;
inline constexpr uint32_t kXnAddrDns = 0x40;
inline constexpr uint32_t kXnAddrOnline = 0x80;
inline constexpr uint32_t kXnAddrStatusOnline =
    kXnAddrOnline | kXnAddrDns | kXnAddrGateway | kXnAddrStatic;                   // 0xE4
inline constexpr uint32_t kXnAddrStatusOffline = kXnAddrEthernet | kXnAddrStatic;  // 0x06

// XNetGetEthernetLinkStatus bits.
inline constexpr uint32_t kEthernetLinkActive = 0x01;
inline constexpr uint32_t kEthernetLink100Mbps = 0x02;
inline constexpr uint32_t kEthernetLinkFullDuplex = 0x08;

// ---------------------------------------------------------------------------
// Big-endian access. Endian-agnostic shifts, no alignment requirements.
// ---------------------------------------------------------------------------
inline uint16_t LoadBe16(const uint8_t* p) {
  return static_cast<uint16_t>((uint32_t{p[0]} << 8) | p[1]);
}
inline uint32_t LoadBe32(const uint8_t* p) {
  return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}
inline uint64_t LoadBe64(const uint8_t* p) {
  return (uint64_t{LoadBe32(p)} << 32) | LoadBe32(p + 4);
}
inline void StoreBe16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}
inline void StoreBe32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}
inline void StoreBe64(uint8_t* p, uint64_t v) {
  StoreBe32(p, static_cast<uint32_t>(v >> 32));
  StoreBe32(p + 4, static_cast<uint32_t>(v));
}

// True if [guest_addr, guest_addr + len) is a non-null range inside the 32-bit guest space.
// An empty range is always valid.
inline bool GuestRangeOk(uint32_t guest_addr, uint64_t len) {
  if (len == 0)
    return true;
  return guest_addr != 0 && uint64_t{guest_addr} + len <= (uint64_t{1} << 32);
}

// ---------------------------------------------------------------------------
// SOCKADDR_IN: u16 BE family, u16 BE port, u32 BE address, 8 zero bytes.
// ---------------------------------------------------------------------------

// Returns a WSA code. `p` may be null (WSAEFAULT).
inline int ParseSockaddrIn(const uint8_t* p, uint32_t len, ac6net::vnet::Endpoint* out) {
  if (!p || len < kSockaddrInSize)
    return ac6net::wsa::kFault;
  if (LoadBe16(p) != kAfInet)
    return ac6net::wsa::kAfNoSupport;
  out->port = LoadBe16(p + 2);
  out->ip = LoadBe32(p + 4);
  return ac6net::wsa::kOk;
}

inline void StoreSockaddrIn(uint8_t* p, const ac6net::vnet::Endpoint& ep) {
  StoreBe16(p, kAfInet);
  StoreBe16(p + 2, ep.port);
  StoreBe32(p + 4, ep.ip);
  std::memset(p + 8, 0, 8);
}

// Output sockaddr (accept, getsockname, getpeername, recvfrom). Winsock: a null address
// means "do not report"; a non-null address needs a length of at least sizeof(SOCKADDR_IN).
inline int CheckSockaddrOut(bool have_addr, bool have_len, uint32_t len) {
  if (!have_addr)
    return ac6net::wsa::kOk;
  if (!have_len || len < kSockaddrInSize)
    return ac6net::wsa::kFault;
  return ac6net::wsa::kOk;
}

// ---------------------------------------------------------------------------
// XNADDR (36 bytes): ina(4) inaOnline(4) wPortOnline(2) abEnet(6) abOnline(20), all BE.
// ---------------------------------------------------------------------------
inline ac6net::XnAddr ReadXnAddr(const uint8_t* p) {
  ac6net::XnAddr a;
  a.ina = LoadBe32(p + 0);
  a.ina_online = LoadBe32(p + 4);
  a.port_online = LoadBe16(p + 8);
  std::memcpy(a.ab_enet.data(), p + 10, a.ab_enet.size());
  std::memcpy(a.ab_online.data(), p + 16, a.ab_online.size());
  return a;
}

inline void WriteXnAddr(uint8_t* p, const ac6net::XnAddr& a) {
  StoreBe32(p + 0, a.ina);
  StoreBe32(p + 4, a.ina_online);
  StoreBe16(p + 8, a.port_online);
  std::memcpy(p + 10, a.ab_enet.data(), a.ab_enet.size());
  std::memcpy(p + 16, a.ab_online.data(), a.ab_online.size());
}

static_assert(10 + 6 + 20 == kXnAddrSize);

// XNKID and XNKEY are opaque bytes kept in guest order.
inline ac6net::XnKid ReadXnKid(const uint8_t* p) {
  ac6net::XnKid k;
  std::memcpy(k.b.data(), p, k.b.size());
  return k;
}
inline void WriteXnKid(uint8_t* p, const ac6net::XnKid& k) {
  std::memcpy(p, k.b.data(), k.b.size());
}

// ---------------------------------------------------------------------------
// x_fd_set: u32 BE count, then 64 u32 BE handles.
// ---------------------------------------------------------------------------

// Returns a WSA code; a count above FD_SETSIZE is rejected (WSAEINVAL) instead of read past.
inline int ReadFdSet(const uint8_t* p, std::vector<uint32_t>* handles) {
  handles->clear();
  const uint32_t count = LoadBe32(p);
  if (count > kFdSetSize)
    return ac6net::wsa::kInval;
  handles->reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    handles->push_back(LoadBe32(p + 4 + 4 * i));
  }
  return ac6net::wsa::kOk;
}

// Caller guarantees handles.size() <= kFdSetSize. Slots past the count are left untouched,
// as FD_ZERO/FD_SET on the guest would leave them.
inline void WriteFdSet(uint8_t* p, const std::vector<uint32_t>& handles) {
  const uint32_t count =
      static_cast<uint32_t>(handles.size() < kFdSetSize ? handles.size() : kFdSetSize);
  StoreBe32(p, count);
  for (uint32_t i = 0; i < count; ++i) {
    StoreBe32(p + 4 + 4 * i, handles[i]);
  }
}

// Winsock rewrites each set to its ready members. `guest` is the set as passed in (order
// kept, duplicates kept); `ready` is what vnet reported ready.
inline std::vector<uint32_t> FilterReady(const std::vector<uint32_t>& guest,
                                         const std::vector<ac6net::vnet::SocketId>& ready) {
  std::vector<uint32_t> out;
  for (uint32_t h : guest) {
    if (!IsVnetHandle(h))
      continue;
    const ac6net::vnet::SocketId id = IdFromHandle(h);
    for (ac6net::vnet::SocketId r : ready) {
      if (r == id) {
        out.push_back(h);
        break;
      }
    }
  }
  return out;
}

// Guest handles -> vnet ids. Returns WSAENOTSOCK if a handle is outside the vnet range.
inline int HandlesToIds(const std::vector<uint32_t>& handles,
                        std::vector<ac6net::vnet::SocketId>* ids) {
  ids->clear();
  ids->reserve(handles.size());
  for (uint32_t h : handles) {
    if (!IsVnetHandle(h))
      return ac6net::wsa::kNotSock;
    ids->push_back(IdFromHandle(h));
  }
  return ac6net::wsa::kOk;
}

// timeval {be i32 sec, be i32 usec} (already scaled to host time by the caller) -> micros.
// Negative values are invalid (WSAEINVAL).
inline int TimevalToMicros(int32_t sec, int32_t usec, int64_t* out_us) {
  if (sec < 0 || usec < 0)
    return ac6net::wsa::kInval;
  *out_us = int64_t{sec} * 1000000 + usec;
  return ac6net::wsa::kOk;
}

// select over guest x_fd_sets. `sets` are host pointers to the read, write and except sets
// (kFdSetBytes each, already range-checked), or null for a set that was not passed or that
// aliases an earlier one.
struct SelectPlan {
  bool vnet = false;           // false: no vnet handle and some XSocket handle -> XSocket path
  int err = ac6net::wsa::kOk;  // vnet path only: fail with this before calling vnet
  std::vector<uint32_t> handles[3];
  ac6net::vnet::SelectSets query;
};

inline SelectPlan PlanSelect(const uint8_t* const sets[3]) {
  SelectPlan plan;
  bool any_vnet = false;
  bool any_other = false;
  for (int i = 0; i < 3; ++i) {
    if (!sets[i])
      continue;
    const int err = ReadFdSet(sets[i], &plan.handles[i]);
    if (err != ac6net::wsa::kOk && plan.err == ac6net::wsa::kOk)
      plan.err = err;
    for (uint32_t h : plan.handles[i]) {
      if (IsVnetHandle(h)) {
        any_vnet = true;
      } else {
        any_other = true;
      }
    }
  }
  // Only XSocket handles: not ours. Empty sets with netplay on are still vnet's to answer.
  plan.vnet = any_vnet || !any_other;
  if (!plan.vnet || plan.err != ac6net::wsa::kOk)
    return plan;
  std::vector<ac6net::vnet::SocketId>* queries[3] = {&plan.query.read, &plan.query.write,
                                                     &plan.query.except};
  for (int i = 0; i < 3; ++i) {
    const int err = HandlesToIds(plan.handles[i], queries[i]);
    if (err != ac6net::wsa::kOk) {
      plan.err = err;  // a vnet select that also names an XSocket handle
      break;
    }
  }
  return plan;
}

// Rewrites each passed set to its ready members (Winsock semantics) and returns the total.
inline int FinishSelect(uint8_t* const sets[3], const SelectPlan& plan,
                        const ac6net::vnet::SelectSets& ready) {
  const std::vector<ac6net::vnet::SocketId>* results[3] = {&ready.read, &ready.write,
                                                           &ready.except};
  int total = 0;
  for (int i = 0; i < 3; ++i) {
    if (!sets[i])
      continue;
    const std::vector<uint32_t> out = FilterReady(plan.handles[i], *results[i]);
    WriteFdSet(sets[i], out);
    total += static_cast<int>(out.size());
  }
  return total;
}

// ---------------------------------------------------------------------------
// Socket option values. The guest passes integers big-endian in `optlen` bytes; vnet takes
// and returns host-endian integers of the same width. SO_LINGER is {u16 onoff, u16 linger}.
// Other widths are opaque and copied as-is.
// ---------------------------------------------------------------------------
inline constexpr uint32_t kMaxOptionBytes = 64;

inline void SwapOptionValue(int level, int option, const uint8_t* src, uint8_t* dst, size_t len,
                            bool guest_to_host) {
  if (level == ac6net::vnet::kSolSocket && option == ac6net::vnet::kSoLinger && len == 4) {
    SwapOptionValue(0, 0, src, dst, 2, guest_to_host);
    SwapOptionValue(0, 0, src + 2, dst + 2, 2, guest_to_host);
    return;
  }
  switch (len) {
    case 1:
      dst[0] = src[0];
      return;
    case 2: {
      uint16_t v;
      if (guest_to_host) {
        v = LoadBe16(src);
        std::memcpy(dst, &v, 2);
      } else {
        std::memcpy(&v, src, 2);
        StoreBe16(dst, v);
      }
      return;
    }
    case 4: {
      uint32_t v;
      if (guest_to_host) {
        v = LoadBe32(src);
        std::memcpy(dst, &v, 4);
      } else {
        std::memcpy(&v, src, 4);
        StoreBe32(dst, v);
      }
      return;
    }
    case 8: {
      uint64_t v;
      if (guest_to_host) {
        v = LoadBe64(src);
        std::memcpy(dst, &v, 8);
      } else {
        std::memcpy(&v, src, 8);
        StoreBe64(dst, v);
      }
      return;
    }
    default:
      if (len)
        std::memmove(dst, src, len);
      return;
  }
}

inline bool IsIgnoredXboxOption(int level, int option) {
  return level == ac6net::vnet::kSolSocket &&
         (option == kSoMarkInsecure || option == kSoPrivate || option == kSoGrantInsecure);
}

// XNKID type byte for XNetCreateKey: 0x00 = system link (Xenia GenerateSessionId(0)).
inline void MakeSystemLinkKid(ac6net::XnKid* kid) {
  kid->b[0] = 0x00;
}

}  // namespace rex::kernel::xam::netplay_net
