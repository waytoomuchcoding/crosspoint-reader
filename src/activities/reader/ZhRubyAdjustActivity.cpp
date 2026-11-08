#include "ZhRubyAdjustActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <ZhDict.h>

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
  if (mappedInput.wasPressed(B::Left) && position > 0) {
    --position;
    requestUpdate();
  } else if (mappedInput.wasPressed(B::Right) && position < last) {
    ++position;
    requestUpdate();
  } else if (mappedInput.wasReleased(B::Confirm)) {
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
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_ZH_RUBY));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 4;
  const int contentWidth = pageWidth - 2 * metrics.contentSidePadding;
  for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, tr(STR_ZH_RUBY_HELP), contentWidth, 6)) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, line.c_str());
    y += renderer.getLineHeight(UI_10_FONT_ID);
  }
  y += metrics.verticalSpacing * 4;

  // Current value, with arrows showing which way more steps remain.
  const int last = static_cast<int>(std::size(ORDER)) - 1;
  char value[64];
  snprintf(value, sizeof(value), "%s  %s  %s", position > 0 ? "<" : " ", I18N.get(LABELS[ORDER[position]]),
           position < last ? ">" : " ");
  renderer.drawCenteredText(UI_12_FONT_ID, y, value, true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing * 2;

  // Position bar: one tick per step.
  const int barX = metrics.contentSidePadding;
  const int step = contentWidth / last;
  renderer.drawLine(barX, y + 6, barX + step * last, y + 6, 2, true);
  for (int k = 0; k <= last; ++k) {
    const int tx = barX + step * k;
    if (k == position) {
      renderer.fillRect(tx - 5, y, 11, 13, true);
    } else {
      renderer.fillRect(tx - 1, y + 2, 3, 9, true);
    }
  }
  y += 24;
  renderer.drawText(SMALL_FONT_ID, barX, y, tr(STR_ZH_RUBY_MORE));
  const char* less = tr(STR_ZH_RUBY_LESS);
  renderer.drawText(SMALL_FONT_ID, barX + step * last - renderer.getTextWidth(SMALL_FONT_ID, less), y, less);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_ZH_RUBY_MORE), tr(STR_ZH_RUBY_LESS));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
