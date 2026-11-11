#include "ProceduralBookCover.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "I18nKeys.h"
#include "fontIds.h"

void ProceduralBookCover::drawProceduralTxtCover(const GfxRenderer& renderer, const int x, const int y, const int w,
                                                 const int h, const std::string& title, const std::string& author,
                                                 const char* extTag) {
  if (w <= 20 || h <= 30) return;

  // White book cover field
  renderer.fillRect(x, y, w, h, false);

  const bool compact = (w < 110);
  const int spineW = compact ? 10 : 14;

  // Left stitched cloth spine (1-bit woven checkerboard + double divider + white stitch ticks)
  for (int py = 0; py < h; ++py) {
    for (int px = 0; px < spineW; ++px) {
      if (((px + py) & 1) == 0) {
        renderer.drawPixel(x + px, y + py, true);
      }
    }
  }
  renderer.fillRect(x + spineW, y, 2, h, true);
  const int stitchStep = compact ? 22 : 28;
  for (int sy = 16; sy < h - 12; sy += stitchStep) {
    renderer.fillRect(x + 2, y + sy, std::max(2, spineW - 4), 2, false);
  }

  // Subtle 6px-spaced 1-bit paper grain in cover field
  for (int py = 6; py < h - 6; py += 6) {
    for (int px = spineW + 6; px < w - 6; px += 6) {
      renderer.drawPixel(x + px, y + py, true);
    }
  }

  // Inner double-bordered white title plaque
  const int padX = compact ? 5 : 8;
  const int bottomBandH = compact ? 22 : 28;
  const int px0 = x + spineW + padX;
  const int py0 = y + (compact ? 10 : 14);
  const int plaqueW = (x + w - padX) - px0;
  const int plaqueH = (y + h - bottomBandH - 6) - py0;
  if (plaqueW <= 16 || plaqueH <= 24) return;

  renderer.fillRect(px0, py0, plaqueW, plaqueH, false);
  renderer.drawRect(px0, py0, plaqueW, plaqueH, 2, true);
  if (!compact) {
    renderer.drawRect(px0 + 3, py0 + 3, plaqueW - 6, plaqueH - 6, 1, true);
  }

  const char* tagText = (extTag && *extTag) ? extTag : "TXT";
  const int pillInset = compact ? 5 : 8;
  const int pillH = compact ? 14 : 16;
  const int pillX = px0 + pillInset;
  const int pillY = py0 + (compact ? 5 : 8);
  const int pillW = plaqueW - pillInset * 2;
  if (pillW > 8) {
    renderer.fillRect(pillX, pillY, pillW, pillH, true);
    const int tagFont = UI_6_FONT_ID;
    const std::string clampedTag = renderer.truncatedText(tagFont, tagText, pillW - 4);
    const int tw = renderer.getTextWidth(tagFont, clampedTag.c_str());
    const int th = renderer.getTextHeight(tagFont);
    renderer.drawText(tagFont, pillX + (pillW - tw) / 2, pillY + std::max(0, (pillH - th) / 2) - 1, clampedTag.c_str(),
                      false);
  }

  // Wrapped title inside plaque (up to 2 lines on compact covers, up to 3 lines on 148x222)
  const int titleFont = SMALL_FONT_ID;
  const int innerW = std::max(12, plaqueW - (compact ? 10 : 14));
  const int maxLines = compact ? 2 : 3;
  const std::string fallbackTitle = title.empty() ? std::string("TXT") : title;
  renderer.prewarmFallbackText(titleFont, fallbackTitle.c_str(), EpdFontFamily::REGULAR);
  const auto lines = renderer.wrappedText(titleFont, fallbackTitle.c_str(), innerW, maxLines);
  const int lineH = std::max(15, renderer.getLineHeight(titleFont));
  int ty = pillY + pillH + (compact ? 6 : 10);
  for (const auto& ln : lines) {
    if (ty + lineH > py0 + plaqueH - 3) break;
    const int lw = renderer.getTextWidth(titleFont, ln.c_str());
    renderer.drawText(titleFont, px0 + (plaqueW - lw) / 2, ty, ln.c_str(), true);
    ty += lineH;
  }

  // Author / local doc plaque at bottom
  const int authY = y + h - bottomBandH;
  const int authH = bottomBandH - (compact ? 6 : 8);
  if (authH >= 12) {
    renderer.fillRect(px0, authY, plaqueW, authH, false);
    renderer.drawRect(px0, authY, plaqueW, authH, 1, true);
    const std::string authLabel = !author.empty() ? author : std::string(tr(STR_TXT_BOOK_FALLBACK));
    renderer.prewarmFallbackText(UI_6_FONT_ID, authLabel.c_str(), EpdFontFamily::REGULAR);
    const std::string clampedAuth = renderer.truncatedText(UI_6_FONT_ID, authLabel.c_str(), plaqueW - 6);
    const int aw = renderer.getTextWidth(UI_6_FONT_ID, clampedAuth.c_str());
    const int ah = renderer.getTextHeight(UI_6_FONT_ID);
    renderer.drawText(UI_6_FONT_ID, px0 + (plaqueW - aw) / 2, authY + std::max(0, (authH - ah) / 2) - 1,
                      clampedAuth.c_str(), true);
  }
}

