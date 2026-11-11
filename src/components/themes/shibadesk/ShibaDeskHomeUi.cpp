#include "ShibaDeskHomeUi.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFont.h>
#include <Txt.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>

#include "HapticFeedback.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/library/TxtCoverPickerActivity.h"
#include "activities/plugins/LibbyActivity.h"
#include "activities/plugins/TelegramSyncActivity.h"
#include "activities/reader/TxtEncodingActivity.h"
#include "components/ProceduralBookCover.h"
#include "components/UITheme.h"
#include "components/themes/shibadesk/ShibaCoverCache.h"
#include "components/themes/shibadesk/ShibaDeskChrome.h"
#include "components/themes/shibadesk/ShibaStatusBar.h"
#include "fontIds.h"
#include "network/BookSyncService.h"
#include "util/BookProgress.h"

namespace {

constexpr int HERO_COVER_W = 124;
constexpr int HERO_COVER_H = 186;
constexpr int SHELF_CARD_GAP = 13;
constexpr int SHELF_LEDGE_Y = 622;
constexpr unsigned long BOOK_OPTIONS_HOLD_MS = 450;
constexpr int HERO_MING_FONT_ID = 0x53484D47;  // "SHMG"
constexpr char DICT_HEADWORD_FONT_PATH[] = "/zh/headword.cpfont";

bool rectContains(const Rect& r, const int x, const int y) {
  return r.width > 0 && r.height > 0 && x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

}  // namespace

ShibaDeskHomeUi::ShibaDeskHomeUi(Activity& hostActivity, GfxRenderer& renderer, MappedInputManager& mappedInput)
    : host_(hostActivity), renderer_(renderer), mappedInput_(mappedInput) {}

ShibaDeskHomeUi::~ShibaDeskHomeUi() { unloadHeroMingFont(); }

void ShibaDeskHomeUi::ensureHeroMingFontLoaded() {
  if (heroMingFontId_ != 0) return;

  // Prefer a native 12pt (24px) Ming/Song face in /.fonts if installed (e.g. CMEX24_12.cpfont).
  static constexpr const char* kMingFamilies[] = {"CMEX24", "SourceHanSerifSC", "SourceHanSerif", "NotoSerifCJK",
                                                  "FZShuSong"};
  for (const char* famName : kMingFamilies) {
    const auto* fam = sdFontSystem.registry().findFamily(famName);
    if (!fam || fam->vector) continue;
    const auto* file = fam->findFile(12);
    if (!file) continue;
    char path[SdCardFontFamilyInfo::PATH_BUFFER_SIZE];
    if (!fam->filePath(*file, path, sizeof(path))) continue;
    auto font = makeUniqueNoThrow<SdCardFont>();
    if (!font || !font->load(path)) continue;
    if (renderer_.getFontMap().count(HERO_MING_FONT_ID)) renderer_.removeFont(HERO_MING_FONT_ID);
    renderer_.registerSdCardFont(HERO_MING_FONT_ID, font.get());
    renderer_.insertFont(HERO_MING_FONT_ID, EpdFontFamily(0, font->getEpdFont(0), font->getEpdFont(1),
                                                          font->getEpdFont(2), font->getEpdFont(3)));
    renderer_.setFallbackFont(HERO_MING_FONT_ID, UI_10_FONT_ID);
    heroMingFile_ = std::move(font);
    heroMingFontId_ = HERO_MING_FONT_ID;
    heroMingScale256_ = 256;
    return;
  }

  // Otherwise reuse /zh/headword.cpfont (48px 2x CMEX Ming 24) scaled at 0.5x = 24px!
  if (!Storage.exists(DICT_HEADWORD_FONT_PATH)) return;
  auto font = makeUniqueNoThrow<SdCardFont>();
  if (!font || !font->load(DICT_HEADWORD_FONT_PATH)) return;
  if (renderer_.getFontMap().count(HERO_MING_FONT_ID)) renderer_.removeFont(HERO_MING_FONT_ID);
  renderer_.registerSdCardFont(HERO_MING_FONT_ID, font.get());
  renderer_.insertFont(HERO_MING_FONT_ID, EpdFontFamily(0, font->getEpdFont(0), font->getEpdFont(1),
                                                        font->getEpdFont(2), font->getEpdFont(3)));
  heroMingFile_ = std::move(font);
  heroMingFontId_ = HERO_MING_FONT_ID;
  heroMingScale256_ = 128;
}

void ShibaDeskHomeUi::unloadHeroMingFont() {
  if (heroMingFontId_ != 0) {
    renderer_.removeFont(heroMingFontId_);
    heroMingFontId_ = 0;
  }
  heroMingFile_.reset();
  heroMingScale256_ = 256;
}

int ShibaDeskHomeUi::measureHeroMingWidth(const char* text) const {
  if (!text || *text == '\0' || heroMingFontId_ == 0) return 0;
  if (heroMingScale256_ == 256) {
    renderer_.prewarmFallbackText(heroMingFontId_, text, EpdFontFamily::REGULAR);
    return renderer_.getTextWidth(heroMingFontId_, text, EpdFontFamily::REGULAR);
  }
  int totalW = 0;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  while (*p) {
    const uint32_t cp = utf8NextCodepoint(&p);
    int32_t advFP = 0;
    int top = 0;
    if (renderer_.getCodepointMetrics(heroMingFontId_, cp, EpdFontFamily::REGULAR, advFP, top)) {
      totalW += ((advFP >> 4) * heroMingScale256_ + 128) >> 8;
    } else {
      char u8[5] = {};
      utf8AppendCodepoint(u8, cp);
      totalW += renderer_.getTextWidth(UI_10_FONT_ID, u8, EpdFontFamily::BOLD);
    }
  }
  return totalW;
}

void ShibaDeskHomeUi::drawHeroMingLine(const int x, const int y, const char* text) const {
  if (!text || *text == '\0' || heroMingFontId_ == 0) return;
  if (heroMingScale256_ == 256) {
    renderer_.prewarmFallbackText(heroMingFontId_, text, EpdFontFamily::REGULAR);
    renderer_.drawText(heroMingFontId_, x, y, text, true, EpdFontFamily::REGULAR);
    return;
  }
  int curX = x;
  const int baselineY = y + 21;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  while (*p) {
    const uint32_t cp = utf8NextCodepoint(&p);
    int32_t advFP = 0;
    int top = 0;
    if (renderer_.getCodepointMetrics(heroMingFontId_, cp, EpdFontFamily::REGULAR, advFP, top)) {
      curX += renderer_.drawScaledCodepoint(heroMingFontId_, cp, EpdFontFamily::REGULAR, curX, baselineY,
                                            heroMingScale256_);
    } else {
      char u8[5] = {};
      utf8AppendCodepoint(u8, cp);
      renderer_.drawText(UI_10_FONT_ID, curX, y + 2, u8, true, EpdFontFamily::BOLD);
      curX += renderer_.getTextWidth(UI_10_FONT_ID, u8, EpdFontFamily::BOLD);
    }
  }
}

void ShibaDeskHomeUi::begin(std::vector<RecentBook>& books, const bool hasPlugins, const bool hasOpdsServers,
                            std::function<void()> onReloadRequested) {
  hasPlugins_ = hasPlugins;
  hasOpdsServers_ = hasOpdsServers;
  onReloadRequested_ = std::move(onReloadRequested);
  coversReady_ = false;
  invalidateCoverSnapshots();

  libby::loadCachedLoans(cachedLoans_);

  totalLibraryBooks_ = static_cast<int>(books.size());
  if (auto idx = makeUniqueNoThrow<library::LibraryIndexFile>()) {
    if (idx->open(library::libraryIndexPath())) {
      totalLibraryBooks_ = std::max<int>(totalLibraryBooks_, idx->bookCount());
      idx->close();
    }
  }

  buildSlotsFromBooks(books);
  pose_ = slots_.empty() ? shibadesk::ShibaPose::LoafSleep : shibadesk::advancePoseCycle();
}

void ShibaDeskHomeUi::buildSlotsFromBooks(const std::vector<RecentBook>& books) {
  slots_.clear();
  const size_t count = std::min<size_t>(books.size(), MAX_BOOKS);
  slots_.reserve(count);

  const Rect content = UITheme::getInstance().getContentArea(renderer_);
  const int leftX = content.x + 20;
  const int rightX = content.x + content.width - 20;
  const int totalW = std::max(240, rightX - leftX);
  const int heroTextMaxW = std::max(130, rightX - (leftX + 16 + HERO_COVER_W + 20) - 16);
  const int shelfCardW = std::max(108, (totalW - SHELF_CARD_GAP * 2) / 3);
  const int shelfCoverW = std::max(84, shelfCardW - 22);
  const int shelfTextMaxW = std::max(72, shelfCardW - 18);

  if (count > 0 && shibadesk::hasCjkCodepoints(books[0].title.c_str())) {
    ensureHeroMingFontLoaded();
  }

  for (size_t i = 0; i < count; ++i) {
    BookSlot slot;
    slot.path = books[i].path;
    slot.isTxtOrMd = Txt::isTxtOrMd(slot.path);
    slot.extTag = shibadesk::detectBookFormatTag(slot.path);

    if (slot.isTxtOrMd) {
      Txt::resolveTitleAndAuthor(slot.path, slot.title, slot.author);
    } else {
      slot.title = books[i].title.empty() ? FsHelpers::getFileNameWithoutExtension(slot.path) : books[i].title;
      slot.author = books[i].author;
    }

    slot.progressPct = loadBookProgress(slot.path);
    slot.loanBadge = loanbadge::resolveForBook(slot.path, cachedLoans_);

    if (i == 0) {
      slot.targetCoverW = HERO_COVER_W;
      slot.targetCoverH = HERO_COVER_H;
      const bool isCjk = shibadesk::hasCjkCodepoints(slot.title.c_str());
      if (isCjk) {
        ensureHeroMingFontLoaded();
        if (heroMingFontId_ != 0 && measureHeroMingWidth(slot.title.c_str()) <= heroTextMaxW) {
          slot.useMingHero24 = true;
          slot.titleFontId = heroMingFontId_;
          slot.wrappedTitle = {slot.title};
        } else {
          slot.titleFontId = SMALL_FONT_ID;
          slot.wrappedTitle = shibadesk::wrapSmartLines(renderer_, SMALL_FONT_ID, slot.title.c_str(), heroTextMaxW, 2);
        }
      } else {
        slot.titleFontId = UI_10_FONT_ID;
        slot.wrappedTitle = shibadesk::wrapSmartLines(renderer_, UI_10_FONT_ID, slot.title.c_str(), heroTextMaxW, 2,
                                                      EpdFontFamily::BOLD);
      }
    } else {
      slot.titleFontId = UI_6_FONT_ID;
      slot.wrappedTitle = shibadesk::wrapSmartLines(renderer_, UI_6_FONT_ID, slot.title.c_str(), shelfTextMaxW, 2);
      slot.targetCoverW = shelfCoverW;
      slot.targetCoverH = (slot.wrappedTitle.size() <= 1) ? 164 : 150;
    }

    slot.exactThumbBmpPath = shibadesk::lookupExistingThumbBmp(slot.path, slot.targetCoverW, slot.targetCoverH);

    slots_.push_back(std::move(slot));
  }
}

void ShibaDeskHomeUi::invalidateCoverSnapshots() {
  for (auto& snap : snapshots_) {
    snap.bytes.reset();
    snap.byteLen = 0;
    snap.valid = false;
  }
}

void ShibaDeskHomeUi::releaseTransientCaches() {
  invalidateCoverSnapshots();
  unloadHeroMingFont();
  cachedLoans_.clear();
  cachedLoans_.shrink_to_fit();
}

bool ShibaDeskHomeUi::ensureCoversGenerated() {
  if (coversReady_) return false;
  coversReady_ = true;

  bool anyGenerated = false;
  bool showingLoading = false;
  Rect popupRect;

  for (size_t i = 0; i < slots_.size(); ++i) {
    auto& slot = slots_[i];
    if (!slot.exactThumbBmpPath.empty() && Storage.exists(slot.exactThumbBmpPath.c_str())) {
      continue;
    }
    if (slot.isTxtOrMd && Txt::findCompanionCoverImage(slot.path).empty()) {
      continue;
    }

    const std::string generated = shibadesk::generateThumbBmp(
        slot.path, slot.targetCoverW, slot.targetCoverH,
        [&]() {
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer_, tr(STR_LOADING_POPUP));
          }
          if (FsHelpers::hasEpubExtension(slot.path)) {
            GUI.fillPopupProgress(renderer_, popupRect,
                                  static_cast<int>((i + 1) * 90 / std::max<size_t>(1, slots_.size())));
          }
        },
        nullptr);

