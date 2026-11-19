#include "TextSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderFontSizes.h"
#include "ReaderLineSpacing.h"
#include "SdCardFontSystem.h"
#include "TextSettingsPreview.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
// Tab labels for English | Chinese | Layout | Style.
constexpr StrId TAB_NAME_IDS[] = {StrId::STR_ENGLISH, StrId::STR_CHINESE, StrId::STR_LAYOUT, StrId::STR_STYLE};

constexpr StrId LAYOUT_ROW_NAME_IDS[] = {
    StrId::STR_LINE_SPACING,          StrId::STR_WORD_SPACING, StrId::STR_CHARACTER_SPACING, StrId::STR_EXTRA_SPACING,
    StrId::STR_PARAGRAPH_INDENTATION, StrId::STR_ALIGNMENT,    StrId::STR_SCREEN_MARGIN};
constexpr StrId STYLE_ROW_NAME_IDS[] = {StrId::STR_FOCUS_READING, StrId::STR_HYPHENATION_NO_PREVIEW,
                                        StrId::STR_EMBEDDED_STYLE_NO_PREVIEW, StrId::STR_TEXT_AA_NO_PREVIEW};

int findCurrentFontIndex(const SdCardFontRegistry* registry, const char* sdFontFamilyName, uint8_t fontFamily) {
  if (sdFontFamilyName[0] != '\0' && registry) {
    const auto& families = registry->getFamilies();
    const auto family = std::find_if(families.begin(), families.end(), [sdFontFamilyName](const auto& candidate) {
      return candidate.name == sdFontFamilyName;
    });
    if (family != families.end()) {
      return CrossPointSettings::BUILTIN_FONT_COUNT + static_cast<int>(family - families.begin());
    }
  }

  return fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? fontFamily : 0;
}

constexpr StrId WORD_SPACING_IDS[] = {StrId::STR_SPACING_50_PERCENT,  StrId::STR_SPACING_75_PERCENT,
                                      StrId::STR_SPACING_100_PERCENT, StrId::STR_SPACING_125_PERCENT,
                                      StrId::STR_SPACING_150_PERCENT, StrId::STR_SPACING_175_PERCENT,
                                      StrId::STR_SPACING_200_PERCENT};
constexpr StrId CHARACTER_SPACING_IDS[] = {StrId::STR_SPACING_MINUS_2, StrId::STR_SPACING_MINUS_1,
                                           StrId::STR_SPACING_ZERO, StrId::STR_SPACING_PLUS_1,
                                           StrId::STR_SPACING_PLUS_2};
constexpr StrId ALIGNMENT_IDS[] = {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                                   StrId::STR_BOOK_S_STYLE};
constexpr int MARGIN_MIN = CrossPointSettings::SCREEN_MARGIN_MIN;
constexpr int MARGIN_MAX = CrossPointSettings::SCREEN_MARGIN_MAX;
constexpr int MARGIN_STEP = CrossPointSettings::SCREEN_MARGIN_STEP;
constexpr int WORD_SPACING_MIN = CrossPointSettings::WORD_SPACING_MIN;
constexpr int WORD_SPACING_MAX = CrossPointSettings::WORD_SPACING_MAX;
constexpr int WORD_SPACING_STEP = CrossPointSettings::WORD_SPACING_STEP;
static_assert(std::size(WORD_SPACING_IDS) == (WORD_SPACING_MAX - WORD_SPACING_MIN) / WORD_SPACING_STEP + 1);
}  // namespace

TextSettingsActivity::TextSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const SdCardFontRegistry* registry, Tab initialTab)
    : UiTabListActivity("TextSettings", renderer, mappedInput), registry_(registry), tab_(initialTab) {}

const char* TextSettingsActivity::tabLabel(const int index) const { return I18N.get(TAB_NAME_IDS[index]); }

