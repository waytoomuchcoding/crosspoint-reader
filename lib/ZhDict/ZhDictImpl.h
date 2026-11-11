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
  uint8_t rows[5];
};
// Per glyph set, tones 2, 3, 4 and neutral.
constexpr ToneMark TONE_MARKS_5X7[4] = {
    {3, 3, {0b001, 0b010, 0b100}},  // ˊ
    {3, 2, {0b101, 0b010}},         // ˇ
    {3, 3, {0b100, 0b010, 0b001}},  // ˋ
    {2, 2, {0b11, 0b11}},           // ˙
};
constexpr ToneMark TONE_MARKS_9X9[4] = {
    {4, 4, {0b0001, 0b0010, 0b0100, 0b1000}},  // ˊ
    {5, 3, {0b10001, 0b01010, 0b00100}},       // ˇ
    {4, 4, {0b1000, 0b0100, 0b0010, 0b0001}},  // ˋ
    {2, 2, {0b11, 0b11}},                      // ˙
};
constexpr ToneMark TONE_MARKS_11X11[4] = {
    {5, 5, {0b00001, 0b00010, 0b00100, 0b01000, 0b10000}},  // ˊ
    {7, 4, {0b1000001, 0b0100010, 0b0010100, 0b0001000}},   // ˇ
    {5, 5, {0b10000, 0b01000, 0b00100, 0b00010, 0b00001}},  // ˋ
    {2, 2, {0b11, 0b11}},                                   // ˙
};

struct GlyphSet {
  uint8_t w, h;           // symbol size; symbols stack at a pitch of h + 1
  const uint16_t* rows;   // ZHUYIN_GLYPH_COUNT glyphs of h rows each
  const ToneMark* tones;  // TONE_MARKS_*
  uint8_t toneW;          // widest tone mark beside the column
  uint8_t lift;           // tone raised this much beside a 2-3 symbol column
};
constexpr GlyphSet GLYPH_SETS[3] = {
    {5, 7, &ZHUYIN_GLYPHS_5X7[0][0], TONE_MARKS_5X7, 3, 1},
    {9, 9, &ZHUYIN_GLYPHS_9X9[0][0], TONE_MARKS_9X9, 5, 2},
    {11, 11, &ZHUYIN_GLYPHS_11X11[0][0], TONE_MARKS_11X11, 7, 2},
};

// Styles by the smallest em that uses them. At 1x the three-symbol column (23, 29,
// 35 px) fits the em. Scaled sets start while their column (46, 58, 70, 87, 105 px)
// is still up to ~15% taller than the em, so stroke weight keeps up with large
// pixel-font text (3x text has 3 px strokes) instead of staying 1 px thin.
struct RubyStep {
  uint8_t minEm;
  RubyStyle style;
};
constexpr RubyStep RUBY_STEPS_BY_EM[] = {{0, {0, 1}},  {29, {1, 1}}, {35, {2, 1}}, {40, {0, 2}},
                                         {54, {1, 2}}, {66, {2, 2}}, {80, {1, 3}}, {96, {2, 3}}};

