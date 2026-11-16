#include "BleShareActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "BleShareStore.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/BleShareService.h"

namespace fui = freeink::ui;

namespace {

enum class MenuCode : int16_t {
  PairPhone = 0,
  Discoverable = 1,
  AutoAccept = 2,
  DestFolder = 3,
  ForgetPhone = 4,
};

void formatBytes(const uint64_t bytes, char* out, const size_t cap) {
  if (bytes >= 1024ULL * 1024ULL) {
    const unsigned tenths = static_cast<unsigned>((bytes * 10ULL) / (1024ULL * 1024ULL));
    std::snprintf(out, cap, "%u.%u MB", tenths / 10u, tenths % 10u);
  } else if (bytes >= 1024ULL) {
    std::snprintf(out, cap, "%llu KB", static_cast<unsigned long long>((bytes + 512ULL) / 1024ULL));
  } else {
    std::snprintf(out, cap, "%llu B", static_cast<unsigned long long>(bytes));
  }
}

}  // namespace

BleShareActivity::BleShareActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const bool backgroundLaunch,
                                   std::string returnBookPath)
    : UiListActivity("BleShare", renderer, mappedInput),
      backgroundLaunch_(backgroundLaunch),
      returnBookPath_(std::move(returnBookPath)) {}

void BleShareActivity::onEnter() {
  UiListActivity::onEnter();
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseSdFontCaches();
  }
  BLESHARE_STORE.loadFromFile();
  incomingPopupShown_ = false;
  donePopupShown_ = false;
  rebuildMenuRows();
  syncSessionToUi();
  requestUpdate();
}

void BleShareActivity::onExit() {
  popup_.dismiss();
  bleshare::stopPairingMode();
  UiListActivity::onExit();
}

bool BleShareActivity::skipLoopDelay() {
  const auto st = bleshare::sessionState();
  return bleshare::hasLiveLink() || st == xpbt::SessionState::ReceivingFile || st == xpbt::SessionState::ReadyForFiles;
}

const char* BleShareActivity::headerTitle() const { return tr(STR_BLE_SHARE_TITLE); }

int BleShareActivity::listCount() const { return view_ == View::Menu ? static_cast<int>(rows_.size()) : 0; }

void BleShareActivity::rebuildMenuRows() {
  RenderLock lock(*this);
  rows_.clear();
  subtitles_.clear();
  values_.clear();
  rows_.reserve(5);
  subtitles_.reserve(5);
  values_.reserve(5);

  const auto addRow = [&](const char* label, std::string sub, std::string val, const MenuCode code,
                          const bool isToggle = false, const bool checked = false) {
    subtitles_.push_back(std::move(sub));
    values_.push_back(std::move(val));
    fui::ListItem item;
    item.label = label;
    item.subtitle = subtitles_.back().empty() ? nullptr : subtitles_.back().c_str();
    item.value = values_.back().empty() ? nullptr : values_.back().c_str();
    item.actionValue = static_cast<int16_t>(code);
    item.toggle = isToggle;
    item.toggleChecked = checked;
    rows_.push_back(item);
  };

  std::string pairSub;
  if (BLESHARE_STORE.isPaired()) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), tr(STR_BLE_SHARE_PAIRED_FMT), BLESHARE_STORE.getPairedPhoneName().c_str());
    pairSub = buf;
  } else {
    pairSub = tr(STR_BLE_SHARE_NOT_PAIRED);
  }
  addRow(tr(STR_BLE_SHARE_PAIR_PHONE), std::move(pairSub), ">", MenuCode::PairPhone);

  addRow(tr(STR_BLE_SHARE_DISCOVERABLE), tr(STR_BLE_SHARE_DISCOVERABLE_SUB), {}, MenuCode::Discoverable, true,
         BLESHARE_STORE.isEnabled());

  addRow(tr(STR_BLE_SHARE_AUTO_ACCEPT), tr(STR_BLE_SHARE_AUTO_ACCEPT_SUB), {}, MenuCode::AutoAccept, true,
         BLESHARE_STORE.isAutoAccept());

  addRow(tr(STR_TELEGRAM_DEST_FOLDER), tr(STR_TELEGRAM_DEST_FOLDER_SUB), BLESHARE_STORE.getDestFolder(),
         MenuCode::DestFolder);

  if (BLESHARE_STORE.isPaired()) {
    addRow(tr(STR_BLE_SHARE_FORGET_PHONE), BLESHARE_STORE.getPairedPhoneName(), {}, MenuCode::ForgetPhone);
  }

  if (nav.selected >= static_cast<int>(rows_.size())) {
    nav.selected = std::max(0, static_cast<int>(rows_.size()) - 1);
  }
}

void BleShareActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rows_.data();
  props.count = static_cast<uint16_t>(rows_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void BleShareActivity::activateIndex(const int index) {
  if (view_ != View::Menu || index < 0 || index >= static_cast<int>(rows_.size())) return;
  nav.selected = index;
  app.clearTapFlash();
  switch (static_cast<MenuCode>(rows_[index].actionValue)) {
    case MenuCode::PairPhone:
      bleshare::startPairingMode();
      wasPairingBeforeTick_ = true;
      view_ = View::Pairing;
      requestUpdate();
      return;
    case MenuCode::Discoverable:
      BLESHARE_STORE.setEnabled(!BLESHARE_STORE.isEnabled());
      BLESHARE_STORE.saveToFile();
      if (!BLESHARE_STORE.isEnabled()) {
        bleshare::stopRadio();
      }
      break;
    case MenuCode::AutoAccept:
      BLESHARE_STORE.setAutoAccept(!BLESHARE_STORE.isAutoAccept());
      BLESHARE_STORE.saveToFile();
      break;
    case MenuCode::DestFolder:
      BLESHARE_STORE.setDestFolder(BLESHARE_STORE.getDestFolder() == "/Books" ? "/zh" : "/Books");
      BLESHARE_STORE.saveToFile();
      break;
    case MenuCode::ForgetPhone:
      BLESHARE_STORE.clearPairing();
      bleshare::cancelTransfer();
      break;
  }
  rebuildMenuRows();
  requestUpdate();
}

void BleShareActivity::openIncomingPopup() {
  const auto& offer = bleshare::currentOffer();
  char titleBuf[96];
  std::snprintf(titleBuf, sizeof(titleBuf), tr(STR_BLE_SHARE_INCOMING_FMT),
                offer.phoneName.empty() ? "Android" : offer.phoneName.c_str());

  char sizeBuf[32];
  formatBytes(offer.totalBytes, sizeBuf, sizeof(sizeBuf));

  char msgBuf[192];
  if (offer.fileCount > 1) {
    std::snprintf(msgBuf, sizeof(msgBuf), "%s (+%u more \xC2\xB7 %s)", offer.firstFileName.c_str(),
                  static_cast<unsigned>(offer.fileCount - 1), sizeBuf);
  } else {
    std::snprintf(msgBuf, sizeof(msgBuf), "%s (%s)", offer.firstFileName.c_str(), sizeBuf);
  }

  const char* opts[2] = {tr(STR_BLE_SHARE_ACCEPT), tr(STR_BLE_SHARE_LATER)};
  popup_.showMessage(titleBuf, msgBuf, opts, 2, 0, [this](const int idx) {
    incomingPopupShown_ = false;
    if (idx == 0) {
      bleshare::respondToOffer(true);
      view_ = View::Transferring;
      lastRenderedBytes_ = 0;
      lastRenderedFileIdx_ = 0xFFFF;
      requestUpdate();
    } else {
      bleshare::respondToOffer(false);
      bleshare::clearCompletedState();
      if (backgroundLaunch_) {
        exitAfterBackgroundSession(false);
      } else {
        view_ = View::Menu;
        requestUpdate();
      }
    }
  });
  incomingPopupShown_ = true;
}

void BleShareActivity::openDonePopup() {
  const auto& offer = bleshare::currentOffer();
  char titleBuf[96];
  std::snprintf(titleBuf, sizeof(titleBuf), tr(STR_BLE_SHARE_COMPLETE_FMT),
                static_cast<unsigned>(bleshare::savedPaths().size()), offer.destFolder.c_str());

  std::string body;
  for (size_t i = 0; i < std::min<size_t>(bleshare::savedPaths().size(), 3); ++i) {
    if (!body.empty()) body += " \xC2\xB7 ";
    const std::string& p = bleshare::savedPaths()[i];
    const size_t slash = p.rfind('/');
    body += (slash != std::string::npos) ? p.substr(slash + 1) : p;
  }

  if (!returnBookPath_.empty()) {
    const char* opts[2] = {tr(STR_BLE_SHARE_RESUME_BOOK), tr(STR_BLE_SHARE_OPEN_LIBRARY)};
    popup_.showMessage(titleBuf, body.c_str(), opts, 2, 0, [this](const int idx) {
      donePopupShown_ = false;
      bleshare::clearCompletedState();
      exitAfterBackgroundSession(/*openLibrary=*/idx == 1);
    });
  } else {
    const char* opts[2] = {tr(STR_BLE_SHARE_OPEN_LIBRARY), tr(STR_BACK)};
    popup_.showMessage(titleBuf, body.c_str(), opts, 2, 0, [this](const int idx) {
      donePopupShown_ = false;
      bleshare::clearCompletedState();
      if (idx == 0) {
        activityManager.goToLibrary();
      } else if (backgroundLaunch_) {
        exitAfterBackgroundSession(false);
      } else {
        view_ = View::Menu;
        rebuildMenuRows();
        requestUpdate();
      }
    });
  }
  donePopupShown_ = true;
}

void BleShareActivity::exitAfterBackgroundSession(const bool openLibrary) {
  if (openLibrary) {
    activityManager.goToLibrary();
    return;
  }
  if (!returnBookPath_.empty()) {
    bleshare::stopRadio();
    activityManager.goToReader(returnBookPath_, true);
    return;
  }
  activityManager.goHome();
}

void BleShareActivity::syncSessionToUi() {
  const auto st = bleshare::sessionState();

  if (view_ == View::Pairing && wasPairingBeforeTick_ && !bleshare::isPairingActive() && BLESHARE_STORE.isPaired()) {
    wasPairingBeforeTick_ = false;
    view_ = View::Menu;
    rebuildMenuRows();
    requestUpdate();
  }

  switch (st) {
    case xpbt::SessionState::OfferPendingDecision:
      if (!incomingPopupShown_) {
        openIncomingPopup();
        requestUpdate();
      }
      break;
    case xpbt::SessionState::ReadyForFiles:
    case xpbt::SessionState::ReceivingFile: {
      if (popup_.isActive()) {
        popup_.dismiss();
        incomingPopupShown_ = false;
      }
      if (view_ != View::Transferring) {
        view_ = View::Transferring;
        lastProgressPaintMs_ = millis();
        requestUpdate();
      } else {
        const auto& prog = bleshare::currentProgress();
        const unsigned long now = millis();
        if (prog.fileIndex != lastRenderedFileIdx_ ||
            (now - lastProgressPaintMs_ >= 400 && prog.receivedBytes != lastRenderedBytes_)) {
          lastRenderedFileIdx_ = prog.fileIndex;
          lastRenderedBytes_ = prog.receivedBytes;
          lastProgressPaintMs_ = now;
          requestUpdate();
        }
      }
      break;
    }
    case xpbt::SessionState::Completed:
      if (view_ != View::Done || !donePopupShown_) {
        view_ = View::Done;
        openDonePopup();
        requestUpdate();
      }
      break;
    case xpbt::SessionState::Deferred:
      bleshare::clearCompletedState();
      if (backgroundLaunch_) {
        exitAfterBackgroundSession(false);
      } else if (view_ != View::Menu) {
        view_ = View::Menu;
        requestUpdate();
      }
      break;
    case xpbt::SessionState::Error:
      if (view_ != View::Error) {
        popup_.dismiss();
        view_ = View::Error;
        requestUpdate();
      }
      break;
    default:
      break;
  }
}

bool BleShareActivity::handleCustomInput() {
  if (popup_.isActive()) {
    const bool wasOpen = popup_.isActive();
    const bool handled = popup_.handleInput(mappedInput, [this] { requestUpdate(); });
    if (wasOpen && !popup_.isActive() && incomingPopupShown_) {
      // Back pressed on the incoming popup counts as Later/Defer.
      incomingPopupShown_ = false;
      bleshare::respondToOffer(false);
      bleshare::clearCompletedState();
      if (backgroundLaunch_) {
        exitAfterBackgroundSession(false);
      } else {
        view_ = View::Menu;
        requestUpdate();
      }
    } else if (wasOpen && !popup_.isActive() && donePopupShown_) {
      donePopupShown_ = false;
      bleshare::clearCompletedState();
      if (backgroundLaunch_) {
        exitAfterBackgroundSession(false);
      } else {
        view_ = View::Menu;
        rebuildMenuRows();
        requestUpdate();
      }
    }
    return handled;
  }

  if (view_ != View::Menu) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onBackButton();
      return true;
    }
    if (view_ == View::Error && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      onBackButton();
      return true;
    }
    return true;
  }
  return false;
}

