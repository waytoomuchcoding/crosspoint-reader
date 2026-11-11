#include "ShibaStatusBar.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "ShibaArt.h"
#include "ShibaDeskChrome.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace shibadesk {
namespace {

bool isLiveWifiConnected() {
  const wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_MODE_NULL) return false;
  if ((mode & WIFI_MODE_STA) && WiFi.status() == WL_CONNECTED) return true;
  if ((mode & WIFI_MODE_AP) != 0) return true;
  return false;
}

}  // namespace

void drawShibaPixelBattery(const GfxRenderer& renderer, const int x, const int y, const int percent, const bool black) {
  const bool bg = !black;
  // 21x12 outer chamber with 2px crisp border + 2x6 right terminal nib = 23x12 total
  renderer.fillRect(x, y, 21, 12, bg);
  renderer.drawRect(x, y, 21, 12, 2, black);
  renderer.fillRect(x + 21, y + 3, 2, 6, black);

  const int clamped = std::clamp(percent, 0, 100);
  if (clamped <= 0) return;

  if (clamped <= 20) {
    // 1 bar on left + pixel '!' warning inside empty battery chamber
    renderer.fillRect(x + 3, y + 3, 3, 6, black);
    renderer.drawLine(x + 12, y + 3, x + 12, y + 6, black);
    renderer.drawPixel(x + 12, y + 8, black);
    return;
  }

  const int bars = std::clamp((clamped + 12) / 25, 1, 4);
  for (int b = 0; b < bars; ++b) {
    renderer.fillRect(x + 3 + b * 4, y + 3, 3, 6, black);
  }
}

void drawShibaStatusBar(const GfxRenderer& renderer, const char* pillLabel, const char* countSubtitle) {
  const Rect content = UITheme::getContentArea(renderer);
  const int screenW = renderer.getScreenWidth();
  const int leftX = content.x + 20;
  const int rightX = content.x + content.width - 20;
  const int topY = content.y;

  // Flush white band + 2px solid architectural rule + 1px dotted rule
  renderer.fillRect(0, topY, screenW, 38, false);
  renderer.fillRect(0, topY + 34, screenW, 2, true);
  for (int x = leftX; x <= rightX; x += 2) {
    renderer.drawPixel(x, topY + 37, true);
  }

  // Right telemetry cluster aligned strictly to `rightX`
  const int pct = static_cast<int>(powerManager.getBatteryPercentage());
  const bool showPct = SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_ALWAYS;

  const int battX = rightX - 23;
  drawShibaPixelBattery(renderer, battX, topY + 11, pct, true);

  int clusterLeft = battX;
  if (showPct) {
    char pctBuf[12];
    snprintf(pctBuf, sizeof(pctBuf), "%d%%", pct);
    const int pctW = renderer.getTextWidth(UI_6_FONT_ID, pctBuf);
    const int pctTextH = renderer.getTextHeight(UI_6_FONT_ID);
    const int pctX = battX - 6 - pctW;
    const int pctY = topY + 11 + std::max(0, (12 - pctTextH) / 2) - 1;
    renderer.drawText(UI_6_FONT_ID, pctX, pctY, pctBuf, true);
    clusterLeft = pctX;
  }

  const int divX = clusterLeft - 8;
  renderer.drawLine(divX, topY + 11, divX, topY + 22, true);

  const ShibaIcon wifiIcon = isLiveWifiConnected() ? ShibaIcon::WifiOn : ShibaIcon::WifiOff;
  const int wifiX = divX - 7 - getIconWidth(wifiIcon);
  drawIcon(renderer, wifiIcon, wifiX, topY + 11, true);

  // Left: solid black rounded pill [pillLabel] + count subtitle clamped cleanly before `wifiX - 8`
  if (pillLabel && *pillLabel != '\0') {
    const EpdFontFamily::Style pillStyle = styleForText(pillLabel, true);
    renderer.prewarmFallbackText(SMALL_FONT_ID, pillLabel, pillStyle);
    const int labelW = renderer.getTextWidth(SMALL_FONT_ID, pillLabel, pillStyle);
    const int labelH = renderer.getTextHeight(SMALL_FONT_ID);
    const int pillW = labelW + 22;
    const int pillH = 22;
    const int pillY = topY + 6;
    renderer.fillRoundedRect(leftX, pillY, pillW, pillH, 5, Color::Black);
    renderer.drawText(SMALL_FONT_ID, leftX + (pillW - labelW) / 2, pillY + std::max(1, (pillH - labelH) / 2) - 1,
                      pillLabel, false, pillStyle);

    if (countSubtitle && *countSubtitle != '\0') {
      const int subX = leftX + pillW + 10;
      const int maxSubW = wifiX - 10 - subX;
      if (maxSubW > 20) {
        const std::string clampedSub = clampSingleLine(renderer, SMALL_FONT_ID, countSubtitle, maxSubW);
        const int subH = renderer.getTextHeight(SMALL_FONT_ID);
        renderer.drawText(SMALL_FONT_ID, subX, pillY + std::max(1, (pillH - subH) / 2) - 1, clampedSub.c_str(), true);
      }
    }
  }
}

