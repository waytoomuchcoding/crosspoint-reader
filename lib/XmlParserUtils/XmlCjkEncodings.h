#pragma once

#include <TextEncoding.h>
#include <expat.h>

#include <cstdint>

namespace xml_cjk_detail {

inline bool equalsIgnoreCaseAscii(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    const char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a - 'A' + 'a') : *a;
    const char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b - 'A' + 'a') : *b;
    if (ca != cb) return false;
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

inline int XMLCALL expatConvertGbk(void* /*data*/, const char* s) {
  if (!s) return 0xFFFD;
  const uint8_t b1 = static_cast<uint8_t>(s[0]);
  const uint8_t b2 = static_cast<uint8_t>(s[1]);
  const uint16_t cp = textencoding::decodeGbk2Byte(b1, b2);
  return textencoding::isXml10ValidCodepoint(cp) ? static_cast<int>(cp) : 0xFFFD;
}

inline int XMLCALL expatConvertBig5(void* /*data*/, const char* s) {
  if (!s) return 0xFFFD;
  const uint8_t b1 = static_cast<uint8_t>(s[0]);
  const uint8_t b2 = static_cast<uint8_t>(s[1]);
  const uint16_t cp = textencoding::decodeBig52Byte(b1, b2);
  return textencoding::isXml10ValidCodepoint(cp) ? static_cast<int>(cp) : 0xFFFD;
}

inline int XMLCALL expatUnknownEncodingHandler(void* /*encodingHandlerData*/, const XML_Char* name,
                                               XML_Encoding* info) {
  if (!name || !info) return XML_STATUS_ERROR;

  const bool isGbk = equalsIgnoreCaseAscii(name, "gb2312") || equalsIgnoreCaseAscii(name, "gbk") ||
                     equalsIgnoreCaseAscii(name, "gb18030") || equalsIgnoreCaseAscii(name, "cp936") ||
                     equalsIgnoreCaseAscii(name, "csgb2312") || equalsIgnoreCaseAscii(name, "x-gbk") ||
                     equalsIgnoreCaseAscii(name, "gb_2312-80");
  const bool isBig5 = equalsIgnoreCaseAscii(name, "big5") || equalsIgnoreCaseAscii(name, "big5-hkscs") ||
                      equalsIgnoreCaseAscii(name, "cp950") || equalsIgnoreCaseAscii(name, "csbig5") ||
                      equalsIgnoreCaseAscii(name, "x-x-big5");

  if (!isGbk && !isBig5) {
    return XML_STATUS_ERROR;
  }

  for (int i = 0; i < 128; ++i) {
    info->map[i] = i;
  }
  for (int i = 128; i < 256; ++i) {
    info->map[i] = 0xFFFD;
  }

  if (isGbk) {
    info->map[0x80] = 0x20AC;
    for (int i = 0x81; i <= 0xFE; ++i) {
      info->map[i] = -2;
    }
    info->data = nullptr;
    info->convert = expatConvertGbk;
    info->release = nullptr;
    return XML_STATUS_OK;
  }

  for (int i = 0xA1; i <= 0xF9; ++i) {
    info->map[i] = -2;
  }
  info->data = nullptr;
  info->convert = expatConvertBig5;
  info->release = nullptr;
  return XML_STATUS_OK;
}

}  // namespace xml_cjk_detail

// Registers an Expat unknown-encoding handler for legacy EPUBs declaring gb2312 / gbk / gb18030 / big5.
inline void registerExpatCjkEncodingHandler(XML_Parser parser) {
  if (!parser) return;
  XML_SetUnknownEncodingHandler(parser, xml_cjk_detail::expatUnknownEncodingHandler, nullptr);
}
