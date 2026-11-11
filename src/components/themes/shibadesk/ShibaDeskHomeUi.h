#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/OptionPopup.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/shibadesk/ShibaArt.h"
#include "network/LibbyClient.h"
#include "util/ButtonNavigator.h"
#include "util/LoanBadgeUtils.h"

class Activity;
class GfxRenderer;
class MappedInputManager;
class SdCardFont;

class ShibaDeskHomeUi final {
 public:
  static constexpr int MAX_BOOKS = 4;  // 1 Hero + up to 3 Recent Shelf books
  static constexpr int DOCK_SLOTS = 4;

  ShibaDeskHomeUi(Activity& hostActivity, GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~ShibaDeskHomeUi();

  void begin(std::vector<RecentBook>& books, bool hasPlugins, bool hasOpdsServers,
             std::function<void()> onReloadRequested);

  int menuItemCount() const { return static_cast<int>(slots_.size()) + DOCK_SLOTS; }
  bool hasPopupActive() const { return popup_.isActive(); }

  // Handles buttons, long-presses, and touch input; returns true if handled.
  bool handleInput(int& selectorIndex, ButtonNavigator& buttonNavigator);

  // Generates exact-size 1-bit cover thumbnails on SD once after initial paint.
  bool ensureCoversGenerated();

  void invalidateCoverSnapshots();
  void releaseTransientCaches();
  void render(int selectorIndex, bool cleanInitialRefresh, bool firstRenderDone);

 private:
  struct BookSlot {
    std::string path;
    std::string title;
    std::string author;
    std::string extTag;
    std::string exactThumbBmpPath;
    int progressPct = -1;
    int targetCoverW = 124;
    int targetCoverH = 186;
    int titleFontId = 0;
    bool useMingHero24 = false;
    std::vector<std::string> wrappedTitle;
    loanbadge::LoanBadgeInfo loanBadge;
    bool isTxtOrMd = false;
  };

  struct CoverSnapshot {
    Rect rect{0, 0, 0, 0};
    std::unique_ptr<uint8_t[]> bytes;
    size_t byteLen = 0;
    bool inverted = false;
    bool valid = false;
  };

  void buildSlotsFromBooks(const std::vector<RecentBook>& books);
  void ensureHeroMingFontLoaded();
  void unloadHeroMingFont();
  int measureHeroMingWidth(const char* text) const;
  void drawHeroMingLine(int x, int y, const char* text) const;
  void drawHeroCard(const Rect& content, int selectorIndex);
  void drawEmptyStudyDesk(const Rect& content);
  void drawRecentShelf(const Rect& content, int selectorIndex);
  void paintOrRestoreCover(size_t slotIdx, int x, int y, int w, int h);

  void activateIndex(int index);
  void openBookOptionsPopup(size_t slotIdx);
  void openSyncMenuPopup();
  void runSyncAllInteractive();

  Activity& host_;
  GfxRenderer& renderer_;
  MappedInputManager& mappedInput_;
  std::function<void()> onReloadRequested_;

  std::vector<BookSlot> slots_;
  std::vector<libby::Loan> cachedLoans_;
  std::array<CoverSnapshot, MAX_BOOKS> snapshots_;
  std::array<Rect, MAX_BOOKS> cardHitRects_;
  std::array<Rect, DOCK_SLOTS> dockHitRects_;
  Rect topSyncPillRect_{0, 0, 0, 0};

  OptionPopup popup_;
  std::unique_ptr<SdCardFont> heroMingFile_;
  int heroMingFontId_ = 0;
  int heroMingScale256_ = 256;
  shibadesk::ShibaPose pose_ = shibadesk::ShibaPose::LoafSleep;
  int totalLibraryBooks_ = 0;
  bool hasPlugins_ = false;
  bool hasOpdsServers_ = false;
  bool coversReady_ = false;
};