    if (!generated.empty()) {
      slot.exactThumbBmpPath = generated;
      if (slot.progressPct < 0) {
        slot.progressPct = loadBookProgress(slot.path);
      }
      snapshots_[i].valid = false;
      anyGenerated = true;
    }
  }

  return anyGenerated || showingLoading;
}

void ShibaDeskHomeUi::paintOrRestoreCover(const size_t slotIdx, const int x, const int y, const int w, const int h) {
  if (slotIdx >= slots_.size() || slotIdx >= snapshots_.size()) return;
  auto& snap = snapshots_[slotIdx];
  const bool curInverted = display.isInverted();

  if (snap.valid && snap.inverted == curInverted && snap.rect.x == x && snap.rect.y == y && snap.rect.width == w &&
      snap.rect.height == h && snap.bytes) {
    if (renderer_.copyBufferToRegion(x, y, w, h, snap.bytes.get(), snap.byteLen)) {
      renderer_.drawRect(x - 1, y - 1, w + 2, h + 2, 2, true);
      return;
    }
  }

  const auto& slot = slots_[slotIdx];
  if (!shibadesk::drawThumbBmp(renderer_, slot.exactThumbBmpPath, x, y, w, h)) {
    ProceduralBookCover::drawProceduralTxtCover(renderer_, x, y, w, h, slot.title, slot.author, slot.extTag.c_str());
  }
  renderer_.drawRect(x - 1, y - 1, w + 2, h + 2, 2, true);

  // Snapshot the rendered 1-bit cover tile in DRAM so D-pad cursor movement never touches SD again
  const size_t needed = renderer_.getRegionByteSize(x, y, w, h);
  if (needed > 0) {
    auto buf = makeUniqueNoThrow<uint8_t[]>(needed);
    if (buf && renderer_.copyRegionToBuffer(x, y, w, h, buf.get(), needed)) {
      snap.rect = Rect{x, y, w, h};
      snap.bytes = std::move(buf);
      snap.byteLen = needed;
      snap.inverted = curInverted;
      snap.valid = true;
    }
  }
}

