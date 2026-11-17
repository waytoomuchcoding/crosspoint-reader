#pragma once
#include <cstdint>

namespace freeink {
namespace ui {
enum class ScreenEdge : uint8_t { Left, Right, Top, Bottom };
enum class SwipeDir : uint8_t { None, Left, Right, Up, Down };
inline SwipeDir swipeDirection(int, int, int, int) { return SwipeDir::None; }
inline bool edgeSwipe(ScreenEdge, int, int, int, int, int, int) { return false; }
struct ListItem {
  const char* label = "";
  const char* value = "";
  int16_t actionValue = 0;
};
}  // namespace ui
}  // namespace freeink
