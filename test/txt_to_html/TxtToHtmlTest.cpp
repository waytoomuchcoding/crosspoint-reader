#include <expat.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <string_view>

#include "Print.h"
#include "TextEncoding.h"
#include "TxtToHtml.h"
#include "XmlCjkEncodings.h"

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

const std::string kHeader =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<!-- TXT_CACHE_VERSION: 2 -->\n"
    "<!DOCTYPE html>\n<html>\n<head><title>test</title></head>\n<body>\n";
const std::string kFooter = "\n</body>\n</html>\n";

std::string convert(std::string_view content, TextEncoding enc = TextEncoding::Auto) {
  StringPrint out;
  EXPECT_TRUE(TxtToHtml::stream("test.txt", content, out, enc));
  return out.str;
}

bool isWellFormedXml(const std::string& xml) {
  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) return false;
  const bool ok = (XML_Parse(parser, xml.data(), static_cast<int>(xml.size()), 1) == XML_STATUS_OK);
  XML_ParserFree(parser);
  return ok;
}

TEST(TxtToHtmlTest, LeadingSpacesIndentation) {
  EXPECT_EQ(convert("  two spaces"), kHeader + "&#160;&#160;two spaces" + kFooter);
  EXPECT_EQ(convert("    four spaces"), kHeader + "&#160;&#160;&#160;&#160;four spaces" + kFooter);
  EXPECT_EQ(convert("Line 1\n   three spaces"), kHeader + "Line 1<br />&#160;&#160;&#160;three spaces" + kFooter);
}

TEST(TxtToHtmlTest, MidLineConsecutiveSpaces) {
  EXPECT_EQ(convert("One space"), kHeader + "One space" + kFooter);
  EXPECT_EQ(convert("Two  spaces"), kHeader + "Two&#160; spaces" + kFooter);
  EXPECT_EQ(convert("Three   spaces"), kHeader + "Three&#160;&#160; spaces" + kFooter);
  EXPECT_EQ(convert("Four    spaces"), kHeader + "Four&#160;&#160;&#160; spaces" + kFooter);
}

TEST(TxtToHtmlTest, PreservesCacheVersionTagsForBothFormats) {
  StringPrint out;
  ASSERT_TRUE(TxtToHtml::stream("test.MD", "text", out));
  EXPECT_EQ(out.str,
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!-- MD_CACHE_VERSION: 2 -->\n"
            "<!DOCTYPE html>\n<html>\n<head><title>test</title></head>\n<body>\ntext" +
                kFooter);
  EXPECT_NE(out.str.find(TxtToHtml::cacheVersionTag("test.MD")), std::string::npos);
  EXPECT_NE(convert("text").find(TxtToHtml::cacheVersionTag("test.txt")), std::string::npos);
}

TEST(TxtToHtmlTest, DetectsAndDecodesGbkAndGb18030) {
  // "第一章 陨落的天才\n这是中文测试内容，我们开始阅读。" in GBK
  const uint8_t gbkBytes[] = {
      0xB5, 0xDA, 0xD2, 0xBB, 0xD5, 0xC2, 0x20, 0xD4, 0xC9, 0xC2, 0xE4, 0xB5, 0xC4, 0xCC, 0xEC, 0xB2, 0xC5,
      0x0A, 0xD5, 0xE2, 0xCA, 0xC7, 0xD6, 0xD0, 0xCE, 0xC4, 0xB2, 0xE2, 0xCA, 0xD4, 0xC4, 0xDA, 0xC8, 0xDD,
      0xA3, 0xAC, 0xCE, 0xD2, 0xC3, 0xC7, 0xBF, 0xAA, 0xCA, 0xBC, 0xD4, 0xC4, 0xB6, 0xC1, 0xA1, 0xA3,
  };
  bool hasCjk = false;
  const TextEncoding detected = textencoding::detectTextEncoding(gbkBytes, sizeof(gbkBytes), &hasCjk);
  EXPECT_EQ(detected, TextEncoding::Gb18030);
  EXPECT_TRUE(hasCjk);

  const std::string html = convert(std::string_view(reinterpret_cast<const char*>(gbkBytes), sizeof(gbkBytes)));
  EXPECT_NE(html.find("<div id=\"ch0\"></div>第一章 陨落的天才<br />这是中文测试内容，我们开始阅读。"),
            std::string::npos);
  EXPECT_TRUE(isWellFormedXml(html));
}

