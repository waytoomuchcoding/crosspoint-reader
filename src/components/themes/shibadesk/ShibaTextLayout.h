#pragma once

#include <Utf8.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace shibadesk {

inline bool hasCjkCodepoints(const char* text) {
  if (!text) return false;
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  while (*p != '\0') {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp >= 0x2E80) return true;
  }
  return false;
}

inline bool isKinsokuForbiddenStart(const uint32_t cp) {
  switch (cp) {
    case ',':
    case '.':
    case '!':
    case '?':
    case ':':
    case ';':
    case ')':
    case ']':
    case '}':
    case 0x00B7:  // ·
    case 0x2014:  // —
    case 0x2026:  // …
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0x3009:  // 〉
    case 0x300B:  // 》
    case 0x300D:  // 」
    case 0x300F:  // 』
    case 0x3011:  // 】
    case 0xFF01:  // ！
    case 0xFF09:  // ）
    case 0xFF0C:  // ，
    case 0xFF0E:  // ．
    case 0xFF1A:  // ：
    case 0xFF1B:  // ；
    case 0xFF1F:  // ？
      return true;
    default:
      return false;
  }
}

template <typename MeasureFn>
std::string truncateUtf8ToWidth(const char* text, const int maxWidthPx, MeasureFn&& measureWidth,
                                const char* ellipsis = "...") {
  if (!text || *text == '\0' || maxWidthPx <= 0) return {};
  if (measureWidth(text) <= maxWidthPx) return std::string(text);

  const int ellipsisW = ellipsis && *ellipsis ? measureWidth(ellipsis) : 0;
  if (ellipsisW >= maxWidthPx) return {};

  std::string out;
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  while (*p != '\0') {
    const auto* start = p;
    (void)utf8NextCodepoint(&p);
    const size_t nextLen = static_cast<size_t>(p - reinterpret_cast<const unsigned char*>(text));
    std::string candidate(text, nextLen);
    if (ellipsis && *ellipsis) candidate += ellipsis;
    if (measureWidth(candidate.c_str()) > maxWidthPx) {
      break;
    }
    out.assign(text, static_cast<size_t>(p - reinterpret_cast<const unsigned char*>(text)));
    (void)start;
  }
  if (!out.empty() && ellipsis && *ellipsis) {
    out += ellipsis;
  }
  return out;
}

template <typename MeasureFn, typename TruncateFn>
std::vector<std::string> wrapSmartLinesWith(const char* text, const int maxWidthPx, const int maxLines,
                                            MeasureFn&& measureWidth, TruncateFn&& truncateSingleLine) {
  std::vector<std::string> lines;
  if (!text || *text == '\0' || maxWidthPx <= 0 || maxLines <= 0) return lines;
  lines.reserve(static_cast<size_t>(maxLines));

  if (maxLines == 1 || measureWidth(text) <= maxWidthPx) {
    lines.push_back(truncateSingleLine(text, maxWidthPx));
    return lines;
  }

  struct SpanToken {
    uint16_t offset = 0;
    uint16_t len = 0;
    bool isSpace = false;
  };

  // Tokenize into zero-copy byte spans into `text` (collapsing whitespace runs into a single
  // space token and attaching kinsoku forbidden-start punctuation to the preceding token).
  std::vector<SpanToken> tokens;
  tokens.reserve(32);
  const auto* base = reinterpret_cast<const unsigned char*>(text);
  const auto* p = base;
  size_t latinStart = 0;
  size_t latinLen = 0;

  auto flushLatin = [&]() {
    if (latinLen > 0 && latinStart <= 0xFFFFu) {
      tokens.push_back(SpanToken{static_cast<uint16_t>(latinStart),
                                 static_cast<uint16_t>(std::min<size_t>(latinLen, 0xFFFFu)), false});
      latinLen = 0;
    }
  };

  while (*p != '\0') {
    const auto* cpStart = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    const size_t off = static_cast<size_t>(cpStart - base);
    const size_t glyphLen = static_cast<size_t>(p - cpStart);
    if (off > 0xFFF0u) break;

    if (cp == '\r' || cp == '\n' || cp == '\t' || cp == ' ') {
      flushLatin();
      if (!tokens.empty() && !tokens.back().isSpace) {
        tokens.push_back(SpanToken{0, 1, true});
      }
      continue;
    }

    if (utf8IsCjkBreakable(cp) || cp >= 0x2E80) {
      flushLatin();
      if (isKinsokuForbiddenStart(cp) && !tokens.empty() && !tokens.back().isSpace &&
          static_cast<size_t>(tokens.back().offset) + tokens.back().len == off) {
        tokens.back().len = static_cast<uint16_t>(tokens.back().len + glyphLen);
      } else {
        tokens.push_back(SpanToken{static_cast<uint16_t>(off), static_cast<uint16_t>(glyphLen), false});
      }
    } else if (isKinsokuForbiddenStart(cp) && latinLen == 0 && !tokens.empty() && !tokens.back().isSpace &&
               static_cast<size_t>(tokens.back().offset) + tokens.back().len == off) {
      tokens.back().len = static_cast<uint16_t>(tokens.back().len + glyphLen);
    } else {
      if (latinLen == 0) latinStart = off;
      latinLen += glyphLen;
    }
  }
  flushLatin();

  auto appendTok = [&](std::string& dst, const SpanToken& tok) {
    if (tok.isSpace) {
      dst.push_back(' ');
    } else {
      dst.append(text + tok.offset, tok.len);
    }
  };

  std::string cur;
  std::string candidate;
  for (size_t i = 0; i < tokens.size(); ++i) {
    const SpanToken& tok = tokens[i];
    if (cur.empty() && tok.isSpace) continue;

    if (static_cast<int>(lines.size()) == maxLines - 1) {
      std::string rem = cur;
      for (size_t j = i; j < tokens.size(); ++j) {
        if (rem.empty() && tokens[j].isSpace) continue;
        appendTok(rem, tokens[j]);
      }
      lines.push_back(truncateSingleLine(rem.c_str(), maxWidthPx));
      cur.clear();
      break;
    }

    candidate = cur;
    appendTok(candidate, tok);
    if (!cur.empty() && measureWidth(candidate.c_str()) > maxWidthPx) {
      while (!cur.empty() && cur.back() == ' ') cur.pop_back();
      lines.push_back(std::move(cur));
      cur.clear();
      if (!tok.isSpace) {
        appendTok(cur, tok);
        if (measureWidth(cur.c_str()) > maxWidthPx) {
          lines.push_back(truncateSingleLine(cur.c_str(), maxWidthPx));
          cur.clear();
        }
      }
    } else {
      cur = std::move(candidate);
    }
  }

  if (!cur.empty() && static_cast<int>(lines.size()) < maxLines) {
    while (!cur.empty() && cur.back() == ' ') cur.pop_back();
    if (!cur.empty()) {
      lines.push_back(truncateSingleLine(cur.c_str(), maxWidthPx));
    }
  }
  return lines;
}

}  // namespace shibadesk
