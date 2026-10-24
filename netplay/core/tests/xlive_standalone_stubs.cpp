// Stand-ins for the overlay lane's log.cpp/netplay.cpp functions that xlive uses, so the xlive
// lane can build and test on its own. They compile out as soon as the overlay sources
// (src/overlay.h) are in the tree; the real implementations are then linked instead.
//
// In standalone mode the stub Trace() records every event so the tests can check them.
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "xlive_fake_backend.h"

#if AC6NET_XLIVE_STANDALONE

#include "ac6net/log.h"
#include "ac6net/netplay.h"

namespace {

std::mutex g_mu;
ac6net::LogSink g_sink;
std::vector<std::string> g_trace;

}  // namespace

namespace ac6net {

void SetLogSink(LogSink sink) {
  std::lock_guard lk(g_mu);
  g_sink = std::move(sink);
}

void LogF(LogLevel level, const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  std::lock_guard lk(g_mu);
  if (g_sink) {
    g_sink(level, buf);
  } else if (level >= LogLevel::kWarn) {
    std::fprintf(stderr, "    [log] %s\n", buf);
  }
}

void Trace(std::string_view event, std::string_view fields_json) {
  std::string line = "{\"ev\":\"" + JsonEscape(event) + "\"";
  if (!fields_json.empty()) {
    line += ',';
    line += fields_json;
  }
  line += '}';
  std::lock_guard lk(g_mu);
  g_trace.push_back(std::move(line));
}

bool TraceEnabled() { return true; }

std::string JsonEscape(std::string_view s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char esc[8];
          std::snprintf(esc, sizeof(esc), "\\u%04x", c);
          out += esc;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// ARCHITECTURE.md section 3.
XnAddr MakeXnAddr(uint32_t vip, PeerId peer_id) {
  XnAddr a;
  a.ina = vip;
  a.ina_online = vip;
  a.port_online = 1000;
  a.ab_enet = {0x00, 0x22, 0x48, uint8_t(vip >> 16), uint8_t(vip >> 8), uint8_t(vip)};
  for (int i = 0; i < 4; ++i) a.ab_online[size_t(i)] = uint8_t(vip >> (24 - 8 * i));
  for (int i = 0; i < 8; ++i) a.ab_online[size_t(8 + i)] = uint8_t(peer_id >> (56 - 8 * i));
  a.ab_online[16] = 2;  // platform type
  return a;
}

bool ParseXnAddr(const XnAddr& addr, uint32_t* vip, PeerId* peer_id) {
  const uint32_t v = addr.ina_online ? addr.ina_online : addr.ina;
  if (!IsVip(v)) return false;
  PeerId p = 0;
  for (int i = 0; i < 8; ++i) p = (p << 8) | addr.ab_online[size_t(8 + i)];
  if (vip) *vip = v;
  if (peer_id) *peer_id = p;
  return true;
}

uint64_t MachineIdFromXnAddr(const XnAddr& addr) {
  uint64_t mac = 0;
  for (uint8_t b : addr.ab_enet) mac = (mac << 8) | b;
  return 0xFA00000000000000ull | mac;
}

}  // namespace ac6net

namespace xlive_test {

std::vector<std::string> TraceLines() {
  std::lock_guard lk(g_mu);
  return g_trace;
}

void ClearTrace() {
  std::lock_guard lk(g_mu);
  g_trace.clear();
}

}  // namespace xlive_test

#else  // the overlay lane's implementations are linked

namespace xlive_test {
std::vector<std::string> TraceLines() { return {}; }
void ClearTrace() {}
}  // namespace xlive_test

#endif
