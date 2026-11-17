#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

// White-box inclusion of TextEncoding.cpp in its own isolated TU to inspect internal tables/matchers
// without modifying production source files.
#include "../../lib/TextEncoding/TextEncoding.cpp"

namespace {

TEST(Phase3TextEncodingBugProof, FrequentCjkTableIsSortedAndBinarySearchFindsAllEntries) {
  EXPECT_TRUE(std::is_sorted(std::begin(textencoding::kFrequentCjkSorted), std::end(textencoding::kFrequentCjkSorted)));
  for (uint16_t cp : textencoding::kFrequentCjkSorted) {
    EXPECT_TRUE(textencoding::isFrequentCjkOrPunct(cp));
  }
  EXPECT_TRUE(textencoding::isFrequentCjkOrPunct(0x500B));  // 個
  EXPECT_TRUE(textencoding::isFrequentCjkOrPunct(0x5011));  // 們
  EXPECT_TRUE(textencoding::isFrequentCjkOrPunct(0x7ECF));  // 经
  EXPECT_TRUE(textencoding::isFrequentCjkOrPunct(0x7ED9));  // 给
  EXPECT_TRUE(textencoding::isFrequentCjkOrPunct(0x7EDF));  // 统
}

TEST(Phase3TextEncodingBugProof, InlineUtf8BannerMatcherPreserves0xE2SmartPunctuation) {
  const std::string banner =
      "\xE2\x80\x9C\xE6\x9C\xAC\xE4\xB9\xA6\xE7\x94\xB1\xE6\xB5\x8B\xE8\xAF\x95\xE7\xAB\x99\xE6\x95\xB4\xE7\x90\x86"
      "\xE2\x80\x9D";  // “本书由测试站整理”
  EXPECT_EQ(static_cast<uint8_t>(banner[0]), 0xE2);
  EXPECT_EQ(textencoding::matchInlineUtf8CjkRun(reinterpret_cast<const uint8_t*>(banner.data()), banner.size(), 3),
            banner.size());

  // Stray 0xE2 punctuation without CJK characters must NOT trigger an inline UTF-8 CJK banner match.
  const std::string punctOnly = "\xE2\x80\x94 \xE2\x80\x94 \xE2\x80\x94";
  EXPECT_EQ(
      textencoding::matchInlineUtf8CjkRun(reinterpret_cast<const uint8_t*>(punctOnly.data()), punctOnly.size(), 3),
      0u);

  const std::string utf8Banner = "\xE2\x80\x94 \xE6\xB5\x8B\xE8\xAF\x95\xE7\xAB\x99\xE6\x95\xB4\xE7\x90\x86";
  textencoding::TextDecoder dec(TextEncoding::Gb18030);
  const std::string decoded =
      dec.decodeToString(reinterpret_cast<const uint8_t*>(utf8Banner.data()), utf8Banner.size(), true);
  EXPECT_NE(decoded.find("\xE2\x80\x94"), std::string::npos);
}

}  // namespace
