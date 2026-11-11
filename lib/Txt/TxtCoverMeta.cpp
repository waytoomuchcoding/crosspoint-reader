#include "TxtCoverMeta.h"

#include <HalStorage.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace txtmeta {
namespace {

char asciiLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool endsWithIgnoreAsciiCase(std::string_view s, std::string_view suffix) {
  if (s.size() < suffix.size()) return false;
  const size_t offset = s.size() - suffix.size();
  for (size_t i = 0; i < suffix.size(); ++i) {
    if (asciiLower(s[offset + i]) != asciiLower(suffix[i])) return false;
  }
  return true;
}

std::string_view trimAsciiSpaces(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
    s.remove_suffix(1);
  }
  return s;
}

int hexDigitVal(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

void appendUtf8Codepoint(std::string& out, uint32_t cp) {
  if (cp <= 0x7Fu) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FFu) {
    out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0xFFFFu) {
    out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0x10FFFFu) {
    out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  }
}

void skipJsonWs(std::string_view src, size_t& pos) {
  while (pos < src.size()) {
    const char c = src[pos];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      ++pos;
    } else {
      break;
    }
  }
}

bool parseJsonHex4(std::string_view src, size_t& pos, uint16_t& out) {
  if (pos + 4 > src.size()) return false;
  uint16_t val = 0;
  for (int i = 0; i < 4; ++i) {
    const int d = hexDigitVal(src[pos + i]);
    if (d < 0) return false;
    val = static_cast<uint16_t>((val << 4) | d);
  }
  pos += 4;
  out = val;
  return true;
}

bool parseJsonString(std::string_view src, size_t& pos, std::string& out) {
  out.clear();
  if (pos >= src.size() || src[pos] != '"') return false;
  ++pos;
  while (pos < src.size()) {
    const char c = src[pos++];
    if (c == '"') return true;
    if (static_cast<uint8_t>(c) < 0x20u) return false;
    if (c != '\\') {
      out.push_back(c);
      continue;
    }
    if (pos >= src.size()) return false;
    const char esc = src[pos++];
    switch (esc) {
      case '"':
      case '\\':
      case '/':
        out.push_back(esc);
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'u': {
        uint16_t u1 = 0;
        if (!parseJsonHex4(src, pos, u1)) return false;
        uint32_t cp = u1;
        if (u1 >= 0xD800u && u1 <= 0xDBFFu && pos + 2 <= src.size() && src[pos] == '\\' && src[pos + 1] == 'u') {
          size_t saved = pos + 2;
          uint16_t u2 = 0;
          if (parseJsonHex4(src, saved, u2) && u2 >= 0xDC00u && u2 <= 0xDFFFu) {
            pos = saved;
            cp = 0x10000u + ((static_cast<uint32_t>(u1 - 0xD800u) << 10) | static_cast<uint32_t>(u2 - 0xDC00u));
          }
        }
        appendUtf8Codepoint(out, cp);
        break;
      }
      default:
        return false;
    }
  }
  return false;
}

bool skipJsonValue(std::string_view src, size_t& pos) {
  skipJsonWs(src, pos);
  if (pos >= src.size()) return false;
  const char c = src[pos];
  if (c == '"') {
    std::string dummy;
    return parseJsonString(src, pos, dummy);
  }
  if (c == '{' || c == '[') {
    const char open = c;
    const char close = (c == '{') ? '}' : ']';
    int depth = 1;
    ++pos;
    while (pos < src.size() && depth > 0) {
      if (src[pos] == '"') {
        std::string dummy;
        if (!parseJsonString(src, pos, dummy)) return false;
        continue;
      }
      if (src[pos] == open) ++depth;
      if (src[pos] == close) --depth;
      ++pos;
    }
    return depth == 0;
  }
  while (pos < src.size()) {
    const char ch = src[pos];
    if (ch == ',' || ch == '}' || ch == ']' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') break;
    ++pos;
  }
  return true;
}

void appendJsonEscaped(std::string& out, std::string_view s) {
  out.push_back('"');
  for (const char raw : s) {
    const auto uc = static_cast<uint8_t>(raw);
    switch (raw) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (uc < 0x20u) {
          char esc[7];
          std::snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned>(uc));
          out.append(esc);
        } else {
          out.push_back(raw);
        }
        break;
    }
  }
  out.push_back('"');
}

}  // namespace

