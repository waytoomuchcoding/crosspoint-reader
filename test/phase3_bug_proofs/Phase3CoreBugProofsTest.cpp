#include <Epub/BookMetadataCache.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <Memory.h>
#include <Txt.h>
#include <TxtEncodingSupport.h>
#include <TxtToHtml.h>
#include <ZhDict.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "ClippingStore.h"
#include "RecentBooksStore.h"
#include "util/BookCacheUtils.h"
#include "util/BookCoverStore.h"

namespace {

class StringPrint : public Print {
 public:
  std::string str;
  size_t write(uint8_t b) override {
    str.push_back(static_cast<char>(b));
    return 1;
  }
  size_t write(const uint8_t* buffer, size_t size) override {
    str.append(reinterpret_cast<const char*>(buffer), size);
    return size;
  }
};

class Phase3CoreBugProofsTest : public ::testing::Test {
 protected:
  void SetUp() override { fake::reset(); }
};

}  // namespace

namespace library {
bool markLibraryIndexDirty() { return true; }
}  // namespace library

void RecentBooksStore::updateBook(const std::string&, const std::string&, const std::string&, const std::string&) {}
bool ClippingStore::deleteForFilePath(const std::string&, const std::string&) { return true; }

TEST_F(Phase3CoreBugProofsTest, Bug2_TxtCacheDistinguishesJapaneseKoreanAndChineseBooks) {
  {
    const std::string jaPath = "/Books/wagahai.txt";
    const std::string jaText = "吾輩は猫である。名前はまだ無い。\nどこで生れたかとんと見当がつかぬ。\n";
    fake::add(jaPath, jaText);
    const std::string jaCache = Txt::getBookCachePath(jaPath);
    auto meta = std::make_unique<BookMetadataCache>(jaCache);
    ASSERT_TRUE(Txt::buildTxtCache(jaPath, jaCache, meta));
    EXPECT_EQ(meta->coreMetadata.language, "ja");
  }
  {
    const std::string koPath = "/Books/hangul.txt";
    const std::string koText = "훈민정음(訓民正音)은 세종대왕이 창제하였다.\n";
    fake::add(koPath, koText);
    const std::string koCache = Txt::getBookCachePath(koPath);
    auto meta = std::make_unique<BookMetadataCache>(koCache);
    ASSERT_TRUE(Txt::buildTxtCache(koPath, koCache, meta));
    EXPECT_EQ(meta->coreMetadata.language, "ko");
  }
  {
    // Middle dot U+30FB (・) in translated Chinese names must stay "zh", not "ja".
    const std::string zhPath = "/Books/translated_zh.txt";
    const std::string zhText = "哈利・波特与魔法石是一部小说。\n";
    fake::add(zhPath, zhText);
    const std::string zhCache = Txt::getBookCachePath(zhPath);
    auto meta = std::make_unique<BookMetadataCache>(zhCache);
    ASSERT_TRUE(Txt::buildTxtCache(zhPath, zhCache, meta));
    EXPECT_EQ(meta->coreMetadata.language, "zh");
  }
  {
    // Predominantly Latin books with a short CJK quote/parenthetical must stay "en" for StarDict lookup.
    const std::string biPath = "/Books/bilingual_en.txt";
    const std::string biText = "Chapter 1\n\nConfucius (孔子) taught ethics and philosophy in ancient times.\n";
    fake::add(biPath, biText);
    const std::string biCache = Txt::getBookCachePath(biPath);
    auto meta = std::make_unique<BookMetadataCache>(biCache);
    ASSERT_TRUE(Txt::buildTxtCache(biPath, biCache, meta));
    EXPECT_EQ(meta->coreMetadata.language, "en");
  }
}

TEST_F(Phase3CoreBugProofsTest, Bug13_ZhuyinGlyphColSpanKeepsErWideAndTrimsHorizontalYi) {
  const zh::detail::GlyphSet& g9 = zh::detail::GLYPH_SETS[1];  // 9x9 set used at medium em (29..34 px)
  ASSERT_EQ(g9.w, 9);

  // Codepoint U+3126 = ㄦ -> symIdx 33 retains full [0, 9) width without outer-stroke clipping.
  int firstEr = -1, spanEr = -1;
  zh::detail::glyphColSpan(g9, 33, firstEr, spanEr);
  EXPECT_EQ(firstEr, 0);
  EXPECT_EQ(spanEr, 9);

  // Codepoint U+3127 = ㄧ -> symIdx 34 trims 1 column on each side [1, 7) in horizontal layout.
  int firstYi = -1, spanYi = -1;
  zh::detail::glyphColSpan(g9, 34, firstYi, spanYi);
  EXPECT_EQ(firstYi, 1);
  EXPECT_EQ(spanYi, 7);
}

