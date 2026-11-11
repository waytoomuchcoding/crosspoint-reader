#include "TxtEncodingActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Txt.h>
#include <TxtEncodingSupport.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <iterator>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr TextEncoding kEncodingOrder[] = {
    TextEncoding::Auto, TextEncoding::Utf8,    TextEncoding::Gb18030,
    TextEncoding::Big5, TextEncoding::Utf16Le, TextEncoding::Utf16Be,
};

bool hasCjkText(const char* text) {
  if (!text) return false;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  while (*p) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if ((cp >= 0x3000 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF) ||
        (cp >= 0x20000 && cp <= 0x2FA1F)) {
      return true;
    }
  }
  return false;
}

}  // namespace

TextEncoding TxtEncodingActivity::currentEncoding() const {
  if (position_ < 0 || position_ >= static_cast<int>(std::size(kEncodingOrder))) {
    return TextEncoding::Auto;
  }
  return kEncodingOrder[position_];
}

void TxtEncodingActivity::refreshPreview() {
  txtenc::decodePreviewSample(rawSample_.data(), rawSample_.size(), sampleOffset_, currentEncoding(), detectedEncoding_,
                              previewText_);
  // Normalize control chars so the preview renders cleanly in wrapped lines
  for (char& c : previewText_) {
    if (c == '\r' || c == '\t') {
      c = ' ';
    }
  }
}

void TxtEncodingActivity::onEnter() {
  Activity::onEnter();
  initialEncoding_ = Txt::readEncodingOverride(cachePath_);
  position_ = 0;
  for (size_t i = 0; i < std::size(kEncodingOrder); ++i) {
    if (kEncodingOrder[i] == initialEncoding_) {
      position_ = static_cast<int>(i);
      break;
    }
  }
  txtenc::loadRawPreviewSample(bookPath_, previewByteOffset_, rawSample_, detectedEncoding_, sampleOffset_);
  refreshPreview();
  requestUpdate();
}