void BleShareActivity::onBackButton() {
  if (view_ == View::Pairing) {
    bleshare::stopPairingMode();
    wasPairingBeforeTick_ = false;
    view_ = View::Menu;
    rebuildMenuRows();
    requestUpdate();
    return;
  }
  if (view_ == View::Transferring) {
    bleshare::cancelTransfer();
    if (backgroundLaunch_) {
      exitAfterBackgroundSession(false);
    } else {
      view_ = View::Menu;
      requestUpdate();
    }
    return;
  }
  if (view_ == View::Error || view_ == View::Done) {
    bleshare::clearCompletedState();
    if (backgroundLaunch_) {
      exitAfterBackgroundSession(false);
    } else {
      view_ = View::Menu;
      rebuildMenuRows();
      requestUpdate();
    }
    return;
  }
  if (backgroundLaunch_) {
    exitAfterBackgroundSession(false);
    return;
  }
  finish();
}

void BleShareActivity::loop() {
  syncSessionToUi();
  UiListActivity::loop();
}

void BleShareActivity::renderPairingCard() const {
  const Rect area = UITheme::getContentArea(renderer);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = metrics.topPadding + metrics.headerHeight + 24;
  const int cardW = std::min(area.width - 48, 420);
  const int cardH = 210;
  const int cardX = area.x + (area.width - cardW) / 2;
  const int cardY = top + std::max(12, (area.height - top - metrics.buttonHintsHeight - cardH) / 2);

  renderer.drawRect(cardX, cardY, cardW, cardH, true);
  renderer.drawRect(cardX + 1, cardY + 1, cardW - 2, cardH - 2, true);

  UITheme::drawCenteredText(renderer, Rect{cardX + 12, cardY + 18, cardW - 24, 28}, UI_12_FONT_ID, cardY + 18,
                            bleshare::deviceDisplayName(), true, EpdFontFamily::BOLD);

  UITheme::drawCenteredWrappedText(renderer, Rect{cardX + 16, cardY + 52, cardW - 32, 48}, UI_10_FONT_ID,
                                   tr(STR_BLE_SHARE_PAIR_HEADING), 2);

  const char* rawPin = bleshare::pairingPin();
  char formattedPin[16];
  std::snprintf(formattedPin, sizeof(formattedPin), "%c%c%c  %c%c%c", rawPin[0], rawPin[1], rawPin[2], rawPin[3],
                rawPin[4], rawPin[5]);

  const int pinBoxW = std::min(cardW - 48, 240);
  const int pinBoxH = 58;
  const int pinBoxX = cardX + (cardW - pinBoxW) / 2;
  const int pinBoxY = cardY + 118;
  renderer.fillRect(pinBoxX, pinBoxY, pinBoxW, pinBoxH, true);
  UITheme::drawCenteredText(renderer, Rect{pinBoxX, pinBoxY, pinBoxW, pinBoxH}, NOTOSANS_18_FONT_ID, pinBoxY + 14,
                            formattedPin, false, EpdFontFamily::BOLD);
}

