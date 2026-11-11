#include "LanguageSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <iterator>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

LanguageSelectActivity::LanguageSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("LanguageSelect", renderer, mappedInput) {}

void LanguageSelectActivity::onEnter() {
  {
    RenderLock lock(*this);
    sdFontSystem.ensureLoaded(renderer);
    cjkReady_ = sdFontSystem.hasCjkUiFallback(renderer);
  }
  UiListActivity::onEnter();

  // Open on the current language, which may sit past the first page; the first
  // screen build pulls the viewport to it (ListNav follow-on-build).
  const auto currentLang = static_cast<uint8_t>(I18N.getLanguage());
  const auto* begin = std::begin(SORTED_LANGUAGE_INDICES);
  const auto* end = std::end(SORTED_LANGUAGE_INDICES);
  const auto* it = std::find(begin, end, currentLang);
  nav.selected = (it != end) ? static_cast<int>(std::distance(begin, it)) : 0;

  // Built once here rather than every buildScreen() call: labels are static,
  // and the "Selected" marker can't go stale mid-visit since activateIndex()
  // finishes the activity immediately on selection.
  for (int i = 0; i < totalItems; ++i) {
    fui::ListItem item;
    const auto lang = static_cast<Language>(SORTED_LANGUAGE_INDICES[i]);
    if (lang == Language::ZH_CN && !cjkReady_) {
      item.label = tr(STR_LANG_CHINESE_SIMPLIFIED_FALLBACK);
    } else {
      item.label = I18N.getLanguageName(lang);
    }
    if (SORTED_LANGUAGE_INDICES[i] == currentLang) {
      item.value = tr(STR_SELECTED);
    }
    item.actionValue = static_cast<int16_t>(i);
    rowItems[i] = item;
  }
}

bool LanguageSelectActivity::handleCustomInput() {
  return cjkWarningPopup_.handleInput(mappedInput, [this] { requestUpdate(); });
}

void LanguageSelectActivity::render(RenderLock&& lock) {
  if (cjkWarningPopup_.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

const char* LanguageSelectActivity::headerTitle() const { return tr(STR_LANGUAGE); }

void LanguageSelectActivity::applyLanguageAndFinish(const uint8_t langIndex) {
  {
    RenderLock lock(*this);
    I18N.setLanguage(static_cast<Language>(langIndex));
  }

  SETTINGS.language = langIndex;
  SETTINGS.saveToFile();

  // Return to previous page
  finish();
}

void LanguageSelectActivity::activateIndex(const int index) {
  // The activated row leaves this screen; a lingering flash would gray an
  // unrelated element on the next render.
  app.clearTapFlash();
  nav.selected = index;
  const uint8_t langIndex = SORTED_LANGUAGE_INDICES[index];

  if (static_cast<Language>(langIndex) == Language::ZH_CN && !cjkReady_) {
    const char* options[2] = {tr(STR_CANCEL), tr(STR_CONFIRM)};
    cjkWarningPopup_.showMessage(tr(STR_CJK_FONT_WARNING_TITLE), tr(STR_CJK_FONT_WARNING_MSG), options, 2, 0,
                                 [this, langIndex](const int choice) {
                                   if (choice == 1) {
                                     applyLanguageAndFinish(langIndex);
                                   }
                                 });
    requestUpdate();
    return;
  }

  applyLanguageAndFinish(langIndex);
}

void LanguageSelectActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band GUI.drawHeader paints.
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // rowItems was built once in onEnter() and is reused here on every repaint.
  fui::ListProps props;
  props.items = rowItems;
  props.count = static_cast<uint16_t>(totalItems);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  // Label at the value's font size: both sides of the row read as one unit.
  // maxLines=2 also marks the style caller-owned (see textStyleUnset).
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
