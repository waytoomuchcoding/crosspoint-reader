#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

enum class TextEncoding : uint8_t {
  Auto = 0,
  Utf8 = 1,
  Gb18030 = 2,
  Big5 = 3,
  Utf16Le = 4,
  Utf16Be = 5,
  Count = 6,
};

namespace textencoding {

const char* encodingShortName(TextEncoding enc);

// Lookup helpers for GB18030/GBK/GB2312 and Big5/CP950
uint16_t decodeGbk2Byte(uint8_t b1, uint8_t b2);
uint32_t decodeGb180304Byte(uint8_t b1, uint8_t b2, uint8_t b3, uint8_t b4);
uint16_t decodeBig52Byte(uint8_t b1, uint8_t b2);

// XML 1.0 legal character check and UTF-8 encoder
bool isXml10ValidCodepoint(uint32_t cp);
size_t encodeUtf8(uint32_t cp, char out[4]);

// Single-window and multi-window corruption-tolerant encoding detection
TextEncoding detectTextEncoding(const uint8_t* data, size_t len, bool* hasCjkOut = nullptr);
TextEncoding detectTextEncodingMultiWindow(const uint8_t* const* windows, const size_t* lengths, size_t windowCount,
                                           bool* hasCjkOut = nullptr);

// Streaming, self-healing UTF-8 transcoder with XML 1.0 sanitization
class TextDecoder {
 public:
  explicit TextDecoder(TextEncoding encoding = TextEncoding::Utf8);

  void reset(TextEncoding encoding);
  TextEncoding encoding() const { return encoding_; }
  bool sawCjk() const { return sawCjk_; }

  // Decodes an input chunk and emits valid XML 1.0 UTF-8 bytes via the callback `emit(const char* utf8, size_t len)`.
  // Set `isFinalChunk = true` at EOF to flush any trailing incomplete sequence as U+FFFD.
  template <typename EmitFn>
  void decodeChunk(const uint8_t* data, size_t len, bool isFinalChunk, EmitFn&& emit) {
    char outBuf[128];
    size_t outLen = 0;
    auto flushLocal = [&]() {
      if (outLen > 0) {
        emit(outBuf, outLen);
        outLen = 0;
      }
    };
    auto emitCodepoint = [&](uint32_t cp) {
      if (cp == 0) return;  // Strip NUL bytes
      if (!isXml10ValidCodepoint(cp)) {
        cp = 0xFFFD;
      }
      if ((cp >= 0x3000 && cp <= 0x303F) || (cp >= 0x3400 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
          (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x2FA1F)) {
        sawCjk_ = true;
      }
      if (outLen + 4 > sizeof(outBuf)) {
        flushLocal();
      }
      outLen += encodeUtf8(cp, outBuf + outLen);
    };

    decodeInternal(data, len, isFinalChunk, emitCodepoint);
    flushLocal();
  }

  // Convenience helper to decode a buffer directly into a std::string
  std::string decodeToString(const uint8_t* data, size_t len, bool isFinalChunk = true);

 private:
  using CodepointCallback = void (*)(void* ctx, uint32_t cp);
  void decodeInternalImpl(const uint8_t* data, size_t len, bool isFinalChunk, CodepointCallback cb, void* ctx);

  template <typename CpFn>
  void decodeInternal(const uint8_t* data, size_t len, bool isFinalChunk, CpFn&& fn) {
    using DecayedFn = std::remove_reference_t<CpFn>;
    decodeInternalImpl(
        data, len, isFinalChunk, [](void* ctx, uint32_t cp) { (*static_cast<DecayedFn*>(ctx))(cp); }, &fn);
  }

  TextEncoding encoding_ = TextEncoding::Utf8;
  uint8_t carry_[4] = {0, 0, 0, 0};
  uint8_t carryLen_ = 0;
  bool bomChecked_ = false;
  bool sawCjk_ = false;
  uint16_t pendingHighSurrogate_ = 0;
};

}  // namespace textencoding
