#include "TxtCoverPickerActivity.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Txt.h>
#include <TxtCoverMeta.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>

#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCoverStore.h"

namespace {

constexpr const char* PREVIEW_TMP_BMP_PATH = "/.crosspoint/covers/.preview.bmp";
constexpr size_t MAX_CANDIDATE_IMAGES = 48;
constexpr int PREVIEW_W = 148;
constexpr int PREVIEW_H = 222;
constexpr unsigned long PREVIEW_DEBOUNCE_MS = 350;

bool isSupportedImageFilename(const std::string& name) {
  return FsHelpers::hasJpgExtension(name) || FsHelpers::hasPngExtension(name) || FsHelpers::hasBmpExtension(name);
}

bool hasCjkCodepoints(const char* s) {
  if (!s) return false;
  const auto* p = reinterpret_cast<const unsigned char*>(s);
  while (*p) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if ((cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF) ||
        (cp >= 0x20000 && cp <= 0x2FA1F)) {
      return true;
    }
  }
  return false;
}

EpdFontFamily::Style styleForText(const char* s, const bool wantBold) {
  return (wantBold && !hasCjkCodepoints(s)) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
}

std::string formatKbSize(const size_t bytes) {
  char buf[32];
  if (bytes >= 1024 * 1024) {
    const unsigned tenths = static_cast<unsigned>((bytes * 10u + (512u * 1024u)) / (1024u * 1024u));
    std::snprintf(buf, sizeof(buf), "%u.%u MB", tenths / 10u, tenths % 10u);
  } else {
    std::snprintf(buf, sizeof(buf), "%u KB", static_cast<unsigned>(std::max<size_t>(1, (bytes + 512) / 1024)));
  }
  return buf;
}

void drawPanel(const GfxRenderer& renderer, const Rect& r, const bool doubleFrame) {
  renderer.fillRect(r.x, r.y, r.width, r.height, false);
  renderer.drawRect(r.x, r.y, r.width, r.height, doubleFrame ? 2 : 1, true);
}

void drawDefaultTxtPreviewCard(const GfxRenderer& renderer, const int x, const int y, const int w, const int h,
                               const std::string& title, const std::string& author, const char* extTag) {
  renderer.fillRect(x, y, w, h, false);
  renderer.drawRect(x, y, w, h, 2, true);
  renderer.drawRect(x + 6, y + 6, w - 12, h - 12, 1, true);

  const int tagW = renderer.getTextWidth(UI_6_FONT_ID, extTag) + 12;
  const int tagX = x + (w - tagW) / 2;
  renderer.fillRect(tagX, y + 16, tagW, 18, true);
  renderer.drawText(UI_6_FONT_ID, tagX + 6, y + 18, extTag, false);

  const char* shownTitle = title.empty() ? extTag : title.c_str();
  const EpdFontFamily::Style tStyle = styleForText(shownTitle, true);
  const auto lines = renderer.wrappedText(SMALL_FONT_ID, shownTitle, w - 24, 4, tStyle);
  const int lh = renderer.getLineHeight(SMALL_FONT_ID);
  int ty = y + 52;
  for (const auto& line : lines) {
    const int lw = renderer.getTextWidth(SMALL_FONT_ID, line.c_str(), tStyle);
    renderer.drawText(SMALL_FONT_ID, x + (w - lw) / 2, ty, line.c_str(), true, tStyle);
    ty += lh + 2;
  }

  if (!author.empty()) {
    const std::string fitAuthor = renderer.truncatedText(UI_6_FONT_ID, author.c_str(), w - 24);
    const int aw = renderer.getTextWidth(UI_6_FONT_ID, fitAuthor.c_str());
    renderer.drawLine(x + 20, y + h - 38, x + w - 20, y + h - 38, true);
    renderer.drawText(UI_6_FONT_ID, x + (w - aw) / 2, y + h - 30, fitAuthor.c_str(), true);
  }
}

}  // namespace

