#include "TextEncoding.h"

#include <algorithm>
#include <cstring>

#include "CjkEncodingTables.h"

namespace textencoding {

namespace {

// Sorted table of the most frequent Simplified/Traditional Chinese characters and CJK punctuation.
// Used by the multi-window encoding scorer to distinguish genuine GBK vs Big5 text even with corruption.
constexpr uint16_t kFrequentCjkSorted[] = {
    0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2026, 0x3000, 0x3001, 0x3002, 0x300A, 0x300B, 0x300C, 0x300D, 0x300E,
    0x300F, 0x3010, 0x3011, 0x4E00, 0x4E03, 0x4E07, 0x4E09, 0x4E0A, 0x4E0B, 0x4E0D, 0x4E0E, 0x4E16, 0x4E1A, 0x4E1C,
    0x4E24, 0x4E2A, 0x4E2D, 0x4E3A, 0x4E3B, 0x4E48, 0x4E4B, 0x4E5D, 0x4E5F, 0x4E66, 0x4E86, 0x4E8B, 0x4E8C, 0x4E9B,
    0x4EA4, 0x4EA7, 0x4EAC, 0x4EB2, 0x4EBA, 0x4EC0, 0x4ECA, 0x4ECE, 0x4ED6, 0x4EE3, 0x4EE5, 0x4EEC, 0x4EF6, 0x4EFB,
    0x4F17, 0x4F1A, 0x4F20, 0x4F46, 0x4F4D, 0x4F4F, 0x4F53, 0x4F55, 0x4F5C, 0x4F60, 0x4F7F, 0x4FBF, 0x4FDD, 0x4FE1,
    0x5011, 0x500B, 0x505A, 0x513F, 0x5148, 0x5149, 0x5165, 0x5168, 0x5169, 0x516C, 0x5171, 0x5173, 0x5176, 0x5185,
    0x518D, 0x51E0, 0x51FA, 0x5206, 0x5219, 0x5229, 0x522B, 0x5230, 0x5236, 0x524D, 0x529B, 0x529E, 0x52A0, 0x52A1,
    0x52A8, 0x5316, 0x5317, 0x5341, 0x534E, 0x5357, 0x5374, 0x539F, 0x53BB, 0x53C8, 0x53CD, 0x53D1, 0x53D6, 0x53D7,
    0x53D8, 0x53E3, 0x53EA, 0x53EB, 0x53EF, 0x53F0, 0x53F2, 0x53F7, 0x53F8, 0x5403, 0x5404, 0x5408, 0x540C, 0x540D,
    0x540E, 0x5411, 0x542C, 0x544A, 0x5458, 0x547D, 0x548C, 0x54EA, 0x554F, 0x56DB, 0x56DE, 0x56E0, 0x56FD, 0x570B,
    0x5728, 0x5730, 0x573A, 0x57CE, 0x5916, 0x591A, 0x5927, 0x5929, 0x592A, 0x5934, 0x5973, 0x5979, 0x597D, 0x5982,
    0x5B50, 0x5B57, 0x5B66, 0x5B83, 0x5B89, 0x5B8C, 0x5B9A, 0x5B9E, 0x5BA2, 0x5BB6, 0x5BFE, 0x5C01, 0x5C06, 0x5C0F,
    0x5C11, 0x5C14, 0x5C31, 0x5C71, 0x5DE5, 0x5DF1, 0x5DF2, 0x5E02, 0x5E08, 0x5E26, 0x5E38, 0x5E73, 0x5E74, 0x5E76,
    0x5E94, 0x5EA6, 0x5EFA, 0x5F00, 0x5F20, 0x5F53, 0x5F80, 0x5F88, 0x5F8C, 0x5F97, 0x5FC3, 0x5FC5, 0x600E, 0x601D,
    0x6027, 0x603B, 0x606F, 0x60C5, 0x60F3, 0x610F, 0x611F, 0x6210, 0x6211, 0x6216, 0x6218, 0x6240, 0x624B, 0x624D,
    0x6253, 0x628A, 0x62A5, 0x62C9, 0x6307, 0x63A5, 0x63D0, 0x653F, 0x6545, 0x6559, 0x6570, 0x6587, 0x65B0, 0x65B9,
    0x65E0, 0x65E5, 0x65F6, 0x660E, 0x662F, 0x6642, 0x66F4, 0x66F8, 0x6700, 0x6708, 0x6709, 0x670B, 0x670D, 0x671B,
    0x671F, 0x672C, 0x673A, 0x6761, 0x6765, 0x6771, 0x6797, 0x679C, 0x6837, 0x6839, 0x6B21, 0x6B64, 0x6B63, 0x6B7B,
    0x6C11, 0x6C14, 0x6C34, 0x6C42, 0x6C92, 0x6CA1, 0x6CD5, 0x6D3B, 0x6D41, 0x6D77, 0x6E05, 0x70B9, 0x7136, 0x7231,
    0x7236, 0x7269, 0x7279, 0x738B, 0x73B0, 0x73FE, 0x7406, 0x751F, 0x7528, 0x7531, 0x7535, 0x754C, 0x767D, 0x767E,
    0x7684, 0x76EE, 0x76F4, 0x76F8, 0x770B, 0x771F, 0x773C, 0x7740, 0x77E5, 0x793E, 0x795E, 0x79CD, 0x79D1, 0x7A0B,
    0x7ACB, 0x7AE0, 0x7B11, 0x7B2C, 0x7B49, 0x7B97, 0x7BA1, 0x7CFB, 0x7D66, 0x7D93, 0x7EDF, 0x7ECF, 0x7ED9, 0x7FA4,
    0x8001, 0x8005, 0x800C, 0x80FD, 0x81EA, 0x81F3, 0x8272, 0x8282, 0x897F, 0x8981, 0x89C1, 0x89C9, 0x89E3, 0x8A00,
    0x8A71, 0x8AA4, 0x8AAA, 0x8ACB, 0x8AD6, 0x8B93, 0x8BA1, 0x8BA4, 0x8BA9, 0x8BB0, 0x8BB8, 0x8BBA, 0x8BBE, 0x8BDD,
    0x8BE5, 0x8BF4, 0x8BF7, 0x8D70, 0x8D77, 0x8EAB, 0x8F66, 0x8F6C, 0x8F7B, 0x8FD0, 0x8FD1, 0x8FD8, 0x8FD9, 0x8FDB,
    0x8FDC, 0x8FDE, 0x9009, 0x901A, 0x9020, 0x9053, 0x90A3, 0x90E8, 0x90FD, 0x91CC, 0x91CD, 0x91CF, 0x91D1, 0x957F,
    0x9580, 0x958B, 0x9593, 0x95E8, 0x95EE, 0x95F4, 0x961F, 0x9633, 0x9662, 0x96BE, 0x9762, 0x97F3, 0x9879, 0x9898,
    0x98A8, 0x98CE, 0x99AC, 0x9A6C, 0x9AD8, 0x9EDE, 0xFF01, 0xFF08, 0xFF09, 0xFF0C, 0xFF1A, 0xFF1B, 0xFF1F,
};

bool isFrequentCjkOrPunct(uint32_t cp) {
  if (cp > 0xFFFF) return false;
  return std::binary_search(std::begin(kFrequentCjkSorted), std::end(kFrequentCjkSorted), static_cast<uint16_t>(cp));
}

bool isGeneralCjkCodepoint(uint32_t cp) {
  return (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0x3400 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x2FA1F);
}

// Checks if `data[pos..len)` starts with at least `minChars` consecutive 3-byte UTF-8 CJK characters.
// Returns the total byte length of the valid UTF-8 CJK/ASCII line segment if matched, or 0 otherwise.
size_t matchInlineUtf8CjkRun(const uint8_t* data, size_t len, size_t minCjkChars = 3) {
  size_t i = 0;
  size_t cjkCount = 0;
  while (i < len) {
    uint8_t b0 = data[i];
    if (b0 == '\n' || b0 == '\r') {
      break;
    }
    if (b0 < 0x80) {
      i++;
      continue;
    }
    if (b0 >= 0xE3 && b0 <= 0xEF && i + 2 < len) {
      uint8_t b1 = data[i + 1];
      uint8_t b2 = data[i + 2];
      if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
        uint32_t cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
        if (isGeneralCjkCodepoint(cp) || (cp >= 0x2000 && cp <= 0x206F)) {
          cjkCount++;
          i += 3;
          continue;
        }
      }
    }
    // Non-UTF8 byte encountered before end of line
    return (cjkCount >= minCjkChars) ? i : 0;
  }
  return (cjkCount >= minCjkChars) ? i : 0;
}

}  // namespace

