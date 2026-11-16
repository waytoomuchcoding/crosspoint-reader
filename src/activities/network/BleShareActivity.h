#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

class BleShareActivity final : public UiListActivity {
 public:
  explicit BleShareActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool backgroundLaunch = false,
                            std::string returnBookPath = {});

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool skipLoopDelay() override;
  bool preventAutoSleep() override { return true; }

 private:
  enum class View : uint8_t { Menu, Pairing, Transferring, Done, Error };

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  void onBackButton() override;
  const char* headerTitle() const override;

  void rebuildMenuRows();
  void syncSessionToUi();
  void openIncomingPopup();
  void openDonePopup();
  void exitAfterBackgroundSession(bool openLibrary);

  void renderPairingCard() const;
  void renderTransferCard() const;
  void renderDoneCard() const;
  void renderErrorCard() const;

  bool backgroundLaunch_ = false;
  std::string returnBookPath_;
  View view_ = View::Menu;

  std::vector<freeink::ui::ListItem> rows_;
  std::vector<std::string> subtitles_;
  std::vector<std::string> values_;

  OptionPopup popup_;
  bool incomingPopupShown_ = false;
  bool donePopupShown_ = false;
  bool wasPairingBeforeTick_ = false;
  uint32_t lastRenderedBytes_ = 0;
  uint16_t lastRenderedFileIdx_ = 0xFFFF;
  unsigned long lastProgressPaintMs_ = 0;
};
