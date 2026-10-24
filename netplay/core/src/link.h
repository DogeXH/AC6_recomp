// ac6net - internal contract between the overlay (transport) and its users (vnet, qos).
#pragma once

#include <cstddef>
#include <cstdint>

namespace ac6net {

// DATA channel numbers (PROTOCOL.md 2.8).
enum class Channel : uint8_t {
  kDatagram = 0x01,
  kStream = 0x02,
  kStreamCtl = 0x03,
  kQos = 0x04,
};

// Implemented by the overlay. Thread-safe; never blocks.
class PeerLink {
 public:
  virtual ~PeerLink() = default;

  // Sends one DATA payload to the peer that owns `vip`. If the peer is not yet known, the
  // overlay requests an introduction and queues the payload (bounded: 32 payloads / 64 KiB per
  // vip, 2 s max age); queued payloads are flushed once a path exists. Returns false if the
  // payload was dropped immediately (vip not ours, queue full, payload too large, overlay down).
  // `len` must be <= MaxPayload().
  virtual bool Send(uint32_t vip, Channel channel, const uint8_t* data, size_t len) = 0;

  // Own virtual IP (host order), 0 before the first registration.
  virtual uint32_t LocalVip() const = 0;

  // Largest DATA payload accepted by Send() (so that the full datagram, including relay framing,
  // stays <= 1400 bytes). At least 1200.
  virtual size_t MaxPayload() const = 0;

  // Milliseconds since an arbitrary epoch; the same clock the overlay uses for Tick().
  virtual uint64_t NowMs() const = 0;
};

// Implemented by vnet and qos; the overlay dispatches by channel on its network thread.
class LinkReceiver {
 public:
  virtual ~LinkReceiver() = default;
  virtual void OnLinkData(uint32_t from_vip, Channel channel, const uint8_t* data, size_t len) = 0;
  // The peer said BYE, or no authenticated packet arrived for 15 s on any path.
  virtual void OnPeerLost(uint32_t vip) = 0;
};

}  // namespace ac6net
