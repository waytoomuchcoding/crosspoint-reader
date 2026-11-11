#pragma once

// Chinese reading mode: dictionary lookup on the SD card, word segmentation,
// and zhuyin ruby selection. The files are built by scripts/zh/build_zh_dict.py;
// see its docstring for the format.
//
// RAM while open (~19 KB): a 16-bit page index (~2.4 KB), slice starts of the
// longest directories (~2.7 KB), a buffer for one word group (~4 KB),
// segmentation scratch (~3 KB) and character/pair caches (~7 KB). Open it only
// while laying out or looking up; close() releases everything.

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace zh {

constexpr int MAX_WORD_CHARS = 8;
constexpr uint32_t NO_RANK = 0xFFFFFF;
constexpr int MAX_MATCHES = 8;

// Packed zhuyin: three 6-bit symbols (codepoint - 0x3104, 0 = unused) then a
// 3-bit tone (1-4, 5 = neutral). 0 = no reading.
inline int zhuyinSymbol(uint32_t code, int i) { return static_cast<int>((code >> (6 * i)) & 63); }
inline int zhuyinTone(uint32_t code) { return static_cast<int>((code >> 18) & 7); }

bool isHan(uint32_t cp);

struct WordMatch {
  uint8_t len = 0;  // characters
  uint8_t hsk = 0;  // HSK 3.0 band 1-7, 0 = not in HSK
  uint8_t flags = 0;
  uint32_t rank = NO_RANK;  // word-frequency rank, 1 = most common
  uint32_t entryOff = 0;    // into entries.bin
  uint32_t zhuyin[MAX_WORD_CHARS] = {};
};

// ---------------------------------------------------------------------------
// Segmentation: unigram model, cost = WORD_COST + ln(rank). Runs over one
// stretch of consecutive Han characters at a time.

struct Segment {
  uint16_t start;
  uint8_t len;
  uint8_t hsk;
  uint32_t rank;  // NO_RANK when unknown or not a dictionary word
  uint32_t entryOff;
  uint32_t zhuyin[MAX_WORD_CHARS];
  bool inDict;
};

constexpr size_t MAX_RUN = 64;

// Working memory for segmentRun and paragraph ruby tagging, allocated once when
// the dictionary opens so ESP32-C3 layout stacks stay small.
struct SegmentScratch {
  WordMatch matches[MAX_MATCHES];
  float best[MAX_RUN + 1];
  Segment seg[MAX_RUN + 1];
  uint8_t path[MAX_RUN + 1];
  uint32_t selfRank[MAX_RUN];
  uint32_t selfZhuyin[MAX_RUN];
  uint32_t runCps[MAX_RUN];
  uint16_t runWord[MAX_RUN];
};

class Dict {
 public:
  Dict() = default;
  Dict(const Dict&) = delete;
  Dict& operator=(const Dict&) = delete;

  bool open(const char* dir = "/zh");
  void close();
  bool isOpen() const { return opened; }

  // Dictionary words starting at cps[i], restricted to cps[i..n), longest first.
  int matchesAt(const uint32_t* cps, size_t n, size_t i, WordMatch* out, int maxOut);
  // Lowest HSK band of any HSK word containing cp, 0 = none.
  uint8_t charHsk(uint32_t cp);

  // Reads an entries.bin record into buf (NUL-terminated). Layout as written by
  // the converter; parse with EntryReader. Returns bytes read, 0 on failure.
  // A record longer than the buffer is cut short; EntryReader stops at the cut.
  size_t readEntry(uint32_t off, uint8_t* buf, size_t bufSize);

  SegmentScratch* scratch() { return scratchBuf.get(); }

  // SD traffic since open, for measuring layout cost.
  struct Stats {
    uint32_t reads = 0;
    uint32_t bytes = 0;
    uint32_t lookups = 0;
  };
  const Stats& stats() const { return statsData; }

 private:
  struct CharRecord {
    uint32_t cp = 0;
    uint32_t dirOff = 0;
    uint16_t dirCount = 0;
    uint8_t hsk = 0;
    uint8_t valid = 0;                // 0 = empty slot, 1 = cached miss, 2 = found
    uint32_t selfEntry = 0xFFFFFFFF;  // the character as a one-character word
    uint32_t selfRank = NO_RANK;
    uint32_t selfZhuyin = 0;
    uint8_t selfHsk = 0;
    uint8_t selfFlags = 0;
    uint32_t sliceOff = 0xFFFFFFFF;  // into slices.bin, for directories over 4 slices
    uint32_t inlineSlices[4] = {};   // slice starts for directories of 2-4 slices
  };
  // A second-character lookup in a pair directory, cached (absent pairs too).
  struct PairRecord {
    uint32_t cp1 = 0, cp2 = 0;
    uint32_t groupOff = 0;
    uint16_t groupBytes = 0;
    uint8_t count = 0;  // 0 = no word starts with this pair
    uint8_t valid = 0;
  };
  static constexpr int CACHE_SIZE = 96;
  static constexpr int PAIR_CACHE_SIZE = 128;

  bool findChar(uint32_t cp, CharRecord& out);
  bool findPair(const CharRecord& first, uint32_t cp2, PairRecord& out);
  bool readAt(HalFile& f, uint32_t off, void* buf, size_t len);

  HalFile charsFile, pairsFile, wordsFile, entriesFile;
  bool opened = false;
  uint32_t charCount = 0;
  uint32_t maxGroupBytes = 0;
  uint32_t pageCount = 0;
  uint32_t recordBase = 0;
  std::unique_ptr<uint16_t[]> pageIndex;  // first codepoint of each 512-byte record page
  std::unique_ptr<uint32_t[]> slices;     // slices.bin
  uint32_t sliceCount = 0;
  std::unique_ptr<uint8_t[]> groupBuf;
  std::unique_ptr<SegmentScratch> scratchBuf;
  // Heap-allocated in open() so a closed dictionary costs no RAM.
  struct Caches {
    CharRecord chars[CACHE_SIZE];
    PairRecord pairs[PAIR_CACHE_SIZE];
  };
  std::unique_ptr<Caches> caches;
  Stats statsData;
};

// One dictionary instance shared by layout and the lookup screen. Opened on
// first use; close it when leaving a Chinese book to release its RAM.
Dict& sharedDict();
// Opens sharedDict() from /zh. After a failure it does not retry until
// resetSharedDict() (the files are missing or unreadable).
bool openSharedDict();
void resetSharedDict();

// Ruby frequency threshold and placement used by layout, set from the reader
// settings before a section is laid out.
uint32_t rubyThreshold();
void setRubyThreshold(uint32_t threshold);
bool rubyUnderLine();
void setRubyUnderLine(bool under);

// Segments cps[0..n) (n <= MAX_RUN, all Han). Calls onSegment for each word in
// order. Returns false if the dictionary is not open.
template <typename F>
bool segmentRun(Dict& dict, const uint32_t* cps, size_t n, F&& onSegment);

// Ruby setting: words whose rank is above the threshold get ruby.
// 0 = ruby off, RUBY_ALL = every character with a reading.
constexpr uint32_t RUBY_ALL = 0x3FFFFFFF;
// Ruby block to the right of a character (DESIGN 7.2): gap, bopomofo column, gap,
// tone column. The glyph set (5x7, 9x9 or 11x11) and an integer scale follow the
// character's em (the advance of a full-width Han character, which can be less than
// the line height); see RUBY_STEPS_BY_EM in ZhDictImpl.h.
// 32 px text gets the 9x9 set at 1x, a 16 px wide block.
struct RubyStyle {
  uint8_t set;    // index into the glyph sets in ZhDictImpl.h
  uint8_t scale;  // integer pixel scale
};
RubyStyle rubyStyle(int em);
// Width of the ruby block beside a character of this em.
int rubyWidth(int em);
// Horizontal width, extra cell advance, and vertical clearance for under-character ruby.
int rubyUnderWidth(uint32_t code, int em);
int rubyUnderExtraWidth(uint32_t code, int em);
int rubyUnderHeight(int em);
// Text measured for a font's em: CJK fonts give 一 a full-width advance.
constexpr const char* EM_PROBE = "\xE4\xB8\x80";

// Reader setting steps: off, all, then "words rarer than the top N".
constexpr uint32_t RUBY_STEPS[] = {0, RUBY_ALL, 500, 1000, 2000, 3000, 5000, 8000, 12000, 20000, 35000, 60000};
constexpr uint8_t RUBY_STEP_COUNT = sizeof(RUBY_STEPS) / sizeof(RUBY_STEPS[0]);
inline uint32_t rubyThresholdForStep(uint8_t step) { return step < RUBY_STEP_COUNT ? RUBY_STEPS[step] : 0; }

inline bool wantsRuby(const Segment& seg, uint32_t threshold) {
  if (threshold == 0 || !seg.inDict) return false;
  return threshold == RUBY_ALL || seg.rank > threshold;
}

// Common grammar particles and suffixes whose secondary or neutral-tone readings
// should not trigger the polyphone exception inside compound words.
inline bool isGrammarParticle(uint32_t cp) {
  return cp == 0x4E86 ||  // 了
         cp == 0x8FC7 ||  // 过
         cp == 0x7684 ||  // 的
         cp == 0x5730 ||  // 地
         cp == 0x5F97 ||  // 得
         cp == 0x5B50 ||  // 子
         cp == 0x513F ||  // 儿
         cp == 0x4EEC;    // 们
}

// Character-level ruby filter inside an uncommon word: annotates character k if
// the character itself is rarer than threshold, or if it takes a non-neutral
// polyphone reading different from its primary reading.
inline bool wantsCharRuby(Dict& dict, const Segment& seg, uint8_t k, uint32_t cp, uint32_t threshold) {
  if (!wantsRuby(seg, threshold) || k >= seg.len || seg.zhuyin[k] == 0) return false;
  if (threshold == RUBY_ALL || seg.len == 1) return true;
  const SegmentScratch* s = dict.scratch();
  if (!s) return true;
  const size_t idx = seg.start + k;
  const uint32_t cRank = s->selfRank[idx];
  const uint32_t cZhuyin = s->selfZhuyin[idx];
  if (cRank > threshold) return true;
  const uint32_t wZhuyin = seg.zhuyin[k];
  if (!isGrammarParticle(cp) && cZhuyin != 0 && zhuyinTone(wZhuyin) != 5 && wZhuyin != cZhuyin) {
    return true;
  }
  return false;
}

// Calls plot(x, y) for each black pixel of the ruby for `code`, relative to the
// top-left of a ruby block beside a character of this em, centred on a line
// lineHeight tall.
template <typename F>
void forEachRubyPixel(uint32_t code, int em, int lineHeight, F&& plot);

// Calls plot(x, y) for each black pixel of the horizontal under-character ruby
// for `code`, centred across `cellWidth` and positioned below `lineHeight`.
template <typename F>
void forEachRubyUnderPixel(uint32_t code, int em, int cellWidth, int lineHeight, F&& plot);

// One reading of an entries.bin record; the strings point into the record buffer.
// Record: reading count, then per reading: pinyin and traditional form (length
// byte + UTF-8), syllable count + 3-byte packed zhuyin each, and the senses
// (16-bit length + UTF-8, separated by '\x1f').
struct EntryReading {
  const char* pinyin = nullptr;
  uint8_t pinyinLen = 0;
  const char* trad = nullptr;
  uint8_t tradLen = 0;
  uint32_t zhuyin[MAX_WORD_CHARS] = {};
  uint8_t syllables = 0;
  const char* senses = nullptr;
  uint16_t sensesLen = 0;
};

class EntryReader {
 public:
  EntryReader(const uint8_t* record, size_t len);
  int count() const { return readings; }
  // Fills `out` with the next reading; false at the end or a truncated record.
  bool next(EntryReading& out);

 private:
  const uint8_t* p;
  const uint8_t* end;
  int readings = 0;
  int done = 0;
};

// Appends packed zhuyin as text with its tone mark, e.g. ㄒㄩㄝˊ (neutral: ˙ㄉㄜ).
void appendZhuyin(std::string& out, uint32_t code);

// Ruby strings stored per word in the page cache: "\x1B" + 6 hex digits.
constexpr char RUBY_MARK = '\x1B';
bool isRubyCode(const char* s);
uint32_t decodeRubyCode(const char* s);
void encodeRubyCode(uint32_t code, char out[8]);

}  // namespace zh

#include "ZhDictImpl.h"
