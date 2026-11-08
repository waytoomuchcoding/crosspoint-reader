#pragma once

#include "activities/Activity.h"

// Steps the Zhuyin Ruby frequency threshold from the reader (◀ more ruby,
// ▶ less ruby). Confirm saves it and returns a non-cancelled result so the
// reader relays out the chapter; Back leaves the setting unchanged.
class ZhRubyAdjustActivity final : public Activity {
 public:
  ZhRubyAdjustActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ZhRubyAdjust", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  int position = 0;  // index into the display order, see the .cpp
};
