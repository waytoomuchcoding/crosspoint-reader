#include "BookSyncService.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <WiFi.h>

#include <ctime>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "TelegramCredentialStore.h"
#include "network/LibbyClient.h"
#include "network/TelegramBookSync.h"

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

void releaseSdFontsForTls(GfxRenderer* renderer) {
  if (!renderer) return;
  if (auto* fcm = renderer->getFontCacheManager()) {
    fcm->releaseSdFontCaches();
  }
}

}  // namespace

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

}  // namespace booksync
