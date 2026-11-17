#pragma once
#include <cstdint>

class GfxRenderer {
 public:
  enum Orientation { Portrait = 0, LandscapeClockwise = 1, PortraitInverted = 2, LandscapeCounterClockwise = 3 };
  Orientation orientation = Portrait;
  Orientation getOrientation() const { return orientation; }
  void tapToLogical(float, float, int& x, int& y) const { x = y = 0; }
  int getScreenWidth() const { return 528; }
  int getScreenHeight() const { return 792; }
};
