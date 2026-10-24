// ac6net - JSON wire encoding for the xlive layer (PROTOCOL.md section 3).
//
// IDs are lower-case hex (XNKID, XUID, peer_id, nonce: 16 digits; XNKEY: 32 digits), vIPs are
// dotted quads, contexts are [id, value] and properties are {"id","t","v"} with unicode and
// binary values in standard base64. Every parser is total: malformed input returns false and
// never throws or reads out of bounds.
#pragma once

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "ac6net/types.h"

namespace ac6net::xlive::codec {

using Json = nlohmann::json;

// --- Text encodings ---
std::string Base64Encode(const uint8_t* data, size_t len);
// Standard alphabet, padding required, no whitespace. Returns false on any malformed input.
bool Base64Decode(std::string_view text, std::vector<uint8_t>* out);

std::string HexU64(uint64_t value);                      // 16 lower-case digits
bool ParseHexU64(std::string_view text, uint64_t* out);  // exactly 16 digits, any case
std::string HexBytes(const uint8_t* data, size_t len);
bool ParseHexBytes(std::string_view text, uint8_t* out, size_t len);  // exactly 2*len digits

inline std::string KidToHex(const XnKid& kid) { return HexBytes(kid.b.data(), kid.b.size()); }
inline bool KidFromHex(std::string_view text, XnKid* kid) {
  return ParseHexBytes(text, kid->b.data(), kid->b.size());
}
inline std::string KeyToHex(const XnKey& key) { return HexBytes(key.b.data(), key.b.size()); }
inline bool KeyFromHex(std::string_view text, XnKey* key) {
  return ParseHexBytes(text, key->b.data(), key->b.size());
}

// Dotted quad "a.b.c.d" -> host-order IPv4. Strict: four decimal octets, no extra characters.
bool ParseIpv4(std::string_view text, uint32_t* out);
std::string FormatIpv4(uint32_t ip);

// --- JSON helpers ---
// Parses without exceptions; returns a discarded value on error (check with is_discarded()).
Json Parse(std::string_view text);
// Serializes without exceptions (invalid UTF-8 is replaced).
std::string Dump(const Json& value);

// Field lookups. All return false (or nullptr) when `obj` is not an object, the key is missing,
// or the value has the wrong type or range.
const Json* Find(const Json& obj, const char* key);
bool GetU32(const Json& obj, const char* key, uint32_t* out);
bool GetU64(const Json& obj, const char* key, uint64_t* out);  // JSON unsigned number
bool GetI64(const Json& obj, const char* key, int64_t* out);   // JSON integer number
bool GetBool(const Json& obj, const char* key, bool* out);
bool GetString(const Json& obj, const char* key, std::string* out);
bool GetHexU64(const Json& obj, const char* key, uint64_t* out);  // 16-digit hex string
bool GetVip(const Json& obj, const char* key, uint32_t* out);     // dotted quad string
// Accepts either a 16-digit hex string or a JSON unsigned number.
bool GetU64Flexible(const Json& obj, const char* key, uint64_t* out);

bool ToU32(const Json& value, uint32_t* out);
bool ToI64(const Json& value, int64_t* out);

// --- XUSER data ---
// Returns false for values that cannot be sent (DataType::kNull).
bool PropertyToJson(const Property& property, Json* out);
bool PropertyFromJson(const Json& value, Property* out);
Json ContextToJson(const Context& context);
bool ContextFromJson(const Json& value, Context* out);

Json PropertiesToJson(const std::vector<Property>& properties);  // skips unsendable values
Json ContextsToJson(const std::vector<Context>& contexts);
// Malformed entries are skipped (and counted in *skipped when given).
std::vector<Property> PropertiesFromJson(const Json& array, size_t* skipped = nullptr);
std::vector<Context> ContextsFromJson(const Json& array, size_t* skipped = nullptr);

// --- SessionDoc (PROTOCOL.md 3.1) ---
struct DocMember {
  Xuid xuid = 0;
  bool is_private = false;
};

struct SessionDoc {
  XnKid session_id;
  XnKey key;
  bool has_key = false;
  uint64_t nonce = 0;  // optional: the host's arbitration nonce (session.get)
  bool has_nonce = false;
  uint32_t flags = 0;
  std::string state;
  uint32_t public_slots = 0, private_slots = 0;
  uint32_t filled_public = 0, filled_private = 0;
  PeerId host_peer_id = 0;
  uint32_t host_vip = 0;
  Xuid host_xuid = 0;
  std::string host_gamertag;
  std::vector<Context> contexts;
  std::vector<Property> properties;
  std::vector<DocMember> members;
};

// Requires session_id and host {peer_id, vip}; everything else is optional.
bool ParseSessionDoc(const Json& value, SessionDoc* out);

}  // namespace ac6net::xlive::codec