void TextSettingsActivity::onEnter() {
  UiTabListActivity::onEnter();

  metrics_ = UITheme::getInstance().getMetrics();
  const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int safeBottom = safeArea.y + safeArea.height;
  afterHeader = safeArea.y + metrics_.topPadding + metrics_.headerHeight + metrics_.verticalSpacing;
  const int usableHeight = safeBottom - afterHeader - metrics_.verticalSpacing;
  previewHeight = usableHeight * metrics_.previewHeightPercent / 100;

  fonts_.clear();
  zhFonts_.clear();
  fonts_.push_back({I18N.get(StrId::STR_NOTO_SANS), true, static_cast<uint8_t>(CrossPointSettings::NOTOSANS)});
  zhFonts_.push_back({I18N.get(StrId::STR_CLOCK_DST_AUTO), true, 0});
  if (registry_) {
    const auto& families = registry_->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      const uint8_t idx = static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i);
      if (sdFontSystem.familyHasHan(families[i])) {
        zhFonts_.push_back({families[i].name, false, idx});
      } else {
        fonts_.push_back({families[i].name, false, idx});
      }
    }
  }

  currentFamilyIndex_ = 0;
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    for (int i = 1; i < static_cast<int>(fonts_.size()); ++i) {
      if (fonts_[i].name == SETTINGS.sdFontFamilyName) {
        currentFamilyIndex_ = i;
        break;
      }
    }
  }
  currentZhFamilyIndex_ = 0;
  if (SETTINGS.zhFontFamilyName[0] != '\0') {
    for (int i = 1; i < static_cast<int>(zhFonts_.size()); ++i) {
      if (zhFonts_[i].name == SETTINGS.zhFontFamilyName) {
        currentZhFamilyIndex_ = i;
        break;
      }
    }
  }

  rebuildSizeList();
  rebuildZhSizeList();

  previewChinese_ = (tab_ == Tab::Chinese);
  // Per-tab ring positions (0 = tab bar, 1..N = row). Row 0 in English/Chinese
  // is the Size picker, followed by the font families.
  for (auto& n : tabNavs) n.selected = 1;
  tabNavs[static_cast<int>(Tab::Family)].selected = currentFamilyIndex_ + 2;
  tabNavs[static_cast<int>(Tab::Chinese)].selected = currentZhFamilyIndex_ + 2;
  tabNavs[static_cast<int>(tab_)].selected = 0;  // screen opens with the tab bar focused

  rebuildRowItems();
}

// Rebuilds rowItems_ (label + actionValue) for the active tab. Structural —
// call only when tab_ or its backing data (fonts_/zhFonts_) changes, never from
// buildScreen(), which just refreshes rowValues_/rowItems_[].value in place.
void TextSettingsActivity::rebuildRowItems() {
  const int count = listCount();
  rowValues_.assign(count, std::string());
  rowItems_.clear();
  rowItems_.reserve(count);
  for (int i = 0; i < count; i++) {
    fui::ListItem item;
    switch (tab_) {
      case Tab::Family:
        item.label = (i == 0) ? I18N.get(StrId::STR_SIZE) : fonts_[i - 1].name.c_str();
        break;
      case Tab::Chinese:
        item.label = (i == 0) ? I18N.get(StrId::STR_SIZE) : zhFonts_[i - 1].name.c_str();
        break;
      case Tab::Layout:
        item.label = I18N.get(LAYOUT_ROW_NAME_IDS[i]);
        break;
      case Tab::Style:
        item.label = I18N.get(STYLE_ROW_NAME_IDS[i]);
        break;
      default:
        break;
    }
    item.actionValue = static_cast<int16_t>(i);
    rowItems_.push_back(item);
  }
}

void TextSettingsActivity::rebuildSizeList() {
  const char* famName = (currentFamilyIndex_ > 0 && currentFamilyIndex_ < static_cast<int>(fonts_.size()))
                            ? fonts_[currentFamilyIndex_].name.c_str()
                            : "";
  const std::vector<uint8_t> points = readerFontPointSizes(registry_, famName);
  const uint8_t selectedPt = snapToNearestPointSize(points, SETTINGS.fontPointSize);

  sizes_.clear();
  sizes_.reserve(points.size());
  currentSizeIndex_ = 0;
  for (const uint8_t pt : points) {
    if (pt == selectedPt) currentSizeIndex_ = static_cast<int>(sizes_.size());
    sizes_.push_back({formatReaderFontSizeLabel(registry_, famName, pt), pt});
  }
}

