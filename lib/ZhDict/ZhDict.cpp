#include "ZhDict.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ZhFlashTables.h"

namespace zh {

namespace {

constexpr uint32_t NO_OFFSET = 0xFFFFFFFF;
// Layout constants, see scripts/zh/build_zh_dict.py.
constexpr size_t SECTOR = 512;
constexpr size_t CHAR_RECORD_BYTES = 40;
constexpr size_t CHARS_PER_PAGE = SECTOR / CHAR_RECORD_BYTES;  // 12
constexpr size_t PAIR_RECORD_BYTES = 16;
constexpr size_t SLICE_ENTRIES = SECTOR / PAIR_RECORD_BYTES;  // 32
constexpr int INLINE_SLICES = 4;
constexpr size_t HEADER_BYTES = 16;

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint32_t le24(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16);
}

uint32_t fnv1aPageIndex(const uint16_t* idx, uint32_t count) {
  uint32_t h = 2166136261u;
  for (uint32_t k = 0; k < count; ++k) {
    const uint16_t v = idx[k];
    h = (h ^ static_cast<uint8_t>(v & 0xFFu)) * 16777619u;
    h = (h ^ static_cast<uint8_t>(v >> 8)) * 16777619u;
  }
  return h;
}

uint32_t fnv1aSlices(const uint32_t* s, uint32_t count) {
  uint32_t h = 2166136261u;
  for (uint32_t k = 0; k < count; ++k) {
    const uint32_t v = s[k];
    for (int b = 0; b < 4; ++b) {
      h = (h ^ static_cast<uint8_t>((v >> (8 * b)) & 0xFFu)) * 16777619u;
    }
  }
  return h;
}

int findTopPairIndex(uint32_t cp1, uint32_t cp2) {
  if (cp1 > 0xFFFFu || cp2 > 0xFFFFu) return -1;
  const uint32_t key = (cp1 << 16) | cp2;
  const uint32_t* begin = flash::TOP_PAIR_KEYS;
  const uint32_t* end = begin + flash::TOP_PAIR_COUNT;
  const uint32_t* it = std::lower_bound(begin, end, key);
  if (it == end || *it != key) return -1;
  return static_cast<int>(it - begin);
}

// Decodes one UTF-8 codepoint; advances p. Returns 0 on malformed input.
uint32_t nextCp(const uint8_t*& p, const uint8_t* end) {
  if (p >= end) return 0;
  const uint8_t b = *p++;
  if (b < 0x80) return b;
  int extra = (b >= 0xF0) ? 3 : (b >= 0xE0) ? 2 : (b >= 0xC0) ? 1 : -1;
  if (extra < 0 || end - p < extra) return 0;
  uint32_t cp = b & (0x3F >> extra);
  while (extra-- > 0) cp = (cp << 6) | (*p++ & 0x3F);
  return cp;
}

}  // namespace

bool isHan(uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0x20000 && cp <= 0x2FFFF) || cp == 0x3007;
}

bool Dict::readAt(HalFile& f, uint32_t off, void* buf, size_t len) {
  ++statsData.reads;
  statsData.bytes += len;
  return f.seekSet(off) && static_cast<int>(f.read(buf, len)) == static_cast<int>(len);
}

void Dict::decodeCharPayload(uint32_t cp, const uint8_t* p, CharRecord& rec) {
  rec.cp = cp;
  rec.dirOff = le32(p);
  rec.dirCount = static_cast<uint16_t>(p[4] | (p[5] << 8));
  rec.hsk = p[6];
  rec.selfHsk = p[7];
  rec.selfEntry = le32(p + 8);
  rec.selfRank = le24(p + 12);
  rec.selfFlags = p[15];
  rec.selfZhuyin = le24(p + 16);
  rec.sliceOff = le32(p + 20);
  for (int s = 0; s < INLINE_SLICES; ++s) rec.inlineSlices[s] = le24(p + 24 + 3 * s);
}

