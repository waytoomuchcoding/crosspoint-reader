#include "SleepVocabScreen.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <PageOrnament.h>
#include <SdCardFont.h>
#include <Utf8.h>
#include <ZhDict.h>
#include <esp_random.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "SdCardFontSystem.h"
#include "fontIds.h"
#include "vocabulary/VocabularyStore.h"

namespace sleep_vocab {
namespace {

constexpr char HEADWORD_FONT_PATH[] = "/zh/headword.cpfont";
constexpr int HEADWORD_FONT_ID = 0x5A48574F;  // Same reserved slot used by ZhLookupActivity
constexpr size_t ENTRY_BUF_BYTES = 3072;
constexpr size_t MAX_HISTORY_SCAN = 160;

struct ScopedHeadwordFont {
  GfxRenderer& renderer;
  std::unique_ptr<SdCardFont> font;
  int id = 0;

  explicit ScopedHeadwordFont(GfxRenderer& r) : renderer(r) {
    if (!Storage.exists(HEADWORD_FONT_PATH)) return;
    auto f = makeUniqueNoThrow<SdCardFont>();
    if (!f || !f->load(HEADWORD_FONT_PATH)) return;
    if (renderer.getFontMap().count(HEADWORD_FONT_ID)) renderer.removeFont(HEADWORD_FONT_ID);
    renderer.registerSdCardFont(HEADWORD_FONT_ID, f.get());
    renderer.insertFont(HEADWORD_FONT_ID,
                        EpdFontFamily(0, f->getEpdFont(0), f->getEpdFont(1), f->getEpdFont(2), f->getEpdFont(3)));
    font = std::move(f);
    id = HEADWORD_FONT_ID;
  }

  ~ScopedHeadwordFont() {
    if (id != 0) renderer.removeFont(id);
  }
};

struct RawVocabItem {
  std::string word;
  std::string headword;
  std::string rawDef;
  std::string bookTitle;
  uint32_t lastTimestamp = 0;
  uint16_t lookupCount = 1;
};

struct SecondaryPron {
  std::string pinyin;
  uint32_t zhuyin[zh::MAX_WORD_CHARS] = {};
  uint8_t syllables = 0;
  std::string sense;
  std::string fitSense;
};

struct PreparedCard {
  std::string word;
  std::vector<uint32_t> cps;
  std::string trad;
  std::string badge;        // e.g. "HSK 6 · #5.4k" or "#9.2k"
  std::string pinyinLine;   // Primary reading pinyin
  uint32_t zhuyin[zh::MAX_WORD_CHARS] = {};
  uint8_t syllables = 0;
  std::vector<std::string> primaryLines;    // 1-2 wrapped lines of primary reading senses
  std::vector<SecondaryPron> otherProns;    // 1 compact line per secondary pronunciation
  std::string sourceBook;                   // e.g. "《唐宋诗词选》"
  bool isChinese = false;
  uint32_t rank = zh::NO_RANK;
  uint8_t hsk = 0;
  float weight = 1.0f;

  // Measured geometry
  int x = 0;
  int y = 0;
  int w = 324;
  int h = 160;
  int titleFont = 0;
  int titleHeight = 36;
};

// Simple deterministic PRNG for layout & weighted sampling.
struct Rng32 {
  uint32_t s;
  explicit Rng32(uint32_t seed) : s(seed ? seed : 0x6D2B79F5u) {}
  uint32_t next() {
    uint32_t z = (s += 0x9E3779B9u);
    z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z = (z ^ (z >> 13)) * 0xC2B2AE35u;
    return z ^ (z >> 16);
  }
  float unit() { return std::max(1e-6f, (next() & 0xFFFFFFu) / 16777216.0f); }
  int rangeInt(int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + static_cast<int>(next() % static_cast<uint32_t>(hi - lo + 1));
  }
};

bool startsWithIgnoreCase(const std::string& s, const char* prefix) {
  const size_t n = std::strlen(prefix);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; ++i) {
    if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

// Strip noisy dictionary metadata senses ("CL:...", "surname ...", "old variant of ...")
// so sleep flashcards stay concise and meaningful.
bool isBoilerplateSense(const std::string& sense, bool isCapitalizedPinyin) {
  if (startsWithIgnoreCase(sense, "CL:")) return true;
  if (startsWithIgnoreCase(sense, "also pr.")) return true;
  if (startsWithIgnoreCase(sense, "Taiwan pr.")) return true;
  if (startsWithIgnoreCase(sense, "see also ")) return true;
  if (startsWithIgnoreCase(sense, "used in ")) return true;
  if (isCapitalizedPinyin && startsWithIgnoreCase(sense, "surname ")) return true;
  if (startsWithIgnoreCase(sense, "old variant of ")) return true;
  if (startsWithIgnoreCase(sense, "variant of ") && sense.size() < 32) return true;
  return false;
}

void splitSenses(const char* senses, size_t len, bool isCapPinyin, std::vector<std::string>& outClean,
                 std::vector<std::string>& outFallback) {
  for (size_t s = 0; s < len;) {
    size_t e = s;
    while (e < len && senses[e] != '\x1f') ++e;
    std::string item(senses + s, e - s);
    // Trim whitespace
    while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front()))) item.erase(item.begin());
    while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back()))) item.pop_back();
    if (!item.empty()) {
      outFallback.push_back(item);
      if (!isBoilerplateSense(item, isCapPinyin)) {
        outClean.push_back(std::move(item));
      }
    }
    s = e + 1;
  }
}