void ShibaDeskHomeUi::drawEmptyStudyDesk(const Rect& content) {
  const int leftX = content.x + 20;
  const int rightX = content.x + content.width - 20;
  const int totalW = rightX - leftX;
  const int ledgeY = 128;
  const int cardH = 468;

  shibadesk::drawShibaCard(renderer_, Rect{leftX, ledgeY, totalW, cardH}, false, 5);
  shibadesk::stampShibaOnLedge(renderer_, leftX + 18, ledgeY, shibadesk::ShibaPose::LoafSleep);
  shibadesk::stampMugOnLedge(renderer_, leftX + 168, ledgeY);

  const int midX = leftX + totalW / 2;
  shibadesk::drawIcon(renderer_, shibadesk::ShibaIcon::Book, midX - 8, ledgeY + 116, true);

  const char* rawTitle = tr(STR_SHIBA_EMPTY_DESK_TITLE);
  const int hFont = shibadesk::hasCjkCodepoints(rawTitle) ? SMALL_FONT_ID : UI_10_FONT_ID;
  const EpdFontFamily::Style hStyle = shibadesk::styleForText(rawTitle, true);
  const std::string hTitle = shibadesk::clampSingleLine(renderer_, hFont, rawTitle, totalW - 48, hStyle);
  renderer_.drawText(hFont, midX - renderer_.getTextWidth(hFont, hTitle.c_str(), hStyle) / 2, ledgeY + 150,
                     hTitle.c_str(), true, hStyle);

  const auto lines1 =
      shibadesk::wrapSmartLines(renderer_, SMALL_FONT_ID, tr(STR_SHIBA_EMPTY_DESK_BODY_1), totalW - 56, 2);
  int ty = ledgeY + 190;
  for (const auto& ln : lines1) {
    renderer_.drawText(SMALL_FONT_ID, midX - renderer_.getTextWidth(SMALL_FONT_ID, ln.c_str()) / 2, ty, ln.c_str(),
                       true);
    ty += 22;
  }

  const auto lines2 =
      shibadesk::wrapSmartLines(renderer_, UI_6_FONT_ID, tr(STR_SHIBA_EMPTY_DESK_BODY_2), totalW - 56, 3);
  ty += 12;
  for (const auto& ln : lines2) {
    renderer_.drawText(UI_6_FONT_ID, midX - renderer_.getTextWidth(UI_6_FONT_ID, ln.c_str()) / 2, ty, ln.c_str(), true);
    ty += 20;
  }

  shibadesk::drawWoodgrainShelfLedge(renderer_, content.x + 14, 622, content.width - 28, 13);
}