bool Dict::open(const char* dir) {
  if (opened) return true;
  char path[160];
  const char* names[] = {"chars.bin", "pairs.bin", "words.bin", "entries.bin"};
  HalFile* files[] = {&charsFile, &pairsFile, &wordsFile, &entriesFile};
  for (int k = 0; k < 4; ++k) {
    snprintf(path, sizeof(path), "%s/%s", dir, names[k]);
    if (!Storage.exists(path) || !Storage.openFileForRead("ZH", path, *files[k])) {
      close();
      return false;
    }
  }
  uint8_t header[HEADER_BYTES];
  if (!readAt(charsFile, 0, header, sizeof(header)) || memcmp(header, "ZHC2", 4) != 0) {
    LOG_ERR("ZH", "chars.bin: bad header (rebuild with scripts/zh/build_zh_dict.py)");
    close();
    return false;
  }
  charCount = le32(header + 4);
  maxGroupBytes = le32(header + 8);
  pageCount = le32(header + 12);
  // One buffer serves word groups, character pages and directory slices.
  const size_t bufBytes = std::max<size_t>(maxGroupBytes, SECTOR);
  pageIndex = makeUniqueNoThrow<uint16_t[]>(pageCount);
  groupBuf = makeUniqueNoThrow<uint8_t[]>(bufBytes);
  scratchBuf = makeUniqueNoThrow<SegmentScratch>();
  caches = makeUniqueNoThrow<Caches>();
  if (!pageIndex || !groupBuf || !scratchBuf || !caches) {
    LOG_ERR("ZH", "OOM opening dictionary");
    close();
    return false;
  }
  // Page index: first codepoint of each page, clamped to 16 bits (pages past
  // U+FFFF are searched linearly; such characters are rare in running text).
  for (uint32_t k = 0; k < pageCount;) {
    const uint32_t n = std::min<uint32_t>(SECTOR / 4, pageCount - k);
    if (!readAt(charsFile, HEADER_BYTES + k * 4, groupBuf.get(), n * 4)) {
      close();
      return false;
    }
    for (uint32_t j = 0; j < n; ++j)
      pageIndex[k + j] = static_cast<uint16_t>(std::min<uint32_t>(le32(groupBuf.get() + 4 * j), 0xFFFF));
    k += n;
  }
  recordBase = static_cast<uint32_t>((HEADER_BYTES + pageCount * 4 + SECTOR - 1) / SECTOR * SECTOR);
  // Slice starts of the longest pair directories (~2.7 KB), kept in RAM.
  snprintf(path, sizeof(path), "%s/slices.bin", dir);
  HalFile slicesFile;
  if (Storage.exists(path) && Storage.openFileForRead("ZH", path, slicesFile)) {
    sliceCount = static_cast<uint32_t>(slicesFile.fileSize() / 4);
    slices = makeUniqueNoThrow<uint32_t[]>(std::max<uint32_t>(sliceCount, 1));
    if (!slices || !readAt(slicesFile, 0, slices.get(), sliceCount * 4)) {
      LOG_ERR("ZH", "slices.bin unreadable");
      close();
      return false;
    }
    slicesFile.close();
  }
  flashTablesMatch = charCount == flash::DICT_CHAR_COUNT && maxGroupBytes == flash::DICT_MAX_GROUP_BYTES &&
                     pageCount == flash::DICT_PAGE_COUNT && sliceCount == flash::DICT_SLICE_COUNT &&
                     static_cast<uint32_t>(pairsFile.fileSize()) == flash::DICT_PAIRS_BYTES &&
                     static_cast<uint32_t>(wordsFile.fileSize()) == flash::DICT_WORDS_BYTES &&
                     static_cast<uint32_t>(entriesFile.fileSize()) == flash::DICT_ENTRIES_BYTES &&
                     fnv1aPageIndex(pageIndex.get(), pageCount) == flash::DICT_PAGE_INDEX_FNV1A &&
                     fnv1aSlices(slices.get(), sliceCount) == flash::DICT_SLICES_FNV1A;

  statsData = Stats{};
  opened = true;
  LOG_INF("ZH",
          "dictionary open: %u characters (flash=%d); RAM %u B index + %u B slices + %u B buffer + %u B scratch + %u "
          "B caches",
          static_cast<unsigned>(charCount), flashTablesMatch ? 1 : 0, static_cast<unsigned>(pageCount * 2),
          static_cast<unsigned>(sliceCount * 4), static_cast<unsigned>(bufBytes),
          static_cast<unsigned>(sizeof(SegmentScratch)), static_cast<unsigned>(sizeof(Caches)));
  return true;
}