void BleShareActivity::renderTransferCard() const {
  const Rect area = UITheme::getContentArea(renderer);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& prog = bleshare::currentProgress();

  const int cardW = std::min(area.width - 48, 430);
  const int cardH = 190;
  const int top = metrics.topPadding + metrics.headerHeight + 20;
  const int cardX = area.x + (area.width - cardW) / 2;
  const int cardY = top + std::max(12, (area.height - top - metrics.buttonHintsHeight - cardH) / 2);

  renderer.drawRect(cardX, cardY, cardW, cardH, true);
  renderer.drawRect(cardX + 1, cardY + 1, cardW - 2, cardH - 2, true);

  char headerBuf[64];
  std::snprintf(headerBuf, sizeof(headerBuf), tr(STR_BLE_SHARE_RECEIVING_FMT),
                static_cast<unsigned>(prog.fileIndex + 1), static_cast<unsigned>(std::max<uint16_t>(1, prog.fileCount)));
  UITheme::drawCenteredText(renderer, Rect{cardX + 16, cardY + 20, cardW - 32, 26}, UI_12_FONT_ID, cardY + 20,
                            headerBuf, true, EpdFontFamily::BOLD);

  const char* fname = prog.fileName.empty() ? "..." : prog.fileName.c_str();
  UITheme::drawCenteredWrappedText(renderer, Rect{cardX + 16, cardY + 56, cardW - 32, 48}, UI_10_FONT_ID, fname, 2);

  const int barX = cardX + 28;
  const int barY = cardY + 120;
  const int barW = cardW - 56;
  const int barH = 18;
  renderer.drawRect(barX, barY, barW, barH, true);
  const uint64_t denom = prog.batchTotalBytes > 0 ? prog.batchTotalBytes : std::max<uint32_t>(1, prog.fileSize);
  const uint64_t numer = prog.batchTotalBytes > 0 ? prog.batchReceivedBytes : prog.receivedBytes;
  const int pct = static_cast<int>(std::min<uint64_t>(100, (numer * 100ULL) / denom));
  const int fillW = ((barW - 4) * pct) / 100;
  if (fillW > 0) {
    renderer.fillRect(barX + 2, barY + 2, fillW, barH - 4, true);
  }

  char rxBuf[24];
  char totBuf[24];
  formatBytes(numer, rxBuf, sizeof(rxBuf));
  formatBytes(denom, totBuf, sizeof(totBuf));
  char statBuf[72];
  std::snprintf(statBuf, sizeof(statBuf), "%d%%  (%s / %s)", pct, rxBuf, totBuf);
  UITheme::drawCenteredText(renderer, Rect{cardX + 16, barY + 26, cardW - 32, 24}, SMALL_FONT_ID, barY + 26, statBuf);
}

