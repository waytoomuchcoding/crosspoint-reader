#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include "EpdFont.h"
#include "EpdFontData.h"
#include "EpdFontFamily.h"
#include "GfxRenderer.h"
#include "HalDisplay.h"

extern "C" {
uint32_t uzlib_adler32(const void*, unsigned int, uint32_t prev_sum) { return prev_sum; }
uint32_t uzlib_crc32(const void*, unsigned int, uint32_t prev_sum) { return prev_sum; }
}

namespace {

class Phase3WrappedTextTest : public ::testing::Test {
 protected:
  HalDisplay display;
  GfxRenderer renderer{display};
  static constexpr int kFontId = 1;

  // Interval 0: ASCII 0x20..0x5A (59 glyphs, 10px advance)
  // Interval 1: CJK 0x4E00..0x4E2F (48 glyphs, 20px advance)
  // Interval 2: CJK 0x5B57..0x5B57 ('字', 1 glyph, 20px advance)
  static constexpr size_t kAsciiCount = 0x5A - 0x20 + 1;
  static constexpr size_t kCjk1Count = 0x4E2F - 0x4E00 + 1;
  static constexpr size_t kCjk2Count = 1;

  std::array<EpdGlyph, kAsciiCount + kCjk1Count + kCjk2Count> glyphs{};
  std::array<EpdUnicodeInterval, 3> intervals{};
  EpdFontData fontData{};
  EpdFont epdFont{&fontData};

  void SetUp() override {
    for (size_t i = 0; i < kAsciiCount; ++i) {
      glyphs[i] = EpdGlyph{10, 16, static_cast<uint16_t>(fp4::fromPixel(10)), 0, 16, 0, 0};
    }
    for (size_t i = 0; i < kCjk1Count + kCjk2Count; ++i) {
      glyphs[kAsciiCount + i] = EpdGlyph{20, 16, static_cast<uint16_t>(fp4::fromPixel(20)), 0, 16, 0, 0};
    }
    intervals[0] = EpdUnicodeInterval{0x20, 0x5A, 0};
    intervals[1] = EpdUnicodeInterval{0x4E00, 0x4E2F, static_cast<uint32_t>(kAsciiCount)};
    intervals[2] = EpdUnicodeInterval{0x5B57, 0x5B57, static_cast<uint32_t>(kAsciiCount + kCjk1Count)};

    fontData.glyph = glyphs.data();
    fontData.intervals = intervals.data();
    fontData.intervalCount = static_cast<uint32_t>(intervals.size());
    fontData.advanceY = 20;
    fontData.ascender = 16;
    fontData.descender = -4;

    renderer.insertFont(kFontId, EpdFontFamily(16, &epdFont));
  }
};

TEST_F(Phase3WrappedTextTest, WrapsLongNoSpaceCjkStringAtCodepointBoundaries) {
  // 10 CJK codepoints (U+4E00..U+4E09), 20px each; maxWidth=60 fits 3 per line.
  const std::string text = "\xE4\xB8\x80\xE4\xB8\x81\xE4\xB8\x82\xE4\xB8\x83\xE4\xB8\x84"
                           "\xE4\xB8\x85\xE4\xB8\x86\xE4\xB8\x87\xE4\xB8\x88\xE4\xB8\x89";
  const auto lines = renderer.wrappedText(kFontId, text.c_str(), 60, 5);
  ASSERT_EQ(lines.size(), 4u);
  EXPECT_EQ(lines[0], "\xE4\xB8\x80\xE4\xB8\x81\xE4\xB8\x82");
  EXPECT_EQ(lines[1], "\xE4\xB8\x83\xE4\xB8\x84\xE4\xB8\x85");
  EXPECT_EQ(lines[2], "\xE4\xB8\x86\xE4\xB8\x87\xE4\xB8\x88");
  EXPECT_EQ(lines[3], "\xE4\xB8\x89");
}

TEST_F(Phase3WrappedTextTest, WrapsLongNoSpaceMixedCjkAndAsciiString) {
  // ASCII = 10px, CJK = 20px. maxWidth = 50:
  // "A中BC" = 10+20+10+10 = 50 <= 50
  // "七D中" = 20+10+20 = 50 <= 50
  // "EF中"  = 10+10+20 = 40 <= 50
  const std::string text = "A\xE4\xB8\xAD" "BC" "\xE4\xB8\x83" "D\xE4\xB8\xAD" "EF\xE4\xB8\xAD";
  const auto lines = renderer.wrappedText(kFontId, text.c_str(), 50, 5);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0], "A\xE4\xB8\xAD" "BC");
  EXPECT_EQ(lines[1], "\xE4\xB8\x83" "D\xE4\xB8\xAD");
  EXPECT_EQ(lines[2], "EF\xE4\xB8\xAD");
}

TEST_F(Phase3WrappedTextTest, AdvancesAtLeastOneCodepointWhenSingleGlyphExceedsMaxWidth) {
  // CJK glyph is 20px > maxWidth=15: must consume 1 codepoint per line and advance.
  const std::string text = "\xE4\xB8\x80\xE4\xB8\x81\xE4\xB8\x82";
  const auto lines = renderer.wrappedText(kFontId, text.c_str(), 15, 4);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0], "\xE4\xB8\x80");
  EXPECT_EQ(lines[1], "\xE4\xB8\x81");
  EXPECT_EQ(lines[2], "\xE4\xB8\x82");
}

TEST_F(Phase3WrappedTextTest, LongNoSpaceCjkAndAsciiRunWrapsAndTruncatesOnMaxLines) {
  // 128 CJK codepoints ('字', 20px each), maxWidth=100 -> 5 codepoints/line.
  std::string text;
  for (int i = 0; i < 128; ++i) {
    text += "\xE5\xAD\x97";
  }
  const auto lines = renderer.wrappedText(kFontId, text.c_str(), 100, 30);
  ASSERT_EQ(lines.size(), 26u);
  for (size_t i = 0; i < 25; ++i) {
    EXPECT_EQ(lines[i], "\xE5\xAD\x97\xE5\xAD\x97\xE5\xAD\x97\xE5\xAD\x97\xE5\xAD\x97");
  }
  EXPECT_EQ(lines[25], "\xE5\xAD\x97\xE5\xAD\x97\xE5\xAD\x97");

  // Long ASCII run across multiple lines
  const std::string ascii(200, 'A');
  const auto asciiLines = renderer.wrappedText(kFontId, ascii.c_str(), 1000, 5);
  ASSERT_EQ(asciiLines.size(), 2u);
  EXPECT_EQ(asciiLines[0], std::string(100, 'A'));
  EXPECT_EQ(asciiLines[1], std::string(100, 'A'));
}

}  // namespace