void ensureGlyphsReady(GfxRenderer& renderer, int fontId, const std::string& text) {
  if (fontId != 0 && renderer.isSdCardFont(fontId) && !text.empty()) {
    renderer.ensureSdCardFontReady(fontId, text.c_str(), 1);
  }
}

std::string truncateToPixelWidth(GfxRenderer& renderer, int fontId, const std::string& text, int maxWidth) {
  ensureGlyphsReady(renderer, fontId, text);
  if (renderer.getTextAdvanceX(fontId, text.c_str(), EpdFontFamily::REGULAR, 0) <= maxWidth) {
    return text;
  }
  const std::string ellipsis = "...";
  const int ellW = renderer.getTextAdvanceX(fontId, ellipsis.c_str(), EpdFontFamily::REGULAR, 0);
  std::string out;
  int curW = 0;
  size_t lastBreak = std::string::npos;
  const auto* p = reinterpret_cast<const unsigned char*>(text.c_str());
  while (*p) {
    const unsigned char* before = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    char one[5] = {};
    std::memcpy(one, before, p - before);
    const int w = renderer.getTextAdvanceX(fontId, one, EpdFontFamily::REGULAR, 0);
    if (curW + w + ellW > maxWidth) break;
    if (cp == ' ' || cp == ';') lastBreak = out.size();
    out.append(reinterpret_cast<const char*>(before), p - before);
    curW += w;
  }
  if (lastBreak != std::string::npos && lastBreak > out.size() * 2 / 3) {
    out.resize(lastBreak);
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == ';' || out.back() == ',')) out.pop_back();
  return out + ellipsis;
}

void wrapLinesBounded(GfxRenderer& renderer, int fontId, const std::string& text, int maxWidth, size_t maxLines,
                      std::vector<std::string>& out) {
  ensureGlyphsReady(renderer, fontId, text);
  std::string cur;
  int curW = 0;
  size_t lastSpace = std::string::npos;
  const auto* p = reinterpret_cast<const unsigned char*>(text.c_str());
  while (*p) {
    const unsigned char* before = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    if (cp == '\n') {
      if (!cur.empty()) {
        out.push_back(std::move(cur));
        cur.clear();
        curW = 0;
        lastSpace = std::string::npos;
        if (out.size() >= maxLines) return;
      }
      continue;
    }
    char one[5] = {};
    std::memcpy(one, before, p - before);
    const int w = renderer.getTextAdvanceX(fontId, one, EpdFontFamily::REGULAR, 0);
    if (curW + w > maxWidth && !cur.empty()) {
      if (out.size() + 1 >= maxLines) {
        // Last allowed line: append remaining and truncate cleanly with ellipsis!
        std::string rest = cur;
        rest.append(reinterpret_cast<const char*>(before));
        out.push_back(truncateToPixelWidth(renderer, fontId, rest, maxWidth));
        return;
      }
      std::string carry;
      if (lastSpace != std::string::npos && lastSpace > 4) {
        carry = cur.substr(lastSpace + 1);
        cur.resize(lastSpace);
      }
      out.push_back(std::move(cur));
      cur = carry;
      curW = carry.empty() ? 0 : renderer.getTextAdvanceX(fontId, carry.c_str(), EpdFontFamily::REGULAR, 0);
      lastSpace = std::string::npos;
    }
    if (cp == ' ' && curW > 0) lastSpace = cur.size();
    cur.append(reinterpret_cast<const char*>(before), p - before);
    curW += w;
  }
  if (!cur.empty() && out.size() < maxLines) {
    out.push_back(std::move(cur));
  }
}