void ProceduralBookCover::drawMiniProceduralTxtCover(const GfxRenderer& renderer, const int x, const int y, const int w,
                                                     const int h, const char* extTag) {
  if (w <= 16 || h <= 24) return;

  renderer.fillRect(x, y, w, h, false);
  const int spineW = 7;
  for (int py = 0; py < h; ++py) {
    for (int px = 0; px < spineW; ++px) {
      if (((px + py) & 1) == 0) {
        renderer.drawPixel(x + px, y + py, true);
      }
    }
  }
  renderer.drawLine(x + spineW, y, x + spineW, y + h - 1, true);
  for (const int sy : {12, 28, 44, 60}) {
    if (sy < h - 6) {
      renderer.drawLine(x + 2, y + sy, x + spineW - 2, y + sy, false);
    }
  }

  const int innerX = x + spineW + 3;
  const int innerY = y + 5;
  const int innerW = w - spineW - 6;
  const int innerH = h - 10;
  if (innerW <= 10 || innerH <= 16) return;
  renderer.fillRect(innerX, innerY, innerW, innerH, false);
  renderer.drawRect(innerX, innerY, innerW, innerH, 1, true);

  const int pillX = innerX + 2;
  const int pillY = innerY + 2;
  const int pillW = innerW - 4;
  const int pillH = 13;
  renderer.fillRect(pillX, pillY, pillW, pillH, true);
  const char* label = (extTag && *extTag) ? extTag : "TXT";
  const std::string clamped = renderer.truncatedText(UI_6_FONT_ID, label, pillW - 2);
  const int tw = renderer.getTextWidth(UI_6_FONT_ID, clamped.c_str());
  const int th = renderer.getTextHeight(UI_6_FONT_ID);
  renderer.drawText(UI_6_FONT_ID, pillX + (pillW - tw) / 2, pillY + std::max(0, (pillH - th) / 2) - 1, clamped.c_str(),
                    false);

  // Decorative ruled title lines
  const int ruleX = x + spineW + 7;
  const int fullRuleW = std::max(6, w - spineW - 14);
  const int shortRuleW = std::max(4, w - spineW - 20);
  const int baseRuleY = pillY + pillH + 7;
  for (int idx = 0; idx < 4; ++idx) {
    const int ry = baseRuleY + idx * 8;
    if (ry >= y + h - 8) break;
    const int rw = (idx < 3) ? fullRuleW : shortRuleW;
    renderer.fillRect(ruleX, ry, rw, idx == 0 ? 2 : 1, true);
  }
}
