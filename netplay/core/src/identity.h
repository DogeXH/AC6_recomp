// ac6net - persistent client identity: <Config::data_dir>/identity.json (mode 0600 on POSIX;
// on Windows it inherits the per-user ACL of the data folder).
//
//   {"v":1,"sk":"<64 hex>",
//    "registration":{"server_key":"<64 hex>","peer_id":"<16 hex>","xuid":"<16 hex>",
//                    "vip":"10.64.0.7","gamertag":"Talisman"}}
//
// `registration` caches the last HELLO_ACK. It is only meaningful for the server whose public key
// is `server_key`; Netplay ignores it when the configured key differs.
#pragma once

#include <cstdint>
#include <string>

#include "ac6net/types.h"
#include "crypto.h"

namespace ac6net {

struct IdentityFile {
  crypto::SecretKey sk{};
  crypto::PublicKey pk{};  // derived from sk, not stored

  bool has_registration = false;
  std::string server_key_hex;  // lower-case
  PeerId peer_id = 0;
  Xuid xuid = 0;
  uint32_t vip = 0;  // host order
  std::string gamertag;

  // True if the cached registration belongs to the server with this public key.
  bool RegistrationMatches(const std::string& server_key_hex) const;
};

// Loads <data_dir>/identity.json, or creates a new keypair and writes the file. Creates
// `data_dir` (mode 0700) if needed. A file that exists but cannot be parsed is moved aside to
// identity.json.corrupt and replaced. With an empty `data_dir` a fresh in-memory identity is
// returned and nothing is written. Returns false only if a new identity could not be persisted
// (the in-memory identity is still filled in and usable for this run). `data_dir` is UTF-8.
// *may_save (optional) is set false when an existing identity.json could not be opened
// (Windows: still locked by another program after ~1 s); the caller must then not call
// SaveIdentity this run, or it would replace the real identity with the temporary one.
bool LoadOrCreateIdentity(const std::string& data_dir, IdentityFile* out, std::string* error,
                          bool* may_save = nullptr);

// Atomically rewrites <data_dir>/identity.json (temp file + rename, mode 0600).
bool SaveIdentity(const std::string& data_dir, const IdentityFile& id, std::string* error);

}  // namespace ac6net
