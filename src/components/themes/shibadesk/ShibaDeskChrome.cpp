#include "ShibaDeskChrome.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "I18nKeys.h"
#include "ShibaTextLayout.h"
#include "fontIds.h"

namespace shibadesk {
namespace {

void fillHatch45(const GfxRenderer& renderer, const int x0, const int y0, const int w, const int h, const bool dense) {
  if (w <= 0 || h <= 0) return;
  renderer.fillRect(x0, y0, w, h, false);
  const int x1 = x0 + w;
  const int y1 = y0 + h;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const bool ink = dense ? (((x + y) & 3) < 2) : (((x + y) % 3) == 0);
      if (ink) {
        renderer.drawPixel(x, y, true);
      }
    }
  }
}

}  // namespace

void drawStaggeredDotBackground(const GfxRenderer& renderer, const Rect rect) {
  if (rect.width <= 0 || rect.height <= 0) return;
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  const int xEnd = rect.x + rect.width;
  const int yEnd = rect.y + rect.height;
  const int firstY = rect.y + ((5 - (rect.y % 5)) % 5);
  for (int y = firstY; y < yEnd; y += 5) {
    const int rowPhase = ((y / 5) & 1) * 5;
    int firstX = (rect.x / 10) * 10 + rowPhase;
    if (firstX < rect.x) firstX += 10;
    for (int x = firstX; x < xEnd; x += 10) {
      renderer.drawPixel(x, y, true);
    }
  }
}

void drawShibaCard(const GfxRenderer& renderer, const Rect rect, const bool focused, const int shadowPx) {
  if (rect.width <= 0 || rect.height <= 0) return;

  if (focused) {
    const int haloPad = 3;
    renderer.fillRect(rect.x - haloPad, rect.y - haloPad, rect.width + shadowPx + haloPad + 2,
                      rect.height + shadowPx + haloPad + 2, false);
    const int sx = rect.x + 4;
    const int sy = rect.y + 4;
    const int sw = rect.width + std::max(0, shadowPx - 3);
    const int sh = rect.height + std::max(0, shadowPx - 3);
    fillHatch45(renderer, sx, sy, sw, sh, true);
    renderer.drawRect(sx, sy, sw, sh, true);

    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
    renderer.drawRect(rect.x, rect.y, rect.width, rect.height, 3, true);
    if (rect.width > 10 && rect.height > 10) {
      renderer.drawRect(rect.x + 4, rect.y + 4, rect.width - 8, rect.height - 8, 1, true);
    }
  } else {
    if (shadowPx > 0) {
      const int sx = rect.x + shadowPx;
      const int sy = rect.y + shadowPx;
      fillHatch45(renderer, sx, sy, rect.width, rect.height, false);
      renderer.drawRect(sx, sy, rect.width, rect.height, true);
    }
    renderer.fillRect(rect.x - 1, rect.y - 1, rect.width + 2, rect.height + 2, false);
    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
    renderer.drawRect(rect.x, rect.y, rect.width, rect.height, 2, true);
  }
}

void drawShibaPopupChrome(const GfxRenderer& renderer, const Rect rect, const int shadowPx) {
  if (rect.width <= 0 || rect.height <= 0) return;
  renderer.fillRect(rect.x - 4, rect.y - 4, rect.width + shadowPx + 8, rect.height + shadowPx + 8, false);
  if (shadowPx > 0) {
    fillHatch45(renderer, rect.x + shadowPx, rect.y + shadowPx, rect.width, rect.height, true);
    renderer.drawRect(rect.x + shadowPx, rect.y + shadowPx, rect.width, rect.height, true);
  }
}

int measurePillWidth(const GfxRenderer& renderer, const int fontId, const char* text, const int padX) {
  if (!text || *text == '\0') return padX;
  renderer.prewarmFallbackText(fontId, text, EpdFontFamily::REGULAR);
  return renderer.getTextWidth(fontId, text, EpdFontFamily::REGULAR) + padX;
}