const char* encodingShortName(TextEncoding enc) {
  switch (enc) {
    case TextEncoding::Auto:
      return "Auto";
    case TextEncoding::Utf8:
      return "UTF-8";
    case TextEncoding::Gb18030:
      return "GB18030 / GBK";
    case TextEncoding::Big5:
      return "Big5";
    case TextEncoding::Utf16Le:
      return "UTF-16LE";
    case TextEncoding::Utf16Be:
      return "UTF-16BE";
    default:
      return "UTF-8";
  }
}

uint16_t decodeGbk2Byte(uint8_t b1, uint8_t b2) {
  if (b1 < 0x81 || b1 > 0xFE) return 0xFFFD;
  if (b2 < 0x40 || b2 > 0xFE || b2 == 0x7F) return 0xFFFD;
  const size_t leadIdx = static_cast<size_t>(b1 - 0x81);
  const size_t trailIdx = static_cast<size_t>(b2 - 0x40 - (b2 > 0x7F ? 1 : 0));
  const size_t idx = leadIdx * 190 + trailIdx;
  if (idx >= tables::kGbk2ByteCount) return 0xFFFD;
  return tables::kGbk2ByteToUcs2[idx];
}

uint32_t decodeGb180304Byte(uint8_t b1, uint8_t b2, uint8_t b3, uint8_t b4) {
  if (b1 < 0x81 || b1 > 0xFE || b2 < 0x30 || b2 > 0x39 || b3 < 0x81 || b3 > 0xFE || b4 < 0x30 || b4 > 0x39) {
    return 0xFFFD;
  }
  const uint32_t p = ((static_cast<uint32_t>(b1 - 0x81) * 10u + static_cast<uint32_t>(b2 - 0x30)) * 1260u) +
                     (static_cast<uint32_t>(b3 - 0x81) * 10u) + static_cast<uint32_t>(b4 - 0x30);

  if (p < 39420u) {
    size_t lo = 0;
    size_t hi = tables::kGb18030BmpRunCount;
    while (lo + 1 < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      if (tables::kGb18030BmpRuns[mid].startPointer <= p) {
        lo = mid;
      } else {
        hi = mid;
      }
    }
    const auto& run = tables::kGb18030BmpRuns[lo];
    return static_cast<uint32_t>(run.startCodepoint) + (p - run.startPointer);
  }

  if (p >= 189000u && p <= 1237575u) {
    return 0x10000u + (p - 189000u);
  }

  return 0xFFFD;
}

