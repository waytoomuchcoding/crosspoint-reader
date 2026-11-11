#include <Epub/BookMetadataCache.h>
#include <HalStorage.h>
#include <TextEncoding.h>
#include <TxtChapterDetector.h>
#include <TxtEncodingSupport.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

std::string decodeInSmallChunks(TextEncoding enc, const std::vector<uint8_t>& input, size_t chunkSize) {
  textencoding::TextDecoder decoder(enc);
  std::string out;
  for (size_t pos = 0; pos < input.size(); pos += chunkSize) {
    const size_t n = std::min(chunkSize, input.size() - pos);
    const bool isFinal = (pos + n == input.size());
    decoder.decodeChunk(input.data() + pos, n, isFinal, [&](const char* s, size_t len) { out.append(s, len); });
  }
  return out;
}

}  // namespace

TEST(TextEncodingTest, SplitBomCarryAcrossOneByteChunks) {
  // UTF-8 BOM + "A中"
  const std::vector<uint8_t> utf8WithBom = {0xEF, 0xBB, 0xBF, 'A', 0xE4, 0xB8, 0xAD};
  EXPECT_EQ(decodeInSmallChunks(TextEncoding::Utf8, utf8WithBom, 1), "A中");
  EXPECT_EQ(decodeInSmallChunks(TextEncoding::Utf8, utf8WithBom, 2), "A中");

  // Partial 2-byte prefix 0xEF 0xBB followed by 'X' is NOT a BOM -> recovers bytes cleanly as U+FFFD + 'X'
  const std::vector<uint8_t> falseBom = {0xEF, 0xBB, 'X'};
  const std::string falseBomDecoded = decodeInSmallChunks(TextEncoding::Utf8, falseBom, 1);
  EXPECT_NE(falseBomDecoded.find('X'), std::string::npos);

  // UTF-16LE BOM + "Hi"
  const std::vector<uint8_t> utf16LeWithBom = {0xFF, 0xFE, 'H', 0x00, 'i', 0x00};
  EXPECT_EQ(decodeInSmallChunks(TextEncoding::Utf16Le, utf16LeWithBom, 1), "Hi");

  // UTF-16BE BOM + "Hi"
  const std::vector<uint8_t> utf16BeWithBom = {0xFE, 0xFF, 0x00, 'H', 0x00, 'i'};
  EXPECT_EQ(decodeInSmallChunks(TextEncoding::Utf16Be, utf16BeWithBom, 1), "Hi");
}

TEST(TextEncodingTest, Gb18030TwoByteAndFourByteSplitBoundaries) {
  // "三体" in GBK/GB18030 = C8 FD CC E5, plus 4-byte U+20000 = 95 32 82 36
  const std::vector<uint8_t> gbBytes = {0xC8, 0xFD, 0xCC, 0xE5, ':', 0x95, 0x32, 0x82, 0x36};
  const std::string expected = "三体:\xF0\xA0\x80\x80";

  EXPECT_EQ(decodeInSmallChunks(TextEncoding::Gb18030, gbBytes, gbBytes.size()), expected);
  EXPECT_EQ(decodeInSmallChunks(TextEncoding::Gb18030, gbBytes, 1), expected);
  EXPECT_EQ(decodeInSmallChunks(TextEncoding::Gb18030, gbBytes, 2), expected);
  EXPECT_EQ(decodeInSmallChunks(TextEncoding::Gb18030, gbBytes, 3), expected);
}

TEST(TextEncodingTest, Big5StreamingAndSplitBoundaries) {
  // "繁體中文" in Big5 = C1 63 C5 E9 A4 A4 A4 E5
  const std::vector<uint8_t> big5Bytes = {0xC1, 0x63, 0xC5, 0xE9, 0xA4, 0xA4, 0xA4, 0xE5};
  bool hasCjk = false;
  EXPECT_EQ(textencoding::detectTextEncoding(big5Bytes.data(), big5Bytes.size(), &hasCjk), TextEncoding::Big5);
  EXPECT_TRUE(hasCjk);

  EXPECT_EQ(decodeInSmallChunks(TextEncoding::Big5, big5Bytes, 1), "繁體中文");
  EXPECT_EQ(decodeInSmallChunks(TextEncoding::Big5, big5Bytes, 3), "繁體中文");
}

TEST(TextEncodingTest, Utf16SurrogatePairsSplitAcrossChunkBoundaries) {
  // U+20000 = D840 DC00, followed by '!'
  const std::vector<uint8_t> le = {0x40, 0xD8, 0x00, 0xDC, '!', 0x00};
  const std::vector<uint8_t> be = {0xD8, 0x40, 0xDC, 0x00, 0x00, '!'};
  const std::string expected = "\xF0\xA0\x80\x80!";

  EXPECT_EQ(decodeInSmallChunks(TextEncoding::Utf16Le, le, 1), expected);
  EXPECT_EQ(decodeInSmallChunks(TextEncoding::Utf16Le, le, 2), expected);
  EXPECT_EQ(decodeInSmallChunks(TextEncoding::Utf16Le, le, 3), expected);
  EXPECT_EQ(decodeInSmallChunks(TextEncoding::Utf16Be, be, 1), expected);
  EXPECT_EQ(decodeInSmallChunks(TextEncoding::Utf16Be, be, 2), expected);
}

