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
#include "SdCardFontSystem.h"
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

const char* sampleTextForFont(const GfxRenderer& renderer, const int fontId, const bool previewChinese) {
  if (previewChinese) return ZH_PREVIEW_TEXT;
  const auto it = renderer.getFontMap().find(fontId);
  if (it != renderer.getFontMap().end() && it->second.hasCodepoint(0x4E00)) {
    return ZH_PREVIEW_TEXT;
  }
  return I18N.get(StrId::STR_FONT_PREVIEW_TEXT);
}

// Lay the sample text out through the reader engine into layout.lines
void relayout(PreviewLayout& layout, const GfxRenderer& renderer, int fontId, int textWidth, bool previewChinese) {
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
  const char* text = sampleTextForFont(renderer, fontId, previewChinese);
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
                   const char* caption, bool previewChinese) {
  const int top = bounds.y;
  const int height = bounds.height;
  const int left = bounds.x + previewPadding;
  const int width = bounds.width - (previewPadding * 2);
  if (width <= 0 || height <= 0) return;

  const int labelH = renderer.getTextHeight(UI_10_FONT_ID);
  const int labelReserved = labelH + labelGap + previewPadding;

  const int labelY = top + height - previewPadding - labelH;
  renderer.drawText(UI_10_FONT_ID, left, labelY, caption);

  const int fontId = (previewChinese && sdFontSystem.getZhReaderFontId() != 0) ? sdFontSystem.getZhReaderFontId()
                                                                               : SETTINGS.getReaderFontId();
  if (fontId == 0) return;

  const int lineH = renderer.getTextHeight(fontId);
  if (lineH <= 0) return;

  const int textLeft = left + SETTINGS.screenMargin;
  const int textWidth = width - 2 * SETTINGS.screenMargin;
  if (textWidth <= 0) return;

  const float compression = SETTINGS.getReaderLineCompression();
  const int baseFontLineH = renderer.getLineHeight(fontId);
  const int lineAdvance = std::max(1, renderer.getLineHeight(fontId, compression));
  const int paragraphGap = SETTINGS.extraParagraphSpacing ? lineAdvance / 2 : 0;

  const int activePt = previewChinese
                           ? (SETTINGS.zhFontPointSize ? SETTINGS.zhFontPointSize : sdFontSystem.resolvedZhPointSize())
                           : SETTINGS.fontPointSize;
  const PreviewKey key{.fontId = fontId,
                       .fontPointSize = activePt,
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
                       .zhRubyStyle = SETTINGS.zhRubyStyle,
                       .previewChinese = previewChinese};
  if (key != layout.key) {
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->prewarmCache(fontId, sampleTextForFont(renderer, fontId, previewChinese),
                        SETTINGS.focusReadingEnabled ? 0x03 : 0x01);
    }
    relayout(layout, renderer, fontId, textWidth, previewChinese);
    layout.key = key;
  }

  // Draw the sample twice so the paragraph gap is visible
  int y = top + previewPadding;
  const int textBottomLimit = top + height - labelReserved;
  for (int paragraph = 0; paragraph < 2; paragraph++) {
    for (const auto& line : layout.lines) {
      const int rubyShift = line->getRubyShift(renderer.getFontAscenderSize(fontId));
      const int rubyUnder = line->getRubyUnderHeight(renderer, fontId);
      const int stepY = (rubyUnder > 0 ? std::max(lineAdvance, baseFontLineH + rubyUnder) : lineAdvance) + rubyShift;
      if (y + lineH + rubyShift + rubyUnder > textBottomLimit) return;
      line->render(renderer, fontId, textLeft, y);
      y += stepY;
    }
    y += paragraphGap;
  }
}

}  // namespace textsettings
