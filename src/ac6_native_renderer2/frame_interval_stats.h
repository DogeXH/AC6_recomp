#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace ac6::nr2 {

// One bounded batch of unique guest-swap intervals, in milliseconds.
// This deliberately does not use host-present intervals (which may repeat a
// guest frame). A batch can still include menus/loading: callers label scenes.
class FrameIntervalStats {
 public:
  static constexpr std::size_t kWindow = 600;
  struct Summary {
    double mean_ms, p50_ms, p95_ms, p99_ms, max_ms;
    std::size_t over_20ms, over_33ms;
  };

  bool Add(double milliseconds, Summary& result) {
    if (!std::isfinite(milliseconds) || milliseconds < 0) return false;
    values_[count_++] = milliseconds;
    if (count_ != kWindow) return false;
    double sum = 0;
    std::size_t over20 = 0, over33 = 0;
    for (double value : values_) {
      sum += value;
      over20 += value > 20.0;
      over33 += value > (1000.0 / 30.0);
    }
    std::sort(values_.begin(), values_.end());
    // Nearest-rank percentiles; the 600th observation is the maximum.
    result = {sum / kWindow, values_[299], values_[569], values_[593],
              values_.back(), over20, over33};
    count_ = 0;
    return true;
  }

 private:
  std::array<double, kWindow> values_{};
  std::size_t count_ = 0;
};
}  // namespace ac6::nr2
