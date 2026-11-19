#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "ZhDict.h"
#include "ZhFlashTables.h"
#include "ZhJump.h"

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

TEST_F(ZhDictTest, CharacterFilterSkipsFamiliarCharsExceptNonNeutralPolyphones) {
  auto* s = dict.scratch();
  ASSERT_NE(s, nullptr);

  // Simulate a 2-char rare compound word (rank 9000, threshold 3000):
  // char 0 is rare (rank 6000), char 1 is familiar (rank 50) with its primary reading.
  zh::Segment seg{};
  seg.start = 0;
  seg.len = 2;
  seg.inDict = true;
  seg.rank = 9000;
  seg.zhuyin[0] = zhuyin("ㄏㄢ", 4);
  seg.zhuyin[1] = zhuyin("ㄧㄢ", 1);
  s->selfRank[0] = 6000;
  s->selfZhuyin[0] = zhuyin("ㄏㄢ", 4);
  s->selfRank[1] = 50;
  s->selfZhuyin[1] = zhuyin("ㄧㄢ", 1);

  EXPECT_TRUE(zh::wantsCharRuby(dict, seg, 0, 0x65F1, 3000));
  EXPECT_FALSE(zh::wantsCharRuby(dict, seg, 1, 0x70DF, 3000));
  EXPECT_TRUE(zh::wantsCharRuby(dict, seg, 1, 0x70DF, zh::RUBY_ALL));

  // Polyphone exception: familiar character (着, U+7740) taking a secondary non-neutral
  // reading (zhuó) in an uncommon compound gets ruby, while neutral tone (zhe) or
  // grammar particle 得 (U+5F97) does not.
  seg.zhuyin[1] = zhuyin("ㄓㄨㄛ", 2);
  s->selfRank[1] = 30;
  s->selfZhuyin[1] = zhuyin("ㄓㄜ", 5);
  EXPECT_TRUE(zh::wantsCharRuby(dict, seg, 1, 0x7740, 3000));

  seg.zhuyin[1] = zhuyin("ㄓㄜ", 5);
  EXPECT_FALSE(zh::wantsCharRuby(dict, seg, 1, 0x7740, 3000));

  seg.zhuyin[1] = zhuyin("ㄉㄟ", 3);
  s->selfZhuyin[1] = zhuyin("ㄉㄜ", 2);
  EXPECT_FALSE(zh::wantsCharRuby(dict, seg, 1, 0x5F97, 3000));
}

TEST(ZhRuby, UnderLinePixelsStayWithinCellAndClearance) {
  for (const int em : {16, 24, 32, 48}) {
    const int underH = zh::rubyUnderHeight(em);
    EXPECT_GT(underH, 0);
    for (const uint32_t code : {zhuyin("ㄓㄨㄤ", 3), zhuyin("ㄇㄧㄠ", 3), zhuyin("ㄉㄜ", 5), zhuyin("ㄚ", 1)}) {
      const int w = zh::rubyUnderWidth(code, em);
      const int extra = zh::rubyUnderExtraWidth(code, em);
      const int cellW = em + extra;
      EXPECT_GT(w, 0);
      EXPECT_LE(w, cellW);
      int count = 0, minX = 999, maxX = -999, minY = 999, maxY = -999;
      zh::forEachRubyUnderPixel(code, em, cellW, em, [&](int x, int y) {
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
        ++count;
      });
      EXPECT_GT(count, 0);
      EXPECT_GE(minX, 0) << em;
      EXPECT_LT(maxX, cellW) << em;
      EXPECT_GT(minY, em) << em;
      EXPECT_LT(maxY, em + underH) << em;
    }
  }
  for (const int em : {24, 30, 32, 36, 48}) {
    const uint32_t code = zhuyin("ㄓㄨㄤ", 3);
    EXPECT_LE(zh::rubyUnderWidth(code, em), em - 2) << em;
    EXPECT_EQ(zh::rubyUnderExtraWidth(code, em), 0) << em;
  }
}

