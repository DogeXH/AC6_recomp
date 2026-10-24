#pragma once

#include <chrono>

namespace rex::graphics::detail {
struct PresentDeadline {
  std::chrono::steady_clock::time_point wake;
  std::chrono::steady_clock::time_point next;
};

// Retain at most a quarter-frame of phase debt. A larger stall starts a fresh
// schedule, so missed deadlines cannot produce a burst of catch-up presents.
inline PresentDeadline SchedulePresentDeadline(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point deadline,
    std::chrono::steady_clock::duration interval, bool preserve_phase) {
  if (deadline < now &&
      (!preserve_phase || now - deadline > interval / 4)) {
    deadline = now;
  }
  return {deadline, deadline + interval};
}
}  // namespace rex::graphics::detail
