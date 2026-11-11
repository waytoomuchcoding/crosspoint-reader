#include "TxtEncodingSupport.h"

#include <Epub/BookMetadataCache.h>
#include <HalStorage.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "TxtChapterDetector.h"

namespace txtenc {

TextEncoding readEncodingOverride(const std::string& cachePath) {
  if (cachePath.empty()) return TextEncoding::Auto;
  const std::string encPath = cachePath + "/encoding";
  if (!Storage.exists(encPath.c_str())) return TextEncoding::Auto;

  HalFile f;
  if (!Storage.openFileForRead("TXT", encPath, f)) return TextEncoding::Auto;
  uint8_t val = 0;
  const int n = f.read(&val, 1);
  f.close();
  if (n == 1 && val < static_cast<uint8_t>(TextEncoding::Count)) {
    return static_cast<TextEncoding>(val);
  }
  return TextEncoding::Auto;
}

bool writeEncodingOverride(const std::string& cachePath, TextEncoding enc) {
  if (cachePath.empty()) return false;
  if (!Storage.exists(cachePath.c_str())) {
    Storage.mkdir(cachePath.c_str());
  }
  const std::string encPath = cachePath + "/encoding";
  if (enc == TextEncoding::Auto) {
    if (Storage.exists(encPath.c_str())) {
      Storage.remove(encPath.c_str());
    }
    return true;
  }
  HalFile f;
  if (!Storage.openFileForWrite("TXT", encPath, f)) return false;
  const uint8_t val = static_cast<uint8_t>(enc);
  const bool ok = (f.write(&val, 1) == 1);
  f.close();
  return ok;
}

TextEncoding detectFileEncoding(const std::string& filepath, const std::string& cachePath, bool* hasCjkOut) {
  if (hasCjkOut) *hasCjkOut = false;

  const TextEncoding overrideEnc = readEncodingOverride(cachePath);
  if (overrideEnc != TextEncoding::Auto) {
    if (!hasCjkOut) {
      return overrideEnc;
    }
    if (overrideEnc == TextEncoding::Gb18030 || overrideEnc == TextEncoding::Big5) {
      *hasCjkOut = true;
      return overrideEnc;
    }
  }

  HalFile src;
  if (!Storage.openFileForRead("TXT", filepath, src)) {
    return overrideEnc != TextEncoding::Auto ? overrideEnc : TextEncoding::Utf8;
  }

  constexpr size_t WINDOW_SIZE = 2048;
  auto buf = makeUniqueNoThrow<uint8_t[]>(WINDOW_SIZE * 3);
  if (!buf) {
    src.close();
    return overrideEnc != TextEncoding::Auto ? overrideEnc : TextEncoding::Utf8;
  }

  const size_t fileSize = src.size();
  const uint8_t* windows[3] = {buf.get(), buf.get() + WINDOW_SIZE, buf.get() + WINDOW_SIZE * 2};
  size_t lengths[3] = {0, 0, 0};
  size_t windowCount = 0;

  const int n0 = src.read(buf.get(), WINDOW_SIZE);
  if (n0 > 0) {
    lengths[0] = static_cast<size_t>(n0);
    windowCount = 1;
  }

  if (fileSize > WINDOW_SIZE * 2) {
    const size_t midOffset = (fileSize / 3) & ~static_cast<size_t>(1);
    if (src.seek(midOffset)) {
      const int n1 = src.read(buf.get() + WINDOW_SIZE, WINDOW_SIZE);
      if (n1 > 0) {
        lengths[1] = static_cast<size_t>(n1);
        windowCount = 2;
      }
    }
  }

  if (fileSize > WINDOW_SIZE * 4) {
    const size_t lateOffset = ((fileSize * 2) / 3) & ~static_cast<size_t>(1);
    if (src.seek(lateOffset)) {
      const int n2 = src.read(buf.get() + WINDOW_SIZE * 2, WINDOW_SIZE);
      if (n2 > 0) {
        lengths[2] = static_cast<size_t>(n2);
        windowCount = 3;
      }
    }
  }
  src.close();

  if (overrideEnc != TextEncoding::Auto) {
    if (hasCjkOut) {
      textencoding::TextDecoder dec(overrideEnc);
      for (size_t w = 0; w < windowCount; ++w) {
        dec.decodeChunk(windows[w], lengths[w], w + 1 == windowCount, [](const char*, size_t) {});
      }
      *hasCjkOut = dec.sawCjk();
    }
    return overrideEnc;
  }

  bool detectedCjk = false;
  const TextEncoding detected =
      textencoding::detectTextEncodingMultiWindow(windows, lengths, windowCount, &detectedCjk);
  if (hasCjkOut) {
    *hasCjkOut = detectedCjk;
  }
  return detected;
}

bool loadRawPreviewSample(const std::string& filepath, const size_t byteOffset, std::vector<uint8_t>& rawOut,
                          TextEncoding& detectedOut, size_t& sampleOffsetOut) {
  rawOut.clear();
  sampleOffsetOut = 0;
  detectedOut = detectFileEncoding(filepath, "");

  HalFile src;
  if (!Storage.openFileForRead("TXT", filepath, src)) {
    return false;
  }

  const size_t fileSize = src.size();
  constexpr size_t RAW_SAMPLE_SIZE = 768;
  size_t startOffset = (byteOffset < fileSize) ? (byteOffset & ~static_cast<size_t>(1)) : 0;
  if (startOffset > 0 && !src.seek(startOffset)) {
    startOffset = 0;
    src.seek(0);
  }

  rawOut.resize(RAW_SAMPLE_SIZE);
  const int bytesRead = src.read(rawOut.data(), RAW_SAMPLE_SIZE);
  src.close();
  if (bytesRead <= 0) {
    rawOut.clear();
    sampleOffsetOut = startOffset;
    return true;
  }

  rawOut.resize(static_cast<size_t>(bytesRead));
  sampleOffsetOut = startOffset;
  return true;
}

void decodePreviewSample(const uint8_t* rawData, const size_t rawLen, const size_t sampleOffset,
                         const TextEncoding encoding, const TextEncoding detectedEncoding, std::string& utf8Out) {
  utf8Out.clear();
  if (!rawData || rawLen == 0) return;

  const TextEncoding active = (encoding == TextEncoding::Auto) ? detectedEncoding : encoding;
  size_t sliceStart = 0;
  if (sampleOffset > 0 && active != TextEncoding::Utf16Le && active != TextEncoding::Utf16Be) {
    // Advance to the start of the next line if within the first 128 bytes so we don't start mid-character.
    for (size_t i = 0; i < std::min<size_t>(rawLen, 128); ++i) {
      if (rawData[i] == '\n') {
        sliceStart = i + 1;
        break;
      }
    }
  } else if (sampleOffset == 0 && active != TextEncoding::Utf16Le && active != TextEncoding::Utf16Be) {
    // If the file starts with an ASCII banner line followed by non-ASCII lines, start at the first non-ASCII line.
    size_t lineStart = 0;
    for (size_t i = 0; i < rawLen; ++i) {
      if (rawData[i] == '\n') {
        lineStart = i + 1;
      } else if (rawData[i] >= 0x80) {
        sliceStart = lineStart;
        break;
      }
    }
  }

  const size_t sliceLen = std::min<size_t>(512, rawLen - sliceStart);
  textencoding::TextDecoder decoder(active);
  utf8Out = decoder.decodeToString(rawData + sliceStart, sliceLen, /*isFinalChunk=*/true);
}

bool readEncodingPreview(const std::string& filepath, const size_t byteOffset, const TextEncoding encoding,
                         std::string& utf8PreviewOut, TextEncoding* detectedOut) {
  std::vector<uint8_t> rawSample;
  TextEncoding detected = TextEncoding::Utf8;
  size_t sampleOffset = 0;
  if (!loadRawPreviewSample(filepath, byteOffset, rawSample, detected, sampleOffset)) {
    utf8PreviewOut.clear();
    return false;
  }
  if (detectedOut) {
    *detectedOut = detected;
  }
  decodePreviewSample(rawSample.data(), rawSample.size(), sampleOffset, encoding, detected, utf8PreviewOut);
  return true;
}

size_t scanChapterHeadings(const std::string& filepath, const TextEncoding activeEncoding, BookMetadataCache& cache,
                           bool& hasCjk) {
  HalFile src;
  if (!Storage.openFileForRead("TXT", filepath, src)) {
    return 0;
  }

  constexpr size_t SCAN_BUF_SIZE = 4096;
  constexpr size_t MAX_LINE_LEN = 512;
  constexpr size_t MAX_TOC_CHAPTERS = 500;
  size_t tocEntriesCreated = 0;

  auto scanBuf = makeUniqueNoThrow<uint8_t[]>(SCAN_BUF_SIZE);
  if (scanBuf) {
    textencoding::TextDecoder decoder(activeEncoding);
    std::string lineBuf;
    lineBuf.reserve(128);
    bool lineOverflowed = false;

    auto checkLine = [&](const std::string& line) {
      if (tocEntriesCreated >= MAX_TOC_CHAPTERS) return;
      std::string chapterTitle;
      if (txtchapter::isChapterHeadingLine(line, &chapterTitle)) {
        char anchorId[24];
        std::snprintf(anchorId, sizeof(anchorId), "ch%zu", tocEntriesCreated++);
        cache.createTocEntry(chapterTitle, "content.html", anchorId, 0);
      }
    };

    auto onDecoded = [&](const char* utf8, size_t len) {
      for (size_t i = 0; i < len; ++i) {
        const char c = utf8[i];
        if (c == '\r') continue;
        if (c == '\n') {
          if (!lineOverflowed && !lineBuf.empty()) {
            checkLine(lineBuf);
          }
          lineBuf.clear();
          lineOverflowed = false;
          continue;
        }
        if (!lineOverflowed) {
          if (lineBuf.size() < MAX_LINE_LEN) {
            lineBuf.push_back(c);
          } else {
            lineOverflowed = true;
            lineBuf.clear();
          }
        }
      }
    };

    int n = 0;
    while ((n = src.read(scanBuf.get(), SCAN_BUF_SIZE)) > 0) {
      decoder.decodeChunk(scanBuf.get(), static_cast<size_t>(n), false, onDecoded);
    }
    decoder.decodeChunk(nullptr, 0, true, onDecoded);
    if (!lineOverflowed && !lineBuf.empty()) {
      checkLine(lineBuf);
    }
    if (decoder.sawCjk()) {
      hasCjk = true;
    }
  }
  src.close();
  return tocEntriesCreated;
}

}  // namespace txtenc
