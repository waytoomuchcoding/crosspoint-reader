#pragma once

#include <string>
#include <vector>

#include "activities/CatalogActivity.h"
#include "network/TelegramBookSync.h"

class TelegramSyncActivity final : public CatalogActivity {
 public:
  TelegramSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : CatalogActivity("TelegramSync", renderer, mappedInput) {}

  void onEnter() override;

 private:
  void startBrowse() override;
  void retryBrowse() override;
  bool hasSearch() const override { return false; }
  void performSearch(const std::string&) override {}
  void downloadFinished(bool cancelled) override;
  void onBackButton() override;

  int listCount() const override;
  void activateIndex(int index) override;
  bool handleButtons() override;
  void buildScreen(UiScreen& screen) override;
  void drawFooter() override;
  void rebuildRows();

  telegram::SyncSummary lastSummary_{};
  std::string lastStatusBanner_;
  std::vector<std::string> rowSubtitles_;
  std::vector<std::string> rowValues_;
  std::vector<freeink::ui::ListItem> rowItems_;
  bool rowsDirty_ = true;
  unsigned long lastProgressMs_ = 0;
  int lastProgressPct_ = -1;
};