bool isTxtOrMdPath(std::string_view path) {
  return endsWithIgnoreAsciiCase(path, ".txt") || endsWithIgnoreAsciiCase(path, ".md");
}

std::string canonicalizeBookPath(std::string_view bookPath) {
  std::vector<std::string_view> parts;
  parts.reserve(8);
  size_t start = 0;
  for (size_t i = 0; i <= bookPath.size(); ++i) {
    if (i == bookPath.size() || bookPath[i] == '/') {
      if (i > start) {
        const std::string_view seg = bookPath.substr(start, i - start);
        if (seg == "..") {
          if (!parts.empty()) parts.pop_back();
        } else if (seg != ".") {
          parts.push_back(seg);
        }
      }
      start = i + 1;
    }
  }
  if (parts.empty()) return "/";
  size_t totalLen = 0;
  for (const auto& p : parts) totalLen += 1 + p.size();
  std::string out;
  out.reserve(totalLen);
  for (const auto& p : parts) {
    out.push_back('/');
    out.append(p.data(), p.size());
  }
  return out;
}

std::string computePathHashHex(std::string_view bookPath) {
  const std::string canon = canonicalizeBookPath(bookPath);
  uint64_t hash = 14695981039346656037ULL;
  for (const char c : canon) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ULL;
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string hex(16, '0');
  for (int i = 15; i >= 0; --i) {
    hex[i] = kHex[hash & 0xFu];
    hash >>= 4;
  }
  return hex;
}

std::string getBoundCoverBmpPath(std::string_view bookPath) {
  std::string path(kHiddenCoversDir);
  path.push_back('/');
  path.append(computePathHashHex(bookPath));
  path.append(".bmp");
  return path;
}

std::string getBoundMetaJsonPath(std::string_view bookPath) {
  std::string path(kHiddenCoversDir);
  path.push_back('/');
  path.append(computePathHashHex(bookPath));
  path.append(".meta.json");
  return path;
}

bool readMetaJson(const std::string& metaPath, BoundBookMeta& out, bool* ioFailedOut) {
  out = {};
  if (ioFailedOut) *ioFailedOut = false;
  if (!Storage.exists(metaPath.c_str())) return false;

  HalFile f;
  if (!Storage.openFileForRead("TXTMETA", metaPath, f)) {
    if (ioFailedOut) *ioFailedOut = true;
    return false;
  }
  const size_t fileSize = f.size();
  if (fileSize == 0 || fileSize > 2048) {
    f.close();
    return false;
  }

  std::string buf(fileSize, '\0');
  size_t totalRead = 0;
  while (totalRead < fileSize) {
    const int n = f.read(&buf[totalRead], fileSize - totalRead);
    if (n < 0) {
      f.close();
      if (ioFailedOut) *ioFailedOut = true;
      return false;
    }
    if (n == 0) break;
    totalRead += static_cast<size_t>(n);
  }
  f.close();
  buf.resize(totalRead);

  const std::string_view src(buf);
  size_t pos = 0;
  skipJsonWs(src, pos);
  if (pos >= src.size() || src[pos] != '{') return false;
  ++pos;

  while (pos < src.size()) {
    skipJsonWs(src, pos);
    if (pos < src.size() && src[pos] == '}') {
      ++pos;
      out.title = utf8ComposeNfc(out.title);
      out.author = utf8ComposeNfc(out.author);
      return true;
    }
    std::string key;
    if (!parseJsonString(src, pos, key)) return false;
    skipJsonWs(src, pos);
    if (pos >= src.size() || src[pos] != ':') return false;
    ++pos;
    skipJsonWs(src, pos);
    if (key == "title") {
      if (!parseJsonString(src, pos, out.title)) return false;
    } else if (key == "author") {
      if (!parseJsonString(src, pos, out.author)) return false;
    } else if (key == "updatedAt") {
      uint64_t ts = 0;
      size_t digits = 0;
      while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
        ts = ts * 10u + static_cast<uint64_t>(src[pos] - '0');
        ++pos;
        ++digits;
      }
      if (digits == 0) return false;
      out.updatedAt = static_cast<uint32_t>(ts);
    } else {
      if (!skipJsonValue(src, pos)) return false;
    }
    skipJsonWs(src, pos);
    if (pos < src.size() && src[pos] == ',') {
      ++pos;
      continue;
    }
    if (pos < src.size() && src[pos] == '}') {
      ++pos;
      out.title = utf8ComposeNfc(out.title);
      out.author = utf8ComposeNfc(out.author);
      return true;
    }
    return false;
  }
  return false;
}

