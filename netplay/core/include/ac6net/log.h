// ac6net - logging and JSONL tracing.
#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace ac6net {

enum class LogLevel { kTrace = 0, kDebug, kInfo, kWarn, kError };

// The glue installs a sink that forwards to the runtime logger (REXKRNL_*). Default: stderr.
using LogSink = std::function<void(LogLevel, std::string_view)>;
void SetLogSink(LogSink sink);

// printf-style logging.
void LogF(LogLevel level, const char* fmt, ...)
#if defined(__MINGW32__)
    // MinGW's plain `printf` archetype is msvcrt's (no %zu/%lld); log.cpp formats with the
    // C99-conforming __mingw_vsnprintf there.
    __attribute__((format(gnu_printf, 2, 3)))
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

// Trace: appends one JSON object per line to Config::trace_path (if set):
//   {"t_ms":<ms since start>,"ev":"<event>", <fields_json_without_braces>}
// `fields_json` must be a valid JSON object body such as "\"vip\":\"10.64.0.7\",\"n\":3" or "".
// Thread-safe. Cheap no-op when tracing is off.
void Trace(std::string_view event, std::string_view fields_json = {});
bool TraceEnabled();

// Opens (append) the process-wide trace file; an empty path turns tracing off. Netplay::Start
// calls this with Config::trace_path. Returns false if the file cannot be opened (tracing off).
// Added by the overlay lane (2026-10-19); backward-compatible.
bool SetTracePath(std::string_view path);

// Escape a string for embedding inside JSON quotes.
std::string JsonEscape(std::string_view s);

}  // namespace ac6net