TxtCoverPickerActivity::PickerLayout TxtCoverPickerActivity::computeLayout() const {
  PickerLayout L;
  auto& theme = UITheme::getInstance();
  const bool hasButtonHints = !mappedInput.hasTouch();
  L.safe = theme.getScreenSafeArea(renderer, hasButtonHints, false);
  L.header = theme.getHeaderRect(renderer, hasButtonHints, false);

  L.leftX = L.safe.x + 20;
  L.rightX = L.safe.x + L.safe.width - 20;
  L.totalW = std::max(240, L.rightX - L.leftX);

  const int topAfterHeader = L.header.y + L.header.height + 10;
  L.summaryY = topAfterHeader;
  L.summaryH = 64;

  L.panesTopY = L.summaryY + L.summaryH + 14;
  const int bottomLimit = L.safe.y + L.safe.height - 8;
  L.panesH = std::max(220, bottomLimit - L.panesTopY);
  L.leftPaneW = (L.totalW * 55) / 100;
  L.rightPaneX = L.leftX + L.leftPaneW + 14;
  L.rightPaneW = std::max(156, L.rightX - L.rightPaneX);

  L.rowFirstY = L.panesTopY + 42;
  L.rowHeight = 60;
  L.rowPitch = 68;
  L.visibleRows = std::clamp((L.panesH - 52) / L.rowPitch, 2, 6);

  const int ctaW = std::max(80, L.rightPaneW - 24);
  const int ctaH = 40;
  L.ctaRect = Rect(L.rightPaneX + (L.rightPaneW - ctaW) / 2, L.panesTopY + L.panesH - ctaH - 14, ctaW, ctaH);
  return L;
}

void TxtCoverPickerActivity::onEnter() {
  Activity::onEnter();
  mappedInput.consumeSuppressedRelease();

  const size_t slash = bookPath_.find_last_of('/');
  bookFilename_ = (slash == std::string::npos) ? bookPath_ : bookPath_.substr(slash + 1);
  Txt::resolveTitleAndAuthor(bookPath_, resolvedTitle_, resolvedAuthor_);

  scanImageCandidates();
  selectedIndex_ = 0;
  previewReadyForIndex_ = -1;
  previewValid_ = false;
  previewPending_ = false;
  lastMoveMs_ = 0;
  ensureSelectedPreviewBmp();
  requestUpdate();
}

void TxtCoverPickerActivity::onExit() {
  if (Storage.exists(PREVIEW_TMP_BMP_PATH)) {
    Storage.remove(PREVIEW_TMP_BMP_PATH);
  }
  Activity::onExit();
}

void TxtCoverPickerActivity::scanImageCandidates() {
  options_.clear();
  options_.reserve(16);

  ImageOption defOpt;
  defOpt.displayName = tr(STR_TXT_COVER_USE_DEFAULT);
  defOpt.subtitle = tr(STR_TXT_COVER_AUTO_GENERATED);
  options_.push_back(std::move(defOpt));

  scanDirectoryForImages("/covers");

  const size_t slash = bookPath_.find_last_of('/');
  if (slash != std::string::npos && slash > 0) {
    const std::string parentDir = bookPath_.substr(0, slash);
    if (parentDir != "/covers") {
      scanDirectoryForImages(parentDir);
    }
  }
  scanDirectoryForImages("/");
}

