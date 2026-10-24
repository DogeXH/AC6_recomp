// ac6net - identity.json load/create/save (see identity.h).
#include "identity.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "ac6net/log.h"
#include "nlohmann/json.hpp"
#include "platform.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ac6net {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr const char* kFileName = "identity.json";

std::string ToLower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool ParseU64Hex(const std::string& s, uint64_t* out) {
  uint8_t b[8];
  if (!HexDecode(s, b, sizeof(b))) return false;
  uint64_t v = 0;
  for (uint8_t x : b) v = (v << 8) | x;
  *out = v;
  return true;
}

std::string U64Hex(uint64_t v) {
  uint8_t b[8];
  for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(v >> (56 - 8 * i));
  return HexEncode(b, sizeof(b));
}

const std::string* GetString(const json& obj, const char* key) {
  auto it = obj.find(key);
  if (it == obj.end() || !it->is_string()) return nullptr;
  return it->get_ptr<const std::string*>();
}

// Parses the file contents. Returns false if the secret key is missing or malformed; a malformed
// registration block is ignored (the identity is still valid).
bool ParseIdentity(const std::string& text, IdentityFile* out) {
  const json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (!doc.is_object()) return false;
  const std::string* sk = GetString(doc, "sk");
  if (!sk || !HexDecode(*sk, out->sk.data(), out->sk.size())) return false;
  out->pk = crypto::PublicKeyFromSecret(out->sk);

  out->has_registration = false;
  auto reg_it = doc.find("registration");
  if (reg_it == doc.end() || !reg_it->is_object()) return true;
  const json& reg = *reg_it;
  const std::string* server_key = GetString(reg, "server_key");
  const std::string* peer_id = GetString(reg, "peer_id");
  const std::string* xuid = GetString(reg, "xuid");
  const std::string* vip = GetString(reg, "vip");
  const std::string* gamertag = GetString(reg, "gamertag");
  uint8_t key_bytes[32];
  IdentityFile tmp;
  if (!server_key || !HexDecode(*server_key, key_bytes, sizeof(key_bytes)) || !peer_id ||
      !ParseU64Hex(*peer_id, &tmp.peer_id) || !xuid || !ParseU64Hex(*xuid, &tmp.xuid) || !vip ||
      !Ipv4FromString(*vip, &tmp.vip) || !gamertag) {
    LogF(LogLevel::kWarn, "identity: ignoring malformed cached registration");
    return true;
  }
  out->has_registration = true;
  out->server_key_hex = ToLower(*server_key);
  out->peer_id = tmp.peer_id;
  out->xuid = tmp.xuid;
  out->vip = tmp.vip;
  out->gamertag = *gamertag;
  return true;
}

std::string SerializeIdentity(const IdentityFile& id) {
  json doc = json::object();
  doc["v"] = 1;
  doc["sk"] = HexEncode(id.sk.data(), id.sk.size());
  if (id.has_registration) {
    doc["registration"] = {
        {"server_key", id.server_key_hex},
        {"peer_id", U64Hex(id.peer_id)},
        {"xuid", U64Hex(id.xuid)},
        {"vip", Ipv4ToString(id.vip)},
        {"gamertag", id.gamertag},
    };
  }
  // Replace invalid UTF-8 rather than throwing (gamertags come from the server).
  return doc.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
}

// Config::data_dir is UTF-8 (platform.h); on Windows a plain fs::path(std::string) would read it
// in the ANSI code page.
bool EnsureDirectory(const std::string& dir_utf8, std::string* error) {
  const fs::path dir = platform::PathFromUtf8(dir_utf8);
  std::error_code ec;
  if (fs::is_directory(dir, ec)) return true;
  fs::create_directories(dir, ec);
  if (ec) {
    if (error) *error = "cannot create " + dir_utf8 + ": " + ec.message();
    return false;
  }
#if !defined(_WIN32)
  ::chmod(dir.c_str(), 0700);
#endif
  return true;
}

#if defined(_WIN32)
std::string Win32ErrorText(DWORD code) {
  return "Windows error " + std::to_string(code);
}
#endif

