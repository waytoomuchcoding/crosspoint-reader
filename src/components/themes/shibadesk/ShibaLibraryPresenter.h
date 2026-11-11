#pragma once

#include <FreeInkUI.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "components/UiAppHost.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/shibadesk/ShibaArt.h"
#include "network/LibbyClient.h"
#include "util/LoanBadgeUtils.h"

class GfxRenderer;
class MappedInputManager;

namespace shibadesk {

struct ShibaLibraryViewContext {
  void* user = nullptr;
  int totalIndexedBooks = 0;
  int bookRowCount = 0;
  int listCount = 0;
  int activeTab = 0;  // 0 = Recent, 1 = Title, 2 = Author
  int selectedEntry = 0;
  uint8_t sortOrderRaw = 0;
  bool isDescending = true;
  bool tabsFocused = true;
  bool degraded = false;
  bool groupsCollapsed = false;
  bool filterFailed = false;
  const std::string* query = nullptr;
  const char* holdHelpText = nullptr;
  const char* backHintLabel = nullptr;
  const char* confirmHintLabel = nullptr;
  const char* prevHintLabel = nullptr;
  const char* nextHintLabel = nullptr;
  freeink::ui::ListNav* nav = nullptr;
  freeink::ui::ActionId actionTab = 0;
  freeink::ui::ActionId actionSearch = 0;
  freeink::ui::ActionId actionRow = 0;
  freeink::ui::ActionId actionBack = 0;

  bool (*resolveBookRow)(void* user, int entry, std::string& pathOut, std::string& titleOut,
                         std::string& authorOut) = nullptr;
  bool (*resolveCollapsedGroup)(void* user, int groupEntry, std::string& headingOut, int& countOut) = nullptr;
  void (*closeIndexTemporarily)(void* user) = nullptr;
  void (*reopenIndex)(void* user) = nullptr;
};

class ShibaLibraryPresenter {
 public:
  static constexpr int MINI_COVER_W = 52;
  static constexpr int MINI_COVER_H = 78;
  static constexpr size_t MAX_MINI_SNAPSHOTS = 6;
  static constexpr size_t MAX_WINDOW_ITEMS = 12;

  ShibaLibraryPresenter(GfxRenderer& renderer, const MappedInputManager& mappedInput);

  void onEnter();
  void invalidateCaches();

  using UiScreen = UiAppHost::UiScreen;

  void buildScreen(UiScreen& screen, const ShibaLibraryViewContext& ctx);
  void drawFooter(const ShibaLibraryViewContext& ctx) const;

 private:
  struct WindowItem {
    int entry = 0;
    std::string path;
    std::string title;
    std::string author;
    std::string extTag;
    std::string groupHeading;
    std::string miniThumbPath;
    loanbadge::LoanBadgeInfo loanBadge;
    uint8_t progressPercent = 0;
    int groupBookCount = 0;
    bool hasProgress = false;
    bool isTxtOrMd = false;
    bool hasCustomTxtCover = false;
    bool startsGroup = false;
  };

  struct MiniCoverSnapshot {
    std::string path;
    Rect rect;
    std::unique_ptr<uint8_t[]> bytes;
    size_t byteLen = 0;
    bool inverted = false;
    bool valid = false;
  };

  void drawFilterStrip(UiScreen& screen, const ShibaLibraryViewContext& ctx, int leftX, int rightX, int ledgeY);
  void drawEmptyState(const ShibaLibraryViewContext& ctx, int leftX, int rightX, int topY, int bottomY) const;
  void buildMiniCoverList(UiScreen& screen, const ShibaLibraryViewContext& ctx, int leftX, int rightX, int topY,
                          int bottomY);
  void buildAuthorSpineList(UiScreen& screen, const ShibaLibraryViewContext& ctx, int leftX, int rightX, int topY,
                            int bottomY);
  void buildCollapsedGroupList(UiScreen& screen, const ShibaLibraryViewContext& ctx, int leftX, int rightX, int topY,
                               int bottomY);

  bool matchesCachedWindow(const ShibaLibraryViewContext& ctx, int top, int count) const;
  void populateBookItemMeta(WindowItem& item, const ShibaLibraryViewContext& ctx);
  void paintOrRestoreMiniCover(size_t slotIdx, const WindowItem& item, int x, int y);

  GfxRenderer& renderer_;
  const MappedInputManager& mappedInput_;

  std::vector<libby::Loan> cachedLoans_;
  bool loansLoaded_ = false;

  ShibaPose currentPose_ = ShibaPose::LoafAwake;
  int lastPoseTab_ = -1;

  std::vector<WindowItem> windowItems_;
  int cachedTop_ = -1;
  int cachedDrawn_ = 0;
  int cachedListCount_ = -1;
  int cachedTab_ = -1;
  uint8_t cachedSortRaw_ = 0xFF;
  bool cachedCollapsed_ = false;
  std::string cachedQuery_;

  int lastRenderedTop_ = 0;
  int lastRenderedDrawn_ = 1;

  std::array<MiniCoverSnapshot, MAX_MINI_SNAPSHOTS> miniSnapshots_;
};

}  // namespace shibadesk
