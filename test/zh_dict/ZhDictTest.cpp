#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

#include "ZhDict.h"

namespace {

std::vector<uint32_t> decode(const std::string& s) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < s.size();) {
    const auto b = static_cast<uint8_t>(s[i]);
    const int n = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
    uint32_t cp = n == 1 ? b : b & (0x3F >> (n - 1));
    for (int k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<uint8_t>(s[i + k]) & 0x3F);
    out.push_back(cp);
    i += n;
  }
  return out;
}

std::string encode(const std::vector<uint32_t>& cps, size_t from, size_t len) {
  std::string out;
  for (size_t i = from; i < from + len; ++i) {
    const uint32_t cp = cps[i];
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  return out;
}

// Packs bopomofo symbols and a tone the way the converter does.
uint32_t zhuyin(const std::string& symbols, int tone) {
  uint32_t v = 0;
  int k = 0;
  for (uint32_t cp : decode(symbols)) v |= (cp - 0x3104) << (6 * k++);
  return v | (static_cast<uint32_t>(tone) << 18);
}

class ZhDictTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(dict.open(ZH_FIXTURE_DIR "/zh")); }
  void TearDown() override { dict.close(); }

  std::vector<zh::WordMatch> matches(const std::string& text, size_t at = 0) {
    const auto cps = decode(text);
    zh::WordMatch out[zh::MAX_MATCHES];
    const int n = dict.matchesAt(cps.data(), cps.size(), at, out, zh::MAX_MATCHES);
    return {out, out + n};
  }

  std::string segment(const std::string& text) {
    const auto cps = decode(text);
    std::string out;
    zh::segmentRun(dict, cps.data(), cps.size(), [&](const zh::Segment& s) {
      if (!out.empty()) out += "/";
      out += encode(cps, s.start, s.len);
    });
    return out;
  }

  zh::Dict dict;
};

TEST_F(ZhDictTest, MatchesAreLongestFirstAndLimitedToTheText) {
  const auto m = matches("工作人员");
  ASSERT_GE(m.size(), 2u);
  EXPECT_EQ(m.front().len, 4);
  for (size_t k = 1; k < m.size(); ++k) EXPECT_LT(m[k].len, m[k - 1].len);
  // Words must not run past the end of the given text.
  for (const auto& w : matches("工作")) EXPECT_LE(w.len, 2);
}

TEST_F(ZhDictTest, CarriesHskRankAndPerCharacterZhuyin) {
  const auto m = matches("银行");
  ASSERT_FALSE(m.empty());
  EXPECT_EQ(m.front().len, 2);
  EXPECT_EQ(m.front().hsk, 2);
  EXPECT_NE(m.front().rank, zh::NO_RANK);
  EXPECT_EQ(m.front().zhuyin[0], zhuyin("ㄧㄣ", 2));
  EXPECT_EQ(m.front().zhuyin[1], zhuyin("ㄏㄤ", 2));
}

TEST_F(ZhDictTest, SingleCharactersUseTheCustomaryReading) {
  // CC-CEDICT lists zhāo/zháo first; Unihan's customary reading is zhe.
  const auto zhe = matches("着");
  ASSERT_EQ(zhe.size(), 1u);
  EXPECT_EQ(zhe.front().zhuyin[0], zhuyin("ㄓㄜ", 5));
  const auto kan = matches("看");
  ASSERT_EQ(kan.size(), 1u);
  EXPECT_EQ(kan.front().zhuyin[0], zhuyin("ㄎㄢ", 4));
}

TEST_F(ZhDictTest, LongDirectoriesUseSlices) {
  // 大 has more than 128 second characters in the fixture: its pair
  // directory is split into sector slices indexed from slices.bin.
  EXPECT_FALSE(matches("大学").empty());
  EXPECT_EQ(matches("大学").front().len, 2);
  EXPECT_EQ(matches("大人").front().len, 2);
  // 一 has 33-128: slice starts are inline in the character record.
  EXPECT_EQ(matches("一个").front().len, 2);
}

TEST_F(ZhDictTest, UnknownCharactersHaveNoMatches) {
  EXPECT_TRUE(matches("龘").empty());
  EXPECT_TRUE(matches("A").empty());
}

TEST_F(ZhDictTest, SegmentationPrefersProbableWords) {
  EXPECT_EQ(segment("她看着手机"), "她/看/着/手机");
  EXPECT_EQ(segment("每个周末都会去图书馆"), "每个/周末/都/会/去/图书馆");
  EXPECT_EQ(segment("工作人员看了看她的资料"), "工作人员/看/了/看/她/的/资料");
}