void drawShibaHomeDock(const GfxRenderer& renderer, const int focusedDockIdx, const char* const labels[4],
                       Rect outSlotRects[4]) {
  const Rect content = UITheme::getContentArea(renderer);
  const int leftX = content.x + 20;
  const int rightX = content.x + content.width - 20;
  const int totalW = rightX - leftX;
  constexpr int minGap = 10;
  const int btnW = (totalW - minGap * 3) / 4;
  const int btnH = 58;
  const int hintReserve = gpio.hasTouch() ? 14 : 62;
  const int btnY = content.y + content.height - hintReserve - btnH;

  static constexpr ShibaIcon DOCK_ICONS[4] = {
      ShibaIcon::DockLibrary,
      ShibaIcon::DockFiles,
      ShibaIcon::DockSync,
      ShibaIcon::DockSettings,
  };

  for (int i = 0; i < 4; ++i) {
    // Distribute any 1-3px integer remainder evenly so button 0 starts at `leftX`
    // and button 3 ends flush at `rightX`, matching Hero & Shelf outer rails.
    const int bx = leftX + (i * (totalW - btnW)) / 3;
    const Rect slotRect{bx, btnY, btnW, btnH};
    if (outSlotRects) {
      outSlotRects[i] = slotRect;
    }

    const bool focused = (i == focusedDockIdx);
    // Crisp white halo + subtle +1px right / +2px bottom tactile bevel shadow
    renderer.fillRoundedRect(bx - 2, btnY - 2, btnW + 5, btnH + 5, 11, Color::White);
    renderer.fillRoundedRect(bx + 1, btnY + 2, btnW + 1, btnH, 10, Color::Black);

    if (focused) {
      renderer.fillRoundedRect(bx, btnY, btnW, btnH, 10, Color::Black);
      renderer.drawRoundedRect(bx + 3, btnY + 3, btnW - 6, btnH - 6, 1, 7, false);
    } else {
      renderer.fillRoundedRect(bx, btnY, btnW, btnH, 10, Color::White);
      renderer.drawRoundedRect(bx, btnY, btnW, btnH, 2, 10, true);
    }

    const char* rawLabel = (labels && labels[i]) ? labels[i] : "";
    const int iconW = getIconWidth(DOCK_ICONS[i]);
    const int iconH = getIconHeight(DOCK_ICONS[i]);
    const int iconGap = 6;
    const int maxTextW = std::max(20, btnW - iconW - iconGap - 8);
    // Uniform SMALL_FONT_ID (16px CJK / 12px Latin REGULAR) across all 4 dock buttons
    const std::string text = clampSingleLine(renderer, SMALL_FONT_ID, rawLabel, maxTextW, EpdFontFamily::REGULAR);
    const int textW = renderer.getTextWidth(SMALL_FONT_ID, text.c_str(), EpdFontFamily::REGULAR);
    const int textH = renderer.getTextHeight(SMALL_FONT_ID);

    const int groupW = iconW + iconGap + textW;
    const int startX = bx + std::max(4, (btnW - groupW) / 2);
    const int iconY = btnY + (btnH - iconH) / 2;
    const int textY = btnY + std::max(2, (btnH - textH) / 2) - 1;

    drawIcon(renderer, DOCK_ICONS[i], startX, iconY, !focused);
    renderer.drawText(SMALL_FONT_ID, startX + iconW + iconGap, textY, text.c_str(), !focused, EpdFontFamily::REGULAR);
  }
}