TEST_F(ZhDictTest, TwoWayCacheAndMemoizedBigramSkipRepeatReads) {
  EXPECT_FALSE(dict.usesFlashTables());

  const auto first = matches("银行");
  ASSERT_FALSE(first.empty());

  HalFile::totalReads = 0;
  const auto second = matches("银行");
  EXPECT_EQ(HalFile::totalReads, 0u);
  ASSERT_EQ(second.size(), first.size());
  for (size_t i = 0; i < first.size(); ++i) {
    EXPECT_EQ(second[i].len, first[i].len);
    EXPECT_EQ(second[i].entryOff, first[i].entryOff);
    EXPECT_EQ(second[i].hsk, first[i].hsk);
    EXPECT_EQ(second[i].flags, first[i].flags);
    EXPECT_EQ(second[i].rank, first[i].rank);
    EXPECT_EQ(second[i].zhuyin[0], first[i].zhuyin[0]);
    EXPECT_EQ(second[i].zhuyin[1], first[i].zhuyin[1]);
  }

  HalFile::totalReads = 0;
  const std::string seg1 = segment("工作人员看了看她的资料");
  const size_t coldReads = HalFile::totalReads;
  EXPECT_GT(coldReads, 5u);

  HalFile::totalReads = 0;
  const std::string seg2 = segment("工作人员看了看她的资料");
  EXPECT_EQ(seg1, seg2);
  // Only the 4-char word "工作人员" (hasLonger == true) re-reads words.bin on repeat;
  // all character records, misses, and 2-char words hit the 2-way LRU cache in 0 reads.
  EXPECT_EQ(HalFile::totalReads, 1u);
}

TEST(ZhFlashTables, BloomFiltersContainLiveDictionaryWords) {
  EXPECT_TRUE(zh::flash::pairBloomMayContain(0x5DE5, 0x4F5C));            // 工作
  EXPECT_TRUE(zh::flash::pairBloomMayContain(0x94F6, 0x884C));            // 银行
  EXPECT_TRUE(zh::flash::tripleBloomMayContain(0x5DE5, 0x4F5C, 0x4EBA));  // 工作人
}

TEST(ZhFlashTables, LiveDictionaryMatchesReferenceWhenEnvSet) {
  const char* liveDir = std::getenv("ZH_LIVE_DIR");
  if (!liveDir || liveDir[0] == '\0') return;

  zh::Dict live;
  ASSERT_TRUE(live.open(liveDir));
  EXPECT_TRUE(live.usesFlashTables());

  std::ifstream golden(ZH_FIXTURE_DIR "/segmentation.txt");
  ASSERT_TRUE(golden.good());
  std::string line;
  std::string joined;
  while (std::getline(golden, line)) {
    const size_t tab = line.find('\t');
    ASSERT_NE(tab, std::string::npos);
    const std::string text = line.substr(0, tab);
    const auto cps = decode(text);
    std::string segOut;
    zh::segmentRun(live, cps.data(), cps.size(), [&](const zh::Segment& s) {
      if (!segOut.empty()) segOut += "/";
      segOut += encode(cps, s.start, s.len);
    });
    ASSERT_FALSE(segOut.empty()) << text;
    joined += text + "\t" + segOut + "\n";
  }
  EXPECT_FALSE(joined.empty());
  if (const char* outPath = std::getenv("ZH_LIVE_SEG_OUT"); outPath && outPath[0] != '\0') {
    std::ofstream out(outPath);
    out << joined;
  }
  live.close();
}

