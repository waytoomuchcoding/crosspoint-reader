#include "ZhLookupActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"

namespace {

constexpr size_t ENTRY_BUF_BYTES = 3072;
constexpr int POPUP_PAD = 10;
constexpr int POPUP_BORDER = 4;
constexpr int MAX_CARD_LINES = 7;  // DESIGN 4.4

std::string zhuyinText(uint32_t code) {
  std::string out;
  const int tone = zh::zhuyinTone(code);
  if (tone == 5) utf8AppendCodepoint(0x02D9, out);  // ˙ leads
  for (int k = 0; k < 3; ++k) {
    const int sym = zh::zhuyinSymbol(code, k);
    if (sym) utf8AppendCodepoint(0x3104 + sym, out);
  }
  if (tone == 2) utf8AppendCodepoint(0x02CA, out);
  if (tone == 3) utf8AppendCodepoint(0x02C7, out);
  if (tone == 4) utf8AppendCodepoint(0x02CB, out);
  return out;
}

}  // namespace

void ZhLookupActivity::onEnter() {
  Activity::onEnter();
  fontId = SETTINGS.getReaderFontId();
  lineHeight = renderer.getLineHeight(fontId);
  const int popupHeight = renderer.getScreenHeight() * 2 / 5;
  linesPerCard =
      std::clamp((popupHeight - 2 * (POPUP_PAD + POPUP_BORDER)) / std::max(1, lineHeight), 3, MAX_CARD_LINES);
  entryBuf = makeUniqueNoThrow<uint8_t[]>(ENTRY_BUF_BYTES);
  if (!entryBuf || !zh::openSharedDict() || !extractChars()) {
    LOG_ERR("ZH", "lookup unavailable (dictionary %d, chars %u)", zh::sharedDict().isOpen() ? 1 : 0,
            static_cast<unsigned>(chars.size()));
    cancel();
    return;
  }
  segmentPage();
  if (words.empty()) {
    cancel();
    return;
  }
  ready = true;
  LOG_INF("ZH", "lookup: %u chars, %u words on page", static_cast<unsigned>(chars.size()),
          static_cast<unsigned>(words.size()));
  requestUpdate();
}

void ZhLookupActivity::onExit() {
  zh::sharedDict().close();
  Activity::onExit();
}

void ZhLookupActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

bool ZhLookupActivity::extractChars() {
  size_t capacity = 0;
  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* block = static_cast<const PageLine&>(*element).getBlock();
    if (block && block->valid()) capacity += block->wordCount();
  }
  chars.reserve(capacity);
  cps.reserve(capacity);

  uint16_t lineIndex = 0;
  uint16_t run = 0;
  bool pendingBreak = false;
  const auto addBreak = [&]() { pendingBreak = true; };
  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    const auto* block = line.getBlock();
    if (!block || !block->valid() || block->wordCount() == 0) continue;
    if (renderer.isSdCardFont(fontId)) {
      const char* text = block->wordText(0);
      const uint16_t last = block->wordCount() - 1;
      const size_t len = block->wordText(last) + block->wordTextLen(last) + 1 - text;
      renderer.ensureSdCardFontReady(fontId, &text, &len, 1, false, false, 1);
    }
    const auto& rubyTexts = block->getRubyTexts();
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      if (block->wordStartsParagraph(i)) addBreak();
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
        addBreak();
        continue;
      }
      if (hanStart != start) addBreak();  // leading punctuation, e.g. “这
      char buf[32];
      int prefixW = 0;
      const size_t prefixLen = std::min<size_t>(hanStart - start, sizeof(buf) - 1);
      if (prefixLen) {
        memcpy(buf, text, prefixLen);
        buf[prefixLen] = 0;
        prefixW = renderer.getTextAdvanceX(fontId, buf, EpdFontFamily::REGULAR, 0);
      }
      const size_t charLen = hanEnd - hanStart;
      memcpy(buf, hanStart, charLen);
      buf[charLen] = 0;
      const int charW = renderer.getTextAdvanceX(fontId, buf, EpdFontFamily::REGULAR, 0);

      if (pendingBreak && !chars.empty()) ++run;
      pendingBreak = false;
      CharBox c{};
      c.cp = hanCp;
      c.ruby = i < rubyTexts.size() ? zh::decodeRubyCode(rubyTexts[i].c_str()) : 0;
      c.charW = static_cast<int16_t>(charW);
      c.x = static_cast<int16_t>(marginLeft + line.xPos + block->wordXpos(i) + prefixW);
      c.y = static_cast<int16_t>(marginTop + line.yPos);
      c.w = static_cast<int16_t>(charW + (c.ruby ? zh::RUBY_WIDTH : 0));
      c.h = static_cast<int16_t>(lineHeight);
      c.line = lineIndex;
      c.run = run;
      chars.push_back(c);
      cps.push_back(hanCp);
      if (*hanEnd) addBreak();  // trailing punctuation, e.g. 卡。
    }
    ++lineIndex;
  }
  return !chars.empty();
}

