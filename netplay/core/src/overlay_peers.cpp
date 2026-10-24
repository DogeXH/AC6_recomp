// ac6net - overlay peers (PROTOCOL.md 2.6-2.8): keys, P2P packets, relay, path probing and
// selection, keepalive, loss detection, and the queues for not-yet-introduced peers.
#include <algorithm>
#include <cstring>

#include "ac6net/log.h"
#include "overlay.h"
#include "overlay_internal.h"

namespace ac6net {

namespace lim = overlay_limits;

namespace {

void StoreU64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

// Same-type candidates must beat the active path's smoothed RTT by max(5 ms, 25 %), and the
// active path must have been in use for 3 s, before we switch (no flapping under jitter). The
// dwell is well inside the 10 s fast probe phase, so a faster path found then is still adopted.
// A better candidate type, or losing the active path, switches at once.
constexpr uint32_t kSwitchMarginMs = 5;
constexpr uint64_t kMinPathDwellMs = 3'000;

}  // namespace

// --- Peer table ---

Peer* Overlay::Impl::FindPeerLocked(PeerId id) {
  auto it = peers.find(id);
  return it == peers.end() ? nullptr : it->second.get();
}

Peer* Overlay::Impl::FindPeerByVipLocked(uint32_t vip) {
  auto it = vip_to_peer.find(vip);
  return it == vip_to_peer.end() ? nullptr : FindPeerLocked(it->second);
}

Peer* Overlay::Impl::UpsertPeerLocked(PeerId id, uint32_t vip, Xuid xuid,
                                      const std::string& gamertag,
                                      const crypto::PublicKey& peer_pk, const BootId& peer_boot,
                                      uint64_t now, Events* ev) {
  if (Peer* existing = FindPeerLocked(id)) {
    if (existing->pk == peer_pk && existing->boot == peer_boot) {
      if (existing->vip != vip) {
        auto old = vip_to_peer.find(existing->vip);
        if (old != vip_to_peer.end() && old->second == id) vip_to_peer.erase(old);
        existing->vip = vip;
      }
      vip_to_peer[vip] = id;
      existing->xuid = xuid;
      existing->gamertag = gamertag;
      return existing;
    }
    // The peer restarted (new boot) or changed keys: its old streams are gone.
    LogF(LogLevel::kInfo, "overlay: peer %s restarted; replacing its keys",
         U64ToHex(id).c_str());
    ev->lost.push_back(existing->vip);
    RemovePeerLocked(id);
  }
  if (peers.size() >= lim::kMaxPeers) {
    if (counters.peer_table_full++ % 100 == 0) {
      LogF(LogLevel::kWarn, "overlay: peer table full (%zu); ignoring INTRO", peers.size());
    }
    return nullptr;
  }
  crypto::Key dh;
  if (!crypto::Dh(sk, peer_pk, &dh)) {
    ++counters.malformed;
    return nullptr;
  }
  auto peer = std::make_unique<Peer>();
  peer->peer_id = id;
  peer->vip = vip;
  peer->xuid = xuid;
  peer->gamertag = gamertag;
  peer->pk = peer_pk;
  peer->boot = peer_boot;
  peer->send_key = crypto::Kdf(dh, "AC6NET-P2P-1", {pk, peer_pk, boot, peer_boot});
  peer->recv_key = crypto::Kdf(dh, "AC6NET-P2P-1", {peer_pk, pk, peer_boot, boot});
  crypto::Wipe(dh.data(), dh.size());
  if (auto t = tombstones.find(id); t != tombstones.end()) {
    if (t->second.boot == peer_boot && t->second.pk == peer_pk) {
      peer->send_ctr = t->second.send_ctr;
      peer->recv_window.Reset(t->second.recv_highest);
    }
    tombstones.erase(t);
  }
  peer->last_recv_ms = now;
  peer->probe_phase_ms = now;
  // Stale mapping from an older record that claimed this vip.
  if (auto old = vip_to_peer.find(vip); old != vip_to_peer.end() && old->second != id) {
    if (Peer* other = FindPeerLocked(old->second)) other->vip = 0;
  }
  vip_to_peer[vip] = id;
  Peer* raw = peer.get();
  peers.emplace(id, std::move(peer));
  Trace("peer_new", "\"vip\":\"" + Ipv4ToString(vip) + "\",\"peer_id\":\"" + U64ToHex(id) +
                        "\",\"gamertag\":\"" + JsonEscape(gamertag) + "\"");
  return raw;
}

void Overlay::Impl::RemovePeerLocked(PeerId id) {
  auto it = peers.find(id);
  if (it == peers.end()) return;
  Peer& p = *it->second;
  tombstones[id] = PeerTombstone{p.boot, p.pk, p.send_ctr, p.recv_window.highest()};
  auto v = vip_to_peer.find(p.vip);
  if (v != vip_to_peer.end() && v->second == id) vip_to_peer.erase(v);
  crypto::Wipe(p.send_key.data(), p.send_key.size());
  crypto::Wipe(p.recv_key.data(), p.recv_key.size());
  peers.erase(it);
}

int Overlay::Impl::FindCandidateLocked(const Peer& peer, const net::Addr& addr) const {
  for (size_t i = 0; i < peer.cands.size(); ++i)
    if (peer.cands[i].addr == addr) return static_cast<int>(i);
  return -1;
}

void Overlay::Impl::MergeCandidateLocked(Peer& peer, const net::Addr& addr, CandType type) {
  const int i = FindCandidateLocked(peer, addr);
  if (i >= 0) {
    Candidate& c = peer.cands[static_cast<size_t>(i)];
    if (type < c.type) c.type = type;  // keep the best classification
    return;
  }
  if (peer.cands.size() >= lim::kMaxCandidates) return;
  if (type == CandType::kPrflx) {
    const auto n = std::count_if(peer.cands.begin(), peer.cands.end(),
                                 [](const Candidate& c) { return c.type == CandType::kPrflx; });
    if (static_cast<size_t>(n) >= lim::kMaxPrflx) return;
  }
  Candidate c;
  c.addr = addr;
  c.type = type;
  peer.cands.push_back(c);
}

// --- Sending ---

bool Overlay::Impl::SendP2PLocked(Peer& peer, uint8_t kind, const uint8_t* body, size_t len,
                                  Route route, const net::Addr* to, uint64_t now) {
  if (!reg.valid) return false;
  const size_t inner_len = 1 + len;
  const size_t p2p_len = wire::kP2PHeader + inner_len + crypto::kTagSize;
  if (wire::kRelayHeader + p2p_len > wire::kMaxDatagram) return false;

  Route r = route;
  const net::Addr* dst = to;
  if (r == Route::kBest) {
    if (direct_allowed && peer.active >= 0) {
      r = Route::kDirect;
      dst = &peer.cands[static_cast<size_t>(peer.active)].addr;
    } else {
      r = Route::kRelay;
    }
  }
  if (r == Route::kDirect && (!dst || !direct_allowed)) return false;
  if (r == Route::kRelay && !server_resolved) return false;

  // Built in place after room for the RELAY header so relaying needs no copy.
  uint8_t buf[wire::kMaxDatagram];
  uint8_t* pkt = buf + wire::kRelayHeader;
  pkt[0] = wire::kMagic;
  pkt[1] = wire::kP2P;
  StoreU64(pkt + 2, reg.peer_id);
  const uint64_t ctr = ++peer.send_ctr;
  StoreU64(pkt + 10, ctr);
  uint8_t* inner = pkt + wire::kP2PHeader;
  inner[0] = kind;
  if (len) std::memcpy(inner + 1, body, len);
  crypto::Seal(peer.send_key, ctr, pkt, wire::kP2PHeader, inner, inner_len, inner);

  bool ok;
  if (r == Route::kDirect) {
    ok = SendRawLocked(*dst, pkt, p2p_len);
  } else {
    buf[0] = wire::kMagic;
    buf[1] = wire::kRelay;
    StoreU64(buf + 2, reg.peer_id);
    StoreU64(buf + 10, peer.peer_id);
    ok = SendRawLocked(server_addr, buf, wire::kRelayHeader + p2p_len);
  }
  // Probes and replies to a specific address do not count as traffic on the active route.
  if (route == Route::kBest) peer.last_sent_ms = now;
  return ok;
}

bool Overlay::Impl::SendDataLocked(Peer& peer, Channel channel, const uint8_t* data, size_t len,
                                   uint64_t now) {
  uint8_t body[wire::kMaxDatagram];
  if (1 + len > sizeof(body)) return false;
  body[0] = static_cast<uint8_t>(channel);
  if (len) std::memcpy(body + 1, data, len);
  const bool direct = direct_allowed && peer.active >= 0;
  if (!SendP2PLocked(peer, wire::kPeerData, body, 1 + len, Route::kBest, nullptr, now)) {
    ++counters.data_dropped;
    return false;
  }
  ++(direct ? counters.data_sent_direct : counters.data_sent_relay);
  return true;
}

void Overlay::Impl::SendProbeLocked(Peer& peer, const net::Addr& to, uint64_t now) {
  uint8_t body[16];
  StoreU64(body, crypto::RandomU64());
  StoreU64(body + 8, OverlayNowUs());
  if (SendP2PLocked(peer, wire::kPeerProbe, body, sizeof(body), Route::kDirect, &to, now)) {
    ++counters.probes_sent;
  }
}

void Overlay::Impl::ProbeAllLocked(Peer& peer, uint64_t now) {
  for (const Candidate& c : peer.cands) SendProbeLocked(peer, c.addr, now);
  peer.last_probe_ms = now;
}

void Overlay::Impl::SendByeToPeersLocked(uint64_t now) {
  for (auto& [id, peer] : peers) SendP2PLocked(*peer, wire::kPeerBye, nullptr, 0, Route::kBest,
                                               nullptr, now);
}

// --- Receiving ---

void Overlay::Impl::HandleRelayedLocked(const uint8_t* p, size_t n, uint64_t now, Events* ev) {
  if (n < wire::kRelayedHeader + wire::kP2PHeader + crypto::kTagSize + 1) {
    ++counters.malformed;
    return;
  }
  const uint64_t src = wire::LoadU64(p + 2);
  const uint8_t* inner = p + wire::kRelayedHeader;
  if (inner[0] != wire::kMagic || inner[1] != wire::kP2P || wire::LoadU64(inner + 2) != src) {
    ++counters.malformed;
    return;
  }
  HandleP2PLocked(inner, n - wire::kRelayedHeader, /*relayed=*/true, server_addr, now, ev);
}

void Overlay::Impl::HandleP2PLocked(const uint8_t* p, size_t n, bool relayed,
                                    const net::Addr& from, uint64_t now, Events* ev) {
  if (n < wire::kP2PHeader + crypto::kTagSize + 1 || n > 2048) {
    ++counters.malformed;
    return;
  }
  const PeerId src = wire::LoadU64(p + 2);
  const uint64_t ctr = wire::LoadU64(p + 10);
  if (src == 0 || (reg.valid && src == reg.peer_id)) {
    ++counters.malformed;
    return;
  }
  Peer* peer = FindPeerLocked(src);
  if (!peer) {
    // Only relayed packets are worth keeping: a peer that has no record of ours cannot have a
    // validated direct path to us (that needs our PROBE_ACK), so its DATA comes via the relay,
    // while what arrives directly is PROBEs, which it repeats every 200 ms anyway. Buffering
    // those would crowd DATA out of the 8-per-source buffer.
    if (relayed) {
      BufferUnknownLocked(src, p, n, relayed, from, now);
    } else {
      ++counters.unknown_direct;
    }
    RequestIntroLocked(wire::kIntroByPeerId, src, now);
    return;
  }
  if (!peer->recv_window.Check(ctr)) {
    ++counters.replays;
    return;
  }
  uint8_t plain[2048];
  const size_t plain_len = n - wire::kP2PHeader - crypto::kTagSize;
  if (!crypto::Open(peer->recv_key, ctr, p, wire::kP2PHeader, p + wire::kP2PHeader,
                    n - wire::kP2PHeader, plain)) {
    ++counters.auth_failures;
    // Repeated failures from a known peer id usually mean it restarted: refresh its record.
    if (++peer->auth_failures >= lim::kAuthFailRefresh &&
        now - peer->last_refresh_ms >= lim::kRefreshMinMs) {
      peer->last_refresh_ms = now;
      RequestIntroLocked(wire::kIntroByPeerId, src, now);
    }
    return;
  }
  peer->recv_window.Accept(ctr);
  peer->auth_failures = 0;
  peer->last_recv_ms = now;

  int ci = -1;
  if (!relayed) {
    ci = FindCandidateLocked(*peer, from);
    if (ci < 0 && direct_allowed) {
      // Peer-reflexive: an authenticated packet from an address we did not know.
      MergeCandidateLocked(*peer, from, CandType::kPrflx);
      ci = FindCandidateLocked(*peer, from);
      if (ci >= 0) SendProbeLocked(*peer, from, now);
    }
    if (ci >= 0) peer->cands[static_cast<size_t>(ci)].last_recv_ms = now;
  }

  wire::Reader r(plain, plain_len);
  uint8_t kind = 0;
  r.U8(&kind);
  switch (kind) {
    case wire::kPeerProbe: {
      if (r.remaining() < 16) {
        ++counters.malformed;
        return;
      }
      if (relayed) {
        SendP2PLocked(*peer, wire::kPeerProbeAck, r.cursor(), 16, Route::kRelay, nullptr, now);
      } else if (direct_allowed) {
        SendP2PLocked(*peer, wire::kPeerProbeAck, r.cursor(), 16, Route::kDirect, &from, now);
      }
      break;
    }
    case wire::kPeerProbeAck: {
      uint64_t probe_id = 0, t_us = 0;
      if (!r.U64(&probe_id) || !r.U64(&t_us)) {
        ++counters.malformed;
        return;
      }
      ++counters.probe_acks_recv;
      const uint64_t now_us = OverlayNowUs();
      if (relayed || ci < 0 || !direct_allowed || t_us > now_us ||
          now_us - t_us > 10'000'000) {
        break;
      }
      Candidate& c = peer->cands[static_cast<size_t>(ci)];
      // Smoothed (7/8 old + 1/8 sample, kept x8 in fixed point like TCP's SRTT so small rises
      // are not lost to rounding), so jitter does not flip paths.
      const uint32_t sample = static_cast<uint32_t>((now_us - t_us + 999) / 1000);
      if (!c.validated || c.srtt_x8 == 0) {
        c.srtt_x8 = sample * 8;
      } else {
        c.srtt_x8 = c.srtt_x8 - c.srtt_x8 / 8 + sample;
      }
      c.rtt_ms = (c.srtt_x8 + 4) / 8;
      c.validated = true;
      ChooseActivePathLocked(*peer, now);
      break;
    }
    case wire::kPeerKeepalive:
      break;
    case wire::kPeerData: {
      uint8_t ch = 0;
      if (!r.U8(&ch) || ch < static_cast<uint8_t>(Channel::kDatagram) ||
          ch > static_cast<uint8_t>(Channel::kQos)) {
        ++counters.malformed;
        return;
      }
      ++(relayed ? counters.data_recv_relay : counters.data_recv_direct);
      ev->data.push_back(Events::Data{peer->vip, static_cast<Channel>(ch),
                                      std::vector<uint8_t>(r.cursor(), r.cursor() + r.remaining())});
      break;
    }
    case wire::kPeerBye: {
      const uint32_t vip = peer->vip;
      LogF(LogLevel::kInfo, "overlay: peer %s (%s) said BYE", Ipv4ToString(vip).c_str(),
           peer->gamertag.c_str());
      Trace("peer_bye", "\"vip\":\"" + Ipv4ToString(vip) + "\"");
      ++counters.peers_lost;
      RemovePeerLocked(src);
      if (vip) ev->lost.push_back(vip);
      break;
    }
    default:
      ++counters.malformed;
      break;
  }
}

// --- Paths ---

void Overlay::Impl::ChooseActivePathLocked(Peer& peer, uint64_t now) {
  int best = -1;
  for (size_t i = 0; i < peer.cands.size(); ++i) {
    Candidate& c = peer.cands[i];
    if (!c.validated) continue;
    if (now - c.last_recv_ms > lim::kDirectSilenceMs) {
      c.validated = false;
      continue;
    }
    if (best < 0) {
      best = static_cast<int>(i);
      continue;
    }
    const Candidate& b = peer.cands[static_cast<size_t>(best)];
    if (c.type < b.type || (c.type == b.type && c.rtt_ms < b.rtt_ms)) best = static_cast<int>(i);
  }
  // Hysteresis: keep the current path unless the best one is a better type or clearly faster.
  if (best >= 0 && peer.active >= 0 && best != peer.active) {
    const Candidate& cur = peer.cands[static_cast<size_t>(peer.active)];
    const Candidate& b = peer.cands[static_cast<size_t>(best)];
    const uint32_t margin = std::max(kSwitchMarginMs, cur.rtt_ms / 4);
    if (cur.validated && cur.type == b.type &&
        (b.rtt_ms + margin >= cur.rtt_ms || now - peer.path_since_ms < kMinPathDwellMs)) {
      best = peer.active;
    }
  }
  if (best == peer.active) return;
  peer.active = best;
  peer.path_since_ms = now;
  if (best >= 0) {
    const Candidate& c = peer.cands[static_cast<size_t>(best)];
    ++counters.path_switches;
    LogF(LogLevel::kInfo, "overlay: peer %s direct via %s (%s, %u ms)",
         Ipv4ToString(peer.vip).c_str(), c.addr.ToString().c_str(), CandTypeName(c.type),
         c.rtt_ms);
    Trace("peer_path", "\"vip\":\"" + Ipv4ToString(peer.vip) + "\",\"path\":\"direct\"," +
                           "\"addr\":\"" + c.addr.ToString() + "\",\"type\":\"" +
                           CandTypeName(c.type) + "\",\"rtt_ms\":" + std::to_string(c.rtt_ms));
  } else {
    Trace("peer_path", "\"vip\":\"" + Ipv4ToString(peer.vip) + "\",\"path\":\"relay\"");
  }
}

void Overlay::Impl::PeerTimersLocked(uint64_t now, Events* ev) {
  std::vector<PeerId> lost;
  for (auto& [id, up] : peers) {
    Peer& p = *up;
    if (now - p.last_recv_ms > lim::kPeerLostMs) {
      lost.push_back(id);
      continue;
    }
    if (p.active >= 0 &&
        now - p.cands[static_cast<size_t>(p.active)].last_recv_ms > lim::kDirectSilenceMs) {
      p.cands[static_cast<size_t>(p.active)].validated = false;
      ChooseActivePathLocked(p, now);
      if (p.active < 0) {
        ++counters.relay_fallbacks;
        LogF(LogLevel::kInfo, "overlay: peer %s direct path silent; using the relay",
             Ipv4ToString(p.vip).c_str());
        Trace("peer_fallback", "\"vip\":\"" + Ipv4ToString(p.vip) + "\"");
        p.probe_phase_ms = now;
        p.last_probe_ms = 0;
      }
    }
    if (direct_allowed && !p.cands.empty()) {
      const uint64_t elapsed = now - p.probe_phase_ms;
      uint64_t interval = 0;
      if (elapsed < lim::kProbeFastPhaseMs) {
        interval = lim::kProbeFastMs;
      } else if (p.active < 0 && elapsed < lim::kProbeStopMs) {
        interval = lim::kProbeSlowMs;
      }
      if (interval && (p.last_probe_ms == 0 || now - p.last_probe_ms >= interval)) {
        ProbeAllLocked(p, now);
      }
    }
    if (p.active < 0 && now - p.last_recv_ms >= lim::kRelaySilenceCheckMs) {
      check_server_soon = true;
    }
    if (p.last_sent_ms == 0 || now - p.last_sent_ms >= lim::kPeerKeepaliveMs) {
      SendP2PLocked(p, wire::kPeerKeepalive, nullptr, 0, Route::kBest, nullptr, now);
    }
  }
  for (PeerId id : lost) {
    Peer* p = FindPeerLocked(id);
    if (!p) continue;
    const uint32_t vip = p->vip;
    LogF(LogLevel::kInfo, "overlay: peer %s (%s) lost (15 s silent)", Ipv4ToString(vip).c_str(),
         p->gamertag.c_str());
    Trace("peer_lost", "\"vip\":\"" + Ipv4ToString(vip) + "\"");
    ++counters.peers_lost;
    RemovePeerLocked(id);
    if (vip) ev->lost.push_back(vip);
  }
}

// --- Queues ---

void Overlay::Impl::BufferUnknownLocked(PeerId src, const uint8_t* p, size_t n, bool relayed,
                                        const net::Addr& from, uint64_t now) {
  const auto per_source = std::count_if(unknown.begin(), unknown.end(),
                                        [src](const UnknownPacket& u) { return u.src == src; });
  if (static_cast<size_t>(per_source) >= lim::kUnknownPerSource ||
      unknown.size() >= lim::kUnknownTotal) {
    ++counters.unknown_dropped;
    return;
  }
  unknown.push_back(UnknownPacket{src, std::vector<uint8_t>(p, p + n), relayed, from, now});
  ++counters.unknown_buffered;
}

void Overlay::Impl::ReplayUnknownLocked(PeerId src, uint64_t now, Events* ev) {
  std::vector<UnknownPacket> mine;
  for (auto it = unknown.begin(); it != unknown.end();) {
    if (it->src == src) {
      mine.push_back(std::move(*it));
      it = unknown.erase(it);
    } else {
      ++it;
    }
  }
  for (const UnknownPacket& u : mine) {
    if (!FindPeerLocked(src)) break;  // removed by an earlier buffered packet (BYE)
    HandleP2PLocked(u.packet.data(), u.packet.size(), u.relayed, u.from, now, ev);
  }
}

void Overlay::Impl::FlushPendingLocked(Peer& peer, uint64_t now) {
  auto it = pending.find(peer.vip);
  if (it == pending.end()) return;
  PendingQueue q = std::move(it->second);
  pending.erase(it);
  for (const PendingPayload& item : q.items) {
    if (now - item.t_ms > lim::kPendingMaxAgeMs) {
      ++counters.data_dropped;
      continue;
    }
    SendDataLocked(peer, item.channel, item.data.data(), item.data.size(), now);
  }
}

void Overlay::Impl::ExpireQueuesLocked(uint64_t now) {
  for (auto it = pending.begin(); it != pending.end();) {
    PendingQueue& q = it->second;
    while (!q.items.empty() && now - q.items.front().t_ms > lim::kPendingMaxAgeMs) {
      q.bytes -= q.items.front().data.size();
      q.items.pop_front();
      ++counters.data_dropped;
    }
    it = q.items.empty() ? pending.erase(it) : std::next(it);
  }
  while (!unknown.empty() && now - unknown.front().t_ms > lim::kUnknownMaxAgeMs) {
    unknown.pop_front();
    ++counters.unknown_dropped;
  }
}

}  // namespace ac6net