void Dict::close() {
  if (charsFile.isOpen()) charsFile.close();
  if (pairsFile.isOpen()) pairsFile.close();
  if (wordsFile.isOpen()) wordsFile.close();
  if (entriesFile.isOpen()) entriesFile.close();
  pageIndex.reset();
  slices.reset();
  sliceCount = 0;
  groupBuf.reset();
  scratchBuf.reset();
  caches.reset();
  flashTablesMatch = false;
  opened = false;
}

bool Dict::findChar(uint32_t cp, CharRecord& out) {
  if (flashTablesMatch && cp <= 0xFFFFu) {
    const uint16_t* begin = flash::TOP_CHAR_CPS;
    const uint16_t* end = begin + flash::TOP_CHAR_COUNT;
    const uint16_t* it = std::lower_bound(begin, end, static_cast<uint16_t>(cp));
    if (it != end && *it == static_cast<uint16_t>(cp)) {
      decodeCharPayload(cp, flash::TOP_CHAR_PAYLOADS[it - begin], out);
      out.valid = 2;
      return true;
    }
  }
  const size_t set = cp % CHAR_SETS;
  for (int w = 0; w < CACHE_WAYS; ++w) {
    CharRecord& slot = caches->chars[set][w];
    if (slot.valid && slot.cp == cp) {
      caches->charNextWay[set] = static_cast<uint8_t>(1 - w);
      out = slot;
      return slot.valid == 2;
    }
  }
  CharRecord rec;
  rec.cp = cp;
  bool found = false;
  // Last page whose first codepoint <= cp; beyond U+FFFF, every clamped page is a candidate.
  const uint16_t key = static_cast<uint16_t>(std::min<uint32_t>(cp, 0xFFFF));
  uint32_t lo = 0, hi = pageCount;
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2;
    if (pageIndex[mid] <= key)
      lo = mid + 1;
    else
      hi = mid;
  }
  uint32_t page = lo > 0 ? lo - 1 : 0;
  if (cp >= 0xFFFF) {
    while (page > 0 && pageIndex[page] == 0xFFFF && pageIndex[page - 1] == 0xFFFF) --page;
  }
  for (; page < pageCount && !found; ++page) {
    uint8_t* buf = groupBuf.get();
    if (!readAt(charsFile, recordBase + page * SECTOR, buf, CHARS_PER_PAGE * CHAR_RECORD_BYTES)) break;
    const uint32_t first = page * CHARS_PER_PAGE;
    const uint32_t n = std::min<uint32_t>(CHARS_PER_PAGE, charCount - first);
    for (uint32_t k = 0; k < n; ++k) {
      const uint8_t* r = buf + k * CHAR_RECORD_BYTES;
      const uint32_t c = le32(r);
      if (c > cp) break;
      if (c != cp) continue;
      decodeCharPayload(cp, r + 4, rec);
      found = true;
      break;
    }
    if (cp < 0xFFFF || (n > 0 && le32(buf + (n - 1) * CHAR_RECORD_BYTES) >= cp)) break;
  }
  rec.valid = found ? 2 : 1;  // 1 = cached miss
  const int w = caches->charNextWay[set];
  caches->charNextWay[set] = static_cast<uint8_t>(1 - w);
  caches->chars[set][w] = rec;
  out = rec;
  return found;
}