void TxtEncodingActivity::loop() {
  using B = MappedInputManager::Button;
  const int last = static_cast<int>(std::size(kEncodingOrder)) - 1;
  navigator_.onPrevious([this] {
    if (position_ > 0) {
      --position_;
      refreshPreview();
      requestUpdate();
    }
  });
  navigator_.onNext([this, last] {
    if (position_ < last) {
      ++position_;
      refreshPreview();
      requestUpdate();
    }
  });
  if (mappedInput.wasReleased(B::Confirm)) {
    const TextEncoding chosen = currentEncoding();
    const bool changed = (chosen != initialEncoding_);
    if (changed) {
      Txt::writeEncodingOverride(cachePath_, chosen);
    }
    ActivityResult result;
    result.isCancelled = !changed;
    setResult(std::move(result));
    finish();
  } else if (mappedInput.wasReleased(B::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  }
}

void TxtEncodingActivity::render(RenderLock&&) {
  renderer.clearScreen();
  auto& theme = UITheme::getInstance();
  const auto& metrics = theme.getMetrics();
  const Rect safe = theme.getScreenSafeArea(renderer, /*hasFrontButtonHints=*/true, /*hasSideButtonHints=*/false);
  const Rect headerRect = theme.getHeaderRect(renderer, /*hasFrontButtonHints=*/true, /*hasSideButtonHints=*/false);

  const int last = static_cast<int>(std::size(kEncodingOrder)) - 1;
  const TextEncoding chosen = currentEncoding();
  char labelBuf[64];
  if (chosen == TextEncoding::Auto) {
    std::snprintf(labelBuf, sizeof(labelBuf), "%s (%s)", tr(STR_CLOCK_DST_AUTO),
                  textencoding::encodingShortName(detectedEncoding_));
  } else {
    std::snprintf(labelBuf, sizeof(labelBuf), "%s", textencoding::encodingShortName(chosen));
  }

  char valueBuf[96];
  std::snprintf(valueBuf, sizeof(valueBuf), "%s  %s  %s", position_ > 0 ? "<" : " ", labelBuf,
                position_ < last ? ">" : " ");

  const int contentX = safe.x + metrics.contentSidePadding;
  const int contentWidth = std::max(60, safe.width - 2 * metrics.contentSidePadding);
  const auto helpLines = renderer.wrappedText(SMALL_FONT_ID, tr(STR_TEXT_ENCODING_HELP), contentWidth, 4);

  auto drawBody = [&]() {
    GUI.drawHeader(renderer, headerRect, tr(STR_TEXT_ENCODING));

    int y = headerRect.y + headerRect.height + metrics.verticalSpacing * 3;
    for (const auto& line : helpLines) {
      renderer.drawText(SMALL_FONT_ID, contentX, y, line.c_str());
      y += renderer.getLineHeight(SMALL_FONT_ID) + 2;
    }
    y += metrics.verticalSpacing * 3;

    const bool valCjk = hasCjkText(valueBuf);
    const int valFont = valCjk ? SMALL_FONT_ID : UI_12_FONT_ID;
    const EpdFontFamily::Style valStyle = valCjk ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD;
    UITheme::drawCenteredText(renderer, safe, valFont, y, valueBuf, true, valStyle);
    y += renderer.getLineHeight(valFont) + metrics.verticalSpacing * 2;

    // Position tick bar
    const int step = contentWidth / std::max(1, last);
    renderer.drawLine(contentX, y + 6, contentX + step * last, y + 6, 2, true);
    for (int k = 0; k <= last; ++k) {
      const int tx = contentX + step * k;
      if (k == position_) {
        renderer.fillRect(tx - 5, y, 11, 13, true);
      } else {
        renderer.fillRect(tx - 1, y + 2, 3, 9, true);
      }
    }
    y += 22 + metrics.verticalSpacing * 2;

    // Live 512-byte preview box
    const char* rawPreviewLbl = tr(STR_TEXT_ENCODING_PREVIEW);
    const bool lblCjk = hasCjkText(rawPreviewLbl);
    const int pLblFont = lblCjk ? SMALL_FONT_ID : UI_10_FONT_ID;
    const EpdFontFamily::Style pLblStyle = lblCjk ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD;
    renderer.drawText(pLblFont, contentX, y, rawPreviewLbl, true, pLblStyle);
    y += renderer.getLineHeight(pLblFont) + metrics.verticalSpacing;

    const int boxTop = y;
    const int boxBottom = safe.y + safe.height - metrics.verticalSpacing * 2;
    const int boxHeight = std::max(60, boxBottom - boxTop);
    renderer.drawRect(contentX, boxTop, contentWidth, boxHeight, true);

    const int innerPad = 8;
    const int innerWidth = std::max(40, contentWidth - innerPad * 2);
    const int previewFontId = hasCjkText(previewText_.c_str()) ? SMALL_FONT_ID : UI_10_FONT_ID;
    const int lineH = renderer.getLineHeight(previewFontId) + 3;
    const int maxLines = std::max(1, (boxHeight - innerPad * 2) / std::max(1, lineH));

    int lineY = boxTop + innerPad;
    int linesDrawn = 0;
    size_t pos = 0;
    while (pos < previewText_.size() && linesDrawn < maxLines) {
      const size_t nl = previewText_.find('\n', pos);
      const std::string para =
          (nl == std::string::npos) ? previewText_.substr(pos) : previewText_.substr(pos, nl - pos);
      pos = (nl == std::string::npos) ? previewText_.size() : (nl + 1);
      if (para.empty()) continue;

      const auto wrapped = renderer.wrappedText(previewFontId, para.c_str(), innerWidth, maxLines - linesDrawn);
      for (const auto& wline : wrapped) {
        if (linesDrawn >= maxLines) break;
        renderer.drawText(previewFontId, contentX + innerPad, lineY, wline.c_str());
        lineY += lineH;
        linesDrawn++;
      }
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
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
