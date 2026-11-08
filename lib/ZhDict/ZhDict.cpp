#include "ZhDict.h"

#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

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
  return f.seekSet(off) && f.read(buf, len) == static_cast<int>(len);
}

bool Dict::open(const char* dir) {
  if (opened) return true;
  char path[48];
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
  if (!pageIndex || !groupBuf || !scratchBuf) {
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
    for (uint32_t j = 0; j < n; ++j) pageIndex[k + j] = static_cast<uint16_t>(std::min<uint32_t>(le32(groupBuf.get() + 4 * j), 0xFFFF));
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
  for (auto& c : cache) c.valid = 0;
  for (auto& c : pairCache) c.valid = 0;
  statsData = Stats{};
  opened = true;
  LOG_INF("ZH", "dictionary open: %u characters; RAM %u B index + %u B slices + %u B buffer + %u B scratch + %u B caches",
          static_cast<unsigned>(charCount), static_cast<unsigned>(pageCount * 2), static_cast<unsigned>(sliceCount * 4),
          static_cast<unsigned>(bufBytes), static_cast<unsigned>(sizeof(SegmentScratch)),
          static_cast<unsigned>(sizeof(cache) + sizeof(pairCache)));
  return true;
}

void Dict::close() {
  charsFile.close();
  pairsFile.close();
  wordsFile.close();
  entriesFile.close();
  pageIndex.reset();
  slices.reset();
  sliceCount = 0;
  groupBuf.reset();
  scratchBuf.reset();
  opened = false;
}

bool Dict::findChar(uint32_t cp, CharRecord& out) {
  CharRecord& slot = cache[cp % CACHE_SIZE];
  if (slot.valid && slot.cp == cp) {
    out = slot;
    return slot.valid == 2;
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
      rec.dirOff = le32(r + 4);
      rec.dirCount = static_cast<uint16_t>(r[8] | (r[9] << 8));
      rec.hsk = r[10];
      rec.selfHsk = r[11];
      rec.selfEntry = le32(r + 12);
      rec.selfRank = le24(r + 16);
      rec.selfFlags = r[19];
      rec.selfZhuyin = le24(r + 20);
      rec.sliceOff = le32(r + 24);
      for (int s = 0; s < INLINE_SLICES; ++s) rec.inlineSlices[s] = le24(r + 28 + 3 * s);
      found = true;
      break;
    }
    if (cp < 0xFFFF || (n > 0 && le32(buf + (n - 1) * CHAR_RECORD_BYTES) >= cp)) break;
  }
  rec.valid = found ? 2 : 1;  // 1 = cached miss
  slot = rec;
  out = rec;
  return found;
}

bool Dict::findPair(const CharRecord& first, uint32_t cp2, PairRecord& out) {
  PairRecord& slot = pairCache[(first.cp * 31 + cp2) % PAIR_CACHE_SIZE];
  if (slot.valid && slot.cp1 == first.cp && slot.cp2 == cp2) {
    out = slot;
    return true;
  }
  PairRecord rec;
  rec.cp1 = first.cp;
  rec.cp2 = cp2;
  rec.valid = 1;
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
    if (!starts) return false;
    while (sliceIndex + 1 < sliceTotal && starts[sliceIndex + 1] <= cp2) ++sliceIndex;
  }
  const uint32_t entries = std::min<uint32_t>(SLICE_ENTRIES, first.dirCount - sliceIndex * SLICE_ENTRIES);
  uint8_t* buf = groupBuf.get();
  if (!readAt(pairsFile, first.dirOff + sliceIndex * SECTOR, buf, entries * PAIR_RECORD_BYTES)) return false;
  for (uint32_t k = 0; k < entries; ++k) {
    const uint8_t* e = buf + k * PAIR_RECORD_BYTES;
    const uint32_t v = le32(e);
    if ((v & 0xFFFFFF) != cp2) continue;
    rec.count = static_cast<uint8_t>(v >> 24);
    rec.groupOff = le32(e + 4);
    rec.groupBytes = static_cast<uint16_t>(e[8] | (e[9] << 8));
    break;
  }
  slot = rec;
  out = rec;
  return true;
}

int Dict::matchesAt(const uint32_t* cps, size_t n, size_t i, WordMatch* out, int maxOut) {
  if (!opened || i >= n || maxOut <= 0) return 0;
  ++statsData.lookups;
  CharRecord rec;
  if (!findChar(cps[i], rec)) return 0;
  int count = 0;

  auto parseRecord = [&](const uint8_t*& p, const uint8_t* end, size_t start) -> bool {
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
  PairRecord pair;
  if (i + 1 < n && findPair(rec, cps[i + 1], pair) && pair.count > 0 && pair.groupBytes <= maxGroupBytes &&
      readAt(wordsFile, pair.groupOff, groupBuf.get(), pair.groupBytes)) {
    const uint8_t* p = groupBuf.get();
    const uint8_t* end = p + pair.groupBytes;
    for (uint32_t k = 0; k < pair.count && p < end; ++k) parseRecord(p, end, i);
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
}  // namespace

bool openSharedDict() {
  Dict& d = sharedDict();
  if (d.isOpen()) return true;
  if (sharedOpenFailed) return false;
  sharedOpenFailed = !d.open("/zh");
  return !sharedOpenFailed;
}

void resetSharedDict() {
  sharedDict().close();
  sharedOpenFailed = false;
}

uint32_t rubyThreshold() { return currentRubyThreshold; }
void setRubyThreshold(uint32_t threshold) { currentRubyThreshold = threshold; }

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

}  // namespace zh