Dict::PairRecord* Dict::findPair(const CharRecord& first, uint32_t cp2) {
  const size_t set = (first.cp * 31u + cp2) % PAIR_SETS;
  for (int w = 0; w < CACHE_WAYS; ++w) {
    PairRecord& slot = caches->pairs[set][w];
    if (slot.valid && slot.cp1 == first.cp && slot.cp2 == cp2) {
      caches->pairNextWay[set] = static_cast<uint8_t>(1 - w);
      return &slot;
    }
  }
  PairRecord rec;
  rec.cp1 = first.cp;
  rec.cp2 = cp2;
  rec.valid = 1;
  if (first.dirCount > 0) {
    // Directories over 32 entries are split into sector-sized slices; pick the
    // slice from the RAM-resident slice starts, then read exactly one sector.
    uint32_t sliceIndex = 0;
    const uint32_t sliceTotal = (first.dirCount + SLICE_ENTRIES - 1) / SLICE_ENTRIES;
    if (sliceTotal > 1) {
      const uint32_t* starts = nullptr;
      uint32_t inlineStarts[INLINE_SLICES];
      if (sliceTotal <= INLINE_SLICES) {
        for (int s = 0; s < INLINE_SLICES; ++s) inlineStarts[s] = first.inlineSlices[s];
        starts = inlineStarts;
      } else if (slices && first.sliceOff / 4 + sliceTotal <= sliceCount) {
        starts = slices.get() + first.sliceOff / 4;
      }
      if (!starts) return nullptr;
      while (sliceIndex + 1 < sliceTotal && starts[sliceIndex + 1] <= cp2) ++sliceIndex;
    }
    const uint32_t entries = std::min<uint32_t>(SLICE_ENTRIES, first.dirCount - sliceIndex * SLICE_ENTRIES);
    uint8_t* buf = groupBuf.get();
    if (!readAt(pairsFile, first.dirOff + sliceIndex * SECTOR, buf, entries * PAIR_RECORD_BYTES)) return nullptr;
    for (uint32_t k = 0; k < entries; ++k) {
      const uint8_t* e = buf + k * PAIR_RECORD_BYTES;
      const uint32_t v = le32(e);
      if ((v & 0xFFFFFF) != cp2) continue;
      rec.count = static_cast<uint8_t>(v >> 24);
      rec.groupOff = le32(e + 4);
      rec.groupBytes = static_cast<uint16_t>(e[8] | (e[9] << 8));
      break;
    }
  }
  const int w = caches->pairNextWay[set];
  caches->pairNextWay[set] = static_cast<uint8_t>(1 - w);
  caches->pairs[set][w] = rec;
  return &caches->pairs[set][w];
}

