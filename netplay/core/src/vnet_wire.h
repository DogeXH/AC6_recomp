// ac6net - byte layouts of the vnet DATA channels (PROTOCOL.md 2.8).
//
//   0x01 datagram:  proto u8, src_port u16 BE, dst_port u16 BE, msg_id u16 LE, frag_idx u8,
//                   frag_cnt u8, bytes
//   0x02 stream:    raw KCP segments, conv = conn_id (KCP's own little-endian encoding)
//   0x03 control:   type u8, conn_id u32 LE, src_port u16 BE, dst_port u16 BE
//                   [FIN only: msg_count u32 LE] (vnet addition, see notes/lane_vnet.md)
//
// Every decoder bounds-checks its input and rejects anything malformed.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ac6net::vnet::wire {

inline void PutBe16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
inline uint16_t GetBe16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
inline void PutLe16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
}
inline uint16_t GetLe16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline void PutLe32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
inline uint32_t GetLe32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline void PutLe64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i));
}
inline uint64_t GetLe64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

// ---- Channel 0x01: datagrams ----

inline constexpr size_t kDgramHeader = 9;
inline constexpr uint8_t kMaxFragments = 8;
inline constexpr uint8_t kProtoUdp = 17;
inline constexpr uint8_t kProtoVdp = 254;

struct DgramHeader {
  uint8_t proto = kProtoUdp;
  uint16_t src_port = 0;
  uint16_t dst_port = 0;
  uint16_t msg_id = 0;
  uint8_t frag_idx = 0;
  uint8_t frag_cnt = 1;
};

inline void EncodeDgramHeader(const DgramHeader& h, uint8_t* out) {
  out[0] = h.proto;
  PutBe16(out + 1, h.src_port);
  PutBe16(out + 3, h.dst_port);
  PutLe16(out + 5, h.msg_id);
  out[7] = h.frag_idx;
  out[8] = h.frag_cnt;
}

inline bool DecodeDgramHeader(const uint8_t* data, size_t len, DgramHeader* h) {
  if (!data || len < kDgramHeader) return false;
  h->proto = data[0];
  h->src_port = GetBe16(data + 1);
  h->dst_port = GetBe16(data + 3);
  h->msg_id = GetLe16(data + 5);
  h->frag_idx = data[7];
  h->frag_cnt = data[8];
  if (h->proto != kProtoUdp && h->proto != kProtoVdp) return false;
  if (h->frag_cnt == 0 || h->frag_cnt > kMaxFragments || h->frag_idx >= h->frag_cnt) return false;
  return true;
}

// ---- Channel 0x03: stream control ----

enum class Ctl : uint8_t { kSyn = 1, kSynAck = 2, kRst = 3, kFin = 4, kFinAck = 5 };

inline constexpr size_t kCtlHeader = 9;
inline constexpr size_t kCtlFinSize = kCtlHeader + 4;
inline constexpr size_t kCtlMaxSize = kCtlFinSize;

struct CtlMsg {
  Ctl type = Ctl::kRst;
  uint32_t conn_id = 0;
  uint16_t src_port = 0;
  uint16_t dst_port = 0;
  uint32_t msg_count = 0;  // FIN only: number of stream messages the sender sent in total
};

// Writes the message into `out` (at least kCtlMaxSize bytes) and returns its length.
inline size_t EncodeCtl(const CtlMsg& m, uint8_t* out) {
  out[0] = uint8_t(m.type);
  PutLe32(out + 1, m.conn_id);
  PutBe16(out + 5, m.src_port);
  PutBe16(out + 7, m.dst_port);
  if (m.type != Ctl::kFin) return kCtlHeader;
  PutLe32(out + 9, m.msg_count);
  return kCtlFinSize;
}

// Trailing bytes beyond the known layout are ignored (forward compatible). A FIN without the
// count field decodes with msg_count = 0, i.e. "EOF once the queued data is drained".
inline bool DecodeCtl(const uint8_t* data, size_t len, CtlMsg* m) {
  if (!data || len < kCtlHeader) return false;
  if (data[0] < uint8_t(Ctl::kSyn) || data[0] > uint8_t(Ctl::kFinAck)) return false;
  m->type = Ctl(data[0]);
  m->conn_id = GetLe32(data + 1);
  m->src_port = GetBe16(data + 5);
  m->dst_port = GetBe16(data + 7);
  m->msg_count = (m->type == Ctl::kFin && len >= kCtlFinSize) ? GetLe32(data + 9) : 0;
  return true;
}

// ---- Channel 0x02: KCP segments ----

inline constexpr size_t kKcpOverhead = 24;
inline constexpr uint8_t kKcpCmdPush = 81;
inline constexpr uint8_t kKcpCmdAck = 82;
inline constexpr uint8_t kKcpCmdWask = 83;
inline constexpr uint8_t kKcpCmdWins = 84;
inline constexpr uint8_t kKcpMaxFrg = 127;      // < KCP's IKCP_WND_RCV; our messages use <= 28
inline constexpr int32_t kKcpMaxRttMs = 60000;  // KCP's IKCP_RTO_MAX

// Validates a KCP packet before it reaches ikcp_input. KCP 1.7 trusts its input: an ACK whose
// echoed timestamp is far in the past makes ikcp_update_ack overflow int32 arithmetic, and an
// absurd fragment count stalls reassembly. Rejects: foreign conv, unknown commands, segment
// lengths beyond the packet, trailing bytes, frg > kKcpMaxFrg and ACK ages > kKcpMaxRttMs
// measured against `kcp_now` (the control block's `current`, which KCP itself uses).
inline bool KcpPacketSane(const uint8_t* data, size_t len, uint32_t conv, uint32_t kcp_now) {
  if (!data || len < kKcpOverhead) return false;
  size_t off = 0;
  while (len - off >= kKcpOverhead) {
    const uint8_t* seg = data + off;
    const uint8_t cmd = seg[4];
    const uint8_t frg = seg[5];
    const uint32_t ts = GetLe32(seg + 8);
    const uint32_t seg_len = GetLe32(seg + 20);
    if (GetLe32(seg) != conv) return false;
    if (cmd != kKcpCmdPush && cmd != kKcpCmdAck && cmd != kKcpCmdWask && cmd != kKcpCmdWins) {
      return false;
    }
    if (seg_len > len - off - kKcpOverhead) return false;
    if (cmd == kKcpCmdPush && frg > kKcpMaxFrg) return false;
    if (cmd == kKcpCmdAck && int32_t(kcp_now - ts) > kKcpMaxRttMs) return false;
    off += kKcpOverhead + seg_len;
  }
  return off == len;
}

// True if a well-formed KCP packet carries at least one data (PUSH) segment. Used to decide
// whether a packet for an unknown connection deserves an RST (pure ACKs never do).
inline bool KcpPacketHasPush(const uint8_t* data, size_t len) {
  size_t off = 0;
  while (len - off >= kKcpOverhead) {
    const uint8_t cmd = data[off + 4];
    const uint32_t seg_len = GetLe32(data + off + 20);
    if (cmd == kKcpCmdPush) return true;
    if (seg_len > len - off - kKcpOverhead) return false;
    off += kKcpOverhead + seg_len;
  }
  return false;
}

}  // namespace ac6net::vnet::wire
