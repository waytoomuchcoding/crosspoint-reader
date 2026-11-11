#include "BookSyncService.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <TrustedTime.h>
#include <WiFi.h>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "TelegramCredentialStore.h"
#include "WifiCredentialStore.h"
#include "network/LibbyClient.h"
#include "network/TelegramBookSync.h"
#include "util/PluginEvents.h"

namespace booksync {

namespace {

constexpr uint16_t kMinSleepSyncBatteryPercent = 20;

struct ScopedRadioRestore {
  bool shouldTurnOff = false;
  ~ScopedRadioRestore() {
    if (shouldTurnOff && WiFi.getMode() != WIFI_MODE_NULL) {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
    }
  }
};

void releaseSdFontsForTls(GfxRenderer* renderer) {
  if (!renderer) return;
  if (auto* fcm = renderer->getFontCacheManager()) {
    fcm->releaseSdFontCaches();
  }
}

bool ensureWifiConnected(const std::function<void(const char*)>& statusCb, std::string& errOut) {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  WIFI_STORE.loadFromFile();
  std::optional<WifiCredential> cred = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!cred && WIFI_STORE.getCredentialCount() > 0) {
    cred = WIFI_STORE.getCredentialAt(0);
  }
  if (!cred || cred->ssid.empty()) {
    errOut = tr(STR_WIFI_CONN_FAILED);
    return false;
  }

  if (statusCb) {
    statusCb(tr(STR_SYNC_CONNECTING_WIFI));
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  const unsigned long deadline = millis() + 12000;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(100);
  }

  if (WiFi.status() != WL_CONNECTED) {
    errOut = tr(STR_WIFI_CONN_FAILED);
    return false;
  }

  WIFI_STORE.setLastConnectedSsid(cred->ssid);
  WIFI_STORE.saveToFile();
  return true;
}

}  // namespace

bool hasConfiguredCloudSync() {
  TELEGRAM_STORE.loadFromFile();
  return TELEGRAM_STORE.hasCredentials() || libby::hasToken();
}

bool wantsSleepAutoSync() {
  TELEGRAM_STORE.loadFromFile();
  if (!TELEGRAM_STORE.getAutoSyncOnSleep()) return false;
  return TELEGRAM_STORE.hasCredentials() || libby::hasToken();
}

void runSleepAutoSync(GfxRenderer* renderer) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (powerManager.getBatteryPercentage() < kMinSleepSyncBatteryPercent) return;

  TELEGRAM_STORE.loadFromFile();
  if (!TELEGRAM_STORE.getAutoSyncOnSleep()) return;

  const bool wantsTelegramSync = TELEGRAM_STORE.hasCredentials();
  const bool wantsLibbySync = libby::hasToken();
  if (!wantsTelegramSync && !wantsLibbySync) return;

  releaseSdFontsForTls(renderer);

  if (wantsTelegramSync) {
    telegram::SyncSummary summary{};
    std::string err;
    telegram::syncPendingFiles(summary, err);
  }
  if (wantsLibbySync) {
    int fulfilled = 0;
    std::string err;
    libby::syncAndFulfillNewLoans(fulfilled, err);
  }
}

bool syncAll(GfxRenderer* renderer, SyncAllSummary& summaryOut, const std::function<void(const char*)>& statusCb) {
  summaryOut = {};

  TELEGRAM_STORE.loadFromFile();
  const bool hasTelegram = TELEGRAM_STORE.hasCredentials();
  const bool hasLibby = libby::hasToken();
  if (!hasTelegram && !hasLibby) {
    summaryOut.errorMessage = tr(STR_SYNC_NOTHING_CONFIGURED);
    return false;
  }

  const wifi_mode_t initialMode = WiFi.getMode();
  const bool radioAlreadyActive =
      (initialMode != WIFI_MODE_NULL) && (WiFi.status() == WL_CONNECTED || (initialMode & WIFI_MODE_AP) != 0);
  ScopedRadioRestore radioGuard{!radioAlreadyActive};

  if (!ensureWifiConnected(statusCb, summaryOut.errorMessage)) {
    return false;
  }
  summaryOut.wifiConnected = true;
  trustedtime::startSync();
  releaseSdFontsForTls(renderer);

  std::string lastErr;
  if (hasTelegram) {
    if (statusCb) statusCb(tr(STR_TELEGRAM_SYNCING));
    telegram::SyncSummary tg{};
    std::string tgErr;
    if (telegram::syncPendingFiles(tg, tgErr)) {
      summaryOut.ranTelegram = true;
      summaryOut.telegramBooks = tg.filesDownloaded;
      summaryOut.telegramCovers = tg.coversBound;
    } else {
      LOG_ERR("SYNC", "Telegram sync failed: %s", tgErr.c_str());
      lastErr = tgErr;
    }
  }

  if (hasLibby) {
    if (statusCb) statusCb(tr(STR_LIBBY_SYNC_LOANS));
    int fulfilled = 0;
    std::string lbyErr;
    if (libby::syncAndFulfillNewLoans(fulfilled, lbyErr)) {
      summaryOut.ranLibby = true;
      summaryOut.libbyLoansFulfilled = fulfilled;
    } else {
      LOG_ERR("SYNC", "Libby sync failed: %s", lbyErr.c_str());
      if (lastErr.empty()) lastErr = lbyErr;
    }
  }

  if (renderer) {
    pluginevents::drain(renderer);
  }

  const bool changed = (summaryOut.telegramBooks + summaryOut.telegramCovers + summaryOut.libbyLoansFulfilled) > 0 ||
                       library::isLibraryIndexDirty();
  if (changed) {
    if (statusCb) statusCb(tr(STR_LIBRARY_REBUILDING));
    library::BuildStats stats;
    library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0);
  }

  if (!summaryOut.ranTelegram && !summaryOut.ranLibby && !lastErr.empty()) {
    summaryOut.errorMessage = lastErr;
    return false;
  }
  return true;
}

}  // namespace booksync