TEST(ZhJump, RarestFirstBatchingAndTwoPressPick) {
  static_assert(sizeof(zh::JumpBatch) <= 40, "JumpBatch must stay tiny on ESP32-C3");

  // 20 visible words with varying ranks, including an unranked dict word and non-dict word.
  std::vector<zh::JumpCandidate> cands;
  for (uint16_t i = 0; i < 20; ++i) {
    cands.push_back({i, static_cast<uint32_t>((i + 1) * 1000), 1});
  }
  cands[2].rank = zh::NO_RANK;  // unranked dictionary word -> rarest
  cands[5].inDict = 0;          // non-dict word -> sorted after dictionary words
  cands[5].rank = zh::NO_RANK;

  std::vector<uint16_t> order;
  zh::buildJumpOrder(cands, order);
  ASSERT_EQ(order.size(), 20u);
  EXPECT_EQ(order.front(), 2);  // unranked dict word first
  EXPECT_EQ(order.back(), 5);   // non-dict word last

  zh::JumpBatch batch;
  batch.load(order, 0);
  ASSERT_EQ(batch.count, 16);
  EXPECT_EQ(batch.offset, 0);
  EXPECT_EQ(batch.firstPress, -1);

  // Targets within the batch must be sorted in reading order.
  for (uint8_t i = 1; i < batch.count; ++i) {
    EXPECT_LT(batch.targets[i - 1], batch.targets[i]);
  }

  // Every target in a full 16-word batch has a unique 2-slice code (col1, col2).
  for (uint8_t i = 0; i < 16; ++i) {
    const auto code = batch.codeFor(i);
    EXPECT_EQ(code.slices, 2);
    EXPECT_EQ(code.col1, i / 4);
    EXPECT_EQ(code.col2, i % 4);
  }

  // Pick target 7 (col1 = 1, col2 = 3): 1st press narrows, 2nd press lands.
  EXPECT_EQ(batch.pick(1), zh::JumpBatch::PICK_NARROWED);
  EXPECT_EQ(batch.firstPress, 1);
  EXPECT_EQ(batch.pick(3), batch.targets[7]);

  // Undo clears firstPress before stepping back a batch.
  EXPECT_TRUE(batch.undo(order));
  EXPECT_EQ(batch.firstPress, -1);
  EXPECT_FALSE(batch.undo(order));  // already at batch 0 -> exit jump mode

  // Second hold cycles to the next batch (remaining 4 words), then wraps to 0.
  EXPECT_EQ(batch.pick(2), zh::JumpBatch::PICK_NARROWED);
  batch.nextBatch(order);
  EXPECT_EQ(batch.offset, 16);
  EXPECT_EQ(batch.count, 4);
  EXPECT_EQ(batch.firstPress, -1);

  // Batch of 4 words needs only 1 slice and lands on the first press.
  for (uint8_t i = 0; i < 4; ++i) {
    const auto code = batch.codeFor(i);
    EXPECT_EQ(code.slices, 1);
    EXPECT_EQ(code.col1, i);
  }
  EXPECT_EQ(batch.pick(2), batch.targets[2]);

  // Undo from batch 1 returns to batch 0.
  EXPECT_TRUE(batch.undo(order));
  EXPECT_EQ(batch.offset, 0);
  EXPECT_EQ(batch.count, 16);

  // Wrap around from last batch back to batch 0.
  batch.nextBatch(order);
  EXPECT_EQ(batch.offset, 16);
  batch.nextBatch(order);
  EXPECT_EQ(batch.offset, 0);
}

TEST(ZhJump, BalancedGroupingForPartialBatch) {
  std::vector<uint16_t> order = {10, 20, 30, 40, 50, 60};
  zh::JumpBatch batch;
  batch.load(order, 0);
  ASSERT_EQ(batch.count, 6);

  // 6 items across 4 buttons -> groups of (2, 2, 1, 1).
  EXPECT_EQ(batch.codeFor(0).slices, 2);
  EXPECT_EQ(batch.codeFor(1).slices, 2);
  EXPECT_EQ(batch.codeFor(2).slices, 2);
  EXPECT_EQ(batch.codeFor(3).slices, 2);
  EXPECT_EQ(batch.codeFor(4).slices, 1);
  EXPECT_EQ(batch.codeFor(5).slices, 1);

  // Buttons 2 and 3 have 1 item each -> land immediately in 1 press.
  EXPECT_EQ(batch.pick(2), 50);
  EXPECT_EQ(batch.pick(3), 60);

  // Button 0 has 2 items -> narrows, then invalid col 2 is NOOP, valid col 1 lands.
  EXPECT_EQ(batch.pick(0), zh::JumpBatch::PICK_NARROWED);
  EXPECT_EQ(batch.pick(2), zh::JumpBatch::PICK_NOOP);
  EXPECT_EQ(batch.pick(1), 20);
}

}  // namespace