void ShibaDeskHomeUi::drawHeroCard(const Rect& content, const int selectorIndex) {
  const int leftX = content.x + 20;
  const int rightX = content.x + content.width - 20;
  const int totalW = rightX - leftX;
  const int ledgeY = 104;
  constexpr int heroH = 222;

  // Quick Sync status pill above right corner of Hero Card (y = 54..86)
  const char* syncText = tr(STR_DESK_DOCK_SYNC);
  renderer_.prewarmFallbackText(SMALL_FONT_ID, syncText, EpdFontFamily::REGULAR);
  const int syncTw = renderer_.getTextWidth(SMALL_FONT_ID, syncText, EpdFontFamily::REGULAR);
  const int syncW = std::max(108, syncTw + 48);
  const int sx0 = rightX - syncW;
  topSyncPillRect_ = Rect{sx0, 54, syncW, 32};
  shibadesk::drawShibaCard(renderer_, topSyncPillRect_, false, 3);
  const int syncContentW = 20 + 6 + syncTw;
  const int syncStartX = sx0 + (syncW - syncContentW) / 2;
  shibadesk::drawIcon(renderer_, shibadesk::ShibaIcon::DockSync, syncStartX, 61, true);
  const int syncTh = renderer_.getTextHeight(SMALL_FONT_ID);
  renderer_.drawText(SMALL_FONT_ID, syncStartX + 26, 54 + std::max(2, (32 - syncTh) / 2) - 1, syncText, true,
                     EpdFontFamily::REGULAR);

  const bool heroFocused = (selectorIndex == 0);
  const Rect heroRect{leftX, ledgeY, totalW, heroH};
  cardHitRects_[0] = heroRect;
  shibadesk::drawShibaCard(renderer_, heroRect, heroFocused, 5);
  shibadesk::stampShibaOnLedge(renderer_, leftX + 4, ledgeY, pose_);
  shibadesk::stampMugOnLedge(renderer_, leftX + 152, ledgeY);

  const auto& hero = slots_[0];
  const int covX = leftX + 16;
  const int covY = ledgeY + 18;
  paintOrRestoreCover(0, covX, covY, HERO_COVER_W, HERO_COVER_H);

  const int rx = covX + HERO_COVER_W + 20;
  const int maxTextW = std::max(120, rightX - rx - 16);

  // Top badge pill: either urgent Libby loan due pill or "» Continue Reading"
  const bool showLoanOnHero = hero.loanBadge.isLoan;
  const char* pillLabel = showLoanOnHero ? hero.loanBadge.label.c_str() : tr(STR_SHIBA_CONTINUE_READING);
  shibadesk::drawShibaBadgePill(renderer_, rx, ledgeY + 18, 20, UI_6_FONT_ID, pillLabel, !hero.loanBadge.expired,
                                hero.loanBadge.urgent);

  int authorY = ledgeY + 86;
  if (hero.useMingHero24) {
    const_cast<ShibaDeskHomeUi*>(this)->ensureHeroMingFontLoaded();
    if (heroMingFontId_ != 0) {
      drawHeroMingLine(rx, ledgeY + 48, hero.title.c_str());
      authorY = ledgeY + 84;
    } else {
      renderer_.drawText(SMALL_FONT_ID, rx, ledgeY + 50, hero.title.c_str(), true, EpdFontFamily::REGULAR);
      authorY = ledgeY + 82;
    }
  } else {
    const int tFont = hero.titleFontId != 0 ? hero.titleFontId : SMALL_FONT_ID;
    const EpdFontFamily::Style tStyle = shibadesk::styleForText(hero.title.c_str(), tFont == UI_10_FONT_ID);
    if (hero.wrappedTitle.size() <= 1) {
      const char* t0 = hero.wrappedTitle.empty() ? hero.title.c_str() : hero.wrappedTitle[0].c_str();
      renderer_.drawText(tFont, rx, ledgeY + 50, t0, true, tStyle);
      authorY = ledgeY + 82;
    } else {
      const int lineStep = (tFont == UI_10_FONT_ID) ? 24 : 22;
      renderer_.drawText(tFont, rx, ledgeY + 45, hero.wrappedTitle[0].c_str(), true, tStyle);
      renderer_.drawText(tFont, rx, ledgeY + 45 + lineStep, hero.wrappedTitle[1].c_str(), true, tStyle);
      authorY = ledgeY + 45 + lineStep * 2 + 4;
    }
  }

  if (!hero.author.empty()) {
    const std::string clampedAuthor =
        shibadesk::clampSingleLine(renderer_, SMALL_FONT_ID, hero.author.c_str(), maxTextW);
    renderer_.drawText(SMALL_FONT_ID, rx, authorY, clampedAuthor.c_str(), true);
  }

  // Reading progress readout (left) + format tag (right) above segmented 1-bit progress bar
  const int pct = std::max(0, hero.progressPct);
  char pctBuf[48];
  if (hero.progressPct < 0) {
    snprintf(pctBuf, sizeof(pctBuf), "%s", tr(STR_SHIBA_BOOK_NEW));
  } else if (hero.progressPct >= 100) {
    snprintf(pctBuf, sizeof(pctBuf), "%s", tr(STR_SHIBA_BOOK_FINISHED));
  } else {
    snprintf(pctBuf, sizeof(pctBuf), "%d%%", pct);
  }
  const int pctFont = shibadesk::hasCjkCodepoints(pctBuf) ? UI_6_FONT_ID : SMALL_FONT_ID;
  renderer_.prewarmFallbackText(pctFont, pctBuf, EpdFontFamily::REGULAR);
  renderer_.drawText(pctFont, rx, ledgeY + 159, pctBuf, true, EpdFontFamily::REGULAR);

  const int fmtW = renderer_.getTextWidth(UI_6_FONT_ID, hero.extTag.c_str());
  renderer_.drawText(UI_6_FONT_ID, rx + maxTextW - fmtW, ledgeY + 161, hero.extTag.c_str(), true);

  shibadesk::drawSegmentedProgressBar(renderer_, Rect{rx, ledgeY + 183, maxTextW, 18}, pct);
}

