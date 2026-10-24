// ac6net - AC6 recomp online core library.
// Plain host-side types shared by the core and the SDK glue. No rex/guest types here:
// the glue converts guest (big-endian) structures to and from these.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ac6net {

using Xuid = uint64_t;
using PeerId = uint64_t;

// XNKID: 8 opaque bytes kept in guest byte order (b[0] is the type byte; 0x80 = online peer).
struct XnKid {
  std::array<uint8_t, 8> b{};
  bool operator==(const XnKid&) const = default;
  bool IsZero() const {
    for (auto v : b)
      if (v) return false;
    return true;
  }
};

// XNKEY: 16 opaque bytes.
struct XnKey {
  std::array<uint8_t, 16> b{};
  bool operator==(const XnKey&) const = default;
};

// Host-endian mirror of the guest XNADDR (36 bytes in guest memory, big-endian fields).
//   guest layout: ina(4) inaOnline(4) wPortOnline(2) abEnet(6) abOnline(20)
struct XnAddr {
  uint32_t ina = 0;         // IPv4 in host order, e.g. 0x0A400007 == 10.64.0.7
  uint32_t ina_online = 0;  // IPv4 in host order
  uint16_t port_online = 0;
  std::array<uint8_t, 6> ab_enet{};
  std::array<uint8_t, 20> ab_online{};  // raw guest bytes (SGADDR; see netplay.h helpers)
  bool operator==(const XnAddr&) const = default;
};

// XSESSION_INFO (60 bytes in guest memory): sessionID, hostAddress, keyExchangeKey.
struct SessionInfo {
  XnKid session_id;
  XnAddr host;
  XnKey key;
};

// XUSER_DATA_TYPE values.
enum class DataType : uint8_t {
  kContext = 0,
  kInt32 = 1,
  kInt64 = 2,
  kDouble = 3,
  kUnicode = 4,  // bytes = guest UTF-16BE including terminator
  kFloat = 5,
  kBinary = 6,
  kDateTime = 7,
  kNull = 0xFF,
};

// XUSER_DATA value (host-side). Exactly one of i/d/bytes is meaningful depending on type.
struct UserData {
  DataType type = DataType::kNull;
  int64_t i = 0;              // kInt32 (sign-extended), kInt64, kDateTime, kContext
  double d = 0.0;             // kDouble, kFloat
  std::vector<uint8_t> bytes;  // kUnicode, kBinary
  bool operator==(const UserData&) const = default;
};

struct Property {  // XUSER_PROPERTY
  uint32_t id = 0;
  UserData value;
  bool operator==(const Property&) const = default;
};

struct Context {  // XUSER_CONTEXT
  uint32_t id = 0;
  uint32_t value = 0;
  bool operator==(const Context&) const = default;
};

// Well-known IDs used by AC6.
inline constexpr uint32_t kContextGameType = 0x0000800A;  // 0 = ranked, 1 = standard (player)
inline constexpr uint32_t kContextGameMode = 0x0000800B;
inline constexpr uint32_t kContextPresence = 0x00008001;

