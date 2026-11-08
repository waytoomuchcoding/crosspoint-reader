#include "ZhDict.h"

#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

namespace zh {

namespace {

constexpr uint32_t NO_OFFSET = 0xFFFFFFFF;
constexpr size_t CHAR_RECORD_BYTES = 16;
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

bool readAt(HalFile& f, uint32_t off, void* buf, size_t len) {
  return f.seekSet(off) && f.read(buf, len) == static_cast<int>(len);
}

}  // namespace

bool isHan(uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0x20000 && cp <= 0x2FFFF) || cp == 0x3007;
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
  if (!readAt(charsFile, 0, header, sizeof(header)) || memcmp(header, "ZHC1", 4) != 0) {
    LOG_ERR("ZH", "chars.bin: bad header");
    close();
    return false;
  }
  charCount = le32(header + 4);
  maxGroupBytes = le32(header + 12);
  sparseCount = (charCount + SPARSE_STEP - 1) / SPARSE_STEP;
  // One buffer serves word groups and the 32-record character chunks.
  const size_t bufBytes = std::max<size_t>(maxGroupBytes, SPARSE_STEP * CHAR_RECORD_BYTES);
  sparse = makeUniqueNoThrow<uint32_t[]>(sparseCount);
  groupBuf = makeUniqueNoThrow<uint8_t[]>(bufBytes);
  scratchBuf = makeUniqueNoThrow<SegmentScratch>();
  if (!sparse || !groupBuf || !scratchBuf || !readAt(charsFile, HEADER_BYTES, sparse.get(), sparseCount * 4)) {
    LOG_ERR("ZH", "open failed: %u chars, %u B group buffer", static_cast<unsigned>(charCount),
            static_cast<unsigned>(bufBytes));
    close();
    return false;
  }
  for (auto& c : cache) c.valid = 0;
  opened = true;
  LOG_INF("ZH", "dictionary open: %u first characters, %u B index, %u B buffer, %u B scratch",
          static_cast<unsigned>(charCount), static_cast<unsigned>(sparseCount * 4), static_cast<unsigned>(bufBytes),
          static_cast<unsigned>(sizeof(SegmentScratch)));
  return true;
}

void Dict::close() {
  charsFile.close();
  pairsFile.close();
  wordsFile.close();
  entriesFile.close();
  sparse.reset();
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
  // Last sparse entry <= cp.
  uint32_t lo = 0, hi = sparseCount;
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2;
    if (sparse[mid] <= cp)
      lo = mid + 1;
    else
      hi = mid;
  }
  bool found = false;
  CharRecord rec;
  rec.cp = cp;
  if (lo > 0) {
    const uint32_t first = (lo - 1) * SPARSE_STEP;
    const uint32_t n = std::min<uint32_t>(SPARSE_STEP, charCount - first);
    const uint32_t base = HEADER_BYTES + sparseCount * 4;
    uint8_t* buf = groupBuf.get();
    if (readAt(charsFile, base + first * CHAR_RECORD_BYTES, buf, n * CHAR_RECORD_BYTES)) {
      for (uint32_t k = 0; k < n; ++k) {
        const uint8_t* r = buf + k * CHAR_RECORD_BYTES;
        if (le32(r) == cp) {
          rec.dirOff = le32(r + 4);
          rec.dirCount = static_cast<uint16_t>(r[8] | (r[9] << 8));
          rec.hsk = r[10];
          rec.selfOff = le32(r + 12);
          found = true;
          break;
        }
      }
    }
  }
  rec.valid = found ? 2 : 1;  // 1 = cached miss
  slot = rec;
  out = rec;
  return found;
}

int Dict::matchesAt(const uint32_t* cps, size_t n, size_t i, WordMatch* out, int maxOut) {
  if (!opened || i >= n || maxOut <= 0) return 0;
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

  // Words of two or more characters: find the second character in the pair directory.
  if (i + 1 < n && rec.dirCount > 0) {
    const uint32_t want = cps[i + 1];
    uint32_t lo = 0, hi = rec.dirCount;
    uint8_t e[8];
    while (lo < hi) {
      const uint32_t mid = (lo + hi) / 2;
      if (!readAt(pairsFile, rec.dirOff + mid * 8, e, 8)) break;
      const uint32_t v = le32(e);
      const uint32_t c2 = v & 0xFFFFFF;
      if (c2 < want) {
        lo = mid + 1;
      } else if (c2 > want) {
        hi = mid;
      } else {
        const uint32_t groupCount = v >> 24;
        const uint32_t groupOff = le32(e + 4);
        const size_t avail = std::min<size_t>(maxGroupBytes, wordsFile.fileSize() - groupOff);
        if (wordsFile.seekSet(groupOff) && wordsFile.read(groupBuf.get(), avail) == static_cast<int>(avail)) {
          const uint8_t* p = groupBuf.get();
          const uint8_t* end = p + avail;
          for (uint32_t k = 0; k < groupCount && p < end; ++k) parseRecord(p, end, i);
        }
        break;
      }
    }
  }
  if (rec.selfOff != NO_OFFSET) {
    uint8_t buf[2 + 9 + 3];
    if (readAt(wordsFile, rec.selfOff, buf, sizeof(buf))) {
      const uint8_t* p = buf;
      parseRecord(p, buf + sizeof(buf), i);
    }
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