size_t ZhLookupActivity::runEnd(const size_t i) const {
  size_t j = i;
  while (j < chars.size() && chars[j].run == chars[i].run) ++j;
  return j;
}

void ZhLookupActivity::segmentPage() {
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

int ZhLookupActivity::wordAtChar(const size_t i) const {
  for (size_t w = 0; w < words.size(); ++w) {
    if (i >= words[w].first && i < static_cast<size_t>(words[w].first + words[w].len)) return static_cast<int>(w);
  }
  return 0;
}

void ZhLookupActivity::moveWord(const int delta) {
  cursor = std::clamp(cursor + delta, 0, static_cast<int>(words.size()) - 1);
}

void ZhLookupActivity::moveLine(const int delta) {
  const CharBox& cur = chars[words[cursor].first];
  const int target = static_cast<int>(cur.line) + delta;
  int best = -1;
  int bestDist = INT32_MAX;
  for (size_t w = 0; w < words.size(); ++w) {
    const CharBox& c = chars[words[w].first];
    if (c.line != target) continue;
    const int dist = std::abs(c.x - cur.x);
    if (dist < bestDist) {
      bestDist = dist;
      best = static_cast<int>(w);
    }
  }
  if (best >= 0) cursor = best;
}

void ZhLookupActivity::openPopup() {
  focus = Focus::Popup;
  const Word& w = words[cursor];
  selectAt(w.first, -1);
  for (int k = 0; k < matchCount; ++k) {
    if (matches[k].len == w.len) {
      matchIndex = k;
      buildCards();
      break;
    }
  }
}

void ZhLookupActivity::selectAt(const size_t start, const int index) {
  selStart = start;
  matchCount = zh::sharedDict().matchesAt(cps.data(), runEnd(start), start, matches, zh::MAX_MATCHES);
  matchIndex = index < 0 || index >= matchCount ? 0 : index;
  buildCards();
}

void ZhLookupActivity::stepStart(const int delta) {
  const int s = static_cast<int>(selStart) + delta;
  if (s < 0 || s >= static_cast<int>(chars.size())) return;
  selectAt(static_cast<size_t>(s), 0);
}

void ZhLookupActivity::wrapText(const char* text, const int maxWidth, const int indent) {
  // Greedy wrap by codepoint, breaking at the last space when there is one.
  std::string cur;
  int curW = 0;
  size_t lastSpace = std::string::npos;
  const int indentW =
      indent > 0 ? renderer.getTextAdvanceX(fontId, std::string(indent, ' ').c_str(), EpdFontFamily::REGULAR, 0) : 0;
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  while (*p) {
    const unsigned char* before = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    char one[5] = {};
    memcpy(one, before, p - before);
    const int w = renderer.getTextAdvanceX(fontId, one, EpdFontFamily::REGULAR, 0);
    if (curW + w > maxWidth && !cur.empty()) {
      std::string carry;
      // Break at the last space unless that would leave only the indent or a
      // sense number ("2.") on the line.
      if (lastSpace != std::string::npos && lastSpace > static_cast<size_t>(indent) + 3) {
        carry = cur.substr(lastSpace + 1);
        cur.resize(lastSpace);
      }
      lines.push_back(std::move(cur));
      cur = std::string(indent, ' ') + carry;
      curW = indentW + (carry.empty() ? 0 : renderer.getTextAdvanceX(fontId, carry.c_str(), EpdFontFamily::REGULAR, 0));
      lastSpace = std::string::npos;
    }
    if (cp == ' ') lastSpace = cur.size();
    cur.append(reinterpret_cast<const char*>(before), p - before);
    curW += w;
  }
  if (!cur.empty()) lines.push_back(std::move(cur));
}

void ZhLookupActivity::buildCards() {
  lines.clear();
  cardStart.clear();
  card = 0;
  std::string head;
  const int len = matchCount > 0 ? matches[matchIndex].len : 1;
  for (int k = 0; k < len && selStart + k < cps.size(); ++k) utf8AppendCodepoint(cps[selStart + k], head);
  const int maxWidth = renderer.getScreenWidth() - 2 * (8 + POPUP_BORDER + POPUP_PAD);

  if (matchCount == 0) {
    cardStart.push_back(0);
    lines.push_back(head);
    lines.push_back(tr(STR_ZH_NOT_IN_DICT));
    return;
  }
  const zh::WordMatch& m = matches[matchIndex];
  const size_t got = zh::sharedDict().readEntry(m.entryOff, entryBuf.get(), ENTRY_BUF_BYTES);
  const uint8_t* b = entryBuf.get();
  const uint8_t* end = b + got;
  if (got == 0) return;
  const int readings = b[0];
  const uint8_t* p = b + 1;
  for (int r = 0; r < readings && p < end; ++r) {
    if (end - p < 1) break;
    const std::string pinyin(reinterpret_cast<const char*>(p + 1), std::min<size_t>(p[0], end - p - 1));
    p += 1 + p[0];
    if (p >= end) break;
    const std::string trad(reinterpret_cast<const char*>(p + 1), std::min<size_t>(p[0], end - p - 1));
    p += 1 + p[0];
    if (end - p < 2) break;
    const size_t defLen = std::min<size_t>(p[0] | (p[1] << 8), end - p - 2);
    const char* defs = reinterpret_cast<const char*>(p + 2);
    p += 2 + defLen;

    const size_t first = lines.size();
    std::string title = head;
    if (!trad.empty()) title += "【" + trad + "】";
    if (readings > 1) title += "  " + std::to_string(r + 1) + "/" + std::to_string(readings);
    if (r == 0 && m.hsk) title += "  HSK" + std::to_string(m.hsk);
    lines.push_back(std::move(title));
    std::string sound = pinyin;
    if (r == 0) {
      sound += "  ";
      for (int k = 0; k < m.len; ++k) {
        if (k) sound += " ";
        sound += zhuyinText(m.zhuyin[k]);
      }
    }
    wrapText(sound.c_str(), maxWidth, 0);
    int sense = 1;
    size_t s = 0;
    while (s < defLen) {
      size_t e = s;
      while (e < defLen && defs[e] != '\x1f') ++e;
      std::string text = std::to_string(sense++) + ". " + std::string(defs + s, e - s);
      wrapText(text.c_str(), maxWidth, 3);
      s = e + 1;
    }
    // Long readings continue on further cards.
    for (size_t at = first; at < lines.size(); at += linesPerCard) cardStart.push_back(static_cast<uint16_t>(at));
  }
  if (cardStart.empty()) cardStart.push_back(0);
}

void ZhLookupActivity::drawHighlight(const size_t first, const size_t len) {
  for (size_t k = first; k < first + len && k < chars.size(); ++k) {
    const CharBox& c = chars[k];
    renderer.fillRect(c.x, c.y, c.w, c.h, true);
    char buf[5] = {};
    utf8AppendCodepoint(buf, c.cp);
    renderer.drawText(fontId, c.x, c.y, buf, false);
    if (c.ruby) {
      const int rx = c.x + c.charW;
      zh::forEachRubyPixel(c.ruby, c.h, [&](int px, int py) { renderer.drawPixel(rx + px, c.y + py, false); });
    }
  }
}

void ZhLookupActivity::drawPopup() {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int w = screenW - 16;
  const int h = linesPerCard * lineHeight + 2 * (POPUP_PAD + POPUP_BORDER);
  const CharBox& sel = chars[selStart];
  int y = sel.y + sel.h / 2 < screenH / 2 ? sel.y + sel.h + 6 : sel.y - 6 - h;
  y = std::clamp(y, 0, screenH - h);
  const int x = 8;
  renderer.fillRect(x, y, w, h, true);
  renderer.fillRect(x + POPUP_BORDER, y + POPUP_BORDER, w - 2 * POPUP_BORDER, h - 2 * POPUP_BORDER, false);
  const size_t from = card < static_cast<int>(cardStart.size()) ? cardStart[card] : 0;
  size_t to = card + 1 < static_cast<int>(cardStart.size()) ? cardStart[card + 1] : lines.size();
  to = std::min(to, from + linesPerCard);
  int ty = y + POPUP_BORDER + POPUP_PAD;
  for (size_t k = from; k < to; ++k) {
    renderer.drawText(fontId, x + POPUP_BORDER + POPUP_PAD, ty, lines[k].c_str(), true);
    ty += lineHeight;
  }
}

void ZhLookupActivity::drawFrame() {
  renderer.clearScreen();
  page->render(renderer, fontId, marginLeft, marginTop);
  if (focus == Focus::Cursor) {
    drawHighlight(words[cursor].first, words[cursor].len);
  } else {
    drawHighlight(selStart, matchCount > 0 ? matches[matchIndex].len : 1);
    drawPopup();
  }
  const auto labels =
      focus == Focus::Cursor
          ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_LOOKUP), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT))
          : mappedInput.mapLabels(tr(STR_BACK), tr(STR_ZH_SHORTER), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ZhLookupActivity::render(RenderLock&&) {
  if (!ready) return;
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  drawFrame();
  scope.endScanAndPrewarm();
  drawFrame();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void ZhLookupActivity::loop() {
  if (!ready) return;
  const uint8_t b = input.pollButtons(mappedInput, millis());
  if (!b) return;
  using In = WordSelectionInput;
  const bool sideUp = b & (In::INPUT_UP | In::INPUT_PREVIOUS);
  const bool sideDown = b & (In::INPUT_DOWN | In::INPUT_NEXT);
  if (focus == Focus::Cursor) {
    if (b & In::INPUT_BACK) {
      cancel();
      return;
    }
    if (b & In::INPUT_LEFT) moveWord(-1);
    if (b & In::INPUT_RIGHT) moveWord(1);
    if (sideUp) moveLine(-1);
    if (sideDown) moveLine(1);
    if (b & In::INPUT_CONFIRM) openPopup();
  } else {
    if (b & In::INPUT_BACK) {
      focus = Focus::Cursor;
      cursor = wordAtChar(selStart);
    }
    if (b & In::INPUT_LEFT) card = std::max(0, card - 1);
    if (b & In::INPUT_RIGHT) card = std::min(static_cast<int>(cardStart.size()) - 1, card + 1);
    if (sideUp) stepStart(-1);
    if (sideDown) stepStart(1);
    if ((b & In::INPUT_CONFIRM) && matchCount > 1) {
      matchIndex = (matchIndex + 1) % matchCount;
      buildCards();
    }
  }
  requestUpdate();
}