uint16_t decodeBig52Byte(uint8_t b1, uint8_t b2) {
  if (b1 < 0xA1 || b1 > 0xF9) return 0xFFFD;
  size_t trailIdx = 0;
  if (b2 >= 0x40 && b2 <= 0x7E) {
    trailIdx = static_cast<size_t>(b2 - 0x40);
  } else if (b2 >= 0xA1 && b2 <= 0xFE) {
    trailIdx = 63u + static_cast<size_t>(b2 - 0xA1);
  } else {
    return 0xFFFD;
  }
  const size_t idx = static_cast<size_t>(b1 - 0xA1) * 157u + trailIdx;
  if (idx >= tables::kBig52ByteCount) return 0xFFFD;
  return tables::kBig52ByteToUcs2[idx];
}

bool isXml10ValidCodepoint(uint32_t cp) {
  return cp == 0x09 || cp == 0x0A || cp == 0x0D || (cp >= 0x20 && cp <= 0xD7FF) || (cp >= 0xE000 && cp <= 0xFFFD) ||
         (cp >= 0x10000 && cp <= 0x10FFFF);
}

size_t encodeUtf8(uint32_t cp, char out[4]) {
  if (cp <= 0x7F) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp <= 0x7FF) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp <= 0xFFFF) {
    if (cp >= 0xD800 && cp <= 0xDFFF) {
      cp = 0xFFFD;
    }
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  if (cp <= 0x10FFFF) {
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
  }
  return encodeUtf8(0xFFFD, out);
}