void ShibaDeskHomeUi::drawRecentShelf(const Rect& content, const int selectorIndex) {
  const int leftX = content.x + 20;
  const int rightX = content.x + content.width - 20;
  const int totalW = rightX - leftX;

  // Shelf section header pills over dotted background (y = 344..366)
  const char* leftHdr = tr(STR_SHIBA_RECENT_SHELF);
  char rightHdr[48];
  snprintf(rightHdr, sizeof(rightHdr), tr(STR_DESK_BOOK_COUNT), totalLibraryBooks_);

  const EpdFontFamily::Style leftStyle = shibadesk::styleForText(leftHdr, true);
  renderer_.prewarmFallbackText(SMALL_FONT_ID, leftHdr, leftStyle);
  renderer_.prewarmFallbackText(UI_6_FONT_ID, rightHdr, EpdFontFamily::REGULAR);
  const int lw = renderer_.getTextWidth(SMALL_FONT_ID, leftHdr, leftStyle) + 16;
  const int rw = renderer_.getTextWidth(UI_6_FONT_ID, rightHdr) + 16;
  renderer_.fillRect(leftX, 344, lw, 22, false);
  renderer_.fillRect(rightX - rw, 344, rw, 22, false);
  renderer_.drawText(SMALL_FONT_ID, leftX + 6, 346, leftHdr, true, leftStyle);
  renderer_.drawText(UI_6_FONT_ID, rightX - rw + 8, 348, rightHdr, true);

  const int cardW = std::max(108, (totalW - SHELF_CARD_GAP * 2) / 3);
  constexpr int shelfTopY = 372;
  constexpr int cardH = 246;
  const size_t shelfCount = (slots_.size() > 1) ? std::min<size_t>(3, slots_.size() - 1) : 0;

  for (size_t idx = 0; idx < shelfCount; ++idx) {
    const size_t slotIdx = idx + 1;
    const auto& sb = slots_[slotIdx];
    const int cx0 = leftX + (static_cast<int>(idx) * (totalW - cardW)) / 2;
    const Rect cardRect{cx0, shelfTopY, cardW, cardH};
    cardHitRects_[slotIdx] = cardRect;

    const bool focused = (selectorIndex == static_cast<int>(slotIdx));
    shibadesk::drawShibaCard(renderer_, cardRect, focused, 4);

    const int scX = cx0 + (cardW - sb.targetCoverW) / 2;
    const int scY = shelfTopY + 11;
    paintOrRestoreCover(slotIdx, scX, scY, sb.targetCoverW, sb.targetCoverH);

    // Optional Libby loan countdown tag in top-right corner of cover
    if (sb.loanBadge.isLoan && !sb.loanBadge.label.empty()) {
      const int pillPad = sb.loanBadge.urgent ? 24 : 18;
      const int maxPillW = std::max(28, sb.targetCoverW - 4);
      const std::string clampedLoan = shibadesk::clampSingleLine(renderer_, UI_6_FONT_ID, sb.loanBadge.label.c_str(),
                                                                 std::max(10, maxPillW - pillPad));
      const int pillW = shibadesk::measurePillWidth(renderer_, UI_6_FONT_ID, clampedLoan.c_str(), pillPad);
      const int px = scX + sb.targetCoverW - pillW;
      renderer_.fillRect(px - 1, scY - 1, pillW + 2, 20, false);
      shibadesk::drawShibaBadgePill(renderer_, px, scY, 18, UI_6_FONT_ID, clampedLoan.c_str(), !sb.loanBadge.expired,
                                    sb.loanBadge.urgent);
    }

    const int centerCardX = cx0 + cardW / 2;
    if (sb.wrappedTitle.size() <= 1) {
      const char* t0 = sb.wrappedTitle.empty() ? sb.title.c_str() : sb.wrappedTitle[0].c_str();
      const int tw = renderer_.getTextWidth(UI_6_FONT_ID, t0);
      renderer_.drawText(UI_6_FONT_ID, centerCardX - tw / 2, shelfTopY + 184, t0, true);
    } else {
      const int tw0 = renderer_.getTextWidth(UI_6_FONT_ID, sb.wrappedTitle[0].c_str());
      const int tw1 = renderer_.getTextWidth(UI_6_FONT_ID, sb.wrappedTitle[1].c_str());
      renderer_.drawText(UI_6_FONT_ID, centerCardX - tw0 / 2, shelfTopY + 170, sb.wrappedTitle[0].c_str(), true);
      renderer_.drawText(UI_6_FONT_ID, centerCardX - tw1 / 2, shelfTopY + 188, sb.wrappedTitle[1].c_str(), true);
    }

    char statusRight[24];
    if (sb.progressPct < 0) {
      snprintf(statusRight, sizeof(statusRight), "%s", tr(STR_SHIBA_BOOK_NEW));
    } else if (sb.progressPct >= 100) {
      snprintf(statusRight, sizeof(statusRight), "%s", tr(STR_SHIBA_BOOK_FINISHED));
    } else {
      snprintf(statusRight, sizeof(statusRight), "%d%%", sb.progressPct);
    }
    renderer_.prewarmFallbackText(UI_6_FONT_ID, statusRight, EpdFontFamily::REGULAR);
    const int stW = renderer_.getTextWidth(UI_6_FONT_ID, statusRight);
    renderer_.drawText(UI_6_FONT_ID, cx0 + cardW - 11 - stW, shelfTopY + 215, statusRight, true);

    const int barW = std::max(28, cardW - 22 - stW - 7);
    shibadesk::drawSegmentedProgressBar(renderer_, Rect{cx0 + 11, shelfTopY + 218, barW, 11},
                                        std::max(0, sb.progressPct));
  }

  for (size_t idx = shelfCount + 1; idx < MAX_BOOKS; ++idx) {
    cardHitRects_[idx] = Rect{0, 0, 0, 0};
  }

  if (shelfCount < 3) {
    shibadesk::drawSparseShelfBookendAndSlot(renderer_, static_cast<int>(shelfCount), leftX, cardW + SHELF_CARD_GAP,
                                             SHELF_LEDGE_Y, rightX);
  }

  shibadesk::drawWoodgrainShelfLedge(renderer_, content.x + 14, SHELF_LEDGE_Y, content.width - 28, 13);
}

