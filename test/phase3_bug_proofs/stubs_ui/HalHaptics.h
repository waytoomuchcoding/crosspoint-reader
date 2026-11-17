#pragma once
#include <cstdint>

namespace HalHaptics {
inline void feedback(bool, bool, uint8_t) {}
inline void longPress(bool, uint8_t) {}
}  // namespace HalHaptics

namespace BoardConfig {
inline bool isX4Pro() { return false; }
}  // namespace BoardConfig
