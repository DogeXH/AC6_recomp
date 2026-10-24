// Test runner for mini_test.h cases. Usage: ac6net_tests [name-substring]
#include <chrono>
#include <exception>

#include "mini_test.h"

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0;
  for (const auto& c : mini_test::Registry()) {
    if (filter && !std::strstr(c.name, filter)) continue;
    const int before = mini_test::Failures();
    const auto t0 = std::chrono::steady_clock::now();
    std::fprintf(stderr, "[ RUN  ] %s\n", c.name);
    try {
      c.fn();
    } catch (const mini_test::RequireFailed&) {
    } catch (const std::exception& e) {
      std::fprintf(stderr, "  FAILED: exception: %s\n", e.what());
      ++mini_test::Failures();
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    std::fprintf(stderr, "[ %s ] %s (%lld ms)\n",
                 mini_test::Failures() == before ? " OK " : "FAIL", c.name, (long long)ms);
    ++ran;
  }
  std::fprintf(stderr, "%d test(s), %d failure(s)\n", ran, mini_test::Failures());
  return mini_test::Failures() ? 1 : 0;
}