bool writeMetaJson(const std::string& metaPath, const BoundBookMeta& meta) {
  if (!Storage.exists("/.crosspoint")) {
    Storage.mkdir("/.crosspoint");
  }
  if (!Storage.exists(kHiddenCoversDir)) {
    Storage.mkdir(kHiddenCoversDir);
  }

  std::string json;
  json.reserve(64 + meta.title.size() + meta.author.size());
  json.append("{\"title\":");
  appendJsonEscaped(json, meta.title);
  json.append(",\"author\":");
  appendJsonEscaped(json, meta.author);
  char tsBuf[32];
  std::snprintf(tsBuf, sizeof(tsBuf), ",\"updatedAt\":%lu}", static_cast<unsigned long>(meta.updatedAt));
  json.append(tsBuf);

  const std::string tmpPath = metaPath + ".tmp";
  HalFile f;
  if (!Storage.openFileForWrite("TXTMETA", tmpPath, f)) {
    return false;
  }
  const bool written = (f.write(reinterpret_cast<const uint8_t*>(json.data()), json.size()) == json.size());
  const bool closed = f.close();
  if (!written || !closed) {
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (Storage.exists(metaPath.c_str()) && !Storage.remove(metaPath.c_str())) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(tmpPath.c_str(), metaPath.c_str())) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return true;
}

bool parseTitleAuthorFromStem(std::string_view stem, std::string& titleOut, std::string& authorOut) {
  titleOut.clear();
  authorOut.clear();
  stem = trimAsciiSpaces(stem);

  // Support CJK book-title brackets: "《Title》Author" or "《Title》 - Author"
  static constexpr std::string_view kOpenBracket = "\xE3\x80\x8A";   // 《
  static constexpr std::string_view kCloseBracket = "\xE3\x80\x8B";  // 》
  if (stem.starts_with(kOpenBracket)) {
    const size_t closePos = stem.find(kCloseBracket, kOpenBracket.size());
    if (closePos != std::string_view::npos) {
      const std::string_view inner = trimAsciiSpaces(stem.substr(kOpenBracket.size(), closePos - kOpenBracket.size()));
      std::string_view rest = trimAsciiSpaces(stem.substr(closePos + kCloseBracket.size()));
      if (rest.starts_with('-') || rest.starts_with(':')) {
        rest = trimAsciiSpaces(rest.substr(1));
      } else if (rest.starts_with("\xE2\x80\x94") || rest.starts_with("\xE2\x80\x93") ||
                 rest.starts_with("\xEF\xBC\x9A")) {
        rest = trimAsciiSpaces(rest.substr(3));
      }
      if (!inner.empty() && !rest.empty()) {
        titleOut = utf8ComposeNfc(std::string(inner));
        authorOut = utf8ComposeNfc(std::string(rest));
        return !titleOut.empty() && !authorOut.empty();
      }
    }
  }

  static constexpr std::string_view kSeparators[] = {" - ", " \xE2\x80\x94 ", " \xE2\x80\x93 "};
  size_t bestPos = std::string_view::npos;
  size_t bestSepLen = 0;
  for (const auto sep : kSeparators) {
    const size_t pos = stem.rfind(sep);
    if (pos != std::string_view::npos && (bestPos == std::string_view::npos || pos > bestPos)) {
      bestPos = pos;
      bestSepLen = sep.size();
    }
  }
  if (bestPos == std::string_view::npos) return false;

  const std::string_view left = trimAsciiSpaces(stem.substr(0, bestPos));
  const std::string_view right = trimAsciiSpaces(stem.substr(bestPos + bestSepLen));
  if (left.empty() || right.empty()) return false;

  titleOut = utf8ComposeNfc(std::string(left));
  authorOut = utf8ComposeNfc(std::string(right));
  return !titleOut.empty() && !authorOut.empty();
}

}  // namespace txtmeta