bool readRecentVocabulary(std::vector<RawVocabItem>& itemsOut, uint32_t& maxTsOut) {
  HalFile file;
  if (!Storage.openFileForRead("SLP", VocabularyStore::FILE_PATH, file)) return false;

  const size_t fSize = file.fileSize();
  if (fSize == 0) return false;
  // Seek near the tail if file is huge so RAM and latency stay tiny on ESP32-C3.
  constexpr size_t MAX_TAIL_BYTES = 48 * 1024;
  if (fSize > MAX_TAIL_BYTES) {
    file.seek(fSize - MAX_TAIL_BYTES);
    // Discard partial first line
    int c = 0;
    while ((c = file.read()) >= 0 && c != '\n') {
    }
  }

  std::string line;
  line.reserve(512);
  uint32_t seqFallback = 1000;
  maxTsOut = 0;

  while (file.available()) {
    line.clear();
    int c = 0;
    while ((c = file.read()) >= 0 && c != '\n') {
      if (c != '\r' && line.size() < 3072) line.push_back(static_cast<char>(c));
    }
    if (line.size() < 8 || line.front() != '{') continue;

    JsonDocument doc;
    if (deserializeJson(doc, line)) continue;
    const char* word = doc["word"] | "";
    if (word[0] == '\0') continue;
    const char* head = doc["headword"] | word;
    const char* def = doc["definition"] | "";
    const char* title = doc["title"] | "";
    uint32_t ts = doc["timestamp"] | 0u;
    if (ts == 0) ts = ++seqFallback * 3600u;
    maxTsOut = std::max(maxTsOut, ts);

    auto it = std::find_if(itemsOut.begin(), itemsOut.end(), [&](const RawVocabItem& x) { return x.word == word; });
    if (it != itemsOut.end()) {
      it->lookupCount++;
      if (ts >= it->lastTimestamp) {
        it->lastTimestamp = ts;
        if (title[0] != '\0') it->bookTitle = title;
        if (def[0] != '\0') it->rawDef = def;
      }
    } else {
      if (itemsOut.size() >= MAX_HISTORY_SCAN) itemsOut.erase(itemsOut.begin());
      itemsOut.push_back({word, head, def, title, ts, 1});
    }
  }
  return !itemsOut.empty();
}

std::string formatRankBadge(uint8_t hsk, uint32_t rank) {
  char buf[32];
  if (rank != zh::NO_RANK && rank > 0) {
    if (rank >= 1000) {
      if (hsk > 0) {
        std::snprintf(buf, sizeof(buf), "HSK%u · #%.1fk", static_cast<unsigned>(hsk), rank / 1000.0);
      } else {
        std::snprintf(buf, sizeof(buf), "#%.1fk", rank / 1000.0);
      }
    } else {
      if (hsk > 0) {
        std::snprintf(buf, sizeof(buf), "HSK%u · #%lu", static_cast<unsigned>(hsk), static_cast<unsigned long>(rank));
      } else {
        std::snprintf(buf, sizeof(buf), "#%lu", static_cast<unsigned long>(rank));
      }
    }
    return buf;
  }
  if (hsk > 0) {
    std::snprintf(buf, sizeof(buf), "HSK %u", static_cast<unsigned>(hsk));
    return buf;
  }
  return "";
}

// Draw an artisanal woodblock/scholar's slip frame:
// pure white knockout interior + 5px chamfered classical corners + 2px outer ink border
// + 1px inner hairline inset + 3px offset woodcut ink drop-shadow on bottom & right.
void drawScholarPlaqueFrame(GfxRenderer& renderer, int x, int y, int w, int h) {
  constexpr int kChamfer = 5;
  constexpr int kShadow = 3;

  // 1. Offset woodcut ink drop-shadow (bottom-right)
  for (int sy = y + kShadow; sy < y + h + kShadow; ++sy) {
    for (int sx = x + kShadow; sx < x + w + kShadow; ++sx) {
      const int lx = sx - (x + kShadow);
      const int ly = sy - (y + kShadow);
      const int cornerDist = std::min(lx, w - 1 - lx) + std::min(ly, h - 1 - ly);
      if (cornerDist >= kChamfer) {
        renderer.drawPixel(sx, sy, true);
      }
    }
  }

  // 2. White paper card interior + 2px chamfered ink border
  for (int py = y; py < y + h; ++py) {
    const int ly = py - y;
    for (int px = x; px < x + w; ++px) {
      const int lx = px - x;
      const int dLeft = lx;
      const int dRight = w - 1 - lx;
      const int dTop = ly;
      const int dBot = h - 1 - ly;
      const int minEdge = std::min({dLeft, dRight, dTop, dBot});
      const int cornerSum = std::min(dLeft, dRight) + std::min(dTop, dBot);
      if (cornerSum < kChamfer) continue;  // Outside chamfered corner
      const bool onOuterBorder = (minEdge <= 1) || (cornerSum <= kChamfer + 1);
      renderer.drawPixel(px, py, onOuterBorder);
    }
  }

  // 3. Delicate inner corner L-brackets (traditional East Asian stationery accent)
  constexpr int kIn = 5;
  constexpr int kArm = 11;
  // Top-left
  renderer.drawLine(x + kIn + 2, y + kIn, x + kIn + kArm, y + kIn, true);
  renderer.drawLine(x + kIn, y + kIn + 2, x + kIn, y + kIn + kArm, true);
  // Top-right
  renderer.drawLine(x + w - 1 - kIn - kArm, y + kIn, x + w - 1 - kIn - 2, y + kIn, true);
  renderer.drawLine(x + w - 1 - kIn, y + kIn + 2, x + w - 1 - kIn, y + kIn + kArm, true);
  // Bottom-left
  renderer.drawLine(x + kIn + 2, y + h - 1 - kIn, x + kIn + kArm, y + h - 1 - kIn, true);
  renderer.drawLine(x + kIn, y + h - 1 - kIn - kArm, x + kIn, y + h - 1 - kIn - 2, true);
  // Bottom-right
  renderer.drawLine(x + w - 1 - kIn - kArm, y + h - 1 - kIn, x + w - 1 - kIn - 2, y + h - 1 - kIn, true);
  renderer.drawLine(x + w - 1 - kIn, y + h - 1 - kIn - kArm, x + w - 1 - kIn, y + h - 1 - kIn - 2, true);
}

