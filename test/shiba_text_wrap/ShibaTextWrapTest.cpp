#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "components/themes/shibadesk/ShibaTextLayout.h"

namespace {

// Deterministic synthetic monospace pixel model:
// - ASCII codepoints (< 0x80): 8 px
// - CJK / wide codepoints (>= 0x2E80): 16 px
// - Other UTF-8 codepoints: 10 px
int syntheticMeasurePx(const char* text) {
  if (!text) return 0;
  int width = 0;
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  while (*p != '\0') {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp < 0x80) {
      width += 8;
    } else if (cp >= 0x2E80) {
      width += 16;
    } else {
      width += 10;
    }
  }
  return width;
}

std::string syntheticTruncate(const char* text, int maxWidthPx) {
  return shibadesk::truncateUtf8ToWidth(text, maxWidthPx, syntheticMeasurePx, "...");
}

}  // namespace

TEST(ShibaTextWrapTest, DetectsCjkAndKinsokuCodepoints) {
  EXPECT_FALSE(shibadesk::hasCjkCodepoints(nullptr));
  EXPECT_FALSE(shibadesk::hasCjkCodepoints("Pride and Prejudice"));
  EXPECT_TRUE(shibadesk::hasCjkCodepoints("三体"));
  EXPECT_TRUE(shibadesk::hasCjkCodepoints("《十日终焉》"));

  EXPECT_TRUE(shibadesk::isKinsokuForbiddenStart(0x3002));   // 。
  EXPECT_TRUE(shibadesk::isKinsokuForbiddenStart(0xFF0C));   // ，
  EXPECT_TRUE(shibadesk::isKinsokuForbiddenStart(0x300B));   // 》
  EXPECT_FALSE(shibadesk::isKinsokuForbiddenStart(0x4E2D));  // 中
}

TEST(ShibaTextWrapTest, TruncateUtf8NeverSplitsMultiByteGlyphs) {
  // Each CJK char is 16px, "..." is 24px. With maxWidthPx = 60, at most 2 CJK chars (32 + 24 = 56 <= 60) fit.
  const std::string truncated = syntheticTruncate("十日终焉杀虫队", 60);
  EXPECT_EQ(truncated, "十日...");
  EXPECT_LE(syntheticMeasurePx(truncated.c_str()), 60);

  // Already fits -> untouched
  EXPECT_EQ(syntheticTruncate("三体", 60), "三体");
}

TEST(ShibaTextWrapTest, WrapsLatinWordsWithoutMidWordSplits) {
  // "Quiet" (40) + " " (8) + "Study" (40) = 88px; "Desk" (32) wraps to line 2 under 96px budget.
  const auto lines = shibadesk::wrapSmartLinesWith("Quiet   Study\nDesk", 96, 2, syntheticMeasurePx, syntheticTruncate);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], "Quiet Study");
  EXPECT_EQ(lines[1], "Desk");
}

TEST(ShibaTextWrapTest, KeepsKinsokuClosingPunctuationAttachedToPrecedingGlyph) {
  // 4 CJK chars = 64px. In "十日终焉，第一卷" ("焉，" is 32px together):
  // With 68px budget: "十日终" = 48px, next token is "焉，" (32px) -> 48 + 32 = 80 > 68!
  // So line 1 breaks BEFORE "焉，" rather than stranding "，" at the start of line 2!
  const auto lines = shibadesk::wrapSmartLinesWith("十日终焉，第一卷", 68, 3, syntheticMeasurePx, syntheticTruncate);
  ASSERT_GE(lines.size(), 2u);
  EXPECT_EQ(lines[0], "十日终");
  EXPECT_TRUE(lines[1].rfind("焉，", 0) == 0);
  // Verify line 2 does NOT begin with forbidden kinsoku comma
  const auto* p = reinterpret_cast<const unsigned char*>(lines[1].c_str());
  EXPECT_FALSE(shibadesk::isKinsokuForbiddenStart(utf8NextCodepoint(&p)));
}

TEST(ShibaTextWrapTest, ClampsFinalLineWithEllipsisOnOverflow) {
  const auto lines = shibadesk::wrapSmartLinesWith("The Hitchhiker's Guide to the Galaxy Complete Omnibus Edition", 120,
                                                   2, syntheticMeasurePx, syntheticTruncate);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_LE(syntheticMeasurePx(lines[0].c_str()), 120);
  EXPECT_LE(syntheticMeasurePx(lines[1].c_str()), 120);
  EXPECT_NE(lines[1].find("..."), std::string::npos);
}