int Dict::matchesAt(const uint32_t* cps, size_t n, size_t i, WordMatch* out, int maxOut) {
  if (!opened || i >= n || maxOut <= 0) return 0;
  ++statsData.lookups;
  CharRecord rec;
  if (!findChar(cps[i], rec)) return 0;
  int count = 0;

  auto parseRecord = [&](const uint8_t*& p, const uint8_t* end, size_t start, PairRecord* memo) -> bool {
    // Returns true if the record matches cps from start and was appended.
    if (end - p < 2) {
      p = end;
      return false;
    }
    const uint8_t nChars = p[0];
    const uint8_t restLen = p[1];
    const uint8_t* rest = p + 2;
    const uint8_t* fields = rest + restLen;
    const uint8_t* recEnd = fields + 9 + 3 * nChars;
    if (recEnd > end || nChars == 0 || nChars > MAX_WORD_CHARS) {
      p = end;
      return false;
    }
    p = recEnd;
    if (memo) {
      if (nChars == 2) {
        const uint32_t wrank = le24(fields + 6);
        if (!memo->hasTwoChar || wrank < memo->twoRank) {
          memo->hasTwoChar = true;
          memo->twoEntryOff = le32(fields);
          memo->twoHsk = fields[4];
          memo->twoFlags = fields[5];
          memo->twoRank = wrank;
          memo->twoZhuyin0 = le24(fields + 9);
          memo->twoZhuyin1 = le24(fields + 12);
        }
      } else if (nChars >= 3) {
        memo->hasLonger = true;
      }
    }
    if (start + nChars > n) return false;
    const uint8_t* q = rest;
    for (size_t k = 2; k < nChars; ++k) {
      if (nextCp(q, fields) != cps[start + k]) return false;
    }
    if (count >= maxOut) return false;
    WordMatch& m = out[count++];
    m.len = nChars;
    m.entryOff = le32(fields);
    m.hsk = fields[4];
    m.flags = fields[5];
    m.rank = le24(fields + 6);
    for (int k = 0; k < MAX_WORD_CHARS; ++k) m.zhuyin[k] = k < nChars ? le24(fields + 9 + 3 * k) : 0;
    return true;
  };

  // Words of two or more characters: their group is found by the second character.
  if (i + 1 < n && rec.dirCount > 0) {
    const uint32_t cp1 = cps[i];
    const uint32_t cp2 = cps[i + 1];
    if (!flashTablesMatch || flash::pairBloomMayContain(cp1, cp2)) {
      const int topIdx = flashTablesMatch ? findTopPairIndex(cp1, cp2) : -1;
      if (topIdx >= 0) {
        const uint8_t* p22 = flash::TOP_PAIR_PAYLOADS[topIdx];
        const uint32_t groupOff = le32(p22);
        const uint16_t groupBytes = static_cast<uint16_t>(p22[4] | (p22[5] << 8));
        const uint8_t groupCount = p22[6];
        const uint8_t meta = p22[7];
        const bool hasTwo = (meta & 1u) != 0;
        const bool hasLonger = (meta & 2u) != 0;
        const bool checkLonger = hasLonger && (i + 2 < n) && flash::tripleBloomMayContain(cp1, cp2, cps[i + 2]);
        if (!checkLonger) {
          if (hasTwo && count < maxOut) {
            WordMatch& m = out[count++];
            m = WordMatch{};
            m.len = 2;
            m.entryOff = le32(p22 + 8);
            m.hsk = static_cast<uint8_t>((meta >> 4) & 0x0Fu);
            m.flags = static_cast<uint8_t>((meta >> 2) & 0x03u);
            m.rank = le24(p22 + 12);
            m.zhuyin[0] = le24(p22 + 15);
            m.zhuyin[1] = le24(p22 + 18);
          }
        } else if (groupBytes <= maxGroupBytes && readAt(wordsFile, groupOff, groupBuf.get(), groupBytes)) {
          const uint8_t* p = groupBuf.get();
          const uint8_t* end = p + groupBytes;
          for (uint32_t k = 0; k < groupCount && p < end; ++k) parseRecord(p, end, i, nullptr);
        }
      } else {
        PairRecord* pair = findPair(rec, cp2);
        if (pair && pair->count > 0 && pair->groupBytes <= maxGroupBytes) {
          if (pair->valid == 1) {
            if (readAt(wordsFile, pair->groupOff, groupBuf.get(), pair->groupBytes)) {
              const uint8_t* p = groupBuf.get();
              const uint8_t* end = p + pair->groupBytes;
              for (uint32_t k = 0; k < pair->count && p < end; ++k) parseRecord(p, end, i, pair);
              pair->valid = 2;
            }
          } else {
            const bool checkLonger = pair->hasLonger && (i + 2 < n) &&
                                     (!flashTablesMatch || flash::tripleBloomMayContain(cp1, cp2, cps[i + 2]));
            if (!checkLonger) {
              if (pair->hasTwoChar && count < maxOut) {
                WordMatch& m = out[count++];
                m = WordMatch{};
                m.len = 2;
                m.entryOff = pair->twoEntryOff;
                m.hsk = pair->twoHsk;
                m.flags = pair->twoFlags;
                m.rank = pair->twoRank;
                m.zhuyin[0] = pair->twoZhuyin0;
                m.zhuyin[1] = pair->twoZhuyin1;
              }
            } else if (readAt(wordsFile, pair->groupOff, groupBuf.get(), pair->groupBytes)) {
              const uint8_t* p = groupBuf.get();
              const uint8_t* end = p + pair->groupBytes;
              for (uint32_t k = 0; k < pair->count && p < end; ++k) parseRecord(p, end, i, nullptr);
            }
          }
        }
      }
    }
  }
  if (rec.selfEntry != NO_OFFSET && count < maxOut) {
    WordMatch& m = out[count++];
    m = WordMatch{};
    m.len = 1;
    m.entryOff = rec.selfEntry;
    m.hsk = rec.selfHsk;
    m.flags = rec.selfFlags;
    m.rank = rec.selfRank;
    m.zhuyin[0] = rec.selfZhuyin;
  }
  // Longest first (insertion sort; count <= MAX_MATCHES).
  for (int a = 1; a < count; ++a) {
    for (int b = a; b > 0 && out[b].len > out[b - 1].len; --b) std::swap(out[b], out[b - 1]);
  }
  return count;
}

uint8_t Dict::charHsk(uint32_t cp) {
  CharRecord rec;
  return findChar(cp, rec) ? rec.hsk : 0;
}

size_t Dict::readEntry(uint32_t off, uint8_t* buf, size_t bufSize) {
  if (!opened || bufSize < 2 || !entriesFile.seekSet(off)) return 0;
  const int got = entriesFile.read(buf, bufSize - 1);
  if (got <= 0) return 0;
  buf[got] = 0;
  return static_cast<size_t>(got);
}

Dict& sharedDict() {
  static Dict dict;
  return dict;
}