int chooseCharFont(GfxRenderer& renderer, int preferredFont, int fallbackFont, const char* ch) {
  if (preferredFont != 0) {
    if (renderer.isSdCardFont(preferredFont)) renderer.ensureSdCardFontReady(preferredFont, ch, 1);
    if (renderer.getTextAdvanceX(preferredFont, ch, EpdFontFamily::REGULAR, 0) > 0) return preferredFont;
  }
  if (fallbackFont != 0 && renderer.isSdCardFont(fallbackFont)) {
    renderer.ensureSdCardFontReady(fallbackFont, ch, 1);
  }
  return fallbackFont;
}

void measureAndPrepareCard(GfxRenderer& renderer, PreparedCard& card, int headFont, int readerFont, int bodyFont,
                           int captionFont, int cardWidth) {
  card.w = cardWidth;
  constexpr int kPadX = 13;
  constexpr int kPadTop = 9;
  constexpr int kPadBot = 8;
  const int innerW = card.w - 2 * kPadX;

  const int bodyLH = renderer.getLineHeight(bodyFont);
  const int capLH = renderer.getLineHeight(captionFont);

  // Pick headword font (48px Ming headFont when it fits alongside the badge, else readerFont).
  int badgeW = 0;
  if (!card.badge.empty()) {
    ensureGlyphsReady(renderer, captionFont, card.badge);
    badgeW = renderer.getTextAdvanceX(captionFont, card.badge.c_str(), EpdFontFamily::REGULAR, 0) + 14;
  }

  card.titleFont = readerFont;
  if (card.isChinese && !card.cps.empty()) {
    for (int cand : {headFont, readerFont, bodyFont}) {
      if (cand == 0) continue;
      ensureGlyphsReady(renderer, cand, zh::EM_PROBE);
      const int em = std::max(16, renderer.getTextAdvanceX(cand, zh::EM_PROBE, EpdFontFamily::REGULAR, 0));
      int totalW = 0;
      for (size_t k = 0; k < card.cps.size(); ++k) {
        char ch[5] = {};
        *utf8AppendCodepoint(ch, card.cps[k]) = '\0';
        const int cf = chooseCharFont(renderer, cand, readerFont, ch);
        totalW += renderer.getTextAdvanceX(cf, ch, EpdFontFamily::REGULAR, 0);
        if (k < card.syllables && card.zhuyin[k] != 0) totalW += zh::rubyWidth(em);
      }
      if (totalW + badgeW + 6 <= innerW) {
        card.titleFont = cand;
        break;
      }
    }
  }
  card.titleHeight = std::max(bodyLH + 4, renderer.getLineHeight(card.titleFont));

  // Wrap primary definition senses (at most 2 lines so card stays compact and airy).
  std::vector<std::string> wrappedPrimary;
  for (const auto& pLine : card.primaryLines) {
    if (wrappedPrimary.size() >= 2) break;
    wrapLinesBounded(renderer, bodyFont, pLine, innerW, 2 - wrappedPrimary.size(), wrappedPrimary);
  }
  card.primaryLines = std::move(wrappedPrimary);

  // Measure each secondary pronunciation so Pinyin + inline 5x7 Bopomofo + 1-line gloss fit crisply!
  if (card.otherProns.size() > 2) card.otherProns.resize(2);
  for (auto& sec : card.otherProns) {
    const std::string pyPart = "• " + sec.pinyin;
    ensureGlyphsReady(renderer, captionFont, pyPart);
    int usedW = renderer.getTextAdvanceX(captionFont, pyPart.c_str(), EpdFontFamily::REGULAR, 0);
    if (sec.syllables > 0 && sec.zhuyin[0] != 0) {
      usedW += 5;
      for (uint8_t s = 0; s < sec.syllables; ++s) {
        if (s > 0) usedW += 3;
        usedW += zh::rubyUnderWidth(sec.zhuyin[s], 14);
      }
    }
    const std::string sep = " — ";
    ensureGlyphsReady(renderer, captionFont, sep);
    const int sepW = renderer.getTextAdvanceX(captionFont, sep.c_str(), EpdFontFamily::REGULAR, 0);
    const int availSense = std::max(36, innerW - usedW - sepW);
    sec.fitSense = sep + truncateToPixelWidth(renderer, captionFont, sec.sense, availSense);
  }

  int h = kPadTop + card.titleHeight + 2;
  if (!card.pinyinLine.empty()) h += capLH + 2;
  h += static_cast<int>(card.primaryLines.size()) * bodyLH;
  if (!card.otherProns.empty()) {
    h += 6 + static_cast<int>(card.otherProns.size()) * (capLH + 1);
  }
  if (!card.sourceBook.empty()) {
    h += capLH + 2;
  }
  h += kPadBot;
  card.h = h;
}

