#include "ZhLookupActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr size_t ENTRY_BUF_BYTES = 3072;
constexpr size_t CONTEXT_CHARS = zh::MAX_WORD_CHARS - 1;
constexpr int POPUP_MARGIN = 8;
constexpr int POPUP_PAD = 8;
constexpr int POPUP_BORDER = 4;
// DESIGN 4.2: hold 450 ms; repeat after 280 ms, every 90 ms, every 45 ms after 1.2 s.
constexpr unsigned long HOLD_MS = 450;
constexpr unsigned long REPEAT_FIRST_MS = 280;
constexpr unsigned long REPEAT_MS = 90;
constexpr unsigned long REPEAT_FAST_AFTER_MS = 1200;
constexpr unsigned long REPEAT_FAST_MS = 45;
constexpr unsigned long PREVIEW_DWELL_MS = 250;  // DESIGN 4.3

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle

void ZhLookupActivity::onEnter() {
  Activity::onEnter();
  fontId = SETTINGS.getReaderFontId();
  lineHeight = renderer.getLineHeight(fontId);
  // Readings and definitions use the reader family at about half size
  // (pixel fonts ship a 1x file next to the 2x reading size).
  smallFont = renderer.isSdCardFont(fontId) ? sdFontSystem.sizeVariant(fontId, 0.5f) : 0;
  if (smallFont == 0) smallFont = fontId;
  smallLineHeight = renderer.getLineHeight(smallFont);
  popupHeight = renderer.getScreenHeight() / 2;
  const int content = popupHeight - 2 * (POPUP_PAD + POPUP_BORDER);
  smallLinesPerPage = std::max(2, (content - lineHeight - POPUP_PAD / 2) / std::max(1, smallLineHeight));
  entryBuf = makeUniqueNoThrow<uint8_t[]>(ENTRY_BUF_BYTES);
  if (!entryBuf || !zh::openSharedDict() || !buildPage(pageIndex)) {
    LOG_ERR("ZH", "lookup unavailable");
    cancel();
    return;
  }
  // Tabs fit two pinyin syllables per line ("chuāng zhuàng" wraps once).
  tabWidth = renderer.getTextAdvanceX(smallFont, "zhuàng ", EpdFontFamily::REGULAR, 0) * 3 / 2 + 2 * POPUP_PAD;
  if (startInJump) startJump();
  // A button still held from opening this screen (the long-press) must not act here.
  {
    using B = MappedInputManager::Button;
    const unsigned long now = millis();
    const auto held = [&](Key& key, const bool level) {
      key.down = level;
      key.longFired = level;
      key.since = key.lastRepeat = now;
    };
    held(keyConfirm, mappedInput.isPressed(B::Confirm));
    held(keyBack, mappedInput.isPressed(B::Back));
    held(keyLeft, mappedInput.isPressed(B::ScreenLeft));
    held(keyRight, mappedInput.isPressed(B::ScreenRight));
    held(keyUp, mappedInput.isPressed(B::ScreenUp));
    held(keyDown, mappedInput.isPressed(B::ScreenDown));
  }
  lastMove = millis();
  ready = true;
  requestUpdate();
}

void ZhLookupActivity::onExit() {
  zh::sharedDict().close();
  if (onPageChanged) onPageChanged(pageIndex);
  Activity::onExit();
}

void ZhLookupActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

// ---------------------------------------------------------------------------
// Page and text model