void TextSettingsActivity::rebuildZhSizeList() {
  const std::string& resolvedName = SETTINGS.zhFontFamilyName[0] != '\0' ? std::string(SETTINGS.zhFontFamilyName)
                                                                         : sdFontSystem.resolvedZhFamilyName();
  const char* famName = resolvedName.c_str();
  zhSizes_.clear();
  zhSizes_.push_back({I18N.get(StrId::STR_CLOCK_DST_AUTO), 0});
  currentZhSizeIndex_ = 0;
  if (famName[0] == '\0' || !registry_) return;
  const std::vector<uint8_t> points = readerFontPointSizes(registry_, famName);
  const uint8_t snappedPt =
      SETTINGS.zhFontPointSize != 0 ? snapToNearestPointSize(points, SETTINGS.zhFontPointSize) : 0;
  for (const uint8_t pt : points) {
    if (snappedPt != 0 && pt == snappedPt) {
      currentZhSizeIndex_ = static_cast<int>(zhSizes_.size());
    }
    zhSizes_.push_back({formatReaderFontSizeLabel(registry_, famName, pt), pt});
  }
}

void TextSettingsActivity::onTabAction(const int index) {
  if (optionPopup_.isActive()) return;
  if (tab_ != static_cast<Tab>(index)) {
    tab_ = static_cast<Tab>(index);
    if (tab_ == Tab::Family) previewChinese_ = false;
    if (tab_ == Tab::Chinese) previewChinese_ = true;
    rebuildRowItems();
    auto& n = activeNav();
    n.selected = 0;          // tab taps land with the tab bar focused (legacy tap behavior)
    n.followOnBuild = true;  // pull the new tab's viewport to its remembered selection
    requestUpdate();
  }
  // The switched-to tab repaints as the selected pill; a flash overlay on top
  // of it just repaints the pill in the focused style.
  app.clearTapFlash();
}

void TextSettingsActivity::activateIndex(const int index) {
  if (optionPopup_.isActive()) return;
  // Most rows repaint a different surface (popup, preview, new value);
  // a lingering tap flash would gray an unrelated element.
  app.clearTapFlash();
  activateRow(index);
}

bool TextSettingsActivity::handleCustomInput() {
  return optionPopup_.handleInput(mappedInput, [this] { requestUpdate(); });
}

bool TextSettingsActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ringPos() == 0) {
      switchTab();
    } else {
      activateRow(ringPos() - 1);
    }
    return true;
  }

  return false;
}

void TextSettingsActivity::buildScreen(UiScreen& screen) {
  const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int tabTop = afterHeader + previewHeight;
  const int rightInset = renderer.getScreenWidth() - (safeArea.x + safeArea.width);
  const int bottomInset = renderer.getScreenHeight() - (safeArea.y + safeArea.height);
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(tabTop), static_cast<int16_t>(rightInset),
                                                static_cast<int16_t>(bottomInset), static_cast<int16_t>(safeArea.x)});

  buildTabBar(screen);

  // rowItems_ (label/actionValue) was built by rebuildRowItems() when the tab
  // was last switched; only the live value text needs refreshing here, by
  // assigning into the existing rowValues_ strings (no vector growth) rather
  // than building a new items/values vector on every render.
  const int count = listCount();
  for (int i = 0; i < count; i++) {
    switch (tab_) {
      case Tab::Family:
        if (i == 0) {
          rowValues_[i] = (currentSizeIndex_ >= 0 && currentSizeIndex_ < static_cast<int>(sizes_.size()))
                              ? sizes_[currentSizeIndex_].name
                              : "";
        } else {
          rowValues_[i] = (i - 1 == currentFamilyIndex_) ? tr(STR_SELECTED) : "";
        }
        break;
      case Tab::Chinese:
        if (i == 0) {
          if (currentZhSizeIndex_ > 0 && currentZhSizeIndex_ < static_cast<int>(zhSizes_.size())) {
            rowValues_[i] = zhSizes_[currentZhSizeIndex_].name;
          } else {
            const uint8_t resolvedPt = sdFontSystem.resolvedZhPointSize();
            if (resolvedPt != 0) {
              char buf[32];
              snprintf(buf, sizeof(buf), "%s (%u pt)", I18N.get(StrId::STR_CLOCK_DST_AUTO), resolvedPt);
              rowValues_[i] = buf;
            } else {
              rowValues_[i] = I18N.get(StrId::STR_CLOCK_DST_AUTO);
            }
          }
        } else {
          rowValues_[i] = (i - 1 == currentZhFamilyIndex_) ? tr(STR_SELECTED) : "";
        }
        break;
      case Tab::Layout:
        rowValues_[i] = layoutValueText(i);
        break;
      default:
        break;
    }
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
    if (tab_ == Tab::Style) {
      GUI.setCheckboxRow(rowItems_[i], styleRowChecked(i));
    } else if (tab_ == Tab::Layout && i == static_cast<int>(LayoutRow::ParaSpacing)) {
      GUI.setCheckboxRow(rowItems_[i], SETTINGS.extraParagraphSpacing);
    }
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  // Titles match the value's font size (smallText) so both sides of a row
  // read as one unit; labels that still don't fit wrap onto a second line.
  // maxLines=2 also marks the style explicitly set (see SettingsActivity).
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncTabListViewport(screen, props);
  screen.list(props);
}

