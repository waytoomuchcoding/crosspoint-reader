#include "LibbyActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/CatalogScreens.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

void LibbyActivity::onEnter() {
  CatalogActivity::onEnter();
  hasAccountToken_ = libby::hasToken();
  libby::loadCachedLoans(loans_);
  state = State::BROWSING;
  rowsDirty_ = true;
  nav.reset();
  requestUpdate();
}

int LibbyActivity::listCount() const {
  if (state != State::BROWSING) return 0;
  const int actionRows = hasAccountToken_ ? 2 : 1;
  return actionRows + static_cast<int>(loans_.size());
}

void LibbyActivity::rebuildRows() {
  rowItems_.clear();
  rowSubtitles_.clear();
  const int total = listCount();
  if (total <= 0) return;

  rowItems_.reserve(total);
  rowSubtitles_.reserve(total);

  auto addRow = [&](const char* label, std::string subtitle, const char* value) {
    rowSubtitles_.push_back(std::move(subtitle));
    fui::ListItem item;
    item.label = label;
    item.subtitle = rowSubtitles_.back().empty() ? nullptr : rowSubtitles_.back().c_str();
    item.value = value;
    item.actionValue = static_cast<int16_t>(rowItems_.size());
    rowItems_.push_back(item);
  };

  addRow(tr(STR_LIBBY_PAIR_CODE), tr(STR_LIBBY_PAIR_SUBTITLE), hasAccountToken_ ? tr(STR_LIBBY_LINKED) : ">");
  if (hasAccountToken_) {
    addRow(tr(STR_LIBBY_SYNC_LOANS), tr(STR_LIBBY_SYNC_SUBTITLE), ">");
  }

  for (const auto& loan : loans_) {
    std::string sub = loan.author;
    if (!sub.empty()) sub += " \xC2\xB7 ";
    if (loan.hasOpenEpub) {
      sub += tr(STR_LIBBY_FORMAT_OPEN);
    } else if (loan.hasAdobeEpub) {
      sub += tr(STR_LIBBY_FORMAT_ADOBE);
    } else {
      sub += tr(STR_LIBBY_FORMAT_NONE);
    }
    addRow(loan.title.c_str(), std::move(sub), loan.isDownloadable() ? tr(STR_FETCH) : "");
  }
}

void LibbyActivity::promptSetupCode() {
  auto kb =
      makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_LIBBY_PAIR_CODE), "", 8, InputType::Text);
  if (!kb) {
    LOG_ERR("LBY", "OOM: KeyboardEntryActivity (%zu B)", sizeof(KeyboardEntryActivity));
    return;
  }
  startActivityForResult(std::move(kb), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      state = State::BROWSING;
      requestUpdate();
      return;
    }
    const auto& keyboard = std::get<KeyboardResult>(result.data);
    pendingSetupCode_ = keyboard.text;
    pendingOp_ = PendingWifiOp::PairCode;
    checkAndConnectWifi();
  });
}

void LibbyActivity::startBrowse() { runPendingWifiOp(); }

void LibbyActivity::retryBrowse() {
  pendingOp_ = lastAttemptedOp_;
  runPendingWifiOp();
}

void LibbyActivity::pumpTransferProgress(const size_t dl, const size_t tot) {
  downloadProgress = dl;
  downloadTotal = tot;
  const int pct = tot > 0 ? static_cast<int>(static_cast<uint64_t>(dl) * 100 / tot) : 0;
  const unsigned long now = millis();
  if (pct >= 100 || lastProgressPct_ < 0 || pct >= lastProgressPct_ + 10 || now - lastProgressMs_ >= 3000) {
    lastProgressPct_ = pct;
    lastProgressMs_ = now;
    requestUpdate(true);
  }
}