namespace {
bool sharedOpenFailed = false;
uint32_t currentRubyThreshold = 0;
bool currentRubyUnderLine = false;
}  // namespace

bool hasSharedDictFiles(const char* dir) {
  if (sharedDict().isOpen()) return true;
  if (sharedOpenFailed) return false;
  char path[160];
  const char* names[] = {"chars.bin", "pairs.bin", "words.bin", "entries.bin"};
  for (const char* name : names) {
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (!Storage.exists(path)) {
      sharedOpenFailed = true;
      return false;
    }
  }
  return true;
}

bool openSharedDict() {
  Dict& d = sharedDict();
  if (d.isOpen()) return true;
  if (!hasSharedDictFiles("/zh")) return false;
  if (!d.open("/zh")) {
    // Only latch permanent failure if files vanished; transient OOM can succeed
    // once the caller releases font caches or tears down the BLE radio.
    char checkPath[64];
    snprintf(checkPath, sizeof(checkPath), "/zh/chars.bin");
    if (!Storage.exists(checkPath)) sharedOpenFailed = true;
    return false;
  }
  return true;
}

void resetSharedDict() {
  sharedDict().close();
  sharedOpenFailed = false;
}

uint32_t rubyThreshold() { return currentRubyThreshold; }
void setRubyThreshold(uint32_t threshold) { currentRubyThreshold = threshold; }
bool rubyUnderLine() { return currentRubyUnderLine; }
void setRubyUnderLine(bool under) { currentRubyUnderLine = under; }

bool isRubyCode(const char* s) { return s && s[0] == RUBY_MARK; }

uint32_t decodeRubyCode(const char* s) {
  if (!isRubyCode(s)) return 0;
  uint32_t v = 0;
  for (int k = 1; k <= 6 && s[k]; ++k) {
    const char c = s[k];
    v = (v << 4) | static_cast<uint32_t>(c <= '9' ? c - '0' : c - 'a' + 10);
  }
  return v;
}

void encodeRubyCode(uint32_t code, char out[8]) { snprintf(out, 8, "%c%06x", RUBY_MARK, code & 0xFFFFFF); }

EntryReader::EntryReader(const uint8_t* record, size_t len) : p(record), end(record + len) {
  if (len > 0) readings = *p++;
}

bool EntryReader::next(EntryReading& out) {
  if (done >= readings) return false;
  out = EntryReading{};
  const auto takeString = [&](const char*& text, uint8_t& textLen) {
    if (p >= end || *p > end - p - 1) return false;
    textLen = *p;
    text = reinterpret_cast<const char*>(p + 1);
    p += 1 + textLen;
    return true;
  };
  if (!takeString(out.pinyin, out.pinyinLen) || !takeString(out.trad, out.tradLen) || p >= end) {
    done = readings;
    return false;
  }
  const int syllables = *p++;
  for (int s = 0; s < syllables && end - p >= 3; ++s, p += 3) {
    if (s < MAX_WORD_CHARS) out.zhuyin[out.syllables++] = le24(p);
  }
  if (end - p < 2) {
    done = readings;
    return false;
  }
  // A sense list cut by the read buffer is kept up to the cut.
  out.sensesLen = static_cast<uint16_t>(std::min<size_t>(p[0] | (p[1] << 8), end - p - 2));
  out.senses = reinterpret_cast<const char*>(p + 2);
  p += 2 + out.sensesLen;
  ++done;
  return true;
}

void appendZhuyin(std::string& out, uint32_t code) {
  const auto append = [&](uint32_t cp) {
    if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
    } else {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    }
    out += static_cast<char>(0x80 | (cp & 0x3F));
  };
  // Tones 2-4 follow the syllable; the neutral tone dot comes first.
  static constexpr uint32_t TONE_MARKS[] = {0, 0, 0x02CA, 0x02C7, 0x02CB, 0x02D9};
  const int tone = zhuyinTone(code);
  if (tone == 5) append(TONE_MARKS[5]);
  for (int i = 0; i < 3; ++i) {
    const int symbol = zhuyinSymbol(code, i);
    if (symbol) append(0x3104 + static_cast<uint32_t>(symbol));
  }
  if (tone >= 2 && tone <= 4) append(TONE_MARKS[tone]);
}

}  // namespace zh
