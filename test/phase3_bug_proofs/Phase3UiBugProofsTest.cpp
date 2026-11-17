#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "GfxRenderer.h"
#include "HalGPIO.h"
#include "MappedInputManager.h"

#define private public
#include "activities/reader/EpubReaderMenuActivity.h"
#undef private

namespace {

class Phase3UiBugProofsTest : public ::testing::Test {
 protected:
  HalGPIO gpio;
  GfxRenderer renderer;
  MappedInputManager input{gpio, renderer};

  void SetUp() override {
    SETTINGS.orientation = CrossPointSettings::PORTRAIT;
    SETTINGS.frontButtonFollowOrientation = 0;
    SETTINGS.sideButtonFollowOrientation = 0;
    SETTINGS.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
    SETTINGS.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
    SETTINGS.frontButtonLeft = CrossPointSettings::FRONT_HW_LEFT;
    SETTINGS.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
    gpio = HalGPIO{};
    renderer.orientation = GfxRenderer::Portrait;
  }
};

TEST_F(Phase3UiBugProofsTest, Bug5_ZhRubyAdjustNavNextPrevFollowOrientationAndSideButtons) {
  // With ButtonNavigator (`onPrevious` -> NavPrevious, `onNext` -> NavNext),
  // in PortraitInverted front BTN_LEFT is labeled "Less" and triggers NavNext (++position, less ruby),
  // front BTN_RIGHT is labeled "More" and triggers NavPrevious (--position, more ruby),
  // and side BTN_UP/BTN_DOWN trigger NavNext / NavPrevious consistently with side inversion.
  SETTINGS.frontButtonFollowOrientation = 1;
  renderer.orientation = GfxRenderer::PortraitInverted;

  const auto labels = input.mapLabels("Back", "Select", "More", "Less");
  EXPECT_STREQ(labels.btn3, "Less");
  EXPECT_STREQ(labels.btn4, "More");

  gpio.pressedBtn = HalGPIO::BTN_LEFT;
  EXPECT_TRUE(input.wasPressed(MappedInputManager::Button::NavNext));
  EXPECT_FALSE(input.wasPressed(MappedInputManager::Button::NavPrevious));

  gpio.pressedBtn = HalGPIO::BTN_RIGHT;
  EXPECT_TRUE(input.wasPressed(MappedInputManager::Button::NavPrevious));
  EXPECT_FALSE(input.wasPressed(MappedInputManager::Button::NavNext));

  gpio.pressedBtn = HalGPIO::BTN_UP;
  EXPECT_TRUE(input.wasPressed(MappedInputManager::Button::NavNext));
  gpio.pressedBtn = HalGPIO::BTN_DOWN;
  EXPECT_TRUE(input.wasPressed(MappedInputManager::Button::NavPrevious));
}

TEST_F(Phase3UiBugProofsTest, Bug6a_ZhLookupLandscapeDirectionalAndJumpMappingMatchInput) {
  // Cursor/Popup use mapDirectionalLabels matching Screen* buttons;
  // Jump mode labels front buttons "1","2","3","4" and polls front Left/Right swapped by isNavDirectionSwapped().
  SETTINGS.frontButtonFollowOrientation = 1;
  renderer.orientation = GfxRenderer::LandscapeClockwise;

  const auto cursorLabels = input.mapDirectionalLabels("Back", "Look Up", "Word", ">", "Up", "Down");
  EXPECT_STREQ(cursorLabels.btn3, "Up");
  EXPECT_STREQ(cursorLabels.btn4, "Down");
  gpio.pressedBtn = HalGPIO::BTN_LEFT;
  EXPECT_TRUE(input.wasPressed(MappedInputManager::Button::ScreenUp));
  gpio.pressedBtn = HalGPIO::BTN_RIGHT;
  EXPECT_TRUE(input.wasPressed(MappedInputManager::Button::ScreenDown));

  const auto jumpLabels = input.mapLabels("1", "2", "3", "4");
  EXPECT_FALSE(input.isNavDirectionSwapped());
  EXPECT_STREQ(jumpLabels.btn3, "3");
  EXPECT_STREQ(jumpLabels.btn4, "4");
  const auto jumpB3 = input.isNavDirectionSwapped() ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto jumpB4 = input.isNavDirectionSwapped() ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;
  gpio.pressedBtn = HalGPIO::BTN_LEFT;
  EXPECT_TRUE(input.wasPressed(jumpB3));
  gpio.pressedBtn = HalGPIO::BTN_RIGHT;
  EXPECT_TRUE(input.wasPressed(jumpB4));

  renderer.orientation = GfxRenderer::LandscapeCounterClockwise;
  EXPECT_TRUE(input.isNavDirectionSwapped());
  const auto jumpCcwLabels = input.mapLabels("1", "2", "3", "4");
  EXPECT_STREQ(jumpCcwLabels.btn3, "4");
  EXPECT_STREQ(jumpCcwLabels.btn4, "3");
  const auto ccwB3 = input.isNavDirectionSwapped() ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto ccwB4 = input.isNavDirectionSwapped() ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;
  gpio.pressedBtn = HalGPIO::BTN_RIGHT;
  EXPECT_TRUE(input.wasPressed(ccwB3));
  gpio.pressedBtn = HalGPIO::BTN_LEFT;
  EXPECT_TRUE(input.wasPressed(ccwB4));
}

TEST_F(Phase3UiBugProofsTest, Bug8_ReaderMenuMaxMenuItemsFitsAll21EnumActions) {
  const size_t totalMenuActions = static_cast<size_t>(EpubReaderMenuActivity::MenuAction::TEXT_ENCODING) + 1;
  EXPECT_EQ(totalMenuActions, 21u);
  EXPECT_GE(EpubReaderMenuActivity::MAX_MENU_ITEMS, totalMenuActions);
  EXPECT_GE(sizeof(EpubReaderMenuActivity::menuRowItems) / sizeof(freeink::ui::ListItem), totalMenuActions);
}

}  // namespace