void LibbyActivity::runPendingWifiOp() {
  const PendingWifiOp op = (pendingOp_ == PendingWifiOp::None) ? PendingWifiOp::SyncLoans : pendingOp_;
  pendingOp_ = PendingWifiOp::None;
  lastAttemptedOp_ = op;

  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseSdFontCaches();
  }

  if (op == PendingWifiOp::PairCode) {
    state = State::LOADING;
    statusMessage = tr(STR_LIBBY_PAIRING);
    requestUpdateAndWait();

    std::string err;
    if (!libby::pairWithSetupCode(pendingSetupCode_, err)) {
      state = State::ERROR;
      errorMessage = err;
      requestUpdate();
      return;
    }
    hasAccountToken_ = true;
    libby::fetchLoans(loans_, err);
    closeRouting();
    rowsDirty_ = true;
    state = State::BROWSING;
    requestUpdate();
    return;
  }

  if (op == PendingWifiOp::DownloadLoan && pendingLoanIndex_ >= 0 &&
      pendingLoanIndex_ < static_cast<int>(loans_.size())) {
    const libby::Loan loan = loans_[pendingLoanIndex_];
    lastProgressPct_ = -1;
    lastProgressMs_ = 0;
    beginDownload(loan.title);

    std::string destPath;
    std::string err;
    const bool ok =
        libby::downloadLoan(loan, destPath, err, [this](size_t dl, size_t tot) { pumpTransferProgress(dl, tot); });
    if (!ok) {
      state = State::ERROR;
      errorMessage = err;
      requestUpdate();
      return;
    }
    statusMessage = destPath;
    state = State::DONE;
    requestUpdate();
    return;
  }

  state = State::LOADING;
  statusMessage = tr(STR_LIBBY_SYNC_LOANS);
  requestUpdateAndWait();

  std::string err;
  if (!libby::fetchLoans(loans_, err)) {
    state = State::ERROR;
    errorMessage = err;
    requestUpdate();
    return;
  }
  hasAccountToken_ = libby::hasToken();
  closeRouting();
  rowsDirty_ = true;
  state = State::BROWSING;
  requestUpdate();
}

void LibbyActivity::downloadFinished(bool /*cancelled*/) {
  state = State::BROWSING;
  rowsDirty_ = true;
  requestUpdate();
}

void LibbyActivity::onBackButton() {
  if (state == State::ERROR || state == State::DONE) {
    state = State::BROWSING;
    rowsDirty_ = true;
    requestUpdate();
    return;
  }
  finish();
}

void LibbyActivity::activateIndex(int index) {
  if (state != State::BROWSING || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;

  if (index == 0) {
    promptSetupCode();
    return;
  }
  if (hasAccountToken_ && index == 1) {
    pendingOp_ = PendingWifiOp::SyncLoans;
    checkAndConnectWifi();
    return;
  }

  const int loanIdx = index - (hasAccountToken_ ? 2 : 1);
  if (loanIdx >= 0 && loanIdx < static_cast<int>(loans_.size())) {
    if (!loans_[loanIdx].isDownloadable()) return;
    pendingLoanIndex_ = loanIdx;
    pendingOp_ = PendingWifiOp::DownloadLoan;
    checkAndConnectWifi();
  }
}

bool LibbyActivity::handleButtons() {
  using B = MappedInputManager::Button;
  if (mappedInput.wasReleased(B::Back)) {
    onBackButton();
    return true;
  }
  if (mappedInput.wasReleased(B::Confirm)) {
    if (state == State::BROWSING) {
      activateIndex(nav.selected);
      return true;
    }
    if (state == State::DONE) {
      state = State::BROWSING;
      rowsDirty_ = true;
      requestUpdate();
      return true;
    }
  }
  return false;
}

void LibbyActivity::buildScreen(UiScreen& screen) {
  screenHeader(screen, tr(STR_LIBBY_LIBRARY));
  if (buildStatusScreen(screen, /*boldError=*/true, /*showDownloadTotal=*/true)) return;

  if (state == State::DONE) {
    catalogCenteredBlock(screen, {{tr(STR_DOWNLOAD_COMPLETE), true}, {statusMessage.c_str()}});
    return;
  }

  if (rowsDirty_) {
    rebuildRows();
    rowsDirty_ = false;
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void LibbyActivity::drawFooter() {
  const int count = listCount();
  const char* confirmLabel =
      (state == State::BROWSING && count > 0) ? tr(STR_SELECT) : (state == State::ERROR ? tr(STR_RETRY) : "");
  const char* up = (state == State::BROWSING && count > 1) ? tr(STR_DIR_UP) : "";
  const char* down = (state == State::BROWSING && count > 1) ? tr(STR_DIR_DOWN) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, up, down);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
