#include "TxtChapterDetector.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace txtchapter {

namespace {

uint32_t nextUtf8Cp(std::string_view s, size_t& pos) {
  if (pos >= s.size()) return 0;
  const uint8_t b0 = static_cast<uint8_t>(s[pos]);
  if (b0 < 0x80) {
    pos += 1;
    return b0;
  }
  if ((b0 & 0xE0) == 0xC0 && pos + 1 < s.size()) {
    const uint32_t cp = ((b0 & 0x1F) << 6) | (static_cast<uint8_t>(s[pos + 1]) & 0x3F);
    pos += 2;
    return cp;
  }
  if ((b0 & 0xF0) == 0xE0 && pos + 2 < s.size()) {
    const uint32_t cp = ((b0 & 0x0F) << 12) | ((static_cast<uint8_t>(s[pos + 1]) & 0x3F) << 6) |
                        (static_cast<uint8_t>(s[pos + 2]) & 0x3F);
    pos += 3;
    return cp;
  }
  if ((b0 & 0xF8) == 0xF0 && pos + 3 < s.size()) {
    const uint32_t cp = ((b0 & 0x07) << 18) | ((static_cast<uint8_t>(s[pos + 1]) & 0x3F) << 12) |
                        ((static_cast<uint8_t>(s[pos + 2]) & 0x3F) << 6) | (static_cast<uint8_t>(s[pos + 3]) & 0x3F);
    pos += 4;
    return cp;
  }
  pos += 1;
  return 0xFFFD;
}

bool isChineseNumeral(uint32_t cp) {
  if (cp >= '0' && cp <= '9') return true;
  if (cp >= 0xFF10 && cp <= 0xFF19) return true;  // Fullwidth digits ０-９
  switch (cp) {
    case 0x3007:  // 〇
    case 0x4E00:  // 一
    case 0x4E8C:  // 二
    case 0x4E09:  // 三
    case 0x56DB:  // 四
    case 0x4E94:  // 五
    case 0x516D:  // 六
    case 0x4E03:  // 七
    case 0x516B:  // 八
    case 0x4E5D:  // 九
    case 0x5341:  // 十
    case 0x767E:  // 百
    case 0x5343:  // 千
    case 0x4E07:  // 万
    case 0x842C:  // 萬
    case 0x96F6:  // 零
    case 0x4E24:  // 两
    case 0x5169:  // 兩
    case 0x58F9:  // 壹
    case 0x8D30:  // 贰
    case 0x53C1:  // 叁
    case 0x8086:  // 肆
    case 0x4F0D:  // 伍
    case 0x9646:  // 陆
    case 0x67D2:  // 柒
    case 0x634C:  // 捌
    case 0x7396:  // 玖
    case 0x62FE:  // 拾
    case 0x4F70:  // 佰
    case 0x4EDF:  // 仟
      return true;
    default:
      return false;
  }
}

bool isChapterUnit(uint32_t cp) {
  switch (cp) {
    case 0x7AE0:  // 章
    case 0x8282:  // 节
    case 0x7BC0:  // 節
    case 0x56DE:  // 回
    case 0x5377:  // 卷
    case 0x96C6:  // 集
    case 0x90E8:  // 部
    case 0x7BC7:  // 篇
    case 0x5E55:  // 幕
      return true;
    default:
      return false;
  }
}

bool isHeadingSeparatorOrEnd(std::string_view s, size_t pos) {
  if (pos >= s.size()) return true;
  size_t tmp = pos;
  const uint32_t cp = nextUtf8Cp(s, tmp);
  return cp == ' ' || cp == '\t' || cp == 0x3000 || cp == ':' || cp == 0xFF1A || cp == 0x3001 || cp == '-' ||
         cp == 0x2014 || cp == ']' || cp == 0x3011 || cp == ')' || cp == 0xFF09;
}

}  // namespace

