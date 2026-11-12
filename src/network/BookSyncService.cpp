#include "BookSyncService.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <TrustedTime.h>
#include <WiFi.h>

#include <ctime>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "I18nKeys.h"
#include "TelegramCredentialStore.h"
#include "WifiCredentialStore.h"
#include "network/LibbyClient.h"
#include "network/TelegramBookSync.h"
#include "util/PluginEvents.h"

namespace booksync {

namespace {

constexpr uint16_t kMinSleepSyncBatteryPercent = 50;
// New sync work stops starting after this; work already started finishes.
constexpr unsigned long kSleepSyncBudgetMs = 30000;

int64_t sleepSyncIntervalSeconds() {
  switch (SETTINGS.sleepSyncInterval) {
    case CrossPointSettings::SLEEP_SYNC_EVERY:
      return 0;
    case CrossPointSettings::SLEEP_SYNC_1H:
      return 3600;
    case CrossPointSettings::SLEEP_SYNC_6H:
      return 6 * 3600;
    case CrossPointSettings::SLEEP_SYNC_12H:
      return 12 * 3600;
    case CrossPointSettings::SLEEP_SYNC_24H:
      return 24 * 3600;
    default:
      return 3 * 3600;
  }
}

// USB power always allows a sync; battery needs the setting and enough charge.
bool sleepSyncPowerAllowed() {
  if (gpio.isUsbConnected()) return true;
  return SETTINGS.sleepSyncOnBattery && powerManager.getBatteryPercentage() >= kMinSleepSyncBatteryPercent;
}

// Deep sleep keeps the system clock, so time() differences hold across sleeps.
// A clock that went backwards (power loss) counts as due.
bool sleepSyncIntervalElapsed() {
  const int64_t interval = sleepSyncIntervalSeconds();
  const int64_t last = APP_STATE.lastSleepSyncAt;
  if (interval == 0 || last == 0) return true;
  const int64_t now = static_cast<int64_t>(time(nullptr));
  return now < last || now - last >= interval;
}

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
  if (!sleepSyncIntervalElapsed() || !sleepSyncPowerAllowed()) return false;
  TELEGRAM_STORE.loadFromFile();
  if (!TELEGRAM_STORE.getAutoSyncOnSleep()) return false;
  return TELEGRAM_STORE.hasCredentials() || libby::hasToken();
}

void runSleepAutoSync(GfxRenderer* renderer) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!wantsSleepAutoSync()) return;

  const bool wantsTelegramSync = TELEGRAM_STORE.hasCredentials();
  const bool wantsLibbySync = libby::hasToken();
  const unsigned long started = millis();
  const auto outOfTime = [started] { return millis() - started >= kSleepSyncBudgetMs; };

  releaseSdFontsForTls(renderer);

  if (wantsTelegramSync) {
    telegram::SyncSummary summary{};
    std::string err;
    telegram::syncPendingFiles(summary, err, nullptr, outOfTime);
  }
  if (wantsLibbySync && !outOfTime()) {
    int fulfilled = 0;
    std::string err;
    libby::syncAndFulfillNewLoans(fulfilled, err, outOfTime);
  }
  APP_STATE.lastSleepSyncAt = static_cast<int64_t>(time(nullptr));
  APP_STATE.saveToFile();
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
