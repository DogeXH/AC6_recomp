// ac6net - wire constants and bounds-checked byte readers/writers (PROTOCOL.md section 2).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ac6net::wire {

inline constexpr uint8_t kMagic = 0xA6;
inline constexpr size_t kMaxDatagram = 1400;

// Outer packet types.
inline constexpr uint8_t kHello = 0x01;
inline constexpr uint8_t kHelloAck = 0x02;
inline constexpr uint8_t kC2S = 0x03;
inline constexpr uint8_t kS2C = 0x04;
inline constexpr uint8_t kRehello = 0x05;
inline constexpr uint8_t kRelay = 0x06;
inline constexpr uint8_t kRelayed = 0x07;
inline constexpr uint8_t kP2P = 0x10;

// Header sizes (bytes covered by the AEAD associated data).
inline constexpr size_t kHelloHeader = 50;     // magic type c_pk c_boot ctr
inline constexpr size_t kHelloAckHeader = 18;  // magic type s_boot ctr
inline constexpr size_t kC2SHeader = 26;       // magic type peer_id c_boot ctr
inline constexpr size_t kS2CHeader = 18;       // magic type s_boot ctr
inline constexpr size_t kP2PHeader = 18;       // magic type src_peer_id ctr
inline constexpr size_t kRelayHeader = 18;     // magic type src dst
inline constexpr size_t kRelayedHeader = 10;   // magic type src

// C2S inner kinds.
inline constexpr uint8_t kRpcReq = 0x10;
inline constexpr uint8_t kKeepalive = 0x11;
inline constexpr uint8_t kIntroReq = 0x12;
inline constexpr uint8_t kBye = 0x13;

// S2C inner kinds.
inline constexpr uint8_t kRpcResp = 0x20;
inline constexpr uint8_t kKeepaliveAck = 0x21;
inline constexpr uint8_t kIntro = 0x22;
inline constexpr uint8_t kIntroFail = 0x23;
inline constexpr uint8_t kNotice = 0x24;

// INTRO_REQ selectors.
inline constexpr uint8_t kIntroByVip = 1;
inline constexpr uint8_t kIntroByPeerId = 2;

// P2P inner kinds.
inline constexpr uint8_t kPeerProbe = 0x01;
inline constexpr uint8_t kPeerProbeAck = 0x02;
inline constexpr uint8_t kPeerKeepalive = 0x03;
inline constexpr uint8_t kPeerData = 0x04;
inline constexpr uint8_t kPeerBye = 0x05;

// RPC framing.
inline constexpr size_t kRpcFragment = 1100;
inline constexpr size_t kRpcMaxFragments = 32;

// Largest DATA payload such that a RELAY datagram stays within kMaxDatagram:
//   RELAY header 18 + P2P header 18 + kind 1 + channel 1 + payload + tag 16 <= 1400.
inline constexpr size_t kMaxDataPayload = kMaxDatagram - kRelayHeader - kP2PHeader - 2 - 16;
static_assert(kMaxDataPayload == 1346);

// Bounds-checked little-endian reader. Every getter returns false (and leaves the output alone)
// when fewer bytes remain than requested; nothing ever reads past `end`.
class Reader {
 public:
  Reader(const uint8_t* data, size_t len) : p_(data), end_(data + len) {}

  size_t remaining() const { return static_cast<size_t>(end_ - p_); }
  const uint8_t* cursor() const { return p_; }

  bool U8(uint8_t* v) {
    if (remaining() < 1) return false;
    *v = *p_++;
    return true;
  }
  bool U32(uint32_t* v) {
    if (remaining() < 4) return false;
    uint32_t r = 0;
    for (int i = 3; i >= 0; --i) r = (r << 8) | p_[i];
    p_ += 4;
    *v = r;
    return true;
  }
  bool U32BE(uint32_t* v) {
    if (remaining() < 4) return false;
    *v = (uint32_t{p_[0]} << 24) | (uint32_t{p_[1]} << 16) | (uint32_t{p_[2]} << 8) | p_[3];
    p_ += 4;
    return true;
  }
  bool U64(uint64_t* v) {
    if (remaining() < 8) return false;
    uint64_t r = 0;
    for (int i = 7; i >= 0; --i) r = (r << 8) | p_[i];
    p_ += 8;
    *v = r;
    return true;
  }
  bool Bytes(uint8_t* out, size_t n) {
    if (remaining() < n) return false;
    std::memcpy(out, p_, n);
    p_ += n;
    return true;
  }
  bool Skip(size_t n) {
    if (remaining() < n) return false;
    p_ += n;
    return true;
  }

 private:
  const uint8_t* p_;
  const uint8_t* end_;
};

// Little-endian writer appending to a vector.
class Writer {
 public:
  explicit Writer(std::vector<uint8_t>* out) : out_(out) {}
  void U8(uint8_t v) { out_->push_back(v); }
  void U32(uint32_t v) {
    for (int i = 0; i < 4; ++i) out_->push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  void U32BE(uint32_t v) {
    for (int i = 3; i >= 0; --i) out_->push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  void U64(uint64_t v) {
    for (int i = 0; i < 8; ++i) out_->push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  void Bytes(const uint8_t* p, size_t n) { out_->insert(out_->end(), p, p + n); }
  void Str(const std::string& s) { Bytes(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

 private:
  std::vector<uint8_t>* out_;
};

inline uint64_t LoadU64(const uint8_t* p) {
  uint64_t r = 0;
  for (int i = 7; i >= 0; --i) r = (r << 8) | p[i];
  return r;
}

}  // namespace ac6net::wire