TEST(TextEncodingTest, SanitizesXml10IllegalControlBytesAndNuls) {
  const std::vector<uint8_t> raw = {'A', 0x00, 0x01, '\n', 0x0B, 'B'};
  textencoding::TextDecoder dec(TextEncoding::Utf8);
  const std::string out = dec.decodeToString(raw.data(), raw.size(), true);
  // NUL is stripped; 0x01 and 0x0B become U+FFFD ("\xEF\xBF\xBD"); '\n' is preserved
  EXPECT_EQ(out,
            "A\xEF\xBF\xBD\n\xEF\xBF\xBD"
            "B");
}

TEST(TextEncodingTest, MultiWindowDetectionToleratesCorruptHeaderBytes) {
  // Window 0 has ASCII + stray invalid byte; windows 1 & 2 have clean GB18030 Simplified Chinese prose
  const std::vector<uint8_t> w0 = {'T', 'X', 'T', 0xFF, '\n'};
  // "第一章 科学边界，汪淼觉得很奇怪。" in GB18030
  const std::vector<uint8_t> w1 = {0xB5, 0xDA, 0xD2, 0xBB, 0xD5, 0xC2, ' ',  0xBF, 0xC6, 0xD1, 0xA7,
                                   0xB1, 0xDF, 0xBD, 0xE7, 0xA3, 0xAC, 0xCD, 0xF4, 0xDF, 0xF5};
  const std::vector<uint8_t> w2 = {0xBE, 0xF5, 0xB5, 0xC3, 0xBA, 0xDC, 0xC6, 0xE6, 0xB9, 0xD6, 0xA1, 0xA3};
  const uint8_t* ptrs[3] = {w0.data(), w1.data(), w2.data()};
  const size_t lens[3] = {w0.size(), w1.size(), w2.size()};

  bool hasCjk = false;
  EXPECT_EQ(textencoding::detectTextEncodingMultiWindow(ptrs, lens, 3, &hasCjk), TextEncoding::Gb18030);
  EXPECT_TRUE(hasCjk);
}

TEST(TextEncodingTest, ChapterDetectorMatchesChineseHeadingsAndRejectsProse) {
  std::string title;
  EXPECT_TRUE(txtchapter::isChapterHeadingLine("  第一章 科学边界  ", &title));
  EXPECT_EQ(title, "第一章 科学边界");
  EXPECT_TRUE(txtchapter::isChapterHeadingLine("第128节 黑暗森林", &title));
  EXPECT_EQ(title, "第128节 黑暗森林");
  EXPECT_TRUE(txtchapter::isChapterHeadingLine("楔子", &title));
  EXPECT_EQ(title, "楔子");
  EXPECT_TRUE(txtchapter::isChapterHeadingLine("番外：观想之宙", &title));
  EXPECT_FALSE(txtchapter::isChapterHeadingLine("他第一次来到北京的时候，天正在下雨。", &title));
}

TEST(TextEncodingTest, EncodingOverrideAndRamPreviewDecode) {
  fake::reset();
  const std::string cacheDir = "/.crosspoint/txt_enc_test";
  Storage.mkdir("/.crosspoint");
  Storage.mkdir(cacheDir.c_str());

  EXPECT_EQ(txtenc::readEncodingOverride(cacheDir), TextEncoding::Auto);
  ASSERT_TRUE(txtenc::writeEncodingOverride(cacheDir, TextEncoding::Big5));
  EXPECT_EQ(txtenc::readEncodingOverride(cacheDir), TextEncoding::Big5);
  ASSERT_TRUE(txtenc::writeEncodingOverride(cacheDir, TextEncoding::Auto));
  EXPECT_EQ(txtenc::readEncodingOverride(cacheDir), TextEncoding::Auto);

  const std::string gbProse = "\xB5\xDA\xD2\xBB\xD5\xC2 \xBF\xC6\xD1\xA7\xB1\xDF\xBD\xE7\n";
  fake::add("/Books/sample.txt", gbProse);

  std::vector<uint8_t> rawSample;
  TextEncoding detected = TextEncoding::Utf8;
  size_t sampleOff = 0;
  ASSERT_TRUE(txtenc::loadRawPreviewSample("/Books/sample.txt", 0, rawSample, detected, sampleOff));
  EXPECT_EQ(detected, TextEncoding::Gb18030);

  std::string previewAuto;
  txtenc::decodePreviewSample(rawSample.data(), rawSample.size(), sampleOff, TextEncoding::Auto, detected, previewAuto);
  EXPECT_EQ(previewAuto, "第一章 科学边界\n");
}
