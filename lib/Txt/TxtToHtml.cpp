#include "TxtToHtml.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <Memory.h>
#include <TextEncoding.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "TxtChapterDetector.h"

const char* TxtToHtml::cacheVersionTag(std::string_view filename) {
  return FsHelpers::hasMarkdownExtension(filename) ? "<!-- MD_CACHE_VERSION: 2 -->" : "<!-- TXT_CACHE_VERSION: 2 -->";
}

size_t TxtToHtml::escapedHtmlSize(const std::string_view text) {
  size_t size = 0;
  for (const char c : text) {
    size += c == '&' ? 5 : (c == '<' || c == '>') ? 4 : 1;
  }
  return size;
}

void TxtToHtml::appendEscapedHtml(std::string& out, const std::string_view text) {
  TxtToHtml::streamEscapedHtml([&out](std::string_view fragment) { out.append(fragment); }, text);
}

bool TxtToHtml::stream(std::string_view filename, void* readerCtx, int (*readFn)(void* ctx, uint8_t* buf, size_t size),
                       Print& out, TextEncoding encoding) {
  constexpr size_t IN_BUF_SIZE = 8192;
  constexpr size_t OUT_BUF_SIZE = 8192;
  constexpr size_t MAX_LINE_BUF_SIZE = 512;
  constexpr size_t MAX_TOC_CHAPTERS = 500;

  auto inBuf = makeUniqueNoThrow<uint8_t[]>(IN_BUF_SIZE);
  auto outBuf = makeUniqueNoThrow<uint8_t[]>(OUT_BUF_SIZE);
  if (!inBuf || !outBuf) {
    LOG_ERR("TXT", "OOM: TXT/MD HTML streaming buffers");
    return false;
  }

  size_t outPos = 0;
  bool outputOk = true;
  auto flushOut = [&]() {
    if (outPos > 0) {
      const size_t written = out.write(outBuf.get(), outPos);
      outputOk = outputOk && (written == outPos);
      outPos = 0;
    }
  };

  auto writeByte = [&](uint8_t b) {
    outBuf[outPos++] = b;
    if (outPos == OUT_BUF_SIZE) flushOut();
  };

  auto writeStr = [&](std::string_view s) {
    for (char c : s) {
      writeByte(static_cast<uint8_t>(c));
    }
  };

  writeStr("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
  writeStr(cacheVersionTag(filename));
  writeByte('\n');
  writeStr("<!DOCTYPE html>\n<html>\n<head><title>");
  std::string title = FsHelpers::getFileNameWithoutExtension(filename);
  TxtToHtml::streamEscapedHtml(writeStr, title);
  writeStr("</title></head>\n<body>\n");

  bool atLineStart = true;
  bool lineOverflowed = false;
  size_t pendingSpaces = 0;
  size_t chapterIndex = 0;
  std::string lineBuf;
  lineBuf.reserve(256);

  auto emitLineSegment = [&](std::string_view segment, bool isCompleteLine, bool startedAtLineStart) {
    if (isCompleteLine && startedAtLineStart && chapterIndex < MAX_TOC_CHAPTERS &&
        txtchapter::isChapterHeadingLine(segment)) {
      char anchorTag[32];
      snprintf(anchorTag, sizeof(anchorTag), "<div id=\"ch%zu\"></div>", chapterIndex++);
      writeStr(anchorTag);
    }

    for (size_t i = 0; i < segment.size(); ++i) {
      const uint8_t b = static_cast<uint8_t>(segment[i]);
      if (b == '\r') continue;

      if (b == ' ') {
        if (atLineStart) {
          writeStr("&#160;");
        } else {
          pendingSpaces++;
        }
        continue;
      }

      if (pendingSpaces > 0) {
        for (size_t s = 0; s < pendingSpaces - 1; s++) {
          writeStr("&#160;");
        }
        writeByte(' ');
        pendingSpaces = 0;
      }
      atLineStart = false;

      if (b == '&' || b == '<' || b == '>') {
        const char special = static_cast<char>(b);
        TxtToHtml::streamEscapedHtml(writeStr, std::string_view(&special, 1));
      } else if (b < 0x20 && b != '\t') {
        writeByte(' ');
      } else {
        writeByte(b);
      }
    }

    if (isCompleteLine) {
      pendingSpaces = 0;
      writeStr("<br />");
      atLineStart = true;
      lineOverflowed = false;
    }
  };

  auto processDecodedUtf8 = [&](const char* utf8, size_t len) {
    for (size_t i = 0; i < len; ++i) {
      const char c = utf8[i];
      if (c == '\r') continue;
      if (c == '\n') {
        emitLineSegment(lineBuf, /*isCompleteLine=*/true, /*startedAtLineStart=*/!lineOverflowed);
        lineBuf.clear();
        continue;
      }
      lineBuf.push_back(c);
      // Flush at a codepoint boundary if the line exceeds MAX_LINE_BUF_SIZE
      if (lineBuf.size() >= MAX_LINE_BUF_SIZE && (i + 1 == len || (static_cast<uint8_t>(utf8[i + 1]) & 0xC0) != 0x80)) {
        emitLineSegment(lineBuf, /*isCompleteLine=*/false, /*startedAtLineStart=*/false);
        lineBuf.clear();
        lineOverflowed = true;
      }
    }
  };

  textencoding::TextDecoder decoder(encoding);
  bool firstChunk = true;
  int bytesRead = 0;

  while ((bytesRead = readFn(readerCtx, inBuf.get(), IN_BUF_SIZE)) > 0) {
    if (firstChunk) {
      firstChunk = false;
      if (encoding == TextEncoding::Auto) {
        const TextEncoding detected = textencoding::detectTextEncoding(inBuf.get(), static_cast<size_t>(bytesRead));
        decoder.reset(detected);
      }
    }
    decoder.decodeChunk(inBuf.get(), static_cast<size_t>(bytesRead), /*isFinalChunk=*/false, processDecodedUtf8);
  }

  if (bytesRead < 0) {
    LOG_ERR("TXT", "Read error while streaming TXT/MD: %.*s", static_cast<int>(filename.size()), filename.data());
    return false;
  }

  decoder.decodeChunk(nullptr, 0, /*isFinalChunk=*/true, processDecodedUtf8);
  if (!lineBuf.empty()) {
    if (!lineOverflowed && chapterIndex < MAX_TOC_CHAPTERS && txtchapter::isChapterHeadingLine(lineBuf)) {
      char anchorTag[32];
      snprintf(anchorTag, sizeof(anchorTag), "<div id=\"ch%zu\"></div>", chapterIndex++);
      writeStr(anchorTag);
    }
    emitLineSegment(lineBuf, /*isCompleteLine=*/false, /*startedAtLineStart=*/false);
    lineBuf.clear();
  }

  writeStr("\n</body>\n</html>\n");
  flushOut();
  if (!outputOk) {
    LOG_ERR("TXT", "Failed to stream complete HTML (write error or disk full)");
    return false;
  }
  return true;
}

bool TxtToHtml::stream(std::string_view filename, std::string_view content, Print& out, TextEncoding encoding) {
  struct ViewReader {
    std::string_view s;
    size_t pos = 0;
  } reader{content, 0};

  return stream(
      filename, &reader,
      [](void* ctx, uint8_t* buf, size_t size) -> int {
        auto* r = static_cast<ViewReader*>(ctx);
        if (r->pos >= r->s.size()) return 0;
        const size_t n = std::min(size, r->s.size() - r->pos);
        memcpy(buf, r->s.data() + r->pos, n);
        r->pos += n;
        return static_cast<int>(n);
      },
      out, encoding);
}
