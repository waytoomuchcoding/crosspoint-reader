#pragma once

#include <cstddef>
#include <cstdint>

class GfxRenderer;
class Page;

namespace page_ornament {

enum class Theme : uint8_t {
  Off = 0,
  BotanicalFloral = 1,     // Western botanical woodcut: roses, cosmos, leaves & spiral tendrils
  PlumAndBamboo = 2,       // Classical ink-wash: gnarled plum blossoms, bamboo & mountain mist
  CelestialAstrolabe = 3,  // Astronomical astrolabe, 3D dithered moon phases & constellations
  MorrisFiligree = 4,      // Euler-spiral scrollwork, acanthus fronds & halftone medallions
  WavesAndPetals = 5,      // Woodblock scalloped Seigaiha waves & drifting notched sakura petals
  AutoCycle = 6,           // Deterministically selects a theme from book/chapter seed
  Count = 7
};

struct RenderContext {
  uint32_t bookSeed = 0;
  uint16_t spineIndex = 0;
  uint16_t pageIndex = 0;
  int fontId = 0;
  int marginTop = 0;
  int marginRight = 0;
  int marginBottom = 0;
  int marginLeft = 0;
  Theme theme = Theme::Off;
};

struct KeepoutRect {
  int16_t x0 = 0;
  int16_t y0 = 0;
  int16_t x1 = 0;
  int16_t y1 = 0;
};

// Detects dead-space regions on an already-rendered B/W page (using both structural
// Page element spans and the actual 1-bit framebuffer ink occupancy) and fills
// qualifying dead spaces with procedural 1-bit dithered designs.
// Strictly preserves a clear breathing halo around all text, images, borders, and chrome.
void renderDeadSpaceOrnaments(const GfxRenderer& renderer, const Page& page, const RenderContext& ctx);

// Fills open negative space on an arbitrary full-screen B/W canvas (such as staggered
// sleep-screen vocabulary cards) while strictly respecting explicit keepout rectangles
// and any already-drawn ink pixels.
void renderCanvasOrnaments(const GfxRenderer& renderer, Theme theme, uint32_t seed, const KeepoutRect* keepouts,
                           size_t keepoutCount, int marginPx = 6, float haloPx = 9.5f);

}  // namespace page_ornament