bool isChapterHeadingLine(std::string_view utf8Line, std::string* cleanTitleOut) {
  // Trim leading whitespace (ASCII and U+3000 fullwidth space)
  size_t start = 0;
  while (start < utf8Line.size()) {
    size_t next = start;
    const uint32_t cp = nextUtf8Cp(utf8Line, next);
    if (cp == ' ' || cp == '\t' || cp == '\r' || cp == 0x3000) {
      start = next;
    } else {
      break;
    }
  }
  size_t end = utf8Line.size();
  while (end > start) {
    const uint8_t b = static_cast<uint8_t>(utf8Line[end - 1]);
    if (b == ' ' || b == '\t' || b == '\r') {
      end--;
    } else if (end >= start + 3 && static_cast<uint8_t>(utf8Line[end - 3]) == 0xE3 &&
               static_cast<uint8_t>(utf8Line[end - 2]) == 0x80 && static_cast<uint8_t>(utf8Line[end - 1]) == 0x80) {
      end -= 3;
    } else {
      break;
    }
  }

  const std::string_view trimmed = utf8Line.substr(start, end - start);
  if (trimmed.empty() || trimmed.size() > 120) {
    return false;
  }

  // Count total codepoints and reject long narrative sentences
  size_t cpCount = 0;
  for (size_t p = 0; p < trimmed.size();) {
    nextUtf8Cp(trimmed, p);
    cpCount++;
    if (cpCount > 45) return false;
  }

  // Check optional prefix ("正文", "【", "[")
  size_t scan = 0;
  if (trimmed.substr(0, 6) == "\xE6\xAD\xA3\xE6\x96\x87") {  // "正文"
    scan = 6;
    while (scan < trimmed.size()) {
      size_t next = scan;
      const uint32_t cp = nextUtf8Cp(trimmed, next);
      if (cp == ' ' || cp == '\t' || cp == 0x3000) {
        scan = next;
      } else {
        break;
      }
    }
  }
  {
    size_t next = scan;
    const uint32_t cp = nextUtf8Cp(trimmed, next);
    if (cp == '[' || cp == 0x3010 || cp == '(' || cp == 0xFF08) {
      scan = next;
    }
  }

  bool matched = false;
  size_t pos = scan;
  const uint32_t firstCp = nextUtf8Cp(trimmed, pos);

  // Pattern 1: 第 + [numerals] + [章节回卷集部篇]
  if (firstCp == 0x7B2C) {  // '第'
    size_t numCount = 0;
    while (pos < trimmed.size()) {
      size_t next = pos;
      const uint32_t cp = nextUtf8Cp(trimmed, next);
      if (isChineseNumeral(cp)) {
        numCount++;
        pos = next;
      } else if (numCount > 0 && isChapterUnit(cp)) {
        pos = next;
        matched = true;
        break;
      } else {
        break;
      }
    }
  }

  // Pattern 2: Standalone prologue/epilogue headers ("序章", "序言", "楔子", "引子", "尾声", "尾聲", "番外")
  if (!matched) {
    static constexpr const char* kStandaloneHeaders[] = {
        "\xE5\xBA\x8F\xE7\xAB\xA0",  // 序章
        "\xE5\xBA\x8F\xE8\xA8\x80",  // 序言
        "\xE6\xA5\x94\xE5\xAD\x90",  // 楔子
        "\xE5\xBC\x95\xE5\xAD\x90",  // 引子
        "\xE5\xB0\xBE\xE5\xA3\xB0",  // 尾声
        "\xE5\xB0\xBE\xE8\x81\xB2",  // 尾聲
        "\xE7\x95\xAA\xE5\xA4\x96",  // 番外
    };
    const std::string_view rest = trimmed.substr(scan);
    for (const char* hdr : kStandaloneHeaders) {
      const size_t hlen = strlen(hdr);
      if (rest.size() >= hlen && rest.substr(0, hlen) == hdr && isHeadingSeparatorOrEnd(rest, hlen)) {
        matched = true;
        break;
      }
    }
  }

  if (matched && cleanTitleOut) {
    cleanTitleOut->assign(trimmed.data(), trimmed.size());
  }
  return matched;
}

}  // namespace txtchapter
