#pragma once

#include <cstdint>

class GfxRenderer;

namespace shibadesk {

enum class ShibaPose : uint8_t {
  LoafSleep = 0,
  LoafAwake = 1,
  CurledCroissant = 2,
  ChinOnPaws = 3,
  Count = 4,
};

enum class ShibaIcon : uint8_t {
  WifiOn = 0,
  WifiOff,
  Book,
  Ribbon,
  Search,
  Image,
  DockLibrary,
  DockFiles,
  DockSync,
  DockSettings,
};

constexpr int SHIBA_SPRITE_W = 142;
constexpr int SHIBA_SPRITE_H = 66;
constexpr int SHIBA_LEDGE_OFFSET_Y = 54;

constexpr int SHIBA_MUG_W = 36;
constexpr int SHIBA_MUG_H = 35;
constexpr int SHIBA_MUG_LEDGE_OFFSET_Y = 33;

ShibaPose getCurrentPose();
void setCurrentPose(ShibaPose pose);
ShibaPose advancePoseCycle();

// Composites the 142x66 1-bit Shiba Inu pose onto the framebuffer so its chin/paws
// rest pixel-accurately on `ledgeY` (top border of the Hero Card or Filter Strip).
void stampShibaOnLedge(const GfxRenderer& renderer, int shibaX, int ledgeY, ShibaPose pose);

// Composites the 36x35 1-bit steaming ceramic mug + coaster resting flat on `ledgeY`.
void stampMugOnLedge(const GfxRenderer& renderer, int cupX, int ledgeY);

// Stamps a crisp 1-bit pixel icon at (x, y). `black = true` draws black ink; `false` draws white ink.
void drawIcon(const GfxRenderer& renderer, ShibaIcon icon, int x, int y, bool black = true);

int getIconWidth(ShibaIcon icon);
int getIconHeight(ShibaIcon icon);

}  // namespace shibadesk
