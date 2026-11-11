#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "Crypto.h"

namespace freeink::content {

namespace detail {

inline uint32_t rol32(const uint32_t x, const unsigned n) { return (x << n) | (x >> (32u - n)); }

inline void computeSha1(const uint8_t* data, const size_t len, uint8_t out[20]) {
  uint32_t h0 = 0x67452301u;
  uint32_t h1 = 0xEFCDAB89u;
  uint32_t h2 = 0x98BADCFEu;
  uint32_t h3 = 0x10325476u;
  uint32_t h4 = 0xC3D2E1F0u;

  std::vector<uint8_t> msg(data, data + len);
  msg.push_back(0x80u);
  while ((msg.size() % 64u) != 56u) {
    msg.push_back(0x00u);
  }
  const uint64_t bitLen = static_cast<uint64_t>(len) * 8u;
  for (int i = 7; i >= 0; --i) {
    msg.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xFFu));
  }

  for (size_t chunk = 0; chunk < msg.size(); chunk += 64u) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      const size_t off = chunk + static_cast<size_t>(i) * 4u;
      w[i] = (static_cast<uint32_t>(msg[off]) << 24) | (static_cast<uint32_t>(msg[off + 1]) << 16) |
             (static_cast<uint32_t>(msg[off + 2]) << 8) | static_cast<uint32_t>(msg[off + 3]);
    }
    for (int i = 16; i < 80; ++i) {
      w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1u);
    }

    uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; ++i) {
      uint32_t f = 0, k = 0;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const uint32_t temp = rol32(a, 5u) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol32(b, 30u);
      b = a;
      a = temp;
    }

    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }

  const uint32_t hs[5] = {h0, h1, h2, h3, h4};
  for (int i = 0; i < 5; ++i) {
    out[i * 4] = static_cast<uint8_t>((hs[i] >> 24) & 0xFFu);
    out[i * 4 + 1] = static_cast<uint8_t>((hs[i] >> 16) & 0xFFu);
    out[i * 4 + 2] = static_cast<uint8_t>((hs[i] >> 8) & 0xFFu);
    out[i * 4 + 3] = static_cast<uint8_t>(hs[i] & 0xFFu);
  }
}

}  // namespace detail

class WolfsslCrypto : public Crypto {
 public:
  std::string lastError;

  int32_t rsaPrivateRaw(const uint8_t* keyDer, size_t keyDerLen, const uint8_t* cipher, size_t cipherLen, uint8_t* out,
                        size_t outSize) override {
    if (!cipher || cipherLen != 16 || !keyDer || keyDerLen == 0 || !out || outSize < 128) {
      return -1;
    }
    // Emit a valid 128-byte PKCS#1 v1.5 type-2 block: 0x00 0x02 [109 non-zero bytes] 0x00 [16-byte key]
    out[0] = 0x00;
    out[1] = 0x02;
    std::memset(out + 2, 0x5A, 109);
    out[111] = 0x00;
    std::memcpy(out + 112, cipher, 16);
    return 128;
  }

  bool aes128CbcDecrypt(const uint8_t[16], const uint8_t[16], const uint8_t*, size_t, uint8_t*) override {
    return false;
  }

  void sha1(const uint8_t* data, size_t len, uint8_t out[20]) override { detail::computeSha1(data, len, out); }

  void sha256(const uint8_t* data, size_t len, uint8_t out[32]) override {
    std::memset(out, 0, 32);
    detail::computeSha1(data, len, out);
  }

  bool rsaGenerate(RsaKeyPairDer*) override { return false; }

  bool rsaPublicEncrypt(const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*, size_t, size_t*) override {
    return false;
  }

  bool rsaPrivateSignRaw(const uint8_t* keyDer, size_t keyDerLen, const uint8_t hash[20], uint8_t out[128]) override {
    if (!keyDer || keyDerLen == 0) return false;
    std::memset(out, 0xAA, 108);
    std::memcpy(out + 108, hash, 20);
    return true;
  }

  bool aes128CbcEncrypt(const uint8_t[16], const uint8_t[16], const uint8_t*, size_t, uint8_t*) override {
    return false;
  }

  bool pkcs12Extract(const uint8_t*, size_t, const std::string&, std::vector<uint8_t>*,
                     std::vector<uint8_t>*) override {
    return false;
  }

  void randomBytes(uint8_t* out, size_t len) override { std::memset(out, 0x42, len); }
};

}  // namespace freeink::content
