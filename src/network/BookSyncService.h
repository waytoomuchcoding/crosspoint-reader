#pragma once

class GfxRenderer;

namespace booksync {

// True when auto-sync on sleep is enabled, a cloud service is configured, the
// interval since the last run has passed and power allows it (USB, or battery
// >= 50% with Sleep sync on battery on). False means sleep should not join Wi-Fi.
bool wantsSleepAutoSync();

// Runs configured cloud syncs during sleep entry when wantsSleepAutoSync() and
// Wi-Fi is connected. New work stops starting after 30 s.
void runSleepAutoSync(GfxRenderer* renderer);

}  // namespace booksync
