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

// Working memory for segmentRun, allocated once when the dictionary opens.
struct SegmentScratch {
  WordMatch matches[MAX_MATCHES];
  float best[MAX_RUN + 1];
  Segment seg[MAX_RUN + 1];
  uint8_t path[MAX_RUN + 1];
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
  CharRecord cache[CACHE_SIZE];
  PairRecord pairCache[PAIR_CACHE_SIZE];
  Stats statsData;
};

// One dictionary instance shared by layout and the lookup screen. Opened on
// first use; close it when leaving a Chinese book to release its RAM.
Dict& sharedDict();
// Opens sharedDict() from /zh. After a failure it does not retry until
// resetSharedDict() (the files are missing or unreadable).
bool openSharedDict();
void resetSharedDict();

// Ruby frequency threshold used by layout, set from the reader settings before
// a section is laid out.
uint32_t rubyThreshold();
void setRubyThreshold(uint32_t threshold);

// Segments cps[0..n) (n <= MAX_RUN, all Han). Calls onSegment for each word in
// order. Returns false if the dictionary is not open.
template <typename F>
bool segmentRun(Dict& dict, const uint32_t* cps, size_t n, F&& onSegment);

// Ruby setting: words whose rank is above the threshold get ruby.
// 0 = ruby off, RUBY_ALL = every character with a reading.
constexpr uint32_t RUBY_ALL = 0xFFFFFFFF;
// Ruby block to the right of a character (DESIGN 7.2): gap, 9 px column, gap, tone.
constexpr int RUBY_WIDTH = 16;

// Reader setting steps: off, all, then "words rarer than the top N".
constexpr uint32_t RUBY_STEPS[] = {0, RUBY_ALL, 500, 1000, 2000, 3000, 5000, 8000, 12000, 20000, 35000, 60000};
constexpr uint8_t RUBY_STEP_COUNT = sizeof(RUBY_STEPS) / sizeof(RUBY_STEPS[0]);
inline uint32_t rubyThresholdForStep(uint8_t step) { return step < RUBY_STEP_COUNT ? RUBY_STEPS[step] : 0; }

inline bool wantsRuby(const Segment& seg, uint32_t threshold) {
  if (threshold == 0 || !seg.inDict) return false;
  return threshold == RUBY_ALL || seg.rank > threshold;
}

// Calls plot(x, y) for each black pixel of the ruby for `code`, relative to the
// top-left of a ruby block beside a character of height charHeight.
template <typename F>
void forEachRubyPixel(uint32_t code, int charHeight, F&& plot);

// Ruby strings stored per word in the page cache: "\x1B" + 6 hex digits.
constexpr char RUBY_MARK = '\x1B';
bool isRubyCode(const char* s);
uint32_t decodeRubyCode(const char* s);
void encodeRubyCode(uint32_t code, char out[8]);

}  // namespace zh

#include "ZhDictImpl.h"
