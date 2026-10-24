// ac6net - log sink, printf-style logging and the JSONL trace (ac6net/log.h).
#include "ac6net/log.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

#include "platform.h"

// MinGW: msvcrt's vsnprintf does not understand C99 length modifiers such as %zu.
#if defined(__MINGW32__)
#define AC6NET_VSNPRINTF __mingw_vsnprintf
#else
#define AC6NET_VSNPRINTF std::vsnprintf
#endif

namespace ac6net {

namespace {

// Captured during static initialisation, which is as close to process start as we can get
// portably. Trace timestamps are relative to it.
const std::chrono::steady_clock::time_point g_process_start = std::chrono::steady_clock::now();

std::mutex& SinkMutex() {
  static std::mutex mu;
  return mu;
}

LogSink& Sink() {
  static LogSink sink;
  return sink;
}

const char* LevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kTrace: return "trace";
    case LogLevel::kDebug: return "debug";
    case LogLevel::kInfo: return "info";
    case LogLevel::kWarn: return "warn";
    case LogLevel::kError: return "error";
  }
  return "?";
}

struct TraceState {
  std::mutex mu;
  std::FILE* file = nullptr;
  std::string path;
};

TraceState& TraceGlobals() {
  static TraceState state;
  return state;
}

std::atomic<bool> g_trace_enabled{false};

}  // namespace

void SetLogSink(LogSink sink) {
  std::lock_guard<std::mutex> lock(SinkMutex());
  Sink() = std::move(sink);
}

void LogF(LogLevel level, const char* fmt, ...) {
  char stack_buf[1024];
  va_list args;
  va_start(args, fmt);
  va_list args_copy;
  va_copy(args_copy, args);
  const int n = AC6NET_VSNPRINTF(stack_buf, sizeof(stack_buf), fmt, args);
  va_end(args);

  std::string message;
  if (n < 0) {
    message = "(log format error)";
  } else if (static_cast<size_t>(n) < sizeof(stack_buf)) {
    message.assign(stack_buf, static_cast<size_t>(n));
  } else {
    message.resize(static_cast<size_t>(n) + 1);
    AC6NET_VSNPRINTF(message.data(), message.size(), fmt, args_copy);
    message.resize(static_cast<size_t>(n));
  }
  va_end(args_copy);

  // Copy the sink so it is never invoked with our mutex held (a sink may log or block).
  LogSink sink;
  {
    std::lock_guard<std::mutex> lock(SinkMutex());
    sink = Sink();
  }
  if (sink) {
    sink(level, message);
  } else {
    std::fprintf(stderr, "[ac6net %s] %s\n", LevelName(level), message.c_str());
  }
}

bool SetTracePath(std::string_view path) {
  TraceState& st = TraceGlobals();
  std::lock_guard<std::mutex> lock(st.mu);
  if (st.file && st.path == path) return true;
  if (st.file) {
    std::fclose(st.file);
    st.file = nullptr;
  }
  st.path.clear();
  g_trace_enabled.store(false, std::memory_order_relaxed);
  if (path.empty()) return true;
  st.path.assign(path.data(), path.size());
  st.file = platform::OpenFileUtf8(st.path, "ab");  // UTF-8 path (Windows: _wfopen)
  if (!st.file) {
    st.path.clear();
    return false;
  }
  g_trace_enabled.store(true, std::memory_order_relaxed);
  return true;
}

bool TraceEnabled() { return g_trace_enabled.load(std::memory_order_relaxed); }

void Trace(std::string_view event, std::string_view fields_json) {
  if (!TraceEnabled()) return;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - g_process_start)
                      .count();
  std::string line;
  line.reserve(48 + event.size() + fields_json.size());
  line += "{\"t_ms\":";
  line += std::to_string(ms);
  line += ",\"ev\":\"";
  line += JsonEscape(event);
  line += '"';
  if (!fields_json.empty()) {
    line += ',';
    line.append(fields_json.data(), fields_json.size());
  }
  line += "}\n";

  TraceState& st = TraceGlobals();
  std::lock_guard<std::mutex> lock(st.mu);
  if (!st.file) return;
  std::fwrite(line.data(), 1, line.size(), st.file);
  std::fflush(st.file);
}

std::string JsonEscape(std::string_view s) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(s.size() + 8);
  for (char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20 || c == 0x7F) {
          out += "\\u00";
          out += kHex[c >> 4];
          out += kHex[c & 0x0F];
        } else {
          out += ch;
        }
    }
  }
  return out;
}

}  // namespace ac6net
