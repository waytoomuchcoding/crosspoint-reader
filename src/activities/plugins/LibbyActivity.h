#pragma once

#include <string>
#include <vector>

#include "activities/CatalogActivity.h"
#include "network/LibbyClient.h"

class LibbyActivity final : public CatalogActivity {
 public:
  LibbyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : CatalogActivity("Libby", renderer, mappedInput) {}

  void onEnter() override;

 private:
  enum class PendingWifiOp : uint8_t {
    None = 0,
    PairCode = 1,
    SyncLoans = 2,
    DownloadLoan = 3,
  };

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
  void promptSetupCode();
  void runPendingWifiOp();
  void pumpTransferProgress(size_t dl, size_t tot);

  std::vector<libby::Loan> loans_;
  std::vector<std::string> rowSubtitles_;
  std::vector<freeink::ui::ListItem> rowItems_;
  bool rowsDirty_ = true;
  bool hasAccountToken_ = false;
  PendingWifiOp pendingOp_ = PendingWifiOp::None;
  PendingWifiOp lastAttemptedOp_ = PendingWifiOp::SyncLoans;
  std::string pendingSetupCode_;
  int pendingLoanIndex_ = -1;
  unsigned long lastProgressMs_ = 0;
  int lastProgressPct_ = -1;
};
