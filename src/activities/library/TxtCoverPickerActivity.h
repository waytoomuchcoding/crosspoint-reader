#pragma once

#include <GfxRenderer.h>

#include <cstddef>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

class TxtCoverPickerActivity final : public Activity {
 public:
  struct ImageOption {
    std::string fullPath;  // Empty for slot 0 ("[ Default Cloth-Spine Cover ]")
    std::string displayName;
    std::string subtitle;
  };

  TxtCoverPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath)
      : Activity("TxtCoverPicker", renderer, mappedInput), bookPath_(std::move(bookPath)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct PickerLayout {
    Rect safe;
    Rect header;
    int leftX = 0;
    int rightX = 0;
    int totalW = 0;
    int summaryY = 0;
    int summaryH = 66;
    int panesTopY = 0;
    int panesH = 0;
    int leftPaneW = 0;
    int rightPaneX = 0;
    int rightPaneW = 0;
    int rowFirstY = 0;
    int rowHeight = 60;
    int rowPitch = 68;
    int visibleRows = 4;
    Rect ctaRect;
  };

  PickerLayout computeLayout() const;
  void scanImageCandidates();
  void scanDirectoryForImages(const std::string& dirPath);
  void ensureSelectedPreviewBmp();
  bool drawSelectedPreviewBmp(int covX, int covY) const;
  void confirmSelection();

  std::string bookPath_;
  std::string bookFilename_;
  std::string resolvedTitle_;
  std::string resolvedAuthor_;
  std::vector<ImageOption> options_;
  ButtonNavigator navigator_;
  int selectedIndex_ = 0;
  int previewReadyForIndex_ = -1;
  bool previewValid_ = false;
};