// Winsock error codes reported through WSAGetLastError.
namespace wsa {
inline constexpr int kOk = 0;
inline constexpr int kInterrupted = 10004;    // WSAEINTR
inline constexpr int kBadF = 10009;           // WSAEBADF
inline constexpr int kAccess = 10013;         // WSAEACCES
inline constexpr int kFault = 10014;          // WSAEFAULT
inline constexpr int kInval = 10022;          // WSAEINVAL
inline constexpr int kWouldBlock = 10035;     // WSAEWOULDBLOCK
inline constexpr int kInProgress = 10036;     // WSAEINPROGRESS
inline constexpr int kAlready = 10037;        // WSAEALREADY
inline constexpr int kNotSock = 10038;        // WSAENOTSOCK
inline constexpr int kDestAddrReq = 10039;    // WSAEDESTADDRREQ
inline constexpr int kMsgSize = 10040;        // WSAEMSGSIZE
inline constexpr int kProtoType = 10041;      // WSAEPROTOTYPE
inline constexpr int kNoProtoOpt = 10042;     // WSAENOPROTOOPT
inline constexpr int kProtoNoSupport = 10043; // WSAEPROTONOSUPPORT
inline constexpr int kOpNotSupp = 10045;      // WSAEOPNOTSUPP
inline constexpr int kAfNoSupport = 10047;    // WSAEAFNOSUPPORT
inline constexpr int kAddrInUse = 10048;      // WSAEADDRINUSE
inline constexpr int kAddrNotAvail = 10049;   // WSAEADDRNOTAVAIL
inline constexpr int kNetDown = 10050;        // WSAENETDOWN
inline constexpr int kNetUnreach = 10051;     // WSAENETUNREACH
inline constexpr int kConnAborted = 10053;    // WSAECONNABORTED
inline constexpr int kConnReset = 10054;      // WSAECONNRESET
inline constexpr int kNoBufs = 10055;         // WSAENOBUFS
inline constexpr int kIsConn = 10056;         // WSAEISCONN
inline constexpr int kNotConn = 10057;        // WSAENOTCONN
inline constexpr int kShutdown = 10058;       // WSAESHUTDOWN
inline constexpr int kTimedOut = 10060;       // WSAETIMEDOUT
inline constexpr int kConnRefused = 10061;    // WSAECONNREFUSED
inline constexpr int kHostUnreach = 10065;    // WSAEHOSTUNREACH
inline constexpr int kNotInitialised = 10093; // WSANOTINITIALISED
}  // namespace wsa

// HRESULT values returned to the guest by the xlive layer (from Xenia's xnet.h).
namespace hr {
inline constexpr uint32_t kOk = 0x00000000;
inline constexpr uint32_t kFail = 0x80004005;                    // E_FAIL
inline constexpr uint32_t kInvalidArg = 0x80070057;              // E_INVALIDARG
inline constexpr uint32_t kInsufficientBuffer = 0x8007007A;      // HRESULT_FROM_WIN32(122)
inline constexpr uint32_t kNotLoggedOn = 0x80151802;             // X_ONLINE_E_LOGON_NOT_LOGGED_ON
inline constexpr uint32_t kLogonConnectionEstablished = 0x001510F0;
inline constexpr uint32_t kLogonDisconnected = 0x001510F1;
inline constexpr uint32_t kSessionNotFound = 0x80155200;
inline constexpr uint32_t kSessionFull = 0x80155202;
inline constexpr uint32_t kSessionInvalidFlags = 0x80155204;
inline constexpr uint32_t kSessionRequiresArbitration = 0x80155205;
inline constexpr uint32_t kSessionWrongState = 0x80155206;
inline constexpr uint32_t kSessionInsufficientBuffer = 0x80155207;
inline constexpr uint32_t kSessionNotLoggedOn = 0x80155209;
inline constexpr uint32_t kSessionJoinIllegal = 0x8015520A;
inline constexpr uint32_t kSessionNotRegistered = 0x8015520C;
inline constexpr uint32_t kStorageFileNotFound = 0x8015C004;
}  // namespace hr

// XSESSION_CREATE_* flags.
namespace sflag {
inline constexpr uint32_t kHost = 0x001;
inline constexpr uint32_t kUsesPresence = 0x002;
inline constexpr uint32_t kUsesStats = 0x004;
inline constexpr uint32_t kUsesMatchmaking = 0x008;
inline constexpr uint32_t kUsesArbitration = 0x010;
inline constexpr uint32_t kUsesPeerNetwork = 0x020;
inline constexpr uint32_t kSocialMatchmakingAllowed = 0x080;
inline constexpr uint32_t kInvitesDisabled = 0x100;
inline constexpr uint32_t kJoinViaPresenceDisabled = 0x200;
inline constexpr uint32_t kJoinInProgressDisabled = 0x400;
inline constexpr uint32_t kJoinViaPresenceFriendsOnly = 0x800;
}  // namespace sflag

// Helpers.
std::string HexEncode(const uint8_t* data, size_t len);  // lower-case
bool HexDecode(const std::string& hex, uint8_t* out, size_t out_len);
std::string Ipv4ToString(uint32_t ip_host_order);
bool Ipv4FromString(const std::string& s, uint32_t* ip_host_order);

}  // namespace ac6net
