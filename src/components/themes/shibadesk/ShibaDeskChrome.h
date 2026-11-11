#pragma once

#include <EpdFontFamily.h>

#include <string>
#include <vector>

#include "components/themes/BaseTheme.h"
#include "components/themes/shibadesk/ShibaTextLayout.h"

class GfxRenderer;

namespace shibadesk {

// Returns BOLD only for pure Latin/ASCII strings; CJK bitmap glyphs always render in crisp
// 1px REGULAR strokes so synthetic horizontal bolding never smears ideographs.
inline EpdFontFamily::Style styleForText(const char* text, const bool latinBold = false) {
  return (latinBold && text && !hasCjkCodepoints(text)) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
}

// Fills `rect` with white and stamps a calm 1-bit staggered architectural dot grid
// (`y % 5 == 0 && x % 10 == ((y / 5) % 2) * 5`) so white cards pop cleanly in 1-bit BW.
void drawStaggeredDotBackground(const GfxRenderer& renderer, Rect rect);

// Draws a crisp 1-bit card with white fill, solid/hatched offset drop shadow, and
// unmistakable double-ink focus framing (never 50% checkerboard over text).
void drawShibaCard(const GfxRenderer& renderer, Rect rect, bool focused, int shadowPx = 4);

// Draws a crisp 1-bit double-ink popup frame + offset drop shadow around `rect`.
void drawShibaPopupChrome(const GfxRenderer& renderer, Rect rect, int shadowPx = 4);

// Draws a high-contrast 1-bit pill tag (either filled black with white text, or
// outlined white with black text; `urgent` adds a double-ink outer frame).
int measurePillWidth(const GfxRenderer& renderer, int fontId, const char* text, int padX = 18);
void drawShibaBadgePill(const GfxRenderer& renderer, int x, int y, int height, int fontId, const char* text,
                        bool filledBlack = true, bool urgent = false);

// Draws a crisp 1-bit segmented progress bar: solid black fill for completed portion
// and 8px-spaced vertical tick marks in the unfilled track.
void drawSegmentedProgressBar(const GfxRenderer& renderer, Rect rect, int percent);

// Draws the 1-bit woodgrain bookshelf ledge + lower corner support brackets.
void drawWoodgrainShelfLedge(const GfxRenderer& renderer, int x, int y, int width, int height);

// Draws the sparse-shelf L-shaped architectural metal bookend, potted sprout plant,
// and calm dashed empty-slot hint card when fewer than 3 recent books sit on the shelf.
void drawSparseShelfBookendAndSlot(const GfxRenderer& renderer, int startSlotIdx, int firstCardX, int cardStepX,
                                   int shelfTopY, int rightEdgeX);

// Truncates `text` at UTF-8 codepoint boundaries to fit `maxWidthPx`, appending "..." / "…" cleanly.
std::string clampSingleLine(const GfxRenderer& renderer, int fontId, const char* text, int maxWidthPx,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR);

// Kinsoku-aware CJK + word-boundary Latin smart line wrapper (up to `maxLines`, clamping final line).
std::vector<std::string> wrapSmartLines(const GfxRenderer& renderer, int fontId, const char* text, int maxWidthPx,
                                        int maxLines, EpdFontFamily::Style style = EpdFontFamily::REGULAR);

}  // namespace shibadesk