bool ZhLookupActivity::extractChars(const Page& src, const bool visible, std::vector<CharBox>& out,
                                    bool& breakAfterLast) const {
  size_t capacity = 0;
  for (const auto& element : src.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* block = static_cast<const PageLine&>(*element).getBlock();
    if (block && block->valid()) capacity += block->wordCount();
  }
  out.reserve(out.size() + capacity);

  uint16_t lineIndex = 0;
  bool pendingBreak = false;
  for (const auto& element : src.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    const auto* block = line.getBlock();
    if (!block || !block->valid() || block->wordCount() == 0) continue;
    if (visible && renderer.isSdCardFont(fontId)) {
      const char* text = block->wordText(0);
      const uint16_t last = block->wordCount() - 1;
      const size_t len = block->wordText(last) + block->wordTextLen(last) + 1 - text;
      renderer.ensureSdCardFontReady(fontId, &text, &len, 1, false, false, 1);
    }
    const auto& rubyTexts = block->getRubyTexts();
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      if (block->wordStartsParagraph(i)) pendingBreak = true;
      const char* text = block->wordText(i);
      const auto* start = reinterpret_cast<const unsigned char*>(text);
      const unsigned char* p = start;
      const unsigned char* hanStart = nullptr;
      const unsigned char* hanEnd = nullptr;
      uint32_t hanCp = 0;
      while (*p) {
        const unsigned char* before = p;
        const uint32_t cp = utf8NextCodepoint(&p);
        if (cp == 0) break;
        if (zh::isHan(cp) && !hanStart) {
          hanStart = before;
          hanEnd = p;
          hanCp = cp;
        }
      }
      if (!hanStart) {
        pendingBreak = true;
        continue;
      }
      if (hanStart != start) pendingBreak = true;  // leading punctuation, e.g. “这
      CharBox c;
      c.cp = hanCp;
      c.breakBefore = pendingBreak;
      pendingBreak = false;
      if (visible) {
        // Headings and other sized text use their own font.
        const int font = TextBlock::renderFontId(renderer, block->wordFontId(i, fontId), fontId);
        c.font = font;
        char buf[32];
        int prefixW = 0;
        const size_t prefixLen = std::min<size_t>(hanStart - start, sizeof(buf) - 1);
        if (prefixLen) {
          memcpy(buf, text, prefixLen);
          buf[prefixLen] = 0;
          prefixW = renderer.getTextAdvanceX(font, buf, EpdFontFamily::REGULAR, 0);
        }
        const size_t charLen = hanEnd - hanStart;
        memcpy(buf, hanStart, charLen);
        buf[charLen] = 0;
        c.charW = static_cast<int16_t>(renderer.getTextAdvanceX(font, buf, EpdFontFamily::REGULAR, 0));
        c.ruby = i < rubyTexts.size() ? zh::decodeRubyCode(rubyTexts[i].c_str()) : 0;
        c.x = static_cast<int16_t>(marginLeft + line.xPos + block->wordXpos(i) + prefixW);
        c.y = static_cast<int16_t>(marginTop + line.yPos);
        c.w = static_cast<int16_t>(c.charW + (c.ruby ? zh::RUBY_WIDTH : 0));
        c.h = static_cast<int16_t>(renderer.getLineHeight(font));
        c.line = lineIndex;
      }
      out.push_back(c);
      if (*hanEnd) pendingBreak = true;  // trailing punctuation, e.g. 卡。
    }
    ++lineIndex;
  }
  breakAfterLast = pendingBreak;
  return true;
}

bool ZhLookupActivity::buildPage(const int index) {
  auto next = loadPage(index);
  if (!next) return false;
  std::vector<CharBox> cur;
  bool curBreakAfter = true;
  extractChars(*next, true, cur, curBreakAfter);

  // Context from the previous page: the tail of a run that continues here.
  std::vector<CharBox> tail;
  if (index > 0 && !cur.empty() && !cur.front().breakBefore) {
    if (auto prev = loadPage(index - 1)) {
      std::vector<CharBox> pv;
      bool pvBreakAfter = true;
      extractChars(*prev, false, pv, pvBreakAfter);
      if (!pvBreakAfter) {
        for (size_t k = pv.size(); k > 0 && tail.size() < CONTEXT_CHARS; --k) {
          tail.push_back(pv[k - 1]);
          if (pv[k - 1].breakBefore) break;
        }
        std::reverse(tail.begin(), tail.end());
        if (!tail.empty()) tail.front().breakBefore = true;
      }
    }
  }
  // Context from the next page: the head of a run continuing from here.
  std::vector<CharBox> head;
  if (index + 1 < pageCount && !cur.empty() && !curBreakAfter) {
    if (auto following = loadPage(index + 1)) {
      std::vector<CharBox> nx;
      bool nxBreakAfter = true;
      extractChars(*following, false, nx, nxBreakAfter);
      for (size_t k = 0; k < nx.size() && head.size() < CONTEXT_CHARS; ++k) {
        if (nx[k].breakBefore) break;
        head.push_back(nx[k]);
      }
    }
  }
  if (!cur.empty() && tail.empty()) cur.front().breakBefore = true;

  page = std::move(next);
  pageIndex = index;
  chars.clear();
  chars.reserve(tail.size() + cur.size() + head.size());
  chars.insert(chars.end(), tail.begin(), tail.end());
  chars.insert(chars.end(), cur.begin(), cur.end());
  chars.insert(chars.end(), head.begin(), head.end());
  firstOn = tail.size();
  endOn = firstOn + cur.size();
  cps.clear();
  cps.reserve(chars.size());
  for (const auto& c : chars) cps.push_back(c.cp);
  lineTotal = 0;
  for (size_t i = firstOn; i < endOn; ++i) lineTotal = std::max<uint16_t>(lineTotal, chars[i].line + 1);
  hanLines.clear();
  lineTops.assign(lineTotal, -1);
  for (size_t i = firstOn; i < endOn; ++i) {
    if (lineTops[chars[i].line] < 0) {
      lineTops[chars[i].line] = chars[i].y;
      hanLines.push_back(chars[i].line);
    }
  }
  segment();
  LOG_DBG("ZH", "lookup page %d: %u chars (+%u/%u context), %u words", index, static_cast<unsigned>(cur.size()),
          static_cast<unsigned>(tail.size()), static_cast<unsigned>(head.size()), static_cast<unsigned>(words.size()));
  return true;
}

size_t ZhLookupActivity::runEnd(const size_t i) const {
  size_t j = i + 1;
  while (j < chars.size() && !chars[j].breakBefore) ++j;
  return j;
}