void ShibaDeskHomeUi::render(const int selectorIndex, const bool cleanInitialRefresh, const bool firstRenderDone) {
  const Rect content = UITheme::getInstance().getContentArea(renderer_);

  renderer_.clearScreen();
  shibadesk::drawStaggeredDotBackground(renderer_,
                                        Rect{0, 36, renderer_.getScreenWidth(), renderer_.getScreenHeight() - 36});

  char countSub[48];
  snprintf(countSub, sizeof(countSub), tr(STR_DESK_BOOK_COUNT), totalLibraryBooks_);
  shibadesk::drawShibaStatusBar(renderer_, tr(STR_DESK_PILL), countSub);

  if (slots_.empty()) {
    topSyncPillRect_ = Rect{0, 0, 0, 0};
    for (auto& r : cardHitRects_) r = Rect{0, 0, 0, 0};
    drawEmptyStudyDesk(content);
  } else {
    drawHeroCard(content, selectorIndex);
    drawRecentShelf(content, selectorIndex);
  }

  const int dockFocus =
      (selectorIndex >= static_cast<int>(slots_.size())) ? (selectorIndex - static_cast<int>(slots_.size())) : -1;
  const char* const dockLabels[DOCK_SLOTS] = {
      tr(STR_LIBRARY),
      tr(STR_DESK_DOCK_FILES),
      tr(STR_DESK_DOCK_SYNC),
      tr(STR_SETTINGS_TITLE),
  };
  shibadesk::drawShibaHomeDock(renderer_, dockFocus, dockLabels, dockHitRects_.data());

  const std::string backHint = slots_.empty() ? std::string() : (std::string("\xC2\xAB ") + tr(STR_RESUME));
  const std::string prevHint = std::string("< ") + tr(STR_DIR_LEFT);
  const std::string nextHint = std::string(tr(STR_DIR_RIGHT)) + " >";
  shibadesk::drawShibaHardwareKeyHints(renderer_, mappedInput_, backHint.c_str(), tr(STR_SELECT), prevHint.c_str(),
                                       nextHint.c_str());

  if (popup_.isActive()) {
    popup_.render(renderer_);
  }

  renderer_.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH
                                                                  : HalDisplay::FAST_REFRESH);
}

