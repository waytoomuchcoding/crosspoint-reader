#include "ShibaDeskTheme.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "ShibaDeskChrome.h"
#include "ShibaStatusBar.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ShibaDeskTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                     const char* btn4) const {
  shibadesk::drawShibaMappedKeyHints(renderer, btn1, btn2, btn3, btn4);
}

Rect ShibaDeskTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int maxPopupW = std::max(120, screenW - 48);
  const int maxTextW = std::max(80, maxPopupW - metrics.popupMarginX * 2);

  const char* rawMsg = message ? message : "";
  const int fontId = shibadesk::hasCjkCodepoints(rawMsg) ? SMALL_FONT_ID : UI_10_FONT_ID;
  const EpdFontFamily::Style style = shibadesk::styleForText(rawMsg, true);
  const std::string clamped = shibadesk::clampSingleLine(renderer, fontId, rawMsg, maxTextW, style);
  const int textW = renderer.getTextWidth(fontId, clamped.c_str(), style);
  const int textH = renderer.getLineHeight(fontId);
  const int w = std::min(maxPopupW, textW + metrics.popupMarginX * 2);
  const int h = textH + metrics.popupMarginY * 2 + 8;
  const int x = (screenW - w) / 2;
  const int y = static_cast<int>(screenH * metrics.popupTopOffsetRatio);

  const Rect box{x, y, w, h};
  shibadesk::drawShibaPopupChrome(renderer, box, 5);
  renderer.fillRoundedRect(x, y, w, h, 8, Color::White);
  renderer.drawRoundedRect(x, y, w, h, 3, 8, true);
  if (w > 12 && h > 12) {
    renderer.drawRoundedRect(x + 4, y + 4, w - 8, h - 8, 1, 5, true);
  }

  const int textX = x + (w - textW) / 2;
  const int textY = y + metrics.popupMarginY + metrics.popupTextBaselineOffsetY;
  renderer.drawText(fontId, textX, textY, clamped.c_str(), true, style);
  renderer.displayBuffer();
  return box;
}

void ShibaDeskTheme::fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int barHeight = std::max(6, metrics.popupProgressBarHeight);
  const int barWidth = std::max(0, layout.width - metrics.popupMarginX * 2);
  const int barX = layout.x + (layout.width - barWidth) / 2;
  const int barY = layout.y + layout.height - barHeight - 7;
  if (barWidth <= 4) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  shibadesk::drawSegmentedProgressBar(renderer, Rect{barX, barY, barWidth, barHeight}, progress);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void ShibaDeskTheme::drawOptionPopupBackdrop(const GfxRenderer& renderer, const Rect dialogRect) const {
  shibadesk::drawShibaPopupChrome(renderer, dialogRect, 4);
}

void ShibaDeskTheme::drawOptionPopupForeground(const GfxRenderer& renderer, const Rect dialogRect) const {
  if (dialogRect.width > 10 && dialogRect.height > 10) {
    renderer.drawRect(dialogRect.x + 4, dialogRect.y + 4, dialogRect.width - 8, dialogRect.height - 8, 1, true);
  }
}