TEST_F(Phase3CoreBugProofsTest, Bug14a_ChangingTxtEncodingOverrideInvalidatesStaleHtmlAndSectionCache) {
  const std::string txtPath = "/Books/gbk_novel.txt";
  // GB18030 bytes for "第一章 测试\n正文内容\n"
  const std::string gbkBytes =
      "\xB5\xDA\xD2\xBB\xD5\xC2 \xB2\xE2\xCA\xD4\n\xD5\xFD\xCE\xC4\xC4\xDA\xC8\xDD\n";
  fake::add(txtPath, gbkBytes);
  const std::string cachePath = Txt::getBookCachePath(txtPath);

  ASSERT_TRUE(Txt::writeEncodingOverride(cachePath, TextEncoding::Utf8));
  StringPrint htmlOut;
  ASSERT_TRUE(Txt::streamTxtToHtml(txtPath, htmlOut, cachePath));
  fake::add(cachePath + "/html/0.html", htmlOut.str);
  fake::add(cachePath + "/sections/0.bin", "STALE_UTF8_SECTION");

  // User switches encoding override via Library menu
  ASSERT_TRUE(Txt::writeEncodingOverride(cachePath, TextEncoding::Gb18030));

  EXPECT_FALSE(Txt::validateCache(txtPath, cachePath, gbkBytes.size()));
  EXPECT_FALSE(Storage.exists((cachePath + "/html/0.html").c_str()));
  EXPECT_FALSE(Storage.exists((cachePath + "/sections/0.bin").c_str()));
}

TEST_F(Phase3CoreBugProofsTest, Bug14b_Exact512ByteHeadingOverflowMatchesBetweenScanAndHtml) {
  // Construct line 0: "第一章 " (10 bytes) + 502 trailing spaces = exact 512 bytes, then "\n第二章 正文\n"
  std::string line1 = "\xE7\xAC\xAC\xE4\xB8\x80\xE7\xAB\xA0 ";  // "第一章 " (10 UTF-8 bytes)
  line1.append(512 - line1.size(), ' ');
  ASSERT_EQ(line1.size(), 512u);
  const std::string content = line1 + "\n\xE7\xAC\xAC\xE4\xBA\x8C\xE7\xAB\xA0 \xE6\xAD\xA3\xE6\x96\x87\n";

  const std::string txtPath = "/Books/boundary512.txt";
  fake::add(txtPath, content);
  BookMetadataCache cache("/.crosspoint/txt_boundary512");
  ASSERT_TRUE(cache.beginWrite());
  bool hasCjk = false;
  const size_t tocCount = txtenc::scanChapterHeadings(txtPath, TextEncoding::Utf8, cache, hasCjk);

  StringPrint htmlOut;
  ASSERT_TRUE(TxtToHtml::stream("boundary512.txt", content, htmlOut, TextEncoding::Utf8));

  // Both scanChapterHeadings and TxtToHtml treat >= 512 bytes as overflowed: 1 TOC entry ("ch0") matching HTML.
  EXPECT_EQ(tocCount, 1u);
  EXPECT_NE(htmlOut.str.find("<div id=\"ch0\"></div>第二章 正文"), std::string::npos);
  EXPECT_EQ(htmlOut.str.find("ch1"), std::string::npos);
}

TEST_F(Phase3CoreBugProofsTest, Bug16a_MoveBookSidecarStateMovesKeyAndRightsFiles) {
  const std::string oldPath = "/Libby/ProtectedBook.epub";
  const std::string newPath = "/Read/ProtectedBook.epub";
  fake::add(oldPath, "FAKE_EPUB_ZIP");
  fake::add(oldPath + ".key", "WRAPPED_AES_KEY");
  fake::add(oldPath + ".rights", "RIGHTS_JSON");
  fake::add(oldPath + ".meta.json", R"({"libby_loan_id":"loan-1"})");

  ASSERT_TRUE(Storage.rename(oldPath.c_str(), newPath.c_str()));
  moveBookSidecarState(oldPath, newPath);

  EXPECT_TRUE(Storage.exists(newPath.c_str()));
  EXPECT_TRUE(Storage.exists((newPath + ".meta.json").c_str()));
  EXPECT_TRUE(Storage.exists((newPath + ".key").c_str()));
  EXPECT_TRUE(Storage.exists((newPath + ".rights").c_str()));
  EXPECT_FALSE(Storage.exists((oldPath + ".key").c_str()));
  EXPECT_FALSE(Storage.exists((oldPath + ".rights").c_str()));
}