void ShibaDeskHomeUi::openBookOptionsPopup(const size_t slotIdx) {
  if (slotIdx >= slots_.size()) return;
  const BookSlot slot = slots_[slotIdx];

  std::vector<std::string> opts;
  opts.emplace_back(tr(STR_BOOK_ACTION_OPEN));
  if (slot.isTxtOrMd) {
    opts.emplace_back(tr(STR_BOOK_ACTION_BIND_COVER));
    opts.emplace_back(tr(STR_BOOK_ACTION_TEXT_ENCODING));
  }
  const bool inRecents = std::any_of(RECENT_BOOKS.getBooks().begin(), RECENT_BOOKS.getBooks().end(),
                                     [&](const RecentBook& rb) { return rb.path == slot.path; });
  if (inRecents) {
    opts.emplace_back(tr(STR_REMOVE_FROM_RECENTS));
  }

  std::vector<const char*> cstrs;
  cstrs.reserve(opts.size());
  for (const auto& s : opts) cstrs.push_back(s.c_str());

  popup_.show(tr(STR_LIBRARY), slot.title.c_str(), cstrs.data(), static_cast<int>(cstrs.size()), 0,
              [this, slot, inRecents](const int choice) {
                if (choice == 0) {
                  activityManager.goToReader(slot.path);
                  return;
                }
                int nextIdx = 1;
                if (slot.isTxtOrMd) {
                  if (choice == nextIdx++) {
                    auto picker = makeUniqueNoThrow<TxtCoverPickerActivity>(renderer_, mappedInput_, slot.path);
                    if (!picker) return;
                    releaseTransientCaches();
                    host_.startActivityForResult(std::move(picker), [this](const ActivityResult& res) {
                      if (!res.isCancelled && onReloadRequested_) onReloadRequested_();
                    });
                    return;
                  }
                  if (choice == nextIdx++) {
                    auto enc = makeUniqueNoThrow<TxtEncodingActivity>(renderer_, mappedInput_, slot.path,
                                                                      Txt::getBookCachePath(slot.path), 0);
                    if (!enc) return;
                    releaseTransientCaches();
                    host_.startActivityForResult(std::move(enc), [this](const ActivityResult&) {
                      if (onReloadRequested_) onReloadRequested_();
                    });
                    return;
                  }
                }
                if (inRecents && choice == nextIdx) {
                  if (RECENT_BOOKS.removeByPath(slot.path)) {
                    RECENT_BOOKS.saveToFile();
                  }
                  if (onReloadRequested_) onReloadRequested_();
                }
              });
  host_.requestUpdate();
}

void ShibaDeskHomeUi::runSyncAllInteractive() {
  releaseTransientCaches();
  booksync::SyncAllSummary summary;
  const bool ok = booksync::syncAll(&renderer_, summary, [this](const char* msg) {
    RenderLock lock(host_);
    GUI.drawPopup(renderer_, msg);
  });

  if (onReloadRequested_) {
    onReloadRequested_();
  }

  const char* okOpts[1] = {tr(STR_OK_BUTTON)};
  if (!ok) {
    popup_.showMessage(tr(STR_SYNC_MENU_TITLE), summary.errorMessage.c_str(), okOpts, 1, 0, nullptr);
  } else {
    char doneMsg[128];
    std::snprintf(doneMsg, sizeof(doneMsg), tr(STR_SYNC_DONE_SUMMARY), summary.telegramBooks, summary.telegramCovers,
                  summary.libbyLoansFulfilled);
    popup_.showMessage(tr(STR_SYNC_MENU_TITLE), doneMsg, okOpts, 1, 0, nullptr);
  }
  host_.requestUpdate();
}

