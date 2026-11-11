#pragma once

#include <cstdint>

namespace trustedtime {
inline int64_t& mockNowRef() {
  static int64_t now = 1794830400LL;  // 2026-11-16T12:00:00Z
  return now;
}
inline int64_t trustedNow() { return mockNowRef(); }
}  // namespace trustedtime
