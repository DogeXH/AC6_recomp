// A minimal in-process lobby stand-in for parser fuzzing: it answers HELLO with HELLO_ACK and
// can then send arbitrary *authenticated* S2C inner messages and RELAYED packets, which reach
// the overlay's post-decryption parsers (random bytes never get past the AEAD check).
// Test-only; the real server is netplay/server.
#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "crypto.h"
#include "overlay_net.h"
#include "overlay_test_util.h"
#include "overlay_wire.h"

namespace overlay_test {

class FakeServer {
 public:
  static constexpr uint16_t kPort = 47818;
  static constexpr uint64_t kPeerId = 0x1111222233334444ull;
  static constexpr uint32_t kVip = 0x0A400005;

  bool Start() {
    ac6net::crypto::GenerateKeypair(&sk_, &pk_);
    ac6net::crypto::RandomBytes(boot_.data(), boot_.size());
    std::string err;
    if (!sock_.Open(AF_INET, kPort, &err)) return false;
    running_ = true;
    thread_ = std::thread([this] { Loop(); });
    return true;
  }
  ~FakeServer() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
  }

  std::string addr() const { return "127.0.0.1:" + std::to_string(kPort); }
  std::string key_hex() const { return ac6net::HexEncode(pk_.data(), pk_.size()); }
  bool has_client() {
    std::lock_guard<std::mutex> lock(mu_);
    return have_client_;
  }
  size_t c2s_count() {
    std::lock_guard<std::mutex> lock(mu_);
    return c2s_inners_.size();
  }
  std::vector<std::vector<uint8_t>> c2s_inners() {
    std::lock_guard<std::mutex> lock(mu_);
    return c2s_inners_;
  }

  // Sends one S2C with `inner` (kind byte first) under the client's s2c key.
  void SendS2C(const std::vector<uint8_t>& inner) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!have_client_) return;
    std::vector<uint8_t> pkt(ac6net::wire::kS2CHeader + inner.size() + ac6net::crypto::kTagSize);
    pkt[0] = 0xA6;
    pkt[1] = 0x04;
    std::memcpy(pkt.data() + 2, boot_.data(), 8);
    const uint64_t ctr = ++s2c_ctr_;
    for (int i = 0; i < 8; ++i) pkt[10 + i] = static_cast<uint8_t>(ctr >> (8 * i));
    ac6net::crypto::Seal(s2c_key_, ctr, pkt.data(), ac6net::wire::kS2CHeader, inner.data(),
                         inner.size(), pkt.data() + ac6net::wire::kS2CHeader);
    sock_.SendTo(client_addr_, pkt.data(), pkt.size());
  }
  void SendRaw(const std::vector<uint8_t>& bytes) {
    std::lock_guard<std::mutex> lock(mu_);
    if (have_client_) sock_.SendTo(client_addr_, bytes.data(), bytes.size());
  }

  ac6net::crypto::PublicKey client_pk() {
    std::lock_guard<std::mutex> lock(mu_);
    return client_pk_;
  }
  std::array<uint8_t, 8> client_boot() {
    std::lock_guard<std::mutex> lock(mu_);
    return client_boot_;
  }

 private:
  void Loop() {
    uint8_t buf[2048];
    while (running_) {
      net_poll();
      ac6net::net::Addr from;
      const int n = sock_.RecvFrom(buf, sizeof(buf), &from);
      if (n <= 0) continue;
      Handle(buf, static_cast<size_t>(n), from);
    }
  }
  void net_poll() {
    ac6net::net::UdpSocket* socks[1] = {&sock_};
    bool readable[1];
    ac6net::net::PollReadable(socks, 1, 5, readable);
  }

  void Handle(const uint8_t* p, size_t n, const ac6net::net::Addr& from) {
    namespace crypto = ac6net::crypto;
    if (n < 2 || p[0] != 0xA6) return;
    std::lock_guard<std::mutex> lock(mu_);
    if (p[1] == 0x01 && n >= ac6net::wire::kHelloHeader + crypto::kTagSize) {
      crypto::PublicKey cpk;
      std::memcpy(cpk.data(), p + 2, 32);
      std::array<uint8_t, 8> cboot;
      std::memcpy(cboot.data(), p + 34, 8);
      const uint64_t ctr = ac6net::wire::LoadU64(p + 42);
      crypto::Key dh;
      if (!crypto::Dh(sk_, cpk, &dh)) return;
      const crypto::Key c2s = crypto::Kdf(dh, "AC6NET-C2S-1", {cpk, pk_, cboot});
      std::vector<uint8_t> plain(n);
      if (!crypto::Open(c2s, ctr, p, ac6net::wire::kHelloHeader, p + ac6net::wire::kHelloHeader,
                        n - ac6net::wire::kHelloHeader, plain.data())) {
        return;
      }
      client_pk_ = cpk;
      client_boot_ = cboot;
      client_addr_ = from;
      c2s_key_ = c2s;
      s2c_key_ = crypto::Kdf(dh, "AC6NET-S2C-1", {cpk, pk_, cboot, boot_});
      have_client_ = true;
      const std::string ack =
          "{\"v\":1,\"peer_id\":\"00000000000000aa\",\"xuid\":\"0009000000000001\","
          "\"vip\":\"10.64.0.1\",\"gamertag\":\"Fuzz\",\"public\":[\"127.0.0.1\"," +
          std::to_string(from.Port()) + "],\"server_time_ms\":1,\"motd\":\"\"}";
      std::vector<uint8_t> pkt(ac6net::wire::kHelloAckHeader + ack.size() + crypto::kTagSize);
      pkt[0] = 0xA6;
      pkt[1] = 0x02;
      std::memcpy(pkt.data() + 2, boot_.data(), 8);
      const uint64_t sctr = ++s2c_ctr_;
      for (int i = 0; i < 8; ++i) pkt[10 + i] = static_cast<uint8_t>(sctr >> (8 * i));
      crypto::Seal(s2c_key_, sctr, pkt.data(), ac6net::wire::kHelloAckHeader,
                   reinterpret_cast<const uint8_t*>(ack.data()), ack.size(),
                   pkt.data() + ac6net::wire::kHelloAckHeader);
      sock_.SendTo(from, pkt.data(), pkt.size());
    } else if (p[1] == 0x03 && have_client_ && n >= ac6net::wire::kC2SHeader + crypto::kTagSize) {
      const uint64_t ctr = ac6net::wire::LoadU64(p + 18);
      std::vector<uint8_t> plain(n);
      if (!crypto::Open(c2s_key_, ctr, p, ac6net::wire::kC2SHeader, p + ac6net::wire::kC2SHeader,
                        n - ac6net::wire::kC2SHeader, plain.data())) {
        return;
      }
      plain.resize(n - ac6net::wire::kC2SHeader - crypto::kTagSize);
      c2s_inners_.push_back(std::move(plain));
    }
  }

  ac6net::crypto::SecretKey sk_{};
  ac6net::crypto::PublicKey pk_{};
  std::array<uint8_t, 8> boot_{};
  ac6net::net::UdpSocket sock_;
  std::thread thread_;
  std::atomic<bool> running_{false};

  std::mutex mu_;
  bool have_client_ = false;
  ac6net::crypto::PublicKey client_pk_{};
  std::array<uint8_t, 8> client_boot_{};
  ac6net::net::Addr client_addr_;
  ac6net::crypto::Key c2s_key_{};
  ac6net::crypto::Key s2c_key_{};
  uint64_t s2c_ctr_ = 0;
  std::vector<std::vector<uint8_t>> c2s_inners_;
};

}  // namespace overlay_test
