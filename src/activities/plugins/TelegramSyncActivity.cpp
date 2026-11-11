#include "TelegramSyncActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "TelegramCredentialStore.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/CatalogScreens.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

void TelegramSyncActivity::onEnter() {
  CatalogActivity::onEnter();
  TELEGRAM_STORE.loadFromFile();
  state = State::BROWSING;
  rowsDirty_ = true;
  nav.reset();
  requestUpdate();
}

int TelegramSyncActivity::listCount() const {
  if (state != State::BROWSING) return 0;
  return 3 + static_cast<int>(lastSummary_.savedPaths.size());
}

void TelegramSyncActivity::rebuildRows() {
  rowItems_.clear();
  rowSubtitles_.clear();
  rowValues_.clear();
  const int total = listCount();
  if (total <= 0) return;

  rowItems_.reserve(total);
  rowSubtitles_.reserve(total);
  rowValues_.reserve(total);

  auto addRow = [&](const char* label, std::string subtitle, std::string value) {
    rowSubtitles_.push_back(std::move(subtitle));
    rowValues_.push_back(std::move(value));
    fui::ListItem item;
    item.label = label;
    item.subtitle = rowSubtitles_.back().empty() ? nullptr : rowSubtitles_.back().c_str();
    item.value = rowValues_.back().empty() ? nullptr : rowValues_.back().c_str();
    item.actionValue = static_cast<int16_t>(rowItems_.size());
    rowItems_.push_back(item);
  };

  std::string syncSub;
  if (TELEGRAM_STORE.hasCredentials()) {
    char chatBuf[80];
    std::snprintf(chatBuf, sizeof(chatBuf), tr(STR_TELEGRAM_CHAT_ID_FMT), TELEGRAM_STORE.getAllowedChatId().c_str());
    syncSub = chatBuf;
    if (!lastStatusBanner_.empty()) {
      syncSub += " \xC2\xB7 " + lastStatusBanner_;
    }
  } else {
    syncSub = tr(STR_TELEGRAM_NOT_CONFIGURED);
  }
  addRow(tr(STR_TELEGRAM_SYNC_NOW), std::move(syncSub), ">");

  addRow(tr(STR_TELEGRAM_AUTO_SYNC), tr(STR_TELEGRAM_AUTO_SYNC_SUB),
         TELEGRAM_STORE.getAutoSyncOnSleep() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));

  addRow(tr(STR_TELEGRAM_DEST_FOLDER), tr(STR_TELEGRAM_DEST_FOLDER_SUB), TELEGRAM_STORE.getDestFolder());

  for (const auto& saved : lastSummary_.savedPaths) {
    addRow(saved.c_str(), tr(STR_TELEGRAM_DOWNLOADED_SUB), tr(STR_TELEGRAM_SAVED));
  }
}

void TelegramSyncActivity::retryBrowse() {
  if (!TELEGRAM_STORE.hasCredentials()) {
    state = State::BROWSING;
    rowsDirty_ = true;
    requestUpdate();
    return;
  }
  startBrowse();
}

void TelegramSyncActivity::startBrowse() {
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseSdFontCaches();
  }

  state = State::LOADING;
  statusMessage = tr(STR_TELEGRAM_SYNCING);
  lastProgressPct_ = -1;
  lastProgressMs_ = 0;
  requestUpdateAndWait();

  std::string err;
  const bool ok = telegram::syncPendingFiles(lastSummary_, err, [this](const std::string& file, size_t dl, size_t tot) {
    state = State::DOWNLOADING;
    statusMessage = file;
    downloadProgress = dl;
    downloadTotal = tot;
    const int pct = tot > 0 ? static_cast<int>(static_cast<uint64_t>(dl) * 100 / tot) : 0;
    const unsigned long now = millis();
    if (pct >= 100 || lastProgressPct_ < 0 || pct >= lastProgressPct_ + 10 || now - lastProgressMs_ >= 3000) {
      lastProgressPct_ = pct;
      lastProgressMs_ = now;
      requestUpdate(true);
    }
  });
  if (!ok) {
    state = State::ERROR;
    errorMessage = err;
    requestUpdate();
    return;
  }

  char syncedBuf[64];
  std::snprintf(syncedBuf, sizeof(syncedBuf), tr(STR_TELEGRAM_FILES_SYNCED_FMT), lastSummary_.filesDownloaded);
  lastStatusBanner_ = syncedBuf;
  closeRouting();
  rowsDirty_ = true;
  state = State::BROWSING;
  requestUpdate();
}

void TelegramSyncActivity::downloadFinished(bool /*cancelled*/) {
  state = State::BROWSING;
  rowsDirty_ = true;
  requestUpdate();
}

void TelegramSyncActivity::onBackButton() {
  if (state == State::ERROR || state == State::DONE) {
    state = State::BROWSING;
    rowsDirty_ = true;
    requestUpdate();
    return;
  }
  finish();
}

void TelegramSyncActivity::activateIndex(int index) {
  if (state != State::BROWSING || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;

  if (index == 0) {
    if (!TELEGRAM_STORE.hasCredentials()) {
      state = State::ERROR;
      errorMessage = tr(STR_TELEGRAM_NOT_CONFIGURED);
      requestUpdate();
      return;
    }
    checkAndConnectWifi();
    return;
  }

  if (index == 1) {
    TELEGRAM_STORE.setAutoSyncOnSleep(!TELEGRAM_STORE.getAutoSyncOnSleep());
    TELEGRAM_STORE.saveToFile();
    rowsDirty_ = true;
    requestUpdate();
    return;
  }

  if (index == 2) {
    auto kb = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_TELEGRAM_DEST_FOLDER),
                                                       TELEGRAM_STORE.getDestFolder(), 64, InputType::Text);
    if (!kb) {
      LOG_ERR("TGS", "OOM: KeyboardEntryActivity (%zu B)", sizeof(KeyboardEntryActivity));
      return;
    }
    startActivityForResult(std::move(kb), [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& keyboard = std::get<KeyboardResult>(result.data);
        TELEGRAM_STORE.setDestFolder(keyboard.text);
        TELEGRAM_STORE.saveToFile();
      }
      state = State::BROWSING;
      rowsDirty_ = true;
      requestUpdate();
    });
  }
}

bool TelegramSyncActivity::handleButtons() {
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

void TelegramSyncActivity::buildScreen(UiScreen& screen) {
  screenHeader(screen, tr(STR_TELEGRAM_SYNC));
  if (buildStatusScreen(screen, /*boldError=*/true, /*showDownloadTotal=*/true)) return;

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

void TelegramSyncActivity::drawFooter() {
  const int count = listCount();
  const char* confirmLabel = (state == State::BROWSING && count > 0)
                                 ? tr(STR_SELECT)
                                 : ((state == State::ERROR && TELEGRAM_STORE.hasCredentials()) ? tr(STR_RETRY) : "");
  const char* up = (state == State::BROWSING && count > 1) ? tr(STR_DIR_UP) : "";
  const char* down = (state == State::BROWSING && count > 1) ? tr(STR_DIR_DOWN) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, up, down);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