void ShibaDeskHomeUi::openSyncMenuPopup() {
  std::vector<std::string> opts;
  opts.emplace_back(tr(STR_SYNC_ALL_NOW));
  opts.emplace_back(tr(STR_SYNC_WIFI_TRANSFER));
  opts.emplace_back(tr(STR_TELEGRAM_SYNC));
  opts.emplace_back(tr(STR_LIBBY_LIBRARY));
  const bool showCatalogSlot = hasPlugins_ || hasOpdsServers_;
  if (showCatalogSlot) {
    opts.emplace_back(hasPlugins_ ? tr(STR_PLUGINS) : tr(STR_OPDS_BROWSER));
  }

  std::vector<const char*> cstrs;
  cstrs.reserve(opts.size());
  for (const auto& s : opts) cstrs.push_back(s.c_str());

  const int defaultIdx = booksync::hasConfiguredCloudSync() ? 0 : 1;
  popup_.show(tr(STR_SYNC_MENU_TITLE), cstrs.data(), static_cast<int>(cstrs.size()), defaultIdx,
              [this, showCatalogSlot](const int choice) {
                if (choice == 0) {
                  runSyncAllInteractive();
                } else if (choice == 1) {
                  activityManager.goToFileTransfer();
                } else if (choice == 2) {
                  auto tg = makeUniqueNoThrow<TelegramSyncActivity>(renderer_, mappedInput_);
                  if (!tg) return;
                  releaseTransientCaches();
                  host_.startActivityForResult(std::move(tg), [this](const ActivityResult&) {
                    if (onReloadRequested_) onReloadRequested_();
                  });
                } else if (choice == 3) {
                  auto lby = makeUniqueNoThrow<LibbyActivity>(renderer_, mappedInput_);
                  if (!lby) return;
                  releaseTransientCaches();
                  host_.startActivityForResult(std::move(lby), [this](const ActivityResult&) {
                    if (onReloadRequested_) onReloadRequested_();
                  });
                } else if (showCatalogSlot && choice == 4) {
                  if (hasPlugins_) {
                    activityManager.goToPlugins(hasOpdsServers_);
                  } else {
                    activityManager.goToBrowser();
                  }
                }
              });
  host_.requestUpdate();
}

void ShibaDeskHomeUi::activateIndex(const int index) {
  const int bookCount = static_cast<int>(slots_.size());
  if (index >= 0 && index < bookCount) {
    activityManager.goToReader(slots_[index].path);
    return;
  }

  const int dockIdx = index - bookCount;
  switch (dockIdx) {
    case 0:
      activityManager.goToLibrary();
      break;
    case 1:
      activityManager.goToFileBrowser();
      break;
    case 2:
      openSyncMenuPopup();
      break;
    case 3:
      activityManager.goToSettings();
      break;
    default:
      break;
  }
}

bool ShibaDeskHomeUi::handleInput(int& selectorIndex, ButtonNavigator& buttonNavigator) {
  if (popup_.handleInput(mappedInput_, [this] { host_.requestUpdate(); })) {
    return true;
  }

  const int totalItems = menuItemCount();
  const int bookCount = static_cast<int>(slots_.size());
  selectorIndex = std::clamp(selectorIndex, 0, std::max(0, totalItems - 1));

  // Long-press Confirm on a focused book opens TXT Cover / Encoding / Recents options;
  // on the Sync dock button, jumps directly to Wi-Fi File Transfer.
  if (mappedInput_.wasLongPressed(MappedInputManager::Button::Confirm, BOOK_OPTIONS_HOLD_MS)) {
    haptic_feedback::touchAction();
    if (selectorIndex >= 0 && selectorIndex < bookCount) {
      openBookOptionsPopup(static_cast<size_t>(selectorIndex));
      return true;
    }
    if (selectorIndex == bookCount + 2) {
      activityManager.goToFileTransfer();
      return true;
    }
  }

  // Touch long-press on any book card
  int lx = -1, ly = -1;
  if (mappedInput_.wasScreenLongPress(lx, ly)) {
    for (int i = 0; i < bookCount; ++i) {
      if (rectContains(cardHitRects_[i], lx, ly)) {
        selectorIndex = i;
        haptic_feedback::touchAction();
        openBookOptionsPopup(static_cast<size_t>(i));
        return true;
      }
    }
  }

  // Touch tap on cards, Top Sync Pill, or Bottom Dock capsules
  int tx = -1, ty = -1;
  if (mappedInput_.wasScreenTapped(tx, ty)) {
    if (rectContains(topSyncPillRect_, tx, ty)) {
      selectorIndex = bookCount + 2;
      haptic_feedback::touchAction();
      openSyncMenuPopup();
      return true;
    }
    for (int i = 0; i < bookCount; ++i) {
      if (rectContains(cardHitRects_[i], tx, ty)) {
        selectorIndex = i;
        haptic_feedback::touchAction();
        activateIndex(i);
        return true;
      }
    }
    for (int d = 0; d < DOCK_SLOTS; ++d) {
      if (rectContains(dockHitRects_[d], tx, ty)) {
        selectorIndex = bookCount + d;
        haptic_feedback::touchAction();
        activateIndex(selectorIndex);
        return true;
      }
    }
  }

  buttonNavigator.onNext([this, &selectorIndex, totalItems] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
    host_.requestUpdate();
  });

  buttonNavigator.onPrevious([this, &selectorIndex, totalItems] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
    host_.requestUpdate();
  });

  const auto swipe = mappedInput_.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Left) {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
    host_.requestUpdate();
    return true;
  }
  if (swipe == MappedInputManager::SwipeDir::Down || swipe == MappedInputManager::SwipeDir::Right) {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
    host_.requestUpdate();
    return true;
  }

  if (mappedInput_.wasReleased(MappedInputManager::Button::Back) && !slots_.empty()) {
    activityManager.goToReader(slots_[0].path);
    return true;
  }

  if (mappedInput_.wasReleased(MappedInputManager::Button::Confirm)) {
    activateIndex(selectorIndex);
    return true;
  }

  return true;
}