TEST(TxtToHtmlTest, DetectsAndDecodesBig5) {
  // "第一章 江湖風雲\n這是一個繁體中文測試，我們在這裡閱讀。" in Big5
  const uint8_t big5Bytes[] = {
      0xB2, 0xC4, 0xA4, 0x40, 0xB3, 0xB9, 0x20, 0xA6, 0xBF, 0xB4, 0xF2, 0xAD, 0xB7, 0xB6, 0xB3, 0x0A, 0xB3, 0x6F,
      0xAC, 0x4F, 0xA4, 0x40, 0xAD, 0xD3, 0xC1, 0x63, 0xC5, 0xE9, 0xA4, 0xA4, 0xA4, 0xE5, 0xB4, 0xFA, 0xB8, 0xD5,
      0xA1, 0x41, 0xA7, 0xDA, 0xAD, 0xCC, 0xA6, 0x62, 0xB3, 0x6F, 0xB8, 0xCC, 0xBE, 0x5C, 0xC5, 0xAA, 0xA1, 0x43,
  };
  bool hasCjk = false;
  const TextEncoding detected = textencoding::detectTextEncoding(big5Bytes, sizeof(big5Bytes), &hasCjk);
  EXPECT_EQ(detected, TextEncoding::Big5);
  EXPECT_TRUE(hasCjk);

  const std::string html = convert(std::string_view(reinterpret_cast<const char*>(big5Bytes), sizeof(big5Bytes)));
  EXPECT_NE(html.find("<div id=\"ch0\"></div>第一章 江湖風雲<br />這是一個繁體中文測試，我們在這裡閱讀。"),
            std::string::npos);
  EXPECT_TRUE(isWellFormedXml(html));
}

TEST(TxtToHtmlTest, DetectsAndDecodesUtf16LeAndBe) {
  // UTF-16LE with BOM: "你好世界"
  const uint8_t utf16Le[] = {0xFF, 0xFE, 0x60, 0x4F, 0x7D, 0x59, 0x16, 0x4E, 0x4C, 0x75};
  EXPECT_EQ(textencoding::detectTextEncoding(utf16Le, sizeof(utf16Le)), TextEncoding::Utf16Le);
  const std::string htmlLe = convert(std::string_view(reinterpret_cast<const char*>(utf16Le), sizeof(utf16Le)));
  EXPECT_NE(htmlLe.find("你好世界"), std::string::npos);
  EXPECT_TRUE(isWellFormedXml(htmlLe));

  // UTF-16BE with BOM: "你好世界"
  const uint8_t utf16Be[] = {0xFE, 0xFF, 0x4F, 0x60, 0x59, 0x7D, 0x4E, 0x16, 0x75, 0x4C};
  EXPECT_EQ(textencoding::detectTextEncoding(utf16Be, sizeof(utf16Be)), TextEncoding::Utf16Be);
  const std::string htmlBe = convert(std::string_view(reinterpret_cast<const char*>(utf16Be), sizeof(utf16Be)));
  EXPECT_NE(htmlBe.find("你好世界"), std::string::npos);
  EXPECT_TRUE(isWellFormedXml(htmlBe));
}

TEST(TxtToHtmlTest, ToleratesCorruptedByteRangesAndInlineUtf8Banner) {
  // GBK text with:
  // 1. An injected UTF-8 banner on line 1 ("本书由测试网站整理发布\n")
  // 2. Corrupted bytes (0xFF, NUL 0x00, control char 0x07, truncated lead byte 0xFE before '\n')
  // 3. Valid GBK text after the corruption ("这是正常的中文内容。")
  std::string mixed = "本书由测试网站整理发布\n";
  const uint8_t corruptAndGbk[] = {
      0xFF, 0x00, 0x07, 0xFE, '\n', 0xD5, 0xE2, 0xCA, 0xC7, 0xD5, 0xFD, 0xB3, 0xA3,
      0xB5, 0xC4, 0xD6, 0xD0, 0xCE, 0xC4, 0xC4, 0xDA, 0xC8, 0xDD, 0xA1, 0xA3,
  };
  mixed.append(reinterpret_cast<const char*>(corruptAndGbk), sizeof(corruptAndGbk));

  const std::string html = convert(mixed, TextEncoding::Gb18030);
  // Both the inline UTF-8 banner and the GBK body are decoded cleanly, NUL is stripped, and XML is 100% valid
  EXPECT_NE(html.find("本书由测试网站整理发布<br />"), std::string::npos);
  EXPECT_NE(html.find("<br />这是正常的中文内容。"), std::string::npos);
  EXPECT_TRUE(isWellFormedXml(html));
}

TEST(TxtToHtmlTest, ExpatUnknownEncodingHandlerSupportsGbkAndBig5) {
  XML_Parser parser = XML_ParserCreate(nullptr);
  ASSERT_NE(parser, nullptr);
  registerExpatCjkEncodingHandler(parser);

  std::string captured;
  XML_SetUserData(parser, &captured);
  XML_SetCharacterDataHandler(parser, [](void* userData, const XML_Char* s, int len) {
    static_cast<std::string*>(userData)->append(s, static_cast<size_t>(len));
  });

  // <?xml version="1.0" encoding="gb2312"?><p>你好</p> in GB2312 (0xC4 0xE3 0xBA 0xC3)
  const char gbkXml[] = "<?xml version=\"1.0\" encoding=\"gb2312\"?><p>\xC4\xE3\xBA\xC3</p>";
  EXPECT_EQ(XML_Parse(parser, gbkXml, static_cast<int>(strlen(gbkXml)), 1), XML_STATUS_OK);
  EXPECT_EQ(captured, "你好");
  XML_ParserFree(parser);
}

}  // namespace
