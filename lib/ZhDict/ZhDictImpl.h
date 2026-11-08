#pragma once
// Template definitions for ZhDict.h. Include ZhDict.h, not this file.

#include <cmath>

#include "ZhuyinGlyphs.h"

namespace zh {

namespace detail {
// A word costs WORD_COST + ln(rank): with Zipf's law (p ~ 1/rank) the cheapest
// path is the most probable split. Must match scripts/zh/zhdict_ref.py.
constexpr float WORD_COST = 3.0f;
constexpr float UNRANKED_LOG = 13.8155f;                // ln(1e6)
constexpr float UNKNOWN_CHAR_COST = WORD_COST + 16.0f;  // not in the dictionary

inline float wordCost(uint32_t rank) { return WORD_COST + (rank == NO_RANK ? UNRANKED_LOG : logf(rank)); }

// Tone marks drawn by hand (DESIGN 7.1): rows top to bottom, MSB = leftmost of `w` bits.
struct ToneMark {
  uint8_t w, h;
  uint8_t rows[4];
};
constexpr ToneMark TONE_MARKS[4] = {
    {4, 4, {0b0001, 0b0010, 0b0100, 0b1000}},  // ˊ tone 2
    {5, 3, {0b10001, 0b01010, 0b00100, 0}},    // ˇ tone 3
    {4, 4, {0b1000, 0b0100, 0b0010, 0b0001}},  // ˋ tone 4
    {2, 2, {0b11, 0b11, 0, 0}},                // ˙ neutral
};
}  // namespace detail

template <typename F>
bool segmentRun(Dict& dict, const uint32_t* cps, size_t n, F&& onSegment) {
  SegmentScratch* s = dict.scratch();
  if (!s || n == 0 || n > MAX_RUN) return false;
  for (size_t j = 0; j <= n; ++j) s->best[j] = INFINITY;
  s->best[0] = 0.0f;
  for (size_t i = 0; i < n; ++i) {
    if (std::isinf(s->best[i])) continue;
    const int count = dict.matchesAt(cps, n, i, s->matches, MAX_MATCHES);
    bool hasSingle = false;
    for (int k = 0; k < count; ++k) {
      const WordMatch& m = s->matches[k];
      hasSingle |= m.len == 1;
      const size_t j = i + m.len;
      const float cost = s->best[i] + detail::wordCost(m.rank);
      if (cost < s->best[j]) {
        s->best[j] = cost;
        Segment& g = s->seg[j];
        g.start = static_cast<uint16_t>(i);
        g.len = m.len;
        g.hsk = m.hsk;
        g.rank = m.rank;
        g.entryOff = m.entryOff;
        g.inDict = true;
        for (int c = 0; c < MAX_WORD_CHARS; ++c) g.zhuyin[c] = m.zhuyin[c];
      }
    }
    if (!hasSingle && s->best[i] + detail::UNKNOWN_CHAR_COST < s->best[i + 1]) {
      s->best[i + 1] = s->best[i] + detail::UNKNOWN_CHAR_COST;
      Segment& g = s->seg[i + 1];
      g = Segment{};
      g.start = static_cast<uint16_t>(i);
      g.len = 1;
      g.rank = NO_RANK;
      g.inDict = false;
    }
  }
  // Walk back from the end, then report in reading order.
  size_t steps = 0;
  for (size_t j = n; j > 0; j = s->seg[j].start) s->path[steps++] = static_cast<uint8_t>(j);
  while (steps > 0) onSegment(s->seg[s->path[--steps]]);
  return true;
}

template <typename F>
void forEachRubyPixel(uint32_t code, int charHeight, F&& plot) {
  if (code == 0) return;
  int symbols[3];
  int count = 0;
  for (int k = 0; k < 3; ++k) {
    const int sym = zhuyinSymbol(code, k);
    if (sym >= 1 && sym <= ZHUYIN_GLYPH_COUNT) symbols[count++] = sym - 1;
  }
  if (count == 0) return;
  // DESIGN 7.2: 9x9 symbols at a 10 px pitch in a column centred on the
  // character, starting 1 px into the block; tone column to its right.
  constexpr int COLUMN_X = 1;
  constexpr int TONE_X = 11;
  const int columnHeight = count * 10 - 1;
  const int top = (charHeight - columnHeight) / 2;
  for (int k = 0; k < count; ++k) {
    const uint16_t* rows = ZHUYIN_GLYPHS[symbols[k]];
    for (int y = 0; y < 9; ++y) {
      for (int x = 0; x < 9; ++x) {
        if (rows[y] & (0x100 >> x)) plot(COLUMN_X + x, top + 10 * k + y);
      }
    }
  }
  const int tone = zhuyinTone(code);
  if (tone < 2 || tone > 5) return;
  const detail::ToneMark& t = detail::TONE_MARKS[tone - 2];
  int tx, ty;
  if (tone == 5) {
    // Neutral tone sits above the first symbol, centred on the column.
    tx = COLUMN_X + (9 - t.w) / 2;
    ty = top - t.h - 1;
  } else {
    // Beside the last symbol, 2 px higher when the column has 2-3 symbols.
    tx = TONE_X;
    ty = top + 10 * (count - 1) + (9 - t.h) / 2 - (count >= 2 ? 2 : 0);
  }
  for (int y = 0; y < t.h; ++y) {
    for (int x = 0; x < t.w; ++x) {
      if (t.rows[y] & (1 << (t.w - 1 - x))) plot(tx + x, ty + y);
    }
  }
}

}  // namespace zh