void drawPreparedCard(GfxRenderer& renderer, const PreparedCard& card, int readerFont, int bodyFont, int captionFont) {
  drawScholarPlaqueFrame(renderer, card.x, card.y, card.w, card.h);

  constexpr int kPadX = 13;
  constexpr int kPadTop = 9;
  const int innerX = card.x + kPadX;
  const int innerRight = card.x + card.w - kPadX;
  const int innerW = innerRight - innerX;

  const int bodyLH = renderer.getLineHeight(bodyFont);
  const int capLH = renderer.getLineHeight(captionFont);

  int curY = card.y + kPadTop;

  // 1. Top-right inverted frequency / HSK pill badge
  int badgeLeft = innerRight;
  if (!card.badge.empty()) {
    ensureGlyphsReady(renderer, captionFont, card.badge);
    const int bw = renderer.getTextAdvanceX(captionFont, card.badge.c_str(), EpdFontFamily::REGULAR, 0) + 10;
    const int bh = capLH + 4;
    const int bx = innerRight - bw;
    const int by = curY + 2;
    // Chamfered 1-bit black pill
    renderer.fillRect(bx + 1, by, bw - 2, bh, true);
    renderer.fillRect(bx, by + 1, bw, bh - 2, true);
    renderer.drawText(captionFont, bx + 5, by + 2, card.badge.c_str(), false);
    badgeLeft = bx - 6;
  }

  // 2. Headword + vertical Zhuyin ruby + optional 【Trad】
  if (card.isChinese && !card.cps.empty()) {
    ensureGlyphsReady(renderer, card.titleFont, zh::EM_PROBE);
    const int em = std::max(16, renderer.getTextAdvanceX(card.titleFont, zh::EM_PROBE, EpdFontFamily::REGULAR, 0));
    const int titleLine = renderer.getLineHeight(card.titleFont);
    int tx = innerX;
    for (size_t k = 0; k < card.cps.size(); ++k) {
      char ch[5] = {};
      *utf8AppendCodepoint(ch, card.cps[k]) = '\0';
      const int cf = chooseCharFont(renderer, card.titleFont, readerFont, ch);
      const int chY = curY + renderer.getFontAscenderSize(card.titleFont) - renderer.getFontAscenderSize(cf);
      renderer.drawText(cf, tx, chY, ch, true);
      tx += renderer.getTextAdvanceX(cf, ch, EpdFontFamily::REGULAR, 0);
      if (k < card.syllables && card.zhuyin[k] != 0) {
        const int rx = tx;
        zh::forEachRubyPixel(card.zhuyin[k], em, titleLine,
                             [&](int px, int py) { renderer.drawPixel(rx + px, curY + py, true); });
        tx += zh::rubyWidth(em);
      }
    }
    if (!card.trad.empty()) {
      const std::string tradTag = "【" + card.trad + "】";
      ensureGlyphsReady(renderer, bodyFont, tradTag);
      const int tw = renderer.getTextAdvanceX(bodyFont, tradTag.c_str(), EpdFontFamily::REGULAR, 0);
      if (tx + tw + 2 <= badgeLeft) {
        const int ty = curY + card.titleHeight - bodyLH - 2;
        renderer.drawText(bodyFont, tx + 2, ty, tradTag.c_str(), true);
      }
    }
  } else {
    ensureGlyphsReady(renderer, card.titleFont, card.word);
    renderer.drawText(card.titleFont, innerX, curY, card.word.c_str(), true);
  }
  curY += card.titleHeight + 2;

  // 3. Primary reading Pinyin line
  if (!card.pinyinLine.empty()) {
    const std::string pyFit = truncateToPixelWidth(renderer, captionFont, card.pinyinLine, innerW);
    renderer.drawText(captionFont, innerX, curY, pyFit.c_str(), true);
    curY += capLH + 2;
  }

  // 4. Primary reading senses
  for (const auto& line : card.primaryLines) {
    ensureGlyphsReady(renderer, bodyFont, line);
    renderer.drawText(bodyFont, innerX, curY, line.c_str(), true);
    curY += bodyLH;
  }

  // 5. Secondary pronunciations (Pinyin + crisp 5x7 native Bopomofo bitmap + 1-line sense)
  if (!card.otherProns.empty()) {
    curY += 2;
    for (int dx = 0; dx < innerW; dx += 4) {
      renderer.drawPixel(innerX + dx, curY, true);
      renderer.drawPixel(innerX + dx + 1, curY, true);
    }
    curY += 4;
    for (const auto& sec : card.otherProns) {
      int lx = innerX;
      const std::string pyPart = "• " + sec.pinyin;
      ensureGlyphsReady(renderer, captionFont, pyPart);
      renderer.drawText(captionFont, lx, curY, pyPart.c_str(), true);
      lx += renderer.getTextAdvanceX(captionFont, pyPart.c_str(), EpdFontFamily::REGULAR, 0);

      if (sec.syllables > 0 && sec.zhuyin[0] != 0) {
        lx += 5;
        const int rubyBaseY = curY + std::max(0, (capLH - 7) / 2) - 3;
        for (uint8_t s = 0; s < sec.syllables; ++s) {
          if (s > 0) lx += 3;
          const int rw = zh::rubyUnderWidth(sec.zhuyin[s], 14);
          zh::forEachRubyUnderPixel(sec.zhuyin[s], 14, rw, 0,
                                    [&](int px, int py) { renderer.drawPixel(lx + px, rubyBaseY + py, true); });
          lx += rw;
        }
      }

      ensureGlyphsReady(renderer, captionFont, sec.fitSense);
      renderer.drawText(captionFont, lx, curY, sec.fitSense.c_str(), true);
      curY += capLH + 1;
    }
  }

  // 6. Subtle right-aligned book title attribution
  if (!card.sourceBook.empty()) {
    curY += 2;
    const std::string srcFit = truncateToPixelWidth(renderer, captionFont, card.sourceBook, innerW - 8);
    const int sw = renderer.getTextAdvanceX(captionFont, srcFit.c_str(), EpdFontFamily::REGULAR, 0);
    renderer.drawText(captionFont, std::max(innerX, innerRight - sw), curY, srcFit.c_str(), true);
  }
}

