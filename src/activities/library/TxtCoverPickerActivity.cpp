#include "TxtCoverPickerActivity.h"

#include <Bitmap.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Txt.h>
#include <TxtCoverMeta.h>

#include <algorithm>
#include <cstdio>

#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "components/ProceduralBookCover.h"
#include "components/UITheme.h"
#include "components/themes/shibadesk/ShibaArt.h"
#include "components/themes/shibadesk/ShibaDeskChrome.h"
#include "components/themes/shibadesk/ShibaStatusBar.h"
#include "fontIds.h"
#include "util/BookCoverStore.h"

namespace {

constexpr const char* PREVIEW_TMP_BMP_PATH = "/.crosspoint/covers/.preview.bmp";
constexpr size_t MAX_CANDIDATE_IMAGES = 48;
constexpr int PREVIEW_W = 148;
constexpr int PREVIEW_H = 222;

bool isSupportedImageFilename(const std::string& name) {
  return FsHelpers::hasJpgExtension(name) || FsHelpers::hasPngExtension(name) || FsHelpers::hasBmpExtension(name);
}

std::string formatKbSize(const size_t bytes) {
  char buf[32];
  if (bytes >= 1024 * 1024) {
    std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else {
    std::snprintf(buf, sizeof(buf), "%u KB", static_cast<unsigned>(std::max<size_t>(1, (bytes + 512) / 1024)));
  }
  return buf;
}

void drawThemedPanel(const GfxRenderer& renderer, const Rect& r, const bool isShiba, const bool doubleFrame) {
  if (isShiba) {
    shibadesk::drawShibaCard(renderer, r, doubleFrame, 4);
    return;
  }
  renderer.fillRect(r.x, r.y, r.width, r.height, false);
  renderer.drawRect(r.x, r.y, r.width, r.height, doubleFrame ? 2 : 1, true);
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

  const int topAfterHeader = UITheme::isShibaDesk() ? (L.safe.y + 82) : (L.header.y + L.header.height + 10);
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
        selectedIndex_ = idx;
        ensureSelectedPreviewBmp();
        requestUpdate();
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

  navigator_.onPrevious([this, count] {
    selectedIndex_ = (selectedIndex_ - 1 + count) % count;
    ensureSelectedPreviewBmp();
    requestUpdate();
  });
  navigator_.onNext([this, count] {
    selectedIndex_ = (selectedIndex_ + 1) % count;
    ensureSelectedPreviewBmp();
    requestUpdate();
  });

  if (mappedInput.wasReleased(B::Confirm)) {
    confirmSelection();
  } else if (mappedInput.wasReleased(B::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  }
}

void TxtCoverPickerActivity::render(RenderLock&&) {
  const PickerLayout L = computeLayout();
  const bool isShiba = UITheme::isShibaDesk();

  auto drawBody = [&]() {
    renderer.clearScreen();
    if (isShiba) {
      shibadesk::drawStaggeredDotBackground(renderer, Rect(0, L.safe.y + 36, renderer.getScreenWidth(), L.safe.height));
      shibadesk::drawShibaStatusBar(renderer, tr(STR_TXT_COVER_TITLE), bookFilename_.c_str());
    } else {
      GUI.drawHeader(renderer, L.header, tr(STR_TXT_COVER_TITLE), bookFilename_.c_str());
    }

    drawThemedPanel(renderer, Rect(L.leftX, L.summaryY, L.totalW, L.summaryH), isShiba, false);
    if (isShiba) {
      shibadesk::stampShibaOnLedge(renderer, L.rightX - shibadesk::SHIBA_SPRITE_W - 6, L.summaryY,
                                   shibadesk::ShibaPose::LoafAwake);
    }

    const int summaryTextW = std::max(80, L.totalW - (isShiba ? (shibadesk::SHIBA_SPRITE_W + 32) : 32));
    const std::string promptLine =
        shibadesk::clampSingleLine(renderer, UI_6_FONT_ID, tr(STR_TXT_COVER_SETTING_FOR), summaryTextW);
    renderer.drawText(UI_6_FONT_ID, L.leftX + 14, L.summaryY + 11, promptLine.c_str(), true);

    std::string bookDesc = resolvedTitle_;
    if (!resolvedAuthor_.empty()) {
      bookDesc += " · " + resolvedAuthor_;
    }
    const int bookFont = shibadesk::hasCjkCodepoints(bookDesc.c_str()) ? SMALL_FONT_ID : UI_10_FONT_ID;
    const EpdFontFamily::Style bookStyle = shibadesk::styleForText(bookDesc.c_str(), true);
    const std::string clampedBook =
        shibadesk::clampSingleLine(renderer, bookFont, bookDesc.c_str(), summaryTextW, bookStyle);
    renderer.drawText(bookFont, L.leftX + 14, L.summaryY + 33, clampedBook.c_str(), true, bookStyle);

    // Left Column: SD Card Image Candidates
    drawThemedPanel(renderer, Rect(L.leftX, L.panesTopY, L.leftPaneW, L.panesH), isShiba, false);
    renderer.fillRect(L.leftX + 2, L.panesTopY + 2, L.leftPaneW - 4, 30, true);
    char leftHdr[64];
    const int imgCount = std::max(0, static_cast<int>(options_.size()) - 1);
    std::snprintf(leftHdr, sizeof(leftHdr), tr(STR_TXT_COVER_SD_IMAGES), imgCount);
    const EpdFontFamily::Style leftHdrStyle = shibadesk::styleForText(leftHdr, true);
    const std::string clampedLeftHdr =
        shibadesk::clampSingleLine(renderer, SMALL_FONT_ID, leftHdr, L.leftPaneW - 20, leftHdrStyle);
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

      int textLeft = rx + 10;
      if (isShiba) {
        const shibadesk::ShibaIcon rowIcon = (i == 0) ? shibadesk::ShibaIcon::Book : shibadesk::ShibaIcon::Image;
        shibadesk::drawIcon(renderer, rowIcon, rx + 10, rowY + 12, !sel);
        textLeft = rx + 30;
      }

      const int maxRowTextW = std::max(36, rx + rw - 8 - textLeft);
      const EpdFontFamily::Style rStyle = shibadesk::styleForText(options_[i].displayName.c_str(), sel);
      const std::string rTitle =
          shibadesk::clampSingleLine(renderer, SMALL_FONT_ID, options_[i].displayName.c_str(), maxRowTextW, rStyle);
      const std::string rSub =
          shibadesk::clampSingleLine(renderer, UI_6_FONT_ID, options_[i].subtitle.c_str(), maxRowTextW);

      renderer.drawText(SMALL_FONT_ID, textLeft, rowY + 9, rTitle.c_str(), !sel, rStyle);
      renderer.drawText(UI_6_FONT_ID, textLeft, rowY + 34, rSub.c_str(), !sel);
      rowY += L.rowPitch;
    }

    if (L.panesTopY + L.panesH - rowY >= 44) {
      const std::string tip1 =
          shibadesk::clampSingleLine(renderer, UI_6_FONT_ID, tr(STR_TXT_COVER_TIP_1), L.leftPaneW - 20);
      const std::string tip2 =
          shibadesk::clampSingleLine(renderer, UI_6_FONT_ID, tr(STR_TXT_COVER_TIP_2), L.leftPaneW - 20);
      const int midLeftX = L.leftX + L.leftPaneW / 2;
      renderer.drawText(UI_6_FONT_ID, midLeftX - renderer.getTextWidth(UI_6_FONT_ID, tip1.c_str()) / 2,
                        L.panesTopY + L.panesH - 40, tip1.c_str(), true);
      renderer.drawText(UI_6_FONT_ID, midLeftX - renderer.getTextWidth(UI_6_FONT_ID, tip2.c_str()) / 2,
                        L.panesTopY + L.panesH - 22, tip2.c_str(), true);
    }

    // Right Column: Live 1:1 1-Bit Atkinson Cover Preview
    drawThemedPanel(renderer, Rect(L.rightPaneX, L.panesTopY, L.rightPaneW, L.panesH), isShiba, true);
    renderer.fillRect(L.rightPaneX + 4, L.panesTopY + 4, L.rightPaneW - 8, 28, true);
    const char* rawRightHdr = tr(STR_TXT_COVER_PREVIEW_HEADER);
    const EpdFontFamily::Style rightHdrStyle = shibadesk::styleForText(rawRightHdr, true);
    const std::string rightHdr =
        shibadesk::clampSingleLine(renderer, SMALL_FONT_ID, rawRightHdr, L.rightPaneW - 16, rightHdrStyle);
    renderer.drawText(
        SMALL_FONT_ID,
        L.rightPaneX + (L.rightPaneW - renderer.getTextWidth(SMALL_FONT_ID, rightHdr.c_str(), rightHdrStyle)) / 2,
        L.panesTopY + 9, rightHdr.c_str(), false, rightHdrStyle);

    const int covX = L.rightPaneX + (L.rightPaneW - PREVIEW_W) / 2;
    const int covY = L.panesTopY + 44;
    if (!drawSelectedPreviewBmp(covX, covY)) {
      const char* extTag = FsHelpers::hasMarkdownExtension(bookPath_) ? "MD" : "TXT";
      ProceduralBookCover::drawProceduralTxtCover(renderer, covX, covY, PREVIEW_W, PREVIEW_H, resolvedTitle_,
                                                  resolvedAuthor_, extTag);
      renderer.drawRect(covX - 1, covY - 1, PREVIEW_W + 2, PREVIEW_H + 2, 2, true);
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
        const std::string line = shibadesk::clampSingleLine(renderer, UI_6_FONT_ID, notes[n], L.rightPaneW - 20);
        renderer.drawText(UI_6_FONT_ID, midRightX - renderer.getTextWidth(UI_6_FONT_ID, line.c_str()) / 2,
                          notesY + n * 18, line.c_str(), true);
      }
    }

    renderer.fillRoundedRect(L.ctaRect.x, L.ctaRect.y, L.ctaRect.width, L.ctaRect.height, 8, Color::Black);
    renderer.drawRoundedRect(L.ctaRect.x + 3, L.ctaRect.y + 3, L.ctaRect.width - 6, L.ctaRect.height - 6, 1, 6, false);
    const char* rawCta = tr(STR_TXT_COVER_BIND_CTA);
    const EpdFontFamily::Style ctaStyle = shibadesk::styleForText(rawCta, true);
    const std::string ctaLabel =
        shibadesk::clampSingleLine(renderer, SMALL_FONT_ID, rawCta, L.ctaRect.width - 14, ctaStyle);
    renderer.drawText(
        SMALL_FONT_ID,
        L.ctaRect.x + (L.ctaRect.width - renderer.getTextWidth(SMALL_FONT_ID, ctaLabel.c_str(), ctaStyle)) / 2,
        L.ctaRect.y + std::max(2, (L.ctaRect.height - renderer.getTextHeight(SMALL_FONT_ID)) / 2) - 1, ctaLabel.c_str(),
        false, ctaStyle);

    if (isShiba) {
      shibadesk::drawShibaHardwareKeyHints(renderer, mappedInput, tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT),
                                           tr(STR_DIR_RIGHT));
    } else if (!mappedInput.hasTouch()) {
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
