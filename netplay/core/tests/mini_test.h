// Minimal test harness shared by all ac6net tests (no external dependency).
//
//   TEST_CASE(name) { CHECK(x == y); REQUIRE(ptr); }
//
// Run:  ac6net_tests [substring-filter]
// Tests whose name contains "[server]" need a running ac6lobby; they are skipped unless the
// environment variable AC6NET_TEST_SERVER="host:port" and AC6NET_TEST_SERVER_KEY="<64 hex>" are
// set. tools/run_core_tests.sh starts a server and sets them.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>  // _getpid (MSVC has no <unistd.h>)
#else
#include <unistd.h>
#endif

namespace mini_test {

struct Case {
  const char* name;
  void (*fn)();
};

inline std::vector<Case>& Registry() {
  static std::vector<Case> cases;
  return cases;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { Registry().push_back({name, fn}); }
};

inline int& Failures() {
  static int failures = 0;
  return failures;
}

struct RequireFailed {};

inline void Fail(const char* file, int line, const char* expr, bool fatal) {
  std::fprintf(stderr, "  FAILED %s:%d: %s\n", file, line, expr);
  ++Failures();
  if (fatal) throw RequireFailed{};
}

// Process id, for per-run temp directory names.
inline long Pid() {
#if defined(_WIN32)
  return static_cast<long>(::_getpid());
#else
  return static_cast<long>(::getpid());
#endif
}

inline const char* Env(const char* name) {
  const char* v = std::getenv(name);
  return (v && *v) ? v : nullptr;
}

}  // namespace mini_test

#define MT_CONCAT2(a, b) a##b
#define MT_CONCAT(a, b) MT_CONCAT2(a, b)
#define TEST_CASE(name)                                                           \
  static void MT_CONCAT(mt_fn_, __LINE__)();                                      \
  static ::mini_test::Registrar MT_CONCAT(mt_reg_, __LINE__)(name, &MT_CONCAT(mt_fn_, __LINE__)); \
  static void MT_CONCAT(mt_fn_, __LINE__)()

#define CHECK(expr) \
  do { if (!(expr)) ::mini_test::Fail(__FILE__, __LINE__, #expr, false); } while (0)
#define REQUIRE(expr) \
  do { if (!(expr)) ::mini_test::Fail(__FILE__, __LINE__, #expr, true); } while (0)

// Skip the current test (prints a note; not a failure).
#define SKIP(msg) do { std::fprintf(stderr, "  skipped: %s\n", msg); return; } while (0)