bool ShibaDeskTheme::renderCustomOptionPopup(const GfxRenderer& renderer, const char* title, const char* headline,
                                             const char* message, const std::vector<std::string>& options,
                                             const int count, const int selectedIndex, Rect& outDialogRect,
                                             std::vector<Rect>& outOptionRects) const {
  if (count <= 0) return false;
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();

  const int modalW = std::clamp(screenW - 76, 220, 424);
  const int innerPadX = 16;
  const int contentW = modalW - innerPadX * 2;

  const bool hasTitle = title && title[0] != '\0';
  const bool hasHeadline = headline && headline[0] != '\0';
  const bool hasMessage = message && message[0] != '\0';

  const int headerStripH = (hasTitle && hasHeadline) ? 52 : (hasTitle || hasHeadline ? 34 : 0);
  std::vector<std::string> msgLines;
  if (hasMessage) {
    msgLines = shibadesk::wrapSmartLines(renderer, SMALL_FONT_ID, message, contentW - 8, 5);
  }
  const int msgBlockH = msgLines.empty() ? 0 : (static_cast<int>(msgLines.size()) * 22 + 10);

  constexpr int optH = 38;
  constexpr int optGap = 8;
  const int optsBlockH = count * optH + std::max(0, count - 1) * optGap;
  const int topPad = headerStripH > 0 ? (headerStripH + 16) : 16;
  const int modalH = std::min(screenH - 64, topPad + msgBlockH + optsBlockH + 16);

  const int mx = (screenW - modalW) / 2;
  const int my = std::max(24, (screenH - 38 - modalH) / 2);
  outDialogRect = Rect{mx, my, modalW, modalH};

  // Dense 45-degree hatched drop shadow + crisp white halo + rounded double-ink frame
  shibadesk::drawShibaPopupChrome(renderer, outDialogRect, 6);
  renderer.fillRoundedRect(mx, my, modalW, modalH, 10, Color::White);
  renderer.drawRoundedRect(mx, my, modalW, modalH, 3, 10, true);
  renderer.drawRoundedRect(mx + 4, my + 4, modalW - 8, modalH - 8, 1, 7, true);

  if (headerStripH > 0) {
    const int hx = mx + 5;
    const int hy = my + 5;
    const int hw = modalW - 10;
    renderer.fillRoundedRect(hx, hy, hw, headerStripH, 6, Color::Black);
    // Square off bottom corners of header strip + crisp divider
    renderer.fillRect(hx, hy + headerStripH - 6, hw, 6, true);

    if (hasTitle && hasHeadline) {
      const EpdFontFamily::Style tStyle = shibadesk::styleForText(title, true);
      const std::string cTitle = shibadesk::clampSingleLine(renderer, SMALL_FONT_ID, title, hw - 24, tStyle);
      const std::string cSub = shibadesk::clampSingleLine(renderer, UI_6_FONT_ID, headline, hw - 24);
      renderer.drawText(SMALL_FONT_ID, hx + 12, hy + 6, cTitle.c_str(), false, tStyle);
      renderer.drawText(UI_6_FONT_ID, hx + 12, hy + 30, cSub.c_str(), false);
    } else {
      const char* hdrText = hasTitle ? title : headline;
      const EpdFontFamily::Style tStyle = shibadesk::styleForText(hdrText, true);
      const std::string clamped = shibadesk::clampSingleLine(renderer, SMALL_FONT_ID, hdrText, hw - 24, tStyle);
      const int th = renderer.getTextHeight(SMALL_FONT_ID);
      renderer.drawText(SMALL_FONT_ID, hx + 12, hy + std::max(2, (headerStripH - th) / 2) - 1, clamped.c_str(), false,
                        tStyle);
    }
  }

  int curY = my + topPad;
  if (!msgLines.empty()) {
    for (const auto& ln : msgLines) {
      const int lw = renderer.getTextWidth(SMALL_FONT_ID, ln.c_str());
      renderer.drawText(SMALL_FONT_ID, mx + (modalW - lw) / 2, curY, ln.c_str(), true);
      curY += 22;
    }
    curY += 10;
  }

  outOptionRects.clear();
  outOptionRects.reserve(static_cast<size_t>(count));
  const bool centerSingle = (count == 1);

  for (int i = 0; i < count; ++i) {
    const int ox = mx + innerPadX;
    const int oy = curY + i * (optH + optGap);
    const Rect optRect{ox, oy, contentW, optH};
    outOptionRects.push_back(optRect);

    const bool sel = (i == selectedIndex);
    if (sel) {
      renderer.fillRoundedRect(ox, oy, contentW, optH, 7, Color::Black);
      renderer.drawRoundedRect(ox + 3, oy + 3, contentW - 6, optH - 6, 1, 5, false);
    } else {
      renderer.fillRoundedRect(ox, oy, contentW, optH, 7, Color::White);
      renderer.drawRoundedRect(ox, oy, contentW, optH, 1, 7, true);
    }

    const char* rawOpt = (static_cast<size_t>(i) < options.size()) ? options[static_cast<size_t>(i)].c_str() : "";
    const EpdFontFamily::Style oStyle = shibadesk::styleForText(rawOpt, sel);
    const std::string cOpt = shibadesk::clampSingleLine(renderer, SMALL_FONT_ID, rawOpt, contentW - 28, oStyle);
    const int ow = renderer.getTextWidth(SMALL_FONT_ID, cOpt.c_str(), oStyle);
    const int oh = renderer.getTextHeight(SMALL_FONT_ID);
    const int tx = centerSingle ? (ox + (contentW - ow) / 2) : (ox + 14);
    const int ty = oy + std::max(2, (optH - oh) / 2) - 1;
    renderer.drawText(SMALL_FONT_ID, tx, ty, cOpt.c_str(), !sel, oStyle);
  }

  return true;
}
