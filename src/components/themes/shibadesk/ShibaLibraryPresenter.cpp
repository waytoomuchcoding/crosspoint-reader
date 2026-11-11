#include "ShibaLibraryPresenter.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Memory.h>
#include <Txt.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "components/ProceduralBookCover.h"
#include "components/UITheme.h"
#include "components/themes/shibadesk/ShibaCoverCache.h"
#include "components/themes/shibadesk/ShibaDeskChrome.h"
#include "components/themes/shibadesk/ShibaStatusBar.h"
#include "fontIds.h"
#include "util/BookProgress.h"

namespace fui = freeink::ui;

namespace shibadesk {

namespace {

void drawSortCaret(const GfxRenderer& renderer, const int cx, const int cy, const bool down, const bool black) {
  if (down) {
    renderer.drawLine(cx - 3, cy - 2, cx + 3, cy - 2, black);
    renderer.drawLine(cx - 2, cy - 1, cx + 2, cy - 1, black);
    renderer.drawLine(cx - 1, cy, cx + 1, cy, black);
    renderer.drawPixel(cx, cy + 1, black);
  } else {
    renderer.drawPixel(cx, cy - 2, black);
    renderer.drawLine(cx - 1, cy - 1, cx + 1, cy - 1, black);
    renderer.drawLine(cx - 2, cy, cx + 2, cy, black);
    renderer.drawLine(cx - 3, cy + 1, cx + 3, cy + 1, black);
  }
}

}  // namespace

ShibaLibraryPresenter::ShibaLibraryPresenter(GfxRenderer& renderer, const MappedInputManager& mappedInput)
    : renderer_(renderer), mappedInput_(mappedInput) {
  windowItems_.reserve(MAX_WINDOW_ITEMS);
}

void ShibaLibraryPresenter::onEnter() {
  loansLoaded_ = false;
  lastPoseTab_ = -1;
  currentPose_ = advancePoseCycle();
  invalidateCaches();
}

void ShibaLibraryPresenter::invalidateCaches() {
  cachedTop_ = -1;
  cachedDrawn_ = 0;
  cachedListCount_ = -1;
  cachedTab_ = -1;
  cachedSortRaw_ = 0xFF;
  cachedCollapsed_ = false;
  cachedQuery_.clear();
  windowItems_.clear();
  for (auto& snap : miniSnapshots_) {
    snap.path.clear();
    snap.bytes.reset();
    snap.byteLen = 0;
    snap.valid = false;
  }
}

bool ShibaLibraryPresenter::matchesCachedWindow(const ShibaLibraryViewContext& ctx, const int top,
                                                const int count) const {
  if (cachedTop_ != top || cachedListCount_ != count || cachedTab_ != ctx.activeTab ||
      cachedSortRaw_ != ctx.sortOrderRaw || cachedCollapsed_ != ctx.groupsCollapsed) {
    return false;
  }
  const std::string q = ctx.query ? *ctx.query : std::string();
  return cachedQuery_ == q && !windowItems_.empty();
}

void ShibaLibraryPresenter::populateBookItemMeta(WindowItem& item, const ShibaLibraryViewContext& ctx) {
  item.isTxtOrMd = Txt::isTxtOrMd(item.path);
  item.extTag = detectBookFormatTag(item.path);
  item.loanBadge = loanbadge::resolveForBook(item.path, cachedLoans_);

  const int pct = loadBookProgress(item.path);
  if (pct >= 0) {
    item.hasProgress = true;
    item.progressPercent = static_cast<uint8_t>(std::clamp(pct, 0, 100));
  } else {
    item.hasProgress = false;
    item.progressPercent = 0;
  }

  // Resolve or lazily generate 52x78 1-bit mini-cover only on Mini-Cover tabs (Recent / Title)
  if (ctx.activeTab == 2) return;

  item.miniThumbPath = generateThumbBmp(
      item.path, MINI_COVER_W, MINI_COVER_H,
      [&]() {
        if (ctx.closeIndexTemporarily) ctx.closeIndexTemporarily(ctx.user);
      },
      [&]() {
        if (ctx.reopenIndex) ctx.reopenIndex(ctx.user);
      });
  item.hasCustomTxtCover = item.isTxtOrMd && !item.miniThumbPath.empty();
}

void ShibaLibraryPresenter::paintOrRestoreMiniCover(const size_t slotIdx, const WindowItem& item, const int x,
                                                    const int y) {
  const bool curInverted = display.isInverted();
  if (slotIdx < miniSnapshots_.size()) {
    auto& snap = miniSnapshots_[slotIdx];
    if (snap.valid && snap.inverted == curInverted && snap.path == item.path && snap.rect.x == x && snap.rect.y == y &&
        snap.rect.width == MINI_COVER_W && snap.rect.height == MINI_COVER_H && snap.bytes) {
      if (renderer_.copyBufferToRegion(x, y, MINI_COVER_W, MINI_COVER_H, snap.bytes.get(), snap.byteLen)) {
        renderer_.drawRect(x - 1, y - 1, MINI_COVER_W + 2, MINI_COVER_H + 2, 1, true);
        return;
      }
    }
  }

  if (!drawThumbBmp(renderer_, item.miniThumbPath, x, y, MINI_COVER_W, MINI_COVER_H)) {
    ProceduralBookCover::drawMiniProceduralTxtCover(renderer_, x, y, MINI_COVER_W, MINI_COVER_H, item.extTag.c_str());
  }
  renderer_.drawRect(x - 1, y - 1, MINI_COVER_W + 2, MINI_COVER_H + 2, 1, true);

  if (slotIdx < miniSnapshots_.size()) {
    auto& snap = miniSnapshots_[slotIdx];
    const size_t needed = renderer_.getRegionByteSize(x, y, MINI_COVER_W, MINI_COVER_H);
    if (needed > 0) {
      auto buf = makeUniqueNoThrow<uint8_t[]>(needed);
      if (buf && renderer_.copyRegionToBuffer(x, y, MINI_COVER_W, MINI_COVER_H, buf.get(), needed)) {
        snap.path = item.path;
        snap.rect = Rect{x, y, MINI_COVER_W, MINI_COVER_H};
        snap.bytes = std::move(buf);
        snap.byteLen = needed;
        snap.inverted = curInverted;
        snap.valid = true;
      }
    }
  }
}

void ShibaLibraryPresenter::drawFilterStrip(UiScreen& screen, const ShibaLibraryViewContext& ctx, const int leftX,
                                            const int rightX, const int ledgeY) {
  const int totalW = rightX - leftX;
  constexpr int stripH = 42;
  drawShibaCard(renderer_, Rect{leftX, ledgeY, totalW, stripH}, ctx.tabsFocused, 3);
  stampShibaOnLedge(renderer_, rightX - SHIBA_SPRITE_W - 6, ledgeY, currentPose_);

  if (ctx.degraded) {
    const char* rawUnsorted = tr(STR_LIBRARY_TITLE_UNSORTED);
    const EpdFontFamily::Style uStyle = styleForText(rawUnsorted, true);
    const std::string label = clampSingleLine(renderer_, SMALL_FONT_ID, rawUnsorted, totalW - 32, uStyle);
    renderer_.drawText(SMALL_FONT_ID, leftX + 14, ledgeY + 12, label.c_str(), true, uStyle);
    return;
  }

  const char* const tabLabels[3] = {
      tr(STR_LIBRARY_TAB_RECENT),
      tr(STR_LIBRARY_TAB_TITLE),
      tr(STR_LIBRARY_TAB_AUTHOR),
  };

  constexpr int pillH = 26;
  const int pillY = ledgeY + (stripH - pillH) / 2;
  int tx = leftX + 10;

  for (int i = 0; i < 3; ++i) {
    const bool active = (i == ctx.activeTab);
    const char* rawLbl = tabLabels[i] ? tabLabels[i] : "";
    const EpdFontFamily::Style style = styleForText(rawLbl, active);
    renderer_.prewarmFallbackText(SMALL_FONT_ID, rawLbl, style);
    const int textW = renderer_.getTextWidth(SMALL_FONT_ID, rawLbl, style);
    const int caretReserve = active ? 12 : 0;
    const int pillW = textW + 18 + caretReserve;

    if (active) {
      renderer_.fillRoundedRect(tx, pillY, pillW, pillH, 6, Color::Black);
      if (ctx.tabsFocused && pillW > 8) {
        renderer_.drawRoundedRect(tx + 2, pillY + 2, pillW - 4, pillH - 4, 1, 4, false);
      }
    } else {
      renderer_.fillRoundedRect(tx, pillY, pillW, pillH, 6, Color::White);
      renderer_.drawRoundedRect(tx, pillY, pillW, pillH, 1, 6, true);
    }

    const int textH = renderer_.getTextHeight(SMALL_FONT_ID);
    const int ty = pillY + std::max(1, (pillH - textH) / 2) - 1;
    renderer_.drawText(SMALL_FONT_ID, tx + 9, ty, rawLbl, !active, style);

    if (active) {
      drawSortCaret(renderer_, tx + pillW - 9, pillY + pillH / 2, ctx.isDescending, false);
    }

    screen.frame().hit(fui::Rect{static_cast<int16_t>(tx), static_cast<int16_t>(ledgeY),
                                 static_cast<int16_t>(pillW + 4), static_cast<int16_t>(stripH)},
                       ctx.actionTab, static_cast<int16_t>(i), fui::InputTouch);
    tx += pillW + 7;
  }

  // Right-aligned Search pill (strictly no 3x2 Bookshelf Grid button)
  const bool searchActive = ctx.query && !ctx.query->empty();
  std::string rawSearch = searchActive ? ("\xE2\x80\x9C" + *ctx.query + "\xE2\x80\x9D") : std::string(tr(STR_SEARCH));
  const EpdFontFamily::Style sStyle = styleForText(rawSearch.c_str(), searchActive);
  const int iconW = getIconWidth(ShibaIcon::Search);
  const int iconH = getIconHeight(ShibaIcon::Search);
  const int maxPillW = std::max(54, (rightX - 10) - (tx + 6));
  const int maxSearchTextW = std::max(18, maxPillW - iconW - 22);
  const std::string clampedSearch =
      clampSingleLine(renderer_, SMALL_FONT_ID, rawSearch.c_str(), maxSearchTextW, sStyle);
  const int sTextW = renderer_.getTextWidth(SMALL_FONT_ID, clampedSearch.c_str(), sStyle);
  const int sPillW = std::min(maxPillW, iconW + 6 + sTextW + 16);
  const int sx1 = rightX - 10;
  const int sx0 = sx1 - sPillW;

  if (sx0 >= tx + 4) {
    if (searchActive) {
      renderer_.fillRoundedRect(sx0, pillY, sPillW, pillH, 6, Color::Black);
    } else {
      renderer_.fillRoundedRect(sx0, pillY, sPillW, pillH, 6, Color::White);
      renderer_.drawRoundedRect(sx0, pillY, sPillW, pillH, 1, 6, true);
    }
    drawIcon(renderer_, ShibaIcon::Search, sx0 + 8, pillY + (pillH - iconH) / 2, !searchActive);
    const int sTextH = renderer_.getTextHeight(SMALL_FONT_ID);
    renderer_.drawText(SMALL_FONT_ID, sx0 + 8 + iconW + 6, pillY + std::max(1, (pillH - sTextH) / 2) - 1,
                       clampedSearch.c_str(), !searchActive, sStyle);
    screen.frame().hit(fui::Rect{static_cast<int16_t>(sx0 - 2), static_cast<int16_t>(ledgeY),
                                 static_cast<int16_t>(sPillW + 8), static_cast<int16_t>(stripH)},
                       ctx.actionSearch, 0, fui::InputTouch);
  }
}

void ShibaLibraryPresenter::drawEmptyState(const ShibaLibraryViewContext& ctx, const int leftX, const int rightX,
                                           const int topY, const int bottomY) const {
  const int totalW = rightX - leftX;
  const int cardH = std::min(320, bottomY - topY - 24);
  const int cardY = topY + std::max(12, (bottomY - topY - cardH) / 2);
  drawShibaCard(renderer_, Rect{leftX, cardY, totalW, cardH}, false, 4);

  const int midX = leftX + totalW / 2;
  const bool hasQuery = ctx.query && !ctx.query->empty();
  const ShibaIcon icon = hasQuery ? ShibaIcon::Search : ShibaIcon::Book;
  drawIcon(renderer_, icon, midX - getIconWidth(icon) / 2, cardY + 64, true);

  const char* msg = tr(STR_SHIBA_LIBRARY_EMPTY);
  if (ctx.filterFailed) {
    msg = tr(STR_LIBRARY_SEARCH_UNAVAILABLE);
  } else if (!hasQuery) {
    msg = tr(STR_LIBRARY_EMPTY);
  }

  const int mFont = hasCjkCodepoints(msg) ? SMALL_FONT_ID : UI_10_FONT_ID;
  const EpdFontFamily::Style mStyle = styleForText(msg, true);
  const auto lines = wrapSmartLines(renderer_, mFont, msg, totalW - 48, 3, mStyle);
  int ty = cardY + 104;
  for (const auto& ln : lines) {
    const int lw = renderer_.getTextWidth(mFont, ln.c_str(), mStyle);
    renderer_.drawText(mFont, midX - lw / 2, ty, ln.c_str(), true, mStyle);
    ty += 24;
  }
}

void ShibaLibraryPresenter::buildMiniCoverList(UiScreen& screen, const ShibaLibraryViewContext& ctx, const int leftX,
                                               const int rightX, const int topY, const int bottomY) {
  const int totalW = rightX - leftX;
  const int bodyH = std::max(104, bottomY - topY);
  constexpr int cardH = 104;
  constexpr int cardGap = 10;

  fui::ListProps props;
  const int selectionOffset = ctx.degraded ? 0 : 1;
  ctx.nav->syncToProps(fui::Rect{static_cast<int16_t>(leftX), static_cast<int16_t>(topY), static_cast<int16_t>(totalW),
                                 static_cast<int16_t>(bodyH)},
                       cardH, cardGap, ctx.listCount, props, selectionOffset);

  const int maxCards = std::min<int>(MAX_MINI_SNAPSHOTS, std::max(1, (bodyH + cardGap) / (cardH + cardGap)));
  const int top = static_cast<int>(props.topIndex);
  const int sel = static_cast<int>(props.selectedIndex);
  const int drawn = std::max(0, std::min(maxCards, ctx.listCount - top));

  if (!matchesCachedWindow(ctx, top, ctx.listCount) || static_cast<int>(windowItems_.size()) != drawn) {
    windowItems_.clear();
    for (int i = 0; i < drawn; ++i) {
      WindowItem item;
      item.entry = top + i;
      if (ctx.resolveBookRow) {
        ctx.resolveBookRow(ctx.user, item.entry, item.path, item.title, item.author);
      }
      populateBookItemMeta(item, ctx);
      windowItems_.push_back(std::move(item));
    }
    cachedTop_ = top;
    cachedDrawn_ = drawn;
    cachedListCount_ = ctx.listCount;
    cachedTab_ = ctx.activeTab;
    cachedSortRaw_ = ctx.sortOrderRaw;
    cachedCollapsed_ = ctx.groupsCollapsed;
    cachedQuery_ = ctx.query ? *ctx.query : std::string();
  }

  for (int i = 0; i < static_cast<int>(windowItems_.size()); ++i) {
    const auto& item = windowItems_[static_cast<size_t>(i)];
    const int ry0 = topY + i * (cardH + cardGap);
    const bool isSelected = (!ctx.tabsFocused && item.entry == sel);

    drawShibaCard(renderer_, Rect{leftX, ry0, totalW, cardH}, isSelected, isSelected ? 4 : 2);
    if (isSelected) {
      renderer_.fillRect(leftX + 6, ry0 + 12, 5, cardH - 24, true);
    }

    const int mcX = leftX + 18;
    const int mcY = ry0 + 13;
    paintOrRestoreMiniCover(static_cast<size_t>(i), item, mcX, mcY);

    // Right-aligned format / loan badge pill
    const int pillRightX = rightX - 14;
    int pillReservedW = 0;
    if (item.loanBadge.isLoan && !item.loanBadge.label.empty()) {
      const int pw =
          measurePillWidth(renderer_, UI_6_FONT_ID, item.loanBadge.label.c_str(), item.loanBadge.urgent ? 24 : 18);
      drawShibaBadgePill(renderer_, pillRightX - pw, ry0 + 13, 19, UI_6_FONT_ID, item.loanBadge.label.c_str(), true,
                         item.loanBadge.urgent);
      pillReservedW = pw + 8;
    } else if (item.isTxtOrMd) {
      const int pw = measurePillWidth(renderer_, UI_6_FONT_ID, item.extTag.c_str(), 18);
      drawShibaBadgePill(renderer_, pillRightX - pw, ry0 + 13, 19, UI_6_FONT_ID, item.extTag.c_str(), false, false);
      pillReservedW = pw + 8;
    }

    // Clamped 1-line title
    const int tx0 = mcX + MINI_COVER_W + 14;
    const int maxTitleW = std::max(40, (pillRightX - pillReservedW) - tx0);
    const EpdFontFamily::Style titleStyle = styleForText(item.title.c_str(), true);
    const std::string clampedTitle =
        clampSingleLine(renderer_, SMALL_FONT_ID, item.title.c_str(), maxTitleW, titleStyle);
    renderer_.drawText(SMALL_FONT_ID, tx0, ry0 + 14, clampedTitle.c_str(), true, titleStyle);

    // Clamped 1-line author
    const std::string rawAuthor = !item.author.empty() ? item.author
                                                       : std::string(item.isTxtOrMd ? tr(STR_SHIBA_AUTHOR_UNKNOWN)
                                                                                    : tr(STR_LIBRARY_UNKNOWN_AUTHOR));
    const std::string clampedAuthor = clampSingleLine(renderer_, UI_6_FONT_ID, rawAuthor.c_str(), rightX - 16 - tx0);
    renderer_.drawText(UI_6_FONT_ID, tx0, ry0 + 42, clampedAuthor.c_str(), true);

    // Bottom segmented progress bar + %
    char pctBuf[24];
    const char* pctLabel = pctBuf;
    if (item.hasProgress && item.progressPercent >= 100) {
      pctLabel = tr(STR_SHIBA_BOOK_FINISHED);
    } else if (item.hasProgress && item.progressPercent > 0) {
      snprintf(pctBuf, sizeof(pctBuf), "%u%%", static_cast<unsigned>(item.progressPercent));
    } else {
      pctLabel = tr(STR_SHIBA_BOOK_NEW);
    }
    renderer_.prewarmFallbackText(UI_6_FONT_ID, pctLabel, EpdFontFamily::REGULAR);
    const int pctW = renderer_.getTextWidth(UI_6_FONT_ID, pctLabel);
    renderer_.drawText(UI_6_FONT_ID, pillRightX - pctW, ry0 + 71, pctLabel, true);

    const int barW = std::max(36, (pillRightX - pctW - 10) - tx0);
    drawSegmentedProgressBar(renderer_, Rect{tx0, ry0 + 73, barW, 11}, item.hasProgress ? item.progressPercent : 0);

    screen.frame().hit(fui::Rect{static_cast<int16_t>(leftX), static_cast<int16_t>(ry0), static_cast<int16_t>(totalW),
                                 static_cast<int16_t>(cardH)},
                       ctx.actionRow, static_cast<int16_t>(item.entry), fui::InputTouch | fui::InputLongPress);
  }

  lastRenderedTop_ = top;
  lastRenderedDrawn_ = std::max(1, drawn);
  ctx.nav->onListRendered(static_cast<uint16_t>(top), drawn, sel >= top && sel < top + drawn);
}

void ShibaLibraryPresenter::buildAuthorSpineList(UiScreen& screen, const ShibaLibraryViewContext& ctx, const int leftX,
                                                 const int rightX, const int topY, const int bottomY) {
  const int totalW = rightX - leftX;
  const int bodyH = std::max(140, bottomY - topY);
  drawShibaCard(renderer_, Rect{leftX, topY, totalW, bodyH}, false, 4);

  const int ix0 = leftX + 12;
  const int ix1 = rightX - 12;
  const int innerW = ix1 - ix0;
  const int innerTopY = topY + 10;
  const int innerBottomY = bottomY - 8;
  const int innerAvailH = std::max(60, innerBottomY - innerTopY);

  constexpr int headerH = 26;
  constexpr int headerGap = 4;
  constexpr int rowH = 44;
  constexpr int rowGap = 2;

  const int prevTop = ctx.nav->top;
  const bool prevTrusted = ctx.nav->trusts(ctx.listCount);
  const int prevDrawn = ctx.nav->drawnRows;

  fui::ListProps props;
  const int selectionOffset = ctx.degraded ? 0 : 1;
  ctx.nav->syncToProps(fui::Rect{static_cast<int16_t>(ix0), static_cast<int16_t>(innerTopY),
                                 static_cast<int16_t>(innerW), static_cast<int16_t>(innerAvailH)},
                       50, 2, ctx.listCount, props, selectionOffset);

  int top = static_cast<int>(props.topIndex);
  const int sel = static_cast<int>(props.selectedIndex);
  if (ctx.nav->followPending && prevTrusted && prevDrawn > 0 && sel >= prevTop && sel < prevTop + prevDrawn) {
    top = prevTop;
  }

  auto measureRowsFrom = [&](const int candidateTop) -> int {
    int usedH = 0;
    int fit = 0;
    std::string prevAuthor;
    std::string p;
    std::string t;
    std::string a;
    for (int e = candidateTop; e < ctx.listCount && fit < static_cast<int>(MAX_WINDOW_ITEMS); ++e) {
      p.clear();
      t.clear();
      a.clear();
      if (ctx.resolveBookRow) ctx.resolveBookRow(ctx.user, e, p, t, a);
      const bool starts = (fit == 0 || a != prevAuthor);
      const int needed = (starts ? (headerH + headerGap) : 0) + rowH + (fit > 0 ? rowGap : 0);
      if (usedH + needed > innerAvailH && fit > 0) break;
      usedH += needed;
      prevAuthor = a;
      ++fit;
    }
    return fit;
  };

  if (ctx.nav->followPending && sel >= 0) {
    if (sel < top) top = sel;
    while (top < sel) {
      const int fit = measureRowsFrom(top);
      if (sel < top + std::max(1, fit)) break;
      ++top;
    }
  }

  if (!matchesCachedWindow(ctx, top, ctx.listCount)) {
    windowItems_.clear();
    int usedH = 0;
    std::string prevAuthor;
    for (int e = top; e < ctx.listCount && windowItems_.size() < MAX_WINDOW_ITEMS; ++e) {
      WindowItem item;
      item.entry = e;
      if (ctx.resolveBookRow) {
        ctx.resolveBookRow(ctx.user, e, item.path, item.title, item.author);
      }
      item.startsGroup = (windowItems_.empty() || item.author != prevAuthor);
      const int needed = (item.startsGroup ? (headerH + headerGap) : 0) + rowH + (!windowItems_.empty() ? rowGap : 0);
      if (usedH + needed > innerAvailH && !windowItems_.empty()) break;
      usedH += needed;
      prevAuthor = item.author;
      item.groupHeading = item.author.empty() ? std::string(tr(STR_SHIBA_AUTHOR_UNKNOWN)) : item.author;
      populateBookItemMeta(item, ctx);
      windowItems_.push_back(std::move(item));
    }
    cachedTop_ = top;
    cachedDrawn_ = static_cast<int>(windowItems_.size());
    cachedListCount_ = ctx.listCount;
    cachedTab_ = ctx.activeTab;
    cachedSortRaw_ = ctx.sortOrderRaw;
    cachedCollapsed_ = ctx.groupsCollapsed;
    cachedQuery_ = ctx.query ? *ctx.query : std::string();
  }

  int cy = innerTopY;
  for (size_t i = 0; i < windowItems_.size(); ++i) {
    const auto& item = windowItems_[i];
    if (i > 0) cy += rowGap;

    if (item.startsGroup) {
      renderer_.fillRect(ix0, cy, innerW, headerH, true);
      drawIcon(renderer_, ShibaIcon::Book, ix0 + 8, cy + (headerH - getIconHeight(ShibaIcon::Book)) / 2, false);
      const EpdFontFamily::Style hStyle = styleForText(item.groupHeading.c_str(), true);
      const std::string clampedHeading =
          clampSingleLine(renderer_, SMALL_FONT_ID, item.groupHeading.c_str(), innerW - 38, hStyle);
      const int hTextH = renderer_.getTextHeight(SMALL_FONT_ID);
      renderer_.drawText(SMALL_FONT_ID, ix0 + 30, cy + std::max(1, (headerH - hTextH) / 2) - 1, clampedHeading.c_str(),
                         false, hStyle);
      cy += headerH + headerGap;
    }

    const bool isSelected = (!ctx.tabsFocused && item.entry == sel);
    if (isSelected) {
      renderer_.fillRoundedRect(ix0, cy, innerW, rowH, 6, Color::White);
      renderer_.drawRoundedRect(ix0, cy, innerW, rowH, 2, 6, true);
      renderer_.fillRect(ix0 + 5, cy + 7, 5, rowH - 14, true);
    } else {
      renderer_.drawLine(ix0 + 4, cy + rowH - 1, ix1 - 4, cy + rowH - 1, true);
    }

    // Status % right-aligned
    char pctBuf[24];
    const char* pctLabel = pctBuf;
    if (item.hasProgress && item.progressPercent >= 100) {
      pctLabel = tr(STR_SHIBA_BOOK_FINISHED);
    } else if (item.hasProgress && item.progressPercent > 0) {
      snprintf(pctBuf, sizeof(pctBuf), "%u%%", static_cast<unsigned>(item.progressPercent));
    } else {
      pctLabel = tr(STR_SHIBA_BOOK_NEW);
    }
    renderer_.prewarmFallbackText(UI_6_FONT_ID, pctLabel, EpdFontFamily::REGULAR);
    const int pctW = renderer_.getTextWidth(UI_6_FONT_ID, pctLabel);
    const int pctTextH = renderer_.getTextHeight(UI_6_FONT_ID);
    renderer_.drawText(UI_6_FONT_ID, ix1 - 6 - pctW, cy + std::max(2, (rowH - pctTextH) / 2) - 1, pctLabel, true);

    // Compact segmented progress bar
    constexpr int barW = 48;
    const int barX = ix1 - 6 - std::max(34, pctW + 8) - barW;
    drawSegmentedProgressBar(renderer_, Rect{barX, cy + (rowH - 10) / 2, barW, 10},
                             item.hasProgress ? item.progressPercent : 0);

    // Format or Loan pill
    const char* badgeStr = item.loanBadge.isLoan ? item.loanBadge.label.c_str() : item.extTag.c_str();
    const bool badgeFilled = item.loanBadge.isLoan;
    const int badgeW = measurePillWidth(renderer_, UI_6_FONT_ID, badgeStr, item.loanBadge.urgent ? 24 : 18);
    const int badgeX = barX - 8 - badgeW;
    drawShibaBadgePill(renderer_, badgeX, cy + (rowH - 19) / 2, 19, UI_6_FONT_ID, badgeStr, badgeFilled,
                       item.loanBadge.urgent);

    // Book title
    const int tx = isSelected ? (ix0 + 18) : (ix0 + 10);
    const int maxTitleW = std::max(48, badgeX - 8 - tx);
    const EpdFontFamily::Style tStyle = styleForText(item.title.c_str(), isSelected);
    const std::string clampedTitle = clampSingleLine(renderer_, SMALL_FONT_ID, item.title.c_str(), maxTitleW, tStyle);
    const int tTextH = renderer_.getTextHeight(SMALL_FONT_ID);
    renderer_.drawText(SMALL_FONT_ID, tx, cy + std::max(2, (rowH - tTextH) / 2) - 1, clampedTitle.c_str(), true,
                       tStyle);

    screen.frame().hit(fui::Rect{static_cast<int16_t>(ix0), static_cast<int16_t>(cy), static_cast<int16_t>(innerW),
                                 static_cast<int16_t>(rowH)},
                       ctx.actionRow, static_cast<int16_t>(item.entry), fui::InputTouch | fui::InputLongPress);
    cy += rowH;
  }

  const int drawn = static_cast<int>(windowItems_.size());
  lastRenderedTop_ = top;
  lastRenderedDrawn_ = std::max(1, drawn);
  ctx.nav->onListRendered(static_cast<uint16_t>(top), drawn, sel >= top && sel < top + drawn);
}

void ShibaLibraryPresenter::buildCollapsedGroupList(UiScreen& screen, const ShibaLibraryViewContext& ctx,
                                                    const int leftX, const int rightX, const int topY,
                                                    const int bottomY) {
  const int totalW = rightX - leftX;
  const int bodyH = std::max(140, bottomY - topY);
  drawShibaCard(renderer_, Rect{leftX, topY, totalW, bodyH}, false, 4);

  const int ix0 = leftX + 12;
  const int ix1 = rightX - 12;
  const int innerW = ix1 - ix0;
  const int innerTopY = topY + 12;
  const int innerAvailH = std::max(60, (bottomY - 10) - innerTopY);
  constexpr int rowH = 48;
  constexpr int rowGap = 6;

  fui::ListProps props;
  const int selectionOffset = ctx.degraded ? 0 : 1;
  ctx.nav->syncToProps(fui::Rect{static_cast<int16_t>(ix0), static_cast<int16_t>(innerTopY),
                                 static_cast<int16_t>(innerW), static_cast<int16_t>(innerAvailH)},
                       rowH, rowGap, ctx.listCount, props, selectionOffset);

  const int maxRows = std::min<int>(MAX_WINDOW_ITEMS, std::max(1, (innerAvailH + rowGap) / (rowH + rowGap)));
  const int top = static_cast<int>(props.topIndex);
  const int sel = static_cast<int>(props.selectedIndex);
  const int drawn = std::max(0, std::min(maxRows, ctx.listCount - top));

  for (int i = 0; i < drawn; ++i) {
    const int groupEntry = top + i;
    std::string heading;
    int bookCount = 0;
    if (ctx.resolveCollapsedGroup) {
      ctx.resolveCollapsedGroup(ctx.user, groupEntry, heading, bookCount);
    }
    if (heading.empty()) heading = tr(STR_SHIBA_AUTHOR_UNKNOWN);

    const int cy = innerTopY + i * (rowH + rowGap);
    const bool isSelected = (!ctx.tabsFocused && groupEntry == sel);

    if (isSelected) {
      renderer_.fillRoundedRect(ix0, cy, innerW, rowH, 7, Color::Black);
      renderer_.drawRoundedRect(ix0 + 3, cy + 3, innerW - 6, rowH - 6, 1, 5, false);
    } else {
      renderer_.fillRoundedRect(ix0, cy, innerW, rowH, 7, Color::White);
      renderer_.drawRoundedRect(ix0, cy, innerW, rowH, 1, 7, true);
    }

    drawIcon(renderer_, ShibaIcon::Book, ix0 + 12, cy + (rowH - getIconHeight(ShibaIcon::Book)) / 2, !isSelected);

    char countBuf[40];
    std::snprintf(countBuf, sizeof(countBuf), tr(STR_SHIBA_AUTHOR_BOOKS_COUNT), bookCount);
    renderer_.prewarmFallbackText(UI_6_FONT_ID, countBuf, EpdFontFamily::REGULAR);
    const int countW = renderer_.getTextWidth(UI_6_FONT_ID, countBuf);
    const int countTextH = renderer_.getTextHeight(UI_6_FONT_ID);
    renderer_.drawText(UI_6_FONT_ID, ix1 - 14 - countW, cy + std::max(2, (rowH - countTextH) / 2) - 1, countBuf,
                       !isSelected);

    const EpdFontFamily::Style hStyle = styleForText(heading.c_str(), true);
    const std::string clampedHeading =
        clampSingleLine(renderer_, SMALL_FONT_ID, heading.c_str(), (ix1 - 24 - countW) - (ix0 + 36), hStyle);
    const int hTextH = renderer_.getTextHeight(SMALL_FONT_ID);
    renderer_.drawText(SMALL_FONT_ID, ix0 + 36, cy + std::max(2, (rowH - hTextH) / 2) - 1, clampedHeading.c_str(),
                       !isSelected, hStyle);

    screen.frame().hit(fui::Rect{static_cast<int16_t>(ix0), static_cast<int16_t>(cy), static_cast<int16_t>(innerW),
                                 static_cast<int16_t>(rowH)},
                       ctx.actionRow, static_cast<int16_t>(groupEntry), fui::InputTouch | fui::InputLongPress);
  }

  lastRenderedTop_ = top;
  lastRenderedDrawn_ = std::max(1, drawn);
  ctx.nav->onListRendered(static_cast<uint16_t>(top), drawn, sel >= top && sel < top + drawn);
}

void ShibaLibraryPresenter::buildScreen(UiScreen& screen, const ShibaLibraryViewContext& ctx) {
  if (!loansLoaded_) {
    cachedLoans_.clear();
    libby::loadCachedLoans(cachedLoans_);
    loansLoaded_ = true;
  }
  if (lastPoseTab_ >= 0 && lastPoseTab_ != ctx.activeTab) {
    currentPose_ = advancePoseCycle();
  }
  lastPoseTab_ = ctx.activeTab;

  const Rect content = UITheme::getContentArea(renderer_);
  const int leftX = content.x + 20;
  const int rightX = content.x + content.width - 20;

  drawStaggeredDotBackground(
      renderer_, Rect{0, content.y + 36, renderer_.getScreenWidth(), renderer_.getScreenHeight() - (content.y + 36)});

  char subBuf[64];
  const int shownBooks = (ctx.query && !ctx.query->empty()) ? ctx.bookRowCount : ctx.totalIndexedBooks;
  std::snprintf(subBuf, sizeof(subBuf), tr(STR_DESK_BOOK_COUNT), shownBooks);
  drawShibaStatusBar(renderer_, tr(STR_LIBRARY), subBuf);

  if (mappedInput_.hasTouch()) {
    screen.frame().hit(fui::Rect{0, 0, 148, 44}, ctx.actionBack, 0, fui::InputTouch);
  }

  const int ledgeY = content.y + 82;
  drawFilterStrip(screen, ctx, leftX, rightX, ledgeY);

  const int hintReserve = gpio.hasTouch() ? 8 : 44;
  const int footerBarY = content.y + content.height - hintReserve - 26;
  const int bodyTopY = ledgeY + 42 + 12;
  const int bodyBottomY = footerBarY - 10;

  if (ctx.bookRowCount <= 0) {
    lastRenderedTop_ = 0;
    lastRenderedDrawn_ = 1;
    drawEmptyState(ctx, leftX, rightX, bodyTopY, bodyBottomY);
    return;
  }

  if (ctx.groupsCollapsed) {
    buildCollapsedGroupList(screen, ctx, leftX, rightX, bodyTopY, bodyBottomY);
  } else if (ctx.activeTab == 2) {
    buildAuthorSpineList(screen, ctx, leftX, rightX, bodyTopY, bodyBottomY);
  } else {
    buildMiniCoverList(screen, ctx, leftX, rightX, bodyTopY, bodyBottomY);
  }
}

void ShibaLibraryPresenter::drawFooter(const ShibaLibraryViewContext& ctx) const {
  const Rect content = UITheme::getContentArea(renderer_);
  const int leftX = content.x + 20;
  const int rightX = content.x + content.width - 20;
  const int barW = rightX - leftX;
  constexpr int barH = 26;
  const int hintReserve = gpio.hasTouch() ? 8 : 44;
  const int barY = content.y + content.height - hintReserve - barH;

  const int pageSize = std::max(1, lastRenderedDrawn_);
  const int currentPage = ctx.listCount > 0 ? (lastRenderedTop_ / pageSize) + 1 : 1;
  const int totalPages = ctx.listCount > 0 ? std::max(currentPage, (ctx.listCount + pageSize - 1) / pageSize) : 1;

  if (mappedInput_.hasTouch() || !ctx.holdHelpText || ctx.holdHelpText[0] == '\0') {
    const int startItem = ctx.listCount > 0 ? (lastRenderedTop_ + 1) : 0;
    const int endItem = ctx.listCount > 0 ? std::min(ctx.listCount, lastRenderedTop_ + lastRenderedDrawn_) : 0;
    drawShibaPaginationBar(renderer_, currentPage, totalPages, startItem, endItem, ctx.listCount);
  } else {
    renderer_.fillRect(leftX - 1, barY - 1, barW + 2, barH + 2, false);
    renderer_.drawRect(leftX, barY, barW, barH, 2, true);

    char posBuf[64];
    if (ctx.listCount > 0) {
      const char* fmt = ctx.groupsCollapsed ? tr(STR_LIBRARY_GROUP_POSITION) : tr(STR_LIBRARY_POSITION);
      char itemPos[36];
      snprintf(itemPos, sizeof(itemPos), fmt, std::min(ctx.listCount, ctx.selectedEntry + 1), ctx.listCount);
      snprintf(posBuf, sizeof(posBuf), "%d/%d \xC2\xB7 %s", currentPage, totalPages, itemPos);
    } else {
      snprintf(posBuf, sizeof(posBuf), "0 / 0");
    }

    const int textH = renderer_.getTextHeight(UI_6_FONT_ID);
    const int textY = barY + std::max(1, (barH - textH) / 2) - 1;
    renderer_.prewarmFallbackText(UI_6_FONT_ID, posBuf, EpdFontFamily::REGULAR);
    const int posW = renderer_.getTextWidth(UI_6_FONT_ID, posBuf);
    renderer_.drawText(UI_6_FONT_ID, rightX - 10 - posW, textY, posBuf, true);

    const int maxHelpW = std::max(40, (rightX - 10 - posW) - (leftX + 10) - 12);
    const std::string clampedHelp = clampSingleLine(renderer_, UI_6_FONT_ID, ctx.holdHelpText, maxHelpW);
    renderer_.drawText(UI_6_FONT_ID, leftX + 10, textY, clampedHelp.c_str(), true);
  }

  drawShibaHardwareKeyHints(renderer_, mappedInput_, ctx.backHintLabel, ctx.confirmHintLabel, ctx.prevHintLabel,
                            ctx.nextHintLabel);
}

}  // namespace shibadesk
