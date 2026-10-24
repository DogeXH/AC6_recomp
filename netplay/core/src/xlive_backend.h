// ac6net - internal seam between XLive and the rest of the core, so XLive can be unit-tested
// with a fake backend. XLive::XLive(Netplay*) (in xlive_netplay.cpp) wraps a Netplay in an
// adapter that implements this interface; tests construct XLive through MakeXLiveForTest().
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "ac6net/config.h"
#include "ac6net/netplay.h"
#include "ac6net/types.h"
#include "ac6net/xlive.h"

namespace ac6net::xlive {

class XLiveBackend {
 public:
  virtual ~XLiveBackend() = default;
  virtual RpcResult Call(const std::string& method, const std::string& args_json,
                         uint32_t timeout_ms) = 0;
  virtual Identity identity() const = 0;
  virtual NetState state() const = 0;
  virtual const Config& config() const = 0;
  virtual XnAddr LocalXnAddr() const = 0;
  virtual int XnAddrToInAddr(const XnAddr& addr, const XnKid* kid, uint32_t* out_vip) = 0;
  virtual void Random(uint8_t* out, size_t len) = 0;
};

// Test-only factory: XLive driven by `backend` (not owned; must outlive the XLive).
std::unique_ptr<XLive> MakeXLiveForTest(XLiveBackend* backend);

}  // namespace ac6net::xlive