void drawShibaBadgePill(const GfxRenderer& renderer, const int x, const int y, const int height, const int fontId,
                        const char* text, const bool filledBlack, const bool urgent) {
  if (!text || *text == '\0' || height <= 4) return;
  renderer.prewarmFallbackText(fontId, text, EpdFontFamily::REGULAR);
  const int textW = renderer.getTextWidth(fontId, text, EpdFontFamily::REGULAR);
  const int textH = renderer.getTextHeight(fontId);

  if (urgent) {
    const int outerW = textW + 24;
    renderer.fillRect(x, y, outerW, height, false);
    renderer.drawRect(x, y, outerW, height, 1, true);
    renderer.fillRect(x + 2, y + 2, outerW - 4, height - 4, true);
    const int textX = x + (outerW - textW) / 2;
    const int textY = y + std::max(1, (height - textH) / 2);
    renderer.drawText(fontId, textX, textY, text, false, EpdFontFamily::REGULAR);
    return;
  }

  const int pillW = textW + 18;
  if (filledBlack) {
    renderer.fillRect(x, y, pillW, height, true);
    const int textX = x + (pillW - textW) / 2;
    const int textY = y + std::max(1, (height - textH) / 2) - 1;
    renderer.drawText(fontId, textX, textY, text, false, EpdFontFamily::REGULAR);
  } else {
    renderer.fillRect(x, y, pillW, height, false);
    renderer.drawRect(x, y, pillW, height, 1, true);
    const int textX = x + (pillW - textW) / 2;
    const int textY = y + std::max(1, (height - textH) / 2) - 1;
    renderer.drawText(fontId, textX, textY, text, true, EpdFontFamily::REGULAR);
  }
}

void drawSegmentedProgressBar(const GfxRenderer& renderer, const Rect rect, const int percent) {
  if (rect.width <= 4 || rect.height <= 4) return;
  const int clamped = std::clamp(percent, 0, 100);
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height, 2, true);

  const int fillW = (rect.width * clamped) / 100;
  if (fillW > 0) {
    renderer.fillRect(rect.x, rect.y, fillW, rect.height, true);
  }

  const int tickTop = rect.y + 3;
  const int tickBottom = rect.y + rect.height - 4;
  if (tickBottom >= tickTop) {
    for (int tx = rect.x + 12; tx < rect.x + rect.width - 4; tx += 12) {
      if (tx > rect.x + fillW + 2) {
        renderer.drawLine(tx, tickTop, tx, tickBottom, true);
      }
    }
  }
}

void drawWoodgrainShelfLedge(const GfxRenderer& renderer, const int x, const int y, const int width, const int height) {
  if (width <= 8 || height <= 4) return;
  renderer.fillRect(x, y, width, height, false);
  renderer.drawRect(x, y, width, height, 2, true);

  const int ix0 = x + 2;
  const int iy0 = y + 2;
  const int ix1 = x + width - 2;
  const int iy1 = y + height - 2;
  for (int py = iy0; py < iy1; ++py) {
    for (int px = ix0; px < ix1; ++px) {
      if ((py % 3 == 0) || (((px + py * 3) % 13) == 0)) {
        renderer.drawPixel(px, py, true);
      }
    }
  }

  // Crisp corner shelf support brackets beneath the ledge
  const int bBottom = y + height + 9;
  const int leftX[3] = {x + 24, x + 38, x + 24};
  const int leftY[3] = {y + height - 1, y + height - 1, bBottom};
  renderer.fillPolygon(leftX, leftY, 3, true);

  const int rightX[3] = {x + width - 38, x + width - 24, x + width - 24};
  const int rightY[3] = {y + height - 1, y + height - 1, bBottom};
  renderer.fillPolygon(rightX, rightY, 3, true);
}

