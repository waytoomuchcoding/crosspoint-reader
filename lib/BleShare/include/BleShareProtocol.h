#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace xpbt {

inline constexpr char kServiceUuid[] = "c7055001-7874-6569-6e6b-783373686172";
inline constexpr char kRxCharUuid[] = "c7055002-7874-6569-6e6b-783373686172";
inline constexpr char kTxCharUuid[] = "c7055003-7874-6569-6e6b-783373686172";

inline constexpr uint8_t kMagic[4] = {'X', 'P', 'B', 'T'};
inline constexpr uint8_t kProtocolVersion = 1;
inline constexpr size_t kHeaderBytes = 12;
inline constexpr size_t kMaxPayloadBytes = 512;
inline constexpr size_t kMaxFrameBytes = kHeaderBytes + kMaxPayloadBytes;
inline constexpr uint16_t kDefaultChunkDataBytes = 480;
inline constexpr size_t kIdBytes = 16;
inline constexpr size_t kNonceBytes = 16;
inline constexpr size_t kSha256Bytes = 32;
inline constexpr size_t kPinDigits = 6;
inline constexpr size_t kMaxNameBytes = 48;
inline constexpr size_t kMaxFileNameBytes = 160;
inline constexpr size_t kMaxFolderBytes = 64;

enum class PacketType : uint8_t {
  PairHello = 1,
  PairChallenge = 2,
  PairVerify = 3,
  PairOk = 4,
  AuthHello = 5,
  AuthChallenge = 6,
  AuthVerify = 7,
  AuthOk = 8,
  BatchOffer = 9,
  BatchDecision = 10,
  FileBegin = 11,
  FileReady = 12,
  FileChunk = 13,
  FileProgress = 14,
  FileEnd = 15,
  FileResult = 16,
  BatchDone = 17,
  BatchComplete = 18,
  ErrorPacket = 19,
};

enum class FileStatus : uint8_t {
  Ok = 0,
  CrcMismatch = 1,
  IoError = 2,
  InvalidName = 3,
};

enum class ErrorCode : uint8_t {
  BadState = 1,
  PairingDisabled = 2,
  InvalidPin = 3,
  NotPaired = 4,
  AuthFailed = 5,
  MalformedPayload = 6,
};

struct PacketView {
  PacketType type = PacketType::ErrorPacket;
  uint32_t sequence = 0;
  const uint8_t* payload = nullptr;
  uint16_t payloadLength = 0;
};

uint16_t readU16(const uint8_t* p);
uint32_t readU32(const uint8_t* p);
uint64_t readU64(const uint8_t* p);
void writeU16(uint8_t* p, uint16_t v);
void writeU32(uint8_t* p, uint32_t v);
void writeU64(uint8_t* p, uint64_t v);

bool encodeFrame(uint8_t* out, size_t capacity, PacketType type, uint32_t sequence, const void* payload,
                 uint16_t payloadLength, size_t& outLen);
bool decodeFrame(const uint8_t* data, size_t len, PacketView& out);

// Reassembles XPBT frames across arbitrary BLE GATT or TCP byte chunks without heap allocation.
class StreamDecoder {
 public:
  void reset() { len_ = 0; }

  // Consumes up to `len` bytes from `data` and writes bytes consumed to `consumed`.
  // Returns true when a complete, valid PacketView is ready in `frame` (valid until next reset/feed after consume).
  bool feed(const uint8_t* data, size_t len, size_t& consumed, PacketView& frame);

  // Advances past the frame returned by feed().
  void consumeFrame();

 private:
  uint8_t buf_[kMaxFrameBytes]{};
  size_t len_ = 0;
  size_t lastFrameBytes_ = 0;
};

// Standard IEEE 802.3 CRC32 (matches java.util.zip.CRC32).
uint32_t crc32Init();
uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len);
uint32_t crc32Final(uint32_t crc);

// Compact FIPS 180-4 SHA-256 and RFC 2104 HMAC-SHA256 (zero heap allocation).
void sha256(const uint8_t* data, size_t len, uint8_t out[kSha256Bytes]);
void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen, uint8_t out[kSha256Bytes]);
bool constantTimeEq32(const uint8_t a[kSha256Bytes], const uint8_t b[kSha256Bytes]);

void derivePairKey(const char pin[kPinDigits], const uint8_t clientNonce[kNonceBytes],
                   const uint8_t serverNonce[kNonceBytes], const uint8_t phoneId[kIdBytes],
                   const uint8_t readerId[kIdBytes], uint8_t outKey[kSha256Bytes]);
void computeClientPairProof(const uint8_t sharedKey[kSha256Bytes], const uint8_t clientNonce[kNonceBytes],
                            const uint8_t serverNonce[kNonceBytes], uint8_t outProof[kSha256Bytes]);
void computeServerPairProof(const uint8_t sharedKey[kSha256Bytes], const uint8_t clientNonce[kNonceBytes],
                            const uint8_t serverNonce[kNonceBytes], uint8_t outProof[kSha256Bytes]);
void computeAuthProof(const uint8_t sharedKey[kSha256Bytes], const uint8_t clientNonce[kNonceBytes],
                      const uint8_t serverNonce[kNonceBytes], const uint8_t phoneId[kIdBytes],
                      uint8_t outProof[kSha256Bytes]);
void computeServerAuthAck(const uint8_t sharedKey[kSha256Bytes], const uint8_t clientNonce[kNonceBytes],
                          const uint8_t serverNonce[kNonceBytes], const uint8_t readerId[kIdBytes],
                          uint8_t outProof[kSha256Bytes]);

void bytesToHex(const uint8_t* bytes, size_t len, char* outHex);
bool hexToBytes(std::string_view hex, uint8_t* outBytes, size_t expectedBytes);

// Validates and strips directory components from an incoming filename; allows .epub, .txt, .md, .xtc, .xtch, .acsm, .cpfont.
std::string sanitizeBookFileName(std::string_view rawName);
std::string sanitizeDestFolder(std::string_view rawFolder);

}  // namespace xpbt