bool WriteFileAtomic(const fs::path& path, const std::string& contents, std::string* error) {
  fs::path tmp = path;
  tmp += ".tmp";
#if defined(_WIN32)
  // Same contract as the POSIX branch: data on disk (FlushFileBuffers ~ fsync) before the file
  // replaces identity.json, so a crash or power cut leaves the old or the new file, never an
  // empty one.
  const std::string tmp_s = platform::PathToUtf8(tmp);
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (error) *error = "cannot create " + tmp_s + ": " + Win32ErrorText(GetLastError());
    return false;
  }
  size_t off = 0;
  bool ok = true;
  DWORD err = 0;
  while (off < contents.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(contents.size() - off, 1u << 20));
    DWORD written = 0;
    if (!WriteFile(h, contents.data() + off, chunk, &written, nullptr) || written == 0) {
      ok = false;
      err = GetLastError();
      break;
    }
    off += written;
  }
  if (ok && !FlushFileBuffers(h)) {
    ok = false;
    err = GetLastError();
  }
  CloseHandle(h);
  if (!ok) {
    if (error) *error = "write failed: " + tmp_s + ": " + Win32ErrorText(err);
    DeleteFileW(tmp.c_str());
    return false;
  }
  // Antivirus scanners, the search indexer and OneDrive briefly open fresh files without
  // FILE_SHARE_DELETE; MoveFileEx then fails with a sharing violation or access denied. Retry
  // for about a second before giving up.
  for (int attempt = 0;; ++attempt) {
    if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      return true;
    err = GetLastError();
    const bool transient = err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED ||
                           err == ERROR_LOCK_VIOLATION;
    if (!transient || attempt >= 20) break;
    Sleep(50);
  }
  if (error) *error = "rename failed: " + tmp_s + ": " + Win32ErrorText(err);
  DeleteFileW(tmp.c_str());
  return false;
#else
  const std::string tmp_s = tmp.string();
  ::unlink(tmp_s.c_str());
  const int fd = ::open(tmp_s.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0600);
  if (fd < 0) {
    if (error) *error = "cannot create " + tmp_s + ": " + std::strerror(errno);
    return false;
  }
  size_t off = 0;
  while (off < contents.size()) {
    const ssize_t n = ::write(fd, contents.data() + off, contents.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (error) *error = "write failed: " + tmp_s + ": " + std::strerror(errno);
      ::close(fd);
      ::unlink(tmp_s.c_str());
      return false;
    }
    off += static_cast<size_t>(n);
  }
  ::fsync(fd);
  ::close(fd);
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    if (error) *error = "rename failed: " + ec.message();
    fs::remove(tmp, ec);
    return false;
  }
  ::chmod(path.string().c_str(), 0600);
  return true;
#endif
}

}  // namespace

bool IdentityFile::RegistrationMatches(const std::string& key_hex) const {
  return has_registration && !server_key_hex.empty() && server_key_hex == ToLower(key_hex);
}

bool LoadOrCreateIdentity(const std::string& data_dir, IdentityFile* out, std::string* error,
                          bool* may_save) {
  *out = IdentityFile{};
  if (may_save) *may_save = true;
  if (data_dir.empty()) {
    crypto::GenerateKeypair(&out->sk, &out->pk);
    LogF(LogLevel::kWarn, "identity: no data_dir; using an ephemeral identity");
    return true;
  }
  if (!EnsureDirectory(data_dir, error)) {
    crypto::GenerateKeypair(&out->sk, &out->pk);
    return false;
  }
  const fs::path path = platform::PathFromUtf8(data_dir) / kFileName;
  std::error_code ec;
  if (fs::exists(path, ec)) {
    std::ifstream f(path, std::ios::binary);
#if defined(_WIN32)
    // An antivirus scan or a OneDrive sync can hold identity.json without read sharing for a
    // moment. "Cannot open" is not "corrupt": wait for it, and if it stays locked keep the file
    // (and the account it holds) and run with a throwaway identity instead of replacing it.
    for (int attempt = 0; !f.is_open() && attempt < 20; ++attempt) {
      Sleep(50);
      f.clear();
      f.open(path, std::ios::binary);
    }
    if (!f.is_open()) {
      crypto::GenerateKeypair(&out->sk, &out->pk);
      if (may_save) *may_save = false;
      if (error) {
        *error = "cannot open " + platform::PathToUtf8(path) +
                 " (in use by another program?); kept it, using a temporary identity";
      }
      return false;
    }
#endif
    std::stringstream ss;
    ss << f.rdbuf();
    // identity.json is tiny; anything large is not ours.
    const std::string text = ss.str().size() <= 64 * 1024 ? ss.str() : std::string();
    if (f && ParseIdentity(text, out)) return true;
    f.close();  // Windows cannot rename a file that is still open
    fs::path aside = path;
    aside += ".corrupt";
    fs::rename(path, aside, ec);
    LogF(LogLevel::kWarn, "identity: %s is unreadable; moved aside, creating a new identity",
         platform::PathToUtf8(path).c_str());
    *out = IdentityFile{};
  }
  crypto::GenerateKeypair(&out->sk, &out->pk);
  return SaveIdentity(data_dir, *out, error);
}

bool SaveIdentity(const std::string& data_dir, const IdentityFile& id, std::string* error) {
  if (data_dir.empty()) return true;
  if (!EnsureDirectory(data_dir, error)) return false;
  return WriteFileAtomic(platform::PathFromUtf8(data_dir) / kFileName, SerializeIdentity(id),
                         error);
}

}  // namespace ac6net
