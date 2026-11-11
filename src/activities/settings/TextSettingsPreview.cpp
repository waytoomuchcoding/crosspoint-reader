#include "TextSettingsPreview.h"

#include <EpdFontFamily.h>
#include <Epub/ParsedText.h>
#include <Epub/blocks/BlockStyle.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <ZhDict.h>

#include <algorithm>
#include <string>
#include <utility>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace textsettings {

PreviewLayout::PreviewLayout() = default;
PreviewLayout::~PreviewLayout() = default;

namespace {

// Map the paragraph-alignment setting to the engine's CssTextAlign (BOOK_STYLE = justified)
CssTextAlign toCssAlign(uint8_t align) {
  if (align == CrossPointSettings::BOOK_STYLE) return CssTextAlign::Justify;
  return static_cast<CssTextAlign>(align);
}

constexpr const char* ZH_PREVIEW_TEXT = "天地玄黄，宇宙洪荒。日月盈昃，辰宿列张。寒来暑往，秋收冬藏。";

const char* sampleTextForFont(const GfxRenderer& renderer, const int fontId) {
  const auto it = renderer.getFontMap().find(fontId);
  if (it != renderer.getFontMap().end() && it->second.hasCodepoint(0x4E00)) {
    return ZH_PREVIEW_TEXT;
  }
  return I18N.get(StrId::STR_FONT_PREVIEW_TEXT);
}

// Lay the sample text out through the reader engine into layout.lines
void relayout(PreviewLayout& layout, const GfxRenderer& renderer, int fontId, int textWidth) {
  layout.lines.clear();

  BlockStyle style;
  style.alignment = toCssAlign(SETTINGS.paragraphAlignment);
  style.textAlignDefined = true;  // honor the user's choice; RTL auto-detected from text

  const uint32_t savedRubyThreshold = zh::rubyThreshold();
  const bool savedRubyUnderLine = zh::rubyUnderLine();
  zh::setRubyThreshold(zh::rubyThresholdForStep(SETTINGS.zhRuby));
  zh::setRubyUnderLine(SETTINGS.zhRubyStyle != CrossPointSettings::ZH_RUBY_BESIDE);

  ParsedText parsed(SETTINGS.hyphenationEnabled != 0, SETTINGS.focusReadingEnabled != 0, style,
                    SETTINGS.paragraphIndentSpaces);

  // Feed one space-separated word at a time; addWord handles NFC/CJK/RTL/focus splitting
  const char* text = sampleTextForFont(renderer, fontId);
  std::string word;
  for (const char* p = text;; p++) {
    if (*p == ' ' || *p == '\0') {
      if (!word.empty()) {
        parsed.addWord(word, EpdFontFamily::REGULAR);
        word.clear();
      }
      if (*p == '\0') break;
    } else {
      word.push_back(*p);
    }
  }

  parsed.layoutAndExtractLines(
      renderer, fontId, static_cast<uint16_t>(textWidth),
      [&layout](std::unique_ptr<TextBlock> line, uint32_t) { layout.lines.push_back(std::move(line)); }, true,
      SETTINGS.getCharacterSpacing(), SETTINGS.wordSpacing);

  zh::sharedDict().close();
  zh::setRubyThreshold(savedRubyThreshold);
  zh::setRubyUnderLine(savedRubyUnderLine);
}

}  // namespace

void renderPreview(const GfxRenderer& renderer, PreviewLayout& layout, int previewPadding, int labelGap, Rect bounds,
                   const char* caption) {
  const int top = bounds.y;
  const int height = bounds.height;
  const int left = bounds.x + previewPadding;
  const int width = bounds.width - (previewPadding * 2);
  if (width <= 0 || height <= 0) return;

  const int labelH = renderer.getTextHeight(UI_10_FONT_ID);
  const int labelReserved = labelH + labelGap + previewPadding;

  const int labelY = top + height - previewPadding - labelH;
  renderer.drawText(UI_10_FONT_ID, left, labelY, caption);

  const int fontId = SETTINGS.getReaderFontId();
  if (fontId == 0) return;

  const int lineH = renderer.getTextHeight(fontId);
  if (lineH <= 0) return;

  const int textLeft = left + SETTINGS.screenMargin;
  const int textWidth = width - 2 * SETTINGS.screenMargin;
  if (textWidth <= 0) return;

  const float compression = SETTINGS.getReaderLineCompression();
  const int lineAdvance = std::max(1, renderer.getLineHeight(fontId, compression));
  const int paragraphGap = SETTINGS.extraParagraphSpacing ? lineAdvance / 2 : 0;

  // Re-lay-out (and re-prewarm glyphs) only when a layout-affecting setting or the
  // geometry changed; else reuse the cache. The prewarm inputs are (fontId, constant
  // sample text, styleMask<-focusReading), all of which are key fields, so a matching
  // key means an identical prewarm call. This relies on nothing else evicting the SD
  // glyph cache while this activity is up — true today: the only evictor is
  // FontCacheManager::PrewarmScope, used solely by the reader/dictionary activities.
  const PreviewKey key{.fontId = fontId,
                       .fontPointSize = SETTINGS.fontPointSize,
                       .screenMargin = SETTINGS.screenMargin,
                       .textWidth = textWidth,
                       .lineCompression = compression,
                       .alignment = SETTINGS.paragraphAlignment,
                       .extraParagraphSpacing = SETTINGS.extraParagraphSpacing != 0,
                       .paragraphIndentSpaces = SETTINGS.paragraphIndentSpaces,
                       .characterSpacing = SETTINGS.getCharacterSpacing(),
                       .wordSpacingPercent = SETTINGS.wordSpacing,
                       .focusReading = SETTINGS.focusReadingEnabled != 0,
                       .hyphenation = SETTINGS.hyphenationEnabled != 0,
                       .zhRuby = SETTINGS.zhRuby,
                       .zhRubyStyle = SETTINGS.zhRubyStyle};
  if (key != layout.key) {
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->prewarmCache(fontId, sampleTextForFont(renderer, fontId), SETTINGS.focusReadingEnabled ? 0x03 : 0x01);
    }
    relayout(layout, renderer, fontId, textWidth);
    layout.key = key;
  }

  // Draw the sample twice so the paragraph gap is visible
  int y = top + previewPadding;
  const int textBottomLimit = top + height - labelReserved;
  for (int paragraph = 0; paragraph < 2; paragraph++) {
    for (const auto& line : layout.lines) {
      const int rubyShift = line->getRubyShift(renderer.getFontAscenderSize(fontId));
      const int rubyUnder = line->getRubyUnderHeight(renderer, fontId);
      const int unspacedGap = std::max(0, renderer.getLineHeight(fontId) - lineAdvance);
      const int stepY = lineAdvance + rubyShift + (rubyUnder > 0 ? rubyUnder + unspacedGap : 0);
      if (y + lineH + rubyShift + rubyUnder > textBottomLimit) return;
      line->render(renderer, fontId, textLeft, y);
      y += stepY;
    }
    y += paragraphGap;
  }
}

}  // namespace textsettings
