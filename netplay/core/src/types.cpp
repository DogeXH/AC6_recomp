// ac6net - helpers declared in ac6net/types.h.
#include "ac6net/types.h"

#include <cstdio>

namespace ac6net {

namespace {

int HexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::string HexEncode(const uint8_t* data, size_t len) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.resize(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out[2 * i] = kDigits[data[i] >> 4];
    out[2 * i + 1] = kDigits[data[i] & 0x0F];
  }
  return out;
}

// Accepts upper- or lower-case digits. The string must encode exactly `out_len` bytes. `out` is
// left untouched on failure.
bool HexDecode(const std::string& hex, uint8_t* out, size_t out_len) {
  if (hex.size() != out_len * 2) return false;
  for (char c : hex)
    if (HexNibble(c) < 0) return false;
  for (size_t i = 0; i < out_len; ++i)
    out[i] = static_cast<uint8_t>((HexNibble(hex[2 * i]) << 4) | HexNibble(hex[2 * i + 1]));
  return true;
}

std::string Ipv4ToString(uint32_t ip) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                (ip >> 8) & 0xFF, ip & 0xFF);
  return buf;
}

// Strict dotted quad: four decimal parts of 1-3 digits, each <= 255. Nothing else is accepted
// (no shorthand forms, no whitespace, no trailing characters).
bool Ipv4FromString(const std::string& s, uint32_t* ip_host_order) {
  uint32_t result = 0;
  size_t pos = 0;
  for (int part = 0; part < 4; ++part) {
    if (part > 0) {
      if (pos >= s.size() || s[pos] != '.') return false;
      ++pos;
    }
    uint32_t value = 0;
    size_t digits = 0;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
      if (++digits > 3) return false;
      value = value * 10 + static_cast<uint32_t>(s[pos] - '0');
      ++pos;
    }
    if (digits == 0 || value > 255) return false;
    result = (result << 8) | value;
  }
  if (pos != s.size()) return false;
  if (ip_host_order) *ip_host_order = result;
  return true;
}

}  // namespace ac6net