constexpr int columnHeight(const RubyStyle& st, int symbols) {
  return (symbols * (GLYPH_SETS[st.set].h + 1) - 1) * st.scale;
}
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
    s->selfRank[i] = NO_RANK;
    s->selfZhuyin[i] = 0;
    for (int k = 0; k < count; ++k) {
      const WordMatch& m = s->matches[k];
      if (m.len == 1) {
        hasSingle = true;
        s->selfRank[i] = m.rank;
        s->selfZhuyin[i] = m.zhuyin[0];
      }
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

inline RubyStyle rubyStyle(int em) {
  RubyStyle best = detail::RUBY_STEPS_BY_EM[0].style;
  for (const detail::RubyStep& step : detail::RUBY_STEPS_BY_EM) {
    if (em >= step.minEm) best = step.style;
  }
  return best;
}

inline int rubyWidth(int em) {
  const RubyStyle st = rubyStyle(em);
  const detail::GlyphSet& g = detail::GLYPH_SETS[st.set];
  return (1 + g.w + 1 + g.toneW) * st.scale;
}

namespace detail {
inline void glyphColSpan(const GlyphSet& g, int symIdx, int& firstCol, int& spanW) {
  // Horizontal ㄧ (index 33) fills all columns in the vertical column set; trim
  // 1 column on each side in horizontal layout so it does not fuse with neighbors.
  if (symIdx == 33 && g.w >= 9) {
    firstCol = 1;
    spanW = g.w - 2;
    return;
  }
  uint16_t mask = 0;
  const uint16_t* rows = g.rows + symIdx * g.h;
  for (int y = 0; y < g.h; ++y) mask |= rows[y];
  int left = 0;
  while (left < g.w && (mask & (1 << (g.w - 1 - left))) == 0) ++left;
  int right = g.w - 1;
  while (right > left && (mask & (1 << (g.w - 1 - right))) == 0) --right;
  firstCol = left;
  spanW = right >= left ? (right - left + 1) : g.w;
}
}  // namespace detail

inline int rubyUnderWidth(uint32_t code, int em) {
  if (code == 0) return 0;
  int symbols[3];
  int count = 0;
  for (int k = 0; k < 3; ++k) {
    const int sym = zhuyinSymbol(code, k);
    if (sym >= 1 && sym <= ZHUYIN_GLYPH_COUNT) symbols[count++] = sym - 1;
  }
  if (count == 0) return 0;
  const RubyStyle st = rubyStyle(em);
  const detail::GlyphSet& g = detail::GLYPH_SETS[st.set];
  int designW = 0;
  const int tone = zhuyinTone(code);
  if (tone == 5) designW += g.tones[3].w + 1;
  for (int k = 0; k < count; ++k) {
    int firstCol = 0, spanW = g.w;
    detail::glyphColSpan(g, symbols[k], firstCol, spanW);
    designW += spanW + (k + 1 < count ? 1 : 0);
  }
  if (tone >= 2 && tone <= 4) designW += 1 + g.tones[tone - 2].w;
  return designW * st.scale;
}

inline int rubyUnderExtraWidth(uint32_t code, int em) {
  const int w = rubyUnderWidth(code, em);
  if (w == 0) return 0;
  const int minCell = w + 4 * rubyStyle(em).scale;
  return minCell > em ? minCell - em : 0;
}

inline int rubyUnderHeight(int em) {
  const RubyStyle st = rubyStyle(em);
  return (detail::GLYPH_SETS[st.set].h + 6) * st.scale;
}

template <typename F>
void forEachRubyUnderPixel(uint32_t code, int em, int cellWidth, int lineHeight, F&& plot) {
  if (code == 0) return;
  int symbols[3];
  int count = 0;
  for (int k = 0; k < 3; ++k) {
    const int sym = zhuyinSymbol(code, k);
    if (sym >= 1 && sym <= ZHUYIN_GLYPH_COUNT) symbols[count++] = sym - 1;
  }
  if (count == 0) return;
  const RubyStyle st = rubyStyle(em);
  const detail::GlyphSet& g = detail::GLYPH_SETS[st.set];
  const int s = st.scale;
  auto dot = [&](int x, int y) {
    for (int dy = 0; dy < s; ++dy) {
      for (int dx = 0; dx < s; ++dx) plot(x + dx, y + dy);
    }
  };
  int firstCols[3], spanWs[3];
  int designW = 0;
  const int tone = zhuyinTone(code);
  if (tone == 5) designW += g.tones[3].w + 1;
  for (int k = 0; k < count; ++k) {
    detail::glyphColSpan(g, symbols[k], firstCols[k], spanWs[k]);
    designW += spanWs[k] + (k + 1 < count ? 1 : 0);
  }
  if (tone >= 2 && tone <= 4) designW += 1 + g.tones[tone - 2].w;

  int curX = (cellWidth - designW * s) / 2;
  const int topY = lineHeight + 3 * s;
  if (tone == 5) {
    const detail::ToneMark& t = g.tones[3];
    const int ty = topY + ((g.h - t.h) / 2) * s;
    for (int y = 0; y < t.h; ++y) {
      for (int x = 0; x < t.w; ++x) {
        if (t.rows[y] & (1 << (t.w - 1 - x))) dot(curX + x * s, ty + y * s);
      }
    }
    curX += (t.w + 1) * s;
  }
  for (int k = 0; k < count; ++k) {
    const uint16_t* rows = g.rows + symbols[k] * g.h;
    for (int y = 0; y < g.h; ++y) {
      for (int x = 0; x < spanWs[k]; ++x) {
        if (rows[y] & (1 << (g.w - 1 - (firstCols[k] + x)))) dot(curX + x * s, topY + y * s);
      }
    }
    curX += (spanWs[k] + (k + 1 < count ? 1 : 0)) * s;
  }
  if (tone >= 2 && tone <= 4) {
    const detail::ToneMark& t = g.tones[tone - 2];
    const int tx = curX + s;
    const int ty = topY + ((g.h - t.h) / 2) * s;
    for (int y = 0; y < t.h; ++y) {
      for (int x = 0; x < t.w; ++x) {
        if (t.rows[y] & (1 << (t.w - 1 - x))) dot(tx + x * s, ty + y * s);
      }
    }
  }
}

template <typename F>
void forEachRubyPixel(uint32_t code, int em, int lineHeight, F&& plot) {
  if (code == 0) return;
  int symbols[3];
  int count = 0;
  for (int k = 0; k < 3; ++k) {
    const int sym = zhuyinSymbol(code, k);
    if (sym >= 1 && sym <= ZHUYIN_GLYPH_COUNT) symbols[count++] = sym - 1;
  }
  if (count == 0) return;
  const RubyStyle st = rubyStyle(em);
  const detail::GlyphSet& g = detail::GLYPH_SETS[st.set];
  const int s = st.scale;
  // Fills one design pixel, s x s screen pixels.
  auto dot = [&](int x, int y) {
    for (int dy = 0; dy < s; ++dy) {
      for (int dx = 0; dx < s; ++dx) plot(x + dx, y + dy);
    }
  };
  // DESIGN 7.2: symbols at a pitch of h + 1 in a column centred on the
  // character, starting 1 px into the block; tone column to its right.
  const int columnX = s;
  const int toneX = (1 + g.w + 1) * s;
  const int pitch = (g.h + 1) * s;
  const int top = (lineHeight - detail::columnHeight(st, count)) / 2;
  for (int k = 0; k < count; ++k) {
    const uint16_t* rows = g.rows + symbols[k] * g.h;
    for (int y = 0; y < g.h; ++y) {
      for (int x = 0; x < g.w; ++x) {
        if (rows[y] & (1 << (g.w - 1 - x))) dot(columnX + x * s, top + pitch * k + y * s);
      }
    }
  }
  const int tone = zhuyinTone(code);
  if (tone < 2 || tone > 5) return;
  const detail::ToneMark& t = g.tones[tone - 2];
  int tx, ty;
  if (tone == 5) {
    // Neutral tone sits above the first symbol, centred on the column.
    tx = columnX + (g.w - t.w) / 2 * s;
    ty = top - (t.h + 1) * s;
  } else {
    // Beside the last symbol, raised a little when the column has 2-3 symbols.
    tx = toneX;
    ty = top + pitch * (count - 1) + ((g.h - t.h) / 2 - (count >= 2 ? g.lift : 0)) * s;
  }
  for (int y = 0; y < t.h; ++y) {
    for (int x = 0; x < t.w; ++x) {
      if (t.rows[y] & (1 << (t.w - 1 - x))) dot(tx + x * s, ty + y * s);
    }
  }
}

}  // namespace zh
