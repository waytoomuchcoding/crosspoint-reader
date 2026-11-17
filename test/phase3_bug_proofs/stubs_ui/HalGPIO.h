#pragma once
#include <cstdint>

#define FREEINK_CAP_TOUCH 0

class HalGPIO {
 public:
  enum { BTN_BACK = 0, BTN_CONFIRM = 1, BTN_LEFT = 2, BTN_RIGHT = 3, BTN_UP = 4, BTN_DOWN = 5, BTN_POWER = 6 };
  int pressedBtn = -1;
  int releasedBtn = -1;
  int heldBtn = -1;
  unsigned long heldMs = 0;
  bool touch = false;

  void update() {}
  bool hasTouch() const { return touch; }
  bool hasHomeKey() const { return false; }
  bool wasHomeKeyTapped() const { return false; }
  bool wasHomeKeyLongPressed() const { return false; }
  bool wasHomeKeyPressed() const { return false; }
  bool isCapacitivePagePressed(uint8_t) const { return false; }
  bool wasCapacitivePagePressed() const { return false; }
  bool wasTouchTap(float&, float&) const { return false; }
  bool wasTouchDown(float&, float&) const { return false; }
  bool isTouchTapCandidate(float&, float&, unsigned long&) const { return false; }
  bool wasTouchLongPress(float&, float&) const { return false; }
  void suppressTouchContact() {}
  bool isTouchHeldAt(float&, float&) const { return false; }
  bool wasTouchReleased() const { return false; }
  bool wasSwipe(float&, float&, float&, float&) const { return false; }
  unsigned long lastTouchHeldMs() const { return 0; }
  unsigned long getHeldTime() const { return heldMs; }
  bool wasAnyPressed() const { return pressedBtn >= 0; }
  bool wasAnyReleased() const { return releasedBtn >= 0; }
  bool wasPressed(uint8_t b) const { return pressedBtn == b; }
  bool wasReleased(uint8_t b) const { return releasedBtn == b; }
  bool isPressed(uint8_t b) const { return heldBtn == b; }
};