TextEncoding detectTextEncodingMultiWindow(const uint8_t* const* windows, const size_t* lengths, size_t windowCount,
                                           bool* hasCjkOut) {
  if (hasCjkOut) *hasCjkOut = false;
  if (!windows || !lengths || windowCount == 0) return TextEncoding::Utf8;

  int32_t scoreUtf8 = 0;
  int32_t scoreGbk = 0;
  int32_t scoreBig5 = 0;
  int32_t scoreUtf16Le = 0;
  int32_t scoreUtf16Be = 0;

  bool hasHighBytes = false;
  bool hasNulBytes = false;
  size_t cjkUtf8Count = 0;
  size_t cjkGbkCount = 0;
  size_t cjkBig5Count = 0;
  size_t cjkUtf16LeCount = 0;
  size_t cjkUtf16BeCount = 0;

  // Check BOM in first window
  if (lengths[0] >= 4 && windows[0][0] == 0x84 && windows[0][1] == 0x31 && windows[0][2] == 0x95 &&
      windows[0][3] == 0x33) {
    if (hasCjkOut) *hasCjkOut = true;
    return TextEncoding::Gb18030;
  }
  if (lengths[0] >= 2 && windows[0][0] == 0xFF && windows[0][1] == 0xFE) {
    scoreUtf16Le += 120;
  } else if (lengths[0] >= 2 && windows[0][0] == 0xFE && windows[0][1] == 0xFF) {
    scoreUtf16Be += 120;
  } else if (lengths[0] >= 3 && windows[0][0] == 0xEF && windows[0][1] == 0xBB && windows[0][2] == 0xBF) {
    scoreUtf8 += 45;
  }

  for (size_t w = 0; w < windowCount; ++w) {
    const uint8_t* data = windows[w];
    const size_t len = lengths[w];
    if (!data || len == 0) continue;

    for (size_t i = 0; i < len; ++i) {
      if (data[i] >= 0x80) hasHighBytes = true;
      if (data[i] == 0x00) hasNulBytes = true;
    }

    // 1. Score UTF-8 with 1-byte error resynchronization
    for (size_t i = 0; i < len;) {
      const uint8_t b0 = data[i];
      if (b0 < 0x80) {
        if (b0 == 0x00) scoreUtf8 -= 4;
        i++;
        continue;
      }
      if (b0 >= 0xC2 && b0 <= 0xDF) {
        if (i + 1 >= len) break;
        if ((data[i + 1] & 0xC0) == 0x80) {
          scoreUtf8 += 3;
          i += 2;
          continue;
        }
      } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        if (i + 2 >= len) break;
        const uint8_t b1 = data[i + 1];
        const uint8_t b2 = data[i + 2];
        const bool valid1 = (b0 == 0xE0)   ? (b1 >= 0xA0 && b1 <= 0xBF)
                            : (b0 == 0xED) ? (b1 >= 0x80 && b1 <= 0x9F)
                                           : ((b1 & 0xC0) == 0x80);
        if (valid1 && (b2 & 0xC0) == 0x80) {
          const uint32_t cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
          if (isFrequentCjkOrPunct(cp)) {
            scoreUtf8 += 8;
            cjkUtf8Count++;
          } else if (isGeneralCjkCodepoint(cp)) {
            scoreUtf8 += 6;
            cjkUtf8Count++;
          } else {
            scoreUtf8 += 3;
          }
          i += 3;
          continue;
        }
      } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        if (i + 3 >= len) break;
        const uint8_t b1 = data[i + 1];
        const uint8_t b2 = data[i + 2];
        const uint8_t b3 = data[i + 3];
        const bool valid1 = (b0 == 0xF0)   ? (b1 >= 0x90 && b1 <= 0xBF)
                            : (b0 == 0xF4) ? (b1 >= 0x80 && b1 <= 0x8F)
                                           : ((b1 & 0xC0) == 0x80);
        if (valid1 && (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80) {
          scoreUtf8 += 4;
          i += 4;
          continue;
        }
      }
      scoreUtf8 -= 6;
      i++;
    }

    // 2. Score GB18030 / GBK with 1-byte error resynchronization
    for (size_t i = 0; i < len;) {
      const uint8_t b0 = data[i];
      if (b0 < 0x80) {
        if (b0 == 0x00) scoreGbk -= 4;
        i++;
        continue;
      }
      if (b0 == 0x80) {
        scoreGbk += 1;
        i++;
        continue;
      }
      if (b0 >= 0x81 && b0 <= 0xFE) {
        if (i + 1 >= len) break;
        const uint8_t b1 = data[i + 1];
        if (b1 >= 0x30 && b1 <= 0x39) {
          if (i + 3 >= len) break;
          const uint32_t cp = decodeGb180304Byte(b0, b1, data[i + 2], data[i + 3]);
          if (cp != 0xFFFD) {
            scoreGbk += 3;
            cjkGbkCount++;
            i += 4;
            continue;
          }
        } else {
          const uint16_t cp = decodeGbk2Byte(b0, b1);
          if (cp != 0xFFFD) {
            cjkGbkCount++;
            if (isFrequentCjkOrPunct(cp)) {
              scoreGbk += 7;
            } else if ((b0 >= 0xB0 && b0 <= 0xD7 && b1 >= 0xA1 && b1 <= 0xFE) ||
                       (b0 >= 0xA1 && b0 <= 0xA3 && b1 >= 0xA1 && b1 <= 0xFE)) {
              scoreGbk += 4;
            } else if (b0 >= 0xD8 && b0 <= 0xF7 && b1 >= 0xA1 && b1 <= 0xFE) {
              scoreGbk += 2;
            }
            i += 2;
            continue;
          }
        }
      }
      scoreGbk -= 7;
      i++;
    }

    // 3. Score Big5 with 1-byte error resynchronization
    for (size_t i = 0; i < len;) {
      const uint8_t b0 = data[i];
      if (b0 < 0x80) {
        if (b0 == 0x00) scoreBig5 -= 4;
        i++;
        continue;
      }
      if (b0 >= 0xA1 && b0 <= 0xF9) {
        if (i + 1 >= len) break;
        const uint8_t b1 = data[i + 1];
        const uint16_t cp = decodeBig52Byte(b0, b1);
        if (cp != 0xFFFD) {
          cjkBig5Count++;
          if (isFrequentCjkOrPunct(cp)) {
            scoreBig5 += 7;
          } else if (b0 >= 0xA1 && b0 <= 0xC6) {
            scoreBig5 += 4;
          } else {
            scoreBig5 += 1;
          }
          i += 2;
          continue;
        }
      }
      scoreBig5 -= 7;
      i++;
    }

    // 4. Score UTF-16LE and UTF-16BE
    const size_t pairEnd = len & ~static_cast<size_t>(1);
    for (size_t i = 0; i < pairEnd; i += 2) {
      const uint16_t uLe = static_cast<uint16_t>(data[i]) | (static_cast<uint16_t>(data[i + 1]) << 8);
      const uint16_t uBe = (static_cast<uint16_t>(data[i]) << 8) | static_cast<uint16_t>(data[i + 1]);

      auto scoreUtf16Unit = [](uint16_t u, int32_t& score, size_t& cjkCount) {
        if (u == 0xFEFF) {
          score += 20;
        } else if (u == 0x0009 || u == 0x000A || u == 0x000D || (u >= 0x0020 && u <= 0x007E)) {
          score += 4;
        } else if (isFrequentCjkOrPunct(u)) {
          score += 7;
          cjkCount++;
        } else if ((u >= 0x3000 && u <= 0x303F) || (u >= 0x4E00 && u <= 0x9FFF) || (u >= 0xFF00 && u <= 0xFFEF)) {
          score += 2;
          cjkCount++;
        } else {
          score -= 6;
        }
      };
      scoreUtf16Unit(uLe, scoreUtf16Le, cjkUtf16LeCount);
      scoreUtf16Unit(uBe, scoreUtf16Be, cjkUtf16BeCount);
    }
  }

  if (!hasHighBytes && !hasNulBytes) {
    return TextEncoding::Utf8;
  }

  // Without a BOM or NUL bytes, require a strong lead for UTF-16 over 8-bit encodings
  if (!hasNulBytes && scoreUtf16Le < 100) scoreUtf16Le -= 80;
  if (!hasNulBytes && scoreUtf16Be < 100) scoreUtf16Be -= 80;

  TextEncoding best = TextEncoding::Utf8;
  int32_t bestScore = scoreUtf8;

  if (scoreGbk > bestScore) {
    bestScore = scoreGbk;
    best = TextEncoding::Gb18030;
  }
  if (scoreBig5 > bestScore) {
    bestScore = scoreBig5;
    best = TextEncoding::Big5;
  }
  if (scoreUtf16Le > bestScore) {
    bestScore = scoreUtf16Le;
    best = TextEncoding::Utf16Le;
  }
  if (scoreUtf16Be > bestScore) {
    bestScore = scoreUtf16Be;
    best = TextEncoding::Utf16Be;
  }

  if (hasCjkOut) {
    switch (best) {
      case TextEncoding::Utf8:
        *hasCjkOut = cjkUtf8Count > 0;
        break;
      case TextEncoding::Gb18030:
        *hasCjkOut = cjkGbkCount > 0;
        break;
      case TextEncoding::Big5:
        *hasCjkOut = cjkBig5Count > 0;
        break;
      case TextEncoding::Utf16Le:
        *hasCjkOut = cjkUtf16LeCount > 0;
        break;
      case TextEncoding::Utf16Be:
        *hasCjkOut = cjkUtf16BeCount > 0;
        break;
      default:
        break;
    }
  }

  return best;
}