void BleShareActivity::renderDoneCard() const {
  const Rect area = UITheme::getContentArea(renderer);
  const int cy = area.y + area.height / 2 - 20;
  char msg[96];
  std::snprintf(msg, sizeof(msg), tr(STR_BLE_SHARE_COMPLETE_FMT), static_cast<unsigned>(bleshare::savedPaths().size()),
                bleshare::currentOffer().destFolder.c_str());
  UITheme::drawCenteredText(renderer, area, UI_12_FONT_ID, cy, msg, true, EpdFontFamily::BOLD);
}

void BleShareActivity::renderErrorCard() const {
  const Rect area = UITheme::getContentArea(renderer);
  const char* err = bleshare::errorMessage().empty() ? "Transfer interrupted" : bleshare::errorMessage().c_str();
  UITheme::drawCenteredWrappedText(renderer, Rect{area.x + 24, area.y + area.height / 3, area.width - 48, 120},
                                   UI_12_FONT_ID, err, 3, true, EpdFontFamily::BOLD);
}

void BleShareActivity::render(RenderLock&& lock) {
  if (view_ == View::Menu && !popup_.isActive()) {
    UiListActivity::render(std::move(lock));
    return;
  }

  renderer.clearScreen();
  drawChrome();
  switch (view_) {
    case View::Menu:
      if (!backgroundLaunch_ || !popup_.isActive()) {
        renderUi();
      }
      break;
    case View::Pairing:
      renderPairingCard();
      break;
    case View::Transferring:
      renderTransferCard();
      break;
    case View::Done:
      if (!popup_.isActive()) {
        renderDoneCard();
      }
      break;
    case View::Error:
      renderErrorCard();
      break;
  }

  if (popup_.processRender(renderer, mappedInput)) {
    return;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), view_ == View::Error ? tr(STR_CONFIRM) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(view_ == View::Transferring ? HalDisplay::FAST_REFRESH : HalDisplay::HALF_REFRESH);
}