void TxtCoverPickerActivity::scanDirectoryForImages(const std::string& dirPath) {
  if (options_.size() >= MAX_CANDIDATE_IMAGES) return;
  HalFile dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  auto nameBuf = makeUniqueNoThrow<char[]>(256);
  if (!nameBuf) {
    LOG_ERR("CVPK", "OOM: directory name buffer");
    dir.close();
    return;
  }

  HalFile entry = dir.openNextFile();
  while (entry && options_.size() < MAX_CANDIDATE_IMAGES) {
    if (!entry.isDirectory()) {
      entry.getName(nameBuf.get(), 256);
      const std::string name(nameBuf.get());
      if (!name.empty() && name[0] != '.' && isSupportedImageFilename(name)) {
        std::string full = (dirPath == "/") ? ("/" + name) : (dirPath + "/" + name);
        bool duplicate = false;
        for (const auto& existing : options_) {
          if (existing.fullPath == full) {
            duplicate = true;
            break;
          }
        }
        if (!duplicate) {
          ImageOption opt;
          opt.fullPath = std::move(full);
          opt.displayName = name;
          opt.subtitle = dirPath + " · " + formatKbSize(entry.size());
          options_.push_back(std::move(opt));
        }
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }
  if (entry) entry.close();
  dir.close();
}

void TxtCoverPickerActivity::ensureSelectedPreviewBmp() {
  if (previewReadyForIndex_ == selectedIndex_) return;
  previewReadyForIndex_ = selectedIndex_;
  previewValid_ = false;

  if (selectedIndex_ <= 0 || selectedIndex_ >= static_cast<int>(options_.size())) {
    return;
  }

  Storage.ensureDirectoryExists("/.crosspoint");
  Storage.ensureDirectoryExists(txtmeta::kHiddenCoversDir);
  previewValid_ = BookCoverStore::convertImageFileTo1BitBmp(options_[selectedIndex_].fullPath, PREVIEW_TMP_BMP_PATH,
                                                            PREVIEW_W, PREVIEW_H);
}

bool TxtCoverPickerActivity::drawSelectedPreviewBmp(const int covX, const int covY) const {
  if (selectedIndex_ <= 0 || !previewValid_) return false;
  auto bmpFile = makeUniqueNoThrow<HalFile>();
  if (!bmpFile || !Storage.openFileForRead("CVPK", PREVIEW_TMP_BMP_PATH, *bmpFile)) {
    return false;
  }
  auto bmp = makeUniqueNoThrow<Bitmap>(*bmpFile);
  bool drew = false;
  if (bmp && bmp->parseHeaders() == BmpReaderError::Ok) {
    renderer.fillRect(covX, covY, PREVIEW_W, PREVIEW_H, false);
    drew = renderer.drawBitmap1Bit(*bmp, covX, covY, PREVIEW_W, PREVIEW_H);
    if (drew) {
      renderer.drawRect(covX - 2, covY - 2, PREVIEW_W + 4, PREVIEW_H + 4, 2, true);
    }
  }
  bmp.reset();
  bmpFile->close();
  return drew;
}

void TxtCoverPickerActivity::selectIndex(const int newIndex) {
  {
    RenderLock lock(*this);
    selectedIndex_ = newIndex;
    previewValid_ = false;
    previewPending_ = true;
    lastMoveMs_ = millis();
  }
  requestUpdate();
}

void TxtCoverPickerActivity::confirmSelection() {
  bool ok = false;
  if (selectedIndex_ <= 0 || selectedIndex_ >= static_cast<int>(options_.size())) {
    ok = BookCoverStore::clearBoundCover(bookPath_);
  } else {
    ok = BookCoverStore::bindCoverAndMetaFromImageFile(bookPath_, options_[selectedIndex_].fullPath);
  }
  ActivityResult result;
  result.isCancelled = !ok;
  setResult(std::move(result));
  finish();
}

void TxtCoverPickerActivity::loop() {
  using B = MappedInputManager::Button;
  const int count = static_cast<int>(options_.size());
  if (count <= 0) return;

  if (mappedInput.hasTouch()) {
    const PickerLayout L = computeLayout();
    const int pageStart = std::clamp(selectedIndex_ - L.visibleRows / 2, 0, std::max(0, count - L.visibleRows));
    const int visibleCount = std::min(L.visibleRows, count - pageStart);
    int touchedRow = -1;
    const auto rt = mappedInput.rowTouch(touchedRow, L.rowFirstY, L.rowPitch, visibleCount, L.leftX + 8,
                                         L.leftX + L.leftPaneW - 8, L.rowHeight);
    if (rt == MappedInputManager::RowTouch::Tap && touchedRow >= 0) {
      const int idx = pageStart + touchedRow;
      if (idx != selectedIndex_) {
        selectIndex(idx);
      }
      return;
    }
    int tx = -1;
    int ty = -1;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      if (tx >= L.ctaRect.x && tx <= L.ctaRect.x + L.ctaRect.width && ty >= L.ctaRect.y &&
          ty <= L.ctaRect.y + L.ctaRect.height) {
        confirmSelection();
        return;
      }
    }
  }

  navigator_.onPrevious([this, count] { selectIndex((selectedIndex_ - 1 + count) % count); });
  navigator_.onNext([this, count] { selectIndex((selectedIndex_ + 1) % count); });

  if (mappedInput.wasReleased(B::Confirm)) {
    confirmSelection();
    return;
  } else if (mappedInput.wasReleased(B::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (previewPending_ && (millis() - lastMoveMs_ >= PREVIEW_DEBOUNCE_MS)) {
    previewPending_ = false;
    bool ready = false;
    {
      RenderLock lock(*this);
      ensureSelectedPreviewBmp();
      ready = previewValid_;
    }
    if (ready) {
      requestUpdate();
    }
  }
}

void TxtCoverPickerActivity::render(RenderLock&&) {
  const PickerLayout L = computeLayout();

  auto drawBody = [&]() {
    renderer.clearScreen();
    GUI.drawHeader(renderer, L.header, tr(STR_TXT_COVER_TITLE), bookFilename_.c_str());

    drawPanel(renderer, Rect(L.leftX, L.summaryY, L.totalW, L.summaryH), false);

    const int summaryTextW = std::max(80, L.totalW - 32);
    const std::string promptLine = renderer.truncatedText(UI_6_FONT_ID, tr(STR_TXT_COVER_SETTING_FOR), summaryTextW);
    renderer.drawText(UI_6_FONT_ID, L.leftX + 14, L.summaryY + 11, promptLine.c_str(), true);

    std::string bookDesc = resolvedTitle_;
    if (!resolvedAuthor_.empty()) {
      bookDesc += " · " + resolvedAuthor_;
    }
    const int bookFont = hasCjkCodepoints(bookDesc.c_str()) ? SMALL_FONT_ID : UI_10_FONT_ID;
    const EpdFontFamily::Style bookStyle = styleForText(bookDesc.c_str(), true);
    const std::string clampedBook = renderer.truncatedText(bookFont, bookDesc.c_str(), summaryTextW, bookStyle);
    renderer.drawText(bookFont, L.leftX + 14, L.summaryY + 33, clampedBook.c_str(), true, bookStyle);

    // Left Column: SD Card Image Candidates
    drawPanel(renderer, Rect(L.leftX, L.panesTopY, L.leftPaneW, L.panesH), false);
    renderer.fillRect(L.leftX + 2, L.panesTopY + 2, L.leftPaneW - 4, 30, true);
    char leftHdr[64];
    const int imgCount = std::max(0, static_cast<int>(options_.size()) - 1);
    std::snprintf(leftHdr, sizeof(leftHdr), tr(STR_TXT_COVER_SD_IMAGES), imgCount);
    const EpdFontFamily::Style leftHdrStyle = styleForText(leftHdr, true);
    const std::string clampedLeftHdr = renderer.truncatedText(SMALL_FONT_ID, leftHdr, L.leftPaneW - 20, leftHdrStyle);
    renderer.drawText(SMALL_FONT_ID, L.leftX + 12, L.panesTopY + 8, clampedLeftHdr.c_str(), false, leftHdrStyle);

    const int totalOpts = static_cast<int>(options_.size());
    const int pageStart = std::clamp(selectedIndex_ - L.visibleRows / 2, 0, std::max(0, totalOpts - L.visibleRows));
    const int visibleEnd = std::min(totalOpts, pageStart + L.visibleRows);

    int rowY = L.rowFirstY;
    for (int i = pageStart; i < visibleEnd; ++i) {
      const bool sel = (i == selectedIndex_);
      const int rx = L.leftX + 10;
      const int rw = std::max(60, L.leftPaneW - 20);

      if (sel) {
        renderer.fillRoundedRect(rx, rowY, rw, L.rowHeight, 6, Color::Black);
        renderer.drawRoundedRect(rx + 3, rowY + 3, rw - 6, L.rowHeight - 6, 1, 4, false);
      } else {
        renderer.fillRoundedRect(rx, rowY, rw, L.rowHeight, 6, Color::White);
        renderer.drawRoundedRect(rx, rowY, rw, L.rowHeight, 1, 6, true);
      }

      const int textLeft = rx + 10;
      const int maxRowTextW = std::max(36, rx + rw - 8 - textLeft);
      const EpdFontFamily::Style rStyle = styleForText(options_[i].displayName.c_str(), sel);
      const std::string rTitle =
          renderer.truncatedText(SMALL_FONT_ID, options_[i].displayName.c_str(), maxRowTextW, rStyle);
      const std::string rSub = renderer.truncatedText(UI_6_FONT_ID, options_[i].subtitle.c_str(), maxRowTextW);

      renderer.drawText(SMALL_FONT_ID, textLeft, rowY + 9, rTitle.c_str(), !sel, rStyle);
      renderer.drawText(UI_6_FONT_ID, textLeft, rowY + 34, rSub.c_str(), !sel);
      rowY += L.rowPitch;
    }

    if (L.panesTopY + L.panesH - rowY >= 44) {
      const std::string tip1 = renderer.truncatedText(UI_6_FONT_ID, tr(STR_TXT_COVER_TIP_1), L.leftPaneW - 20);
      const std::string tip2 = renderer.truncatedText(UI_6_FONT_ID, tr(STR_TXT_COVER_TIP_2), L.leftPaneW - 20);
      const int midLeftX = L.leftX + L.leftPaneW / 2;
      renderer.drawText(UI_6_FONT_ID, midLeftX - renderer.getTextWidth(UI_6_FONT_ID, tip1.c_str()) / 2,
                        L.panesTopY + L.panesH - 40, tip1.c_str(), true);
      renderer.drawText(UI_6_FONT_ID, midLeftX - renderer.getTextWidth(UI_6_FONT_ID, tip2.c_str()) / 2,
                        L.panesTopY + L.panesH - 22, tip2.c_str(), true);
    }

    // Right Column: Live 1:1 1-Bit Atkinson Cover Preview
    drawPanel(renderer, Rect(L.rightPaneX, L.panesTopY, L.rightPaneW, L.panesH), true);
    renderer.fillRect(L.rightPaneX + 4, L.panesTopY + 4, L.rightPaneW - 8, 28, true);
    const char* rawRightHdr = tr(STR_TXT_COVER_PREVIEW_HEADER);
    const EpdFontFamily::Style rightHdrStyle = styleForText(rawRightHdr, true);
    const std::string rightHdr = renderer.truncatedText(SMALL_FONT_ID, rawRightHdr, L.rightPaneW - 16, rightHdrStyle);
    renderer.drawText(
        SMALL_FONT_ID,
        L.rightPaneX + (L.rightPaneW - renderer.getTextWidth(SMALL_FONT_ID, rightHdr.c_str(), rightHdrStyle)) / 2,
        L.panesTopY + 9, rightHdr.c_str(), false, rightHdrStyle);

    const int covX = L.rightPaneX + (L.rightPaneW - PREVIEW_W) / 2;
    const int covY = L.panesTopY + 44;
    if (!drawSelectedPreviewBmp(covX, covY)) {
      const char* extTag = FsHelpers::hasMarkdownExtension(bookPath_) ? "MD" : "TXT";
      drawDefaultTxtPreviewCard(renderer, covX, covY, PREVIEW_W, PREVIEW_H, resolvedTitle_, resolvedAuthor_, extTag);
    }

    const int midRightX = L.rightPaneX + L.rightPaneW / 2;
    const int notesY = covY + PREVIEW_H + 12;
    if (L.ctaRect.y - notesY >= 56) {
      const char* const notes[3] = {
          tr(STR_TXT_COVER_PREVIEW_NOTE_1),
          tr(STR_TXT_COVER_PREVIEW_NOTE_2),
          tr(STR_TXT_COVER_PREVIEW_NOTE_3),
      };
      for (int n = 0; n < 3; ++n) {
        const std::string line = renderer.truncatedText(UI_6_FONT_ID, notes[n], L.rightPaneW - 20);
        renderer.drawText(UI_6_FONT_ID, midRightX - renderer.getTextWidth(UI_6_FONT_ID, line.c_str()) / 2,
                          notesY + n * 18, line.c_str(), true);
      }
    }

    renderer.fillRoundedRect(L.ctaRect.x, L.ctaRect.y, L.ctaRect.width, L.ctaRect.height, 8, Color::Black);
    renderer.drawRoundedRect(L.ctaRect.x + 3, L.ctaRect.y + 3, L.ctaRect.width - 6, L.ctaRect.height - 6, 1, 6, false);
    const char* rawCta = tr(STR_TXT_COVER_BIND_CTA);
    const EpdFontFamily::Style ctaStyle = styleForText(rawCta, true);
    const std::string ctaLabel = renderer.truncatedText(SMALL_FONT_ID, rawCta, L.ctaRect.width - 14, ctaStyle);
    renderer.drawText(
        SMALL_FONT_ID,
        L.ctaRect.x + (L.ctaRect.width - renderer.getTextWidth(SMALL_FONT_ID, ctaLabel.c_str(), ctaStyle)) / 2,
        L.ctaRect.y + std::max(2, (L.ctaRect.height - renderer.getTextHeight(SMALL_FONT_ID)) / 2) - 1, ctaLabel.c_str(),
        false, ctaStyle);

    if (!mappedInput.hasTouch()) {
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  };

  if (auto* fcm = renderer.getFontCacheManager()) {
    auto scope = fcm->createPrewarmScope();
    drawBody();
    scope.endScanAndPrewarm();
    drawBody();
  } else {
    drawBody();
  }

  renderer.displayBuffer();
}
