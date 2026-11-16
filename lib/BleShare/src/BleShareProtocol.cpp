#include "BleShareProtocol.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace xpbt {
namespace {

constexpr uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfc,  0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

constexpr uint32_t rotr32(const uint32_t x, const unsigned n) { return (x >> n) | (x << (32u - n)); }

struct Sha256Ctx {
  uint32_t state[8];
  uint64_t bitCount;
  uint8_t block[64];
  size_t blockLen;
};

void sha256Transform(uint32_t state[8], const uint8_t block[64]) {
  uint32_t w[16];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | static_cast<uint32_t>(block[i * 4 + 3]);
  }
  uint32_t a = state[0];
  uint32_t b = state[1];
  uint32_t c = state[2];
  uint32_t d = state[3];
  uint32_t e = state[4];
  uint32_t f = state[5];
  uint32_t g = state[6];
  uint32_t h = state[7];

  for (int i = 0; i < 64; ++i) {
    uint32_t wi;
    if (i < 16) {
      wi = w[i];
    } else {
      const uint32_t w15 = w[(i - 15) & 15];
      const uint32_t w2 = w[(i - 2) & 15];
      const uint32_t s0 = rotr32(w15, 7) ^ rotr32(w15, 18) ^ (w15 >> 3);
      const uint32_t s1 = rotr32(w2, 17) ^ rotr32(w2, 19) ^ (w2 >> 10);
      wi = w[i & 15] + s0 + w[(i - 7) & 15] + s1;
      w[i & 15] = wi;
    }
    const uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
    const uint32_t ch = (e & f) ^ ((~e) & g);
    const uint32_t temp1 = h + S1 + ch + kSha256K[i] + wi;
    const uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = S0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

void sha256Init(Sha256Ctx& ctx) {
  ctx.state[0] = 0x6a09e667u;
  ctx.state[1] = 0xbb67ae85u;
  ctx.state[2] = 0x3c6ef372u;
  ctx.state[3] = 0xa54ff53au;
  ctx.state[4] = 0x510e527fu;
  ctx.state[5] = 0x9b05688cu;
  ctx.state[6] = 0x1f83d9abu;
  ctx.state[7] = 0x5be0cd19u;
  ctx.bitCount = 0;
  ctx.blockLen = 0;
}

void sha256Update(Sha256Ctx& ctx, const uint8_t* data, size_t len) {
  if (!data || len == 0) return;
  ctx.bitCount += static_cast<uint64_t>(len) * 8u;
  while (len > 0) {
    const size_t take = std::min<size_t>(len, 64 - ctx.blockLen);
    memcpy(ctx.block + ctx.blockLen, data, take);
    ctx.blockLen += take;
    data += take;
    len -= take;
    if (ctx.blockLen == 64) {
      sha256Transform(ctx.state, ctx.block);
      ctx.blockLen = 0;
    }
  }
}

void sha256Finish(Sha256Ctx& ctx, uint8_t out[kSha256Bytes]) {
  ctx.block[ctx.blockLen++] = 0x80u;
  if (ctx.blockLen > 56) {
    memset(ctx.block + ctx.blockLen, 0, 64 - ctx.blockLen);
    sha256Transform(ctx.state, ctx.block);
    ctx.blockLen = 0;
  }
  memset(ctx.block + ctx.blockLen, 0, 56 - ctx.blockLen);
  for (int i = 0; i < 8; ++i) {
    ctx.block[56 + i] = static_cast<uint8_t>(ctx.bitCount >> ((7 - i) * 8));
  }
  sha256Transform(ctx.state, ctx.block);
  for (int i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<uint8_t>(ctx.state[i] >> 24);
    out[i * 4 + 1] = static_cast<uint8_t>(ctx.state[i] >> 16);
    out[i * 4 + 2] = static_cast<uint8_t>(ctx.state[i] >> 8);
    out[i * 4 + 3] = static_cast<uint8_t>(ctx.state[i]);
  }
}

bool isValidType(const uint8_t raw) {
  return raw >= static_cast<uint8_t>(PacketType::PairHello) && raw <= static_cast<uint8_t>(PacketType::ErrorPacket);
}

int hexNibble(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

bool hasAllowedBookExt(std::string_view name) {
  const size_t dot = name.rfind('.');
  if (dot == std::string_view::npos || dot == 0 || dot + 1 >= name.size()) return false;
  std::string ext;
  ext.reserve(name.size() - dot);
  for (size_t i = dot; i < name.size(); ++i) {
    ext.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(name[i]))));
  }
  return ext == ".epub" || ext == ".txt" || ext == ".md" || ext == ".xtc" || ext == ".xtch" || ext == ".acsm" ||
         ext == ".cpfont";
}

}  // namespace

uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8); }

uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t readU64(const uint8_t* p) {
  return static_cast<uint64_t>(readU32(p)) | (static_cast<uint64_t>(readU32(p + 4)) << 32);
}

void writeU16(uint8_t* p, const uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

void writeU32(uint8_t* p, const uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

void writeU64(uint8_t* p, const uint64_t v) {
  writeU32(p, static_cast<uint32_t>(v));
  writeU32(p + 4, static_cast<uint32_t>(v >> 32));
}

bool encodeFrame(uint8_t* out, const size_t capacity, const PacketType type, const uint32_t sequence,
                 const void* payload, const uint16_t payloadLength, size_t& outLen) {
  outLen = 0;
  if (!out || payloadLength > kMaxPayloadBytes || capacity < kHeaderBytes + payloadLength ||
      (payloadLength > 0 && !payload)) {
    return false;
  }
  memcpy(out, kMagic, sizeof(kMagic));
  out[4] = kProtocolVersion;
  out[5] = static_cast<uint8_t>(type);
  writeU16(out + 6, payloadLength);
  writeU32(out + 8, sequence);
  if (payloadLength > 0) {
    memcpy(out + kHeaderBytes, payload, payloadLength);
  }
  outLen = kHeaderBytes + payloadLength;
  return true;
}

bool decodeFrame(const uint8_t* data, const size_t len, PacketView& out) {
  if (!data || len < kHeaderBytes || len > kMaxFrameBytes) return false;
  if (memcmp(data, kMagic, sizeof(kMagic)) != 0 || data[4] != kProtocolVersion) return false;
  if (!isValidType(data[5])) return false;
  const uint16_t payloadLen = readU16(data + 6);
  if (payloadLen > kMaxPayloadBytes || len != kHeaderBytes + payloadLen) return false;
  out.type = static_cast<PacketType>(data[5]);
  out.sequence = readU32(data + 8);
  out.payload = data + kHeaderBytes;
  out.payloadLength = payloadLen;
  return true;
}

bool StreamDecoder::feed(const uint8_t* data, const size_t len, size_t& consumed, PacketView& frame) {
  consumed = 0;
  lastFrameBytes_ = 0;
  while (consumed < len || len_ >= kHeaderBytes) {
    if (consumed < len && len_ < kMaxFrameBytes) {
      const size_t take = std::min<size_t>(len - consumed, kMaxFrameBytes - len_);
      memcpy(buf_ + len_, data + consumed, take);
      len_ += take;
      consumed += take;
    }

    // Align buffer head to 'X','P','B','T'.
    size_t magicPos = 0;
    while (magicPos < len_) {
      if (buf_[magicPos] == kMagic[0]) {
        const size_t rem = len_ - magicPos;
        const size_t cmpLen = std::min<size_t>(rem, sizeof(kMagic));
        if (memcmp(buf_ + magicPos, kMagic, cmpLen) == 0) break;
      }
      ++magicPos;
    }
    if (magicPos > 0) {
      memmove(buf_, buf_ + magicPos, len_ - magicPos);
      len_ -= magicPos;
    }
    if (len_ < kHeaderBytes) {
      if (consumed >= len) return false;
      continue;
    }

    const uint16_t payloadLen = readU16(buf_ + 6);
    if (buf_[4] != kProtocolVersion || !isValidType(buf_[5]) || payloadLen > kMaxPayloadBytes) {
      // Corrupt header after magic: advance 1 byte and resync.
      memmove(buf_, buf_ + 1, len_ - 1);
      --len_;
      continue;
    }

    const size_t frameBytes = kHeaderBytes + payloadLen;
    if (len_ < frameBytes) {
      if (consumed >= len) return false;
      continue;
    }

    lastFrameBytes_ = frameBytes;
    frame.type = static_cast<PacketType>(buf_[5]);
    frame.sequence = readU32(buf_ + 8);
    frame.payload = buf_ + kHeaderBytes;
    frame.payloadLength = payloadLen;
    return true;
  }
  return false;
}

void StreamDecoder::consumeFrame() {
  if (lastFrameBytes_ == 0 || lastFrameBytes_ > len_) return;
  const size_t rem = len_ - lastFrameBytes_;
  if (rem > 0) memmove(buf_, buf_ + lastFrameBytes_, rem);
  len_ = rem;
  lastFrameBytes_ = 0;
}

uint32_t crc32Init() { return 0xFFFFFFFFu; }

uint32_t crc32Update(uint32_t crc, const uint8_t* data, const size_t len) {
  if (!data) return crc;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; ++b) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return crc;
}

uint32_t crc32Final(const uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

void sha256(const uint8_t* data, const size_t len, uint8_t out[kSha256Bytes]) {
  Sha256Ctx ctx;
  sha256Init(ctx);
  sha256Update(ctx, data, len);
  sha256Finish(ctx, out);
}

void hmacSha256(const uint8_t* key, const size_t keyLen, const uint8_t* msg, const size_t msgLen,
                uint8_t out[kSha256Bytes]) {
  uint8_t kPad[64]{};
  if (keyLen > 64) {
    sha256(key, keyLen, kPad);
  } else if (key && keyLen > 0) {
    memcpy(kPad, key, keyLen);
  }

  uint8_t pad[64];
  for (int i = 0; i < 64; ++i) pad[i] = static_cast<uint8_t>(kPad[i] ^ 0x36u);

  uint8_t innerHash[kSha256Bytes];
  Sha256Ctx ctx;
  sha256Init(ctx);
  sha256Update(ctx, pad, sizeof(pad));
  sha256Update(ctx, msg, msgLen);
  sha256Finish(ctx, innerHash);

  for (int i = 0; i < 64; ++i) pad[i] = static_cast<uint8_t>(kPad[i] ^ 0x5cu);
  sha256Init(ctx);
  sha256Update(ctx, pad, sizeof(pad));
  sha256Update(ctx, innerHash, sizeof(innerHash));
  sha256Finish(ctx, out);
}

bool constantTimeEq32(const uint8_t a[kSha256Bytes], const uint8_t b[kSha256Bytes]) {
  uint8_t diff = 0;
  for (size_t i = 0; i < kSha256Bytes; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

void derivePairKey(const char pin[kPinDigits], const uint8_t clientNonce[kNonceBytes],
                   const uint8_t serverNonce[kNonceBytes], const uint8_t phoneId[kIdBytes],
                   const uint8_t readerId[kIdBytes], uint8_t outKey[kSha256Bytes]) {
  static constexpr char kLabel[] = "xpbt-pair-key-v1";
  uint8_t msg[(sizeof(kLabel) - 1) + kNonceBytes * 2 + kIdBytes * 2];
  size_t off = 0;
  memcpy(msg + off, kLabel, sizeof(kLabel) - 1);
  off += sizeof(kLabel) - 1;
  memcpy(msg + off, clientNonce, kNonceBytes);
  off += kNonceBytes;
  memcpy(msg + off, serverNonce, kNonceBytes);
  off += kNonceBytes;
  memcpy(msg + off, phoneId, kIdBytes);
  off += kIdBytes;
  memcpy(msg + off, readerId, kIdBytes);
  off += kIdBytes;
  hmacSha256(reinterpret_cast<const uint8_t*>(pin), kPinDigits, msg, off, outKey);
}

void computeClientPairProof(const uint8_t sharedKey[kSha256Bytes], const uint8_t clientNonce[kNonceBytes],
                            const uint8_t serverNonce[kNonceBytes], uint8_t outProof[kSha256Bytes]) {
  static constexpr char kLabel[] = "xpbt-client-verify";
  uint8_t msg[(sizeof(kLabel) - 1) + kNonceBytes * 2];
  size_t off = 0;
  memcpy(msg + off, kLabel, sizeof(kLabel) - 1);
  off += sizeof(kLabel) - 1;
  memcpy(msg + off, serverNonce, kNonceBytes);
  off += kNonceBytes;
  memcpy(msg + off, clientNonce, kNonceBytes);
  off += kNonceBytes;
  hmacSha256(sharedKey, kSha256Bytes, msg, off, outProof);
}

void computeServerPairProof(const uint8_t sharedKey[kSha256Bytes], const uint8_t clientNonce[kNonceBytes],
                            const uint8_t serverNonce[kNonceBytes], uint8_t outProof[kSha256Bytes]) {
  static constexpr char kLabel[] = "xpbt-server-verify";
  uint8_t msg[(sizeof(kLabel) - 1) + kNonceBytes * 2];
  size_t off = 0;
  memcpy(msg + off, kLabel, sizeof(kLabel) - 1);
  off += sizeof(kLabel) - 1;
  memcpy(msg + off, clientNonce, kNonceBytes);
  off += kNonceBytes;
  memcpy(msg + off, serverNonce, kNonceBytes);
  off += kNonceBytes;
  hmacSha256(sharedKey, kSha256Bytes, msg, off, outProof);
}

void computeAuthProof(const uint8_t sharedKey[kSha256Bytes], const uint8_t clientNonce[kNonceBytes],
                      const uint8_t serverNonce[kNonceBytes], const uint8_t phoneId[kIdBytes],
                      uint8_t outProof[kSha256Bytes]) {
  static constexpr char kLabel[] = "xpbt-auth-v1";
  uint8_t msg[(sizeof(kLabel) - 1) + kNonceBytes * 2 + kIdBytes];
  size_t off = 0;
  memcpy(msg + off, kLabel, sizeof(kLabel) - 1);
  off += sizeof(kLabel) - 1;
  memcpy(msg + off, clientNonce, kNonceBytes);
  off += kNonceBytes;
  memcpy(msg + off, serverNonce, kNonceBytes);
  off += kNonceBytes;
  memcpy(msg + off, phoneId, kIdBytes);
  off += kIdBytes;
  hmacSha256(sharedKey, kSha256Bytes, msg, off, outProof);
}

void computeServerAuthAck(const uint8_t sharedKey[kSha256Bytes], const uint8_t clientNonce[kNonceBytes],
                          const uint8_t serverNonce[kNonceBytes], const uint8_t readerId[kIdBytes],
                          uint8_t outProof[kSha256Bytes]) {
  static constexpr char kLabel[] = "xpbt-auth-ack-v1";
  uint8_t msg[(sizeof(kLabel) - 1) + kNonceBytes * 2 + kIdBytes];
  size_t off = 0;
  memcpy(msg + off, kLabel, sizeof(kLabel) - 1);
  off += sizeof(kLabel) - 1;
  memcpy(msg + off, serverNonce, kNonceBytes);
  off += kNonceBytes;
  memcpy(msg + off, clientNonce, kNonceBytes);
  off += kNonceBytes;
  memcpy(msg + off, readerId, kIdBytes);
  off += kIdBytes;
  hmacSha256(sharedKey, kSha256Bytes, msg, off, outProof);
}

void bytesToHex(const uint8_t* bytes, const size_t len, char* outHex) {
  static constexpr char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < len; ++i) {
    outHex[i * 2] = kHex[bytes[i] >> 4];
    outHex[i * 2 + 1] = kHex[bytes[i] & 0x0Fu];
  }
  outHex[len * 2] = '\0';
}

bool hexToBytes(std::string_view hex, uint8_t* outBytes, const size_t expectedBytes) {
  if (hex.size() != expectedBytes * 2) return false;
  for (size_t i = 0; i < expectedBytes; ++i) {
    const int hi = hexNibble(hex[i * 2]);
    const int lo = hexNibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    outBytes[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

std::string sanitizeBookFileName(std::string_view rawName) {
  // Strip any leading path components.
  const size_t slash = rawName.find_last_of("/\\");
  if (slash != std::string_view::npos) {
    rawName.remove_prefix(slash + 1);
  }
  while (!rawName.empty() && (rawName.front() == ' ' || rawName.front() == '.')) {
    rawName.remove_prefix(1);
  }
  while (!rawName.empty() && (rawName.back() == ' ' || rawName.back() == '.')) {
    rawName.remove_suffix(1);
  }
  if (rawName.empty() || rawName.size() > kMaxFileNameBytes) return {};

  std::string clean;
  clean.reserve(rawName.size());
  for (const unsigned char ch : rawName) {
    if (ch < 0x20 || ch == 0x7F || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' ||
        ch == '|' || ch == '/' || ch == '\\') {
      clean.push_back('_');
    } else {
      clean.push_back(static_cast<char>(ch));
    }
  }
  if (clean.find("..") != std::string::npos || clean.find('~') != std::string::npos) return {};
  if (!hasAllowedBookExt(clean)) return {};
  return clean;
}

std::string sanitizeDestFolder(std::string_view rawFolder) {
  if (rawFolder.empty()) return "/Books";
  std::string out;
  out.reserve(rawFolder.size() + 2);
  if (rawFolder.front() != '/') out.push_back('/');
  for (const unsigned char ch : rawFolder) {
    if (ch < 0x20 || ch == 0x7F || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' ||
        ch == '>' || ch == '|') {
      return "/Books";
    }
    out.push_back(static_cast<char>(ch));
  }
  while (out.size() > 1 && out.back() == '/') out.pop_back();
  if (out.size() <= 1 || out.size() > kMaxFolderBytes || out.find("..") != std::string::npos ||
      out.find("/.") != std::string::npos || out.find("//") != std::string::npos || out.find('~') != std::string::npos) {
    return "/Books";
  }
  return out;
}

}  // namespace xpbt
