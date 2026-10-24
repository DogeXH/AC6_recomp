// ac6net - in-band QoS service (XNetQosListen / XNetQosLookup) over overlay channel 0x04.
// Contract between the vnet lane (implements) and the overlay lane (wires it in netplay.cpp).
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "ac6net/netplay.h"
#include "link.h"

namespace ac6net {

class QosService {
 public:
  explicit QosService(PeerLink* link);
  ~QosService();

  // XNET_QOS_LISTEN_* flags: 0x01 ENABLE, 0x02 DISABLE, 0x04 SET_DATA, 0x08 SET_BITSPERSEC,
  // 0x10 RELEASE. Registrations are keyed by XNKID; RELEASE removes one.
  void Listen(const XnKid& kid, const uint8_t* data, size_t len, uint32_t bps, uint32_t flags);

  // `vips[i] == 0` means "unresolvable": that target completes at once with contacted = false.
  // Sends `probes` REQ packets per target (every 50 ms); each target completes when all probes
  // are answered or 2 s after the last probe. Returns a lookup id > 0.
  uint64_t Lookup(const std::vector<uint32_t>& vips, const std::vector<XnKid>& kids,
                  uint32_t probes, QosCallback cb);
  void Cancel(uint64_t lookup_id);

  // Network-thread entry points.
  void OnLinkData(uint32_t from_vip, const uint8_t* data, size_t len);
  void OnPeerLost(uint32_t vip);
  void Tick(uint64_t now_ms);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ac6net