TEST_F(ZhDictTest, SegmentationMatchesTheReference) {
  std::ifstream golden(ZH_FIXTURE_DIR "/segmentation.txt");
  ASSERT_TRUE(golden.good());
  std::string line;
  int checked = 0;
  while (std::getline(golden, line)) {
    const size_t tab = line.find('\t');
    ASSERT_NE(tab, std::string::npos);
    const std::string text = line.substr(0, tab);
    EXPECT_EQ(segment(text), line.substr(tab + 1)) << "run: " << text;
    ++checked;
  }
  EXPECT_GT(checked, 400);
}

TEST_F(ZhDictTest, ReadsEntries) {
  const auto m = matches("银行");
  ASSERT_FALSE(m.empty());
  uint8_t buf[1024];
  const size_t got = dict.readEntry(m.front().entryOff, buf, sizeof(buf));
  ASSERT_GT(got, 4u);
  EXPECT_GE(buf[0], 1);  // readings
  const std::string pinyin(reinterpret_cast<const char*>(buf + 2), buf[1]);
  EXPECT_EQ(pinyin, "yín háng");
}

TEST_F(ZhDictTest, EntryReaderSplitsReadings) {
  const auto m = matches("银行");
  ASSERT_FALSE(m.empty());
  uint8_t buf[1024];
  const size_t got = dict.readEntry(m.front().entryOff, buf, sizeof(buf));
  zh::EntryReader entry(buf, got);
  ASSERT_GE(entry.count(), 1);
  zh::EntryReading r;
  ASSERT_TRUE(entry.next(r));
  EXPECT_EQ(std::string(r.pinyin, r.pinyinLen), "yín háng");
  EXPECT_EQ(std::string(r.trad, r.tradLen), "銀行");
  ASSERT_EQ(r.syllables, 2);
  EXPECT_EQ(r.zhuyin[0], zhuyin("ㄧㄣ", 2));
  EXPECT_EQ(r.zhuyin[1], zhuyin("ㄏㄤ", 2));
  EXPECT_NE(std::string(r.senses, r.sensesLen).find("bank"), std::string::npos);
  int rest = 0;
  while (entry.next(r)) ++rest;
  EXPECT_EQ(rest + 1, entry.count());
}

TEST_F(ZhDictTest, EntryReaderStopsAtATruncatedRecord) {
  const auto m = matches("银行");
  ASSERT_FALSE(m.empty());
  uint8_t buf[1024];
  const size_t got = dict.readEntry(m.front().entryOff, buf, sizeof(buf));
  // Every cut either yields the full first reading or none, never reads past the cut.
  for (size_t cut = 0; cut < got; ++cut) {
    zh::EntryReader entry(buf, cut);
    zh::EntryReading r;
    while (entry.next(r)) {
      EXPECT_LE(reinterpret_cast<const uint8_t*>(r.senses + r.sensesLen), buf + cut);
      EXPECT_LE(reinterpret_cast<const uint8_t*>(r.trad + r.tradLen), buf + cut);
    }
  }
}

TEST(ZhZhuyin, TextHasToneMarks) {
  std::string out;
  zh::appendZhuyin(out, zhuyin("ㄒㄩㄝ", 2));
  EXPECT_EQ(out, "ㄒㄩㄝˊ");
  out.clear();
  zh::appendZhuyin(out, zhuyin("ㄇㄚ", 1));
  EXPECT_EQ(out, "ㄇㄚ");
  out.clear();
  zh::appendZhuyin(out, zhuyin("ㄉㄜ", 5));
  EXPECT_EQ(out, "˙ㄉㄜ");
  out.clear();
  zh::appendZhuyin(out, zhuyin("ㄏㄠ", 3));
  zh::appendZhuyin(out, zhuyin("ㄕ", 4));
  EXPECT_EQ(out, "ㄏㄠˇㄕˋ");
}