void ZhLookupActivity::segment() {
  words.clear();
  words.reserve(chars.size());
  zh::Dict& dict = zh::sharedDict();
  size_t a = 0;
  while (a < chars.size()) {
    const size_t end = runEnd(a);
    while (a < end) {
      const size_t n = std::min(end - a, zh::MAX_RUN);
      const size_t base = a;
      const bool ok = zh::segmentRun(dict, cps.data() + base, n, [&](const zh::Segment& seg) {
        words.push_back({static_cast<uint16_t>(base + seg.start), seg.len});
      });
      if (!ok) {
        for (size_t k = 0; k < n; ++k) words.push_back({static_cast<uint16_t>(base + k), 1});
      }
      a += n;
    }
  }
}

// ---------------------------------------------------------------------------
// Cursor

size_t ZhLookupActivity::cursorStart() const {
  if (charMode) return static_cast<size_t>(cursor);
  return words.empty() ? firstOn : words[cursor].first;
}

size_t ZhLookupActivity::cursorLen() const {
  if (charMode || words.empty()) return 1;
  return words[cursor].len;
}

int ZhLookupActivity::wordContaining(const size_t i) const {
  for (size_t w = 0; w < words.size(); ++w) {
    if (i >= words[w].first && i < static_cast<size_t>(words[w].first + words[w].len)) return static_cast<int>(w);
  }
  return 0;
}

void ZhLookupActivity::moveCursor(const int delta) {
  if (charMode) {
    const int next = cursor + delta;
    if (next < static_cast<int>(firstOn) || next >= static_cast<int>(endOn)) {
      flipPage(delta);
    } else {
      cursor = next;
    }
    return;
  }
  for (int w = cursor + delta; w >= 0 && w < static_cast<int>(words.size()); w += delta) {
    if (wordVisible(words[w])) {
      cursor = w;
      return;
    }
  }
  flipPage(delta);
}

void ZhLookupActivity::moveLine(const int delta) {
  if (endOn == firstOn) {
    flipPage(delta);
    return;
  }
  const size_t start = std::clamp(cursorStart(), firstOn, endOn - 1);
  const CharBox& cur = chars[start];
  // The next line that has Han characters, in the given direction.
  int target = -1;
  for (size_t k = 0; k < hanLines.size(); ++k) {
    if (hanLines[k] != cur.line) continue;
    const int t = static_cast<int>(k) + delta;
    if (t >= 0 && t < static_cast<int>(hanLines.size())) target = hanLines[t];
    break;
  }
  if (target < 0) {
    flipPage(delta);
    return;
  }
  int bestDist = INT_MAX;
  size_t best = start;
  for (size_t i = firstOn; i < endOn; ++i) {
    if (chars[i].line != target) continue;
    const int dist = std::abs(chars[i].x - cur.x);
    if (dist < bestDist) {
      bestDist = dist;
      best = i;
    }
  }
  cursor = charMode ? static_cast<int>(best) : wordContaining(best);
}