uint32_t nextSleepVocabSeed() {
  // Optional deterministic seed file on SD for reproducible simulator previews.
  HalFile sf;
  if (Storage.openFileForRead("SLP", "/.crosspoint/vocab_sleep_seed.txt", sf)) {
    char buf[32] = {};
    const int n = sf.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf) - 1);
    if (n > 0) {
      const unsigned long parsed = std::strtoul(buf, nullptr, 0);
      if (parsed != 0) return static_cast<uint32_t>(parsed);
    }
  }
  uint32_t seed = 0, state = 0;
  uint16_t left = 0, total = 0;
  APP_STATE.getSleepLCG(seed, state, left, total);
  const uint32_t next = (state * 1664525u + 1013904223u) ^ esp_random();
  APP_STATE.setSleepLCG(seed ? seed : 0x51EE9u, next, left, total);
  APP_STATE.saveToFile();
  return next;
}

}  // namespace

bool renderVocabSleepScreen(GfxRenderer& renderer) {
  sdFontSystem.ensureLoaded(renderer);

  std::vector<RawVocabItem> rawItems;
  uint32_t maxTs = 0;
  if (!readRecentVocabulary(rawItems, maxTs) || rawItems.empty()) {
    return false;
  }

  const bool zhDictReady = zh::openSharedDict();
  auto entryBuf = makeUniqueNoThrow<uint8_t[]>(ENTRY_BUF_BYTES);
  if (!entryBuf) {
    if (zhDictReady) zh::sharedDict().close();
    return false;
  }

  // User's selected frequency threshold from reader ruby settings (defaulting to top 3,000 if Off/All).
  uint32_t threshold = zh::rubyThresholdForStep(SETTINGS.zhRuby);
  if (threshold == 0 || threshold == zh::RUBY_ALL) {
    threshold = 3000;
  }
  const uint32_t easyCutoff = (threshold * 85u) / 100u;

  std::vector<PreparedCard> pool;
  pool.reserve(rawItems.size());

  for (const auto& item : rawItems) {
    PreparedCard card;
    card.word = item.headword.empty() ? item.word : item.headword;
    if (!item.bookTitle.empty()) {
      // Add guillemets for CJK book titles if not already present.
      const auto* bp = reinterpret_cast<const unsigned char*>(item.bookTitle.c_str());
      const uint32_t firstBookCp = utf8NextCodepoint(&bp);
      card.sourceBook = zh::isHan(firstBookCp) ? ("《" + item.bookTitle + "》") : ("— " + item.bookTitle);
    }

    const auto* p = reinterpret_cast<const unsigned char*>(card.word.c_str());
    bool allHan = true;
    while (const uint32_t cp = utf8NextCodepoint(&p)) {
      card.cps.push_back(cp);
      if (!zh::isHan(cp)) allHan = false;
    }
    card.isChinese = allHan && !card.cps.empty() && card.cps.size() <= zh::MAX_WORD_CHARS;

    bool enrichedFromZhDict = false;
    if (card.isChinese && zhDictReady) {
      zh::WordMatch matches[zh::MAX_MATCHES];
      const int mCount =
          zh::sharedDict().matchesAt(card.cps.data(), card.cps.size(), 0, matches, zh::MAX_MATCHES);
      const zh::WordMatch* exact = nullptr;
      for (int i = 0; i < mCount; ++i) {
        if (matches[i].len == card.cps.size()) {
          exact = &matches[i];
          break;
        }
      }
      if (exact) {
        card.rank = exact->rank;
        card.hsk = exact->hsk;

        // Hard filter: hide words on the easy side completely (unless looked up 2+ times!).
        if (card.rank != zh::NO_RANK && card.rank < easyCutoff && item.lookupCount < 2) {
          continue;
        }

        const size_t got = zh::sharedDict().readEntry(exact->entryOff, entryBuf.get(), ENTRY_BUF_BYTES);
        if (got > 0) {
          zh::EntryReader reader(entryBuf.get(), got);
          struct ParsedReading {
            std::string pinyin;
            std::string trad;
            uint32_t zhuyin[zh::MAX_WORD_CHARS] = {};
            uint8_t syllables = 0;
            std::vector<std::string> senses;
            bool surnameOnly = false;
          };
          std::vector<ParsedReading> lexical;
          std::vector<ParsedReading> fallbackSurname;

          zh::EntryReading er;
          while (reader.next(er)) {
            ParsedReading pr;
            pr.pinyin.assign(er.pinyin, er.pinyinLen);
            if (er.tradLen > 0 && card.word.compare(0, std::string::npos, er.trad, er.tradLen) != 0) {
              pr.trad.assign(er.trad, er.tradLen);
            }
            pr.syllables = er.syllables;
            std::copy(er.zhuyin, er.zhuyin + er.syllables, pr.zhuyin);

            const bool isCap = !pr.pinyin.empty() && std::isupper(static_cast<unsigned char>(pr.pinyin[0]));
            std::vector<std::string> cleanSenses;
            std::vector<std::string> rawSenses;
            splitSenses(er.senses, er.sensesLen, isCap, cleanSenses, rawSenses);

            if (!cleanSenses.empty()) {
              pr.senses = std::move(cleanSenses);
              lexical.push_back(std::move(pr));
            } else if (!rawSenses.empty()) {
              pr.senses = std::move(rawSenses);
              pr.surnameOnly = true;
              fallbackSurname.push_back(std::move(pr));
            }
          }

          const auto& chosenList = !lexical.empty() ? lexical : fallbackSurname;
          if (!chosenList.empty()) {
            const auto& prim = chosenList[0];
            card.pinyinLine = prim.pinyin;
            card.trad = prim.trad;
            card.syllables = prim.syllables;
            std::copy(prim.zhuyin, prim.zhuyin + prim.syllables, card.zhuyin);
            card.badge = formatRankBadge(card.hsk, card.rank);

            // Combine up to first 2 concise senses for the primary pronunciation when both fit cleanly.
            std::string gloss = "1. " + prim.senses[0];
            if (prim.senses.size() >= 2 && (gloss.size() + prim.senses[1].size()) <= 44) {
              gloss += "  2. " + prim.senses[1];
            }
            card.primaryLines.push_back(std::move(gloss));

            // One compact line from each secondary pronunciation (deduplicating identical pinyin)!
            for (size_t r = 1; r < chosenList.size() && card.otherProns.size() < 2; ++r) {
              const auto& sec = chosenList[r];
              if (sec.pinyin == prim.pinyin) continue;
              SecondaryPron sp;
              sp.pinyin = sec.pinyin;
              sp.syllables = sec.syllables;
              std::copy(sec.zhuyin, sec.zhuyin + sec.syllables, sp.zhuyin);
              sp.sense = sec.senses[0];
              card.otherProns.push_back(std::move(sp));
            }
            enrichedFromZhDict = true;
          }
        }
      }
    }

    if (!enrichedFromZhDict) {
      if (item.rawDef.empty()) continue;
      card.primaryLines.push_back(item.rawDef);
    }

    // Compute sampling weight = DifficultySweetSpot(rank, threshold) * RecencyDecay * RepeatBoost!
    float wDiff = 0.72f;
    if (card.isChinese) {
      const float rEff =
          (card.rank == zh::NO_RANK) ? (static_cast<float>(threshold) * 3.8f) : static_cast<float>(card.rank);
      const float logRatio = std::log(std::max(1.0f, rEff) / (static_cast<float>(threshold) * 1.65f)) / 0.90f;
      wDiff = 0.14f + 0.86f * std::exp(-0.5f * logRatio * logRatio);
    }
    const float ageDays =
        (maxTs > item.lastTimestamp) ? static_cast<float>(maxTs - item.lastTimestamp) / 86400.0f : 0.0f;
    const float wRecency = 0.18f + 0.82f * std::exp(-ageDays / 9.0f);
    const float wRepeats = 1.0f + 0.45f * static_cast<float>(std::min<int>(item.lookupCount - 1, 3));
    card.weight = std::max(0.02f, wDiff * wRecency * wRepeats);

    pool.push_back(std::move(card));
  }

  if (zhDictReady) {
    zh::sharedDict().close();
  }

  if (pool.empty()) return false;

  const uint32_t sleepSeed = nextSleepVocabSeed();
  Rng32 rng(sleepSeed);

  // Efraimidis-Spirakis weighted random sampling without replacement: key_i = U_i ^ (1 / w_i).
  struct KeyedIdx {
    float key;
    size_t idx;
  };
  std::vector<KeyedIdx> keyed;
  keyed.reserve(pool.size());
  for (size_t i = 0; i < pool.size(); ++i) {
    const float u = rng.unit();
    const float key = std::pow(u, 1.0f / pool[i].weight);
    keyed.push_back({key, i});
  }
  std::sort(keyed.begin(), keyed.end(), [](const KeyedIdx& a, const KeyedIdx& b) { return a.key > b.key; });

  ScopedHeadwordFont headFont(renderer);
  const int readerFont = SETTINGS.getReaderFontId();
  const int bodyFont = UI_12_FONT_ID;
  const int captionFont = UI_10_FONT_ID;

  const int W = renderer.getScreenWidth();
  const int H = renderer.getScreenHeight();

  // Decide between 3-Card Staggered S-Cascade (when >=3 words & height permits) vs 2-Card Sweeping Diagonal Courtyard!
  const bool preferTwoCardCourtyard = (pool.size() == 2) || ((sleepSeed & 3u) == 3u);
  const size_t wantCards = std::min<size_t>(pool.size(), preferTwoCardCourtyard ? 2 : 3);

  std::vector<PreparedCard> selected;
  selected.reserve(wantCards);
  for (size_t i = 0; i < wantCards; ++i) {
    selected.push_back(std::move(pool[keyed[i].idx]));
  }

  const int baseCardW = std::clamp(static_cast<int>(W * 0.62f), 280, 344);
  for (size_t i = 0; i < selected.size(); ++i) {
    const int cw = baseCardW + rng.rangeInt(-8, 10);
    measureAndPrepareCard(renderer, selected[i], headFont.id, readerFont, bodyFont, captionFont, cw);
  }

  // If 3 cards are too tall to preserve >= 32px ornamental channels between them, gracefully drop to 2 cards!
  if (selected.size() == 3) {
    const int sumH = selected[0].h + selected[1].h + selected[2].h;
    if (sumH + 2 * 32 + 32 > H) {
      selected.pop_back();
    }
  }

  // Artsy staggered placement leaving wide side bays & diagonal S-channels for PageOrnament!
  const bool startLeft = ((sleepSeed >> 2) & 1u) == 0u;
  if (selected.size() == 1) {
    selected[0].x = startLeft ? 24 : (W - selected[0].w - 26);
    selected[0].y = std::max(28, (H - selected[0].h) / 3);
  } else if (selected.size() == 2) {
    // Sweeping Diagonal Courtyard: one card anchored upper-corner, the other lower-opposite corner,
    // opening a lush central diagonal garden/ink expanse between them!
    const bool leftFirst = startLeft;
    selected[0].x = leftFirst ? rng.rangeInt(18, 30) : (W - selected[0].w - rng.rangeInt(20, 32));
    selected[0].y = rng.rangeInt(26, 44);

    selected[1].x = leftFirst ? (W - selected[1].w - rng.rangeInt(20, 32)) : rng.rangeInt(18, 30);
    selected[1].y = H - selected[1].h - rng.rangeInt(32, 52);
    if (selected[1].y < selected[0].y + selected[0].h + 56) {
      selected[1].y = std::min(H - selected[1].h - 16, selected[0].y + selected[0].h + 56);
    }
  } else {
    // 3-Card Asymmetric S-Cascade (Left -> Right -> Left, or Right -> Left -> Right)!
    const int sumH = selected[0].h + selected[1].h + selected[2].h;
    const int freeV = std::max(60, H - sumH - 44);
    const int gap01 = freeV / 2;
    const int gap12 = freeV - gap01;

    selected[0].y = 20;
    selected[1].y = selected[0].y + selected[0].h + gap01;
    selected[2].y = selected[1].y + selected[1].h + gap12;

    for (size_t i = 0; i < 3; ++i) {
      const bool pinLeft = ((i % 2 == 0) == startLeft);
      selected[i].x = pinLeft ? rng.rangeInt(16, 28) : (W - selected[i].w - rng.rangeInt(18, 30));
    }
  }

  renderer.clearScreen();

  // Draw all vocabulary plaques first on crisp white paper.
  std::vector<page_ornament::KeepoutRect> keepouts;
  keepouts.reserve(selected.size());
  for (const auto& card : selected) {
    drawPreparedCard(renderer, card, readerFont, bodyFont, captionFont);
    keepouts.push_back({static_cast<int16_t>(card.x - 2), static_cast<int16_t>(card.y - 2),
                        static_cast<int16_t>(card.x + card.w + 5), static_cast<int16_t>(card.y + card.h + 5)});
  }

  // Fill all gaps, channels & alcoves between the cards with 1-bit procedural ornaments!
  auto theme = static_cast<page_ornament::Theme>(SETTINGS.deadSpaceOrnamentTheme);
  if (theme == page_ornament::Theme::Off) {
    theme = page_ornament::Theme::AutoCycle;
  }
  page_ornament::renderCanvasOrnaments(renderer, theme, sleepSeed, keepouts.data(), keepouts.size(), 6, 9.0f);

  return true;
}

}  // namespace sleep_vocab
