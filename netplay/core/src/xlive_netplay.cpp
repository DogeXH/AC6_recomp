// ac6net - XLive bound to a Netplay instance: an XLiveBackend adapter over the public Netplay
// API (identity, state, lobby RPC and XNet helpers).
#include <random>

#include "ac6net/netplay.h"
#include "ac6net/xlive.h"
#include "xlive_backend.h"

namespace ac6net::xlive {

namespace {

class NetplayBackend final : public XLiveBackend {
 public:
  explicit NetplayBackend(Netplay* netplay) : netplay_(netplay) {}

  RpcResult Call(const std::string& method, const std::string& args_json,
                 uint32_t timeout_ms) override {
    if (!netplay_) {
      RpcResult r;
      r.err = "offline";
      return r;
    }
    return netplay_->Call(method, args_json, timeout_ms);
  }

  Identity identity() const override { return netplay_ ? netplay_->identity() : Identity{}; }

  NetState state() const override { return netplay_ ? netplay_->state() : NetState::kDisabled; }

  const Config& config() const override {
    static const Config kDisabledConfig;
    return netplay_ ? netplay_->config() : kDisabledConfig;
  }

  XnAddr LocalXnAddr() const override {
    if (netplay_) return netplay_->LocalXnAddr();
    XnAddr addr;
    addr.ina = kLoopback;
    return addr;
  }

  int XnAddrToInAddr(const XnAddr& addr, const XnKid* kid, uint32_t* out_vip) override {
    return netplay_ ? netplay_->XnAddrToInAddr(addr, kid, out_vip) : wsa::kNetDown;
  }

  void Random(uint8_t* out, size_t len) override {
    if (netplay_) {
      netplay_->Random(out, len);
      return;
    }
    std::random_device rd;
    for (size_t i = 0; i < len; ++i) out[i] = uint8_t(rd());
  }

 private:
  Netplay* netplay_;  // not owned; outlives the XLive (it owns it)
};

}  // namespace

XLive::XLive(Netplay* netplay) : XLive(new NetplayBackend(netplay), /*owns_backend=*/true) {}

}  // namespace ac6net::xlive
