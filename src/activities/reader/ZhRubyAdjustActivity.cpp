#include "ZhRubyAdjustActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <ZhDict.h>

#include <algorithm>
#include <iterator>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Display order runs from the most ruby to none: All, top 500 ... top 60,000, Off.
// Values are CrossPointSettings::zhRuby steps (zh::RUBY_STEPS indices).
constexpr uint8_t ORDER[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0};
constexpr StrId LABELS[] = {StrId::STR_ZH_RUBY_OFF,   StrId::STR_ZH_RUBY_ALL,   StrId::STR_ZH_RUBY_500,
                            StrId::STR_ZH_RUBY_1000,  StrId::STR_ZH_RUBY_2000,  StrId::STR_ZH_RUBY_3000,
                            StrId::STR_ZH_RUBY_5000,  StrId::STR_ZH_RUBY_8000,  StrId::STR_ZH_RUBY_12000,
                            StrId::STR_ZH_RUBY_20000, StrId::STR_ZH_RUBY_35000, StrId::STR_ZH_RUBY_60000};
static_assert(std::size(LABELS) == zh::RUBY_STEP_COUNT, "one label per ruby step");
static_assert(std::size(ORDER) == zh::RUBY_STEP_COUNT, "every ruby step in the display order");

}  // namespace

void ZhRubyAdjustActivity::onEnter() {
  Activity::onEnter();
  position = 0;
  for (size_t k = 0; k < std::size(ORDER); ++k) {
    if (ORDER[k] == SETTINGS.zhRuby) position = static_cast<int>(k);
  }
  requestUpdate();
}

void ZhRubyAdjustActivity::loop() {
  using B = MappedInputManager::Button;
  const int last = static_cast<int>(std::size(ORDER)) - 1;
  navigator_.onPrevious([this] {
    if (position > 0) {
      --position;
      requestUpdate();
    }
  });
  navigator_.onNext([this, last] {
    if (position < last) {
      ++position;
      requestUpdate();
    }
  });
  if (mappedInput.wasReleased(B::Confirm)) {
    ActivityResult result;
    result.isCancelled = SETTINGS.zhRuby == ORDER[position];
    if (!result.isCancelled) {
      SETTINGS.zhRuby = ORDER[position];
      SETTINGS.saveToFile();
    }
    setResult(std::move(result));
    finish();
  } else if (mappedInput.wasReleased(B::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  }
}

void ZhRubyAdjustActivity::render(RenderLock&&) {
  renderer.clearScreen();
  auto& theme = UITheme::getInstance();
  const auto& metrics = theme.getMetrics();
  const Rect safe = theme.getScreenSafeArea(renderer, /*hasFrontButtonHints=*/true, /*hasSideButtonHints=*/false);
  const Rect headerRect = theme.getHeaderRect(renderer, /*hasFrontButtonHints=*/true, /*hasSideButtonHints=*/false);
  const int contentX = safe.x + metrics.contentSidePadding;
  const int contentWidth = std::max(60, safe.width - 2 * metrics.contentSidePadding);
  const int last = static_cast<int>(std::size(ORDER)) - 1;

  char value[64];
  snprintf(value, sizeof(value), "%s  %s  %s", position > 0 ? "<" : " ", I18N.get(LABELS[ORDER[position]]),
           position < last ? ">" : " ");
  const auto helpLines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_ZH_RUBY_HELP), contentWidth, 6);

  auto drawBody = [&]() {
    GUI.drawHeader(renderer, headerRect, tr(STR_ZH_RUBY));

    int y = headerRect.y + headerRect.height + metrics.verticalSpacing * 4;
    for (const auto& line : helpLines) {
      renderer.drawText(UI_10_FONT_ID, contentX, y, line.c_str());
      y += renderer.getLineHeight(UI_10_FONT_ID);
    }
    y += metrics.verticalSpacing * 4;

    UITheme::drawCenteredText(renderer, safe, UI_12_FONT_ID, y, value, true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing * 2;

    // Position bar: one tick per step.
    const int step = contentWidth / std::max(1, last);
    renderer.drawLine(contentX, y + 6, contentX + step * last, y + 6, 2, true);
    for (int k = 0; k <= last; ++k) {
      const int tx = contentX + step * k;
      if (k == position) {
        renderer.fillRect(tx - 5, y, 11, 13, true);
      } else {
        renderer.fillRect(tx - 1, y + 2, 3, 9, true);
      }
    }
    y += 24;
    renderer.drawText(SMALL_FONT_ID, contentX, y, tr(STR_ZH_RUBY_MORE));
    const char* less = tr(STR_ZH_RUBY_LESS);
    renderer.drawText(SMALL_FONT_ID, contentX + step * last - renderer.getTextWidth(SMALL_FONT_ID, less), y, less);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_ZH_RUBY_MORE), tr(STR_ZH_RUBY_LESS));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  };

  if (auto* fcm = renderer.getFontCacheManager()) {
    auto scope = fcm->createPrewarmScope();
    drawBody();
    scope.endScanAndPrewarm();
    renderer.clearScreen();
    drawBody();
  } else {
    drawBody();
  }

  renderer.displayBuffer();
}
