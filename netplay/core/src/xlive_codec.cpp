// ac6net - JSON wire encoding for the xlive layer (PROTOCOL.md section 3).
#include "xlive_codec.h"

#include <cmath>
#include <limits>

namespace ac6net::xlive::codec {

namespace {

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char kHexDigits[] = "0123456789abcdef";

int Base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool IsKnownType(uint32_t t) {
  return t <= static_cast<uint32_t>(DataType::kDateTime) ||
         t == static_cast<uint32_t>(DataType::kNull);
}

}  // namespace

// --- Text encodings ---

std::string Base64Encode(const uint8_t* data, size_t len) {
  std::string out;
  out.reserve((len + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 3 <= len; i += 3) {
    const uint32_t v = (uint32_t{data[i]} << 16) | (uint32_t{data[i + 1]} << 8) | data[i + 2];
    out.push_back(kBase64Alphabet[(v >> 18) & 63]);
    out.push_back(kBase64Alphabet[(v >> 12) & 63]);
    out.push_back(kBase64Alphabet[(v >> 6) & 63]);
    out.push_back(kBase64Alphabet[v & 63]);
  }
  const size_t rest = len - i;
  if (rest == 1) {
    const uint32_t v = uint32_t{data[i]} << 16;
    out.push_back(kBase64Alphabet[(v >> 18) & 63]);
    out.push_back(kBase64Alphabet[(v >> 12) & 63]);
    out.append("==");
  } else if (rest == 2) {
    const uint32_t v = (uint32_t{data[i]} << 16) | (uint32_t{data[i + 1]} << 8);
    out.push_back(kBase64Alphabet[(v >> 18) & 63]);
    out.push_back(kBase64Alphabet[(v >> 12) & 63]);
    out.push_back(kBase64Alphabet[(v >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

bool Base64Decode(std::string_view text, std::vector<uint8_t>* out) {
  out->clear();
  if (text.size() % 4 != 0) return false;
  size_t pad = 0;
  if (!text.empty() && text.back() == '=') {
    pad = (text.size() >= 2 && text[text.size() - 2] == '=') ? 2 : 1;
  }
  out->reserve(text.size() / 4 * 3);
  for (size_t i = 0; i < text.size(); i += 4) {
    const bool last = i + 4 == text.size();
    int v[4];
    for (size_t k = 0; k < 4; ++k) {
      const char c = text[i + k];
      if (last && k >= 4 - pad) {
        if (c != '=') return false;
        v[k] = 0;
        continue;
      }
      v[k] = Base64Value(c);
      if (v[k] < 0) return false;  // includes '=' in the middle
    }
    const uint32_t bits =
        (uint32_t(v[0]) << 18) | (uint32_t(v[1]) << 12) | (uint32_t(v[2]) << 6) | uint32_t(v[3]);
    out->push_back(uint8_t(bits >> 16));
    if (!last || pad < 2) out->push_back(uint8_t(bits >> 8));
    if (!last || pad < 1) out->push_back(uint8_t(bits));
  }
  return true;
}

std::string HexU64(uint64_t value) {
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[size_t(i)] = kHexDigits[value & 0xF];
    value >>= 4;
  }
  return out;
}

bool ParseHexU64(std::string_view text, uint64_t* out) {
  if (text.size() != 16) return false;
  uint64_t v = 0;
  for (char c : text) {
    const int d = HexValue(c);
    if (d < 0) return false;
    v = (v << 4) | uint64_t(d);
  }
  *out = v;
  return true;
}

std::string HexBytes(const uint8_t* data, size_t len) {
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out.push_back(kHexDigits[data[i] >> 4]);
    out.push_back(kHexDigits[data[i] & 0xF]);
  }
  return out;
}

bool ParseHexBytes(std::string_view text, uint8_t* out, size_t len) {
  if (text.size() != len * 2) return false;
  for (size_t i = 0; i < len; ++i) {
    const int hi = HexValue(text[2 * i]);
    const int lo = HexValue(text[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = uint8_t((hi << 4) | lo);
  }
  return true;
}

bool ParseIpv4(std::string_view text, uint32_t* out) {
  uint32_t ip = 0;
  size_t pos = 0;
  for (int octet = 0; octet < 4; ++octet) {
    if (octet > 0) {
      if (pos >= text.size() || text[pos] != '.') return false;
      ++pos;
    }
    size_t digits = 0;
    uint32_t v = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9' && digits < 3) {
      v = v * 10 + uint32_t(text[pos] - '0');
      ++pos;
      ++digits;
    }
    if (digits == 0 || v > 255) return false;
    ip = (ip << 8) | v;
  }
  if (pos != text.size()) return false;
  *out = ip;
  return true;
}

std::string FormatIpv4(uint32_t ip) {
  return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 0xFF) + "." +
         std::to_string((ip >> 8) & 0xFF) + "." + std::to_string(ip & 0xFF);
}

// --- JSON helpers ---

Json Parse(std::string_view text) {
  return Json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
}

std::string Dump(const Json& value) {
  return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

const Json* Find(const Json& obj, const char* key) {
  if (!obj.is_object()) return nullptr;
  const auto it = obj.find(key);
  return it == obj.end() ? nullptr : &*it;
}

bool ToU32(const Json& value, uint32_t* out) {
  if (!value.is_number_unsigned()) return false;
  const uint64_t v = value.get<uint64_t>();
  if (v > std::numeric_limits<uint32_t>::max()) return false;
  *out = uint32_t(v);
  return true;
}

bool ToI64(const Json& value, int64_t* out) {
  if (value.is_number_unsigned()) {
    const uint64_t v = value.get<uint64_t>();
    if (v > uint64_t(std::numeric_limits<int64_t>::max())) return false;
    *out = int64_t(v);
    return true;
  }
  if (value.is_number_integer()) {
    *out = value.get<int64_t>();
    return true;
  }
  return false;
}

bool GetU32(const Json& obj, const char* key, uint32_t* out) {
  const Json* v = Find(obj, key);
  return v && ToU32(*v, out);
}

bool GetU64(const Json& obj, const char* key, uint64_t* out) {
  const Json* v = Find(obj, key);
  if (!v || !v->is_number_unsigned()) return false;
  *out = v->get<uint64_t>();
  return true;
}

bool GetI64(const Json& obj, const char* key, int64_t* out) {
  const Json* v = Find(obj, key);
  return v && ToI64(*v, out);
}

bool GetBool(const Json& obj, const char* key, bool* out) {
  const Json* v = Find(obj, key);
  if (!v || !v->is_boolean()) return false;
  *out = v->get<bool>();
  return true;
}

bool GetString(const Json& obj, const char* key, std::string* out) {
  const Json* v = Find(obj, key);
  if (!v || !v->is_string()) return false;
  *out = v->get_ref<const std::string&>();
  return true;
}

bool GetHexU64(const Json& obj, const char* key, uint64_t* out) {
  const Json* v = Find(obj, key);
  return v && v->is_string() && ParseHexU64(v->get_ref<const std::string&>(), out);
}

bool GetVip(const Json& obj, const char* key, uint32_t* out) {
  const Json* v = Find(obj, key);
  return v && v->is_string() && ParseIpv4(v->get_ref<const std::string&>(), out);
}

bool GetU64Flexible(const Json& obj, const char* key, uint64_t* out) {
  return GetHexU64(obj, key, out) || GetU64(obj, key, out);
}

// --- XUSER data ---

bool PropertyToJson(const Property& property, Json* out) {
  const UserData& d = property.value;
  Json v;
  switch (d.type) {
    case DataType::kContext:
      v = uint32_t(d.i);
      break;
    case DataType::kInt32:
      v = int32_t(d.i);
      break;
    case DataType::kInt64:
    case DataType::kDateTime:
      v = d.i;
      break;
    case DataType::kDouble:
      // JSON has no NaN/Inf; send 0 rather than an invalid document.
      v = std::isfinite(d.d) ? d.d : 0.0;
      break;
    case DataType::kFloat: {
      const float f = float(d.d);
      v = std::isfinite(f) ? double(f) : 0.0;
      break;
    }
    case DataType::kUnicode:
    case DataType::kBinary:
      v = Base64Encode(d.bytes.data(), d.bytes.size());
      break;
    case DataType::kNull:
    default:
      return false;
  }
  *out = Json{{"id", property.id}, {"t", uint32_t(d.type)}, {"v", std::move(v)}};
  return true;
}

bool PropertyFromJson(const Json& value, Property* out) {
  uint32_t id = 0, t = 0;
  if (!GetU32(value, "id", &id) || !GetU32(value, "t", &t) || !IsKnownType(t)) return false;
  Property p;
  p.id = id;
  p.value.type = DataType(t);
  if (p.value.type == DataType::kNull) {
    *out = std::move(p);
    return true;
  }
  const Json* v = Find(value, "v");
  if (!v) return false;
  switch (p.value.type) {
    case DataType::kContext: {
      uint32_t u = 0;
      if (!ToU32(*v, &u)) return false;
      p.value.i = u;
      break;
    }
    case DataType::kInt32: {
      int64_t i = 0;
      if (!ToI64(*v, &i)) return false;
      // Accept the signed range and its unsigned reinterpretation.
      if (i < std::numeric_limits<int32_t>::min() || i > int64_t{0xFFFFFFFF}) return false;
      p.value.i = int32_t(uint32_t(i));
      break;
    }
    case DataType::kInt64:
    case DataType::kDateTime:
      if (!ToI64(*v, &p.value.i)) return false;
      break;
    case DataType::kDouble:
    case DataType::kFloat:
      if (!v->is_number()) return false;
      p.value.d = v->get<double>();
      if (p.value.type == DataType::kFloat) p.value.d = double(float(p.value.d));
      break;
    case DataType::kUnicode:
    case DataType::kBinary:
      if (!v->is_string() || !Base64Decode(v->get_ref<const std::string&>(), &p.value.bytes)) {
        return false;
      }
      break;
    default:
      return false;
  }
  *out = std::move(p);
  return true;
}

Json ContextToJson(const Context& context) { return Json::array({context.id, context.value}); }

bool ContextFromJson(const Json& value, Context* out) {
  if (!value.is_array() || value.size() != 2) return false;
  Context c;
  if (!ToU32(value[0], &c.id) || !ToU32(value[1], &c.value)) return false;
  *out = c;
  return true;
}

Json PropertiesToJson(const std::vector<Property>& properties) {
  Json arr = Json::array();
  for (const auto& p : properties) {
    Json j;
    if (PropertyToJson(p, &j)) arr.push_back(std::move(j));
  }
  return arr;
}

Json ContextsToJson(const std::vector<Context>& contexts) {
  Json arr = Json::array();
  for (const auto& c : contexts) arr.push_back(ContextToJson(c));
  return arr;
}

std::vector<Property> PropertiesFromJson(const Json& array, size_t* skipped) {
  std::vector<Property> out;
  size_t bad = 0;
  if (array.is_array()) {
    for (const auto& item : array) {
      Property p;
      if (PropertyFromJson(item, &p)) {
        out.push_back(std::move(p));
      } else {
        ++bad;
      }
    }
  } else if (!array.is_null()) {
    ++bad;
  }
  if (skipped) *skipped = bad;
  return out;
}

std::vector<Context> ContextsFromJson(const Json& array, size_t* skipped) {
  std::vector<Context> out;
  size_t bad = 0;
  if (array.is_array()) {
    for (const auto& item : array) {
      Context c;
      if (ContextFromJson(item, &c)) {
        out.push_back(c);
      } else {
        ++bad;
      }
    }
  } else if (!array.is_null()) {
    ++bad;
  }
  if (skipped) *skipped = bad;
  return out;
}

// --- SessionDoc ---

bool ParseSessionDoc(const Json& value, SessionDoc* out) {
  SessionDoc doc;
  std::string sid;
  if (!GetString(value, "session_id", &sid) || !KidFromHex(sid, &doc.session_id)) return false;
  const Json* host = Find(value, "host");
  if (!host || !GetHexU64(*host, "peer_id", &doc.host_peer_id) ||
      !GetVip(*host, "vip", &doc.host_vip)) {
    return false;
  }
  GetHexU64(*host, "xuid", &doc.host_xuid);
  GetString(*host, "gamertag", &doc.host_gamertag);

  std::string key;
  doc.has_key = GetString(value, "key", &key) && KeyFromHex(key, &doc.key);
  doc.has_nonce = GetHexU64(value, "nonce", &doc.nonce);
  GetU32(value, "flags", &doc.flags);
  GetString(value, "state", &doc.state);
  GetU32(value, "public_slots", &doc.public_slots);
  GetU32(value, "private_slots", &doc.private_slots);
  GetU32(value, "filled_public", &doc.filled_public);
  GetU32(value, "filled_private", &doc.filled_private);
  if (const Json* c = Find(value, "contexts")) doc.contexts = ContextsFromJson(*c);
  if (const Json* p = Find(value, "properties")) doc.properties = PropertiesFromJson(*p);
  if (const Json* m = Find(value, "members"); m && m->is_array()) {
    for (const auto& item : *m) {
      DocMember member;
      if (!GetHexU64(item, "xuid", &member.xuid)) continue;
      GetBool(item, "private", &member.is_private);
      doc.members.push_back(member);
    }
  }
  *out = std::move(doc);
  return true;
}

}  // namespace ac6net::xlive::codec
