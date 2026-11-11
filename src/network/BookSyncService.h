#pragma once

#include <functional>
#include <string>

class GfxRenderer;

namespace booksync {

struct SyncAllSummary {
  bool wifiConnected = false;
  bool ranTelegram = false;
  bool ranLibby = false;
  int telegramBooks = 0;
  int telegramCovers = 0;
  int libbyLoansFulfilled = 0;
  std::string errorMessage;
};

// Returns true when either Telegram Bot credentials or a Libby account token is configured.
bool hasConfiguredCloudSync();

// Returns true when auto-sync on sleep is enabled and at least one cloud sync service is configured.
bool wantsSleepAutoSync();

// Runs configured cloud syncs during sleep entry if battery >= 20% and Wi-Fi is connected.
void runSleepAutoSync(GfxRenderer* renderer);

// Brings up Wi-Fi STA if needed (restoring radio-off state on exit when it was previously off),
// syncs trusted time, pulls pending Telegram files & TXT cover photos, refreshes & auto-fulfills
// active Libby EPUB loans, drains queued plugin events, and reconciles library.idx.
bool syncAll(GfxRenderer* renderer, SyncAllSummary& summaryOut,
             const std::function<void(const char*)>& statusCb = nullptr);

}  // namespace booksync