void ZhLookupActivity::flipPage(const int delta) {
  const int target = pageIndex + delta;
  if (target < 0 || target >= pageCount) return;
  if (!buildPage(target)) return;
  if (charMode) {
    cursor = static_cast<int>(endOn > firstOn ? (delta > 0 ? firstOn : endOn - 1) : firstOn);
    return;
  }
  cursor = 0;
  if (delta > 0) {
    // Skip a word that started on the previous page: it was the last one there.
    for (size_t w = 0; w < words.size(); ++w) {
      if (words[w].first >= firstOn && words[w].first < endOn) {
        cursor = static_cast<int>(w);
        break;
      }
    }
  } else {
    for (size_t w = words.size(); w > 0; --w) {
      if (wordVisible(words[w - 1])) {
        cursor = static_cast<int>(w - 1);
        break;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Pop-up

void ZhLookupActivity::openPopup() {
  if (chars.empty()) return;
  focus = Focus::Popup;
  const size_t start = cursorStart();
  const size_t len = cursorLen();
  selStart = start;
  matchCount = zh::sharedDict().matchesAt(cps.data(), runEnd(start), start, matches, zh::MAX_MATCHES);
  // Word cursor: the word itself. Character cursor: every word starting at
  // this character, longest first (e.g. the second character of a four-character word).
  matchIndex = 0;
  for (int k = 0; k < matchCount && !charMode; ++k) {
    if (matches[k].len == len) matchIndex = k;
  }
  buildPopup();
}

void ZhLookupActivity::selectAt(const size_t start, const int index) {
  selStart = start;
  matchCount = zh::sharedDict().matchesAt(cps.data(), runEnd(start), start, matches, zh::MAX_MATCHES);
  matchIndex = index < 0 || index >= matchCount ? 0 : index;
  buildPopup();
}

void ZhLookupActivity::stepStart(const int delta) {
  const int s = static_cast<int>(selStart) + delta;
  if (s < static_cast<int>(firstOn) || s >= static_cast<int>(endOn)) return;
  selectAt(static_cast<size_t>(s), 0);
}

void ZhLookupActivity::wrapInto(const int font, const std::string& text, const int maxWidth, const int indent,
                                std::vector<std::string>& out) const {
  // Greedy wrap by codepoint; breaks at the last space unless that would leave
  // only the indent or a sense number ("2.") on the line.
  std::string cur;
  int curW = 0;
  size_t lastSpace = std::string::npos;
  const std::string indentText(indent, ' ');
  const int indentW = indent > 0 ? renderer.getTextAdvanceX(font, indentText.c_str(), EpdFontFamily::REGULAR, 0) : 0;
  const auto* p = reinterpret_cast<const unsigned char*>(text.c_str());
  while (*p) {
    const unsigned char* before = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    char one[5] = {};
    memcpy(one, before, p - before);
    const int w = renderer.getTextAdvanceX(font, one, EpdFontFamily::REGULAR, 0);
    if (curW + w > maxWidth && !cur.empty()) {
      std::string carry;
      if (lastSpace != std::string::npos && lastSpace > static_cast<size_t>(indent) + 3) {
        carry = cur.substr(lastSpace + 1);
        cur.resize(lastSpace);
      }
      out.push_back(std::move(cur));
      cur = indentText + carry;
      curW = indentW + (carry.empty() ? 0 : renderer.getTextAdvanceX(font, carry.c_str(), EpdFontFamily::REGULAR, 0));
      lastSpace = std::string::npos;
    }
    if (cp == ' ' && curW > 0) lastSpace = cur.size();
    cur.append(reinterpret_cast<const char*>(before), p - before);
    curW += w;
  }
  if (!cur.empty()) out.push_back(std::move(cur));
}

void ZhLookupActivity::buildPopup() {
  lines.clear();
  readings.clear();
  popupPages.clear();
  popupPage = 0;
  const int textWidth = popupTextWidth();

  if (matchCount == 0) {
    Reading r;
    lines.push_back(tr(STR_ZH_NOT_IN_DICT));
    r.lineCount = 1;
    readings.push_back(std::move(r));
    popupPages.push_back({0, 0});
    fitPopupHeight();
    return;
  }
  const zh::WordMatch& m = matches[matchIndex];
  const size_t got = zh::sharedDict().readEntry(m.entryOff, entryBuf.get(), ENTRY_BUF_BYTES);
  const uint8_t* p = entryBuf.get();
  const uint8_t* end = p + got;
  if (got == 0) {
    fitPopupHeight();
    return;
  }
  const int count = *p++;
  readings.reserve(count);
  for (int r = 0; r < count && p < end; ++r) {
    Reading rd;
    const auto takeString = [&](std::string& out) {
      if (p >= end) return false;
      const size_t n = std::min<size_t>(*p, end - p - 1);
      out.assign(reinterpret_cast<const char*>(p + 1), n);
      p += 1 + n;
      return true;
    };
    if (!takeString(rd.pinyin) || !takeString(rd.trad) || p >= end) break;
    const int syllables = *p++;
    for (int s = 0; s < syllables && end - p >= 3; ++s, p += 3) {
      if (s < zh::MAX_WORD_CHARS) rd.zhuyin[rd.syllables++] = p[0] | (p[1] << 8) | (p[2] << 16);
    }
    if (end - p < 2) break;
    const size_t defLen = std::min<size_t>(p[0] | (p[1] << 8), end - p - 2);
    const char* defs = reinterpret_cast<const char*>(p + 2);
    p += 2 + defLen;

    // The title row (reader font) is drawn per page; the rest uses the small font.
    if (r == 0 && m.hsk) rd.hsk = m.hsk;
    wrapInto(smallFont, rd.pinyin, tabWidth - 2 * POPUP_PAD, 0, rd.tabLines);
    rd.firstLine = static_cast<uint16_t>(lines.size());
    // Zhuyin is drawn beside the title's characters, like ruby in the book.
    wrapInto(smallFont, rd.pinyin, textWidth, 0, lines);
    int sense = 1;
    for (size_t s = 0; s < defLen;) {
      size_t e = s;
      while (e < defLen && defs[e] != '\x1f') ++e;
      wrapInto(smallFont, std::to_string(sense++) + ". " + std::string(defs + s, e - s), textWidth, 3, lines);
      s = e + 1;
    }
    rd.lineCount = static_cast<uint16_t>(lines.size() - rd.firstLine);
    for (size_t at = rd.firstLine; at < lines.size(); at += smallLinesPerPage) {
      popupPages.push_back({static_cast<uint8_t>(readings.size()), static_cast<uint16_t>(at)});
    }
    readings.push_back(std::move(rd));
  }
  if (popupPages.empty()) popupPages.push_back({0, 0});
  fitPopupHeight();
}

void ZhLookupActivity::fitPopupHeight() {
  // Shrink to the tallest page of this selection (tabs or text), so short
  // entries do not leave an empty box, without the height jumping between pages.
  int tabs = 0;
  for (const auto& r : readings) tabs += std::max<int>(1, r.tabLines.size()) * smallLineHeight + POPUP_PAD;
  int text = 0;
  for (const auto& pg : popupPages) {
    const Reading& rd = readings[pg.reading];
    const int pageLines = std::min<int>(smallLinesPerPage, rd.firstLine + rd.lineCount - pg.firstLine);
    text = std::max(text, lineHeight + POPUP_PAD / 2 + pageLines * smallLineHeight);
  }
  const int content = std::max(tabs, text);
  popupFit = std::min(popupHeight, content + 2 * (POPUP_PAD + POPUP_BORDER));
}

void ZhLookupActivity::buildPreview() {
  // DESIGN 4.4: every dictionary word starting at the cursor, longest first,
  // one line each in the small font: word, first reading, first sense.
  previewLines.clear();
  const size_t start = cursorStart();
  if (start >= chars.size()) return;
  zh::WordMatch found[zh::MAX_MATCHES];
  const int count = zh::sharedDict().matchesAt(cps.data(), runEnd(start), start, found, zh::MAX_MATCHES);
  const int width = renderer.getScreenWidth() - 2 * (POPUP_MARGIN + POPUP_BORDER + POPUP_PAD);
  std::vector<std::string> wrapped;
  for (int k = 0; k < count && static_cast<int>(previewLines.size()) < smallLinesPerPage; ++k) {
    std::string line;
    for (int c = 0; c < found[k].len; ++c) utf8AppendCodepoint(cps[start + c], line);
    const size_t got = zh::sharedDict().readEntry(found[k].entryOff, entryBuf.get(), ENTRY_BUF_BYTES);
    const uint8_t* p = entryBuf.get();
    const uint8_t* end = p + got;
    if (got > 3 && p[0] > 0) {
      const size_t pyLen = std::min<size_t>(p[1], got - 2);
      line += "  " + std::string(reinterpret_cast<const char*>(p + 2), pyLen);
      const uint8_t* q = p + 2 + pyLen;
      if (q < end) q += 1 + *q;        // traditional form
      if (q < end) q += 1 + 3 * (*q);  // zhuyin
      if (end - q >= 2) {
        const size_t defLen = std::min<size_t>(q[0] | (q[1] << 8), end - q - 2);
        const char* defs = reinterpret_cast<const char*>(q + 2);
        size_t e = 0;
        while (e < defLen && defs[e] != '\x1f') ++e;
        line += "  " + std::string(defs, e);
      }
    }
    // Keep the first wrapped line only.
    wrapped.clear();
    wrapInto(smallFont, line, width, 0, wrapped);
    if (!wrapped.empty()) previewLines.push_back(wrapped.size() > 1 ? wrapped[0] + "…" : wrapped[0]);
  }
}

// ---------------------------------------------------------------------------
// Jump mode (DESIGN 6)

void ZhLookupActivity::startJump() {
  if (hanLines.empty()) return;
  jumpReturn = focus == Focus::Jump ? Focus::Cursor : focus;
  focus = Focus::Jump;
  jumpStack.clear();
  jumpStack.push_back({0, static_cast<int>(hanLines.size())});
}

void ZhLookupActivity::bandRange(const int band, int& first, int& count) const {
  // DESIGN 6.2: g = min(4, n) groups; the first n mod g get one extra line.
  const Band& top = jumpStack.back();
  const int n = top.count;
  const int g = std::min(4, n);
  first = top.first;
  for (int b = 0; b < band && b < g; ++b) first += n / g + (b < n % g ? 1 : 0);
  count = band < g ? n / g + (band < n % g ? 1 : 0) : 0;
}

void ZhLookupActivity::pickBand(const int band) {
  int first, count;
  bandRange(band, first, count);
  if (count <= 0) return;
  if (count > 1) {
    jumpStack.push_back({first, count});
    return;
  }
  // One line left: the cursor lands on its first word.
  const uint16_t line = hanLines[first];
  for (size_t i = firstOn; i < endOn; ++i) {
    if (chars[i].line != line) continue;
    cursor = charMode ? static_cast<int>(i) : wordContaining(i);
    break;
  }
  jumpStack.clear();
  focus = Focus::Cursor;
}

void ZhLookupActivity::undoJump() {
  if (!jumpStack.empty()) jumpStack.pop_back();
  if (jumpStack.empty()) focus = jumpReturn;
}

// ---------------------------------------------------------------------------
// Drawing

void ZhLookupActivity::drawHighlight(const size_t first, const size_t len) {
  for (size_t k = first; k < first + len && k < chars.size(); ++k) {
    if (!onScreen(k)) continue;
    const CharBox& c = chars[k];
    renderer.fillRect(c.x, c.y, c.w, c.h, true);
    char buf[5] = {};
    utf8AppendCodepoint(buf, c.cp);
    renderer.drawText(c.font, c.x, c.y, buf, false);
    if (c.ruby) {
      const int rx = c.x + c.charW;
      zh::forEachRubyPixel(c.ruby, c.h, [&](int px, int py) { renderer.drawPixel(rx + px, c.y + py, false); });
    }
  }
}

int ZhLookupActivity::popupTextWidth() const {
  return renderer.getScreenWidth() - 2 * (POPUP_MARGIN + POPUP_BORDER + POPUP_PAD) - tabWidth - POPUP_PAD;
}

int ZhLookupActivity::popupTop(const int height) const {
  const int screenH = renderer.getScreenHeight();
  if (endOn == firstOn) return 0;
  const size_t s = std::clamp(focus == Focus::Popup ? selStart : cursorStart(), firstOn, endOn - 1);
  const CharBox& sel = chars[s];
  // Below the selection in the top half, above it otherwise; never over it.
  const int y = sel.y + sel.h / 2 < screenH / 2 ? sel.y + sel.h + 6 : sel.y - 6 - height;
  return std::clamp(y, 0, screenH - height);
}

void ZhLookupActivity::drawPreview() {
  if (previewLines.empty()) return;
  const int w = renderer.getScreenWidth() - 2 * POPUP_MARGIN;
  const int h = static_cast<int>(previewLines.size()) * smallLineHeight + 2 * (POPUP_PAD + POPUP_BORDER);
  const int x = POPUP_MARGIN;
  const int y = popupTop(h);
  renderer.fillRect(x, y, w, h, false);
  // Preview: double hairline (DESIGN 4.3).
  renderer.drawRect(x, y, w, h, true);
  renderer.drawRect(x + 3, y + 3, w - 6, h - 6, true);
  int ty = y + POPUP_BORDER + POPUP_PAD;
  for (const auto& line : previewLines) {
    renderer.drawText(smallFont, x + POPUP_BORDER + POPUP_PAD, ty, line.c_str(), true);
    ty += smallLineHeight;
  }
}

void ZhLookupActivity::drawPopup() {
  const int w = renderer.getScreenWidth() - 2 * POPUP_MARGIN;
  const int h = popupFit;
  const int x = POPUP_MARGIN;
  const int y = popupTop(h);
  // Focused pop-up: 4 px border (DESIGN 4.3).
  renderer.fillRect(x, y, w, h, true);
  renderer.fillRect(x + POPUP_BORDER, y + POPUP_BORDER, w - 2 * POPUP_BORDER, h - 2 * POPUP_BORDER, false);
  if (popupPages.empty() || readings.empty()) return;
  const PopupPage& pg = popupPages[std::min<size_t>(popupPage, popupPages.size() - 1)];
  const int innerY = y + POPUP_BORDER + POPUP_PAD;
  const int innerBottom = y + h - POPUP_BORDER - POPUP_PAD;

  // Reading tabs down the left, each wrapping its full pinyin in the small
  // font; the current one inverted. Scroll so the current tab is visible.
  const int tabX = x + POPUP_BORDER;
  const auto tabHeight = [&](const Reading& r) {
    return std::max<int>(1, r.tabLines.size()) * smallLineHeight + POPUP_PAD;
  };
  int firstTab = 0;
  for (;;) {
    int used = 0;
    for (int r = firstTab; r <= pg.reading; ++r) used += tabHeight(readings[r]);
    if (used <= innerBottom - innerY || firstTab == pg.reading) break;
    ++firstTab;
  }
  int ty = innerY - POPUP_PAD / 2;
  for (int r = firstTab; r < static_cast<int>(readings.size()); ++r) {
    const int th = tabHeight(readings[r]);
    if (ty + th > innerBottom + POPUP_PAD) break;
    const bool current = r == pg.reading;
    if (current) renderer.fillRect(tabX, ty, tabWidth, th, true);
    int ly = ty + POPUP_PAD / 2;
    for (const auto& line : readings[r].tabLines) {
      renderer.drawText(smallFont, tabX + POPUP_PAD, ly, line.c_str(), !current);
      ly += smallLineHeight;
    }
    ty += th;
    if (!current) renderer.drawLine(tabX, ty - 1, tabX + tabWidth - 1, ty - 1, true);
  }
  renderer.drawLine(tabX + tabWidth, y + POPUP_BORDER, tabX + tabWidth, y + h - POPUP_BORDER - 1, 2, true);

  // Right pane: the title in the reader font, then this page in the small font.
  const Reading& rd = readings[pg.reading];
  const int textX = tabX + tabWidth + POPUP_PAD;
  // Each character followed by its zhuyin column (as ruby in the book), then
  // the traditional form and HSK band.
  int tx = textX;
  const int wordLen = matchCount > 0 ? matches[matchIndex].len : 1;
  for (int k = 0; k < wordLen && selStart + k < cps.size(); ++k) {
    char ch[5] = {};
    utf8AppendCodepoint(ch, cps[selStart + k]);
    renderer.drawText(fontId, tx, innerY, ch, true);
    tx += renderer.getTextAdvanceX(fontId, ch, EpdFontFamily::REGULAR, 0);
    if (k < rd.syllables && rd.zhuyin[k]) {
      const int rx = tx;
      zh::forEachRubyPixel(rd.zhuyin[k], lineHeight, [&](int px, int py) { renderer.drawPixel(rx + px, innerY + py); });
      tx += zh::RUBY_WIDTH;
    }
  }
  if (!rd.trad.empty()) {
    const std::string trad = "【" + rd.trad + "】";
    renderer.drawText(fontId, tx, innerY, trad.c_str(), true);
    tx += renderer.getTextAdvanceX(fontId, trad.c_str(), EpdFontFamily::REGULAR, 0);
  }
  if (rd.hsk) {
    char hsk[8];
    snprintf(hsk, sizeof(hsk), "HSK%d", rd.hsk);
    renderer.drawText(smallFont, tx + POPUP_PAD, innerY + (lineHeight - smallLineHeight) / 2, hsk, true);
  }
  const size_t to = std::min<size_t>({static_cast<size_t>(rd.firstLine + rd.lineCount),
                                      static_cast<size_t>(pg.firstLine + smallLinesPerPage), lines.size()});
  int ly = innerY + lineHeight + POPUP_PAD / 2;
  for (size_t k = pg.firstLine; k < to; ++k) {
    renderer.drawText(smallFont, textX, ly, lines[k].c_str(), true);
    ly += smallLineHeight;
  }
  // Page position on the bottom border.
  if (popupPages.size() > 1) {
    char pos[16];
    snprintf(pos, sizeof(pos), " %d/%d ", popupPage + 1, static_cast<int>(popupPages.size()));
    const int pw = renderer.getTextWidth(SMALL_FONT_ID, pos);
    const int ph = renderer.getLineHeight(SMALL_FONT_ID);
    const int px = x + w - pw - 12;
    const int py = y + h - ph / 2 - 2;
    renderer.fillRect(px, py, pw, ph, false);
    renderer.drawText(SMALL_FONT_ID, px, py, pos, true);
  }
}

void ZhLookupActivity::drawJump() {
  if (jumpStack.empty()) return;
  const int width = renderer.getScreenWidth();
  const int g = std::min(4, jumpStack.back().count);
  const char* labels[] = {"1", "2", "3", "4"};
  for (int b = 0; b < g; ++b) {
    int first, count;
    bandRange(b, first, count);
    const int top = lineTops[hanLines[first]];
    const int bottom = lineTops[hanLines[first + count - 1]] + lineHeight;
    // Margin bar and label (DESIGN 6.1).
    renderer.fillRect(0, top + 2, 4, bottom - top - 4, true);
    const int labelH = renderer.getLineHeight(UI_10_FONT_ID);
    const int labelW = renderer.getTextWidth(UI_10_FONT_ID, labels[b]) + 8;
    const int ly = top + (bottom - top - labelH) / 2;
    renderer.fillRect(0, ly, labelW, labelH, true);
    renderer.drawText(UI_10_FONT_ID, 4, ly, labels[b], false);
    // Dashed rule between bands.
    if (b + 1 < g) {
      int nextFirst, nextCount;
      bandRange(b + 1, nextFirst, nextCount);
      const int ruleY = (bottom + lineTops[hanLines[nextFirst]]) / 2;
      for (int dx = 0; dx < width; dx += 10) renderer.fillRect(dx, ruleY, 6, 2, true);
    }
  }
}

void ZhLookupActivity::drawFrame() {
  renderer.clearScreen();
  page->render(renderer, fontId, marginLeft, marginTop);
  switch (focus) {
    case Focus::Cursor:
      drawHighlight(cursorStart(), cursorLen());
      if (previewShown) drawPreview();
      break;
    case Focus::Popup:
      drawHighlight(selStart, matchCount > 0 ? matches[matchIndex].len : 1);
      drawPopup();
      break;
    case Focus::Jump:
      drawJump();
      break;
  }
  const char* b1 = tr(STR_BACK);
  const char* b2 = tr(STR_LOOKUP);
  const char* b3 = charMode ? tr(STR_ZH_CHAR) : tr(STR_ZH_WORD);
  const char* b4 = ">";
  if (focus == Focus::Jump) {
    b1 = "1";
    b2 = "2";
    b3 = "3";
    b4 = "4";
  } else if (focus == Focus::Popup) {
    b2 = tr(STR_ZH_SHORTER);
    b3 = "<";
  }
  const auto labels = mappedInput.mapLabels(b1, b2, b3, b4);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ZhLookupActivity::render(RenderLock&& lock) {
  if (!ready) return;
  {
    auto* fcm = renderer.getFontCacheManager();
    auto scope = fcm->createPrewarmScope();
    drawFrame();
    scope.endScanAndPrewarm();
    drawFrame();
  }
  // The frame is in the buffer: let input keep moving the cursor during the
  // panel refresh (~380 ms on the X3). Presses made meanwhile are drawn as one
  // frame when it finishes, so a held button sweeps several words per refresh.
  lock.unlock();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

// ---------------------------------------------------------------------------
// Input

bool ZhLookupActivity::repeatDue(Key& key, const unsigned long now) {
  if (!key.down) return false;
  const unsigned long held = now - key.since;
  if (held < REPEAT_FIRST_MS) return false;
  const unsigned long interval = held < REPEAT_FAST_AFTER_MS ? REPEAT_MS : REPEAT_FAST_MS;
  if (key.lastRepeat != key.since && now - key.lastRepeat < interval) return false;
  key.lastRepeat = now;
  return true;
}

void ZhLookupActivity::loop() {
  if (!ready) return;
  handleInput();
  if (focus == Focus::Cursor && !previewShown && millis() - lastMove >= PREVIEW_DWELL_MS) {
    {
      RenderLock lock;
      buildPreview();
      previewShown = true;
    }
    requestUpdate();
  }
}

void ZhLookupActivity::handleInput() {
  using B = MappedInputManager::Button;
  const unsigned long now = millis();
  enum Event : uint8_t { NONE, PRESS, REPEAT, RELEASE_SHORT, HOLD };
  // Edges come from the input manager, so short taps are never missed; holds
  // and repeats come from the level.
  const auto update = [&](Key& key, const bool pressedEdge, const bool releasedEdge, const bool level,
                          const bool repeats) -> Event {
    if ((pressedEdge && !(key.down && key.longFired)) || (level && !key.down)) {
      key.down = true;
      key.longFired = false;
      key.since = now;
      key.lastRepeat = now;
      return PRESS;
    }
    if (releasedEdge || (!level && key.down)) {
      const bool wasLong = key.longFired;
      key.down = false;
      return wasLong ? NONE : RELEASE_SHORT;
    }
    if (!key.down) return NONE;
    if (repeats) return repeatDue(key, now) ? REPEAT : NONE;
    if (!key.longFired && now - key.since >= HOLD_MS) {
      key.longFired = true;
      return HOLD;
    }
    return NONE;
  };
  const auto edges = [&](Key& key, B a, B b, bool repeats) {
    return update(key, mappedInput.wasPressed(a) || mappedInput.wasPressed(b),
                  mappedInput.wasReleased(a) || mappedInput.wasReleased(b),
                  mappedInput.isPressed(a) || mappedInput.isPressed(b), repeats);
  };
  const Event left = edges(keyLeft, B::ScreenLeft, B::ScreenLeft, true);
  const Event right = edges(keyRight, B::ScreenRight, B::ScreenRight, true);
  // Side buttons only: NavNext/NavPrevious also include the front ◀ ▶.
  const Event up = edges(keyUp, B::ScreenUp, B::ScreenUp, true);
  const Event down = edges(keyDown, B::ScreenDown, B::ScreenDown, true);
  const Event confirm = edges(keyConfirm, B::Confirm, B::Confirm, false);
  const Event back = edges(keyBack, B::Back, B::Back, false);
  const auto step = [](const Event e) { return e == PRESS || e == REPEAT; };
  // A press that changes focus must not also act on its release.
  const auto consume = [](Key& key) { key.longFired = true; };

  bool changed = false;
  bool exit = false;
  {
    RenderLock lock;
    switch (focus) {
      case Focus::Cursor:
        if (step(left)) moveCursor(-1), changed = true;
        if (step(right)) moveCursor(1), changed = true;
        if (step(up)) moveLine(-1), changed = true;
        if (step(down)) moveLine(1), changed = true;
        // Confirm and Back act on release here, because holding them does something else.
        if (confirm == RELEASE_SHORT) openPopup(), changed = true;
        if (confirm == HOLD) startJump(), changed = true;
        if (back == HOLD) {
          // Toggle the word / character cursor, keeping the position.
          const size_t start = endOn > firstOn ? std::clamp(cursorStart(), firstOn, endOn - 1) : firstOn;
          charMode = !charMode;
          cursor = charMode ? static_cast<int>(start) : wordContaining(start);
          changed = true;
        }
        if (back == RELEASE_SHORT) exit = true;
        break;
      case Focus::Popup:
        if (step(left) && popupPage > 0) --popupPage, changed = true;
        if (step(right) && popupPage + 1 < static_cast<int>(popupPages.size())) ++popupPage, changed = true;
        if (step(up)) stepStart(-1), changed = true;
        if (step(down)) stepStart(1), changed = true;
        if (confirm == PRESS && matchCount > 1) {
          matchIndex = (matchIndex + 1) % matchCount;
          buildPopup();
          changed = true;
        }
        if (back == PRESS) {
          consume(keyBack);
          focus = Focus::Cursor;
          cursor = charMode ? static_cast<int>(selStart) : wordContaining(selStart);
          changed = true;
        }
        break;
      case Focus::Jump:
        // DESIGN 6.1: Back, Confirm, ◀, ▶ pick bands 1-4 top to bottom.
        if (back == PRESS) consume(keyBack), pickBand(0), changed = true;
        if (confirm == PRESS) consume(keyConfirm), pickBand(1), changed = true;
        if (left == PRESS) pickBand(2), changed = true;
        if (right == PRESS) pickBand(3), changed = true;
        if (up == PRESS) undoJump(), changed = true;
        if (down == PRESS) jumpStack.clear(), focus = jumpReturn, changed = true;
        break;
    }
    if (changed) {
      lastMove = now;
      previewShown = false;
    }
  }
  if (exit) {
    cancel();
    return;
  }
  if (changed) requestUpdate();
}