TEST(ZhRuby, ThresholdSelectsRareDictionaryWords) {
  zh::Segment common{};
  common.inDict = true;
  common.rank = 100;
  zh::Segment rare = common;
  rare.rank = 9000;
  zh::Segment unranked = common;
  unranked.rank = zh::NO_RANK;
  zh::Segment unknown{};
  unknown.inDict = false;
  unknown.rank = zh::NO_RANK;

  EXPECT_FALSE(zh::wantsRuby(rare, 0));  // off
  EXPECT_TRUE(zh::wantsRuby(common, zh::RUBY_ALL));
  EXPECT_FALSE(zh::wantsRuby(common, 3000));
  EXPECT_TRUE(zh::wantsRuby(rare, 3000));
  EXPECT_TRUE(zh::wantsRuby(unranked, 60000));
  EXPECT_FALSE(zh::wantsRuby(unknown, zh::RUBY_ALL));  // no reading to show
}

TEST(ZhRuby, StepsRunFromOffToAllToRarer) {
  EXPECT_EQ(zh::rubyThresholdForStep(0), 0u);
  EXPECT_EQ(zh::rubyThresholdForStep(1), zh::RUBY_ALL);
  for (uint8_t s = 3; s < zh::RUBY_STEP_COUNT; ++s) {
    EXPECT_GT(zh::rubyThresholdForStep(s), zh::rubyThresholdForStep(s - 1));
  }
  EXPECT_EQ(zh::rubyThresholdForStep(zh::RUBY_STEP_COUNT), 0u);
}

TEST(ZhRuby, CodesRoundTripThroughRubyStrings) {
  const uint32_t code = zhuyin("ㄓㄨㄤ", 3);
  char text[8];
  zh::encodeRubyCode(code, text);
  EXPECT_TRUE(zh::isRubyCode(text));
  EXPECT_EQ(zh::decodeRubyCode(text), code);
  EXPECT_FALSE(zh::isRubyCode("ㄓ"));
  EXPECT_EQ(zh::decodeRubyCode("plain"), 0u);
}

TEST(ZhRuby, PixelsStayInsideTheRubyColumn) {
  for (const int height : {16, 24, 28, 32, 36, 40, 42, 48, 54, 64, 66, 72, 80, 96}) {
    const int width = zh::rubyWidth(height);
    // The smallest column overflows below 23 px; scaled ones by up to ~15%.
    const int slack = height < 23 ? 23 - height : height * 3 / 20;
    for (const uint32_t code : {zhuyin("ㄓㄨㄤ", 3), zhuyin("ㄉㄜ", 5), zhuyin("ㄚ", 1), zhuyin("ㄌㄧㄡ", 4)}) {
      int count = 0, minY = 999, maxY = -999;
      zh::forEachRubyPixel(code, height, height, [&](int x, int y) {
        EXPECT_GE(x, 0) << height;
        EXPECT_LT(x, width) << height;
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
        ++count;
      });
      EXPECT_GT(count, 0);
      // Centred on the character: within the line, allowing the neutral-tone dot above.
      const int scale = zh::rubyStyle(height).scale;
      EXPECT_GE(minY, -4 * scale - slack) << height;
      EXPECT_LT(maxY, height + slack) << height;
    }
  }
  // A line taller than the em (leading) centres the same column lower.
  int top32 = 999, top40 = 999;
  zh::forEachRubyPixel(zhuyin("ㄓㄨㄤ", 3), 32, 32, [&](int, int y) { top32 = std::min(top32, y); });
  zh::forEachRubyPixel(zhuyin("ㄓㄨㄤ", 3), 32, 40, [&](int, int y) { top40 = std::min(top40, y); });
  EXPECT_EQ(top40 - top32, 4);
  int none = 0;
  zh::forEachRubyPixel(0, 32, 32, [&](int, int) { ++none; });
  EXPECT_EQ(none, 0);
}

TEST(ZhRuby, StyleFollowsTheTextSize) {
  // {em, set, scale, block width}
  const int cases[][4] = {{16, 0, 1, 10}, {24, 0, 1, 10}, {28, 0, 1, 10}, {29, 1, 1, 16},
                          {32, 1, 1, 16}, {36, 2, 1, 20}, {39, 2, 1, 20}, {42, 0, 2, 20},
                          {48, 0, 2, 20}, {64, 1, 2, 32}, {72, 2, 2, 40}, {96, 2, 3, 60}};
  for (const auto& c : cases) {
    const zh::RubyStyle st = zh::rubyStyle(c[0]);
    EXPECT_EQ(st.set, c[1]) << c[0];
    EXPECT_EQ(st.scale, c[2]) << c[0];
    EXPECT_EQ(zh::rubyWidth(c[0]), c[3]) << c[0];
  }
}

}  // namespace