void drawShibaPaginationBar(const GfxRenderer& renderer, const int currentPage, const int totalPages,
                            const int startItem, const int endItem, const int totalItems) {
  const Rect content = UITheme::getContentArea(renderer);
  const int leftX = content.x + 20;
  const int rightX = content.x + content.width - 20;
  const int barW = rightX - leftX;
  constexpr int barH = 26;
  const int hintReserve = gpio.hasTouch() ? 8 : 44;
  const int barY = content.y + content.height - hintReserve - barH;

  renderer.fillRect(leftX - 1, barY - 1, barW + 2, barH + 2, false);
  renderer.drawRect(leftX, barY, barW, barH, 2, true);

  char buf[96];
  if (totalItems > 0) {
    std::snprintf(buf, sizeof(buf), tr(STR_DESK_PAGINATION), std::max(1, currentPage), std::max(1, totalPages),
                  std::max(1, startItem), std::max(startItem, endItem), totalItems);
  } else {
    std::snprintf(buf, sizeof(buf), tr(STR_DESK_PAGINATION), 1, 1, 0, 0, 0);
  }

  const std::string clamped = clampSingleLine(renderer, UI_6_FONT_ID, buf, barW - 56);
  const int textW = renderer.getTextWidth(UI_6_FONT_ID, clamped.c_str());
  const int textH = renderer.getTextHeight(UI_6_FONT_ID);
  const int textY = barY + std::max(1, (barH - textH) / 2) - 1;

  if (currentPage > 1) {
    renderer.drawText(UI_6_FONT_ID, leftX + 10, textY, "\xC2\xAB", true);  // «
  }
  renderer.drawText(UI_6_FONT_ID, leftX + (barW - textW) / 2, textY, clamped.c_str(), true);
  if (currentPage < totalPages) {
    const int rw = renderer.getTextWidth(UI_6_FONT_ID, "\xC2\xBB");
    renderer.drawText(UI_6_FONT_ID, rightX - 10 - rw, textY, "\xC2\xBB", true);  // »
  }
}

void drawShibaHardwareKeyHints(const GfxRenderer& renderer, const MappedInputManager& mappedInput,
                               const char* backLabel, const char* confirmLabel, const char* prevLabel,
                               const char* nextLabel) {
  if (gpio.hasTouch()) return;
  const auto mapped = mappedInput.mapLabels(backLabel ? backLabel : "", confirmLabel ? confirmLabel : "",
                                            prevLabel ? prevLabel : "", nextLabel ? nextLabel : "");
  drawShibaMappedKeyHints(const_cast<GfxRenderer&>(renderer), mapped.btn1, mapped.btn2, mapped.btn3, mapped.btn4);
}

void drawShibaMappedKeyHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                             const char* btn4) {
  if (gpio.hasTouch()) return;

  const GfxRenderer::Orientation origOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  constexpr int stripH = 38;
  const int topY = screenH - stripH;
  const bool grayscale = renderer.getRenderMode() != GfxRenderer::BW && !renderer.grayPlanesAreAbsolute();

  renderer.fillRect(0, topY, screenW, stripH, grayscale);
  if (!grayscale) {
    renderer.fillRect(0, topY, screenW, 2, true);
    const int slotW = screenW / 4;
    const char* const slots[4] = {btn1, btn2, btn3, btn4};
    for (int i = 0; i < 4; ++i) {
      const int sx = i * slotW;
      if (i > 0) {
        renderer.drawLine(sx, topY + 5, sx, screenH - 4, true);
      }
      if (slots[i] && slots[i][0] != '\0') {
        const std::string text = clampSingleLine(renderer, UI_6_FONT_ID, slots[i], slotW - 10);
        const int tw = renderer.getTextWidth(UI_6_FONT_ID, text.c_str());
        const int th = renderer.getTextHeight(UI_6_FONT_ID);
        renderer.drawText(UI_6_FONT_ID, sx + (slotW - tw) / 2, topY + 2 + std::max(1, (stripH - 2 - th) / 2) - 1,
                          text.c_str(), true);
      }
    }
  }

  renderer.setOrientation(origOrientation);
}

}  // namespace shibadesk