void drawSparseShelfBookendAndSlot(const GfxRenderer& renderer, const int startSlotIdx, const int firstCardX,
                                   const int cardStepX, const int shelfTopY, const int rightEdgeX) {
  const int bx = firstCardX + startSlotIdx * cardStepX;
  if (bx >= rightEdgeX - 20) return;

  // Heavy L-shaped architectural metal bookend pressing against the last book
  renderer.fillRect(bx - 6, shelfTopY - 106, 6, 106, true);
  renderer.fillRect(bx - 6, shelfTopY - 5, 60, 5, true);

  // Ceramic pot + upright sprout leaves resting on the bookend base (zero paw prints)
  const int potX = bx + 12;
  const int potTopY = shelfTopY - 36;
  const int potBaseY = shelfTopY - 5;
  const int potPolyX[4] = {potX, potX + 34, potX + 29, potX + 5};
  const int potPolyY[4] = {potTopY, potTopY, potBaseY, potBaseY};
  renderer.fillPolygon(potPolyX, potPolyY, 4, false);
  for (int i = 0; i < 4; ++i) {
    const int n = (i + 1) & 3;
    renderer.drawLine(potPolyX[i], potPolyY[i], potPolyX[n], potPolyY[n], 2, true);
  }
  renderer.fillRect(potX - 2, potTopY - 6, 38, 6, false);
  renderer.drawRect(potX - 2, potTopY - 6, 38, 6, 2, true);

  const int leaf1X[3] = {potX + 17, potX + 9, potX + 19};
  const int leaf1Y[3] = {potTopY - 6, potTopY - 38, potTopY - 24};
  renderer.fillPolygon(leaf1X, leaf1Y, 3, true);

  const int leaf2X[3] = {potX + 17, potX + 27, potX + 22};
  const int leaf2Y[3] = {potTopY - 6, potTopY - 42, potTopY - 6};
  renderer.fillPolygon(leaf2X, leaf2Y, 3, false);
  renderer.drawLine(leaf2X[0], leaf2Y[0], leaf2X[1], leaf2Y[1], 2, true);
  renderer.drawLine(leaf2X[1], leaf2Y[1], leaf2X[2], leaf2Y[2], 2, true);

  // Calm dashed empty-shelf hint card in remaining right space when wide enough
  const int rx0 = bx + 72;
  const int rx1 = rightEdgeX - 2;
  const int ry0 = shelfTopY - 204;
  const int ry1 = shelfTopY - 18;
  const int rw = rx1 - rx0;
  const int rh = ry1 - ry0;
  if (rw < 64 || rh < 80) return;

  renderer.fillRect(rx0, ry0, rw, rh, false);
  for (int x = rx0; x < rx1; x += 10) {
    const int segEnd = std::min(x + 5, rx1);
    renderer.drawLine(x, ry0, segEnd, ry0, 2, true);
    renderer.drawLine(x, ry1, segEnd, ry1, 2, true);
  }
  for (int y = ry0; y < ry1; y += 10) {
    const int segEnd = std::min(y + 5, ry1);
    renderer.drawLine(rx0, y, rx0, segEnd, 2, true);
    renderer.drawLine(rx1, y, rx1, segEnd, 2, true);
  }

  const int innerW = rw - 16;
  const int midX = rx0 + rw / 2;
  if (rw >= 150) {
    const char* rawT1 = tr(STR_DESK_EMPTY_SLOTS_TITLE);
    const EpdFontFamily::Style t1Style = styleForText(rawT1, true);
    const std::string t1 = clampSingleLine(renderer, SMALL_FONT_ID, rawT1, innerW, t1Style);
    const std::string t2 = clampSingleLine(renderer, UI_6_FONT_ID, tr(STR_DESK_EMPTY_SLOTS_SYNC), innerW);
    const std::string t3 = clampSingleLine(renderer, UI_6_FONT_ID, tr(STR_DESK_EMPTY_SLOTS_FILES), innerW);

    renderer.drawText(SMALL_FONT_ID, midX - renderer.getTextWidth(SMALL_FONT_ID, t1.c_str(), t1Style) / 2, ry0 + 54,
                      t1.c_str(), true, t1Style);
    renderer.drawText(UI_6_FONT_ID, midX - renderer.getTextWidth(UI_6_FONT_ID, t2.c_str()) / 2, ry0 + 84, t2.c_str(),
                      true);
    renderer.drawText(UI_6_FONT_ID, midX - renderer.getTextWidth(UI_6_FONT_ID, t3.c_str()) / 2, ry0 + 104, t3.c_str(),
                      true);
  } else {
    // Single remaining slot (2 books on shelf): compact centered "+ Sync" hint
    const std::string plusSync = std::string("+ ") + tr(STR_DESK_DOCK_SYNC);
    const std::string clamped = clampSingleLine(renderer, SMALL_FONT_ID, plusSync.c_str(), innerW);
    renderer.drawText(SMALL_FONT_ID, midX - renderer.getTextWidth(SMALL_FONT_ID, clamped.c_str()) / 2,
                      ry0 + rh / 2 - renderer.getTextHeight(SMALL_FONT_ID) / 2, clamped.c_str(), true);
  }
}

std::string clampSingleLine(const GfxRenderer& renderer, const int fontId, const char* text, const int maxWidthPx,
                            const EpdFontFamily::Style style) {
  if (!text || *text == '\0' || maxWidthPx <= 0) return {};
  renderer.prewarmFallbackText(fontId, text, style);
  return renderer.truncatedText(fontId, text, maxWidthPx, style);
}

std::vector<std::string> wrapSmartLines(const GfxRenderer& renderer, const int fontId, const char* text,
                                        const int maxWidthPx, const int maxLines, const EpdFontFamily::Style style) {
  if (!text || *text == '\0' || maxWidthPx <= 0 || maxLines <= 0) return {};
  renderer.prewarmFallbackText(fontId, text, style);
  return wrapSmartLinesWith(
      text, maxWidthPx, maxLines, [&](const char* s) { return renderer.getTextWidth(fontId, s, style); },
      [&](const char* s, const int w) { return renderer.truncatedText(fontId, s, w, style); });
}

}  // namespace shibadesk
