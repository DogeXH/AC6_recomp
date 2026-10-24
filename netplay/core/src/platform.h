// ac6net - small OS helpers: UTF-8 paths and thread names.
//
// Paths in ac6net::Config (data_dir, trace_path) are UTF-8. On POSIX that is the native narrow
// encoding and these helpers are plain conversions. On Windows a std::filesystem::path built from
// a std::string uses the ANSI code page instead, so a user folder such as C:\Users\Jose\ (with an
// accent) or a non-Latin user name would be mangled or make path::string() throw; the helpers
// convert through UTF-16 explicitly.
#pragma once

#include <cstdio>
#include <filesystem>
#include <string>

namespace ac6net::platform {

// UTF-8 text -> path. Windows: a string that is not valid UTF-8 is taken as ANSI-code-page text
// (what std::filesystem::path(std::string) would do), so callers that still pass ANSI keep
// working.
std::filesystem::path PathFromUtf8(const std::string& utf8);

// path -> UTF-8 text for logs and error messages. Never throws.
std::string PathToUtf8(const std::filesystem::path& path);

// std::fopen with a UTF-8 path (Windows: _wfopen).
std::FILE* OpenFileUtf8(const std::string& utf8_path, const char* mode);

// Names the calling thread for debuggers and crash dumps (Windows 10 1607+:
// SetThreadDescription, looked up at run time). No-op elsewhere.
void SetCurrentThreadName(const char* ascii_name);

}  // namespace ac6net::platform