TextEncoding detectTextEncoding(const uint8_t* data, size_t len, bool* hasCjkOut) {
  return detectTextEncodingMultiWindow(&data, &len, 1, hasCjkOut);
}

TextDecoder::TextDecoder(TextEncoding encoding) : encoding_(encoding) {}

void TextDecoder::reset(TextEncoding encoding) {
  encoding_ = encoding;
  carryLen_ = 0;
  bomChecked_ = false;
  sawCjk_ = false;
  pendingHighSurrogate_ = 0;
}

std::string TextDecoder::decodeToString(const uint8_t* data, size_t len, bool isFinalChunk) {
  std::string out;
  out.reserve(len);
  decodeChunk(data, len, isFinalChunk, [&out](const char* utf8, size_t n) { out.append(utf8, n); });
  return out;
}

void TextDecoder::decodeInternalImpl(const uint8_t* data, size_t len, bool isFinalChunk, CodepointCallback cb,
                                     void* ctx) {
  if (!data && len > 0) return;

  size_t pos = 0;

  auto getByte = [&](size_t idx) -> uint8_t {
    if (idx < carryLen_) return carry_[idx];
    return data[idx - carryLen_];
  };
  const size_t totalLen = carryLen_ + len;

  if (!bomChecked_) {
    if (totalLen < 4 && !isFinalChunk) {
      for (size_t i = 0; i < len; ++i) {
        carry_[carryLen_++] = data[i];
      }
      return;
    }
    bomChecked_ = true;
    if (encoding_ == TextEncoding::Auto) {
      if (carryLen_ == 0) {
        encoding_ = detectTextEncoding(data, len);
      } else {
        uint8_t probe[64];
        const size_t probeCarry = std::min<size_t>(carryLen_, sizeof(probe));
        memcpy(probe, carry_, probeCarry);
        const size_t probeData = std::min<size_t>(len, sizeof(probe) - probeCarry);
        if (probeData > 0) {
          memcpy(probe + probeCarry, data, probeData);
        }
        encoding_ = detectTextEncoding(probe, probeCarry + probeData);
      }
    }
    if (totalLen >= 3 && getByte(0) == 0xEF && getByte(1) == 0xBB && getByte(2) == 0xBF) {
      pos = 3;
    } else if (totalLen >= 4 && getByte(0) == 0x84 && getByte(1) == 0x31 && getByte(2) == 0x95 && getByte(3) == 0x33) {
      pos = 4;
    } else if (totalLen >= 2 && ((encoding_ == TextEncoding::Utf16Le && getByte(0) == 0xFF && getByte(1) == 0xFE) ||
                                 (encoding_ == TextEncoding::Utf16Be && getByte(0) == 0xFE && getByte(1) == 0xFF))) {
      pos = 2;
    }
  }

  if (encoding_ == TextEncoding::Utf16Le || encoding_ == TextEncoding::Utf16Be) {
    while (pos + 1 < totalLen) {
      const uint8_t b0 = getByte(pos);
      const uint8_t b1 = getByte(pos + 1);
      pos += 2;
      const uint16_t u = (encoding_ == TextEncoding::Utf16Le)
                             ? (static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8))
                             : ((static_cast<uint16_t>(b0) << 8) | static_cast<uint16_t>(b1));
      if (pendingHighSurrogate_ != 0) {
        if (u >= 0xDC00 && u <= 0xDFFF) {
          const uint32_t cp = 0x10000u + ((static_cast<uint32_t>(pendingHighSurrogate_ - 0xD800) << 10) | (u - 0xDC00));
          pendingHighSurrogate_ = 0;
          cb(ctx, cp);
          continue;
        }
        cb(ctx, 0xFFFD);
        pendingHighSurrogate_ = 0;
      }
      if (u >= 0xD800 && u <= 0xDBFF) {
        pendingHighSurrogate_ = u;
      } else if (u >= 0xDC00 && u <= 0xDFFF) {
        cb(ctx, 0xFFFD);
      } else {
        cb(ctx, u);
      }
    }
    uint8_t newCarry[4];
    uint8_t newCarryLen = 0;
    while (pos < totalLen) {
      newCarry[newCarryLen++] = getByte(pos++);
    }
    if (isFinalChunk) {
      if (pendingHighSurrogate_ != 0 || newCarryLen > 0) {
        cb(ctx, 0xFFFD);
      }
      pendingHighSurrogate_ = 0;
      carryLen_ = 0;
    } else {
      memcpy(carry_, newCarry, newCarryLen);
      carryLen_ = newCarryLen;
    }
    return;
  }

  if (encoding_ == TextEncoding::Gb18030 || encoding_ == TextEncoding::Big5) {
    const bool isGbk = (encoding_ == TextEncoding::Gb18030);
    while (pos < totalLen) {
      const uint8_t b0 = getByte(pos);
      if (b0 < 0x80) {
        cb(ctx, b0);
        pos++;
        continue;
      }

      // Check for an injected UTF-8 banner segment (e.g. >= 3 consecutive 3-byte UTF-8 CJK chars on this line)
      if (pos >= carryLen_ && b0 >= 0xE3 && b0 <= 0xEF) {
        const size_t rawOffset = pos - carryLen_;
        const size_t utf8Run = matchInlineUtf8CjkRun(data + rawOffset, len - rawOffset, 3);
        if (utf8Run > 0) {
          const size_t runEnd = pos + utf8Run;
          while (pos < runEnd) {
            const uint8_t u0 = getByte(pos);
            if (u0 < 0x80) {
              cb(ctx, u0);
              pos++;
            } else {
              const uint32_t cp = ((u0 & 0x0F) << 12) | ((getByte(pos + 1) & 0x3F) << 6) | (getByte(pos + 2) & 0x3F);
              cb(ctx, cp);
              pos += 3;
            }
          }
          continue;
        }
      }

      if (isGbk && b0 == 0x80) {
        cb(ctx, 0x20AC);
        pos++;
        continue;
      }

      if (pos + 1 >= totalLen) {
        if (!isFinalChunk) break;
        cb(ctx, 0xFFFD);
        pos++;
        continue;
      }

      const uint8_t b1 = getByte(pos + 1);
      if (isGbk && b0 >= 0x81 && b0 <= 0xFE && b1 >= 0x30 && b1 <= 0x39) {
        if (pos + 3 >= totalLen) {
          if (!isFinalChunk) break;
          cb(ctx, 0xFFFD);
          pos++;
          continue;
        }
        const uint32_t cp = decodeGb180304Byte(b0, b1, getByte(pos + 2), getByte(pos + 3));
        if (cp != 0xFFFD) {
          cb(ctx, cp);
          pos += 4;
          continue;
        }
      }

      const uint16_t cp = isGbk ? decodeGbk2Byte(b0, b1) : decodeBig52Byte(b0, b1);
      if (cp != 0xFFFD) {
        cb(ctx, cp);
        pos += 2;
      } else {
        cb(ctx, 0xFFFD);
        // Never swallow an ASCII byte (such as '\n', '\r', ' ', '<') when resynchronizing after a corrupt lead byte
        pos += (b1 < 0x80) ? 1 : 2;
      }
    }

    uint8_t newCarry[4];
    uint8_t newCarryLen = 0;
    while (pos < totalLen) {
      newCarry[newCarryLen++] = getByte(pos++);
    }
    memcpy(carry_, newCarry, newCarryLen);
    carryLen_ = newCarryLen;
    return;
  }

  // Default: UTF-8 with WHATWG maximal-subpart resynchronization
  while (pos < totalLen) {
    const uint8_t b0 = getByte(pos);
    if (b0 < 0x80) {
      cb(ctx, b0);
      pos++;
      continue;
    }

    if (b0 >= 0xC2 && b0 <= 0xDF) {
      if (pos + 1 >= totalLen) {
        if (!isFinalChunk) break;
        cb(ctx, 0xFFFD);
        pos++;
        continue;
      }
      const uint8_t b1 = getByte(pos + 1);
      if ((b1 & 0xC0) == 0x80) {
        cb(ctx, ((b0 & 0x1F) << 6) | (b1 & 0x3F));
        pos += 2;
      } else {
        cb(ctx, 0xFFFD);
        pos += 1;
      }
      continue;
    }

    if (b0 >= 0xE0 && b0 <= 0xEF) {
      if (pos + 1 >= totalLen) {
        if (!isFinalChunk) break;
        cb(ctx, 0xFFFD);
        pos++;
        continue;
      }
      const uint8_t b1 = getByte(pos + 1);
      const bool valid1 = (b0 == 0xE0)   ? (b1 >= 0xA0 && b1 <= 0xBF)
                          : (b0 == 0xED) ? (b1 >= 0x80 && b1 <= 0x9F)
                                         : ((b1 & 0xC0) == 0x80);
      if (!valid1) {
        cb(ctx, 0xFFFD);
        pos += 1;
        continue;
      }
      if (pos + 2 >= totalLen) {
        if (!isFinalChunk) break;
        cb(ctx, 0xFFFD);
        pos += 2;
        continue;
      }
      const uint8_t b2 = getByte(pos + 2);
      if ((b2 & 0xC0) == 0x80) {
        cb(ctx, ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F));
        pos += 3;
      } else {
        cb(ctx, 0xFFFD);
        pos += 2;
      }
      continue;
    }

    if (b0 >= 0xF0 && b0 <= 0xF4) {
      if (pos + 1 >= totalLen) {
        if (!isFinalChunk) break;
        cb(ctx, 0xFFFD);
        pos++;
        continue;
      }
      const uint8_t b1 = getByte(pos + 1);
      const bool valid1 = (b0 == 0xF0)   ? (b1 >= 0x90 && b1 <= 0xBF)
                          : (b0 == 0xF4) ? (b1 >= 0x80 && b1 <= 0x8F)
                                         : ((b1 & 0xC0) == 0x80);
      if (!valid1) {
        cb(ctx, 0xFFFD);
        pos += 1;
        continue;
      }
      if (pos + 2 >= totalLen) {
        if (!isFinalChunk) break;
        cb(ctx, 0xFFFD);
        pos += 2;
        continue;
      }
      const uint8_t b2 = getByte(pos + 2);
      if ((b2 & 0xC0) != 0x80) {
        cb(ctx, 0xFFFD);
        pos += 2;
        continue;
      }
      if (pos + 3 >= totalLen) {
        if (!isFinalChunk) break;
        cb(ctx, 0xFFFD);
        pos += 3;
        continue;
      }
      const uint8_t b3 = getByte(pos + 3);
      if ((b3 & 0xC0) == 0x80) {
        cb(ctx, ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F));
        pos += 4;
      } else {
        cb(ctx, 0xFFFD);
        pos += 3;
      }
      continue;
    }

    // Stray continuation byte or 0xC0/0xC1/0xF5..0xFF
    cb(ctx, 0xFFFD);
    pos++;
  }

  uint8_t newCarry[4];
  uint8_t newCarryLen = 0;
  while (pos < totalLen) {
    newCarry[newCarryLen++] = getByte(pos++);
  }
  memcpy(carry_, newCarry, newCarryLen);
  carryLen_ = newCarryLen;
}

}  // namespace textencoding
