// ac6net - XNADDR synthesis/parsing, arbitration machine IDs and the session compat key
// (ARCHITECTURE.md sections 3 and 8). Kept apart from netplay.cpp so these link without the
// rest of the core.
#include <cstring>

#include "ac6net/netplay.h"
#include "crypto.h"

namespace ac6net {

namespace {

constexpr uint8_t kMicrosoftOui[3] = {0x00, 0x22, 0x48};
constexpr uint16_t kXnAddrPortOnline = 1000;
constexpr uint8_t kPlatformXbox360 = 2;

// SGADDR (abOnline) layout, all big-endian: ina[0..4) spi[4..8) xbox_id[8..16)
// platform_type[16] reserved[17..20).
constexpr size_t kSgInaOffset = 0;
constexpr size_t kSgXboxIdOffset = 8;
constexpr size_t kSgPlatformOffset = 16;

void PutBE32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

void PutLE32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

}  // namespace

XnAddr MakeXnAddr(uint32_t vip, PeerId peer_id) {
  XnAddr a;
  a.ina = vip;
  a.ina_online = vip;
  a.port_online = kXnAddrPortOnline;
  a.ab_enet = {kMicrosoftOui[0], kMicrosoftOui[1], kMicrosoftOui[2],
               static_cast<uint8_t>(vip >> 16), static_cast<uint8_t>(vip >> 8),
               static_cast<uint8_t>(vip)};
  PutBE32(a.ab_online.data() + kSgInaOffset, vip);
  // spi stays 0.
  for (int i = 0; i < 8; ++i) {
    a.ab_online[kSgXboxIdOffset + i] = static_cast<uint8_t>(peer_id >> (56 - 8 * i));
  }
  a.ab_online[kSgPlatformOffset] = kPlatformXbox360;
  return a;
}

bool ParseXnAddr(const XnAddr& addr, uint32_t* vip, PeerId* peer_id) {
  const uint32_t v = addr.ina_online ? addr.ina_online : addr.ina;
  if (!IsVip(v)) return false;
  PeerId id = 0;
  for (int i = 0; i < 8; ++i) id = (id << 8) | addr.ab_online[kSgXboxIdOffset + i];
  if (vip) *vip = v;
  if (peer_id) *peer_id = id;
  return true;
}

uint64_t MachineIdFromXnAddr(const XnAddr& addr) {
  uint64_t mac = 0;
  for (uint8_t b : addr.ab_enet) mac = (mac << 8) | b;
  return 0xFA00000000000000ull | mac;
}

// BLAKE2b-128, keyed with 32 zero bytes, over
//   "AC6NET-COMPAT-1" || LE32(title_id) || LE32(media_id) || LE32(xex_version)
//   || LE32(protocol_version) || fps_mode bytes
// returned as 32 lower-case hex characters.
std::string ComputeCompatKey(uint32_t title_id, uint32_t media_id, uint32_t xex_version,
                             uint32_t protocol_version, const std::string& fps_mode) {
  static constexpr char kLabel[] = "AC6NET-COMPAT-1";
  std::string msg(kLabel, sizeof(kLabel) - 1);
  uint8_t fields[16];
  PutLE32(fields + 0, title_id);
  PutLE32(fields + 4, media_id);
  PutLE32(fields + 8, xex_version);
  PutLE32(fields + 12, protocol_version);
  msg.append(reinterpret_cast<const char*>(fields), sizeof(fields));
  msg += fps_mode;
  const uint8_t key[32] = {};
  uint8_t digest[16];
  crypto::Blake2b(digest, sizeof(digest), key, sizeof(key),
                  reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
  return HexEncode(digest, sizeof(digest));
}

}  // namespace ac6net