const char* TextSettingsActivity::confirmLabelText() const {
  if (ringPos() == 0) {
    // Confirm on the tab bar advances to the next tab.
    return I18N.get(TAB_NAME_IDS[(static_cast<int>(tab_) + 1) % static_cast<int>(Tab::Count)]);
  }
  switch (tab_) {
    case Tab::Layout:
      return ringPos() - 1 == static_cast<int>(LayoutRow::ParaSpacing) ? tr(STR_TOGGLE) : tr(STR_SELECT);
    case Tab::Style:
      return tr(STR_TOGGLE);
    default:
      return tr(STR_SELECT);
  }
}

void TextSettingsActivity::drawChrome() {
  GUI.drawHeader(renderer, UITheme::getInstance().getHeaderRect(renderer, true, false), tr(STR_TEXT_SETTINGS));

  std::string familyName;
  std::string sizeName;
  if (previewChinese_) {
    const std::string& resolvedFam = sdFontSystem.resolvedZhFamilyName();
    familyName = !resolvedFam.empty()
                     ? resolvedFam
                     : ((currentZhFamilyIndex_ >= 0 && currentZhFamilyIndex_ < static_cast<int>(zhFonts_.size()))
                            ? zhFonts_[currentZhFamilyIndex_].name
                            : "");
    const uint8_t resolvedPt =
        SETTINGS.zhFontPointSize != 0 ? SETTINGS.zhFontPointSize : sdFontSystem.resolvedZhPointSize();
    if (resolvedPt != 0) {
      sizeName = formatReaderFontSizeLabel(registry_, familyName.c_str(), resolvedPt);
    } else if (currentZhSizeIndex_ >= 0 && currentZhSizeIndex_ < static_cast<int>(zhSizes_.size())) {
      sizeName = zhSizes_[currentZhSizeIndex_].name;
    }
  } else {
    familyName = (currentFamilyIndex_ >= 0 && currentFamilyIndex_ < static_cast<int>(fonts_.size()))
                     ? fonts_[currentFamilyIndex_].name
                     : "";
    sizeName = (currentSizeIndex_ >= 0 && currentSizeIndex_ < static_cast<int>(sizes_.size()))
                   ? sizes_[currentSizeIndex_].name
                   : "";
  }
  char labelBuf[128];
  snprintf(labelBuf, sizeof(labelBuf), "%s \"%s, %s\"", tr(STR_PREVIEW), familyName.c_str(), sizeName.c_str());
  const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect previewBounds{safeArea.x, afterHeader, safeArea.width, previewHeight};
  const auto [savedX, savedY, savedWidth, savedHeight] = renderer.getClipRect();
  const int clipLeft = std::max(previewBounds.x, savedX);
  const int clipTop = std::max(previewBounds.y, savedY);
  const int clipRight = std::min(previewBounds.x + previewBounds.width, savedX + savedWidth);
  const int clipBottom = std::min(previewBounds.y + previewBounds.height, savedY + savedHeight);
  renderer.setClipRect(clipLeft, clipTop, std::max(0, clipRight - clipLeft), std::max(0, clipBottom - clipTop));
  textsettings::renderPreview(renderer, previewLayout_, metrics_.previewPadding, metrics_.verticalSpacing,
                              previewBounds, labelBuf, previewChinese_);
  renderer.setClipRect(savedX, savedY, savedWidth, savedHeight);
}

void TextSettingsActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabelText(), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TextSettingsActivity::render(RenderLock&& lock) {
  if (optionPopup_.processRender(renderer, mappedInput)) return;  // picker draws over everything
  UiListActivity::render(std::move(lock));
}

// Font switching runs on the main task from loop(), which deliberately holds no
// RenderLock. ensureLoaded() deletes the resident SdCardFont before loading the
// next one, and the render task walks that same object inside the preview's
// prewarmCache() — so without this lock a font switch can free the mini glyph
// arrays out from under prewarmStyle() (crash: null s.miniGlyphs mid-read/sort).
void TextSettingsActivity::applyFamily(int listIndex) {
  RenderLock lock;
  const auto& font = fonts_[listIndex];
  if (font.isBuiltin) {
    SETTINGS.fontFamily = font.settingIndex;
    SETTINGS.sdFontFamilyName[0] = '\0';
    sdFontSystem.ensureLoaded(renderer);  // unloads the previously resident SD font
    currentFamilyIndex_ = listIndex;
  } else if (registry_) {
    const int sdIdx = font.settingIndex - CrossPointSettings::BUILTIN_FONT_COUNT;
    const auto& families = registry_->getFamilies();
    if (sdIdx < static_cast<int>(families.size())) {
      strncpy(SETTINGS.sdFontFamilyName, families[sdIdx].name.c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
      SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
      sdFontSystem.ensureLoaded(renderer);
      currentFamilyIndex_ = listIndex;
    }
  }

  if (currentFamilyIndex_ != listIndex) return;  // switch failed — keep the old size list

  rebuildSizeList();
}

void TextSettingsActivity::applyZhFamily(int listIndex) {
  RenderLock lock;
  const auto& font = zhFonts_[listIndex];
  if (font.isBuiltin) {
    SETTINGS.zhFontFamilyName[0] = '\0';
  } else {
    strncpy(SETTINGS.zhFontFamilyName, font.name.c_str(), sizeof(SETTINGS.zhFontFamilyName) - 1);
    SETTINGS.zhFontFamilyName[sizeof(SETTINGS.zhFontFamilyName) - 1] = '\0';
  }
  sdFontSystem.ensureLoaded(renderer);
  currentZhFamilyIndex_ = listIndex;
  rebuildZhSizeList();
}

void TextSettingsActivity::applyZhSize(int listIndex) {
  RenderLock lock;
  currentZhSizeIndex_ = listIndex;
  SETTINGS.zhFontPointSize = zhSizes_[listIndex].pointSize;
  sdFontSystem.ensureLoaded(renderer);
}

void TextSettingsActivity::openEnglishSizePopup() {
  std::vector<const char*> labels;
  labels.reserve(sizes_.size());
  for (const auto& s : sizes_) labels.push_back(s.name.c_str());
  optionPopup_.show(I18N.get(StrId::STR_SIZE), labels.data(), static_cast<int>(labels.size()), currentSizeIndex_,
                    [this](int idx) {
                      if (idx >= 0 && idx < static_cast<int>(sizes_.size()) && idx != currentSizeIndex_) {
                        applySize(idx);
                        SETTINGS.saveToFile();
                        requestUpdate();
                      }
                    });
  requestUpdate();
}

void TextSettingsActivity::openChineseSizePopup() {
  std::vector<const char*> labels;
  labels.reserve(zhSizes_.size());
  for (const auto& s : zhSizes_) labels.push_back(s.name.c_str());
  optionPopup_.show(I18N.get(StrId::STR_SIZE), labels.data(), static_cast<int>(labels.size()), currentZhSizeIndex_,
                    [this](int idx) {
                      if (idx >= 0 && idx < static_cast<int>(zhSizes_.size()) && idx != currentZhSizeIndex_) {
                        applyZhSize(idx);
                        SETTINGS.saveToFile();
                        requestUpdate();
                      }
                    });
  requestUpdate();
}

void TextSettingsActivity::activateRow(int row) {
  switch (tab_) {
    case Tab::Family:
      if (row == 0) {
        openEnglishSizePopup();
      } else if (row - 1 != currentFamilyIndex_) {
        applyFamily(row - 1);
        if (currentFamilyIndex_ == row - 1) {
          SETTINGS.saveToFile();
        }
        requestUpdate();
      }
      break;
    case Tab::Chinese:
      if (row == 0) {
        openChineseSizePopup();
      } else if (row - 1 != currentZhFamilyIndex_) {
        applyZhFamily(row - 1);
        SETTINGS.saveToFile();
        requestUpdate();
      }
      break;
    case Tab::Layout:
      confirmLayoutRow(row);
      break;
    case Tab::Style:
      confirmStyleRow(row);
      break;
    default:
      break;
  }
}

// Same RenderLock rationale as applyFamily(): a size change reloads the SD font
// file, which frees and replaces the SdCardFont the render task may be reading.
void TextSettingsActivity::applySize(int listIndex) {
  RenderLock lock;

  currentSizeIndex_ = listIndex;
  SETTINGS.fontPointSize = sizes_[listIndex].pointSize;
  sdFontSystem.ensureLoaded(renderer);
}

void TextSettingsActivity::confirmLayoutRow(int row) {
  switch (static_cast<LayoutRow>(row)) {
    case LayoutRow::ParaSpacing:
      SETTINGS.extraParagraphSpacing = !SETTINGS.extraParagraphSpacing;
      SETTINGS.saveToFile();
      requestUpdate();
      break;
    case LayoutRow::ParaIndentation: {
      std::vector<std::string> options;
      options.reserve(6);
      options.push_back(tr(STR_STATE_OFF));
      for (int spaces = 1; spaces <= 5; ++spaces) options.push_back(std::to_string(spaces));
      optionPopup_.show(StrId::STR_PARAGRAPH_INDENTATION, options, SETTINGS.paragraphIndentSpaces, [](int idx) {
        SETTINGS.paragraphIndentSpaces = static_cast<uint8_t>(idx);
        SETTINGS.saveToFile();
      });
      requestUpdate();
      break;
    }
    case LayoutRow::LineSpacing:
      optionPopup_.show(StrId::STR_LINE_SPACING, LINE_SPACING_IDS, static_cast<int>(std::size(LINE_SPACING_IDS)),
                        SETTINGS.lineSpacing, [](int idx) {
                          SETTINGS.lineSpacing = static_cast<uint8_t>(idx);
                          SETTINGS.saveToFile();
                        });
      requestUpdate();
      break;
    case LayoutRow::Alignment:
      optionPopup_.show(StrId::STR_ALIGNMENT, ALIGNMENT_IDS, static_cast<int>(std::size(ALIGNMENT_IDS)),
                        SETTINGS.paragraphAlignment, [](int idx) {
                          SETTINGS.paragraphAlignment = static_cast<uint8_t>(idx);
                          SETTINGS.saveToFile();
                        });
      requestUpdate();
      break;
    case LayoutRow::WordSpacing: {
      const int cur = (std::clamp<int>(SETTINGS.wordSpacing, WORD_SPACING_MIN, WORD_SPACING_MAX) - WORD_SPACING_MIN) /
                      WORD_SPACING_STEP;
      optionPopup_.show(StrId::STR_WORD_SPACING, WORD_SPACING_IDS, static_cast<int>(std::size(WORD_SPACING_IDS)), cur,
                        [](int idx) {
                          SETTINGS.wordSpacing = static_cast<uint8_t>(WORD_SPACING_MIN + idx * WORD_SPACING_STEP);
                          SETTINGS.saveToFile();
                        });
      requestUpdate();
      break;
    }
    case LayoutRow::CharacterSpacing:
      optionPopup_.show(StrId::STR_CHARACTER_SPACING, CHARACTER_SPACING_IDS,
                        static_cast<int>(std::size(CHARACTER_SPACING_IDS)), SETTINGS.characterSpacing, [](int idx) {
                          SETTINGS.characterSpacing = static_cast<uint8_t>(idx);
                          SETTINGS.saveToFile();
                        });
      requestUpdate();
      break;
    case LayoutRow::ScreenMargin: {
      std::vector<std::string> options;
      options.reserve((MARGIN_MAX - MARGIN_MIN) / MARGIN_STEP + 1);
      for (int m = MARGIN_MIN; m <= MARGIN_MAX; m += MARGIN_STEP) options.push_back(std::to_string(m));
      const int cur = (std::clamp<int>(SETTINGS.screenMargin, MARGIN_MIN, MARGIN_MAX) - MARGIN_MIN) / MARGIN_STEP;
      optionPopup_.show(StrId::STR_SCREEN_MARGIN, options, cur, [](int idx) {
        SETTINGS.screenMargin = static_cast<uint8_t>(MARGIN_MIN + idx * MARGIN_STEP);
        SETTINGS.saveToFile();
      });
      requestUpdate();
      break;
    }

    default:
      break;
  }
}

std::string TextSettingsActivity::layoutValueText(int row) const {
  switch (static_cast<LayoutRow>(row)) {
    case LayoutRow::LineSpacing: {
      const uint8_t v = SETTINGS.lineSpacing;
      return I18N.get(LINE_SPACING_IDS[v < std::size(LINE_SPACING_IDS) ? v : CrossPointSettings::LINE_SPACING_120]);
    }
    case LayoutRow::ParaIndentation: {
      const int spaces = std::clamp<int>(SETTINGS.paragraphIndentSpaces, 0, 5);
      return spaces == 0 ? tr(STR_STATE_OFF) : std::to_string(spaces);
    }
    case LayoutRow::Alignment: {
      const uint8_t v = SETTINGS.paragraphAlignment;
      return v < std::size(ALIGNMENT_IDS) ? I18N.get(ALIGNMENT_IDS[v]) : I18N.get(StrId::STR_JUSTIFY);
    }
    case LayoutRow::WordSpacing:
      return std::to_string(SETTINGS.wordSpacing) + "%";
    case LayoutRow::CharacterSpacing: {
      const uint8_t v = SETTINGS.characterSpacing;
      return v < std::size(CHARACTER_SPACING_IDS) ? I18N.get(CHARACTER_SPACING_IDS[v])
                                                  : I18N.get(StrId::STR_SPACING_ZERO);
    }
    case LayoutRow::ScreenMargin:
      return std::to_string(SETTINGS.screenMargin);

    default:
      return "";
  }
}

void TextSettingsActivity::confirmStyleRow(int row) {
  switch (static_cast<StyleRow>(row)) {
    case StyleRow::FocusReading:
      SETTINGS.focusReadingEnabled = !SETTINGS.focusReadingEnabled;
      break;
    case StyleRow::Hyphenation:
      SETTINGS.hyphenationEnabled = !SETTINGS.hyphenationEnabled;
      break;
    case StyleRow::EmbeddedStyle:
      SETTINGS.embeddedStyle = !SETTINGS.embeddedStyle;
      break;
    case StyleRow::AntiAliasing:
      SETTINGS.textAntiAliasing = !SETTINGS.textAntiAliasing;
      break;

    default:
      return;
  }
  SETTINGS.saveToFile();
  requestUpdate();
}

bool TextSettingsActivity::styleRowChecked(int row) const {
  switch (static_cast<StyleRow>(row)) {
    case StyleRow::FocusReading:
      return SETTINGS.focusReadingEnabled;
    case StyleRow::Hyphenation:
      return SETTINGS.hyphenationEnabled;
    case StyleRow::EmbeddedStyle:
      return SETTINGS.embeddedStyle;
    case StyleRow::AntiAliasing:
      return SETTINGS.textAntiAliasing;

    default:
      return false;
  }
}

void TextSettingsActivity::switchTab(const int direction) {
  const bool onTabBar = ringPos() == 0;
  constexpr int count = static_cast<int>(Tab::Count);
  tab_ = static_cast<Tab>((static_cast<int>(tab_) + direction + count) % count);
  if (tab_ == Tab::Family) previewChinese_ = false;
  if (tab_ == Tab::Chinese) previewChinese_ = true;
  rebuildRowItems();
  auto& n = activeNav();
  if (onTabBar) n.selected = 0;
  n.followOnBuild = true;  // pull the new tab's viewport to its remembered selection
  requestUpdate();
}

int TextSettingsActivity::listCount() const {
  switch (tab_) {
    case Tab::Family:
      return 1 + static_cast<int>(fonts_.size());
    case Tab::Chinese:
      return 1 + static_cast<int>(zhFonts_.size());
    case Tab::Layout:
      return static_cast<int>(LayoutRow::Count);
    case Tab::Style:
      return static_cast<int>(StyleRow::Count);

    default:
      return 0;
  }
}
