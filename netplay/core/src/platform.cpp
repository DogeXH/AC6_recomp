// ac6net - OS helpers (see platform.h).
#include "platform.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace ac6net::platform {

#if defined(_WIN32)

namespace {

// UTF-8 -> UTF-16; false if `s` is not well-formed UTF-8.
bool Utf8ToWide(const std::string& s, std::wstring* out) {
  out->clear();
  if (s.empty()) return true;
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return false;
  out->resize(static_cast<size_t>(n));
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                             out->data(), n) == n;
}

}  // namespace

std::filesystem::path PathFromUtf8(const std::string& utf8) {
  std::wstring wide;
  if (Utf8ToWide(utf8, &wide)) return std::filesystem::path(wide);
  // Not UTF-8: an ANSI-code-page string from an older caller.
  try {
    return std::filesystem::path(utf8);
  } catch (...) {
    return std::filesystem::path();
  }
}

std::string PathToUtf8(const std::filesystem::path& path) {
  const std::wstring& w = path.native();
  if (w.empty()) return std::string();
  // Unpaired surrogates (legal in NTFS names) become U+FFFD instead of failing.
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                    nullptr, nullptr);
  if (n <= 0) return "<unprintable path>";
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr,
                      nullptr);
  return out;
}

std::FILE* OpenFileUtf8(const std::string& utf8_path, const char* mode) {
  const std::filesystem::path p = PathFromUtf8(utf8_path);
  if (p.empty()) return nullptr;
  std::wstring wmode;
  for (const char* m = mode; *m; ++m) wmode.push_back(static_cast<wchar_t>(*m));
  return _wfopen(p.c_str(), wmode.c_str());
}

void SetCurrentThreadName(const char* ascii_name) {
  using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
  static const auto fn = reinterpret_cast<SetThreadDescriptionFn>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
  if (!fn || !ascii_name) return;
  std::wstring w;
  for (const char* c = ascii_name; *c; ++c) w.push_back(static_cast<wchar_t>(*c));
  fn(GetCurrentThread(), w.c_str());
}

#else  // POSIX: UTF-8 is the native narrow encoding.

std::filesystem::path PathFromUtf8(const std::string& utf8) { return std::filesystem::path(utf8); }

std::string PathToUtf8(const std::filesystem::path& path) { return path.string(); }

std::FILE* OpenFileUtf8(const std::string& utf8_path, const char* mode) {
  return std::fopen(utf8_path.c_str(), mode);
}

void SetCurrentThreadName(const char*) {}

#endif

}  // namespace ac6net::platform
